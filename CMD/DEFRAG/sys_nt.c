/*
 * SYS_NT.C - Win32 console host for testing DEFRAG against disk images
 * on a development machine.  Not part of the DOS product: MKHOST.CMD
 * builds it as DEFRAGNT.EXE, with -dHOSTBUILD and with HOSTFE.C, which
 * says in lines of text what DEFRAG.EXE shows on its screen.
 *
 *   DEFRAGNT @image [defrag arguments]
 *
 * The image holds one FAT volume, at sector 0 or in the first partition
 * of a hard disk image.  It is drive C:, the current drive.
 *
 *   @ro         open the image read-only: any write fails
 *
 * THE POWER CUT.  DFCRASH=n in the environment ends the program at its
 * n-th write of sectors, before the write is made - and DFTORN=k lets
 * the first k sectors of that write through first.  Every move DEFRAG
 * makes is meant to leave the drive whole wherever it is stopped
 * (MOVE.C), and this is how that is tried: run after run, each stopped
 * one write later than the last, and the image looked at after each.
 * The exit code is then 99.
 *
 * DFBAD in the environment makes sectors unreadable, as it does on
 * PM-DOS (VOL.C).
 */
#include "defrag.h"

#define WINAPI __stdcall
typedef void *HANDLE;

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
u32    WINAPI GetFileAttributesA( const char *name );
void * WINAPI VirtualAlloc( void *addr, u32 size, u32 type, u32 prot );
u32    WINAPI GetTickCount( void );
u32    WINAPI GetEnvironmentVariableA( const char *name, char *buf, u32 size );

#define STD_INPUT   ((u32)-10)
#define STD_OUTPUT  ((u32)-11)
#define BAD_HANDLE  ((HANDLE)-1)
#define FTYPE_CHAR  2
#define THE_DRIVE   2

static char   tail[1024];
static HANDLE img = BAD_HANDLE;
static u32    img_bps = 512;
static u32    img_base;                 /* where the volume starts: a partition */
static int    img_ro;
static u32    crash_at, torn, writes;
static int    write_log;                /* DFWLOG: every write is said */

int main( void )
{
    defrag_main();
    return 255;
}

static void err( const char *msg )
{
    sys_stdout( msg, str_len( msg ) );
}

static u32 env_number( const char *name )
{
    const char *text = sys_getenv( name );
    u32 val = 0;

    for ( ; text && *text >= '0' && *text <= '9'; text++ ) {
        val = val * 10 + (u32)(*text - '0');
    }
    return val;
}

static int harness_option( const char *tok )
{
    u8 sec[512];
    u32 got;

    if ( tok[1] == 0 ) {
        return -1;
    }
    if ( str_icmp( tok, "@ro" ) == 0 ) {
        img_ro = 1;
        return 0;
    }
    if ( img != BAD_HANDLE ) {
        return -1;
    }
    img = CreateFileA( tok + 1, img_ro ? 0x80000000ul : 0xC0000000ul, 3, NULL, 3, 0x80, NULL );
    if ( img == BAD_HANDLE ) {
        return -1;
    }
    if ( SetFilePointer( img, 0, NULL, 0 ) == 0 && ReadFile( img, sec, 512, &got, NULL ) && got == 512 ) {
        /* a hard disk image: the volume is its first partition */
        if ( !(RD16( sec + 11 ) == 512 && sec[13] && RD16( sec + 14 ) && sec[16]) &&
             sec[510] == 0x55 && sec[511] == 0xAA && sec[446 + 4] != 0 ) {
            img_base = RD32( sec + 446 + 8 );
        } else {
            img_bps = RD16( sec + 11 );
            if ( img_bps < 128 || img_bps > 4096 || (img_bps & (img_bps - 1)) ) {
                img_bps = 512;
            }
        }
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
            err( "DEFRAGNT: bad harness option or image: " );
            err( tok );
            err( "\r\n" );
            return -1;
        }
    }
    if ( img == BAD_HANDLE ) {
        err( "usage: DEFRAGNT @image [defrag arguments]\r\n" );
        return -1;
    }
    tail[0] = ' ';
    str_cpy( tail + 1, cur );
    crash_at = env_number( "DFCRASH" );
    torn = env_number( "DFTORN" );
    write_log = env_number( "DFWLOG" ) != 0;
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
    return -1;
}

int sys_handle_is_file( int handle )
{
    (void)handle;
    return 0;
}

int sys_exec( const char *prog, const char *args, int *error )
{
    (void)prog;
    (void)args;
    *error = 2;
    return -1;
}

void sys_restart( void )
{
}

u32 sys_hundredths( void )
{
    return GetTickCount() / 10 % 8640000ul;
}

void sys_get_country( COUNTRY *ctry )
{
    (void)ctry;
}

/* ------------------------------------------------------------------ */
/* drives                                                              */
/* ------------------------------------------------------------------ */

int sys_get_drive( void )
{
    return THE_DRIVE;
}

int sys_boot_drive( void )
{
    return 25;
}

int sys_drive_kind( int drv )
{
    return drv == THE_DRIVE ? DK_FIXED : DK_NONE;
}

int sys_get_cwd( int drv, char *buf, int max )
{
    (void)drv;
    (void)max;
    buf[0] = 0;
    return 0;
}

int sys_chdir( int drv, const char *dir )
{
    (void)drv;
    (void)dir;
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

int sys_vol_flags( int drv )
{
    (void)drv;
    return 0;
}

static int seek_sec( u32 lba )
{
    U64 off;
    s32 hi;
    u32 pos;

    off.whole = mul32( lba + img_base, img_bps );
    hi = (s32)off.parts.hi;
    pos = SetFilePointer( img, (s32)off.parts.lo, &hi, 0 );
    return (pos == 0xFFFFFFFFul && off.parts.lo != 0xFFFFFFFFul) ? -1 : 0;
}

int sys_read_sec( int drv, u32 lba, u32 count, u32 bps, void *buf )
{
    u32 got;

    (void)bps;
    if ( drv != THE_DRIVE || seek_sec( lba ) != 0 ) {
        return 27;
    }
    if ( !ReadFile( img, buf, count * img_bps, &got, NULL ) || got != count * img_bps ) {
        return 27;
    }
    return 0;
}

int sys_write_sec( int drv, u32 lba, u32 count, u32 bps, const void *buf, int kind )
{
    u32 put;

    (void)bps;
    (void)kind;
    if ( drv != THE_DRIVE || img_ro || seek_sec( lba ) != 0 ) {
        return 19;
    }
    writes++;
    if ( write_log ) {
        char line[80];

        sfmt( line, sizeof( line ), "  write %u: sector %u, %u\r\n", writes, lba, count );
        sys_stdout( line, str_len( line ) );
    }
    /* the power goes: at this write, with "torn" sectors of it made */
    if ( crash_at != 0 && writes >= crash_at ) {
        if ( torn > count ) {
            torn = count;
        }
        if ( torn ) {
            WriteFile( img, buf, torn * img_bps, &put, NULL );
        }
        CloseHandle( img );
        ExitProcess( 99 );
    }
    if ( !WriteFile( img, buf, count * img_bps, &put, NULL ) || put != count * img_bps ) {
        return 29;
    }
    return 0;
}

void sys_reset_drive( int drv )
{
    (void)drv;
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

int sys_create( const char *path, u32 *handle )
{
    HANDLE file = CreateFileA( path, 0xC0000000ul, 3, NULL, 2, 0x80, NULL );

    if ( file == BAD_HANDLE ) {
        return 2;
    }
    *handle = (u32)file;
    return 0;
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

int sys_exists( const char *path )
{
    return GetFileAttributesA( path ) != 0xFFFFFFFFul;
}
