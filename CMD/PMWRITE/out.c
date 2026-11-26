/*
 * OUT.C - bytes going to a file or into memory (FMT.H says how it is
 * used).
 *
 * A FILE'S BYTES go through 4K of buffer.  Going back to fill in a
 * length (out_seek) empties the buffer and moves the file, and what is
 * written from there overwrites; the end is remembered, so going
 * forward again to it is another out_seek.
 *
 * MEMORY doubles as it fills.
 */
#include "fmt.h"
#include "sys.h"

#define BUF_LEN         4096
#define MEM_FIRST       4096

struct OUT {
    int          in_mem;
    int          error;
    uint32_t     pos;           /* where the next byte goes */
    uint32_t     size;          /* the furthest any byte has gone */
    /* a file */
    uint32_t     handle;
    uint32_t     buf_at;        /* the file offset buf[0] is for */
    unsigned int buf_len;
    uint8_t     *buf;
    /* memory */
    uint8_t     *data;
    unsigned int cap;
};

OUT *out_file( const char *path )
{
    OUT *out = (OUT *)try_alloc( sizeof( OUT ) + BUF_LEN );

    if ( out == NULL ) {
        return NULL;
    }
    mem_set( out, 0, sizeof( OUT ) );
    out->buf = (uint8_t *)(out + 1);
    if ( sys_create( path, &out->handle ) != 0 ) {
        xfree( out );
        return NULL;
    }
    return out;
}

OUT *out_mem( void )
{
    OUT *out = (OUT *)try_alloc( sizeof( OUT ) );

    if ( out == NULL ) {
        return NULL;
    }
    mem_set( out, 0, sizeof( OUT ) );
    out->in_mem = 1;
    return out;
}

static void file_flush( OUT *out )
{
    uint32_t done;
    int rc;

    if ( out->buf_len && !out->error ) {
        rc = sys_write( out->handle, out->buf, out->buf_len, &done );
        if ( rc != 0 ) {
            out->error = rc;
        } else if ( done != out->buf_len ) {
            out->error = ERR_DISKFULL;
        }
    }
    out->buf_at += out->buf_len;
    out->buf_len = 0;
}

int out_close( OUT *out )
{
    int error;

    if ( out == NULL ) {
        return WE_NOMEM;
    }
    if ( out->in_mem ) {
        xfree( out->data );
    } else {
        file_flush( out );
        if ( sys_close( out->handle ) != 0 && !out->error ) {
            out->error = ERR_ACCESS;
        }
    }
    error = out->error;
    xfree( out );
    return error;
}

int out_error( const OUT *out )
{
    return out == NULL ? WE_NOMEM : out->error;
}

void out_fail( OUT *out, int error )
{
    if ( out != NULL && !out->error ) {
        out->error = error;
    }
}

static int mem_room( OUT *out, unsigned int need )
{
    unsigned int cap;
    uint8_t *data;

    if ( need <= out->cap ) {
        return 1;
    }
    cap = out->cap ? out->cap : MEM_FIRST;
    while ( cap < need ) {
        if ( cap >= 0x40000000UL ) {
            out->error = WE_NOMEM;
            return 0;
        }
        cap *= 2;
    }
    data = (uint8_t *)try_realloc( out->data, cap );
    if ( data == NULL ) {
        out->error = WE_NOMEM;
        return 0;
    }
    out->data = data;
    out->cap = mem_size( data );
    return 1;
}

void out_put( OUT *out, const void *data, unsigned int len )
{
    const uint8_t *src = (const uint8_t *)data;
    unsigned int part;

    if ( out == NULL || out->error || len == 0 ) {
        return;
    }
    if ( len > 0xFFFFFFFFUL - out->pos ) {
        out->error = WE_NOMEM;
        return;
    }
    if ( out->in_mem ) {
        if ( !mem_room( out, out->pos + len ) ) {
            return;
        }
        mem_cpy( out->data + out->pos, src, len );
        out->pos += len;
    } else {
        while ( len ) {
            part = BUF_LEN - out->buf_len;
            if ( part > len ) {
                part = len;
            }
            mem_cpy( out->buf + out->buf_len, src, part );
            out->buf_len += part;
            out->pos += part;
            src += part;
            len -= part;
            if ( out->buf_len == BUF_LEN ) {
                file_flush( out );
                if ( out->error ) {
                    return;
                }
            }
        }
    }
    if ( out->pos > out->size ) {
        out->size = out->pos;
    }
}

void out_byte( OUT *out, int value )
{
    uint8_t one = (uint8_t)value;

    out_put( out, &one, 1 );
}

void out_le16( OUT *out, uint32_t value )
{
    uint8_t raw[2];

    raw[0] = (uint8_t)value;
    raw[1] = (uint8_t)(value >> 8);
    out_put( out, raw, 2 );
}

void out_le32( OUT *out, uint32_t value )
{
    uint8_t raw[4];

    raw[0] = (uint8_t)value;
    raw[1] = (uint8_t)(value >> 8);
    raw[2] = (uint8_t)(value >> 16);
    raw[3] = (uint8_t)(value >> 24);
    out_put( out, raw, 4 );
}

void out_be16( OUT *out, uint32_t value )
{
    uint8_t raw[2];

    raw[0] = (uint8_t)(value >> 8);
    raw[1] = (uint8_t)value;
    out_put( out, raw, 2 );
}

void out_be32( OUT *out, uint32_t value )
{
    uint8_t raw[4];

    raw[0] = (uint8_t)(value >> 24);
    raw[1] = (uint8_t)(value >> 16);
    raw[2] = (uint8_t)(value >> 8);
    raw[3] = (uint8_t)value;
    out_put( out, raw, 4 );
}

void out_fill( OUT *out, int value, unsigned int count )
{
    uint8_t block[64];
    unsigned int part;

    mem_set( block, value, sizeof( block ) );
    while ( count ) {
        part = count > sizeof( block ) ? sizeof( block ) : count;
        out_put( out, block, part );
        count -= part;
    }
}

void out_str( OUT *out, const char *text )
{
    out_put( out, text, str_len( text ) );
}

/* what a line can come to when a caller hands a long string to %s */
#define FMT_MOST        0x100000UL

void __cdecl out_fmt( OUT *out, const char *fmt, ... )
{
    char line[512];
    char *big;
    unsigned int size = sizeof( line );
    int len;

    len = vfmt( line, sizeof( line ), fmt, VA_START( fmt ) );
    if ( (unsigned int)len + 1 < sizeof( line ) ) {
        out_put( out, line, (unsigned int)len );
        return;
    }
    /* the line filled the buffer, so some of it may be missing: made
       again in a block that is big enough, and never written cut */
    while ( out != NULL && !out->error ) {
        size *= 4;
        big = size <= FMT_MOST ? (char *)try_alloc( size ) : NULL;
        if ( big == NULL ) {
            out->error = WE_NOMEM;
            return;
        }
        len = vfmt( big, size, fmt, VA_START( fmt ) );
        if ( (unsigned int)len + 1 < size ) {
            out_put( out, big, (unsigned int)len );
            xfree( big );
            return;
        }
        xfree( big );
    }
}

uint32_t out_tell( const OUT *out )
{
    return out == NULL ? 0 : out->pos;
}

uint32_t out_size( const OUT *out )
{
    return out == NULL ? 0 : out->size;
}

void out_seek( OUT *out, uint32_t pos )
{
    int rc;

    if ( out == NULL || out->error || pos == out->pos ) {
        return;
    }
    if ( pos > out->size ) {
        out->error = ERR_ACCESS;
        return;
    }
    if ( !out->in_mem ) {
        file_flush( out );
        if ( out->error ) {
            return;
        }
        rc = sys_seek( out->handle, pos );
        if ( rc != 0 ) {
            out->error = rc;
            return;
        }
        out->buf_at = pos;
    }
    out->pos = pos;
}

uint8_t *out_data( OUT *out, uint32_t *len )
{
    if ( out == NULL || !out->in_mem ) {
        *len = 0;
        return NULL;
    }
    *len = out->size;
    return out->data;
}

uint8_t *out_take( OUT *out, uint32_t *len )
{
    uint8_t *data = out_data( out, len );

    if ( data != NULL ) {
        out->data = NULL;
        out->cap = out->pos = out->size = 0;
    }
    return data;
}
