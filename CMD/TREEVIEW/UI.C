/*
 * UI.C - the DOSForms toolkit's widgets that TreeView uses, as C: a box
 * that puts back what it covered (POPUP), buttons, the message box that
 * sizes itself to its text, the title and status lines, and the question
 * asked when a disk is not there.  XARC's UI.C, less the widgets
 * TreeView has no use for.
 *
 * Each is the toolkit's class of the same name done over (DOSFORMS.CPP,
 * POPUP.CPP, BUTTON.CPP, DLGBOX.CPP, TITLEBAR.CPP, STATBAR.CPP,
 * CRITERR.CPP) with the same sizes, places, colours and keys, so that
 * the screens are the DOS program's.  What changed is underneath: a box
 * waits in ev_wait for an event instead of spinning on kbhit and the
 * mouse driver, and a click is an event SCR.C has already found the
 * edge of, so none of the toolkit's "seed the edge detector" care is
 * needed here.
 */
#include "treeview.h"

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

/* The box, sized to its text and centred, or where "place" puts it
   (DiskStat's About box was given its rectangle); the text in it, as
   lines centred one by one; buttons on the bottom row; and the answer -
   Enter the first button, Esc the last. */
static int dlg_run( int fg, int bg, const char *title, const char *text,
                    const char *const *labels, int count, const RECT *place )
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
    if ( place ) {
        popup_place( &box, place->row, place->col, place->end_row, place->end_col );
    } else {
        popup_place( &box, top, left, top + h - 1, left + w - 1 );
    }
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
        len = (int)strlen( dlg_line[line] );
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

int msg_ok( int fg, int bg, const char *title, const char *text, const RECT *at )
{
    static const char *const ok[] = { "OK" };

    dlg_run( fg, bg, title, text, ok, 1, at );
    return 0;
}

int msg_choose( int fg, int bg, const char *title, const char *text,
                const char *const *labels, int count )
{
    if ( count > DLG_MAX_BUTTONS ) {
        count = DLG_MAX_BUTTONS;
    }
    return dlg_run( fg, bg, title, text, labels, count, NULL );
}

/* ------------------------------------------------------------------ */
/* the title and status lines (TITLEBAR.CPP, STATBAR.CPP)              */
/* ------------------------------------------------------------------ */

static char title_text[35] = APP_NAME;
static char status_text[160];

void title_set( const char *text )
{
    strncpy( title_text, text, sizeof( title_text ) - 1 );
    title_text[sizeof( title_text ) - 1] = 0;
}

/* row 1, centred, and the close box in the corner: "[-]" with the box
   line's dash, which is what the toolkit drew when there was a mouse
   object at all - and DiskStat always made one */
void title_draw( void )
{
    char box[4];
    int len = (int)strlen( title_text ), cols = df_cols();

    df_clear_row( 1, FG_WHITE, BG_BLUE );
    if ( title_text[0] ) {
        df_text( 1, len < cols ? (cols - len) / 2 + 1 : 1, title_text, FG_WHITE, BG_BLUE );
    }
    box[0] = '[';
    box[1] = (char)196;
    box[2] = ']';
    box[3] = 0;
    df_text( 1, 1, box, FG_WHITE, BG_BLUE );
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
    df_clear_row( df_rows(), FG_LIGHT_CYAN, BG_CYAN );
    if ( status_text[0] ) {
        df_text( df_rows(), 1, status_text, FG_LIGHT_CYAN, BG_CYAN );
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
