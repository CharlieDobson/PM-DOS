/*
 * UI.C - the menu bar, its menus, dialog boxes and message boxes.
 *
 * NOTHING HERE REMEMBERS WHAT IT COVERED.  A dialog is drawn over the
 * whole screen as the rest of the program draws it (app_draw), with any
 * dialogs under it drawn first - ui_push keeps that stack - and the
 * screen layer sends only what changed.  A message box over the Replace
 * dialog over a window is three paints and one short flush.
 *
 * A DIALOG IS A TABLE OF CONTROLS: labels, entry fields, check boxes,
 * option buttons, lists and buttons, each at a row and column inside
 * the box.  Tab and Shift+Tab walk them; Alt and a letter (or just the
 * letter, when the focus is not somewhere that takes letters) jumps to
 * the one whose label has that access key; Enter presses the default
 * button; Esc is Cancel; F1 is help.  The mouse presses what it lands
 * on - a button when it is let go over it, as buttons are pressed - and
 * a double click in a list is Enter.  Two hooks let a dialog react:
 * "changed" when a control's value moves, and "activate" for Enter on a
 * list and for a button - which returns the dialog's answer, or 0 to
 * stay open.
 */
#include "edit.h"

/* ------------------------------------------------------------------ */
/* the menus                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *text;           /* NULL: a separator */
    const char *keys;
    int         cmd;
    const char *hint;
} MITEM;

static const MITEM file_items[] = {
    { "&New",        "",        CMD_NEW,    "Starts a new, empty file" },
    { "&Open...",    "",        CMD_OPEN,   "Opens a file from a disk" },
    { "&Save",       "",        CMD_SAVE,   "Saves the file under its own name" },
    { "Save &As...", "",        CMD_SAVEAS, "Saves the file under a name you choose" },
    { "&Close",      "",        CMD_CLOSE,  "Closes the file" },
    { NULL,          NULL,      0,          NULL },
    { "&Print...",   "",        CMD_PRINT,  "Prints the file, or the selected text" },
    { NULL,          NULL,      0,          NULL },
    { "E&xit",       "",        CMD_EXIT,   "Leaves the editor" }
};

static const MITEM edit_items[] = {
    { "Cu&t",        "Shift+Del", CMD_CUT,   "Moves the selected text to the clipboard" },
    { "&Copy",       "Ctrl+Ins",  CMD_COPY,  "Copies the selected text to the clipboard" },
    { "&Paste",      "Shift+Ins", CMD_PASTE, "Puts the clipboard's text in at the cursor" },
    { "Cl&ear",      "Del",       CMD_CLEAR, "Deletes the selected text" }
};

static const MITEM search_items[] = {
    { "&Find...",          "",   CMD_FIND,    "Looks for some text" },
    { "Repeat &Last Find", "F3", CMD_REPEAT,  "Looks for the same text again" },
    { "&Replace...",       "",   CMD_REPLACE, "Looks for some text and changes it" }
};

static const MITEM view_items[] = {
    { "&Split Window",  "Ctrl+F6", CMD_SPLIT,    "Shows two windows, or goes back to one" },
    { "Si&ze Window",   "Ctrl+F8", CMD_SIZE,     "Moves the line between the two windows" },
    { "&Close Window",  "Ctrl+F4", CMD_CLOSEWIN, "Closes the window" },
    { "&Output Screen", "F4",      CMD_OUTPUT,   "Shows the screen from before the editor" }
};

static const MITEM options_items[] = {
    { "&Settings...", "", CMD_SETTINGS, "Tab stops and the printer port" },
    { "&Colors...",   "", CMD_COLORS,   "The colours of the text and the screen" }
};

static const MITEM help_items[] = {
    { "&Commands...", "F1", CMD_HELPCMDS, "How to use the editor" },
    { "&About...",    "",   CMD_ABOUT,    "The editor's version" }
};

typedef struct {
    const char  *title;
    const MITEM *items;
    int          count;
} MENU;

static const MENU menus[] = {
    { "&File",    file_items,    sizeof( file_items ) / sizeof( MITEM ) },
    { "&Edit",    edit_items,    sizeof( edit_items ) / sizeof( MITEM ) },
    { "&Search",  search_items,  sizeof( search_items ) / sizeof( MITEM ) },
    { "&View",    view_items,    sizeof( view_items ) / sizeof( MITEM ) },
    { "&Options", options_items, sizeof( options_items ) / sizeof( MITEM ) },
    { "&Help",    help_items,    sizeof( help_items ) / sizeof( MITEM ) }
};
#define NMENUS  (int)(sizeof( menus ) / sizeof( MENU ))

/* the View menu ends with the open files: "&1 NAME" */
static char   doc_texts[MAX_DOCS][40];
static MITEM  items_buf[24];

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

    scr_fill( 0, 0, scr_cols, ' ', pal.menu );
    for ( index = 0; index < NMENUS; index++ ) {
        col = menu_col( index );
        if ( index == selected ) {
            scr_fill( 0, col - 1, scr_hotlen( menus[index].title ) + 2, ' ', pal.menu_sel );
            scr_hot( 0, col, menus[index].title, pal.menu_sel,
                     show_hot ? pal.menu_sel_hot : pal.menu_sel );
        } else {
            scr_hot( 0, col, menus[index].title, pal.menu, show_hot ? pal.menu_hot : pal.menu );
        }
    }
}

/* the items of menu "menu", the View menu's open files included */
static int menu_items( int menu, const MITEM **items )
{
    int count = menus[menu].count, index;

    if ( menu != 3 ) {
        *items = menus[menu].items;
        return count;
    }
    mem_cpy( items_buf, view_items, sizeof( view_items ) );
    if ( ndocs ) {
        items_buf[count].text = NULL;
        count++;
    }
    for ( index = 0; index < ndocs; index++ ) {
        doc_texts[index][0] = '&';
        doc_texts[index][1] = (char)('1' + index);
        doc_texts[index][2] = ' ';
        str_cpyn( doc_texts[index] + 3, docs[index]->name, sizeof( doc_texts[index] ) - 3 );
        items_buf[count].text = doc_texts[index];
        items_buf[count].keys = "";
        items_buf[count].cmd = CMD_DOC1 + index;
        items_buf[count].hint = "Brings this file to the front";
        count++;
    }
    *items = items_buf;
    return count;
}

static int menu_width( const MITEM *items, int count )
{
    int index, width = 0, text, keys;

    for ( index = 0; index < count; index++ ) {
        if ( items[index].text ) {
            text = scr_hotlen( items[index].text );
            keys = (int)str_len( items[index].keys );
            if ( text + (keys ? keys + 3 : 0) > width ) {
                width = text + (keys ? keys + 3 : 0);
            }
        }
    }
    return width + 4;
}

/* where a menu's pull-down goes: kept on the screen, shadow and all */
static int menu_left( int menu, int width )
{
    int left = menu_col( menu ) - 2;

    if ( left + width + 2 > scr_cols ) {
        left = scr_cols - width - 2;
    }
    return left;
}

/* the menu whose title is at "col" on the bar, or -1 */
static int menu_at( int col )
{
    int index, start;

    for ( index = 0; index < NMENUS; index++ ) {
        start = menu_col( index );
        if ( col >= start - 1 && col <= start + scr_hotlen( menus[index].title ) ) {
            return index;
        }
    }
    return -1;
}

/* the item of an open menu at a cell, or -1 - a separator is not one */
static int menu_item_at( int menu, int row, int col )
{
    const MITEM *items;
    int count = menu_items( menu, &items );
    int width = menu_width( items, count );
    int left = menu_left( menu, width );

    if ( row < 2 || row >= 2 + count || col <= left || col >= left + width - 1 ) {
        return -1;
    }
    return items[row - 2].text ? row - 2 : -1;
}

static void menu_draw_pull( int menu, int sel )
{
    const MITEM *items;
    int count = menu_items( menu, &items );
    int width = menu_width( items, count );
    int left = menu_left( menu, width ), index, row;
    u8 attr, hot;

    scr_box( 1, left, count + 2, left + width - 1, pal.menu );
    for ( index = 0; index < count; index++ ) {
        row = 2 + index;
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
        scr_hot( row, left + 2, items[index].text, attr, hot );
        if ( items[index].keys[0] ) {
            scr_put( row, left + width - 2 - (int)str_len( items[index].keys ),
                     items[index].keys, attr );
        }
    }
    scr_shadow( 1, left, count + 2, left + width - 1 );
}

static int menu_by_letter( int letter )
{
    int index;

    for ( index = 0; index < NMENUS; index++ ) {
        if ( scr_hotkey( menus[index].title ) == letter ) {
            return index;
        }
    }
    return -1;
}

static int first_item( const MITEM *items, int count, int from, int step )
{
    int index = from, tries;

    for ( tries = 0; tries < count; tries++ ) {
        index = (index + step + count) % count;
        if ( items[index].text ) {
            return index;
        }
    }
    return from;
}

/*
 * F10 or a tap of Alt: the bar, with the first title lit.  Alt+letter:
 * that menu, open.  K_MOUSE, pressed on the bar: the menu under the
 * pointer, open, following the pointer while the button is held - let
 * go on an item and that is the command; let go on the title and the
 * menu stays open for a click or a key.  Returns the command, or 0.
 *
 * "sel" is -1 while the mouse has opened a menu without pointing at an
 * item: nothing is lit until it does, or a key moves.
 */
int menu_run( int key )
{
    const MITEM *items;
    int menu = 0, open = 0, sel = 0, count, letter, index, tracking = 0;

    letter = key_alt_letter( key );
    if ( letter ) {
        menu = menu_by_letter( letter );
        if ( menu < 0 ) {
            return 0;
        }
        open = 1;
    }
    count = menu_items( menu, &items );
    sel = first_item( items, count, -1 + count, 1 );
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
        count = menu_items( menu, &items );
        app_status( !open ? "Choose a menu: Enter opens it, Esc leaves"
                    : sel >= 0 ? items[sel].hint : "Choose a command" );
        app_draw();
        menu_draw_bar( menu, 1 );
        if ( open ) {
            menu_draw_pull( menu, sel );
        }
        scr_cursor( 0, 0, CUR_HIDE );
        scr_flush();
        key = key_get();
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        if ( key == K_MOUSE ) {
            index = mouse.row == 0 ? menu_at( mouse.col ) : -1;
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
            index = open ? menu_item_at( menu, mouse.row, mouse.col ) : -1;
            if ( index >= 0 ) {                 /* on an item */
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
            menu = (menu + NMENUS - 1) % NMENUS;
            count = menu_items( menu, &items );
            sel = first_item( items, count, count - 1, 1 );
            continue;
        case K_RIGHT:
            menu = (menu + 1) % NMENUS;
            count = menu_items( menu, &items );
            sel = first_item( items, count, count - 1, 1 );
            continue;
        case K_UP:
            if ( open ) {
                sel = first_item( items, count, sel >= 0 ? sel : 0, -1 );
            }
            open = 1;
            continue;
        case K_DOWN:
            if ( open ) {
                sel = first_item( items, count, sel >= 0 ? sel : count - 1, 1 );
            }
            open = 1;
            continue;
        case K_ENTER:
            if ( !open ) {
                open = 1;
                continue;
            }
            if ( sel < 0 ) {
                sel = first_item( items, count, count - 1, 1 );
                continue;
            }
            if ( app_can( items[sel].cmd ) ) {
                app_status( NULL );
                return items[sel].cmd;
            }
            continue;
        }
        letter = key_alt_letter( key );
        if ( letter == 0 && key < 0x100 ) {
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
                count = menu_items( menu, &items );
                sel = first_item( items, count, count - 1, 1 );
            }
            continue;
        }
        for ( index = 0; index < count; index++ ) {
            if ( items[index].text && scr_hotkey( items[index].text ) == letter ) {
                if ( app_can( items[index].cmd ) ) {
                    app_status( NULL );
                    return items[index].cmd;
                }
                sel = index;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* the dialog stack                                                    */
/* ------------------------------------------------------------------ */

static DLG *stack[8];
static int  depth;

void ui_push( DLG *dlg )
{
    if ( depth < 8 ) {
        stack[depth++] = dlg;
    }
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
    for ( index = 0; index < depth; index++ ) {
        dlg_draw( stack[index] );
    }
}

/* ------------------------------------------------------------------ */
/* drawing a dialog                                                    */
/* ------------------------------------------------------------------ */

static int focusable( CTL *ctl )
{
    return ctl->type == CT_EDIT || ctl->type == CT_CHECK || ctl->type == CT_RADIO
           || ctl->type == CT_LIST || ctl->type == CT_BUTTON;
}

static void draw_edit( DLG *dlg, CTL *ctl, int focus )
{
    int row = dlg->top + ctl->row, col = dlg->left + ctl->col;
    int len = (int)str_len( ctl->buf );

    if ( ctl->caret > len ) {
        ctl->caret = len;
    }
    if ( ctl->caret < ctl->scroll ) {
        ctl->scroll = ctl->caret;
    }
    if ( ctl->caret >= ctl->scroll + ctl->width ) {
        ctl->scroll = ctl->caret - ctl->width + 1;
    }
    scr_fill( row, col, ctl->width, ' ', pal.field );
    if ( len > ctl->scroll ) {
        scr_putn( row, col, ctl->buf + ctl->scroll,
                  len - ctl->scroll < ctl->width ? len - ctl->scroll : ctl->width,
                  focus && ctl->fresh && len ? pal.field_sel : pal.field );
    }
    if ( focus ) {
        scr_cursor( row, col + ctl->caret - ctl->scroll, CUR_LINE );
    }
}

static void draw_list( DLG *dlg, CTL *ctl, int focus )
{
    int top = dlg->top + ctl->row, left = dlg->left + ctl->col;
    int rows = ctl->height - 2, index, item;
    const char *text;

    scr_box( top, left, top + ctl->height - 1, left + ctl->width - 1, pal.dlg );
    if ( ctl->sel >= ctl->count ) {
        ctl->sel = ctl->count ? ctl->count - 1 : 0;
    }
    if ( ctl->sel < ctl->top ) {
        ctl->top = ctl->sel;
    }
    if ( ctl->sel >= ctl->top + rows ) {
        ctl->top = ctl->sel - rows + 1;
    }
    /* a list sits on the dialog's own colour, its chosen entry dark */
    for ( index = 0; index < rows; index++ ) {
        item = ctl->top + index;
        scr_fill( top + 1 + index, left + 1, ctl->width - 2, ' ', pal.dlg );
        if ( item < ctl->count ) {
            text = ctl->item( dlg, item );
            scr_putn( top + 1 + index, left + 2, text,
                      (int)str_len( text ) < ctl->width - 3 ? (int)str_len( text ) : ctl->width - 3,
                      item == ctl->sel ? pal.menu_sel : pal.dlg );
            if ( item == ctl->sel ) {
                scr_recolor( top + 1 + index, left + 1, ctl->width - 2, pal.menu_sel );
                if ( focus ) {
                    scr_cursor( top + 1 + index, left + 2, CUR_LINE );
                }
            }
        }
    }
    if ( ctl->count > rows ) {
        scr_ch( top + 1, left + ctl->width - 1, 0x18, pal.dlg );
        scr_ch( top + rows, left + ctl->width - 1, 0x19, pal.dlg );
    }
    if ( ctl->text ) {
        scr_hot( top, left + 2, ctl->text, pal.dlg, pal.dlg_hot );
    }
}

void dlg_draw( DLG *dlg )
{
    int index, row, col, len;
    CTL *ctl;
    const char *text, *end;
    u8 attr;

    scr_box( dlg->top, dlg->left, dlg->top + dlg->rows - 1, dlg->left + dlg->cols - 1, pal.dlg );
    scr_shadow( dlg->top, dlg->left, dlg->top + dlg->rows - 1, dlg->left + dlg->cols - 1 );
    if ( dlg->title ) {
        len = (int)str_len( dlg->title );
        col = dlg->left + (dlg->cols - len - 2) / 2;
        scr_ch( dlg->top, col, ' ', pal.dlg );
        scr_put( dlg->top, col + 1, dlg->title, pal.dlg );
        scr_ch( dlg->top, col + 1 + len, ' ', pal.dlg );
    }
    for ( index = 0; index < dlg->nctl; index++ ) {
        ctl = &dlg->ctl[index];
        row = dlg->top + ctl->row;
        col = dlg->left + ctl->col;
        switch ( ctl->type ) {
        case CT_LABEL:
            scr_hot( row, col, ctl->text, pal.dlg, pal.dlg_hot );
            break;
        case CT_TEXT:                           /* lines, as they are */
            text = ctl->text;
            while ( *text ) {
                end = str_chr( text, '\n' );
                len = end ? (int)(end - text) : (int)str_len( text );
                scr_putn( row++, ctl->width ? col + (ctl->width - len) / 2 : col, text, len,
                          pal.dlg );
                text += len + (end ? 1 : 0);
            }
            break;
        case CT_EDIT:
            draw_edit( dlg, ctl, index == dlg->focus );
            break;
        case CT_CHECK:
            scr_put( row, col, *ctl->value ? "[X]" : "[ ]", pal.dlg );
            scr_hot( row, col + 4, ctl->text, pal.dlg, pal.dlg_hot );
            if ( index == dlg->focus ) {
                scr_cursor( row, col + 1, CUR_LINE );
            }
            break;
        case CT_RADIO:
            scr_put( row, col, *ctl->value == ctl->id ? "(\x07)" : "( )", pal.dlg );
            scr_hot( row, col + 4, ctl->text, pal.dlg, pal.dlg_hot );
            if ( index == dlg->focus ) {
                scr_cursor( row, col + 1, CUR_LINE );
            }
            break;
        case CT_LIST:
            draw_list( dlg, ctl, index == dlg->focus );
            break;
        case CT_BUTTON:
            attr = index == dlg->focus ? pal.button_focus : pal.button;
            len = scr_hotlen( ctl->text );
            scr_ch( row, col, '<', attr );
            scr_ch( row, col + 1, ' ', attr );
            scr_hot( row, col + 2, ctl->text, attr, index == dlg->focus ? attr : pal.dlg_hot );
            scr_ch( row, col + 2 + len, ' ', attr );
            scr_ch( row, col + 3 + len, '>', attr );
            if ( index == dlg->focus ) {
                scr_cursor( row, col + 2, CUR_LINE );
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* running a dialog                                                    */
/* ------------------------------------------------------------------ */

static int default_button( DLG *dlg )
{
    int index;

    if ( dlg->ctl[dlg->focus].type == CT_BUTTON ) {
        return dlg->focus;
    }
    for ( index = 0; index < dlg->nctl; index++ ) {
        if ( dlg->ctl[index].type == CT_BUTTON && dlg->ctl[index].is_default ) {
            return index;
        }
    }
    return -1;
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
        if ( ctl->type == CT_EDIT ) {
            ctl->fresh = 1;
            ctl->caret = (int)str_len( ctl->buf );
        }
        return;
    }
}

/* Alt+letter, or the letter: the control whose label has it */
static int hot_jump( DLG *dlg, int letter )
{
    int index, target;
    CTL *ctl;

    for ( index = 0; index < dlg->nctl; index++ ) {
        ctl = &dlg->ctl[index];
        if ( ctl->text == NULL || ctl->type == CT_TEXT || scr_hotkey( ctl->text ) != letter ) {
            continue;
        }
        target = index;
        if ( ctl->type == CT_LABEL ) {          /* a label names what follows it */
            while ( target < dlg->nctl && !focusable( &dlg->ctl[target] ) ) {
                target++;
            }
            if ( target == dlg->nctl ) {
                return 0;
            }
        }
        dlg->focus = target;
        ctl = &dlg->ctl[target];
        if ( ctl->type == CT_EDIT ) {
            ctl->fresh = 1;
            ctl->caret = (int)str_len( ctl->buf );
        } else if ( ctl->type == CT_CHECK ) {
            *ctl->value = !*ctl->value;
            if ( dlg->changed ) {
                dlg->changed( dlg, target );
            }
        } else if ( ctl->type == CT_RADIO ) {
            *ctl->value = ctl->id;
            if ( dlg->changed ) {
                dlg->changed( dlg, target );
            }
        } else if ( ctl->type == CT_BUTTON ) {
            return target + 1;                  /* pressed */
        }
        return 0;
    }
    return 0;
}

static int edit_key( DLG *dlg, CTL *ctl, int key )
{
    int len = (int)str_len( ctl->buf );

    switch ( key ) {
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
        if ( dlg->changed ) {
            dlg->changed( dlg, (int)(ctl - dlg->ctl) );
        }
        return 1;
    case K_DEL:
        if ( ctl->fresh ) {
            ctl->buf[0] = 0;
            ctl->caret = 0;
        } else if ( ctl->caret < len ) {
            mem_cpy( ctl->buf + ctl->caret, ctl->buf + ctl->caret + 1, (u32)(len - ctl->caret) );
        }
        ctl->fresh = 0;
        if ( dlg->changed ) {
            dlg->changed( dlg, (int)(ctl - dlg->ctl) );
        }
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
        if ( dlg->changed ) {
            dlg->changed( dlg, (int)(ctl - dlg->ctl) );
        }
        return 1;
    }
    return 0;
}

static int list_key( DLG *dlg, CTL *ctl, int key )
{
    int rows = ctl->height - 2, old = ctl->sel, index, letter;

    switch ( key ) {
    case K_UP:   ctl->sel--;         break;
    case K_DOWN: ctl->sel++;         break;
    case K_PGUP: ctl->sel -= rows - 1; break;
    case K_PGDN: ctl->sel += rows - 1; break;
    case K_HOME: ctl->sel = 0;       break;
    case K_END:  ctl->sel = ctl->count - 1; break;
    default:
        if ( key < ' ' || key >= 0x100 || ctl->count == 0 ) {
            return 0;
        }
        /* a letter: the next entry that starts with it */
        letter = ch_upper( key );
        for ( index = 1; index <= ctl->count; index++ ) {
            int at = (ctl->sel + index) % ctl->count;
            const char *text = ctl->item( dlg, at );
            if ( ch_upper( text[0] ) == letter
                 || (text[0] == '[' && ch_upper( text[1] ) == letter) ) {
                ctl->sel = at;
                break;
            }
        }
        break;
    }
    if ( ctl->sel >= ctl->count ) {
        ctl->sel = ctl->count - 1;
    }
    if ( ctl->sel < 0 ) {
        ctl->sel = 0;
    }
    if ( ctl->sel != old && dlg->changed ) {
        dlg->changed( dlg, (int)(ctl - dlg->ctl) );
    }
    return 1;
}

static int radio_step( DLG *dlg, CTL *ctl, int step )
{
    int index = (int)(ctl - dlg->ctl) + step;

    if ( index >= 0 && index < dlg->nctl && dlg->ctl[index].type == CT_RADIO
         && dlg->ctl[index].value == ctl->value ) {
        *ctl->value = dlg->ctl[index].id;
        dlg->focus = index;
        if ( dlg->changed ) {
            dlg->changed( dlg, index );
        }
    }
    return 1;
}

/* a button pressed: what the dialog answers, or 0 to stay open */
static int press( DLG *dlg, int index )
{
    int answer;

    if ( dlg->ctl[index].id == ID_HELP ) {
        help_show( dlg->help );
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
        answer = dlg->activate( dlg, dlg->focus );
        if ( answer || default_button( dlg ) < 0 ) {
            return answer;
        }
    }
    index = default_button( dlg );
    return index >= 0 ? press( dlg, index ) : 0;
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
        switch ( ctl->type ) {
        case CT_LIST:
            if ( row >= top && row < top + ctl->height && col >= left && col < left + ctl->width ) {
                return index;
            }
            continue;
        case CT_EDIT:
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

/* a press on a list: its arrows scroll it, an entry is chosen, and a
   double press on one is Enter */
static int list_mouse( DLG *dlg, CTL *ctl, int dbl )
{
    int top = dlg->top + ctl->row, left = dlg->left + ctl->col;
    int rows = ctl->height - 2, item;

    if ( mouse.col == left + ctl->width - 1 && ctl->count > rows ) {
        if ( mouse.row == top + 1 ) {
            list_key( dlg, ctl, K_UP );
        } else if ( mouse.row == top + rows ) {
            list_key( dlg, ctl, K_DOWN );
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
    /* told even when it was chosen already: a click is a choice, and the
       Open dialog, for one, answers it by filling in the name */
    if ( dlg->changed ) {
        dlg->changed( dlg, (int)(ctl - dlg->ctl) );
    }
    return dbl ? enter_key( dlg ) : 0;
}

/* the mouse in a dialog: the answer, when it has one.  "armed" is the
   button a press went down on - it is pressed when the button is let go
   over it, as a button is */
static int dlg_mouse( DLG *dlg, int *armed )
{
    int index = ctl_at( dlg, mouse.row, mouse.col ), target, was;
    CTL *ctl;

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
        for ( target = index + 1; target < dlg->nctl && !focusable( &dlg->ctl[target] ); target++ ) {
        }
        if ( target == dlg->nctl ) {
            return 0;
        }
        dlg->focus = target;
        if ( dlg->ctl[target].type == CT_EDIT ) {
            dlg->ctl[target].fresh = 1;
            dlg->ctl[target].caret = (int)str_len( dlg->ctl[target].buf );
        }
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
        if ( dlg->changed ) {
            dlg->changed( dlg, index );
        }
        break;
    case CT_RADIO:
        *ctl->value = ctl->id;
        if ( dlg->changed ) {
            dlg->changed( dlg, index );
        }
        break;
    case CT_LIST:
        return list_mouse( dlg, ctl, mouse.kind == ME_DOUBLE );
    case CT_BUTTON:
        *armed = index;
        break;
    }
    return 0;
}

int dlg_run( DLG *dlg )
{
    int key, answer = 0, index, letter, armed = -1;
    CTL *ctl;

    if ( dlg->top == 0 ) {
        dlg->top = (scr_rows - dlg->rows) / 2;
    }
    if ( dlg->left == 0 ) {
        dlg->left = (scr_cols - dlg->cols) / 2;
    }
    if ( !focusable( &dlg->ctl[dlg->focus] ) ) {
        dlg->focus = dlg->nctl - 1;
        move_focus( dlg, 1 );
    }
    if ( dlg->ctl[dlg->focus].type == CT_EDIT ) {
        dlg->ctl[dlg->focus].fresh = 1;
        dlg->ctl[dlg->focus].caret = (int)str_len( dlg->ctl[dlg->focus].buf );
    }
    ui_push( dlg );
    app_status( "F1=Help   Enter=Execute   Esc=Cancel   Tab=Next Field   Arrow=Next Item" );
    while ( answer == 0 ) {
        scr_cursor( 0, 0, CUR_HIDE );
        ui_redraw();
        scr_flush();
        key = key_get();
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        if ( key == K_MOUSE ) {
            answer = dlg_mouse( dlg, &armed );
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
            help_show( dlg->help );
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
        if ( ctl->type == CT_LIST && key != ' ' && list_key( dlg, ctl, key ) ) {
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
                if ( dlg->changed ) {
                    dlg->changed( dlg, dlg->focus );
                }
            } else if ( ctl->type == CT_BUTTON ) {
                answer = press( dlg, dlg->focus );
            }
            continue;
        }
        if ( ctl->type == CT_BUTTON && (key == K_LEFT || key == K_UP) ) {
            move_focus( dlg, -1 );
            continue;
        }
        if ( ctl->type == CT_BUTTON && (key == K_RIGHT || key == K_DOWN) ) {
            move_focus( dlg, 1 );
            continue;
        }
        letter = key_alt_letter( key );
        if ( letter == 0 && key < 0x100 && ctl->type != CT_EDIT && ctl->type != CT_LIST ) {
            letter = ch_upper( key );
        }
        if ( letter ) {
            index = hot_jump( dlg, letter );
            if ( index ) {
                answer = press( dlg, index - 1 );
            }
        }
    }
    ui_pop();
    app_status( NULL );
    return answer;
}

/* ------------------------------------------------------------------ */
/* message boxes                                                       */
/* ------------------------------------------------------------------ */

int msg_box2( const char *line1, const char *line2, int buttons )
{
    static char text[200];
    CTL ctl[4];
    DLG dlg;
    int width, len, nbtn = 0, index, col, total = 0;
    static const char *labels[4];
    static int ids[4];

    str_cpyn( text, line1, sizeof( text ) );
    if ( line2 && *line2 ) {
        str_catn( text, "\n", sizeof( text ) );
        str_catn( text, line2, sizeof( text ) );
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
        labels[0] = "OK";     ids[0] = ID_OK;
        nbtn = 1;
        break;
    }
    width = (int)str_len( line1 );
    len = line2 ? (int)str_len( line2 ) : 0;
    if ( len > width ) {
        width = len;
    }
    for ( index = 0; index < nbtn; index++ ) {
        total += scr_hotlen( labels[index] ) + 6;
    }
    if ( total > width ) {
        width = total;
    }
    width += 6;
    if ( width > scr_cols - 4 ) {
        width = scr_cols - 4;
    }
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_TEXT;
    ctl[0].row = 2;
    ctl[0].col = 1;
    ctl[0].width = width - 2;
    ctl[0].text = text;
    col = (width - total) / 2 + 1;
    for ( index = 0; index < nbtn; index++ ) {
        ctl[1 + index].type = CT_BUTTON;
        ctl[1 + index].row = line2 && *line2 ? 5 : 4;
        ctl[1 + index].col = col;
        ctl[1 + index].text = labels[index];
        ctl[1 + index].id = ids[index];
        ctl[1 + index].is_default = index == 0;
        col += scr_hotlen( labels[index] ) + 6;
    }
    dlg.rows = line2 && *line2 ? 7 : 6;
    dlg.cols = width;
    dlg.ctl = ctl;
    dlg.nctl = 1 + nbtn;
    dlg.focus = 1;
    dlg.help = HELP_INDEX;
    return dlg_run( &dlg );
}

int msg_box( const char *text, int buttons )
{
    return msg_box2( text, NULL, buttons );
}
