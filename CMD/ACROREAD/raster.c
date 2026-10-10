/*
 * RASTER.C - shapes into pixels.
 *
 * A shape arrives as straight edges (curves are already flattened, in
 * PATH.C) and is scanned from the top.  Each pixel row is sampled at
 * four heights, and along each sample the shape's extent is taken
 * exactly, to a 64th of a pixel - so a pixel's coverage is one of 256
 * levels, a vertical stem's edge is as good as it can be, and a
 * horizontal one has four steps to fade through.  That is what makes
 * ten-point text readable on a 640-pixel screen.
 *
 * The coverage of a row is kept as differences: a span adds at its
 * start and takes away at its end, and one pass along the row sums
 * them.  A span therefore costs the same however long it is.
 *
 * What is done with the coverage is the caller's: ras_fill paints it
 * into a canvas through a PAINT and a CLIP; a glyph keeps it as a
 * bitmap (GLYPH.C); a clipping path keeps it as a MASK, run-length
 * coded, which later shapes are multiplied by.
 */
#include "gfx.h"

#define SUB_ROWS    4               /* samples down one pixel */
#define SUB_FULL    64              /* a sample of a whole pixel */
#define DEV_LIMIT   0x3E800000L     /* 16000 pixels: nothing is drawn beyond */

typedef struct {
    s32 top, bot;                   /* sample rows: the first, and one past the last */
    fx  xpos;                       /* where it crosses the current sample row */
    fx  step;                       /* ...and how far that moves a row down */
    s32 dir;                        /* +1 going down, -1 going up */
} EDGE;

static EDGE *edges;
static u32   edge_count, edge_cap;
static s32   box_top, box_bot;      /* sample rows */
static fx    box_left, box_right;

static u32  *active;
static u32   active_cap;
static s32  *diffs;                 /* a row's coverage, as differences */
static u8   *cover;                 /* ...and summed */
static u32   row_cap;
static u8   *work_cov;              /* paint_row's own copies */
static u8   *work_bgr;
static u32   work_cap;

static void *grow( void *old, u32 old_bytes, u32 new_bytes )
{
    void *mem = xalloc( new_bytes );

    if ( old ) {
        mem_cpy( mem, old, old_bytes );
        xfree( old );
    }
    return mem;
}

/* the caches RASTER.C keeps are scratch: all of it can go */
int raster_reclaim( void )
{
    int freed = 0;

    if ( edge_count == 0 && edge_cap > 4096 ) {
        xfree( edges );
        edges = NULL;
        edge_cap = 0;
        freed = 1;
    }
    return freed;
}

/* ------------------------------------------------------------------ */
/* edges                                                               */
/* ------------------------------------------------------------------ */

void ras_begin( void )
{
    edge_count = 0;
    box_top = 0x7FFFFFFFL;
    box_bot = -0x7FFFFFFFL;
    box_left = FX_MAX;
    box_right = -FX_MAX;
}

int ras_empty( void )
{
    return edge_count == 0;
}

#define LIMIT( val )    ((val) > DEV_LIMIT ? DEV_LIMIT : ((val) < -DEV_LIMIT ? -DEV_LIMIT : (val)))

void ras_line( fx x0, fx y0, fx x1, fx y1 )
{
    EDGE *edge;
    fx swap;
    s32 top, bot, dir = 1;

    x0 = LIMIT( x0 );
    y0 = LIMIT( y0 );
    x1 = LIMIT( x1 );
    y1 = LIMIT( y1 );
    if ( y0 > y1 ) {
        swap = y0;
        y0 = y1;
        y1 = swap;
        swap = x0;
        x0 = x1;
        x1 = swap;
        dir = -1;
    }
    /* sample row k is at height (k + 1/2) / 4: the rows this edge crosses */
    top = (y0 + 8191) >> 14;
    bot = (y1 + 8191) >> 14;
    if ( top >= bot ) {
        return;
    }
    if ( edge_count == edge_cap ) {
        edges = (EDGE *)grow( edges, edge_count * sizeof( EDGE ),
                              (edge_cap ? edge_cap * 2 : 1024) * sizeof( EDGE ) );
        edge_cap = edge_cap ? edge_cap * 2 : 1024;
    }
    edge = &edges[edge_count++];
    edge->top = top;
    edge->bot = bot;
    edge->dir = dir;
    edge->step = mul_div( x1 - x0, 16384, y1 - y0 );
    edge->xpos = x0 + mul_div( (top << 14) + 8192 - y0, x1 - x0, y1 - y0 );
    if ( top < box_top ) {
        box_top = top;
    }
    if ( bot > box_bot ) {
        box_bot = bot;
    }
    if ( x0 < box_left ) {
        box_left = x0;
    }
    if ( x1 < box_left ) {
        box_left = x1;
    }
    if ( x0 > box_right ) {
        box_right = x0;
    }
    if ( x1 > box_right ) {
        box_right = x1;
    }
}

static void sort_edges( void )
{
    EDGE hold;
    u32 gap, at, back;

    for ( gap = 1; gap < edge_count / 3; gap = gap * 3 + 1 ) {
    }
    for ( ; gap; gap /= 3 ) {
        for ( at = gap; at < edge_count; at++ ) {
            if ( edges[at].top >= edges[at - gap].top ) {
                continue;
            }
            hold = edges[at];
            for ( back = at; back >= gap && edges[back - gap].top > hold.top; back -= gap ) {
                edges[back] = edges[back - gap];
            }
            edges[back] = hold;
        }
    }
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */
/* ------------------------------------------------------------------ */

void ras_scan( int cx0, int cy0, int cx1, int cy1, int evenodd, ROWFN fn, void *ctx )
{
    EDGE *edge;
    u32 next = 0, live = 0, at, keep, width, hold;
    s32 row, sample, wind, run;
    fx left, right, from = 0, to, base, limit;
    int ypos, lo, hi, index, cell;

    if ( edge_count == 0 ) {
        return;
    }
    if ( cy0 < (box_top >> 2) ) {
        cy0 = (int)(box_top >> 2);
    }
    if ( cy1 > ((box_bot + 3) >> 2) ) {
        cy1 = (int)((box_bot + 3) >> 2);
    }
    if ( cx0 < FX_FLOOR( box_left ) ) {
        cx0 = (int)FX_FLOOR( box_left );
    }
    if ( cx1 > FX_FLOOR( box_right ) + 1 ) {
        cx1 = (int)FX_FLOOR( box_right ) + 1;
    }
    if ( cx0 >= cx1 || cy0 >= cy1 ) {
        return;
    }
    width = (u32)(cx1 - cx0);
    if ( width + 2 > row_cap ) {
        if ( diffs ) {
            xfree( diffs );
            xfree( cover );
        }
        row_cap = width + 2 + 256;
        diffs = (s32 *)xalloc( row_cap * sizeof( s32 ) );
        cover = (u8 *)xalloc( row_cap );
    }
    if ( edge_count > active_cap ) {
        if ( active ) {
            xfree( active );
        }
        active_cap = edge_cap;
        active = (u32 *)xalloc( active_cap * sizeof( u32 ) );
    }
    sort_edges();
    base = I2FX( cx0 );
    limit = I2FX( cx1 ) - base;

    for ( ypos = cy0; ypos < cy1; ypos++ ) {
        lo = (int)width;
        hi = -1;
        for ( sample = 0; sample < SUB_ROWS; sample++ ) {
            row = (s32)ypos * SUB_ROWS + sample;
            /* the edges that start here, or started above the clip */
            while ( next < edge_count && edges[next].top <= row ) {
                edge = &edges[next];
                if ( edge->bot > row ) {
                    edge->xpos += edge->step * (row - edge->top);
                    active[live++] = next;
                }
                next++;
            }
            /* the ones that have ended go; the rest are put in order */
            keep = 0;
            for ( at = 0; at < live; at++ ) {
                if ( edges[active[at]].bot > row ) {
                    hold = active[at];
                    for ( index = (int)keep; index > 0 &&
                          edges[active[index - 1]].xpos > edges[hold].xpos; index-- ) {
                        active[index] = active[index - 1];
                    }
                    active[index] = hold;
                    keep++;
                }
            }
            live = keep;
            if ( live == 0 ) {
                if ( next >= edge_count ) {
                    break;
                }
                continue;
            }
            if ( hi < 0 ) {
                mem_set( diffs, 0, (width + 2) * sizeof( s32 ) );
            }
            wind = 0;
            for ( at = 0; at < live; at++ ) {
                edge = &edges[active[at]];
                if ( evenodd ) {
                    wind ^= 1;
                    if ( wind ) {
                        from = edge->xpos;
                        edge->xpos += edge->step;
                        continue;
                    }
                } else {
                    if ( wind == 0 ) {
                        from = edge->xpos;
                    }
                    wind += edge->dir;
                    if ( wind != 0 ) {
                        edge->xpos += edge->step;
                        continue;
                    }
                }
                to = edge->xpos;
                edge->xpos += edge->step;
                /* a span, from "from" to "to": into the differences */
                left = from - base;
                right = to - base;
                if ( left < 0 ) {
                    left = 0;
                }
                if ( right > limit ) {
                    right = limit;
                }
                if ( left >= right ) {
                    continue;
                }
                cell = (int)(left >> 16);
                run = (left & 0xFFFF) >> 10;
                diffs[cell] += SUB_FULL - run;
                diffs[cell + 1] += run;
                if ( cell < lo ) {
                    lo = cell;
                }
                cell = (int)(right >> 16);
                run = (right & 0xFFFF) >> 10;
                diffs[cell] -= SUB_FULL - run;
                diffs[cell + 1] -= run;
                if ( cell > hi ) {
                    hi = cell;
                }
            }
            if ( evenodd && (wind & 1) ) {
                /* an edge with no partner: nothing to do but end the row */
                continue;
            }
        }
        if ( hi < 0 ) {
            if ( live == 0 && next >= edge_count ) {
                break;
            }
            continue;
        }
        if ( hi >= (int)width ) {
            hi = (int)width - 1;
        }
        run = 0;
        for ( index = lo; index <= hi; index++ ) {
            run += diffs[index];
            cover[index] = (u8)(run > 255 ? 255 : (run < 0 ? 0 : run));
        }
        fn( ctx, ypos, cx0 + lo, cx0 + hi + 1, cover + lo );
    }
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

u32 rgb_grey( u32 rgb )
{
    return (((rgb >> 16) & 0xFF) * 77 + ((rgb >> 8) & 0xFF) * 150 + (rgb & 0xFF) * 29) >> 8;
}

void canvas_clear( CANVAS *cv, u32 rgb )
{
    u8 *row = cv->pix;
    u32 count = (u32)cv->width * (u32)cv->height;

    if ( cv->bpp == 1 ) {
        mem_set( row, (int)rgb_grey( rgb ), count );
    } else if ( ((rgb >> 16) & 0xFF) == (rgb & 0xFF) && ((rgb >> 8) & 0xFF) == (rgb & 0xFF) ) {
        mem_set( row, (int)(rgb & 0xFF), count * 3 );
    } else {
        while ( count-- ) {
            row[0] = (u8)rgb;
            row[1] = (u8)(rgb >> 8);
            row[2] = (u8)(rgb >> 16);
            row += 3;
        }
    }
}

static void work_need( u32 count )
{
    if ( count > work_cap ) {
        if ( work_cov ) {
            xfree( work_cov );
            xfree( work_bgr );
        }
        work_cap = count + 256;
        work_cov = (u8 *)xalloc( work_cap );
        work_bgr = (u8 *)xalloc( work_cap * 3 );
    }
}

static int blend_chan( int mode, int dst, int src )
{
    switch ( mode ) {
    case BM_MULTIPLY:
        return (dst * src + 127) / 255;
    case BM_DARKEN:
        return dst < src ? dst : src;
    case BM_SCREEN:
        return dst + src - (dst * src + 127) / 255;
    case BM_LIGHTEN:
        return dst > src ? dst : src;
    }
    return src;
}

/* "count" pixels of colour at (x0, ypos): bgr is 3 bytes each; alpha a
   byte each, or NULL for all opaque.  For images and shadings. */
void paint_pixels( CANVAS *cv, const CLIP *clip, int ypos, int x0, int count, const u8 *bgr,
                   const u8 *alpha, int blend )
{
    u8 *dst;
    int skip, index, level, red, green, blue, grey;

    if ( ypos < clip->y0 || ypos >= clip->y1 ) {
        return;
    }
    if ( x0 < clip->x0 ) {
        skip = clip->x0 - x0;
        bgr += skip * 3;
        if ( alpha ) {
            alpha += skip;
        }
        count -= skip;
        x0 = clip->x0;
    }
    if ( x0 + count > clip->x1 ) {
        count = clip->x1 - x0;
    }
    if ( count <= 0 ) {
        return;
    }
    if ( clip->mask ) {
        work_need( (u32)count );
        if ( alpha ) {
            mem_cpy( work_cov, alpha, (u32)count );
        } else {
            mem_set( work_cov, 255, (u32)count );
        }
        mask_row( clip->mask, ypos, x0, x0 + count, work_cov );
        alpha = work_cov;
    }
    dst = cv->pix + (u32)ypos * cv->stride + (u32)x0 * (u32)cv->bpp;
    for ( index = 0; index < count; index++, bgr += 3, dst += cv->bpp ) {
        level = alpha ? alpha[index] : 255;
        if ( level == 0 ) {
            continue;
        }
        blue = bgr[0];
        green = bgr[1];
        red = bgr[2];
        if ( cv->bpp == 1 ) {
            grey = (red * 77 + green * 150 + blue * 29) >> 8;
            if ( blend ) {
                grey = blend_chan( blend, dst[0], grey );
            }
            dst[0] = (u8)(level == 255 ? grey : dst[0] + (((grey - dst[0]) * level + 128) >> 8));
        } else {
            if ( blend ) {
                blue = blend_chan( blend, dst[0], blue );
                green = blend_chan( blend, dst[1], green );
                red = blend_chan( blend, dst[2], red );
            }
            if ( level == 255 ) {
                dst[0] = (u8)blue;
                dst[1] = (u8)green;
                dst[2] = (u8)red;
            } else {
                dst[0] = (u8)(dst[0] + (((blue - dst[0]) * level + 128) >> 8));
                dst[1] = (u8)(dst[1] + (((green - dst[1]) * level + 128) >> 8));
                dst[2] = (u8)(dst[2] + (((red - dst[2]) * level + 128) >> 8));
            }
        }
    }
}

/* A row of a shape: "cov" is its coverage from x0 to x1, or NULL for
   all of it.  Clipped here, to the rectangle and to the mask. */
void paint_row( CANVAS *cv, const CLIP *clip, const PAINT *paint, int ypos, int x0, int x1,
                const u8 *cov )
{
    CLIP bare;
    u8 *dst;
    int count, index, level, alpha = paint->alpha, red, green, blue, grey;

    if ( paint->none || alpha == 0 || ypos < clip->y0 || ypos >= clip->y1 ) {
        return;
    }
    if ( x0 < clip->x0 ) {
        if ( cov ) {
            cov += clip->x0 - x0;
        }
        x0 = clip->x0;
    }
    if ( x1 > clip->x1 ) {
        x1 = clip->x1;
    }
    count = x1 - x0;
    if ( count <= 0 ) {
        return;
    }
    if ( clip->mask || paint->row ) {
        work_need( (u32)count );
    }
    if ( clip->mask ) {
        if ( cov ) {
            mem_cpy( work_cov, cov, (u32)count );
        } else {
            mem_set( work_cov, 255, (u32)count );
        }
        mask_row( clip->mask, ypos, x0, x1, work_cov );
        cov = work_cov;
    }
    if ( paint->row ) {
        /* a colour for each pixel */
        paint->row( paint, ypos, x0, count, work_bgr );
        if ( alpha != 255 ) {
            if ( cov != work_cov ) {
                if ( cov ) {
                    mem_cpy( work_cov, cov, (u32)count );
                } else {
                    mem_set( work_cov, 255, (u32)count );
                }
                cov = work_cov;
            }
            for ( index = 0; index < count; index++ ) {
                work_cov[index] = (u8)((work_cov[index] * alpha + 127) / 255);
            }
        }
        /* the mask has been applied: paint_pixels gets a clip without one */
        bare = *clip;
        bare.mask = NULL;
        paint_pixels( cv, &bare, ypos, x0, count, work_bgr, cov, paint->blend );
        return;
    }

    red = (int)((paint->rgb >> 16) & 0xFF);
    green = (int)((paint->rgb >> 8) & 0xFF);
    blue = (int)(paint->rgb & 0xFF);
    dst = cv->pix + (u32)ypos * cv->stride + (u32)x0 * (u32)cv->bpp;
    if ( cv->bpp == 1 ) {
        grey = (red * 77 + green * 150 + blue * 29) >> 8;
        if ( cov == NULL && alpha == 255 && paint->blend == BM_NORMAL ) {
            mem_set( dst, grey, (u32)count );
            return;
        }
        for ( index = 0; index < count; index++, dst++ ) {
            level = cov ? cov[index] : 255;
            if ( level == 0 ) {
                continue;
            }
            if ( alpha != 255 ) {
                level = (level * alpha + 127) / 255;
            }
            if ( paint->blend ) {
                red = blend_chan( paint->blend, dst[0], grey );
                dst[0] = (u8)(dst[0] + (((red - dst[0]) * level + 128) >> 8));
            } else if ( level == 255 ) {
                dst[0] = (u8)grey;
            } else {
                dst[0] = (u8)(dst[0] + (((grey - dst[0]) * level + 128) >> 8));
            }
        }
        return;
    }
    for ( index = 0; index < count; index++, dst += 3 ) {
        level = cov ? cov[index] : 255;
        if ( level == 0 ) {
            continue;
        }
        if ( alpha != 255 ) {
            level = (level * alpha + 127) / 255;
        }
        if ( paint->blend ) {
            dst[0] = (u8)(dst[0] + (((blend_chan( paint->blend, dst[0], blue ) - dst[0]) * level + 128) >> 8));
            dst[1] = (u8)(dst[1] + (((blend_chan( paint->blend, dst[1], green ) - dst[1]) * level + 128) >> 8));
            dst[2] = (u8)(dst[2] + (((blend_chan( paint->blend, dst[2], red ) - dst[2]) * level + 128) >> 8));
        } else if ( level == 255 ) {
            dst[0] = (u8)blue;
            dst[1] = (u8)green;
            dst[2] = (u8)red;
        } else {
            dst[0] = (u8)(dst[0] + (((blue - dst[0]) * level + 128) >> 8));
            dst[1] = (u8)(dst[1] + (((green - dst[1]) * level + 128) >> 8));
            dst[2] = (u8)(dst[2] + (((red - dst[2]) * level + 128) >> 8));
        }
    }
}

void paint_rect( CANVAS *cv, const CLIP *clip, const PAINT *paint, int x0, int y0, int x1, int y1 )
{
    int ypos;

    if ( y0 < clip->y0 ) {
        y0 = clip->y0;
    }
    if ( y1 > clip->y1 ) {
        y1 = clip->y1;
    }
    for ( ypos = y0; ypos < y1; ypos++ ) {
        paint_row( cv, clip, paint, ypos, x0, x1, NULL );
    }
}

typedef struct {
    CANVAS      *cv;
    const CLIP  *clip;
    const PAINT *paint;
} FILLCTX;

static void fill_row( void *ctx, int ypos, int x0, int x1, const u8 *cov )
{
    FILLCTX *fill = (FILLCTX *)ctx;

    paint_row( fill->cv, fill->clip, fill->paint, ypos, x0, x1, cov );
}

void ras_fill( CANVAS *cv, const CLIP *clip, int evenodd, const PAINT *paint )
{
    FILLCTX fill;

    fill.cv = cv;
    fill.clip = clip;
    fill.paint = paint;
    if ( !paint->none && paint->alpha ) {
        ras_scan( clip->x0, clip->y0, clip->x1, clip->y1, evenodd, fill_row, &fill );
    }
}

/* ------------------------------------------------------------------ */
/* masks                                                               */
/* ------------------------------------------------------------------ */

/*
 * A MASK is the coverage of a clipping path over its bounding box, a
 * row at a time, as runs: a count (1 to 255) and a level.  A row's
 * runs stop where the rest is zero.  A rectangle is three runs a row;
 * a circle a dozen.
 */
struct MASK {
    int  x0, y0, x1, y1;
    u32 *rows;                      /* where each row's runs start; one extra */
    u8  *runs;
    u32  used, cap;
    int  next_row;                  /* the row to be written next */
    int  refs;
    const CLIP *under;              /* while it is being made */
};

static void mask_put( MASK *mask, int count, int level )
{
    u8 *grown;

    while ( count > 0 ) {
        if ( mask->used + 2 > mask->cap ) {
            grown = (u8 *)pool_big( doc.page_pool, mask->cap * 2 );
            mem_cpy( grown, mask->runs, mask->used );
            pool_unbig( mask->runs );
            mask->runs = grown;
            mask->cap *= 2;
        }
        mask->runs[mask->used++] = (u8)(count > 255 ? 255 : count);
        mask->runs[mask->used++] = (u8)level;
        count -= 255;
    }
}

static void mask_build_row( void *ctx, int ypos, int x0, int x1, const u8 *cov )
{
    MASK *mask = (MASK *)ctx;
    int index, start, count = x1 - x0;

    while ( mask->next_row <= ypos ) {
        mask->rows[mask->next_row++ - mask->y0] = mask->used;
    }
    if ( mask->under->mask ) {
        work_need( (u32)count );
        mem_cpy( work_cov, cov, (u32)count );
        mask_row( mask->under->mask, ypos, x0, x1, work_cov );
        cov = work_cov;
    }
    mask_put( mask, x0 - mask->x0, 0 );
    for ( start = 0; start < count; start = index ) {
        for ( index = start + 1; index < count && cov[index] == cov[start]; index++ ) {
        }
        if ( index == count && cov[start] == 0 ) {
            break;
        }
        mask_put( mask, index - start, cov[start] );
    }
}

MASK *mask_make( const CLIP *clip, int evenodd )
{
    MASK *mask;
    int x0 = clip->x0, y0 = clip->y0, x1 = clip->x1, y1 = clip->y1, height;

    if ( edge_count ) {
        if ( y0 < (box_top >> 2) ) {
            y0 = (int)(box_top >> 2);
        }
        if ( y1 > ((box_bot + 3) >> 2) ) {
            y1 = (int)((box_bot + 3) >> 2);
        }
        if ( x0 < FX_FLOOR( box_left ) ) {
            x0 = (int)FX_FLOOR( box_left );
        }
        if ( x1 > FX_FLOOR( box_right ) + 1 ) {
            x1 = (int)FX_FLOOR( box_right ) + 1;
        }
    }
    if ( edge_count == 0 || x0 >= x1 || y0 >= y1 ) {
        x1 = x0;                    /* nothing is inside it */
        y1 = y0;
    }
    height = y1 - y0;
    mask = (MASK *)pool_big( doc.page_pool, sizeof( MASK ) + ((u32)height + 2) * sizeof( u32 ) );
    mask->x0 = x0;
    mask->y0 = y0;
    mask->x1 = x1;
    mask->y1 = y1;
    mask->rows = (u32 *)(mask + 1);
    mask->cap = 1024;
    mask->runs = (u8 *)pool_big( doc.page_pool, mask->cap );
    mask->next_row = y0;
    mask->under = clip;
    if ( height > 0 ) {
        ras_scan( x0, y0, x1, y1, evenodd, mask_build_row, mask );
    }
    while ( mask->next_row <= y1 ) {
        mask->rows[mask->next_row++ - y0] = mask->used;
    }
    mask->under = NULL;
    mask->refs = 1;
    return mask;
}

MASK *mask_keep( MASK *mask )
{
    if ( mask ) {
        mask->refs++;
    }
    return mask;
}

void mask_drop( MASK *mask )
{
    if ( mask && --mask->refs == 0 ) {
        pool_unbig( mask->runs );
        pool_unbig( mask );
    }
}

void mask_box( const MASK *mask, int *x0, int *y0, int *x1, int *y1 )
{
    *x0 = mask->x0;
    *y0 = mask->y0;
    *x1 = mask->x1;
    *y1 = mask->y1;
}

void mask_row( const MASK *mask, int ypos, int x0, int x1, u8 *cov )
{
    const u8 *run, *end;
    int at, stop, from, upto, level;

    if ( ypos < mask->y0 || ypos >= mask->y1 || x1 <= mask->x0 || x0 >= mask->x1 ) {
        mem_set( cov, 0, (u32)(x1 - x0) );
        return;
    }
    run = mask->runs + mask->rows[ypos - mask->y0];
    end = mask->runs + mask->rows[ypos - mask->y0 + 1];
    at = mask->x0;
    if ( x0 < at ) {
        mem_set( cov, 0, (u32)(at - x0) );
    }
    for ( ; run < end && at < x1; run += 2 ) {
        level = run[1];
        stop = at + run[0];
        from = at < x0 ? x0 : at;
        upto = stop > x1 ? x1 : stop;
        if ( from < upto && level != 255 ) {
            if ( level == 0 ) {
                mem_set( cov + (from - x0), 0, (u32)(upto - from) );
            } else {
                for ( ; from < upto; from++ ) {
                    cov[from - x0] = (u8)((cov[from - x0] * level + 127) / 255);
                }
            }
        }
        at = stop;
    }
    if ( at < x1 ) {
        from = at < x0 ? x0 : at;
        mem_set( cov + (from - x0), 0, (u32)(x1 - from) );
    }
}
