/*
 * IDE_UI.C - the menu bar, its menus, dialog boxes and message boxes.
 *
 * EDIT's UI.C (see it for the machinery: nothing remembers what it
 * covered, a dialog is a table of controls, two hooks let a dialog
 * react), laid out and coloured as QBasic's are:
 *
 *   The bar's titles start at column 2, one blank either side of each,
 *   and Help sits at the right.  Its access letters show only while Alt
 *   is held; after a tap of Alt (or F10) the first title is lit.  A
 *   dropped menu covers the window's frame, and the item lit in it is
 *   described on the status line; F1 there is help on the item.
 *
 *   A dialog's buttons are "< OK >" with the brackets bright on the one
 *   Enter would press; an entry field has a box of its own; a list's
 *   scroll bar stands in its right-hand border.  Access letters show
 *   while Alt is held, as on the bar.
 *
 * The words on the status line are this program's own.
 */
#include "ide.h"

/* ------------------------------------------------------------------ */
/* the menus                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *text;           /* NULL: a separator */
    const char *keys;
    int         cmd;
    const char *hint;
    const char *help;           /* F1 on it */
} MITEM;

static const MITEM file_items[] = {
    { "&New",        "", CMD_NEW,    "Clears the program away for a new one", "m.new" },
    { "&Open...",    "", CMD_OPEN,   "Loads a program from a disk", "m.open" },
    { "&Save",       "", CMD_SAVE,   "Writes the program to its file", "m.save" },
    { "Save &As...", "", CMD_SAVEAS, "Writes the program under a name you give", "m.saveas" },
    { NULL,          NULL, 0, NULL, NULL },
    { "&Print...",   "", CMD_PRINT,  "Prints the program, the window or the selection", "m.print" },
    { NULL,          NULL, 0, NULL, NULL },
    { "E&xit",       "", CMD_EXIT,   "Leaves QBasic", "m.exit" }
};

static const MITEM edit_items[] = {
    { "Cu&t",             "Shift+Del", CMD_CUT,   "Moves the selected text to the clipboard", "m.cut" },
    { "&Copy",            "Ctrl+Ins",  CMD_COPY,  "Copies the selected text to the clipboard", "m.copy" },
    { "&Paste",           "Shift+Ins", CMD_PASTE, "Puts the clipboard's text in at the cursor", "m.paste" },
    { "Cl&ear",           "Del",       CMD_CLEAR, "Deletes the selected text", "m.clear" },
    { NULL,               NULL, 0, NULL, NULL },
    { "New &SUB...",      "", CMD_NEWSUB,  "Starts a new SUB procedure", "m.newsub" },
    { "New &FUNCTION...", "", CMD_NEWFUNC, "Starts a new FUNCTION procedure", "m.newfunc" }
};

static const MITEM view_items[] = {
    { "&SUBs...",        "F2", CMD_SUBS,   "Shows a procedure, or the module, in the window", "m.subs" },
    { "S&plit",          "",   CMD_SPLIT,  "Two windows onto the program, or back to one", "m.split" },
    { "&Output Screen",  "F4", CMD_OUTPUT, "Shows the screen the program writes to", "m.output" }
};

static const MITEM search_items[] = {
    { "&Find...",          "",   CMD_FIND,   "Looks for some text", "m.find" },
    { "&Repeat Last Find", "F3", CMD_REPEAT, "Looks for the same text again", "m.repeat" },
    { "&Change...",        "",   CMD_CHANGE, "Looks for some text and puts other text in its place", "m.change" }
};

static const MITEM run_items[] = {
    { "&Start",    "Shift+F5", CMD_START,    "Runs the program from the beginning", "m.start" },
    { "&Restart",  "",         CMD_RESTART,  "Sets the program back to its first statement", "m.restart" },
    { "&Continue", "F5",       CMD_CONTINUE, "Runs on from where the program stopped", "m.continue" },
    { NULL,        NULL, 0, NULL, NULL },
    { "Make E&XE File...", "", CMD_MAKEEXE,  "Writes the program as an .EXE that runs without QBasic", "m.makeexe" }
};

static const MITEM debug_items[] = {
    { "&Step",                  "F8",  CMD_STEP,       "Runs the next statement, into a procedure too", "m.step" },
    { "&Procedure Step",        "F10", CMD_PSTEP,      "Runs the next statement, a procedure call whole", "m.pstep" },
    { NULL,                     NULL, 0, NULL, NULL },
    { "&Trace On",              "",    CMD_TRACE,      "Lights each statement as it runs", "m.trace" },
    { NULL,                     NULL, 0, NULL, NULL },
    { "Toggle &Breakpoint",     "F9",  CMD_BREAKPOINT, "Sets or clears a stop on the cursor's line", "m.breakpoint" },
    { "&Clear All Breakpoints", "",    CMD_CLEARBP,    "Takes every breakpoint away", "m.clearbp" },
    { "Set &Next Statement",    "",    CMD_SETNEXT,    "Makes the cursor's statement the next to run", "m.setnext" }
};

static const MITEM options_items[] = {
    { "&Display...",     "", CMD_DISPLAY,  "The colours, the scroll bars and the tab stops", "m.display" },
    { "Help &Path...",   "", CMD_HELPPATH, "Where QBASIC.HLP is kept", "m.helppath" },
    { "&Syntax Checking", "", CMD_SYNTAX,  "Each line checked as the cursor leaves it, or not", "m.syntax" }
};

static char topic_text[48] = "&Topic:";

static MITEM help_items[] = {
    { "&Index",      "",         CMD_INDEX,     "Every keyword, A to Z", "m.index" },
    { "&Contents",   "",         CMD_CONTENTS,  "What the help holds, in groups", "m.contents" },
    { topic_text,    "F1",       CMD_TOPIC,     "Help on the keyword the cursor is on", "m.topic" },
    { "Using &Help", "Shift+F1", CMD_USINGHELP, "How the help works", "m.usinghelp" },
    { NULL,          NULL, 0, NULL, NULL },
    { "&About...",   "",         CMD_ABOUT,     "QBasic's version", "m.about" }
};

typedef struct {
    const char  *title;
    const MITEM *items;
    int          count;
    int          width;         /* the box, borders and all */
} MENU;

static const MENU menus[] = {
    { "&File",    file_items,    sizeof( file_items ) / sizeof( MITEM ),    18 },
    { "&Edit",    edit_items,    sizeof( edit_items ) / sizeof( MITEM ),    24 },
    { "&View",    view_items,    sizeof( view_items ) / sizeof( MITEM ),    25 },
    { "&Search",  search_items,  sizeof( search_items ) / sizeof( MITEM ),  28 },
    { "&Run",     run_items,     sizeof( run_items ) / sizeof( MITEM ),     23 },
    { "&Debug",   debug_items,   sizeof( debug_items ) / sizeof( MITEM ),   31 },
    { "&Options", options_items, sizeof( options_items ) / sizeof( MITEM ), 19 },
    { "&Help",    help_items,    sizeof( help_items ) / sizeof( MITEM ),    29 }
};
#define NMENUS  (int)(sizeof( menus ) / sizeof( MENU ))
#define HELP_MENU   (NMENUS - 1)

/* where a title's blank-word-blank starts on the bar */
static int menu_col( int menu )
{
    int col = 2, index;

    if ( menu == HELP_MENU ) {
        return scr_cols - 7;
    }
    for ( index = 0; index < menu; index++ ) {
        col += scr_hotlen( menus[index].title ) + 2;
    }
    return col;
}

/* selected: the lit title (-1 none); show_hot: the letters; lit: the
   menu below it is open, and its title is lit whole */
void menu_draw_bar( int selected, int show_hot, int lit )
{
    int index, col, len;

    scr_fill( 0, 0, scr_cols, ' ', pal.menu );
    for ( index = 0; index < NMENUS; index++ ) {
        col = menu_col( index );
        len = scr_hotlen( menus[index].title );
        if ( index == selected ) {
            scr_fill( 0, col, len + 2, ' ', pal.menu_sel );
            scr_hot( 0, col + 1, menus[index].title, pal.menu_sel, lit ? pal.menu_sel : pal.menu_sel_hot );
        } else {
            scr_hot( 0, col + 1, menus[index].title, pal.menu,
                     show_hot && !lit ? pal.menu_hot : pal.menu );
        }
    }
}

static int menu_left( int menu )
{
    if ( menu == HELP_MENU ) {
        return scr_cols - 3 - menus[menu].width + 1;
    }
    return menu_col( menu ) - 1;
}

/* the menu whose title is at "col" on the bar, or -1 */
static int menu_at( int col )
{
    int index, start;

    for ( index = 0; index < NMENUS; index++ ) {
        start = menu_col( index );
        if ( col >= start && col < start + scr_hotlen( menus[index].title ) + 2 ) {
            return index;
        }
    }
    return -1;
}

static int menu_item_at( int menu, int row, int col )
{
    int left = menu_left( menu ), width = menus[menu].width;

    if ( row < 2 || row >= 2 + menus[menu].count || col <= left || col >= left + width - 1 ) {
        return -1;
    }
    return menus[menu].items[row - 2].text ? row - 2 : -1;
}

/* Help > Topic: names the keyword under the cursor */
static void topic_update( void )
{
    char word[40];

    str_cpy( topic_text, "&Topic:" );
    if ( active < 2 && help_has_keyword( app_view() ) ) {
        view_word_at( app_view(), word, sizeof( word ) );
        str_catn( topic_text, " ", sizeof( topic_text ) );
        str_catn( topic_text, word, sizeof( topic_text ) );
    }
}

static void menu_draw_pull( int menu, int sel )
{
    const MITEM *items = menus[menu].items;
    int count = menus[menu].count, width = menus[menu].width;
    int left = menu_left( menu ), index, row, keys;
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
            hot = app_can( items[index].cmd ) ? pal.menu_sel_hot : pal.menu_sel;
        } else if ( !app_can( items[index].cmd ) ) {
            attr = pal.menu_off;
            hot = pal.menu_off;
        } else {
            attr = pal.menu;
            hot = pal.menu_hot;
        }
        scr_fill( row, left + 1, width - 2, ' ', attr );
        if ( app_checked( items[index].cmd ) ) {
            scr_ch( row, left + 1, 0x07, attr );
        }
        scr_hot( row, left + 2, items[index].text, attr, hot );
        keys = (int)str_len( items[index].keys );
        if ( keys ) {
            scr_put( row, left + width - 2 - keys, items[index].keys, attr );
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

static int next_item( const MITEM *items, int count, int from, int step )
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

static void menu_hint( const MITEM *item )
{
    static char line[100];

    if ( item == NULL ) {
        app_status( " F1=Help   Enter=Display Menu   Esc=Cancel   Arrow=Next Item" );
        return;
    }
    str_cpy( line, " F1=Help \xB3 " );
    str_catn( line, item->hint, sizeof( line ) );
    app_status( line );
}

/*
 * F10 or a tap of Alt: the bar, with the first title lit.  Alt+letter:
 * that menu, open.  K_MOUSE pressed on the bar: the menu under the
 * pointer, open, following the pointer while the button is held.
 * Returns the command, or 0.
 */
int menu_run( int key )
{
    const MITEM *items;
    int menu = 0, open = 0, sel = 0, count, letter, index, tracking = 0;

    topic_update();
    letter = key_alt_letter( key );
    if ( letter ) {
        menu = menu_by_letter( letter );
        if ( menu < 0 ) {
            return 0;
        }
        open = 1;
    }
    sel = next_item( menus[menu].items, menus[menu].count, menus[menu].count - 1, 1 );
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
        menu_hint( open && sel >= 0 ? &items[sel] : NULL );
        app_draw();
        menu_draw_bar( menu, 1, open );
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
        if ( key == K_TICK ) {
            continue;
        }
        if ( key == K_MOUSE ) {
            index = mouse.row == 0 ? menu_at( mouse.col ) : -1;
            if ( index >= 0 ) {
                if ( mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE ) {
                    if ( open && index == menu && !tracking ) {
                        app_status( NULL );
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
                    tracking = 0;
                }
                continue;
            }
            index = open ? menu_item_at( menu, mouse.row, mouse.col ) : -1;
            if ( index >= 0 ) {
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
        case K_F1:
            if ( open && sel >= 0 ) {
                help_box( items[sel].help );
            } else {
                help_box( "m.menus" );
            }
            continue;
        case K_LEFT:
            menu = (menu + NMENUS - 1) % NMENUS;
            sel = next_item( menus[menu].items, menus[menu].count, menus[menu].count - 1, 1 );
            continue;
        case K_RIGHT:
            menu = (menu + 1) % NMENUS;
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
                sel = next_item( menus[menu].items, menus[menu].count, menus[menu].count - 1, 1 );
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

/* the letters show while Alt is held */
static u8 hot_attr( void )
{
    return key_alt_held() ? pal.menu_hot : pal.menu;
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
    if ( ctl->height != 1 ) {
        scr_box( row - 1, col - 1, row + 1, col + ctl->width, pal.menu );   /* a field's own box */
    }
    scr_fill( row, col, ctl->width, ' ', pal.menu );
    if ( len > ctl->scroll ) {
        scr_putn( row, col, ctl->buf + ctl->scroll,
                  len - ctl->scroll < ctl->width ? len - ctl->scroll : ctl->width,
                  focus && ctl->fresh && len ? pal.field_sel : pal.menu );
    }
    if ( focus ) {
        scr_cursor( row, col + ctl->caret - ctl->scroll, CUR_LINE );
    }
}

/* entries in columns of "colw" cells, as the Open dialog's files are */
static int list_rows( CTL *ctl )
{
    return ctl->height - 2;
}

static int list_colw( CTL *ctl )
{
    return ctl->columns > 1 ? (ctl->width - 2) / ctl->columns : ctl->width - 2;
}

static void draw_list( DLG *dlg, CTL *ctl, int focus )
{
    int top = dlg->top + ctl->row, left = dlg->left + ctl->col;
    int rows = list_rows( ctl ), cols = ctl->columns > 1 ? ctl->columns : 1;
    int colw = list_colw( ctl ), index, item, c, len, thumb, track;
    const char *text;

    scr_box( top, left, top + ctl->height - 1, left + ctl->width - 1, pal.menu );
    if ( ctl->sel >= ctl->count ) {
        ctl->sel = ctl->count ? ctl->count - 1 : 0;
    }
    if ( ctl->sel < 0 ) {
        ctl->sel = 0;
    }
    /* the first entry shown keeps the chosen one in sight */
    if ( cols == 1 ) {
        if ( ctl->sel < ctl->top ) {
            ctl->top = ctl->sel;
        }
        if ( ctl->sel >= ctl->top + rows ) {
            ctl->top = ctl->sel - rows + 1;
        }
    } else {
        if ( ctl->sel < ctl->top ) {
            ctl->top = ctl->sel / rows * rows;
        }
        while ( ctl->sel >= ctl->top + rows * cols ) {
            ctl->top += rows;
        }
    }
    for ( c = 0; c < cols; c++ ) {
        for ( index = 0; index < rows; index++ ) {
            item = ctl->top + c * rows + index;
            if ( item >= ctl->count ) {
                continue;
            }
            text = ctl->item( dlg, item );
            len = (int)str_len( text );
            if ( len > colw - 2 ) {
                len = colw - 2;
            }
            scr_putn( top + 1 + index, left + 2 + c * colw, text, len, pal.menu );
            if ( item == ctl->sel && (focus || cols == 1 || dlg->ctl[dlg->focus].type != CT_LIST) ) {
                scr_recolor( top + 1 + index, left + 1 + c * colw, cols == 1 ? ctl->width - 2 : colw - 1,
                             pal.menu_sel );
            }
            if ( item == ctl->sel && focus ) {
                scr_cursor( top + 1 + index, left + 2 + c * colw, CUR_LINE );
            }
        }
    }
    if ( cols == 1 && ctl->count > rows ) {
        /* the scroll bar stands in the right-hand border */
        track = rows - 2;
        scr_ch( top + 1, left + ctl->width - 1, 0x18, pal.scroll );
        scr_fill( top + 1, left + ctl->width - 1, 0, 0, pal.scroll );
        for ( index = 0; index < track; index++ ) {
            scr_ch( top + 2 + index, left + ctl->width - 1, 0xB0, pal.scroll );
        }
        scr_ch( top + rows, left + ctl->width - 1, 0x19, pal.scroll );
        if ( track > 0 ) {
            thumb = ctl->count > 1 ? ctl->sel * (track - 1) / (ctl->count - 1) : 0;
            scr_ch( top + 2 + thumb, left + ctl->width - 1, ' ', pal.thumb );
        }
    } else if ( cols > 1 ) {
        /* ...and a list in columns has it in the bottom border */
        int bottom = top + ctl->height - 1, width = ctl->width - 4, pages;
        scr_ch( bottom, left + 1, 0x1B, pal.scroll );
        scr_fill( bottom, left + 2, width, 0xB0, pal.scroll );
        scr_ch( bottom, left + ctl->width - 2, 0x1A, pal.scroll );
        pages = ctl->count > 0 ? (ctl->count - 1) / rows : 0;
        thumb = pages > 0 ? (ctl->top / rows) * (width - 1) / pages : 0;
        scr_ch( bottom, left + 2 + thumb, ' ', pal.thumb );
    }
    if ( ctl->text ) {
        len = scr_hotlen( ctl->text );
        scr_hot( top - 1, left + (ctl->width - len) / 2, ctl->text, pal.menu, hot_attr() );
    }
}

static void draw_button( DLG *dlg, CTL *ctl, int index, int focus_is_button )
{
    int row = dlg->top + ctl->row, col = dlg->left + ctl->col, len, first;
    u8 bracket = pal.menu;

    if ( index == dlg->focus || (ctl->is_default && !focus_is_button) ) {
        bracket = pal.menu_hot;
    }
    len = scr_hotlen( ctl->text );
    scr_ch( row, col, '<', bracket );
    scr_hot( row, col + 1, ctl->text, pal.menu, hot_attr() );
    scr_ch( row, col + 1 + len, '>', bracket );
    if ( index == dlg->focus ) {
        for ( first = 0; ctl->text[first] == ' '; first++ ) {
        }
        scr_cursor( row, col + 1 + first, CUR_LINE );
    }
}

void dlg_draw( DLG *dlg )
{
    int index, row, col, len, focus_is_button;
    CTL *ctl;
    const char *text, *end;

    scr_box( dlg->top, dlg->left, dlg->top + dlg->rows - 1, dlg->left + dlg->cols - 1, pal.menu );
    scr_shadow( dlg->top, dlg->left, dlg->top + dlg->rows - 1, dlg->left + dlg->cols - 1 );
    if ( dlg->title ) {
        len = (int)str_len( dlg->title ) + 2;
        col = dlg->left + 1 + (dlg->cols - 2 - len) / 2;
        scr_ch( dlg->top, col, ' ', pal.menu );
        scr_put( dlg->top, col + 1, dlg->title, pal.menu );
        scr_ch( dlg->top, col + len - 1, ' ', pal.menu );
    }
    if ( dlg->sep ) {
        row = dlg->top + dlg->sep;
        scr_ch( row, dlg->left, 0xC3, pal.menu );
        scr_fill( row, dlg->left + 1, dlg->cols - 2, 0xC4, pal.menu );
        scr_ch( row, dlg->left + dlg->cols - 1, 0xB4, pal.menu );
    }
    focus_is_button = dlg->ctl[dlg->focus].type == CT_BUTTON;
    for ( index = 0; index < dlg->nctl; index++ ) {
        ctl = &dlg->ctl[index];
        row = dlg->top + ctl->row;
        col = dlg->left + ctl->col;
        switch ( ctl->type ) {
        case CT_LABEL:
            scr_hot( row, col, ctl->text, pal.menu, hot_attr() );
            break;
        case CT_TEXT:
            text = ctl->text;
            while ( *text ) {
                end = str_chr( text, '\n' );
                len = end ? (int)(end - text) : (int)str_len( text );
                scr_putn( row++, ctl->width ? col + (ctl->width - len) / 2 : col, text, len, pal.menu );
                text += len + (end ? 1 : 0);
            }
            break;
        case CT_FRAME:                          /* a box round a group, titled */
            scr_box( row, col, row + ctl->height - 1, col + ctl->width - 1, pal.menu );
            if ( ctl->text ) {
                len = (int)str_len( ctl->text ) + 2;
                scr_ch( row, col + (ctl->width - len) / 2, ' ', pal.menu );
                scr_put( row, col + (ctl->width - len) / 2 + 1, ctl->text, pal.menu );
                scr_ch( row, col + (ctl->width - len) / 2 + len - 1, ' ', pal.menu );
            }
            break;
        case CT_SAMPLE:
            scr_put( row, col, ctl->text, ctl->attr );
            break;
        case CT_EDIT:
            draw_edit( dlg, ctl, index == dlg->focus );
            break;
        case CT_CHECK:
            scr_put( row, col, *ctl->value ? "[X]" : "[ ]", pal.menu );
            scr_hot( row, col + 4, ctl->text, pal.menu, hot_attr() );
            if ( index == dlg->focus ) {
                scr_cursor( row, col + 1, CUR_LINE );
            }
            break;
        case CT_RADIO:
            scr_put( row, col, *ctl->value == ctl->id ? "(\x07)" : "( )", pal.menu );
            scr_hot( row, col + 4, ctl->text, pal.menu, hot_attr() );
            if ( index == dlg->focus ) {
                scr_cursor( row, col + 1, CUR_LINE );
            }
            break;
        case CT_LIST:
            draw_list( dlg, ctl, index == dlg->focus );
            break;
        case CT_BUTTON:
            draw_button( dlg, ctl, index, focus_is_button );
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

static int hot_jump( DLG *dlg, int letter )
{
    int index, target;
    CTL *ctl;

    for ( index = 0; index < dlg->nctl; index++ ) {
        ctl = &dlg->ctl[index];
        if ( ctl->text == NULL || ctl->type == CT_TEXT || ctl->type == CT_FRAME || ctl->type == CT_SAMPLE
             || scr_hotkey( ctl->text ) != letter ) {
            continue;
        }
        target = index;
        if ( ctl->type == CT_LABEL ) {
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
            return target + 1;
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
        if ( ctl->fresh ) {
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
    int rows = list_rows( ctl ), old = ctl->sel, index, letter;

    switch ( key ) {
    case K_UP:   ctl->sel--;         break;
    case K_DOWN: ctl->sel++;         break;
    case K_LEFT:
        if ( ctl->columns <= 1 ) {
            return 0;
        }
        ctl->sel -= rows;
        break;
    case K_RIGHT:
        if ( ctl->columns <= 1 ) {
            return 0;
        }
        ctl->sel += rows;
        break;
    case K_PGUP: ctl->sel -= rows - 1; break;
    case K_PGDN: ctl->sel += rows - 1; break;
    case K_HOME: ctl->sel = 0;       break;
    case K_END:  ctl->sel = ctl->count - 1; break;
    default:
        if ( key < ' ' || key >= 0x100 || ctl->count == 0 ) {
            return 0;
        }
        letter = ch_upper( key );
        for ( index = 1; index <= ctl->count; index++ ) {
            int at = (ctl->sel + index) % ctl->count;
            const char *text = ctl->item( dlg, at );
            while ( *text == ' ' ) {
                text++;
            }
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
    if ( (ctl->sel != old || key == K_DOWN || key == K_UP) && dlg->changed ) {
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

static int press( DLG *dlg, int index )
{
    int answer;

    if ( dlg->ctl[index].id == ID_HELP ) {
        help_box( dlg->help );
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
            width = scr_hotlen( ctl->text ) + 4;
            break;
        case CT_BUTTON:
            width = scr_hotlen( ctl->text ) + 2;
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

static int list_mouse( DLG *dlg, CTL *ctl, int dbl )
{
    int top = dlg->top + ctl->row, left = dlg->left + ctl->col;
    int rows = list_rows( ctl ), item, colw = list_colw( ctl );

    if ( ctl->columns <= 1 && mouse.col == left + ctl->width - 1 && ctl->count > rows ) {
        if ( mouse.row == top + 1 ) {
            list_key( dlg, ctl, K_UP );
        } else if ( mouse.row == top + rows ) {
            list_key( dlg, ctl, K_DOWN );
        } else if ( mouse.row > top + 1 && mouse.row < top + rows ) {
            list_key( dlg, ctl, mouse.row - top - 2 < (ctl->sel * (rows - 3)) / (ctl->count - 1)
                      ? K_PGUP : K_PGDN );
        }
        return 0;
    }
    if ( ctl->columns > 1 && mouse.row == top + ctl->height - 1 ) {
        if ( mouse.col == left + 1 ) {
            list_key( dlg, ctl, K_LEFT );
        } else if ( mouse.col == left + ctl->width - 2 ) {
            list_key( dlg, ctl, K_RIGHT );
        }
        return 0;
    }
    if ( mouse.row <= top || mouse.row > top + rows ) {
        return 0;
    }
    item = ctl->top + (mouse.row - top - 1);
    if ( ctl->columns > 1 ) {
        item += ((mouse.col - left - 1) / colw) * rows;
    }
    if ( item >= ctl->count ) {
        return 0;
    }
    ctl->sel = item;
    if ( dlg->changed ) {
        dlg->changed( dlg, (int)(ctl - dlg->ctl) );
    }
    return dbl ? enter_key( dlg ) : 0;
}

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
    if ( ctl->type == CT_LABEL ) {
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
    while ( answer == 0 ) {
        app_status_wide( " F1=Help   Enter=Execute   Esc=Cancel   Tab=Next Field   Arrow=Next Item" );
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
        if ( key == K_TICK || key == K_ALTTAP ) {
            continue;                           /* Alt down or up: the letters */
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
            help_box( dlg->help );
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

/* QBasic's: seven rows with the words in the middle one (eight with two
   lines), a line, and the buttons centred under it */
int msg_box2( const char *line1, const char *line2, int buttons, int help )
{
    /* not static: Help on the box can open a second one over it */
    char text[200];
    const char *labels[4];
    int ids[4];
    CTL ctl[5];
    DLG dlg;
    int width, len, nbtn = 0, index, col, total = 0, two = line2 && *line2;

    str_cpyt( text, line1, sizeof( text ) );
    if ( two ) {
        str_catn( text, "\n", sizeof( text ) );
        str_catn( text, line2, sizeof( text ) );
    }
    if ( buttons == MB_YESNOCANCEL ) {
        labels[0] = " &Yes ";   ids[0] = ID_YES;
        labels[1] = "  &No  ";  ids[1] = ID_NO;
        labels[2] = "Cancel";   ids[2] = ID_CANCEL;
        nbtn = 3;
    } else if ( buttons == MB_OKCANCEL ) {
        labels[0] = "  OK  ";   ids[0] = ID_OK;
        labels[1] = "Cancel";   ids[1] = ID_CANCEL;
        nbtn = 2;
    } else {
        labels[0] = "  OK  ";   ids[0] = ID_OK;
        nbtn = 1;
    }
    if ( help ) {
        labels[nbtn] = " &Help ";
        ids[nbtn] = ID_HELP;
        nbtn++;
    }
    width = (int)str_len( line1 );
    len = two ? (int)str_len( line2 ) : 0;
    if ( len > width ) {
        width = len;
    }
    width += 6;
    for ( index = 0; index < nbtn; index++ ) {
        total += scr_hotlen( labels[index] ) + 2 + (index ? 3 : 0);
    }
    if ( total + 6 > width ) {
        width = total + 6;
    }
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
    col = (width - total) / 2;
    for ( index = 0; index < nbtn; index++ ) {
        ctl[1 + index].type = CT_BUTTON;
        ctl[1 + index].row = two ? 6 : 5;
        ctl[1 + index].col = col;
        ctl[1 + index].text = labels[index];
        ctl[1 + index].id = ids[index];
        ctl[1 + index].is_default = index == 0;
        col += scr_hotlen( labels[index] ) + 5;
    }
    dlg.rows = two ? 8 : 7;
    /* off the line the cursor is on, which the message is about */
    {
        int crow, ccol, kind;
        scr_cursor_get( &crow, &ccol, &kind );
        dlg.top = (scr_rows - dlg.rows) / 2;
        if ( kind != CUR_HIDE && crow >= dlg.top && crow <= dlg.top + dlg.rows ) {
            dlg.top = crow + 2 + dlg.rows < scr_rows - 1 ? crow + 2 : crow - dlg.rows - 1;
            if ( dlg.top < 1 ) {
                dlg.top = 1;
            }
        }
    }
    dlg.cols = width;
    dlg.sep = two ? 5 : 4;
    dlg.ctl = ctl;
    dlg.nctl = 1 + nbtn;
    dlg.focus = 1;
    /* Help on an error: the topic for its message, if there is one */
    dlg.help = !help ? "m.message" : help_has( line1 ) ? line1
               : str_nicmp( line1, "Expected:", 9 ) == 0 ? "m.expected" : "m.message";
    return dlg_run( &dlg );
}

int msg_box( const char *text, int buttons, int help )
{
    return msg_box2( text, NULL, buttons, help );
}
