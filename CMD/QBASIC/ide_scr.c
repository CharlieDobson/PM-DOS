/*
 * IDE_SCR.C - the editor's screen in memory, the colours, the keyboard
 * and the mouse.  EDIT's SCR.C (see it for how the screen is sent: only
 * the runs of cells that changed), with QBasic's colours and two things
 * EDIT has no need of:
 *
 *   The program's screen.  A running program draws through CON.C and
 *   GFX.C straight to the hardware, so when the editor comes back what
 *   it last sent is not what is there: scr_invalidate() says so, and the
 *   next flush sends every cell.
 *
 *   Alt held down.  QBasic shows the menu bar's access letters while Alt
 *   is down and nothing else has been pressed; key_alt_held() says when.
 *
 * TEST MODE is EDIT's: with QBTEST naming a log file and standard input
 * a file, the keys come from the file (TOOLS\MKEDKEYS.PL writes them).
 */
#include "ide.h"

PALETTE pal;
int scr_rows = 25, scr_cols = 80;

static u16 *want;               /* the screen as it should be */
static u16 *shown;              /* the screen as it was last given */
static int  cur_row, cur_col, cur_kind = CUR_LINE;
static int  shown_row = -1, shown_col = -1, shown_kind = -1;
static int  lines_set;

/* ------------------------------------------------------------------ */
/* colours: QBasic's own                                               */
/* ------------------------------------------------------------------ */

void scr_palette( void )
{
    pal.text = 0x17;            /* grey on blue */
    pal.select = 0x71;          /* blue on grey */
    pal.frame = 0x17;
    pal.title = 0x71;
    pal.current = 0x1F;         /* bright white on blue */
    pal.bkpt = 0x47;            /* grey on red */
    pal.bkpt_current = 0x4F;
    pal.scroll = 0x70;
    pal.thumb = 0x07;
    pal.menu = 0x70;            /* black on grey */
    pal.menu_hot = 0x7F;        /* bright white on grey */
    pal.menu_sel = 0x07;        /* grey on black */
    pal.menu_sel_hot = 0x0F;
    pal.menu_off = 0x78;        /* dark grey on grey */
    pal.status = 0x3F;          /* bright white on cyan */
    pal.status_right = 0x30;    /* black on cyan */
    pal.field_sel = 0x07;
    pal.shadow = 0x08;
    pal.help = 0x07;
    pal.help_bold = 0x0F;
    pal.help_link = 0x0A;
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

void scr_init( int lines )
{
    u32 cells;

    sys_screen_size( &scr_rows, &scr_cols );
    if ( lines > scr_rows ) {
        sys_set_lines( lines );
        lines_set = 1;
        sys_screen_size( &scr_rows, &scr_cols );
    }
    if ( scr_rows > 60 ) {
        scr_rows = 60;
    }
    if ( scr_cols != 80 ) {
        sys_text_mode( 3, scr_rows > 25 ? 50 : 25 );     /* the editor is 80 wide */
        sys_screen_size( &scr_rows, &scr_cols );
    }
    cells = (u32)scr_rows * scr_cols;
    want = (u16 *)xalloc( cells * 2 );
    shown = (u16 *)xalloc( cells * 2 );
    mem_set( want, 0, cells * 2 );
    mem_set( shown, 0xFF, cells * 2 );      /* nothing shown matches */
    scr_palette();
}

void scr_done( void )
{
    mouse_done();
    if ( lines_set ) {
        sys_set_lines( 25 );
    }
    sys_cursor_shape( CUR_LINE );
}

void scr_invalidate( void )
{
    int rows, cols;

    sys_screen_size( &rows, &cols );
    if ( rows != scr_rows || cols != scr_cols ) {
        sys_text_mode( 3, scr_rows );
        sys_screen_size( &rows, &cols );
        if ( rows > 60 ) {
            rows = 60;
        }
        if ( rows != scr_rows || cols != scr_cols ) {
            /* the BIOS cannot make the editor's size again (28, 30, 34 or
               60 rows from the start mode): the editor takes the one made */
            u32 cells = (u32)rows * (u32)cols;
            u16 *new_want = (u16 *)try_alloc( cells * 2 ), *new_shown = (u16 *)try_alloc( cells * 2 );
            if ( new_want != NULL && new_shown != NULL ) {
                xfree( want );
                xfree( shown );
                want = new_want;
                shown = new_shown;
                mem_set( want, 0, cells * 2 );
                scr_rows = rows;
                scr_cols = cols;
                app_layout();
            } else {
                xfree( new_want );
                xfree( new_shown );
            }
        }
    }
    mem_set( shown, 0xFF, (u32)scr_rows * scr_cols * 2 );
    shown_row = shown_col = shown_kind = -1;
}

u16 scr_cell( int row, int col )
{
    if ( row >= 0 && row < scr_rows && col >= 0 && col < scr_cols ) {
        return want[row * scr_cols + col];
    }
    return 0;
}

void scr_ch( int row, int col, int ch, u8 attr )
{
    if ( row >= 0 && row < scr_rows && col >= 0 && col < scr_cols ) {
        want[row * scr_cols + col] = (u16)((attr << 8) | (ch & 0xFF));
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

/* "&File": the character after '&' in hot_attr, the '&' not shown */
void scr_hot( int row, int col, const char *text, u8 attr, u8 hot_attr )
{
    int hot = 0;

    for ( ; *text; text++ ) {
        if ( *text == '&' && !hot ) {
            hot = 1;
            continue;
        }
        scr_ch( row, col++, (u8)*text, hot == 1 ? hot_attr : attr );
        if ( hot == 1 ) {
            hot = 2;
        }
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

    if ( row < 0 || row >= scr_rows ) {
        return;
    }
    for ( pos = col; pos < col + len; pos++ ) {
        if ( pos >= 0 && pos < scr_cols ) {
            want[row * scr_cols + pos] = (u16)((want[row * scr_cols + pos] & 0xFF) | (attr << 8));
        }
    }
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

void scr_flush( void )
{
    int row, first, last;
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
        sys_put_cells( base + first, want + base + first, (u32)(last - first + 1) );
        mem_cpy( shown + base + first, want + base + first, (u32)(last - first + 1) * 2 );
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

/* ------------------------------------------------------------------ */
/* the keyboard                                                        */
/* ------------------------------------------------------------------ */

static int shift_seen;
static int alt_armed;
static int testing;
static int test_shift;
static u32 dump_handle;
static int dump_open;

/* ------------------------------------------------------------------ */
/* the mouse: EDIT's (see its SCR.C)                                   */
/* ------------------------------------------------------------------ */

#define MQ_SIZE         16
#define DOUBLE_TIME     40      /* hundredths: a second press in this */

MEVENT mouse;

static int    mouse_on, mouse_hidden;
static int    mouse_xmax = 639, mouse_ymax = 199;
static MEVENT mqueue[MQ_SIZE];
static int    mq_head, mq_tail;
static int    m_row = -1, m_col = -1, m_down;
static int    last_row = -1, last_col = -1;
static u32    last_time;
static int    click_here;

static int mouse_next( void );

static void to_cell( int x, int y, int *row, int *col )
{
    *col = (int)((long)x * scr_cols / (mouse_xmax + 1));
    *row = (int)((long)y * scr_rows / (mouse_ymax + 1));
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
        now = testing ? 0 : sys_hundredths();
        if ( row == last_row && col == last_col
             && (testing ? click_here : now - last_time < DOUBLE_TIME) ) {
            kind = ME_DOUBLE;
            last_row = -1;
        } else {
            last_row = row;
            last_col = col;
        }
        last_time = now;
    }
    if ( next == mq_head ) {
        return;
    }
    mqueue[mq_tail].kind = kind;
    mqueue[mq_tail].row = row;
    mqueue[mq_tail].col = col;
    mqueue[mq_tail].shift = key_shift();
    mq_tail = next;
}

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
    to_cell( x, y, &row, &col );
    to_cell( px, py, &prow, &pcol );
    to_cell( rx, ry, &rrow, &rcol );
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

void mouse_init( void )
{
    int x, y;

    if ( sys_mouse_reset() == 0 ) {
        return;
    }
    mouse_on = 1;
    sys_mouse_extent( &mouse_xmax, &mouse_ymax );
    if ( mouse_xmax < 1 || mouse_ymax < 1 ) {
        mouse_xmax = 639;
        mouse_ymax = 199;
    }
    sys_mouse_state( &x, &y, &m_down );
    m_down &= 1;
    sys_mouse_count( 0, 0, &x, &y );
    sys_mouse_count( 1, 0, &x, &y );
    sys_mouse_show( 1 );
}

void mouse_done( void )
{
    if ( mouse_on ) {
        sys_mouse_reset();
        mouse_on = 0;
    }
}

/* the pointer off while a program has the screen, and back after: the
   driver is told again of the screen's size, which the program may have
   changed */
void mouse_hide( int hide )
{
    int x, y;

    if ( !mouse_on || hide == mouse_hidden ) {
        return;
    }
    mouse_hidden = hide;
    if ( hide ) {
        sys_mouse_show( 0 );
        return;
    }
    sys_mouse_extent( &mouse_xmax, &mouse_ymax );
    if ( mouse_xmax < 1 || mouse_ymax < 1 ) {
        mouse_xmax = 639;
        mouse_ymax = 199;
    }
    sys_mouse_count( 0, 0, &x, &y );
    sys_mouse_count( 1, 0, &x, &y );
    sys_mouse_show( 1 );
}

int mouse_present( void )
{
    return mouse_on;
}

int mouse_held( void )
{
    return m_down;
}

void mouse_where( int *row, int *col )
{
    int x, y, buttons;

    if ( mouse_on && !testing ) {
        sys_mouse_state( &x, &y, &buttons );
        to_cell( x, y, row, col );
        return;
    }
    *row = m_row;
    *col = m_col;
}

static void test_mouse( int buttons, int row, int col )
{
    int was_up = !m_down, prow = m_row, pcol = m_col;

    if ( !mouse_on ) {
        return;
    }
    sys_mouse_move( col * (mouse_xmax + 1) / scr_cols, row * (mouse_ymax + 1) / scr_rows );
    sys_mouse_inject( buttons );
    mouse_poll();
    if ( was_up && (buttons & 1) == 0 ) {
        click_here = 0;
    }
    if ( !was_up && (buttons & 1) == 0 && prow == row && pcol == col ) {
        click_here = 1;
    }
}

void key_init( void )
{
    const char *log = sys_getenv( "QBTEST" );

    if ( log && *log && !sys_stdin_is_console() ) {
        testing = 1;
        if ( sys_create( log, &dump_handle ) == 0 ) {
            dump_open = 1;
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

int key_alt_held( void )
{
    return alt_armed;
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

    if ( raw == K_BREAK ) {
        return K_BREAK;
    }
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
            if ( sys_getenv( "QBHOLD" ) ) {
                testing = 0;
                return key_get();
            }
            return K_EOF;
        }
        if ( byte == 0xFE ) {
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
        before = test_shift;
        test_shift = next & SH_KEYS;
        alt_armed = test_shift == SH_ALT;
        if ( before == SH_ALT && test_shift == 0 ) {
            alt_armed = 0;
            return K_ALTTAP;
        }
        if ( alt_armed != (before == SH_ALT) ) {
            return K_TICK;                      /* the bar's letters change */
        }
    }
}

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
   K_TICK when neither came.  Alt going down alone also comes back as
   K_TICK, so the bar can show its letters. */
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
        flags = mouse_on && !mouse_hidden ? KW_MOUSE : 0;
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
            shift_seen = shift;
            if ( alt_armed ) {
                return K_TICK;
            }
        } else if ( !(shift & SH_ALT) && (shift_seen & SH_ALT) && alt_armed ) {
            alt_armed = 0;
            shift_seen = shift;
            return K_ALTTAP;
        } else if ( shift & ~SH_ALT ) {
            if ( alt_armed ) {
                alt_armed = 0;
                shift_seen = shift;
                return K_TICK;
            }
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

int key_get_timed( int hundredths )
{
    return key_wait( testing ? -1 : hundredths );
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

/* test mode: the screen as text into the log, and where the cursor is */
void scr_dump( void )
{
    char line[140], num[12];
    u32 done;
    int row, col;

    if ( !dump_open ) {
        return;
    }
    for ( row = 0; row < scr_rows; row++ ) {
        for ( col = 0; col < scr_cols && col < 132; col++ ) {
            line[col] = (char)(want[row * scr_cols + col] & 0xFF);
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
    str_cpy( line, "[cursor " );
    num_fmt_l( cur_row, num );
    str_catn( line, num, sizeof( line ) );
    str_catn( line, ",", sizeof( line ) );
    num_fmt_l( cur_col, num );
    str_catn( line, num, sizeof( line ) );
    str_catn( line, cur_kind == CUR_BLOCK ? " block]\r\n" : cur_kind == CUR_HIDE ?
              " hidden]\r\n" : " line]\r\n", sizeof( line ) );
    sys_write( dump_handle, line, str_len( line ), &done );
}

/* test mode: the program's screen as the display has it, text modes only */
void scr_dump_output( void )
{
    u8 cells[132 * 2], cols = 80, rows = 24;
    u16 start = 0;
    char line[140];
    u32 done;
    int row, col;

    if ( !dump_open ) {
        return;
    }
    sys_peek( 0x44A, &cols, 1 );
    sys_peek( 0x484, &rows, 1 );
    sys_peek( 0x44E, &start, 2 );
    if ( cols == 0 || cols > 132 ) {
        cols = 80;
    }
    if ( rows == 0 || rows > 59 ) {
        rows = 24;
    }
    for ( row = 0; row <= rows; row++ ) {
        if ( !sys_peek( 0xB8000UL + start + (u32)row * cols * 2, cells, (u32)cols * 2 ) ) {
            break;
        }
        for ( col = 0; col < cols; col++ ) {
            line[col] = (char)(cells[col * 2] < ' ' ? '.' : cells[col * 2]);
        }
        while ( col > 0 && line[col - 1] == ' ' ) {
            col--;
        }
        line[col++] = '\r';
        line[col++] = '\n';
        sys_write( dump_handle, line, (u32)col, &done );
    }
    sys_write( dump_handle, "[output screen]\r\n", 17, &done );
}

void scr_cursor_get( int *row, int *col, int *kind )
{
    *row = cur_row;
    *col = cur_col;
    *kind = cur_kind;
}
