/*
 * SYS_NT.C - Win32 console host for testing CHKDSK/32 against disk
 * images on a development machine.  Not part of the DOS product.
 *
 *   CHKDSKNT @image [@opt ...] [chkdsk arguments]
 *
 * The image is mounted as drive C: (the current drive) and holds one
 * FAT volume starting at sector 0.  Harness options:
 *
 *   @drive=X        mount the image as drive X: instead
 *   @cwd=\DIR       current directory on the image drive
 *   @bad=L[,L...]   sectors (volume LBA) that fail to read and write
 *   @remote=X       drive X: reports as a network drive
 *   @subst=X        drive X: reports as a SUBSTed drive
 *   @dmy / @ymd     country date order (default month-day-year)
 *   @24h            24-hour clock
 *   @dot            "." as thousands separator
 *   @datesep=C      date separator (default "-", or "." with @dmy)
 *
 * When standard input is not a console, answers read from it are
 * echoed so a transcript looks as it would on screen.  Links with no
 * libraries: kernel32 functions are imported by CHKDSKNT.LNK.
 */
#include "chkdsk.h"

#define WINAPI __stdcall
typedef void *HANDLE;
typedef struct {
    u16 wYear, wMonth, wDayOfWeek, wDay;
    u16 wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

HANDLE WINAPI GetStdHandle( u32 std_handle );
int    WINAPI WriteFile( HANDLE file, const void *buf, u32 len, u32 *written,
                         void *overlapped );
int    WINAPI ReadFile( HANDLE file, void *buf, u32 len, u32 *got,
                        void *overlapped );
u32    WINAPI GetFileType( HANDLE file );
char * WINAPI GetCommandLineA( void );
void   WINAPI ExitProcess( u32 code );
HANDLE WINAPI CreateFileA( const char *name, u32 acc, u32 share, void *sa,
                           u32 disp, u32 flags, HANDLE templ );
u32    WINAPI SetFilePointer( HANDLE file, s32 lo, s32 *hi, u32 method );
void * WINAPI VirtualAlloc( void *addr, u32 size, u32 type, u32 prot );
void   WINAPI GetLocalTime( SYSTEMTIME *now );

#define STD_INPUT   ((u32)-10)
#define STD_OUTPUT  ((u32)-11)
#define STD_ERROR   ((u32)-12)
#define BAD_HANDLE  ((HANDLE)-1)
#define FTYPE_CHAR  2

static char   tail[1024];
static HANDLE img = BAD_HANDLE;
static int    img_ro;
static u32    img_bps = 512;
static int    img_drive = 2;
static int    remote_drive = -1, subst_drive = -1;
static char   img_cwd[260];
static u32    bad[64];
static int    nbad;
static int    date_fmt, time_24;
static char   thou = ',', date_sep;

void __cdecl ntentry( void )
{
    chkdsk_main();
    ExitProcess( 255 );
}

static void err( const char *msg )
{
    sys_write( H_ERR, msg, str_len( msg ) );
}

static int opt_is( const char *tok, const char *name )
{
    return str_icmp( tok, name ) == 0;
}

static u32 parse_u32( const char **pp )
{
    const char *cur = *pp;
    u32 val = 0;

    while ( *cur >= '0' && *cur <= '9' ) {
        val = val * 10 + (u32)(*cur++ - '0');
    }
    *pp = cur;
    return val;
}

static int harness_option( const char *tok )
{
    const char *cur;

    if ( tok[1] == 0 ) {
        return -1;
    }
    if ( str_len( tok ) > 7 && mem_cmp( tok, "@drive=", 7 ) == 0 ) {
        img_drive = ch_upper( tok[7] ) - 'A';
    } else if ( str_len( tok ) > 5 && mem_cmp( tok, "@cwd=", 5 ) == 0 ) {
        cur = tok + 5;
        while ( *cur == '\\' ) {
            cur++;
        }
        str_cpy( img_cwd, cur );
    } else if ( str_len( tok ) > 5 && mem_cmp( tok, "@bad=", 5 ) == 0 ) {
        cur = tok + 5;
        while ( *cur && nbad < 64 ) {
            bad[nbad++] = parse_u32( &cur );
            if ( *cur == ',' ) {
                cur++;
            } else {
                break;
            }
        }
    } else if ( str_len( tok ) > 8 && mem_cmp( tok, "@remote=", 8 ) == 0 ) {
        remote_drive = ch_upper( tok[8] ) - 'A';
    } else if ( str_len( tok ) > 7 && mem_cmp( tok, "@subst=", 7 ) == 0 ) {
        subst_drive = ch_upper( tok[7] ) - 'A';
    } else if ( opt_is( tok, "@dmy" ) ) {
        date_fmt = 1;
    } else if ( opt_is( tok, "@ymd" ) ) {
        date_fmt = 2;
    } else if ( opt_is( tok, "@24h" ) ) {
        time_24 = 1;
    } else if ( opt_is( tok, "@dot" ) ) {
        thou = '.';
    } else if ( str_len( tok ) == 10 && mem_cmp( tok, "@datesep=", 9 ) == 0 ) {
        date_sep = tok[9];
    } else if ( img == BAD_HANDLE ) {
        img = CreateFileA( tok + 1, 0xC0000000ul, 3, NULL, 3, 0x80, NULL );
        if ( img == BAD_HANDLE ) {
            img = CreateFileA( tok + 1, 0x80000000ul, 3, NULL, 3, 0x80, NULL );
            img_ro = 1;
        }
        if ( img == BAD_HANDLE ) {
            return -1;
        }
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
    u8 sec[512];
    u32 got;

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
            err( "CHKDSKNT: bad harness option or image: " );
            err( tok );
            err( "\r\n" );
            return -1;
        }
    }
    if ( img == BAD_HANDLE ) {
        err( "usage: CHKDSKNT @image [@opt...] [drive:][[path]filename] [/F] [/V]\r\n" );
        return -1;
    }
    tail[0] = ' ';
    str_cpy( tail + 1, cur );

    if ( SetFilePointer( img, 0, NULL, 0 ) == 0 &&
         ReadFile( img, sec, 512, &got, NULL ) && got == 512 ) {
        img_bps = RD16( sec + 11 );
        if ( img_bps < 128 || img_bps > 4096 || (img_bps & (img_bps - 1)) ) {
            img_bps = 512;
        }
    }
    return 0;
}

const char *sys_cmdline( void )
{
    return tail;
}

void sys_write( int handle, const char *buf, u32 len )
{
    u32 written;

    WriteFile( GetStdHandle( handle == H_ERR ? STD_ERROR : STD_OUTPUT ), buf,
               len, &written, NULL );
}

int sys_read_line( char *buf, int max )
{
    HANDLE handle = GetStdHandle( STD_INPUT );
    int console = GetFileType( handle ) == FTYPE_CHAR;
    int len = 0, eof = 0;
    u32 got;
    char ch;

    for ( ;; ) {
        if ( !ReadFile( handle, &ch, 1, &got, NULL ) || got == 0 ) {
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
            buf[len++] = ch;
        }
    }
    buf[len] = 0;
    if ( !console ) {
        sys_write( H_OUT, buf, (u32)len );
        sys_write( H_OUT, "\r\n", 2 );
    }
    return (eof && len == 0) ? -1 : len;
}

void sys_exit( int code )
{
    ExitProcess( (u32)code );
}

int sys_get_drive( void )
{
    return img_drive;
}

int sys_drive_type( int drv )
{
    if ( drv == remote_drive ) {
        return DRV_REMOTE;
    }
    if ( drv == subst_drive ) {
        return DRV_SUBST;
    }
    return drv == img_drive ? DRV_LOCAL : DRV_INVALID;
}

int sys_truename_drive( int drv )
{
    (void)drv;
    return -1;
}

int sys_get_cwd( int drv, char *buf, int max )
{
    if ( drv != img_drive || (int)str_len( img_cwd ) >= max ) {
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

static int is_bad( u32 lba, u32 count )
{
    int i;

    for ( i = 0; i < nbad; i++ ) {
        if ( bad[i] >= lba && bad[i] < lba + count ) {
            return 1;
        }
    }
    return 0;
}

static int seek( u32 lba )
{
    U64 off;
    s32 hi;
    u32 pos;

    off.whole = mul32( lba, img_bps );
    hi = (s32)off.parts.hi;
    pos = SetFilePointer( img, (s32)off.parts.lo, &hi, 0 );
    return (pos == 0xFFFFFFFFul && off.parts.lo != 0xFFFFFFFFul) ? -1 : 0;
}

int sys_read_sec( int drv, u32 lba, u32 count, u32 bps, void *buf )
{
    u32 got;

    (void)bps;
    if ( drv != img_drive || is_bad( lba, count ) || seek( lba ) != 0 ) {
        return 1;
    }
    if ( !ReadFile( img, buf, count * img_bps, &got, NULL ) ||
         got != count * img_bps ) {
        return 1;
    }
    return 0;
}

int sys_write_sec( int drv, u32 lba, u32 count, u32 bps, const void *buf,
                   int kind )
{
    u32 put;

    (void)bps;
    (void)kind;
    if ( drv != img_drive || img_ro || is_bad( lba, count ) ||
         seek( lba ) != 0 ) {
        return 1;
    }
    if ( !WriteFile( img, buf, count * img_bps, &put, NULL ) ||
         put != count * img_bps ) {
        return 1;
    }
    return 0;
}

void sys_reset_drive( int drv )
{
    (void)drv;
}

void *sys_mem_alloc( u32 size )
{
    return VirtualAlloc( NULL, size, 0x3000, 4 );
}

void sys_mem_info( u32 *total, u32 *avail )
{
    *total = 655360ul;
    *avail = 598016ul;
}

void sys_get_datetime( u16 *dos_date, u16 *dos_time )
{
    SYSTEMTIME now;

    GetLocalTime( &now );
    *dos_date = (u16)(((now.wYear - 1980) << 9) | (now.wMonth << 5) |
                      now.wDay);
    *dos_time = (u16)((now.wHour << 11) | (now.wMinute << 5) |
                      (now.wSecond / 2));
}

void sys_get_country( COUNTRY *ctry )
{
    ctry->date_fmt = date_fmt;
    ctry->time_24 = time_24;
    ctry->thou_sep = thou;
    if ( date_fmt == 1 ) {
        ctry->date_sep = '.';
    }
    if ( date_sep ) {
        ctry->date_sep = date_sep;
    }
}
