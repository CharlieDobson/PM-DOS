/*
 * UI.C - the DOSForms toolkit's widgets that XArc uses, as C: a box that
 * puts back what it covered (POPUP), buttons, the message box that sizes
 * itself to its text, a field to type in and the box around one, check
 * boxes and a set of choices, the progress and "please wait" boxes, the
 * text viewer, the title and status lines, and the question asked when
 * a disk is not there.
 *
 * Each is the toolkit's class of the same name done over (DOSFORMS.CPP,
 * POPUP.CPP, BUTTON.CPP, DLGBOX.CPP, TEXTFLD.CPP, INPUTBOX.CPP,
 * CHECKBOX.CPP, PROGBOX.CPP, TEXTVIEW.CPP, TITLEBAR.CPP, STATBAR.CPP,
 * CRITERR.CPP) with the same sizes, places, colours and keys, so that
 * the screens are the DOS program's.  What changed is underneath: a box
 * waits in ev_wait for an event instead of spinning on kbhit and the
 * mouse driver, and a click is an event SCR.C has already found the
 * edge of, so none of the toolkit's "seed the edge detector" care is
 * needed here.
 */
#include "xarc.h"

/* Keep the TAIL of a name that will not fit: the file at the end is what
   identifies it.  "dst" holds room + 1. */
void shorten_tail( char *dst, int room, const char *text )
{
    int len;

    if ( text == NULL ) {
        text = "";
    }
    len = (int)strlen( text );
    if ( room < 4 ) {
        dst[0] = 0;
        return;
    }
    if ( len <= room ) {
        strcpy( dst, text );
        return;
    }
    strcpy( dst, "..." );
    strcat( dst, text + (len - (room - 3)) );
}

/* ------------------------------------------------------------------ */
/* boxes                                                               */
/* ------------------------------------------------------------------ */

void draw_box( int top, int left, int bottom, int right, int style, int fg, int bg )
{
    char edge[134], side[2];
    int tl, tr, bl, br, hz, vt, width, at, row;

    if ( style == BORDER_NONE || bottom < top || right < left ) {
        return;
    }
    if ( style == BORDER_DOUBLE ) {
        tl = 201; tr = 187; bl = 200; br = 188; hz = 205; vt = 186;
    } else {
        tl = 218; tr = 191; bl = 192; br = 217; hz = 196; vt = 179;
    }
    width = right - left + 1;
    if ( width > (int)sizeof( edge ) - 1 ) {
        width = (int)sizeof( edge ) - 1;
    }
    edge[0] = (char)tl;
    for ( at = 1; at < width - 1; at++ ) {
        edge[at] = (char)hz;
    }
    edge[width - 1] = (char)tr;
    edge[width] = 0;
    df_text( top, left, edge, fg, bg );
    side[0] = (char)vt;
    side[1] = 0;
    for ( row = top + 1; row < bottom; row++ ) {
        df_text( row, left, side, fg, bg );
        df_text( row, right, side, fg, bg );
    }
    edge[0] = (char)bl;
    edge[width - 1] = (char)br;
    df_text( bottom, left, edge, fg, bg );
}

void popup_init( POPUP *pop, int fg, int bg, const char *title )
{
    memset( pop, 0, sizeof( *pop ) );
    pop->fg = fg;
    pop->bg = bg;
    pop->border = BORDER_NONE;
    pop->just = JUST_LEFT;
    pop->shadow = 1;
    if ( title ) {
        strncpy( pop->title, title, sizeof( pop->title ) - 1 );
    }
}

void popup_place( POPUP *pop, int row, int col, int end_row, int end_col )
{
    if ( end_row < row || end_col < col ) {
        return;
    }
    pop->at.row = row;
    pop->at.col = col;
    pop->at.end_row = end_row;
    pop->at.end_col = end_col;
}

static int popup_save( POPUP *pop )
{
    RECT box = pop->at;
    u32 want;

    if ( pop->shadow ) {
        box.end_row += 1;
        box.end_col += 2;
    }
    if ( box.end_row > df_rows() - 1 ) {
        box.end_row = df_rows() - 1;
    }
    if ( box.end_col > df_cols() - 1 ) {
        box.end_col = df_cols() - 1;
    }
    if ( box.end_row < box.row || box.end_col < box.col ) {
        return 0;
    }
    want = (u32)(box.end_row - box.row + 1) * (u32)(box.end_col - box.col + 1);
    if ( want > pop->save_cells ) {
        CELL *grown = (CELL *)malloc( want * sizeof( CELL ) );

        if ( grown == NULL ) {
            return 0;
        }
        free( pop->save );
        pop->save = grown;
        pop->save_cells = want;
    }
    df_cells_read( box.row + 1, box.col + 1, box.end_row + 1, box.end_col + 1, pop->save );
    pop->saved_at = box;
    pop->saved = 1;
    return 1;
}

void popup_draw( POPUP *pop )
{
    int top = pop->at.row + 1, left = pop->at.col + 1;
    int bottom = pop->at.end_row + 1, right = pop->at.end_col + 1;
    int len, title_col;
    char temp[40];

    if ( pop->at.end_row == 0 && pop->at.end_col == 0 ) {
        return;
    }
    if ( !popup_save( pop ) ) {
        return;
    }
    if ( pop->shadow ) {
        df_shade( top + 1, left + 2, bottom + 1, right + 2 );
    }
    draw_box( top, left, bottom, right, pop->border, pop->fg, pop->bg );
    df_fill( top + 1, left + 1, bottom - 1, right - 1, pop->bg );
    if ( pop->title[0] ) {
        len = (int)strlen( pop->title );
        if ( pop->just == JUST_CENTER ) {
            title_col = pop->at.col + (pop->at.end_col - pop->at.col) / 2 - len / 2;
        } else if ( pop->just == JUST_RIGHT ) {
            title_col = pop->at.end_col - len - 3;
        } else {
            title_col = pop->at.col + 3;
        }
        sprintf( temp, " %s ", pop->title );
        df_text( pop->at.row + 1, title_col + 1, temp, pop->fg, pop->bg );
    }
}

void popup_restore( POPUP *pop )
{
    if ( !pop->save || !pop->saved ) {
        return;
    }
    df_cells_write( pop->saved_at.row + 1, pop->saved_at.col + 1,
                    pop->saved_at.end_row + 1, pop->saved_at.end_col + 1, pop->save );
}

void popup_free( POPUP *pop )
{
    free( pop->save );
    pop->save = NULL;
    pop->save_cells = 0;
    pop->saved = 0;
}

/* ------------------------------------------------------------------ */
/* buttons: "[ Label ]" on one row                                     */
/* ------------------------------------------------------------------ */

void button_init( BUTTON *btn, int fg, int bg, const char *label )
{
    memset( btn, 0, sizeof( *btn ) );
    btn->fg = fg;
    btn->bg = bg;
    strncpy( btn->label, label, sizeof( btn->label ) - 1 );
}

void button_row( BUTTON *btn, int row, int col )
{
    int width = btn->label[0] ? (int)strlen( btn->label ) + 4 : 4;

    btn->at.row = btn->at.end_row = row;
    btn->at.col = col;
    btn->at.end_col = col + width - 1;
}

void button_draw( const BUTTON *btn )
{
    int top = btn->at.row + 1, left = btn->at.col + 1, right = btn->at.end_col + 1;
    int inner = right - left - 1, len;
    char temp[24];

    df_fill( top, left, top, right, btn->bg );
    df_text( top, left, "[", btn->fg, btn->bg );
    df_text( top, right, "]", btn->fg, btn->bg );
    if ( btn->label[0] && inner > 0 ) {
        len = (int)strlen( btn->label );
        if ( len > inner ) {
            len = inner;
        }
        memcpy( temp, btn->label, (size_t)len );
        temp[len] = 0;
        df_text( top, left + 1 + (inner - len) / 2, temp, btn->fg, btn->bg );
    }
}

int button_hit( const BUTTON *btn, int x, int y )
{
    return y >= btn->at.row && y <= btn->at.end_row && x >= btn->at.col && x <= btn->at.end_col;
}

/* ------------------------------------------------------------------ */
/* message boxes (DLGBOX.CPP)                                          */
/* ------------------------------------------------------------------ */

#define DLG_MSG_MAX     1024
#define DLG_WRAP_LINES  24
#define DLG_WRAP_COLS   78
#define DLG_MAX_BUTTONS 4

static char dlg_line[DLG_WRAP_LINES][DLG_WRAP_COLS];
static int  dlg_lines, dlg_widest;

/* One wrapper, for the sizing and the painting alike: '\n' starts a
   line where it stands, and everything else breaks at a space, a word
   going to the next line whole unless it is longer than a line. */
static void dlg_wrap( const char *text, int width, int max_lines )
{
    const char *at = text, *scan;
    int len, brk;

    dlg_lines = dlg_widest = 0;
    if ( width < 1 ) {
        width = 1;
    }
    if ( width > DLG_WRAP_COLS - 1 ) {
        width = DLG_WRAP_COLS - 1;
    }
    if ( max_lines > DLG_WRAP_LINES ) {
        max_lines = DLG_WRAP_LINES;
    }
    if ( max_lines < 1 ) {
        max_lines = 1;
    }
    while ( text && *at && dlg_lines < max_lines ) {
        scan = at;
        len = 0;
        brk = -1;
        while ( *scan && *scan != '\n' && len < width ) {
            if ( *scan == ' ' ) {
                brk = len;
            }
            dlg_line[dlg_lines][len++] = *scan++;
        }
        if ( *scan && *scan != '\n' && *scan != ' ' && brk > 0 ) {
            len = brk;
            scan = at + brk;
        }
        while ( len > 0 && dlg_line[dlg_lines][len - 1] == ' ' ) {
            len--;
        }
        dlg_line[dlg_lines][len] = 0;
        if ( len > dlg_widest ) {
            dlg_widest = len;
        }
        dlg_lines++;
        while ( *scan == ' ' ) {
            scan++;
        }
        if ( *scan == '\n' ) {
            scan++;
        }
        at = scan;
    }
    if ( dlg_lines == 0 ) {
        dlg_line[0][0] = 0;
        dlg_lines = 1;
    }
}

/* the first letter of each word not already taken, or any free letter */
static void dlg_hotkeys( const char *const *labels, int count, char *keys )
{
    char used[DLG_MAX_BUTTONS + 1];
    int nused = 0, one, at, prev;
    char ch;

    for ( one = 0; one < count; one++ ) {
        keys[one] = 0;
        for ( at = 0; labels[one][at] && !keys[one]; at++ ) {
            if ( at > 0 && labels[one][at - 1] != ' ' ) {
                continue;
            }
            ch = (char)toupper( (u8)labels[one][at] );
            if ( !isalnum( (u8)ch ) ) {
                continue;
            }
            for ( prev = 0; prev < nused && used[prev] != ch; prev++ ) {
            }
            if ( prev == nused ) {
                keys[one] = ch;
            }
        }
        for ( at = 0; labels[one][at] && !keys[one]; at++ ) {
            ch = (char)toupper( (u8)labels[one][at] );
            if ( !isalnum( (u8)ch ) ) {
                continue;
            }
            for ( prev = 0; prev < nused && used[prev] != ch; prev++ ) {
            }
            if ( prev == nused ) {
                keys[one] = ch;
            }
        }
        if ( keys[one] ) {
            used[nused++] = keys[one];
        }
    }
}

/* The box, sized to its text and centred; the text in it, as lines
   centred one by one or (block) sharing a left edge; buttons on the
   bottom row; and the answer - Enter the first button, Esc the last. */
static int dlg_run( int fg, int bg, const char *title, const char *text, int block,
                    const char *const *labels, int count )
{
    POPUP box;
    BUTTON btn[DLG_MAX_BUTTONS];
    char keys[DLG_MAX_BUTTONS], message[DLG_MSG_MAX];
    int rows = df_rows(), cols = df_cols(), wrap, max_lines, w, h, top, left;
    int inner_width, inner_top, inner_height, start_row, line, len, col;
    int total, start, at, result = -1;
    EVENT ev;

    strncpy( message, text ? text : "", sizeof( message ) - 1 );
    message[sizeof( message ) - 1] = 0;

    wrap = cols - 8;
    if ( wrap < 16 ) {
        wrap = 16;
    }
    max_lines = rows - 6;
    dlg_wrap( message, wrap, max_lines );
    w = dlg_widest + 6;
    if ( w < 24 ) {
        w = 24;
    }
    if ( w > cols ) {
        w = cols;
    }
    h = dlg_lines + 5;
    if ( h > rows ) {
        h = rows;
    }
    left = (cols - w) / 2;
    top = (rows - h) / 2;

    popup_init( &box, fg, bg, title );
    box.border = BORDER_DOUBLE;
    box.just = JUST_CENTER;
    popup_place( &box, top, left, top + h - 1, left + w - 1 );
    popup_draw( &box );

    inner_width = box.at.end_col - box.at.col - 2;
    inner_top = box.at.row + 1;
    inner_height = box.at.end_row - 2 - inner_top;
    if ( inner_height < 1 ) {
        inner_height = 1;
    }
    dlg_wrap( message, inner_width, inner_height );
    start_row = inner_top + (inner_height - dlg_lines) / 2;
    if ( start_row < inner_top ) {
        start_row = inner_top;
    }
    for ( line = 0; line < dlg_lines; line++ ) {
        len = block ? dlg_widest : (int)strlen( dlg_line[line] );
        col = box.at.col + 1 + (inner_width - len) / 2;
        df_text( start_row + line + 1, col + 1, dlg_line[line], fg, bg );
    }

    dlg_hotkeys( labels, count, keys );
    total = (count - 1) * 2;
    for ( at = 0; at < count; at++ ) {
        total += (int)strlen( labels[at] ) + 4;
    }
    start = box.at.col + 1 + (inner_width - total) / 2;
    if ( start < box.at.col + 1 ) {
        start = box.at.col + 1;
    }
    for ( at = 0; at < count; at++ ) {
        button_init( &btn[at], FG_BLACK, BG_WHITE, labels[at] );
        button_row( &btn[at], box.at.end_row - 1, start );
        button_draw( &btn[at] );
        start += (int)strlen( labels[at] ) + 4 + 2;
    }

    while ( result < 0 ) {
        ev_wait( &ev );
        if ( ev.kind == EV_MOUSE && (ev.press & LEFT_BUTTON) ) {
            for ( at = 0; at < count; at++ ) {
                if ( button_hit( &btn[at], ev.x, ev.y ) ) {
                    result = at;
                    break;
                }
            }
        } else if ( ev.kind == EV_KEY ) {
            if ( ev.key == KEY_ENTER ) {
                result = 0;
            } else if ( ev.key == KEY_ESC ) {
                result = count - 1;
            } else if ( ev.key > 0 && ev.key < 0x100 ) {
                for ( at = 0; at < count; at++ ) {
                    if ( keys[at] && toupper( ev.key ) == keys[at] ) {
                        result = at;
                        break;
                    }
                }
            }
        }
    }
    popup_restore( &box );
    popup_free( &box );
    return result;
}

int msg_box( int fg, int bg, const char *title, const char *text, int buttons )
{
    static const char *const ok[] = { "OK" };
    static const char *const okcancel[] = { "OK", "Cancel" };
    static const char *const yesno[] = { "Yes", "No" };
    int picked;

    if ( buttons == DB_YESNO ) {
        picked = dlg_run( fg, bg, title, text, 0, yesno, 2 );
        return picked == 0 ? ANS_YES : ANS_NO;
    }
    if ( buttons == DB_OKCANCEL ) {
        picked = dlg_run( fg, bg, title, text, 0, okcancel, 2 );
        return picked == 0 ? ANS_OK : ANS_CANCEL;
    }
    dlg_run( fg, bg, title, text, 0, ok, 1 );
    return ANS_OK;
}

void msg_block( const char *title, const char *text )
{
    static const char *const ok[] = { "OK" };

    dlg_run( FG_BLACK, BG_WHITE, title, text, 1, ok, 1 );
}

int msg_choose( int fg, int bg, const char *title, const char *text,
                const char *const *labels, int count )
{
    if ( count > DLG_MAX_BUTTONS ) {
        count = DLG_MAX_BUTTONS;
    }
    return dlg_run( fg, bg, title, text, 0, labels, count );
}

/* ------------------------------------------------------------------ */
/* a field (TEXTFLD.CPP)                                               */
/* ------------------------------------------------------------------ */

void field_init( FIELD *fld, int fg, int bg )
{
    memset( fld, 0, sizeof( *fld ) );
    fld->fg = fg;
    fld->bg = bg;
    fld->maxlen = TF_TEXT_MAX - 1;
}

/* one column kept free at the right, for the cursor past the last
   character to sit in */
static void field_clamp( FIELD *fld )
{
    if ( fld->cur < 0 ) {
        fld->cur = 0;
    }
    if ( fld->cur > fld->len ) {
        fld->cur = fld->len;
    }
    if ( fld->width < 1 ) {
        fld->scroll = 0;
        return;
    }
    if ( fld->width < 3 ) {
        fld->scroll = fld->cur;
        return;
    }
    if ( fld->cur < fld->scroll ) {
        fld->scroll = fld->cur;
    }
    if ( fld->cur > fld->scroll + fld->width - 2 ) {
        fld->scroll = fld->cur - (fld->width - 2);
    }
    if ( fld->scroll < 0 ) {
        fld->scroll = 0;
    }
}

void field_place( FIELD *fld, int row, int col, int width )
{
    fld->row = row;
    fld->col = col;
    fld->width = width > TF_TEXT_MAX - 1 ? TF_TEXT_MAX - 1 : width;
    fld->scroll = 0;
    field_clamp( fld );
}

void field_set( FIELD *fld, const char *text )
{
    fld->buffer[0] = 0;
    if ( text ) {
        strncpy( fld->buffer, text, (size_t)fld->maxlen );
        fld->buffer[fld->maxlen] = 0;
    }
    fld->len = (int)strlen( fld->buffer );
    fld->cur = fld->len;
    fld->scroll = 0;
    field_clamp( fld );
}

void field_max( FIELD *fld, int max )
{
    if ( max < 1 ) {
        max = 1;
    }
    if ( max > TF_TEXT_MAX - 1 ) {
        max = TF_TEXT_MAX - 1;
    }
    fld->maxlen = max;
    if ( fld->len > max ) {
        fld->len = max;
        fld->buffer[max] = 0;
    }
    field_clamp( fld );
}

int field_hit( const FIELD *fld, int x, int y )
{
    return y == fld->row && x >= fld->col && x < fld->col + fld->width;
}

void field_draw( FIELD *fld )
{
    char out[TF_TEXT_MAX], cell[2];
    int at, src;

    if ( fld->width < 1 ) {
        return;
    }
    field_clamp( fld );
    for ( at = 0; at < fld->width; at++ ) {
        src = fld->scroll + at;
        out[at] = src < fld->len ? (fld->masked ? (char)250 : fld->buffer[src]) : ' ';
    }
    out[fld->width] = 0;
    df_text( fld->row + 1, fld->col + 1, out, fld->fg, fld->bg );
    if ( fld->focus && fld->cur - fld->scroll >= 0 && fld->cur - fld->scroll < fld->width ) {
        cell[0] = fld->cur < fld->len ? (fld->masked ? (char)250 : fld->buffer[fld->cur]) : ' ';
        cell[1] = 0;
        df_text( fld->row + 1, fld->col + 1 + (fld->cur - fld->scroll), cell, FG_WHITE, BG_BLACK );
    }
}

int field_event( FIELD *fld, const EVENT *ev )
{
    int at;

    if ( ev->kind == EV_MOUSE ) {
        if ( !(ev->press & LEFT_BUTTON) || !field_hit( fld, ev->x, ev->y ) ) {
            return 0;
        }
        fld->focus = 1;
        fld->cur = fld->scroll + (ev->x - fld->col);
        if ( fld->cur > fld->len ) {
            fld->cur = fld->len;
        }
        field_draw( fld );
        return 1;
    }
    if ( ev->kind != EV_KEY || !fld->focus ) {
        return 0;
    }
    switch ( ev->key ) {
    case KEY_LEFT:
        if ( fld->cur > 0 ) {
            fld->cur--;
        }
        break;
    case KEY_RIGHT:
        if ( fld->cur < fld->len ) {
            fld->cur++;
        }
        break;
    case KEY_HOME:
        fld->cur = 0;
        break;
    case KEY_END:
        fld->cur = fld->len;
        break;
    case KEY_DEL:
        if ( fld->cur < fld->len ) {
            for ( at = fld->cur; at < fld->len - 1; at++ ) {
                fld->buffer[at] = fld->buffer[at + 1];
            }
            fld->buffer[--fld->len] = 0;
        }
        break;
    case KEY_BS:
        if ( fld->cur > 0 ) {
            for ( at = fld->cur - 1; at < fld->len - 1; at++ ) {
                fld->buffer[at] = fld->buffer[at + 1];
            }
            fld->buffer[--fld->len] = 0;
            fld->cur--;
        }
        break;
    default:
        if ( ev->key < 32 || ev->key > 255 || ev->key == 127 ) {
            return 0;
        }
        if ( fld->len >= fld->maxlen ) {
            break;
        }
        for ( at = fld->len; at > fld->cur; at-- ) {
            fld->buffer[at] = fld->buffer[at - 1];
        }
        fld->buffer[fld->cur++] = (char)ev->key;
        fld->buffer[++fld->len] = 0;
        break;
    }
    field_draw( fld );
    return 1;
}

/* ------------------------------------------------------------------ */
/* the input box (INPUTBOX.CPP)                                        */
/* ------------------------------------------------------------------ */

#define IB_W_MIN    62
#define IB_H        8

int input_box( int fg, int bg, const char *title, const char *prompt,
               char *text, int maxlen, int flags )
{
    POPUP box;
    FIELD fld;
    BUTTON ok, cancel, skip;
    char shown[128];
    int rows = df_rows(), cols = df_cols(), want, room, top, left, row, wide, oc;
    int result = -1, has_skip = (flags & IB_SKIPBTN) != 0;
    EVENT ev;

    want = prompt ? (int)strlen( prompt ) + 6 : IB_W_MIN;
    if ( want < IB_W_MIN ) {
        want = IB_W_MIN;
    }
    if ( want > cols - 2 ) {
        want = cols - 2;
    }
    if ( want < 24 ) {
        want = 24;
    }
    room = want - 6;
    if ( room > (int)sizeof( shown ) - 1 ) {
        room = (int)sizeof( shown ) - 1;
    }
    shown[0] = 0;
    if ( prompt && room > 0 ) {
        strncpy( shown, prompt, (size_t)room );
        shown[room] = 0;
    }
    left = (cols - want) / 2;
    top = (rows - IB_H) / 2;

    popup_init( &box, fg, bg, title );
    box.border = BORDER_DOUBLE;
    box.just = JUST_CENTER;
    popup_place( &box, top, left, top + IB_H - 1, left + want - 1 );
    popup_draw( &box );
    df_text( box.at.row + 3, box.at.col + 4, shown, fg, bg );

    row = box.at.row + IB_H - 2;
    wide = has_skip ? 33 : 20;
    oc = box.at.col + (want - wide) / 2;
    if ( oc < box.at.col + 2 ) {
        oc = box.at.col + 2;
    }
    button_init( &ok, FG_WHITE, BG_CYAN, "OK" );
    button_init( &skip, FG_WHITE, BG_CYAN, "Skip F3" );
    button_init( &cancel, FG_WHITE, BG_CYAN, "Cancel" );
    button_row( &ok, row, oc );
    if ( has_skip ) {
        button_row( &skip, row, oc + 8 );
        button_row( &cancel, row, oc + 21 );
    } else {
        button_row( &cancel, row, oc + 10 );
    }
    button_draw( &ok );
    if ( has_skip ) {
        button_draw( &skip );
    }
    button_draw( &cancel );

    field_init( &fld, FG_BLACK, BG_WHITE );
    fld.focus = 1;
    fld.masked = (flags & IB_MASKED) != 0;
    if ( maxlen > 0 ) {
        field_max( &fld, maxlen );
    }
    field_set( &fld, text );
    field_place( &fld, box.at.row + 4, box.at.col + 3, want - 6 < 8 ? 8 : want - 6 );
    field_draw( &fld );

    while ( result < 0 ) {
        ev_wait( &ev );
        if ( ev.kind == EV_KEY ) {
            if ( has_skip && ev.key == KEY_F3 ) {
                result = IB_SKIP;
            } else if ( ev.key == KEY_ENTER ) {
                result = IB_OK;
            } else if ( ev.key == KEY_ESC ) {
                result = IB_CANCEL;
            } else {
                field_event( &fld, &ev );
            }
        } else if ( ev.kind == EV_MOUSE && (ev.press & LEFT_BUTTON) ) {
            if ( button_hit( &ok, ev.x, ev.y ) ) {
                result = IB_OK;
            } else if ( has_skip && button_hit( &skip, ev.x, ev.y ) ) {
                result = IB_SKIP;
            } else if ( button_hit( &cancel, ev.x, ev.y ) ) {
                result = IB_CANCEL;
            } else {
                field_event( &fld, &ev );
            }
        }
    }
    popup_restore( &box );
    popup_free( &box );
    if ( result == IB_OK ) {
        strcpy( text, fld.buffer );
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* check boxes and a set of choices (CHECKBOX.CPP)                     */
/* ------------------------------------------------------------------ */

void check_init( CHECK *chk, int fg, int bg, const char *label, int row, int col )
{
    memset( chk, 0, sizeof( *chk ) );
    chk->fg = fg;
    chk->bg = bg;
    strncpy( chk->label, label, sizeof( chk->label ) - 1 );
    chk->row = row;
    chk->col = col;
    chk->end_col = col + 3 + (int)strlen( chk->label );
}

void check_draw( const CHECK *chk )
{
    char out[56];

    sprintf( out, "[%c] %s", chk->checked ? 'X' : ' ', chk->label );
    df_text( chk->row + 1, chk->col + 1, out, chk->fg, chk->bg );
}

int check_event( CHECK *chk, const EVENT *ev )
{
    if ( ev->kind == EV_MOUSE && (ev->press & LEFT_BUTTON) && ev->y == chk->row
         && ev->x >= chk->col && ev->x <= chk->end_col ) {
        chk->checked = !chk->checked;
        check_draw( chk );
        return 1;
    }
    return 0;
}

void radio_init( RADIO *rad, int fg, int bg, int row, int col )
{
    memset( rad, 0, sizeof( *rad ) );
    rad->fg = fg;
    rad->bg = bg;
    rad->row = row;
    rad->col = col;
}

void radio_add( RADIO *rad, const char *label )
{
    if ( rad->count < RG_MAX ) {
        strncpy( rad->item[rad->count], label, sizeof( rad->item[0] ) - 1 );
        rad->count++;
    }
}

void radio_draw( const RADIO *rad )
{
    char out[56];
    int at;

    for ( at = 0; at < rad->count; at++ ) {
        sprintf( out, "(%c) %s", at == rad->chosen ? '*' : ' ', rad->item[at] );
        df_text( rad->row + 1 + at, rad->col + 1, out, rad->fg, rad->bg );
    }
}

/* a click picks a line and gives the arrows to the set, as the
   toolkit's did; until then the arrows are somebody else's */
int radio_event( RADIO *rad, const EVENT *ev )
{
    int at;

    if ( ev->kind == EV_MOUSE ) {
        if ( !(ev->press & LEFT_BUTTON) ) {
            return 0;
        }
        at = ev->y - rad->row;
        if ( at < 0 || at >= rad->count || ev->x < rad->col
             || ev->x > rad->col + 3 + (int)strlen( rad->item[at] ) ) {
            return 0;
        }
        rad->focused = 1;
        rad->chosen = at;
        radio_draw( rad );
        return 1;
    }
    if ( ev->kind != EV_KEY || !rad->focused ) {
        return 0;
    }
    if ( ev->key == KEY_UP ) {
        if ( rad->chosen > 0 ) {
            rad->chosen--;
            radio_draw( rad );
        }
        return 1;
    }
    if ( ev->key == KEY_DOWN ) {
        if ( rad->chosen < rad->count - 1 ) {
            rad->chosen++;
            radio_draw( rad );
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the progress box and the wait box (PROGBOX.CPP)                     */
/* ------------------------------------------------------------------ */

#define PB_HEIGHT   6

static POPUP prog_box;
static int   prog_opened, prog_cancelled;

void prog_open( const char *title )
{
    int rows = df_rows(), cols = df_cols(), bw = PB_WIDTH, left, top;

    if ( prog_opened ) {
        return;
    }
    if ( bw > cols ) {
        bw = cols;
    }
    left = (cols - bw) / 2;
    top = (rows - PB_HEIGHT) / 2;
    popup_init( &prog_box, FG_BLACK, BG_WHITE, title );
    prog_box.border = BORDER_DOUBLE;
    prog_box.just = JUST_CENTER;
    popup_place( &prog_box, top, left, top + PB_HEIGHT - 1, left + bw - 1 );
    popup_draw( &prog_box );
    df_bar( top + 4, left + 3, PB_ROOM, 0, 1, FG_BLUE, BG_WHITE );
    df_text( top + 5, left + 3, "Press Esc to cancel", FG_BLACK, BG_WHITE );
    prog_cancelled = 0;
    prog_opened = 1;
    df_flush();
}

int prog_is_open( void )
{
    return prog_opened;
}

/* the name line and the bar, then a look for Esc that does not wait.
   Once the answer is "stop" it stays "stop". */
int prog_update( const char *text, u32 done, u32 total )
{
    char line[PB_ROOM + 2];
    int row, col, room, len;
    EVENT ev;

    if ( prog_cancelled ) {
        return 0;
    }
    if ( !prog_opened ) {
        return 1;
    }
    if ( total < 1 ) {
        total = 1;
    }
    if ( done > total ) {
        done = total;
    }
    row = prog_box.at.row;
    col = prog_box.at.col;
    room = prog_box.at.end_col - prog_box.at.col - 5;
    if ( room > PB_ROOM ) {
        room = PB_ROOM;
    }
    if ( room < 1 ) {
        room = 1;
    }
    if ( text ) {
        shorten_tail( line, room, text );
        for ( len = (int)strlen( line ); len < room; len++ ) {
            line[len] = ' ';
        }
        line[room] = 0;
        df_text( row + 3, col + 3, line, FG_BLACK, BG_WHITE );
    }
    df_bar( row + 4, col + 3, room, done, total, FG_BLUE, BG_WHITE );
    while ( ev_poll( &ev ) ) {
        if ( ev.kind == EV_KEY && ev.key == KEY_ESC ) {
            prog_cancelled = 1;
        }
    }
    return !prog_cancelled;
}

void prog_close( void )
{
    if ( !prog_opened ) {
        return;
    }
    prog_opened = 0;
    popup_restore( &prog_box );
    popup_free( &prog_box );
}

static POPUP wait_box;
static int   wait_opened;

void wait_open( const char *message )
{
    int rows = df_rows(), cols = df_cols(), bh = 5, w, bw, left, top;
    char shown[128];

    if ( wait_opened ) {
        return;
    }
    w = message ? (int)strlen( message ) : 0;
    if ( w > cols - 6 ) {
        w = cols - 6;
    }
    if ( w > (int)sizeof( shown ) - 1 ) {
        w = (int)sizeof( shown ) - 1;
    }
    bw = w + 6;
    if ( bw < 24 ) {
        bw = 24;
    }
    if ( bw > cols ) {
        bw = cols;
    }
    left = (cols - bw) / 2;
    top = (rows - bh) / 2;
    popup_init( &wait_box, FG_BLACK, BG_WHITE, "Please Wait" );
    wait_box.border = BORDER_DOUBLE;
    wait_box.just = JUST_CENTER;
    popup_place( &wait_box, top, left, top + bh - 1, left + bw - 1 );
    popup_draw( &wait_box );
    if ( message ) {
        memcpy( shown, message, (size_t)w );
    }
    shown[w] = 0;
    df_text( top + 3, left + (bw - w) / 2 + 1, shown, FG_BLACK, BG_WHITE );
    wait_opened = 1;
    df_flush();
}

void wait_close( void )
{
    if ( !wait_opened ) {
        return;
    }
    wait_opened = 0;
    popup_restore( &wait_box );
    popup_free( &wait_box );
}

/* ------------------------------------------------------------------ */
/* the text viewer (TEXTVIEW.CPP)                                      */
/* ------------------------------------------------------------------ */

#define TV_MAX_BYTES    (192L * 1024L)
#define TV_MAX_LINES    8192
#define TV_TAB          8
#define TV_LINEBUF      168
#define TV_FIND_MAX     64

typedef struct {
    char  *text;
    long   text_len;
    char **line;
    int    lines, truncated, top, left;
    char   find[TV_FIND_MAX];
    int    hit, not_found;
    char   title[40];
    POPUP  box;
    BUTTON close;
} VIEWER;

/* tabs to spaces, CRs dropped, control bytes as '.', and the lines
   indexed by pointing into the one buffer */
static int tv_index( VIEWER *tv, const char *raw, long got, int cut )
{
    long at, out_len = 0;
    int col = 0, pad;
    u8 ch;

    tv->truncated = cut;
    for ( at = 0; at < got; at++ ) {
        if ( raw[at] == '\t' ) {
            out_len += TV_TAB - col % TV_TAB;
            col += TV_TAB - col % TV_TAB;
        } else if ( raw[at] == '\n' ) {
            out_len++;
            col = 0;
        } else if ( raw[at] != '\r' ) {
            out_len++;
            col++;
        }
    }
    tv->text = (char *)malloc( (size_t)out_len + 2 );
    if ( tv->text == NULL ) {
        return -1;
    }
    col = 0;
    tv->text_len = 0;
    for ( at = 0; at < got; at++ ) {
        ch = (u8)raw[at];
        if ( ch == '\t' ) {
            for ( pad = TV_TAB - col % TV_TAB; pad > 0; pad-- ) {
                tv->text[tv->text_len++] = ' ';
                col++;
            }
        } else if ( ch == '\n' ) {
            tv->text[tv->text_len++] = '\n';
            col = 0;
        } else if ( ch == '\r' ) {
            /* dropped: CRLF and LF look the same here */
        } else if ( ch < 32 || ch == 127 ) {
            tv->text[tv->text_len++] = '.';
            col++;
        } else {
            tv->text[tv->text_len++] = (char)ch;
            col++;
        }
    }
    tv->text[tv->text_len] = 0;
    tv->line = (char **)malloc( (size_t)TV_MAX_LINES * sizeof( char * ) );
    if ( tv->line == NULL ) {
        free( tv->text );
        tv->text = NULL;
        return -1;
    }
    tv->lines = 0;
    tv->line[tv->lines++] = tv->text;
    for ( at = 0; at < tv->text_len && tv->lines < TV_MAX_LINES; at++ ) {
        if ( tv->text[at] == '\n' ) {
            tv->text[at] = 0;
            tv->line[tv->lines++] = tv->text + at + 1;
        }
    }
    /* the table is full: the last line it holds still ends at its own
       newline, and only what follows that is left out */
    if ( tv->lines >= TV_MAX_LINES ) {
        for ( ; at < tv->text_len && tv->text[at] != '\n'; at++ ) {
        }
        if ( at < tv->text_len ) {
            tv->text[at] = 0;
            if ( at + 1 < tv->text_len ) {
                tv->truncated = 1;
            }
        }
    }
    if ( tv->lines > 1 && tv->line[tv->lines - 1][0] == 0 ) {
        tv->lines--;
    }
    return tv->lines;
}

static int tv_vis_rows( const VIEWER *tv )
{
    int h = tv->box.at.end_row - tv->box.at.row + 1 - 3;

    return h < 1 ? 1 : h;
}

static int tv_vis_cols( const VIEWER *tv )
{
    int w = tv->box.at.end_col - tv->box.at.col + 1;

    if ( w < 1 ) {
        w = 1;
    }
    return w > TV_LINEBUF - 2 ? TV_LINEBUF - 2 : w;
}

static void tv_clamp( VIEWER *tv )
{
    int vis = tv_vis_rows( tv ), max_top = tv->lines > vis ? tv->lines - vis : 0;

    if ( tv->top > max_top ) {
        tv->top = max_top;
    }
    if ( tv->top < 0 ) {
        tv->top = 0;
    }
    if ( tv->left < 0 ) {
        tv->left = 0;
    }
    if ( tv->left > 512 ) {
        tv->left = 512;
    }
}

static void tv_render( VIEWER *tv )
{
    int vis = tv_vis_rows( tv ), iw = tv_vis_cols( tv ), at, idx, len, used, last;
    int row0 = tv->box.at.row, col0 = tv->box.at.col, fg, bg;
    char out[TV_LINEBUF];
    const char *src;

    df_fill( row0 + 1, col0 + 1, tv->box.at.end_row + 1, tv->box.at.end_col + 1, BG_WHITE );

    /* the title, centred on the first row */
    len = (int)strlen( tv->title );
    used = 0;
    for ( at = (iw - len) / 2; at > 0 && used < iw; at-- ) {
        out[used++] = ' ';
    }
    for ( at = 0; tv->title[at] && used < iw; at++ ) {
        out[used++] = tv->title[at];
    }
    while ( used < iw ) {
        out[used++] = ' ';
    }
    out[used] = 0;
    df_text( row0 + 1, col0 + 1, out, FG_BLACK, BG_CYAN );

    for ( at = 0; at < vis; at++ ) {
        idx = tv->top + at;
        src = "";
        if ( idx < tv->lines ) {
            len = (int)strlen( tv->line[idx] );
            src = tv->left < len ? tv->line[idx] + tv->left : "";
        }
        for ( used = 0; src[used] && used < iw; used++ ) {
            out[used] = src[used];
        }
        while ( used < iw ) {
            out[used++] = ' ';
        }
        out[iw] = 0;
        fg = FG_BLACK;
        bg = BG_WHITE;
        if ( idx == tv->hit && idx < tv->lines ) {
            bg = BG_CYAN;
        }
        df_text( row0 + 2 + at, col0 + 1, out, fg, bg );
    }

    last = tv->top + vis;
    if ( last > tv->lines ) {
        last = tv->lines;
    }
    sprintf( out, " Lines %d-%d of %d   F=Find  F3=Again%s%s",
             tv->lines ? tv->top + 1 : 0, last, tv->lines,
             tv->truncated ? "   (TRUNCATED - too big to show it all)" : "",
             tv->not_found ? "   NOT FOUND" : (tv->left ? "   >>" : "") );
    for ( used = (int)strlen( out ); used < iw; used++ ) {
        out[used] = ' ';
    }
    out[iw] = 0;
    df_text( row0 + 2 + vis, col0 + 1, out, FG_YELLOW, BG_WHITE );
    button_draw( &tv->close );
}

static const char *find_nocase( const char *hay, const char *needle )
{
    int len = (int)strlen( needle ), at, k;

    if ( len == 0 ) {
        return hay;
    }
    for ( at = 0; hay[at]; at++ ) {
        for ( k = 0; k < len; k++ ) {
            if ( !hay[at + k] ) {
                return NULL;
            }
            if ( toupper( (u8)hay[at + k] ) != toupper( (u8)needle[k] ) ) {
                break;
            }
        }
        if ( k == len ) {
            return hay + at;
        }
    }
    return NULL;
}

static int tv_find_from( VIEWER *tv, int start )
{
    int at;

    if ( !tv->find[0] || tv->lines < 1 ) {
        return 0;
    }
    if ( start < 0 ) {
        start = 0;
    }
    for ( at = start; at < tv->lines; at++ ) {
        if ( find_nocase( tv->line[at], tv->find ) ) {
            tv->hit = at;
            tv->top = at - tv_vis_rows( tv ) / 3;
            tv_clamp( tv );
            return 1;
        }
    }
    return 0;
}

static void tv_show( VIEWER *tv )
{
    int rows = df_rows(), cols = df_cols(), vis, before, before_left, oc;
    char find[TF_TEXT_MAX];
    EVENT ev;

    popup_init( &tv->box, FG_BLACK, BG_WHITE, tv->title );
    tv->box.border = BORDER_NONE;
    tv->box.shadow = 0;
    popup_place( &tv->box, 0, 0, rows - 1, cols - 1 );
    popup_draw( &tv->box );
    oc = (cols - 9) / 2;
    button_init( &tv->close, FG_WHITE, BG_CYAN, "Close" );
    button_row( &tv->close, rows - 1, oc < 0 ? 0 : oc );
    tv_clamp( tv );
    tv_render( tv );
    vis = tv_vis_rows( tv );

    for ( ;; ) {
        ev_wait( &ev );
        if ( ev.kind == EV_MOUSE ) {
            if ( (ev.press & LEFT_BUTTON) && button_hit( &tv->close, ev.x, ev.y ) ) {
                break;
            }
            continue;
        }
        if ( ev.key == KEY_ESC || ev.key == KEY_ENTER ) {
            break;
        }
        if ( ev.key == 'f' || ev.key == 'F' ) {
            strcpy( find, tv->find );
            if ( input_box( FG_BLUE, BG_WHITE, "Find", "Text to look for:", find,
                            TV_FIND_MAX - 1, 0 ) == IB_OK ) {
                strncpy( tv->find, find, sizeof( tv->find ) - 1 );
                tv->find[sizeof( tv->find ) - 1] = 0;
                tv->hit = -1;
                tv->not_found = tv_find_from( tv, 0 ) ? 0 : 1;
            }
            tv_render( tv );
            continue;
        }
        before = tv->top;
        before_left = tv->left;
        switch ( ev.key ) {
        case KEY_UP:    tv->top--;              break;
        case KEY_DOWN:  tv->top++;              break;
        case KEY_PGUP:  tv->top -= vis;         break;
        case KEY_PGDN:  tv->top += vis;         break;
        case KEY_HOME:  tv->top = 0; tv->left = 0; break;
        case KEY_END:   tv->top = tv->lines;    break;
        case KEY_LEFT:  tv->left -= 8;          break;
        case KEY_RIGHT: tv->left += 8;          break;
        case KEY_F3:
            if ( tv->find[0] ) {
                tv->not_found = (tv_find_from( tv, tv->hit + 1 ) || tv_find_from( tv, 0 )) ? 0 : 1;
            }
            tv_render( tv );
            continue;
        default:
            continue;
        }
        tv_clamp( tv );
        if ( tv->top != before || tv->left != before_left ) {
            tv_render( tv );
        }
    }
    popup_restore( &tv->box );
    popup_free( &tv->box );
}

static void tv_init( VIEWER *tv, const char *title )
{
    memset( tv, 0, sizeof( *tv ) );
    tv->hit = -1;
    strncpy( tv->title, title ? title : "", sizeof( tv->title ) - 1 );
}

static void tv_free( VIEWER *tv )
{
    free( tv->text );
    free( tv->line );
}

int view_file( const char *path, const char *title )
{
    VIEWER tv;
    FILE *fp;
    char *raw;
    long got;
    int lines;

    tv_init( &tv, title );
    fp = fopen( path, "rb" );
    if ( fp == NULL ) {
        return -1;
    }
    raw = (char *)malloc( (size_t)TV_MAX_BYTES + 1 );
    if ( raw == NULL ) {
        fclose( fp );
        return -1;
    }
    got = (long)fread( raw, 1, (size_t)TV_MAX_BYTES, fp );
    raw[got] = 0;
    lines = tv_index( &tv, raw, got, fgetc( fp ) != EOF );
    fclose( fp );
    free( raw );
    if ( lines >= 0 ) {
        tv_show( &tv );
    }
    tv_free( &tv );
    return lines;
}

int view_text( const char *text, const char *title )
{
    VIEWER tv;
    long len;
    int lines;

    if ( text == NULL ) {
        return -1;
    }
    tv_init( &tv, title );
    len = (long)strlen( text );
    lines = tv_index( &tv, text, len > TV_MAX_BYTES ? TV_MAX_BYTES : len, len > TV_MAX_BYTES );
    if ( lines >= 0 ) {
        tv_show( &tv );
    }
    tv_free( &tv );
    return lines;
}

/* ------------------------------------------------------------------ */
/* the title and status lines (TITLEBAR.CPP, STATBAR.CPP)              */
/* ------------------------------------------------------------------ */

static char title_text[35] = "XArchive";
static char status_text[160];

void title_set( const char *text )
{
    strncpy( title_text, text, sizeof( title_text ) - 1 );
    title_text[sizeof( title_text ) - 1] = 0;
}

/* row 1, centred, and the close box in the corner: "[-]" with the box
   line's dash, which is what the toolkit drew when there was a mouse
   object at all - and XArc always made one */
void title_draw( void )
{
    char box[4];
    int len = (int)strlen( title_text ), cols = df_cols();

    df_clear_row( 1, FG_BLACK, BG_CYAN );
    if ( title_text[0] ) {
        df_text( 1, len < cols ? (cols - len) / 2 + 1 : 1, title_text, FG_BLACK, BG_CYAN );
    }
    box[0] = '[';
    box[1] = (char)196;
    box[2] = ']';
    box[3] = 0;
    df_text( 1, 1, box, FG_BLACK, BG_CYAN );
}

/* clipped to the width less three, as the toolkit's was */
void status_set( const char *text )
{
    int room = df_cols() - 3;

    if ( room > (int)sizeof( status_text ) - 1 ) {
        room = (int)sizeof( status_text ) - 1;
    }
    if ( room < 1 ) {
        room = 1;
    }
    status_text[0] = 0;
    if ( text ) {
        strncpy( status_text, text, (size_t)room );
        status_text[room] = 0;
    }
    status_draw();
}

void status_draw( void )
{
    df_clear_row( df_rows(), FG_BLACK, BG_CYAN );
    if ( status_text[0] ) {
        df_text( df_rows(), 1, status_text, FG_BLACK, BG_CYAN );
    }
}

/* ------------------------------------------------------------------ */
/* a disk that is not there (CRITERR.CPP)                              */
/* ------------------------------------------------------------------ */

#define CE_WRITE        0x01
#define CE_CHARDEV      0x80

void crit_install( void )
{
    REGS regs;

    xa_clear( &regs );
    regs.edx = (u32)crit_handler;
    xa_dos( &regs, 0x2524 );
}

void crit_clear( void )
{
    crit_hits = 0;
    crit_code = 0;
    crit_drive = 0;
    crit_flags = 0;
}

int crit_hit( void )
{
    return (int)crit_hits;
}

void crit_text( char *buf, int size )
{
    char line[80];
    char letter = (char)('A' + crit_drive);
    int code = crit_code & 0xFF;

    if ( size <= 0 ) {
        return;
    }
    if ( crit_flags & CE_CHARDEV ) {
        strcpy( line, "Device error." );
    } else if ( code == 0x02 ) {
        sprintf( line, "No disk in drive %c:.", letter );
    } else if ( code == 0x00 ) {
        sprintf( line, "The disk in drive %c: is write-protected.", letter );
    } else {
        sprintf( line, "Error %s drive %c:.", (crit_flags & CE_WRITE) ? "writing" : "reading",
                 letter );
    }
    strncpy( buf, line, (size_t)size - 1 );
    buf[size - 1] = 0;
}

/* Retry or Cancel.  Retry has the kernel forget its buffers first (0Dh),
   or what it kept of the sector it could not read would be read again. */
int crit_ask( void )
{
    static const char *const labels[] = { "Retry", "Cancel" };
    REGS regs;
    char text[80];

    crit_text( text, sizeof( text ) );
    if ( msg_choose( FG_BLACK, BG_WHITE, "Error", text, labels, 2 ) != 0 ) {
        return 0;
    }
    xa_clear( &regs );
    xa_dos( &regs, 0x0D00 );
    return 1;
}
