/*
 * EDIT.C - a document being edited (EDIT.H).
 *
 * EVERY COMMAND that changes the document is one action of WEDIT.C's,
 * so one Undo takes it back; letters typed one after another are one
 * action a word at a time.  A command that finds no memory leaves the
 * document as it was and sets "no_memory", which whoever called says
 * to the user and clears.
 *
 * THE LOOK OF WHAT IS TYPED NEXT is the look of the character before
 * the caret, as in Word - until the user chooses one with nothing
 * selected (presses Bold, say), which lasts until the caret is moved.
 */
#include "fmt.h"
#include "edit.h"

static WDOC *clip;              /* the clipboard: a piece, as WEDIT.C makes them */
static int   type_kind = 1;     /* we_begin_more's: changes after each word */

/* ------------------------------------------------------------------ */
/* small things                                                        */
/* ------------------------------------------------------------------ */

static WPARA *para_of( const EDITOR *ed, u32 index )
{
    return ed->story->para[index < ed->story->count ? index : ed->story->count - 1];
}

static int is_low_half( u32 unit )
{
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

/* the place one character on, or back: over a paragraph's end too */
static WPOS step_right( const EDITOR *ed, WPOS pos )
{
    const WPARA *para = para_of( ed, pos.para );

    if ( pos.off < para->len ) {
        pos.off++;
        if ( pos.off < para->len && is_low_half( para->text[pos.off] ) ) {
            pos.off++;
        }
    } else if ( pos.para + 1 < ed->story->count ) {
        pos.para++;
        pos.off = 0;
    }
    return pos;
}

static WPOS step_left( const EDITOR *ed, WPOS pos )
{
    const WPARA *para = para_of( ed, pos.para );

    if ( pos.off > 0 ) {
        pos.off--;
        if ( pos.off > 0 && is_low_half( para->text[pos.off] ) ) {
            pos.off--;
        }
    } else if ( pos.para > 0 ) {
        pos.para--;
        pos.off = para_of( ed, pos.para )->len;
    }
    return pos;
}

/* 0 a space, 1 part of a word, 2 anything else */
static int char_class( u32 code )
{
    if ( code == ' ' || code == WCH_TAB || code == WCH_NBSP || code == WCH_LINE || code == WCH_PAGE ) {
        return 0;
    }
    if ( (code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z') ||
         code == '_' || code == '\'' || code == 0x2019 || (code >= 0xC0 && code < 0x2000 && code != 0xD7 &&
         code != 0xF7) || (code >= 0xD800 && code <= 0xDFFF) ) {
        return 1;
    }
    return 2;
}

/* the look text typed at a place would have */
static void look_at( EDITOR *ed, WPOS pos, CHP *chp )
{
    const WPARA *para = para_of( ed, pos.para );
    const WDOC *doc = ed->doc;

    if ( pos.off > 0 && pos.off <= para->len ) {
        *chp = *wd_chp_get( doc, para->attr[pos.off - 1] );
    } else if ( para->len > 0 ) {
        *chp = *wd_chp_get( doc, para->attr[0] );
    } else {
        *chp = doc->style[para->pap.style < doc->nstyle ? para->pap.style : 0].chp;
    }
}

/* the caret has been put somewhere */
static void moved( EDITOR *ed, int keep_x )
{
    ed->caret = wpos_clamp( ed->story, ed->caret );
    ed->anchor = wpos_clamp( ed->story, ed->anchor );
    ed->type_set = 0;
    look_at( ed, ed->caret, &ed->type_chp );
    if ( !keep_x ) {
        ed->want_x = -1;
    }
    ed->follow = 1;
    type_kind ^= 3;                     /* typing after this is a new action */
}

static void act_begin( EDITOR *ed )
{
    we_begin( ed->doc, ed->story, ed->caret );
}

static void act_end( EDITOR *ed )
{
    if ( !we_end( ed->doc, ed->caret ) ) {
        ed->no_memory = 1;
    }
    ed->caret = wpos_clamp( ed->story, ed->caret );
    ed->anchor = ed->caret;
    ed->want_x = -1;
    ed->follow = 1;
}

int ed_has_sel( const EDITOR *ed )
{
    return wpos_cmp( ed->caret, ed->anchor ) != 0;
}

void ed_sel( const EDITOR *ed, WPOS *from, WPOS *to )
{
    if ( wpos_cmp( ed->caret, ed->anchor ) <= 0 ) {
        *from = ed->caret;
        *to = ed->anchor;
    } else {
        *from = ed->anchor;
        *to = ed->caret;
    }
}

/* can the end of paragraph "index" be taken out, joining it to the
   next?  Not when that would run one table cell into another. */
static int can_join( const EDITOR *ed, u32 index )
{
    const WPARA *para, *next;

    if ( index + 1 >= ed->story->count ) {
        return 0;
    }
    para = ed->story->para[index];
    next = ed->story->para[index + 1];
    if ( para->pap.flags & PF_CELLEND ) {
        return 0;
    }
    return (para->pap.flags & PF_INTABLE) == (next->pap.flags & PF_INTABLE);
}

/* the selection taken out, inside an action; the caret where it was */
static void drop_sel( EDITOR *ed )
{
    WPOS from, to, start, end;
    u32 index;
    int plain = 1;

    if ( !ed_has_sel( ed ) ) {
        return;
    }
    ed_sel( ed, &from, &to );
    from = wpos_clamp( ed->story, from );
    to = wpos_clamp( ed->story, to );
    for ( index = from.para; index < to.para && plain; index++ ) {
        plain = can_join( ed, index );
    }
    if ( plain ) {
        we_delete( ed->doc, ed->story, from, to );
    } else {
        /* it runs over the end of a table's cell: what the cells hold
           goes and the cells stay.  The last paragraph first, so that
           the ones still to do keep their numbers. */
        for ( index = to.para; ; index-- ) {
            start.para = end.para = index;
            start.off = index == from.para ? from.off : 0;
            end.off = index == to.para ? to.off : para_of( ed, index )->len;
            we_delete( ed->doc, ed->story, start, end );
            if ( index < to.para && can_join( ed, index ) ) {
                start.off = para_of( ed, index )->len;
                end.para = index + 1;
                end.off = 0;
                we_delete( ed->doc, ed->story, start, end );
            }
            if ( index == from.para ) {
                break;
            }
        }
    }
    ed->caret = ed->anchor = from;
}

/* ------------------------------------------------------------------ */
/* an editor                                                           */
/* ------------------------------------------------------------------ */

EDITOR *ed_new( WDOC *doc, int grid )
{
    EDITOR *ed = (EDITOR *)try_alloc( sizeof( EDITOR ) );

    if ( ed == NULL ) {
        return NULL;
    }
    mem_set( ed, 0, sizeof( EDITOR ) );
    ed->doc = doc;
    ed->story = &doc->body;
    ed->grid = grid;
    ed->lay = lay_new( doc, ed->story, grid );
    if ( ed->lay == NULL ) {
        xfree( ed );
        return NULL;
    }
    ed->want_x = -1;
    ed->view_w = 8640;
    ed->view_h = 4800;
    ed->format = doc->format;
    ed->variant = doc->variant;
    ed->saved_serial = doc->serial;
    look_at( ed, ed->caret, &ed->type_chp );
    return ed;
}

void ed_free( EDITOR *ed )
{
    if ( ed != NULL ) {
        lay_free( ed->lay );
        wd_free( ed->doc );
        xfree( ed );
    }
}

void ed_set_device( EDITOR *ed, int grid )
{
    LAY *fresh;

    if ( (grid != 0) == (ed->grid != 0) ) {
        return;
    }
    fresh = lay_new( ed->doc, ed->story, grid );
    if ( fresh == NULL ) {
        ed->no_memory = 1;
        return;
    }
    lay_free( ed->lay );
    ed->lay = fresh;
    ed->grid = grid != 0;
    ed->want_x = -1;
    ed->scroll_x = 0;
    ed->follow = 1;
}

void ed_set_story( EDITOR *ed, STORY *story )
{
    LAY *fresh;

    if ( story == ed->story || story == NULL || story->count == 0 ) {
        return;
    }
    fresh = lay_new( ed->doc, story, ed->grid );
    if ( fresh == NULL ) {
        ed->no_memory = 1;
        return;
    }
    if ( ed->story == &ed->doc->body ) {
        ed->body_caret = ed->caret;
        ed->body_scroll = ed->scroll_y;
    }
    lay_free( ed->lay );
    ed->lay = fresh;
    ed->story = story;
    ed->caret.para = ed->caret.off = 0;
    ed->scroll_x = ed->scroll_y = 0;
    if ( story == &ed->doc->body ) {
        /* back where it was left */
        ed->caret = wpos_clamp( story, ed->body_caret );
        ed->scroll_y = ed->body_scroll;
    }
    ed->anchor = ed->caret;
    moved( ed, 0 );
}

void ed_set_view( EDITOR *ed, s32 width, s32 height )
{
    ed->view_w = width > GRID_W ? width : GRID_W;
    ed->view_h = height > GRID_H ? height : GRID_H;
}

static void scroll_clamp( EDITOR *ed )
{
    s32 most = lay_height( ed->lay ) - ed->view_h;

    if ( ed->scroll_y > most ) {
        ed->scroll_y = most;
    }
    if ( ed->scroll_y < 0 ) {
        ed->scroll_y = 0;
    }
    if ( ed->grid ) {
        ed->scroll_y = ed->scroll_y / GRID_H * GRID_H;
        ed->scroll_x = ed->scroll_x / GRID_W * GRID_W;
    }
}

void ed_update( EDITOR *ed )
{
    s32 x, y, height, edge = ed->grid ? GRID_W : 240;

    if ( !lay_update( ed->lay ) ) {
        ed->no_memory = 1;
    }
    ed->caret = wpos_clamp( ed->story, ed->caret );
    ed->anchor = wpos_clamp( ed->story, ed->anchor );
    if ( ed->follow ) {
        ed->follow = 0;
        lay_caret( ed->lay, ed->caret, &x, &y, &height );
        if ( y + height > ed->scroll_y + ed->view_h ) {
            ed->scroll_y = y + height - ed->view_h;
            if ( ed->grid && ed->scroll_y % GRID_H ) {
                ed->scroll_y += GRID_H - ed->scroll_y % GRID_H;
            }
        }
        if ( y < ed->scroll_y ) {
            ed->scroll_y = y;
        }
        if ( x + edge > ed->scroll_x + ed->view_w ) {
            ed->scroll_x = x + edge - ed->view_w;
            if ( ed->grid && ed->scroll_x % GRID_W ) {
                ed->scroll_x += GRID_W - ed->scroll_x % GRID_W;
            }
        }
        if ( x < ed->scroll_x ) {
            ed->scroll_x = x > edge ? x - edge : 0;
        }
        /* the caret's own line may be the last thing in the window, but
           the window does not otherwise run past the text */
        if ( ed->scroll_y < 0 ) {
            ed->scroll_y = 0;
        }
        if ( ed->grid ) {
            ed->scroll_y = ed->scroll_y / GRID_H * GRID_H;
            ed->scroll_x = ed->scroll_x / GRID_W * GRID_W;
        }
    } else {
        scroll_clamp( ed );
    }
}

void ed_scroll_to( EDITOR *ed, s32 y )
{
    ed->scroll_y = y;
    scroll_clamp( ed );
}

void ed_scroll_by( EDITOR *ed, s32 lines )
{
    ed->scroll_y += lines * (ed->grid ? GRID_H : 240);
    scroll_clamp( ed );
}

int ed_modified( const EDITOR *ed )
{
    return ed->doc->serial != ed->saved_serial;
}

void ed_mark_saved( EDITOR *ed )
{
    ed->saved_serial = ed->doc->serial;
}

/* ------------------------------------------------------------------ */
/* the caret                                                           */
/* ------------------------------------------------------------------ */

void ed_goto( EDITOR *ed, WPOS pos, int select )
{
    ed->caret = wpos_clamp( ed->story, pos );
    if ( !select ) {
        ed->anchor = ed->caret;
    }
    moved( ed, 0 );
}

static WPOS word_right( const EDITOR *ed, WPOS pos )
{
    const WPARA *para = para_of( ed, pos.para );
    int kind;

    if ( pos.off >= para->len ) {
        return step_right( ed, pos );
    }
    kind = char_class( para->text[pos.off] );
    while ( pos.off < para->len && kind != 0 && char_class( para->text[pos.off] ) == kind ) {
        pos.off++;
    }
    while ( pos.off < para->len && char_class( para->text[pos.off] ) == 0 ) {
        pos.off++;
    }
    return pos;
}

static WPOS word_left( const EDITOR *ed, WPOS pos )
{
    const WPARA *para = para_of( ed, pos.para );
    int kind;

    if ( pos.off == 0 ) {
        return step_left( ed, pos );
    }
    while ( pos.off > 0 && char_class( para->text[pos.off - 1] ) == 0 ) {
        pos.off--;
    }
    if ( pos.off > 0 ) {
        kind = char_class( para->text[pos.off - 1] );
        while ( pos.off > 0 && char_class( para->text[pos.off - 1] ) == kind ) {
            pos.off--;
        }
    }
    return pos;
}

void ed_move( EDITOR *ed, int how, int select )
{
    LINFO info;
    WPOS pos = ed->caret, from, to, target;
    s32 x, y, height, jump, top, high, block_top, block_h;
    u32 line;
    int keep_x = 0;

    lay_update( ed->lay );
    lay_caret( ed->lay, ed->caret, &x, &y, &height );
    line = lay_line_of( ed->lay, ed->caret );
    if ( ed->want_x >= 0 ) {
        x = ed->want_x;
    }
    ed_sel( ed, &from, &to );
    switch ( how ) {
    case MV_LEFT:
        pos = ed_has_sel( ed ) && !select ? from : step_left( ed, pos );
        break;
    case MV_RIGHT:
        pos = ed_has_sel( ed ) && !select ? to : step_right( ed, pos );
        break;
    case MV_WORD_LEFT:
        pos = word_left( ed, pos );
        break;
    case MV_WORD_RIGHT:
        pos = word_right( ed, pos );
        break;
    case MV_UP:
    case MV_DOWN:
        keep_x = 1;
        if ( how == MV_UP ) {
            if ( line > 0 ) {
                pos = lay_hit_line( ed->lay, pos.para, line - 1, x );
            } else if ( pos.para > 0 ) {
                lay_para_box( ed->lay, pos.para, &top, &high, &block_top, &block_h );
                if ( (para_of( ed, pos.para - 1 )->pap.flags & PF_CELLEND) && block_top > 0 && block_h > 0 ) {
                    /* out of the top of a table's cell: to what is above
                       it on the page, which is not the cell before it */
                    target = lay_hit( ed->lay, x, block_top - 1 );
                    if ( wpos_cmp( target, pos ) < 0 ) {
                        pos = target;
                        break;
                    }
                }
                line = lay_lines( ed->lay, pos.para - 1 );
                pos = lay_hit_line( ed->lay, pos.para - 1, line ? line - 1 : 0, x );
            }
        } else if ( line + 1 < lay_lines( ed->lay, pos.para ) ) {
            pos = lay_hit_line( ed->lay, pos.para, line + 1, x );
        } else if ( pos.para + 1 < ed->story->count ) {
            lay_para_box( ed->lay, pos.para, &top, &high, &block_top, &block_h );
            if ( ((para_of( ed, pos.para )->pap.flags & PF_CELLEND) ||
                  ((para_of( ed, pos.para + 1 )->pap.flags & PF_INTABLE) &&
                   !(para_of( ed, pos.para )->pap.flags & PF_INTABLE))) && block_h > 0 ) {
                /* out of the foot of a cell, or into a table from above
                   it: to the cell that is below on the page */
                target = lay_hit( ed->lay, x, block_top + block_h + 1 );
                if ( wpos_cmp( target, pos ) > 0 ) {
                    pos = target;
                    break;
                }
            }
            pos = lay_hit_line( ed->lay, pos.para + 1, 0, x );
        }
        break;
    case MV_HOME:
        if ( lay_line( ed->lay, pos.para, line, &info ) ) {
            pos.off = info.start;
        }
        break;
    case MV_END:
        pos = lay_hit_line( ed->lay, pos.para, line, 0x3FFFFFFFL );
        break;
    case MV_PAGE_UP:
    case MV_PAGE_DOWN:
        keep_x = 1;
        jump = ed->view_h - height;
        if ( jump < height ) {
            jump = height;
        }
        if ( how == MV_PAGE_UP ) {
            jump = -jump;
        }
        y += jump;
        if ( y < 0 ) {
            y = 0;
        }
        pos = lay_hit( ed->lay, x, y + height / 2 );
        ed->scroll_y += jump;
        scroll_clamp( ed );
        break;
    case MV_TOP:
        pos.para = pos.off = 0;
        break;
    case MV_BOTTOM:
        pos = wpos_end( ed->story );
        break;
    case MV_PARA_UP:
        if ( pos.off > 0 ) {
            pos.off = 0;
        } else if ( pos.para > 0 ) {
            pos.para--;
        }
        break;
    case MV_PARA_DOWN:
        if ( pos.para + 1 < ed->story->count ) {
            pos.para++;
            pos.off = 0;
        } else {
            pos = wpos_end( ed->story );
        }
        break;
    }
    ed->caret = pos;
    if ( !select ) {
        ed->anchor = pos;
    }
    moved( ed, keep_x );
    if ( keep_x ) {
        ed->want_x = x;
    }
}

void ed_click( EDITOR *ed, s32 x, s32 y, int select )
{
    lay_update( ed->lay );
    ed_goto( ed, lay_hit( ed->lay, x, y ), select );
}

void ed_select_word( EDITOR *ed )
{
    const WPARA *para = para_of( ed, ed->caret.para );
    WPOS from = ed->caret, to = ed->caret;
    int kind;

    if ( para->len == 0 ) {
        return;
    }
    if ( from.off >= para->len ) {
        from.off = para->len - 1;
    }
    kind = char_class( para->text[from.off] );
    while ( from.off > 0 && char_class( para->text[from.off - 1] ) == kind ) {
        from.off--;
    }
    to.off = from.off;
    while ( to.off < para->len && char_class( para->text[to.off] ) == kind ) {
        to.off++;
    }
    ed->anchor = from;
    ed->caret = to;
    moved( ed, 0 );
}

void ed_select_para( EDITOR *ed )
{
    ed->anchor.para = ed->caret.para;
    ed->anchor.off = 0;
    if ( ed->caret.para + 1 < ed->story->count ) {
        ed->caret.para++;
        ed->caret.off = 0;
    } else {
        ed->caret.off = para_of( ed, ed->caret.para )->len;
    }
    moved( ed, 0 );
}

void ed_select_all( EDITOR *ed )
{
    ed->anchor.para = ed->anchor.off = 0;
    ed->caret = wpos_end( ed->story );
    moved( ed, 0 );
}

/* ------------------------------------------------------------------ */
/* typing                                                              */
/* ------------------------------------------------------------------ */

static void put_code( EDITOR *ed, u32 code )
{
    u16 units[2];
    u32 count = 1;

    if ( code >= 0x10000UL ) {
        code -= 0x10000UL;
        units[0] = (u16)(0xD800 + (code >> 10));
        units[1] = (u16)(0xDC00 + (code & 0x3FF));
        count = 2;
    } else {
        units[0] = (u16)code;
    }
    we_insert( ed->doc, ed->story, &ed->caret, units, count, wd_chp( ed->doc, &ed->type_chp ) );
}

void ed_type( EDITOR *ed, u32 code )
{
    CHP chp = ed->type_chp;
    int set = ed->type_set;
    WPOS next;

    if ( code == 0 || code == WCH_OBJ || (code < 0x20 && code != WCH_TAB) || code > 0x10FFFFUL ) {
        return;
    }
    if ( ed_has_sel( ed ) ) {
        act_begin( ed );
        drop_sel( ed );
    } else {
        we_begin_more( ed->doc, ed->story, ed->caret, type_kind );
        if ( ed->overtype && ed->caret.off < para_of( ed, ed->caret.para )->len ) {
            next = step_right( ed, ed->caret );
            we_delete( ed->doc, ed->story, ed->caret, next );
        }
    }
    put_code( ed, code );
    act_end( ed );
    /* the look that was being typed in carries on */
    ed->type_chp = chp;
    ed->type_set = set;
    if ( code == ' ' ) {
        type_kind ^= 3;                 /* the next word is another action */
    }
}

void ed_break( EDITOR *ed, u32 code )
{
    if ( code != WCH_LINE && code != WCH_PAGE ) {
        return;
    }
    act_begin( ed );
    drop_sel( ed );
    put_code( ed, code );
    act_end( ed );
    type_kind ^= 3;
}

void ed_enter( EDITOR *ed )
{
    const WPARA *para;
    PAP pap;
    CHP chp = ed->type_chp;
    int set = ed->type_set;

    act_begin( ed );
    drop_sel( ed );
    para = para_of( ed, ed->caret.para );
    if ( para->len == 0 && para->pap.list != LIST_NONE ) {
        /* Enter on an empty list item ends the list, as in Word */
        pap_init( &pap );
        we_set_pap( ed->doc, ed->story, ed->caret.para, ed->caret.para, PM_LIST | PM_LEFT | PM_FIRST, &pap, 0 );
    } else {
        we_split( ed->doc, ed->story, &ed->caret );
    }
    act_end( ed );
    ed->type_chp = chp;
    ed->type_set = set;
    type_kind ^= 3;
}

void ed_backspace( EDITOR *ed )
{
    const WPARA *para = para_of( ed, ed->caret.para );
    PAP pap;
    WPOS from;

    if ( !ed_has_sel( ed ) ) {
        if ( ed->caret.off == 0 && para->pap.list != LIST_NONE ) {
            /* at the start of a list item: the number goes first */
            pap_init( &pap );
            act_begin( ed );
            we_set_pap( ed->doc, ed->story, ed->caret.para, ed->caret.para, PM_LIST, &pap, 0 );
            act_end( ed );
            return;
        }
        if ( ed->caret.off == 0 && (ed->caret.para == 0 || !can_join( ed, ed->caret.para - 1 )) ) {
            return;
        }
        from = step_left( ed, ed->caret );
        ed->anchor = from;
    }
    act_begin( ed );
    drop_sel( ed );
    act_end( ed );
    moved( ed, 0 );
}

void ed_delete( EDITOR *ed )
{
    const WPARA *para = para_of( ed, ed->caret.para );

    if ( !ed_has_sel( ed ) ) {
        if ( ed->caret.off >= para->len && !can_join( ed, ed->caret.para ) ) {
            return;
        }
        ed->anchor = step_right( ed, ed->caret );
    }
    act_begin( ed );
    drop_sel( ed );
    act_end( ed );
    moved( ed, 0 );
}

void ed_delete_word( EDITOR *ed, int back )
{
    if ( !ed_has_sel( ed ) ) {
        ed->anchor = back ? word_left( ed, ed->caret ) : word_right( ed, ed->caret );
        if ( ed->anchor.para != ed->caret.para ) {
            ed->anchor = ed->caret;
            if ( back ) {
                ed_backspace( ed );
            } else {
                ed_delete( ed );
            }
            return;
        }
    }
    if ( ed_has_sel( ed ) ) {
        act_begin( ed );
        drop_sel( ed );
        act_end( ed );
        moved( ed, 0 );
    }
}

/* ------------------------------------------------------------------ */
/* the clipboard, undo                                                 */
/* ------------------------------------------------------------------ */

int ed_can_paste( void )
{
    return clip != NULL;
}

void ed_clip_free( void )
{
    wd_free( clip );
    clip = NULL;
}

void ed_copy( EDITOR *ed )
{
    WDOC *piece;
    WPOS from, to;

    if ( !ed_has_sel( ed ) ) {
        return;
    }
    ed_sel( ed, &from, &to );
    piece = we_copy( ed->doc, ed->story, from, to );
    if ( piece == NULL ) {
        ed->no_memory = 1;
        return;
    }
    wd_free( clip );
    clip = piece;
}

void ed_cut( EDITOR *ed )
{
    if ( !ed_has_sel( ed ) ) {
        return;
    }
    ed_copy( ed );
    if ( ed->no_memory ) {
        return;                         /* not copied: so not taken out either */
    }
    act_begin( ed );
    drop_sel( ed );
    act_end( ed );
    moved( ed, 0 );
}

void ed_paste( EDITOR *ed )
{
    if ( clip == NULL ) {
        return;
    }
    act_begin( ed );
    drop_sel( ed );
    we_paste( ed->doc, ed->story, &ed->caret, clip );
    act_end( ed );
    moved( ed, 0 );
}

void ed_insert_doc( EDITOR *ed, WDOC *other )
{
    if ( other == NULL || other == ed->doc ) {
        return;
    }
    act_begin( ed );
    drop_sel( ed );
    we_paste( ed->doc, ed->story, &ed->caret, other );
    act_end( ed );
    moved( ed, 0 );
}

void ed_insert_table( EDITOR *ed, int rows, int cols )
{
    lay_update( ed->lay );
    act_begin( ed );
    drop_sel( ed );
    we_insert_table( ed->doc, ed->story, &ed->caret, rows, cols, lay_width( ed->lay ) );
    act_end( ed );
    moved( ed, 0 );
}

int ed_in_table( const EDITOR *ed )
{
    return (para_of( ed, ed->caret.para )->pap.flags & PF_INTABLE) != 0;
}

/* an empty row below the one the caret is in; the caret goes to its
   first cell.  0 if the caret is not in a table's row. */
int ed_insert_row( EDITOR *ed )
{
    const STORY *story = ed->story;
    WPOS pos = ed->caret;
    u32 index = ed->caret.para;

    while ( index < story->count && (story->para[index]->pap.flags & (PF_INTABLE | PF_ROWEND)) == PF_INTABLE ) {
        index++;
    }
    if ( index >= story->count || !(story->para[index]->pap.flags & PF_ROWEND) ||
         story->para[index]->pap.row == NULL ) {
        return 0;
    }
    ed->anchor = ed->caret;
    act_begin( ed );
    we_add_row( ed->doc, ed->story, index, &pos );
    ed->caret = pos;
    act_end( ed );
    moved( ed, 0 );
    return 1;
}

/* what the table commands share: the change, as one action that Undo
   takes back, with the caret put where the change says.  1 if anything
   was changed. */
static int table_done( EDITOR *ed, u32 serial, WPOS pos )
{
    int changed = we_serial( ed->doc ) != serial;

    if ( changed ) {
        ed->caret = pos;
    }
    act_end( ed );
    moved( ed, 0 );
    return changed;
}

int ed_insert_row_above( EDITOR *ed )
{
    WPOS pos = ed->caret;
    u32 serial = we_serial( ed->doc );

    if ( !ed_in_table( ed ) ) {
        return 0;
    }
    ed->anchor = ed->caret;
    act_begin( ed );
    we_add_row_above( ed->doc, ed->story, ed->caret.para, &pos );
    return table_done( ed, serial, pos );
}

int ed_delete_rows( EDITOR *ed )
{
    WPOS pos = ed->caret;
    u32 serial = we_serial( ed->doc ), from = ed->anchor.para, to = ed->caret.para;

    if ( !ed_in_table( ed ) ) {
        return 0;
    }
    ed->anchor = ed->caret;
    act_begin( ed );
    we_delete_rows( ed->doc, ed->story, from, to, &pos );
    return table_done( ed, serial, pos );
}

int ed_insert_col( EDITOR *ed, int after )
{
    WPOS pos = ed->caret;
    u32 serial = we_serial( ed->doc );

    if ( !ed_in_table( ed ) ) {
        return 0;
    }
    lay_update( ed->lay );
    ed->anchor = ed->caret;
    act_begin( ed );
    we_insert_col( ed->doc, ed->story, ed->caret.para, after, lay_width( ed->lay ), &pos );
    return table_done( ed, serial, pos );
}

int ed_delete_col( EDITOR *ed )
{
    WPOS pos = ed->caret;
    u32 serial = we_serial( ed->doc );

    if ( !ed_in_table( ed ) ) {
        return 0;
    }
    ed->anchor = ed->caret;
    act_begin( ed );
    we_delete_col( ed->doc, ed->story, ed->caret.para, &pos );
    return table_done( ed, serial, pos );
}

s32 ed_col_width( const EDITOR *ed )
{
    return we_col_width( ed->story, ed->caret.para );
}

int ed_set_col_width( EDITOR *ed, s32 width )
{
    u32 serial = we_serial( ed->doc );

    if ( !ed_in_table( ed ) ) {
        return 0;
    }
    ed->anchor = ed->caret;
    act_begin( ed );
    we_set_col_width( ed->doc, ed->story, ed->caret.para, width );
    return table_done( ed, serial, ed->caret );
}

/* is paragraph "index" in a table, and not the last of its cell? */
static int cell_goes_on( const STORY *story, u32 index )
{
    return (story->para[index]->pap.flags & (PF_INTABLE | PF_CELLEND)) == PF_INTABLE;
}

int ed_cell_step( EDITOR *ed, int back )
{
    const STORY *story = ed->story;
    WPOS pos;
    u32 index = ed->caret.para;

    if ( !ed_in_table( ed ) ) {
        return 0;
    }
    if ( !back ) {
        /* past the end of this cell: the next one's first paragraph */
        while ( index + 1 < story->count && cell_goes_on( story, index ) ) {
            index++;
        }
        index++;
        if ( index >= story->count || !(story->para[index]->pap.flags & PF_INTABLE) ) {
            ed_insert_row( ed );        /* Tab in the table's last cell starts a new row */
            return 1;
        }
    } else {
        /* back to this cell's first paragraph, then to the first of the
           cell before it */
        while ( index > 0 && cell_goes_on( story, index - 1 ) ) {
            index--;
        }
        if ( index == 0 || !(story->para[index - 1]->pap.flags & PF_INTABLE) ) {
            return 1;
        }
        index--;
        while ( index > 0 && cell_goes_on( story, index - 1 ) ) {
            index--;
        }
    }
    /* what the cell holds is selected, so that typing replaces it */
    pos.para = index;
    pos.off = 0;
    ed_goto( ed, pos, 0 );
    while ( index + 1 < story->count && cell_goes_on( story, index ) ) {
        index++;
    }
    pos.para = index;
    pos.off = story->para[index]->len;
    ed_goto( ed, pos, 1 );
    return 1;
}

int ed_insert_image( EDITOR *ed, const u8 *data, u32 len, int fmt, s32 width, s32 height )
{
    int obj = wd_image_new( ed->doc, data, len, fmt, width, height );

    if ( obj < 0 ) {
        ed->doc->failed = 0;
        ed->no_memory = 1;
        return 0;
    }
    act_begin( ed );
    drop_sel( ed );
    we_insert_obj( ed->doc, ed->story, &ed->caret, obj, wd_chp( ed->doc, &ed->type_chp ) );
    act_end( ed );
    moved( ed, 0 );
    return !ed->no_memory;
}

/*
 * A footnote or an endnote, called from where the caret is in the
 * document's own text: -> its story, with one empty paragraph to type
 * in, or NULL (not in the document's own text, or no memory).
 */
STORY *ed_insert_note( EDITOR *ed, int kind )
{
    STORY *story;
    PAP pap;
    int obj;

    if ( ed->story != &ed->doc->body ) {
        return NULL;                    /* a note has no notes of its own */
    }
    obj = wd_note_new( ed->doc, kind );
    story = obj < 0 ? NULL : wd_note_story( ed->doc, obj );
    if ( story != NULL ) {
        pap_init( &pap );
        pap.style = (u8)wd_style( ed->doc, "Footnote Text", STY_FOOTNOTE );
        if ( wd_para_add( ed->doc, story, &pap ) == NULL ) {
            story = NULL;
        }
    }
    if ( story == NULL ) {
        ed->doc->failed = 0;
        ed->no_memory = 1;
        return NULL;
    }
    act_begin( ed );
    drop_sel( ed );
    we_insert_obj( ed->doc, ed->story, &ed->caret, obj, wd_chp( ed->doc, &ed->type_chp ) );
    act_end( ed );
    moved( ed, 0 );
    return ed->no_memory ? NULL : story;
}

/* the note whose mark is nearest the caret in its paragraph: the one
   just before it first, then the next after it; NULL if it has none */
STORY *ed_note_near( EDITOR *ed )
{
    const WPARA *para = para_of( ed, ed->caret.para );
    STORY *story;
    u32 off;
    int obj;

    if ( ed->story != &ed->doc->body ) {
        return NULL;
    }
    for ( off = ed->caret.off; off > 0; off-- ) {
        if ( para->text[off - 1] == WCH_OBJ ) {
            obj = wd_obj_at( para, off - 1 );
            story = obj < 0 ? NULL : wd_note_story( ed->doc, obj );
            if ( story != NULL && story->count ) {
                return story;
            }
        }
    }
    for ( off = ed->caret.off; off < para->len; off++ ) {
        if ( para->text[off] == WCH_OBJ ) {
            obj = wd_obj_at( para, off );
            story = obj < 0 ? NULL : wd_note_story( ed->doc, obj );
            if ( story != NULL && story->count ) {
                return story;
            }
        }
    }
    return NULL;
}

static void after_undo( EDITOR *ed, STORY *story, WPOS caret )
{
    if ( story != ed->story && story != NULL ) {
        ed_set_story( ed, story );
    }
    ed->caret = ed->anchor = caret;
    moved( ed, 0 );
}

void ed_undo( EDITOR *ed )
{
    STORY *story;
    WPOS caret;

    if ( we_undo( ed->doc, &story, &caret ) ) {
        after_undo( ed, story, caret );
    }
}

void ed_redo( EDITOR *ed )
{
    STORY *story;
    WPOS caret;

    if ( we_redo( ed->doc, &story, &caret ) ) {
        after_undo( ed, story, caret );
    }
}

/* ------------------------------------------------------------------ */
/* looks                                                               */
/* ------------------------------------------------------------------ */

void ed_set_chp( EDITOR *ed, u32 mask, const CHP *value, u16 flag_mask )
{
    WPOS from, to, caret = ed->caret, anchor = ed->anchor;

    if ( ed_has_sel( ed ) ) {
        ed_sel( ed, &from, &to );
        act_begin( ed );
        we_set_chp( ed->doc, ed->story, from, to, mask, value, flag_mask );
        act_end( ed );
        ed->caret = caret;              /* the selection stays */
        ed->anchor = anchor;
    }
    chp_apply( &ed->type_chp, mask, value, flag_mask );
    ed->type_set = 1;
    type_kind ^= 3;
}

void ed_toggle( EDITOR *ed, u16 flag )
{
    CHP value;
    WPOS from, to;
    u16 mask = flag;
    int on;

    if ( flag & (CF_SUPER | CF_SUB) ) {
        mask |= CF_SUPER | CF_SUB;      /* one or the other */
    }
    if ( flag & (CF_UNDER | CF_DUNDER | CF_WUNDER) ) {
        mask |= CF_UNDER | CF_DUNDER | CF_WUNDER;
    }
    if ( ed_has_sel( ed ) ) {
        ed_sel( ed, &from, &to );
        on = !we_all_have( ed->doc, ed->story, from, to, flag );
    } else {
        on = (ed->type_chp.flags & flag) != flag;
    }
    mem_set( &value, 0, sizeof( value ) );
    value.flags = (u16)(on ? flag : 0);
    ed_set_chp( ed, CM_FLAGS, &value, mask );
}

/* the paragraphs a paragraph command is for: the selection's, without
   the last one when the selection only just reaches its start */
static void para_span( const EDITOR *ed, u32 *first, u32 *last )
{
    WPOS from, to;

    ed_sel( ed, &from, &to );
    *first = from.para;
    *last = to.para;
    if ( to.para > from.para && to.off == 0 ) {
        (*last)--;
    }
}

void ed_set_pap( EDITOR *ed, u32 mask, const PAP *value, u16 flag_mask )
{
    WPOS caret = ed->caret, anchor = ed->anchor;
    u32 first, last;

    para_span( ed, &first, &last );
    act_begin( ed );
    we_set_pap( ed->doc, ed->story, first, last, mask, value, flag_mask );
    act_end( ed );
    ed->caret = caret;
    ed->anchor = anchor;
}

void ed_set_style( EDITOR *ed, int style )
{
    const WSTYLE *from;
    WPOS caret = ed->caret, anchor = ed->anchor, start, end;
    PAP pap;
    u32 first, last;

    if ( style < 0 || (u32)style >= ed->doc->nstyle ) {
        return;
    }
    from = &ed->doc->style[style];
    para_span( ed, &first, &last );
    pap = from->pap;
    pap.style = (u8)style;
    start.para = first;
    start.off = 0;
    end.para = last;
    end.off = para_of( ed, last )->len;
    act_begin( ed );
    we_set_pap( ed->doc, ed->story, first, last,
                PM_STYLE | PM_ALIGN | PM_LEFT | PM_RIGHT | PM_FIRST | PM_BEFORE | PM_AFTER | PM_LINE, &pap, 0 );
    we_set_chp( ed->doc, ed->story, start, end, CM_FONT | CM_SIZE | CM_COLOR | CM_FLAGS, &from->chp,
                CF_BOLD | CF_ITALIC | CF_UNDER | CF_DUNDER | CF_WUNDER | CF_ALLCAPS | CF_SMALLCAPS );
    act_end( ed );
    ed->caret = caret;
    ed->anchor = anchor;
    ed->type_chp = from->chp;
    ed->type_set = 1;
}

void ed_indent( EDITOR *ed, int more )
{
    WPOS caret = ed->caret, anchor = ed->anchor;
    PAP pap;
    u32 first, last, index;

    para_span( ed, &first, &last );
    act_begin( ed );
    for ( index = first; index <= last && index < ed->story->count; index++ ) {
        pap_init( &pap );
        pap.left = ed->story->para[index]->pap.left + (more ? 720 : -720);
        if ( pap.left < 0 ) {
            pap.left = 0;
        }
        if ( pap.left > 15840 ) {
            pap.left = 15840;
        }
        we_set_pap( ed->doc, ed->story, index, index, PM_LEFT, &pap, 0 );
    }
    act_end( ed );
    ed->caret = caret;
    ed->anchor = anchor;
}

void ed_cur_chp( EDITOR *ed, CHP *chp )
{
    const WPARA *para;
    WPOS from, to;

    if ( ed_has_sel( ed ) ) {
        ed_sel( ed, &from, &to );
        para = para_of( ed, from.para );
        if ( from.off < para->len ) {
            *chp = *wd_chp_get( ed->doc, para->attr[from.off] );
            return;
        }
    }
    *chp = ed->type_chp;
}

const PAP *ed_cur_pap( EDITOR *ed )
{
    return &para_of( ed, ed->caret.para )->pap;
}

/* ------------------------------------------------------------------ */
/* find and replace                                                    */
/* ------------------------------------------------------------------ */

int ed_find( EDITOR *ed, const u16 *what, u32 len, int flags )
{
    WPOS from, to, at, start;
    int found;

    if ( len == 0 ) {
        return 0;
    }
    ed_sel( ed, &from, &to );
    start = (flags & WF_BACK) ? from : to;
    if ( (flags & WF_BACK) && ed_has_sel( ed ) && to.para == from.para && to.off - from.off == len &&
         from.off + len > 0 ) {
        /* the match that is selected is not found again */
        start.off = from.off + len - 1;
    }
    found = we_find( ed->doc, ed->story, start, what, len, flags, &at );
    if ( !found ) {
        /* once round: from the other end */
        if ( flags & WF_BACK ) {
            start = wpos_end( ed->story );
        } else {
            start.para = start.off = 0;
        }
        found = we_find( ed->doc, ed->story, start, what, len, flags, &at );
    }
    if ( !found ) {
        return 0;
    }
    ed->anchor = at;
    ed->caret = at;
    ed->caret.off += len;
    moved( ed, 0 );
    return 1;
}

/* the text at a place replaced: the new text takes the look of the old */
static void replace_at( EDITOR *ed, WPOS at, u32 len, const u16 *with, u32 with_len )
{
    const WPARA *para = para_of( ed, at.para );
    WPOS end = at;
    u16 chp = at.off < para->len ? para->attr[at.off] : 0;

    end.off += len;
    we_delete( ed->doc, ed->story, at, end );
    ed->caret = at;
    we_insert( ed->doc, ed->story, &ed->caret, with, with_len, chp );
}

int ed_replace( EDITOR *ed, const u16 *what, u32 len, const u16 *with, u32 with_len, int flags )
{
    WPOS from, to, at;

    ed_sel( ed, &from, &to );
    if ( len == 0 || from.para != to.para || to.off - from.off != len ||
         !we_find( ed->doc, ed->story, from, what, len, flags & ~WF_BACK, &at ) || wpos_cmp( at, from ) != 0 ) {
        return 0;
    }
    act_begin( ed );
    replace_at( ed, from, len, with, with_len );
    act_end( ed );
    moved( ed, 0 );
    return !ed->no_memory;
}

u32 ed_replace_all( EDITOR *ed, const u16 *what, u32 len, const u16 *with, u32 with_len, int flags )
{
    WPOS at, from;
    u32 count = 0, serial;

    if ( len == 0 ) {
        return 0;
    }
    from.para = from.off = 0;
    act_begin( ed );
    while ( we_find( ed->doc, ed->story, from, what, len, flags & ~WF_BACK, &at ) ) {
        serial = ed->doc->serial;
        replace_at( ed, at, len, with, with_len );
        /* each change that is made counts one: fewer than there should
           be is a change that had no memory, and what was to be replaced
           is still there to be found for ever */
        if ( ed->doc->failed || ed->doc->serial - serial != (with_len ? 2u : 1u) ) {
            break;
        }
        /* on from after what was put in, so that it is never found in itself */
        from = ed->caret;
        count++;
    }
    act_end( ed );
    moved( ed, 0 );
    return ed->no_memory ? 0 : count;
}

/* ------------------------------------------------------------------ */
/* for the status bar                                                  */
/* ------------------------------------------------------------------ */

void ed_where( EDITOR *ed, u32 *page, u32 *pages, u32 *line, u32 *col )
{
    const LPAGE *start;
    LINFO info;
    u32 index, caret_line, count = 0, para;

    lay_update( ed->lay );
    index = lay_page_of( ed->lay, ed->caret );
    start = lay_page( ed->lay, index );
    caret_line = lay_line_of( ed->lay, ed->caret );
    *page = index + 1;                  /* which sheet it is: not the number printed on it */
    *pages = lay_pages( ed->lay );
    for ( para = start->para; para < ed->caret.para && para < ed->story->count; para++ ) {
        count += lay_lines( ed->lay, para ) - (para == start->para ? start->line : 0);
    }
    count += caret_line - (start->para == ed->caret.para ? start->line : 0);
    *line = count + 1;
    *col = 1;
    if ( lay_line( ed->lay, ed->caret.para, caret_line, &info ) && ed->caret.off >= info.start ) {
        *col = ed->caret.off - info.start + 1;
    }
}
