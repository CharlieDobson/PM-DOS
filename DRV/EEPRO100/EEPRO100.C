/*
 * EEPRO100.C - EEPRO100.SYS, a driver for the Intel 8255x (i82557/8/9)
 *
 * Intel's 10/100 "EtherExpress PRO/100" chips, the i82557 and its
 * children - QEMU offers the family as i82557b and the rest.  Unlike
 * every card above, it is not driven by poking a few registers: the
 * driver builds COMMAND BLOCKS in memory and hands the chip a pointer
 * to a list of them, and receiving is a list of frame descriptors the
 * chip fills.  There is one small control register, the SCB, through
 * which the lists are started and the chip's state is read.
 *
 * SIMPLIFIED MODE.  The chip can gather a frame from scattered buffers;
 * this driver does not need that and uses the simpler form, where a
 * transmit command block carries its frame inline and a receive
 * descriptor is followed by the frame it caught.  The chip is told at
 * the start who it is (an individual-address command) and how to behave
 * (a configure command, whose 22 bytes are the family's long-standing
 * defaults).
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).  86Box has no card of
 * this family; this is proved on QEMU.
 */
#include "netdrv.h"

/* the SCB, at the first I/O port */
#define SCB_STATUS  0x00        /* word: status and the ack bits */
#define SCB_CMD     0x02        /* word: low = a CU/RU command, high = int mask */
#define SCB_PTR     0x04        /* dword: the block a command acts on */
#define SCB_PORT    0x08        /* dword: reset and the like */
#define SCB_EEPROM  0x0E        /* word: the serial EEPROM */

/* SCB command-byte values */
#define RUC_START   0x01
#define RUC_RESUME  0x02
#define RUC_LOADBASE 0x06
#define CUC_START   0x10
#define CUC_RESUME  0x20
#define CUC_LOADBASE 0x60

/* SCB status ack bits (high byte of the status word) */
#define ACK_CX      0x8000      /* a command finished */
#define ACK_FR      0x4000      /* a frame was received */
#define ACK_CNA     0x2000      /* the CU is idle */
#define ACK_RNR     0x1000      /* the RU is out of resources */
#define ACK_ALL     0xFF00

/* a command block's command word */
#define CB_NOP      0
#define CB_IASETUP  1
#define CB_CONFIG   2
#define CB_TX       4
#define CB_EL       0x8000      /* end of list */
#define CB_S        0x4000      /* suspend after this one */
#define CB_I        0x2000      /* interrupt after this one */
#define CB_C        0x8000      /* status: this one completed */

/* EEPROM (93C46) bits in SCB_EEPROM */
#define EE_SK       0x01
#define EE_CS       0x02
#define EE_DI       0x04
#define EE_DO       0x08

#define RFDN        8
#define TXN         4
#define RFDSZ       ( 16 + 1536 )       /* the header and a frame */
#define TXCBSZ      ( 16 + 1536 )

#define SPIN        400000L

static u8  *rfd[RFDN];                  /* receive frame descriptors */
static u8  *txcb[TXN];                  /* transmit command blocks */
static u8  *cmdcb;                      /* for IA setup and configure */
static int  rx_cur, tx_cur;

static const PCI_ID eepro_ids[] = {
    { 0x8086, 0x1229 },         /* 82557/8/9 - QEMU's i8255x */
    { 0x8086, 0x1209 },
    { 0x8086, 0x1029 },
    { 0x8086, 0x2449 },
    { 0, 0 }
};

/* Intel's long-standing default configure block, 22 bytes */
/* byte 21 has the Multicast-All bit (0x08): take every multicast address.
   NetBEUI's functional address 03-00-00-00-00-01 is a multicast, so a card
   that is to carry NetBEUI must accept multicast, not only broadcast. */
static const u8 config_default[22] = {
    22, 0x08, 0, 0, 0, 0, 0x32, 0x03, 1, 0,
    0x2E, 0, 0x60, 0, 0xF2, 0x48, 0, 0x40, 0xF2, 0x80, 0x3F, 0x0D
};

static u32 align16( u32 p ) { return ( p + 15 ) & ~15L; }
static void eedelay( void ) { inw( net_port + SCB_EEPROM ); }

static u16 scb_status( void ) { return inw( net_port + SCB_STATUS ); }

/* wait for the SCB command byte to clear (the last command taken) */
static int scb_wait( void )
{
    u32 n;
    for ( n = 0; n < SPIN; n++ ) {
        if ( inb( net_port + SCB_CMD ) == 0 ) return 1;
    }
    return 0;
}

static int scb_cmd( u8 cmd, u32 ptr )
{
    if ( !scb_wait() ) return 0;
    outl( net_port + SCB_PTR, ptr );
    outb( net_port + SCB_CMD, cmd );
    return 1;
}

/* a word of the 93C46.  The EEPROM address is 6 or 8 bits wide; the
   width is found by clocking address bits until the chip's dummy zero
   comes back */
static u16 ee_word( int addr, int abits )
{
    u32 n;
    u16 val = 0;
    int i;

    outw( net_port + SCB_EEPROM, EE_CS );
    /* command: read is 110, then the address bits, most significant first */
    for ( i = 2; i >= 0; i-- ) {
        u16 d = (u16)( EE_CS | ( ( 6 >> i ) & 1 ? EE_DI : 0 ) );
        outw( net_port + SCB_EEPROM, d ); eedelay();
        outw( net_port + SCB_EEPROM, d | EE_SK ); eedelay();
        outw( net_port + SCB_EEPROM, d ); eedelay();
    }
    for ( i = abits - 1; i >= 0; i-- ) {
        u16 d = (u16)( EE_CS | ( ( addr >> i ) & 1 ? EE_DI : 0 ) );
        outw( net_port + SCB_EEPROM, d ); eedelay();
        outw( net_port + SCB_EEPROM, d | EE_SK ); eedelay();
        outw( net_port + SCB_EEPROM, d ); eedelay();
    }
    for ( i = 0; i < 16; i++ ) {
        outw( net_port + SCB_EEPROM, EE_CS | EE_SK ); eedelay();
        val = (u16)( ( val << 1 ) | ( ( inw( net_port + SCB_EEPROM ) & EE_DO ) ? 1 : 0 ) );
        outw( net_port + SCB_EEPROM, EE_CS ); eedelay();
    }
    outw( net_port + SCB_EEPROM, 0 );
    n = 0; (void)n;
    return val;
}

/* the EEPROM address width: clock a read of address 0 and count the
   leading bits until the chip pulls its data line low (the dummy 0) */
static int ee_abits( void )
{
    int bits = 0;
    int i;

    outw( net_port + SCB_EEPROM, EE_CS );
    for ( i = 2; i >= 0; i-- ) {
        u16 d = (u16)( EE_CS | ( ( 6 >> i ) & 1 ? EE_DI : 0 ) );
        outw( net_port + SCB_EEPROM, d ); eedelay();
        outw( net_port + SCB_EEPROM, d | EE_SK ); eedelay();
        outw( net_port + SCB_EEPROM, d ); eedelay();
    }
    for ( bits = 1; bits <= 8; bits++ ) {
        outw( net_port + SCB_EEPROM, EE_CS | EE_DI ); eedelay();
        outw( net_port + SCB_EEPROM, EE_CS | EE_DI | EE_SK ); eedelay();
        if ( !( inw( net_port + SCB_EEPROM ) & EE_DO ) ) break;   /* the dummy zero */
        outw( net_port + SCB_EEPROM, EE_CS | EE_DI ); eedelay();
    }
    /* finish the word out */
    for ( i = 0; i < 16; i++ ) { outw( net_port + SCB_EEPROM, EE_CS | EE_SK ); eedelay(); outw( net_port + SCB_EEPROM, EE_CS ); eedelay(); }
    outw( net_port + SCB_EEPROM, 0 );
    return ( bits >= 1 && bits <= 8 ) ? bits : 6;
}

int card_find( void )
{
    const PCI_ID *which;
    int isio, i, abits;
    u32 at, need;
    u8 *pool;

    if ( !net_pci_find( eepro_ids, &which ) ) return 1;
    net_pci_enable( net_pciaddr );
    /* the family has a memory BAR (0) and an I/O BAR (1); the SCB is
       reachable through either, and the port is simplest */
    net_port = (u16)net_pci_bar( net_pciaddr, 1, &isio );
    if ( net_port == 0 || !isio ) {
        net_port = (u16)net_pci_bar( net_pciaddr, 0, &isio );
        if ( net_port == 0 || !isio ) return 1;
    }
    net_irq = (u8)net_pci_irqline( net_pciaddr );
    net_bus = NICB_PCI;
    net_flags = NICF_WORD;

    outl( net_port + SCB_PORT, 0 );             /* software reset */
    for ( i = 0; i < 10000; i++ ) eedelay();
    outw( net_port + SCB_CMD, 0 );              /* mask off interrupts */

    abits = ee_abits();
    for ( i = 0; i < 3; i++ ) {
        u16 w = ee_word( i, abits );
        net_mac[i * 2] = (u8)w;
        net_mac[i * 2 + 1] = (u8)( w >> 8 );
    }
    if ( !net_mac_ok( net_mac ) ) return 1;

    if ( cmdcb == 0 ) {
        need = RFDN * RFDSZ + TXN * TXCBSZ + 128 + 16;
        pool = net_alloc( need );
        if ( pool == 0 ) return 1;
        at = align16( (u32)pool );
        cmdcb = (u8 *)at;                   at += 128;
        for ( i = 0; i < RFDN; i++ ) { rfd[i] = (u8 *)at; at += RFDSZ; }
        for ( i = 0; i < TXN; i++ ) { txcb[i] = (u8 *)at; at += TXCBSZ; }
    }
    net_strcpy( net_name, "Intel 8255x (PRO/100)" );
    return 0;
}

/* run one command block through the CU and wait for it to complete */
static int run_cb( u8 *cb )
{
    u32 n;

    if ( !scb_cmd( CUC_START, (u32)cb ) ) return 0;
    for ( n = 0; n < SPIN; n++ ) {
        if ( *(volatile u16 *)cb & CB_C ) return 1;
    }
    return 0;
}

int card_setup( void )
{
    int i;
    u8 *cb = cmdcb;

    outl( net_port + SCB_PORT, 0 );             /* reset */
    for ( i = 0; i < 10000; i++ ) eedelay();
    outw( net_port + SCB_CMD, 0 );

    /* both unit bases absolute (zero) so the links below are addresses */
    scb_cmd( RUC_LOADBASE, 0 );
    scb_cmd( CUC_LOADBASE, 0 );

    /* who we are */
    net_memzero( cb, 32 );
    *(u16 *)( cb + 2 ) = CB_IASETUP | CB_EL;
    net_memcpy( cb + 8, net_mac, 6 );
    if ( !run_cb( cb ) ) return 1;

    /* how to behave */
    net_memzero( cb, 32 );
    *(u16 *)( cb + 2 ) = CB_CONFIG | CB_EL;
    net_memcpy( cb + 8, config_default, 22 );
    if ( !run_cb( cb ) ) return 1;

    /* the receive descriptors, linked in a ring, each able to hold a
       whole frame right behind its 16-byte header (simplified) */
    for ( i = 0; i < RFDN; i++ ) {
        u8 *r = rfd[i];
        net_memzero( r, 16 );
        *(u32 *)( r + 4 ) = (u32)rfd[( i + 1 ) % RFDN];   /* link */
        *(u32 *)( r + 8 ) = 0xFFFFFFFFL;                  /* no buffer descriptor */
        *(u16 *)( r + 14 ) = 1518;                        /* size */
    }
    for ( i = 0; i < TXN; i++ ) {
        u8 *t = txcb[i];
        net_memzero( t, 16 );
        *(u16 *)( t + 0 ) = CB_C;                         /* free */
    }
    rx_cur = tx_cur = 0;

    if ( !scb_cmd( RUC_START, (u32)rfd[0] ) ) return 1;

    outw( net_port + SCB_CMD, (u16)( net_flags & NICF_POLLED ? 0x0100 : 0x0000 ) );  /* mask, or let it interrupt */
    return 0;
}

void card_stop( void )
{
    outl( net_port + SCB_PORT, 0 );             /* reset: CU and RU idle */
    outw( net_port + SCB_CMD, 0x0100 );         /* interrupts masked */
}

int card_send( const u8 *frame, u32 len )
{
    int entry = tx_cur;
    u8 *t = txcb[entry];
    u32 n;

    if ( !( *(volatile u16 *)t & CB_C ) ) {
        for ( n = 0; n < SPIN; n++ ) {
            if ( *(volatile u16 *)t & CB_C ) break;
        }
        if ( !( *(volatile u16 *)t & CB_C ) ) { net_stats.txerrors++; return 0; }
    }
    *(u16 *)( t + 0 ) = 0;                       /* status */
    *(u16 *)( t + 2 ) = CB_TX | CB_EL;
    *(u32 *)( t + 4 ) = 0;                       /* link (end of list) */
    *(u32 *)( t + 8 ) = 0xFFFFFFFFL;             /* simplified: frame inline */
    *(u16 *)( t + 12 ) = (u16)( len | 0x8000 );  /* byte count, EOF */
    t[14] = 0xE0;                                /* transmit threshold */
    t[15] = 0;                                   /* no buffer descriptors */
    net_memcpy( t + 16, frame, len );
    if ( !scb_cmd( CUC_START, (u32)t ) ) { net_stats.txerrors++; return 0; }
    tx_cur = ( entry + 1 ) % TXN;
    return 1;
}

static void rx_drain( void )
{
    u32 count, len, n;
    u8 *r, *slot;

    for ( n = 0; n < RFDN * 2; n++ ) {
        r = rfd[rx_cur];
        if ( !( *(volatile u16 *)r & CB_C ) ) break;    /* not filled yet */
        count = *(volatile u16 *)( r + 12 );
        len = count & 0x3FFF;                            /* includes the CRC */
        if ( ( count & 0x8000 ) && len >= NIC_HDRLEN + 4 && len <= NIC_MAXFRAME + 4 ) {
            slot = net_rx_begin();
            net_memcpy( slot, r + 16, len - 4 );
            net_rx_commit( len - 4 );
        } else {
            net_stats.rxerrors++;
        }
        *(u16 *)( r + 0 ) = 0;                           /* give it back */
        *(u16 *)( r + 12 ) = 0;
        rx_cur = ( rx_cur + 1 ) % RFDN;
    }
}

int card_service( void )
{
    u16 st;
    u32 n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        st = scb_status();
        if ( !( st & ACK_ALL ) ) break;
        mine = 1;
        outw( net_port + SCB_STATUS, (u16)( st & ACK_ALL ) );   /* ack */
        if ( st & ( ACK_FR | ACK_RNR ) ) rx_drain();
        if ( st & ACK_RNR ) scb_cmd( RUC_RESUME, 0 );           /* the RU stalled */
    }
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { 'E', 'E', 'P', 'R', 'O', '1', '0', '0' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
