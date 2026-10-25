/*
 * SCR.C - the screen in memory, the parts every Setup screen is made
 * of, and the keyboard.
 *
 * EVERYTHING IS DRAWN INTO MEMORY FIRST and scr_flush() sends the cells
 * that differ from what the screen was last given, as SCANDISK does.  A
 * box that goes away is taken away by putting back the cells saved
 * before it was drawn (scr_save/scr_restore), which is what MS-DOS
 * Setup did with the area under its windows.
 *
 * THE LOOK IS MS-DOS 6.22 SETUP'S, measured from the real program: a
 * blue screen, the title at row 1 column 1 over a double rule as long
 * as itself, body text from column 10, bullets at 12 with their text at
 * 14, and the status line on row 24 with a bar at column 57 whenever
 * the keys end before it.  Boxes have single lines, no shadow, and the
 * title sits in the top line.
 *
 * THERE IS NO MOUSE, as in 6.22 Setup, which was keyboard only.
 *
 * TEST MODE: with SETUPTEST naming a log file and standard input taken
 * from a file, the keys come from the file: a byte for an ASCII key, a
 * zero and a scan code for any other; FFh FFh writes the screen into
 * the log as text and FFh FEh writes its colours as hex.  When the file
 * runs out Setup ends (code 99).
 */
#include "setup.h"
#include "text.h"

static u16  want[SCR_ROWS * SCR_COLS];
static u16  shown[SCR_ROWS * SCR_COLS];
static int  cur_row = -1, cur_col = -1;         /* -1 = hidden */
static int  shown_row = -2, shown_col = -2;

/* the status line's keys, kept so a box can put them back */
static char status_keys[100];

int test_mode;
static char test_log[PATHLEN];

static void test_open( void );

void scr_open( void )
{
    int i;

    plat_screen_open();
    for ( i = 0; i < SCR_ROWS * SCR_COLS; i++ ) {
        shown[i] = 0xFFFF;              /* nothing shown matches */
        want[i] = (A_DESK << 8) | ' ';
    }
    test_open();
}

void scr_close( void )
{
    plat_screen_close();
}

void scr_flush( void )
{
    int pos = 0, end, total = SCR_ROWS * SCR_COLS;

    while ( pos < total ) {
        if ( want[pos] == shown[pos] ) {
            pos++;
            continue;
        }
        end = pos;
        while ( end < total && want[end] != shown[end] ) {
            shown[end] = want[end];
            end++;
        }
        plat_screen_put( pos, want + pos, end - pos );
        pos = end;
    }
    if ( cur_row != shown_row || cur_col != shown_col ) {
        plat_cursor( cur_row, cur_col, cur_row >= 0 );
        shown_row = cur_row;
        shown_col = cur_col;
    }
}

void scr_putn( int row, int col, int attr, const char *text, int len )
{
    u16 *cell;

    if ( row < 0 || row >= SCR_ROWS ) {
        return;
    }
    cell = want + row * SCR_COLS;
    while ( len-- > 0 && *text ) {
        if ( col >= 0 && col < SCR_COLS ) {
            cell[col] = ((u16)attr << 8) | (u8)*text;
        }
        col++;
        text++;
    }
}

void scr_put( int row, int col, int attr, const char *text )
{
    scr_putn( row, col, attr, text, SCR_COLS );
}

void scr_fill( int row, int col, int rows, int cols, int attr, int ch )
{
    int r, c;

    for ( r = row; r < row + rows && r < SCR_ROWS; r++ ) {
        for ( c = col; c < col + cols && c < SCR_COLS; c++ ) {
            if ( r >= 0 && c >= 0 ) {
                want[r * SCR_COLS + c] = ((u16)attr << 8) | (u8)ch;
            }
        }
    }
}

void scr_attr( int row, int col, int cols, int attr )
{
    u16 *cell = want + row * SCR_COLS;

    for ( ; cols > 0 && col < SCR_COLS; col++, cols-- ) {
        cell[col] = ((u16)attr << 8) | (cell[col] & 0xFF);
    }
}

void scr_cursor( int row, int col )
{
    cur_row = row;
    cur_col = col;
}

void scr_nocursor( void )
{
    cur_row = cur_col = -1;
}

void scr_save( u16 *cells )
{
    mem_cpy( cells, want, sizeof( want ) );
}

void scr_restore( const u16 *cells )
{
    mem_cpy( want, cells, sizeof( want ) );
}

/* ------------------------------------------------------------------ */
/* the parts                                                           */
/* ------------------------------------------------------------------ */

/* The work area cleared, the title written over a double rule of its
   own length.  The status line is the caller's. */
void scr_desk( const char *title )
{
    int len = str_len( title ), i;

    scr_fill( 0, 0, STATUS_ROW, SCR_COLS, A_DESK, ' ' );
    scr_put( 1, 1, A_DESK, title );
    for ( i = 0; i < len; i++ ) {
        want[2 * SCR_COLS + 1 + i] = (A_DESK << 8) | 0xCD;
    }
    scr_nocursor();
}

/* "ENTER=Continue  F1=Help  F3=Exit" from column 0; the bar at 57 when
   there is room for it, and what Setup is doing after it */
void scr_status( const char *keys, const char *right )
{
    int len = str_len( keys );

    str_cpyn( status_keys, keys, sizeof( status_keys ) );
    scr_fill( STATUS_ROW, 0, 1, SCR_COLS, A_BAR, ' ' );
    scr_put( STATUS_ROW, 0, A_BAR, keys );
    if ( len <= STATUS_SPLIT ) {
        want[STATUS_ROW * SCR_COLS + STATUS_SPLIT] = (A_BAR << 8) | 0xB3;
        if ( right ) {
            scr_putn( STATUS_ROW, STATUS_SPLIT + 1, A_BAR, right,
                      SCR_COLS - STATUS_SPLIT - 1 );
        }
    }
}

const char *status_now( void )
{
    return status_keys;
}

/* how many rows a paragraph takes at this width; '\n' breaks a line */
static int wrap_next( const char *para, int width, int *len )
{
    int end = 0, brk = -1;

    while ( para[end] && para[end] != '\n' ) {
        if ( para[end] == ' ' ) {
            brk = end;
        }
        if ( end >= width ) {
            if ( brk > 0 ) {
                end = brk;
            }
            break;
        }
        end++;
    }
    *len = end;
    if ( para[end] == ' ' || para[end] == '\n' ) {
        end++;
    }
    return end;
}

int scr_text( int row, int col, int width, int attr, const char *para )
{
    int rows = 0, len, step;

    while ( *para ) {
        step = wrap_next( para, width, &len );
        if ( attr >= 0 ) {
            scr_putn( row + rows, col, attr, para, len );
        }
        rows++;
        para += step;
    }
    return rows;
}

int scr_text_rows( int width, const char *para )
{
    return scr_text( 0, 0, width, -1, para );
}

/* body text: column 10, sixty wide - where 6.22 broke its lines */
int scr_para( int row, const char *para )
{
    return scr_text( row, 10, 60, A_DESK, para );
}

/* a bullet at 12, its text at 14 */
int scr_bullet( int row, const char *para )
{
    scr_putn( row, 12, A_DESK, "\x07", 1 );     /* (a row off the screen: none) */
    return scr_text( row, 14, 56, A_DESK, para );
}

/* "Note:" at 10, the paragraph from 16 */
int scr_note( int row, const char *label, const char *para )
{
    scr_put( row, 10, A_DESK, label );
    return scr_text( row, 16, 54, A_DESK, para );
}

void scr_center( int row, int left, int right, int attr, const char *text )
{
    int len = str_len( text );

    scr_put( row, left + (right - left + 1 - len) / 2, attr, text );
}

void scr_box( int top, int left, int bottom, int right, int attr,
              const char *title )
{
    int r, len;
    u16 a = (u16)attr << 8;

    /* every box Setup draws fits; one that would not is cut to the
       screen rather than written past it */
    if ( top < 0 ) {
        top = 0;
    }
    if ( left < 0 ) {
        left = 0;
    }
    if ( bottom > STATUS_ROW - 1 ) {
        bottom = STATUS_ROW - 1;
    }
    if ( right > SCR_COLS - 1 ) {
        right = SCR_COLS - 1;
    }
    if ( bottom <= top || right <= left ) {
        return;
    }
    scr_fill( top, left, bottom - top + 1, right - left + 1, attr, ' ' );
    for ( r = left + 1; r < right; r++ ) {
        want[top * SCR_COLS + r] = a | 0xC4;
        want[bottom * SCR_COLS + r] = a | 0xC4;
    }
    for ( r = top + 1; r < bottom; r++ ) {
        want[r * SCR_COLS + left] = a | 0xB3;
        want[r * SCR_COLS + right] = a | 0xB3;
    }
    want[top * SCR_COLS + left] = a | 0xDA;
    want[top * SCR_COLS + right] = a | 0xBF;
    want[bottom * SCR_COLS + left] = a | 0xC0;
    want[bottom * SCR_COLS + right] = a | 0xD9;
    if ( title && *title ) {
        len = str_len( title ) + 2;
        r = left + (right - left + 1 - len) / 2;
        scr_put( top, r, attr, " " );
        scr_put( top, r + 1, attr, title );
        scr_put( top, r + len - 1, attr, " " );
    }
}

/* Row 16: "NNN% complete"; rows 17-20 a box, and in rows 18-19 a bar of
   58 blocks for the whole */
void scr_gauge( u32 done, u32 total )
{
    char line[16];
    int pct, cells;

    if ( total == 0 ) {
        total = 1;
    }
    if ( done >= total ) {
        pct = 100;
    } else {
        /* done * 100 stays inside 32 bits below 40MB */
        pct = total < 0x02000000ul ? (int)(done * 100 / total)
                                   : (int)(done / (total / 100));
        if ( pct > 99 ) {
            pct = 99;                   /* held there until the end */
        }
    }
    sfmt( line, sizeof( line ), "%3d%s", pct, tx_complete );
    scr_fill( 16, 10, 1, 60, A_DESK, ' ' );
    scr_put( 16, 10, A_DESK, line );
    scr_box( 17, 10, 20, 69, A_DESK, NULL );
    cells = pct * 58 / 100;
    scr_fill( 18, 11, 2, cells, A_GAUGE, 0xDB );
}

/* ------------------------------------------------------------------ */
/* keys                                                                */
/* ------------------------------------------------------------------ */

static int test_key( void );

int key_get( void )
{
    scr_flush();
    if ( test_mode ) {
        return test_key();
    }
    while ( plat_key_ready() ) {        /* 6.22 threw away keys typed */
        plat_key_read();                /* ahead of the question      */
    }
    for ( ;; ) {
        if ( plat_key_ready() ) {
            return plat_key_read();
        }
        plat_idle();
    }
}

int key_hit( void )
{
    if ( test_mode ) {
        return 0;                       /* the file is for the questions */
    }
    if ( plat_key_ready() ) {
        return plat_key_read();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* test mode                                                           */
/* ------------------------------------------------------------------ */

static void test_open( void )
{
    const char *log = plat_getenv( "SETUPTEST" );

    if ( log && *log ) {
        str_cpyn( test_log, log, sizeof( test_log ) );
    }
}

static void log_write( const char *text, int len )
{
    u16 handle, done;
    u32 size;

    if ( !test_log[0] ) {
        return;
    }
    if ( plat_open( test_log, 1, &handle ) ) {
        if ( plat_create( test_log, &handle ) ) {
            return;
        }
    }
    if ( plat_size( handle, &size ) == 0 ) {
        plat_seek( handle, size );      /* written on the end */
    }
    plat_write( handle, text, (u16)len, &done );
    plat_close( handle );
}

void test_dump( void )
{
    char line[SCR_COLS * 2 + 4];
    int row, col, len;

    if ( !test_log[0] ) {
        return;
    }
    log_write( "----\r\n", 6 );
    for ( row = 0; row < SCR_ROWS; row++ ) {
        for ( col = 0; col < SCR_COLS; col++ ) {
            int ch = want[row * SCR_COLS + col] & 0xFF;
            line[col] = (char)(ch < ' ' ? '.' : ch);
        }
        len = SCR_COLS;
        while ( len > 0 && line[len - 1] == ' ' ) {
            len--;
        }
        line[len++] = '\r';
        line[len++] = '\n';
        log_write( line, len );
    }
}

static void test_attrs( void )
{
    static const char hex[] = "0123456789ABCDEF";
    char line[SCR_COLS * 2 + 4];
    int row, col, len;

    log_write( "====\r\n", 6 );
    for ( row = 0; row < SCR_ROWS; row++ ) {
        for ( col = 0; col < SCR_COLS; col++ ) {
            int attr = want[row * SCR_COLS + col] >> 8;
            line[col * 2] = hex[attr >> 4];
            line[col * 2 + 1] = hex[attr & 15];
        }
        len = SCR_COLS * 2;
        line[len++] = '\r';
        line[len++] = '\n';
        log_write( line, len );
    }
}

static int test_key( void )
{
    int byte, next;

    for ( ;; ) {
        byte = plat_stdin_byte();
        if ( byte < 0 ) {
            scr_close();
            plat_exit( 99 );
        }
        if ( byte == 0xFF ) {
            next = plat_stdin_byte();
            if ( next == 0xFF ) {
                test_dump();
            } else if ( next == 0xFE ) {
                test_attrs();
            }
            continue;
        }
        if ( byte == 0 || byte == 0xE0 ) {
            next = plat_stdin_byte();
            return K_EXT( next & 0xFF );
        }
        return byte;
    }
}
