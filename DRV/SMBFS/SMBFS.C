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
#include "pwl.h"

void *memcpy( void *to, const void *from, unsigned len );
void *memset( void *to, int value, unsigned len );

/* the worker fibre (DRV\SMBFS\DRVFIB.ASM) - how a resumable op gives the
   slice back to the bridge's poll and resumes when called again */
void __cdecl fib_init( uint32_t stacktop, void ( *entry )( void ) );
void __cdecl fib_enter( void );
void __cdecl fib_yield( void );

/* ---- limits ---- */
#define NET_DRIVES     26          /* = LASTDRIVE, A: to Z: */
#define NET_OPEN_MAX   16          /* open files per mounted share */
/* A find keeps the entries that match it, in a table and a pool of long
   names that start at these sizes and double as a directory needs them,
   up to the most: 65,535 entries, because 4Fh's DTA keeps the next index
   in a word.  Back to the start sizes when a listing ends or the next
   one begins, so a big directory's memory is held only while it is
   listed. */
#define NET_FIND_START 256         /* entries */
#define NET_FIND_MAX   65535
#define NET_POOL_START 8192UL      /* bytes of long names, NUL-terminated */
#define NET_POOL_MAX   4194304UL
#define NET_CWD_MAX    256         /* a current directory, with its NUL */

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
#define E_NOMEM    8
#define E_BADDRIVE 15
#define E_GENERAL  0x1F

/* a cached directory entry, ready for the DTA */
typedef struct {
    char          disp[13];        /* 8.3 ASCIIZ */
    uint8_t       fcb[11];         /* FCB form, for the wildcard match */
    uint8_t       attr;            /* DOS attribute byte */
    uint32_t      size;
    uint32_t      td;              /* (date << 16) | time, DOS format */
    unsigned int  lname;           /* the long name's offset in lpool, or
                                      NO_LNAME when the pool was full */
} ncent;
#define NO_LNAME 0xFFFFFFFFUL

/* a connection and logon to a server: shared by every drive mapped to it
   with the same credentials (see "connections" below) */
typedef struct {
    int           refs;            /* the drives on it */
    unsigned int  gen;             /* made again this many times, plus one */
    uint32_t      ip;              /* the server's address; 0 = NetBEUI */
    int           want;            /* 1 SMB1 (NET USE /SMB1), 2 SMB2 first */
    unsigned int  vc;              /* the logon's VcNumber (SMB1) */
    int           sock;            /* the TCP socket */
    int           proto;           /* 1 = SMB1 (NBT/139), 2 = SMB2/3 (445) */
    uint32_t      nbh;             /* or a NetBEUI session (SMB1 then) */
    uint8_t       nb_pend[4];      /* NetBIOS-over-TCP header bytes still to give */
    int           nb_pendn, nb_pendoff;
    unsigned int  nb_msgleft;      /* bytes of the message under it still to give */
    char          server[64];      /* kept to make it again: the server drops */
    char          user[64];        /* an idle connection, and restarts */
    char          pass[64];
    char          domain[64];
    union {                        /* one protocol engine, per the logon */
        smb2_client c2;
        smb1_client c1;
    } u;
} netconn;

typedef struct {
    netconn      *nc;              /* the drive's connection */
    uint32_t      tid;             /* its tree on it */
    unsigned int  tgen;            /* ...connected when nc->gen was this */
    char          share[80];
    struct {
        int       used;
        int       bridged;         /* opened through the bridge, while parked */
        int       write;           /* opened for writing */
        unsigned  gen;             /* fh is from when nc->gen was this */
        smb2_fh   fh;
        char      path[260];       /* to open it again after a reconnect */
    } open[NET_OPEN_MAX];
    /* the last directory listing, for FINDFIRST/FINDNEXT: the entries
       that match its pattern and attributes, in the order they came */
    unsigned int  gen;
    int           count;
    int           fshort;          /* the listing did not all fit: no more
                                      memory, or NET_FIND_MAX matches */
    int           fseen;           /* entries the server sent, kept or not */
    uint8_t       fpat[11];        /* the find pattern, FCB form */
    uint8_t       fsrch;           /* the search attributes */
    int           flong;           /* a long-name (714Eh) find: lpat too */
    char          lpat[260];       /* the find pattern as typed */
    ncent        *ent;             /* entmax entries (nds_alloc) */
    int           entmax;
    char         *lpool;           /* poolmax bytes (nds_alloc) */
    unsigned int  lpool_used, poolmax;
    /* the drive's current directory, share-relative with no leading
       backslash ("" = the root): the server's own spelling, and the
       same path in 8.3 names, as 47h gives it */
    char          cwd[NET_CWD_MAX];
    char          cwd83[NET_CWD_MAX];
    /* walk()'s scratch, here and not on the stack: the kernel's ring-0
       stack is 14K and nested EXECs share it.  One op runs at a time. */
    char          wdir[260], wcomp[260];
    char          wlong[NET_CWD_MAX], wshort[NET_CWD_MAX];
    const char   *fwant;               /* name_emit: the name looked for */
    int           ffound;
    smb2_dirent   fent;                /* ...and what matched it */
} session;

static session *g_sess[NET_DRIVES];
static netconn *g_conn[NET_DRIVES];        /* at most one per drive */
static unsigned g_gen = 1;
static int      g_on_fiber;                /* an op the bridge runs is on the worker
                                              fibre: dsock_idle() yields, and a file
                                              it opens is the bridge's (nd_park) */

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
static void div64( uint32_t hi, uint32_t lo, uint32_t d, uint32_t *qhi, uint32_t *qlo, uint32_t *rem )
{
    uint32_t remder = 0, qh = 0, ql = 0;
    int i;
    for ( i = 0; i < 64; i++ ) {
        uint32_t bit = ( hi >> 31 ) & 1;
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

static uint32_t filetime_to_dos( uint32_t lo, uint32_t hi )
{
    uint32_t shi, slo, rem, dhi, dlo, sod;
    long days;
    int year, month, day, hour, minute, second;
    uint32_t dosdate, dostime;

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

    dosdate = ( (uint32_t)( year - 1980 ) << 9 ) | ( (uint32_t)month << 5 ) | (uint32_t)day;
    dostime = ( (uint32_t)hour << 11 ) | ( (uint32_t)minute << 5 ) | ( (uint32_t)( second / 2 ) );
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

/* split a find path (canonical, share-relative) into its directory and
   its last component (the wildcard).  'dir' and 'pat' hold 260 bytes
   each; 'pat' is the last component as typed. */
static void split_find( const char *path, char *dir, char *pat )
{
    int len = slen( path ), cut = -1, i;
    for ( i = 0; i < len; i++ )
        if ( path[i] == '\\' || path[i] == '/' ) cut = i;
    if ( cut < 0 ) {
        dir[0] = 0;
        scopy( pat, path, 260 );
    } else {
        for ( i = 0; i < cut && i < 259; i++ ) dir[i] = path[i];
        dir[i] = 0;
        scopy( pat, path + cut + 1, 260 );
    }
    if ( pat[0] == 0 ) { pat[0] = '*'; pat[1] = 0; }       /* "DIR\" -> "*" */
}

/* 'raw' (the drive already stripped) as a canonical path from the share's
   root: a relative path is taken from the drive's current directory.
   -1 when it does not fit 'dmax'. */
static int fullpath( const session *s, const char *raw, char *dst, int dmax )
{
    char tmp[NET_CWD_MAX + 262];
    int n = 0, i;

    if ( raw[0] == '\\' || raw[0] == '/' || s->cwd[0] == 0 ) return canon( raw, dst, dmax );
    for ( i = 0; s->cwd[i] && n < NET_CWD_MAX - 1; i++ ) tmp[n++] = s->cwd[i];
    tmp[n++] = '\\';
    for ( i = 0; raw[i] && n < (int)sizeof( tmp ) - 1; i++ ) tmp[n++] = raw[i];
    if ( raw[i] ) { dst[0] = 0; return -1; }
    tmp[n] = 0;
    return canon( tmp, dst, dmax );
}

/* a long name against a wildcard pattern, as Windows matches one: case
   does not matter, '*' is any run of characters and '?' any one, and
   "*.*" is everything (a name need not have a dot) */
static int wild_match( const char *pat, const char *name )
{
    const char *star = (const char *)0, *back = (const char *)0;

    if ( pat[0] == '*' && pat[1] == '.' && pat[2] == '*' && pat[3] == 0 ) return 1;
    while ( *name ) {
        if ( *pat == '*' ) { star = ++pat; back = name; continue; }
        if ( *pat && ( *pat == '?' || upper( *pat ) == upper( *name ) ) ) { pat++; name++; continue; }
        if ( star ) { pat = star; name = ++back; continue; }
        return 0;
    }
    while ( *pat == '*' ) pat++;
    if ( pat[0] == '.' && pat[1] == '*' && pat[2] == 0 ) return 1;   /* "NAME.*" takes "NAME" */
    return *pat == 0;
}

/* is 'name' already an 8.3 name, in either case?  (then it is its own
   short name, upper-cased - the server does not mind which) */
static int is83( const char *name )
{
    int i, base = 0, ext = -1;
    if ( name[0] == '.' ) return name[1] == 0 || ( name[1] == '.' && name[2] == 0 );
    for ( i = 0; name[i]; i++ ) {
        char ch = name[i];
        if ( ch == '.' ) { if ( ext >= 0 ) return 0; ext = 0; continue; }
        if ( ch == ' ' || ch == '+' || ch == ',' || ch == ';' || ch == '=' ||
             ch == '[' || ch == ']' ) return 0;
        if ( ext >= 0 ) { if ( ++ext > 3 ) return 0; }
        else if ( ++base > 8 ) return 0;
    }
    return base > 0 && ext != 0;
}

/* an ASCIIZ name -> its 8.3 display form and its 11-byte FCB form */
static void to83( const char *name, char *disp, uint8_t *fcb )
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
        fcb[pos] = (uint8_t)upper( *p );
        disp[dp++] = upper( *p );
        pos++;
    }
    if ( dot ) {
        disp[dp++] = '.';
        pos = 8;
        for ( p = dot + 1; *p && pos < 11; p++ ) {
            fcb[pos] = (uint8_t)upper( *p );
            disp[dp++] = upper( *p );
            pos++;
        }
        if ( disp[dp - 1] == '.' ) dp--;                   /* a trailing dot */
    }
    disp[dp] = 0;
}

/* a DOS wildcard pattern -> its 11-byte FCB form ('*' fills with '?') */
static void pat83( const char *pat, uint8_t *fcb )
{
    int i, pos;
    const char *p = pat;

    for ( i = 0; i < 11; i++ ) fcb[i] = ' ';
    pos = 0;
    while ( *p && *p != '.' ) {
        if ( *p == '*' ) { while ( pos < 8 ) fcb[pos++] = '?'; while ( *p && *p != '.' ) p++; break; }
        if ( pos < 8 ) fcb[pos++] = ( *p == '?' ) ? '?' : (uint8_t)upper( *p );
        p++;
    }
    if ( *p == '.' ) p++;
    pos = 8;
    while ( *p ) {
        if ( *p == '*' ) { while ( pos < 11 ) fcb[pos++] = '?'; break; }
        if ( pos < 11 ) fcb[pos++] = ( *p == '?' ) ? '?' : (uint8_t)upper( *p );
        p++;
    }
}

static int fcb_match( const uint8_t *pat, const uint8_t *name )
{
    int i;
    for ( i = 0; i < 11; i++ )
        if ( pat[i] != '?' && pat[i] != name[i] ) return 0;
    return 1;
}

/* the classic DOS directory-search attribute rule (fat_search's) */
static int attr_match( uint8_t ent, uint8_t srch )
{
    if ( srch & ATTR_VOLUME )
        return ( ent & ( ATTR_VOLUME | ATTR_DIR ) ) == ATTR_VOLUME;
    if ( ent & ATTR_VOLUME ) return 0;
    if ( srch & 0x80 ) {                                   /* SRCH_FILTER */
        uint8_t hs = ent & ( ATTR_HIDDEN | ATTR_SYSTEM );
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

static int drv_smb_send( void *ctx, const uint8_t *buf, int len )
{
    return dsock_send( (int)ctx, buf, (unsigned int)len, 5000 );
}
static int drv_smb_recv( void *ctx, uint8_t *buf, int len )
{
    return dsock_recvwait( (int)ctx, buf, (unsigned int)len, 6000 );
}

/* The same callbacks over NetBEUI.  The SMB1 engine speaks NetBIOS over
   TCP: a 4-byte header (type, length) on every message, and a SESSION
   REQUEST first.  Over NetBEUI the session is NetBIOS's own and messages
   keep their bounds, so the header is taken off on the way out and made
   from the message's length on the way in, and the SESSION REQUEST is
   answered here with the positive response it would have had. */
static int nb_smb_send( void *ctx, const uint8_t *buf, int len )
{
    netconn *s = (netconn *)ctx;
    unsigned int plen;

    if ( len < 4 ) return -1;
    if ( buf[0] == 0x81 ) {             /* SESSION REQUEST: already in session */
        s->nb_pend[0] = 0x82;
        s->nb_pend[1] = s->nb_pend[2] = s->nb_pend[3] = 0;
        s->nb_pendn = 4;
        s->nb_pendoff = 0;
        return len;
    }
    if ( buf[0] != 0x00 ) return len;   /* a keepalive: NetBEUI has its own */
    plen = ( (unsigned int)( buf[1] & 1 ) << 16 ) | ( (unsigned int)buf[2] << 8 ) | buf[3];
    if ( (unsigned int)len != 4 + plen ) return -1; /* the engine sends whole messages */
    return dnb_send( s->nbh, buf + 4, plen, 15000 ) ? len : -1;
}
static int nb_smb_recv( void *ctx, uint8_t *buf, int len )
{
    netconn *s = (netconn *)ctx;
    unsigned int n;

    if ( len <= 0 ) return 0;
    if ( s->nb_pendoff == s->nb_pendn && s->nb_msgleft == 0 ) {
        n = dnb_wait( s->nbh, 6000 );   /* the next message: make its header */
        if ( n == 0 ) return 0;
        s->nb_pend[0] = 0x00;
        s->nb_pend[1] = (uint8_t)( ( n >> 16 ) & 1 );
        s->nb_pend[2] = (uint8_t)( n >> 8 );
        s->nb_pend[3] = (uint8_t)n;
        s->nb_pendn = 4;
        s->nb_pendoff = 0;
        s->nb_msgleft = n;
    }
    if ( s->nb_pendoff < s->nb_pendn ) {
        n = (unsigned int)( s->nb_pendn - s->nb_pendoff );
        if ( n > (unsigned int)len ) n = (unsigned int)len;
        memcpy( buf, s->nb_pend + s->nb_pendoff, n );
        s->nb_pendoff += (int)n;
        return (int)n;
    }
    n = s->nb_msgleft < (unsigned int)len ? s->nb_msgleft : (unsigned int)len;
    n = dnb_recv( s->nbh, buf, n );
    s->nb_msgleft -= n;
    return (int)n;
}

/* =================================================================== */
/* connections - ONE PER SERVER LOGON, shared by every drive on it      */
/*                                                                      */
/* Each drive used to make its own TCP connection and its own logon.   */
/* A logon says VcNumber 0, which Windows Server 2003 takes to mean the */
/* client has restarted, and it ends the client's other connections:   */
/* the third drive mapped to a server took the first one with it.  So   */
/* drives on one server with the same credentials share a connection,  */
/* each with a tree of its own, as a DOS or Windows client does; a      */
/* second logon (other credentials) says VcNumber 1.  And a connection  */
/* the server has dropped - its idle disconnect, a restart - is made    */
/* again with the credentials kept here, and the request run again      */
/* (conn_revive, redir_dispatch).                                       */
/* =================================================================== */

static int same_text( const char *a, const char *b )
{
    while ( *a && upper( *a ) == upper( *b ) ) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int conn_broken( const netconn *c )
{
    return ( c->proto == 1 ) ? c->u.c1.broken : c->u.c2.broken;
}

static uint32_t conn_status( const netconn *c )
{
    return ( c->proto == 1 ) ? c->u.c1.last_status : smb2_status( &c->u.c2 );
}

/* the connection, or its logon or tree, is gone: the server ended it, or
   said so in the last answer */
static int conn_lost( const netconn *c )
{
    uint32_t st;
    if ( conn_broken( c ) ) return 1;
    st = conn_status( c );
    return st == 0xC000035CUL ||        /* STATUS_NETWORK_SESSION_EXPIRED */
           st == 0xC0000203UL ||        /* STATUS_USER_SESSION_DELETED */
           st == 0xC00000C9UL ||        /* STATUS_NETWORK_NAME_DELETED */
           st == 0x005B0002UL ||        /* SMB1 ERRSRV/ERRbaduid */
           st == 0x00050002UL;          /* SMB1 ERRSRV/ERRinvnid */
}

/* every request this drive makes is on its own tree */
static void sess_tree( session *s )
{
    if ( s->nc->proto == 1 ) s->nc->u.c1.tree_id = s->tid;
    else                     s->nc->u.c2.tree_id = s->tid;
}

static void conn_hangup( netconn *c )
{
    if ( c->sock ) dsock_close( c->sock );
    if ( c->nbh ) dnb_hangup( c->nbh );
    c->sock = 0;
    c->nbh = 0;
}

static int starts( const char *text, const char *head )
{
    while ( *head ) if ( *text++ != *head++ ) return 0;
    return 1;
}

/* the engine's stage when it gave up -> MNT_*.  SMB2 says "negotiate",
   "session-setup(n)", "tree-connect"; SMB1 "negotiate", "session
   request", "session setup", "tree connect". */
static unsigned int mount_step( const char *stage )
{
    if ( starts( stage, "session r" ) ) return MNT_NETBIOS;
    if ( starts( stage, "session" ) )   return MNT_LOGON;
    if ( starts( stage, "tree" ) )      return MNT_SHARE;
    return MNT_DIALECT;
}

/* make c's connection and logon, and connect 'share' on it: TCP to c->ip
   (445 with SMB2 when c->want is 2, else 139), or NetBEUI when c->ip is 0.
   c->proto says which engine answered.  0, or a MNT_* step with *srvstatus
   the server's answer at it. */
static unsigned int conn_open( netconn *c, const char *share, uint32_t *srvstatus )
{
    uint32_t seed;
    int rc = -1, i;

    /* VcNumber 1 while another logon to this server is up, at a reconnect
       as at a mount: 0 would end that one (Windows Server 2003) */
    c->vc = 0;
    for ( i = 0; i < NET_DRIVES; i++ ) {
        netconn *k = g_conn[i];
        if ( k == (netconn *)0 || k == c || k->ip != c->ip ) continue;
        if ( c->ip == 0 && !same_text( k->server, c->server ) ) continue;
        if ( !conn_broken( k ) ) c->vc = 1;
    }
    seed = nds_ticks() * 2654435761UL + (uint32_t)c->vc;
    *srvstatus = 0;
    c->nb_pendn = c->nb_pendoff = 0;
    c->nb_msgleft = 0;
    if ( c->ip == 0 ) {
        /* NetBEUI, with SMB1 - the only SMB that runs over it */
        c->proto = 1;
        c->nbh = dnb_call( c->server, 0x20, 8000 );     /* the server service */
        if ( c->nbh == 0 ) return MNT_NONAME;
        smb1_vc_number = c->vc;
        rc = smb1_connect( &c->u.c1, nb_smb_send, nb_smb_recv, (void *)c, seed,
                           c->server, share, c->user, c->pass, c->domain );
    } else {
        c->proto = c->want;
        c->sock = dsock_open( SOCK_STREAM );
        if ( c->sock == 0 ) return MNT_MEMORY;
        if ( !dsock_connect( c->sock, c->ip, (uint16_t)( ( c->proto == 1 ) ? 139 : 445 ), 5000 ) ) {
            /* nothing on 445: a server that speaks only SMB1 (Windows for
               Workgroups, NT 3.x, SMBSRV) is tried on 139, as with /SMB1 */
            conn_hangup( c );
            if ( c->proto == 2 ) {
                c->proto = 1;
                c->sock = dsock_open( SOCK_STREAM );
                if ( c->sock && !dsock_connect( c->sock, c->ip, 139, 5000 ) ) conn_hangup( c );
            }
            if ( c->sock == 0 ) return MNT_CONNECT;
        }
        if ( c->proto == 2 ) {
            rc = smb2_connect( &c->u.c2, drv_smb_send, drv_smb_recv, (void *)c->sock, seed,
                               c->server, share, c->user, c->pass, c->domain );
            if ( rc != 0 && smb2_smb1_only( &c->u.c2 ) ) {
                /* the server answered in SMB1 (Windows Server 2003, an
                   older NAS): SMB1 on 139, as with /SMB1 */
                conn_hangup( c );
                c->proto = 1;
                c->sock = dsock_open( SOCK_STREAM );
                if ( c->sock && !dsock_connect( c->sock, c->ip, 139, 5000 ) ) conn_hangup( c );
                if ( c->sock == 0 ) return MNT_CONNECT;
            }
        }
        if ( c->proto == 1 ) {
            dsock_nbname( smb1_calling );       /* introduce ourselves by /NAME= */
            smb1_vc_number = c->vc;
            rc = smb1_connect( &c->u.c1, drv_smb_send, drv_smb_recv, (void *)c->sock, seed,
                               c->server, share, c->user, c->pass, c->domain );
        }
    }
    if ( rc != 0 ) {
        unsigned int step;
        if ( c->proto == 1 ) {
            step = mount_step( smb1_stage( &c->u.c1 ) );
            *srvstatus = c->u.c1.last_status;
        } else {
            step = mount_step( smb2_stage( &c->u.c2 ) );
            *srvstatus = smb2_status( &c->u.c2 );
        }
        conn_hangup( c );
        return step;
    }
    return 0;
}

/* the drive's tree on its connection: connect the share again when the
   connection was made afresh since (another drive revived it).  0, or -1. */
static int tree_again( session *s )
{
    netconn *c = s->nc;
    int rc;

    if ( c->proto == 1 ) {
        rc = smb1_tree_connect( &c->u.c1, c->server, s->share, c->pass );
        s->tid = c->u.c1.tree_id;
    } else {
        rc = smb2_tree_connect( &c->u.c2, c->server, s->share );
        s->tid = c->u.c2.tree_id;
    }
    if ( rc != 0 ) return -1;
    s->tgen = c->gen;
    return 0;
}

/* make the drive's connection again: the server ended it.  Every drive on
   it reconnects its tree when it is next used (tgen); open files are
   opened again by name (file_ready).  0, or -1 when the server is still
   not there or will not have us. */
static int conn_revive( session *s )
{
    netconn *c = s->nc;
    uint32_t srvstatus;

    conn_hangup( c );
    if ( conn_open( c, s->share, &srvstatus ) != 0 ) return -1;
    c->gen++;
    s->tid = ( c->proto == 1 ) ? c->u.c1.tree_id : c->u.c2.tree_id;
    s->tgen = c->gen;
    return 0;
}

/* before a request: the connection there, the drive's tree on it, and the
   engine pointed at that tree.  0, or -1 (the request then fails as the
   server's answer says) */
static int sess_ready( session *s )
{
    netconn *c = s->nc;
    if ( conn_broken( c ) && conn_revive( s ) != 0 ) return -1;
    if ( s->tgen != c->gen && tree_again( s ) != 0 ) {
        if ( !conn_lost( c ) || conn_revive( s ) != 0 ) return -1;
    }
    sess_tree( s );
    return 0;
}

/* =================================================================== */
/* sessions - one code path over the two engines, chosen by the conn    */
/* =================================================================== */

static int sess_list_dir( session *s, const char *dir, const char *pat,
                          void ( *emit )( void *, const smb2_dirent * ), void *user )
{
    sess_tree( s );
    if ( s->nc->proto == 1 ) return smb1_list_dir( &s->nc->u.c1, dir, pat, emit, user );
    return smb2_list_dir( &s->nc->u.c2, dir, pat, emit, user );
}
static int sess_open_disp( session *s, const char *path, int write, uint32_t disp, smb2_fh *fh )
{
    sess_tree( s );
    if ( s->nc->proto == 1 ) return smb1_open_disp( &s->nc->u.c1, path, write, disp, fh );
    return smb2_open_disp( &s->nc->u.c2, path, write, disp, fh );
}
static int sess_read( session *s, const smb2_fh *fh, uint32_t lo, uint32_t hi, uint8_t *buf, int len )
{
    sess_tree( s );
    if ( s->nc->proto == 1 ) return smb1_read( &s->nc->u.c1, fh, lo, hi, buf, len );
    return smb2_read( &s->nc->u.c2, fh, lo, hi, buf, len );
}
static int sess_write( session *s, const smb2_fh *fh, uint32_t lo, uint32_t hi, const uint8_t *buf, int len )
{
    sess_tree( s );
    if ( s->nc->proto == 1 ) return smb1_write( &s->nc->u.c1, fh, lo, hi, buf, len );
    return smb2_write( &s->nc->u.c2, fh, lo, hi, buf, len );
}
static void sess_close( session *s, const smb2_fh *fh )
{
    sess_tree( s );
    if ( s->nc->proto == 1 ) smb1_close_file( &s->nc->u.c1, fh );
    else                     smb2_close_file( &s->nc->u.c2, fh );
}
static int sess_delete( session *s, const char *path )
{
    sess_tree( s );
    return ( s->nc->proto == 1 ) ? smb1_delete( &s->nc->u.c1, path ) : smb2_delete( &s->nc->u.c2, path );
}
static int sess_mkdir( session *s, const char *path )
{
    sess_tree( s );
    return ( s->nc->proto == 1 ) ? smb1_mkdir( &s->nc->u.c1, path ) : smb2_mkdir( &s->nc->u.c2, path );
}
static int sess_rmdir( session *s, const char *path )
{
    sess_tree( s );
    return ( s->nc->proto == 1 ) ? smb1_rmdir( &s->nc->u.c1, path ) : smb2_rmdir( &s->nc->u.c2, path );
}
static int sess_rename( session *s, const char *oldp, const char *newp )
{
    sess_tree( s );
    return ( s->nc->proto == 1 ) ? smb1_rename( &s->nc->u.c1, oldp, newp ) : smb2_rename( &s->nc->u.c2, oldp, newp );
}
static int sess_isdir( session *s, const char *path )
{
    sess_tree( s );
    return ( s->nc->proto == 1 ) ? smb1_isdir( &s->nc->u.c1, path ) : smb2_isdir( &s->nc->u.c2, path );
}
static int sess_fs_size( session *s, smb_fs_size *fs )
{
    sess_tree( s );
    return ( s->nc->proto == 1 ) ? smb1_fs_size( &s->nc->u.c1, fs ) : smb2_fs_size( &s->nc->u.c2, fs );
}

/* the connection's slot in g_conn, or -1 */
static int conn_slot( const netconn *c )
{
    int i;
    for ( i = 0; i < NET_DRIVES; i++ ) if ( g_conn[i] == c ) return i;
    return -1;
}

/* =================================================================== */
/* a listing's table and long-name pool                                 */
/* =================================================================== */

static void list_free( session *s )
{
    if ( s->ent ) nds_free( s->ent );
    if ( s->lpool ) nds_free( s->lpool );
    s->ent = (ncent *)0;
    s->lpool = (char *)0;
    s->entmax = 0;
    s->poolmax = 0;
    s->count = 0;
    s->lpool_used = 0;
}

/* the table and the pool at their start sizes, made at the mount: a
   find in a directory they hold asks for no memory.  0 when there is
   none to be had. */
static int list_alloc( session *s )
{
    s->ent = (ncent *)nds_alloc( (uint32_t)NET_FIND_START * sizeof( ncent ) );
    s->lpool = (char *)nds_alloc( NET_POOL_START );
    if ( s->ent == (ncent *)0 || s->lpool == (char *)0 ) { list_free( s ); return 0; }
    s->entmax = NET_FIND_START;
    s->poolmax = NET_POOL_START;
    return 1;
}

/* a session's memory, its listing's with it */
static void sess_free( session *s )
{
    list_free( s );
    nds_free( s );
}

/* the listing emptied, and a table or pool that grew back at its start
   size.  The big block goes first, so the small one has its room; if
   even that is not had, there is none until a find needs one. */
static void list_reset( session *s )
{
    s->count = 0;
    s->lpool_used = 0;
    s->fshort = 0;
    s->fseen = 0;
    if ( s->entmax > NET_FIND_START ) {
        nds_free( s->ent );
        s->ent = (ncent *)nds_alloc( (uint32_t)NET_FIND_START * sizeof( ncent ) );
        s->entmax = s->ent ? NET_FIND_START : 0;
    }
    if ( s->poolmax > NET_POOL_START ) {
        nds_free( s->lpool );
        s->lpool = (char *)nds_alloc( NET_POOL_START );
        s->poolmax = s->lpool ? NET_POOL_START : 0;
    }
}

/* room for one more entry: the table twice as long, the entries copied
   over, up to NET_FIND_MAX.  0 when it is that long already or the
   memory is not there. */
static int ent_grow( session *s )
{
    int want;
    ncent *n;

    if ( s->entmax >= NET_FIND_MAX ) return 0;
    want = ( s->entmax > 0 ) ? s->entmax * 2 : NET_FIND_START;
    if ( want > NET_FIND_MAX ) want = NET_FIND_MAX;
    n = (ncent *)nds_alloc( (uint32_t)want * sizeof( ncent ) );
    if ( n == (ncent *)0 ) return 0;
    if ( s->count > 0 ) memcpy( n, s->ent, (unsigned)s->count * sizeof( ncent ) );
    if ( s->ent ) nds_free( s->ent );
    s->ent = n;
    s->entmax = want;
    return 1;
}

/* room for 'need' more bytes of long names: the pool doubled until they
   fit, what it holds copied over, up to NET_POOL_MAX.  0 when they
   cannot fit. */
static int pool_grow( session *s, unsigned int need )
{
    unsigned int want = ( s->poolmax > 0 ) ? s->poolmax : NET_POOL_START;
    char *n;

    while ( want < s->lpool_used + need && want < NET_POOL_MAX ) want *= 2;
    if ( want > NET_POOL_MAX ) want = NET_POOL_MAX;
    if ( want < s->lpool_used + need || want <= s->poolmax ) return 0;
    n = (char *)nds_alloc( want );
    if ( n == (char *)0 ) return 0;
    if ( s->lpool_used > 0 ) memcpy( n, s->lpool, (unsigned)s->lpool_used );
    if ( s->lpool ) nds_free( s->lpool );
    s->lpool = n;
    s->poolmax = want;
    return 1;
}

/* the drive goes: its tree is disconnected, and its connection with it
   when it was the last drive on it */
static void drop_session( int drive )
{
    session *s = g_sess[drive];
    netconn *c;
    int i;

    if ( s == (session *)0 ) return;
    c = s->nc;
    if ( !conn_broken( c ) && s->tgen == c->gen ) {
        sess_tree( s );
        if ( c->proto == 1 ) smb1_disconnect( &c->u.c1 );    /* the tree alone */
        else                 smb2_tree_disconnect( &c->u.c2 );
    }
    if ( --c->refs == 0 ) {
        if ( c->proto == 2 && !conn_broken( c ) ) smb2_disconnect( &c->u.c2 );   /* the logoff */
        conn_hangup( c );
        i = conn_slot( c );
        if ( i >= 0 ) g_conn[i] = (netconn *)0;
        nds_free( c );
    }
    sess_free( s );
    g_sess[drive] = (session *)0;
}

/* a failed mount: the DOS error, and for NET USE the step (MNT_*) and
   the server's status at it */
static int mount_fail( redir_req *r, unsigned int err, unsigned int step, uint32_t srvstatus )
{
    r->status = err;
    r->count = step;
    r->result = srvstatus;
    return 0;
}

static int same_bytes( const char *a, const char *b )
{
    while ( *a && *a == *b ) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int do_mount_as( redir_req *r );

/* A mount from the password list leaves no copy of the password in the
   kernel's request block, which nobody would clear before the next call:
   the connection keeps the one it needs to reconnect. */
static int do_mount( redir_req *r )
{
    int rc = do_mount_as( r );
    if ( r->spare & 2 ) memset( r->pass, 0, sizeof( r->pass ) );
    return rc;
}

static int do_mount_as( redir_req *r )
{
    int drive = (int)r->drive, i, want;
    session *s;
    netconn *c = (netconn *)0;
    uint32_t ip, srvstatus;
    unsigned int step;

    if ( drive < 0 || drive >= NET_DRIVES ) return mount_fail( r, E_BADDRIVE, 0, 0 );
    if ( g_sess[drive] ) drop_session( drive );

    /* TCP/IP first, when it is loaded and has an address: the name by
       NetBIOS (WINS, broadcast), then DNS.  None: NetBEUI, whose names are
       up to 15 letters with no dot, called by our own NetBEUI name. */
    ip = 0;
    if ( dsock_up( 3000 ) ) ip = dsock_resolve( r->server, 8000 );
    if ( ip == 0 ) {
        for ( i = 0; r->server[i]; i++ ) if ( r->server[i] == '.' || i >= 15 ) break;
        if ( r->server[i] || i == 0 || !dnb_here( smb1_calling ) )
            return mount_fail( r, E_NOFILE, MNT_NONAME, 0 );
    }
    want = ( r->spare & 1 ) ? 1 : 2;            /* NR_SPARE bit 0: NET USE /SMB1 */

    /* NR_SPARE bit 1: the password from the unlocked password list (PWL.C),
       and the user name too when the command gave none */
    if ( ( r->spare & 2 ) && !pwl_fill( r->server, r->domain, r->user, (int)sizeof( r->user ),
                                        r->pass, (int)sizeof( r->pass ) ) )
        return mount_fail( r, E_ACCESS, MNT_NOSAVED, 0 );

    s = (session *)nds_alloc( sizeof( session ) );
    if ( s == (session *)0 ) return mount_fail( r, E_GENERAL, MNT_MEMORY, 0 );
    memset( s, 0, sizeof( *s ) );
    if ( !list_alloc( s ) ) { nds_free( s ); return mount_fail( r, E_GENERAL, MNT_MEMORY, 0 ); }
    scopy( s->share, r->share, (int)sizeof( s->share ) );

    /* a connection to this server with these credentials already: the
       share is one more tree on it */
    for ( i = 0; i < NET_DRIVES; i++ ) {
        netconn *k = g_conn[i];
        if ( k == (netconn *)0 || k->ip != ip || k->want != want ) continue;
        if ( ip == 0 && !same_text( k->server, r->server ) ) continue;
        if ( !same_text( k->user, r->user ) || !same_text( k->domain, r->domain ) ||
             !same_bytes( k->pass, r->pass ) ) continue;
        s->nc = k;
        if ( conn_broken( k ) ) {               /* made again, with this share */
            if ( conn_revive( s ) == 0 ) { c = k; break; }
            s->nc = (netconn *)0;
            continue;
        }
        if ( tree_again( s ) == 0 ) { c = k; break; }
        if ( !conn_lost( k ) ) {                 /* the server refused the share */
            srvstatus = conn_status( k );
            sess_free( s );
            return mount_fail( r, E_ACCESS, MNT_SHARE, srvstatus );
        }
        if ( conn_revive( s ) == 0 ) { c = k; break; }
        s->nc = (netconn *)0;
    }

    if ( c == (netconn *)0 ) {
        c = (netconn *)nds_alloc( sizeof( netconn ) );
        if ( c == (netconn *)0 ) { sess_free( s ); return mount_fail( r, E_GENERAL, MNT_MEMORY, 0 ); }
        memset( c, 0, sizeof( *c ) );
        c->ip = ip;
        c->want = want;
        scopy( c->server, r->server, (int)sizeof( c->server ) );
        scopy( c->user, r->user, (int)sizeof( c->user ) );
        scopy( c->pass, r->pass, (int)sizeof( c->pass ) );
        scopy( c->domain, r->domain, (int)sizeof( c->domain ) );
        step = conn_open( c, r->share, &srvstatus );
        if ( step != 0 ) {
            nds_free( c );
            sess_free( s );
            return mount_fail( r, ( step == MNT_MEMORY || step == MNT_CONNECT ) ? E_GENERAL :
                                  ( step == MNT_NONAME ) ? E_NOFILE : E_ACCESS,   /* auth or share refused */
                               step, srvstatus );
        }
        c->gen = 1;
        for ( i = 0; i < NET_DRIVES; i++ ) if ( g_conn[i] == (netconn *)0 ) { g_conn[i] = c; break; }
        s->nc = c;
        s->tid = ( c->proto == 1 ) ? c->u.c1.tree_id : c->u.c2.tree_id;
        s->tgen = c->gen;
    }
    c->refs++;
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

/* one entry of the server's listing: kept when the find can give it back
   - its pattern and attributes are set before the listing starts - so a
   pattern that matches a few names in a big directory keeps a few */
static void collect( void *user, const smb2_dirent *e )
{
    session *s = (session *)user;
    ncent ne;
    unsigned int len;

    s->fseen++;
    if ( s->fshort ) return;                   /* full: the find fails */
    /* DOS sees the server's own 8.3 name for a long one: that name is
       a path the server knows, which a cut-down long name is not */
    to83( e->short_name[0] ? e->short_name : e->name, ne.disp, ne.fcb );
    ne.attr = (uint8_t)( e->attr & 0x37 );                 /* RO|HID|SYS|DIR|ARC */
    if ( ne.attr == 0 ) ne.attr = ATTR_ARCHIVE;            /* a plain file */
    if ( s->flong ) {
        /* a long-name find matches either name, as Windows does */
        if ( !wild_match( s->lpat, e->name ) && !fcb_match( s->fpat, ne.fcb ) ) return;
    } else if ( !fcb_match( s->fpat, ne.fcb ) ) return;
    if ( !attr_match( ne.attr, s->fsrch ) ) return;
    if ( s->count >= s->entmax && !ent_grow( s ) ) { s->fshort = 1; return; }

    ne.size = e->size_lo;
    ne.td = filetime_to_dos( e->wtime_lo, e->wtime_hi );
    /* and the long name, for a 714Eh find; a pool that cannot grow
       leaves the entry its 8.3 name */
    ne.lname = NO_LNAME;
    len = (unsigned int)slen( e->name ) + 1;
    if ( len <= s->poolmax - s->lpool_used || pool_grow( s, len ) ) {
        memcpy( s->lpool + s->lpool_used, e->name, len );
        ne.lname = s->lpool_used;
        s->lpool_used += len;
    }
    s->ent[s->count++] = ne;
}

/* an entry's long name: its own, or its 8.3 one when the pool was full */
static const char *ent_long( const session *s, const ncent *c )
{
    return ( c->lname == NO_LNAME ) ? c->disp : s->lpool + c->lname;
}

/* the entry at 'start' (every one kept matches the find) */
static int find_from( redir_req *r, session *s, int start )
{
    const ncent *c;

    if ( start < 0 || start >= s->count ) { r->status = E_NOFILE; return 0; }
    c = &s->ent[start];
    scopy( r->name, c->disp, 16 );
    if ( s->flong ) scopy( r->path, ent_long( s, c ), (int)sizeof( r->path ) );
    r->attr = c->attr;
    r->size_lo = c->size;
    r->size_hi = 0;
    r->time_date = c->td;
    r->gen = s->gen;
    r->count = (uint32_t)( start + 1 );        /* where FINDNEXT resumes */
    r->status = 0;
    return 0;
}

static int do_findfirst( redir_req *r )
{
    int drive = (int)r->drive;
    session *s;
    char full[260], dir[260], pat[260];

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_BADDRIVE; return 0;
    }
    s = g_sess[drive];
    if ( fullpath( s, r->path, full, (int)sizeof( full ) ) != 0 ) { r->status = E_NOPATH; return 0; }
    split_find( full, dir, pat );

    list_reset( s );                           /* the last listing goes */
    pat83( pat, s->fpat );
    scopy( s->lpat, pat, (int)sizeof( s->lpat ) );
    s->flong = ( r->spare == 1 );              /* NR_SPARE 1: 714Eh's find */
    s->fsrch = (uint8_t)r->attr;
    if ( sess_list_dir( s, dir, "*", collect, s ) != 0 && s->fseen == 0 ) {
        r->status = E_NOPATH;                  /* the directory is not there */
        return 0;
    }
    /* what matched did not all fit: no listing, rather than part of one
       that reads as the whole directory */
    if ( s->fshort ) {
        list_reset( s );
        r->status = E_NOMEM;
        return 0;
    }
    s->gen = ++g_gen;
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
    if ( r->count > (uint32_t)s->count ) { r->status = E_NOFILE; return 0; } /* past the end */
    find_from( r, s, (int)r->count );
    /* read to the end: a listing that grew gives its memory back now,
       not at the next find (one that did not grow stays, as before) */
    if ( r->status == E_NOFILE && ( s->entmax > NET_FIND_START || s->poolmax > NET_POOL_START ) )
        list_reset( s );
    return 0;
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
    if ( fullpath( s, r->path, path, (int)sizeof( path ) ) != 0 ) { r->status = E_NOPATH; return 0; }
    write = ( r->count == 1 || r->count == 2 );            /* access 1 write, 2 both */

    for ( slot = 0; slot < NET_OPEN_MAX; slot++ )
        if ( !s->open[slot].used ) break;
    if ( slot == NET_OPEN_MAX ) { r->status = E_NOHANDLE; return 0; }

    /* r->spare picks the disposition: 0 open an existing file (3Dh), 1
       create or truncate (3Ch create), 2 open or create (6Ch) */
    {
        uint32_t disp = ( r->spare == 1 ) ? 0x5UL : ( r->spare == 2 ) ? 0x3UL : 0x1UL;
        if ( sess_open_disp( s, path, write, disp, &fh ) != 0 ) {
            r->status = E_NOFILE;
            return 0;
        }
    }
    s->open[slot].used = 1;
    s->open[slot].bridged = g_on_fiber;
    s->open[slot].fh = fh;
    s->open[slot].write = write;
    s->open[slot].gen = s->nc->gen;
    scopy( s->open[slot].path, path, (int)sizeof( s->open[slot].path ) );
    r->handle = (uint32_t)( slot + 1 );
    r->size_lo = fh.size_lo;
    r->size_hi = fh.size_hi;
    r->attr = (uint8_t)( fh.attr & 0x37 );
    r->status = 0;
    return 0;
}

/* an open file's handle on the connection as it is now: one from before
   a reconnect is opened again by its name, for reading or writing as it
   was.  0, or -1. */
static int file_ready( session *s, int slot )
{
    smb2_fh fh;
    if ( s->open[slot].gen == s->nc->gen ) return 0;
    if ( sess_open_disp( s, s->open[slot].path, s->open[slot].write, 0x1UL, &fh ) != 0 ) return -1;
    s->open[slot].fh = fh;
    s->open[slot].gen = s->nc->gen;
    return 0;
}

static int do_read( redir_req *r )
{
    int drive = (int)r->drive, slot = (int)r->handle - 1;
    session *s;
    smb2_fh *fh;
    uint8_t *dst = (uint8_t *)r->bufptr;
    uint32_t off_lo = r->pos_lo, off_hi = r->pos_hi;
    unsigned int want = r->count, done = 0;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) { r->status = E_ACCESS; return 0; }
    s = g_sess[drive];
    if ( slot < 0 || slot >= NET_OPEN_MAX || !s->open[slot].used ) { r->status = E_NOHANDLE; return 0; }
    if ( file_ready( s, slot ) != 0 ) { r->status = E_ACCESS; return 0; }
    fh = &s->open[slot].fh;

    while ( done < want ) {
        unsigned int chunk = want - done;
        int n;
        uint32_t nlo;
        if ( chunk > 0x10000UL ) chunk = 0x10000UL;          /* both engines take less */
        n = sess_read( s, fh, off_lo, off_hi, dst + done, (int)chunk );
        if ( n < 0 ) { r->status = E_ACCESS; return 0; }
        if ( n == 0 ) break;                               /* end of file */
        done += (unsigned int)n;
        nlo = off_lo + (uint32_t)n;
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
    const uint8_t *src = (const uint8_t *)r->bufptr;
    uint32_t off_lo = r->pos_lo, off_hi = r->pos_hi;
    unsigned int want = r->count, done = 0;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) { r->status = E_ACCESS; return 0; }
    s = g_sess[drive];
    if ( slot < 0 || slot >= NET_OPEN_MAX || !s->open[slot].used ) { r->status = E_NOHANDLE; return 0; }
    if ( file_ready( s, slot ) != 0 ) { r->status = E_ACCESS; return 0; }
    fh = &s->open[slot].fh;

    if ( want == 0 ) { r->result = 0; r->status = 0; return 0; }   /* truncate: not yet */

    while ( done < want ) {
        unsigned int chunk = want - done;
        int n;
        uint32_t nlo;
        if ( chunk > 0x10000UL ) chunk = 0x10000UL;          /* both engines take less */
        n = sess_write( s, fh, off_lo, off_hi, src + done, (int)chunk );
        if ( n <= 0 ) { r->status = E_ACCESS; return 0; }
        if ( (unsigned int)n > chunk ) n = (int)chunk;
        done += (unsigned int)n;
        nlo = off_lo + (uint32_t)n;
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
    if ( fullpath( s, r->path, path, (int)sizeof( path ) ) != 0 ) { r->status = E_NOPATH; return 0; }
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
    if ( fullpath( s, r->path, oldp, (int)sizeof( oldp ) ) != 0 ||
         fullpath( s, r->share, newp, (int)sizeof( newp ) ) != 0 ) { r->status = E_NOPATH; return 0; }
    rc = sess_rename( s, oldp, newp );
    r->status = ( rc == 0 ) ? 0 : E_ACCESS;
    return 0;
}

/* ISDIR: is the path a directory?  CD asks before the kernel makes it the
   drive's current directory.  The share root always is. */
static int do_isdir( redir_req *r )
{
    int drive = (int)r->drive, i;
    session *s;
    char path[260];

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_GENERAL; return 0;
    }
    s = g_sess[drive];
    if ( fullpath( s, r->path, path, (int)sizeof( path ) ) != 0 ) { r->status = E_NOPATH; return 0; }
    for ( i = 0; path[i]; i++ )
        if ( path[i] == '*' || path[i] == '?' ) { r->status = E_NOPATH; return 0; }
    if ( path[0] == 0 ) { r->status = 0; return 0; }
    r->status = ( sess_isdir( s, path ) == 0 ) ? 0 : E_NOPATH;
    return 0;
}

/* =================================================================== */
/* names on the server: one entry by name, and a path walked by them   */
/* =================================================================== */

static int same_name( const char *a, const char *b )
{
    while ( *a && upper( *a ) == upper( *b ) ) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int has_wild( const char *name )
{
    for ( ; *name; name++ ) if ( *name == '*' || *name == '?' ) return 1;
    return 0;
}

static void name_emit( void *user, const smb2_dirent *e )
{
    session *s = (session *)user;
    if ( s->ffound ) return;
    if ( !same_name( s->fwant, e->name ) &&
         !( e->short_name[0] && same_name( s->fwant, e->short_name ) ) ) return;
    memcpy( &s->fent, e, sizeof( s->fent ) );
    s->ffound = 1;
}

/* the entry called 'name' (its long or its short name) in 'dir', into
   s->fent.  The directory is listed with the name as the server's
   pattern; the match is made here as well, because a LAN Manager server
   answers with the whole directory.  1 found, 0 not, -1 no such dir. */
static int lookup( session *s, const char *dir, const char *name )
{
    int rc;
    s->fwant = name;
    s->ffound = 0;
    rc = sess_list_dir( s, dir, name, name_emit, s );
    if ( s->ffound ) return 1;
    return ( rc != 0 ) ? -1 : 0;
}

/* add a component to a path being built: a backslash between, and the
   whole of it or nothing.  'up' upper-cases it.  0, or -1 if no room. */
static int path_add( char *buf, int *len, const char *comp, int max, int up )
{
    int n = *len, i;
    if ( n > 0 ) { if ( n + 1 >= max ) return -1; buf[n++] = '\\'; }
    for ( i = 0; comp[i]; i++ ) {
        if ( n + 1 >= max ) return -1;
        buf[n++] = up ? upper( comp[i] ) : comp[i];
    }
    buf[n] = 0;
    *len = n;
    return 0;
}

/* walk a canonical path from the share's root, a component at a time,
   into s->wlong (the server's own spelling) and s->wshort (each
   component's 8.3 name: the server's short one, an 8.3 name upper-
   cased, or a long name the server keeps no short name for, as it is).
   'dirs': every component must be a directory that is there (CD).
   Otherwise a component that is not there, or holds wildcards, and all
   after it go as typed (truename).  0, or E_NOPATH. */
static int walk( session *s, const char *path, int dirs )
{
    const char *p = path;
    int ln = 0, sn = 0, dn = 0, gone = 0;

    s->wlong[0] = s->wshort[0] = s->wdir[0] = 0;
    while ( *p ) {
        int cn = 0;
        const char *lname, *sname;
        int up;

        while ( *p && *p != '\\' ) {
            if ( cn >= (int)sizeof( s->wcomp ) - 1 ) return E_NOPATH;
            s->wcomp[cn++] = *p++;
        }
        s->wcomp[cn] = 0;
        if ( *p == '\\' ) p++;
        if ( cn == 0 ) continue;

        if ( !gone && !has_wild( s->wcomp ) && lookup( s, s->wdir, s->wcomp ) == 1 ) {
            if ( dirs && !( s->fent.attr & ATTR_DIR ) ) return E_NOPATH;
            lname = s->fent.name;
            if ( s->fent.short_name[0] ) { sname = s->fent.short_name; up = 0; }
            else { sname = s->fent.name; up = is83( s->fent.name ); }
        } else {
            if ( dirs ) return E_NOPATH;
            gone = 1;                                       /* the rest as typed */
            lname = s->wcomp;
            sname = s->wcomp;
            up = is83( s->wcomp ) || has_wild( s->wcomp );
        }
        if ( path_add( s->wlong, &ln, lname, (int)sizeof( s->wlong ), 0 ) != 0 ||
             path_add( s->wshort, &sn, sname, (int)sizeof( s->wshort ), up ) != 0 )
            return E_NOPATH;
        /* the next listing is of this directory, by its own name */
        if ( !gone && path_add( s->wdir, &dn, lname, (int)sizeof( s->wdir ), 0 ) != 0 )
            gone = 1;
    }
    return 0;
}

/* GETATTR: one entry's attribute byte, by its name (long or short). */
static int do_getattr( redir_req *r )
{
    int drive = (int)r->drive, i, cut = -1, rc;
    session *s;
    char path[260];

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_GENERAL; return 0;
    }
    s = g_sess[drive];
    if ( fullpath( s, r->path, path, (int)sizeof( path ) ) != 0 ) { r->status = E_NOPATH; return 0; }
    if ( has_wild( path ) ) { r->status = E_NOFILE; return 0; }
    for ( i = 0; path[i]; i++ ) if ( path[i] == '\\' ) cut = i;
    if ( path[0] == 0 ) { r->status = E_NOFILE; return 0; }     /* the root has none */
    scopy( s->wcomp, path + cut + 1, (int)sizeof( s->wcomp ) );
    if ( cut >= 0 ) path[cut] = 0;                             /* path: the directory */
    else            path[0] = 0;
    rc = lookup( s, path, s->wcomp );
    if ( rc < 0 ) { r->status = E_NOPATH; return 0; }
    if ( rc == 0 ) { r->status = E_NOFILE; return 0; }
    r->attr = (uint8_t)( s->fent.attr & 0x37 );
    if ( r->attr == 0 ) r->attr = ATTR_ARCHIVE;
    r->status = 0;
    return 0;
}

/* CHDIR: the drive's new current directory, every component of it a
   directory on the server.  NR_SPARE 1 (LFN=OFF): its 8.3 form must fit
   47h's 64 bytes, as on any DOS drive. */
static int do_chdir( redir_req *r )
{
    int drive = (int)r->drive, rc;
    session *s;
    char path[260];

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_GENERAL; return 0;
    }
    s = g_sess[drive];
    if ( fullpath( s, r->path, path, (int)sizeof( path ) ) != 0 || has_wild( path ) ) {
        r->status = E_NOPATH; return 0;
    }
    rc = walk( s, path, 1 );
    if ( rc != 0 ) { r->status = (uint32_t)rc; return 0; }
    if ( r->spare == 1 && slen( s->wshort ) > 63 ) { r->status = E_NOPATH; return 0; }
    scopy( s->cwd, s->wlong, (int)sizeof( s->cwd ) );
    scopy( s->cwd83, s->wshort, (int)sizeof( s->cwd83 ) );
    r->status = 0;
    return 0;
}

/* GETCWD: the current directory into path - the long form, or with
   NR_SPARE 1 the 8.3 one.  No server traffic: CD worked both out. */
static int do_getcwd( redir_req *r )
{
    int drive = (int)r->drive;
    session *s;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_GENERAL; return 0;
    }
    s = g_sess[drive];
    scopy( r->path, ( r->spare == 1 ) ? s->cwd83 : s->cwd, (int)sizeof( r->path ) );
    r->status = 0;
    return 0;
}

/* CANON: a path as truename gives it, from the share's root and with no
   leading backslash: NR_SPARE 1 the long form, 0 the 8.3 one. */
static int do_canon( redir_req *r )
{
    int drive = (int)r->drive, rc;
    session *s;
    char path[260];

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_GENERAL; return 0;
    }
    s = g_sess[drive];
    if ( fullpath( s, r->path, path, (int)sizeof( path ) ) != 0 ) { r->status = E_NOPATH; return 0; }
    rc = walk( s, path, 0 );
    if ( rc != 0 ) { r->status = (uint32_t)rc; return 0; }
    scopy( r->path, ( r->spare == 1 ) ? s->wlong : s->wshort, (int)sizeof( r->path ) );
    r->status = 0;
    return 0;
}

/* INFO: the share's size, in the server's own units (REDIR.H) */
static int do_info( redir_req *r )
{
    int drive = (int)r->drive;
    session *s;
    smb_fs_size fs;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) {
        r->status = E_GENERAL; return 0;
    }
    s = g_sess[drive];
    if ( sess_fs_size( s, &fs ) != 0 || fs.sec_per_unit == 0 || fs.bytes_per_sec == 0 ) {
        r->status = E_GENERAL; return 0;
    }
    r->size_lo = fs.total_lo;
    r->size_hi = fs.total_hi;
    r->pos_lo  = fs.free_lo;
    r->pos_hi  = fs.free_hi;
    r->count   = fs.sec_per_unit;
    r->result  = fs.bytes_per_sec;
    r->status  = 0;
    return 0;
}

static int do_close( redir_req *r )
{
    int drive = (int)r->drive, slot = (int)r->handle - 1;
    session *s;

    if ( drive < 0 || drive >= NET_DRIVES || g_sess[drive] == (session *)0 ) { r->status = 0; return 0; }
    s = g_sess[drive];
    if ( slot >= 0 && slot < NET_OPEN_MAX && s->open[slot].used ) {
        /* a handle from before a reconnect went with the old connection */
        if ( s->open[slot].gen == s->nc->gen && !conn_broken( s->nc ) )
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

void __cdecl nd_halt( void )
{
}

int __cdecl nd_irq( uint32_t line )
{
    (void)line;
    return 0;
}

void __cdecl nd_tick( void )
{
}

static int redir_run( unsigned int op, redir_req *r );

/* A REQUEST ON A DRIVE WHOSE CONNECTION HAS GONE is run on a new one.
   sess_ready makes it again before the request when an earlier one found
   it broken; when it breaks under this request - the server ended it
   while the drive sat idle, which Windows does after 15 minutes - it is
   made again and the request run once more from a copy of how it came. */
static redir_req g_rreq;                   /* the request as it came, for the rerun */

static int redir_dispatch( unsigned int op, redir_req *r )
{
    session *s = (session *)0;
    int rc;

    if ( op != REDIR_MOUNT && op != REDIR_UMOUNT && op != REDIR_PWL && r->drive < NET_DRIVES )
        s = g_sess[r->drive];
    if ( s == (session *)0 ) return redir_run( op, r );
    if ( sess_ready( s ) != 0 ) return redir_run( op, r );     /* the server is not
                                              there: the answer says so, once */
    memcpy( &g_rreq, r, sizeof( g_rreq ) );
    rc = redir_run( op, r );
    if ( g_sess[r->drive] == s && conn_lost( s->nc ) && conn_revive( s ) == 0 ) {
        memcpy( r, &g_rreq, sizeof( *r ) );
        rc = redir_run( op, r );
    }
    return rc;
}

static int redir_run( unsigned int op, redir_req *r )
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
    case REDIR_INFO:      return do_info( r );
    case REDIR_ISDIR:     return do_isdir( r );
    case REDIR_GETATTR:   return do_getattr( r );
    case REDIR_CHDIR:     return do_chdir( r );
    case REDIR_GETCWD:    return do_getcwd( r );
    case REDIR_CANON:     return do_canon( r );
    case REDIR_PWL:
        /* the block is the program's, checked by the kernel to be its own
           and this long; the answer goes in it */
        if ( r->count < sizeof( pwl_req ) || r->bufptr == 0 ) return ERR_BADLEN;
        pwl_call( (pwl_req *)r->bufptr );
        r->status = 0;
        return 0;
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

static int      g_wstate = WS_IDLE;
static int      g_wdone;                   /* the op finished this slice */
static int      g_wlive;                   /* the fibre has been bootstrapped */
static int      g_wrc;                      /* redir_dispatch's return, for nd_call */
static redir_req g_wreq;                   /* the op's own copy, stable across slices */
static uint32_t g_wstack[4096];            /* 16 KB worker stack (image carries it) */

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

int __cdecl nd_call( uint32_t fn, void *buf, uint32_t len, uint32_t *answer )
{
    redir_req *r = (redir_req *)buf;
    *answer = 0;
    if ( len < sizeof( redir_req ) ) return ERR_BADLEN;

    if ( !( fn & REDIR_RESUME ) )
        return redir_dispatch( fn, r );     /* native: run to completion */

    if ( g_wstate == WS_IDLE ) {            /* the first slice of a new op */
        memcpy( &g_wreq, r, sizeof( g_wreq ) );
        g_wreq.op = fn & ~(uint32_t)REDIR_RESUME;
        g_wstate  = WS_RUN;
        g_wdone   = 0;
        g_on_fiber = 1;
        if ( !g_wlive ) {
            fib_init( (uint32_t)( g_wstack + sizeof( g_wstack ) / sizeof( g_wstack[0] ) ),
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
/* the park                                                             */
/*                                                                      */
/* A MAPPED DRIVE OUTLIVES WINDOWS.  TCPIP.SYS keeps its sockets while  */
/* the kernel is parked, and the bridge (PMDOS.386, or RMDOS in         */
/* standard mode) uses these sessions meanwhile, so they are kept.      */
/* They were freed here until 2026-11-26, and every NET USE letter came */
/* back from Windows with no session behind it: "Invalid drive          */
/* specification" or "No files found" until NET USE was typed again.    */
/*                                                                      */
/* What a park can leave behind is put right on the way back:           */
/*   - an op the bridge started and Windows did not let finish.  Its    */
/*     fibre is dropped, and every connection is made again at its next */
/*     use, because the op may have left half a message in the stream; */
/*   - a file opened through the bridge and never closed: Windows ended */
/*     with it open.  Its slot is freed, and its connection made again, */
/*     which closes it at the server.  A file the kernel opened before  */
/*     the park (a batch file a shell is reading) is left alone.        */
/* A connection the server ended while Windows ran is found and made    */
/* again by the first request (redir_dispatch).                         */
/* =================================================================== */

static void conn_mark_broken( netconn *c )
{
    if ( c->proto == 1 ) c->u.c1.broken = 1;
    else                 c->u.c2.broken = 1;
}

void __cdecl nd_park( int back )
{
    int i, slot, abandoned;
    session *s;

    if ( !back ) return;
    abandoned = ( g_wstate != WS_IDLE );
    g_wstate = WS_IDLE;
    g_on_fiber = 0;                         /* a native call must not yield */
    g_wlive = 0;                            /* the next resumable op starts a new fibre */
    for ( i = 0; i < NET_DRIVES; i++ ) {
        s = g_sess[i];
        if ( s == (session *)0 ) continue;
        if ( abandoned ) conn_mark_broken( s->nc );
        for ( slot = 0; slot < NET_OPEN_MAX; slot++ ) {
            if ( s->open[slot].used && s->open[slot].bridged ) {
                s->open[slot].used = 0;
                conn_mark_broken( s->nc );
            }
        }
    }
}

/* =================================================================== */
/* the driver table - NDSTART.ASM names it                              */
/* =================================================================== */

const ND_TABLE nd_table = {
    NDT_SIGVAL, NDT_VERSION, NDTF_REDIR,
    { 'S', 'M', 'B', 'F', 'S', ' ', ' ', ' ' },
    ndv_init, ndv_park, ndv_halt, ndv_call
};
