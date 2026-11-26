/*
 * SYS_NT.C - QBASIC's system calls on Windows, for QBHOST.EXE: the
 * interpreter run from a console, so that a program's results can be
 * checked against real QBasic's without booting anything (HOST.C).
 *
 * THE SCREEN IS IN MEMORY: cells written and read back, a cursor, no
 * window.  KEYS come from a script (-keys), one key a line: a character,
 * or a scan code and a character in hex ("4800" is Up); when it runs out
 * a program waiting for a key is ended.  "C:\" in a path is the folder
 * given with -root, so a test's C:\NAME.OUT lands beside it.
 *
 * Graphics modes are pretended: the picture is GFX.C's own memory, and
 * POINT and GET read that, which is all a test can look at.
 *
 * Not built by BUILD.CMD; MKHOST.CMD builds it.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <direct.h>
#include "rt.h"
#include "sys.h"

static char     host_root[MAX_PATH] = ".";
static char     cmdline_tail[260];
static uint16_t screen[60 * 80];
static int      scr_rows = 25, scr_cols = 80;
static int      cur_row, cur_col;
static FILE    *keyfile;
static int      pending_key = -1;
static char     env_block[32768];

void host_set_root( const char *root )
{
    strncpy( host_root, root, sizeof( host_root ) - 1 );
}

void host_set_keys( const char *path )
{
    keyfile = fopen( path, "r" );
}

uint16_t *host_screen( int *rows, int *cols )
{
    *rows = scr_rows;
    *cols = scr_cols;
    return screen;
}

/* the current directory of C:, below the root folder ("" at the root) */
static char host_cwd[MAX_PATH] = "";

/* the parts of "text" added to "out", with . and .. worked out */
static void add_parts( char *out, const char *text )
{
    char work[MAX_PATH];
    char *part, *next, *cut;
    size_t len;

    strncpy( work, text, sizeof( work ) - 1 );
    work[sizeof( work ) - 1] = 0;
    for ( part = work; part != NULL; part = next ) {
        next = strpbrk( part, "\\/" );
        if ( next != NULL ) {
            *next++ = 0;
        }
        if ( strcmp( part, ".." ) == 0 ) {
            cut = strrchr( out, '\\' );
            if ( cut != NULL ) {
                *cut = 0;
            } else {
                out[0] = 0;
            }
        } else if ( part[0] != 0 && strcmp( part, "." ) != 0 ) {
            len = strlen( out );
            if ( len + strlen( part ) + 2 < MAX_PATH ) {
                if ( len > 0 ) {
                    out[len++] = '\\';
                }
                strcpy( out + len, part );
            }
        }
    }
}

/* a DOS path as one below the root folder: C:\ is the root, a relative
   name goes from the current directory */
static void dos_abs( const char *path, char *out )
{
    if ( (path[0] == 'C' || path[0] == 'c') && path[1] == ':' ) {
        path += 2;
    }
    out[0] = 0;
    if ( *path != '\\' && *path != '/' ) {
        add_parts( out, host_cwd );
    }
    add_parts( out, path );
}

/* a DOS path as the host's */
static void host_path( const char *path, char *out )
{
    char rel[MAX_PATH];

    dos_abs( path, rel );
    sprintf( out, "%s\\%s", host_root, rel );
}

static int win_error( void )
{
    switch ( GetLastError() ) {
    case ERROR_FILE_NOT_FOUND:      return ERR_NOFILE;
    case ERROR_PATH_NOT_FOUND:      return ERR_NOPATH;
    case ERROR_TOO_MANY_OPEN_FILES: return ERR_NOHANDLES;
    case ERROR_ACCESS_DENIED:       return ERR_ACCESS;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:      return ERR_EXISTS;
    case ERROR_DISK_FULL:           return ERR_DISKFULL;
    case ERROR_NOT_SAME_DEVICE:     return ERR_NOTSAME;
    case ERROR_SHARING_VIOLATION:   return ERR_SHARE;
    }
    return ERR_ACCESS;
}

int sys_init( void )
{
    return 0;
}

const char *sys_cmdline( void )
{
    return cmdline_tail;
}

const char *sys_getenv( const char *name )
{
    return getenv( name );
}

const char *sys_environment( void )
{
    char *strings = GetEnvironmentStringsA(), *cur;
    size_t used = 0, len;

    for ( cur = strings; *cur; cur += len + 1 ) {
        len = strlen( cur );
        if ( cur[0] == '=' ) {
            continue;
        }
        if ( used + len + 2 >= sizeof( env_block ) ) {
            break;
        }
        memcpy( env_block + used, cur, len + 1 );
        used += len + 1;
    }
    env_block[used] = 0;
    FreeEnvironmentStringsA( strings );
    return env_block;
}

void sys_exit( int code )
{
    exit( code );
}

void *sys_mem_alloc( unsigned int size )
{
    return malloc( size );
}

unsigned int sys_mem_largest( void )
{
    return 64UL * 1024 * 1024;
}

void sys_break_install( void )
{
}

void sys_stdout( const char *buf, unsigned int len )
{
    fwrite( buf, 1, len, stdout );
}

int sys_stdin_is_console( void )
{
    return 0;
}

int sys_stdin_byte( void )
{
    return getchar();
}

int sys_fpu_present( void )
{
    return 1;
}

const char *sys_shell( void )
{
    return "cmd";
}

int sys_exec( const char *program, const char *tail, const char *env )
{
    char line[600];

    (void)program;
    (void)env;
    sprintf( line, "cmd %s", tail );
    system( line );
    return 0;
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

void sys_screen_size( int *rows, int *cols )
{
    *rows = scr_rows;
    *cols = scr_cols;
}

void sys_put_cells( unsigned int first, const uint16_t *cells, unsigned int count )
{
    if ( first + count <= (unsigned int)(scr_rows * scr_cols) ) {
        memcpy( screen + first, cells, count * 2 );
    }
}

void sys_get_cells( unsigned int first, uint16_t *cells, unsigned int count )
{
    unsigned int i;

    for ( i = 0; i < count; i++ ) {
        cells[i] = first + i < (unsigned int)(scr_rows * scr_cols) ? (screen[first + i] ? screen[first + i] : 0x0720) : 0x0720;
    }
}

void sys_cursor( int row, int col )
{
    cur_row = row;
    cur_col = col;
}

void sys_get_cursor( int *row, int *col )
{
    *row = cur_row;
    *col = cur_col;
}

void sys_cursor_shape( int kind )
{
    (void)kind;
}

void sys_cursor_lines( int first, int last )
{
    (void)first;
    (void)last;
}

int sys_set_lines( int lines )
{
    scr_rows = lines > 25 ? 50 : 25;
    return scr_rows;
}

int sys_text_mode( int mode, int lines )
{
    scr_cols = mode <= 1 ? 40 : 80;
    scr_rows = lines;
    return 1;
}

static uint8_t fake_vram[640 * 480];

int sys_video_set( uint32_t mode, uint32_t *selector, uint32_t *bytes, uint32_t *pitch )
{
    (void)mode;
    *selector = 1;
    *bytes = sizeof( fake_vram );
    *pitch = 0;
    return 1;
}

void sys_video_restore( void )
{
}

int sys_video_palette( int first, int count, const uint8_t *entries )
{
    (void)first;
    (void)count;
    (void)entries;
    return 1;
}

int sys_video_palette_get( int first, int count, uint8_t *entries )
{
    (void)first;
    (void)count;
    (void)entries;
    return 0;
}

int sys_peek( uint32_t addr, void *buf, unsigned int len )
{
    (void)addr;
    memset( buf, 0, len );
    return 0;
}

void __cdecl vid_put( uint32_t selector, uint32_t offset, const void *src, unsigned int len )
{
    (void)selector;
    if ( offset + len <= sizeof( fake_vram ) ) {
        memcpy( fake_vram + offset, src, len );
    }
}

void __cdecl vid_fill( uint32_t selector, uint32_t offset, uint32_t value, unsigned int len )
{
    (void)selector;
    if ( offset + len <= sizeof( fake_vram ) ) {
        memset( fake_vram + offset, (int)value, len );
    }
}

void __cdecl port_out( uint32_t port, uint32_t value )
{
    (void)port;
    (void)value;
}

uint32_t __cdecl port_in( uint32_t port )
{
    (void)port;
    return 0xFF;
}

/* ------------------------------------------------------------------ */
/* keys                                                                */
/* ------------------------------------------------------------------ */

static int next_script_key( void )
{
    char line[64];
    unsigned val;

    if ( keyfile == NULL ) {
        return -1;
    }
    while ( fgets( line, sizeof( line ), keyfile ) != NULL ) {
        size_t len = strlen( line );
        while ( len && (line[len - 1] == '\n' || line[len - 1] == '\r') ) {
            line[--len] = 0;
        }
        if ( len == 0 ) {
            continue;
        }
        if ( len == 1 ) {
            return (uint8_t)line[0];
        }
        if ( strcmp( line, "enter" ) == 0 ) {
            return 0x1C0D;
        }
        if ( strcmp( line, "esc" ) == 0 ) {
            return 0x011B;
        }
        if ( strcmp( line, "space" ) == 0 ) {
            return 0x3920;
        }
        if ( strcmp( line, "break" ) == 0 ) {
            return 0x0003;
        }
        if ( sscanf( line, "%x", &val ) == 1 ) {
            return (int)val;
        }
    }
    return -1;
}

int sys_key_wait( int shift_seen, int *shift_now, int flags )
{
    (void)shift_seen;
    *shift_now = 0;
    if ( pending_key < 0 ) {
        pending_key = next_script_key();
    }
    if ( pending_key >= 0 ) {
        return 1;
    }
    if ( !(flags & KW_NOWAIT) ) {
        fprintf( stderr, "qbhost: the program waits for a key and the script has none left\n" );
        exit( 3 );
    }
    return 0;
}

int sys_key_read( int *shift_now )
{
    int key;

    *shift_now = 0;
    if ( pending_key < 0 ) {
        pending_key = next_script_key();
    }
    if ( pending_key < 0 ) {
        fprintf( stderr, "qbhost: the program waits for a key and the script has none left\n" );
        exit( 3 );
    }
    key = pending_key;
    pending_key = -1;
    return key;
}

int sys_shift( void )
{
    return 0;
}

/* ------------------------------------------------------------------ */
/* the time                                                            */
/* ------------------------------------------------------------------ */

uint32_t sys_hundredths( void )
{
    SYSTEMTIME st;

    GetLocalTime( &st );
    return (((uint32_t)st.wHour * 60 + st.wMinute) * 60 + st.wSecond) * 100 + st.wMilliseconds / 10;
}

void sys_date( int *year, int *month, int *day )
{
    SYSTEMTIME st;

    GetLocalTime( &st );
    *year = st.wYear;
    *month = st.wMonth;
    *day = st.wDay;
}

void sys_time( int *hour, int *minute, int *second, int *hundredth )
{
    SYSTEMTIME st;

    GetLocalTime( &st );
    *hour = st.wHour;
    *minute = st.wMinute;
    *second = st.wSecond;
    *hundredth = st.wMilliseconds / 10;
}

int sys_set_date( int year, int month, int day )
{
    (void)year;
    (void)month;
    (void)day;
    return 0;                                   /* pretended: the host's clock stays */
}

int sys_set_time( int hour, int minute, int second )
{
    (void)hour;
    (void)minute;
    (void)second;
    return 0;
}

/* ------------------------------------------------------------------ */
/* the mouse: none                                                     */
/* ------------------------------------------------------------------ */

int sys_mouse_reset( void ) { return 0; }
void sys_mouse_show( int show ) { (void)show; }
void sys_mouse_state( int *x, int *y, int *buttons ) { *x = *y = *buttons = 0; }
int sys_mouse_count( int release, int button, int *x, int *y ) { (void)release; (void)button; *x = *y = 0; return 0; }
void sys_mouse_extent( int *xmax, int *ymax ) { *xmax = 639; *ymax = 199; }
void sys_mouse_move( int x, int y ) { (void)x; (void)y; }
void sys_mouse_inject( int buttons ) { (void)buttons; }

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

int sys_lfn( void )
{
    return 1;
}

static int is_device_name( const char *path, char *dev )
{
    const char *base = strrchr( path, '\\' );

    base = base ? base + 1 : path;
    if ( _stricmp( base, "PRN" ) == 0 || _strnicmp( base, "LPT", 3 ) == 0 || _strnicmp( base, "COM", 3 ) == 0 ) {
        strcpy( dev, "NUL" );
        return 1;
    }
    return 0;
}

static int open_mode( const char *path, int flags, uint32_t *handle )
{
    char host[MAX_PATH * 2];
    int fd;

    if ( !is_device_name( path, host ) ) {
        host_path( path, host );
    }
    fd = open( host, flags | O_BINARY, S_IREAD | S_IWRITE );
    if ( fd < 0 ) {
        DWORD attr = GetFileAttributesA( host );
        if ( attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) ) {
            return ERR_ACCESS;
        }
        if ( attr == INVALID_FILE_ATTRIBUTES ) {
            char dir[MAX_PATH * 2], *slash;
            strcpy( dir, host );
            slash = strrchr( dir, '\\' );
            if ( slash != NULL ) {
                *slash = 0;
                if ( GetFileAttributesA( dir ) == INVALID_FILE_ATTRIBUTES ) {
                    return ERR_NOPATH;
                }
            }
            return (flags & O_CREAT) ? ERR_ACCESS : ERR_NOFILE;
        }
        return ERR_ACCESS;
    }
    *handle = (uint32_t)fd;
    return 0;
}

int sys_open( const char *path, int mode, uint32_t *handle )
{
    return open_mode( path, mode == OPEN_READ ? O_RDONLY : mode == OPEN_WRITE ? O_WRONLY : O_RDWR, handle );
}

int sys_open_read( const char *path, uint32_t *handle )
{
    return open_mode( path, O_RDONLY, handle );
}

int sys_open_write( const char *path, uint32_t *handle )
{
    return open_mode( path, O_WRONLY | O_CREAT, handle );
}

int sys_create( const char *path, uint32_t *handle )
{
    return open_mode( path, O_RDWR | O_CREAT | O_TRUNC, handle );
}

int sys_read( uint32_t handle, void *buf, unsigned int len, unsigned int *done )
{
    int got = read( (int)handle, buf, len );

    *done = got < 0 ? 0 : (unsigned int)got;
    return got < 0 ? ERR_ACCESS : 0;
}

int sys_write( uint32_t handle, const void *buf, unsigned int len, unsigned int *done )
{
    int put = write( (int)handle, buf, len );

    *done = put < 0 ? 0 : (unsigned int)put;
    return put < 0 ? ERR_ACCESS : ((unsigned int)put < len ? ERR_DISKFULL : 0);
}

int sys_close( uint32_t handle )
{
    return close( (int)handle ) < 0 ? ERR_BADHANDLE : 0;
}

int sys_seek( uint32_t handle, uint32_t pos )
{
    return lseek( (int)handle, (long)pos, SEEK_SET ) < 0 ? ERR_ACCESS : 0;
}

int sys_tell( uint32_t handle, uint32_t *pos )
{
    long at = lseek( (int)handle, 0, SEEK_CUR );

    *pos = at < 0 ? 0 : (uint32_t)at;
    return 0;
}

int sys_file_size( uint32_t handle, uint32_t *size )
{
    long end = lseek( (int)handle, 0, SEEK_END );

    *size = end < 0 ? 0 : (uint32_t)end;
    lseek( (int)handle, 0, SEEK_SET );
    return 0;
}

int sys_size_keep( uint32_t handle, uint32_t *size )
{
    long at = lseek( (int)handle, 0, SEEK_CUR ), end = lseek( (int)handle, 0, SEEK_END );

    *size = end < 0 ? 0 : (uint32_t)end;
    lseek( (int)handle, at, SEEK_SET );
    return 0;
}

int sys_truncate( uint32_t handle )
{
    long at = lseek( (int)handle, 0, SEEK_CUR );

    return chsize( (int)handle, at ) < 0 ? ERR_ACCESS : 0;
}

int sys_rewind( uint32_t handle )
{
    return sys_seek( handle, 0 );
}

int sys_is_device( uint32_t handle )
{
    (void)handle;
    return 0;
}

int sys_get_attr( const char *path, uint16_t *attr )
{
    char host[MAX_PATH * 2];
    DWORD a;

    host_path( path, host );
    a = GetFileAttributesA( host );
    if ( a == INVALID_FILE_ATTRIBUTES ) {
        return win_error();
    }
    *attr = (uint16_t)(((a & FILE_ATTRIBUTE_DIRECTORY) ? ATTR_DIR : 0) | ((a & FILE_ATTRIBUTE_READONLY) ? ATTR_READONLY : 0)
                       | ((a & FILE_ATTRIBUTE_HIDDEN) ? ATTR_HIDDEN : 0) | ATTR_ARCHIVE);
    return 0;
}

int sys_delete( const char *path )
{
    char host[MAX_PATH * 2];

    host_path( path, host );
    return DeleteFileA( host ) ? 0 : win_error();
}

int sys_rename( const char *from, const char *to )
{
    char hfrom[MAX_PATH * 2], hto[MAX_PATH * 2];

    host_path( from, hfrom );
    host_path( to, hto );
    return MoveFileA( hfrom, hto ) ? 0 : win_error();
}

int sys_mkdir( const char *path )
{
    char host[MAX_PATH * 2];

    host_path( path, host );
    return CreateDirectoryA( host, NULL ) ? 0 : win_error();
}

int sys_rmdir( const char *path )
{
    char host[MAX_PATH * 2];

    host_path( path, host );
    return RemoveDirectoryA( host ) ? 0 : win_error();
}

typedef struct {
    HANDLE find;
    WIN32_FIND_DATAA data;
    uint16_t attrs;
} HOSTFIND;

static void rec_from( WIN32_FIND_DATAA *data, FINDREC *rec )
{
    rec->attr = (uint8_t)(((data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? ATTR_DIR : 0)
                          | ((data->dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? ATTR_READONLY : 0) | ATTR_ARCHIVE);
    rec->size = data->nFileSizeLow;
    strncpy( rec->name, data->cFileName, sizeof( rec->name ) - 1 );
    rec->name[sizeof( rec->name ) - 1] = 0;
    strncpy( rec->alias, data->cAlternateFileName[0] ? data->cAlternateFileName : data->cFileName, sizeof( rec->alias ) - 1 );
    rec->alias[sizeof( rec->alias ) - 1] = 0;
}

static int find_ok( HOSTFIND *f )
{
    if ( (f->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(f->attrs & ATTR_DIR) ) {
        return 0;
    }
    return 1;
}

int sys_find_first( const char *pattern, uint16_t attrs, FINDREC *rec, uint32_t *handle )
{
    char host[MAX_PATH * 2];
    HOSTFIND *f = (HOSTFIND *)malloc( sizeof( HOSTFIND ) );

    host_path( pattern, host );
    f->attrs = attrs;
    f->find = FindFirstFileA( host, &f->data );
    if ( f->find == INVALID_HANDLE_VALUE ) {
        free( f );
        return ERR_NOFILE;
    }
    while ( !find_ok( f ) ) {
        if ( !FindNextFileA( f->find, &f->data ) ) {
            FindClose( f->find );
            free( f );
            return ERR_NOFILE;
        }
    }
    rec_from( &f->data, rec );
    *handle = (uint32_t)f;
    return 0;
}

int sys_find_next( uint32_t handle, FINDREC *rec )
{
    HOSTFIND *f = (HOSTFIND *)handle;

    do {
        if ( !FindNextFileA( f->find, &f->data ) ) {
            return ERR_NOMORE;
        }
    } while ( !find_ok( f ) );
    rec_from( &f->data, rec );
    return 0;
}

void sys_find_close( uint32_t handle )
{
    HOSTFIND *f = (HOSTFIND *)handle;

    if ( f ) {
        FindClose( f->find );
        free( f );
    }
}

int sys_get_drive( void )
{
    return 2;
}

void sys_set_drive( int drv )
{
    (void)drv;
}

int sys_drive_valid( int drv )
{
    return drv == 2;
}

int sys_get_cwd( int drv, char *buf )
{
    (void)drv;
    strcpy( buf, host_cwd );
    return 0;
}

int sys_chdir( const char *path )
{
    char host[MAX_PATH * 2], rel[MAX_PATH];
    DWORD attr;

    dos_abs( path, rel );
    sprintf( host, "%s\\%s", host_root, rel );
    attr = GetFileAttributesA( host );
    if ( attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ) {
        return ERR_NOPATH;
    }
    strcpy( host_cwd, rel );
    return 0;
}

int sys_truename( const char *path, char *out, int form )
{
    char host[MAX_PATH * 2];

    (void)form;
    host_path( path, host );
    if ( GetFullPathNameA( host, PATH_MAX, out, NULL ) == 0 ) {
        return win_error();
    }
    return 0;
}

int sys_disk_free( int drv, uint32_t *bytes )
{
    (void)drv;
    *bytes = 1000000UL;
    return 0;
}
