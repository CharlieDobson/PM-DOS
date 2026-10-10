/*
 * VIEW.C - a file's contents, as text or in hexadecimal.
 *
 * THE FILE STAYS ON THE DISK.  What is on the screen is read through a
 * small window of the file kept in memory, so a file of any size opens
 * at once and costs the same.  For the text view the file is read
 * through once first, to note where each line of the screen starts: a
 * line ends at a line feed or when the screen is full, and a tab goes
 * to the next eighth column.  The hexadecimal view needs no such list -
 * sixteen bytes a line, wherever they fall.
 *
 * F9 changes between the two views, as it opened the viewer.
 */
#include "shell.h"

#define CACHE_SIZE      (16u * 1024u)

static u32  handle;
static u32  size;
static u8  *cache;
static u32  cache_pos, cache_len;
static u32 *line_at;            /* where each line of the text view starts */
static u32  nlines, line_cap;
static int  hex;
static u32  top;                /* the first line showing */
static const char *shown_path;

static int byte_at( u32 pos )
{
    u32 start, done;

    if ( pos >= size ) {
        return -1;
    }
    if ( pos < cache_pos || pos >= cache_pos + cache_len ) {
        start = pos & ~(CACHE_SIZE / 2 - 1);
        if ( sys_seek( handle, start ) != 0
             || sys_read( handle, cache, CACHE_SIZE, &done ) != 0 || done == 0 ) {
            return -1;
        }
        cache_pos = start;
        cache_len = done;
        if ( pos >= cache_pos + cache_len ) {
            return -1;
        }
    }
    return cache[pos - cache_pos];
}

static int line_note( u32 pos )
{
    u32 *bigger;

    if ( nlines == line_cap ) {
        bigger = (u32 *)try_alloc( (line_cap + 4096) * sizeof( u32 ) );
        if ( bigger == NULL ) {
            return 0;
        }
        if ( nlines ) {
            mem_cpy( bigger, line_at, nlines * sizeof( u32 ) );
        }
        xfree( line_at );
        line_at = bigger;
        line_cap += 4096;
    }
    line_at[nlines++] = pos;
    return 1;
}

/* where the lines start, for a screen "width" columns wide */
static void index_lines( int width )
{
    u32 pos;
    int col = 0, ch;

    nlines = 0;
    if ( !line_note( 0 ) ) {
        return;
    }
    for ( pos = 0; pos < size; pos++ ) {
        ch = byte_at( pos );
        if ( ch < 0 ) {
            break;
        }
        if ( ch == '\n' ) {
            if ( pos + 1 < size && !line_note( pos + 1 ) ) {
                break;
            }
            col = 0;
            continue;
        }
        if ( ch == '\r' ) {
            continue;
        }
        col = ch == '\t' ? (col + 8) & ~7 : col + 1;
        if ( col > width ) {                    /* this one starts a new line */
            if ( !line_note( pos ) ) {
                break;
            }
            col = ch == '\t' ? 8 : 1;
        }
        if ( (pos & 0xFFFF) == 0xFFFF ) {
            busy_show( "Reading the file...", shown_path );
        }
    }
}

static u32 total_lines( void )
{
    return hex ? (size + 15) / 16 : nlines;
}

static void draw_text_line( int row, u32 line, int width )
{
    u32 pos = line_at[line], end = line + 1 < nlines ? line_at[line + 1] : size;
    int col = 0, ch;

    for ( ; pos < end && col < width; pos++ ) {
        ch = byte_at( pos );
        if ( ch < 0 || ch == '\n' || ch == '\r' ) {
            continue;
        }
        if ( ch == '\t' ) {
            col = (col + 8) & ~7;
            continue;
        }
        scr_ch( row, 1 + col, ch ? ch : ' ', pal.pane );
        col++;
    }
}

static void draw_hex_line( int row, u32 line )
{
    static const char digits[] = "0123456789ABCDEF";
    u32 pos = line * 16;
    int index, ch, col;
    char addr[9];

    for ( index = 0; index < 8; index++ ) {
        addr[index] = digits[(pos >> (28 - index * 4)) & 15];
    }
    addr[8] = 0;
    scr_put( row, 1, addr, pal.pane );
    for ( index = 0; index < 16; index++ ) {
        ch = byte_at( pos + index );
        if ( ch < 0 ) {
            break;
        }
        col = 11 + index * 3 + (index >= 8 ? 1 : 0);
        scr_ch( row, col, digits[ch >> 4], pal.pane );
        scr_ch( row, col + 1, digits[ch & 15], pal.pane );
        scr_ch( row, 62 + index, ch ? ch : '.', pal.pane );
    }
}

static void draw( void )
{
    int rows = scr_rows - 3, row, width = scr_cols - 2;
    char title[PATH_MAX + 32];
    u32 total = total_lines();

    str_cpy( title, "View File Contents - " );
    str_catn( title, shown_path, sizeof( title ) );
    scr_fill( 0, 0, scr_cols, ' ', pal.title );
    scr_putn( 0, 1, title, (int)str_len( title ) < scr_cols - 2 ? (int)str_len( title )
              : scr_cols - 2, pal.title );
    scr_flag( 0, 0, scr_cols, CF_GRAD );
    for ( row = 0; row < rows + 1; row++ ) {
        scr_fill( 1 + row, 0, scr_cols, ' ', pal.pane );
    }
    for ( row = 0; row < rows && top + row < total; row++ ) {
        if ( hex ) {
            draw_hex_line( 2 + row, top + (u32)row );
        } else {
            draw_text_line( 2 + row, top + (u32)row, width );
        }
    }
    if ( size == 0 ) {
        scr_put( 2, 1, "The file is empty.", pal.pane );
    }
    sbar_draw( 2, scr_rows - 2, scr_cols - 1, (int)top, rows, (int)total );
    scr_fill( scr_rows - 1, 0, scr_cols, ' ', pal.status );
    scr_put( scr_rows - 1, 1, hex
             ? "Arrows, PgUp, PgDn, Home, End=Scroll   F9=Text   Esc=Close"
             : "Arrows, PgUp, PgDn, Home, End=Scroll   F9=Hex   Esc=Close", pal.status );
}

static void scroll_to( long line )
{
    long last = (long)total_lines() - (scr_rows - 3);

    if ( line > last ) {
        line = last;
    }
    if ( line < 0 ) {
        line = 0;
    }
    top = (u32)line;
}

void view_file( const char *path )
{
    int key, rc, rows, part;

    rc = sys_open_read( path, &handle );
    if ( rc == 0 ) {
        rc = sys_file_size( handle, &size );
    }
    if ( rc ) {
        msg_error( path, rc );
        return;
    }
    if ( cache == NULL ) {
        cache = (u8 *)try_alloc( CACHE_SIZE );
    }
    if ( cache == NULL ) {
        sys_close( handle );
        msg_box( "There is not enough memory to show the file.", MB_OK );
        return;
    }
    shown_path = path;
    cache_len = 0;
    hex = 0;
    top = 0;
    busy_show( "Reading the file...", path );
    index_lines( scr_cols - 2 );
    for ( ;; ) {
        rows = scr_rows - 3;
        scr_cursor( 0, 0, CUR_HIDE );
        draw();
        scr_flush();
        key = key_get();
        if ( key_service( key ) ) {
            continue;
        }
        if ( key == K_MOUSE ) {
            if ( (mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE) && mouse.col == scr_cols - 1 ) {
                part = sbar_hit( 2, scr_rows - 2, 0, (int)top, rows, (int)total_lines(), mouse.row );
                scroll_to( part == SB_UP ? (long)top - 1 : part == SB_DOWN ? (long)top + 1
                           : part == SB_PGUP ? (long)top - (rows - 1)
                           : part == SB_PGDN ? (long)top + (rows - 1) : (long)top );
            }
            continue;
        }
        switch ( key & ~K_SHIFT ) {
        case K_ESC:
        case K_ENTER:
        case K_F3:
            sys_close( handle );
            xfree( line_at );
            line_at = NULL;
            nlines = line_cap = 0;
            return;
        case K_F9:
            /* the same place in the file, in the other view */
            if ( hex ) {
                u32 pos = top * 16, line;
                for ( line = 0; line + 1 < nlines && line_at[line + 1] <= pos; line++ ) {
                }
                hex = 0;
                scroll_to( (long)line );
            } else {
                u32 pos = nlines ? line_at[top < nlines ? top : nlines - 1] : 0;
                hex = 1;
                scroll_to( (long)(pos / 16) );
            }
            break;
        case K_F1:      help_show( HELP_VIEWER );           break;
        case K_UP:      scroll_to( (long)top - 1 );         break;
        case K_DOWN:    scroll_to( (long)top + 1 );         break;
        case K_PGUP:    scroll_to( (long)top - (rows - 1) ); break;
        case K_PGDN:    scroll_to( (long)top + (rows - 1) ); break;
        case K_HOME:
        case K_CHOME:   scroll_to( 0 );                     break;
        case K_END:
        case K_CEND:    scroll_to( 0x7FFFFFFFL );           break;
        }
    }
}
