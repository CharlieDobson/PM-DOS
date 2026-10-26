/*
 * LIST.C - the scrolling list: DOSForms' LISTBOX.CPP in C.
 *
 * Rows come from a callback that writes one into a buffer; the list
 * draws its frame, a header, the rows in view and a scroll bar one
 * column inside the right-hand border when there are rows out of view.
 * Column 0 of every row is left for the mark: a '*' on a text screen,
 * a tick or the row's icon on the graphics one.
 *
 * MARKS are one byte a row, kept count of as they change: the status
 * line asks for the count after every key.  Space and Ins mark the row
 * and step down, + marks all, - none, * turns them over; the right
 * button marks the row under it, and so does the left one in the mark
 * column.  A caller's veto (can_mark) stops a mark going on, never one
 * coming off.
 *
 * AS A TREE (FileBox's drives and folders) the list owns the name field
 * at the left - the mark column, two spaces a level, "[+] " or "[-] ",
 * the icon on a graphics screen, the name - and + and - and the arrows
 * open and shut the node under the bar.  A click on the [+] does that
 * and nothing else.
 *
 * A SECOND PRESS on the same row inside half a second is a double click
 * and goes to on_double, not on_activate: Enter on a file extracts it,
 * and a stray double click should not put a box on the screen.
 */
#include "xarc.h"

#define MARK_GLYPH  '*'

static void format_pad( char *dst, int size, const char *src, int width )
{
    int at = 0;

    if ( width < 0 ) {
        width = 0;
    }
    if ( width > size - 1 ) {
        width = size - 1;
    }
    for ( ; src[at] && at < width; at++ ) {
        dst[at] = src[at];
    }
    for ( ; at < width; at++ ) {
        dst[at] = ' ';
    }
    dst[at] = 0;
}

void list_init( LIST *lst, int fg, int bg )
{
    memset( lst, 0, sizeof( *lst ) );
    lst->fg = fg;
    lst->bg = bg;
    lst->sel_fg = FG_BLACK;
    lst->sel_bg = BG_CYAN;
    lst->mark_fg = FG_YELLOW;
    lst->mark_icon = ICON_NONE;
    lst->name_width = 24;
    lst->last_click_row = -1;
}

void list_free( LIST *lst )
{
    free( lst->marks );
    lst->marks = NULL;
    lst->mark_count = 0;
}

void list_region( LIST *lst, int row, int col, int end_row, int end_col )
{
    lst->at.row = row;
    lst->at.col = col;
    lst->at.end_row = end_row;
    lst->at.end_col = end_col;
}

void list_header( LIST *lst, const char *text )
{
    strncpy( lst->header, text ? text : "", sizeof( lst->header ) - 1 );
    lst->header[sizeof( lst->header ) - 1] = 0;
}

static int interior_cols( const LIST *lst )
{
    int width = lst->at.end_col - lst->at.col - 1;

    return width < 0 ? 0 : width;
}

static int interior_rows( const LIST *lst )
{
    int height = lst->at.end_row - lst->at.row - 1;

    if ( lst->header[0] ) {
        height--;
    }
    return height < 0 ? 0 : height;
}

static void clamp_view( LIST *lst )
{
    int vis = interior_rows( lst );

    if ( lst->count <= 0 ) {
        lst->sel = lst->top = 0;
        return;
    }
    if ( lst->sel < 0 ) {
        lst->sel = 0;
    }
    if ( lst->sel > lst->count - 1 ) {
        lst->sel = lst->count - 1;
    }
    if ( vis <= 0 ) {
        lst->top = lst->sel;
        return;
    }
    if ( lst->sel < lst->top ) {
        lst->top = lst->sel;
    }
    if ( lst->sel >= lst->top + vis ) {
        lst->top = lst->sel - vis + 1;
    }
    if ( lst->top > lst->count - vis ) {
        lst->top = lst->count - vis;
    }
    if ( lst->top < 0 ) {
        lst->top = 0;
    }
}

/* new rows: the marks and a half-made double click are forgotten */
void list_data( LIST *lst, int count, ROWFILL fill, void *user )
{
    lst->count = count;
    lst->fill = fill;
    lst->user = user;
    list_free( lst );
    lst->last_click_row = -1;
    if ( count > 0 ) {
        lst->marks = (u8 *)calloc( (size_t)count, 1 );
    }
    if ( lst->sel >= count ) {
        lst->sel = count - 1;
    }
    if ( lst->sel < 0 ) {
        lst->sel = 0;
    }
    lst->top = 0;
    clamp_view( lst );
}

/* the same list with more rows or fewer: the bar and what marks still
   exist are kept */
void list_count( LIST *lst, int count )
{
    u8 *old = lst->marks;
    int old_count = lst->count, at;

    if ( count < 0 ) {
        count = 0;
    }
    lst->count = count;
    lst->marks = NULL;
    lst->mark_count = 0;
    if ( count > 0 ) {
        lst->marks = (u8 *)calloc( (size_t)count, 1 );
        if ( lst->marks && old ) {
            for ( at = 0; at < old_count && at < count; at++ ) {
                if ( old[at] ) {
                    lst->marks[at] = 1;
                    lst->mark_count++;
                }
            }
        }
    }
    free( old );
    lst->last_click_row = -1;
    clamp_view( lst );
}

void list_tree( LIST *lst, ROWINT depth, ROWINT has_kids, ROWINT expanded,
                ROWINT toggle, ROWNAME label, int name_width )
{
    lst->depth = depth;
    lst->has_kids = has_kids;
    lst->expanded = expanded;
    lst->toggle = toggle;
    lst->label = label;
    lst->name_width = name_width > 6 ? name_width : 6;
}

void list_select( LIST *lst, int row )
{
    lst->sel = row;
    clamp_view( lst );
}

static int tree_icons( const LIST *lst )
{
    return lst->depth && lst->row_icon && df_has_icons();
}

static int tree_indent( const LIST *lst, int row, int width )
{
    int depth = lst->depth ? lst->depth( lst->user, row ) : 0;
    int fixed = tree_icons( lst ) ? 8 : 6, indent;

    if ( depth < 0 ) {
        depth = 0;
    }
    indent = depth * 2;
    if ( indent > width - fixed ) {
        indent = width > fixed ? width - fixed : 0;
    }
    return indent;
}

static void tree_name( LIST *lst, int row, char *dst, int width, int *icon_at )
{
    char label[260];
    int kids = lst->has_kids ? lst->has_kids( lst->user, row ) : 0;
    int open = lst->expanded ? lst->expanded( lst->user, row ) : 0;
    int used = 0, at, indent;

    *icon_at = -1;
    if ( width < 1 ) {
        dst[0] = 0;
        return;
    }
    indent = tree_indent( lst, row, width );
    dst[used++] = ' ';                          /* the mark column */
    for ( at = 0; at < indent && used < width; at++ ) {
        dst[used++] = ' ';
    }
    if ( kids ) {
        if ( used < width ) dst[used++] = '[';
        if ( used < width ) dst[used++] = (char)(open ? '-' : '+');
        if ( used < width ) dst[used++] = ']';
        if ( used < width ) dst[used++] = ' ';
    } else {
        for ( at = 0; at < 4 && used < width; at++ ) {
            dst[used++] = ' ';
        }
    }
    if ( tree_icons( lst ) && used < width ) {
        *icon_at = used;
        dst[used++] = ' ';
        if ( used < width ) {
            dst[used++] = ' ';
        }
    }
    label[0] = 0;
    if ( lst->label ) {
        lst->label( lst->user, row, label, sizeof( label ) );
    }
    for ( at = 0; label[at] && used < width; at++ ) {
        dst[used++] = label[at];
    }
    while ( used < width ) {
        dst[used++] = ' ';
    }
    dst[used] = 0;
}

int list_marked( const LIST *lst, int row )
{
    return lst->marks && row >= 0 && row < lst->count && lst->marks[row];
}

static int can_mark( const LIST *lst, int row )
{
    if ( !lst->marks || row < 0 || row >= lst->count ) {
        return 0;
    }
    return !lst->can_mark || lst->can_mark( lst->user, row );
}

void list_mark( LIST *lst, int row, int on )
{
    if ( !lst->marks || row < 0 || row >= lst->count ) {
        return;
    }
    if ( on && !can_mark( lst, row ) ) {
        return;
    }
    if ( on && !lst->marks[row] ) {
        lst->marks[row] = 1;
        lst->mark_count++;
    } else if ( !on && lst->marks[row] ) {
        lst->marks[row] = 0;
        lst->mark_count--;
    }
}

void list_mark_all( LIST *lst )
{
    int at;

    if ( !lst->marks ) {
        return;
    }
    lst->mark_count = 0;
    for ( at = 0; at < lst->count; at++ ) {
        lst->marks[at] = (u8)can_mark( lst, at );
        lst->mark_count += lst->marks[at];
    }
}

void list_unmark_all( LIST *lst )
{
    if ( !lst->marks ) {
        return;
    }
    memset( lst->marks, 0, (size_t)lst->count );
    lst->mark_count = 0;
}

void list_invert( LIST *lst )
{
    int at;

    if ( !lst->marks ) {
        return;
    }
    lst->mark_count = 0;
    for ( at = 0; at < lst->count; at++ ) {
        lst->marks[at] = (u8)(!lst->marks[at] && can_mark( lst, at ));
        lst->mark_count += lst->marks[at];
    }
}

int list_get_marked( const LIST *lst, int *out, int max )
{
    int at, got = 0;

    if ( !lst->marks || out == NULL ) {
        return 0;
    }
    for ( at = 0; at < lst->count && got < max; at++ ) {
        if ( lst->marks[at] ) {
            out[got++] = at;
        }
    }
    return got;
}

static void notify_marks( LIST *lst )
{
    list_draw( lst );
    if ( lst->on_marks ) {
        lst->on_marks( lst->user );
    }
}

void list_draw( LIST *lst )
{
    int top1 = lst->at.row + 1, left1 = lst->at.col + 1;
    int bot1 = lst->at.end_row + 1, right1 = lst->at.end_col + 1;
    int icols, hdr, vis, at, idx, icon_at, mk, hi, fg, bg, nw, icon;
    int use_icons, trees;
    char line[132], raw[260], tail[132], cell[2];

    if ( lst->at.end_row <= lst->at.row || lst->at.end_col <= lst->at.col ) {
        return;
    }
    clamp_view( lst );
    draw_box( top1, left1, bot1, right1, BORDER_SINGLE, lst->fg, lst->bg );
    df_fill( top1 + 1, left1 + 1, bot1 - 1, right1 - 1, lst->bg );
    icols = interior_cols( lst );
    hdr = lst->header[0] ? 1 : 0;
    use_icons = df_has_icons() && (lst->row_icon || lst->mark_icon >= 0);
    trees = tree_icons( lst );
    if ( hdr ) {
        format_pad( line, sizeof( line ), lst->header, icols );
        df_text( top1 + 1, left1 + 1, line, FG_BRIGHT_WHITE, lst->bg );
    }
    vis = interior_rows( lst );
    for ( at = 0; at < vis; at++ ) {
        idx = lst->top + at;
        hi = idx == lst->sel && lst->count > 0;
        mk = list_marked( lst, idx );
        fg = hi ? lst->sel_fg : mk ? lst->mark_fg : lst->fg;
        bg = hi ? lst->sel_bg : lst->bg;
        icon_at = -1;
        if ( idx < lst->count && lst->depth ) {
            nw = lst->name_width > icols ? icols : lst->name_width;
            tree_name( lst, idx, raw, nw, &icon_at );
            tail[0] = 0;
            if ( lst->fill ) {
                lst->fill( lst->user, idx, tail, sizeof( tail ) );
            }
            strncat( raw, tail, sizeof( raw ) - 1 - strlen( raw ) );
            format_pad( line, sizeof( line ), raw, icols );
        } else if ( idx < lst->count && lst->fill ) {
            raw[0] = 0;
            lst->fill( lst->user, idx, raw, sizeof( line ) );
            format_pad( line, sizeof( line ), raw, icols );
        } else {
            format_pad( line, sizeof( line ), "", icols );
        }
        if ( mk && icols > 0 && !use_icons ) {
            line[0] = MARK_GLYPH;
        }
        df_text( top1 + 1 + hdr + at, left1 + 1, line, fg, bg );
        if ( use_icons && trees && icols > 0 && idx < lst->count ) {
            if ( mk && lst->mark_icon >= 0 ) {
                df_icon( top1 + 1 + hdr + at, left1 + 1, lst->mark_icon, bg );
            }
            if ( icon_at >= 0 && icon_at < icols ) {
                icon = lst->row_icon( lst->user, idx );
                if ( icon >= 0 ) {
                    df_icon( top1 + 1 + hdr + at, left1 + 1 + icon_at, icon, bg );
                }
            }
        } else if ( use_icons && icols > 0 && idx < lst->count ) {
            icon = mk ? lst->mark_icon : lst->row_icon ? lst->row_icon( lst->user, idx ) : ICON_NONE;
            if ( icon >= 0 ) {
                df_icon( top1 + 1 + hdr + at, left1 + 1, icon, bg );
            }
        }
    }

    /* the scroll bar, one column in from the right-hand border */
    if ( lst->count > vis && vis > 0 ) {
        int track = vis >= 3 ? vis - 2 : vis, max_top = lst->count - vis;
        int thumb = track * vis / lst->count, pos, local;

        if ( thumb < 1 ) {
            thumb = 1;
        }
        if ( thumb > track ) {
            thumb = track;
        }
        pos = max_top > 0 ? (int)((long)lst->top * (track - thumb) / max_top) : 0;
        cell[1] = 0;
        for ( at = 0; at < vis; at++ ) {
            if ( vis >= 3 && at == 0 ) {
                cell[0] = 0x18;
            } else if ( vis >= 3 && at == vis - 1 ) {
                cell[0] = 0x19;
            } else {
                local = vis >= 3 ? at - 1 : at;
                cell[0] = (char)(local >= pos && local < pos + thumb ? 0xDB : 0xB1);
            }
            df_text( top1 + 1 + hdr + at, right1 - 1, cell, FG_BRIGHT_WHITE, lst->bg );
        }
    }
}

void list_toggle_node( LIST *lst, int row )
{
    if ( !lst->toggle || row < 0 || row >= lst->count ) {
        return;
    }
    if ( lst->has_kids && !lst->has_kids( lst->user, row ) ) {
        return;
    }
    list_count( lst, lst->toggle( lst->user, row ) );
    lst->sel = row;
    clamp_view( lst );
    list_draw( lst );
}

int list_event( LIST *lst, const EVENT *ev )
{
    int vis = interior_rows( lst ), first, row, idx, track, local, top_in, nw, m0, dbl;
    u32 now;

    if ( ev->kind == EV_MOUSE ) {
        if ( !ev->press ) {
            return 0;
        }
        first = lst->at.row + 1 + (lst->header[0] ? 1 : 0);
        /* the scroll bar: the arrows step, the track jumps */
        if ( (ev->press & LEFT_BUTTON) && lst->count > vis && vis > 0
             && ev->x == lst->at.end_col - 1 ) {
            row = ev->y - first;
            if ( row >= 0 && row < vis ) {
                if ( vis >= 3 && row == 0 ) {
                    if ( lst->sel > 0 ) {
                        lst->sel--;
                    }
                } else if ( vis >= 3 && row == vis - 1 ) {
                    if ( lst->sel < lst->count - 1 ) {
                        lst->sel++;
                    }
                } else {
                    top_in = vis >= 3 ? 1 : 0;
                    track = vis >= 3 ? vis - 2 : vis;
                    local = row - top_in;
                    if ( local < 0 ) {
                        local = 0;
                    }
                    if ( local > track - 1 ) {
                        local = track - 1;
                    }
                    lst->sel = track > 1 ? local * (lst->count - 1) / (track - 1) : 0;
                }
                clamp_view( lst );
                list_draw( lst );
                return 1;
            }
        }
        /* marking: the right button anywhere on a row, the left one in
           the mark column */
        if ( (ev->press & RIGHT_BUTTON)
             || ((ev->press & LEFT_BUTTON) && ev->x == lst->at.col + 1) ) {
            if ( ev->x > lst->at.col && ev->x < lst->at.end_col ) {
                row = ev->y - first;
                if ( row >= 0 && row < vis ) {
                    idx = lst->top + row;
                    if ( idx >= 0 && idx < lst->count ) {
                        lst->sel = idx;
                        list_mark( lst, idx, !list_marked( lst, idx ) );
                        clamp_view( lst );
                        notify_marks( lst );
                        return 1;
                    }
                }
            }
            if ( ev->press & RIGHT_BUTTON ) {
                return 0;
            }
        }
        if ( (ev->press & LEFT_BUTTON) && ev->x > lst->at.col && ev->x < lst->at.end_col ) {
            row = ev->y - first;
            if ( row >= 0 && row < vis ) {
                idx = lst->top + row;
                /* the [+] of a tree node: open or shut, nothing else */
                if ( lst->depth && idx >= 0 && idx < lst->count && lst->has_kids
                     && lst->has_kids( lst->user, idx ) ) {
                    nw = lst->name_width > interior_cols( lst ) ? interior_cols( lst )
                                                                : lst->name_width;
                    m0 = lst->at.col + 2 + tree_indent( lst, idx, nw );
                    if ( ev->x >= m0 && ev->x <= m0 + 2 ) {
                        lst->sel = idx;
                        list_toggle_node( lst, idx );
                        return 1;
                    }
                }
                if ( idx >= 0 && idx < lst->count ) {
                    now = ev_clock();
                    dbl = idx == lst->sel && idx == lst->last_click_row
                          && now - lst->last_click_at < 50;
                    lst->sel = idx;
                    clamp_view( lst );
                    list_draw( lst );
                    lst->last_click_row = idx;
                    lst->last_click_at = now;
                    if ( dbl && lst->on_double ) {
                        lst->last_click_row = -1;
                        lst->on_double( lst->user, lst->sel );
                    }
                    return 1;
                }
            }
        }
        return 0;
    }
    if ( ev->kind != EV_KEY ) {
        return 0;
    }
    if ( vis < 1 ) {
        vis = 1;
    }
    switch ( ev->key ) {
    case KEY_UP:
        if ( lst->sel > 0 ) {
            lst->sel--;
        }
        break;
    case KEY_DOWN:
        if ( lst->sel < lst->count - 1 ) {
            lst->sel++;
        }
        break;
    case KEY_PGUP:
        lst->sel -= vis;
        if ( lst->sel < 0 ) {
            lst->sel = 0;
        }
        break;
    case KEY_PGDN:
        lst->sel += vis;
        if ( lst->sel > lst->count - 1 ) {
            lst->sel = lst->count - 1;
        }
        break;
    case KEY_HOME:
        lst->sel = 0;
        break;
    case KEY_END:
        lst->sel = lst->count - 1;
        break;
    case ' ':
    case KEY_INS:
        if ( lst->count > 0 ) {
            list_mark( lst, lst->sel, !list_marked( lst, lst->sel ) );
            if ( lst->sel < lst->count - 1 ) {
                lst->sel++;
            }
            clamp_view( lst );
            notify_marks( lst );
        }
        return 1;
    case '+':
    case KEY_RIGHT:
        if ( lst->depth ) {
            if ( lst->count > 0 && lst->expanded && !lst->expanded( lst->user, lst->sel ) ) {
                list_toggle_node( lst, lst->sel );
            }
            return 1;
        }
        if ( ev->key == '+' && lst->count > 0 ) {
            list_mark_all( lst );
            notify_marks( lst );
        }
        return ev->key == '+';
    case '-':
    case KEY_LEFT:
        if ( lst->depth ) {
            if ( lst->count > 0 && lst->expanded && lst->expanded( lst->user, lst->sel ) ) {
                list_toggle_node( lst, lst->sel );
            }
            return 1;
        }
        if ( ev->key == '-' && lst->count > 0 ) {
            list_unmark_all( lst );
            notify_marks( lst );
        }
        return ev->key == '-';
    case '*':
        if ( lst->count > 0 ) {
            list_invert( lst );
            notify_marks( lst );
        }
        return 1;
    case KEY_ENTER:
        if ( lst->depth && lst->count > 0 && lst->has_kids && lst->has_kids( lst->user, lst->sel ) ) {
            list_toggle_node( lst, lst->sel );
            return 1;
        }
        if ( lst->on_activate && lst->count > 0 ) {
            lst->on_activate( lst->user, lst->sel );
        }
        return 1;
    default:
        return 0;
    }
    clamp_view( lst );
    list_draw( lst );
    return 1;
}
