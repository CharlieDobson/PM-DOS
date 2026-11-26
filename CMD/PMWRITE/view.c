/*
 * VIEW.C - the document on the screen.
 *
 * ON A TEXT SCREEN a document is cells: LAY.C has set it for a screen
 * of characters ten to the inch, so a twip position divided by a
 * cell's size is a column and a row.  What cannot be shown as it is -
 * bold, italic, underlining - is shown by colour, as Word for DOS did.
 *
 * ON A GRAPHICS SCREEN it is type.  The page is a pane (DISP.H): the
 * screen layer asks for a rectangle of it to be painted, and paint
 * draws the lines that touch the rectangle - each piece of text into a
 * block of coverage (TF.H) which is then laid on the page in its
 * colour.  The selection is the same rectangle turned to its opposite
 * colours.  Because a keystroke changes one paragraph, only what is
 * below the top of that paragraph is painted again after one, and only
 * the paragraph itself when the text below it did not move.
 *
 * A PDF is neither: it is its pages as pictures (PDFVIEW.H), one at a
 * time, drawn once and kept as pixel numbers until another page or
 * size is asked for - or its words, on a text screen.
 */
#include "pw.h"
#include "img.h"
#include "pdfview.h"

#define PAD_X           10      /* pixels between the pane's edge and the text */
#define SEL_BAR         1       /* cells left of the text on a text screen */
#define IMG_SLOTS       6
#define IMG_BYTES       (1536UL * 1024)

static int32_t      scale;      /* pixels to a twip, 16.16 */
static uint8_t     *cover;      /* a line's worth of coverage */
static unsigned int cover_cap;
static int          page_x, page_y, page_w, page_h; /* the pane, in pixels */

/* what was on the screen when it was last drawn: what has changed
   since says what must be painted again */
static struct {
    int          doc;
    EDITOR      *ed;
    STORY       *story;
    unsigned int serial;
    int          scroll_x, scroll_y, height;
    WPOS         caret, anchor;
    int          zoom, marks;
    int          block_top, block_bottom;  /* the caret's paragraph */
    int          x, y, w, h;
    const uint8_t *pix;                    /* a PDF: the page's pixels */
} seen;

/* pictures made ready for the screen: pixel numbers at the size shown */
static struct {
    WDOC        *doc;
    int          obj, w, h;
    uint8_t     *pix;
    unsigned int used;
} images[IMG_SLOTS];
static unsigned int image_clock;

static int pdf_open_doc = -1;   /* the document whose file PDFVIEW has open */
static int pdf_wanted;          /* the page showing is still to be drawn */

void view_init( void )
{
    mem_set( &seen, 0, sizeof( seen ) );
    seen.doc = -1;
}

void view_reset( void )
{
    seen.doc = -1;
}

int view_px( int twips )
{
    /* further than any screen reaches is as far as the sum can go in
       32 bits at the largest zoom: a place that far off is off the
       screen either way */
    if ( twips > 120000L ) {
        twips = 120000L;
    } else if ( twips < -120000L ) {
        twips = -120000L;
    }
    return (int)((twips * scale) >> 16);
}

static int px_twips( int pixels )
{
    if ( scale <= 0 ) {
        return 0;
    }
    /* left of the text, or above it, is a length the other way: not the
       huge one that a negative number read as unsigned would be */
    if ( pixels < 0 ) {
        return -(int32_t)(((uint32_t)(-pixels) << 16) / (uint32_t)scale);
    }
    return (int32_t)(((uint32_t)pixels << 16) / (uint32_t)scale);
}

int view_reclaim( void )
{
    int index, any = 0;

    for ( index = 0; index < IMG_SLOTS; index++ ) {
        if ( images[index].pix != NULL ) {
            xfree( images[index].pix );
            images[index].pix = NULL;
            any = 1;
        }
    }
    for ( index = 0; index < ndocs; index++ ) {
        if ( docs[index].pdf != NULL && docs[index].pdf->pix != NULL && index != cur_doc ) {
            xfree( docs[index].pdf->pix );
            docs[index].pdf->pix = NULL;
            any = 1;
        }
    }
    return any;
}

/* ------------------------------------------------------------------ */
/* a text screen                                                       */
/* ------------------------------------------------------------------ */

/* the colours a look has on a text screen: Word for DOS's way of
   showing what a character cell cannot */
static uint8_t look_attr( const CHP *chp )
{
    uint8_t back = (uint8_t)(pal.text & 0xF0);
    uint8_t fore = (uint8_t)(pal.text & 0x0F);
    static const uint8_t near16[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint32_t color = chp->color;
    int bits;

    if ( chp->flags & CF_HIDDEN ) {
        return (uint8_t)(back | 0x08);
    }
    if ( color != COLOR_AUTO ) {
        bits = ((color & 0x800000UL) ? 4 : 0) | ((color & 0x008000UL) ? 2 : 0) | ((color & 0x000080UL) ? 1 : 0);
        fore = near16[bits];
        if ( (color & 0x404040UL) && bits ) {
            fore |= 8;
        }
        if ( fore == (back >> 4) ) {
            fore ^= 7;                  /* never the colour of the paper */
        }
    }
    if ( chp->flags & CF_BOLD ) {
        fore |= 0x08;
    }
    if ( color == COLOR_AUTO ) {
        if ( chp->flags & CF_ITALIC ) {
            fore = (uint8_t)((fore & 8) | ((back >> 4) == 2 ? 3 : 2));
        }
        if ( chp->flags & (CF_UNDER | CF_DUNDER | CF_WUNDER) ) {
            fore = (uint8_t)((fore & 8) | ((back >> 4) == 3 ? 6 : 3));
        }
        if ( chp->flags & (CF_SUPER | CF_SUB | CF_STRIKE) ) {
            fore = (uint8_t)((fore & 8) | ((back >> 4) == 5 ? 6 : 5));
        }
    }
    return (uint8_t)(back | fore);
}

static int screen_char( uint32_t code )
{
    char near_buf[8];

    if ( code == WCH_NBSP ) {
        return ' ';
    }
    if ( code < 0x80 ) {
        return code < 0x20 ? ' ' : (int)code;
    }
    cp_put( country.codepage ? country.codepage : CP_437, code, near_buf );
    return (uint8_t)near_buf[0];
}

typedef struct {
    EDITOR      *ed;
    int          row;
    unsigned int para;
    int          sel;
    WPOS         from, to;
} CELLCTX;

static int cell_selected( const CELLCTX *cc, unsigned int off )
{
    WPOS pos;

    if ( !cc->sel ) {
        return 0;
    }
    pos.para = cc->para;
    pos.off = off;
    return wpos_cmp( pos, cc->from ) >= 0 && wpos_cmp( pos, cc->to ) < 0;
}

static void cell_put( const CELLCTX *cc, int x, int ch, uint8_t attr, int selected )
{
    int col = (int)((x - cc->ed->scroll_x) / GRID_W) + SEL_BAR;

    if ( col >= SEL_BAR && col < plan.doc_cols ) {
        scr_ch( cc->row, col, ch, selected ? pal.text_sel : attr );
    }
}

static void cell_seg( void *ctx, const LSEG *seg )
{
    CELLCTX *cc = (CELLCTX *)ctx;
    const CHP *chp = wd_chp_get( cc->ed->doc, seg->chp );
    uint8_t attr = look_attr( chp );
    unsigned int index;
    int cells, code;

    switch ( seg->kind ) {
    case SEG_TEXT:
        for ( index = 0; index < seg->count; index++ ) {
            code = seg->text[index];
            if ( seg->upper && code >= 'a' && code <= 'z' ) {
                code -= 0x20;
            }
            cell_put( cc, seg->x + (int)index * GRID_W, screen_char( (uint32_t)code ), attr,
                      cell_selected( cc, seg->off + index ) );
        }
        break;
    case SEG_MADE:
        for ( index = 0; index < seg->count; index++ ) {
            cell_put( cc, seg->x + (int)index * GRID_W, screen_char( seg->text[index] ), attr,
                      cell_selected( cc, seg->off ) );
        }
        break;
    case SEG_TAB:
        cells = (int)(seg->width / GRID_W);
        for ( index = 0; (int)index < cells; index++ ) {
            cell_put( cc, seg->x + (int)index * GRID_W, seg->lead == LEAD_DOTS ? '.' : seg->lead ? '_' :
                      set.show_marks && index == 0 ? 0x1A : ' ', attr, cell_selected( cc, seg->off ) );
        }
        break;
    case SEG_OBJECT:
        cell_put( cc, seg->x, 0xFE, attr, cell_selected( cc, seg->off ) );
        break;
    }
}

static void text_draw( EDITOR *ed )
{
    CELLCTX cc;
    LINFO info;
    WPOS end;
    unsigned int pi, li, lines, page = 0, pages = lay_pages( ed->lay );
    int top, height, block_top, block_h, x, y, bottom = ed->scroll_y + ed->view_h;
    int row, col;

    for ( row = 0; row < plan.doc_rows; row++ ) {
        scr_fill( plan.doc_top + row, 0, plan.doc_cols, ' ', pal.text );
    }
    cc.ed = ed;
    cc.sel = ed_has_sel( ed );
    ed_sel( ed, &cc.from, &cc.to );
    for ( pi = lay_para_at( ed->lay, ed->scroll_y ); pi < ed->story->count; pi++ ) {
        lay_para_box( ed->lay, pi, &top, &height, &block_top, &block_h );
        if ( block_top >= bottom ) {
            break;
        }
        lines = lay_lines( ed->lay, pi );
        cc.para = pi;
        for ( li = 0; li < lines; li++ ) {
            if ( !lay_line( ed->lay, pi, li, &info ) || info.y + info.height <= ed->scroll_y || info.y >= bottom ) {
                continue;
            }
            /* a line more than one row high has its text on its first
               row, and that row may be the one scrolled off the top */
            if ( info.y < ed->scroll_y || (info.y - ed->scroll_y) / GRID_H >= plan.doc_rows ) {
                continue;
            }
            cc.row = plan.doc_top + (int)((info.y - ed->scroll_y) / GRID_H);
            /* a mark in the margin where a page starts */
            while ( page + 1 < pages && (lay_page( ed->lay, page + 1 )->para < pi ||
                    (lay_page( ed->lay, page + 1 )->para == pi && lay_page( ed->lay, page + 1 )->line <= li)) ) {
                page++;
                if ( lay_page( ed->lay, page )->para == pi && lay_page( ed->lay, page )->line == li ) {
                    scr_ch( cc.row, 0, 0xAF, (uint8_t)((pal.text & 0xF0) | 0x0E) );
                }
            }
            lay_segs( ed->lay, pi, li, cell_seg, &cc );
            if ( info.flags & LL_LAST ) {
                /* the paragraph's end, when it is selected or being shown */
                end.para = pi;
                end.off = ed->story->para[pi]->len;
                if ( (cc.sel && wpos_cmp( end, cc.from ) >= 0 && wpos_cmp( end, cc.to ) < 0) || set.show_marks ) {
                    lay_caret( ed->lay, end, &x, &y, &height );
                    cell_put( &cc, x, set.show_marks ? 0x14 : ' ', (uint8_t)((pal.text & 0xF0) | 0x08),
                              cc.sel && wpos_cmp( end, cc.from ) >= 0 && wpos_cmp( end, cc.to ) < 0 );
                }
            }
        }
    }
    lay_caret( ed->lay, ed->caret, &x, &y, &height );
    row = (int)((y - ed->scroll_y) / GRID_H);
    col = (int)((x - ed->scroll_x) / GRID_W) + SEL_BAR;
    if ( row >= 0 && row < plan.doc_rows && col >= 0 && col < plan.doc_cols ) {
        scr_cursor( plan.doc_top + row, col, ed->overtype ? CUR_BLOCK : CUR_LINE );
    } else {
        scr_cursor( 0, 0, CUR_HIDE );
    }
}

/* ------------------------------------------------------------------ */
/* pictures                                                            */
/* ------------------------------------------------------------------ */

static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* a colour's part nudged up or down by where the pixel is, so that the
   few colours a screen has mix into the ones between: "spread" is how
   far apart the screen's own are */
static int dither( int value, int xpos, int ypos, int spread )
{
    if ( value >= 250 || value <= 5 ) {
        return value;                   /* paper stays paper, and ink ink */
    }
    value += (bayer[ypos & 3][xpos & 3] * spread) / 16 - spread / 2;
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

/* rows of blue, green, red made pixel numbers for this screen */
static void bgr_to_pixels( const uint8_t *bgr, uint8_t *pix, int count, int xpos, int ypos )
{
    int index, spread;

    for ( index = 0; index < count; index++, bgr += 3 ) {
        /* 256 colours have greys ten apart and colours fifty-one apart;
           sixteen have four greys and little else */
        spread = gfx_colors != 256 ? 96 : bgr[0] == bgr[1] && bgr[1] == bgr[2] ? 10 : 51;
        pix[index] = (uint8_t)gfx_rgb( ((uint32_t)dither( bgr[2], xpos + index, ypos, spread ) << 16) |
                                       ((uint32_t)dither( bgr[1], xpos + index, ypos, spread ) << 8) |
                                       (uint32_t)dither( bgr[0], xpos + index, ypos, spread ) );
    }
}

static uint8_t *image_pixels( WDOC *doc, int obj, int w, int h )
{
    const WOBJ *from = &doc->obj[obj];
    uint8_t *bgr, *pix, *line;
    uint32_t pic_w, pic_h;
    unsigned int total = 0;
    int index, slot = 0, xpos, ypos;

    for ( index = 0; index < IMG_SLOTS; index++ ) {
        if ( images[index].pix != NULL && images[index].doc == doc && images[index].obj == obj &&
             images[index].w == w && images[index].h == h ) {
            images[index].used = ++image_clock;
            return images[index].pix;
        }
    }
    if ( w <= 0 || h <= 0 || w > 2048 || h > 2048 ) {
        return NULL;
    }
    /* the one longest unused makes room; so do others while there is too much */
    for ( index = 0; index < IMG_SLOTS; index++ ) {
        if ( images[index].pix == NULL ) {
            slot = index;
            break;
        }
        total += (unsigned int)images[index].w * (unsigned int)images[index].h;
        if ( images[index].used < images[slot].used ) {
            slot = index;
        }
    }
    if ( total + (unsigned int)w * (unsigned int)h > IMG_BYTES ) {
        view_reclaim();
        slot = 0;
    }
    xfree( images[slot].pix );
    images[slot].pix = NULL;
    bgr = img_decode( from->data, from->len, &pic_w, &pic_h );
    if ( bgr == NULL ) {
        return NULL;
    }
    pix = (uint8_t *)try_alloc( (unsigned int)w * (unsigned int)h );
    line = (uint8_t *)try_alloc( (unsigned int)w * 3 );
    if ( pix != NULL && line != NULL ) {
        for ( ypos = 0; ypos < h; ypos++ ) {
            const uint8_t *src = bgr + (unsigned int)ypos * pic_h / (unsigned int)h * pic_w * 3;

            for ( xpos = 0; xpos < w; xpos++ ) {
                mem_cpy( line + xpos * 3, src + (unsigned int)xpos * pic_w / (unsigned int)w * 3, 3 );
            }
            bgr_to_pixels( line, pix + (unsigned int)ypos * (unsigned int)w, w, 0, ypos );
        }
    } else {
        xfree( pix );
        pix = NULL;
    }
    xfree( line );
    xfree( bgr );
    images[slot].doc = doc;
    images[slot].obj = obj;
    images[slot].w = w;
    images[slot].h = h;
    images[slot].pix = pix;
    images[slot].used = ++image_clock;
    return pix;
}

/* ------------------------------------------------------------------ */
/* a graphics screen                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    EDITOR *ed;
    int     top, height, base;  /* the line, in screen pixels */
} PIXCTX;

static int cover_room( unsigned int bytes )
{
    uint8_t *grown;

    if ( bytes <= cover_cap ) {
        return 1;
    }
    grown = (uint8_t *)try_realloc( cover, bytes + 1024 );
    if ( grown == NULL ) {
        return 0;
    }
    cover = grown;
    cover_cap = bytes + 1024;
    return 1;
}

static void pix_text( const PIXCTX *pc, const LSEG *seg, const CHP *chp, int fg, int bg )
{
    COVER block;
    TFACE *face = tf_face( &pc->ed->doc->font[chp->font < pc->ed->doc->nfont ? chp->font : 0], chp->flags );
    uint16_t piece[96];
    int32_t pen, em = seg->size * scale, start;
    unsigned int done = 0, part, index;
    int xpix, width;

    start = (seg->x - pc->ed->scroll_x) * scale + ((int32_t)(page_x + PAD_X) << 16);
    xpix = (int)(start >> 16);
    width = (int)(((seg->width * scale) + (start & 0xFFFF)) >> 16) + 2;
    if ( xpix + width <= page_x || xpix >= page_x + page_w || width <= 0 || pc->height <= 0 ||
         !cover_room( (unsigned int)width * (unsigned int)pc->height ) ) {
        return;
    }
    mem_set( cover, 0, (unsigned int)width * (unsigned int)pc->height );
    block.pix = cover;
    block.width = width;
    block.height = pc->height;
    block.stride = (unsigned int)width;
    pen = start & 0xFFFF;
    while ( done < seg->count ) {
        part = seg->count - done > 96 ? 96 : seg->count - done;
        /* a pair of units that make one character is not split */
        if ( part == 96 && seg->text[done + 95] >= 0xD800 && seg->text[done + 95] <= 0xDBFF ) {
            part = 95;
        }
        for ( index = 0; index < part; index++ ) {
            piece[index] = seg->text[done + index];
            if ( seg->upper && ((piece[index] >= 'a' && piece[index] <= 'z') ||
                                (piece[index] >= 0xE0 && piece[index] <= 0xFE && piece[index] != 0xF7)) ) {
                piece[index] -= 0x20;
            }
            if ( piece[index] == WCH_NBSP ) {
                piece[index] = ' ';
            } else if ( piece[index] == WCH_NBHYPHEN ) {
                piece[index] = '-';
            }
        }
        pen = tf_draw( face, em, pen, ((int32_t)(pc->base - pc->top) << 16) - seg->rise * scale, piece, part, &block );
        done += part;
    }
    gfx_cover( xpix, pc->top, width - 2, pc->height, cover, width, fg, bg );
}

static void pix_seg( void *ctx, const LSEG *seg )
{
    PIXCTX *pc = (PIXCTX *)ctx;
    EDITOR *ed = pc->ed;
    const CHP *chp = wd_chp_get( ed->doc, seg->chp );
    uint8_t *pix;
    int xpix = page_x + PAD_X + view_px( seg->x - ed->scroll_x ), wide = view_px( seg->width );
    int fg = chp->color == COLOR_AUTO ? pal.px_ink : gfx_rgb( chp->color );
    int bg = pal.px_page, em = view_px( seg->size ), thick, base, high, step;

    if ( xpix >= page_x + page_w || xpix + wide < page_x ) {
        return;
    }
    if ( chp->hilite ) {
        bg = 14;                        /* a marker's yellow */
        gfx_rect( xpix, pc->top, wide, pc->height, bg );
    }
    base = pc->base - view_px( seg->rise );
    thick = em / 16 > 0 ? em / 16 : 1;
    switch ( seg->kind ) {
    case SEG_TEXT:
    case SEG_MADE:
        pix_text( pc, seg, chp, fg, bg );
        break;
    case SEG_TAB:
        if ( seg->lead == LEAD_LINE ) {
            gfx_rect( xpix, base + 1, wide, thick, fg );
        } else if ( seg->lead != LEAD_NONE ) {
            for ( step = 2; step < wide - 2; step += seg->lead == LEAD_DOTS ? 4 : 6 ) {
                gfx_rect( xpix + step, seg->lead == LEAD_DOTS ? base - thick : base - em / 4,
                          seg->lead == LEAD_DOTS ? thick : 3, thick, fg );
            }
        }
        break;
    case SEG_OBJECT:
        high = view_px( seg->height );
        pix = seg->obj >= 0 ? image_pixels( ed->doc, seg->obj, wide, high ) : NULL;
        if ( pix != NULL ) {
            gfx_blit( xpix, pc->base - high, wide, high, pix, wide );
        } else {
            gfx_rect( xpix, pc->base - high, wide, high, 7 );
            gfx_outline( xpix, pc->base - high, wide, high, 8 );
        }
        return;
    }
    if ( chp->flags & (CF_UNDER | CF_DUNDER | CF_WUNDER) ) {
        gfx_rect( xpix, base + thick + 1, wide, thick, fg );
        if ( chp->flags & CF_DUNDER ) {
            gfx_rect( xpix, base + 3 * thick + 1, wide, thick, fg );
        }
    }
    if ( chp->flags & CF_STRIKE ) {
        gfx_rect( xpix, base - em * 3 / 10, wide, thick, fg );
    }
}

/* the part of a line that is selected, turned to its opposite colours */
static void pix_selection( EDITOR *ed, unsigned int pi, unsigned int li, const LINFO *info, const PIXCTX *pc )
{
    WPOS from, to, start, end;
    int x0, x1, y, height;

    ed_sel( ed, &from, &to );
    start.para = end.para = pi;
    start.off = info->start;
    end.off = info->start + info->len;
    if ( wpos_cmp( to, start ) <= 0 || wpos_cmp( from, end ) > 0 ||
         (wpos_cmp( from, end ) == 0 && !(info->flags & LL_LAST)) ) {
        return;
    }
    x0 = info->x;
    if ( wpos_cmp( from, start ) > 0 ) {
        lay_caret( ed->lay, from, &x0, &y, &height );
    }
    if ( wpos_cmp( to, end ) < 0 || (wpos_cmp( to, end ) == 0 && lay_line_of( ed->lay, to ) == li) ) {
        lay_caret( ed->lay, to, &x1, &y, &height );
        /* a place that is the start of the next line is this line's end */
        if ( lay_line_of( ed->lay, to ) != li ) {
            x1 = info->x + info->width;
        }
    } else {
        /* it runs on past the line: the line's text and a little more,
           for the space or the paragraph's end that is selected too */
        x1 = info->x + info->width + 90;
    }
    if ( x1 > x0 ) {
        gfx_invert( page_x + PAD_X + view_px( x0 - ed->scroll_x ), pc->top,
                    view_px( x1 - ed->scroll_x ) - view_px( x0 - ed->scroll_x ), pc->height );
    }
}

#define BORDER_GAP      20      /* between a paragraph's text and a line round it, in twips */

/* the lines a paragraph has above, below and beside it: from its first
   line's top to its last line's foot, between its indents */
static void para_borders( EDITOR *ed, unsigned int pi )
{
    const PAP *pap = &ed->story->para[pi]->pap;
    LINFO first, last;
    int col_x, col_w;
    unsigned int lines = lay_lines( ed->lay, pi );
    int left, right, top, bottom;

    if ( !(pap->flags & (PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT)) || lines == 0 ||
         !lay_line( ed->lay, pi, 0, &first ) || !lay_line( ed->lay, pi, lines - 1, &last ) ) {
        return;
    }
    lay_para_col( ed->lay, pi, &col_x, &col_w );
    left = page_x + PAD_X + view_px( col_x + (pap->left > 0 ? pap->left : 0) - BORDER_GAP - ed->scroll_x ) - 2;
    right = page_x + PAD_X + view_px( col_x + col_w - (pap->right > 0 ? pap->right : 0) + BORDER_GAP - ed->scroll_x ) + 2;
    top = page_y + view_px( first.y - ed->scroll_y ) - 1;
    bottom = page_y + view_px( last.y + last.height - ed->scroll_y ) + 1;
    if ( right <= left || bottom <= top ) {
        return;
    }
    if ( pap->flags & PF_BORDER_TOP ) {
        gfx_hline( left, top, right - left + 1, pal.px_ink );
    }
    if ( pap->flags & PF_BORDER_BOT ) {
        gfx_hline( left, bottom, right - left + 1, pal.px_ink );
    }
    if ( pap->flags & PF_BORDER_LEFT ) {
        gfx_vline( left, top, bottom - top + 1, pal.px_ink );
    }
    if ( pap->flags & PF_BORDER_RIGHT ) {
        gfx_vline( right, top, bottom - top + 1, pal.px_ink );
    }
}

static void page_paint( int pane, int x0, int y0, int x1, int y1 )
{
    EDITOR *ed;
    PIXCTX pc;
    LINFO info;
    int edges[MAX_CELLS + 1], top, height, block_top, block_h, clip_top, clip_bottom, rule_top = -1;
    unsigned int pi, li, lines, page = 0, pages;
    int count, index, xpix, ypix, selected;

    (void)pane;
    gfx_rect( x0, y0, x1 - x0 + 1, y1 - y0 + 1, pal.px_page );
    if ( cur_doc < 0 || cur_doc >= ndocs || ED == NULL ) {
        return;
    }
    ed = ED;
    pc.ed = ed;
    selected = ed_has_sel( ed );
    pages = lay_pages( ed->lay );
    clip_top = ed->scroll_y + px_twips( y0 - page_y ) - 30;
    clip_bottom = ed->scroll_y + px_twips( y1 - page_y + 1 ) + 30;
    for ( pi = lay_para_at( ed->lay, clip_top ); pi < ed->story->count; pi++ ) {
        lay_para_box( ed->lay, pi, &top, &height, &block_top, &block_h );
        if ( block_top > clip_bottom ) {
            break;
        }
        lines = lay_lines( ed->lay, pi );
        for ( li = 0; li < lines; li++ ) {
            if ( !lay_line( ed->lay, pi, li, &info ) || info.y + info.height < clip_top || info.y > clip_bottom ) {
                continue;
            }
            pc.top = page_y + view_px( info.y - ed->scroll_y );
            pc.height = view_px( info.y + info.height - ed->scroll_y ) - view_px( info.y - ed->scroll_y );
            pc.base = pc.top + view_px( info.base );
            /* a dotted line across where a page starts */
            while ( page + 1 < pages && (lay_page( ed->lay, page + 1 )->para < pi ||
                    (lay_page( ed->lay, page + 1 )->para == pi && lay_page( ed->lay, page + 1 )->line <= li)) ) {
                page++;
                if ( lay_page( ed->lay, page )->para == pi && lay_page( ed->lay, page )->line == li ) {
                    for ( xpix = page_x; xpix < page_x + page_w; xpix += 4 ) {
                        gfx_hline( xpix, pc.top, 2, 8 );
                    }
                }
            }
            lay_segs( ed->lay, pi, li, pix_seg, &pc );
            if ( selected ) {
                pix_selection( ed, pi, li, &info, &pc );
            }
        }
    }
    /* the tables' rules go on last: a line of text is painted as a whole
       box, and that box starts where its cell's top rule is */
    for ( pi = lay_para_at( ed->lay, clip_top ); pi < ed->story->count; pi++ ) {
        lay_para_box( ed->lay, pi, &top, &height, &block_top, &block_h );
        if ( block_top > clip_bottom ) {
            break;
        }
        para_borders( ed, pi );
        if ( lay_row_rules( ed->lay, pi, &top, &height, edges, &count ) && top != rule_top ) {
            rule_top = top;
            ypix = page_y + view_px( top - ed->scroll_y );
            for ( index = 0; index <= count; index++ ) {
                gfx_vline( page_x + PAD_X + view_px( edges[index] - ed->scroll_x ), ypix, view_px( height ) + 1,
                           pal.px_ink );
            }
            xpix = page_x + PAD_X + view_px( edges[0] - ed->scroll_x );
            gfx_hline( xpix, ypix, view_px( edges[count] - edges[0] ) + 1, pal.px_ink );
            gfx_hline( xpix, ypix + view_px( height ), view_px( edges[count] - edges[0] ) + 1, pal.px_ink );
        }
    }
}

/* what must be painted again, from what has changed since last time */
static void page_damage( EDITOR *ed )
{
    int top, height, block_top, block_h, y0, y1, x, y, high;
    int whole = 0;

    lay_para_box( ed->lay, ed->caret.para, &top, &height, &block_top, &block_h );
    if ( seen.doc != cur_doc || seen.ed != ed || seen.story != ed->story || seen.scroll_x != ed->scroll_x ||
         seen.scroll_y != ed->scroll_y || seen.zoom != set.zoom || seen.marks != set.show_marks ||
         seen.x != page_x || seen.y != page_y || seen.w != page_w || seen.h != page_h ) {
        whole = 1;
    } else if ( seen.serial != ed->doc->serial ) {
        y0 = seen.block_top < block_top ? seen.block_top : block_top;
        y1 = seen.block_bottom > block_top + block_h ? seen.block_bottom : block_top + block_h;
        if ( seen.height != lay_height( ed->lay ) ) {
            y1 = ed->scroll_y + ed->view_h;     /* everything below it moved */
        }
        pane_damage( PANE_PAGE, page_x, page_y + view_px( y0 - ed->scroll_y ) - 2, page_x + page_w - 1,
                     page_y + view_px( y1 - ed->scroll_y ) + 2 );
    } else if ( wpos_cmp( seen.caret, ed->caret ) != 0 || wpos_cmp( seen.anchor, ed->anchor ) != 0 ) {
        if ( wpos_cmp( seen.caret, seen.anchor ) != 0 || ed_has_sel( ed ) ) {
            /* the selection changed between the old caret and the new */
            lay_caret( ed->lay, wpos_clamp( ed->story, seen.caret ), &x, &y0, &high );
            y1 = y0 + high;
            lay_caret( ed->lay, ed->caret, &x, &y, &high );
            if ( y < y0 ) {
                y0 = y;
            }
            if ( y + high > y1 ) {
                y1 = y + high;
            }
            if ( wpos_cmp( seen.caret, seen.anchor ) != 0 && !ed_has_sel( ed ) ) {
                /* it was dropped: all of it shows as it did */
                lay_caret( ed->lay, wpos_clamp( ed->story, seen.anchor ), &x, &y, &high );
                if ( y < y0 ) {
                    y0 = y;
                }
                if ( y + high > y1 ) {
                    y1 = y + high;
                }
            }
            pane_damage( PANE_PAGE, page_x, page_y + view_px( y0 - ed->scroll_y ) - 1, page_x + page_w - 1,
                         page_y + view_px( y1 - ed->scroll_y ) + 1 );
        }
    }
    if ( whole ) {
        pane_damage_all( PANE_PAGE );
    }
    seen.doc = cur_doc;
    seen.ed = ed;
    seen.story = ed->story;
    seen.serial = ed->doc->serial;
    seen.scroll_x = ed->scroll_x;
    seen.scroll_y = ed->scroll_y;
    seen.height = lay_height( ed->lay );
    seen.caret = ed->caret;
    seen.anchor = ed->anchor;
    seen.zoom = set.zoom;
    seen.marks = set.show_marks;
    seen.block_top = block_top;
    seen.block_bottom = block_top + block_h;
    seen.x = page_x;
    seen.y = page_y;
    seen.w = page_w;
    seen.h = page_h;
}

/* ------------------------------------------------------------------ */
/* a PDF                                                               */
/* ------------------------------------------------------------------ */

void view_free_pdf( PDFDOCVIEW *pdf )
{
    if ( pdf != NULL ) {
        xfree( pdf->pix );
        xfree( pdf->lines );
        xfree( pdf );
    }
    pdf_open_doc = -1;
    pdfview_close();
}

/* PDFVIEW has one file open: this document's */
static int pdf_ready( void )
{
    PDFDOCVIEW *pdf = PDFV;
    int rc;

    if ( pdf_open_doc == cur_doc && pdfview_is_open() ) {
        return 1;
    }
    rc = pdfview_open( docs[cur_doc].path );
    if ( rc == PV_PASSWORD ) {
        rc = pdfview_password( pdf->password, str_len( pdf->password ) );
    }
    pdf_open_doc = rc == PV_OK ? cur_doc : -1;
    return rc == PV_OK;
}

static int pdf_zoom_now( PDFDOCVIEW *pdf )
{
    int32_t width, height;
    int zoom = pdf->zoom;

    if ( zoom == 0 ) {
        /* as wide as the window */
        zoom = 100;
        if ( pdfview_page_size( pdf->page, &width, &height ) == PV_OK && width > 0x10000 ) {
            zoom = (int)((uint32_t)(page_w - 20) * 75 / (uint32_t)(width >> 16));
        }
    }
    return zoom < 10 ? 10 : zoom > 400 ? 400 : zoom;
}

/* the page as pixel numbers, drawn if it has not been: in strips when
   there is no memory for the whole page in three bytes a pixel */
static int pdf_render( PDFDOCVIEW *pdf )
{
    PVBLOCK block;
    uint8_t *strip;
    int zoom = pdf_zoom_now( pdf ), width, height, rows, top, line, rc = PV_OK;

    if ( pdf->pix != NULL && pdf->pix_page == pdf->page && pdf->pix_zoom == zoom ) {
        return 1;
    }
    xfree( pdf->pix );
    pdf->pix = NULL;
    /* this page at this size has been tried, whatever comes of it: one
       that cannot be drawn is not tried again on every turn */
    pdf->pix_page = pdf->page;
    pdf->pix_zoom = zoom;
    pdf->pix_failed = 1;
    if ( pdfview_page_pixels( pdf->page, PV_ZOOM( 96, zoom ), 0, &width, &height ) != PV_OK || width <= 0 ||
         height <= 0 || width > 4096 || height > 8192 ) {
        return 0;
    }
    mouse_shape( MP_WAIT );
    pdf->pix = (uint8_t *)try_alloc( (unsigned int)width * (unsigned int)height );
    strip = NULL;
    for ( rows = height; rows >= 16 && pdf->pix != NULL; rows /= 2 ) {
        strip = (uint8_t *)try_alloc( (unsigned int)width * (unsigned int)rows * 3 );
        if ( strip != NULL ) {
            break;
        }
    }
    if ( pdf->pix == NULL || strip == NULL ) {
        xfree( strip );
        xfree( pdf->pix );
        pdf->pix = NULL;
        mouse_shape( MP_ARROW );
        return 0;
    }
    for ( top = 0; top < height && (rc == PV_OK || top == 0); top += rows ) {
        block.pix = strip;
        block.width = width;
        block.height = top + rows > height ? height - top : rows;
        block.bpp = 3;
        block.stride = (unsigned int)width * 3;
        block.org_x = 0;
        block.org_y = top;
        rc = pdfview_render( pdf->page, PV_ZOOM( 96, zoom ), 0, &block );
        for ( line = 0; line < block.height; line++ ) {
            bgr_to_pixels( strip + (unsigned int)line * block.stride, pdf->pix + (unsigned int)(top + line) * (unsigned int)width, width, 0,
                           top + line );
        }
    }
    xfree( strip );
    pdf->pix_w = width;
    pdf->pix_h = height;
    pdf->pix_failed = 0;
    mouse_shape( MP_ARROW );
    if ( rc == PV_NOMEM ) {
        app_message( "There is not enough memory to draw all of this page." );
    } else if ( rc != PV_OK ) {
        app_message( "Part of this page could not be read." );
    }
    return 1;
}

static void pdf_paint( int x0, int y0, int x1, int y1 )
{
    PDFDOCVIEW *pdf = PDFV;
    int left, top;

    gfx_rect( x0, y0, x1 - x0 + 1, y1 - y0 + 1, pal.px_desk );
    if ( pdf == NULL || pdf->pix == NULL ) {
        return;
    }
    left = pdf->pix_w < page_w ? page_x + (page_w - pdf->pix_w) / 2 : page_x - (int)pdf->scroll_x;
    top = page_y + 8 - (int)pdf->scroll_y;
    gfx_blit( left, top, pdf->pix_w, pdf->pix_h, pdf->pix, pdf->pix_w );
    gfx_outline( left - 1, top - 1, pdf->pix_w + 2, pdf->pix_h + 2, pal.px_frame );
}

static void pane_paint( int pane, int x0, int y0, int x1, int y1 )
{
    if ( cur_doc >= 0 && cur_doc < ndocs && PDFV != NULL ) {
        pdf_paint( x0, y0, x1, y1 );
    } else {
        page_paint( pane, x0, y0, x1, y1 );
    }
}

static void pdf_draw( void )
{
    PDFDOCVIEW *pdf = PDFV;
    int row, rc, most;

    if ( !pdf_ready() ) {
        for ( row = 0; row < plan.doc_rows; row++ ) {
            scr_fill( plan.doc_top + row, 0, plan.doc_cols + 1, ' ', pal.text );
        }
        scr_put( plan.doc_top + 1, 2, "This file can no longer be read.", pal.text );
        return;
    }
    if ( gfx_on ) {
        pane_place( PANE_PAGE, plan.doc_top, 0, plan.doc_rows, plan.doc_cols );
        pane_rect( PANE_PAGE, &page_x, &page_y, &page_w, &page_h );
        pane_pointer( PANE_PAGE, MP_ARROW );
        caret_off();
        scr_cursor( 0, 0, CUR_HIDE );
        /* A page that has not been drawn yet is drawn AFTER this turn's
           screen is out (view_after_draw): drawing it takes seconds, and
           this is inside the screen's own drawing, where nothing may
           put up a box or wait. */
        most = pdf_zoom_now( pdf );
        if ( pdf->pix_page != pdf->page || pdf->pix_zoom != most || (pdf->pix == NULL && !pdf->pix_failed) ) {
            pdf_wanted = 1;
            app_message( "Drawing the page..." );
        } else if ( pdf->pix == NULL ) {
            app_message( "There is not enough memory to draw this page." );
        }
        if ( pdf->pix != NULL ) {
            most = pdf->pix_h + 16 - page_h;
            if ( pdf->scroll_y > most ) {
                pdf->scroll_y = most;
            }
            if ( pdf->scroll_y < 0 ) {
                pdf->scroll_y = 0;
            }
            most = pdf->pix_w - page_w;
            if ( pdf->scroll_x > most ) {
                pdf->scroll_x = most;
            }
            if ( pdf->scroll_x < 0 ) {
                pdf->scroll_x = 0;
            }
        }
        if ( seen.doc != cur_doc || seen.scroll_x != pdf->scroll_x || seen.scroll_y != pdf->scroll_y ||
             seen.zoom != pdf->pix_zoom || seen.serial != (unsigned int)pdf->pix_page || seen.w != page_w ||
             seen.h != page_h || seen.pix != pdf->pix ) {
            pane_damage_all( PANE_PAGE );
        }
        seen.doc = cur_doc;
        seen.pix = pdf->pix;
        seen.ed = NULL;
        seen.scroll_x = pdf->scroll_x;
        seen.scroll_y = pdf->scroll_y;
        seen.zoom = pdf->pix_zoom;
        seen.serial = (unsigned int)pdf->pix_page;
        seen.w = page_w;
        seen.h = page_h;
        sbar_draw( plan.doc_top, plan.doc_top + plan.doc_rows - 1, plan.doc_cols, (int)pdf->scroll_y / CELL_H,
                   plan.doc_rows, (pdf->pix_h + 16) / CELL_H );
        return;
    }
    /* a text screen: the page's words */
    if ( pdf->lines == NULL || pdf->lines_page != pdf->page ) {
        xfree( pdf->lines );
        pdf->lines = NULL;
        pdf->nlines = 0;
        rc = pdfview_text( pdf->page, &pdf->lines, &pdf->nlines );
        pdf->lines_page = pdf->page;
        pdf->top = 0;
        if ( rc != PV_OK ) {
            app_message( "This page's text could not be read." );
        }
    }
    if ( pdf->top > pdf->nlines - plan.doc_rows ) {
        pdf->top = pdf->nlines - plan.doc_rows;
    }
    if ( pdf->top < 0 ) {
        pdf->top = 0;
    }
    for ( row = 0; row < plan.doc_rows; row++ ) {
        scr_fill( plan.doc_top + row, 0, plan.doc_cols, ' ', pal.text );
        if ( pdf->top + row < pdf->nlines && pdf->lines != NULL ) {
            scr_putw( plan.doc_top + row, 1, pdf->lines[pdf->top + row], plan.doc_cols - 1, pal.text );
        }
    }
    scr_cursor( 0, 0, CUR_HIDE );
    sbar_draw( plan.doc_top, plan.doc_top + plan.doc_rows - 1, plan.doc_cols, pdf->top, plan.doc_rows,
               pdf->nlines );
}

void view_pdf_key( int key )
{
    PDFDOCVIEW *pdf = PDFV;
    int step = gfx_on ? 32 : 1, window = gfx_on ? page_h - 32 : plan.doc_rows - 1;

    switch ( key ) {
    case K_UP:
    case K_DOWN:
        if ( gfx_on ) {
            pdf->scroll_y += key == K_UP ? -step : step;
        } else {
            pdf->top += key == K_UP ? -1 : 1;
        }
        break;
    case K_LEFT:
        pdf->scroll_x -= step;
        break;
    case K_RIGHT:
        pdf->scroll_x += step;
        break;
    case K_PGDN:
    case ' ':
        /* down the page, and from its foot to the top of the next */
        if ( gfx_on ? pdf->scroll_y + page_h < pdf->pix_h + 16 : pdf->top + plan.doc_rows < pdf->nlines ) {
            if ( gfx_on ) {
                pdf->scroll_y += window;
            } else {
                pdf->top += window;
            }
        } else if ( pdf->page + 1 < pdf->pages ) {
            pdf->page++;
            pdf->scroll_y = 0;
            pdf->top = 0;
        }
        break;
    case K_PGUP:
        if ( gfx_on ? pdf->scroll_y > 0 : pdf->top > 0 ) {
            if ( gfx_on ) {
                pdf->scroll_y -= window;
            } else {
                pdf->top -= window;
            }
        } else if ( pdf->page > 0 ) {
            pdf->page--;
            pdf->scroll_y = 0x3FFFFFFFL;
            pdf->top = 0x3FFFFFFF;
        }
        break;
    case K_HOME | K_CTRL:
        pdf->page = 0;
        pdf->scroll_y = 0;
        pdf->top = 0;
        break;
    case K_END | K_CTRL:
        pdf->page = pdf->pages - 1;
        pdf->scroll_y = 0;
        pdf->top = 0;
        break;
    case '+':
    case '=':
        pdf->zoom = pdf_zoom_now( pdf ) * 5 / 4;
        break;
    case '-':
        pdf->zoom = pdf_zoom_now( pdf ) * 4 / 5;
        break;
    }
    if ( pdf->page < 0 ) {
        pdf->page = 0;
    }
    if ( pdf->scroll_y < 0 ) {
        pdf->scroll_y = 0;
    }
}

/* ------------------------------------------------------------------ */
/* the document's part of the screen                                   */
/* ------------------------------------------------------------------ */

void view_draw( void )
{
    EDITOR *ed;
    int x, y, height, unit;
    int cx, cy;
    static int defined;

    if ( !defined ) {
        pane_define( PANE_PAGE, pane_paint );
        defined = 1;
    }
    if ( cur_doc < 0 || cur_doc >= ndocs ) {
        return;
    }
    if ( PDFV != NULL ) {
        pdf_draw();
        return;
    }
    ed = ED;
    ed_set_device( ed, !gfx_on );
    if ( !gfx_on ) {
        ed_set_view( ed, (int)(plan.doc_cols - SEL_BAR) * GRID_W, (int)plan.doc_rows * GRID_H );
        lay_show_hidden( ed->lay, set.show_marks );
        ed_update( ed );
        text_draw( ed );
        unit = GRID_H;
    } else {
        scale = (int32_t)((uint32_t)(set.zoom < 25 ? 25 : set.zoom > 400 ? 400 : set.zoom) * 65536UL / 1500UL);
        pane_place( PANE_PAGE, plan.doc_top, 0, plan.doc_rows, plan.doc_cols );
        pane_rect( PANE_PAGE, &page_x, &page_y, &page_w, &page_h );
        pane_pointer( PANE_PAGE, MP_IBEAM );
        ed_set_view( ed, px_twips( page_w - PAD_X - 4 ), px_twips( page_h ) );
        lay_show_hidden( ed->lay, set.show_marks );
        ed_update( ed );
        page_damage( ed );
        scr_cursor( 0, 0, CUR_HIDE );
        lay_caret( ed->lay, ed->caret, &x, &y, &height );
        cx = page_x + PAD_X + view_px( x - ed->scroll_x );
        cy = page_y + view_px( y - ed->scroll_y );
        if ( !ed_has_sel( ed ) && cy >= page_y && cy + view_px( height ) <= page_y + page_h && cx >= page_x &&
             cx < page_x + page_w ) {
            caret_at( cx, cy, view_px( height ) );
        } else {
            caret_off();
        }
        unit = 240;
    }
    sbar_draw( plan.doc_top, plan.doc_top + plan.doc_rows - 1, plan.doc_cols, (int)(ed->scroll_y / unit),
               (int)(ed->view_h / unit), (int)(lay_height( ed->lay ) / unit) + 1 );
}

/*
 * What a turn's drawing found to do that could not be done while the
 * screen was being drawn: say that a command ran out of memory, draw a
 * PDF's page.  MAIN.C calls it when the screen is out; 1 means the
 * screen is to be drawn again before a key is waited for.
 */
int view_after_draw( void )
{
    if ( cur_doc < 0 || cur_doc >= ndocs ) {
        pdf_wanted = 0;
        return 0;
    }
    if ( ED != NULL && ED->no_memory ) {
        ED->no_memory = 0;
        no_memory();
        return 1;
    }
    if ( pdf_wanted ) {
        pdf_wanted = 0;
        app_message( NULL );
        if ( PDFV != NULL && pdf_ready() ) {
            pdf_render( PDFV );
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

/* the mouse's place as a point of the story */
static void mouse_point( EDITOR *ed, int *x, int *y )
{
    if ( gfx_on ) {
        *x = ed->scroll_x + px_twips( mouse.x - page_x - PAD_X );
        *y = ed->scroll_y + px_twips( mouse.y - page_y );
    } else {
        *x = ed->scroll_x + (int)(mouse.col - SEL_BAR) * GRID_W + GRID_W / 2;
        *y = ed->scroll_y + (int)(mouse.row - plan.doc_top) * GRID_H + GRID_H / 2;
    }
    if ( *x < 0 ) {
        *x = 0;
    }
    if ( *y < 0 ) {
        *y = 0;
    }
}

static void redraw( void )
{
    ui_redraw();
    scr_flush();
}

/* the scroll bar: its arrows and track repeat while the button is held,
   and its box is dragged */
static void bar_mouse( void )
{
    EDITOR *ed = ED;
    PDFDOCVIEW *pdf = PDFV;
    int top = plan.doc_top, bottom = plan.doc_top + plan.doc_rows - 1, part, first, shown, total, key, wait = 40;
    int unit = gfx_on ? 240 : GRID_H;

    for ( ;; ) {
        if ( ed != NULL ) {
            first = (int)(ed->scroll_y / unit);
            shown = (int)(ed->view_h / unit);
            total = (int)(lay_height( ed->lay ) / unit) + 1;
        } else {
            first = gfx_on ? (int)pdf->scroll_y / CELL_H : pdf->top;
            shown = plan.doc_rows;
            total = gfx_on ? (pdf->pix_h + 16) / CELL_H : pdf->nlines;
        }
        part = sbar_hit( top, bottom, first, shown, total, mouse.row );
        if ( part == SB_THUMB || mouse.kind == ME_DRAG ) {
            first = sbar_drag( top, bottom, shown, total, mouse.row );
        } else if ( part == SB_UP ) {
            first--;
        } else if ( part == SB_DOWN ) {
            first++;
        } else if ( part == SB_PGUP ) {
            first -= shown - 1;
        } else if ( part == SB_PGDN ) {
            first += shown - 1;
        }
        if ( first < 0 ) {
            first = 0;
        }
        if ( ed != NULL ) {
            ed_scroll_to( ed, (int)first * unit );
        } else if ( gfx_on ) {
            pdf->scroll_y = (int)first * CELL_H;
        } else {
            pdf->top = first;
        }
        redraw();
        key = key_get_timed( part == SB_THUMB ? 2 : wait );
        wait = 6;
        if ( key_service( key ) ) {
            continue;
        }
        if ( key == K_MOUSE && mouse.kind == ME_UP ) {
            return;
        }
        if ( key != K_TICK && key != K_MOUSE ) {
            return;
        }
        if ( key_testing() && key == K_TICK ) {
            return;
        }
    }
}

void view_wheel( int notches )
{
    if ( ED != NULL ) {
        ed_scroll_by( ED, notches * 3 );
    } else if ( PDFV != NULL ) {
        if ( gfx_on ) {
            PDFV->scroll_y += notches * 48;
        } else {
            PDFV->top += notches * 3;
        }
    }
}

void view_mouse( void )
{
    static const MENUITEM quick[] = {
        { "Cu&t", "", CMD_CUT, "" }, { "&Copy", "", CMD_COPY, "" }, { "&Paste", "", CMD_PASTE, "" },
        { NULL, NULL, 0, NULL },
        { "&Font...", "", CMD_FONT, "" }, { "P&aragraph...", "", CMD_PARAGRAPH, "" }
    };
    EDITOR *ed = ED;
    int x, y;
    int key, cmd;

    if ( mouse.kind == ME_WHEEL ) {
        view_wheel( mouse.wheel );
        return;
    }
    if ( mouse.col >= plan.doc_cols ) {
        if ( mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE ) {
            bar_mouse();
        }
        return;
    }
    if ( ed == NULL ) {
        return;
    }
    if ( mouse.kind == ME_DOWN && mouse.button == MBTN_RIGHT ) {
        cmd = menu_popup( quick, 6, mouse.row, mouse.col );
        if ( cmd ) {
            cmd_do( cmd );
        }
        return;
    }
    if ( mouse.kind == ME_DOUBLE ) {
        mouse_point( ed, &x, &y );
        ed_click( ed, x, y, 0 );
        ed_select_word( ed );
        return;
    }
    if ( mouse.kind != ME_DOWN ) {
        return;
    }
    mouse_point( ed, &x, &y );
    ed_click( ed, x, y, (mouse.shift & SH_SHIFT) != 0 );
    /* held and moved: the selection follows, and the text scrolls when
       the pointer is past the window's top or bottom */
    for ( ;; ) {
        redraw();
        key = key_get_timed( 8 );
        if ( key_service( key ) ) {
            continue;
        }
        if ( key == K_MOUSE && mouse.kind == ME_UP ) {
            return;
        }
        if ( key != K_MOUSE && key != K_TICK ) {
            return;
        }
        if ( key == K_TICK ) {
            if ( key_testing() || !mouse_buttons() ) {
                return;
            }
            mouse_where( &mouse.x, &mouse.y );
            mouse.row = mouse.y / CELL_H;
            mouse.col = mouse.x / CELL_W;
            if ( mouse.row >= plan.doc_top && mouse.row < plan.doc_top + plan.doc_rows ) {
                continue;
            }
            ed_scroll_by( ed, mouse.row < plan.doc_top ? -1 : 1 );
        }
        mouse_point( ed, &x, &y );
        ed_click( ed, x, y, 1 );
    }
}
