/*
 * WGET.C - WGET.EXE, fetch a URL over HTTP or HTTPS
 *
 *   WGET url [/O:file] [/CA:file] [/K] [/V]
 *     url      http://host[:port]/path  or  https://host[:port]/path
 *     /O:file  write the body to a file; without it the body goes to
 *              the screen (so a page can be read or redirected)
 *     /CA:file use this CA bundle instead of CACERT.BND
 *     /K       connect without checking the server's certificate
 *     /V       say whether the certificate was verified
 *
 * HTTP/1.0 with Connection: close, so the body is everything up to the
 * close - no chunked decoding needed.  HTTPS is the same request and
 * reply carried over TLS (CMD\NET\TLS, the NSTLS port); built in only
 * when WITH_TLS is defined and the TLS library is linked.
 *
 * For HTTPS the server's certificate is checked against a trust bundle
 * of root CAs (concatenated DER) - CACERT.BND, looked up with /CA:,
 * then the CACERT environment variable, then the current directory,
 * then each PATH entry.  The chain, the host name and the dates all
 * have to check out or the fetch stops; /K turns the check off, and
 * with no bundle found it connects but warns the certificate is
 * unchecked.
 *
 * Built over CMD\NET\SOCKLIB.C, behind CMD\NET\NETSTART.ASM.
 */
#include "socklib.h"
#ifdef WITH_TLS
#include "tlsglue.h"
#endif

#define RBUF 4096

typedef struct {
    int  handle;                /* the socket */
    int  https;
    void *tls;                  /* the TLS connection, when https */
    u8   buf[RBUF];
    int  pos, len;
    int  eof;
} CONN;

#ifdef WITH_TLS
/* the trust bundle read off the disk, and what tls_open is told to do
   with it - set by dos_entry, read inside conn_open */
static u8                    ca_bundle[40960];
static const unsigned char  *g_ca       = NULL;
static int                    g_calen    = 0;
static int                    g_insecure = 0;
static int                    g_outcome  = TLSV_UNVERIFIED;

/* read a whole file into buf (at most bufmax bytes); 0 if it will not open */
static int read_whole( const char *path, unsigned char *buf, int bufmax )
{
    REGS regs;
    int  handle, total = 0, got;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3D00;                  /* open existing, read only */
    regs.edx = (u32)path;
    int21( &regs );
    if ( CF( regs ) ) return 0;
    handle = (int)( regs.eax & 0xFFFF );

    while ( total < bufmax ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x3F00;
        regs.ebx = (u32)handle;
        regs.ecx = (u32)( bufmax - total );
        regs.edx = (u32)( buf + total );
        int21( &regs );
        if ( CF( regs ) ) { total = 0; break; }
        got = (int)( regs.eax & 0xFFFF );
        if ( got <= 0 ) break;
        total += got;
    }

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = 0x3E00;                  /* close */
    regs.ebx = (u32)handle;
    int21( &regs );
    return total;
}

/* find and load the CA bundle: /CA:file, then CACERT, then the current
   directory, then each PATH entry; the byte count, or 0 if none found */
static int load_ca( const char *cafile, unsigned char *buf, int bufmax )
{
    const char *path;
    char        trial[160];
    int         got;
    u32         seg, end;

    if ( cafile && cafile[0] ) return read_whole( cafile, buf, bufmax );

    path = sl_getenv( "CACERT" );
    if ( path && path[0] ) {
        got = read_whole( path, buf, bufmax );
        if ( got > 0 ) return got;
    }

    got = read_whole( "CACERT.BND", buf, bufmax );
    if ( got > 0 ) return got;

    path = sl_getenv( "PATH" );
    while ( path && *path ) {
        for ( seg = 0; path[seg] && path[seg] != ';'; seg++ ) {
        }
        if ( seg > 0 && seg < sizeof( trial ) - 12 ) {
            sl_memcpy( trial, path, seg );
            end = seg;
            if ( trial[end - 1] != '\\' && trial[end - 1] != ':' ) trial[end++] = '\\';
            sl_strcpy( trial + end, "CACERT.BND", sizeof( trial ) - end );
            got = read_whole( trial, buf, bufmax );
            if ( got > 0 ) return got;
        }
        path += seg;
        if ( *path == ';' ) path++;
    }
    return 0;
}

/* the X509_ERR_* a failed certificate check leaves, in words */
static const char *verr_text( int code )
{
    switch ( code ) {
    case -6: return "the name does not match the certificate";              /* HOST    */
    case -5: return "the certificate is expired or not yet valid";          /* EXPIRED */
    case -4: return "the certificate is not trusted (no path to a root in the bundle)"; /* SIG */
    case -3: return "the certificate is too large";                         /* TOOBIG  */
    case -2: return "the certificate uses an unsupported key or signature"; /* NOTRSA  */
    default: return "the certificate could not be parsed";                  /* PARSE   */
    }
}
#endif /* WITH_TLS */

/* ---- the transport: TCP, or TLS over it ---- */

static int conn_raw_recv( CONN *c, void *buf, u32 max )
{
#ifdef WITH_TLS
    if ( c->https ) return tls_recv( c->tls, buf, max );
#endif
    return sl_recvwait( c->handle, buf, max, 1500 );
}

static int conn_send( CONN *c, const void *data, u32 len )
{
#ifdef WITH_TLS
    if ( c->https ) return tls_send( c->tls, data, len );
#endif
    return sl_send( c->handle, data, len, 1500 );
}

static int conn_open( CONN *c, const char *host, u32 ip, u16 port, int https )
{
    c->pos = c->len = c->eof = 0;
    c->https = https;
    c->tls = NULL;
    c->handle = sl_open( SOCK_STREAM );
    if ( c->handle == 0 ) return 0;
    if ( !sl_connect( c->handle, ip, port, 1500 ) ) return 0;
#ifdef WITH_TLS
    if ( https ) {
        c->tls = tls_open( c->handle, host, g_ca, g_calen, g_insecure, &g_outcome );
        return c->tls != NULL;
    }
#else
    if ( https ) return 0;
#endif
    (void)host;
    return 1;
}

static void conn_close( CONN *c )
{
#ifdef WITH_TLS
    if ( c->tls ) tls_close( c->tls );
#endif
    if ( c->handle ) sl_sclose( c->handle );
}

static int conn_fill( CONN *c )
{
    if ( c->pos < c->len ) return 1;
    if ( c->eof ) return 0;
    c->len = conn_raw_recv( c, c->buf, RBUF );
    c->pos = 0;
    if ( c->len <= 0 ) { c->eof = 1; c->len = 0; return 0; }
    return 1;
}

static int conn_getc( CONN *c )
{
    if ( !conn_fill( c ) ) return -1;
    return c->buf[c->pos++];
}

/* ---- the URL ---- */

static int parse_url( const char *url, int *https, char *host, u32 hostmax, u16 *port, char *path, u32 pathmax )
{
    const char *p = url;
    u32 i;

    *https = 0;
    *port = 80;
    if ( sl_strlen( url ) >= 8 && url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' ) {
        if ( url[4] == 's' && url[5] == ':' ) { *https = 1; *port = 443; p = url + 8; }
        else if ( url[4] == ':' ) { p = url + 7; }
    }
    /* host */
    i = 0;
    while ( *p && *p != ':' && *p != '/' && i + 1 < hostmax ) host[i++] = *p++;
    host[i] = 0;
    if ( i == 0 ) return 0;
    if ( *p == ':' ) { p++; *port = (u16)sl_atou( p ); while ( *p >= '0' && *p <= '9' ) p++; }
    /* path */
    if ( *p != '/' ) { path[0] = '/'; path[1] = 0; }
    else { for ( i = 0; *p && i + 1 < pathmax; i++ ) path[i] = *p++; path[i] = 0; }
    return 1;
}

/* ---- the fetch ---- */

static u8   body[RBUF];
static char line[1024];
static char host[256], path[1024], req[1400];

void __cdecl dos_entry( void )
{
    char **argv;
    int   argc, i, https, status = 0, n;
    const char *url = NULL, *outfile = NULL;
    u16   port;
    u32   ip, total = 0;
    CONN  conn;
    REGS  regs;
    int   outhandle = 1;        /* stdout by default */
#ifdef WITH_TLS
    int   insecure = 0, verbose = 0;
    const char *cafile = NULL;
#endif

    argc = sl_args( &argv );
    for ( i = 0; i < argc; i++ ) {
        char c0 = argv[i][0], c1 = argv[i][1];
        if ( ( c0 == '/' || c0 == '-' ) && ( c1 == 'O' || c1 == 'o' ) && argv[i][2] == ':' ) {
            outfile = argv[i] + 3;
#ifdef WITH_TLS
        } else if ( ( c0 == '/' || c0 == '-' ) && ( c1 == 'C' || c1 == 'c' ) &&
                    ( argv[i][2] == 'A' || argv[i][2] == 'a' ) && argv[i][3] == ':' ) {
            cafile = argv[i] + 4;
        } else if ( ( c0 == '/' || c0 == '-' ) && ( c1 == 'K' || c1 == 'k' ) && argv[i][2] == 0 ) {
            insecure = 1;
        } else if ( ( c0 == '/' || c0 == '-' ) && ( c1 == 'V' || c1 == 'v' ) && argv[i][2] == 0 ) {
            verbose = 1;
#endif
        } else if ( c0 == '/' && c1 == '?' ) {
            sl_puts( "WGET url [/O:file] [/CA:file] [/K] [/V]\r\n" );
            sl_puts( "  Fetch a URL over HTTP or HTTPS.  Without /O the body goes to the screen.\r\n" );
            sl_puts( "  /CA:file  a CA bundle to trust instead of CACERT.BND\r\n" );
            sl_puts( "  /K        do not check the server's certificate\r\n" );
            sl_puts( "  /V        say whether the certificate was verified\r\n" );
            sl_flush();
            return;
        } else if ( url == NULL ) {
            url = argv[i];
        }
    }
    if ( url == NULL ) { sl_puts( "usage: WGET url [/O:file] [/CA:file] [/K] [/V]\r\n" ); sl_flush(); return; }

    if ( !parse_url( url, &https, host, sizeof( host ), &port, path, sizeof( path ) ) ) {
        sl_puts( "WGET: bad URL\r\n" ); sl_flush(); return;
    }
#ifndef WITH_TLS
    if ( https ) { sl_puts( "WGET: this build has no HTTPS (TLS was not linked).\r\n" ); sl_flush(); return; }
#endif

    if ( !sl_up( 800 ) ) { sl_puts( "WGET: the network is not up.\r\n" ); sl_flush(); return; }
    ip = sl_resolve( host, 600 );
    if ( ip == 0 ) { sl_puts( "WGET: cannot find " ); sl_puts( host ); sl_puts( "\r\n" ); sl_flush(); return; }

#ifdef WITH_TLS
    if ( https ) {
        g_insecure = insecure;
        g_ca = NULL;
        g_calen = 0;
        if ( !insecure ) {
            g_calen = load_ca( cafile, ca_bundle, (int)sizeof( ca_bundle ) );
            if ( g_calen > 0 ) g_ca = ca_bundle;
        }
    }
#endif

    if ( !conn_open( &conn, host, ip, port, https ) ) {
#ifdef WITH_TLS
        if ( https && g_outcome == TLSV_BADCERT ) {
            sl_puts( "WGET: refused " ); sl_puts( host );
            sl_puts( " - " ); sl_puts( verr_text( tls_verify_reason() ) );
            sl_puts( "\r\n       use /K to connect without checking the certificate.\r\n" );
        } else
#endif
        { sl_puts( "WGET: could not connect to " ); sl_puts( host ); sl_puts( "\r\n" ); }
        conn_close( &conn ); sl_flush(); return;
    }

#ifdef WITH_TLS
    if ( https ) {
        if ( g_outcome == TLSV_VERIFIED ) {
            if ( verbose ) { sl_puts( "WGET: certificate verified for " ); sl_puts( host ); sl_puts( "\r\n" ); sl_flush(); }
        } else if ( insecure ) {
            if ( verbose ) { sl_puts( "WGET: certificate not checked (/K)\r\n" ); sl_flush(); }
        } else {
            /* no bundle found: warn on handle 2 so a piped body stays clean */
            static const char warn[] =
                "WGET: no CA bundle (CACERT.BND) found - the certificate was NOT checked.\r\n";
            sl_memzero( &regs, sizeof( regs ) );
            regs.eax = 0x4000;
            regs.ebx = 2;
            regs.ecx = (u32)( sizeof( warn ) - 1 );
            regs.edx = (u32)warn;
            int21( &regs );
        }
    }
#endif

    /* the request */
    {
        char *w = req;
        const char *s;
        for ( s = "GET "; *s; ) *w++ = *s++;
        for ( s = path; *s; ) *w++ = *s++;
        for ( s = " HTTP/1.0\r\nHost: "; *s; ) *w++ = *s++;
        for ( s = host; *s; ) *w++ = *s++;
        for ( s = "\r\nUser-Agent: PM-DOS/WGET\r\nConnection: close\r\n\r\n"; *s; ) *w++ = *s++;
        conn_send( &conn, req, (u32)( w - req ) );
    }

    /* the status line */
    n = 0;
    for ( ;; ) {
        int ch = conn_getc( &conn );
        if ( ch < 0 ) break;
        if ( ch == '\n' ) break;
        if ( ch != '\r' && n < (int)sizeof( line ) - 1 ) line[n++] = (char)ch;
    }
    line[n] = 0;
    /* "HTTP/1.x NNN ..." */
    {
        char *p = line;
        while ( *p && *p != ' ' ) p++;
        while ( *p == ' ' ) p++;
        status = (int)sl_atou( p );
    }

    /* the rest of the headers, to the blank line */
    for ( ;; ) {
        n = 0;
        for ( ;; ) {
            int ch = conn_getc( &conn );
            if ( ch < 0 ) break;
            if ( ch == '\n' ) break;
            if ( ch != '\r' && n < (int)sizeof( line ) - 1 ) line[n++] = (char)ch;
        }
        line[n] = 0;
        if ( n == 0 ) break;            /* the blank line: body follows */
        if ( conn.eof && conn.pos >= conn.len ) break;
    }

    /* open the output file if asked */
    if ( outfile ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x3C00;
        regs.ecx = 0;
        regs.edx = (u32)outfile;
        int21( &regs );
        if ( CF( regs ) ) { sl_puts( "WGET: cannot create " ); sl_puts( outfile ); sl_puts( "\r\n" ); conn_close( &conn ); sl_flush(); return; }
        outhandle = (int)regs.eax;
    }

    /* the body, streamed to the output */
    for ( ;; ) {
        int got;
        if ( conn.pos < conn.len ) {
            got = conn.len - conn.pos;
            sl_memcpy( body, conn.buf + conn.pos, (u32)got );
            conn.pos = conn.len;
        } else {
            got = conn_raw_recv( &conn, body, RBUF );
            if ( got <= 0 ) break;
        }
        total += got;
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x4000;
        regs.ebx = outhandle;
        regs.ecx = (u32)got;
        regs.edx = (u32)body;
        int21( &regs );
    }

    conn_close( &conn );

    if ( outhandle != 1 ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = 0x3E00;
        regs.ebx = outhandle;
        int21( &regs );
        sl_puts( "saved " ); sl_putu( total ); sl_puts( " bytes to " ); sl_puts( outfile );
        sl_puts( " (HTTP " ); sl_putu( (u32)status ); sl_puts( ")\r\n" );
    }
    sl_flush();
}
