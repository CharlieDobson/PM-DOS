/*
 * EL3C90X.C - EL3C90X.SYS, a driver for the 3Com 3C90x (Boomerang)
 *
 * The PCI 10/100 EtherLink XL, 3Com's bus-master card: the 3C900 and
 * 3C905 (Boomerang), and the 3C905B/C (Cyclone/Tornado), which speak
 * the same descriptor lists.  Like the 3C509 it has banked WINDOW
 * registers for setup, and like the PCI cards above it moves frames by
 * DMA - a "download" list for transmit and an "upload" list for
 * receive, each descriptor owning a buffer and the chip walking the
 * list on its own.  No emulator has it, so this is written to the
 * 3Com specification and the 3c59x driver and is for Charlie's real
 * 3C905 to prove - it has NOT been run.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).  DMA is the driver's
 * own pointers (the identity map).
 */
#include "netdrv.h"

/* the command/status register, base+0x0E, in every window */
#define EL_CMD      0x0E
#define EL_STATUS   0x0E
#define ST_CMDBUSY  0x1000

#define CMD_RESET       0x0000
#define CMD_SELWIN      0x0800
#define CMD_RXENABLE    0x2000
#define CMD_RXRESET     0x2800
#define CMD_UPUNSTALL   0x3001
#define CMD_DNUNSTALL   0x3003
#define CMD_TXENABLE    0x4800
#define CMD_TXRESET     0x5800
#define CMD_ACKINTR     0x6800
#define CMD_SETINTR     0x7000
#define CMD_SETIND      0x7800
#define CMD_SETFILTER   0x8000

/* the bus-master registers, at fixed offsets whatever the window */
#define EL_DNLISTPTR    0x24    /* the transmit (download) list */
#define EL_UPLISTPTR    0x38    /* the receive (upload) list */

/* interrupt/indication bits */
#define I_UPCOMPLETE 0x0400
#define I_DNCOMPLETE 0x0200
#define I_INTLATCH   0x0001

/* filter bits */
#define FILTER_STATION 0x01
#define FILTER_MULTI   0x02     /* receive all multicast frames */
#define FILTER_BCAST   0x04

/* descriptor bits */
#define LAST_FRAG   0x80000000L
#define DN_INDICATE 0x80008000L /* FSH: request a download-complete, len below */
#define UP_COMPLETE 0x00008000L
#define UP_ERROR    0x00004000L
#define UP_LEN      0x00001FFFL

#define RXN     16
#define TXN     16
#define BUFSZ   1536

#define SPIN_CMD    100000L
#define SPIN_TX     400000L

/* a descriptor: next, status/header, buffer address, buffer length */
typedef struct { u32 next, status, addr, length; } DESC;

static volatile DESC *rxd, *txd;
static u8  *rxbuf[RXN];
static u8  *txbuf[TXN];
static int  rx_cur, tx_cur;

static const PCI_ID el90_ids[] = {
    { 0x10B7, 0x9000 }, { 0x10B7, 0x9001 },     /* 3C900 */
    { 0x10B7, 0x9050 }, { 0x10B7, 0x9055 },     /* 3C905 / 905B */
    { 0x10B7, 0x9200 }, { 0x10B7, 0x9201 },     /* 3C905C */
    { 0x10B7, 0x9800 }, { 0x10B7, 0x9805 },
    { 0, 0 }
};

static u32 align16( u32 p ) { return ( p + 15 ) & ~15L; }

static void el_cmd( u16 cmd )
{
    u32 n;
    outw( net_port + EL_CMD, cmd );
    for ( n = 0; n < SPIN_CMD; n++ ) {
        if ( !( inw( net_port + EL_STATUS ) & ST_CMDBUSY ) ) break;
    }
}

static void el_window( int win ) { outw( net_port + EL_CMD, (u16)( CMD_SELWIN | win ) ); }

int card_find( void )
{
    const PCI_ID *which;
    int isio, i;
    u32 at, need;
    u8 *pool;

    if ( !net_pci_find( el90_ids, &which ) ) return 1;
    net_pci_enable( net_pciaddr );
    net_port = (u16)net_pci_bar( net_pciaddr, 0, &isio );
    if ( net_port == 0 || !isio ) return 1;
    net_irq = (u8)net_pci_irqline( net_pciaddr );
    net_bus = NICB_PCI;
    net_flags = NICF_WORD;

    el_cmd( CMD_RESET );
    for ( i = 0; i < 100000; i++ ) if ( !( inw( net_port + EL_STATUS ) & ST_CMDBUSY ) ) break;

    /* the station address from the EEPROM, window 0 */
    el_window( 0 );
    for ( i = 0; i < 3; i++ ) {
        u32 n;
        outw( net_port + 0x0A, (u16)( 0x80 | ( 0x0A + i ) ) );   /* read OEM node address */
        for ( n = 0; n < 5000; n++ ) ;
        {
            u16 w = inw( net_port + 0x0C );
            net_mac[i * 2] = (u8)( w >> 8 );
            net_mac[i * 2 + 1] = (u8)w;
        }
    }
    if ( !net_mac_ok( net_mac ) ) {
        /* some parts hold it at words 0..2 instead */
        el_window( 0 );
        for ( i = 0; i < 3; i++ ) {
            u32 n;
            outw( net_port + 0x0A, (u16)( 0x80 | i ) );
            for ( n = 0; n < 5000; n++ ) ;
            {
                u16 w = inw( net_port + 0x0C );
                net_mac[i * 2] = (u8)( w >> 8 );
                net_mac[i * 2 + 1] = (u8)w;
            }
        }
        if ( !net_mac_ok( net_mac ) ) return 1;
    }

    if ( rxd == 0 ) {
        need = RXN * sizeof( DESC ) + TXN * sizeof( DESC ) + ( RXN + TXN ) * BUFSZ + 16;
        pool = net_alloc( need );
        if ( pool == 0 ) return 1;
        at = align16( (u32)pool );
        rxd = (volatile DESC *)at;  at += RXN * sizeof( DESC );
        txd = (volatile DESC *)at;  at += TXN * sizeof( DESC );
        for ( i = 0; i < RXN; i++ ) { rxbuf[i] = (u8 *)at; at += BUFSZ; }
        for ( i = 0; i < TXN; i++ ) { txbuf[i] = (u8 *)at; at += BUFSZ; }
    }
    net_strcpy( net_name, "3Com 3C90x (Boomerang)" );
    return 0;
}

int card_setup( void )
{
    int i;

    el_cmd( CMD_RXRESET );
    el_cmd( CMD_TXRESET );

    /* the station address, window 2 */
    el_window( 2 );
    for ( i = 0; i < 6; i++ ) outb( net_port + i, net_mac[i] );

    /* the upload (receive) ring */
    for ( i = 0; i < RXN; i++ ) {
        rxd[i].next = (u32)&rxd[( i + 1 ) % RXN];
        rxd[i].status = 0;
        rxd[i].addr = (u32)rxbuf[i];
        rxd[i].length = BUFSZ | LAST_FRAG;
    }
    for ( i = 0; i < TXN; i++ ) {
        txd[i].next = 0;
        txd[i].status = DN_INDICATE;             /* free (download complete) */
        txd[i].addr = (u32)txbuf[i];
        txd[i].length = 0;
    }
    rx_cur = tx_cur = 0;

    el_window( 7 );                              /* the operating window */
    outl( net_port + EL_UPLISTPTR, (u32)&rxd[0] );
    outl( net_port + EL_DNLISTPTR, 0 );

    /* station, broadcast AND multicast (NetBEUI's functional address
       03-00-00-00-00-01 is a multicast) */
    el_cmd( (u16)( CMD_SETFILTER | FILTER_STATION | FILTER_MULTI | FILTER_BCAST ) );
    el_cmd( CMD_RXENABLE );
    el_cmd( CMD_TXENABLE );
    el_cmd( CMD_UPUNSTALL );
    el_cmd( (u16)( CMD_ACKINTR | 0xFF ) );
    if ( net_flags & NICF_POLLED ) { el_cmd( CMD_SETINTR ); el_cmd( CMD_SETIND ); }
    else {
        el_cmd( (u16)( CMD_SETINTR | I_UPCOMPLETE | I_DNCOMPLETE | I_INTLATCH ) );
        el_cmd( (u16)( CMD_SETIND | I_UPCOMPLETE | I_DNCOMPLETE | I_INTLATCH ) );
    }
    return 0;
}

void card_stop( void )
{
    el_window( 7 );
    el_cmd( CMD_SETINTR );                        /* mask */
    el_cmd( 0x1800 );                             /* RX disable */
    el_cmd( 0x5000 );                             /* TX disable */
}

int card_send( const u8 *frame, u32 len )
{
    static u8 pad[64];
    int entry = tx_cur;
    volatile DESC *d = &txd[entry];
    u32 n;

    /* one frame is downloaded at a time, and the engine clears DnListPtr
       when it is done with it - every chip does that, where only the
       Cyclone and Tornado mark the descriptor */
    if ( inl( net_port + EL_DNLISTPTR ) != 0 ) {  /* download not complete: busy */
        for ( n = 0; n < SPIN_TX; n++ ) if ( inl( net_port + EL_DNLISTPTR ) == 0 ) break;
        if ( inl( net_port + EL_DNLISTPTR ) != 0 ) { net_stats.txerrors++; return 0; }
    }
    if ( len < 60 ) {
        net_memzero( pad, sizeof( pad ) );
        net_memcpy( pad, frame, len );
        net_memcpy( txbuf[entry], pad, 60 );
        len = 60;
    } else {
        net_memcpy( txbuf[entry], frame, len );
    }
    d->next = 0;
    d->status = 0x00008000L | len;                /* FSH: request dn-complete, length */
    d->addr = (u32)txbuf[entry];
    d->length = len | LAST_FRAG;
    el_window( 7 );
    /* point the download engine at this descriptor and unstall it */
    outl( net_port + EL_DNLISTPTR, (u32)d );
    el_cmd( CMD_DNUNSTALL );
    tx_cur = ( entry + 1 ) % TXN;
    return 1;
}

static void rx_drain( void )
{
    volatile DESC *d;
    u32 status, len, n;
    u8 *slot;

    for ( n = 0; n < RXN * 2; n++ ) {
        d = &rxd[rx_cur];
        status = d->status;
        if ( !( status & UP_COMPLETE ) ) break;
        if ( status & UP_ERROR ) {
            net_stats.rxerrors++;
        } else {
            len = status & UP_LEN;
            if ( len >= NIC_HDRLEN && len <= NIC_MAXFRAME ) {
                slot = net_rx_begin();
                net_memcpy( slot, rxbuf[rx_cur], len );
                net_rx_commit( len );
            } else {
                net_stats.rxerrors++;
            }
        }
        d->status = 0;                            /* give it back */
        rx_cur = ( rx_cur + 1 ) % RXN;
    }
    el_window( 7 );
    el_cmd( CMD_UPUNSTALL );                       /* keep the upload engine going */
}

int card_service( void )
{
    u16 st;
    u32 n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        st = inw( net_port + EL_STATUS );
        if ( !( st & ( I_UPCOMPLETE | I_DNCOMPLETE | I_INTLATCH ) ) ) break;
        mine = 1;
        if ( st & I_UPCOMPLETE ) rx_drain();
        el_cmd( (u16)( CMD_ACKINTR | ( st & 0xFF ) ) );
    }
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { '3', 'C', '9', '0', 'x', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
