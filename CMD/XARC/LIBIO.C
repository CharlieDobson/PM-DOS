/*
 * LIBIO.C - the half of XARC's C library that talks to the kernel:
 * files, folders, the clock and the program's own surroundings.
 *
 * LONG NAMES FIRST.  Every call that takes a path tries the 71h form
 * first - 716Ch to open, 7139h to make a folder, 7141h to delete, 714Eh
 * to search - and falls back to the classic call for good the first
 * time the kernel answers 7100h, which is what it says when CONFIG.SYS
 * has LFN=OFF.  PM-DOS's classic calls take long names too, but only
 * the 71h searches GIVE one back, and a program that asked the volume
 * whether it holds long names should be using the calls that go with
 * the answer.
 *
 * A FILE is a handle and a buffer of BUFSIZ bytes, reading or writing
 * but not both at once: a change of direction flushes, or seeks back to
 * where the reader had got to.  A read or a write bigger than the
 * buffer goes straight between the caller's memory and the disk, which
 * is how an extraction's output goes out.  Text mode is the DOS one -
 * see STDIO.H.
 *
 * The program's command tail, environment and exit are here too, and
 * the calendar: a time_t is local seconds since 1970, worked out with
 * Howard Hinnant's civil-day formulas - the same arithmetic the DOS and
 * OS/2 builds' FILETIME conversions use.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include "xsys.h"

u8  *xa_psp;
int  xa_lfn = 1;

static const char *env_block;
static char tail[300];

/* ------------------------------------------------------------------ */
/* the kernel                                                          */
/* ------------------------------------------------------------------ */

void xa_clear( REGS *regs )
{
    memset( regs, 0, sizeof( *regs ) );
}

int xa_dos( REGS *regs, u32 func )
{
    regs->eax = func;
    int21( regs );
    if ( !CF( *regs ) ) {
        return 0;
    }
    return AX( *regs ) ? (int)AX( *regs ) : 31;
}

/* A 71h call: 0 when it worked, its error when it failed, and -1 when
   the long-name calls are not there - after which none is tried again. */
static int lfn_call( REGS *regs, u32 func )
{
    int rc;

    if ( !xa_lfn ) {
        return -1;
    }
    regs->flags = 1;                    /* carry set: as LFN probes do */
    rc = xa_dos( regs, func );
    if ( rc == 0x7100 ) {
        xa_lfn = 0;
        return -1;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* the program                                                         */
/* ------------------------------------------------------------------ */

#define MAX_EXIT_FNS    4

static void (*exit_fn[MAX_EXIT_FNS])( void );
static int exit_fns;

void xa_crt_init( void )
{
    REGS regs;
    const char *full;
    u32 len;
    int quoted;

    xa_clear( &regs );
    xa_dos( &regs, 0x6200 );
    xa_psp = (u8 *)regs.ebx;
    env_block = (const char *)*(u32 *)(xa_psp + 0x2C);
    len = xa_psp[0x80];
    if ( len > 126 ) {
        len = 126;
    }
    memcpy( tail, xa_psp + 0x81, len );
    tail[len] = 0;
    /* a tail too long for the PSP is in CMDLINE, program name and all */
    if ( xa_psp[0x80] == 0x7F && (full = getenv( "CMDLINE" )) != NULL ) {
        quoted = 0;
        while ( *full == ' ' || *full == '\t' ) {
            full++;
        }
        while ( *full && (quoted || (*full != ' ' && *full != '\t' && *full != '/')) ) {
            if ( *full == '"' ) {
                quoted = !quoted;
            }
            full++;
        }
        strncpy( tail, full, sizeof( tail ) - 1 );
        tail[sizeof( tail ) - 1] = 0;
    }
}

const char *xa_cmdtail( void )
{
    return tail;
}

char *getenv( const char *name )
{
    const char *env = env_block;
    size_t len = strlen( name );

    if ( env == NULL ) {
        return NULL;
    }
    while ( *env ) {
        if ( strnicmp( env, name, len ) == 0 && env[len] == '=' ) {
            return (char *)env + len + 1;
        }
        env += strlen( env ) + 1;
    }
    return NULL;
}

void xa_on_exit( void (*fn)( void ) )
{
    if ( exit_fns < MAX_EXIT_FNS ) {
        exit_fn[exit_fns++] = fn;
    }
}

void exit( int code )
{
    REGS regs;

    while ( exit_fns > 0 ) {
        exit_fn[--exit_fns]();
    }
    fflush( NULL );
    xa_clear( &regs );
    xa_dos( &regs, 0x4C00 | ((u32)code & 0xFF) );
}

/* ------------------------------------------------------------------ */
/* FILE                                                                */
/* ------------------------------------------------------------------ */

#define XF_READ     0x0001      /* may be read */
#define XF_WRITE    0x0002      /* may be written */
#define XF_TEXT     0x0004
#define XF_EOF      0x0008
#define XF_ERR      0x0010
#define XF_LINE     0x0020      /* written out at each '\n' */
#define XF_NOBUF    0x0040      /* written out at once */
#define XF_STATIC   0x0080      /* stdin, stdout, stderr */

#define ST_IDLE     0
#define ST_READ     1           /* buf holds len bytes from "base" on, pos read */
#define ST_WRITE    2           /* buf holds pos bytes to go out at "base" */

struct _xa_file {
    int      handle;
    unsigned flags;
    int      state;
    u8      *buf;
    u32      size;              /* of buf */
    u32      pos;
    u32      len;
    long     base;              /* the file offset buf[0] stands for */
    int      ungot;             /* text mode's look past a CR, or -1 */
    FILE    *next;              /* every file open, for exit's flush */
};

static u8 out_buf[256];
static u8 err_buf[16];
static u8 in_buf[256];

/* text streams, as DOS's are: the console moves down a line on LF and
   back to the left only on CR (test mode's keys are read raw, SCR.C) */
static FILE std_in  = { 0, XF_READ | XF_TEXT | XF_STATIC, ST_IDLE, in_buf, sizeof( in_buf ), 0, 0, 0, -1, NULL };
static FILE std_out = { 1, XF_WRITE | XF_TEXT | XF_LINE | XF_STATIC, ST_IDLE, out_buf, sizeof( out_buf ), 0, 0, 0, -1, NULL };
static FILE std_err = { 2, XF_WRITE | XF_TEXT | XF_NOBUF | XF_STATIC, ST_IDLE, err_buf, sizeof( err_buf ), 0, 0, 0, -1, NULL };

FILE *_xa_stdin  = &std_in;
FILE *_xa_stdout = &std_out;
FILE *_xa_stderr = &std_err;

static FILE *open_files;

static int raw_read( int handle, void *buf, u32 len, u32 *got )
{
    REGS regs;
    int rc;

    xa_clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    rc = xa_dos( &regs, 0x3F00 );
    *got = rc ? 0 : regs.eax;
    return rc;
}

static int raw_write( int handle, const void *buf, u32 len )
{
    REGS regs;

    if ( len == 0 ) {
        return 0;
    }
    xa_clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    if ( xa_dos( &regs, 0x4000 ) ) {
        return -1;
    }
    return regs.eax == len ? 0 : -1;    /* short: the disk is full */
}

/* 42h, the native form: the offset in ECX, the new position in EAX */
static long raw_seek( int handle, long offset, int whence )
{
    REGS regs;

    xa_clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = (u32)offset;
    if ( xa_dos( &regs, 0x4200 | (u32)(whence & 3) ) ) {
        return -1L;
    }
    return (long)regs.eax;
}

static int raw_close( int handle )
{
    REGS regs;

    xa_clear( &regs );
    regs.ebx = (u32)handle;
    return xa_dos( &regs, 0x3E00 );
}

/* Open or create.  "action" is 716Ch's DX: 01h open what is there, 10h
   create what is not, 02h empty what is there. */
static int raw_open( const char *path, int access, int action, int *handle )
{
    REGS regs;
    int rc;

    xa_clear( &regs );
    regs.esi = (u32)path;
    regs.ebx = (u32)access;
    regs.ecx = 0;
    regs.edx = (u32)action;
    rc = lfn_call( &regs, 0x716C );
    if ( rc >= 0 ) {
        *handle = (int)AX( regs );
        return rc;
    }
    /* the classic calls: 3Ch makes or empties, 3Dh opens */
    xa_clear( &regs );
    regs.edx = (u32)path;
    if ( action & 0x02 ) {
        rc = xa_dos( &regs, 0x3C00 );
    } else {
        rc = xa_dos( &regs, 0x3D00 | (u32)access );
        if ( rc == 2 && (action & 0x10) ) {
            xa_clear( &regs );
            regs.edx = (u32)path;
            rc = xa_dos( &regs, 0x3C00 );
        }
    }
    *handle = (int)AX( regs );
    return rc;
}

FILE *fopen( const char *path, const char *mode )
{
    FILE *fp;
    int access, action, handle, plus = strchr( mode, '+' ) != NULL;
    unsigned flags;

    switch ( mode[0] ) {
    case 'r':
        access = plus ? 2 : 0;
        action = 0x01;
        flags = plus ? XF_READ | XF_WRITE : XF_READ;
        break;
    case 'w':
        access = plus ? 2 : 1;
        action = 0x12;
        flags = plus ? XF_READ | XF_WRITE : XF_WRITE;
        break;
    case 'a':
        access = plus ? 2 : 1;
        action = 0x11;
        flags = plus ? XF_READ | XF_WRITE : XF_WRITE;
        break;
    default:
        return NULL;
    }
    if ( strchr( mode, 'b' ) == NULL ) {
        flags |= XF_TEXT;
    }
    fp = (FILE *)malloc( sizeof( FILE ) + BUFSIZ );
    if ( fp == NULL ) {
        return NULL;
    }
    if ( raw_open( path, access, action, &handle ) != 0 ) {
        free( fp );
        return NULL;
    }
    memset( fp, 0, sizeof( FILE ) );
    fp->handle = handle;
    fp->flags = flags;
    fp->state = ST_IDLE;
    fp->buf = (u8 *)(fp + 1);
    fp->size = BUFSIZ;
    fp->ungot = -1;
    if ( mode[0] == 'a' ) {
        fp->base = raw_seek( handle, 0, SEEK_END );
        if ( fp->base < 0 ) {
            fp->base = 0;
        }
    }
    fp->next = open_files;
    open_files = fp;
    return fp;
}

/* what is waiting to be written goes out; a reader's handle is put back
   where the reader had got to, and its buffer forgotten */
static int settle( FILE *fp )
{
    int rc = 0;
    long at;

    if ( fp->state == ST_WRITE ) {
        if ( raw_write( fp->handle, fp->buf, fp->pos ) != 0 ) {
            fp->flags |= XF_ERR;
            rc = EOF;
        }
        fp->base += (long)fp->pos;
    } else if ( fp->state == ST_READ ) {
        at = fp->base + (long)fp->pos - (fp->ungot >= 0 ? 1 : 0);
        if ( fp->pos != fp->len || fp->ungot >= 0 ) {
            raw_seek( fp->handle, at, SEEK_SET );
        }
        fp->base = at;
        fp->ungot = -1;
    }
    fp->state = ST_IDLE;
    fp->pos = fp->len = 0;
    return rc;
}

int fflush( FILE *fp )
{
    FILE *each;
    int rc = 0;

    if ( fp == NULL ) {
        for ( each = open_files; each; each = each->next ) {
            if ( each->state == ST_WRITE && settle( each ) ) {
                rc = EOF;
            }
        }
        settle( &std_out );
        settle( &std_err );
        return rc;
    }
    if ( fp->state == ST_WRITE && settle( fp ) ) {
        return EOF;
    }
    /* and an earlier flush that failed is still a failure */
    return (fp->flags & (XF_WRITE | XF_ERR)) == (XF_WRITE | XF_ERR) ? EOF : 0;
}

int fclose( FILE *fp )
{
    FILE **link;
    int rc;

    if ( fp == NULL ) {
        return EOF;
    }
    rc = settle( fp );
    if ( (fp->flags & (XF_WRITE | XF_ERR)) == (XF_WRITE | XF_ERR) ) {
        rc = EOF;                       /* an earlier flush failed */
    }
    if ( fp->flags & XF_STATIC ) {
        return rc;
    }
    if ( raw_close( fp->handle ) != 0 ) {
        rc = EOF;
    }
    for ( link = &open_files; *link; link = &(*link)->next ) {
        if ( *link == fp ) {
            *link = fp->next;
            break;
        }
    }
    free( fp );
    return rc;
}

/* the next byte as the file holds it; EOF at the end */
static int raw_getc( FILE *fp )
{
    u32 got;

    if ( fp->state != ST_READ ) {
        if ( !(fp->flags & XF_READ) ) {
            fp->flags |= XF_ERR;
            return EOF;
        }
        settle( fp );
        fp->state = ST_READ;
    }
    if ( fp->pos >= fp->len ) {
        fp->base += (long)fp->len;
        fp->pos = fp->len = 0;
        if ( raw_read( fp->handle, fp->buf, fp->size, &got ) != 0 ) {
            fp->flags |= XF_ERR;
            return EOF;
        }
        if ( got == 0 ) {
            fp->flags |= XF_EOF;
            return EOF;
        }
        fp->len = got;
    }
    return fp->buf[fp->pos++];
}

int fgetc( FILE *fp )
{
    int ch, next;

    if ( fp->ungot >= 0 ) {
        ch = fp->ungot;
        fp->ungot = -1;
        return ch;
    }
    ch = raw_getc( fp );
    if ( !(fp->flags & XF_TEXT) || ch == EOF ) {
        return ch;
    }
    if ( ch == 0x1A ) {                 /* Ctrl+Z ends a text file */
        fp->flags |= XF_EOF;
        fp->pos--;
        return EOF;
    }
    if ( ch == '\r' ) {
        next = raw_getc( fp );
        if ( next == '\n' ) {
            return '\n';
        }
        /* the byte is still in the buffer: read again, it goes through
           the CR and Ctrl+Z tests like any other */
        if ( next != EOF ) {
            fp->pos--;
        }
    }
    return ch;
}

char *fgets( char *buf, int size, FILE *fp )
{
    int used = 0, ch;

    if ( size < 1 ) {
        return NULL;
    }
    while ( used < size - 1 ) {
        ch = fgetc( fp );
        if ( ch == EOF ) {
            break;
        }
        buf[used++] = (char)ch;
        if ( ch == '\n' ) {
            break;
        }
    }
    buf[used] = 0;
    return used ? buf : NULL;
}

size_t fread( void *buf, size_t size, size_t count, FILE *fp )
{
    u8 *to = (u8 *)buf;
    u32 want, done = 0, take, got;
    int ch;

    if ( size == 0 || count == 0 ) {
        return 0;
    }
    want = (u32)size * (u32)count;
    if ( fp->flags & XF_TEXT ) {
        for ( ; done < want; done++ ) {
            ch = fgetc( fp );
            if ( ch == EOF ) {
                break;
            }
            to[done] = (u8)ch;
        }
        return done / size;
    }
    if ( fp->state != ST_READ ) {
        if ( !(fp->flags & XF_READ) ) {
            fp->flags |= XF_ERR;
            return 0;
        }
        settle( fp );
        fp->state = ST_READ;
    }
    while ( done < want ) {
        if ( fp->pos < fp->len ) {
            take = fp->len - fp->pos;
            if ( take > want - done ) {
                take = want - done;
            }
            memcpy( to + done, fp->buf + fp->pos, take );
            fp->pos += take;
            done += take;
            continue;
        }
        fp->base += (long)fp->len;
        fp->pos = fp->len = 0;
        if ( want - done >= fp->size ) {
            /* big: straight into the caller's memory */
            if ( raw_read( fp->handle, to + done, want - done, &got ) != 0 ) {
                fp->flags |= XF_ERR;
                break;
            }
            fp->base += (long)got;
            if ( got < want - done ) {
                fp->flags |= XF_EOF;        /* short: the end came first */
            }
            done += got;
            break;
        }
        if ( raw_read( fp->handle, fp->buf, fp->size, &got ) != 0 ) {
            fp->flags |= XF_ERR;
            break;
        }
        if ( got == 0 ) {
            fp->flags |= XF_EOF;
            break;
        }
        fp->len = got;
    }
    return done / size;
}

static int raw_putc( FILE *fp, int ch )
{
    if ( fp->state != ST_WRITE ) {
        if ( !(fp->flags & XF_WRITE) ) {
            fp->flags |= XF_ERR;
            return EOF;
        }
        settle( fp );
        fp->state = ST_WRITE;
    }
    fp->buf[fp->pos++] = (u8)ch;
    if ( fp->pos >= fp->size || (fp->flags & XF_NOBUF)
         || ((fp->flags & XF_LINE) && ch == '\n') ) {
        if ( settle( fp ) ) {
            return EOF;
        }
    }
    return ch & 0xFF;
}

int fputc( int ch, FILE *fp )
{
    if ( (fp->flags & XF_TEXT) && ch == '\n' && raw_putc( fp, '\r' ) == EOF ) {
        return EOF;
    }
    return raw_putc( fp, ch );
}

size_t fwrite( const void *buf, size_t size, size_t count, FILE *fp )
{
    const u8 *from = (const u8 *)buf;
    u32 want, done = 0, take;

    if ( size == 0 || count == 0 ) {
        return 0;
    }
    want = (u32)size * (u32)count;
    if ( (fp->flags & (XF_TEXT | XF_LINE | XF_NOBUF)) ) {
        for ( ; done < want; done++ ) {
            if ( fputc( from[done], fp ) == EOF ) {
                break;
            }
        }
        return done / size;
    }
    if ( fp->state != ST_WRITE ) {
        if ( !(fp->flags & XF_WRITE) ) {
            fp->flags |= XF_ERR;
            return 0;
        }
        settle( fp );
        fp->state = ST_WRITE;
    }
    while ( done < want ) {
        if ( fp->pos == 0 && want - done >= fp->size ) {
            /* big: straight from the caller's memory */
            if ( raw_write( fp->handle, from + done, want - done ) != 0 ) {
                fp->flags |= XF_ERR;
                break;
            }
            fp->base += (long)(want - done);
            done = want;
            break;
        }
        take = fp->size - fp->pos;
        if ( take > want - done ) {
            take = want - done;
        }
        memcpy( fp->buf + fp->pos, from + done, take );
        fp->pos += take;
        if ( fp->pos >= fp->size ) {
            if ( settle( fp ) ) {
                break;                  /* these bytes did not get there */
            }
            fp->state = ST_WRITE;
        }
        done += take;
    }
    return done / size;
}

int fputs( const char *text, FILE *fp )
{
    for ( ; *text; text++ ) {
        if ( fputc( (u8)*text, fp ) == EOF ) {
            return EOF;
        }
    }
    return 0;
}

int puts( const char *text )
{
    if ( fputs( text, stdout ) == EOF ) {
        return EOF;
    }
    return fputc( '\n', stdout );
}

long ftell( FILE *fp )
{
    if ( fp->state == ST_READ ) {
        return fp->base + (long)fp->pos - (fp->ungot >= 0 ? 1 : 0);
    }
    if ( fp->state == ST_WRITE ) {
        return fp->base + (long)fp->pos;
    }
    return fp->base;
}

int fseek( FILE *fp, long offset, int whence )
{
    long target, now;

    if ( whence == SEEK_CUR ) {
        offset += ftell( fp );
        whence = SEEK_SET;
    }
    /* inside what has been read already: no call at all, which is how a
       reader that steps back over a header costs nothing */
    if ( fp->state == ST_READ && whence == SEEK_SET && fp->ungot < 0
         && offset >= fp->base && offset <= fp->base + (long)fp->len ) {
        fp->pos = (u32)(offset - fp->base);
        fp->flags &= ~XF_EOF;
        return 0;
    }
    if ( settle( fp ) ) {
        return -1;
    }
    target = whence == SEEK_SET ? offset : 0;
    if ( whence == SEEK_SET && target < 0 ) {
        return -1;
    }
    now = raw_seek( fp->handle, whence == SEEK_SET ? target : offset, whence );
    if ( now < 0 ) {
        fp->flags |= XF_ERR;
        return -1;
    }
    fp->base = now;
    fp->flags &= ~XF_EOF;
    return 0;
}

void rewind( FILE *fp )
{
    fseek( fp, 0L, SEEK_SET );
    fp->flags &= ~(XF_EOF | XF_ERR);
}

int  feof( FILE *fp )       { return (fp->flags & XF_EOF) != 0; }
int  ferror( FILE *fp )     { return (fp->flags & XF_ERR) != 0; }
void clearerr( FILE *fp )   { fp->flags &= ~(XF_EOF | XF_ERR); }
int  fileno( FILE *fp )     { return fp->handle; }

/* a seek to "size" and a write of nothing: DOS cuts the file there */
int _chsize( int handle, long size )
{
    REGS regs;

    if ( raw_seek( handle, size, SEEK_SET ) != size ) {
        return -1;
    }
    xa_clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = 0;
    regs.edx = (u32)&regs;
    return xa_dos( &regs, 0x4000 ) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* names and folders                                                   */
/* ------------------------------------------------------------------ */

static int path_call( const char *path, u32 lfn_func, u32 func )
{
    REGS regs;
    int rc;

    xa_clear( &regs );
    regs.edx = (u32)path;
    regs.esi = 0;                       /* 7141h: no wildcards */
    rc = lfn_call( &regs, lfn_func );
    if ( rc < 0 ) {
        xa_clear( &regs );
        regs.edx = (u32)path;
        rc = xa_dos( &regs, func );
    }
    return rc ? -1 : 0;
}

int remove( const char *path ) { return path_call( path, 0x7141, 0x4100 ); }
int _mkdir( const char *path ) { return path_call( path, 0x7139, 0x3900 ); }
int _rmdir( const char *path ) { return path_call( path, 0x713A, 0x3A00 ); }
int chdir( const char *path )  { return path_call( path, 0x713B, 0x3B00 ); }

int rename( const char *from, const char *to )
{
    REGS regs;
    int rc;

    xa_clear( &regs );
    regs.edx = (u32)from;
    regs.edi = (u32)to;
    rc = lfn_call( &regs, 0x7156 );
    if ( rc < 0 ) {
        xa_clear( &regs );
        regs.edx = (u32)from;
        regs.edi = (u32)to;
        rc = xa_dos( &regs, 0x5600 );
    }
    return rc ? -1 : 0;
}

int _getdrive( void )
{
    REGS regs;

    xa_clear( &regs );
    xa_dos( &regs, 0x1900 );
    return (int)(regs.eax & 0xFF) + 1;
}

int _chdrive( int drive )
{
    REGS regs;

    xa_clear( &regs );
    regs.edx = (u32)(drive - 1);
    xa_dos( &regs, 0x0E00 );
    return _getdrive() == drive ? 0 : -1;
}

/* "DIR\SUB" of drive "drive" (1 = A:, 0 the current one), the long
   form: no drive and no leading backslash, as 47h has always given it */
static int drive_cwd( int drive, char *buf, size_t size )
{
    REGS regs;
    char tmp[270];
    int rc;

    tmp[0] = 0;
    xa_clear( &regs );
    regs.edx = (u32)drive;
    regs.esi = (u32)tmp;
    rc = lfn_call( &regs, 0x7147 );
    if ( rc < 0 ) {
        xa_clear( &regs );
        regs.edx = (u32)drive;
        regs.esi = (u32)tmp;
        rc = xa_dos( &regs, 0x4700 );
    }
    if ( rc || strlen( tmp ) >= size ) {
        return -1;
    }
    strcpy( buf, tmp );
    return 0;
}

char *getcwd( char *buf, size_t size )
{
    char dir[270];
    int drive = _getdrive();

    if ( size < 4 || drive_cwd( drive, dir, sizeof( dir ) ) != 0
         || strlen( dir ) + 4 > size ) {
        return NULL;
    }
    buf[0] = (char)('A' + drive - 1);
    buf[1] = ':';
    buf[2] = '\\';
    strcpy( buf + 3, dir );
    return buf;
}

static int is_sep( char ch )
{
    return ch == '\\' || ch == '/';
}

/* See STDLIB.H.  A path that starts "\\" is a network name and is left
   as it is but for its separators. */
char *_fullpath( char *buf, const char *path, size_t size )
{
    char work[540], dir[270];
    const char *rest;
    char *out, *mark;
    int drive, len;
    size_t used;

    if ( path == NULL || path[0] == 0 ) {
        path = ".";
    }
    if ( is_sep( path[0] ) && is_sep( path[1] ) ) {
        if ( strlen( path ) >= size ) {
            return NULL;
        }
        for ( used = 0; path[used]; used++ ) {
            buf[used] = path[used] == '/' ? '\\' : path[used];
        }
        buf[used] = 0;
        return buf;
    }
    if ( isalpha( (u8)path[0] ) && path[1] == ':' ) {
        drive = toupper( (u8)path[0] ) - 'A' + 1;
        rest = path + 2;
    } else {
        drive = _getdrive();
        rest = path;
    }
    work[0] = (char)('A' + drive - 1);
    work[1] = ':';
    work[2] = 0;
    if ( !is_sep( rest[0] ) ) {
        if ( drive_cwd( drive, dir, sizeof( dir ) ) != 0 ) {
            return NULL;
        }
        if ( dir[0] ) {
            strcat( work, "\\" );
            strcat( work, dir );
        }
    }
    if ( strlen( work ) + strlen( rest ) + 2 > sizeof( work ) ) {
        return NULL;
    }
    strcat( work, "\\" );
    strcat( work, rest );

    /* the components, with "." dropped and ".." taking the one before */
    out = buf;
    if ( size < 4 ) {
        return NULL;
    }
    out[0] = work[0];
    out[1] = ':';
    out[2] = 0;
    used = 2;
    rest = work + 2;
    for ( ;; ) {
        while ( is_sep( *rest ) ) {
            rest++;
        }
        if ( *rest == 0 ) {
            break;
        }
        for ( len = 0; rest[len] && !is_sep( rest[len] ); len++ ) {
        }
        if ( len == 1 && rest[0] == '.' ) {
            /* here */
        } else if ( len == 2 && rest[0] == '.' && rest[1] == '.' ) {
            mark = strrchr( out, '\\' );
            if ( mark ) {
                *mark = 0;
                used = (size_t)(mark - out);
            }
        } else {
            if ( used + 1 + (size_t)len + 1 > size ) {
                return NULL;
            }
            out[used++] = '\\';
            memcpy( out + used, rest, (size_t)len );
            used += (size_t)len;
            out[used] = 0;
        }
        rest += len;
    }
    if ( used == 2 ) {
        out[2] = '\\';
        out[3] = 0;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* the calendar - see TIME.H                                           */
/* ------------------------------------------------------------------ */

long xa_days_from_civil( long year, int month, int day )
{
    long era, yoe, doy, doe;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    yoe = year - era * 400;
    doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void xa_civil_from_days( long days, int *year, int *month, int *day )
{
    long era, doe, yoe, doy, mp, y;

    days += 719468;
    era = (days >= 0 ? days : days - 146096) / 146097;
    doe = days - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(y + (*month <= 2));
}

/* unsigned, as Watcom's is: DOS dates run to 2107, and 2038 is where a
   signed count of seconds would turn negative */
time_t mktime( struct tm *when )
{
    return (time_t)xa_days_from_civil( when->tm_year + 1900L, when->tm_mon + 1, when->tm_mday )
           * 86400UL + (time_t)(when->tm_hour * 3600L + when->tm_min * 60L + when->tm_sec);
}

struct tm *localtime( const time_t *when )
{
    static struct tm out;
    long days, secs;
    int year, month, day;

    if ( when == NULL ) {
        return NULL;
    }
    days = (long)(*when / 86400UL);
    secs = (long)(*when % 86400UL);
    xa_civil_from_days( days, &year, &month, &day );
    out.tm_year = year - 1900;
    out.tm_mon = month - 1;
    out.tm_mday = day;
    out.tm_hour = (int)(secs / 3600);
    out.tm_min = (int)(secs / 60 % 60);
    out.tm_sec = (int)(secs % 60);
    out.tm_wday = (int)((days % 7 + 11) % 7);       /* 1970-01-01 was a Thursday */
    out.tm_yday = (int)(days - xa_days_from_civil( year, 1, 1 ));
    out.tm_isdst = 0;
    return &out;
}

time_t time( time_t *now )
{
    REGS regs;
    struct tm at;
    time_t secs;

    xa_clear( &regs );
    xa_dos( &regs, 0x2A00 );
    at.tm_year = (int)(regs.ecx & 0xFFFF) - 1900;
    at.tm_mon = (int)((regs.edx >> 8) & 0xFF) - 1;
    at.tm_mday = (int)(regs.edx & 0xFF);
    xa_clear( &regs );
    xa_dos( &regs, 0x2C00 );
    at.tm_hour = (int)((regs.ecx >> 8) & 0xFF);
    at.tm_min = (int)(regs.ecx & 0xFF);
    at.tm_sec = (int)((regs.edx >> 8) & 0xFF);
    secs = mktime( &at );
    if ( now ) {
        *now = secs;
    }
    return secs;
}

/* a DOS date and time, as a time_t */
static time_t dos_to_time( unsigned date, unsigned clock )
{
    struct tm at;

    at.tm_year = (int)((date >> 9) & 0x7F) + 80;
    at.tm_mon = (int)((date >> 5) & 0x0F) - 1;
    at.tm_mday = (int)(date & 0x1F);
    at.tm_hour = (int)((clock >> 11) & 0x1F);
    at.tm_min = (int)((clock >> 5) & 0x3F);
    at.tm_sec = (int)((clock & 0x1F) * 2);
    if ( at.tm_mon < 0 ) {
        at.tm_mon = 0;
    }
    if ( at.tm_mday < 1 ) {
        at.tm_mday = 1;
    }
    return mktime( &at );
}

/* ------------------------------------------------------------------ */
/* stat                                                                */
/* ------------------------------------------------------------------ */

#define FIND_ALL    0x37        /* read-only, hidden, system, folder, archive */

int stat( const char *path, struct stat *info )
{
    REGS regs;
    static u8 rec[320];
    u32 attr, size, stamp;
    int rc;
    size_t len = strlen( path );

    memset( info, 0, sizeof( *info ) );
    /* a drive's root has no directory entry to find */
    if ( (len == 3 && path[1] == ':' && is_sep( path[2] )) || (len == 1 && is_sep( path[0] ))
         || (len == 2 && path[1] == ':') ) {
        info->st_mode = S_IFDIR;
        info->st_attr = 0x10;
        return 0;
    }
    xa_clear( &regs );
    regs.edx = (u32)path;
    regs.edi = (u32)rec;
    regs.ecx = FIND_ALL;                /* CL allowed, CH required: none */
    regs.esi = 1;                       /* DOS date and time */
    rc = lfn_call( &regs, 0x714E );
    if ( rc == 0 ) {
        u32 handle = AX( regs );

        attr = RD32( rec );
        stamp = RD32( rec + 0x14 );
        size = RD32( rec + 0x20 );
        xa_clear( &regs );
        regs.ebx = handle;
        xa_dos( &regs, 0x71A1 );
    } else if ( rc < 0 ) {
        xa_clear( &regs );
        regs.edx = (u32)rec;
        xa_dos( &regs, 0x1A00 );        /* the DTA, for the classic search */
        xa_clear( &regs );
        regs.edx = (u32)path;
        regs.ecx = FIND_ALL;
        if ( xa_dos( &regs, 0x4E00 ) != 0 ) {
            return -1;
        }
        attr = rec[0x15];
        stamp = ((u32)RD16( rec + 0x18 ) << 16) | RD16( rec + 0x16 );
        size = RD32( rec + 0x1A );
    } else {
        return -1;
    }
    info->st_attr = (unsigned)(attr & 0xFF);
    info->st_mode = (attr & 0x10) ? S_IFDIR : S_IFREG;
    info->st_size = (off_t)size;
    info->st_mtime = dos_to_time( (unsigned)(stamp >> 16), (unsigned)(stamp & 0xFFFF) );
    info->st_atime = info->st_ctime = info->st_mtime;
    return 0;
}
