/*
 * STDIO.C - streams: opening, buffering, reading and writing
 *
 * TEXT STREAMS are translated a character at a time, as they are read
 * and written, so the buffer always holds the bytes of the file and
 * ftell() and fseek() deal in true file positions.  Reading, CR LF is
 * \n and Ctrl+Z is the end; writing, \n is CR LF.
 *
 * BUFFERING is decided at the first read or write: a stream on a
 * device (the console, a printer) is line-buffered, a file fully
 * buffered in BUFSIZ bytes, stderr unbuffered.  Reading a device
 * first flushes every line-buffered stream, so a prompt without a \n
 * is on the screen before the program waits for the answer.
 *
 * A stream may switch between reading and writing without the fseek()
 * C asks for in between: the switch puts the file position where the
 * program thinks it is.
 */
#include "pmlib.h"
#include "stream.h"
#include <stdlib.h>
#include <string.h>
#include <io.h>

#define CTRL_Z  0x1A

struct __pm_file __pm_files[FOPEN_MAX] = {
    { 0, SF_INUSE | SF_READ | SF_TEXT, NULL, 0, NULL, 0, 0, EOF, NULL, 0 },
    { 1, SF_INUSE | SF_WRITE | SF_TEXT, NULL, 0, NULL, 0, 0, EOF, NULL, 0 },
    { 2, SF_INUSE | SF_WRITE | SF_TEXT | SF_NBF | SF_SETBUF, NULL, 0, NULL, 0, 0, EOF, NULL, 0 }
};

FILE *stdin = &__pm_files[0];
FILE *stdout = &__pm_files[1];
FILE *stderr = &__pm_files[2];

static void flush_all( void )
{
    int i;

    for ( i = 0; i < FOPEN_MAX; i++ ) {
        if ( (__pm_files[i].flags & (SF_INUSE | SF_WRITING)) == (SF_INUSE | SF_WRITING) ) {
            __pm_stream_flush( &__pm_files[i] );
        }
    }
}

void __pm_stream_setup( FILE *fp )
{
    if ( fp->flags & SF_SETUP ) {
        return;
    }
    __pm_exit_flush = flush_all;
    if ( isatty( fp->handle ) ) {
        fp->flags |= SF_DEVICE;
    }
    if ( !(fp->flags & SF_SETBUF) ) {
        if ( fp->flags & SF_DEVICE ) {
            fp->flags |= SF_LBF;
        }
        fp->buf = (unsigned char *)malloc( BUFSIZ );
        if ( fp->buf != NULL ) {
            fp->size = BUFSIZ;
            fp->flags |= SF_OWNBUF;
        } else {
            fp->flags |= SF_NBF;
        }
    }
    if ( fp->flags & SF_NBF ) {
        fp->buf = &fp->one;
        fp->size = 1;
    }
    fp->ptr = fp->buf;
    fp->cnt = 0;
    fp->wlen = 0;
    fp->flags |= SF_SETUP;
}

int __pm_stream_flush( FILE *fp )
{
    size_t done = 0;
    int n;

    if ( !(fp->flags & SF_WRITING) ) {
        return 0;
    }
    while ( done < fp->wlen ) {
        n = write( fp->handle, fp->buf + done, (unsigned)(fp->wlen - done) );
        if ( n <= 0 ) {
            fp->flags |= SF_ERR;
            fp->wlen = 0;
            return EOF;
        }
        done += (size_t)n;
    }
    fp->wlen = 0;
    fp->flags &= ~SF_WRITING;
    return 0;
}

/* give up what is buffered for reading, and put the file position
   back where the program has read to */
static int drop_input( FILE *fp )
{
    long back;

    if ( !(fp->flags & SF_READING) ) {
        return 0;
    }
    back = (long)fp->cnt + (fp->ungot != EOF ? 1 : 0);
    fp->cnt = 0;
    fp->ungot = EOF;
    fp->flags &= ~SF_READING;
    if ( back && !(fp->flags & SF_DEVICE) && lseek( fp->handle, -back, SEEK_CUR ) < 0 ) {
        fp->flags |= SF_ERR;
        return EOF;
    }
    return 0;
}

/* get ready to read: 0, or EOF when the stream cannot be read now */
static int to_read( FILE *fp )
{
    if ( !(fp->flags & SF_READ) ) {
        fp->flags |= SF_ERR;
        errno = EBADF;
        return EOF;
    }
    __pm_stream_setup( fp );
    if ( (fp->flags & SF_WRITING) && __pm_stream_flush( fp ) == EOF ) {
        return EOF;
    }
    return 0;
}

static int to_write( FILE *fp )
{
    if ( !(fp->flags & SF_WRITE) ) {
        fp->flags |= SF_ERR;
        errno = EBADF;
        return EOF;
    }
    __pm_stream_setup( fp );
    if ( fp->flags & SF_READING ) {
        if ( drop_input( fp ) == EOF ) {
            return EOF;
        }
    }
    return 0;
}

/* fill the buffer: 0, or EOF at the end or on an error */
static int refill( FILE *fp )
{
    int n;
    int i;

    if ( fp->flags & SF_EOF ) {
        return EOF;
    }
    if ( fp->flags & SF_DEVICE ) {
        for ( i = 0; i < FOPEN_MAX; i++ ) {
            if ( (__pm_files[i].flags & (SF_INUSE | SF_LBF | SF_WRITING)) == (SF_INUSE | SF_LBF | SF_WRITING) ) {
                __pm_stream_flush( &__pm_files[i] );
            }
        }
    }
    n = read( fp->handle, fp->buf, (unsigned)fp->size );
    if ( n < 0 ) {
        fp->flags |= SF_ERR;
        return EOF;
    }
    if ( n == 0 ) {
        fp->flags |= SF_EOF;
        return EOF;
    }
    fp->ptr = fp->buf;
    fp->cnt = (size_t)n;
    fp->flags |= SF_READING;
    return 0;
}

/* the next byte of the file, untranslated */
static int raw_get( FILE *fp )
{
    if ( fp->cnt == 0 && refill( fp ) == EOF ) {
        return EOF;
    }
    fp->cnt--;
    return *fp->ptr++;
}

int fgetc( FILE *fp )
{
    int c;

    if ( fp->ungot != EOF ) {
        c = fp->ungot;
        fp->ungot = EOF;
        return c;
    }
    if ( !(fp->flags & SF_READING) && to_read( fp ) == EOF ) {
        return EOF;
    }
    c = raw_get( fp );
    if ( c == EOF || !(fp->flags & SF_TEXT) ) {
        return c;
    }
    if ( c == CTRL_Z ) {
        fp->flags |= SF_EOF;            /* and stay at the Ctrl+Z */
        fp->ptr--;
        fp->cnt++;
        return EOF;
    }
    if ( c == '\r' ) {
        c = raw_get( fp );
        if ( c == '\n' ) {
            return '\n';
        }
        if ( c != EOF ) {
            fp->ptr--;                  /* a CR on its own is a CR */
            fp->cnt++;
        } else {
            fp->flags &= ~SF_EOF;       /* the CR first; the end next time */
        }
        return '\r';
    }
    return c;
}

int getc( FILE *fp )
{
    return fgetc( fp );
}

int getchar( void )
{
    return fgetc( stdin );
}

int ungetc( int c, FILE *fp )
{
    if ( c == EOF || fp->ungot != EOF || !(fp->flags & SF_READ) ) {
        return EOF;
    }
    __pm_stream_setup( fp );
    if ( fp->flags & SF_WRITING ) {
        if ( __pm_stream_flush( fp ) == EOF ) {
            return EOF;
        }
    }
    fp->ungot = (unsigned char)c;
    fp->flags &= ~SF_EOF;
    fp->flags |= SF_READING;
    return (unsigned char)c;
}

char *fgets( char *buf, int size, FILE *fp )
{
    int n = 0;
    int c = 0;

    if ( size <= 0 ) {
        return NULL;
    }
    while ( n < size - 1 ) {
        c = fgetc( fp );
        if ( c == EOF ) {
            break;
        }
        buf[n++] = (char)c;
        if ( c == '\n' ) {
            break;
        }
    }
    if ( n == 0 && c == EOF ) {
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

size_t fread( void *buf, size_t size, size_t count, FILE *fp )
{
    unsigned char *out = (unsigned char *)buf;
    size_t want, got = 0;
    size_t take;
    int c;

    if ( size == 0 || count == 0 ) {
        return 0;
    }
    if ( count > (size_t)-1 / size ) {
        count = (size_t)-1 / size;
    }
    want = size * count;
    if ( fp->flags & SF_TEXT ) {
        while ( got < want && (c = fgetc( fp )) != EOF ) {
            out[got++] = (unsigned char)c;
        }
        return got / size;
    }
    if ( fp->ungot != EOF && want ) {
        out[got++] = (unsigned char)fp->ungot;
        fp->ungot = EOF;
    }
    if ( !(fp->flags & SF_READING) && to_read( fp ) == EOF ) {
        return got / size;
    }
    while ( got < want ) {
        if ( fp->cnt == 0 && refill( fp ) == EOF ) {
            break;
        }
        take = fp->cnt < want - got ? fp->cnt : want - got;
        memcpy( out + got, fp->ptr, take );
        fp->ptr += take;
        fp->cnt -= take;
        got += take;
    }
    return got / size;
}

/* one byte into the buffer, untranslated */
static int raw_put( FILE *fp, unsigned char c )
{
    if ( fp->wlen >= fp->size && __pm_stream_flush( fp ) == EOF ) {
        return EOF;
    }
    fp->buf[fp->wlen++] = c;
    fp->flags |= SF_WRITING;
    return 0;
}

int __pm_stream_write( FILE *fp, const char *s, size_t n )
{
    size_t i;
    int newline = 0;

    if ( !(fp->flags & SF_WRITING) && to_write( fp ) == EOF ) {
        return EOF;
    }
    for ( i = 0; i < n; i++ ) {
        if ( s[i] == '\n' ) {
            newline = 1;
            if ( (fp->flags & SF_TEXT) && raw_put( fp, '\r' ) == EOF ) {
                return EOF;
            }
        }
        if ( raw_put( fp, (unsigned char)s[i] ) == EOF ) {
            return EOF;
        }
    }
    if ( (fp->flags & SF_NBF) || ((fp->flags & SF_LBF) && newline) ) {
        return __pm_stream_flush( fp );
    }
    return 0;
}

int fputc( int c, FILE *fp )
{
    char ch = (char)c;

    return __pm_stream_write( fp, &ch, 1 ) == EOF ? EOF : (unsigned char)c;
}

int putc( int c, FILE *fp )
{
    return fputc( c, fp );
}

int putchar( int c )
{
    return fputc( c, stdout );
}

int fputs( const char *s, FILE *fp )
{
    return __pm_stream_write( fp, s, strlen( s ) ) == EOF ? EOF : 0;
}

int puts( const char *s )
{
    if ( __pm_stream_write( stdout, s, strlen( s ) ) == EOF ) {
        return EOF;
    }
    return __pm_stream_write( stdout, "\n", 1 ) == EOF ? EOF : 0;
}

size_t fwrite( const void *buf, size_t size, size_t count, FILE *fp )
{
    size_t total;

    if ( size == 0 || count == 0 ) {
        return 0;
    }
    if ( count > (size_t)-1 / size ) {
        count = (size_t)-1 / size;
    }
    total = size * count;
    if ( !(fp->flags & SF_TEXT) && !(fp->flags & SF_WRITING) && total >= fp->size
         && fp->size > 1 && to_write( fp ) == 0 ) {
        /* a big binary write goes straight to the file */
        int n = write( fp->handle, buf, (unsigned)total );

        if ( n < 0 ) {
            fp->flags |= SF_ERR;
            return 0;
        }
        return (size_t)n / size;
    }
    return __pm_stream_write( fp, (const char *)buf, total ) == EOF ? 0 : count;
}

int fflush( FILE *fp )
{
    int i;
    int result = 0;

    if ( fp == NULL ) {
        for ( i = 0; i < FOPEN_MAX; i++ ) {
            if ( (__pm_files[i].flags & SF_INUSE) && fflush( &__pm_files[i] ) == EOF ) {
                result = EOF;
            }
        }
        return result;
    }
    if ( fp->flags & SF_WRITING ) {
        return __pm_stream_flush( fp );
    }
    if ( fp->flags & SF_READING ) {
        return drop_input( fp );        /* fflush on input: what POSIX does */
    }
    return 0;
}

int setvbuf( FILE *fp, char *buf, int mode, size_t size )
{
    if ( fp->flags & SF_SETUP ) {
        return -1;                      /* only before the first I/O */
    }
    if ( mode != _IOFBF && mode != _IOLBF && mode != _IONBF ) {
        return -1;
    }
    fp->flags &= ~(SF_LBF | SF_NBF);
    fp->flags |= SF_SETBUF;
    if ( mode == _IONBF ) {
        fp->flags |= SF_NBF;
        return 0;
    }
    if ( size < 2 ) {
        size = BUFSIZ;
    }
    if ( buf == NULL ) {
        buf = (char *)malloc( size );
        if ( buf == NULL ) {
            fp->flags |= SF_NBF;
            return -1;
        }
        fp->flags |= SF_OWNBUF;
    }
    fp->buf = (unsigned char *)buf;
    fp->size = size;
    if ( mode == _IOLBF ) {
        fp->flags |= SF_LBF;
    }
    return 0;
}

void setbuf( FILE *fp, char *buf )
{
    setvbuf( fp, buf, buf ? _IOFBF : _IONBF, BUFSIZ );
}

int fileno( FILE *fp )
{
    return fp->handle;
}

int fseek( FILE *fp, long offset, int origin )
{
    if ( (fp->flags & SF_WRITING) && __pm_stream_flush( fp ) == EOF ) {
        return -1;
    }
    if ( fp->flags & SF_READING ) {
        if ( origin == SEEK_CUR ) {
            offset -= (long)fp->cnt + (fp->ungot != EOF ? 1 : 0);
        }
        fp->cnt = 0;
        fp->ungot = EOF;
        fp->flags &= ~SF_READING;
    }
    if ( lseek( fp->handle, offset, origin ) < 0 ) {
        return -1;
    }
    fp->flags &= ~SF_EOF;
    return 0;
}

long ftell( FILE *fp )
{
    long pos = lseek( fp->handle, 0, SEEK_CUR );

    if ( pos < 0 ) {
        return -1;
    }
    if ( fp->flags & SF_WRITING ) {
        return pos + (long)fp->wlen;
    }
    if ( fp->flags & SF_READING ) {
        return pos - (long)fp->cnt - (fp->ungot != EOF ? 1 : 0);
    }
    return pos;
}

void rewind( FILE *fp )
{
    fseek( fp, 0, SEEK_SET );
    fp->flags &= ~SF_ERR;
}

int fgetpos( FILE *fp, fpos_t *pos )
{
    *pos = ftell( fp );
    return *pos < 0 ? -1 : 0;
}

int fsetpos( FILE *fp, const fpos_t *pos )
{
    return fseek( fp, *pos, SEEK_SET );
}

int feof( FILE *fp )
{
    return (fp->flags & SF_EOF) != 0;
}

int ferror( FILE *fp )
{
    return (fp->flags & SF_ERR) != 0;
}

void clearerr( FILE *fp )
{
    fp->flags &= ~(SF_EOF | SF_ERR);
}

/* the mode string: O_* flags for open() and SF_* for the stream, or -1 */
static int parse_mode( const char *mode, int *sflags )
{
    int oflags;

    switch ( *mode++ ) {
    case 'r':
        oflags = O_RDONLY;
        *sflags = SF_READ;
        break;
    case 'w':
        oflags = O_WRONLY | O_CREAT | O_TRUNC;
        *sflags = SF_WRITE;
        break;
    case 'a':
        oflags = O_WRONLY | O_CREAT | O_APPEND;
        *sflags = SF_WRITE;
        break;
    default:
        return -1;
    }
    *sflags |= SF_TEXT;
    for ( ; *mode; mode++ ) {
        if ( *mode == '+' ) {
            oflags = (oflags & ~O_ACCMODE) | O_RDWR;
            *sflags |= SF_READ | SF_WRITE;
        } else if ( *mode == 'b' ) {
            *sflags &= ~SF_TEXT;
        } else if ( *mode == 't' ) {
            *sflags |= SF_TEXT;
        } else if ( *mode == 'x' ) {
            oflags |= O_EXCL;
        }
    }
    return oflags;
}

static void init_stream( FILE *fp, int handle, int sflags )
{
    memset( fp, 0, sizeof( *fp ) );
    fp->handle = handle;
    fp->flags = SF_INUSE | sflags;
    fp->ungot = EOF;
}

static FILE *free_slot( void )
{
    int i;

    for ( i = 0; i < FOPEN_MAX; i++ ) {
        if ( !(__pm_files[i].flags & SF_INUSE) ) {
            return &__pm_files[i];
        }
    }
    errno = EMFILE;
    return NULL;
}

FILE *fopen( const char *name, const char *mode )
{
    FILE *fp = free_slot();
    int sflags;
    int oflags = parse_mode( mode, &sflags );
    int handle;

    if ( fp == NULL ) {
        return NULL;
    }
    if ( oflags < 0 ) {
        errno = EINVAL;
        return NULL;
    }
    handle = open( name, oflags, 0666 );
    if ( handle < 0 ) {
        return NULL;
    }
    init_stream( fp, handle, sflags );
    return fp;
}

FILE *fdopen( int handle, const char *mode )
{
    FILE *fp = free_slot();
    int sflags;

    if ( fp == NULL ) {
        return NULL;
    }
    if ( parse_mode( mode, &sflags ) < 0 ) {
        errno = EINVAL;
        return NULL;
    }
    init_stream( fp, handle, sflags );
    return fp;
}

/* close what the stream holds, keeping the slot in use or not */
static int release( FILE *fp )
{
    int result = 0;

    if ( (fp->flags & SF_WRITING) && __pm_stream_flush( fp ) == EOF ) {
        result = EOF;
    }
    if ( close( fp->handle ) < 0 ) {
        result = EOF;
    }
    if ( fp->flags & SF_OWNBUF ) {
        free( fp->buf );
    }
    if ( fp->tmpname != NULL ) {
        unlink( fp->tmpname );
        free( fp->tmpname );
    }
    fp->flags = 0;
    return result;
}

int fclose( FILE *fp )
{
    if ( !(fp->flags & SF_INUSE) ) {
        errno = EBADF;
        return EOF;
    }
    return release( fp );
}

FILE *freopen( const char *name, const char *mode, FILE *fp )
{
    int sflags;
    int oflags = parse_mode( mode, &sflags );
    int handle;

    if ( fp->flags & SF_INUSE ) {
        release( fp );
    }
    if ( oflags < 0 ) {
        errno = EINVAL;
        return NULL;
    }
    handle = open( name, oflags, 0666 );
    if ( handle < 0 ) {
        return NULL;
    }
    init_stream( fp, handle, sflags );
    return fp;
}
