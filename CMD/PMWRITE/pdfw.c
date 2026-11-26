/*
 * PDFW.C - a document written as a PDF file.
 *
 * THE PAGES are LAY.C's: the same lines, broken at the same places, as
 * a graphics screen shows, each piece of text put at its own place.
 * The type of a built-in face is the fourteen fonts every PDF reader
 * has had since the first one - Times, Helvetica, Courier and Symbol -
 * so nothing is embedded for it, and the widths the layout measured
 * with are those fonts' own (TF.H).  Text is written in the Windows
 * character set, which is what those fonts are encoded with; a
 * character it has not got is written as the nearest thing (CP.C).
 * The type of a face that is a TrueType file is that file, cut down to
 * the glyphs used and put in the PDF (EFONT, below).
 *
 * THE VERSION THE FILE SAYS IT IS decides what may be in it, and this
 * keeps to that: a 1.0 or 1.1 file has no compressed streams (Flate
 * came with 1.2); from 1.4 the catalogue points at XMP metadata; a 2.0
 * file's Info dictionary has only its dates, as 2.0 asks.  Everything
 * else here was already in 1.0.  The cross-reference is the table
 * form, which every version reads.
 *
 * PICTURES.  A JPEG goes in as the file it is.  A PNG or a BMP goes in
 * as its pixels (IMG.C); a picture of a kind that cannot be decoded is
 * an empty frame of its size.
 *
 * WHAT IS LEFT OUT.  Footnotes are set after the text, as endnotes.
 * Paragraph borders are not drawn.
 */
#include "fmt.h"
#include "lay.h"
#include "tf.h"
#include "flate.h"
#include "img.h"
#include "crc.h"
#include "sys.h"

const char *const pdfw_variants[] = {
    "PDF 1.0", "PDF 1.1", "PDF 1.2", "PDF 1.3", "PDF 1.4", "PDF 1.5", "PDF 1.6", "PDF 1.7",
    "PDF 2.0", NULL
};

#define PDFW_DEFAULT    4       /* 1.4: every reader there is opens it */

#define OBJ_CATALOG     1
#define OBJ_PAGES       2
#define OBJ_INFO        3
#define OBJ_FONTS       4       /* the dictionary of them all */
#define OBJ_FONT0       5       /* thirteen: three faces in four styles, and Symbol */
#define NFONTS          13
#define OBJ_FIRST_FREE  (OBJ_FONT0 + NFONTS)

static const char *const font_names[NFONTS] = {
    "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
    "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
    "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique",
    "Symbol"
};

/*
 * A font of drawings.  The thirteen fonts every reader has are Latin
 * ones: Windows' code page 1252 is all they can be asked for.  A Greek
 * or Cyrillic letter, or one of Latin Extended-A, is in none of them,
 * so the built-in face's own drawing of it goes into the file, as a
 * character of a Type 3 font - the pen's path, stroked - with a
 * ToUnicode map so that a reader can still copy and find the text.
 * One such font holds 255 characters of one of the first twelve fonts
 * (a bold letter is a different drawing); a thirteenth hundred starts
 * another.
 */
#define XFONT_MOST      64

typedef struct {
    TFACE   *face;
    uint16_t base;              /* which of the first twelve fonts it goes with */
    uint16_t count;             /* codes 1 to "count" are taken */
    uint16_t uni[256];          /* the character each code is */
    uint32_t obj;               /* its object, once it is written */
} XFONT;

/*
 * A font from a file.  Text in a face that is a TrueType file (TF.H)
 * goes out in that font, and the font goes into the PDF: a composite
 * font whose codes are the file's own glyph numbers, two bytes each
 * (Identity-H), with the file itself cut down to the glyphs the
 * document used (TF.C's tf_subset; the numbers stay as they were), the
 * widths of those glyphs, and a ToUnicode map so the text can still be
 * copied and found.  That needs PDF 1.3; an older version, or a file
 * whose maker does not allow it to be put in a document, has its text
 * written in the standard font the face is nearest to, as a built-in
 * face's is.  One such font is one FILE: a bold or a slope that the
 * face makes because the family has no file for it is made here too -
 * the text matrix leans, the glyphs are filled and outlined - and not
 * by a second copy.  A character the file has not got is the stand-in
 * face's, and goes the way a built-in face's does.
 */
#define EFONT_MOST      32

typedef struct {
    TFACE       *face;          /* a face that is the file: any one answers for it */
    void        *file;          /* tf_file's: which file */
    uint32_t    *uni;           /* for each glyph the text used, a character it was; 0: not used */
    unsigned int glyphs;
    uint32_t     obj;           /* its object, once it is written */
} EFONT;

typedef struct {
    WDOC        *doc;
    OUT         *out;           /* the file */
    OUT         *page;          /* the page being drawn: its content stream */
    int          major, minor;
    uint32_t    *offset;        /* where each object is in the file */
    unsigned int nobj, obj_cap;
    uint32_t    *kids;          /* the page objects */
    unsigned int nkids, kid_cap;
    uint32_t    *image;         /* the object each picture was written as, or 0 */
    uint32_t    *used;          /* pictures on this page */
    unsigned int nused, used_cap;
    uint32_t     color;         /* the fill colour last set on this page */
    int          color_set;
    uint32_t     stroke;        /* and the colour of a pen, which a font of drawings uses */
    int          stroke_set;
    XFONT       *xfont[XFONT_MOST];
    unsigned int nxfont;
    EFONT       *efont[EFONT_MOST];
    unsigned int nefont;
    int          embed;         /* the version can hold a font from a file: 1.3 and later */
    int          failed;        /* no memory */
    /* bookmarks: a heading's paragraph and the page object it is on */
    uint32_t    *mark_para, *mark_page;
    int         *mark_y;
    unsigned int nmarks, mark_cap;
} PDFW;

/* ------------------------------------------------------------------ */
/* numbers, strings, objects                                           */
/* ------------------------------------------------------------------ */

/* twips as points: two decimal places are exact for a twentieth */
static void put_tw( OUT *out, int twips )
{
    unsigned int hundredths;

    if ( twips < 0 ) {
        out_byte( out, '-' );
        twips = -twips;
    }
    hundredths = (unsigned int)twips * 5;
    out_fmt( out, "%u", hundredths / 100 );
    if ( hundredths % 100 ) {
        out_fmt( out, ".%02u", hundredths % 100 );
    }
    out_byte( out, ' ' );
}

/* a colour's part, 0 to 255, as 0 to 1 */
static void put_unit( OUT *out, uint32_t part )
{
    uint32_t thousandths = (part * 1000 + 127) / 255;

    if ( thousandths >= 1000 ) {
        out_str( out, "1 " );
    } else {
        out_fmt( out, "0.%03u ", thousandths );
    }
}

static int grow_u32( PDFW *pw, uint32_t **array, unsigned int *cap, unsigned int need )
{
    uint32_t *grown;
    unsigned int want = *cap ? *cap : 32;

    if ( need <= *cap ) {
        return 1;
    }
    while ( want < need ) {
        want *= 2;
    }
    grown = (uint32_t *)try_realloc( *array, want * sizeof( uint32_t ) );
    if ( grown == NULL ) {
        pw->failed = 1;
        return 0;
    }
    mem_set( grown + *cap, 0, (want - *cap) * sizeof( uint32_t ) );
    *array = grown;
    *cap = want;
    return 1;
}

/* an object number nobody has yet */
static uint32_t obj_new( PDFW *pw )
{
    if ( !grow_u32( pw, &pw->offset, &pw->obj_cap, pw->nobj + 1 ) ) {
        return 0;
    }
    return pw->nobj++;
}

static void obj_begin( PDFW *pw, uint32_t num )
{
    if ( num < pw->nobj ) {
        pw->offset[num] = out_tell( pw->out );
    }
    out_fmt( pw->out, "%u 0 obj\n", num );
}

/* a stream object: compressed when the version has Flate and it helps */
static void obj_stream( PDFW *pw, uint32_t num, const char *dict, const uint8_t *data, uint32_t len )
{
    uint8_t *packed = NULL;
    uint32_t packed_len = 0;

    if ( (pw->major > 1 || pw->minor >= 2) && len > 64 ) {
        packed = deflate_alloc( data, len, &packed_len, 1 );
        if ( packed != NULL && packed_len >= len ) {
            xfree( packed );
            packed = NULL;
        }
    }
    obj_begin( pw, num );
    out_fmt( pw->out, "<< %s/Length %u%s >>\nstream\n", dict, packed != NULL ? packed_len : len,
             packed != NULL ? " /Filter /FlateDecode" : "" );
    out_put( pw->out, packed != NULL ? packed : data, packed != NULL ? packed_len : len );
    out_str( pw->out, "\nendstream\nendobj\n" );
    xfree( packed );
}

/* text for the Info dictionary: ASCII as it is, anything else UTF-16 */
static void put_info( OUT *out, const char *key, const char *utf8 )
{
    const uint8_t *cur = (const uint8_t *)utf8, *end = cur + str_len( utf8 );
    uint32_t code;
    int plain = 1;

    if ( *utf8 == 0 ) {
        return;
    }
    for ( ; cur < end; cur++ ) {
        if ( *cur >= 0x7F || *cur < 0x20 ) {
            plain = 0;
        }
    }
    out_fmt( out, "/%s ", key );
    cur = (const uint8_t *)utf8;
    if ( plain ) {
        out_byte( out, '(' );
        for ( ; cur < end; cur++ ) {
            if ( *cur == '(' || *cur == ')' || *cur == '\\' ) {
                out_byte( out, '\\' );
            }
            out_byte( out, *cur );
        }
        out_str( out, ")\n" );
        return;
    }
    out_str( out, "<FEFF" );
    while ( cur < end ) {
        code = utf8_get( &cur, end );
        if ( code >= 0x10000UL ) {
            code -= 0x10000UL;
            out_fmt( out, "%04X%04X", 0xD800 + (code >> 10), 0xDC00 + (code & 0x3FF) );
        } else {
            out_fmt( out, "%04X", code );
        }
    }
    out_str( out, ">\n" );
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static void set_color( PDFW *pw, uint32_t color )
{
    if ( color == COLOR_AUTO ) {
        color = 0;
    }
    if ( pw->color_set && pw->color == color ) {
        return;
    }
    put_unit( pw->page, (color >> 16) & 0xFF );
    put_unit( pw->page, (color >> 8) & 0xFF );
    put_unit( pw->page, color & 0xFF );
    out_str( pw->page, "rg\n" );
    pw->color = color;
    pw->color_set = 1;
}

/* a filled box: x and y are its top left corner, down the page */
static void put_box( PDFW *pw, int x, int y, int width, int height )
{
    put_tw( pw->page, x );
    put_tw( pw->page, pw->doc->page.height - y - height );
    put_tw( pw->page, width );
    put_tw( pw->page, height );
    out_str( pw->page, "re f\n" );
}

static int font_index( PDFW *pw, const CHP *chp, TFACE **face_out )
{
    TFACE *face = tf_face( &pw->doc->font[chp->font < pw->doc->nfont ? chp->font : 0], chp->flags );
    int base = tf_base( face );

    *face_out = face;
    if ( base == TFB_SYMBOL ) {
        return 12;
    }
    return base * 4 + ((chp->flags & CF_BOLD) ? 1 : 0) + ((chp->flags & CF_ITALIC) ? 2 : 0);
}

static void put_text_byte( OUT *out, int byte )
{
    byte &= 0xFF;
    if ( byte == '(' || byte == ')' || byte == '\\' ) {
        out_byte( out, '\\' );
        out_byte( out, byte );
    } else if ( byte < 0x20 || byte >= 0x7F ) {
        /* three octal digits, which no reader can take for anything else */
        out_byte( out, '\\' );
        out_byte( out, '0' + (byte >> 6) );
        out_byte( out, '0' + ((byte >> 3) & 7) );
        out_byte( out, '0' + (byte & 7) );
    } else {
        out_byte( out, byte );
    }
}

static uint32_t upper_of( uint32_t code )
{
    if ( (code >= 'a' && code <= 'z') || (code >= 0xE0 && code <= 0xFE && code != 0xF7) ) {
        return code - 0x20;
    }
    return code;
}

/*
 * The code a character has in a font of drawings, the character added
 * to one if it is new: the font's number in the high part, the code -
 * 1 to 255 - in the low byte.  0 if there is no room or no memory, and
 * the character is then written as the standard font's nearest.
 */
static uint32_t xfont_code( PDFW *pw, int base, TFACE *face, uint32_t code )
{
    XFONT *font;
    unsigned int which, slot;

    for ( which = 0; which < pw->nxfont; which++ ) {
        font = pw->xfont[which];
        if ( font->base != base ) {
            continue;
        }
        for ( slot = 1; slot <= font->count; slot++ ) {
            if ( font->uni[slot] == code ) {
                return (which << 8) | slot;
            }
        }
        if ( font->count < 255 ) {
            font->uni[++font->count] = (uint16_t)code;
            return (which << 8) | font->count;
        }
    }
    if ( pw->nxfont >= XFONT_MOST ) {
        return 0;
    }
    font = (XFONT *)try_alloc( sizeof( XFONT ) );
    if ( font == NULL ) {
        return 0;
    }
    mem_set( font, 0, sizeof( *font ) );
    font->face = face;
    font->base = (uint16_t)base;
    font->count = 1;
    font->uni[1] = (uint16_t)code;
    pw->xfont[pw->nxfont] = font;
    return (pw->nxfont++ << 8) | 1;
}

/* the pen's colour, for what is stroked: a drawing, a bold that is made */
static void set_stroke( PDFW *pw, uint32_t color )
{
    if ( color == COLOR_AUTO ) {
        color = 0;
    }
    if ( !pw->stroke_set || pw->stroke != color ) {
        put_unit( pw->page, (color >> 16) & 0xFF );
        put_unit( pw->page, (color >> 8) & 0xFF );
        put_unit( pw->page, color & 0xFF );
        out_str( pw->page, "RG " );
        pw->stroke = color;
        pw->stroke_set = 1;
    }
}

/*
 * Which of the fonts from files a face's file is, the file added to
 * them if it is new.  -1 if the face is not a file's, or the file may
 * not go into a PDF, or this version of PDF cannot hold it, or there
 * is no room or no memory: the text is then written as a built-in
 * face's is.
 */
static int efont_index( PDFW *pw, TFACE *face )
{
    TFINFO info;
    EFONT *font;
    void *file;
    unsigned int which;

    if ( !pw->embed || !tf_embed( face ) ) {
        return -1;
    }
    file = tf_file( face );
    for ( which = 0; which < pw->nefont; which++ ) {
        if ( pw->efont[which]->file == file ) {
            return (int)which;
        }
    }
    if ( file == NULL || pw->nefont >= EFONT_MOST || !tf_file_info( face, &info ) || info.glyphs == 0 ||
         info.glyphs > 0xFFFF ) {
        return -1;
    }
    font = (EFONT *)try_alloc( sizeof( EFONT ) );
    if ( font == NULL ) {
        return -1;
    }
    font->uni = (uint32_t *)try_alloc( info.glyphs * sizeof( uint32_t ) );
    if ( font->uni == NULL ) {
        xfree( font );
        return -1;
    }
    mem_set( font->uni, 0, info.glyphs * sizeof( uint32_t ) );
    font->face = face;
    font->file = file;
    font->glyphs = info.glyphs;
    font->obj = 0;
    pw->efont[pw->nefont] = font;
    return (int)pw->nefont++;
}

/* text at a place: x, and the baseline's y down the page */
static void put_text( PDFW *pw, const CHP *chp, int size, int x, int base_y, const uint16_t *text, unsigned int count,
                      int upper )
{
    TFACE *face, *builtin;
    char near_buf[8];
    unsigned int pos = 0;
    uint32_t code, drawn, glyph;
    unsigned int part = 0;
    int pen = 0;
    int font = font_index( pw, chp, &face ), len, index, now = -1, want, efont, made = 0;
    int lean = 0, leaning = 0, heavy = 0, heavier = 0;

    if ( count == 0 ) {
        return;
    }
    /* a face that is a file: its font in the PDF, and the built-in face
       that stands in for it where the file has no such character */
    efont = efont_index( pw, face );
    builtin = tf_standin( face );
    if ( efont >= 0 ) {
        made = tf_made( face );
    }
    set_color( pw, chp->color );
    out_str( pw->page, "BT 1 0 0 1 " );
    put_tw( pw->page, x );
    put_tw( pw->page, pw->doc->page.height - base_y );
    out_str( pw->page, "Tm " );
    while ( pos < count ) {
        code = utf16_get( text, count, &pos );
        if ( upper ) {
            code = upper_of( code );
        }
        if ( code == WCH_NBSP ) {
            code = ' ';
        } else if ( code == WCH_NBHYPHEN ) {
            code = '-';
        }
        /* which font the character is in: the file's; or the standard
           one; or a font of drawings when the standard one has no such
           character */
        glyph = efont >= 0 ? tf_glyph( face, code ) : 0;
        if ( efont >= 0 && glyph >= pw->efont[efont]->glyphs ) {
            glyph = 0;                          /* not a number the font's table of them has */
        }
        drawn = 0;
        if ( glyph == 0 && font != 12 && code >= 0x100 && code <= 0xFFFF && cp_from_uni( CP_1252, code ) < 0 &&
             tf_draws( builtin, code ) ) {
            drawn = xfont_code( pw, font, builtin, code );
        }
        want = glyph ? 0x200 + efont : drawn ? (int)(0x100 + (drawn >> 8)) : font;
        if ( want != now ) {
            if ( now >= 0 ) {
                out_str( pw->page, now >= 0x200 ? "> Tj\n" : ") Tj\n" );
            }
            /* a slope and a bold that are made are the file's glyphs'
               alone: the text matrix leans from where the pen now is,
               and the glyphs are outlined as well as filled */
            lean = glyph != 0 && (made & CF_ITALIC) != 0;
            heavy = glyph != 0 && (made & CF_BOLD) != 0;
            if ( lean != leaning ) {
                out_str( pw->page, lean ? "1 0 0.2 1 " : "1 0 0 1 " );
                put_tw( pw->page, x + pen );
                put_tw( pw->page, pw->doc->page.height - base_y );
                out_str( pw->page, "Tm " );
                leaning = lean;
            }
            if ( heavy != heavier ) {
                if ( heavy ) {
                    set_stroke( pw, chp->color );
                    out_str( pw->page, "2 Tr " );
                    put_tw( pw->page, size / 24 > 0 ? size / 24 : 1 );
                    out_str( pw->page, "w " );
                } else {
                    out_str( pw->page, "0 Tr " );
                }
                heavier = heavy;
            }
            if ( glyph ) {
                out_fmt( pw->page, "/T%u ", (uint32_t)efont );
            } else if ( drawn ) {
                /* a drawing is stroked: the pen has the text's colour */
                set_stroke( pw, chp->color );
                out_fmt( pw->page, "/X%u ", drawn >> 8 );
            } else {
                out_fmt( pw->page, "/F%u ", (uint32_t)font );
            }
            put_tw( pw->page, size );
            out_str( pw->page, glyph ? "Tf <" : "Tf (" );
            now = want;
        }
        if ( efont >= 0 ) {
            /* where the pen is, in twips, for a text matrix that has to
               be set again: the widths the lines were laid out with */
            part += tf_advance( face, code ) * (unsigned int)size;
            pen += (int)(part / 1000);
            part %= 1000;
        }
        if ( glyph ) {
            out_fmt( pw->page, "%04X", glyph );
            if ( pw->efont[efont]->uni[glyph] == 0 ) {
                pw->efont[efont]->uni[glyph] = code;
            }
            continue;
        }
        if ( drawn ) {
            put_text_byte( pw->page, (int)(drawn & 0xFF) );
            continue;
        }
        if ( font == 12 ) {
            /* Symbol's own codes, as Windows keeps them: F020h to F0FFh */
            put_text_byte( pw->page, (code >= 0xF020 && code <= 0xF0FF) || code < 0x100 ? (int)(code & 0xFF) : '?' );
            continue;
        }
        len = cp_put( CP_1252, code, near_buf );
        for ( index = 0; index < len; index++ ) {
            put_text_byte( pw->page, near_buf[index] );
        }
    }
    if ( now >= 0 ) {
        out_str( pw->page, now >= 0x200 ? "> Tj " : ") Tj " );
    }
    out_str( pw->page, heavier ? "0 Tr ET\n" : "ET\n" );
}

/*
 * The fonts from files, at the end of the file when every glyph that
 * is wanted of them is known.  Five objects each: the file, cut down;
 * what a reader is told about it; the font of glyphs by number, with
 * their widths; the map back to Unicode; and the composite font the
 * pages name.
 */
static void write_efonts( PDFW *pw )
{
    EFONT *font;
    TFINFO info;
    OUT *map;
    char dict[40], name[80];
    uint8_t *keep, *data;
    unsigned int which;
    uint32_t gid, len, crc;
    unsigned int used, left;
    uint32_t code;
    uint32_t file_obj, desc_obj, cid_obj, map_obj, self;
    int open, index;

    for ( which = 0; which < pw->nefont && !pw->failed; which++ ) {
        font = pw->efont[which];
        file_obj = obj_new( pw );
        desc_obj = obj_new( pw );
        cid_obj = obj_new( pw );
        map_obj = obj_new( pw );
        self = obj_new( pw );
        keep = (uint8_t *)try_alloc( font->glyphs );
        if ( keep == NULL || self == 0 || pw->failed || !tf_file_info( font->face, &info ) ) {
            xfree( keep );
            pw->failed = 1;
            break;
        }
        used = 0;
        for ( gid = 0; gid < font->glyphs; gid++ ) {
            keep[gid] = (uint8_t)(font->uni[gid] != 0);
            used += keep[gid];
        }
        data = tf_subset( font->face, keep, font->glyphs, &len );
        xfree( keep );
        if ( data == NULL ) {
            /* the text is already written in its glyphs: without the
               file there is no PDF to be had */
            pw->failed = 1;
            break;
        }
        sfmt( dict, sizeof( dict ), "/Length1 %u ", len );
        obj_stream( pw, file_obj, dict, data, len );
        xfree( data );

        /* a subset's name starts with six letters of its own */
        crc = crc32_update( which, (const uint8_t *)font->uni, font->glyphs * sizeof( uint32_t ) );
        for ( index = 0; index < 6; index++ ) {
            name[index] = (char)('A' + crc % 26);
            crc /= 26;
        }
        name[6] = '+';
        str_cpyt( name + 7, info.name, sizeof( name ) - 7 );

        obj_begin( pw, desc_obj );
        out_fmt( pw->out, "<< /Type /FontDescriptor /FontName /%s /Flags %u /FontBBox [%d %d %d %d] "
                 "/ItalicAngle %d\n/Ascent %d /Descent %d /CapHeight %d /StemV 80 /FontFile2 %u 0 R >>\nendobj\n",
                 name, (uint32_t)((info.fixed ? 1 : 0) | (info.serif ? 2 : 0) | (info.symbol ? 4 : 32) |
                 (info.angle ? 64 : 0)), info.box[0], info.box[1], info.box[2], info.box[3], info.angle,
                 info.ascent, -info.descent, info.cap, file_obj );

        obj_begin( pw, cid_obj );
        out_fmt( pw->out, "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s /CIDSystemInfo << /Registry (Adobe) "
                 "/Ordering (Identity) /Supplement 0 >>\n/FontDescriptor %u 0 R /CIDToGIDMap /Identity /DW 1000 /W [",
                 name, desc_obj );
        /* the widths: runs of glyphs next to one another, each run its
           first glyph's number and then a width apiece */
        open = 0;
        index = 0;
        for ( gid = 0; gid < font->glyphs; gid++ ) {
            if ( font->uni[gid] == 0 ) {
                if ( open ) {
                    out_str( pw->out, "]" );
                    open = 0;
                }
                continue;
            }
            if ( !open ) {
                out_fmt( pw->out, "%s%u [", (++index & 7) == 0 ? "\n" : " ", gid );
                open = 1;
            } else {
                out_byte( pw->out, ' ' );
            }
            out_fmt( pw->out, "%u", tf_glyph_advance( font->face, gid ) );
        }
        out_fmt( pw->out, "%s ] >>\nendobj\n", open ? "]" : "" );

        map = out_mem();
        if ( map == NULL ) {
            pw->failed = 1;
            break;
        }
        out_str( map, "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
                 "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                 "/CMapName /Adobe-Identity-UCS def /CMapType 2 def\n"
                 "1 begincodespacerange <0000> <FFFF> endcodespacerange\n" );
        left = used;
        index = 0;
        for ( gid = 0; gid < font->glyphs; gid++ ) {
            code = font->uni[gid];
            if ( code == 0 ) {
                continue;
            }
            if ( index == 0 ) {
                out_fmt( map, "%u beginbfchar\n", left > 100 ? 100 : left );
            }
            if ( code >= 0x10000UL ) {
                code -= 0x10000UL;
                out_fmt( map, "<%04X> <%04X%04X>\n", gid, 0xD800 + ((code >> 10) & 0x3FF), 0xDC00 + (code & 0x3FF) );
            } else {
                out_fmt( map, "<%04X> <%04X>\n", gid, code );
            }
            left--;
            if ( ++index == 100 || left == 0 ) {
                out_str( map, "endbfchar\n" );
                index = 0;
            }
        }
        out_str( map, "endcmap CMapName currentdict /CMap defineresource pop end end\n" );
        data = out_data( map, &len );
        obj_stream( pw, map_obj, "", data, len );
        if ( out_error( map ) ) {
            pw->failed = 1;
        }
        out_close( map );

        obj_begin( pw, self );
        out_fmt( pw->out, "<< /Type /Font /Subtype /Type0 /BaseFont /%s /Encoding /Identity-H "
                 "/DescendantFonts [%u 0 R] /ToUnicode %u 0 R >>\nendobj\n", name, cid_obj, map_obj );
        font->obj = self;
    }
}

/* a point of a drawing's path, for tf_path */
static void xfont_point( void *ctx, int move, int x, int y )
{
    out_fmt( (OUT *)ctx, "%d %d %c\n", x, y, move ? 'm' : 'l' );
}

/* the fonts of drawings, at the end of the file when every character
   that is wanted of them is known: each character a stream of its own */
static void write_xfonts( PDFW *pw )
{
    XFONT *font;
    OUT *proc;
    char head[48];
    uint8_t *data;
    unsigned int which, slot;
    uint32_t len, first, map, self;
    int pen;

    for ( which = 0; which < pw->nxfont; which++ ) {
        font = pw->xfont[which];
        first = 0;
        for ( slot = 1; slot <= font->count; slot++ ) {
            len = obj_new( pw );
            if ( slot == 1 ) {
                first = len;
            }
        }
        map = (pw->major > 1 || pw->minor >= 2) ? obj_new( pw ) : 0;
        self = obj_new( pw );
        if ( first == 0 || self == 0 || pw->failed ) {
            continue;                           /* it has no object: not named among the fonts */
        }
        for ( slot = 1; slot <= font->count; slot++ ) {
            proc = out_mem();
            if ( proc == NULL ) {
                pw->failed = 1;
                break;
            }
            if ( tf_path( font->face, font->uni[slot], xfont_point, proc, &pen ) ) {
                out_str( proc, "S\n" );
            }
            data = out_data( proc, &len );
            /* how wide the character is, then the pen - its width, and
               round ends and corners - and then the path */
            sfmt( head, sizeof( head ), "%u 0 d0\n%d w 1 J 1 j\n", tf_advance( font->face, font->uni[slot] ),
                  pen );
            obj_begin( pw, first + slot - 1 );
            out_fmt( pw->out, "<< /Length %u >>\nstream\n%s", len + str_len( head ), head );
            out_put( pw->out, data, len );
            out_str( pw->out, "\nendstream\nendobj\n" );
            if ( out_error( proc ) ) {
                pw->failed = 1;
            }
            out_close( proc );
        }
        if ( map ) {
            proc = out_mem();
            if ( proc == NULL ) {
                pw->failed = 1;
            } else {
                out_fmt( proc, "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
                         "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                         "/CMapName /Adobe-Identity-UCS def /CMapType 2 def\n"
                         "1 begincodespacerange <00> <FF> endcodespacerange\n" );
                for ( slot = 1; slot <= font->count; slot++ ) {
                    if ( slot % 100 == 1 ) {
                        out_fmt( proc, "%s%u beginbfchar\n", slot > 1 ? "endbfchar\n" : "",
                                 font->count - slot + 1 > 100 ? 100 : font->count - slot + 1 );
                    }
                    out_fmt( proc, "<%02X> <%04X>\n", slot, (uint32_t)font->uni[slot] );
                }
                out_str( proc, "endbfchar\nendcmap CMapName currentdict /CMap defineresource pop end end\n" );
                data = out_data( proc, &len );
                obj_stream( pw, map, "", data, len );
                if ( out_error( proc ) ) {
                    pw->failed = 1;
                }
                out_close( proc );
            }
        }
        obj_begin( pw, self );
        out_fmt( pw->out, "<< /Type /Font /Subtype /Type3 /Name /X%u /FontBBox [-400 -400 2000 1200] "
                 "/FontMatrix [0.001 0 0 0.001 0 0]\n/FirstChar 1 /LastChar %u /Widths [", which,
                 (uint32_t)font->count );
        for ( slot = 1; slot <= font->count; slot++ ) {
            out_fmt( pw->out, "%u%s", tf_advance( font->face, font->uni[slot] ), (slot & 15) == 0 ? "\n" : " " );
        }
        out_str( pw->out, "]\n/Encoding << /Type /Encoding /Differences [1" );
        for ( slot = 1; slot <= font->count; slot++ ) {
            out_fmt( pw->out, " /c%u%s", slot, (slot & 15) == 0 ? "\n" : "" );
        }
        out_str( pw->out, "] >>\n/CharProcs <<" );
        for ( slot = 1; slot <= font->count; slot++ ) {
            out_fmt( pw->out, " /c%u %u 0 R%s", slot, first + slot - 1, (slot & 7) == 0 ? "\n" : "" );
        }
        out_str( pw->out, " >>\n/Resources << /ProcSet [/PDF] >>" );
        if ( map ) {
            out_fmt( pw->out, " /ToUnicode %u 0 R", map );
        }
        out_str( pw->out, " >>\nendobj\n" );
        font->obj = self;
    }
}

/* a JPEG file's size and colours: 0 if it is not one this can use */
static int jpeg_info( const uint8_t *data, uint32_t len, uint32_t *width, uint32_t *height, uint32_t *comps )
{
    uint32_t pos = 2, seg;

    if ( len < 4 || data[0] != 0xFF || data[1] != 0xD8 ) {
        return 0;
    }
    while ( IN_RANGE( pos, 4, len ) ) {
        if ( data[pos] != 0xFF ) {
            return 0;
        }
        if ( data[pos + 1] == 0xFF ) {
            pos++;
            continue;
        }
        seg = BE16( data + pos + 2 );
        if ( data[pos + 1] >= 0xC0 && data[pos + 1] <= 0xC2 ) {
            if ( seg < 8 || !IN_RANGE( pos, 10, len ) ) {
                return 0;
            }
            *height = BE16( data + pos + 5 );
            *width = BE16( data + pos + 7 );
            *comps = data[pos + 9];
            return *width && *height && (*comps == 1 || *comps == 3 || *comps == 4);
        }
        if ( data[pos + 1] == 0xD9 || data[pos + 1] == 0xDA || seg < 2 || !IN_RANGE( pos + 2, seg, len ) ) {
            return 0;
        }
        pos += 2 + seg;
    }
    return 0;
}

/* a picture: its object written the first time it is met */
/* a picture's object, written the first time the picture is met: a JPEG
   file goes in as it is, anything else IMG.C can decode as its pixels.
   0 if it is a kind that cannot be had either way. */
static uint32_t image_object( PDFW *pw, int which )
{
    const WOBJ *obj = &pw->doc->obj[which];
    char dict[120];
    uint8_t *pixels, hold;
    uint32_t pix_w, pix_h, comps, num;
    unsigned int index;

    if ( pw->image[which] != 0 ) {
        return pw->image[which];
    }
    if ( obj->fmt == IMG_JPEG && jpeg_info( obj->data, obj->len, &pix_w, &pix_h, &comps ) ) {
        num = obj_new( pw );
        if ( num == 0 ) {
            return 0;
        }
        obj_begin( pw, num );
        out_fmt( pw->out, "<< /Type /XObject /Subtype /Image /Width %u /Height %u /BitsPerComponent 8 "
                 "/ColorSpace /%s%s /Filter /DCTDecode /Length %u >>\nstream\n", pix_w, pix_h,
                 comps == 1 ? "DeviceGray" : comps == 3 ? "DeviceRGB" : "DeviceCMYK",
                 comps == 4 ? " /Decode [1 0 1 0 1 0 1 0]" : "", obj->len );
        out_put( pw->out, obj->data, obj->len );
        out_str( pw->out, "\nendstream\nendobj\n" );
        pw->image[which] = num;
        return num;
    }
    pixels = img_decode( obj->data, obj->len, &pix_w, &pix_h );
    if ( pixels == NULL ) {
        return 0;
    }
    /* blue, green, red as they come; red, green, blue as a PDF has them */
    for ( index = 0; index < pix_w * pix_h; index++ ) {
        hold = pixels[index * 3];
        pixels[index * 3] = pixels[index * 3 + 2];
        pixels[index * 3 + 2] = hold;
    }
    num = obj_new( pw );
    if ( num != 0 ) {
        sfmt( dict, sizeof( dict ), "/Type /XObject /Subtype /Image /Width %u /Height %u /BitsPerComponent 8 "
              "/ColorSpace /DeviceRGB ", pix_w, pix_h );
        obj_stream( pw, num, dict, pixels, pix_w * pix_h * 3 );
        pw->image[which] = num;
    }
    xfree( pixels );
    return num;
}

/* a picture on the page: x and top are its top left corner */
static void put_image( PDFW *pw, int which, int x, int top, int width, int height )
{
    if ( image_object( pw, which ) != 0 ) {
        if ( grow_u32( pw, &pw->used, &pw->used_cap, pw->nused + 1 ) ) {
            pw->used[pw->nused++] = (uint32_t)which;
        }
        out_str( pw->page, "q " );
        put_tw( pw->page, width );
        out_str( pw->page, "0 0 " );
        put_tw( pw->page, height );
        put_tw( pw->page, x );
        put_tw( pw->page, pw->doc->page.height - top - height );
        out_fmt( pw->page, "cm /Im%u Do Q\n", (uint32_t)which );
        return;
    }
    /* a frame where a picture that cannot be had is */
    set_color( pw, RGB( 128, 128, 128 ) );
    put_box( pw, x, top, width, 10 );
    put_box( pw, x, top + height - 10, width, 10 );
    put_box( pw, x, top, 10, height );
    put_box( pw, x + width - 10, top, 10, height );
    pw->doc->lost |= LOST_IMAGES;
}
typedef struct {
    PDFW   *pw;
    LAY    *lay;
    int     left;               /* the text area's left edge on the page */
    int     base;               /* the line's baseline, down the page */
    int     line_top;
} LINECTX;

static void line_seg( void *ctx, const LSEG *seg )
{
    LINECTX *lc = (LINECTX *)ctx;
    PDFW *pw = lc->pw;
    const CHP *chp = wd_chp_get( pw->doc, seg->chp );
    TFACE *face;
    uint16_t dots[64];
    int x = lc->left + seg->x, base = lc->base - seg->rise, dot, thick;
    unsigned int count, index;

    switch ( seg->kind ) {
    case SEG_TEXT:
    case SEG_MADE:
        put_text( pw, chp, seg->size, x, base, seg->text, seg->count, seg->upper );
        break;
    case SEG_TAB:
        if ( seg->lead == LEAD_DOTS || seg->lead == LEAD_DASHES ) {
            font_index( pw, chp, &face );
            dot = (int)(tf_advance( face, seg->lead == LEAD_DOTS ? '.' : '-' ) * (unsigned int)seg->size / 1000);
            count = dot > 0 ? (unsigned int)(seg->width / dot) : 0;
            if ( count > 64 ) {
                count = 64;
            }
            for ( index = 0; index < count; index++ ) {
                dots[index] = (uint16_t)(seg->lead == LEAD_DOTS ? '.' : '-');
            }
            /* set against the right, so the rows of dots of a list line up */
            put_text( pw, chp, seg->size, x + seg->width - (int)count * dot, base, dots, count, 0 );
        } else if ( seg->lead == LEAD_LINE ) {
            set_color( pw, chp->color );
            put_box( pw, x, base + seg->size / 10, seg->width, seg->size / 20 > 10 ? seg->size / 20 : 10 );
        }
        break;
    case SEG_OBJECT:
        if ( seg->obj >= 0 && (unsigned int)seg->obj < pw->doc->nobj ) {
            put_image( pw, seg->obj, x, lc->base - seg->height, seg->width, seg->height );
        }
        return;
    }
    /* the lines under and through text */
    thick = seg->size / 20 > 10 ? seg->size / 20 : 10;
    if ( chp->flags & (CF_UNDER | CF_DUNDER | CF_WUNDER) ) {
        set_color( pw, chp->color );
        put_box( pw, x, base + seg->size / 10, seg->width, thick );
        if ( chp->flags & CF_DUNDER ) {
            put_box( pw, x, base + seg->size / 10 + 2 * thick, seg->width, thick );
        }
    }
    if ( chp->flags & CF_STRIKE ) {
        set_color( pw, chp->color );
        put_box( pw, x, base - seg->size * 3 / 10, seg->width, thick );
    }
}

/* one line of a story, the story's "story_top" at "page_top" down the page */
static void draw_line( PDFW *pw, LAY *lay, unsigned int para, unsigned int line, int left, int shift )
{
    LINECTX lc;
    LINFO info;

    if ( !lay_line( lay, para, line, &info ) ) {
        return;
    }
    lc.pw = pw;
    lc.lay = lay;
    lc.left = left;
    lc.line_top = info.y + shift;
    lc.base = lc.line_top + info.base;
    lay_segs( lay, para, line, line_seg, &lc );
}

/* the lines round a table row's cells, once for the row */
static void draw_rules( PDFW *pw, LAY *lay, unsigned int para, int left, int shift, int *last_top )
{
    int edges[MAX_CELLS + 1], top, height;
    int count, index;

    if ( !lay_row_rules( lay, para, &top, &height, edges, &count ) || top == *last_top ) {
        return;
    }
    *last_top = top;
    set_color( pw, 0 );
    for ( index = 0; index <= count; index++ ) {
        put_box( pw, left + edges[index], top + shift, 10, height + 10 );
    }
    put_box( pw, left + edges[0], top + shift, edges[count] - edges[0], 10 );
    put_box( pw, left + edges[0], top + shift + height, edges[count] - edges[0], 10 );
}

/* the lines a paragraph has above, below and beside it */
static void draw_borders( PDFW *pw, LAY *lay, const WPARA *para, unsigned int index, int left, int shift )
{
    const PAP *pap = &para->pap;
    LINFO first, last;
    int col_x, col_w, x0, x1, top, bottom;
    unsigned int lines = lay_lines( lay, index );

    if ( !(pap->flags & (PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT)) || lines == 0 ||
         !lay_line( lay, index, 0, &first ) || !lay_line( lay, index, lines - 1, &last ) ) {
        return;
    }
    lay_para_col( lay, index, &col_x, &col_w );
    x0 = left + col_x + (pap->left > 0 ? pap->left : 0) - 40;
    x1 = left + col_x + col_w - (pap->right > 0 ? pap->right : 0) + 40;
    top = first.y + shift - 10;
    bottom = last.y + last.height + shift + 10;
    if ( x1 <= x0 || bottom <= top ) {
        return;
    }
    set_color( pw, 0 );
    if ( pap->flags & PF_BORDER_TOP ) {
        put_box( pw, x0, top, x1 - x0, 10 );
    }
    if ( pap->flags & PF_BORDER_BOT ) {
        put_box( pw, x0, bottom, x1 - x0 + 10, 10 );
    }
    if ( pap->flags & PF_BORDER_LEFT ) {
        put_box( pw, x0, top, 10, bottom - top );
    }
    if ( pap->flags & PF_BORDER_RIGHT ) {
        put_box( pw, x1, top, 10, bottom - top );
    }
}

/* a whole story - a header, a footer - with its top at "top" */
static void draw_story( PDFW *pw, STORY *story, int top, int from_bottom, unsigned int page_no, unsigned int pages )
{
    LAY *lay;
    unsigned int para, line, lines;
    int last_top = -1;

    if ( story->count == 0 ) {
        return;
    }
    lay = lay_new( pw->doc, story, 0 );
    if ( lay == NULL ) {
        pw->failed = 1;
        return;
    }
    lay_set_page( lay, page_no, pages );
    if ( !lay_update( lay ) ) {
        pw->failed = 1;
    }
    if ( from_bottom ) {
        top -= lay_height( lay );
    }
    for ( para = 0; para < story->count; para++ ) {
        lines = lay_lines( lay, para );
        draw_rules( pw, lay, para, pw->doc->page.left, top, &last_top );
        draw_borders( pw, lay, story->para[para], para, pw->doc->page.left, top );
        for ( line = 0; line < lines; line++ ) {
            draw_line( pw, lay, para, line, pw->doc->page.left, top );
        }
    }
    lay_free( lay );
}

static void page_begin( PDFW *pw )
{
    pw->page = out_mem();
    if ( pw->page == NULL ) {
        pw->failed = 1;
    }
    pw->nused = 0;
    pw->color_set = 0;
    pw->stroke_set = 0;
}

static void page_end( PDFW *pw )
{
    uint8_t *data;
    uint32_t len, content, page;
    unsigned int index, which;

    content = obj_new( pw );
    page = obj_new( pw );
    if ( pw->page == NULL || content == 0 || page == 0 ||
         !grow_u32( pw, &pw->kids, &pw->kid_cap, pw->nkids + 1 ) ) {
        out_close( pw->page );
        pw->page = NULL;
        pw->failed = 1;
        return;
    }
    if ( out_error( pw->page ) ) {
        pw->failed = 1;
    }
    data = out_data( pw->page, &len );
    obj_stream( pw, content, "", data, len );
    out_close( pw->page );
    pw->page = NULL;
    obj_begin( pw, page );
    out_fmt( pw->out, "<< /Type /Page /Parent %u 0 R /Contents %u 0 R /Resources << /ProcSet [/PDF /Text "
             "/ImageB /ImageC] /Font %u 0 R", (uint32_t)OBJ_PAGES, content, (uint32_t)OBJ_FONTS );
    if ( pw->nused ) {
        out_str( pw->out, " /XObject <<" );
        for ( index = 0; index < pw->nused; index++ ) {
            which = pw->used[index];
            /* a picture used twice on a page is named once */
            if ( pw->image[which] & 0x80000000UL ) {
                continue;
            }
            out_fmt( pw->out, " /Im%u %u 0 R", which, pw->image[which] );
            pw->image[which] |= 0x80000000UL;
        }
        for ( index = 0; index < pw->nused; index++ ) {
            pw->image[pw->used[index]] &= 0x7FFFFFFFUL;
        }
        out_str( pw->out, " >>" );
    }
    out_str( pw->out, " >> >>\nendobj\n" );
    pw->kids[pw->nkids++] = page;
}

/* ------------------------------------------------------------------ */
/* the file                                                            */
/* ------------------------------------------------------------------ */

static void mark_add( PDFW *pw, unsigned int para, int y )
{
    if ( !grow_u32( pw, &pw->mark_para, &pw->mark_cap, pw->nmarks + 1 ) ) {
        return;
    }
    /* the three grow together: the first sets the size of all */
    if ( pw->nmarks == 0 || mem_size( pw->mark_page ) < pw->mark_cap * sizeof( uint32_t ) ) {
        uint32_t *pages = (uint32_t *)try_realloc( pw->mark_page, pw->mark_cap * sizeof( uint32_t ) );
        int *tops = (int *)try_realloc( pw->mark_y, pw->mark_cap * sizeof( int ) );

        if ( pages != NULL ) {
            pw->mark_page = pages;
        }
        if ( tops != NULL ) {
            pw->mark_y = tops;
        }
        if ( pages == NULL || tops == NULL ) {
            pw->failed = 1;
            return;
        }
    }
    pw->mark_para[pw->nmarks] = para;
    pw->mark_page[pw->nmarks] = pw->nkids;      /* the page being drawn: its number in kids */
    pw->mark_y[pw->nmarks] = y;
    pw->nmarks++;
}

static void write_body( PDFW *pw )
{
    WDOC *doc = pw->doc;
    LAY *lay;
    const LPAGE *start, *next;
    WPARA *para;
    LINFO info;
    unsigned int pages, index, pi, li, lines, first_line, kind;
    int shift, last_top;

    lay = lay_new( doc, &doc->body, 0 );
    if ( lay == NULL || !lay_update( lay ) ) {
        pw->failed = 1;
        lay_free( lay );
        return;
    }
    pages = lay_pages( lay );
    for ( index = 0; index < pages && !pw->failed && !out_error( pw->out ); index++ ) {
        start = lay_page( lay, index );
        next = index + 1 < pages ? lay_page( lay, index + 1 ) : NULL;
        shift = doc->page.top - start->y;
        last_top = -1;
        page_begin( pw );
        if ( pw->failed ) {
            break;
        }
        draw_story( pw, &doc->header, doc->page.header, 0, index + doc->page.first_page, pages );
        draw_story( pw, &doc->footer, doc->page.height - doc->page.footer, 1, index + doc->page.first_page,
                    pages );
        for ( pi = start->para; pi < doc->body.count; pi++ ) {
            if ( next != NULL && pi > next->para ) {
                break;
            }
            para = doc->body.para[pi];
            lines = lay_lines( lay, pi );
            first_line = pi == start->para ? start->line : 0;
            if ( next != NULL && pi == next->para ) {
                lines = next->line;
            }
            if ( first_line >= lines ) {
                continue;
            }
            kind = doc->style[para->pap.style < doc->nstyle ? para->pap.style : 0].kind;
            if ( first_line == 0 && kind >= STY_HEADING1 && kind <= STY_HEADING6 && para->len &&
                 lay_line( lay, pi, 0, &info ) ) {
                mark_add( pw, pi, info.y + shift );
            }
            draw_rules( pw, lay, pi, doc->page.left, shift, &last_top );
            if ( first_line == 0 && lines == lay_lines( lay, pi ) ) {
                /* a paragraph that is all on this page: one cut in two by
                   the page's end goes without the lines round it */
                draw_borders( pw, lay, para, pi, doc->page.left, shift );
            }
            for ( li = first_line; li < lines; li++ ) {
                draw_line( pw, lay, pi, li, doc->page.left, shift );
            }
        }
        page_end( pw );
    }
    lay_free( lay );
}

/* the footnotes, after the text: each under its number */
static void write_notes( PDFW *pw )
{
    WDOC *doc = pw->doc;
    WPARA *para;
    STORY *story;
    LAY *lay;
    LINFO info;
    CHP chp = doc->style[0].chp;
    uint16_t digits[12];
    char text[12];
    unsigned int pi, index, number = 0, line, lines, count, note_para;
    int y = 0, room = doc->page.height - doc->page.top - doc->page.bottom, top;
    int open = 0;

    for ( pi = 0; pi < doc->body.count && !pw->failed; pi++ ) {
        para = doc->body.para[pi];
        for ( index = 0; index < para->nanch; index++ ) {
            if ( para->anch[index].obj >= doc->nobj || doc->obj[para->anch[index].obj].kind != OBJ_NOTE ) {
                continue;
            }
            story = doc->obj[para->anch[index].obj].story;
            number++;
            if ( story == NULL || story->count == 0 ) {
                continue;
            }
            lay = lay_new( doc, story, 0 );
            if ( lay == NULL ) {
                pw->failed = 1;
                return;
            }
            lay_set_width( lay, doc->page.width - doc->page.left - doc->page.right - 480 );
            lay_update( lay );
            fmt_dec( number, 0, text );
            for ( count = 0; text[count]; count++ ) {
                digits[count] = (uint16_t)text[count];
            }
            digits[count++] = '.';
            for ( note_para = 0; note_para < story->count; note_para++ ) {
                lines = lay_lines( lay, note_para );
                for ( line = 0; line < lines; line++ ) {
                    if ( !lay_line( lay, note_para, line, &info ) ) {
                        continue;
                    }
                    if ( open && y + info.height > room ) {
                        page_end( pw );
                        open = 0;
                    }
                    if ( !open ) {
                        page_begin( pw );
                        if ( pw->failed ) {
                            lay_free( lay );
                            return;
                        }
                        open = 1;
                        y = 0;
                    }
                    top = doc->page.top + y;
                    if ( note_para == 0 && line == 0 ) {
                        chp.size = 20;
                        put_text( pw, &chp, 200, doc->page.left, top + info.base, digits, count, 0 );
                    }
                    draw_line( pw, lay, note_para, line, doc->page.left + 480, top - info.y );
                    y += info.height;
                }
            }
            y += 120;
            lay_free( lay );
        }
    }
    if ( open ) {
        page_end( pw );
    }
}

static void put_date( OUT *out, const char *key )
{
    DATETIME now;

    sys_now( &now );
    out_fmt( out, "/%s (D:%04u%02u%02u%02u%02u%02u)\n", key, (uint32_t)now.year, (uint32_t)now.month, (uint32_t)now.day,
             (uint32_t)now.hour, (uint32_t)now.minute, (uint32_t)now.second );
}

/* text for XML: the three characters it cannot hold as themselves */
static void put_xml( OUT *out, const char *utf8 )
{
    for ( ; *utf8; utf8++ ) {
        if ( *utf8 == '&' ) {
            out_str( out, "&amp;" );
        } else if ( *utf8 == '<' ) {
            out_str( out, "&lt;" );
        } else if ( *utf8 == '>' ) {
            out_str( out, "&gt;" );
        } else if ( *utf8 >= 0x20 ) {
            out_byte( out, *utf8 );
        }
    }
}

static void write_tail( PDFW *pw )
{
    WDOC *doc = pw->doc;
    OUT *out = pw->out, *xmp;
    WPARA *para;
    char title[INFO_LEN];
    uint8_t *data;
    unsigned int index;
    uint32_t xref, len, meta = 0, outline = 0, first_mark = 0, crc;
    int with_info = pw->major < 2;

    /* the fonts */
    for ( index = 0; index < NFONTS; index++ ) {
        obj_begin( pw, OBJ_FONT0 + index );
        out_fmt( out, "<< /Type /Font /Subtype /Type1 /Name /F%u /BaseFont /%s%s >>\nendobj\n", index,
                 font_names[index], index == 12 ? "" : " /Encoding /WinAnsiEncoding" );
    }
    write_xfonts( pw );
    write_efonts( pw );
    obj_begin( pw, OBJ_FONTS );
    out_str( out, "<<" );
    for ( index = 0; index < NFONTS; index++ ) {
        out_fmt( out, " /F%u %u 0 R", index, OBJ_FONT0 + index );
    }
    for ( index = 0; index < pw->nefont; index++ ) {
        if ( pw->efont[index]->obj ) {
            out_fmt( out, " /T%u %u 0 R", index, pw->efont[index]->obj );
        }
    }
    for ( index = 0; index < pw->nxfont; index++ ) {
        if ( pw->xfont[index]->obj ) {
            out_fmt( out, " /X%u %u 0 R", index, pw->xfont[index]->obj );
        }
    }
    out_str( out, " >>\nendobj\n" );

    /* bookmarks: the headings, one after another */
    if ( pw->nmarks ) {
        outline = obj_new( pw );
        for ( index = 0; index < pw->nmarks; index++ ) {
            len = obj_new( pw );
            if ( index == 0 ) {
                first_mark = len;
            }
        }
        if ( outline == 0 || first_mark == 0 || pw->failed ) {
            outline = 0;
        }
    }
    if ( outline ) {
        obj_begin( pw, outline );
        out_fmt( out, "<< /Type /Outlines /First %u 0 R /Last %u 0 R /Count %u >>\nendobj\n", first_mark,
                 first_mark + pw->nmarks - 1, pw->nmarks );
        for ( index = 0; index < pw->nmarks; index++ ) {
            para = doc->body.para[pw->mark_para[index]];
            utf8_copy( title, sizeof( title ), para->text, para->len > 60 ? 60 : para->len );
            obj_begin( pw, first_mark + index );
            out_fmt( out, "<< /Parent %u 0 R ", outline );
            put_info( out, "Title", title[0] ? title : " " );
            if ( index > 0 ) {
                out_fmt( out, "/Prev %u 0 R ", first_mark + index - 1 );
            }
            if ( index + 1 < pw->nmarks ) {
                out_fmt( out, "/Next %u 0 R ", first_mark + index + 1 );
            }
            out_fmt( out, "/Dest [%u 0 R /XYZ ", pw->kids[pw->mark_page[index] < pw->nkids ?
                     pw->mark_page[index] : 0] );
            put_tw( out, doc->page.left );
            put_tw( out, doc->page.height - pw->mark_y[index] );
            out_str( out, "null] >>\nendobj\n" );
        }
    }

    /* what the document is about, as XMP, where the version has it */
    if ( pw->major > 1 || pw->minor >= 4 ) {
        xmp = out_mem();
        meta = obj_new( pw );
        if ( xmp != NULL && meta != 0 ) {
            out_str( xmp, "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                     "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
                     "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                     "<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                     "xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">\n" );
            if ( doc->title[0] ) {
                out_str( xmp, "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">" );
                put_xml( xmp, doc->title );
                out_str( xmp, "</rdf:li></rdf:Alt></dc:title>\n" );
            }
            if ( doc->author[0] ) {
                out_str( xmp, "<dc:creator><rdf:Seq><rdf:li>" );
                put_xml( xmp, doc->author );
                out_str( xmp, "</rdf:li></rdf:Seq></dc:creator>\n" );
            }
            if ( doc->subject[0] ) {
                out_str( xmp, "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">" );
                put_xml( xmp, doc->subject );
                out_str( xmp, "</rdf:li></rdf:Alt></dc:description>\n" );
            }
            if ( doc->keywords[0] ) {
                out_str( xmp, "<pdf:Keywords>" );
                put_xml( xmp, doc->keywords );
                out_str( xmp, "</pdf:Keywords>\n" );
            }
            out_str( xmp, "<pdf:Producer>PMWRITE 1.0</pdf:Producer>\n<xmp:CreatorTool>PMWRITE 1.0"
                     "</xmp:CreatorTool>\n</rdf:Description>\n</rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>" );
            data = out_data( xmp, &len );
            /* never compressed: something that is not a PDF reader may want to find it */
            obj_begin( pw, meta );
            out_fmt( out, "<< /Type /Metadata /Subtype /XML /Length %u >>\nstream\n", len );
            out_put( out, data, len );
            out_str( out, "\nendstream\nendobj\n" );
        } else {
            meta = 0;
        }
        out_close( xmp );
    }

    obj_begin( pw, OBJ_PAGES );
    out_str( out, "<< /Type /Pages /Kids [" );
    for ( index = 0; index < pw->nkids; index++ ) {
        out_fmt( out, "%u 0 R%s", pw->kids[index], (index & 7) == 7 ? "\n" : " " );
    }
    out_fmt( out, "] /Count %u /MediaBox [0 0 ", pw->nkids );
    put_tw( out, doc->page.width );
    put_tw( out, doc->page.height );
    out_str( out, "] >>\nendobj\n" );

    obj_begin( pw, OBJ_CATALOG );
    out_fmt( out, "<< /Type /Catalog /Pages %u 0 R", (uint32_t)OBJ_PAGES );
    if ( outline ) {
        out_fmt( out, " /Outlines %u 0 R /PageMode /UseOutlines", outline );
    }
    if ( meta ) {
        out_fmt( out, " /Metadata %u 0 R", meta );
    }
    out_str( out, " >>\nendobj\n" );

    obj_begin( pw, OBJ_INFO );
    out_str( out, "<<\n" );
    if ( with_info ) {
        put_info( out, "Title", doc->title );
        put_info( out, "Author", doc->author );
        put_info( out, "Subject", doc->subject );
        put_info( out, "Keywords", doc->keywords );
        out_str( out, "/Creator (PMWRITE 1.0)\n/Producer (PMWRITE 1.0)\n" );
    }
    put_date( out, "CreationDate" );
    put_date( out, "ModDate" );
    out_str( out, ">>\nendobj\n" );

    /* the table: twenty bytes an entry, exactly */
    xref = out_tell( out );
    out_fmt( out, "xref\n0 %u\n0000000000 65535 f \n", pw->nobj );
    for ( index = 1; index < pw->nobj; index++ ) {
        out_fmt( out, "%010u 00000 n \n", pw->offset[index] );
    }
    /* an ID that is this file's: from where its objects fell, and its size */
    crc = crc32_update( 0, (const uint8_t *)pw->offset, pw->nobj * sizeof( uint32_t ) );
    out_fmt( out, "trailer\n<< /Size %u /Root %u 0 R /Info %u 0 R /ID [<%08X%08X%08X%08X> <%08X%08X%08X%08X>] >>\n"
             "startxref\n%u\n%%%%EOF\n", pw->nobj, (uint32_t)OBJ_CATALOG, (uint32_t)OBJ_INFO, crc, xref, pw->nobj,
             crc ^ 0x504D5752UL, crc, xref, pw->nobj, crc ^ 0x504D5752UL, xref );
}

int pdfw_save( WDOC *doc, OUT *out, int variant )
{
    static const uint8_t minors[] = { 0, 1, 2, 3, 4, 5, 6, 7, 0 };
    PDFW pw;
    unsigned int index;
    int rc;

    if ( variant < 0 || variant > 8 ) {
        variant = PDFW_DEFAULT;
    }
    mem_set( &pw, 0, sizeof( pw ) );
    pw.doc = doc;
    pw.out = out;
    pw.major = variant == 8 ? 2 : 1;
    pw.minor = minors[variant];
    pw.embed = pw.major > 1 || pw.minor >= 3;
    /* object 0 is the head of the free list; the fixed ones follow */
    for ( index = 0; index < OBJ_FIRST_FREE; index++ ) {
        obj_new( &pw );
    }
    pw.image = (uint32_t *)try_alloc( (doc->nobj + 1) * sizeof( uint32_t ) );
    if ( pw.image == NULL ) {
        pw.failed = 1;
    } else {
        mem_set( pw.image, 0, (doc->nobj + 1) * sizeof( uint32_t ) );
    }
    /* the second line's four bytes above 7Fh tell a mail program that
       the file is not text */
    out_fmt( out, "%%PDF-%u.%u\n%%\xE2\xE3\xCF\xD3\n", (uint32_t)pw.major, (uint32_t)pw.minor );
    if ( !pw.failed ) {
        write_body( &pw );
    }
    if ( !pw.failed ) {
        write_notes( &pw );
    }
    if ( !pw.failed ) {
        write_tail( &pw );
    }
    out_close( pw.page );
    xfree( pw.offset );
    xfree( pw.kids );
    xfree( pw.image );
    xfree( pw.used );
    xfree( pw.mark_para );
    xfree( pw.mark_page );
    xfree( pw.mark_y );
    for ( index = 0; index < pw.nxfont; index++ ) {
        xfree( pw.xfont[index] );
    }
    for ( index = 0; index < pw.nefont; index++ ) {
        xfree( pw.efont[index]->uni );
        xfree( pw.efont[index] );
    }
    rc = pw.failed ? WE_NOMEM : out_error( out ) ? WE_IO : WE_OK;
    return rc;
}
