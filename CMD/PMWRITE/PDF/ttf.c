/*
 * TTF.C - a TrueType font program: its tables, its character maps and
 * the outlines of its glyphs.
 *
 * What a PDF file embeds is usually a subset - only the glyphs the
 * document uses, and only the tables an interpreter cannot do without -
 * so nothing is assumed to be there: a table that is missing is an
 * offset of zero, and every read is checked against the length.
 *
 * The outlines are used as they are drawn.  The instructions that fit
 * them to a pixel grid are not run: with coverage for every pixel the
 * unfitted shape is the better one, and the interpreter for them is as
 * large as the rest of this program.
 *
 * A FONT OF THE USER'S (TF.C) is read here too, a whole file on the
 * heap: ttf_open_heap and what follows it give its widths, its heights,
 * its names and the glyph of a Unicode character, and ttf_subset writes
 * the file again with only the glyphs a document used, for a PDF.
 */
#include "gfx.h"

typedef struct {
    uint8_t *data;
    uint32_t len;
    uint32_t glyf, loca, head, maxp, cmap;
    uint32_t glyf_len, loca_len, cmap_len;
    uint32_t cff, cff_len;
    int      units, glyphs, loca_long;
    /* what a font of the user's is asked as well */
    uint32_t base;                          /* where its table directory is */
    uint32_t hmtx, hmtx_len, nhm;
    uint32_t uni;                           /* the map that takes Unicode, or 0... */
    int      uni_symbol;                    /* ...which is a symbol font's: F020h to F0FFh */
} TTF;

typedef struct {
    int16_t x, y;
    uint8_t on;
} TPOINT;

static TPOINT      *points;
static unsigned int point_cap;
static unsigned int parts;              /* glyphs drawn as parts of this one, against a loop */

static uint32_t rd16( const TTF *ttf, uint32_t off )
{
    if ( off > ttf->len || ttf->len - off < 2 ) {
        return 0;
    }
    return ((uint32_t)ttf->data[off] << 8) | ttf->data[off + 1];
}

static int32_t rds16( const TTF *ttf, uint32_t off )
{
    return (int16_t)rd16( ttf, off );
}

static uint32_t rd32( const TTF *ttf, uint32_t off )
{
    return (rd16( ttf, off ) << 16) | rd16( ttf, off + 2 );
}

/* what the file says of itself, into a TTF that is all zeros: 0 if it
   has no outlines of either kind */
static int ttf_fill( TTF *ttf, uint8_t *data, uint32_t len )
{
    uint32_t base = 0, tables;
    unsigned int index;
    uint32_t rec, tag, off, size;

    ttf->data = data;
    ttf->len = len;
    if ( rd32( ttf, 0 ) == 0x74746366UL ) {         /* 'ttcf': the first font in it */
        base = rd32( ttf, 12 );
    }
    ttf->base = base;
    tables = rd16( ttf, base + 4 );
    for ( index = 0; index < tables; index++ ) {
        rec = base + 12 + index * 16;
        tag = rd32( ttf, rec );
        off = rd32( ttf, rec + 8 );
        size = rd32( ttf, rec + 12 );
        if ( off >= len ) {
            continue;
        }
        if ( size > len - off ) {
            size = len - off;
        }
        switch ( tag ) {
        case 0x676C7966UL: ttf->glyf = off; ttf->glyf_len = size; break;
        case 0x6C6F6361UL: ttf->loca = off; ttf->loca_len = size; break;
        case 0x68656164UL: ttf->head = off; break;
        case 0x6D617870UL: ttf->maxp = off; break;
        case 0x636D6170UL: ttf->cmap = off; ttf->cmap_len = size; break;
        case 0x43464620UL: ttf->cff = off; ttf->cff_len = size; break;
        }
    }
    ttf->units = ttf->head ? (int)rd16( ttf, ttf->head + 18 ) : 1000;
    if ( ttf->units < 16 || ttf->units > 16384 ) {
        ttf->units = 1000;
    }
    ttf->loca_long = ttf->head ? (int)rd16( ttf, ttf->head + 50 ) : 0;
    ttf->glyphs = ttf->maxp ? (int)rd16( ttf, ttf->maxp + 4 ) : 0;
    if ( ttf->loca ) {
        /* what the table has room for, whatever maxp says */
        index = ttf->loca_len / (ttf->loca_long ? 4 : 2);
        if ( index && (ttf->glyphs == 0 || (uint32_t)ttf->glyphs > index - 1) ) {
            ttf->glyphs = (int)index - 1;
        }
    }
    return ttf->cff != 0 || (ttf->glyf != 0 && ttf->loca != 0);
}

void *ttf_open( POOL *pool, uint8_t *data, uint32_t len )
{
    TTF *ttf = (TTF *)pool_alloc( pool, sizeof( TTF ) );

    return ttf_fill( ttf, data, len ) ? ttf : NULL;
}

int ttf_units( void *prog )
{
    return ((TTF *)prog)->units;
}

int ttf_cff( void *prog, uint8_t **data, uint32_t *len )
{
    TTF *ttf = (TTF *)prog;

    if ( ttf->cff == 0 ) {
        return 0;
    }
    *data = ttf->data + ttf->cff;
    *len = ttf->cff_len;
    return 1;
}

/* ------------------------------------------------------------------ */
/* character maps                                                      */
/* ------------------------------------------------------------------ */

static uint32_t cmap_sub( const TTF *ttf, int platform, int encoding )
{
    uint32_t count;
    unsigned int index;
    uint32_t rec;

    if ( ttf->cmap == 0 ) {
        return 0;
    }
    count = rd16( ttf, ttf->cmap + 2 );
    for ( index = 0; index < count; index++ ) {
        rec = ttf->cmap + 4 + index * 8;
        if ( (int)rd16( ttf, rec ) == platform && (int)rd16( ttf, rec + 2 ) == encoding ) {
            return ttf->cmap + rd32( ttf, rec + 4 );
        }
    }
    return 0;
}

int ttf_has_cmap( void *prog, int platform, int encoding )
{
    return cmap_sub( (TTF *)prog, platform, encoding ) != 0;
}

static uint32_t cmap_lookup( const TTF *ttf, uint32_t sub, uint32_t code )
{
    uint32_t format = rd16( ttf, sub ), segs, lo, hi, mid, start, end, range, first, count, at;

    switch ( format ) {
    case 0:
        return code < 256 && sub + 6 + code < ttf->len ? ttf->data[sub + 6 + code] : 0;
    case 4:
        segs = rd16( ttf, sub + 6 ) / 2;
        lo = 0;
        hi = segs;
        while ( lo < hi ) {                         /* the first segment that ends at or after it */
            mid = (lo + hi) / 2;
            if ( rd16( ttf, sub + 14 + mid * 2 ) < code ) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if ( lo >= segs ) {
            return 0;
        }
        end = sub + 14 + lo * 2;
        start = rd16( ttf, end + 2 + segs * 2 );
        if ( code < start || code > 0xFFFF ) {
            return 0;
        }
        range = rd16( ttf, end + 2 + segs * 6 );
        if ( range == 0 ) {
            return (code + rd16( ttf, end + 2 + segs * 4 )) & 0xFFFF;
        }
        at = rd16( ttf, end + 2 + segs * 6 + range + (code - start) * 2 );
        return at ? (at + rd16( ttf, end + 2 + segs * 4 )) & 0xFFFF : 0;
    case 6:
        first = rd16( ttf, sub + 6 );
        count = rd16( ttf, sub + 8 );
        return code >= first && code - first < count ? rd16( ttf, sub + 10 + (code - first) * 2 ) : 0;
    case 12:
        count = rd32( ttf, sub + 12 );
        lo = 0;
        hi = count;
        while ( lo < hi ) {
            mid = (lo + hi) / 2;
            at = sub + 16 + mid * 12;
            if ( code < rd32( ttf, at ) ) {
                hi = mid;
            } else if ( code > rd32( ttf, at + 4 ) ) {
                lo = mid + 1;
            } else {
                return rd32( ttf, at + 8 ) + (code - rd32( ttf, at ));
            }
        }
        return 0;
    }
    return 0;
}

int ttf_cmap( void *prog, int platform, int encoding, uint32_t code )
{
    TTF *ttf = (TTF *)prog;
    uint32_t sub = cmap_sub( ttf, platform, encoding );

    return sub ? (int)cmap_lookup( ttf, sub, code ) : 0;
}

/* The character a glyph is, by the font's own Unicode map read
   backwards: for the text of a font that came with no ToUnicode.  Slow -
   every code in the map is tried - so only the text view asks. */
uint16_t ttf_unicode_of( void *prog, uint32_t gid )
{
    TTF *ttf = (TTF *)prog;
    uint32_t sub = cmap_sub( ttf, 3, 1 ), segs, seg, start, end, code, tried = 0;

    if ( sub == 0 ) {
        sub = cmap_sub( ttf, 0, 3 );
    }
    if ( sub == 0 || rd16( ttf, sub ) != 4 ) {
        return 0;
    }
    segs = rd16( ttf, sub + 6 ) / 2;
    for ( seg = 0; seg < segs; seg++ ) {
        end = rd16( ttf, sub + 14 + seg * 2 );
        start = rd16( ttf, sub + 16 + segs * 2 + seg * 2 );
        if ( end == 0xFFFF || end - start > 4096 ) {
            continue;
        }
        for ( code = start; code <= end; code++ ) {
            if ( ++tried > 65536UL ) {
                return 0;                           /* more than a whole map's worth */
            }
            if ( cmap_lookup( ttf, sub, code ) == gid ) {
                return (uint16_t)code;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* outlines                                                            */
/* ------------------------------------------------------------------ */

static void emit_point( const FMAT *xf, int32_t px, int32_t py, fx *ox, fx *oy )
{
    *ox = xf->a * px + xf->c * py + xf->e;
    *oy = xf->b * px + xf->d * py + xf->f;
}

/* one contour: points first to last, quadratic, with a point on the
   curve implied between two that are off it */
static void emit_contour( const FMAT *xf, const TPOINT *pts, int count )
{
    fx sx, sy, cx = 0, cy = 0, px, py, mx, my;
    int index, start = -1, have_ctl = 0;
    const TPOINT *pt;

    if ( count < 2 ) {
        return;
    }
    for ( index = 0; index < count; index++ ) {
        if ( pts[index].on ) {
            start = index;
            break;
        }
    }
    if ( start >= 0 ) {
        emit_point( xf, pts[start].x, pts[start].y, &sx, &sy );
    } else {
        /* none on the curve: start between the first two */
        emit_point( xf, pts[0].x, pts[0].y, &px, &py );
        emit_point( xf, pts[1].x, pts[1].y, &mx, &my );
        sx = (px + mx) / 2;
        sy = (py + my) / 2;
        start = 0;
        cx = px;
        cy = py;
        have_ctl = 1;
    }
    gl_move( sx, sy );
    for ( index = 1; index <= count; index++ ) {
        pt = &pts[(start + index) % count];
        if ( index == count && pt->on ) {
            /* back at the start */
            if ( have_ctl ) {
                gl_quad( cx, cy, sx, sy );
            }
            break;
        }
        emit_point( xf, pt->x, pt->y, &px, &py );
        if ( pt->on ) {
            if ( have_ctl ) {
                gl_quad( cx, cy, px, py );
                have_ctl = 0;
            } else {
                gl_line( px, py );
            }
        } else {
            if ( have_ctl ) {
                mx = (cx + px) / 2;
                my = (cy + py) / 2;
                gl_quad( cx, cy, mx, my );
            }
            cx = px;
            cy = py;
            have_ctl = 1;
            if ( index == count ) {
                gl_quad( cx, cy, sx, sy );
            }
        }
    }
    gl_close();
}

static int glyph_emit( const TTF *ttf, uint32_t gid, const FMAT *xf, int depth )
{
    FMAT sub;
    TPOINT *grown;
    uint32_t at, next, off, flags_at, total;
    unsigned int index;
    uint32_t comp, arg;
    int32_t contours, dx, dy, val;
    fx ma, mb, mc, md;
    int flag = 0, repeat, first;

    if ( gid >= (uint32_t)ttf->glyphs || depth > 6 ) {
        return 0;
    }
    if ( ttf->loca_long ) {
        at = rd32( ttf, ttf->loca + gid * 4 );
        next = rd32( ttf, ttf->loca + gid * 4 + 4 );
    } else {
        at = rd16( ttf, ttf->loca + gid * 2 ) * 2;
        next = rd16( ttf, ttf->loca + gid * 2 + 2 ) * 2;
    }
    if ( next <= at || at > ttf->glyf_len || ttf->glyf_len - at < 10 ) {
        return 1;                                   /* a glyph with no outline: a space */
    }
    at += ttf->glyf;
    contours = rds16( ttf, at );
    off = at + 10;

    if ( contours < 0 ) {
        /* made of other glyphs */
        for ( ;; ) {
            flags_at = rd16( ttf, off );
            comp = rd16( ttf, off + 2 );
            off += 4;
            if ( flags_at & 1 ) {                   /* the arguments are words */
                dx = rds16( ttf, off );
                dy = rds16( ttf, off + 2 );
                off += 4;
            } else {
                dx = (int8_t)ttf->data[off < ttf->len ? off : 0];
                dy = (int8_t)ttf->data[off + 1 < ttf->len ? off + 1 : 0];
                off += 2;
            }
            if ( !(flags_at & 2) ) {                /* points to line up, not an offset */
                dx = dy = 0;
            }
            ma = md = FX_ONE;
            mb = mc = 0;
            if ( flags_at & 8 ) {
                ma = md = rds16( ttf, off ) * 4;    /* 2.14 */
                off += 2;
            } else if ( flags_at & 0x40 ) {
                ma = rds16( ttf, off ) * 4;
                md = rds16( ttf, off + 2 ) * 4;
                off += 4;
            } else if ( flags_at & 0x80 ) {
                ma = rds16( ttf, off ) * 4;
                mb = rds16( ttf, off + 2 ) * 4;
                mc = rds16( ttf, off + 4 ) * 4;
                md = rds16( ttf, off + 6 ) * 4;
                off += 8;
            }
            /* the component's matrix, then this glyph's */
            sub.a = fx_mul( ma, xf->a ) + fx_mul( mb, xf->c );
            sub.b = fx_mul( ma, xf->b ) + fx_mul( mb, xf->d );
            sub.c = fx_mul( mc, xf->a ) + fx_mul( md, xf->c );
            sub.d = fx_mul( mc, xf->b ) + fx_mul( md, xf->d );
            sub.e = xf->a * dx + xf->c * dy + xf->e;
            sub.f = xf->b * dx + xf->d * dy + xf->f;
            if ( ++parts > 256 ) {
                break;
            }
            glyph_emit( ttf, comp, &sub, depth + 1 );
            if ( !(flags_at & 0x20) || off >= ttf->len ) {
                break;
            }
        }
        return 1;
    }
    if ( contours == 0 ) {
        return 1;
    }
    total = rd16( ttf, off + (uint32_t)(contours - 1) * 2 ) + 1;
    if ( total > 20000 ) {
        return 0;
    }
    if ( total > point_cap ) {
        grown = (TPOINT *)xalloc( (total + 64) * sizeof( TPOINT ) );
        if ( points ) {
            xfree( points );
        }
        points = grown;
        point_cap = total + 64;
    }
    flags_at = off + (uint32_t)contours * 2;
    flags_at += 2 + rd16( ttf, flags_at );          /* past the instructions */
    /* the flags, a byte a point with runs; then the x's; then the y's */
    repeat = 0;
    for ( index = 0; index < total; index++ ) {
        if ( repeat ) {
            repeat--;
        } else {
            if ( flags_at >= ttf->len ) {
                return 0;
            }
            flag = ttf->data[flags_at++];
            if ( flag & 8 ) {
                repeat = flags_at < ttf->len ? ttf->data[flags_at++] : 0;
            }
        }
        points[index].on = (uint8_t)flag;
    }
    val = 0;
    for ( index = 0; index < total; index++ ) {
        flag = points[index].on;
        if ( flag & 2 ) {
            arg = flags_at < ttf->len ? ttf->data[flags_at] : 0;
            flags_at++;
            val += (flag & 0x10) ? (int32_t)arg : -(int32_t)arg;
        } else if ( !(flag & 0x10) ) {
            val += rds16( ttf, flags_at );
            flags_at += 2;
        }
        points[index].x = (int16_t)val;
    }
    val = 0;
    for ( index = 0; index < total; index++ ) {
        flag = points[index].on;
        if ( flag & 4 ) {
            arg = flags_at < ttf->len ? ttf->data[flags_at] : 0;
            flags_at++;
            val += (flag & 0x20) ? (int32_t)arg : -(int32_t)arg;
        } else if ( !(flag & 0x20) ) {
            val += rds16( ttf, flags_at );
            flags_at += 2;
        }
        points[index].y = (int16_t)val;
        points[index].on = (uint8_t)(flag & 1);
    }
    first = 0;
    for ( index = 0; index < (unsigned int)contours; index++ ) {
        next = rd16( ttf, off + index * 2 );
        if ( next >= total || (int)next < first ) {
            break;
        }
        emit_contour( xf, points + first, (int)next - first + 1 );
        first = (int)next + 1;
    }
    return 1;
}

/* the points of the glyph that was drawn last (pdf_done) */
void ttf_done( void )
{
    xfree( points );
    points = NULL;
    point_cap = 0;
}

int ttf_outline( void *prog, uint32_t gid )
{
    FMAT unit;

    unit.a = unit.d = FX_ONE;
    unit.b = unit.c = unit.e = unit.f = 0;
    parts = 0;
    return glyph_emit( (TTF *)prog, gid, &unit, 0 );
}

/* ------------------------------------------------------------------ */
/* a font of the user's                                                */
/* ------------------------------------------------------------------ */

#define TAG( c1, c2, c3, c4 )   (((uint32_t)(c1) << 24) | ((uint32_t)(c2) << 16) | ((uint32_t)(c3) << 8) | (uint32_t)(c4))

#define SUBSET_FILE     0x4000000UL         /* a file larger than this is not made a subset of */

/* a table by its tag: where it is, and how much of it the file has */
static int table_find( const TTF *ttf, uint32_t tag, uint32_t *off, uint32_t *size )
{
    uint32_t tables = rd16( ttf, ttf->base + 4 );
    unsigned int index;
    uint32_t rec;

    for ( index = 0; index < tables; index++ ) {
        rec = ttf->base + 12 + index * 16;
        if ( rd32( ttf, rec ) != tag ) {
            continue;
        }
        *off = rd32( ttf, rec + 8 );
        *size = rd32( ttf, rec + 12 );
        if ( *off >= ttf->len ) {
            return 0;
        }
        if ( *size > ttf->len - *off ) {
            *size = ttf->len - *off;
        }
        return 1;
    }
    return 0;
}

/*
 * A whole font file that is in a block of the caller's.  The reading
 * of it is a block from the heap, which xfree gives back; NULL when
 * there is no memory for that, or the file has no TrueType outlines
 * (an OpenType font whose outlines are CFF is one such).
 */
void *ttf_open_heap( uint8_t *data, uint32_t len )
{
    static const uint8_t maps[8][2] = {
        { 3, 10 }, { 3, 1 }, { 0, 4 }, { 0, 3 }, { 0, 2 }, { 0, 1 }, { 0, 0 }, { 3, 0 }
    };
    TTF *ttf = (TTF *)try_alloc( sizeof( TTF ) );
    uint32_t hhea, size;
    unsigned int index;
    uint32_t sub, format;

    if ( ttf == NULL ) {
        return NULL;
    }
    mem_set( ttf, 0, sizeof( *ttf ) );
    if ( !ttf_fill( ttf, data, len ) || ttf->glyf == 0 || ttf->loca == 0 || ttf->glyphs <= 0 ) {
        xfree( ttf );
        return NULL;
    }
    if ( table_find( ttf, TAG( 'h', 'h', 'e', 'a' ), &hhea, &size ) && size >= 36 ) {
        ttf->nhm = rd16( ttf, hhea + 34 );
    }
    if ( !table_find( ttf, TAG( 'h', 'm', 't', 'x' ), &ttf->hmtx, &ttf->hmtx_len ) ) {
        ttf->hmtx = ttf->hmtx_len = 0;
    }
    /* the map from Unicode: the widest first, a symbol font's last */
    for ( index = 0; index < 8 && ttf->uni == 0; index++ ) {
        sub = cmap_sub( ttf, maps[index][0], maps[index][1] );
        if ( sub == 0 ) {
            continue;
        }
        format = rd16( ttf, sub );
        if ( format == 0 || format == 4 || format == 6 || format == 12 ) {
            ttf->uni = sub;
            ttf->uni_symbol = maps[index][0] == 3 && maps[index][1] == 0;
        }
    }
    return ttf;
}

int ttf_glyph_count( void *prog )
{
    return ((TTF *)prog)->glyphs;
}

/* The glyph of a Unicode character, or 0.  A symbol font keeps its
   characters at F020h to F0FFh, and is asked for a plain byte there;
   a font that is not one is asked for such a code as the plain byte. */
uint32_t ttf_glyph_of( void *prog, uint32_t code )
{
    TTF *ttf = (TTF *)prog;
    uint32_t gid = 0;

    if ( ttf->uni == 0 ) {
        return 0;
    }
    if ( ttf->uni_symbol && code >= 0x20 && code <= 0xFF ) {
        gid = cmap_lookup( ttf, ttf->uni, 0xF000 + code );
    }
    if ( gid == 0 ) {
        gid = cmap_lookup( ttf, ttf->uni, code );
    }
    if ( gid == 0 && !ttf->uni_symbol && code >= 0xF020 && code <= 0xF0FF ) {
        gid = cmap_lookup( ttf, ttf->uni, code - 0xF000 );
    }
    return gid < (uint32_t)ttf->glyphs ? gid : 0;
}

/* how far a glyph moves the pen, in the font's units */
int ttf_advance( void *prog, uint32_t gid )
{
    TTF *ttf = (TTF *)prog;

    if ( ttf->hmtx == 0 || ttf->nhm == 0 ) {
        return ttf->units / 2;
    }
    if ( gid >= ttf->nhm ) {
        gid = ttf->nhm - 1;                 /* the rest are as wide as the last */
    }
    if ( gid * 4 + 2 > ttf->hmtx_len ) {
        return 0;
    }
    return (int)rd16( ttf, ttf->hmtx + gid * 4 );
}

static uint32_t raw16( const uint8_t *at )
{
    return ((uint32_t)at[0] << 8) | at[1];
}

/*
 * One of a font's names out of its table of names, which is "len"
 * bytes at "table": 1 is the family, 6 the name PostScript knows it
 * by.  The English one Windows uses is taken before another language's,
 * and that before a Macintosh one.  ASCII comes out, a question mark
 * for anything else; the result is how long it is, 0 if there is none.
 */
int ttf_name( const uint8_t *table, uint32_t len, int id, char *out, unsigned int max )
{
    uint32_t count, strings, index, rec, size, off, at, code, used = 0, best_at = 0, best_size = 0;
    int score, best = 0, wide = 0;

    out[0] = 0;
    if ( len < 6 || max < 2 ) {
        return 0;
    }
    count = raw16( table + 2 );
    strings = raw16( table + 4 );
    for ( index = 0; index < count; index++ ) {
        rec = 6 + index * 12;
        if ( rec + 12 > len ) {
            break;
        }
        if ( (int)raw16( table + rec + 6 ) != id ) {
            continue;
        }
        size = raw16( table + rec + 8 );
        off = strings + raw16( table + rec + 10 );
        if ( size == 0 || off > len || size > len - off ) {
            continue;
        }
        switch ( raw16( table + rec ) ) {
        case 3:  score = raw16( table + rec + 4 ) == 0x0409 ? 4 : 2; break;
        case 0:  score = 3; break;
        case 1:  score = raw16( table + rec + 4 ) == 0 ? 1 : 0; break;
        default: score = 0; break;
        }
        if ( score > best ) {
            best = score;
            best_at = off;
            best_size = size;
            wide = raw16( table + rec ) != 1;       /* two bytes a character, the high one first */
        }
    }
    for ( at = 0; at + (wide ? 2 : 1) <= best_size && used + 1 < max; at += wide ? 2 : 1 ) {
        code = wide ? raw16( table + best_at + at ) : table[best_at + at];
        if ( code >= 0x20 ) {
            out[used++] = (char)(code < 0x7F ? code : '?');
        }
    }
    while ( used && out[used - 1] == ' ' ) {
        used--;
    }
    out[used] = 0;
    return (int)used;
}

/* what the layout and a PDF's description of the font want to know */
void ttf_facts( void *prog, TTFACTS *facts )
{
    TTF *ttf = (TTF *)prog;
    uint32_t off, size;
    int32_t rise = 0, fall = 0, lead = 0, extra;
    int have_hhea = 0;

    mem_set( facts, 0, sizeof( *facts ) );
    facts->units = ttf->units;
    facts->glyphs = ttf->glyphs;
    if ( table_find( ttf, TAG( 'h', 'e', 'a', 'd' ), &off, &size ) && size >= 54 ) {
        facts->box[0] = rds16( ttf, off + 36 );
        facts->box[1] = rds16( ttf, off + 38 );
        facts->box[2] = rds16( ttf, off + 40 );
        facts->box[3] = rds16( ttf, off + 42 );
        facts->whole = 1;
    }
    if ( table_find( ttf, TAG( 'h', 'h', 'e', 'a' ), &off, &size ) && size >= 36 ) {
        rise = rds16( ttf, off + 4 );
        fall = -rds16( ttf, off + 6 );
        lead = rds16( ttf, off + 8 );
        have_hhea = 1;
    } else {
        facts->whole = 0;
    }
    if ( !table_find( ttf, TAG( 'm', 'a', 'x', 'p' ), &off, &size ) || size < 6 || rd16( ttf, off + 4 ) == 0 ||
         ttf->hmtx == 0 || ttf->len > SUBSET_FILE ) {
        facts->whole = 0;
    }
    if ( have_hhea ) {
        facts->ascent = rise > 0 ? rise : 0;
        facts->descent = fall > 0 ? fall : 0;
        facts->gap = lead > 0 ? lead : 0;
    }
    facts->cap = ttf->units * 7 / 10;
    if ( table_find( ttf, TAG( 'O', 'S', '/', '2' ), &off, &size ) ) {
        if ( size >= 10 ) {
            facts->fstype = (uint16_t)rd16( ttf, off + 8 );
        }
        /* lines as Windows spaces them: its own two heights, and of the
           space the other table asks between lines what they leave over */
        if ( size >= 78 && rd16( ttf, off + 74 ) + rd16( ttf, off + 76 ) != 0 ) {
            facts->ascent = (int32_t)rd16( ttf, off + 74 );
            facts->descent = (int32_t)rd16( ttf, off + 76 );
            extra = have_hhea ? lead - ((facts->ascent + facts->descent) - (rise + fall)) : 0;
            facts->gap = extra > 0 ? extra : 0;
        }
        if ( rd16( ttf, off ) >= 2 && size >= 90 && rds16( ttf, off + 88 ) > 0 ) {
            facts->cap = rds16( ttf, off + 88 );
        }
    }
    if ( table_find( ttf, TAG( 'p', 'o', 's', 't' ), &off, &size ) && size >= 16 ) {
        facts->angle = rds16( ttf, off + 4 );           /* whole degrees: the fraction is let go */
        facts->fixed = (uint8_t)(rd32( ttf, off + 12 ) != 0);
    }
    if ( table_find( ttf, TAG( 'n', 'a', 'm', 'e' ), &off, &size ) ) {
        ttf_name( ttf->data + off, size, 6, facts->name, sizeof( facts->name ) );
    }
}

/* ------------------------------------------------------------------ */
/* a subset                                                            */
/* ------------------------------------------------------------------ */

static void wr16( uint8_t *at, uint32_t val )
{
    at[0] = (uint8_t)(val >> 8);
    at[1] = (uint8_t)val;
}

static void wr32( uint8_t *at, uint32_t val )
{
    wr16( at, val >> 16 );
    wr16( at + 2, val & 0xFFFF );
}

/* a table's checksum: its bytes as words, the high byte first, added
   up.  "len" is a multiple of four. */
static uint32_t sum_words( const uint8_t *data, uint32_t len )
{
    uint32_t sum = 0, at;

    for ( at = 0; at + 4 <= len; at += 4 ) {
        sum += ((uint32_t)data[at] << 24) | ((uint32_t)data[at + 1] << 16) | ((uint32_t)data[at + 2] << 8) | data[at + 3];
    }
    return sum;
}

/* where a glyph's outline is in the table of them, and how long: 0
   long for a glyph that has none */
static void glyph_span( const TTF *ttf, uint32_t gid, uint32_t *at, uint32_t *size )
{
    uint32_t from, to;

    *at = *size = 0;
    if ( gid >= (uint32_t)ttf->glyphs ) {
        return;
    }
    if ( ttf->loca_long ) {
        from = rd32( ttf, ttf->loca + gid * 4 );
        to = rd32( ttf, ttf->loca + gid * 4 + 4 );
    } else {
        from = rd16( ttf, ttf->loca + gid * 2 ) * 2;
        to = rd16( ttf, ttf->loca + gid * 2 + 2 ) * 2;
    }
    if ( to <= from || to > ttf->glyf_len ) {
        return;
    }
    *at = from;
    *size = to - from;
}

/* the glyphs a glyph is made of are wanted as it is */
static void mark_parts( const TTF *ttf, uint32_t gid, uint8_t *want )
{
    uint32_t at, size, off, end, flags, part;

    glyph_span( ttf, gid, &at, &size );
    if ( size < 10 || rds16( ttf, ttf->glyf + at ) >= 0 ) {
        return;
    }
    off = ttf->glyf + at + 10;
    end = ttf->glyf + at + size;
    while ( off + 4 <= end ) {
        flags = rd16( ttf, off );
        part = rd16( ttf, off + 2 );
        off += (flags & 1) ? 8 : 6;
        if ( flags & 8 ) {
            off += 2;
        } else if ( flags & 0x40 ) {
            off += 4;
        } else if ( flags & 0x80 ) {
            off += 8;
        }
        if ( part < (uint32_t)ttf->glyphs && want[part] == 0 ) {
            want[part] = 1;
        }
        if ( !(flags & 0x20) ) {
            break;
        }
    }
}

#define SUB_TABLES  9

/*
 * The font written again as a file of its own with the outlines of
 * only the glyphs that are wanted - "keep" is a byte a glyph, "count"
 * of them - and of the glyphs those are made of.  Every glyph keeps its
 * number: one that is not wanted is there with no outline.  The tables
 * are the ones a PDF reader draws from, and no others.  The result is a
 * block from the heap, *out_len long; NULL if there is no memory or the
 * file has not all of those tables.
 */
uint8_t *ttf_subset( void *prog, const uint8_t *keep, unsigned int count, uint32_t *out_len )
{
    /* in the order a table directory is to be in */
    static const uint32_t tags[SUB_TABLES] = {
        TAG( 'c', 'v', 't', ' ' ), TAG( 'f', 'p', 'g', 'm' ), TAG( 'g', 'l', 'y', 'f' ),
        TAG( 'h', 'e', 'a', 'd' ), TAG( 'h', 'h', 'e', 'a' ), TAG( 'h', 'm', 't', 'x' ),
        TAG( 'l', 'o', 'c', 'a' ), TAG( 'm', 'a', 'x', 'p' ), TAG( 'p', 'r', 'e', 'p' )
    };
    enum { SUB_CVT, SUB_FPGM, SUB_GLYF, SUB_HEAD, SUB_HHEA, SUB_HMTX, SUB_LOCA, SUB_MAXP, SUB_PREP };
    TTF *ttf = (TTF *)prog;
    uint8_t *want, *out, *rec, *glyf_out = NULL, *loca_out = NULL, *head_out = NULL;
    uint32_t from[SUB_TABLES], size[SUB_TABLES], glyphs = (uint32_t)ttf->glyphs, total, gid, at, len, outlines = 0;
    unsigned int index;
    uint32_t tables = 0, bytes, pos, power;
    unsigned int pass;
    int again;

    *out_len = 0;
    if ( ttf->len > SUBSET_FILE ) {
        return NULL;
    }
    for ( index = 0; index < SUB_TABLES; index++ ) {
        if ( !table_find( ttf, tags[index], &from[index], &size[index] ) ) {
            from[index] = size[index] = 0;
        }
    }
    if ( size[SUB_HEAD] < 54 || size[SUB_HHEA] < 36 || size[SUB_MAXP] < 6 || size[SUB_HMTX] == 0 ||
         size[SUB_GLYF] == 0 || size[SUB_LOCA] == 0 ) {
        return NULL;
    }
    total = rd16( ttf, from[SUB_MAXP] + 4 );        /* how many glyphs the file says it has */
    if ( total == 0 ) {
        return NULL;
    }
    if ( glyphs > total ) {
        glyphs = total;
    }
    want = (uint8_t *)try_alloc( glyphs + 1 );
    if ( want == NULL ) {
        return NULL;
    }
    mem_set( want, 0, glyphs + 1 );
    want[0] = 1;                                    /* the glyph for "no such character" */
    for ( gid = 0; gid < glyphs && gid < count; gid++ ) {
        if ( keep[gid] ) {
            want[gid] = 1;
        }
    }
    /* 1: wanted, its parts not yet looked for; 2: they have been.  A
       part may be made of parts, so more than once through - but no
       font goes eight deep */
    for ( pass = 0; pass < 8; pass++ ) {
        again = 0;
        for ( gid = 0; gid < glyphs; gid++ ) {
            if ( want[gid] == 1 ) {
                mark_parts( ttf, gid, want );
                want[gid] = 2;
                again = 1;
            }
        }
        if ( !again ) {
            break;
        }
    }
    /* how long the new table of outlines is: each on a boundary of
       four.  The outlines of a sound file do not overlap, so together
       they are no longer than the table they came from */
    for ( gid = 0; gid < glyphs; gid++ ) {
        if ( want[gid] ) {
            glyph_span( ttf, gid, &at, &len );
            outlines += (len + 3) & ~3UL;
            if ( outlines > ttf->glyf_len + 4 * glyphs ) {
                xfree( want );
                return NULL;
            }
        }
    }
    if ( outlines == 0 ) {
        outlines = 4;
    }
    size[SUB_GLYF] = outlines;
    size[SUB_LOCA] = (total + 1) * 4;
    bytes = 0;
    for ( index = 0; index < SUB_TABLES; index++ ) {
        if ( size[index] ) {
            tables++;
            bytes += (size[index] + 3) & ~3UL;
        }
    }
    bytes += 12 + tables * 16;
    out = (uint8_t *)try_alloc( bytes );
    if ( out == NULL ) {
        xfree( want );
        return NULL;
    }
    mem_set( out, 0, bytes );
    for ( power = 0; (2UL << power) <= tables; power++ ) {
    }
    wr32( out, 0x00010000UL );
    wr16( out + 4, tables );
    wr16( out + 6, 16UL << power );
    wr16( out + 8, power );
    wr16( out + 10, tables * 16 - (16UL << power) );
    rec = out + 12;
    pos = 12 + tables * 16;
    for ( index = 0; index < SUB_TABLES; index++ ) {
        if ( size[index] == 0 ) {
            continue;
        }
        if ( index == SUB_GLYF ) {
            glyf_out = out + pos;
        } else if ( index == SUB_LOCA ) {
            loca_out = out + pos;
        } else {
            mem_cpy( out + pos, ttf->data + from[index], size[index] );
        }
        if ( index == SUB_HEAD ) {
            head_out = out + pos;
            wr32( head_out + 8, 0 );                /* the file's checksum, put in at the end */
            wr16( head_out + 50, 1 );               /* the offsets to the outlines are long ones */
        }
        wr32( rec, tags[index] );
        wr32( rec + 8, pos );
        wr32( rec + 12, size[index] );
        rec += 16;
        pos += (size[index] + 3) & ~3UL;
    }
    /* the outlines, and where each is.  Both tables and "head" are
       there: none of the three had a size of 0 */
    pos = 0;
    for ( gid = 0; gid <= total; gid++ ) {
        wr32( loca_out + gid * 4, pos );
        if ( gid < glyphs && want[gid] ) {
            glyph_span( ttf, gid, &at, &len );
            if ( len && len <= outlines - pos ) {
                mem_cpy( glyf_out + pos, ttf->data + ttf->glyf + at, len );
                pos += (len + 3) & ~3UL;
            }
        }
    }
    xfree( want );
    /* each table's sum, and then the file's */
    rec = out + 12;
    for ( index = 0; index < tables; index++, rec += 16 ) {
        at = (raw16( rec + 8 ) << 16) | raw16( rec + 10 );
        len = (raw16( rec + 12 ) << 16) | raw16( rec + 14 );
        wr32( rec + 4, sum_words( out + at, (len + 3) & ~3UL ) );
    }
    wr32( head_out + 8, 0xB1B0AFBAUL - sum_words( out, bytes ) );
    *out_len = bytes;
    return out;
}
