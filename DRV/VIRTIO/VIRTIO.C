/*
 * VIRTIO.C - VIRTIO.SYS, a driver for the virtio network device
 *
 * The network card of a virtual machine that is not pretending to be
 * real hardware: QEMU's and KVM's "virtio-net", which Proxmox, libvirt
 * and most clouds give a guest by default, and VirtualBox's "virtio-net"
 * adapter.  There is no chip.  The driver and the host share rings in
 * the guest's memory - a VIRTQUEUE, one for receive and one for
 * transmit - and the registers only say who the device is, where the
 * rings are, and "look at queue N now".
 *
 * TWO INTERFACES.  A "transitional" device (PCI ID 1AF4:1000, QEMU's
 * default and VirtualBox's) is run through the LEGACY one (virtio
 * 0.9.5), in I/O BAR0: features at 0 and 4, the queue's page number at
 * 8, its size at 0Ch, the queue select at 0Eh, the notify at 10h, the
 * device status at 12h, the interrupt status at 13h (reading it clears
 * it and drops the line), and the station address at 14h.
 *
 * A "modern-only" device (1AF4:1041 - QEMU's with disable-legacy=on,
 * and what a newer host may give) has no I/O ports.  It is run through
 * the MODERN interface (virtio 1.0): vendor capabilities in its PCI
 * configuration space say where in its memory BARs four blocks are -
 * the common configuration (features, status, the queue select, size,
 * enable and the three addresses of each queue), the notify area (the
 * queue's index written at the queue's own offset in it), the interrupt
 * status (read to clear, as legacy's) and the device's configuration
 * (the station address).  It must be offered VIRTIO_F_VERSION_1 and
 * shown FEATURES_OK before its queues are set up, and its virtio-net
 * header is 12 bytes, not 10 (num_buffers is always there).
 *
 * A VIRTQUEUE is laid out the legacy way for both, in one page-aligned
 * block sized by the device (QEMU says 256 entries): the descriptor
 * table (16 bytes an entry: address, length, flags, next), the "avail"
 * ring the driver puts the head of a chain in, and - on the next 4K
 * boundary - the "used" ring the device puts finished chains in, with
 * the number of bytes it wrote.  A legacy device is given the block's
 * page; a modern one the three addresses, which that layout satisfies.
 * Every buffer here is TWO descriptors chained: the virtio-net header,
 * then the frame.  A legacy device may insist on the header being a
 * descriptor of its own (VIRTIO_F_ANY_LAYOUT is not asked for), so it
 * always is.
 *
 * Only VIRTIO_NET_F_MAC is accepted (and VERSION_1 on a modern device):
 * no checksum offload, no segmentation, no merged receive buffers - so
 * every header sent is zero and every header received is ignored.
 *
 * INTERRUPTS: there is no mask on the device, only a "no interrupt"
 * flag on each avail ring.  Receive keeps it set until card_setup is
 * done (and for good when polled); transmit keeps it set always, and
 * its finished buffers are taken back when the next frame is sent.  MSI-X
 * is left off, so the device uses its INTx line.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).  Proved on QEMU's
 * virtio-net-pci, pc and q35 machines, transitional and modern-only.
 */
#include "netdrv.h"

/* the legacy registers, from BAR0 */
#define VIO_HOST_FEATURES   0x00    /* 32 */
#define VIO_GUEST_FEATURES  0x04    /* 32 */
#define VIO_QUEUE_PFN       0x08    /* 32: the queue's address / 4096 */
#define VIO_QUEUE_SIZE      0x0C    /* 16 */
#define VIO_QUEUE_SELECT    0x0E    /* 16 */
#define VIO_QUEUE_NOTIFY    0x10    /* 16 */
#define VIO_STATUS          0x12    /* 8 */
#define VIO_ISR             0x13    /* 8: read to clear */
#define VIO_MAC             0x14    /* 6 bytes of the network's own */

/* the modern common configuration (struct virtio_pci_common_cfg) */
#define CC_DFSELECT         0x00    /* 32: which half of the device's features */
#define CC_DF               0x04    /* 32 */
#define CC_GFSELECT         0x08    /* 32: which half of the driver's */
#define CC_GF               0x0C    /* 32 */
#define CC_STATUS           0x14    /* 8 */
#define CC_QSELECT          0x16    /* 16 */
#define CC_QSIZE            0x18    /* 16 */
#define CC_QENABLE          0x1C    /* 16 */
#define CC_QNOTIFYOFF       0x1E    /* 16 */
#define CC_QDESC            0x20    /* 64 */
#define CC_QDRIVER          0x28    /* 64: the avail ring */
#define CC_QDEVICE          0x30    /* 64: the used ring */
#define CC_LEN              0x38

/* the vendor capability (struct virtio_pci_cap) and its kinds */
#define PCI_CAP_VENDOR      0x09
#define VCAP_COMMON         1
#define VCAP_NOTIFY         2
#define VCAP_ISR            3
#define VCAP_DEVICE         4

#define ST_ACKNOWLEDGE  0x01
#define ST_DRIVER       0x02
#define ST_DRIVER_OK    0x04
#define ST_FEATURES_OK  0x08
#define ST_FAILED       0x80

#define NET_F_MAC       0x00000020L     /* the device has an address for us */
#define F_VERSION_1     0x00000001L     /* bit 32: the high half's bit 0 */

#define VQ_RX           0
#define VQ_TX           1

#define DESC_NEXT       1
#define DESC_WRITE      2
#define AVAIL_NO_INTERRUPT 1

#define HDR_LEGACY      10              /* struct virtio_net_hdr */
#define HDR_MODERN      12              /* ...with num_buffers */
#define HDRSLOT         16              /* the room each header has */
#define BUFSZ           1536
#define RXN             16              /* buffers: two descriptors each */
#define TXN             8
#define QMAX            1024            /* the most entries a queue may say */

typedef struct { uint32_t addr_lo, addr_hi, len; uint16_t flags, next; } VDESC;
typedef struct { uint32_t id, len; } VUSED;

typedef struct {
    uint16_t size;              /* entries, as the device says */
    volatile VDESC *desc;
    volatile uint16_t *avail;   /* flags, idx, ring[size] */
    volatile uint16_t *used;    /* flags, idx, then VUSED[size] */
    uint16_t used_seen;         /* the used index taken so far */
    uint16_t avail_next;        /* the avail index given so far */
    uint32_t notify;            /* modern: where its notify is written */
} VQUEUE;

static VQUEUE   vq[2];
static uint8_t *qmem[2];        /* the page-aligned block of each */
static unsigned int qcap[2];    /* ...and the entries it was made for */
static uint8_t *hdrs;           /* RXN + TXN headers */
static uint8_t *rxbuf[RXN];
static uint8_t *txbuf[TXN];
static unsigned short tx_free;  /* TX buffer slots not in flight */
static unsigned char tx_busy[TXN];
static unsigned int hdrlen = HDR_LEGACY;

/* the modern device's four blocks, mapped (0 on a legacy one) */
static unsigned char modern;
static uint32_t m_common, m_isr, m_device;
static uint32_t m_notify, m_notify_len, m_notify_mul;

static const PCI_ID vio_ids[] = {
    { 0x1AF4, 0x1000 },         /* transitional virtio-net: the legacy ports */
    { 0x1AF4, 0x1041 },         /* modern-only virtio-net */
    { 0, 0 }
};

static uint32_t align4k( uint32_t p ) { return ( p + 4095 ) & ~4095L; }

/* ---------------------------------------------------------------- */
/* the registers, either way                                         */
/* ---------------------------------------------------------------- */

static void st_set( uint8_t value )
{
    if ( modern ) MMIO8( m_common, CC_STATUS ) = value;
    else outb( net_port + VIO_STATUS, value );
}

static uint8_t st_get( void )
{
    return modern ? MMIO8( m_common, CC_STATUS ) : inb( net_port + VIO_STATUS );
}

static uint8_t isr_get( void )
{
    return modern ? MMIO8( m_isr, 0 ) : inb( net_port + VIO_ISR );
}

static void notify( int index )
{
    if ( modern ) MMIO16( vq[index].notify, 0 ) = (uint16_t)index;
    else outw( net_port + VIO_QUEUE_NOTIFY, (uint16_t)index );
}

static unsigned int queue_size( int index )
{
    if ( modern ) {
        MMIO16( m_common, CC_QSELECT ) = (uint16_t)index;
        return MMIO16( m_common, CC_QSIZE );
    }
    outw( net_port + VIO_QUEUE_SELECT, (uint16_t)index );
    return inw( net_port + VIO_QUEUE_SIZE );
}

/* the device reset.  A modern device says the reset is done by reading
   back 0, and must not be touched before (virtio 1.0, 4.1.4.3.2) */
static void dev_reset( void )
{
    int i;

    st_set( 0 );
    if ( modern ) {
        for ( i = 0; i < 1000 && st_get() != 0; i++ ) net_delay_us( 10 );
    }
    isr_get();
}

/* the features: the station address, and on a modern device VERSION_1,
   which it then confirms with FEATURES_OK.  0 if the device will not */
static int features_set( void )
{
    uint32_t low, high;

    st_set( ST_ACKNOWLEDGE );
    st_set( ST_ACKNOWLEDGE | ST_DRIVER );
    if ( !modern ) {
        low = inl( net_port + VIO_HOST_FEATURES );
        if ( !( low & NET_F_MAC ) ) return 0;
        outl( net_port + VIO_GUEST_FEATURES, NET_F_MAC );
        return 1;
    }
    MMIO32( m_common, CC_DFSELECT ) = 0;
    low = MMIO32( m_common, CC_DF );
    MMIO32( m_common, CC_DFSELECT ) = 1;
    high = MMIO32( m_common, CC_DF );
    if ( !( low & NET_F_MAC ) || !( high & F_VERSION_1 ) ) return 0;
    MMIO32( m_common, CC_GFSELECT ) = 0;
    MMIO32( m_common, CC_GF ) = NET_F_MAC;
    MMIO32( m_common, CC_GFSELECT ) = 1;
    MMIO32( m_common, CC_GF ) = F_VERSION_1;
    st_set( ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK );
    return ( st_get() & ST_FEATURES_OK ) != 0;
}

/* ---------------------------------------------------------------- */
/* the modern device's blocks, from its capabilities                  */
/* ---------------------------------------------------------------- */

/* one block: its BAR, its place and its length, mapped; 0 if it cannot
   be reached */
static uint32_t cap_map( uint32_t bar, uint32_t offset, uint32_t length, uint32_t need )
{
    uint32_t base, lin;
    int isio;

    if ( bar > 5 || length < need ) return 0;
    base = net_pci_bar( net_pciaddr, (int)bar, &isio );
    if ( base == 0 || isio ) return 0;
    lin = net_pci_map( base + offset, length );
    if ( lin && !net_mem ) net_mem = base + offset;     /* for "found at" */
    return lin;
}

/* the capability list walked for the four blocks; 1 if every one is
   there and mapped */
static int caps_find( void )
{
    uint32_t at, kind, bar, offset, length;
    int guard;

    m_common = m_isr = m_device = m_notify = 0;
    net_mem = 0;
    if ( !( net_pci_cfg_read( net_pciaddr, 0x06, 2 ) & 0x10 ) ) return 0;    /* no list */
    at = net_pci_cfg_read( net_pciaddr, 0x34, 1 ) & 0xFC;
    for ( guard = 0; guard < 48 && at >= 0x40; guard++ ) {
        if ( net_pci_cfg_read( net_pciaddr, (int)at, 1 ) == PCI_CAP_VENDOR ) {
            kind = net_pci_cfg_read( net_pciaddr, (int)at + 3, 1 );
            bar = net_pci_cfg_read( net_pciaddr, (int)at + 4, 1 );
            offset = net_pci_cfg_read( net_pciaddr, (int)at + 8, 4 );
            length = net_pci_cfg_read( net_pciaddr, (int)at + 12, 4 );
            /* the first of each kind is the one to use */
            if ( kind == VCAP_COMMON && !m_common ) {
                m_common = cap_map( bar, offset, length, CC_LEN );
            } else if ( kind == VCAP_NOTIFY && !m_notify ) {
                m_notify = cap_map( bar, offset, length, 2 );
                m_notify_len = length;
                m_notify_mul = net_pci_cfg_read( net_pciaddr, (int)at + 16, 4 );
            } else if ( kind == VCAP_ISR && !m_isr ) {
                m_isr = cap_map( bar, offset, length, 1 );
            } else if ( kind == VCAP_DEVICE && !m_device ) {
                m_device = cap_map( bar, offset, length, 6 );
            }
        }
        at = net_pci_cfg_read( net_pciaddr, (int)at + 1, 1 ) & 0xFC;
    }
    return m_common && m_notify && m_isr && m_device;
}

/* ---------------------------------------------------------------- */
/* the queues                                                        */
/* ---------------------------------------------------------------- */

/* the bytes a queue of SIZE entries takes: descriptors and the avail
   ring, then the used ring from the next page */
static unsigned int vq_bytes( unsigned int size )
{
    return align4k( size * 16 + 6 + size * 2 ) + align4k( 6 + size * 8 );
}

static void vq_avail_put( VQUEUE *q, uint16_t head )
{
    q->avail[2 + ( q->avail_next % q->size )] = head;
    q->avail_next++;
    q->avail[1] = q->avail_next;                /* the device may take it now */
}

static volatile VUSED *vq_used_ring( VQUEUE *q ) { return (volatile VUSED *)( q->used + 2 ); }

/* a queue laid out in BLOCK and handed to the device; 0 if it cannot be */
static int vq_setup( int index, uint8_t *block )
{
    VQUEUE *q = &vq[index];
    unsigned int size;
    uint32_t off;

    size = queue_size( index );                 /* selects the queue too */
    /* a power of two, and no bigger than the block made at the load: a
       device that now says more would have its rings run off the end */
    if ( size == 0 || size > qcap[index] || ( size & ( size - 1 ) ) ) return 0;
    q->size = (uint16_t)size;
    net_memzero( block, vq_bytes( size ) );
    q->desc = (volatile VDESC *)block;
    q->avail = (volatile uint16_t *)( block + size * 16 );
    q->used = (volatile uint16_t *)( block + align4k( size * 16 + 6 + size * 2 ) );
    q->used_seen = 0;
    q->avail_next = 0;
    q->avail[0] = AVAIL_NO_INTERRUPT;
    if ( !modern ) {
        outl( net_port + VIO_QUEUE_PFN, (uint32_t)block >> 12 );
        return 1;
    }
    /* the queue's notify is at its own offset in the notify area, which
       must hold the two bytes written there */
    off = (uint32_t)MMIO16( m_common, CC_QNOTIFYOFF ) * m_notify_mul;
    if ( off > m_notify_len || m_notify_len - off < 2 ) return 0;
    q->notify = m_notify + off;
    MMIO32( m_common, CC_QDESC ) = (uint32_t)q->desc;
    MMIO32( m_common, CC_QDESC + 4 ) = 0;
    MMIO32( m_common, CC_QDRIVER ) = (uint32_t)q->avail;
    MMIO32( m_common, CC_QDRIVER + 4 ) = 0;
    MMIO32( m_common, CC_QDEVICE ) = (uint32_t)q->used;
    MMIO32( m_common, CC_QDEVICE + 4 ) = 0;
    MMIO16( m_common, CC_QENABLE ) = 1;
    return 1;
}

int card_find( void )
{
    const PCI_ID *which;
    int isio, i;
    unsigned int need;
    uint32_t at;
    uint8_t *pool;

    if ( !net_pci_find( vio_ids, &which ) ) { net_strcpy( net_name, "virtio-net" ); return 1; }
    net_pci_enable( net_pciaddr );
    net_irq = (uint8_t)net_pci_irqline( net_pciaddr );
    net_bus = NICB_PCI;
    net_flags = NICF_WORD;
    if ( which->device == 0x1041 ) {
        modern = 1;
        hdrlen = HDR_MODERN;
        net_port = 0;
        net_strcpy( net_name, "virtio-net 1.0" );
        /* mapped once: card_find runs again when a park comes back, and
           the blocks have not moved */
        if ( !m_common && !caps_find() ) {
            net_print( "virtio-net: the device's registers cannot be reached\r\n" );
            return 1;
        }
    } else {
        modern = 0;
        hdrlen = HDR_LEGACY;
        net_mem = 0;
        net_strcpy( net_name, "virtio-net" );
        net_port = (uint16_t)net_pci_bar( net_pciaddr, 0, &isio );
        if ( net_port == 0 || !isio ) {
            net_print( "virtio-net: no legacy I/O ports\r\n" );
            return 1;
        }
    }

    dev_reset();
    if ( !features_set() ) {
        net_print( "virtio-net: the device refused the features the driver needs\r\n" );
        st_set( ST_FAILED );
        return 1;
    }
    for ( i = 0; i < 6; i++ ) {
        net_mac[i] = modern ? MMIO8( m_device, i ) : inb( (uint16_t)( net_port + VIO_MAC + i ) );
    }
    if ( !net_mac_ok( net_mac ) ) return 1;

    if ( hdrs == 0 ) {
        /* each queue's size is the device's, read now; the blocks are
           kept for every later card_setup, so the sizes must not grow */
        for ( i = 0; i < 2; i++ ) {
            need = queue_size( i );
            if ( need == 0 || need > QMAX ) { net_print( "virtio-net: a queue the driver cannot use\r\n" ); return 1; }
            pool = net_alloc( vq_bytes( need ) + 4096 );
            if ( pool == 0 ) return 1;
            qmem[i] = (uint8_t *)align4k( (uint32_t)pool );
            qcap[i] = need;
        }
        pool = net_alloc( ( RXN + TXN ) * HDRSLOT + ( RXN + TXN ) * BUFSZ + 16 );
        if ( pool == 0 ) return 1;
        at = ( (uint32_t)pool + 15 ) & ~15L;
        hdrs = (uint8_t *)at;                at += ( RXN + TXN ) * HDRSLOT;
        for ( i = 0; i < RXN; i++ ) { rxbuf[i] = (uint8_t *)at; at += BUFSZ; }
        for ( i = 0; i < TXN; i++ ) { txbuf[i] = (uint8_t *)at; at += BUFSZ; }
    }
    return 0;
}

/* receive buffer N, as descriptors 2N (header) and 2N+1 (frame), on
   the avail ring */
static void rx_give( int entry )
{
    VQUEUE *q = &vq[VQ_RX];
    int head = entry * 2;

    q->desc[head].addr_lo = (uint32_t)( hdrs + entry * HDRSLOT );
    q->desc[head].addr_hi = 0;
    q->desc[head].len = hdrlen;
    q->desc[head].flags = DESC_WRITE | DESC_NEXT;
    q->desc[head].next = (uint16_t)( head + 1 );
    q->desc[head + 1].addr_lo = (uint32_t)rxbuf[entry];
    q->desc[head + 1].addr_hi = 0;
    q->desc[head + 1].len = BUFSZ;
    q->desc[head + 1].flags = DESC_WRITE;
    q->desc[head + 1].next = 0;
    vq_avail_put( q, (uint16_t)head );
}

int card_setup( void )
{
    int i;

    /* reset and start again: the device forgets its queues, so each
       card_setup lays them out from nothing */
    dev_reset();
    if ( !features_set() ) {
        net_print( "virtio-net: the device refused the features the driver needs\r\n" );
        st_set( ST_FAILED );
        return 1;
    }
    for ( i = 0; i < 2; i++ ) {
        if ( !vq_setup( i, qmem[i] ) ) {
            net_print( "virtio-net: the device refused a queue\r\n" );
            st_set( ST_FAILED );
            return 1;
        }
    }
    if ( vq[VQ_RX].size < RXN * 2 || vq[VQ_TX].size < TXN * 2 ) {
        net_print( "virtio-net: the queues are too small\r\n" );
        st_set( ST_FAILED );
        return 1;
    }
    for ( i = 0; i < RXN; i++ ) rx_give( i );
    for ( i = 0; i < TXN; i++ ) tx_busy[i] = 0;
    tx_free = TXN;
    vq[VQ_TX].avail[0] = AVAIL_NO_INTERRUPT;    /* finished frames are taken back on the next send */

    st_set( (uint8_t)( ST_ACKNOWLEDGE | ST_DRIVER | ST_DRIVER_OK | ( modern ? ST_FEATURES_OK : 0 ) ) );
    notify( VQ_RX );                            /* the buffers are there */
    isr_get();                                  /* nothing old left to ask */
    vq[VQ_RX].avail[0] = (uint16_t)( net_flags & NICF_POLLED ? AVAIL_NO_INTERRUPT : 0 );
    return 0;
}

void card_stop( void )
{
    dev_reset();                                /* the queues are dropped */
}

/* the transmit buffers the device has finished with, free again */
static void tx_reclaim( void )
{
    VQUEUE *q = &vq[VQ_TX];
    uint32_t id;

    while ( q->used_seen != q->used[1] ) {
        id = vq_used_ring( q )[q->used_seen % q->size].id;
        q->used_seen++;
        if ( id < TXN * 2 && !( id & 1 ) && tx_busy[id / 2] ) {
            tx_busy[id / 2] = 0;
            tx_free++;
        }
    }
}

int card_send( const uint8_t *frame, unsigned int len )
{
    VQUEUE *q = &vq[VQ_TX];
    uint8_t *hdr;
    unsigned int n;
    int entry, head;

    tx_reclaim();
    for ( n = 0; n < 400000L && tx_free == 0; n++ ) tx_reclaim();
    if ( tx_free == 0 ) { net_stats.txerrors++; return 0; }
    for ( entry = 0; entry < TXN && tx_busy[entry]; entry++ ) ;
    if ( entry == TXN ) { net_stats.txerrors++; return 0; }
    if ( len < 60 ) {
        net_memzero( txbuf[entry], 60 );        /* the host does not pad */
        net_memcpy( txbuf[entry], frame, len );
        len = 60;
    } else {
        net_memcpy( txbuf[entry], frame, len );
    }
    hdr = hdrs + ( RXN + entry ) * HDRSLOT;
    net_memzero( hdr, hdrlen );
    head = entry * 2;
    q->desc[head].addr_lo = (uint32_t)hdr;
    q->desc[head].addr_hi = 0;
    q->desc[head].len = hdrlen;
    q->desc[head].flags = DESC_NEXT;
    q->desc[head].next = (uint16_t)( head + 1 );
    q->desc[head + 1].addr_lo = (uint32_t)txbuf[entry];
    q->desc[head + 1].addr_hi = 0;
    q->desc[head + 1].len = len;
    q->desc[head + 1].flags = 0;
    q->desc[head + 1].next = 0;
    tx_busy[entry] = 1;
    tx_free--;
    vq_avail_put( q, (uint16_t)head );
    notify( VQ_TX );
    return 1;
}

/* the frames the device has written, handed over; how many */
static int rx_drain( void )
{
    VQUEUE *q = &vq[VQ_RX];
    volatile VUSED *used;
    uint32_t id;
    unsigned int len;
    int got = 0, entry;
    uint8_t *slot;

    while ( q->used_seen != q->used[1] && got < RXN ) {
        used = &vq_used_ring( q )[q->used_seen % q->size];
        id = used->id;
        len = used->len;                        /* the header and the frame */
        q->used_seen++;
        got++;
        if ( id >= RXN * 2 || ( id & 1 ) ) { net_stats.rxerrors++; continue; }
        entry = (int)( id / 2 );
        if ( len >= hdrlen + NIC_HDRLEN && len <= hdrlen + NIC_MAXFRAME ) {
            slot = net_rx_begin();
            net_memcpy( slot, rxbuf[entry], len - hdrlen );
            net_rx_commit( len - hdrlen );
        } else {
            net_stats.rxerrors++;
        }
        rx_give( entry );
    }
    if ( got ) notify( VQ_RX );
    return got;
}

int card_service( void )
{
    uint8_t isr;
    int got;

    isr = isr_get();                            /* reading it drops the line */
    got = rx_drain();
    return ( isr & 1 ) != 0 || got != 0;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'v', 'i', 'r', 't', 'i', 'o', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
