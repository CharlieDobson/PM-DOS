/*
 * SYS_NT.C - Win32 console host for testing SCANDISK against disk
 * images on a development machine.  Not part of the DOS product:
 * MKHOST.CMD builds it as SCANDKNT.EXE, with -dHOSTBUILD and with
 * HOSTFE.C, which says in lines of text what SCANDISK.EXE shows on its
 * screens.
 *
 *   SCANDKNT @image [@image ...] [@opt ...] [scandisk arguments]
 *
 * Each image holds one FAT volume starting at sector 0.  The first is
 * drive C: (the current drive), the next D:, and so on.  Options:
 *
 *   @cwd=\DIR       current directory on the first image
 *   @undo=DIR       a host directory that stands for the root of A: and
 *                   of B: - with it there are two diskette drives, and
 *                   the Undo file is DIR\SCANDISK.UND
 *   @boot=DIR       the root of the boot drive, Z: (where the log is
 *                   saved); the current directory if not given
 *   @ro             open the images read-only: any write fails
 *
 * SCANDISK.INI is read from the current directory.  SDBAD in the
 * environment makes sectors unreadable, as it does on PM-DOS (VOL.C).
 * Answers read from standard input are echoed when it is not a console,
 * so a transcript looks as it would on screen.
 */
#include "scandisk.h"

#define WINAPI __stdcall
typedef void *HANDLE;
typedef struct {
    u16 wYear, wMonth, wDayOfWeek, wDay;
    u16 wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

HANDLE WINAPI GetStdHandle( u32 std_handle );
int    WINAPI WriteFile( HANDLE file, const void *buf, u32 len, u32 *written, void *overlapped );
int    WINAPI ReadFile( HANDLE file, void *buf, u32 len, u32 *got, void *overlapped );
u32    WINAPI GetFileType( HANDLE file );
char * WINAPI GetCommandLineA( void );
void   WINAPI ExitProcess( u32 code );
HANDLE WINAPI CreateFileA( const char *name, u32 acc, u32 share, void *sa, u32 disp, u32 flags,
                           HANDLE templ );
int    WINAPI CloseHandle( HANDLE handle );
u32    WINAPI SetFilePointer( HANDLE file, s32 lo, s32 *hi, u32 method );
u32    WINAPI GetFileSize( HANDLE file, u32 *hi );
int    WINAPI FlushFileBuffers( HANDLE file );
u32    WINAPI GetFileAttributesA( const char *name );
void * WINAPI VirtualAlloc( void *addr, u32 size, u32 type, u32 prot );
void   WINAPI GetLocalTime( SYSTEMTIME *now );
u32    WINAPI GetTickCount( void );
u32    WINAPI GetEnvironmentVariableA( const char *name, char *buf, u32 size );

#define STD_INPUT   ((u32)-10)
#define STD_OUTPUT  ((u32)-11)
#define BAD_HANDLE  ((HANDLE)-1)
#define FTYPE_CHAR  2
#define MAX_IMG     4
#define FIRST_DRIVE 2

static char   tail[1024];
static HANDLE img[MAX_IMG];
static u32    img_bps[MAX_IMG];
static u32    img_base[MAX_IMG];       /* where the volume starts: a partition */
static int    nimg, img_ro;
static char   img_cwd[260];
static char   undo_dir[260], boot_dir[260] = ".";

int main( void )
{
    scandisk_main();
    return 255;
}

static void err( const char *msg )
{
    sys_stdout( msg, str_len( msg ) );
}

static int harness_option( const char *tok )
{
    const char *cur;
    u8 sec[512];
    u32 got;

    if ( tok[1] == 0 ) {
        return -1;
    }
    if ( str_nicmp( tok, "@cwd=", 5 ) == 0 ) {
        for ( cur = tok + 5; *cur == '\\'; cur++ ) {
        }
        str_cpy( img_cwd, cur );
    } else if ( str_nicmp( tok, "@undo=", 6 ) == 0 ) {
        str_cpy( undo_dir, tok + 6 );
    } else if ( str_nicmp( tok, "@boot=", 6 ) == 0 ) {
        str_cpy( boot_dir, tok + 6 );
    } else if ( str_icmp( tok, "@ro" ) == 0 ) {
        img_ro = 1;
    } else if ( nimg < MAX_IMG ) {
        img[nimg] = CreateFileA( tok + 1, img_ro ? 0x80000000ul : 0xC0000000ul, 3, NULL, 3,
                                 0x80, NULL );
        if ( img[nimg] == BAD_HANDLE ) {
            return -1;
        }
        img_bps[nimg] = 512;
        img_base[nimg] = 0;
        if ( SetFilePointer( img[nimg], 0, NULL, 0 ) == 0 &&
             ReadFile( img[nimg], sec, 512, &got, NULL ) && got == 512 ) {
            /* a hard disk image: the volume is its first partition */
            if ( !(RD16( sec + 11 ) == 512 && sec[13] && RD16( sec + 14 ) && sec[16]) &&
                 sec[510] == 0x55 && sec[511] == 0xAA && sec[446 + 4] != 0 ) {
                img_base[nimg] = RD32( sec + 446 + 8 );
            } else {
                img_bps[nimg] = RD16( sec + 11 );
                if ( img_bps[nimg] < 128 || img_bps[nimg] > 4096 ||
                     (img_bps[nimg] & (img_bps[nimg] - 1)) ) {
                    img_bps[nimg] = 512;
                }
            }
        }
        nimg++;
    } else {
        return -1;
    }
    return 0;
}

int sys_init( void )
{
    const char *cur = GetCommandLineA();
    char tok[300];
    int len;

    /* skip the program name */
    if ( *cur == '"' ) {
        for ( cur++; *cur && *cur != '"'; cur++ ) {
        }
        if ( *cur ) {
            cur++;
        }
    } else {
        while ( *cur && *cur != ' ' && *cur != '\t' ) {
            cur++;
        }
    }
    /* leading @options belong to the harness */
    for ( ;; ) {
        while ( *cur == ' ' || *cur == '\t' ) {
            cur++;
        }
        if ( *cur != '@' ) {
            break;
        }
        for ( len = 0; *cur && *cur != ' ' && *cur != '\t' && len < 299; ) {
            tok[len++] = *cur++;
        }
        tok[len] = 0;
        if ( harness_option( tok ) != 0 ) {
            err( "SCANDKNT: bad harness option or image: " );
            err( tok );
            err( "\r\n" );
            return -1;
        }
    }
    if ( nimg == 0 ) {
        err( "usage: SCANDKNT @image [@image...] [@opt...] [scandisk arguments]\r\n" );
        return -1;
    }
    tail[0] = ' ';
    str_cpy( tail + 1, cur );
    return 0;
}

const char *sys_cmdline( void )
{
    return tail;
}

const char *sys_getenv( const char *name )
{
    static char value[4][512];
    static int next;
    char *buf = value[next];
    u32 len = GetEnvironmentVariableA( name, buf, 512 );

    if ( len == 0 || len >= 512 ) {
        return NULL;
    }
    next = (next + 1) & 3;
    return buf;
}

void sys_exit( int code )
{
    ExitProcess( (u32)code );
}

void *sys_mem_alloc( u32 size )
{
    return VirtualAlloc( NULL, size, 0x3000, 4 );
}

void sys_break_install( void )
{
}

int sys_break_take( void )
{
    return 0;
}

void sys_stdout( const char *buf, u32 len )
{
    u32 written;

    WriteFile( GetStdHandle( STD_OUTPUT ), buf, len, &written, NULL );
}

int sys_stdin_is_console( void )
{
    return GetFileType( GetStdHandle( STD_INPUT ) ) == FTYPE_CHAR;
}

int sys_stdin_byte( void )
{
    u32 got;
    u8 ch;

    if ( !ReadFile( GetStdHandle( STD_INPUT ), &ch, 1, &got, NULL ) || got == 0 ) {
        return -1;
    }
    return ch;
}

int sys_read_line( char *buf, int max )
{
    int console = sys_stdin_is_console(), len = 0, eof = 0, ch;

    for ( ;; ) {
        ch = sys_stdin_byte();
        if ( ch < 0 ) {
            eof = 1;
            break;
        }
        if ( ch == '\r' ) {
            continue;
        }
        if ( ch == '\n' ) {
            break;
        }
        if ( len < max - 1 ) {
            buf[len++] = (char)ch;
        }
    }
    buf[len] = 0;
    if ( !console ) {
        sys_stdout( buf, (u32)len );
        sys_stdout( "\r\n", 2 );
    }
    return (eof && len == 0) ? -1 : len;
}

u32 sys_hundredths( void )
{
    return GetTickCount() / 10 % 8640000ul;
}

void sys_get_datetime( u16 *dos_date, u16 *dos_time )
{
    SYSTEMTIME now;

    GetLocalTime( &now );
    *dos_date = (u16)(((now.wYear - 1980) << 9) | (now.wMonth << 5) | now.wDay);
    *dos_time = (u16)((now.wHour << 11) | (now.wMinute << 5) | (now.wSecond / 2));
}

void sys_get_country( COUNTRY *ctry )
{
    (void)ctry;
}

/* ------------------------------------------------------------------ */
/* drives                                                              */
/* ------------------------------------------------------------------ */

static int img_of( int drv )
{
    return drv >= FIRST_DRIVE && drv < FIRST_DRIVE + nimg ? drv - FIRST_DRIVE : -1;
}

int sys_get_drive( void )
{
    return FIRST_DRIVE;
}

int sys_boot_drive( void )
{
    return 25;
}

int sys_drive_type( int drv )
{
    return img_of( drv ) >= 0 ? DRV_LOCAL : DRV_INVALID;
}

int sys_drive_fixed( int drv )
{
    return img_of( drv ) >= 0;
}

int sys_drive_floppy( int drv )
{
    return (drv == 0 || drv == 1) && undo_dir[0] != 0;
}

int sys_truename_drive( int drv )
{
    (void)drv;
    return -1;
}

int sys_get_cwd( int drv, char *buf, int max )
{
    if ( drv != FIRST_DRIVE || (int)str_len( img_cwd ) >= max ) {
        return -1;
    }
    str_cpy( buf, img_cwd );
    return 0;
}

int sys_get_bpb( int drv, u8 *sector )
{
    (void)drv;
    (void)sector;
    return -1;
}

int sys_lock( int drv, int fat32 )
{
    (void)drv;
    (void)fat32;
    return 0;
}

void sys_unlock( int drv, int fat32 )
{
    (void)drv;
    (void)fat32;
}

static int seek_sec( int which, u32 lba )
{
    U64 off;
    s32 hi;
    u32 pos;

    off.whole = mul32( lba + img_base[which], img_bps[which] );
    hi = (s32)off.parts.hi;
    pos = SetFilePointer( img[which], (s32)off.parts.lo, &hi, 0 );
    return (pos == 0xFFFFFFFFul && off.parts.lo != 0xFFFFFFFFul) ? -1 : 0;
}

int sys_read_sec( int drv, u32 lba, u32 count, u32 bps, void *buf )
{
    int which = img_of( drv );
    u32 got;

    (void)bps;
    if ( which < 0 || seek_sec( which, lba ) != 0 ) {
        return 27;
    }
    if ( !ReadFile( img[which], buf, count * img_bps[which], &got, NULL ) ||
         got != count * img_bps[which] ) {
        return 27;
    }
    return 0;
}

int sys_write_sec( int drv, u32 lba, u32 count, u32 bps, const void *buf, int kind )
{
    int which = img_of( drv );
    u32 put;

    (void)bps;
    (void)kind;
    if ( which < 0 || img_ro || seek_sec( which, lba ) != 0 ) {
        return 19;
    }
    if ( !WriteFile( img[which], buf, count * img_bps[which], &put, NULL ) ||
         put != count * img_bps[which] ) {
        return 29;
    }
    return 0;
}

void sys_reset_drive( int drv )
{
    (void)drv;
}

void sys_mark_checked( int drv )
{
    (void)drv;
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

/* "A:\NAME" and "B:\NAME" are in the Undo directory, "Z:\NAME" in the
   boot directory; anything else is the host's own path */
static int host_path( const char *path, char *out )
{
    const char *dir = NULL;

    if ( path[0] && path[1] == ':' && path[2] == '\\' ) {
        if ( (ch_upper( path[0] ) == 'A' || ch_upper( path[0] ) == 'B') && undo_dir[0] ) {
            dir = undo_dir;
        } else if ( ch_upper( path[0] ) == 'Z' ) {
            dir = boot_dir;
        } else {
            return -1;
        }
        sfmt( out, 300, "%s\\%s", dir, path + 3 );
        return 0;
    }
    str_cpyn( out, path, 300 );
    return 0;
}

static int open_as( const char *path, u32 access, u32 disp, u32 *handle )
{
    char host[300];
    HANDLE file;

    if ( host_path( path, host ) != 0 ) {
        return 3;
    }
    file = CreateFileA( host, access, 3, NULL, disp, 0x80, NULL );
    if ( file == BAD_HANDLE ) {
        return 2;
    }
    *handle = (u32)file;
    return 0;
}

int sys_open_read( const char *path, u32 *handle )
{
    return open_as( path, 0x80000000ul, 3, handle );
}

int sys_open_rw( const char *path, u32 *handle )
{
    return open_as( path, 0xC0000000ul, 3, handle );
}

int sys_create( const char *path, u32 *handle )
{
    return open_as( path, 0xC0000000ul, 2, handle );
}

int sys_read( u32 handle, void *buf, u32 len, u32 *done )
{
    return ReadFile( (HANDLE)handle, buf, len, done, NULL ) ? 0 : 30;
}

int sys_write( u32 handle, const void *buf, u32 len, u32 *done )
{
    if ( !WriteFile( (HANDLE)handle, buf, len, done, NULL ) ) {
        return 29;
    }
    return *done < len ? 112 : 0;
}

int sys_close( u32 handle )
{
    return CloseHandle( (HANDLE)handle ) ? 0 : 6;
}

int sys_file_size( u32 handle, u32 *size )
{
    *size = GetFileSize( (HANDLE)handle, NULL );
    SetFilePointer( (HANDLE)handle, 0, NULL, 0 );
    return *size == 0xFFFFFFFFul ? 5 : 0;
}

int sys_seek( u32 handle, u32 pos )
{
    return SetFilePointer( (HANDLE)handle, (s32)pos, NULL, 0 ) == 0xFFFFFFFFul ? 25 : 0;
}

int sys_seek_end( u32 handle )
{
    return SetFilePointer( (HANDLE)handle, 0, NULL, 2 ) == 0xFFFFFFFFul ? 25 : 0;
}

int sys_commit( u32 handle )
{
    return FlushFileBuffers( (HANDLE)handle ) ? 0 : 29;
}

int sys_exists( const char *path )
{
    char host[300];

    return host_path( path, host ) == 0 && GetFileAttributesA( host ) != 0xFFFFFFFFul;
}
