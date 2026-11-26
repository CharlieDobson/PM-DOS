/*
 * SOCKLIB.C - the shared runtime for the PM-DOS network clients
 *
 * See SOCKLIB.H.  The socket calls go to the stack on 21h/F1h AL=26h
 * (the stack pumps itself at every call, so a poll here moves it on);
 * the waiting loops read the clock with 21h/2Ch.  The console is 40h to
 * handle 1 and 0Bh/08h from handle 0.
 */
#include "socklib.h"

/* ---------------------------------------------------------------- */
/* small helpers                                                     */
/* ---------------------------------------------------------------- */

void sl_memcpy( void *to, const void *from, unsigned int len )
{
    uint8_t *dst = (uint8_t *)to;
    const uint8_t *src = (const uint8_t *)from;
    while ( len-- ) *dst++ = *src++;
}

void sl_memzero( void *to, unsigned int len )
{
    uint8_t *dst = (uint8_t *)to;
    while ( len-- ) *dst++ = 0;
}

unsigned int sl_strlen( const char *s )
{
    unsigned int n = 0;
    while ( s[n] ) n++;
    return n;
}

void sl_strcpy( char *to, const char *from, unsigned int max )
{
    unsigned int i;
    for ( i = 0; i + 1 < max && from[i]; i++ ) to[i] = from[i];
    to[i] = 0;
}

int sl_strcmp( const char *a, const char *b )
{
    while ( *a && *a == *b ) { a++; b++; }
    return (int)( (unsigned char)*a ) - (int)( (unsigned char)*b );
}

static char lower( char ch )
{
    return ( ch >= 'A' && ch <= 'Z' ) ? (char)( ch + 32 ) : ch;
}

int sl_strcasecmp( const char *a, const char *b )
{
    while ( *a && lower( *a ) == lower( *b ) ) { a++; b++; }
    return (int)( (unsigned char)lower( *a ) ) - (int)( (unsigned char)lower( *b ) );
}

unsigned int sl_atou( const char *s )
{
    unsigned int value = 0;
    while ( *s == ' ' ) s++;
    while ( *s >= '0' && *s <= '9' ) value = value * 10 + ( *s++ - '0' );
    return value;
}

static void clear( REGS *regs )
{
    sl_memzero( regs, sizeof( *regs ) );
}

/* ---------------------------------------------------------------- */
/* the clock                                                         */
/* ---------------------------------------------------------------- */

uint32_t sl_now( void )
{
    REGS         regs;
    unsigned int hour, minute, second, cs;

    clear( &regs );
    regs.eax = 0x2C00;
    int21( &regs );
    hour   = ( regs.ecx >> 8 ) & 0xFF;
    minute = regs.ecx & 0xFF;
    second = ( regs.edx >> 8 ) & 0xFF;
    cs     = regs.edx & 0xFF;
    return ( ( hour * 60 + minute ) * 60 + second ) * 100 + cs;
}

uint32_t sl_since( uint32_t start )
{
    uint32_t now = sl_now();
    if ( now < start ) now += 24UL * 60 * 60 * 100;     /* past midnight */
    return now - start;
}

static void two( char *to, unsigned int value )
{
    to[0] = (char)( '0' + ( value / 10 ) % 10 );
    to[1] = (char)( '0' + value % 10 );
}

void sl_now14( char *out )
{
    REGS         regs;
    unsigned int year, month, day, hour, minute, second;

    clear( &regs );
    regs.eax = 0x2A00;                  /* get date: CX year, DH month, DL day */
    int21( &regs );
    year  = regs.ecx & 0xFFFF;
    month = ( regs.edx >> 8 ) & 0xFF;
    day   = regs.edx & 0xFF;

    clear( &regs );
    regs.eax = 0x2C00;                  /* get time: CH hour, CL minute, DH second */
    int21( &regs );
    hour   = ( regs.ecx >> 8 ) & 0xFF;
    minute = regs.ecx & 0xFF;
    second = ( regs.edx >> 8 ) & 0xFF;

    out[0] = (char)( '0' + ( year / 1000 ) % 10 );
    out[1] = (char)( '0' + ( year / 100 ) % 10 );
    out[2] = (char)( '0' + ( year / 10 ) % 10 );
    out[3] = (char)( '0' + year % 10 );
    two( out + 4,  month );
    two( out + 6,  day );
    two( out + 8,  hour );
    two( out + 10, minute );
    two( out + 12, second );
    out[14] = 0;
}

/* ---------------------------------------------------------------- */
/* the console                                                       */
/* ---------------------------------------------------------------- */

#define OUT_BUF 2048
static uint8_t      out_buf[OUT_BUF];
static unsigned int out_len;

void sl_flush( void )
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

void sl_putc( char ch )
{
    if ( out_len == OUT_BUF ) sl_flush();
    out_buf[out_len++] = (uint8_t)ch;
    if ( ch == '\n' ) sl_flush();
}

void sl_puts( const char *text )
{
    while ( *text ) sl_putc( *text++ );
}

void sl_putu( unsigned int value )
{
    char digits[12];
    int  n = 0;
    do { digits[n++] = (char)( '0' + value % 10 ); value /= 10; } while ( value );
    while ( n ) sl_putc( digits[--n] );
}

void sl_putip( uint32_t ip )
{
    sl_putu( IP_A( ip ) ); sl_putc( '.' );
    sl_putu( IP_B( ip ) ); sl_putc( '.' );
    sl_putu( IP_C( ip ) ); sl_putc( '.' );
    sl_putu( IP_D( ip ) );
}

int sl_kbhit( void )
{
    REGS regs;
    clear( &regs );
    regs.eax = 0x0B00;
    int21( &regs );
    return ( regs.eax & 0xFF ) == 0xFF;
}

int sl_getkey( void )
{
    REGS         regs;
    unsigned int ch;

    clear( &regs );
    regs.eax = 0x0800;                  /* read, no echo */
    int21( &regs );
    ch = regs.eax & 0xFF;
    if ( ch == 0 ) {                    /* an extended key: the scan follows */
        clear( &regs );
        regs.eax = 0x0800;
        int21( &regs );
        return 0x100 | ( regs.eax & 0xFF );
    }
    return (int)ch;
}

/* ---------------------------------------------------------------- */
/* the command tail                                                  */
/* ---------------------------------------------------------------- */

static char  arg_tail[160];
static char *arg_vec[32];
static int   arg_count = -1;

int sl_args( char ***argv )
{
    REGS         regs;
    uint8_t     *psp;
    unsigned int len, i;
    char        *in, *out;
    int          quoted;

    if ( arg_count < 0 ) {
        clear( &regs );
        regs.eax = 0x6200;
        int21( &regs );
        psp = (uint8_t *)regs.ebx;
        len = psp[0x80];
        if ( len > 127 ) len = 127;
        for ( i = 0; i < len; i++ ) arg_tail[i] = (char)psp[0x81 + i];
        arg_tail[len] = 0;

        in = out = arg_tail;
        arg_count = 0;
        for ( ;; ) {
            while ( *in == ' ' || *in == '\t' ) in++;
            if ( *in == 0 || *in == '\r' ) break;
            if ( arg_count == 31 ) break;
            arg_vec[arg_count++] = out;
            quoted = 0;
            while ( *in && *in != '\r' ) {
                if ( *in == '"' ) { quoted = !quoted; in++; continue; }
                if ( !quoted && ( *in == ' ' || *in == '\t' ) ) break;
                *out++ = *in++;
            }
            if ( *in ) in++;
            *out++ = 0;
        }
    }
    *argv = arg_vec;
    return arg_count;
}

/* ---------------------------------------------------------------- */
/* the environment                                                   */
/* ---------------------------------------------------------------- */

const char *sl_getenv( const char *name )
{
    REGS         regs;
    const char  *env;
    unsigned int namelen = sl_strlen( name ), i;

    clear( &regs );
    regs.eax = 0x6200;                  /* get PSP (flat pointer in EBX) */
    int21( &regs );
    env = *(const char **)( (uint8_t *)regs.ebx + 0x2C ); /* the env block, flat */
    if ( env == NULL ) return NULL;

    while ( *env ) {
        for ( i = 0; i < namelen && env[i]; i++ ) {
            char a = env[i], b = name[i];
            if ( a >= 'a' && a <= 'z' ) a = (char)( a - 32 );
            if ( b >= 'a' && b <= 'z' ) b = (char)( b - 32 );
            if ( a != b ) break;
        }
        if ( i == namelen && env[namelen] == '=' ) return env + namelen + 1;
        while ( *env ) env++;           /* to this variable's NUL */
        env++;                          /* past it, to the next */
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* the socket call (21h/F1h AL=26h)                                  */
/* ---------------------------------------------------------------- */

static uint32_t sk( uint32_t fn, void *buf, uint32_t len, int *err )
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
/* the interface                                                     */
/* ---------------------------------------------------------------- */

int sl_ifstat( SOCK_IF *info )
{
    int err;
    sl_memzero( info, SOCKIF_LEN );
    sk( SOCK_IFSTAT, info, SOCKIF_LEN, &err );
    return err == 0;
}

int sl_up( unsigned int ms )
{
    SOCK_IF info;
    uint32_t start = sl_now();
    int err;

    for ( ;; ) {
        sl_memzero( &info, SOCKIF_LEN );
        sk( SOCK_IFSTAT, &info, SOCKIF_LEN, &err );
        if ( err != 0 ) return 0;               /* no stack */
        if ( info.up ) return 1;
        if ( sl_since( start ) > ms ) return 0;
    }
}

uint32_t sl_resolve( const char *name, unsigned int ms )
{
    SOCK_RES req;
    uint32_t start = sl_now();
    unsigned int state;
    int err;

    /* a dotted quad resolves to itself */
    {
        unsigned int parts[4], np = 0, value = 0, digits = 0;
        const char *p = name;
        int dotted = 1;
        for ( ;; p++ ) {
            if ( *p >= '0' && *p <= '9' ) { value = value * 10 + ( *p - '0' ); digits++; }
            else if ( *p == '.' || *p == 0 ) {
                if ( !digits || np > 3 ) { dotted = 0; break; }
                if ( np < 4 ) parts[np] = value;
                np++; value = 0; digits = 0;
                if ( *p == 0 ) break;
            } else { dotted = 0; break; }
        }
        if ( dotted && np == 4 && parts[0] < 256 && parts[1] < 256 && parts[2] < 256 && parts[3] < 256 ) {
            return MAKE_IP( parts[0], parts[1], parts[2], parts[3] );
        }
    }

    sl_memzero( &req, sizeof( req ) );
    sl_strcpy( req.name, name, sizeof( req.name ) );
    for ( ;; ) {
        state = sk( SOCK_RESOLVE, &req, SOCKRES_LEN, &err );
        if ( err ) return 0;
        if ( state == NET_DONE ) return req.ip;
        if ( state == NET_FAIL || sl_since( start ) > ms ) return 0;
    }
}

int sl_ping( uint32_t ip, uint16_t seq, uint32_t *rttms, unsigned int ms )
{
    SOCK_PINGREQ req;
    uint32_t start = sl_now();
    unsigned int state;
    int err;

    sl_memzero( &req, sizeof( req ) );
    req.ip = ip;
    req.seq = seq;
    for ( ;; ) {
        state = sk( SOCK_PING, &req, SOCKPING_LEN, &err );
        if ( err ) return 0;
        if ( state == NET_DONE ) { if ( rttms ) *rttms = req.rttms; return 1; }
        if ( state == NET_FAIL || sl_since( start ) > ms ) return 0;
    }
}

/* ---------------------------------------------------------------- */
/* TCP                                                               */
/* ---------------------------------------------------------------- */

int sl_open( uint32_t type )
{
    SOCK_OPENREQ req;
    unsigned int handle;
    int err;

    req.type = type;
    handle = sk( SOCK_OPEN, &req, sizeof( req ), &err );
    return err ? 0 : (int)handle;
}

void sl_sclose( int handle )
{
    SOCK_HANDLE req;
    int err;
    req.handle = handle;
    sk( SOCK_CLOSE, &req, sizeof( req ), &err );
}

int sl_state( int handle, SOCK_STAT *st )
{
    int err;
    sl_memzero( st, SOCKSTAT_LEN );
    st->handle = handle;
    return (int)sk( SOCK_STATE, st, SOCKSTAT_LEN, &err );
}

int sl_connect( int handle, uint32_t ip, uint16_t port, unsigned int ms )
{
    SOCK_CONNREQ req;
    SOCK_STAT st;
    uint32_t start = sl_now();
    unsigned int state;
    int err;

    sl_memzero( &req, sizeof( req ) );
    req.handle = handle;
    req.ip = ip;
    req.port = port;
    sk( SOCK_CONNECT, &req, sizeof( req ), &err );
    if ( err ) return 0;

    for ( ;; ) {
        state = (unsigned int)sl_state( handle, &st );
        /* SS_CLOSING too: a server that accepts and ends at once was
           connected to, and the first read says it has ended */
        if ( state == SS_ESTABLISHED || state == SS_CLOSING ) return 1;
        if ( state == SS_ERROR || state == SS_CLOSED ) return 0;
        if ( sl_since( start ) > ms ) return 0;
    }
}

static unsigned int io_buf_room;
static uint8_t *io_buf;                 /* lazily grown send/recv staging */

int sl_send( int handle, const void *data, unsigned int len, unsigned int ms )
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t start = sl_now();
    unsigned int done = 0, state;
    int err;
    SOCK_STAT st;

    /* the SOCK_IO header plus a chunk of data in one buffer */
    static uint8_t sbuf[2048];
    SOCK_IO *msg = (SOCK_IO *)sbuf;

    while ( done < len ) {
        unsigned int chunk = len - done;
        unsigned int taken;
        if ( chunk > sizeof( sbuf ) - SOCK_IO_HDR ) chunk = sizeof( sbuf ) - SOCK_IO_HDR;
        sl_memzero( msg, SOCK_IO_HDR );
        msg->handle = handle;
        msg->len = chunk;
        sl_memcpy( sbuf + SOCK_IO_HDR, p + done, chunk );
        taken = sk( SOCK_SEND, sbuf, SOCK_IO_HDR + chunk, &err );
        if ( err ) return (int)done;
        done += taken;
        if ( taken < chunk ) {
            /* the send buffer is full: let it drain, watch the state */
            if ( sl_since( start ) > ms ) return (int)done;
            state = (unsigned int)sl_state( handle, &st );
            /* the peer having ended (SS_CLOSING) does not stop our side */
            if ( state != SS_ESTABLISHED && state != SS_CLOSING ) return (int)done;
        } else {
            start = sl_now();
        }
    }
    (void)io_buf; (void)io_buf_room;
    return (int)done;
}

int sl_recv( int handle, void *buf, unsigned int max )
{
    static uint8_t rbuf[2048];
    SOCK_IO *msg = (SOCK_IO *)rbuf;
    unsigned int want = max, got;
    int err;

    if ( want > sizeof( rbuf ) - SOCK_IO_HDR ) want = sizeof( rbuf ) - SOCK_IO_HDR;
    sl_memzero( msg, SOCK_IO_HDR );
    msg->handle = handle;
    msg->len = want;
    got = sk( SOCK_RECV, rbuf, SOCK_IO_HDR + want, &err );
    if ( err || got == 0 ) return 0;
    sl_memcpy( buf, rbuf + SOCK_IO_HDR, got );
    return (int)got;
}

int sl_closed( int handle )
{
    SOCK_STAT st;
    unsigned int state = (unsigned int)sl_state( handle, &st );
    if ( st.rxready ) return 0;
    return state == SS_CLOSED || state == SS_ERROR || state == SS_CLOSING;
}

int sl_recvwait( int handle, void *buf, unsigned int max, unsigned int ms )
{
    uint32_t start = sl_now();
    int got;

    for ( ;; ) {
        got = sl_recv( handle, buf, max );
        if ( got ) return got;
        if ( sl_closed( handle ) ) return 0;
        if ( sl_since( start ) > ms ) return 0;
    }
}

int sl_recvline( int handle, char *buf, int max, unsigned int ms )
{
    int      n = 0, got;
    uint8_t  ch;
    uint32_t start = sl_now();

    for ( ;; ) {
        got = sl_recv( handle, &ch, 1 );
        if ( got ) {
            start = sl_now();
            if ( ch == '\n' ) break;
            if ( ch != '\r' && n < max - 1 ) buf[n++] = (char)ch;
        } else {
            if ( sl_closed( handle ) ) { if ( n == 0 ) { buf[0] = 0; return -1; } break; }
            if ( sl_since( start ) > ms ) break;
        }
    }
    buf[n] = 0;
    return n;
}

/* ---------------------------------------------------------------- */
/* TCP server                                                        */
/* ---------------------------------------------------------------- */

int sl_listen( int handle, uint16_t port )
{
    SOCK_LISTENREQ req;
    int err;
    sl_memzero( &req, sizeof( req ) );
    req.handle = handle;
    req.port = port;
    sk( SOCK_LISTEN, &req, sizeof( req ), &err );
    return err == 0;
}

int sl_accept( int handle, uint32_t *peer_ip, uint16_t *peer_port, unsigned int ms )
{
    SOCK_ACCEPTREQ req;
    uint32_t start = sl_now();
    unsigned int newh;
    int err;

    for ( ;; ) {
        sl_memzero( &req, sizeof( req ) );
        req.handle = handle;
        newh = sk( SOCK_ACCEPT, &req, sizeof( req ), &err );
        if ( err ) return 0;
        if ( newh ) {
            if ( peer_ip ) *peer_ip = req.ip;
            if ( peer_port ) *peer_port = req.port;
            return (int)newh;
        }
        if ( sl_since( start ) > ms ) return 0;
    }
}

/* ---------------------------------------------------------------- */
/* UDP                                                               */
/* ---------------------------------------------------------------- */

int sl_dgram_send( int handle, uint32_t ip, uint16_t port, const void *data, unsigned int len )
{
    static uint8_t sbuf[1536];
    SOCK_IO *msg = (SOCK_IO *)sbuf;
    int err;
    unsigned int got;

    if ( len > sizeof( sbuf ) - SOCK_IO_HDR ) len = sizeof( sbuf ) - SOCK_IO_HDR;
    sl_memzero( msg, SOCK_IO_HDR );
    msg->handle = handle;
    msg->ip = ip;
    msg->port = port;
    msg->len = len;
    sl_memcpy( sbuf + SOCK_IO_HDR, data, len );
    got = sk( SOCK_SEND, sbuf, SOCK_IO_HDR + len, &err );
    return err ? 0 : (int)got;
}

int sl_dgram_recv( int handle, uint32_t *from_ip, uint16_t *from_port, void *buf, unsigned int max, unsigned int ms )
{
    static uint8_t rbuf[1536];
    SOCK_IO *msg = (SOCK_IO *)rbuf;
    uint32_t start = sl_now();
    unsigned int want = max, got;
    int err;

    if ( want > sizeof( rbuf ) - SOCK_IO_HDR ) want = sizeof( rbuf ) - SOCK_IO_HDR;
    for ( ;; ) {
        sl_memzero( msg, SOCK_IO_HDR );
        msg->handle = handle;
        msg->len = want;
        got = sk( SOCK_RECV, rbuf, SOCK_IO_HDR + want, &err );
        if ( err ) return 0;
        if ( got ) {
            if ( from_ip ) *from_ip = msg->ip;
            if ( from_port ) *from_port = msg->port;
            sl_memcpy( buf, rbuf + SOCK_IO_HDR, got );
            return (int)got;
        }
        if ( sl_since( start ) > ms ) return 0;
    }
}
