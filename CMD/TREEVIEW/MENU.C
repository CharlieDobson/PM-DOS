/*
 * MENU.C - the menu bar and its lists: DOSForms' MENU.CPP in C, as XARC
 * has it, with a number given to each item's action - the Drive menu's
 * items are one action and a drive each, where the toolkit's carried a
 * pointer.
 *
 * A bare Alt puts the keyboard on the bar without opening anything; the
 * arrows walk it, Enter or Down drops the list, a letter opens the menu
 * it names.  Alt+letter opens one from anywhere.  With a list down, the
 * arrows move in it and across to the next, Enter or the item's letter
 * runs it, and Esc closes the list and leaves the bar lit, so the next
 * Esc is what leaves the menu.  The mouse opens a menu with a click on
 * its name and runs an item with a click on it; a click anywhere else,
 * or the right button, closes the list.
 */
#include "treeview.h"

#define MAX_COLUMNS 8
#define MAX_ITEMS   24
#define LABEL_LEN   24

typedef struct {
    char    label[LABEL_LEN];
    char    hotkey;
    MENUACT action;
    int     arg;
} ITEM;

typedef struct {
    char label[LABEL_LEN];
    char hotkey;
    int  x, width;              /* where the name is on the bar, from 0 */
    ITEM items[MAX_ITEMS];
    int  count;
} COLUMN;

static COLUMN columns[MAX_COLUMNS];
static int    ncolumns;
static int    bar_row;
static int    active = -1;      /* the column whose list is down */
static int    bar_focus = -1;   /* the column lit with no list down */
static int    highlight;
static POPUP  dropdown;

#define MENU_FG     FG_BLACK
#define MENU_BG     BG_WHITE

int menu_add( const char *label, char hotkey )
{
    COLUMN *col;

    if ( ncolumns >= MAX_COLUMNS ) {
        return -1;
    }
    col = &columns[ncolumns];
    strncpy( col->label, label, LABEL_LEN - 1 );
    col->hotkey = hotkey;
    col->count = 0;
    col->width = (int)strlen( col->label );
    col->x = ncolumns == 0 ? 1 : columns[ncolumns - 1].x + columns[ncolumns - 1].width + 2;
    return ncolumns++;
}

int menu_item( int menu, const char *label, char hotkey, MENUACT action, int arg )
{
    COLUMN *col;
    ITEM *item;

    if ( menu < 0 || menu >= ncolumns || columns[menu].count >= MAX_ITEMS ) {
        return -1;
    }
    col = &columns[menu];
    item = &col->items[col->count];
    strncpy( item->label, label, LABEL_LEN - 1 );
    item->hotkey = hotkey;
    item->action = action;
    item->arg = arg;
    return col->count++;
}

void menu_row( int row )
{
    bar_row = row;
}

/* the lit column in reverse video, whether its list is down or the bar
   only has the keyboard - which is how a bare Alt is seen to have
   done something */
void menu_draw( void )
{
    int at, lit = active >= 0 ? active : bar_focus;

    df_clear_row( bar_row + 1, MENU_FG, MENU_BG );
    for ( at = 0; at < ncolumns; at++ ) {
        df_text( bar_row + 1, columns[at].x + 1, columns[at].label,
                 at == lit ? MENU_BG : MENU_FG, at == lit ? MENU_FG : MENU_BG );
    }
}

static void draw_items( void )
{
    char line[LABEL_LEN + 8];
    int top = dropdown.at.row, col = dropdown.at.col;
    int interior = dropdown.at.end_col - col - 1, at, ch;
    const char *label;

    if ( interior > (int)sizeof( line ) - 1 ) {
        interior = (int)sizeof( line ) - 1;
    }
    for ( at = 0; at < columns[active].count; at++ ) {
        label = columns[active].items[at].label;
        for ( ch = 0; ch < interior; ch++ ) {
            line[ch] = ' ';
        }
        line[interior] = 0;
        for ( ch = 0; label[ch] && ch + 1 < interior; ch++ ) {
            line[ch + 1] = label[ch];
        }
        df_text( top + 2 + at, col + 2, line,
                 at == highlight ? MENU_BG : MENU_FG, at == highlight ? MENU_FG : MENU_BG );
    }
}

static void menu_close( void )
{
    if ( active < 0 ) {
        return;
    }
    popup_restore( &dropdown );
    popup_free( &dropdown );
    active = -1;
    menu_draw();
}

static void menu_open( int which )
{
    COLUMN *col;
    int width = 1, at, len, top, left;

    if ( which < 0 || which >= ncolumns ) {
        return;
    }
    if ( active >= 0 ) {
        menu_close();
    }
    active = which;
    bar_focus = -1;
    highlight = 0;
    col = &columns[which];
    for ( at = 0; at < col->count; at++ ) {
        len = (int)strlen( col->items[at].label );
        if ( len > width ) {
            width = len;
        }
    }
    top = bar_row + 1;
    left = col->x;
    popup_init( &dropdown, MENU_FG, MENU_BG, NULL );
    dropdown.border = BORDER_SINGLE;
    popup_place( &dropdown, top, left, top + col->count + 1, left + width + 3 );
    menu_draw();
    popup_draw( &dropdown );
    draw_items();
}

static void focus_bar( int which )
{
    if ( ncolumns <= 0 ) {
        return;
    }
    if ( which < 0 ) {
        which = ncolumns - 1;
    }
    if ( which >= ncolumns ) {
        which = 0;
    }
    if ( active >= 0 ) {
        menu_close();
    }
    bar_focus = which;
    menu_draw();
}

static void leave_bar( void )
{
    if ( bar_focus < 0 ) {
        return;
    }
    bar_focus = -1;
    menu_draw();
}

static void run_item( int which )
{
    MENUACT action = columns[active].items[which].action;
    int arg = columns[active].items[which].arg;

    menu_close();
    if ( action ) {
        action( arg );
    }
}

/* Alt+letter: a zero and the letter's scan code */
static int alt_letter( int key )
{
    static const char row1[] = "QWERTYUIOP";
    static const char row2[] = "ASDFGHJKL";
    static const char row3[] = "ZXCVBNM";
    int scan;

    if ( (key & 0xFF00) != 0x100 ) {
        return 0;
    }
    scan = key & 0xFF;
    if ( scan >= 0x10 && scan <= 0x19 ) {
        return row1[scan - 0x10];
    }
    if ( scan >= 0x1E && scan <= 0x26 ) {
        return row2[scan - 0x1E];
    }
    if ( scan >= 0x2C && scan <= 0x32 ) {
        return row3[scan - 0x2C];
    }
    return 0;
}

static int column_for( int letter )
{
    int at;

    for ( at = 0; at < ncolumns; at++ ) {
        if ( columns[at].hotkey && toupper( letter ) == toupper( columns[at].hotkey ) ) {
            return at;
        }
    }
    return -1;
}

int menu_event( const EVENT *ev )
{
    int at, which;

    if ( ev->kind == EV_KEY ) {
        if ( ev->key == KEY_ALT ) {
            /* a second bare Alt puts the keyboard back */
            if ( active >= 0 ) {
                menu_close();
            } else if ( bar_focus >= 0 ) {
                leave_bar();
            } else {
                focus_bar( 0 );
            }
            return 1;
        }
        if ( bar_focus >= 0 && active < 0 ) {
            if ( ev->key == KEY_ESC ) {
                leave_bar();
                return 1;
            }
            if ( ev->key == KEY_LEFT ) {
                focus_bar( bar_focus - 1 );
                return 1;
            }
            if ( ev->key == KEY_RIGHT ) {
                focus_bar( bar_focus + 1 );
                return 1;
            }
            if ( ev->key == KEY_ENTER || ev->key == KEY_DOWN ) {
                menu_open( bar_focus );
                return 1;
            }
            if ( ev->key > 0 && ev->key < 0x100 && (which = column_for( ev->key )) >= 0 ) {
                menu_open( which );
                return 1;
            }
            if ( ev->key >= 0x100 && alt_letter( ev->key ) && (which = column_for( alt_letter( ev->key ) )) >= 0 ) {
                menu_open( which );
                return 1;
            }
            /* anything else gives the bar up and goes on its way */
            leave_bar();
            return 0;
        }
        if ( active >= 0 ) {
            if ( ev->key == KEY_ESC ) {
                which = active;
                menu_close();
                focus_bar( which );
                return 1;
            }
            if ( ev->key == KEY_LEFT ) {
                menu_open( active > 0 ? active - 1 : ncolumns - 1 );
                return 1;
            }
            if ( ev->key == KEY_RIGHT ) {
                menu_open( active < ncolumns - 1 ? active + 1 : 0 );
                return 1;
            }
            if ( ev->key == KEY_UP ) {
                highlight = highlight > 0 ? highlight - 1 : columns[active].count - 1;
                draw_items();
                return 1;
            }
            if ( ev->key == KEY_DOWN ) {
                highlight = highlight < columns[active].count - 1 ? highlight + 1 : 0;
                draw_items();
                return 1;
            }
            if ( ev->key == KEY_ENTER ) {
                if ( highlight >= 0 && highlight < columns[active].count ) {
                    run_item( highlight );
                }
                return 1;
            }
            if ( ev->key > 0 && ev->key < 0x100 ) {
                for ( at = 0; at < columns[active].count; at++ ) {
                    if ( columns[active].items[at].hotkey
                         && toupper( ev->key ) == toupper( columns[active].items[at].hotkey ) ) {
                        run_item( at );
                        return 1;
                    }
                }
            }
        }
        if ( ev->key >= 0x100 && alt_letter( ev->key ) && (which = column_for( alt_letter( ev->key ) )) >= 0 ) {
            if ( active == which ) {
                menu_close();
            } else {
                menu_open( which );
            }
            return 1;
        }
        return 0;
    }

    if ( ev->kind != EV_MOUSE || !ev->press ) {
        return 0;
    }
    /* the right button with a list down: close it, as a click away does */
    if ( active >= 0 && (ev->press & RIGHT_BUTTON) ) {
        menu_close();
        return 1;
    }
    /* the mouse while the bar has the keyboard: give it up, and let the
       click through to what it was aimed at */
    if ( bar_focus >= 0 && active < 0 && ev->y != bar_row ) {
        leave_bar();
    }
    if ( ev->press & LEFT_BUTTON ) {
        if ( ev->y == bar_row ) {
            for ( at = 0; at < ncolumns; at++ ) {
                if ( ev->x >= columns[at].x && ev->x < columns[at].x + columns[at].width ) {
                    if ( active == at ) {
                        menu_close();
                    } else {
                        menu_open( at );
                    }
                    return 1;
                }
            }
        }
        if ( active >= 0 ) {
            for ( at = 0; at < columns[active].count; at++ ) {
                if ( ev->y == dropdown.at.row + 1 + at && ev->x > dropdown.at.col
                     && ev->x < dropdown.at.end_col ) {
                    run_item( at );
                    return 1;
                }
            }
            menu_close();
            return 1;
        }
    }
    return 0;
}
