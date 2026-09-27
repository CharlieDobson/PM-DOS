/*
 * SYS_NT.C - Win32 console host for testing LABEL/32 against disk
 * images on a development machine.  Not part of the DOS product.
 *
 *   LABELNT @image [@opt ...] [label arguments]
 *
 * The image is mounted as drive C: (the current drive) and holds one
 * FAT volume starting at sector 0.  Harness options:
 *
 *   @drive=X        mount the image as drive X: instead
 *   @bad=L[,L...]   sectors (volume LBA) that fail: reads with "Data
 *                   error", writes with "Write fault error"
 *   @wbad=L[,L...]  sectors that fail only when written
 *   @ro             the disk is write-protected
 *   @notready       every transfer fails with "Not ready"
 *   @nolock         the volume lock is refused
 *   @remote=X       drive X: reports as a network drive
 *   @subst=X        drive X: reports as a SUBSTed drive
 *   @swap=image     the disk is changed for this image while LABEL waits
 *                   at its first prompt
 *
 * Characters from 80h up are capitalized with the MS-DOS built-in
 * table for code page 437, as INT 21h/6520h does on a US system.
 *
 * When standard input is not a console, answers read from it are
 * echoed so a transcript looks as it would on screen.  Links with no
 * libraries: kernel32 functions are imported by LABELNT.LNK.
 */
#include "label.h"

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
int    WINAPI CloseHandle( HANDLE file );
u32    WINAPI SetFilePointer( HANDLE file, s32 lo, s32 *hi, u32 method );
void * WINAPI VirtualAlloc( void *addr, u32 size, u32 type, u32 prot );
void   WINAPI GetLocalTime( SYSTEMTIME *now );

#define STD_INPUT   ((u32)-10)
#define STD_OUTPUT  ((u32)-11)
#define STD_ERROR   ((u32)-12)
#define BAD_HANDLE  ((HANDLE)-1)
#define FTYPE_CHAR  2
#define MAX_BAD     64

static char   tail[1024];
static HANDLE img = BAD_HANDLE;
static int    img_ro;
static u32    img_bps = 512;
static int    img_drive = 2;
static int    remote_drive = -1, subst_drive = -1;
static int    not_ready, no_lock;
static u32    bad[MAX_BAD], wbad[MAX_BAD];
static int    nbad, nwbad;
static char   swap_path[260];

/* MS-DOS code page 437 upper case table, 80h-AFh (B0h-FFh unchanged) */
static const u8 ucase437[48] = {
    0x80, 0x9A, 'E',  'A',  0x8E, 'A',  0x8F, 0x80,
    'E',  'E',  'E',  'I',  'I',  'I',  0x8E, 0x8F,
    0x90, 0x92, 0x92, 'O',  0x99, 'O',  'U',  'U',
    'Y',  0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
    'A',  'I',  'O',  'U',  0xA5, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF
};

void __cdecl ntentry( void )
{
    label_main();
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

static int opt_val( const char *tok, const char *name )
{
    u32 len = str_len( name );
    return str_len( tok ) > len && mem_cmp( tok, name, len ) == 0;
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

static void parse_list( const char *cur, u32 *list, int *count )
{
    while ( *cur && *count < MAX_BAD ) {
        list[(*count)++] = parse_u32( &cur );
        if ( *cur == ',' ) {
            cur++;
        } else {
            break;
        }
    }
}

/* open an image read/write (read-only if that fails) and take its
   sector size from its BPB */
static int open_image( const char *path )
{
    u8 sec[512];
    u32 got;

    img = CreateFileA( path, 0xC0000000ul, 3, NULL, 3, 0x80, NULL );
    if ( img == BAD_HANDLE ) {
        img = CreateFileA( path, 0x80000000ul, 3, NULL, 3, 0x80, NULL );
        img_ro = 1;
    }
    if ( img == BAD_HANDLE ) {
        return -1;
    }
    img_bps = 512;
    if ( SetFilePointer( img, 0, NULL, 0 ) == 0 &&
         ReadFile( img, sec, 512, &got, NULL ) && got == 512 ) {
        img_bps = RD16( sec + 11 );
        if ( img_bps < 128 || img_bps > 4096 || (img_bps & (img_bps - 1)) ) {
            img_bps = 512;
        }
    }
    return 0;
}

/* @swap: the user changes the disk while a prompt waits */
static void change_disk( void )
{
    if ( swap_path[0] == 0 ) {
        return;
    }
    CloseHandle( img );
    if ( open_image( swap_path ) != 0 ) {
        err( "LABELNT: cannot open swap image\r\n" );
        ExitProcess( 255 );
    }
    swap_path[0] = 0;
}

static int harness_option( const char *tok )
{
    if ( tok[1] == 0 ) {
        return -1;
    }
    if ( opt_val( tok, "@swap=" ) &&
         str_len( tok ) < sizeof( swap_path ) + 6 ) {
        str_cpy( swap_path, tok + 6 );
    } else if ( opt_val( tok, "@drive=" ) ) {
        img_drive = ch_upper( tok[7] ) - 'A';
    } else if ( opt_val( tok, "@bad=" ) ) {
        parse_list( tok + 5, bad, &nbad );
    } else if ( opt_val( tok, "@wbad=" ) ) {
        parse_list( tok + 6, wbad, &nwbad );
    } else if ( opt_val( tok, "@remote=" ) ) {
        remote_drive = ch_upper( tok[8] ) - 'A';
    } else if ( opt_val( tok, "@subst=" ) ) {
        subst_drive = ch_upper( tok[7] ) - 'A';
    } else if ( opt_is( tok, "@ro" ) ) {
        img_ro = 1;
    } else if ( opt_is( tok, "@notready" ) ) {
        not_ready = 1;
    } else if ( opt_is( tok, "@nolock" ) ) {
        no_lock = 1;
    } else if ( img == BAD_HANDLE ) {
        return open_image( tok + 1 );
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
            err( "LABELNT: bad harness option or image: " );
            err( tok );
            err( "\r\n" );
            return -1;
        }
    }
    if ( img == BAD_HANDLE ) {
        err( "usage: LABELNT @image [@opt...] [drive:][label]\r\n" );
        return -1;
    }
    /* DOS puts the blank after the command name in the tail too */
    tail[0] = ' ';
    str_cpy( tail + 1, cur );
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

static HANDLE in_handle( int *console )
{
    HANDLE handle = GetStdHandle( STD_INPUT );

    *console = GetFileType( handle ) == FTYPE_CHAR;
    return handle;
}

int sys_read_line( char *buf, int max )
{
    int console, len = 0, eof = 0;
    HANDLE handle = in_handle( &console );
    u32 got;
    char ch;

    change_disk();
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

/* the console is in line mode, so there a key is the first character
   of a line; either way line ends are skipped */
int sys_read_key( void )
{
    int console;
    HANDLE handle = in_handle( &console );
    u32 got;
    u8 ch;

    change_disk();
    do {
        if ( !ReadFile( handle, &ch, 1, &got, NULL ) || got == 0 ) {
            return -1;
        }
    } while ( ch == '\r' || ch == '\n' );
    if ( !console && ch >= ' ' ) {
        sys_write( H_OUT, (const char *)&ch, 1 );
    }
    return ch;
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
    return no_lock ? -1 : 0;
}

void sys_unlock( int drv, int fat32 )
{
    (void)drv;
    (void)fat32;
}

static int in_list( const u32 *list, int len, u32 lba, u32 count )
{
    int i;

    for ( i = 0; i < len; i++ ) {
        if ( list[i] >= lba && list[i] < lba + count ) {
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
    if ( drv != img_drive ) {
        return DE_GENERAL;
    }
    if ( not_ready ) {
        return DE_NOTREADY;
    }
    if ( in_list( bad, nbad, lba, count ) ) {
        return DE_DATA;
    }
    if ( seek( lba ) != 0 ||
         !ReadFile( img, buf, count * img_bps, &got, NULL ) ||
         got != count * img_bps ) {
        return DE_GENERAL;
    }
    return 0;
}

int sys_write_sec( int drv, u32 lba, u32 count, u32 bps, const void *buf,
                   int kind )
{
    u32 put;

    (void)bps;
    (void)kind;
    if ( drv != img_drive ) {
        return DE_GENERAL;
    }
    if ( not_ready ) {
        return DE_NOTREADY;
    }
    if ( img_ro ) {
        return DE_WRPROT;
    }
    if ( in_list( bad, nbad, lba, count ) ||
         in_list( wbad, nwbad, lba, count ) ) {
        return DE_WRFAULT;
    }
    if ( seek( lba ) != 0 ||
         !WriteFile( img, buf, count * img_bps, &put, NULL ) ||
         put != count * img_bps ) {
        return DE_GENERAL;
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

void sys_get_datetime( u16 *dos_date, u16 *dos_time )
{
    SYSTEMTIME now;

    GetLocalTime( &now );
    *dos_date = (u16)(((now.wYear - 1980) << 9) | (now.wMonth << 5) |
                      now.wDay);
    *dos_time = (u16)((now.wHour << 11) | (now.wMinute << 5) |
                      (now.wSecond / 2));
}

int sys_upcase( int ch )
{
    ch &= 0xFF;
    return ch >= 0x80 && ch < 0xB0 ? ucase437[ch - 0x80] : ch;
}
