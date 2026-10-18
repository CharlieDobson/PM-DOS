/*
 * E1000.C - E1000.SYS, a driver for the Intel 8254x gigabit family
 *
 * Intel's 82540/82545/82574 and the cards built on them - "e1000", the
 * card QEMU offers by that name and the one a hundred servers and
 * virtual machines use.  A PCI bus-master whose registers are MEMORY
 * mapped, not in a port, and whose rings are the cleanest of the lot:
 * a head and a tail register each, the chip filling from the head and
 * the driver releasing at the tail.
 *
 * THE REGISTERS are reached through the card's memory, which the
 * kernel maps for the driver (net_pci_map); the identity map means the
 * address is the physical one.  THE STATION ADDRESS the chip has
 * already loaded from its EEPROM into the receive-address registers,
 * so it is read from there without touching the EEPROM at all.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).  86Box has no card of
 * this family; this is proved on QEMU.
 */
#include "netdrv.h"

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200      /* 128 dwords */
#define REG_RAL     0x5400
#define REG_RAH     0x5404

#define CTRL_SLU    0x00000040L /* set link up */
#define CTRL_ASDE   0x00000020L /* auto-speed detect */
#define CTRL_RST    0x04000000L /* reset */

#define RCTL_EN     0x00000002L
#define RCTL_BAM    0x00008000L /* accept broadcast */
#define RCTL_SECRC  0x04000000L /* strip the ethernet CRC */
#define RCTL_BSIZE  0x00000000L /* 2048-byte buffers */

#define TCTL_EN     0x00000002L
#define TCTL_PSP    0x00000008L /* pad short packets */
#define TCTL_CT     0x000000F0L /* collision threshold 15 */
#define TCTL_COLD   0x00040000L /* collision distance 64 (half duplex) */

#define RAH_AV      0x80000000L /* the address is valid */

#define ICR_RXT0    0x00000080L /* a receive timer went off: frames waiting */
#define ICR_RXO     0x00000040L /* receive overrun */
#define ICR_RXDMT0  0x00000010L /* receive descriptors low */

#define TXD_EOP     0x01        /* end of packet */
#define TXD_IFCS    0x02        /* insert the FCS */
#define TXD_RS      0x08        /* report status */
#define XD_DD       0x01        /* descriptor done */
#define RXD_EOP     0x02        /* end of packet */

#define RXN     8               /* 8 * 16 = 128 bytes, RDLEN's unit */
#define TXN     8
#define BUFSZ   2048

typedef struct { u32 addr_lo, addr_hi; u16 length, csum; u8 status, errors; u16 special; } RXD;
typedef struct { u32 addr_lo, addr_hi; u16 length; u8 cso, cmd, status, css; u16 special; } TXD;

static u32  membase;
static volatile RXD *rxd;
static volatile TXD *txd;
static u8  *rxbuf[RXN];
static u8  *txbuf[TXN];
static int  rx_cur, tx_cur;

static const PCI_ID e1000_ids[] = {
    { 0x8086, 0x100E },         /* 82540EM - QEMU's e1000 */
    { 0x8086, 0x1004 },         /* 82543 */
    { 0x8086, 0x100F },         /* 82545EM */
    { 0x8086, 0x10D3 },         /* 82574L - e1000e */
    { 0x8086, 0x1010 },         /* 82546EB */
    { 0, 0 }
};

static u32 align16( u32 p ) { return ( p + 15 ) & ~15L; }
static u32 reg_rd( u32 off ) { return MMIO32( membase, off ); }
static void reg_wr( u32 off, u32 value ) { MMIO32( membase, off ) = value; }

int card_find( void )
{
    const PCI_ID *which;
    int isio, i;
    u32 at, need, bar, ral, rah;
    u8 *pool;

    if ( !net_pci_find( e1000_ids, &which ) ) return 1;
    net_pci_enable( net_pciaddr );
    bar = net_pci_bar( net_pciaddr, 0, &isio );     /* BAR0 is memory */
    if ( bar == 0 || isio ) return 1;
    membase = net_pci_map( bar, 0x20000 );
    if ( membase == 0 ) return 1;
    net_irq = (u8)net_pci_irqline( net_pciaddr );
    net_bus = NICB_PCI;
    net_flags = NICF_WORD;

    /* reset, then read the address the chip loaded from its EEPROM */
    reg_wr( REG_CTRL, reg_rd( REG_CTRL ) | CTRL_RST );
    for ( i = 0; i < 100000; i++ ) { if ( !( reg_rd( REG_CTRL ) & CTRL_RST ) ) break; }
    reg_wr( REG_IMC, 0xFFFFFFFFL );                 /* no interrupts yet */
    reg_rd( REG_ICR );

    ral = reg_rd( REG_RAL );
    rah = reg_rd( REG_RAH );
    if ( !( rah & RAH_AV ) ) return 1;
    net_mac[0] = (u8)ral;        net_mac[1] = (u8)( ral >> 8 );
    net_mac[2] = (u8)( ral >> 16 ); net_mac[3] = (u8)( ral >> 24 );
    net_mac[4] = (u8)rah;        net_mac[5] = (u8)( rah >> 8 );
    if ( !net_mac_ok( net_mac ) ) return 1;

    if ( rxd == 0 ) {
        need = RXN * 16 + TXN * 16 + ( RXN + TXN ) * BUFSZ + 16;
        pool = net_alloc( need );
        if ( pool == 0 ) return 1;
        at = align16( (u32)pool );
        rxd = (volatile RXD *)at;   at += RXN * 16;
        txd = (volatile TXD *)at;   at += TXN * 16;
        for ( i = 0; i < RXN; i++ ) { rxbuf[i] = (u8 *)at; at += BUFSZ; }
        for ( i = 0; i < TXN; i++ ) { txbuf[i] = (u8 *)at; at += BUFSZ; }
    }
    net_strcpy( net_name, "Intel 8254x (e1000)" );
    return 0;
}

int card_setup( void )
{
    int i;

    reg_wr( REG_CTRL, reg_rd( REG_CTRL ) | CTRL_SLU | CTRL_ASDE );
    for ( i = 0; i < 128; i++ ) reg_wr( REG_MTA + i * 4, 0 );

    for ( i = 0; i < RXN; i++ ) {
        rxd[i].addr_lo = (u32)rxbuf[i];
        rxd[i].addr_hi = 0;
        rxd[i].status = 0;
    }
    reg_wr( REG_RDBAL, (u32)rxd );
    reg_wr( REG_RDBAH, 0 );
    reg_wr( REG_RDLEN, RXN * 16 );
    reg_wr( REG_RDH, 0 );
    reg_wr( REG_RDT, RXN - 1 );
    rx_cur = 0;
    reg_wr( REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE );

    for ( i = 0; i < TXN; i++ ) {
        txd[i].addr_lo = (u32)txbuf[i];
        txd[i].addr_hi = 0;
        txd[i].status = XD_DD;                   /* free */
    }
    reg_wr( REG_TDBAL, (u32)txd );
    reg_wr( REG_TDBAH, 0 );
    reg_wr( REG_TDLEN, TXN * 16 );
    reg_wr( REG_TDH, 0 );
    reg_wr( REG_TDT, 0 );
    tx_cur = 0;
    reg_wr( REG_TIPG, 0x0060200AL );
    reg_wr( REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD );

    reg_rd( REG_ICR );
    if ( net_flags & NICF_POLLED ) reg_wr( REG_IMC, 0xFFFFFFFFL );
    else reg_wr( REG_IMS, ICR_RXT0 | ICR_RXO | ICR_RXDMT0 );
    return 0;
}

void card_stop( void )
{
    reg_wr( REG_IMC, 0xFFFFFFFFL );
    reg_wr( REG_RCTL, 0 );
    reg_wr( REG_TCTL, 0 );
}

int card_send( const u8 *frame, u32 len )
{
    int entry = tx_cur;
    u32 n;

    if ( !( txd[entry].status & XD_DD ) ) {
        for ( n = 0; n < 400000L; n++ ) {
            if ( txd[entry].status & XD_DD ) break;
        }
        if ( !( txd[entry].status & XD_DD ) ) { net_stats.txerrors++; return 0; }
    }
    net_memcpy( txbuf[entry], frame, len );
    txd[entry].addr_lo = (u32)txbuf[entry];
    txd[entry].addr_hi = 0;
    txd[entry].length = (u16)len;
    txd[entry].cso = 0;
    txd[entry].cmd = TXD_EOP | TXD_IFCS | TXD_RS;
    txd[entry].status = 0;
    tx_cur = ( entry + 1 ) % TXN;
    reg_wr( REG_TDT, tx_cur );
    return 1;
}

static void rx_drain( void )
{
    u32 len, n;
    int last = -1;
    u8 *slot;

    for ( n = 0; n < RXN * 2; n++ ) {
        if ( !( rxd[rx_cur].status & XD_DD ) ) break;
        len = rxd[rx_cur].length;               /* the CRC is already stripped */
        if ( ( rxd[rx_cur].status & RXD_EOP ) && len >= NIC_HDRLEN && len <= NIC_MAXFRAME ) {
            slot = net_rx_begin();
            net_memcpy( slot, rxbuf[rx_cur], len );
            net_rx_commit( len );
        } else {
            net_stats.rxerrors++;
        }
        rxd[rx_cur].status = 0;
        last = rx_cur;
        rx_cur = ( rx_cur + 1 ) % RXN;
    }
    if ( last >= 0 ) reg_wr( REG_RDT, (u32)last );
}

int card_service( void )
{
    u32 icr;

    icr = reg_rd( REG_ICR );                     /* reading it clears it */
    if ( icr == 0 ) return 0;
    if ( icr & ( ICR_RXT0 | ICR_RXO | ICR_RXDMT0 ) ) rx_drain();
    return 1;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'e', '1', '0', '0', '0', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
