/*
 * VMXNET3.C - VMXNET3.SYS, a driver for VMware's vmxnet3 network device
 *
 * The paravirtual card of VMware Workstation, Player, Fusion and ESXi
 * (PCI 15AD:07B0), which QEMU also offers as "vmxnet3".  Like virtio
 * there is no chip: the driver and the host share rings in the guest's
 * memory, and two small memory BARs carry the rest.
 *
 * BAR0, the "passthrough" registers: the interrupt mask of each vector
 * at 0 + 8n (1 masks, 0 unmasks), the transmit producer at 600h and the
 * receive producers at 800h and A00h - written with the next free index
 * after descriptors are handed over.  BAR1, the "virtual device"
 * registers: the revision and UPT version (read what is offered, write
 * the one chosen - 1 here) at 0 and 8, the DriverShared block's address
 * at 10h and 18h, the command register at 20h (write a command, read
 * its answer back), the station address at 28h and 30h, the interrupt
 * cause at 38h (reading it acknowledges the INTx line) and the event
 * acknowledge at 40h.  All of them are 32-bit.
 *
 * DriverShared (720 bytes, Vmxnet3_DriverShared) holds the driver's
 * versions, the MTU, the queue counts and where the queue descriptors
 * are, the interrupt setup, the receive filter (unicast, broadcast, all
 * multicast; VLAN 0 allowed so untagged frames pass) and the event
 * register the device sets.  The queue descriptors (256 bytes each, the
 * transmit one then the receive one) say where each ring is.
 *
 * ONE QUEUE EACH WAY, as iPXE runs the card: 32 transmit descriptors
 * and their 32 completions, 32 receive descriptors in ring 0 (ring 1,
 * for the bodies of frames larger than a buffer, is not used: every
 * buffer holds a whole frame) and their 32 completions.  Every
 * descriptor carries a GENERATION bit; the side that fills a ring flips
 * the bit it writes each time round, so the other side tells a new
 * entry from an old one without an index register.  A transmit
 * descriptor's generation is written last, after its other fields.
 *
 * INTERRUPTS: one vector, INTx (MSI and MSI-X stay off), automatic
 * masking off; the vector is masked while card_setup runs and unmasked
 * at its end unless the card is polled.  The device is told of /POLL
 * too (DISABLE_ALL in the interrupt control).  A queue error (the event
 * register's RQERR or TQERR: the device has stopped that queue) is
 * answered by setting the card up again.
 *
 * Written from QEMU's hw/net/vmxnet3.c (the model it is tested on),
 * iPXE's drivers/net/vmxnet3.c (the same single-queue shape, run on real
 * VMware), and Linux's and FreeBSD's vmxnet3 headers for the layouts.
 * On the shared base (DRV\NETDRV.H, NETCORE.C).  Proved on QEMU's
 * vmxnet3, pc and q35 machines.
 */
#include "netdrv.h"

/* BAR0 */
#define PT_IMR          0x000           /* vector 0's mask */
#define PT_TXPROD       0x600
#define PT_RXPROD       0x800
#define PT_LEN          0x1000

/* BAR1 */
#define VD_VRRS         0x000
#define VD_UVRS         0x008
#define VD_DSAL         0x010
#define VD_DSAH         0x018
#define VD_CMD          0x020
#define VD_MACL         0x028
#define VD_MACH         0x030
#define VD_ICR          0x038
#define VD_ECR          0x040
#define VD_LEN          0x1000

#define CMD_ACTIVATE    0xCAFE0000L
#define CMD_QUIESCE     0xCAFE0001L
#define CMD_RESET       0xCAFE0002L
#define CMD_GET_LINK    0xF00D0002L
#define CMD_GET_MACL    0xF00D0003L
#define CMD_GET_MACH    0xF00D0004L

#define ECR_RQERR       0x01
#define ECR_TQERR       0x02

/* DriverShared */
#define DS_MAGIC        0               /* VMXNET3_REV1_MAGIC */
#define DS_VERSION      8               /* the driver's own */
#define DS_GUEST        12              /* bits 0-1: 1 = a 32-bit guest */
#define DS_REVSPT       16
#define DS_UPTSPT       20
#define DS_QDESC_PA     40              /* 64 */
#define DS_QDESC_LEN    52
#define DS_MTU          56
#define DS_MAXRXSG      60              /* 16 */
#define DS_NTXQ         62              /* 8 */
#define DS_NRXQ         63              /* 8 */
#define DS_AUTOMASK     80              /* 8 */
#define DS_NINTR        81              /* 8 */
#define DS_EVINTR       82              /* 8 */
#define DS_ICTRL        108
#define DS_RXMODE       120
#define DS_VLANTAB      136             /* 512 bytes, a bit for each VLAN */
#define DS_ECR          696
#define DS_LEN          720

#define DS_MAGIC_VAL    0xBABEFEE1L
#define IC_DISABLE_ALL  0x01
#define RXM_UCAST       0x01
#define RXM_BCAST       0x04
#define RXM_ALLMULTI    0x08

/* the queue descriptors: transmit at 0, receive at 256 */
#define QD_TX           0
#define QD_RX           256
#define QD_LEN          512
#define TQ_RING_PA      16              /* 64 */
#define TQ_COMP_PA      32              /* 64 */
#define TQ_RING_LEN     56
#define TQ_COMP_LEN     64
#define TQ_INTR         72              /* 8 */
#define RQ_RING0_PA     16              /* 64 */
#define RQ_COMP_PA      32              /* 64 */
#define RQ_RING0_LEN    56
#define RQ_COMP_LEN     64
#define RQ_INTR         72              /* 8 */

/* the descriptors: 16 bytes, an address and two words */
#define TXF_GEN         0x00004000L     /* word 0, with the length */
#define TXF_EOP         0x00001000L     /* word 1 */
#define TXF_CQ          0x00002000L     /* word 1: a completion wanted */
#define RXF_GEN         0x80000000L     /* word 0, with the length */
#define CF_GEN          0x80000000L     /* a completion's last word */
#define RXC_EOP         0x00004000L     /* a receive completion's word 0 */
#define RXC_SOP         0x00008000L
#define RXC_ERR         0x00004000L     /* its length word */

#define NDESC           32              /* every ring; a power of two */
#define TXBUF           8               /* frames in flight */
#define BUFSZ           1536

typedef struct { u32 addr_lo, addr_hi, word0, word1; } VDESC;

static u32  pt, vd;                     /* the BARs, mapped */
static u8  *block;                      /* the rings, the queues, DriverShared */
static volatile VDESC *tx_ring, *tx_comp, *rx_ring, *rx_comp;
static volatile u8 *qdesc;
static volatile u8 *shared;
static u8  *rxbuf[NDESC];
static u8  *txbuf[TXBUF];
static u32  tx_prod, tx_cons;           /* counted, not wrapped */
static u32  rx_prod, rx_cons;
static u8   restart;                    /* a queue error: set up again */

static const PCI_ID vmx_ids[] = {
    { 0x15AD, 0x07B0 },                 /* VMware vmxnet3 */
    { 0, 0 }
};

static void w32( volatile u8 *at, u32 value ) { *(volatile u32 *)at = value; }

static u32 command( u32 cmd )
{
    MMIO32( vd, VD_CMD ) = cmd;
    return MMIO32( vd, VD_CMD );
}

/* the interrupt cause: nonzero if the line was the card's, which the
   read drops */
static u32 icr_take( void ) { return MMIO32( vd, VD_ICR ); }

/* the generation a ring's Nth entry carries: 1 the first time round */
static u32 gen_of( u32 count, u32 bit ) { return ( count & NDESC ) ? 0 : bit; }

int card_find( void )
{
    const PCI_ID *which;
    int isio;
    u32 base, low, high, at;
    int i;

    net_strcpy( net_name, "VMware vmxnet3" );
    if ( !net_pci_find( vmx_ids, &which ) ) return 1;
    net_pci_enable( net_pciaddr );
    net_irq = (u8)net_pci_irqline( net_pciaddr );
    net_bus = NICB_PCI;
    net_flags = NICF_WORD;              /* as VIRTIO.SYS says it: no ISA width applies */
    net_port = 0;

    if ( pt == 0 ) {
        base = net_pci_bar( net_pciaddr, 0, &isio );
        if ( base == 0 || isio ) { net_print( "vmxnet3: no register memory\r\n" ); return 1; }
        pt = net_pci_map( base, PT_LEN );
        base = net_pci_bar( net_pciaddr, 1, &isio );
        if ( base == 0 || isio ) { net_print( "vmxnet3: no register memory\r\n" ); return 1; }
        net_mem = base;
        vd = net_pci_map( base, VD_LEN );
        if ( pt == 0 || vd == 0 ) { net_print( "vmxnet3: the registers cannot be reached\r\n" ); return 1; }
    }

    /* revision 1 and UPT version 1, the ones every vmxnet3 has */
    if ( !( MMIO32( vd, VD_VRRS ) & 1 ) || !( MMIO32( vd, VD_UVRS ) & 1 ) ) {
        net_print( "vmxnet3: the device does not offer revision 1\r\n" );
        return 1;
    }
    MMIO32( vd, VD_VRRS ) = 1;
    MMIO32( vd, VD_UVRS ) = 1;
    command( CMD_RESET );

    low = command( CMD_GET_MACL );
    high = command( CMD_GET_MACH );
    for ( i = 0; i < 4; i++ ) net_mac[i] = (u8)( low >> ( i * 8 ) );
    net_mac[4] = (u8)high;
    net_mac[5] = (u8)( high >> 8 );
    if ( !net_mac_ok( net_mac ) ) return 1;

    if ( block == 0 ) {
        /* four rings of 512 bytes on 512-byte boundaries, the queue
           descriptors (on 128) and DriverShared, then the buffers */
        at = (u32)net_alloc( 4 * 512 + QD_LEN + DS_LEN + 512 + ( NDESC + TXBUF ) * BUFSZ );
        if ( at == 0 ) return 1;
        at = ( at + 511 ) & ~511L;
        block = (u8 *)at;
        tx_ring = (volatile VDESC *)( block );
        tx_comp = (volatile VDESC *)( block + 512 );
        rx_ring = (volatile VDESC *)( block + 1024 );
        rx_comp = (volatile VDESC *)( block + 1536 );
        qdesc = block + 2048;
        shared = block + 2048 + QD_LEN;
        at += 2048 + QD_LEN + DS_LEN;
        for ( i = 0; i < NDESC; i++ ) { rxbuf[i] = (u8 *)at; at += BUFSZ; }
        for ( i = 0; i < TXBUF; i++ ) { txbuf[i] = (u8 *)at; at += BUFSZ; }
    }
    return 0;
}

/* receive descriptors handed over up to a full ring, and the producer
   told */
static void rx_fill( void )
{
    volatile VDESC *desc;
    u32 index, start = rx_prod;

    while ( rx_prod - rx_cons < NDESC ) {
        index = rx_prod % NDESC;
        desc = &rx_ring[index];
        desc->addr_lo = (u32)rxbuf[index];
        desc->addr_hi = 0;
        desc->word1 = 0;
        desc->word0 = gen_of( rx_prod, RXF_GEN ) | BUFSZ;   /* a head buffer */
        rx_prod++;
    }
    if ( rx_prod != start ) MMIO32( pt, PT_RXPROD ) = rx_prod % NDESC;
}

int card_setup( void )
{
    u32 status;

    MMIO32( pt, PT_IMR ) = 1;                   /* masked until the end */
    command( CMD_QUIESCE );
    command( CMD_RESET );
    icr_take();                                 /* nothing old left asserted */

    net_memzero( block, 2048 + QD_LEN + DS_LEN );
    tx_prod = tx_cons = rx_prod = rx_cons = 0;
    restart = 0;

    w32( qdesc + QD_TX + TQ_RING_PA, (u32)tx_ring );
    w32( qdesc + QD_TX + TQ_COMP_PA, (u32)tx_comp );
    w32( qdesc + QD_TX + TQ_RING_LEN, NDESC );
    w32( qdesc + QD_TX + TQ_COMP_LEN, NDESC );
    qdesc[QD_TX + TQ_INTR] = 0;
    w32( qdesc + QD_RX + RQ_RING0_PA, (u32)rx_ring );
    w32( qdesc + QD_RX + RQ_COMP_PA, (u32)rx_comp );
    w32( qdesc + QD_RX + RQ_RING0_LEN, NDESC );
    w32( qdesc + QD_RX + RQ_COMP_LEN, NDESC );
    qdesc[QD_RX + RQ_INTR] = 0;

    w32( shared + DS_MAGIC, DS_MAGIC_VAL );
    w32( shared + DS_VERSION, 0x01000000L );
    w32( shared + DS_GUEST, 1 );                /* 32 bits */
    w32( shared + DS_REVSPT, 1 );
    w32( shared + DS_UPTSPT, 1 );
    w32( shared + DS_QDESC_PA, (u32)qdesc );
    w32( shared + DS_QDESC_LEN, QD_LEN );
    w32( shared + DS_MTU, 1500 );
    *(volatile u16 *)( shared + DS_MAXRXSG ) = 1;
    shared[DS_NTXQ] = 1;
    shared[DS_NRXQ] = 1;
    shared[DS_AUTOMASK] = 0;
    shared[DS_NINTR] = 1;
    shared[DS_EVINTR] = 0;
    w32( shared + DS_ICTRL, net_flags & NICF_POLLED ? IC_DISABLE_ALL : 0 );
    w32( shared + DS_RXMODE, RXM_UCAST | RXM_BCAST | RXM_ALLMULTI );
    shared[DS_VLANTAB] = 1;                     /* VLAN 0: untagged frames */

    MMIO32( vd, VD_DSAL ) = (u32)shared;
    MMIO32( vd, VD_DSAH ) = 0;
    status = command( CMD_ACTIVATE );
    if ( status != 0 ) {
        net_print( "vmxnet3: the device would not start\r\n" );
        return 1;
    }
    rx_fill();
    /* said at the load only: card_service sets the card up again after a
       queue error, from the interrupt */
    if ( net_line && !( command( CMD_GET_LINK ) & 1 ) ) net_print( "vmxnet3: no link\r\n" );

    icr_take();                                 /* acknowledged, then unmasked */
    if ( !( net_flags & NICF_POLLED ) ) MMIO32( pt, PT_IMR ) = 0;
    return 0;
}

void card_stop( void )
{
    MMIO32( pt, PT_IMR ) = 1;
    command( CMD_QUIESCE );
    command( CMD_RESET );
    icr_take();
}

/* the transmit completions, taken: each frees the oldest frame in
   flight (the device finishes them in order) */
static void tx_reclaim( void )
{
    while ( tx_cons != tx_prod &&
            ( tx_comp[tx_cons % NDESC].word1 & CF_GEN ) == gen_of( tx_cons, CF_GEN ) ) {
        tx_cons++;
    }
}

int card_send( const u8 *frame, u32 len )
{
    volatile VDESC *desc;
    u8 *buf;
    u32 n, index;

    tx_reclaim();
    for ( n = 0; n < 400000L && tx_prod - tx_cons >= TXBUF; n++ ) tx_reclaim();
    if ( tx_prod - tx_cons >= TXBUF ) { net_stats.txerrors++; return 0; }
    buf = txbuf[tx_prod % TXBUF];
    if ( len < 60 ) {
        net_memzero( buf, 60 );                 /* the host does not pad */
        net_memcpy( buf, frame, len );
        len = 60;
    } else {
        net_memcpy( buf, frame, len );
    }
    index = tx_prod % NDESC;
    desc = &tx_ring[index];
    desc->addr_lo = (u32)buf;
    desc->addr_hi = 0;
    desc->word1 = TXF_EOP | TXF_CQ;
    desc->word0 = gen_of( tx_prod, TXF_GEN ) | len;     /* the device may take it now */
    tx_prod++;
    MMIO32( pt, PT_TXPROD ) = tx_prod % NDESC;
    return 1;
}

/* the frames the device has written, handed over; how many */
static int rx_drain( void )
{
    volatile VDESC *comp;
    u32 index, len;
    int got = 0;
    u8 *slot;

    for ( ;; ) {
        comp = &rx_comp[rx_cons % NDESC];
        if ( ( comp->word1 & CF_GEN ) != gen_of( rx_cons, CF_GEN ) ) break;
        index = comp->addr_lo & 0xFFF;          /* rxdIdx; the queue is ring 0's */
        len = comp->word0 & 0x3FFF;
        if ( ( comp->addr_lo & ( RXC_SOP | RXC_EOP ) ) == ( RXC_SOP | RXC_EOP ) &&
             !( comp->word0 & RXC_ERR ) && ( ( comp->addr_lo >> 16 ) & 0x3FF ) == 0 &&
             index < NDESC && len >= NIC_HDRLEN && len <= NIC_MAXFRAME ) {
            slot = net_rx_begin();
            net_memcpy( slot, rxbuf[index], len );
            net_rx_commit( len );
        } else {
            net_stats.rxerrors++;
        }
        rx_cons++;
        got++;
    }
    if ( got ) rx_fill();
    return got;
}

int card_service( void )
{
    u32 icr, events;
    int got;

    icr = icr_take();                          /* reading it drops the line */
    events = *(volatile u32 *)( shared + DS_ECR );
    if ( events ) {
        MMIO32( vd, VD_ECR ) = events;
        if ( events & ( ECR_RQERR | ECR_TQERR ) ) {
            net_stats.rxerrors++;
            restart = 1;
        }
    }
    if ( restart ) {
        card_setup();
        return 1;
    }
    got = rx_drain();
    return icr != 0 || events != 0 || got != 0;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'v', 'm', 'x', 'n', 'e', 't', '3', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
