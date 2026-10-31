/* ==========================================================================
 * SMBSRVDOS.C - SMBSRV.EXE: the native PM-DOS front end of the SMB1 file
 * server, so PM-DOS can SHARE its own files over the network.
 * ==========================================================================
 * A native command over the shared net runtime (CMD\NET\SOCKLIB behind
 * NETSTART).  It listens on NetBIOS-over-TCP port 139, and for each client
 * drives the protocol core (SMBSRV.C) with an INT 21h filesystem back end:
 * the served tree is an ordinary PM-DOS directory reached through 3Dh /
 * 3Fh / 40h / 4Eh and the rest.
 *
 *   SMBSRV [path]        (shares 'path', or C:\ by default)
 * ========================================================================== */
#include "socklib.h"
#include "smbsrv.h"
#include "netbios.h"

/* the little C runtime SMBSRV.C needs (like the driver's DRVRT) */
void *memcpy( void *to, const void *from, unsigned len )
{ unsigned char *d = (unsigned char *)to; const unsigned char *s = (const unsigned char *)from; while ( len-- ) *d++ = *s++; return to; }
void *memset( void *to, int v, unsigned len )
{ unsigned char *d = (unsigned char *)to; while ( len-- ) *d++ = (unsigned char)v; return to; }

static unsigned int  get16( const unsigned char *p ) { return (unsigned int)p[0] | ( (unsigned int)p[1] << 8 ); }
static unsigned long get32( const unsigned char *p ) { return (unsigned long)p[0] | ( (unsigned long)p[1] << 8 ) | ( (unsigned long)p[2] << 16 ) | ( (unsigned long)p[3] << 24 ); }

/* ====================================================================== */
/* the INT 21h filesystem back end                                        */
/* ====================================================================== */

typedef struct { char root[80]; } dosfs;

/* root + share-relative "\a\b" -> an absolute DOS path; 0 if it escapes or
   will not fit 'cap' (the client's path is untrusted, so every write to
   'out' is bounded). */
#define DPATH_MAX  400
static int dpath( dosfs *fs, const char *rel, char *out, int cap )
{
    int n = 0, i;
    const char *p = rel;
    for ( i = 0; fs->root[i]; i++ ) { if ( n >= cap - 1 ) return 0; out[n++] = fs->root[i]; }
    if ( n > 0 && out[n - 1] != '\\' ) { if ( n >= cap - 1 ) return 0; out[n++] = '\\'; }
    while ( *p == '\\' || *p == '/' ) p++;
    while ( *p ) {
        char comp[80]; int cl = 0;
        while ( *p && *p != '\\' && *p != '/' ) { if ( cl >= 79 ) return 0; comp[cl++] = *p; p++; }
        comp[cl] = 0;
        while ( *p == '\\' || *p == '/' ) p++;
        if ( cl == 0 ) continue;
        if ( comp[0] == '.' && comp[1] == 0 ) continue;
        if ( comp[0] == '.' && comp[1] == '.' && comp[2] == 0 ) return 0;   /* escape */
        if ( n > 0 && out[n - 1] != '\\' ) { if ( n >= cap - 1 ) return 0; out[n++] = '\\'; }
        for ( i = 0; i < cl; i++ ) { if ( n >= cap - 1 ) return 0; out[n++] = comp[i]; }
    }
    out[n] = 0;
    return 1;
}

static int d_open( void *ctx, const char *path, int write, int dispo, unsigned long *size, int *action )
{
    dosfs *fs = (dosfs *)ctx;
    char full[DPATH_MAX];
    REGS r;
    int h;

    if ( !dpath( fs, path, full, DPATH_MAX ) ) return -1;

    if ( dispo == 5 ) {                                 /* create / truncate */
        sl_memzero( &r, sizeof( r ) ); r.eax = 0x3C00; r.ecx = 0; r.edx = (u32)full; int21( &r );
        if ( CF( r ) ) return -1;
        h = (int)( r.eax & 0xFFFF ); *action = 2;
    } else if ( dispo == 3 ) {                          /* open, else create */
        sl_memzero( &r, sizeof( r ) ); r.eax = 0x3D02; r.edx = (u32)full; int21( &r );
        if ( !CF( r ) ) { h = (int)( r.eax & 0xFFFF ); *action = 1; }
        else {
            sl_memzero( &r, sizeof( r ) ); r.eax = 0x3C00; r.ecx = 0; r.edx = (u32)full; int21( &r );
            if ( CF( r ) ) return -1;
            h = (int)( r.eax & 0xFFFF ); *action = 2;
        }
    } else {                                            /* open existing */
        sl_memzero( &r, sizeof( r ) ); r.eax = (u32)( 0x3D00 | ( write ? 2 : 0 ) ); r.edx = (u32)full; int21( &r );
        if ( CF( r ) ) return -1;
        h = (int)( r.eax & 0xFFFF ); *action = 1;
    }

    /* a device name (CON, PRN, ...) opens the device: serve files only */
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4400; r.ebx = (u32)h; int21( &r );
    if ( CF( r ) || ( r.edx & 0x80 ) ) {
        sl_memzero( &r, sizeof( r ) ); r.eax = 0x3E00; r.ebx = (u32)h; int21( &r );
        return -1;
    }

    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4202; r.ebx = (u32)h; r.ecx = 0; int21( &r );
    *size = CF( r ) ? 0 : r.eax;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4200; r.ebx = (u32)h; r.ecx = 0; int21( &r );
    return h;
}

static int d_read( void *ctx, int h, unsigned long off, void *buf, int len )
{
    REGS r; (void)ctx;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4200; r.ebx = (u32)h; r.ecx = off; int21( &r );
    if ( CF( r ) ) return -1;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x3F00; r.ebx = (u32)h; r.ecx = (u32)len; r.edx = (u32)buf; int21( &r );
    if ( CF( r ) ) return -1;
    return (int)r.eax;
}

static int d_write( void *ctx, int h, unsigned long off, const void *buf, int len )
{
    REGS r; (void)ctx;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4200; r.ebx = (u32)h; r.ecx = off; int21( &r );
    if ( CF( r ) ) return -1;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4000; r.ebx = (u32)h; r.ecx = (u32)len; r.edx = (u32)buf; int21( &r );
    if ( CF( r ) ) return -1;
    return (int)r.eax;
}

static void d_close( void *ctx, int h )
{
    REGS r; (void)ctx;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x3E00; r.ebx = (u32)h; int21( &r );
}

/* ---- directory enumeration via the DTA find ---- */
static unsigned char g_dta[64];
static int           g_pending;

/* DOS date/time -> a Windows FILETIME (lo/hi), via a 32x32->64 multiply */
static void umul32( u32 a, u32 b, u32 *hi, u32 *lo )
{
    u32 a0 = a & 0xFFFF, a1 = a >> 16, b0 = b & 0xFFFF, b1 = b >> 16;
    u32 p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;
    u32 mid = ( p0 >> 16 ) + ( p1 & 0xFFFF ) + ( p2 & 0xFFFF );
    *lo = ( p0 & 0xFFFF ) | ( mid << 16 );
    *hi = p3 + ( p1 >> 16 ) + ( p2 >> 16 ) + ( mid >> 16 );
}
static long days_from_civil( int y, int m, int d )
{
    long era; unsigned yoe, doy, doe;
    y -= ( m <= 2 );
    era = (long)( ( y >= 0 ? y : y - 399 ) / 400 );
    yoe = (unsigned)( y - era * 400 );
    doy = (unsigned)( ( 153 * ( m > 2 ? m - 3 : m + 9 ) + 2 ) / 5 + d - 1 );
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static void dos_filetime( unsigned date, unsigned time, u32 *lo, u32 *hi )
{
    int year  = 1980 + ( ( date >> 9 ) & 0x7F );
    int month = ( date >> 5 ) & 0x0F;
    int day   = date & 0x1F;
    int hour  = ( time >> 11 ) & 0x1F, min = ( time >> 5 ) & 0x3F, sec = ( time & 0x1F ) * 2;
    long days;
    u32 shi, slo, phi, plo, add;

    if ( month < 1 ) month = 1;
    if ( day < 1 ) day = 1;
    days = days_from_civil( year, month, day ) + 134774L;       /* days since 1601 */
    umul32( (u32)days, 86400UL, &shi, &slo );
    add = (u32)( hour * 3600 + min * 60 + sec );
    { u32 n = slo + add; if ( n < slo ) shi++; slo = n; }
    umul32( slo, 10000000UL, &phi, &plo );
    *lo = plo;
    *hi = phi + shi * 10000000UL;
}

static void *d_findopen( void *ctx, const char *dir )
{
    dosfs *fs = (dosfs *)ctx;
    char full[DPATH_MAX];
    REGS r;
    int n;

    if ( !dpath( fs, dir, full, DPATH_MAX - 5 ) ) return (void *)0;   /* room for \*.* */
    n = (int)sl_strlen( full );
    if ( n == 0 || full[n - 1] != '\\' ) full[n++] = '\\';
    full[n++] = '*'; full[n++] = '.'; full[n++] = '*'; full[n] = 0;

    sl_memzero( &r, sizeof( r ) ); r.eax = 0x1A00; r.edx = (u32)g_dta; int21( &r );   /* set DTA */
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4E00; r.ecx = 0x16; r.edx = (u32)full; int21( &r );
    if ( CF( r ) ) { g_pending = 0; return (void *)0; }
    g_pending = 1;
    return (void *)1;
}

static int d_findnext( void *ctx, void *it, srv_dirent *de )
{
    REGS r; int i; unsigned date, time;
    (void)ctx; (void)it;
    if ( !g_pending ) return 0;

    de->attr = g_dta[0x15];
    time = get16( g_dta + 0x16 );
    date = get16( g_dta + 0x18 );
    de->size = get32( g_dta + 0x1A );
    for ( i = 0; i < 13 && g_dta[0x1E + i]; i++ ) de->name[i] = (char)g_dta[0x1E + i];
    de->name[i] = 0;
    dos_filetime( date, time, &de->ft_lo, &de->ft_hi );
    de->dosdate = date;
    de->dostime = time;

    sl_memzero( &r, sizeof( r ) ); r.eax = 0x1A00; r.edx = (u32)g_dta; int21( &r );
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4F00; int21( &r );
    if ( CF( r ) ) g_pending = 0;
    return 1;
}

static void d_findclose( void *ctx, void *it ) { (void)ctx; (void)it; g_pending = 0; }

static int d_mkdir( void *ctx, const char *path )
{ char f[DPATH_MAX]; REGS r; if ( !dpath( (dosfs *)ctx, path, f, DPATH_MAX ) ) return -1; sl_memzero( &r, sizeof( r ) ); r.eax = 0x3900; r.edx = (u32)f; int21( &r ); return CF( r ) ? -1 : 0; }
static int d_rmdir( void *ctx, const char *path )
{ char f[DPATH_MAX]; REGS r; if ( !dpath( (dosfs *)ctx, path, f, DPATH_MAX ) ) return -1; sl_memzero( &r, sizeof( r ) ); r.eax = 0x3A00; r.edx = (u32)f; int21( &r ); return CF( r ) ? -1 : 0; }
static int d_unlink( void *ctx, const char *path )
{ char f[DPATH_MAX]; REGS r; if ( !dpath( (dosfs *)ctx, path, f, DPATH_MAX ) ) return -1; sl_memzero( &r, sizeof( r ) ); r.eax = 0x4100; r.edx = (u32)f; int21( &r ); return CF( r ) ? -1 : 0; }

/* a file or directory's entry, by a 4Eh search for its own name (the
   share root has none: it is a directory with no date) */
static unsigned char g_sdta[64];
static int d_stat( void *ctx, const char *path, srv_dirent *de )
{
    char f[DPATH_MAX];
    REGS r;
    const char *p;

    sl_memzero( de, sizeof( *de ) );
    p = path;
    while ( *p == '\\' || *p == '/' ) p++;
    if ( *p == 0 ) { de->attr = 0x10; return 0; }
    for ( p = path; *p; p++ ) if ( *p == '*' || *p == '?' ) return -1;
    if ( !dpath( (dosfs *)ctx, path, f, DPATH_MAX ) ) return -1;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x1A00; r.edx = (u32)g_sdta; int21( &r );
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4E00; r.ecx = 0x16; r.edx = (u32)f; int21( &r );
    if ( CF( r ) ) return -1;
    de->attr = g_sdta[0x15];
    de->dostime = get16( g_sdta + 0x16 );
    de->dosdate = get16( g_sdta + 0x18 );
    de->size = get32( g_sdta + 0x1A );
    dos_filetime( de->dosdate, de->dostime, &de->ft_lo, &de->ft_hi );
    return 0;
}

static int d_rename( void *ctx, const char *from, const char *to )
{
    char f[DPATH_MAX], t[DPATH_MAX];
    REGS r;
    if ( !dpath( (dosfs *)ctx, from, f, DPATH_MAX ) || !dpath( (dosfs *)ctx, to, t, DPATH_MAX ) ) return -1;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x5600; r.edx = (u32)f; r.edi = (u32)t; int21( &r );
    return CF( r ) ? -1 : 0;
}

static int d_setattr( void *ctx, const char *path, unsigned int attr )
{
    char f[DPATH_MAX];
    REGS r;
    if ( !dpath( (dosfs *)ctx, path, f, DPATH_MAX ) ) return -1;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4301; r.ecx = attr; r.edx = (u32)f; int21( &r );
    return CF( r ) ? -1 : 0;
}

/* an open file's size and date (the reads and writes seek for themselves,
   so the file pointer is free to move) */
static int d_fstat( void *ctx, int h, srv_dirent *de )
{
    REGS r; (void)ctx;
    sl_memzero( de, sizeof( *de ) );
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x5700; r.ebx = (u32)h; int21( &r );
    if ( CF( r ) ) return -1;
    de->dostime = (unsigned int)( r.ecx & 0xFFFF );
    de->dosdate = (unsigned int)( r.edx & 0xFFFF );
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x4202; r.ebx = (u32)h; int21( &r );
    if ( CF( r ) ) return -1;
    de->size = r.eax;
    de->attr = 0x20;
    dos_filetime( de->dosdate, de->dostime, &de->ft_lo, &de->ft_hi );
    return 0;
}

static int d_settime( void *ctx, int h, unsigned int date, unsigned int time )
{
    REGS r; (void)ctx;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x5701; r.ebx = (u32)h; r.ecx = time; r.edx = date; int21( &r );
    return CF( r ) ? -1 : 0;
}

/* the served drive's size and free space, in clusters (36h) */
static int d_disk( void *ctx, unsigned long *units, unsigned long *avail, unsigned long *unitsize )
{
    dosfs *fs = (dosfs *)ctx;
    REGS r;
    char drv = fs->root[0];
    if ( drv >= 'a' && drv <= 'z' ) drv = (char)( drv - 32 );
    if ( drv < 'A' || drv > 'Z' || fs->root[1] != ':' ) return -1;
    sl_memzero( &r, sizeof( r ) ); r.eax = 0x3600; r.edx = (u32)( drv - 'A' + 1 ); int21( &r );
    if ( ( r.eax & 0xFFFF ) == 0xFFFF ) return -1;
    *units = r.edx & 0xFFFF;
    *avail = r.ebx & 0xFFFF;
    *unitsize = ( r.eax & 0xFFFF ) * ( r.ecx & 0xFFFF );
    return 0;
}

static const srv_vfs DOSVFS = {
    d_open, d_read, d_write, d_close, d_findopen, d_findnext, d_findclose, d_mkdir, d_rmdir, d_unlink,
    d_stat, d_rename, d_setattr, d_fstat, d_settime, d_disk
};

/* ====================================================================== */
/* the transport, over the accepted socket                               */
/* ====================================================================== */

static int srv_send( void *ctx, const unsigned char *buf, int len ) { return sl_send( (int)ctx, buf, (u32)len, 15000 ); }
static int srv_recv( void *ctx, unsigned char *buf, int len ) { return sl_recvwait( (int)ctx, buf, (u32)len, 30000 ); }

/* ====================================================================== */
/* the transport over NetBEUI (NETBEUI.SYS, on its row: 21h/F1h AL=25h)  */
/* ====================================================================== */

/* SMBSRV.C speaks NetBIOS over TCP: a 4-byte header on every message.
   A NetBEUI session keeps the messages whole, so the header is taken off
   what goes and made from the length of what comes */

#define NB_PIECE 1024

static int nb_row = -1;                 /* NETBEUI.SYS's row, or -1 */

static u32 nbc( u32 fn, void *buf, u32 len, int *err )
{
    REGS regs;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = ( (u32)NDF_CALL << 16 ) | 0xF125;
    regs.edx = (u32)nb_row;
    regs.ebx = fn;
    regs.ecx = len;
    regs.edi = (u32)buf;
    int21( &regs );
    *err = CF( regs ) ? (int)( regs.eax & 0xFFFF ) : 0;
    return *err ? 0 : regs.eax;
}

/* the row of a loaded NETBEUI.SYS, or -1 */
static int nb_find_row( void )
{
    REGS regs;
    ND_ROW row;
    u32 count, i;

    sl_memzero( &regs, sizeof( regs ) );
    regs.eax = ( (u32)NDF_COUNT << 16 ) | 0xF125;
    int21( &regs );
    if ( CF( regs ) ) return -1;
    count = regs.ecx;
    for ( i = 0; i < count; i++ ) {
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = ( (u32)NDF_INFO << 16 ) | 0xF125;
        regs.edx = i;
        regs.edi = (u32)&row;
        regs.ecx = sizeof( row );
        int21( &regs );
        if ( !CF( regs ) && ( row.flags & NDTF_NETBIOS ) ) return (int)i;
    }
    return -1;
}

static u32 nb_state( u32 h, NB_STAT *st )
{
    int err;
    sl_memzero( st, NBSTAT_LEN );
    st->handle = h;
    nbc( NB_STATE, st, NBSTAT_LEN, &err );
    return err ? NBX_FAIL : st->state;
}

typedef struct {
    u32           h;
    unsigned char pend[4];
    int           pendn, pendoff;
    u32           left;
} nbconn;

static int nb_send( void *ctx, const unsigned char *buf, int len )
{
    nbconn *c = (nbconn *)ctx;
    unsigned char out[NBIO_HDR + NB_PIECE];
    NB_IO *io = (NB_IO *)out;
    NB_STAT st;
    u32 plen, n, got, start = sl_now();
    const unsigned char *p;
    int err;

    if ( len < 4 ) return -1;
    if ( buf[0] != 0x00 ) return len;   /* 0x82 and keepalives: not on NetBEUI */
    plen = ( (u32)( buf[1] & 1 ) << 16 ) | ( (u32)buf[2] << 8 ) | buf[3];
    if ( (u32)len != 4 + plen ) return -1;
    p = buf + 4;
    do {
        n = plen > NB_PIECE ? NB_PIECE : plen;
        io->handle = c->h;
        io->len = n;
        sl_memcpy( out + NBIO_HDR, p, n );
        for ( ;; ) {
            got = nbc( n == plen ? NB_SEND : NB_SENDP, out, NBIO_HDR + n, &err );
            if ( err ) return -1;
            if ( got ) break;
            if ( nb_state( c->h, &st ) != NBX_UP || sl_since( start ) > 1500 ) return -1;
        }
        p += n;
        plen -= n;
    } while ( plen );
    return len;
}

static int nb_recv( void *ctx, unsigned char *buf, int len )
{
    nbconn *c = (nbconn *)ctx;
    unsigned char in[NBIO_HDR + NB_PIECE];
    NB_IO *io = (NB_IO *)in;
    NB_STAT st;
    u32 n, start = sl_now();
    int err;

    if ( len <= 0 ) return 0;
    if ( c->pendoff == c->pendn && c->left == 0 ) {
        for ( ;; ) {                    /* the next message */
            if ( nb_state( c->h, &st ) != NBX_UP && st.rxready == 0 ) return 0;
            if ( st.rxready ) break;
            if ( sl_since( start ) > 30000 ) return 0;
        }
        n = st.rxready;
        c->pend[0] = 0x00;
        c->pend[1] = (unsigned char)( ( n >> 16 ) & 1 );
        c->pend[2] = (unsigned char)( n >> 8 );
        c->pend[3] = (unsigned char)n;
        c->pendn = 4;
        c->pendoff = 0;
        c->left = n;
    }
    if ( c->pendoff < c->pendn ) {
        n = (u32)( c->pendn - c->pendoff );
        if ( n > (u32)len ) n = (u32)len;
        sl_memcpy( buf, c->pend + c->pendoff, n );
        c->pendoff += (int)n;
        return (int)n;
    }
    n = c->left < (u32)len ? c->left : (u32)len;
    if ( n > NB_PIECE ) n = NB_PIECE;
    io->handle = c->h;
    io->len = 0;
    n = nbc( NB_RECV, in, NBIO_HDR + n, &err );
    if ( err ) return -1;
    sl_memcpy( buf, in + NBIO_HDR, n );
    c->left -= n;
    return (int)n;
}

/* a listen on our server name, <20> */
static u32 nb_listen( void )
{
    NB_NAMEREQ req;
    int err;
    u32 h;

    sl_memzero( &req, NBNAME_LEN );
    req.suffix = 0x20;
    h = nbc( NB_LISTEN, &req, NBNAME_LEN, &err );
    return err ? 0 : h;
}

void __cdecl dos_entry( void )
{
    char **argv;
    int    argc, lh = 0, nh, tcp, err;
    dosfs  fs;
    u32    peer_ip, nbh = 0;
    u16    peer_port;
    NB_STAT st;
    NB_IF nbif;

    argc = sl_args( &argv );
    sl_strcpy( fs.root, ( argc >= 1 && argv[0][0] ) ? argv[0] : "C:\\", sizeof( fs.root ) );

    nb_row = nb_find_row();
    tcp = sl_up( nb_row >= 0 ? 300 : 8000 );    /* no TCP/IP loaded: at once */
    if ( !tcp && nb_row < 0 ) { sl_puts( "SMBSRV: the network is not up.\r\n" ); sl_flush(); return; }

    /* the computer's NetBIOS name (TCPIP.SYS /NAME=, else NETBEUI.SYS's),
       for SESSION_SETUP */
    if ( tcp ) {
        SOCK_IF info;
        REGS regs;
        sl_memzero( &info, SOCKIF_LEN2 );
        sl_memzero( &regs, sizeof( regs ) );
        regs.eax = ( (u32)SOCK_IFSTAT << 16 ) | 0xF126;
        regs.ecx = SOCKIF_LEN2;
        regs.edi = (u32)&info;
        int21( &regs );
        if ( !CF( regs ) && info.nbname[0] ) sl_strcpy( smbsrv_name, info.nbname, sizeof( smbsrv_name ) );
    }
    if ( nb_row >= 0 ) {
        sl_memzero( &nbif, NBIF_LEN );
        nbc( NB_IFSTAT, &nbif, NBIF_LEN, &err );
        if ( err || !nbif.up ) nb_row = -1;
        else if ( !tcp || sl_strcmp( smbsrv_name, "PMDOS" ) == 0 ) sl_strcpy( smbsrv_name, nbif.name, sizeof( smbsrv_name ) );
    }

    if ( tcp ) {
        lh = sl_open( SOCK_STREAM );
        if ( lh == 0 || !sl_listen( lh, 139 ) ) { sl_puts( "SMBSRV: cannot listen on port 139.\r\n" ); tcp = 0; }
    }
    if ( nb_row >= 0 ) {
        nbh = nb_listen();
        if ( nbh == 0 ) { sl_puts( "SMBSRV: cannot listen on NetBEUI.\r\n" ); nb_row = -1; }
    }
    if ( !tcp && nb_row < 0 ) { sl_flush(); return; }

    sl_puts( "SMBSRV: sharing " ); sl_puts( fs.root );
    sl_puts( " as \\\\" ); sl_puts( smbsrv_name );
    sl_puts( tcp && nb_row >= 0 ? " on TCP/IP and NetBEUI" : tcp ? " on TCP/IP" : " on NetBEUI" );
    sl_puts( ".  Ctrl-Break to stop.\r\n" );
    sl_flush();

    for ( ;; ) {
        if ( tcp ) {
            nh = sl_accept( lh, &peer_ip, &peer_port, nb_row >= 0 ? 20 : 2000 );
            if ( nh ) {
                sl_puts( "SMBSRV: client " ); sl_putip( peer_ip ); sl_puts( " connected.\r\n" ); sl_flush();
                smbsrv_serve( srv_send, srv_recv, (void *)nh, &DOSVFS, &fs, "PMSHARE" );
                sl_sclose( nh );
                sl_puts( "SMBSRV: client done.\r\n" ); sl_flush();
            }
        }
        if ( nb_row >= 0 && nb_state( nbh, &st ) != NBX_LISTEN ) {
            if ( st.state == NBX_UP ) {
                nbconn c;
                sl_memzero( &c, sizeof( c ) );
                c.h = nbh;
                sl_puts( "SMBSRV: NetBEUI client connected.\r\n" ); sl_flush();
                smbsrv_serve( nb_send, nb_recv, (void *)&c, &DOSVFS, &fs, "PMSHARE" );
                sl_puts( "SMBSRV: client done.\r\n" ); sl_flush();
            }
            nbc( NB_HANGUP, &nbh, sizeof( nbh ), &err );
            nbh = nb_listen();
            if ( nbh == 0 ) { sl_puts( "SMBSRV: cannot listen on NetBEUI.\r\n" ); nb_row = -1; }
        }
        if ( sl_kbhit() ) break;
        if ( !tcp && nb_row < 0 ) break;
    }
    if ( tcp ) sl_sclose( lh );
    if ( nb_row >= 0 && nbh ) nbc( NB_HANGUP, &nbh, sizeof( nbh ), &err );
}
