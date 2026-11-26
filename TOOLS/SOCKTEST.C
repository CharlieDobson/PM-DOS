/*
 * SOCKTEST.C - SOCKTEST.EXE, the TCP/IP stack proved through its sockets
 *
 * Where NICTEST built frames by hand to prove a card, this proves the
 * stack above the card (DRV\TCPIP) through the socket API on 21h/F1h
 * AL=26h (INC\SOCKET.H): the interface DHCP brought up, a ping to the
 * gateway, a name looked up, and a whole TCP exchange - connect, a
 * request sent, the reply read, the socket closed.
 *
 * The address DHCP hands out on every emulator's user-mode network is
 * 10.0.2.15, the gateway 10.0.2.2, the name server 10.0.2.3, so those
 * lines are the same on QEMU and 86Box.  The TCP line needs a server
 * to connect to: the rig (rig\netrun.ps1 -TcpProbe) forwards a guest
 * connection to 10.0.9.9:80 to a listener on the host that answers
 * with the body PMDOS-OK, which is what this looks for.  Without it the
 * TCP line cannot pass; the DNS line needs a resolver that answers and
 * so is reported, not counted.
 *
 * Built as NICTEST is: CSTART.ASM is the startup and the INT 21h thunk.
 */
#include "ndrv.h"
#include "socket.h"

void __cdecl int21( REGS *regs );

#define TRUE   1
#define FALSE  0
#define NULL   ((void *)0)

#define OUT_BUF    4096
#define DOTS_COL   58

/* how long to wait, in hundredths of a second */
#define WAIT_UP    600
#define WAIT_PING  300
#define WAIT_DNS   400
#define WAIT_CONN  400
#define WAIT_DATA  600

static uint8_t  out_buf[OUT_BUF];
static unsigned int out_len;
static unsigned int failures;
static int      column;

static uint8_t io[2048];        /* a SOCK_IO header and its data */

static void mem_zero( void *to, unsigned int len )
{
    uint8_t *dst = (uint8_t *)to;
    while ( len-- ) *dst++ = 0;
}

static void str_set( char *to, const char *from, unsigned int max )
{
    unsigned int i;
    for ( i = 0; i + 1 < max && from[i]; i++ ) to[i] = from[i];
    to[i] = 0;
}

static int has_text( const uint8_t *buf, unsigned int len, const char *want )
{
    unsigned int i, j;
    for ( i = 0; i < len; i++ ) {
        for ( j = 0; want[j] && i + j < len && buf[i + j] == (uint8_t)want[j]; j++ ) ;
        if ( !want[j] ) return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- */
/* output - as NICTEST does it                                       */
/* ---------------------------------------------------------------- */

static void clear( REGS *regs ) { mem_zero( regs, sizeof( *regs ) ); }

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
    regs.eax = 0x4C00 | ( code & 0xFF );
    int21( &regs );
}

static void out_ch( char ch )
{
    if ( out_len == OUT_BUF ) out_flush();
    out_buf[out_len++] = (uint8_t)ch;
    column = ( ch == '\n' ) ? 0 : column + 1;
}

static void out_str( const char *str ) { while ( *str ) out_ch( *str++ ); }
static void out_nl( void ) { out_str( "\r\n" ); }

static void out_dec( unsigned int value )
{
    char digits[12];
    int  n = 0;
    do { digits[n++] = (char)( '0' + value % 10 ); value /= 10; } while ( value );
    while ( n ) out_ch( digits[--n] );
}

static void out_ip( uint32_t ip )
{
    out_dec( IP_A( ip ) ); out_ch( '.' );
    out_dec( IP_B( ip ) ); out_ch( '.' );
    out_dec( IP_C( ip ) ); out_ch( '.' );
    out_dec( IP_D( ip ) );
}

static void check( const char *text )
{
    out_str( "  " );
    out_str( text );
    out_ch( ' ' );
    while ( column < DOTS_COL ) out_ch( '.' );
    out_ch( ' ' );
}

static void ok( void ) { out_str( " ok" ); out_nl(); }
static void fail( void ) { failures++; out_str( " FAIL" ); out_nl(); }
static void note( void ) { out_nl(); }       /* reported, not counted */

/* ---------------------------------------------------------------- */
/* the clock, in hundredths (21h/2Ch counts ticks on PM-DOS)         */
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
    if ( now < start ) now += 24UL * 60 * 60 * 100;     /* past midnight */
    return now - start;
}

/* ---------------------------------------------------------------- */
/* the socket call                                                   */
/* ---------------------------------------------------------------- */

static uint32_t sk( unsigned int fn, void *buf, unsigned int len, int *err )
{
    REGS regs;
    clear( &regs );
    regs.eax = ( fn << 16 ) | 0xF126;
    regs.ecx = len;
    regs.edi = (uint32_t)buf;
    int21( &regs );
    if ( CF( regs ) ) { if ( err ) *err = (int)( regs.eax & 0xFFFF ); return 0; }
    if ( err ) *err = 0;
    return regs.eax;
}

/* ---------------------------------------------------------------- */
/* the checks                                                        */
/* ---------------------------------------------------------------- */

static SOCK_IF ifs;

static int stack_here( void )
{
    int err;
    sk( SOCK_IFSTAT, &ifs, SOCKIF_LEN, &err );
    return err == 0;
}

static int wait_up( void )
{
    unsigned int start = now_cs();
    int err;
    for ( ;; ) {
        sk( SOCK_IFSTAT, &ifs, SOCKIF_LEN, &err );
        if ( err == 0 && ifs.up ) return TRUE;
        if ( since( start ) > WAIT_UP ) return FALSE;
    }
}

static void out_mac( const uint8_t *mac )
{
    int i;
    for ( i = 0; i < 6; i++ ) {
        if ( i ) out_ch( '-' );
        out_ch( "0123456789ABCDEF"[( mac[i] >> 4 ) & 15] );
        out_ch( "0123456789ABCDEF"[mac[i] & 15] );
    }
}

/* the card's own frame and error counts, so we can see where a dead DHCP
   stands: tx counts up if the DISCOVER is leaving, rx if anything arrives */
static void show_nicstats( void )
{
    uint32_t st[16];
    int err;
    mem_zero( st, sizeof( st ) );
    sk( SOCK_NICSTATS, st, 40, &err );          /* NICS_LEN = 40 */
    if ( err != 0 ) { out_str( "  (NIC stats unavailable)" ); out_nl(); return; }
    out_str( "  card counts: tx=" ); out_dec( st[2] );     /* txframes */
    out_str( " txerr=" ); out_dec( st[6] );                /* txerrors */
    out_str( " rx=" ); out_dec( st[0] );                   /* rxframes */
    out_str( " rxerr=" ); out_dec( st[5] );                /* rxerrors */
    out_str( " rxdrop=" ); out_dec( st[4] );               /* rxdropped */
    out_str( " irq=" ); out_dec( st[8] );                  /* irqs */
    out_nl();
}

static void test_dhcp( void )
{
    int err;
    sk( SOCK_IFSTAT, &ifs, SOCKIF_LEN, &err );
    out_str( "  card seen: '" );
    out_str( ifs.nic );
    out_str( "' mac " );
    out_mac( ifs.mac );
    out_nl();

    check( "the interface has an address (DHCP)" );
    if ( !wait_up() ) { out_str( "no address" ); fail(); show_nicstats(); return; }
    out_ip( ifs.ip );
    out_str( " mask " );
    out_ip( ifs.mask );
    out_str( " gw " );
    out_ip( ifs.gw );
    if ( ifs.ip == MAKE_IP( 10, 0, 2, 15 ) &&
         ifs.mask == MAKE_IP( 255, 255, 255, 0 ) &&
         ifs.gw == MAKE_IP( 10, 0, 2, 2 ) ) ok();
    else fail();

    check( "the name server" );
    out_ip( ifs.dns );
    if ( ifs.dns == MAKE_IP( 10, 0, 2, 3 ) ) ok(); else fail();
}

static void test_ping( void )
{
    SOCK_PINGREQ req;
    unsigned int start, state;
    int err;

    check( "ping the gateway" );
    mem_zero( &req, sizeof( req ) );
    req.ip = ifs.gw;
    req.seq = 1;
    start = now_cs();
    for ( ;; ) {
        state = sk( SOCK_PING, &req, SOCKPING_LEN, &err );
        if ( err ) { out_str( "error" ); fail(); return; }
        if ( state != NET_PENDING ) break;
        if ( since( start ) > WAIT_PING ) { out_str( "no reply" ); fail(); return; }
    }
    if ( req.state == NET_DONE ) {
        out_str( "reply in " );
        out_dec( req.rttms );
        out_str( " ms" );
        ok();
    } else { out_str( "no reply" ); fail(); }
}

static void test_dns( void )
{
    SOCK_RES req;
    unsigned int start, state;
    int err;

    check( "a name looked up (DNS)" );
    mem_zero( &req, sizeof( req ) );
    str_set( req.name, "www.example.com", sizeof( req.name ) );
    start = now_cs();
    for ( ;; ) {
        state = sk( SOCK_RESOLVE, &req, SOCKRES_LEN, &err );
        if ( err ) { out_str( "error" ); note(); return; }
        if ( state != NET_PENDING ) break;
        if ( since( start ) > WAIT_DNS ) { out_str( "no answer (no resolver?)" ); note(); return; }
    }
    if ( req.state == NET_DONE ) { out_str( "www.example.com is " ); out_ip( req.ip ); note(); }
    else { out_str( "no answer (no resolver?)" ); note(); }
}

static int open_connect( int type, uint32_t ip, uint16_t port )
{
    SOCK_OPENREQ oreq;
    SOCK_CONNREQ creq;
    SOCK_STAT st;
    unsigned int start, state;
    uint32_t handle;
    int err;

    oreq.type = type;
    handle = sk( SOCK_OPEN, &oreq, sizeof( oreq ), &err );
    if ( err || handle == 0 ) return 0;

    mem_zero( &creq, sizeof( creq ) );
    creq.handle = handle;
    creq.ip = ip;
    creq.port = port;
    sk( SOCK_CONNECT, &creq, sizeof( creq ), &err );
    if ( err ) return 0;

    start = now_cs();
    for ( ;; ) {
        mem_zero( &st, sizeof( st ) );
        st.handle = handle;
        state = sk( SOCK_STATE, &st, SOCKSTAT_LEN, &err );
        if ( err ) return 0;
        if ( state == SS_ESTABLISHED ) return (int)handle;
        if ( state == SS_ERROR || state == SS_CLOSED ) return 0;
        if ( since( start ) > WAIT_CONN ) return 0;
    }
}

/* resolve a name to an address, polling until it is known or time is up */
static uint32_t resolve( const char *name )
{
    SOCK_RES req;
    unsigned int start, state;
    int err;

    mem_zero( &req, sizeof( req ) );
    str_set( req.name, name, sizeof( req.name ) );
    start = now_cs();
    for ( ;; ) {
        state = sk( SOCK_RESOLVE, &req, SOCKRES_LEN, &err );
        if ( err ) return 0;
        if ( state == NET_DONE ) return req.ip;
        if ( state == NET_FAIL || since( start ) > WAIT_DNS ) return 0;
    }
}

static void test_tcp( void )
{
    static const char request[] =
        "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    SOCK_IO *msg = (SOCK_IO *)io;
    SOCK_HANDLE hreq;
    SOCK_STAT st;
    unsigned int start, got, total = 0, state;
    uint32_t ip;
    int handle, err, i;
    static uint8_t reply[2048];

    check( "connect to example.com:80 and GET over TCP" );
    ip = resolve( "example.com" );
    if ( ip == 0 ) { out_str( "could not resolve the name" ); note(); return; }

    handle = open_connect( SOCK_STREAM, ip, 80 );
    if ( handle == 0 ) { out_str( "did not connect to " ); out_ip( ip ); fail(); return; }

    mem_zero( msg, SOCK_IO_HDR );
    msg->handle = handle;
    msg->len = sizeof( request ) - 1;
    for ( i = 0; i < (int)msg->len; i++ ) io[SOCK_IO_HDR + i] = (uint8_t)request[i];
    got = sk( SOCK_SEND, io, SOCK_IO_HDR + msg->len, &err );
    if ( err ) { out_str( "send failed" ); fail(); return; }

    start = now_cs();
    for ( ;; ) {
        mem_zero( msg, SOCK_IO_HDR );
        msg->handle = handle;
        msg->len = 1024;
        got = sk( SOCK_RECV, io, SOCK_IO_HDR + 1024, &err );
        if ( err ) break;
        if ( got ) {
            for ( i = 0; i < (int)got && total < sizeof( reply ); i++ ) reply[total++] = io[SOCK_IO_HDR + i];
            start = now_cs();
        } else {
            mem_zero( &st, sizeof( st ) );
            st.handle = handle;
            state = sk( SOCK_STATE, &st, SOCKSTAT_LEN, &err );
            if ( ( state != SS_ESTABLISHED && st.rxready == 0 ) || since( start ) > WAIT_DATA ) break;
        }
    }

    hreq.handle = handle;
    sk( SOCK_CLOSE, &hreq, sizeof( hreq ), &err );

    out_ip( ip );
    out_str( ", " );
    out_dec( total );
    out_str( " bytes: " );
    if ( has_text( reply, total, "HTTP/1" ) ) ok();
    else fail();
}

void __cdecl dos_entry( void )
{
    out_str( "SOCKTEST - the TCP/IP stack through its sockets: DHCP, ping, DNS, TCP" );
    out_nl();

    if ( !stack_here() ) {
        out_str( "  no TCP/IP stack loaded - nothing to test here" );
        out_nl();
        out_str( "SOCKTEST done" );
        out_nl();
        test_exit( 0 );
    }

    test_dhcp();
    if ( ifs.up ) {
        test_ping();
        test_dns();
        test_tcp();
    }

    out_str( "  checks failed here: " );
    out_dec( failures );
    out_nl();
    out_str( failures ? "SOME CHECKS FAILED." : "all checks passed." );
    out_nl();
    out_str( "SOCKTEST done" );
    out_nl();
    test_exit( failures ? 1 : 0 );
}
