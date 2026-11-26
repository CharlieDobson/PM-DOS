/*
 * NICTEST.C - NICTEST.EXE, a network card driver proved from the wire
 *
 * A native program that finds the card driver among the 32-bit
 * drivers (21h/F1h AL=25h, INC\NDRV.INC: the row with NDTF_NIC), asks
 * it what the card is (INC\NIC.INC NIC_INFO), and then speaks to the
 * network through it with the three things every stack has to be
 * able to do before it can do anything else:
 *
 *   DHCP   a DISCOVER to everybody, the OFFER back, a REQUEST for
 *          what was offered, and the ACK that gives an address, a
 *          mask, a gateway and a name server;
 *   ARP    who has the gateway's address - and the answer, which is
 *          the gateway's station address;
 *   ping   three ICMP echoes to the gateway, 32 bytes each, and the
 *          replies, checked byte for byte.
 *
 * Every emulator's user-mode network answers all three the same way
 * - the gateway is 10.0.2.2, the address given is 10.0.2.15 - so the
 * lines here are the same on QEMU, 86Box and DOSBox-X, and a card
 * driver that passes on one of them has sent and received frames.
 * The frames are built here by hand, big-endian field by field, with
 * no stack underneath: this is the test of the driver, and the stack
 * is what comes after it.
 *
 * Every line is a check, and the last line is how many failed.
 * Needs a card driver loaded by CONFIG.SYS - NE2000.SYS - and a
 * network behind the card that serves DHCP.  Built as FC.EXE is:
 * CSTART.ASM is the startup and the INT 21h thunk; what a library
 * would have supplied is at the bottom.
 */
#include "ndrv.h"
#include "nic.h"

void __cdecl int21( REGS *regs );

#define TRUE         1
#define FALSE        0
#define NULL         ((void *)0)

#define OUT_BUF      4096
#define FRAME_BUF    1536
#define DOTS_COL     58                 /* where the verdict goes */

#define ETH_IP       0x0800
#define ETH_ARP      0x0806
#define IP_ICMP      1
#define IP_UDP       17
#define BOOTP_LEN    300                /* the least a server takes */
#define DHCP_XID     0x504D4453L        /* 'PMDS' */
#define PING_ID      0x504D             /* 'PM' */
#define PING_DATA    32

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

/* how long to wait, in hundredths, and how often to ask again */
#define WAIT_DHCP    300
#define TRIES_DHCP   4
#define WAIT_ARP     100
#define TRIES_ARP    3
#define WAIT_PING    200
#define PINGS        3

static uint8_t  out_buf[OUT_BUF];
static unsigned int out_len;
static unsigned int failures;
static int      column;

static unsigned char nic_row;
static uint8_t *rx, *tx;
static NIC_INFOBLOCK info;

static uint8_t  my_mac[6];
static uint8_t  my_ip[4];
static uint8_t  gw_ip[4], gw_mac[6];
static uint8_t  server_ip[4], mask_ip[4], dns_ip[4];
static uint32_t lease;
static uint8_t  offered[4];
static int      have_ip;
static uint8_t  ping_ttl;

static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t bcast_ip[4]  = { 255, 255, 255, 255 };
static const uint8_t zero_ip[4]   = { 0, 0, 0, 0 };
static const char ping_text[PING_DATA + 1] = "PM-DOS NICTEST: an echo, please.";

static void mem_copy( void *to, const void *from, unsigned int len );
static void mem_zero( void *to, unsigned int len );
static int  mem_same( const void *a, const void *b, unsigned int len );

/* ---------------------------------------------------------------- */
/* output                                                            */
/* ---------------------------------------------------------------- */

static void clear( REGS *regs )
{
    mem_zero( regs, sizeof( *regs ) );
}

static void out_flush( void )
{
    REGS regs;

    if ( out_len ) {
        clear( &regs );
        regs.eax = 0x4000;
        regs.ebx = 1;
        regs.ecx = out_len;
        regs.edx = (uint32_t)out_buf;
        int21( &regs );
        out_len = 0;
    }
}

static void test_exit( int code )
{
    REGS regs;

    out_flush();
    clear( &regs );
    regs.eax = 0x4C00 | (code & 0xFF);
    int21( &regs );
}

static void out_ch( char ch )
{
    if ( out_len == OUT_BUF ) out_flush();
    out_buf[out_len++] = (uint8_t)ch;
    column = ( ch == '\n' ) ? 0 : column + 1;
}

static void out_str( const char *str )
{
    while ( *str ) out_ch( *str++ );
}

static void out_nl( void )
{
    out_str( "\r\n" );
}

static void out_dec( unsigned int value )
{
    char digits[12];
    int  n = 0;

    do {
        digits[n++] = (char)( '0' + value % 10 );
        value /= 10;
    } while ( value );
    while ( n ) out_ch( digits[--n] );
}

static void out_hex( uint32_t value, int digits )
{
    int shift;

    for ( shift = ( digits - 1 ) * 4; shift >= 0; shift -= 4 ) {
        out_ch( "0123456789ABCDEF"[( value >> shift ) & 15] );
    }
}

static void out_ip( const uint8_t *ip )
{
    int i;

    for ( i = 0; i < 4; i++ ) {
        if ( i ) out_ch( '.' );
        out_dec( ip[i] );
    }
}

static void out_mac( const uint8_t *mac )
{
    int i;

    for ( i = 0; i < 6; i++ ) {
        if ( i ) out_ch( '-' );
        out_hex( mac[i], 2 );
    }
}

/* a check's line: two blanks, the text, dots to the verdict's column */
static void check( const char *text )
{
    out_str( "  " );
    out_str( text );
    out_ch( ' ' );
    while ( column < DOTS_COL ) out_ch( '.' );
    out_ch( ' ' );
}

static void ok( void )
{
    out_str( " ok" );
    out_nl();
}

static void fail( void )
{
    failures++;
    out_str( " FAIL" );
    out_nl();
}

/* ---------------------------------------------------------------- */
/* the clock, in hundredths of a second (2Ch counts ticks)           */
/* ---------------------------------------------------------------- */

static unsigned int now_cs( void )
{
    REGS     regs;
    unsigned int h, m, s, cs;

    clear( &regs );
    regs.eax = 0x2C00;
    int21( &regs );
    h  = ( regs.ecx >> 8 ) & 0xFF;
    m  = regs.ecx & 0xFF;
    s  = ( regs.edx >> 8 ) & 0xFF;
    cs = regs.edx & 0xFF;
    return ( ( h * 60 + m ) * 60 + s ) * 100 + cs;
}

static unsigned int since( unsigned int start )
{
    unsigned int now = now_cs();

    if ( now < start ) now += 8640000L;     /* midnight went by */
    return now - start;
}

/* ---------------------------------------------------------------- */
/* the kernel and the driver                                         */
/* ---------------------------------------------------------------- */

static void *mem_alloc( unsigned int size )
{
    REGS regs;

    clear( &regs );
    regs.eax = 0x4800;
    regs.ebx = size;
    int21( &regs );
    return CF( regs ) ? NULL : (void *)regs.eax;
}

static unsigned int nd_count( unsigned int *max )
{
    REGS regs;

    clear( &regs );
    regs.eax = ( (uint32_t)NDF_COUNT << 16 ) | 0xF125;
    int21( &regs );
    if ( CF( regs ) ) return 0;
    *max = regs.eax;
    return regs.ecx;
}

static int nd_info( unsigned char row, ND_ROW *out )
{
    REGS regs;

    clear( &regs );
    regs.eax = ( (uint32_t)NDF_INFO << 16 ) | 0xF125;
    regs.edx = row;
    regs.edi = (uint32_t)out;
    int21( &regs );
    return !CF( regs );
}

/* the driver's CALL entry: 0 and *answer, or the error it gave */
static unsigned int nd_call( unsigned int fn, void *buf, unsigned int len, uint32_t *answer )
{
    REGS regs;

    clear( &regs );
    regs.eax = ( (uint32_t)NDF_CALL << 16 ) | 0xF125;
    regs.edx = nic_row;
    regs.ebx = fn;
    regs.edi = (uint32_t)buf;
    regs.ecx = len;
    int21( &regs );
    if ( CF( regs ) ) return regs.eax & 0xFFFF;
    *answer = regs.eax;
    return 0;
}

static unsigned int nic_send( const uint8_t *frame, unsigned int len )
{
    uint32_t answer;

    return nd_call( NIC_SEND, (void *)frame, len, &answer );
}

static unsigned int nic_recv( uint8_t *buf )
{
    uint32_t answer = 0;

    if ( nd_call( NIC_RECV, buf, FRAME_BUF, &answer ) ) return 0;
    return answer;
}

/* ---------------------------------------------------------------- */
/* frames, field by field                                            */
/* ---------------------------------------------------------------- */

static void put16( uint8_t *p, uint32_t value )
{
    p[0] = (uint8_t)( value >> 8 );
    p[1] = (uint8_t)value;
}

static void put32( uint8_t *p, uint32_t value )
{
    p[0] = (uint8_t)( value >> 24 );
    p[1] = (uint8_t)( value >> 16 );
    p[2] = (uint8_t)( value >> 8 );
    p[3] = (uint8_t)value;
}

static uint32_t get16( const uint8_t *p )
{
    return ( (uint32_t)p[0] << 8 ) | p[1];
}

static uint32_t get32( const uint8_t *p )
{
    return ( (uint32_t)p[0] << 24 ) | ( (uint32_t)p[1] << 16 ) | ( (uint32_t)p[2] << 8 ) | p[3];
}

/* the Internet checksum: one's complement of the one's complement sum */
static uint32_t checksum( const uint8_t *data, unsigned int len )
{
    uint32_t sum = 0;

    while ( len > 1 ) {
        sum += get16( data );
        data += 2;
        len -= 2;
    }
    if ( len ) sum += (uint32_t)data[0] << 8;
    while ( sum >> 16 ) sum = ( sum & 0xFFFF ) + ( sum >> 16 );
    return ( ~sum ) & 0xFFFF;
}

static uint8_t *eth_head( uint8_t *f, const uint8_t *dst, uint32_t type )
{
    mem_copy( f, dst, 6 );
    mem_copy( f + 6, my_mac, 6 );
    put16( f + 12, type );
    return f + NIC_HDRLEN;
}

/* an IP header of 20 bytes, its checksum filled in when the whole
   datagram's length is known */
static uint8_t *ip_head( uint8_t *p, const uint8_t *src, const uint8_t *dst, uint32_t proto, uint32_t total, uint32_t id )
{
    p[0] = 0x45;
    p[1] = 0;
    put16( p + 2, total );
    put16( p + 4, id );
    put16( p + 6, 0 );
    p[8] = 64;
    p[9] = (uint8_t)proto;
    put16( p + 10, 0 );
    mem_copy( p + 12, src, 4 );
    mem_copy( p + 16, dst, 4 );
    put16( p + 10, checksum( p, 20 ) );
    return p + 20;
}

/* a DHCP message, as a frame: DISCOVER, or REQUEST for what was
   offered.  The whole frame's length comes back */
static unsigned int build_dhcp( uint8_t *f, int kind )
{
    uint8_t *ip, *udp, *bp, *opt;
    unsigned int bootp_len, udp_len;

    ip  = eth_head( f, bcast_mac, ETH_IP );
    udp = ip + 20;
    bp  = udp + 8;
    mem_zero( bp, BOOTP_LEN + 32 );
    bp[0] = 1;                          /* a request */
    bp[1] = 1;                          /* Ethernet */
    bp[2] = 6;
    put32( bp + 4, DHCP_XID );
    put16( bp + 10, 0x8000 );           /* answer to everybody: a
                                           client with no address yet */
    mem_copy( bp + 28, my_mac, 6 );
    bp[236] = 99;                       /* the magic cookie */
    bp[237] = 130;
    bp[238] = 83;
    bp[239] = 99;
    opt = bp + 240;
    *opt++ = 53;                        /* message type */
    *opt++ = 1;
    *opt++ = (uint8_t)kind;
    if ( kind == DHCP_REQUEST ) {
        *opt++ = 50;                    /* the address offered */
        *opt++ = 4;
        mem_copy( opt, offered, 4 );
        opt += 4;
        *opt++ = 54;                    /* by this server */
        *opt++ = 4;
        mem_copy( opt, server_ip, 4 );
        opt += 4;
    }
    *opt++ = 55;                        /* what the answer should say */
    *opt++ = 4;
    *opt++ = 1;                         /* the mask */
    *opt++ = 3;                         /* the gateway */
    *opt++ = 6;                         /* the name server */
    *opt++ = 51;                        /* the lease */
    *opt++ = 255;
    bootp_len = (unsigned int)( opt - bp );
    if ( bootp_len < BOOTP_LEN ) bootp_len = BOOTP_LEN;
    udp_len = 8 + bootp_len;
    put16( udp, 68 );
    put16( udp + 2, 67 );
    put16( udp + 4, udp_len );
    put16( udp + 6, 0 );                /* no checksum: allowed over IPv4 */
    ip_head( ip, zero_ip, bcast_ip, IP_UDP, 20 + udp_len, kind );
    return NIC_HDRLEN + 20 + udp_len;
}

/* ARP: who has ip?  And the answer to one who asks for ours */
static unsigned int build_arp( uint8_t *f, int reply, const uint8_t *to_mac, const uint8_t *to_ip )
{
    uint8_t *p;

    p = eth_head( f, reply ? to_mac : bcast_mac, ETH_ARP );
    put16( p, 1 );                      /* Ethernet */
    put16( p + 2, ETH_IP );
    p[4] = 6;
    p[5] = 4;
    put16( p + 6, reply ? 2 : 1 );
    mem_copy( p + 8, my_mac, 6 );
    mem_copy( p + 14, my_ip, 4 );
    mem_copy( p + 18, reply ? to_mac : (const uint8_t *)"\0\0\0\0\0\0", 6 );
    mem_copy( p + 24, to_ip, 4 );
    return NIC_HDRLEN + 28;
}

/* an echo request to the gateway, PING_DATA bytes of text in it */
static unsigned int build_ping( uint8_t *f, uint32_t seq )
{
    uint8_t *ip, *icmp;

    ip = eth_head( f, gw_mac, ETH_IP );
    icmp = ip + 20;
    icmp[0] = 8;
    icmp[1] = 0;
    put16( icmp + 2, 0 );
    put16( icmp + 4, PING_ID );
    put16( icmp + 6, seq );
    mem_copy( icmp + 8, ping_text, PING_DATA );
    put16( icmp + 2, checksum( icmp, 8 + PING_DATA ) );
    ip_head( ip, my_ip, gw_ip, IP_ICMP, 20 + 8 + PING_DATA, 0x100 + seq );
    return NIC_HDRLEN + 20 + 8 + PING_DATA;
}

/* ---------------------------------------------------------------- */
/* what arrives                                                      */
/* ---------------------------------------------------------------- */

/* the IP datagram inside a frame of ours, if it is one and whole:
   its header, with the payload length and protocol out */
static const uint8_t *ip_in( const uint8_t *f, unsigned int len, unsigned int *payload, uint32_t proto )
{
    const uint8_t *ip;
    unsigned int hlen, total;

    if ( len < NIC_HDRLEN + 20 || get16( f + 12 ) != ETH_IP ) return NULL;
    ip = f + NIC_HDRLEN;
    if ( ( ip[0] >> 4 ) != 4 ) return NULL;
    hlen = ( ip[0] & 15 ) * 4;
    total = get16( ip + 2 );
    if ( hlen < 20 || total < hlen || total > len - NIC_HDRLEN ) return NULL;
    if ( ip[9] != proto ) return NULL;
    if ( get16( ip + 6 ) & 0x1FFF ) return NULL;    /* a fragment */
    *payload = total - hlen;
    return ip;
}

/* a DHCP answer of the kind wanted, to our transaction: what it says
   is kept.  1 if it was one */
static int take_dhcp( const uint8_t *f, unsigned int len, int kind )
{
    const uint8_t *ip, *udp, *bp, *opt, *end;
    unsigned int payload;
    uint32_t code;
    unsigned int olen;
    int type = 0;

    ip = ip_in( f, len, &payload, IP_UDP );
    if ( ip == NULL || payload < 8 + 240 ) return FALSE;
    udp = ip + ( ip[0] & 15 ) * 4;
    if ( get16( udp + 2 ) != 68 ) return FALSE;
    bp = udp + 8;
    if ( bp[0] != 2 || get32( bp + 4 ) != DHCP_XID ) return FALSE;
    if ( bp[236] != 99 || bp[237] != 130 || bp[238] != 83 || bp[239] != 99 ) return FALSE;
    opt = bp + 240;
    end = udp + payload;
    while ( opt < end && *opt != 255 ) {
        code = *opt++;
        if ( code == 0 ) continue;
        if ( opt >= end ) break;
        olen = *opt++;
        if ( opt + olen > end ) break;
        switch ( code ) {
        case 53: if ( olen == 1 ) type = opt[0]; break;
        case 54: if ( olen == 4 ) mem_copy( server_ip, opt, 4 ); break;
        case 1:  if ( olen == 4 ) mem_copy( mask_ip, opt, 4 ); break;
        case 3:  if ( olen >= 4 ) mem_copy( gw_ip, opt, 4 ); break;
        case 6:  if ( olen >= 4 ) mem_copy( dns_ip, opt, 4 ); break;
        case 51: if ( olen == 4 ) lease = get32( opt ); break;
        }
        opt += olen;
    }
    if ( type != kind ) return FALSE;
    mem_copy( offered, bp + 16, 4 );
    if ( mem_same( server_ip, zero_ip, 4 ) ) mem_copy( server_ip, bp + 20, 4 );
    return TRUE;
}

/* an ARP reply from the gateway: its station address is kept */
static int take_arp_reply( const uint8_t *f, unsigned int len )
{
    const uint8_t *p = f + NIC_HDRLEN;

    if ( len < NIC_HDRLEN + 28 || get16( f + 12 ) != ETH_ARP ) return FALSE;
    if ( get16( p ) != 1 || get16( p + 2 ) != ETH_IP || p[4] != 6 || p[5] != 4 ) return FALSE;
    if ( get16( p + 6 ) != 2 || !mem_same( p + 14, gw_ip, 4 ) ) return FALSE;
    mem_copy( gw_mac, p + 8, 6 );
    return TRUE;
}

/* an ARP request for our address, answered - whoever the frame was
   meant for, this is what a station on the wire owes the others */
static void answer_arp( const uint8_t *f, unsigned int len )
{
    const uint8_t *p = f + NIC_HDRLEN;
    unsigned int n;

    if ( !have_ip || len < NIC_HDRLEN + 28 || get16( f + 12 ) != ETH_ARP ) return;
    if ( get16( p ) != 1 || get16( p + 2 ) != ETH_IP || p[4] != 6 || p[5] != 4 ) return;
    if ( get16( p + 6 ) != 1 || !mem_same( p + 24, my_ip, 4 ) ) return;
    n = build_arp( tx, TRUE, p + 8, p + 14 );
    nic_send( tx, n );
}

/* an echo reply from the gateway with our id, the text intact: its
   sequence number (1..PINGS), or 0 if the frame is not one */
static uint32_t take_ping_any( const uint8_t *f, unsigned int len )
{
    const uint8_t *ip, *icmp;
    unsigned int payload;
    uint32_t seq;

    ip = ip_in( f, len, &payload, IP_ICMP );
    if ( ip == NULL || payload < 8 + PING_DATA ) return 0;
    if ( !mem_same( ip + 12, gw_ip, 4 ) || !mem_same( ip + 16, my_ip, 4 ) ) return 0;
    icmp = ip + ( ip[0] & 15 ) * 4;
    if ( icmp[0] != 0 || get16( icmp + 4 ) != PING_ID ) return 0;
    if ( !mem_same( icmp + 8, ping_text, PING_DATA ) ) return 0;
    seq = get16( icmp + 6 );
    if ( seq < 1 || seq > PINGS ) return 0;
    ping_ttl = ip[8];
    return seq;
}

/* frames, until one is what is wanted or the time is up.  ARP
   requests for our address are answered on the way */
#define WANT_DHCP    1
#define WANT_ARP     2

static int wait_for( int what, unsigned int arg, unsigned int timeout )
{
    unsigned int start = now_cs();
    unsigned int len;

    for ( ;; ) {
        len = nic_recv( rx );
        if ( len ) {
            answer_arp( rx, len );
            switch ( what ) {
            case WANT_DHCP: if ( take_dhcp( rx, len, (int)arg ) ) return TRUE; break;
            case WANT_ARP:  if ( take_arp_reply( rx, len ) ) return TRUE; break;
            }
            continue;
        }
        if ( since( start ) >= timeout ) return FALSE;
    }
}

/* ---------------------------------------------------------------- */
/* the checks                                                        */
/* ---------------------------------------------------------------- */

static int find_driver( void )
{
    ND_ROW row;
    unsigned int count, max, i;
    int j;

    check( "a card driver among the 32-bit drivers" );
    count = nd_count( &max );
    for ( i = 0; i < count; i++ ) {
        if ( !nd_info( (unsigned char)i, &row ) ) continue;
        if ( !( row.flags & NDTF_NIC ) ) continue;
        nic_row = (unsigned char)i;
        out_str( "row " );
        out_dec( i );
        out_str( ", " );
        for ( j = 0; j < 8 && row.name[j] != ' '; j++ ) out_ch( row.name[j] );
        ok();
        return TRUE;
    }
    /* a machine with no card driver says so and tests nothing, as
       NICIRQ does: a DOSBox-X whose build has no network behind its
       card gets this line and no failure */
    out_str( "none of " );
    out_dec( count );
    out_str( " loaded: nothing to test here" );
    out_nl();
    return FALSE;
}

static int ask_info( void )
{
    uint32_t answer;
    unsigned int err;
    int i;

    check( "NIC_INFO: the card, its ports and line, its address" );
    err = nd_call( NIC_INFO, &info, sizeof( info ), &answer );
    if ( err || answer != NICI_LEN ) {
        out_str( "error " );
        out_hex( err, 4 );
        fail();
        return FALSE;
    }
    out_str( info.name );
    out_str( " at " );
    out_hex( info.port, 4 );
    out_str( "h, " );
    if ( info.irq == NIC_NOIRQ ) {
        out_str( "polled" );
    } else {
        out_str( "IRQ " );
        out_dec( info.irq );
    }
    out_str( ( info.flags & NICF_WORD ) ? ", 16-bit, " : ", 8-bit, " );
    out_mac( info.mac );
    mem_copy( my_mac, info.mac, 6 );
    for ( i = 0; i < 6 && my_mac[i] == 0; i++ ) ;
    if ( i == 6 || info.kind != NICK_ETHERNET || info.mtu != 1500 ) {
        fail();
        return FALSE;
    }
    ok();
    return TRUE;
}

static void bad_lengths( void )
{
    unsigned int e1, e2;

    check( "NIC_SEND: a frame too short and one too long, refused" );
    e1 = nic_send( tx, NIC_MINFRAME - 1 );
    e2 = nic_send( tx, NIC_MAXFRAME + 1 );
    out_hex( e1, 4 );
    out_str( " and " );
    out_hex( e2, 4 );
    if ( e1 == ERR_BADLEN && e2 == ERR_BADLEN ) ok(); else fail();
}

/* what was waiting before anything was asked for is nobody's */
static void drain( void )
{
    while ( nic_recv( rx ) ) ;
}

static int dhcp( void )
{
    unsigned int n;
    int tries, got = FALSE;

    check( "DHCP: a DISCOVER to everybody, an OFFER back" );
    for ( tries = 0; tries < TRIES_DHCP && !got; tries++ ) {
        n = build_dhcp( tx, DHCP_DISCOVER );
        if ( nic_send( tx, n ) ) break;
        got = wait_for( WANT_DHCP, DHCP_OFFER, WAIT_DHCP );
    }
    if ( !got ) {
        out_str( "no offer" );
        fail();
        return FALSE;
    }
    out_ip( offered );
    out_str( " from " );
    out_ip( server_ip );
    ok();

    check( "DHCP: a REQUEST for it, the ACK that binds it" );
    got = FALSE;
    for ( tries = 0; tries < TRIES_DHCP && !got; tries++ ) {
        n = build_dhcp( tx, DHCP_REQUEST );
        if ( nic_send( tx, n ) ) break;
        got = wait_for( WANT_DHCP, DHCP_ACK, WAIT_DHCP );
    }
    if ( !got ) {
        out_str( "no ack" );
        fail();
        return FALSE;
    }
    mem_copy( my_ip, offered, 4 );
    have_ip = TRUE;
    out_ip( my_ip );
    out_str( " mask " );
    out_ip( mask_ip );
    out_str( ", gateway " );
    out_ip( gw_ip );
    out_str( ", DNS " );
    out_ip( dns_ip );
    out_str( ", lease " );
    out_dec( lease );
    out_str( "s" );
    if ( mem_same( gw_ip, zero_ip, 4 ) || mem_same( mask_ip, zero_ip, 4 ) ) {
        fail();
        return FALSE;
    }
    ok();
    return TRUE;
}

static int arp( void )
{
    unsigned int n;
    int tries, got = FALSE;

    check( "ARP: who has the gateway" );
    for ( tries = 0; tries < TRIES_ARP && !got; tries++ ) {
        n = build_arp( tx, FALSE, NULL, gw_ip );
        if ( nic_send( tx, n ) ) break;
        got = wait_for( WANT_ARP, 0, WAIT_ARP );
    }
    if ( !got ) {
        out_str( "no reply" );
        fail();
        return FALSE;
    }
    out_ip( gw_ip );
    out_str( " is at " );
    out_mac( gw_mac );
    ok();
    return TRUE;
}

static void ping( void )
{
    unsigned int n, len, start;
    uint32_t seq;
    unsigned int replies = 0;
    int seen[PINGS + 1];
    int i;

    for ( i = 0; i <= PINGS; i++ ) seen[i] = FALSE;
    check( "ping the gateway, 32 bytes, three times" );
    /* send all three, then collect the replies: on a slow card a reply
       can land just outside a strict per-request wait, but it is still
       a reply */
    for ( i = 1; i <= PINGS; i++ ) {
        n = build_ping( tx, i );
        nic_send( tx, n );
    }
    start = now_cs();
    while ( replies < PINGS && since( start ) < WAIT_PING ) {
        len = nic_recv( rx );
        if ( len == 0 ) continue;
        answer_arp( rx, len );
        seq = take_ping_any( rx, len );
        if ( seq && !seen[seq] ) { seen[seq] = TRUE; replies++; }
    }
    out_dec( replies );
    out_str( " replies, the text intact, ttl " );
    out_dec( ping_ttl );
    if ( replies == PINGS ) ok(); else fail();
}

static void stats( void )
{
    NIC_STATBLOCK st;
    uint32_t answer;
    unsigned int err;

    /* FRAMES DROPPED FOR A FULL QUEUE ARE COUNTED, NOT FAILED.  This
       test reads only the frames it waits for, and a real network sends
       broadcasts and multicasts all the while; on the 5x86's 3C515,
       which takes every multicast or none, 103 were dropped during a
       run that passed everything else (2026-11-01).  The emulators'
       networks are silent, which is why nothing was ever dropped there.
       Errors and the card's own buffer running over still fail. */
    check( "NIC_STATS: no errors, no overflow" );
    err = nd_call( NIC_STATS, &st, sizeof( st ), &answer );
    if ( err || answer != NICS_LEN ) {
        out_str( "error " );
        out_hex( err, 4 );
        fail();
        return;
    }
    out_dec( st.rxerrors + st.txerrors );
    out_str( ", " );
    out_dec( st.overflows );
    if ( st.rxerrors == 0 && st.txerrors == 0 && st.overflows == 0 ) ok(); else fail();
    out_str( "  frames out " );
    out_dec( st.txframes );
    out_str( ", in " );
    out_dec( st.rxframes );
    out_str( ", dropped for a full queue " );
    out_dec( st.rxdropped );
    out_str( ", interrupts " );
    out_dec( st.irqs );
    out_nl();
}

void __cdecl dos_entry( void )
{
    out_str( "NICTEST - a network card driver, from the wire: DHCP, ARP and a ping" );
    out_nl();
    rx = mem_alloc( FRAME_BUF );
    tx = mem_alloc( FRAME_BUF );
    if ( rx == NULL || tx == NULL ) {
        out_str( "out of memory" );
        out_nl();
        test_exit( 1 );
    }
    if ( !find_driver() ) {
        out_str( "NICTEST done" );
        out_nl();
        test_exit( 0 );
    }
    if ( ask_info() ) {
        bad_lengths();
        drain();
        if ( dhcp() && arp() ) ping();
        stats();
    }
    out_str( "  checks failed here: " );
    out_dec( failures );
    out_nl();
    out_str( failures ? "SOME CHECKS FAILED." : "all checks passed." );
    out_nl();
    out_str( "NICTEST done" );
    out_nl();
    test_exit( failures ? 1 : 0 );
}

/* ---------------------------------------------------------------- */
/* what a library would have supplied                                */
/* ---------------------------------------------------------------- */

static void mem_copy( void *to, const void *from, unsigned int len )
{
    uint8_t *t = (uint8_t *)to;
    const uint8_t *f = (const uint8_t *)from;

    while ( len-- ) *t++ = *f++;
}

static void mem_zero( void *to, unsigned int len )
{
    uint8_t *t = (uint8_t *)to;

    while ( len-- ) *t++ = 0;
}

static int mem_same( const void *a, const void *b, unsigned int len )
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;

    while ( len-- ) {
        if ( *p++ != *q++ ) return FALSE;
    }
    return TRUE;
}
