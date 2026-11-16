/*
 * EXP16.C - EXP16.SYS, a driver for the Intel EtherExpress 16 family
 *
 * The ISA 10Mbit cards built on the Intel 82586 LAN coprocessor: the
 * EtherExpress 16, 16C, 16TP and the Combo (twisted pair, BNC coax and
 * AUI).  Charlie's card is an "Intel 8/16 TPC": twisted pair and coax,
 * for an 8-bit or a 16-bit slot.  No emulator has an 82586, so this is
 * written to the references below and is for the real card to prove.
 *
 * THE CARD.  The 82586 does no DMA into the PC.  It runs out of the
 * card's own memory (16K to 64K, 32K on most cards), and the PC reaches
 * that memory through three ports: a write pointer (base+2), a read
 * pointer (base+4) and a data port (base+0).  A word written to the data
 * port goes where the write pointer points, which then moves on by two;
 * reads the same with the read pointer.  Everything the 82586 needs - its
 * start-up blocks, the command blocks, the receive lists, the frames
 * themselves - is put there and taken from there with those ports, by
 * the processor (rep outsw / rep insw for a frame).  A write to base+6 is
 * the channel attention (CA) that makes the 82586 look at its SCB.
 *
 * The card also has "shadow" windows (base+4000h, 8000h and C000h) onto
 * small pieces of its memory; Linux reads the SCB through base+C008h.
 * This driver does not use them: one way into card memory (the pointers,
 * as NetBSD's PIO path does it for every access) is one thing for the
 * real card to get right instead of two.  Every pointer write and the
 * data transfer after it are done with interrupts off, so card_service
 * (from the interrupt or the clock) never moves a pointer under
 * card_send.
 *
 * FINDING IT.  The card is set up by Intel's SOFTSET: the base and the
 * line are in its EEPROM, there are no jumpers.  The bases it can have
 * are 200h-270h and 300h-370h; at each, base+0Fh gives a nibble of the
 * signature BABAh a read, with the nibble's number in the low two bits
 * (Linux's do_express_probe, FreeBSD's ie_ee16_port_check).  The scan
 * only reads until the signature is there.  ONE READ CAN DO HARM: an
 * NE2000 at the base 10h below has its reset port where this reads
 * (31Fh for an NE2000 at 300h).  /P:base skips the scan.
 *
 * Once found: the card's own logic (Intel's "BART" ASIC) is reset, the
 * 82586 held in reset, and the EEPROM read - 64 words of a 93C46 summing
 * to BABAh.  Words 4, 3 and 2 are the station address, high byte first.
 * Word 0: bits 15-13 the line's code (1-6 = IRQ 9, 3, 4, 5, 10, 11),
 * bit 12 set for the card's own transceiver (then word 5 bit 0 is twisted
 * pair, else BNC) and clear for AUI, bit 10 set for 16-bit operation.
 * The size of card memory is found as Linux does: all of it cleared, then
 * a mark at each kilobyte until a mark is already there (the memory has
 * wrapped round) or will not stay.
 *
 * THE LINE.  The card puts its interrupt on whichever line its IRQ select
 * register (base+7) names: the EEPROM's code at the load, or the line /I:
 * gives, if it is one the card has.  A card whose EEPROM names no line,
 * or an /I: line it does not have, is run polled; probing for the line
 * would only find the line this driver chose.  Bit 3 of the register
 * lets the interrupt out.  It is off from card_find to the end of
 * card_setup, and on only when every cause the 82586 had is answered.
 *
 * THE CONNECTOR is what the EEPROM says, or /TP, /BNC or /AUI, set in
 * the register Linux's eexp_hw_set_interface uses, base+300Eh (bit 7:
 * the card's own transceiver; bit 1 with it: twisted pair).  Neither BSD
 * driver writes it; Linux writes it every time and waits 20ms.  It is
 * written with the 82586 held in reset and the EEPROM control rewritten
 * after it, so that a card which did not tell base+300Eh from base+0Eh
 * would do no harm.
 *
 * 8-BIT OR 16-BIT is the EEPROM's bit 10, as Linux reads it; /8 forces
 * 8-bit.  For 8-bit, Linux clears bit 2 of the configuration register
 * (base+0Dh) and tells the 82586 its bus is 8 bits wide (the SCP's first
 * byte); so does this.  The ports are read and written in words either
 * way: in an 8-bit slot the motherboard turns each word into two byte
 * cycles, which is what an 8-bit card expects, and Linux does the same.
 * Linux's other 8-bit change - suspending the command unit while it
 * links a frame onto a running list ("CU wedged") - is not needed here:
 * a transmit block is only ever written while the command unit is not
 * looking at it.  Most words written while the 82586 runs (the end-of-
 * list marks) have a low byte that does not change, so a word that the
 * 82586 reads between the two byte cycles is never half old, half new.
 *
 * CARD MEMORY, offsets the 82586 also uses (its 24-bit addresses have the
 * top bits zero: the card decodes only as many bits as it has memory):
 *   0000h ISCP; 0008h SCB; 0020h a command block for setup;
 *   0040h two transmit blocks of 620h; 0C80h the receive slots, 620h
 *   each, as many as fit (18 in 32K); the SCP at the top less 10 bytes,
 *   where the 82586's FFFFF6h lands (Linux and NetBSD agree on this).
 *
 * BRINGING IT UP (card_setup, every time, after a park as well): the ASIC
 * reset, the line masked, the connector, loopback on, the 82586 held in
 * reset while the ISCP (busy), the SCB and the SCP are written; reset
 * let go; CA; the 82586 reads the SCP and the ISCP and clears busy.  Then
 * one command at a time, each the end of its own list: CONFIGURE, IA-
 * SETUP, MC-SETUP; loopback off; at the first start a TDR (the 82586's
 * cable test), said only when it finds a fault; the receive lists laid
 * out and the receive unit started; every cause acknowledged; the line
 * let out.  Linux runs a DIAGNOSE as well; both it and the TDR only
 * report, and DIAGNOSE is left out.
 *
 * CONFIGURE is Linux's twelve bytes, which the BSD drivers share but for
 * two: the minimum frame length (60 here and in Linux, 64 in BSD - 60
 * cannot refuse a frame of legal length however the chip counts) and
 * byte 11 (0 in Linux, FFh in BSD "for the 82596"); Linux is followed,
 * being the driver for this card through these same ports.  AL-LOC is 1:
 * the Ethernet header is part of the frame in the buffer, both ways.
 * The 82586 does not keep a frame's CRC in memory, and pads short frames
 * only in HDLC mode: a short frame is padded to 60 here.
 *
 * MULTICAST.  The 82586 has promiscuous mode and a 64-bit multicast hash,
 * and no bit for all multicast frames (FreeBSD's if_ie says the same and
 * goes promiscuous instead).  The hash is six bits of the CRC of the
 * destination; QEMU's models of the 82596 and the 8255x, its successors,
 * take bits 2-7.  MC-SETUP is given a list chosen so that the CRCs of its
 * addresses cover all 64 values in bits 2-7, and also in the top six,
 * the bottom six and bits 24-29, in case the 82586 counts them another
 * way: whichever of those it uses, every bit of its table is set and
 * every multicast frame comes in.  The list starts with NetBEUI's
 * 03-00-00-00-00-01 and IP's 01-00-5E-00-00-01, which come in whatever
 * the hash is.  113 addresses; computed once (mc_build).
 *
 * RECEIVING.  A slot is a receive frame descriptor (RFD), its receive
 * buffer descriptor (RBD) and a 1536-byte buffer, as Linux lays them out:
 * a frame fits one buffer, so the two lists move together.  The last RFD
 * and RBD carry EL, the end of the list (the BSD drivers); Linux leaves
 * the list without an end, which lets the 82586 write over frames not yet
 * read.  A slot whose frame has been taken becomes the new end, and the
 * old end is taken off after it.  When the receive unit stops (no
 * resources: RNR) or the lists are found out of step (a frame the chip
 * gave up on, one in more than one buffer), what is complete is taken,
 * the lists are laid out again and the receive unit started at the first
 * slot - FreeBSD's iernr does the same.  No copy is longer than
 * NIC_MAXFRAME.
 *
 * SENDING.  Two transmit blocks (command, buffer descriptor, frame).  A
 * frame is copied into the one not in use while the other may still be
 * going out; then, when that one shows complete and the command unit is
 * idle, the command unit is started on the new one.  Each transmit is a
 * list of one, EL set.
 *
 * THE INTERRUPT.  The 82586 holds INT up while any of its four causes
 * (CX, FR, CNA, RNR) is unacknowledged; the ISA line is edge-triggered.
 * card_service takes the enable bit away while it answers and gives it
 * back at the end, so a cause that came in meanwhile makes a new edge.
 * Linux, FreeBSD and NetBSD all do this.
 *
 * Its own switches, beside NETCORE's /P:, /I: and /POLL:
 *   /TP, /BNC, /AUI   the connector, whatever the EEPROM says
 *   /8                8-bit operation, whatever the EEPROM says
 *
 * References: Linux drivers/net/eexpress.c and eexpress.h (v3.0), the
 * driver for this card through these ports; FreeBSD sys/dev/ie (if_ie.c,
 * if_ie_isa.c, if_iee16.h, dev/ic/i82586.h, releng/10.0), which maps the
 * card's memory instead; NetBSD sys/dev/isa/if_ix.c and dev/ic/i82586.c,
 * which can do either.  Nothing here has run on the card.  The driver has
 * been run against a model of the card and the 82586 written from the
 * same references (scratchpad), which proves its own bookkeeping, not
 * the hardware.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).
 */
#include "netdrv.h"

/* ---------------------------------------------------------------- */
/* the card's ports, from the base                                   */
/* ---------------------------------------------------------------- */

#define EE_DATA     0x00        /* word: card memory at a pointer, which moves on */
#define EE_WRPTR    0x02        /* word: where the next write to EE_DATA goes */
#define EE_RDPTR    0x04        /* word: where the next read of EE_DATA comes from */
#define EE_CA       0x06        /* byte: a write is a channel attention */
#define EE_IRQ      0x07        /* byte: the line's code (bits 2-0), the enable */
#define EE_CONFIG   0x0D        /* byte: the ASIC's configuration */
#define EE_ECTRL    0x0E        /* byte: the EEPROM's lines, the two resets */
#define EE_ID       0x0F        /* byte: the signature, a nibble a read */
#define EE_CONN     0x300E      /* byte: the connector */

#define IRQ_ENABLE  0x08

#define CFG_LOOP    0x02        /* loopback: nothing goes out on the wire */
#define CFG_16BIT   0x04        /* clear: the ASIC in 8-bit mode */

#define EC_SK       0x01        /* the EEPROM's clock */
#define EC_CS       0x02        /* ...chip select */
#define EC_DI       0x04        /* ...data into it */
#define EC_DO       0x08        /* ...data out of it */
#define EC_ASIC     0x40        /* reset the card's ASIC */
#define EC_586      0x80        /* hold the 82586 in reset */

#define CONN_OWN    0x80        /* the card's own transceiver, not AUI */
#define CONN_TPBIT  0x02        /* ...and that is twisted pair, not BNC */

#define EE_SIG      0xBABA      /* the ID nibbles, and the EEPROM's sum */

/* the connectors */
#define XC_AUI      0
#define XC_BNC      1
#define XC_TP       2

/* EEPROM word 0 */
#define W0_LINE     13          /* bits 15-13: the line's code */
#define W0_OWN      0x1000      /* the card's own transceiver */
#define W0_WIDE     0x0400      /* 16-bit operation */
#define W5_TP       0x0001      /* word 5: the own transceiver is twisted pair */

/* ---------------------------------------------------------------- */
/* the 82586's structures, at offsets in card memory                  */
/* ---------------------------------------------------------------- */

#define M_ISCP      0x0000
#define M_SCB       0x0008
#define SCB_STATUS  ( M_SCB + 0 )
#define SCB_CMD     ( M_SCB + 2 )
#define SCB_CBL     ( M_SCB + 4 )       /* the command list */
#define SCB_RFA     ( M_SCB + 6 )       /* the receive frame area */
#define M_CMD       0x0020              /* a command block for setup */
#define M_TX        0x0040              /* the two transmit blocks */
#define TX_SIZE     0x0620
#define TX_TBD      16                  /* the buffer descriptor in a block */
#define TX_DATA     24                  /* the frame */
#define M_RX        ( M_TX + 2 * TX_SIZE )
#define RX_SLOT     0x0620              /* RFD, RBD and buffer */
#define RX_RBD      0x16                /* the RBD in a slot, after the 22-byte RFD */
#define RX_BUF      0x20                /* the buffer, after the 10-byte RBD */
#define RX_BUFLEN   0x0600              /* 1536: its low byte is 0, so adding or
                                           taking off EL changes one byte */
#define RX_MAX      40

/* the SCB status word */
#define ST_CX       0x8000              /* a command with I set finished */
#define ST_FR       0x4000              /* a frame came in */
#define ST_CNA      0x2000              /* the command unit left active */
#define ST_RNR      0x1000              /* the receive unit left ready */
#define ST_CAUSE    0xF000
#define ST_CUS      0x0700              /* the command unit: */
#define CUS_ACTIVE  0x0200              /* ...running a list */
#define ST_RUS      0x0070              /* the receive unit: */
#define RUS_READY   0x0040              /* ...taking frames in */

/* the SCB command word (the acknowledgements are the cause bits) */
#define CUC_START   0x0100
#define CUC_ABORT   0x0400
#define RUC_START   0x0010
#define RUC_ABORT   0x0040

/* command blocks and frame descriptors */
#define CB_C        0x8000              /* status: complete */
#define CB_OK       0x2000              /* status: ...and no error */
#define CB_EL       0x8000              /* command: the end of the list */
#define CMD_IA      1
#define CMD_CONFIG  2
#define CMD_MC      3
#define CMD_TX      4
#define CMD_TDR     5

#define TBD_EOF     0x8000              /* the last buffer of the frame */
#define RBD_EOF     0x8000              /* count: the frame ends in this buffer */
#define RBD_F       0x4000              /* count: the count is filled in */
#define RBD_COUNT   0x3FFF
#define RBD_EL      0x8000              /* size: the end of the list */

/* TDR results */
#define TDR_OK      0x8000
#define TDR_XCVR    0x4000
#define TDR_OPEN    0x2000
#define TDR_SHORT   0x1000
#define TDR_TIME    0x07FF

#define MC_MAX      250                 /* fits in a transmit block */
#define EE_STRIKES  8                   /* commands not taken, in a row, before
                                           the 82586 is left alone as dead */

/* ---------------------------------------------------------------- */
/* what the driver keeps                                             */
/* ---------------------------------------------------------------- */

static u8  ee_called;           /* card_find has run once: the DEVICE= load */
static u8  ee_started;          /* card_setup has run once */
static u8  ee_dead;             /* the 82586 stopped taking commands */
static u8  ee_fails;            /* ...how many times in a row */
static u8  ee_xc;               /* XC_*: the connector used */
static u8  ee_xc_rom;           /* ...and the EEPROM's */
static u8  ee_wide;             /* 16-bit operation */
static u8  ee_code;             /* the EEPROM's line code */
static u8  ee_irqcode;          /* the code card_setup gave the card */
static u16 ee_kb;               /* card memory, kilobytes */
static u32 ee_bytes;            /* ...bytes */

static int rx_n;                /* receive slots */
static int rx_head;             /* the next slot to be filled */
static int rx_tail;             /* the slot with EL */
static int tx_cur;              /* the transmit block to fill next */
static u16 tx_prev;             /* the block started last, 0 none */

static u8  mc_list[MC_MAX * 6];
static int mc_n;

/* the 82586's configuration (Linux's eexpress.c; see the top) */
static const u8 ee_config[12] = {
    0x0C,       /* 12 bytes */
    0x08,       /* FIFO limit 8 */
    0x40,       /* SRDY/ARDY; bad frames not kept */
    0x2E,       /* 6-byte addresses, AL-LOC = 1, 8-byte preamble, no loopback */
    0x00,       /* priority and backoff as the standard has them */
    0x60,       /* 96 bit times between frames */
    0x00, 0xF2, /* slot time 512, 15 retries */
    0x00,       /* not promiscuous, broadcast taken, NRZ, CRC-32 added */
    0x00,       /* carrier and collision sense from the transceiver */
    0x3C,       /* frames under 60 bytes are refused */
    0x00
};

/* the line each EEPROM code means, and the bases the card can have */
static const u8  code_line[8] = { NIC_NOIRQ, 9, 3, 4, 5, 10, 11, NIC_NOIRQ };
static const u16 ee_bases[] = {
    0x300, 0x310, 0x320, 0x330, 0x340, 0x350, 0x360, 0x370,
    0x200, 0x210, 0x220, 0x230, 0x240, 0x250, 0x260, 0x270, 0
};
static const char *const xc_name[3] = { "AUI", "BNC", "TP" };

/* ---------------------------------------------------------------- */
/* the lines this driver says                                        */
/* ---------------------------------------------------------------- */

static char say_buf[96];
static int  say_len;

static void say_text( const char *text )
{
    while ( *text && say_len < (int)sizeof( say_buf ) - 3 ) say_buf[say_len++] = *text++;
}

static void say_begin( const char *text )
{
    say_len = 0;
    say_text( "EtherExpress 16" );
    say_text( text );
}

static void say_hex( u32 value, int digits )
{
    static const char hexdig[] = "0123456789ABCDEF";
    int shift;

    for ( shift = ( digits - 1 ) * 4; shift >= 0; shift -= 4 ) {
        if ( say_len < (int)sizeof( say_buf ) - 3 ) say_buf[say_len++] = hexdig[( value >> shift ) & 15];
    }
}

static void say_dec( u32 value )
{
    char digits[12];
    int  count = 0;

    do { digits[count++] = (char)( '0' + value % 10 ); value /= 10; } while ( value && count < 11 );
    while ( count && say_len < (int)sizeof( say_buf ) - 3 ) say_buf[say_len++] = digits[--count];
}

static void say_end( void )
{
    say_buf[say_len++] = '\r';
    say_buf[say_len++] = '\n';
    say_buf[say_len] = 0;
    net_print( say_buf );
}

/* ---------------------------------------------------------------- */
/* card memory, through the pointers                                 */
/* ---------------------------------------------------------------- */

static u16 mem_rd( u16 off )
{
    u32 flags = int_off();
    u16 value;

    outw( net_port + EE_RDPTR, off );
    value = inw( net_port + EE_DATA );
    int_back( flags );
    return value;
}

static void mem_wr( u16 off, u16 value )
{
    u32 flags = int_off();

    outw( net_port + EE_WRPTR, off );
    outw( net_port + EE_DATA, value );
    int_back( flags );
}

/* COUNT words from WORDS, in order, from OFF */
static void mem_put( u16 off, const u16 *words, u32 count )
{
    u32 flags = int_off();

    outw( net_port + EE_WRPTR, off );
    outsw( net_port + EE_DATA, words, count );
    int_back( flags );
}

/* COUNT words of zero from OFF; past FFFFh the pointer wraps to 0 */
static void mem_zero( u16 off, u32 count )
{
    u32 flags = int_off();
    u32 i;

    outw( net_port + EE_WRPTR, off );
    for ( i = 0; i < count; i++ ) outw( net_port + EE_DATA, 0 );
    int_back( flags );
}

/* ---------------------------------------------------------------- */
/* the SCB                                                           */
/* ---------------------------------------------------------------- */

static u16 scb_status( void )
{
    return mem_rd( SCB_STATUS );
}

/* The command word back at 0: the 82586 has taken the last command.  It
   does within microseconds of a CA; 1000 steps of 10us is ample (Linux
   waits up to 10 ticks and says so when the word does not clear). */
static int scb_idle( void )
{
    int i;

    for ( i = 0; i < 1000; i++ ) {
        if ( mem_rd( SCB_CMD ) == 0 ) return 1;
        net_delay_us( 10 );
    }
    return 0;
}

/* A command (and/or acknowledgements) through the SCB, taken.  With
   interrupts off from the wait to the CA: card_service, finding a command
   written and no CA yet, would wait out its own scb_idle for nothing. */
static int scb_cmd( u16 cmd )
{
    u32 flags = int_off();
    int ok = 0;

    if ( scb_idle() ) {
        mem_wr( SCB_CMD, cmd );
        outb( net_port + EE_CA, 0 );
        ok = scb_idle();
    }
    int_back( flags );
    return ok;
}

/* the command unit started on the list at OFF; the same */
static int cu_start( u16 off )
{
    u32 flags = int_off();
    int ok = 0;

    if ( scb_idle() ) {
        mem_wr( SCB_CBL, off );
        ok = scb_cmd( CUC_START );
    }
    int_back( flags );
    return ok;
}

/* every cause the 82586 has, acknowledged */
static void scb_ack_all( void )
{
    u16 cause;
    int i;

    for ( i = 0; i < 4; i++ ) {
        cause = (u16)( scb_status() & ST_CAUSE );
        if ( cause == 0 ) break;
        if ( !scb_cmd( cause ) ) break;
    }
}

/* Run the one command block at OFF, WORDS long, the end of its own list:
   its final status, or 0 if it did not complete.  .37 seconds is the
   bound the Clarkson packet driver gives (FreeBSD and NetBSD quote it). */
static u16 run_cmd( u16 off, const u16 *words, u32 count )
{
    u16 status = 0;
    int i;

    mem_put( off, words, count );
    if ( !cu_start( off ) ) return 0;
    for ( i = 0; i < 3700; i++ ) {
        status = mem_rd( off );
        if ( status & CB_C ) break;
        net_delay_us( 100 );
    }
    if ( !( status & CB_C ) ) return 0;
    for ( i = 0; i < 1000 && ( scb_status() & ST_CUS ) == CUS_ACTIVE; i++ ) net_delay_us( 10 );
    scb_ack_all();
    return status;
}

/* ---------------------------------------------------------------- */
/* the EEPROM                                                        */
/* ---------------------------------------------------------------- */

static void ec_out( u8 value )
{
    outb( net_port + EE_ECTRL, value );
}

/* Word ADDR of the 93C46, with the 82586 held in reset throughout, as
   Linux and FreeBSD keep it.  The read command is a start bit, 10 and
   six address bits; sixteen data bits follow, each after a rising
   clock.  FreeBSD wants the clock steady for 8.38us: 10 is given. */
static u16 ee_eeprom( int addr )
{
    u16 cmd = (u16)( 0x180 | ( addr & 0x3F ) );
    u16 value = 0;
    u8  line;
    int i;

    ec_out( EC_586 );
    net_delay_us( 2 );
    ec_out( EC_586 | EC_CS );
    net_delay_us( 2 );
    for ( i = 8; i >= 0; i-- ) {
        line = (u8)( EC_586 | EC_CS | ( ( ( cmd >> i ) & 1 ) ? EC_DI : 0 ) );
        ec_out( line );
        net_delay_us( 2 );
        ec_out( (u8)( line | EC_SK ) );
        net_delay_us( 10 );
        ec_out( line );
        net_delay_us( 10 );
    }
    for ( i = 0; i < 16; i++ ) {
        ec_out( EC_586 | EC_CS | EC_SK );
        net_delay_us( 10 );
        value = (u16)( ( value << 1 ) | ( ( inb( net_port + EE_ECTRL ) & EC_DO ) ? 1 : 0 ) );
        ec_out( EC_586 | EC_CS );
        net_delay_us( 10 );
    }
    ec_out( EC_586 );                           /* deselected, and one more */
    net_delay_us( 2 );                          /* clock, as both drivers */
    ec_out( EC_586 | EC_SK );                   /* finish a read */
    net_delay_us( 10 );
    ec_out( EC_586 );
    net_delay_us( 10 );
    return value;
}

/* ---------------------------------------------------------------- */
/* finding the card                                                  */
/* ---------------------------------------------------------------- */

/* The four nibbles at base+0Fh put together; *WHICH gets a bit for each
   nibble number seen.  An empty port reads FFh four times: F000h. */
static u16 ee_id( u16 base, u8 *which )
{
    u16 sum = 0;
    u8  value, seen = 0;
    int i;

    for ( i = 0; i < 4; i++ ) {
        value = inb( base + EE_ID );
        sum = (u16)( sum | ( ( value >> 4 ) << ( ( value & 3 ) << 2 ) ) );
        seen = (u8)( seen | ( 1 << ( value & 3 ) ) );
    }
    *which = seen;
    return sum;
}

static int ee_is_card( u16 base )
{
    u8 which;

    return ee_id( base, &which ) == EE_SIG && which == 0x0F;
}

/* The ASIC reset (Linux waits 500us after it, FreeBSD 240us), the line
   CODE given with no enable bit, and the 82586 left held in reset. */
static void ee_reset_asic( u8 code )
{
    ec_out( EC_ASIC );
    net_delay_us( 10 );
    ec_out( 0 );
    net_delay_us( 500 );
    ec_out( EC_586 );
    outb( net_port + EE_IRQ, code );
    net_delay_us( 100 );
}

/* Card memory in kilobytes, as Linux's eexp_hw_probe: all 64K cleared (a
   32K card twice over), then a mark at each kilobyte until one is there
   already, the memory having wrapped round, or will not stay.  The first
   32 bytes are written once before, as both BSD drivers "warm up" the
   card's DRAM.  The 82586 is held in reset meanwhile. */
static u16 ee_size( void )
{
    u16 kb, off;

    mem_zero( 0, 16 );
    mem_zero( 0, 32768L );
    for ( kb = 0; kb < 64; kb++ ) {
        off = (u16)( kb << 10 );
        if ( mem_rd( off ) != 0 ) break;
        mem_wr( off, (u16)( 0x5000 | kb ) );
        if ( mem_rd( off ) != (u16)( 0x5000 | kb ) ) break;
    }
    return kb;
}

/* Is /NAME on the DEVICE= line?  Only at the load, while NETCORE has the
   line.  A letter or digit straight after NAME makes it another switch.
   None of these begins as NETCORE's do (/P: /M: /I: /PO). */
static int ee_switch( const char *name )
{
    const char *at = net_line;
    char ch;
    int  i;

    if ( at == 0 ) return 0;
    while ( *at && *at != ' ' && *at != '\t' && *at != '\r' && *at != '\n' ) at++;
    for ( ; *at && *at != '\r' && *at != '\n'; at++ ) {
        if ( *at != '/' ) continue;
        for ( i = 0; name[i] && ( at[1 + i] | 0x20 ) == ( name[i] | 0x20 ); i++ ) ;
        if ( name[i] ) continue;
        ch = (char)( at[1 + i] | 0x20 );
        if ( ( ch >= 'a' && ch <= 'z' ) || ( ch >= '0' && ch <= '9' ) ) continue;
        return 1;
    }
    return 0;
}

/* the code the IRQ select register takes for LINE, 0 for none */
static u8 line_code( u8 line )
{
    u8 code;

    for ( code = 1; code < 7; code++ ) {
        if ( code_line[code] == line ) return code;
    }
    return 0;
}

/* the name said at the load: "EtherExpress 16 BNC 32K 16-bit", 31 at
   most (net_name is 32) */
static void ee_name( void )
{
    char *at;

    net_strcpy( net_name, "EtherExpress 16 " );
    at = net_name + 16;
    net_strcpy( at, xc_name[ee_xc] );
    while ( *at ) at++;
    *at++ = ' ';
    *at++ = (char)( '0' + ee_kb / 10 );
    *at++ = (char)( '0' + ee_kb % 10 );
    net_strcpy( at, ee_wide ? "K 16-bit" : "K 8-bit" );
}

/* the first card_find: everything the EEPROM and the card can say */
static int ee_find_first( void )
{
    u16 base = 0, sum, word, w0, w5, mac[3];
    u8  which;
    int i;

    if ( net_opt_port ) {
        base = (u16)net_opt_port;
        sum = ee_id( base, &which );
        if ( sum != EE_SIG || which != 0x0F ) {
            say_begin( ": no card at " );
            say_hex( base, 3 );
            say_text( "h (ID reads " );
            say_hex( sum, 4 );
            say_text( ", BABA wanted)" );
            say_end();
            return 1;
        }
    } else {
        for ( i = 0; ee_bases[i]; i++ ) {
            if ( ee_is_card( ee_bases[i] ) ) { base = ee_bases[i]; break; }
        }
        if ( base == 0 ) {
            say_begin( ": no card answers at 200h-270h or 300h-370h" );
            say_end();
            return 1;
        }
    }
    net_port = base;

    /* interrupts off at the card before anything else is written */
    ee_code = 0;
    outb( net_port + EE_IRQ, 0 );
    ee_reset_asic( 0 );

    sum = 0;
    w0 = w5 = 0;
    for ( i = 0; i < 64; i++ ) {
        word = ee_eeprom( i );
        sum = (u16)( sum + word );
        if ( i == 0 ) w0 = word;
        if ( i == 5 ) w5 = word;
        if ( i >= 2 && i <= 4 ) mac[i - 2] = word;
    }
    for ( i = 0; i < 3; i++ ) {                 /* word 4 is the first two bytes */
        net_mac[i * 2] = (u8)( mac[2 - i] >> 8 );
        net_mac[i * 2 + 1] = (u8)mac[2 - i];
    }
    if ( !net_mac_ok( net_mac ) ) {
        say_begin( " at " );
        say_hex( base, 3 );
        say_text( "h: station address " );
        for ( i = 0; i < 6; i++ ) {
            if ( i ) say_text( "-" );
            say_hex( net_mac[i], 2 );
        }
        say_text( ", EEPROM sum " );
        say_hex( sum, 4 );
        say_end();
        return 1;
    }
    if ( sum != EE_SIG ) {
        /* Linux goes on with a bad sum, the BSD drivers refuse the card;
           the address is good, so it goes on, saying so */
        say_begin( " at " );
        say_hex( base, 3 );
        say_text( "h: EEPROM sum " );
        say_hex( sum, 4 );
        say_text( ", BABA wanted; word 0 is " );
        say_hex( w0, 4 );
        say_end();
    }

    ee_code = (u8)( ( w0 >> W0_LINE ) & 7 );
    if ( code_line[ee_code] == NIC_NOIRQ ) ee_code = 0;
    ee_xc_rom = (u8)( !( w0 & W0_OWN ) ? XC_AUI : ( w5 & W5_TP ) ? XC_TP : XC_BNC );
    ee_xc = ee_xc_rom;
    if ( ee_switch( "TP" ) ) ee_xc = XC_TP;
    else if ( ee_switch( "BNC" ) ) ee_xc = XC_BNC;
    else if ( ee_switch( "AUI" ) ) ee_xc = XC_AUI;
    ee_wide = (u8)( ( w0 & W0_WIDE ) && !ee_switch( "8" ) );
    if ( !ee_wide ) outb( net_port + EE_CONFIG, (u8)( inb( net_port + EE_CONFIG ) & ~CFG_16BIT ) );

    ee_kb = ee_size();
    if ( ee_kb != 16 && ee_kb != 32 && ee_kb != 48 && ee_kb != 64 ) {
        say_begin( " at " );
        say_hex( base, 3 );
        say_text( "h: card memory test gave " );
        say_dec( ee_kb );
        say_text( "K; configuration " );
        say_hex( inb( net_port + EE_CONFIG ), 2 );
        say_text( "h, EEPROM word 0 " );
        say_hex( w0, 4 );
        say_end();
        return 1;
    }
    ee_bytes = (u32)ee_kb << 10;
    rx_n = (int)( ( ee_bytes - 16 - M_RX ) / RX_SLOT );
    if ( rx_n > RX_MAX ) rx_n = RX_MAX;

    if ( ee_xc != ee_xc_rom ) {
        say_begin( ": " );
        say_text( xc_name[ee_xc] );
        say_text( " as the DEVICE= line says; the EEPROM says " );
        say_text( xc_name[ee_xc_rom] );
        say_end();
    }

    /* The line.  /I:2 is the line an AT calls 9; one the card does not
       have runs the card polled, said; with no /I:, the EEPROM's line,
       and polled when it names none.  NETCORE takes net_opt_irq over
       net_irq after this returns, so an /I: refused here is taken off. */
    if ( net_opt_irq == 2 ) net_opt_irq = 9;
    if ( net_opt_irq != NIC_NOIRQ && line_code( net_opt_irq ) == 0 ) {
        say_begin( ": the card cannot use IRQ " );
        say_dec( net_opt_irq );
        say_text( " (3, 4, 5, 9, 10, 11); polled" );
        say_end();
        net_opt_irq = NIC_NOIRQ;
        net_irq = NIC_NOIRQ;
    } else if ( net_opt_irq == NIC_NOIRQ ) {
        net_irq = code_line[ee_code];
        if ( net_irq == NIC_NOIRQ ) {
            say_begin( ": the EEPROM names no IRQ; polled (/I: gives one)" );
            say_end();
        }
    }
    ee_name();
    return 0;
}

int card_find( void )
{
    int first = !ee_called;

    ee_called = 1;
    if ( first ) {
        net_strcpy( net_name, "EtherExpress 16" );
        if ( ee_find_first() ) return 1;
    } else {
        /* after a park: the same card at the same base, the line as
           nd_init settled it; card_setup does the rest */
        if ( !ee_is_card( net_port ) ) return 1;
    }
    net_mem = 0;
    net_bus = NICB_ISA;
    net_flags = (u8)( ( ee_wide ? NICF_WORD : 0 ) | ( net_flags & NICF_POLLED ) );
    return 0;
}

/* ---------------------------------------------------------------- */
/* the multicast list                                                */
/* ---------------------------------------------------------------- */

/* the CRC register after the six bytes of ADDR, low bit of each byte
   first, as QEMU's net_crc32 (its 82596 and 8255x take bits 2-7) */
static u32 mc_crc( const u8 *addr )
{
    u32 crc = 0xFFFFFFFFL;
    u8  byte;
    int i, j, carry;

    for ( i = 0; i < 6; i++ ) {
        byte = addr[i];
        for ( j = 0; j < 8; j++ ) {
            carry = (int)( ( ( crc >> 31 ) ^ byte ) & 1 );
            crc <<= 1;
            byte >>= 1;
            if ( carry ) crc ^= 0x04C11DB7L;
        }
    }
    return crc;
}

/* the windows of the CRC that are covered: bits 2-7, 26-31, 0-5, 24-29 */
static const u8 mc_shift[4] = { 2, 26, 0, 24 };

/* ADDR onto the list if it is wanted (KEEP) or hashes where nothing on
   the list does yet, in any window; SEEN is the hashes taken */
static void mc_take( const u8 *addr, u8 seen[4][8], int keep )
{
    u32 crc = mc_crc( addr );
    int win, idx, fresh = keep;

    for ( win = 0; win < 4; win++ ) {
        idx = (int)( ( crc >> mc_shift[win] ) & 63 );
        if ( !( seen[win][idx >> 3] & ( 1 << ( idx & 7 ) ) ) ) fresh = 1;
    }
    if ( !fresh || mc_n >= MC_MAX ) return;
    for ( win = 0; win < 4; win++ ) {
        idx = (int)( ( crc >> mc_shift[win] ) & 63 );
        seen[win][idx >> 3] = (u8)( seen[win][idx >> 3] | ( 1 << ( idx & 7 ) ) );
    }
    net_memcpy( mc_list + mc_n * 6, addr, 6 );
    mc_n++;
}

static void mc_build( void )
{
    static const u8 netbeui[6] = { 0x03, 0x00, 0x00, 0x00, 0x00, 0x01 };
    static const u8 allhosts[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0x01 };
    u8  seen[4][8];
    u8  addr[6];
    u32 cand;
    int i, full;

    net_memzero( seen, sizeof( seen ) );
    mc_n = 0;
    mc_take( netbeui, seen, 1 );
    mc_take( allhosts, seen, 1 );
    net_memzero( addr, sizeof( addr ) );
    addr[0] = 0x01;                             /* a group address */
    for ( cand = 0; cand < 4096 && mc_n < MC_MAX; cand++ ) {
        addr[4] = (u8)( cand >> 8 );
        addr[5] = (u8)cand;
        mc_take( addr, seen, 0 );
        full = 1;
        for ( i = 0; i < 32; i++ ) {
            if ( ( (u8 *)seen )[i] != 0xFF ) { full = 0; break; }
        }
        if ( full ) break;
    }
}

/* ---------------------------------------------------------------- */
/* receiving                                                         */
/* ---------------------------------------------------------------- */

static u16 rx_slot( int i )
{
    return (u16)( M_RX + (u32)i * RX_SLOT );
}

/* Both lists laid out: each RFD linked to the next, the RBD after it to
   the next RBD, the last of each with EL; the first RFD names the first
   RBD and the others none (0FFFFh) - the 82586 fills those in. */
static void rx_layout( void )
{
    u16 words[16];
    u16 slot, next;
    int i, j;

    for ( i = 0; i < rx_n; i++ ) {
        slot = rx_slot( i );
        next = rx_slot( ( i + 1 ) % rx_n );
        for ( j = 0; j < 16; j++ ) words[j] = 0;
        words[1] = (u16)( i == rx_n - 1 ? CB_EL : 0 );     /* RFD command */
        words[2] = next;                                    /* ...link */
        words[3] = (u16)( i == 0 ? slot + RX_RBD : 0xFFFF );/* ...its RBD */
        words[12] = (u16)( next + RX_RBD );                 /* RBD link */
        words[13] = (u16)( slot + RX_BUF );                 /* ...buffer, low */
        words[15] = (u16)( RX_BUFLEN | ( i == rx_n - 1 ? RBD_EL : 0 ) );
        mem_put( slot, words, 16 );
    }
    rx_head = 0;
    rx_tail = rx_n - 1;
}

/* the lists laid out afresh and the receive unit started at the first */
static int rx_start( void )
{
    u32 flags;
    int ok = 0;

    rx_layout();
    flags = int_off();
    if ( scb_idle() ) {
        mem_wr( SCB_RFA, M_RX );
        ok = scb_cmd( RUC_START );
    }
    int_back( flags );
    return ok;
}

static void rx_restart( void )
{
    (void)scb_cmd( RUC_ABORT );
    (void)rx_start();
}

/* Take the complete frames, oldest first, into NETCORE's queue, each
   slot given back as the new end of both lists: EL on it first, then off
   the old end, so the lists always have an end. */
static void rx_drain( void )
{
    u16 slot, tail, status, rbd, count;
    u32 len, flags;
    u8 *buf;
    int pass;

    for ( pass = 0; pass < rx_n; pass++ ) {
        slot = rx_slot( rx_head );
        status = mem_rd( slot );
        if ( !( status & CB_C ) ) break;
        rbd = mem_rd( (u16)( slot + 6 ) );
        count = mem_rd( (u16)( slot + RX_RBD ) );
        if ( !( status & CB_OK ) || rbd != (u16)( slot + RX_RBD ) ||
             ( count & ( RBD_EOF | RBD_F ) ) != ( RBD_EOF | RBD_F ) ) {
            /* a frame the chip gave up on (out of resources) or one in
               more than one buffer: the lists are no longer in step with
               this driver's count, and are laid out again */
            net_stats.rxerrors++;
            rx_restart();
            return;
        }
        len = count & RBD_COUNT;
        if ( len < NIC_HDRLEN || len > NIC_MAXFRAME ) {
            net_stats.rxerrors++;
        } else {
            /* in words: one byte past an odd frame lands in the slot,
               which has room (QSLOT, NETCORE.C) */
            buf = net_rx_begin();
            flags = int_off();
            outw( net_port + EE_RDPTR, (u16)( slot + RX_BUF ) );
            insw( net_port + EE_DATA, buf, ( len + 1 ) / 2 );
            int_back( flags );
            net_rx_commit( len );
        }
        tail = rx_slot( rx_tail );
        mem_wr( (u16)( slot + RX_RBD ), 0 );                         /* RBD count */
        mem_wr( (u16)( slot + RX_RBD + 8 ), RX_BUFLEN | RBD_EL );    /* new end */
        mem_wr( (u16)( tail + RX_RBD + 8 ), RX_BUFLEN );             /* old end */
        mem_wr( slot, 0 );                                           /* RFD status */
        mem_wr( (u16)( slot + 6 ), 0xFFFF );                         /* ...no RBD */
        mem_wr( (u16)( slot + 2 ), CB_EL );                          /* new end */
        mem_wr( (u16)( tail + 2 ), 0 );                              /* old end */
        rx_tail = rx_head;
        rx_head = ( rx_head + 1 ) % rx_n;
    }
}

/* ---------------------------------------------------------------- */
/* card_setup                                                        */
/* ---------------------------------------------------------------- */

/* the TDR, the 82586's cable test: said only when it finds a fault */
static void ee_cable_test( void )
{
    u16 words[4];
    u16 status, result;

    words[0] = 0;
    words[1] = CB_EL | CMD_TDR;
    words[2] = 0xFFFF;
    words[3] = 0;
    status = run_cmd( M_CMD, words, 4 );
    result = mem_rd( M_CMD + 6 );
    if ( status & CB_C ) {
        if ( result & TDR_OK ) return;
        say_begin( " cable test: " );
        if ( result & TDR_XCVR ) say_text( "transceiver problem (no link?)" );
        else if ( result & ( TDR_OPEN | TDR_SHORT ) ) {
            say_text( result & TDR_OPEN ? "open circuit " : "short circuit " );
            say_dec( result & TDR_TIME );
            say_text( " clocks away" );
        } else {
            say_text( "result " );
            say_hex( result, 4 );
        }
    } else {
        say_begin( " cable test did not finish" );
    }
    say_end();
}

/* a step of card_setup that failed, said at the load */
static int ee_failed( int said, const char *what, u16 value )
{
    if ( said ) {
        say_begin( ": " );
        say_text( what );
        say_hex( value, 4 );
        say_text( ", SCB status " );
        say_hex( scb_status(), 4 );
        say_end();
    }
    return 1;
}

int card_setup( void )
{
    u16 words[12];
    u16 status, top;
    u8  conn, config;
    int said, i, j;

    said = !ee_started;                 /* the first start only: not on
                                           an un-park, which may not print */
    ee_started = 1;
    ee_dead = 0;
    ee_fails = 0;
    ee_irqcode = line_code( net_irq );

    /* The ASIC reset, the line masked, the 82586 held in reset; then the
       connector (Linux: 20ms to settle) and the EEPROM control written
       again after it, as the top says why */
    ee_reset_asic( ee_irqcode );
    conn = (u8)( inb( net_port + EE_CONN ) & ~( CONN_OWN | CONN_TPBIT ) );
    if ( ee_xc == XC_TP ) conn |= CONN_OWN | CONN_TPBIT;
    else if ( ee_xc == XC_BNC ) conn |= CONN_OWN;
    outb( net_port + EE_CONN, conn );
    net_delay_us( 20000 );
    ec_out( EC_586 );
    outb( net_port + EE_IRQ, ee_irqcode );

    /* loopback while the 82586 starts (Linux and both BSD drivers keep
       it off the wire so), and the ASIC's width */
    config = (u8)( inb( net_port + EE_CONFIG ) | CFG_LOOP );
    if ( !ee_wide ) config &= (u8)~CFG_16BIT;
    outb( net_port + EE_CONFIG, config );

    /* card memory: warmed up, cleared at the first start; the ISCP (busy,
       the SCB at 8, base 0), the SCB and the SCP (the bus width, the ISCP
       at 0).  The receive lists are laid out when the unit is started. */
    mem_zero( 0, 16 );
    if ( said ) mem_zero( 0, ee_bytes / 2 );
    words[0] = 0x0001;
    words[1] = M_SCB;
    words[2] = 0;
    words[3] = 0;
    mem_put( M_ISCP, words, 4 );
    mem_zero( M_SCB, 8 );
    top = (u16)( ee_bytes - 12 );
    for ( i = 0; i < 6; i++ ) words[i] = 0;
    words[1] = (u16)( ee_wide ? 0 : 1 );        /* SYSBUS: 1 = 8-bit */
    mem_put( top, words, 6 );
    tx_cur = 0;
    tx_prev = 0;

    /* the 82586 out of reset and told to look: it reads the SCP and the
       ISCP and clears busy.  Linux gives it 5ms and kicks it again with
       another CA when it is slow; so does this */
    ec_out( 0 );
    net_delay_us( 5000 );
    status = 1;
    for ( j = 0; j < 5 && status; j++ ) {
        outb( net_port + EE_CA, 0 );
        for ( i = 0; i < 100; i++ ) {
            status = mem_rd( M_ISCP );
            if ( status == 0 ) break;
            net_delay_us( 100 );
        }
    }
    if ( status ) return ee_failed( said, "the 82586 did not start, ISCP ", status );
    for ( i = 0; i < 100 && !( scb_status() & ST_CAUSE ); i++ ) net_delay_us( 100 );
    scb_ack_all();

    /* CONFIGURE */
    words[0] = 0;
    words[1] = CB_EL | CMD_CONFIG;
    words[2] = 0xFFFF;
    for ( i = 0; i < 6; i++ ) words[3 + i] = (u16)( ee_config[i * 2] | ( ee_config[i * 2 + 1] << 8 ) );
    status = run_cmd( M_CMD, words, 9 );
    if ( ( status & ( CB_C | CB_OK ) ) != ( CB_C | CB_OK ) ) return ee_failed( said, "configure: status ", status );

    /* IA-SETUP: who this is */
    words[1] = CB_EL | CMD_IA;
    for ( i = 0; i < 3; i++ ) words[3 + i] = (u16)( net_mac[i * 2] | ( net_mac[i * 2 + 1] << 8 ) );
    status = run_cmd( M_CMD, words, 6 );
    if ( ( status & ( CB_C | CB_OK ) ) != ( CB_C | CB_OK ) ) return ee_failed( said, "address setup: status ", status );

    /* MC-SETUP, in the first transmit block (free until a frame is sent):
       the list goes in after the four words of its header */
    if ( mc_n == 0 ) mc_build();
    words[1] = CB_EL | CMD_MC;
    words[3] = (u16)( mc_n * 6 );
    mem_put( (u16)( M_TX + 8 ), (const u16 *)mc_list, (u32)mc_n * 3 );
    status = run_cmd( M_TX, words, 4 );
    if ( said && ( status & ( CB_C | CB_OK ) ) != ( CB_C | CB_OK ) ) {
        /* not fatal: unicast and broadcast still come in */
        (void)ee_failed( said, "multicast setup: status ", status );
    }

    /* onto the wire; the cable test at the first start */
    outb( net_port + EE_CONFIG, (u8)( config & ~CFG_LOOP ) );
    if ( said ) ee_cable_test();

    /* the receive unit, on lists laid out afresh */
    if ( !rx_start() ) return ee_failed( said, "the receive unit was not started, command ", mem_rd( SCB_CMD ) );
    for ( i = 0; i < 100 && ( scb_status() & ST_RUS ) != RUS_READY; i++ ) net_delay_us( 100 );
    if ( said && ( scb_status() & ST_RUS ) != RUS_READY ) {
        /* not fatal either: card_service starts it again */
        (void)ee_failed( said, "the receive unit is not ready, RFD ", mem_rd( M_RX ) );
    }

    /* what setting up raised is acknowledged, then the line let out */
    scb_ack_all();
    if ( !( net_flags & NICF_POLLED ) && ee_irqcode ) {
        outb( net_port + EE_IRQ, (u8)( ee_irqcode | IRQ_ENABLE ) );
    }
    return 0;
}

void card_stop( void )
{
    outb( net_port + EE_IRQ, ee_irqcode );          /* masked */
    if ( !ee_dead ) (void)scb_cmd( CUC_ABORT | RUC_ABORT );
    ec_out( EC_586 );                               /* and held in reset */
    tx_prev = 0;
}

/* ---------------------------------------------------------------- */
/* sending                                                           */
/* ---------------------------------------------------------------- */

/* The frame started last finished and the command unit idle: up to
   10000 steps of at least 10us.  A frame takes 1.2ms at most on the
   wire; ten collisions' backoff is about 50ms; past 100ms the network
   is broken.  A failed frame is counted here. */
static int tx_wait( void )
{
    u16 status;
    u32 i;

    for ( i = 0; i < 10000; i++ ) {
        status = (u16)( tx_prev ? mem_rd( tx_prev ) : CB_C );
        if ( ( status & CB_C ) && ( scb_status() & ST_CUS ) != CUS_ACTIVE ) {
            if ( tx_prev && !( status & CB_OK ) ) net_stats.txerrors++;
            tx_prev = 0;
            return 1;
        }
        net_delay_us( 10 );
    }
    return 0;
}

int card_send( const u8 *frame, u32 len )
{
    u16 head[12];
    u16 block, use;
    u32 flags, done;
    int i, ok;

    if ( ee_dead || len < NIC_HDRLEN || len > NIC_MAXFRAME ) { net_stats.txerrors++; return 0; }
    use = (u16)( len < 60 ? 60 : len );
    block = (u16)( M_TX + tx_cur * TX_SIZE );

    /* the block: transmit, the end of its list; its buffer descriptor;
       the frame, padded with zeroes, read no further than LEN */
    for ( i = 0; i < 12; i++ ) head[i] = 0;
    head[1] = CB_EL | CMD_TX;
    head[2] = 0xFFFF;
    head[3] = (u16)( block + TX_TBD );
    head[8] = (u16)( TBD_EOF | use );
    head[9] = 0xFFFF;
    head[10] = (u16)( block + TX_DATA );
    flags = int_off();
    outw( net_port + EE_WRPTR, block );
    outsw( net_port + EE_DATA, head, 12 );
    outsw( net_port + EE_DATA, frame, len / 2 );
    done = len & ~1L;
    if ( len & 1 ) {
        outw( net_port + EE_DATA, frame[len - 1] );
        done += 2;
    }
    for ( ; done < use; done += 2 ) outw( net_port + EE_DATA, 0 );
    int_back( flags );

    /* the other block finished (it may have been going out meanwhile);
       one that never does is aborted, and this frame sent anyway */
    if ( !tx_wait() ) {
        net_stats.txerrors++;
        (void)scb_cmd( CUC_ABORT );
        tx_prev = 0;
    }
    ok = cu_start( block );
    if ( !ok ) {
        net_stats.txerrors++;
        if ( ++ee_fails >= EE_STRIKES ) ee_dead = 1;
        return 0;
    }
    ee_fails = 0;
    tx_prev = block;
    tx_cur ^= 1;
    return 1;
}

/* ---------------------------------------------------------------- */
/* card_service                                                      */
/* ---------------------------------------------------------------- */

/* Called on the card's line and on every clock tick.  1 only when the
   82586 had a cause up; the slots are looked at either way (one read when
   none is full), so a frame whose FR the end of card_setup acknowledged
   is not left waiting for the next one. */
int card_service( void )
{
    u32 flags;
    u16 status, cause;
    int pass, mine = 0;

    if ( ee_dead ) return 0;
    flags = int_off();
    status = scb_status();
    if ( status == 0xFFFF ) {                   /* nothing answering */
        int_back( flags );
        return 0;
    }
    cause = (u16)( status & ST_CAUSE );
    for ( pass = 0; pass < 8; pass++ ) {
        if ( cause ) {
            /* the line masked while the causes are answered, from the
               first one seen; each acknowledged before it is acted on, so
               that what comes in meanwhile is seen */
            if ( !mine && !( net_flags & NICF_POLLED ) ) outb( net_port + EE_IRQ, ee_irqcode );
            mine = 1;
            if ( !scb_cmd( cause ) ) {
                if ( ++ee_fails >= EE_STRIKES ) ee_dead = 1;
                break;
            }
            ee_fails = 0;
        }
        rx_drain();
        status = scb_status();
        if ( ( status & ST_RUS ) != RUS_READY ) {
            /* out of resources (or a start that did not take): what came
               in is taken, and the unit started again */
            net_stats.overflows++;
            rx_drain();
            rx_restart();
            status = scb_status();
        }
        cause = (u16)( status & ST_CAUSE );
        if ( !cause ) break;
    }
    if ( mine && !( net_flags & NICF_POLLED ) && ee_irqcode ) {
        /* let out again: a cause still up makes a new edge */
        outb( net_port + EE_IRQ, (u8)( ee_irqcode | IRQ_ENABLE ) );
    }
    int_back( flags );
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'E', 'X', 'P', '1', '6', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
