/*
 * TLSTEST.C - TLSTEST.EXE, the kernel-side TLS socket proved end to end
 *
 * Where SOCKTEST proves the plain TCP stack, this proves the TLS socket
 * the stack grew in DRV\TCPIP: SOCK_OPEN with SOCK_TLS, SOCK_TLSHOST to
 * name the server, SOCK_CONNECT to its 443, then - once SOCK_STATE says
 * SS_ESTABLISHED, which for a TLS socket means the handshake is done -
 * an ordinary SOCK_SEND of a plaintext GET and SOCK_RECV of the
 * plaintext reply.  All the TLS 1.2 handshake and record crypto runs in
 * the stack; the program sends and reads cleartext and never sees a
 * record.  A healthy run shows the reply beginning "HTTP/1".
 *
 * It fetches https://example.com/ over the emulator's SLiRP NAT (the
 * reliable outbound path WGET and SOCKTEST use).  Built as SOCKTEST is:
 * CSTART.ASM is the startup and the INT 21h thunk.
 */
#include "ndrv.h"
#include "socket.h"

void __cdecl int21( REGS *regs );

#define TRUE   1
#define FALSE  0
#define NULL   ((void *)0)

#define OUT_BUF    4096

/* how long to wait, in hundredths of a second */
#define WAIT_UP    600
#define WAIT_DNS   400
#define WAIT_CONN  1800        /* a TLS handshake is several round trips */
#define WAIT_DATA  1000

static u8   out_buf[OUT_BUF];
static u32  out_len;
static u32  failures;

static u8   io[2048];          /* a SOCK_IO header and its data */

static void mem_zero( void *to, u32 len )
{
    u8 *dst = (u8 *)to;
    while ( len-- ) *dst++ = 0;
}

static void str_set( char *to, const char *from, u32 max )
{
    u32 i;
    for ( i = 0; i + 1 < max && from[i]; i++ ) to[i] = from[i];
    to[i] = 0;
}

static int has_text( const u8 *buf, u32 len, const char *want )
{
    u32 i, j;
    for ( i = 0; i < len; i++ ) {
        for ( j = 0; want[j] && i + j < len && buf[i + j] == (u8)want[j]; j++ ) ;
        if ( !want[j] ) return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- */
/* output                                                            */
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
        regs.edx = (u32)out_buf;
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
    out_buf[out_len++] = (u8)ch;
}

static void out_str( const char *str ) { while ( *str ) out_ch( *str++ ); }
static void out_nl( void ) { out_str( "\r\n" ); }

static void out_dec( u32 value )
{
    char digits[12];
    int  n = 0;
    do { digits[n++] = (char)( '0' + value % 10 ); value /= 10; } while ( value );
    while ( n ) out_ch( digits[--n] );
}

static void out_ip( u32 ip )
{
    out_dec( IP_A( ip ) ); out_ch( '.' );
    out_dec( IP_B( ip ) ); out_ch( '.' );
    out_dec( IP_C( ip ) ); out_ch( '.' );
    out_dec( IP_D( ip ) );
}

/* print up to 'max' bytes as text, control codes shown as dots */
static void out_text( const u8 *buf, u32 len, u32 max )
{
    u32 i;
    if ( len > max ) len = max;
    for ( i = 0; i < len; i++ ) {
        char ch = (char)buf[i];
        if ( ch == '\r' || ch == '\n' || ( ch >= ' ' && ch < 127 ) ) out_ch( ch );
        else out_ch( '.' );
    }
}

/* ---------------------------------------------------------------- */
/* the clock, in hundredths (21h/2Ch counts ticks on PM-DOS)         */
/* ---------------------------------------------------------------- */

static u32 now_cs( void )
{
    REGS regs;
    u32  h, m, s, cs;
    clear( &regs );
    regs.eax = 0x2C00;
    int21( &regs );
    h  = ( regs.ecx >> 8 ) & 0xFF;
    m  = regs.ecx & 0xFF;
    s  = ( regs.edx >> 8 ) & 0xFF;
    cs = regs.edx & 0xFF;
    return ( ( h * 60 + m ) * 60 + s ) * 100 + cs;
}

static u32 since( u32 start )
{
    u32 now = now_cs();
    if ( now < start ) now += 24UL * 60 * 60 * 100;
    return now - start;
}

/* ---------------------------------------------------------------- */
/* the socket call (21h/F1h AL=26h)                                  */
/* ---------------------------------------------------------------- */

static u32 sk( u32 fn, void *buf, u32 len, int *err )
{
    REGS regs;
    clear( &regs );
    regs.eax = ( fn << 16 ) | 0xF126;
    regs.ecx = len;
    regs.edi = (u32)buf;
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
    u32 start = now_cs();
    int err;
    for ( ;; ) {
        sk( SOCK_IFSTAT, &ifs, SOCKIF_LEN, &err );
        if ( err == 0 && ifs.up ) return TRUE;
        if ( since( start ) > WAIT_UP ) return FALSE;
    }
}

static u32 resolve( const char *name )
{
    SOCK_RES req;
    u32 start, state;
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

/* open a TLS socket to host/ip:port, naming the server for SNI and the
   certificate, and wait for the handshake to finish */
static int tls_connect( u32 ip, u16 port, const char *host, u32 *why )
{
    SOCK_OPENREQ oreq;
    SOCK_TLSHOSTREQ hreq;
    SOCK_CONNREQ creq;
    SOCK_STAT st;
    u32 start, state, handle;
    int err;

    *why = 0;
    oreq.type = SOCK_STREAM | SOCK_TLS;
    handle = sk( SOCK_OPEN, &oreq, sizeof( oreq ), &err );
    if ( err || handle == 0 ) { *why = 1; return 0; }

    mem_zero( &hreq, sizeof( hreq ) );
    hreq.handle = handle;
    str_set( hreq.name, host, sizeof( hreq.name ) );
    sk( SOCK_TLSHOST, &hreq, SOCKTLSHOST_LEN, &err );
    if ( err ) { *why = 2; return 0; }

    mem_zero( &creq, sizeof( creq ) );
    creq.handle = handle;
    creq.ip = ip;
    creq.port = port;
    sk( SOCK_CONNECT, &creq, sizeof( creq ), &err );
    if ( err ) { *why = 3; return 0; }

    start = now_cs();
    for ( ;; ) {
        mem_zero( &st, sizeof( st ) );
        st.handle = handle;
        state = sk( SOCK_STATE, &st, SOCKSTAT_LEN, &err );
        if ( err ) { *why = 4; return 0; }
        if ( state == SS_ESTABLISHED ) return (int)handle;
        if ( state == SS_ERROR ) { *why = 5; return 0; }
        if ( since( start ) > WAIT_CONN ) { *why = 6; return 0; }
    }
}

static void test_tls( void )
{
    static const char request[] =
        "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    SOCK_IO *msg = (SOCK_IO *)io;
    SOCK_HANDLE hreq;
    SOCK_STAT st;
    u32 start, got, total = 0, state, ip, why;
    int handle, err, i;
    static u8 reply[4096];

    out_str( "  resolve example.com ... " );
    ip = resolve( "example.com" );
    if ( ip == 0 ) { out_str( "could not resolve" ); out_nl(); failures++; return; }
    out_ip( ip ); out_nl();

    out_str( "  TLS handshake to :443 ... " );
    handle = tls_connect( ip, 443, "example.com", &why );
    if ( handle == 0 ) {
        out_str( "FAILED (why=" ); out_dec( why ); out_str( ")" ); out_nl();
        failures++;
        return;
    }
    out_str( "up" ); out_nl();

    mem_zero( msg, SOCK_IO_HDR );
    msg->handle = handle;
    msg->len = sizeof( request ) - 1;
    for ( i = 0; i < (int)msg->len; i++ ) io[SOCK_IO_HDR + i] = (u8)request[i];
    sk( SOCK_SEND, io, SOCK_IO_HDR + msg->len, &err );
    if ( err ) { out_str( "  send failed" ); out_nl(); failures++; }

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

    out_str( "  got " ); out_dec( total ); out_str( " plaintext bytes" ); out_nl();
    out_str( "  --- first bytes ---" ); out_nl();
    out_text( reply, total, 160 );
    out_nl();
    out_str( "  decrypted HTTP reply: " );
    if ( has_text( reply, total, "HTTP/1" ) ) out_str( "OK" );
    else { out_str( "NOT FOUND" ); failures++; }
    out_nl();
}

void __cdecl dos_entry( void )
{
    out_str( "TLSTEST - the kernel-side TLS socket: https://example.com/ over the bridge-less stack" );
    out_nl();

    if ( !stack_here() ) {
        out_str( "  no TCP/IP stack loaded - nothing to test here" );
        out_nl();
        out_str( "TLSTEST done" );
        out_nl();
        test_exit( 0 );
    }

    if ( !wait_up() ) {
        out_str( "  the interface never got an address (DHCP)" );
        out_nl();
        out_str( "TLSTEST done" );
        out_nl();
        test_exit( 1 );
    }
    out_str( "  interface up: " ); out_ip( ifs.ip ); out_nl();

    test_tls();

    out_str( "  checks failed here: " );
    out_dec( failures );
    out_nl();
    out_str( failures ? "SOME CHECKS FAILED." : "all checks passed." );
    out_nl();
    out_str( "TLSTEST done" );
    out_nl();
    test_exit( failures ? 1 : 0 );
}
