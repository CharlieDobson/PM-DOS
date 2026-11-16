/*
 * TAG.C - titles and names from a file's tags, made into code page 437
 * for the screen (DEC.H).
 *
 * TAGS COME IN THREE ENCODINGS.  ID3 version 1 and many version 2
 * frames are Latin-1, newer ID3 frames and Vorbis comments are UTF-8,
 * and ID3's other choice and every WMA field are UTF-16.  The screen
 * has code page 437, which holds the accented letters of the western
 * European languages and little else.
 *
 * A LETTER 437 LACKS LOSES ITS ACCENT before it becomes a question
 * mark: an A with a tilde shows as A, which reads better in a title
 * than a row of question marks.  The curly quotation marks and the
 * dashes a word processor makes become their plain forms.
 *
 * WHAT ENDS A TAG: its length, a 0, or the room in the destination.
 * Control characters are dropped, and so are blanks at the end - ID3
 * version 1 pads with them.
 */
#include "rt.h"
#include "dec.h"

/* what code page 437 has from 80h to FFh */
static const u16 cp437_high[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

/* Latin-1's letters from C0h to FFh with the accent taken off, for the
   ones 437 does not have ('?' where there is no plain letter) */
static const char latin1_plain[65] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuypy";

int tag_437( u32 uni )
{
    int i;

    if ( uni >= 0x20 && uni < 0x7F ) {
        return (int)uni;
    }
    for ( i = 0; i < 128; i++ ) {
        if ( cp437_high[i] == uni ) {
            return 0x80 + i;
        }
    }
    if ( uni >= 0xC0 && uni <= 0xFF ) {
        return latin1_plain[uni - 0xC0];
    }
    switch ( uni ) {
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x2032:
        return '\'';
    case 0x201C:
    case 0x201D:
    case 0x201E:
        return '"';
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2015:
        return '-';
    case 0x2026:
        return '.';
    }
    return '?';
}

/* One character onto the end, if there is room for it and the 0 */
static void put_char( char *dst, u32 max, u32 *len, u32 uni )
{
    if ( uni < 0x20 || uni == 0x7F || (uni >= 0x80 && uni < 0xA0) ) {
        return;
    }
    if ( *len + 1 < max ) {
        dst[(*len)++] = (char)tag_437( uni );
    }
}

/* the 0, with the blanks before it taken off */
static void finish( char *dst, u32 len )
{
    while ( len > 0 && (dst[len - 1] == ' ' || (u8)dst[len - 1] == 0xFF) ) {
        len--;
    }
    dst[len] = 0;
}

void tag_latin1( char *dst, u32 max, const u8 *src, u32 len )
{
    u32 i, out = 0;

    if ( max == 0 ) {
        return;
    }
    for ( i = 0; i < len && src[i] != 0; i++ ) {
        put_char( dst, max, &out, src[i] );
    }
    finish( dst, out );
}

void tag_utf8( char *dst, u32 max, const u8 *src, u32 len )
{
    u32 i = 0, out = 0, uni, need, k;

    if ( max == 0 ) {
        return;
    }
    while ( i < len && src[i] != 0 ) {
        if ( src[i] < 0x80 ) {
            put_char( dst, max, &out, src[i++] );
            continue;
        }
        if ( (src[i] & 0xE0) == 0xC0 ) {
            uni = src[i] & 0x1F;
            need = 1;
        } else if ( (src[i] & 0xF0) == 0xE0 ) {
            uni = src[i] & 0x0F;
            need = 2;
        } else if ( (src[i] & 0xF8) == 0xF0 ) {
            uni = src[i] & 0x07;
            need = 3;
        } else {
            put_char( dst, max, &out, '?' );     /* a byte that starts nothing */
            i++;
            continue;
        }
        if ( need > len - i - 1 ) {
            put_char( dst, max, &out, '?' );     /* cut off */
            break;
        }
        for ( k = 1; k <= need; k++ ) {
            if ( (src[i + k] & 0xC0) != 0x80 ) {
                break;
            }
            uni = (uni << 6) | (src[i + k] & 0x3F);
        }
        if ( k <= need ) {
            put_char( dst, max, &out, '?' );     /* broken: go on from the byte that broke it */
            i += k;
            continue;
        }
        put_char( dst, max, &out, uni );
        i += need + 1;
    }
    finish( dst, out );
}

void tag_utf16( char *dst, u32 max, const u8 *src, u32 len, int big_endian )
{
    u32 i, out = 0, unit;

    if ( max == 0 ) {
        return;
    }
    for ( i = 0; i + 1 < len; i += 2 ) {
        unit = big_endian ? BE16( src + i ) : LE16( src + i );
        if ( unit == 0 ) {
            break;
        }
        if ( unit == 0xFEFF || unit == 0xFFFE ) {
            continue;                           /* a byte-order mark */
        }
        if ( unit >= 0xD800 && unit <= 0xDFFF ) {
            if ( unit < 0xDC00 ) {
                put_char( dst, max, &out, '?' );    /* a character past the first 64K */
            }
            continue;
        }
        put_char( dst, max, &out, unit );
    }
    finish( dst, out );
}
