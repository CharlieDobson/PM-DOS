/*
 * WSKPROBE.C - WSKPROBE.EXE, a minimal 16-bit Windows program that
 * drives PMDOS.386's PM API (the bridge into the parked PM-DOS kernel's
 * TCP/IP stack) from PROTECTED mode, the way WINSOCK.DLL will.  It opens
 * a TCP socket, connects to the offline HTTP peer, sends a GET and
 * receives the reply - all through the bridge, while Windows runs.
 *
 * This is Stage C's analogue of BRGTEST (which proved the V86 API before
 * the redirector used it): it proves the new PM API path - a protected-
 * mode caller reaching the VxD and getting its conventional-memory
 * request buffer bridged by flat linear address - before the full
 * WINSOCK.DLL is built on it.
 *
 * The proof is on port E9h: the VxD traces each completed socket op, and
 * the receive shows the reply bytes as text ("data=HTTP/1.0 200 OK...").
 * WfW launches this from WIN.INI run= (winbuild -WinRun); it does its
 * work in WinMain and exits.
 *
 * Built: wcl -zq -bt=windows -l=windows -ml wskprobe.c wskglue.obj
 */
#define NOCOMM
#include <windows.h>

/* ---- the bridge glue (WSKGLUE.ASM) ---- */
extern unsigned long wsk_getapi( void );
#pragma aux wsk_getapi "wsk_getapi_" far value [dx ax] \
                       modify [ax bx cx dx si di es];
extern unsigned long wsk_call( unsigned fn, unsigned len,
                               unsigned long buflin,
                               unsigned apiseg, unsigned apioff );
#pragma aux wsk_call "wsk_call_" far parm [bx] [cx] [dx ax] [si] [di] \
                     value [dx ax] modify [ax bx cx dx si di es];
extern void wsk_yield( void );
#pragma aux wsk_yield "wsk_yield_" far modify [ax bx cx dx si di es];

/* a direct byte to port E9h (QEMU debugcon) - a PM app's I/O to a non-
   virtualised port passes through, so the probe can report its own
   progress on the headless channel alongside the VxD's traces */
extern void e9c( int ch );
#pragma aux e9c = "mov dx,0E9h" "out dx,al" parm [ax] modify [dx];
static void e9s( const char *s ) { while ( *s ) e9c( *s++ ); }
static void e9hex( unsigned long v )
{
    int i; char hx[ 9 ];
    for ( i = 7; i >= 0; i-- ) { int d = (int)( v & 0xF ); hx[ i ] = (char)( d < 10 ? '0' + d : 'A' + d - 10 ); v >>= 4; }
    hx[ 8 ] = 0; e9s( hx );
}

/* ---- KERNEL: a conventional-memory block (selector + paragraph) ---- */
extern unsigned long FAR PASCAL GlobalDosAlloc( unsigned long );
extern unsigned FAR PASCAL GlobalDosFree( unsigned );

/* ---- the socket API request blocks (INC\SOCKET.H) ---- */
#define SOCK_OPEN       1
#define SOCK_CLOSE      2
#define SOCK_CONNECT    3
#define SOCK_SEND       6
#define SOCK_RECV       7
#define SOCK_STATE      8
#define SOCK_STREAM     1
#define SS_ESTABLISHED  4
#define SS_ERROR        6

typedef struct { unsigned long type; } S_OPEN;
typedef struct { unsigned long handle, ip; unsigned short port, spare; } S_CONN;
typedef struct { unsigned long handle, ip; unsigned short port, spare;
                 unsigned long len; } S_IO;
#define S_IO_HDR 16
typedef struct { unsigned long handle, state, rxready, txfree, err; } S_STAT;

/* the offline HTTP peer the rig forwards (winrun -HttpPeer): 10.0.2.200:80
   as a u32, most significant octet first */
#define PEER_IP    0x0A0002C8L
#define PEER_PORT  80

static DWORD g_api;
static WORD  g_apiseg, g_apioff;
static char FAR *g_buf;     /* the conventional-memory request block */
static DWORD g_buflin;      /* its flat linear address */

static void pump( void )
{
    MSG msg;
    while ( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) ) {
        TranslateMessage( &msg );
        DispatchMessage( &msg );
    }
    wsk_yield();
}

static void settle( DWORD ms )
{
    DWORD start = GetTickCount();
    while ( GetTickCount() - start < ms ) pump();
}

/* one socket op; returns the answer, sets *cf to the carry */
static WORD call_op( WORD fn, WORD len, WORD *cf )
{
    DWORD r = wsk_call( fn, len, g_buflin, g_apiseg, g_apioff );
    *cf = HIWORD( r );
    return LOWORD( r );
}

int PASCAL WinMain( HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show )
{
    DWORD dosblk;
    WORD  sel, seg, sh, ans, cf;
    S_OPEN FAR *op;
    S_CONN FAR *cn;
    S_IO   FAR *io;
    S_STAT FAR *st;
    DWORD poll;
    static char req[] = "GET / HTTP/1.0\r\nHost: pmdos\r\n\r\n";
    int reqlen = sizeof( req ) - 1;
    int i;

    (void)hInst; (void)hPrev; (void)cmd; (void)show;

    e9s( "\r\nWSKPROBE: start" );
    g_api = wsk_getapi();
    e9s( "\r\nWSKPROBE: api=" ); e9hex( g_api );
    if ( !g_api ) return 0;                 /* no bridge under us */
    g_apiseg = HIWORD( g_api );
    g_apioff = LOWORD( g_api );

    dosblk = GlobalDosAlloc( 1024L );
    e9s( "\r\nWSKPROBE: dosblk=" ); e9hex( dosblk );
    if ( !dosblk ) return 0;
    sel = LOWORD( dosblk );                 /* low word = selector (PM access) */
    seg = HIWORD( dosblk );                 /* high word = segment (real/linear) */
    g_buf = (char FAR *)( (DWORD)sel << 16 );
    g_buflin = (DWORD)seg << 4;
    e9s( "\r\nWSKPROBE: buflin=" ); e9hex( g_buflin );

    settle( 3000 );                         /* let DHCP bind */

    /* open a TCP socket */
    e9s( "\r\nWSKPROBE: open" );
    op = (S_OPEN FAR *)g_buf;
    op->type = SOCK_STREAM;
    sh = call_op( SOCK_OPEN, sizeof( S_OPEN ), &cf );
    e9s( "\r\nWSKPROBE: open done h=" ); e9hex( sh );
    if ( cf || !sh ) goto freeup;

    /* connect to the HTTP peer */
    cn = (S_CONN FAR *)g_buf;
    cn->handle = sh;
    cn->ip = PEER_IP;
    cn->port = PEER_PORT;
    cn->spare = 0;
    call_op( SOCK_CONNECT, sizeof( S_CONN ), &cf );
    if ( cf ) goto closeup;

    /* poll the state until established */
    for ( poll = 0; poll < 40000L; poll++ ) {
        st = (S_STAT FAR *)g_buf;
        st->handle = sh;
        st->state = 0;
        call_op( SOCK_STATE, sizeof( S_STAT ), &cf );
        if ( (WORD)st->state == SS_ESTABLISHED ) break;
        if ( (WORD)st->state == SS_ERROR ) goto closeup;
        pump();
    }
    e9s( "\r\nWSKPROBE: conn st=" ); e9hex( st->state );

    /* send the GET */
    io = (S_IO FAR *)g_buf;
    io->handle = sh;
    io->ip = 0; io->port = 0; io->spare = 0;
    io->len = reqlen;
    for ( i = 0; i < reqlen; i++ ) g_buf[ S_IO_HDR + i ] = req[ i ];
    ans = call_op( SOCK_SEND, (WORD)( S_IO_HDR + reqlen ), &cf );
    e9s( "\r\nWSKPROBE: sent cf=" ); e9hex( cf ); e9s( " n=" ); e9hex( ans );
    if ( cf ) goto closeup;

    /* receive the reply (the VxD traces the bytes as text) */
    for ( poll = 0; poll < 40000L; poll++ ) {
        io = (S_IO FAR *)g_buf;
        io->handle = sh;
        io->ip = 0; io->port = 0; io->spare = 0;
        io->len = 512;
        ans = call_op( SOCK_RECV, S_IO_HDR + 512, &cf );
        if ( !cf && ans ) break;            /* got data */
        st = (S_STAT FAR *)g_buf;
        st->handle = sh;
        st->state = 0;
        st->rxready = 0;
        call_op( SOCK_STATE, sizeof( S_STAT ), &cf );
        if ( ( poll % 4000L ) == 0 ) {
            e9s( "\r\nWSKPROBE: rx st=" ); e9hex( st->state );
            e9s( " rxr=" ); e9hex( st->rxready );
            e9s( " ans=" ); e9hex( ans );
        }
        if ( (WORD)st->state > SS_ESTABLISHED ) {
            e9s( "\r\nWSKPROBE: rx closed st=" ); e9hex( st->state );
            e9s( " poll=" ); e9hex( poll );
            break;   /* closing/error */
        }
        pump();
    }

closeup:
    io = (S_IO FAR *)g_buf;                  /* reuse the block for close */
    ( (S_OPEN FAR *)g_buf )->type = sh;      /* SOCK_HANDLE: just the handle */
    call_op( SOCK_CLOSE, 4, &cf );
freeup:
    GlobalDosFree( sel );
    return 0;
}
