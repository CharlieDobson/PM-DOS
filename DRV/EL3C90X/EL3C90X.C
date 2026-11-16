/*
 * EL3C90X.C - EL3C90X.SYS, a driver for the 3Com 3C90x (EtherLink XL)
 *
 * The PCI 10/100 EtherLink XL, 3Com's bus-master card: the 3C900 and
 * 3C905 (Boomerang), the 3C900B and 3C905B (Cyclone), the 3C905C and
 * 3C920 (Tornado), and their server and small-office kin (3C980, 3C982,
 * 3CSOHO100) and the 3C555 laptop part, which all speak the same
 * descriptor lists.  Like the 3C509 it has banked WINDOW registers for
 * setup, and like the PCI cards above it moves frames by DMA - a
 * "download" list for transmit and an "upload" list for receive, each
 * descriptor owning a buffer and the chip walking the list on its own.
 * The 3C590/3C595 (Vortex) move frames through a FIFO instead and are
 * not taken.  No emulator has any of these, so this is written to the
 * 3Com specification and Linux's 3c59x.c, for Charlie's real 3C905s to
 * prove - it has NOT been run.
 *
 * THE FIRST VERSION (2026-10-15) would not have worked on a real card,
 * and the reading that found why is worth keeping:
 *   - It read the EEPROM after an empty 5,000-pass loop.  A word takes
 *     162 microseconds to read; on a Pentium the loop is gone in a few.
 *     Now the busy bit is waited on, 162us at a time, as Linux does.
 *   - It never chose a transceiver.  The 3C905 reaches 100BASE-TX
 *     through an MII PHY; the 3C905B/C through their own NWAY one.  The
 *     3C515 on the 5x86 heard nothing until its transceiver was chosen
 *     (EL3C515.C), and this family is its PCI cousin.
 *   - It acknowledged only the low byte of the status, but upload
 *     complete is bit 10: the level line would never have dropped.
 *   - Polled, it enabled no status bits at all, so it saw nothing.
 *   - Every frame asked for a transmit-complete indication and nothing
 *     ever popped the transmit status stack, which stalls the
 *     transmitter when it fills.
 *   - The card was not masked while it was being set up.
 *
 * THE TRANSCEIVER, as Linux's vortex_up chooses it: the EEPROM's own
 * choice, unless it says choose one (autoselect) - then NWAY on the
 * chips that have it, and otherwise the first fitted of 100BASE-TX,
 * 100BASE-FX, MII, 10BASE-T, 10BASE2, AUI.  With MII or NWAY the PHY is
 * found on the management lines (address 24 first, where the B and C
 * keep their own), given up to three seconds at the first start to
 * agree a speed, and asked what it agreed, for the duplex.  It is set
 * BEFORE the transmitter and receiver are reset, as Linux and 3Com's
 * notes want.
 *
 * TRANSMIT is one frame at a time: the download engine stalled, the
 * descriptor put on an empty list, the engine started again; the next
 * frame waits until the chip has cleared the list pointer, which every
 * chip of the family does when it is done.  RECEIVE is a ring of upload
 * descriptors the chip fills and marks complete; the ring is drained
 * on every service, interrupt or clock, and the engine unstalled after.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).  DMA is the driver's
 * own pointers (the identity map).
 */
#include "netdrv.h"

/* the command/status register, base+0x0E, in every window */
#define EL_CMD      0x0E
#define EL_STATUS   0x0E

#define CMD_SELWIN      0x0800  /* | window */
#define CMD_STARTCOAX   0x1000
#define CMD_RXDISABLE   0x1800
#define CMD_RXENABLE    0x2000
#define CMD_RXRESET     0x2800
#define CMD_UPSTALL     0x3000
#define CMD_UPUNSTALL   0x3001
#define CMD_DNSTALL     0x3002
#define CMD_DNUNSTALL   0x3003
#define CMD_TXENABLE    0x4800
#define CMD_TXDISABLE   0x5000
#define CMD_TXRESET     0x5800
#define CMD_ACKINTR     0x6800  /* | what is acknowledged */
#define CMD_SETINTR     0x7000  /* | what may interrupt */
#define CMD_SETSTATUS   0x7800  /* | what may be seen */
#define CMD_SETFILTER   0x8000
#define CMD_SETRXTHRESH 0x8800  /* | the early-receive threshold / 4 */
#define CMD_STATSDIS    0xB000
#define CMD_STOPCOAX    0xB800

#define RXRESET_KEEP_PHY 0x0004 /* RxReset: leave the network interface */

/* status bits */
#define ST_INTLATCH 0x0001
#define ST_HOSTERR  0x0002
#define ST_TXCOMP   0x0004
#define ST_TXAVAIL  0x0008
#define ST_RXEARLY  0x0020
#define ST_INTREQ   0x0040
#define ST_DNCOMP   0x0200
#define ST_UPCOMP   0x0400
#define ST_CMDBUSY  0x1000
#define ST_ACKMASK  0x07FF      /* every bit AckIntr takes */

/* the operating registers, whatever the window */
#define OP_TXSTATUS 0x1B        /* byte: the transmit status stack */
#define OP_PKTSTATUS 0x20       /* doubleword */
#define OP_DNLISTPTR 0x24
#define OP_TXFREETHRESH 0x2F    /* byte, Boomerang */
#define OP_UPLISTPTR 0x38

/* window 0: the EEPROM */
#define W0_EECMD    0x0A
#define W0_EEDATA   0x0C
#define EE_READ     0x0080
#define EE_READ_8BIT 0x0230     /* the 3C555's EEPROM */
#define EE_BUSY     0x8000

/* window 3: the configuration */
#define W3_CONFIG   0x00        /* doubleword InternalConfig */
#define W3_MACCTRL  0x06
#define W3_OPTIONS  0x08        /* the transceivers fitted */
#define CFG_XCVR    0x00F00000L /* bits 23-20 */
#define CFG_AUTO    0x01000000L /* the EEPROM says choose one */
#define MAC_FULLDUP 0x0020

#define OPT_100TX   0x02
#define OPT_100FX   0x04
#define OPT_10T     0x08
#define OPT_10B2    0x10
#define OPT_AUI     0x20
#define OPT_MII     0x41        /* MII or T4: Linux's mask */

/* window 4: the media and the PHY's management lines */
#define W4_NETDIAG  0x06
#define W4_PHYMGMT  0x08
#define W4_MEDIA    0x0A
#define MDIO_CLK    0x01
#define MDIO_DATA   0x02
#define MDIO_OUT    0x04
#define MED_SQE     0x0008
#define MED_10TP    0x00C0
#define MED_LNK     0x0080

#define MII_BMSR    1
#define MII_ID1     2
#define MII_ANAR    4
#define MII_ANLPAR  5

#define XCVR_10T    0
#define XCVR_AUI    1
#define XCVR_10B2   3
#define XCVR_100TX  4
#define XCVR_100FX  5
#define XCVR_MII    6
#define XCVR_NWAY   8
#define XCVR_EXTMII 9

#define IS_PHY( x ) ( ( x ) == XCVR_MII || ( x ) == XCVR_NWAY || ( x ) == XCVR_EXTMII )

/* filter bits */
#define FILTER_STATION 0x01
#define FILTER_MULTI   0x02     /* receive all multicast frames */
#define FILTER_BCAST   0x04

/* descriptor bits */
#define LAST_FRAG   0x80000000L
#define UP_COMPLETE 0x00008000L
#define UP_ERROR    0x00004000L
#define UP_LEN      0x00001FFFL

#define RXN     16
#define TXN     8
#define BUFSZ   1536

#define SPIN_CMD    100000L
#define SPIN_TX     400000L

/* the chip kinds */
#define K_BOOMERANG 0x01
#define K_NWAY      0x02        /* its own autonegotiating transceiver */
#define K_EE8BIT    0x04

/* a descriptor: next, status/header, buffer address, buffer length */
typedef struct { u32 next, status, addr, length; } DESC;

typedef struct { u16 device; u8 kind; const char *name; } CHIP;

static const CHIP chips[] = {
    { 0x9000, K_BOOMERANG, "3Com 3C900" }, { 0x9001, K_BOOMERANG, "3Com 3C900" },
    { 0x9004, 0, "3Com 3C900B" },  { 0x9005, 0, "3Com 3C900B" },
    { 0x9006, 0, "3Com 3C900B" },  { 0x900A, 0, "3Com 3C900B-FL" },
    { 0x9050, K_BOOMERANG, "3Com 3C905" }, { 0x9051, K_BOOMERANG, "3Com 3C905" },
    { 0x9054, K_NWAY, "3Com 3C905B" }, { 0x9055, K_NWAY, "3Com 3C905B" },
    { 0x9056, K_NWAY, "3Com 3C905B" }, { 0x9058, K_NWAY, "3Com 3C905B" },
    { 0x905A, 0, "3Com 3C905B-FX" },
    { 0x9200, K_NWAY, "3Com 3C905C" }, { 0x9201, K_NWAY, "3Com 3C920" },
    { 0x9202, 0, "3Com 3C920B" },      { 0x9210, K_NWAY, "3Com 3C920B" },
    { 0x9800, 0, "3Com 3C980" },       { 0x9805, K_NWAY, "3Com 3C980C" },
    { 0x1201, K_NWAY, "3Com 3C982" },  { 0x1202, K_NWAY, "3Com 3C982" },
    { 0x7646, K_NWAY, "3Com 3CSOHO100" },
    { 0x5055, K_EE8BIT, "3Com 3C555" },
    { 0, 0, 0 }
};

static const CHIP *chip;
static volatile DESC *rxd, *txd;
static u8  *rxbuf[RXN];
static u8  *txbuf[TXN];
static int  rx_cur, tx_cur;
static u8   xcvr = 0xFF;        /* the transceiver chosen at the first start */
static u8   fdx;                /* the PHY agreed full duplex */
static int  phy_addr = -1;
static const char *speed;       /* what the PHY agreed, for the name */

static u32 align16( u32 p ) { return ( p + 15 ) & ~15L; }

static void el_cmd( u16 cmd )
{
    u32 n;
    outw( net_port + EL_CMD, cmd );
    for ( n = 0; n < SPIN_CMD; n++ ) {
        if ( !( inw( net_port + EL_STATUS ) & ST_CMDBUSY ) ) break;
    }
}

static void el_window( int win ) { outw( net_port + EL_CMD, (u16)( CMD_SELWIN | win ) ); }

static void name_add( const char *text )
{
    char *to = net_name;
    int  left = (int)sizeof( net_name ) - 1;

    while ( *to && left ) { to++; left--; }
    while ( *text && left ) { *to++ = *text++; left--; }
    *to = 0;
}

/* one EEPROM word: the command, then the busy bit waited on, 162us at
   a time (the read takes that long), at most eleven times */
static u16 ee_word( int addr )
{
    int i;

    el_window( 0 );
    outw( net_port + W0_EECMD, (u16)( ( chip->kind & K_EE8BIT ? EE_READ_8BIT : EE_READ ) + addr ) );
    for ( i = 0; i < 11; i++ ) {
        net_delay_us( 162 );
        if ( !( inw( net_port + W0_EECMD ) & EE_BUSY ) ) break;
    }
    return inw( net_port + W0_EEDATA );
}

int card_find( void )
{
    PCI_ID one[2];
    int isio, i, from;
    u32 at, need;
    u8 *pool;

    one[1].vendor = 0;
    one[1].device = 0;
    for ( chip = chips; chip->device; chip++ ) {
        one[0].vendor = 0x10B7;
        one[0].device = chip->device;
        if ( net_pci_find( one, 0 ) ) break;
    }
    if ( chip->device == 0 ) { net_strcpy( net_name, "3Com 3C90x" ); return 1; }
    net_strcpy( net_name, chip->name );
    net_pci_enable( net_pciaddr );
    net_port = (u16)net_pci_bar( net_pciaddr, 0, &isio );
    if ( net_port == 0 || !isio ) return 1;
    net_irq = (u8)net_pci_irqline( net_pciaddr );
    net_bus = NICB_PCI;
    net_flags = NICF_WORD;

    /* quiet before anything: nothing may interrupt, and a boot ROM that
       used the card has its engines stopped */
    el_cmd( CMD_SETINTR );
    el_cmd( CMD_UPSTALL );
    el_cmd( CMD_DNSTALL );
    el_cmd( CMD_RXDISABLE );
    el_cmd( CMD_TXDISABLE );
    el_cmd( (u16)( CMD_ACKINTR | ST_ACKMASK ) );

    /* the station address: the OEM words 10-12, as Linux reads it, and
       the 3Com words 0-2 when those are empty */
    for ( from = 10; ; from = 0 ) {
        for ( i = 0; i < 3; i++ ) {
            u16 word = ee_word( from + i );
            net_mac[i * 2] = (u8)( word >> 8 );
            net_mac[i * 2 + 1] = (u8)word;
        }
        if ( net_mac_ok( net_mac ) || from == 0 ) break;
    }
    if ( !net_mac_ok( net_mac ) ) { net_print( "3Com 3C90x: no station address in the EEPROM\r\n" ); return 1; }

    if ( rxd == 0 ) {
        need = RXN * sizeof( DESC ) + TXN * sizeof( DESC ) + ( RXN + TXN ) * BUFSZ + 16;
        pool = net_alloc( need );
        if ( pool == 0 ) return 1;
        at = align16( (u32)pool );
        rxd = (volatile DESC *)at;  at += RXN * sizeof( DESC );
        txd = (volatile DESC *)at;  at += TXN * sizeof( DESC );
        for ( i = 0; i < RXN; i++ ) { rxbuf[i] = (u8 *)at; at += BUFSZ; }
        for ( i = 0; i < TXN; i++ ) { txbuf[i] = (u8 *)at; at += BUFSZ; }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* the PHY (MII), clocked by hand through window 4, as Linux does    */
/* ---------------------------------------------------------------- */

/* each port cycle on PCI is several hundred nanoseconds, well over the
   200 the MII wants a half-clock to be; the read between is the delay */
static void mdio_bit( u16 out )
{
    outw( net_port + W4_PHYMGMT, out );
    (void)inw( net_port + W4_PHYMGMT );
    outw( net_port + W4_PHYMGMT, (u16)( out | MDIO_CLK ) );
    (void)inw( net_port + W4_PHYMGMT );
}

/* register REG of the PHY at ADDR, with a full preamble every time (the
   B and C want one); FFFF when nothing answers.  Window 4 selected. */
static u16 mii_read( int addr, int reg )
{
    u32 cmd = ( 0xF6L << 10 ) | ( (u32)addr << 5 ) | (u32)reg;
    u32 value = 0;
    int i;

    for ( i = 0; i < 32; i++ ) mdio_bit( MDIO_OUT | MDIO_DATA );
    for ( i = 14; i >= 0; i-- ) mdio_bit( (u16)( MDIO_OUT | ( ( cmd >> i ) & 1 ? MDIO_DATA : 0 ) ) );
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

/* the PHY found (24 first: the 3C905CX answers at every address, and
   its own PHY is at 24), up to WAIT_TENTHS tenths of a second given to
   agree a speed, and what it agreed taken for the duplex and the name.
   Says whether there is a link. */
static int phy_check( int wait_tenths )
{
    int i, addr;
    u16 status = 0, both;

    el_window( 4 );
    if ( phy_addr < 0 ) {
        for ( i = 0; i < 32; i++ ) {
            addr = ( i == 0 ) ? 24 : ( i <= 24 ) ? i - 1 : i;
            status = mii_read( addr, MII_BMSR );
            if ( status != 0 && status != 0xFFFF ) { phy_addr = addr; break; }
        }
        if ( phy_addr < 0 ) return 0;
    }
    for ( i = 0; i <= wait_tenths * 10; i++ ) {
        status = mii_read( phy_addr, MII_BMSR );    /* the link bit latches */
        status = mii_read( phy_addr, MII_BMSR );    /* low: the second read is now */
        if ( ( status & 0x0024 ) == 0x0024 ) break;
        if ( i < wait_tenths * 10 ) net_delay_us( 10000 );
    }
    fdx = 0;
    speed = 0;
    if ( status & 0x0020 ) {
        both = (u16)( mii_read( phy_addr, MII_ANAR ) & mii_read( phy_addr, MII_ANLPAR ) );
        if ( both & 0x0100 )      { speed = "100BASE-T full"; fdx = 1; }
        else if ( both & 0x0080 ) speed = "100BASE-T half";
        else if ( both & 0x0040 ) { speed = "10BASE-T full"; fdx = 1; }
        else if ( both & 0x0020 ) speed = "10BASE-T half";
    }
    return ( status & 0x0004 ) != 0;
}

/* the transceiver at the first start (Linux's vortex_up), into the
   name with what is known of its link */
static void choose_xcvr( void )
{
    u32 config;
    u16 fitted;
    int link = -1;

    el_window( 3 );
    config = inl( net_port + W3_CONFIG );
    fitted = inw( net_port + W3_OPTIONS );
    if ( ( fitted & 0xFF ) == 0 ) fitted = OPT_MII;     /* Linux: the 3C916 */
    xcvr = (u8)( ( config & CFG_XCVR ) >> 20 );
    if ( config & CFG_AUTO ) {
        if ( ( chip->kind & K_NWAY ) || xcvr == XCVR_NWAY ) xcvr = XCVR_NWAY;
        else if ( fitted & OPT_100TX ) xcvr = XCVR_100TX;
        else if ( fitted & OPT_100FX ) xcvr = XCVR_100FX;
        else if ( fitted & OPT_MII ) xcvr = XCVR_MII;
        else if ( fitted & OPT_10T ) xcvr = XCVR_10T;
        else if ( fitted & OPT_10B2 ) xcvr = XCVR_10B2;
        else if ( fitted & OPT_AUI ) xcvr = XCVR_AUI;
    }
    if ( IS_PHY( xcvr ) ) link = phy_check( 30 );

    net_strcpy( net_name, chip->name );
    switch ( xcvr ) {
    case XCVR_10T:   name_add( ", 10BASE-T" );   break;
    case XCVR_AUI:   name_add( ", AUI" );        break;
    case XCVR_10B2:  name_add( ", 10BASE2" );    break;
    case XCVR_100TX: name_add( ", 100BASE-TX" ); break;
    case XCVR_100FX: name_add( ", 100BASE-FX" ); break;
    default:         name_add( ", " ); name_add( ( link > 0 && speed ) ? speed : "MII" ); break;
    }
    if ( link == 0 ) name_add( ", no link" );
}

/* the chosen transceiver into InternalConfig, the duplex into MacCtrl,
   the coax started or stopped, the link-beat bits into the media
   register (not for NWAY, as Linux leaves that one) */
static void set_xcvr( void )
{
    u32 config;
    u16 media;

    el_window( 3 );
    config = inl( net_port + W3_CONFIG );
    outl( net_port + W3_CONFIG, ( config & ~CFG_XCVR ) | ( (u32)xcvr << 20 ) );
    media = inw( net_port + W3_MACCTRL );
    if ( IS_PHY( xcvr ) && fdx ) media |= MAC_FULLDUP;
    else media &= (u16)~MAC_FULLDUP;
    outw( net_port + W3_MACCTRL, media );
    el_cmd( xcvr == XCVR_10B2 ? CMD_STARTCOAX : CMD_STOPCOAX );
    if ( xcvr != XCVR_NWAY ) {
        el_window( 4 );
        media = (u16)( inw( net_port + W4_MEDIA ) & ~( MED_10TP | MED_SQE ) );
        if ( xcvr == XCVR_10T ) media |= MED_10TP;
        else if ( xcvr == XCVR_AUI ) media |= MED_SQE;
        else if ( xcvr == XCVR_100TX || xcvr == XCVR_100FX ) media |= MED_LNK;
        outw( net_port + W4_MEDIA, media );
    }
}

/* ---------------------------------------------------------------- */
/* running it                                                        */
/* ---------------------------------------------------------------- */

/* the rest of the start, after the transceiver: also the way back from
   an adapter failure */
static void el_start( void )
{
    int i;

    el_cmd( CMD_TXRESET );
    el_cmd( CMD_RXRESET | RXRESET_KEEP_PHY );
    el_cmd( CMD_SETSTATUS );

    el_window( 2 );                             /* the address, and no mask */
    for ( i = 0; i < 6; i++ ) outb( net_port + i, net_mac[i] );
    for ( ; i < 12; i += 2 ) outw( net_port + i, 0 );
    el_cmd( CMD_STATSDIS );

    for ( i = 0; i < RXN; i++ ) {
        rxd[i].next = (u32)&rxd[( i + 1 ) % RXN];
        rxd[i].status = 0;
        rxd[i].addr = (u32)rxbuf[i];
        rxd[i].length = BUFSZ | LAST_FRAG;
    }
    for ( i = 0; i < TXN; i++ ) {
        txd[i].next = 0;
        txd[i].status = 0;
        txd[i].addr = (u32)txbuf[i];
        txd[i].length = 0;
    }
    rx_cur = tx_cur = 0;

    el_window( 7 );
    el_cmd( (u16)( CMD_SETRXTHRESH | ( 1536 >> 2 ) ) );
    outl( net_port + OP_PKTSTATUS, 0x00000020L );
    outl( net_port + OP_UPLISTPTR, (u32)&rxd[0] );
    if ( chip->kind & K_BOOMERANG ) outb( net_port + OP_TXFREETHRESH, (u8)( BUFSZ >> 8 ) );
    outl( net_port + OP_DNLISTPTR, 0 );

    /* station, broadcast AND multicast (NetBEUI's functional address
       03-00-00-00-00-01 is a multicast) */
    el_cmd( (u16)( CMD_SETFILTER | FILTER_STATION | FILTER_MULTI | FILTER_BCAST ) );
    el_cmd( CMD_RXENABLE );
    el_cmd( CMD_TXENABLE );
    el_cmd( CMD_UPUNSTALL );
    el_cmd( CMD_DNUNSTALL );

    /* what may be seen whether polled or not; what was raised while it
       was set up acknowledged; then what may interrupt */
    el_cmd( (u16)( CMD_SETSTATUS | ST_HOSTERR | ST_TXCOMP | ST_INTREQ | ST_DNCOMP | ST_UPCOMP ) );
    el_cmd( (u16)( CMD_ACKINTR | ST_ACKMASK ) );
    if ( net_flags & NICF_POLLED ) el_cmd( CMD_SETINTR );
    else el_cmd( (u16)( CMD_SETINTR | ST_INTLATCH | ST_HOSTERR | ST_TXCOMP | ST_UPCOMP ) );
}

int card_setup( void )
{
    el_cmd( CMD_SETINTR );                      /* masked while it is set up */
    if ( xcvr == 0xFF ) choose_xcvr();
    else if ( IS_PHY( xcvr ) ) phy_check( 0 );
    set_xcvr();
    el_start();
    return 0;
}

void card_stop( void )
{
    el_cmd( CMD_SETINTR );                      /* mask */
    el_cmd( CMD_UPSTALL );
    el_cmd( CMD_DNSTALL );
    el_cmd( CMD_RXDISABLE );
    el_cmd( CMD_TXDISABLE );
    if ( xcvr == XCVR_10B2 ) el_cmd( CMD_STOPCOAX );
}

/* The transmit status stack: a frame that failed leaves a byte here and
   the transmitter stopped until the byte is popped.  An underrun or a
   jabber also wants the transmitter reset (Linux's vortex_error). */
static void tx_status_pop( void )
{
    u8  ts;
    int i, reset = 0;

    for ( i = 0; i < 32; i++ ) {
        ts = inb( net_port + OP_TXSTATUS );
        if ( ts == 0 || ts == 0xFF ) break;
        if ( ts & 0x30 ) reset = 1;             /* jabber or underrun */
        if ( ts & 0x3C ) net_stats.txerrors++;
        outb( net_port + OP_TXSTATUS, 0 );      /* pop it */
    }
    if ( reset ) {
        /* the reset loses the Boomerang's free threshold too (Linux sets
           it again here); the frame in flight is lost */
        el_cmd( CMD_TXRESET );
        outl( net_port + OP_DNLISTPTR, 0 );
        if ( chip->kind & K_BOOMERANG ) outb( net_port + OP_TXFREETHRESH, (u8)( BUFSZ >> 8 ) );
    }
    el_cmd( CMD_TXENABLE );
}

int card_send( const u8 *frame, u32 len )
{
    int entry = tx_cur;
    volatile DESC *d = &txd[entry];
    u32 n;

    /* one frame at a time: the engine clears DnListPtr when it is done
       with the list, on every chip of the family */
    for ( n = 0; n < SPIN_TX && inl( net_port + OP_DNLISTPTR ) != 0; n++ ) ;
    if ( inl( net_port + OP_DNLISTPTR ) != 0 ) { net_stats.txerrors++; return 0; }
    if ( len < 60 ) {
        net_memzero( txbuf[entry], 60 );
        net_memcpy( txbuf[entry], frame, len );
        len = 60;
    } else {
        net_memcpy( txbuf[entry], frame, len );
    }
    d->next = 0;
    d->status = len;                            /* the header: no indication asked */
    d->addr = (u32)txbuf[entry];
    d->length = len | LAST_FRAG;
    el_cmd( CMD_DNSTALL );
    if ( inl( net_port + OP_DNLISTPTR ) == 0 ) outl( net_port + OP_DNLISTPTR, (u32)d );
    el_cmd( CMD_DNUNSTALL );
    tx_cur = ( entry + 1 ) % TXN;
    return 1;
}

/* the frames the chip has finished, handed over; how many */
static int rx_drain( void )
{
    volatile DESC *d;
    u32 status, len, n;
    int got = 0;
    u8 *slot;

    for ( n = 0; n < RXN; n++ ) {
        d = &rxd[rx_cur];
        status = d->status;
        if ( !( status & UP_COMPLETE ) ) break;
        if ( status & UP_ERROR ) {
            net_stats.rxerrors++;
        } else {
            len = status & UP_LEN;
            if ( len >= NIC_HDRLEN && len <= NIC_MAXFRAME ) {
                slot = net_rx_begin();
                net_memcpy( slot, rxbuf[rx_cur], len );
                net_rx_commit( len );
            } else {
                net_stats.rxerrors++;
            }
        }
        d->status = 0;                          /* give it back */
        rx_cur = ( rx_cur + 1 ) % RXN;
        got++;
    }
    if ( got ) el_cmd( CMD_UPUNSTALL );         /* keep the upload engine going */
    return got;
}

int card_service( void )
{
    u16 st;
    u32 n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        st = inw( net_port + EL_STATUS );
        if ( st == 0xFFFF ) break;              /* a card that has gone */
        if ( !( st & ( ST_INTLATCH | ST_HOSTERR | ST_TXCOMP | ST_INTREQ | ST_DNCOMP | ST_UPCOMP ) ) ) break;
        mine = 1;
        if ( st & ST_TXCOMP ) tx_status_pop();
        el_cmd( (u16)( CMD_ACKINTR | ( st & ( ST_INTLATCH | ST_RXEARLY | ST_INTREQ | ST_DNCOMP | ST_UPCOMP ) ) ) );
        if ( st & ST_HOSTERR ) {
            /* an adapter failure (a bus error, most likely): the card
               is started again from its rings up */
            net_stats.rxerrors++;
            el_start();
        }
    }
    return mine | ( rx_drain() != 0 );
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { '3', 'C', '9', '0', 'x', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
