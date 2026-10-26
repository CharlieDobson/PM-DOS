/*
 * SCR.C - XARC's screen in memory, the keyboard and the mouse.
 *
 * THE SCREEN IS CELLS, the toolkit's grid (DOSForms' SCREEN.HPP): every
 * df_* call writes into "want", and df_flush sends what differs from
 * "shown" - to the kernel as characters and colours on a text screen
 * (21h/F0h AL=4), or to GFX.C cell by cell on the graphics one.  A box
 * that covers part of the screen saves the cells under it and puts them
 * back (UI.C's POPUP), as the toolkit's did, so an icon or a slice of a
 * progress bar comes back with them.  ev_wait flushes before it waits,
 * so whatever was drawn is on the glass before anyone can answer it.
 *
 * THE TEXT SCREEN IS THE ONE THE PROGRAM WAS STARTED ON - 25 lines, or
 * 43 or 50 - and is put back as it was found.  The graphics one is VGA
 * mode 12h: eighty columns of thirty lines.
 *
 * THE KEYBOARD is 21h/F0h AL=7 to 9: a key is its ASCII code, or 100h
 * and the scan code for an extended one, as the toolkit's getch pairs
 * were; Alt pressed and let go with nothing typed is KEY_ALT, which is
 * how the menu bar is reached without a mouse.
 *
 * THE MOUSE is INT 33h.  An event is a change in the buttons - left and
 * right, the right one marks a row - and 05h and 06h are asked on every
 * look as well as 03h, so a click that is over between two looks is
 * still a press and a release.  21h/F0h AL=8 is told to wake for the
 * mouse too, so nothing here spins.  The graphics screen's pointer is
 * drawn by GFX.C; the driver's own stays hidden there.
 *
 * TEST MODE: with XATEST naming a log file and standard input taken from
 * a file, the keys come from the file (TOOLS\MKKEYS.PL writes them):
 * a byte is a key, 00h and a scan code an extended one, FFh FFh writes
 * the screen into the log as text, FFh FDh its colours as well, FFh FEh
 * a graphics screen as a .BMP beside the log, FFh and another byte sets
 * the shift state (FFh 08h then FFh 00h is a tap of Alt), and FEh with
 * three bytes is the mouse: the buttons, then a row and a column, which
 * INT 33h is told as if the mouse had done it.  A job that is running
 * (an extraction) takes no keys from the file unless XAPOLL is set;
 * when the file runs out every question is answered with Esc, which
 * ends the program, unless XAHOLD is set and the keyboard takes over.
 */
#include "xarc.h"

#define GRID_ROWS   60                  /* the text screen is held to these */
#define GRID_COLS   132
#define GRID_CELLS  ((u32)GRID_ROWS * GRID_COLS)

static int  rows = 25, cols = 80;
static int  line_cols = 80;             /* the text screen's own width */
static int  gfx;
static CELL *want, *shown;
static u16 *dos_screen;
static int  dos_rows, dos_cols, dos_row, dos_col, char_height = 16;

/* ------------------------------------------------------------------ */
/* the kernel's console                                                */
/* ------------------------------------------------------------------ */

static void text_size( int *nrows, int *ncols )
{
    REGS regs;

    xa_clear( &regs );
    xa_dos( &regs, 0xF003 );
    *nrows = (int)((regs.edx >> 8) & 0xFF);
    *ncols = (int)(regs.edx & 0xFF);
    if ( *nrows < 5 ) {
        *nrows = 25;
    }
    if ( *ncols < 40 ) {
        *ncols = 80;
    }
    line_cols = *ncols;
    char_height = (int)(regs.ecx & 0xFF);
    if ( char_height < 4 || char_height > 32 ) {
        char_height = 16;
    }
}

static void put_cells( u32 first, const u16 *cells, u32 count )
{
    REGS regs;

    xa_clear( &regs );
    regs.ebx = first;
    regs.ecx = count;
    regs.esi = (u32)cells;
    xa_dos( &regs, 0xF004 );
}

static void get_cells( u32 first, u16 *cells, u32 count )
{
    REGS regs;

    xa_clear( &regs );
    regs.ebx = first;
    regs.ecx = count;
    regs.edi = (u32)cells;
    xa_dos( &regs, 0xF005 );
}

static void cursor_shape( u32 shape )
{
    REGS regs;

    xa_clear( &regs );
    regs.ecx = shape;
    xa_dos( &regs, 0xF006 );
}

static void cursor_to( int row, int col )
{
    REGS regs;

    xa_clear( &regs );
    regs.edx = ((u32)row << 8) | (u32)col;
    xa_dos( &regs, 0xF002 );
}

/* ------------------------------------------------------------------ */
/* the grid                                                            */
/* ------------------------------------------------------------------ */

/* nothing on the glass is known: the next flush draws every cell */
static void grid_forget( void )
{
    u32 at;

    for ( at = 0; at < GRID_CELLS; at++ ) {
        shown[at] = 0xFFFFFFFFUL;
    }
}

/* a blank grid of the present size */
static void grid_reset( void )
{
    u32 cells = (u32)rows * cols, at;

    for ( at = 0; at < cells; at++ ) {
        want[at] = CELL_MAKE( ' ', 0x07 );
    }
    grid_forget();
}

/* Once, at the largest size either screen has, so that changing screens
   under a running program never has to ask for memory it might not get. */
static int grid_alloc( void )
{
    want = (CELL *)malloc( GRID_CELLS * sizeof( CELL ) );
    shown = (CELL *)malloc( GRID_CELLS * sizeof( CELL ) );
    if ( want == NULL || shown == NULL ) {
        free( want );
        free( shown );
        want = shown = NULL;
        return 0;
    }
    grid_reset();
    return 1;
}

int scr_open( int graphics )
{
    REGS regs;

    text_size( &dos_rows, &dos_cols );
    xa_clear( &regs );
    xa_dos( &regs, 0xF001 );
    dos_row = (int)((regs.edx >> 8) & 0xFF);
    dos_col = (int)(regs.edx & 0xFF);
    dos_screen = (u16 *)malloc( (u32)dos_rows * dos_cols * 2 );
    if ( dos_screen ) {
        get_cells( 0, dos_screen, (u32)dos_rows * dos_cols );
    }
    rows = dos_rows > GRID_ROWS ? GRID_ROWS : dos_rows;
    cols = dos_cols > GRID_COLS ? GRID_COLS : dos_cols;
    gfx = 0;
    if ( graphics && gfx_start() ) {
        gfx = 1;
        rows = GFX_ROWS;
        cols = 80;
    }
    if ( !grid_alloc() ) {
        if ( gfx ) {
            gfx_stop();                     /* the message goes on a text screen */
            gfx = 0;
        }
        return -1;
    }
    cursor_shape( 0x2000 );                 /* hidden, as XArc always had it */
    return gfx;
}

void scr_close( void )
{
    REGS regs;

    if ( ev_mouse_on() ) {
        xa_clear( &regs );
        regs.eax = 0;
        int33( &regs );                     /* a reset, which hides it */
    }
    if ( gfx ) {
        gfx_stop();
        gfx = 0;
    }
    if ( dos_screen ) {
        put_cells( 0, dos_screen, (u32)dos_rows * dos_cols );
    }
    cursor_shape( char_height >= 14 ? ((u32)(char_height - 3) << 8) | (u32)(char_height - 2)
                                    : ((u32)(char_height - 2) << 8) | (u32)(char_height - 1) );
    cursor_to( dos_row, dos_col );
}

/* The other screen, under a running program: the grid takes the new
   size and starts blank, and the mouse is told about it (see XARC.C). */
int scr_graphics( int on )
{
    if ( on == gfx ) {
        return 1;
    }
    if ( on ) {
        /* a refusal can come after a mode set, and the text mode that
           gfx_start then puts back is a blank screen */
        if ( !gfx_start() ) {
            grid_forget();
            return 0;
        }
        gfx = 1;
        rows = GFX_ROWS;
        cols = 80;
    } else {
        gfx_stop();
        gfx = 0;
        text_size( &rows, &cols );
        if ( rows > GRID_ROWS ) {
            rows = GRID_ROWS;
        }
        if ( cols > GRID_COLS ) {
            cols = GRID_COLS;
        }
        cursor_shape( 0x2000 );
    }
    grid_reset();
    ev_mouse_reset();
    return 1;
}

int df_rows( void )         { return rows; }
int df_cols( void )         { return cols; }
int df_is_graphics( void )  { return gfx; }
int df_has_icons( void )    { return gfx; }

static u8 attr_of( int fg, int bg )
{
    return (u8)((fg & 0x0F) | ((bg & 0x0F) << 4));
}

static CELL *cell_ptr( int row, int col )
{
    if ( !want || row < 1 || row > rows || col < 1 || col > cols ) {
        return NULL;
    }
    return &want[(u32)(row - 1) * cols + (col - 1)];
}

void df_text( int row, int col, const char *text, int fg, int bg )
{
    u8 attr = attr_of( fg, bg );
    CELL *at;

    if ( text == NULL ) {
        return;
    }
    for ( ; *text && col <= cols; text++, col++ ) {
        at = cell_ptr( row, col );
        if ( at ) {
            *at = CELL_MAKE( *text, attr );
        }
    }
}

/* background only, as the toolkit's was: the foreground of a blanked
   cell is whatever it was, which nothing can see */
void df_fill( int top, int left, int bottom, int right, int bg )
{
    int row, col;
    CELL *at;

    for ( row = top; row <= bottom; row++ ) {
        for ( col = left; col <= right; col++ ) {
            at = cell_ptr( row, col );
            if ( at ) {
                *at = CELL_MAKE( ' ', (CELL_ATTR( *at ) & 0x0F) | ((bg & 0x0F) << 4) );
            }
        }
    }
}

void df_clear( int fg, int bg )
{
    u32 at, cells = (u32)rows * cols;
    CELL blank = CELL_MAKE( ' ', attr_of( fg, bg ) );

    for ( at = 0; at < cells; at++ ) {
        want[at] = blank;
    }
}

void df_clear_row( int row, int fg, int bg )
{
    int col;
    CELL *at;
    CELL blank = CELL_MAKE( ' ', attr_of( fg, bg ) );

    for ( col = 1; col <= cols; col++ ) {
        at = cell_ptr( row, col );
        if ( at ) {
            *at = blank;
        }
    }
}

/* A drop shadow.  Text mode can darken nothing, so the toolkit's shadow
   has always been a black fill there; the graphics screen keeps what is
   under it and recolours it dark grey on black, the look the black was
   imitating.  An icon or a bar slice has no character to dim: blank. */
void df_shade( int top, int left, int bottom, int right )
{
    int row, col;
    CELL *at;

    if ( !gfx ) {
        df_fill( top, left, bottom, right, BG_BLACK );
        return;
    }
    for ( row = top; row <= bottom; row++ ) {
        for ( col = left; col <= right; col++ ) {
            at = cell_ptr( row, col );
            if ( at ) {
                *at = CELL_EXTRA( *at ) ? CELL_MAKE( ' ', 0x08 )
                                        : CELL_MAKE( CELL_CHAR( *at ), 0x08 );
            }
        }
    }
}

/* Text mode draws the bar in CP437 blocks; the graphics screen draws it
   a pixel at a time, eight steps to a cell, from the same call. */
void df_bar( int row, int col, int cells, u32 done, u32 total, int fg, int bg )
{
    u8 attr = attr_of( fg, bg );
    u32 pixels, filled, start;
    int at, here;
    u8 extra;
    CELL *cell;

    if ( cells < 1 ) {
        return;
    }
    if ( col + cells - 1 > cols ) {
        cells = cols - col + 1;
    }
    if ( cells < 1 ) {
        return;
    }
    /* how many of the cells (text) or pixels (graphics) are done */
    pixels = gfx ? (u32)cells * 8 : (u32)cells;
    if ( total == 0 ) {
        filled = 0;
    } else if ( done >= total ) {
        filled = pixels;
    } else {
        u64 prod = xa_mul32( pixels, done );
        xa_div64( &prod, total );
        filled = xa_lo32( prod );
    }
    if ( !gfx ) {
        for ( at = 0; at < cells; at++ ) {
            cell = cell_ptr( row, col + at );
            if ( cell ) {
                *cell = CELL_MAKE( (u32)at < filled ? 0xDB : 0xB0, attr );
            }
        }
        return;
    }
    for ( at = 0; at < cells; at++ ) {
        start = (u32)at * 8;
        here = filled <= start ? 0 : filled >= start + 8 ? 8 : (int)(filled - start);
        extra = (u8)(EXTRA_BAR | here);
        if ( at == 0 ) {
            extra |= EXTRA_BAR_LEFT;
        }
        if ( at == cells - 1 ) {
            extra |= EXTRA_BAR_RIGHT;
        }
        cell = cell_ptr( row, col + at );
        if ( cell ) {
            *cell = CELL_WITH( CELL_MAKE( 0, attr ), extra );
        }
    }
}

void df_icon( int row, int col, int icon, int bg )
{
    CELL *at = cell_ptr( row, col );

    if ( at == NULL || !gfx || icon < 0 || icon >= ICON_MAX ) {
        return;
    }
    *at = CELL_WITH( CELL_MAKE( 0, (CELL_ATTR( *at ) & 0x0F) | ((bg & 0x0F) << 4) ),
                     EXTRA_ICON( icon ) );
}

void df_cells_read( int top, int left, int bottom, int right, CELL *buf )
{
    int row, col;
    CELL *at;

    for ( row = top; row <= bottom; row++ ) {
        for ( col = left; col <= right; col++ ) {
            at = cell_ptr( row, col );
            *buf++ = at ? *at : CELL_MAKE( ' ', 0x07 );
        }
    }
}

void df_cells_write( int top, int left, int bottom, int right, const CELL *buf )
{
    int row, col;
    CELL *at;

    for ( row = top; row <= bottom; row++ ) {
        for ( col = left; col <= right; col++, buf++ ) {
            at = cell_ptr( row, col );
            if ( at ) {
                *at = *buf;
            }
        }
    }
}

CELL df_cell_at( int row, int col )
{
    CELL *at = cell_ptr( row, col );

    return at ? *at : CELL_MAKE( ' ', 0x07 );
}

void df_flush( void )
{
    static u16 run[132];
    int row, first, last, col;
    u32 base;

    if ( want == NULL ) {
        return;
    }
    if ( gfx ) {
        for ( row = 0; row < rows; row++ ) {
            base = (u32)row * cols;
            for ( col = 0; col < cols; col++ ) {
                if ( want[base + col] != shown[base + col] ) {
                    gfx_cell( row, col, want[base + col] );
                    shown[base + col] = want[base + col];
                }
            }
        }
        gfx_flush();
        return;
    }
    for ( row = 0; row < rows; row++ ) {
        base = (u32)row * cols;
        for ( first = 0; first < cols && want[base + first] == shown[base + first]; first++ ) {
        }
        if ( first == cols ) {
            continue;
        }
        for ( last = cols - 1; want[base + last] == shown[base + last]; last-- ) {
        }
        for ( col = first; col <= last; col++ ) {
            run[col - first] = (u16)want[base + col];
            shown[base + col] = want[base + col];
        }
        put_cells( (u32)row * line_cols + first, run, (u32)(last - first + 1) );
    }
}

/* ------------------------------------------------------------------ */
/* icons: the art is the program's (ARCICON.C); GFX.C draws it         */
/* ------------------------------------------------------------------ */

static const char *const *icon_art[ICON_MAX];
static int icon_rows[ICON_MAX];
static int icon_count;

int df_icon_register( const char *const *art, int nrows )
{
    if ( art == NULL || nrows <= 0 || icon_count >= ICON_MAX ) {
        return ICON_NONE;
    }
    icon_art[icon_count] = art;
    icon_rows[icon_count] = nrows;
    return icon_count++;
}

const char *const *df_icon_art( int icon, int *nrows )
{
    if ( icon < 0 || icon >= icon_count ) {
        return NULL;
    }
    if ( nrows ) {
        *nrows = icon_rows[icon];
    }
    return icon_art[icon];
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

#define MQ_SIZE 16

static int   mouse_on;
static int   mouse_xmax = 639, mouse_ymax = 199;
static int   mouse_x, mouse_y;
static int   down;                      /* the buttons, as the events have them */
static EVENT queue[MQ_SIZE];
static int   q_head, q_tail;

static void call33( REGS *regs, u32 func )
{
    regs->eax = func;
    int33( regs );
}

static void to_cell( int x, int y, int *col, int *row )
{
    if ( gfx ) {
        *col = x / 8;
        *row = y / GFX_CELL_H;
    } else {
        *col = (int)((long)x * cols / (mouse_xmax + 1));
        *row = (int)((long)y * rows / (mouse_ymax + 1));
    }
    if ( *col >= cols ) {
        *col = cols - 1;
    }
    if ( *row >= rows ) {
        *row = rows - 1;
    }
    if ( *col < 0 ) {
        *col = 0;
    }
    if ( *row < 0 ) {
        *row = 0;
    }
}

static void queue_put( const EVENT *ev )
{
    int next = (q_tail + 1) % MQ_SIZE;

    if ( next == q_head ) {
        return;                         /* full: a slow program loses one */
    }
    queue[q_tail] = *ev;
    q_tail = next;
}

static void queue_button( int bit, int pressed, int x, int y )
{
    EVENT ev;

    if ( pressed ) {
        down |= bit;
    } else {
        down &= ~bit;
    }
    ev.kind = EV_MOUSE;
    ev.key = 0;
    to_cell( x, y, &ev.x, &ev.y );
    ev.buttons = down;
    ev.press = pressed ? bit : 0;
    queue_put( &ev );
}

/* what the driver has to say since the last look, as events */
static void mouse_poll( void )
{
    REGS regs;
    int button, bit, presses, releases, px, py, rx, ry, now, was;

    if ( !mouse_on ) {
        return;
    }
    xa_clear( &regs );
    call33( &regs, 0x0003 );
    now = (int)(regs.ebx & 3);
    mouse_x = (int)(regs.ecx & 0xFFFF);
    mouse_y = (int)(regs.edx & 0xFFFF);
    if ( gfx ) {
        gfx_pointer( mouse_x, mouse_y, 1 );
        gfx_flush();
    }
    for ( button = 0; button < 2; button++ ) {
        bit = 1 << button;
        xa_clear( &regs );
        regs.ebx = (u32)button;
        call33( &regs, 0x0005 );
        presses = (int)(regs.ebx & 0xFFFF);
        px = (int)(regs.ecx & 0xFFFF);
        py = (int)(regs.edx & 0xFFFF);
        xa_clear( &regs );
        regs.ebx = (u32)button;
        call33( &regs, 0x0006 );
        releases = (int)(regs.ebx & 0xFFFF);
        rx = (int)(regs.ecx & 0xFFFF);
        ry = (int)(regs.edx & 0xFFFF);
        was = (down & bit) != 0;
        if ( presses > 2 ) {
            presses = 2;
        }
        if ( releases > 2 ) {
            releases = 2;
        }
        /* in the order they happened: a button down now went down last */
        if ( now & bit ) {
            if ( was && releases ) {
                queue_button( bit, 0, rx, ry );
                releases--;
            }
            while ( presses > 1 && releases > 0 ) {
                queue_button( bit, 1, px, py );
                queue_button( bit, 0, rx, ry );
                presses--;
                releases--;
            }
            if ( presses || !(down & bit) ) {
                queue_button( bit, 1, px, py );
            }
        } else {
            if ( was && releases ) {
                queue_button( bit, 0, rx, ry );
                releases--;
            }
            while ( presses > 0 ) {
                queue_button( bit, 1, px, py );
                queue_button( bit, 0, releases ? rx : mouse_x, releases ? ry : mouse_y );
                presses--;
                if ( releases ) {
                    releases--;
                }
            }
            if ( down & bit ) {
                queue_button( bit, 0, mouse_x, mouse_y );
            }
        }
    }
}

/* A text screen's pointer is the driver's own; a graphics screen's is
   GFX.C's, and the driver is told how many pixels there are and asked
   only where the mouse is. */
void ev_mouse_reset( void )
{
    REGS regs;

    xa_clear( &regs );
    call33( &regs, 0x0000 );
    mouse_on = (regs.eax & 0xFFFF) == 0xFFFF;
    if ( !mouse_on ) {
        return;
    }
    if ( gfx ) {
        mouse_xmax = 639;
        mouse_ymax = GFX_ROWS * GFX_CELL_H - 1;
        xa_clear( &regs );
        regs.edx = (u32)mouse_xmax;
        call33( &regs, 0x0007 );
        xa_clear( &regs );
        regs.edx = (u32)mouse_ymax;
        call33( &regs, 0x0008 );
        xa_clear( &regs );
        regs.ecx = 320;
        regs.edx = (u32)(mouse_ymax / 2);
        call33( &regs, 0x0004 );
    } else {
        xa_clear( &regs );
        call33( &regs, 0x0026 );
        mouse_xmax = (int)(regs.ecx & 0xFFFF);
        mouse_ymax = (int)(regs.edx & 0xFFFF);
        if ( mouse_xmax < 1 || mouse_ymax < 1 ) {
            mouse_xmax = 639;
            mouse_ymax = 199;
        }
    }
    /* nothing from before this moment */
    xa_clear( &regs );
    call33( &regs, 0x0003 );
    down = (int)(regs.ebx & 3);
    mouse_x = (int)(regs.ecx & 0xFFFF);
    mouse_y = (int)(regs.edx & 0xFFFF);
    xa_clear( &regs ); regs.ebx = 0; call33( &regs, 0x0005 );
    xa_clear( &regs ); regs.ebx = 0; call33( &regs, 0x0006 );
    xa_clear( &regs ); regs.ebx = 1; call33( &regs, 0x0005 );
    xa_clear( &regs ); regs.ebx = 1; call33( &regs, 0x0006 );
    q_head = q_tail = 0;
    if ( gfx ) {
        gfx_pointer( mouse_x, mouse_y, 1 );
    } else {
        xa_clear( &regs );
        call33( &regs, 0x0001 );
    }
}

int ev_mouse_on( void )
{
    return mouse_on;
}

int ev_buttons( void )
{
    return down;
}

/* ------------------------------------------------------------------ */
/* the keyboard, and the test mode's key file                          */
/* ------------------------------------------------------------------ */

#define SH_ALT      0x08
#define SH_KEYS     0x0F

static int shift_seen, alt_armed;
static int testing, poll_keys, test_shift, test_eof;
static u32 test_clock;
static u32 log_handle;
static int log_open;
static char log_path[260];
static int shots;

static int stdin_byte( void )
{
    REGS regs;
    u8 ch;

    xa_clear( &regs );
    regs.ebx = 0;
    regs.ecx = 1;
    regs.edx = (u32)&ch;
    if ( xa_dos( &regs, 0x3F00 ) || regs.eax == 0 ) {
        return -1;
    }
    return ch;
}

static void log_write( const char *text, u32 len )
{
    REGS regs;

    if ( !log_open ) {
        return;
    }
    xa_clear( &regs );
    regs.ebx = log_handle;
    regs.ecx = len;
    regs.edx = (u32)text;
    xa_dos( &regs, 0x4000 );
}

void ev_init( void )
{
    REGS regs;
    const char *log = getenv( "XATEST" );

    if ( log && *log ) {
        /* 4400h: standard input is a file, not the console */
        xa_clear( &regs );
        regs.ebx = 0;
        if ( xa_dos( &regs, 0x4400 ) == 0 && (regs.edx & 0x81) != 0x81 ) {
            testing = 1;
            poll_keys = getenv( "XAPOLL" ) != NULL;
            xa_clear( &regs );
            regs.edx = (u32)log;
            if ( xa_dos( &regs, 0x3C00 ) == 0 ) {
                log_handle = AX( regs );
                log_open = 1;
                strncpy( log_path, log, sizeof( log_path ) - 1 );
            }
        }
    }
    xa_clear( &regs );
    xa_dos( &regs, 0xF009 );
    shift_seen = (int)(regs.ebx & SH_KEYS);
    ev_mouse_reset();
}

int ev_testing( void )
{
    return testing;
}

/* ^C and ^Break: noted by DOSINT.ASM's handler, not the end of us */
void ev_break_off( void )
{
    REGS regs;

    xa_clear( &regs );
    regs.edx = (u32)break_handler;
    xa_dos( &regs, 0x2523 );
}

u32 ev_clock( void )
{
    REGS regs;

    if ( testing ) {
        return test_clock;
    }
    xa_clear( &regs );
    xa_dos( &regs, 0x2C00 );
    return ((((regs.ecx >> 8) & 0xFF) * 60 + (regs.ecx & 0xFF)) * 60
            + ((regs.edx >> 8) & 0xFF)) * 100 + (regs.edx & 0xFF);
}

/* The screen as text into the log - with the box lines as + - | and the
   blocks as #, the way XArc's DOS test harness wrote its snapshots, so
   the two can be compared line for line. */
void scr_dump( const char *why )
{
    char line[140];
    int row, col, len;
    u8 ch;

    if ( !log_open ) {
        return;
    }
    len = sprintf( line, "--- %s\r\n", why );
    log_write( line, (u32)len );
    for ( row = 1; row <= rows; row++ ) {
        for ( col = 1; col <= cols && col <= 132; col++ ) {
            ch = CELL_CHAR( df_cell_at( row, col ) );
            if ( CELL_EXTRA( df_cell_at( row, col ) ) ) {
                ch = EXTRA_IS_BAR( CELL_EXTRA( df_cell_at( row, col ) ) ) ? '#' : '*';
            } else if ( ch == 0 || ch == 32 ) {
                ch = ' ';
            } else if ( ch == 186 || ch == 179 ) {
                ch = '|';
            } else if ( ch == 205 || ch == 196 ) {
                ch = '-';
            } else if ( ch == 201 || ch == 187 || ch == 200 || ch == 188 || ch == 218
                        || ch == 191 || ch == 192 || ch == 217 || ch == 213 || ch == 184
                        || ch == 212 || ch == 190 ) {
                ch = '+';
            } else if ( ch == 219 || ch == 176 || ch == 177 || ch == 178 ) {
                ch = '#';
            } else if ( ch < 32 || ch > 126 ) {
                ch = '.';
            }
            line[col - 1] = (char)ch;
        }
        len = col - 1;
        line[len++] = '\r';
        line[len++] = '\n';
        log_write( line, (u32)len );
    }
}

/* the colours: two hex digits a cell, background then foreground */
static void scr_dump_attrs( void )
{
    static const char hex[] = "0123456789ABCDEF";
    char line[300];
    int row, col, len;
    u8 attr;

    for ( row = 1; row <= rows; row++ ) {
        len = 0;
        for ( col = 1; col <= cols && col <= 132; col++ ) {
            attr = CELL_ATTR( df_cell_at( row, col ) );
            line[len++] = hex[attr >> 4];
            line[len++] = hex[attr & 15];
        }
        line[len++] = '\r';
        line[len++] = '\n';
        log_write( line, (u32)len );
    }
}

static void scr_shot( void )
{
    char path[280], line[320];
    int len, dot, ok;

    if ( !log_open || !gfx ) {
        return;
    }
    strcpy( path, log_path );
    len = (int)strlen( path );
    for ( dot = len; dot > 0 && path[dot - 1] != '.' && path[dot - 1] != '\\'; dot-- ) {
    }
    if ( dot > 0 && path[dot - 1] == '.' ) {
        len = dot - 1;
    }
    shots++;
    sprintf( path + len, "%02d.BMP", shots % 100 );
    df_flush();
    ok = gfx_snapshot( path );
    len = sprintf( line, "[picture %s%s]\r\n", path, ok ? "" : " NOT WRITTEN" );
    log_write( line, (u32)len );
}

/* test mode: FEh, the buttons, a row and a column - the pointer is put
   on that cell (04h) and the buttons set there (21h/F0h AL=0Bh), and
   what that amounts to is read back through INT 33h like anything the
   mouse does */
static void test_mouse( int buttons, int row, int col )
{
    REGS regs;

    if ( !mouse_on ) {
        return;
    }
    xa_clear( &regs );
    if ( gfx ) {
        regs.ecx = (u32)(col * 8 + 4);
        regs.edx = (u32)(row * GFX_CELL_H + GFX_CELL_H / 2);
    } else {
        regs.ecx = (u32)(col * (mouse_xmax + 1) / cols + 1);
        regs.edx = (u32)(row * (mouse_ymax + 1) / rows + 1);
    }
    call33( &regs, 0x0004 );
    xa_clear( &regs );
    regs.ebx = (u32)(buttons & 7);
    xa_dos( &regs, 0xF00B );
    mouse_poll();
}

static int key_from( int ascii, int scan )
{
    if ( ascii == 0 || (ascii == 0xE0 && scan != 0) ) {
        return KEY_EXT( scan );
    }
    return ascii;
}

/* the next thing from the key file: a key, or 0 when it was the mouse
   (the queue has it) */
static int test_next( void )
{
    int byte, next, before;

    for ( ;; ) {
        if ( test_eof ) {
            return KEY_ESC;
        }
        byte = stdin_byte();
        if ( byte < 0 ) {
            if ( getenv( "XAHOLD" ) ) {
                testing = 0;            /* the keyboard from here on */
                return -1;
            }
            test_eof = 1;
            return KEY_ESC;
        }
        if ( byte == 0xFE ) {
            int buttons = stdin_byte(), row = stdin_byte(), col = stdin_byte();

            if ( col < 0 ) {
                test_eof = 1;
                return KEY_ESC;
            }
            test_clock += 1;
            test_mouse( buttons, row, col );
            if ( q_head != q_tail ) {
                return 0;
            }
            continue;
        }
        test_clock += 100;
        if ( byte == 0 ) {
            next = stdin_byte();
            return next < 0 ? KEY_ESC : KEY_EXT( next );
        }
        if ( byte != 0xFF ) {
            return byte;
        }
        next = stdin_byte();
        if ( next == 0xFF ) {
            df_flush();
            scr_dump( "screen" );
            continue;
        }
        if ( next == 0xFD ) {
            df_flush();
            scr_dump( "screen and colours" );
            scr_dump_attrs();
            continue;
        }
        if ( next == 0xFE ) {
            scr_shot();
            continue;
        }
        if ( next < 0 ) {
            test_eof = 1;
            return KEY_ESC;
        }
        before = test_shift;
        test_shift = next & SH_KEYS;
        if ( before == SH_ALT && test_shift == 0 ) {
            return KEY_ALT;
        }
    }
}

static int queue_take( EVENT *ev )
{
    if ( q_head == q_tail ) {
        return 0;
    }
    *ev = queue[q_head];
    q_head = (q_head + 1) % MQ_SIZE;
    return 1;
}

static void key_event( EVENT *ev, int key )
{
    ev->kind = EV_KEY;
    ev->key = key;
    ev->x = ev->y = 0;
    ev->buttons = down;
    ev->press = 0;
}

/* a key, the mouse, or nothing; "wait" says whether to wait for one */
static int ev_get( EVENT *ev, int wait )
{
    REGS regs;
    int shift, got, key;

    df_flush();
    for ( ;; ) {
        if ( queue_take( ev ) ) {
            alt_armed = 0;
            return 1;
        }
        if ( testing ) {
            if ( !wait && !poll_keys ) {
                ev->kind = EV_NONE;
                return 0;
            }
            key = test_next();
            if ( key < 0 ) {
                continue;               /* XAHOLD: the keyboard now */
            }
            if ( key == 0 ) {
                continue;               /* the mouse: in the queue */
            }
            key_event( ev, key );
            return 1;
        }
        xa_clear( &regs );
        regs.edx = ((u32)((mouse_on ? 2 : 0) | (wait ? 0 : 1)) << 8) | (u32)(shift_seen & 0xFF);
        xa_dos( &regs, 0xF008 );
        shift = (int)(regs.ebx & 0xFFFF);
        got = (int)(regs.eax & 0xFF);
        if ( got == 1 ) {
            xa_clear( &regs );
            xa_dos( &regs, 0xF007 );
            shift_seen = (int)(regs.ebx & SH_KEYS);
            alt_armed = 0;
            key_event( ev, key_from( (int)(regs.eax & 0xFF), (int)((regs.eax >> 8) & 0xFF) ) );
            return 1;
        }
        if ( got == 2 ) {
            mouse_poll();
            continue;
        }
        if ( mouse_on && !wait ) {
            mouse_poll();
            if ( queue_take( ev ) ) {
                return 1;
            }
        }
        shift &= SH_KEYS;
        if ( (shift & SH_ALT) && !(shift_seen & SH_ALT) ) {
            alt_armed = shift == SH_ALT;
        } else if ( !(shift & SH_ALT) && (shift_seen & SH_ALT) && alt_armed ) {
            alt_armed = 0;
            shift_seen = shift;
            key_event( ev, KEY_ALT );
            return 1;
        } else if ( shift & ~SH_ALT ) {
            alt_armed = 0;
        }
        shift_seen = shift;
        if ( !wait ) {
            ev->kind = EV_NONE;
            return 0;
        }
    }
}

int ev_wait( EVENT *ev )
{
    return ev_get( ev, 1 );
}

int ev_poll( EVENT *ev )
{
    return ev_get( ev, 0 );
}
