/*
 * IDE_VIEW.C - a window onto one of the program's texts (the module or
 * a procedure), or onto the Immediate window's lines: drawing it, the
 * cursor, selection, every key that moves or edits, and the check a
 * line gets when the cursor leaves it.
 *
 * EDIT's VIEW.C underneath (the cursor is a screen column and may stand
 * past the end of a line; a selection runs from an anchor to the
 * cursor; the WordStar keys), drawn as QBasic draws its windows:
 *
 *   a title in the middle of the top border, lit when the window is the
 *   active one, and a box at the right that makes the window fill the
 *   screen; the scroll bars only on the active window, the vertical one
 *   in the right-hand border and the horizontal one along the bottom
 *   row; a breakpoint's line all in red, and the statement a stopped
 *   program will run next in bright white.
 *
 * LEAVING A LINE CHECKS IT.  When the cursor moves off a line that was
 * typed on, the line is compiled on its own against the rest of the
 * program (COMP.C's comp_check_line): if it is good it is put back in
 * QBasic's form - keywords in capitals, names spelled as they were last
 * typed everywhere they appear, the spaces where QBasic puts them - and
 * if not, the error is shown and the line is left as it was typed, not
 * asked about again until it is changed.  A SUB or FUNCTION line typed
 * in the module starts a procedure of that name, in a text of its own.
 */
#include "ide.h"

int insert_mode = 1;
int syntax_check = 1;
int scroll_bars = 1;

#define TEXT_COLS   78          /* columns 1 to 78 */
#define MAX_COL     255         /* the longest line QBasic takes */

static int prefix;              /* ^Q or ^K waiting for its second key */
static int literal;             /* ^P: the next key goes in as it is */

int view_text_rows( VIEW *view )
{
    int rows = view->height - 1 - (view->bars ? 1 : 0);

    return rows > 0 ? rows : 1;
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

static u32 snap( VIEW *view, u32 line, u32 col )
{
    LINE *text = doc_line( view->doc, line );
    u32 index = doc_index_of( view->doc, text, col );

    return index < text->len ? doc_col_of( view->doc, text, index ) : col;
}

/* the window follows the cursor: a line at a time up and down, and
   across in a jump that leaves the cursor well inside it */
void view_keep_visible( VIEW *view )
{
    int rows = view_text_rows( view );

    if ( view->line < view->top ) {
        view->top = view->line;
    } else if ( view->line >= view->top + (u32)rows ) {
        view->top = view->line - (u32)rows + 1;
    }
    if ( view->col < view->left ) {
        view->left = view->col > 58 ? view->col - 58 : 0;
    } else if ( view->col >= view->left + TEXT_COLS ) {
        view->left = view->col - 19;
    }
}

void view_fixup( DOC *doc )
{
    int index;
    VIEW *view;

    for ( index = 0; index < 3; index++ ) {
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

/* every text keeps an empty line at its end for the cursor to stand on */
static void keep_last_empty( DOC *doc )
{
    if ( doc->count == 0 || doc_line( doc, doc->count - 1 )->len != 0 ) {
        doc_append( doc, "", 0 );
    }
}

/* the window shows another text; where it was in this one is kept */
void view_show_text( VIEW *view, int text )
{
    DOC *doc = text >= 0 ? ptext.texts[text] : view->doc;

    if ( view->doc ) {
        view->doc->at_line = view->line;
        view->doc->at_col = view->col;
        view->doc->at_top = view->top;
        view->doc->at_left = view->left;
    }
    view->text = text;
    view->doc = doc;
    keep_last_empty( doc );
    view->line = doc->at_line < doc->count ? doc->at_line : doc->count - 1;
    view->col = doc->at_col;
    view->top = doc->at_top < doc->count ? doc->at_top : 0;
    view->left = doc->at_left;
    view->want = view->col;
    view->sel = 0;
    view->dirty = 0;
    view->err_line = -1;
    view_keep_visible( view );
}

/* ------------------------------------------------------------------ */
/* selection                                                           */
/* ------------------------------------------------------------------ */

int view_has_sel( VIEW *view )
{
    return view->sel && (view->sel_line != view->line || view->sel_col != view->col);
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

/* something in the text changed: the program will have to start again,
   and the line will be checked when the cursor leaves it */
static void changed( VIEW *view )
{
    view->dirty = 1;
    view->err_line = -1;
    if ( view->text >= 0 ) {
        view->doc->modified = 1;
        ptext.modified = 1;
        app_edited();
    }
}

/* a delete that joins lines: too long a line is refused */
static int join_fits( VIEW *view, POS from, POS to )
{
    if ( to.line > from.line && doc_joined_len( view->doc, from, to ) > MAX_LINE ) {
        msg_box( "Line too long", MB_OK, 1 );
        return 0;
    }
    return 1;
}

void view_delete_sel( VIEW *view )
{
    POS from, to;

    if ( !view_has_sel( view ) ) {
        view->sel = 0;
        return;
    }
    view_sel_range( view, &from, &to );
    if ( !join_fits( view, from, to ) ) {
        return;
    }
    if ( doc_delete( view->doc, from, to ) ) {
        view->sel = 0;
        view->line = from.line;
        view->col = doc_col_of( view->doc, doc_line( view->doc, from.line ), from.col );
        view->want = view->col;
        changed( view );
        keep_last_empty( view->doc );
        view_fixup( view->doc );
        view_keep_visible( view );
    } else {
        out_of_memory();
    }
}

int view_insert_text( VIEW *view, const char *text, u32 len )
{
    POS pos;

    view_delete_sel( view );
    pos = view_pos( view );
    if ( !doc_insert( view->doc, &pos, text, len ) ) {
        out_of_memory();
        return 0;
    }
    view->line = pos.line;
    view->col = doc_col_of( view->doc, doc_line( view->doc, pos.line ), pos.col );
    view->want = view->col;
    changed( view );
    keep_last_empty( view->doc );
    view_fixup( view->doc );
    view_keep_visible( view );
    return 1;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_title( VIEW *view, int is_active )
{
    char title[80];
    int len, at, top = view->row == 1 && !help_shown();
    u8 corner = pal.frame;

    scr_fill( view->row, 0, scr_cols, 0xC4, pal.frame );
    scr_ch( view->row, 0, top ? 0xDA : 0xC3, corner );
    scr_ch( view->row, scr_cols - 1, top ? 0xBF : 0xB4, corner );
    title[0] = ' ';
    if ( view->text < 0 ) {
        str_cpy( title + 1, "Immediate" );
    } else {
        app_title( view->text, title + 1, sizeof( title ) - 3 );
    }
    str_catn( title, " ", sizeof( title ) );
    len = (int)str_len( title );
    if ( len > scr_cols - 10 ) {
        len = scr_cols - 10;
    }
    at = 39 - len / 2;
    scr_putn( view->row, at, title, len, is_active ? pal.title : pal.frame );
    if ( view->text >= 0 ) {
        /* the box that fills the screen with this window, or puts it back */
        scr_ch( view->row, scr_cols - 5, 0xB4, pal.frame );
        scr_ch( view->row, scr_cols - 4, view->height >= scr_rows - 3 ? 0x12 : 0x18, pal.title );
        scr_ch( view->row, scr_cols - 3, 0xC3, pal.frame );
    }
}

static void draw_line( VIEW *view, int row, u32 line, POS *from, POS *to, int has_sel )
{
    u32 col = 0, index = 0, sel_end;
    LINE *text;
    u8 attr, base = pal.text;
    int ch, width, tab, screen, cur_col = -1, cur_len = 0;

    if ( line < view->doc->count ) {
        text = doc_line( view->doc, line );
        if ( text->flags & LF_BREAK ) {
            base = pal.bkpt;
        }
        if ( view->text >= 0 && !app_current( view->text, line, &cur_col, &cur_len ) ) {
            cur_col = -1;
        }
    } else {
        text = NULL;
    }
    scr_fill( row, 1, TEXT_COLS, ' ', base );
    if ( text == NULL ) {
        return;
    }
    while ( index < text->len && col < view->left + TEXT_COLS ) {
        ch = (u8)text->text[index];
        tab = ch == '\t';
        width = tab ? (int)((col / tab_size + 1) * tab_size - col) : 1;
        attr = base;
        if ( has_sel && line >= from->line && line <= to->line
             && (line > from->line || index >= from->col)
             && (line < to->line || index < to->col) ) {
            attr = pal.select;
        }
        while ( width-- > 0 ) {
            if ( col >= view->left ) {
                screen = (int)(col - view->left);
                if ( screen < TEXT_COLS ) {
                    scr_ch( row, 1 + screen, tab ? ' ' : ch, attr );
                }
            }
            col++;
        }
        index++;
    }
    if ( cur_col >= 0 ) {
        /* the statement to run next, and one blank after it */
        u32 from_col = doc_col_of( view->doc, text, (u32)cur_col );
        u32 to_col = doc_col_of( view->doc, text, (u32)cur_col + (u32)cur_len ) + 1;
        for ( col = from_col; col < to_col; col++ ) {
            if ( col >= view->left && col < view->left + TEXT_COLS ) {
                scr_recolor( row, 1 + (int)(col - view->left), 1,
                             base == pal.bkpt ? pal.bkpt_current : pal.current );
            }
        }
    }
    if ( has_sel && line >= from->line && line < to->line && (line > from->line || from->col <= text->len) ) {
        sel_end = doc_col_of( view->doc, text, text->len );
        if ( sel_end >= view->left && sel_end < view->left + TEXT_COLS ) {
            scr_recolor( row, 1 + (int)(sel_end - view->left), 1, pal.select );
        }
    }
}

/* the vertical box as a row of its track (0 is the first below the
   arrow), the horizontal one as a screen column */
static int vthumb( VIEW *view )
{
    int rows = view_text_rows( view );
    u32 count = view->doc->count;

    return count > 1 ? (int)(view->line * (u32)(rows - 3) / (count - 1)) : 0;
}

static int hthumb( VIEW *view )
{
    u32 left = view->left > 178 ? 178 : view->left;

    return 2 + (int)(left * 75 / 178);
}

static void draw_vscroll( VIEW *view )
{
    int rows = view_text_rows( view ), row;
    int col = scr_cols - 1, first = view->row + 1;

    if ( rows < 3 ) {
        return;
    }
    scr_ch( first, col, 0x18, pal.scroll );
    for ( row = 1; row < rows - 1; row++ ) {
        scr_ch( first + row, col, 0xB0, pal.scroll );
    }
    scr_ch( first + rows - 1, col, 0x19, pal.scroll );
    scr_ch( first + 1 + vthumb( view ), col, ' ', pal.thumb );
}

static void draw_hscroll( VIEW *view )
{
    int row = view->row + view->height - 1;

    scr_ch( row, 0, 0xB3, pal.frame );
    scr_ch( row, 1, 0x1B, pal.scroll );
    scr_fill( row, 2, scr_cols - 4, 0xB0, pal.scroll );
    scr_ch( row, scr_cols - 2, 0x1A, pal.scroll );
    scr_ch( row, hthumb( view ), ' ', pal.thumb );
    scr_ch( row, scr_cols - 1, 0xB3, pal.frame );
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
        scr_ch( view->row + 1 + row, scr_cols - 1, 0xB3, pal.frame );
    }
    if ( view->bars ) {
        draw_vscroll( view );
        draw_hscroll( view );
    }
}

void view_place_cursor( VIEW *view )
{
    scr_cursor( view->row + 1 + (int)(view->line - view->top),
                1 + (int)(view->col - view->left), insert_mode ? CUR_LINE : CUR_BLOCK );
}

/* the keyword or name the cursor is on */
static int is_word( int ch )
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
           || ch == '_' || ch == '.' || ch == '$' || ch == '%' || ch == '&' || ch == '!' || ch == '#';
}

void view_word_at( VIEW *view, char *out, u32 max )
{
    LINE *text = cur_line( view );
    u32 index = doc_index_of( view->doc, text, view->col ), start, end;

    out[0] = 0;
    if ( index > text->len ) {
        return;
    }
    if ( index == text->len || !is_word( (u8)text->text[index] ) ) {
        if ( index == 0 || !is_word( (u8)text->text[index - 1] ) ) {
            return;
        }
        index--;
    }
    for ( start = index; start > 0 && is_word( (u8)text->text[start - 1] ); start-- ) {
    }
    for ( end = index; end < text->len && is_word( (u8)text->text[end] ); end++ ) {
    }
    if ( end - start >= max ) {
        end = start + max - 1;
    }
    mem_cpy( out, text->text + start, end - start );
    out[end - start] = 0;
}

/* ------------------------------------------------------------------ */
/* leaving a line                                                      */
/* ------------------------------------------------------------------ */

static TOKEN check_toks[256];

/* the names on a line, learnt as spelt there: -> 1 if one changed */
static int learn_names( const char *text, u32 len )
{
    int count, i, changed_any = 0, col;
    const char *msg;
    u32 nlen;

    count = lex_line( text, len, check_toks, 256, &col, &msg );
    for ( i = 0; i < count; i++ ) {
        if ( check_toks[i].kind == TK_ID || check_toks[i].kind == TK_LABEL ) {
            nlen = check_toks[i].len;
            if ( nlen > 0 && lex_suffix_type( text[check_toks[i].col + nlen - 1] ) != TY_NONE ) {
                nlen--;
            }
            changed_any |= names_learn( text + check_toks[i].col, nlen );
        }
    }
    return changed_any;
}

/* one line put in QBasic's form, as it stands; 0 if it will not lex */
static int relist( DOC *doc, u32 line )
{
    static char out[MAX_LINE + 64];
    LINE *ln = doc_line( doc, line );
    int count, col;
    const char *msg;

    if ( ln->len == 0 ) {
        return 1;
    }
    count = lex_line( ln->text, ln->len, check_toks, 256, &col, &msg );
    if ( count < 0 ) {
        return 0;
    }
    if ( list_line( ln->text, ln->len, check_toks, out, sizeof( out ) ) < 0 ) {
        return 1;                               /* too long once listed: left as it is */
    }
    if ( str_len( out ) != ln->len || mem_cmp( out, ln->text, ln->len ) != 0 ) {
        doc_set_line( doc, line, out, str_len( out ) );
    }
    return 1;
}

/* every line of the program, after a name's spelling changed */
static void relist_all( void )
{
    int t;
    u32 line;

    for ( t = 0; t < ptext.ntexts; t++ ) {
        for ( line = 0; line < ptext.texts[t]->count; line++ ) {
            relist( ptext.texts[t], line );
        }
    }
}

/* a program loaded: its names learnt in the order they come, then every
   line in QBasic's form */
void view_learn_program( void )
{
    int t;
    u32 line;
    LINE *ln;

    names_reset();
    for ( t = 0; t < ptext.ntexts; t++ ) {
        for ( line = 0; line < ptext.texts[t]->count; line++ ) {
            ln = doc_line( ptext.texts[t], line );
            learn_names( ln->text ? ln->text : "", ln->len );
        }
    }
    relist_all();
}

/* SUB name or FUNCTION name, typed in the module: the line goes to a
   new procedure of its own, with a blank line and its END, and the
   window shows it there.  -> 1 if it did */
static int split_off_proc( VIEW *view, u32 line )
{
    LINE *ln = doc_line( view->doc, line );
    char name[NAME_MAX + 2], heading[MAX_LINE + 1];
    int kind, text;
    DOC *doc;

    if ( view->text != 0 || !prog_line_proc( ln->text, ln->len, name, &kind ) ) {
        return 0;
    }
    if ( prog_find( name ) >= 0 ) {
        msg_box( "Duplicate definition", MB_OK, 1 );
        return 1;
    }
    {
        u32 len = ln->len > MAX_LINE ? MAX_LINE : ln->len;
        mem_cpy( heading, ln->text, len );
        heading[len] = 0;
    }
    text = prog_add_proc( name, kind );
    if ( text < 0 ) {
        out_of_memory();
        return 1;
    }
    doc = ptext.texts[text];
    /* the heading as it was typed, parameters and all, and a blank line */
    {
        u32 at;
        for ( at = 0; at < doc->count; at++ ) {
            LINE *hl = doc_line( doc, at );
            if ( hl->len >= 3 && (ch_upper( hl->text[0] ) == 'S' || ch_upper( hl->text[0] ) == 'F') ) {
                doc_set_line( doc, at, heading, str_len( heading ) );
                doc_insert_line( doc, at + 1, "", 0 );
                doc->at_line = at + 1;
                break;
            }
        }
    }
    doc->at_col = 0;
    doc->at_top = 0;
    doc->at_left = 0;
    doc_delete_lines( view->doc, line, 1 );
    keep_last_empty( view->doc );
    view_fixup( view->doc );
    view->dirty = 0;
    view_show_text( view, text );
    return 1;
}

/* the cursor's line, typed on, checked as the cursor leaves it.
   -> 0 if it has an error (shown, and the cursor put on it) */
int view_leave_line( VIEW *view )
{
    static char out[MAX_LINE + 64];
    CERR err;
    LINE *ln;
    u32 line = view->line;

    if ( !view->dirty || view->text < 0 ) {
        view->dirty = 0;
        return 1;
    }
    view->dirty = 0;
    if ( line >= view->doc->count ) {
        return 1;
    }
    ln = doc_line( view->doc, line );
    if ( ln->len == 0 ) {
        return 1;
    }
    if ( !syntax_check ) {
        return 1;
    }
    if ( !comp_check_line( &ptext, view->text, line, ln->text, ln->len, out, sizeof( out ), &err ) ) {
        /* the place in the line shown, the message over it */
        view->err_line = (int)line;
        view->sel = 1;
        view->sel_line = line;
        view->sel_col = doc_col_of( view->doc, ln, (u32)err.col );
        view->line = line;
        view->col = doc_col_of( view->doc, ln, (u32)(err.col + (err.len > 0 ? err.len : 1)) );
        view_keep_visible( view );
        msg_box( err.msg, MB_OK, 1 );
        view->sel = 0;
        view->col = doc_col_of( view->doc, ln, (u32)err.col );
        view->want = view->col;
        view_keep_visible( view );
        return 0;
    }
    if ( learn_names( ln->text, ln->len ) ) {
        doc_set_line( view->doc, line, out, str_len( out ) );
        relist_all();
    } else if ( str_len( out ) != ln->len || mem_cmp( out, ln->text, ln->len ) != 0 ) {
        doc_set_line( view->doc, line, out, str_len( out ) );
    }
    split_off_proc( view, line );
    return 1;
}

/* ------------------------------------------------------------------ */
/* moving                                                              */
/* ------------------------------------------------------------------ */

static int is_word_ch( int ch )
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
           || ch == '_' || ch >= 0x80;
}

static void word_right( VIEW *view, u32 *line, u32 *col )
{
    LINE *text = doc_line( view->doc, *line );
    u32 index = doc_index_of( view->doc, text, *col );

    while ( index < text->len && is_word_ch( (u8)text->text[index] ) ) {
        index++;
    }
    while ( index < text->len && !is_word_ch( (u8)text->text[index] ) ) {
        index++;
    }
    if ( index >= text->len && *line + 1 < view->doc->count ) {
        (*line)++;
        text = doc_line( view->doc, *line );
        index = 0;
        while ( index < text->len && !is_word_ch( (u8)text->text[index] ) ) {
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
    while ( index > 0 && !is_word_ch( (u8)text->text[index - 1] ) ) {
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
        while ( index > 0 && !is_word_ch( (u8)text->text[index - 1] ) ) {
            index--;
        }
    }
    while ( index > 0 && is_word_ch( (u8)text->text[index - 1] ) ) {
        index--;
    }
    *col = doc_col_of( view->doc, text, index );
}

static int move_key( VIEW *view, int key )
{
    int select = (key & K_SHIFT) != 0;
    int rows = view_text_rows( view );
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
        /* to the text's first character, or to column 1 from there */
        text = cur_line( view );
        for ( index = 0; index < text->len && (text->text[index] == ' ' || text->text[index] == '\t'); index++ ) {
        }
        end = doc_col_of( view->doc, text, index );
        col = col == end || index == text->len ? 0 : end;
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
        view->left = view->left > TEXT_COLS ? view->left - TEXT_COLS : 0;
        col = col > TEXT_COLS ? col - TEXT_COLS : 0;
        keep_want = 0;
        break;
    case K_CPGDN:
        view->left += TEXT_COLS;
        col += TEXT_COLS;
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

    view_delete_sel( view );
    text = cur_line( view );
    pos = view_pos( view );
    if ( !insert_mode && pos.col < text->len ) {
        text->text[pos.col] = byte;
        view->col = doc_col_of( view->doc, text, pos.col + 1 );
        changed( view );
    } else {
        if ( view->col >= MAX_COL || !doc_insert( view->doc, &pos, &byte, 1 ) ) {
            return;
        }
        view->col = doc_col_of( view->doc, cur_line( view ), pos.col );
        changed( view );
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
    if ( view->text >= 0 ) {
        view->doc->modified = 1;
        ptext.modified = 1;
        app_edited();
    }
    keep_last_empty( view->doc );
    view_fixup( view->doc );
    view_keep_visible( view );
}

static void back_space( VIEW *view )
{
    LINE *text;
    POS from, to;
    u32 end;

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
        if ( !join_fits( view, from, to ) ) {
            return;
        }
        if ( !doc_delete( view->doc, from, to ) ) {
            out_of_memory();
            return;
        }
        view->line = from.line;
        view->col = doc_col_of( view->doc, doc_line( view->doc, from.line ), from.col );
        changed( view );
    } else if ( view->col > end ) {
        view->col--;
    } else {
        to.line = view->line;
        to.col = doc_index_of( view->doc, text, view->col );
        from.line = view->line;
        if ( doc_col_of( view->doc, text, to.col ) < view->col ) {
            from.col = to.col++;                /* inside a tab's columns: the tab goes */
        } else {
            from.col = to.col - 1;
        }
        doc_delete( view->doc, from, to );
        view->col = doc_col_of( view->doc, cur_line( view ), from.col );
        changed( view );
    }
    view->want = view->col;
    keep_last_empty( view->doc );
    view_fixup( view->doc );
    view_keep_visible( view );
}

static void delete_char( VIEW *view )
{
    LINE *text;
    POS from, to;

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
    if ( !join_fits( view, from, to ) ) {
        return;
    }
    if ( !doc_delete( view->doc, from, to ) ) {
        out_of_memory();
        return;
    }
    changed( view );
    keep_last_empty( view->doc );
    view_fixup( view->doc );
}

static void delete_word( VIEW *view )
{
    POS from = view_pos( view ), to;
    u32 line = view->line, col = view->col;

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
    changed( view );
    view_fixup( view->doc );
}

/* ^Y the line, ^QY the rest of it - both onto the clipboard */
static void cut_line( VIEW *view, int rest_only )
{
    POS from, to;
    char *text;
    u32 len;

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
        view->dirty = 0;                        /* a whole line is not checked */
        if ( view->text >= 0 ) {
            view->doc->modified = 1;
            ptext.modified = 1;
            app_edited();
        }
    } else {
        changed( view );
    }
    view->want = view->col;
    keep_last_empty( view->doc );
    view_fixup( view->doc );
    view_keep_visible( view );
}

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
    last = to.col == 0 ? to.line - 1 : to.line;
    mem_set( spaces, ' ', sizeof( spaces ) );
    for ( line = from.line; line <= last; line++ ) {
        text = doc_line( view->doc, line );
        at.line = line;
        at.col = 0;
        if ( out ) {
            remove = 0;
            while ( remove < text->len && remove < (u32)tab_size && text->text[remove] == ' ' ) {
                remove++;
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
    view->sel = 1;
    view->sel_line = from.line;
    view->sel_col = 0;
    view->line = last + 1 < view->doc->count ? last + 1 : last;
    view->col = last + 1 < view->doc->count ? 0 : line_end( view, last );
    if ( view->text >= 0 ) {
        view->doc->modified = 1;
        ptext.modified = 1;
        app_edited();
    }
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
    if ( !insert_mode || col >= line_end( view, view->line ) ) {
        view_goto( view, view->line, stop, 0 );
        view->want = view->col;
        return;
    }
    mem_set( spaces, ' ', sizeof( spaces ) );
    view_insert_text( view, spaces, stop - col < 32 ? stop - col : 32 );
}

static int prefixed( VIEW *view, int first, int key )
{
    int letter = ch_upper( key & 0xFF );

    if ( key >= 0x100 ) {
        letter = 0;
    } else if ( key < ' ' ) {
        letter = key + '@';
    }
    if ( first == CTRL( 'K' ) ) {
        return 1;
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
    case 'F': app_command( CMD_FIND );   break;
    case 'A': app_command( CMD_CHANGE ); break;
    }
    return 1;
}

/* one key in the window; the line is checked if the cursor left it */
static int edit_key( VIEW *view, int key )
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
    case CTRL( 'N' ):
        {
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
        app_status( " ^Q" );
        return 1;
    case CTRL( 'K' ):
        prefix = CTRL( 'K' );
        app_status( " ^K" );
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

int view_key( VIEW *view, int key )
{
    u32 old_line = view->line, now_line, now_col, now_want;
    int was_dirty = view->dirty, done;
    DOC *old_doc = view->doc;

    done = edit_key( view, key );
    if ( view->doc != old_doc || !was_dirty || view->line == old_line ) {
        return done;
    }
    if ( key == K_BS || key == K_DEL || key == CTRL( 'G' ) ) {
        return done;                            /* two lines joined: still typing */
    }
    /* the line typed on was left - by Enter, it is the first half, which
       stays where it was */
    now_line = view->line;
    now_col = view->col;
    now_want = view->want;
    view->line = old_line;
    view->dirty = 1;
    if ( view_leave_line( view ) && view->doc == old_doc ) {
        view->line = now_line < view->doc->count ? now_line : view->doc->count - 1;
        view->col = now_col;
        view->want = now_want;
        view_keep_visible( view );
    }
    return done;
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

int view_hit( VIEW *view, int row, int col )
{
    int rows = view_text_rows( view ), first = view->row + 1, thumb;

    if ( row == view->row ) {
        if ( view->text >= 0 && col >= scr_cols - 5 && col <= scr_cols - 3 ) {
            return VH_MAXBOX;
        }
        return VH_TITLE;
    }
    if ( view->bars && row == view->row + view->height - 1 ) {
        if ( col == 1 ) {
            return VH_LEFT;
        }
        if ( col == scr_cols - 2 ) {
            return VH_RIGHT;
        }
        if ( col < 2 || col > scr_cols - 3 ) {
            return VH_NONE;
        }
        thumb = hthumb( view );
        return col < thumb ? VH_PGLEFT : col > thumb ? VH_PGRIGHT : VH_HTHUMB;
    }
    if ( row < first || row >= first + rows ) {
        return VH_NONE;
    }
    if ( col == scr_cols - 1 ) {
        if ( !view->bars || rows < 3 ) {
            return VH_NONE;
        }
        if ( row == first ) {
            return VH_UP;
        }
        if ( row == first + rows - 1 ) {
            return VH_DOWN;
        }
        thumb = first + 1 + vthumb( view );
        return row < thumb ? VH_PGUP : row > thumb ? VH_PGDN : VH_VTHUMB;
    }
    if ( col >= 1 && col <= TEXT_COLS ) {
        return VH_TEXT;
    }
    return VH_NONE;
}

void view_mouse_to( VIEW *view, int row, int col, int select )
{
    int rows = view_text_rows( view );
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
    if ( text_col >= TEXT_COLS ) {
        text_col = TEXT_COLS - 1;
    }
    line = view->top + (u32)text_row;
    if ( line >= view->doc->count ) {
        line = view->doc->count - 1;
    }
    view_goto( view, line, snap( view, line, view->left + (u32)text_col ), select );
    view->want = view->col;
}

void view_select_word( VIEW *view )
{
    LINE *text = cur_line( view );
    u32 index = doc_index_of( view->doc, text, view->col ), start, end;

    if ( index >= text->len || !is_word_ch( (u8)text->text[index] ) ) {
        return;
    }
    for ( start = index; start > 0 && is_word_ch( (u8)text->text[start - 1] ); start-- ) {
    }
    for ( end = index; end < text->len && is_word_ch( (u8)text->text[end] ); end++ ) {
    }
    view->sel = 1;
    view->sel_line = view->line;
    view->sel_col = doc_col_of( view->doc, text, start );
    view->col = doc_col_of( view->doc, text, end );
    view->want = view->col;
    view_keep_visible( view );
}

void view_scroll_part( VIEW *view, int part, int row, int col )
{
    int rows = view_text_rows( view ), first = view->row + 1, at;
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
        if ( view->col >= view->left + TEXT_COLS ) {
            view_goto( view, view->line, view->left + TEXT_COLS - 1, 0 );
            view->want = view->col;
        }
        break;
    case VH_RIGHT:
        if ( view->left < MAX_COL ) {
            view->left++;
        }
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
        if ( at < 0 ) {
            at = 0;
        }
        if ( at > 75 ) {
            at = 75;
        }
        view->left = (u32)at * 178 / 75;
        view_goto( view, view->line, view->left, 0 );
        view->want = view->col;
        break;
    }
}
