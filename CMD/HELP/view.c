/*
 * VIEW.C - the help window: a topic drawn in it, the cursor, the links,
 * the scroll bar and the mouse.
 *
 * THE CURSOR IS A SCREEN COLUMN on a line of the topic, as MS-DOS's
 * help has it: the arrow keys move it, the window scrolls to keep it in
 * sight, and a link is "selected" when the cursor stands on it.  Tab
 * and Shift+Tab take it to the next or the previous link, a letter to
 * the next link that starts with that letter (Shift+letter to the
 * previous), and Enter follows the link under it.
 *
 * A TOPIC'S FROZEN LINES (.freeze) stay at the top of the window while
 * the rest scrolls under them.  The cursor can be on either kind.
 *
 * The window is rows 1 to scr_rows-2: a title row, the text rows with
 * the frame's bar down the left and the scroll bar down the right, and
 * no bottom - the status bar is right under it, as in the original.
 */
#include "help.h"

HVIEW view;

#define TITLE_ROW   1
#define FIRST_ROW   2

static RLINE rl;                /* the line being looked at */

int view_rows( void )
{
    return scr_rows - 3;
}

int view_cols( void )
{
    return scr_cols - 2;
}

int view_lines( void )
{
    return view.topic < 0 ? 0 : (int)topics[view.topic].count;
}

static int frozen( void )
{
    int count;

    if ( view.topic < 0 ) {
        return 0;
    }
    count = topics[view.topic].freeze;
    if ( count > (int)topics[view.topic].count ) {
        count = (int)topics[view.topic].count;
    }
    if ( count > view_rows() - 1 ) {
        count = view_rows() - 1;
    }
    return count;
}

/* rows for the lines that scroll */
static int scroll_rows( void )
{
    return view_rows() - frozen();
}

static void render( u32 line )
{
    if ( view.topic < 0 || line >= topics[view.topic].count ) {
        rl.len = 0;
        rl.nlinks = 0;
        return;
    }
    hlp_render( topics[view.topic].first + line, &rl );
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_title( void )
{
    char title[90];
    int len, at;

    scr_fill( TITLE_ROW, 0, scr_cols, 0xC4, pal.frame );
    str_cpy( title, " PM-DOS Help: " );
    if ( view.topic >= 0 && topics[view.topic].title ) {
        str_catn( title, topics[view.topic].title, sizeof( title ) - 2 );
    }
    str_catn( title, " ", sizeof( title ) );
    len = (int)str_len( title );
    if ( len > scr_cols - 4 ) {
        len = scr_cols - 4;
    }
    at = (scr_cols - len) / 2;
    scr_putn( TITLE_ROW, at, title, len, pal.title );
}

/* where the scroll box is: a row of its bar, 1 being the first below the arrow */
static int vthumb( void )
{
    int rows = view_rows();
    u32 count = (u32)view_lines();

    return 1 + (count > 1 ? (int)(view.line * (u32)(rows - 3) / (count - 1)) : 0);
}

static void draw_vscroll( void )
{
    int rows = view_rows(), row, col = scr_cols - 1;

    for ( row = 0; row < rows; row++ ) {
        scr_ch( FIRST_ROW + row, col, 0xB1, pal.scroll );
    }
    if ( rows >= 2 ) {
        scr_ch( FIRST_ROW, col, 0x18, pal.scroll );
        scr_ch( FIRST_ROW + rows - 1, col, 0x19, pal.scroll );
    }
    if ( rows >= 3 ) {
        scr_ch( FIRST_ROW + vthumb(), col, ' ', pal.thumb );
    }
}

static void draw_line( int row, u32 line )
{
    int cols = view_cols(), len;

    scr_ch( row, 0, 0xB3, pal.frame );
    scr_fill( row, 1, cols, ' ', pal.text );
    if ( view.topic < 0 || line >= topics[view.topic].count ) {
        return;
    }
    render( line );
    len = rl.len < cols ? rl.len : cols;
    for ( cols = 0; cols < len; cols++ ) {
        scr_ch( row, 1 + cols, rl.ch[cols], rl.attr[cols] );
    }
}

void view_draw( void )
{
    int rows = view_rows(), row, freeze = frozen();

    draw_title();
    for ( row = 0; row < freeze; row++ ) {
        draw_line( FIRST_ROW + row, (u32)row );
    }
    for ( ; row < rows; row++ ) {
        draw_line( FIRST_ROW + row, view.top + (u32)(row - freeze) );
    }
    draw_vscroll();
}

/* the screen row the cursor's line is on, or -1 when scrolled away */
static int cursor_row( void )
{
    int freeze = frozen();

    if ( view.line < (u32)freeze ) {
        return FIRST_ROW + (int)view.line;
    }
    if ( view.line < view.top || view.line >= view.top + (u32)scroll_rows() ) {
        return -1;
    }
    return FIRST_ROW + freeze + (int)(view.line - view.top);
}

void view_place_cursor( void )
{
    int row = cursor_row();

    if ( row < 0 || view.topic < 0 ) {
        scr_cursor( 0, 0, CUR_HIDE );
        return;
    }
    scr_cursor( row, 1 + (int)view.col, CUR_LINE );
}

/* test mode: what the window is showing, on the dump's last line */
void view_dump( char *buf, u32 max )
{
    sfmt( buf, max, " [view topic %d top %u line %u col %u freeze %d count %d]",
          view.topic, view.top, view.line, view.col, frozen(), view_lines() );
}

/* ------------------------------------------------------------------ */
/* scrolling and the cursor                                            */
/* ------------------------------------------------------------------ */

static u32 min_top( void )
{
    return (u32)frozen();
}

static u32 max_top( void )
{
    u32 count = (u32)view_lines(), least = min_top();

    return count > least ? count - 1 : least;      /* the last line can reach the top */
}

static void clamp_top( void )
{
    if ( view.top < min_top() ) {
        view.top = min_top();
    }
    if ( view.top > max_top() ) {
        view.top = max_top();
    }
}

void view_keep_visible( void )
{
    int rows = scroll_rows();
    u32 freeze = min_top();

    if ( view.line < freeze ) {
        clamp_top();
        return;
    }
    if ( view.line < view.top ) {
        view.top = view.line;
    }
    if ( view.line >= view.top + (u32)rows ) {
        view.top = view.line - (u32)rows + 1;
    }
    clamp_top();
}

/* a far jump puts the line a few rows down from the top, as a jump to
   a found word does in the original */
void view_goto( u32 line, u32 col )
{
    u32 count = (u32)view_lines(), freeze = min_top();
    int rows = scroll_rows();

    if ( count == 0 ) {
        view.line = 0;
        view.col = 0;
        view.top = freeze;
        return;
    }
    if ( line >= count ) {
        line = count - 1;
    }
    view.line = line;
    view.col = col < (u32)view_cols() ? col : (u32)view_cols() - 1;
    view.want = view.col;
    if ( view.line >= freeze
         && (view.line < view.top || view.line >= view.top + (u32)rows) ) {
        view.top = view.line > freeze + 4 ? view.line - 4 : freeze;
    }
    view_keep_visible();
}

void view_scroll( int lines )
{
    u32 was = view.top;
    int rows = scroll_rows();

    if ( lines < 0 ) {
        view.top = view.top > (u32)-lines ? view.top - (u32)-lines : 0;
    } else {
        view.top += (u32)lines;
    }
    clamp_top();
    /* the cursor stays on the screen: it moves with the text */
    if ( view.top != was ) {
        if ( view.line >= min_top() && view.line < view.top ) {
            view.line = view.top;
        }
        if ( view.line >= view.top + (u32)rows ) {
            view.line = view.top + (u32)rows - 1;
        }
    }
}

void view_show( int topic )
{
    view.topic = topic;
    view.top = min_top();
    view.line = 0;
    view.col = 0;
    view.want = 0;
    if ( topic >= 0 ) {
        view.col = 0;
        /* the cursor starts on the first link, as the original's does */
        if ( !view_next_link( 1, 0 ) ) {
            view.line = 0;
            view.col = 0;
        } else if ( view.line >= min_top() + (u32)scroll_rows() ) {
            view.line = 0;                      /* too far down: start at the top */
            view.col = 0;
            view.top = min_top();
        }
    }
    view_keep_visible();
}

/* ------------------------------------------------------------------ */
/* links                                                               */
/* ------------------------------------------------------------------ */

/* the link on the line rendered last that the cursor column is in */
static int link_at( int col, HLINK *out )
{
    int index;

    for ( index = 0; index < rl.nlinks; index++ ) {
        if ( col >= rl.links[index].start - 1 && col <= rl.links[index].end ) {
            *out = rl.links[index];
            return 1;
        }
    }
    return 0;
}

int view_link_at_cursor( HLINK *link )
{
    if ( view.topic < 0 ) {
        return 0;
    }
    render( view.line );
    return link_at( (int)view.col, link );
}

static int link_starts_with( HLINK *link, int letter )
{
    int ch = link->start < rl.len ? rl.ch[link->start] : 0;

    return letter == 0 || ch_upper( ch ) == ch_upper( letter );
}

/* the next link after the cursor (step 1) or before it (-1), round the
   topic and back; letter: only one whose text starts with it.  Moves
   the cursor, 1; or 0 when there is none */
int view_next_link( int step, int letter )
{
    u32 count = (u32)view_lines(), line = view.line, tries;
    int index, found;

    if ( count == 0 ) {
        return 0;
    }
    /* the rest of this line first */
    render( line );
    found = -1;
    if ( step > 0 ) {
        for ( index = 0; index < rl.nlinks; index++ ) {
            if ( rl.links[index].start > (int)view.col && link_starts_with( &rl.links[index], letter ) ) {
                found = index;
                break;
            }
        }
    } else {
        for ( index = rl.nlinks - 1; index >= 0; index-- ) {
            if ( rl.links[index].start < (int)view.col && link_starts_with( &rl.links[index], letter ) ) {
                found = index;
                break;
            }
        }
    }
    for ( tries = 0; found < 0 && tries < count; tries++ ) {
        line = step > 0 ? (line + 1) % count : (line == 0 ? count - 1 : line - 1);
        render( line );
        if ( step > 0 ) {
            for ( index = 0; index < rl.nlinks; index++ ) {
                if ( link_starts_with( &rl.links[index], letter ) ) {
                    found = index;
                    break;
                }
            }
        } else {
            for ( index = rl.nlinks - 1; index >= 0; index-- ) {
                if ( link_starts_with( &rl.links[index], letter ) ) {
                    found = index;
                    break;
                }
            }
        }
    }
    if ( found < 0 ) {
        return 0;
    }
    view.line = line;
    view.col = (u32)rl.links[found].start;
    view.want = view.col;
    view_keep_visible();
    return 1;
}

/* ------------------------------------------------------------------ */
/* keys                                                                */
/* ------------------------------------------------------------------ */

static int word_ch( int ch )
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
           || ch == '_' || ch >= 128;
}

static void word_left( void )
{
    render( view.line );
    while ( view.col > 0 && (view.col > (u32)rl.len || !word_ch( rl.ch[view.col - 1] )) ) {
        view.col--;
    }
    while ( view.col > 0 && word_ch( rl.ch[view.col - 1] ) ) {
        view.col--;
    }
}

static void word_right( void )
{
    int cols = view_cols();

    render( view.line );
    while ( (int)view.col < rl.len && word_ch( rl.ch[view.col] ) ) {
        view.col++;
    }
    while ( (int)view.col < rl.len && !word_ch( rl.ch[view.col] ) ) {
        view.col++;
    }
    if ( (int)view.col >= cols ) {
        view.col = (u32)cols - 1;
    }
}

int view_key( int key )
{
    u32 count = (u32)view_lines();
    int rows = scroll_rows(), cols = view_cols();
    int shift = key_shift() & SH_SHIFT;

    if ( view.topic < 0 ) {
        return 0;
    }
    switch ( key ) {
    case K_UP:
        if ( view.line > 0 ) {
            view.line--;
        }
        view.col = view.want;
        break;
    case K_DOWN:
        if ( view.line + 1 < count ) {
            view.line++;
        }
        view.col = view.want;
        break;
    case K_LEFT:
        if ( view.col > 0 ) {
            view.col--;
        }
        view.want = view.col;
        break;
    case K_RIGHT:
        if ( (int)view.col < cols - 1 ) {
            view.col++;
        }
        view.want = view.col;
        break;
    case K_HOME:
        view.col = 0;
        view.want = 0;
        break;
    case K_END:
        render( view.line );
        view.col = rl.len ? (u32)(rl.len < cols ? rl.len : cols) - 1 : 0;
        view.want = view.col;
        break;
    case K_PGUP:
        view_scroll( -(rows - 1) );
        if ( view.line >= (u32)(rows - 1) ) {
            view.line -= (u32)(rows - 1);
        } else {
            view.line = 0;
        }
        view.col = view.want;
        break;
    case K_PGDN:
        view_scroll( rows - 1 );
        view.line += (u32)(rows - 1);
        if ( view.line >= count ) {
            view.line = count ? count - 1 : 0;
        }
        view.col = view.want;
        break;
    case K_CHOME:
        view.line = 0;
        view.col = 0;
        view.want = 0;
        view.top = min_top();           /* the body back to its start too */
        break;
    case K_CEND:
        view.line = count ? count - 1 : 0;
        view.col = 0;
        view.want = 0;
        break;
    case K_CUP:
        view_scroll( -1 );
        return 1;
    case K_CDOWN:
        view_scroll( 1 );
        return 1;
    case K_CLEFT:
        word_left();
        view.want = view.col;
        break;
    case K_CRIGHT:
        word_right();
        view.want = view.col;
        break;
    case K_TAB:
        view_next_link( 1, 0 );
        return 1;
    case K_SHTAB:
        view_next_link( -1, 0 );
        return 1;
    default:
        if ( key >= 0x100 || key < ' ' ) {
            return 0;
        }
        if ( (key >= 'A' && key <= 'Z') || (key >= 'a' && key <= 'z') || (key >= '0' && key <= '9') ) {
            view_next_link( shift ? -1 : 1, key );
            return 1;
        }
        return 0;
    }
    view_keep_visible();
    return 1;
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

int view_hit( int row, int col )
{
    int rows = view_rows(), thumb;

    if ( row < FIRST_ROW || row >= FIRST_ROW + rows ) {
        return VH_NONE;
    }
    if ( col == scr_cols - 1 ) {
        if ( rows < 2 ) {
            return VH_NONE;
        }
        if ( row == FIRST_ROW ) {
            return VH_UP;
        }
        if ( row == FIRST_ROW + rows - 1 ) {
            return VH_DOWN;
        }
        if ( rows < 3 ) {
            return VH_NONE;
        }
        thumb = FIRST_ROW + vthumb();
        return row < thumb ? VH_PGUP : row > thumb ? VH_PGDN : VH_VTHUMB;
    }
    if ( col >= 1 && col <= view_cols() ) {
        return VH_TEXT;
    }
    return VH_NONE;
}

/* the scroll bar: a line, a page, or wherever the box was dragged to */
void view_scroll_part( int part, int row, int col )
{
    int rows = view_rows();
    u32 count = (u32)view_lines(), line;

    (void)col;
    switch ( part ) {
    case VH_UP:
        view_key( K_UP );
        break;
    case VH_DOWN:
        view_key( K_DOWN );
        break;
    case VH_PGUP:
        view_key( K_PGUP );
        break;
    case VH_PGDN:
        view_key( K_PGDN );
        break;
    case VH_VTHUMB:
        if ( rows < 4 || count < 2 ) {
            break;
        }
        row -= FIRST_ROW + 1;
        if ( row < 0 ) {
            row = 0;
        }
        if ( row > rows - 3 ) {
            row = rows - 3;
        }
        line = (u32)row * (count - 1) / (u32)(rows - 3);
        view_goto( line, view.want );
        break;
    }
}

/* a press in the text: the cursor goes there */
void view_mouse_to( int row, int col )
{
    int freeze = frozen();
    u32 line;

    row -= FIRST_ROW;
    if ( row < freeze ) {
        line = (u32)row;
    } else {
        line = view.top + (u32)(row - freeze);
    }
    if ( line >= (u32)view_lines() ) {
        return;
    }
    view.line = line;
    view.col = (u32)(col - 1);
    view.want = view.col;
}
