/*
 * EL3C509.C - EL3C509.SYS, a driver for the 3Com EtherLink III (3C509)
 *
 * The ISA 10Mbit card in Charlie's 486.  No emulator has it, so this is
 * written to the 3Com specification and the long-standing DOS and Linux
 * drivers and is for the real card to prove.  On the 486 (2026-10-22/24)
 * the Plug and Play path found the card and frames went out, but none
 * came in; the status mask below is the reason found for that since.
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
 * FINDING IT, in this order:
 *
 *   /P:base   a card already at a known port.  Nothing else is tried.
 *   PLUG AND PLAY.  A 3C509B set to Plug and Play is numbered by the
 *             kernel at boot (IO\ISAPNP.INC), whatever the BIOS did
 *             before; here it is matched by its id, given a port and a
 *             line, and switched on.
 *   A CARD THAT IS ON ALREADY, at one of the bases 200h to 3E0h with
 *             window 0 selected: 3Com's maker word 6D50h at the base.
 *             Only reads - nothing is written to a port until the word
 *             is there.
 *   THE ID PORT.  An EtherLink III that nothing has configured answers
 *             at no address until the ID sequence is written to the ID
 *             port: 255 writes of a walking pattern put every card into
 *             the ID state, each card's EEPROM is read back a bit at a
 *             time, and the card is told its I/O base and activated.  A
 *             Plug and Play 3C509B answers the sequence too (Linux's
 *             3c509.c says so, and skips the cards its isapnp side took).
 *
 * When none of them finds a card, a line for each says what was tried
 * and what came back, so a failed test on the real machine says why.
 *
 * THE PAUSES ARE TIMED BY THE CLOCK.  The EEPROM needs 162us a word and
 * the card some milliseconds after a reset.  A loop count is a time
 * only on the machine it was tried on, and a read of port 80h is an ISA
 * cycle only where the chipset passes it to the ISA bus, so the timer's
 * channel 0 is read instead (el_wait_us).
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).
 */
#include "netdrv.h"

#define ID_PORT     0x110       /* where the ID sequence is written first */

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
#define CMD_SETZERO     0x7800  /* | the status bits that may be seen: the
                                   rest read as zero and interrupt nobody */
#define CMD_SETFILTER   0x8000  /* | the receive filter */
#define CMD_STATSDIS    0xB000

/* status bits (EL_STATUS) */
#define ST_INTLATCH 0x0001
#define ST_TXCOMP   0x0004
#define ST_TXAVAIL  0x0008
#define ST_RXCOMP   0x0010
#define ST_CMDBUSY  0x1000

/* window 0: the setup registers */
#define W0_MAKER    0x00        /* word: 6D50h, 3Com's */
#define W0_CFGCTRL  0x04        /* bit 0: the adapter enabled */
#define W0_RESCFG   0x08        /* bits 15-12: the line */
#define W0_EECMD    0x0A        /* EEPROM command; bit 15 = busy */
#define W0_EEDATA   0x0C

/* window 1 operating registers */
#define W1_TXFIFO   0x00        /* write a frame here */
#define W1_RXFIFO   0x00        /* read one here */
#define W1_RXSTATUS 0x08        /* word: the frame at the top of the FIFO */
#define W1_TXSTATUS 0x0B        /* byte: the transmit status stack */
#define W1_TXFREE   0x0C        /* word: bytes free in the transmit FIFO */

#define RXS_INCOMPLETE 0x8000   /* the frame is still arriving */
#define RXS_ERROR      0x4000
#define RXS_LEN        0x07FF

#define FILTER_STATION 0x01
#define FILTER_MULTI   0x02     /* receive all multicast frames */
#define FILTER_BCAST   0x04

#define EL_MAKER    0x6D50

#define SPIN_CMD    100000L
#define SPIN_TX     400000L

/* how the card was found, which is how it is brought back after the
   reset in card_setup */
#define HOW_PORT    1           /* /P: */
#define HOW_PNP     2
#define HOW_SEEN    3           /* on already, found by the maker word */
#define HOW_ID      4

static u16 id_port = ID_PORT;
static u8  el_how;              /* HOW_* */
static u8  el_called;           /* card_find has run once: the DEVICE= load */
static u8  el_started;          /* card_setup has run once */
static u8  el_card_irq;         /* the line the card says it has */
static u8  pnp_csn;             /* HOW_PNP: the card's number */
static u8  pnp_line;            /* ...and the line it was given */

/* what each way of finding the card saw, for the lines said when none
   finds it */
static u32 rep_pnp_cards;       /* Plug and Play cards the kernel numbered */
static u32 rep_pnp_3com;        /* ...of them 3C509Bs */
static u8  rep_pnp_id[4];       /* the first other card's id */
static u8  rep_id_kept;         /* ...is in rep_pnp_id */
static u16 rep_id_word;         /* EEPROM word 7 through the first ID port */
static u16 rep_id_port;         /* the ID port a card answered at, or 0 */
static u16 rep_id_base;         /* ...the base it was put at */
static u16 rep_id_read;         /* ...and what that base read back */

static const PCI_ID el3_ids[] = { { 0, 0 } };   /* ISA: found by the ID port */

/* ---------------------------------------------------------------- */
/* time                                                              */
/* ---------------------------------------------------------------- */

/* the timer's channel 0, latched: it counts down at 1,193,182 a second
   whatever the processor and the bus are */
static u16 pit_count( void )
{
    u32 flags = int_off();
    u16 low, high;

    outb( 0x43, 0x00 );                 /* latch channel 0 */
    low = inb( 0x40 );
    high = inb( 0x40 );
    int_back( flags );
    return (u16)( ( high << 8 ) | low );
}

/* At least USEC microseconds.  In the square-wave mode the BIOS leaves
   channel 0 in, the count goes down by two a step, so five counts in two
   are taken as a microsecond - longer than that in the other mode, never
   shorter.  A count that goes up has been reloaded, and only what was
   left before the reload is counted.  The guard is for a timer that does
   not move: reading it is four bus cycles, so the guard is never what
   ends the wait on a machine whose timer runs. */
static void el_wait_us( u32 usec )
{
    u32 want = usec * 5 / 2, got = 0, guard = usec * 8 + 64;
    u16 last, now;

    last = pit_count();
    while ( got < want && guard ) {
        now = pit_count();
        got += ( now <= last ) ? (u32)( last - now ) : (u32)last;
        last = now;
        guard--;
    }
}

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
    say_text( "3C509 " );
    say_text( text );
}

static void say_hex( u32 val, int digits )
{
    static const char hexdig[] = "0123456789ABCDEF";
    int shift;

    for ( shift = ( digits - 1 ) * 4; shift >= 0; shift -= 4 ) {
        if ( say_len < (int)sizeof( say_buf ) - 3 ) say_buf[say_len++] = hexdig[( val >> shift ) & 15];
    }
}

static void say_dec( u32 val )
{
    char digits[12];
    int  n = 0;

    do { digits[n++] = (char)( '0' + val % 10 ); val /= 10; } while ( val && n < 11 );
    while ( n && say_len < (int)sizeof( say_buf ) - 3 ) say_buf[say_len++] = digits[--n];
}

static void say_end( void )
{
    say_buf[say_len++] = '\r';
    say_buf[say_len++] = '\n';
    say_buf[say_len] = 0;
    net_print( say_buf );
}

/* ---------------------------------------------------------------- */
/* the card's own registers                                          */
/* ---------------------------------------------------------------- */

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

/* a word of the EEPROM through window 0, at net_port: 162us to read,
   then the busy bit (window 0, port 0x0A bit 15) is watched for up to
   another two milliseconds.  A port with no card reads busy for ever,
   and FFFF after that. */
static u16 el_eeprom( int addr )
{
    int i;

    outw( net_port + W0_EECMD, (u16)( 0x80 | ( addr & 0x3F ) ) );
    el_wait_us( 200 );
    for ( i = 0; i < 40 && ( inw( net_port + W0_EECMD ) & 0x8000 ); i++ ) el_wait_us( 50 );
    return inw( net_port + W0_EEDATA );
}

/* read the station address from the EEPROM in window 0, at net_port */
static void el_read_mac( void )
{
    int i;
    u16 w;

    el_window( 0 );
    for ( i = 0; i < 3; i++ ) {
        w = el_eeprom( i );
        net_mac[i * 2] = (u8)( w >> 8 );
        net_mac[i * 2 + 1] = (u8)w;
    }
}

/* a line the card can be on, from the top four bits of a resource
   configuration word - the window-0 register's, or the EEPROM's */
static u8 el_line( u16 rescfg )
{
    u8 line = (u8)( rescfg >> 12 );

    switch ( line ) {
    case 3: case 5: case 7: case 9: case 10: case 11: case 12: case 15:
        return line;
    }
    return NIC_NOIRQ;
}

/* Diagnostic: the six bytes el_read_mac got, and the base.  A card that
   does not answer there reads back FF-FF-FF-FF-FF-FF (an open bus) or
   00-00-..., which tells that apart from an address that came out wrong.
   Remove once the card is proven. */
static void el_say_read( void )
{
    int i;

    say_begin( "read " );
    for ( i = 0; i < 6; i++ ) {
        if ( i ) say_text( "-" );
        say_hex( net_mac[i], 2 );
    }
    say_text( " at " );
    say_hex( net_port, 3 );
    say_text( "h" );
    say_end();
}

/* Diagnostic: the status register (command port, window-independent) at a
   labelled point in setup.  FFFF means the card has stopped decoding its
   base - gone dark.  Remove once the card is proven. */
static void el_say_reg( const char *label )
{
    say_begin( label );
    say_text( "=" );
    say_hex( inw( net_port + EL_STATUS ), 4 );
    say_end();
}

/* Diagnostic: a 32-bit value (the window-3 InternalConfig, to see which
   transceiver the card is wired for).  Remove once the card is proven. */
static void el_say_hex32( const char *label, u32 val )
{
    say_begin( label );
    say_text( "=" );
    say_hex( val, 8 );
    say_end();
}

/* ---------------------------------------------------------------- */
/* the ID port                                                       */
/* ---------------------------------------------------------------- */

/* Where the ID sequence is tried, 110h first as every 3Com driver does.
   The card listens at whichever of 100h-1F0h the sequence went to; the
   rest are for a machine where something else is at 110h.  170h and 1F0h
   are left out: they are the IDE data ports, and 255 bytes written to a
   drive's data port are not harmless. */
static const u16 id_ports[] = {
    0x110, 0x120, 0x130, 0x140, 0x150, 0x160, 0x180,
    0x190, 0x1A0, 0x1B0, 0x1C0, 0x1D0, 0x1E0, 0
};

/* the ID sequence: a walking pattern the cards' logic recognises */
static void id_sequence( void )
{
    u8 al = 0xFF;
    int i;

    outb( id_port, 0 );
    outb( id_port, 0 );
    for ( i = 0; i < 255; i++ ) {
        outb( id_port, al );
        al = (u8)( ( al << 1 ) ^ ( ( al & 0x80 ) ? 0xCF : 0x00 ) );
    }
}

/* Read a word of a card's EEPROM through the ID port, a bit at a time,
   bit 0 of a BYTE read each: a word read would be two reads of the port
   on a bus that splits it.  The card wants 162us; Linux waits 4ms, its
   note saying some cards want much longer, and so does this. */
static u16 id_eeprom( int addr )
{
    u16 val = 0;
    int i;

    outb( id_port, (u8)( 0x80 | ( addr & 0x3F ) ) );
    el_wait_us( 4000 );
    for ( i = 0; i < 16; i++ ) {
        val = (u16)( ( val << 1 ) | ( inb( id_port ) & 1 ) );
    }
    return val;
}

/* Bring one card to the base its EEPROM names and activate it there.
   Reading the station address through the ID port is the contention:
   with two cards, the one with the lower address is the one left in the
   ID state, and the one activated.  It is then given tag 1, which keeps
   it out of a later sequence until the tags are cleared again (D0h, as
   here, so that card_setup can bring it back after its reset). */
static int id_activate( void )
{
    u16 w, field;
    int i, k;

    for ( k = 0; id_ports[k]; k++ ) {
        id_port = id_ports[k];
        id_sequence();
        outb( id_port, 0xD0 );                  /* every card's tag to 0 */
        w = id_eeprom( 7 );                     /* 3Com's maker word */
        if ( k == 0 ) rep_id_word = w;
        if ( w == EL_MAKER ) break;
    }
    if ( !id_ports[k] ) return 0;
    rep_id_port = id_port;

    for ( i = 0; i < 3; i++ ) {                 /* big-endian, words 0-2 */
        w = id_eeprom( i );
        net_mac[i * 2] = (u8)( w >> 8 );
        net_mac[i * 2 + 1] = (u8)w;
    }
    /* the base is word 8's low five bits: 200h + 10h each.  1Fh means
       EISA, and 3F0h is the floppy controller's, so such a card is put at
       300h instead.  The line is word 9's top four bits. */
    field = (u16)( id_eeprom( 8 ) & 0x1F );
    if ( field == 0x1F ) field = 0x10;
    el_card_irq = el_line( id_eeprom( 9 ) );
    net_port = (u16)( 0x200 + ( field << 4 ) );

    outb( id_port, 0xD1 );                      /* tag 1 */
    outb( id_port, (u8)( 0xE0 | field ) );      /* activate at the base */

    el_window( 0 );
    w = inw( net_port + W0_MAKER );
    rep_id_base = net_port;
    rep_id_read = w;
    if ( w != EL_MAKER ) return 0;
    el_how = HOW_ID;
    return 1;
}

/* ---------------------------------------------------------------- */
/* a card that is on already                                         */
/* ---------------------------------------------------------------- */

/* The bases an EtherLink III can be at, read for the maker word in
   window 0; the status register's top three bits must say window 0 as
   well.  The maker word is read first, at the base and base+1, so the
   status (base+0Eh and base+0Fh) is read only where a 3C509 is - an
   NE2000 at 300h has its reset port at 31Fh, and a read of it resets the
   card. */
static int find_seen( void )
{
    u16 base;

    for ( base = 0x200; base <= 0x3E0; base += 0x10 ) {
        if ( inw( base + W0_MAKER ) != EL_MAKER ) continue;
        if ( ( inw( base + EL_STATUS ) >> 13 ) != 0 ) continue;
        net_port = base;
        el_read_mac();
        if ( !net_mac_ok( net_mac ) ) continue;
        el_card_irq = el_line( inw( net_port + W0_RESCFG ) );
        el_how = HOW_SEEN;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* ISA Plug and Play                                                 */
/* ---------------------------------------------------------------- */

/* A 3C509B set to Plug and Play is numbered by the kernel at boot; here
   the 3Com one is matched by its Plug and Play id, given an I/O base and
   a line, and activated through the kernel's services.  After that it is
   an ordinary 3C509 at that base and the rest of the driver drives it
   unchanged.  The id's vendor bytes are 'TCM' (0x50 0x6D), the same 3Com
   maker word the legacy EEPROM check reads.  Linux's 3c509.c takes the
   five TCM ids and PNP80F7/PNP80F8, "3Com Etherlink III compatible",
   which a card may give as its device's compatible id. */
static const u8 pnp_3com[][4] = {
    { 0x50, 0x6D, 0x50, 0x90 },     /* TCM5090  3C509B (TP)    */
    { 0x50, 0x6D, 0x50, 0x91 },     /* TCM5091  3C509B         */
    { 0x50, 0x6D, 0x50, 0x92 },     /* TCM5092                 */
    { 0x50, 0x6D, 0x50, 0x93 },     /* TCM5093                 */
    { 0x50, 0x6D, 0x50, 0x94 },     /* TCM5094  3C509B (combo) */
    { 0x50, 0x6D, 0x50, 0x95 },     /* TCM5095  3C509B (TPO)   */
    { 0x50, 0x6D, 0x50, 0x98 },     /* TCM5098  3C509B (TPC)   */
    { 0x41, 0xD0, 0x80, 0xF7 },     /* PNP80F7  compatible     */
    { 0x41, 0xD0, 0x80, 0xF8 }      /* PNP80F8  compatible     */
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

/* an EISA id as its seven letters and digits: TCM5095 */
static void say_eisa( const u8 *id )
{
    char text[8];

    text[0] = (char)( '@' + ( ( id[0] >> 2 ) & 31 ) );
    text[1] = (char)( '@' + ( ( ( id[0] & 3 ) << 3 ) | ( id[1] >> 5 ) ) );
    text[2] = (char)( '@' + ( id[1] & 31 ) );
    text[3] = 0;
    say_text( text );
    say_hex( id[2], 2 );
    say_hex( id[3], 2 );
}

/* Decoding off, set the port and the line, decoding on - a device is
   moved with its decoding off, as the specification says.  THIS IS DONE
   EVEN FOR A CARD WHOSE REGISTERS ALREADY READ ON: the kernel re-isolates
   the Plug and Play cards at boot, after which a 3C509B reads its old
   base back but does not decode it again until its activate register is
   written (Charlie's 486, 2026-10-22: FF-FF-FF-FF-FF-FF at 300h before,
   the card's address after).  Writing it is what ISAPNP's /A does. */
static int pnp_activate( u32 csn, u16 base, u8 line )
{
    REGS r;
    u8   pairs[12];

    pairs[0] = 0x30;  pairs[1] = 0;
    pairs[2] = 0x60;  pairs[3] = (u8)( base >> 8 );
    pairs[4] = 0x61;  pairs[5] = (u8)base;
    pairs[6] = 0x70;  pairs[7] = line;
    pairs[8] = 0x71;  pairs[9] = 2;             /* edge, high */
    pairs[10] = 0x30; pairs[11] = 1;
    net_memzero( &r, sizeof( r ) );
    r.eax = (u32)BUS_PNPSET << 8;
    r.edx = csn;                                /* DH = device 0 */
    r.esi = (u32)pairs;
    r.ecx = 6;
    return nds_bus( &r );
}

static int find_pnp( int first )
{
    REGS r;
    u8  *res, *regs, tag;
    u32  cards, csn, len, pos, size, kind;
    u32  io_min, io_max, io_align, irq_mask;
    int  ours, devices, i, was_on;
    u8   line;
    u16  choice;

    net_memzero( &r, sizeof( r ) );
    r.eax = (u32)BUS_PNPHERE << 8;
    if ( !nds_bus( &r ) || r.eax == 0 ) return 0;
    cards = r.eax;
    rep_pnp_cards = cards;
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
        if ( len > 2048 ) len = 2048;

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
        if ( !ours ) {
            if ( !rep_id_kept ) {               /* the first other card's id */
                net_memcpy( rep_pnp_id, res, 4 );
                rep_id_kept = 1;
            }
            continue;
        }
        rep_pnp_3com++;

        /* Where it should live.  A card CTCM or a Plug and Play BIOS has
           configured keeps its base and line in its registers (0x30 reads
           on).  One that is off is put back where this driver had it, when
           this is the card coming back after a park; otherwise it is given
           0x300 if its range allows, and the first free line it will take.
           /I: is the line whatever the card had. */
        net_memzero( &r, sizeof( r ) );
        r.eax = (u32)BUS_PNPREGS << 8;
        r.edx = csn;                            /* DH = device 0 */
        r.edi = (u32)regs;
        was_on = nds_bus( &r ) && ( regs[0x30] & 1 ) && ( regs[0x60] | regs[0x61] );
        if ( was_on ) {
            choice = (u16)( ( (u16)regs[0x60] << 8 ) | regs[0x61] );
            line = (u8)( regs[0x70] & 15 );
        } else if ( el_how == HOW_PNP && csn == pnp_csn ) {
            choice = net_port;
            line = pnp_line;
        } else {
            if ( io_align == 0 ) continue;
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
        if ( net_opt_irq != NIC_NOIRQ ) line = net_opt_irq;

        if ( first ) {                          /* diagnostic, see el_say_read */
            say_begin( "Plug and Play card " );
            say_dec( csn );
            say_text( " " );
            say_eisa( res );
            if ( was_on ) {
                say_text( ": on at " );
                say_hex( choice, 3 );
                say_text( "h IRQ " );
                say_dec( line );
            } else {
                say_text( ": off" );
            }
            say_end();
        }

        if ( !pnp_activate( csn, choice, line ) ) continue;
        net_port = choice;
        el_read_mac();
        if ( first ) el_say_read();
        if ( !net_mac_ok( net_mac ) ) continue;
        pnp_csn = (u8)csn;
        pnp_line = line;
        el_card_irq = line ? line : NIC_NOIRQ;
        el_how = HOW_PNP;
        nds_free( res );
        return 1;
    }
    nds_free( res );
    return 0;
}

/* ---------------------------------------------------------------- */
/* card_find                                                         */
/* ---------------------------------------------------------------- */

/* the lines said when no way found a card: one a way */
static void el_report( void )
{
    say_begin( "Plug and Play: " );
    if ( rep_pnp_cards == 0 ) {
        say_text( "no cards" );
    } else if ( rep_pnp_3com == 0 ) {
        say_dec( rep_pnp_cards );
        say_text( rep_pnp_cards == 1 ? " card, " : " cards, " );
        say_eisa( rep_pnp_id );
        say_text( rep_pnp_cards == 1 ? ", not a 3C509B" : " and others, no 3C509B" );
        /* the 3C515 answers to 3Com's name and is nothing like a 3C509
           to drive: Charlie's 5x86, 2026-11-01 */
        if ( rep_pnp_id[0] == 0x50 && rep_pnp_id[1] == 0x6D && rep_pnp_id[2] == 0x50 && rep_pnp_id[3] == 0x51 ) {
            say_end();
            say_begin( "TCM5051 is a 3C515: use EL3C515.SYS" );
        }
    } else {
        say_text( "no station address read from the 3C509B" );
    }
    say_end();

    say_begin( "ports 200h-3E0h: no card in window 0" );
    say_end();

    if ( rep_id_port ) {
        say_begin( "ID port " );
        say_hex( rep_id_port, 3 );
        say_text( "h: card put at " );
        say_hex( rep_id_base, 3 );
        say_text( "h, which read " );
        say_hex( rep_id_read, 4 );
    } else {
        say_begin( "ID ports 110h-1E0h: no answer, 110h read " );
        say_hex( rep_id_word, 4 );
    }
    say_end();
}

int card_find( void )
{
    int first = !el_called;

    el_called = 1;
    el_card_irq = NIC_NOIRQ;
    net_strcpy( net_name, "3Com EtherLink III (3C509)" );
    if ( net_opt_port ) {
        /* a card already at a known base (/P:) - read its address */
        net_port = (u16)net_opt_port;
        el_read_mac();
        if ( first ) el_say_read();             /* diagnostic, see above */
        if ( !net_mac_ok( net_mac ) ) return 1;
        el_card_irq = el_line( inw( net_port + W0_RESCFG ) );
        el_how = HOW_PORT;
    } else if ( find_pnp( first ) ) {
        /* an ISA Plug and Play 3C509B: given a base and a line and
           switched on by find_pnp; its address is already read */
        ;
    } else if ( find_seen() ) {
        /* a card that was on at a base already */
        ;
    } else if ( id_activate() ) {
        /* a legacy (non-PnP) 3C509: the ID sequence set the base + address */
        ;
    } else {
        if ( first ) el_report();
        return 1;
    }
    if ( !net_mac_ok( net_mac ) ) {
        if ( first ) el_say_read();
        return 1;
    }

    /* the line the card has, at the DEVICE= load; after a park the line
       is the one nd_init settled on and hooked, and stays */
    if ( first && net_irq == NIC_NOIRQ ) net_irq = el_card_irq;
    net_bus = NICB_ISA;
    net_flags = (u8)( NICF_WORD | ( net_flags & NICF_POLLED ) );
    (void)el3_ids;
    return 0;
}

/* After the GlobalReset: a card Plug and Play switched on is switched on
   again where it was (the reset drops its decode - on the 486, the status
   read FFFF after the reset and 0000 after this).  Any other card that
   has gone dark is put back through the ID port, which a Plug and Play
   3C509B answers too. */
static void el_wake( void )
{
    if ( el_how == HOW_PNP ) {
        (void)pnp_activate( pnp_csn, net_port, pnp_line );
        return;
    }
    if ( inw( net_port + EL_STATUS ) != 0xFFFF ) return;
    (void)id_activate();
}

/* ---------------------------------------------------------------- */
/* running it                                                        */
/* ---------------------------------------------------------------- */

/* The transmit status stack: a frame that failed - jabber, underrun, too
   many collisions - leaves a byte here and the transmitter stopped until
   the byte is popped and the transmitter enabled again; Linux's 3c509.c
   pops up to four before each frame.  0 is an empty stack, FF a card that
   has gone dark. */
static void tx_status_pop( void )
{
    u8  ts;
    int i;

    for ( i = 0; i < 4; i++ ) {
        ts = inb( net_port + W1_TXSTATUS );
        if ( ts == 0 || ts == 0xFF ) break;
        if ( ts & 0x38 ) net_stats.txerrors++;
        if ( ts & 0x30 ) el_cmd( CMD_TXRESET );
        if ( ts & 0x3C ) el_cmd( CMD_TXENABLE );
        outb( net_port + W1_TXSTATUS, 0 );      /* pop it */
    }
}

int card_setup( void )
{
    int i, said;
    u32 media_cfg, xcvr;

    said = !el_started;                     /* the diagnostics: the first
                                               start only, not each wake
                                               under Windows */
    el_started = 1;
    if ( said ) el_say_reg( "entry" );

    /* GlobalReset loads the card's internal configuration - the transceiver
       and the receive datapath - from the EEPROM.  WITHOUT it the receiver
       takes nothing in (window 3's InternalConfig reads FFFF).  The reset
       drops the Plug and Play I/O decode, so the card is switched on again
       straight after (el_wake); the command-wait guard keeps the card, dark
       for that moment, from pinning the processor in between.  The reset
       also clears the status mask, which is set again below. */
    outw( net_port + EL_CMD, CMD_RESET );
    el_wait_us( 5000 );
    if ( said ) el_say_reg( "post-reset" );
    el_wake();
    if ( said ) el_say_reg( "post-react" );

    /* the adapter enabled and told its line, as Linux's el3_up does for
       every 3C509 ("required and does no harm otherwise") */
    el_window( 0 );
    outw( net_port + W0_CFGCTRL, 0x0001 );
    if ( net_irq != NIC_NOIRQ ) outw( net_port + W0_RESCFG, (u16)( ( (u16)net_irq << 12 ) | 0x0F00 ) );

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
    if ( said ) el_say_hex32( "cfg", media_cfg );
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
    /* EVERY STATUS BIT SEEN.  The mask of the status bits that may be seen
       is all zeroes after a reset, and a bit that is masked reads as zero
       and interrupts nobody: RxComplete never showed, so card_service never
       took a frame and the line never fired - the 486's rx=0 irq=0 with
       frames going out, and its status 0000 after the reset.  Linux sets
       all eight. */
    el_cmd( (u16)( CMD_SETZERO | 0xFF ) );
    if ( said ) el_say_reg( "post-enable" );    /* still answering after setup? */
    /* acknowledge everything, then let RX-complete and TX-complete
       interrupt unless polled.  NOT TX-available: the TxAvailable status
       is a level that is set whenever the transmit FIFO has room, which at
       idle is always, so to enable it is to take an interrupt that
       re-asserts the instant it is acknowledged - a storm that pins the
       processor and starves everything else.  The transmit path polls the
       free-byte count (card_send) and wants no interrupt of its own;
       TX-complete is set only by a frame that failed, and card_service
       pops the status that set it. */
    el_cmd( (u16)( CMD_ACKINTR | 0xFF ) );
    if ( net_flags & NICF_POLLED ) el_cmd( CMD_SETINTR );
    else el_cmd( (u16)( CMD_SETINTR | ST_RXCOMP | ST_TXCOMP | ST_INTLATCH ) );
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

    if ( len > NIC_MAXFRAME ) { net_stats.txerrors++; return 0; }
    if ( use < 60 ) {
        net_memzero( pad, sizeof( pad ) );
        net_memcpy( pad, frame, len );
        frame = pad;
        use = 60;
    }
    tx_status_pop();                             /* a stopped transmitter
                                                    would never make room */
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
    u16 status;
    u32 len, words, n, wait;
    u8 *slot;

    for ( n = 0; n < 16; n++ ) {
        status = inw( net_port + W1_RXSTATUS );
        if ( status & RXS_INCOMPLETE ) break;    /* still arriving, or none */
        if ( ( status & RXS_LEN ) == 0 && !( status & RXS_ERROR ) ) break;  /* FIFO empty */
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
        /* A DISCARD THAT NEVER FINISHED.  The receiver is flushed instead
           (RxReset clears the filter, so it is set again and the receiver
           enabled) and this pass given up.  Two frames of one length in a
           row have the same status word, so a status seen twice is NOT a
           sign of a stuck FIFO - that test dropped good frames. */
        if ( wait == SPIN_CMD ) {
            el_cmd( CMD_RXRESET );
            el_cmd( (u16)( CMD_SETFILTER | FILTER_STATION | FILTER_MULTI | FILTER_BCAST ) );
            el_cmd( CMD_RXENABLE );
            net_stats.rxerrors++;
            break;
        }
    }
}

int card_service( void )
{
    u16 st;
    u32 n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        st = inw( net_port + EL_STATUS );
        if ( st == 0xFFFF ) break;               /* a dark card is nobody's */
        /* the interrupt latch, a complete frame and a failed transmit are
           what is ours to act on.  TxAvailable is NOT tested: its status is
           set at idle whether or not an interrupt fired, so reading it as
           "mine" would claim every interrupt, this card's or another's on a
           shared line. */
        if ( !( st & ( ST_RXCOMP | ST_TXCOMP | ST_INTLATCH ) ) ) break;
        mine = 1;
        if ( st & ST_RXCOMP ) rx_drain();
        if ( st & ST_TXCOMP ) tx_status_pop();
        el_cmd( (u16)( CMD_ACKINTR | ( st & 0xFF ) ) );
    }
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { '3', 'C', '5', '0', '9', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
