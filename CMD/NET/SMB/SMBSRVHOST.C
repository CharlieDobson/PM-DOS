/* ==========================================================================
 * SMBSRVHOST.C - prove SMBSRV.C on the host: a Win32 back end (files via
 * the CRT, enumeration via FindFirstFile) under the server core, over
 * Winsock, so the proven PM-DOS SMB1 client (SMB1HOST) can mount it.
 *   wcl386 -q -bt=nt -fe=smbsrvhost.exe smbsrvhost.c smbsrv.c wsock32.lib
 *   smbsrvhost.exe [port] [rootdir]      (default 8446 .)
 * ========================================================================== */
#include "smbsrv.h"
#include <winsock.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

static SOCKET g_sock;
static int io_send( void *ctx, const unsigned char *buf, int len ) { (void)ctx; return send( g_sock, (const char *)buf, len, 0 ); }
static int io_recv( void *ctx, unsigned char *buf, int len ) { (void)ctx; return recv( g_sock, (char *)buf, len, 0 ); }

typedef struct { char root[260]; FILE *fh[16]; } hostfs;

/* join the served root with a share-relative, backslash path; reject ".." */
static int joinpath( hostfs *fs, const char *rel, char *out )
{
    int n = 0, i, started;
    const char *p = rel;
    for ( i = 0; fs->root[i]; i++ ) out[n++] = fs->root[i];
    while ( *p == '\\' || *p == '/' ) p++;
    started = 1;
    while ( *p ) {
        char comp[260]; int cl = 0;
        while ( *p && *p != '\\' && *p != '/' ) { if ( cl < 259 ) comp[cl++] = *p; p++; }
        comp[cl] = 0;
        while ( *p == '\\' || *p == '/' ) p++;
        if ( cl == 0 ) continue;
        if ( comp[0] == '.' && comp[1] == 0 ) continue;
        if ( comp[0] == '.' && comp[1] == '.' && comp[2] == 0 ) return 0;   /* unsafe */
        out[n++] = '\\';
        for ( i = 0; i < cl; i++ ) out[n++] = comp[i];
        (void)started;
    }
    out[n] = 0;
    return 1;
}

static int h_open( void *ctx, const char *path, int write, int dispo, unsigned long *size, int *action )
{
    hostfs *fs = (hostfs *)ctx;
    char full[520];
    FILE *f = (FILE *)0;
    int slot, existed;

    if ( !joinpath( fs, path, full ) ) return -1;
    for ( slot = 0; slot < 16; slot++ ) if ( fs->fh[slot] == (FILE *)0 ) break;
    if ( slot == 16 ) return -1;

    existed = ( GetFileAttributesA( full ) != 0xFFFFFFFFUL );
    if ( dispo == 5 )        { f = fopen( full, "w+b" ); *action = existed ? 3 : 2; }
    else if ( dispo == 3 )   { f = existed ? fopen( full, "r+b" ) : fopen( full, "w+b" ); *action = existed ? 1 : 2; }
    else                     { f = fopen( full, write ? "r+b" : "rb" ); *action = 1; }
    (void)write;
    if ( f == (FILE *)0 ) return -1;

    fseek( f, 0, SEEK_END );
    *size = (unsigned long)ftell( f );
    fseek( f, 0, SEEK_SET );
    fs->fh[slot] = f;
    return slot;
}
static int h_read( void *ctx, int h, unsigned long off, void *buf, int len )
{
    hostfs *fs = (hostfs *)ctx;
    if ( h < 0 || h >= 16 || !fs->fh[h] ) return -1;
    fseek( fs->fh[h], (long)off, SEEK_SET );
    return (int)fread( buf, 1, (size_t)len, fs->fh[h] );
}
static int h_write( void *ctx, int h, unsigned long off, const void *buf, int len )
{
    hostfs *fs = (hostfs *)ctx;
    if ( h < 0 || h >= 16 || !fs->fh[h] ) return -1;
    fseek( fs->fh[h], (long)off, SEEK_SET );
    return (int)fwrite( buf, 1, (size_t)len, fs->fh[h] );
}
static void h_close( void *ctx, int h )
{
    hostfs *fs = (hostfs *)ctx;
    if ( h >= 0 && h < 16 && fs->fh[h] ) { fclose( fs->fh[h] ); fs->fh[h] = (FILE *)0; }
}

/* a FILETIME -> the entry's DOS date and time (local, as DOS keeps them) */
static void dos_stamp( const FILETIME *ft, srv_dirent *de )
{
    FILETIME lft;
    WORD date = 0, time = 0;
    FileTimeToLocalFileTime( ft, &lft );
    FileTimeToDosDateTime( &lft, &date, &time );
    de->dosdate = date;
    de->dostime = time;
}

typedef struct { HANDLE h; WIN32_FIND_DATAA fd; int first, done; } hiter;
static hiter g_iter;

static void *h_findopen( void *ctx, const char *dir )
{
    hostfs *fs = (hostfs *)ctx;
    char full[520];
    int n;
    if ( !joinpath( fs, dir, full ) ) return (void *)0;
    n = (int)strlen( full );
    if ( n == 0 || full[n - 1] != '\\' ) full[n++] = '\\';
    full[n++] = '*'; full[n] = 0;
    g_iter.h = FindFirstFileA( full, &g_iter.fd );
    if ( g_iter.h == INVALID_HANDLE_VALUE ) return (void *)0;
    g_iter.first = 1; g_iter.done = 0;
    return &g_iter;
}
static int h_findnext( void *ctx, void *it, srv_dirent *de )
{
    hiter *i = (hiter *)it;
    (void)ctx;
    if ( i->done ) return 0;
    if ( !i->first ) { if ( !FindNextFileA( i->h, &i->fd ) ) { i->done = 1; return 0; } }
    i->first = 0;
    { int k; for ( k = 0; i->fd.cFileName[k] && k < 259; k++ ) de->name[k] = i->fd.cFileName[k]; de->name[k] = 0; }
    de->attr = ( i->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) ? 0x10UL : 0x20UL;
    de->size = i->fd.nFileSizeLow;
    de->ft_lo = i->fd.ftLastWriteTime.dwLowDateTime;
    de->ft_hi = i->fd.ftLastWriteTime.dwHighDateTime;
    dos_stamp( &i->fd.ftLastWriteTime, de );
    return 1;
}
static void h_findclose( void *ctx, void *it ) { hiter *i = (hiter *)it; (void)ctx; if ( i->h != INVALID_HANDLE_VALUE ) FindClose( i->h ); }

static int h_mkdir( void *ctx, const char *path ) { char f[520]; if ( !joinpath( (hostfs *)ctx, path, f ) ) return -1; return CreateDirectoryA( f, 0 ) ? 0 : -1; }
static int h_rmdir( void *ctx, const char *path ) { char f[520]; if ( !joinpath( (hostfs *)ctx, path, f ) ) return -1; return RemoveDirectoryA( f ) ? 0 : -1; }
static int h_unlink( void *ctx, const char *path ) { char f[520]; if ( !joinpath( (hostfs *)ctx, path, f ) ) return -1; return DeleteFileA( f ) ? 0 : -1; }

static int h_stat( void *ctx, const char *path, srv_dirent *de )
{
    char f[520];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    memset( de, 0, sizeof( *de ) );
    if ( !joinpath( (hostfs *)ctx, path, f ) ) return -1;
    if ( !GetFileAttributesExA( f, GetFileExInfoStandard, &fa ) ) return -1;
    de->attr = fa.dwFileAttributes & 0x37;
    de->size = fa.nFileSizeLow;
    de->ft_lo = fa.ftLastWriteTime.dwLowDateTime;
    de->ft_hi = fa.ftLastWriteTime.dwHighDateTime;
    dos_stamp( &fa.ftLastWriteTime, de );
    return 0;
}
static int h_rename( void *ctx, const char *from, const char *to )
{
    char f[520], t[520];
    if ( !joinpath( (hostfs *)ctx, from, f ) || !joinpath( (hostfs *)ctx, to, t ) ) return -1;
    return MoveFileA( f, t ) ? 0 : -1;
}
static int h_setattr( void *ctx, const char *path, unsigned int attr )
{
    char f[520];
    if ( !joinpath( (hostfs *)ctx, path, f ) ) return -1;
    return SetFileAttributesA( f, attr ? attr : FILE_ATTRIBUTE_NORMAL ) ? 0 : -1;
}
static int h_fstat( void *ctx, int h, srv_dirent *de )
{
    hostfs *fs = (hostfs *)ctx;
    FILETIME ft;
    HANDLE oh;
    memset( de, 0, sizeof( *de ) );
    if ( h < 0 || h >= 16 || !fs->fh[h] ) return -1;
    fflush( fs->fh[h] );
    oh = (HANDLE)_get_osfhandle( _fileno( fs->fh[h] ) );
    de->size = GetFileSize( oh, (DWORD *)0 );
    if ( GetFileTime( oh, (FILETIME *)0, (FILETIME *)0, &ft ) ) dos_stamp( &ft, de );
    de->attr = 0x20;
    return 0;
}
static int h_settime( void *ctx, int h, unsigned int date, unsigned int time )
{
    hostfs *fs = (hostfs *)ctx;
    FILETIME lft, ft;
    if ( h < 0 || h >= 16 || !fs->fh[h] ) return -1;
    fflush( fs->fh[h] );
    if ( !DosDateTimeToFileTime( (WORD)date, (WORD)time, &lft ) || !LocalFileTimeToFileTime( &lft, &ft ) ) return -1;
    return SetFileTime( (HANDLE)_get_osfhandle( _fileno( fs->fh[h] ) ), (FILETIME *)0, (FILETIME *)0, &ft ) ? 0 : -1;
}
static int h_disk( void *ctx, unsigned long *units, unsigned long *avail, unsigned long *unitsize )
{
    DWORD spc, bps, freec, totalc;
    if ( !GetDiskFreeSpaceA( ( (hostfs *)ctx )->root[0] ? ( (hostfs *)ctx )->root : (const char *)0, &spc, &bps, &freec, &totalc ) ) {
        if ( !GetDiskFreeSpaceA( (const char *)0, &spc, &bps, &freec, &totalc ) ) return -1;
    }
    *units = totalc; *avail = freec; *unitsize = spc * bps;
    return 0;
}

static const srv_vfs HOSTFS = {
    h_open, h_read, h_write, h_close, h_findopen, h_findnext, h_findclose, h_mkdir, h_rmdir, h_unlink,
    h_stat, h_rename, h_setattr, h_fstat, h_settime, h_disk
};

int main( int argc, char **argv )
{
    WSADATA wsa;
    SOCKET lis;
    struct sockaddr_in sa;
    int port = ( argc > 1 ) ? atoi( argv[1] ) : 8446;
    hostfs fs;

    memset( &fs, 0, sizeof( fs ) );
    strcpy( fs.root, ( argc > 2 ) ? argv[2] : "." );

    if ( WSAStartup( 0x0101, &wsa ) != 0 ) { printf( "WSAStartup failed\n" ); return 1; }
    lis = socket( AF_INET, SOCK_STREAM, 0 );
    { int one = 1; setsockopt( lis, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof( one ) ); }
    memset( &sa, 0, sizeof( sa ) );
    sa.sin_family = AF_INET;
    sa.sin_port = htons( (unsigned short)port );
    sa.sin_addr.s_addr = inet_addr( "127.0.0.1" );
    if ( bind( lis, (struct sockaddr *)&sa, sizeof( sa ) ) != 0 ) { printf( "bind %d failed (%d)\n", port, WSAGetLastError() ); return 1; }
    listen( lis, 4 );
    printf( "smbsrvhost: serving %s on 127.0.0.1:%d\n", fs.root, port );
    fflush( stdout );

    for ( ;; ) {
        int sl = sizeof( sa );
        g_sock = accept( lis, (struct sockaddr *)&sa, &sl );
        if ( g_sock == INVALID_SOCKET ) break;
        smbsrv_serve( io_send, io_recv, (void *)0, &HOSTFS, &fs, "PMSHARE" );
        closesocket( g_sock );
    }
    return 0;
}
