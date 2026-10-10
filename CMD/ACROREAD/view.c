/*
 * VIEW.C - the viewer: a page on the screen, and the keys that move
 * about it.
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
 * TEST MODE.  With standard input redirected the keys come from it -
 * a byte for a plain key, 0 and the scan code for a function key, FFh
 * and a count for that many seconds' wait - and nothing interrupts a
 * drawing, so that what a key file leaves on the screen is the same
 * every time.
 */
#include "gfx.h"
#include "view.h"

#define AREA_H      464             /* the page's part of a graphics screen */
#define BAR_H       16
#define BACK_GREY   0x58
#define WORK_BYTES  (700UL * 1024)  /* left free for drawing a page with */

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

#define ATTR_TEXT   0x07
#define ATTR_BAR    0x70
#define ATTR_FOUND  0x0F

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
static CANVAS canvas;
static u32    canvas_cap;
static int    canvas_page = -1, canvas_turn;
static fx     canvas_zoom;
static CANVAS bar;
static FONT   ui_font;
static u32    last_shown;
static int    grey_told;

/* the text */
static int    rows, cols;
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

static void bar_show( const char *doing );

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
/* keys                                                                */
/* ------------------------------------------------------------------ */

static int key_get( void )
{
    u32 until;
    int byte, shift;

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
        key = key_get();
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
        "",
        "  ACROREAD - keys",
        "",
        "  PgDn, Space      Down the page, and on to the next page",
        "  PgUp, Backspace  Up the page, and back to the page before",
        "  Arrow keys       Move a little",
        "  Home, End        The top or the bottom of the page",
        "  N, P             The next or the previous page",
        "  Ctrl+Home        The first page",
        "  Ctrl+End         The last page",
        "  G                Go to a page by its number",
        "  Tab              Change between the picture and the text",
        "",
        "  In the picture:",
        "  +, -             Larger or smaller",
        "  W                Fit the page's width to the screen",
        "  F                Fit the whole page on the screen",
        "  R, L             Turn the page right or left",
        "",
        "  In the text:",
        "  F, F3            Find text, and find it again",
        "  W                Wrap long lines, or leave them long",
        "",
        "  Esc, Q           Leave ACROREAD"
    };
    int index, was_gfx = !in_text;

    if ( was_gfx ) {
        video_text();
        sys_screen_size( &rows, &cols );
    }
    for ( index = 0; index < rows; index++ ) {
        put_text( index, 0, ATTR_TEXT, index < (int)(sizeof( lines ) / sizeof( lines[0] )) ? lines[index] : "", cols );
    }
    put_text( rows - 1, 0, ATTR_BAR, " Press a key", cols );
    key_get();
    if ( was_gfx && !video_set( gfx_kind ) ) {
        gfx_kind = 0;
        in_text = 1;
    }
}

/* ------------------------------------------------------------------ */
/* the picture                                                         */
/* ------------------------------------------------------------------ */

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

/* the bar under the page: "doing" if that is something, else where
   we are */
static void bar_show( const char *doing )
{
    char text[100];
    int percent;

    if ( bar.pix == NULL ) {
        bar.width = 640;
        bar.height = BAR_H;
        bar.bpp = 3;
        bar.stride = 640 * 3;
        bar.pix = (u8 *)xalloc( bar.stride * BAR_H );
    }
    canvas_clear( &bar, 0x202830UL );
    if ( doing && doing[0] ) {
        ui_text( &bar, 8, 12, 13, 0xFFFFFFUL, doing );
    } else {
        ui_text( &bar, 8, 12, 13, 0xFFFFFFUL, file_name );
        sfmt( text, sizeof( text ), "Page %d of %d", page_no + 1, page_total );
        ui_text( &bar, 320 - ui_width( 13, text ) / 2, 12, 13, 0xFFFFFFUL, text );
        percent = (int)((zoom * 100 + 0x8000) >> 16);
        sfmt( text, sizeof( text ), "%d%%   F1 Help", percent );
        ui_text( &bar, 632 - ui_width( 13, text ), 12, 13, 0xC0D0E0UL, text );
    }
    video_show( &bar, 0, 0, AREA_H, AREA_H + BAR_H, 0 );
}

/* the canvas to the screen, and the bar */
static void gfx_show( const char *doing )
{
    if ( canvas.pix ) {
        video_show( &canvas, view_x - canvas.org_x, view_y - canvas.org_y, 0, AREA_H, BACK_GREY );
    }
    bar_show( doing );
    last_shown = sys_hundredths();
}

/* called while a page is being drawn: 1 to give it up */
static int drawing_poll( void )
{
    u32 now;

    if ( test_keys ) {
        return 0;
    }
    if ( sys_key_ready() ) {
        return 1;                                   /* whatever it is, it is what matters now */
    }
    now = sys_hundredths();
    if ( now - last_shown > 150 || now < last_shown ) {
        gfx_show( "Drawing the page..." );
    }
    return 0;
}

static void limits( void )
{
    int most_x = view.width - 640, most_y = view.height - AREA_H;

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
    int x1 = view_x + 640 > view.width ? view.width : view_x + 640;
    int y1 = view_y + AREA_H > view.height ? view.height : view_y + AREA_H;

    return canvas.pix != NULL && canvas_page == page_no && canvas_zoom == zoom && canvas_turn == turn &&
           x0 >= canvas.org_x && y0 >= canvas.org_y && x1 <= canvas.org_x + canvas.width &&
           y1 <= canvas.org_y + canvas.height;
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
            fit_w = fx_div( I2FX( 640 ), wide );
            fit_h = fx_div( I2FX( AREA_H ), high );
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
            if ( bpp == 3 && room < 640UL * AREA_H * 3 + WORK_BYTES ) {
                bpp = 1;
                if ( !grey_told ) {
                    str_cpy( notice, "Not enough memory for color: pages are drawn in gray" );
                    grey_told = 1;
                }
            }
            most = room > WORK_BYTES ? room - WORK_BYTES : 0;
            if ( most < 640UL * AREA_H * (u32)bpp ) {
                most = 640UL * AREA_H * (u32)bpp;
            }
            if ( (u32)cw * (u32)ch * (u32)bpp > most ) {
                if ( cw > 640 ) {
                    cw = 640;
                }
                ch = (int)(most / ((u32)cw * (u32)bpp));
                if ( ch > view.height ) {
                    ch = view.height;
                }
                if ( ch < AREA_H && ch < view.height ) {
                    ch = view.height < AREA_H ? view.height : AREA_H;
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
            canvas.org_x = view_x - (cw - 640) / 2;
            canvas.org_y = view_y - (ch - AREA_H) / 2;
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
            gfx_show( "Drawing the page..." );
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
    int mid_x = view_x + 320, mid_y = view_y + AREA_H / 2;

    if ( to < 0x2000L ) {
        to = 0x2000L;                               /* an eighth */
    }
    if ( to > 8 * FX_ONE ) {
        to = 8 * FX_ONE;
    }
    /* the middle of the screen stays the middle */
    view_x = (int)mul_div( mid_x, to, zoom ) - 320;
    view_y = (int)mul_div( mid_y, to, zoom ) - AREA_H / 2;
    zoom = to;
    fit = 0;
}

/* ------------------------------------------------------------------ */
/* the text                                                            */
/* ------------------------------------------------------------------ */

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
    for ( pass = 0; pass < 2; pass++ ) {
        count = 0;
        for ( index = 0; index < text_count; index++ ) {
            line = text_lines[index];
            len = str_len( line );
            at = 0;
            do {
                take = len - at;
                if ( wrap && take > (u32)cols ) {
                    take = (u32)cols;
                    for ( back = take; back > (u32)cols / 2; back-- ) {
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
    char text[100];
    const TROW *trow;
    const char *line;
    int row, body = rows - 2, len, from;

    sfmt( text, sizeof( text ), " %s", file_name );
    put_text( 0, 0, ATTR_BAR, text, cols );
    sfmt( text, sizeof( text ), "Page %d of %d ", page_no + 1, page_total );
    put_text( 0, cols - (int)str_len( text ), ATTR_BAR, text, (int)str_len( text ) );
    for ( row = 0; row < body; row++ ) {
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
                if ( len > cols ) {
                    len = cols;
                }
                mem_cpy( cells, line + from, (u32)len );
                cells[len] = 0;
            }
        }
        put_text( row + 1, 0, text_top + row == found_row ? ATTR_FOUND : ATTR_TEXT, cells, cols );
    }
    if ( notice[0] ) {
        sfmt( text, sizeof( text ), " %s", notice );
        notice[0] = 0;
    } else {
        str_cpy( text, " PgUp PgDn  N P Page  G Go to  F Find  Tab Picture  F1 Help  Esc Exit" );
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
    text_top = at_end && trow_count > rows - 2 ? trow_count - (rows - 2) : 0;
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
            sfmt( notice, sizeof( notice ), "There are %d pages", page_total );
        }
    }
}

static void text_loop( void )
{
    int key, body, most;

    sys_screen_size( &rows, &cols );
    if ( cols > 160 ) {
        cols = 160;
    }
    sys_cursor_shape( CUR_HIDE );
    text_page = -1;
    text_go( page_no, 0 );
    while ( !quit && in_text ) {
        body = rows - 2;
        most = trow_count > body ? trow_count - body : 0;
        if ( text_top > most ) {
            text_top = most;
        }
        if ( text_top < 0 ) {
            text_top = 0;
        }
        text_show();
        key = key_get();
        switch ( key ) {
        case KEY_ESC: case 'q': case 'Q': case KEY_ALTX: case KEY_F10:
            quit = 1;
            break;
        case KEY_TAB:
            if ( gfx_kind && video_set( gfx_kind ) ) {
                in_text = 0;
            } else {
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
            text_left = text_left > 8 ? text_left - 8 : 0;
            break;
        case KEY_PGDN: case ' ': case KEY_ENTER:
            if ( text_top >= most ) {
                if ( page_no + 1 < page_total ) {
                    text_go( page_no + 1, 0 );
                }
            } else {
                text_top += body - 1;
            }
            break;
        case KEY_PGUP: case KEY_BKSP:
            if ( text_top == 0 ) {
                if ( page_no > 0 ) {
                    text_go( page_no - 1, 1 );
                }
            } else {
                text_top -= body - 1;
            }
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

    canvas_page = -1;
    gfx_page( 0 );
    while ( !quit && !in_text ) {
        /* a key already typed is seen to first: the drawing it broke
           off would only be broken off again */
        if ( !canvas_covers() && !key_waiting() ) {
            gfx_page( 0 );
        }
        gfx_show( notice );
        notice[0] = 0;
        key = key_get();
        switch ( key ) {
        case KEY_ESC: case 'q': case 'Q': case KEY_ALTX: case KEY_F10:
            quit = 1;
            break;
        case KEY_TAB: case 't': case 'T':
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
            if ( view_y + AREA_H >= view.height ) {
                if ( page_no + 1 < page_total ) {
                    gfx_go( page_no + 1, 1 );
                }
            } else {
                view_y += AREA_H - 32;
            }
            break;
        case KEY_PGUP: case KEY_BKSP:
            if ( view_y <= 0 ) {
                if ( page_no > 0 ) {
                    gfx_go( page_no - 1, -1 );
                }
            } else {
                view_y -= AREA_H - 32;
            }
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
            if ( (canvas_zoom != zoom || canvas_turn != turn || canvas_page != page_no) && !key_waiting() ) {
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
    sys_put_cells( 0, saved_cells, (u32)saved_rows * (u32)saved_cols );
    sys_cursor( saved_row, saved_col );
    sys_cursor_shape( CUR_LINE );
}
