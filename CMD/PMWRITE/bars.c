/*
 * BARS.C - what is round the document: the title, the toolbar, the
 * formatting line, the ruler and the status bar.
 *
 * THE TOOLBAR is a graphics screen's: a row of buttons with pictures,
 * as Word for Windows has.  A text screen cannot show pictures, and
 * what it has in the toolbar's place is the FORMATTING LINE - the
 * style, the font and the size of the text at the caret, and B, I and
 * U for bold, italic and underlined - which Word for DOS called the
 * ribbon.  A graphics screen has that line too, under its buttons.
 * View > Toolbar takes both away and gives the document their lines.
 *
 * THE RULER shows the caret's paragraph: its indents, and its tabs.
 * On a text screen it is a line of characters, ten to the inch.
 */
#include "pw.h"

#define BTN_W           24      /* a button, in pixels */
#define BTN_H           24
#define BTN_GAP         7
#define RIBBON_STYLE    8       /* where the formatting line's fields start, */
#define RIBBON_STYLE_W  16      /* and how wide each is, in cells */
#define RIBBON_FONT     33
#define RIBBON_FONT_W   22
#define RIBBON_SIZE     62
#define RIBBON_SIZE_W   4
#define RIBBON_BIU      69

static int tool_x, tool_y, tool_w, tool_h;
static int held = -1;           /* the button the mouse is holding down */
static const char *message;     /* app_message's */

/* what the toolbar was last painted for: it is painted again when any
   of this changes */
static struct {
    u16 flags;
    u8  align, list;
    int undo, redo, paste, sel, held, doc;
} tool_seen;

void bars_init( void )
{
    mem_set( &tool_seen, 0xFF, sizeof( tool_seen ) );
}

void bars_changed( void )
{
    tool_seen.doc = -2;
}

void app_message( const char *text )
{
    message = text;
}

/* is a toggle button's thing on at the caret? */
static int is_on( int cmd )
{
    CHP chp;
    const PAP *pap;

    if ( cur_doc < 0 || cur_doc >= ndocs || ED == NULL ) {
        return 0;
    }
    ed_cur_chp( ED, &chp );
    pap = ed_cur_pap( ED );
    switch ( cmd ) {
    case CMD_BOLD:      return (chp.flags & CF_BOLD) != 0;
    case CMD_ITALIC:    return (chp.flags & CF_ITALIC) != 0;
    case CMD_UNDERLINE: return (chp.flags & CF_UNDER) != 0;
    case CMD_LEFT:      return pap->align == AL_LEFT;
    case CMD_CENTER:    return pap->align == AL_CENTER;
    case CMD_RIGHT:     return pap->align == AL_RIGHT;
    case CMD_JUSTIFY:   return pap->align == AL_JUSTIFY;
    case CMD_NUMBERING: return pap->list >= LIST_NUMBER;
    case CMD_BULLETS:   return pap->list == LIST_BULLET;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the toolbar                                                         */
/* ------------------------------------------------------------------ */

static int button_x( int index )
{
    int x = tool_x + 6, scan;

    for ( scan = 0; scan <= index; scan++ ) {
        if ( tool_buttons[scan].gap ) {
            x += BTN_GAP;
        }
        if ( scan < index ) {
            x += BTN_W + 1;
        }
    }
    return x;
}

static void tool_paint( int pane, int x0, int y0, int x1, int y1 )
{
    int index, x, y, down;

    (void)pane;
    gfx_rect( x0, y0, x1 - x0 + 1, y1 - y0 + 1, pal.px_face );
    gfx_hline( tool_x, tool_y, tool_w, pal.px_light );
    gfx_hline( tool_x, tool_y + tool_h - 1, tool_w, pal.px_shade );
    y = tool_y + (tool_h - BTN_H) / 2;
    for ( index = 0; index < tool_count; index++ ) {
        x = button_x( index );
        if ( x + BTN_W > tool_x + tool_w ) {
            break;                      /* the screen is not wide enough for the rest */
        }
        down = index == held || is_on( tool_buttons[index].cmd );
        if ( down ) {
            gfx_rect( x + 1, y + 1, BTN_W - 2, BTN_H - 2, 15 );
        }
        gfx_frame( x, y, BTN_W, BTN_H, down ? FR_SUNKEN : FR_RAISED );
        gfx_icon( x + 4 + down, y + 4 + down, tool_buttons[index].rows, pal.px_ink,
                  !app_can( tool_buttons[index].cmd ) );
    }
}

static int button_at( int x, int y )
{
    int index, left, top = tool_y + (tool_h - BTN_H) / 2;

    if ( y < top || y >= top + BTN_H ) {
        return -1;
    }
    for ( index = 0; index < tool_count; index++ ) {
        left = button_x( index );
        if ( x >= left && x < left + BTN_W && left + BTN_W <= tool_x + tool_w ) {
            return index;
        }
    }
    return -1;
}

static void tool_draw( void )
{
    CHP chp;
    const PAP *pap;
    static int defined;

    if ( !defined ) {
        pane_define( PANE_TOOLBAR, tool_paint );
        defined = 1;
    }
    if ( !pane_place( PANE_TOOLBAR, plan.tool_row, 0, plan.tool_rows, scr_cols ) ) {
        return;
    }
    pane_rect( PANE_TOOLBAR, &tool_x, &tool_y, &tool_w, &tool_h );
    mem_set( &chp, 0, sizeof( chp ) );
    pap = NULL;
    if ( ED != NULL ) {
        ed_cur_chp( ED, &chp );
        pap = ed_cur_pap( ED );
    }
    /* painted again only when a button would look different */
    if ( tool_seen.doc != cur_doc || tool_seen.flags != chp.flags || tool_seen.held != held ||
         tool_seen.align != (pap != NULL ? pap->align : 0) || tool_seen.list != (pap != NULL ? pap->list : 0) ||
         tool_seen.undo != app_can( CMD_UNDO ) || tool_seen.redo != app_can( CMD_REDO ) ||
         tool_seen.paste != app_can( CMD_PASTE ) || tool_seen.sel != app_can( CMD_CUT ) ) {
        pane_damage_all( PANE_TOOLBAR );
        tool_seen.doc = cur_doc;
        tool_seen.flags = chp.flags;
        tool_seen.held = held;
        tool_seen.align = (u8)(pap != NULL ? pap->align : 0);
        tool_seen.list = (u8)(pap != NULL ? pap->list : 0);
        tool_seen.undo = app_can( CMD_UNDO );
        tool_seen.redo = app_can( CMD_REDO );
        tool_seen.paste = app_can( CMD_PASTE );
        tool_seen.sel = app_can( CMD_CUT );
    }
}

/* a button pressed: it stays down while the pointer is on it, and is
   the command when the mouse lets go there */
static int tool_press( void )
{
    int index = button_at( mouse.x, mouse.y ), key;

    if ( index < 0 || !app_can( tool_buttons[index].cmd ) ) {
        return 0;
    }
    for ( ;; ) {
        held = button_at( mouse.x, mouse.y ) == index ? index : -1;
        ui_redraw();
        scr_flush();
        if ( key_testing() ) {
            break;                      /* a press in a key file is a whole click */
        }
        key = key_get();
        if ( key_service( key ) ) {
            continue;
        }
        if ( key != K_MOUSE || mouse.kind == ME_UP ) {
            break;
        }
    }
    key = held;
    held = -1;
    return key == index || key_testing() ? tool_buttons[index].cmd : 0;
}

/* ------------------------------------------------------------------ */
/* the formatting line                                                 */
/* ------------------------------------------------------------------ */

static void ribbon_field( int row, int col, int width, const char *text )
{
    scr_ch( row, col - 1, pal.brackets ? '[' : ' ', pal.ribbon );
    scr_putw( row, col, text, width, pal.field );
    scr_ch( row, col + width, pal.brackets ? ']' : ' ', pal.ribbon );
    if ( gfx_on ) {
        scr_ch( row, col + width - 1, 0x19, pal.field );
    }
}

static void ribbon_draw( void )
{
    EDITOR *ed = ED;
    CHP chp;
    char size[12];
    int row = plan.ribbon_row;

    scr_fill( row, 0, scr_cols, ' ', pal.ribbon );
    if ( ed == NULL ) {
        return;
    }
    ed_cur_chp( ed, &chp );
    scr_put( row, 1, "Style:", pal.ribbon );
    ribbon_field( row, RIBBON_STYLE, RIBBON_STYLE_W,
                  ed->doc->style[ed_cur_pap( ed )->style < ed->doc->nstyle ? ed_cur_pap( ed )->style : 0].name );
    scr_put( row, RIBBON_FONT - 7, "Font:", pal.ribbon );
    ribbon_field( row, RIBBON_FONT, RIBBON_FONT_W, ed->doc->font[chp.font < ed->doc->nfont ? chp.font : 0].name );
    scr_put( row, RIBBON_SIZE - 5, "Pts:", pal.ribbon );
    sfmt( size, sizeof( size ), (chp.size & 1) ? "%u.5" : "%u", (u32)chp.size / 2 );
    ribbon_field( row, RIBBON_SIZE, RIBBON_SIZE_W, size );
    if ( scr_cols >= RIBBON_BIU + 10 ) {
        scr_put( row, RIBBON_BIU, " B ", (chp.flags & CF_BOLD) ? pal.ribbon_hot : pal.ribbon );
        scr_put( row, RIBBON_BIU + 3, " I ", (chp.flags & CF_ITALIC) ? pal.ribbon_hot : pal.ribbon );
        scr_put( row, RIBBON_BIU + 6, " U ", (chp.flags & CF_UNDER) ? pal.ribbon_hot : pal.ribbon );
    }
}

static int ribbon_mouse( void )
{
    int col = mouse.col;

    if ( mouse.kind != ME_DOWN && mouse.kind != ME_DOUBLE ) {
        return 0;
    }
    if ( col >= RIBBON_STYLE - 1 && col <= RIBBON_STYLE + RIBBON_STYLE_W ) {
        return CMD_PICKSTYLE;
    }
    if ( col >= RIBBON_FONT - 1 && col <= RIBBON_FONT + RIBBON_FONT_W ) {
        return CMD_PICKFONT;
    }
    if ( col >= RIBBON_SIZE - 1 && col <= RIBBON_SIZE + RIBBON_SIZE_W ) {
        return CMD_PICKSIZE;
    }
    if ( col >= RIBBON_BIU && col < RIBBON_BIU + 9 ) {
        return col < RIBBON_BIU + 3 ? CMD_BOLD : col < RIBBON_BIU + 6 ? CMD_ITALIC : CMD_UNDERLINE;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the ruler                                                           */
/* ------------------------------------------------------------------ */

static int rule_x, rule_y, rule_w, rule_h;
static struct {
    s32 left, right, first, scroll, width;
    int zoom, ntabs, doc;
    u32 para;
} rule_seen;

static void rule_paint( int pane, int x0, int y0, int x1, int y1 )
{
    EDITOR *ed = ED;
    const PAP *pap;
    char digit[4];
    s32 width, pos;
    int origin, x, index, count;

    (void)pane;
    gfx_rect( x0, y0, x1 - x0 + 1, y1 - y0 + 1, pal.px_face );
    gfx_hline( rule_x, rule_y + rule_h - 1, rule_w, pal.px_shade );
    if ( ed == NULL ) {
        return;
    }
    pap = ed_cur_pap( ed );
    width = lay_width( ed->lay );
    origin = rule_x + 10 - view_px( ed->scroll_x );
    /* the text's width in white, an eighth of an inch to a mark */
    gfx_rect( origin, rule_y + 2, view_px( width ), rule_h - 5, pal.px_page );
    count = (int)(width / 180);
    for ( index = 0; index <= count; index++ ) {
        x = origin + view_px( (s32)index * 180 );
        if ( index % 8 == 0 ) {
            if ( index ) {
                sfmt( digit, sizeof( digit ), "%u", (u32)(index / 8) );
                gfx_text( x - 4 * (int)str_len( digit ), rule_y, digit, (int)str_len( digit ), pal.px_ink, -1 );
            }
        } else {
            gfx_vline( x, rule_y + (index % 4 == 0 ? 5 : 8), index % 4 == 0 ? 6 : 3, pal.px_shade );
        }
    }
    /* the paragraph's tabs, and its indents: the first line's above,
       the others' below, the right one's at the right */
    for ( index = 0; index < pap->ntabs && pap->tabs != NULL; index++ ) {
        x = origin + view_px( pap->tabs[index].pos );
        gfx_vline( x, rule_y + rule_h - 8, 5, pal.px_ink );
        gfx_hline( pap->tabs[index].kind == TAB_RIGHT ? x - 3 : pap->tabs[index].kind == TAB_LEFT ? x : x - 2,
                   rule_y + rule_h - 4, 4, pal.px_ink );
    }
    pos = pap->left + pap->first;
    x = origin + view_px( pos < 0 ? 0 : pos );
    for ( index = 0; index < 4; index++ ) {
        gfx_hline( x - 3 + index, rule_y + 1 + index, 7 - 2 * index, pal.px_ink );
    }
    x = origin + view_px( pap->left < 0 ? 0 : pap->left );
    for ( index = 0; index < 4; index++ ) {
        gfx_hline( x - index, rule_y + rule_h - 6 + index, 1 + 2 * index, pal.px_ink );
    }
    x = origin + view_px( width - pap->right );
    for ( index = 0; index < 4; index++ ) {
        gfx_hline( x - index, rule_y + rule_h - 6 + index, 1 + 2 * index, pal.px_ink );
    }
}

static void ruler_draw( void )
{
    EDITOR *ed = ED;
    const PAP *pap;
    s32 width;
    int row = plan.ruler_row, col, index, cols;
    static int defined;

    if ( !defined ) {
        pane_define( PANE_RULER, rule_paint );
        defined = 1;
    }
    if ( gfx_on ) {
        pane_place( PANE_RULER, row, 0, 1, scr_cols );
        pane_rect( PANE_RULER, &rule_x, &rule_y, &rule_w, &rule_h );
        if ( ed == NULL ) {
            return;
        }
        pap = ed_cur_pap( ed );
        if ( rule_seen.doc != cur_doc || rule_seen.left != pap->left || rule_seen.right != pap->right ||
             rule_seen.first != pap->first || rule_seen.scroll != ed->scroll_x || rule_seen.zoom != set.zoom ||
             rule_seen.ntabs != pap->ntabs || rule_seen.para != ed->caret.para ||
             rule_seen.width != lay_width( ed->lay ) ) {
            pane_damage_all( PANE_RULER );
            rule_seen.doc = cur_doc;
            rule_seen.left = pap->left;
            rule_seen.right = pap->right;
            rule_seen.first = pap->first;
            rule_seen.scroll = ed->scroll_x;
            rule_seen.zoom = set.zoom;
            rule_seen.ntabs = pap->ntabs;
            rule_seen.para = ed->caret.para;
            rule_seen.width = lay_width( ed->lay );
        }
        return;
    }
    /* a text screen: a dot a column, a digit at each inch */
    scr_fill( row, 0, scr_cols, ' ', pal.ribbon );
    if ( ed == NULL ) {
        return;
    }
    pap = ed_cur_pap( ed );
    width = lay_width( ed->lay );
    cols = (int)(width / GRID_W);
    for ( index = 0; index <= cols; index++ ) {
        col = 1 + index - (int)(ed->scroll_x / GRID_W);
        if ( col < 1 || col >= plan.doc_cols ) {
            continue;
        }
        scr_ch( row, col, index % 10 == 0 && index ? '0' + (index / 10) % 10 : index % 5 == 0 ? 0xC5 : 0xC4,
                pal.ribbon );
    }
    for ( index = 0; index < pap->ntabs && pap->tabs != NULL; index++ ) {
        col = 1 + (int)((pap->tabs[index].pos + GRID_W / 2 - ed->scroll_x) / GRID_W);
        if ( col >= 1 && col < plan.doc_cols ) {
            scr_ch( row, col, "LCRD"[pap->tabs[index].kind & 3], pal.ribbon_hot );
        }
    }
    col = 1 + (int)((pap->left + pap->first + GRID_W / 2 - ed->scroll_x) / GRID_W);
    if ( col >= 1 && col < plan.doc_cols ) {
        scr_ch( row, col, '|', pal.ribbon_hot );
    }
    col = 1 + (int)((pap->left + GRID_W / 2 - ed->scroll_x) / GRID_W);
    if ( col >= 1 && col < plan.doc_cols ) {
        scr_ch( row, col, '[', pal.ribbon_hot );
    }
    col = 1 + (int)((width - pap->right + GRID_W / 2 - ed->scroll_x) / GRID_W);
    if ( col >= 1 && col < plan.doc_cols ) {
        scr_ch( row, col, ']', pal.ribbon_hot );
    }
}

/* ------------------------------------------------------------------ */
/* the title and the status bar                                        */
/* ------------------------------------------------------------------ */

static void title_draw( void )
{
    char text[80];
    int len;

    scr_fill( plan.title_row, 0, scr_cols, ' ', pal.title );
    sfmt( text, sizeof( text ), "PMWRITE - %s", cur_doc >= 0 && cur_doc < ndocs ? docs[cur_doc].name : "" );
    len = (int)str_len( text );
    scr_put( plan.title_row, (scr_cols - len) / 2, text, pal.title );
}

static void status_draw( void )
{
    EDITOR *ed = ED;
    char text[96];
    u32 page, pages, line, col;
    int row = plan.status_row, right;

    scr_fill( row, 0, scr_cols, ' ', pal.status );
    if ( message != NULL ) {
        scr_putw( row, 1, message, scr_cols - 2, pal.status );
        return;
    }
    if ( cur_doc < 0 || cur_doc >= ndocs ) {
        return;
    }
    if ( ed != NULL ) {
        ed_where( ed, &page, &pages, &line, &col );
        sfmt( text, sizeof( text ), "Page %u of %u    Ln %u  Col %u", page, pages, line, col );
        scr_put( row, 1, text, pal.status );
        sfmt( text, sizeof( text ), "%s%s  %s  %u%%", ed->story == &ed->doc->header ? "Header  " :
              ed->story == &ed->doc->footer ? "Footer  " : ed->story != &ed->doc->body ? "Note  " : "", ed_modified( ed ) ? "Changed" : "       ",
              ed->overtype ? "OVR" : "INS", (u32)set.zoom );
    } else {
        sfmt( text, sizeof( text ), "Page %u of %u", (u32)PDFV->page + 1, (u32)PDFV->pages );
        scr_put( row, 1, text, pal.status );
        sfmt( text, sizeof( text ), "PDF  %u%%", (u32)(PDFV->pix_zoom ? PDFV->pix_zoom : 100) );
    }
    right = scr_cols - 1 - (int)str_len( text );
    if ( right > 40 ) {
        scr_put( row, right, text, pal.status );
    }
}

void bars_draw( void )
{
    if ( plan.title_row >= 0 ) {
        title_draw();
    }
    if ( plan.tool_rows > 0 ) {
        tool_draw();
    }
    if ( plan.ribbon_row >= 0 ) {
        ribbon_draw();
    }
    if ( plan.ruler_row >= 0 ) {
        ruler_draw();
    }
    if ( plan.status_row >= 0 ) {
        status_draw();
    }
}

int bars_mouse( void )
{
    if ( plan.tool_rows > 0 && mouse.row >= plan.tool_row && mouse.row < plan.tool_row + plan.tool_rows ) {
        return mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE ? tool_press() : 0;
    }
    if ( mouse.row == plan.ribbon_row ) {
        return ribbon_mouse();
    }
    if ( mouse.row == plan.ruler_row || mouse.row == plan.status_row || mouse.row == plan.title_row ) {
        return 0;
    }
    return -1;
}
