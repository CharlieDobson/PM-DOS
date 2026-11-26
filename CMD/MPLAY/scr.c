/*
 * SCR.C - the screen in memory, the display modes, the panes, the
 * keyboard and the mouse.  DISP.H is what the application sees of it.
 *
 * EVERYTHING IS DRAWN INTO MEMORY FIRST, as EDIT and DOSSHELL draw.
 * The whole screen - menu bar, status bar, whatever dialogs are open -
 * is put into "want" on every turn of the loop, and scr_flush() sends
 * only the cells that differ from "shown".  Nothing ever has to
 * remember what was under a dialog.
 *
 * ONE SCREEN, TWO WAYS TO SHOW IT.  On a text screen a run of changed
 * cells goes to the kernel (21h/F0h AL=4) and the kernel's mouse driver
 * keeps the pointer; on a graphics screen each changed cell is painted
 * into GFX.C's picture from the ROM's font, and GFX.C keeps the
 * pointer, the cursor and the caret.
 *
 * A PANE IS A HOLE IN THE CELLS.  Its cells carry CELLF_PANE and the
 * pane's number and nothing else, so the diff still works: a cell that
 * was a dialog's and is the pane's again differs, and that is how the
 * pane learns which of its pixels were uncovered.  scr_flush gathers
 * those cells into rectangles, adds the rectangles the application
 * said had changed (pane_damage), and has the pane paint each one with
 * the clip set to it.  A paint may run over cells that lie on top of
 * the pane - a dialog while the page behind it is redrawn - so every
 * such cell inside a painted rectangle is marked not shown and painted
 * again afterwards.  It all happens in the picture; the screen sees
 * the result once.
 *
 * THE KEYBOARD knows what the kernel's key calls do not say: that Alt
 * was pressed and let go with no key in between, which opens the menu
 * bar (21h/F0h AL=8 comes back when the shift keys change, and the
 * change is watched here); and which key a control character came
 * from.  The BIOS gives Ctrl+I and Tab the same character, and a word
 * processor needs both, so the scan code is looked at before it is
 * thrown away - translate() has the rules.
 *
 * THE MOUSE arrives as K_MOUSE with "mouse" saying what happened, in
 * pixels and in cells, for both buttons - see mouse_poll.
 *
 * A WAIT WITH A TIME LIMIT IS A LOOP.  The kernel's wait has none, so
 * key_get_timed looks, and looks again; and so does key_get while a
 * caret is blinking.  caret_blink( 0 ) is how a program on a battery
 * gets its sleep back.
 *
 * TEST MODE: with PWTEST naming a log file and standard input taken
 * from a file, the keys come from the file.  DISP.H lists the records.
 */
#include "dispint.h"

#define MAX_MODES       12
#define MAX_DAMAGE      8       /* rectangles a pane keeps between flushes */
#define BLINK_TIME      50      /* hundredths the caret stays on, and off */

typedef struct {
    int x0, y0, x1, y1;
} PXRECT;

typedef struct {
    PANEPAINT paint;
    int    row, col, rows, cols;        /* where it was placed */
    int    on;                          /* it is on the screen */
    int    shown_row, shown_col, shown_rows, shown_cols;    /* ...and where it was painted */
    int    pointer;                     /* MP_* over it */
    PXRECT damage[MAX_DAMAGE];
    int    ndamage;
} PANE;

int scr_rows = 25, scr_cols = 80;
int gfx_on;

static DISPMODE modes[MAX_MODES];
static int  nmodes;
static int  mode_now;
static int  last_error;
static int  found_entry = -1;   /* the entry for a screen found that is none of ours */

static CELL     *want;           /* the screen as it should be */
static CELL     *shown;          /* the screen as it was last given */
static int       hw_rows = 25, hw_cols = 80;    /* the text screen as the kernel has it */
static int       dos_rows, dos_cols;            /* ...and as it was found */
static int       dos_row, dos_col;              /* the cursor then */
static uint16_t *dos_cells;                     /* what was on it */
static int       lines_changed;  /* sys_set_lines was used: the screen found is gone */
static int       cur_row, cur_col, cur_kind = CUR_HIDE;
static int       shown_row = -1, shown_col = -1, shown_kind = -1;
static int       desk_color = 7;

static PANE     panes[MAX_PANES];
static int      car_x, car_y, car_h;    /* the pane caret the application wants; car_h 0 for none */
static int      car_blinks = 1;
static int      car_phase = 1;          /* it is in the half of its blink that shows */
static int      car_showing;            /* it was wanted and uncovered at the last flush */
static uint32_t car_time;

static void mouse_start( void );
static void mouse_stop( void );
static void pointer_show( void );
static void key_start( void );

/* ------------------------------------------------------------------ */
/* small things                                                        */
/* ------------------------------------------------------------------ */

/* as much of "src" as "max" bytes hold, with the NUL */
static void fit( char *dst, const char *src, unsigned int max )
{
    unsigned int len = str_len( src );

    if ( max == 0 ) {
        return;
    }
    if ( len > max - 1 ) {
        len = max - 1;
    }
    mem_cpy( dst, src, len );
    dst[len] = 0;
}

/* hundredths from "then" to "now", across midnight */
static uint32_t since( uint32_t then, uint32_t now )
{
    return now >= then ? now - then : now + 8640000UL - then;
}

static void forget_shown( void )
{
    int index;

    mem_set( want, 0, (unsigned int)MAX_ROWS * MAX_COLS * sizeof( CELL ) );
    mem_set( shown, 0xFF, (unsigned int)MAX_ROWS * MAX_COLS * sizeof( CELL ) );
    shown_row = shown_col = shown_kind = -1;
    for ( index = 0; index < MAX_PANES; index++ ) {
        panes[index].on = 0;
        panes[index].ndamage = 0;
        panes[index].shown_rows = 0;
    }
    car_h = 0;
    car_showing = 0;
}

/* ------------------------------------------------------------------ */
/* the modes                                                           */
/* ------------------------------------------------------------------ */

static void text_entry( int rows, int cols, int found )
{
    DISPMODE *mode;

    if ( nmodes >= MAX_MODES ) {
        return;
    }
    mode = &modes[nmodes];
    mem_set( mode, 0, sizeof( *mode ) );
    mode->backend = DB_TEXT;
    mode->rows = (uint16_t)rows;
    mode->cols = (uint16_t)cols;
    mode->colors = 16;
    if ( found ) {
        sfmt( mode->name, sizeof( mode->name ), "Text, %d lines of %d", rows, cols );
        found_entry = nmodes;
    } else {
        sfmt( mode->name, sizeof( mode->name ), "Text, %d lines", rows );
    }
    nmodes++;
}

static void drop_entry( int index )
{
    if ( index < 0 || index >= nmodes ) {
        return;
    }
    mem_cpy( &modes[index], &modes[index + 1], (unsigned int)(nmodes - index - 1) * sizeof( DISPMODE ) );
    nmodes--;
    if ( found_entry == index ) {
        found_entry = -1;
    } else if ( found_entry > index ) {
        found_entry--;
    }
    if ( mode_now > index ) {
        mode_now--;
    }
}

int disp_mode_count( void )
{
    return nmodes;
}

const DISPMODE *disp_mode_info( int index )
{
    return index >= 0 && index < nmodes ? &modes[index] : NULL;
}

int disp_mode_now( void )
{
    return mode_now;
}

int disp_error( void )
{
    return last_error;
}

int disp_mode_find( int width, int height )
{
    int index;

    for ( index = 0; index < nmodes; index++ ) {
        if ( width == 0 ) {
            if ( modes[index].backend == DB_TEXT && modes[index].rows == height
                 && modes[index].cols == 80 ) {
                return index;
            }
        } else if ( modes[index].width == width && modes[index].height == height ) {
            return index;
        }
    }
    return -1;
}

static void adopt_text( int rows, int cols )
{
    hw_rows = rows;
    hw_cols = cols;
    scr_rows = rows > MAX_ROWS ? MAX_ROWS : rows;
    scr_cols = cols > MAX_COLS ? MAX_COLS : cols;
}

/* A text screen of that many lines.  When the screen already has them
   nothing is set, and so nothing flickers.  The screen the program
   was started on can be gone back to only while no line count has
   been set: no call sets a text mode the kernel was not asked for. */
static int enter_text( const DISPMODE *mode )
{
    int rows, cols;

    gfx_leave();
    gfx_on = 0;
    sys_screen_size( &rows, &cols );
    if ( rows != mode->rows || cols != mode->cols ) {
        if ( mode->cols != 80 ) {
            return DSE_REFUSED;
        }
        sys_set_lines( mode->rows );
        lines_changed = 1;
        sys_screen_size( &rows, &cols );
        if ( rows != mode->rows || cols != mode->cols ) {
            return DSE_REFUSED;
        }
    }
    adopt_text( rows, cols );
    return DSE_NONE;
}

static int enter_mode( int index )
{
    const DISPMODE *mode = &modes[index];
    int err;

    if ( mode->backend == DB_TEXT ) {
        return enter_text( mode );
    }
    err = gfx_enter( mode );
    if ( err != DSE_NONE ) {
        return err;
    }
    gfx_on = 1;
    scr_rows = mode->rows;
    scr_cols = mode->cols;
    return DSE_NONE;
}

/* a mode is on the screen: nothing of the old one is left */
static void mode_set_done( int index )
{
    mode_now = index;
    forget_shown();
    if ( gfx_on ) {
        gfx_strip( desk_color );
    }
    mouse_start();
}

/* the entry that is the same screen as "mode", or -1: entries move
   when one is dropped */
static int same_entry( const DISPMODE *mode )
{
    int index;

    for ( index = 0; index < nmodes; index++ ) {
        if ( modes[index].backend == mode->backend && modes[index].width == mode->width
             && modes[index].height == mode->height && modes[index].rows == mode->rows
             && modes[index].cols == mode->cols && modes[index].vmode == mode->vmode ) {
            return index;
        }
    }
    return -1;
}

int disp_set_mode( int index )
{
    DISPMODE asked, before;
    int err, other, width;

    last_error = DSE_NONE;
    if ( index < 0 || index >= nmodes ) {
        last_error = DSE_REFUSED;
        return 0;
    }
    asked = modes[index];
    before = modes[mode_now];
    mouse_stop();
    err = enter_mode( index );
    if ( err == DSE_NONE ) {
        mode_set_done( index );
        return 1;
    }
    last_error = err;
    if ( err == DSE_REFUSED && index != 0 ) {
        drop_entry( index );            /* the BIOS has not got it after all */
    }
    if ( lines_changed && found_entry >= 0 ) {
        drop_entry( found_entry );      /* ...and there is no way back to this one */
    }
    /* THE NEXT BEST: the screen from before, then each graphics screen
       smaller than the one asked for, the largest first, then text. */
    other = same_entry( &before );
    if ( other >= 0 && enter_mode( other ) == DSE_NONE ) {
        mode_set_done( other );
        return 0;
    }
    width = asked.width;
    for ( other = nmodes - 1; other > 0; other-- ) {
        if ( modes[other].backend == DB_TEXT || modes[other].width >= width ) {
            continue;
        }
        width = modes[other].width;
        err = enter_mode( other );
        if ( err == DSE_NONE ) {
            mode_set_done( other );
            return 0;
        }
        if ( err == DSE_REFUSED ) {
            drop_entry( other );
        }
    }
    if ( enter_mode( 0 ) != DSE_NONE ) {
        /* 25 lines of text refused: whatever text screen there is */
        gfx_leave();
        gfx_on = 0;
        sys_screen_size( &err, &other );
        adopt_text( err, other );
    }
    mode_set_done( 0 );
    return 0;
}

void disp_init( void )
{
    int rows, cols;
    unsigned int count;

    sys_screen_size( &rows, &cols );
    dos_rows = rows;
    dos_cols = cols;
    sys_get_cursor( &dos_row, &dos_col );
    count = (unsigned int)rows * (unsigned int)cols;
    dos_cells = (uint16_t *)try_alloc( count * 2 );
    if ( dos_cells != NULL ) {
        sys_get_cells( 0, dos_cells, count );
    }
    want = (CELL *)xalloc( (unsigned int)MAX_ROWS * MAX_COLS * sizeof( CELL ) );
    shown = (CELL *)xalloc( (unsigned int)MAX_ROWS * MAX_COLS * sizeof( CELL ) );

    nmodes = 0;
    text_entry( 25, 80, 0 );
    text_entry( 43, 80, 0 );
    text_entry( 50, 80, 0 );
    mode_now = disp_mode_find( 0, rows );
    if ( mode_now < 0 || cols != 80 ) {
        mode_now = nmodes;
        text_entry( rows, cols, 1 );
    }
    nmodes += gfx_probe( modes + nmodes, MAX_MODES - nmodes );

    adopt_text( rows, cols );
    forget_shown();
    key_start();
    mouse_start();
}

/* the screen as DOS programs expect to find it: text, the lines the
   program was started on, the cursor a line */
static void dos_screen( void )
{
    int rows, cols;

    mouse_stop();
    gfx_leave();
    gfx_on = 0;
    sys_screen_size( &rows, &cols );
    if ( rows != dos_rows && dos_cols == 80 ) {
        sys_set_lines( dos_rows );
        lines_changed = 1;
    }
    sys_cursor_shape( CUR_LINE );
}

void disp_done( void )
{
    int rows, cols;

    dos_screen();
    sys_screen_size( &rows, &cols );
    if ( rows == dos_rows && cols == dos_cols && dos_cells != NULL ) {
        sys_put_cells( 0, dos_cells, (unsigned int)rows * (unsigned int)cols );
        sys_cursor( dos_row, dos_col );
    } else {
        sys_cls();
        sys_cursor( 0, 0 );
    }
}

void scr_suspend( void )
{
    dos_screen();
    sys_cls();
    sys_cursor( 0, 0 );
}

void scr_resume( void )
{
    disp_set_mode( mode_now );
}

void scr_repaint( void )
{
    int index;

    mem_set( shown, 0xFF, (unsigned int)MAX_ROWS * MAX_COLS * sizeof( CELL ) );
    shown_row = shown_col = shown_kind = -1;
    for ( index = 0; index < MAX_PANES; index++ ) {
        panes[index].ndamage = 0;       /* every cell of it will be uncovered */
    }
}

void scr_desk( int color )
{
    desk_color = color & 15;
    if ( gfx_on ) {
        gfx_strip( desk_color );
    }
}

/* ------------------------------------------------------------------ */
/* drawing cells                                                       */
/* ------------------------------------------------------------------ */

void scr_ch( int row, int col, int ch, uint8_t attr )
{
    if ( row >= 0 && row < scr_rows && col >= 0 && col < scr_cols ) {
        want[row * MAX_COLS + col] = ((uint32_t)attr << 8) | (uint32_t)(ch & 0xFF);
    }
}

void scr_putn( int row, int col, const char *text, int len, uint8_t attr )
{
    int pos;

    for ( pos = 0; pos < len; pos++ ) {
        scr_ch( row, col + pos, (uint8_t)text[pos], attr );
    }
}

void scr_put( int row, int col, const char *text, uint8_t attr )
{
    scr_putn( row, col, text, (int)str_len( text ), attr );
}

/* exactly "width" cells: the text, cut short or padded with blanks */
void scr_putw( int row, int col, const char *text, int width, uint8_t attr )
{
    int len = (int)str_len( text );

    if ( len > width ) {
        len = width;
    }
    scr_putn( row, col, text, len, attr );
    scr_fill( row, col + len, width - len, ' ', attr );
}

/* "&File": the character after '&' in hot_attr - and underlined, on a
   graphics screen - with the '&' not shown.  hot_attr the same as attr
   is how a caller says the access keys are not being shown. */
void scr_hot( int row, int col, const char *text, uint8_t attr, uint8_t hot_attr )
{
    int hot = 0;

    for ( ; *text; text++ ) {
        if ( *text == '&' && !hot ) {
            hot = 1;
            continue;
        }
        if ( hot == 1 ) {
            scr_ch( row, col, (uint8_t)*text, hot_attr );
            if ( gfx_on && hot_attr != attr ) {
                scr_flag( row, col, 1, CELLF_UNDER );
            }
            hot = 2;
        } else {
            scr_ch( row, col, (uint8_t)*text, attr );
        }
        col++;
    }
}

int scr_hotlen( const char *text )
{
    int len = 0;

    for ( ; *text; text++ ) {
        if ( *text != '&' ) {
            len++;
        }
    }
    return len;
}

int scr_hotkey( const char *text )
{
    const char *amp = str_chr( text, '&' );

    return amp && amp[1] ? ch_upper( amp[1] ) : 0;
}

void scr_fill( int row, int col, int len, int ch, uint8_t attr )
{
    int pos;

    for ( pos = 0; pos < len; pos++ ) {
        scr_ch( row, col + pos, ch, attr );
    }
}

/* a pane's cell has no character to keep: it becomes a blank */
void scr_recolor( int row, int col, int len, uint8_t attr )
{
    int pos;
    CELL *cell;

    if ( row < 0 || row >= scr_rows ) {
        return;
    }
    for ( pos = col; pos < col + len; pos++ ) {
        if ( pos >= 0 && pos < scr_cols ) {
            cell = &want[row * MAX_COLS + pos];
            if ( *cell & CELLF_PANE ) {
                *cell = ((uint32_t)attr << 8) | ' ';
            } else {
                *cell = (*cell & ~0xFF00UL) | ((uint32_t)attr << 8);
            }
        }
    }
}

void scr_flag( int row, int col, int len, uint32_t flags )
{
    int pos;

    flags &= CELLF_TOP | CELLF_BOTTOM | CELLF_LEFT | CELLF_RIGHT | CELLF_UNDER | CELLF_RAISE | CELLF_SINK;
    if ( row < 0 || row >= scr_rows || !gfx_on ) {
        return;
    }
    for ( pos = col; pos < col + len; pos++ ) {
        if ( pos >= 0 && pos < scr_cols && !(want[row * MAX_COLS + pos] & CELLF_PANE) ) {
            want[row * MAX_COLS + pos] |= flags;
        }
    }
}

void scr_box( int top, int left, int bottom, int right, uint8_t attr )
{
    int row;

    scr_ch( top, left, 0xDA, attr );
    scr_fill( top, left + 1, right - left - 1, 0xC4, attr );
    scr_ch( top, right, 0xBF, attr );
    for ( row = top + 1; row < bottom; row++ ) {
        scr_ch( row, left, 0xB3, attr );
        scr_fill( row, left + 1, right - left - 1, ' ', attr );
        scr_ch( row, right, 0xB3, attr );
    }
    scr_ch( bottom, left, 0xC0, attr );
    scr_fill( bottom, left + 1, right - left - 1, 0xC4, attr );
    scr_ch( bottom, right, 0xD9, attr );
}

/* the shadow a box casts: two columns to its right, one row below.
   What is under it keeps its characters and loses its colours; a
   pane's pixels cannot be dimmed, so there the shadow is plain. */
void scr_shadow( int top, int left, int bottom, int right, uint8_t attr )
{
    int row;

    for ( row = top + 1; row <= bottom + 1; row++ ) {
        if ( row == bottom + 1 ) {
            scr_recolor( row, left + 2, right - left + 1, attr );
        } else {
            scr_recolor( row, right + 1, 2, attr );
        }
    }
}

void scr_cursor( int row, int col, int kind )
{
    cur_row = row;
    cur_col = col;
    cur_kind = kind;
}

uint32_t scr_cell( int row, int col )
{
    CELL cell;

    if ( row < 0 || row >= scr_rows || col < 0 || col >= scr_cols ) {
        return 0;
    }
    cell = want[row * MAX_COLS + col];
    return (cell & CELLF_PANE) ? ' ' : (cell & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* panes                                                               */
/* ------------------------------------------------------------------ */

void pane_define( int pane, PANEPAINT paint )
{
    if ( pane >= 0 && pane < MAX_PANES ) {
        panes[pane].paint = paint;
        panes[pane].pointer = MP_ARROW;
    }
}

void pane_pointer( int pane, int shape )
{
    if ( pane >= 0 && pane < MAX_PANES ) {
        panes[pane].pointer = shape;
    }
}

int pane_place( int pane, int row, int col, int rows, int cols )
{
    PANE *pn;
    int at_row, at_col;

    if ( !gfx_on || pane < 0 || pane >= MAX_PANES ) {
        return 0;
    }
    if ( row < 0 ) {
        rows += row;
        row = 0;
    }
    if ( col < 0 ) {
        cols += col;
        col = 0;
    }
    if ( row + rows > scr_rows ) {
        rows = scr_rows - row;
    }
    if ( col + cols > scr_cols ) {
        cols = scr_cols - col;
    }
    pn = &panes[pane];
    if ( rows <= 0 || cols <= 0 ) {
        pn->on = 0;
        return 1;
    }
    pn->row = row;
    pn->col = col;
    pn->rows = rows;
    pn->cols = cols;
    pn->on = 1;
    for ( at_row = row; at_row < row + rows; at_row++ ) {
        for ( at_col = col; at_col < col + cols; at_col++ ) {
            want[at_row * MAX_COLS + at_col] = CELLF_PANE | (uint32_t)pane;
        }
    }
    return 1;
}

int pane_rect( int pane, int *x, int *y, int *w, int *h )
{
    PANE *pn;

    if ( !gfx_on || pane < 0 || pane >= MAX_PANES || !panes[pane].on ) {
        *x = *y = *w = *h = 0;
        return 0;
    }
    pn = &panes[pane];
    *x = pn->col * CELL_W;
    *y = pn->row * CELL_H;
    *w = pn->cols * CELL_W;
    *h = pn->rows * CELL_H;
    return 1;
}

int pane_at( int x, int y )
{
    CELL cell;

    if ( !gfx_on || x < 0 || y < 0 || x / CELL_W >= scr_cols || y / CELL_H >= scr_rows ) {
        return -1;
    }
    cell = want[(y / CELL_H) * MAX_COLS + x / CELL_W];
    return (cell & CELLF_PANE) ? (int)(cell & 0xFF) : -1;
}

/* One more rectangle a pane has to paint.  Rows of uncovered cells
   arrive one at a time, so a rectangle with the same left and right
   edges that touches this one grows; when the list is full it all
   becomes one rectangle round the lot. */
static void damage_add( PANE *pn, int x0, int y0, int x1, int y1 )
{
    PXRECT *rect;
    int index;

    if ( x0 > x1 || y0 > y1 ) {
        return;
    }
    for ( index = 0; index < pn->ndamage; index++ ) {
        rect = &pn->damage[index];
        if ( x0 >= rect->x0 && x1 <= rect->x1 && y0 >= rect->y0 && y1 <= rect->y1 ) {
            return;
        }
        if ( rect->x0 == x0 && rect->x1 == x1 && y0 <= rect->y1 + 1 && y1 >= rect->y0 - 1 ) {
            if ( y0 < rect->y0 ) {
                rect->y0 = y0;
            }
            if ( y1 > rect->y1 ) {
                rect->y1 = y1;
            }
            return;
        }
    }
    if ( pn->ndamage == MAX_DAMAGE ) {
        rect = &pn->damage[0];
        for ( index = 1; index < MAX_DAMAGE; index++ ) {
            if ( pn->damage[index].x0 < rect->x0 ) {
                rect->x0 = pn->damage[index].x0;
            }
            if ( pn->damage[index].y0 < rect->y0 ) {
                rect->y0 = pn->damage[index].y0;
            }
            if ( pn->damage[index].x1 > rect->x1 ) {
                rect->x1 = pn->damage[index].x1;
            }
            if ( pn->damage[index].y1 > rect->y1 ) {
                rect->y1 = pn->damage[index].y1;
            }
        }
        pn->ndamage = 1;
        damage_add( pn, x0, y0, x1, y1 );
        if ( pn->ndamage == 2 ) {       /* it did not join: round both, then */
            if ( x0 < rect->x0 ) {
                rect->x0 = x0;
            }
            if ( y0 < rect->y0 ) {
                rect->y0 = y0;
            }
            if ( x1 > rect->x1 ) {
                rect->x1 = x1;
            }
            if ( y1 > rect->y1 ) {
                rect->y1 = y1;
            }
            pn->ndamage = 1;
        }
        return;
    }
    rect = &pn->damage[pn->ndamage++];
    rect->x0 = x0;
    rect->y0 = y0;
    rect->x1 = x1;
    rect->y1 = y1;
}

void pane_damage( int pane, int x0, int y0, int x1, int y1 )
{
    if ( gfx_on && pane >= 0 && pane < MAX_PANES ) {
        damage_add( &panes[pane], x0, y0, x1, y1 );
    }
}

void pane_damage_all( int pane )
{
    if ( gfx_on && pane >= 0 && pane < MAX_PANES ) {
        panes[pane].ndamage = 0;
        damage_add( &panes[pane], 0, 0, MAX_COLS * CELL_W, MAX_ROWS * CELL_H );
    }
}

/* the panes' part of a flush: which cells were uncovered, and then
   every pane paints what it has to */
static void flush_panes( void )
{
    PANE *pn;
    PXRECT rect;
    CELL cell, mine;
    int row, col, start, pane, index, left, top, right, bottom;
    int seen[MAX_PANES];

    for ( pane = 0; pane < MAX_PANES; pane++ ) {
        seen[pane] = 0;
    }
    for ( row = 0; row < scr_rows; row++ ) {
        start = -1;
        mine = 0;
        for ( col = 0; col <= scr_cols; col++ ) {
            cell = col < scr_cols ? want[row * MAX_COLS + col] : 0;
            if ( (cell & CELLF_PANE) && (cell & 0xFF) < MAX_PANES ) {
                seen[cell & 0xFF] = 1;
            }
            /* a run of one pane's cells that are not on the screen ends here? */
            if ( start >= 0 && (cell != mine || shown[row * MAX_COLS + col] == cell) ) {
                damage_add( &panes[mine & 0xFF], start * CELL_W, row * CELL_H,
                            col * CELL_W - 1, row * CELL_H + CELL_H - 1 );
                start = -1;
            }
            if ( start < 0 && (cell & CELLF_PANE) && (cell & 0xFF) < MAX_PANES
                 && shown[row * MAX_COLS + col] != cell ) {
                start = col;
                mine = cell;
            }
            if ( start >= 0 ) {
                shown[row * MAX_COLS + col] = cell;
            }
        }
    }
    for ( pane = 0; pane < MAX_PANES; pane++ ) {
        pn = &panes[pane];
        if ( !seen[pane] ) {
            pn->on = 0;                 /* nobody placed it this turn */
            pn->shown_rows = 0;
        }
        if ( !pn->on || pn->paint == NULL ) {
            pn->ndamage = 0;
            continue;
        }
        if ( pn->shown_rows && (pn->row != pn->shown_row || pn->col != pn->shown_col
                                || pn->rows != pn->shown_rows || pn->cols != pn->shown_cols) ) {
            pn->ndamage = 0;            /* it moved: all of it is different */
            damage_add( pn, 0, 0, MAX_COLS * CELL_W, MAX_ROWS * CELL_H );
        }
        pn->shown_row = pn->row;
        pn->shown_col = pn->col;
        pn->shown_rows = pn->rows;
        pn->shown_cols = pn->cols;
        left = pn->col * CELL_W;
        top = pn->row * CELL_H;
        right = left + pn->cols * CELL_W - 1;
        bottom = top + pn->rows * CELL_H - 1;
        mine = CELLF_PANE | (uint32_t)pane;
        for ( index = 0; index < pn->ndamage; index++ ) {
            rect = pn->damage[index];
            if ( rect.x0 < left ) {
                rect.x0 = left;
            }
            if ( rect.y0 < top ) {
                rect.y0 = top;
            }
            if ( rect.x1 > right ) {
                rect.x1 = right;
            }
            if ( rect.y1 > bottom ) {
                rect.y1 = bottom;
            }
            if ( rect.x0 > rect.x1 || rect.y0 > rect.y1 ) {
                continue;
            }
            gfx_clip( rect.x0, rect.y0, rect.x1, rect.y1 );
            pn->paint( pane, rect.x0, rect.y0, rect.x1, rect.y1 );
            /* whatever lies over the pane there was painted over, and
               goes back on top */
            for ( row = rect.y0 / CELL_H; row <= rect.y1 / CELL_H; row++ ) {
                for ( col = rect.x0 / CELL_W; col <= rect.x1 / CELL_W; col++ ) {
                    if ( want[row * MAX_COLS + col] != mine ) {
                        shown[row * MAX_COLS + col] = ~want[row * MAX_COLS + col];
                    }
                }
            }
        }
        pn->ndamage = 0;
    }
    gfx_clip_all();
}

/* ------------------------------------------------------------------ */
/* the caret                                                           */
/* ------------------------------------------------------------------ */

void caret_at( int x, int y, int height )
{
    car_x = x;
    car_y = y;
    car_h = height > 0 ? height : 0;
}

void caret_off( void )
{
    car_h = 0;
}

void caret_blink( int on )
{
    car_blinks = on;
    car_phase = 1;
}

/* every cell the caret touches is a pane's */
static int caret_clear( void )
{
    int row, col;

    if ( car_h <= 0 || car_x < 0 || car_y < 0 ) {
        return 0;
    }
    for ( row = car_y / CELL_H; row <= (car_y + car_h - 1) / CELL_H; row++ ) {
        for ( col = car_x / CELL_W; col <= (car_x + 1) / CELL_W; col++ ) {
            if ( row >= scr_rows || col >= scr_cols || !(want[row * MAX_COLS + col] & CELLF_PANE) ) {
                return 0;
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* the screen sent                                                     */
/* ------------------------------------------------------------------ */

static void flush_text( void )
{
    static uint16_t run[MAX_COLS];
    int row, first, last, col;
    CELL *wrow, *srow;

    for ( row = 0; row < scr_rows; row++ ) {
        wrow = want + row * MAX_COLS;
        srow = shown + row * MAX_COLS;
        for ( first = 0; first < scr_cols && wrow[first] == srow[first]; first++ ) {
        }
        if ( first == scr_cols ) {
            continue;
        }
        for ( last = scr_cols - 1; wrow[last] == srow[last]; last-- ) {
        }
        for ( col = first; col <= last; col++ ) {
            run[col - first] = (uint16_t)wrow[col];
            srow[col] = wrow[col];
        }
        /* the cell's number is counted in the screen's own columns,
           which may be more than are used */
        sys_put_cells( (unsigned int)row * (unsigned int)hw_cols + (unsigned int)first, run, (unsigned int)(last - first + 1) );
    }
    if ( cur_kind != shown_kind ) {
        sys_cursor_shape( cur_kind );
        shown_kind = cur_kind;
    }
    if ( cur_kind != CUR_HIDE && (cur_row != shown_row || cur_col != shown_col)
         && cur_row >= 0 && cur_row < hw_rows && cur_col >= 0 && cur_col < hw_cols ) {
        sys_cursor( cur_row, cur_col );
        shown_row = cur_row;
        shown_col = cur_col;
    }
}

void scr_flush( void )
{
    int row, col, color = 0;
    CELL *wrow, *srow;

    if ( !gfx_on ) {
        flush_text();
        return;
    }
    flush_panes();
    for ( row = 0; row < scr_rows; row++ ) {
        wrow = want + row * MAX_COLS;
        srow = shown + row * MAX_COLS;
        for ( col = 0; col < scr_cols; col++ ) {
            if ( wrow[col] != srow[col] && !(wrow[col] & CELLF_PANE) ) {
                gfx_cell( row, col, wrow[col] );
                srow[col] = wrow[col];
            }
        }
    }
    if ( cur_row >= 0 && cur_row < scr_rows && cur_col >= 0 && cur_col < scr_cols
         && !(want[cur_row * MAX_COLS + cur_col] & CELLF_PANE) ) {
        color = (int)((want[cur_row * MAX_COLS + cur_col] >> 8) & 0x0F);
        gfx_cursor( cur_row, cur_col, cur_kind, color );
    } else {
        gfx_cursor( 0, 0, CUR_HIDE, 0 );
    }
    /* the caret starts each turn showing: it does not blink away under
       someone who is typing */
    car_showing = caret_clear();
    car_phase = 1;
    car_time = sys_hundredths();
    gfx_caret( car_x, car_y, car_h, car_showing );
    pointer_show();
    gfx_flush();
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/*                                                                     */
/* The driver keeps the buttons and the position; this turns what it   */
/* says into events - a button pressed, let go, the pointer dragged,   */
/* a second press - and queues them behind the keys.  21h/F0h AL=8 is  */
/* asked to wake for the mouse too, and every wake is a look at        */
/* INT 33h: 05h and 06h for each button's presses and releases since   */
/* the last look, and 03h for where the pointer is now.  On a graphics */
/* screen the same look moves the pointer GFX.C draws.                 */
/* ------------------------------------------------------------------ */

#define MQ_SIZE         32
#define DOUBLE_TIME     40      /* hundredths: a second press in this */
#define DOUBLE_NEAR     4       /* ...and within this many pixels */

MEVENT mouse;

static int      mouse_on;
static int      mouse_xmax = 639, mouse_ymax = 199;     /* a text screen's, as the driver counts */
static MEVENT   mqueue[MQ_SIZE];
static int      mq_head, mq_tail;
static int      m_x, m_y, m_held;       /* as last seen: the driver's position, the buttons */
static int      last_x = -100, last_y = -100, last_button;      /* the last press */
static uint32_t last_time;
static int      moves_wanted;
static int      shape_wanted = MP_ARROW;
static int      testing;

/* MPLAY: what runs while key_get waits (DISP.H), and a wait a key file
   asks for (FF FA tt: tt tenths of a second, the idle running) */
void (*key_idle)( void );
static int      test_waiting;
static uint32_t test_wait_start, test_wait_len;
static int      test_shift;
static int      click_here;     /* test mode: the input just before was a
                                   click, and here */

static int mouse_next( void );

/* the driver's position as pixels and a cell */
static void locate( int x, int y, int *px, int *py, int *row, int *col )
{
    if ( gfx_on ) {
        *col = x / CELL_W;
        *row = y / CELL_H;
    } else {
        *col = (int)((long)x * hw_cols / (mouse_xmax + 1));
        *row = (int)((long)y * hw_rows / (mouse_ymax + 1));
    }
    if ( *col >= scr_cols ) {
        *col = scr_cols - 1;
    }
    if ( *row >= scr_rows ) {
        *row = scr_rows - 1;
    }
    if ( *col < 0 ) {
        *col = 0;
    }
    if ( *row < 0 ) {
        *row = 0;
    }
    if ( gfx_on ) {
        *px = x;
        *py = y;
    } else {
        *px = *col * CELL_W;
        *py = *row * CELL_H;
    }
}

static void queue_event( int kind, int button, int x, int y, int wheel )
{
    MEVENT *event;
    int next, px, py, row, col, newest;
    uint32_t now;

    locate( x, y, &px, &py, &row, &col );
    if ( kind == ME_DOWN ) {
        /* a second press of the same button on the same spot, soon
           enough, is a double one; in test mode "soon" is "with
           nothing typed in between" */
        now = testing ? 0 : sys_hundredths();
        if ( button == last_button && px - last_x <= DOUBLE_NEAR && last_x - px <= DOUBLE_NEAR
             && py - last_y <= DOUBLE_NEAR && last_y - py <= DOUBLE_NEAR
             && (testing ? click_here : since( last_time, now ) < DOUBLE_TIME) ) {
            kind = ME_DOUBLE;
            last_x = -100;              /* and a third press starts again */
        } else {
            last_x = px;
            last_y = py;
            last_button = button;
        }
        last_time = now;
    }
    /* a move on top of a move: only where it got to matters */
    if ( (kind == ME_DRAG || kind == ME_MOVE) && mq_head != mq_tail ) {
        newest = (mq_tail + MQ_SIZE - 1) % MQ_SIZE;
        if ( mqueue[newest].kind == kind && mqueue[newest].button == button ) {
            mq_tail = newest;
        }
    }
    next = (mq_tail + 1) % MQ_SIZE;
    if ( next == mq_head ) {
        return;                         /* full: a slow program loses a drag */
    }
    event = &mqueue[mq_tail];
    event->kind = kind;
    event->button = button;
    event->x = px;
    event->y = py;
    event->row = row;
    event->col = col;
    event->shift = key_shift();
    event->wheel = wheel;
    mq_tail = next;
}

/* the pointer's shape: an hourglass when asked, else what the pane
   under it says */
static void pointer_show( void )
{
    int shape = shape_wanted, pane;

    if ( !gfx_on ) {
        return;
    }
    if ( shape != MP_WAIT ) {
        pane = pane_at( m_x, m_y );
        shape = pane >= 0 ? panes[pane].pointer : MP_ARROW;
    }
    gfx_pointer( m_x, m_y, shape, mouse_on );
}

/* what the driver has to say since the last look */
static void mouse_poll( void )
{
    int x, y, buttons, button, bit, presses, releases, px, py, rx, ry;
    int pressed = 0, moved, wheel, row, col, old_row, old_col, dummy;

    if ( !mouse_on ) {
        return;
    }
    sys_mouse_state( &x, &y, &buttons );
    buttons &= MBTN_LEFT | MBTN_RIGHT;
    for ( button = 0; button < 2; button++ ) {
        bit = 1 << button;
        presses = sys_mouse_count( 0, button, &px, &py );
        releases = sys_mouse_count( 1, button, &rx, &ry );
        pressed |= presses;
        /* the order they happened in: a button down now was pressed last */
        if ( buttons & bit ) {
            if ( releases && (m_held & bit) ) {
                queue_event( ME_UP, bit, rx, ry, 0 );
            }
            if ( presses ) {
                queue_event( ME_DOWN, bit, px, py, 0 );
            }
        } else {
            if ( presses ) {
                queue_event( ME_DOWN, bit, px, py, 0 );
            }
            if ( releases && ((m_held & bit) || presses) ) {
                queue_event( ME_UP, bit, rx, ry, 0 );
            }
        }
    }
    if ( gfx_on ) {
        moved = x != m_x || y != m_y;
    } else {
        locate( x, y, &dummy, &dummy, &row, &col );
        locate( m_x, m_y, &dummy, &dummy, &old_row, &old_col );
        moved = row != old_row || col != old_col;
    }
    m_x = x;
    m_y = y;
    m_held = buttons;
    if ( gfx_on ) {
        pointer_show();
        gfx_flush();
    }
    if ( moved && !pressed ) {
        if ( buttons ) {
            queue_event( ME_DRAG, buttons, x, y, 0 );
        } else if ( moves_wanted ) {
            queue_event( ME_MOVE, 0, x, y, 0 );
        }
    }
    wheel = sys_mouse_wheel();
    if ( wheel ) {
        queue_event( ME_WHEEL, buttons, x, y, wheel );
    }
}

/* After every mode set: the driver works the screen out again on a
   reset.  A text screen's pointer is the driver's own; a graphics
   screen's is GFX.C's, so the driver's stays hidden and is only asked
   where it is - in pixels, once it has been told how many there are. */
static void mouse_start( void )
{
    int x, y, buttons;

    mq_head = mq_tail = 0;
    m_held = 0;
    if ( sys_mouse_reset() == 0 ) {
        mouse_on = 0;
        return;
    }
    mouse_on = 1;
    if ( gfx_on ) {
        sys_mouse_range( gfx_width - 1, gfx_height - 1 );
        sys_mouse_move( gfx_width / 2, gfx_height / 2 );
    } else {
        sys_mouse_extent( &mouse_xmax, &mouse_ymax );
        if ( mouse_xmax < 1 || mouse_ymax < 1 ) {
            mouse_xmax = 639;
            mouse_ymax = 199;
        }
    }
    sys_mouse_count( 0, 0, &x, &y );        /* nothing from before we started */
    sys_mouse_count( 1, 0, &x, &y );
    sys_mouse_count( 0, 1, &x, &y );
    sys_mouse_count( 1, 1, &x, &y );
    sys_mouse_wheel();
    sys_mouse_state( &m_x, &m_y, &buttons );
    m_held = buttons & (MBTN_LEFT | MBTN_RIGHT);
    if ( gfx_on ) {
        pointer_show();
    } else {
        sys_mouse_show( 1 );
    }
}

static void mouse_stop( void )
{
    if ( mouse_on ) {
        if ( gfx_on ) {
            gfx_pointer( 0, 0, MP_ARROW, 0 );
        }
        sys_mouse_reset();                  /* which hides it */
        mouse_on = 0;
    }
}

int mouse_present( void )
{
    return mouse_on;
}

int mouse_buttons( void )
{
    return m_held;
}

void mouse_where( int *x, int *y )
{
    int row, col;

    locate( m_x, m_y, x, y, &row, &col );
}

void mouse_moves( int on )
{
    moves_wanted = on;
}

void mouse_shape( int shape )
{
    shape_wanted = shape;
    if ( gfx_on ) {
        pointer_show();
        gfx_flush();
    }
}

/* a queued mouse event into "mouse": 1 if there was one */
static int mouse_next( void )
{
    if ( mq_head == mq_tail ) {
        return 0;
    }
    mouse = mqueue[mq_head];
    mq_head = (mq_head + 1) % MQ_SIZE;
    return 1;
}

/* test mode: the pointer is put at a place (04h) and the buttons set
   there (21h/F0h AL=0Bh), and what that amounts to is read back
   through INT 33h like anything the mouse does */
static void test_mouse( int buttons, int x, int y )
{
    int was = m_held, old_x = m_x, old_y = m_y;

    if ( !mouse_on ) {
        return;
    }
    if ( gfx_on ) {
        if ( x > gfx_width - 1 ) {
            x = gfx_width - 1;
        }
        if ( y > gfx_height - 1 ) {
            y = gfx_height - 1;
        }
    }
    sys_mouse_move( x, y );
    sys_mouse_inject( buttons );
    mouse_poll();
    if ( was == 0 && buttons == 0 ) {
        click_here = 0;                     /* it only moved */
    }
    if ( was != 0 && buttons == 0 && old_x == m_x && old_y == m_y ) {
        click_here = 1;                     /* a click: the next press here
                                               is a double one */
    }
}

/* ------------------------------------------------------------------ */
/* the keyboard                                                        */
/* ------------------------------------------------------------------ */

static int      shift_seen;
static int      alt_armed;
static uint32_t dump_handle;
static int      dump_open;
static char     shot_stem[PATH_LEN];    /* the log's path, less its extension */
static int      shot_count;

static void key_start( void )
{
    const char *log = sys_getenv( "MPTEST" );
    char *name, *dot;

    if ( log && *log && !sys_stdin_is_console() ) {
        testing = 1;
        if ( sys_create( log, &dump_handle ) == 0 ) {
            dump_open = 1;
        }
        /* pictures go beside the log, named after it: T.LOG's are
           T01.BMP on.  Six characters of the name are kept, for the
           number. */
        fit( shot_stem, log, sizeof( shot_stem ) - 8 );
        name = shot_stem;
        for ( dot = shot_stem; *dot; dot++ ) {
            if ( *dot == '\\' || *dot == '/' || *dot == ':' ) {
                name = dot + 1;
            }
        }
        dot = str_rchr( name, '.' );
        if ( dot != NULL ) {
            *dot = 0;
        }
        if ( str_len( name ) > 6 ) {
            name[6] = 0;
        }
    }
    shift_seen = sys_shift() & SH_KEYS;
}

int key_testing( void )
{
    return testing;
}

int key_shift( void )
{
    return testing ? test_shift : (sys_shift() & SH_KEYS);
}

/* the keys held, as they are added to a key */
static int held( int shift, int with_shift )
{
    int mods = 0;

    if ( with_shift && (shift & SH_SHIFT) ) {
        mods |= K_SHIFT;
    }
    if ( shift & SH_CTRL ) {
        mods |= K_CTRL;
    }
    if ( shift & SH_ALT ) {
        mods |= K_ALT;
    }
    return mods;
}

/* A key with no character.  The BIOS has a scan code of its own for a
   function or arrow key held with Shift, Ctrl or Alt; each is turned
   back into the plain key and what was held. */
static int ext_key( int scan, int shift )
{
    static const uint8_t ctrl_nav[] = { 0x73, 0x4B, 0x74, 0x4D, 0x75, 0x4F, 0x76, 0x51, 0x77, 0x47,
                                   0x84, 0x49, 0x8D, 0x48, 0x8F, 0x4C, 0x91, 0x50, 0x92, 0x52,
                                   0x93, 0x53 };
    static const uint8_t alt_nav[] = { 0x97, 0x47, 0x98, 0x48, 0x99, 0x49, 0x9B, 0x4B, 0x9D, 0x4D,
                                  0x9F, 0x4F, 0xA0, 0x50, 0xA1, 0x51, 0xA2, 0x52, 0xA3, 0x53 };
    static const char row1[] = "QWERTYUIOP";    /* 10h..19h */
    static const char row2[] = "ASDFGHJKL";     /* 1Eh..26h */
    static const char row3[] = "ZXCVBNM";       /* 2Ch..32h */
    int mods = held( shift, 1 ), index;

    if ( scan >= 0x54 && scan <= 0x5D ) {
        return K_EXT( scan - 0x54 + 0x3B ) | mods | K_SHIFT;
    }
    if ( scan >= 0x5E && scan <= 0x67 ) {
        return K_EXT( scan - 0x5E + 0x3B ) | mods | K_CTRL;
    }
    if ( scan >= 0x68 && scan <= 0x71 ) {
        return K_EXT( scan - 0x68 + 0x3B ) | mods | K_ALT;
    }
    if ( scan >= 0x10 && scan <= 0x19 ) {
        return row1[scan - 0x10] | mods | K_ALT;
    }
    if ( scan >= 0x1E && scan <= 0x26 ) {
        return row2[scan - 0x1E] | mods | K_ALT;
    }
    if ( scan >= 0x2C && scan <= 0x32 ) {
        return row3[scan - 0x2C] | mods | K_ALT;
    }
    if ( scan >= 0x78 && scan <= 0x80 ) {
        return ('1' + (scan - 0x78)) | mods | K_ALT;
    }
    switch ( scan ) {
    case 0x81: return '0' | mods | K_ALT;
    case 0x82: return '-' | mods | K_ALT;
    case 0x83: return '=' | mods | K_ALT;
    case 0x87: case 0x88: return K_EXT( scan - 2 ) | mods | K_SHIFT;    /* F11 and F12 */
    case 0x89: case 0x8A: return K_EXT( scan - 4 ) | mods | K_CTRL;
    case 0x8B: case 0x8C: return K_EXT( scan - 6 ) | mods | K_ALT;
    case 0x0F: return K_TAB | mods | K_SHIFT;
    case 0x94: return K_TAB | mods | K_CTRL;
    case 0xA5: return K_TAB | mods | K_ALT;
    case 0x0E: return K_BS | mods | K_ALT;
    case 0x1C: case 0xA6: return K_ENTER | mods | K_ALT;
    case 0x01: return K_ESC | mods | K_ALT;
    }
    for ( index = 0; index < (int)sizeof( ctrl_nav ); index += 2 ) {
        if ( ctrl_nav[index] == scan ) {
            return K_EXT( ctrl_nav[index + 1] ) | mods | K_CTRL;
        }
    }
    for ( index = 0; index < (int)sizeof( alt_nav ); index += 2 ) {
        if ( alt_nav[index] == scan ) {
            return K_EXT( alt_nav[index + 1] ) | mods | K_ALT;
        }
    }
    return K_EXT( scan ) | mods;
}

/*
 * What the kernel read (AL the character, AH the scan code) and the
 * shift keys at the time, as one of DISP.H's keys.
 *
 * A control character is its letter with K_CTRL - except the four that
 * have keys of their own, and the scan code says which it was: 0Eh is
 * the Backspace key, 0Fh Tab, 1Ch (and E0h, the number pad's) Enter,
 * 01h Esc.  A key file's plain byte has no scan code (0), and is
 * Backspace, Tab, Enter or Esc unless the file holds Ctrl down.
 * Ctrl+Enter comes as a line feed and Ctrl+Backspace as 7Fh, from
 * those keys.
 */
static int translate( int raw, int shift )
{
    static const char punct[] = "[\\]^-";       /* 1Bh..1Fh */
    int ascii = raw & 0xFF;
    int scan = (raw >> 8) & 0xFF;
    int ctrl = (shift & SH_CTRL) != 0;

    if ( ascii == 0 || (ascii == 0xE0 && scan != 0) ) {
        return ext_key( scan, shift );
    }
    switch ( ascii ) {
    case 0x08:
        if ( scan == 0x0E || (scan == 0 && !ctrl) ) {
            return K_BS | (held( shift, 1 ) & ~K_CTRL);
        }
        break;
    case 0x7F:
        if ( scan == 0x0E ) {
            return K_BS | K_CTRL | held( shift, 1 );
        }
        return ascii;
    case 0x09:
        if ( scan == 0x0F || (scan == 0 && !ctrl) ) {
            return K_TAB | (held( shift, 1 ) & ~K_CTRL);
        }
        break;
    case 0x0D:
        if ( scan == 0x1C || scan == 0xE0 || (scan == 0 && !ctrl) ) {
            return K_ENTER | (held( shift, 1 ) & ~K_CTRL);
        }
        break;
    case 0x0A:
        if ( scan == 0x1C || scan == 0xE0 ) {
            return K_ENTER | K_CTRL | held( shift, 1 );
        }
        break;
    case 0x1B:
        if ( scan == 0x01 || (scan == 0 && !ctrl) ) {
            return K_ESC | (held( shift, 1 ) & ~K_CTRL);
        }
        break;
    case ' ':
        if ( shift & (SH_CTRL | SH_ALT) ) {
            return ' ' | held( shift, 1 );
        }
        return ' ';
    }
    if ( ascii >= 1 && ascii <= 26 ) {
        return ('A' + ascii - 1) | K_CTRL | held( shift, 1 );
    }
    if ( ascii >= 0x1B && ascii <= 0x1F ) {
        return punct[ascii - 0x1B] | K_CTRL | held( shift, 1 );
    }
    return ascii;
}

/* test mode: the next thing the key file says.  0 when it said
   something that is not a key - the mouse moved to no effect, the
   keyboard took over. */
static int test_key( void )
{
    int byte, next, before, buttons, lo, hi, x, y;

    byte = sys_stdin_byte();
    if ( byte < 0 ) {
        /* PWHOLD: stay on whatever is showing - the keyboard from here
           on, which nobody may be typing at */
        if ( sys_getenv( "MPHOLD" ) ) {
            testing = 0;
            shift_seen = sys_shift() & SH_KEYS;
            return 0;
        }
        return K_EOF;
    }
    if ( byte == 0xFE ) {                   /* the mouse: buttons, row, col */
        buttons = sys_stdin_byte();
        y = sys_stdin_byte();
        x = sys_stdin_byte();
        if ( x < 0 ) {
            return K_EOF;
        }
        if ( gfx_on ) {
            test_mouse( buttons, x * CELL_W + CELL_W / 2, y * CELL_H + CELL_H / 2 );
        } else {
            test_mouse( buttons, x * (mouse_xmax + 1) / hw_cols, y * (mouse_ymax + 1) / hw_rows );
        }
        return 0;
    }
    if ( byte == 0 ) {
        next = sys_stdin_byte();
        click_here = 0;
        return next < 0 ? K_EOF : translate( next << 8, test_shift );
    }
    if ( byte != 0xFF ) {
        click_here = 0;
        return translate( byte, test_shift );
    }
    next = sys_stdin_byte();
    if ( next < 0 ) {
        return K_EOF;
    }
    switch ( next ) {
    case 0xFF:
        return K_DUMP;
    case 0xFE:
        return K_SHOT;
    case 0xFD:                              /* the mouse, in pixels */
        buttons = sys_stdin_byte();
        lo = sys_stdin_byte();
        hi = sys_stdin_byte();
        x = lo | (hi << 8);
        lo = sys_stdin_byte();
        hi = sys_stdin_byte();
        y = lo | (hi << 8);
        if ( hi < 0 ) {
            return K_EOF;
        }
        if ( gfx_on ) {
            test_mouse( buttons, x, y );
        } else {
            test_mouse( buttons, (x / CELL_W) * (mouse_xmax + 1) / hw_cols,
                        (y / CELL_H) * (mouse_ymax + 1) / hw_rows );
        }
        return 0;
    case 0xFC:                              /* a key, character and scan code */
        lo = sys_stdin_byte();
        hi = sys_stdin_byte();
        if ( hi < 0 ) {
            return K_EOF;
        }
        click_here = 0;
        return translate( lo | (hi << 8), test_shift );
    case 0xFA:                              /* a wait, in tenths of a second */
        lo = sys_stdin_byte();
        if ( lo < 0 ) {
            return K_EOF;
        }
        test_wait_len = (uint32_t)lo * 10;
        test_wait_start = sys_hundredths();
        test_waiting = 1;
        return 0;
    case 0xFB:                              /* the wheel */
        lo = sys_stdin_byte();
        if ( lo < 0 ) {
            return K_EOF;
        }
        queue_event( ME_WHEEL, m_held, m_x, m_y, (int)(int8_t)lo );
        return 0;
    }
    before = test_shift;
    test_shift = next & SH_KEYS;
    if ( before == SH_ALT && test_shift == 0 ) {
        return K_ALTTAP;
    }
    return 0;
}

/* the caret's other half, when its time has come */
static void blink( void )
{
    uint32_t now = sys_hundredths();

    if ( since( car_time, now ) >= BLINK_TIME ) {
        car_time = now;
        car_phase = !car_phase;
        gfx_caret( car_x, car_y, car_h, car_phase );
        gfx_flush();
    }
}

/* A key, the mouse, or - with a time limit, "hundredths" not negative -
   K_TICK when neither came.  A time limit, and a caret that blinks,
   wait by looking and looking again. */
static int key_wait( int hundredths )
{
    int shift, raw, got, flags, key, blinking;
    uint32_t start = hundredths >= 0 ? sys_hundredths() : 0;

    for ( ;; ) {
        if ( mouse_next() ) {
            alt_armed = 0;
            return K_MOUSE;
        }
        if ( testing ) {
            if ( test_waiting ) {
                if ( key_idle != NULL ) {
                    key_idle();
                }
                if ( since( test_wait_start, sys_hundredths() ) < test_wait_len ) {
                    continue;
                }
                test_waiting = 0;
            }
            key = test_key();
            if ( key ) {
                return key;
            }
            continue;
        }
        blinking = gfx_on && car_showing && car_blinks;
        flags = mouse_on ? KW_MOUSE : 0;
        if ( hundredths >= 0 || blinking || key_idle != NULL ) {
            flags |= KW_NOWAIT;
        }
        got = sys_key_wait( shift_seen, &shift, flags );
        if ( got == 1 ) {
            raw = sys_key_read( &shift );
            shift_seen = shift & SH_KEYS;
            alt_armed = 0;
            return translate( raw, shift );
        }
        if ( got == 2 ) {
            mouse_poll();
            continue;
        }
        shift &= SH_KEYS;
        if ( (shift & SH_ALT) && !(shift_seen & SH_ALT) ) {
            alt_armed = shift == SH_ALT;
        } else if ( !(shift & SH_ALT) && (shift_seen & SH_ALT) && alt_armed ) {
            alt_armed = 0;
            shift_seen = shift;
            return K_ALTTAP;
        } else if ( shift & ~SH_ALT ) {
            alt_armed = 0;
        }
        shift_seen = shift;
        if ( key_idle != NULL ) {
            key_idle();
        }
        if ( blinking ) {
            blink();
        }
        if ( hundredths >= 0 && since( start, sys_hundredths() ) >= (uint32_t)hundredths ) {
            return K_TICK;
        }
    }
}

int key_get( void )
{
    return key_wait( -1 );
}

/* in test mode there is no time: the key file says what happens next */
int key_get_timed( int hundredths )
{
    return key_wait( testing ? -1 : hundredths );
}

int key_pending( void )
{
    int shift;

    if ( testing ) {
        return 0;
    }
    return sys_key_wait( shift_seen, &shift, KW_NOWAIT ) == 1;
}

int key_alt_letter( int key )
{
    int base = key & ~K_MODS;

    if ( (key & K_MODS) != K_ALT ) {
        return 0;
    }
    return (base >= 'A' && base <= 'Z') || (base >= '0' && base <= '9') ? base : 0;
}

/* ------------------------------------------------------------------ */
/* test mode: the screen into the log                                  */
/* ------------------------------------------------------------------ */

void scr_log( const char *text )
{
    uint32_t done;

    if ( dump_open ) {
        sys_write( dump_handle, text, str_len( text ), &done );
        sys_write( dump_handle, "\r\n", 2, &done );
    }
}

/* the cells as text - the characters, whatever kind of screen they are
   on - and then the mode, the cursor and the caret */
void scr_dump( void )
{
    char line[MAX_COLS + 8];
    uint32_t done;
    int row, col;
    CELL cell;

    if ( !dump_open ) {
        return;
    }
    for ( row = 0; row < scr_rows; row++ ) {
        for ( col = 0; col < scr_cols; col++ ) {
            cell = want[row * MAX_COLS + col];
            line[col] = (cell & CELLF_PANE) ? ' ' : (char)(cell & 0xFF);
            if ( (uint8_t)line[col] < ' ' ) {
                line[col] = '.';
            }
        }
        while ( col > 0 && line[col - 1] == ' ' ) {
            col--;
        }
        line[col++] = '\r';
        line[col++] = '\n';
        sys_write( dump_handle, line, (uint32_t)col, &done );
    }
    sfmt( line, sizeof( line ), "[%s] [cursor %d,%d %s] [caret %d,%d,%d]", modes[mode_now].name,
          cur_row, cur_col, cur_kind == CUR_BLOCK ? "block" : cur_kind == CUR_HIDE ? "hidden" : "line",
          car_h ? car_x : 0, car_h ? car_y : 0, car_h );
    scr_log( line );
}

/* a graphics screen as a .BMP file beside the log - the log's name and
   a number - and a line in the log saying so */
void scr_shot( void )
{
    char path[PATH_LEN], line[PATH_LEN + 32];
    const char *name, *cur;
    int ok = 0;

    if ( !dump_open ) {
        return;
    }
    shot_count++;
    sfmt( path, sizeof( path ), "%s%02d.BMP", shot_stem, shot_count );
    if ( gfx_on ) {
        ok = gfx_snapshot( path );
    }
    name = path;
    for ( cur = path; *cur; cur++ ) {
        if ( *cur == '\\' || *cur == '/' || *cur == ':' ) {
            name = cur + 1;
        }
    }
    sfmt( line, sizeof( line ), "[%s %s]", ok ? "picture" : "no picture", name );
    scr_log( line );
}
