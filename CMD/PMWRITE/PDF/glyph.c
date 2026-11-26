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
    GLYPH   *next;
    FONT    *font;
    uint32_t code;
    int32_t  ma, mb, mc, md;
    int16_t  left, top;             /* the map's corner, from the origin's pixel */
    int16_t  width, height;
    uint8_t  phase;
    uint8_t  bits[3];
};

#define BUCKETS     512
#define KEEP_BYTES  (256UL * 1024)
#define KEEP_EM     72              /* pixels: larger is not kept */

GLYPHOUT gout;

static GLYPH       *buckets[BUCKETS];
static POOL        *keep;
static unsigned int kept;
static PATH         gpath;
static int          busy;           /* a glyph is in hand: the cache stays */

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
    uint8_t *bits;
    int      x0, y0, width;
} MAPCTX;

static void map_row( void *ctx, int ypos, int x0, int x1, const uint8_t *cov )
{
    MAPCTX *map = (MAPCTX *)ctx;

    mem_cpy( map->bits + (uint32_t)(ypos - map->y0) * (uint32_t)map->width + (uint32_t)(x0 - map->x0), cov,
             (uint32_t)(x1 - x0) );
}

/* the outline of "code" into gpath and from there to the rasteriser:
   0 if there is nothing to draw */
static int outline_edges( FONT *font, uint32_t code )
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

static int32_t mag( int32_t val )
{
    return val < 0 ? -val : val;
}

/*
 * The kept map of a glyph, made now if it has not been.  NULL for a
 * glyph that is not kept - one too large, or one with nothing to draw
 * that is too large to tell - and then *direct says whether its edges
 * have been given to the rasteriser instead, placed at (xpos, ypos) and
 * waiting to be scanned.
 */
static GLYPH *glyph_find( FONT *font, uint32_t code, fx xpos, fx ypos, const int32_t *mat, int *direct )
{
    GLYPH *glyph;
    MAPCTX map;
    fx bx0, by0, bx1, by1;
    int32_t across = mag( mat[0] ) + mag( mat[2] ), down = mag( mat[1] ) + mag( mat[3] );
    uint32_t hash;
    unsigned int size;
    int em, phase, x0, y0, x1, y1;

    *direct = 0;
    em = (int)mul_shr( across > down ? across : down, font->units, 24 );
    gout.ma = mat[0];
    gout.mb = mat[1];
    gout.mc = mat[2];
    gout.md = mat[3];
    if ( em > KEEP_EM ) {
        gout.ox = xpos;
        gout.oy = ypos;
        *direct = outline_edges( font, code );
        return NULL;
    }
    phase = (int)((xpos >> 14) & 3);
    hash = (code * 31 + (uint32_t)font->key * 7 + (uint32_t)(mat[0] >> 6) + (uint32_t)(mat[3] >> 6) * 5 +
            (uint32_t)phase * 131 + (uint32_t)font) & (BUCKETS - 1);
    for ( glyph = buckets[hash]; glyph; glyph = glyph->next ) {
        if ( glyph->font == font && glyph->code == code && glyph->phase == phase &&
             glyph->ma == mat[0] && glyph->mb == mat[1] && glyph->mc == mat[2] && glyph->md == mat[3] ) {
            return glyph;
        }
    }
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
            *direct = outline_edges( font, code );
            return NULL;
        }
    }
    size = sizeof( GLYPH ) + (unsigned int)(x1 - x0) * (unsigned int)(y1 - y0);
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
    glyph->phase = (uint8_t)phase;
    glyph->ma = mat[0];
    glyph->mb = mat[1];
    glyph->mc = mat[2];
    glyph->md = mat[3];
    glyph->left = (int16_t)x0;
    glyph->top = (int16_t)y0;
    glyph->width = (int16_t)(x1 - x0);
    glyph->height = (int16_t)(y1 - y0);
    if ( glyph->width ) {
        map.bits = glyph->bits;
        map.x0 = x0;
        map.y0 = y0;
        map.width = x1 - x0;
        ras_scan( x0, y0, x1, y1, 0, map_row, &map );
    }
    glyph->next = buckets[hash];
    buckets[hash] = glyph;
    return glyph;
}

static void glyph_put( FONT *font, uint32_t code, fx xpos, fx ypos, const int32_t *mat, CANVAS *cv,
                       const CLIP *clip, const PAINT *paint )
{
    GLYPH *glyph;
    int direct, ix, iy, row;

    if ( font->kind == FT_TYPE3 || paint->none ) {
        return;
    }
    glyph = glyph_find( font, code, xpos, ypos, mat, &direct );
    if ( glyph == NULL ) {
        if ( direct ) {
            ras_fill( cv, clip, 0, paint );
        }
        return;
    }
    ix = (int)FX_FLOOR( xpos );
    iy = (int)FX_ROUND( ypos );
    for ( row = 0; row < glyph->height; row++ ) {
        paint_row( cv, clip, paint, iy + glyph->top + row, ix + glyph->left,
                   ix + glyph->left + glyph->width, glyph->bits + (uint32_t)row * (uint32_t)glyph->width );
    }
}

void glyph_draw( FONT *font, uint32_t code, fx xpos, fx ypos, const int32_t *mat, CANVAS *cv,
                 const CLIP *clip, const PAINT *paint )
{
    busy++;
    glyph_put( font, code, xpos, ypos, mat, cv, clip, paint );
    busy--;
}

/* The glyph as rows of coverage, for a caller that does its own
   painting: the rows and the part of each that lie inside the
   rectangle, which is the caller's whole picture or less. */
void glyph_cover( FONT *font, uint32_t code, fx xpos, fx ypos, const int32_t *mat, int x0, int y0,
                  int x1, int y1, ROWFN fn, void *ctx )
{
    GLYPH *glyph;
    const uint8_t *bits;
    int direct, ypos_row, left, right, row;

    if ( font->kind == FT_TYPE3 || x0 >= x1 || y0 >= y1 ) {
        return;
    }
    busy++;
    glyph = glyph_find( font, code, xpos, ypos, mat, &direct );
    if ( glyph == NULL ) {
        if ( direct ) {
            ras_scan( x0, y0, x1, y1, 0, fn, ctx );
        }
    } else {
        left = (int)FX_FLOOR( xpos ) + glyph->left;
        right = left + glyph->width;
        for ( row = 0; row < glyph->height; row++ ) {
            ypos_row = (int)FX_ROUND( ypos ) + glyph->top + row;
            if ( ypos_row < y0 || ypos_row >= y1 || left >= x1 || right <= x0 ) {
                continue;
            }
            bits = glyph->bits + (uint32_t)row * (uint32_t)glyph->width;
            fn( ctx, ypos_row, left < x0 ? x0 : left, right > x1 ? x1 : right,
                left < x0 ? bits + (x0 - left) : bits );
        }
    }
    busy--;
}

/* the path glyphs are drawn through, and the kept ones (pdf_done) */
void glyph_done( void )
{
    busy = 0;
    glyph_flush();
    path_free( &gpath );
}
