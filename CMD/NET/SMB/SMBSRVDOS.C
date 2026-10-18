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
        while ( *p && *p != '\\' && *p != '/' ) { if ( cl < 79 ) comp[cl++] = *p; p++; }
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

static const srv_vfs DOSVFS = {
    d_open, d_read, d_write, d_close, d_findopen, d_findnext, d_findclose, d_mkdir, d_rmdir, d_unlink
};

/* ====================================================================== */
/* the transport, over the accepted socket                               */
/* ====================================================================== */

static int srv_send( void *ctx, const unsigned char *buf, int len ) { return sl_send( (int)ctx, buf, (u32)len, 15000 ); }
static int srv_recv( void *ctx, unsigned char *buf, int len ) { return sl_recvwait( (int)ctx, buf, (u32)len, 30000 ); }

void __cdecl dos_entry( void )
{
    char **argv;
    int    argc, lh, nh;
    dosfs  fs;
    u32    peer_ip;
    u16    peer_port;

    argc = sl_args( &argv );
    sl_strcpy( fs.root, ( argc >= 1 && argv[0][0] ) ? argv[0] : "C:\\", sizeof( fs.root ) );

    if ( !sl_up( 8000 ) ) { sl_puts( "SMBSRV: the network is not up.\r\n" ); sl_flush(); return; }
    lh = sl_open( SOCK_STREAM );
    if ( lh == 0 ) { sl_puts( "SMBSRV: no socket.\r\n" ); sl_flush(); return; }
    if ( !sl_listen( lh, 139 ) ) { sl_puts( "SMBSRV: cannot listen on 139.\r\n" ); sl_flush(); return; }

    sl_puts( "SMBSRV: sharing " ); sl_puts( fs.root ); sl_puts( " on port 139.  Ctrl-Break to stop.\r\n" );
    sl_flush();

    for ( ;; ) {
        nh = sl_accept( lh, &peer_ip, &peer_port, 2000 );
        if ( nh == 0 ) { if ( sl_kbhit() ) break; continue; }
        sl_puts( "SMBSRV: client " ); sl_putip( peer_ip ); sl_puts( " connected.\r\n" ); sl_flush();
        smbsrv_serve( srv_send, srv_recv, (void *)nh, &DOSVFS, &fs, "PMSHARE" );
        sl_sclose( nh );
        sl_puts( "SMBSRV: client done.\r\n" ); sl_flush();
    }
    sl_sclose( lh );
}
