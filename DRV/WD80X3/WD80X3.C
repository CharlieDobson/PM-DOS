/*
 * WD80X3.C - WD80X3.SYS, a driver for the shared-memory 8390 boards
 *             (Western Digital / SMC WD8003 and WD8013)
 *
 * The same National DP8390 the NE2000 is built on, but reached a
 * different way: instead of clocking packet memory through a data port,
 * these boards map the chip's RAM into a window of the machine's memory
 * and the driver reads and writes it directly.  That makes the chip
 * programming the NE2000's and the packet moves plain memory copies.
 * 86Box offers the WD8003E, WD8003EB and WD8013EBT; this is proved on
 * its WD8013EBT and WD8003E.
 *
 * FINDING IT.  The board's control registers are at an ISA port (0x280,
 * 0x300, 0x240, 0x340...) and bytes 8-15 there carry the station address,
 * a board ID and a checksum: the eight bytes sum to 0xFF.  /P:port
 * names the port.
 *
 * WHICH BOARD, as Linux's wd.c tells them apart.  On the oldest WD8003s
 * registers 0-5 are the address bytes again (8-13): an 8-bit board.
 * Otherwise bit 0 of register 1 is the 16-bit slot bit, read-only on a
 * WD8013 in a 16-bit slot: one that stays set when written clear is a
 * 16-bit board with 16K of RAM, anything else an 8-bit one with 8K.
 * (Bit 0 of the board ID is "Ethernet" and is set on every board; it
 * once made every WD8003 a 16-bit board here, with a ring twice the size
 * of its RAM.)
 *
 * THE WINDOW.  /M:segment, or where the board was set to decode: bits
 * 0-5 of register 0 are address bits 13-18, and on a 16-bit board bits
 * 0-4 of register 5 are 19-23 (bit 19 is 1 on an 8-bit board).  A board
 * that reads 0 or FFh there gets D000, Linux's choice.  The kernel
 * maps it: if a UMB or the EMS frame has been put over that address,
 * BUS_MAPPHYS hands back a second address that reaches the card.
 *
 * THE LINE.  /I:, or found by making the card interrupt: a remote DMA
 * read of no bytes ends at once and raises the remote-DMA-complete
 * interrupt (Linux's autoirq for these boards), and if no line answers
 * that, a frame sent to itself in internal loopback (see find_line).
 * A board that does not interrupt is run polled.
 *
 * THE RING.  The chip's RAM: six pages to transmit from, the rest
 * receive.  The next frame is at BNRY+1, the page after the last one
 * given back - the chip's own pointer, as NE2000.SYS reads it.  (A copy
 * of it kept here once started one page behind CURR, so every header
 * read was the wrong page and the ring was reset for every frame.)
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).
 */
#include "netdrv.h"

/* the board's control registers, from the I/O base */
#define WD_MSR      0x00        /* memory select: reset, enable, window A13-A18 */
#define WD_ICR      0x01        /* interface configuration (16-bit on 8013) */
#define WD_IRR      0x04        /* interrupt request (soft-configured boards) */
#define WD_LAAR     0x05        /* for 16-bit boards: 16-bit enable, window A19-A23 */
#define WD_PROM     0x08        /* 8 bytes: the station address, ID, checksum */
#define WD_ID       0x0E        /* the board ID byte */
#define WD_8390     0x10        /* the DP8390's registers start here */

#define MSR_RESET   0x80
#define MSR_MENB    0x40        /* the memory window is enabled */
#define ICR_16BIT   0x01        /* the board is in a 16-bit slot */
#define IRR_IEN     0x80        /* the board's interrupt output on */
#define LAAR_L16EN  0x40        /* let the chip do 16-bit transfers */
#define LAAR_M16EN  0x80        /* ...and the memory window be 16-bit */
#define ID_SOFTCFG  0x20        /* the board is configured by software */

/* the DP8390, as offsets from WD_8390 (same chip as the NE2000) */
#define NE_CR       0x00
#define NE_PSTART   0x01
#define NE_PSTOP    0x02
#define NE_BNRY     0x03
#define NE_TPSR     0x04
#define NE_TBCR0    0x05
#define NE_TBCR1    0x06
#define NE_ISR      0x07
#define NE_RBCR0    0x0A
#define NE_RBCR1    0x0B
#define NE_RCR      0x0C
#define NE_TCR      0x0D
#define NE_DCR      0x0E
#define NE_IMR      0x0F
#define NE_PAR0     0x01        /* page 1 */
#define NE_CURR     0x07        /* page 1 */
#define NE_MAR0     0x08        /* page 1: the multicast hash, eight bytes */

#define CR_STP      0x01
#define CR_STA      0x02
#define CR_TXP      0x04
#define CR_RD0      0x08        /* remote read: only for the line probe */
#define CR_RD2      0x20        /* no remote DMA: the RAM is in the window */
#define CR_PAGE0    0x00
#define CR_PAGE1    0x40

#define ISR_PRX     0x01
#define ISR_PTX     0x02
#define ISR_RXE     0x04
#define ISR_TXE     0x08
#define ISR_OVW     0x10
#define ISR_CNT     0x20
#define ISR_RDC     0x40
#define ISR_ALL     ( ISR_PRX | ISR_PTX | ISR_RXE | ISR_TXE | ISR_OVW | ISR_CNT )

#define DCR_WTS     0x01
#define DCR_LS      0x08
#define DCR_FT1     0x40
#define RCR_AB      0x04
#define RCR_AM      0x08        /* take multicast (filtered by the MAR hash) */
#define RCR_MON     0x20
#define TCR_LB      0x02
#define RSR_PRX     0x01
#define IMR_WANTED  ( ISR_PRX | ISR_PTX | ISR_RXE | ISR_TXE | ISR_OVW )

#define TX_PAGES    6
#define SPIN_TXP    200000L
#define PROBE_TICKS 4           /* the line probe's wait, in ticks */

static uint32_t win;                    /* the mapped memory window */
static uint32_t membase;                /* its physical address */
static unsigned int ramsize;            /* 8K or 16K */
static uint8_t  tx_page, rx_start, rx_stop;
static unsigned char word;

static const uint16_t wd_ports[] = { 0x280, 0x300, 0x240, 0x340, 0x2A0, 0x380, 0 };

static uint8_t reg( int off ) { return inb( net_port + off ); }
static void setreg( int off, uint8_t v ) { outb( net_port + off, v ); }
static uint8_t nic_rd( int r ) { return inb( net_port + WD_8390 + r ); }
static void nic_wr( int r, uint8_t v ) { outb( net_port + WD_8390 + r, v ); }

/* a page of the card's RAM, in the window */
static uint8_t *page_at( unsigned int page )
{
    return (uint8_t *)( win + ( page << 8 ) );
}

/* is there an SMC board at net_port?  The checksum of the eight PROM
   bytes is the mark */
static int wd_probe( void )
{
    uint8_t sum = 0;
    int i;

    for ( i = 0; i < 8; i++ ) sum = (uint8_t)( sum + reg( WD_PROM + i ) );
    if ( sum != 0xFF ) return 0;
    for ( i = 0; i < 6; i++ ) net_mac[i] = reg( WD_PROM + i );
    return net_mac_ok( net_mac );
}

/* a 16-bit board, by Linux's test: not an old board whose registers
   0-5 repeat the address, and the slot bit stays set when cleared */
static int wd_is16( void )
{
    uint8_t was;
    int i, sixteen;

    for ( i = 0; i < 6 && reg( i ) == reg( WD_PROM + i ); i++ ) ;
    if ( i == 6 ) return 0;
    was = reg( WD_ICR );
    setreg( WD_ICR, (uint8_t)( was ^ ICR_16BIT ) );
    sixteen = ( reg( WD_ICR ) & ICR_16BIT ) && ( was & ICR_16BIT );
    setreg( WD_ICR, was );
    return sixteen;
}

/* the board reset, its window placed at membase and enabled, and the
   8390 set up and started with the ring empty: the station address,
   every multicast address (NetBEUI's functional address 03-00-00-00-
   00-01 is one), IMR as given.  LOOP puts the chip in internal loopback
   (DCR's LS clear and TCR's LB set), where a frame sent never reaches
   the wire.  The window's address is split: MSR carries bits A13-A18,
   and on a 16-bit board LAAR carries A19-A23 */
static void chip_init( int loop, uint8_t imr )
{
    int i;

    setreg( WD_MSR, MSR_RESET );
    net_delay_us( 2000 );
    setreg( WD_MSR, 0 );
    if ( word ) setreg( WD_LAAR, (uint8_t)( LAAR_L16EN | LAAR_M16EN | ( ( membase >> 19 ) & 0x1F ) ) );
    setreg( WD_MSR, (uint8_t)( MSR_MENB | ( ( membase >> 13 ) & 0x3F ) ) );

    tx_page = 0;
    rx_start = TX_PAGES;
    rx_stop = (uint8_t)( ramsize >> 8 );        /* last page + 1 */

    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    nic_wr( NE_DCR, (uint8_t)( DCR_FT1 | ( loop ? 0 : DCR_LS ) | ( word ? DCR_WTS : 0 ) ) );
    nic_wr( NE_RBCR0, 0 );
    nic_wr( NE_RBCR1, 0 );
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
    for ( i = 0; i < 8; i++ ) nic_wr( NE_MAR0 + i, 0xFF );
    nic_wr( NE_CURR, (uint8_t)( rx_start + 1 ) ); /* BNRY + 1: the ring is empty */
    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
    nic_wr( NE_ISR, 0xFF );
    nic_wr( NE_TCR, (uint8_t)( loop ? TCR_LB : 0 ) );
    nic_wr( NE_RCR, (uint8_t)( RCR_AB | RCR_AM ) );
    nic_wr( NE_IMR, imr );
}

/* the line probe's answer: the board is interrupting, and is quieted */
static int probe_isr( void )
{
    if ( !( nic_rd( NE_ISR ) & ( ISR_RDC | ISR_PTX | ISR_PRX ) ) ) return 0;
    nic_wr( NE_IMR, 0 );
    nic_wr( NE_ISR, 0xFF );
    return 1;
}

static void probe_wait( void )
{
    uint32_t start = net_ticks();

    while ( net_probe_hit == NIC_NOIRQ && net_ticks() - start < PROBE_TICKS ) ;
}

/* the line, found by making the board interrupt: at the load only.
   First Linux's way, a remote read of no bytes, which a real 8390 ends
   at once with its INT pin held until the end is acknowledged.  86Box
   raises and drops the line in the same instant, which an 8259 does
   not take, so if no line answered, a frame is sent to the board itself
   in internal loopback: its end is held as an interrupt like any other */
static void find_line( void )
{
    uint8_t *frame;

    net_probe_isr = probe_isr;
    net_irq_probe_begin();
    chip_init( 1, 0 );
    nic_wr( NE_IMR, ISR_RDC );
    nic_wr( NE_CR, CR_PAGE0 | CR_RD0 | CR_STA );    /* a read of nothing */
    probe_wait();
    if ( net_probe_hit == NIC_NOIRQ ) {
        nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
        nic_wr( NE_ISR, 0xFF );
        frame = page_at( tx_page );
        net_memzero( frame, 60 );
        net_memcpy( frame, net_mac, 6 );            /* to itself, from itself */
        net_memcpy( frame + 6, net_mac, 6 );
        nic_wr( NE_TPSR, tx_page );
        nic_wr( NE_TBCR0, 60 );
        nic_wr( NE_TBCR1, 0 );
        nic_wr( NE_IMR, ISR_PTX | ISR_PRX );
        nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_TXP | CR_STA );
        probe_wait();
    }
    nic_wr( NE_IMR, 0 );
    nic_wr( NE_ISR, 0xFF );
    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    net_irq_probe_end();
    net_probe_isr = 0;
}

int card_find( void )
{
    int i;
    uint8_t msr, high;

    net_strcpy( net_name, "WD8003/WD8013" );
    if ( net_opt_port ) {
        net_port = (uint16_t)net_opt_port;
        if ( !wd_probe() ) return 1;
    } else {
        for ( i = 0; wd_ports[i]; i++ ) {
            net_port = wd_ports[i];
            if ( wd_probe() ) break;
        }
        if ( !wd_ports[i] ) return 1;
    }

    word = (unsigned char)wd_is16();
    ramsize = word ? 0x4000 : 0x2000;
    net_bus = NICB_ISA;
    net_flags = (uint8_t)( word ? NICF_WORD : 0 );

    /* the window: /M:, or where the board decodes */
    if ( net_opt_mem ) {
        membase = net_opt_mem << 4;
    } else {
        msr = reg( WD_MSR );
        if ( msr == 0 || msr == 0xFF ) {
            membase = 0xD0000L;
        } else {
            high = (uint8_t)( word ? reg( WD_LAAR ) & 0x1F : 0x01 );
            if ( high == 0x1F ) high = 0x01;            /* no register 5 */
            membase = ( (uint32_t)( msr & 0x3F ) << 13 ) | ( (uint32_t)high << 19 );
        }
    }
    /* a window the board can decode: in upper memory, on a boundary of
       its RAM size */
    if ( net_opt_mem > 0xFFFFL || membase < 0xA0000L || ( membase & ( ramsize - 1 ) ) != 0
         || membase + ramsize > 0x100000L ) {
        net_print( "/M: is not a window this board can use\r\n" );
        return 1;
    }
    net_mem = membase;
    win = net_pci_map( membase, ramsize );
    if ( win == 0 ) {
        net_print( "WD80X3: the card's memory window cannot be reached\r\n" );
        return 1;
    }
    net_strcpy( net_name, word ? "WD8013 (shared memory)" : "WD8003 (shared memory)" );

    /* the line: /I: (2 is 9 on an AT), or found at the load */
    if ( net_opt_irq == 2 ) net_opt_irq = 9;
    if ( reg( WD_ID ) & ID_SOFTCFG ) setreg( WD_IRR, (uint8_t)( reg( WD_IRR ) | IRR_IEN ) );
    if ( net_line && net_opt_irq == NIC_NOIRQ && !net_opt_poll ) find_line();
    return 0;
}

int card_setup( void )
{
    chip_init( 0, (uint8_t)( net_flags & NICF_POLLED ? 0 : IMR_WANTED ) );
    return 0;
}

void card_stop( void )
{
    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
    nic_wr( NE_IMR, 0 );
    nic_wr( NE_ISR, 0xFF );
    setreg( WD_MSR, (uint8_t)( ( membase >> 13 ) & 0x3F ) ); /* window off */
}

int card_send( const uint8_t *frame, unsigned int len )
{
    unsigned int n;
    uint8_t *dst = page_at( tx_page );

    for ( n = 0; n < SPIN_TXP; n++ ) {
        if ( !( nic_rd( NE_CR ) & CR_TXP ) ) break;
    }
    if ( n == SPIN_TXP ) { net_stats.txerrors++; return 0; }
    net_memcpy( dst, frame, len );
    if ( len < 60 ) { net_memzero( dst + len, 60 - len ); len = 60; }
    nic_wr( NE_TPSR, tx_page );
    nic_wr( NE_TBCR0, (uint8_t)len );
    nic_wr( NE_TBCR1, (uint8_t)( len >> 8 ) );
    nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_TXP | CR_STA );
    return 1;
}

/* the ring emptied into the queue: the next frame is at BNRY + 1 */
static void rx_drain( void )
{
    uint8_t  curr, bnry, next, page;
    uint8_t *hdr;
    unsigned int count, len, n, off, end, first;
    uint8_t *slot;

    for ( n = 0; n < 64; n++ ) {
        nic_wr( NE_CR, CR_PAGE1 | CR_RD2 | CR_STA );
        curr = nic_rd( NE_CURR );
        nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
        bnry = nic_rd( NE_BNRY );
        page = (uint8_t)( bnry + 1 );
        if ( page >= rx_stop ) page = rx_start;
        if ( page == curr ) break;

        hdr = page_at( page );
        next = hdr[1];
        count = (uint16_t)( hdr[2] | ( hdr[3] << 8 ) );
        /* a frame that is whole, of a length a frame can have, whose
           next page is in the ring: anything else is a ring the chip
           lost hold of, and it is started again empty */
        if ( !( hdr[0] & RSR_PRX ) || count < 4 + NIC_HDRLEN || count > 4 + NIC_MAXFRAME + 4
                || next < rx_start || next >= rx_stop ) {
            net_stats.rxerrors++;
            nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STP );
            nic_wr( NE_BNRY, rx_start );
            nic_wr( NE_CR, CR_PAGE1 | CR_RD2 | CR_STP );
            nic_wr( NE_CURR, (uint8_t)( rx_start + 1 ) );
            nic_wr( NE_CR, CR_PAGE0 | CR_RD2 | CR_STA );
            break;
        }
        len = count - 4;
        if ( len > NIC_MAXFRAME ) len = NIC_MAXFRAME;   /* the CRC, on a real chip */
        slot = net_rx_begin();
        off = ( (uint32_t)page << 8 ) + 4;
        /* a frame that runs off the end of the ring goes on at rx_start */
        end = (uint32_t)rx_stop << 8;
        first = end - off;
        if ( len > first ) {
            net_memcpy( slot, (uint8_t *)( win + off ), first );
            net_memcpy( slot + first, (uint8_t *)( win + ( (uint32_t)rx_start << 8 ) ), len - first );
        } else {
            net_memcpy( slot, (uint8_t *)( win + off ), len );
        }
        net_rx_commit( len );
        nic_wr( NE_BNRY, (uint8_t)( next == rx_start ? rx_stop - 1 : next - 1 ) );
    }
}

int card_service( void )
{
    uint8_t  isr;
    unsigned int n;
    int      mine = 0;

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
