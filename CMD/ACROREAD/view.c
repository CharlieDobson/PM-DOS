/*
 * VIEW.C - the viewer: a page on the screen, and the keys and the mouse
 * that move about it.
 *
 * TWO VIEWS OF A PAGE.  The picture is the page drawn (INTERP.C) into
 * a canvas and the canvas put on a graphics screen; the text is the
 * page's characters laid out in lines (TEXT.C) on the text screen.
 * Tab goes from either to the other and stays on the page.
 *
 * THE CANVAS IS AS MUCH OF THE PAGE AS THERE IS MEMORY FOR.  All of it
 * if it fits, and then moving about the page costs nothing; if not, a
 * piece the width of the screen and as tall as can be had, drawn again
 * when the screen is moved off it.  What the drawing itself needs -
 * the page's fonts, a decompressor's window, an image's rows - is left
 * free first (WORK_BYTES), and on a machine where a screenful in
 * colour would not leave that, the canvas is grey: a page in grey is
 * better than half a page in colour.  A page being drawn is shown
 * every second or so, and a key ends the drawing: the page asked for
 * next is the one that matters.
 *
 * A GRAPHICS SCREEN IS PUT TOGETHER A LINE AT A TIME.  The page has all
 * of it but a line at the head with the menus' names, a scroll bar down
 * the right, another under the page when the page is wider than its
 * part of the screen, and a line at the foot that says where we are and
 * has the buttons.  None of these is drawn into another: compose() is
 * asked for each line of the screen as VIDEO.C sends it, and lays the
 * canvas, the bars, a menu that is down and the pointer into it.  So
 * the pointer moving is the lines under it sent again, a scroll bar's
 * box moving is nothing but the screen shown, and a menu put away is
 * the lines it lay over sent without it.
 *
 * THE MENUS are the keys over again, for somebody with a mouse who has
 * not learned them: a line of a menu is a key, and choosing the line
 * hands the loop that key.  F10, or Alt and a menu's letter, or a press
 * on its name lets one down.  Which menus there are, and which lines,
 * goes by the view - the text has nothing to zoom and the picture
 * nothing to find.
 *
 * THE MOUSE.  On a graphics screen the pointer is drawn here - the
 * kernel's driver draws one on a text screen and on no VBE one - and
 * the driver is only asked where the mouse is.  A press is answered
 * where it happened: INT 33h counts presses and says where the last one
 * was, so a click is not lost for the button being up again by the time
 * anybody looks.  On the page a press takes hold of the page, on a
 * scroll bar's box of the box, and anywhere else it is a key - an
 * arrow's is the arrow key's, and is repeated while the button is held.
 * While the page is held nothing is drawn that the canvas has not got;
 * the page is drawn when it is let go.
 *
 * TEST MODE.  With standard input redirected the keys come from it -
 * a byte for a plain key, 0 and the scan code for a function key, FFh
 * and a count for that many seconds' wait, FEh for the mouse (the
 * buttons, then where: two bytes across and two down, in pixels on a
 * graphics screen and in columns and rows on a text one) - and nothing
 * interrupts a drawing, so that what a key file leaves on the screen is
 * the same every time.
 */
#include "gfx.h"
#include "view.h"

#define SCR_W       640
#define SCR_H       480
#define SB          16              /* a scroll bar: how thick, and an arrow's box */
#define TOP_H       16              /* the line at the head: the menus' names */
#define BAR_H       16              /* the line at the foot */
#define BAR_Y       (SCR_H - BAR_H) /* where the line at the foot starts */
#define AREA_W      (SCR_W - SB)    /* the page's part of a graphics screen: across, */
#define PAGE_H      (BAR_Y - TOP_H) /* and down when no scroll bar is under it */
#define BOX_MIN     12              /* a scroll bar's box is never shorter */
#define HOLD_FIRST  400             /* milliseconds a button is held before its key repeats, */
#define HOLD_NEXT   70              /* and from one repeat to the next */
#define NAME_W      200             /* the file's name, in the line at the foot */
#define BACK_GREY   0x58
#define WORK_BYTES  (700UL * 1024)  /* left free for drawing a page with */

/* the colours of the furniture, 00RRGGBB */
#define C_BAR       0x202830UL
#define C_BUTTON    0x40506AUL
#define C_LABEL     0xFFFFFFUL
#define C_TROUGH    0x8C9298UL
#define C_FACE      0xD4D8DCUL
#define C_EDGE      0x383C40UL
#define C_ARROW     0x000000UL

#define PTR_W       12
#define PTR_H       19

#define KEY_ESC     0x001B
#define KEY_ENTER   0x000D
#define KEY_TAB     0x0009
#define KEY_BKSP    0x0008
#define KEY_UP      0x4800
#define KEY_DOWN    0x5000
#define KEY_LEFT    0x4B00
#define KEY_RIGHT   0x4D00
#define KEY_PGUP    0x4900
#define KEY_PGDN    0x5100
#define KEY_HOME    0x4700
#define KEY_END     0x4F00
#define KEY_CPGUP   0x8400
#define KEY_CPGDN   0x7600
#define KEY_CHOME   0x7700
#define KEY_CEND    0x7500
#define KEY_F1      0x3B00
#define KEY_F3      0x3D00
#define KEY_F10     0x4400
#define KEY_ALTX    0x2D00
#define KEY_ALTF    0x2100
#define KEY_ALTV    0x2F00
#define KEY_ALTP    0x1900
#define KEY_ALTS    0x1F00
#define KEY_ALTH    0x2300

/* not keys: what the mouse did, said the way a key is */
#define EV_MOUSE    0x10000         /* a key file's mouse record: look at the mouse */
#define EV_VIEW     0x10001         /* the view moved, or is to be shown again */
#define EV_UP       0x10002         /* a screenful, and no further than the page */
#define EV_DOWN     0x10003
#define EV_LEFT     0x10004
#define EV_RIGHT    0x10005
#define EV_FIT      0x10006         /* fitted to the width, or the whole page */
#define EV_MENU     0x10010         /* a press on a menu's name: this, and the menu's number */

/* what event_get waits for */
#define WAIT_KEYS   0               /* a key: the pointer follows the mouse, and that is all */
#define WAIT_ALL    1               /* a key, or what the mouse does to the view */
#define WAIT_CLICK  2               /* a key, or a press anywhere (EV_VIEW) */

/* where a press landed */
#define HIT_NONE    0
#define HIT_PAGE    1
#define HIT_BAR     2               /* the line at the foot */
#define HIT_TITLE   3               /* the line at the head */
#define HIT_V       0x10            /* the scroll bar at the side, and which part of it */
#define HIT_H       0x20            /* the one underneath */
#define PART_BACK   0               /* the arrow at the top, or the left */
#define PART_BEFORE 1               /* the trough before the box */
#define PART_BOX    2
#define PART_AFTER  3
#define PART_ON     4               /* the arrow at the far end */

/* what the held button has hold of */
#define DRAG_PAGE   1
#define DRAG_V      2
#define DRAG_H      3

#define ATTR_TEXT   0x07
#define ATTR_BAR    0x70
#define ATTR_FOUND  0x0F
#define ATTR_PICK   0x0F            /* the menu that is down, and the line of it pointed at */
#define ATTR_HOT    0x7F            /* the letter that chooses a menu or a line */

/* a menu on a graphics screen */
#define MENU_SIZE   13              /* the size the names and the lines are written at */
#define ITEM_H      16              /* a line of a menu, */
#define RULE_H      8               /* and a line across it */
#define DROP_W      240             /* no menu is wider than this, or taller than that */
#define DROP_H      160
#define MLINES      12              /* nor has more lines */

typedef struct {
    u16 line, start, len;
} TROW;

static const char *file_name;
static int    test_keys;
static int    gfx_kind;             /* the graphics screen there is: VS_VGA, VS_VBE, or 0 */
static int    in_text;
static int    page_no, page_total;
static int    quit;
static char   notice[80];           /* said once, in the bar */

/* the picture */
static PAGE   page;
static VIEW   view;
static fx     zoom;
static int    fit = 1;              /* 1: to the width, 2: the whole page, 0: as set */
static int    turn;
static int    view_x, view_y;       /* the page pixel at the screen's top left */
static int    area_h = PAGE_H;      /* the page's part of the screen, as it now is */
static int    v_box, v_len;         /* the scroll bars' boxes, as last shown */
static int    h_box, h_len;
static CANVAS canvas;
static u32    canvas_cap;
static int    canvas_page = -1, canvas_turn;
static fx     canvas_zoom;
static CANVAS bar;
static int    bar_whole;            /* the bar is all one message: no buttons on it */
static CANVAS top;                  /* the line at the head, with the menus' names */
static CANVAS drop;                 /* a menu let down: */
static int    drop_on, drop_x;      /* whether it is on the screen, and where across */
static u8    *top_pix, *drop_pix;   /* what the two are drawn in */
static FONT   ui_font;
static u32    last_shown;
static int    grey_told;

/* the text */
static int    rows, cols;
static int    text_cols;            /* a row's width: the screen's, less the scroll bar */
static int    text_widest;          /* the longest row */
static int    tbody;                /* rows of text on the screen */
static int    thbar;                /* a scroll bar under them, for rows left long */
static int    tv_box, tv_len;       /* the scroll bars' boxes, in cells */
static int    th_box, th_len;
static int    title_page;           /* the column the top line's page number starts at */
static int    drop_col, drop_wide;  /* a menu let down on the text screen: where, in cells */
static char  *text_block;
static char **text_lines;
static int    text_count;
static int    text_page = -1;
static TROW  *trows;
static int    trow_count;
static int    wrap = 1;
static int    text_top, text_left;
static int    found_row = -1;
static char   find_text[40];

static u16   *saved_cells;
static int    saved_row, saved_col, saved_rows, saved_cols;

/* the mouse */
static int    mouse_on;
static int    mouse_xmax, mouse_ymax;
static int    ms_x, ms_y, ms_down;              /* as last read */
static int    press_seen, press_x, press_y;     /* a press nothing has answered yet */
static int    ptr_on, ptr_x, ptr_y;             /* the pointer on a graphics screen */
static int    drag;                             /* DRAG_*, or 0 */
static int    drag_x, drag_y;                   /* where the pointer took hold */
static int    drag_at;                          /* a box: the pointer when it took hold */
static int    drag_view_x, drag_view_y;         /* and where the page was then */
static int    hold_key, hold_hit;               /* a key the held button repeats, and where */
static u32    hold_next;

static void bar_show( const char *message );
static void pointer_to( int xpos, int ypos );
static int  gfx_mouse( void );
static int  text_mouse( void );
static void text_show( void );

const char *why_text( int why )
{
    switch ( why ) {
    case PE_NOMEM:    return "Not enough memory";
    case PE_DAMAGED:  return "The file is damaged and cannot be read";
    case PE_NOTPDF:   return "Not a PDF file";
    case PE_PASSWORD: return "A password is needed";
    case PE_CRYPT:    return "The file is encrypted in a way ACROREAD cannot read";
    case PE_IO:       return "The file cannot be read";
    }
    return "";
}

/* ------------------------------------------------------------------ */
/* a scroll bar, in pixels or in cells                                 */
/* ------------------------------------------------------------------ */

/* The box in a trough "trough" long: where it starts and how long it
   is when "seen" of "all" is on the screen from "at" on.  No box when
   all of it is on the screen. */
static void box_of( int trough, s32 all, s32 seen, s32 at, int least, int *start, int *len )
{
    *start = *len = 0;
    if ( all <= seen || trough <= least ) {
        return;
    }
    *len = (int)((s32)trough * seen / all);
    if ( *len < least ) {
        *len = least;
    }
    if ( at < 0 ) {
        at = 0;
    }
    if ( at > all - seen ) {
        at = all - seen;
    }
    *start = (int)((s32)(trough - *len) * at / (all - seen));
}

/* and back: what is first on the screen with the box starting at "start" */
static s32 box_to( int trough, s32 all, s32 seen, int len, int start )
{
    if ( all <= seen || trough <= len ) {
        return 0;
    }
    if ( start < 0 ) {
        start = 0;
    }
    if ( start > trough - len ) {
        start = trough - len;
    }
    return (s32)start * (all - seen) / (trough - len);
}

/* which part of a bar "length" long is "at": the arrows are "arrow" long */
static int bar_part( int at, int length, int arrow, int box, int box_len )
{
    if ( at < arrow ) {
        return PART_BACK;
    }
    if ( at >= length - arrow ) {
        return PART_ON;
    }
    at -= arrow;
    return at < box ? PART_BEFORE : (at < box + box_len ? PART_BOX : PART_AFTER);
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

/* before the screen changes: the driver's pointer off the one there is */
static void mouse_rest( void )
{
    if ( mouse_on ) {
        sys_mouse_reset();
    }
    ptr_on = 0;
}

/* After every change of screen: the driver works the screen out again
   when it is reset.  The pointer goes back where it was, as nearly as
   two screens of different sizes allow. */
static void mouse_setup( void )
{
    int was_x = ms_x, was_y = ms_y, was_xmax = mouse_xmax, was_ymax = mouse_ymax;
    int had = mouse_on, buttons, xpos, ypos;

    press_seen = drag = hold_key = ptr_on = 0;
    mouse_on = sys_mouse_reset() != 0;
    if ( !mouse_on ) {
        return;
    }
    if ( screen.kind == VS_TEXT ) {
        sys_mouse_extent( &mouse_xmax, &mouse_ymax );
        if ( mouse_xmax < 1 || mouse_ymax < 1 ) {
            mouse_xmax = 639;
            mouse_ymax = 199;
        }
    } else {
        mouse_xmax = SCR_W - 1;
        mouse_ymax = SCR_H - 1;
        sys_mouse_range( mouse_xmax, mouse_ymax );
    }
    if ( had ) {
        sys_mouse_move( was_x * (mouse_xmax + 1) / (was_xmax + 1), was_y * (mouse_ymax + 1) / (was_ymax + 1) );
    } else if ( screen.kind != VS_TEXT ) {
        sys_mouse_move( SCR_W / 2, SCR_H / 2 );
    }
    sys_mouse_presses( &xpos, &ypos );              /* nothing from before */
    sys_mouse_state( &ms_x, &ms_y, &buttons );
    ms_down = buttons & 1;
    if ( screen.kind == VS_TEXT ) {
        sys_mouse_show( 1 );
    } else {
        ptr_x = ms_x;
        ptr_y = ms_y;
        ptr_on = 1;
    }
}

/* what the driver has to say since the last look */
static void mouse_read( void )
{
    int xpos, ypos, buttons;

    if ( sys_mouse_presses( &xpos, &ypos ) ) {
        press_seen = 1;
        press_x = xpos;
        press_y = ypos;
    }
    sys_mouse_state( &ms_x, &ms_y, &buttons );
    ms_down = buttons & 1;
}

static void mouse_cell( int xpos, int ypos, int *row, int *col )
{
    *col = xpos * cols / (mouse_xmax + 1);
    *row = ypos * rows / (mouse_ymax + 1);
}

/* A key the button repeats for as long as it is held where it is.  The
   time is sys_clock_ms's, which is finer than DOS's: see there. */
static void hold_begin( int key, int hit )
{
    hold_key = test_keys ? 0 : key;
    hold_hit = hit;
    hold_next = sys_clock_ms() + HOLD_FIRST;
}

/* the key again, if it is time and the pointer is still on the part of
   the bar that gave it - a trough stops giving it when the box has come
   under the pointer */
static int hold_due( int hit )
{
    u32 now;

    if ( !hold_key ) {
        return 0;
    }
    now = sys_clock_ms();                           /* asked every time: that is how it counts */
    if ( hit != hold_hit || (s32)(now - hold_next) < 0 ) {
        return 0;
    }
    hold_next = now + HOLD_NEXT;
    return hold_key;
}

/* ------------------------------------------------------------------ */
/* keys                                                                */
/* ------------------------------------------------------------------ */

static int key_get( void )
{
    u32 until;
    int byte, shift, buttons, xpos, ypos;

    if ( !test_keys ) {
        byte = sys_key_read( &shift );
        /* a grey key says E0h where the white one says 0 */
        if ( (byte & 0xFF) == 0 || ((byte & 0xFF) == 0xE0 && (byte & 0xFF00)) ) {
            return byte & 0xFF00;
        }
        return byte & 0xFF;
    }
    for ( ;; ) {
        byte = sys_stdin_byte();
        if ( byte < 0 ) {
            return KEY_ESC;                         /* the key file ran out */
        }
        if ( byte == 0 ) {
            return (sys_stdin_byte() & 0xFF) << 8;
        }
        if ( byte == 0xFE ) {
            /* the pointer put there and the buttons set, and what that
               amounts to read back like anything the mouse does */
            buttons = sys_stdin_byte() & 7;
            xpos = sys_stdin_byte() & 0xFF;
            xpos |= (sys_stdin_byte() & 0xFF) << 8;
            ypos = sys_stdin_byte() & 0xFF;
            ypos |= (sys_stdin_byte() & 0xFF) << 8;
            if ( mouse_on ) {
                if ( screen.kind == VS_TEXT ) {
                    xpos = xpos * (mouse_xmax + 1) / cols;
                    ypos = ypos * (mouse_ymax + 1) / rows;
                }
                sys_mouse_move( xpos, ypos );
                sys_mouse_inject( buttons );
            }
            return EV_MOUSE;
        }
        if ( byte != 0xFF ) {
            return byte;
        }
        until = sys_hundredths() + (u32)(sys_stdin_byte() & 0xFF) * 100;
        while ( sys_hundredths() < until ) {
        }
    }
}

static int key_waiting( void )
{
    return !test_keys && sys_key_ready();
}

/* a key typed, or a press made, that nothing has answered yet */
static int input_waiting( void )
{
    return key_waiting() || press_seen;
}

/* A key - or, if "mode" asks for them, something the mouse did, said
   as a key.  The pointer follows the mouse whatever is asked for. */
static int event_get( int mode )
{
    int key, got;

    for ( ;; ) {
        if ( mouse_on && press_seen && mode == WAIT_ALL ) {
            /* a press seen while a page was being drawn: nothing to wait for */
        } else if ( test_keys || !mouse_on ) {
            key = key_get();
            if ( key != EV_MOUSE ) {
                return key;
            }
            if ( !mouse_on ) {
                continue;
            }
            mouse_read();
        } else {
            /* a held button that repeats a key has no news to wake us with */
            if ( sys_event( EVF_MOUSE | (hold_key ? EVF_NOWAIT : 0) ) == 1 ) {
                return key_get();
            }
            mouse_read();
        }
        pointer_to( ms_x, ms_y );
        if ( mode == WAIT_ALL ) {
            key = in_text ? text_mouse() : gfx_mouse();
            if ( key ) {
                return key;
            }
        } else {
            got = press_seen;
            press_seen = drag = hold_key = 0;
            if ( got && mode == WAIT_CLICK ) {
                return EV_VIEW;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* the menus: what is in them                                          */
/* ------------------------------------------------------------------ */

#define MV_PICTURE  1               /* a line is there in the picture, */
#define MV_TEXT     2               /* in the text, */
#define MV_BOTH     3               /* or in both; */
#define MV_WRAPPED  4               /* and only while the text's long lines are wrapped, */
#define MV_LONG     8               /* or only while they are left long */

typedef struct {
    const char *label;              /* '&' before the letter that chooses it; NULL is a line across */
    const char *keys;               /* the key for the same thing, said beside it */
    int key;                        /* what the loop is handed */
    int where;
} MITEM;

typedef struct {
    const char *title;
    int alt;                        /* Alt and its letter */
    const MITEM *items;
    int count;
} MENU;

static const MITEM file_items[] = {
    { "E&xit", "Esc", KEY_ESC, MV_BOTH } };
static const MITEM view_items[] = {
    { "Zoom &In", "+", '+', MV_PICTURE },
    { "Zoom &Out", "-", '-', MV_PICTURE },
    { "&Actual Size", "1", '1', MV_PICTURE },
    { "Fit &Width", "W", 'w', MV_PICTURE },
    { "Whole &Page", "F", 'f', MV_PICTURE },
    { "Turn &Right", "R", 'r', MV_PICTURE },
    { "Turn &Left", "L", 'l', MV_PICTURE },
    { "Leave Lines &Long", "W", 'w', MV_TEXT | MV_WRAPPED },
    { "&Wrap Long Lines", "W", 'w', MV_TEXT | MV_LONG },
    { NULL, NULL, 0, MV_BOTH },
    { "&Text", "Tab", KEY_TAB, MV_PICTURE },
    { "&Picture", "Tab", KEY_TAB, MV_TEXT } };
static const MITEM page_items[] = {
    { "&Next Page", "N", 'n', MV_BOTH },
    { "&Previous Page", "P", 'p', MV_BOTH },
    { "&First Page", "Ctrl+Home", KEY_CHOME, MV_BOTH },
    { "&Last Page", "Ctrl+End", KEY_CEND, MV_BOTH },
    { NULL, NULL, 0, MV_BOTH },
    { "&Go to Page...", "G", 'g', MV_BOTH } };
static const MITEM search_items[] = {
    { "&Find...", "F", 'f', MV_TEXT },
    { "Find &Again", "F3", KEY_F3, MV_TEXT } };
static const MITEM help_items[] = {
    { "&Keys", "F1", KEY_F1, MV_BOTH } };

#define MENUS       5
static const MENU menus[MENUS] = {
    { "&File", KEY_ALTF, file_items, sizeof( file_items ) / sizeof( MITEM ) },
    { "&View", KEY_ALTV, view_items, sizeof( view_items ) / sizeof( MITEM ) },
    { "&Page", KEY_ALTP, page_items, sizeof( page_items ) / sizeof( MITEM ) },
    { "&Search", KEY_ALTS, search_items, sizeof( search_items ) / sizeof( MITEM ) },
    { "&Help", KEY_ALTH, help_items, sizeof( help_items ) / sizeof( MITEM ) } };

static const MITEM *mline[MLINES];  /* the lines of the menu that is down, as they now are, */
static int mlines;
static int mline_y[MLINES];         /* and how far down the picture of it each one starts */
static int title_from[MENUS];       /* where each name is on the top line, in pixels or in */
static int title_to[MENUS];         /* columns: nowhere, if the two are the same */
static int top_made = -2;           /* the menu marked in the picture's top line as it is drawn:
                                       -2, and it is to be drawn again */

/* a label without its '&', and where the letter after it is: -1 if it
   has none */
static int label_plain( const char *label, char *out, int max )
{
    int len = 0, hot = -1;

    for ( ; *label && len < max - 1; label++ ) {
        if ( *label == '&' ) {
            hot = len;
        } else {
            out[len++] = *label;
        }
    }
    out[len] = 0;
    return hot;
}

static int letter_of( int key )
{
    return key >= 'a' && key <= 'z' ? key - 'a' + 'A' : key;
}

/* is a line there, as things now are? */
static int item_shown( const MITEM *item )
{
    if ( !(item->where & (in_text ? MV_TEXT : MV_PICTURE)) ) {
        return 0;
    }
    if ( (item->where & MV_WRAPPED) && !wrap ) {
        return 0;
    }
    return !((item->where & MV_LONG) && wrap);
}

/* how many lines a menu has to choose from: none, and it is not there */
static int menu_count( int index )
{
    int at, count = 0;

    if ( index < 0 || index >= MENUS ) {
        return 0;
    }
    for ( at = 0; at < menus[index].count; at++ ) {
        if ( menus[index].items[at].label && item_shown( &menus[index].items[at] ) ) {
            count++;
        }
    }
    return count;
}

/* the lines of a menu, as it is about to be let down: a line across is
   left out at the head, at the foot and next to another */
static void menu_fill( int index )
{
    const MITEM *item;
    int at;

    mlines = 0;
    for ( at = 0; at < menus[index].count && mlines < MLINES; at++ ) {
        item = &menus[index].items[at];
        if ( !item_shown( item ) ) {
            continue;
        }
        if ( item->label == NULL && (mlines == 0 || mline[mlines - 1]->label == NULL) ) {
            continue;
        }
        mline[mlines++] = item;
    }
    while ( mlines && mline[mlines - 1]->label == NULL ) {
        mlines--;
    }
}

/* the line to choose after "from", going "step": lines across are
   passed over, and the end goes round to the other end */
static int menu_step( int from, int step )
{
    int tries;

    for ( tries = 0; tries < mlines; tries++ ) {
        from += step;
        if ( from < 0 ) {
            from = mlines - 1;
        } else if ( from >= mlines ) {
            from = 0;
        }
        if ( mline[from]->label ) {
            return from;
        }
    }
    return -1;
}

/* the menu after "from", going "step", of the menus there are */
static int menu_next( int from, int step )
{
    int tries;

    for ( tries = 0; tries < MENUS; tries++ ) {
        from = (from + step + MENUS) % MENUS;
        if ( menu_count( from ) ) {
            return from;
        }
    }
    return 0;
}

/* the menu whose name is at "at" on the top line - a pixel or a column,
   as the screen is - or -1 */
static int title_at( int at )
{
    int index;

    for ( index = 0; index < MENUS; index++ ) {
        if ( at >= title_from[index] && at < title_to[index] ) {
            return index;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* the text screen's furniture                                         */
/* ------------------------------------------------------------------ */

static void put_text( int row, int col, int attr, const char *text, int width )
{
    static u16 cells[160];
    int index;

    if ( width > 160 ) {
        width = 160;
    }
    for ( index = 0; index < width; index++ ) {
        if ( *text ) {
            cells[index] = (u16)((attr << 8) | (u8)*text++);
        } else {
            cells[index] = (u16)((attr << 8) | ' ');
        }
    }
    sys_put_cells( (u32)(row * cols + col), cells, (u32)width );
}

/* a cell of a text scroll bar: "back" and "on" are its two arrows */
static u16 bar_cell( int part, int back, int on )
{
    switch ( part ) {
    case PART_BACK: return (u16)((ATTR_BAR << 8) | back);
    case PART_ON:   return (u16)((ATTR_BAR << 8) | on);
    case PART_BOX:  return (u16)((ATTR_FOUND << 8) | 0xDB);
    }
    return (u16)((ATTR_TEXT << 8) | 0xB0);
}

/* The top line of the text screen: the menus' names, with the one
   numbered "open" marked as being down, then the file's name, and the
   page at the far end. */
static void text_title( int open )
{
    char text[100], name[24];
    int index, col = 1, hot, len;

    top_made = -2;                                  /* the names' places are this screen's now */
    put_text( 0, 0, ATTR_BAR, "", cols );
    for ( index = 0; index < MENUS; index++ ) {
        title_from[index] = title_to[index] = 0;
        if ( !menu_count( index ) ) {
            continue;
        }
        name[0] = ' ';
        hot = label_plain( menus[index].title, name + 1, sizeof( name ) - 2 );
        len = (int)str_len( name );
        name[len++] = ' ';
        name[len] = 0;
        put_text( 0, col, index == open ? ATTR_PICK : ATTR_BAR, name, len );
        if ( hot >= 0 && index != open ) {
            put_text( 0, col + 1 + hot, ATTR_HOT, name + 1 + hot, 1 );
        }
        title_from[index] = col;
        title_to[index] = col + len;
        col += len;
    }
    sfmt( text, sizeof( text ), "Page %d of %d ", page_no + 1, page_total );
    title_page = cols - (int)str_len( text );
    put_text( 0, title_page, ATTR_BAR, text, (int)str_len( text ) );
    col += 3;
    if ( title_page - 2 > col ) {
        put_text( 0, col, ATTR_BAR, file_name, title_page - 2 - col );
    }
}

/* The menu that is down, drawn under its name, with line "pick" marked:
   a box of the line-drawing characters, a row to a line. */
static void text_drop( int open, int pick )
{
    char text[84], label[40];
    int index, inner = 0, len, hot, keys;

    for ( index = 0; index < mlines; index++ ) {
        if ( mline[index]->label ) {
            label_plain( mline[index]->label, label, sizeof( label ) );
            len = (int)(str_len( label ) + 2 + str_len( mline[index]->keys ));
            if ( len > inner ) {
                inner = len;
            }
        }
    }
    drop_wide = inner + 4;
    drop_col = title_from[open] - 1;
    if ( drop_col + drop_wide > cols ) {
        drop_col = cols - drop_wide;
    }
    if ( drop_col < 0 ) {
        drop_col = 0;
    }
    mem_set( text, 0xC4, (u32)drop_wide );
    text[drop_wide] = 0;
    text[0] = (char)0xDA;
    text[drop_wide - 1] = (char)0xBF;
    put_text( 1, drop_col, ATTR_BAR, text, drop_wide );
    for ( index = 0; index < mlines; index++ ) {
        if ( mline[index]->label == NULL ) {
            mem_set( text, 0xC4, (u32)drop_wide );
            text[0] = (char)0xC3;
            text[drop_wide - 1] = (char)0xB4;
            put_text( 2 + index, drop_col, ATTR_BAR, text, drop_wide );
            continue;
        }
        mem_set( text, ' ', (u32)drop_wide );
        text[0] = text[drop_wide - 1] = (char)0xB3;
        hot = label_plain( mline[index]->label, label, sizeof( label ) );
        mem_cpy( text + 2, label, str_len( label ) );
        keys = (int)str_len( mline[index]->keys );
        mem_cpy( text + drop_wide - 2 - keys, mline[index]->keys, (u32)keys );
        put_text( 2 + index, drop_col, ATTR_BAR, text, drop_wide );
        if ( index == pick ) {
            put_text( 2 + index, drop_col + 1, ATTR_PICK, text + 1, drop_wide - 2 );
        } else if ( hot >= 0 ) {
            put_text( 2 + index, drop_col + 2 + hot, ATTR_HOT, label + hot, 1 );
        }
    }
    mem_set( text, 0xC4, (u32)drop_wide );
    text[0] = (char)0xC0;
    text[drop_wide - 1] = (char)0xD9;
    put_text( 2 + mlines, drop_col, ATTR_BAR, text, drop_wide );
}

/* a line typed into the bottom row: 0 if it was given up */
static int ask( const char *prompt, char *buf, int max, int digits )
{
    char shown[100];
    int len = 0, key;

    buf[0] = 0;
    for ( ;; ) {
        sfmt( shown, sizeof( shown ), "%s%s_", prompt, buf );
        if ( in_text ) {
            put_text( rows - 1, 0, ATTR_BAR, shown, cols );
        } else {
            bar_show( shown );
        }
        key = event_get( WAIT_KEYS );
        if ( key == KEY_ESC ) {
            return 0;
        }
        if ( key == KEY_ENTER ) {
            return len > 0;
        }
        if ( key == KEY_BKSP ) {
            if ( len ) {
                buf[--len] = 0;
            }
        } else if ( key >= 32 && key < 127 && len < max - 1 && (!digits || (key >= '0' && key <= '9')) ) {
            buf[len++] = (char)key;
            buf[len] = 0;
        }
    }
}

static void help( void )
{
    static const char *const lines[] = {
        "  PgDn, Space           Down the page, and on to the next page",
        "  PgUp, Backspace       Up the page, and back to the page before",
        "  Arrow keys            Move a little",
        "  Home, End             The top or the bottom of the page",
        "  N, P                  The next or the previous page",
        "  Ctrl+Home, Ctrl+End   The first or the last page",
        "  G                     Go to a page by its number",
        "  Tab                   Change between the picture and the text",
        "  Esc, Q                Leave ACROREAD",
        "",
        "  In the picture:",
        "  +, -                  Larger or smaller",
        "  W, F                  Fit the page's width, or the whole page, to the screen",
        "  R, L                  Turn the page right or left",
        "",
        "  In the text:",
        "  F, F3                 Find text, and find it again",
        "  W                     Wrap long lines, or leave them long",
        "",
        "  The top line          The menus: click a name, or press F10",
        "  The picture           Drag the picture to move it",
        "  A scroll bar          Click an arrow, or drag the box",
        "  The bottom line       Click a button or a word"
    };
    int index, was_gfx = screen.kind != VS_TEXT;

    if ( was_gfx ) {
        video_text();
        sys_screen_size( &rows, &cols );
        mouse_setup();
    }
    sys_cursor_shape( CUR_HIDE );
    put_text( 0, 0, ATTR_BAR, " ACROREAD Help", cols );
    for ( index = 1; index < rows - 1; index++ ) {
        put_text( index, 0, ATTR_TEXT, index <= (int)(sizeof( lines ) / sizeof( lines[0] )) ? lines[index - 1] : "",
                  cols );
    }
    put_text( rows - 1, 0, ATTR_BAR, mouse_on ? " Press a key, or click the mouse" : " Press a key", cols );
    event_get( WAIT_CLICK );
    if ( was_gfx ) {
        mouse_rest();
        if ( video_set( gfx_kind ) ) {
            mouse_setup();
        } else {
            gfx_kind = 0;
            in_text = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* a line of the graphics screen                                       */
/* ------------------------------------------------------------------ */

static const char *const pointer_shape[PTR_H] = {
    "k...........",
    "kk..........",
    "kwk.........",
    "kwwk........",
    "kwwwk.......",
    "kwwwwk......",
    "kwwwwwk.....",
    "kwwwwwwk....",
    "kwwwwwwwk...",
    "kwwwwwwwwk..",
    "kwwwwwwwwwk.",
    "kwwwwwwkkkkk",
    "kwwwkwwk....",
    "kwwkkwwk....",
    "kwk..kwwk...",
    "kk...kwwk...",
    "k.....kwwk..",
    "......kwwk..",
    ".......kk..." };

/* pixels "from" up to "to" of a line, one colour: the line is the
   screen's kind, a byte of grey a pixel or three of colour */
static void px_fill( u8 *out, int from, int to, u32 rgb )
{
    if ( to <= from ) {
        return;
    }
    if ( screen.canvas_bpp == 1 ) {
        mem_set( out + from, (int)rgb_grey( rgb ), (u32)(to - from) );
        return;
    }
    for ( out += from * 3; from < to; from++, out += 3 ) {
        out[0] = (u8)rgb;
        out[1] = (u8)(rgb >> 8);
        out[2] = (u8)(rgb >> 16);
    }
}

/* Line "line" of an arrow's box, SB pixels square, at "out".  "dir" is
   0 for up, 1 down, 2 left, 3 right: a triangle eight pixels across
   its foot and four from foot to point. */
static void arrow_row( u8 *out, int dir, int line )
{
    int inset, from = 0, to = 0;

    px_fill( out, 0, SB, line == 0 || line == SB - 1 ? C_EDGE : C_FACE );
    px_fill( out, 0, 1, C_EDGE );
    px_fill( out, SB - 1, SB, C_EDGE );
    if ( dir < 2 ) {
        inset = dir == 0 ? line - 6 : 9 - line;
        if ( inset >= 0 && inset < 4 ) {
            from = 7 - inset;
            to = 9 + inset;
        }
    } else {
        inset = line < 8 ? 7 - line : line - 8;
        if ( inset < 4 ) {
            from = dir == 2 ? 6 + inset : 6;
            to = dir == 2 ? 10 : 10 - inset;
        }
    }
    px_fill( out, from, to, C_ARROW );
}

/* line "ypos" of the scroll bar at the side, SB pixels at "out" */
static void vbar_row( int ypos, u8 *out )
{
    int at;

    if ( ypos < SB ) {
        arrow_row( out, 0, ypos );
        return;
    }
    if ( ypos >= area_h - SB ) {
        arrow_row( out, 1, ypos - (area_h - SB) );
        return;
    }
    px_fill( out, 0, SB, C_TROUGH );
    px_fill( out, 0, 1, C_EDGE );
    at = ypos - SB - v_box;
    if ( at >= 0 && at < v_len ) {
        px_fill( out, 1, SB, at == 0 || at == v_len - 1 ? C_EDGE : C_FACE );
        px_fill( out, SB - 1, SB, C_EDGE );
    }
}

/* line "line" of the scroll bar under the page, AREA_W pixels at "out" */
static void hbar_row( int line, u8 *out )
{
    int bpp = screen.canvas_bpp, from = SB + h_box, to = SB + h_box + h_len;

    px_fill( out, SB, AREA_W - SB, line == 0 ? C_EDGE : C_TROUGH );
    arrow_row( out, 2, line );
    arrow_row( out + (AREA_W - SB) * bpp, 3, line );
    if ( h_len && line > 0 ) {
        px_fill( out, from, to, line == SB - 1 ? C_EDGE : C_FACE );
        px_fill( out, from, from + 1, C_EDGE );
        px_fill( out, to - 1, to, C_EDGE );
    }
}

/* line "ypos" of the page's part of the screen: the canvas where there
   is canvas, and the desk the page lies on where there is not */
static void page_row( int ypos, u8 *out )
{
    const u8 *src;
    int bpp = screen.canvas_bpp, from, to, skip = 0, line, count;

    mem_set( out, BACK_GREY, (u32)(AREA_W * bpp) );
    if ( canvas.pix == NULL ) {
        return;
    }
    line = view_y + ypos - canvas.org_y;
    if ( line < 0 || line >= canvas.height ) {
        return;
    }
    from = canvas.org_x - view_x;                   /* where the canvas's first column falls */
    to = from + canvas.width;
    if ( from < 0 ) {
        skip = -from;
        from = 0;
    }
    if ( to > AREA_W ) {
        to = AREA_W;
    }
    if ( to <= from ) {
        return;
    }
    count = to - from;
    src = canvas.pix + (u32)line * canvas.stride + (u32)(skip * canvas.bpp);
    out += from * bpp;
    if ( canvas.bpp == bpp ) {
        mem_cpy( out, src, (u32)(count * bpp) );
    } else if ( bpp == 3 ) {
        /* a grey canvas on a screen of colours */
        for ( ; count > 0; count--, src++, out += 3 ) {
            out[0] = out[1] = out[2] = *src;
        }
    } else {
        for ( ; count > 0; count--, src += 3, out++ ) {
            *out = (u8)((src[2] * 77 + src[1] * 150 + src[0] * 29) >> 8);
        }
    }
}

/* what VIDEO.C asks for: line "ypos" of the screen, whole */
static void compose( int ypos, u8 *row )
{
    const char *shape;
    u8 *out;
    int bpp = screen.canvas_bpp, col, level, line = ypos - TOP_H;

    if ( line < 0 ) {
        if ( top.pix && top.bpp == bpp ) {
            mem_cpy( row, top.pix + (u32)ypos * top.stride, (u32)(SCR_W * bpp) );
        } else {
            px_fill( row, 0, SCR_W, C_BAR );
        }
    } else if ( ypos >= BAR_Y ) {
        if ( bar.pix && bar.bpp == bpp ) {
            mem_cpy( row, bar.pix + (u32)(ypos - BAR_Y) * bar.stride, (u32)(SCR_W * bpp) );
        } else {
            px_fill( row, 0, SCR_W, C_BAR );
        }
    } else if ( line >= area_h ) {
        hbar_row( line - area_h, row );
        px_fill( row, AREA_W, SCR_W, C_FACE );      /* the corner the two bars leave */
    } else {
        page_row( line, row );
        vbar_row( line, row + AREA_W * bpp );
    }
    /* a menu that is down lies over whatever is under the top line */
    if ( drop_on && line >= 0 && line < drop.height && drop.bpp == bpp ) {
        mem_cpy( row + drop_x * bpp, drop.pix + (u32)line * drop.stride, (u32)(drop.width * bpp) );
    }
    if ( ptr_on && ypos >= ptr_y && ypos < ptr_y + PTR_H ) {
        shape = pointer_shape[ypos - ptr_y];
        for ( col = 0; col < PTR_W && ptr_x + col < SCR_W; col++ ) {
            if ( shape[col] == '.' ) {
                continue;
            }
            level = shape[col] == 'k' ? 0 : 255;
            out = row + (ptr_x + col) * bpp;
            out[0] = (u8)level;
            if ( bpp == 3 ) {
                out[1] = out[2] = (u8)level;
            }
        }
    }
}

/* lines "top" up to "bottom" of the screen, as things now are */
static void rows_show( int top, int bottom )
{
    box_of( area_h - 2 * SB, view.height, area_h, view_y, BOX_MIN, &v_box, &v_len );
    box_of( AREA_W - 2 * SB, view.width, AREA_W, view_x, BOX_MIN, &h_box, &h_len );
    video_rows( top, bottom, compose );
}

/* the pointer to where the mouse is: the lines it was on and the lines
   it is on, again */
static void pointer_to( int xpos, int ypos )
{
    int was = ptr_y;

    if ( !ptr_on || screen.kind == VS_TEXT || (xpos == ptr_x && ypos == ptr_y) ) {
        return;
    }
    ptr_x = xpos;
    ptr_y = ypos;
    if ( ypos > was + PTR_H || was > ypos + PTR_H ) {
        rows_show( was, was + PTR_H );
        rows_show( ypos, ypos + PTR_H );
    } else {
        rows_show( was < ypos ? was : ypos, (was > ypos ? was : ypos) + PTR_H );
    }
}

/* ------------------------------------------------------------------ */
/* the line at the foot of the picture                                 */
/* ------------------------------------------------------------------ */

/* What is on the line and what a press there is.  A label of one
   character is a shape drawn; no label is words that change. */
#define ZONES       10
static const struct {
    int x0, x1, key;
    const char *label;
} zones[ZONES] = {
    { 216, 234, KEY_CHOME, "[" },
    { 236, 254, 'p',       "<" },
    { 256, 384, 'g',       NULL },
    { 386, 404, 'n',       ">" },
    { 406, 424, KEY_CEND,  "]" },
    { 452, 470, '-',       "-" },
    { 472, 512, EV_FIT,    NULL },
    { 514, 532, '+',       "+" },
    { 548, 588, KEY_TAB,   "Text" },
    { 592, 634, KEY_F1,    "Help" } };

static void ui_text( CANVAS *cv, int xpos, int base, int size, u32 rgb, const char *text )
{
    CLIP clip;
    PAINT paint;
    s32 mat[4];
    fx pen = I2FX( xpos );

    mem_set( &clip, 0, sizeof( clip ) );
    clip.x1 = cv->width;
    clip.y1 = cv->height;
    mem_set( &paint, 0, sizeof( paint ) );
    paint.rgb = rgb;
    paint.alpha = 255;
    mat[0] = (s32)size * 16777L;                    /* pixels to a thousandth of an em, 8.24 */
    mat[1] = mat[2] = 0;
    mat[3] = -mat[0];
    for ( ; *text; text++ ) {
        glyph_draw( &ui_font, (u8)*text, pen, I2FX( base ), mat, cv, &clip, &paint );
        pen += (fx)(((s32)sfont_width( (u8)*text, 0 ) * size * 65536L) / 1000);
    }
}

static int ui_width( int size, const char *text )
{
    s32 total = 0;

    for ( ; *text; text++ ) {
        total += sfont_width( (u8)*text, 0 );
    }
    return (int)(total * size / 1000);
}

static void bar_fill( int x0, int y0, int x1, int y1, u32 rgb )
{
    for ( ; y0 < y1; y0++ ) {
        px_fill( bar.pix + (u32)y0 * bar.stride, x0, x1, rgb );
    }
}

/* a triangle on the line's middle with its point at column "point":
   "step" is 1 for one pointing left, -1 for right */
static void bar_point( int point, int step )
{
    int index;

    for ( index = 0; index < 4; index++ ) {
        bar_fill( point + index * step, 7 - index, point + index * step + 1, 9 + index, C_LABEL );
    }
}

/* The line drawn - into its own canvas, not onto the screen.  "message"
   if that is something has the whole line; else "status" (or the file's
   name) is at the left and the buttons are there. */
static void bar_paint( const char *status, const char *message )
{
    static char made[400];
    char want[400], text[100];
    const char *label;
    int index, len, mid, percent = (int)((zoom * 100 + 0x8000) >> 16);

    if ( bar.pix == NULL ) {
        bar.pix = (u8 *)xalloc( SCR_W * 3 * BAR_H );
        made[0] = 0;
    }
    if ( status == NULL ) {
        status = file_name;
    }
    if ( message == NULL ) {
        message = "";
    }
    /* drawn already, if what it says has not changed */
    sfmt( want, sizeof( want ), "%d %d %d %d %s|%s", screen.canvas_bpp, page_no, page_total, percent, message,
          status );
    if ( str_cmp( want, made ) == 0 ) {
        return;
    }
    str_cpy( made, want );
    bar.width = SCR_W;
    bar.height = BAR_H;
    bar.bpp = screen.canvas_bpp;
    bar.stride = (u32)SCR_W * (u32)bar.bpp;
    bar.org_x = bar.org_y = 0;
    canvas_clear( &bar, C_BAR );
    bar_whole = message[0] != 0;
    if ( bar_whole ) {
        ui_text( &bar, 8, 12, 13, C_LABEL, message );
        return;
    }
    if ( !str_cpyn( text, status, sizeof( text ) ) ) {
        mem_cpy( text, status, sizeof( text ) - 1 );
        text[sizeof( text ) - 1] = 0;
    }
    for ( len = (int)str_len( text ); len > 0 && ui_width( 13, text ) > NAME_W; ) {
        text[--len] = 0;
    }
    ui_text( &bar, 6, 12, 13, C_LABEL, text );
    for ( index = 0; index < ZONES; index++ ) {
        label = zones[index].label;
        mid = (zones[index].x0 + zones[index].x1) / 2;
        if ( label == NULL ) {
            if ( zones[index].key == 'g' ) {
                sfmt( text, sizeof( text ), "Page %d of %d", page_no + 1, page_total );
            } else {
                sfmt( text, sizeof( text ), "%d%%", percent );
            }
            ui_text( &bar, mid - ui_width( 13, text ) / 2, 12, 13, C_LABEL, text );
            continue;
        }
        bar_fill( zones[index].x0, 2, zones[index].x1, 14, C_BUTTON );
        switch ( label[0] ) {
        case '[':                                   /* the first page: a stop, and a point at it */
            bar_fill( mid - 4, 4, mid - 2, 12, C_LABEL );
            bar_point( mid - 1, 1 );
            break;
        case '<':
            bar_point( mid - 2, 1 );
            break;
        case '>':
            bar_point( mid + 1, -1 );
            break;
        case ']':
            bar_fill( mid + 2, 4, mid + 4, 12, C_LABEL );
            bar_point( mid, -1 );
            break;
        case '+':
            bar_fill( mid - 1, 4, mid + 1, 12, C_LABEL );
            bar_fill( mid - 4, 7, mid + 4, 9, C_LABEL );
            break;
        case '-':
            bar_fill( mid - 4, 7, mid + 4, 9, C_LABEL );
            break;
        default:
            ui_text( &bar, mid - ui_width( 12, label ) / 2, 12, 12, C_LABEL, label );
            break;
        }
    }
}

/* the line alone, with something said or asked on it */
static void bar_show( const char *message )
{
    bar_paint( NULL, message );
    rows_show( BAR_Y, SCR_H );
}

/* ------------------------------------------------------------------ */
/* the menus, in the picture                                           */
/* ------------------------------------------------------------------ */

static void cv_fill( CANVAS *cv, int x0, int y0, int x1, int y1, u32 rgb )
{
    for ( ; y0 < y1; y0++ ) {
        px_fill( cv->pix + (u32)y0 * cv->stride, x0, x1, rgb );
    }
}

/* a canvas of the screen's kind over "pix", all one colour */
static void cv_begin( CANVAS *cv, u8 *pix, int width, int height, u32 rgb )
{
    cv->pix = pix;
    cv->width = width;
    cv->height = height;
    cv->bpp = screen.canvas_bpp;
    cv->stride = (u32)width * (u32)cv->bpp;
    cv->org_x = cv->org_y = 0;
    canvas_clear( cv, rgb );
}

/* a label written at "xpos" on the line whose foot is "base", and the
   letter that chooses it underlined */
static void menu_text( CANVAS *cv, int xpos, int base, u32 rgb, const char *label )
{
    char text[40], one[2];
    int hot = label_plain( label, text, sizeof( text ) );

    ui_text( cv, xpos, base, MENU_SIZE, rgb, text );
    if ( hot >= 0 ) {
        one[0] = text[hot];
        one[1] = 0;
        text[hot] = 0;
        xpos += ui_width( MENU_SIZE, text );
        cv_fill( cv, xpos, base + 2, xpos + ui_width( MENU_SIZE, one ), base + 3, rgb );
    }
}

/* The line at the head drawn - into its own canvas, as the line at the
   foot is: the menus' names, with the one numbered "open" marked as
   being down. */
static void top_paint( int open )
{
    static int made_bpp;
    char text[24];
    int index, xpos = 4, wide;

    if ( top_made == open && made_bpp == screen.canvas_bpp ) {
        return;                                     /* drawn already */
    }
    top_made = open;
    made_bpp = screen.canvas_bpp;
    cv_begin( &top, top_pix, SCR_W, TOP_H, C_BAR );
    for ( index = 0; index < MENUS; index++ ) {
        title_from[index] = title_to[index] = 0;
        if ( !menu_count( index ) ) {
            continue;
        }
        label_plain( menus[index].title, text, sizeof( text ) );
        wide = ui_width( MENU_SIZE, text ) + 16;
        if ( index == open ) {
            cv_fill( &top, xpos, 1, xpos + wide, TOP_H, C_FACE );
        }
        menu_text( &top, xpos + 8, 12, index == open ? C_ARROW : C_LABEL, menus[index].title );
        title_from[index] = xpos;
        title_to[index] = xpos + wide;
        xpos += wide;
    }
}

/* The menu that is down, drawn into its canvas with line "pick" marked,
   and put under its name. */
static void drop_paint( int open, int pick )
{
    char text[40];
    u32 rgb;
    int index, wide, high = 2, most = 0, keys = 0, len;

    for ( index = 0; index < mlines; index++ ) {
        mline_y[index] = high;
        if ( mline[index]->label == NULL ) {
            high += RULE_H;
            continue;
        }
        high += ITEM_H;
        label_plain( mline[index]->label, text, sizeof( text ) );
        len = ui_width( MENU_SIZE, text );
        if ( len > most ) {
            most = len;
        }
        len = ui_width( MENU_SIZE, mline[index]->keys );
        if ( len > keys ) {
            keys = len;
        }
    }
    high += 2;
    wide = 12 + most + 24 + keys + 12;
    if ( wide > DROP_W ) {
        wide = DROP_W;
    }
    if ( high > DROP_H ) {
        high = DROP_H;
    }
    cv_begin( &drop, drop_pix, wide, high, C_FACE );
    cv_fill( &drop, 0, 0, wide, 1, C_EDGE );
    cv_fill( &drop, 0, high - 1, wide, high, C_EDGE );
    cv_fill( &drop, 0, 0, 1, high, C_EDGE );
    cv_fill( &drop, wide - 1, 0, wide, high, C_EDGE );
    for ( index = 0; index < mlines && mline_y[index] + ITEM_H <= high; index++ ) {
        if ( mline[index]->label == NULL ) {
            cv_fill( &drop, 1, mline_y[index] + RULE_H / 2, wide - 1, mline_y[index] + RULE_H / 2 + 1, C_TROUGH );
            continue;
        }
        rgb = C_ARROW;
        if ( index == pick ) {
            cv_fill( &drop, 2, mline_y[index], wide - 2, mline_y[index] + ITEM_H, C_BUTTON );
            rgb = C_LABEL;
        }
        menu_text( &drop, 12, mline_y[index] + 12, rgb, mline[index]->label );
        ui_text( &drop, wide - 12 - ui_width( MENU_SIZE, mline[index]->keys ), mline_y[index] + 12, MENU_SIZE, rgb,
                 mline[index]->keys );
    }
    drop_x = title_from[open];
    if ( drop_x + wide > SCR_W ) {
        drop_x = SCR_W - wide;
    }
    drop_on = 1;
}

/* the whole screen */
static void gfx_show( const char *status, const char *message )
{
    top_paint( -1 );
    bar_paint( status, message );
    rows_show( 0, SCR_H );
}

/* ------------------------------------------------------------------ */
/* the picture                                                         */
/* ------------------------------------------------------------------ */

static void limits( void )
{
    int most_x, most_y;

    /* a page wider than its part of the screen has a scroll bar under
       it - but not a page fitted to the screen, which rounding may
       have left a pixel over */
    area_h = !fit && view.width > AREA_W ? PAGE_H - SB : PAGE_H;
    most_x = view.width - AREA_W;
    most_y = view.height - area_h;
    if ( most_x <= 0 ) {
        view_x = most_x / 2;                        /* narrower than the screen: in the middle */
    } else {
        view_x = view_x < 0 ? 0 : (view_x > most_x ? most_x : view_x);
    }
    if ( most_y <= 0 ) {
        view_y = most_y / 2;
    } else {
        view_y = view_y < 0 ? 0 : (view_y > most_y ? most_y : view_y);
    }
}

/* is the part of the page on the screen in the canvas? */
static int canvas_covers( void )
{
    int x0 = view_x < 0 ? 0 : view_x, y0 = view_y < 0 ? 0 : view_y;
    int x1 = view_x + AREA_W > view.width ? view.width : view_x + AREA_W;
    int y1 = view_y + area_h > view.height ? view.height : view_y + area_h;

    return canvas.pix != NULL && canvas_page == page_no && canvas_zoom == zoom && canvas_turn == turn &&
           x0 >= canvas.org_x && y0 >= canvas.org_y && x1 <= canvas.org_x + canvas.width &&
           y1 <= canvas.org_y + canvas.height;
}

static int gfx_hit( int xpos, int ypos )
{
    if ( ypos < TOP_H ) {
        return HIT_TITLE;
    }
    if ( ypos >= BAR_Y ) {
        return HIT_BAR;
    }
    ypos -= TOP_H;
    if ( xpos >= AREA_W ) {
        return ypos < area_h ? HIT_V + bar_part( ypos, area_h, SB, v_box, v_len ) : HIT_NONE;
    }
    if ( ypos >= area_h ) {
        return HIT_H + bar_part( xpos, AREA_W, SB, h_box, h_len );
    }
    return HIT_PAGE;
}

/* the button went down there: the key that is, or 0 with something
   taken hold of */
static int gfx_press( int xpos, int ypos )
{
    static const int v_keys[5] = { KEY_UP, EV_UP, 0, EV_DOWN, KEY_DOWN };
    static const int h_keys[5] = { KEY_LEFT, EV_LEFT, 0, EV_RIGHT, KEY_RIGHT };
    int hit = gfx_hit( xpos, ypos ), index, key = 0;

    drag = hold_key = 0;
    if ( hit == HIT_PAGE ) {
        drag = DRAG_PAGE;
        drag_x = xpos;
        drag_y = ypos;
        drag_view_x = view_x;
        drag_view_y = view_y;
    } else if ( hit == HIT_TITLE ) {
        index = title_at( xpos );
        return index < 0 ? 0 : EV_MENU + index;
    } else if ( hit == HIT_BAR ) {
        if ( bar_whole ) {
            return EV_VIEW;                         /* something was said there: the buttons come back */
        }
        for ( index = 0; index < ZONES; index++ ) {
            if ( xpos >= zones[index].x0 && xpos < zones[index].x1 ) {
                return zones[index].key;
            }
        }
    } else if ( hit & HIT_V ) {
        if ( (hit & 7) == PART_BOX ) {
            drag = DRAG_V;
            drag_y = ypos - SB - v_box;             /* how far down the box the pointer has it */
            drag_at = ypos;
        } else {
            key = v_keys[hit & 7];
        }
    } else if ( hit & HIT_H ) {
        if ( (hit & 7) == PART_BOX ) {
            drag = DRAG_H;
            drag_x = xpos - SB - h_box;
            drag_at = xpos;
        } else {
            key = h_keys[hit & 7];
        }
    }
    if ( key ) {
        hold_begin( key, hit );
    }
    return key;
}

/* what the mouse has done, as a key: 0 for nothing that changes the
   screen */
static int gfx_mouse( void )
{
    int key, had, was_x = view_x, was_y = view_y;

    if ( press_seen ) {
        press_seen = 0;
        key = gfx_press( press_x, press_y );
        if ( key ) {
            return key;
        }
    }
    if ( !ms_down ) {
        had = drag;
        drag = hold_key = 0;
        /* let go: what the canvas had not got is drawn now */
        return had && !canvas_covers() ? EV_VIEW : 0;
    }
    switch ( drag ) {
    case DRAG_PAGE:
        view_x = drag_view_x - (ms_x - drag_x);
        view_y = drag_view_y - (ms_y - drag_y);
        break;
    case DRAG_V:
        /* a box only held is not a box moved: where the box is says
           less exactly where the page is than the page does */
        if ( ms_y == drag_at ) {
            return 0;
        }
        drag_at = -1;
        view_y = (int)box_to( area_h - 2 * SB, view.height, area_h, v_len, ms_y - SB - drag_y );
        break;
    case DRAG_H:
        if ( ms_x == drag_at ) {
            return 0;
        }
        drag_at = -1;
        view_x = (int)box_to( AREA_W - 2 * SB, view.width, AREA_W, h_len, ms_x - SB - drag_x );
        break;
    default:
        return hold_due( gfx_hit( ms_x, ms_y ) );
    }
    limits();
    return view_x != was_x || view_y != was_y ? EV_VIEW : 0;
}

/* called while a page is being drawn: 1 to give it up */
static int drawing_poll( void )
{
    u32 now;
    int got;

    if ( test_keys ) {
        return 0;
    }
    got = sys_event( EVF_NOWAIT | (mouse_on ? EVF_MOUSE : 0) );
    if ( got == 1 ) {
        return 1;                                   /* whatever it is, it is what matters now */
    }
    if ( got == 2 ) {
        mouse_read();
        pointer_to( ms_x, ms_y );
        /* a button or a scroll bar is as good as a key; a press on the
           page waits for there to be a page */
        if ( press_seen && gfx_hit( press_x, press_y ) != HIT_PAGE ) {
            return 1;
        }
    }
    /* DOS's clock is the one for this: sys_clock_ms counts only while
       it is asked often, and a page being drawn does not ask often */
    now = sys_hundredths();
    if ( now - last_shown > 150 || now < last_shown ) {
        gfx_show( "Drawing the page...", NULL );
        last_shown = now;
    }
    return 0;
}

/* The page, measured and - if the canvas has not got what the screen
   is to show - drawn.  "where" is -1 for the foot of a page that has
   just been arrived at backwards, 1 for its head, 0 to stay put. */
static void gfx_page( int where )
{
    static JMPBUF trap;
    fx wide, high, fit_w, fit_h;
    u32 need, most, room;
    int why, cw, ch, bpp = screen.canvas_bpp;

    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        if ( !pdf_page( page_no, &page ) ) {
            pdf_fail( PE_DAMAGED );
        }
        if ( fit ) {
            wide = real_to_fx( page.box[2] ) - real_to_fx( page.box[0] );
            high = real_to_fx( page.box[3] ) - real_to_fx( page.box[1] );
            if ( (page.rotate + turn) % 180 != 0 ) {
                fit_w = wide;
                wide = high;
                high = fit_w;
            }
            fit_w = fx_div( I2FX( AREA_W ), wide );
            fit_h = fx_div( I2FX( PAGE_H ), high );
            zoom = fit == 2 && fit_h < fit_w ? fit_h : fit_w;
        }
        view_setup( &view, &page, zoom, turn );
        if ( where > 0 ) {
            view_y = 0;
        } else if ( where < 0 ) {
            view_y = view.height;
        }
        limits();
        if ( !canvas_covers() ) {
            /* all of the page if there is the memory, or a piece as
               wide as the screen and as tall as can be had */
            cw = view.width;
            ch = view.height;
            room = sys_mem_largest() + canvas_cap;
            if ( bpp == 3 && room < (u32)AREA_W * PAGE_H * 3 + WORK_BYTES ) {
                bpp = 1;
                if ( !grey_told ) {
                    str_cpy( notice, "Not enough memory for color: pages are drawn in gray" );
                    grey_told = 1;
                }
            }
            most = room > WORK_BYTES ? room - WORK_BYTES : 0;
            if ( most < (u32)AREA_W * PAGE_H * (u32)bpp ) {
                most = (u32)AREA_W * PAGE_H * (u32)bpp;
            }
            if ( (u32)cw * (u32)ch * (u32)bpp > most ) {
                if ( cw > AREA_W ) {
                    cw = AREA_W;
                }
                ch = (int)(most / ((u32)cw * (u32)bpp));
                if ( ch > view.height ) {
                    ch = view.height;
                }
                if ( ch < PAGE_H && ch < view.height ) {
                    ch = view.height < PAGE_H ? view.height : PAGE_H;
                }
            }
            need = (u32)cw * (u32)ch * (u32)bpp;
            if ( need > canvas_cap ) {
                if ( canvas.pix ) {
                    xfree( canvas.pix );
                }
                canvas.pix = NULL;
                canvas_cap = 0;
                canvas.pix = (u8 *)xalloc( need );
                canvas_cap = need;
            }
            canvas.width = cw;
            canvas.height = ch;
            canvas.bpp = bpp;
            canvas.stride = (u32)cw * (u32)bpp;
            /* round where the screen is, as far as the page allows */
            canvas.org_x = view_x - (cw - AREA_W) / 2;
            canvas.org_y = view_y - (ch - area_h) / 2;
            if ( canvas.org_x > view.width - cw ) {
                canvas.org_x = view.width - cw;
            }
            if ( canvas.org_y > view.height - ch ) {
                canvas.org_y = view.height - ch;
            }
            if ( canvas.org_x < 0 ) {
                canvas.org_x = 0;
            }
            if ( canvas.org_y < 0 ) {
                canvas.org_y = 0;
            }
            canvas_page = -1;
            canvas_clear( &canvas, 0xFFFFFFUL );
            gfx_show( "Drawing the page...", NULL );
            last_shown = sys_hundredths();
            render_poll = drawing_poll;
            page_render( &page, &view, &canvas );
            canvas_page = page_no;
            canvas_zoom = zoom;
            canvas_turn = turn;
        }
    } else if ( why != PE_ABORT ) {
        sfmt( notice, sizeof( notice ), "Page %d: %s", page_no + 1, why_text( why ) );
        if ( why == PE_NOMEM ) {
            font_reclaim( 1 );
        }
        canvas_page = page_no;                      /* what there is of it is all there will be */
        canvas_zoom = zoom;
        canvas_turn = turn;
    }
    render_poll = NULL;
    pdf_page_end();
    pdf_trap = NULL;
}

static void gfx_go( int to, int where )
{
    if ( to < 0 ) {
        to = 0;
    }
    if ( to >= page_total ) {
        to = page_total - 1;
    }
    if ( to != page_no ) {
        page_no = to;
        gfx_page( where ? where : 1 );
    }
}

static void zoom_to( fx to )
{
    int mid_x = view_x + AREA_W / 2, mid_y = view_y + area_h / 2;

    if ( to < 0x2000L ) {
        to = 0x2000L;                               /* an eighth */
    }
    if ( to > 8 * FX_ONE ) {
        to = 8 * FX_ONE;
    }
    /* the middle of the screen stays the middle */
    view_x = (int)mul_div( mid_x, to, zoom ) - AREA_W / 2;
    view_y = (int)mul_div( mid_y, to, zoom ) - area_h / 2;
    zoom = to;
    fit = 0;
}

/* ------------------------------------------------------------------ */
/* the text                                                            */
/* ------------------------------------------------------------------ */

/* the words on the bottom line, each of them the key it names */
#define HINTS       9
static const struct {
    const char *label;
    int key;
} hints[HINTS] = {
    { "PgUp", KEY_PGUP }, { "PgDn", KEY_PGDN }, { "P Prev", 'p' }, { "N Next", 'n' }, { "G Go to", 'g' },
    { "F Find", 'f' }, { "Tab Picture", KEY_TAB }, { "F1 Help", KEY_F1 }, { "Esc Exit", KEY_ESC } };
static int hint_col[HINTS];
static int hints_on;                /* the words are there, and not something said */

static void text_free( void )
{
    if ( text_block ) {
        xfree( text_block );
        text_block = NULL;
    }
    if ( trows ) {
        xfree( trows );
        trows = NULL;
    }
    text_count = trow_count = 0;
    text_page = -1;
}

/* the lines as rows of the screen: each whole, or folded at a space */
static void text_rows( void )
{
    const char *line;
    u32 len, at, take, back;
    int index, count = 0, pass;

    if ( trows ) {
        xfree( trows );
        trows = NULL;
    }
    text_widest = 0;
    for ( pass = 0; pass < 2; pass++ ) {
        count = 0;
        for ( index = 0; index < text_count; index++ ) {
            line = text_lines[index];
            len = str_len( line );
            at = 0;
            do {
                take = len - at;
                if ( wrap && take > (u32)text_cols ) {
                    take = (u32)text_cols;
                    for ( back = take; back > (u32)text_cols / 2; back-- ) {
                        if ( line[at + back] == ' ' ) {
                            take = back;
                            break;
                        }
                    }
                }
                if ( pass ) {
                    trows[count].line = (u16)index;
                    trows[count].start = (u16)at;
                    trows[count].len = (u16)take;
                }
                if ( (int)take > text_widest ) {
                    text_widest = (int)take;
                }
                count++;
                at += take;
                while ( wrap && at < len && line[at] == ' ' ) {
                    at++;
                }
            } while ( at < len );
        }
        if ( pass == 0 ) {
            trows = (TROW *)xalloc( ((u32)count + 1) * sizeof( TROW ) );
        }
    }
    trow_count = count;
}

/* how the screen is shared out, and the view kept inside the text */
static void text_measure( void )
{
    int most;

    thbar = !wrap && text_widest > text_cols;
    tbody = rows - 2 - thbar;
    most = trow_count > tbody ? trow_count - tbody : 0;
    text_top = text_top < 0 ? 0 : (text_top > most ? most : text_top);
    most = thbar ? text_widest - text_cols : 0;
    text_left = text_left < 0 ? 0 : (text_left > most ? most : text_left);
}

/* the text of page "want", kept: 0 and the reason in notice if not */
static int text_load( int want )
{
    static JMPBUF trap;
    TEXTPAGE *tp;
    u32 total = 0;
    char *cur;
    int why, index;

    if ( want == text_page ) {
        return 1;
    }
    text_free();
    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        tp = text_extract( want );
        if ( tp ) {
            for ( index = 0; index < tp->count; index++ ) {
                total += str_len( tp->lines[index] ) + 1;
            }
            text_block = (char *)xalloc( total + ((u32)tp->count + 1) * sizeof( char * ) + 4 );
            text_lines = (char **)text_block;
            cur = text_block + ((u32)tp->count + 1) * sizeof( char * );
            for ( index = 0; index < tp->count; index++ ) {
                text_lines[index] = cur;
                str_cpy( cur, tp->lines[index] );
                cur += str_len( cur ) + 1;
            }
            text_count = tp->count;
        }
    } else {
        sfmt( notice, sizeof( notice ), "Page %d: %s", want + 1, why_text( why ) );
        text_free();
    }
    pdf_page_end();
    pdf_trap = NULL;
    text_page = want;
    text_rows();
    return why == 0;
}

static void text_show( void )
{
    static char cells[164];
    static u16 under[160];
    char text[100];
    const TROW *trow;
    const char *line;
    u16 cell;
    int row, len, from, index;

    text_title( -1 );
    box_of( tbody - 2, trow_count, tbody, text_top, 1, &tv_box, &tv_len );
    for ( row = 0; row < tbody; row++ ) {
        cells[0] = 0;
        if ( text_count == 0 && row == 1 ) {
            str_cpy( cells, "  This page has no text.  Tab shows the page as a picture." );
        } else if ( text_top + row < trow_count ) {
            trow = &trows[text_top + row];
            line = text_lines[trow->line] + trow->start;
            len = trow->len;
            from = wrap ? 0 : text_left;
            if ( from < len ) {
                len -= from;
                if ( len > text_cols ) {
                    len = text_cols;
                }
                mem_cpy( cells, line + from, (u32)len );
                cells[len] = 0;
            }
        }
        put_text( row + 1, 0, text_top + row == found_row ? ATTR_FOUND : ATTR_TEXT, cells, text_cols );
        cell = bar_cell( bar_part( row, tbody, 1, tv_box, tv_len ), 0x18, 0x19 );
        sys_put_cells( (u32)((row + 1) * cols + cols - 1), &cell, 1 );
    }
    if ( thbar ) {
        box_of( text_cols - 2, text_widest, text_cols, text_left, 1, &th_box, &th_len );
        for ( index = 0; index < text_cols; index++ ) {
            under[index] = bar_cell( bar_part( index, text_cols, 1, th_box, th_len ), 0x1B, 0x1A );
        }
        under[text_cols] = (u16)((ATTR_BAR << 8) | ' ');
        sys_put_cells( (u32)((rows - 2) * cols), under, (u32)cols );
    }
    hints_on = notice[0] == 0;
    if ( !hints_on ) {
        sfmt( text, sizeof( text ), " %s", notice );
        notice[0] = 0;
    } else {
        text[0] = 0;
        for ( index = 0; index < HINTS; index++ ) {
            str_catn( text, index ? "  " : " ", sizeof( text ) );
            hint_col[index] = (int)str_len( text );
            str_catn( text, hints[index].label, sizeof( text ) );
        }
    }
    put_text( rows - 1, 0, ATTR_BAR, text, cols );
}

static void text_go( int to, int at_end )
{
    if ( to < 0 ) {
        to = 0;
    }
    if ( to >= page_total ) {
        to = page_total - 1;
    }
    page_no = to;
    text_load( page_no );
    found_row = -1;
    text_left = 0;
    text_top = at_end ? trow_count : 0;
    text_measure();
}

/* from the row after "after" on this page, then page by page */
static void find_next( int after )
{
    char text[60];
    int start = page_no, row, at;

    if ( find_text[0] == 0 ) {
        return;
    }
    for ( at = start; at < page_total; at++ ) {
        if ( at != start ) {
            if ( key_waiting() ) {
                key_get();
                break;
            }
            sfmt( text, sizeof( text ), " Looking on page %d...", at + 1 );
            put_text( rows - 1, 0, ATTR_BAR, text, cols );
            text_load( at );
            after = -1;
        }
        for ( row = after + 1; row < trow_count; row++ ) {
            if ( mem_ifind( (const u8 *)text_lines[trows[row].line] + trows[row].start, trows[row].len,
                            find_text ) >= 0 ) {
                page_no = at;
                found_row = row;
                text_top = row > 2 ? row - 2 : 0;
                return;
            }
        }
    }
    /* not there: back where we were */
    text_load( start );
    page_no = start;
    sfmt( notice, sizeof( notice ), "\"%s\" was not found", find_text );
}

static int text_hit( int row, int col )
{
    if ( row == 0 ) {
        return HIT_TITLE;
    }
    if ( row == rows - 1 ) {
        return HIT_BAR;
    }
    if ( row <= tbody ) {
        return col == cols - 1 ? HIT_V + bar_part( row - 1, tbody, 1, tv_box, tv_len ) : HIT_PAGE;
    }
    if ( thbar && col < text_cols ) {
        return HIT_H + bar_part( col, text_cols, 1, th_box, th_len );
    }
    return HIT_NONE;
}

static int text_press( int row, int col )
{
    static const int v_keys[5] = { KEY_UP, EV_UP, 0, EV_DOWN, KEY_DOWN };
    static const int h_keys[5] = { KEY_LEFT, EV_LEFT, 0, EV_RIGHT, KEY_RIGHT };
    int hit = text_hit( row, col ), index, key = 0;

    drag = hold_key = 0;
    if ( hit == HIT_TITLE ) {
        index = title_at( col );
        return col >= title_page ? 'g' : (index < 0 ? 0 : EV_MENU + index);
    }
    if ( hit == HIT_BAR ) {
        if ( !hints_on ) {
            return EV_VIEW;                         /* something was said there: the words come back */
        }
        for ( index = 0; index < HINTS; index++ ) {
            if ( col >= hint_col[index] && col < hint_col[index] + (int)str_len( hints[index].label ) ) {
                return hints[index].key;
            }
        }
    } else if ( hit & HIT_V ) {
        if ( (hit & 7) == PART_BOX ) {
            drag = DRAG_V;
            drag_y = row - 2 - tv_box;
            drag_at = row;
        } else {
            key = v_keys[hit & 7];
        }
    } else if ( hit & HIT_H ) {
        if ( (hit & 7) == PART_BOX ) {
            drag = DRAG_H;
            drag_x = col - 1 - th_box;
            drag_at = col;
        } else {
            key = h_keys[hit & 7];
        }
    }
    if ( key ) {
        hold_begin( key, hit );
    }
    return key;
}

static int text_mouse( void )
{
    int row, col, key, was_top = text_top, was_left = text_left;

    if ( press_seen ) {
        press_seen = 0;
        mouse_cell( press_x, press_y, &row, &col );
        key = text_press( row, col );
        if ( key ) {
            return key;
        }
    }
    if ( !ms_down ) {
        drag = hold_key = 0;
        return 0;
    }
    mouse_cell( ms_x, ms_y, &row, &col );
    switch ( drag ) {
    case DRAG_V:
        if ( row == drag_at ) {
            return 0;
        }
        drag_at = -1;
        text_top = (int)box_to( tbody - 2, trow_count, tbody, tv_len, row - 2 - drag_y );
        break;
    case DRAG_H:
        if ( col == drag_at ) {
            return 0;
        }
        drag_at = -1;
        text_left = (int)box_to( text_cols - 2, text_widest, text_cols, th_len, col - 1 - drag_x );
        break;
    default:
        return hold_due( text_hit( row, col ) );
    }
    return text_top != was_top || text_left != was_left ? EV_VIEW : 0;
}

/* ------------------------------------------------------------------ */
/* the menus: one let down, and a line of it chosen                    */
/* ------------------------------------------------------------------ */

/* where the mouse is, of the menus: 0 and up for a menu's name, or */
#define MH_OUT      -1              /* off them altogether, */
#define MH_IN       -2              /* on the menu that is down but on no line of it, */
#define MH_LINE     -3              /* or on a line of it, and "line" says which */

static int menu_hit( int xpos, int ypos, int *line )
{
    int row, col, index;

    if ( in_text ) {
        mouse_cell( xpos, ypos, &row, &col );
        if ( row == 0 ) {
            index = title_at( col );
            return index < 0 ? MH_OUT : index;
        }
        if ( row > mlines + 2 || col < drop_col || col >= drop_col + drop_wide ) {
            return MH_OUT;
        }
        index = row - 2;
        if ( index < 0 || index >= mlines || mline[index]->label == NULL || col == drop_col ||
             col == drop_col + drop_wide - 1 ) {
            return MH_IN;
        }
        *line = index;
        return MH_LINE;
    }
    if ( ypos < TOP_H ) {
        index = title_at( xpos );
        return index < 0 ? MH_OUT : index;
    }
    ypos -= TOP_H;
    if ( ypos >= drop.height || xpos < drop_x || xpos >= drop_x + drop.width ) {
        return MH_OUT;
    }
    for ( index = 0; index < mlines; index++ ) {
        if ( mline[index]->label && ypos >= mline_y[index] && ypos < mline_y[index] + ITEM_H ) {
            *line = index;
            return MH_LINE;
        }
    }
    return MH_IN;
}

/* while a menu is down: a key, or EV_MOUSE when the mouse has moved or
   its button has */
static int menu_event( void )
{
    int key;

    if ( test_keys || !mouse_on ) {
        key = key_get();
        if ( key != EV_MOUSE || !mouse_on ) {
            return key;
        }
    } else if ( sys_event( EVF_MOUSE ) == 1 ) {
        return key_get();
    }
    mouse_read();
    pointer_to( ms_x, ms_y );
    return EV_MOUSE;
}

/* The menus on the screen: menu "open" down, with its line "pick"
   marked.  "fresh" when it is another menu than was down before, and
   what that one lay over has to come back. */
static void menu_show( int open, int pick, int fresh )
{
    int was = drop_on ? drop.height : 0;

    if ( in_text ) {
        if ( fresh ) {
            text_show();
            text_title( open );
        }
        text_drop( open, pick );
        return;
    }
    top_paint( open );
    drop_paint( open, pick );
    rows_show( 0, TOP_H + (was > drop.height ? was : drop.height) );
}

/* and off it: the screen as it was before a menu came down */
static void menu_hide( void )
{
    int was = drop_on ? drop.height : 0;

    drop_on = 0;
    if ( in_text ) {
        text_show();
        return;
    }
    top_paint( -1 );
    rows_show( 0, TOP_H + was );
}

/* Menu "open" let down, and the others to be had from it, until a line
   is chosen or the menus are left: the line's key, or 0.  "keyed" when
   a key let it down, which starts with its first line marked; let down
   by the mouse nothing is marked until the pointer is on a line.

   WITH THE MOUSE a line is chosen by letting the button go on it, so
   that a press on a name, a pull down the menu and a letting go is one
   movement - and a click on the name and a click on the line is the
   same thing done in two.  A press on another name lets that menu
   down, a press on the name of the one that is down puts it away, and
   so does a press anywhere else, which is then all that press does. */
static int menu_run( int open, int keyed )
{
    int pick, chosen = 0, done = 0, key, index, hit, line = 0, pressed, was_down, shown_open, shown_pick;

    drag = hold_key = 0;
    press_seen = 0;
    if ( !menu_count( open ) ) {
        open = menu_next( open, 1 );
    }
    menu_fill( open );
    pick = keyed ? menu_step( -1, 1 ) : -1;
    was_down = !keyed && ms_down;
    menu_show( open, pick, 1 );
    while ( !done ) {
        shown_open = open;
        shown_pick = pick;
        key = menu_event();
        if ( key != EV_MOUSE ) {
            switch ( key ) {
            case KEY_ESC: case KEY_F10:
                done = 1;
                break;
            case KEY_LEFT:
                open = menu_next( open, -1 );
                break;
            case KEY_RIGHT:
                open = menu_next( open, 1 );
                break;
            case KEY_UP:
                pick = menu_step( pick < 0 ? 0 : pick, -1 );
                break;
            case KEY_DOWN:
                pick = menu_step( pick, 1 );
                break;
            case KEY_ENTER:
                if ( pick >= 0 ) {
                    chosen = mline[pick]->key;
                    done = 1;
                }
                break;
            default:
                /* Alt and another menu's letter, or the letter of a line */
                for ( index = 0; index < MENUS; index++ ) {
                    if ( key == menus[index].alt && menu_count( index ) ) {
                        open = index;
                    }
                }
                for ( index = 0; index < mlines && !done && open == shown_open; index++ ) {
                    char label[40];
                    int hot;

                    if ( mline[index]->label == NULL ) {
                        continue;
                    }
                    hot = label_plain( mline[index]->label, label, sizeof( label ) );
                    if ( hot >= 0 && key < 128 && letter_of( key ) == letter_of( label[hot] ) ) {
                        chosen = mline[index]->key;
                        done = 1;
                    }
                }
                break;
            }
            if ( open != shown_open ) {
                menu_fill( open );
                pick = menu_step( -1, 1 );
            }
        } else if ( mouse_on ) {
            pressed = press_seen;
            press_seen = 0;
            if ( pressed ) {
                hit = menu_hit( press_x, press_y, &line );
                if ( hit == MH_OUT || hit == open ) {
                    done = 1;                       /* off the menus, or on the name of the one that is down */
                } else if ( hit >= 0 ) {
                    open = hit;
                    menu_fill( open );
                    pick = -1;
                }
            }
            if ( !done ) {
                /* a click over before it was looked at was let go where it was made */
                if ( pressed && !ms_down ) {
                    hit = menu_hit( press_x, press_y, &line );
                } else {
                    hit = menu_hit( ms_x, ms_y, &line );
                }
                if ( hit == MH_LINE ) {
                    pick = line;
                }
                if ( ms_down ) {
                    /* held, and pulled along the names: each comes down in turn */
                    if ( hit >= 0 && hit != open ) {
                        open = hit;
                        menu_fill( open );
                        pick = -1;
                    }
                } else if ( was_down || pressed ) {
                    if ( hit == MH_LINE ) {
                        chosen = mline[line]->key;
                        done = 1;
                    } else if ( hit == MH_OUT ) {
                        done = 1;
                    }
                }
            }
            was_down = ms_down;
        }
        if ( !done && (open != shown_open || pick != shown_pick) ) {
            menu_show( open, pick, open != shown_open );
        }
    }
    menu_hide();
    return chosen;
}

/* A key on its way to a loop.  If it is the menus' - F10, Alt and a
   menu's letter, a press on a name - the menus have it, and the loop
   gets the key of the line chosen there, or 0. */
static int menu_key( int key )
{
    int index;

    if ( key == KEY_F10 ) {
        return menu_run( 0, 1 );
    }
    for ( index = 0; index < MENUS; index++ ) {
        if ( key == EV_MENU + index ) {
            return menu_run( index, 0 );
        }
        if ( key == menus[index].alt && menu_count( index ) ) {
            return menu_run( index, 1 );
        }
    }
    return key;
}

/* ------------------------------------------------------------------ */
/* the two loops                                                       */
/* ------------------------------------------------------------------ */

static void go_to_page( void )
{
    char digits[8];
    int ok, want;

    if ( ask( in_text ? " Go to page: " : "Go to page: ", digits, sizeof( digits ), 1 ) ) {
        want = (int)dec_parse( digits, &ok );
        if ( ok && want >= 1 && want <= page_total ) {
            if ( in_text ) {
                text_go( want - 1, 0 );
            } else {
                gfx_go( want - 1, 1 );
            }
        } else {
            sfmt( notice, sizeof( notice ), page_total == 1 ? "There is %d page" : "There are %d pages", page_total );
        }
    }
}

static void text_loop( void )
{
    int key, most;

    sys_screen_size( &rows, &cols );
    if ( cols > 160 ) {
        cols = 160;
    }
    text_cols = cols - 1;
    sys_cursor_shape( CUR_HIDE );
    mouse_setup();
    text_page = -1;
    text_go( page_no, 0 );
    while ( !quit && in_text ) {
        text_measure();
        most = trow_count > tbody ? trow_count - tbody : 0;
        text_show();
        key = menu_key( event_get( WAIT_ALL ) );
        switch ( key ) {
        case KEY_ESC: case 'q': case 'Q': case KEY_ALTX:
            quit = 1;
            break;
        case KEY_TAB:
            mouse_rest();
            if ( gfx_kind && video_set( gfx_kind ) ) {
                in_text = 0;
            } else {
                mouse_setup();
                str_cpy( notice, "This machine has no graphics screen ACROREAD can use" );
            }
            break;
        case KEY_DOWN:
            text_top++;
            break;
        case KEY_UP:
            text_top--;
            break;
        case KEY_RIGHT:
            text_left += 8;
            break;
        case KEY_LEFT:
            text_left -= 8;
            break;
        case KEY_PGDN: case ' ': case KEY_ENTER:
            if ( text_top >= most ) {
                if ( page_no + 1 < page_total ) {
                    text_go( page_no + 1, 0 );
                }
            } else {
                text_top += tbody - 1;
            }
            break;
        case KEY_PGUP: case KEY_BKSP:
            if ( text_top == 0 ) {
                if ( page_no > 0 ) {
                    text_go( page_no - 1, 1 );
                }
            } else {
                text_top -= tbody - 1;
            }
            break;
        case EV_DOWN:
            text_top += tbody - 1;
            break;
        case EV_UP:
            text_top -= tbody - 1;
            break;
        case EV_RIGHT:
            text_left += text_cols - 8;
            break;
        case EV_LEFT:
            text_left -= text_cols - 8;
            break;
        case KEY_HOME:
            text_top = 0;
            text_left = 0;
            break;
        case KEY_END:
            text_top = most;
            break;
        case 'n': case 'N': case KEY_CPGDN:
            if ( page_no + 1 < page_total ) {
                text_go( page_no + 1, 0 );
            }
            break;
        case 'p': case 'P': case KEY_CPGUP:
            if ( page_no > 0 ) {
                text_go( page_no - 1, 0 );
            }
            break;
        case KEY_CHOME:
            text_go( 0, 0 );
            break;
        case KEY_CEND:
            text_go( page_total - 1, 0 );
            break;
        case 'g': case 'G':
            go_to_page();
            break;
        case 'w': case 'W':
            wrap = !wrap;
            text_left = 0;
            found_row = -1;
            text_rows();
            break;
        case 'f': case 'F':
            if ( ask( " Find: ", find_text, sizeof( find_text ), 0 ) ) {
                find_next( text_top - 1 );
            }
            break;
        case KEY_F3:
            find_next( found_row >= 0 ? found_row : text_top - 1 );
            break;
        case KEY_F1: case '?':
            help();
            break;
        }
    }
}

static void gfx_loop( void )
{
    int key;

    mouse_setup();
    canvas_page = -1;
    gfx_page( 0 );
    while ( !quit && !in_text ) {
        /* a key already typed is seen to first: the drawing it broke
           off would only be broken off again.  And nothing is drawn
           under a page the mouse has hold of. */
        if ( !canvas_covers() && !input_waiting() && !drag ) {
            gfx_page( 0 );
        }
        gfx_show( NULL, notice );
        notice[0] = 0;
        key = menu_key( event_get( WAIT_ALL ) );
        switch ( key ) {
        case KEY_ESC: case 'q': case 'Q': case KEY_ALTX:
            quit = 1;
            break;
        case KEY_TAB: case 't': case 'T':
            mouse_rest();
            video_text();
            in_text = 1;
            break;
        case KEY_DOWN:
            view_y += 48;
            break;
        case KEY_UP:
            view_y -= 48;
            break;
        case KEY_RIGHT:
            view_x += 64;
            break;
        case KEY_LEFT:
            view_x -= 64;
            break;
        case KEY_PGDN: case ' ': case KEY_ENTER:
            if ( view_y + area_h >= view.height ) {
                if ( page_no + 1 < page_total ) {
                    gfx_go( page_no + 1, 1 );
                }
            } else {
                view_y += area_h - 32;
            }
            break;
        case KEY_PGUP: case KEY_BKSP:
            if ( view_y <= 0 ) {
                if ( page_no > 0 ) {
                    gfx_go( page_no - 1, -1 );
                }
            } else {
                view_y -= area_h - 32;
            }
            break;
        case EV_DOWN:
            view_y += area_h - 32;
            break;
        case EV_UP:
            view_y -= area_h - 32;
            break;
        case EV_RIGHT:
            view_x += AREA_W - 64;
            break;
        case EV_LEFT:
            view_x -= AREA_W - 64;
            break;
        case KEY_HOME:
            view_y = 0;
            view_x = 0;
            break;
        case KEY_END:
            view_y = view.height;
            break;
        case 'n': case 'N': case KEY_CPGDN:
            gfx_go( page_no + 1, 1 );
            break;
        case 'p': case 'P': case KEY_CPGUP:
            gfx_go( page_no - 1, 1 );
            break;
        case KEY_CHOME:
            gfx_go( 0, 1 );
            break;
        case KEY_CEND:
            gfx_go( page_total - 1, 1 );
            break;
        case 'g': case 'G':
            go_to_page();
            break;
        case '+': case '=':
            zoom_to( zoom + zoom / 4 );
            break;
        case '-': case '_':
            zoom_to( zoom - zoom / 5 );
            break;
        case '1':
            zoom_to( FX_ONE );
            break;
        case 'w': case 'W':
            fit = 1;
            canvas_page = -1;
            break;
        case 'f': case 'F': case 'z': case 'Z':
            fit = 2;
            canvas_page = -1;
            break;
        case EV_FIT:
            fit = fit == 1 ? 2 : 1;
            canvas_page = -1;
            break;
        case 'r': case 'R':
            turn = (turn + 90) % 360;
            view_x = view_y = 0;
            break;
        case 'l': case 'L':
            turn = (turn + 270) % 360;
            view_x = view_y = 0;
            break;
        case KEY_F1: case '?':
            help();
            break;
        }
        if ( !in_text && !quit ) {
            /* the page's size may have changed under the screen */
            if ( (canvas_zoom != zoom || canvas_turn != turn || canvas_page != page_no) && !input_waiting() &&
                 !drag ) {
                gfx_page( 0 );
            }
            limits();
        }
    }
}

void view_run( const char *name, int first_page, int want )
{
    int index;

    file_name = name;
    page_total = doc.page_count;
    page_no = first_page < 0 ? 0 : (first_page >= page_total ? page_total - 1 : first_page);
    test_keys = !sys_stdin_is_console();
    zoom = FX_ONE;

    /* the screen as it is, to be put back */
    sys_screen_size( &rows, &cols );
    saved_rows = rows;
    saved_cols = cols;
    saved_cells = (u16 *)xalloc( (u32)rows * (u32)cols * 2 );
    sys_get_cells( 0, saved_cells, (u32)rows * (u32)cols );
    sys_get_cursor( &saved_row, &saved_col );

    /* the font the bar is written in: the built-in one, plain */
    mem_set( &ui_font, 0, sizeof( ui_font ) );
    ui_font.kind = FT_BUILTIN;
    ui_font.units = 1000;
    ui_font.key = 0xFFFFFFFFUL;
    ui_font.dw = -1;
    for ( index = 0; index < 256; index++ ) {
        ui_font.uni[index] = (u16)(index >= 32 && index < 127 ? index : 0);
        ui_font.width[index] = -1;
    }
    /* what the top line and a menu are drawn in, had now: by the time
       either is wanted a page may have all the memory there is */
    top_pix = (u8 *)xalloc( (u32)SCR_W * 3 * TOP_H );
    drop_pix = (u8 *)xalloc( (u32)DROP_W * 3 * DROP_H );

    in_text = want == WANT_TEXT;
    if ( want == WANT_VGA ) {
        gfx_kind = VS_VGA;
    } else if ( video_probe() ) {
        gfx_kind = VS_VBE;
    } else {
        gfx_kind = VS_VGA;
    }
    if ( !in_text ) {
        if ( !video_set( gfx_kind ) ) {
            /* no frame buffer after all: the VGA's own mode, or text */
            gfx_kind = gfx_kind == VS_VBE && video_set( VS_VGA ) ? VS_VGA : 0;
            in_text = gfx_kind == 0;
        }
    }
    while ( !quit ) {
        if ( in_text ) {
            text_loop();
        } else {
            gfx_loop();
        }
    }
    video_text();
    mouse_rest();
    sys_put_cells( 0, saved_cells, (u32)saved_rows * (u32)saved_cols );
    sys_cursor( saved_row, saved_col );
    sys_cursor_shape( CUR_LINE );
}
