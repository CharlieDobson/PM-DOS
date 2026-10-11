/*
 * GLYPH.C - a character onto the canvas.
 *
 * A glyph's outline comes from its font program in the font's own
 * units, through gl_move and the rest, which turn each point into
 * pixels with the matrix in "gout".  At text sizes the result is
 * scanned once into a small map of coverage and kept: a page says "e"
 * three hundred times, and the second and later ones are a copy.  The
 * map is made at one of four positions across a pixel, because text
 * set to a quarter of a pixel keeps its spacing and text set to whole
 * pixels does not.
 *
 * A glyph too large to be worth keeping is filled straight into the
 * canvas.
 */
#include "gfx.h"

typedef struct GLYPH GLYPH;
struct GLYPH {
    GLYPH *next;
    FONT  *font;
    u32    code;
    s32    ma, mb, mc, md;
    s16    left, top;               /* the map's corner, from the origin's pixel */
    s16    width, height;
    u8     phase;
    u8     bits[3];
};

#define BUCKETS     512
#define KEEP_BYTES  (256UL * 1024)
#define KEEP_EM     72              /* pixels: larger is not kept */

GLYPHOUT gout;

static GLYPH *buckets[BUCKETS];
static POOL  *keep;
static u32    kept;
static PATH   gpath;
static int    busy;                 /* a glyph is in hand: the cache stays */

/* ------------------------------------------------------------------ */
/* where an outline goes                                               */
/* ------------------------------------------------------------------ */

static void to_pixels( fx ux, fx uy, fx *px, fx *py )
{
    *px = gout.ox + mul_shr( ux, gout.ma, 24 ) + mul_shr( uy, gout.mc, 24 );
    *py = gout.oy + mul_shr( ux, gout.mb, 24 ) + mul_shr( uy, gout.md, 24 );
}

void gl_move( fx ux, fx uy )
{
    fx px, py;

    to_pixels( ux, uy, &px, &py );
    path_move( gout.path, px, py );
    gout.last_x = ux;
    gout.last_y = uy;
}

void gl_line( fx ux, fx uy )
{
    fx px, py;

    to_pixels( ux, uy, &px, &py );
    path_line( gout.path, px, py );
    gout.last_x = ux;
    gout.last_y = uy;
}

void gl_curve( fx x1, fx y1, fx x2, fx y2, fx x3, fx y3 )
{
    fx px1, py1, px2, py2, px3, py3;

    to_pixels( x1, y1, &px1, &py1 );
    to_pixels( x2, y2, &px2, &py2 );
    to_pixels( x3, y3, &px3, &py3 );
    path_curve( gout.path, px1, py1, px2, py2, px3, py3 );
    gout.last_x = x3;
    gout.last_y = y3;
}

/* TrueType's curve, with one control point: the cubic that is the same */
void gl_quad( fx cx, fx cy, fx ux, fx uy )
{
    gl_curve( gout.last_x + (cx - gout.last_x) / 3 * 2, gout.last_y + (cy - gout.last_y) / 3 * 2,
              ux + (cx - ux) / 3 * 2, uy + (cy - uy) / 3 * 2, ux, uy );
}

void gl_close( void )
{
    path_close( gout.path );
}

/* ------------------------------------------------------------------ */
/* the cache                                                           */
/* ------------------------------------------------------------------ */

static void glyph_flush( void )
{
    if ( keep ) {
        pool_free( keep );
        keep = NULL;
    }
    kept = 0;
    mem_set( buckets, 0, sizeof( buckets ) );
}

int glyph_reclaim( void )
{
    if ( kept == 0 || busy ) {
        return 0;
    }
    glyph_flush();
    return 1;
}

/* what can be given back while a page is being drawn: the glyphs, and
   the fonts this page has not used */
int cache_reclaim( void )
{
    return glyph_reclaim() || (!busy && font_reclaim( 0 )) || raster_reclaim();
}

/* a page that was given up may have left a glyph in hand */
void glyph_page_begin( void )
{
    busy = 0;
}

typedef struct {
    u8  *bits;
    int  x0, y0, width;
} MAPCTX;

static void map_row( void *ctx, int ypos, int x0, int x1, const u8 *cov )
{
    MAPCTX *map = (MAPCTX *)ctx;

    mem_cpy( map->bits + (u32)(ypos - map->y0) * (u32)map->width + (u32)(x0 - map->x0), cov,
             (u32)(x1 - x0) );
}

/* the outline of "code" into gpath and from there to the rasteriser:
   0 if there is nothing to draw */
static int outline_edges( FONT *font, u32 code )
{
    STROKE stroke;
    fx pen = 0;
    int kind;

    path_reset( &gpath );
    gout.path = &gpath;
    kind = font_outline( font, code, &pen );
    if ( kind == 0 || gpath.count == 0 ) {
        return 0;
    }
    ras_begin();
    if ( kind == 2 ) {
        /* the built-in font: a pen's path, to be stroked */
        mem_set( &stroke, 0, sizeof( stroke ) );
        stroke.width = mul_shr( pen, fx_hypot( gout.mc >> 8, gout.md >> 8 ), 16 );
        stroke.cap = 1;
        stroke.join = 1;
        path_stroke_edges( &gpath, &stroke );
    } else {
        path_edges( &gpath );
    }
    return !ras_empty();
}

static s32 mag( s32 val )
{
    return val < 0 ? -val : val;
}

static void glyph_put( FONT *font, u32 code, fx xpos, fx ypos, const s32 *mat, CANVAS *cv,
                       const CLIP *clip, const PAINT *paint )
{
    GLYPH *glyph;
    MAPCTX map;
    fx bx0, by0, bx1, by1;
    s32 across = mag( mat[0] ) + mag( mat[2] ), down = mag( mat[1] ) + mag( mat[3] );
    u32 hash, size;
    int em, phase, ix, iy, row, x0, y0, x1, y1;

    if ( font->kind == FT_TYPE3 || paint->none ) {
        return;
    }
    em = (int)mul_shr( across > down ? across : down, font->units, 24 );
    gout.ma = mat[0];
    gout.mb = mat[1];
    gout.mc = mat[2];
    gout.md = mat[3];
    if ( em > KEEP_EM ) {
        gout.ox = xpos;
        gout.oy = ypos;
        if ( outline_edges( font, code ) ) {
            ras_fill( cv, clip, 0, paint );
        }
        return;
    }
    phase = (int)((xpos >> 14) & 3);
    ix = (int)FX_FLOOR( xpos );
    iy = (int)FX_ROUND( ypos );
    hash = (code * 31 + (u32)font->key * 7 + (u32)(mat[0] >> 6) + (u32)(mat[3] >> 6) * 5 +
            (u32)phase * 131 + (u32)font) & (BUCKETS - 1);
    for ( glyph = buckets[hash]; glyph; glyph = glyph->next ) {
        if ( glyph->font == font && glyph->code == code && glyph->phase == phase &&
             glyph->ma == mat[0] && glyph->mb == mat[1] && glyph->mc == mat[2] && glyph->md == mat[3] ) {
            break;
        }
    }
    if ( glyph == NULL ) {
        gout.ox = (fx)phase << 14;
        gout.oy = 0;
        x0 = y0 = x1 = y1 = 0;
        if ( outline_edges( font, code ) ) {
            path_bounds( &gpath, &bx0, &by0, &bx1, &by1 );
            /* a pen's path is wider than its points by half the pen */
            x0 = (int)FX_FLOOR( bx0 ) - 3;
            y0 = (int)FX_FLOOR( by0 ) - 3;
            x1 = (int)FX_CEIL( bx1 ) + 3;
            y1 = (int)FX_CEIL( by1 ) + 3;
            if ( x1 - x0 > 400 || y1 - y0 > 400 ) {
                /* not a shape to keep after all */
                gout.ox = xpos;
                gout.oy = ypos;
                if ( outline_edges( font, code ) ) {
                    ras_fill( cv, clip, 0, paint );
                }
                return;
            }
        }
        size = sizeof( GLYPH ) + (u32)(x1 - x0) * (u32)(y1 - y0);
        if ( kept + size > KEEP_BYTES ) {
            glyph_flush();
        }
        if ( keep == NULL ) {
            keep = pool_new();
        }
        glyph = (GLYPH *)pool_alloc( keep, size );
        kept += size;
        glyph->font = font;
        glyph->code = code;
        glyph->phase = (u8)phase;
        glyph->ma = mat[0];
        glyph->mb = mat[1];
        glyph->mc = mat[2];
        glyph->md = mat[3];
        glyph->left = (s16)x0;
        glyph->top = (s16)y0;
        glyph->width = (s16)(x1 - x0);
        glyph->height = (s16)(y1 - y0);
        if ( glyph->width ) {
            map.bits = glyph->bits;
            map.x0 = x0;
            map.y0 = y0;
            map.width = x1 - x0;
            ras_scan( x0, y0, x1, y1, 0, map_row, &map );
        }
        glyph->next = buckets[hash];
        buckets[hash] = glyph;
    }
    for ( row = 0; row < glyph->height; row++ ) {
        paint_row( cv, clip, paint, iy + glyph->top + row, ix + glyph->left,
                   ix + glyph->left + glyph->width, glyph->bits + (u32)row * (u32)glyph->width );
    }
}

void glyph_draw( FONT *font, u32 code, fx xpos, fx ypos, const s32 *mat, CANVAS *cv,
                 const CLIP *clip, const PAINT *paint )
{
    busy++;
    glyph_put( font, code, xpos, ypos, mat, cv, clip, paint );
    busy--;
}
