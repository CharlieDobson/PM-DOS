/*
 * UI.C - the menu bar, its menus, dialog boxes and message boxes.
 *
 * EDIT'S MACHINERY, with the shell's menus in it.  Nothing here
 * remembers what it covered: a dialog is drawn over the whole screen as
 * the rest of the program draws it (app_draw), with any dialogs under
 * it drawn first - ui_push keeps that stack - and the screen layer
 * sends only what changed.
 *
 * THE MENU BAR HAS TWO SETS OF MENUS, as the MS-DOS Shell's has: one
 * for when the keys are among files and directories (File, Options,
 * View, Tree, Help) and one for when they are in the list of programs,
 * where File means something else and there is no Tree.
 *
 * A DIALOG IS A TABLE OF CONTROLS: labels, entry fields, check boxes,
 * option buttons, lists and buttons, each at a row and column inside
 * the box.  Tab and Shift+Tab walk them; Alt and a letter (or just the
 * letter, when the focus is not somewhere that takes letters) jumps to
 * the one whose label has that access key; Enter presses the default
 * button; Esc is Cancel; F1 is help.  The mouse presses what it lands
 * on - a button when it is let go over it - and a double click in a
 * list is Enter.  Two hooks let a dialog react: "changed" when a
 * control's value moves, and "activate" for Enter on a list and for a
 * button - which returns the dialog's answer, or 0 to stay open.
 *
 * ON A GRAPHICS SCREEN the same tables are drawn with a little more:
 * a button is a box and not a pair of angle brackets, a check box and
 * an option button are icons, and an access key is underlined.
 */
#include "shell.h"

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
    { "&Open",               "Enter",  CMD_OPEN,      "Starts a program, or opens a file with its program" },
    { "&Run...",             "",       CMD_RUN,       "Runs a command you type" },
    { "&Print",              "",       CMD_PRINT,     "Sends the selected files to the printer" },
    { "Associ&ate...",       "",       CMD_ASSOCIATE, "Ties a kind of file to the program that opens it" },
    { "Searc&h...",          "",       CMD_SEARCH,    "Looks for files by name" },
    { "&View File Contents", "F9",     CMD_VIEWFILE,  "Shows what is in the file" },
    { NULL,                  NULL,     0,             NULL },
    { "&Move...",            "F7",     CMD_MOVE,      "Moves the selected files to another directory" },
    { "&Copy...",            "F8",     CMD_COPY,      "Copies the selected files" },
    { "&Delete...",          "Del",    CMD_DELETE,    "Deletes the selected files, or an empty directory" },
    { "Re&name...",          "",       CMD_RENAME,    "Gives a file or a directory another name" },
    { "Chan&ge Attributes...", "",     CMD_ATTRIB,    "Sets the hidden, system, archive and read-only marks" },
    { NULL,                  NULL,     0,             NULL },
    { "Cr&eate Directory...", "",      CMD_MKDIR,     "Makes a directory inside the selected one" },
    { NULL,                  NULL,     0,             NULL },
    { "&Select All",         "Ctrl+/", CMD_SELALL,    "Selects every file in the list" },
    { "Dese&lect All",       "Ctrl+\\", CMD_DESELALL, "Selects none of them" },
    { NULL,                  NULL,     0,             NULL },
    { "E&xit",               "Alt+F4", CMD_EXIT,      "Leaves the shell" }
};

static const MITEM pfile_items[] = {
    { "&New...",        "",       CMD_PNEW,     "Adds a program or a group to this group" },
    { "&Open",          "Enter",  CMD_POPEN,    "Starts the program, or opens the group" },
    { "&Copy",          "",       CMD_PCOPY,    "Copies the program into another group" },
    { "&Delete...",     "Del",    CMD_PDELETE,  "Takes the program or the empty group off the list" },
    { "&Properties...", "",       CMD_PPROPS,   "The title, the commands and the rest" },
    { "&Reorder",       "",       CMD_PREORDER, "Moves the entry to another place in the list" },
    { NULL,             NULL,     0,            NULL },
    { "R&un...",        "",       CMD_RUN,      "Runs a command you type" },
    { NULL,             NULL,     0,            NULL },
    { "E&xit",          "Alt+F4", CMD_EXIT,     "Leaves the shell" }
};

static const MITEM options_items[] = {
    { "&Confirmation...",          "", CMD_CONFIRM,  "What the shell asks about before it acts" },
    { "&File Display Options...",  "", CMD_FILEOPTS, "Which files are listed, and in what order" },
    { "&Select Across Directories", "", CMD_ACROSS,  "Keeps files selected when another directory is shown" },
    { "Show &Information...",      "", CMD_INFO,     "The file, the selection, the directory and the disk" },
    { NULL,                        NULL, 0,          NULL },
    { "&Display...",               "", CMD_DISPLAY,  "Text or graphics, and how many lines" },
    { "C&olors...",                "", CMD_COLORS,   "The colour scheme" }
};

static const MITEM view_items[] = {
    { "&Single File List",   "",         CMD_VSINGLE,   "One drive's directories and files" },
    { "&Dual File Lists",    "",         CMD_VDUAL,     "Two of them, one above the other" },
    { "&All Files",          "",         CMD_VALL,      "Every file on the drive in one list" },
    { "Program/&File Lists", "",         CMD_VPROGFILE, "Files above, programs below" },
    { "&Program List",       "",         CMD_VPROGS,    "The programs alone" },
    { NULL,                  NULL,       0,             NULL },
    { "&Repaint Screen",     "Shift+F5", CMD_REPAINT,   "Draws the screen again" },
    { "R&efresh",            "F5",       CMD_REFRESH,   "Reads the disk again" }
};

static const MITEM tree_items[] = {
    { "E&xpand One Level", "+",      CMD_EXPAND1,   "Shows the directories inside the selected one" },
    { "Expand &Branch",    "*",      CMD_EXPANDBR,  "Shows every directory below the selected one" },
    { "Expand &All",       "Ctrl+*", CMD_EXPANDALL, "Shows every directory on the drive" },
    { "&Collapse Branch",  "-",      CMD_COLLAPSE,  "Hides the directories inside the selected one" }
};

static const MITEM help_items[] = {
    { "&Index",        "", CMD_HINDEX,    "The list of help subjects" },
    { "&Keyboard",     "", CMD_HKEYS,     "What the keys do" },
    { "S&hell Basics", "", CMD_HBASICS,   "The parts of the screen and how to move among them" },
    { "&Commands",     "", CMD_HCOMMANDS, "What each menu command does" },
    { "&Procedures",   "", CMD_HPROCS,    "How to do the usual things" },
    { "&Using Help",   "", CMD_HUSING,    "How help itself works" },
    { NULL,            NULL, 0,           NULL },
    { "&About Shell",  "", CMD_ABOUT,     "The shell's version" }
};

typedef struct {
    const char  *title;
    const MITEM *items;
    int          count;
} MENU;

#define COUNT( table )  (int)(sizeof( table ) / sizeof( MITEM ))

static const MENU file_menus[] = {
    { "&File",    file_items,    COUNT( file_items ) },
    { "&Options", options_items, COUNT( options_items ) },
    { "&View",    view_items,    COUNT( view_items ) },
    { "&Tree",    tree_items,    COUNT( tree_items ) },
    { "&Help",    help_items,    COUNT( help_items ) }
};

static const MENU prog_menus[] = {
    { "&File",    pfile_items,   COUNT( pfile_items ) },
    { "&Options", options_items, COUNT( options_items ) },
    { "&View",    view_items,    COUNT( view_items ) },
    { "&Help",    help_items,    COUNT( help_items ) }
};

static const MENU *menus;
static int nmenus;

static void menus_pick( void )
{
    if ( app_files_menu() ) {
        menus = file_menus;
        nmenus = 5;
    } else {
        menus = prog_menus;
        nmenus = 4;
    }
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

    menus_pick();
    scr_fill( MENU_ROW, 0, scr_cols, ' ', pal.menu );
    for ( index = 0; index < nmenus; index++ ) {
        col = menu_col( index );
        if ( index == selected ) {
            scr_fill( MENU_ROW, col - 1, scr_hotlen( menus[index].title ) + 2, ' ', pal.menu_sel );
            scr_hot( MENU_ROW, col, menus[index].title, pal.menu_sel,
                     show_hot ? pal.menu_sel_hot : pal.menu_sel );
        } else {
            scr_hot( MENU_ROW, col, menus[index].title, pal.menu,
                     show_hot ? pal.menu_hot : pal.menu );
        }
    }
}

/* text, a gap, the keys - and two cells at the left for a mark */
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
    return width + 6;
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

    for ( index = 0; index < nmenus; index++ ) {
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
    const MITEM *items = menus[menu].items;
    int count = menus[menu].count;
    int width = menu_width( items, count );
    int left = menu_left( menu, width ), first = MENU_ROW + 2;

    if ( row < first || row >= first + count || col <= left || col >= left + width - 1 ) {
        return -1;
    }
    return items[row - first].text ? row - first : -1;
}

static void menu_draw_pull( int menu, int sel )
{
    const MITEM *items = menus[menu].items;
    int count = menus[menu].count;
    int width = menu_width( items, count );
    int left = menu_left( menu, width ), index, row, top = MENU_ROW + 1;
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
            scr_ch( row, left + 2, 0x07, attr );
        }
        scr_hot( row, left + 4, items[index].text, attr, hot );
        if ( items[index].keys[0] ) {
            scr_put( row, left + width - 2 - (int)str_len( items[index].keys ),
                     items[index].keys, attr );
        }
    }
    scr_shadow( top, left, top + count + 1, left + width - 1 );
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

    menus_pick();
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
    sel = first_item( items, count, count - 1, 1 );
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
            menu_draw_pull( menu, sel );
        }
        scr_cursor( 0, 0, CUR_HIDE );
        scr_flush();
        key = key_get();
        if ( key_service( key ) ) {
            continue;
        }
        if ( key == K_MOUSE ) {
            index = mouse.row == MENU_ROW ? menu_at( mouse.col ) : -1;
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
            menu = (menu + nmenus - 1) % nmenus;
            sel = first_item( menus[menu].items, menus[menu].count, menus[menu].count - 1, 1 );
            continue;
        case K_RIGHT:
            menu = (menu + 1) % nmenus;
            sel = first_item( menus[menu].items, menus[menu].count, menus[menu].count - 1, 1 );
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
                sel = first_item( menus[menu].items, menus[menu].count,
                                  menus[menu].count - 1, 1 );
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

/* a box with its title in a bar across the top, and its shadow */
void dlg_frame( int top, int left, int rows, int cols, const char *title )
{
    int right = left + cols - 1, len;

    scr_box( top, left, top + rows - 1, right, pal.dlg );
    scr_shadow( top, left, top + rows - 1, right );
    if ( title ) {
        len = (int)str_len( title );
        scr_fill( top, left, cols, ' ', pal.dlg_title );
        scr_put( top, left + (cols - len) / 2, title, pal.dlg_title );
        scr_flag( top, left, cols, CF_GRAD | scr_bar_edges() );
        scr_flag( top, left, 1, CF_LEFT );
        scr_flag( top, right, 1, CF_RIGHT );
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
    scr_fill( row, col, ctl->width, ' ', pal.field );
    if ( len > ctl->scroll ) {
        shown = len - ctl->scroll < ctl->width ? len - ctl->scroll : ctl->width;
        attr = focus && ctl->fresh && len ? pal.field_sel : pal.field;
        if ( ctl->hidden ) {
            for ( pos = 0; pos < shown; pos++ ) {
                scr_ch( row, col + pos, '*', attr );
            }
        } else {
            scr_putn( row, col, ctl->buf + ctl->scroll, shown, attr );
        }
    }
    scr_flag( row, col, ctl->width, CF_TOP | CF_BOTTOM );
    scr_flag( row, col, 1, CF_LEFT );
    scr_flag( row, col + ctl->width - 1, 1, CF_RIGHT );
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
    for ( index = 0; index < rows; index++ ) {
        item = ctl->top + index;
        scr_fill( top + 1 + index, left + 1, ctl->width - 2, ' ', pal.dlg );
        if ( item < ctl->count ) {
            text = ctl->item( dlg, item );
            scr_putn( top + 1 + index, left + 2, text,
                      (int)str_len( text ) < ctl->width - 3 ? (int)str_len( text ) : ctl->width - 3,
                      item == ctl->sel ? pal.select : pal.dlg );
            if ( item == ctl->sel ) {
                scr_recolor( top + 1 + index, left + 1, ctl->width - 2, pal.select );
                if ( focus ) {
                    scr_cursor( top + 1 + index, left + 2, CUR_LINE );
                }
            }
        }
    }
    if ( ctl->count > rows ) {
        sbar_draw( top + 1, top + rows, left + ctl->width - 1, ctl->top, rows, ctl->count );
    }
    if ( ctl->text ) {
        scr_hot( top, left + 2, ctl->text, pal.dlg, pal.dlg_hot );
    }
}

static void draw_button( int row, int col, const char *text, int focus )
{
    u8 attr = focus ? pal.button_focus : pal.button;
    int len = scr_hotlen( text );

    scr_ch( row, col, gfx_on ? ' ' : '<', attr );
    scr_ch( row, col + 1, ' ', attr );
    scr_hot( row, col + 2, text, attr, focus ? attr : pal.dlg_hot );
    scr_ch( row, col + 2 + len, ' ', attr );
    scr_ch( row, col + 3 + len, gfx_on ? ' ' : '>', attr );
    scr_flag( row, col, len + 4, CF_TOP | CF_BOTTOM );
    scr_flag( row, col, 1, CF_LEFT );
    scr_flag( row, col + 3 + len, 1, CF_RIGHT );
    if ( focus ) {
        scr_cursor( row, col + 2, gfx_on ? CUR_HIDE : CUR_LINE );
    }
}

/* a check box or an option button: three characters and a blank, or an
   icon as wide */
static void draw_mark( int row, int col, int on, int radio, int focus )
{
    if ( gfx_on ) {
        scr_icon( row, col, radio ? (on ? IC_RADIO_ON : IC_RADIO_OFF)
                                  : (on ? IC_CHECK_ON : IC_CHECK_OFF), pal.dlg );
        scr_fill( row, col + 2, 2, ' ', pal.dlg );
    } else {
        scr_put( row, col, radio ? (on ? "(\x07)" : "( )") : (on ? "[X]" : "[ ]"), pal.dlg );
    }
    if ( focus ) {
        scr_cursor( row, col + 1, gfx_on ? CUR_HIDE : CUR_LINE );
    }
}

void dlg_draw( DLG *dlg )
{
    int index, row, col, len, focus;
    CTL *ctl;
    const char *text, *end;

    dlg_frame( dlg->top, dlg->left, dlg->rows, dlg->cols, dlg->title );
    for ( index = 0; index < dlg->nctl; index++ ) {
        ctl = &dlg->ctl[index];
        row = dlg->top + ctl->row;
        col = dlg->left + ctl->col;
        focus = index == dlg->focus;
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
            draw_edit( dlg, ctl, focus );
            break;
        case CT_CHECK:
            draw_mark( row, col, *ctl->value, 0, focus );
            scr_hot( row, col + 4, ctl->text, pal.dlg, pal.dlg_hot );
            if ( focus && gfx_on ) {
                scr_flag( row, col + 4, scr_hotlen( ctl->text ), CF_BOTTOM );
            }
            break;
        case CT_RADIO:
            draw_mark( row, col, *ctl->value == ctl->id, 1, focus );
            scr_hot( row, col + 4, ctl->text, pal.dlg, pal.dlg_hot );
            if ( focus && gfx_on ) {
                scr_flag( row, col + 4, scr_hotlen( ctl->text ), CF_BOTTOM );
            }
            break;
        case CT_LIST:
            draw_list( dlg, ctl, focus );
            break;
        case CT_BUTTON:
            draw_button( row, col, ctl->text, focus );
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

    switch ( key & ~K_SHIFT ) {
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
            while ( *text == ' ' || *text == '[' ) {
                text++;
            }
            if ( ch_upper( text[0] ) == letter ) {
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

/* a press on a list: its scroll bar scrolls it, an entry is chosen, and
   a double press on one is Enter */
static int list_mouse( DLG *dlg, CTL *ctl, int dbl )
{
    int top = dlg->top + ctl->row, left = dlg->left + ctl->col;
    int rows = ctl->height - 2, item;

    if ( mouse.col == left + ctl->width - 1 && ctl->count > rows ) {
        switch ( sbar_hit( top + 1, top + rows, mouse.col, ctl->top, rows, ctl->count, mouse.row ) ) {
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
        if ( dlg->top < MENU_ROW + 1 ) {
            dlg->top = scr_rows > dlg->rows ? 1 : 0;
        }
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
    app_status( "F1=Help   Enter=OK   Esc=Cancel   Tab=Next Field" );
    while ( answer == 0 ) {
        scr_cursor( 0, 0, CUR_HIDE );
        ui_redraw();
        scr_flush();
        key = key_get();
        if ( key_service( key ) ) {
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

void ctl_button( CTL *ctl, int row, int col, const char *text, int id, int is_default )
{
    ctl_at_cell( ctl, CT_BUTTON, row, col, text );
    ctl->id = id;
    ctl->is_default = is_default;
}

/* ------------------------------------------------------------------ */
/* message boxes                                                       */
/* ------------------------------------------------------------------ */

/* lines of text, a "\n" between them, over a row of buttons */
int msg_text( const char *title, const char *text, int buttons, int help )
{
    CTL ctl[4];
    DLG dlg;
    int width = 0, len, nbtn = 0, index, col, total = 0, lines = 0;
    const char *labels[3], *cur, *end;
    int ids[3];

    for ( cur = text; *cur; cur += len + (end ? 1 : 0) ) {
        end = str_chr( cur, '\n' );
        len = end ? (int)(end - cur) : (int)str_len( cur );
        if ( len > width ) {
            width = len;
        }
        lines++;
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
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_text( &ctl[0], 2, 1, width - 2, text );
    col = (width - total) / 2 + 1;
    for ( index = 0; index < nbtn; index++ ) {
        ctl_button( &ctl[1 + index], lines + 3, col, labels[index], ids[index], index == 0 );
        col += scr_hotlen( labels[index] ) + 6;
    }
    dlg.title = title;
    dlg.rows = lines + 5;
    dlg.cols = width;
    dlg.ctl = ctl;
    dlg.nctl = 1 + nbtn;
    dlg.focus = 1;
    dlg.help = help;
    return dlg_run( &dlg );
}

int msg_box2( const char *line1, const char *line2, int buttons )
{
    char text[240];

    str_fit( text, line1, sizeof( text ) );
    if ( line2 && *line2 ) {
        str_catn( text, "\n", sizeof( text ) );
        str_catn( text, line2, sizeof( text ) );
    }
    return msg_text( "PM-DOS Shell", text, buttons, HELP_DIALOGS );
}

int msg_box( const char *text, int buttons )
{
    return msg_box2( text, NULL, buttons );
}

void msg_error( const char *what, int rc )
{
    msg_box2( what, err_text( rc ), MB_OK );
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
    dlg_frame( top, left, 6, width, "PM-DOS Shell" );
    scr_putw( top + 2, left + 3, line1, width - 6, pal.dlg );
    if ( line2 ) {
        scr_putw( top + 3, left + 3, line2, width - 6, pal.dlg );
    }
    scr_cursor( 0, 0, CUR_HIDE );
    scr_flush();
}
