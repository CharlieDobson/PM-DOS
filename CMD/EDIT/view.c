/*
 * VIEW.C - a window onto a document: drawing it, the cursor, selection
 * and every key that moves or edits.
 *
 * THE CURSOR IS A SCREEN COLUMN, not a byte, and it may stand past the
 * end of the line: DOS's editor lets it, and typing there fills the gap
 * with spaces.  Inside the text it only ever stands at the start of a
 * character - a tab is crossed in one step - so turning it into a byte
 * (doc_index_of) is exact wherever something can be typed.  "want" is
 * the column Up and Down try to get back to across shorter lines.
 *
 * A SELECTION is the stretch between an anchor and the cursor, a
 * character at a time, lines included.  Shift with any movement key
 * makes one or stretches it; any other movement drops it; typing or
 * pasting replaces it.
 *
 * The WordStar keys are here too, two-key ones and all (^Q and ^K and
 * a second key), because DOS's editor has them.
 */
#include "edit.h"

int insert_mode = 1;

static int prefix;              /* ^Q or ^K waiting for its second key */
static int literal;             /* ^P: the next key goes in as it is */
static struct {
    DOC *doc;
    u32  line, col;
} marks[4];

int view_text_rows( VIEW *view )
{
    return view->height > 2 ? view->height - 2 : 1;
}

int view_text_cols( void )
{
    return scr_cols - 2;
}

static LINE *cur_line( VIEW *view )
{
    return doc_line( view->doc, view->line );
}

static u32 line_end( VIEW *view, u32 line )
{
    LINE *text = doc_line( view->doc, line );

    return doc_col_of( view->doc, text, text->len );
}

POS view_pos( VIEW *view )
{
    POS pos;

    pos.line = view->line;
    pos.col = doc_index_of( view->doc, cur_line( view ), view->col );
    return pos;
}

/* the column a character at-or-before "col" starts at */
static u32 snap( VIEW *view, u32 line, u32 col )
{
    LINE *text = doc_line( view->doc, line );
    u32 index = doc_index_of( view->doc, text, col );

    return index < text->len ? doc_col_of( view->doc, text, index ) : col;
}

void view_keep_visible( VIEW *view )
{
    int rows = view_text_rows( view ), cols = view_text_cols();

    if ( view->line < view->top ) {
        view->top = view->line;
    } else if ( view->line >= view->top + (u32)rows ) {
        view->top = view->line - (u32)rows + 1;
    }
    if ( view->col < view->left ) {
        view->left = view->col;
    } else if ( view->col >= view->left + (u32)cols ) {
        view->left = view->col - (u32)cols + 1;
    }
}

void view_fixup( DOC *doc )
{
    int index;
    VIEW *view;

    for ( index = 0; index < nviews; index++ ) {
        view = &views[index];
        if ( view->doc != doc ) {
            continue;
        }
        if ( view->line >= doc->count ) {
            view->line = doc->count - 1;
            view->col = snap( view, view->line, view->col );
        }
        if ( view->sel_line >= doc->count ) {
            view->sel_line = doc->count - 1;
        }
        if ( view->top >= doc->count ) {
            view->top = doc->count - 1;
        }
        view_keep_visible( view );
    }
}

/* ------------------------------------------------------------------ */
/* selection                                                           */
/* ------------------------------------------------------------------ */

int view_has_sel( VIEW *view )
{
    return view->sel && (view->sel_line != view->line || view->sel_col != view->col);
}

void view_sel_clear( VIEW *view )
{
    view->sel = 0;
}

static POS clamp_pos( VIEW *view, u32 line, u32 col )
{
    LINE *text = doc_line( view->doc, line );
    POS pos;

    pos.line = line;
    pos.col = doc_index_of( view->doc, text, col );
    if ( pos.col > text->len ) {
        pos.col = text->len;
    }
    return pos;
}

void view_sel_range( VIEW *view, POS *from, POS *to )
{
    POS anchor = clamp_pos( view, view->sel_line, view->sel_col );
    POS here = clamp_pos( view, view->line, view->col );

    if ( anchor.line < here.line || (anchor.line == here.line && anchor.col <= here.col) ) {
        *from = anchor;
        *to = here;
    } else {
        *from = here;
        *to = anchor;
    }
}

void view_goto( VIEW *view, u32 line, u32 col, int select )
{
    if ( select ) {
        if ( !view->sel ) {
            view->sel = 1;
            view->sel_line = view->line;
            view->sel_col = view->col;
        }
    } else {
        view->sel = 0;
    }
    if ( line >= view->doc->count ) {
        line = view->doc->count - 1;
    }
    if ( col > MAX_COL ) {
        col = MAX_COL;
    }
    view->line = line;
    view->col = col;
    view_keep_visible( view );
}

void view_delete_sel( VIEW *view )
{
    POS from, to;

    if ( !view_has_sel( view ) ) {
        view->sel = 0;
        return;
    }
    view_sel_range( view, &from, &to );
    if ( doc_delete( view->doc, from, to ) ) {
        view->sel = 0;
        view->line = from.line;
        view->col = doc_col_of( view->doc, doc_line( view->doc, from.line ), from.col );
        view->want = view->col;
        view_fixup( view->doc );
        view_keep_visible( view );
    } else {
        out_of_memory();
    }
}

/* text at the cursor, replacing a selection; 0 when there was no room */
int view_insert_text( VIEW *view, const char *text, u32 len )
{
    POS pos;

    if ( view->doc->readonly ) {
        return 0;
    }
    view_delete_sel( view );
    pos = view_pos( view );
    if ( !doc_insert( view->doc, &pos, text, len ) ) {
        out_of_memory();
        return 0;
    }
    view->line = pos.line;
    view->col = doc_col_of( view->doc, doc_line( view->doc, pos.line ), pos.col );
    view->want = view->col;
    view_fixup( view->doc );
    view_keep_visible( view );
    return 1;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_title( VIEW *view, int is_active )
{
    DOC *doc = view->doc;
    char title[80];
    int len, at;

    scr_fill( view->row, 0, scr_cols, 0xC4, pal.frame );
    scr_ch( view->row, 0, 0xDA, pal.frame );
    scr_ch( view->row, scr_cols - 1, 0xBF, pal.frame );
    title[0] = ' ';
    str_cpyn( title + 1, doc->name, sizeof( title ) - 3 );
    str_catn( title, " ", sizeof( title ) );
    len = (int)str_len( title );
    if ( len > scr_cols - 4 ) {
        len = scr_cols - 4;
    }
    at = (scr_cols - len) / 2;
    scr_putn( view->row, at, title, len, is_active ? pal.title : pal.frame );
}

static void draw_line( VIEW *view, int row, u32 line, POS *from, POS *to, int has_sel )
{
    int cols = view_text_cols(), screen;
    u32 col = 0, index = 0, sel_end;
    LINE *text;
    u8 attr;
    int ch, width, tab;

    scr_fill( row, 1, cols, ' ', pal.text );
    if ( line >= view->doc->count ) {
        return;
    }
    text = doc_line( view->doc, line );
    while ( index < text->len && col < view->left + (u32)cols ) {
        ch = (u8)text->text[index];
        tab = ch == '\t' && !view->doc->binary;    /* binary: its glyph, one wide */
        width = tab ? (int)((col / tab_size + 1) * tab_size - col) : 1;
        attr = pal.text;
        if ( has_sel && line >= from->line && line <= to->line
             && (line > from->line || index >= from->col)
             && (line < to->line || index < to->col) ) {
            attr = pal.select;
        }
        while ( width-- > 0 ) {
            if ( col >= view->left ) {
                screen = (int)(col - view->left);
                if ( screen < cols ) {
                    scr_ch( row, 1 + screen, tab ? ' ' : ch, attr );
                }
            }
            col++;
        }
        index++;
    }
    /* a selection that runs on to the next line takes the line's end too */
    if ( has_sel && line >= from->line && line < to->line && (line > from->line || from->col <= text->len) ) {
        sel_end = doc_col_of( view->doc, text, text->len );
        if ( sel_end >= view->left && sel_end < view->left + (u32)cols ) {
            scr_recolor( row, 1 + (int)(sel_end - view->left), 1, pal.select );
        }
    }
}

/* where the scroll boxes are: the vertical one as a row of its bar (1 is
   the first below the arrow), the horizontal one as a screen column */
static int vthumb( VIEW *view )
{
    int rows = view_text_rows( view );
    u32 count = view->doc->count;

    return 1 + (count > 1 ? (int)((u32)view->line * (u32)(rows - 3) / (count - 1)) : 0);
}

static int hthumb( VIEW *view )
{
    int width = scr_cols - 2;

    return 2 + (int)((view->col > 255 ? 255 : view->col) * (u32)(width - 3) / 255);
}

static void draw_vscroll( VIEW *view )
{
    int rows = view_text_rows( view ), row, thumb;
    int col = scr_cols - 1, first = view->row + 1;

    for ( row = 0; row < rows; row++ ) {
        scr_ch( first + row, col, 0xB0, pal.scroll );
    }
    if ( rows >= 2 ) {
        scr_ch( first, col, 0x18, pal.scroll );
        scr_ch( first + rows - 1, col, 0x19, pal.scroll );
    }
    if ( rows >= 3 ) {
        thumb = vthumb( view );
        scr_ch( first + thumb, col, ' ', pal.thumb );
    }
}

static void draw_hscroll( VIEW *view )
{
    int row = view->row + view->height - 1, width = scr_cols - 2, thumb;

    scr_ch( row, 0, 0xB3, pal.frame );
    scr_fill( row, 1, width, 0xB0, pal.scroll );
    scr_ch( row, 1, 0x1B, pal.scroll );
    scr_ch( row, width, 0x1A, pal.scroll );
    if ( width >= 3 ) {
        thumb = hthumb( view );
        scr_ch( row, thumb, ' ', pal.thumb );
    }
    scr_ch( row, scr_cols - 1, ' ', pal.scroll );
}

void view_draw( VIEW *view, int is_active )
{
    int rows = view_text_rows( view ), row;
    POS from, to;
    int has_sel = view_has_sel( view );

    if ( has_sel ) {
        view_sel_range( view, &from, &to );
    }
    draw_title( view, is_active );
    for ( row = 0; row < rows; row++ ) {
        scr_ch( view->row + 1 + row, 0, 0xB3, pal.frame );
        draw_line( view, view->row + 1 + row, view->top + (u32)row, &from, &to, has_sel );
    }
    draw_vscroll( view );
    if ( view->height >= 3 ) {
        draw_hscroll( view );
    }
}

void view_place_cursor( VIEW *view )
{
    scr_cursor( view->row + 1 + (int)(view->line - view->top),
                1 + (int)(view->col - view->left), insert_mode ? CUR_LINE : CUR_BLOCK );
}

/* ------------------------------------------------------------------ */
/* moving                                                              */
/* ------------------------------------------------------------------ */

static int is_word( int ch )
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
           || ch == '_' || ch >= 0x80;
}

static void word_right( VIEW *view, u32 *line, u32 *col )
{
    LINE *text = doc_line( view->doc, *line );
    u32 index = doc_index_of( view->doc, text, *col );

    while ( index < text->len && is_word( (u8)text->text[index] ) ) {
        index++;
    }
    while ( index < text->len && !is_word( (u8)text->text[index] ) ) {
        index++;
    }
    if ( index >= text->len && *line + 1 < view->doc->count ) {
        (*line)++;
        text = doc_line( view->doc, *line );
        index = 0;
        while ( index < text->len && !is_word( (u8)text->text[index] ) ) {
            index++;
        }
    }
    *col = doc_col_of( view->doc, text, index );
}

static void word_left( VIEW *view, u32 *line, u32 *col )
{
    LINE *text = doc_line( view->doc, *line );
    u32 index = doc_index_of( view->doc, text, *col );

    if ( index > text->len ) {
        index = text->len;
    }
    while ( index > 0 && !is_word( (u8)text->text[index - 1] ) ) {
        index--;
    }
    if ( index == 0 ) {
        if ( *line == 0 ) {
            *col = 0;
            return;
        }
        (*line)--;
        text = doc_line( view->doc, *line );
        index = text->len;
        while ( index > 0 && !is_word( (u8)text->text[index - 1] ) ) {
            index--;
        }
    }
    while ( index > 0 && is_word( (u8)text->text[index - 1] ) ) {
        index--;
    }
    *col = doc_col_of( view->doc, text, index );
}

/* 1 if it was a movement key, and then it has been done */
static int move_key( VIEW *view, int key )
{
    int select = (key & K_SHIFT) != 0;
    int rows = view_text_rows( view ), cols = view_text_cols();
    u32 line = view->line, col = view->col, keep_want = 1, end;
    LINE *text;
    u32 index;

    switch ( key & ~K_SHIFT ) {
    case K_LEFT:
    case CTRL( 'S' ):
        text = cur_line( view );
        end = doc_col_of( view->doc, text, text->len );
        if ( col > end ) {
            col--;
        } else if ( col > 0 ) {
            index = doc_index_of( view->doc, text, col );
            col = doc_col_of( view->doc, text, index ? index - 1 : 0 );
        }
        keep_want = 0;
        break;
    case K_RIGHT:
    case CTRL( 'D' ):
        text = cur_line( view );
        index = doc_index_of( view->doc, text, col );
        col = index < text->len ? doc_col_of( view->doc, text, index + 1 ) : col + 1;
        keep_want = 0;
        break;
    case K_UP:
    case CTRL( 'E' ):
        if ( line > 0 ) {
            line--;
        }
        col = snap( view, line, view->want );
        break;
    case K_DOWN:
    case CTRL( 'X' ):
        if ( line + 1 < view->doc->count ) {
            line++;
        }
        col = snap( view, line, view->want );
        break;
    case K_HOME:
        col = 0;
        keep_want = 0;
        break;
    case K_END:
        col = line_end( view, line );
        keep_want = 0;
        break;
    case K_PGUP:
    case CTRL( 'R' ):
        line = line > (u32)(rows - 1) ? line - (u32)(rows - 1) : 0;
        view->top = view->top > (u32)(rows - 1) ? view->top - (u32)(rows - 1) : 0;
        col = snap( view, line, view->want );
        break;
    case K_PGDN:
    case CTRL( 'C' ):
        line += (u32)(rows - 1);
        if ( line >= view->doc->count ) {
            line = view->doc->count - 1;
        }
        if ( view->top + (u32)(rows - 1) < view->doc->count ) {
            view->top += (u32)(rows - 1);
        }
        col = snap( view, line, view->want );
        break;
    case K_CHOME:
        line = 0;
        col = 0;
        keep_want = 0;
        break;
    case K_CEND:
        line = view->doc->count - 1;
        col = line_end( view, line );
        keep_want = 0;
        break;
    case K_CLEFT:
    case CTRL( 'A' ):
        word_left( view, &line, &col );
        keep_want = 0;
        break;
    case K_CRIGHT:
    case CTRL( 'F' ):
        word_right( view, &line, &col );
        keep_want = 0;
        break;
    case K_CUP:
    case CTRL( 'W' ):
        if ( view->top > 0 ) {
            view->top--;
        }
        if ( line >= view->top + (u32)rows ) {
            line = view->top + (u32)rows - 1;
            col = snap( view, line, view->want );
        }
        break;
    case K_CDOWN:
    case CTRL( 'Z' ):
        if ( view->top + 1 < view->doc->count ) {
            view->top++;
        }
        if ( line < view->top ) {
            line = view->top;
            col = snap( view, line, view->want );
        }
        break;
    case K_CPGUP:
        view->left = view->left > (u32)cols ? view->left - (u32)cols : 0;
        col = col > (u32)cols ? col - (u32)cols : 0;
        keep_want = 0;
        break;
    case K_CPGDN:
        view->left += (u32)cols;
        col += (u32)cols;
        keep_want = 0;
        break;
    default:
        return 0;
    }
    view_goto( view, line, col, select );
    if ( !keep_want ) {
        view->want = view->col;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* editing                                                             */
/* ------------------------------------------------------------------ */

static void type_char( VIEW *view, int ch )
{
    LINE *text;
    POS pos;
    char byte = (char)ch;

    if ( view->doc->readonly ) {
        return;
    }
    view_delete_sel( view );
    text = cur_line( view );
    pos = view_pos( view );
    if ( !insert_mode && pos.col < text->len ) {
        text->text[pos.col] = byte;
        view->doc->modified = 1;
        view->col = doc_col_of( view->doc, text, pos.col + 1 );
    } else {
        if ( view->col >= MAX_COL || !doc_insert( view->doc, &pos, &byte, 1 ) ) {
            return;
        }
        view->col = doc_col_of( view->doc, cur_line( view ), pos.col );
    }
    view->want = view->col;
    view_fixup( view->doc );
    view_keep_visible( view );
}

/* Enter: the rest of the line down one, indented as this one is */
static void new_line( VIEW *view )
{
    LINE *text;
    POS pos;
    char buf[81];
    u32 indent = 0, tail;

    if ( view->doc->readonly ) {
        return;
    }
    view_delete_sel( view );
    text = cur_line( view );
    pos = view_pos( view );
    while ( indent < text->len && indent < pos.col && indent < 79
            && (text->text[indent] == ' ' || text->text[indent] == '\t') ) {
        indent++;
    }
    tail = pos.col < text->len ? text->len - pos.col : 0;
    buf[0] = '\n';
    mem_cpy( buf + 1, text->text, indent );
    if ( tail == 0 ) {
        /* nothing moves down: the indent is only where the cursor goes */
        if ( !doc_insert( view->doc, &pos, buf, 1 ) ) {
            out_of_memory();
            return;
        }
        view->line = pos.line;
        view->col = doc_col_of( view->doc, doc_line( view->doc, pos.line - 1 ), indent );
    } else {
        if ( !doc_insert( view->doc, &pos, buf, indent + 1 ) ) {
            out_of_memory();
            return;
        }
        view->line = pos.line;
        view->col = doc_col_of( view->doc, cur_line( view ), pos.col );
    }
    view->want = view->col;
    view_fixup( view->doc );
    view_keep_visible( view );
}

static void back_space( VIEW *view )
{
    LINE *text;
    POS from, to;
    u32 end;

    if ( view->doc->readonly ) {
        return;
    }
    if ( view_has_sel( view ) ) {
        view_delete_sel( view );
        return;
    }
    view->sel = 0;
    text = cur_line( view );
    end = doc_col_of( view->doc, text, text->len );
    if ( view->col == 0 ) {
        if ( view->line == 0 ) {
            return;
        }
        from.line = view->line - 1;
        from.col = doc_line( view->doc, from.line )->len;
        to.line = view->line;
        to.col = 0;
        if ( !doc_delete( view->doc, from, to ) ) {
            out_of_memory();
            return;
        }
        view->line = from.line;
        view->col = doc_col_of( view->doc, doc_line( view->doc, from.line ), from.col );
    } else if ( view->col > end ) {
        view->col--;
    } else {
        to.line = view->line;
        to.col = doc_index_of( view->doc, text, view->col );
        from.line = view->line;
        from.col = to.col - 1;
        doc_delete( view->doc, from, to );
        view->col = doc_col_of( view->doc, cur_line( view ), from.col );
    }
    view->want = view->col;
    view_fixup( view->doc );
    view_keep_visible( view );
}

static void delete_char( VIEW *view )
{
    LINE *text;
    POS from, to;

    if ( view->doc->readonly ) {
        return;
    }
    if ( view_has_sel( view ) ) {
        view_delete_sel( view );
        return;
    }
    view->sel = 0;
    text = cur_line( view );
    from = view_pos( view );
    if ( from.col < text->len ) {
        to = from;
        to.col++;
    } else {
        if ( view->line + 1 >= view->doc->count ) {
            return;
        }
        to.line = view->line + 1;
        to.col = 0;
    }
    if ( !doc_delete( view->doc, from, to ) ) {
        out_of_memory();
        return;
    }
    view_fixup( view->doc );
}

/* ^T: from the cursor to the start of the next word */
static void delete_word( VIEW *view )
{
    POS from = view_pos( view ), to;
    u32 line = view->line, col = view->col;

    if ( view->doc->readonly ) {
        return;
    }
    word_right( view, &line, &col );
    to = clamp_pos( view, line, col );
    if ( to.line > from.line ) {
        to.line = from.line;
        to.col = cur_line( view )->len;
        if ( to.col <= from.col ) {
            delete_char( view );
            return;
        }
    }
    doc_delete( view->doc, from, to );
    view_fixup( view->doc );
}

/* ^Y the line, ^QY the rest of it - both onto the clipboard */
static void cut_line( VIEW *view, int rest_only )
{
    POS from, to;
    char *text;
    u32 len;

    if ( view->doc->readonly ) {
        return;
    }
    view->sel = 0;
    from.line = view->line;
    from.col = rest_only ? view_pos( view ).col : 0;
    if ( from.col > cur_line( view )->len ) {
        return;
    }
    if ( !rest_only && view->line + 1 < view->doc->count ) {
        to.line = view->line + 1;
        to.col = 0;
    } else {
        to.line = view->line;
        to.col = cur_line( view )->len;
    }
    text = doc_extract( view->doc, from, to, &len );
    if ( text == NULL ) {
        out_of_memory();
        return;
    }
    clip_set( text, len );
    doc_delete( view->doc, from, to );
    if ( !rest_only ) {
        view->col = 0;
    }
    view->want = view->col;
    view_fixup( view->doc );
    view_keep_visible( view );
}

/* Tab and Shift+Tab over a selection of whole lines: indent, outdent */
static int shift_block( VIEW *view, int out )
{
    POS from, to, at;
    u32 line, last, remove;
    LINE *text;
    char spaces[32];

    if ( !view_has_sel( view ) ) {
        return 0;
    }
    view_sel_range( view, &from, &to );
    if ( from.line == to.line ) {
        return 0;
    }
    if ( view->doc->readonly ) {
        return 1;
    }
    last = to.col == 0 ? to.line - 1 : to.line;
    mem_set( spaces, ' ', sizeof( spaces ) );
    for ( line = from.line; line <= last; line++ ) {
        text = doc_line( view->doc, line );
        at.line = line;
        at.col = 0;
        if ( out ) {
            remove = 0;
            if ( text->len && text->text[0] == '\t' ) {
                remove = 1;
            } else {
                while ( remove < text->len && remove < (u32)tab_size && text->text[remove] == ' ' ) {
                    remove++;
                }
            }
            if ( remove ) {
                POS end = at;
                end.col = remove;
                doc_delete( view->doc, at, end );
            }
        } else if ( text->len ) {
            doc_insert( view->doc, &at, spaces, (u32)(tab_size < 32 ? tab_size : 32) );
        }
    }
    /* keep the lines selected, whole */
    view->sel = 1;
    view->sel_line = from.line;
    view->sel_col = 0;
    view->line = last + 1 < view->doc->count ? last + 1 : last;
    view->col = last + 1 < view->doc->count ? 0 : line_end( view, last );
    view_fixup( view->doc );
    view_keep_visible( view );
    return 1;
}

static void tab_key( VIEW *view, int back )
{
    u32 col = view->col, stop;
    char spaces[32];

    if ( shift_block( view, back ) ) {
        return;
    }
    if ( back ) {
        stop = col ? (col - 1) / tab_size * tab_size : 0;
        view_goto( view, view->line, stop, 0 );
        view->want = view->col;
        return;
    }
    stop = (col / tab_size + 1) * tab_size;
    if ( !insert_mode || col >= line_end( view, view->line ) || view->doc->readonly ) {
        view_goto( view, view->line, stop, 0 );
        view->want = view->col;
        return;
    }
    mem_set( spaces, ' ', sizeof( spaces ) );
    view_insert_text( view, spaces, stop - col < 32 ? stop - col : 32 );
}

/* ^K n sets bookmark n, ^Q n goes to it */
static void bookmark( VIEW *view, int number, int set )
{
    if ( set ) {
        marks[number].doc = view->doc;
        marks[number].line = view->line;
        marks[number].col = view->col;
    } else if ( marks[number].doc == view->doc ) {
        view_goto( view, marks[number].line, marks[number].col, 0 );
        view->want = view->col;
    }
}

static int prefixed( VIEW *view, int first, int key )
{
    int letter = ch_upper( key & 0xFF );

    if ( key >= 0x100 ) {
        letter = 0;
    } else if ( key < ' ' ) {
        letter = key + '@';                     /* ^Q ^S reads as ^Q S */
    }
    if ( letter >= '0' && letter <= '3' ) {
        bookmark( view, letter - '0', first == CTRL( 'K' ) );
        return 1;
    }
    if ( first == CTRL( 'K' ) ) {
        return 1;                               /* nothing else after ^K */
    }
    switch ( letter ) {
    case 'S': move_key( view, K_HOME );  break;
    case 'D': move_key( view, K_END );   break;
    case 'R': move_key( view, K_CHOME ); break;
    case 'C': move_key( view, K_CEND );  break;
    case 'E':
        view_goto( view, view->top, snap( view, view->top, view->want ), 0 );
        break;
    case 'X':
        view_goto( view, view->top + (u32)view_text_rows( view ) - 1, view->want, 0 );
        view->col = snap( view, view->line, view->want );
        break;
    case 'Y': cut_line( view, 1 ); break;
    case 'F': app_command( CMD_FIND );    break;
    case 'A': app_command( CMD_REPLACE ); break;
    }
    return 1;
}

/* 1 if the key was one of the window's own */
int view_key( VIEW *view, int key )
{
    int first;

    if ( literal ) {
        literal = 0;
        if ( key < 0x100 ) {
            type_char( view, key );
        }
        return 1;
    }
    if ( prefix ) {
        first = prefix;
        prefix = 0;
        app_status( NULL );
        return prefixed( view, first, key );
    }
    if ( move_key( view, key ) ) {
        return 1;
    }
    switch ( key ) {
    case K_ENTER:
        new_line( view );
        return 1;
    case K_BS:
        back_space( view );
        return 1;
    case K_DEL:
    case CTRL( 'G' ):
        delete_char( view );
        return 1;
    case K_TAB:
        tab_key( view, 0 );
        return 1;
    case K_SHTAB:
        tab_key( view, 1 );
        return 1;
    case K_INS:
    case CTRL( 'V' ):
        insert_mode = !insert_mode;
        return 1;
    case CTRL( 'Y' ):
        cut_line( view, 0 );
        return 1;
    case CTRL( 'T' ):
        delete_word( view );
        return 1;
    case CTRL( 'N' ):                          /* a line break after the cursor */
        if ( !view->doc->readonly ) {
            u32 line = view->line, col = view->col;
            view_insert_text( view, "\n", 1 );
            view_goto( view, line, col, 0 );
        }
        return 1;
    case CTRL( 'P' ):
        literal = 1;
        return 1;
    case CTRL( 'Q' ):
        prefix = CTRL( 'Q' );
        app_status( "^Q" );
        return 1;
    case CTRL( 'K' ):
        prefix = CTRL( 'K' );
        app_status( "^K" );
        return 1;
    case CTRL( 'L' ):
        app_command( CMD_REPEAT );
        return 1;
    }
    if ( key >= ' ' && key < 0x100 && key != 0x7F ) {
        type_char( view, key );
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

/* which part of the window a cell is */
int view_hit( VIEW *view, int row, int col )
{
    int rows = view_text_rows( view ), first = view->row + 1, width = scr_cols - 2, thumb;

    if ( row == view->row ) {
        return VH_TITLE;
    }
    if ( view->height >= 3 && row == view->row + view->height - 1 ) {
        if ( col == 1 ) {
            return VH_LEFT;
        }
        if ( col == width ) {
            return VH_RIGHT;
        }
        if ( col < 1 || col > width || width < 3 ) {
            return VH_NONE;
        }
        thumb = hthumb( view );
        return col < thumb ? VH_PGLEFT : col > thumb ? VH_PGRIGHT : VH_HTHUMB;
    }
    if ( row < first || row >= first + rows ) {
        return VH_NONE;
    }
    if ( col == scr_cols - 1 ) {
        if ( rows < 2 ) {
            return VH_NONE;
        }
        if ( row == first ) {
            return VH_UP;
        }
        if ( row == first + rows - 1 ) {
            return VH_DOWN;
        }
        if ( rows < 3 ) {
            return VH_NONE;
        }
        thumb = first + vthumb( view );
        return row < thumb ? VH_PGUP : row > thumb ? VH_PGDN : VH_VTHUMB;
    }
    if ( col >= 1 && col <= width ) {
        return VH_TEXT;
    }
    return VH_NONE;
}

/* the cursor to a cell of the text - onto the start of the character
   there, or past the end of the line, where a click is allowed to put it */
void view_mouse_to( VIEW *view, int row, int col, int select )
{
    int rows = view_text_rows( view ), cols = view_text_cols();
    int text_row = row - view->row - 1, text_col = col - 1;
    u32 line;

    if ( text_row < 0 ) {
        text_row = 0;
    }
    if ( text_row >= rows ) {
        text_row = rows - 1;
    }
    if ( text_col < 0 ) {
        text_col = 0;
    }
    if ( text_col >= cols ) {
        text_col = cols - 1;
    }
    line = view->top + (u32)text_row;
    if ( line >= view->doc->count ) {
        line = view->doc->count - 1;
    }
    view_goto( view, line, snap( view, line, view->left + (u32)text_col ), select );
    view->want = view->col;
}

/* a double click: the word the cursor is on, selected */
void view_select_word( VIEW *view )
{
    LINE *text = cur_line( view );
    u32 index = doc_index_of( view->doc, text, view->col ), start, end;

    if ( index >= text->len || !is_word( (u8)text->text[index] ) ) {
        return;
    }
    for ( start = index; start > 0 && is_word( (u8)text->text[start - 1] ); start-- ) {
    }
    for ( end = index; end < text->len && is_word( (u8)text->text[end] ); end++ ) {
    }
    view->sel = 1;
    view->sel_line = view->line;
    view->sel_col = doc_col_of( view->doc, text, start );
    view->col = doc_col_of( view->doc, text, end );
    view->want = view->col;
    view_keep_visible( view );
}

/* the scroll bars: a line, a page, or wherever the box was dragged to */
void view_scroll_part( VIEW *view, int part, int row, int col )
{
    int rows = view_text_rows( view ), width = scr_cols - 2, first = view->row + 1, at;
    u32 count = view->doc->count, line;

    switch ( part ) {
    case VH_UP:
        move_key( view, K_CUP );
        break;
    case VH_DOWN:
        move_key( view, K_CDOWN );
        break;
    case VH_PGUP:
        move_key( view, K_PGUP );
        break;
    case VH_PGDN:
        move_key( view, K_PGDN );
        break;
    case VH_VTHUMB:
        at = row - first - 1;
        if ( rows < 4 || count < 2 ) {
            break;
        }
        if ( at < 0 ) {
            at = 0;
        }
        if ( at > rows - 3 ) {
            at = rows - 3;
        }
        line = (u32)at * (count - 1) / (u32)(rows - 3);
        view_goto( view, line, snap( view, line, view->want ), 0 );
        break;
    case VH_LEFT:
        if ( view->left > 0 ) {
            view->left--;
        }
        if ( view->col >= view->left + (u32)view_text_cols() ) {
            view_goto( view, view->line, view->left + (u32)view_text_cols() - 1, 0 );
            view->want = view->col;
        }
        break;
    case VH_RIGHT:
        view->left++;
        if ( view->col < view->left ) {
            view_goto( view, view->line, view->left, 0 );
            view->want = view->col;
        }
        break;
    case VH_PGLEFT:
        move_key( view, K_CPGUP );
        break;
    case VH_PGRIGHT:
        move_key( view, K_CPGDN );
        break;
    case VH_HTHUMB:
        at = col - 2;
        if ( width < 4 ) {
            break;
        }
        if ( at < 0 ) {
            at = 0;
        }
        if ( at > width - 3 ) {
            at = width - 3;
        }
        view_goto( view, view->line, (u32)at * 255 / (u32)(width - 3), 0 );
        view->want = view->col;
        break;
    }
}
