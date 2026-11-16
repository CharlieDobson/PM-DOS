/*
 * UI.C - the menu bar, its menus, dialog boxes and message boxes.
 *
 * EDIT'S MACHINERY BY WAY OF DOSSHELL'S AND QBASIC'S, with the menus
 * taken out of it: the tables are the application's (menu_set).
 * Nothing here remembers what it covered: a dialog is drawn over the
 * whole screen as the application draws it (app_draw), with any
 * dialogs under it drawn first - ui_push keeps that stack - and the
 * screen layer sends only what changed.  On a graphics screen the
 * application's panes are under all of it, and the screen layer has
 * them paint what a closed menu uncovers.
 *
 * A DIALOG IS A TABLE OF CONTROLS: labels, entry fields, check boxes,
 * option buttons, lists, drop-down lists and buttons, each at a row
 * and column inside the box.  Tab and Shift+Tab walk them; Alt and a
 * letter (or just the letter, when the keys are not somewhere that
 * takes letters) jumps to the one whose label has that access key;
 * Enter presses the default button; Esc is Cancel.  The mouse presses
 * what it lands on - a button when it is let go over it - and a double
 * click in a list is Enter.  Two hooks let a dialog react: "changed"
 * when a control's value moves, and "activate" for Enter on a list and
 * for a button - which returns the dialog's answer, or 0 to stay open.
 *
 * A DROP-DOWN LIST is one line until it is opened - Alt+Down, F4, the
 * space bar, or a click - and then its entries are a box drawn over
 * whatever is below it (or above, when there is no room below), the
 * last thing drawn, so that it lies on top of the controls it covers.
 * While it is open every key and click is the list's.  Closed, Up and
 * Down change the entry where it stands, as a list would.
 *
 * TWO LOOKS, ONE SET OF TABLES.  The words and the places are the
 * same; what differs is in "pal": on a text screen a field is in
 * brackets with dots where nothing is typed and a button is in angle
 * brackets, as Word for DOS drew them, and boxes cast shadows; on a
 * graphics screen a field is a white well, a button stands out of the
 * dialog with a lit edge and a shaded one, a title is a navy bar, and
 * nothing casts a shadow.
 */
#include "rt.h"
#include "ui.h"

PALETTE pal;
const char *ui_name = "PMWRITE";
void (*ui_help)( const char *topic );

static int look_asked = LOOK_AUTO;

/* ------------------------------------------------------------------ */
/* the looks                                                           */
/* ------------------------------------------------------------------ */

void ui_look( int look )
{
    look_asked = look;
    if ( look == LOOK_AUTO ) {
        look = gfx_on ? LOOK_GRAPHICS : LOOK_TEXT;
    }
    mem_set( &pal, 0, sizeof( pal ) );
    /* what a pane is painted with is the same whatever the cells wear */
    pal.px_face = 7;
    pal.px_light = 15;
    pal.px_shade = 8;
    pal.px_frame = 0;
    pal.px_desk = 8;
    pal.px_page = 15;
    pal.px_ink = 0;
    pal.px_select = 1;
    pal.px_select_ink = 15;
    switch ( look ) {
    case LOOK_GRAPHICS:                 /* after Word 6 */
        pal.desk = 0x87;
        pal.title = 0x1F;
        pal.menu = 0x70;
        pal.menu_hot = 0x70;
        pal.menu_sel = 0x1F;
        pal.menu_sel_hot = 0x1F;
        pal.menu_off = 0x78;
        pal.status = 0x70;
        pal.status_hot = 0x71;
        pal.dlg = 0x70;
        pal.dlg_hot = 0x70;
        pal.dlg_off = 0x78;
        pal.field = 0xF0;
        pal.field_sel = 0x1F;
        pal.list = 0xF0;
        pal.select = 0x1F;
        pal.button = 0x70;
        pal.button_hot = 0x70;
        pal.scroll = 0x78;
        pal.thumb = 0x70;
        pal.shadow = 0x08;
        pal.text = 0xF0;
        pal.text_sel = 0x1F;
        pal.ribbon = 0x70;
        pal.ribbon_hot = 0x71;
        pal.shadows = 0;
        pal.brackets = 0;
        pal.fill = ' ';
        break;
    case LOOK_MONO:
        pal.desk = 0x07;
        pal.title = 0x70;
        pal.menu = 0x70;
        pal.menu_hot = 0x7F;
        pal.menu_sel = 0x07;
        pal.menu_sel_hot = 0x0F;
        pal.menu_off = 0x70;
        pal.status = 0x70;
        pal.status_hot = 0x7F;
        pal.dlg = 0x70;
        pal.dlg_hot = 0x7F;
        pal.dlg_off = 0x70;
        pal.field = 0x70;
        pal.field_sel = 0x07;
        pal.list = 0x70;
        pal.select = 0x07;
        pal.button = 0x70;
        pal.button_hot = 0x7F;
        pal.scroll = 0x70;
        pal.thumb = 0x07;
        pal.shadow = 0x07;
        pal.text = 0x07;
        pal.text_sel = 0x70;
        pal.ribbon = 0x07;
        pal.ribbon_hot = 0x0F;
        pal.shadows = 1;
        pal.brackets = 1;
        pal.fill = 0xFA;
        break;
    default:                            /* after Word 5.5 and 6.0 for DOS */
        pal.desk = 0x17;
        pal.title = 0x17;
        pal.menu = 0x70;
        pal.menu_hot = 0x7F;
        pal.menu_sel = 0x07;
        pal.menu_sel_hot = 0x0F;
        pal.menu_off = 0x78;
        pal.status = 0x30;
        pal.status_hot = 0x3F;
        pal.dlg = 0x70;
        pal.dlg_hot = 0x7F;
        pal.dlg_off = 0x78;
        pal.field = 0x70;
        pal.field_sel = 0x07;
        pal.list = 0x70;
        pal.select = 0x07;
        pal.button = 0x70;
        pal.button_hot = 0x7F;
        pal.scroll = 0x70;
        pal.thumb = 0x07;
        pal.shadow = 0x08;
        pal.text = 0x17;
        pal.text_sel = 0x71;
        pal.ribbon = 0x17;
        pal.ribbon_hot = 0x1F;
        pal.shadows = 1;
        pal.brackets = 1;
        pal.fill = 0xFA;
        break;
    }
    if ( gfx_on ) {
        scr_desk( pal.status >> 4 );    /* the spare lines carry on from the status bar */
    }
}

int ui_set_mode( int index )
{
    const DISPMODE *mode = disp_mode_info( index );
    char name[40], text[200];
    const char *why;
    int ok;

    if ( mode == NULL ) {
        return 0;
    }
    sfmt( name, sizeof( name ), "%s", mode->name );
    ok = disp_set_mode( index );
    ui_look( look_asked );
    if ( !ok ) {
        switch ( disp_error() ) {
        case DE_NOMEM:  why = "There is not enough memory for"; break;
        case DE_NOFONT: why = "The video card's 8x16 font could not be read for"; break;
        default:        why = "The video card would not show"; break;
        }
        mode = disp_mode_info( disp_mode_now() );
        sfmt( text, sizeof( text ), "%s %s.\nThe display is now %s.", why, name,
              mode ? mode->name : "text" );
        msg_box( text, MB_OK );
    }
    return ok;
}

int key_service( int key )
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
    }
    return 0;
}

/* "&File" with its access key shown or not.  A graphics look gives the
   key no colour of its own and underlines it. */
static void put_hot( int row, int col, const char *text, u8 attr, u8 hot, int show )
{
    const char *amp;

    scr_hot( row, col, text, attr, show ? hot : attr );
    if ( show && gfx_on && hot == attr ) {
        amp = str_chr( text, '&' );
        if ( amp != NULL && amp[1] ) {
            scr_flag( row, col + (int)(amp - text), 1, CELLF_UNDER );
        }
    }
}

/* the left button did this (a drag: it is among those held) */
static int left_mouse( void )
{
    if ( mouse.kind == ME_WHEEL || mouse.kind == ME_MOVE ) {
        return 0;
    }
    return (mouse.button & MBTN_LEFT) != 0;
}

/* ------------------------------------------------------------------ */
/* scroll bars                                                         */
/*                                                                     */
/* An arrow at each end and a track between them, with a box in the    */
/* track when there is more than fits - one cell, placed by how far    */
/* down the list the first line showing is.                            */
/* ------------------------------------------------------------------ */

static int thumb_row( int top, int bottom, int first, int shown_n, int total )
{
    int track = bottom - top - 1, range = total - shown_n;

    if ( track < 1 || range < 1 ) {
        return -1;
    }
    if ( first > range ) {
        first = range;
    }
    if ( first < 0 ) {
        first = 0;
    }
    return top + 1 + (track > 1 ? first * (track - 1) / range : 0);
}

void sbar_draw( int top, int bottom, int col, int first, int shown_n, int total )
{
    int row, thumb = thumb_row( top, bottom, first, shown_n, total );

    if ( bottom - top < 1 ) {
        return;
    }
    for ( row = top + 1; row < bottom; row++ ) {
        scr_ch( row, col, gfx_on ? ' ' : 0xB0, pal.scroll );
    }
    scr_ch( top, col, 0x18, pal.thumb );
    scr_ch( bottom, col, 0x19, pal.thumb );
    if ( thumb >= 0 ) {
        scr_ch( thumb, col, gfx_on ? ' ' : 0xDB, gfx_on ? pal.thumb : pal.scroll );
    }
    if ( gfx_on ) {
        for ( row = top; row <= bottom; row++ ) {
            scr_flag( row, col, 1, CELLF_LEFT | CELLF_FRAME );
        }
        scr_flag( top, col, 1, CELLF_TOP | CELLF_BOTTOM | CELLF_RIGHT | CELLF_RAISE );
        scr_flag( bottom, col, 1, CELLF_TOP | CELLF_BOTTOM | CELLF_RIGHT | CELLF_RAISE );
        if ( thumb >= 0 ) {
            scr_flag( thumb, col, 1, CELLF_TOP | CELLF_BOTTOM | CELLF_RIGHT | CELLF_RAISE );
        }
    }
}

int sbar_hit( int top, int bottom, int first, int shown_n, int total, int row )
{
    int thumb = thumb_row( top, bottom, first, shown_n, total );

    if ( row == top ) {
        return SB_UP;
    }
    if ( row == bottom ) {
        return SB_DOWN;
    }
    if ( row < top || row > bottom || thumb < 0 ) {
        return SB_NONE;
    }
    if ( row == thumb ) {
        return SB_THUMB;
    }
    return row < thumb ? SB_PGUP : SB_PGDN;
}

/* the box dragged to "row": the first line that should show */
int sbar_drag( int top, int bottom, int shown_n, int total, int row )
{
    int track = bottom - top - 1, range = total - shown_n;

    if ( track < 2 || range < 1 ) {
        return 0;
    }
    row -= top + 1;
    if ( row < 0 ) {
        row = 0;
    }
    if ( row > track - 1 ) {
        row = track - 1;
    }
    return (row * range + (track - 1) / 2) / (track - 1);
}

/* ------------------------------------------------------------------ */
/* the menus                                                           */
/* ------------------------------------------------------------------ */

static const MENU *menus;
static int nmenus;
static int menu_row;

void menu_set( const MENU *table, int count, int row )
{
    menus = table;
    nmenus = count;
    menu_row = row;
}

static int menu_col( int menu )
{
    int col = 2, index;

    for ( index = 0; index < menu; index++ ) {
        col += scr_hotlen( menus[index].title ) + 2;
    }
    return col;
}

void menu_draw_bar( int selected, int show_hot )
{
    int index, col;

    scr_fill( menu_row, 0, scr_cols, ' ', pal.menu );
    for ( index = 0; index < nmenus; index++ ) {
        col = menu_col( index );
        if ( index == selected ) {
            scr_fill( menu_row, col - 1, scr_hotlen( menus[index].title ) + 2, ' ', pal.menu_sel );
            put_hot( menu_row, col, menus[index].title, pal.menu_sel, pal.menu_sel_hot, show_hot );
        } else {
            put_hot( menu_row, col, menus[index].title, pal.menu, pal.menu_hot, show_hot );
        }
    }
}

/* text, a gap, the keys - and two cells at the left for a mark */
static int pull_width( const MENUITEM *items, int count )
{
    int index, width = 0, text, keys;

    for ( index = 0; index < count; index++ ) {
        if ( items[index].text ) {
            text = scr_hotlen( items[index].text );
            keys = items[index].keys ? (int)str_len( items[index].keys ) : 0;
            if ( text + (keys ? keys + 3 : 0) > width ) {
                width = text + (keys ? keys + 3 : 0);
            }
        }
    }
    return width + 6;
}

/* where a menu's pull-down goes: kept on the screen, shadow and all */
static int menu_left( int menu, int width )
{
    int left = menu_col( menu ) - 2;

    if ( left + width + 2 > scr_cols ) {
        left = scr_cols - width - 2;
    }
    return left < 0 ? 0 : left;
}

/* the menu whose title is at "col" on the bar, or -1 */
static int menu_at( int col )
{
    int index, start;

    for ( index = 0; index < nmenus; index++ ) {
        start = menu_col( index );
        if ( col >= start - 1 && col <= start + scr_hotlen( menus[index].title ) ) {
            return index;
        }
    }
    return -1;
}

/* the line of an open menu at a cell, or -1 - a line across is not one */
static int pull_item_at( const MENUITEM *items, int count, int top, int left, int row, int col )
{
    int width = pull_width( items, count ), first = top + 1;

    if ( row < first || row >= first + count || col <= left || col >= left + width - 1 ) {
        return -1;
    }
    return items[row - first].text ? row - first : -1;
}

static void pull_draw( const MENUITEM *items, int count, int top, int left, int sel )
{
    int width = pull_width( items, count ), index, row;
    u8 attr, hot;

    scr_box( top, left, top + count + 1, left + width - 1, pal.menu );
    for ( index = 0; index < count; index++ ) {
        row = top + 1 + index;
        if ( items[index].text == NULL ) {
            scr_ch( row, left, 0xC3, pal.menu );
            scr_fill( row, left + 1, width - 2, 0xC4, pal.menu );
            scr_ch( row, left + width - 1, 0xB4, pal.menu );
            continue;
        }
        if ( index == sel ) {
            attr = pal.menu_sel;
            hot = pal.menu_sel_hot;
        } else if ( !app_can( items[index].cmd ) ) {
            attr = pal.menu_off;
            hot = pal.menu_off;
        } else {
            attr = pal.menu;
            hot = pal.menu_hot;
        }
        scr_fill( row, left + 1, width - 2, ' ', attr );
        if ( app_checked( items[index].cmd ) ) {
            scr_ch( row, left + 2, 0xFB, attr );
        }
        put_hot( row, left + 4, items[index].text, attr, hot, attr != pal.menu_off );
        if ( items[index].keys && items[index].keys[0] ) {
            scr_put( row, left + width - 2 - (int)str_len( items[index].keys ),
                     items[index].keys, attr );
        }
    }
    if ( pal.shadows ) {
        scr_shadow( top, left, top + count + 1, left + width - 1, pal.shadow );
    }
}

static int menu_by_letter( int letter )
{
    int index;

    for ( index = 0; index < nmenus; index++ ) {
        if ( scr_hotkey( menus[index].title ) == letter ) {
            return index;
        }
    }
    return -1;
}

/* the next line that is not a line across, "step" from "from" */
static int next_item( const MENUITEM *items, int count, int from, int step )
{
    int index = from, tries;

    if ( count <= 0 ) {
        return -1;                      /* a menu with nothing in it lights nothing */
    }
    for ( tries = 0; tries < count; tries++ ) {
        index = (index + step + count) % count;
        if ( items[index].text ) {
            return index;
        }
    }
    return items[from < 0 || from >= count ? 0 : from].text ? from : -1;
}

/* the line with that access key, or -1 */
static int item_by_letter( const MENUITEM *items, int count, int letter )
{
    int index;

    for ( index = 0; index < count; index++ ) {
        if ( items[index].text && scr_hotkey( items[index].text ) == letter ) {
            return index;
        }
    }
    return -1;
}

int menu_wants( int key )
{
    int letter;

    if ( menus == NULL || nmenus == 0 ) {
        return 0;
    }
    if ( key == K_F10 || key == K_ALTTAP ) {
        return 1;
    }
    if ( key == K_MOUSE ) {
        return (mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE) && mouse.button == MBTN_LEFT
               && mouse.row == menu_row && menu_at( mouse.col ) >= 0;
    }
    letter = key_alt_letter( key );
    return letter && menu_by_letter( letter ) >= 0;
}

/*
 * F10 or a tap of Alt: the bar, with the first title lit.  Alt+letter:
 * that menu, open.  K_MOUSE, pressed on the bar: the menu under the
 * pointer, open, following the pointer while the button is held - let
 * go on a line and that is the command; let go on the title and the
 * menu stays open for a click or a key.  Returns the command, or 0.
 *
 * "sel" is -1 while the mouse has opened a menu without pointing at a
 * line: nothing is lit until it does, or a key moves.
 */
int menu_run( int key )
{
    const MENUITEM *items;
    int menu = 0, open = 0, sel = 0, count, letter, index, tracking = 0, top = menu_row + 1;

    if ( menus == NULL || nmenus == 0 ) {
        return 0;
    }
    letter = key_alt_letter( key );
    if ( letter ) {
        menu = menu_by_letter( letter );
        if ( menu < 0 ) {
            return 0;
        }
        open = 1;
    }
    items = menus[menu].items;
    count = menus[menu].count;
    sel = next_item( items, count, count - 1, 1 );
    if ( key == K_MOUSE ) {
        menu = menu_at( mouse.col );
        if ( menu < 0 ) {
            return 0;
        }
        open = 1;
        sel = -1;
        tracking = 1;
    }
    for ( ;; ) {
        items = menus[menu].items;
        count = menus[menu].count;
        app_status( !open ? "Choose a menu: Enter opens it, Esc leaves"
                    : sel >= 0 ? items[sel].hint : "Choose a command" );
        app_draw();
        menu_draw_bar( menu, 1 );
        if ( open ) {
            pull_draw( items, count, top, menu_left( menu, pull_width( items, count ) ), sel );
        }
        scr_cursor( 0, 0, CUR_HIDE );
        caret_off();
        scr_flush();
        key = key_get();
        if ( key_service( key ) ) {
            continue;
        }
        if ( key == K_MOUSE ) {
            if ( !left_mouse() ) {
                continue;
            }
            index = mouse.row == menu_row ? menu_at( mouse.col ) : -1;
            if ( index >= 0 ) {                 /* on a title */
                if ( mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE ) {
                    if ( open && index == menu && !tracking ) {
                        app_status( NULL );     /* its own title again: shut */
                        return 0;
                    }
                    menu = index;
                    open = 1;
                    sel = -1;
                    tracking = 1;
                } else if ( mouse.kind == ME_DRAG && tracking ) {
                    if ( index != menu ) {
                        menu = index;
                        sel = -1;
                    }
                } else if ( mouse.kind == ME_UP ) {
                    tracking = 0;               /* it stays open */
                }
                continue;
            }
            index = open ? pull_item_at( items, count, top,
                                         menu_left( menu, pull_width( items, count ) ),
                                         mouse.row, mouse.col ) : -1;
            if ( index >= 0 ) {                 /* on a line */
                sel = index;
                if ( mouse.kind == ME_UP ) {
                    tracking = 0;
                    if ( app_can( items[sel].cmd ) ) {
                        app_status( NULL );
                        return items[sel].cmd;
                    }
                } else if ( mouse.kind != ME_DRAG ) {
                    tracking = 1;
                }
                continue;
            }
            /* anywhere else: a press there, or letting go there after a
               drag, is leaving the menus */
            if ( mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE
                 || (mouse.kind == ME_UP && tracking) ) {
                app_status( NULL );
                return 0;
            }
            if ( mouse.kind == ME_DRAG && tracking ) {
                sel = -1;
            }
            continue;
        }
        switch ( key ) {
        case K_ESC:
        case K_ALTTAP:
        case K_F10:
            app_status( NULL );
            return 0;
        case K_LEFT:
            menu = (menu + nmenus - 1) % nmenus;
            sel = next_item( menus[menu].items, menus[menu].count, menus[menu].count - 1, 1 );
            continue;
        case K_RIGHT:
            menu = (menu + 1) % nmenus;
            sel = next_item( menus[menu].items, menus[menu].count, menus[menu].count - 1, 1 );
            continue;
        case K_UP:
            if ( open ) {
                sel = next_item( items, count, sel >= 0 ? sel : 0, -1 );
            }
            open = 1;
            continue;
        case K_DOWN:
            if ( open ) {
                sel = next_item( items, count, sel >= 0 ? sel : count - 1, 1 );
            }
            open = 1;
            continue;
        case K_ENTER:
            if ( !open ) {
                open = 1;
                continue;
            }
            if ( sel < 0 ) {
                sel = next_item( items, count, count - 1, 1 );
                continue;
            }
            if ( app_can( items[sel].cmd ) ) {
                app_status( NULL );
                return items[sel].cmd;
            }
            continue;
        }
        letter = key_alt_letter( key );
        if ( letter == 0 && key >= ' ' && key < 0x100 ) {
            letter = ch_upper( key );
        }
        if ( letter == 0 ) {
            continue;
        }
        if ( !open || key_alt_letter( key ) ) {
            index = menu_by_letter( letter );
            if ( index >= 0 ) {
                menu = index;
                open = 1;
                sel = next_item( menus[menu].items, menus[menu].count, menus[menu].count - 1, 1 );
            }
            continue;
        }
        index = item_by_letter( items, count, letter );
        if ( index >= 0 ) {
            if ( app_can( items[index].cmd ) ) {
                app_status( NULL );
                return items[index].cmd;
            }
            sel = index;
        }
    }
}

/* a menu by itself, where the right button was pressed */
int menu_popup( const MENUITEM *items, int count, int row, int col )
{
    int width = pull_width( items, count ), sel = -1, key, index, letter;

    if ( count <= 0 ) {
        return 0;
    }
    if ( row + count + 2 + pal.shadows > scr_rows ) {
        row = scr_rows - count - 2 - pal.shadows;
    }
    if ( col + width + 2 * pal.shadows > scr_cols ) {
        col = scr_cols - width - 2 * pal.shadows;
    }
    if ( row < 0 ) {
        row = 0;
    }
    if ( col < 0 ) {
        col = 0;
    }
    for ( ;; ) {
        app_status( sel >= 0 ? items[sel].hint : "Choose a command" );
        app_draw();
        pull_draw( items, count, row, col, sel );
        scr_cursor( 0, 0, CUR_HIDE );
        caret_off();
        scr_flush();
        key = key_get();
        if ( key_service( key ) ) {
            continue;
        }
        if ( key == K_MOUSE ) {
            if ( mouse.kind == ME_WHEEL || mouse.kind == ME_MOVE ) {
                continue;
            }
            index = pull_item_at( items, count, row, col, mouse.row, mouse.col );
            if ( index >= 0 ) {
                sel = index;
                if ( mouse.kind == ME_UP && app_can( items[sel].cmd ) ) {
                    app_status( NULL );
                    return items[sel].cmd;
                }
                continue;
            }
            if ( mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE ) {
                app_status( NULL );
                return 0;
            }
            if ( mouse.kind == ME_DRAG ) {
                sel = -1;
            }
            continue;
        }
        switch ( key ) {
        case K_ESC:
        case K_ALTTAP:
        case K_F10:
            app_status( NULL );
            return 0;
        case K_UP:
            sel = next_item( items, count, sel >= 0 ? sel : 0, -1 );
            continue;
        case K_DOWN:
            sel = next_item( items, count, sel >= 0 ? sel : count - 1, 1 );
            continue;
        case K_ENTER:
            if ( sel >= 0 && app_can( items[sel].cmd ) ) {
                app_status( NULL );
                return items[sel].cmd;
            }
            continue;
        }
        letter = key >= ' ' && key < 0x100 ? ch_upper( key ) : key_alt_letter( key );
        index = letter ? item_by_letter( items, count, letter ) : -1;
        if ( index >= 0 ) {
            if ( app_can( items[index].cmd ) ) {
                app_status( NULL );
                return items[index].cmd;
            }
            sel = index;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the dialog stack                                                    */
/* ------------------------------------------------------------------ */

#define MAX_DIALOGS     8

static DLG *stack[MAX_DIALOGS];
static int  depth;

/* A dialog too deep to be kept is still counted, so that its ui_pop
   takes itself off and not the one under it; it is drawn by its own
   dlg_run all the same, last. */
void ui_push( DLG *dlg )
{
    if ( depth < MAX_DIALOGS ) {
        stack[depth] = dlg;
    }
    depth++;
}

void ui_pop( void )
{
    if ( depth ) {
        depth--;
    }
}

void ui_redraw( void )
{
    int index;

    app_draw();
    scr_cursor( 0, 0, CUR_HIDE );
    caret_off();                        /* the keys are not in the document now */
    for ( index = 0; index < depth && index < MAX_DIALOGS; index++ ) {
        dlg_draw( stack[index] );
    }
}

/* ------------------------------------------------------------------ */
/* drawing a dialog                                                    */
/* ------------------------------------------------------------------ */

static int focusable( const CTL *ctl )
{
    if ( ctl->disabled ) {
        return 0;
    }
    return ctl->type == CT_EDIT || ctl->type == CT_CHECK || ctl->type == CT_RADIO
           || ctl->type == CT_LIST || ctl->type == CT_DROP || ctl->type == CT_BUTTON;
}

static const char *entry( DLG *dlg, CTL *ctl, int index )
{
    const char *text = NULL;

    if ( index < 0 || index >= ctl->count ) {
        return "";
    }
    if ( ctl->strings != NULL ) {
        text = ctl->strings[index];
    } else if ( ctl->item != NULL ) {
        text = ctl->item( dlg, index );
    }
    return text ? text : "";
}

/* a box with its title, and its shadow where boxes cast one */
void dlg_frame( int top, int left, int rows, int cols, const char *title )
{
    int right = left + cols - 1, bottom = top + rows - 1, len, row;

    if ( pal.brackets ) {
        scr_box( top, left, bottom, right, pal.dlg );
        if ( title && *title ) {
            len = (int)str_len( title );
            scr_ch( top, left + (cols - len) / 2 - 1, ' ', pal.dlg );
            scr_put( top, left + (cols - len) / 2, title, pal.dlg );
            scr_ch( top, left + (cols - len) / 2 + len, ' ', pal.dlg );
        }
    } else {
        /* a slab with a navy bar across its top and a black line round it */
        for ( row = top; row <= bottom; row++ ) {
            scr_fill( row, left, cols, ' ', row == top ? pal.title : pal.dlg );
            scr_flag( row, left, 1, CELLF_LEFT | CELLF_FRAME );
            scr_flag( row, right, 1, CELLF_RIGHT | CELLF_FRAME );
        }
        if ( title && *title ) {
            len = (int)str_len( title );
            scr_put( top, left + (cols - len) / 2, title, pal.title );
        }
        scr_flag( top, left, cols, CELLF_TOP | CELLF_FRAME );
        scr_flag( bottom, left, cols, CELLF_BOTTOM | CELLF_FRAME );
    }
    if ( pal.shadows ) {
        scr_shadow( top, left, bottom, right, pal.shadow );
    }
}

/* the cells of a field: brackets round it on a text screen, a well on
   a graphics one */
static void field_frame( int row, int col, int width )
{
    if ( pal.brackets ) {
        scr_ch( row, col - 1, '[', pal.dlg );
        scr_ch( row, col + width, ']', pal.dlg );
    } else {
        scr_flag( row, col, width, CELLF_TOP | CELLF_BOTTOM | CELLF_SINK );
        scr_flag( row, col, 1, CELLF_LEFT | CELLF_SINK );
        scr_flag( row, col + width - 1, 1, CELLF_RIGHT | CELLF_SINK );
    }
}

static void draw_edit( DLG *dlg, CTL *ctl, int focus )
{
    int row = dlg->top + ctl->row, col = dlg->left + ctl->col;
    int len = (int)str_len( ctl->buf ), shown, pos;
    u8 attr;

    if ( ctl->caret > len ) {
        ctl->caret = len;
    }
    if ( ctl->caret < ctl->scroll ) {
        ctl->scroll = ctl->caret;
    }
    if ( ctl->caret >= ctl->scroll + ctl->width ) {
        ctl->scroll = ctl->caret - ctl->width + 1;
    }
    scr_fill( row, col, ctl->width, pal.fill, ctl->disabled ? pal.dlg_off : pal.field );
    if ( len > ctl->scroll ) {
        shown = len - ctl->scroll < ctl->width ? len - ctl->scroll : ctl->width;
        attr = ctl->disabled ? pal.dlg_off : focus && ctl->fresh ? pal.field_sel : pal.field;
        if ( ctl->hidden ) {
            for ( pos = 0; pos < shown; pos++ ) {
                scr_ch( row, col + pos, '*', attr );
            }
        } else {
            scr_putn( row, col, ctl->buf + ctl->scroll, shown, attr );
        }
    }
    field_frame( row, col, ctl->width );
    if ( focus ) {
        scr_cursor( row, col + ctl->caret - ctl->scroll, CUR_LINE );
    }
}

/* a drop-down list, closed: the chosen entry and an arrow */
static void draw_drop( DLG *dlg, CTL *ctl, int focus )
{
    int row = dlg->top + ctl->row, col = dlg->left + ctl->col, width = ctl->width - 1;
    const char *text = entry( dlg, ctl, ctl->sel );
    int len = (int)str_len( text );
    u8 attr = ctl->disabled ? pal.dlg_off : focus ? pal.field_sel : pal.field;

    if ( len > width ) {
        len = width;
    }
    scr_fill( row, col, width, ' ', attr );
    scr_putn( row, col, text, len, attr );
    if ( pal.brackets ) {
        scr_fill( row, col + len, width - len, pal.fill, ctl->disabled ? pal.dlg_off : pal.field );
        scr_ch( row, col + width, 0x19, ctl->disabled ? pal.dlg_off : pal.field );
        scr_ch( row, col - 1, '[', pal.dlg );
        scr_ch( row, col + ctl->width, ']', pal.dlg );
    } else {
        scr_ch( row, col + width, 0x19, ctl->disabled ? pal.dlg_off : pal.button );
        field_frame( row, col, width );
        scr_flag( row, col + width, 1, CELLF_TOP | CELLF_BOTTOM | CELLF_LEFT | CELLF_RIGHT | CELLF_RAISE );
    }
    if ( focus ) {
        scr_cursor( row, col, pal.brackets ? CUR_LINE : CUR_HIDE );
    }
}

/* where an open drop-down's list goes: under its line, or over it when
   the screen ends too soon */
static void drop_place( DLG *dlg, CTL *ctl, int *top, int *left, int *rows, int *width )
{
    int most = ctl->drop_rows > 0 ? ctl->drop_rows : 8, line = dlg->top + ctl->row;

    *rows = ctl->count < most ? ctl->count : most;
    if ( *rows < 1 ) {
        *rows = 1;
    }
    *left = dlg->left + ctl->col - 1;
    *width = ctl->width + 2;
    *top = line + 1;
    if ( *top + *rows + 2 > scr_rows ) {
        if ( line - (*rows + 2) >= 0 ) {
            *top = line - (*rows + 2);
        } else {
            *rows = scr_rows - *top - 2;        /* neither way: what fits below */
            if ( *rows < 1 ) {
                *rows = 1;
            }
        }
    }
}

static void draw_drop_list( DLG *dlg, CTL *ctl )
{
    int top, left, rows, width, index, item, len;
    const char *text;

    drop_place( dlg, ctl, &top, &left, &rows, &width );
    if ( dlg->drop_pick < dlg->drop_top ) {
        dlg->drop_top = dlg->drop_pick;
    }
    if ( dlg->drop_pick >= dlg->drop_top + rows ) {
        dlg->drop_top = dlg->drop_pick - rows + 1;
    }
    if ( dlg->drop_top < 0 ) {
        dlg->drop_top = 0;
    }
    scr_box( top, left, top + rows + 1, left + width - 1, pal.list );
    for ( index = 0; index < rows; index++ ) {
        item = dlg->drop_top + index;
        if ( item >= ctl->count ) {
            break;
        }
        text = entry( dlg, ctl, item );
        len = (int)str_len( text );
        if ( len > width - 3 ) {
            len = width - 3;
        }
        if ( item == dlg->drop_pick ) {
            scr_fill( top + 1 + index, left + 1, width - 2, ' ', pal.select );
        }
        scr_putn( top + 1 + index, left + 2, text, len, item == dlg->drop_pick ? pal.select : pal.list );
    }
    if ( ctl->count > rows ) {
        sbar_draw( top + 1, top + rows, left + width - 1, dlg->drop_top, rows, ctl->count );
    }
    if ( pal.shadows ) {
        scr_shadow( top, left, top + rows + 1, left + width - 1, pal.shadow );
    }
    scr_cursor( 0, 0, CUR_HIDE );
}

static void draw_list( DLG *dlg, CTL *ctl, int focus )
{
    int top = dlg->top + ctl->row, left = dlg->left + ctl->col;
    int rows = ctl->height - 2, index, item, len;
    const char *text;
    u8 attr = ctl->disabled ? pal.dlg_off : pal.list;

    scr_box( top, left, top + ctl->height - 1, left + ctl->width - 1, attr );
    if ( ctl->sel >= ctl->count ) {
        ctl->sel = ctl->count ? ctl->count - 1 : 0;
    }
    if ( ctl->sel < 0 ) {
        ctl->sel = 0;
    }
    if ( ctl->sel < ctl->top ) {
        ctl->top = ctl->sel;
    }
    if ( ctl->sel >= ctl->top + rows ) {
        ctl->top = ctl->sel - rows + 1;
    }
    if ( ctl->top < 0 ) {
        ctl->top = 0;
    }
    for ( index = 0; index < rows; index++ ) {
        item = ctl->top + index;
        if ( item >= ctl->count ) {
            break;
        }
        text = entry( dlg, ctl, item );
        len = (int)str_len( text );
        if ( len > ctl->width - 3 ) {
            len = ctl->width - 3;
        }
        if ( item == ctl->sel && !ctl->disabled ) {
            scr_fill( top + 1 + index, left + 1, ctl->width - 2, ' ', pal.select );
            scr_putn( top + 1 + index, left + 2, text, len, pal.select );
            if ( focus ) {
                scr_cursor( top + 1 + index, left + 2, pal.brackets ? CUR_LINE : CUR_HIDE );
            }
        } else {
            scr_putn( top + 1 + index, left + 2, text, len, attr );
        }
    }
    if ( ctl->count > rows ) {
        sbar_draw( top + 1, top + rows, left + ctl->width - 1, ctl->top, rows, ctl->count );
    }
    if ( ctl->text ) {
        scr_ch( top, left + 1, ' ', attr );
        put_hot( top, left + 2, ctl->text, attr, ctl->disabled ? attr : (u8)((attr & 0xF0) | (pal.dlg_hot & 0x0F)), 1 );
        scr_ch( top, left + 2 + scr_hotlen( ctl->text ), ' ', attr );
    }
}

/* A button.  "pressed_by_enter" is the one Enter would press now: on
   a text screen its angle brackets stand out, on a graphics one it has
   a black line round it where the others have a lit edge. */
static void draw_button( DLG *dlg, CTL *ctl, int focus, int pressed_by_enter )
{
    int row = dlg->top + ctl->row, col = dlg->left + ctl->col;
    int len = scr_hotlen( ctl->text );
    u8 attr = ctl->disabled ? pal.dlg_off : pal.button;
    u8 edge = pressed_by_enter && !ctl->disabled ? pal.button_hot : attr;

    scr_fill( row, col, len + 4, ' ', attr );
    put_hot( row, col + 2, ctl->text, attr, ctl->disabled ? attr : pal.dlg_hot, 1 );
    if ( pal.brackets ) {
        scr_ch( row, col, '<', edge );
        scr_ch( row, col + 3 + len, '>', edge );
    } else {
        scr_flag( row, col, len + 4, CELLF_TOP | CELLF_BOTTOM | (pressed_by_enter ? CELLF_FRAME : CELLF_RAISE) );
        scr_flag( row, col, 1, CELLF_LEFT | (pressed_by_enter ? CELLF_FRAME : CELLF_RAISE) );
        scr_flag( row, col + 3 + len, 1, CELLF_RIGHT | (pressed_by_enter ? CELLF_FRAME : CELLF_RAISE) );
    }
    if ( focus ) {
        scr_cursor( row, col + 2, CUR_LINE );
    }
}

/* a check box or an option button: three characters and a blank */
static void draw_mark( DLG *dlg, CTL *ctl, int on, int radio, int focus )
{
    int row = dlg->top + ctl->row, col = dlg->left + ctl->col;
    u8 attr = ctl->disabled ? pal.dlg_off : pal.dlg;

    scr_put( row, col, radio ? (on ? "(\x07)" : "( )") : (on ? "[X]" : "[ ]"), attr );
    put_hot( row, col + 4, ctl->text, attr, ctl->disabled ? attr : pal.dlg_hot, 1 );
    if ( focus ) {
        scr_cursor( row, col + 1, CUR_LINE );
    }
}

/* the button Enter would press: the one the keys are on, else the default */
static int default_button( DLG *dlg )
{
    int index;

    if ( dlg->ctl[dlg->focus].type == CT_BUTTON ) {
        return dlg->focus;
    }
    for ( index = 0; index < dlg->nctl; index++ ) {
        if ( dlg->ctl[index].type == CT_BUTTON && dlg->ctl[index].is_default
             && !dlg->ctl[index].disabled ) {
            return index;
        }
    }
    return -1;
}

void dlg_draw( DLG *dlg )
{
    int index, row, col, len, focus, enter = default_button( dlg ), off;
    CTL *ctl;
    const char *text, *end;

    dlg_frame( dlg->top, dlg->left, dlg->rows, dlg->cols, dlg->title );
    if ( dlg->sep ) {
        row = dlg->top + dlg->sep;
        if ( pal.brackets ) {
            scr_ch( row, dlg->left, 0xC3, pal.dlg );
            scr_fill( row, dlg->left + 1, dlg->cols - 2, 0xC4, pal.dlg );
            scr_ch( row, dlg->left + dlg->cols - 1, 0xB4, pal.dlg );
        } else {
            scr_fill( row, dlg->left + 1, dlg->cols - 2, 0xC4, pal.dlg_off );
        }
    }
    for ( index = 0; index < dlg->nctl; index++ ) {
        ctl = &dlg->ctl[index];
        row = dlg->top + ctl->row;
        col = dlg->left + ctl->col;
        focus = index == dlg->focus && !dlg->drop;
        switch ( ctl->type ) {
        case CT_LABEL:
            /* a label is as grey as what it names */
            off = ctl->disabled || (index + 1 < dlg->nctl && dlg->ctl[index + 1].disabled);
            put_hot( row, col, ctl->text, off ? pal.dlg_off : pal.dlg, off ? pal.dlg_off : pal.dlg_hot, 1 );
            break;
        case CT_TEXT:                           /* lines, as they are */
            text = ctl->text;
            while ( *text ) {
                end = str_chr( text, '\n' );
                len = end ? (int)(end - text) : (int)str_len( text );
                scr_putn( row++, ctl->width ? col + (ctl->width - len) / 2 : col, text, len,
                          ctl->disabled ? pal.dlg_off : pal.dlg );
                text += len + (end ? 1 : 0);
            }
            break;
        case CT_FRAME:                          /* a box round a group, titled */
            scr_box( row, col, row + ctl->height - 1, col + ctl->width - 1, pal.dlg );
            if ( ctl->text && *ctl->text ) {
                scr_ch( row, col + 1, ' ', pal.dlg );
                scr_put( row, col + 2, ctl->text, pal.dlg );
                scr_ch( row, col + 2 + (int)str_len( ctl->text ), ' ', pal.dlg );
            }
            break;
        case CT_EDIT:
            draw_edit( dlg, ctl, focus );
            break;
        case CT_CHECK:
            draw_mark( dlg, ctl, *ctl->value != 0, 0, focus );
            break;
        case CT_RADIO:
            draw_mark( dlg, ctl, *ctl->value == ctl->id, 1, focus );
            break;
        case CT_LIST:
            draw_list( dlg, ctl, focus );
            break;
        case CT_DROP:
            draw_drop( dlg, ctl, focus );
            break;
        case CT_BUTTON:
            draw_button( dlg, ctl, focus, index == enter );
            break;
        }
    }
    if ( dlg->drop ) {
        draw_drop_list( dlg, &dlg->ctl[dlg->drop - 1] );
    }
}

/* ------------------------------------------------------------------ */
/* running a dialog                                                    */
/* ------------------------------------------------------------------ */

static void told( DLG *dlg, int index )
{
    if ( dlg->changed ) {
        dlg->changed( dlg, index );
    }
}

static void enter_edit( CTL *ctl )
{
    if ( ctl->type == CT_EDIT ) {
        ctl->fresh = 1;
        ctl->caret = (int)str_len( ctl->buf );
    }
}

static void move_focus( DLG *dlg, int step )
{
    int index = dlg->focus, tries;
    CTL *ctl;

    for ( tries = 0; tries < dlg->nctl; tries++ ) {
        index = (index + step + dlg->nctl) % dlg->nctl;
        ctl = &dlg->ctl[index];
        if ( !focusable( ctl ) ) {
            continue;
        }
        /* an option-button group is one stop: the one that is set */
        if ( ctl->type == CT_RADIO && *ctl->value != ctl->id ) {
            continue;
        }
        dlg->focus = index;
        enter_edit( ctl );
        return;
    }
}

/* Alt+letter, or the letter: the control whose label has it.  Returns
   a button's place + 1 when that is what it was, to be pressed. */
static int hot_jump( DLG *dlg, int letter )
{
    int index, target;
    CTL *ctl;

    for ( index = 0; index < dlg->nctl; index++ ) {
        ctl = &dlg->ctl[index];
        if ( ctl->text == NULL || ctl->type == CT_TEXT || ctl->type == CT_FRAME
             || scr_hotkey( ctl->text ) != letter ) {
            continue;
        }
        target = index;
        if ( ctl->type == CT_LABEL ) {          /* a label names what follows it */
            target = index + 1;
        }
        if ( target >= dlg->nctl || !focusable( &dlg->ctl[target] ) ) {
            return 0;
        }
        dlg->focus = target;
        ctl = &dlg->ctl[target];
        if ( ctl->type == CT_EDIT ) {
            enter_edit( ctl );
        } else if ( ctl->type == CT_CHECK ) {
            *ctl->value = !*ctl->value;
            told( dlg, target );
        } else if ( ctl->type == CT_RADIO ) {
            *ctl->value = ctl->id;
            told( dlg, target );
        } else if ( ctl->type == CT_BUTTON ) {
            return target + 1;                  /* pressed */
        }
        return 0;
    }
    return 0;
}

static int edit_key( DLG *dlg, CTL *ctl, int key )
{
    int len = (int)str_len( ctl->buf ), index = (int)(ctl - dlg->ctl);

    switch ( key & ~K_SHIFT ) {
    case K_LEFT:
        ctl->fresh = 0;
        if ( ctl->caret > 0 ) {
            ctl->caret--;
        }
        return 1;
    case K_RIGHT:
        ctl->fresh = 0;
        if ( ctl->caret < len ) {
            ctl->caret++;
        }
        return 1;
    case K_HOME:
        ctl->fresh = 0;
        ctl->caret = 0;
        return 1;
    case K_END:
        ctl->fresh = 0;
        ctl->caret = len;
        return 1;
    case K_BS:
        if ( ctl->fresh ) {
            ctl->buf[0] = 0;
            ctl->caret = 0;
        } else if ( ctl->caret > 0 ) {
            mem_cpy( ctl->buf + ctl->caret - 1, ctl->buf + ctl->caret, (u32)(len - ctl->caret + 1) );
            ctl->caret--;
        }
        ctl->fresh = 0;
        told( dlg, index );
        return 1;
    case K_DEL:
        if ( ctl->fresh ) {
            ctl->buf[0] = 0;
            ctl->caret = 0;
        } else if ( ctl->caret < len ) {
            mem_cpy( ctl->buf + ctl->caret, ctl->buf + ctl->caret + 1, (u32)(len - ctl->caret) );
        }
        ctl->fresh = 0;
        told( dlg, index );
        return 1;
    }
    if ( key >= ' ' && key < 0x100 ) {
        if ( ctl->fresh ) {                     /* typing replaces what was offered */
            ctl->buf[0] = 0;
            ctl->caret = 0;
            len = 0;
        }
        ctl->fresh = 0;
        if ( len < ctl->max - 1 ) {
            mem_cpy( ctl->buf + ctl->caret + 1, ctl->buf + ctl->caret, (u32)(len - ctl->caret + 1) );
            ctl->buf[ctl->caret++] = (char)key;
        }
        told( dlg, index );
        return 1;
    }
    return 0;
}

/* A list's keys on a position: "*sel" of "count" entries, "rows" of
   them in sight.  1 = the key was the list's. */
static int pick_key( DLG *dlg, CTL *ctl, int key, int rows, int *sel )
{
    int index, letter, at;
    const char *text;

    switch ( key & ~K_SHIFT ) {
    case K_UP:   (*sel)--;              break;
    case K_DOWN: (*sel)++;              break;
    case K_PGUP: *sel -= rows > 1 ? rows - 1 : 1; break;
    case K_PGDN: *sel += rows > 1 ? rows - 1 : 1; break;
    case K_HOME: *sel = 0;              break;
    case K_END:  *sel = ctl->count - 1; break;
    default:
        if ( key <= ' ' || key >= 0x100 || ctl->count == 0 ) {
            return 0;
        }
        /* a letter: the next entry that starts with it */
        letter = ch_upper( key );
        for ( index = 1; index <= ctl->count; index++ ) {
            at = (*sel + index) % ctl->count;
            text = entry( dlg, ctl, at );
            while ( *text == ' ' || *text == '[' ) {
                text++;
            }
            if ( ch_upper( text[0] ) == letter ) {
                *sel = at;
                break;
            }
        }
        break;
    }
    if ( *sel >= ctl->count ) {
        *sel = ctl->count - 1;
    }
    if ( *sel < 0 ) {
        *sel = 0;
    }
    return 1;
}

static int list_key( DLG *dlg, CTL *ctl, int key )
{
    int old = ctl->sel;

    if ( !pick_key( dlg, ctl, key, ctl->type == CT_LIST ? ctl->height - 2 : 1, &ctl->sel ) ) {
        return 0;
    }
    if ( ctl->sel != old ) {
        told( dlg, (int)(ctl - dlg->ctl) );
    }
    return 1;
}

static int radio_step( DLG *dlg, CTL *ctl, int step )
{
    int index = (int)(ctl - dlg->ctl) + step;

    if ( index >= 0 && index < dlg->nctl && dlg->ctl[index].type == CT_RADIO
         && dlg->ctl[index].value == ctl->value && !dlg->ctl[index].disabled ) {
        *ctl->value = dlg->ctl[index].id;
        dlg->focus = index;
        told( dlg, index );
    }
    return 1;
}

static void help( DLG *dlg )
{
    if ( ui_help != NULL && dlg->help != NULL ) {
        ui_help( dlg->help );
    }
}

/* a button pressed: what the dialog answers, or 0 to stay open */
static int press( DLG *dlg, int index )
{
    int answer;

    if ( dlg->ctl[index].id == ID_HELP ) {
        help( dlg );
        return 0;
    }
    if ( dlg->activate ) {
        answer = dlg->activate( dlg, index );
        if ( answer ) {
            return answer;
        }
        if ( dlg->ctl[index].id != ID_CANCEL ) {
            return 0;
        }
    }
    return dlg->ctl[index].id;
}

/* Enter: the list's own answer first, then the default button's */
static int enter_key( DLG *dlg )
{
    int answer, index;

    if ( dlg->ctl[dlg->focus].type == CT_LIST && dlg->activate ) {
        dlg->stay = 0;
        answer = dlg->activate( dlg, dlg->focus );
        if ( answer || dlg->stay || default_button( dlg ) < 0 ) {
            return answer;
        }
    }
    index = default_button( dlg );
    return index >= 0 ? press( dlg, index ) : 0;
}

static void drop_open( DLG *dlg, int index )
{
    dlg->focus = index;
    dlg->drop = index + 1;
    dlg->drop_pick = dlg->ctl[index].sel;
    dlg->drop_top = dlg->ctl[index].sel;
}

/* shut it: with the entry the keys were on taken, or as it was */
static void drop_close( DLG *dlg, int take )
{
    CTL *ctl = &dlg->ctl[dlg->drop - 1];
    int index = dlg->drop - 1;

    dlg->drop = 0;
    if ( take && dlg->drop_pick != ctl->sel && dlg->drop_pick < ctl->count ) {
        ctl->sel = dlg->drop_pick;
        told( dlg, index );
    }
}

static void drop_key( DLG *dlg, int key )
{
    CTL *ctl = &dlg->ctl[dlg->drop - 1];
    int top, left, rows, width;

    drop_place( dlg, ctl, &top, &left, &rows, &width );
    switch ( key ) {
    case K_ESC:
        drop_close( dlg, 0 );
        return;
    case K_ENTER:
    case ' ':
    case K_F4:
    case K_UP | K_ALT:
    case K_DOWN | K_ALT:
        drop_close( dlg, 1 );
        return;
    case K_TAB:
        drop_close( dlg, 1 );
        move_focus( dlg, 1 );
        return;
    case K_SHTAB:
        drop_close( dlg, 1 );
        move_focus( dlg, -1 );
        return;
    }
    pick_key( dlg, ctl, key, rows, &dlg->drop_pick );
}

static void drop_mouse( DLG *dlg )
{
    CTL *ctl = &dlg->ctl[dlg->drop - 1];
    int top, left, rows, width, item;

    drop_place( dlg, ctl, &top, &left, &rows, &width );
    if ( mouse.kind == ME_WHEEL ) {
        pick_key( dlg, ctl, mouse.wheel < 0 ? K_UP : K_DOWN, rows, &dlg->drop_pick );
        return;
    }
    if ( !left_mouse() ) {
        return;
    }
    if ( mouse.row > top && mouse.row <= top + rows && mouse.col > left && mouse.col < left + width ) {
        if ( mouse.col == left + width - 1 && ctl->count > rows ) {
            if ( mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE ) {
                switch ( sbar_hit( top + 1, top + rows, dlg->drop_top, rows, ctl->count, mouse.row ) ) {
                case SB_UP:   pick_key( dlg, ctl, K_UP, rows, &dlg->drop_pick );   break;
                case SB_DOWN: pick_key( dlg, ctl, K_DOWN, rows, &dlg->drop_pick ); break;
                case SB_PGUP: pick_key( dlg, ctl, K_PGUP, rows, &dlg->drop_pick ); break;
                case SB_PGDN: pick_key( dlg, ctl, K_PGDN, rows, &dlg->drop_pick ); break;
                }
            }
            return;
        }
        item = dlg->drop_top + (mouse.row - top - 1);
        if ( item < ctl->count ) {
            dlg->drop_pick = item;              /* the pointer lights it, */
            if ( mouse.kind == ME_UP ) {
                drop_close( dlg, 1 );           /* and letting go takes it */
            }
        }
        return;
    }
    if ( mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE ) {
        drop_close( dlg, 0 );                   /* a press anywhere else shuts it */
    }
}

/* the control at a cell of the screen, or -1 */
static int ctl_at( DLG *dlg, int row, int col )
{
    int index, top, left, width;
    CTL *ctl;

    for ( index = 0; index < dlg->nctl; index++ ) {
        ctl = &dlg->ctl[index];
        top = dlg->top + ctl->row;
        left = dlg->left + ctl->col;
        if ( ctl->disabled ) {
            continue;
        }
        switch ( ctl->type ) {
        case CT_LIST:
            if ( row >= top && row < top + ctl->height && col >= left && col < left + ctl->width ) {
                return index;
            }
            continue;
        case CT_EDIT:
        case CT_DROP:
            width = ctl->width;
            break;
        case CT_LABEL:
            width = scr_hotlen( ctl->text );
            break;
        case CT_CHECK:
        case CT_RADIO:
        case CT_BUTTON:
            width = scr_hotlen( ctl->text ) + 4;
            break;
        default:
            continue;
        }
        if ( row == top && col >= left && col < left + width ) {
            return index;
        }
    }
    return -1;
}

/* a press on a list: its scroll bar scrolls it, an entry is chosen, and
   a double press on one is Enter */
static int list_mouse( DLG *dlg, CTL *ctl, int dbl )
{
    int top = dlg->top + ctl->row, left = dlg->left + ctl->col;
    int rows = ctl->height - 2, item;

    if ( mouse.col == left + ctl->width - 1 && ctl->count > rows ) {
        switch ( sbar_hit( top + 1, top + rows, ctl->top, rows, ctl->count, mouse.row ) ) {
        case SB_UP:   list_key( dlg, ctl, K_UP );   break;
        case SB_DOWN: list_key( dlg, ctl, K_DOWN ); break;
        case SB_PGUP: list_key( dlg, ctl, K_PGUP ); break;
        case SB_PGDN: list_key( dlg, ctl, K_PGDN ); break;
        }
        return 0;
    }
    if ( mouse.row <= top || mouse.row > top + rows ) {
        return 0;
    }
    item = ctl->top + (mouse.row - top - 1);
    if ( item >= ctl->count ) {
        return 0;
    }
    ctl->sel = item;
    /* told even when it was chosen already: a click is a choice */
    told( dlg, (int)(ctl - dlg->ctl) );
    return dbl ? enter_key( dlg ) : 0;
}

/* the mouse in a dialog: the answer, when it has one.  "armed" is the
   button a press went down on - it is pressed when the button is let go
   over it, as a button is */
static int dlg_mouse( DLG *dlg, int *armed )
{
    int index = ctl_at( dlg, mouse.row, mouse.col ), target, was;
    CTL *ctl;

    if ( mouse.kind == ME_WHEEL ) {
        if ( index >= 0 && (dlg->ctl[index].type == CT_LIST || dlg->ctl[index].type == CT_DROP) ) {
            list_key( dlg, &dlg->ctl[index], mouse.wheel < 0 ? K_UP : K_DOWN );
        }
        return 0;
    }
    if ( !left_mouse() ) {
        return 0;
    }
    if ( mouse.kind == ME_UP ) {
        was = *armed;
        *armed = -1;
        return was >= 0 && index == was ? press( dlg, was ) : 0;
    }
    if ( mouse.kind == ME_DRAG || index < 0 ) {
        return 0;
    }
    ctl = &dlg->ctl[index];
    if ( ctl->type == CT_LABEL ) {              /* a label names what follows it */
        target = index + 1;
        if ( target >= dlg->nctl || !focusable( &dlg->ctl[target] ) ) {
            return 0;
        }
        dlg->focus = target;
        enter_edit( &dlg->ctl[target] );
        return 0;
    }
    dlg->focus = index;
    switch ( ctl->type ) {
    case CT_EDIT:
        ctl->fresh = 0;
        ctl->caret = ctl->scroll + mouse.col - (dlg->left + ctl->col);
        if ( ctl->caret > (int)str_len( ctl->buf ) ) {
            ctl->caret = (int)str_len( ctl->buf );
        }
        break;
    case CT_CHECK:
        *ctl->value = !*ctl->value;
        told( dlg, index );
        break;
    case CT_RADIO:
        *ctl->value = ctl->id;
        told( dlg, index );
        break;
    case CT_LIST:
        return list_mouse( dlg, ctl, mouse.kind == ME_DOUBLE );
    case CT_DROP:
        drop_open( dlg, index );
        break;
    case CT_BUTTON:
        *armed = index;
        break;
    }
    return 0;
}

/* where the box goes: the middle of the screen unless it was placed,
   and all of it on the screen either way */
static void dlg_settle( DLG *dlg )
{
    if ( !dlg->placed ) {
        dlg->top = (scr_rows - dlg->rows) / 2;
        dlg->left = (scr_cols - dlg->cols) / 2;
    }
    if ( dlg->top + dlg->rows > scr_rows ) {
        dlg->top = scr_rows - dlg->rows;
    }
    if ( dlg->left + dlg->cols > scr_cols ) {
        dlg->left = scr_cols - dlg->cols;
    }
    if ( dlg->top < 0 ) {
        dlg->top = 0;
    }
    if ( dlg->left < 0 ) {
        dlg->left = 0;
    }
}

int dlg_run( DLG *dlg )
{
    int key, answer = 0, index, letter, armed = -1;
    CTL *ctl;

    if ( dlg->nctl <= 0 ) {
        return ID_CANCEL;
    }
    if ( dlg->focus < 0 || dlg->focus >= dlg->nctl || !focusable( &dlg->ctl[dlg->focus] ) ) {
        dlg->focus = dlg->nctl - 1;
        move_focus( dlg, 1 );
    }
    enter_edit( &dlg->ctl[dlg->focus] );
    dlg->drop = 0;
    ui_push( dlg );
    while ( answer == 0 ) {
        /* said every turn: a box that was open over this one took its
           own hint away with it */
        app_status( "Enter=OK   Esc=Cancel   Tab=Next Field" );
        dlg_settle( dlg );                      /* every turn: the screen may have changed size */
        ui_redraw();
        if ( depth > MAX_DIALOGS ) {
            dlg_draw( dlg );                    /* too deep for the stack: drawn here */
        }
        scr_flush();
        key = key_get();
        if ( key_service( key ) ) {
            continue;
        }
        if ( key == K_MOUSE ) {
            if ( dlg->drop ) {
                drop_mouse( dlg );
            } else {
                answer = dlg_mouse( dlg, &armed );
            }
            continue;
        }
        if ( dlg->drop ) {
            drop_key( dlg, key );
            continue;
        }
        ctl = &dlg->ctl[dlg->focus];
        switch ( key ) {
        case K_ESC:
            for ( index = 0; index < dlg->nctl; index++ ) {
                if ( dlg->ctl[index].type == CT_BUTTON && dlg->ctl[index].id == ID_CANCEL ) {
                    answer = press( dlg, index );
                    break;
                }
            }
            if ( index == dlg->nctl ) {
                answer = ID_CANCEL;
            }
            continue;
        case K_F1:
            help( dlg );
            continue;
        case K_TAB:
            move_focus( dlg, 1 );
            continue;
        case K_SHTAB:
            move_focus( dlg, -1 );
            continue;
        case K_ENTER:
            answer = enter_key( dlg );
            continue;
        }
        if ( ctl->type == CT_EDIT && edit_key( dlg, ctl, key ) ) {
            continue;
        }
        if ( ctl->type == CT_DROP
             && (key == K_F4 || key == ' ' || key == (K_DOWN | K_ALT) || key == (K_UP | K_ALT)) ) {
            drop_open( dlg, dlg->focus );
            continue;
        }
        if ( (ctl->type == CT_LIST || ctl->type == CT_DROP) && key != ' ' && list_key( dlg, ctl, key ) ) {
            continue;
        }
        if ( ctl->type == CT_RADIO ) {
            if ( key == K_UP || key == K_LEFT ) {
                radio_step( dlg, ctl, -1 );
                continue;
            }
            if ( key == K_DOWN || key == K_RIGHT ) {
                radio_step( dlg, ctl, 1 );
                continue;
            }
        }
        if ( key == ' ' ) {
            if ( ctl->type == CT_CHECK ) {
                *ctl->value = !*ctl->value;
                told( dlg, dlg->focus );
            } else if ( ctl->type == CT_BUTTON ) {
                answer = press( dlg, dlg->focus );
            } else if ( ctl->type == CT_LIST && dlg->activate ) {
                answer = dlg->activate( dlg, dlg->focus );
            }
            continue;
        }
        if ( (ctl->type == CT_BUTTON || ctl->type == CT_CHECK) && (key == K_LEFT || key == K_UP) ) {
            move_focus( dlg, -1 );
            continue;
        }
        if ( (ctl->type == CT_BUTTON || ctl->type == CT_CHECK) && (key == K_RIGHT || key == K_DOWN) ) {
            move_focus( dlg, 1 );
            continue;
        }
        letter = key_alt_letter( key );
        if ( letter == 0 && key > ' ' && key < 0x100 && ctl->type != CT_EDIT && ctl->type != CT_LIST
             && ctl->type != CT_DROP ) {
            letter = ch_upper( key );
        }
        if ( letter ) {
            index = hot_jump( dlg, letter );
            if ( index ) {
                answer = press( dlg, index - 1 );
            }
        }
    }
    dlg->drop = 0;
    ui_pop();
    app_status( NULL );
    return answer;
}

/* ------------------------------------------------------------------ */
/* controls, filled in a line at a time                                */
/* ------------------------------------------------------------------ */

static void ctl_at_cell( CTL *ctl, int type, int row, int col, const char *text )
{
    mem_set( ctl, 0, sizeof( *ctl ) );
    ctl->type = type;
    ctl->row = row;
    ctl->col = col;
    ctl->text = text;
}

void ctl_label( CTL *ctl, int row, int col, const char *text )
{
    ctl_at_cell( ctl, CT_LABEL, row, col, text );
}

void ctl_text( CTL *ctl, int row, int col, int width, const char *text )
{
    ctl_at_cell( ctl, CT_TEXT, row, col, text );
    ctl->width = width;
}

void ctl_frame( CTL *ctl, int row, int col, int width, int height, const char *text )
{
    ctl_at_cell( ctl, CT_FRAME, row, col, text );
    ctl->width = width;
    ctl->height = height;
}

void ctl_edit( CTL *ctl, int row, int col, int width, char *buf, int max )
{
    ctl_at_cell( ctl, CT_EDIT, row, col, NULL );
    ctl->width = width;
    ctl->buf = buf;
    ctl->max = max;
}

void ctl_check( CTL *ctl, int row, int col, const char *text, int *value )
{
    ctl_at_cell( ctl, CT_CHECK, row, col, text );
    ctl->value = value;
}

void ctl_radio( CTL *ctl, int row, int col, const char *text, int *value, int id )
{
    ctl_at_cell( ctl, CT_RADIO, row, col, text );
    ctl->value = value;
    ctl->id = id;
}

void ctl_list( CTL *ctl, int row, int col, int width, int height, const char *title,
               const char *const *strings, int count )
{
    ctl_at_cell( ctl, CT_LIST, row, col, title );
    ctl->width = width;
    ctl->height = height;
    ctl->strings = strings;
    ctl->count = count;
}

void ctl_drop( CTL *ctl, int row, int col, int width, const char *const *strings, int count )
{
    ctl_at_cell( ctl, CT_DROP, row, col, NULL );
    ctl->width = width;
    ctl->strings = strings;
    ctl->count = count;
}

void ctl_button( CTL *ctl, int row, int col, const char *text, int id, int is_default )
{
    ctl_at_cell( ctl, CT_BUTTON, row, col, text );
    ctl->id = id;
    ctl->is_default = is_default;
}

int ctl_button_width( const char *text )
{
    return scr_hotlen( text ) + 4;
}

/* ------------------------------------------------------------------ */
/* message boxes                                                       */
/* ------------------------------------------------------------------ */

#define MSG_MAX         600     /* a message's characters */
#define MSG_WIDE        60      /* ...and how long a line of it may be */

/* "text" into "out" with every line no longer than MSG_WIDE: broken at
   a blank where there is one.  Returns the lines, and the longest. */
static int wrap( const char *text, char *out, u32 max, int *widest )
{
    u32 len = 0;
    int col = 0, lines = 1, last_blank = -1, index;

    *widest = 0;
    for ( ; *text && len + 2 < max; text++ ) {
        if ( *text == '\n' ) {
            if ( col > *widest ) {
                *widest = col;
            }
            out[len++] = '\n';
            col = 0;
            lines++;
            last_blank = -1;
            continue;
        }
        if ( col == MSG_WIDE ) {
            if ( last_blank >= 0 ) {
                /* the blank becomes the break, and what follows it is
                   the start of the new line */
                out[last_blank] = '\n';
                index = (int)len - last_blank - 1;
                if ( col - index - 1 > *widest ) {
                    *widest = col - index - 1;
                }
                col = index;
            } else {
                out[len++] = '\n';
                if ( col > *widest ) {
                    *widest = col;
                }
                col = 0;
            }
            lines++;
            last_blank = -1;
        }
        if ( *text == ' ' ) {
            last_blank = (int)len;
        }
        out[len++] = *text;
        col++;
    }
    out[len] = 0;
    if ( col > *widest ) {
        *widest = col;
    }
    return lines;
}

/* lines of text, a "\n" between them, over a row of buttons */
int msg_text( const char *title, const char *text, int buttons )
{
    CTL ctl[4];
    DLG dlg;
    char *lines_buf;
    int width = 0, nbtn = 0, index, col, total = 0, lines, answer;
    const char *labels[3];
    int ids[3];

    lines_buf = (char *)try_alloc( MSG_MAX );
    if ( lines_buf == NULL ) {
        return ID_CANCEL;
    }
    lines = wrap( text, lines_buf, MSG_MAX, &width );
    if ( lines > 16 ) {
        lines = 16;
    }
    switch ( buttons ) {
    case MB_YESNO:
        labels[0] = "&Yes"; ids[0] = ID_YES;
        labels[1] = "&No";  ids[1] = ID_NO;
        nbtn = 2;
        break;
    case MB_YESNOCANCEL:
        labels[0] = "&Yes";   ids[0] = ID_YES;
        labels[1] = "&No";    ids[1] = ID_NO;
        labels[2] = "Cancel"; ids[2] = ID_CANCEL;
        nbtn = 3;
        break;
    case MB_OKCANCEL:
        labels[0] = "OK";     ids[0] = ID_OK;
        labels[1] = "Cancel"; ids[1] = ID_CANCEL;
        nbtn = 2;
        break;
    default:
        labels[0] = "  OK  "; ids[0] = ID_OK;
        nbtn = 1;
        break;
    }
    for ( index = 0; index < nbtn; index++ ) {
        total += ctl_button_width( labels[index] ) + (index ? 2 : 0);
    }
    if ( total > width ) {
        width = total;
    }
    if ( title && (int)str_len( title ) + 2 > width ) {
        width = (int)str_len( title ) + 2;
    }
    width += 6;
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_text( &ctl[0], 2, 3, 0, lines_buf );
    col = (width - total) / 2;
    for ( index = 0; index < nbtn; index++ ) {
        ctl_button( &ctl[1 + index], lines + 3, col, labels[index], ids[index], index == 0 );
        col += ctl_button_width( labels[index] ) + 2;
    }
    dlg.title = title;
    dlg.rows = lines + 5;
    dlg.cols = width;
    dlg.ctl = ctl;
    dlg.nctl = 1 + nbtn;
    dlg.focus = 1;
    answer = dlg_run( &dlg );
    xfree( lines_buf );
    /* Esc in a box with no Cancel: No where that is asked, else OK */
    if ( answer == ID_CANCEL && buttons != MB_OKCANCEL && buttons != MB_YESNOCANCEL ) {
        answer = buttons == MB_YESNO ? ID_NO : ID_OK;
    }
    return answer;
}

int msg_box2( const char *line1, const char *line2, int buttons )
{
    char text[400];

    if ( line2 && *line2 ) {
        sfmt( text, sizeof( text ), "%s\n%s", line1, line2 );
    } else {
        sfmt( text, sizeof( text ), "%s", line1 );
    }
    return msg_text( ui_name, text, buttons );
}

int msg_box( const char *text, int buttons )
{
    return msg_text( ui_name, text, buttons );
}

/* a DOS error as a sentence */
const char *err_text( int rc )
{
    static char other[32];

    switch ( rc ) {
    case 0:             return "";
    case ERR_NOFILE:    return "The file was not found.";
    case ERR_NOPATH:    return "The directory was not found.";
    case ERR_NOHANDLES: return "Too many files are open.";
    case ERR_ACCESS:    return "DOS will not allow it. The file may be read-only or in use.";
    case ERR_NOMEM:     return "There is not enough memory.";
    case ERR_BADDRIVE:  return "There is no such drive.";
    case ERR_NOMORE:    return "There are no such files.";
    case ERR_WRPROTECT: return "The disk is write-protected.";
    case ERR_NOTREADY:  return "The drive is not ready.";
    case ERR_EXISTS:    return "There is a file of that name already.";
    case ERR_DISKFULL:  return "The disk is full.";
    }
    sfmt( other, sizeof( other ), "DOS error %d.", rc );
    return other;
}

void msg_error( const char *what, int rc )
{
    msg_box2( what, err_text( rc ), MB_OK );
}

int input_box( const char *title, const char *label, char *buf, int max, int width )
{
    CTL ctl[4];
    DLG dlg;
    char *text;
    int cols, answer, label_len = scr_hotlen( label );

    if ( max <= 0 ) {
        return 0;
    }
    text = (char *)try_alloc( (u32)max );
    if ( text == NULL ) {
        return 0;
    }
    sfmt( text, (u32)max, "%s", buf );
    if ( width > 70 - label_len ) {
        width = 70 - label_len;
    }
    if ( width < 4 ) {
        width = 4;
    }
    cols = label_len + width + 7;
    if ( cols < 30 ) {
        cols = 30;
    }
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_label( &ctl[0], 2, 3, label );
    ctl_edit( &ctl[1], 2, 4 + label_len, width, text, max );
    ctl_button( &ctl[2], 4, cols / 2 - 11, "  OK  ", ID_OK, 1 );
    ctl_button( &ctl[3], 4, cols / 2 + 1, "Cancel", ID_CANCEL, 0 );
    dlg.title = title;
    dlg.rows = 6;
    dlg.cols = cols;
    dlg.ctl = ctl;
    dlg.nctl = 4;
    dlg.focus = 1;
    answer = dlg_run( &dlg );
    if ( answer == ID_OK ) {
        str_cpy( buf, text );           /* it was never let past max */
    }
    xfree( text );
    return answer == ID_OK;
}

/* ------------------------------------------------------------------ */
/* a box that says what is going on                                    */
/* ------------------------------------------------------------------ */

void busy_show( const char *line1, const char *line2 )
{
    int width, len, top, left;

    if ( line1 == NULL ) {
        return;                         /* the next redraw leaves it out */
    }
    width = (int)str_len( line1 );
    len = line2 ? (int)str_len( line2 ) : 0;
    if ( len > width ) {
        width = len;
    }
    width += 6;
    if ( width < 40 ) {
        width = 40;
    }
    if ( width > scr_cols - 4 ) {
        width = scr_cols - 4;
    }
    top = (scr_rows - 6) / 2;
    left = (scr_cols - width) / 2;
    ui_redraw();
    dlg_frame( top, left, 6, width, ui_name );
    scr_putw( top + 2, left + 3, line1, width - 6, pal.dlg );
    if ( line2 ) {
        scr_putw( top + 3, left + 3, line2, width - 6, pal.dlg );
    }
    scr_cursor( 0, 0, CUR_HIDE );
    scr_flush();
}
