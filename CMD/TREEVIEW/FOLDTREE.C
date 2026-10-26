/*
 * FOLDTREE.C - the right pane: DiskStat's FOLDTREE.CPP, the folders as
 * an indented list that scrolls, with Size and % Used columns and the
 * [+]/[-] that opens and closes a folder.  The cursor's row is
 * selected_node, which the left pane draws as the yellow part of the
 * disk, so a move here draws both.
 *
 * On the graphics screen each row also gets a folder icon, in the one
 * cell every row already leaves blank between the [+]/[-] and the name -
 * so the icons cost no columns, and the text screen, which never asks
 * for them, is unchanged.
 *
 * WHERE IT DIFFERS FROM THE DOS PANE:
 *   - a name may be long, and is cut with "..." where it meets the Size
 *     column (the DOS pane cut at 12, which an 8.3 name never passed);
 *   - the Size column starts two columns further left, so that "1,023
 *     bytes", eleven characters with the comma, leaves a space before
 *     the percentage - "1023 bytes" already ran into it;
 *   - a folder deep enough to push its name past the Size column is
 *     drawn at the deepest indent that leaves the name a cell, rather
 *     than over the columns and the frame;
 *   - a scroll arrow held down with the mouse goes on scrolling, after a
 *     wait, as DOSSHELL's does.
 */
#include "treeview.h"

#define INDENT_WIDTH    2               /* cells a level */
#define REPEAT_FIRST    40              /* hundredths before a held arrow repeats */
#define REPEAT_NEXT     8               /* ...and between the repeats */

static RECT region;                     /* from 0, the frame included */
static int  top;                        /* the first row shown */

/* ------------------------------------------------------------------ */
/* folder icons                                                        */
/* ------------------------------------------------------------------ */

/* Eight pixels by sixteen: one character to the pixel, '.' is
   transparent (the row's background shows through, so an icon sits
   right on the highlighted row too) and a hex digit is an EGA colour -
   6 brown, E yellow, 8 dark grey.

   The RIGHT-HAND COLUMN IS LEFT CLEAR, as the VGA font leaves it clear
   in every letter: that column is the gap between one character and
   the next, and an icon that filled it ran straight into the folder
   name after it.  The open folder shows its dark inside above a front
   that has dropped forward - the one row allowed to reach the last
   column is the lip of it. */
static const char *const ART_FOLDER[16] = {
    "........",
    "........",
    "........",
    "66......",
    "6EE666..",
    "6EEEEE6.",
    "6EEEEE6.",
    "6EEEEE6.",
    "6EEEEE6.",
    "6EEEEE6.",
    "6EEEEE6.",
    "6EEEEE6.",
    "6666666.",
    "........",
    "........",
    "........"
};

static const char *const ART_FOLDER_OPEN[16] = {
    "........",
    "........",
    "........",
    "66......",
    "6EE666..",
    "6888886.",
    "6888886.",
    "66666666",
    "6EEEEEE6",
    "6EEEEE6.",
    "6EEEEE6.",
    "6EEEEE6.",
    "666666..",
    "........",
    "........",
    "........"
};

static int icon_closed = ICON_NONE;
static int icon_open = ICON_NONE;

/* ------------------------------------------------------------------ */
/* geometry: one place, so drawing and hit-testing agree               */
/* ------------------------------------------------------------------ */

void tree_region( int row, int col, int end_row, int end_col )
{
    region.row = row;
    region.col = col;
    region.end_row = end_row;
    region.end_col = end_col;
    if ( icon_closed == ICON_NONE ) {
        icon_closed = df_icon_register( ART_FOLDER, 16 );
        icon_open = df_icon_register( ART_FOLDER_OPEN, 16 );
    }
}

/* the frame's rows less its top and bottom and the header */
static int interior_rows( void )
{
    int height = region.end_row - region.row - 2;

    return height < 0 ? 0 : height;
}

/* 1-based columns: the name's left edge, Size, % Used, the scroll bar,
   the right-hand frame */
static void columns( int *name_left, int *size_col, int *pct_col, int *scroll_col, int *right )
{
    *right = region.end_col + 1;
    *scroll_col = *right - 1;
    *pct_col = *right - 8;              /* "% Used" / "100.0%" */
    *size_col = *pct_col - 12;          /* "1,023 bytes" and a space */
    *name_left = region.col + 3;        /* the frame and a cell of room */
}

/* how far a row is pushed in: two cells a level, as far as leaves its
   name one cell before the Size column */
static int row_indent( int idx, int name_left, int size_col )
{
    int indent = tree_nodes[idx].depth * INDENT_WIDTH;
    int most = size_col - 2 - 4 - name_left;

    if ( most < 0 ) {
        most = 0;
    }
    return indent > most ? most : indent;
}

static void clamp_view( void )
{
    int vis = interior_rows();

    if ( vis < 1 ) {
        vis = 1;
    }
    if ( tree_count <= 0 ) {
        top = 0;
        return;
    }
    if ( selected_node >= 0 ) {
        if ( selected_node < top ) {
            top = selected_node;
        }
        if ( selected_node >= top + vis ) {
            top = selected_node - vis + 1;
        }
    }
    if ( top > tree_count - vis ) {
        top = tree_count - vis;
    }
    if ( top < 0 ) {
        top = 0;
    }
}

void tree_after_scan( void )
{
    top = 0;
    clamp_view();
}

/* the left pane first: it shows what the cursor is on */
static void refresh( void )
{
    usage_draw();
    tree_draw();
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

void tree_draw( void )
{
    char blanks[134], name[134], text[40];
    int top1 = region.row + 1, left1 = region.col + 1, bot1 = region.end_row + 1;
    int name_left, size_col, pct_col, scroll_col, right1;
    int in_width, vis, at, idx, scr, fg, bg, hi, mark_col, name_col, avail;

    if ( region.end_row <= region.row || region.end_col <= region.col ) {
        return;
    }
    clamp_view();
    columns( &name_left, &size_col, &pct_col, &scroll_col, &right1 );

    draw_box( top1, left1, bot1, right1, BORDER_SINGLE, FG_BLACK, BG_WHITE );
    df_fill( top1 + 1, left1 + 1, bot1 - 1, right1 - 1, BG_WHITE );

    in_width = right1 - left1 - 1;
    if ( in_width < 1 ) {
        return;
    }

    df_text( top1 + 1, name_left, "Folder", FG_BLUE, BG_WHITE );
    df_text( top1 + 1, size_col, "Size", FG_BLUE, BG_WHITE );
    df_text( top1 + 1, pct_col, "% Used", FG_BLUE, BG_WHITE );

    if ( !data_valid ) {
        df_text( top1 + 3, name_left, "Scanning, please wait...", FG_BLACK, BG_WHITE );
        return;
    }

    vis = interior_rows();
    if ( in_width > (int)sizeof( blanks ) - 1 ) {
        in_width = (int)sizeof( blanks ) - 1;
    }
    memset( blanks, ' ', (size_t)in_width );
    blanks[in_width] = 0;

    for ( at = 0; at < vis; at++ ) {
        idx = top + at;
        scr = top1 + 2 + at;
        if ( idx >= tree_count ) {
            break;
        }
        hi = idx == selected_node;
        fg = hi ? FG_BRIGHT_WHITE : FG_BLACK;
        bg = hi ? BG_BLUE : BG_WHITE;

        df_text( scr, left1 + 1, blanks, fg, bg );

        /* the marker and the name, pushed in by the depth */
        mark_col = name_left + row_indent( idx, name_left, size_col );
        name_col = mark_col + 4;
        if ( tree_nodes[idx].flags & NODE_HAS_CHILDREN ) {
            df_text( scr, mark_col, (tree_nodes[idx].flags & NODE_EXPANDED) ? "[-]" : "[+]",
                     fg, bg );
        }
        if ( df_has_icons() && name_col - 1 < size_col ) {
            df_icon( scr, name_col - 1,
                     (tree_nodes[idx].flags & NODE_EXPANDED) ? icon_open : icon_closed, bg );
        }
        avail = size_col - name_col - 1;
        if ( avail < 1 ) {
            avail = 1;
        }
        if ( avail > (int)sizeof( name ) - 1 ) {
            avail = (int)sizeof( name ) - 1;
        }
        shorten_name( name, avail, tree_nodes[idx].name );
        df_text( scr, name_col, name, fg, bg );

        format_size( tree_nodes[idx].total_size, text );
        df_text( scr, size_col, text, fg, bg );

        format_pct( tree_nodes[idx].total_size, total_disk_space, text );
        df_text( scr, pct_col, text, fg, bg );
    }

    /* the scroll bar on the right, inside the frame, when rows are hidden */
    if ( tree_count > vis && vis > 0 ) {
        int track = vis >= 3 ? vis - 2 : vis;
        int max_top = tree_count - vis;
        int thumb = (int)((long)track * vis / tree_count);
        int thumb_at, local;
        char cell[2];

        if ( thumb < 1 ) {
            thumb = 1;
        }
        if ( thumb > track ) {
            thumb = track;
        }
        thumb_at = max_top > 0 ? (int)((long)top * (track - thumb) / max_top) : 0;
        cell[1] = 0;
        for ( at = 0; at < vis; at++ ) {
            if ( vis >= 3 && at == 0 ) {
                cell[0] = '\x18';
            } else if ( vis >= 3 && at == vis - 1 ) {
                cell[0] = '\x19';
            } else {
                local = vis >= 3 ? at - 1 : at;
                cell[0] = (local >= thumb_at && local < thumb_at + thumb) ? '\xDB' : '\xB1';
            }
            df_text( top1 + 2 + at, scroll_col, cell, FG_BLUE, BG_WHITE );
        }
    }
}

/* ------------------------------------------------------------------ */
/* the mouse and the keys                                              */
/* ------------------------------------------------------------------ */

/* a click on the scroll bar's row "r": an arrow moves the cursor a row,
   the track puts it as far down the list as the click was down the
   track; 1 if it was an arrow */
static int scroll_click( int r, int vis )
{
    int base = selected_node < 0 ? 0 : selected_node;
    int arrow = 0;

    if ( vis >= 3 && r == 0 ) {
        if ( base > 0 ) {
            base--;
        }
        arrow = 1;
    } else if ( vis >= 3 && r == vis - 1 ) {
        if ( base < tree_count - 1 ) {
            base++;
        }
        arrow = 1;
    } else {
        int in_top = vis >= 3 ? 1 : 0;
        int track = vis >= 3 ? vis - 2 : vis;
        int local = r - in_top;

        if ( local < 0 ) {
            local = 0;
        }
        if ( local > track - 1 ) {
            local = track - 1;
        }
        base = track > 1 ? (int)((long)local * (tree_count - 1) / (track - 1)) : 0;
    }
    selected_node = base;
    clamp_view();
    refresh();
    return arrow;
}

/* an arrow held down: again after a wait, then steadily, for as long as
   the button is down on the same arrow.  Not in a test run, whose
   clock stands still between keys. */
static void scroll_hold( int x, int y, int r, int vis )
{
    EVENT ev;
    u32 next = ev_clock() + REPEAT_FIRST, now;
    int mx, my;

    if ( ev_testing() ) {
        return;
    }
    while ( ev_buttons() & LEFT_BUTTON ) {
        df_flush();
        ev_poll( &ev );
        if ( ev.kind == EV_MOUSE && !(ev.buttons & LEFT_BUTTON) ) {
            break;
        }
        now = ev_clock();
        if ( now < next && next - now <= REPEAT_FIRST ) {
            continue;
        }
        ev_mouse_cell( &mx, &my );
        if ( mx == x && my == y ) {
            scroll_click( r, vis );
        }
        next = now + REPEAT_NEXT;
    }
}

int tree_event( const EVENT *ev )
{
    int vis = interior_rows(), base;

    if ( vis < 1 ) {
        vis = 1;
    }

    if ( ev->kind == EV_MOUSE ) {
        int name_left, size_col, pct_col, scroll_col, right1;
        int first_row = region.row + 2, r, idx, mark0;

        if ( !(ev->press & LEFT_BUTTON) || !data_valid || tree_count <= 0 ) {
            return 0;
        }
        columns( &name_left, &size_col, &pct_col, &scroll_col, &right1 );

        /* the scroll bar */
        if ( tree_count > vis && ev->x == scroll_col - 1 ) {
            r = ev->y - first_row;
            if ( r >= 0 && r < vis ) {
                if ( scroll_click( r, vis ) ) {
                    scroll_hold( ev->x, ev->y, r, vis );
                }
                return 1;
            }
        }

        /* the list: a click picks a row, and on its [+] or [-] opens or
           closes the folder */
        if ( ev->x > region.col && ev->x < region.end_col ) {
            r = ev->y - first_row;
            idx = top + r;
            if ( r >= 0 && r < vis && idx < tree_count ) {
                mark0 = name_left - 1 + row_indent( idx, name_left, size_col );
                selected_node = idx;
                if ( (tree_nodes[idx].flags & NODE_HAS_CHILDREN) && ev->x >= mark0
                     && ev->x < mark0 + 3 ) {
                    if ( tree_nodes[idx].flags & NODE_EXPANDED ) {
                        collapse_node( idx );
                    } else {
                        expand_node_ask( idx );
                    }
                }
                clamp_view();
                refresh();
                return 1;
            }
        }
        return 0;
    }

    if ( ev->kind != EV_KEY || !data_valid ) {
        return 0;
    }
    switch ( ev->key ) {
    case KEY_DOWN:
        if ( tree_count <= 0 ) {
            return 0;
        }
        if ( selected_node < 0 ) {
            selected_node = 0;
        } else if ( selected_node < tree_count - 1 ) {
            selected_node++;
        }
        break;

    case KEY_UP:
        if ( selected_node < 0 ) {
            return 0;
        }
        selected_node--;                /* off the top row: nothing chosen */
        break;

    case KEY_PGDN:
        base = (selected_node < 0 ? 0 : selected_node) + vis;
        if ( base > tree_count - 1 ) {
            base = tree_count - 1;
        }
        selected_node = tree_count > 0 ? base : -1;
        break;

    case KEY_PGUP:
        base = (selected_node < 0 ? 0 : selected_node) - vis;
        if ( base < 0 ) {
            base = 0;
        }
        selected_node = tree_count > 0 ? base : -1;
        break;

    case KEY_HOME:
        selected_node = tree_count > 0 ? 0 : -1;
        break;

    case KEY_END:
        selected_node = tree_count > 0 ? tree_count - 1 : -1;
        break;

    case KEY_RIGHT:
    case '+':
        if ( selected_node >= 0 && (tree_nodes[selected_node].flags & NODE_HAS_CHILDREN)
             && !(tree_nodes[selected_node].flags & NODE_EXPANDED) ) {
            expand_node_ask( selected_node );
            clamp_view();
            refresh();
        }
        return 1;

    case KEY_LEFT:
    case '-':
        if ( selected_node >= 0 && (tree_nodes[selected_node].flags & NODE_EXPANDED) ) {
            collapse_node( selected_node );
            clamp_view();
            refresh();
        }
        return 1;

    case KEY_ENTER:
        if ( selected_node >= 0 && (tree_nodes[selected_node].flags & NODE_HAS_CHILDREN) ) {
            if ( tree_nodes[selected_node].flags & NODE_EXPANDED ) {
                collapse_node( selected_node );
            } else {
                expand_node_ask( selected_node );
            }
            clamp_view();
            refresh();
        }
        return 1;

    default:
        return 0;
    }
    clamp_view();
    refresh();
    return 1;
}
