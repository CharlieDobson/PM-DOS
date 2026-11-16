/*
 * CP.C - code pages, UTF-8 and UTF-16 (FMT.H).
 *
 * The document is Unicode.  Every old format is bytes in some code
 * page, and the text screen is one too, so text is turned one way when
 * a file is read and the other when it is written or shown.  The
 * tables are CPTAB.C's, a hundred and twenty-eight entries for the
 * upper half of each page; the lower half of every page here is ASCII.
 *
 * WHAT A CODE PAGE HAS NOT GOT is written as the nearest thing in
 * ASCII (uni_fallback) - a curly quote as a straight one, a letter
 * without its accent - before it is given up as a question mark.
 */
#include "fmt.h"

extern const u16 cp_tab_437[128], cp_tab_850[128], cp_tab_852[128], cp_tab_860[128],
                 cp_tab_863[128], cp_tab_865[128], cp_tab_866[128], cp_tab_1250[128],
                 cp_tab_1251[128], cp_tab_1252[128], cp_tab_1253[128], cp_tab_1254[128],
                 cp_tab_10000[128];

static const struct {
    int        page;
    const u16 *high;
} pages[] = {
    { CP_437, cp_tab_437 },   { CP_850, cp_tab_850 },   { CP_852, cp_tab_852 },
    { CP_860, cp_tab_860 },   { CP_863, cp_tab_863 },   { CP_865, cp_tab_865 },
    { CP_866, cp_tab_866 },   { CP_1250, cp_tab_1250 }, { CP_1251, cp_tab_1251 },
    { CP_1252, cp_tab_1252 }, { CP_1253, cp_tab_1253 }, { CP_1254, cp_tab_1254 },
    { CP_MAC, cp_tab_10000 }
};

#define NPAGES  (sizeof( pages ) / sizeof( pages[0] ))

static const u16 *high_of( int codepage )
{
    u32 index;

    for ( index = 0; index < NPAGES; index++ ) {
        if ( pages[index].page == codepage ) {
            return pages[index].high;
        }
    }
    return NULL;
}

int cp_known( int codepage )
{
    return codepage == CP_ASCII || codepage == CP_LATIN1 || codepage == CP_UTF8 ||
           high_of( codepage ) != NULL;
}

u16 cp_to_uni( int codepage, int byte )
{
    const u16 *high;

    byte &= 0xFF;
    if ( byte < 0x80 ) {
        return (u16)byte;
    }
    if ( codepage == CP_LATIN1 ) {
        return (u16)byte;
    }
    high = high_of( codepage );
    return high == NULL ? 0xFFFD : high[byte - 0x80];
}

int cp_from_uni( int codepage, u32 code )
{
    const u16 *high;
    int index;

    if ( code < 0x80 ) {
        return (int)code;
    }
    if ( codepage == CP_LATIN1 ) {
        return code < 0x100 ? (int)code : -1;
    }
    high = high_of( codepage );
    if ( high == NULL || code > 0xFFFF || code == 0xFFFD ) {
        return -1;
    }
    for ( index = 0; index < 128; index++ ) {
        if ( high[index] == code ) {
            return index + 0x80;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* the nearest thing in ASCII                                          */
/* ------------------------------------------------------------------ */

/* U+00C0 to U+00FF without their accents; a space where there is no
   letter under one */
static const char latin1_base[] =
    "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUY s" "aaaaaaaceeeeiiii" "dnooooo/ouuuuy y";

/* U+0100 to U+017F the same way */
static const char latina_base[] =
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiJjJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";

static const struct {
    u16         code;
    const char *text;
} nearest[] = {
    { 0x00A0, " " },   { 0x00A1, "!" },   { 0x00A6, "|" },   { 0x00A9, "(c)" },
    { 0x00AB, "<<" },  { 0x00AD, "-" },   { 0x00AE, "(R)" }, { 0x00B1, "+/-" },
    { 0x00B2, "2" },   { 0x00B3, "3" },   { 0x00B4, "'" },   { 0x00B7, "." },
    { 0x00B9, "1" },   { 0x00BB, ">>" },  { 0x00BC, "1/4" }, { 0x00BD, "1/2" },
    { 0x00BE, "3/4" }, { 0x00BF, "?" },   { 0x00C6, "AE" },  { 0x00DF, "ss" },
    { 0x00E6, "ae" },  { 0x0152, "OE" },  { 0x0153, "oe" },  { 0x0192, "f" },
    { 0x02C6, "^" },   { 0x02DC, "~" },   { 0x2002, " " },   { 0x2003, " " },
    { 0x2009, " " },   { 0x2010, "-" },   { 0x2011, "-" },   { 0x2012, "-" },
    { 0x2013, "-" },   { 0x2014, "--" },  { 0x2015, "--" },  { 0x2018, "'" },
    { 0x2019, "'" },   { 0x201A, "," },   { 0x201C, "\"" },  { 0x201D, "\"" },
    { 0x201E, "\"" },  { 0x2020, "+" },   { 0x2022, "*" },   { 0x2026, "..." },
    { 0x2030, "o/oo" }, { 0x2032, "'" },  { 0x2033, "\"" },  { 0x2039, "<" },
    { 0x203A, ">" },   { 0x2044, "/" },   { 0x20AC, "EUR" }, { 0x2122, "(TM)" },
    { 0x2190, "<-" },  { 0x2192, "->" },  { 0x2212, "-" },   { 0x2215, "/" },
    { 0x2219, "." },   { 0x2260, "<>" },  { 0x2264, "<=" },  { 0x2265, ">=" },
    { 0x25A0, "*" },   { 0x25CF, "*" },   { 0xFB01, "fi" },  { 0xFB02, "fl" }
};

int uni_fallback( u32 code, char *out )
{
    u32 index;
    char base;

    out[0] = 0;
    for ( index = 0; index < sizeof( nearest ) / sizeof( nearest[0] ); index++ ) {
        if ( nearest[index].code == code ) {
            str_cpy( out, nearest[index].text );
            return (int)str_len( out );
        }
    }
    base = ' ';
    if ( code >= 0xC0 && code <= 0xFF ) {
        base = latin1_base[code - 0xC0];
    } else if ( code >= 0x100 && code <= 0x17F ) {
        base = latina_base[code - 0x100];
    }
    if ( base == ' ' ) {
        return 0;
    }
    out[0] = base;
    out[1] = 0;
    return 1;
}

int cp_put( int codepage, u32 code, char *out )
{
    int byte = cp_from_uni( codepage, code );
    int len;

    if ( byte >= 0 ) {
        out[0] = (char)byte;
        return 1;
    }
    len = uni_fallback( code, out );
    if ( len == 0 ) {
        out[0] = '?';
        len = 1;
    }
    return len;
}

/* ------------------------------------------------------------------ */
/* UTF-8 and UTF-16                                                    */
/* ------------------------------------------------------------------ */

u32 utf8_get( const u8 **cur, const u8 *end )
{
    const u8 *walk = *cur;
    u32 code, least;
    int more;

    if ( walk >= end ) {
        return 0xFFFD;
    }
    code = *walk++;
    if ( code < 0x80 ) {
        *cur = walk;
        return code;
    }
    if ( code >= 0xC2 && code <= 0xDF ) {
        more = 1;
        code &= 0x1F;
        least = 0x80;
    } else if ( code >= 0xE0 && code <= 0xEF ) {
        more = 2;
        code &= 0x0F;
        least = 0x800;
    } else if ( code >= 0xF0 && code <= 0xF4 ) {
        more = 3;
        code &= 0x07;
        least = 0x10000UL;
    } else {
        *cur = walk;
        return 0xFFFD;
    }
    while ( more-- ) {
        if ( walk >= end || (*walk & 0xC0) != 0x80 ) {
            *cur = walk;
            return 0xFFFD;
        }
        code = (code << 6) | (u32)(*walk++ & 0x3F);
    }
    *cur = walk;
    if ( code < least || code > 0x10FFFFUL || (code >= 0xD800 && code <= 0xDFFF) ) {
        return 0xFFFD;
    }
    return code;
}

int utf8_put( u8 *out, u32 code )
{
    if ( code < 0x80 ) {
        out[0] = (u8)code;
        return 1;
    }
    if ( code < 0x800 ) {
        out[0] = (u8)(0xC0 | (code >> 6));
        out[1] = (u8)(0x80 | (code & 0x3F));
        return 2;
    }
    if ( code < 0x10000UL ) {
        out[0] = (u8)(0xE0 | (code >> 12));
        out[1] = (u8)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (code & 0x3F));
        return 3;
    }
    if ( code > 0x10FFFFUL ) {
        code = 0xFFFD;
        return utf8_put( out, code );
    }
    out[0] = (u8)(0xF0 | (code >> 18));
    out[1] = (u8)(0x80 | ((code >> 12) & 0x3F));
    out[2] = (u8)(0x80 | ((code >> 6) & 0x3F));
    out[3] = (u8)(0x80 | (code & 0x3F));
    return 4;
}

u32 utf16_get( const u16 *text, u32 len, u32 *pos )
{
    u32 code = text[(*pos)++];
    u32 low;

    if ( code >= 0xD800 && code <= 0xDBFF && *pos < len ) {
        low = text[*pos];
        if ( low >= 0xDC00 && low <= 0xDFFF ) {
            (*pos)++;
            return 0x10000UL + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
    }
    if ( code >= 0xD800 && code <= 0xDFFF ) {
        return 0xFFFD;                  /* half a pair on its own */
    }
    return code;
}

void out_utf8( OUT *out, const u16 *text, u32 len )
{
    u8 buf[256];
    u32 pos = 0, used = 0;

    while ( pos < len ) {
        if ( used > sizeof( buf ) - 4 ) {
            out_put( out, buf, used );
            used = 0;
        }
        used += (u32)utf8_put( buf + used, utf16_get( text, len, &pos ) );
    }
    out_put( out, buf, used );
}

void utf8_copy( char *dst, u32 max, const u16 *text, u32 len )
{
    u8 one[4];
    u32 pos = 0, used = 0;
    int count;

    if ( max == 0 ) {
        return;
    }
    while ( pos < len ) {
        count = utf8_put( one, utf16_get( text, len, &pos ) );
        if ( used + (u32)count >= max ) {
            break;
        }
        mem_cpy( dst + used, one, (u32)count );
        used += (u32)count;
    }
    dst[used] = 0;
}
