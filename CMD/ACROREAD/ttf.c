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
 */
#include "gfx.h"

typedef struct {
    u8  *data;
    u32  len;
    u32  glyf, loca, head, maxp, cmap;
    u32  glyf_len, loca_len, cmap_len;
    u32  cff, cff_len;
    int  units, glyphs, loca_long;
} TTF;

typedef struct {
    s16 x, y;
    u8  on;
} TPOINT;

static TPOINT *points;
static u32     point_cap;

static u32 rd16( const TTF *ttf, u32 off )
{
    if ( off + 2 > ttf->len ) {
        return 0;
    }
    return ((u32)ttf->data[off] << 8) | ttf->data[off + 1];
}

static s32 rds16( const TTF *ttf, u32 off )
{
    return (s16)rd16( ttf, off );
}

static u32 rd32( const TTF *ttf, u32 off )
{
    return (rd16( ttf, off ) << 16) | rd16( ttf, off + 2 );
}

void *ttf_open( POOL *pool, u8 *data, u32 len )
{
    TTF *ttf = (TTF *)pool_alloc( pool, sizeof( TTF ) );
    u32 base = 0, tables, index, rec, tag, off, size;

    ttf->data = data;
    ttf->len = len;
    if ( rd32( ttf, 0 ) == 0x74746366UL ) {         /* 'ttcf': the first font in it */
        base = rd32( ttf, 12 );
    }
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
        if ( index && (ttf->glyphs == 0 || (u32)ttf->glyphs > index - 1) ) {
            ttf->glyphs = (int)index - 1;
        }
    }
    if ( ttf->cff == 0 && (ttf->glyf == 0 || ttf->loca == 0) ) {
        return NULL;
    }
    return ttf;
}

int ttf_units( void *prog )
{
    return ((TTF *)prog)->units;
}

int ttf_cff( void *prog, u8 **data, u32 *len )
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

static u32 cmap_sub( const TTF *ttf, int platform, int encoding )
{
    u32 count, index, rec;

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

static u32 cmap_lookup( const TTF *ttf, u32 sub, u32 code )
{
    u32 format = rd16( ttf, sub ), segs, lo, hi, mid, start, end, range, first, count, at;

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

int ttf_cmap( void *prog, int platform, int encoding, u32 code )
{
    TTF *ttf = (TTF *)prog;
    u32 sub = cmap_sub( ttf, platform, encoding );

    return sub ? (int)cmap_lookup( ttf, sub, code ) : 0;
}

/* The character a glyph is, by the font's own Unicode map read
   backwards: for the text of a font that came with no ToUnicode.  Slow -
   every code in the map is tried - so only the text view asks. */
u16 ttf_unicode_of( void *prog, u32 gid )
{
    TTF *ttf = (TTF *)prog;
    u32 sub = cmap_sub( ttf, 3, 1 ), segs, seg, start, end, code;

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
            if ( cmap_lookup( ttf, sub, code ) == gid ) {
                return (u16)code;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* outlines                                                            */
/* ------------------------------------------------------------------ */

static void emit_point( const FMAT *xf, s32 px, s32 py, fx *ox, fx *oy )
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

static int glyph_emit( const TTF *ttf, u32 gid, const FMAT *xf, int depth )
{
    FMAT sub;
    TPOINT *grown;
    u32 at, next, off, flags_at, total, index, comp, arg;
    s32 contours, dx, dy, val;
    fx ma, mb, mc, md;
    int flag = 0, repeat, first;

    if ( gid >= (u32)ttf->glyphs || depth > 6 ) {
        return 0;
    }
    if ( ttf->loca_long ) {
        at = rd32( ttf, ttf->loca + gid * 4 );
        next = rd32( ttf, ttf->loca + gid * 4 + 4 );
    } else {
        at = rd16( ttf, ttf->loca + gid * 2 ) * 2;
        next = rd16( ttf, ttf->loca + gid * 2 + 2 ) * 2;
    }
    if ( next <= at || at + 10 > ttf->glyf_len ) {
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
                dx = (s8)ttf->data[off < ttf->len ? off : 0];
                dy = (s8)ttf->data[off + 1 < ttf->len ? off + 1 : 0];
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
    total = rd16( ttf, off + (u32)(contours - 1) * 2 ) + 1;
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
    flags_at = off + (u32)contours * 2;
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
        points[index].on = (u8)flag;
    }
    val = 0;
    for ( index = 0; index < total; index++ ) {
        flag = points[index].on;
        if ( flag & 2 ) {
            arg = flags_at < ttf->len ? ttf->data[flags_at] : 0;
            flags_at++;
            val += (flag & 0x10) ? (s32)arg : -(s32)arg;
        } else if ( !(flag & 0x10) ) {
            val += rds16( ttf, flags_at );
            flags_at += 2;
        }
        points[index].x = (s16)val;
    }
    val = 0;
    for ( index = 0; index < total; index++ ) {
        flag = points[index].on;
        if ( flag & 4 ) {
            arg = flags_at < ttf->len ? ttf->data[flags_at] : 0;
            flags_at++;
            val += (flag & 0x20) ? (s32)arg : -(s32)arg;
        } else if ( !(flag & 0x20) ) {
            val += rds16( ttf, flags_at );
            flags_at += 2;
        }
        points[index].y = (s16)val;
        points[index].on = (u8)(flag & 1);
    }
    first = 0;
    for ( index = 0; index < (u32)contours; index++ ) {
        next = rd16( ttf, off + index * 2 );
        if ( next >= total || (int)next < first ) {
            break;
        }
        emit_contour( xf, points + first, (int)next - first + 1 );
        first = (int)next + 1;
    }
    return 1;
}

int ttf_outline( void *prog, u32 gid )
{
    FMAT unit;

    unit.a = unit.d = FX_ONE;
    unit.b = unit.c = unit.e = unit.f = 0;
    return glyph_emit( (TTF *)prog, gid, &unit, 0 );
}
