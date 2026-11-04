/*
 * EL3C515.C - EL3C515.SYS, a driver for the 3Com Fast EtherLink ISA (3C515)
 *
 * The ISA 10/100 card in Charlie's Cyrix 5x86, which reports itself to
 * Plug and Play as TCM5051, "3Com Fast EtherLink ISA".  3Com called the
 * chip Corkscrew: a 3C59x/3C90x (Vortex/Boomerang) put on the ISA bus.
 * It is NOT a 3C509 with a faster wire - EL3C509.SYS read rubbish from
 * it - and no emulator has one, so this is written to Linux's 3c515.c
 * and is for the real card to prove.
 *
 * WHAT IS THE SAME AS THE 3C509: the command and status register at
 * base+0Eh in every window, the commands themselves, the banked windows
 * for setup (2: the station address, 3: the configuration, 4: the
 * media), the transmit status stack, the EEPROM's words (0-2 the
 * station address, 7 the maker's 6D50h).
 *
 * WHAT IS NOT:
 *   - The operating registers are at base+10h to 1Fh in EVERY window,
 *     not in window 1 at base+00h: the FIFO is base+10h, the receive
 *     status base+18h, the transmit status base+1Bh, the free count
 *     base+1Ch.  The card takes 20h ports, not 10h.
 *   - The EEPROM and two configuration registers are 2000h above the
 *     base, outside the windows: the command at base+200Ah, the data at
 *     base+200Ch, the resource configuration (bits 8-4 the base, 3-0
 *     the line) at base+2002h.
 *   - The FIFO is read and written in doublewords, as Linux does; the
 *     ISA bus makes each one two word cycles.
 *   - Two speeds and up to six transceivers, chosen in window 3's
 *     InternalConfig and window 4's media register (see el_media).
 *
 * The card can move frames by ISA bus-master DMA - the DMA channel
 * ISAPNP shows for it is that one.  This driver does not: frames go
 * through the FIFO by the processor, as on the 3C509, so the channel
 * is never used and nothing below 16MB has to be set aside for it.
 *
 * FINDING IT, in this order:
 *
 *   /P:base   a card already at a known port.  Nothing else is tried.
 *   PLUG AND PLAY.  The kernel numbers the cards at boot (IO\ISAPNP.INC);
 *             here the 3C515 is matched by its id, given a port and a
 *             line, and switched on.
 *   A CARD THAT IS ON ALREADY, at one of the bases 100h to 3E0h - a
 *             3C515 with Plug and Play switched off in its EEPROM.  As
 *             Linux does, the resource configuration register has to
 *             name the base before anything is written to the card.
 *
 * At the load the card says one line, NETCORE's: the name with the
 * speed, the port, the line and the address.
 *
 * ITS OWN SWITCHES, beside NETCORE's /P:, /I: and /POLL:
 *
 *   /10, /100 the speed, 10BASE-T or 100BASE-TX, whatever the EEPROM says,
 *             on a card with that transceiver; one with MII only (the
 *             5x86's) has its PHY agree the speed
 *   /T        a test at the load (el_test): the card's own counts of what
 *             it sent and heard
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).
 */
#include "netdrv.h"

/* the command/status register, base+0x0E, in every window */
#define EL_CMD      0x0E
#define EL_STATUS   0x0E

/* commands: the top five bits of the word written to EL_CMD */
#define CMD_SELWIN      0x0800  /* | window number */
#define CMD_STARTCOAX   0x1000
#define CMD_RXDISABLE   0x1800
#define CMD_RXENABLE    0x2000
#define CMD_RXRESET     0x2800
#define CMD_RXDISCARD   0x4000  /* drop the frame at the top of the RX FIFO */
#define CMD_TXENABLE    0x4800
#define CMD_TXDISABLE   0x5000
#define CMD_TXRESET     0x5800
#define CMD_ACKINTR     0x6800  /* | the bits to acknowledge */
#define CMD_SETINTR     0x7000  /* | the interrupt mask */
#define CMD_SETSTATUS   0x7800  /* | the status bits that may be seen */
#define CMD_SETFILTER   0x8000  /* | the receive filter */
#define CMD_STATSEN     0xA800
#define CMD_STATSDIS    0xB000
#define CMD_STOPCOAX    0xB800

/* status bits (EL_STATUS) */
#define ST_INTLATCH 0x0001
#define ST_ADFAIL   0x0002      /* adapter failure: the receiver is reset */
#define ST_TXCOMP   0x0004
#define ST_TXAVAIL  0x0008
#define ST_RXCOMP   0x0010
#define ST_INTREQ   0x0040
#define ST_CMDBUSY  0x1000

/* the operating registers: base+10h-1Fh whatever the window */
#define OP_FIFO     0x10        /* doubleword: frames in and out */
#define OP_RXERRORS 0x14        /* byte: why the frame at the top is bad */
#define OP_RXSTATUS 0x18        /* word: the frame at the top of the FIFO */
#define OP_TXSTATUS 0x1B        /* byte: the transmit status stack */
#define OP_TXFREE   0x1C        /* word: bytes free in the transmit FIFO */

#define RXS_INCOMPLETE 0x8000   /* the frame is still arriving */
#define RXS_ERROR      0x4000
#define RXS_LEN        0x1FFF

/* outside the windows, 2000h above the base */
#define CS_RESCFG   0x2002      /* bits 8-4: the base, 3-0: the line */
#define CS_EECMD    0x200A      /* EEPROM command; bit 9 = busy */
#define CS_EEDATA   0x200C

/* window 3: the configuration */
#define W3_CONFIG   0x00        /* doubleword InternalConfig */
#define W3_MACCTRL  0x06        /* word: full duplex and the like */
#define W3_OPTIONS  0x08        /* word: the transceivers fitted */
#define CFG_XCVR    0x00700000L /* bits 22-20: the transceiver in use */
#define CFG_AUTO    0x01000000L /* the EEPROM says choose one */
#define MAC_FULLDUP 0x0020      /* W3_MACCTRL: full duplex */

/* W3_OPTIONS: the transceivers fitted (Linux's media_tbl masks) */
#define OPT_100TX   0x02
#define OPT_100FX   0x04
#define OPT_10T     0x08
#define OPT_10B2    0x10
#define OPT_AUI     0x20
#define OPT_MII     0x40

/* window 4: the media */
#define W4_FIFODIAG 0x04
#define W4_NETDIAG  0x06
#define W4_PHYMGMT  0x08        /* the MII management lines, by hand */
#define W4_MEDIA    0x0A

/* W4_PHYMGMT bits, as Linux's 3c59x.c drives them */
#define MDIO_CLK    0x01
#define MDIO_DATA   0x02        /* the data line, in or out */
#define MDIO_OUT    0x04        /* this end drives the data line */

/* the PHY's registers */
#define MII_BMCR    0
#define MII_BMSR    1           /* bit 2 link, bit 5 negotiation done */
#define MII_ID1     2
#define MII_ID2     3
#define MII_ANAR    4           /* what this end offers */
#define MII_ANLPAR  5           /* what the other end offers */
#define MED_SQE     0x0008      /* AUI */
#define MED_10TP    0x00C0      /* link beat and jabber, 10BASE-T */
#define MED_LNK     0x0080      /* link beat, 100BASE-TX/FX */
#define MED_LNKBEAT 0x0800      /* there is a link */

/* the transceivers: InternalConfig's number, and the W3_OPTIONS bit
   that says the card has one */
#define XCVR_10T    0
#define XCVR_AUI    1
#define XCVR_10B2   3
#define XCVR_100TX  4
#define XCVR_100FX  5
#define XCVR_MII    6

#define FILTER_STATION 0x01
#define FILTER_MULTI   0x02     /* receive all multicast frames */
#define FILTER_BCAST   0x04

#define EL_MAKER    0x6D50

#define SPIN_CMD    100000L
#define SPIN_TX     400000L

/* how the card was found, which is how it is found again after a park */
#define HOW_PORT    1           /* /P: */
#define HOW_PNP     2
#define HOW_SEEN    3           /* on already, at a base the scan read */

static u8  el_how;              /* HOW_* */
static u8  el_called;           /* card_find has run once: the DEVICE= load */
static u8  el_started;          /* card_setup has run once */
static u8  el_card_irq;         /* the line the card says it has */
static u8  el_xcvr = 0xFF;      /* the transceiver chosen at the first start */
static u8  el_fdx;              /* MII: the PHY agreed full duplex */
static const char *el_speed;    /* MII: what the PHY agreed, for the name */
static u8  pnp_csn;             /* HOW_PNP: the card's number */
static u8  pnp_line;            /* ...and the line it was given */

/* the rest of a frame that does not end on a doubleword */
static u8  tail[64];

static const PCI_ID el_ids[] = { { 0, 0 } };    /* ISA */

void insd( u16 port, void *buf, u32 dwords );
#pragma aux insd = "rep insd" parm [dx] [edi] [ecx] modify exact [edi ecx];
void outsd( u16 port, const void *buf, u32 dwords );
#pragma aux outsd = "rep outsd" parm [dx] [esi] [ecx] modify exact [esi ecx];

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

/* At least USEC microseconds, by the timer and not by a loop count:
   the same as EL3C509.C, which says why. */
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
    say_text( "3C515 " );
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
        if ( st == 0xFFFF ) break;          /* nothing answering: do not spin */
        if ( !( st & ST_CMDBUSY ) ) break;
    }
}

static void el_window( int win )
{
    outw( net_port + EL_CMD, (u16)( CMD_SELWIN | win ) );
}

/* a word of the EEPROM at BASE: 162us to read, then the busy bit is
   watched for up to another millisecond */
static u16 el_eeprom( u16 base, int addr )
{
    int i;

    outw( base + CS_EECMD, (u16)( 0x80 | ( addr & 0x3F ) ) );
    el_wait_us( 200 );
    for ( i = 0; i < 20 && ( inw( base + CS_EECMD ) & 0x0200 ); i++ ) el_wait_us( 50 );
    return inw( base + CS_EEDATA );
}

/* Is there a 3C515 at BASE?  Its resource configuration register names
   the base before anything is written, as Linux's check_device has it:
   a port with nothing on it reads FFFF, whose base bits never match a
   base 20h apart, so the EEPROM command is written only to a card that
   is one. */
static int el_names( u16 base )
{
    u16 rescfg = inw( base + CS_RESCFG );

    /* 0000 is no more a card than FFFF is, though its base bits would
       match 200h */
    if ( rescfg == 0x0000 || rescfg == 0xFFFF ) return 0;
    return ( rescfg & 0x1F0 ) == ( base & 0x1F0 );
}

static int el_check( u16 base )
{
    if ( !el_names( base ) ) return 0;
    return el_eeprom( base, 7 ) == EL_MAKER;
}

/* read the station address from the EEPROM, at net_port */
static void el_read_mac( void )
{
    int i;
    u16 w;

    for ( i = 0; i < 3; i++ ) {
        w = el_eeprom( net_port, i );
        net_mac[i * 2] = (u8)( w >> 8 );
        net_mac[i * 2 + 1] = (u8)w;
    }
}

/* the line in the resource configuration register, if it is one an ISA
   card can have */
static u8 el_line( void )
{
    u8 line = (u8)( inw( net_port + CS_RESCFG ) & 15 );

    switch ( line ) {
    case 3: case 5: case 7: case 9: case 10: case 11: case 12: case 15:
        return line;
    }
    return NIC_NOIRQ;
}

/* ---------------------------------------------------------------- */
/* ISA Plug and Play                                                 */
/* ---------------------------------------------------------------- */

/* TCM5051, the one id Linux's 3c515.c takes */
static const u8 pnp_3c515[4] = { 0x50, 0x6D, 0x50, 0x51 };
static const u8 pnp_irq_pref[] = { 10, 11, 5, 9, 3, 15, 12, 4, 0 };

static int pnp_id_ours( const u8 *id )
{
    return id[0] == pnp_3c515[0] && id[1] == pnp_3c515[1] &&
           id[2] == pnp_3c515[2] && id[3] == pnp_3c515[3];
}

/* Decoding off, set the port and the line, decoding on - done even for
   a card whose registers already read on, because the kernel's
   isolation at boot leaves a card not decoding until its activate
   register is written (EL3C509.C, Charlie's 486). */
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

static int find_pnp( void )
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

        /* the card's own id or its first logical device's; that device's
           first port range and first line mask */
        ours = pnp_id_ours( res );
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
                if ( size >= 4 && pnp_id_ours( res + pos ) ) ours = 1;
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

        /* Where it should live: where the BIOS put it, if it did; where
           this driver had it, if this is the card coming back after a
           park; otherwise 300h if its range allows, and the first free
           line it will take.  /I: is the line whatever the card had. */
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

        if ( !pnp_activate( csn, choice, line ) ) continue;
        net_port = choice;
        /* THE FIRST EEPROM READ AFTER THE CARD IS SWITCHED ON CAME BACK
           STALE on the 5x86 - 6D50, the maker's word, where the address
           starts 0060 - and the address was refused as a multicast; the
           port scan found the card instead (2026-11-01).  One read is
           thrown away, and the maker's word checked as Linux's
           check_device does, before the address is read. */
        (void)el_eeprom( choice, 7 );
        if ( !el_check( choice ) ) continue;
        el_read_mac();
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

/* a 3C515 switched on by its own EEPROM, at a base 20h apart */
static int find_seen( void )
{
    u16 base;

    for ( base = 0x100; base <= 0x3E0; base += 0x20 ) {
        if ( !el_check( base ) ) continue;
        net_port = base;
        el_read_mac();
        if ( !net_mac_ok( net_mac ) ) continue;
        el_card_irq = el_line();
        el_how = HOW_SEEN;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* card_find                                                         */
/* ---------------------------------------------------------------- */

int card_find( void )
{
    int first = !el_called;

    el_called = 1;
    el_card_irq = NIC_NOIRQ;
    net_strcpy( net_name, "3Com 3C515" );
    if ( net_opt_port ) {
        /* a card already at a known base (/P:) */
        net_port = (u16)net_opt_port;
        if ( !el_check( net_port ) ) return 1;
        el_read_mac();
        el_card_irq = el_line();
        el_how = HOW_PORT;
    } else if ( find_pnp() ) {
        /* a Plug and Play 3C515: given a base and a line and switched on
           by find_pnp; its address is already read */
        ;
    } else if ( find_seen() ) {
        /* a card that was on at a base already */
        ;
    } else {
        return 1;
    }
    if ( !net_mac_ok( net_mac ) ) return 1;

    /* the line the card has, at the DEVICE= load; after a park the line
       is the one nd_init settled on and hooked, and stays */
    if ( first && net_irq == NIC_NOIRQ ) net_irq = el_card_irq;
    net_bus = NICB_ISA;
    net_flags = (u8)( NICF_WORD | ( net_flags & NICF_POLLED ) );
    (void)el_ids;
    return 0;
}

/* ---------------------------------------------------------------- */
/* this driver's own switches                                        */
/* ---------------------------------------------------------------- */

/* Is /NAME on the DEVICE= line?  Only at the load, while NETCORE has the
   line.  A letter or digit straight after NAME makes it another switch:
   /10 is not /100. */
static int el_switch( const char *name )
{
    const char *p = net_line;
    char ch;
    int  i;

    if ( p == 0 ) return 0;
    while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' ) p++;
    for ( ; *p && *p != '\r' && *p != '\n'; p++ ) {
        if ( *p != '/' ) continue;
        for ( i = 0; name[i] && ( p[1 + i] | 0x20 ) == ( name[i] | 0x20 ); i++ ) ;
        if ( name[i] ) continue;
        ch = (char)( p[1 + i] | 0x20 );
        if ( ( ch >= 'a' && ch <= 'z' ) || ( ch >= '0' && ch <= '9' ) ) continue;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* the transceiver                                                   */
/* ---------------------------------------------------------------- */

/* Put the card on transceiver XCVR: its number into window 3's
   InternalConfig, its link-beat bits into window 4's media register,
   the coax converter started or stopped.  Leaves window 4 selected. */
static void el_media( u8 xcvr )
{
    u32 config;
    u16 media;

    el_window( 3 );
    config = inl( net_port + W3_CONFIG );
    config = ( config & ~CFG_XCVR ) | ( (u32)xcvr << 20 );
    outl( net_port + W3_CONFIG, config );
    /* full duplex only where an MII PHY agreed it with the other end */
    media = inw( net_port + W3_MACCTRL );
    if ( xcvr == XCVR_MII && el_fdx ) media |= MAC_FULLDUP;
    else media &= (u16)~MAC_FULLDUP;
    outw( net_port + W3_MACCTRL, media );
    el_cmd( xcvr == XCVR_10B2 ? CMD_STARTCOAX : CMD_STOPCOAX );
    el_window( 4 );
    media = (u16)( inw( net_port + W4_MEDIA ) & ~( MED_10TP | MED_SQE ) );
    if ( xcvr == XCVR_10T ) media |= MED_10TP;
    else if ( xcvr == XCVR_AUI ) media |= MED_SQE;
    else if ( xcvr == XCVR_100TX || xcvr == XCVR_100FX ) media |= MED_LNK;
    outw( net_port + W4_MEDIA, media );
}

/* up to TENTHS tenths of a second for a link on the transceiver just
   chosen */
static int el_link( int tenths )
{
    int i;

    el_window( 4 );
    for ( i = 0; i < tenths * 10; i++ ) {
        if ( inw( net_port + W4_MEDIA ) & MED_LNKBEAT ) return 1;
        el_wait_us( 10000 );
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* an MII PHY                                                        */
/* ---------------------------------------------------------------- */

/* The management lines, clocked by hand through W4_PHYMGMT, as Linux's
   3c59x.c does for the same family (mdio_sync, mdio_read).  Each port
   cycle on the ISA bus is near a microsecond, far slower than the
   2.5MHz the MII allows, so the reads between are delay enough.
   Window 4 must be selected. */
static void mdio_bit( u16 out )
{
    outw( net_port + W4_PHYMGMT, out );
    (void)inw( net_port + W4_PHYMGMT );
    outw( net_port + W4_PHYMGMT, (u16)( out | MDIO_CLK ) );
    (void)inw( net_port + W4_PHYMGMT );
}

/* register REG of the PHY at ADDR; FFFF when nothing answers */
static u16 mii_read( int addr, int reg )
{
    u32 cmd = ( 0xF6L << 10 ) | ( (u32)addr << 5 ) | (u32)reg;
    u32 value = 0;
    int i;

    for ( i = 0; i < 32; i++ ) mdio_bit( MDIO_OUT | MDIO_DATA );   /* preamble */
    for ( i = 14; i >= 0; i-- ) {
        mdio_bit( (u16)( MDIO_OUT | ( ( cmd >> i ) & 1 ? MDIO_DATA : 0 ) ) );
    }
    /* the turnaround, sixteen bits of data and the idle bit */
    for ( i = 0; i < 19; i++ ) {
        outw( net_port + W4_PHYMGMT, 0 );
        (void)inw( net_port + W4_PHYMGMT );
        value = ( value << 1 ) | ( ( inw( net_port + W4_PHYMGMT ) & MDIO_DATA ) ? 1 : 0 );
        outw( net_port + W4_PHYMGMT, MDIO_CLK );
        (void)inw( net_port + W4_PHYMGMT );
    }
    if ( value & 0x20000L ) return 0xFFFF;
    return (u16)( value >> 1 );
}

/* The card's PHY: found, given up to three seconds to finish agreeing a
   speed with the other end (it starts at power-up), and asked what it
   agreed.  Sets el_fdx and el_speed, and says whether there is a
   link.  Leaves window 4 selected. */
static int el_mii( void )
{
    int addr, i, found = -1;
    u16 id1 = 0xFFFF, status = 0, mine, theirs, both;

    el_fdx = 0;
    el_speed = 0;
    el_window( 4 );
    for ( addr = 0; addr < 32; addr++ ) {
        id1 = mii_read( addr, MII_ID1 );
        if ( id1 != 0xFFFF && id1 != 0x0000 ) { found = addr; break; }
    }
    if ( found < 0 ) return 0;
    for ( i = 0; i < 300; i++ ) {               /* 300 x 10ms */
        status = mii_read( found, MII_BMSR );   /* the link bit latches  */
        status = mii_read( found, MII_BMSR );   /* low: the second read  */
        if ( ( status & 0x0024 ) == 0x0024 ) break;   /* is the line now */
        el_wait_us( 10000 );
    }
    mine = mii_read( found, MII_ANAR );
    theirs = mii_read( found, MII_ANLPAR );
    both = (u16)( mine & theirs );
    if ( status & 0x0020 ) {                    /* the two ends agreed */
        if ( both & 0x0100 )      { el_speed = "100BASE-T"; el_fdx = 1; }
        else if ( both & 0x0080 ) el_speed = "100BASE-T";
        else if ( both & 0x0040 ) { el_speed = "10BASE-T";  el_fdx = 1; }
        else if ( both & 0x0020 ) el_speed = "10BASE-T";
    }
    return ( status & 0x0004 ) != 0;
}

/* The transceiver at the first start.  An EEPROM that says choose one
   (the card as 3Com ships it) has them tried in Linux's order, among
   those W3_OPTIONS says are fitted: 100BASE-TX, 100BASE-FX, MII,
   10BASE-T, 10BASE2, AUI.  One with a link-beat bit is kept if a link
   comes up; MII is kept as it is, because its PHY does the agreeing;
   the others are kept untested.  With none, the EEPROM's own choice is
   used, and without the choose-one bit that is the answer anyway.
   100BASE-TX gets three seconds: the card's own does not negotiate, and
   a switch that does takes a while to give up asking and see what the
   line is carrying.  *LINK says whether one came up.

   THE FIRST TWO DRIVERS KNEW ONLY 100BASE-TX AND 10BASE-T, and the
   5x86's card has neither: W3_OPTIONS F040, MII alone - a PHY chip for
   both speeds.  Both fell back to the EEPROM's choice, 10BASE-T, which
   is not there; /T showed the card had sent nothing (2026-11-01). */
static u8 el_choose( int *link )
{
    u32 config;
    u16 fitted;
    u8  xcvr;

    el_window( 3 );
    config = inl( net_port + W3_CONFIG );
    fitted = inw( net_port + W3_OPTIONS );
    xcvr = (u8)( ( config & CFG_XCVR ) >> 20 );
    el_window( 4 );
    *link = 1;
    /* /10 or /100: that transceiver, whatever the EEPROM says, if the
       card has it.  A card with MII only agrees its speed in the PHY. */
    if ( el_switch( "100" ) && ( fitted & OPT_100TX ) ) xcvr = XCVR_100TX;
    else if ( el_switch( "10" ) && ( fitted & OPT_10T ) ) xcvr = XCVR_10T;
    else if ( config & CFG_AUTO ) {
        if ( fitted & OPT_100TX ) {
            el_media( XCVR_100TX );
            if ( el_link( 30 ) ) return XCVR_100TX;
        }
        if ( fitted & OPT_100FX ) {
            el_media( XCVR_100FX );
            if ( el_link( 20 ) ) return XCVR_100FX;
        }
        if ( fitted & OPT_MII ) xcvr = XCVR_MII;
        else {
            if ( fitted & OPT_10T ) {
                el_media( XCVR_10T );
                if ( el_link( 20 ) ) return XCVR_10T;
            }
            if ( fitted & OPT_10B2 ) xcvr = XCVR_10B2;
            else if ( fitted & OPT_AUI ) xcvr = XCVR_AUI;
        }
    }
    if ( xcvr == XCVR_MII ) {
        el_media( XCVR_MII );
        *link = el_mii();
        el_media( XCVR_MII );                   /* now with the duplex */
        return XCVR_MII;
    }
    el_media( xcvr );
    if ( xcvr == XCVR_10T || xcvr == XCVR_100TX || xcvr == XCVR_100FX ) {
        *link = el_link( xcvr == XCVR_10T ? 20 : 30 );
    }
    return xcvr;
}

/* the name said at the load: the transceiver, and whether it has a link.
   net_name holds 31 characters: "3Com 3C515, 100BASE-FX, no link" is
   the longest, and an MII PHY's speed is named only with a link. */
static void el_say_xcvr( u8 xcvr, int link )
{
    switch ( xcvr ) {
    case XCVR_10T:   net_strcpy( net_name, "3Com 3C515, 10BASE-T" );  break;
    case XCVR_AUI:   net_strcpy( net_name, "3Com 3C515, AUI" );       break;
    case XCVR_10B2:  net_strcpy( net_name, "3Com 3C515, 10BASE2" );   break;
    case XCVR_100TX: net_strcpy( net_name, "3Com 3C515, 100BASE-T" ); break;
    case XCVR_100FX: net_strcpy( net_name, "3Com 3C515, 100BASE-FX" ); break;
    case XCVR_MII:
        net_strcpy( net_name, "3Com 3C515, " );
        net_strcpy( net_name + 12, ( link && el_speed ) ? el_speed : "MII" );
        break;
    }
    if ( !link ) {
        char *end = net_name;

        while ( *end ) end++;
        net_strcpy( end, ", no link" );
    }
}

/* ---------------------------------------------------------------- */
/* running it                                                        */
/* ---------------------------------------------------------------- */

/* The transmit status stack: a frame that failed leaves a byte here and
   the transmitter stopped until the byte is popped and the transmitter
   enabled again.  0 is an empty stack, FF a card that has gone dark. */
static void tx_status_pop( void )
{
    u8  ts;
    int i;

    for ( i = 0; i < 4; i++ ) {
        ts = inb( net_port + OP_TXSTATUS );
        if ( ts == 0 || ts == 0xFF ) break;
        if ( ts & 0x38 ) net_stats.txerrors++;
        if ( ts & 0x30 ) el_cmd( CMD_TXRESET );
        if ( ts & 0x3C ) el_cmd( CMD_TXENABLE );
        outb( net_port + OP_TXSTATUS, 0 );      /* pop it */
    }
}

static void el_filter( void )
{
    /* station, broadcast AND multicast: the NetBEUI functional address
       is a multicast (EL3C509.C) */
    el_cmd( (u16)( CMD_SETFILTER | FILTER_STATION | FILTER_MULTI | FILTER_BCAST ) );
}

/* /T: a test at the load, for a card that sends and hears nothing
   (the 5x86, 2026-11-01).  One broadcast frame of the Ethernet loopback
   protocol (type 9000h, which nothing answers or minds) is sent, and the
   driver listens for five seconds.  The card's own counts then say
   whether the frame left and whether anything came in, which the
   driver's counts cannot.  Interrupts are masked throughout. */
static void el_test( void )
{
    u8  frame[60];
    u8  counts[10];
    u16 status, rxst, seen = 0, first = 0, empty = 0, rxbytes, txbytes;
    u8  txst, rxerr = 0;
    u32 heard = 0, i, wait;

    el_cmd( CMD_SETINTR );
    el_cmd( (u16)( CMD_SETSTATUS | 0x00FE ) );
    el_cmd( (u16)( CMD_ACKINTR | 0xFF ) );
    el_cmd( CMD_STATSEN );
    el_window( 6 );                             /* reading clears them */
    for ( i = 0; i < 10; i++ ) (void)inb( net_port + i );
    (void)inw( net_port + 10 );
    (void)inw( net_port + 12 );

    el_window( 3 );
    say_begin( "test before: configuration " );
    say_hex( inl( net_port + W3_CONFIG ), 8 );
    say_text( ", MAC control " );
    say_hex( inw( net_port + W3_MACCTRL ), 4 );
    say_end();
    el_window( 4 );
    say_begin( "test before: status " );
    say_hex( inw( net_port + EL_STATUS ), 4 );
    say_text( ", diagnostic " );
    say_hex( inw( net_port + W4_NETDIAG ), 4 );
    say_text( ", FIFO " );
    say_hex( inw( net_port + W4_FIFODIAG ), 4 );
    say_text( ", media " );
    say_hex( inw( net_port + W4_MEDIA ), 4 );
    say_end();
    el_window( 7 );

    net_memzero( frame, sizeof( frame ) );
    for ( i = 0; i < 6; i++ ) {
        frame[i] = 0xFF;
        frame[6 + i] = net_mac[i];
    }
    frame[12] = 0x90;
    frame[13] = 0x00;
    card_send( frame, sizeof( frame ) );

    for ( i = 0; i < 500; i++ ) {               /* 500 x 10ms */
        status = inw( net_port + EL_STATUS );
        seen |= status;
        rxst = inw( net_port + OP_RXSTATUS );
        if ( !( rxst & RXS_INCOMPLETE ) && rxst != 0 ) {
            heard++;
            if ( first == 0 ) first = rxst;
            if ( rxst & RXS_ERROR ) rxerr |= inb( net_port + OP_RXERRORS );
            el_cmd( CMD_RXDISCARD );
            for ( wait = 0; wait < SPIN_CMD; wait++ ) if ( !( inw( net_port + EL_STATUS ) & ST_CMDBUSY ) ) break;
            el_cmd( (u16)( CMD_ACKINTR | ST_RXCOMP ) );
            continue;                           /* the next one at once */
        }
        empty = rxst;
        el_wait_us( 10000 );
    }

    txst = inb( net_port + OP_TXSTATUS );
    if ( txst != 0 && txst != 0xFF ) outb( net_port + OP_TXSTATUS, 0 );
    el_window( 6 );
    for ( i = 0; i < 10; i++ ) counts[i] = inb( net_port + i );
    rxbytes = inw( net_port + 10 );
    txbytes = inw( net_port + 12 );

    /* say_buf holds 93 characters a line: the counts on four lines */
    say_begin( "test sent 1: status " );
    say_hex( txst, 2 );
    say_text( ", card sent " );
    say_dec( counts[6] );
    say_text( " (" );
    say_dec( txbytes );
    say_text( " bytes), carrier lost " );
    say_dec( counts[0] );
    say_end();
    say_begin( "test collisions: " );
    say_dec( counts[3] );
    say_text( " single, " );
    say_dec( counts[2] );
    say_text( " multiple, " );
    say_dec( counts[4] );
    say_text( " late, deferred " );
    say_dec( counts[8] );
    say_end();
    say_begin( "test heard " );
    say_dec( heard );
    say_text( ", first " );
    say_hex( first, 4 );
    say_text( ", errors " );
    say_hex( rxerr, 2 );
    say_text( ", card heard " );
    say_dec( counts[7] );
    say_text( " (" );
    say_dec( rxbytes );
    say_text( " bytes), overruns " );
    say_dec( counts[5] );
    say_end();
    el_window( 4 );
    say_begin( "test after: status " );
    say_hex( seen, 4 );
    say_text( ", empty " );
    say_hex( empty, 4 );
    say_text( ", diagnostic " );
    say_hex( inw( net_port + W4_NETDIAG ), 4 );
    say_text( ", FIFO " );
    say_hex( inw( net_port + W4_FIFODIAG ), 4 );
    say_text( ", media " );
    say_hex( inw( net_port + W4_MEDIA ), 4 );
    say_end();

    el_cmd( CMD_STATSDIS );
    el_window( 7 );
}

int card_setup( void )
{
    int i, link;

    el_cmd( CMD_SETINTR );

    /* THE TRANSCEIVER FIRST: chosen at the first start, the link waited
       for then; after a park the same one is put back.  Linux's open
       sets it before the resets too, and 3Com's own notes (quoted in
       Linux's 3c59x.c) want the transmitter and receiver reset after a
       change of transceiver.  The first EL3C515 chose it last, after
       both were reset and enabled, and on the 5x86 four frames went out
       and nothing came in (2026-11-01). */
    if ( !el_started ) {
        el_xcvr = el_choose( &link );
        el_say_xcvr( el_xcvr, link );
    } else {
        el_media( el_xcvr );
    }
    el_started = 1;

    /* Linux's corkscrew_open, without the bus master: no TotalReset (the
       card loaded its configuration from the EEPROM when it was powered
       up), the transmitter and receiver reset, the station address, the
       statistics off - nothing here reads them - and the two enabled */
    el_cmd( CMD_TXRESET );
    el_cmd( CMD_RXRESET );
    el_cmd( CMD_SETSTATUS );

    el_window( 2 );                         /* the station address */
    for ( i = 0; i < 6; i++ ) outb( net_port + i, net_mac[i] );
    for ( ; i < 12; i += 2 ) outw( net_port + i, 0 );   /* and no mask */

    el_cmd( CMD_STATSDIS );
    el_filter();
    el_cmd( CMD_RXENABLE );
    el_cmd( CMD_TXENABLE );
    el_window( 7 );                         /* where Linux leaves it */
    if ( el_switch( "T" ) ) el_test();      /* only at the load: net_line */

    /* the status bits that may be seen, and those that interrupt.  NOT
       TX-available as an interrupt: it is set whenever the FIFO has room,
       which at idle is always (EL3C509.C).  The transmit path polls the
       free count instead; TX-complete is set only by a frame that
       failed, and card_service pops the status that set it. */
    el_cmd( (u16)( CMD_SETSTATUS | ST_ADFAIL | ST_TXCOMP | ST_TXAVAIL | ST_RXCOMP | ST_INTREQ ) );
    el_cmd( (u16)( CMD_ACKINTR | 0xFF ) );
    if ( net_flags & NICF_POLLED ) el_cmd( CMD_SETINTR );
    else el_cmd( (u16)( CMD_SETINTR | ST_INTLATCH | ST_ADFAIL | ST_RXCOMP | ST_TXCOMP ) );
    return 0;
}

void card_stop( void )
{
    el_cmd( CMD_SETINTR );                      /* mask everything */
    el_cmd( CMD_RXDISABLE );
    el_cmd( CMD_TXDISABLE );
    if ( el_xcvr == XCVR_10B2 ) el_cmd( CMD_STOPCOAX );
}

int card_send( const u8 *frame, u32 len )
{
    u32 n, use = len, whole, rest;

    if ( len > NIC_MAXFRAME ) { net_stats.txerrors++; return 0; }
    if ( use < 60 ) use = 60;                    /* padded with zeroes */
    tx_status_pop();                             /* a stopped transmitter
                                                    would never make room */
    /* wait for room: the length doubleword and the frame in doublewords */
    for ( n = 0; n < SPIN_TX; n++ ) {
        if ( inw( net_port + OP_TXFREE ) >= ( ( use + 3 ) & ~3L ) + 4 ) break;
    }
    if ( n == SPIN_TX ) { net_stats.txerrors++; return 0; }
    outl( net_port + OP_FIFO, use );
    /* the whole doublewords straight from the frame; the rest, padding
       and all, from a copy - nothing past the caller's frame is read */
    whole = len / 4;
    if ( whole ) outsd( net_port + OP_FIFO, frame, whole );
    rest = ( ( use + 3 ) & ~3L ) - whole * 4;
    if ( rest ) {
        net_memzero( tail, rest );
        net_memcpy( tail, frame + whole * 4, len - whole * 4 );
        outsd( net_port + OP_FIFO, tail, rest / 4 );
    }
    return 1;
}

static void rx_drain( void )
{
    u16 status;
    u32 len, n, wait;
    u8 *slot;

    for ( n = 0; n < 16; n++ ) {
        status = inw( net_port + OP_RXSTATUS );
        if ( status & RXS_INCOMPLETE ) break;    /* still arriving, or none */
        if ( ( status & RXS_LEN ) == 0 && !( status & RXS_ERROR ) ) break;  /* FIFO empty */
        len = status & RXS_LEN;
        if ( ( status & RXS_ERROR ) || len < NIC_HDRLEN || len > NIC_MAXFRAME ) {
            net_stats.rxerrors++;
        } else {
            /* in doublewords: up to three bytes past the frame land in
               the slot, which has room for them (QSLOT, NETCORE.C) */
            slot = net_rx_begin();
            insd( net_port + OP_FIFO, slot, ( len + 3 ) / 4 );
            net_rx_commit( len );
        }
        el_cmd( CMD_RXDISCARD );                 /* drop this frame from the FIFO */
        for ( wait = 0; wait < SPIN_CMD; wait++ ) if ( !( inw( net_port + EL_STATUS ) & ST_CMDBUSY ) ) break;
        /* a discard that never finished: the receiver is flushed instead
           and this pass given up (EL3C509.C) */
        if ( wait == SPIN_CMD ) {
            el_cmd( CMD_RXRESET );
            el_filter();
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
        /* TxAvailable is not tested: it is set at idle whether or not an
           interrupt fired (EL3C509.C) */
        if ( !( st & ( ST_RXCOMP | ST_TXCOMP | ST_ADFAIL | ST_INTLATCH ) ) ) break;
        mine = 1;
        if ( st & ST_RXCOMP ) rx_drain();
        if ( st & ST_TXCOMP ) tx_status_pop();
        if ( st & ST_ADFAIL ) {
            /* the receiver is reset and started again, as Linux does */
            el_cmd( CMD_RXRESET );
            el_filter();
            el_cmd( CMD_RXENABLE );
        }
        el_cmd( (u16)( CMD_ACKINTR | ( st & ( ST_INTLATCH | ST_ADFAIL | ST_TXCOMP | ST_INTREQ ) ) ) );
    }
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { '3', 'C', '5', '1', '5', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
