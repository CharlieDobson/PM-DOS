/*
 * SCR.C - the screen in memory, the colours, the keyboard and the
 * mouse.  EDIT's, by way of PMD and SCANDISK, with DEFRAG's colours.
 *
 * EVERYTHING IS DRAWN INTO MEMORY FIRST.  The whole screen - the map,
 * the two boxes under it, the menu, whatever box is open - is painted
 * into "want" on every turn of a loop, and scr_flush() sends only the
 * cells that differ from "shown", what the screen was last given.
 * Nothing ever has to remember what was under a box, and a map of a
 * thousand blocks costs a write for the one that changed.
 *
 * ONE SCREEN, TWO WAYS OF SHOWING IT.  On a text screen the runs of
 * changed cells go to the kernel as characters and colours.  On a
 * graphics screen (DEFRAG /G) each changed cell goes to GFX.C, which
 * paints it - and a cell may then be more than a character: see UI.H.
 * What draws those extras asks gfx_on first and writes plain
 * characters when the screen is text.
 *
 * THE COLOURS ARE MS-DOS DEFRAG'S: white on bright blue, a white line
 * along the top and the bottom, boxes that are bright cyan.  Every one
 * of those is a background a text screen does not have until the
 * adapter is told that the top bit of a cell's colours means "bright"
 * and not "blink" - which INT 10h/1003h would do, and a native program
 * has no INT 10h.  On a VGA the bit is in the attribute controller's
 * mode register and SCR.C sets it there, and puts it back; on anything
 * else the same screen is drawn in the eight backgrounds there are:
 * blue, grey and cyan.
 *
 * THE KEYBOARD AND THE MOUSE arrive through key_get(), which waits, and
 * key_poll(), which does not: a drive that is being optimized looks for
 * a key between one move and the next.  The mouse comes as K_MOUSE,
 * with "mouse" saying what the left button did and on which cell.
 *
 * TEST MODE: with DFTEST naming a log file and standard input taken
 * from a file, the keys come from the file - the bytes 21h/07h would
 * have handed back, a zero and a scan code for an extended key - with
 * FFh and a byte to set the shift state, FFh FFh to write the screen
 * into the log as text, FFh FEh to write a graphics screen as a .BMP
 * beside the log, and FEh and three bytes for the mouse: the buttons, a
 * row and a column, which INT 33h is told as if the mouse had done it.
 * NEITHER FILE MAY BE ON THE DRIVE BEING OPTIMIZED: see MAIN.C.
 *
 * A drive that is being optimized does not take keys from the file -
 * they are for the questions - unless DFPOLL=n is set, and then its
 * n-th look for a key, and every look after that, takes the next one:
 * that is how Esc in the middle of a run is tested.
 * DFHOLD leaves the last screen up when the file runs out.
 */
#include "ui.h"

PALETTE pal;
int scr_rows = 25, scr_cols = 80;
int gfx_on;

static CELL *want;              /* the screen as it should be */
static CELL *shown;             /* the screen as it was last given */
static u16 *dos_screen;         /* what DEFRAG started on */
static int  dos_rows, dos_cols, dos_row, dos_col;
static int  cur_row, cur_col, cur_kind = CUR_LINE;
static int  shown_row = -1, shown_col = -1, shown_kind = -1;

/* ------------------------------------------------------------------ */
/* colours                                                             */
/* ------------------------------------------------------------------ */

#define ATTR_STATUS     6       /* the status port, counted from the CRTC's */
#define ATTR_INDEX      0x3C0
#define ATTR_READ       0x3C1
#define ATTR_MODE       0x30    /* the mode register, the screen left on */
#define ATTR_BLINK      0x08

static int  bright_bg;          /* a text screen with sixteen backgrounds */
static u8   attr_mode;          /* the mode register as it was found */
static u16  crtc_port;

static void attr_mode_set( unsigned value )
{
    port_in( crtc_port + ATTR_STATUS );     /* the next write is an index */
    port_out( ATTR_INDEX, ATTR_MODE );
    port_out( ATTR_INDEX, value );
}

/* VGA text: the top bit of a cell's colours stops meaning "blink".  An
   EGA cannot be asked what its register holds, so it is left alone. */
static void blink_off( void )
{
    if ( gfx_probe() != GA_VGA || !sys_peek( 0x463, &crtc_port, 2 ) ||
         (crtc_port != 0x3D4 && crtc_port != 0x3B4) ) {
        return;
    }
    port_in( crtc_port + ATTR_STATUS );
    port_out( ATTR_INDEX, ATTR_MODE );
    attr_mode = (u8)port_in( ATTR_READ );
    port_in( crtc_port + ATTR_STATUS );     /* and the next write is an index again */
    if ( (attr_mode & 0x01) || attr_mode == 0xFF ) {
        return;                             /* not a text screen's: not believed */
    }
    attr_mode_set( attr_mode & ~ATTR_BLINK );
    bright_bg = 1;
}

static void blink_back( void )
{
    if ( bright_bg ) {
        attr_mode_set( attr_mode );
        bright_bg = 0;
    }
}

static void scr_palette( void )
{
    if ( gfx_on || bright_bg ) {
        pal.desk = 0x9F;            /* white on bright blue */
        pal.desk_title = 0x9E;      /* yellow */
        pal.done = 0x9E;
        pal.bar = 0xF0;             /* black on white */
        pal.bar_hot = 0xF4;         /* red */
        pal.bar_sel = 0x0F;
        pal.hint = 0xF4;
        pal.hint_rule = 0xF0;
        pal.dlg = 0xB0;             /* black on bright cyan */
        pal.dlg_title = 0xBC;       /* bright red */
        pal.dlg_caption = 0xF1;     /* blue on white */
        pal.dlg_focus = 0xBC;
        pal.err = 0x4F;             /* white on red */
        pal.err_title = 0x4E;
        pal.btn = 0x70;             /* black on grey */
        pal.btn_default = 0x71;     /* blue */
        pal.btn_focus = 0xF1;       /* blue on white */
        pal.list_sel = 0x0F;
        pal.track = 0xF0;
        pal.shadow = 0x07;
        return;
    }
    /* a text screen whose top bit still blinks */
    pal.desk = 0x1F;                /* white on blue */
    pal.desk_title = 0x1E;
    pal.done = 0x1E;
    pal.bar = 0x70;                 /* black on grey */
    pal.bar_hot = 0x74;
    pal.bar_sel = 0x0F;
    pal.hint = 0x74;
    pal.hint_rule = 0x70;
    pal.dlg = 0x30;                 /* black on cyan */
    pal.dlg_title = 0x34;
    pal.dlg_caption = 0x30;
    pal.dlg_focus = 0x34;
    pal.err = 0x4F;
    pal.err_title = 0x4E;
    pal.btn = 0x70;
    pal.btn_default = 0x71;
    pal.btn_focus = 0x1F;           /* white on blue */
    pal.list_sel = 0x0F;
    pal.track = 0x70;
    pal.shadow = 0x07;
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

void scr_init( int graphics )
{
    u32 cells;

    sys_screen_size( &dos_rows, &dos_cols );
    sys_get_cursor( &dos_row, &dos_col );
    dos_screen = (u16 *)try_alloc( (u32)dos_rows * dos_cols * 2 );
    if ( dos_screen ) {
        sys_get_cells( 0, dos_screen, (u32)dos_rows * dos_cols );
    }
    scr_rows = dos_rows > 60 ? 60 : dos_rows;
    scr_cols = dos_cols > 132 ? 132 : dos_cols;
    if ( graphics && gfx_enter() ) {
        gfx_on = 1;
        scr_rows = gfx_rows;
        scr_cols = 80;
    } else {
        blink_off();
    }
    cells = (u32)scr_rows * scr_cols;
    want = (CELL *)xalloc( cells * sizeof( CELL ) );
    shown = (CELL *)xalloc( cells * sizeof( CELL ) );
    mem_set( want, 0, cells * sizeof( CELL ) );
    mem_set( shown, 0xFF, cells * sizeof( CELL ) );     /* nothing shown matches */
    scr_palette();
}

void scr_done( void )
{
    mouse_done();
    if ( gfx_on ) {
        gfx_leave();
        gfx_on = 0;
    }
    blink_back();
    if ( dos_screen ) {
        sys_put_cells( 0, dos_screen, (u32)dos_rows * dos_cols );
    }
    sys_cursor_shape( CUR_LINE );
    sys_cursor( dos_row, dos_col );
    /* it may be taken again: another program is run from the middle */
    xfree( want );
    xfree( shown );
    xfree( dos_screen );
    want = shown = NULL;
    dos_screen = NULL;
    shown_row = shown_col = shown_kind = -1;
}

void scr_ch( int row, int col, int ch, u8 attr )
{
    if ( row >= 0 && row < scr_rows && col >= 0 && col < scr_cols ) {
        want[row * scr_cols + col] = ((CELL)attr << 8) | (CELL)(ch & 0xFF);
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

/* graphics: more bits for cells that are there already */
void scr_flag( int row, int col, int len, u32 flags )
{
    int pos;

    if ( !gfx_on || row < 0 || row >= scr_rows ) {
        return;
    }
    for ( pos = col; pos < col + len; pos++ ) {
        if ( pos >= 0 && pos < scr_cols ) {
            want[row * scr_cols + pos] |= flags;
        }
    }
}

/* graphics: a picture in a cell, and what the picture needs told */
void scr_pic( int row, int col, int pic, int data, u8 attr )
{
    if ( gfx_on && row >= 0 && row < scr_rows && col >= 0 && col < scr_cols ) {
        want[row * scr_cols + col] = ((CELL)pic << CF_PIC_SHIFT) | ((CELL)attr << 8)
                                     | (CELL)(data & 0xFF);
    }
}

/* a string in the middle of "width" cells from "left" */
void scr_center( int row, int left, int width, const char *text, u8 attr )
{
    int len = (int)str_len( text );

    scr_put( row, left + (width > len ? (width - len) / 2 : 0), text, attr );
}

/* "&File": the character after '&' in hot_attr - underlined, on a
   graphics screen - and the '&' not shown */
void scr_hot( int row, int col, const char *text, u8 attr, u8 hot_attr )
{
    int hot = 0;

    for ( ; *text; text++ ) {
        if ( *text == '&' && !hot ) {
            hot = 1;
            continue;
        }
        scr_ch( row, col, (u8)*text, hot == 1 ? hot_attr : attr );
        if ( hot == 1 ) {
            scr_flag( row, col, 1, CF_UNDER );
            hot = 2;
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

/* other colours for what is there.  A graphics cell loses whatever
   else it was - an edge, a button, a picture - and is its character. */
void scr_recolor( int row, int col, int len, u8 attr )
{
    int pos;
    CELL cell;

    if ( row < 0 || row >= scr_rows ) {
        return;
    }
    for ( pos = col; pos < col + len; pos++ ) {
        if ( pos >= 0 && pos < scr_cols ) {
            cell = want[row * scr_cols + pos];
            want[row * scr_cols + pos] = ((cell & CF_PIC_MASK) ? ' ' : (cell & 0xFF))
                                         | ((CELL)attr << 8);
        }
    }
}

void scr_box( int top, int left, int bottom, int right, u8 attr, int twice )
{
    static const u8 lines[2][6] = {
        { 0xDA, 0xC4, 0xBF, 0xB3, 0xC0, 0xD9 },
        { 0xC9, 0xCD, 0xBB, 0xBA, 0xC8, 0xBC }
    };
    const u8 *use = lines[twice ? 1 : 0];
    int row;

    scr_ch( top, left, use[0], attr );
    scr_fill( top, left + 1, right - left - 1, use[1], attr );
    scr_ch( top, right, use[2], attr );
    for ( row = top + 1; row < bottom; row++ ) {
        scr_ch( row, left, use[3], attr );
        scr_fill( row, left + 1, right - left - 1, ' ', attr );
        scr_ch( row, right, use[3], attr );
    }
    scr_ch( bottom, left, use[4], attr );
    scr_fill( bottom, left + 1, right - left - 1, use[1], attr );
    scr_ch( bottom, right, use[5], attr );
}

/* the shadow a box casts: two columns to its right, one row below */
void scr_shadow( int top, int left, int bottom, int right )
{
    int row;

    for ( row = top + 1; row <= bottom + 1; row++ ) {
        scr_recolor( row, right + 1, 2, pal.shadow );
    }
    scr_recolor( bottom + 1, left + 2, right - left + 1, pal.shadow );
}

void scr_cursor( int row, int col, int kind )
{
    cur_row = row;
    cur_col = col;
    cur_kind = kind;
}

static void flush_text( void )
{
    static u16 run[132];
    int row, col, first, last;
    u32 base;

    for ( row = 0; row < scr_rows; row++ ) {
        base = (u32)row * scr_cols;
        for ( first = 0; first < scr_cols && want[base + first] == shown[base + first]; first++ ) {
        }
        if ( first == scr_cols ) {
            continue;
        }
        for ( last = scr_cols - 1; want[base + last] == shown[base + last]; last-- ) {
        }
        for ( col = first; col <= last; col++ ) {
            run[col - first] = (u16)want[base + col];
            shown[base + col] = want[base + col];
        }
        sys_put_cells( base + first, run, (u32)(last - first + 1) );
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
    u32 at;

    if ( !gfx_on ) {
        flush_text();
        return;
    }
    for ( row = 0; row < scr_rows; row++ ) {
        for ( col = 0; col < scr_cols; col++ ) {
            at = (u32)row * scr_cols + col;
            if ( want[at] != shown[at] ) {
                gfx_cell( row, col, want[at] );
                shown[at] = want[at];
            }
        }
    }
    gfx_flush();
}

/* ------------------------------------------------------------------ */
/* the keyboard                                                        */
/* ------------------------------------------------------------------ */

static int shift_seen;
static int alt_armed;
static int testing;
static u32 poll_from;           /* test mode: a run takes keys too, from
                                   this look for one on (0 = never) */
static u32 polls;
static int test_shift;
static u32 dump_handle;
static int dump_open;
static char dump_path[PATH_MAX];
static int shots;

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/*                                                                     */
/* The driver keeps the buttons and the position; this turns what it   */
/* says into events on screen cells - the left button pressed, let go, */
/* dragged across a cell, pressed twice - and queues them behind the   */
/* keys.  21h/F0h AL=8 is asked to wake for the mouse too, and every   */
/* wake is a look at INT 33h: 05h and 06h for the presses and releases */
/* since the last look, which a fast click would otherwise lose        */
/* between two looks, and 03h for where the pointer is now.  On a      */
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
        *row = y / gfx_cell_h;
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

/* A text screen's pointer is the driver's own; a graphics screen's is
   GFX.C's, so the driver's stays hidden and is only asked where it is -
   in pixels, once it has been told how many there are. */
void mouse_init( void )
{
    int x, y;

    mq_head = mq_tail = 0;                  /* nothing from before a program was run */
    last_row = last_col = -1;
    if ( sys_mouse_reset() == 0 ) {
        return;
    }
    mouse_on = 1;
    if ( gfx_on ) {
        mouse_xmax = 639;
        mouse_ymax = gfx_rows * gfx_cell_h - 1;
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

int mouse_present( void )
{
    return mouse_on;
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
        sys_mouse_move( col * 8 + 4, row * gfx_cell_h + gfx_cell_h / 2 );
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
    const char *log = sys_getenv( "DFTEST" ), *from = sys_getenv( "DFPOLL" );

    if ( log && *log && !sys_stdin_is_console() ) {
        testing = 1;
        for ( poll_from = 0; from && *from >= '0' && *from <= '9'; from++ ) {
            poll_from = poll_from * 10 + (u32)(*from - '0');
        }
        if ( sys_create( log, &dump_handle ) == 0 ) {
            dump_open = 1;
            str_cpyn( dump_path, log, sizeof( dump_path ) );
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
    case K_PGUP: case K_PGDN: case K_CLEFT: case K_CRIGHT: case K_CHOME:
    case K_CEND: case K_CPGUP: case K_CPGDN: case K_DEL: case K_INS:
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

static int translate( int raw, int shift )
{
    int ascii = raw & 0xFF;
    int scan = (raw >> 8) & 0xFF;

    if ( ascii == 0 || ascii == 0xE0 ) {
        return with_shift( K_EXT( scan ), shift );
    }
    return with_shift( ascii, shift );
}

static int test_key( void )
{
    int byte, next, before;

    for ( ;; ) {
        byte = sys_stdin_byte();
        if ( byte < 0 ) {
            /* DFHOLD: stay on whatever is showing, for a screenshot -
               the keyboard from here on, which nobody is typing at */
            if ( sys_getenv( "DFHOLD" ) ) {
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
   K_TICK when neither came.  A limit of nothing is one look. */
static int key_wait( int hundredths )
{
    int shift, raw, got, flags;
    u32 start = hundredths > 0 ? sys_hundredths() : 0;

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
        if ( hundredths == 0 || (hundredths > 0 && sys_hundredths() - start >= (u32)hundredths) ) {
            return K_TICK;
        }
    }
}

int key_get( void )
{
    return key_wait( -1 );
}

/* A look, and back at once.  In test mode the key file is for the
   questions, not for a drive that is being optimized - unless DFPOLL
   says from which look on the keys are for that as well. */
int key_poll( void )
{
    if ( testing && (poll_from == 0 || ++polls < poll_from) ) {
        return K_TICK;
    }
    return key_wait( 0 );
}

int key_wait_for( int hundredths )
{
    return key_wait( hundredths );
}

/* test mode: the drive the log is being written to, which must not be
   the one optimized; -1 when there is no log */
int key_log_drive( void )
{
    if ( !dump_open ) {
        return -1;
    }
    if ( dump_path[0] && dump_path[1] == ':' ) {
        return ch_upper( dump_path[0] ) - 'A';
    }
    return sys_get_drive();
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

/* a cell as one character of the log: the pictures of a graphics screen
   and the characters below a space as what they are on the screen */
static char dump_char( CELL cell )
{
    int ch = (int)(cell & 0xFF), pic = (int)((cell & CF_PIC_MASK) >> CF_PIC_SHIFT);
    int yellow = ((cell >> 8) & 0x0F) == 0x0E;

    switch ( pic ) {
    case PIC_BAR:
        return (char)((ch & 0x0F) >= 4 ? 0xDB : 0xB0);
    case PIC_BLOCK:
        return (char)(ch == BK_USED ? '#' : ch == BK_DONE ? '@' : 0xB1);
    case PIC_RADIO:
        return (char)((ch & 1) ? ((ch & 2) ? '*' : ' ') : '(');
    case PIC_CLOSE:
        return '-';
    case PIC_DRIVE:
        return (char)((ch & 4) ? 'f' : 'h');
    }
    switch ( ch ) {
    case 0x07:  return '*';
    case 0x08:  return (char)(yellow ? '@' : '#');
    case 0x10:  return '>';
    case 0x11:  return '<';
    case 0x18:  return '^';
    case 0x19:  return 'v';
    }
    return (char)(ch < ' ' ? '.' : ch);
}

/* test mode: the screen as text into the log, and where the cursor is */
void scr_dump( void )
{
    char line[140];
    u32 done;
    int row, col;

    if ( !dump_open ) {
        return;
    }
    for ( row = 0; row < scr_rows; row++ ) {
        for ( col = 0; col < scr_cols && col < 132; col++ ) {
            line[col] = dump_char( want[row * scr_cols + col] );
        }
        while ( col > 0 && line[col - 1] == ' ' ) {
            col--;
        }
        line[col++] = '\r';
        line[col++] = '\n';
        sys_write( dump_handle, line, (u32)col, &done );
    }
    col = sfmt( line, sizeof( line ), "[cursor %u,%u %s]\r\n", (u32)cur_row, (u32)cur_col,
                cur_kind == CUR_BLOCK ? "block" : cur_kind == CUR_HIDE ? "hidden" : "line" );
    sys_write( dump_handle, line, (u32)col, &done );
}

/* test mode: a graphics screen as a .BMP file beside the log - the
   log's name with two digits in place of its extension - and a line in
   the log to say so */
void scr_shot( void )
{
    char path[PATH_MAX + 8], line[PATH_MAX + 40];
    u32 done;
    int len, dot, ok;

    if ( !dump_open || !gfx_on ) {
        return;
    }
    str_cpy( path, dump_path );
    len = (int)str_len( path );
    for ( dot = len; dot > 0 && path[dot - 1] != '.' && path[dot - 1] != '\\'; dot-- ) {
    }
    if ( dot > 0 && path[dot - 1] == '.' ) {
        len = dot - 1;
    }
    shots++;
    sfmt( path + len, 8, "%u%u.BMP", (u32)(shots / 10 % 10), (u32)(shots % 10) );
    ok = gfx_snapshot( path );
    len = sfmt( line, sizeof( line ), "[picture %s%s]\r\n", path, ok ? "" : " NOT WRITTEN" );
    sys_write( dump_handle, line, (u32)len, &done );
}
