/*
 * F_DOCX.C - Office Open XML: the .docx of Word 2007 and later.
 *
 * A .docx is a ZIP of XML "parts".  A part called _rels/.rels says
 * which part is the document; that part's own relationships say where
 * its styles, numbering, notes, headers, footers and pictures are.
 * Nothing here goes by a part's name: Word's are always word/..., but
 * other writers' are not.
 *
 * READING.  THE STYLES COME FIRST, because a paragraph in the file
 * says only how it differs from its style, the style only how it
 * differs from the one it is based on, and that one from the
 * document's defaults - and PMWRITE's paragraphs hold the answer, not
 * the chain.  So each style is kept as "what it gives" (a set of
 * properties with a mask of which are given), the chains are folded
 * together once, and a paragraph is: the defaults, its table's style,
 * its own style, its list level's indents, and what it says itself, in
 * that order.  A run is the paragraph's result, its character style,
 * and what it says itself.  The on/off properties (bold and the like)
 * follow Word's rule: a character style that says "on" turns over
 * what the paragraph's style had.
 *
 * THE BODY IS READ IN ONE PASS, WITH NO RECURSION, however deep the
 * file nests tables and text boxes: an element that is only a wrapper
 * (a content control, a hyperlink, an insertion) is simply walked
 * into, one that must not be read (deleted text, old properties) is
 * walked over, and the few that mean something set a little state.
 * A table inside a table cell becomes paragraphs of that cell.  A text
 * box's paragraphs are put where the box is anchored, between the two
 * halves of the paragraph that held it.  A field Word works out itself
 * (PAGE, NUMPAGES, DATE, TIME, FILENAME) becomes the document's own
 * field character; any other keeps the text Word last gave it.
 *
 * SECTIONS.  The document has one page setup: the first section's.  A
 * later section that starts a page puts a page break at the end of
 * the paragraph before it, which is where Word itself shows one.
 *
 * WRITING is the same package as plain as it can be made: the
 * document's styles written out in full, each paragraph and run with
 * what differs from its style, lists as numbering definitions made
 * for the purpose, and every part deflated.  The big parts are never
 * whole in memory: they are handed to ZIP.C as they fill (see
 * docx_drain).
 *
 * NOT READ: Word 2003's XML (.xml with w:wordDocument), a document
 * with a password (that is an OLE2 file, and F_DOC.C's to report),
 * comments, charts and SmartArt (LOST_OBJECTS), columns.
 */
#include "fmt.h"
#include "sys.h"
#include "zip.h"
#include "xml.h"

/* PW_PEAK: ZIPHOST.EXE's build, which reports the most heap a file took */
#ifdef PW_PEAK
u32 pw_peak_heap;
#define PEAK()  if ( heap_in_use() > pw_peak_heap ) { pw_peak_heap = heap_in_use(); }
#else
#define PEAK()
#endif

#define EMU_PER_TWIP    635

/* ------------------------------------------------------------------ */
/* the namespaces, and the names in Word's own                         */
/* ------------------------------------------------------------------ */

enum { NS_W = 1, NS_R, NS_WP, NS_A, NS_V, NS_O, NS_MC, NS_M, NS_PR, NS_CT, NS_CP, NS_DC, NS_PKG };

/* each as "transitional" files spell it and as "strict" ones do */
static const struct {
    const char *uri;
    int id;
} docx_spaces[] = {
    { "http://schemas.openxmlformats.org/wordprocessingml/2006/main", NS_W },
    { "http://purl.oclc.org/ooxml/wordprocessingml/main", NS_W },
    { "http://schemas.openxmlformats.org/officeDocument/2006/relationships", NS_R },
    { "http://purl.oclc.org/ooxml/officeDocument/relationships", NS_R },
    { "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing", NS_WP },
    { "http://purl.oclc.org/ooxml/drawingml/wordprocessingDrawing", NS_WP },
    { "http://schemas.openxmlformats.org/drawingml/2006/main", NS_A },
    { "http://purl.oclc.org/ooxml/drawingml/main", NS_A },
    { "urn:schemas-microsoft-com:vml", NS_V },
    { "urn:schemas-microsoft-com:office:office", NS_O },
    { "http://schemas.openxmlformats.org/markup-compatibility/2006", NS_MC },
    { "http://schemas.openxmlformats.org/officeDocument/2006/math", NS_M },
    { "http://purl.oclc.org/ooxml/officeDocument/math", NS_M },
    { "http://schemas.openxmlformats.org/package/2006/relationships", NS_PR },
    { "http://schemas.openxmlformats.org/package/2006/content-types", NS_CT },
    { "http://schemas.openxmlformats.org/package/2006/metadata/core-properties", NS_CP },
    { "http://purl.org/dc/elements/1.1/", NS_DC },
    { "http://schemas.microsoft.com/office/2006/xmlPackage", NS_PKG }
};

/*
 * The elements of the w: namespace that are looked at.  One list makes
 * both the numbers and the names, so they cannot get out of step.
 */
#define W_TAGS \
    X( P, "p" ) X( R, "r" ) X( T, "t" ) X( PPR, "pPr" ) X( RPR, "rPr" ) X( TAB, "tab" ) \
    X( BR, "br" ) X( CR, "cr" ) X( NBHYPHEN, "noBreakHyphen" ) X( SOFTHYPHEN, "softHyphen" ) \
    X( SYM, "sym" ) X( PTAB, "ptab" ) X( PGNUM, "pgNum" ) \
    X( FOOTREF, "footnoteReference" ) X( ENDREF, "endnoteReference" ) \
    X( FLDSIMPLE, "fldSimple" ) X( FLDCHAR, "fldChar" ) X( INSTRTEXT, "instrText" ) \
    X( DELTEXT, "delText" ) X( DELINSTR, "delInstrText" ) X( DEL, "del" ) \
    X( MOVEFROM, "moveFrom" ) X( INS, "ins" ) X( MOVETO, "moveTo" ) \
    X( TBL, "tbl" ) X( TBLPR, "tblPr" ) X( TBLGRID, "tblGrid" ) X( TR, "tr" ) X( TRPR, "trPr" ) \
    X( TBLPREX, "tblPrEx" ) X( TC, "tc" ) X( TCPR, "tcPr" ) X( SECTPR, "sectPr" ) \
    X( TXBX, "txbxContent" ) X( DRAWING, "drawing" ) X( PICT, "pict" ) X( OBJECT, "object" ) \
    X( FOOTNOTE, "footnote" ) X( ENDNOTE, "endnote" ) X( SDTPR, "sdtPr" ) \
    X( SDTENDPR, "sdtEndPr" ) X( RUBYPR, "rubyPr" ) X( RT, "rt" ) X( CUSTOMXMLPR, "customXmlPr" ) \
    X( SMARTTAGPR, "smartTagPr" ) X( FLDDATA, "fldData" ) X( BINDATA, "binData" ) \
    X( COMMENTREF, "commentReference" ) X( COMMENT, "comment" ) X( PPRCHANGE, "pPrChange" ) \
    X( RPRCHANGE, "rPrChange" ) X( ALTCHUNK, "altChunk" ) \
    X( PSTYLE, "pStyle" ) X( JC, "jc" ) X( IND, "ind" ) X( SPACING, "spacing" ) \
    X( KEEPNEXT, "keepNext" ) X( KEEPLINES, "keepLines" ) X( PAGEBEFORE, "pageBreakBefore" ) \
    X( TABS, "tabs" ) X( PBDR, "pBdr" ) X( NUMPR, "numPr" ) X( ILVL, "ilvl" ) X( NUMID, "numId" ) \
    X( TOP, "top" ) X( LEFT, "left" ) X( BOTTOM, "bottom" ) X( RIGHT, "right" ) \
    X( START, "start" ) X( END, "end" ) X( INSIDEH, "insideH" ) X( INSIDEV, "insideV" ) \
    X( RSTYLE, "rStyle" ) X( RFONTS, "rFonts" ) X( B, "b" ) X( I, "i" ) X( CAPS, "caps" ) \
    X( SMALLCAPS, "smallCaps" ) X( STRIKE, "strike" ) X( DSTRIKE, "dstrike" ) \
    X( VANISH, "vanish" ) X( OUTLINE, "outline" ) X( SHADOW, "shadow" ) X( U, "u" ) \
    X( VERTALIGN, "vertAlign" ) X( SZ, "sz" ) X( COLOR, "color" ) X( HIGHLIGHT, "highlight" ) \
    X( PGSZ, "pgSz" ) X( PGMAR, "pgMar" ) X( PGNUMTYPE, "pgNumType" ) \
    X( HEADERREF, "headerReference" ) X( FOOTERREF, "footerReference" ) X( TYPE, "type" ) \
    X( COLS, "cols" ) X( TBLSTYLE, "tblStyle" ) X( TBLIND, "tblInd" ) \
    X( TBLBORDERS, "tblBorders" ) X( GRIDCOL, "gridCol" ) X( TBLHEADER, "tblHeader" ) \
    X( GRIDBEFORE, "gridBefore" ) X( TCW, "tcW" ) X( GRIDSPAN, "gridSpan" ) \
    X( VMERGE, "vMerge" ) X( TCBORDERS, "tcBorders" ) \
    X( DOCDEFAULTS, "docDefaults" ) X( RPRDEFAULT, "rPrDefault" ) X( PPRDEFAULT, "pPrDefault" ) \
    X( STYLE, "style" ) X( NAME, "name" ) X( BASEDON, "basedOn" ) \
    X( ABSTRACTNUM, "abstractNum" ) X( LVL, "lvl" ) X( NUMFMT, "numFmt" ) X( NUM, "num" ) \
    X( ABSTRACTNUMID, "abstractNumId" ) X( LVLOVERRIDE, "lvlOverride" ) \
    X( STARTOVERRIDE, "startOverride" ) X( STYLELINK, "styleLink" ) \
    X( NUMSTYLELINK, "numStyleLink" ) X( DEFAULTTAB, "defaultTabStop" ) \
    X( TBLSTYLEPR, "tblStylePr" ) X( TBLLOOK, "tblLook" )

#define X( id, name )   T_##id,
enum { T_NONE, W_TAGS T_COUNT };
#undef X
#define X( id, name )   name,
static const char *const tag_names[T_COUNT] = { "", W_TAGS };
#undef X

/* ------------------------------------------------------------------ */
/* what a style, or a paragraph or run of the file, gives              */
/* ------------------------------------------------------------------ */

#define XC_FONT         0x01
#define XC_SIZE         0x02
#define XC_COLOR        0x04
#define XC_HILITE       0x08
#define XC_UNDER        0x10
#define XC_VERT         0x20

#define CF_TOGGLES      (CF_BOLD | CF_ITALIC | CF_STRIKE | CF_HIDDEN | CF_SMALLCAPS | \
                         CF_ALLCAPS | CF_OUTLINE | CF_SHADOW)
#define CF_UNDERS       (CF_UNDER | CF_DUNDER | CF_WUNDER)
#define CF_VERTS        (CF_SUPER | CF_SUB)

typedef struct {
    u16 set;                    /* XC_*: which of the members below are given */
    u16 tog_set;                /* which of CF_TOGGLES are given... */
    u16 tog;                    /* ...and whether each is on */
    u16 font, size;
    u16 under;                  /* one of CF_UNDERS, or 0 for none */
    u16 vert;                   /* one of CF_VERTS, or 0 for neither */
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
#define XP_NUM          0x0080
#define XP_ILVL         0x0100
#define XP_STYLE        0x0200

#define PF_GIVEN        (PF_KEEP | PF_KEEPNEXT | PF_PAGEBEFORE | PF_BORDER_TOP | PF_BORDER_BOT | \
                         PF_BORDER_LEFT | PF_BORDER_RIGHT)
#define TAB_CLEAR       0xFF    /* a WTAB's kind: the tab there is taken away */

typedef struct {
    u16 set;                    /* XP_* */
    u16 flag_set;               /* which of PF_GIVEN are given... */
    u16 flags;                  /* ...and whether each is on */
    u8  align, line_rule;
    u8  ilvl;
    u8  ntabs;
    u16 before, after, line;
    s32 left, right, first;
    s32 num_id;
    int style;                  /* its paragraph style: which DSTYLE, -1 for none such */
    WTAB *tabs;
} XPAP;

enum { ST_PARA = 1, ST_CHAR, ST_TABLE, ST_OTHER };

#define MAX_DSTYLES     4096
#define BASED_DEPTH     32      /* how long a chain of "based on" is followed */

typedef struct {
    u32  id, name, based;       /* strings (docx_str); "based" 0 for none */
    u8   type;                  /* ST_* */
    u8   is_default;
    u8   state;                 /* 0 as read, 1 being folded, 2 folded with its chain */
    u8   ruled;                 /* a table style: 1 no borders, 2 borders */
    int  wd;                    /* the document's style it became, -1 not yet */
    XCHP chp;
    XCHP first;                 /* a table style: what its first row's text has besides */
    XPAP pap;                   /* its tabs are a heap block of its own */
} DSTYLE;

/* ------------------------------------------------------------------ */
/* numbering                                                           */
/* ------------------------------------------------------------------ */

#define NUM_LEVELS      9
#define MAX_ABS         4096
#define MAX_NUMS        16384

typedef struct {
    u8  fmt;                    /* LIST_* */
    u8  ind_set;                /* 1: "left" is given, 2: "first" is */
    u16 start;
    s32 left, first;
} DLEVEL;

typedef struct {
    s32 id;
    u32 style_link, num_style_link;     /* strings */
    DLEVEL lvl[NUM_LEVELS];
    /* while the text is read: which levels have had a paragraph, which
       start again at their next, and whose paragraph was the last */
    u8  seen[NUM_LEVELS], again[NUM_LEVELS];
    s32 last_num[NUM_LEVELS];
} DABS;

typedef struct {
    s32 id, abs_id;
    u16 over_set;               /* a bit for each level whose start it changes */
    u16 start[NUM_LEVELS];
} DNUM;

/* ------------------------------------------------------------------ */
/* everything else a document being read needs                         */
/* ------------------------------------------------------------------ */

enum { REL_OTHER, REL_DOCUMENT, REL_STYLES, REL_NUMBERING, REL_FOOTNOTES, REL_ENDNOTES,
       REL_HEADER, REL_FOOTER, REL_IMAGE, REL_THEME, REL_SETTINGS, REL_CORE };

#define MAX_RELS        65536UL
#define PART_NAME_LEN   512

typedef struct {
    u32 id, target;             /* strings: "rId4", and the part's name in the package */
    u8  kind;                   /* REL_* */
    u8  external;
} DREL;

typedef struct {
    s32 id;
    int obj;
    u8  kind;                   /* NOTE_* */
    u8  filled;
} DNOTE;

#define MAX_FIELDS      8
#define INSTR_LEN       48

typedef struct {
    u8   simple;                /* a fldSimple: it ends with its element */
    u8   phase;                 /* 0 its instructions, 1 its result */
    u8   emitted;
    u8   instr_len;
    u16  code;                  /* the WCH_* it is, or 0 for a field kept as text */
    int  depth;
    char instr[INSTR_LEN];
} DFIELD;

#define MAX_ALT         16

typedef struct {
    int depth;
    u8  fallback;               /* it has one, so no Choice is read */
    u8  taken;                  /* a Choice was read: no other is */
} DALT;

typedef struct {
    XCHP x;                     /* what a run of it starts as */
    int  style;                 /* its DSTYLE, or -1 */
    s32  num_id;                /* its list, 0 for none */
    u8   ilvl, list;
    u8   joined;                /* its mark was deleted: the next runs on from it */
    u8   spare;
} DPARA;

#define MAX_GRID        64
#define EDGE_MAX        0x00FFFFFFL

enum { STORY_BODY, STORY_NOTES, STORY_HEADER, STORY_FOOTER };

typedef struct {
    WDOC *doc;
    ZIP  *zip;
    int   nomem;
    char *str;                  /* every string kept, one after another */
    u32   str_len, str_cap;
    DREL *rel;                  /* the relationships of the part in hand */
    u32   nrel, rel_cap;
    DSTYLE *style;
    u32   nstyle, style_cap;
    XCHP  def_chp;              /* the document's defaults: everything given */
    XPAP  def_pap;
    int   def_para, def_table;  /* the default styles, -1 for none */
    int   theme_major, theme_minor;     /* the theme's fonts, -1 for none */
    DABS *abs;
    u32   nabs, abs_cap;
    DNUM *num;
    u32   nnum, num_cap;
    DNOTE *note;
    u32   nnote, note_cap, note_hint;
    u8    tag_head[32], tag_next[T_COUNT], tag_len[T_COUNT];

    /* the story being read */
    WBUILD wb;
    int   story;                /* STORY_* */
    int   mute;                 /* nothing is kept: between the notes of a notes part */
    int   in_text, in_instr;
    int   p_depth;              /* paragraphs open, one inside another */
    DPARA para;
    int   split;                /* a text box has just cut the paragraph in two */
    int   txbx_depth;
    DPARA host;                 /* the paragraph a text box is in, kept while the box is read */
    PAP   host_pap;
    CHP   host_chp;
    int   host_saved;
    DFIELD fld[MAX_FIELDS];
    int   nfld, fld_over;
    DALT  alt[MAX_ALT];
    int   nalt;
    /* a drawing */
    int   draw_on, draw_text;
    s32   draw_cx, draw_cy, vml_cx, vml_cy;
    char  draw_embed[64];
    /* the outermost table */
    int   tbl_depth, tbl_style;
    int   tbl_first, tbl_row;   /* its first row is dressed as the style says; the row in hand */
    int   tbl_ruled;            /* -1 not said, 0 no, 1 yes */
    s32   tbl_left;
    s32   grid[MAX_GRID];
    int   ngrid;
    TROW  row;
    int   row_ruled, col;
    s32   edge;
    u32   cell_from;
    int   cell_span;
    s32   cell_width;
    WPARA *cell_last;
    /* sections */
    int   have_sect, sect_mark;
    WPARA *sect_para;
    u32   header_part, footer_part;     /* strings */
} DOCX;

#define STR( ctx, at )  ((ctx)->str + (at))

/* ------------------------------------------------------------------ */
/* small things                                                        */
/* ------------------------------------------------------------------ */

/* a string kept for as long as the reading lasts -> where, 0 for "" */
static u32 docx_str( DOCX *ctx, const char *text )
{
    char *fresh;
    u32 len = str_len( text ), want, at;

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

/* room for one more in an array that doubles, up to a limit */
static int docx_grow( DOCX *ctx, void **array, u32 *cap, u32 need, u32 size, u32 limit )
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

/* value * num / den, for numbers whose product would not fit */
static u32 docx_muldiv( u32 value, u32 num, u32 den )
{
    return (value / den) * num + (value % den) * num / den;
}

/* a whole number, with a sign perhaps: 0 if there are no digits */
static int docx_int( const char *text, s32 *out )
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
 * A length as the format writes one: a whole number of twips, or (the
 * "strict" files, and VML) a number with a unit - 12.5pt, 1in, 2.54cm.
 * "plain" is how many twips a number with no unit counts, times 1000.
 */
static int docx_twips( const char *text, s32 *out, u32 plain )
{
    u32 whole = 0, frac = 0, scale = 100, milli, twips;
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
        if ( whole < 1000000UL ) {
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
    if ( whole > 1000000UL ) {
        whole = 1000000UL;
    }
    milli = whole * 1000 + frac;
    if ( text[0] == 'p' && text[1] == 't' ) {
        twips = docx_muldiv( milli, 1, 50 );
    } else if ( text[0] == 'i' && text[1] == 'n' ) {
        twips = docx_muldiv( milli, 36, 25 );
    } else if ( text[0] == 'c' && text[1] == 'm' ) {
        twips = docx_muldiv( milli, 72, 127 );
    } else if ( text[0] == 'm' && text[1] == 'm' ) {
        twips = docx_muldiv( milli, 36, 635 );
    } else if ( text[0] == 'p' && (text[1] == 'c' || text[1] == 'i') ) {
        twips = docx_muldiv( milli, 6, 25 );
    } else if ( text[0] == 'p' && text[1] == 'x' ) {
        twips = docx_muldiv( milli, 3, 200 );
    } else {
        twips = docx_muldiv( milli, plain, 1000000UL );
    }
    *out = minus ? -(s32)twips : (s32)twips;
    return 1;
}

static s32 docx_clamp( s32 value, s32 low, s32 high )
{
    return value < low ? low : value > high ? high : value;
}

/* hexadecimal, all of the text: 0 if it is not */
static int docx_hex( const char *text, u32 *out )
{
    u32 value = 0;
    int digits = 0, digit;

    for ( ; text != NULL && *text; text++, digits++ ) {
        digit = *text;
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
    return digits > 0 && digits <= 8;
}

#define WVAL( xml )     xml_attr( (xml), NS_W, "val" )

/* an on/off property: there with no value is "on" */
static int docx_on( const XML *xml )
{
    const char *val = WVAL( xml );

    return val == NULL || !(str_cmp( val, "0" ) == 0 || str_icmp( val, "false" ) == 0 ||
                            str_icmp( val, "off" ) == 0);
}

static int docx_tag( const DOCX *ctx, const XML *xml )
{
    u32 len = xml->name_len;
    int tag;

    if ( xml->ns != NS_W || len == 0 || len > 255 ) {
        return T_NONE;
    }
    for ( tag = ctx->tag_head[(len * 5 + (u8)xml->name[0]) & 31]; tag; tag = ctx->tag_next[tag] ) {
        if ( ctx->tag_len[tag] == len && mem_cmp( xml->name, tag_names[tag], len ) == 0 ) {
            return tag;
        }
    }
    return T_NONE;
}

/* what kind of picture some bytes are */
static int docx_sniff( const u8 *data, u32 len )
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

/* ------------------------------------------------------------------ */
/* the package                                                         */
/* ------------------------------------------------------------------ */

/* a part's bytes, the caller's to xfree: NULL if it has no such part */
static u8 *docx_part( DOCX *ctx, const char *name, u32 *len )
{
    u8 *data;
    int index;

    *len = 0;
    if ( name == NULL || name[0] == 0 ) {
        return NULL;
    }
    index = zip_find( ctx->zip, name );
    if ( index < 0 ) {
        index = zip_ifind( ctx->zip, name );    /* part names go by any case */
    }
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

static XML *docx_xml( DOCX *ctx, const u8 *data, u32 len )
{
    XML *xml = xml_open( data, len );
    u32 index;

    if ( xml == NULL ) {
        ctx->nomem = 1;
        return NULL;
    }
    for ( index = 0; index < sizeof( docx_spaces ) / sizeof( docx_spaces[0] ); index++ ) {
        if ( !xml_ns( xml, docx_spaces[index].uri, docx_spaces[index].id ) ) {
            ctx->nomem = 1;
            xml_close( xml );
            return NULL;
        }
    }
    return xml;
}

/*
 * Where a relationship points: "target" as it is written in the
 * relationships of the part "base", made into a name in the package.
 * -> the name (a string), 0 if it is too long to be one.
 */
static u32 docx_target( DOCX *ctx, const char *base, const char *target )
{
    char path[PART_NAME_LEN];
    const char *slash;
    u32 len = 0, piece;

    if ( target[0] == '/' ) {
        target++;
    } else {
        slash = str_rchr( base, '/' );
        if ( slash != NULL ) {
            len = (u32)(slash - base) + 1;
            if ( len + 2 > sizeof( path ) ) {
                return 0;
            }
            mem_cpy( path, base, len );
        }
    }
    while ( *target ) {
        for ( piece = 0; target[piece] && target[piece] != '/'; piece++ ) {
        }
        if ( piece == 2 && target[0] == '.' && target[1] == '.' ) {
            if ( len ) {
                len--;
                while ( len && path[len - 1] != '/' ) {
                    len--;
                }
            }
        } else if ( piece && !(piece == 1 && target[0] == '.') ) {
            if ( piece > sizeof( path ) || len + piece + 2 > sizeof( path ) ) {
                return 0;
            }
            mem_cpy( path + len, target, piece );
            len += piece;
            if ( target[piece] == '/' ) {
                path[len++] = '/';
            }
        }
        target += piece;
        if ( *target == '/' ) {
            target++;
        }
    }
    path[len] = 0;
    return docx_str( ctx, path );
}

static int docx_rel_kind( const char *type )
{
    static const struct {
        const char *tail;
        int kind;
    } kinds[] = {
        { "officeDocument", REL_DOCUMENT }, { "styles", REL_STYLES },
        { "numbering", REL_NUMBERING }, { "footnotes", REL_FOOTNOTES },
        { "endnotes", REL_ENDNOTES }, { "header", REL_HEADER }, { "footer", REL_FOOTER },
        { "image", REL_IMAGE }, { "theme", REL_THEME }, { "settings", REL_SETTINGS },
        { "core-properties", REL_CORE }
    };
    const char *tail = str_rchr( type, '/' );
    u32 index;

    tail = tail != NULL ? tail + 1 : type;
    for ( index = 0; index < sizeof( kinds ) / sizeof( kinds[0] ); index++ ) {
        if ( str_cmp( tail, kinds[index].tail ) == 0 ) {
            return kinds[index].kind;
        }
    }
    return REL_OTHER;
}

/* the relationships of a part ("" for the package's own) made the ones in hand */
static void docx_rels( DOCX *ctx, const char *part )
{
    char base[PART_NAME_LEN], name[PART_NAME_LEN + 16];
    const char *slash, *id, *type, *target, *mode;
    XML *xml;
    DREL *rel;
    u8 *data;
    u32 len, dir;
    int token;

    ctx->nrel = 0;
    if ( !str_cpyn( base, part, sizeof( base ) ) ) {
        return;
    }
    slash = str_rchr( base, '/' );
    dir = slash != NULL ? (u32)(slash - base) + 1 : 0;
    mem_cpy( name, base, dir );
    sfmt( name + dir, sizeof( name ) - dir, "_rels/%s.rels", base + dir );
    data = docx_part( ctx, name, &len );
    if ( data == NULL ) {
        return;
    }
    xml = docx_xml( ctx, data, len );
    for ( ;; ) {
        token = xml_next( xml );
        if ( token == XML_EOF || token == XML_ERROR ) {
            break;
        }
        if ( token != XML_START || !xml_is( xml, NS_PR, "Relationship" ) ) {
            continue;
        }
        id = xml_attr( xml, 0, "Id" );
        type = xml_attr( xml, 0, "Type" );
        target = xml_attr( xml, 0, "Target" );
        mode = xml_attr( xml, 0, "TargetMode" );
        if ( id == NULL || type == NULL || target == NULL ||
             !docx_grow( ctx, (void **)&ctx->rel, &ctx->rel_cap, ctx->nrel + 1, sizeof( DREL ), MAX_RELS ) ) {
            continue;
        }
        rel = &ctx->rel[ctx->nrel];
        rel->kind = (u8)docx_rel_kind( type );
        rel->external = (u8)(mode != NULL && str_icmp( mode, "External" ) == 0);
        rel->id = docx_str( ctx, id );
        rel->target = rel->external ? 0 : docx_target( ctx, base, target );
        ctx->nrel++;
    }
    xml_close( xml );
    xfree( data );
}

static const DREL *docx_rel( const DOCX *ctx, const char *id )
{
    u32 index;

    for ( index = 0; id != NULL && index < ctx->nrel; index++ ) {
        if ( str_cmp( STR( ctx, ctx->rel[index].id ), id ) == 0 ) {
            return &ctx->rel[index];
        }
    }
    return NULL;
}

/* the first part of a kind the part in hand is related to: its name, or 0 */
static u32 docx_rel_first( const DOCX *ctx, int kind )
{
    u32 index;

    for ( index = 0; index < ctx->nrel; index++ ) {
        if ( ctx->rel[index].kind == kind && !ctx->rel[index].external ) {
            return ctx->rel[index].target;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* properties: one set laid over another                               */
/* ------------------------------------------------------------------ */

/* "toggle": src is a character style over a paragraph's - an on/off
   property it has on turns over what is there, as Word has it */
static void xchp_over( XCHP *dst, const XCHP *src, int toggle )
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
    if ( toggle ) {
        dst->tog ^= src->tog & src->tog_set;
    } else {
        dst->tog = (u16)((dst->tog & ~src->tog_set) | (src->tog & src->tog_set));
    }
    dst->tog_set |= src->tog_set;
}

/* everything but the tabs, which add up (docx_tabs_over) */
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
    if ( src->set & XP_NUM ) {
        dst->num_id = src->num_id;
    }
    if ( src->set & XP_ILVL ) {
        dst->ilvl = src->ilvl;
    }
    if ( src->set & XP_STYLE ) {
        dst->style = src->style;
    }
    dst->set |= src->set;
    dst->flags = (u16)((dst->flags & ~src->flag_set) | (src->flags & src->flag_set));
    dst->flag_set |= src->flag_set;
}

/* tabs laid over a list of at most MAX_TABS: one at the place of an
   old one replaces it, and a TAB_CLEAR only takes the old one away */
static void docx_tabs_over( WTAB *list, int *count, const WTAB *over, int nover )
{
    int index, at, kept;

    for ( index = 0; index < nover; index++ ) {
        kept = 0;
        for ( at = 0; at < *count; at++ ) {
            if ( list[at].pos != over[index].pos ) {
                list[kept++] = list[at];
            }
        }
        *count = kept;
        if ( over[index].kind != TAB_CLEAR && *count < MAX_TABS ) {
            list[(*count)++] = over[index];
        }
    }
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
/* styles.xml                                                          */
/* ------------------------------------------------------------------ */

static int docx_style_find( const DOCX *ctx, const char *id, int type )
{
    u32 index;

    for ( index = 0; id != NULL && index < ctx->nstyle; index++ ) {
        if ( ctx->style[index].type == type && str_cmp( STR( ctx, ctx->style[index].id ), id ) == 0 ) {
            return (int)index;
        }
    }
    return -1;
}

static void docx_toggle( const XML *xml, XCHP *chp, u16 flag )
{
    chp->tog_set |= flag;
    if ( docx_on( xml ) ) {
        chp->tog |= flag;
    } else {
        chp->tog &= (u16)~flag;
    }
}

/* the font an rFonts element names for Latin text: the theme's if it
   says so, else the name itself */
static void docx_fonts( DOCX *ctx, const XML *xml, XCHP *chp )
{
    static const char *const names[4] = { "asciiTheme", "ascii", "hAnsiTheme", "hAnsi" };
    const char *val;
    int index, font = -1;

    for ( index = 0; index < 4 && font < 0; index++ ) {
        val = xml_attr( xml, NS_W, names[index] );
        if ( val == NULL || val[0] == 0 ) {
            continue;
        }
        if ( index & 1 ) {
            font = wd_font( ctx->doc, val, FAM_ANY, -1 );
        } else if ( str_ncmp( val, "major", 5 ) == 0 ) {
            /* a file with no theme of its own is given Word's */
            if ( ctx->theme_major < 0 ) {
                ctx->theme_major = wd_font( ctx->doc, "Aptos Display", FAM_SWISS, 0 );
            }
            font = ctx->theme_major;
        } else {
            if ( ctx->theme_minor < 0 ) {
                ctx->theme_minor = wd_font( ctx->doc, "Aptos", FAM_SWISS, 0 );
            }
            font = ctx->theme_minor;
        }
    }
    if ( font >= 0 ) {
        chp->font = (u16)font;
        chp->set |= XC_FONT;
    }
}

/* Word's sixteen highlighters, in the order Word numbers them */
static int docx_hilite( const char *val )
{
    static const char *const names[16] = {
        "black", "blue", "cyan", "green", "magenta", "red", "yellow", "white", "darkBlue",
        "darkCyan", "darkGreen", "darkMagenta", "darkRed", "darkYellow", "darkGray", "lightGray"
    };
    int index;

    for ( index = 0; val != NULL && index < 16; index++ ) {
        if ( str_cmp( val, names[index] ) == 0 ) {
            return index + 1;
        }
    }
    return 0;
}

/*
 * An rPr element: what it gives.  *rstyle is the character style it
 * names, -1 for none.
 */
static void docx_rpr( DOCX *ctx, XML *xml, XCHP *chp, int *rstyle )
{
    const char *val;
    u32 rgb;
    s32 value;
    int depth = xml->depth;

    mem_set( chp, 0, sizeof( XCHP ) );
    *rstyle = -1;
    while ( xml_child( xml, depth ) ) {
        switch ( docx_tag( ctx, xml ) ) {
        case T_RSTYLE:
            *rstyle = docx_style_find( ctx, WVAL( xml ), ST_CHAR );
            break;
        case T_RFONTS:
            docx_fonts( ctx, xml, chp );
            break;
        case T_B:
            docx_toggle( xml, chp, CF_BOLD );
            break;
        case T_I:
            docx_toggle( xml, chp, CF_ITALIC );
            break;
        case T_CAPS:
            docx_toggle( xml, chp, CF_ALLCAPS );
            break;
        case T_SMALLCAPS:
            docx_toggle( xml, chp, CF_SMALLCAPS );
            break;
        case T_STRIKE:
        case T_DSTRIKE:
            docx_toggle( xml, chp, CF_STRIKE );
            break;
        case T_VANISH:
            docx_toggle( xml, chp, CF_HIDDEN );
            break;
        case T_OUTLINE:
            docx_toggle( xml, chp, CF_OUTLINE );
            break;
        case T_SHADOW:
            docx_toggle( xml, chp, CF_SHADOW );
            break;
        case T_U:
            val = WVAL( xml );
            chp->set |= XC_UNDER;
            chp->under = (u16)(val == NULL || str_cmp( val, "none" ) == 0 ? 0 :
                               str_cmp( val, "words" ) == 0 ? CF_WUNDER :
                               str_cmp( val, "double" ) == 0 ? CF_DUNDER : CF_UNDER);
            break;
        case T_VERTALIGN:
            val = WVAL( xml );
            chp->set |= XC_VERT;
            chp->vert = (u16)(val == NULL ? 0 : str_cmp( val, "superscript" ) == 0 ? CF_SUPER :
                              str_cmp( val, "subscript" ) == 0 ? CF_SUB : 0);
            break;
        case T_SZ:
            /* half points; a "strict" file may say 12pt instead */
            if ( docx_twips( WVAL( xml ), &value, 10000UL ) && value > 0 ) {
                chp->size = (u16)docx_clamp( value / 10, 2, 1000 );
                chp->set |= XC_SIZE;
            }
            break;
        case T_COLOR:
            val = WVAL( xml );
            if ( val != NULL && str_cmp( val, "auto" ) == 0 ) {
                chp->color = COLOR_AUTO;
                chp->set |= XC_COLOR;
            } else if ( val != NULL && str_len( val ) == 6 && docx_hex( val, &rgb ) ) {
                chp->color = rgb;
                chp->set |= XC_COLOR;
            }
            break;
        case T_HIGHLIGHT:
            chp->hilite = (u8)docx_hilite( WVAL( xml ) );
            chp->set |= XC_HILITE;
            break;
        }
    }
}

/* a border's side: is there a line? */
static int docx_line( const XML *xml )
{
    const char *val = WVAL( xml );

    return val != NULL && str_cmp( val, "nil" ) != 0 && str_cmp( val, "none" ) != 0;
}

static void docx_sectpr( DOCX *ctx, XML *xml );

/*
 * A pPr element: what it gives.  "tabs" has room for MAX_TABS and is
 * where its tabs are put.  "body" is 1 for a paragraph of the text,
 * which may also end a section or have had its mark deleted.
 */
static void docx_ppr( DOCX *ctx, XML *xml, XPAP *pap, WTAB *tabs, int body )
{
    static const u16 sides[4] = { PF_BORDER_TOP, PF_BORDER_LEFT, PF_BORDER_BOT, PF_BORDER_RIGHT };
    const char *val, *rule;
    WTAB *tab;
    s32 value;
    int depth = xml->depth, inner, tag;

    mem_set( pap, 0, sizeof( XPAP ) );
    pap->tabs = tabs;
    pap->style = -1;
    while ( xml_child( xml, depth ) ) {
        switch ( docx_tag( ctx, xml ) ) {
        case T_PSTYLE:
            pap->style = docx_style_find( ctx, WVAL( xml ), ST_PARA );
            pap->set |= XP_STYLE;
            break;
        case T_JC:
            val = WVAL( xml );
            if ( val != NULL ) {
                pap->align = (u8)(str_cmp( val, "center" ) == 0 ? AL_CENTER :
                                  (str_cmp( val, "right" ) == 0 || str_cmp( val, "end" ) == 0) ? AL_RIGHT :
                                  (str_cmp( val, "both" ) == 0 || str_cmp( val, "distribute" ) == 0) ?
                                  AL_JUSTIFY : AL_LEFT);
                pap->set |= XP_ALIGN;
            }
            break;
        case T_IND:
            val = xml_attr( xml, NS_W, "left" );
            if ( val == NULL ) {
                val = xml_attr( xml, NS_W, "start" );
            }
            if ( docx_twips( val, &value, 1000 ) ) {
                pap->left = docx_clamp( value, -31680, 31680 );
                pap->set |= XP_LEFT;
            }
            val = xml_attr( xml, NS_W, "right" );
            if ( val == NULL ) {
                val = xml_attr( xml, NS_W, "end" );
            }
            if ( docx_twips( val, &value, 1000 ) ) {
                pap->right = docx_clamp( value, -31680, 31680 );
                pap->set |= XP_RIGHT;
            }
            if ( docx_twips( xml_attr( xml, NS_W, "hanging" ), &value, 1000 ) ) {
                pap->first = -docx_clamp( value, 0, 31680 );
                pap->set |= XP_FIRST;
            } else if ( docx_twips( xml_attr( xml, NS_W, "firstLine" ), &value, 1000 ) ) {
                pap->first = docx_clamp( value, 0, 31680 );
                pap->set |= XP_FIRST;
            }
            break;
        case T_SPACING:
            if ( docx_twips( xml_attr( xml, NS_W, "before" ), &value, 1000 ) ) {
                pap->before = (u16)docx_clamp( value, 0, 31680 );
                pap->set |= XP_BEFORE;
            }
            if ( docx_twips( xml_attr( xml, NS_W, "after" ), &value, 1000 ) ) {
                pap->after = (u16)docx_clamp( value, 0, 31680 );
                pap->set |= XP_AFTER;
            }
            if ( docx_twips( xml_attr( xml, NS_W, "line" ), &value, 1000 ) && value > 0 ) {
                rule = xml_attr( xml, NS_W, "lineRule" );
                pap->line = (u16)docx_clamp( value, 1, 31680 );
                pap->line_rule = (u8)(rule == NULL ? LS_MULTIPLE : str_cmp( rule, "exact" ) == 0 ? LS_EXACT :
                                      str_cmp( rule, "atLeast" ) == 0 ? LS_ATLEAST : LS_MULTIPLE);
                pap->set |= XP_LINE;
            }
            break;
        case T_KEEPNEXT:
        case T_KEEPLINES:
        case T_PAGEBEFORE:
            tag = docx_tag( ctx, xml );
            value = tag == T_KEEPNEXT ? PF_KEEPNEXT : tag == T_KEEPLINES ? PF_KEEP : PF_PAGEBEFORE;
            pap->flag_set |= (u16)value;
            if ( docx_on( xml ) ) {
                pap->flags |= (u16)value;
            } else {
                pap->flags &= (u16)~value;
            }
            break;
        case T_TABS:
            inner = xml->depth;
            while ( xml_child( xml, inner ) ) {
                if ( docx_tag( ctx, xml ) != T_TAB || pap->ntabs >= MAX_TABS ||
                     !docx_twips( xml_attr( xml, NS_W, "pos" ), &value, 1000 ) ) {
                    continue;
                }
                tab = &tabs[pap->ntabs++];
                mem_set( tab, 0, sizeof( WTAB ) );
                tab->pos = docx_clamp( value, -31680, 31680 );
                val = WVAL( xml );
                if ( val != NULL ) {
                    tab->kind = (u8)(str_cmp( val, "clear" ) == 0 ? TAB_CLEAR :
                                     str_cmp( val, "center" ) == 0 ? TAB_CENTER :
                                     (str_cmp( val, "right" ) == 0 || str_cmp( val, "end" ) == 0) ? TAB_RIGHT :
                                     str_cmp( val, "decimal" ) == 0 ? TAB_DECIMAL : TAB_LEFT);
                }
                val = xml_attr( xml, NS_W, "leader" );
                if ( val != NULL ) {
                    tab->lead = (u8)((str_cmp( val, "dot" ) == 0 || str_cmp( val, "middleDot" ) == 0) ? LEAD_DOTS :
                                     str_cmp( val, "hyphen" ) == 0 ? LEAD_DASHES :
                                     (str_cmp( val, "underscore" ) == 0 || str_cmp( val, "heavy" ) == 0) ?
                                     LEAD_LINE : LEAD_NONE);
                }
            }
            break;
        case T_PBDR:
            inner = xml->depth;
            while ( xml_child( xml, inner ) ) {
                tag = docx_tag( ctx, xml );
                if ( tag >= T_TOP && tag <= T_RIGHT ) {
                    pap->flag_set |= sides[tag - T_TOP];
                    if ( docx_line( xml ) ) {
                        pap->flags |= sides[tag - T_TOP];
                    }
                }
            }
            break;
        case T_NUMPR:
            inner = xml->depth;
            while ( xml_child( xml, inner ) ) {
                tag = docx_tag( ctx, xml );
                if ( tag == T_NUMID && docx_int( WVAL( xml ), &value ) ) {
                    pap->num_id = value;
                    pap->set |= XP_NUM;
                } else if ( tag == T_ILVL && docx_int( WVAL( xml ), &value ) ) {
                    pap->ilvl = (u8)docx_clamp( value, 0, NUM_LEVELS - 1 );
                    pap->set |= XP_ILVL;
                }
            }
            break;
        case T_RPR:
            /* the paragraph mark's own: all that matters is whether it
               was deleted with changes tracked */
            inner = xml->depth;
            while ( xml_child( xml, inner ) ) {
                tag = docx_tag( ctx, xml );
                if ( body && (tag == T_DEL || tag == T_MOVEFROM) ) {
                    ctx->para.joined = 1;
                    ctx->doc->lost |= LOST_REVISIONS;
                }
            }
            break;
        case T_SECTPR:
            if ( body ) {
                docx_sectpr( ctx, xml );
            }
            break;
        }
    }
}

static void docx_style_free( DOCX *ctx )
{
    u32 index;

    for ( index = 0; index < ctx->nstyle; index++ ) {
        xfree( ctx->style[index].pap.tabs );
    }
    xfree( ctx->style );
    ctx->style = NULL;
    ctx->nstyle = 0;
}

/* a style's tabs, read into a block on the stack, kept in one of its own */
static void docx_style_tabs( DOCX *ctx, DSTYLE *style, const WTAB *tabs, int count )
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

/* one <w:style> */
static void docx_style_read( DOCX *ctx, XML *xml )
{
    WTAB tabs[MAX_TABS];
    XPAP pap;
    DSTYLE *style;
    const char *val;
    int depth = xml->depth, inner, tag, rstyle;

    if ( !docx_grow( ctx, (void **)&ctx->style, &ctx->style_cap, ctx->nstyle + 1, sizeof( DSTYLE ),
                     MAX_DSTYLES ) ) {
        return;
    }
    style = &ctx->style[ctx->nstyle];
    mem_set( style, 0, sizeof( DSTYLE ) );
    style->wd = -1;
    style->pap.style = -1;
    val = xml_attr( xml, NS_W, "type" );
    style->type = (u8)(val == NULL || str_cmp( val, "paragraph" ) == 0 ? ST_PARA :
                       str_cmp( val, "character" ) == 0 ? ST_CHAR :
                       str_cmp( val, "table" ) == 0 ? ST_TABLE : ST_OTHER);
    val = xml_attr( xml, NS_W, "default" );
    style->is_default = (u8)(val != NULL && (str_cmp( val, "1" ) == 0 || str_icmp( val, "true" ) == 0 ||
                                             str_icmp( val, "on" ) == 0));
    val = xml_attr( xml, NS_W, "styleId" );
    style->id = docx_str( ctx, val != NULL ? val : "" );
    ctx->nstyle++;
    while ( xml_child( xml, depth ) ) {
        /* docx_str can move the strings, not the styles; all the same */
        style = &ctx->style[ctx->nstyle - 1];
        switch ( docx_tag( ctx, xml ) ) {
        case T_NAME:
            val = WVAL( xml );
            style->name = docx_str( ctx, val != NULL ? val : "" );
            break;
        case T_BASEDON:
            val = WVAL( xml );
            style->based = docx_str( ctx, val != NULL ? val : "" );
            break;
        case T_PPR:
            docx_ppr( ctx, xml, &pap, tabs, 0 );
            pap.tabs = NULL;
            pap.set &= ~XP_STYLE;
            pap.style = -1;
            style = &ctx->style[ctx->nstyle - 1];
            xfree( style->pap.tabs );
            style->pap = pap;
            style->pap.ntabs = 0;
            docx_style_tabs( ctx, style, tabs, pap.ntabs );
            break;
        case T_RPR:
            docx_rpr( ctx, xml, &style->chp, &rstyle );
            break;
        case T_TBLSTYLEPR:
            /* what the style does to parts of a table: the first row's
               characters are the part that is kept */
            val = xml_attr( xml, NS_W, "type" );
            if ( val == NULL || str_cmp( val, "firstRow" ) != 0 ) {
                break;
            }
            inner = xml->depth;
            while ( xml_child( xml, inner ) ) {
                if ( docx_tag( ctx, xml ) == T_RPR ) {
                    docx_rpr( ctx, xml, &ctx->style[ctx->nstyle - 1].first, &rstyle );
                }
            }
            break;
        case T_TBLPR:
            inner = xml->depth;
            while ( xml_child( xml, inner ) ) {
                if ( docx_tag( ctx, xml ) != T_TBLBORDERS ) {
                    continue;
                }
                style = &ctx->style[ctx->nstyle - 1];
                style->ruled = 1;
                tag = xml->depth;
                while ( xml_child( xml, tag ) ) {
                    if ( xml->ns == NS_W && docx_line( xml ) ) {
                        style->ruled = 2;
                    }
                }
            }
            break;
        }
    }
}

/* a style folded together with the ones it is based on */
static void docx_style_fold( DOCX *ctx, int index, int depth )
{
    WTAB tabs[MAX_TABS];
    DSTYLE *style = &ctx->style[index], *base;
    XCHP chp;
    XPAP pap;
    WTAB *own;
    int from, ntabs = 0;

    if ( style->state ) {
        return;
    }
    style->state = 1;
    from = style->based ? docx_style_find( ctx, STR( ctx, style->based ), style->type ) : -1;
    if ( from >= 0 && from != index && depth < BASED_DEPTH ) {
        docx_style_fold( ctx, from, depth + 1 );
        base = &ctx->style[from];
        /* a style still being folded is one this chain has come back
           round to: the chain is cut there */
        if ( base->state == 2 ) {
            chp = base->chp;
            xchp_over( &chp, &style->chp, 0 );
            style->chp = chp;
            pap = base->pap;
            xpap_over( &pap, &style->pap );
            docx_tabs_over( tabs, &ntabs, base->pap.tabs, base->pap.ntabs );
            docx_tabs_over( tabs, &ntabs, style->pap.tabs, style->pap.ntabs );
            own = style->pap.tabs;
            style->pap = pap;
            style->pap.tabs = own;
            docx_style_tabs( ctx, style, tabs, ntabs );
            if ( style->ruled == 0 ) {
                style->ruled = base->ruled;
            }
            chp = base->first;
            xchp_over( &chp, &style->first, 0 );
            style->first = chp;
        }
    }
    style->state = 2;
}

static void docx_styles( DOCX *ctx, const char *part )
{
    WTAB tabs[MAX_TABS];
    XPAP pap;
    XCHP chp;
    XML *xml;
    u8 *data;
    u32 len, index;
    int depth, inner, deeper, tag, rstyle;

    data = docx_part( ctx, part, &len );
    if ( data == NULL ) {
        return;
    }
    xml = docx_xml( ctx, data, len );
    if ( xml != NULL && xml_next( xml ) == XML_START ) {
        depth = xml->depth;
        while ( xml_child( xml, depth ) ) {
            tag = docx_tag( ctx, xml );
            if ( tag == T_STYLE ) {
                docx_style_read( ctx, xml );
            } else if ( tag == T_DOCDEFAULTS ) {
                inner = xml->depth;
                while ( xml_child( xml, inner ) ) {
                    tag = docx_tag( ctx, xml );
                    deeper = xml->depth;
                    while ( xml_child( xml, deeper ) ) {
                        if ( tag == T_RPRDEFAULT && docx_tag( ctx, xml ) == T_RPR ) {
                            docx_rpr( ctx, xml, &chp, &rstyle );
                            xchp_over( &ctx->def_chp, &chp, 0 );
                        } else if ( tag == T_PPRDEFAULT && docx_tag( ctx, xml ) == T_PPR ) {
                            docx_ppr( ctx, xml, &pap, tabs, 0 );
                            pap.set &= ~(XP_STYLE | XP_NUM | XP_ILVL);
                            xpap_over( &ctx->def_pap, &pap );
                        }
                    }
                }
            }
        }
    }
    xml_close( xml );
    xfree( data );
    for ( index = 0; index < ctx->nstyle; index++ ) {
        docx_style_fold( ctx, (int)index, 0 );
        if ( ctx->style[index].is_default && ctx->style[index].type == ST_PARA ) {
            ctx->def_para = (int)index;
        } else if ( ctx->style[index].is_default && ctx->style[index].type == ST_TABLE ) {
            ctx->def_table = (int)index;
        }
    }
}

/* which of the document's own kinds of style a name is */
static int docx_style_kind( const char *name, const char **as )
{
    static const struct {
        const char *name, *as;
        int kind;
    } known[] = {
        { "normal", "Normal", STY_NORMAL }, { "heading 1", "Heading 1", STY_HEADING1 },
        { "heading 2", "Heading 2", STY_HEADING2 }, { "heading 3", "Heading 3", STY_HEADING3 },
        { "heading 4", "Heading 4", STY_HEADING4 }, { "heading 5", "Heading 5", STY_HEADING5 },
        { "heading 6", "Heading 6", STY_HEADING6 }, { "title", "Title", STY_TITLE },
        { "subtitle", "Subtitle", STY_SUBTITLE }, { "header", "Header", STY_HEADER },
        { "footer", "Footer", STY_FOOTER }, { "footnote text", "Footnote Text", STY_FOOTNOTE }
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

/*
 * The document's style for one of the file's paragraph styles, made at
 * its first use: the name, and the look a paragraph of it starts with.
 */
static int docx_wd_style( DOCX *ctx, int index )
{
    DSTYLE *style;
    WSTYLE *made;
    const char *name;
    XCHP chp;
    XPAP pap;
    int kind;

    if ( index < 0 || (u32)index >= ctx->nstyle ) {
        return 0;
    }
    style = &ctx->style[index];
    if ( style->wd >= 0 ) {
        return style->wd;
    }
    kind = docx_style_kind( STR( ctx, style->name ), &name );
    if ( index == ctx->def_para ) {
        kind = STY_NORMAL;
        name = "Normal";
    } else if ( kind == STY_NORMAL ) {
        kind = STY_OTHER;               /* called Normal, and not the default */
    }
    if ( name[0] == 0 ) {
        name = STR( ctx, style->id );
    }
    style->wd = wd_style( ctx->doc, name[0] ? name : "Style", kind );
    if ( style->wd == 0 && kind != STY_NORMAL ) {
        return 0;                       /* the document has all the styles it can hold */
    }
    made = &ctx->doc->style[style->wd];
    chp = ctx->def_chp;
    xchp_over( &chp, &style->chp, 0 );
    xchp_chp( &chp, &made->chp );
    pap = ctx->def_pap;
    xpap_over( &pap, &style->pap );
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

/* ------------------------------------------------------------------ */
/* numbering.xml, the theme, the settings, the properties              */
/* ------------------------------------------------------------------ */

static int docx_list_kind( const char *val )
{
    if ( val == NULL ) {
        return LIST_NUMBER;
    }
    return str_cmp( val, "bullet" ) == 0 ? LIST_BULLET :
           str_cmp( val, "none" ) == 0 ? LIST_NONE :
           str_cmp( val, "lowerLetter" ) == 0 ? LIST_LOWER :
           str_cmp( val, "upperLetter" ) == 0 ? LIST_UPPER :
           str_cmp( val, "lowerRoman" ) == 0 ? LIST_ROMAN_LOWER :
           str_cmp( val, "upperRoman" ) == 0 ? LIST_ROMAN_UPPER : LIST_NUMBER;
}

static void docx_numbering( DOCX *ctx, const char *part )
{
    WTAB tabs[MAX_TABS];
    XPAP pap;
    XML *xml;
    DABS *abs;
    DNUM *num;
    DLEVEL *level;
    const char *val;
    u8 *data;
    u32 len;
    s32 value, which;
    int depth, inner, deeper, tag;

    data = docx_part( ctx, part, &len );
    if ( data == NULL ) {
        return;
    }
    xml = docx_xml( ctx, data, len );
    if ( xml != NULL && xml_next( xml ) == XML_START ) {
        depth = xml->depth;
        while ( xml_child( xml, depth ) ) {
            tag = docx_tag( ctx, xml );
            inner = xml->depth;
            if ( tag == T_ABSTRACTNUM && docx_int( xml_attr( xml, NS_W, "abstractNumId" ), &value ) &&
                 docx_grow( ctx, (void **)&ctx->abs, &ctx->abs_cap, ctx->nabs + 1, sizeof( DABS ), MAX_ABS ) ) {
                abs = &ctx->abs[ctx->nabs++];
                mem_set( abs, 0, sizeof( DABS ) );
                abs->id = value;
                for ( which = 0; which < NUM_LEVELS; which++ ) {
                    abs->lvl[which].fmt = LIST_NUMBER;
                    abs->lvl[which].start = 1;
                }
                while ( xml_child( xml, inner ) ) {
                    tag = docx_tag( ctx, xml );
                    abs = &ctx->abs[ctx->nabs - 1];
                    if ( tag == T_STYLELINK || tag == T_NUMSTYLELINK ) {
                        val = WVAL( xml );
                        which = (s32)docx_str( ctx, val != NULL ? val : "" );
                        if ( tag == T_STYLELINK ) {
                            abs->style_link = (u32)which;
                        } else {
                            abs->num_style_link = (u32)which;
                        }
                        continue;
                    }
                    if ( tag != T_LVL || !docx_int( xml_attr( xml, NS_W, "ilvl" ), &which ) || which < 0 ||
                         which >= NUM_LEVELS ) {
                        continue;
                    }
                    level = &abs->lvl[which];
                    deeper = xml->depth;
                    while ( xml_child( xml, deeper ) ) {
                        tag = docx_tag( ctx, xml );
                        if ( tag == T_START && docx_int( WVAL( xml ), &value ) ) {
                            level->start = (u16)docx_clamp( value, 0, 65535L );
                        } else if ( tag == T_NUMFMT ) {
                            level->fmt = (u8)docx_list_kind( WVAL( xml ) );
                        } else if ( tag == T_PPR ) {
                            docx_ppr( ctx, xml, &pap, tabs, 0 );
                            if ( pap.set & XP_LEFT ) {
                                level->left = pap.left;
                                level->ind_set |= 1;
                            }
                            if ( pap.set & XP_FIRST ) {
                                level->first = pap.first;
                                level->ind_set |= 2;
                            }
                        }
                    }
                }
            } else if ( tag == T_NUM && docx_int( xml_attr( xml, NS_W, "numId" ), &value ) &&
                        docx_grow( ctx, (void **)&ctx->num, &ctx->num_cap, ctx->nnum + 1, sizeof( DNUM ), MAX_NUMS ) ) {
                num = &ctx->num[ctx->nnum++];
                mem_set( num, 0, sizeof( DNUM ) );
                num->id = value;
                num->abs_id = -1;
                while ( xml_child( xml, inner ) ) {
                    tag = docx_tag( ctx, xml );
                    if ( tag == T_ABSTRACTNUMID && docx_int( WVAL( xml ), &value ) ) {
                        num->abs_id = value;
                    } else if ( tag == T_LVLOVERRIDE && docx_int( xml_attr( xml, NS_W, "ilvl" ), &which ) &&
                                which >= 0 && which < NUM_LEVELS ) {
                        deeper = xml->depth;
                        while ( xml_child( xml, deeper ) ) {
                            if ( docx_tag( ctx, xml ) == T_STARTOVERRIDE && docx_int( WVAL( xml ), &value ) ) {
                                num->start[which] = (u16)docx_clamp( value, 0, 65535L );
                                num->over_set |= (u16)(1u << which);
                            }
                        }
                    }
                }
            }
        }
    }
    xml_close( xml );
    xfree( data );
}

/* the list a paragraph is in: its definition, or NULL */
static DABS *docx_list( DOCX *ctx, s32 num_id, DNUM **num_out )
{
    DNUM *num = NULL;
    DABS *abs = NULL;
    u32 index;

    for ( index = 0; index < ctx->nnum; index++ ) {
        if ( ctx->num[index].id == num_id ) {
            num = &ctx->num[index];
            break;
        }
    }
    if ( num == NULL ) {
        return NULL;
    }
    for ( index = 0; index < ctx->nabs; index++ ) {
        if ( ctx->abs[index].id == num->abs_id ) {
            abs = &ctx->abs[index];
            break;
        }
    }
    /* a definition that only names a list style: the one that style is */
    if ( abs != NULL && abs->num_style_link ) {
        for ( index = 0; index < ctx->nabs; index++ ) {
            if ( ctx->abs[index].style_link &&
                 str_cmp( STR( ctx, ctx->abs[index].style_link ), STR( ctx, abs->num_style_link ) ) == 0 ) {
                abs = &ctx->abs[index];
                break;
            }
        }
    }
    *num_out = num;
    return abs;
}

/* the fonts a theme gives headings ("major") and text ("minor") */
static void docx_theme( DOCX *ctx, const char *part )
{
    XML *xml;
    const char *face;
    u8 *data;
    u32 len;
    int token, which = 0;

    data = docx_part( ctx, part, &len );
    if ( data == NULL ) {
        return;
    }
    xml = docx_xml( ctx, data, len );
    for ( ;; ) {
        token = xml_next( xml );
        if ( token == XML_EOF || token == XML_ERROR ) {
            break;
        }
        if ( token == XML_TEXT || xml->ns != NS_A ) {
            continue;                   /* text has no name: the one there is the last element's */
        }
        if ( xml_is( xml, NS_A, "majorFont" ) ) {
            which = token == XML_START ? 1 : 0;
        } else if ( xml_is( xml, NS_A, "minorFont" ) ) {
            which = token == XML_START ? 2 : 0;
        } else if ( token == XML_START && which && xml_is( xml, NS_A, "latin" ) ) {
            face = xml_attr( xml, 0, "typeface" );
            if ( face != NULL && face[0] ) {
                if ( which == 1 ) {
                    ctx->theme_major = wd_font( ctx->doc, face, FAM_ANY, -1 );
                } else {
                    ctx->theme_minor = wd_font( ctx->doc, face, FAM_ANY, -1 );
                }
            }
        }
    }
    xml_close( xml );
    xfree( data );
}

static void docx_settings( DOCX *ctx, const char *part )
{
    XML *xml;
    u8 *data;
    u32 len;
    s32 value;
    int depth;

    data = docx_part( ctx, part, &len );
    if ( data == NULL ) {
        return;
    }
    xml = docx_xml( ctx, data, len );
    if ( xml != NULL && xml_next( xml ) == XML_START ) {
        depth = xml->depth;
        while ( xml_child( xml, depth ) ) {
            if ( docx_tag( ctx, xml ) == T_DEFAULTTAB && docx_twips( WVAL( xml ), &value, 1000 ) &&
                 value > 0 ) {
                ctx->doc->deftab = docx_clamp( value, 20, 31680 );
            }
        }
    }
    xml_close( xml );
    xfree( data );
}

/* title, author, subject, keywords */
static void docx_core( DOCX *ctx, const char *part )
{
    WDOC *doc = ctx->doc;
    XML *xml;
    u8 *data;
    u32 len;
    int depth;

    data = docx_part( ctx, part, &len );
    if ( data == NULL ) {
        return;
    }
    xml = docx_xml( ctx, data, len );
    if ( xml != NULL && xml_next( xml ) == XML_START ) {
        depth = xml->depth;
        while ( xml_child( xml, depth ) ) {
            if ( xml_is( xml, NS_DC, "title" ) ) {
                xml_gather( xml, doc->title, INFO_LEN );
            } else if ( xml_is( xml, NS_DC, "creator" ) ) {
                xml_gather( xml, doc->author, INFO_LEN );
            } else if ( xml_is( xml, NS_DC, "subject" ) ) {
                xml_gather( xml, doc->subject, INFO_LEN );
            } else if ( xml_is( xml, NS_CP, "keywords" ) ) {
                xml_gather( xml, doc->keywords, INFO_LEN );
            }
        }
    }
    xml_close( xml );
    xfree( data );
}

/* ------------------------------------------------------------------ */
/* the text: what goes into the document                               */
/* ------------------------------------------------------------------ */

/*
 * 1 when what the file has here is not to be kept: it is a field's
 * instructions, or the result of a field that the document has a
 * character of its own for - and that character is put in its place,
 * once, the first time anything of the result comes by.
 */
static int docx_quiet( DOCX *ctx )
{
    int index;

    if ( ctx->mute ) {
        return 1;
    }
    for ( index = 0; index < ctx->nfld; index++ ) {
        if ( ctx->fld[index].phase == 0 ) {
            return 1;
        }
        if ( ctx->fld[index].code ) {
            if ( !ctx->fld[index].emitted ) {
                ctx->fld[index].emitted = 1;
                wb_char( &ctx->wb, ctx->fld[index].code );
            }
            return 1;
        }
    }
    return 0;
}

static void docx_put( DOCX *ctx, u32 code )
{
    if ( !docx_quiet( ctx ) ) {
        wb_char( &ctx->wb, code );
    }
}

/* the text of a <w:t> */
static void docx_text( DOCX *ctx, const u8 *text, u32 len )
{
    u16 run[256];
    const u8 *end = text + len;
    u32 used = 0, code;

    if ( docx_quiet( ctx ) ) {
        return;
    }
    while ( text < end ) {
        code = utf8_get( &text, end );
        if ( code < 0x20 ) {
            if ( code != '\t' ) {
                continue;               /* a line end in the XML is not one in the text */
            }
        } else if ( WCH_IS_FIELD( code ) || code == WCH_OBJ ) {
            code = 0xFFFD;              /* the document's own marks, met as text */
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

/* a run's properties: the paragraph's, a character style's over them,
   the run's own over those */
static void docx_run_setup( DOCX *ctx, const XCHP *direct, int rstyle )
{
    XCHP chp = ctx->para.x;

    if ( rstyle >= 0 ) {
        xchp_over( &chp, &ctx->style[rstyle].chp, 1 );
    }
    if ( direct != NULL ) {
        xchp_over( &chp, direct, 0 );
    }
    xchp_chp( &chp, &ctx->wb.chp );
}

/*
 * A paragraph's properties, worked out into the builder: "direct" is
 * what its own pPr gave, NULL when it has none (or has not been read
 * yet - a paragraph starts with what its style gives).
 */
static void docx_para_setup( DOCX *ctx, const XPAP *direct )
{
    WTAB tabs[MAX_TABS];
    XPAP pap = ctx->def_pap;
    XCHP chp = ctx->def_chp;
    PAP *out = &ctx->wb.pap;
    const DSTYLE *style = NULL;
    const DLEVEL *level;
    DABS *abs;
    DNUM *num;
    s32 num_id;
    int index, ntabs = 0, by_style = 1, ilvl;

    index = ctx->tbl_depth ? ctx->tbl_style : -1;
    if ( index >= 0 ) {
        xpap_over( &pap, &ctx->style[index].pap );
        xchp_over( &chp, &ctx->style[index].chp, 0 );
        if ( ctx->tbl_depth == 1 && ctx->tbl_first && ctx->tbl_row == 0 ) {
            xchp_over( &chp, &ctx->style[index].first, 0 );
        }
    }
    index = direct != NULL && (direct->set & XP_STYLE) ? direct->style : -1;
    if ( index < 0 ) {
        index = ctx->def_para;
    }
    ctx->para.style = index;
    if ( index >= 0 ) {
        style = &ctx->style[index];
        xpap_over( &pap, &style->pap );
        xchp_over( &chp, &style->chp, 0 );
        docx_tabs_over( tabs, &ntabs, style->pap.tabs, style->pap.ntabs );
    }
    /* its list: the paragraph's own word, or its style's.  The level's
       indents come between the style's and the paragraph's own - but a
       style that brings the list and also sets an indent means it. */
    num_id = pap.num_id;
    ilvl = pap.ilvl;
    if ( direct != NULL && (direct->set & XP_NUM) ) {
        num_id = direct->num_id;
        by_style = 0;
    }
    if ( direct != NULL && (direct->set & XP_ILVL) ) {
        ilvl = direct->ilvl;
    }
    ctx->para.num_id = 0;
    ctx->para.list = LIST_NONE;
    ctx->para.ilvl = (u8)ilvl;
    if ( num_id > 0 ) {
        abs = docx_list( ctx, num_id, &num );
        if ( abs != NULL ) {
            level = &abs->lvl[ilvl];
            if ( (level->ind_set & 1) && !(by_style && style != NULL && (style->pap.set & XP_LEFT)) ) {
                pap.left = level->left;
            }
            if ( (level->ind_set & 2) && !(by_style && style != NULL && (style->pap.set & XP_FIRST)) ) {
                pap.first = level->first;
            }
            ctx->para.num_id = num_id;
            ctx->para.list = level->fmt;
        }
    }
    if ( direct != NULL ) {
        xpap_over( &pap, direct );
        docx_tabs_over( tabs, &ntabs, direct->tabs, direct->ntabs );
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
    out->list = ctx->para.list;
    out->level = (u8)(ctx->para.list != LIST_NONE ? ilvl : 0);
    out->list_start = 0;
    if ( !pap_set_tabs( out, tabs, ntabs ) ) {
        ctx->nomem = 1;
    }
    ctx->para.x = chp;
    docx_run_setup( ctx, NULL, -1 );
}

/* where a list's numbers stand at the paragraph in hand: the number
   it starts again with, or 0 when it carries on */
static u16 docx_list_start( DOCX *ctx )
{
    DABS *abs;
    DNUM *num;
    int level = ctx->para.ilvl, deeper;
    u16 start = 0;

    if ( ctx->para.num_id <= 0 || ctx->para.list == LIST_NONE ) {
        return 0;
    }
    abs = docx_list( ctx, ctx->para.num_id, &num );
    if ( abs == NULL ) {
        return 0;
    }
    if ( !abs->seen[level] || abs->again[level] ) {
        start = abs->lvl[level].start;
        if ( !abs->seen[level] && ((num->over_set >> level) & 1) ) {
            start = num->start[level];
        }
    } else if ( ((num->over_set >> level) & 1) && abs->last_num[level] != num->id ) {
        start = num->start[level];
    }
    abs->seen[level] = 1;
    abs->again[level] = 0;
    abs->last_num[level] = num->id;
    for ( deeper = level + 1; deeper < NUM_LEVELS; deeper++ ) {
        abs->again[deeper] = 1;
    }
    return (u16)(ctx->para.list == LIST_BULLET ? 0 : start);
}

static WPARA *docx_last_para( DOCX *ctx )
{
    STORY *story = ctx->wb.story;

    return story->count ? story->para[story->count - 1] : NULL;
}

/* the paragraph in hand ends here, with the builder's properties */
static void docx_par( DOCX *ctx )
{
    PAP *pap = &ctx->wb.pap;

    pap->flags &= (u16)~(PF_INTABLE | PF_CELLEND | PF_ROWEND);
    if ( ctx->tbl_depth ) {
        pap->flags |= PF_INTABLE;
    }
    pap->style = (u8)docx_wd_style( ctx, ctx->para.style );
    pap->list_start = docx_list_start( ctx );
    wb_par( &ctx->wb );
    pap->list_start = 0;
    if ( ctx->sect_mark ) {
        ctx->sect_mark = 0;
        ctx->sect_para = docx_last_para( ctx );
    }
    PEAK();
}

/* a picture, by the relationship that names its part */
static void docx_image( DOCX *ctx, const char *rid, s32 width, s32 height )
{
    const DREL *rel = docx_rel( ctx, rid );
    u8 *data;
    u32 len;
    int kind, obj;

    if ( docx_quiet( ctx ) ) {
        return;
    }
    if ( rel == NULL || rel->external || rel->target == 0 ) {
        ctx->doc->lost |= LOST_IMAGES;  /* a picture in another file */
        return;
    }
    data = docx_part( ctx, STR( ctx, rel->target ), &len );
    if ( data == NULL || len == 0 ) {
        xfree( data );
        ctx->doc->lost |= LOST_IMAGES;
        return;
    }
    kind = docx_sniff( data, len );
    if ( kind == IMG_UNKNOWN ) {
        ctx->doc->lost |= LOST_IMAGES;
    }
    obj = wd_image_new( ctx->doc, data, len, kind, docx_clamp( width, 0, 0x7FFFFFL ),
                        docx_clamp( height, 0, 0x7FFFFFL ) );
    xfree( data );
    if ( obj >= 0 ) {
        wb_object( &ctx->wb, obj );
    }
    PEAK();
}

/* a width or height out of a VML style: "width:468pt;height:1.5in" */
static s32 docx_vml_size( const char *style, const char *name )
{
    u32 len = str_len( name );
    s32 value = 0;

    while ( style != NULL && *style ) {
        while ( *style == ' ' || *style == ';' ) {
            style++;
        }
        if ( str_nicmp( style, name, len ) == 0 && style[len] == ':' ) {
            docx_twips( style + len + 1, &value, 20000UL );
            return value > 0 ? value : 0;
        }
        while ( *style && *style != ';' ) {
            style++;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* sections                                                            */
/* ------------------------------------------------------------------ */

/* a part named by r:id in a header or footer reference -> its name */
static u32 docx_ref_part( DOCX *ctx, const XML *xml )
{
    const char *type = xml_attr( xml, NS_W, "type" );
    const DREL *rel;

    /* the one for ordinary pages: a first page's or an even page's own
       is not kept */
    if ( type != NULL && str_cmp( type, "default" ) != 0 ) {
        return 0;
    }
    rel = docx_rel( ctx, xml_attr( xml, NS_R, "id" ) );
    return rel != NULL && !rel->external ? rel->target : 0;
}

static void docx_sectpr( DOCX *ctx, XML *xml )
{
    PAGESETUP page;
    WDOC *doc = ctx->doc;
    const char *val;
    u32 header = 0, footer = 0;
    s32 value;
    u16 mark = WCH_PAGE;
    int depth = xml->depth, has_size = 0, runs_on = 0;

    page = doc->page;
    page.landscape = 0;
    page.first_page = 1;
    while ( xml_child( xml, depth ) ) {
        switch ( docx_tag( ctx, xml ) ) {
        case T_PGSZ:
            if ( docx_twips( xml_attr( xml, NS_W, "w" ), &value, 1000 ) && value >= 720 && value <= 31680 * 2 ) {
                page.width = value;
                has_size |= 1;
            }
            if ( docx_twips( xml_attr( xml, NS_W, "h" ), &value, 1000 ) && value >= 720 && value <= 31680 * 2 ) {
                page.height = value;
                has_size |= 2;
            }
            val = xml_attr( xml, NS_W, "orient" );
            page.landscape = (u8)(val != NULL && str_cmp( val, "landscape" ) == 0);
            break;
        case T_PGMAR:
            if ( docx_twips( xml_attr( xml, NS_W, "top" ), &value, 1000 ) ) {
                page.top = docx_clamp( value < 0 ? -value : value, 0, 31680 );
            }
            if ( docx_twips( xml_attr( xml, NS_W, "bottom" ), &value, 1000 ) ) {
                page.bottom = docx_clamp( value < 0 ? -value : value, 0, 31680 );
            }
            if ( docx_twips( xml_attr( xml, NS_W, "left" ), &value, 1000 ) ) {
                page.left = docx_clamp( value, 0, 31680 );
            }
            if ( docx_twips( xml_attr( xml, NS_W, "right" ), &value, 1000 ) ) {
                page.right = docx_clamp( value, 0, 31680 );
            }
            if ( docx_twips( xml_attr( xml, NS_W, "header" ), &value, 1000 ) ) {
                page.header = docx_clamp( value, 0, 31680 );
            }
            if ( docx_twips( xml_attr( xml, NS_W, "footer" ), &value, 1000 ) ) {
                page.footer = docx_clamp( value, 0, 31680 );
            }
            break;
        case T_PGNUMTYPE:
            if ( docx_int( xml_attr( xml, NS_W, "start" ), &value ) ) {
                page.first_page = (u16)docx_clamp( value, 0, 65535L );
            }
            break;
        case T_HEADERREF:
            if ( header == 0 ) {
                header = docx_ref_part( ctx, xml );
            }
            break;
        case T_FOOTERREF:
            if ( footer == 0 ) {
                footer = docx_ref_part( ctx, xml );
            }
            break;
        case T_TYPE:
            val = WVAL( xml );
            runs_on = val != NULL && str_cmp( val, "continuous" ) == 0;
            break;
        case T_COLS:
            if ( docx_int( xml_attr( xml, NS_W, "num" ), &value ) && value > 1 ) {
                doc->lost |= LOST_LAYOUT;
            }
            break;
        }
    }
    if ( ctx->story != STORY_BODY ) {
        return;
    }
    if ( !ctx->have_sect ) {
        /* the first section's page is the document's */
        ctx->have_sect = 1;
        if ( has_size == 3 && page.left + page.right < page.width && page.top + page.bottom < page.height ) {
            doc->page = page;
        }
        ctx->header_part = header;
        ctx->footer_part = footer;
    } else if ( !runs_on && ctx->sect_para != NULL ) {
        /* this section started a page: the one before it ends with the break */
        wd_text_insert( doc, ctx->sect_para, ctx->sect_para->len, &mark, 1,
                        (u16)(ctx->sect_para->len ? ctx->sect_para->attr[ctx->sect_para->len - 1] : 0) );
    }
    ctx->sect_para = NULL;
    ctx->sect_mark = 1;                 /* the paragraph this is in ends a section */
}

/* ------------------------------------------------------------------ */
/* fields                                                              */
/* ------------------------------------------------------------------ */

/* the field character for a field's instructions, or 0 */
static u16 docx_field_code( DOCX *ctx, const char *instr )
{
    static const struct {
        const char *name;
        u16 code;
    } known[] = {
        { "PAGE", WCH_PAGENUM }, { "NUMPAGES", WCH_NUMPAGES }, { "DATE", WCH_DATE },
        { "TIME", WCH_TIME }, { "FILENAME", WCH_FILENAME }
    };
    u32 len, index;

    while ( *instr == ' ' ) {
        instr++;
    }
    for ( len = 0; instr[len] && instr[len] != ' ' && instr[len] != '\\'; len++ ) {
    }
    for ( index = 0; index < sizeof( known ) / sizeof( known[0] ); index++ ) {
        if ( str_len( known[index].name ) == len && str_nicmp( instr, known[index].name, len ) == 0 ) {
            return known[index].code;
        }
    }
    /* a link's text is what a link is; anything else is a field lost */
    if ( !(len == 9 && str_nicmp( instr, "HYPERLINK", 9 ) == 0) && len != 0 ) {
        ctx->doc->lost |= LOST_FIELDS;
    }
    return 0;
}

static void docx_field_begin( DOCX *ctx, int simple, int depth, const char *instr )
{
    DFIELD *field;

    if ( ctx->nfld >= MAX_FIELDS ) {
        if ( !simple ) {
            ctx->fld_over++;
        }
        return;
    }
    field = &ctx->fld[ctx->nfld++];
    mem_set( field, 0, sizeof( DFIELD ) );
    field->simple = (u8)simple;
    field->depth = depth;
    if ( simple ) {
        field->phase = 1;
        field->code = docx_field_code( ctx, instr != NULL ? instr : "" );
    }
}

/* the instructions are over and the result starts */
static void docx_field_result( DOCX *ctx )
{
    DFIELD *field;

    if ( ctx->fld_over || ctx->nfld == 0 ) {
        return;
    }
    field = &ctx->fld[ctx->nfld - 1];
    if ( field->phase == 0 ) {
        field->instr[field->instr_len] = 0;
        field->code = docx_field_code( ctx, field->instr );
        field->phase = 1;
    }
}

static void docx_field_end( DOCX *ctx )
{
    if ( ctx->nfld == 0 ) {
        return;
    }
    /* one with no result at all still shows: its character goes in now,
       if nothing around it says otherwise */
    docx_field_result( ctx );
    if ( ctx->fld[ctx->nfld - 1].code ) {
        docx_quiet( ctx );
    }
    ctx->nfld--;
}

/* ------------------------------------------------------------------ */
/* tables                                                              */
/* ------------------------------------------------------------------ */

static void docx_tblpr( DOCX *ctx, XML *xml )
{
    const char *val;
    u32 look;
    s32 value;
    int depth = xml->depth, inner, tag;

    while ( xml_child( xml, depth ) ) {
        tag = docx_tag( ctx, xml );
        if ( tag == T_TBLSTYLE ) {
            ctx->tbl_style = docx_style_find( ctx, WVAL( xml ), ST_TABLE );
        } else if ( tag == T_TBLIND ) {
            val = xml_attr( xml, NS_W, "type" );
            if ( (val == NULL || str_cmp( val, "dxa" ) == 0) &&
                 docx_twips( xml_attr( xml, NS_W, "w" ), &value, 1000 ) ) {
                ctx->tbl_left = docx_clamp( value, -31680, 31680 );
            }
        } else if ( tag == T_TBLLOOK ) {
            val = xml_attr( xml, NS_W, "firstRow" );
            if ( val != NULL ) {
                ctx->tbl_first = str_cmp( val, "0" ) != 0 && str_icmp( val, "false" ) != 0;
            } else if ( docx_hex( WVAL( xml ), &look ) ) {
                ctx->tbl_first = (look & 0x0020) != 0;
            }
        } else if ( tag == T_TBLBORDERS ) {
            ctx->tbl_ruled = 0;
            inner = xml->depth;
            while ( xml_child( xml, inner ) ) {
                if ( xml->ns == NS_W && docx_line( xml ) ) {
                    ctx->tbl_ruled = 1;
                }
            }
        }
    }
}

static void docx_tcpr( DOCX *ctx, XML *xml )
{
    const char *val;
    s32 value, text;
    int depth = xml->depth, inner, tag;

    while ( xml_child( xml, depth ) ) {
        tag = docx_tag( ctx, xml );
        if ( tag == T_TCW ) {
            val = xml_attr( xml, NS_W, "type" );
            if ( !docx_twips( xml_attr( xml, NS_W, "w" ), &value, 1000 ) || value <= 0 ) {
                continue;
            }
            if ( val == NULL || str_cmp( val, "dxa" ) == 0 ) {
                ctx->cell_width = value;
            } else if ( str_cmp( val, "pct" ) == 0 ) {
                /* fiftieths of a percent of the width between the margins */
                text = ctx->doc->page.width - ctx->doc->page.left - ctx->doc->page.right;
                ctx->cell_width = (s32)docx_muldiv( (u32)docx_clamp( text, 1440, 31680 * 2 ),
                                                    (u32)docx_clamp( value, 1, 5000 ), 5000 );
            }
        } else if ( tag == T_GRIDSPAN ) {
            if ( docx_int( WVAL( xml ), &value ) ) {
                ctx->cell_span = (int)docx_clamp( value, 1, MAX_GRID );
            }
        } else if ( tag == T_VMERGE ) {
            ctx->doc->lost |= LOST_TABLES;      /* cells joined downward are single cells here */
        } else if ( tag == T_TCBORDERS ) {
            inner = xml->depth;
            while ( xml_child( xml, inner ) ) {
                if ( xml->ns == NS_W && docx_line( xml ) ) {
                    ctx->row_ruled = 1;
                }
            }
        }
    }
}

/* a cell of the outermost table ends: its last paragraph is marked,
   and its right edge goes into the row */
static void docx_cell_end( DOCX *ctx )
{
    WPARA *para;
    s32 width = 0;
    int index;

    if ( ctx->wb.story->count == ctx->cell_from || ctx->wb.para != NULL ) {
        /* a cell with nothing in it has an empty paragraph all the same */
        if ( ctx->wb.para == NULL ) {
            docx_para_setup( ctx, NULL );
        }
        docx_par( ctx );
    }
    para = docx_last_para( ctx );
    if ( para == NULL ) {
        return;
    }
    if ( ctx->cell_width > 0 ) {
        width = ctx->cell_width;
    } else if ( ctx->col + ctx->cell_span <= ctx->ngrid ) {
        for ( index = 0; index < ctx->cell_span; index++ ) {
            width += ctx->grid[ctx->col + index];
        }
    }
    if ( width <= 0 ) {
        width = 1440;
    }
    ctx->col += ctx->cell_span;
    if ( ctx->edge < EDGE_MAX ) {
        ctx->edge += docx_clamp( width, 1, 31680 * 2 );
    }
    para->pap.flags |= PF_INTABLE | PF_CELLEND;
    if ( ctx->row.ncells < MAX_CELLS ) {
        ctx->row.edge[ctx->row.ncells++] = ctx->edge;
    } else {
        /* more cells than a row holds: the rest are part of the last */
        ctx->row.edge[MAX_CELLS - 1] = ctx->edge;
        if ( ctx->cell_last != NULL && ctx->cell_last != para ) {
            ctx->cell_last->pap.flags &= (u16)~PF_CELLEND;
        }
        ctx->doc->lost |= LOST_TABLES;
    }
    ctx->cell_last = para;
}

static void docx_row_end( DOCX *ctx )
{
    int ruled = ctx->tbl_ruled;

    if ( ctx->row.ncells == 0 || ctx->cell_last == NULL ) {
        return;
    }
    if ( ruled < 0 ) {
        ruled = ctx->tbl_style >= 0 && ctx->style[ctx->tbl_style].ruled == 2;
    }
    ctx->row.borders = (u8)(ruled || ctx->row_ruled);
    ctx->cell_last->pap.flags |= PF_ROWEND;
    if ( !pap_set_row( &ctx->cell_last->pap, &ctx->row ) ) {
        ctx->nomem = 1;
    }
    ctx->cell_last = NULL;
}

/* ------------------------------------------------------------------ */
/* the pass over a part's XML                                          */
/* ------------------------------------------------------------------ */

/* an AlternateContent: has it a Fallback?  Looked for by reading
   ahead, and then the reading goes back to where it was. */
static void docx_alt_begin( DOCX *ctx, XML *xml )
{
    XMARK mark;
    DALT *alt;
    int depth = xml->depth;

    if ( ctx->nalt >= MAX_ALT ) {
        return;                         /* its Choices will be passed over */
    }
    alt = &ctx->alt[ctx->nalt++];
    alt->depth = depth;
    alt->fallback = 0;
    alt->taken = 0;
    xml_mark( xml, &mark );
    while ( xml_child( xml, depth ) ) {
        if ( xml_is( xml, NS_MC, "Fallback" ) ) {
            alt->fallback = 1;
        }
    }
    xml_rewind( xml, &mark );
}

/* the paragraph a text box is in is put by; the box's paragraphs follow */
static void docx_txbx_begin( DOCX *ctx )
{
    ctx->doc->lost |= LOST_LAYOUT;
    ctx->draw_text = 1;
    if ( ctx->txbx_depth++ == 0 && !ctx->host_saved ) {
        if ( pap_copy( &ctx->host_pap, &ctx->wb.pap ) ) {
            ctx->host = ctx->para;
            ctx->host_chp = ctx->wb.chp;
            ctx->host_saved = 1;
        } else {
            ctx->nomem = 1;
        }
    }
    if ( ctx->wb.para != NULL ) {
        docx_par( ctx );
    }
}

static void docx_txbx_end( DOCX *ctx )
{
    if ( ctx->wb.para != NULL ) {
        docx_par( ctx );
    }
    if ( ctx->txbx_depth && --ctx->txbx_depth == 0 && ctx->host_saved ) {
        pap_free( &ctx->wb.pap );
        ctx->wb.pap = ctx->host_pap;
        ctx->wb.chp = ctx->host_chp;
        ctx->para = ctx->host;
        ctx->host_saved = 0;
        ctx->split = 1;
    }
}

/* a drawing's picture, if one is waiting, goes into the text */
static void docx_draw_flush( DOCX *ctx )
{
    if ( ctx->draw_embed[0] ) {
        docx_image( ctx, ctx->draw_embed, ctx->draw_cx, ctx->draw_cy );
        ctx->draw_embed[0] = 0;
    }
}

static void docx_note_ref( DOCX *ctx, const XML *xml, int kind )
{
    DNOTE *note;
    s32 id;
    int obj;

    if ( ctx->story != STORY_BODY || docx_quiet( ctx ) || !docx_int( xml_attr( xml, NS_W, "id" ), &id ) ) {
        return;                         /* a note has no notes of its own */
    }
    obj = wd_note_new( ctx->doc, kind );
    if ( obj < 0 ) {
        return;
    }
    wb_object( &ctx->wb, obj );
    if ( docx_grow( ctx, (void **)&ctx->note, &ctx->note_cap, ctx->nnote + 1, sizeof( DNOTE ), 0x00FFFFFFUL ) ) {
        note = &ctx->note[ctx->nnote++];
        note->id = id;
        note->obj = obj;
        note->kind = (u8)kind;
        note->filled = 0;
    }
}

/* in a notes part: a note starts.  1 if it is one the text refers to. */
static int docx_note_begin( DOCX *ctx, const XML *xml, int kind )
{
    const char *type = xml_attr( xml, NS_W, "type" );
    STORY *story;
    u32 index, count;
    s32 id;

    if ( ctx->story != STORY_NOTES || (type != NULL && str_cmp( type, "normal" ) != 0) ||
         !docx_int( xml_attr( xml, NS_W, "id" ), &id ) ) {
        return 0;                       /* the rule above the notes, and such */
    }
    /* from where the last one was found: a file has them in order */
    for ( count = 0; count < ctx->nnote; count++ ) {
        index = (ctx->note_hint + count) % ctx->nnote;
        if ( ctx->note[index].id == id && ctx->note[index].kind == kind && !ctx->note[index].filled ) {
            ctx->note_hint = index + 1;
            story = wd_note_story( ctx->doc, ctx->note[index].obj );
            if ( story == NULL ) {
                return 0;
            }
            ctx->note[index].filled = 1;
            wb_story( &ctx->wb, story );
            ctx->mute = 0;
            return 1;
        }
    }
    return 0;
}

static void docx_start( DOCX *ctx, XML *xml )
{
    WTAB tabs[MAX_TABS];
    XPAP pap;
    XCHP chp;
    CHP keep;
    const char *val;
    DALT *alt;
    u32 code;
    s32 value, high;
    int tag, rstyle, depth;

    if ( xml->ns != NS_W ) {
        if ( xml->ns == NS_MC ) {
            if ( xml_is( xml, NS_MC, "AlternateContent" ) ) {
                docx_alt_begin( ctx, xml );
            } else if ( xml_is( xml, NS_MC, "Choice" ) ) {
                alt = ctx->nalt ? &ctx->alt[ctx->nalt - 1] : NULL;
                if ( alt == NULL || alt->depth != xml->depth - 1 || alt->fallback || alt->taken ) {
                    xml_skip( xml );
                } else {
                    alt->taken = 1;
                }
            }
        } else if ( xml->ns == NS_WP ) {
            if ( xml_is( xml, NS_WP, "extent" ) && ctx->draw_on && ctx->draw_cx == 0 &&
                 docx_int( xml_attr( xml, 0, "cx" ), &value ) && docx_int( xml_attr( xml, 0, "cy" ), &high ) ) {
                ctx->draw_cx = value / EMU_PER_TWIP;
                ctx->draw_cy = high / EMU_PER_TWIP;
            }
        } else if ( xml->ns == NS_A ) {
            if ( xml_is( xml, NS_A, "blip" ) && ctx->draw_on ) {
                docx_draw_flush( ctx );         /* a second picture in one drawing */
                val = xml_attr( xml, NS_R, "embed" );
                if ( val == NULL || !str_cpyn( ctx->draw_embed, val, sizeof( ctx->draw_embed ) ) ) {
                    ctx->draw_embed[0] = 0;
                    ctx->doc->lost |= LOST_IMAGES;      /* linked, not kept in the file */
                }
            }
        } else if ( xml->ns == NS_V ) {
            val = xml_attr( xml, 0, "style" );
            if ( val != NULL ) {
                value = docx_vml_size( val, "width" );
                high = docx_vml_size( val, "height" );
                if ( value > 0 && high > 0 ) {
                    ctx->vml_cx = value;
                    ctx->vml_cy = high;
                }
            }
            if ( xml_is( xml, NS_V, "imagedata" ) ) {
                val = xml_attr( xml, NS_R, "id" );
                if ( val == NULL ) {
                    val = xml_attr( xml, NS_O, "relid" );
                }
                if ( val != NULL ) {
                    docx_image( ctx, val, ctx->vml_cx, ctx->vml_cy );
                }
            }
        } else if ( xml->ns == NS_M ) {
            if ( xml_is( xml, NS_M, "t" ) ) {
                ctx->in_text = 1;
            } else if ( xml_is( xml, NS_M, "oMath" ) ) {
                ctx->doc->lost |= LOST_OBJECTS; /* an equation: its letters are kept */
                docx_run_setup( ctx, NULL, -1 );
            }
        }
        return;
    }
    tag = docx_tag( ctx, xml );
    switch ( tag ) {
    case T_P:
        /* a paragraph inside a paragraph: what the outer one has so
           far stands alone */
        if ( ctx->p_depth > 0 && ctx->wb.para != NULL ) {
            docx_par( ctx );
        }
        ctx->p_depth++;
        ctx->split = 0;
        ctx->para.joined = 0;
        docx_para_setup( ctx, NULL );
        break;
    case T_PPR:
        docx_ppr( ctx, xml, &pap, tabs, 1 );
        docx_para_setup( ctx, &pap );
        break;
    case T_R:
        docx_run_setup( ctx, NULL, -1 );
        break;
    case T_RPR:
        docx_rpr( ctx, xml, &chp, &rstyle );
        docx_run_setup( ctx, &chp, rstyle );
        break;
    case T_T:
        ctx->in_text = 1;
        break;
    case T_TAB:
    case T_PTAB:
        docx_put( ctx, WCH_TAB );
        break;
    case T_BR:
        val = xml_attr( xml, NS_W, "type" );
        docx_put( ctx, val != NULL && (str_cmp( val, "page" ) == 0 || str_cmp( val, "column" ) == 0) ?
                  WCH_PAGE : WCH_LINE );
        break;
    case T_CR:
        docx_put( ctx, WCH_LINE );
        break;
    case T_NBHYPHEN:
        docx_put( ctx, WCH_NBHYPHEN );
        break;
    case T_SOFTHYPHEN:
        docx_put( ctx, WCH_SHY );
        break;
    case T_PGNUM:
        docx_put( ctx, WCH_PAGENUM );
        break;
    case T_SYM:
        /* a character of a symbol font: in that font, as Word keeps it */
        if ( docx_hex( xml_attr( xml, NS_W, "char" ), &code ) && code >= 0x20 && code <= 0xFFFF &&
             !WCH_IS_FIELD( code ) && code != WCH_OBJ ) {
            if ( code < 0x100 ) {
                code |= 0xF000;         /* where Word itself keeps a symbol font's */
            }
            keep = ctx->wb.chp;
            val = xml_attr( xml, NS_W, "font" );
            if ( val != NULL && val[0] ) {
                ctx->wb.chp.font = (u16)wd_font( ctx->doc, val, FAM_SYMBOL, -1 );
            }
            docx_put( ctx, code );
            ctx->wb.chp = keep;
        }
        break;
    case T_FOOTREF:
        docx_note_ref( ctx, xml, NOTE_FOOT );
        break;
    case T_ENDREF:
        docx_note_ref( ctx, xml, NOTE_END );
        break;
    case T_FLDSIMPLE:
        docx_field_begin( ctx, 1, xml->depth, xml_attr( xml, NS_W, "instr" ) );
        break;
    case T_FLDCHAR:
        val = xml_attr( xml, NS_W, "fldCharType" );
        if ( val == NULL ) {
            break;
        }
        if ( str_cmp( val, "begin" ) == 0 ) {
            docx_field_begin( ctx, 0, xml->depth, NULL );
        } else if ( str_cmp( val, "separate" ) == 0 ) {
            docx_field_result( ctx );
        } else if ( str_cmp( val, "end" ) == 0 ) {
            if ( ctx->fld_over ) {
                ctx->fld_over--;
            } else if ( ctx->nfld && !ctx->fld[ctx->nfld - 1].simple ) {
                docx_field_end( ctx );
            }
        }
        break;
    case T_INSTRTEXT:
        ctx->in_instr = 1;
        break;
    case T_DEL:
    case T_MOVEFROM:
        ctx->doc->lost |= LOST_REVISIONS;
        xml_skip( xml );
        break;
    case T_INS:
    case T_MOVETO:
        ctx->doc->lost |= LOST_REVISIONS;
        break;
    case T_DELTEXT:
    case T_DELINSTR:
    case T_SDTPR:
    case T_SDTENDPR:
    case T_RUBYPR:
    case T_RT:
    case T_CUSTOMXMLPR:
    case T_SMARTTAGPR:
    case T_FLDDATA:
    case T_BINDATA:
    case T_COMMENT:
    case T_PPRCHANGE:
    case T_RPRCHANGE:
    case T_TBLPREX:
        xml_skip( xml );
        break;
    case T_ALTCHUNK:
        ctx->doc->lost |= LOST_OBJECTS;
        break;
    case T_TBL:
        if ( ctx->tbl_depth++ == 0 ) {
            ctx->tbl_style = ctx->def_table;
            ctx->tbl_first = 0;
            ctx->tbl_row = -1;
            ctx->tbl_ruled = -1;
            ctx->tbl_left = 0;
            ctx->ngrid = 0;
            ctx->cell_last = NULL;
        } else {
            ctx->doc->lost |= LOST_TABLES;      /* a table in a cell: its text stays in the cell */
        }
        break;
    case T_TBLPR:
        if ( ctx->tbl_depth == 1 ) {
            docx_tblpr( ctx, xml );
        } else {
            xml_skip( xml );
        }
        break;
    case T_TBLGRID:
        depth = xml->depth;
        while ( xml_child( xml, depth ) ) {
            if ( ctx->tbl_depth == 1 && docx_tag( ctx, xml ) == T_GRIDCOL && ctx->ngrid < MAX_GRID ) {
                value = 0;
                docx_twips( xml_attr( xml, NS_W, "w" ), &value, 1000 );
                ctx->grid[ctx->ngrid++] = docx_clamp( value, 0, 31680 * 2 );
            }
        }
        break;
    case T_TR:
        if ( ctx->tbl_depth == 1 ) {
            mem_set( &ctx->row, 0, sizeof( TROW ) );
            ctx->tbl_row++;
            ctx->row.left = ctx->tbl_left;
            ctx->edge = ctx->tbl_left;
            ctx->col = 0;
            ctx->row_ruled = 0;
            ctx->cell_last = NULL;
        }
        break;
    case T_TRPR:
        depth = xml->depth;
        while ( xml_child( xml, depth ) ) {
            if ( ctx->tbl_depth != 1 ) {
                continue;
            }
            tag = docx_tag( ctx, xml );
            if ( tag == T_TBLHEADER ) {
                ctx->row.header = (u8)docx_on( xml );
            } else if ( tag == T_GRIDBEFORE && docx_int( WVAL( xml ), &value ) ) {
                /* the row starts some columns in */
                for ( ; value > 0 && ctx->col < ctx->ngrid; value--, ctx->col++ ) {
                    ctx->edge += ctx->grid[ctx->col];
                }
                ctx->row.left = ctx->edge;
            }
        }
        break;
    case T_TC:
        if ( ctx->tbl_depth == 1 ) {
            ctx->cell_from = ctx->wb.story->count;
            ctx->cell_span = 1;
            ctx->cell_width = 0;
        }
        break;
    case T_TCPR:
        if ( ctx->tbl_depth == 1 ) {
            docx_tcpr( ctx, xml );
        } else {
            xml_skip( xml );
        }
        break;
    case T_SECTPR:
        docx_sectpr( ctx, xml );
        break;
    case T_TXBX:
        docx_txbx_begin( ctx );
        break;
    case T_DRAWING:
        ctx->draw_on = 1;
        ctx->draw_text = 0;
        ctx->draw_cx = ctx->draw_cy = 0;
        ctx->draw_embed[0] = 0;
        break;
    case T_OBJECT:
        ctx->doc->lost |= LOST_OBJECTS;         /* an embedded object: its picture is kept */
        ctx->vml_cx = ctx->vml_cy = 0;
        break;
    case T_PICT:
        ctx->vml_cx = ctx->vml_cy = 0;
        break;
    case T_FOOTNOTE:
    case T_ENDNOTE:
        if ( !docx_note_begin( ctx, xml, tag == T_FOOTNOTE ? NOTE_FOOT : NOTE_END ) ) {
            xml_skip( xml );
        }
        break;
    }
}

static void docx_end( DOCX *ctx, XML *xml )
{
    if ( xml->ns == NS_M ) {
        if ( xml_is( xml, NS_M, "t" ) ) {
            ctx->in_text = 0;
        }
        return;
    }
    if ( xml->ns == NS_MC ) {
        if ( ctx->nalt && ctx->alt[ctx->nalt - 1].depth == xml->depth &&
             xml_is( xml, NS_MC, "AlternateContent" ) ) {
            ctx->nalt--;
        }
        return;
    }
    switch ( docx_tag( ctx, xml ) ) {
    case T_P:
        if ( ctx->p_depth ) {
            ctx->p_depth--;
        }
        if ( ctx->para.joined ) {
            ctx->para.joined = 0;       /* the next paragraph carries on in this one */
        } else if ( !(ctx->split && ctx->wb.para == NULL) ) {
            docx_par( ctx );
        }
        ctx->split = 0;
        break;
    case T_T:
        ctx->in_text = 0;
        break;
    case T_INSTRTEXT:
        ctx->in_instr = 0;
        break;
    case T_FLDSIMPLE:
        if ( ctx->nfld && ctx->fld[ctx->nfld - 1].simple && ctx->fld[ctx->nfld - 1].depth == xml->depth ) {
            docx_field_end( ctx );
        }
        break;
    case T_TC:
        if ( ctx->tbl_depth == 1 ) {
            docx_cell_end( ctx );
        }
        break;
    case T_TR:
        if ( ctx->tbl_depth == 1 ) {
            docx_row_end( ctx );
        }
        break;
    case T_TBL:
        if ( ctx->tbl_depth ) {
            ctx->tbl_depth--;
        }
        break;
    case T_TXBX:
        docx_txbx_end( ctx );
        break;
    case T_DRAWING:
        if ( ctx->draw_embed[0] ) {
            docx_draw_flush( ctx );
        } else if ( !ctx->draw_text ) {
            ctx->doc->lost |= LOST_OBJECTS;     /* a chart, a shape, a diagram */
        }
        ctx->draw_on = 0;
        break;
    case T_FOOTNOTE:
    case T_ENDNOTE:
        if ( ctx->story == STORY_NOTES ) {
            wb_story( &ctx->wb, NULL );
            ctx->mute = 1;
        }
        break;
    }
}

/*
 * One part read into one story (or, for a notes part, into the notes
 * the text referred to).  1 if the part was there and was XML to its
 * end; 0 otherwise - and what was read before it went wrong is kept.
 */
static int docx_story( DOCX *ctx, const char *part, STORY *story, int kind )
{
    char name[PART_NAME_LEN];
    XML *xml;
    DFIELD *field;
    u8 *data;
    u32 len;
    int token, ok;

    if ( !str_cpyn( name, part, sizeof( name ) ) ) {
        return 0;
    }
    data = docx_part( ctx, name, &len );
    if ( data == NULL ) {
        return 0;
    }
    docx_rels( ctx, name );
    xml = docx_xml( ctx, data, len );
    if ( xml == NULL ) {
        xfree( data );
        return 0;
    }
    ctx->story = kind;
    ctx->mute = kind == STORY_NOTES;
    ctx->in_text = ctx->in_instr = 0;
    ctx->p_depth = ctx->txbx_depth = ctx->split = 0;
    ctx->nfld = ctx->fld_over = ctx->nalt = 0;
    ctx->draw_on = 0;
    ctx->draw_embed[0] = 0;
    ctx->tbl_depth = 0;
    ctx->cell_last = NULL;
    mem_set( &ctx->para, 0, sizeof( DPARA ) );
    wb_init( &ctx->wb, ctx->doc, story );
    docx_para_setup( ctx, NULL );
    for ( ;; ) {
        token = xml_next( xml );
        if ( token == XML_EOF || token == XML_ERROR ) {
            break;
        }
        if ( token == XML_START ) {
            docx_start( ctx, xml );
        } else if ( token == XML_END ) {
            docx_end( ctx, xml );
        } else if ( ctx->in_text ) {
            docx_text( ctx, (const u8 *)xml->text, xml->text_len );
        } else if ( ctx->in_instr && ctx->nfld && !ctx->fld_over ) {
            field = &ctx->fld[ctx->nfld - 1];
            while ( field->phase == 0 && xml->text_len && field->instr_len < INSTR_LEN - 1 ) {
                field->instr[field->instr_len++] = *xml->text++;
                xml->text_len--;
            }
        }
    }
    ok = token == XML_EOF;
    if ( ctx->wb.para != NULL ) {
        docx_par( ctx );                /* a file that stopped in a paragraph */
    }
    if ( kind == STORY_BODY && story->count == 0 ) {
        /* no paragraph at all: Word shows one, and it is the default
           style's, not a bare one */
        docx_para_setup( ctx, NULL );
        docx_par( ctx );
    }
    wb_done( &ctx->wb );
    if ( ctx->host_saved ) {
        pap_free( &ctx->host_pap );
        ctx->host_saved = 0;
    }
    xml_close( xml );
    xfree( data );
    return ok;
}

/* ------------------------------------------------------------------ */
/* detecting and loading                                               */
/* ------------------------------------------------------------------ */

static DOCX *docx_new( WDOC *doc )
{
    DOCX *ctx = (DOCX *)try_alloc( sizeof( DOCX ) );
    u32 len, key;
    int tag;

    if ( ctx == NULL ) {
        return NULL;
    }
    mem_set( ctx, 0, sizeof( DOCX ) );
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
    ctx->def_para = ctx->def_table = -1;
    ctx->theme_major = ctx->theme_minor = -1;
    ctx->tbl_style = -1;
    for ( tag = 1; tag < T_COUNT; tag++ ) {
        len = str_len( tag_names[tag] );
        key = (len * 5 + (u8)tag_names[tag][0]) & 31;
        ctx->tag_len[tag] = (u8)len;
        ctx->tag_next[tag] = ctx->tag_head[key];
        ctx->tag_head[key] = (u8)tag;
    }
    /* what a file that says nothing at all gets: Word's own defaults */
    ctx->def_chp.set = XC_FONT | XC_SIZE | XC_COLOR | XC_HILITE | XC_UNDER | XC_VERT;
    ctx->def_chp.tog_set = CF_TOGGLES;
    ctx->def_chp.size = 20;
    ctx->def_chp.color = COLOR_AUTO;
    ctx->def_pap.set = XP_ALIGN | XP_LEFT | XP_RIGHT | XP_FIRST | XP_BEFORE | XP_AFTER | XP_LINE;
    ctx->def_pap.flag_set = PF_GIVEN;
    ctx->def_pap.line = 240;
    ctx->def_pap.line_rule = LS_MULTIPLE;
    ctx->def_pap.style = -1;
    return ctx;
}

static void docx_free( DOCX *ctx )
{
    if ( ctx == NULL ) {
        return;
    }
    docx_style_free( ctx );
    zip_close( ctx->zip );
    xfree( ctx->str );
    xfree( ctx->rel );
    xfree( ctx->abs );
    xfree( ctx->num );
    xfree( ctx->note );
    xfree( ctx );
}

/*
 * The part that is the document, found through the package's
 * relationships: its name (a string), and with "check" whether the
 * content types say it is a word processor's.  0 if there is none.
 */
static u32 docx_main_part( DOCX *ctx, int check )
{
    XML *xml;
    const char *name, *type;
    u8 *data;
    u32 len, main_part;
    int token, ours = 0;

    docx_rels( ctx, "" );
    main_part = docx_rel_first( ctx, REL_DOCUMENT );
    if ( main_part == 0 || zip_ifind( ctx->zip, STR( ctx, main_part ) ) < 0 ) {
        return 0;
    }
    if ( !check ) {
        return main_part;
    }
    data = docx_part( ctx, "[Content_Types].xml", &len );
    if ( data == NULL ) {
        return 0;
    }
    xml = docx_xml( ctx, data, len );
    for ( ;; ) {
        token = xml_next( xml );
        if ( token == XML_EOF || token == XML_ERROR ) {
            break;
        }
        if ( token != XML_START || !xml_is( xml, NS_CT, "Override" ) ) {
            continue;
        }
        name = xml_attr( xml, 0, "PartName" );
        type = xml_attr( xml, 0, "ContentType" );
        if ( name != NULL && type != NULL && name[0] == '/' &&
             str_icmp( name + 1, STR( ctx, main_part ) ) == 0 &&
             (mem_find( (const u8 *)type, str_len( type ), "wordprocessingml" ) >= 0 ||
              mem_find( (const u8 *)type, str_len( type ), "ms-word" ) >= 0) ) {
            ours = 1;
        }
    }
    xml_close( xml );
    xfree( data );
    return ours ? main_part : 0;
}

static int docx_detect( const u8 *data, u32 len, const char *ext )
{
    DOCX *ctx;
    int sure = 0;

    (void)ext;
    /* a ZIP, from its first byte: a document with a password is an
       OLE2 file whatever it is called, and is not this format's */
    if ( len < 100 || data[0] != 'P' || data[1] != 'K' || data[2] != 3 || data[3] != 4 ) {
        return 0;
    }
    ctx = docx_new( NULL );
    if ( ctx == NULL ) {
        return 0;
    }
    ctx->zip = zip_open( data, len );
    if ( ctx->zip != NULL && zip_ifind( ctx->zip, "[Content_Types].xml" ) >= 0 && docx_main_part( ctx, 1 ) ) {
        sure = 100;
    }
    docx_free( ctx );
    return sure;
}

static int docx_load( const u8 *data, u32 len, WDOC *doc )
{
    DOCX *ctx = docx_new( doc );
    u32 main_part, styles, numbering, theme, settings, footnotes, endnotes, core;
    u32 index;
    int why, rc = WE_OK, foot = 0, end = 0;

    if ( ctx == NULL ) {
        return WE_NOMEM;
    }
    ctx->zip = zip_open_why( data, len, &why );
    if ( ctx->zip == NULL ) {
        docx_free( ctx );
        return why == ZIP_NOMEM ? WE_NOMEM : WE_FORMAT;
    }
    /* the page a file that says nothing about it has in Word: Letter,
       an inch all round */
    doc->page.left = doc->page.right = 1440;
    main_part = docx_main_part( ctx, 0 );
    core = docx_rel_first( ctx, REL_CORE );
    if ( main_part == 0 ) {
        rc = ctx->nomem ? WE_NOMEM : WE_FORMAT;
        docx_free( ctx );
        return rc;
    }
    docx_rels( ctx, STR( ctx, main_part ) );
    styles = docx_rel_first( ctx, REL_STYLES );
    numbering = docx_rel_first( ctx, REL_NUMBERING );
    theme = docx_rel_first( ctx, REL_THEME );
    settings = docx_rel_first( ctx, REL_SETTINGS );
    footnotes = docx_rel_first( ctx, REL_FOOTNOTES );
    endnotes = docx_rel_first( ctx, REL_ENDNOTES );

    docx_theme( ctx, STR( ctx, theme ) );
    docx_styles( ctx, STR( ctx, styles ) );
    docx_numbering( ctx, STR( ctx, numbering ) );
    docx_settings( ctx, STR( ctx, settings ) );
    docx_wd_style( ctx, ctx->def_para );
    if ( ctx->def_para < 0 ) {
        /* no styles at all: Normal is the defaults */
        xchp_chp( &ctx->def_chp, &doc->style[0].chp );
    }

    if ( !docx_story( ctx, STR( ctx, main_part ), &doc->body, STORY_BODY ) && doc->body.count == 0 ) {
        rc = WE_FORMAT;                 /* nothing of it could be read */
    }
    for ( index = 0; index < ctx->nnote; index++ ) {
        if ( ctx->note[index].kind == NOTE_FOOT ) {
            foot = 1;
        } else {
            end = 1;
        }
    }
    if ( rc == WE_OK && foot && footnotes ) {
        docx_story( ctx, STR( ctx, footnotes ), &doc->body, STORY_NOTES );
    }
    if ( rc == WE_OK && end && endnotes ) {
        docx_story( ctx, STR( ctx, endnotes ), &doc->body, STORY_NOTES );
    }
    if ( rc == WE_OK && ctx->header_part ) {
        docx_story( ctx, STR( ctx, ctx->header_part ), &doc->header, STORY_HEADER );
    }
    if ( rc == WE_OK && ctx->footer_part ) {
        docx_story( ctx, STR( ctx, ctx->footer_part ), &doc->footer, STORY_FOOTER );
    }
    if ( rc == WE_OK ) {
        docx_core( ctx, STR( ctx, core ) );
    }
    if ( ctx->nomem || wd_failed( doc ) ) {
        rc = WE_NOMEM;
    }
    docx_free( ctx );
    return rc;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

#define NS_MAIN     "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
#define NS_RELS     "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
#define NS_DRAW     "http://schemas.openxmlformats.org/drawingml/2006/main"
#define NS_PICT     "http://schemas.openxmlformats.org/drawingml/2006/picture"
#define NS_WPDRAW   "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing"
#define XML_HEAD    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
#define DRAIN_AT    32768UL     /* a part goes to the archive this much at a time */
#define MAX_GRIDW   63          /* the columns Word lets a table have */
#define LEN_MAX     31680L      /* the longest length Word takes: 22 inches */

/* a list made for the file: what each of its levels is, 0xFF for a
   level no paragraph has used */
typedef struct {
    u8  kind[NUM_LEVELS];
    u16 start[NUM_LEVELS];
} WLIST;

typedef struct {
    WDOC *doc;
    ZIPW *zipw;
    OUT  *part;                 /* the part being written, a piece of it */
    int   failed;
    WLIST *list;
    u32   nlist, list_cap;
    u32   cur[NUM_LEVELS];      /* the list each level is in: its number, 0 for none */
    u32  *note_id;              /* by object: the number its note was given, 0 not written */
    u8   *in_part;              /* by object: a picture used in the part in hand */
    u8   *in_file;              /* ...and in any part */
    u32   next_note[2];
    u32   draw_id;
    int   body;                 /* the part in hand is the document itself */
    int   note_mark;            /* 1, 2: the next paragraph starts a footnote, an endnote */
    int   has_lists, has_foot, has_end;
} DOCXW;

#define PUT( text )     out_str( w->part, (text) )

/* what is waiting in memory goes to the archive, when there is enough of it */
static void docxw_drain( DOCXW *w, int all )
{
    u8 *data;
    u32 len = out_tell( w->part ), size;

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

static void docxw_begin( DOCXW *w, const char *name )
{
    if ( !zipw_begin( w->zipw, name ) ) {
        w->failed = 1;
    }
    PUT( XML_HEAD );
}

static void docxw_finish( DOCXW *w )
{
    docxw_drain( w, 1 );
    if ( !zipw_finish( w->zipw ) ) {
        w->failed = 1;
    }
}

/* the name a style goes by inside the file: letters and digits only,
   and no two alike */
static void docxw_style_id( const WDOC *doc, u32 index, char *id )
{
    static const char *const fixed[STY_OTHER] = {
        "Normal", "Heading1", "Heading2", "Heading3", "Heading4", "Heading5", "Heading6",
        "Title", "Subtitle", "Header", "Footer", "FootnoteText"
    };
    const WSTYLE *style;
    const char *name;
    u32 len = 0;

    if ( index >= doc->nstyle ) {
        index = 0;
    }
    style = &doc->style[index];
    if ( index == 0 || style->kind < STY_OTHER ) {
        str_cpy( id, fixed[index == 0 ? 0 : style->kind] );
        return;
    }
    for ( name = style->name; *name && len < 20; name++ ) {
        if ( (*name >= 'A' && *name <= 'Z') || (*name >= 'a' && *name <= 'z') || (*name >= '0' && *name <= '9') ) {
            id[len++] = *name;
        }
    }
    sfmt( id + len, 8, "S%u", index );
}

static const char *docxw_ext( int kind )
{
    switch ( kind ) {
    case IMG_PNG:   return "png";
    case IMG_JPEG:  return "jpeg";
    case IMG_GIF:   return "gif";
    case IMG_BMP:
    case IMG_DIB:   return "bmp";
    case IMG_TIFF:  return "tiff";
    case IMG_WMF:   return "wmf";
    case IMG_EMF:   return "emf";
    }
    return NULL;                        /* not a kind Word shows: left out */
}

static void docxw_attr_esc( DOCXW *w, const char *name, const char *value )
{
    out_fmt( w->part, " %s=\"", name );
    xml_esc( w->part, value, 1 );
    PUT( "\"" );
}

/* an on/off property written only where it is not what it would be */
static void docxw_toggle( DOCXW *w, const char *tag, int on, int was )
{
    if ( on != was ) {
        out_fmt( w->part, on ? "<w:%s/>" : "<w:%s w:val=\"0\"/>", tag );
    }
}

static u16 docxw_under( u16 flags )
{
    return (u16)(flags & CF_DUNDER ? CF_DUNDER : flags & CF_WUNDER ? CF_WUNDER : flags & CF_UNDER);
}

static u16 docxw_vert( u16 flags )
{
    return (u16)(flags & CF_SUPER ? CF_SUPER : flags & CF_SUB);
}

/*
 * Character properties, as how "chp" differs from "base".  The order
 * is the schema's, which Word holds a file to.
 */
static void docxw_rpr( DOCXW *w, const CHP *chp, const CHP *base )
{
    static const struct {
        u16 flag;
        const char *tag;
    } toggles[8] = {
        { CF_BOLD, "b" }, { CF_ITALIC, "i" }, { CF_ALLCAPS, "caps" }, { CF_SMALLCAPS, "smallCaps" },
        { CF_STRIKE, "strike" }, { CF_OUTLINE, "outline" }, { CF_SHADOW, "shadow" }, { CF_HIDDEN, "vanish" }
    };
    static const char *const hilites[17] = {
        "none", "black", "blue", "cyan", "green", "magenta", "red", "yellow", "white", "darkBlue",
        "darkCyan", "darkGreen", "darkMagenta", "darkRed", "darkYellow", "darkGray", "lightGray"
    };
    const WDOC *doc = w->doc;
    u32 start = out_tell( w->part );
    u16 under = docxw_under( chp->flags ), vert = docxw_vert( chp->flags );
    const char *name;
    int index;

    PUT( "<w:rPr>" );
    if ( chp->font != base->font && chp->font < doc->nfont ) {
        name = doc->font[chp->font].name;
        PUT( "<w:rFonts" );
        docxw_attr_esc( w, "w:ascii", name );
        docxw_attr_esc( w, "w:hAnsi", name );
        docxw_attr_esc( w, "w:cs", name );
        PUT( "/>" );
    }
    for ( index = 0; index < 8; index++ ) {
        docxw_toggle( w, toggles[index].tag, (chp->flags & toggles[index].flag) != 0,
                      (base->flags & toggles[index].flag) != 0 );
    }
    if ( chp->color != base->color ) {
        if ( chp->color == COLOR_AUTO ) {
            PUT( "<w:color w:val=\"auto\"/>" );
        } else {
            out_fmt( w->part, "<w:color w:val=\"%06X\"/>", chp->color & 0xFFFFFFUL );
        }
    }
    if ( chp->size != base->size ) {
        out_fmt( w->part, "<w:sz w:val=\"%u\"/><w:szCs w:val=\"%u\"/>", (u32)chp->size, (u32)chp->size );
    }
    if ( chp->hilite != base->hilite ) {
        out_fmt( w->part, "<w:highlight w:val=\"%s\"/>", hilites[chp->hilite <= 16 ? chp->hilite : 0] );
    }
    if ( under != docxw_under( base->flags ) ) {
        out_fmt( w->part, "<w:u w:val=\"%s\"/>", under == CF_DUNDER ? "double" : under == CF_WUNDER ? "words" :
                 under ? "single" : "none" );
    }
    if ( vert != docxw_vert( base->flags ) ) {
        out_fmt( w->part, "<w:vertAlign w:val=\"%s\"/>", vert == CF_SUPER ? "superscript" :
                 vert ? "subscript" : "baseline" );
    }
    if ( out_tell( w->part ) == start + 7 ) {
        out_seek( w->part, start );     /* nothing differs: no element at all */
    } else {
        PUT( "</w:rPr>" );
    }
}

/* the list a paragraph is in -> its number in the file.  A paragraph
   that starts the numbers again, or is of another kind than the list
   its level was in, begins a new one. */
static u32 docxw_list( DOCXW *w, const PAP *pap )
{
    WLIST *list;
    u32 want;
    int level = pap->level < NUM_LEVELS ? pap->level : NUM_LEVELS - 1;

    if ( pap->list_start == 0 && w->cur[level] != 0 && w->list[w->cur[level] - 1].kind[level] == pap->list ) {
        return w->cur[level];
    }
    if ( w->nlist == w->list_cap ) {
        want = w->list_cap ? w->list_cap * 2 : 16;
        list = w->nlist >= 0x00FFFFFFUL ? NULL : (WLIST *)try_realloc( w->list, want * sizeof( WLIST ) );
        if ( list == NULL ) {
            w->failed = 1;
            return 1;
        }
        w->list = list;
        w->list_cap = want;
    }
    list = &w->list[w->nlist++];
    mem_set( list->kind, 0xFF, NUM_LEVELS );
    mem_set( list->start, 0, sizeof( list->start ) );
    list->kind[level] = pap->list;
    list->start[level] = (u16)(pap->list_start ? pap->list_start : 1);
    w->cur[level] = w->nlist;
    return w->nlist;
}

static void docxw_border( DOCXW *w, const char *side, int on )
{
    out_fmt( w->part, on ? "<w:%s w:val=\"single\" w:sz=\"4\" w:space=\"1\" w:color=\"auto\"/>" :
             "<w:%s w:val=\"nil\"/>", side );
}

#define PF_SIDES        (PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT)

/*
 * Paragraph properties, as how "pap" differs from "base" (its style's,
 * or for a style the one it is based on).  "style_id" is the pStyle to
 * name, NULL for none; "outline" a heading's level + 1, 0 for none.
 */
static void docxw_ppr( DOCXW *w, const PAP *pap, const PAP *base, const char *style_id, int outline )
{
    static const char *const aligns[4] = { "left", "center", "right", "both" };
    static const char *const kinds[4] = { "left", "center", "right", "decimal" };
    static const char *const leads[4] = { "none", "dot", "hyphen", "underscore" };
    u32 start = out_tell( w->part ), number;
    u16 sides = (u16)(pap->flags & PF_SIDES);
    int index, listed = pap->list != LIST_NONE && pap->list <= LIST_ROMAN_UPPER;

    PUT( "<w:pPr>" );
    if ( style_id != NULL ) {
        out_fmt( w->part, "<w:pStyle w:val=\"%s\"/>", style_id );
    }
    docxw_toggle( w, "keepNext", (pap->flags & PF_KEEPNEXT) != 0, (base->flags & PF_KEEPNEXT) != 0 );
    docxw_toggle( w, "keepLines", (pap->flags & PF_KEEP) != 0, (base->flags & PF_KEEP) != 0 );
    docxw_toggle( w, "pageBreakBefore", (pap->flags & PF_PAGEBEFORE) != 0, (base->flags & PF_PAGEBEFORE) != 0 );
    if ( listed ) {
        number = docxw_list( w, pap );
        out_fmt( w->part, "<w:numPr><w:ilvl w:val=\"%u\"/><w:numId w:val=\"%u\"/></w:numPr>",
                 (u32)(pap->level < NUM_LEVELS ? pap->level : NUM_LEVELS - 1), number );
    }
    if ( sides != (u16)(base->flags & PF_SIDES) ) {
        PUT( "<w:pBdr>" );
        docxw_border( w, "top", sides & PF_BORDER_TOP );
        docxw_border( w, "left", sides & PF_BORDER_LEFT );
        docxw_border( w, "bottom", sides & PF_BORDER_BOT );
        docxw_border( w, "right", sides & PF_BORDER_RIGHT );
        PUT( "</w:pBdr>" );
    }
    if ( pap->ntabs && pap->tabs != NULL ) {
        PUT( "<w:tabs>" );
        for ( index = 0; index < pap->ntabs; index++ ) {
            out_fmt( w->part, "<w:tab w:val=\"%s\" w:leader=\"%s\" w:pos=\"%d\"/>", kinds[pap->tabs[index].kind & 3],
                     leads[pap->tabs[index].lead & 3], docx_clamp( pap->tabs[index].pos, -LEN_MAX, LEN_MAX ) );
        }
        PUT( "</w:tabs>" );
    }
    if ( pap->before != base->before || pap->after != base->after || pap->line != base->line ||
         pap->line_rule != base->line_rule ) {
        out_fmt( w->part, "<w:spacing w:before=\"%d\" w:after=\"%d\" w:line=\"%d\" w:lineRule=\"%s\"/>",
                 docx_clamp( pap->before, 0, LEN_MAX ), docx_clamp( pap->after, 0, LEN_MAX ),
                 docx_clamp( pap->line ? pap->line : 240, 1, LEN_MAX ),
                 pap->line_rule == LS_EXACT ? "exact" : pap->line_rule == LS_ATLEAST ? "atLeast" : "auto" );
    }
    /* a list's level has indents of its own: a paragraph in one says
       its own always, so that they are what it has */
    if ( listed || pap->left != base->left || pap->right != base->right || pap->first != base->first ) {
        out_fmt( w->part, "<w:ind w:left=\"%d\" w:right=\"%d\" w:%s=\"%d\"/>", docx_clamp( pap->left, -LEN_MAX, LEN_MAX ),
                 docx_clamp( pap->right, -LEN_MAX, LEN_MAX ), pap->first < 0 ? "hanging" : "firstLine",
                 docx_clamp( pap->first < 0 ? -pap->first : pap->first, 0, LEN_MAX ) );
    }
    if ( pap->align != base->align ) {
        out_fmt( w->part, "<w:jc w:val=\"%s\"/>", aligns[pap->align & 3] );
    }
    if ( outline ) {
        out_fmt( w->part, "<w:outlineLvl w:val=\"%u\"/>", (u32)outline - 1 );
    }
    if ( out_tell( w->part ) == start + 7 ) {
        out_seek( w->part, start );
    } else {
        PUT( "</w:pPr>" );
    }
}

/* a picture, set in the line */
static void docxw_image( DOCXW *w, u32 obj )
{
    const WOBJ *image = &w->doc->obj[obj];
    const char *ext = docxw_ext( image->fmt );
    u32 wide, high;

    w->in_part[obj] = w->in_file[obj] = 1;
    w->draw_id++;
    wide = (u32)docx_clamp( image->width, 1, 3168000L ) * EMU_PER_TWIP;
    high = (u32)docx_clamp( image->height, 1, 3168000L ) * EMU_PER_TWIP;
    out_fmt( w->part, "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
             "<wp:extent cx=\"%u\" cy=\"%u\"/><wp:docPr id=\"%u\" name=\"Picture %u\"/>", wide, high, w->draw_id,
             w->draw_id );
    PUT( "<wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
         "<a:graphic><a:graphicData uri=\"" NS_PICT "\"><pic:pic><pic:nvPicPr>" );
    out_fmt( w->part, "<pic:cNvPr id=\"%u\" name=\"image%u.%s\"/><pic:cNvPicPr/></pic:nvPicPr>"
             "<pic:blipFill><a:blip r:embed=\"rImg%u\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>",
             w->draw_id, obj + 1, ext, obj + 1 );
    out_fmt( w->part, "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%u\" cy=\"%u\"/></a:xfrm>"
             "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData>"
             "</a:graphic></wp:inline></w:drawing>", wide, high );
}

/* what one run holds: text, and the characters that are elements */
static void docxw_run_text( DOCXW *w, const u16 *text, u32 len )
{
    u32 pos = 0, from;
    int open = 0;

    while ( pos < len ) {
        from = pos;
        while ( pos < len && text[pos] != WCH_TAB && text[pos] != WCH_LINE && text[pos] != WCH_PAGE &&
                text[pos] != WCH_NBHYPHEN && text[pos] != WCH_SHY ) {
            pos++;
        }
        if ( pos > from ) {
            if ( !open ) {
                PUT( "<w:t xml:space=\"preserve\">" );
                open = 1;
            }
            xml_esc16( w->part, text + from, pos - from, 0 );
        }
        if ( pos < len ) {
            if ( open ) {
                PUT( "</w:t>" );
                open = 0;
            }
            PUT( text[pos] == WCH_TAB ? "<w:tab/>" : text[pos] == WCH_LINE ? "<w:br/>" :
                 text[pos] == WCH_PAGE ? "<w:br w:type=\"page\"/>" :
                 text[pos] == WCH_NBHYPHEN ? "<w:noBreakHyphen/>" : "<w:softHyphen/>" );
            pos++;
        }
    }
    if ( open ) {
        PUT( "</w:t>" );
    }
}

/* a field character: Word's field, with something for it to show
   until it has worked the field out */
static void docxw_field( DOCXW *w, u32 code, const CHP *chp, const CHP *base )
{
    static const char *const fields[5][2] = {
        { "PAGE", "1" }, { "NUMPAGES", "1" }, { "DATE", "1/1/2026" }, { "TIME", "12:00" },
        { "FILENAME", "Document" }
    };

    out_fmt( w->part, "<w:fldSimple w:instr=\" %s \"><w:r>", fields[code - WCH_PAGENUM][0] );
    docxw_rpr( w, chp, base );
    out_fmt( w->part, "<w:t>%s</w:t></w:r></w:fldSimple>", fields[code - WCH_PAGENUM][1] );
}

/* a note's own number, which Word draws: in the note's first paragraph */
static void docxw_mark( DOCXW *w )
{
    if ( w->note_mark ) {
        out_fmt( w->part, "<w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:%s/></w:r>",
                 w->note_mark == 1 ? "footnoteRef" : "endnoteRef" );
        w->note_mark = 0;
    }
}

static void docxw_para( DOCXW *w, const WPARA *para )
{
    char id[40];
    const WDOC *doc = w->doc;
    const WSTYLE *style = &doc->style[para->pap.style < doc->nstyle ? para->pap.style : 0];
    const CHP *chp;
    const WOBJ *item;
    u32 off, run, pos, from;
    int obj;

    PUT( "<w:p>" );
    docxw_style_id( doc, para->pap.style, id );
    docxw_ppr( w, &para->pap, &style->pap, para->pap.style != 0 && para->pap.style < doc->nstyle ? id : NULL, 0 );
    docxw_mark( w );
    for ( off = 0; off < para->len; off += run ) {
        run = wd_run( para, off );
        chp = wd_chp_get( doc, para->attr[off] );
        if ( para->text[off] == WCH_OBJ ) {
            obj = wd_obj_at( para, off );
            if ( obj < 0 || (u32)obj >= doc->nobj ) {
                continue;
            }
            item = &doc->obj[obj];
            if ( item->kind == OBJ_IMAGE ) {
                if ( docxw_ext( item->fmt ) != NULL && item->data != NULL && item->len != 0 ) {
                    PUT( "<w:r>" );
                    docxw_rpr( w, chp, &style->chp );
                    docxw_image( w, (u32)obj );
                    PUT( "</w:r>" );
                }
            } else if ( item->kind == OBJ_NOTE && w->body && w->note_id[obj] == 0 ) {
                w->note_id[obj] = ++w->next_note[item->fmt == NOTE_END];
                PUT( "<w:r>" );
                docxw_rpr( w, chp, &style->chp );
                out_fmt( w->part, "<w:%s w:id=\"%u\"/></w:r>", item->fmt == NOTE_END ? "endnoteReference" :
                         "footnoteReference", w->note_id[obj] );
            }
            continue;
        }
        /* the run, cut at each field character */
        for ( pos = off; pos < off + run; ) {
            from = pos;
            while ( pos < off + run && !WCH_IS_FIELD( para->text[pos] ) ) {
                pos++;
            }
            if ( pos > from ) {
                PUT( "<w:r>" );
                docxw_rpr( w, chp, &style->chp );
                docxw_run_text( w, para->text + from, pos - from );
                PUT( "</w:r>" );
            }
            if ( pos < off + run ) {
                docxw_field( w, para->text[pos], chp, &style->chp );
                pos++;
            }
        }
    }
    PUT( "</w:p>" );
}

/* a table's column edges, kept in order with no two alike */
static int docxw_grid_add( s32 *grid, int count, s32 pos )
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

/* which edge a place is: the last one not past it */
static int docxw_grid_at( const s32 *grid, int count, s32 pos )
{
    int at = 0;

    while ( at + 1 < count && grid[at + 1] <= pos ) {
        at++;
    }
    return at;
}

static void docxw_cell_open( DOCXW *w, s32 width, int span, int ruled )
{
    out_fmt( w->part, "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"dxa\"/>", width );
    if ( span > 1 ) {
        out_fmt( w->part, "<w:gridSpan w:val=\"%d\"/>", span );
    }
    if ( ruled ) {
        PUT( "<w:tcBorders>" );
        docxw_border( w, "top", 1 );
        docxw_border( w, "left", 1 );
        docxw_border( w, "bottom", 1 );
        docxw_border( w, "right", 1 );
        PUT( "</w:tcBorders>" );
    }
    PUT( "</w:tcPr>" );
}

/* one row: paragraphs "from" to "to" of the story, cut into cells */
static void docxw_row( DOCXW *w, const STORY *story, u32 from, u32 to, const TROW *row, const s32 *grid,
                       int ngrid, int ruled_all )
{
    s32 prev = row->left, edge;
    u32 index = from;
    int cell, col = docxw_grid_at( grid, ngrid, row->left ), next, ncells = row->ncells;
    int ruled = row->borders && !ruled_all;

    if ( ncells > MAX_CELLS ) {
        ncells = MAX_CELLS;
    }
    PUT( "<w:tr>" );
    if ( col > 0 || row->header ) {
        PUT( "<w:trPr>" );
        if ( col > 0 ) {
            out_fmt( w->part, "<w:gridBefore w:val=\"%d\"/>", col );
        }
        if ( row->header ) {
            PUT( "<w:tblHeader/>" );
        }
        PUT( "</w:trPr>" );
    }
    for ( cell = 0; cell < ncells; cell++ ) {
        edge = row->edge[cell] > prev ? row->edge[cell] : prev + 1;
        next = docxw_grid_at( grid, ngrid, edge );
        docxw_cell_open( w, docx_clamp( edge - prev, 1, LEN_MAX ), next - col, ruled );
        col = next > col ? next : col;
        prev = edge;
        if ( index > to ) {
            PUT( "<w:p/>" );            /* a cell the row promised and nothing filled */
        }
        while ( index <= to ) {
            docxw_para( w, story->para[index] );
            index++;
            /* the last cell takes whatever is left of the row */
            if ( (story->para[index - 1]->pap.flags & PF_CELLEND) && cell + 1 < ncells ) {
                break;
            }
        }
        PUT( "</w:tc>" );
    }
    PUT( "</w:tr>" );
}

/* the table that starts at paragraph "from" -> the paragraph after it */
static u32 docxw_table( DOCXW *w, const STORY *story, u32 from )
{
    s32 grid[MAX_GRIDW + 2];
    TROW whole;
    const PAP *pap;
    const TROW *row;
    s32 prev;
    u32 to, index, row_from;
    int ngrid = 0, ruled_all = 1, cell;

    for ( to = from; to < story->count && (story->para[to]->pap.flags & PF_INTABLE); to++ ) {
        pap = &story->para[to]->pap;
        if ( !(pap->flags & PF_ROWEND) || pap->row == NULL ) {
            continue;
        }
        row = pap->row;
        ngrid = docxw_grid_add( grid, ngrid, row->left );
        prev = row->left;
        for ( cell = 0; cell < row->ncells && cell < MAX_CELLS; cell++ ) {
            prev = row->edge[cell] > prev ? row->edge[cell] : prev + 1;
            ngrid = docxw_grid_add( grid, ngrid, prev );
        }
        if ( !row->borders ) {
            ruled_all = 0;
        }
    }
    if ( ngrid < 2 ) {
        /* marked as a table's, with no row to say where its cells are */
        for ( index = from; index < to; index++ ) {
            docxw_para( w, story->para[index] );
        }
        return to;
    }
    out_fmt( w->part, "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/><w:tblInd w:w=\"%d\" w:type=\"dxa\"/>",
             docx_clamp( grid[0], -LEN_MAX, LEN_MAX ) );
    if ( ruled_all ) {
        PUT( "<w:tblBorders>" );
        docxw_border( w, "top", 1 );
        docxw_border( w, "left", 1 );
        docxw_border( w, "bottom", 1 );
        docxw_border( w, "right", 1 );
        docxw_border( w, "insideH", 1 );
        docxw_border( w, "insideV", 1 );
        PUT( "</w:tblBorders>" );
    }
    PUT( "<w:tblLayout w:type=\"fixed\"/></w:tblPr><w:tblGrid>" );
    for ( cell = 0; cell + 1 < ngrid; cell++ ) {
        out_fmt( w->part, "<w:gridCol w:w=\"%d\"/>", docx_clamp( grid[cell + 1] - grid[cell], 1, LEN_MAX ) );
    }
    PUT( "</w:tblGrid>" );
    row_from = from;
    for ( index = from; index < to; index++ ) {
        pap = &story->para[index]->pap;
        if ( (pap->flags & PF_ROWEND) && pap->row != NULL ) {
            docxw_row( w, story, row_from, index, pap->row, grid, ngrid, ruled_all );
            row_from = index + 1;
            docxw_drain( w, 0 );
        }
    }
    if ( row_from < to ) {
        /* paragraphs after the last row that say they are in the table:
           a row of one cell for them */
        mem_set( &whole, 0, sizeof( whole ) );
        whole.ncells = 1;
        whole.left = grid[0];
        whole.edge[0] = grid[ngrid - 1];
        docxw_row( w, story, row_from, to - 1, &whole, grid, ngrid, ruled_all );
    }
    PUT( "</w:tbl>" );
    if ( to == story->count ) {
        PUT( "<w:p/>" );                /* Word will not have a table last */
    }
    return to;
}

static void docxw_story( DOCXW *w, const STORY *story )
{
    u32 index = 0;

    while ( index < story->count ) {
        if ( story->para[index]->pap.flags & PF_INTABLE ) {
            index = docxw_table( w, story, index );
        } else {
            docxw_para( w, story->para[index] );
            index++;
        }
        docxw_drain( w, 0 );
    }
    if ( story->count == 0 ) {
        PUT( "<w:p>" );
        docxw_mark( w );
        PUT( "</w:p>" );
    }
}

/* the relationships of the part just written: for the document itself
   ("main") its other parts, and for any part the pictures it used */
static void docxw_rels( DOCXW *w, const char *name, int main_part )
{
    static const char *const parts[7] = {
        "styles", "settings", "numbering", "footnotes", "endnotes", "header", "footer"
    };
    const WDOC *doc = w->doc;
    u32 index;
    int any = main_part, have[7];

    for ( index = 0; index < doc->nobj; index++ ) {
        any |= w->in_part[index];
    }
    if ( !any ) {
        return;
    }
    docxw_begin( w, name );
    PUT( "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" );
    if ( main_part ) {
        have[0] = have[1] = 1;
        have[2] = w->has_lists;
        have[3] = w->has_foot;
        have[4] = w->has_end;
        have[5] = doc->header.count != 0;
        have[6] = doc->footer.count != 0;
        for ( index = 0; index < 7; index++ ) {
            if ( have[index] ) {
                out_fmt( w->part, "<Relationship Id=\"rId%u\" Type=\"" NS_RELS "/%s\" Target=\"%s%s.xml\"/>", index + 1,
                         parts[index], parts[index], index >= 5 ? "1" : "" );
            }
        }
    }
    for ( index = 0; index < doc->nobj; index++ ) {
        if ( w->in_part[index] ) {
            out_fmt( w->part, "<Relationship Id=\"rImg%u\" Type=\"" NS_RELS "/image\" Target=\"media/image%u.%s\"/>",
                     index + 1, index + 1, docxw_ext( doc->obj[index].fmt ) );
            w->in_part[index] = 0;
        }
    }
    PUT( "</Relationships>" );
    docxw_finish( w );
}

#define W_ROOT  " xmlns:w=\"" NS_MAIN "\" xmlns:r=\"" NS_RELS "\" xmlns:wp=\"" NS_WPDRAW "\"" \
                " xmlns:a=\"" NS_DRAW "\" xmlns:pic=\"" NS_PICT "\">"

/* footnotes.xml or endnotes.xml: the two rules Word wants defined, and
   each note the text referred to */
static void docxw_notes( DOCXW *w, int end )
{
    const WDOC *doc = w->doc;
    const char *tag = end ? "endnote" : "footnote";
    u32 index;

    docxw_begin( w, end ? "word/endnotes.xml" : "word/footnotes.xml" );
    out_fmt( w->part, "<w:%ss", tag );
    PUT( W_ROOT );
    out_fmt( w->part, "<w:%s w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:%s>", tag, tag );
    out_fmt( w->part, "<w:%s w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:r><w:continuationSeparator/></w:r>"
             "</w:p></w:%s>", tag, tag );
    for ( index = 0; index < doc->nobj; index++ ) {
        if ( doc->obj[index].kind != OBJ_NOTE || w->note_id[index] == 0 || doc->obj[index].story == NULL ||
             (doc->obj[index].fmt == NOTE_END) != end ) {
            continue;
        }
        out_fmt( w->part, "<w:%s w:id=\"%u\">", tag, w->note_id[index] );
        w->note_mark = end ? 2 : 1;
        docxw_story( w, doc->obj[index].story );
        w->note_mark = 0;
        out_fmt( w->part, "</w:%s>", tag );
    }
    out_fmt( w->part, "</w:%ss>", tag );
    docxw_finish( w );
    docxw_rels( w, end ? "word/_rels/endnotes.xml.rels" : "word/_rels/footnotes.xml.rels", 0 );
}

static void docxw_header( DOCXW *w, const STORY *story, int footer )
{
    docxw_begin( w, footer ? "word/footer1.xml" : "word/header1.xml" );
    PUT( footer ? "<w:ftr" : "<w:hdr" );
    PUT( W_ROOT );
    docxw_story( w, story );
    PUT( footer ? "</w:ftr>" : "</w:hdr>" );
    docxw_finish( w );
    docxw_rels( w, footer ? "word/_rels/footer1.xml.rels" : "word/_rels/header1.xml.rels", 0 );
}

static void docxw_styles( DOCXW *w )
{
    static const char *const names[STY_OTHER] = {
        "Normal", "heading 1", "heading 2", "heading 3", "heading 4", "heading 5", "heading 6",
        "Title", "Subtitle", "header", "footer", "footnote text"
    };
    char id[40];
    const WDOC *doc = w->doc;
    const WSTYLE *style;
    CHP plain;
    PAP none;
    u32 index;
    int head;

    /* what a style with nothing in it would be: the defaults written
       just below */
    mem_set( &plain, 0, sizeof( plain ) );
    plain.size = 20;
    plain.color = COLOR_AUTO;
    pap_init( &none );
    docxw_begin( w, "word/styles.xml" );
    PUT( "<w:styles xmlns:w=\"" NS_MAIN "\"><w:docDefaults><w:rPrDefault><w:rPr><w:rFonts" );
    docxw_attr_esc( w, "w:ascii", doc->font[0].name );
    docxw_attr_esc( w, "w:hAnsi", doc->font[0].name );
    docxw_attr_esc( w, "w:cs", doc->font[0].name );
    PUT( "/><w:sz w:val=\"20\"/><w:szCs w:val=\"20\"/></w:rPr></w:rPrDefault><w:pPrDefault/></w:docDefaults>" );
    for ( index = 0; index < doc->nstyle; index++ ) {
        style = &doc->style[index];
        docxw_style_id( doc, index, id );
        out_fmt( w->part, "<w:style w:type=\"paragraph\"%s w:styleId=\"%s\"><w:name", index == 0 ? " w:default=\"1\"" : "",
                 id );
        docxw_attr_esc( w, "w:val", index == 0 ? names[0] : style->kind < STY_OTHER ? names[style->kind] :
                        style->name );
        PUT( "/>" );
        head = style->kind >= STY_HEADING1 && style->kind <= STY_HEADING6 ? style->kind - STY_HEADING1 + 1 : 0;
        if ( index != 0 ) {
            PUT( "<w:basedOn w:val=\"Normal\"/>" );
        }
        if ( index != 0 && (head || style->kind == STY_TITLE || style->kind == STY_SUBTITLE) ) {
            PUT( "<w:next w:val=\"Normal\"/>" );
        }
        PUT( "<w:qFormat/>" );
        docxw_ppr( w, &style->pap, index == 0 ? &none : &doc->style[0].pap, NULL, index != 0 ? head : 0 );
        docxw_rpr( w, &style->chp, index == 0 ? &plain : &doc->style[0].chp );
        PUT( "</w:style>" );
    }
    PUT( "</w:styles>" );
    docxw_finish( w );
}

/* numbering.xml: a definition for each list the paragraphs made */
static void docxw_numbering( DOCXW *w )
{
    static const char *const formats[7] = {
        "none", "bullet", "decimal", "lowerLetter", "upperLetter", "lowerRoman", "upperRoman"
    };
    const WLIST *list;
    u32 index;
    int level, kind, fill;

    docxw_begin( w, "word/numbering.xml" );
    PUT( "<w:numbering xmlns:w=\"" NS_MAIN "\">" );
    for ( index = 0; index < w->nlist; index++ ) {
        list = &w->list[index];
        /* the levels nothing used are of the kind the first used one is */
        fill = LIST_NUMBER;
        for ( level = 0; level < NUM_LEVELS; level++ ) {
            if ( list->kind[level] != 0xFF ) {
                fill = list->kind[level];
                break;
            }
        }
        out_fmt( w->part, "<w:abstractNum w:abstractNumId=\"%u\"><w:multiLevelType w:val=\"hybridMultilevel\"/>", index );
        for ( level = 0; level < NUM_LEVELS; level++ ) {
            kind = list->kind[level] != 0xFF ? list->kind[level] : fill;
            if ( kind < LIST_BULLET || kind > LIST_ROMAN_UPPER ) {
                kind = LIST_NUMBER;
            }
            out_fmt( w->part, "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"%u\"/><w:numFmt w:val=\"%s\"/>", level,
                     (u32)(list->kind[level] != 0xFF ? list->start[level] : 1), formats[kind] );
            if ( kind == LIST_BULLET ) {
                /* the round bullet of the Symbol font, as Word's own lists have it */
                PUT( "<w:lvlText w:val=\"\xEF\x82\xB7\"/><w:lvlJc w:val=\"left\"/>" );
            } else {
                out_fmt( w->part, "<w:lvlText w:val=\"%%%d.\"/><w:lvlJc w:val=\"left\"/>", level + 1 );
            }
            out_fmt( w->part, "<w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr>", 720 * (level + 1) );
            if ( kind == LIST_BULLET ) {
                PUT( "<w:rPr><w:rFonts w:ascii=\"Symbol\" w:hAnsi=\"Symbol\" w:hint=\"default\"/></w:rPr>" );
            }
            PUT( "</w:lvl>" );
        }
        PUT( "</w:abstractNum>" );
        docxw_drain( w, 0 );
    }
    for ( index = 0; index < w->nlist; index++ ) {
        out_fmt( w->part, "<w:num w:numId=\"%u\"><w:abstractNumId w:val=\"%u\"/></w:num>", index + 1, index );
        docxw_drain( w, 0 );
    }
    PUT( "</w:numbering>" );
    docxw_finish( w );
}

/* what the file will hold, found before the first part is written:
   the list of parts comes first in it */
static void docxw_survey( DOCXW *w )
{
    const WDOC *doc = w->doc;
    const STORY *story;
    const WPARA *para;
    u32 which, index, at, obj;

    for ( which = 0; which < 3 + doc->nobj; which++ ) {
        story = which == 0 ? &doc->body : which == 1 ? &doc->header : which == 2 ? &doc->footer :
                doc->obj[which - 3].story;
        if ( story == NULL ) {
            continue;
        }
        for ( index = 0; index < story->count; index++ ) {
            para = story->para[index];
            if ( para->pap.list != LIST_NONE && para->pap.list <= LIST_ROMAN_UPPER ) {
                w->has_lists = 1;
            }
            for ( at = 0; which == 0 && at < para->nanch; at++ ) {
                obj = para->anch[at].obj;
                if ( obj < doc->nobj && doc->obj[obj].kind == OBJ_NOTE ) {
                    if ( doc->obj[obj].fmt == NOTE_END ) {
                        w->has_end = 1;
                    } else {
                        w->has_foot = 1;
                    }
                }
            }
        }
    }
}

static void docxw_override( DOCXW *w, const char *part, const char *type )
{
    out_fmt( w->part, "<Override PartName=\"/%s\" ContentType=\"application/vnd.openxmlformats-%s+xml\"/>", part, type );
}

static void docxw_package( DOCXW *w )
{
    const WDOC *doc = w->doc;

    docxw_begin( w, "[Content_Types].xml" );
    PUT( "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
         "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
         "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
         "<Default Extension=\"png\" ContentType=\"image/png\"/>"
         "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
         "<Default Extension=\"gif\" ContentType=\"image/gif\"/>"
         "<Default Extension=\"bmp\" ContentType=\"image/bmp\"/>"
         "<Default Extension=\"tiff\" ContentType=\"image/tiff\"/>"
         "<Default Extension=\"wmf\" ContentType=\"image/x-wmf\"/>"
         "<Default Extension=\"emf\" ContentType=\"image/x-emf\"/>" );
    docxw_override( w, "word/document.xml", "officedocument.wordprocessingml.document.main" );
    docxw_override( w, "word/styles.xml", "officedocument.wordprocessingml.styles" );
    docxw_override( w, "word/settings.xml", "officedocument.wordprocessingml.settings" );
    if ( w->has_lists ) {
        docxw_override( w, "word/numbering.xml", "officedocument.wordprocessingml.numbering" );
    }
    if ( w->has_foot ) {
        docxw_override( w, "word/footnotes.xml", "officedocument.wordprocessingml.footnotes" );
    }
    if ( w->has_end ) {
        docxw_override( w, "word/endnotes.xml", "officedocument.wordprocessingml.endnotes" );
    }
    if ( doc->header.count ) {
        docxw_override( w, "word/header1.xml", "officedocument.wordprocessingml.header" );
    }
    if ( doc->footer.count ) {
        docxw_override( w, "word/footer1.xml", "officedocument.wordprocessingml.footer" );
    }
    docxw_override( w, "docProps/core.xml", "package.core-properties" );
    docxw_override( w, "docProps/app.xml", "officedocument.extended-properties" );
    PUT( "</Types>" );
    docxw_finish( w );

    docxw_begin( w, "_rels/.rels" );
    PUT( "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
         "<Relationship Id=\"rId1\" Type=\"" NS_RELS "/officeDocument\" Target=\"word/document.xml\"/>"
         "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/"
         "core-properties\" Target=\"docProps/core.xml\"/>"
         "<Relationship Id=\"rId3\" Type=\"" NS_RELS "/extended-properties\" Target=\"docProps/app.xml\"/>"
         "</Relationships>" );
    docxw_finish( w );
}

static void docxw_info( DOCXW *w, const char *tag, const char *text )
{
    if ( text[0] ) {
        out_fmt( w->part, "<%s>", tag );
        xml_esc( w->part, text, 0 );
        out_fmt( w->part, "</%s>", tag );
    }
}

static void docxw_props( DOCXW *w )
{
    const WDOC *doc = w->doc;
    DATETIME now;
    char stamp[40];

    sys_now( &now );
    sfmt( stamp, sizeof( stamp ), "%04u-%02u-%02uT%02u:%02u:%02uZ", (u32)now.year, (u32)now.month, (u32)now.day,
          (u32)now.hour, (u32)now.minute, (u32)now.second );
    docxw_begin( w, "docProps/core.xml" );
    PUT( "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\""
         " xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\""
         " xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">" );
    docxw_info( w, "dc:title", doc->title );
    docxw_info( w, "dc:subject", doc->subject );
    docxw_info( w, "dc:creator", doc->author );
    docxw_info( w, "cp:keywords", doc->keywords );
    out_fmt( w->part, "<dcterms:created xsi:type=\"dcterms:W3CDTF\">%s</dcterms:created>"
             "<dcterms:modified xsi:type=\"dcterms:W3CDTF\">%s</dcterms:modified></cp:coreProperties>", stamp, stamp );
    docxw_finish( w );

    docxw_begin( w, "docProps/app.xml" );
    PUT( "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
         "<Application>PMWRITE</Application><AppVersion>1.0000</AppVersion></Properties>" );
    docxw_finish( w );
}

/* a picture's bytes into the archive.  A bare DIB is given the header
   that makes it a .BMP file. */
static void docxw_media( DOCXW *w, u32 obj )
{
    char name[48];
    const WOBJ *image = &w->doc->obj[obj];
    OUT *bmp;
    u8 *data;
    u32 len, head, colors, bits;
    int packed = image->fmt == IMG_PNG || image->fmt == IMG_JPEG || image->fmt == IMG_GIF;

    sfmt( name, sizeof( name ), "word/media/image%u.%s", obj + 1, docxw_ext( image->fmt ) );
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

/* document.xml: the text, and at its end the page */
static void docxw_document( DOCXW *w )
{
    const WDOC *doc = w->doc;
    const PAGESETUP *page = &doc->page;

    docxw_begin( w, "word/document.xml" );
    PUT( "<w:document" );
    PUT( W_ROOT );
    PUT( "<w:body>" );
    w->body = 1;
    docxw_story( w, &doc->body );
    w->body = 0;
    PUT( "<w:sectPr>" );
    if ( doc->header.count ) {
        PUT( "<w:headerReference w:type=\"default\" r:id=\"rId6\"/>" );
    }
    if ( doc->footer.count ) {
        PUT( "<w:footerReference w:type=\"default\" r:id=\"rId7\"/>" );
    }
    out_fmt( w->part, "<w:pgSz w:w=\"%d\" w:h=\"%d\"%s/>", docx_clamp( page->width, 720, LEN_MAX ),
             docx_clamp( page->height, 720, LEN_MAX ), page->landscape ? " w:orient=\"landscape\"" : "" );
    out_fmt( w->part, "<w:pgMar w:top=\"%d\" w:right=\"%d\" w:bottom=\"%d\" w:left=\"%d\" w:header=\"%d\""
             " w:footer=\"%d\" w:gutter=\"0\"/>", docx_clamp( page->top, 0, LEN_MAX ),
             docx_clamp( page->right, 0, LEN_MAX ), docx_clamp( page->bottom, 0, LEN_MAX ),
             docx_clamp( page->left, 0, LEN_MAX ), docx_clamp( page->header, 0, LEN_MAX ),
             docx_clamp( page->footer, 0, LEN_MAX ) );
    if ( page->first_page != 1 ) {
        out_fmt( w->part, "<w:pgNumType w:start=\"%u\"/>", (u32)page->first_page );
    }
    PUT( "</w:sectPr></w:body></w:document>" );
    docxw_finish( w );
    docxw_rels( w, "word/_rels/document.xml.rels", 1 );
}

static int docx_save( WDOC *doc, OUT *out, int variant )
{
    DOCXW *w;
    u32 index;
    int rc = WE_OK;

    (void)variant;
    w = (DOCXW *)try_alloc( sizeof( DOCXW ) );
    if ( w == NULL ) {
        return WE_NOMEM;
    }
    mem_set( w, 0, sizeof( DOCXW ) );
    w->doc = doc;
    w->part = out_mem();
    w->zipw = zipw_new( out );
    w->note_id = (u32 *)try_alloc( (doc->nobj + 1) * sizeof( u32 ) );
    w->in_part = (u8 *)try_alloc( doc->nobj * 2 + 2 );
    if ( w->part == NULL || w->zipw == NULL || w->note_id == NULL || w->in_part == NULL ) {
        w->failed = 1;
    } else {
        mem_set( w->note_id, 0, (doc->nobj + 1) * sizeof( u32 ) );
        mem_set( w->in_part, 0, doc->nobj * 2 + 2 );
        w->in_file = w->in_part + doc->nobj + 1;
        docxw_survey( w );
        docxw_package( w );
        docxw_document( w );
        if ( w->has_foot ) {
            docxw_notes( w, 0 );
        }
        if ( w->has_end ) {
            docxw_notes( w, 1 );
        }
        if ( doc->header.count ) {
            docxw_header( w, &doc->header, 0 );
        }
        if ( doc->footer.count ) {
            docxw_header( w, &doc->footer, 1 );
        }
        docxw_styles( w );
        docxw_begin( w, "word/settings.xml" );
        out_fmt( w->part, "<w:settings xmlns:w=\"" NS_MAIN "\"><w:defaultTabStop w:val=\"%d\"/>",
                 docx_clamp( doc->deftab, 20, LEN_MAX ) );
        if ( w->has_foot ) {
            PUT( "<w:footnotePr><w:footnote w:id=\"-1\"/><w:footnote w:id=\"0\"/></w:footnotePr>" );
        }
        if ( w->has_end ) {
            PUT( "<w:endnotePr><w:endnote w:id=\"-1\"/><w:endnote w:id=\"0\"/></w:endnotePr>" );
        }
        PUT( "<w:compat><w:compatSetting w:name=\"compatibilityMode\""
             " w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"/></w:compat></w:settings>" );
        docxw_finish( w );
        if ( w->has_lists ) {
            docxw_numbering( w );
        }
        for ( index = 0; index < doc->nobj; index++ ) {
            if ( w->in_file[index] ) {
                docxw_media( w, index );
            }
        }
        docxw_props( w );
    }
    if ( w->zipw != NULL && !zipw_end( w->zipw ) ) {
        w->failed = 1;
    }
    if ( w->failed || out_error( out ) ) {
        rc = out_error( out ) != 0 && out_error( out ) != WE_NOMEM ? WE_IO : WE_NOMEM;
    }
    out_close( w->part );
    xfree( w->list );
    xfree( w->note_id );
    xfree( w->in_part );
    xfree( w );
    return rc;
}

const FORMAT fmt_docx = {
    FMT_DOCX, "Word Document", "DOCX;DOCM;DOTX", docx_detect, docx_load, docx_save, NULL, 0
};
