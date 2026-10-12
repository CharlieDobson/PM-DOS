/*
 * UI.C - what DEFRAG's boxes are put together from: the menu, a frame,
 * buttons, round buttons, a box that asks, a window of text.  They look
 * as MS-DOS DEFRAG's do - on a text screen as its do when it is started
 * with /G0 and draws with the characters there are, on a graphics
 * screen (DEFRAG /G) as its do when it has loaded its own shapes into
 * the font.
 *
 * A BUTTON is a slab with its label in the middle and a shadow under
 * it and to its right, half a cell deep.  The one Enter presses has an
 * arrowhead at each end; when the keys are on the buttons it is white.
 * Pressed with the mouse, a button moves onto its own shadow, and it is
 * chosen when the mouse lets go on it.
 *
 * A ROUND BUTTON is one of a group, of which one is chosen.  The arrow
 * keys move from one to the next, and the space bar - or the mouse -
 * chooses; moving does not, as it does not in MS-DOS DEFRAG.
 *
 * A BOX is drawn over whatever ui_backdrop() was told paints the
 * screen, every time round its loop (SCR.C: the whole screen is drawn
 * again and only what differs is sent), so nothing has to remember what
 * a box covers.
 *
 * F1 IS HELP WHEREVER A KEY IS WAITED FOR, and the test mode's keys -
 * write the screen to the log, the key file has run out - are seen to
 * in the same place: ui_common_key().
 */
#include "ui.h"
#include "text.h"

static void (*backdrop)( void );
static const char *hint_text;
static int in_help;

void ui_backdrop( void (*paint)( void ) )
{
    backdrop = paint;
}

void ui_paint_under( void )
{
    if ( backdrop ) {
        backdrop();
    }
}

void ui_hint( const char *text )
{
    hint_text = text;
}

const char *ui_hint_now( void )
{
    return hint_text;
}

void app_abandon( void )
{
    leave( 100 );
}

int ui_common_key( int key )
{
    switch ( key ) {
    case K_EOF:
        app_abandon();
        return 1;
    case K_DUMP:
        scr_dump();
        return 1;
    case K_SHOT:
        scr_shot();
        return 1;
    case K_F1:
        if ( !in_help ) {
            ui_help();
        }
        return 1;
    }
    return 0;
}

/* a label without the '&' that marks its letter */
static void put_label( int row, int col, const char *label, u8 attr )
{
    for ( ; *label; label++ ) {
        if ( *label != '&' ) {
            scr_ch( row, col++, (u8)*label, attr );
        }
    }
}

/* where a box of that size goes: where MS-DOS DEFRAG puts one */
static void place( int width, int height, int *top, int *left )
{
    *top = (scr_rows - height) / 2;
    *left = (79 - width) / 2;
    if ( *top < 1 ) {
        *top = 1;
    }
    if ( *left < 0 ) {
        *left = 0;
    }
}

/* ------------------------------------------------------------------ */
/* a frame                                                             */
/*                                                                     */
/* On a text screen: a double line with the title in the top of it.    */
/* On a graphics screen: a thin line all the way round, and the title  */
/* in a white bar along the top with a small box in its corner, which  */
/* the mouse can press to close the box.  Either way it has a shadow.  */
/* ------------------------------------------------------------------ */

void ui_frame( int top, int left, int bottom, int right, const char *title, int error )
{
    u8 attr = error ? pal.err : pal.dlg, title_attr = error ? pal.err_title : pal.dlg_title;
    int wide = right - left + 1, len = title ? (int)str_len( title ) : 0, row;

    if ( gfx_on ) {
        for ( row = top; row <= bottom; row++ ) {
            scr_fill( row, left, wide, ' ', attr );
            scr_flag( row, left, 1, CF_LEFT );
            scr_flag( row, right, 1, CF_RIGHT );
        }
        if ( title ) {
            scr_fill( top, left, wide, ' ', pal.dlg_caption );
            scr_center( top, left, wide, title, pal.dlg_caption );
            scr_pic( top, left, PIC_CLOSE, 0, pal.dlg_caption );
            scr_pic( top, left + 1, PIC_CLOSE, 1, pal.dlg_caption );
            scr_flag( top, left + 2, wide - 2, CF_BOTTOM );
        }
        scr_flag( top, left, wide, CF_TOP );
        scr_flag( top, left, 1, CF_LEFT );
        scr_flag( top, right, 1, CF_RIGHT );
        scr_flag( bottom, left, wide, CF_BOTTOM );
    } else {
        scr_box( top, left, bottom, right, attr, 1 );
        if ( title ) {
            scr_ch( top, left + (wide - len - 2) / 2, ' ', attr );
            scr_put( top, left + (wide - len - 2) / 2 + 1, title, title_attr );
            scr_ch( top, left + (wide - len - 2) / 2 + 1 + len, ' ', attr );
        }
    }
    scr_shadow( top, left, bottom, right );
}

int ui_close_hit( int top, int left )
{
    return gfx_on && (mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE) && mouse.row == top
           && mouse.col >= left && mouse.col <= left + 1;
}

/* ------------------------------------------------------------------ */
/* buttons                                                             */
/* ------------------------------------------------------------------ */

void btn_set( BTNROW *row, const BTN *btn, int count, int width, int chosen )
{
    row->btn = btn;
    row->count = count > BTN_MAX ? BTN_MAX : count;
    row->width = width;
    row->chosen = chosen;
    row->focus = 1;
    row->armed = -1;
}

void btn_draw_at( BTNROW *row, int index, int at_row, int col, u8 ground )
{
    const char *label = row->btn[index].label;
    int len = scr_hotlen( label ), wide = row->width;
    int lit = index == row->chosen, down = index == row->armed, face = col + (down ? 1 : 0);
    u8 attr = lit ? (row->focus ? pal.btn_focus : pal.btn_default) : pal.btn;
    u8 shade = (u8)(ground & 0xF0);

    row->row[index] = at_row;
    row->col[index] = col;
    row->ground = ground;
    scr_fill( at_row, col, wide + 1, ' ', ground );
    scr_fill( at_row + 1, col, wide + 1, ' ', ground );
    scr_fill( at_row, face, wide, ' ', attr );
    put_label( at_row, face + (wide - len) / 2, label, attr );
    if ( lit ) {
        scr_ch( at_row, face, 0x10, attr );
        scr_ch( at_row, face + wide - 1, 0x11, attr );
    }
    if ( !down ) {
        scr_ch( at_row, col + wide, 0xDC, shade );
        scr_fill( at_row + 1, col + 1, wide, 0xDF, shade );
    }
}

void btn_draw( BTNROW *row, int at_row, int left, int width, int gap, u8 ground )
{
    int total = row->count * row->width + (row->count - 1) * gap;
    int col = left + (width - total) / 2, index;

    for ( index = 0; index < row->count; index++ ) {
        btn_draw_at( row, index, at_row, col + index * (row->width + gap), ground );
    }
    scr_cursor( 0, 0, CUR_HIDE );
}

static int btn_at( const BTNROW *row, int at_row, int col )
{
    int index;

    for ( index = 0; index < row->count; index++ ) {
        if ( at_row == row->row[index] && col >= row->col[index]
             && col < row->col[index] + row->width ) {
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
            if ( index >= 0 ) {
                row->chosen = index;
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
    if ( key == K_ENTER ) {
        return row->btn[row->chosen].id;
    }
    if ( row->focus ) {
        switch ( key ) {
        case K_RIGHT:
        case K_DOWN:
            row->chosen = (row->chosen + 1) % row->count;
            return 0;
        case K_LEFT:
        case K_UP:
            row->chosen = (row->chosen + row->count - 1) % row->count;
            return 0;
        case ' ':
            return row->btn[row->chosen].id;
        }
    }
    letter = key_alt_letter( key );
    if ( letter == 0 && key > ' ' && key < 0x100 ) {
        letter = ch_upper( key );
    }
    if ( letter ) {
        for ( index = 0; index < row->count; index++ ) {
            if ( scr_hotkey( row->btn[index].label ) == letter ) {
                row->chosen = index;
                return row->btn[index].id;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* round buttons                                                       */
/* ------------------------------------------------------------------ */

void radio_draw( const RADIO *grp, int focus, u8 attr )
{
    int index, row, on;
    u8 mark;

    for ( index = 0; index < grp->count; index++ ) {
        row = grp->row + index * grp->step;
        on = index == grp->chosen;
        mark = focus && index == grp->at ? pal.dlg_focus : attr;
        if ( gfx_on ) {
            scr_pic( row, grp->col, PIC_RADIO, on ? 2 : 0, mark );
            scr_pic( row, grp->col + 1, PIC_RADIO, on ? 3 : 1, mark );
            scr_ch( row, grp->col + 2, ' ', attr );
        } else {
            scr_ch( row, grp->col, '(', mark );
            scr_ch( row, grp->col + 1, on ? 0x07 : ' ', mark );
            scr_ch( row, grp->col + 2, ')', mark );
        }
        scr_put( row, grp->col + grp->gap, grp->label[index], attr );
    }
}

int radio_key( RADIO *grp, int key )
{
    switch ( key ) {
    case K_UP:
        grp->at = (grp->at + grp->count - 1) % grp->count;
        return 1;
    case K_DOWN:
        grp->at = (grp->at + 1) % grp->count;
        return 1;
    case ' ':
        grp->chosen = grp->at;
        return 1;
    }
    return 0;
}

int radio_mouse( RADIO *grp )
{
    int index;

    if ( mouse.kind != ME_DOWN && mouse.kind != ME_DOUBLE ) {
        return 0;
    }
    for ( index = 0; index < grp->count; index++ ) {
        if ( mouse.row == grp->row + index * grp->step && mouse.col >= grp->col
             && mouse.col < grp->col + grp->wide ) {
            grp->at = grp->chosen = index;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the menu                                                            */
/* ------------------------------------------------------------------ */

#define MENU_TOP        1
#define MENU_LEFT       2
#define MENU_RIGHT      35

static int menu_at;             /* the item that is lit */

/* the menu's name in the top line: the line itself is the screen's */
void ui_menu_bar( int lit )
{
    scr_fill( 0, MENU_LEFT, 12, ' ', lit ? pal.bar_sel : pal.bar );
    put_label( 0, MENU_LEFT + 2, tx_menu_title, lit ? pal.bar_sel : pal.bar );
}

int ui_menu_hit( void )
{
    return (mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE) && mouse.row == 0
           && mouse.col >= MENU_LEFT && mouse.col < MENU_LEFT + 12;
}

static void menu_move( int by )
{
    do {
        menu_at = (menu_at + MENU_ITEMS + by) % MENU_ITEMS;
    } while ( tx_menu[menu_at].label == NULL );
}

/* the item the mouse is on, or -1 */
static int menu_under( void )
{
    int index = mouse.row - (MENU_TOP + 1);

    if ( index < 0 || index >= MENU_ITEMS || mouse.col <= MENU_LEFT || mouse.col >= MENU_RIGHT
         || tx_menu[index].label == NULL ) {
        return -1;
    }
    return index;
}

int ui_menu( int open )
{
    int bottom = MENU_TOP + MENU_ITEMS + 1, index, row, key, letter;
    const MENUTEXT *item;
    u8 attr;

    (void)open;
    for ( ;; ) {
        ui_hint( tx_menu[menu_at].hint );
        ui_paint_under();
        ui_menu_bar( 1 );
        scr_box( MENU_TOP, MENU_LEFT, bottom, MENU_RIGHT, pal.bar, 0 );
        for ( index = 0; index < MENU_ITEMS; index++ ) {
            item = &tx_menu[index];
            row = MENU_TOP + 1 + index;
            if ( item->label == NULL ) {
                scr_fill( row, MENU_LEFT + 2, MENU_RIGHT - MENU_LEFT - 3, 0xC4, pal.bar );
                continue;
            }
            attr = index == menu_at ? pal.bar_sel : pal.bar;
            scr_fill( row, MENU_LEFT + 1, MENU_RIGHT - MENU_LEFT - 1, ' ', attr );
            scr_hot( row, MENU_LEFT + 2, item->label, attr, index == menu_at ? attr : pal.bar_hot );
            if ( item->key ) {
                scr_put( row, MENU_RIGHT - 1 - (int)str_len( item->key ), item->key, attr );
            }
        }
        scr_shadow( MENU_TOP, MENU_LEFT, bottom, MENU_RIGHT );
        scr_cursor( 0, 0, CUR_HIDE );
        scr_flush();

        key = key_get();
        if ( ui_common_key( key ) ) {
            continue;
        }
        if ( key == K_MOUSE ) {
            index = menu_under();
            if ( index >= 0 ) {
                menu_at = index;
                if ( mouse.kind == ME_UP ) {
                    return tx_menu[index].go;
                }
            } else if ( mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE ) {
                return 0;               /* pressed anywhere else: closed */
            }
            continue;
        }
        switch ( key ) {
        case K_UP:
            menu_move( -1 );
            continue;
        case K_DOWN:
            menu_move( 1 );
            continue;
        case K_HOME:
            menu_at = MENU_ITEMS - 1;
            menu_move( 1 );
            continue;
        case K_END:
            menu_at = 0;
            menu_move( -1 );
            continue;
        case K_ENTER:
            return tx_menu[menu_at].go;
        case K_ESC:
        case K_ALTTAP:
        case K_F10:
            return 0;
        }
        letter = key_alt_letter( key );
        if ( letter == 0 && key > ' ' && key < 0x100 ) {
            letter = ch_upper( key );
        }
        for ( index = 0; letter && index < MENU_ITEMS; index++ ) {
            if ( tx_menu[index].label && scr_hotkey( tx_menu[index].label ) == letter ) {
                menu_at = index;
                return tx_menu[index].go;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* a box that asks                                                     */
/* ------------------------------------------------------------------ */

int ui_ask( const char *title, const char *text, const BTN *btn, int count, int width, int chosen,
            int esc, int error )
{
    BTNROW row;
    TEXT lines;
    u8 attr = error ? pal.err : pal.dlg;
    int wide = count * width + (count - 1) * 3 + 2, high, top, left, index, len, key, answer = 0;

    tx_init( &lines );
    tx_wrap( &lines, text, 56 );
    for ( index = 0; index < lines.count; index++ ) {
        len = (int)str_len( lines.line[index] );
        if ( len > wide ) {
            wide = len;
        }
    }
    if ( title && (int)str_len( title ) + 6 > wide ) {
        wide = (int)str_len( title ) + 6;
    }
    wide += 4;
    high = lines.count + 6;
    place( wide, high, &top, &left );
    btn_set( &row, btn, count, width, chosen );

    while ( answer == 0 ) {
        ui_paint_under();
        ui_frame( top, left, top + high - 1, left + wide - 1, title, error );
        for ( index = 0; index < lines.count; index++ ) {
            scr_center( top + 2 + index, left + 1, wide - 2, lines.line[index], attr );
        }
        btn_draw( &row, top + high - 3, left + 1, wide - 2, 3, attr );
        scr_flush();

        key = key_get();
        if ( ui_common_key( key ) ) {
            continue;
        }
        if ( key == K_ESC || key == K_CTRLC || (key == K_MOUSE && title && ui_close_hit( top, left )) ) {
            answer = esc;
            if ( answer ) {
                break;
            }
            continue;
        }
        answer = btn_key( &row, key );
    }
    tx_free( &lines );
    return answer;
}

/* ------------------------------------------------------------------ */
/* help: a window of text that scrolls                                 */
/* ------------------------------------------------------------------ */

static int thumb_at( int first, int max_first, int track )
{
    return max_first ? (int)((long)first * (track - 1) / max_first) : 0;
}

void ui_help( void )
{
    static const BTN ok[] = { { tx_btn_ok, 1 } };
    BTNROW row;
    TEXT text;
    const char *before = ui_hint_now(), *src;
    int wide = 66, high = scr_rows - 6, top, left, rows, bar, track;
    int first = 0, max_first, line, key, heading, done = 0;

    in_help = 1;
    tx_init( &text );
    tx_wrap( &text, tx_help, wide - 8 );
    place( wide, high, &top, &left );
    rows = high - 6;                    /* lines of text that show */
    bar = left + wide - 3;
    track = rows - 2;
    max_first = text.count > rows ? text.count - rows : 0;
    btn_set( &row, ok, 1, 10, 0 );
    ui_hint( tx_hint_help );

    while ( !done ) {
        ui_paint_under();
        ui_frame( top, left, top + high - 1, left + wide - 1, tx_help_title, 0 );
        for ( line = 0; line < rows && first + line < text.count; line++ ) {
            src = text.line[first + line];
            heading = src[0] == '\1';
            scr_put( top + 2 + line, left + 3, src + heading, heading ? pal.dlg_title : pal.dlg );
        }
        if ( max_first ) {
            scr_ch( top + 2, bar, 0x18, pal.track );
            for ( line = 0; line < track; line++ ) {
                scr_ch( top + 3 + line, bar, line == thumb_at( first, max_first, track ) ? 0xDB : 0xB1,
                        pal.track );
            }
            scr_ch( top + 1 + rows, bar, 0x19, pal.track );
        }
        btn_draw( &row, top + high - 3, left + 1, wide - 2, 3, pal.dlg );
        scr_flush();

        key = key_get();
        if ( ui_common_key( key ) ) {
            continue;
        }
        if ( key == K_MOUSE ) {
            if ( ui_close_hit( top, left ) ) {
                break;
            }
            if ( (mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE) && max_first && mouse.col == bar
                 && mouse.row >= top + 2 && mouse.row <= top + 1 + rows ) {
                if ( mouse.row == top + 2 ) {
                    key = K_UP;
                } else if ( mouse.row == top + 1 + rows ) {
                    key = K_DOWN;
                } else {
                    key = mouse.row - (top + 3) < thumb_at( first, max_first, track ) ? K_PGUP : K_PGDN;
                }
            }
        }
        switch ( key ) {
        case K_UP:      first--;                break;
        case K_DOWN:    first++;                break;
        case K_PGUP:    first -= rows - 1;      break;
        case K_PGDN:    first += rows - 1;      break;
        case K_HOME:
        case K_CHOME:   first = 0;              break;
        case K_END:
        case K_CEND:    first = max_first;      break;
        case K_ESC:     done = 1;               break;
        default:
            if ( btn_key( &row, key ) ) {
                done = 1;
            }
            break;
        }
        if ( first > max_first ) {
            first = max_first;
        }
        if ( first < 0 ) {
            first = 0;
        }
    }
    tx_free( &text );
    ui_hint( before );
    in_help = 0;
}
