/*
 * SCR.C - the screen in memory, the colours, the keyboard and the mouse.
 *
 * EVERYTHING IS DRAWN INTO MEMORY FIRST, as EDIT draws.  The whole
 * screen - title, menu bar, lists, status bar, whatever dialogs are
 * open - is painted into "want" on every turn of the loop, and
 * scr_flush() sends only the cells that differ from "shown".  Nothing
 * ever has to remember what was under a dialog.
 *
 * ONE SCREEN, TWO WAYS TO SHOW IT.  The rest of the shell draws cells
 * and does not care which: on a text screen a run of changed cells goes
 * to the kernel (21h/F0h AL=4) and the kernel's mouse driver keeps the
 * pointer; on a graphics screen each changed cell is painted into
 * GFX.C's picture from the ROM's font, and GFX.C keeps the pointer and
 * the cursor.  A cell may carry an icon and edge lines, which a text
 * screen never sees - what draws them asks gfx_on first and writes
 * characters instead.
 *
 * THE KEYBOARD knows one thing the kernel's key calls do not say: that
 * Alt was pressed and let go with no key in between, which opens the
 * menu bar.  21h/F0h AL=8 comes back when a key arrives OR the shift
 * keys change, and the change is watched here.
 *
 * THE MOUSE arrives the same way, as K_MOUSE with "mouse" saying what
 * the left button did and on which cell - see mouse_poll.
 *
 * TEST MODE: with DSTEST naming a log file and standard input taken
 * from a file, the keys come from the file - the bytes 21h/07h would
 * have handed back, a zero and a scan code for an extended key - with
 * FFh and a byte to set the shift state, FFh FFh to write the screen
 * into the log as text, FFh FEh to write a graphics screen as a .BMP
 * file beside the log, and FEh and three bytes for the mouse: the
 * buttons, a row and a column.  TOOLS\MKDSKEYS.PL writes such files.
 */
#include "shell.h"

#define MAX_ROWS        60
#define MAX_COLS        132

const DMODE dmodes[NDMODES] = {
    { VK_TEXT, 25,  0, "text25", "Text",     "25 lines   Low resolution" },
    { VK_TEXT, 50,  0, "text50", "Text",     "50 lines   High resolution" },
    { VK_EGA,  25, 14, "ega25",  "Graphics", "25 lines   EGA 640x350, 16 colors" },
    { VK_EGA,  43,  8, "ega43",  "Graphics", "43 lines   EGA 640x350, 16 colors" },
    { VK_VGA,  30, 16, "vga30",  "Graphics", "30 lines   VGA 640x480, 16 colors" },
    { VK_VGA,  34, 14, "vga34",  "Graphics", "34 lines   VGA 640x480, 16 colors" },
    { VK_VGA,  60,  8, "vga60",  "Graphics", "60 lines   VGA 640x480, 16 colors" },
    { VK_VBE,  30, 16, "vbe30",  "Graphics", "30 lines   VBE 640x480, 256 colors" },
    { VK_VBE,  34, 14, "vbe34",  "Graphics", "34 lines   VBE 640x480, 256 colors" },
    { VK_VBE,  60,  8, "vbe60",  "Graphics", "60 lines   VBE 640x480, 256 colors" }
};

PALETTE pal;
int scr_rows = 25, scr_cols = 80;
int scr_mode;
int gfx_on;

static CELL *want;              /* the screen as it should be */
static CELL *shown;             /* the screen as it was last given */
static int  dos_rows;           /* the lines the shell was started on */
static int  cur_row, cur_col, cur_kind = CUR_HIDE;
static int  shown_row = -1, shown_col = -1, shown_kind = -1;
static int  scheme_now;

/* ------------------------------------------------------------------ */
/* the modes                                                           */
/* ------------------------------------------------------------------ */

int scr_mode_ok( int index )
{
    if ( index < 0 || index >= NDMODES ) {
        return 0;
    }
    switch ( dmodes[index].kind ) {
    case VK_TEXT: return 1;
    case VK_EGA:  return gfx_adapter >= VK_EGA;
    case VK_VGA:  return gfx_adapter == VK_VGA;
    case VK_VBE:  return gfx_adapter == VK_VGA && gfx_vbe != 0;
    }
    return 0;
}

static void forget_shown( void )
{
    mem_set( want, 0, (u32)MAX_ROWS * MAX_COLS * sizeof( CELL ) );
    mem_set( shown, 0xFF, (u32)MAX_ROWS * MAX_COLS * sizeof( CELL ) );
    shown_row = shown_col = shown_kind = -1;
}

/* a text screen of at least that many lines; 0Ah's "more than 25" is
   the 8x8 font, which is 50 lines on a VGA and 43 on an EGA */
static void text_mode( int lines )
{
    int rows, cols;

    sys_screen_size( &rows, &cols );
    if ( (lines > 25) != (rows > 25) || cols < 80 ) {
        sys_set_lines( lines );
        sys_screen_size( &rows, &cols );
    }
    scr_rows = rows > MAX_ROWS ? MAX_ROWS : rows;
    scr_cols = cols > MAX_COLS ? MAX_COLS : cols;
}

int scr_set_mode( int index )
{
    const DMODE *mode;
    int ok = 1;

    if ( !scr_mode_ok( index ) ) {
        return 0;
    }
    mode = &dmodes[index];
    mouse_done();
    if ( gfx_on ) {
        gfx_leave();
        gfx_on = 0;
    }
    if ( mode->kind == VK_TEXT ) {
        text_mode( mode->rows );
    } else if ( gfx_enter( mode ) ) {
        gfx_on = 1;
        scr_rows = mode->rows;
        scr_cols = 80;
    } else {
        ok = 0;                         /* the BIOS would not: text, then */
        index = 0;
        text_mode( 25 );
    }
    scr_mode = index;
    forget_shown();
    scr_palette( scheme_now );
    mouse_init();
    return ok;
}

int scr_init( int index )
{
    int cols;

    sys_screen_size( &dos_rows, &cols );
    want = (CELL *)xalloc( (u32)MAX_ROWS * MAX_COLS * sizeof( CELL ) );
    shown = (CELL *)xalloc( (u32)MAX_ROWS * MAX_COLS * sizeof( CELL ) );
    gfx_probe();
    if ( !scr_mode_ok( index ) ) {
        index = 0;
    }
    scr_set_mode( index );
    return scr_mode;
}

/* the screen as DOS programs expect to find it: text, the lines the
   shell was started on, empty, the cursor at the top */
static void dos_screen( void )
{
    int rows, cols;

    mouse_done();
    if ( gfx_on ) {
        gfx_leave();
        gfx_on = 0;
    }
    sys_screen_size( &rows, &cols );
    if ( (rows > 25) != (dos_rows > 25) ) {
        sys_set_lines( dos_rows );
    }
    sys_cls();
    sys_cursor_shape( CUR_LINE );
    sys_cursor( 0, 0 );
}

void scr_done( void )
{
    dos_screen();
}

void scr_suspend( void )
{
    dos_screen();
}

void scr_resume( void )
{
    scr_set_mode( scr_mode );
}

void scr_repaint( void )
{
    mem_set( shown, 0xFF, (u32)MAX_ROWS * MAX_COLS * sizeof( CELL ) );
    shown_row = shown_col = shown_kind = -1;
}

void scr_palette( int scheme )
{
    scheme_now = scheme;
    scheme_fill( scheme, gfx_on, &pal );
    if ( gfx_on ) {
        gfx_scheme( scheme );
    }
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

void scr_ch( int row, int col, int ch, u8 attr )
{
    if ( row >= 0 && row < scr_rows && col >= 0 && col < scr_cols ) {
        want[row * MAX_COLS + col] = ((u32)attr << 8) | (u32)(ch & 0xFF);
    }
}

void scr_putn( int row, int col, const char *text, int len, u8 attr )
{
    int pos;

    for ( pos = 0; pos < len; pos++ ) {
        scr_ch( row, col + pos, (u8)text[pos], attr );
    }
}

void scr_put( int row, int col, const char *text, u8 attr )
{
    scr_putn( row, col, text, (int)str_len( text ), attr );
}

/* exactly "width" cells: the text, cut short or padded with blanks */
void scr_putw( int row, int col, const char *text, int width, u8 attr )
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
void scr_hot( int row, int col, const char *text, u8 attr, u8 hot_attr )
{
    int hot = 0;

    for ( ; *text; text++ ) {
        if ( *text == '&' && !hot ) {
            hot = 1;
            continue;
        }
        if ( hot == 1 ) {
            scr_ch( row, col, (u8)*text, hot_attr );
            if ( gfx_on && hot_attr != attr ) {
                scr_flag( row, col, 1, CF_UNDER );
            }
            hot = 2;
        } else {
            scr_ch( row, col, (u8)*text, attr );
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

void scr_fill( int row, int col, int len, int ch, u8 attr )
{
    int pos;

    for ( pos = 0; pos < len; pos++ ) {
        scr_ch( row, col + pos, ch, attr );
    }
}

void scr_recolor( int row, int col, int len, u8 attr )
{
    int pos;
    CELL *cell;

    if ( row < 0 || row >= scr_rows ) {
        return;
    }
    for ( pos = col; pos < col + len; pos++ ) {
        if ( pos >= 0 && pos < scr_cols ) {
            cell = &want[row * MAX_COLS + pos];
            *cell = (*cell & ~0xFF00UL) | ((u32)attr << 8);
        }
    }
}

void scr_flag( int row, int col, int len, u32 flags )
{
    int pos;

    if ( row < 0 || row >= scr_rows || !gfx_on ) {
        return;
    }
    for ( pos = col; pos < col + len; pos++ ) {
        if ( pos >= 0 && pos < scr_cols ) {
            want[row * MAX_COLS + pos] |= flags;
        }
    }
}

/* The lines along the top and bottom of a title bar - where a cell has
   lines to spare for them.  An 8-line cell is all character, and a bar
   there is its colour and nothing else. */
u32 scr_bar_edges( void )
{
    return gfx_on && dmodes[scr_mode].font > 8 ? CF_TOP | CF_BOTTOM : 0;
}

/* an icon, as many cells as it is wide - on a graphics screen only */
int scr_icon( int row, int col, int icon, u8 attr )
{
    int cells = icons[icon].width / 8, slice;

    if ( !gfx_on ) {
        return 0;
    }
    for ( slice = 0; slice < cells; slice++ ) {
        if ( row >= 0 && row < scr_rows && col + slice >= 0 && col + slice < scr_cols ) {
            want[row * MAX_COLS + col + slice] = ((u32)attr << 8) | ' '
                                                 | CELL_ICON( icon, slice );
        }
    }
    return cells;
}

void scr_box( int top, int left, int bottom, int right, u8 attr )
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
   What is under it keeps its characters and loses its colours - and
   its icons, which have colours of their own. */
void scr_shadow( int top, int left, int bottom, int right )
{
    int row, col;

    for ( row = top + 1; row <= bottom + 1; row++ ) {
        for ( col = row == bottom + 1 ? left + 2 : right + 1; col <= right + 2; col++ ) {
            if ( row >= 0 && row < scr_rows && col >= 0 && col < scr_cols ) {
                want[row * MAX_COLS + col] = (want[row * MAX_COLS + col] & 0xFF)
                                             | ((u32)pal.shadow << 8);
            }
        }
    }
}

void scr_cursor( int row, int col, int kind )
{
    cur_row = row;
    cur_col = col;
    cur_kind = kind;
}

static void flush_text( void )
{
    static u16 run[MAX_COLS];
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
            run[col - first] = (u16)wrow[col];
            srow[col] = wrow[col];
        }
        sys_put_cells( (u32)row * scr_cols + first, run, (u32)(last - first + 1) );
    }
    if ( cur_kind != shown_kind ) {
        sys_cursor_shape( cur_kind );
        shown_kind = cur_kind;
    }
    if ( cur_kind != CUR_HIDE && (cur_row != shown_row || cur_col != shown_col) ) {
        sys_cursor( cur_row, cur_col );
        shown_row = cur_row;
        shown_col = cur_col;
    }
}

void scr_flush( void )
{
    int row, col;
    CELL *wrow, *srow;

    if ( !gfx_on ) {
        flush_text();
        return;
    }
    for ( row = 0; row < scr_rows; row++ ) {
        wrow = want + row * MAX_COLS;
        srow = shown + row * MAX_COLS;
        for ( col = 0; col < scr_cols; col++ ) {
            if ( wrow[col] != srow[col] ) {
                gfx_cell( row, col, wrow[col] );
                srow[col] = wrow[col];
            }
        }
    }
    gfx_caret( cur_row, cur_col, cur_kind,
               cur_row >= 0 && cur_row < scr_rows && cur_col >= 0 && cur_col < scr_cols
               ? (int)((want[cur_row * MAX_COLS + cur_col] >> 8) & 0x0F) : 0 );
    gfx_flush();
}

/* ------------------------------------------------------------------ */
/* scroll bars                                                         */
/*                                                                     */
/* An arrow at each end and a track between them, with a box in the    */
/* track when there is more than fits - one cell, placed by how far    */
/* down the list the first line showing is.                            */
/* ------------------------------------------------------------------ */

static int thumb_row( int top, int bottom, int first, int shown_n, int total )
{
    int track = bottom - top - 1, range = total - shown_n;

    if ( track < 1 || range < 1 ) {
        return -1;
    }
    if ( first > range ) {
        first = range;
    }
    return top + 1 + (track > 1 ? first * (track - 1) / range : 0);
}

void sbar_draw( int top, int bottom, int col, int first, int shown_n, int total )
{
    int row, thumb = thumb_row( top, bottom, first, shown_n, total );

    if ( bottom - top < 1 ) {
        return;
    }
    for ( row = top + 1; row < bottom; row++ ) {
        scr_ch( row, col, gfx_on ? ' ' : 0xB0, pal.scroll );
    }
    if ( gfx_on ) {
        scr_icon( top, col, IC_ARROW_UP, pal.scroll );
        scr_icon( bottom, col, IC_ARROW_DOWN, pal.scroll );
        for ( row = top; row <= bottom; row++ ) {
            scr_flag( row, col, 1, CF_LEFT );
        }
        if ( thumb >= 0 ) {
            scr_ch( thumb, col, ' ', pal.thumb );
            scr_flag( thumb, col, 1, CF_TOP | CF_BOTTOM | CF_LEFT | CF_RIGHT );
        }
    } else {
        scr_ch( top, col, 0x18, pal.scroll );
        scr_ch( bottom, col, 0x19, pal.scroll );
        if ( thumb >= 0 ) {
            scr_ch( thumb, col, 0xDB, pal.thumb );
        }
    }
}

int sbar_hit( int top, int bottom, int col, int first, int shown_n, int total, int row )
{
    int thumb = thumb_row( top, bottom, first, shown_n, total );

    (void)col;
    if ( row == top ) {
        return SB_UP;
    }
    if ( row == bottom ) {
        return SB_DOWN;
    }
    if ( row < top || row > bottom || thumb < 0 ) {
        return SB_NONE;
    }
    if ( row == thumb ) {
        return SB_THUMB;
    }
    return row < thumb ? SB_PGUP : SB_PGDN;
}

/* the box dragged to "row": the first line that should show */
int sbar_drag( int top, int bottom, int shown_n, int total, int row )
{
    int track = bottom - top - 1, range = total - shown_n;

    if ( track < 2 || range < 1 ) {
        return 0;
    }
    row -= top + 1;
    if ( row < 0 ) {
        row = 0;
    }
    if ( row > track - 1 ) {
        row = track - 1;
    }
    return (row * range + (track - 1) / 2) / (track - 1);
}

/* ------------------------------------------------------------------ */
/* the keyboard                                                        */
/* ------------------------------------------------------------------ */

static int shift_seen;
static int alt_armed;
static int testing;
static int test_shift;
static u32 dump_handle;
static int dump_open;
static char shot_stem[PATH_MAX];       /* the log's path, less its extension */
static int shot_count;

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/*                                                                     */
/* The driver keeps the buttons and the position; this turns what it   */
/* says into events on screen cells - the left button pressed, let go, */
/* dragged across a cell, pressed twice - and queues them behind the   */
/* keys.  21h/F0h AL=8 is asked to wake for the mouse too, and every   */
/* wake is a look at INT 33h: 05h and 06h for the presses and releases */
/* since the last look, and 03h for where the pointer is now.  On a    */
/* graphics screen the same look moves the pointer GFX.C draws.        */
/* ------------------------------------------------------------------ */

#define MQ_SIZE         16
#define DOUBLE_TIME     40      /* hundredths: a second press in this */

MEVENT mouse;

static int    mouse_on;
static int    mouse_xmax = 639, mouse_ymax = 199;
static MEVENT mqueue[MQ_SIZE];
static int    mq_head, mq_tail;
static int    m_row = -1, m_col = -1, m_down;
static int    last_row = -1, last_col = -1;     /* the last press */
static u32    last_time;
static int    click_here;       /* test mode: the input just before was
                                   a release, and where it was */

static int mouse_next( void );

static void to_cell( int x, int y, int *row, int *col )
{
    if ( gfx_on ) {
        *col = x / 8;
        *row = y / dmodes[scr_mode].font;
    } else {
        *col = (int)((long)x * scr_cols / (mouse_xmax + 1));
        *row = (int)((long)y * scr_rows / (mouse_ymax + 1));
    }
    if ( *col >= scr_cols ) {
        *col = scr_cols - 1;
    }
    if ( *row >= scr_rows ) {
        *row = scr_rows - 1;
    }
}

static void queue_event( int kind, int row, int col )
{
    int next = (mq_tail + 1) % MQ_SIZE;
    u32 now;

    if ( kind == ME_DOWN ) {
        /* a second press on the same cell, soon enough, is a double one;
           in test mode "soon" is "with nothing typed in between" */
        now = testing ? 0 : sys_hundredths();
        if ( row == last_row && col == last_col
             && (testing ? click_here : now - last_time < DOUBLE_TIME) ) {
            kind = ME_DOUBLE;
            last_row = -1;              /* and a third press starts again */
        } else {
            last_row = row;
            last_col = col;
        }
        last_time = now;
    }
    if ( next == mq_head ) {
        return;                         /* full: a slow program loses a drag */
    }
    mqueue[mq_tail].kind = kind;
    mqueue[mq_tail].row = row;
    mqueue[mq_tail].col = col;
    mqueue[mq_tail].shift = key_shift();
    mq_tail = next;
}

/* what the driver has to say since the last look */
static void mouse_poll( void )
{
    int x, y, buttons, row, col, presses, releases, px, py, rx, ry;
    int prow, pcol, rrow, rcol;

    if ( !mouse_on ) {
        return;
    }
    presses = sys_mouse_count( 0, 0, &px, &py );
    releases = sys_mouse_count( 1, 0, &rx, &ry );
    sys_mouse_state( &x, &y, &buttons );
    if ( gfx_on ) {
        gfx_pointer( x, y, 1 );
        gfx_flush();
    }
    to_cell( x, y, &row, &col );
    to_cell( px, py, &prow, &pcol );
    to_cell( rx, ry, &rrow, &rcol );
    /* the order they happened in: a button down now was pressed last */
    if ( buttons & 1 ) {
        if ( releases && m_down ) {
            queue_event( ME_UP, rrow, rcol );
        }
        if ( presses ) {
            queue_event( ME_DOWN, prow, pcol );
        }
    } else {
        if ( presses ) {
            queue_event( ME_DOWN, prow, pcol );
        }
        if ( releases && (m_down || presses) ) {
            queue_event( ME_UP, rrow, rcol );
        }
    }
    m_down = buttons & 1;
    if ( m_down && (row != m_row || col != m_col) && !presses ) {
        queue_event( ME_DRAG, row, col );
    }
    m_row = row;
    m_col = col;
}

/* After every mode set: the driver works the screen out again on a
   reset.  A text screen's pointer is the driver's own; a graphics
   screen's is GFX.C's, so the driver's stays hidden and is only asked
   where it is - in pixels, once it has been told how many there are. */
void mouse_init( void )
{
    int x, y;

    mq_head = mq_tail = 0;
    if ( sys_mouse_reset() == 0 ) {
        return;
    }
    mouse_on = 1;
    if ( gfx_on ) {
        mouse_xmax = 639;
        mouse_ymax = dmodes[scr_mode].kind == VK_EGA ? 349 : 479;
        sys_mouse_range( mouse_xmax, mouse_ymax );
        sys_mouse_move( 320, mouse_ymax / 2 );
    } else {
        sys_mouse_extent( &mouse_xmax, &mouse_ymax );
        if ( mouse_xmax < 1 || mouse_ymax < 1 ) {
            mouse_xmax = 639;
            mouse_ymax = 199;
        }
    }
    sys_mouse_state( &x, &y, &m_down );
    m_down &= 1;
    to_cell( x, y, &m_row, &m_col );
    sys_mouse_count( 0, 0, &x, &y );        /* nothing from before we started */
    sys_mouse_count( 1, 0, &x, &y );
    if ( gfx_on ) {
        sys_mouse_state( &x, &y, &m_down );
        m_down &= 1;
        gfx_pointer( x, y, 1 );
    } else {
        sys_mouse_show( 1 );
    }
}

void mouse_done( void )
{
    if ( mouse_on ) {
        if ( gfx_on ) {
            gfx_pointer( 0, 0, 0 );
        }
        sys_mouse_reset();                  /* which hides it */
        mouse_on = 0;
    }
}

/* test mode: FEh, the buttons, a row and a column - the pointer is put
   on that cell (04h) and the buttons set there (21h/F0h AL=0Bh), and
   what that amounts to is read back through INT 33h like anything the
   mouse does */
static void test_mouse( int buttons, int row, int col )
{
    int was_up = !m_down, prow = m_row, pcol = m_col;

    if ( !mouse_on ) {
        return;
    }
    if ( gfx_on ) {
        sys_mouse_move( col * 8 + 4, row * dmodes[scr_mode].font + dmodes[scr_mode].font / 2 );
    } else {
        sys_mouse_move( col * (mouse_xmax + 1) / scr_cols, row * (mouse_ymax + 1) / scr_rows );
    }
    sys_mouse_inject( buttons );
    mouse_poll();
    if ( was_up && (buttons & 1) == 0 ) {
        click_here = 0;
    }
    if ( !was_up && (buttons & 1) == 0 && prow == row && pcol == col ) {
        click_here = 1;                     /* a click: the next press here
                                               is a double one */
    }
}

void key_init( void )
{
    const char *log = sys_getenv( "DSTEST" );

    if ( log && *log && !sys_stdin_is_console() ) {
        testing = 1;
        if ( sys_create( log, &dump_handle ) == 0 ) {
            dump_open = 1;
        }
        /* pictures go beside the log, named after it: T.SCR's are T01.BMP on */
        str_fit( shot_stem, log, sizeof( shot_stem ) - 8 );
        if ( *path_ext( shot_stem ) ) {
            ((char *)path_ext( shot_stem ))[-1] = 0;
        }
        if ( str_len( path_name( shot_stem ) ) > 6 ) {
            ((char *)path_name( shot_stem ))[6] = 0;
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
    return testing ? test_shift : sys_shift();
}

static int is_move( int key )
{
    switch ( key ) {
    case K_HOME: case K_END: case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT:
    case K_PGUP: case K_PGDN: case K_CHOME: case K_CEND:
        return 1;
    }
    return 0;
}

static int with_shift( int key, int shift )
{
    if ( shift & SH_SHIFT ) {
        if ( is_move( key ) ) {
            return key | K_SHIFT;
        }
        if ( key == K_TAB ) {
            return K_SHTAB;
        }
    }
    return key;
}

/* Ctrl and a letter is a drive, and three of them are also Backspace,
   Tab and Enter: the scan code tells those from Ctrl+H, I and M */
static int translate( int raw, int shift )
{
    int ascii = raw & 0xFF;
    int scan = (raw >> 8) & 0xFF;

    if ( ascii == 0 || ascii == 0xE0 ) {
        return with_shift( K_EXT( scan ), shift );
    }
    if ( ascii >= 1 && ascii <= 26 && (shift & SH_CTRL)
         && scan != 0x0E && scan != 0x0F && scan != 0x1C ) {
        return K_CTRL( 'A' + ascii - 1 );
    }
    return with_shift( ascii, shift );
}

static int test_key( void )
{
    int byte, next, before;

    for ( ;; ) {
        byte = sys_stdin_byte();
        if ( byte < 0 ) {
            /* DSHOLD: stay on whatever is showing - the keyboard from
               here on, which nobody is typing at */
            if ( sys_getenv( "DSHOLD" ) ) {
                testing = 0;
                return key_get();
            }
            return K_EOF;
        }
        if ( byte == 0xFE ) {                   /* the mouse: buttons, row, col */
            int buttons = sys_stdin_byte(), row = sys_stdin_byte(), col = sys_stdin_byte();
            if ( col < 0 ) {
                return K_EOF;
            }
            test_mouse( buttons, row, col );
            if ( mouse_next() ) {
                return K_MOUSE;
            }
            continue;
        }
        click_here = 0;
        if ( byte == 0 ) {
            next = sys_stdin_byte();
            return next < 0 ? K_EOF : with_shift( K_EXT( next ), test_shift );
        }
        if ( byte != 0xFF ) {
            if ( byte <= 26 && byte != K_BS && byte != K_TAB && byte != K_ENTER ) {
                return K_CTRL( 'A' + byte - 1 );
            }
            return with_shift( byte, test_shift );
        }
        next = sys_stdin_byte();
        if ( next < 0 ) {
            return K_EOF;
        }
        if ( next == 0xFF ) {
            return K_DUMP;
        }
        if ( next == 0xFE ) {
            return K_SHOT;
        }
        before = test_shift;
        test_shift = next & SH_KEYS;
        if ( before == SH_ALT && test_shift == 0 ) {
            return K_ALTTAP;
        }
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

/* A key, the mouse, or - with a time limit, "hundredths" not negative -
   K_TICK when neither came.  A time limit waits by looking and looking
   again, which is what holding a scroll arrow down costs. */
static int key_wait( int hundredths )
{
    int shift, raw, got, flags;
    u32 start = hundredths >= 0 ? sys_hundredths() : 0;

    for ( ;; ) {
        if ( mouse_next() ) {
            alt_armed = 0;
            return K_MOUSE;
        }
        if ( testing ) {
            return test_key();
        }
        flags = mouse_on ? KW_MOUSE : 0;
        if ( hundredths >= 0 ) {
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
        if ( hundredths >= 0 && sys_hundredths() - start >= (u32)hundredths ) {
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

/* what every loop that reads keys does with the test harness's own:
   1 = it was one of them, and it is dealt with */
int key_service( int key )
{
    switch ( key ) {
    case K_EOF:
        app_abandon();
        return 1;
    case K_DUMP:
        scr_dump();
        return 1;
    case K_SHOT:
        scr_shot();
        return 1;
    }
    return 0;
}

/* Alt+letter and Alt+digit arrive as a zero and the key's scan code */
int key_alt_letter( int key )
{
    static const char row1[] = "QWERTYUIOP";    /* 10h..19h */
    static const char row2[] = "ASDFGHJKL";     /* 1Eh..26h */
    static const char row3[] = "ZXCVBNM";       /* 2Ch..32h */
    int scan;

    if ( (key & 0xFF00) != 0x100 ) {
        return 0;
    }
    scan = key & 0xFF;
    if ( scan >= 0x10 && scan <= 0x19 ) {
        return row1[scan - 0x10];
    }
    if ( scan >= 0x1E && scan <= 0x26 ) {
        return row2[scan - 0x1E];
    }
    if ( scan >= 0x2C && scan <= 0x32 ) {
        return row3[scan - 0x2C];
    }
    if ( scan >= 0x78 && scan <= 0x80 ) {
        return '1' + (scan - 0x78);
    }
    if ( scan == 0x81 ) {
        return '0';
    }
    return 0;
}

/* test mode: the screen as text into the log - the characters, whatever
   kind of screen they are on - and where the cursor is */
void scr_dump( void )
{
    char line[MAX_COLS + 8], num[12];
    u32 done;
    int row, col;

    if ( !dump_open ) {
        return;
    }
    for ( row = 0; row < scr_rows; row++ ) {
        for ( col = 0; col < scr_cols; col++ ) {
            line[col] = (char)(want[row * MAX_COLS + col] & 0xFF);
            if ( (u8)line[col] < ' ' ) {
                line[col] = '.';
            }
        }
        while ( col > 0 && line[col - 1] == ' ' ) {
            col--;
        }
        line[col++] = '\r';
        line[col++] = '\n';
        sys_write( dump_handle, line, (u32)col, &done );
    }
    str_cpy( line, "[" );
    str_catn( line, dmodes[scr_mode].tag, sizeof( line ) );
    str_catn( line, "] [cursor ", sizeof( line ) );
    str_catn( line, fmt_dec( (u32)cur_row, 0, num ), sizeof( line ) );
    str_catn( line, ",", sizeof( line ) );
    str_catn( line, fmt_dec( (u32)cur_col, 0, num ), sizeof( line ) );
    str_catn( line, cur_kind == CUR_BLOCK ? " block]" : cur_kind == CUR_HIDE ?
              " hidden]" : " line]", sizeof( line ) );
    str_catn( line, "\r\n", sizeof( line ) );
    sys_write( dump_handle, line, str_len( line ), &done );
}

/* test mode: a graphics screen as a .BMP file beside the log - the
   log's name and a number - and a line in the log saying so */
void scr_shot( void )
{
    char path[PATH_MAX], line[80], num[12];
    u32 done;
    int ok;

    if ( !dump_open || !gfx_on ) {
        return;
    }
    shot_count++;
    str_cpy( path, shot_stem );
    str_catn( path, fmt_dec2( (u32)shot_count, num ), sizeof( path ) );
    str_catn( path, ".BMP", sizeof( path ) );
    ok = gfx_snapshot( path );
    str_cpy( line, ok ? "[picture " : "[no picture " );
    str_catn( line, path_name( path ), sizeof( line ) );
    str_catn( line, "]\r\n", sizeof( line ) );
    sys_write( dump_handle, line, str_len( line ), &done );
}
