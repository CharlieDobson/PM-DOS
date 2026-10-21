/*
 * EL3C509.C - EL3C509.SYS, a driver for the 3Com EtherLink III (3C509)
 *
 * The ISA 10Mbit card in Charlie's 486.  No emulator has it, so this is
 * written to the 3Com specification and the long-standing DOS and Linux
 * drivers and is for the real card to prove - it has NOT been run.
 *
 * THE 3C509 is programmed I/O, no bus master: a frame is pushed into a
 * transmit FIFO through one port and pulled out of a receive FIFO
 * through the same, the card keeping the counts.  Its registers are
 * banked - a WINDOW is selected and then the sixteen ports mean
 * different things - which is the one thing to keep in mind reading
 * this.  Window 1 is the operating window and the driver leaves it
 * selected except while reading the address EEPROM (window 0) or
 * setting the station address (window 2).
 *
 * FINDING IT.  An EtherLink III that the BIOS or a 3Com utility has not
 * configured answers at no address until the ID sequence is sent to the
 * ID port: 255 writes of a walking pattern put every card into the ID
 * state, each card's EEPROM is read back a bit at a time on the data
 * line, and the card is told its I/O base and activated.  /P:base skips
 * all that for a card already at a known port.
 *
 * On the shared base (DRV\NETDRV.H, NETCORE.C).
 */
#include "netdrv.h"

#define ID_PORT     0x110       /* where the ID sequence is written */

/* the command/status register, base+0x0E, in every window */
#define EL_CMD      0x0E
#define EL_STATUS   0x0E

/* commands: the top five bits of the word written to EL_CMD */
#define CMD_RESET       0x0000
#define CMD_SELWIN      0x0800  /* | window number */
#define CMD_RXENABLE    0x2000
#define CMD_RXRESET     0x2800
#define CMD_RXDISCARD   0x4000  /* drop the frame at the top of the RX FIFO */
#define CMD_TXENABLE    0x4800
#define CMD_TXRESET     0x5800
#define CMD_ACKINTR     0x6800  /* | the bits to acknowledge */
#define CMD_SETINTR     0x7000  /* | the interrupt mask */
#define CMD_SETZERO     0x7800  /* | the read-zero mask */
#define CMD_SETFILTER   0x8000  /* | the receive filter */
#define CMD_STATSDIS    0xB000

/* status bits (EL_STATUS) */
#define ST_INTLATCH 0x0001
#define ST_TXAVAIL  0x0008
#define ST_RXCOMP   0x0010
#define ST_CMDBUSY  0x1000

/* window 1 operating registers */
#define W1_TXFIFO   0x00        /* write a frame here */
#define W1_RXFIFO   0x00        /* read one here */
#define W1_RXSTATUS 0x08        /* word: the frame at the top of the FIFO */
#define W1_TXSTATUS 0x0B        /* byte */
#define W1_TXFREE   0x0C        /* word: bytes free in the transmit FIFO */

#define RXS_INCOMPLETE 0x8000   /* the frame is still arriving */
#define RXS_ERROR      0x4000
#define RXS_LEN        0x07FF

#define FILTER_STATION 0x01
#define FILTER_MULTI   0x02     /* receive all multicast frames */
#define FILTER_BCAST   0x04

#define SPIN_CMD    100000L
#define SPIN_TX     400000L

static u16 id_port = ID_PORT;

static const PCI_ID el3_ids[] = { { 0, 0 } };   /* ISA: found by the ID port */

static void el_cmd( u16 cmd )
{
    u32 n;
    outw( net_port + EL_CMD, cmd );
    for ( n = 0; n < SPIN_CMD; n++ ) {
        if ( !( inw( net_port + EL_STATUS ) & ST_CMDBUSY ) ) break;
    }
}

static void el_window( int win )
{
    outw( net_port + EL_CMD, (u16)( CMD_SELWIN | win ) );
}

/* the ID sequence: a walking pattern the cards' logic recognises */
static void id_sequence( void )
{
    u8 al = 0xFF;
    int i;

    outb( (u16)id_port, 0 );
    outb( (u16)id_port, 0 );
    for ( i = 0; i < 255; i++ ) {
        outb( (u16)id_port, al );
        al = (u8)( ( al << 1 ) ^ ( ( al & 0x80 ) ? 0xCF : 0x00 ) );
    }
}

/* read a word of a card's EEPROM through the ID port, a bit at a time */
static u16 id_eeprom( int addr )
{
    u16 val = 0;
    int i;

    outb( (u16)id_port, (u8)( 0x80 | ( addr & 0x3F ) ) );   /* read-register command */
    for ( i = 0; i < 162; i++ ) ;                            /* the card needs time */
    for ( i = 0; i < 16; i++ ) {
        val = (u16)( ( val << 1 ) | ( inw( (u16)id_port ) & 1 ) );
    }
    return val;
}

/* bring one unconfigured card to a known base and activate it */
static int id_activate( void )
{
    u16 w;
    int i;

    /* try a few ID ports: the sequence selects the port too */
    for ( id_port = 0x110; id_port <= 0x1F0; id_port += 0x10 ) {
        id_sequence();
        outb( (u16)id_port, 0xD0 );             /* global reset of the ID state */
        id_sequence();
        /* the manufacturer ID word must be 3Com's 0x6D50 */
        w = id_eeprom( 7 );
        if ( w == 0x6D50 ) break;
    }
    if ( id_port > 0x1F0 ) return 0;

    /* the card's configured I/O base is EEPROM word 8, low 5 bits times
       16 plus 0x200 */
    w = id_eeprom( 8 );
    net_port = (u16)( 0x200 + ( ( w & 0x1F ) << 4 ) );
    /* the station address is EEPROM words 0..2, big-endian */
    for ( i = 0; i < 3; i++ ) {
        w = id_eeprom( i );
        net_mac[i * 2] = (u8)( w >> 8 );
        net_mac[i * 2 + 1] = (u8)w;
    }
    /* activate: tell the card its tag and move it from ID state to the
       command state at its base (write 0xFF then the base-select) */
    outb( (u16)id_port, (u8)( 0xE0 | 0 ) );     /* set the tag to 0 */
    outb( (u16)id_port, 0xFF );                 /* activate and map at the base */
    return 1;
}

int card_find( void )
{
    int i;
    u16 w;

    if ( net_opt_port ) {
        /* a card already at a known base: read its address from the
           EEPROM in window 0 */
        net_port = (u16)net_opt_port;
        el_window( 0 );
        for ( i = 0; i < 3; i++ ) {
            u32 n;
            outw( net_port + 0x0A, (u16)( 0x80 | i ) );     /* EEPROM read command */
            for ( n = 0; n < 5000; n++ ) ;
            w = inw( net_port + 0x0C );
            net_mac[i * 2] = (u8)( w >> 8 );
            net_mac[i * 2 + 1] = (u8)w;
        }
    } else {
        if ( !id_activate() ) return 1;
    }
    if ( !net_mac_ok( net_mac ) ) return 1;

    net_bus = NICB_ISA;
    net_flags = NICF_WORD;
    (void)el3_ids;
    net_strcpy( net_name, "3Com EtherLink III (3C509)" );
    return 0;
}

int card_setup( void )
{
    int i;

    el_cmd( CMD_RESET );
    /* the station address goes in window 2 */
    el_window( 2 );
    for ( i = 0; i < 6; i++ ) outb( net_port + i, net_mac[i] );

    el_window( 1 );
    el_cmd( CMD_RXRESET );
    el_cmd( CMD_TXRESET );
    el_cmd( CMD_STATSDIS );
    /* station, broadcast AND multicast: the NetBEUI functional address
       03-00-00-00-00-01 is a multicast, so a card that is to carry NetBEUI
       must take multicast frames in, not only station-match and broadcast */
    el_cmd( (u16)( CMD_SETFILTER | FILTER_STATION | FILTER_MULTI | FILTER_BCAST ) );
    el_cmd( CMD_RXENABLE );
    el_cmd( CMD_TXENABLE );
    /* acknowledge everything, then let RX-complete and TX-available
       interrupt unless polled */
    el_cmd( (u16)( CMD_ACKINTR | 0xFF ) );
    if ( net_flags & NICF_POLLED ) el_cmd( CMD_SETINTR );
    else el_cmd( (u16)( CMD_SETINTR | ST_RXCOMP | ST_TXAVAIL | ST_INTLATCH ) );
    return 0;
}

void card_stop( void )
{
    el_window( 1 );
    el_cmd( CMD_SETINTR );                       /* mask everything */
    el_cmd( 0x1800 );                            /* RX disable */
    el_cmd( 0x5000 );                            /* TX disable */
}

int card_send( const u8 *frame, u32 len )
{
    static u8 pad[64];
    u32 n, words;
    u32 use = len;

    if ( use < 60 ) {
        net_memzero( pad, sizeof( pad ) );
        net_memcpy( pad, frame, len );
        frame = pad;
        use = 60;
    }
    /* wait for room: the preamble is four bytes plus the frame */
    for ( n = 0; n < SPIN_TX; n++ ) {
        if ( inw( net_port + W1_TXFREE ) >= use + 4 ) break;
    }
    if ( n == SPIN_TX ) { net_stats.txerrors++; return 0; }
    outw( net_port + W1_TXFIFO, (u16)use );      /* length */
    outw( net_port + W1_TXFIFO, 0 );             /* the second preamble word */
    words = use / 2;
    for ( n = 0; n < words; n++ ) outw( net_port + W1_TXFIFO, ( (u16)frame[n * 2] ) | ( (u16)frame[n * 2 + 1] << 8 ) );
    if ( use & 1 ) outw( net_port + W1_TXFIFO, frame[use - 1] );
    return 1;
}

static void rx_drain( void )
{
    u16 status;
    u32 len, words, n;
    u8 *slot;

    for ( n = 0; n < 16; n++ ) {
        status = inw( net_port + W1_RXSTATUS );
        if ( status & RXS_INCOMPLETE ) break;    /* still arriving, or none */
        if ( ( status & 0x8000 ) == 0 && ( status & RXS_LEN ) == 0 && !( status & RXS_ERROR ) ) break;
        len = status & RXS_LEN;
        if ( ( status & RXS_ERROR ) || len < NIC_HDRLEN || len > NIC_MAXFRAME ) {
            net_stats.rxerrors++;
        } else {
            slot = net_rx_begin();
            words = ( len + 1 ) / 2;
            {
                u32 i;
                u16 *w = (u16 *)slot;
                for ( i = 0; i < words; i++ ) w[i] = inw( net_port + W1_RXFIFO );
            }
            net_rx_commit( len );
        }
        el_cmd( CMD_RXDISCARD );                 /* drop this frame from the FIFO */
        for ( n = 0; n < SPIN_CMD; n++ ) if ( !( inw( net_port + EL_STATUS ) & ST_CMDBUSY ) ) break;
    }
}

int card_service( void )
{
    u16 st;
    u32 n;
    int mine = 0;

    for ( n = 0; n < 8; n++ ) {
        st = inw( net_port + EL_STATUS );
        if ( !( st & ( ST_RXCOMP | ST_TXAVAIL | ST_INTLATCH ) ) ) break;
        mine = 1;
        if ( st & ST_RXCOMP ) rx_drain();
        el_cmd( (u16)( CMD_ACKINTR | ( st & 0xFF ) ) );
    }
    return mine;
}

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_NIC,
    { '3', 'C', '5', '0', '9', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
