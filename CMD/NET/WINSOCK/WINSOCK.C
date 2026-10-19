/*
 * WINSOCK.C - WINSOCK.DLL, a 16-bit Windows Sockets 1.1 library for
 * Windows for Workgroups 3.11, backed by the parked PM-DOS kernel's
 * TCP/IP stack through the PMDOS.386 bridge (Stage C).
 *
 * A Win16 program that speaks Windows Sockets loads this DLL and gets
 * real TCP and UDP: each call marshals a request into a conventional-
 * memory buffer and far-calls the VxD's PM API (PDAPI_SOCK), which
 * bridges it into the kernel's socket API (INC\SOCKET.H).  The kernel's
 * stack never blocks - a call does what it can at once and returns - so
 * the WAITING lives here: a blocking call polls and, between polls,
 * runs the blocking hook (which pumps Windows messages and releases the
 * time slice), so Windows stays live and the bridge's async tick
 * advances the parked stack.
 *
 * This first DLL implements the blocking Berkeley core (socket, connect,
 * send, recv, bind, listen, accept, ...), the byte-order and address
 * helpers, select, and gethostbyname.  The message-driven asynchronous
 * calls (WSAAsyncSelect and the WSAAsyncGet* family) are present but
 * minimal - they are Stage C's next increment.
 *
 * Built as WINSOCK.DLL (module WINSOCK) with the standard Winsock 1.1
 * export ordinals; see WSKBUILD.CMD.
 */
#define NOCOMM
#include <windows.h>
#include <winsock.h>

/* Every exported function is defined __loadds: a Win16 DLL is entered
   with DS = the CALLER's data segment, so without loading DS = the DLL's
   DGROUP on entry the DLL's globals would be addressed in the wrong
   segment (a GPF).  The loader fixes up the loadds prolog to the DLL's
   automatic data segment. */

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

extern unsigned long FAR PASCAL GlobalDosAlloc( unsigned long );
extern unsigned FAR PASCAL GlobalDosFree( unsigned );

/* ---- the kernel's socket API (INC\SOCKET.H) functions ---- */
#define K_IFSTAT   0
#define K_OPEN     1
#define K_CLOSE    2
#define K_CONNECT  3
#define K_LISTEN   4
#define K_ACCEPT   5
#define K_SEND     6
#define K_RECV     7
#define K_STATE    8
#define K_RESOLVE  9
#define K_TLSHOST  12         /* name a TLS socket's server (SNI + cert match) */
#define K_SETCA    13         /* hand the stack a CA-bundle chunk */

#define K_STREAM   1
#define K_DGRAM    2

#define KS_CLOSED       1
#define KS_CONNECTING   2
#define KS_LISTEN       3
#define KS_ESTABLISHED  4
#define KS_CLOSING      5
#define KS_ERROR        6

#define K_NET_PENDING  0
#define K_NET_DONE     1
#define K_NET_FAIL     2

#define K_IO_HDR   16
#define KBUF_MAX   512         /* data bytes per bridge send/recv */

/* the kernel request blocks, laid out as in INC\SOCKET.H */
typedef struct { unsigned long type; } KOPEN;
typedef struct { unsigned long handle; } KHANDLE;
typedef struct { unsigned long handle, ip; unsigned short port, spare; } KCONN;
typedef struct { unsigned long handle; unsigned short port, spare; } KLISTEN;
typedef struct { unsigned long handle, newh, ip; unsigned short port, spare; } KACCEPT;
typedef struct { unsigned long handle, ip; unsigned short port, spare;
                 unsigned long len; } KIO;
typedef struct { unsigned long handle, state, rxready, txfree, err; } KSTAT;
typedef struct { unsigned long ip, state; char name[128]; } KRES;
typedef struct { unsigned long handle; char name[128]; } KTLSHOST;
typedef struct { unsigned long offset, total, len; } KCA;   /* then the bytes */

/* ---- per-socket state ---- */
#define MAXSOCK  32
typedef struct {
    int      used;
    int      type;            /* K_STREAM / K_DGRAM */
    int      nonblock;        /* FIONBIO */
    unsigned short lport;      /* a bound local port (host order) */
    HWND     awnd;            /* WSAAsyncSelect window */
    unsigned amsg;            /* WSAAsyncSelect message */
    long     aevent;          /* WSAAsyncSelect event mask (requested) */
    long     afired;          /* events already posted and not re-armed */
} SKENT;

static SKENT g_sk[ MAXSOCK + 1 ];      /* indexed by kernel handle 1..MAXSOCK */

/* ---- DLL-wide state ---- */
static int   g_started = 0;            /* WSAStartup reference count */
static DWORD g_api = 0;                /* the VxD PM API entry (sel:off) */
static WORD  g_apiseg, g_apioff;
static WORD  g_dossel = 0;             /* conventional-memory buffer selector */
static char FAR *g_buf = 0;            /* the request buffer */
static DWORD g_buflin = 0;             /* its flat linear address */
static int   g_lasterr = 0;            /* WSAGetLastError */
static FARPROC g_blkhook = 0;          /* the blocking hook */
static int   g_inblock = 0;            /* a blocking call is running */
static int   g_cancel = 0;             /* WSACancelBlockingCall was called */
static HINSTANCE g_hinst = 0;          /* the DLL's instance (from LibMain) */
static UINT  g_timer = 0;              /* the async-event poll timer, 0 = none */

/* the HTTPS port map: a stream connect() to one of these ports is opened as
   TLS transparently, so an ordinary winsock program speaks HTTPS without
   knowing.  443 is the default; the rest are spare for the future. */
static WORD  g_tlsport[ 8 ] = { 443, 0, 0, 0, 0, 0, 0, 0 };
static int   g_cadone = 0;             /* the trust bundle has been handed down (or tried) */

/* the last name gethostbyname resolved and its address (network order),
   reused as the server name (SNI, certificate) when connect opens TLS */
static unsigned long g_headdr;
static char          g_hename[ 128 ];

static int is_tls_port( WORD port )
{
    int i;
    for ( i = 0; i < 8; i++ ) if ( g_tlsport[ i ] == port ) return 1;
    return 0;
}

/* the async-event poller (WSAAsyncSelect); defined below, exported so
   the loader fixes up its DS-load prolog for the timer callback */
void __loadds FAR PASCAL ws_ontimer( HWND hw, UINT msg, UINT id, DWORD t );

/* ---- tiny helpers (no C runtime) ---- */
static void sk_memcpy( char FAR *to, const char FAR *from, int n )
{
    while ( n-- > 0 ) *to++ = *from++;
}
static int sk_strlen( const char FAR *s )
{
    int n = 0;
    while ( s[ n ] ) n++;
    return n;
}

/* one bridge socket op on g_buf; returns the answer, sets *cf to carry */
static WORD sk_op( WORD fn, WORD len, WORD *cf )
{
    DWORD r = wsk_call( fn, len, g_buflin, g_apiseg, g_apioff );
    *cf = HIWORD( r );
    return LOWORD( r );
}

/* the default blocking hook: pump pending messages, yield.  Returns
   TRUE (there may be more to do), like the real default hook. */
static BOOL FAR PASCAL sk_defhook( void )
{
    MSG msg;
    if ( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) ) {
        TranslateMessage( &msg );
        DispatchMessage( &msg );
    }
    wsk_yield();
    return TRUE;
}

/* run the blocking hook once; returns 0 normally, SOCKET_ERROR if the
   blocking call was cancelled (sets WSAEINTR) */
static int sk_wait( void )
{
    if ( g_blkhook ) {
        #ifdef __WATCOMC__
        ( (BOOL (FAR PASCAL *)( void ))g_blkhook )();
        #endif
    } else {
        sk_defhook();
    }
    if ( g_cancel ) {
        g_lasterr = WSAEINTR;
        return SOCKET_ERROR;
    }
    return 0;
}

static int sk_valid( SOCKET s )
{
    if ( s < 1 || s > MAXSOCK || !g_sk[ s ].used ) {
        g_lasterr = WSAENOTSOCK;
        return 0;
    }
    return 1;
}

/* read a socket's kernel state into *stp; returns 0 ok, -1 bridge error */
static int sk_state( SOCKET s, KSTAT FAR *out )
{
    KSTAT FAR *st = (KSTAT FAR *)g_buf;
    WORD cf;
    st->handle = s;
    st->state = st->rxready = st->txfree = st->err = 0;
    sk_op( K_STATE, sizeof( KSTAT ), &cf );
    if ( cf ) return -1;
    *out = *st;
    return 0;
}

/* =================================================================== */
/*  startup / cleanup                                                  */
/* =================================================================== */
int __loadds PASCAL FAR WSAStartup( WORD wVersionRequired, LPWSADATA lpWSAData )
{
    DWORD blk;
    int i;

    if ( lpWSAData == 0 ) return WSAEFAULT;

    /* fill the description every time (cheap, harmless) */
    lpWSAData->wVersion = 0x0101;
    lpWSAData->wHighVersion = 0x0101;
    {
        static const char desc[] = "PM-DOS Windows Sockets 1.1 (WfW bridge)";
        for ( i = 0; i < WSADESCRIPTION_LEN && desc[ i ]; i++ )
            lpWSAData->szDescription[ i ] = desc[ i ];
        lpWSAData->szDescription[ i ] = 0;
    }
    lpWSAData->szSystemStatus[ 0 ] = 0;
    lpWSAData->iMaxSockets = MAXSOCK;
    lpWSAData->iMaxUdpDg = KBUF_MAX;
    lpWSAData->lpVendorInfo = 0;

    if ( g_started ) { g_started++; return 0; }

    g_api = wsk_getapi();
    if ( !g_api ) return WSASYSNOTREADY;    /* no PMDOS.386 bridge under us */
    g_apiseg = HIWORD( g_api );
    g_apioff = LOWORD( g_api );

    blk = GlobalDosAlloc( 1024L );
    if ( !blk ) return WSASYSNOTREADY;
    g_dossel = LOWORD( blk );               /* low word = selector (PM access) */
    g_buf = (char FAR *)( (DWORD)g_dossel << 16 );
    g_buflin = (DWORD)HIWORD( blk ) << 4;   /* high word = segment; <<4 = linear */

    for ( i = 0; i <= MAXSOCK; i++ ) g_sk[ i ].used = 0;
    g_blkhook = 0;
    g_lasterr = 0;
    g_cadone = 0;               /* push the trust bundle on the first TLS connect */
    g_started = 1;
    return 0;
}

int __loadds PASCAL FAR WSACleanup( void )
{
    if ( !g_started ) { g_lasterr = WSANOTINITIALISED; return SOCKET_ERROR; }
    if ( --g_started > 0 ) return 0;
    if ( g_timer ) { KillTimer( NULL, g_timer ); g_timer = 0; }
    if ( g_dossel ) { GlobalDosFree( g_dossel ); g_dossel = 0; }
    g_buf = 0;
    return 0;
}

/* =================================================================== */
/*  socket lifecycle                                                   */
/* =================================================================== */
SOCKET __loadds PASCAL FAR socket( int af, int type, int protocol )
{
    KOPEN FAR *op = (KOPEN FAR *)g_buf;
    WORD h, cf;
    (void)protocol;

    if ( !g_started ) { g_lasterr = WSANOTINITIALISED; return INVALID_SOCKET; }
    if ( af != AF_INET ) { g_lasterr = WSAEAFNOSUPPORT; return INVALID_SOCKET; }
    if ( type != SOCK_STREAM && type != SOCK_DGRAM ) {
        g_lasterr = WSAESOCKTNOSUPPORT; return INVALID_SOCKET;
    }
    op->type = ( type == SOCK_DGRAM ) ? K_DGRAM : K_STREAM;
    h = sk_op( K_OPEN, sizeof( KOPEN ), &cf );
    if ( cf || h < 1 || h > MAXSOCK ) { g_lasterr = WSAEMFILE; return INVALID_SOCKET; }
    g_sk[ h ].used = 1;
    g_sk[ h ].type = op->type;
    g_sk[ h ].nonblock = 0;
    g_sk[ h ].lport = 0;
    g_sk[ h ].awnd = 0;
    g_sk[ h ].amsg = 0;
    g_sk[ h ].aevent = 0;
    return h;
}

int __loadds PASCAL FAR closesocket( SOCKET s )
{
    KHANDLE FAR *hc = (KHANDLE FAR *)g_buf;
    WORD cf;
    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    hc->handle = s;
    sk_op( K_CLOSE, sizeof( KHANDLE ), &cf );
    g_sk[ s ].used = 0;
    return 0;
}

/* hand the parked stack the trust bundle (CACERT.BND), once, in chunks the
   bridge can carry.  It is looked for beside WINSOCK.DLL, then in C:\.  With
   no bundle a TLS socket still connects, but the certificate is not checked. */
static void push_ca( void )
{
    char path[ 160 ];
    HFILE fh;
    long total, off;
    KCA FAR *ca = (KCA FAR *)g_buf;
    WORD cf;
    int i, n;

    if ( g_cadone ) return;
    g_cadone = 1;

    n = GetModuleFileName( g_hinst, path, sizeof( path ) - 16 );
    while ( n > 0 && path[ n - 1 ] != '\\' && path[ n - 1 ] != '/' && path[ n - 1 ] != ':' ) n--;
    {
        static const char fn[] = "CACERT.BND";
        for ( i = 0; fn[ i ]; i++ ) path[ n + i ] = fn[ i ];
        path[ n + i ] = 0;
    }
    fh = _lopen( path, OF_READ );
    if ( fh == HFILE_ERROR ) fh = _lopen( "C:\\CACERT.BND", OF_READ );
    if ( fh == HFILE_ERROR ) return;

    total = _llseek( fh, 0L, 2 );       /* to the end: the size */
    _llseek( fh, 0L, 0 );
    if ( total <= 0 || total > 65000L ) { _lclose( fh ); return; }

    for ( off = 0; off < total; ) {
        int want = (int)( total - off );
        if ( want > 500 ) want = 500;
        n = _lread( fh, g_buf + sizeof( KCA ), (WORD)want );
        if ( n <= 0 ) break;
        ca->offset = (unsigned long)off;
        ca->total = (unsigned long)total;
        ca->len = (unsigned long)n;
        sk_op( K_SETCA, (WORD)( sizeof( KCA ) + n ), &cf );
        if ( cf ) break;
        off += n;
    }
    _lclose( fh );
}

int __loadds PASCAL FAR connect( SOCKET s, const struct sockaddr FAR *name, int namelen )
{
    const struct sockaddr_in FAR *sin = (const struct sockaddr_in FAR *)name;
    KCONN FAR *cn = (KCONN FAR *)g_buf;
    KSTAT kst;
    WORD cf;
    (void)namelen;

    if ( !sk_valid( s ) ) return SOCKET_ERROR;

    /* the port map: a stream connection to a TLS port (443) becomes a TLS
       connection transparently.  Naming the server (SNI and the certificate
       to match) upgrades the socket to TLS; it must be done before connect,
       with the trust bundle already in the stack. */
    if ( g_sk[ s ].type == K_STREAM && is_tls_port( ntohs( sin->sin_port ) ) ) {
        KTLSHOST FAR *th = (KTLSHOST FAR *)g_buf;
        WORD tcf;
        int i;
        push_ca();
        th->handle = s;
        if ( g_headdr == sin->sin_addr.s_addr && g_hename[ 0 ] ) {
            for ( i = 0; i < 127 && g_hename[ i ]; i++ ) th->name[ i ] = g_hename[ i ];
            th->name[ i ] = 0;
        } else {
            th->name[ 0 ] = 0;
        }
        sk_op( K_TLSHOST, (WORD)sizeof( KTLSHOST ), &tcf );
    }

    cn = (KCONN FAR *)g_buf;                     /* g_buf may have held K_TLSHOST */
    cn->handle = s;
    cn->ip = ntohl( sin->sin_addr.s_addr );     /* host order for the kernel */
    cn->port = ntohs( sin->sin_port );
    cn->spare = 0;
    sk_op( K_CONNECT, sizeof( KCONN ), &cf );
    if ( cf ) { g_lasterr = WSAECONNREFUSED; return SOCKET_ERROR; }

    if ( g_sk[ s ].type == K_DGRAM ) return 0;  /* UDP: just set the peer */
    if ( g_sk[ s ].nonblock ) { g_lasterr = WSAEWOULDBLOCK; return SOCKET_ERROR; }

    g_inblock = 1; g_cancel = 0;
    for ( ;; ) {
        if ( sk_state( s, &kst ) == 0 ) {
            if ( (WORD)kst.state == KS_ESTABLISHED ) { g_inblock = 0; return 0; }
            if ( (WORD)kst.state == KS_ERROR ) {
                g_inblock = 0; g_lasterr = WSAECONNREFUSED; return SOCKET_ERROR;
            }
        }
        if ( sk_wait() ) { g_inblock = 0; return SOCKET_ERROR; }
    }
}

/* =================================================================== */
/*  send / recv                                                        */
/* =================================================================== */
int __loadds PASCAL FAR send( SOCKET s, const char FAR *buf, int len, int flags )
{
    KIO FAR *io = (KIO FAR *)g_buf;
    int done = 0, chunk, took;
    WORD cf;
    (void)flags;

    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    if ( len == 0 ) return 0;
    g_inblock = 1; g_cancel = 0;
    while ( done < len ) {
        chunk = len - done;
        if ( chunk > KBUF_MAX ) chunk = KBUF_MAX;
        io->handle = s; io->ip = 0; io->port = 0; io->spare = 0;
        io->len = chunk;
        sk_memcpy( g_buf + K_IO_HDR, buf + done, chunk );
        took = (int)sk_op( K_SEND, (WORD)( K_IO_HDR + chunk ), &cf );
        if ( cf ) { g_inblock = 0; g_lasterr = WSAECONNRESET; return SOCKET_ERROR; }
        if ( took > 0 ) { done += took; continue; }
        /* no room right now */
        if ( g_sk[ s ].nonblock ) {
            g_sk[ s ].afired &= ~FD_WRITE;  /* re-arm FD_WRITE for the poller */
            g_inblock = 0;
            if ( done > 0 ) return done;
            g_lasterr = WSAEWOULDBLOCK; return SOCKET_ERROR;
        }
        if ( sk_wait() ) { g_inblock = 0; return SOCKET_ERROR; }
    }
    g_inblock = 0;
    return done;
}

int __loadds PASCAL FAR recv( SOCKET s, char FAR *buf, int len, int flags )
{
    KIO FAR *io = (KIO FAR *)g_buf;
    KSTAT kst;
    int want, got;
    WORD cf;
    (void)flags;

    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    if ( len == 0 ) return 0;
    want = ( len > KBUF_MAX ) ? KBUF_MAX : len;
    g_inblock = 1; g_cancel = 0;
    for ( ;; ) {
        io->handle = s; io->ip = 0; io->port = 0; io->spare = 0;
        io->len = want;
        got = (int)sk_op( K_RECV, (WORD)( K_IO_HDR + want ), &cf );
        if ( cf ) { g_inblock = 0; g_lasterr = WSAECONNRESET; return SOCKET_ERROR; }
        if ( got > 0 ) {
            sk_memcpy( buf, g_buf + K_IO_HDR, got );
            g_sk[ s ].afired &= ~FD_READ;   /* re-arm FD_READ for the poller */
            g_inblock = 0;
            return got;
        }
        /* nothing yet: closed (orderly) gives 0, else keep waiting */
        if ( sk_state( s, &kst ) == 0 && (WORD)kst.state > KS_ESTABLISHED
             && kst.rxready == 0 ) {
            g_inblock = 0;
            return 0;                   /* connection closed, no data left */
        }
        if ( g_sk[ s ].nonblock ) { g_inblock = 0; g_lasterr = WSAEWOULDBLOCK; return SOCKET_ERROR; }
        if ( sk_wait() ) { g_inblock = 0; return SOCKET_ERROR; }
    }
}

int __loadds PASCAL FAR shutdown( SOCKET s, int how )
{
    (void)how;
    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    return 0;                           /* the kernel closes on closesocket */
}

/* =================================================================== */
/*  server side                                                        */
/* =================================================================== */
int __loadds PASCAL FAR bind( SOCKET s, const struct sockaddr FAR *name, int namelen )
{
    const struct sockaddr_in FAR *sin = (const struct sockaddr_in FAR *)name;
    (void)namelen;
    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    g_sk[ s ].lport = ntohs( sin->sin_port );
    return 0;
}

int __loadds PASCAL FAR listen( SOCKET s, int backlog )
{
    KLISTEN FAR *ls = (KLISTEN FAR *)g_buf;
    WORD cf;
    (void)backlog;
    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    ls->handle = s;
    ls->port = g_sk[ s ].lport;
    ls->spare = 0;
    sk_op( K_LISTEN, sizeof( KLISTEN ), &cf );
    if ( cf ) { g_lasterr = WSAEINVAL; return SOCKET_ERROR; }
    return 0;
}

SOCKET __loadds PASCAL FAR accept( SOCKET s, struct sockaddr FAR *addr, int FAR *addrlen )
{
    KACCEPT FAR *ac = (KACCEPT FAR *)g_buf;
    WORD cf, nh;

    if ( !sk_valid( s ) ) return INVALID_SOCKET;
    g_inblock = 1; g_cancel = 0;
    for ( ;; ) {
        ac->handle = s; ac->newh = 0; ac->ip = 0; ac->port = 0; ac->spare = 0;
        sk_op( K_ACCEPT, sizeof( KACCEPT ), &cf );
        if ( cf ) { g_inblock = 0; g_lasterr = WSAEINVAL; return INVALID_SOCKET; }
        nh = (WORD)ac->newh;
        if ( nh >= 1 && nh <= MAXSOCK ) {
            g_sk[ nh ].used = 1;
            g_sk[ nh ].type = K_STREAM;
            g_sk[ nh ].nonblock = 0;
            g_sk[ nh ].awnd = 0; g_sk[ nh ].amsg = 0; g_sk[ nh ].aevent = 0;
            if ( addr && addrlen && *addrlen >= (int)sizeof( struct sockaddr_in ) ) {
                struct sockaddr_in FAR *sin = (struct sockaddr_in FAR *)addr;
                sin->sin_family = AF_INET;
                sin->sin_port = htons( ac->port );
                sin->sin_addr.s_addr = htonl( ac->ip );
                *addrlen = sizeof( struct sockaddr_in );
            }
            g_inblock = 0;
            return nh;
        }
        if ( g_sk[ s ].nonblock ) { g_inblock = 0; g_lasterr = WSAEWOULDBLOCK; return INVALID_SOCKET; }
        if ( sk_wait() ) { g_inblock = 0; return INVALID_SOCKET; }
    }
}

/* =================================================================== */
/*  select / FD macros                                                 */
/* =================================================================== */
int __loadds PASCAL FAR __WSAFDIsSet( SOCKET s, fd_set FAR *set )
{
    unsigned i;
    for ( i = 0; i < set->fd_count; i++ )
        if ( set->fd_array[ i ] == s ) return 1;
    return 0;
}

static void fd_zero( fd_set FAR *set ) { if ( set ) set->fd_count = 0; }
static void fd_add( fd_set FAR *set, SOCKET s )
{
    if ( set && set->fd_count < FD_SETSIZE ) set->fd_array[ set->fd_count++ ] = s;
}

int __loadds PASCAL FAR select( int nfds, fd_set FAR *rd, fd_set FAR *wr,
                       fd_set FAR *ex, const struct timeval FAR *tv )
{
    fd_set rin, win;
    DWORD deadline = 0;
    int haveto = 0;
    (void)nfds;

    if ( rd ) rin = *rd; else rin.fd_count = 0;
    if ( wr ) win = *wr; else win.fd_count = 0;

    if ( tv ) {
        haveto = 1;
        deadline = GetTickCount() + (DWORD)tv->tv_sec * 1000L + (DWORD)tv->tv_usec / 1000L;
    }
    g_inblock = 1; g_cancel = 0;
    for ( ;; ) {
        int count = 0;
        unsigned i;
        fd_zero( rd ); fd_zero( wr ); fd_zero( ex );
        for ( i = 0; i < rin.fd_count; i++ ) {
            SOCKET s = rin.fd_array[ i ];
            KSTAT kst;
            if ( sk_state( s, &kst ) == 0 &&
                 ( kst.rxready > 0 || (WORD)kst.state > KS_ESTABLISHED
                   || (WORD)kst.state == KS_LISTEN ) ) {
                fd_add( rd, s ); count++;
            }
        }
        for ( i = 0; i < win.fd_count; i++ ) {
            SOCKET s = win.fd_array[ i ];
            KSTAT kst;
            if ( sk_state( s, &kst ) == 0 &&
                 ( (WORD)kst.state == KS_ESTABLISHED && kst.txfree > 0 ) ) {
                fd_add( wr, s ); count++;
            }
        }
        if ( count > 0 ) { g_inblock = 0; return count; }
        if ( haveto && GetTickCount() >= deadline ) { g_inblock = 0; return 0; }
        if ( sk_wait() ) { g_inblock = 0; return SOCKET_ERROR; }
    }
}

/* =================================================================== */
/*  ioctl / options / names                                            */
/* =================================================================== */
int __loadds PASCAL FAR ioctlsocket( SOCKET s, long cmd, unsigned long FAR *argp )
{
    KSTAT kst;
    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    if ( cmd == (long)FIONBIO ) {
        g_sk[ s ].nonblock = ( argp && *argp ) ? 1 : 0;
        return 0;
    }
    if ( cmd == (long)FIONREAD ) {
        if ( argp ) {
            *argp = ( sk_state( s, &kst ) == 0 ) ? kst.rxready : 0;
        }
        return 0;
    }
    g_lasterr = WSAEINVAL;
    return SOCKET_ERROR;
}

int __loadds PASCAL FAR setsockopt( SOCKET s, int level, int opt,
                           const char FAR *val, int len )
{
    (void)level; (void)opt; (void)val; (void)len;
    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    return 0;                           /* accepted and ignored */
}

int __loadds PASCAL FAR getsockopt( SOCKET s, int level, int opt, char FAR *val, int FAR *len )
{
    (void)level; (void)opt;
    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    if ( val && len && *len >= 4 ) { *(long FAR *)val = 0; *len = 4; }
    return 0;
}

int __loadds PASCAL FAR getpeername( SOCKET s, struct sockaddr FAR *name, int FAR *namelen )
{
    struct sockaddr_in FAR *sin = (struct sockaddr_in FAR *)name;
    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    if ( sin && namelen && *namelen >= (int)sizeof( struct sockaddr_in ) ) {
        sin->sin_family = AF_INET;
        sin->sin_port = 0;
        sin->sin_addr.s_addr = 0;
        *namelen = sizeof( struct sockaddr_in );
    }
    return 0;
}

int __loadds PASCAL FAR getsockname( SOCKET s, struct sockaddr FAR *name, int FAR *namelen )
{
    struct sockaddr_in FAR *sin = (struct sockaddr_in FAR *)name;
    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    if ( sin && namelen && *namelen >= (int)sizeof( struct sockaddr_in ) ) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons( g_sk[ s ].lport );
        sin->sin_addr.s_addr = 0;
        *namelen = sizeof( struct sockaddr_in );
    }
    return 0;
}

/* =================================================================== */
/*  byte order and address helpers                                     */
/* =================================================================== */
u_long __loadds PASCAL FAR htonl( u_long h )
{
    return ( ( h & 0xFFL ) << 24 ) | ( ( h & 0xFF00L ) << 8 ) |
           ( ( h >> 8 ) & 0xFF00L ) | ( ( h >> 24 ) & 0xFFL );
}
u_long __loadds PASCAL FAR ntohl( u_long n ) { return htonl( n ); }
u_short __loadds PASCAL FAR htons( u_short h ) { return (u_short)( ( h << 8 ) | ( h >> 8 ) ); }
u_short __loadds PASCAL FAR ntohs( u_short n ) { return htons( n ); }

unsigned long __loadds PASCAL FAR inet_addr( const char FAR *cp )
{
    unsigned long parts[ 4 ];
    int np = 0;
    unsigned long v = 0;
    int digits = 0;

    if ( cp == 0 ) return INADDR_NONE;
    for ( ;; ) {
        char c = *cp++;
        if ( c >= '0' && c <= '9' ) { v = v * 10 + ( c - '0' ); digits = 1; }
        else if ( c == '.' ) {
            if ( !digits || np >= 3 || v > 255 ) return INADDR_NONE;
            parts[ np++ ] = v; v = 0; digits = 0;
        }
        else if ( c == 0 ) {
            if ( !digits || np != 3 || v > 255 ) return INADDR_NONE;
            parts[ np ] = v;
            /* network byte order: a.b.c.d -> bytes a,b,c,d on the wire */
            return htonl( ( parts[0] << 24 ) | ( parts[1] << 16 ) |
                          ( parts[2] << 8 ) | parts[3] );
        }
        else return INADDR_NONE;
    }
}

static char g_ntoa[ 18 ];
static char *putu( char *p, unsigned v )
{
    char tmp[ 4 ]; int n = 0;
    if ( v == 0 ) { *p++ = '0'; return p; }
    while ( v ) { tmp[ n++ ] = (char)( '0' + v % 10 ); v /= 10; }
    while ( n ) *p++ = tmp[ --n ];
    return p;
}
char FAR * __loadds PASCAL FAR inet_ntoa( struct in_addr in )
{
    unsigned long n = ntohl( in.s_addr );     /* back to host order a.b.c.d */
    char *p = g_ntoa;
    p = putu( p, (unsigned)( ( n >> 24 ) & 0xFF ) ); *p++ = '.';
    p = putu( p, (unsigned)( ( n >> 16 ) & 0xFF ) ); *p++ = '.';
    p = putu( p, (unsigned)( ( n >> 8 ) & 0xFF ) ); *p++ = '.';
    p = putu( p, (unsigned)( n & 0xFF ) ); *p = 0;
    return (char FAR *)g_ntoa;
}

/* =================================================================== */
/*  name resolution                                                    */
/* =================================================================== */
static struct hostent g_he;
static char FAR      *g_healist[ 2 ];
static char FAR      *g_healias[ 1 ];

struct hostent FAR * __loadds PASCAL FAR gethostbyname( const char FAR *name )
{
    KRES FAR *rs = (KRES FAR *)g_buf;
    unsigned long ip;
    WORD cf;
    int i;

    if ( !g_started || name == 0 ) { g_lasterr = WSANOTINITIALISED; return 0; }

    ip = inet_addr( name );
    if ( ip != INADDR_NONE ) {
        g_headdr = ip;                  /* already a dotted quad */
    } else {
        /* ask the parked stack's resolver; poll until done */
        for ( i = 0; name[ i ] && i < 127; i++ ) g_hename[ i ] = name[ i ];
        g_hename[ i ] = 0;
        g_inblock = 1; g_cancel = 0;
        for ( ;; ) {
            rs->ip = 0; rs->state = 0;
            for ( i = 0; i < 128; i++ ) rs->name[ i ] = g_hename[ i ];
            sk_op( K_RESOLVE, (WORD)sizeof( KRES ), &cf );
            if ( cf ) { g_inblock = 0; g_lasterr = WSAHOST_NOT_FOUND; return 0; }
            if ( (WORD)rs->state == K_NET_DONE ) {
                g_headdr = htonl( rs->ip );   /* kernel gives host order */
                break;
            }
            if ( (WORD)rs->state == K_NET_FAIL ) {
                g_inblock = 0; g_lasterr = WSAHOST_NOT_FOUND; return 0;
            }
            if ( sk_wait() ) { g_inblock = 0; return 0; }
        }
        g_inblock = 0;
    }

    for ( i = 0; name[ i ] && i < 127; i++ ) g_hename[ i ] = name[ i ];
    g_hename[ i ] = 0;
    g_healist[ 0 ] = (char FAR *)&g_headdr;
    g_healist[ 1 ] = 0;
    g_healias[ 0 ] = 0;
    g_he.h_name = (char FAR *)g_hename;
    g_he.h_aliases = (char FAR * FAR *)g_healias;
    g_he.h_addrtype = AF_INET;
    g_he.h_length = 4;
    g_he.h_addr_list = (char FAR * FAR *)g_healist;
    return (struct hostent FAR *)&g_he;
}

struct hostent FAR * __loadds PASCAL FAR gethostbyaddr( const char FAR *addr, int len, int type )
{
    (void)len;
    if ( !g_started || addr == 0 || type != AF_INET ) { g_lasterr = WSAHOST_NOT_FOUND; return 0; }
    g_headdr = *(const unsigned long FAR *)addr;      /* network order in */
    {
        struct in_addr ia; char FAR *txt; int i;
        ia.s_addr = g_headdr;
        txt = inet_ntoa( ia );
        for ( i = 0; txt[ i ] && i < 127; i++ ) g_hename[ i ] = txt[ i ];
        g_hename[ i ] = 0;
    }
    g_healist[ 0 ] = (char FAR *)&g_headdr; g_healist[ 1 ] = 0;
    g_healias[ 0 ] = 0;
    g_he.h_name = (char FAR *)g_hename;
    g_he.h_aliases = (char FAR * FAR *)g_healias;
    g_he.h_addrtype = AF_INET; g_he.h_length = 4;
    g_he.h_addr_list = (char FAR * FAR *)g_healist;
    return (struct hostent FAR *)&g_he;
}

int __loadds PASCAL FAR gethostname( char FAR *name, int namelen )
{
    static const char hn[] = "pmdos";
    int i;
    for ( i = 0; i < namelen - 1 && hn[ i ]; i++ ) name[ i ] = hn[ i ];
    if ( namelen > 0 ) name[ i ] = 0;
    return 0;
}

/* services and protocols: a minimal set, enough for the common clients */
static struct servent  g_se;
static char            g_sename[ 16 ];
static char            g_seproto[ 8 ];
static char FAR       *g_sealias[ 1 ] = { 0 };

static struct servent FAR *fill_serv( const char FAR *nm, unsigned short port,
                                      const char FAR *proto )
{
    int i;
    for ( i = 0; nm[ i ] && i < 15; i++ ) g_sename[ i ] = nm[ i ];
    g_sename[ i ] = 0;
    for ( i = 0; proto[ i ] && i < 7; i++ ) g_seproto[ i ] = proto[ i ];
    g_seproto[ i ] = 0;
    g_se.s_name = (char FAR *)g_sename;
    g_se.s_aliases = (char FAR * FAR *)g_sealias;
    g_se.s_port = htons( port );
    g_se.s_proto = (char FAR *)g_seproto;
    return (struct servent FAR *)&g_se;
}

static int sk_streq( const char FAR *a, const char FAR *b )
{
    while ( *a && *b ) { if ( *a != *b ) return 0; a++; b++; }
    return *a == *b;
}

struct servent FAR * __loadds PASCAL FAR getservbyname( const char FAR *name, const char FAR *proto )
{
    const char FAR *pr = proto ? proto : (const char FAR *)"tcp";
    if ( sk_streq( name, (const char FAR *)"http" ) )  return fill_serv( name, 80, pr );
    if ( sk_streq( name, (const char FAR *)"https" ) ) return fill_serv( name, 443, pr );
    if ( sk_streq( name, (const char FAR *)"ftp" ) )   return fill_serv( name, 21, pr );
    if ( sk_streq( name, (const char FAR *)"telnet" ) )return fill_serv( name, 23, pr );
    if ( sk_streq( name, (const char FAR *)"smtp" ) )  return fill_serv( name, 25, pr );
    if ( sk_streq( name, (const char FAR *)"pop3" ) )  return fill_serv( name, 110, pr );
    if ( sk_streq( name, (const char FAR *)"domain" ) )return fill_serv( name, 53, pr );
    if ( sk_streq( name, (const char FAR *)"ntp" ) )   return fill_serv( name, 123, pr );
    g_lasterr = WSANO_DATA;
    return 0;
}

struct servent FAR * __loadds PASCAL FAR getservbyport( int port, const char FAR *proto )
{
    const char FAR *pr = proto ? proto : (const char FAR *)"tcp";
    switch ( ntohs( (u_short)port ) ) {
        case 80:  return fill_serv( (const char FAR *)"http", 80, pr );
        case 443: return fill_serv( (const char FAR *)"https", 443, pr );
        case 21:  return fill_serv( (const char FAR *)"ftp", 21, pr );
        case 23:  return fill_serv( (const char FAR *)"telnet", 23, pr );
        case 25:  return fill_serv( (const char FAR *)"smtp", 25, pr );
        case 110: return fill_serv( (const char FAR *)"pop3", 110, pr );
        case 53:  return fill_serv( (const char FAR *)"domain", 53, pr );
        case 123: return fill_serv( (const char FAR *)"ntp", 123, pr );
    }
    g_lasterr = WSANO_DATA;
    return 0;
}

static struct protoent g_pe;
static char            g_pename[ 8 ];
static char FAR       *g_pealias[ 1 ] = { 0 };
static struct protoent FAR *fill_proto( const char FAR *nm, int num )
{
    int i;
    for ( i = 0; nm[ i ] && i < 7; i++ ) g_pename[ i ] = nm[ i ];
    g_pename[ i ] = 0;
    g_pe.p_name = (char FAR *)g_pename;
    g_pe.p_aliases = (char FAR * FAR *)g_pealias;
    g_pe.p_proto = num;
    return (struct protoent FAR *)&g_pe;
}
struct protoent FAR * __loadds PASCAL FAR getprotobyname( const char FAR *name )
{
    if ( sk_streq( name, (const char FAR *)"tcp" ) ) return fill_proto( name, 6 );
    if ( sk_streq( name, (const char FAR *)"udp" ) ) return fill_proto( name, 17 );
    if ( sk_streq( name, (const char FAR *)"icmp" ) ) return fill_proto( name, 1 );
    g_lasterr = WSANO_DATA;
    return 0;
}
struct protoent FAR * __loadds PASCAL FAR getprotobynumber( int num )
{
    if ( num == 6 ) return fill_proto( (const char FAR *)"tcp", 6 );
    if ( num == 17 ) return fill_proto( (const char FAR *)"udp", 17 );
    if ( num == 1 ) return fill_proto( (const char FAR *)"icmp", 1 );
    g_lasterr = WSANO_DATA;
    return 0;
}

/* =================================================================== */
/*  UDP                                                                */
/* =================================================================== */
int __loadds PASCAL FAR sendto( SOCKET s, const char FAR *buf, int len, int flags,
                       const struct sockaddr FAR *to, int tolen )
{
    const struct sockaddr_in FAR *sin = (const struct sockaddr_in FAR *)to;
    KIO FAR *io = (KIO FAR *)g_buf;
    int chunk;
    WORD cf, took;
    (void)flags; (void)tolen;

    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    chunk = ( len > KBUF_MAX ) ? KBUF_MAX : len;
    io->handle = s;
    io->ip = to ? ntohl( sin->sin_addr.s_addr ) : 0;
    io->port = to ? ntohs( sin->sin_port ) : 0;
    io->spare = 0;
    io->len = chunk;
    sk_memcpy( g_buf + K_IO_HDR, buf, chunk );
    took = sk_op( K_SEND, (WORD)( K_IO_HDR + chunk ), &cf );
    if ( cf ) { g_lasterr = WSAECONNRESET; return SOCKET_ERROR; }
    return took;
}

int __loadds PASCAL FAR recvfrom( SOCKET s, char FAR *buf, int len, int flags,
                         struct sockaddr FAR *from, int FAR *fromlen )
{
    KIO FAR *io = (KIO FAR *)g_buf;
    int want, got;
    WORD cf;
    (void)flags;

    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    want = ( len > KBUF_MAX ) ? KBUF_MAX : len;
    g_inblock = 1; g_cancel = 0;
    for ( ;; ) {
        io->handle = s; io->ip = 0; io->port = 0; io->spare = 0;
        io->len = want;
        got = (int)sk_op( K_RECV, (WORD)( K_IO_HDR + want ), &cf );
        if ( cf ) { g_inblock = 0; g_lasterr = WSAECONNRESET; return SOCKET_ERROR; }
        if ( got > 0 ) {
            sk_memcpy( buf, g_buf + K_IO_HDR, got );
            if ( from && fromlen && *fromlen >= (int)sizeof( struct sockaddr_in ) ) {
                struct sockaddr_in FAR *sin = (struct sockaddr_in FAR *)from;
                sin->sin_family = AF_INET;
                sin->sin_port = htons( io->port );
                sin->sin_addr.s_addr = htonl( io->ip );
                *fromlen = sizeof( struct sockaddr_in );
            }
            g_inblock = 0;
            return got;
        }
        if ( g_sk[ s ].nonblock ) { g_inblock = 0; g_lasterr = WSAEWOULDBLOCK; return SOCKET_ERROR; }
        if ( sk_wait() ) { g_inblock = 0; return SOCKET_ERROR; }
    }
}

/* =================================================================== */
/*  errors and blocking-hook management                                */
/* =================================================================== */
int __loadds PASCAL FAR WSAGetLastError( void ) { return g_lasterr; }
void __loadds PASCAL FAR WSASetLastError( int err ) { g_lasterr = err; }
BOOL __loadds PASCAL FAR WSAIsBlocking( void ) { return g_inblock ? TRUE : FALSE; }

int __loadds PASCAL FAR WSACancelBlockingCall( void )
{
    if ( !g_inblock ) { g_lasterr = WSAEINVAL; return SOCKET_ERROR; }
    g_cancel = 1;
    return 0;
}

FARPROC __loadds PASCAL FAR WSASetBlockingHook( FARPROC hook )
{
    FARPROC old = g_blkhook;
    g_blkhook = hook;
    return old;
}

int __loadds PASCAL FAR WSAUnhookBlockingHook( void )
{
    g_blkhook = 0;
    return 0;
}

/* =================================================================== */
/*  asynchronous calls - minimal (Stage C's next increment)            */
/* =================================================================== */
/* ws_ontimer - the async-event poller.  Runs from a Windows timer (so
   it fires whenever the app pumps messages); for each socket with a
   WSAAsyncSelect registration it reads the kernel state and PostMessages
   the newly-true events the app asked for.  FD_READ re-arms when recv
   drains the socket; FD_WRITE re-arms when a send would block; the one-
   shot events (CONNECT, CLOSE, ACCEPT) stay fired. */
void __loadds FAR PASCAL ws_ontimer( HWND hw, UINT msg, UINT id, DWORD t )
{
    SOCKET s;
    KSTAT  kst;
    (void)hw; (void)msg; (void)id; (void)t;

    for ( s = 1; s <= MAXSOCK; s++ ) {
        long want;
        if ( !g_sk[ s ].used || !g_sk[ s ].awnd ) continue;
        want = g_sk[ s ].aevent;
        if ( !want ) continue;
        if ( sk_state( s, &kst ) != 0 ) continue;

        if ( ( want & FD_CONNECT ) && !( g_sk[ s ].afired & FD_CONNECT ) ) {
            if ( (WORD)kst.state == KS_ESTABLISHED ) {
                PostMessage( g_sk[ s ].awnd, g_sk[ s ].amsg, s,
                             WSAMAKESELECTREPLY( FD_CONNECT, 0 ) );
                g_sk[ s ].afired |= FD_CONNECT;
            } else if ( (WORD)kst.state == KS_ERROR ) {
                PostMessage( g_sk[ s ].awnd, g_sk[ s ].amsg, s,
                             WSAMAKESELECTREPLY( FD_CONNECT, WSAECONNREFUSED ) );
                g_sk[ s ].afired |= FD_CONNECT;
            }
        }
        if ( ( want & FD_WRITE ) && !( g_sk[ s ].afired & FD_WRITE ) &&
             (WORD)kst.state == KS_ESTABLISHED && kst.txfree > 0 ) {
            PostMessage( g_sk[ s ].awnd, g_sk[ s ].amsg, s,
                         WSAMAKESELECTREPLY( FD_WRITE, 0 ) );
            g_sk[ s ].afired |= FD_WRITE;
        }
        if ( ( want & FD_READ ) && !( g_sk[ s ].afired & FD_READ ) &&
             kst.rxready > 0 ) {
            PostMessage( g_sk[ s ].awnd, g_sk[ s ].amsg, s,
                         WSAMAKESELECTREPLY( FD_READ, 0 ) );
            g_sk[ s ].afired |= FD_READ;
        }
        if ( ( want & FD_ACCEPT ) && !( g_sk[ s ].afired & FD_ACCEPT ) &&
             (WORD)kst.state == KS_LISTEN && kst.rxready > 0 ) {
            PostMessage( g_sk[ s ].awnd, g_sk[ s ].amsg, s,
                         WSAMAKESELECTREPLY( FD_ACCEPT, 0 ) );
            g_sk[ s ].afired |= FD_ACCEPT;
        }
        if ( ( want & FD_CLOSE ) && !( g_sk[ s ].afired & FD_CLOSE ) &&
             (WORD)kst.state > KS_ESTABLISHED ) {
            PostMessage( g_sk[ s ].awnd, g_sk[ s ].amsg, s,
                         WSAMAKESELECTREPLY( FD_CLOSE, 0 ) );
            g_sk[ s ].afired |= FD_CLOSE;
        }
    }
}

int __loadds PASCAL FAR WSAAsyncSelect( SOCKET s, HWND hWnd, unsigned wMsg, long lEvent )
{
    if ( !sk_valid( s ) ) return SOCKET_ERROR;
    g_sk[ s ].awnd = hWnd;
    g_sk[ s ].amsg = wMsg;
    g_sk[ s ].aevent = lEvent;
    g_sk[ s ].afired = 0;
    g_sk[ s ].nonblock = lEvent ? 1 : 0;   /* async sockets are non-blocking */
    if ( lEvent && hWnd && !g_timer ) {
        /* a 100ms timer drives the poller; it fires whenever the app's
           message loop runs, which an async winsock app always has */
        g_timer = SetTimer( NULL, 0, 100, (TIMERPROC)ws_ontimer );
    }
    return 0;
}

HANDLE __loadds PASCAL FAR WSAAsyncGetHostByName( HWND hWnd, unsigned wMsg, const char FAR *name,
                                         char FAR *buf, int buflen )
{
    (void)hWnd; (void)wMsg; (void)name; (void)buf; (void)buflen;
    g_lasterr = WSAEOPNOTSUPP;
    return 0;
}
HANDLE __loadds PASCAL FAR WSAAsyncGetHostByAddr( HWND hWnd, unsigned wMsg, const char FAR *addr,
                                         int len, int type, char FAR *buf, int buflen )
{
    (void)hWnd; (void)wMsg; (void)addr; (void)len; (void)type; (void)buf; (void)buflen;
    g_lasterr = WSAEOPNOTSUPP;
    return 0;
}
HANDLE __loadds PASCAL FAR WSAAsyncGetProtoByName( HWND hWnd, unsigned wMsg, const char FAR *name,
                                          char FAR *buf, int buflen )
{
    (void)hWnd; (void)wMsg; (void)name; (void)buf; (void)buflen;
    g_lasterr = WSAEOPNOTSUPP;
    return 0;
}
HANDLE __loadds PASCAL FAR WSAAsyncGetProtoByNumber( HWND hWnd, unsigned wMsg, int number,
                                            char FAR *buf, int buflen )
{
    (void)hWnd; (void)wMsg; (void)number; (void)buf; (void)buflen;
    g_lasterr = WSAEOPNOTSUPP;
    return 0;
}
HANDLE __loadds PASCAL FAR WSAAsyncGetServByName( HWND hWnd, unsigned wMsg, const char FAR *name,
                                         const char FAR *proto, char FAR *buf, int buflen )
{
    (void)hWnd; (void)wMsg; (void)name; (void)proto; (void)buf; (void)buflen;
    g_lasterr = WSAEOPNOTSUPP;
    return 0;
}
HANDLE __loadds PASCAL FAR WSAAsyncGetServByPort( HWND hWnd, unsigned wMsg, int port,
                                         const char FAR *proto, char FAR *buf, int buflen )
{
    (void)hWnd; (void)wMsg; (void)port; (void)proto; (void)buf; (void)buflen;
    g_lasterr = WSAEOPNOTSUPP;
    return 0;
}
int __loadds PASCAL FAR WSACancelAsyncRequest( HANDLE hAsyncTaskHandle )
{
    (void)hAsyncTaskHandle;
    return 0;
}

/* =================================================================== */
/*  DLL entry and exit                                                 */
/* =================================================================== */
int __loadds FAR PASCAL LibMain( HINSTANCE hInstance, WORD wDataSeg, WORD cbHeap, LPSTR lpCmdLine )
{
    (void)wDataSeg; (void)cbHeap; (void)lpCmdLine;
    g_hinst = hInstance;
    return 1;
}

int __loadds FAR PASCAL WEP( int nParam )
{
    (void)nParam;
    if ( g_timer ) { KillTimer( NULL, g_timer ); g_timer = 0; }
    if ( g_dossel ) { GlobalDosFree( g_dossel ); g_dossel = 0; }
    return 1;
}
