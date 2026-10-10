/*
 * SFONT.C - the font that is built in, for text whose own font is not
 * in the file.
 *
 * A PDF file need not carry the fourteen standard fonts - Helvetica,
 * Times, Courier and the rest - and may leave out any other font if it
 * gives the widths; the reader is expected to have something to draw
 * with.  PM-DOS has no fonts at all, so this is one: every character
 * as the path of a pen, on a grid of twenty units to the em, with the
 * capitals fourteen high and the small letters ten.  A pen's path can
 * be drawn at any size, slanted for an italic and drawn with a wider
 * pen for a bold, and it is small: the whole of it is about two
 * thousand bytes.
 *
 * It is drawn to the width the file gives each character, so a line
 * set in Times or in Arial is as long as it should be and ends where
 * it should, whatever the letters look like.  The widths of the
 * standard fonts themselves are in the tables at the bottom, for the
 * files that rely on a reader knowing them.
 */
#include "gfx.h"

#define MV      100             /* lift the pen and move */
#define AR      101             /* a quarter of an ellipse: round this corner, to there */
#define EN      102             /* the end of the character */

#define G_BULLET    95
#define G_ENDASH    96
#define G_EMDASH    97
#define G_QLEFT     98
#define G_QRIGHT    99
#define G_BOX       100
#define G_GRAVE     101         /* the accents: grave acute circumflex tilde dieresis
                                   ring cedilla caron, in that order */
#define G_ELLIPSIS  109
#define G_CHECK     110
#define G_ARROWR    111
#define G_ARROWL    112
#define G_COUNT     113

/* each: its width, then the pen's path.  The first 95 are ASCII from
   '!' on. */
static const s8 strokes[] = {
    /* ! */ 6, 3,14, 3,4, MV, 3,0, 3,1, EN,
    /* " */ 8, 2,14, 2,10, MV, 6,14, 6,10, EN,
    /* # */ 12, 5,14, 3,0, MV, 9,14, 7,0, MV, 1,9, 11,9, MV, 1,5, 11,5, EN,
    /* $ */ 12, 10,10, AR, 10,13, 6,13, AR, 2,13, 2,10, AR, 2,7, 6,7, AR, 10,7, 10,4,
            AR, 10,1, 6,1, AR, 2,1, 2,4, MV, 6,15, 6,-1, EN,
    /* % */ 18, 4,14, AR, 7,14, 7,11, AR, 7,8, 4,8, AR, 1,8, 1,11, AR, 1,14, 4,14, MV, 13,14, 5,0,
            MV, 14,6, AR, 17,6, 17,3, AR, 17,0, 14,0, AR, 11,0, 11,3, AR, 11,6, 14,6, EN,
    /* & */ 14, 13,0, 4,10, AR, 4,14, 7,14, AR, 10,14, 10,11, 2,4, AR, 2,0, 6,0, AR, 12,0, 12,5, EN,
    /* ' */ 4, 2,14, 2,10, EN,
    /* ( */ 7, 5,15, AR, 2,15, 2,6, AR, 2,-3, 5,-3, EN,
    /* ) */ 7, 2,15, AR, 5,15, 5,6, AR, 5,-3, 2,-3, EN,
    /* * */ 10, 5,14, 5,8, MV, 2,13, 8,9, MV, 8,13, 2,9, EN,
    /* + */ 12, 6,11, 6,3, MV, 2,7, 10,7, EN,
    /* , */ 6, 3,1, 3,0, 2,-3, EN,
    /* - */ 7, 1,5, 6,5, EN,
    /* . */ 6, 3,0, 3,1, EN,
    /* / */ 6, 5,15, 1,-2, EN,
    /* 0 */ 12, 6,14, AR, 10,14, 10,7, AR, 10,0, 6,0, AR, 2,0, 2,7, AR, 2,14, 6,14, EN,
    /* 1 */ 12, 4,11, 7,14, 7,0, EN,
    /* 2 */ 12, 2,11, AR, 2,14, 6,14, AR, 10,14, 10,10, 2,0, 10,0, EN,
    /* 3 */ 12, 2,11, AR, 2,14, 6,14, AR, 10,14, 10,11, AR, 10,8, 6,8, 5,8, MV, 6,8,
            AR, 10,8, 10,4, AR, 10,0, 6,0, AR, 2,0, 2,3, EN,
    /* 4 */ 12, 8,0, 8,14, 1,4, 11,4, EN,
    /* 5 */ 12, 10,14, 3,14, 3,8, 6,8, AR, 10,8, 10,4, AR, 10,0, 6,0, AR, 2,0, 2,3, EN,
    /* 6 */ 12, 10,11, AR, 10,14, 6,14, AR, 2,14, 2,7, 2,4, AR, 2,0, 6,0, AR, 10,0, 10,4,
            AR, 10,8, 6,8, AR, 2,8, 2,4, EN,
    /* 7 */ 12, 2,14, 10,14, 4,0, EN,
    /* 8 */ 12, 6,14, AR, 9,14, 9,11, AR, 9,8, 6,8, AR, 3,8, 3,11, AR, 3,14, 6,14, MV, 6,8,
            AR, 10,8, 10,4, AR, 10,0, 6,0, AR, 2,0, 2,4, AR, 2,8, 6,8, EN,
    /* 9 */ 12, 2,3, AR, 2,0, 6,0, AR, 10,0, 10,7, 10,10, AR, 10,14, 6,14, AR, 2,14, 2,10,
            AR, 2,6, 6,6, AR, 10,6, 10,10, EN,
    /* : */ 6, 3,0, 3,1, MV, 3,9, 3,10, EN,
    /* ; */ 6, 3,1, 3,0, 2,-3, MV, 3,9, 3,10, EN,
    /* < */ 12, 10,12, 2,7, 10,2, EN,
    /* = */ 12, 2,9, 10,9, MV, 2,5, 10,5, EN,
    /* > */ 12, 2,12, 10,7, 2,2, EN,
    /* ? */ 11, 2,11, AR, 2,14, 6,14, AR, 10,14, 10,11, 6,7, 6,4, MV, 6,0, 6,1, EN,
    /* @ */ 20, 13,7, AR, 13,10, 10,10, AR, 7,10, 7,7, AR, 7,4, 10,4, AR, 13,4, 13,7, MV, 13,10, 13,5,
            AR, 13,4, 15,4, AR, 18,4, 18,8, AR, 18,15, 10,15, AR, 2,15, 2,7, AR, 2,-1, 10,-1,
            15,-1, EN,
    /* A */ 14, 1,0, 7,14, 13,0, MV, 3,5, 11,5, EN,
    /* B */ 15, 2,0, 2,14, 8,14, AR, 11,14, 11,11, AR, 11,8, 8,8, 2,8, MV, 8,8, 9,8,
            AR, 13,8, 13,4, AR, 13,0, 9,0, 2,0, EN,
    /* C */ 15, 13,10, AR, 13,14, 8,14, AR, 2,14, 2,7, AR, 2,0, 8,0, AR, 13,0, 13,4, EN,
    /* D */ 15, 2,0, 2,14, 7,14, AR, 13,14, 13,7, AR, 13,0, 7,0, 2,0, EN,
    /* E */ 13, 11,14, 2,14, 2,0, 11,0, MV, 2,7, 10,7, EN,
    /* F */ 12, 11,14, 2,14, 2,0, MV, 2,7, 9,7, EN,
    /* G */ 16, 14,10, AR, 14,14, 8,14, AR, 2,14, 2,7, AR, 2,0, 8,0, AR, 14,0, 14,5, 14,7, 9,7, EN,
    /* H */ 15, 2,0, 2,14, MV, 13,0, 13,14, MV, 2,7, 13,7, EN,
    /* I */ 6, 3,0, 3,14, EN,
    /* J */ 10, 8,14, 8,4, AR, 8,0, 5,0, AR, 2,0, 2,4, EN,
    /* K */ 14, 2,0, 2,14, MV, 12,14, 2,5, MV, 5,8, 13,0, EN,
    /* L */ 12, 2,14, 2,0, 11,0, EN,
    /* M */ 18, 2,0, 2,14, 9,2, 16,14, 16,0, EN,
    /* N */ 15, 2,0, 2,14, 13,0, 13,14, EN,
    /* O */ 16, 8,14, AR, 14,14, 14,7, AR, 14,0, 8,0, AR, 2,0, 2,7, AR, 2,14, 8,14, EN,
    /* P */ 14, 2,0, 2,14, 8,14, AR, 12,14, 12,10, AR, 12,6, 8,6, 2,6, EN,
    /* Q */ 16, 8,14, AR, 14,14, 14,7, AR, 14,0, 8,0, AR, 2,0, 2,7, AR, 2,14, 8,14, MV, 10,4, 15,-1, EN,
    /* R */ 14, 2,0, 2,14, 8,14, AR, 12,14, 12,10, AR, 12,6, 8,6, 2,6, MV, 8,6, 13,0, EN,
    /* S */ 14, 12,11, AR, 12,14, 7,14, AR, 2,14, 2,11, AR, 2,8, 7,8, AR, 12,8, 12,4,
            AR, 12,0, 7,0, AR, 2,0, 2,3, EN,
    /* T */ 12, 1,14, 11,14, MV, 6,0, 6,14, EN,
    /* U */ 14, 2,14, 2,5, AR, 2,0, 7,0, AR, 12,0, 12,5, 12,14, EN,
    /* V */ 14, 1,14, 7,0, 13,14, EN,
    /* W */ 20, 1,14, 5,0, 10,12, 15,0, 19,14, EN,
    /* X */ 14, 2,14, 12,0, MV, 12,14, 2,0, EN,
    /* Y */ 14, 1,14, 7,6, 13,14, MV, 7,6, 7,0, EN,
    /* Z */ 13, 2,14, 11,14, 2,0, 11,0, EN,
    /* [ */ 7, 6,15, 2,15, 2,-3, 6,-3, EN,
    /* \ */ 6, 1,15, 5,-2, EN,
    /* ] */ 7, 1,15, 5,15, 5,-3, 1,-3, EN,
    /* ^ */ 10, 2,8, 5,14, 8,8, EN,
    /* _ */ 11, 0,-3, 11,-3, EN,
    /* ` */ 6, 2,14, 4,11, EN,
    /* a */ 11, 3,8, AR, 3,10, 6,10, AR, 9,10, 9,7, 9,0, MV, 9,6, 5,6, AR, 2,6, 2,3, AR, 2,0, 5,0,
            6,0, AR, 9,0, 9,3, EN,
    /* b */ 12, 2,14, 2,0, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,5, AR, 10,0, 6,0, AR, 2,0, 2,4, EN,
    /* c */ 11, 9,7, AR, 9,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 9,0, 9,3, EN,
    /* d */ 12, 10,14, 10,0, MV, 10,6, AR, 10,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 10,0, 10,4, EN,
    /* e */ 12, 2,5, 10,5, AR, 10,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, 7,0, AR, 10,0, 10,2, EN,
    /* f */ 7, 7,14, 6,14, AR, 3,14, 3,11, 3,0, MV, 1,10, 6,10, EN,
    /* g */ 12, 10,10, 10,-1, AR, 10,-4, 6,-4, AR, 2,-4, 2,-2, MV, 10,6, AR, 10,10, 6,10,
            AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 10,0, 10,4, EN,
    /* h */ 12, 2,14, 2,0, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,6, 10,0, EN,
    /* i */ 4, 2,10, 2,0, MV, 2,13, 2,14, EN,
    /* j */ 5, 3,10, 3,-2, AR, 3,-4, 1,-4, 0,-4, MV, 3,13, 3,14, EN,
    /* k */ 11, 2,14, 2,0, MV, 9,10, 2,4, MV, 5,6, 10,0, EN,
    /* l */ 4, 2,14, 2,0, EN,
    /* m */ 16, 2,10, 2,0, MV, 2,6, AR, 2,10, 5,10, AR, 8,10, 8,6, 8,0, MV, 8,6, AR, 8,10, 11,10,
            AR, 14,10, 14,6, 14,0, EN,
    /* n */ 12, 2,10, 2,0, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,6, 10,0, EN,
    /* o */ 12, 6,10, AR, 10,10, 10,5, AR, 10,0, 6,0, AR, 2,0, 2,5, AR, 2,10, 6,10, EN,
    /* p */ 12, 2,10, 2,-4, MV, 2,6, AR, 2,10, 6,10, AR, 10,10, 10,5, AR, 10,0, 6,0, AR, 2,0, 2,4, EN,
    /* q */ 12, 10,10, 10,-4, MV, 10,6, AR, 10,10, 6,10, AR, 2,10, 2,5, AR, 2,0, 6,0, AR, 10,0, 10,4, EN,
    /* r */ 8, 2,10, 2,0, MV, 2,6, AR, 2,10, 6,10, 7,10, EN,
    /* s */ 10, 8,8, AR, 8,10, 5,10, AR, 2,10, 2,7, AR, 2,5, 5,5, AR, 8,5, 8,3, AR, 8,0, 5,0,
            AR, 2,0, 2,2, EN,
    /* t */ 7, 3,13, 3,2, AR, 3,0, 5,0, 6,0, MV, 1,10, 6,10, EN,
    /* u */ 12, 2,10, 2,4, AR, 2,0, 6,0, AR, 10,0, 10,4, MV, 10,10, 10,0, EN,
    /* v */ 10, 1,10, 5,0, 9,10, EN,
    /* w */ 16, 1,10, 4,0, 8,9, 12,0, 15,10, EN,
    /* x */ 10, 1,10, 9,0, MV, 9,10, 1,0, EN,
    /* y */ 10, 1,10, 5,0, MV, 9,10, 3,-4, EN,
    /* z */ 10, 2,10, 8,10, 2,0, 8,0, EN,
    /* { */ 7, 6,15, AR, 4,15, 4,13, 4,8, AR, 4,6, 2,6, AR, 4,6, 4,4, 4,-1, AR, 4,-3, 6,-3, EN,
    /* | */ 6, 3,15, 3,-4, EN,
    /* } */ 7, 1,15, AR, 3,15, 3,13, 3,8, AR, 3,6, 5,6, AR, 3,6, 3,4, 3,-1, AR, 3,-3, 1,-3, EN,
    /* ~ */ 12, 2,6, AR, 2,8, 4,8, AR, 6,8, 6,7, AR, 6,6, 8,6, AR, 10,6, 10,8, EN,
    /* bullet */ 10, 5,8, AR, 6,8, 6,7, AR, 6,6, 5,6, AR, 4,6, 4,7, AR, 4,8, 5,8, EN,
    /* en dash */ 10, 0,5, 10,5, EN,
    /* em dash */ 20, 0,5, 20,5, EN,
    /* left quote */ 4, 2,14, 3,11, EN,
    /* right quote */ 4, 3,14, 2,11, EN,
    /* a box, for a character there is no drawing of */ 10, 2,0, 8,0, 8,10, 2,10, 2,0, EN,
    /* grave */ 0, -2,14, 1,12, EN,
    /* acute */ 0, -1,12, 2,14, EN,
    /* circumflex */ 0, -3,12, 0,14, 3,12, EN,
    /* tilde */ 0, -3,12, -1,14, 1,12, 3,14, EN,
    /* dieresis */ 0, -2,13, -2,13, MV, 2,13, 2,13, EN,
    /* ring */ 0, 0,15, AR, 2,15, 2,13, AR, 2,11, 0,11, AR, -2,11, -2,13, AR, -2,15, 0,15, EN,
    /* cedilla */ 0, 0,0, 0,-2, -2,-3, EN,
    /* caron */ 0, -3,14, 0,12, 3,14, EN,
    /* ellipsis */ 20, 3,0, 3,1, MV, 10,0, 10,1, MV, 17,0, 17,1, EN,
    /* check */ 12, 2,6, 5,2, 11,13, EN,
    /* arrow right */ 14, 1,5, 13,5, MV, 9,9, 13,5, 9,1, EN,
    /* arrow left */ 14, 13,5, 1,5, MV, 5,9, 1,5, 5,1, EN
};

static u16 starts[G_COUNT];
static int starts_made;

static void starts_make( void )
{
    u32 pos = 0;
    int index;

    for ( index = 0; index < G_COUNT && pos < sizeof( strokes ); index++ ) {
        starts[index] = (u16)pos;
        while ( pos < sizeof( strokes ) && strokes[pos] != EN ) {
            pos++;
        }
        pos++;
    }
    starts_made = 1;
}

/* Latin-1 C0h to FFh: the letter, and the accent on it (0 none, then
   grave acute circumflex tilde dieresis ring cedilla) */
static const char latin_base[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPBaaaaaaaceeeeiiiidnooooo/ouuuuypy";
static const char latin_mark[] =
    "1234560712351235041234500123520012345607123512350412345001235205";

/* what to draw for a character: up to three drawings side by side, and
   an accent over the first.  0 if it is drawn as nothing. */
static int pick( u16 uni, int *parts, int *mark )
{
    const char *seq = NULL;
    int count = 0;

    *mark = 0;
    if ( uni >= 0x21 && uni <= 0x7E ) {
        parts[0] = uni - 0x21;
        return 1;
    }
    if ( uni >= 0xC0 && uni <= 0xFF && uni != 0xC6 && uni != 0xE6 ) {
        parts[0] = latin_base[uni - 0xC0] - 0x21;
        *mark = latin_mark[uni - 0xC0] - '0';
        return 1;
    }
    switch ( uni ) {
    case 0x0020: case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x00AD:
        return 0;
    case 0x00A1: seq = "!"; break;
    case 0x00A6: seq = "|"; break;
    case 0x00AB: case 0x2039: seq = "<"; break;
    case 0x00BB: case 0x203A: seq = ">"; break;
    case 0x00B1: seq = "+"; break;
    case 0x00B4: case 0x2032: seq = "'"; break;
    case 0x00A8: case 0x2033: case 0x201C: case 0x201D: case 0x201E: seq = "\""; break;
    case 0x00BF: seq = "?"; break;
    case 0x00C6: case 0x01FC: seq = "AE"; break;
    case 0x00E6: seq = "ae"; break;
    case 0x0152: seq = "OE"; break;
    case 0x0153: seq = "oe"; break;
    case 0x0131: seq = "i"; break;
    case 0x0141: seq = "L"; break;
    case 0x0142: seq = "l"; break;
    case 0x0192: seq = "f"; break;
    case 0x02C6: seq = "^"; break;
    case 0x02DC: case 0x223C: seq = "~"; break;
    case 0x201A: seq = ","; break;
    case 0x2044: case 0x2215: seq = "/"; break;
    case 0x2217: seq = "*"; break;
    case 0xFB00: seq = "ff"; break;
    case 0xFB01: seq = "fi"; break;
    case 0xFB02: seq = "fl"; break;
    case 0xFB03: seq = "ffi"; break;
    case 0xFB04: seq = "ffl"; break;
    case 0x0160: seq = "S"; *mark = 8; break;
    case 0x0161: seq = "s"; *mark = 8; break;
    case 0x017D: seq = "Z"; *mark = 8; break;
    case 0x017E: seq = "z"; *mark = 8; break;
    case 0x0178: seq = "Y"; *mark = 5; break;
    case 0x00B7: case 0x2022: case 0x2219: case 0x22C5: case 0x25CF: case 0x25CB: case 0x25E6:
        parts[0] = G_BULLET;
        return 1;
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: case 0x00AF:
        parts[0] = G_ENDASH;
        return 1;
    case 0x2014: case 0x2015:
        parts[0] = G_EMDASH;
        return 1;
    case 0x2018: case 0x201B:
        parts[0] = G_QLEFT;
        return 1;
    case 0x2019:
        parts[0] = G_QRIGHT;
        return 1;
    case 0x2026:
        parts[0] = G_ELLIPSIS;
        return 1;
    case 0x2713: case 0x2714: case 0x221A:
        parts[0] = G_CHECK;
        return 1;
    case 0x2192: case 0x21D2: case 0x25BA:
        parts[0] = G_ARROWR;
        return 1;
    case 0x2190: case 0x21D0: case 0x25C4:
        parts[0] = G_ARROWL;
        return 1;
    }
    if ( seq == NULL ) {
        if ( uni < 0x21 ) {
            return 0;
        }
        parts[0] = G_BOX;
        return 1;
    }
    for ( ; seq[count] && count < 3; count++ ) {
        parts[count] = seq[count] - 0x21;
    }
    return count;
}

typedef struct {
    fx scale_x;                 /* font units to a grid unit, across */
    fx shear;                   /* how far a unit of height leans */
    fx left;                    /* where the drawing starts, in font units */
} SLANT;

static void put( const SLANT *sl, int first, fx gx, fx gy )
{
    fx ux = sl->left + fx_mul( gx, sl->scale_x ) + fx_mul( gy * 50, sl->shear );
    fx uy = gy * 50;

    if ( first ) {
        gl_move( ux, uy );
    } else {
        gl_line( ux, uy );
    }
}

/* one drawing, its grid shifted by (dx, dy) */
static void draw( const SLANT *sl, int glyph, fx dx, fx dy )
{
    const s8 *cur = strokes + starts[glyph] + 1;
    fx px = 0, py = 0, cx, cy, ex, ey, sine, cosine;
    int first = 1, step;

    while ( *cur != EN ) {
        if ( *cur == MV ) {
            first = 1;
            cur++;
            continue;
        }
        if ( *cur == AR ) {
            cx = I2FX( cur[1] ) + dx;
            cy = I2FX( cur[2] ) + dy;
            ex = I2FX( cur[3] ) + dx;
            ey = I2FX( cur[4] ) + dy;
            for ( step = 1; step <= 6; step++ ) {
                sine = fx_sin( step * 15 );
                cosine = fx_cos( step * 15 );
                put( sl, 0, cx + fx_mul( px - cx, FX_ONE - sine ) + fx_mul( ex - cx, FX_ONE - cosine ),
                     cy + fx_mul( py - cy, FX_ONE - sine ) + fx_mul( ey - cy, FX_ONE - cosine ) );
            }
            px = ex;
            py = ey;
            cur += 5;
            continue;
        }
        px = I2FX( cur[0] ) + dx;
        py = I2FX( cur[1] ) + dy;
        put( sl, first, px, py );
        first = 0;
        cur += 2;
    }
}

/* The character, as a pen's path in thousandths of an em, drawn to be
   "advance" wide (0 for as wide as it is).  2 and the pen's width if
   there is something to stroke, 0 if the character is a space. */
int sfont_outline( u16 uni, int flags, int advance, fx *pen )
{
    SLANT sl;
    int parts[3], mark, count, index, natural = 0, upper;
    fx across;

    if ( !starts_made ) {
        starts_make();
    }
    count = pick( uni, parts, &mark );
    if ( count == 0 ) {
        return 0;
    }
    for ( index = 0; index < count; index++ ) {
        natural += strokes[starts[parts[index]]];
    }
    if ( natural == 0 ) {
        natural = 10;
    }
    /* the grid stretched or squeezed to the width asked for, within
       reason; what cannot be made up is left as space either side */
    sl.scale_x = 50 * FX_ONE;
    sl.left = 0;
    if ( advance > 0 ) {
        sl.scale_x = (fx)(((s32)advance << 16) / natural);
        if ( sl.scale_x > 70 * FX_ONE ) {
            sl.scale_x = 70 * FX_ONE;
        }
        if ( sl.scale_x < 30 * FX_ONE ) {
            sl.scale_x = 30 * FX_ONE;
        }
        sl.left = (I2FX( advance ) - natural * sl.scale_x) / 2;
    }
    sl.shear = (flags & FF_ITALIC) ? 13107L : 0;        /* a fifth */
    across = 0;
    for ( index = 0; index < count; index++ ) {
        draw( &sl, parts[index], across, 0 );
        if ( index == 0 && mark ) {
            /* over the middle of the letter; higher over a capital */
            upper = parts[0] + 0x21 >= 'A' && parts[0] + 0x21 <= 'Z';
            draw( &sl, G_GRAVE + mark - 1, I2FX( strokes[starts[parts[0]]] ) / 2,
                  mark == 7 ? 0 : (upper ? 4 * FX_ONE : 0) );
        }
        across += I2FX( strokes[starts[parts[index]]] );
    }
    *pen = (flags & FF_BOLD) ? 135 * FX_ONE : ((flags & FF_FIXED) ? 70 * FX_ONE : 82 * FX_ONE);
    return 2;
}

/* ------------------------------------------------------------------ */
/* the widths of the standard fonts                                    */
/* ------------------------------------------------------------------ */

/* ASCII 32 to 126, in thousandths of an em */
static const u16 helvetica[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015,
    667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667,
    611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333,
    556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500,
    278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584 };
static const u16 helvetica_bold[95] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611, 975,
    722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778, 667, 778, 722, 667,
    611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333,
    556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611, 611, 611, 389, 556,
    333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584 };
static const u16 times_roman[95] = {
    250, 333, 408, 500, 500, 833, 778, 180, 333, 333, 500, 564, 250, 333, 250, 278,
    500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 278, 278, 564, 564, 564, 444, 921,
    722, 667, 667, 722, 611, 556, 722, 722, 333, 389, 722, 611, 889, 722, 722, 556, 722, 667, 556,
    611, 722, 722, 944, 722, 722, 611, 333, 278, 333, 469, 500, 333,
    444, 500, 444, 500, 444, 333, 500, 500, 278, 278, 500, 278, 778, 500, 500, 500, 500, 333, 389,
    278, 500, 500, 722, 500, 500, 444, 480, 200, 480, 541 };
static const u16 times_bold[95] = {
    250, 333, 555, 500, 500, 1000, 833, 278, 333, 333, 500, 570, 250, 333, 250, 278,
    500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 333, 333, 570, 570, 570, 500, 930,
    722, 667, 722, 722, 667, 611, 778, 778, 389, 500, 778, 667, 944, 722, 778, 611, 778, 722, 556,
    667, 722, 722, 1000, 722, 722, 667, 333, 278, 333, 581, 500, 333,
    500, 556, 444, 556, 444, 333, 500, 556, 278, 333, 556, 278, 833, 556, 500, 556, 556, 444, 389,
    333, 556, 500, 722, 500, 500, 444, 394, 220, 394, 520 };
static const u16 times_italic[95] = {
    250, 333, 420, 500, 500, 833, 778, 214, 333, 333, 500, 675, 250, 333, 250, 278,
    500, 500, 500, 500, 500, 500, 500, 500, 500, 500, 333, 333, 675, 675, 675, 500, 920,
    611, 611, 667, 722, 611, 611, 722, 722, 333, 444, 667, 556, 833, 667, 722, 611, 722, 611, 500,
    556, 722, 611, 833, 611, 556, 556, 389, 278, 389, 422, 500, 333,
    500, 500, 444, 500, 444, 278, 500, 500, 278, 278, 444, 278, 722, 500, 500, 500, 500, 389, 389,
    278, 500, 444, 667, 444, 444, 389, 400, 275, 400, 541 };

/* how wide the standard font of these flags makes a character, for a
   file that leaves the widths to the reader */
int sfont_width( u16 uni, int flags )
{
    const u16 *table;
    int parts[3], mark, count, index, total = 0;

    if ( flags & FF_FIXED ) {
        return 600;
    }
    if ( flags & FF_SERIF ) {
        table = (flags & FF_BOLD) ? times_bold : ((flags & FF_ITALIC) ? times_italic : times_roman);
    } else {
        table = (flags & FF_BOLD) ? helvetica_bold : helvetica;
    }
    if ( uni == 0x00A0 ) {
        uni = 0x20;
    }
    if ( uni >= 0x20 && uni <= 0x7E ) {
        return table[uni - 0x20];
    }
    if ( uni >= 0xC0 && uni <= 0xFF && uni != 0xC6 && uni != 0xE6 ) {
        return table[latin_base[uni - 0xC0] - 0x20];
    }
    switch ( uni ) {
    case 0x2018: case 0x2019: case 0x201A:
        return table[','  - 0x20];
    case 0x201C: case 0x201D: case 0x201E:
        return table['"' - 0x20];
    case 0x2013:
        return 500;
    case 0x2014: case 0x2026:
        return 1000;
    case 0x2022:
        return 350;
    }
    if ( !starts_made ) {
        starts_make();
    }
    count = pick( uni, parts, &mark );
    for ( index = 0; index < count; index++ ) {
        total += parts[index] < 95 ? table[parts[index] + 1] : strokes[starts[parts[index]]] * 50;
    }
    return count ? total : table[0];
}
