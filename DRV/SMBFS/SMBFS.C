/*
 * SMBFS.C - the SMB2/3 network redirector, a 32-bit DEVICE= driver
 *
 * It carries the NDTF_REDIR flag, so the kernel's net-drive file
 * branches (DOS\NETFS.INC) reach it through nd_find_flag + nd_call_row,
 * exactly as a program reaches a driver on 21h/F1h AL=25h.  The kernel
 * marks a drive letter net on NET USE and then routes that letter's
 * opens, reads, writes, closes and finds here as REDIR_* requests
 * (INC\REDIR.H).  This driver turns each into the SMB2/3 conversation
 * (CMD\NET\SMB) over the TCP/IP stack it reaches through NDS_SOCKCALL.
 *
 * The path work is here, in C, rather than in the kernel: folding "."
 * and "..", splitting a find's directory from its wildcard, matching
 * DOS 8.3 wildcards and turning a Windows FILETIME into a DOS stamp.
 *
 * Linked behind DRV\NDSTART.ASM with the SMB2 engine, the crypto, the
 * TLS SHA/SHA512/AES primitives, the socket shim (DRVSOCK) and the
 * little C runtime (DRVRT) - see BUILD.CMD.
 */
#define ND_DRIVER
#include "ndrv.h"
#include "redir.h"
#include "socket.h"
#include "smb2.h"
#include "smb1.h"
#include "drvsock.h"

void *memcpy( void *to, const void *from, unsigned len );
void *memset( void *to, int value, unsigned len );

/* the worker fibre (DRV\SMBFS\DRVFIB.ASM) - how a resumable op gives the
   slice back to the bridge's poll and resumes when called again */
void __cdecl fib_init( u32 stacktop, void ( *entry )( void ) );
void __cdecl fib_enter( void );
void __cdecl fib_yield( void );

/* ---- limits ---- */
#define NET_DRIVES     16          /* = LASTDRIVE */
#define NET_OPEN_MAX   16          /* open files per mounted share */
#define NET_FIND_MAX   256         /* directory entries cached per find */

/* the DOS attribute bits (PMDOS.INC's ATTR_*) */
#define ATTR_READONLY 0x01
#define ATTR_HIDDEN   0x02
#define ATTR_SYSTEM   0x04
#define ATTR_VOLUME   0x08
#define ATTR_DIR      0x10
#define ATTR_ARCHIVE  0x20

/* the DOS error codes a redirector returns (PMDOS.INC's ERR_*) */
#define E_NOFILE   2
#define E_NOPATH   3
#define E_ACCESS   5
#define E_NOHANDLE 6
#define E_BADDRIVE 15
#define E_GENERAL  0x1F

/* a cached directory entry, ready for the DTA */
typedef struct {
    char          disp[13];        /* 8.3 ASCIIZ */
    unsigned char fcb[11];         /* FCB form, for the wildcard match */
    unsigned char attr;            /* DOS attribute byte */
    u32           size;
    u32           td;              /* (date << 16) | time, DOS format */
} ncent;

typedef struct {
    int           sock;            /* the TCP socket */
    int           proto;           /* 1 = SMB1 (NBT/139), 2 = SMB2/3 (445) */
    u32           nbh;             /* or a NetBEUI session (SMB1 then) */
    unsigned char nb_pend[4];      /* NetBIOS-over-TCP header bytes still to give */
    int           nb_pendn, nb_pendoff;
    u32           nb_msgleft;      /* bytes of the message under it still to give */
    union {                        /* one protocol engine, per the mount */
        smb2_client c2;
        smb1_client c1;
    } u;
    struct {
        int       used;
        smb2_fh   fh;
    } open[NET_OPEN_MAX];
    /* the last directory listing, for FINDFIRST/FINDNEXT */
    u32           gen;
    int           count;
    unsigned char fpat[11];        /* the find pattern, FCB form */
    unsigned char fsrch;           /* the search attributes */
    ncent         ent[NET_FIND_MAX];
} session;

static session *g_sess[NET_DRIVES];
static u32      g_gen = 1;

/* =================================================================== */
/* small helpers                                                       */
/* =================================================================== */

static char upper( char ch )
{
    return ( ch >= 'a' && ch <= 'z' ) ? (char)( ch - 32 ) : ch;
}

static int slen( const char *s )
{
    int n = 0;
    while ( s[n] ) n++;
    return n;
}

static void scopy( char *to, const char *from, int max )
{
    int i;
    for ( i = 0; i + 1 < max && from[i]; i++ ) to[i] = from[i];
    to[i] = 0;
}

/* =================================================================== */
/* a Windows FILETIME (100ns since 1601) -> a DOS (date<<16)|time       */
/* =================================================================== */

/* (hi:lo) / d -> (qhi:qlo), remainder in *rem.  Restoring division, so
   no 64-bit type is needed (the codebase is deliberately __int64-free). */
static void div64( u32 hi, u32 lo, u32 d, u32 *qhi, u32 *qlo, u32 *rem )
{
    u32 remder = 0, qh = 0, ql = 0;
    int i;
    for ( i = 0; i < 64; i++ ) {
        u32 bit = ( hi >> 31 ) & 1;
        remder = ( remder << 1 ) | bit;
        hi = ( hi << 1 ) | ( lo >> 31 );
        lo <<= 1;
        qh = ( qh << 1 ) | ( ql >> 31 );
        ql <<= 1;
        if ( remder >= d ) { remder -= d; ql |= 1; }
    }
    *qhi = qh; *qlo = ql; *rem = remder;
}

/* days since 1970-01-01 (may be negative) -> civil year/month/day
   (Howard Hinnant's algorithm) */
static void civil( long z, int *yy, int *mm, int *dd )
{
    long era, doe, yoe, y, doy, mp, d, m;
    z += 719468;
    era = ( z >= 0 ? z : z - 146096 ) / 146097;
    doe = z - era * 146097;
    yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
    y   = yoe + era * 400;
    doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
    mp  = ( 5 * doy + 2 ) / 153;
    d   = doy - ( 153 * mp + 2 ) / 5 + 1;
    m   = mp < 10 ? mp + 3 : mp - 9;
    *yy = (int)( y + ( m <= 2 ) );
    *mm = (int)m;
    *dd = (int)d;
}

static u32 filetime_to_dos( u32 lo, u32 hi )
{
    u32 shi, slo, rem, dhi, dlo, sod;
    long days;
    int year, month, day, hour, minute, second;
    u32 dosdate, dostime;

    if ( lo == 0 && hi == 0 ) return ( 0x0021UL << 16 );   /* 1980-01-01 */

    div64( hi, lo, 10000000UL, &shi, &slo, &rem );         /* seconds since 1601 */
    div64( shi, slo, 86400UL, &dhi, &dlo, &sod );          /* days + sec-of-day */
    days = (long)dlo - 134774L;                            /* -> days since 1970 */

    civil( days, &year, &month, &day );
    if ( year < 1980 ) { year = 1980; month = 1; day = 1; sod = 0; }
    if ( year > 2107 ) { year = 2107; month = 12; day = 31; sod = 86399; }

    hour   = (int)( sod / 3600 );
    minute = (int)( ( sod % 3600 ) / 60 );
    second = (int)( sod % 60 );

    dosdate = ( (u32)( year - 1980 ) << 9 ) | ( (u32)month << 5 ) | (u32)day;
    dostime = ( (u32)hour << 11 ) | ( (u32)minute << 5 ) | ( (u32)( second / 2 ) );
    return ( dosdate << 16 ) | dostime;
}

/* =================================================================== */
/* path canonicalisation and the DOS 8.3 wildcard match                 */
/* =================================================================== */

/* fold "." and "..", turn '/' into '\', drop a leading slash: a
   share-relative path with no leading backslash.  'dst' holds 'dmax'
   bytes; -1 (and an empty 'dst') when the path does not fit. */
static int canon( const char *src, char *dst, int dmax )
{
    const char *p = src;
    int out = 0;

    while ( *p == '\\' || *p == '/' ) p++;
    while ( *p ) {
        char comp[260];
        int clen = 0;
        while ( *p && *p != '\\' && *p != '/' ) {
            if ( clen < 259 ) comp[clen++] = *p;
            p++;
        }
        comp[clen] = 0;
        while ( *p == '\\' || *p == '/' ) p++;
        if ( clen == 0 ) continue;
        if ( comp[0] == '.' && comp[1] == 0 ) continue;
        if ( comp[0] == '.' && comp[1] == '.' && comp[2] == 0 ) {
            int i = out;
            while ( i > 0 && dst[i - 1] != '\\' ) i--;
            if ( i > 0 ) i--;
            out = i;
            continue;
        }
        if ( out + ( out > 0 ? 1 : 0 ) + clen >= dmax ) { dst[0] = 0; return -1; }
        if ( out > 0 ) dst[out++] = '\\';
        { int i; for ( i = 0; i < clen; i++ ) dst[out++] = comp[i]; }
    }
    dst[out] = 0;
    return 0;
}

/* split a find path (drive already stripped) into its directory and its
   last component (the wildcard).  'dir' (260 bytes) comes back canonical
   and share-relative; 'pat' is the raw last component. */
static void split_find( const char *path, char *dir, char *pat )
{
    int len = slen( path ), cut = -1, i;
    for ( i = 0; i < len; i++ )
        if ( path[i] == '\\' || path[i] == '/' ) cut = i;
    if ( cut < 0 ) {
        dir[0] = 0;
        scopy( pat, path, 16 );
    } else {
        char tmp[260];
        for ( i = 0; i < cut && i < 259; i++ ) tmp[i] = path[i];
        tmp[i] = 0;
        canon( tmp, dir, 260 );                            /* tmp is shorter: it fits */
        scopy( pat, path + cut + 1, 16 );
    }
    if ( pat[0] == 0 ) { pat[0] = '*'; pat[1] = 0; }       /* "DIR\" -> "*" */
}

/* an ASCIIZ name -> its 8.3 display form and its 11-byte FCB form */
static void to83( const char *name, char *disp, unsigned char *fcb )
{
    int i, pos, dp = 0;
    const char *dot, *p;

    for ( i = 0; i < 11; i++ ) fcb[i] = ' ';

    if ( name[0] == '.' && name[1] == 0 ) {
        fcb[0] = '.';
        disp[0] = '.'; disp[1] = 0;
        return;
    }
    if ( name[0] == '.' && name[1] == '.' && name[2] == 0 ) {
        fcb[0] = '.'; fcb[1] = '.';
        disp[0] = '.'; disp[1] = '.'; disp[2] = 0;
        return;
    }

    /* the last '.' splits name from extension */
    dot = (const char *)0;
    for ( p = name; *p; p++ ) if ( *p == '.' ) dot = p;

    pos = 0;
    for ( p = name; *p && pos < 8; p++ ) {
        if ( dot && p == dot ) break;
        fcb[pos] = (unsigned char)upper( *p );
        disp[dp++] = upper( *p );
        pos++;
    }
    if ( dot ) {
        disp[dp++] = '.';
        pos = 8;
        for ( p = dot + 1; *p && pos < 11; p++ ) {
            fcb[pos] = (unsigned char)upper( *p );
            disp[dp++] = upper( *p );
            pos++;
        }
        if ( disp[dp - 1] == '.' ) dp--;                   /* a trailing dot */
    }
    disp[dp] = 0;
}

/* a DOS wildcard pattern -> its 11-byte FCB form ('*' fills with '?') */
static void pat83( const char *pat, unsigned char *fcb )
{
    int i, pos;
    const char *p = pat;

    for ( i = 0; i < 11; i++ ) fcb[i] = ' ';
    pos = 0;
    while ( *p && *p != '.' ) {
        if ( *p == '*' ) { while ( pos < 8 ) fcb[pos++] = '?'; while ( *p && *p != '.' ) p++; break; }
        if ( pos < 8 ) fcb[pos++] = ( *p == '?' ) ? '?' : (unsigned char)upper( *p );
        p++;
    }
    if ( *p == '.' ) p++;
    pos = 8;
    while ( *p ) {
        if ( *p == '*' ) { while ( pos < 11 ) fcb[pos++] = '?'; break; }
        if ( pos < 11 ) fcb[pos++] = ( *p == '?' ) ? '?' : (unsigned char)upper( *p );
        p++;
    }
}

static int fcb_match( const unsigned char *pat, const unsigned char *name )
{
    int i;
    for ( i = 0; i < 11; i++ )
        if ( pat[i] != '?' && pat[i] != name[i] ) return 0;
    return 1;
}

/* the classic DOS directory-search attribute rule (fat_search's) */
static int attr_match( unsigned char ent, unsigned char srch )
{
    if ( srch & ATTR_VOLUME )
        return ( ent & ( ATTR_VOLUME | ATTR_DIR ) ) == ATTR_VOLUME;
    if ( ent & ATTR_VOLUME ) return 0;
    if ( srch & 0x80 ) {                                   /* SRCH_FILTER */
        unsigned char hs = ent & ( ATTR_HIDDEN | ATTR_SYSTEM );
        if ( hs & ~srch ) return 0;
    }
    if ( ent & ATTR_DIR ) {
        if ( !( srch & ATTR_DIR ) ) return 0;
    }
    return 1;
}

/* =================================================================== */
/* the SMB2 transport callbacks                                         */
/* =================================================================== */

static int drv_smb_send( void *ctx, const unsigned char *buf, int len )
{
    return dsock_send( (int)ctx, buf, (u32)len, 5000 );
}
static int drv_smb_recv( void *ctx, unsigned char *buf, int len )
{
    return dsock_recvwait( (int)ctx, buf, (u32)len, 6000 );
}

/* The same callbacks over NetBEUI.  The SMB1 engine speaks NetBIOS over
   TCP: a 4-byte header (type, length) on every message, and a SESSION
   REQUEST first.  Over NetBEUI the session is NetBIOS's own and messages
   keep their bounds, so the header is taken off on the way out and made
   from the message's length on the way in, and the SESSION REQUEST is
   answered here with the positive response it would have had. */
static int nb_smb_send( void *ctx, const unsigned char *buf, int len )
{
    session *s = (session *)ctx;
    u32 plen;

    if ( len < 4 ) return -1;
    if ( buf[0] == 0x81 ) {             /* SESSION REQUEST: already in session */
        s->nb_pend[0] = 0x82;
        s->nb_pend[1] = s->nb_pend[2] = s->nb_pend[3] = 0;
        s->nb_pendn = 4;
        s->nb_pendoff = 0;
        return len;
    }
    if ( buf[0] != 0x00 ) return len;   /* a keepalive: NetBEUI has its own */
    plen = ( (u32)( buf[1] & 1 ) << 16 ) | ( (u32)buf[2] << 8 ) | buf[3];
    if ( (u32)len != 4 + plen ) return -1;      /* the engine sends whole messages */
    return dnb_send( s->nbh, buf + 4, plen, 15000 ) ? len : -1;
}
static int nb_smb_recv( void *ctx, unsigned char *buf, int len )
{
    session *s = (session *)ctx;
    u32 n;

    if ( len <= 0 ) return 0;
    if ( s->nb_pendoff == s->nb_pendn && s->nb_msgleft == 0 ) {
        n = dnb_wait( s->nbh, 6000 );   /* the next message: make its header */
        if ( n == 0 ) return 0;
        s->nb_pend[0] = 0x00;
        s->nb_pend[1] = (unsigned char)( ( n >> 16 ) & 1 );
        s->nb_pend[2] = (unsigned char)( n >> 8 );
        s->nb_pend[3] = (unsigned char)n;
        s->nb_pendn = 4;
        s->nb_pendoff = 0;
        s->nb_msgleft = n;
    }
    if ( s->nb_pendoff < s->nb_pendn ) {
        n = (u32)( s->nb_pendn - s->nb_pendoff );
        if ( n > (u32)len ) n = (u32)len;
        memcpy( buf, s->nb_pend + s->nb_pendoff, n );
        s->nb_pendoff += (int)n;
        return (int)n;
    }
    n = s->nb_msgleft < (u32)len ? s->nb_msgleft : (u32)len;
    n = dnb_recv( s->nbh, buf, n );
    s->nb_msgleft -= n;
    return (int)n;
}

/* =================================================================== */
/* sessions - one code path over the two engines, chosen by s->proto    */
/* =================================================================== */

static int sess_list_dir( session *s, const char *dir, const char *pat,
                          void ( *emit )( void *, const smb2_dirent * ), void *user )
{
    if ( s->proto == 1 ) return smb1_list_dir( &s->u.c1, dir, pat, emit, user );
    return smb2_list_dir( &s->u.c2, dir, pat, emit, user );
}
static int sess_open_disp( session *s, const char *path, int write, unsigned long disp, smb2_fh *fh )
{
    if ( s->proto == 1 ) return smb1_open_disp( &s->u.c1, path, write, disp, fh );
    return smb2_open_disp( &s->u.c2, path, write, disp, fh );
}
static int sess_read( session *s, const smb2_fh *fh, u32 lo, u32 hi, unsigned char *buf, int len )
{
    if ( s->proto == 1 ) return smb1_read( &s->u.c1, fh, lo, hi, buf, len );
    return smb2_read( &s->u.c2, fh, lo, hi, buf, len );
}
static int sess_write( session *s, const smb2_fh *fh, u32 lo, u32 hi, const unsigned char *buf, int len )
{
    if ( s->proto == 1 ) return smb1_write( &s->u.c1, fh, lo, hi, buf, len );
    return smb2_write( &s->u.c2, fh, lo, hi, buf, len );
}
static void sess_close( session *s, const smb2_fh *fh )
{
    if ( s->proto == 1 ) smb1_close_file( &s->u.c1, fh );
    else                 smb2_close_file( &s->u.c2, fh );
}
static int sess_delete( session *s, const char *path )
{ return ( s->proto == 1 ) ? smb1_delete( &s->u.c1, path ) : smb2_delete( &s->u.c2, path ); }
static int sess_mkdir( session *s, const char *path )
{ return ( s->proto == 1 ) ? smb1_mkdir( &s->u.c1, path ) : smb2_mkdir( &s->u.c2, path ); }
static int sess_rmdir( session *s, const char *path )
{ return ( s->proto == 1 ) ? smb1_rmdir( &s->u.c1, path ) : smb2_rmdir( &s->u.c2, path ); }
static int sess_rename( session *s, const char *oldp, const char *newp )
{ return ( s->proto == 1 ) ? smb1_rename( &s->u.c1, oldp, newp ) : smb2_rename( &s->u.c2, oldp, newp ); }
static void sess_disconnect( session *s )
{ if ( s->proto == 1 ) smb1_disconnect( &s->u.c1 ); else smb2_disconnect( &s->u.c2 ); }

static void drop_session( int drive )
{
    session *s = g_sess[drive];
    if ( s == (session *)0 ) return;
    sess_disconnect( s );
    if ( s->sock ) dsock_close( s->sock );
    if ( s->nbh ) dnb_hangup( s->nbh );
    nds_free( s );
    g_sess[drive] = (session *)0;
}

/* the server is not found over TCP/IP (or there is none): NetBEUI, if
   NETBEUI.SYS is loaded, with SMB1 - the only SMB that runs over it */
static int mount_netbeui( redir_req *r, int drive )
{
    session *s;
    int i, rc;
    u32 seed;

    for ( i = 0; r->server[i]; i++ ) if ( r->server[i] == '.' || i >= 15 ) break;
    if ( r->server[i] || i == 0 || !dnb_here( smb1_calling ) ) {     /* our NetBEUI name calls */
        r->status = E_NOFILE;           /* unknown server */
        return 0;
    }
    s = (session *)nds_alloc( sizeof( session ) );
    if ( s == (session *)0 ) { r->status = E_GENERAL; return 0; }
    memset( s, 0, sizeof( *s ) );
    s->proto = 1;
    s->nbh = dnb_call( r->server, 0x20, 8000 );         /* the server service */
    if ( s->nbh == 0 ) { nds_free( s ); r->status = E_NOFILE; return 0; }
    seed = nds_ticks() * 2654435761UL + (u32)drive;
    rc = smb1_connect( &s->u.c1, nb_smb_send, nb_smb_recv, (void *)s, seed,
                       r->server, r->share, r->user, r->pass, r->domain );
    if ( rc != 0 ) {
        dnb_hangup( s->nbh );
        nds_free( s );
        r->status = E_ACCESS;           /* auth or share refused */
        return 0;
    }
    g_sess[drive] = s;
    r->status = 0;
    return 0;
}

static int do_mount( redir_req *r )
{
    int drive = (int)r->drive;
    session *s;
    u32 ip;

    if ( drive < 0 || drive >= NET_DRIVES ) { r->status = E_BADDRIVE; return 0; }
    if ( g_sess[drive] ) drop_session( drive );

    /* TCP/IP first, when it is loaded and has an address: the name by
       NetBIOS (WINS, broadcast), then DNS */
    ip = 0;
    if ( dsock_up( 3000 ) ) ip = dsock_resolve( r->server, 8000 );
    if ( ip == 0 ) return mount_netbeui( r, drive );

    s = (session *)nds_alloc( sizeof( session ) );
    if ( s == (session *)0 ) { r->status = E_GENERAL; return 0; }
    memset( s, 0, sizeof( *s ) );
    s->proto = ( r->spare == 1 ) ? 1 : 2;       /* NR_SPARE from NET USE /SMB1 */

    s->sock = dsock_open( SOCK_STREAM );
    if ( s->sock == 0 ) { nds_free( s ); r->status = E_GENERAL; return 0; }
    if ( !dsock_connect( s->sock, ip, (u16)( ( s->proto == 1 ) ? 139 : 445 ), 5000 ) ) {
        /* nothing on 445: a server that speaks only SMB1 (Windows for
           Workgroups, NT 3.x, SMBSRV) is tried on 139, as with /SMB1 */
        dsock_close( s->sock );
        s->sock = 0;
        if ( s->proto == 2 ) {
            s->proto = 1;
            s->sock = dsock_open( SOCK_STREAM );
            if ( s->sock && !dsock_connect( s->sock, ip, 139, 5000 ) ) { dsock_close( s->sock ); s->sock = 0; }
        }
        if ( s->sock == 0 ) { nds_free( s ); r->status = E_GENERAL; return 0; }
    }
    {
        u32 seed = nds_ticks() * 2654435761UL + (u32)drive;
        int rc;
        if ( s->proto == 1 ) {
            dsock_nbname( smb1_calling );       /* introduce ourselves by /NAME= */
            rc = smb1_connect( &s->u.c1, drv_smb_send, drv_smb_recv, (void *)s->sock, seed,
                               r->server, r->share, r->user, r->pass, r->domain );
        } else
            rc = smb2_connect( &s->u.c2, drv_smb_send, drv_smb_recv, (void *)s->sock, seed,
                               r->server, r->share, r->user, r->pass, r->domain );
        if ( rc != 0 ) {
            dsock_close( s->sock ); nds_free( s );
            r->status = E_ACCESS;               /* auth or share refused */
            return 0;
        }
    }
    g_sess[drive] = s;
    r->status = 0;
    return 0;
}

static int do_umount( redir_req *r )
{
    int drive = (int)r->drive;
    if ( drive >= 0 && drive < NET_DRIVES ) drop_session( drive );
    r->status = 0;
    return 0;
}

/* =================================================================== */
/* find                                                                 */
/* =================================================================== */

static void collect( void *user, const smb2_dirent *e )
{
    session *s = (session *)user;
    ncent *c;
    if ( s->count >= NET_FIND_MAX ) return;
    c = &s->ent[s->count];
    to83( e->name, c->disp, c->fcb );
    c->attr = (unsigned char)( e->attr & 0x37 );           /* RO|HID|SYS|DIR|ARC */
    if ( c->attr == 0 ) c->attr = ATTR_ARCHIVE;            /* a plain file */
    c->size = e->size_lo;
    c->td = filetime_to_dos( e->wtime_lo, e->wtime_hi );
    s->count++;
}

static int find_from( redir_req *r, session *s, int start )
{
    int i;
    for ( i = start; i < s->count; i++ ) {
        ncent *c = &s->ent[i];
        if ( !fcb_match( s->fpat, c->fcb ) ) continue;
        if ( !attr_match( c->attr, s->fsrch ) ) continue;
        scopy( r->name, c->disp, 16 );
        r->attr = c->attr;
        r->size_lo = c->size;
        r->size_hi = 0;
        r->time_date = c->td;
        r->gen = s->gen;
        r->count = (u32)( i + 1 );             /* where FINDNEXT resumes */
        r->status = 0;
        return 0;
    }
    r->status = E_NOFILE;
    return 0;
}

static int do_findfirst( redir_req *r )
{
    int drive = (int)r->drive;
    session *s;
    char dir[260], pat[16];

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_BADDRIVE; return 0;
    }
    s = g_sess[drive];
    split_find( r->path, dir, pat );

    s->count = 0;
    if ( sess_list_dir( s, dir, "*", collect, s ) != 0 && s->count == 0 ) {
        r->status = E_NOPATH;                  /* the directory is not there */
        return 0;
    }
    s->gen = ++g_gen;
    pat83( pat, s->fpat );
    s->fsrch = (unsigned char)r->attr;
    return find_from( r, s, 0 );
}

static int do_findnext( redir_req *r )
{
    int drive = (int)r->drive;
    session *s;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_NOFILE; return 0;
    }
    s = g_sess[drive];
    if ( r->gen != s->gen ) { r->status = E_NOFILE; return 0; }   /* a new find ran */
    if ( r->count > (u32)s->count ) { r->status = E_NOFILE; return 0; }   /* past the end */
    return find_from( r, s, (int)r->count );
}

/* =================================================================== */
/* open / read / write / close                                          */
/* =================================================================== */

static int do_open( redir_req *r )
{
    int drive = (int)r->drive;
    session *s;
    char path[260];
    smb2_fh fh;
    int write, slot;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_GENERAL; return 0;
    }
    s = g_sess[drive];
    if ( canon( r->path, path, (int)sizeof( path ) ) != 0 ) { r->status = E_NOPATH; return 0; }
    write = ( r->count == 1 || r->count == 2 );            /* access 1 write, 2 both */

    for ( slot = 0; slot < NET_OPEN_MAX; slot++ )
        if ( !s->open[slot].used ) break;
    if ( slot == NET_OPEN_MAX ) { r->status = E_NOHANDLE; return 0; }

    /* r->spare picks the disposition: 0 open an existing file (3Dh), 1
       create or truncate (3Ch create), 2 open or create (6Ch) */
    {
        unsigned long disp = ( r->spare == 1 ) ? 0x5UL : ( r->spare == 2 ) ? 0x3UL : 0x1UL;
        if ( sess_open_disp( s, path, write, disp, &fh ) != 0 ) {
            r->status = E_NOFILE;
            return 0;
        }
    }
    s->open[slot].used = 1;
    s->open[slot].fh = fh;
    r->handle = (u32)( slot + 1 );
    r->size_lo = fh.size_lo;
    r->size_hi = fh.size_hi;
    r->attr = (unsigned char)( fh.attr & 0x37 );
    r->status = 0;
    return 0;
}

static int do_read( redir_req *r )
{
    int drive = (int)r->drive, slot = (int)r->handle - 1;
    session *s;
    smb2_fh *fh;
    unsigned char *dst = (unsigned char *)r->bufptr;
    u32 off_lo = r->pos_lo, off_hi = r->pos_hi, want = r->count, done = 0;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) { r->status = E_ACCESS; return 0; }
    s = g_sess[drive];
    if ( slot < 0 || slot >= NET_OPEN_MAX || !s->open[slot].used ) { r->status = E_NOHANDLE; return 0; }
    fh = &s->open[slot].fh;

    while ( done < want ) {
        u32 chunk = want - done;
        int n;
        u32 nlo;
        if ( chunk > 0x10000UL ) chunk = 0x10000UL;          /* both engines take less */
        n = sess_read( s, fh, off_lo, off_hi, dst + done, (int)chunk );
        if ( n < 0 ) { r->status = E_ACCESS; return 0; }
        if ( n == 0 ) break;                               /* end of file */
        done += (u32)n;
        nlo = off_lo + (u32)n;
        if ( nlo < off_lo ) off_hi++;
        off_lo = nlo;
    }
    r->result = done;
    r->status = 0;
    return 0;
}

static int do_write( redir_req *r )
{
    int drive = (int)r->drive, slot = (int)r->handle - 1;
    session *s;
    smb2_fh *fh;
    const unsigned char *src = (const unsigned char *)r->bufptr;
    u32 off_lo = r->pos_lo, off_hi = r->pos_hi, want = r->count, done = 0;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) { r->status = E_ACCESS; return 0; }
    s = g_sess[drive];
    if ( slot < 0 || slot >= NET_OPEN_MAX || !s->open[slot].used ) { r->status = E_NOHANDLE; return 0; }
    fh = &s->open[slot].fh;

    if ( want == 0 ) { r->result = 0; r->status = 0; return 0; }   /* truncate: not yet */

    while ( done < want ) {
        u32 chunk = want - done;
        int n;
        u32 nlo;
        if ( chunk > 0x10000UL ) chunk = 0x10000UL;          /* both engines take less */
        n = sess_write( s, fh, off_lo, off_hi, src + done, (int)chunk );
        if ( n <= 0 ) { r->status = E_ACCESS; return 0; }
        if ( (u32)n > chunk ) n = (int)chunk;
        done += (u32)n;
        nlo = off_lo + (u32)n;
        if ( nlo < off_lo ) off_hi++;
        off_lo = nlo;
    }
    r->result = done;
    r->status = 0;
    return 0;
}

static int do_pathop( redir_req *r, int op )
{
    int drive = (int)r->drive;
    session *s;
    char path[260];
    int rc;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_GENERAL; return 0;
    }
    s = g_sess[drive];
    if ( canon( r->path, path, (int)sizeof( path ) ) != 0 ) { r->status = E_NOPATH; return 0; }
    switch ( op ) {
    case REDIR_DELETE: rc = sess_delete( s, path ); break;
    case REDIR_MKDIR:  rc = sess_mkdir( s, path );  break;
    case REDIR_RMDIR:  rc = sess_rmdir( s, path );  break;
    default:           rc = -1; break;
    }
    r->status = ( rc == 0 ) ? 0 : E_ACCESS;
    return 0;
}

/* RENAME: the old name is in path, the new name in share (REDIR.H says so
   - both are DOS paths and a rename never needs the MOUNT credentials). */
static int do_rename( redir_req *r )
{
    int drive = (int)r->drive;
    session *s;
    char oldp[260], newp[260];
    int rc;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_GENERAL; return 0;
    }
    s = g_sess[drive];
    if ( canon( r->path, oldp, (int)sizeof( oldp ) ) != 0 ||
         canon( r->share, newp, (int)sizeof( newp ) ) != 0 ) { r->status = E_NOPATH; return 0; }
    rc = sess_rename( s, oldp, newp );
    r->status = ( rc == 0 ) ? 0 : E_ACCESS;
    return 0;
}

static int do_close( redir_req *r )
{
    int drive = (int)r->drive, slot = (int)r->handle - 1;
    session *s;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) { r->status = 0; return 0; }
    s = g_sess[drive];
    if ( slot >= 0 && slot < NET_OPEN_MAX && s->open[slot].used ) {
        sess_close( s, &s->open[slot].fh );
        s->open[slot].used = 0;
    }
    r->status = 0;
    return 0;
}

/* =================================================================== */
/* the driver entries                                                   */
/* =================================================================== */

int __cdecl nd_init( const char *line )
{
    int i;
    (void)line;
    for ( i = 0; i < NET_DRIVES; i++ ) g_sess[i] = (session *)0;
    nds_print( "SMBFS: SMB2/3 + SMB1 network redirector ready.\r\n" );
    return 0;
}

void __cdecl nd_park( int back )
{
    /* The stack and the sockets are real mode's while parked; a mapped
       share's session cannot survive it yet.  On the way back, drop the
       sessions so the next use re-mounts.  (Reconnect-in-place is the
       next step - the credentials would have to be kept to do it.) */
    int i;
    if ( back ) {
        for ( i = 0; i < NET_DRIVES; i++ )
            if ( g_sess[i] ) { nds_free( g_sess[i] ); g_sess[i] = (session *)0; }
    }
}

void __cdecl nd_halt( void )
{
}

int __cdecl nd_irq( u32 line )
{
    (void)line;
    return 0;
}

void __cdecl nd_tick( void )
{
}

static int redir_dispatch( u32 op, redir_req *r )
{
    /* every string the caller filled ends inside its field */
    r->path[sizeof( r->path ) - 1] = 0;
    r->pattern[sizeof( r->pattern ) - 1] = 0;
    r->server[sizeof( r->server ) - 1] = 0;
    r->share[sizeof( r->share ) - 1] = 0;
    r->user[sizeof( r->user ) - 1] = 0;
    r->pass[sizeof( r->pass ) - 1] = 0;
    r->domain[sizeof( r->domain ) - 1] = 0;

    switch ( op ) {
    case REDIR_MOUNT:     return do_mount( r );
    case REDIR_UMOUNT:    return do_umount( r );
    case REDIR_FINDFIRST: return do_findfirst( r );
    case REDIR_FINDNEXT:  return do_findnext( r );
    case REDIR_OPEN:      return do_open( r );
    case REDIR_READ:      return do_read( r );
    case REDIR_WRITE:     return do_write( r );
    case REDIR_CLOSE:     return do_close( r );
    case REDIR_DELETE:
    case REDIR_MKDIR:
    case REDIR_RMDIR:     return do_pathop( r, (int)op );
    case REDIR_RENAME:    return do_rename( r );
    default:              return ERR_FUNC;
    }
}

/* =================================================================== */
/* the resumable path - one op at a time on the worker fibre            */
/*                                                                      */
/* The native kernel calls nd_call with a plain op and it runs to the   */
/* end here, as it always has.  The WfW bridge cannot wait in a window, */
/* so it ORs REDIR_RESUME into the op and polls: the first call copies   */
/* the request aside and starts the op on the worker fibre; the fibre    */
/* runs until a socket wait has no data, where dsock_idle() yields it;   */
/* nd_call answers REDIR_PENDING, the bridge lets Windows run (the tick  */
/* and the card interrupt advance the parked stack), and the next call   */
/* resumes the fibre.  When the op finishes, its result is copied back.  */
/* =================================================================== */
#define WS_IDLE 0
#define WS_RUN  1

static int      g_on_fiber;                /* dsock_idle() yields when set */
static int      g_wstate = WS_IDLE;
static int      g_wdone;                   /* the op finished this slice */
static int      g_wlive;                   /* the fibre has been bootstrapped */
static int      g_wrc;                      /* redir_dispatch's return, for nd_call */
static redir_req g_wreq;                   /* the op's own copy, stable across slices */
static u32      g_wstack[4096];            /* 16 KB worker stack (image carries it) */

void dsock_idle( void )
{
    if ( g_on_fiber ) fib_yield();
}

static void worker_main( void )
{
    for ( ;; ) {
        g_wrc = redir_dispatch( g_wreq.op, &g_wreq );
        g_wdone = 1;
        fib_yield();                       /* back to nd_call; it resets for the next op */
    }
}

int __cdecl nd_call( u32 fn, void *buf, u32 len, u32 *answer )
{
    redir_req *r = (redir_req *)buf;
    *answer = 0;
    if ( len < sizeof( redir_req ) ) return ERR_BADLEN;

    if ( !( fn & REDIR_RESUME ) )
        return redir_dispatch( fn, r );     /* native: run to completion */

    if ( g_wstate == WS_IDLE ) {            /* the first slice of a new op */
        memcpy( &g_wreq, r, sizeof( g_wreq ) );
        g_wreq.op = fn & ~(u32)REDIR_RESUME;
        g_wstate  = WS_RUN;
        g_wdone   = 0;
        g_on_fiber = 1;
        if ( !g_wlive ) {
            fib_init( (u32)( g_wstack + sizeof( g_wstack ) / sizeof( g_wstack[0] ) ),
                      worker_main );
            g_wlive = 1;
        }
    }
    fib_enter();                            /* run until the worker yields */
    if ( g_wdone ) {
        memcpy( r, &g_wreq, sizeof( g_wreq ) );   /* bring the answer back */
        g_wstate = WS_IDLE;
        g_on_fiber = 0;
        return g_wrc;
    }
    r->status = REDIR_PENDING;              /* not finished; the bridge polls again */
    return 0;
}

/* =================================================================== */
/* the driver table - NDSTART.ASM names it                              */
/* =================================================================== */

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_REDIR,
    { 'S', 'M', 'B', 'F', 'S', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
