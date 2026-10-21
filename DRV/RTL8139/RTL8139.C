/*
 * RTL8139.C - RTL8139.SYS, a driver for the Realtek RTL8139
 *
 * The Realtek Fast Ethernet chip that every PC emulator has (QEMU's
 * rtl8139, 86Box's RTL8139C+) and that filled a hundred cheap cards:
 * a PCI bus-master with a receive RING the card writes packets into
 * continuously and four transmit registers.  The first of the family
 * drivers written on the shared base (DRV\NETDRV.H, NETCORE.C) - which
 * holds the queue, the counts, the DEVICE= line and the four NIC_
 * functions, so this is the chip and nothing else.
 *
 * THE RECEIVE RING is one buffer the card fills round and round, each
 * packet behind a four-byte header (a status word and a length), with
 * CAPR saying how far the driver has read and the card stopping before
 * it.  WRAP is set and 1536 bytes of slack follow the ring, so a packet
 * that would run off the end is written whole into the slack and read
 * without a seam.  THE TRANSMIT is four descriptors, each a register
 * pair - an address and a status/length - used round-robin.
 *
 * DMA is the driver's own pointers (NETDRV.H): the ring and the four
 * transmit buffers are net_alloc'd and their addresses handed to the
 * card as they are.
 */
#include "netdrv.h"

/* the registers, from the first I/O port */
#define R_IDR0      0x00        /* the station address, six bytes */
#define R_MAR0      0x08        /* the multicast hash, eight bytes */
#define R_TSD0      0x10        /* transmit status/length, four at 4 apart */
#define R_TSAD0     0x20        /* transmit buffer address, four at 4 apart */
#define R_RBSTART   0x30        /* the receive ring's address */
#define R_CR        0x37        /* command */
#define R_CAPR      0x38        /* how far the driver has read */
#define R_CBR       0x3A        /* how far the card has written */
#define R_IMR       0x3C
#define R_ISR       0x3E
#define R_TCR       0x40
#define R_RCR       0x44
#define R_9346CR    0x50        /* the config-register lock */
#define R_CONFIG1   0x52

#define CR_BUFE     0x01        /* the ring is empty (read only) */
#define CR_TE       0x04        /* transmit on */
#define CR_RE       0x08        /* receive on */
#define CR_RST      0x10        /* reset */

#define IR_ROK      0x0001      /* a frame arrived */
#define IR_RER      0x0002
#define IR_TOK      0x0004      /* a frame went */
#define IR_TER      0x0008
#define IR_RXOVW    0x0010      /* the ring overflowed */
#define IR_FOVW     0x0040      /* the FIFO overflowed */
#define IR_WANTED   ( IR_ROK | IR_RER | IR_TOK | IR_TER | IR_RXOVW | IR_FOVW )

#define RX_STATUS_OK 0x0001     /* in a packet's own header word */

/* 8K ring + the 16-byte lead CAPR starts behind + 1536 of wrap slack */
#define RING        8192
#define RXSZ        ( RING + 16 + 1536 )
#define TXSZ        1536
#define TXN         4
#define TX_THRESH   0x00080000L /* early transmit at 256 bytes */

#define SPIN_RESET  1000000L
#define SPIN_TX     400000L

static u8  *rxbuf;
static u8  *txbuf[TXN];
static u32  cur_rx;
static int  tx_cur;
static u8   tx_used[TXN];

static const PCI_ID rtl_ids[] = {
    { 0x10EC, 0x8139 },         /* Realtek */
    { 0x10EC, 0x1211 },
    { 0x1113, 0x1211 },         /* Accton */
    { 0x1186, 0x1300 },         /* D-Link DFE-530TX+ */
    { 0x018A, 0x0106 },
    { 0x1259, 0xA117 },
    { 0, 0 }
};

static u32 align256( u32 p )
{
    return ( p + 255 ) & ~255L;
}

static int chip_reset( void )
{
    u32 n;

    outb( net_port + R_CR, CR_RST );
    for ( n = 0; n < SPIN_RESET; n++ ) {
        if ( !( inb( net_port + R_CR ) & CR_RST ) ) return 1;
    }
    return 0;
}

int card_find( void )
{
    const PCI_ID *which;
    int isio, i;

    if ( net_opt_port == 0 ) {
        if ( !net_pci_find( rtl_ids, &which ) ) return 1;
        net_pci_enable( net_pciaddr );
        net_port = (u16)net_pci_bar( net_pciaddr, 0, &isio );
        if ( net_port == 0 || !isio ) return 1;
        net_irq = (u8)net_pci_irqline( net_pciaddr );
        net_bus = NICB_PCI;
    } else {
        net_port = (u16)net_opt_port;
        net_bus = NICB_PCI;
    }
    net_flags = NICF_WORD;

    if ( !chip_reset() ) return 1;
    for ( i = 0; i < 6; i++ ) net_mac[i] = inb( net_port + R_IDR0 + i );
    if ( !net_mac_ok( net_mac ) ) return 1;

    if ( rxbuf == 0 ) {
        u8 *pool = net_alloc( RXSZ + TXN * TXSZ + 256 );
        if ( pool == 0 ) return 1;
        rxbuf = (u8 *)align256( (u32)pool );
        for ( i = 0; i < TXN; i++ ) txbuf[i] = rxbuf + RXSZ + i * TXSZ;
    }
    net_strcpy( net_name, "RTL8139 (PCI)" );
    return 0;
}

int card_setup( void )
{
    int i;

    if ( !chip_reset() ) return 1;
    outb( net_port + R_9346CR, 0xC0 );          /* unlock the config */
    outl( net_port + R_RBSTART, (u32)rxbuf );
    cur_rx = 0;
    tx_cur = 0;
    for ( i = 0; i < TXN; i++ ) tx_used[i] = 0;
    outb( net_port + R_CR, CR_TE | CR_RE );
    /* accept every multicast address: the NetBEUI functional address
       03-00-00-00-00-01 is a multicast, so a MAC that is to carry NetBEUI
       must take multicast frames in, not only physical-match and broadcast */
    outl( net_port + R_MAR0, 0xFFFFFFFFL );
    outl( net_port + R_MAR0 + 4, 0xFFFFFFFFL );
    /* accept physical-match, multicast and broadcast, wrap, no FIFO
       threshold, unlimited DMA burst, the 8K ring */
    outl( net_port + R_RCR, 0x0000E78EL );
    outl( net_port + R_TCR, 0x03000700L );      /* standard gap, unlimited DMA */
    outw( net_port + R_CAPR, (u16)( -16 ) );
    outb( net_port + R_9346CR, 0x00 );          /* lock it again */
    outw( net_port + R_ISR, 0xFFFF );
    outw( net_port + R_IMR, (u16)( net_flags & NICF_POLLED ? 0 : IR_WANTED ) );
    return 0;
}

void card_stop( void )
{
    outb( net_port + R_CR, 0 );
    outw( net_port + R_IMR, 0 );
    outw( net_port + R_ISR, 0xFFFF );
}

int card_send( const u8 *frame, u32 len )
{
    static u8 pad[64];
    int entry = tx_cur;
    u32 n;

    if ( tx_used[entry] ) {
        for ( n = 0; n < SPIN_TX; n++ ) {
            if ( inl( net_port + R_TSD0 + entry * 4 ) & 0x2000 ) break;    /* OWN: done */
        }
        if ( n == SPIN_TX ) { net_stats.txerrors++; return 0; }
    }
    net_memcpy( txbuf[entry], frame, len );
    if ( len < 60 ) {
        net_memzero( pad, sizeof( pad ) );
        net_memcpy( pad, frame, len );
        net_memcpy( txbuf[entry], pad, 60 );
        len = 60;
    }
    outl( net_port + R_TSAD0 + entry * 4, (u32)txbuf[entry] );
    outl( net_port + R_TSD0 + entry * 4, TX_THRESH | len );
    tx_used[entry] = 1;
    tx_cur = ( entry + 1 ) & ( TXN - 1 );
    return 1;
}

/* the ring, drained into the queue.  Interrupts are off */
static void rx_drain( void )
{
    u32 off, status, size, len, n;
    u8 *slot;

    for ( n = 0; n < 64; n++ ) {
        if ( inb( net_port + R_CR ) & CR_BUFE ) break;
        off = cur_rx % RING;
        status = *(volatile u32 *)( rxbuf + off );
        size = status >> 16;                    /* the frame and its 4-byte CRC */
        if ( !( status & RX_STATUS_OK ) || size < 4 + NIC_HDRLEN || size > 4 + NIC_MAXFRAME + 4 ) {
            /* a ring the card and the driver no longer agree about:
               stop, point it at the start, and go again */
            net_stats.rxerrors++;
            outb( net_port + R_CR, CR_TE );
            outl( net_port + R_RBSTART, (u32)rxbuf );
            outw( net_port + R_CAPR, (u16)( -16 ) );
            outb( net_port + R_CR, CR_TE | CR_RE );
            outl( net_port + R_RCR, 0x0000E78EL );   /* +AM, as card_setup */
            cur_rx = 0;
            break;
        }
        len = size - 4;
        slot = net_rx_begin();
        net_memcpy( slot, rxbuf + off + 4, len );
        net_rx_commit( len );
        cur_rx = ( cur_rx + size + 4 + 3 ) & ~3L;
        outw( net_port + R_CAPR, (u16)( cur_rx - 16 ) );
    }
}

int card_service( void )
{
    u16 isr;
    u32 n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        isr = inw( net_port + R_ISR );
        if ( !( isr & IR_WANTED ) ) break;
        outw( net_port + R_ISR, isr );          /* dismiss what was read */
        mine = 1;
        if ( isr & IR_RER ) net_stats.rxerrors++;
        if ( isr & IR_TER ) net_stats.txerrors++;
        if ( isr & ( IR_RXOVW | IR_FOVW ) ) {
            net_stats.overflows++;
            outw( net_port + R_CAPR, (u16)( inw( net_port + R_CBR ) - 16 ) );
            cur_rx = inw( net_port + R_CBR );
        }
        if ( isr & ( IR_ROK | IR_RER ) ) rx_drain();
    }
    return mine;
}

/* the table, with this card's name, pointing at NDSTART.ASM's veneers */
const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'R', 'T', 'L', '8', '1', '3', '9', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
