/*
 * TLSTEST.C - TLSTEST.EXE, the kernel-side TLS socket proved end to end
 *
 * Where SOCKTEST proves the plain TCP stack, this proves the TLS socket
 * the stack grew in DRV\TCPIP: SOCK_OPEN with SOCK_TLS, SOCK_TLSHOST to
 * name the server, SOCK_CONNECT to its 443, then - once SOCK_STATE says
 * SS_ESTABLISHED, which for a TLS socket means the handshake is done -
 * an ordinary SOCK_SEND of a plaintext GET and SOCK_RECV of the
 * plaintext reply.  All the TLS handshake and record crypto runs in
 * the stack; the program sends and reads cleartext and never sees a
 * record.  A healthy run shows the reply beginning "HTTP/1".
 *
 * It fetches https://example.com/ over the emulator's SLiRP NAT (the
 * reliable outbound path WGET and SOCKTEST use), or https://host/ for
 * TLSTEST host, on another port for TLSTEST host:port.  A handshake that
 * fails says why, as SOCK_STATE gives it (err and why): a certificate
 * refused, and which check refused it, is
 * told apart from a server that never answered.  Built as SOCKTEST is:
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
#define WAIT_CONN  4500        /* a TLS handshake is several round trips, and
                                  TCP takes about 30 s to give up on a server */
#define WAIT_DATA  1000

static uint8_t  out_buf[OUT_BUF];
static unsigned int out_len;
static unsigned int failures;

static uint8_t  io[2048];      /* a SOCK_IO header and its data */
static char     host[128] = "example.com";  /* or the first word of the tail */
static uint16_t port = 443;                 /* or the :port after the host */
static unsigned int fail_err, fail_why;     /* SOCK_STATE's, when it was SS_ERROR */

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

/* print up to 'max' bytes as text, control codes shown as dots */
static void out_text( const uint8_t *buf, unsigned int len, unsigned int max )
{
    unsigned int i;
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
    if ( now < start ) now += 24UL * 60 * 60 * 100;
    return now - start;
}

/* ---------------------------------------------------------------- */
/* the socket call (21h/F1h AL=26h)                                  */
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

/* a socket that will not be used: closed, so the stack lets it go - the
   stack does not know when a program ends */
static int give_up( uint32_t handle, unsigned int *why, unsigned int code )
{
    SOCK_HANDLE hreq;
    int err;

    hreq.handle = handle;
    sk( SOCK_CLOSE, &hreq, sizeof( hreq ), &err );
    *why = code;
    return 0;
}

/* open a TLS socket to host/ip:port, naming the server for SNI and the
   certificate, and wait for the handshake to finish */
static int tls_connect( uint32_t ip, uint16_t port, const char *host, unsigned int *why )
{
    SOCK_OPENREQ oreq;
    SOCK_TLSHOSTREQ hreq;
    SOCK_CONNREQ creq;
    SOCK_STAT st;
    unsigned int start, state;
    uint32_t handle;
    int err;

    *why = 0;
    oreq.type = SOCK_STREAM | SOCK_TLS;
    handle = sk( SOCK_OPEN, &oreq, sizeof( oreq ), &err );
    if ( err || handle == 0 ) { *why = 1; return 0; }

    mem_zero( &hreq, sizeof( hreq ) );
    hreq.handle = handle;
    str_set( hreq.name, host, sizeof( hreq.name ) );
    sk( SOCK_TLSHOST, &hreq, SOCKTLSHOST_LEN, &err );
    if ( err ) return give_up( handle, why, 2 );

    mem_zero( &creq, sizeof( creq ) );
    creq.handle = handle;
    creq.ip = ip;
    creq.port = port;
    sk( SOCK_CONNECT, &creq, sizeof( creq ), &err );
    if ( err ) return give_up( handle, why, 3 );

    start = now_cs();
    for ( ;; ) {
        mem_zero( &st, sizeof( st ) );
        st.handle = handle;
        state = sk( SOCK_STATE, &st, SOCKSTAT_LEN2, &err );
        if ( err ) return give_up( handle, why, 4 );
        if ( state == SS_ESTABLISHED ) return (int)handle;
        if ( state == SS_ERROR ) {
            fail_err = st.err;
            fail_why = st.why;
            return give_up( handle, why, 5 );
        }
        if ( since( start ) > WAIT_CONN ) return give_up( handle, why, 6 );
    }
}

/* SOCK_STATE's err and why, in words */
static void out_failure( void )
{
    static const char *const errs[] = {
        "", "", "failed", "refused by the server", "the server did not answer",
        "nothing at that address (no ARP answer)", "reset by the server",
        "certificate refused", "TLS handshake failed"
    };
    static const char *const whys[] = {
        "", "not trusted (no path to a root in the bundle)", "made out to another name",
        "expired or not yet valid", "could not be read"
    };

    out_str( fail_err < 9 ? errs[fail_err] : "failed" );
    out_str( " (err=" ); out_dec( fail_err );
    if ( fail_why ) {
        out_str( " why=" ); out_dec( fail_why ); out_str( ": " );
        out_str( fail_why < 5 ? whys[fail_why] : "?" );
    }
    out_str( ")" );
}

/* an address written a.b.c.d, so a failure can be had without DNS; 0 if
   the host is a name */
static uint32_t dotted( const char *text )
{
    uint32_t ip = 0;
    unsigned int part, i;

    for ( i = 0; i < 4; i++ ) {
        if ( *text < '0' || *text > '9' ) return 0;
        for ( part = 0; *text >= '0' && *text <= '9'; text++ ) part = part * 10 + (unsigned int)( *text - '0' );
        if ( part > 255 ) return 0;
        ip = ( ip << 8 ) | part;
        if ( i < 3 && *text++ != '.' ) return 0;
    }
    return *text ? 0 : ip;
}

/* the first word of the command tail, if there is one (21h/62h gives the
   PSP's address), as host or host:port */
static void take_host( void )
{
    REGS regs;
    const uint8_t *psp;
    unsigned int len, i = 0, n = 0, num = 0;

    clear( &regs );
    regs.eax = 0x6200;
    int21( &regs );
    psp = (const uint8_t *)regs.ebx;
    len = psp[0x80];
    if ( len > 127 ) len = 127;
    while ( i < len && psp[0x81 + i] == ' ' ) i++;
    while ( i < len && psp[0x81 + i] != ' ' && psp[0x81 + i] != '\r' &&
            psp[0x81 + i] != ':' && n + 1 < sizeof( host ) ) {
        host[n++] = (char)psp[0x81 + i++];
    }
    if ( n ) host[n] = 0;
    if ( i < len && psp[0x81 + i] == ':' ) {
        i++;
        while ( i < len && psp[0x81 + i] >= '0' && psp[0x81 + i] <= '9' && num <= 65535 ) {
            num = num * 10 + (unsigned int)( psp[0x81 + i++] - '0' );
        }
        if ( num > 0 && num <= 65535 ) port = (uint16_t)num;
    }
}

static void test_tls( void )
{
    static char request[256];
    SOCK_IO *msg = (SOCK_IO *)io;
    SOCK_HANDLE hreq;
    SOCK_STAT st;
    unsigned int start, got, total = 0, state;
    uint32_t ip;
    unsigned int why, reqlen;
    int handle, err, i;
    static uint8_t reply[4096];

    str_set( request, "GET / HTTP/1.0\r\nHost: ", sizeof( request ) );
    for ( reqlen = 0; request[reqlen]; reqlen++ ) ;
    str_set( request + reqlen, host, sizeof( request ) - reqlen );
    for ( ; request[reqlen]; reqlen++ ) ;
    str_set( request + reqlen, "\r\nConnection: close\r\n\r\n", sizeof( request ) - reqlen );
    for ( ; request[reqlen]; reqlen++ ) ;

    out_str( "  resolve " ); out_str( host ); out_str( " ... " );
    ip = dotted( host );
    if ( ip == 0 ) ip = resolve( host );
    if ( ip == 0 ) { out_str( "could not resolve" ); out_nl(); failures++; return; }
    out_ip( ip ); out_nl();

    out_str( "  TLS handshake to :" ); out_dec( port ); out_str( " ... " );
    handle = tls_connect( ip, port, host, &why );
    if ( handle == 0 ) {
        out_str( "FAILED (why=" ); out_dec( why ); out_str( ")" );
        if ( why == 5 ) { out_str( ": " ); out_failure(); }
        out_nl();
        failures++;
        return;
    }
    out_str( "up" ); out_nl();

    mem_zero( msg, SOCK_IO_HDR );
    msg->handle = handle;
    msg->len = reqlen;
    for ( i = 0; i < (int)msg->len; i++ ) io[SOCK_IO_HDR + i] = (uint8_t)request[i];
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
    take_host();
    out_str( "TLSTEST - the kernel-side TLS socket: https://" );
    out_str( host );
    out_str( "/ over the bridge-less stack" );
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
