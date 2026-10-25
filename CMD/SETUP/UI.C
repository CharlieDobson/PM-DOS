/*
 * UI.C - the pieces of a Setup screen that wait for the user: lists,
 * boxes, the help pages and the edit fields.
 *
 * A BOX IS LAID OUT THE WAY 6.22 SETUP LAID ITS OWN OUT: it is a list of
 * lines, the first of them its title, and its size and place come from
 * the lines alone - top = (25 - lines) / 2, bottom = top + lines, as
 * wide as the longest line plus three, and centred.  Every line is
 * centred on its own; the lines of a paragraph are padded to one length
 * so that the paragraph comes out as a block.  Measured against the
 * real program, that rule puts every box where 6.22 put it.
 *
 * A LIST is the in-page kind: a box from column 9 to 70, items at 11,
 * the highlight across 10-69.  UP and DOWN move it one item and stop
 * at the ends; nothing else moves it, as in 6.22.  An empty item is a
 * gap the highlight steps over.  A list longer than its window scrolls
 * a line at a time.
 */
#include "setup.h"
#include "text.h"

/* ------------------------------------------------------------------ */
/* boxes                                                               */
/* ------------------------------------------------------------------ */

void dlg_start( DLG *dlg, const char *title, int width )
{
    dlg->n = 0;
    dlg->used = 0;
    dlg->width = width;
    dlg->line[dlg->n++] = title ? title : "";
}

static char *dlg_room( DLG *dlg, int len )
{
    char *buf;

    if ( dlg->used + len + 1 > (int)sizeof( dlg->buf ) || dlg->n >= DLG_LINES ) {
        return NULL;
    }
    buf = dlg->buf + dlg->used;
    dlg->used += len + 1;
    return buf;
}

void dlg_blank( DLG *dlg )
{
    if ( dlg->n < DLG_LINES ) {
        dlg->line[dlg->n++] = "";
    }
}

void dlg_center( DLG *dlg, const char *text )
{
    if ( dlg->n < DLG_LINES ) {
        dlg->line[dlg->n++] = text;
    }
}

/* a paragraph, broken at the box's width and padded to it; "first"
   starts its first line and as many blanks start the others */
static void dlg_wrap( DLG *dlg, const char *first, const char *text )
{
    int width = dlg->width, lead = str_len( first ), end, brk;
    char *line;

    while ( *text ) {
        end = 0;
        brk = -1;
        while ( text[end] && text[end] != '\n' ) {
            if ( text[end] == ' ' ) {
                brk = end;
            }
            if ( end >= width - lead ) {
                if ( brk > 0 ) {
                    end = brk;
                }
                break;
            }
            end++;
        }
        line = dlg_room( dlg, width );
        if ( line == NULL ) {
            return;
        }
        mem_set( line, ' ', (u16)width );
        line[width] = 0;
        if ( first ) {
            mem_cpy( line, first, (u16)lead );
            first = NULL;               /* blanks from now on */
        }
        mem_cpy( line + lead, text, (u16)end );
        dlg->line[dlg->n++] = line;
        text += end;
        if ( *text == ' ' || *text == '\n' ) {
            text++;
        }
    }
}

void dlg_para( DLG *dlg, const char *text )
{
    dlg_wrap( dlg, "", text );
}

void dlg_bullet( DLG *dlg, const char *text )
{
    dlg_wrap( dlg, "\x07 ", text );
}

static int accepted( int key, const int *keys )
{
    if ( keys == NULL ) {
        return key < 0x100;             /* any key with a character */
    }
    for ( ; *keys; keys++ ) {
        if ( *keys == key ) {
            return 1;
        }
    }
    return 0;
}

void dlg_draw( DLG *dlg, int attr )
{
    int i, len, top, bottom, left, right, longest = 0;

    for ( i = 0; i < dlg->n; i++ ) {
        len = str_len( dlg->line[i] );
        if ( len > longest ) {
            longest = len;
        }
    }
    top = (SCR_ROWS - dlg->n) / 2;
    bottom = top + dlg->n;
    left = (SCR_COLS - (longest + 3)) / 2;
    right = left + longest + 3;
    scr_box( top, left, bottom, right, attr, dlg->line[0] );
    for ( i = 1; i < dlg->n; i++ ) {
        len = str_len( dlg->line[i] );
        scr_put( top + i, (SCR_COLS - len) / 2, attr, dlg->line[i] );
    }
}

/* Draw the box over the screen, wait for one of "keys" (NULL = any key
   with a character), take the box away again.  F3 opens the Exit box
   when it is not one of the keys and "f3" is set. */
int dlg_run( DLG *dlg, int attr, const char *status, const int *keys, int f3 )
{
    static u16 under[SCR_ROWS * SCR_COLS];
    static char keep[100];
    int key;

    scr_save( under );
    str_cpyn( keep, status_now(), sizeof( keep ) );
    for ( ;; ) {
        dlg_draw( dlg, attr );
        scr_status( status, NULL );
        key = key_get();
        if ( accepted( key, keys ) ) {
            break;
        }
        if ( key == K_F3 && f3 ) {
            ui_exit_box();
        }
    }
    scr_restore( under );
    scr_status( keep, NULL );
    return key;
}

/* One box of text and a key, the common shape: a notice in cyan or a
   warning in grey.  The text's paragraphs are separated by '\n'. */
int ui_notice( const char *title, const char *text, int attr, const char *keys )
{
    static const int enter[] = { K_ENTER, 0 };
    DLG dlg;

    dlg_start( &dlg, title, 40 );
    dlg_blank( &dlg );
    dlg_para( &dlg, text );
    dlg_blank( &dlg );
    return dlg_run( &dlg, attr, keys, enter, 1 );
}

/* F3, wherever it is pressed: the "Exiting Setup" box.  A second F3
   leaves; ESC goes back to where Setup was. */
int ui_exit_box( void )
{
    static int open;
    static const int keys[] = { K_F3, K_ESC, 0 };
    DLG dlg;
    int key;

    if ( open ) {
        return 0;
    }
    open = 1;
    dlg_start( &dlg, tx_exit_title, 52 );
    dlg_blank( &dlg );
    dlg_para( &dlg, tx_exit_text );
    dlg_blank( &dlg );
    dlg_bullet( &dlg, tx_exit_f3 );
    dlg_blank( &dlg );
    dlg_bullet( &dlg, tx_exit_esc );
    dlg_blank( &dlg );
    key = dlg_run( &dlg, A_BAR, "", keys, 0 );
    open = 0;
    if ( key == K_F3 ) {
        leave( 1 );
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* help                                                                */
/* ------------------------------------------------------------------ */

/* A page of help text: paragraphs separated by a blank line ("\n\n"),
   a paragraph that starts "* " is a bullet */
int scr_page( int row, int attr, const char *text )
{
    char para[600];
    int len, bullet;

    while ( *text ) {
        len = 0;
        while ( text[len] && !(text[len] == '\n' && text[len + 1] == '\n') ) {
            len++;
        }
        bullet = text[0] == '*' && text[1] == ' ';
        str_cpyn( para, text + (bullet ? 2 : 0),
                  len - (bullet ? 2 : 0) + 1 < (int)sizeof( para )
                  ? len - (bullet ? 2 : 0) + 1 : (int)sizeof( para ) );
        if ( bullet ) {
            scr_putn( row, 12, attr, "\x07", 1 );
            row += scr_text( row, 14, 56, attr, para );
        } else {
            row += scr_text( row, 10, 60, attr, para );
        }
        text += len;
        if ( *text ) {
            text += 2;
            row++;
        }
    }
    return row;
}

/* F1: one page, grey; only ESC goes back */
void ui_help( const char *heading, const char *text )
{
    static int open;
    static u16 under[SCR_ROWS * SCR_COLS];
    static char keep[100];
    int key, i;

    if ( open || text == NULL ) {
        return;
    }
    open = 1;
    scr_save( under );
    str_cpyn( keep, status_now(), sizeof( keep ) );
    for ( ;; ) {
        scr_fill( 0, 0, SCR_ROWS, SCR_COLS, A_BAR, ' ' );
        scr_fill( 1, 1, 2, 79, A_HELPHEAD, ' ' );
        scr_put( 1, 1, A_HELPHEAD, tx_help_title );
        for ( i = 0; i < str_len( tx_help_title ); i++ ) {
            scr_putn( 2, 1 + i, A_HELPHEAD, "\xCD", 1 );
        }
        scr_put( 4, 10, A_BAR, heading );
        scr_page( 6, A_BAR, text );
        scr_status( tx_keys_help, NULL );
        scr_attr( STATUS_ROW, 0, SCR_COLS, A_DESK );
        scr_nocursor();
        key = key_get();
        if ( key == K_ESC ) {
            break;
        }
        if ( key == K_F3 ) {
            scr_restore( under );
            scr_status( keep, NULL );
            ui_exit_box();
        }
    }
    scr_restore( under );
    scr_status( keep, NULL );
    open = 0;
}

/* ------------------------------------------------------------------ */
/* lists                                                               */
/* ------------------------------------------------------------------ */

static int next_item( const char **items, int count, int from, int step )
{
    int i;

    for ( i = from + step; i >= 0 && i < count; i += step ) {
        if ( items[i][0] ) {
            return i;
        }
    }
    return from;
}

/* The in-page list.  The page around it is the caller's, already drawn
   (and kept by the list, which redraws only its own box).  -> the item
   chosen with ENTER, or -1 for ESC when the status line offers ESC. */
int ui_list( LIST *list )
{
    int top = list->top, rows = list->rows, count = list->count;
    int sel = list->sel, first = list->first, i, key, row;
    int bottom = top + rows + 1;

    if ( sel < 0 || sel >= count || !list->items[sel][0] ) {
        sel = next_item( list->items, count, -1, 1 );
    }
    if ( first < 0 ) {
        /* opened so the window is full and the choice is in it, low
           down as 6.22's country list had it */
        first = sel - (rows - 2);
    }
    for ( ;; ) {
        if ( sel < first ) {
            first = sel;
        }
        if ( sel >= first + rows ) {
            first = sel - rows + 1;
        }
        if ( first > count - rows ) {
            first = count - rows;
        }
        if ( first < 0 ) {
            first = 0;
        }
        scr_box( top, 9, bottom, 70, A_DESK, NULL );
        for ( i = 0; i < rows && first + i < count; i++ ) {
            row = top + 1 + i;
            if ( first + i == sel ) {
                scr_fill( row, 10, 1, 60, A_BAR, ' ' );
                scr_putn( row, 11, A_BAR, list->items[first + i], 58 );
            } else {
                scr_putn( row, 11, A_DESK, list->items[first + i], 58 );
            }
        }
        key = key_get();
        switch ( key ) {
        case K_UP:
            sel = next_item( list->items, count, sel, -1 );
            break;
        case K_DOWN:
            sel = next_item( list->items, count, sel, 1 );
            break;
        case K_ENTER:
            list->sel = sel;
            list->first = first;
            return sel;
        case K_ESC:
            if ( !(list->flags & LS_NOESC) ) {
                list->sel = sel;
                return -1;
            }
            break;
        case K_F1:
            ui_help( list->help_head, list->help_text );
            break;
        case K_F3:
            ui_exit_box();
            break;
        default:
            if ( list->other && list->other( key ) ) {
                list->sel = sel;
                list->first = first;
                return -2 - key;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the edit field                                                      */
/* ------------------------------------------------------------------ */

/* "width" cells in the highlight colour with the cursor after the text;
   the first "fixed" characters cannot be removed.  Comes back with the
   key that ended it: ENTER, ESC, F1, F3, UP or DOWN. */
int ui_field( FIELD *fld )
{
    int key, len, i;
    char *buf = fld->buf;

    if ( fld->pos < 0 || fld->pos > str_len( buf ) ) {
        fld->pos = str_len( buf );
    }
    for ( ;; ) {
        len = str_len( buf );
        scr_fill( fld->row, fld->col, 1, fld->width, A_BAR, ' ' );
        scr_putn( fld->row, fld->col, A_BAR, buf, fld->width );
        scr_cursor( fld->row, fld->col + (fld->pos < fld->width ? fld->pos
                                                                : fld->width - 1) );
        key = key_get();
        switch ( key ) {
        case K_LEFT:
            if ( fld->pos > fld->fixed ) {
                fld->pos--;
            }
            break;
        case K_RIGHT:
            if ( fld->pos < len ) {
                fld->pos++;
            }
            break;
        case K_HOME:
            fld->pos = fld->fixed;
            break;
        case K_END:
            fld->pos = len;
            break;
        case K_BKSP:
            if ( fld->pos > fld->fixed ) {
                fld->pos--;
                for ( i = fld->pos; i < len; i++ ) {
                    buf[i] = buf[i + 1];
                }
            }
            break;
        case K_DEL:
            if ( fld->pos < len ) {
                for ( i = fld->pos; i < len; i++ ) {
                    buf[i] = buf[i + 1];
                }
            }
            break;
        case K_ENTER: case K_ESC: case K_F1: case K_F3: case K_UP: case K_DOWN:
            scr_nocursor();
            return key;
        default:
            if ( key >= ' ' && key < 0x7F && len < fld->max - 1 &&
                 (fld->accept == NULL || str_chr( fld->accept, key )) ) {
                for ( i = len + 1; i > fld->pos; i-- ) {
                    buf[i] = buf[i - 1];
                }
                buf[fld->pos++] = (char)ch_upper( key );
            }
            break;
        }
    }
}

/* What Setup is doing, while nothing is asked: a small box of its own */
void ui_working( const char *what )
{
    DLG dlg;

    dlg_start( &dlg, "", str_len( what ) );
    dlg_blank( &dlg );
    dlg_center( &dlg, what );
    dlg_blank( &dlg );
    dlg_draw( &dlg, A_NOTE );
    scr_flush();
}
