/*
 * PCNET.C - PCNET.SYS, a driver for the AMD PCnet-PCI (Am79C970 and
 *           kin), the Lance family
 *
 * AMD's Lance was the first widely-cloned bus-master Ethernet chip and
 * the PCnet-PCI is its PCI form - the card QEMU calls pcnet and 86Box
 * offers as the PCnet-PCI II and FAST III.  It works from two rings of
 * DESCRIPTORS in memory: each descriptor owns a buffer and a bit that
 * says whether the chip or the driver has it, and the chip walks the
 * rings on its own.
 *
 * THE 32-BIT SOFTWARE STYLE.  The Lance was a 16-bit chip and its first
 * descriptor and init-block formats held a buffer address in 24 bits -
 * which cannot name memory above sixteen megabytes, and PM-DOS's arena
 * is wherever the machine's memory is.  The PCnet has a second format,
 * "software style 2", whose addresses are a full 32 bits; this driver
 * sets it (BCR20) and uses it throughout, so a buffer anywhere is
 * named in full.  Register access stays 16-bit word I/O, the chip's
 * state after a reset, which is enough and simplest.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C); DMA is the driver's own
 * pointers (the identity map, NETDRV.H).
 */
#include "netdrv.h"

/* word I/O registers, from the first port */
#define R_RDP       0x10        /* a CSR's data */
#define R_RAP       0x12        /* which CSR or BCR */
#define R_RESET     0x14        /* read it to reset */
#define R_BDP       0x16        /* a BCR's data */

/* CSR0, the command and status register */
#define C0_INIT     0x0001
#define C0_STRT     0x0002
#define C0_STOP     0x0004
#define C0_TDMD     0x0008      /* poll the transmit ring now */
#define C0_IENA     0x0040      /* let the chip interrupt */
#define C0_INTR     0x0080      /* ...and it is */
#define C0_IDON     0x0100      /* the init block was read */
#define C0_TINT     0x0200      /* a frame went */
#define C0_RINT     0x0400      /* a frame arrived */
#define C0_STATUS   0xFF00      /* the bits write-one-to-clear */

/* a descriptor's second dword: status in the high half, 2's-complement
   buffer length in the low half with its top nibble ones */
#define OWN         0x80000000L /* the chip owns it (status bit 15) */
#define RD_ERR      0x40000000L
#define TD_STP      0x02000000L /* start of packet */
#define TD_ENP      0x01000000L /* end of packet */

#define RXN         8           /* log2 3 */
#define TXN         4           /* log2 2 */
#define BUFSZ       1536
#define TLEN_RLEN   0x2030      /* RLEN=3 at bits 4-7, TLEN=2 at 12-15 */

#define SPIN_INIT   1000000L
#define SPIN_TX     400000L

static volatile uint32_t *rxr, *txr; /* the rings, four dwords a descriptor */
static uint8_t *rxbuf[RXN];
static uint8_t *txbuf[TXN];
static uint8_t *iblock;
static int      rx_cur, tx_cur;

static const PCI_ID pcnet_ids[] = {
    { 0x1022, 0x2000 },         /* Am79C970/971/972/973 PCnet-PCI */
    { 0x1022, 0x2001 },         /* Am79C978 HomePNA */
    { 0, 0 }
};

static uint32_t align16( uint32_t p ) { return ( p + 15 ) & ~15L; }

static void     csr_write( int reg, uint16_t value ) { outw( net_port + R_RAP, (uint16_t)reg ); outw( net_port + R_RDP, value ); }
static uint16_t csr_read( int reg )             { outw( net_port + R_RAP, (uint16_t)reg ); return inw( net_port + R_RDP ); }
static void     bcr_write( int reg, uint16_t value ) { outw( net_port + R_RAP, (uint16_t)reg ); outw( net_port + R_BDP, value ); }

int card_find( void )
{
    const PCI_ID *which;
    int isio, i;

    if ( net_opt_port == 0 ) {
        if ( !net_pci_find( pcnet_ids, &which ) ) return 1;
        net_pci_enable( net_pciaddr );
        net_port = (uint16_t)net_pci_bar( net_pciaddr, 0, &isio );
        if ( net_port == 0 || !isio ) return 1;
        net_irq = (uint8_t)net_pci_irqline( net_pciaddr );
        net_bus = NICB_PCI;
    } else {
        net_port = (uint16_t)net_opt_port;
        net_bus = NICB_PCI;
    }
    net_flags = NICF_WORD;

    inw( net_port + R_RESET );                  /* reset -> 16-bit word I/O */
    csr_write( 0, C0_STOP );
    for ( i = 0; i < 6; i++ ) net_mac[i] = inb( net_port + i );   /* the APROM */
    if ( !net_mac_ok( net_mac ) ) return 1;

    if ( iblock == 0 ) {
        unsigned int need = 32 + RXN * 16 + TXN * 16 + ( RXN + TXN ) * BUFSZ + 16;
        uint8_t *pool = net_alloc( need );
        uint32_t at;
        if ( pool == 0 ) return 1;
        at = align16( (uint32_t)pool );
        iblock = (uint8_t *)at;                  at += 32;
        rxr = (volatile uint32_t *)at;           at += RXN * 16;
        txr = (volatile uint32_t *)at;           at += TXN * 16;
        for ( i = 0; i < RXN; i++ ) { rxbuf[i] = (uint8_t *)at; at += BUFSZ; }
        for ( i = 0; i < TXN; i++ ) { txbuf[i] = (uint8_t *)at; at += BUFSZ; }
    }
    net_strcpy( net_name, "PCnet-PCI" );
    return 0;
}

int card_setup( void )
{
    unsigned int n;
    int i;

    inw( net_port + R_RESET );
    csr_write( 0, C0_STOP );
    bcr_write( 20, 2 );                         /* software style 2: 32-bit */

    for ( i = 0; i < RXN; i++ ) {
        rxr[i * 4 + 0] = (uint32_t)rxbuf[i];
        rxr[i * 4 + 1] = OWN | ( (uint32_t)(uint16_t)( -BUFSZ ) ); /* 0xF000 is in the top nibble of -BUFSZ */
        rxr[i * 4 + 2] = 0;
        rxr[i * 4 + 3] = 0;
    }
    for ( i = 0; i < TXN; i++ ) {
        txr[i * 4 + 0] = (uint32_t)txbuf[i];
        txr[i * 4 + 1] = 0;                      /* OWN clear: the driver's */
        txr[i * 4 + 2] = 0;
        txr[i * 4 + 3] = 0;
    }
    rx_cur = tx_cur = 0;

    /* the init block, software style 2 */
    net_memzero( iblock, 32 );
    *(uint16_t *)( iblock + 0 ) = 0;               /* MODE: normal, broadcast on */
    *(uint16_t *)( iblock + 2 ) = TLEN_RLEN;
    net_memcpy( iblock + 4, net_mac, 6 );
    /* LADRF (the 64-bit multicast hash, init-block offset 12) all ones: take
       every multicast address.  NetBEUI's functional address
       03-00-00-00-00-01 is a multicast, not only broadcast */
    for ( i = 0; i < 8; i++ ) iblock[12 + i] = 0xFF;
    *(uint32_t *)( iblock + 20 ) = (uint32_t)rxr;  /* RDRA */
    *(uint32_t *)( iblock + 24 ) = (uint32_t)txr;  /* TDRA */

    csr_write( 1, (uint16_t)( (uint32_t)iblock & 0xFFFF ) );
    csr_write( 2, (uint16_t)( ( (uint32_t)iblock >> 16 ) & 0xFFFF ) );
    csr_write( 4, 0x0915 );                      /* auto-pad transmit, and the rest off */
    csr_write( 3, 0x0000 );
    csr_write( 0, C0_INIT );
    for ( n = 0; n < SPIN_INIT; n++ ) {
        if ( csr_read( 0 ) & C0_IDON ) break;
    }
    if ( n == SPIN_INIT ) return 1;
    /* IDON is cleared (it is written back) in the same write that lets
       the chip interrupt: left set, it asserted the line the moment IENA
       went on, while the driver was not yet answering, and the kernel
       shut the line after 256 of them - every frame came in on the
       clock instead (QEMU showed "interrupts 0", 2026-11-14) */
    csr_write( 0, (uint16_t)( C0_IDON | C0_STRT | ( net_flags & NICF_POLLED ? 0 : C0_IENA ) ) );
    return 0;
}

void card_stop( void )
{
    csr_write( 0, C0_STOP );
}

int card_send( const uint8_t *frame, unsigned int len )
{
    static uint8_t pad[64];
    int entry = tx_cur;
    volatile uint32_t *d = txr + entry * 4;
    unsigned int n;

    if ( d[1] & OWN ) {
        for ( n = 0; n < SPIN_TX; n++ ) {
            if ( !( d[1] & OWN ) ) break;
        }
        if ( n == SPIN_TX ) { net_stats.txerrors++; return 0; }
    }
    if ( len < 60 ) {
        net_memzero( pad, sizeof( pad ) );
        net_memcpy( pad, frame, len );
        net_memcpy( txbuf[entry], pad, 60 );
        len = 60;
    } else {
        net_memcpy( txbuf[entry], frame, len );
    }
    d[0] = (uint32_t)txbuf[entry];
    d[2] = 0;
    d[1] = OWN | TD_STP | TD_ENP | ( (uint32_t)(uint16_t)( -(long)len ) );
    tx_cur = ( entry + 1 ) % TXN;
    csr_write( 0, (uint16_t)( C0_TDMD | ( net_flags & NICF_POLLED ? 0 : C0_IENA ) ) );
    return 1;
}

static void rx_drain( void )
{
    volatile uint32_t *d;
    uint32_t status, mcnt;
    unsigned int len, n;
    uint8_t *slot;

    for ( n = 0; n < RXN * 2; n++ ) {
        d = rxr + rx_cur * 4;
        status = d[1];
        if ( status & OWN ) break;              /* the chip still has it */
        if ( ( status & RD_ERR ) || ( status & ( TD_STP | TD_ENP ) ) != ( TD_STP | TD_ENP ) ) {
            net_stats.rxerrors++;
        } else {
            mcnt = d[2] & 0xFFF;                          /* the frame and its CRC */
            len = mcnt - 4;
            if ( len >= NIC_HDRLEN && len <= NIC_MAXFRAME ) {
                slot = net_rx_begin();
                net_memcpy( slot, rxbuf[rx_cur], len );
                net_rx_commit( len );
            } else {
                net_stats.rxerrors++;
            }
        }
        d[2] = 0;
        d[1] = OWN | ( (uint32_t)(uint16_t)( -BUFSZ ) );  /* give it back to the chip */
        rx_cur = ( rx_cur + 1 ) % RXN;
    }
}

int card_service( void )
{
    uint16_t csr;
    unsigned int n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        csr = csr_read( 0 );
        if ( !( csr & C0_INTR ) ) break;
        mine = 1;
        /* clear the latched status bits by writing them back; keep the
           chip running and, unless polled, interrupting */
        csr_write( 0, (uint16_t)( ( csr & C0_STATUS ) | ( net_flags & NICF_POLLED ? 0 : C0_IENA ) ) );
        if ( csr & C0_RINT ) rx_drain();
        if ( csr & C0_TINT ) {
            /* a transmit that erred leaves ERR in its descriptor */
            int i;
            for ( i = 0; i < TXN; i++ ) {
                if ( ( txr[i * 4 + 1] & ( OWN | RD_ERR ) ) == RD_ERR ) {
                    net_stats.txerrors++;
                    txr[i * 4 + 1] &= ~RD_ERR;
                }
            }
        }
    }
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'P', 'C', 'n', 'e', 't', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
