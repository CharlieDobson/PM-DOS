/*
 * DOC.C - the lines of one text: the module, or one procedure.  EDIT's
 * document, with what QBasic needs: a line carries its breakpoint (so the
 * mark moves with the line as lines come and go above it), and whole
 * lines are set, put in and taken out by the loader and the editor.
 *
 * A line has a block of its own; there are no pools here, because a
 * program is read in through PROG.C, a line at a time, and is a few
 * thousand lines at most.
 */
#include "qb.h"


int tab_size = 8;

static int line_reserve( LINE *line, u32 need )
{
    u32 cap;
    char *grown;

    if ( need > 65535u ) {
        return 0;                               /* a LINE's length is 16 bits */
    }
    if ( need <= line->cap ) {
        return 1;
    }
    cap = line->cap ? line->cap * 2u : (need < 16 ? 16 : need + need / 4);
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
    xfree( line->text );
    line->text = grown;
    line->cap = (u16)cap;
    return 1;
}

static void line_release( LINE *line )
{
    xfree( line->text );
    line->text = NULL;
    line->len = line->cap = 0;
    line->flags = 0;
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
    u32 index;

    if ( doc == NULL ) {
        return;
    }
    for ( index = 0; index < doc->count; index++ ) {
        line_release( &doc->lines[index] );
    }
    xfree( doc->lines );
    xfree( doc );
}

LINE *doc_line( DOC *doc, u32 line )
{
    return line < doc->count ? &doc->lines[line] : NULL;
}

/* a line's text replaced; its breakpoint stays */
int doc_set_line( DOC *doc, u32 line, const char *text, u32 len )
{
    LINE *ln = &doc->lines[line];

    if ( len > MAX_LINE ) {
        len = MAX_LINE;
    }
    if ( !line_reserve( ln, len ? len : 1 ) ) {
        return 0;
    }
    mem_cpy( ln->text, text, len );
    ln->len = (u16)len;
    doc->modified = 1;
    return 1;
}

/* a whole line put in before "at" */
int doc_insert_line( DOC *doc, u32 at, const char *text, u32 len )
{
    LINE fresh;

    mem_set( &fresh, 0, sizeof( fresh ) );
    if ( len > MAX_LINE ) {
        len = MAX_LINE;
    }
    if ( !lines_reserve( doc, doc->count + 1 ) || !line_reserve( &fresh, len ? len : 1 ) ) {
        xfree( fresh.text );
        return 0;
    }
    mem_cpy( fresh.text, text, len );
    fresh.len = (u16)len;
    mem_cpy( doc->lines + at + 1, doc->lines + at, (doc->count - at) * sizeof( LINE ) );
    doc->lines[at] = fresh;
    doc->count++;
    doc->modified = 1;
    return 1;
}

/* a line at the end; a new document's first, empty line is used first */
int doc_append( DOC *doc, const char *text, u32 len )
{
    return doc_insert_line( doc, doc->count, text, len );
}

int doc_delete_lines( DOC *doc, u32 at, u32 count )
{
    u32 index;

    if ( at + count > doc->count ) {
        count = doc->count - at;
    }
    for ( index = at; index < at + count; index++ ) {
        line_release( &doc->lines[index] );
    }
    mem_cpy( doc->lines + at, doc->lines + at + count, (doc->count - at - count) * sizeof( LINE ) );
    doc->count -= count;
    mem_set( doc->lines + doc->count, 0, count * sizeof( LINE ) );
    if ( doc->count == 0 ) {
        doc->count = 1;                         /* never no lines at all */
    }
    doc->modified = 1;
    return 1;
}

u32 doc_col_of( DOC *doc, LINE *line, u32 index )
{
    u32 pos, col = 0;

    (void)doc;
    for ( pos = 0; pos < index && pos < line->len; pos++ ) {
        if ( line->text[pos] == '\t' ) {
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

u32 doc_index_of( DOC *doc, LINE *line, u32 col )
{
    u32 pos, at = 0, width;

    (void)doc;
    for ( pos = 0; pos < line->len; pos++ ) {
        width = line->text[pos] == '\t' ? (at / tab_size + 1) * tab_size - at : 1;
        if ( at + width > col ) {
            return pos;
        }
        at += width;
    }
    return line->len + (col - at);
}

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
    if ( at->col > line->len ) {
        if ( !line_reserve( line, at->col ) ) {
            return 0;
        }
        mem_set( line->text + line->len, ' ', at->col - line->len );
        line->len = (u16)at->col;
    }
    if ( breaks == 0 ) {
        if ( line->len + len > MAX_LINE || !line_reserve( line, line->len + len ) ) {
            return 0;
        }
        mem_cpy( line->text + at->col + len, line->text + at->col, line->len - at->col );
        mem_cpy( line->text + at->col, text, len );
        line->len = (u16)(line->len + len);
        at->col += len;
        doc->modified = 1;
        return 1;
    }
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
    seg = ++pos;
    for ( ; pos <= len; pos++ ) {
        if ( pos == len || text[pos] == '\n' ) {
            u32 want = pos - seg + (index == breaks - 1 ? tail_len : 0);
            if ( want > MAX_LINE || !line_reserve( &fresh[index], want ? want : 1 ) ) {
                for ( ;; ) {
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
            fresh[index].len = (u16)(pos - seg);
            index++;
            seg = pos + 1;
        }
    }
    tail = line->text + at->col;
    mem_cpy( fresh[breaks - 1].text + fresh[breaks - 1].len, tail, tail_len );
    fresh[breaks - 1].len = (u16)(fresh[breaks - 1].len + tail_len);
    seg = 0;
    while ( seg < len && text[seg] != '\n' ) {
        seg++;
    }
    mem_cpy( line->text + at->col, text, seg );
    line->len = (u16)(at->col + seg);
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

/* the length of the line a delete from "from" to "to" leaves, when it
   joins two lines (MAX_LINE is the most a line may be) */
u32 doc_joined_len( DOC *doc, POS from, POS to )
{
    LINE *last = &doc->lines[to.line];

    return from.col + (to.col < last->len ? last->len - to.col : 0);
}

int doc_delete( DOC *doc, POS from, POS to )
{
    LINE *first, *last;
    u32 index, keep;

    if ( to.line < from.line || (to.line == from.line && to.col <= from.col) ) {
        return 1;
    }
    if ( from.col > MAX_LINE || (to.line > from.line && doc_joined_len( doc, from, to ) > MAX_LINE) ) {
        return 0;
    }
    first = &doc->lines[from.line];
    if ( from.col > first->len ) {
        if ( !line_reserve( first, from.col ) ) {
            return 0;
        }
        mem_set( first->text + first->len, ' ', from.col - first->len );
        first->len = (u16)from.col;
    }
    last = &doc->lines[to.line];
    if ( to.col > last->len ) {
        to.col = last->len;
    }
    if ( from.line == to.line ) {
        if ( to.col > from.col ) {
            mem_cpy( first->text + from.col, first->text + to.col, first->len - to.col );
            first->len = (u16)(first->len - (to.col - from.col));
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
    first->len = (u16)(from.col + keep);
    for ( index = from.line + 1; index <= to.line; index++ ) {
        line_release( &doc->lines[index] );
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
