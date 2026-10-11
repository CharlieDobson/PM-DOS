/*
 * UI.C - what ScanDisk's screens are put together from: a row of
 * buttons, a box that asks something, and a window of text that
 * scrolls.  They look as MS-DOS ScanDisk's do on a text screen, and on
 * a graphics screen (SCANDISK /G) they are the same things in the same
 * places, drawn as a graphics screen can draw them.
 *
 * A BUTTON is its label between angle brackets, < Like This >, and the
 * one Enter would press has arrowheads for brackets.  A row of them is
 * worked the same way wherever it is: Tab and the arrow keys move from
 * one to the next, Enter or the space bar chooses the one that is lit,
 * a button's letter chooses it at once, and the mouse chooses the one
 * it is pressed AND let go on.  A row with no button lit - the buttons
 * under a check that is running - is worked by the letters and the
 * mouse alone.  Buttons on the screen itself cast a shadow; buttons in
 * a box do not.  On a graphics screen a button is a raised grey face,
 * taller than its row of text, with a heavy outline round the one that
 * is lit and its letter underlined.
 *
 * A BOX is drawn over whatever ui_backdrop() was last told paints the
 * screen, every time round its loop (SCR.C: the whole screen is drawn
 * again and only what differs is sent), so nothing has to remember what
 * a box covers.  One box is up at a time: "More Info" takes the place
 * of the question it was asked from, and the question comes back.
 */
#include "ui.h"

#define ASK_WRAP        62      /* a box's text: MS-DOS ScanDisk's width */
#define TEXT_WRAP       64      /* a window of text */

static void (*backdrop)( void );
static int  boxes;              /* how many boxes are up */

void ui_backdrop( void (*paint)( void ) )
{
    backdrop = paint;
}

int ui_box_up( void )
{
    return boxes > 0;
}

static void paint_under( void )
{
    if ( backdrop ) {
        backdrop();
    }
}

/* ------------------------------------------------------------------ */
/* buttons                                                             */
/* ------------------------------------------------------------------ */

void btn_set( BTNROW *row, const BTN *btn, int count, int focus, int on_desk )
{
    row->btn = btn;
    row->count = count > BTN_MAX ? BTN_MAX : count;
    row->focus = focus;
    row->armed = -1;
    row->on_desk = on_desk;
    row->row = -1;
}

int btn_width( const BTNROW *row )
{
    int index, width = 0;

    for ( index = 0; index < row->count; index++ ) {
        width += scr_hotlen( row->btn[index].label ) + 4 + (index ? 3 : 0);
    }
    return width;
}

/* one button, its label two cells in from each end */
static void btn_one( BTNROW *row, int index, int at_row, int col )
{
    const char *label = row->btn[index].label;
    int len = scr_hotlen( label ), wide = len + 4;
    /* nothing is lit, and no letter shown, on a row a box is over or a
       check is running above */
    int quiet = row->on_desk && (ui_box_up() || row->focus < 0);
    int lit = index == row->focus && !(row->on_desk && ui_box_up());
    int down = index == row->armed;
    u8 attr, hot;
    u32 flags;

    row->col[index] = col;
    row->len[index] = wide;
    if ( gfx_on ) {
        scr_fill( at_row, col, wide, ' ', pal.button );
        if ( quiet ) {
            scr_hot( at_row, col + 2, label, pal.button, pal.button );
            scr_recolor( at_row, col, wide, pal.button );       /* and no underline */
        } else {
            scr_hot( at_row, col + 2, label, pal.button, pal.button_hot );
        }
        flags = (lit ? CF_FOCUS : 0) | (down ? CF_DOWN : 0);
        scr_flag( at_row - 1, col, wide, flags | ((u32)BP_ABOVE << CF_PART_SHIFT) );
        scr_flag( at_row, col, wide, flags | ((u32)BP_LABEL << CF_PART_SHIFT) );
        scr_flag( at_row + 1, col, wide, flags | ((u32)BP_BELOW << CF_PART_SHIFT) );
        scr_flag( at_row - 1, col, 1, CF_LEFT );
        scr_flag( at_row, col, 1, CF_LEFT );
        scr_flag( at_row + 1, col, 1, CF_LEFT );
        scr_flag( at_row - 1, col + wide - 1, 1, CF_RIGHT );
        scr_flag( at_row, col + wide - 1, 1, CF_RIGHT );
        scr_flag( at_row + 1, col + wide - 1, 1, CF_RIGHT );
        return;
    }
    if ( down ) {
        attr = hot = pal.button_down;
    } else if ( lit ) {
        attr = hot = pal.button_focus;
    } else {
        attr = pal.button;
        hot = quiet ? pal.button : pal.button_hot;
    }
    scr_ch( at_row, col, lit ? 0x11 : '<', attr );
    scr_ch( at_row, col + 1, ' ', attr );
    scr_hot( at_row, col + 2, label, attr, hot );
    scr_ch( at_row, col + 2 + len, ' ', attr );
    scr_ch( at_row, col + 3 + len, lit ? 0x10 : '>', attr );
    if ( row->on_desk ) {
        /* half a cell of black to the right and below */
        scr_ch( at_row, col + wide, 0xDC, pal.button_shadow );
        scr_fill( at_row + 1, col + 1, wide, 0xDF, pal.button_shadow );
    }
}

void btn_draw_from( BTNROW *row, int at_row, int col )
{
    int index;

    row->row = at_row;
    for ( index = 0; index < row->count; index++ ) {
        btn_one( row, index, at_row, col );
        col += row->len[index] + 3;
    }
    scr_cursor( 0, 0, CUR_HIDE );
}

void btn_draw( BTNROW *row, int at_row, int left, int width )
{
    btn_draw_from( row, at_row, left + (width - btn_width( row )) / 2 );
}

static int btn_at( const BTNROW *row, int at_row, int col )
{
    int index;

    if ( at_row != row->row ) {
        return -1;
    }
    for ( index = 0; index < row->count; index++ ) {
        if ( col >= row->col[index] && col < row->col[index] + row->len[index] ) {
            return index;
        }
    }
    return -1;
}

int btn_key( BTNROW *row, int key )
{
    int index, letter;

    if ( key == K_MOUSE ) {
        index = btn_at( row, mouse.row, mouse.col );
        if ( mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE ) {
            row->armed = index;
            if ( index >= 0 && row->focus >= 0 ) {
                row->focus = index;
            }
        } else if ( mouse.kind == ME_UP ) {
            if ( row->armed >= 0 && index == row->armed ) {
                row->armed = -1;
                return row->btn[index].id;
            }
            row->armed = -1;
        }
        return 0;
    }
    if ( row->focus >= 0 ) {
        switch ( key ) {
        case K_TAB:
        case K_RIGHT:
        case K_DOWN:
            row->focus = (row->focus + 1) % row->count;
            return 0;
        case K_SHTAB:
        case K_LEFT:
        case K_UP:
            row->focus = (row->focus + row->count - 1) % row->count;
            return 0;
        case K_ENTER:
        case ' ':
            return row->btn[row->focus].id;
        }
    }
    letter = key_alt_letter( key );
    if ( letter == 0 && key > ' ' && key < 0x100 ) {
        letter = ch_upper( key );
    }
    if ( letter ) {
        for ( index = 0; index < row->count; index++ ) {
            if ( scr_hotkey( row->btn[index].label ) == letter ) {
                if ( row->focus >= 0 ) {
                    row->focus = index;
                }
                return row->btn[index].id;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* a box                                                               */
/*                                                                     */
/* On a text screen: grey, a white line round it with the title in the */
/* top of the line, and a rule across it over the row the buttons are  */
/* in.  On a graphics screen: a raised grey panel, the title in a bar  */
/* along its top.  Either way it casts a shadow.                       */
/* ------------------------------------------------------------------ */

void ui_frame( int top, int left, int bottom, int right, const char *title )
{
    int len = (int)str_len( title ), wide = right - left + 1, row, col;

    if ( gfx_on ) {
        for ( row = top; row <= bottom; row++ ) {
            scr_fill( row, left, wide, ' ', row == top ? pal.dlg_caption : pal.dlg );
        }
        if ( len == 0 ) {
            title = "ScanDisk";
            len = 8;
        }
        scr_put( top, left + (wide - len) / 2, title, pal.dlg_caption );
        scr_fill( bottom - 2, left + 1, wide - 2, 0xC4, pal.dlg_frame );
        scr_flag( top, left, wide, CF_TOP | CF_RAISED );
        scr_flag( bottom, left, wide, CF_BOTTOM | CF_RAISED );
        for ( row = top; row <= bottom; row++ ) {
            scr_flag( row, left, 1, CF_LEFT | CF_RAISED );
            scr_flag( row, right, 1, CF_RIGHT | CF_RAISED );
        }
    } else {
        scr_box( top, left, bottom, right, pal.dlg_frame );
        for ( row = top + 1; row < bottom; row++ ) {
            scr_fill( row, left + 1, wide - 2, ' ', pal.dlg );
        }
        if ( len ) {
            col = left + (wide - len - 2) / 2;
            scr_ch( top, col, ' ', pal.dlg_frame );
            scr_put( top, col + 1, title, pal.dlg_frame );
            scr_ch( top, col + 1 + len, ' ', pal.dlg_frame );
        }
        scr_ch( bottom - 2, left, 0xC3, pal.dlg_frame );
        scr_fill( bottom - 2, left + 1, wide - 2, 0xC4, pal.dlg_frame );
        scr_ch( bottom - 2, right, 0xB4, pal.dlg_frame );
    }
    scr_shadow( top, left, bottom, right );
}

/* ------------------------------------------------------------------ */
/* a box that asks                                                     */
/* ------------------------------------------------------------------ */

int ui_dialog( const char *title, const TEXT *body, const BTN *btn, int count, int focus,
               int ctrlc, int pic )
{
    BTNROW row;
    int show_pic = gfx_on && pic != PIC_NONE;
    int lines = body->count, wide = scr_cols - 12, rows, top, left, bottom, right, index, key;
    int answer = 0;

    if ( show_pic && lines < 2 ) {
        lines = 2;                      /* a sign is two rows high */
    }
    btn_set( &row, btn, count, focus, 0 );
    rows = lines + 6;
    /* a question starts under the title, as MS-DOS ScanDisk's do; a
       message with nothing to choose is in the middle of the screen */
    top = count > 1 ? 4 : (scr_rows - rows) / 2;
    if ( top + rows > scr_rows - 2 ) {
        top = scr_rows - 2 - rows;
    }
    if ( top < 0 ) {
        top = 0;
    }
    left = (scr_cols - wide) / 2;
    bottom = top + rows - 1;
    right = left + wide - 1;

    boxes++;
    while ( answer == 0 ) {
        paint_under();
        ui_frame( top, left, bottom, right, title );
        if ( show_pic ) {
            scr_icon( top + 2, left + 3, pic, pal.dlg );
        }
        for ( index = 0; index < body->count; index++ ) {
            scr_put( top + 2 + index, left + (show_pic ? 9 : 3), body->line[index], pal.dlg );
        }
        btn_draw( &row, bottom - 1, left + 1, wide - 2 );
        scr_flush();
        key = key_get();
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        if ( key == K_SHOT ) {
            scr_shot();
            continue;
        }
        if ( key == K_CTRLC && ctrlc ) {
            answer = ctrlc;
            break;
        }
        answer = btn_key( &row, key );
    }
    boxes--;
    return answer;
}

int ui_ask( const char *title, const char *para, const BTN *btn, int count, int focus, int ctrlc,
            int pic )
{
    TEXT body;
    int answer, width = scr_cols - 18;

    if ( width > ASK_WRAP ) {
        width = ASK_WRAP;
    }
    if ( gfx_on && pic != PIC_NONE ) {
        width -= 6;
    }
    tx_init( &body );
    tx_wrap( &body, para, width );
    answer = ui_dialog( title, &body, btn, count, focus, ctrlc, pic );
    tx_free( &body );
    return answer;
}

/* ------------------------------------------------------------------ */
/* a window of text                                                    */
/*                                                                     */
/* Most of the screen, whatever it is given.  When there is more than  */
/* fits, a bar on its right edge says where in the text the window is  */
/* and "(more)" under the last line says there is more below.          */
/* ------------------------------------------------------------------ */

static int thumb_at( int first, int max_first, int track )
{
    return max_first ? (int)((long)first * (track - 1) / max_first) : 0;
}

int ui_text( const char *title, const TEXT *text, const BTN *btn, int count, int ctrlc, int esc )
{
    static const char more[] = "(more)";
    BTNROW row;
    int top = 2, left = 3, bottom = scr_rows - 4, right = scr_cols - 6;
    int cols = right - left - 6, rows = bottom - top - 5, bar = gfx_on ? right - 1 : right;
    int first = 0, shift = 0, max_first, max_shift = 0, wide = 0, index, line, len, key;
    int answer = 0, track = rows - 2, heading;
    const char *src;

    for ( index = 0; index < text->count; index++ ) {
        len = (int)str_len( text->line[index] );
        if ( len > wide ) {
            wide = len;
        }
    }
    btn_set( &row, btn, count, 0, 0 );
    max_first = text->count > rows ? text->count - rows : 0;
    if ( wide > cols ) {
        max_shift = wide - cols;
    }

    boxes++;
    while ( answer == 0 ) {
        paint_under();
        ui_frame( top, left, bottom, right, title );
        for ( line = 0; line < rows && first + line < text->count; line++ ) {
            src = text->line[first + line];
            heading = src[0] == '\1';
            if ( heading ) {
                src++;
            }
            len = (int)str_len( src ) - shift;
            if ( len > cols ) {
                len = cols;
            }
            if ( len > 0 ) {
                scr_putn( top + 2 + line, left + 3, src + shift, len,
                          heading ? pal.dlg_bright : pal.dlg );
            }
        }
        if ( max_first ) {
            scr_ch( top + 2, bar, 0x18, pal.dlg );
            for ( line = 0; line < track; line++ ) {
                if ( line == thumb_at( first, max_first, track ) ) {
                    scr_ch( top + 3 + line, bar, 0xDB, pal.dlg_bright );
                } else {
                    scr_ch( top + 3 + line, bar, 0xB1, pal.dlg );
                }
            }
            scr_ch( top + 1 + rows, bar, 0x19, pal.dlg );
            if ( first < max_first ) {
                scr_put( bottom - 3, right - 3 - (int)sizeof( more ) + 1, more, pal.dlg_bright );
            }
        }
        btn_draw( &row, bottom - 1, left + 1, right - left - 1 );
        scr_flush();

        key = key_get();
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        if ( key == K_SHOT ) {
            scr_shot();
            continue;
        }
        if ( key == K_CTRLC && ctrlc ) {
            answer = ctrlc;
            break;
        }
        if ( key == K_MOUSE && (mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE) && max_first &&
             mouse.col == bar && mouse.row >= top + 2 && mouse.row <= top + 1 + rows ) {
            line = mouse.row - (top + 3);
            if ( mouse.row == top + 2 ) {
                key = K_UP;
            } else if ( mouse.row == top + 1 + rows ) {
                key = K_DOWN;
            } else {
                key = line < thumb_at( first, max_first, track ) ? K_PGUP : K_PGDN;
            }
        }
        switch ( key ) {
        case K_UP:    first--;              break;
        case K_DOWN:  first++;              break;
        case K_PGUP:  first -= rows - 1;    break;
        case K_PGDN:  first += rows - 1;    break;
        case K_HOME:
        case K_CHOME: first = 0; shift = 0; break;
        case K_END:
        case K_CEND:  first = max_first;    break;
        case K_ESC:   answer = esc;         break;
        default:
            if ( (key == K_LEFT || key == K_RIGHT) && max_shift ) {
                shift += key == K_LEFT ? -8 : 8;
            } else {
                answer = btn_key( &row, key );
            }
            break;
        }
        if ( first > max_first ) {
            first = max_first;
        }
        if ( first < 0 ) {
            first = 0;
        }
        if ( shift > max_shift ) {
            shift = max_shift;
        }
        if ( shift < 0 ) {
            shift = 0;
        }
    }
    boxes--;
    return answer;
}

int ui_info( const char *title, const char *para, const BTN *btn, int count, int ctrlc, int esc )
{
    TEXT text;
    int answer, width = scr_cols - 16;

    tx_init( &text );
    tx_wrap( &text, para, width > TEXT_WRAP ? TEXT_WRAP : width );
    answer = ui_text( title, &text, btn, count, ctrlc, esc );
    tx_free( &text );
    return answer;
}
