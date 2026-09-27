/*
 * DOC.C - a file in memory.
 *
 * AN ARRAY OF LINES.  Every edit EDIT makes is to one line or joins or
 * splits two, and a line of a few dozen bytes is cheap to change where
 * a gap in a whole file is not when the file is most of the memory
 * there is: a program slot is half a megabyte, and the editor, nine
 * files and a clipboard share it.
 *
 * AND MEMORY IS WHAT DECIDES HOW BIG A FILE CAN BE, so a loaded file
 * costs as little over its own size as it can: its lines are packed
 * end to end into a few large POOLS, each line entry is eight bytes,
 * and the entries are counted before they are made so the array is the
 * right size first time.  A line leaves its pool for a block of its
 * own the first time an edit makes it longer (cap 0 means "pooled").
 * A block per line from the start, grown by doubling, cost more than
 * half as much again as the file - a 477K file did not fit, and a
 * 300K one of short lines would not have either.
 *
 * WHAT IS LOADED IS WHAT IS SAVED.  A line is the bytes between line
 * ends, tabs and all - they are shown at their tab stops, not turned
 * into spaces - and CR LF, a bare LF and a bare CR each end a line.  A
 * file saved without changes comes out with CR LF line ends and is
 * otherwise the file that went in: a last line with no line end after
 * it still has none, and one that had one gets it back, because that
 * file ends with an empty line.
 *
 * /nnn (binary) is the other way round: nothing ends a line, every
 * byte is text, and the file is cut into lines nnn bytes long just to
 * show it.  Saving puts the lines back together with nothing between
 * them, so the bytes are the file.
 *
 * Everything that can run out of memory says so rather than stopping
 * the program: an editor that exits when it cannot grow a line takes
 * every unsaved file with it.
 */
#include "edit.h"

int tab_size = 8;

#define POOL_SIZE   32768u
#define LOAD_BUF    8192u       /* small: it is freed again, and a hole below
                                   the pools is memory no file can use */

/* a line's own block, big enough for "need" bytes - moving it out of
   its pool if it is still there */
static int line_reserve( LINE *line, u32 need )
{
    u32 cap;
    char *grown;

    if ( need <= line->cap ) {
        return 1;
    }
    if ( line->cap ) {
        cap = line->cap * 2u;
    } else {
        cap = need < 16 ? 16 : need + need / 4;
    }
    while ( cap < need ) {
        cap *= 2;
    }
    if ( cap > 65535u ) {
        cap = 65535u;
    }
    grown = (char *)try_alloc( cap );
    if ( grown == NULL ) {
        grown = (char *)try_alloc( need );
        if ( grown == NULL ) {
            return 0;
        }
        cap = need;
    }
    if ( line->len ) {
        mem_cpy( grown, line->text, line->len );
    }
    if ( line->cap ) {
        xfree( line->text );                    /* a pooled line's is not ours */
    }
    line->text = grown;
    line->cap = (u16)cap;
    return 1;
}

static void line_release( LINE *line )
{
    if ( line->cap ) {
        xfree( line->text );
    }
    line->text = NULL;
    line->len = line->cap = 0;
}

static int lines_reserve( DOC *doc, u32 need )
{
    u32 cap;
    LINE *grown;

    if ( need <= doc->cap ) {
        return 1;
    }
    cap = doc->cap ? doc->cap + doc->cap / 2 + 16 : 64;
    if ( cap < need ) {
        cap = need;
    }
    grown = (LINE *)try_alloc( cap * sizeof( LINE ) );
    if ( grown == NULL ) {
        cap = need;
        grown = (LINE *)try_alloc( cap * sizeof( LINE ) );
        if ( grown == NULL ) {
            return 0;
        }
    }
    if ( doc->count ) {
        mem_cpy( grown, doc->lines, doc->count * sizeof( LINE ) );
    }
    mem_set( grown + doc->count, 0, (cap - doc->count) * sizeof( LINE ) );
    xfree( doc->lines );
    doc->lines = grown;
    doc->cap = cap;
    return 1;
}

static void free_lines( DOC *doc )
{
    u32 index;

    for ( index = 0; index < doc->count; index++ ) {
        line_release( &doc->lines[index] );
    }
    for ( index = 0; index < doc->npools; index++ ) {
        xfree( doc->pools[index] );
    }
    xfree( doc->pools );
    doc->pools = NULL;
    doc->npools = doc->pool_cap = 0;
    xfree( doc->lines );
    doc->lines = NULL;
    doc->count = doc->cap = 0;
}

/* a new pool of "size" bytes, remembered so it is freed with the file */
static char *pool_new( DOC *doc, u32 size )
{
    char **grown;
    char *pool;

    if ( doc->npools == doc->pool_cap ) {
        u32 cap = doc->pool_cap ? doc->pool_cap * 2 : 16;
        grown = (char **)try_alloc( cap * sizeof( char * ) );
        if ( grown == NULL ) {
            return NULL;
        }
        if ( doc->npools ) {
            mem_cpy( grown, doc->pools, doc->npools * sizeof( char * ) );
        }
        xfree( doc->pools );
        doc->pools = grown;
        doc->pool_cap = cap;
    }
    pool = (char *)try_alloc( size );
    if ( pool ) {
        doc->pools[doc->npools++] = pool;
    }
    return pool;
}

DOC *doc_new( void )
{
    DOC *doc = (DOC *)try_alloc( sizeof( DOC ) );

    if ( doc == NULL ) {
        return NULL;
    }
    mem_set( doc, 0, sizeof( DOC ) );
    if ( !lines_reserve( doc, 1 ) ) {
        xfree( doc );
        return NULL;
    }
    doc->count = 1;
    return doc;
}

void doc_free( DOC *doc )
{
    if ( doc ) {
        free_lines( doc );
        xfree( doc );
    }
}

LINE *doc_line( DOC *doc, u32 line )
{
    return &doc->lines[line];
}

/* the title: the name without its directory - or, with /S, the 8.3 */
void doc_set_path( DOC *doc, const char *path )
{
    char shortform[PATH_MAX];
    const char *use = path;
    const char *last;

    str_cpyn( doc->path, path, sizeof( doc->path ) );
    if ( short_names && sys_truename( path, shortform, 1 ) == 0 ) {
        use = shortform;
    }
    last = str_rchr( use, '\\' );
    str_cpyn( doc->name, last ? last + 1 : use, sizeof( doc->name ) );
}

/* ------------------------------------------------------------------ */
/* loading: the lines packed into pools as they are read               */
/* ------------------------------------------------------------------ */

typedef struct {
    DOC  *doc;
    char *pool;             /* the pool being filled */
    u32   used, size;       /* its finished lines, and how big it is */
    u32   len;              /* the line being read, at pool + used */
    u32   left;             /* bytes of the file still to come */
} LOADER;

/* room for one more byte of the line being read: a new pool when this
   one is full, with the line so far moved into it */
static int ld_room( LOADER *ld )
{
    char *fresh;
    u32 size;

    if ( ld->pool && ld->used + ld->len < ld->size ) {
        return 1;
    }
    size = ld->left + ld->len + 64;             /* no bigger than what is left */
    if ( size > POOL_SIZE ) {
        size = POOL_SIZE;
    }
    fresh = pool_new( ld->doc, size );
    if ( fresh == NULL ) {
        return 0;
    }
    if ( ld->len ) {
        mem_cpy( fresh, ld->pool + ld->used, ld->len );
    }
    ld->pool = fresh;
    ld->used = 0;
    ld->size = size;
    return 1;
}

/* the line is finished: an entry for it, pointing into the pool */
static int ld_end( LOADER *ld )
{
    DOC *doc = ld->doc;
    LINE *line;

    if ( !lines_reserve( doc, doc->count + 1 ) ) {
        return 0;
    }
    line = &doc->lines[doc->count++];
    line->text = ld->len ? ld->pool + ld->used : NULL;
    line->len = (u16)ld->len;
    line->cap = 0;
    ld->used += ld->len;
    ld->len = 0;
    return 1;
}

static int ld_byte( LOADER *ld, int ch )
{
    if ( ld->len >= MAX_LINE || (ld->doc->binary && ld->len >= (u32)ld->doc->binary) ) {
        if ( !ld->doc->binary ) {
            ld->doc->split = 1;                 /* saving will add a line end */
        }
        if ( !ld_end( ld ) ) {
            return 0;
        }
    }
    if ( !ld_room( ld ) ) {
        return 0;
    }
    ld->pool[ld->used + ld->len++] = (char)ch;
    return 1;
}

/* Two passes: the first counts the lines, so the array of them is the
   right size before any is made; the second makes them.  The second
   reads every byte into a pool, so an array grown as it went would
   have been the one thing still allocated twice over. */
int doc_load( DOC *doc, const char *path )
{
    LOADER ld;
    u8 *buf;
    u32 handle, got, pos, size = 0, lines = 1;
    int rc, ok = 1, last_cr = 0, pass;

    rc = sys_open_read( path, &handle );
    if ( rc != 0 ) {
        return rc;
    }
    buf = (u8 *)try_alloc( LOAD_BUF );
    if ( buf == NULL ) {
        sys_close( handle );
        return -1;
    }
    free_lines( doc );
    sys_file_size( handle, &size );
    mem_set( &ld, 0, sizeof( ld ) );
    ld.doc = doc;
    ld.left = size;
    for ( pass = 0; pass < 2 && ok && rc == 0; pass++ ) {
        if ( pass == 1 ) {
            if ( doc->binary ) {
                lines = size / (u32)doc->binary + 1;
            }
            if ( !lines_reserve( doc, lines + 16 ) ) {
                ok = 0;
                break;
            }
            sys_rewind( handle );
            last_cr = 0;
            ld.left = size;                     /* the count read it all once */
        }
        for ( ;; ) {
            rc = sys_read( handle, buf, LOAD_BUF, &got );
            if ( rc != 0 || got == 0 || !ok ) {
                break;
            }
            ld.left = ld.left > got ? ld.left - got : 0;
            for ( pos = 0; pos < got && ok; pos++ ) {
                if ( doc->binary ) {
                    if ( pass == 1 ) {
                        ok = ld_byte( &ld, buf[pos] );
                    }
                    continue;
                }
                if ( buf[pos] == '\n' ) {
                    if ( !last_cr ) {
                        if ( pass == 0 ) {
                            lines++;
                        } else {
                            ok = ld_end( &ld );
                        }
                    }
                    last_cr = 0;
                } else if ( buf[pos] == '\r' ) {
                    if ( pass == 0 ) {
                        lines++;
                    } else {
                        ok = ld_end( &ld );
                    }
                    last_cr = 1;
                } else {
                    if ( pass == 1 ) {
                        ok = ld_byte( &ld, buf[pos] );
                    }
                    last_cr = 0;
                }
            }
        }
    }
    if ( ok && rc == 0 ) {
        ok = ld_end( &ld );                     /* the last line, empty or not */
    }
    xfree( buf );
    sys_close( handle );
    if ( !ok || rc != 0 ) {
        free_lines( doc );
        lines_reserve( doc, 1 );
        doc->count = 1;
        return ok ? rc : -1;
    }
    doc_set_path( doc, path );
    doc->modified = 0;
    return 0;
}

int doc_save( DOC *doc, const char *path )
{
    u8 *buf;
    u32 handle, used = 0, done, index, pos;
    LINE *line;
    int rc;

    buf = (u8 *)try_alloc( 16384 );
    if ( buf == NULL ) {
        return ERR_NOMEM;
    }
    rc = sys_create( path, &handle );
    if ( rc != 0 ) {
        xfree( buf );
        return rc;
    }
    for ( index = 0; index < doc->count && rc == 0; index++ ) {
        line = &doc->lines[index];
        for ( pos = 0; pos < line->len && rc == 0; pos++ ) {
            buf[used++] = (u8)line->text[pos];
            if ( used == 16384 ) {
                rc = sys_write( handle, buf, used, &done );
                used = 0;
            }
        }
        if ( !doc->binary && index + 1 < doc->count && rc == 0 ) {
            buf[used++] = '\r';
            if ( used == 16384 ) {
                rc = sys_write( handle, buf, used, &done );
                used = 0;
            }
            buf[used++] = '\n';
            if ( used == 16384 && rc == 0 ) {
                rc = sys_write( handle, buf, used, &done );
                used = 0;
            }
        }
    }
    if ( used && rc == 0 ) {
        rc = sys_write( handle, buf, used, &done );
    }
    if ( sys_close( handle ) != 0 && rc == 0 ) {
        rc = ERR_ACCESS;
    }
    xfree( buf );
    if ( rc == 0 ) {
        doc_set_path( doc, path );
        doc->modified = 0;
    }
    return rc;
}

u32 doc_size( DOC *doc )
{
    u32 index, size = 0;

    for ( index = 0; index < doc->count; index++ ) {
        size += doc->lines[index].len;
    }
    if ( !doc->binary && doc->count ) {
        size += (doc->count - 1) * 2;
    }
    return size;
}

/* ------------------------------------------------------------------ */
/* columns: a tab reaches the next tab stop                            */
/* ------------------------------------------------------------------ */

/* In a binary file (/nnn) a tab is a byte like any other, one column
   wide: a line there is exactly nnn bytes of the file, and a tab that
   reached a tab stop would push the rest of the line out of place. */
u32 doc_col_of( DOC *doc, LINE *line, u32 index )
{
    u32 pos, col = 0;

    for ( pos = 0; pos < index && pos < line->len; pos++ ) {
        if ( line->text[pos] == '\t' && !doc->binary ) {
            col = (col / tab_size + 1) * tab_size;
        } else {
            col++;
        }
    }
    if ( index > line->len ) {
        col += index - line->len;
    }
    return col;
}

/* the byte at a column - the one a tab starts at, for a column inside
   the tab - or past the end by as many columns as it is past */
u32 doc_index_of( DOC *doc, LINE *line, u32 col )
{
    u32 pos, at = 0, width;

    for ( pos = 0; pos < line->len; pos++ ) {
        width = line->text[pos] == '\t' && !doc->binary ? (at / tab_size + 1) * tab_size - at : 1;
        if ( at + width > col ) {
            return pos;
        }
        at += width;
    }
    return line->len + (col - at);
}

/* ------------------------------------------------------------------ */
/* editing                                                             */
/* ------------------------------------------------------------------ */

/* "text" (with '\n' between lines) into the document at "at", which is
   moved to the end of it.  0 when there was no memory - and then the
   document is as it was. */
int doc_insert( DOC *doc, POS *at, const char *text, u32 len )
{
    LINE *line = &doc->lines[at->line];
    u32 breaks = 0, pos, seg, index, tail_len;
    LINE *fresh;
    char *tail;

    for ( pos = 0; pos < len; pos++ ) {
        if ( text[pos] == '\n' ) {
            breaks++;
        }
    }
    if ( at->col > line->len ) {                /* past the end: spaces first */
        if ( !line_reserve( line, at->col ) ) {
            return 0;
        }
        mem_set( line->text + line->len, ' ', at->col - line->len );
        line->len = at->col;
    }
    if ( breaks == 0 ) {
        if ( line->len + len > MAX_LINE || !line_reserve( line, line->len + len ) ) {
            return 0;
        }
        mem_cpy( line->text + at->col + len, line->text + at->col, line->len - at->col );
        mem_cpy( line->text + at->col, text, len );
        line->len += len;
        at->col += len;
        doc->modified = 1;
        return 1;
    }

    /* more than one line: everything that can fail, first */
    if ( !lines_reserve( doc, doc->count + breaks ) ) {
        return 0;
    }
    line = &doc->lines[at->line];
    fresh = (LINE *)try_alloc( breaks * sizeof( LINE ) );
    if ( fresh == NULL ) {
        return 0;
    }
    mem_set( fresh, 0, breaks * sizeof( LINE ) );
    tail_len = line->len - at->col;
    pos = 0;
    while ( pos < len && text[pos] != '\n' ) {
        pos++;
    }
    if ( at->col + pos > MAX_LINE || !line_reserve( line, at->col + pos ) ) {
        xfree( fresh );
        return 0;
    }
    index = 0;
    seg = ++pos;                                /* past the first break */
    for ( ; pos <= len; pos++ ) {
        if ( pos == len || text[pos] == '\n' ) {
            u32 want = pos - seg + (index == breaks - 1 ? tail_len : 0);
            if ( want > MAX_LINE || !line_reserve( &fresh[index], want ? want : 1 ) ) {
                while ( index + 1 > 0 ) {
                    xfree( fresh[index].text );
                    if ( index == 0 ) {
                        break;
                    }
                    index--;
                }
                xfree( fresh );
                return 0;
            }
            mem_cpy( fresh[index].text, text + seg, pos - seg );
            fresh[index].len = pos - seg;
            index++;
            seg = pos + 1;
        }
    }

    /* and now nothing can: split the line and put the new ones in */
    tail = line->text + at->col;
    mem_cpy( fresh[breaks - 1].text + fresh[breaks - 1].len, tail, tail_len );
    fresh[breaks - 1].len += tail_len;
    seg = 0;
    while ( seg < len && text[seg] != '\n' ) {
        seg++;
    }
    mem_cpy( line->text + at->col, text, seg );
    line->len = at->col + seg;
    mem_cpy( doc->lines + at->line + 1 + breaks, doc->lines + at->line + 1,
             (doc->count - at->line - 1) * sizeof( LINE ) );
    mem_cpy( doc->lines + at->line + 1, fresh, breaks * sizeof( LINE ) );
    doc->count += breaks;
    xfree( fresh );
    at->line += breaks;
    at->col = doc->lines[at->line].len - tail_len;
    doc->modified = 1;
    return 1;
}

/* the text from "from" up to (not including) "to" */
int doc_delete( DOC *doc, POS from, POS to )
{
    LINE *first, *last;
    u32 index, keep;

    if ( to.line < from.line || (to.line == from.line && to.col <= from.col) ) {
        return 1;
    }
    first = &doc->lines[from.line];
    if ( from.col > first->len ) {              /* past the end: pad it out */
        if ( !line_reserve( first, from.col ) ) {
            return 0;
        }
        mem_set( first->text + first->len, ' ', from.col - first->len );
        first->len = from.col;
    }
    last = &doc->lines[to.line];
    if ( to.col > last->len ) {
        to.col = last->len;
    }
    if ( from.line == to.line ) {
        if ( to.col > from.col ) {
            mem_cpy( first->text + from.col, first->text + to.col, first->len - to.col );
            first->len -= to.col - from.col;
        }
        doc->modified = 1;
        return 1;
    }
    keep = last->len - to.col;
    if ( !line_reserve( first, from.col + keep ) ) {
        return 0;
    }
    last = &doc->lines[to.line];
    mem_cpy( first->text + from.col, last->text + to.col, keep );
    first->len = from.col + keep;
    for ( index = from.line + 1; index <= to.line; index++ ) {
        line_release( &doc->lines[index] );          /* pooled or its own */
    }
    mem_cpy( doc->lines + from.line + 1, doc->lines + to.line + 1,
             (doc->count - to.line - 1) * sizeof( LINE ) );
    doc->count -= to.line - from.line;
    mem_set( doc->lines + doc->count, 0, (to.line - from.line) * sizeof( LINE ) );
    doc->modified = 1;
    return 1;
}

char *doc_extract( DOC *doc, POS from, POS to, u32 *len )
{
    u32 total = 0, index, start, end;
    LINE *line;
    char *text;

    for ( index = from.line; index <= to.line; index++ ) {
        line = &doc->lines[index];
        start = index == from.line ? from.col : 0;
        end = index == to.line ? to.col : line->len;
        if ( end > line->len ) {
            end = line->len;
        }
        if ( end > start ) {
            total += end - start;
        }
        if ( index < to.line ) {
            total++;
        }
    }
    text = (char *)try_alloc( total + 1 );
    if ( text == NULL ) {
        return NULL;
    }
    total = 0;
    for ( index = from.line; index <= to.line; index++ ) {
        line = &doc->lines[index];
        start = index == from.line ? from.col : 0;
        end = index == to.line ? to.col : line->len;
        if ( end > line->len ) {
            end = line->len;
        }
        if ( end > start ) {
            mem_cpy( text + total, line->text + start, end - start );
            total += end - start;
        }
        if ( index < to.line ) {
            text[total++] = '\n';
        }
    }
    text[total] = 0;
    *len = total;
    return text;
}
