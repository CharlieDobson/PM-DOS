/*
 * IGB.C - IGB.SYS, a driver for Intel's 82575 family and the I225/I226
 *
 * The chips that came after the PRO/1000 (E1000.SYS): the 82575, 82576
 * and 82580 server parts, the I350 and I354, the I210 and I211 that are
 * on boards and cards from 2013 on, and their 2.5 gigabit successors the
 * I225 and I226 (Intel's "igc").  The registers a driver needs are
 * still the PRO/1000's - CTRL, STATUS, RCTL, TCTL, the receive address
 * and the multicast table where they always were - but the rings moved
 * into per-queue blocks, and each queue has to be switched on by itself
 * (RXDCTL/TXDCTL.ENABLE, read back before the queue is used).  Where the
 * blocks are differs: the 82575..I211 keep queue 0 where the 8254x had
 * its rings (2800h, 3800h) and the interrupt cause at C0h; the I225 and
 * I226 have them at C000h/E000h and 1500h.  That is the GROUP table.
 *
 * ADVANCED DESCRIPTORS, one buffer each, both ways: the "legacy" ring
 * format is gone from the I225, and QEMU's 82576 does not take it for
 * receive either.  A receive descriptor is handed out as a buffer
 * address and comes back with the address overwritten by the status,
 * the errors and the length, so it is written whole again each time it
 * is given back.
 *
 * RESET: the PCI Express master is stopped first (GIO_MASTER_DISABLE,
 * waited for), as Intel's own code does, so no transaction is cut in
 * half; then the global reset, and the NVM is waited for (EECD.AUTO_RD)
 * - after it the station address is in RAL0/RAH0.  Then, on a copper
 * part, negotiation is started again through the PHY's control register
 * (MDIC), as Intel's drivers all do after a reset; the PHY is claimed
 * from the management firmware first (SWSM, then SW_FW_SYNC), since a
 * board with a BMC shares it.  QEMU's 82576 keeps its link down until
 * this is done.  A PHY powered down by another system before a warm
 * reboot is not powered up again here; that machine says "no link".
 *
 * INTERRUPTS: the legacy line, with queue 0's receive and transmit
 * causes given to vector 0 (IVAR0; the 82575 has MSIXBM instead) as
 * Intel's drivers do when there is no MSI-X, and ICR's receive bits
 * enabled.  The clock drains the ring as well, as for every card.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).  Proved on QEMU's igb
 * (82576), on the pc and q35 machines.  The other chips, and the I225
 * and I226 above all, have not been run.
 */
#include "netdrv.h"

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EECD    0x0010
#define REG_CTRL_EXT 0x0018
#define REG_MDIC    0x0020
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_MSIXBM0 0x1600      /* 82575 */
#define REG_IVAR0   0x1700      /* the rest */
#define REG_MTA     0x5200      /* 128 dwords */
#define REG_RAL     0x5400
#define REG_RAH     0x5404
#define REG_SWSM    0x5B50      /* software semaphore */
#define REG_SW_FW_SYNC 0x5B5C   /* who has the PHY: software or firmware */

/* inside a queue's block (GROUP.rxq, GROUP.txq) */
#define Q_BAL       0x00
#define Q_BAH       0x04
#define Q_LEN       0x08
#define Q_SRRCTL    0x0C        /* receive only */
#define Q_HEAD      0x10
#define Q_TAIL      0x18
#define Q_DCTL      0x28

#define CTRL_GIO_MASTER_DISABLE 0x00000004L
#define CTRL_SLU    0x00000040L
#define CTRL_FRCSPD 0x00000800L
#define CTRL_FRCDPX 0x00001000L
#define CTRL_RST    0x04000000L

#define CTRL_EXT_DRV_LOAD 0x10000000L

#define STATUS_FD   0x00000001L
#define STATUS_LU   0x00000002L
#define STATUS_SPEED_100  0x00000040L
#define STATUS_SPEED_1000 0x00000080L
#define STATUS_SPEED_2500 0x00400000L   /* I225/I226 */
#define STATUS_GIO_MASTER 0x00080000L

#define EECD_AUTO_RD 0x00000200L

#define RCTL_EN     0x00000002L
#define RCTL_MPE    0x00000010L
#define RCTL_BAM    0x00008000L
#define RCTL_SECRC  0x04000000L

#define TCTL_EN     0x00000002L
#define TCTL_PSP    0x00000008L
#define TCTL_CT     0x000000F0L
#define TCTL_COLD   0x00040000L
#define TCTL_RTLC   0x01000000L

#define RAH_AV      0x80000000L

#define SRRCTL_2K       0x00000002L     /* the buffer, in kilobytes */
#define SRRCTL_ONEBUF   0x02000000L     /* advanced, one buffer */
#define DCTL_ENABLE     0x02000000L
/* prefetch 8, host 8 (1 for transmit), write-back 1: every descriptor is
   written back as soon as it is done, so the done bit can be waited on */
#define RXDCTL_THRESH   ( 8L | ( 8L << 8 ) | ( 1L << 16 ) )
#define TXDCTL_THRESH   ( 8L | ( 1L << 8 ) | ( 1L << 16 ) )

#define ICR_RXDMT0  0x00000010L
#define ICR_RXO     0x00000040L
#define ICR_RXDW    0x00000080L

#define IVAR_VALID  0x80L

#define SWSM_SMBI   0x00000001L
#define SWSM_SWESMBI 0x00000002L
#define SWFW_PHY0   0x00000002L         /* port 0's PHY; firmware's is << 16 */

#define MDIC_REG_SHIFT 16
#define MDIC_PHY_SHIFT 21
#define MDIC_WRITE  0x04000000L
#define MDIC_READ   0x08000000L
#define MDIC_READY  0x10000000L
#define MDIC_ERROR  0x40000000L

#define BMCR_ANRESTART 0x0200           /* the PHY's control register, 0 */
#define BMCR_ANENABLE  0x1000

/* the advanced descriptors' bits */
#define TXD_DATA    0x00300000L         /* DTYP: data */
#define TXD_EOP     0x01000000L
#define TXD_IFCS    0x02000000L
#define TXD_RS      0x08000000L
#define TXD_DEXT    0x20000000L
#define TXD_PAYLEN_SHIFT 14
#define XD_DD       0x00000001L
#define RXD_EOP     0x00000002L
#define RXD_FRAME_ERR 0x97000000L       /* CRC, symbol, sequence, carrier, RX error */

#define RXN     32              /* a multiple of 8: the length counts 128 bytes */
#define TXN     8
#define BUFSZ   2048

#define GROUP_82575 0
#define GROUP_IGB   1
#define GROUP_IGC   2

typedef struct {
    uint32_t icr, ims, imc;     /* the interrupt registers */
    uint32_t rxq, txq;          /* queue 0's blocks */
} GROUP;

static const GROUP groups[3] = {
    { 0x00C0, 0x00D0, 0x00D8, 0x2800, 0x3800 },    /* 82575 */
    { 0x00C0, 0x00D0, 0x00D8, 0x2800, 0x3800 },    /* 82576 .. I211 */
    { 0x1500, 0x1508, 0x150C, 0xC000, 0xE000 }     /* I225, I226 */
};

/* both kinds are four dwords; the receive one is read as an address
   (word0/1) and comes back as status (word2) and length (word3) */
typedef struct { uint32_t word0, word1, word2, word3; } DESC;

/* COPPER: a PHY on the chip's MDIO, at PHY address 1 (igb) or 0 (igc,
   which Linux's igc leaves at 0).  The fiber, SerDes and SGMII parts
   have none there and are left to negotiate on their own */
#define CU          1

typedef struct { uint16_t device; uint8_t group; uint8_t copper; const char *name; } CHIP;

static const CHIP chips[] = {
    { 0x10A7, GROUP_82575, CU, "Intel 82575" }, { 0x10A9, GROUP_82575, 0, "Intel 82575" },
    { 0x10D6, GROUP_82575, CU, "Intel 82575" },
    { 0x10C9, GROUP_IGB, CU, "Intel 82576" }, { 0x10E6, GROUP_IGB, 0, "Intel 82576" },
    { 0x10E7, GROUP_IGB, 0, "Intel 82576" },  { 0x10E8, GROUP_IGB, CU, "Intel 82576" },
    { 0x1526, GROUP_IGB, CU, "Intel 82576" }, { 0x150A, GROUP_IGB, CU, "Intel 82576" },
    { 0x1518, GROUP_IGB, 0, "Intel 82576" },  { 0x150D, GROUP_IGB, 0, "Intel 82576" },
    { 0x150E, GROUP_IGB, CU, "Intel 82580" }, { 0x150F, GROUP_IGB, 0, "Intel 82580" },
    { 0x1510, GROUP_IGB, 0, "Intel 82580" },  { 0x1511, GROUP_IGB, 0, "Intel 82580" },
    { 0x1516, GROUP_IGB, CU, "Intel 82580" }, { 0x1527, GROUP_IGB, 0, "Intel 82580" },
    { 0x0438, GROUP_IGB, 0, "Intel DH89xxCC" }, { 0x043A, GROUP_IGB, 0, "Intel DH89xxCC" },
    { 0x043C, GROUP_IGB, 0, "Intel DH89xxCC" }, { 0x0440, GROUP_IGB, 0, "Intel DH89xxCC" },
    { 0x1521, GROUP_IGB, CU, "Intel I350" },  { 0x1522, GROUP_IGB, 0, "Intel I350" },
    { 0x1523, GROUP_IGB, 0, "Intel I350" },   { 0x1524, GROUP_IGB, 0, "Intel I350" },
    { 0x1F40, GROUP_IGB, 0, "Intel I354" },   { 0x1F41, GROUP_IGB, 0, "Intel I354" },
    { 0x1F45, GROUP_IGB, 0, "Intel I354" },
    { 0x1533, GROUP_IGB, CU, "Intel I210" },  { 0x1536, GROUP_IGB, 0, "Intel I210" },
    { 0x1537, GROUP_IGB, 0, "Intel I210" },   { 0x1538, GROUP_IGB, 0, "Intel I210" },
    { 0x157B, GROUP_IGB, CU, "Intel I210" },  { 0x157C, GROUP_IGB, 0, "Intel I210" },
    { 0x1539, GROUP_IGB, CU, "Intel I211" },
    { 0x15F2, GROUP_IGC, CU, "Intel I225" },  { 0x15F3, GROUP_IGC, CU, "Intel I225" },
    { 0x15F8, GROUP_IGC, CU, "Intel I225" },  { 0x15F7, GROUP_IGC, CU, "Intel I220" },
    { 0x3100, GROUP_IGC, CU, "Intel I225" },  { 0x3101, GROUP_IGC, CU, "Intel I225" },
    { 0x5502, GROUP_IGC, CU, "Intel I225" },  { 0x0D9F, GROUP_IGC, CU, "Intel I225" },
    { 0x15FD, GROUP_IGC, CU, "Intel I225" },
    { 0x125B, GROUP_IGC, CU, "Intel I226" },  { 0x125C, GROUP_IGC, CU, "Intel I226" },
    { 0x125D, GROUP_IGC, CU, "Intel I226" },  { 0x125E, GROUP_IGC, CU, "Intel I221" },
    { 0x3102, GROUP_IGC, CU, "Intel I226" },  { 0x5503, GROUP_IGC, CU, "Intel I226" },
    { 0x125F, GROUP_IGC, CU, "Intel I226" },
    { 0, 0, 0, 0 }
};

static uint32_t membase;
static const GROUP *grp;
static const CHIP *chip;
static volatile DESC *rxd;
static volatile DESC *txd;
static uint8_t *rxbuf[RXN];
static uint8_t *txbuf[TXN];
static int      rx_cur, tx_cur;

static uint32_t align128( uint32_t p ) { return ( p + 127 ) & ~127L; }
static uint32_t reg_rd( uint32_t off ) { return MMIO32( membase, off ); }
static void reg_wr( uint32_t off, uint32_t value ) { MMIO32( membase, off ) = value; }

static void name_add( const char *text )
{
    char *to = net_name;
    int  left = (int)sizeof( net_name ) - 1;

    while ( *to && left ) { to++; left--; }
    while ( *text && left ) { *to++ = *text++; left--; }
    *to = 0;
}

/* a queue switched on or off, and read back: the chip takes a moment
   to start fetching, and a queue used before then loses frames */
static int queue_enable( uint32_t dctl, uint32_t thresh, int on )
{
    int i;

    reg_wr( dctl, on ? thresh | DCTL_ENABLE : 0 );
    for ( i = 0; i < 100; i++ ) {
        if ( ( ( reg_rd( dctl ) & DCTL_ENABLE ) != 0 ) == ( on != 0 ) ) return 1;
        net_delay_us( 100 );
    }
    return 0;
}

static void quiet( void )
{
    reg_wr( grp->imc, 0xFFFFFFFFL );
    reg_wr( REG_RCTL, 0 );
    reg_wr( REG_TCTL, TCTL_PSP );
    reg_rd( REG_STATUS );                       /* the writes are done */
}

/* ---------------------------------------------------------------- */
/* the PHY, shared with the management firmware                      */
/* ---------------------------------------------------------------- */

/* SWSM: SMBI is taken by reading it clear (the read sets it), SWESMBI
   by writing it and reading it back set - Intel's two-step semaphore */
static int sem_get( void )
{
    int i;

    for ( i = 0; i < 2000 && ( reg_rd( REG_SWSM ) & SWSM_SMBI ); i++ ) net_delay_us( 50 );
    if ( i == 2000 ) return 0;
    for ( i = 0; i < 2000; i++ ) {
        reg_wr( REG_SWSM, reg_rd( REG_SWSM ) | SWSM_SWESMBI );
        if ( reg_rd( REG_SWSM ) & SWSM_SWESMBI ) return 1;
        net_delay_us( 50 );
    }
    reg_wr( REG_SWSM, reg_rd( REG_SWSM ) & ~( SWSM_SMBI | SWSM_SWESMBI ) );
    return 0;
}

static void sem_put( void )
{
    reg_wr( REG_SWSM, reg_rd( REG_SWSM ) & ~( SWSM_SMBI | SWSM_SWESMBI ) );
}

/* port 0's PHY claimed from the firmware (SW_FW_SYNC), up to a second */
static int phy_take( void )
{
    uint32_t sync;
    int i;

    for ( i = 0; i < 200; i++ ) {
        if ( !sem_get() ) return 0;
        sync = reg_rd( REG_SW_FW_SYNC );
        if ( !( sync & ( SWFW_PHY0 | ( SWFW_PHY0 << 16 ) ) ) ) {
            reg_wr( REG_SW_FW_SYNC, sync | SWFW_PHY0 );
            sem_put();
            return 1;
        }
        sem_put();
        net_delay_us( 5000 );
    }
    return 0;
}

static void phy_give( void )
{
    if ( !sem_get() ) return;
    reg_wr( REG_SW_FW_SYNC, reg_rd( REG_SW_FW_SYNC ) & ~SWFW_PHY0 );
    sem_put();
}

/* one PHY register through MDIC, read or written; 0 if it failed */
static int mdic( uint32_t op, int reg, uint16_t *value )
{
    uint32_t cmd, back;
    int i;

    cmd = op | ( (uint32_t)reg << MDIC_REG_SHIFT ) | ( (uint32_t)( chip->group == GROUP_IGC ? 0 : 1 ) << MDIC_PHY_SHIFT );
    if ( op == MDIC_WRITE ) cmd |= *value;
    reg_wr( REG_MDIC, cmd );
    for ( i = 0; i < 1000; i++ ) {
        net_delay_us( 50 );
        back = reg_rd( REG_MDIC );
        if ( back & MDIC_READY ) {
            if ( back & MDIC_ERROR ) return 0;
            if ( op == MDIC_READ ) *value = (uint16_t)back;
            return 1;
        }
    }
    return 0;
}

/* negotiation started again, as every Intel driver does after a reset:
   the PHY may not have started by itself (QEMU's 82576 does not) */
static void phy_renegotiate( void )
{
    uint16_t bmcr;

    if ( !chip->copper ) return;
    if ( !phy_take() ) { net_print( "Intel: the firmware would not let go of the PHY\r\n" ); return; }
    if ( mdic( MDIC_READ, 0, &bmcr ) ) {
        bmcr |= BMCR_ANENABLE | BMCR_ANRESTART;
        mdic( MDIC_WRITE, 0, &bmcr );
    }
    phy_give();
}

int card_find( void )
{
    PCI_ID one[2];
    int isio, i;
    uint32_t at, bar, ral, rah;
    unsigned int need;
    uint8_t *pool;

    one[1].vendor = 0;
    one[1].device = 0;
    for ( chip = chips; chip->device; chip++ ) {
        one[0].vendor = 0x8086;
        one[0].device = chip->device;
        if ( net_pci_find( one, 0 ) ) break;
    }
    if ( chip->device == 0 ) { net_strcpy( net_name, "Intel I210/I350/I225" ); return 1; }
    grp = &groups[chip->group];
    if ( net_line ) net_strcpy( net_name, chip->name );     /* after a park the name stands */
    net_pci_enable( net_pciaddr );
    bar = net_pci_bar( net_pciaddr, 0, &isio );     /* BAR0 is memory */
    if ( bar == 0 || isio ) return 1;
    membase = net_pci_map( bar, 0x20000 );
    if ( membase == 0 ) return 1;
    net_mem = membase;
    net_irq = (uint8_t)net_pci_irqline( net_pciaddr );
    net_bus = NICB_PCI;
    net_flags = NICF_WORD;

    /* stop the bus master, then everything, then reset */
    reg_wr( REG_CTRL, reg_rd( REG_CTRL ) | CTRL_GIO_MASTER_DISABLE );
    for ( i = 0; i < 8000 && ( reg_rd( REG_STATUS ) & STATUS_GIO_MASTER ); i++ ) net_delay_us( 100 );
    quiet();
    net_delay_us( 10000 );
    reg_wr( REG_CTRL, reg_rd( REG_CTRL ) | CTRL_RST );
    net_delay_us( 3000 );
    for ( i = 0; i < 100 && !( reg_rd( REG_EECD ) & EECD_AUTO_RD ); i++ ) net_delay_us( 100 );
    net_delay_us( 10000 );                      /* the PHY's own configuration */
    reg_wr( grp->imc, 0xFFFFFFFFL );
    reg_rd( grp->icr );
    reg_wr( REG_CTRL_EXT, reg_rd( REG_CTRL_EXT ) | CTRL_EXT_DRV_LOAD );
    reg_wr( REG_CTRL, ( reg_rd( REG_CTRL ) | CTRL_SLU ) & ~( CTRL_FRCSPD | CTRL_FRCDPX ) );
    phy_renegotiate();

    ral = reg_rd( REG_RAL );
    rah = reg_rd( REG_RAH );
    if ( !( rah & RAH_AV ) ) { net_print( "Intel: no station address loaded\r\n" ); return 1; }
    net_mac[0] = (uint8_t)ral;        net_mac[1] = (uint8_t)( ral >> 8 );
    net_mac[2] = (uint8_t)( ral >> 16 ); net_mac[3] = (uint8_t)( ral >> 24 );
    net_mac[4] = (uint8_t)rah;        net_mac[5] = (uint8_t)( rah >> 8 );
    if ( !net_mac_ok( net_mac ) ) { net_print( "Intel: the station address is not usable\r\n" ); return 1; }

    if ( rxd == 0 ) {
        need = ( RXN + TXN ) * sizeof( DESC ) + 128 + ( RXN + TXN ) * BUFSZ;
        pool = net_alloc( need );
        if ( pool == 0 ) return 1;
        at = align128( (uint32_t)pool );
        rxd = (volatile DESC *)at;  at += RXN * sizeof( DESC );
        txd = (volatile DESC *)at;  at += TXN * sizeof( DESC );
        for ( i = 0; i < RXN; i++ ) { rxbuf[i] = (uint8_t *)at; at += BUFSZ; }
        for ( i = 0; i < TXN; i++ ) { txbuf[i] = (uint8_t *)at; at += BUFSZ; }
    }
    return 0;
}

/* a receive descriptor handed (back) to the chip: the whole of it, since
   the chip wrote its status over the address */
static void rx_give( int entry )
{
    rxd[entry].word0 = (uint32_t)rxbuf[entry];
    rxd[entry].word1 = 0;
    rxd[entry].word2 = 0;
    rxd[entry].word3 = 0;
}

int card_setup( void )
{
    uint32_t status;
    int i;

    quiet();
    queue_enable( grp->rxq + Q_DCTL, RXDCTL_THRESH, 0 );
    queue_enable( grp->txq + Q_DCTL, TXDCTL_THRESH, 0 );
    reg_wr( REG_CTRL, ( reg_rd( REG_CTRL ) | CTRL_SLU ) & ~( CTRL_FRCSPD | CTRL_FRCDPX ) );
    for ( i = 0; i < 128; i++ ) reg_wr( REG_MTA + i * 4, 0 );

    /* receive: the ring, the buffer size and kind, the queue on, then
       the unit, then the tail that hands the buffers over */
    for ( i = 0; i < RXN; i++ ) rx_give( i );
    reg_wr( grp->rxq + Q_BAL, (uint32_t)rxd );
    reg_wr( grp->rxq + Q_BAH, 0 );
    reg_wr( grp->rxq + Q_LEN, RXN * sizeof( DESC ) );
    reg_wr( grp->rxq + Q_HEAD, 0 );
    reg_wr( grp->rxq + Q_TAIL, 0 );
    reg_wr( grp->rxq + Q_SRRCTL, SRRCTL_2K | SRRCTL_ONEBUF );
    if ( !queue_enable( grp->rxq + Q_DCTL, RXDCTL_THRESH, 1 ) ) {
        net_print( "Intel: the receive queue would not start\r\n" );
        return 1;
    }
    /* RCTL_MPE accepts all multicast: NetBEUI's functional address
       03-00-00-00-00-01 is a multicast, not only broadcast */
    reg_wr( REG_RCTL, RCTL_EN | RCTL_MPE | RCTL_BAM | RCTL_SECRC );
    reg_wr( grp->rxq + Q_TAIL, RXN - 1 );
    rx_cur = 0;

    /* transmit */
    for ( i = 0; i < TXN; i++ ) {
        txd[i].word0 = (uint32_t)txbuf[i];
        txd[i].word1 = 0;
        txd[i].word2 = 0;
        txd[i].word3 = XD_DD;                   /* free */
    }
    reg_wr( grp->txq + Q_BAL, (uint32_t)txd );
    reg_wr( grp->txq + Q_BAH, 0 );
    reg_wr( grp->txq + Q_LEN, TXN * sizeof( DESC ) );
    reg_wr( grp->txq + Q_HEAD, 0 );
    reg_wr( grp->txq + Q_TAIL, 0 );
    if ( !queue_enable( grp->txq + Q_DCTL, TXDCTL_THRESH, 1 ) ) {
        net_print( "Intel: the transmit queue would not start\r\n" );
        return 1;
    }
    reg_wr( REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | TCTL_RTLC );
    tx_cur = 0;

    /* queue 0's causes to the one vector there is without MSI-X */
    if ( chip->group == GROUP_82575 ) reg_wr( REG_MSIXBM0, 0x00000101L );
    else reg_wr( REG_IVAR0, ( reg_rd( REG_IVAR0 ) & 0xFFFF0000L ) | ( IVAR_VALID << 8 ) | IVAR_VALID );

    /* the link and its speed into the name: waited for at boot only, as
       E1000.SYS does (a later card_setup may run with interrupts off) */
    for ( i = 0; net_line && i < 60 && !( reg_rd( REG_STATUS ) & STATUS_LU ); i++ ) net_delay_us( 50000 );
    status = reg_rd( REG_STATUS );
    if ( net_line ) {                           /* after a park the name stands */
        net_strcpy( net_name, chip->name );
        if ( !( status & STATUS_LU ) ) name_add( ", no link" );
        else {
            if ( chip->group == GROUP_IGC && ( status & STATUS_SPEED_2500 ) ) name_add( ", 2500" );
            else if ( status & STATUS_SPEED_1000 ) name_add( ", 1000" );
            else if ( status & STATUS_SPEED_100 ) name_add( ", 100" );
            else name_add( ", 10" );
            name_add( status & STATUS_FD ? " full" : " half" );
        }
    }

    reg_rd( grp->icr );                          /* nothing old left to ask */
    if ( net_flags & NICF_POLLED ) reg_wr( grp->imc, 0xFFFFFFFFL );
    else reg_wr( grp->ims, ICR_RXDW | ICR_RXO | ICR_RXDMT0 );
    return 0;
}

void card_stop( void )
{
    quiet();
    queue_enable( grp->rxq + Q_DCTL, RXDCTL_THRESH, 0 );
    queue_enable( grp->txq + Q_DCTL, TXDCTL_THRESH, 0 );
}

int card_send( const uint8_t *frame, unsigned int len )
{
    int entry = tx_cur;
    unsigned int n;

    if ( !( txd[entry].word3 & XD_DD ) ) {
        for ( n = 0; n < 400000L; n++ ) {
            if ( txd[entry].word3 & XD_DD ) break;
        }
        if ( !( txd[entry].word3 & XD_DD ) ) { net_stats.txerrors++; return 0; }
    }
    net_memcpy( txbuf[entry], frame, len );
    txd[entry].word0 = (uint32_t)txbuf[entry];
    txd[entry].word1 = 0;
    txd[entry].word2 = TXD_DATA | TXD_EOP | TXD_IFCS | TXD_RS | TXD_DEXT | len;
    txd[entry].word3 = len << TXD_PAYLEN_SHIFT;  /* and DD clear: not done */
    tx_cur = ( entry + 1 ) % TXN;
    reg_wr( grp->txq + Q_TAIL, (uint32_t)tx_cur );
    return 1;
}

/* the frames the chip has finished, handed over; how many */
static int rx_drain( void )
{
    uint32_t status;
    unsigned int len, n;
    int last = -1, got = 0;
    uint8_t *slot;

    for ( n = 0; n < RXN; n++ ) {
        status = rxd[rx_cur].word2;
        if ( !( status & XD_DD ) ) break;
        len = rxd[rx_cur].word3 & 0xFFFF;       /* the CRC is already stripped */
        if ( ( status & RXD_EOP ) && !( status & RXD_FRAME_ERR ) && len >= NIC_HDRLEN && len <= NIC_MAXFRAME ) {
            slot = net_rx_begin();
            net_memcpy( slot, rxbuf[rx_cur], len );
            net_rx_commit( len );
        } else {
            net_stats.rxerrors++;
        }
        rx_give( rx_cur );
        last = rx_cur;
        rx_cur = ( rx_cur + 1 ) % RXN;
        got++;
    }
    if ( last >= 0 ) reg_wr( grp->rxq + Q_TAIL, (uint32_t)last );
    return got;
}

int card_service( void )
{
    uint32_t icr;
    int got;

    icr = reg_rd( grp->icr );                    /* reading it clears it */
    got = rx_drain();
    return icr != 0 || got != 0;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'i', 'g', 'b', ' ', ' ', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
