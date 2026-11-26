/*
 * E1000.C - E1000.SYS, a driver for Intel's PRO/1000 gigabit family
 *
 * Intel's 8254x and everything after it that kept the same registers
 * and the same rings - "e1000", the card QEMU offers by that name and
 * the one a hundred servers and virtual machines use, and "e1000e", the
 * PCI Express parts and the networks Intel built into its chipsets from
 * the ICH8 on.  A bus-master whose registers are MEMORY mapped, not in a
 * port, and whose rings are the cleanest of the lot: a head and a tail
 * register each, the chip filling from the head and the driver
 * releasing at the tail.  The 82575 and later server chips (82576, I350,
 * I210, I225) are another family with other ring registers: IGB.SYS.
 *
 * FOUR GROUPS, by how much of the chip may be reset:
 *   8254x    82540/1/4/5/6/7, PCI and PCI-X.  Reset whole (CTRL.RST);
 *            the PHY is not touched by that and comes up negotiating.
 *   8257x    82571/2/3/4, 82583, 80003ES2 - PCI Express cards and the
 *            networks of the 965 and 3-series boards.  Reset whole, and
 *            DRV_LOAD is set so the management firmware leaves the
 *            network to the driver.
 *   ICH      82566/82567 (ICH8, ICH9, ICH10) and 82577/82578/82579
 *            (the 5-, 6- and 7-series PCH).  NOT reset: a full reset of
 *            these resets their PHY with them, and Linux follows that
 *            with dozens of PHY register workarounds a chip at a time.
 *            The BIOS left the network working; it is stopped, its rings
 *            replaced, and started again as it stands.
 *   I21x     I217, I218, I219 (Haswell on, to today's boards).  As ICH,
 *            and first the PHY is brought out of its ultra-low-power
 *            state (ULP) - which these enter when the machine was last
 *            shut down without a network driver, and in which the PHY
 *            answers nothing and the link never comes up - by asking the
 *            Management Engine to undo it (H2ME, FWSM.ULP_CFG_DONE).  A
 *            machine without ME firmware would need the PHY reached over
 *            SMBus instead; that is not done here, and such a machine
 *            says "no link".
 * The 82542 and 82543 (1998-2000, a register map of their own and a PHY
 * driven through the chip's pins) are not taken.
 *
 * THE STATION ADDRESS every group has already loaded from its EEPROM or
 * flash into the receive-address registers, so it is read from there
 * without touching the EEPROM at all.
 *
 * THE LINK is waited for up to two and a half seconds at the end of
 * card_setup and goes into the name with its speed; no link is said and
 * is not a failure, since a cable put in later is taken up by the chip.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).  86Box has no card of
 * this family; proved on QEMU's e1000 (82540EM), e1000-82544gc,
 * e1000-82545em and e1000e (82574L).  The ICH and I21x groups have not
 * been run.
 */
#include "netdrv.h"

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EECD    0x0010
#define REG_CTRL_EXT 0x0018
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
#define REG_H2ME    0x5B50      /* I21x: host to Management Engine */
#define REG_FWSM    0x5B54      /* I21x: firmware semaphore */

#define CTRL_ASDE   0x00000020L /* auto-speed detect */
#define CTRL_SLU    0x00000040L /* set link up */
#define CTRL_FRCSPD 0x00000800L /* force the speed */
#define CTRL_FRCDPX 0x00001000L /* force the duplex */
#define CTRL_RST    0x04000000L /* reset */

#define CTRL_EXT_DRV_LOAD 0x10000000L   /* the firmware: a driver has it */

#define STATUS_FD   0x00000001L
#define STATUS_LU   0x00000002L
#define STATUS_SPEED 0x000000C0L

#define EECD_AUTO_RD 0x00000200L /* the EEPROM's words are loaded */

#define FWSM_FW_VALID    0x00008000L
#define FWSM_ULP_CFG_DONE 0x00000400L
#define H2ME_ULP         0x00000800L
#define H2ME_ENFORCE     0x00001000L

#define RCTL_EN     0x00000002L
#define RCTL_MPE    0x00000010L /* accept all multicast */
#define RCTL_BAM    0x00008000L /* accept broadcast */
#define RCTL_SECRC  0x04000000L /* strip the ethernet CRC */
#define RCTL_BSIZE  0x00000000L /* 2048-byte buffers */

#define TCTL_EN     0x00000002L
#define TCTL_PSP    0x00000008L /* pad short packets */
#define TCTL_CT     0x000000F0L /* collision threshold 15 */
#define TCTL_COLD   0x00040000L /* collision distance 64 (half duplex) */
#define TCTL_RTLC   0x01000000L /* send again after a late collision */

#define RAH_AV      0x80000000L /* the address is valid */

#define ICR_LSC     0x00000004L /* the link changed */
#define ICR_RXDMT0  0x00000010L /* receive descriptors low */
#define ICR_RXO     0x00000040L /* receive overrun */
#define ICR_RXT0    0x00000080L /* a receive timer went off: frames waiting */

#define TXD_EOP     0x01        /* end of packet */
#define TXD_IFCS    0x02        /* insert the FCS */
#define TXD_RS      0x08        /* report status */
#define XD_DD       0x01        /* descriptor done */
#define RXD_EOP     0x02        /* end of packet */

#define RXN     32              /* a multiple of 8: RDLEN counts 128 bytes */
#define TXN     8
#define BUFSZ   2048

#define GROUP_8254X 0
#define GROUP_8257X 1
#define GROUP_ICH   2
#define GROUP_I21X  3

typedef struct { uint32_t addr_lo, addr_hi; uint16_t length, csum; uint8_t status, errors; uint16_t special; } RXD;
typedef struct { uint32_t addr_lo, addr_hi; uint16_t length; uint8_t cso, cmd, status, css; uint16_t special; } TXD;

static uint32_t membase;
static int      group;
static volatile RXD *rxd;
static volatile TXD *txd;
static uint8_t *rxbuf[RXN];
static uint8_t *txbuf[TXN];
static int      rx_cur, tx_cur;
static char     base_name[24];

/* 82540/1/4/5/6/7 */
static const PCI_ID ids_8254x[] = {
    { 0x8086, 0x1008 }, { 0x8086, 0x1009 }, { 0x8086, 0x100C }, { 0x8086, 0x100D },
    { 0x8086, 0x100E }, { 0x8086, 0x1015 }, { 0x8086, 0x1016 }, { 0x8086, 0x1017 },
    { 0x8086, 0x101E }, { 0x8086, 0x100F }, { 0x8086, 0x1011 }, { 0x8086, 0x1026 },
    { 0x8086, 0x1027 }, { 0x8086, 0x1028 }, { 0x8086, 0x1010 }, { 0x8086, 0x1012 },
    { 0x8086, 0x101D }, { 0x8086, 0x1079 }, { 0x8086, 0x107A }, { 0x8086, 0x107B },
    { 0x8086, 0x108A }, { 0x8086, 0x1099 }, { 0x8086, 0x10B5 }, { 0x8086, 0x1013 },
    { 0x8086, 0x1018 }, { 0x8086, 0x1014 }, { 0x8086, 0x1078 }, { 0x8086, 0x1076 },
    { 0x8086, 0x1077 }, { 0x8086, 0x107C }, { 0x8086, 0x1019 }, { 0x8086, 0x101A },
    { 0x8086, 0x1075 },
    { 0, 0 }
};

/* 82571/2/3/4, 82583, 80003ES2 */
static const PCI_ID ids_8257x[] = {
    { 0x8086, 0x105E }, { 0x8086, 0x105F }, { 0x8086, 0x1060 }, { 0x8086, 0x10A4 },
    { 0x8086, 0x10D5 }, { 0x8086, 0x10A5 }, { 0x8086, 0x10BC }, { 0x8086, 0x10D9 },
    { 0x8086, 0x10DA }, { 0x8086, 0x107D }, { 0x8086, 0x107E }, { 0x8086, 0x107F },
    { 0x8086, 0x10B9 }, { 0x8086, 0x108B }, { 0x8086, 0x108C }, { 0x8086, 0x109A },
    { 0x8086, 0x10D3 }, { 0x8086, 0x10F6 }, { 0x8086, 0x150C }, { 0x8086, 0x1096 },
    { 0x8086, 0x1098 }, { 0x8086, 0x10BA }, { 0x8086, 0x10BB },
    { 0, 0 }
};

/* ICH8, ICH9, ICH10, and the PCH of the 5-, 6- and 7-series */
static const PCI_ID ids_ich[] = {
    { 0x8086, 0x1049 }, { 0x8086, 0x104A }, { 0x8086, 0x104B }, { 0x8086, 0x104C },
    { 0x8086, 0x10C4 }, { 0x8086, 0x10C5 }, { 0x8086, 0x104D }, { 0x8086, 0x1501 },
    { 0x8086, 0x10BD }, { 0x8086, 0x10E5 }, { 0x8086, 0x10F5 }, { 0x8086, 0x10BF },
    { 0x8086, 0x10CB }, { 0x8086, 0x294C }, { 0x8086, 0x10C0 }, { 0x8086, 0x10C3 },
    { 0x8086, 0x10C2 }, { 0x8086, 0x10CC }, { 0x8086, 0x10CD }, { 0x8086, 0x10CE },
    { 0x8086, 0x10DE }, { 0x8086, 0x10DF }, { 0x8086, 0x1525 }, { 0x8086, 0x10EA },
    { 0x8086, 0x10EB }, { 0x8086, 0x10EF }, { 0x8086, 0x10F0 }, { 0x8086, 0x1502 },
    { 0x8086, 0x1503 },
    { 0, 0 }
};

/* I217, I218, I219 */
static const PCI_ID ids_i21x[] = {
    { 0x8086, 0x153A }, { 0x8086, 0x153B }, { 0x8086, 0x155A }, { 0x8086, 0x1559 },
    { 0x8086, 0x15A0 }, { 0x8086, 0x15A1 }, { 0x8086, 0x15A2 }, { 0x8086, 0x15A3 },
    { 0x8086, 0x156F }, { 0x8086, 0x1570 }, { 0x8086, 0x15B7 }, { 0x8086, 0x15B8 },
    { 0x8086, 0x15B9 }, { 0x8086, 0x15D7 }, { 0x8086, 0x15D8 }, { 0x8086, 0x15E3 },
    { 0x8086, 0x15D6 }, { 0x8086, 0x15BD }, { 0x8086, 0x15BE }, { 0x8086, 0x15BB },
    { 0x8086, 0x15BC }, { 0x8086, 0x15DF }, { 0x8086, 0x15E0 }, { 0x8086, 0x15E1 },
    { 0x8086, 0x15E2 }, { 0x8086, 0x0D4E }, { 0x8086, 0x0D4F }, { 0x8086, 0x0D4C },
    { 0x8086, 0x0D4D }, { 0x8086, 0x0D53 }, { 0x8086, 0x0D55 }, { 0x8086, 0x15FB },
    { 0x8086, 0x15FC }, { 0x8086, 0x15F9 }, { 0x8086, 0x15FA }, { 0x8086, 0x15F4 },
    { 0x8086, 0x15F5 }, { 0x8086, 0x0DC5 }, { 0x8086, 0x0DC6 }, { 0x8086, 0x1A1E },
    { 0x8086, 0x1A1F }, { 0x8086, 0x1A1C }, { 0x8086, 0x1A1D }, { 0x8086, 0x0DC7 },
    { 0x8086, 0x0DC8 }, { 0x8086, 0x550A }, { 0x8086, 0x550B }, { 0x8086, 0x550C },
    { 0x8086, 0x550D }, { 0x8086, 0x550E }, { 0x8086, 0x550F }, { 0x8086, 0x5510 },
    { 0x8086, 0x5511 }, { 0x8086, 0x57A0 }, { 0x8086, 0x57A1 }, { 0x8086, 0x57B3 },
    { 0x8086, 0x57B4 }, { 0x8086, 0x57B7 }, { 0x8086, 0x57B8 }, { 0x8086, 0x57B9 },
    { 0x8086, 0x57BA },
    { 0, 0 }
};

static uint32_t align16( uint32_t p ) { return ( p + 15 ) & ~15L; }
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

/* the PHY out of its ultra-low-power state, through the ME.  The I217
   and the first I218s have no ULP; a machine with no ME firmware is
   left as it is (see the top of the file) */
static void ulp_off( uint32_t device )
{
    uint32_t h2me;
    int i;

    if ( device == 0x153A || device == 0x153B || device == 0x15A0 || device == 0x15A1 ) return;
    if ( !( reg_rd( REG_FWSM ) & FWSM_FW_VALID ) ) return;
    h2me = reg_rd( REG_H2ME );
    reg_wr( REG_H2ME, ( h2me & ~H2ME_ULP ) | H2ME_ENFORCE );
    for ( i = 0; i < 250 && ( reg_rd( REG_FWSM ) & FWSM_ULP_CFG_DONE ); i++ ) net_delay_us( 10000 );
    reg_wr( REG_H2ME, reg_rd( REG_H2ME ) & ~H2ME_ENFORCE );
    if ( i == 250 ) net_print( "Intel I21x: the ME did not wake the PHY\r\n" );
}

int card_find( void )
{
    const PCI_ID *which;
    int isio, i;
    uint32_t at, bar, ral, rah;
    unsigned int need;
    uint8_t *pool;

    if ( net_pci_find( ids_8254x, &which ) ) {
        group = GROUP_8254X; net_strcpy( base_name, "Intel PRO/1000 8254x" );
    } else if ( net_pci_find( ids_8257x, &which ) ) {
        group = GROUP_8257X; net_strcpy( base_name, "Intel PRO/1000 8257x" );
    } else if ( net_pci_find( ids_ich, &which ) ) {
        group = GROUP_ICH;   net_strcpy( base_name, "Intel ICH gigabit" );
    } else if ( net_pci_find( ids_i21x, &which ) ) {
        group = GROUP_I21X;  net_strcpy( base_name, "Intel I217/I218/I219" );
    } else {
        net_strcpy( net_name, "Intel PRO/1000" );
        return 1;
    }
    if ( net_line ) net_strcpy( net_name, base_name );  /* after a park the name stands */
    net_pci_enable( net_pciaddr );
    bar = net_pci_bar( net_pciaddr, 0, &isio );     /* BAR0 is memory */
    if ( bar == 0 || isio ) return 1;
    membase = net_pci_map( bar, 0x20000 );
    if ( membase == 0 ) return 1;
    net_mem = membase;
    net_irq = (uint8_t)net_pci_irqline( net_pciaddr );
    net_bus = NICB_PCI;
    net_flags = NICF_WORD;

    /* nothing may interrupt, and neither unit may run, from here until
       card_setup has the new rings in place - a network the BIOS's boot
       ROM left running is still writing into ITS ring */
    reg_wr( REG_IMC, 0xFFFFFFFFL );
    reg_wr( REG_RCTL, 0 );
    reg_wr( REG_TCTL, TCTL_PSP );
    reg_rd( REG_STATUS );                           /* the writes are done */
    net_delay_us( 10000 );

    if ( group <= GROUP_8257X ) {
        /* reset; the chip loads its EEPROM again, which takes a few
           milliseconds, and the address is in RAL/RAH after it */
        reg_wr( REG_CTRL, reg_rd( REG_CTRL ) | CTRL_RST );
        net_delay_us( 3000 );
        for ( i = 0; i < 100 && ( reg_rd( REG_CTRL ) & CTRL_RST ); i++ ) net_delay_us( 1000 );
        for ( i = 0; i < 10 && !( reg_rd( REG_EECD ) & EECD_AUTO_RD ); i++ ) net_delay_us( 1000 );
        reg_wr( REG_IMC, 0xFFFFFFFFL );
    } else if ( group == GROUP_I21X ) {
        ulp_off( which->device );
    }
    reg_rd( REG_ICR );                              /* whatever was pending */
    if ( group != GROUP_8254X ) reg_wr( REG_CTRL_EXT, reg_rd( REG_CTRL_EXT ) | CTRL_EXT_DRV_LOAD );

    ral = reg_rd( REG_RAL );
    rah = reg_rd( REG_RAH );
    if ( !( rah & RAH_AV ) ) { net_print( "Intel PRO/1000: no station address loaded\r\n" ); return 1; }
    net_mac[0] = (uint8_t)ral;        net_mac[1] = (uint8_t)( ral >> 8 );
    net_mac[2] = (uint8_t)( ral >> 16 ); net_mac[3] = (uint8_t)( ral >> 24 );
    net_mac[4] = (uint8_t)rah;        net_mac[5] = (uint8_t)( rah >> 8 );
    if ( !net_mac_ok( net_mac ) ) { net_print( "Intel PRO/1000: the station address is not usable\r\n" ); return 1; }

    if ( rxd == 0 ) {
        need = RXN * 16 + TXN * 16 + ( RXN + TXN ) * BUFSZ + 16;
        pool = net_alloc( need );
        if ( pool == 0 ) return 1;
        at = align16( (uint32_t)pool );
        rxd = (volatile RXD *)at;   at += RXN * 16;
        txd = (volatile TXD *)at;   at += TXN * 16;
        for ( i = 0; i < RXN; i++ ) { rxbuf[i] = (uint8_t *)at; at += BUFSZ; }
        for ( i = 0; i < TXN; i++ ) { txbuf[i] = (uint8_t *)at; at += BUFSZ; }
    }
    return 0;
}

int card_setup( void )
{
    uint32_t status;
    int i;

    reg_wr( REG_IMC, 0xFFFFFFFFL );
    reg_wr( REG_RCTL, 0 );
    reg_wr( REG_TCTL, TCTL_PSP );
    /* the link up, the speed and duplex the PHY's; auto-speed detect only
       on the cards (e1000e leaves it alone on the chipset networks) */
    reg_wr( REG_CTRL, ( reg_rd( REG_CTRL ) | CTRL_SLU | ( group <= GROUP_8257X ? CTRL_ASDE : 0 ) ) & ~( CTRL_FRCSPD | CTRL_FRCDPX ) );
    for ( i = 0; i < 128; i++ ) reg_wr( REG_MTA + i * 4, 0 );

    for ( i = 0; i < RXN; i++ ) {
        rxd[i].addr_lo = (uint32_t)rxbuf[i];
        rxd[i].addr_hi = 0;
        rxd[i].status = 0;
    }
    reg_wr( REG_RDBAL, (uint32_t)rxd );
    reg_wr( REG_RDBAH, 0 );
    reg_wr( REG_RDLEN, RXN * 16 );
    reg_wr( REG_RDH, 0 );
    reg_wr( REG_RDT, RXN - 1 );
    rx_cur = 0;
    /* RCTL_MPE accepts all multicast: NetBEUI's functional address
       03-00-00-00-00-01 is a multicast, not only broadcast */
    reg_wr( REG_RCTL, RCTL_EN | RCTL_MPE | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE );

    for ( i = 0; i < TXN; i++ ) {
        txd[i].addr_lo = (uint32_t)txbuf[i];
        txd[i].addr_hi = 0;
        txd[i].cmd = 0;
        txd[i].status = XD_DD;                   /* free */
    }
    reg_wr( REG_TDBAL, (uint32_t)txd );
    reg_wr( REG_TDBAH, 0 );
    reg_wr( REG_TDLEN, TXN * 16 );
    reg_wr( REG_TDH, 0 );
    reg_wr( REG_TDT, 0 );
    tx_cur = 0;
    reg_wr( REG_TIPG, 0x0060200AL );
    reg_wr( REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | TCTL_RTLC );

    /* the link, if it is coming, and its speed into the name.  Waited for
       at boot only (net_line is set then): a later card_setup is a park
       coming back or the WfW bridge, which runs this with interrupts off */
    for ( i = 0; net_line && i < 50 && !( reg_rd( REG_STATUS ) & STATUS_LU ); i++ ) net_delay_us( 50000 );
    status = reg_rd( REG_STATUS );
    if ( net_line ) {
        net_strcpy( net_name, base_name );
        if ( !( status & STATUS_LU ) ) name_add( ", no link" );
        else {
            name_add( ( status & STATUS_SPEED ) == 0 ? ", 10" : ( status & STATUS_SPEED ) == 0x40 ? ", 100" : ", 1000" );
            name_add( status & STATUS_FD ? " full" : " half" );
        }
    }

    reg_rd( REG_ICR );                           /* nothing old left to ask */
    if ( net_flags & NICF_POLLED ) reg_wr( REG_IMC, 0xFFFFFFFFL );
    else reg_wr( REG_IMS, ICR_RXT0 | ICR_RXO | ICR_RXDMT0 );
    return 0;
}

void card_stop( void )
{
    reg_wr( REG_IMC, 0xFFFFFFFFL );
    reg_wr( REG_RCTL, 0 );
    reg_wr( REG_TCTL, TCTL_PSP );
}

int card_send( const uint8_t *frame, unsigned int len )
{
    int entry = tx_cur;
    unsigned int n;

    if ( !( txd[entry].status & XD_DD ) ) {
        for ( n = 0; n < 400000L; n++ ) {
            if ( txd[entry].status & XD_DD ) break;
        }
        if ( !( txd[entry].status & XD_DD ) ) { net_stats.txerrors++; return 0; }
    }
    net_memcpy( txbuf[entry], frame, len );
    txd[entry].addr_lo = (uint32_t)txbuf[entry];
    txd[entry].addr_hi = 0;
    txd[entry].length = (uint16_t)len;
    txd[entry].cso = 0;
    txd[entry].cmd = TXD_EOP | TXD_IFCS | TXD_RS;
    txd[entry].status = 0;
    tx_cur = ( entry + 1 ) % TXN;
    reg_wr( REG_TDT, tx_cur );
    return 1;
}

/* the frames the chip has finished, handed over; how many */
static int rx_drain( void )
{
    unsigned int len, n;
    int last = -1, got = 0;
    uint8_t *slot;

    for ( n = 0; n < RXN; n++ ) {
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
        got++;
    }
    if ( last >= 0 ) reg_wr( REG_RDT, (uint32_t)last );
    return got;
}

/* the ring is looked at whatever ICR says: with interrupts masked (a
   polled card) some of the PCI Express parts do not latch the causes */
int card_service( void )
{
    uint32_t icr;
    int got;

    icr = reg_rd( REG_ICR );                     /* reading it clears it */
    got = rx_drain();
    return icr != 0 || got != 0;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'e', '1', '0', '0', '0', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
