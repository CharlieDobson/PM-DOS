/*
 * PATH.C - a path, in pixels: built, flattened, and turned into the
 * edges RASTER.C scans.
 *
 * A path is kept as points and nothing else.  Curves are flattened as
 * they arrive - into as many chords as it takes to stay within a
 * quarter of a pixel - so filling is handing over the points, and
 * stroking is building, for each line between two of them, the
 * rectangle a pen of the stroke's width would cover, and at each
 * corner what the join asks for.  The pieces overlap; they are all
 * wound the same way, so filled together by the non-zero rule they
 * are their union, which is the stroke.
 */
#include "gfx.h"

#define MIN_WIDTH   0xC000L         /* three quarters of a pixel: thinner is not seen */

static PT  *dash_pts;               /* one dash, being collected */
static u32  dash_count, dash_cap;

/* ------------------------------------------------------------------ */
/* building                                                            */
/* ------------------------------------------------------------------ */

void path_reset( PATH *path )
{
    path->count = 0;
    path->sub = 0;
    path->has_cur = 0;
}

static void path_add( PATH *path, fx px, fx py, int flags )
{
    PT *pts;
    u8 *marks;
    u32 cap;

    if ( path->count == path->cap ) {
        cap = path->cap ? path->cap * 2 : 256;
        pts = (PT *)xalloc( cap * sizeof( PT ) );
        marks = (u8 *)xalloc( cap );
        if ( path->pts ) {
            mem_cpy( pts, path->pts, path->count * sizeof( PT ) );
            mem_cpy( marks, path->flags, path->count );
            xfree( path->pts );
            xfree( path->flags );
        }
        path->pts = pts;
        path->flags = marks;
        path->cap = cap;
    }
    path->pts[path->count].x = px;
    path->pts[path->count].y = py;
    path->flags[path->count] = (u8)flags;
    path->count++;
}

void path_move( PATH *path, fx px, fx py )
{
    /* a move straight after a move replaces it */
    if ( path->count && path->sub == path->count - 1 && (path->flags[path->sub] & PF_START) &&
         !(path->flags[path->sub] & PF_CLOSED) ) {
        path->count--;
    }
    path->sub = path->count;
    path_add( path, px, py, PF_START );
    path->cur_x = px;
    path->cur_y = py;
    path->has_cur = 1;
}

/* a line or a curve after a closepath starts a new subpath where the
   closed one began */
static void path_resume( PATH *path )
{
    if ( path->count && (path->flags[path->sub] & PF_CLOSED) ) {
        path->sub = path->count;
        path_add( path, path->cur_x, path->cur_y, PF_START );
    }
}

void path_line( PATH *path, fx px, fx py )
{
    if ( !path->has_cur ) {
        path_move( path, px, py );
        return;
    }
    path_resume( path );
    path_add( path, px, py, 0 );
    path->cur_x = px;
    path->cur_y = py;
}

static fx fx_abs( fx val )
{
    return val < 0 ? -val : val;
}

void path_curve( PATH *path, fx x1, fx y1, fx x2, fx y2, fx x3, fx y3 )
{
    fx x0, y0, bend, other, tee, inv, w0, w1, w2, w3;
    int steps, index;

    if ( !path->has_cur ) {
        path_move( path, x1, y1 );
    }
    path_resume( path );
    x0 = path->cur_x;
    y0 = path->cur_y;
    /* how far the curve is from its chords, by its second differences */
    bend = fx_abs( x0 - 2 * x1 + x2 ) + fx_abs( y0 - 2 * y1 + y2 );
    other = fx_abs( x1 - 2 * x2 + x3 ) + fx_abs( y1 - 2 * y2 + y3 );
    if ( other > bend ) {
        bend = other;
    }
    steps = (int)isqrt( (u32)(bend >> 14) ) + 1;    /* about sqrt( 3 * bend ) over 2 */
    if ( steps > 96 ) {
        steps = 96;
    }
    for ( index = 1; index < steps; index++ ) {
        tee = (fx)((65536L * index) / steps);
        inv = FX_ONE - tee;
        w0 = fx_mul( fx_mul( inv, inv ), inv );
        w1 = 3 * fx_mul( fx_mul( inv, inv ), tee );
        w2 = 3 * fx_mul( fx_mul( inv, tee ), tee );
        w3 = fx_mul( fx_mul( tee, tee ), tee );
        path_add( path, fx_mul( w0, x0 ) + fx_mul( w1, x1 ) + fx_mul( w2, x2 ) + fx_mul( w3, x3 ),
                  fx_mul( w0, y0 ) + fx_mul( w1, y1 ) + fx_mul( w2, y2 ) + fx_mul( w3, y3 ), 0 );
    }
    path_add( path, x3, y3, 0 );
    path->cur_x = x3;
    path->cur_y = y3;
}

void path_close( PATH *path )
{
    if ( !path->has_cur || path->count == 0 ) {
        return;
    }
    path->flags[path->sub] |= PF_CLOSED;
    path->cur_x = path->pts[path->sub].x;
    path->cur_y = path->pts[path->sub].y;
}

void path_bounds( const PATH *path, fx *x0, fx *y0, fx *x1, fx *y1 )
{
    u32 index;

    *x0 = *y0 = FX_MAX;
    *x1 = *y1 = -FX_MAX;
    for ( index = 0; index < path->count; index++ ) {
        if ( path->pts[index].x < *x0 ) {
            *x0 = path->pts[index].x;
        }
        if ( path->pts[index].x > *x1 ) {
            *x1 = path->pts[index].x;
        }
        if ( path->pts[index].y < *y0 ) {
            *y0 = path->pts[index].y;
        }
        if ( path->pts[index].y > *y1 ) {
            *y1 = path->pts[index].y;
        }
    }
}

/* Is the path one rectangle with its sides along the axes?  Its
   bounds, to the nearest pixel, if so. */
int path_as_rect( const PATH *path, int *x0, int *y0, int *x1, int *y1 )
{
    const PT *pts = path->pts;
    fx bx0, by0, bx1, by1;
    u32 count = path->count;

    if ( count == 5 && pts[4].x == pts[0].x && pts[4].y == pts[0].y ) {
        count = 4;
    }
    if ( count != 4 || path->sub != 0 ) {
        return 0;
    }
#define NEAR( lhs, rhs )    (fx_abs( (lhs) - (rhs) ) < 0x800)
    if ( !((NEAR( pts[0].x, pts[1].x ) && NEAR( pts[1].y, pts[2].y ) &&
            NEAR( pts[2].x, pts[3].x ) && NEAR( pts[3].y, pts[0].y )) ||
           (NEAR( pts[0].y, pts[1].y ) && NEAR( pts[1].x, pts[2].x ) &&
            NEAR( pts[2].y, pts[3].y ) && NEAR( pts[3].x, pts[0].x ))) ) {
        return 0;
    }
#undef NEAR
    path_bounds( path, &bx0, &by0, &bx1, &by1 );
    *x0 = (int)FX_ROUND( bx0 );
    *y0 = (int)FX_ROUND( by0 );
    *x1 = (int)FX_ROUND( bx1 );
    *y1 = (int)FX_ROUND( by1 );
    return 1;
}

/* ------------------------------------------------------------------ */
/* filling                                                             */
/* ------------------------------------------------------------------ */

void path_edges( const PATH *path )
{
    const PT *pts = path->pts;
    u32 index, start = 0;

    for ( index = 0; index < path->count; index++ ) {
        if ( path->flags[index] & PF_START ) {
            if ( index > start + 1 ) {
                ras_line( pts[index - 1].x, pts[index - 1].y, pts[start].x, pts[start].y );
            }
            start = index;
        } else {
            ras_line( pts[index - 1].x, pts[index - 1].y, pts[index].x, pts[index].y );
        }
    }
    if ( path->count > start + 1 ) {
        ras_line( pts[path->count - 1].x, pts[path->count - 1].y, pts[start].x, pts[start].y );
    }
}

/* ------------------------------------------------------------------ */
/* stroking                                                            */
/* ------------------------------------------------------------------ */

/* a polygon to the rasteriser, turned if need be so that every piece
   of a stroke winds the same way */
static void emit_poly( PT *pts, int count )
{
    s32 area = 0;
    int index, other;

    for ( index = 0; index < count; index++ ) {
        other = index + 1 == count ? 0 : index + 1;
        area += mul_shr( pts[index].x - pts[0].x, pts[other].y - pts[0].y, 28 ) -
                mul_shr( pts[other].x - pts[0].x, pts[index].y - pts[0].y, 28 );
    }
    if ( area > 0 ) {
        for ( index = count - 1; index >= 0; index-- ) {
            other = index == 0 ? count - 1 : index - 1;
            ras_line( pts[index].x, pts[index].y, pts[other].x, pts[other].y );
        }
    } else {
        for ( index = 0; index < count; index++ ) {
            other = index + 1 == count ? 0 : index + 1;
            ras_line( pts[index].x, pts[index].y, pts[other].x, pts[other].y );
        }
    }
}

static void emit_circle( fx cx, fx cy, fx radius )
{
    PT ring[36];
    int steps = radius < 2 * FX_ONE ? 8 : (radius < 6 * FX_ONE ? 12 : (radius < 20 * FX_ONE ? 24 : 36));
    int index;

    for ( index = 0; index < steps; index++ ) {
        ring[index].x = cx + fx_mul( radius, fx_cos( index * 360 / steps ) );
        ring[index].y = cy + fx_mul( radius, fx_sin( index * 360 / steps ) );
    }
    emit_poly( ring, steps );
}

/* the unit vector along (dx, dy), or 0 if it has no length */
static int unit( fx dx, fx dy, fx *ux, fx *uy )
{
    fx len = fx_hypot( dx, dy );

    if ( len < 16 ) {
        return 0;
    }
    *ux = fx_div( dx, len );
    *uy = fx_div( dy, len );
    return 1;
}

static void emit_join( const PT *at, fx ax, fx ay, fx bx, fx by, fx half, const STROKE *stroke )
{
    PT poly[4];
    fx cross = fx_mul( ax, by ) - fx_mul( ay, bx );     /* the sine of the turn */
    fx dot = fx_mul( ax, bx ) + fx_mul( ay, by );       /* and its cosine */
    fx side, scale, limit;
    int count = 3;

    if ( stroke->join == 1 ) {
        emit_circle( at->x, at->y, half );
        return;
    }
    if ( fx_abs( cross ) < 0x400 ) {
        return;                         /* straight on, or straight back */
    }
    side = cross > 0 ? -half : half;    /* the outside of the turn */
    poly[0] = *at;
    poly[1].x = at->x - fx_mul( ay, side );
    poly[1].y = at->y + fx_mul( ax, side );
    poly[2].x = at->x - fx_mul( by, side );
    poly[2].y = at->y + fx_mul( bx, side );
    if ( stroke->join == 0 ) {
        /* the miter's point is half / cos( turn / 2 ) out along the
           bisector, if the limit allows a point that long */
        scale = FX_ONE + dot;           /* 2 cos^2( turn / 2 ) */
        limit = stroke->miter > FX_ONE ? stroke->miter : FX_ONE;
        if ( scale > 0x200 && fx_div( 2 * FX_ONE, scale ) <= fx_mul( limit, limit ) ) {
            poly[3] = poly[2];
            poly[2].x = at->x + fx_div( fx_mul( -(ay + by), side ), scale );
            poly[2].y = at->y + fx_div( fx_mul( ax + bx, side ), scale );
            count = 4;
        }
    }
    emit_poly( poly, count );
}

/* one run of points, stroked: "closed" joins the last to the first */
static void stroke_run( const PT *pts, u32 count, int closed, const STROKE *stroke )
{
    PT quad[4];
    fx half = (stroke->width < MIN_WIDTH ? MIN_WIDTH : stroke->width) / 2;
    fx ux = 0, uy = 0, px = 0, py = 0, fx0 = 0, fy0 = 0, x0, y0, x1, y1;
    u32 index, total = closed ? count + 1 : count, segs = 0;
    const PT *from, *to;
    int joins = half > 0xA000L;

    if ( count == 0 ) {
        return;
    }
    for ( index = 1; index < total; index++ ) {
        from = &pts[index - 1];
        to = &pts[index == count ? 0 : index];
        if ( !unit( to->x - from->x, to->y - from->y, &ux, &uy ) ) {
            continue;
        }
        x0 = from->x;
        y0 = from->y;
        x1 = to->x;
        y1 = to->y;
        if ( !closed && stroke->cap == 2 ) {            /* a square cap: half a width more */
            if ( segs == 0 ) {
                x0 -= fx_mul( ux, half );
                y0 -= fx_mul( uy, half );
            }
            if ( index == count - 1 ) {
                x1 += fx_mul( ux, half );
                y1 += fx_mul( uy, half );
            }
        }
        quad[0].x = x0 - fx_mul( uy, half );
        quad[0].y = y0 + fx_mul( ux, half );
        quad[1].x = x1 - fx_mul( uy, half );
        quad[1].y = y1 + fx_mul( ux, half );
        quad[2].x = x1 + fx_mul( uy, half );
        quad[2].y = y1 - fx_mul( ux, half );
        quad[3].x = x0 + fx_mul( uy, half );
        quad[3].y = y0 - fx_mul( ux, half );
        emit_poly( quad, 4 );
        if ( segs == 0 ) {
            fx0 = ux;
            fy0 = uy;
        } else if ( joins ) {
            emit_join( from, px, py, ux, uy, half, stroke );
        }
        px = ux;
        py = uy;
        segs++;
    }
    if ( segs == 0 ) {
        /* a point: a dot if the caps are round or square, else nothing */
        if ( stroke->cap == 1 ) {
            emit_circle( pts[0].x, pts[0].y, half );
        } else if ( stroke->cap == 2 ) {
            quad[0].x = pts[0].x - half;
            quad[0].y = pts[0].y - half;
            quad[1].x = pts[0].x + half;
            quad[1].y = pts[0].y - half;
            quad[2].x = pts[0].x + half;
            quad[2].y = pts[0].y + half;
            quad[3].x = pts[0].x - half;
            quad[3].y = pts[0].y + half;
            emit_poly( quad, 4 );
        }
        return;
    }
    if ( closed ) {
        if ( joins ) {
            emit_join( &pts[0], px, py, fx0, fy0, half, stroke );
        }
    } else if ( stroke->cap == 1 && half > 0x8000L ) {
        emit_circle( pts[0].x, pts[0].y, half );
        emit_circle( pts[count - 1].x, pts[count - 1].y, half );
    }
}

static void dash_point( fx px, fx py )
{
    PT *grown;

    if ( dash_count == dash_cap ) {
        grown = (PT *)xalloc( (dash_cap ? dash_cap * 2 : 256) * sizeof( PT ) );
        if ( dash_pts ) {
            mem_cpy( grown, dash_pts, dash_count * sizeof( PT ) );
            xfree( dash_pts );
        }
        dash_pts = grown;
        dash_cap = dash_cap ? dash_cap * 2 : 256;
    }
    dash_pts[dash_count].x = px;
    dash_pts[dash_count].y = py;
    dash_count++;
}

static void stroke_dashed( const PT *pts, u32 count, int closed, const STROKE *stroke )
{
    fx remain, seglen, pos, take, ux, uy, skip = stroke->phase;
    u32 index, total = closed ? count + 1 : count;
    const PT *from, *to;
    int which = 0, on = 1, guard;

    /* into the pattern by the phase */
    remain = stroke->dash[0];
    for ( guard = 0; skip > 0 && guard < 10000; guard++ ) {
        if ( skip < remain ) {
            remain -= skip;
            break;
        }
        skip -= remain;
        which = (which + 1) % stroke->ndash;
        on = !on;
        remain = stroke->dash[which];
    }
    dash_count = 0;
    for ( index = 1; index < total; index++ ) {
        from = &pts[index - 1];
        to = &pts[index == count ? 0 : index];
        seglen = fx_hypot( to->x - from->x, to->y - from->y );
        if ( !unit( to->x - from->x, to->y - from->y, &ux, &uy ) ) {
            continue;
        }
        if ( on && dash_count == 0 ) {
            dash_point( from->x, from->y );
        }
        for ( pos = 0; pos < seglen; ) {
            take = remain < seglen - pos ? remain : seglen - pos;
            pos += take;
            remain -= take;
            if ( remain > 0 ) {
                break;                  /* the segment ended first */
            }
            if ( on ) {
                dash_point( from->x + fx_mul( ux, pos ), from->y + fx_mul( uy, pos ) );
                stroke_run( dash_pts, dash_count, 0, stroke );
                dash_count = 0;
            }
            which = (which + 1) % stroke->ndash;
            on = !on;
            remain = stroke->dash[which];
            if ( remain <= 0 ) {
                remain = 1;             /* a dash of nothing: a dot, and on we go */
            }
            if ( on ) {
                dash_point( from->x + fx_mul( ux, pos ), from->y + fx_mul( uy, pos ) );
            }
        }
        if ( on ) {
            dash_point( to->x, to->y );
        }
    }
    if ( on && dash_count ) {
        stroke_run( dash_pts, dash_count, 0, stroke );
    }
    dash_count = 0;
}

void path_stroke_edges( const PATH *path, const STROKE *stroke )
{
    u32 index, start = 0, count;
    fx total = 0;
    int dashed = 0, closed;

    for ( index = 0; index < (u32)stroke->ndash; index++ ) {
        total += stroke->dash[index];
    }
    dashed = stroke->ndash > 0 && total > 0x4000L;
    for ( index = 1; index <= path->count; index++ ) {
        if ( index < path->count && !(path->flags[index] & PF_START) ) {
            continue;
        }
        count = index - start;
        closed = (path->flags[start] & PF_CLOSED) != 0;
        /* a closed run that says its first point again at the end */
        if ( closed && count > 1 && path->pts[start].x == path->pts[index - 1].x &&
             path->pts[start].y == path->pts[index - 1].y ) {
            count--;
        }
        if ( count == 1 && !closed ) {
            start = index;              /* a move and nothing more */
            continue;
        }
        if ( dashed ) {
            stroke_dashed( path->pts + start, count, closed, stroke );
        } else {
            stroke_run( path->pts + start, count, closed && count > 1, stroke );
        }
        start = index;
    }
}
