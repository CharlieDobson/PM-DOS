/*
 * EL3C509.C - EL3C509.SYS, a driver for the 3Com EtherLink III (3C509)
 *
 * The ISA 10Mbit card in Charlie's 486.  No emulator has it, so this is
 * written to the 3Com specification and the long-standing DOS and Linux
 * drivers and is for the real card to prove - it has NOT been run.
 *
 * THE 3C509 is programmed I/O, no bus master: a frame is pushed into a
 * transmit FIFO through one port and pulled out of a receive FIFO
 * through the same, the card keeping the counts.  Its registers are
 * banked - a WINDOW is selected and then the sixteen ports mean
 * different things - which is the one thing to keep in mind reading
 * this.  Window 1 is the operating window and the driver leaves it
 * selected except while reading the address EEPROM (window 0) or
 * setting the station address (window 2).
 *
 * FINDING IT.  An EtherLink III that the BIOS or a 3Com utility has not
 * configured answers at no address until the ID sequence is sent to the
 * ID port: 255 writes of a walking pattern put every card into the ID
 * state, each card's EEPROM is read back a bit at a time on the data
 * line, and the card is told its I/O base and activated.  /P:base skips
 * all that for a card already at a known port.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).
 */
#include "netdrv.h"

#define ID_PORT     0x110       /* where the ID sequence is written */

/* the command/status register, base+0x0E, in every window */
#define EL_CMD      0x0E
#define EL_STATUS   0x0E

/* commands: the top five bits of the word written to EL_CMD */
#define CMD_RESET       0x0000
#define CMD_SELWIN      0x0800  /* | window number */
#define CMD_RXENABLE    0x2000
#define CMD_RXRESET     0x2800
#define CMD_RXDISCARD   0x4000  /* drop the frame at the top of the RX FIFO */
#define CMD_TXENABLE    0x4800
#define CMD_TXRESET     0x5800
#define CMD_ACKINTR     0x6800  /* | the bits to acknowledge */
#define CMD_SETINTR     0x7000  /* | the interrupt mask */
#define CMD_SETZERO     0x7800  /* | the read-zero mask */
#define CMD_SETFILTER   0x8000  /* | the receive filter */
#define CMD_STATSDIS    0xB000

/* status bits (EL_STATUS) */
#define ST_INTLATCH 0x0001
#define ST_TXAVAIL  0x0008
#define ST_RXCOMP   0x0010
#define ST_CMDBUSY  0x1000

/* window 1 operating registers */
#define W1_TXFIFO   0x00        /* write a frame here */
#define W1_RXFIFO   0x00        /* read one here */
#define W1_RXSTATUS 0x08        /* word: the frame at the top of the FIFO */
#define W1_TXSTATUS 0x0B        /* byte */
#define W1_TXFREE   0x0C        /* word: bytes free in the transmit FIFO */

#define RXS_INCOMPLETE 0x8000   /* the frame is still arriving */
#define RXS_ERROR      0x4000
#define RXS_LEN        0x07FF

#define FILTER_STATION 0x01
#define FILTER_MULTI   0x02     /* receive all multicast frames */
#define FILTER_BCAST   0x04

#define SPIN_CMD    100000L
#define SPIN_TX     400000L

static u16 id_port = ID_PORT;

static const PCI_ID el3_ids[] = { { 0, 0 } };   /* ISA: found by the ID port */

static void el_cmd( u16 cmd )
{
    u32 n;
    u16 st;
    outw( net_port + EL_CMD, cmd );
    for ( n = 0; n < SPIN_CMD; n++ ) {
        st = inw( net_port + EL_STATUS );
        if ( st == 0xFFFF ) break;          /* nothing answering: do not spin -
                                               a dark card here is what pinned
                                               the processor, every command
                                               waiting the full count */
        if ( !( st & ST_CMDBUSY ) ) break;
    }
}

static void el_window( int win )
{
    outw( net_port + EL_CMD, (u16)( CMD_SELWIN | win ) );
}

/* the ID sequence: a walking pattern the cards' logic recognises */
static void id_sequence( void )
{
    u8 al = 0xFF;
    int i;

    outb( (u16)id_port, 0 );
    outb( (u16)id_port, 0 );
    for ( i = 0; i < 255; i++ ) {
        outb( (u16)id_port, al );
        al = (u8)( ( al << 1 ) ^ ( ( al & 0x80 ) ? 0xCF : 0x00 ) );
    }
}

/* read a word of a card's EEPROM through the ID port, a bit at a time */
static u16 id_eeprom( int addr )
{
    u16 val = 0;
    int i;

    outb( (u16)id_port, (u8)( 0x80 | ( addr & 0x3F ) ) );   /* read-register command */
    for ( i = 0; i < 200; i++ ) inb( 0x80 );                 /* the card needs time -
                                                                a real IN the optimiser
                                                                cannot drop, not the ID
                                                                port the bits come from */
    for ( i = 0; i < 16; i++ ) {
        val = (u16)( ( val << 1 ) | ( inw( (u16)id_port ) & 1 ) );
    }
    return val;
}

/* bring one unconfigured card to a known base and activate it */
static int id_activate( void )
{
    u16 w;
    int i;

    /* try a few ID ports: the sequence selects the port too */
    for ( id_port = 0x110; id_port <= 0x1F0; id_port += 0x10 ) {
        id_sequence();
        outb( (u16)id_port, 0xD0 );             /* global reset of the ID state */
        id_sequence();
        /* the manufacturer ID word must be 3Com's 0x6D50 */
        w = id_eeprom( 7 );
        if ( w == 0x6D50 ) break;
    }
    if ( id_port > 0x1F0 ) return 0;

    /* the card's configured I/O base is EEPROM word 8, low 5 bits times
       16 plus 0x200 */
    w = id_eeprom( 8 );
    net_port = (u16)( 0x200 + ( ( w & 0x1F ) << 4 ) );
    /* the station address is EEPROM words 0..2, big-endian */
    for ( i = 0; i < 3; i++ ) {
        w = id_eeprom( i );
        net_mac[i * 2] = (u8)( w >> 8 );
        net_mac[i * 2 + 1] = (u8)w;
    }
    /* activate: tell the card its tag and move it from ID state to the
       command state at its base (write 0xFF then the base-select) */
    outb( (u16)id_port, (u8)( 0xE0 | 0 ) );     /* set the tag to 0 */
    outb( (u16)id_port, 0xFF );                 /* activate and map at the base */
    return 1;
}

/* read the station address from the EEPROM in window 0, at net_port */
static void el_read_mac( void )
{
    int i;
    u32 n;
    u16 w;

    el_window( 0 );
    for ( i = 0; i < 3; i++ ) {
        outw( net_port + 0x0A, (u16)( 0x80 | i ) );     /* EEPROM read command */
        /* the read takes about 162us.  Poll the busy bit (window 0, port
           0x0A bit 15); the IN is evaluated every time round, so it is a
           delay the optimiser cannot drop, and the word is not read before
           a floor well above that time has passed. */
        for ( n = 0; n < 50000; n++ ) {
            if ( !( inw( net_port + 0x0A ) & 0x8000 ) && n >= 300 ) break;
        }
        w = inw( net_port + 0x0C );
        net_mac[i * 2] = (u8)( w >> 8 );
        net_mac[i * 2 + 1] = (u8)w;
    }
}

/* Diagnostic for the /P: path: the six bytes el_read_mac got from the base
   given on the DEVICE= line.  A card that does not answer there reads back
   FF-FF-FF-FF-FF-FF (an open bus) or 00-00-..., which tells that apart from
   an address that came out wrong.  Remove once the card is proven. */
static void el_say_read( void )
{
    static const char hexdig[] = "0123456789ABCDEF";
    char text[48];
    int i, p;

    net_strcpy( text, "3C509 read " );
    for ( p = 0; text[p]; p++ ) ;
    for ( i = 0; i < 6; i++ ) {
        if ( i ) text[p++] = '-';
        text[p++] = hexdig[net_mac[i] >> 4];
        text[p++] = hexdig[net_mac[i] & 15];
    }
    text[p++] = '\r';
    text[p++] = '\n';
    text[p] = 0;
    net_print( text );
}

/* Diagnostic: the status register (command port, window-independent) at a
   labelled point in setup.  FFFF means the card has stopped decoding its
   base - gone dark.  Remove once the card is proven. */
static void el_say_reg( const char *label )
{
    static const char hexdig[] = "0123456789ABCDEF";
    char text[48];
    const char *s;
    u16 st;
    int p;

    st = inw( net_port + EL_STATUS );
    net_strcpy( text, "3C509 " );
    for ( p = 0; text[p]; p++ ) ;
    for ( s = label; *s; ) text[p++] = *s++;
    text[p++] = '=';
    text[p++] = hexdig[( st >> 12 ) & 15];
    text[p++] = hexdig[( st >> 8 ) & 15];
    text[p++] = hexdig[( st >> 4 ) & 15];
    text[p++] = hexdig[st & 15];
    text[p++] = '\r';
    text[p++] = '\n';
    text[p] = 0;
    net_print( text );
}

/* Diagnostic: a 32-bit value (the window-3 InternalConfig, to see which
   transceiver the card is wired for).  Remove once the card is proven. */
static void el_say_hex32( const char *label, u32 val )
{
    static const char hexdig[] = "0123456789ABCDEF";
    char text[48];
    const char *s;
    int p, shift;

    net_strcpy( text, "3C509 " );
    for ( p = 0; text[p]; p++ ) ;
    for ( s = label; *s; ) text[p++] = *s++;
    text[p++] = '=';
    for ( shift = 28; shift >= 0; shift -= 4 ) text[p++] = hexdig[( val >> shift ) & 15];
    text[p++] = '\r';
    text[p++] = '\n';
    text[p] = 0;
    net_print( text );
}

/* ISA Plug and Play.  A 3C509B defaults to Plug and Play and answers at no
   port until it is isolated and switched on - the ID sequence above never
   reaches it.  The kernel isolates the cards; here the 3Com one is matched by
   its Plug and Play id, given an I/O base and a line, and activated through
   the kernel's services.  After that it is an ordinary 3C509 at that base and
   the rest of the driver drives it unchanged.  A card a Plug and Play BIOS
   switched on already is taken where it is.  The id's vendor bytes are 'TCM'
   (0x50 0x6D), the same 3Com maker word the legacy EEPROM check reads. */
static const u8 pnp_3com[][4] = {
    { 0x50, 0x6D, 0x50, 0x90 },     /* TCM5090  3C509B-TPO  */
    { 0x50, 0x6D, 0x50, 0x91 },     /* TCM5091  3C509B-TP   */
    { 0x50, 0x6D, 0x50, 0x92 },     /* TCM5092              */
    { 0x50, 0x6D, 0x50, 0x93 },     /* TCM5093              */
    { 0x50, 0x6D, 0x50, 0x94 },     /* TCM5094  3C509B-Combo*/
    { 0x50, 0x6D, 0x50, 0x95 },     /* TCM5095  3C509B-TPC  */
    { 0x50, 0x6D, 0x50, 0x98 }      /* TCM5098  3C509B-TPO  */
};
#define PNP_3COM_N ( (int)( sizeof( pnp_3com ) / sizeof( pnp_3com[0] ) ) )
static const u8 pnp_irq_pref[] = { 10, 11, 5, 9, 3, 15, 12, 4, 0 };

static int same4( const u8 *a, const u8 *b )
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int pnp_id_3com( const u8 *id )
{
    int i;
    for ( i = 0; i < PNP_3COM_N; i++ ) {
        if ( same4( id, pnp_3com[i] ) ) return 1;
    }
    return 0;
}

static int find_pnp( void )
{
    REGS r;
    u8  *res, *regs, pairs[12], tag;
    u32  cards, csn, len, pos, size, kind;
    u32  io_min, io_max, io_align, irq_mask;
    int  ours, devices, i, line;
    u16  choice;

    net_memzero( &r, sizeof( r ) );
    r.eax = (u32)BUS_PNPHERE << 8;
    if ( !nds_bus( &r ) || r.eax == 0 ) return 0;
    cards = r.eax;
    res = nds_alloc( 2048 + 256 );
    if ( res == 0 ) return 0;
    regs = res + 2048;

    for ( csn = 1; csn <= cards; csn++ ) {
        net_memzero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PNPRES << 8;
        r.edx = csn;
        r.edi = (u32)res;
        r.ecx = 2048;
        if ( !nds_bus( &r ) || r.ecx < 9 ) continue;
        len = r.ecx;

        /* the card's own id, then its first logical device's id and what
           that is compatible with; the device's first port range and first
           line mask - the first set of choices when it has sets */
        ours = pnp_id_3com( res );
        devices = 0;
        io_min = io_max = io_align = irq_mask = 0;
        pos = 9;
        while ( pos < len ) {
            tag = res[pos++];
            if ( tag & 0x80 ) {                 /* a large item: skipped */
                if ( pos + 2 > len ) break;
                size = res[pos] | ( (u32)res[pos + 1] << 8 );
                pos += 2 + size;
                continue;
            }
            kind = ( tag >> 3 ) & 15;
            size = tag & 7;
            if ( pos + size > len || kind == 15 ) break;
            switch ( kind ) {
            case 2:                             /* a logical device */
                if ( devices++ ) break;
                if ( size >= 4 && pnp_id_3com( res + pos ) ) ours = 1;
                break;
            case 3:                             /* ...compatible with */
                if ( devices == 1 && size >= 4 && pnp_id_3com( res + pos ) ) ours = 1;
                break;
            case 4:                             /* a line */
                if ( devices == 1 && size >= 2 && irq_mask == 0 ) irq_mask = res[pos] | ( (u32)res[pos + 1] << 8 );
                break;
            case 8:                             /* a port range */
                if ( devices == 1 && size >= 7 && io_align == 0 ) {
                    io_min = res[pos + 1] | ( (u32)res[pos + 2] << 8 );
                    io_max = res[pos + 3] | ( (u32)res[pos + 4] << 8 );
                    io_align = res[pos + 5] ? res[pos + 5] : 1;
                }
                break;
            case 9:                             /* a port range of one */
                if ( devices == 1 && size >= 3 && io_align == 0 ) {
                    io_min = io_max = res[pos] | ( (u32)res[pos + 1] << 8 );
                    io_align = 1;
                }
                break;
            }
            if ( devices > 1 ) break;
            pos += size;
        }
        if ( !ours ) continue;

        /* Where it should live.  A card CTCM or a Plug and Play BIOS has
           configured keeps its base and line in its registers (0x30 reads
           on); one left untouched is given 0x300 if its range allows, and
           the first free line it will take. */
        net_memzero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PNPREGS << 8;
        r.edx = csn;                            /* DH = device 0 */
        r.edi = (u32)regs;
        if ( nds_bus( &r ) && ( regs[0x30] & 1 ) && ( regs[0x60] | regs[0x61] ) ) {
            choice = (u16)( ( (u16)regs[0x60] << 8 ) | regs[0x61] );
            line = regs[0x70] & 15;
        } else {
            if ( io_align == 0 ) continue;
            /* 0x300 when the range allows it, else the range's first; the
               first line the card allows that nothing here has already */
            choice = (u16)io_min;
            if ( io_min <= 0x300 && 0x300 <= io_max && ( ( 0x300 - io_min ) % io_align ) == 0 ) choice = 0x300;
            line = 0;
            for ( i = 0; pnp_irq_pref[i]; i++ ) {
                if ( !( irq_mask & ( 1L << pnp_irq_pref[i] ) ) ) continue;
                net_memzero( &r, sizeof( r ) );
                r.eax = (u32)BUS_IRQSTAT << 8;
                r.edx = pnp_irq_pref[i];
                if ( nds_bus( &r ) && ( r.eax & ( IRQF_KERNEL | IRQF_HOOKED | IRQF_OPEN | IRQF_DEAD ) ) ) continue;
                line = pnp_irq_pref[i];
                break;
            }
        }

        /* Decoding off, set the port and the line, decoding on - a device is
           moved with its decoding off, as the specification says.  THIS IS
           DONE EVEN FOR A CARD WHOSE REGISTERS ALREADY READ ON: the kernel
           re-isolates the Plug and Play cards at boot, after which a 3C509B
           reads its old base back but does not decode it again until its
           activate register is written.  Writing it here is what ISAPNP's /A
           does and what makes the card answer at the base. */
        pairs[0] = 0x30;  pairs[1] = 0;
        pairs[2] = 0x60;  pairs[3] = (u8)( choice >> 8 );
        pairs[4] = 0x61;  pairs[5] = (u8)choice;
        pairs[6] = 0x70;  pairs[7] = (u8)line;
        pairs[8] = 0x71;  pairs[9] = 2;
        pairs[10] = 0x30; pairs[11] = 1;
        net_memzero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PNPSET << 8;
        r.edx = csn;
        r.esi = (u32)pairs;
        r.ecx = 6;
        if ( !nds_bus( &r ) ) continue;

        net_port = choice;
        net_irq = line ? (u8)line : NIC_NOIRQ;
        el_read_mac();
        el_say_read();                          /* diagnostic, see above */
        if ( !net_mac_ok( net_mac ) ) continue;
        nds_free( res );
        return 1;
    }
    nds_free( res );
    return 0;
}

int card_find( void )
{
    if ( net_opt_port ) {
        /* a card already at a known base (/P:) - read its address */
        net_port = (u16)net_opt_port;
        el_read_mac();
        el_say_read();                          /* diagnostic, see above */
    } else if ( find_pnp() ) {
        /* an ISA Plug and Play 3C509B: isolated, given a base and a line,
           and switched on by find_pnp; its address is already read */
        ;
    } else if ( id_activate() ) {
        /* a legacy (non-PnP) 3C509: the ID sequence set the base + address */
        ;
    } else {
        return 1;
    }
    if ( !net_mac_ok( net_mac ) ) return 1;

    net_bus = NICB_ISA;
    net_flags = NICF_WORD;
    (void)el3_ids;
    net_strcpy( net_name, "3Com EtherLink III (3C509)" );
    return 0;
}

int card_setup( void )
{
    int i;
    u32 media_cfg, xcvr, d;

    el_say_reg( "entry" );                  /* alive from find_pnp */

    /* GlobalReset loads the card's internal configuration - the transceiver
       and the receive datapath - from the EEPROM.  WITHOUT it the receiver
       takes nothing in (window 3's InternalConfig reads FFFF).  The reset
       drops the Plug and Play I/O decode, so the card is re-found and re-
       activated straight after; the command-wait guard keeps the card, dark
       for that moment, from pinning the processor in between. */
    outw( net_port + EL_CMD, CMD_RESET );
    for ( d = 0; d < 6000; d++ ) inb( 0x80 );       /* ~3ms to settle */
    el_say_reg( "post-reset" );
    find_pnp();                             /* re-activate: restore the decode */
    el_say_reg( "post-react" );

    el_window( 2 );                         /* the station address */
    for ( i = 0; i < 6; i++ ) outb( net_port + i, net_mac[i] );

    el_window( 1 );
    el_cmd( CMD_RXRESET );
    el_cmd( CMD_TXRESET );

    /* the transceiver the card is wired for, now its config is loaded: twisted
       pair needs its link beat enabled (window 4 MediaType |= 0x00C0), coax
       needs the DC converter started.  The config is printed to confirm it. */
    el_window( 3 );
    media_cfg = (u32)inw( net_port + 0x00 ) | ( (u32)inw( net_port + 0x02 ) << 16 );
    el_say_hex32( "cfg", media_cfg );
    xcvr = ( media_cfg >> 20 ) & 7;
    el_window( 4 );
    if ( xcvr == 3 ) {
        el_cmd( 0x1000 );                   /* 10Base2 coax: StartCoax */
    } else {
        outw( net_port + 0x0A, (u16)( inw( net_port + 0x0A ) | 0x00C0 ) );  /* TP link beat + jabber */
    }
    el_window( 1 );

    el_cmd( CMD_STATSDIS );
    /* station, broadcast AND multicast: the NetBEUI functional address
       03-00-00-00-00-01 is a multicast, so a card that is to carry NetBEUI
       must take multicast frames in, not only station-match and broadcast */
    el_cmd( (u16)( CMD_SETFILTER | FILTER_STATION | FILTER_MULTI | FILTER_BCAST ) );
    el_cmd( CMD_RXENABLE );
    el_cmd( CMD_TXENABLE );
    el_say_reg( "post-enable" );            /* still answering after setup? */
    /* acknowledge everything, then let RX-complete interrupt unless polled.
       NOT TX-available: the TxAvailable status is a level that is set
       whenever the transmit FIFO has room, which at idle is always, so to
       enable it is to take an interrupt that re-asserts the instant it is
       acknowledged - a storm that pins the processor and starves everything
       else.  The transmit path polls the free-byte count (card_send) and
       wants no interrupt of its own. */
    el_cmd( (u16)( CMD_ACKINTR | 0xFF ) );
    if ( net_flags & NICF_POLLED ) el_cmd( CMD_SETINTR );
    else el_cmd( (u16)( CMD_SETINTR | ST_RXCOMP | ST_INTLATCH ) );
    return 0;
}

void card_stop( void )
{
    el_window( 1 );
    el_cmd( CMD_SETINTR );                       /* mask everything */
    el_cmd( 0x1800 );                            /* RX disable */
    el_cmd( 0x5000 );                            /* TX disable */
}

int card_send( const u8 *frame, u32 len )
{
    static u8 pad[64];
    u32 n, words;
    u32 use = len;

    if ( use < 60 ) {
        net_memzero( pad, sizeof( pad ) );
        net_memcpy( pad, frame, len );
        frame = pad;
        use = 60;
    }
    /* wait for room: the preamble is four bytes plus the frame */
    for ( n = 0; n < SPIN_TX; n++ ) {
        if ( inw( net_port + W1_TXFREE ) >= use + 4 ) break;
    }
    if ( n == SPIN_TX ) { net_stats.txerrors++; return 0; }
    outw( net_port + W1_TXFIFO, (u16)use );      /* length */
    outw( net_port + W1_TXFIFO, 0 );             /* the second preamble word */
    words = use / 2;
    for ( n = 0; n < words; n++ ) outw( net_port + W1_TXFIFO, ( (u16)frame[n * 2] ) | ( (u16)frame[n * 2 + 1] << 8 ) );
    if ( use & 1 ) outw( net_port + W1_TXFIFO, frame[use - 1] );
    return 1;
}

static void rx_drain( void )
{
    u16 status, prev = 0;
    u32 len, words, n, wait;
    u8 *slot;

    for ( n = 0; n < 16; n++ ) {
        status = inw( net_port + W1_RXSTATUS );
        if ( status & RXS_INCOMPLETE ) break;    /* still arriving, or none */
        if ( ( status & RXS_LEN ) == 0 && !( status & RXS_ERROR ) ) break;  /* FIFO empty */
        /* THE DISCARD DID NOT ADVANCE THE FIFO.  The same status word coming
           back after a discard means the top packet was not dropped, and
           reading it again - as the loop would - is the per-tick spin that
           pins the processor.  Flush the receiver instead (RxReset clears
           the filter, so set it again and re-enable) and give up this pass. */
        if ( status == prev ) {
            el_cmd( CMD_RXRESET );
            el_cmd( (u16)( CMD_SETFILTER | FILTER_STATION | FILTER_MULTI | FILTER_BCAST ) );
            el_cmd( CMD_RXENABLE );
            net_stats.rxerrors++;
            break;
        }
        prev = status;
        len = status & RXS_LEN;
        if ( ( status & RXS_ERROR ) || len < NIC_HDRLEN || len > NIC_MAXFRAME ) {
            net_stats.rxerrors++;
        } else {
            slot = net_rx_begin();
            words = ( len + 1 ) / 2;
            {
                u32 i;
                u16 *w = (u16 *)slot;
                for ( i = 0; i < words; i++ ) w[i] = inw( net_port + W1_RXFIFO );
            }
            net_rx_commit( len );
        }
        el_cmd( CMD_RXDISCARD );                 /* drop this frame from the FIFO */
        /* wait for the discard to finish on its own counter - NOT n, which
           walks the frames; reusing it here would cut the drain short */
        for ( wait = 0; wait < SPIN_CMD; wait++ ) if ( !( inw( net_port + EL_STATUS ) & ST_CMDBUSY ) ) break;
    }
}

int card_service( void )
{
    u16 st;
    u32 n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        st = inw( net_port + EL_STATUS );
        /* the interrupt latch, and a complete frame, are what is ours to act
           on.  TxAvailable is NOT tested: its status is set at idle whether
           or not an interrupt fired, so reading it as "mine" would claim
           every interrupt, this card's or another's on a shared line. */
        if ( !( st & ( ST_RXCOMP | ST_INTLATCH ) ) ) break;
        mine = 1;
        if ( st & ST_RXCOMP ) rx_drain();
        el_cmd( (u16)( CMD_ACKINTR | ( st & 0xFF ) ) );
    }
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { '3', 'C', '5', '0', '9', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
