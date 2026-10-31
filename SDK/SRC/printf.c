/*
 * PRINTF.C - printf() and its family, on FMT.C's engine
 */
#include "pmlib.h"
#include "fmt.h"
#include "stream.h"
#include <string.h>
#include <limits.h>

static int to_stream( void *ctx, const char *s, size_t n )
{
    return __pm_stream_write( (FILE *)ctx, s, n ) == EOF ? -1 : 0;
}

/* a string: what fits in size - 1 bytes is kept, the rest only counted */
typedef struct {
    char   *buf;
    size_t  size;
    size_t  len;
} STRBUF;

static int to_string( void *ctx, const char *s, size_t n )
{
    STRBUF *sb = (STRBUF *)ctx;
    size_t room;

    if ( sb->size > 0 && sb->len < sb->size - 1 ) {
        room = sb->size - 1 - sb->len;
        memcpy( sb->buf + sb->len, s, n < room ? n : room );
    }
    sb->len += n;
    return 0;
}

int vfprintf( FILE *fp, const char *fmt, va_list args )
{
    return __pm_vformat( to_stream, fp, fmt, args );
}

int vprintf( const char *fmt, va_list args )
{
    return __pm_vformat( to_stream, stdout, fmt, args );
}

int vsnprintf( char *buf, size_t size, const char *fmt, va_list args )
{
    STRBUF sb;
    int n;

    sb.buf = buf;
    sb.size = size;
    sb.len = 0;
    n = __pm_vformat( to_string, &sb, fmt, args );
    if ( size > 0 ) {
        buf[sb.len < size - 1 ? sb.len : size - 1] = '\0';
    }
    return n;
}

int vsprintf( char *buf, const char *fmt, va_list args )
{
    return vsnprintf( buf, INT_MAX, fmt, args );
}

int printf( const char *fmt, ... )
{
    va_list args;
    int n;

    va_start( args, fmt );
    n = __pm_vformat( to_stream, stdout, fmt, args );
    va_end( args );
    return n;
}

int fprintf( FILE *fp, const char *fmt, ... )
{
    va_list args;
    int n;

    va_start( args, fmt );
    n = __pm_vformat( to_stream, fp, fmt, args );
    va_end( args );
    return n;
}

int sprintf( char *buf, const char *fmt, ... )
{
    va_list args;
    int n;

    va_start( args, fmt );
    n = vsnprintf( buf, INT_MAX, fmt, args );
    va_end( args );
    return n;
}

int snprintf( char *buf, size_t size, const char *fmt, ... )
{
    va_list args;
    int n;

    va_start( args, fmt );
    n = vsnprintf( buf, size, fmt, args );
    va_end( args );
    return n;
}
