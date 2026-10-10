/*
 * NAMES.C - what a character is called, in each of the places that
 * call it something: a glyph's name in a font, its Unicode number, its
 * code in the encodings PDF defines, and the character on a PC's text
 * screen that will do for it.
 *
 * The first 229 names are in the order of the Compact Font Format's
 * standard strings, so that a CFF font's string number below 229 is an
 * index into them; the rest are the names a /Differences array is
 * likely to use that are not among those.
 */
#include "gfx.h"

/* names 0 to 228: CFF's standard strings (the ISO Adobe character set) */
static char std_text[] =
    ".notdef space exclam quotedbl numbersign dollar percent ampersand quoteright "
    "parenleft parenright asterisk plus comma hyphen period slash zero one two three "
    "four five six seven eight nine colon semicolon less equal greater question at "
    "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z bracketleft backslash "
    "bracketright asciicircum underscore quoteleft a b c d e f g h i j k l m n o p q "
    "r s t u v w x y z braceleft bar braceright asciitilde exclamdown cent sterling "
    "fraction yen florin section currency quotesingle quotedblleft guillemotleft "
    "guilsinglleft guilsinglright fi fl endash dagger daggerdbl periodcentered "
    "paragraph bullet quotesinglbase quotedblbase quotedblright guillemotright "
    "ellipsis perthousand questiondown grave acute circumflex tilde macron breve "
    "dotaccent dieresis ring cedilla hungarumlaut ogonek caron emdash AE ordfeminine "
    "Lslash Oslash OE ordmasculine ae dotlessi lslash oslash oe germandbls "
    "onesuperior logicalnot mu trademark Eth onehalf plusminus Thorn onequarter "
    "divide brokenbar degree thorn threequarters twosuperior registered minus eth "
    "multiply threesuperior copyright Aacute Acircumflex Adieresis Agrave Aring "
    "Atilde Ccedilla Eacute Ecircumflex Edieresis Egrave Iacute Icircumflex "
    "Idieresis Igrave Ntilde Oacute Ocircumflex Odieresis Ograve Otilde Scaron "
    "Uacute Ucircumflex Udieresis Ugrave Yacute Ydieresis Zcaron aacute acircumflex "
    "adieresis agrave aring atilde ccedilla eacute ecircumflex edieresis egrave "
    "iacute icircumflex idieresis igrave ntilde oacute ocircumflex odieresis ograve "
    "otilde scaron uacute ucircumflex udieresis ugrave yacute ydieresis zcaron "
    /* and the others */
    "Euro notequal infinity lessequal greaterequal partialdiff summation product pi "
    "integral Omega radical approxequal Delta lozenge nbspace sfthyphen ff ffi ffl "
    "Abreve abreve Aogonek aogonek Cacute cacute Ccaron ccaron Dcaron dcaron Dcroat "
    "dcroat Eogonek eogonek Ecaron ecaron Gbreve gbreve Idotaccent Lacute lacute "
    "Lcaron lcaron Nacute nacute Ncaron ncaron Ohungarumlaut ohungarumlaut Racute "
    "racute Rcaron rcaron Sacute sacute Scedilla scedilla Tcaron tcaron Uring uring "
    "Uhungarumlaut uhungarumlaut Zacute zacute Zdotaccent zdotaccent Amacron amacron "
    "Emacron emacron Imacron imacron Omacron omacron Umacron umacron Alpha Beta Gamma "
    "Epsilon Zeta Eta Theta Iota Kappa Lambda Mu Nu Xi Omicron Pi Rho Sigma Tau "
    "Upsilon Phi Chi Psi alpha beta gamma delta epsilon zeta eta theta iota kappa "
    "lambda nu xi omicron rho sigma tau upsilon phi chi psi omega arrowleft arrowup "
    "arrowright arrowdown element proportional equivalence intersection union minute "
    "second dotmath angle gradient therefore similar congruent logicaland logicalor "
    "emptyset heart club diamond spade filledbox circle openbullet quotereversed "
    "apple sigma1 theta1 phi1 omega1 Upsilon1 asteriskmath existential universal "
    "suchthat perpendicular arrowboth notelement propersubset propersuperset "
    "reflexsubset reflexsuperset notsubset aleph Ifraktur Rfraktur weierstrass "
    "circlemultiply circleplus arrowdblboth arrowdblleft arrowdblup arrowdblright "
    "arrowdbldown angleleft angleright";

#define STD_COUNT   229

static const u16 name_uni[] = {
    0x0000, 0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x2019,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033,
    0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040,
    0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D,
    0x004E, 0x004F, 0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A,
    0x005B, 0x005C,
    0x005D, 0x005E, 0x005F, 0x2018, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069,
    0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071,
    0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E,
    0x00A1, 0x00A2, 0x00A3,
    0x2044, 0x00A5, 0x0192, 0x00A7, 0x00A4, 0x0027, 0x201C, 0x00AB,
    0x2039, 0x203A, 0xFB01, 0xFB02, 0x2013, 0x2020, 0x2021, 0x00B7,
    0x00B6, 0x2022, 0x201A, 0x201E, 0x201D, 0x00BB,
    0x2026, 0x2030, 0x00BF, 0x0060, 0x00B4, 0x02C6, 0x02DC, 0x00AF, 0x02D8,
    0x02D9, 0x00A8, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7, 0x2014, 0x00C6, 0x00AA,
    0x0141, 0x00D8, 0x0152, 0x00BA, 0x00E6, 0x0131, 0x0142, 0x00F8, 0x0153, 0x00DF,
    0x00B9, 0x00AC, 0x00B5, 0x2122, 0x00D0, 0x00BD, 0x00B1, 0x00DE, 0x00BC,
    0x00F7, 0x00A6, 0x00B0, 0x00FE, 0x00BE, 0x00B2, 0x00AE, 0x2212, 0x00F0,
    0x00D7, 0x00B3, 0x00A9, 0x00C1, 0x00C2, 0x00C4, 0x00C0, 0x00C5,
    0x00C3, 0x00C7, 0x00C9, 0x00CA, 0x00CB, 0x00C8, 0x00CD, 0x00CE,
    0x00CF, 0x00CC, 0x00D1, 0x00D3, 0x00D4, 0x00D6, 0x00D2, 0x00D5, 0x0160,
    0x00DA, 0x00DB, 0x00DC, 0x00D9, 0x00DD, 0x0178, 0x017D, 0x00E1, 0x00E2,
    0x00E4, 0x00E0, 0x00E5, 0x00E3, 0x00E7, 0x00E9, 0x00EA, 0x00EB, 0x00E8,
    0x00ED, 0x00EE, 0x00EF, 0x00EC, 0x00F1, 0x00F3, 0x00F4, 0x00F6, 0x00F2,
    0x00F5, 0x0161, 0x00FA, 0x00FB, 0x00FC, 0x00F9, 0x00FD, 0x00FF, 0x017E,
    /* and the others */
    0x20AC, 0x2260, 0x221E, 0x2264, 0x2265, 0x2202, 0x2211, 0x220F, 0x03C0,
    0x222B, 0x03A9, 0x221A, 0x2248, 0x0394, 0x25CA, 0x00A0, 0x00AD, 0xFB00, 0xFB03, 0xFB04,
    0x0102, 0x0103, 0x0104, 0x0105, 0x0106, 0x0107, 0x010C, 0x010D, 0x010E, 0x010F, 0x0110,
    0x0111, 0x0118, 0x0119, 0x011A, 0x011B, 0x011E, 0x011F, 0x0130, 0x0139, 0x013A,
    0x013D, 0x013E, 0x0143, 0x0144, 0x0147, 0x0148, 0x0150, 0x0151, 0x0154,
    0x0155, 0x0158, 0x0159, 0x015A, 0x015B, 0x015E, 0x015F, 0x0164, 0x0165, 0x016E, 0x016F,
    0x0170, 0x0171, 0x0179, 0x017A, 0x017B, 0x017C, 0x0100, 0x0101,
    0x0112, 0x0113, 0x012A, 0x012B, 0x014C, 0x014D, 0x016A, 0x016B, 0x0391, 0x0392, 0x0393,
    0x0395, 0x0396, 0x0397, 0x0398, 0x0399, 0x039A, 0x039B, 0x039C, 0x039D, 0x039E, 0x039F, 0x03A0, 0x03A1,
    0x03A3, 0x03A4,
    0x03A5, 0x03A6, 0x03A7, 0x03A8, 0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7, 0x03B8, 0x03B9,
    0x03BA,
    0x03BB, 0x03BD, 0x03BE, 0x03BF, 0x03C1, 0x03C3, 0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8, 0x03C9, 0x2190,
    0x2191,
    0x2192, 0x2193, 0x2208, 0x221D, 0x2261, 0x2229, 0x222A, 0x2032,
    0x2033, 0x22C5, 0x2220, 0x2207, 0x2234, 0x223C, 0x2245, 0x2227, 0x2228,
    0x2205, 0x2665, 0x2663, 0x2666, 0x2660, 0x25A0, 0x25CB, 0x25E6, 0x201B,
    0xF8FF, 0x03C2, 0x03D1, 0x03D5, 0x03D6, 0x03D2, 0x2217, 0x2203, 0x2200,
    0x220B, 0x22A5, 0x2194, 0x2209, 0x2282, 0x2283,
    0x2286, 0x2287, 0x2284, 0x2135, 0x2111, 0x211C, 0x2118,
    0x2297, 0x2295, 0x21D4, 0x21D0, 0x21D1, 0x21D2,
    0x21D3, 0x2329, 0x232A
};

#define NAME_COUNT  (sizeof( name_uni ) / sizeof( name_uni[0] ))

static const char *names[NAME_COUNT];
static int names_made;

static void names_make( void )
{
    char *cur = std_text;
    u32 index;

    for ( index = 0; index < NAME_COUNT; index++ ) {
        names[index] = cur;
        while ( *cur && *cur != ' ' ) {
            cur++;
        }
        if ( *cur == 0 ) {
            index++;
            break;
        }
        *cur++ = 0;
    }
    for ( ; index < NAME_COUNT; index++ ) {
        names[index] = "";              /* the lists disagree: not reached */
    }
    names_made = 1;
}

const char *name_std( int sid )
{
    if ( !names_made ) {
        names_make();
    }
    return sid >= 0 && sid < STD_COUNT ? names[sid] : NULL;
}

static int hex4( const char *text, int digits, u32 *val )
{
    int index, ch;

    *val = 0;
    for ( index = 0; index < digits; index++ ) {
        ch = text[index];
        if ( ch >= '0' && ch <= '9' ) {
            ch -= '0';
        } else if ( ch >= 'A' && ch <= 'F' ) {
            ch -= 'A' - 10;
        } else {
            return 0;
        }
        *val = *val * 16 + (u32)ch;
    }
    return 1;
}

/* a glyph's name to Unicode: 0 if nothing is known of it */
u16 name_to_uni( const char *name )
{
    char stem[40];
    const char *dot;
    u32 index, val, len;

    if ( !names_made ) {
        names_make();
    }
    if ( name == NULL || name[0] == 0 ) {
        return 0;
    }
    for ( index = 1; index < NAME_COUNT; index++ ) {
        if ( names[index][0] == name[0] && str_cmp( names[index], name ) == 0 ) {
            return name_uni[index];
        }
    }
    len = str_len( name );
    if ( name[0] == 'u' && name[1] == 'n' && name[2] == 'i' && len >= 7 && hex4( name + 3, 4, &val ) ) {
        return (u16)val;
    }
    if ( name[0] == 'u' && len >= 5 && len <= 7 && hex4( name + 1, (int)len - 1, &val ) && val < 0x10000UL ) {
        return (u16)val;
    }
    /* "a.sc", "one.oldstyle": the plain character */
    dot = str_chr( name, '.' );
    if ( dot && dot != name && (u32)(dot - name) < sizeof( stem ) ) {
        mem_cpy( stem, name, (u32)(dot - name) );
        stem[dot - name] = 0;
        return name_to_uni( stem );
    }
    return 0;
}

const char *uni_to_name( u16 uni )
{
    u32 index;

    if ( !names_made ) {
        names_make();
    }
    if ( uni == 0x00A0 ) {
        uni = 0x0020;
    } else if ( uni == 0x00AD ) {
        uni = 0x002D;
    }
    for ( index = 1; index < NAME_COUNT; index++ ) {
        if ( name_uni[index] == uni ) {
            return names[index];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* the encodings                                                       */
/* ------------------------------------------------------------------ */

/* Windows code page 1252, 80h to 9Fh */
static const u16 win_high[32] = {
    0x20AC, 0x2022, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
    0x0152, 0x2022, 0x017D, 0x2022, 0x2022, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x2022, 0x017E, 0x0178 };

/* Mac OS Roman, 80h to FFh, as PDF has it */
static const u16 mac_high[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3,
    0x00E5, 0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3,
    0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA,
    0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x0394, 0x00AB,
    0x00BB, 0x2026, 0x0020, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
    0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x00A4, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE,
    0x00CF, 0x00CC, 0x00D3, 0x00D4, 0x0000, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7 };

/* Adobe's standard encoding: the code of each of the names 1 to 149 */
static const u8 std_codes[149] = {
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
    55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77,
    78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100,
    101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118,
    119, 120, 121, 122, 123, 124, 125, 126,
    161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    177, 178, 179, 180, 182, 183, 184, 185, 186, 187, 188, 189, 191,
    193, 194, 195, 196, 197, 198, 199, 200, 202, 203, 205, 206, 207, 208,
    225, 227, 232, 233, 234, 235, 241, 245, 248, 249, 250, 251 };

/* the Symbol font's own, 20h to 7Eh and A0h to FEh */
static const u16 symbol_low[95] = {
    0x0020, 0x0021, 0x2200, 0x0023, 0x2203, 0x0025, 0x0026, 0x220B, 0x0028, 0x0029, 0x2217, 0x002B,
    0x002C, 0x2212, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x2245, 0x0391, 0x0392, 0x03A7,
    0x0394, 0x0395, 0x03A6, 0x0393, 0x0397, 0x0399, 0x03D1, 0x039A, 0x039B, 0x039C, 0x039D, 0x039F,
    0x03A0, 0x0398, 0x03A1, 0x03A3, 0x03A4, 0x03A5, 0x03C2, 0x03A9, 0x039E, 0x03A8, 0x0396, 0x005B,
    0x2234, 0x005D, 0x22A5, 0x005F, 0x203E, 0x03B1, 0x03B2, 0x03C7, 0x03B4, 0x03B5, 0x03C6, 0x03B3,
    0x03B7, 0x03B9, 0x03D5, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BF, 0x03C0, 0x03B8, 0x03C1, 0x03C3,
    0x03C4, 0x03C5, 0x03D6, 0x03C9, 0x03BE, 0x03C8, 0x03B6, 0x007B, 0x007C, 0x007D, 0x223C };
static const u16 symbol_high[95] = {
    0x20AC, 0x03D2, 0x2032, 0x2264, 0x2044, 0x221E, 0x0192, 0x2663, 0x2666, 0x2665, 0x2660, 0x2194,
    0x2190, 0x2191, 0x2192, 0x2193, 0x00B0, 0x00B1, 0x2033, 0x2265, 0x00D7, 0x221D, 0x2202, 0x2022,
    0x00F7, 0x2260, 0x2261, 0x2248, 0x2026, 0x007C, 0x2014, 0x21B5, 0x2135, 0x2111, 0x211C, 0x2118,
    0x2297, 0x2295, 0x2205, 0x2229, 0x222A, 0x2283, 0x2287, 0x2284, 0x2282, 0x2286, 0x2208, 0x2209,
    0x2220, 0x2207, 0x00AE, 0x00A9, 0x2122, 0x220F, 0x221A, 0x22C5, 0x00AC, 0x2227, 0x2228, 0x21D4,
    0x21D0, 0x21D1, 0x21D2, 0x21D3, 0x25CA, 0x2329, 0x00AE, 0x00A9, 0x2122, 0x2211, 0x0028, 0x007C,
    0x0028, 0x005B, 0x007C, 0x005B, 0x007B, 0x007B, 0x007B, 0x007C, 0x0000, 0x232A, 0x222B, 0x2320,
    0x007C, 0x2321, 0x0029, 0x007C, 0x0029, 0x005D, 0x007C, 0x005D, 0x007D, 0x007D, 0x007D };

void enc_base( int which, u16 *uni )
{
    int code;

    mem_set( uni, 0, 256 * sizeof( u16 ) );
    switch ( which ) {
    case ENC_STD:
        for ( code = 0; code < 149; code++ ) {
            uni[std_codes[code]] = name_uni[code + 1];
        }
        break;
    case ENC_MAC:
        for ( code = 32; code < 127; code++ ) {
            uni[code] = (u16)code;
        }
        mem_cpy( uni + 128, mac_high, sizeof( mac_high ) );
        break;
    case ENC_SYMBOL:
        mem_cpy( uni + 32, symbol_low, sizeof( symbol_low ) );
        mem_cpy( uni + 160, symbol_high, sizeof( symbol_high ) );
        break;
    default:
        for ( code = 32; code < 256; code++ ) {
            uni[code] = (u16)code;
        }
        uni[127] = 0x2022;
        mem_cpy( uni + 128, win_high, sizeof( win_high ) );
        uni[160] = 0x0020;
        uni[173] = 0x002D;
        break;
    }
}

/* the code Adobe's standard encoding gives a character, for the accents
   a Type 1 "seac" builds a letter from: -1 if it has none */
const char *enc_std_name( int code )
{
    int index;

    if ( !names_made ) {
        names_make();
    }
    for ( index = 0; index < 149; index++ ) {
        if ( std_codes[index] == code ) {
            return names[index + 1];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* the text screen                                                     */
/* ------------------------------------------------------------------ */

/* Unicode A0h to FFh in code page 437: a character, or 0 for one that
   takes more than one, or a letter without its accent */
static const u8 latin1_437[96] = {
    ' ',  0xAD, 0x9B, 0x9C, '*',  0x9D, '|',  0x15, '"',  0,    0xA6, 0xAE, 0xAA, '-',  0,    '-',
    0xF8, 0xF1, 0xFD, '3',  '\'', 0xE6, 0x14, 0xFA, ',',  '1',  0xA7, 0xAF, 0xAC, 0xAB, 0,    0xA8,
    'A',  'A',  'A',  'A',  0x8E, 0x8F, 0x92, 0x80, 'E',  0x90, 'E',  'E',  'I',  'I',  'I',  'I',
    'D',  0xA5, 'O',  'O',  'O',  'O',  0x99, 'x',  'O',  'U',  'U',  'U',  0x9A, 'Y',  'T',  0xE1,
    0x85, 0xA0, 0x83, 'a',  0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
    'd',  0xA4, 0x95, 0xA2, 0x93, 'o',  0x94, 0xF6, 'o',  0x97, 0xA3, 0x96, 0x81, 'y',  't',  0x98 };

typedef struct {
    u16         uni;
    const char *text;
} UNITEXT;

static const UNITEXT others[] = {
    { 0x00A9, "(c)" }, { 0x00AE, "(R)" }, { 0x00BE, "3/4" }, { 0x0131, "i" }, { 0x0141, "L" },
    { 0x0142, "l" }, { 0x0152, "OE" }, { 0x0153, "oe" }, { 0x0192, "\x9F" }, { 0x02C6, "^" },
    { 0x02C7, "v" }, { 0x02DC, "~" }, { 0x0393, "\xE2" }, { 0x0394, "\x7F" }, { 0x0398, "\xE9" },
    { 0x03A3, "\xE4" }, { 0x03A6, "\xE8" }, { 0x03A9, "\xEA" }, { 0x03B1, "\xE0" }, { 0x03B2, "\xE1" },
    { 0x03B4, "\xEB" }, { 0x03B5, "\xEE" }, { 0x03BC, "\xE6" }, { 0x03C0, "\xE3" }, { 0x03C3, "\xE5" },
    { 0x03C4, "\xE7" }, { 0x03C6, "\xED" }, { 0x2010, "-" }, { 0x2011, "-" }, { 0x2012, "-" },
    { 0x2013, "-" }, { 0x2014, "--" }, { 0x2015, "--" }, { 0x2018, "'" }, { 0x2019, "'" },
    { 0x201A, "," }, { 0x201B, "'" }, { 0x201C, "\"" }, { 0x201D, "\"" }, { 0x201E, "\"" },
    { 0x2020, "+" }, { 0x2021, "#" }, { 0x2022, "\xF9" }, { 0x2026, "..." }, { 0x2030, "o/oo" },
    { 0x2032, "'" }, { 0x2033, "\"" }, { 0x2039, "<" }, { 0x203A, ">" }, { 0x2044, "/" },
    { 0x207F, "\xFC" }, { 0x20AC, "EUR" }, { 0x2122, "(TM)" }, { 0x2190, "<-" }, { 0x2192, "->" },
    { 0x2212, "-" }, { 0x2215, "/" }, { 0x2217, "*" }, { 0x2219, "\xF9" }, { 0x221A, "\xFB" },
    { 0x221E, "\xEC" }, { 0x2229, "\xEF" }, { 0x2248, "\xF7" }, { 0x2260, "!=" }, { 0x2261, "\xF0" },
    { 0x2264, "\xF3" }, { 0x2265, "\xF2" }, { 0x2320, "\xF4" }, { 0x2321, "\xF5" }, { 0x22C5, "\xFA" },
    { 0x2500, "\xC4" }, { 0x2502, "\xB3" }, { 0x25A0, "\xFE" }, { 0x25AA, "\xFE" }, { 0x25CF, "\xF9" },
    { 0x25CB, "o" }, { 0x25E6, "o" }, { 0x2666, "\x04" }, { 0xFB00, "ff" }, { 0xFB01, "fi" },
    { 0xFB02, "fl" }, { 0xFB03, "ffi" }, { 0xFB04, "ffl" }, { 0xFFFD, "?" }
};

/* A Unicode character as the text screen can show it: the characters
   into "out" (four at most), and how many.  None for a character that
   is no character - a soft hyphen's cousins, a private one. */
int uni_to_cp437( u16 uni, char *out )
{
    static const char plain[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkk";
    u32 index;
    int count = 0, base;

    if ( uni < 0x20 ) {
        if ( uni == 9 ) {
            out[count++] = ' ';
        }
        return count;
    }
    if ( uni < 0x7F ) {
        out[0] = (char)uni;
        return 1;
    }
    if ( uni >= 0xA0 && uni <= 0xFF && latin1_437[uni - 0xA0] ) {
        out[0] = (char)latin1_437[uni - 0xA0];
        return 1;
    }
    for ( index = 0; index < sizeof( others ) / sizeof( others[0] ); index++ ) {
        if ( others[index].uni == uni ) {
            for ( count = 0; others[index].text[count]; count++ ) {
                out[count] = others[index].text[count];
            }
            return count;
        }
    }
    if ( uni >= 0x0100 && uni <= 0x017F ) {
        /* a Latin letter with something on it: the letter */
        if ( uni <= 0x0138 ) {
            out[0] = plain[uni - 0x0100];
            return 1;
        }
        base = uni <= 0x0149 ? "LlLlLlLlLlNnNnNnn"[uni - 0x0139]
             : uni <= 0x0177 ? "NnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYy"[uni - 0x014A]
             : "YZzZzZzs"[uni - 0x0178];
        out[0] = (char)base;
        return 1;
    }
    if ( uni >= 0x2000 && uni <= 0x200A ) {
        out[0] = ' ';
        return 1;
    }
    if ( uni >= 0xE000 && uni <= 0xF8FF ) {
        return 0;
    }
    if ( (uni >= 0x200B && uni <= 0x200F) || uni == 0xFEFF || uni == 0x00AD ) {
        return 0;
    }
    out[0] = '?';
    return 1;
}
