/*
 * SHADE.C - shadings: a colour that changes from place to place.
 *
 * The two that documents use are drawn: the axial one, a blend along a
 * line (a button's gradient, a chart's bar), and the radial one, a
 * blend outward from a point.  Either is a function of one number, so
 * the function is asked once for each of 256 steps along the blend,
 * and a pixel's colour is then a matter of finding how far along it
 * is: for the axial one that is a straight-line function of where the
 * pixel is, worked out once a row and stepped across it; for the
 * radial one it is a square root a pixel.
 *
 * A radial shading whose circles do not share a centre is drawn as if
 * they did, about the outer one.  The mesh shadings (types 4 to 7) and
 * the one that is a function of both coordinates (type 1) are not
 * drawn at all.
 */
#include "gfx.h"

struct SHADE {
    int   type;
    u32   ramp[256];
    /* axial: how far along a pixel is, as A*x + B*y + C */
    REAL  along_x, along_y, along_c;
    /* radial, in pixels of the canvas */
    fx    centre_x, centre_y, radius0, radius1;
};

static void shade_row( const PAINT *paint, int ypos, int xpos, int count, u8 *bgr )
{
    const SHADE *shade = (const SHADE *)paint->ctx;
    REAL start;
    s32 tee, step, dx, dy, dist, span;
    u32 rgb;
    int index, level;

    if ( shade->type == 2 ) {
        /* 8.24, so that a blend a thousand pixels long still has steps */
        start = real_add( real_add( real_mul( shade->along_x, real_int( xpos ) ),
                                    real_mul( shade->along_y, real_int( ypos ) ) ), shade->along_c );
        tee = real_to_fx( real_mul( start, real_int( 256 ) ) );
        step = real_to_fx( real_mul( shade->along_x, real_int( 256 ) ) );
        for ( index = 0; index < count; index++, bgr += 3 ) {
            level = tee < 0 ? 0 : (tee >= 0x1000000L ? 255 : (int)(tee >> 16));
            rgb = shade->ramp[level];
            bgr[0] = (u8)rgb;
            bgr[1] = (u8)(rgb >> 8);
            bgr[2] = (u8)(rgb >> 16);
            if ( (step > 0 && tee > 0x7F000000L - step) || (step < 0 && tee < -0x7F000000L - step) ) {
                continue;                           /* far off the end: stay there */
            }
            tee += step;
        }
        return;
    }
    /* radial: the distance from the centre, in sixteenths of a pixel */
    span = (shade->radius1 - shade->radius0) >> 12;
    dy = ((I2FX( ypos ) + FX_HALF - shade->centre_y) >> 12);
    for ( index = 0; index < count; index++, bgr += 3 ) {
        dx = ((I2FX( xpos + index ) + FX_HALF - shade->centre_x) >> 12);
        if ( dx > 30000 || dx < -30000 || dy > 30000 || dy < -30000 ) {
            dist = 46000;
        } else {
            dist = (s32)isqrt( (u32)(dx * dx + dy * dy) );
        }
        dist -= shade->radius0 >> 12;
        level = span == 0 ? 255 : (int)((dist * 255) / span);
        rgb = shade->ramp[level < 0 ? 0 : (level > 255 ? 255 : level)];
        bgr[0] = (u8)rgb;
        bgr[1] = (u8)(rgb >> 8);
        bgr[2] = (u8)(rgb >> 16);
    }
}

/* "ctm" takes the shading's own space to the canvas's pixels */
SHADE *shade_load( OBJ *shading, const RMAT *ctm, DICT *res )
{
    DICT *dict = obj_dict( obj_resolve( shading ) );
    SHADE *shade;
    CSPACE *cs;
    FUNC *fn;
    ARR *coords, *domain;
    RMAT inv;
    REAL x0, y0, x1, y1, dx, dy, den;
    fx comps[MAX_COMPS], tee, lo = 0, hi = FX_ONE, px0, py0, px1, py1, scale;
    FMAT flat;
    int index, type;

    if ( dict == NULL ) {
        return NULL;
    }
    type = (int)dict_int( dict, "ShadingType", 0 );
    coords = dict_arr( dict, "Coords" );
    if ( (type != 2 && type != 3) || coords == NULL || coords->count < (u32)(type == 2 ? 4 : 6) ) {
        return NULL;
    }
    shade = (SHADE *)pool_alloc( doc.page_pool, sizeof( SHADE ) );
    shade->type = type;
    cs = cs_load( dict_get( dict, "ColorSpace" ), res );
    fn = func_load( dict_get( dict, "Function" ) );
    domain = dict_arr( dict, "Domain" );
    if ( domain && domain->count >= 2 ) {
        lo = obj_fx( arr_get( domain, 0 ) );
        hi = obj_fx( arr_get( domain, 1 ) );
    }
    for ( index = 0; index < 256; index++ ) {
        mem_set( comps, 0, sizeof( comps ) );
        if ( fn ) {
            tee = lo + mul_div( hi - lo, index, 255 );
            func_eval( fn, &tee, comps );
        }
        shade->ramp[index] = cs_rgb( cs, comps );
    }

    x0 = obj_real( arr_get( coords, 0 ) );
    y0 = obj_real( arr_get( coords, 1 ) );
    if ( type == 2 ) {
        if ( !rmat_invert( &inv, ctm ) ) {
            return NULL;
        }
        x1 = obj_real( arr_get( coords, 2 ) );
        y1 = obj_real( arr_get( coords, 3 ) );
        dx = real_add( x1, real_neg( x0 ) );
        dy = real_add( y1, real_neg( y0 ) );
        den = real_add( real_mul( dx, dx ), real_mul( dy, dy ) );
        if ( den.man == 0 ) {
            return NULL;
        }
        shade->along_x = real_div( real_add( real_mul( inv.m[0], dx ), real_mul( inv.m[1], dy ) ), den );
        shade->along_y = real_div( real_add( real_mul( inv.m[2], dx ), real_mul( inv.m[3], dy ) ), den );
        shade->along_c = real_div( real_add( real_mul( real_add( inv.m[4], real_neg( x0 ) ), dx ),
                                             real_mul( real_add( inv.m[5], real_neg( y0 ) ), dy ) ), den );
        /* a pixel is asked for at its corner and is its middle */
        shade->along_c = real_add( shade->along_c,
                                   real_mul( real_add( shade->along_x, shade->along_y ), real_fx( FX_HALF ) ) );
        return shade;
    }
    /* radial: the circles in pixels.  About the second one's centre. */
    rmat_to_fmat( &flat, ctm );
    scale = fx_sqrt( fx_mul( flat.a, flat.d ) - fx_mul( flat.b, flat.c ) < 0
                     ? fx_mul( flat.b, flat.c ) - fx_mul( flat.a, flat.d )
                     : fx_mul( flat.a, flat.d ) - fx_mul( flat.b, flat.c ) );
    rmat_point( ctm, x0, y0, &px0, &py0 );
    rmat_point( ctm, obj_real( arr_get( coords, 3 ) ), obj_real( arr_get( coords, 4 ) ), &px1, &py1 );
    shade->radius0 = fx_mul( obj_fx( arr_get( coords, 2 ) ), scale );
    shade->radius1 = fx_mul( obj_fx( arr_get( coords, 5 ) ), scale );
    if ( shade->radius1 >= shade->radius0 ) {
        shade->centre_x = px1;
        shade->centre_y = py1;
    } else {
        shade->centre_x = px0;
        shade->centre_y = py0;
    }
    return shade;
}

void shade_paint( SHADE *shade, PAINT *paint )
{
    paint->row = shade_row;
    paint->ctx = shade;
    paint->none = 0;
}

/* the "sh" operator: the whole of what the clip leaves */
void shade_fill( SHADE *shade, CANVAS *cv, const CLIP *clip, int alpha, int blend )
{
    PAINT paint;
    int x0, y0, x1, y1;

    mem_set( &paint, 0, sizeof( paint ) );
    paint.alpha = (u8)alpha;
    paint.blend = (u8)blend;
    shade_paint( shade, &paint );
    x0 = clip->x0;
    y0 = clip->y0;
    x1 = clip->x1;
    y1 = clip->y1;
    if ( clip->mask ) {
        mask_box( clip->mask, &x0, &y0, &x1, &y1 );
    }
    paint_rect( cv, clip, &paint, x0, y0, x1, y1 );
}
