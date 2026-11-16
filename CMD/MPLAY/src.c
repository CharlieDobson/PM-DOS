/*
 * SRC.C - a file read through a buffer, for the decoders (DEC.H).
 *
 * A MEDIA FILE IS READ ONCE, FORWARDS, IN SMALL BITES.  A decoder asks
 * for a frame header, then a frame, then the next header, and a call
 * to DOS for each of those would cost more than the decoding on a slow
 * machine.  So the file comes in SRC_BUFSIZE bytes at a time and the
 * bites are taken from memory.
 *
 * SEEKING INSIDE THE BUFFER IS FREE.  Moving back a few bytes to look
 * at a header again, or skipping a tag that is already in memory, does
 * not touch the disk; only a position outside the buffer makes the
 * next read go to DOS.
 *
 * A PEEK MAY NEED BYTES PAST THE END OF THE BUFFER.  Then what is left
 * of the buffer is moved to its front and the rest is read in behind
 * it, so the bytes asked for are always in one piece.
 */
#include "rt.h"
#include "sys.h"
#include "dec.h"

/* Fill the buffer starting at file position "at".  0, or -1 when DOS
   could not read (the error flag is set) */
static int src_fill( SRC *src, u32 at )
{
    u32 done = 0;

    src->have = 0;
    src->buf_pos = at;
    if ( at >= src->size ) {
        return 0;
    }
    if ( sys_seek( src->handle, at ) != 0 ) {
        src->error = 1;
        return -1;
    }
    if ( sys_read( src->handle, src->buf, SRC_BUFSIZE, &done ) != 0 ) {
        src->error = 1;
        return -1;
    }
    if ( done > SRC_BUFSIZE ) {
        done = SRC_BUFSIZE;
    }
    src->have = done;
    return 0;
}

int src_open( SRC *src, const char *path )
{
    int rc;

    mem_set( src, 0, sizeof( SRC ) );
    src->buf = (u8 *)try_alloc( SRC_BUFSIZE );
    if ( src->buf == NULL ) {
        return ERR_NOMEM;
    }
    rc = sys_open_read( path, &src->handle );
    if ( rc != 0 ) {
        xfree( src->buf );
        src->buf = NULL;
        return rc;
    }
    rc = sys_file_size( src->handle, &src->size );
    if ( rc != 0 ) {
        sys_close( src->handle );
        xfree( src->buf );
        src->buf = NULL;
        return rc;
    }
    src->is_open = 1;
    return 0;
}

void src_close( SRC *src )
{
    if ( src->is_open ) {
        sys_close( src->handle );
        src->is_open = 0;
    }
    if ( src->buf != NULL ) {
        xfree( src->buf );
        src->buf = NULL;
    }
    src->have = 0;
}

u32 src_read( SRC *src, void *dst, u32 len )
{
    u8 *out = (u8 *)dst;
    u32 got = 0, avail, off;

    if ( !src->is_open ) {
        return 0;
    }
    if ( len > src->size - src->pos ) {
        len = src->size - src->pos;
    }
    while ( got < len ) {
        if ( src->pos < src->buf_pos || src->pos >= src->buf_pos + src->have ) {
            if ( src_fill( src, src->pos ) != 0 || src->have == 0 ) {
                break;
            }
        }
        off = src->pos - src->buf_pos;
        avail = src->have - off;
        if ( avail > len - got ) {
            avail = len - got;
        }
        mem_cpy( out + got, src->buf + off, avail );
        got += avail;
        src->pos += avail;
    }
    return got;
}

int src_seek( SRC *src, u32 pos )
{
    if ( pos > src->size ) {
        src->pos = src->size;
        return -1;
    }
    src->pos = pos;
    return 0;
}

int src_skip( SRC *src, u32 len )
{
    if ( len > src->size - src->pos ) {
        src->pos = src->size;
        return -1;
    }
    src->pos += len;
    return 0;
}

int src_byte( SRC *src )
{
    if ( !src->is_open || src->pos >= src->size ) {
        return -1;
    }
    if ( src->pos < src->buf_pos || src->pos >= src->buf_pos + src->have ) {
        if ( src_fill( src, src->pos ) != 0 || src->have == 0 ) {
            return -1;
        }
    }
    return src->buf[src->pos++ - src->buf_pos];
}

u32 src_tell( const SRC *src )
{
    return src->pos;
}

u32 src_left( const SRC *src )
{
    return src->size - src->pos;
}

const u8 *src_peek( SRC *src, u32 len )
{
    u32 off, keep, done = 0;

    if ( !src->is_open || len > SRC_PEEK || len > src->size - src->pos ) {
        return NULL;
    }
    if ( src->pos >= src->buf_pos && src->pos + len <= src->buf_pos + src->have ) {
        return src->buf + (src->pos - src->buf_pos);
    }
    if ( src->pos >= src->buf_pos && src->pos < src->buf_pos + src->have ) {
        /* part of it is here: move that part to the front, read the rest behind it */
        off = src->pos - src->buf_pos;
        keep = src->have - off;
        mem_cpy( src->buf, src->buf + off, keep );
        src->buf_pos = src->pos;
        src->have = keep;
        if ( sys_seek( src->handle, src->pos + keep ) != 0
             || sys_read( src->handle, src->buf + keep, SRC_BUFSIZE - keep, &done ) != 0 ) {
            src->error = 1;
            done = 0;
        }
        if ( done > SRC_BUFSIZE - keep ) {
            done = SRC_BUFSIZE - keep;
        }
        src->have = keep + done;
    } else if ( src_fill( src, src->pos ) != 0 ) {
        return NULL;
    }
    if ( src->have < len ) {
        return NULL;
    }
    return src->buf;
}
