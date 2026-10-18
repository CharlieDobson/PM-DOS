/*
 * WD80X3.C - WD80X3.SYS, a driver for the shared-memory 8390 boards
 *             (Western Digital / SMC WD8003 and WD8013)
 *
 * The same National DP8390 the NE2000 is built on, but reached a
 * different way: instead of clocking packet memory through a data port,
 * these boards map the chip's RAM into a window of the machine's memory
 * and the driver reads and writes it directly.  That makes the chip
 * programming the NE2000's and the packet moves plain memory copies.
 * 86Box offers the family as the WD8013EP/A and EB/T; this is written
 * to the SMC specification and has NOT been run here (the PM-DOS VM's
 * 86Box was in use), so it is for 86Box or the real card to prove.
 *
 * FINDING IT.  The board's control registers are at an ISA port (0x280,
 * 0x300, 0x240, 0x340...) and its first sixteen bytes carry the station
 * address and a checksum that marks an SMC board - the eight bytes from
 * offset 8 sum to 0xFF.  /P:port names the port and /M:addr the memory
 * window (its segment, e.g. /M:D000); without /M: the window the card
 * was strapped to is used.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).
 */
#include "netdrv.h"

/* the board's control registers, from the I/O base */
#define WD_MSR      0x00        /* memory select: reset, enable, window A13-A18 */
#define WD_ICR      0x01        /* interface configuration (16-bit on 8013) */
#define WD_IAR      0x02        /* I/O address, for software config */
#define WD_LAAR     0x05        /* for 16-bit boards: 16-bit enable, window high */
#define WD_PROM     0x08        /* 8 bytes: the station address and the checksum */
#define WD_8390     0x10        /* the DP8390's registers start here */

#define MSR_RESET   0x80
#define MSR_MENB    0x40        /* the memory window is enabled */
#define ICR_16BIT   0x01        /* the board is a 16-bit one */
#define LAAR_L16EN  0x40        /* let the chip do 16-bit transfers */
#define LAAR_M16EN  0x80        /* ...and the memory window be 16-bit */

/* the DP8390, as offsets from WD_8390 (same chip as the NE2000) */
#define NE_CR       0x00
#define NE_CLDA0    0x01
#define NE_PSTART   0x01
#define NE_PSTOP    0x02
#define NE_BNRY     0x03
#define NE_TPSR     0x04
#define NE_TBCR0    0x05
#define NE_TBCR1    0x06
#define NE_ISR      0x07
#define NE_RCR      0x0C
#define NE_TCR      0x0D
#define NE_DCR      0x0E
#define NE_IMR      0x0F
#define NE_PAR0     0x01        /* page 1 */
#define NE_CURR     0x07        /* page 1 */

#define CR_STP      0x01
#define CR_STA      0x02
#define CR_TXP      0x04
#define CR_RD2      0x20        /* no remote DMA: the RAM is in the window */
#define CR_PAGE0    0x00
#define CR_PAGE1    0x40

#define ISR_PRX     0x01
#define ISR_PTX     0x02
#define ISR_RXE     0x04
#define ISR_TXE     0x08
#define ISR_OVW     0x10
#define ISR_CNT     0x20
#define ISR_ALL     ( ISR_PRX | ISR_PTX | ISR_RXE | ISR_TXE | ISR_OVW | ISR_CNT )

#define DCR_WTS     0x01
#define DCR_LS      0x08
#define DCR_FT1     0x40
#define RCR_AB      0x04
#define RCR_MON     0x20
#define TCR_LB      0x02
#define RSR_PRX     0x01
#define IMR_WANTED  ( ISR_PRX | ISR_PTX | ISR_RXE | ISR_TXE | ISR_OVW )

#define TX_PAGES    6

#define SPIN_RESET  100000L
#define SPIN_TXP    200000L

static u32  win;                        /* the mapped memory window */
static u32  membase;                    /* its physical address */
static u32  ramsize;                    /* 8K or 16K */
static u8   tx_page, rx_start, rx_stop;
static u8   word;
static u32  q_cur;                      /* our read page of the ring */

static const PCI_ID wd_ids[] = { { 0, 0 } };    /* ISA */
static const u16 wd_ports[] = { 0x280, 0x300, 0x240, 0x340, 0x2A0, 0x380, 0 };

static u8 reg( int off ) { return inb( net_port + off ); }
static void setreg( int off, u8 v ) { outb( net_port + off, v ); }
static u8 nic_rd( int r ) { return inb( net_port + WD_8390 + r ); }
static void nic_wr( int r, u8 v ) { outb( net_port + WD_8390 + r, v ); }

/* is there an SMC board at net_port?  The checksum of the eight PROM
   bytes is the mark */
static int wd_probe( void )
{
    u8 sum = 0;
    int i;

    for ( i = 0; i < 8; i++ ) sum = (u8)( sum + reg( WD_PROM + i ) );
    if ( sum != 0xFF ) return 0;
    for ( i = 0; i < 6; i++ ) net_mac[i] = reg( WD_PROM + i );
    return net_mac_ok( net_mac );
}

int card_find( void )
{
    int i;
    u8 type;

    if ( net_opt_port ) {
        net_port = (u16)net_opt_port;
        if ( !wd_probe() ) return 1;
    } else {
        for ( i = 0; wd_ports[i]; i++ ) {
            net_port = wd_ports[i];
            if ( wd_probe() ) break;
        }
        if ( !wd_ports[i] ) return 1;
    }
    (void)wd_ids;

    /* a WD8013 (and kin) is a 16-bit board; the WD8003 is 8-bit.  The
       board revision byte tells them apart; a 16-bit board has 16K of
       RAM, an 8-bit one 8K */
    type = reg( WD_PROM + 6 );                  /* board ID / revision */
    word = (u8)( ( reg( WD_ICR ) & ICR_16BIT ) || ( type & 0x01 ) );
    ramsize = word ? 0x4000 : 0x2000;
    net_bus = NICB_ISA;
    net_flags = (u8)( word ? NICF_WORD : 0 );

    /* the window: /M: segment, or a sane upper-memory default.  The
       card is PROGRAMMED to decode there (card_setup), rather than
       trusting a strapped base - the address is split across MSR and
       LAAR on a 16-bit board and is awkward to read back */
    membase = net_opt_mem ? ( net_opt_mem << 4 ) : 0xD0000L;
    net_mem = membase;
    win = net_pci_map( membase, ramsize );
    if ( win == 0 ) return 1;

    net_strcpy( net_name, word ? "SMC WD8013 (shared memory)" : "WD8003 (shared memory)" );
    return 0;
}

int card_setup( void )
{
    int i;

    /* reset the board, place the memory window at membase and enable
       it.  The address is split: MSR carries bits A13-A18, and on a
       16-bit board LAAR carries A19-A23 */
    setreg( WD_MSR, MSR_RESET );
    for ( i = 0; i < 1000; i++ ) reg( WD_MSR );
    setreg( WD_MSR, 0 );
    if ( word ) setreg( WD_LAAR, (u8)( LAAR_L16EN | LAAR_M16EN | ( ( membase >> 19 ) & 0x1F ) ) );
    setreg( WD_MSR, (u8)( MSR_MENB | ( ( membase >> 13 ) & 0x3F ) ) );

    tx_page = 0;
    rx_start = TX_PAGES;
    rx_stop = (u8)( ramsize >> 8 );             /* last page + 1 */

    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    nic_wr( NE_DCR, (u8)( DCR_FT1 | DCR_LS | ( word ? DCR_WTS : 0 ) ) );
    nic_wr( NE_RCR, RCR_MON );
    nic_wr( NE_TCR, TCR_LB );
    nic_wr( NE_TPSR, tx_page );
    nic_wr( NE_PSTART, rx_start );
    nic_wr( NE_BNRY, rx_start );
    nic_wr( NE_PSTOP, rx_stop );
    nic_wr( NE_ISR, 0xFF );
    nic_wr( NE_IMR, 0 );
    nic_wr( NE_CR, CR_PAGE1 | CR_RD2 | CR_STP );
    for ( i = 0; i < 6; i++ ) nic_wr( NE_PAR0 + i, net_mac[i] );
    nic_wr( NE_CURR, (u8)( rx_start + 1 ) );
    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
    nic_wr( NE_ISR, 0xFF );
    nic_wr( NE_IMR, (u8)( net_flags & NICF_POLLED ? 0 : IMR_WANTED ) );
    nic_wr( NE_TCR, 0 );
    nic_wr( NE_RCR, RCR_AB );
    q_cur = rx_start;
    return 0;
}

void card_stop( void )
{
    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    nic_wr( NE_IMR, 0 );
    nic_wr( NE_ISR, 0xFF );
    setreg( WD_MSR, 0 );                        /* window off */
}

/* a page of the card's RAM, in the window */
static u8 *page_at( u32 page )
{
    return (u8 *)( win + ( page << 8 ) );
}

int card_send( const u8 *frame, u32 len )
{
    u32 n;
    u8 *dst = page_at( tx_page );

    for ( n = 0; n < SPIN_TXP; n++ ) {
        if ( !( nic_rd( NE_CR ) & CR_TXP ) ) break;
    }
    if ( n == SPIN_TXP ) { net_stats.txerrors++; return 0; }
    net_memcpy( dst, frame, len );
    if ( len < 60 ) { net_memzero( dst + len, 60 - len ); len = 60; }
    nic_wr( NE_TPSR, tx_page );
    nic_wr( NE_TBCR0, (u8)len );
    nic_wr( NE_TBCR1, (u8)( len >> 8 ) );
    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_TXP | CR_STA );
    return 1;
}

static void rx_drain( void )
{
    u8  curr, next;
    u8 *hdr;
    u32 count, len, n, off;
    u8 *slot;

    for ( n = 0; n < 64; n++ ) {
        nic_wr( NE_CR, CR_PAGE1 | CR_RD2 | CR_STA );
        curr = nic_rd( NE_CURR );
        nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
        if ( (u8)q_cur == curr ) break;

        hdr = page_at( q_cur );
        next = hdr[1];
        count = (u16)( hdr[2] | ( hdr[3] << 8 ) );
        if ( !( hdr[0] & RSR_PRX ) || count < 4 + NIC_HDRLEN || count > 4 + NIC_MAXFRAME + 4
                || next < rx_start || next >= rx_stop ) {
            net_stats.rxerrors++;
            nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
            nic_wr( NE_BNRY, rx_start );
            nic_wr( NE_CR, CR_PAGE1 | CR_RD2 | CR_STP );
            nic_wr( NE_CURR, (u8)( rx_start + 1 ) );
            nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
            q_cur = rx_start;
            break;
        }
        len = count - 4;
        slot = net_rx_begin();
        off = ( q_cur << 8 ) + 4;
        /* a frame that runs off the end of the ring wraps to rx_start */
        {
            u32 end = (u32)rx_stop << 8;
            u32 first = end - off;
            if ( len > first ) {
                net_memcpy( slot, (u8 *)( win + off ), first );
                net_memcpy( slot + first, (u8 *)( win + ( (u32)rx_start << 8 ) ), len - first );
            } else {
                net_memcpy( slot, (u8 *)( win + off ), len );
            }
        }
        net_rx_commit( len );
        q_cur = next;
        nic_wr( NE_BNRY, (u8)( next == rx_start ? rx_stop - 1 : next - 1 ) );
    }
}

int card_service( void )
{
    u8  isr;
    u32 n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
        isr = nic_rd( NE_ISR );
        if ( !( isr & ISR_ALL ) ) break;
        mine = 1;
        if ( isr & ( ISR_PRX | ISR_RXE ) ) {
            nic_wr( NE_ISR, ISR_PRX | ISR_RXE );
            if ( isr & ISR_RXE ) net_stats.rxerrors++;
            rx_drain();
        }
        if ( isr & ( ISR_PTX | ISR_TXE ) ) {
            nic_wr( NE_ISR, ISR_PTX | ISR_TXE );
            if ( isr & ISR_TXE ) net_stats.txerrors++;
        }
        if ( isr & ISR_OVW ) { nic_wr( NE_ISR, ISR_OVW ); net_stats.overflows++; rx_drain(); }
        if ( isr & ISR_CNT ) nic_wr( NE_ISR, ISR_CNT );
    }
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'W', 'D', '8', '0', 'x', '3', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
