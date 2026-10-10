/*
 * STREAM.C - bytes to be read: the IN, the file as one, memory as one.
 *
 * Everything that reads - the lexer, a filter, an image - reads an IN,
 * and a filter is an IN with another behind it, so a stream's data is
 * pulled through however many filters its dictionary names without any
 * of them holding more than a buffer.
 */
#include "pdf.h"

#define FILE_BUF    4096

typedef struct {
    u32 next;                   /* the offset of the next read */
    u32 left;                   /* bytes this IN may still read */
    u8  buf[FILE_BUF];
} FILEIN;

static u32 file_at = 0xFFFFFFFFUL;      /* where the handle is: a seek saved */

int in_more( IN *in )
{
    if ( in->at_eof ) {
        return -1;
    }
    if ( !in->fill( in ) || in->cur >= in->end ) {
        in->at_eof = 1;
        in->cur = in->end;
        return -1;
    }
    return *in->cur++;
}

int in_peek( IN *in )
{
    int ch;

    if ( in->cur < in->end ) {
        return *in->cur;
    }
    ch = in_more( in );
    if ( ch >= 0 ) {
        in->cur--;
    }
    return ch;
}

u32 in_read( IN *in, u8 *buf, u32 len )
{
    u32 done = 0, have;
    int ch;

    while ( done < len ) {
        have = (u32)(in->end - in->cur);
        if ( have == 0 ) {
            ch = in_more( in );
            if ( ch < 0 ) {
                break;
            }
            buf[done++] = (u8)ch;
            continue;
        }
        if ( have > len - done ) {
            have = len - done;
        }
        mem_cpy( buf + done, in->cur, have );
        in->cur += have;
        done += have;
    }
    return done;
}

u32 in_skip( IN *in, u32 count )
{
    u32 done = 0, have;

    while ( done < count ) {
        have = (u32)(in->end - in->cur);
        if ( have == 0 ) {
            if ( in_more( in ) < 0 ) {
                break;
            }
            done++;
            continue;
        }
        if ( have > count - done ) {
            have = count - done;
        }
        in->cur += have;
        done += have;
    }
    return done;
}

void in_close( IN *in )
{
    IN *src;

    while ( in ) {
        src = in->src;
        if ( in->done ) {
            in->done( in );
        }
        pool_unbig( in );
        in = src;
    }
}

IN *in_new( u32 extra, int (*fill)( IN * ), IN *src )
{
    IN *in = (IN *)pool_big( doc.page_pool, sizeof( IN ) + extra );

    in->fill = fill;
    in->src = src;
    in->cur = in->end = (u8 *)(in + 1);
    return in;
}

/* ------------------------------------------------------------------ */
/* the file                                                            */
/* ------------------------------------------------------------------ */

static int file_fill( IN *in )
{
    FILEIN *state = IN_STATE( in, FILEIN );
    u32 want = state->left < FILE_BUF ? state->left : FILE_BUF, done = 0;

    if ( want == 0 ) {
        return 0;
    }
    if ( file_at != state->next ) {
        if ( sys_seek( doc.file, state->next ) ) {
            file_at = 0xFFFFFFFFUL;
            return 0;
        }
    }
    if ( sys_read( doc.file, state->buf, want, &done ) || done == 0 ) {
        file_at = 0xFFFFFFFFUL;
        return 0;
    }
    state->next += done;
    state->left -= done;
    file_at = state->next;
    in->cur = state->buf;
    in->end = state->buf + done;
    in->pos = state->next;
    return 1;
}

IN *in_file( u32 pos, u32 len )
{
    IN *in = in_new( sizeof( FILEIN ), file_fill, NULL );
    FILEIN *state = IN_STATE( in, FILEIN );

    if ( pos > doc.size ) {
        pos = doc.size;
    }
    if ( len > doc.size - pos ) {
        len = doc.size - pos;
    }
    state->next = pos;
    state->left = len;
    in->pos = pos;
    return in;
}

u32 in_tell( IN *in )
{
    return in->pos - (u32)(in->end - in->cur);
}

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

static int mem_fill( IN *in )
{
    (void)in;
    return 0;
}

IN *in_mem( const u8 *data, u32 len )
{
    IN *in = in_new( 0, mem_fill, NULL );

    in->cur = (u8 *)data;
    in->end = (u8 *)data + len;
    return in;
}

/* ------------------------------------------------------------------ */
/* part of another IN                                                  */
/* ------------------------------------------------------------------ */

/*
 * An inline image's data is in the content stream, between ID and EI,
 * and the image wants to read it as it reads any other - through
 * filters, if it has them.  This is the content stream's own buffer
 * handed over a piece at a time, "limit" bytes of it at most, with
 * nothing copied: what the image has not read when it closes this is
 * given back, and the lexer carries on from there.
 */
typedef struct {
    IN  *parent;
    u32  left;
} BORROW;

static int borrow_fill( IN *in )
{
    BORROW *st = IN_STATE( in, BORROW );
    IN *parent = st->parent;
    u32 have;

    if ( st->left == 0 || in_peek( parent ) < 0 ) {
        return 0;
    }
    have = (u32)(parent->end - parent->cur);
    if ( have > st->left ) {
        have = st->left;
    }
    in->cur = parent->cur;
    in->end = parent->cur + have;
    parent->cur += have;
    st->left -= have;
    return 1;
}

static void borrow_done( IN *in )
{
    BORROW *st = IN_STATE( in, BORROW );

    st->parent->cur -= (u32)(in->end - in->cur);
}

IN *in_borrow( IN *parent, u32 limit )
{
    IN *in = in_new( sizeof( BORROW ), borrow_fill, NULL );
    BORROW *st = IN_STATE( in, BORROW );

    st->parent = parent;
    st->left = limit;
    in->done = borrow_done;
    return in;
}
