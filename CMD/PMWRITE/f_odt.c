/*
 * F_ODT.C - OpenDocument Text: the .odt of OpenOffice, LibreOffice and
 * Word, its template (.ott), the same as one XML file (.fodt), and
 * OpenOffice 1's .sxw.
 *
 * An .odt is a ZIP whose first member, "mimetype", says what it is.
 * The text is in content.xml; the styles with names, the page and the
 * header and footer are in styles.xml; the title and author are in
 * meta.xml.  A .fodt is all three in one document.
 *
 * READING.  A paragraph names a style, the style names a parent, and
 * what neither gives comes from the family's default style; PMWRITE's
 * paragraphs hold the answer, so each style is kept as "what it
 * gives" and folded together with its parents the first time it is
 * wanted.  Styles the writing program made up (P1, T3 - "automatic"
 * ones) are looked for first among those of the part being read, for
 * content.xml and styles.xml each have a P1 of their own.
 *
 * THE ORDER: styles.xml for its styles; content.xml for its own and
 * then the text, in one pass with no recursion; then the page layout
 * of the master page the first paragraph asks for, and styles.xml
 * once more for that master page's header and footer.  So one part is
 * in memory at a time.
 *
 * WHAT IS NOT WHAT IT SEEMS.  White space in the XML is not text:
 * runs of it are one space and a paragraph's leading space is nothing
 * (real spaces are <text:s/>).  Lengths have units and decimals, and
 * are worked out here in integers, to the nearest twip.  A header's
 * height is part of the page's top margin as PMWRITE counts it.  A
 * page break is a property of the paragraph after it, or before it.
 *
 * OPENOFFICE 1 (.sxw) uses other namespaces and a few other names
 * (style:properties for both kinds of property, text:footnote,
 * text:ordered-list, style:page-master).  They are read as the same
 * things.
 *
 * WRITING is OpenDocument 1.2: the document's styles as common styles,
 * one automatic style for each different set of paragraph properties
 * and each different set of character properties (not one a run), and
 * the parts handed to ZIP.C as they fill.
 *
 * NOT READ: a document with a password (WE_PASSWORD), drawings and
 * embedded objects other than pictures (LOST_OBJECTS), columns.
 */
#include "fmt.h"
#include "sys.h"
#include "zip.h"
#include "xml.h"

#ifdef PW_PEAK
extern u32 pw_peak_heap;        /* F_DOCX.C's */
#define PEAK()  if ( heap_in_use() > pw_peak_heap ) { pw_peak_heap = heap_in_use(); }
#else
#define PEAK()
#endif

#define MIME_ODT    "application/vnd.oasis.opendocument.text"
#define MIME_OTT    "application/vnd.oasis.opendocument.text-template"
#define MIME_SXW    "application/vnd.sun.xml.writer"

enum { NS_OFFICE = 1, NS_STYLE, NS_TEXT, NS_TABLE, NS_DRAW, NS_FO, NS_XLINK, NS_DC, NS_META, NS_SVG,
       NS_MANIFEST };

/* each as OpenDocument spells it and as OpenOffice 1 did */
static const struct {
    const char *uri;
    int id;
} odt_spaces[] = {
    { "urn:oasis:names:tc:opendocument:xmlns:office:1.0", NS_OFFICE },
    { "http://openoffice.org/2000/office", NS_OFFICE },
    { "urn:oasis:names:tc:opendocument:xmlns:style:1.0", NS_STYLE },
    { "http://openoffice.org/2000/style", NS_STYLE },
    { "urn:oasis:names:tc:opendocument:xmlns:text:1.0", NS_TEXT },
    { "http://openoffice.org/2000/text", NS_TEXT },
    { "urn:oasis:names:tc:opendocument:xmlns:table:1.0", NS_TABLE },
    { "http://openoffice.org/2000/table", NS_TABLE },
    { "urn:oasis:names:tc:opendocument:xmlns:drawing:1.0", NS_DRAW },
    { "http://openoffice.org/2000/drawing", NS_DRAW },
    { "urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0", NS_FO },
    { "http://www.w3.org/1999/XSL/Format", NS_FO },
    { "http://www.w3.org/1999/xlink", NS_XLINK },
    { "http://purl.org/dc/elements/1.1/", NS_DC },
    { "urn:oasis:names:tc:opendocument:xmlns:meta:1.0", NS_META },
    { "http://openoffice.org/2000/meta", NS_META },
    { "urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0", NS_SVG },
    { "http://www.w3.org/2000/svg", NS_SVG },
    { "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0", NS_MANIFEST },
    { "http://openoffice.org/2001/manifest", NS_MANIFEST }
};

/* ------------------------------------------------------------------ */
/* what a style gives                                                  */
/* ------------------------------------------------------------------ */

#define XC_FONT         0x01
#define XC_SIZE         0x02
#define XC_COLOR        0x04
#define XC_HILITE       0x08
#define XC_UNDER        0x10
#define XC_VERT         0x20

#define CF_TOGGLES      (CF_BOLD | CF_ITALIC | CF_STRIKE | CF_HIDDEN | CF_SMALLCAPS | \
                         CF_ALLCAPS | CF_OUTLINE | CF_SHADOW)

typedef struct {
    u16 set;                    /* XC_*: which of the members below are given */
    u16 tog_set;                /* which of CF_TOGGLES are given... */
    u16 tog;                    /* ...and whether each is on */
    u16 font, size;
    u16 under;                  /* CF_UNDER, CF_DUNDER, CF_WUNDER or 0 */
    u16 vert;                   /* CF_SUPER, CF_SUB or 0 */
    u8  hilite;
    u8  spare;
    u32 color;
} XCHP;

#define XP_ALIGN        0x0001
#define XP_LEFT         0x0002
#define XP_RIGHT        0x0004
#define XP_FIRST        0x0008
#define XP_BEFORE       0x0010
#define XP_AFTER        0x0020
#define XP_LINE         0x0040
#define XP_TABS         0x0080
#define XP_BREAK        0x0100

#define PF_GIVEN        (PF_KEEP | PF_KEEPNEXT | PF_PAGEBEFORE | PF_BORDER_TOP | PF_BORDER_BOT | \
                         PF_BORDER_LEFT | PF_BORDER_RIGHT)

typedef struct {
    u16 set;                    /* XP_* */
    u16 flag_set;               /* which of PF_GIVEN are given... */
    u16 flags;                  /* ...and whether each is on */
    u8  align, line_rule;
    u8  break_after;            /* a page ends after it */
    u8  ntabs;
    u16 before, after, line;
    s32 left, right, first;
    WTAB *tabs;                 /* a style's own block; from the paragraph's indent */
} XPAP;

enum { FAMILY_PARA = 1, FAMILY_TEXT, FAMILY_TABLE, FAMILY_COLUMN, FAMILY_CELL, FAMILY_OTHER };

#define MAX_OSTYLES     16384
#define PARENT_DEPTH    32

typedef struct {
    u32  name, parent, display, list, master;   /* strings */
    u8   family;                /* FAMILY_* */
    u8   scope;                 /* 0 a common style, 1 content.xml's own, 2 styles.xml's own */
    u8   state;                 /* 0 as read, 1 being folded, 2 folded with its parents */
    u8   is_default;            /* the family's default style */
    u8   ruled;                 /* a cell's: it has borders */
    u8   has_width;
    u16  page_number;           /* the page number it starts with, 0 for none */
    s32  width, tbl_left;       /* a column's or table's width; a table's left margin */
    int  wd;                    /* the document's style it became, -1 not yet */
    XCHP chp;
    XPAP pap;
} OSTYLE;

#define LIST_LEVELS     10

typedef struct {
    u8  fmt;                    /* LIST_* */
    u8  ind_set;                /* its indents are given */
    u8  legacy;                 /* ...as space before and label width */
    u8  spare;
    u16 start;
    s32 left, first;
} OLEVEL;

typedef struct {
    u32 name;
    u8  scope;
    u8  used;                   /* a list of this style has been met in the text */
    OLEVEL lvl[LIST_LEVELS];
} OLIST;

typedef struct {
    u32 name;
    int font;                   /* the document's font */
} OFONT;

typedef struct {
    u32 name;
    PAGESETUP page;
    s32 head_room, foot_room;   /* a header's and footer's height with its gap */
    u8  has_size, has_head, has_foot;
} OPAGE;

typedef struct {
    u32 name, layout;
    u32 next;                   /* the master page of the pages after its first */
} OMASTER;

/* the list a paragraph is being read in, at one depth */
typedef struct {
    int list;                   /* which OLIST, -1 for none named */
    u8  items;                  /* how many items it has had: 2 for "more than one" */
    u8  paras;                  /* how many paragraphs the item in hand has had */
    u8  runs_on;                /* its numbers carry on from the list before */
    u8  header;                 /* the item in hand is a list's heading */
    u16 item_start;             /* the number the item in hand says it has, 0 for none */
} OLDEPTH;

#define MAX_LIST_DEPTH  16
#define MAX_SPANS       32
#define MAX_NEST        4
#define MAX_PDEPTH      8
#define MAX_GRID        64
#define EDGE_MAX        0x00FFFFFFL
#define MAX_REPEAT      64

/* what is put by while a note's or a text box's paragraphs are read */
typedef struct {
    WBUILD wb;
    int   p_depth, nspan, span_base, nlst, lst_base, tbl_depth, space, break_after;
    int   para_style;
    int   pbase[MAX_PDEPTH];
} ONEST;

#define M_STYLES        1       /* keep the styles a part defines */
#define M_BODY          2       /* read the text */
#define M_MASTER        4       /* read the chosen master page's header and footer */
#define M_META          8       /* read the title and author */

typedef struct {
    WDOC *doc;
    ZIP  *zip;
    int   nomem, flat;
    char *str;
    u32   str_len, str_cap;
    OSTYLE *style;
    u32   nstyle, style_cap;
    OLIST *list;
    u32   nlist, list_cap;
    OFONT *font;
    u32   nfont, font_cap;
    OPAGE *page;
    u32   npage, page_cap;
    OMASTER *master;
    u32   nmaster, master_cap;
    XCHP  def_chp;              /* what a file that says nothing gets */
    XPAP  def_pap;
    int   mode;                 /* M_* */
    int   scope;                /* whose automatic styles the part in hand has: 1 or 2 */

    /* the story being read */
    WBUILD wb;
    int   story_on;
    int   p_depth;
    int   para_style;           /* the paragraph's OSTYLE, -1 for none */
    int   break_after;
    int   space;                /* the last character was white space */
    XCHP  span[MAX_SPANS];      /* the properties in force: the paragraph's, then each span's */
    int   nspan, span_base, span_over;
    int   pbase[MAX_PDEPTH];    /* where each open paragraph's own properties are in "span" */
    OLDEPTH lst[MAX_LIST_DEPTH];
    int   nlst, lst_base, lst_over;
    ONEST nest[MAX_NEST];
    int   nnest;
    int   note_obj;             /* the note whose body comes next, -1 for none */
    /* a frame */
    s32   frame_cx, frame_cy;
    int   frame_done;
    /* the outermost table */
    int   tbl_depth, in_head_rows;
    s32   tbl_left, tbl_width;
    s32   grid[MAX_GRID];
    int   ngrid, grid_fixed;
    TROW  row;
    int   row_ruled, col, covered;
    s32   edge;
    u32   cell_from;
    int   cell_span, cell_repeat;
    WPARA *cell_last;
    /* the page */
    int   seen_first;           /* the first paragraph of the text has been met */
    u32   first_master;         /* the master page it asks for: a string, 0 for none */
    u16   first_number;
    u32   want_master;          /* the master page whose header and footer are wanted */
    u32   next_master;          /* ...and the one its later pages have */
    int   in_master, head_done, foot_done;
} ODT;

#define STR( ctx, at )  ((ctx)->str + (at))

/* ------------------------------------------------------------------ */
/* small things                                                        */
/* ------------------------------------------------------------------ */

static u32 odt_str( ODT *ctx, const char *text )
{
    char *fresh;
    u32 len, want, at;

    if ( text == NULL ) {
        return 0;
    }
    len = str_len( text );
    if ( len == 0 || len > 0x00FFFFFFUL || ctx->str_len > 0x3FFFFFFFUL ) {
        return 0;
    }
    if ( len + 1 > ctx->str_cap - ctx->str_len ) {
        want = ctx->str_cap * 2;
        while ( want - ctx->str_len < len + 1 ) {
            want *= 2;
        }
        fresh = (char *)try_realloc( ctx->str, want );
        if ( fresh == NULL ) {
            ctx->nomem = 1;
            return 0;
        }
        ctx->str = fresh;
        ctx->str_cap = want;
    }
    at = ctx->str_len;
    mem_cpy( ctx->str + at, text, len + 1 );
    ctx->str_len += len + 1;
    return at;
}

static int odt_grow( ODT *ctx, void **array, u32 *cap, u32 need, u32 size, u32 limit )
{
    void *fresh;
    u32 want;

    if ( need <= *cap ) {
        return 1;
    }
    if ( need > limit ) {
        return 0;
    }
    want = *cap ? *cap * 2 : 16;
    if ( want > limit ) {
        want = limit;
    }
    fresh = try_realloc( *array, want * size );
    if ( fresh == NULL ) {
        ctx->nomem = 1;
        return 0;
    }
    *array = fresh;
    *cap = want;
    return 1;
}

static s32 odt_clamp( s32 value, s32 low, s32 high )
{
    return value < low ? low : value > high ? high : value;
}

/* value * num / den to the nearest, for numbers whose product would not fit */
static u32 odt_muldiv( u32 value, u32 num, u32 den )
{
    return (value / den) * num + ((value % den) * num + den / 2) / den;
}

static int odt_int( const char *text, s32 *out )
{
    s32 value = 0;
    int minus = 0, digits = 0;

    if ( text == NULL ) {
        return 0;
    }
    if ( *text == '-' || *text == '+' ) {
        minus = *text++ == '-';
    }
    for ( ; *text >= '0' && *text <= '9'; text++, digits++ ) {
        if ( value < 100000000L ) {
            value = value * 10 + (*text - '0');
        }
    }
    *out = minus ? -value : value;
    return digits != 0;
}

/*
 * A number with a decimal part, as ten-thousandths: "12.5" is 125000
 * (Word writes its inches to four places, and they must come back as
 * the twips they were).
 * *rest is where the number stopped.  0 if there are no digits.
 */
static int odt_number( const char *text, s32 *milli, const char **rest )
{
    u32 whole = 0, frac = 0, scale = 1000;
    int minus = 0, digits = 0;

    if ( text == NULL ) {
        return 0;
    }
    while ( *text == ' ' ) {
        text++;
    }
    if ( *text == '-' || *text == '+' ) {
        minus = *text++ == '-';
    }
    for ( ; *text >= '0' && *text <= '9'; text++, digits++ ) {
        if ( whole < 100000UL ) {
            whole = whole * 10 + (u32)(*text - '0');
        }
    }
    if ( *text == '.' ) {
        for ( text++; *text >= '0' && *text <= '9'; text++, digits++ ) {
            frac += (u32)(*text - '0') * scale;
            scale /= 10;
        }
    }
    if ( digits == 0 ) {
        return 0;
    }
    if ( whole > 100000UL ) {
        whole = 100000UL;
    }
    *milli = minus ? -(s32)(whole * 10000 + frac) : (s32)(whole * 10000 + frac);
    *rest = text;
    return 1;
}

/* a length in any of the units OpenDocument has -> twips, to the
   nearest.  0 for what is not a length (a percentage is not). */
static int odt_twips( const char *text, s32 *out )
{
    const char *unit;
    s32 milli;
    u32 size, twips;

    if ( !odt_number( text, &milli, &unit ) ) {
        return 0;
    }
    size = (u32)(milli < 0 ? -milli : milli);
    if ( unit[0] == 'p' && unit[1] == 't' ) {
        twips = odt_muldiv( size, 1, 500 );
    } else if ( unit[0] == 'i' && unit[1] == 'n' ) {
        twips = odt_muldiv( size, 18, 125 );
    } else if ( unit[0] == 'c' && unit[1] == 'm' ) {
        twips = odt_muldiv( size, 36, 635 );
    } else if ( unit[0] == 'm' && unit[1] == 'm' ) {
        twips = odt_muldiv( size, 18, 3175 );
    } else if ( unit[0] == 'p' && unit[1] == 'c' ) {
        twips = odt_muldiv( size, 3, 125 );
    } else if ( unit[0] == 'p' && unit[1] == 'x' ) {
        twips = odt_muldiv( size, 3, 2000 );
    } else if ( size == 0 ) {
        twips = 0;                      /* nothing needs no unit */
    } else {
        return 0;
    }
    *out = milli < 0 ? -(s32)twips : (s32)twips;
    return 1;
}

/* "#rrggbb" */
static int odt_color( const char *text, u32 *out )
{
    u32 value = 0;
    int index, digit;

    if ( text == NULL || text[0] != '#' ) {
        return 0;
    }
    for ( index = 1; index <= 6; index++ ) {
        digit = text[index];
        if ( digit >= '0' && digit <= '9' ) {
            digit -= '0';
        } else if ( digit >= 'a' && digit <= 'f' ) {
            digit -= 'a' - 10;
        } else if ( digit >= 'A' && digit <= 'F' ) {
            digit -= 'A' - 10;
        } else {
            return 0;
        }
        value = (value << 4) | (u32)digit;
    }
    *out = value;
    return 1;
}

/* Word's sixteen highlighters, in the order Word numbers them */
static const u32 odt_hilites[16] = {
    0x000000UL, 0x0000FFUL, 0x00FFFFUL, 0x00FF00UL, 0xFF00FFUL, 0xFF0000UL, 0xFFFF00UL, 0xFFFFFFUL,
    0x000080UL, 0x008080UL, 0x008000UL, 0x800080UL, 0x800000UL, 0x808000UL, 0x808080UL, 0xC0C0C0UL
};

/* the highlighter nearest a background colour */
static int odt_hilite( u32 rgb )
{
    s32 part, far_off, best = 0x7FFFFFFFL;
    int index, shift, nearest = 0;

    for ( index = 0; index < 16; index++ ) {
        far_off = 0;
        for ( shift = 0; shift < 24; shift += 8 ) {
            part = (s32)((rgb >> shift) & 0xFF) - (s32)((odt_hilites[index] >> shift) & 0xFF);
            far_off += part * part;
        }
        if ( far_off < best ) {
            best = far_off;
            nearest = index;
        }
    }
    return nearest + 1;
}

static int odt_sniff( const u8 *data, u32 len )
{
    if ( len >= 8 && mem_cmp( data, "\x89PNG\r\n\x1A\n", 8 ) == 0 ) {
        return IMG_PNG;
    }
    if ( len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF ) {
        return IMG_JPEG;
    }
    if ( len >= 6 && mem_cmp( data, "GIF8", 4 ) == 0 ) {
        return IMG_GIF;
    }
    if ( len >= 14 && data[0] == 'B' && data[1] == 'M' ) {
        return IMG_BMP;
    }
    if ( len >= 8 && (mem_cmp( data, "II*\0", 4 ) == 0 || mem_cmp( data, "MM\0*", 4 ) == 0) ) {
        return IMG_TIFF;
    }
    if ( len >= 44 && LE32( data ) == 1 && mem_cmp( data + 40, " EMF", 4 ) == 0 ) {
        return IMG_EMF;
    }
    if ( len >= 22 && (LE32( data ) == 0x9AC6CDD7UL || (LE16( data ) <= 2 && LE16( data + 2 ) == 9 &&
                                                          (LE16( data + 4 ) & 0xFEFF) == 0x0200)) ) {
        return IMG_WMF;
    }
    return IMG_UNKNOWN;
}

static u8 *odt_part( ODT *ctx, const char *name, u32 *len )
{
    u8 *data;
    int index = zip_find( ctx->zip, name );

    *len = 0;
    if ( index < 0 ) {
        return NULL;
    }
    data = zip_read( ctx->zip, (u32)index, len );
    if ( data == NULL && zip_error( ctx->zip ) == ZIP_NOMEM ) {
        ctx->nomem = 1;
    }
    PEAK();
    return data;
}

static XML *odt_xml( ODT *ctx, const u8 *data, u32 len )
{
    XML *xml = xml_open( data, len );
    u32 index;

    if ( xml == NULL ) {
        ctx->nomem = 1;
        return NULL;
    }
    for ( index = 0; index < sizeof( odt_spaces ) / sizeof( odt_spaces[0] ); index++ ) {
        if ( !xml_ns( xml, odt_spaces[index].uri, odt_spaces[index].id ) ) {
            ctx->nomem = 1;
            xml_close( xml );
            return NULL;
        }
    }
    return xml;
}

/* ------------------------------------------------------------------ */
/* properties: one set laid over another                               */
/* ------------------------------------------------------------------ */

static void xchp_over( XCHP *dst, const XCHP *src )
{
    if ( src->set & XC_FONT ) {
        dst->font = src->font;
    }
    if ( src->set & XC_SIZE ) {
        dst->size = src->size;
    }
    if ( src->set & XC_COLOR ) {
        dst->color = src->color;
    }
    if ( src->set & XC_HILITE ) {
        dst->hilite = src->hilite;
    }
    if ( src->set & XC_UNDER ) {
        dst->under = src->under;
    }
    if ( src->set & XC_VERT ) {
        dst->vert = src->vert;
    }
    dst->set |= src->set;
    dst->tog = (u16)((dst->tog & ~src->tog_set) | (src->tog & src->tog_set));
    dst->tog_set |= src->tog_set;
}

/* everything but the tabs: a style that has tab stops has all of its own */
static void xpap_over( XPAP *dst, const XPAP *src )
{
    if ( src->set & XP_ALIGN ) {
        dst->align = src->align;
    }
    if ( src->set & XP_LEFT ) {
        dst->left = src->left;
    }
    if ( src->set & XP_RIGHT ) {
        dst->right = src->right;
    }
    if ( src->set & XP_FIRST ) {
        dst->first = src->first;
    }
    if ( src->set & XP_BEFORE ) {
        dst->before = src->before;
    }
    if ( src->set & XP_AFTER ) {
        dst->after = src->after;
    }
    if ( src->set & XP_LINE ) {
        dst->line = src->line;
        dst->line_rule = src->line_rule;
    }
    if ( src->set & XP_BREAK ) {
        dst->break_after = src->break_after;
    }
    dst->set |= src->set;
    dst->flags = (u16)((dst->flags & ~src->flag_set) | (src->flags & src->flag_set));
    dst->flag_set |= src->flag_set;
}

static void xchp_chp( const XCHP *from, CHP *chp )
{
    chp->font = from->font;
    chp->size = from->size;
    chp->flags = (u16)((from->tog & CF_TOGGLES) | from->under | from->vert);
    chp->hilite = from->hilite;
    chp->spare = 0;
    chp->color = from->color;
}

/* ------------------------------------------------------------------ */
/* styles                                                              */
/* ------------------------------------------------------------------ */

static void odt_toggle( XCHP *chp, u16 flag, int on )
{
    chp->tog_set |= flag;
    if ( on ) {
        chp->tog |= flag;
    } else {
        chp->tog &= (u16)~flag;
    }
}

/* a font by the name a style calls it: the declared font of that name,
   or the name itself */
static int odt_font( ODT *ctx, const char *name )
{
    u32 index;

    for ( index = 0; index < ctx->nfont; index++ ) {
        if ( str_cmp( STR( ctx, ctx->font[index].name ), name ) == 0 ) {
            return ctx->font[index].font;
        }
    }
    return wd_font( ctx->doc, name, FAM_ANY, -1 );
}

/* a family as CSS writes one: the first of a list, without its quotes */
static int odt_family( ODT *ctx, const char *family, int kind, int fixed )
{
    char name[FONT_NAME_LEN];
    u32 len = 0;

    while ( *family == ' ' || *family == '\'' || *family == '"' ) {
        family++;
    }
    while ( *family && *family != '\'' && *family != '"' && *family != ',' && len < FONT_NAME_LEN - 1 ) {
        name[len++] = *family++;
    }
    name[len] = 0;
    return wd_font( ctx->doc, name, kind, fixed );
}

/* the text properties in the attributes of the element in hand */
static void odt_text_props( ODT *ctx, const XML *xml, XCHP *chp )
{
    const char *val, *type, *rest;
    s32 value;
    u32 rgb;

    val = xml_attr( xml, NS_STYLE, "font-name" );
    if ( val != NULL && val[0] ) {
        chp->font = (u16)odt_font( ctx, val );
        chp->set |= XC_FONT;
    } else if ( (val = xml_attr( xml, NS_FO, "font-family" )) != NULL && val[0] ) {
        chp->font = (u16)odt_family( ctx, val, FAM_ANY, -1 );
        chp->set |= XC_FONT;
    }
    if ( odt_twips( xml_attr( xml, NS_FO, "font-size" ), &value ) && value > 0 ) {
        chp->size = (u16)odt_clamp( (value + 5) / 10, 2, 1000 );
        chp->set |= XC_SIZE;
    }
    val = xml_attr( xml, NS_FO, "font-weight" );
    if ( val != NULL ) {
        odt_toggle( chp, CF_BOLD, str_cmp( val, "bold" ) == 0 || (odt_int( val, &value ) && value >= 600) );
    }
    val = xml_attr( xml, NS_FO, "font-style" );
    if ( val != NULL ) {
        odt_toggle( chp, CF_ITALIC, str_cmp( val, "italic" ) == 0 || str_cmp( val, "oblique" ) == 0 );
    }
    val = xml_attr( xml, NS_STYLE, "text-underline-style" );
    if ( val == NULL ) {
        val = xml_attr( xml, NS_STYLE, "text-underline" );      /* OpenOffice 1 */
    }
    if ( val != NULL ) {
        type = xml_attr( xml, NS_STYLE, "text-underline-type" );
        rest = xml_attr( xml, NS_STYLE, "text-underline-mode" );
        chp->set |= XC_UNDER;
        chp->under = (u16)(str_cmp( val, "none" ) == 0 ? 0 :
                           ((type != NULL && str_cmp( type, "double" ) == 0) || str_cmp( val, "double" ) == 0) ?
                           CF_DUNDER : (rest != NULL && str_cmp( rest, "skip-white-space" ) == 0) ? CF_WUNDER :
                           CF_UNDER);
    }
    val = xml_attr( xml, NS_STYLE, "text-line-through-style" );
    if ( val == NULL ) {
        val = xml_attr( xml, NS_STYLE, "text-crossing-out" );
    }
    if ( val != NULL ) {
        odt_toggle( chp, CF_STRIKE, str_cmp( val, "none" ) != 0 );
    }
    val = xml_attr( xml, NS_STYLE, "text-position" );
    if ( val != NULL ) {
        chp->set |= XC_VERT;
        chp->vert = 0;
        if ( str_ncmp( val, "super", 5 ) == 0 ) {
            chp->vert = CF_SUPER;
        } else if ( str_ncmp( val, "sub", 3 ) == 0 ) {
            chp->vert = CF_SUB;
        } else if ( odt_number( val, &value, &rest ) && value != 0 ) {
            chp->vert = (u16)(value > 0 ? CF_SUPER : CF_SUB);
        }
    }
    val = xml_attr( xml, NS_STYLE, "use-window-font-color" );
    if ( val != NULL && str_cmp( val, "true" ) == 0 ) {
        chp->color = COLOR_AUTO;
        chp->set |= XC_COLOR;
    } else if ( odt_color( xml_attr( xml, NS_FO, "color" ), &rgb ) ) {
        chp->color = rgb;
        chp->set |= XC_COLOR;
    }
    val = xml_attr( xml, NS_FO, "background-color" );
    if ( val == NULL ) {
        val = xml_attr( xml, NS_STYLE, "text-background-color" );
    }
    if ( val != NULL ) {
        chp->hilite = (u8)(odt_color( val, &rgb ) ? odt_hilite( rgb ) : 0);
        chp->set |= XC_HILITE;
    }
    val = xml_attr( xml, NS_FO, "font-variant" );
    if ( val != NULL ) {
        odt_toggle( chp, CF_SMALLCAPS, str_cmp( val, "small-caps" ) == 0 );
    }
    val = xml_attr( xml, NS_FO, "text-transform" );
    if ( val != NULL ) {
        odt_toggle( chp, CF_ALLCAPS, str_cmp( val, "uppercase" ) == 0 );
    }
    val = xml_attr( xml, NS_TEXT, "display" );
    if ( val != NULL ) {
        odt_toggle( chp, CF_HIDDEN, str_cmp( val, "none" ) == 0 );
    }
    val = xml_attr( xml, NS_STYLE, "text-outline" );
    if ( val != NULL ) {
        odt_toggle( chp, CF_OUTLINE, str_cmp( val, "true" ) == 0 );
    }
    val = xml_attr( xml, NS_FO, "text-shadow" );
    if ( val != NULL ) {
        odt_toggle( chp, CF_SHADOW, str_cmp( val, "none" ) != 0 );
    }
}

/* a border attribute: is there a line? */
static int odt_line( const char *val )
{
    return val != NULL && str_ncmp( val, "none", 4 ) != 0 && str_ncmp( val, "hidden", 6 ) != 0;
}

/* the paragraph properties in the attributes of the element in hand */
static void odt_para_props( ODT *ctx, const XML *xml, XPAP *pap )
{
    static const char *const sides[4] = { "border-top", "border-bottom", "border-left", "border-right" };
    static const u16 bits[4] = { PF_BORDER_TOP, PF_BORDER_BOT, PF_BORDER_LEFT, PF_BORDER_RIGHT };
    const char *val, *rest;
    s32 value;
    int index;

    val = xml_attr( xml, NS_FO, "text-align" );
    if ( val != NULL ) {
        pap->align = (u8)(str_cmp( val, "center" ) == 0 ? AL_CENTER :
                          (str_cmp( val, "end" ) == 0 || str_cmp( val, "right" ) == 0) ? AL_RIGHT :
                          str_cmp( val, "justify" ) == 0 ? AL_JUSTIFY : AL_LEFT);
        pap->set |= XP_ALIGN;
    }
    if ( odt_twips( xml_attr( xml, NS_FO, "margin" ), &value ) ) {
        pap->left = pap->right = odt_clamp( value, -31680, 31680 );
        pap->before = pap->after = (u16)odt_clamp( value, 0, 31680 );
        pap->set |= XP_LEFT | XP_RIGHT | XP_BEFORE | XP_AFTER;
    }
    if ( odt_twips( xml_attr( xml, NS_FO, "margin-left" ), &value ) ) {
        pap->left = odt_clamp( value, -31680, 31680 );
        pap->set |= XP_LEFT;
    }
    if ( odt_twips( xml_attr( xml, NS_FO, "margin-right" ), &value ) ) {
        pap->right = odt_clamp( value, -31680, 31680 );
        pap->set |= XP_RIGHT;
    }
    if ( odt_twips( xml_attr( xml, NS_FO, "text-indent" ), &value ) ) {
        pap->first = odt_clamp( value, -31680, 31680 );
        pap->set |= XP_FIRST;
    }
    if ( odt_twips( xml_attr( xml, NS_FO, "margin-top" ), &value ) ) {
        pap->before = (u16)odt_clamp( value, 0, 31680 );
        pap->set |= XP_BEFORE;
    }
    if ( odt_twips( xml_attr( xml, NS_FO, "margin-bottom" ), &value ) ) {
        pap->after = (u16)odt_clamp( value, 0, 31680 );
        pap->set |= XP_AFTER;
    }
    /* the three ways a line's height is said */
    val = xml_attr( xml, NS_FO, "line-height" );
    if ( val != NULL && str_cmp( val, "normal" ) == 0 ) {
        pap->line = 240;
        pap->line_rule = LS_MULTIPLE;
        pap->set |= XP_LINE;
    } else if ( val != NULL && odt_number( val, &value, &rest ) && value > 0 ) {
        if ( rest[0] == '%' ) {
            pap->line = (u16)odt_clamp( (s32)odt_muldiv( (u32)value, 12, 50000UL ), 1, 31680 );
            pap->line_rule = LS_MULTIPLE;
            pap->set |= XP_LINE;
        } else if ( odt_twips( val, &value ) && value > 0 ) {
            pap->line = (u16)odt_clamp( value, 1, 31680 );
            pap->line_rule = LS_EXACT;
            pap->set |= XP_LINE;
        }
    }
    if ( odt_twips( xml_attr( xml, NS_STYLE, "line-height-at-least" ), &value ) && value > 0 ) {
        pap->line = (u16)odt_clamp( value, 1, 31680 );
        pap->line_rule = LS_ATLEAST;
        pap->set |= XP_LINE;
    }
    val = xml_attr( xml, NS_FO, "keep-with-next" );
    if ( val != NULL ) {
        pap->flag_set |= PF_KEEPNEXT;
        pap->flags = (u16)(str_cmp( val, "always" ) == 0 ? pap->flags | PF_KEEPNEXT : pap->flags & ~PF_KEEPNEXT);
    }
    val = xml_attr( xml, NS_FO, "keep-together" );
    if ( val != NULL ) {
        pap->flag_set |= PF_KEEP;
        pap->flags = (u16)(str_cmp( val, "always" ) == 0 ? pap->flags | PF_KEEP : pap->flags & ~PF_KEEP);
    }
    val = xml_attr( xml, NS_FO, "break-before" );
    if ( val != NULL ) {
        pap->flag_set |= PF_PAGEBEFORE;
        pap->flags = (u16)((str_cmp( val, "page" ) == 0 || str_cmp( val, "column" ) == 0) ?
                           pap->flags | PF_PAGEBEFORE : pap->flags & ~PF_PAGEBEFORE);
    }
    val = xml_attr( xml, NS_FO, "break-after" );
    if ( val != NULL ) {
        pap->break_after = (u8)(str_cmp( val, "page" ) == 0 || str_cmp( val, "column" ) == 0);
        pap->set |= XP_BREAK;
    }
    val = xml_attr( xml, NS_FO, "border" );
    if ( val != NULL ) {
        pap->flag_set |= PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT;
        pap->flags &= (u16)~(PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT);
        if ( odt_line( val ) ) {
            pap->flags |= PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT;
        }
    }
    for ( index = 0; index < 4; index++ ) {
        val = xml_attr( xml, NS_FO, sides[index] );
        if ( val != NULL ) {
            pap->flag_set |= bits[index];
            pap->flags = (u16)(odt_line( val ) ? pap->flags | bits[index] : pap->flags & ~bits[index]);
        }
    }
    if ( odt_twips( xml_attr( xml, NS_STYLE, "tab-stop-distance" ), &value ) && value > 0 && ctx->doc != NULL ) {
        ctx->doc->deftab = odt_clamp( value, 20, 31680 );
    }
}

/* the tab stops inside a paragraph-properties element, which is in hand */
static void odt_tab_stops( XML *xml, XPAP *pap, WTAB *tabs )
{
    const char *val;
    WTAB *tab;
    s32 value;
    int depth = xml->depth, inner;

    while ( xml_child( xml, depth ) ) {
        if ( !xml_is( xml, NS_STYLE, "tab-stops" ) ) {
            continue;
        }
        pap->set |= XP_TABS;
        pap->ntabs = 0;
        inner = xml->depth;
        while ( xml_child( xml, inner ) ) {
            if ( !xml_is( xml, NS_STYLE, "tab-stop" ) || pap->ntabs >= MAX_TABS ||
                 !odt_twips( xml_attr( xml, NS_STYLE, "position" ), &value ) ) {
                continue;
            }
            tab = &tabs[pap->ntabs++];
            mem_set( tab, 0, sizeof( WTAB ) );
            tab->pos = odt_clamp( value, -31680, 31680 );
            val = xml_attr( xml, NS_STYLE, "type" );
            if ( val != NULL ) {
                tab->kind = (u8)(str_cmp( val, "center" ) == 0 ? TAB_CENTER : str_cmp( val, "right" ) == 0 ? TAB_RIGHT :
                                 str_cmp( val, "char" ) == 0 ? TAB_DECIMAL : TAB_LEFT);
            }
            val = xml_attr( xml, NS_STYLE, "leader-style" );
            if ( val != NULL && str_cmp( val, "none" ) != 0 ) {
                tab->lead = (u8)(str_cmp( val, "dotted" ) == 0 ? LEAD_DOTS :
                                 (str_cmp( val, "dash" ) == 0 || str_cmp( val, "long-dash" ) == 0) ? LEAD_DASHES :
                                 LEAD_LINE);
                val = xml_attr( xml, NS_STYLE, "leader-text" );
                if ( val != NULL && val[0] == '.' ) {
                    tab->lead = LEAD_DOTS;
                } else if ( val != NULL && val[0] == '-' ) {
                    tab->lead = LEAD_DASHES;
                } else if ( val != NULL && val[0] == '_' ) {
                    tab->lead = LEAD_LINE;
                }
            } else if ( (val = xml_attr( xml, NS_STYLE, "leader-char" )) != NULL && val[0] > ' ' ) {
                tab->lead = (u8)(val[0] == '.' ? LEAD_DOTS : val[0] == '-' ? LEAD_DASHES : LEAD_LINE);
            }
        }
    }
}

static void odt_style_tabs( ODT *ctx, OSTYLE *style, const WTAB *tabs, int count )
{
    WTAB *fresh = NULL;

    if ( count > 0 ) {
        fresh = (WTAB *)try_alloc( (u32)count * sizeof( WTAB ) );
        if ( fresh == NULL ) {
            ctx->nomem = 1;
            count = 0;
        } else {
            mem_cpy( fresh, tabs, (u32)count * sizeof( WTAB ) );
        }
    }
    xfree( style->pap.tabs );
    style->pap.tabs = fresh;
    style->pap.ntabs = (u8)count;
}

/* a <style:style> or <style:default-style>, kept */
static void odt_style_read( ODT *ctx, XML *xml, int scope, int is_default )
{
    WTAB tabs[MAX_TABS];
    XPAP stops;
    OSTYLE *style;
    const char *val;
    s32 value;
    int depth = xml->depth, both;

    if ( !odt_grow( ctx, (void **)&ctx->style, &ctx->style_cap, ctx->nstyle + 1, sizeof( OSTYLE ), MAX_OSTYLES ) ) {
        return;
    }
    style = &ctx->style[ctx->nstyle];
    mem_set( style, 0, sizeof( OSTYLE ) );
    style->wd = -1;
    style->scope = (u8)scope;
    style->is_default = (u8)is_default;
    val = xml_attr( xml, NS_STYLE, "family" );
    style->family = (u8)(val == NULL ? FAMILY_OTHER : str_cmp( val, "paragraph" ) == 0 ? FAMILY_PARA :
                         str_cmp( val, "text" ) == 0 ? FAMILY_TEXT : str_cmp( val, "table" ) == 0 ? FAMILY_TABLE :
                         str_cmp( val, "table-column" ) == 0 ? FAMILY_COLUMN :
                         str_cmp( val, "table-cell" ) == 0 ? FAMILY_CELL : FAMILY_OTHER);
    ctx->nstyle++;
    /* the strings may move as they are added; the style does not */
    style->name = odt_str( ctx, xml_attr( xml, NS_STYLE, "name" ) );
    style->parent = odt_str( ctx, xml_attr( xml, NS_STYLE, "parent-style-name" ) );
    style->display = odt_str( ctx, xml_attr( xml, NS_STYLE, "display-name" ) );
    style->list = odt_str( ctx, xml_attr( xml, NS_STYLE, "list-style-name" ) );
    style->master = odt_str( ctx, xml_attr( xml, NS_STYLE, "master-page-name" ) );
    while ( xml_child( xml, depth ) ) {
        if ( xml->ns != NS_STYLE ) {
            continue;
        }
        both = xml_is( xml, NS_STYLE, "properties" );   /* OpenOffice 1: one element for all */
        if ( both || xml_is( xml, NS_STYLE, "text-properties" ) ) {
            odt_text_props( ctx, xml, &style->chp );
        }
        if ( both || xml_is( xml, NS_STYLE, "table-properties" ) ) {
            if ( style->family == FAMILY_TABLE ) {
                if ( odt_twips( xml_attr( xml, NS_STYLE, "width" ), &value ) && value > 0 ) {
                    style->width = value;
                    style->has_width = 1;
                }
                if ( odt_twips( xml_attr( xml, NS_FO, "margin-left" ), &value ) ) {
                    style->tbl_left = odt_clamp( value, -31680, 31680 );
                }
            }
        }
        if ( both || xml_is( xml, NS_STYLE, "table-column-properties" ) ) {
            if ( style->family == FAMILY_COLUMN && odt_twips( xml_attr( xml, NS_STYLE, "column-width" ), &value ) &&
                 value > 0 ) {
                style->width = value;
                style->has_width = 1;
            }
        }
        if ( both || xml_is( xml, NS_STYLE, "table-cell-properties" ) ) {
            if ( style->family == FAMILY_CELL &&
                 (odt_line( xml_attr( xml, NS_FO, "border" ) ) || odt_line( xml_attr( xml, NS_FO, "border-top" ) ) ||
                  odt_line( xml_attr( xml, NS_FO, "border-left" ) ) ||
                  odt_line( xml_attr( xml, NS_FO, "border-bottom" ) ) ||
                  odt_line( xml_attr( xml, NS_FO, "border-right" ) )) ) {
                style->ruled = 1;
            }
        }
        if ( both || xml_is( xml, NS_STYLE, "paragraph-properties" ) ) {
            if ( style->family != FAMILY_PARA ) {
                continue;
            }
            odt_para_props( ctx, xml, &style->pap );
            if ( odt_int( xml_attr( xml, NS_STYLE, "page-number" ), &value ) && value > 0 ) {
                style->page_number = (u16)odt_clamp( value, 1, 65535L );
            }
            mem_set( &stops, 0, sizeof( stops ) );
            odt_tab_stops( xml, &stops, tabs );
            if ( stops.set & XP_TABS ) {
                style->pap.set |= XP_TABS;
                odt_style_tabs( ctx, style, tabs, stops.ntabs );
            }
        }
    }
}

static void odt_styles_free( ODT *ctx )
{
    u32 index;

    for ( index = 0; index < ctx->nstyle; index++ ) {
        xfree( ctx->style[index].pap.tabs );
    }
    xfree( ctx->style );
    ctx->style = NULL;
    ctx->nstyle = 0;
}

/* a style by family and name: one of the part's own first, then a
   common one.  -1 for none. */
static int odt_style_find( const ODT *ctx, int family, const char *name, int scope )
{
    u32 index;
    int common = -1;

    if ( name == NULL || name[0] == 0 ) {
        return -1;
    }
    for ( index = 0; index < ctx->nstyle; index++ ) {
        if ( ctx->style[index].family != family || ctx->style[index].is_default ||
             str_cmp( STR( ctx, ctx->style[index].name ), name ) != 0 ) {
            continue;
        }
        if ( ctx->style[index].scope == scope ) {
            return (int)index;
        }
        if ( ctx->style[index].scope == 0 && common < 0 ) {
            common = (int)index;
        }
    }
    return common;
}

/* a style folded together with its parents */
static void odt_style_fold( ODT *ctx, int index, int depth )
{
    OSTYLE *style = &ctx->style[index], *base;
    XCHP chp;
    XPAP pap;
    WTAB *own;
    int from, own_count, had_tabs;

    if ( style->state ) {
        return;
    }
    style->state = 1;
    from = style->parent ? odt_style_find( ctx, style->family, STR( ctx, style->parent ), 0 ) : -1;
    if ( from >= 0 && from != index && depth < PARENT_DEPTH ) {
        odt_style_fold( ctx, from, depth + 1 );
        base = &ctx->style[from];
        if ( base->state == 2 ) {       /* not one this chain has come back round to */
            chp = base->chp;
            xchp_over( &chp, &style->chp );
            style->chp = chp;
            own = style->pap.tabs;
            own_count = style->pap.ntabs;
            had_tabs = style->pap.set & XP_TABS;
            pap = base->pap;
            xpap_over( &pap, &style->pap );
            style->pap = pap;
            style->pap.tabs = own;
            style->pap.ntabs = (u8)own_count;
            if ( !had_tabs ) {
                /* no tab stops of its own: its parent's */
                odt_style_tabs( ctx, style, base->pap.tabs, base->pap.ntabs );
            }
            if ( !style->ruled ) {
                style->ruled = base->ruled;
            }
            if ( !style->has_width && base->has_width ) {
                style->width = base->width;
                style->has_width = 1;
            }
            if ( style->list == 0 ) {
                style->list = base->list;
            }
        }
    }
    style->state = 2;
}

/* a name as a user sees it: "Heading_20_1" is "Heading 1" */
static void odt_display( const char *name, char *out, u32 max )
{
    u32 used = 0, code;
    int digit, index;

    while ( *name && used < max - 1 ) {
        if ( name[0] == '_' && name[1] && name[2] && name[3] == '_' ) {
            code = 0;
            for ( index = 1; index <= 2; index++ ) {
                digit = name[index];
                digit = digit >= '0' && digit <= '9' ? digit - '0' : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 :
                        digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : 99;
                code = code * 16 + (u32)digit;
            }
            if ( code >= 0x20 && code < 0x7F ) {
                out[used++] = (char)code;
                name += 4;
                continue;
            }
        }
        out[used++] = *name++;
    }
    out[used] = 0;
}

/* which of the document's own kinds of style a name is */
static int odt_style_kind( const char *name, const char **as )
{
    static const struct {
        const char *name, *as;
        int kind;
    } known[] = {
        { "Standard", "Normal", STY_NORMAL }, { "Normal", "Normal", STY_NORMAL },
        { "Default Paragraph Style", "Normal", STY_NORMAL }, { "Default Style", "Normal", STY_NORMAL },
        { "Heading 1", "Heading 1", STY_HEADING1 }, { "Heading 2", "Heading 2", STY_HEADING2 },
        { "Heading 3", "Heading 3", STY_HEADING3 }, { "Heading 4", "Heading 4", STY_HEADING4 },
        { "Heading 5", "Heading 5", STY_HEADING5 }, { "Heading 6", "Heading 6", STY_HEADING6 },
        { "Title", "Title", STY_TITLE }, { "Subtitle", "Subtitle", STY_SUBTITLE },
        { "Header", "Header", STY_HEADER }, { "Footer", "Footer", STY_FOOTER },
        { "Footnote", "Footnote Text", STY_FOOTNOTE }, { "Footnote Text", "Footnote Text", STY_FOOTNOTE }
    };
    u32 index;

    for ( index = 0; index < sizeof( known ) / sizeof( known[0] ); index++ ) {
        if ( str_icmp( name, known[index].name ) == 0 ) {
            *as = known[index].as;
            return known[index].kind;
        }
    }
    *as = name;
    return STY_OTHER;
}

/* the look a paragraph of a style starts with: the defaults, and the
   style with its parents over them */
static void odt_look( ODT *ctx, int index, XPAP *pap, XCHP *chp )
{
    *pap = ctx->def_pap;
    *chp = ctx->def_chp;
    pap->tabs = NULL;
    pap->ntabs = 0;
    if ( index >= 0 ) {
        odt_style_fold( ctx, index, 0 );
        xpap_over( pap, &ctx->style[index].pap );
        xchp_over( chp, &ctx->style[index].chp );
        pap->tabs = ctx->style[index].pap.tabs;
        pap->ntabs = ctx->style[index].pap.ntabs;
    }
}

/*
 * The document's style for one of the file's paragraph styles: an
 * automatic style is of the common style it comes from.  "heading" is
 * 1 to 6 for a <text:h> of that level, which makes a style nobody
 * knows by name a heading all the same.
 */
static int odt_wd_style( ODT *ctx, int index, int heading )
{
    char shown[STYLE_NAME_LEN + 8];
    OSTYLE *style;
    WSTYLE *made;
    const char *name;
    XPAP pap;
    XCHP chp;
    int kind, depth;

    for ( depth = 0; index >= 0 && ctx->style[index].scope != 0 && depth < PARENT_DEPTH; depth++ ) {
        index = ctx->style[index].parent ?
                odt_style_find( ctx, FAMILY_PARA, STR( ctx, ctx->style[index].parent ), 0 ) : -1;
    }
    if ( index < 0 || ctx->style[index].scope != 0 ) {
        return 0;
    }
    style = &ctx->style[index];
    if ( style->wd >= 0 ) {
        return style->wd;
    }
    if ( style->display ) {
        str_cpyn( shown, STR( ctx, style->display ), sizeof( shown ) );
    } else {
        odt_display( STR( ctx, style->name ), shown, sizeof( shown ) );
    }
    kind = odt_style_kind( shown, &name );
    if ( kind == STY_OTHER && heading >= 1 && heading <= 6 ) {
        kind = STY_HEADING1 + heading - 1;
    }
    style->wd = wd_style( ctx->doc, name[0] ? name : "Style", kind );
    if ( style->wd == 0 && kind != STY_NORMAL ) {
        return 0;
    }
    made = &ctx->doc->style[style->wd];
    odt_look( ctx, index, &pap, &chp );
    xchp_chp( &chp, &made->chp );
    made->pap.align = pap.align;
    made->pap.left = pap.left;
    made->pap.right = pap.right;
    made->pap.first = pap.first;
    made->pap.before = pap.before;
    made->pap.after = pap.after;
    made->pap.line = pap.line;
    made->pap.line_rule = pap.line_rule;
    made->pap.flags = (u16)(pap.flags & PF_GIVEN);
    return style->wd;
}

/* <style:font-face>, <text:list-style>, <style:page-layout>, <style:master-page> */

static void odt_font_read( ODT *ctx, const XML *xml )
{
    const char *name = xml_attr( xml, NS_STYLE, "name" ), *family, *generic, *pitch;
    int kind = FAM_ANY;

    family = xml_attr( xml, NS_SVG, "font-family" );
    if ( family == NULL ) {
        family = xml_attr( xml, NS_FO, "font-family" );
    }
    if ( name == NULL || !odt_grow( ctx, (void **)&ctx->font, &ctx->font_cap, ctx->nfont + 1, sizeof( OFONT ), 4096 ) ) {
        return;
    }
    generic = xml_attr( xml, NS_STYLE, "font-family-generic" );
    pitch = xml_attr( xml, NS_STYLE, "font-pitch" );
    if ( generic != NULL ) {
        kind = str_cmp( generic, "roman" ) == 0 ? FAM_ROMAN : str_cmp( generic, "swiss" ) == 0 ? FAM_SWISS :
               str_cmp( generic, "modern" ) == 0 ? FAM_MODERN : str_cmp( generic, "script" ) == 0 ? FAM_SCRIPT :
               str_cmp( generic, "decorative" ) == 0 ? FAM_DECOR : FAM_ANY;
    }
    ctx->font[ctx->nfont].font = odt_family( ctx, family != NULL && family[0] ? family : name, kind,
                                             pitch == NULL ? -1 : str_cmp( pitch, "fixed" ) == 0 );
    ctx->font[ctx->nfont].name = odt_str( ctx, name );
    ctx->nfont++;
}

static void odt_list_read( ODT *ctx, XML *xml, int scope )
{
    OLIST *list;
    OLEVEL *level;
    const char *val;
    s32 value, before, width;
    int depth = xml->depth, inner, deeper, index, bullet;

    if ( !odt_grow( ctx, (void **)&ctx->list, &ctx->list_cap, ctx->nlist + 1, sizeof( OLIST ), 4096 ) ) {
        return;
    }
    list = &ctx->list[ctx->nlist];
    mem_set( list, 0, sizeof( OLIST ) );
    list->scope = (u8)scope;
    for ( index = 0; index < LIST_LEVELS; index++ ) {
        list->lvl[index].fmt = LIST_BULLET;
        list->lvl[index].start = 1;
    }
    ctx->nlist++;
    list->name = odt_str( ctx, xml_attr( xml, NS_STYLE, "name" ) );
    while ( xml_child( xml, depth ) ) {
        bullet = xml_is( xml, NS_TEXT, "list-level-style-bullet" ) || xml_is( xml, NS_TEXT, "list-level-style-image" );
        if ( (!bullet && !xml_is( xml, NS_TEXT, "list-level-style-number" )) ||
             !odt_int( xml_attr( xml, NS_TEXT, "level" ), &value ) || value < 1 || value > LIST_LEVELS ) {
            continue;
        }
        level = &list->lvl[value - 1];
        level->fmt = LIST_BULLET;
        if ( !bullet ) {
            val = xml_attr( xml, NS_STYLE, "num-format" );
            level->fmt = (u8)(val == NULL || val[0] == 0 ? LIST_NONE : val[0] == 'a' ? LIST_LOWER :
                              val[0] == 'A' ? LIST_UPPER : val[0] == 'i' ? LIST_ROMAN_LOWER :
                              val[0] == 'I' ? LIST_ROMAN_UPPER : LIST_NUMBER);
            if ( odt_int( xml_attr( xml, NS_TEXT, "start-value" ), &value ) ) {
                level->start = (u16)odt_clamp( value, 0, 65535L );
            }
        }
        inner = xml->depth;
        while ( xml_child( xml, inner ) ) {
            if ( !xml_is( xml, NS_STYLE, "list-level-properties" ) && !xml_is( xml, NS_STYLE, "properties" ) ) {
                continue;
            }
            before = width = 0;
            if ( odt_twips( xml_attr( xml, NS_TEXT, "space-before" ), &before ) |
                 odt_twips( xml_attr( xml, NS_TEXT, "min-label-width" ), &width ) ) {
                level->left = odt_clamp( before + width, -31680, 31680 );
                level->first = -odt_clamp( width, 0, 31680 );
                level->ind_set = level->legacy = 1;
            }
            deeper = xml->depth;
            while ( xml_child( xml, deeper ) ) {
                if ( xml_is( xml, NS_STYLE, "list-level-label-alignment" ) ) {
                    if ( odt_twips( xml_attr( xml, NS_FO, "margin-left" ), &value ) ) {
                        level->left = odt_clamp( value, -31680, 31680 );
                        level->ind_set = 1;
                        level->legacy = 0;
                    }
                    if ( odt_twips( xml_attr( xml, NS_FO, "text-indent" ), &value ) ) {
                        level->first = odt_clamp( value, -31680, 31680 );
                        level->ind_set = 1;
                        level->legacy = 0;
                    }
                }
            }
        }
    }
}

static int odt_list_find( const ODT *ctx, const char *name, int scope )
{
    u32 index;
    int common = -1;

    for ( index = 0; name != NULL && name[0] && index < ctx->nlist; index++ ) {
        if ( str_cmp( STR( ctx, ctx->list[index].name ), name ) != 0 ) {
            continue;
        }
        if ( ctx->list[index].scope == scope ) {
            return (int)index;
        }
        if ( ctx->list[index].scope == 0 && common < 0 ) {
            common = (int)index;
        }
    }
    return common;
}

static void odt_page_read( ODT *ctx, XML *xml )
{
    OPAGE *page;
    const char *val;
    s32 value, room;
    int depth = xml->depth, inner, head, both;

    if ( !odt_grow( ctx, (void **)&ctx->page, &ctx->page_cap, ctx->npage + 1, sizeof( OPAGE ), 4096 ) ) {
        return;
    }
    page = &ctx->page[ctx->npage];
    mem_set( page, 0, sizeof( OPAGE ) );
    page->page = ctx->doc->page;
    page->page.landscape = 0;
    ctx->npage++;
    page->name = odt_str( ctx, xml_attr( xml, NS_STYLE, "name" ) );
    while ( xml_child( xml, depth ) ) {
        both = xml_is( xml, NS_STYLE, "properties" );
        if ( both || xml_is( xml, NS_STYLE, "page-layout-properties" ) ) {
            if ( odt_twips( xml_attr( xml, NS_FO, "page-width" ), &value ) && value >= 720 && value <= 63360L ) {
                page->page.width = value;
                page->has_size |= 1;
            }
            if ( odt_twips( xml_attr( xml, NS_FO, "page-height" ), &value ) && value >= 720 && value <= 63360L ) {
                page->page.height = value;
                page->has_size |= 2;
            }
            val = xml_attr( xml, NS_STYLE, "print-orientation" );
            page->page.landscape = (u8)(val != NULL && str_cmp( val, "landscape" ) == 0);
            if ( odt_twips( xml_attr( xml, NS_FO, "margin" ), &value ) ) {
                page->page.top = page->page.bottom = page->page.left = page->page.right = odt_clamp( value, 0, 31680 );
            }
            if ( odt_twips( xml_attr( xml, NS_FO, "margin-top" ), &value ) ) {
                page->page.top = odt_clamp( value, 0, 31680 );
            }
            if ( odt_twips( xml_attr( xml, NS_FO, "margin-bottom" ), &value ) ) {
                page->page.bottom = odt_clamp( value, 0, 31680 );
            }
            if ( odt_twips( xml_attr( xml, NS_FO, "margin-left" ), &value ) ) {
                page->page.left = odt_clamp( value, 0, 31680 );
            }
            if ( odt_twips( xml_attr( xml, NS_FO, "margin-right" ), &value ) ) {
                page->page.right = odt_clamp( value, 0, 31680 );
            }
            continue;
        }
        head = xml_is( xml, NS_STYLE, "header-style" );
        if ( !head && !xml_is( xml, NS_STYLE, "footer-style" ) ) {
            continue;
        }
        /* how much room the header or footer takes beyond the margin:
           its height, which has the gap between it and the text in it */
        inner = xml->depth;
        while ( xml_child( xml, inner ) ) {
            if ( xml->ns != NS_STYLE ) {
                continue;
            }
            room = 0;
            if ( odt_twips( xml_attr( xml, NS_FO, "min-height" ), &value ) ||
                 odt_twips( xml_attr( xml, NS_SVG, "height" ), &value ) ) {
                room += odt_clamp( value, 0, 31680 );
            }
            page = &ctx->page[ctx->npage - 1];
            if ( head ) {
                page->head_room = room;
                page->has_head = 1;
            } else {
                page->foot_room = room;
                page->has_foot = 1;
            }
        }
    }
}

/* the children of <office:styles> or <office:automatic-styles> */
static void odt_styles_in( ODT *ctx, XML *xml, int scope )
{
    int depth = xml->depth;

    while ( xml_child( xml, depth ) ) {
        if ( xml_is( xml, NS_STYLE, "style" ) ) {
            odt_style_read( ctx, xml, scope, 0 );
        } else if ( xml_is( xml, NS_STYLE, "default-style" ) ) {
            odt_style_read( ctx, xml, 0, 1 );
        } else if ( xml_is( xml, NS_TEXT, "list-style" ) ) {
            odt_list_read( ctx, xml, scope );
        } else if ( xml_is( xml, NS_STYLE, "page-layout" ) || xml_is( xml, NS_STYLE, "page-master" ) ) {
            odt_page_read( ctx, xml );
        }
    }
}

/* the defaults a paragraph starts from: what the format says when a
   file says nothing, then the file's default styles */
static void odt_defaults( ODT *ctx )
{
    u32 index;

    for ( index = 0; index < ctx->nstyle; index++ ) {
        if ( ctx->style[index].is_default && ctx->style[index].family == FAMILY_PARA ) {
            xchp_over( &ctx->def_chp, &ctx->style[index].chp );
            xpap_over( &ctx->def_pap, &ctx->style[index].pap );
        }
    }
}

/* ------------------------------------------------------------------ */
/* the text                                                            */
/* ------------------------------------------------------------------ */

static void odt_chp_now( ODT *ctx )
{
    xchp_chp( &ctx->span[ctx->nspan - 1], &ctx->wb.chp );
}

static void odt_put( ODT *ctx, u32 code )
{
    if ( ctx->story_on ) {
        wb_char( &ctx->wb, code );
        ctx->space = 0;
    }
}

/* characters of the XML: white space is not text until it is one space
   between words */
static void odt_text( ODT *ctx, const u8 *text, u32 len )
{
    u16 run[256];
    const u8 *end = text + len;
    u32 used = 0, code;

    while ( text < end ) {
        code = utf8_get( &text, end );
        if ( code == ' ' || code == '\t' || code == '\n' || code == '\r' ) {
            if ( ctx->space ) {
                continue;
            }
            ctx->space = 1;
            code = ' ';
        } else if ( code < 0x20 ) {
            continue;
        } else {
            ctx->space = 0;
            if ( WCH_IS_FIELD( code ) || code == WCH_OBJ ) {
                code = 0xFFFD;
            }
        }
        if ( code >= 0x10000UL ) {
            code -= 0x10000UL;
            run[used++] = (u16)(0xD800 + (code >> 10));
            code = 0xDC00 + (code & 0x3FF);
        }
        run[used++] = (u16)code;
        if ( used >= sizeof( run ) / sizeof( run[0] ) - 2 ) {
            wb_text( &ctx->wb, run, used );
            used = 0;
        }
    }
    wb_text( &ctx->wb, run, used );
}

static WPARA *odt_last_para( ODT *ctx )
{
    STORY *story = ctx->wb.story;

    return story->count ? story->para[story->count - 1] : NULL;
}

/* a paragraph starts: its properties worked out into the builder */
static void odt_para_begin( ODT *ctx, const XML *xml, int heading )
{
    WTAB tabs[MAX_TABS];
    XPAP pap;
    XCHP chp;
    PAP *out = &ctx->wb.pap;
    const OSTYLE *style = NULL;
    const OLEVEL *level;
    OLDEPTH *at;
    s32 value;
    int index, list = -1, depth, count;

    if ( ctx->p_depth > 0 && ctx->wb.para != NULL ) {
        wb_par( &ctx->wb );             /* a paragraph in a paragraph: the outer one's start stands alone */
    }
    /* the paragraph is in the story from its start, so that what is
       anchored in it - a text box's paragraphs - comes after it */
    if ( ctx->wb.para == NULL ) {
        ctx->wb.para = wd_para_add( ctx->doc, ctx->wb.story, NULL );
    }
    index = odt_style_find( ctx, FAMILY_PARA, xml_attr( xml, NS_TEXT, "style-name" ), ctx->scope );
    odt_look( ctx, index, &pap, &chp );
    if ( index >= 0 ) {
        style = &ctx->style[index];
    }
    ctx->para_style = index;
    if ( heading && odt_int( xml_attr( xml, NS_TEXT, "outline-level" ), &value ) ) {
        heading = (int)odt_clamp( value, 1, 6 );
    }
    out->style = (u8)odt_wd_style( ctx, index, heading );
    out->list = LIST_NONE;
    out->level = 0;
    out->list_start = 0;
    /* in a list: the level's kind and number, and its indents unless
       the paragraph's style has its own */
    depth = ctx->nlst - ctx->lst_base;
    if ( depth > 0 ) {
        at = &ctx->lst[ctx->nlst - 1];
        for ( count = ctx->nlst; count > ctx->lst_base && list < 0; count-- ) {
            list = ctx->lst[count - 1].list;
        }
        if ( list < 0 && style != NULL && style->list ) {
            list = odt_list_find( ctx, STR( ctx, style->list ), ctx->scope );
        }
        if ( list >= 0 ) {
            level = &ctx->list[list].lvl[depth <= LIST_LEVELS ? depth - 1 : LIST_LEVELS - 1];
            if ( level->ind_set ) {
                if ( level->legacy ) {
                    pap.left += level->left;
                    pap.first = level->first;
                } else {
                    if ( style == NULL || !(style->pap.set & XP_LEFT) ) {
                        pap.left = level->left;
                    }
                    if ( style == NULL || !(style->pap.set & XP_FIRST) ) {
                        pap.first = level->first;
                    }
                }
            }
            if ( at->paras == 0 && !at->header && level->fmt != LIST_NONE ) {
                out->list = level->fmt;
                out->level = (u8)(depth <= LIST_LEVELS ? depth - 1 : LIST_LEVELS - 1);
                if ( level->fmt != LIST_BULLET ) {
                    out->list_start = (u16)(at->item_start ? at->item_start :
                                            at->items > 1 ? 0 : (depth == 1 && at->runs_on) ? 0 : level->start);
                }
            } else if ( at->paras != 0 || at->header ) {
                pap.first = 0;          /* more of an item: beside its text, with no number */
            }
        }
        if ( at->paras < 2 ) {
            at->paras++;
        }
    }
    out->align = pap.align;
    out->left = pap.left;
    out->right = pap.right;
    out->first = pap.first;
    out->before = pap.before;
    out->after = pap.after;
    out->line = pap.line;
    out->line_rule = pap.line_rule;
    out->flags = (u16)(pap.flags & PF_GIVEN);
    /* a file's tab stops are counted from the paragraph's indent */
    count = pap.ntabs <= MAX_TABS ? pap.ntabs : MAX_TABS;
    for ( index = 0; index < count; index++ ) {
        tabs[index] = pap.tabs[index];
        tabs[index].pos += pap.left;
    }
    if ( !pap_set_tabs( out, tabs, count ) ) {
        ctx->nomem = 1;
    }
    ctx->break_after = pap.break_after;
    /* the first paragraph of the text says which page it is on */
    if ( (ctx->mode & M_BODY) && !ctx->seen_first && ctx->nnest == 0 ) {
        ctx->seen_first = 1;
        if ( style != NULL ) {
            ctx->first_master = style->master;
            ctx->first_number = style->page_number;
        }
        out->flags &= (u16)~PF_PAGEBEFORE;      /* a break before the first is no break */
    } else if ( style != NULL && style->master && style->scope != 0 ) {
        out->flags |= PF_PAGEBEFORE;            /* a new kind of page starts a page */
    }
    if ( ctx->p_depth < MAX_PDEPTH ) {
        ctx->pbase[ctx->p_depth] = ctx->nspan;
    }
    ctx->p_depth++;
    ctx->span_base = ctx->nspan;
    if ( ctx->nspan < MAX_SPANS ) {
        ctx->span[ctx->nspan++] = chp;
    } else {
        ctx->span_over++;
    }
    odt_chp_now( ctx );
    ctx->space = 1;
}

static void odt_para_end( ODT *ctx )
{
    PAP *pap = &ctx->wb.pap;

    if ( ctx->p_depth == 0 ) {
        return;
    }
    ctx->p_depth--;
    if ( ctx->break_after ) {
        wb_char( &ctx->wb, WCH_PAGE );
        ctx->break_after = 0;
    }
    pap->flags &= (u16)~(PF_INTABLE | PF_CELLEND | PF_ROWEND);
    if ( ctx->tbl_depth ) {
        pap->flags |= PF_INTABLE;
    }
    wb_par( &ctx->wb );
    /* its properties, and any span it left open, come off */
    ctx->span_over = 0;
    if ( ctx->p_depth < MAX_PDEPTH ) {
        ctx->nspan = ctx->pbase[ctx->p_depth];
        ctx->span_base = ctx->p_depth > 0 ? ctx->pbase[ctx->p_depth - 1] : 0;
    } else if ( ctx->nspan > 1 ) {
        ctx->nspan--;
    }
    if ( ctx->nspan < 1 ) {
        ctx->nspan = 1;
    }
    PEAK();
}

/* a story inside the one being read - a note's, or a text box's
   paragraphs - is read with a builder of its own */
static int odt_nest_begin( ODT *ctx, STORY *story )
{
    ONEST *nest;

    if ( ctx->nnest >= MAX_NEST ) {
        return 0;
    }
    nest = &ctx->nest[ctx->nnest++];
    nest->wb = ctx->wb;
    nest->p_depth = ctx->p_depth;
    nest->nspan = ctx->nspan;
    nest->span_base = ctx->span_base;
    nest->nlst = ctx->nlst;
    nest->lst_base = ctx->lst_base;
    nest->tbl_depth = ctx->tbl_depth;
    nest->space = ctx->space;
    nest->break_after = ctx->break_after;
    nest->para_style = ctx->para_style;
    mem_cpy( nest->pbase, ctx->pbase, sizeof( nest->pbase ) );
    if ( story == NULL ) {
        story = ctx->wb.story;          /* a text box: more paragraphs of the same story */
    } else {
        ctx->tbl_depth = 0;
    }
    wb_init( &ctx->wb, ctx->doc, story );
    ctx->p_depth = 0;
    ctx->lst_base = ctx->nlst;
    ctx->break_after = 0;
    return 1;
}

static void odt_nest_end( ODT *ctx )
{
    ONEST *nest;

    if ( ctx->nnest == 0 ) {
        return;
    }
    nest = &ctx->nest[--ctx->nnest];
    wb_done( &ctx->wb );
    ctx->wb = nest->wb;
    ctx->p_depth = nest->p_depth;
    ctx->nspan = nest->nspan;
    ctx->span_base = nest->span_base;
    ctx->span_over = 0;
    ctx->nlst = nest->nlst;
    ctx->lst_base = nest->lst_base;
    ctx->lst_over = 0;
    ctx->tbl_depth = nest->tbl_depth;
    ctx->space = nest->space;
    ctx->break_after = nest->break_after;
    ctx->para_style = nest->para_style;
    mem_cpy( ctx->pbase, nest->pbase, sizeof( ctx->pbase ) );
    odt_chp_now( ctx );
}

/* base64, white space and all, into a block of its own */
static u8 *odt_base64( const u8 *text, u32 len, u32 *out_len )
{
    u8 *out = (u8 *)try_alloc( len / 4 * 3 + 4 );
    u32 used = 0, bits = 0, index;
    int have = 0, value, code;

    if ( out == NULL ) {
        return NULL;
    }
    for ( index = 0; index < len; index++ ) {
        code = text[index];
        value = code >= 'A' && code <= 'Z' ? code - 'A' : code >= 'a' && code <= 'z' ? code - 'a' + 26 :
                code >= '0' && code <= '9' ? code - '0' + 52 : code == '+' ? 62 : code == '/' ? 63 : -1;
        if ( value < 0 ) {
            continue;
        }
        bits = (bits << 6) | (u32)value;
        if ( ++have == 4 ) {
            out[used++] = (u8)(bits >> 16);
            out[used++] = (u8)(bits >> 8);
            out[used++] = (u8)bits;
            have = 0;
            bits = 0;
        }
    }
    if ( have == 3 ) {
        out[used++] = (u8)(bits >> 10);
        out[used++] = (u8)(bits >> 2);
    } else if ( have == 2 ) {
        out[used++] = (u8)(bits >> 4);
    }
    *out_len = used;
    return out;
}

/* a picture's bytes become an object in the line */
static void odt_image_put( ODT *ctx, const u8 *data, u32 len )
{
    int kind, obj;

    if ( data == NULL || len == 0 ) {
        ctx->doc->lost |= LOST_IMAGES;
        return;
    }
    kind = odt_sniff( data, len );
    if ( kind == IMG_UNKNOWN ) {
        ctx->doc->lost |= LOST_IMAGES;
    }
    obj = wd_image_new( ctx->doc, data, len, kind, odt_clamp( ctx->frame_cx, 0, 0x7FFFFFL ),
                        odt_clamp( ctx->frame_cy, 0, 0x7FFFFFL ) );
    if ( obj >= 0 ) {
        odt_chp_now( ctx );
        wb_object( &ctx->wb, obj );
        ctx->space = 0;
    }
    ctx->frame_done = 1;
    PEAK();
}

static void odt_image( ODT *ctx, XML *xml )
{
    const char *href = xml_attr( xml, NS_XLINK, "href" );
    u8 *data;
    u32 len;
    int depth = xml->depth;

    if ( ctx->frame_done ) {
        return;                         /* the same picture again, in another form */
    }
    if ( href != NULL && href[0] ) {
        if ( href[0] == '.' && href[1] == '/' ) {
            href += 2;
        } else if ( href[0] == '#' ) {
            href++;                     /* OpenOffice 1 */
        }
        data = ctx->zip != NULL ? odt_part( ctx, href, &len ) : NULL;
        odt_image_put( ctx, data, data != NULL ? len : 0 );
        xfree( data );
        return;
    }
    /* in a one-file document the picture is inside the element */
    while ( xml_child( xml, depth ) ) {
        if ( xml_is( xml, NS_OFFICE, "binary-data" ) && xml_next( xml ) == XML_TEXT ) {
            data = odt_base64( (const u8 *)xml->text, xml->text_len, &len );
            if ( data == NULL ) {
                ctx->nomem = 1;
            }
            odt_image_put( ctx, data, data != NULL ? len : 0 );
            xfree( data );
        }
    }
}

/* ------------------------------------------------------------------ */
/* tables                                                              */
/* ------------------------------------------------------------------ */

/* the columns have all been met: any with no width share what is left */
static void odt_grid_fix( ODT *ctx )
{
    s32 known = 0, share;
    int index, blank = 0;

    if ( ctx->grid_fixed ) {
        return;
    }
    ctx->grid_fixed = 1;
    for ( index = 0; index < ctx->ngrid; index++ ) {
        if ( ctx->grid[index] > 0 ) {
            known += ctx->grid[index];
        } else {
            blank++;
        }
    }
    if ( blank == 0 ) {
        return;
    }
    share = ctx->tbl_width > 0 ? ctx->tbl_width :
            ctx->doc->page.width - ctx->doc->page.left - ctx->doc->page.right;
    share = (share - known) / blank;
    for ( index = 0; index < ctx->ngrid; index++ ) {
        if ( ctx->grid[index] <= 0 ) {
            ctx->grid[index] = odt_clamp( share, 144, 31680 );
        }
    }
}

/* a cell of the outermost table ends: "span" columns wide */
static void odt_cell_end( ODT *ctx, int span )
{
    PAP blank;
    WPARA *para;
    s32 width = 0;
    int index;

    if ( ctx->wb.para != NULL ) {
        ctx->wb.pap.flags |= PF_INTABLE;
        wb_par( &ctx->wb );
    }
    if ( ctx->wb.story->count == ctx->cell_from ) {
        /* a cell with nothing in it has a paragraph all the same */
        pap_init( &blank );
        blank.flags = PF_INTABLE;
        wd_para_add( ctx->doc, ctx->wb.story, &blank );
    }
    para = odt_last_para( ctx );
    if ( para == NULL ) {
        return;
    }
    for ( index = 0; index < span; index++ ) {
        width += ctx->col + index < ctx->ngrid ? ctx->grid[ctx->col + index] : 1440;
    }
    ctx->col += span;
    if ( ctx->edge < EDGE_MAX ) {
        ctx->edge += odt_clamp( width, 1, 63360L );
    }
    para->pap.flags |= PF_INTABLE | PF_CELLEND;
    if ( ctx->row.ncells < MAX_CELLS ) {
        ctx->row.edge[ctx->row.ncells++] = ctx->edge;
    } else {
        ctx->row.edge[MAX_CELLS - 1] = ctx->edge;
        if ( ctx->cell_last != NULL && ctx->cell_last != para ) {
            ctx->cell_last->pap.flags &= (u16)~PF_CELLEND;
        }
        ctx->doc->lost |= LOST_TABLES;
    }
    ctx->cell_last = para;
}

/* an empty cell, for one the file has only as "covered" by the cell above */
static void odt_cell_blank( ODT *ctx )
{
    PAP blank;

    pap_init( &blank );
    blank.flags = PF_INTABLE;
    ctx->cell_from = ctx->wb.story->count;
    wd_para_add( ctx->doc, ctx->wb.story, &blank );
    if ( ctx->wb.story->count > ctx->cell_from ) {
        odt_cell_end( ctx, 1 );
    }
}

static void odt_row_end( ODT *ctx )
{
    if ( ctx->row.ncells == 0 || ctx->cell_last == NULL ) {
        return;
    }
    ctx->row.borders = (u8)ctx->row_ruled;
    ctx->cell_last->pap.flags |= PF_ROWEND;
    if ( !pap_set_row( &ctx->cell_last->pap, &ctx->row ) ) {
        ctx->nomem = 1;
    }
    ctx->cell_last = NULL;
}

static void odt_table_start( ODT *ctx, XML *xml )
{
    s32 value;
    int index, repeat;

    if ( xml_is( xml, NS_TABLE, "table" ) ) {
        if ( ctx->p_depth > 0 && ctx->wb.para != NULL ) {
            wb_par( &ctx->wb );
        }
        if ( ctx->tbl_depth++ == 0 ) {
            index = odt_style_find( ctx, FAMILY_TABLE, xml_attr( xml, NS_TABLE, "style-name" ), ctx->scope );
            if ( index >= 0 ) {
                odt_style_fold( ctx, index, 0 );
            }
            ctx->tbl_left = index >= 0 ? ctx->style[index].tbl_left : 0;
            ctx->tbl_width = index >= 0 && ctx->style[index].has_width ? ctx->style[index].width : 0;
            ctx->ngrid = ctx->grid_fixed = ctx->in_head_rows = 0;
            ctx->cell_last = NULL;
        } else {
            ctx->doc->lost |= LOST_TABLES;      /* a table in a cell: its text stays in the cell */
        }
        return;
    }
    if ( ctx->tbl_depth != 1 ) {
        return;
    }
    if ( xml_is( xml, NS_TABLE, "table-column" ) ) {
        index = odt_style_find( ctx, FAMILY_COLUMN, xml_attr( xml, NS_TABLE, "style-name" ), ctx->scope );
        if ( index >= 0 ) {
            odt_style_fold( ctx, index, 0 );
        }
        repeat = odt_int( xml_attr( xml, NS_TABLE, "number-columns-repeated" ), &value ) ?
                 (int)odt_clamp( value, 1, MAX_GRID ) : 1;
        for ( ; repeat > 0 && ctx->ngrid < MAX_GRID; repeat-- ) {
            ctx->grid[ctx->ngrid++] = index >= 0 && ctx->style[index].has_width ?
                                      odt_clamp( ctx->style[index].width, 1, 63360L ) : 0;
        }
    } else if ( xml_is( xml, NS_TABLE, "table-header-rows" ) ) {
        ctx->in_head_rows = 1;
    } else if ( xml_is( xml, NS_TABLE, "table-row" ) ) {
        odt_grid_fix( ctx );
        mem_set( &ctx->row, 0, sizeof( TROW ) );
        ctx->row.left = ctx->tbl_left;
        ctx->row.header = (u8)ctx->in_head_rows;
        ctx->edge = ctx->tbl_left;
        ctx->col = ctx->covered = ctx->row_ruled = 0;
        ctx->cell_last = NULL;
    } else if ( xml_is( xml, NS_TABLE, "table-cell" ) ) {
        ctx->cell_from = ctx->wb.story->count;
        ctx->cell_span = odt_int( xml_attr( xml, NS_TABLE, "number-columns-spanned" ), &value ) ?
                         (int)odt_clamp( value, 1, MAX_GRID ) : 1;
        ctx->cell_repeat = odt_int( xml_attr( xml, NS_TABLE, "number-columns-repeated" ), &value ) ?
                           (int)odt_clamp( value, 1, MAX_REPEAT ) : 1;
        if ( odt_int( xml_attr( xml, NS_TABLE, "number-rows-spanned" ), &value ) && value > 1 ) {
            ctx->doc->lost |= LOST_TABLES;      /* cells joined downward are single cells here */
        }
        index = odt_style_find( ctx, FAMILY_CELL, xml_attr( xml, NS_TABLE, "style-name" ), ctx->scope );
        if ( index >= 0 ) {
            odt_style_fold( ctx, index, 0 );
            ctx->row_ruled |= ctx->style[index].ruled;
        }
    } else if ( xml_is( xml, NS_TABLE, "covered-table-cell" ) ) {
        repeat = odt_int( xml_attr( xml, NS_TABLE, "number-columns-repeated" ), &value ) ?
                 (int)odt_clamp( value, 1, MAX_REPEAT ) : 1;
        for ( ; repeat > 0; repeat-- ) {
            if ( ctx->covered > 0 ) {
                ctx->covered--;         /* under the cell beside it, which has taken its room */
            } else {
                odt_cell_blank( ctx );
            }
        }
        xml_skip( xml );
    }
}

static void odt_table_end( ODT *ctx, const XML *xml )
{
    int repeat;

    if ( xml_is( xml, NS_TABLE, "table" ) ) {
        if ( ctx->tbl_depth ) {
            ctx->tbl_depth--;
        }
        return;
    }
    if ( ctx->tbl_depth != 1 ) {
        return;
    }
    if ( xml_is( xml, NS_TABLE, "table-cell" ) ) {
        odt_cell_end( ctx, ctx->cell_span );
        ctx->covered = ctx->cell_span - 1;
        for ( repeat = ctx->cell_repeat; repeat > 1; repeat-- ) {
            odt_cell_blank( ctx );
        }
    } else if ( xml_is( xml, NS_TABLE, "table-row" ) ) {
        odt_row_end( ctx );
    } else if ( xml_is( xml, NS_TABLE, "table-header-rows" ) ) {
        ctx->in_head_rows = 0;
    }
}

/* ------------------------------------------------------------------ */
/* the pass over a story's XML                                         */
/* ------------------------------------------------------------------ */

static void odt_note_begin( ODT *ctx, int kind )
{
    ctx->note_obj = wd_note_new( ctx->doc, kind );
    if ( ctx->note_obj >= 0 ) {
        odt_chp_now( ctx );
        wb_object( &ctx->wb, ctx->note_obj );
        ctx->space = 0;
    }
}

/* an element of the text starts */
static void odt_content_start( ODT *ctx, XML *xml )
{
    OLDEPTH *at;
    XCHP chp;
    const char *val;
    s32 value;
    int index;

    if ( xml->ns == NS_TEXT ) {
        if ( xml_is( xml, NS_TEXT, "span" ) ) {
            if ( ctx->nspan == 0 || ctx->nspan >= MAX_SPANS ) {
                ctx->span_over++;
                return;
            }
            chp = ctx->span[ctx->nspan - 1];
            index = odt_style_find( ctx, FAMILY_TEXT, xml_attr( xml, NS_TEXT, "style-name" ), ctx->scope );
            if ( index >= 0 ) {
                odt_style_fold( ctx, index, 0 );
                xchp_over( &chp, &ctx->style[index].chp );
            }
            ctx->span[ctx->nspan++] = chp;
            odt_chp_now( ctx );
        } else if ( xml_is( xml, NS_TEXT, "p" ) ) {
            odt_para_begin( ctx, xml, 0 );
        } else if ( xml_is( xml, NS_TEXT, "h" ) ) {
            odt_para_begin( ctx, xml, 1 );
        } else if ( xml_is( xml, NS_TEXT, "s" ) ) {
            value = 1;
            odt_int( xml_attr( xml, NS_TEXT, "c" ), &value );
            for ( value = odt_clamp( value, 1, 4096 ); value > 0 && ctx->p_depth; value-- ) {
                odt_put( ctx, ' ' );
            }
        } else if ( xml_is( xml, NS_TEXT, "tab" ) || xml_is( xml, NS_TEXT, "tab-stop" ) ) {
            if ( ctx->p_depth ) {
                odt_put( ctx, WCH_TAB );
            }
        } else if ( xml_is( xml, NS_TEXT, "line-break" ) ) {
            if ( ctx->p_depth ) {
                odt_put( ctx, WCH_LINE );
            }
        } else if ( xml_is( xml, NS_TEXT, "list" ) || xml_is( xml, NS_TEXT, "ordered-list" ) ||
                    xml_is( xml, NS_TEXT, "unordered-list" ) ) {
            if ( ctx->nlst >= MAX_LIST_DEPTH ) {
                ctx->lst_over++;
                return;
            }
            at = &ctx->lst[ctx->nlst++];
            mem_set( at, 0, sizeof( OLDEPTH ) );
            at->list = odt_list_find( ctx, xml_attr( xml, NS_TEXT, "style-name" ), ctx->scope );
            val = xml_attr( xml, NS_TEXT, "continue-numbering" );
            at->runs_on = (u8)(val != NULL && str_cmp( val, "true" ) == 0);
            if ( at->list >= 0 && ctx->nlst - ctx->lst_base == 1 ) {
                /* numbers that "carry on" need a list before this one to
                   carry on from: Word says it of every list */
                if ( !ctx->list[at->list].used ) {
                    at->runs_on = 0;
                }
                ctx->list[at->list].used = 1;
            }
        } else if ( xml_is( xml, NS_TEXT, "list-item" ) || xml_is( xml, NS_TEXT, "list-header" ) ) {
            if ( ctx->nlst > ctx->lst_base && !ctx->lst_over ) {
                at = &ctx->lst[ctx->nlst - 1];
                at->paras = 0;
                at->header = (u8)xml_is( xml, NS_TEXT, "list-header" );
                at->item_start = 0;
                if ( odt_int( xml_attr( xml, NS_TEXT, "start-value" ), &value ) ) {
                    at->item_start = (u16)odt_clamp( value, 0, 65535L );
                }
                if ( at->items < 2 ) {
                    at->items++;
                }
            }
        } else if ( xml_is( xml, NS_TEXT, "note" ) ) {
            val = xml_attr( xml, NS_TEXT, "note-class" );
            odt_note_begin( ctx, val != NULL && str_cmp( val, "endnote" ) == 0 ? NOTE_END : NOTE_FOOT );
        } else if ( xml_is( xml, NS_TEXT, "footnote" ) ) {
            odt_note_begin( ctx, NOTE_FOOT );
        } else if ( xml_is( xml, NS_TEXT, "endnote" ) ) {
            odt_note_begin( ctx, NOTE_END );
        } else if ( xml_is( xml, NS_TEXT, "note-body" ) || xml_is( xml, NS_TEXT, "footnote-body" ) ||
                    xml_is( xml, NS_TEXT, "endnote-body" ) ) {
            if ( ctx->note_obj < 0 || !odt_nest_begin( ctx, wd_note_story( ctx->doc, ctx->note_obj ) ) ) {
                xml_skip( xml );
            }
            ctx->note_obj = -1;
        } else if ( xml_is( xml, NS_TEXT, "page-number" ) ) {
            odt_put( ctx, WCH_PAGENUM );
            xml_skip( xml );
        } else if ( xml_is( xml, NS_TEXT, "page-count" ) ) {
            odt_put( ctx, WCH_NUMPAGES );
            xml_skip( xml );
        } else if ( xml_is( xml, NS_TEXT, "date" ) ) {
            odt_put( ctx, WCH_DATE );
            xml_skip( xml );
        } else if ( xml_is( xml, NS_TEXT, "time" ) ) {
            odt_put( ctx, WCH_TIME );
            xml_skip( xml );
        } else if ( xml_is( xml, NS_TEXT, "file-name" ) ) {
            odt_put( ctx, WCH_FILENAME );
            xml_skip( xml );
        } else if ( xml_is( xml, NS_TEXT, "tracked-changes" ) ) {
            ctx->doc->lost |= LOST_REVISIONS;   /* what was deleted is kept there, and not here */
            xml_skip( xml );
        } else if ( xml_is( xml, NS_TEXT, "note-citation" ) || xml_is( xml, NS_TEXT, "footnote-citation" ) ||
                    xml_is( xml, NS_TEXT, "endnote-citation" ) || xml_is( xml, NS_TEXT, "ruby-text" ) ||
                    xml_is( xml, NS_TEXT, "sequence-decls" ) || xml_is( xml, NS_TEXT, "variable-decls" ) ||
                    xml_is( xml, NS_TEXT, "user-field-decls" ) ||
                    (xml->name_len > 7 && mem_cmp( xml->name + xml->name_len - 7, "-source", 7 ) == 0) ) {
            xml_skip( xml );
        } else if ( xml_is( xml, NS_TEXT, "author-name" ) || xml_is( xml, NS_TEXT, "title" ) ||
                    xml_is( xml, NS_TEXT, "subject" ) || xml_is( xml, NS_TEXT, "chapter" ) ||
                    xml_is( xml, NS_TEXT, "variable-get" ) || xml_is( xml, NS_TEXT, "user-field-get" ) ||
                    xml_is( xml, NS_TEXT, "database-display" ) || xml_is( xml, NS_TEXT, "sequence" ) ) {
            ctx->doc->lost |= LOST_FIELDS;      /* kept as the text it last showed */
        }
    } else if ( xml->ns == NS_TABLE ) {
        odt_table_start( ctx, xml );
    } else if ( xml->ns == NS_DRAW ) {
        if ( xml_is( xml, NS_DRAW, "frame" ) ) {
            ctx->frame_cx = ctx->frame_cy = 0;
            odt_twips( xml_attr( xml, NS_SVG, "width" ), &ctx->frame_cx );
            odt_twips( xml_attr( xml, NS_SVG, "height" ), &ctx->frame_cy );
            ctx->frame_done = 0;
        } else if ( xml_is( xml, NS_DRAW, "image" ) ) {
            odt_image( ctx, xml );
        } else if ( xml_is( xml, NS_DRAW, "text-box" ) ) {
            ctx->doc->lost |= LOST_LAYOUT;
            ctx->frame_done = 1;
            if ( !odt_nest_begin( ctx, NULL ) ) {
                xml_skip( xml );
            }
        } else if ( xml_is( xml, NS_DRAW, "object" ) || xml_is( xml, NS_DRAW, "object-ole" ) ||
                    xml_is( xml, NS_DRAW, "custom-shape" ) || xml_is( xml, NS_DRAW, "plugin" ) ) {
            ctx->doc->lost |= LOST_OBJECTS;
        }
    } else if ( xml->ns == NS_SVG ) {
        if ( xml_is( xml, NS_SVG, "title" ) || xml_is( xml, NS_SVG, "desc" ) ) {
            xml_skip( xml );
        }
    } else if ( xml->ns == NS_OFFICE ) {
        if ( xml_is( xml, NS_OFFICE, "annotation" ) || xml_is( xml, NS_OFFICE, "forms" ) ||
             xml_is( xml, NS_OFFICE, "binary-data" ) ) {
            xml_skip( xml );
        }
    }
}

static void odt_content_end( ODT *ctx, const XML *xml )
{
    if ( xml->ns == NS_TEXT ) {
        if ( xml_is( xml, NS_TEXT, "span" ) ) {
            if ( ctx->span_over ) {
                ctx->span_over--;
            } else if ( ctx->nspan > ctx->span_base + 1 ) {
                ctx->nspan--;
                odt_chp_now( ctx );
            }
        } else if ( xml_is( xml, NS_TEXT, "p" ) || xml_is( xml, NS_TEXT, "h" ) ) {
            odt_para_end( ctx );
        } else if ( xml_is( xml, NS_TEXT, "list" ) || xml_is( xml, NS_TEXT, "ordered-list" ) ||
                    xml_is( xml, NS_TEXT, "unordered-list" ) ) {
            if ( ctx->lst_over ) {
                ctx->lst_over--;
            } else if ( ctx->nlst > ctx->lst_base ) {
                ctx->nlst--;
            }
        } else if ( xml_is( xml, NS_TEXT, "note-body" ) || xml_is( xml, NS_TEXT, "footnote-body" ) ||
                    xml_is( xml, NS_TEXT, "endnote-body" ) ) {
            odt_nest_end( ctx );
        }
    } else if ( xml->ns == NS_TABLE ) {
        odt_table_end( ctx, xml );
    } else if ( xml->ns == NS_DRAW && xml_is( xml, NS_DRAW, "text-box" ) ) {
        odt_nest_end( ctx );
    }
}

static void odt_story_begin( ODT *ctx, STORY *story )
{
    wb_init( &ctx->wb, ctx->doc, story );
    ctx->story_on = 1;
    ctx->p_depth = ctx->nspan = ctx->span_base = ctx->span_over = 0;
    ctx->nlst = ctx->lst_base = ctx->lst_over = ctx->nnest = 0;
    ctx->tbl_depth = 0;
    ctx->note_obj = -1;
    ctx->break_after = 0;
    ctx->cell_last = NULL;
    ctx->span[0] = ctx->def_chp;
    ctx->nspan = 1;
    ctx->span_base = 0;
}

static void odt_story_end( ODT *ctx )
{
    while ( ctx->nnest ) {
        odt_nest_end( ctx );            /* a file that stopped inside a note */
    }
    wb_done( &ctx->wb );
    ctx->story_on = 0;
}

/* title, author, subject, keywords */
static void odt_meta( ODT *ctx, XML *xml )
{
    WDOC *doc = ctx->doc;
    char word[INFO_LEN];
    const char *from;
    int depth = xml->depth;

    while ( xml_child( xml, depth ) ) {
        if ( xml_is( xml, NS_DC, "title" ) ) {
            xml_gather( xml, doc->title, INFO_LEN );
        } else if ( xml_is( xml, NS_DC, "subject" ) ) {
            xml_gather( xml, doc->subject, INFO_LEN );
        } else if ( xml_is( xml, NS_META, "initial-creator" ) || xml_is( xml, NS_DC, "creator" ) ) {
            xml_gather( xml, word, INFO_LEN );
            if ( word[0] && (doc->author[0] == 0 || xml_is( xml, NS_META, "initial-creator" )) ) {
                str_cpy( doc->author, word );
            }
        } else if ( xml_is( xml, NS_META, "keyword" ) ) {
            xml_gather( xml, word, INFO_LEN );
            for ( from = word; *from == ' '; from++ ) {
            }
            if ( doc->keywords[0] && *from ) {
                str_catn( doc->keywords, ", ", INFO_LEN );
            }
            str_catn( doc->keywords, from, INFO_LEN );
        } else if ( xml_is( xml, NS_META, "keywords" ) ) {
            /* OpenOffice 1 put them in a list of their own */
            continue;
        }
    }
}

/*
 * One pass over one document: what "mode" asks for is read and the
 * rest is passed over.  1 if the XML was whole.
 */
static int odt_pass( ODT *ctx, const u8 *data, u32 len, int mode, int scope )
{
    XML *xml = odt_xml( ctx, data, len );
    const char *name;
    int token, depth;

    if ( xml == NULL ) {
        return 0;
    }
    ctx->mode = mode;
    ctx->scope = scope;
    ctx->story_on = ctx->in_master = 0;
    for ( ;; ) {
        token = xml_next( xml );
        if ( token == XML_EOF || token == XML_ERROR ) {
            break;
        }
        if ( token == XML_TEXT ) {
            if ( ctx->story_on && ctx->p_depth ) {
                odt_text( ctx, (const u8 *)xml->text, xml->text_len );
            }
            continue;
        }
        if ( token == XML_END ) {
            if ( ctx->story_on ) {
                if ( (xml->ns == NS_OFFICE && xml_is( xml, NS_OFFICE, "text" )) ||
                     (xml->ns == NS_STYLE && (xml_is( xml, NS_STYLE, "header" ) || xml_is( xml, NS_STYLE, "footer" ))) ) {
                    odt_story_end( ctx );
                } else {
                    odt_content_end( ctx, xml );
                }
            } else if ( xml->ns == NS_STYLE && xml_is( xml, NS_STYLE, "master-page" ) ) {
                ctx->in_master = 0;
            }
            continue;
        }
        if ( ctx->story_on ) {
            odt_content_start( ctx, xml );
            continue;
        }
        /* the document's own structure */
        if ( xml->ns == NS_OFFICE ) {
            if ( xml_is( xml, NS_OFFICE, "font-face-decls" ) || xml_is( xml, NS_OFFICE, "font-decls" ) ) {
                depth = xml->depth;
                while ( xml_child( xml, depth ) ) {
                    if ( (mode & M_STYLES) && xml->ns == NS_STYLE ) {
                        odt_font_read( ctx, xml );
                    }
                }
            } else if ( xml_is( xml, NS_OFFICE, "styles" ) ) {
                if ( mode & M_STYLES ) {
                    odt_styles_in( ctx, xml, 0 );
                } else {
                    xml_skip( xml );
                }
            } else if ( xml_is( xml, NS_OFFICE, "automatic-styles" ) ) {
                if ( mode & M_STYLES ) {
                    odt_styles_in( ctx, xml, scope );
                    odt_defaults( ctx );
                } else {
                    xml_skip( xml );
                }
            } else if ( xml_is( xml, NS_OFFICE, "master-styles" ) ) {
                if ( !(mode & (M_STYLES | M_MASTER)) ) {
                    xml_skip( xml );
                }
            } else if ( xml_is( xml, NS_OFFICE, "meta" ) ) {
                if ( mode & M_META ) {
                    odt_meta( ctx, xml );
                } else {
                    xml_skip( xml );
                }
            } else if ( xml_is( xml, NS_OFFICE, "body" ) ) {
                if ( !(mode & M_BODY) ) {
                    xml_skip( xml );
                }
            } else if ( xml_is( xml, NS_OFFICE, "text" ) ) {
                odt_story_begin( ctx, &ctx->doc->body );
            } else if ( xml_is( xml, NS_OFFICE, "settings" ) || xml_is( xml, NS_OFFICE, "scripts" ) ) {
                xml_skip( xml );
            }
        } else if ( xml->ns == NS_STYLE ) {
            if ( xml_is( xml, NS_STYLE, "master-page" ) ) {
                name = xml_attr( xml, NS_STYLE, "name" );
                if ( (mode & M_STYLES) && name != NULL &&
                     odt_grow( ctx, (void **)&ctx->master, &ctx->master_cap, ctx->nmaster + 1, sizeof( OMASTER ), 4096 ) ) {
                    ctx->master[ctx->nmaster].layout = odt_str( ctx, xml_attr( xml, NS_STYLE, "page-layout-name" ) );
                    if ( ctx->master[ctx->nmaster].layout == 0 ) {
                        ctx->master[ctx->nmaster].layout = odt_str( ctx, xml_attr( xml, NS_STYLE, "page-master-name" ) );
                    }
                    ctx->master[ctx->nmaster].next = odt_str( ctx, xml_attr( xml, NS_STYLE, "next-style-name" ) );
                    ctx->master[ctx->nmaster].name = odt_str( ctx, xml_attr( xml, NS_STYLE, "name" ) );
                    ctx->nmaster++;
                    name = xml_attr( xml, NS_STYLE, "name" );
                }
                if ( (mode & M_MASTER) && name != NULL && ctx->want_master &&
                     str_cmp( name, STR( ctx, ctx->want_master ) ) == 0 ) {
                    ctx->in_master = 1;
                } else {
                    xml_skip( xml );
                }
            } else if ( ctx->in_master && xml_is( xml, NS_STYLE, "header" ) && !ctx->head_done ) {
                ctx->head_done = 1;
                odt_story_begin( ctx, &ctx->doc->header );
            } else if ( ctx->in_master && xml_is( xml, NS_STYLE, "footer" ) && !ctx->foot_done ) {
                ctx->foot_done = 1;
                odt_story_begin( ctx, &ctx->doc->footer );
            } else if ( ctx->in_master ) {
                xml_skip( xml );        /* a left page's or a first page's own */
            }
        }
    }
    if ( ctx->story_on ) {
        if ( ctx->wb.para != NULL ) {
            wb_par( &ctx->wb );
        }
        odt_story_end( ctx );
    }
    xml_close( xml );
    return token == XML_EOF;
}

/* ------------------------------------------------------------------ */
/* detecting and loading                                               */
/* ------------------------------------------------------------------ */

static ODT *odt_new( WDOC *doc )
{
    ODT *ctx = (ODT *)try_alloc( sizeof( ODT ) );

    if ( ctx == NULL ) {
        return NULL;
    }
    mem_set( ctx, 0, sizeof( ODT ) );
    /* the strings: place 0 is "", so a string that was never given is one */
    ctx->str = (char *)try_alloc( 2048 );
    if ( ctx->str == NULL ) {
        xfree( ctx );
        return NULL;
    }
    ctx->str[0] = 0;
    ctx->str_len = 1;
    ctx->str_cap = 2048;
    ctx->doc = doc;
    ctx->note_obj = -1;
    /* what a file that says nothing gets: 12 point, single spaced */
    ctx->def_chp.set = XC_FONT | XC_SIZE | XC_COLOR | XC_HILITE | XC_UNDER | XC_VERT;
    ctx->def_chp.tog_set = CF_TOGGLES;
    ctx->def_chp.size = 24;
    ctx->def_chp.color = COLOR_AUTO;
    ctx->def_pap.set = XP_ALIGN | XP_LEFT | XP_RIGHT | XP_FIRST | XP_BEFORE | XP_AFTER | XP_LINE;
    ctx->def_pap.flag_set = PF_GIVEN;
    ctx->def_pap.line = 240;
    ctx->def_pap.line_rule = LS_MULTIPLE;
    return ctx;
}

static void odt_free( ODT *ctx )
{
    if ( ctx == NULL ) {
        return;
    }
    odt_styles_free( ctx );
    zip_close( ctx->zip );
    xfree( ctx->str );
    xfree( ctx->list );
    xfree( ctx->font );
    xfree( ctx->page );
    xfree( ctx->master );
    xfree( ctx );
}

/* a one-file document: XML whose root says it is a text document */
static int odt_is_flat( const u8 *data, u32 len )
{
    u32 look = len < 4096 ? len : 4096;

    while ( look && (data[0] == ' ' || data[0] == '\r' || data[0] == '\n' || data[0] == '\t' || data[0] >= 0x80) ) {
        data++;
        look--;
    }
    return look > 64 && data[0] == '<' && mem_find( data, look, ":document" ) >= 0 &&
           mem_find( data, look, "office:mimetype=\"" MIME_ODT ) >= 0;
}

static int odt_detect( const u8 *data, u32 len, const char *ext )
{
    u8 head[512];
    ZIP *zip;
    int index, sure = 0;

    (void)ext;
    if ( len < 60 ) {
        return 0;
    }
    if ( data[0] != 'P' || data[1] != 'K' || data[2] != 3 || data[3] != 4 ) {
        return odt_is_flat( data, len ) ? 100 : 0;
    }
    zip = zip_open( data, len );
    if ( zip == NULL ) {
        return 0;
    }
    index = zip_find( zip, "mimetype" );
    if ( index >= 0 && zip_peek( zip, (u32)index, head, sizeof( head ) - 1 ) ) {
        head[sizeof( head ) - 1] = 0;
        for ( index = (int)str_len( (const char *)head ); index > 0 && head[index - 1] <= ' '; index-- ) {
            head[index - 1] = 0;
        }
        if ( str_cmp( (const char *)head, MIME_ODT ) == 0 || str_cmp( (const char *)head, MIME_OTT ) == 0 ||
             str_cmp( (const char *)head, MIME_SXW ) == 0 ) {
            sure = 100;
        }
    } else if ( index < 0 ) {
        /* no mimetype at all: the root of content.xml says */
        index = zip_find( zip, "content.xml" );
        if ( index >= 0 && zip_peek( zip, (u32)index, head, sizeof( head ) - 1 ) &&
             mem_find( head, sizeof( head ) - 1, ":document-content" ) >= 0 &&
             mem_find( head, sizeof( head ) - 1, "opendocument:xmlns:text" ) >= 0 ) {
            sure = 90;
        }
    }
    zip_close( zip );
    return sure;
}

/* the style every other is measured from, made the document's Normal
   before the text is read */
static void odt_normal( ODT *ctx )
{
    int index = odt_style_find( ctx, FAMILY_PARA, "Standard", 0 );

    if ( index < 0 ) {
        index = odt_style_find( ctx, FAMILY_PARA, "Normal", 0 );
    }
    if ( index >= 0 ) {
        odt_wd_style( ctx, index, 0 );
    } else {
        xchp_chp( &ctx->def_chp, &ctx->doc->style[0].chp );
    }
}

/* the page the first paragraph is on, from its master page's layout */
static void odt_page_setup( ODT *ctx )
{
    WDOC *doc = ctx->doc;
    const OPAGE *page = NULL;
    u32 index, layout = 0;

    ctx->want_master = ctx->next_master = 0;
    for ( index = 0; index < ctx->nmaster; index++ ) {
        if ( ctx->first_master && str_cmp( STR( ctx, ctx->master[index].name ), STR( ctx, ctx->first_master ) ) == 0 ) {
            ctx->want_master = ctx->master[index].name;
            ctx->next_master = ctx->master[index].next;
            layout = ctx->master[index].layout;
            break;
        }
    }
    for ( index = 0; ctx->want_master == 0 && index < ctx->nmaster; index++ ) {
        if ( index == 0 || str_cmp( STR( ctx, ctx->master[index].name ), "Standard" ) == 0 ) {
            ctx->want_master = ctx->master[index].name;
            layout = ctx->master[index].layout;
        }
    }
    for ( index = 0; layout && index < ctx->npage; index++ ) {
        if ( str_cmp( STR( ctx, ctx->page[index].name ), STR( ctx, layout ) ) == 0 ) {
            page = &ctx->page[index];
        }
    }
    if ( page != NULL && page->has_size == 3 && page->page.left + page->page.right < page->page.width &&
         page->page.top + page->page.bottom < page->page.height ) {
        doc->page = page->page;
        /* the margin is to the header; the text starts below it */
        if ( page->has_head ) {
            doc->page.header = page->page.top;
            doc->page.top = odt_clamp( page->page.top + page->head_room, 0, 31680 );
        }
        if ( page->has_foot ) {
            doc->page.footer = page->page.bottom;
            doc->page.bottom = odt_clamp( page->page.bottom + page->foot_room, 0, 31680 );
        }
    }
    doc->page.first_page = (u16)(ctx->first_number ? ctx->first_number : 1);
}

static int odt_load( const u8 *data, u32 len, WDOC *doc )
{
    ODT *ctx = odt_new( doc );
    u8 *part;
    u32 part_len;
    int why, rc = WE_OK, ok;

    if ( ctx == NULL ) {
        return WE_NOMEM;
    }
    if ( len < 4 ) {
        odt_free( ctx );
        return WE_FORMAT;
    }
    if ( data[0] != 'P' ) {
        /* one file: its styles, then its text, then its header and footer */
        ctx->flat = 1;
        ok = odt_pass( ctx, data, len, M_STYLES | M_META, 1 );
        odt_defaults( ctx );
        odt_normal( ctx );
        ok = ok && odt_pass( ctx, data, len, M_BODY, 1 );
        if ( !ok && doc->body.count == 0 ) {
            rc = WE_FORMAT;
        } else {
            odt_page_setup( ctx );
            odt_pass( ctx, data, len, M_MASTER, 1 );
            if ( ctx->next_master && (!ctx->head_done || !ctx->foot_done) ) {
                ctx->want_master = ctx->next_master;
                odt_pass( ctx, data, len, M_MASTER, 1 );
            }
        }
    } else {
        ctx->zip = zip_open_why( data, len, &why );
        if ( ctx->zip == NULL ) {
            odt_free( ctx );
            return why == ZIP_NOMEM ? WE_NOMEM : WE_FORMAT;
        }
        part = odt_part( ctx, "META-INF/manifest.xml", &part_len );
        if ( part != NULL && mem_find( part, part_len, "encryption-data" ) >= 0 ) {
            rc = WE_PASSWORD;
        }
        xfree( part );
        if ( rc == WE_OK ) {
            part = odt_part( ctx, "styles.xml", &part_len );
            if ( part != NULL ) {
                odt_pass( ctx, part, part_len, M_STYLES, 2 );
                xfree( part );
            }
            odt_defaults( ctx );
            odt_normal( ctx );
            part = odt_part( ctx, "content.xml", &part_len );
            if ( part == NULL || (!odt_pass( ctx, part, part_len, M_STYLES | M_BODY, 1 ) && doc->body.count == 0) ) {
                rc = WE_FORMAT;
            }
            xfree( part );
        }
        if ( rc == WE_OK ) {
            odt_page_setup( ctx );
            part = odt_part( ctx, "styles.xml", &part_len );
            if ( part != NULL ) {
                /* a first page with a master page of its own has the
                   header and footer of the pages after it, where it
                   has none itself */
                odt_pass( ctx, part, part_len, M_MASTER, 2 );
                if ( ctx->next_master && (!ctx->head_done || !ctx->foot_done) ) {
                    ctx->want_master = ctx->next_master;
                    odt_pass( ctx, part, part_len, M_MASTER, 2 );
                }
                xfree( part );
            }
            part = odt_part( ctx, "meta.xml", &part_len );
            if ( part != NULL ) {
                odt_pass( ctx, part, part_len, M_META, 2 );
                xfree( part );
            }
        }
    }
    if ( rc == WE_OK && (ctx->nomem || wd_failed( doc )) ) {
        rc = WE_NOMEM;
    }
    odt_free( ctx );
    return rc;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

#define XML_HEAD    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
#define ODF_SPACES  " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\"" \
                    " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\"" \
                    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\"" \
                    " xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\"" \
                    " xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\"" \
                    " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\"" \
                    " xmlns:xlink=\"http://www.w3.org/1999/xlink\"" \
                    " xmlns:dc=\"http://purl.org/dc/elements/1.1/\"" \
                    " xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\"" \
                    " xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"" \
                    " office:version=\"1.2\">"
#define DRAIN_AT    32768UL
#define MAX_GRIDW   63
#define LEN_MAX     31680L

/* what makes two paragraphs need two styles, beyond their properties */
#define PK_AFTER    1           /* a page ends after it */
#define PK_BEFORE   2           /* ...or before it, for a break that is its first character */
#define PK_FIRST    4           /* the text's first: it says which page number that is */

typedef struct {
    const WPARA *para;          /* a paragraph that looks so */
    u8  key;                    /* PK_* */
} WPSTYLE;

typedef struct {
    u8  kind[LIST_LEVELS];      /* LIST_*, 0xFF for a level nothing has used */
} WLIST;

typedef struct {
    WDOC *doc;
    ZIPW *zipw;
    OUT  *part;                 /* the part being written, a piece of it */
    OUT  *scrap;                /* where the text goes the first time through: nowhere */
    OUT  *to;                   /* one of the two: where text is written now */
    int   failed;
    const char *prefix;         /* "" in content.xml, "M" in styles.xml: its styles' names */
    WPSTYLE *pstyle;
    u32   npstyle, pstyle_cap;
    WLIST *list;
    u32   nlist, list_cap;      /* lists met so far in this pass; made in the first */
    u32   cur_list;             /* the list open, or the one before: its number, 0 for none */
    u8   *chp_used;             /* by CHP number */
    u8   *img_used;             /* by object */
    u32   ntable, nnote;
    int   content;              /* the part in hand is content.xml: notes are written */
    int   first_left;           /* the text's first paragraph is still to come */
    /* the lists open where the text is being written */
    int   ldepth;
    u8    item_open[LIST_LEVELS + 1];
    u8    item_count[LIST_LEVELS + 1];  /* 0 until a list has had an item */
    int   space;                /* the last character written was a space, or the paragraph's start */
} ODTW;

#define PUT( text )     out_str( w->to, (text) )

static void odtw_drain( ODTW *w, int all )
{
    u8 *data;
    u32 len, size;

    if ( w->to == w->scrap ) {
        if ( out_error( w->scrap ) ) {
            w->failed = 1;
        }
        out_seek( w->scrap, 0 );        /* the first time through nothing is kept */
        return;
    }
    len = out_tell( w->part );
    if ( out_error( w->part ) ) {
        w->failed = 1;
        return;
    }
    if ( len == 0 || (!all && len < DRAIN_AT) ) {
        return;
    }
    data = out_data( w->part, &size );
    if ( !zipw_write( w->zipw, data, len ) ) {
        w->failed = 1;
    }
    out_seek( w->part, 0 );
    PEAK();
}

static void odtw_begin( ODTW *w, const char *name )
{
    if ( !zipw_begin( w->zipw, name ) ) {
        w->failed = 1;
    }
    w->to = w->part;
    PUT( XML_HEAD );
}

static void odtw_finish( ODTW *w )
{
    w->to = w->part;
    odtw_drain( w, 1 );
    if ( !zipw_finish( w->zipw ) ) {
        w->failed = 1;
    }
}

/* a length as an attribute: points, which twips are a whole number of
   hundredths of */
static void odtw_len( OUT *out, const char *name, s32 twips )
{
    u32 size = (u32)(twips < 0 ? -twips : twips);

    out_fmt( out, " %s=\"%s%u.%02upt\"", name, twips < 0 ? "-" : "", size / 20, (size % 20) * 5 );
}

static void odtw_attr( OUT *out, const char *name, const char *value )
{
    out_fmt( out, " %s=\"", name );
    xml_esc( out, value, 1 );
    out_str( out, "\"" );
}

/* a style's name as the file must spell it: letters and digits, and
   anything else as _hh_ */
static void odtw_name( const WDOC *doc, u32 index, char *out )
{
    const char *name;
    u32 used = 0;

    if ( index == 0 || index >= doc->nstyle ) {
        str_cpy( out, "Standard" );
        return;
    }
    for ( name = doc->style[index].name; *name && used < 120; name++ ) {
        if ( (*name >= 'A' && *name <= 'Z') || (*name >= 'a' && *name <= 'z') ||
             (*name >= '0' && *name <= '9' && used > 0) ) {
            out[used++] = *name;
        } else {
            used += (u32)sfmt( out + used, 8, "_%02x_", (u32)(u8)*name );
        }
    }
    if ( used == 0 ) {
        used = (u32)sfmt( out, 16, "Style%u", index );
    }
    out[used] = 0;
}

static const char *odtw_ext( int kind )
{
    switch ( kind ) {
    case IMG_PNG:   return "png";
    case IMG_JPEG:  return "jpg";
    case IMG_GIF:   return "gif";
    case IMG_BMP:
    case IMG_DIB:   return "bmp";
    case IMG_TIFF:  return "tif";
    case IMG_WMF:   return "wmf";
    case IMG_EMF:   return "emf";
    }
    return NULL;
}

static u16 odtw_under( u16 flags )
{
    return (u16)(flags & CF_DUNDER ? CF_DUNDER : flags & CF_WUNDER ? CF_WUNDER : flags & CF_UNDER);
}

static u16 odtw_vert( u16 flags )
{
    return (u16)(flags & CF_SUPER ? CF_SUPER : flags & CF_SUB);
}

/*
 * Text properties: how "chp" differs from "base", or with "base" NULL
 * all of them (a span's style says everything, so that it is the same
 * in whatever paragraph it is used).
 */
static void odtw_text_props( ODTW *w, OUT *out, const CHP *chp, const CHP *base )
{
    const WDOC *doc = w->doc;
    u16 under = odtw_under( chp->flags ), vert = odtw_vert( chp->flags ), was = (u16)(base != NULL ? base->flags : ~chp->flags);
    u32 start = out_tell( out );

    out_str( out, "<style:text-properties" );
    if ( (base == NULL || chp->font != base->font) && chp->font < doc->nfont ) {
        odtw_attr( out, "style:font-name", doc->font[chp->font].name );
    }
    if ( base == NULL || chp->size != base->size ) {
        out_fmt( out, " fo:font-size=\"%u%spt\" style:font-size-asian=\"%u%spt\" style:font-size-complex=\"%u%spt\"",
                 (u32)chp->size / 2, chp->size & 1 ? ".5" : "", (u32)chp->size / 2, chp->size & 1 ? ".5" : "",
                 (u32)chp->size / 2, chp->size & 1 ? ".5" : "" );
    }
    if ( (chp->flags ^ was) & CF_BOLD ) {
        out_fmt( out, " fo:font-weight=\"%s\" style:font-weight-asian=\"%s\" style:font-weight-complex=\"%s\"",
                 chp->flags & CF_BOLD ? "bold" : "normal", chp->flags & CF_BOLD ? "bold" : "normal",
                 chp->flags & CF_BOLD ? "bold" : "normal" );
    }
    if ( (chp->flags ^ was) & CF_ITALIC ) {
        out_fmt( out, " fo:font-style=\"%s\" style:font-style-asian=\"%s\" style:font-style-complex=\"%s\"",
                 chp->flags & CF_ITALIC ? "italic" : "normal", chp->flags & CF_ITALIC ? "italic" : "normal",
                 chp->flags & CF_ITALIC ? "italic" : "normal" );
    }
    if ( base == NULL || under != odtw_under( base->flags ) ) {
        if ( under == 0 ) {
            out_str( out, " style:text-underline-style=\"none\"" );
        } else {
            out_fmt( out, " style:text-underline-style=\"solid\" style:text-underline-type=\"%s\""
                     " style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\""
                     " style:text-underline-mode=\"%s\"", under == CF_DUNDER ? "double" : "single",
                     under == CF_WUNDER ? "skip-white-space" : "continuous" );
        }
    }
    if ( (chp->flags ^ was) & CF_STRIKE ) {
        out_fmt( out, " style:text-line-through-style=\"%s\"", chp->flags & CF_STRIKE ? "solid" : "none" );
    }
    if ( base == NULL || vert != odtw_vert( base->flags ) ) {
        out_fmt( out, " style:text-position=\"%s\"", vert == CF_SUPER ? "super 58%" : vert ? "sub 58%" : "0% 100%" );
    }
    if ( base == NULL || chp->color != base->color ) {
        if ( chp->color == COLOR_AUTO ) {
            out_str( out, " style:use-window-font-color=\"true\"" );
        } else {
            out_fmt( out, " fo:color=\"#%06x\" style:use-window-font-color=\"false\"", chp->color & 0xFFFFFFUL );
        }
    }
    if ( base == NULL || chp->hilite != base->hilite ) {
        if ( chp->hilite >= 1 && chp->hilite <= 16 ) {
            out_fmt( out, " fo:background-color=\"#%06x\"", odt_hilites[chp->hilite - 1] );
        } else {
            out_str( out, " fo:background-color=\"transparent\"" );
        }
    }
    if ( (chp->flags ^ was) & CF_SMALLCAPS ) {
        out_fmt( out, " fo:font-variant=\"%s\"", chp->flags & CF_SMALLCAPS ? "small-caps" : "normal" );
    }
    if ( (chp->flags ^ was) & CF_ALLCAPS ) {
        out_fmt( out, " fo:text-transform=\"%s\"", chp->flags & CF_ALLCAPS ? "uppercase" : "none" );
    }
    if ( (chp->flags ^ was) & CF_HIDDEN ) {
        out_fmt( out, " text:display=\"%s\"", chp->flags & CF_HIDDEN ? "none" : "true" );
    }
    if ( (chp->flags ^ was) & CF_OUTLINE ) {
        out_fmt( out, " style:text-outline=\"%s\"", chp->flags & CF_OUTLINE ? "true" : "false" );
    }
    if ( (chp->flags ^ was) & CF_SHADOW ) {
        out_fmt( out, " fo:text-shadow=\"%s\"", chp->flags & CF_SHADOW ? "1pt 1pt" : "none" );
    }
    if ( out_tell( out ) == start + 22 ) {
        out_seek( out, start );         /* nothing to say */
    } else {
        out_str( out, "/>" );
    }
}

#define PF_SIDES        (PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT)

/*
 * Paragraph properties: how "pap" differs from "base".  "key" is PK_*;
 * "listed" makes the indents said whether they differ or not, for a
 * list's level has indents that would otherwise be the paragraph's.
 */
static void odtw_para_props( ODTW *w, OUT *out, const PAP *pap, const PAP *base, int key, int listed )
{
    static const char *const aligns[4] = { "start", "center", "end", "justify" };
    static const char *const sides[4] = { "top", "bottom", "left", "right" };
    static const u16 bits[4] = { PF_BORDER_TOP, PF_BORDER_BOT, PF_BORDER_LEFT, PF_BORDER_RIGHT };
    static const char *const kinds[4] = { "left", "center", "right", "char" };
    u32 start = out_tell( out ), tenths;
    int index;

    out_str( out, "<style:paragraph-properties" );
    if ( pap->align != base->align ) {
        out_fmt( out, " fo:text-align=\"%s\"", aligns[pap->align & 3] );
    }
    if ( listed || pap->left != base->left || pap->right != base->right || pap->first != base->first ) {
        odtw_len( out, "fo:margin-left", odt_clamp( pap->left, -LEN_MAX, LEN_MAX ) );
        odtw_len( out, "fo:margin-right", odt_clamp( pap->right, -LEN_MAX, LEN_MAX ) );
        odtw_len( out, "fo:text-indent", odt_clamp( pap->first, -LEN_MAX, LEN_MAX ) );
        out_str( out, " style:auto-text-indent=\"false\"" );
    }
    if ( pap->before != base->before || pap->after != base->after ) {
        odtw_len( out, "fo:margin-top", odt_clamp( pap->before, 0, LEN_MAX ) );
        odtw_len( out, "fo:margin-bottom", odt_clamp( pap->after, 0, LEN_MAX ) );
    }
    if ( pap->line != base->line || pap->line_rule != base->line_rule ) {
        if ( pap->line_rule == LS_EXACT ) {
            odtw_len( out, "fo:line-height", odt_clamp( pap->line, 1, LEN_MAX ) );
        } else if ( pap->line_rule == LS_ATLEAST ) {
            odtw_len( out, "style:line-height-at-least", odt_clamp( pap->line, 1, LEN_MAX ) );
        } else {
            /* a whole percentage where that comes back as the same
               number of 240ths, a tenth of one where it would not */
            tenths = ((u32)(pap->line ? pap->line : 240) * 1000 + 120) / 240;
            if ( (((tenths + 5) / 10) * 240 + 50) / 100 == (u32)(pap->line ? pap->line : 240) ) {
                out_fmt( out, " fo:line-height=\"%u%%\"", (tenths + 5) / 10 );
            } else {
                out_fmt( out, " fo:line-height=\"%u.%u%%\"", tenths / 10, tenths % 10 );
            }
        }
    }
    if ( (pap->flags ^ base->flags) & PF_KEEPNEXT ) {
        out_fmt( out, " fo:keep-with-next=\"%s\"", pap->flags & PF_KEEPNEXT ? "always" : "auto" );
    }
    if ( (pap->flags ^ base->flags) & PF_KEEP ) {
        out_fmt( out, " fo:keep-together=\"%s\"", pap->flags & PF_KEEP ? "always" : "auto" );
    }
    if ( ((pap->flags ^ base->flags) & PF_PAGEBEFORE) || (key & PK_BEFORE) ) {
        out_fmt( out, " fo:break-before=\"%s\"", (pap->flags & PF_PAGEBEFORE) || (key & PK_BEFORE) ? "page" : "auto" );
    }
    if ( key & PK_AFTER ) {
        out_str( out, " fo:break-after=\"page\"" );
    }
    if ( (pap->flags ^ base->flags) & PF_SIDES ) {
        for ( index = 0; index < 4; index++ ) {
            out_fmt( out, " fo:border-%s=\"%s\"", sides[index], pap->flags & bits[index] ? "0.5pt solid #000000" : "none" );
        }
        out_str( out, " fo:padding=\"1pt\"" );
    }
    if ( (key & PK_FIRST) && w->doc->page.first_page != 1 ) {
        out_fmt( out, " style:page-number=\"%u\"", (u32)w->doc->page.first_page );
    }
    if ( pap->ntabs && pap->tabs != NULL ) {
        out_str( out, "><style:tab-stops>" );
        for ( index = 0; index < pap->ntabs; index++ ) {
            /* the file counts a tab stop from the paragraph's indent */
            out_str( out, "<style:tab-stop" );
            odtw_len( out, "style:position", odt_clamp( pap->tabs[index].pos - pap->left, -LEN_MAX, LEN_MAX ) );
            out_fmt( out, " style:type=\"%s\"", kinds[pap->tabs[index].kind & 3] );
            if ( (pap->tabs[index].kind & 3) == TAB_DECIMAL ) {
                out_str( out, " style:char=\".\"" );
            }
            if ( pap->tabs[index].lead == LEAD_DOTS ) {
                out_str( out, " style:leader-style=\"dotted\" style:leader-text=\".\"" );
            } else if ( pap->tabs[index].lead == LEAD_DASHES ) {
                out_str( out, " style:leader-style=\"dash\" style:leader-text=\"-\"" );
            } else if ( pap->tabs[index].lead == LEAD_LINE ) {
                out_str( out, " style:leader-style=\"solid\" style:leader-text=\"_\"" );
            }
            out_str( out, "/>" );
        }
        out_str( out, "</style:tab-stops></style:paragraph-properties>" );
    } else if ( out_tell( out ) == start + 27 ) {
        out_seek( out, start );
    } else {
        out_str( out, "/>" );
    }
}

/* what a paragraph needs of its style beyond its properties */
static int odtw_key( const ODTW *w, const WPARA *para )
{
    int key = 0;

    if ( para->len && para->text[para->len - 1] == WCH_PAGE ) {
        key |= PK_AFTER;
    }
    if ( para->len > 1 && para->text[0] == WCH_PAGE ) {
        key |= PK_BEFORE;
    }
    if ( w->first_left && w->content && w->doc->page.first_page != 1 ) {
        key |= PK_FIRST;
    }
    return key;
}

static int odtw_same( const PAP *one, const PAP *two )
{
    return one->align == two->align && one->style == two->style &&
           ((one->flags ^ two->flags) & PF_GIVEN) == 0 && one->left == two->left && one->right == two->right &&
           one->first == two->first && one->before == two->before && one->after == two->after &&
           one->line == two->line && one->line_rule == two->line_rule &&
           (one->list != LIST_NONE) == (two->list != LIST_NONE) && one->ntabs == two->ntabs &&
           (one->ntabs == 0 || mem_cmp( one->tabs, two->tabs, one->ntabs * sizeof( WTAB ) ) == 0);
}

/*
 * The automatic style a paragraph is written with: its number, or -1
 * when its own style says all there is to say.  The first time through
 * the text the styles are made; the second they are found again.
 */
static int odtw_pstyle( ODTW *w, const WPARA *para, int key )
{
    const WDOC *doc = w->doc;
    const WSTYLE *style = &doc->style[para->pap.style < doc->nstyle ? para->pap.style : 0];
    WPSTYLE *fresh;
    PAP look;
    u32 index, want;

    look = style->pap;
    look.style = para->pap.style;
    look.list = LIST_NONE;
    look.ntabs = 0;
    if ( key == 0 && para->pap.list == LIST_NONE && odtw_same( &para->pap, &look ) ) {
        return -1;
    }
    for ( index = 0; index < w->npstyle; index++ ) {
        if ( w->pstyle[index].key == key && odtw_same( &w->pstyle[index].para->pap, &para->pap ) ) {
            return (int)index;
        }
    }
    if ( w->npstyle == w->pstyle_cap ) {
        want = w->pstyle_cap ? w->pstyle_cap * 2 : 32;
        fresh = w->npstyle >= 0x00FFFFFFUL ? NULL : (WPSTYLE *)try_realloc( w->pstyle, want * sizeof( WPSTYLE ) );
        if ( fresh == NULL ) {
            w->failed = 1;
            return -1;
        }
        w->pstyle = fresh;
        w->pstyle_cap = want;
    }
    w->pstyle[w->npstyle].para = para;
    w->pstyle[w->npstyle].key = (u8)key;
    return (int)w->npstyle++;
}

/* ------------------------------------------------------------------ */
/* the text                                                            */
/* ------------------------------------------------------------------ */

/* every list that is open is closed: what comes next is not in one */
static void odtw_lists_close( ODTW *w, int down_to )
{
    while ( w->ldepth > down_to ) {
        if ( w->item_open[w->ldepth] ) {
            PUT( "</text:list-item>" );
            w->item_open[w->ldepth] = 0;
        }
        PUT( "</text:list>" );
        w->ldepth--;
    }
}

/* the lists are brought to where a paragraph needs them, and its item
   opened.  A list of its own style is begun when none is open, or the
   one that is has another kind at the paragraph's level. */
static void odtw_lists_for( ODTW *w, const PAP *pap )
{
    WLIST *list;
    u32 want_cap;
    int listed = pap->list != LIST_NONE && pap->list <= LIST_ROMAN_UPPER;
    int level = pap->level < LIST_LEVELS ? pap->level : LIST_LEVELS - 1, want = listed ? level + 1 : 0, runs_on = 0;

    if ( want && w->ldepth && w->list[w->cur_list - 1].kind[level] != 0xFF &&
         w->list[w->cur_list - 1].kind[level] != pap->list ) {
        odtw_lists_close( w, 0 );
    }
    odtw_lists_close( w, want );
    if ( want == 0 ) {
        return;
    }
    if ( w->ldepth == want && w->item_open[want] ) {
        PUT( "</text:list-item>" );
        w->item_open[want] = 0;
    }
    while ( w->ldepth < want ) {
        if ( w->ldepth == 0 ) {
            /* numbers that carry on from the list before keep its style,
               which is how a file says so */
            runs_on = level == 0 && pap->list_start == 0 && pap->list != LIST_BULLET && w->cur_list != 0 &&
                      (w->list[w->cur_list - 1].kind[0] == 0xFF || w->list[w->cur_list - 1].kind[0] == pap->list);
            if ( !runs_on ) {
                w->cur_list = ++w->nlist;
                if ( w->to == w->scrap ) {
                    if ( w->nlist > w->list_cap ) {
                        want_cap = w->list_cap ? w->list_cap * 2 : 16;
                        list = w->nlist >= 0x00FFFFFFUL ? NULL : (WLIST *)try_realloc( w->list, want_cap * sizeof( WLIST ) );
                        if ( list == NULL ) {
                            w->failed = 1;
                            w->nlist--;
                            w->cur_list = 0;
                            return;
                        }
                        w->list = list;
                        w->list_cap = want_cap;
                    }
                    mem_set( w->list[w->nlist - 1].kind, 0xFF, LIST_LEVELS );
                } else if ( w->nlist > w->list_cap ) {
                    w->failed = 1;      /* the second time through found a list the first had not */
                    w->nlist--;
                    w->cur_list = 0;
                    return;
                }
            }
            out_fmt( w->to, "<text:list text:style-name=\"%sL%u\"%s>", w->prefix, w->cur_list,
                     runs_on ? " text:continue-numbering=\"true\"" : "" );
        } else {
            if ( !w->item_open[w->ldepth] ) {
                PUT( "<text:list-item>" );
                w->item_open[w->ldepth] = 1;
                w->item_count[w->ldepth] = 1;
            }
            PUT( "<text:list>" );
        }
        w->ldepth++;
        w->item_open[w->ldepth] = 0;
        w->item_count[w->ldepth] = 0;
    }
    if ( w->cur_list == 0 ) {
        return;
    }
    if ( w->list[w->cur_list - 1].kind[level] == 0xFF ) {
        w->list[w->cur_list - 1].kind[level] = pap->list;
    }
    /* a list's first item starts at 1 without being told; any other
       start is said */
    if ( pap->list_start > 1 || (pap->list_start == 1 && w->item_count[want]) ) {
        out_fmt( w->to, "<text:list-item text:start-value=\"%u\">", (u32)pap->list_start );
    } else {
        PUT( "<text:list-item>" );
    }
    w->item_open[want] = 1;
    w->item_count[want] = 1;
}

static void odtw_story( ODTW *w, const STORY *story );

/* a run of the paragraph's text, with what is not a letter written as
   the element it is */
static void odtw_run_text( ODTW *w, const u16 *text, u32 len )
{
    static const char *const fields[5] = {
        "<text:page-number text:select-page=\"current\">1</text:page-number>", "<text:page-count>1</text:page-count>",
        "<text:date>1/1/2026</text:date>", "<text:time>12:00</text:time>",
        "<text:file-name text:display=\"name-and-extension\">Document</text:file-name>"
    };
    u32 pos = 0, from, count;

    while ( pos < len ) {
        from = pos;
        while ( pos < len && text[pos] != ' ' && text[pos] != WCH_TAB && text[pos] != WCH_LINE &&
                text[pos] != WCH_PAGE && !WCH_IS_FIELD( text[pos] ) ) {
            pos++;
        }
        if ( pos > from ) {
            xml_esc16( w->to, text + from, pos - from, 0 );
            w->space = 0;
        }
        if ( pos >= len ) {
            break;
        }
        if ( text[pos] == ' ' ) {
            /* one space after a letter is a space; any other is said */
            for ( count = 0; pos < len && text[pos] == ' '; pos++ ) {
                count++;
            }
            if ( !w->space ) {
                PUT( " " );
                count--;
            }
            if ( count == 1 ) {
                PUT( "<text:s/>" );
            } else if ( count > 1 ) {
                out_fmt( w->to, "<text:s text:c=\"%u\"/>", count );
            }
            w->space = 1;
            continue;
        }
        if ( text[pos] == WCH_TAB ) {
            PUT( "<text:tab/>" );
        } else if ( WCH_IS_FIELD( text[pos] ) ) {
            PUT( fields[text[pos] - WCH_PAGENUM] );
        } else {
            PUT( "<text:line-break/>" );        /* a page break in mid paragraph has no other form */
        }
        w->space = 0;
        pos++;
    }
}

static void odtw_para( ODTW *w, const WPARA *para )
{
    char name[160];
    const WDOC *doc = w->doc;
    const WSTYLE *style = &doc->style[para->pap.style < doc->nstyle ? para->pap.style : 0];
    const CHP *chp;
    const WOBJ *item;
    const char *tag = "text:p";
    u8 saved_open[LIST_LEVELS + 1], saved_count[LIST_LEVELS + 1];
    u32 off, run, len = para->len, from = 0, saved_list;
    int saved_depth;
    int key = odtw_key( w, para ), number = odtw_pstyle( w, para, key ), obj, span, heading = 0;

    w->first_left = 0;
    odtw_lists_for( w, &para->pap );
    if ( style->kind >= STY_HEADING1 && style->kind <= STY_HEADING6 && para->pap.style != 0 ) {
        heading = style->kind - STY_HEADING1 + 1;
        tag = "text:h";
    }
    if ( number >= 0 ) {
        out_fmt( w->to, "<%s text:style-name=\"%sP%u\"", tag, w->prefix, (u32)number + 1 );
    } else {
        odtw_name( doc, para->pap.style, name );
        out_fmt( w->to, "<%s text:style-name=\"%s\"", tag, name );
    }
    if ( heading ) {
        out_fmt( w->to, " text:outline-level=\"%d\"", heading );
    }
    PUT( ">" );
    if ( key & PK_AFTER ) {
        len--;
    }
    if ( key & PK_BEFORE ) {
        from = 1;
    }
    w->space = 1;
    for ( off = from; off < len; off += run ) {
        run = wd_run( para, off );
        if ( run > len - off ) {
            run = len - off;
        }
        chp = wd_chp_get( doc, para->attr[off] );
        span = mem_cmp( chp, &style->chp, sizeof( CHP ) ) != 0;
        if ( para->text[off] == WCH_OBJ ) {
            obj = wd_obj_at( para, off );
            if ( obj < 0 || (u32)obj >= doc->nobj ) {
                continue;
            }
            item = &doc->obj[obj];
            if ( item->kind == OBJ_IMAGE && odtw_ext( item->fmt ) != NULL && item->data != NULL && item->len ) {
                w->img_used[obj] = 1;
                out_fmt( w->to, "<draw:frame draw:name=\"Picture%u\" text:anchor-type=\"as-char\" draw:z-index=\"0\"",
                         (u32)obj + 1 );
                odtw_len( w->to, "svg:width", odt_clamp( item->width, 1, 0x7FFFFL ) );
                odtw_len( w->to, "svg:height", odt_clamp( item->height, 1, 0x7FFFFL ) );
                out_fmt( w->to, "><draw:image xlink:href=\"Pictures/img%u.%s\" xlink:type=\"simple\""
                         " xlink:show=\"embed\" xlink:actuate=\"onLoad\"/></draw:frame>", (u32)obj + 1,
                         odtw_ext( item->fmt ) );
                w->space = 0;
            } else if ( item->kind == OBJ_NOTE && w->content && item->story != NULL ) {
                if ( span ) {
                    w->chp_used[para->attr[off]] = 1;
                    out_fmt( w->to, "<text:span text:style-name=\"%sT%u\">", w->prefix, (u32)para->attr[off] );
                }
                w->nnote++;
                out_fmt( w->to, "<text:note text:id=\"%s%u\" text:note-class=\"%s\"><text:note-citation>%u"
                         "</text:note-citation><text:note-body>", item->fmt == NOTE_END ? "edn" : "ftn", w->nnote,
                         item->fmt == NOTE_END ? "endnote" : "footnote", w->nnote );
                /* the note's paragraphs are in no list of this one's */
                saved_depth = w->ldepth;
                saved_list = w->cur_list;
                mem_cpy( saved_open, w->item_open, sizeof( saved_open ) );
                mem_cpy( saved_count, w->item_count, sizeof( saved_count ) );
                w->ldepth = 0;
                w->content = 0;         /* a note in a note is not written */
                odtw_story( w, item->story );
                w->content = 1;
                w->ldepth = saved_depth;
                w->cur_list = saved_list;
                mem_cpy( w->item_open, saved_open, sizeof( saved_open ) );
                mem_cpy( w->item_count, saved_count, sizeof( saved_count ) );
                PUT( "</text:note-body></text:note>" );
                if ( span ) {
                    PUT( "</text:span>" );
                }
                w->space = 0;
            }
            continue;
        }
        if ( span ) {
            w->chp_used[para->attr[off]] = 1;
            out_fmt( w->to, "<text:span text:style-name=\"%sT%u\">", w->prefix, (u32)para->attr[off] );
        }
        odtw_run_text( w, para->text + off, run );
        if ( span ) {
            PUT( "</text:span>" );
        }
    }
    out_fmt( w->to, "</%s>", tag );
}

static int odtw_grid_add( s32 *grid, int count, s32 pos )
{
    int at, move;

    for ( at = 0; at < count && grid[at] < pos; at++ ) {
    }
    if ( (at < count && grid[at] == pos) || count > MAX_GRIDW ) {
        return count;
    }
    for ( move = count; move > at; move-- ) {
        grid[move] = grid[move - 1];
    }
    grid[at] = pos;
    return count + 1;
}

static int odtw_grid_at( const s32 *grid, int count, s32 pos )
{
    int at = 0;

    while ( at + 1 < count && grid[at + 1] <= pos ) {
        at++;
    }
    return at;
}

/* a cell "span" columns wide: its start, and the covered cells a file
   has after one that is more than a column wide are owed at its end */
static void odtw_cell_open( ODTW *w, int span, int ruled )
{
    out_fmt( w->to, "<table:table-cell table:style-name=\"%sCell%s\" office:value-type=\"string\"", w->prefix,
             ruled ? "R" : "N" );
    if ( span > 1 ) {
        out_fmt( w->to, " table:number-columns-spanned=\"%d\"", span );
    }
    PUT( ">" );
}

static void odtw_cell_close( ODTW *w, int span )
{
    odtw_lists_close( w, 0 );
    PUT( "</table:table-cell>" );
    for ( ; span > 1; span-- ) {
        PUT( "<table:covered-table-cell/>" );
    }
}

static void odtw_row( ODTW *w, const STORY *story, u32 from, u32 to, const TROW *row, const s32 *grid, int ngrid )
{
    s32 prev = row->left, edge;
    u32 index = from;
    int cell, col = odtw_grid_at( grid, ngrid, row->left ), next, ncells = row->ncells, span;

    if ( ncells > MAX_CELLS ) {
        ncells = MAX_CELLS;
    }
    PUT( "<table:table-row>" );
    if ( col > 0 ) {
        /* a row that starts further in: an empty cell with no lines fills the gap */
        odtw_cell_open( w, col, 0 );
        PUT( "<text:p/>" );
        odtw_cell_close( w, col );
    }
    for ( cell = 0; cell < ncells; cell++ ) {
        edge = row->edge[cell] > prev ? row->edge[cell] : prev + 1;
        next = odtw_grid_at( grid, ngrid, edge );
        span = next > col ? next - col : 1;
        odtw_cell_open( w, span, row->borders );
        col = next > col ? next : col;
        prev = edge;
        if ( index > to ) {
            PUT( "<text:p/>" );
        }
        while ( index <= to ) {
            odtw_para( w, story->para[index] );
            index++;
            if ( (story->para[index - 1]->pap.flags & PF_CELLEND) && cell + 1 < ncells ) {
                break;
            }
        }
        odtw_cell_close( w, span );
    }
    PUT( "</table:table-row>" );
}

/* the table that starts at paragraph "from" -> the paragraph after it.
   Its own styles - its width and each column's - are written the first
   time through, into the part itself. */
static u32 odtw_table( ODTW *w, const STORY *story, u32 from )
{
    s32 grid[MAX_GRIDW + 2];
    TROW whole;
    const PAP *pap;
    const TROW *row;
    s32 prev;
    u32 to, index, row_from;
    int ngrid = 0, cell, heads = 1, in_heads = 0;

    odtw_lists_close( w, 0 );
    for ( to = from; to < story->count && (story->para[to]->pap.flags & PF_INTABLE); to++ ) {
        pap = &story->para[to]->pap;
        if ( !(pap->flags & PF_ROWEND) || pap->row == NULL ) {
            continue;
        }
        row = pap->row;
        ngrid = odtw_grid_add( grid, ngrid, row->left );
        prev = row->left;
        for ( cell = 0; cell < row->ncells && cell < MAX_CELLS; cell++ ) {
            prev = row->edge[cell] > prev ? row->edge[cell] : prev + 1;
            ngrid = odtw_grid_add( grid, ngrid, prev );
        }
    }
    if ( ngrid < 2 ) {
        for ( index = from; index < to; index++ ) {
            odtw_para( w, story->para[index] );
        }
        return to;
    }
    w->ntable++;
    if ( w->to == w->scrap ) {
        out_fmt( w->part, "<style:style style:name=\"%sTbl%u\" style:family=\"table\"><style:table-properties"
                 " table:align=\"left\"", w->prefix, w->ntable );
        odtw_len( w->part, "style:width", odt_clamp( grid[ngrid - 1] - grid[0], 1, LEN_MAX * 2 ) );
        odtw_len( w->part, "fo:margin-left", odt_clamp( grid[0], -LEN_MAX, LEN_MAX ) );
        out_str( w->part, "/></style:style>" );
        for ( cell = 0; cell + 1 < ngrid; cell++ ) {
            out_fmt( w->part, "<style:style style:name=\"%sTbl%u.C%d\" style:family=\"table-column\">"
                     "<style:table-column-properties", w->prefix, w->ntable, cell + 1 );
            odtw_len( w->part, "style:column-width", odt_clamp( grid[cell + 1] - grid[cell], 1, LEN_MAX * 2 ) );
            out_str( w->part, "/></style:style>" );
        }
    }
    out_fmt( w->to, "<table:table table:name=\"%sTable%u\" table:style-name=\"%sTbl%u\">", w->prefix, w->ntable,
             w->prefix, w->ntable );
    for ( cell = 0; cell + 1 < ngrid; cell++ ) {
        out_fmt( w->to, "<table:table-column table:style-name=\"%sTbl%u.C%d\"/>", w->prefix, w->ntable, cell + 1 );
    }
    row_from = from;
    for ( index = from; index < to; index++ ) {
        pap = &story->para[index]->pap;
        if ( !(pap->flags & PF_ROWEND) || pap->row == NULL ) {
            continue;
        }
        /* the rows at the top that repeat on each page go in a group */
        if ( heads && pap->row->header && !in_heads ) {
            PUT( "<table:table-header-rows>" );
            in_heads = 1;
        } else if ( !pap->row->header && in_heads ) {
            PUT( "</table:table-header-rows>" );
            in_heads = 0;
        }
        if ( !pap->row->header ) {
            heads = 0;
        }
        odtw_row( w, story, row_from, index, pap->row, grid, ngrid );
        row_from = index + 1;
        odtw_drain( w, 0 );
    }
    if ( in_heads ) {
        PUT( "</table:table-header-rows>" );
    }
    if ( row_from < to ) {
        mem_set( &whole, 0, sizeof( whole ) );
        whole.ncells = 1;
        whole.left = grid[0];
        whole.edge[0] = grid[ngrid - 1];
        odtw_row( w, story, row_from, to - 1, &whole, grid, ngrid );
    }
    PUT( "</table:table>" );
    return to;
}

static void odtw_story( ODTW *w, const STORY *story )
{
    u32 index = 0;

    while ( index < story->count ) {
        if ( story->para[index]->pap.flags & PF_INTABLE ) {
            index = odtw_table( w, story, index );
        } else {
            odtw_para( w, story->para[index] );
            index++;
        }
        odtw_drain( w, 0 );
    }
    odtw_lists_close( w, 0 );
}

/* ------------------------------------------------------------------ */
/* the styles of a part, and the parts                                 */
/* ------------------------------------------------------------------ */

static void odtw_fonts( ODTW *w )
{
    static const char *const generic[7] = { "", "roman", "swiss", "modern", "script", "decorative", "" };
    const WDOC *doc = w->doc;
    u32 index;

    PUT( "<office:font-face-decls>" );
    for ( index = 0; index < doc->nfont; index++ ) {
        PUT( "<style:font-face" );
        odtw_attr( w->to, "style:name", doc->font[index].name );
        PUT( " svg:font-family=\"&apos;" );
        xml_esc( w->to, doc->font[index].name, 1 );
        PUT( "&apos;\"" );
        if ( doc->font[index].family >= FAM_ROMAN && doc->font[index].family <= FAM_DECOR ) {
            out_fmt( w->to, " style:font-family-generic=\"%s\"", generic[doc->font[index].family] );
        }
        out_fmt( w->to, " style:font-pitch=\"%s\"/>", doc->font[index].fixed ? "fixed" : "variable" );
    }
    PUT( "</office:font-face-decls>" );
}

/* the styles the text just gone through needs: one for each look of
   paragraph, one for each look of character, one for each list */
static void odtw_auto_styles( ODTW *w )
{
    static const char *const formats[7] = { "", "", "1", "a", "A", "i", "I" };
    char name[160];
    const WDOC *doc = w->doc;
    const WPARA *para;
    const WSTYLE *style;
    u32 index;
    int level, kind, fill;

    for ( index = 0; index < w->npstyle; index++ ) {
        para = w->pstyle[index].para;
        style = &doc->style[para->pap.style < doc->nstyle ? para->pap.style : 0];
        odtw_name( doc, para->pap.style, name );
        out_fmt( w->part, "<style:style style:name=\"%sP%u\" style:family=\"paragraph\" style:parent-style-name=\"%s\"%s>",
                 w->prefix, index + 1, name,
                 (w->pstyle[index].key & PK_FIRST) ? " style:master-page-name=\"Standard\"" : "" );
        odtw_para_props( w, w->part, &para->pap, &style->pap, w->pstyle[index].key, para->pap.list != LIST_NONE );
        out_str( w->part, "</style:style>" );
    }
    for ( index = 0; index < doc->nchp; index++ ) {
        if ( w->chp_used[index] ) {
            out_fmt( w->part, "<style:style style:name=\"%sT%u\" style:family=\"text\">", w->prefix, index );
            odtw_text_props( w, w->part, &doc->chp[index], NULL );
            out_str( w->part, "</style:style>" );
            w->chp_used[index] = 0;
        }
    }
    for ( index = 0; index < w->nlist; index++ ) {
        fill = LIST_BULLET;
        for ( level = 0; level < LIST_LEVELS; level++ ) {
            if ( w->list[index].kind[level] != 0xFF ) {
                fill = w->list[index].kind[level];
                break;
            }
        }
        out_fmt( w->part, "<text:list-style style:name=\"%sL%u\">", w->prefix, index + 1 );
        for ( level = 0; level < LIST_LEVELS; level++ ) {
            kind = w->list[index].kind[level] != 0xFF ? w->list[index].kind[level] : fill;
            if ( kind < LIST_BULLET || kind > LIST_ROMAN_UPPER ) {
                kind = LIST_BULLET;
            }
            if ( kind == LIST_BULLET ) {
                out_fmt( w->part, "<text:list-level-style-bullet text:level=\"%d\" text:bullet-char=\"\xE2\x80\xA2\">",
                         level + 1 );
            } else {
                out_fmt( w->part, "<text:list-level-style-number text:level=\"%d\" style:num-suffix=\".\""
                         " style:num-format=\"%s\">", level + 1, formats[kind] );
            }
            out_str( w->part, "<style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\">"
                     "<style:list-level-label-alignment text:label-followed-by=\"listtab\"" );
            odtw_len( w->part, "text:list-tab-stop-position", 360L * (level + 2) );
            odtw_len( w->part, "fo:text-indent", -360 );
            odtw_len( w->part, "fo:margin-left", 360L * (level + 2) );
            out_fmt( w->part, "/></style:list-level-properties></text:list-level-style-%s>",
                     kind == LIST_BULLET ? "bullet" : "number" );
        }
        out_str( w->part, "</text:list-style>" );
    }
    /* a cell with lines round it, and one without */
    out_fmt( w->part, "<style:style style:name=\"%sCellR\" style:family=\"table-cell\"><style:table-cell-properties"
             " fo:padding=\"2pt\" fo:border=\"0.5pt solid #000000\"/></style:style>"
             "<style:style style:name=\"%sCellN\" style:family=\"table-cell\"><style:table-cell-properties"
             " fo:padding=\"2pt\" fo:border=\"none\"/></style:style>", w->prefix, w->prefix );
}

/* the text gone through twice: once to nowhere, to find what styles it
   needs - they are written before it - and once into the part */
static void odtw_twice_begin( ODTW *w, const char *prefix, int content )
{
    w->prefix = prefix;
    w->content = content;
    w->npstyle = w->nlist = w->cur_list = w->ntable = w->nnote = 0;
    w->ldepth = 0;
    w->first_left = content;
    w->to = w->scrap;
}

static void odtw_twice_again( ODTW *w )
{
    odtw_auto_styles( w );
    w->nlist = w->cur_list = w->ntable = w->nnote = 0;
    w->ldepth = 0;
    w->first_left = w->content;
    w->to = w->part;
}

static void odtw_content( ODTW *w )
{
    odtw_begin( w, "content.xml" );
    PUT( "<office:document-content" ODF_SPACES );
    odtw_fonts( w );
    PUT( "<office:automatic-styles>" );
    odtw_twice_begin( w, "", 1 );
    odtw_story( w, &w->doc->body );
    odtw_twice_again( w );
    PUT( "</office:automatic-styles><office:body><office:text>" );
    odtw_story( w, &w->doc->body );
    if ( w->doc->body.count == 0 ) {
        PUT( "<text:p text:style-name=\"Standard\"/>" );
    }
    PUT( "</office:text></office:body></office:document-content>" );
    odtw_finish( w );
}

/* styles.xml: the document's styles, the page, the header and footer */
static void odtw_styles( ODTW *w )
{
    char name[160];
    const WDOC *doc = w->doc;
    const PAGESETUP *page = &doc->page;
    const WSTYLE *style;
    CHP plain;
    PAP none;
    s32 room;
    u32 index;
    int head;

    mem_set( &plain, 0, sizeof( plain ) );
    plain.size = 20;
    plain.color = COLOR_AUTO;
    pap_init( &none );
    odtw_begin( w, "styles.xml" );
    PUT( "<office:document-styles" ODF_SPACES );
    odtw_fonts( w );
    /* the defaults: what "plain" and "none" above are */
    PUT( "<office:styles><style:default-style style:family=\"paragraph\"><style:paragraph-properties" );
    odtw_len( w->to, "style:tab-stop-distance", odt_clamp( doc->deftab, 20, LEN_MAX ) );
    PUT( " fo:margin-top=\"0pt\" fo:margin-bottom=\"0pt\" fo:line-height=\"100%\"/><style:text-properties" );
    odtw_attr( w->to, "style:font-name", doc->font[0].name );
    PUT( " fo:font-size=\"10pt\" style:font-size-asian=\"10pt\" style:font-size-complex=\"10pt\"/></style:default-style>" );
    for ( index = 0; index < doc->nstyle; index++ ) {
        style = &doc->style[index];
        odtw_name( doc, index, name );
        head = style->kind >= STY_HEADING1 && style->kind <= STY_HEADING6 ? style->kind - STY_HEADING1 + 1 : 0;
        out_fmt( w->to, "<style:style style:name=\"%s\"", name );
        odtw_attr( w->to, "style:display-name", index == 0 ? "Default Paragraph Style" : style->name );
        PUT( " style:family=\"paragraph\"" );
        if ( index != 0 ) {
            PUT( " style:parent-style-name=\"Standard\"" );
        }
        if ( index != 0 && head ) {
            out_fmt( w->to, " style:next-style-name=\"Standard\" style:default-outline-level=\"%d\"", head );
        }
        PUT( ">" );
        odtw_para_props( w, w->to, &style->pap, index == 0 ? &none : &doc->style[0].pap, 0, 0 );
        odtw_text_props( w, w->to, &style->chp, index == 0 ? &plain : &doc->style[0].chp );
        PUT( "</style:style>" );
    }
    PUT( "</office:styles><office:automatic-styles><style:page-layout style:name=\"pm1\"><style:page-layout-properties" );
    odtw_len( w->to, "fo:page-width", odt_clamp( page->width, 720, LEN_MAX ) );
    odtw_len( w->to, "fo:page-height", odt_clamp( page->height, 720, LEN_MAX ) );
    out_fmt( w->to, " style:print-orientation=\"%s\"", page->landscape ? "landscape" : "portrait" );
    /* with a header the margin is to the header, and the header's
       height brings the text to where its own margin is */
    odtw_len( w->to, "fo:margin-top", odt_clamp( doc->header.count && page->header < page->top ? page->header : page->top, 0, LEN_MAX ) );
    odtw_len( w->to, "fo:margin-bottom", odt_clamp( doc->footer.count && page->footer < page->bottom ? page->footer : page->bottom, 0, LEN_MAX ) );
    odtw_len( w->to, "fo:margin-left", odt_clamp( page->left, 0, LEN_MAX ) );
    odtw_len( w->to, "fo:margin-right", odt_clamp( page->right, 0, LEN_MAX ) );
    PUT( "/>" );
    if ( doc->header.count ) {
        room = page->header < page->top ? page->top - page->header : 0;
        PUT( "<style:header-style><style:header-footer-properties" );
        odtw_len( w->to, "fo:min-height", odt_clamp( room, 0, LEN_MAX ) );
        PUT( " fo:margin-bottom=\"0pt\"/></style:header-style>" );
    }
    if ( doc->footer.count ) {
        room = page->footer < page->bottom ? page->bottom - page->footer : 0;
        PUT( "<style:footer-style><style:header-footer-properties" );
        odtw_len( w->to, "fo:min-height", odt_clamp( room, 0, LEN_MAX ) );
        PUT( " fo:margin-top=\"0pt\"/></style:footer-style>" );
    }
    PUT( "</style:page-layout>" );
    odtw_twice_begin( w, "M", 0 );
    odtw_story( w, &doc->header );
    odtw_story( w, &doc->footer );
    odtw_twice_again( w );
    PUT( "</office:automatic-styles><office:master-styles>"
         "<style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\">" );
    if ( doc->header.count ) {
        PUT( "<style:header>" );
        odtw_story( w, &doc->header );
        PUT( "</style:header>" );
    }
    if ( doc->footer.count ) {
        PUT( "<style:footer>" );
        odtw_story( w, &doc->footer );
        PUT( "</style:footer>" );
    }
    PUT( "</style:master-page></office:master-styles></office:document-styles>" );
    odtw_finish( w );
}

static void odtw_info( ODTW *w, const char *tag, const char *text )
{
    if ( text[0] ) {
        out_fmt( w->to, "<%s>", tag );
        xml_esc( w->to, text, 0 );
        out_fmt( w->to, "</%s>", tag );
    }
}

static void odtw_meta( ODTW *w )
{
    const WDOC *doc = w->doc;
    DATETIME now;

    sys_now( &now );
    odtw_begin( w, "meta.xml" );
    PUT( "<office:document-meta" ODF_SPACES "<office:meta><meta:generator>PMWRITE/1.0</meta:generator>" );
    odtw_info( w, "dc:title", doc->title );
    odtw_info( w, "dc:subject", doc->subject );
    odtw_info( w, "meta:initial-creator", doc->author );
    odtw_info( w, "dc:creator", doc->author );
    odtw_info( w, "meta:keyword", doc->keywords );
    out_fmt( w->to, "<meta:creation-date>%04u-%02u-%02uT%02u:%02u:%02u</meta:creation-date>", (u32)now.year,
             (u32)now.month, (u32)now.day, (u32)now.hour, (u32)now.minute, (u32)now.second );
    PUT( "</office:meta></office:document-meta>" );
    odtw_finish( w );
}

static void odtw_manifest( ODTW *w )
{
    static const char *const types[11] = {
        "", "image/png", "image/jpeg", "image/bmp", "image/gif", "image/tiff", "image/x-wmf", "image/x-emf", "", "",
        "image/bmp"
    };
    const WDOC *doc = w->doc;
    u32 index;

    odtw_begin( w, "META-INF/manifest.xml" );
    PUT( "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\""
         " manifest:version=\"1.2\"><manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\""
         " manifest:media-type=\"" MIME_ODT "\"/>"
         "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
         "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
         "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>" );
    for ( index = 0; index < doc->nobj; index++ ) {
        if ( w->img_used[index] ) {
            out_fmt( w->to, "<manifest:file-entry manifest:full-path=\"Pictures/img%u.%s\" manifest:media-type=\"%s\"/>",
                     index + 1, odtw_ext( doc->obj[index].fmt ), types[doc->obj[index].fmt <= IMG_DIB ? doc->obj[index].fmt : 0] );
        }
    }
    PUT( "</manifest:manifest>" );
    odtw_finish( w );
}

/* a picture's bytes into the archive; a bare DIB with the header that
   makes it a .BMP file */
static void odtw_media( ODTW *w, u32 obj )
{
    char name[48];
    const WOBJ *image = &w->doc->obj[obj];
    OUT *bmp;
    u8 *data;
    u32 len, head, colors, bits;
    int packed = image->fmt == IMG_PNG || image->fmt == IMG_JPEG || image->fmt == IMG_GIF;

    sfmt( name, sizeof( name ), "Pictures/img%u.%s", obj + 1, odtw_ext( image->fmt ) );
    if ( image->fmt != IMG_DIB || image->len < 40 ) {
        if ( !zipw_add( w->zipw, name, image->data, image->len, packed ) ) {
            w->failed = 1;
        }
        return;
    }
    head = LE32( image->data );
    bits = LE16( image->data + 14 );
    colors = LE32( image->data + 32 );
    if ( colors == 0 && bits <= 8 ) {
        colors = 1UL << bits;
    }
    if ( head > image->len || colors > 256 ) {
        head = 40;
        colors = 0;
    }
    bits = 14 + head + colors * 4 + (head == 40 && LE32( image->data + 16 ) == 3 ? 12 : 0);
    bmp = out_mem();
    out_put( bmp, "BM", 2 );
    out_le32( bmp, image->len + 14 );
    out_le32( bmp, 0 );
    out_le32( bmp, bits );
    out_put( bmp, image->data, image->len );
    data = out_data( bmp, &len );
    if ( out_error( bmp ) || !zipw_add( w->zipw, name, data, len, 0 ) ) {
        w->failed = 1;
    }
    out_close( bmp );
}

static int odt_save( WDOC *doc, OUT *out, int variant )
{
    ODTW *w;
    u32 index;
    int rc = WE_OK;

    (void)variant;
    w = (ODTW *)try_alloc( sizeof( ODTW ) );
    if ( w == NULL ) {
        return WE_NOMEM;
    }
    mem_set( w, 0, sizeof( ODTW ) );
    w->doc = doc;
    w->part = out_mem();
    w->scrap = out_mem();
    w->zipw = zipw_new( out );
    w->chp_used = (u8 *)try_alloc( doc->nchp + 1 );
    w->img_used = (u8 *)try_alloc( doc->nobj + 1 );
    if ( w->part == NULL || w->scrap == NULL || w->zipw == NULL || w->chp_used == NULL || w->img_used == NULL ) {
        w->failed = 1;
    } else {
        mem_set( w->chp_used, 0, doc->nchp + 1 );
        mem_set( w->img_used, 0, doc->nobj + 1 );
        w->to = w->part;
        /* first, and as it is: how a reader knows the file at a glance */
        if ( !zipw_add( w->zipw, "mimetype", (const u8 *)MIME_ODT, sizeof( MIME_ODT ) - 1, 1 ) ) {
            w->failed = 1;
        }
        odtw_content( w );
        odtw_styles( w );
        odtw_meta( w );
        for ( index = 0; index < doc->nobj; index++ ) {
            if ( w->img_used[index] ) {
                odtw_media( w, index );
            }
        }
        odtw_manifest( w );
    }
    if ( w->zipw != NULL && !zipw_end( w->zipw ) ) {
        w->failed = 1;
    }
    if ( w->failed || out_error( out ) ) {
        rc = out_error( out ) != 0 && out_error( out ) != WE_NOMEM ? WE_IO : WE_NOMEM;
    }
    out_close( w->part );
    out_close( w->scrap );
    xfree( w->pstyle );
    xfree( w->list );
    xfree( w->chp_used );
    xfree( w->img_used );
    xfree( w );
    return rc;
}

const FORMAT fmt_odt = {
    FMT_ODT, "OpenDocument Text", "ODT;OTT;FODT;SXW", odt_detect, odt_load, odt_save, NULL, 0
};
