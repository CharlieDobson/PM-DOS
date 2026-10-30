/*
 * DRVSOCK.C - the socket shim for the SMBFS driver
 *
 * The SMB2 engine (CMD\NET\SMB) is transport-agnostic: it drives a
 * connection through a send and a recv callback.  SMBTEST gives it
 * SOCKLIB's, which reach the TCP/IP stack on 21h/F1h AL=26h.  A driver
 * cannot call INT 21h, so this gives the engine the same conversation
 * over NDS_SOCKCALL (the stack directly, the way the stack reaches a
 * card through NDS_NICCALL) and NDS_TICKS for the waiting.
 *
 * The logic mirrors SOCKLIB.C's sl_* exactly; only the two kernel
 * services replace the INT 21h calls.  Nothing blocks in the stack, so
 * the loops poll and the clock tick pumps the stack between polls - the
 * same model a program uses.
 */
#define ND_DRIVER
#include "ndrv.h"
#include "socket.h"
#include "netbios.h"
#include "drvsock.h"

/* ---- the clock: the 18.2 Hz tick, and milliseconds turned into ticks -- */

static u32 ms_ticks( u32 ms )
{
    /* 18.2065 Hz: ticks ~= ms * 0.0182.  Round up, and never zero, so a
       short timeout still waits at least one tick. */
    u32 ticks = ( ms * 182UL + 9999UL ) / 10000UL;
    return ticks ? ticks : 1;
}

static u32 since( u32 start )
{
    return nds_ticks() - start;             /* unsigned: wraps cleanly */
}

/* ---- the socket call (NDS_SOCKCALL) ---- */

static u32 sk( u32 fn, void *buf, u32 len, int *err )
{
    u32 answer = 0;
    int rc = nds_sockcall( fn, buf, len, &answer );
    if ( rc != 0 ) { if ( err ) *err = rc; return 0; }
    if ( err ) *err = 0;
    return answer;
}

static void zero( void *to, u32 len )
{
    u8 *dst = (u8 *)to;
    while ( len-- ) *dst++ = 0;
}

static void copy( void *to, const void *from, u32 len )
{
    u8 *dst = (u8 *)to;
    const u8 *src = (const u8 *)from;
    while ( len-- ) *dst++ = *src++;
}

/* ---- the interface ---- */

/* the computer's NetBIOS name (TCPIP.SYS /NAME=), into out[16]; out is
   left as it is when there is none */
void dsock_nbname( char *out )
{
    SOCK_IF info;
    int err, i;

    zero( &info, SOCKIF_LEN2 );
    sk( SOCK_IFSTAT, &info, SOCKIF_LEN2, &err );
    if ( err != 0 || info.nbname[0] == 0 ) return;
    for ( i = 0; i < 15 && info.nbname[i]; i++ ) out[i] = info.nbname[i];
    out[i] = 0;
}

int dsock_up( u32 ms )
{
    SOCK_IF info;
    u32 start = nds_ticks(), limit = ms_ticks( ms );
    int err;

    for ( ;; ) {
        zero( &info, SOCKIF_LEN );
        sk( SOCK_IFSTAT, &info, SOCKIF_LEN, &err );
        if ( err != 0 ) return 0;               /* no stack */
        if ( info.up ) return 1;
        if ( since( start ) > limit ) return 0;
        dsock_idle();
    }
}

u32 dsock_resolve( const char *name, u32 ms )
{
    SOCK_RES req;
    u32 start = nds_ticks(), limit = ms_ticks( ms ), state;
    int err;

    /* a dotted quad resolves to itself */
    {
        u32 parts[4], np = 0, value = 0, digits = 0;
        const char *scan = name;
        int dotted = 1;
        for ( ;; scan++ ) {
            if ( *scan >= '0' && *scan <= '9' ) { value = value * 10 + (u32)( *scan - '0' ); digits++; }
            else if ( *scan == '.' || *scan == 0 ) {
                if ( !digits || np > 3 ) { dotted = 0; break; }
                if ( np < 4 ) parts[np] = value;
                np++; value = 0; digits = 0;
                if ( *scan == 0 ) break;
            } else { dotted = 0; break; }
        }
        if ( dotted && np == 4 && parts[0] < 256 && parts[1] < 256 && parts[2] < 256 && parts[3] < 256 )
            return MAKE_IP( parts[0], parts[1], parts[2], parts[3] );
    }

    zero( &req, sizeof( req ) );
    {
        u32 i;
        for ( i = 0; i + 1 < sizeof( req.name ) && name[i]; i++ ) req.name[i] = name[i];
        req.name[i] = 0;
    }
    for ( ;; ) {
        state = sk( SOCK_RESOLVE, &req, SOCKRES_LEN, &err );
        if ( err ) return 0;
        if ( state == NET_DONE ) return req.ip;
        if ( state == NET_FAIL || since( start ) > limit ) return 0;
        dsock_idle();
    }
}

/* ---- TCP ---- */

int dsock_open( u32 type )
{
    SOCK_OPENREQ req;
    u32 handle;
    int err;
    req.type = type;
    handle = sk( SOCK_OPEN, &req, sizeof( req ), &err );
    return err ? 0 : (int)handle;
}

void dsock_close( int handle )
{
    SOCK_HANDLE req;
    int err;
    req.handle = (u32)handle;
    sk( SOCK_CLOSE, &req, sizeof( req ), &err );
}

static u32 dsock_state( int handle, SOCK_STAT *st )
{
    int err;
    zero( st, SOCKSTAT_LEN );
    st->handle = (u32)handle;
    return sk( SOCK_STATE, st, SOCKSTAT_LEN, &err );
}

int dsock_connect( int handle, u32 ip, u16 port, u32 ms )
{
    SOCK_CONNREQ req;
    SOCK_STAT st;
    u32 start = nds_ticks(), limit = ms_ticks( ms ), state;
    int err;

    zero( &req, sizeof( req ) );
    req.handle = (u32)handle;
    req.ip = ip;
    req.port = port;
    sk( SOCK_CONNECT, &req, sizeof( req ), &err );
    if ( err ) return 0;

    for ( ;; ) {
        state = dsock_state( handle, &st );
        if ( state == SS_ESTABLISHED ) return 1;
        if ( state == SS_ERROR || state == SS_CLOSED ) return 0;
        if ( since( start ) > limit ) return 0;
        dsock_idle();
    }
}

int dsock_send( int handle, const void *data, u32 len, u32 ms )
{
    const u8 *ptr = (const u8 *)data;
    u32 start = nds_ticks(), limit = ms_ticks( ms ), done = 0;
    int err;
    SOCK_STAT st;
    static u8 sbuf[2048];
    SOCK_IO *msg = (SOCK_IO *)sbuf;

    while ( done < len ) {
        u32 chunk = len - done, taken;
        if ( chunk > sizeof( sbuf ) - SOCK_IO_HDR ) chunk = sizeof( sbuf ) - SOCK_IO_HDR;
        zero( msg, SOCK_IO_HDR );
        msg->handle = (u32)handle;
        msg->len = chunk;
        copy( sbuf + SOCK_IO_HDR, ptr + done, chunk );
        taken = sk( SOCK_SEND, sbuf, SOCK_IO_HDR + chunk, &err );
        if ( err ) return (int)done;
        done += taken;
        if ( taken < chunk ) {
            if ( since( start ) > limit ) return (int)done;
            if ( dsock_state( handle, &st ) != SS_ESTABLISHED ) return (int)done;
            dsock_idle();
        } else {
            start = nds_ticks();
        }
    }
    return (int)done;
}

static int dsock_recv( int handle, void *buf, u32 max )
{
    static u8 rbuf[2048];
    SOCK_IO *msg = (SOCK_IO *)rbuf;
    u32 want = max, got;
    int err;

    if ( want > sizeof( rbuf ) - SOCK_IO_HDR ) want = sizeof( rbuf ) - SOCK_IO_HDR;
    zero( msg, SOCK_IO_HDR );
    msg->handle = (u32)handle;
    msg->len = want;
    got = sk( SOCK_RECV, rbuf, SOCK_IO_HDR + want, &err );
    if ( err || got == 0 ) return 0;
    copy( buf, rbuf + SOCK_IO_HDR, got );
    return (int)got;
}

static int dsock_closed( int handle )
{
    SOCK_STAT st;
    u32 state = dsock_state( handle, &st );
    if ( st.rxready ) return 0;
    return state == SS_CLOSED || state == SS_ERROR || state == SS_CLOSING;
}

int dsock_recvwait( int handle, void *buf, u32 max, u32 ms )
{
    u32 start = nds_ticks(), limit = ms_ticks( ms );
    int got;

    for ( ;; ) {
        got = dsock_recv( handle, buf, max );
        if ( got ) return got;
        if ( dsock_closed( handle ) ) return 0;
        if ( since( start ) > limit ) return 0;
        dsock_idle();
    }
}

/* ---- NetBIOS over NetBEUI (NETBEUI.SYS, through NDS_FLAGCALL) ---- */

#define NB_PIECE 1024                   /* what one call carries, on the stack */

static u32 nb( u32 fn, void *buf, u32 len, int *err )
{
    u32 answer = 0;
    int rc = nds_flagcall( NDTF_NETBIOS, fn, buf, len, &answer );
    if ( err ) *err = rc;
    return rc == 0 ? answer : 0;
}

static u32 nb_state( u32 handle, NB_STAT *st )
{
    int err;
    zero( st, NBSTAT_LEN );
    st->handle = handle;
    nb( NB_STATE, st, NBSTAT_LEN, &err );
    return err ? NBX_FAIL : st->state;
}

int dnb_here( char *name )
{
    NB_IF info;
    int err, i;
    zero( &info, NBIF_LEN );
    nb( NB_IFSTAT, &info, NBIF_LEN, &err );
    if ( err != 0 || !info.up ) return 0;
    for ( i = 0; i < 15 && info.name[i]; i++ ) name[i] = info.name[i];
    name[i] = 0;
    return 1;
}

u32 dnb_call( const char *name, u8 suffix, u32 ms )
{
    NB_NAMEREQ req;
    NB_STAT st;
    u32 handle, start = nds_ticks(), limit = ms_ticks( ms ), state;
    int err, i;

    zero( &req, NBNAME_LEN );
    for ( i = 0; i < 15 && name[i]; i++ ) req.name[i] = name[i];
    req.suffix = suffix;
    handle = nb( NB_CALL, &req, NBNAME_LEN, &err );
    if ( err || handle == 0 ) return 0;
    for ( ;; ) {
        state = nb_state( handle, &st );
        if ( state == NBX_UP ) return handle;
        if ( state != NBX_CALLING || since( start ) > limit ) {
            dnb_hangup( handle );
            return 0;
        }
        dsock_idle();
    }
}

void dnb_hangup( u32 handle )
{
    int err;
    nb( NB_HANGUP, &handle, sizeof( handle ), &err );
}

/* one message, in pieces of NB_PIECE: the parts first, the last with
   NB_SEND, each part waiting while the message before is still going */
int dnb_send( u32 handle, const void *data, u32 len, u32 ms )
{
    static u8 buf[NBIO_HDR + NB_PIECE];  /* off the stack, like dsock_send's */
    NB_IO *io = (NB_IO *)buf;
    NB_STAT st;
    const u8 *p = (const u8 *)data;
    u32 start = nds_ticks(), limit = ms_ticks( ms ), n, got;
    int err;

    do {
        n = len > NB_PIECE ? NB_PIECE : len;
        io->handle = handle;
        io->len = n;
        copy( buf + NBIO_HDR, p, n );
        for ( ;; ) {
            got = nb( n == len ? NB_SEND : NB_SENDP, buf, NBIO_HDR + n, &err );
            if ( err ) return 0;
            if ( got ) break;
            if ( nb_state( handle, &st ) != NBX_UP || since( start ) > limit ) return 0;
            dsock_idle();
        }
        p += n;
        len -= n;
    } while ( len );
    return 1;
}

/* wait for a message: its length, or 0 when the session ended or the
   time ran out */
u32 dnb_wait( u32 handle, u32 ms )
{
    NB_STAT st;
    u32 start = nds_ticks(), limit = ms_ticks( ms );

    for ( ;; ) {
        if ( nb_state( handle, &st ) != NBX_UP ) return st.state == NBX_CLOSED && st.rxready ? st.rxready : 0;
        if ( st.rxready ) return st.rxready;
        if ( since( start ) > limit ) return 0;
        dsock_idle();
    }
}

/* the next part of the message waiting, up to max bytes */
u32 dnb_recv( u32 handle, void *out, u32 max )
{
    static u8 buf[NBIO_HDR + NB_PIECE];
    NB_IO *io = (NB_IO *)buf;
    u32 got;
    int err;

    if ( max > NB_PIECE ) max = NB_PIECE;
    io->handle = handle;
    io->len = 0;
    got = nb( NB_RECV, buf, NBIO_HDR + max, &err );
    if ( err ) return 0;
    if ( got > max ) got = max;
    copy( out, buf + NBIO_HDR, got );
    return got;
}
