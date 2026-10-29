/*
 * CONS.C - the program's own screen in text, its keyboard, and the PRINT
 * family's layout.
 *
 * THE OUTPUT SCREEN IS KEPT HERE, in memory, page by page: a program
 * writes cells into the page it is working on, and what changed is put
 * on the real screen (21h/F0h AL=4) when the program waits for a key,
 * ends, or every few hundred statements.  The IDE draws over the same
 * screen and puts this one back with con_show().  In a graphics mode the
 * characters are kept here too - SCREEN(r, c) reads them back - and
 * GFX.C draws each one in the picture.
 *
 * THE SCROLLING AREA is VIEW PRINT's, rows 1 to 24 unless told: the
 * last row never scrolls, and a new line on it scrolls the area and
 * leaves the cursor on the area's last row.
 *
 * PRINT'S LAYOUT is the same for the screen, the printer and a file
 * (PRINT #): zones of 14 columns but never one that would not have 14
 * left, an item that does not fit on the rest of the line moved whole
 * to the next, TAB and SPC taken modulo the width.  The three differ
 * only in where the characters go, which out_*() say.
 *
 * THE KEYS a program has not read yet are kept in a queue of their own,
 * filled whenever anyone looks: that is how Ctrl+Break is seen while a
 * program runs and never reads the keyboard, and how ON KEY takes its
 * keys before INKEY$ can.
 */
#include "qb.h"

#define MAX_PAGES       8
#define KEYQ            64

static u16 *pages[MAX_PAGES];
static int  npages = 1;
static int  apage, vpage;
static int  rows = 25, cols = 80;
static int  crow = 1, ccol = 1;         /* the cursor, 1-based */
static int  page_row[MAX_PAGES], page_col[MAX_PAGES];  /* other pages' */
static int  view_top = 1, view_bot = 24;
static int  view_set;                   /* VIEW PRINT given */
static int  wrapped;                    /* the last character filled a line */
static u8   attr = 0x07;
static int  fg = 7, bg = 0;
static int  cursor_on;                  /* LOCATE ,,1 */
static int  hidden;                     /* the editor is up until something is written */
static int  cur_start = -1, cur_stop = -1;
static u32  dirty_lo = 0xFFFFFFFFUL, dirty_hi;
static int  keyline;                    /* KEY ON */
static STR *softkeys[13];                /* 1-10 F1-F10, 11 and 12 F11 and F12 (KEY 30, 31) */

static u16  keyq[KEYQ];
static u8   keyshift[KEYQ];          /* the shift keys and locks with each */
static int  kq_head, kq_count;
static int  last_scan;
static int  shift_seen;
static int  in_scan = -1;               /* INPUT$: an extended key's scan code, next */

int  gfx_text_attr( int fgc, int bgc );
void gfx_set_colors( int fgc, int bgc );

/* ------------------------------------------------------------------ */
/* pages                                                               */
/* ------------------------------------------------------------------ */

/* SCREEN 0 has 8 pages, 4 when the screen has more than 25 lines */
static int text_pages( int new_rows, int new_cols )
{
    return new_cols == 40 || new_rows <= 25 ? 8 : 4;
}

static int alloc_pages( int count, int new_rows, int new_cols )
{
    int i;
    u32 cells = (u32)new_rows * (u32)new_cols, c;
    u16 *made[MAX_PAGES];

    /* the new pages first: without page 0 the old ones stay */
    for ( i = 0; i < count && i < MAX_PAGES; i++ ) {
        made[i] = (u16 *)try_alloc( cells * sizeof( u16 ) );
        if ( made[i] == NULL ) {
            if ( i == 0 ) {
                return 0;
            }
            break;                      /* fewer pages, not none */
        }
        for ( c = 0; c < cells; c++ ) {
            made[i][c] = 0x0720;
        }
    }
    count = i;
    for ( i = 0; i < MAX_PAGES; i++ ) {
        xfree( pages[i] );
        pages[i] = i < count ? made[i] : NULL;
        page_row[i] = page_col[i] = 1;
    }
    /* what the old size made dirty is no measure of the new */
    dirty_lo = 0xFFFFFFFFUL;
    dirty_hi = 0;
    npages = count;
    rows = new_rows;
    cols = new_cols;
    apage = vpage = 0;
    return 1;
}

static void mark( u32 first, u32 count )
{
    if ( first < dirty_lo ) {
        dirty_lo = first;
    }
    if ( first + count > dirty_hi ) {
        dirty_hi = first + count;
    }
}

static void mark_all( void )
{
    mark( 0, (u32)rows * (u32)cols );
}

/* one cell of the working page; in a graphics mode the picture too */
static void put_cell( int row, int col, int ch, int cell_attr )
{
    u32 at = (u32)(row - 1) * (u32)cols + (u32)(col - 1);

    if ( row < 1 || row > rows || col < 1 || col > cols ) {
        return;                                 /* off the screen: nowhere to go */
    }
    pages[apage][at] = (u16)((cell_attr << 8) | (ch & 0xFF));
    if ( gfx_mode() ) {
        gfx_text_cell( row, col, ch, cell_attr );
    } else if ( apage == vpage ) {
        mark( at, 1 );
    }
}

void con_flush( void )
{
    if ( hidden ) {
        if ( gfx_mode() ? !gfx_dirty() : dirty_lo >= dirty_hi ) {
            return;
        }
        hidden = 0;
        con_show();
        return;
    }
    if ( gfx_mode() ) {
        gfx_flush();
        dirty_lo = 0xFFFFFFFFUL;
        dirty_hi = 0;
        return;
    }
    if ( dirty_hi > (u32)rows * (u32)cols ) {
        dirty_hi = (u32)rows * (u32)cols;
    }
    if ( dirty_lo < dirty_hi ) {
        sys_put_cells( dirty_lo, pages[vpage] + dirty_lo, dirty_hi - dirty_lo );
    }
    dirty_lo = 0xFFFFFFFFUL;
    dirty_hi = 0;
    if ( cursor_on ) {
        sys_cursor( crow - 1, ccol - 1 );
    }
}

/* the whole output screen put back, as F4 and a running program want it */
void con_show( void )
{
    int srows, scols;

    if ( gfx_mode() ) {
        gfx_show();
        return;
    }
    sys_screen_size( &srows, &scols );
    if ( srows != rows || scols != cols ) {
        if ( cols == 40 ) {
            sys_text_mode( 1, rows );
        } else {
            sys_set_lines( rows );
        }
    }
    mark_all();
    con_flush();
    con_cursor_visible( cursor_on );
}

/* An Immediate line about to run: the editor stays on the screen until
   the line writes something there - a line that only sets a variable
   never shows the output screen.  con_output_shown() says whether it did. */
void con_show_on_output( void )
{
    hidden = 1;
}

int con_output_shown( void )
{
    int shown = !hidden;

    hidden = 0;
    return shown;
}

/* QBASIC leaving: DOS gets the output screen back in text, the cursor
   showing where the program's was.  A picture cannot go back to DOS:
   the screen is cleared instead. */
void con_exit( void )
{
    u16 blank[132];
    int row, col, srows, scols;

    if ( gfx_mode() ) {
        gfx_restore();
        sys_screen_size( &srows, &scols );
        if ( scols > 132 ) {
            scols = 132;
        }
        for ( col = 0; col < scols; col++ ) {
            blank[col] = 0x0720;
        }
        for ( row = 0; row < srows; row++ ) {
            sys_put_cells( (u32)row * (u32)scols, blank, (u32)scols );
        }
        sys_cursor_shape( CUR_LINE );
        sys_cursor( 0, 0 );
        return;
    }
    con_show();
    sys_cursor_shape( CUR_LINE );
    sys_cursor( crow - 1, ccol - 1 );
}

void con_cursor_visible( int on )
{
    if ( gfx_mode() ) {
        return;
    }
    if ( on ) {
        if ( cur_start >= 0 ) {
            sys_cursor_lines( cur_start, cur_stop );
        } else {
            sys_cursor_shape( CUR_LINE );
        }
        sys_cursor( crow - 1, ccol - 1 );
    } else {
        sys_cursor_shape( CUR_HIDE );
    }
}

/* the screen QBASIC was started on is the first output screen */
void con_capture_dos( void )
{
    int srows, scols, row, col;

    sys_screen_size( &srows, &scols );
    if ( scols != 40 && scols != 80 ) {
        scols = 80;
    }
    if ( srows != 25 && srows != 43 && srows != 50 ) {
        srows = 25;
    }
    if ( !alloc_pages( text_pages( srows, scols ), srows, scols ) ) {
        return;
    }
    sys_get_cells( 0, pages[0], (u32)srows * (u32)scols );
    sys_get_cursor( &row, &col );
    crow = row + 1;
    ccol = col + 1;
    if ( crow > srows ) {
        crow = srows;
    }
    if ( ccol > scols ) {
        ccol = scols;                   /* a wider screen than QBasic keeps */
    }
    view_top = 1;
    view_bot = srows - 1;
}

/* a run starting: SCREEN 0, the colours and the view as at the start,
   the text where it was */
void con_reset( void )
{
    if ( pages[0] == NULL ) {
        alloc_pages( text_pages( 25, 80 ), 25, 80 );
    }
    if ( gfx_mode() ) {
        gfx_reset();
        alloc_pages( text_pages( 25, 80 ), 25, 80 );
        crow = ccol = 1;
    }
    draw_reset();
    if ( apage != 0 ) {
        page_row[apage] = crow;
        page_col[apage] = ccol;
        crow = page_row[0];
        ccol = page_col[0];
    }
    if ( vpage != 0 ) {
        mark_all();
    }
    wrapped = 0;
    fg = 7;
    bg = 0;
    attr = 0x07;
    view_top = 1;
    view_bot = rows - 1;
    view_set = 0;
    cursor_on = 0;
    cur_start = cur_stop = -1;
    keyline = 0;
    apage = vpage = 0;
    kq_count = 0;
    if ( crow > rows ) {
        crow = rows;
    }
    in_scan = -1;
    if ( ccol > cols ) {
        ccol = cols;
    }
}

/* the geometry a graphics mode gives the text; GFX.C calls it */
void con_set_geometry( int new_rows, int new_cols, int new_pages )
{
    alloc_pages( new_pages, new_rows, new_cols );
    crow = ccol = 1;
    view_top = 1;
    view_bot = rows - 1;
    view_set = 0;
}

int con_rows( void )
{
    return rows;
}

int con_cols( void )
{
    return cols;
}

int con_width( void )
{
    return cols;
}

int con_col( void )
{
    return ccol;
}

int con_row( void )
{
    return crow;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

/* rows top..bottom up by one, the last one blank in the current colour */
static void scroll_up( int top, int bottom )
{
    u16 *page = pages[apage];
    u32 from = (u32)top * (u32)cols, to = (u32)(top - 1) * (u32)cols;
    u32 count = (u32)(bottom - top) * (u32)cols, i;
    u16 blank = (u16)((attr << 8) | ' ');

    if ( bottom > top ) {
        mem_cpy( page + to, page + from, count * sizeof( u16 ) );
    }
    for ( i = 0; i < (u32)cols; i++ ) {
        page[(u32)(bottom - 1) * (u32)cols + i] = blank;
    }
    if ( gfx_mode() ) {
        gfx_scroll_text( top, bottom, attr );
    } else if ( apage == vpage ) {
        mark( to, count + (u32)cols );
    }
}

void con_newline( void )
{
    if ( wrapped == 2 ) {
        scroll_up( view_top, view_bot );        /* the scroll a full last line owed */
    }
    wrapped = 0;
    ccol = 1;
    if ( crow < view_bot ) {
        crow++;
        return;
    }
    if ( crow == view_bot ) {
        scroll_up( view_top, view_bot );
        return;
    }
    /* below the area (row 25): the area scrolls, the cursor goes to its
       last row */
    scroll_up( view_top, view_bot );
    crow = view_bot;
}

static void clear_rows( int top, int bottom )
{
    u16 *page = pages[apage];
    u32 i, from = (u32)(top - 1) * (u32)cols, to = (u32)bottom * (u32)cols;
    u16 blank = (u16)((attr << 8) | ' ');

    for ( i = from; i < to; i++ ) {
        page[i] = blank;
    }
    if ( gfx_mode() ) {
        gfx_clear_text( top, bottom, attr );
    } else if ( apage == vpage ) {
        mark( from, to - from );
    }
}

void con_putc( int ch )
{
    ch &= 0xFF;
    switch ( ch ) {
    case 7:
        con_beep();
        return;
    case 9:
        do {
            con_putc( ' ' );
        } while ( (ccol - 1) % 8 != 0 && ccol != 1 );
        return;
    case 10:
    case 13:
        con_newline();
        return;
    case 11:
        crow = view_top;
        ccol = 1;
        wrapped = 0;
        return;
    case 12:
        clear_rows( view_top, view_bot );
        crow = view_top;
        ccol = 1;
        wrapped = 0;
        return;
    case 28:
        if ( ccol < cols ) {
            ccol++;
        }
        wrapped = 0;
        return;
    case 29:
        if ( ccol > 1 ) {
            ccol--;
        }
        wrapped = 0;
        return;
    case 30:
        if ( crow > view_top ) {
            crow--;
        }
        wrapped = 0;
        return;
    case 31:
        if ( crow < view_bot ) {
            crow++;
        }
        wrapped = 0;
        return;
    }
    if ( ch == 0 ) {
        ch = ' ';                       /* CHR$(0) prints as a blank */
    }
    if ( wrapped == 2 ) {
        scroll_up( view_top, view_bot );
    }
    wrapped = 0;
    put_cell( crow, ccol, ch, attr );
    ccol++;
    if ( ccol > cols ) {
        /* the line is full: the cursor goes to the next line at once, and
           a PRINT that ends there does not move it again.  On the last
           line the scroll waits for the next character or newline, and a
           LOCATE before then cancels it. */
        ccol = 1;
        if ( crow >= view_bot ) {
            crow = view_bot;
            wrapped = 2;
        } else {
            crow++;
            wrapped = 1;
        }
    }
}

void con_write( const char *text, u32 len )
{
    while ( len-- ) {
        con_putc( (u8)*text++ );
    }
}

void con_beep( void )
{
    extern void snd_beep( void );

    snd_beep();
}

/* ------------------------------------------------------------------ */
/* PRINT's channel: the screen, the printer, or a file                 */
/* ------------------------------------------------------------------ */

static int chan;                        /* 0 screen, -1 printer, n file */

int  file_col( int num );
int  file_width( int num );
void file_text( int num, const char *text, u32 len );
void file_newline( int num );
void file_check_output( int num );
int  lpt_col( void );
int  lpt_width( void );
void lpt_text( const char *text, u32 len );
void lpt_newline( void );

static int out_col( void )
{
    return chan == 0 ? ccol : chan < 0 ? lpt_col() : file_col( chan );
}

static int out_width( void )
{
    return chan == 0 ? cols : chan < 0 ? lpt_width() : file_width( chan );
}

static void out_text( const char *text, u32 len )
{
    if ( chan == 0 ) {
        con_write( text, len );
    } else if ( chan < 0 ) {
        lpt_text( text, len );
    } else {
        file_text( chan, text, len );
    }
}

static void out_newline( void )
{
    if ( chan == 0 ) {
        if ( wrapped && ccol == 1 ) {
            /* the line wrapped already: only an owed scroll is left */
            if ( wrapped == 2 ) {
                scroll_up( view_top, view_bot );
            }
            wrapped = 0;
            return;
        }
        con_newline();
    } else if ( chan < 0 ) {
        lpt_newline();
    } else {
        file_newline( chan );
    }
}

static void out_spaces( u32 count )
{
    static const char blanks[] = "                                ";

    while ( count > 0 ) {
        u32 n = count > 32 ? 32 : count;
        out_text( blanks, n );
        count -= n;
    }
}

/* an item: moved whole to the next line when it does not fit the rest
   of this one (and would fit a line) */
static void out_item( const char *text, u32 len )
{
    int width = out_width(), col = out_col();

    if ( width > 0 && col > 1 && (u32)(col - 1) + len > (u32)width && len <= (u32)width ) {
        out_newline();
    }
    out_text( text, len );
}

void rtf_PRCHAN( void )
{
    chan = (int)POP_L();
    if ( chan > 0 ) {
        file_check_output( chan );
    }
}

static void print_number( const char *digits )
{
    char buf[48];
    u32 len;

    if ( digits[0] == '-' ) {
        str_cpy( buf, digits );
    } else {
        buf[0] = ' ';
        str_cpy( buf + 1, digits );
    }
    len = str_len( buf );
    buf[len++] = ' ';
    out_item( buf, len );
}

void rtf_PRNUML( void )
{
    char digits[40];

    num_fmt_l( POP_L(), digits );
    print_number( digits );
}

void rtf_PRNUMS( void )
{
    char digits[40];

    num_fmt_s( pop_single(), digits );
    print_number( digits );
}

void rtf_PRNUMD( void )
{
    char digits[40];

    num_fmt_d( POP_D(), digits );
    print_number( digits );
}

void rtf_PRSTR( void )
{
    STR *s = POP_STR();

    out_item( QS_TEXT( s ), QS_LEN( s ) );
    qs_release( s );
}

/* to the next zone of 14 that has 14 columns left, or the next line */
void rtf_PRCOMMA( void )
{
    int col = out_col(), width = out_width(), next;

    if ( chan == 0 && wrapped ) {
        out_newline();                  /* a comma after a full line: no move */
        return;
    }
    next = ((col - 1) / 14 + 1) * 14 + 1;
    if ( width > 0 && next + 14 > width + 1 ) {
        out_newline();
        return;
    }
    out_spaces( (u32)(next - col) );
}

void rtf_PRTAB( void )
{
    s32 n = POP_L();
    int width = out_width(), col = out_col();

    if ( chan == 0 || chan < 0 ) {
        if ( n > width ) {
            n = width ? n % width : n;
        }
    }
    if ( n < 1 ) {
        n = 1;
    }
    if ( n < col ) {
        out_newline();
        col = 1;
    }
    out_spaces( (u32)(n - col) );
}

void rtf_PRSPC( void )
{
    s32 n = POP_L();
    int width = out_width();

    if ( n < 0 ) {
        n = 0;
    }
    if ( width > 0 ) {
        n %= width;
    }
    out_spaces( (u32)n );
}

void rtf_PREOL( void )
{
    out_newline();
}

/* WRITE's forms: numbers with no blanks, strings in quotes, commas */
static void write_number( const char *digits )
{
    out_item( digits, str_len( digits ) );
}

void rtf_WRNUML( void )
{
    char digits[40];

    num_fmt_l( POP_L(), digits );
    write_number( digits );
}

void rtf_WRNUMS( void )
{
    char digits[40];

    num_fmt_s( pop_single(), digits );
    write_number( digits );
}

void rtf_WRNUMD( void )
{
    char digits[40];

    num_fmt_d( POP_D(), digits );
    write_number( digits );
}

void rtf_WRSTR( void )
{
    STR *s = POP_STR();

    out_text( "\"", 1 );
    out_text( QS_TEXT( s ), QS_LEN( s ) );
    out_text( "\"", 1 );
    qs_release( s );
}

void rtf_WRSEP( void )
{
    out_text( ",", 1 );
}

/* PRINT USING: the format, then each value through it */
static STR *using_fmt;
static int  using_pos;
static int  using_any;

void con_print_push( PRINTSTATE *ps )
{
    ps->chan = chan;
    ps->fmt = using_fmt;
    ps->pos = using_pos;
    ps->any = using_any;
    chan = 0;
    using_fmt = NULL;
    using_pos = 0;
    using_any = 0;
}

void con_print_pop( PRINTSTATE *ps )
{
    qs_release( using_fmt );
    chan = ps->chan;
    using_fmt = ps->fmt;
    using_pos = ps->pos;
    using_any = ps->any;
    ps->fmt = NULL;
}

void rtf_PRUSING( void )
{
    qs_release( using_fmt );
    using_fmt = POP_STR();
    using_pos = 0;
    using_any = 0;
    if ( QS_LEN( using_fmt ) == 0 ) {
        vm_error( E_ILLEGAL_CALL );
    }
}

STR *using_number( STR *fmt, int *pos, double v, int is_double );
STR *using_string( STR *fmt, int *pos, STR *s );
STR *using_tail( STR *fmt, int *pos );

static void using_out( STR *piece )
{
    out_text( QS_TEXT( piece ), QS_LEN( piece ) );
    qs_release( piece );
    using_any = 1;
}

void rtf_PRUNUMS( void )
{
    double v = num_sng( pop_single() );     /* the SINGLE itself, not the wider value */

    using_out( using_number( using_fmt, &using_pos, v, 0 ) );
}

void rtf_PRUNUMD( void )
{
    double v = POP_D();

    using_out( using_number( using_fmt, &using_pos, v, 1 ) );
}

void rtf_PRUSTR( void )
{
    STR *s = POP_STR();

    using_out( using_string( using_fmt, &using_pos, s ) );
}

void rtf_PRUEND( void )
{
    int eol = (int)POP_L();

    if ( using_any ) {
        /* literal text after the last field is written too */
        using_out( using_tail( using_fmt, &using_pos ) );
    }
    qs_release( using_fmt );
    using_fmt = NULL;
    if ( eol ) {
        out_newline();
    }
}

/* ------------------------------------------------------------------ */
/* the keyboard                                                        */
/* ------------------------------------------------------------------ */

static void key_push( int key, int shift )
{
    if ( (key & 0xFF) == 0xE0 && (key >> 8) != 0 ) {
        key &= 0xFF00;                          /* a grey key: as the white one */
    }
    if ( kq_count < KEYQ ) {
        keyq[(kq_head + kq_count) % KEYQ] = (u16)key;
        keyshift[(kq_head + kq_count) % KEYQ] = (u8)shift;
        kq_count++;
    }
}

/* the keyboard into the queue; Ctrl+Break stops the program */
void con_poll_keys( void )
{
    int shift, key, waiting;
    extern int mach_ring_key( void );

    while ( (key = mach_ring_key()) >= 0 ) {
        key_push( key, 0 );                     /* typed into 40:1E by the program */
    }
    for ( ;; ) {
        waiting = sys_key_wait( shift_seen, &shift, KW_NOWAIT );
        shift_seen = shift & SH_KEYS;
        if ( waiting != 1 ) {
            return;
        }
        key = sys_key_read( &shift );
        if ( key == 0x0003 ) {
            kq_count = 0;
            vm_break();
        }
        last_scan = (key >> 8) & 0x7F;
        key_push( key, shift );
    }
}

int con_key_waiting( void )
{
    return kq_count;
}

int con_last_scan( void )
{
    return last_scan;
}

void con_flush_keys( void )
{
    kq_count = 0;
}

static int key_is_trapped( int key, int shift );

/* the next key: scan << 8 | character, or -1 with "wait" 0 */
int con_key( int wait )
{
    int key;

    for ( ;; ) {
        con_poll_keys();
        if ( kq_count > 0 && !wait ) {
            /* INKEY$: a key ON KEY traps is left for the trap, which runs
               at the next statement */
            int i, j;
            for ( i = 0; i < kq_count; i++ ) {
                if ( !key_is_trapped( keyq[(kq_head + i) % KEYQ], keyshift[(kq_head + i) % KEYQ] ) ) {
                    break;
                }
            }
            if ( i == kq_count ) {
                vm_poll_now();
                return -1;
            }
            key = keyq[(kq_head + i) % KEYQ];
            for ( j = i; j > 0; j-- ) {
                keyq[(kq_head + j) % KEYQ] = keyq[(kq_head + j - 1) % KEYQ];
                keyshift[(kq_head + j) % KEYQ] = keyshift[(kq_head + j - 1) % KEYQ];
            }
            kq_head = (kq_head + 1) % KEYQ;
            kq_count--;
            if ( i > 0 ) {
                vm_poll_now();
            }
            return key;
        }
        if ( kq_count > 0 ) {
            key = keyq[kq_head];
            kq_head = (kq_head + 1) % KEYQ;
            kq_count--;
            return key;
        }
        if ( !wait ) {
            return -1;
        }
        snd_service();
        {
            int shift;
            /* wait for a key, but come back for the music now and then */
            sys_key_wait( shift_seen, &shift, snd_queued() > 0 ? KW_NOWAIT : 0 );
            shift_seen = shift & SH_KEYS;
        }
    }
}

/* KEY(n) trapping: the key numbers of the function and cursor keys */
static int trap_number( int key )
{
    int scan = (key >> 8) & 0xFF;

    if ( (key & 0xFF) != 0 ) {
        return 0;
    }
    if ( scan >= 0x3B && scan <= 0x44 ) {
        return scan - 0x3B + 1;
    }
    switch ( scan ) {
    case 0x48:  return 11;
    case 0x4B:  return 12;
    case 0x4D:  return 13;
    case 0x50:  return 14;
    case 0x85:  return 30;
    case 0x86:  return 31;
    }
    return 0;
}

static u8 user_trap_scan[26], user_trap_shift[26];

/* KEY n, CHR$(flags) + CHR$(scan): either Shift (1-3), Ctrl (4), Alt (8),
   NumLock (20h) and CapsLock (40h) must be as given - with CapsLock on, a
   key defined without 40h is not trapped, as in QBasic */
static int shift_matches( int want, int have )
{
    return ((want & SH_SHIFT) != 0) == ((have & SH_SHIFT) != 0)
        && (want & 0x6C) == (have & 0x6C);
}

/* is the key one an armed KEY(n) traps? */
static int key_is_trapped( int key, int shift )
{
    int n = trap_number( key ), j;

    if ( n != 0 && event_key_armed( n ) ) {
        return 1;
    }
    for ( j = 15; j <= 25; j++ ) {
        if ( event_key_armed( j ) && user_trap_scan[j] && user_trap_scan[j] == ((key >> 8) & 0xFF)
          && shift_matches( user_trap_shift[j], shift ) ) {
            return 1;
        }
    }
    return 0;
}

/* a trapped key out of the queue: -> 1 with its KEY number */
int con_take_trapped_key( int *which, const u8 *armed )
{
    int i, n, j, key;

    con_poll_keys();
    for ( i = 0; i < kq_count; i++ ) {
        key = keyq[(kq_head + i) % KEYQ];
        n = trap_number( key );
        if ( n == 0 || !armed[n] ) {
            n = 0;
            for ( j = 15; j <= 25; j++ ) {
                if ( armed[j] && user_trap_scan[j] && user_trap_scan[j] == ((key >> 8) & 0xFF)
                  && shift_matches( user_trap_shift[j], keyshift[(kq_head + i) % KEYQ] ) ) {
                    n = j;
                    break;
                }
            }
        }
        if ( n != 0 ) {
            for ( j = i; j < kq_count - 1; j++ ) {
                keyq[(kq_head + j) % KEYQ] = keyq[(kq_head + j + 1) % KEYQ];
                keyshift[(kq_head + j) % KEYQ] = keyshift[(kq_head + j + 1) % KEYQ];
            }
            kq_count--;
            *which = n;
            return 1;
        }
    }
    return 0;
}

void rtf_INKEY( void )
{
    int key;
    char buf[2];

    con_flush();
    key = con_key( 0 );
    if ( key < 0 ) {
        PUSH_STR( NULL );
        return;
    }
    if ( (key & 0xFF) != 0 ) {
        buf[0] = (char)(key & 0xFF);
        PUSH_STR( qs_make( buf, 1 ) );
    } else {
        buf[0] = 0;
        buf[1] = (char)(key >> 8);
        PUSH_STR( qs_make( buf, 2 ) );
    }
}

/* INPUT$(n): n characters, not shown */
void rtf_INPUTS( void )
{
    s32 n = POP_L();
    STR *s;
    s32 i;
    int key;

    if ( n < 1 || n > STR_MAX ) {
        vm_error( E_ILLEGAL_CALL );
    }
    con_flush();
    s = qs_new( (u32)n );
    PUSH_STR( s );                              /* on the stack: Ctrl+Break while waiting frees it */
    for ( i = 0; i < n; i++ ) {
        if ( in_scan >= 0 ) {
            s->text[i] = (char)in_scan;
            in_scan = -1;
            continue;
        }
        key = con_key( 1 );
        s->text[i] = (char)(key & 0xFF);
        if ( (key & 0xFF) == 0 ) {
            in_scan = (key >> 8) & 0xFF;
        }
    }
}

/* ------------------------------------------------------------------ */
/* INPUT's line                                                        */
/* ------------------------------------------------------------------ */

/* A line typed at the cursor: overtype, Ins for insert, Esc empties it,
   Enter ends it.  -> its length.  The line may run on over rows. */
int con_read_line( char *buf, int max, int is_input )
{
    int len = 0, pos = 0, insert = 0, key, ch, i, row0, col0, bottom;

    (void)is_input;
    if ( wrapped == 2 ) {
        scroll_up( view_top, view_bot );        /* the scroll a full last line owed */
    }
    wrapped = 0;
    row0 = crow;
    col0 = ccol;
    /* a long line scrolls the view up under it, or (begun below the view,
       on the line VIEW PRINT leaves out) stops at the screen's end */
    bottom = row0 <= view_bot ? view_bot : rows;
    con_flush();
    if ( !gfx_mode() ) {
        con_cursor_visible( 1 );
    }
    for ( ;; ) {
        /* the cursor where the editing point is */
        {
            int at = col0 - 1 + pos;
            crow = row0 + at / cols;
            ccol = at % cols + 1;
            if ( crow > rows ) {
                crow = rows;
            }
            con_flush();
            if ( !gfx_mode() ) {
                sys_cursor( crow - 1, ccol - 1 );
            }
        }
        key = con_key( 1 );
        ch = key & 0xFF;
        if ( ch == 13 ) {
            break;
        }
        if ( ch == 27 ) {
            for ( i = 0; i < len; i++ ) {
                int at = col0 - 1 + i;
                put_cell( row0 + at / cols, at % cols + 1, ' ', attr );
            }
            len = pos = 0;
            continue;
        }
        if ( ch == 8 ) {
            if ( pos > 0 ) {
                pos--;
                mem_cpy( buf + pos, buf + pos + 1, (u32)(len - pos - 1) );
                len--;
                for ( i = pos; i <= len; i++ ) {
                    int at = col0 - 1 + i;
                    put_cell( row0 + at / cols, at % cols + 1, i < len ? (u8)buf[i] : ' ', attr );
                }
            }
            continue;
        }
        if ( ch == 0 ) {
            switch ( key >> 8 ) {
            case 0x4B:  if ( pos > 0 ) { pos--; } break;
            case 0x4D:  if ( pos < len ) { pos++; } break;
            case 0x47:  pos = 0; break;
            case 0x4F:  pos = len; break;
            case 0x52:  insert = !insert; break;
            case 0x53:
                if ( pos < len ) {
                    mem_cpy( buf + pos, buf + pos + 1, (u32)(len - pos - 1) );
                    len--;
                    for ( i = pos; i <= len; i++ ) {
                        int at = col0 - 1 + i;
                        put_cell( row0 + at / cols, at % cols + 1, i < len ? (u8)buf[i] : ' ', attr );
                    }
                }
                break;
            }
            continue;
        }
        if ( ch < 32 && ch != 9 ) {
            continue;
        }
        if ( (insert || pos >= len) && len < max ) {
            /* the line gets longer: its new end must be on the screen,
               after the view has scrolled it up as far as it can */
            int at = col0 - 1 + len;
            int can_scroll = bottom == view_bot && row0 > view_top ? row0 - view_top : 0;
            if ( row0 + at / cols - can_scroll > rows ) {
                con_beep();
                continue;
            }
        }
        if ( insert && len < max ) {
            mem_cpy( buf + pos + 1, buf + pos, (u32)(len - pos) );
            buf[pos] = (char)ch;
            len++;
        } else if ( pos < len ) {
            buf[pos] = (char)ch;
        } else if ( len < max ) {
            buf[len++] = (char)ch;
        } else {
            con_beep();
            continue;
        }
        /* the line may need the screen to scroll under it */
        {
            int at = col0 - 1 + len - 1;
            while ( row0 + at / cols > bottom && bottom == view_bot && row0 > view_top ) {
                scroll_up( view_top, view_bot );
                row0--;
            }
        }
        for ( i = pos; i < len; i++ ) {
            int at = col0 - 1 + i;
            put_cell( row0 + at / cols, at % cols + 1, (u8)buf[i], attr );
        }
        pos++;
    }
    {
        int at = col0 - 1 + len;
        crow = row0 + at / cols;
        ccol = at % cols + 1;
        if ( crow > rows ) {
            crow = rows;
        }
    }
    if ( !gfx_mode() ) {
        con_cursor_visible( cursor_on );
    }
    buf[len] = 0;
    return len;
}

/* ------------------------------------------------------------------ */
/* INPUT                                                               */
/* ------------------------------------------------------------------ */

#define MAX_ITEMS   64

static char  in_line[256];
static struct {
    u16 start, len;
    u8  quoted;
} in_items[MAX_ITEMS];
static int   in_count, in_next;
static int   in_file;           /* INPUT #: the file, else 0 */

/* the line split into items at commas outside quotes */
static int split_items( const char *line, int len, int want )
{
    int pos = 0, count = 0, start, end;

    for ( ;; ) {
        while ( pos < len && line[pos] == ' ' ) {
            pos++;
        }
        if ( count >= MAX_ITEMS ) {
            return -1;
        }
        in_items[count].quoted = 0;
        if ( pos < len && line[pos] == '"' ) {
            start = ++pos;
            while ( pos < len && line[pos] != '"' ) {
                pos++;
            }
            end = pos;
            if ( pos < len ) {
                pos++;
            }
            while ( pos < len && line[pos] == ' ' ) {
                pos++;
            }
            if ( pos < len && line[pos] != ',' ) {
                return -1;                      /* something after the quotes */
            }
            in_items[count].quoted = 1;
        } else {
            start = pos;
            while ( pos < len && line[pos] != ',' ) {
                pos++;
            }
            end = pos;
            while ( end > start && line[end - 1] == ' ' ) {
                end--;
            }
        }
        in_items[count].start = (u16)start;
        in_items[count].len = (u16)(end - start);
        count++;
        if ( pos >= len ) {
            break;
        }
        pos++;                                  /* the comma */
    }
    (void)want;
    return count;
}

/* is item i acceptable as type c ('I', 'L', 'S', 'D', '$')? 0 = redo,
   -1 = overflow */
static int item_ok( int i, int c )
{
    double val;
    int type;
    const char *text = in_line + in_items[i].start;
    u32 len = in_items[i].len;

    if ( c == '$' ) {
        return 1;
    }
    if ( in_items[i].quoted ) {
        return 0;
    }
    if ( len == 0 ) {
        return 1;
    }
    if ( !num_scan( text, len, &val, &type ) ) {
        return 0;
    }
    switch ( c ) {
    case 'I':
        val = fp_round( val );
        return (val >= -32768.0 && val <= 32767.0) ? 1 : -1;
    case 'L':
        val = fp_round( val );
        return (val >= -2147483648.0 && val <= 2147483647.0) ? 1 : -1;
    case 'S':
        return (val <= SNG_MAX && val >= -SNG_MAX) ? 1 : -1;
    }
    return 1;
}

/* INPUT: the prompt, its flags, the variables' types: the line is read
   and checked until it is right, before anything is stored */
void rtf_INBEGIN( void )
{
    STR *sig = POP_STR();
    int flags = (int)POP_L(), len, i, count, nsig = (int)QS_LEN( sig ), r;
    STR *prompt = POP_STR();

    in_file = 0;
    for ( ;; ) {
        con_write( QS_TEXT( prompt ), QS_LEN( prompt ) );
        if ( flags & 2 ) {
            con_write( "? ", 2 );
        }
        len = con_read_line( in_line, 255, 1 );
        if ( len == 0 ) {
            count = 0;                          /* nothing typed: 0s and empty strings */
            break;
        }
        count = split_items( in_line, len, nsig );
        r = count == nsig ? 1 : 0;
        for ( i = 0; r > 0 && i < nsig; i++ ) {
            r = item_ok( i, sig->text[i] );
        }
        if ( r > 0 ) {
            break;
        }
        con_newline();                          /* the messages come after a blank line */
        con_newline();
        if ( r < 0 ) {
            con_write( "Overflow", 8 );
            con_newline();
        }
        con_write( "Redo from start", 15 );
        con_newline();
    }
    if ( !(flags & 1) ) {
        con_newline();
    }
    in_count = count;
    in_next = 0;
    qs_release( prompt );
    qs_release( sig );
}

int  file_input_item( int num, char *buf, int max, int *quoted, int want_string );

void rtf_INFILE( void )
{
    in_file = (int)POP_L();
    if ( in_file <= 0 ) {
        vm_error( E_BAD_FILENUM );
    }
}

/* a line or an item from a file, up to a string's length: the heap's,
   taken the first time and kept */
#define ITEM_MAX    32767

static char *item_buf( void )
{
    static char *buf;

    if ( buf == NULL ) {
        buf = (char *)try_alloc( ITEM_MAX + 1 );
        if ( buf == NULL ) {
            vm_error( E_OUT_OF_MEMORY );
        }
    }
    return buf;
}

/* the next item's text: from the line read, or from the file */
static const char *next_item( u32 *len, int *quoted, int want_string )
{
    if ( in_file ) {
        char *fbuf = item_buf();
        int n = file_input_item( in_file, fbuf, ITEM_MAX, quoted, want_string );
        *len = (u32)n;
        return fbuf;
    }
    if ( in_next >= in_count ) {
        *len = 0;
        *quoted = 0;
        return "";
    }
    *len = in_items[in_next].len;
    *quoted = in_items[in_next].quoted;
    return in_line + in_items[in_next++].start;
}

static double item_value( int *type )
{
    u32 len;
    int quoted;
    const char *text = next_item( &len, &quoted, 0 );
    double val;
    u32 used;

    if ( len == 0 ) {
        *type = TY_INT;
        return 0;
    }
    /* a file's number ends where a number can: "12abc" is 12 */
    val = num_parse( text, len, &used );
    *type = TY_DOUBLE;
    return val;
}

void rtf_INITEMI( void )
{
    s16 *var = (s16 *)POP_A();
    int type;
    double v = fp_round( item_value( &type ) );

    if ( v < -32768.0 || v > 32767.0 ) {
        vm_error( E_OVERFLOW );
    }
    *var = (s16)v;
}

void rtf_INITEML( void )
{
    s32 *var = (s32 *)POP_A();
    int type;
    double v = fp_round( item_value( &type ) );

    if ( v < -2147483648.0 || v > 2147483647.0 ) {
        vm_error( E_OVERFLOW );
    }
    *var = (s32)v;
}

void rtf_INITEMS( void )
{
    float *var = (float *)POP_A();
    int type;
    double v = item_value( &type );

    if ( v > SNG_MAX || v < -SNG_MAX ) {
        vm_error( E_OVERFLOW );
    }
    *var = (float)v;
}

void rtf_INITEMD( void )
{
    double *var = (double *)POP_A();
    int type;

    *var = item_value( &type );
}

void rtf_INITEMSTR( void )
{
    STR **var = (STR **)POP_A();
    u32 len;
    int quoted;
    const char *text = next_item( &len, &quoted, 1 );
    STR *old = *var;

    *var = qs_make( text, len );
    qs_release( old );
}

void rtf_INITEMFS( void )
{
    u32 width = (u32)POP_L();
    char *field = (char *)POP_A();
    u32 len, n;
    int quoted;
    const char *text = next_item( &len, &quoted, 1 );

    n = len < width ? len : width;
    mem_cpy( field, text, n );
    mem_set( field + n, ' ', width - n );
}

/* LINE INPUT: prompt, flags, the variable and its width (0 = a STRING) */
void rtf_LINEIN( void )
{
    u32 width = (u32)POP_L();
    void *var = POP_A();
    int flags = (int)POP_L();
    STR *prompt = POP_STR();
    int len;

    con_write( QS_TEXT( prompt ), QS_LEN( prompt ) );
    qs_release( prompt );
    len = con_read_line( in_line, 255, 0 );
    if ( !(flags & 1) ) {
        con_newline();
    }
    if ( width == 0 ) {
        STR **slot = (STR **)var, *old = *slot;
        *slot = qs_make( in_line, (u32)len );
        qs_release( old );
    } else {
        u32 n = (u32)len < width ? (u32)len : width;
        mem_cpy( var, in_line, n );
        mem_set( (char *)var + n, ' ', width - n );
    }
}

int file_line_input( int num, char *buf, int max );

void rtf_LINEINF( void )
{
    u32 width = (u32)POP_L();
    void *var = POP_A();
    int num = (int)POP_L();
    char *buf = item_buf();
    int len = file_line_input( num, buf, ITEM_MAX );

    if ( width == 0 ) {
        STR **slot = (STR **)var, *old = *slot;
        *slot = qs_make( buf, (u32)len );
        qs_release( old );
    } else {
        u32 n = (u32)len < width ? (u32)len : width;
        mem_cpy( var, buf, n );
        mem_set( (char *)var + n, ' ', width - n );
    }
}

/* RANDOMIZE with nothing after it asks for the seed */
void rtf_RANDASK( void )
{
    extern void randomize( double seed );
    char line[256];
    double val;
    int type, len;

    for ( ;; ) {
        con_write( "Random-number seed (-32768 to 32767)? ", 38 );
        len = con_read_line( line, 255, 1 );
        con_newline();
        if ( num_scan( line, (u32)len, &val, &type ) && val >= -32768.0 && val <= 32767.0 ) {
            break;
        }
        con_write( "Redo from start", 15 );
        con_newline();
    }
    randomize( fp_round( val ) );
}

/* ------------------------------------------------------------------ */
/* the screen statements                                               */
/* ------------------------------------------------------------------ */

static void need( int ok )
{
    if ( !ok ) {
        vm_error( E_ILLEGAL_CALL );
    }
}

static void draw_keyline( void );

void rtf_CLS( void )
{
    s32 how = POP_L();

    need( how >= -1 && how <= 2 );
    if ( gfx_mode() ) {
        /* no argument: the graphics viewport if there is one, else the
           text one, else the screen */
        if ( how == -1 ) {
            how = gfx_view_on() ? 1 : view_set ? 2 : 0;
        }
        if ( how != 2 ) {
            gfx_cls( (int)how );
        }
        if ( how == 1 ) {
            return;
        }
    }
    if ( how == 1 ) {
        crow = view_top;                /* no graphics viewport to clear */
    } else if ( how == 2 || (how == -1 && view_set) ) {
        clear_rows( view_top, view_bot );
        crow = view_top;
    } else {
        clear_rows( 1, rows );
        if ( keyline ) {
            draw_keyline();                     /* the key line comes back after a CLS */
        }
        crow = view_top;
    }
    ccol = 1;
    wrapped = 0;
}

/* LOCATE row, col, cursor, start, stop: the mask of those given on top */
void rtf_LOCATE( void )
{
    int mask = (int)POP_L();
    s32 stop = POP_L(), start = POP_L(), cursor = POP_L(), col = POP_L(), row = POP_L();

    if ( mask & 1 ) {
        if ( view_set ) {
            need( row >= view_top && row <= view_bot );
        } else {
            need( row >= 1 && row <= rows && !(keyline && row == rows) );
        }
        crow = (int)row;
        wrapped = 0;
    }
    if ( mask & 2 ) {
        need( col >= 1 && col <= cols );
        ccol = (int)col;
        wrapped = 0;
    }
    if ( mask & 4 ) {
        need( cursor == 0 || cursor == 1 );
        cursor_on = (int)cursor;
    }
    if ( mask & 8 ) {
        need( start >= 0 && start <= 31 );
        cur_start = (int)start;
        cur_stop = (mask & 16) ? (int)stop : (int)start;
        need( cur_stop >= 0 && cur_stop <= 31 );
    }
    if ( mask & (4 | 8) ) {
        con_cursor_visible( cursor_on );
    }
}

void rtf_COLOR( void )
{
    int mask = (int)POP_L();
    s32 border = POP_L(), b = POP_L(), f = POP_L();

    (void)border;
    if ( gfx_mode() ) {
        if ( mask & 1 ) {
            fg = (int)f;
        }
        if ( mask & 2 ) {
            bg = (int)b;
        }
        gfx_set_colors( mask & 1 ? (int)f : -1, mask & 2 ? (int)b : -1 );
        attr = (u8)gfx_text_attr( fg, bg );
        return;
    }
    if ( mask & 1 ) {
        need( f >= 0 && f <= 31 );
        fg = (int)f;
    }
    if ( mask & 2 ) {
        need( b >= 0 && b <= 15 );
        bg = (int)b & 7;
    }
    if ( mask & 4 ) {
        need( border >= 0 && border <= 15 );
    }
    attr = (u8)(((bg & 7) << 4) | (fg & 15) | ((fg & 16) ? 0x80 : 0));
}

int  gfx_width( int new_cols, int new_rows );

/* WIDTH cols, rows: the mask of those given on top */
void rtf_WIDTH( void )
{
    int mask = (int)POP_L();
    s32 r = POP_L(), c = POP_L();
    int new_cols = mask & 1 ? (int)c : cols, new_rows = mask & 2 ? (int)r : rows;

    if ( gfx_mode() ) {
        need( gfx_width( new_cols, new_rows ) );
        return;
    }
    need( (new_cols == 40 || new_cols == 80) && (new_rows == 25 || new_rows == 43 || new_rows == 50) );
    if ( new_cols == cols && new_rows == rows ) {
        return;
    }
    if ( !sys_text_mode( new_cols == 40 ? 1 : 3, new_rows ) ) {
        /* the adapter cannot: back to the size there was */
        sys_text_mode( cols == 40 ? 1 : 3, rows );
        mark_all();
        vm_error( E_ILLEGAL_CALL );
    }
    if ( !alloc_pages( text_pages( new_rows, new_cols ), new_rows, new_cols ) ) {
        sys_text_mode( cols == 40 ? 1 : 3, rows );
        mark_all();
        vm_error( E_OUT_OF_MEMORY );
    }
    crow = ccol = 1;
    view_top = 1;
    view_bot = rows - 1;
    view_set = 0;
    mark_all();
}

void rtf_VIEWPRINT( void )
{
    s32 bottom = POP_L(), top = POP_L();
    int given = (int)POP_L();

    if ( !given ) {
        view_top = 1;
        view_bot = rows - 1;
        view_set = 0;
    } else {
        need( top >= 1 && bottom <= rows && top <= bottom );
        view_top = (int)top;
        view_bot = (int)bottom;
        view_set = 1;
    }
    crow = view_top;
    ccol = 1;
    wrapped = 0;
}

void rtf_POS( void )
{
    (void)POP_L();
    PUSH_L( ccol > cols ? cols : ccol );
}

void rtf_CSRLIN( void )
{
    PUSH_L( crow );
}

/* SCREEN(row, col [, 1]): a character or its colour */
void rtf_SCREENF( void )
{
    int given = (int)POP_L();
    s32 want_attr = POP_L(), col = POP_L(), row = POP_L();
    u16 cell;

    (void)given;
    need( row >= 1 && row <= rows && col >= 1 && col <= cols );
    cell = pages[apage][(u32)(row - 1) * (u32)cols + (u32)(col - 1)];
    if ( want_attr ) {
        PUSH_L( gfx_mode() ? 0 : cell >> 8 );
    } else {
        PUSH_L( cell & 0xFF );
    }
}

/* text pages for SCREEN's apage and vpage, from GFX.C in SCREEN 0 */
int con_set_pages( int active, int visual )
{
    if ( active < 0 || active >= npages || visual < 0 || visual >= npages ) {
        return 0;
    }
    if ( active != apage ) {
        /* each page keeps a cursor of its own */
        page_row[apage] = crow;
        page_col[apage] = ccol;
        crow = page_row[active];
        ccol = page_col[active];
        wrapped = 0;
    }
    apage = active;
    if ( visual != vpage ) {
        vpage = visual;
        mark_all();
    }
    return 1;
}

int con_page_count( void )
{
    return npages;
}

/* PCOPY in text */
int con_copy_page( int from, int to )
{
    if ( from < 0 || from >= npages || to < 0 || to >= npages ) {
        return 0;
    }
    mem_cpy( pages[to], pages[from], (u32)rows * (u32)cols * sizeof( u16 ) );
    if ( to == vpage ) {
        mark_all();
    }
    return 1;
}

/* one cell of a text page, for PEEK and POKE at B800 */
u16 *con_page_cells( int page )
{
    return page >= 0 && page < npages ? pages[page] : NULL;
}

void con_page_changed( int page, u32 first, u32 count )
{
    if ( page == vpage && !gfx_mode() ) {
        mark( first, count );
    }
}

/* ------------------------------------------------------------------ */
/* the function keys' line                                             */
/* ------------------------------------------------------------------ */

static void draw_keyline( void )
{
    int k, i, col;
    STR *s;

    if ( !keyline ) {
        return;
    }
    for ( col = 1; col <= cols; col++ ) {
        put_cell( rows, col, ' ', 0x07 );
    }
    for ( k = 1; k <= 10; k++ ) {
        col = (k - 1) * (cols / 10) + 1;
        if ( col > cols ) {
            break;
        }
        put_cell( rows, col, k == 10 ? '0' : '0' + k, 0x07 );
        s = softkeys[k];
        for ( i = 0; i < 6 && i < (int)QS_LEN( s ) && col + 1 + i <= cols; i++ ) {
            int ch = (u8)s->text[i];
            put_cell( rows, col + 1 + i, ch == 13 ? 27 : ch, 0x70 );
        }
    }
}

void rtf_KEYONOFF( void )
{
    int how = (int)POP_L(), k;
    char num[8];

    if ( how == 2 ) {
        /* F1 to F12, each name in four columns */
        for ( k = 1; k <= 12; k++ ) {
            con_write( "F", 1 );
            num_fmt_l( k, num );
            con_write( num, str_len( num ) );
            con_write( "  ", k < 10 ? 2 : 1 );
            if ( softkeys[k] ) {
                con_write( softkeys[k]->text, softkeys[k]->len > 15 ? 15 : softkeys[k]->len );
            }
            con_newline();
        }
        return;
    }
    keyline = how;
    if ( keyline ) {
        draw_keyline();
    } else {
        int col;
        for ( col = 1; col <= cols; col++ ) {
            put_cell( rows, col, ' ', attr );
        }
    }
}

/* KEY n, string$: a soft key's text (1-10, 30, 31) or a trapped key's
   definition (15-25: shift flags, scan code) */
void rtf_KEYSET( void )
{
    STR *s = POP_STR();
    s32 n = POP_L();

    if ( n >= 15 && n <= 25 ) {
        if ( QS_LEN( s ) != 2 ) {
            qs_release( s );
            vm_error( E_ILLEGAL_CALL );
        }
        user_trap_shift[n] = (u8)s->text[0];
        user_trap_scan[n] = (u8)s->text[1];
        qs_release( s );
        return;
    }
    if ( !((n >= 1 && n <= 10) || n == 30 || n == 31) ) {
        qs_release( s );
        vm_error( E_ILLEGAL_CALL );
    }
    if ( QS_LEN( s ) > 15 ) {
        /* QBasic keeps the first 15 characters */
        STR *cut = qs_make( s->text, 15 );
        qs_release( s );
        s = cut;
    }
    if ( n > 10 ) {
        n -= 19;                                /* 30, 31: F11, F12 */
    }
    qs_release( softkeys[n] );
    softkeys[n] = s;
    if ( n <= 10 ) {
        draw_keyline();
    }
}

void rtf_LPOS( void )
{
    (void)POP_L();
    PUSH_L( lpt_col() );
}

/* PRINT USING with a format that has no field: its text is written,
   and then it is an error */
void con_text_then_error( STR *text, int code )
{
    out_text( QS_TEXT( text ), QS_LEN( text ) );
    qs_release( text );
    vm_error( code );
}

/* after SHELL: what the child left on the screen is the output screen */
void con_after_shell( void )
{
    int srows, scols, row, col;

    if ( gfx_mode() ) {
        gfx_show();
        return;
    }
    sys_screen_size( &srows, &scols );
    if ( srows == rows && scols == cols ) {
        sys_get_cells( 0, pages[vpage], (u32)rows * (u32)cols );
        sys_get_cursor( &row, &col );
        crow = row + 1;
        ccol = col + 1;
    } else {
        mark_all();
    }
}

/* ------------------------------------------------------------------ */
/* the BIOS data area's screen fields, as the program's screen has them */
/* ------------------------------------------------------------------ */

/* a byte of 0040:0049-0066 or 0040:0084-0086 (off is from 0400h), or
   -1 for one this does not answer; MACH.C's PEEK asks */
int con_bda_byte( u32 off )
{
    static const u8 bios_mode[14] = { 3, 4, 6, 0, 0, 0, 0, 0x0D, 0x0E, 0x10, 0x0F, 0x11, 0x12, 0x13 };
    int mode = gfx_mode(), page, word = -1;
    u32 page_size = (u32)rows * (u32)cols * 2;

    page_size = page_size <= 2048 ? 2048 : page_size <= 4096 ? 4096 : 8192;
    switch ( off ) {
    case 0x49:
        return mode ? bios_mode[mode] : cols == 40 ? 1 : 3;
    case 0x4A: case 0x4B:
        word = cols;
        break;
    case 0x4C: case 0x4D:
        word = mode ? 0 : (int)page_size;
        break;
    case 0x4E: case 0x4F:
        word = mode ? 0 : (int)(page_size * (u32)vpage);
        break;
    case 0x60:
        return cur_stop >= 0 ? cur_stop : 7;
    case 0x61:
        return cur_start >= 0 ? cur_start : 6;
    case 0x62:
        return vpage;
    case 0x63: case 0x64:
        word = 0x3D4;
        break;
    case 0x84:
        return rows - 1;
    case 0x85: case 0x86:
        word = rows > 25 ? 8 : 16;
        break;
    default:
        if ( off >= 0x50 && off < 0x60 ) {
            /* each page's cursor, column then row, from 0 */
            page = (int)(off - 0x50) / 2;
            if ( page == apage ) {
                return (off & 1) ? crow - 1 : (ccol > cols ? cols : ccol) - 1;
            }
            if ( page < npages ) {
                return (off & 1) ? page_row[page] - 1 : page_col[page] - 1;
            }
            return 0;
        }
        return -1;
    }
    return (off & 1) ? (word >> 8) & 0xFF : word & 0xFF;
}

/* "Press any key to continue" at the start of the bottom row of the
   screen being shown, when a run from the editor ends: the program's
   cursor and colours are left as they were */
void con_end_prompt( const char *text )
{
    int save_page = apage, save_row = crow, save_col = ccol, save_wrap = wrapped, gpage = -1;
    u8 save_attr = attr;

    if ( pages[0] == NULL ) {
        return;
    }
    apage = vpage;
    if ( gfx_mode() ) {
        gpage = gfx_draw_on_visible();
    }
    wrapped = 0;                                /* the program's full last line owes no scroll here */
    crow = rows;
    ccol = 1;
    attr = gfx_mode() ? (u8)gfx_text_attr( 15, 0 ) : 0x07;
    while ( *text && ccol < cols ) {
        con_putc( (u8)*text++ );
    }
    apage = save_page;
    if ( gpage >= 0 ) {
        gfx_draw_on( gpage );
    }
    crow = save_row;
    ccol = save_col;
    wrapped = save_wrap;
    attr = save_attr;
    con_flush();
}
