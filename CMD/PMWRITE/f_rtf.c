/*
 * F_RTF.C - Rich Text Format.
 *
 * THE FILE is text: groups in braces, control words that start with a
 * backslash and may carry a number, and the document's own characters
 * between them.  A group saves everything that is in force when it
 * opens and puts it back when it closes; some groups are not text at
 * all but "destinations" - the font table, a picture's bytes in hex,
 * a footnote.
 *
 * READ.  One loop over the bytes, with the saved states in an array on
 * the heap: no function here calls itself, however deep the groups go.
 * Past 256 deep the groups are counted and what is in them is passed
 * over.  A destination that is not known is skipped whole when the
 * file marks it as one that may be (the "\*" in front), and so is
 * anything binary.  Characters come three ways - as themselves, as
 * \'hh in the code page of the font in use, and as \uN in Unicode
 * followed by a stand-in to be passed over.
 *
 * Properties are resolved as they are read: every paragraph and run in
 * an RTF file carries all of its own, so a style is a name and a look
 * and nothing is inherited.
 *
 * WRITE.  Plain RTF of the kind Word 97 and everything since reads:
 * code page 1252 with \uN for what that has not got, a font table, a
 * color table, the styles, lists as a list table, tables, the header
 * and footer, notes, fields, and pictures as PNG or JPEG in hex.
 */
#include "fmt.h"

#define MAX_DEPTH       256
#define RTF_FIELDS      16
#define INST_LEN        80
#define NAME_LEN        64
#define MAX_LEVELS      9
#define WORD_LEN        32
#define HASH_SIZE       1024
#define MAX_FONT_NUM    65535           /* fonts are found by number: one place for each */
#define MAX_RSTYLES     256
#define MAX_RLISTS      2048

/* where the text in a group goes */
enum { D_BODY, D_SKIP, D_FONTTBL, D_COLORTBL, D_STYLESHEET, D_STYLE, D_INFO, D_INFOTEXT, D_PICT,
       D_FLDINST, D_FLDHIDE, D_NOTE, D_HDRFTR, D_PN, D_LISTTABLE, D_LIST, D_LISTLEVEL,
       D_OVERTABLE, D_OVERRIDE, D_LFOLEVEL };

/* what a control word does */
enum { A_NOP, A_SKIP, A_DEST, A_INFO, A_PICT, A_FIELD, A_FLDINST, A_FLDRSLT, A_NOTE, A_FTNALT,
       A_HEADER, A_PN, A_LIST, A_LISTLEVEL, A_OVERRIDE, A_LFOLEVEL, A_UPR, A_UD, A_SHP, A_ANNOT,
       A_PLAIN, A_FLAG, A_UL, A_SUPER, A_FONT, A_FS, A_CF, A_HILITE, A_DELETED, A_REVISED,
       A_PAR, A_PARD, A_ALIGN, A_LI, A_RI, A_FI, A_SB, A_SA, A_SL, A_SLMULT, A_PFLAG, A_STYLE,
       A_INTBL, A_ITAP, A_LS, A_ILVL, A_TX, A_TB, A_TABKIND, A_TABLEAD, A_BRDR, A_BRDRON,
       A_BRDROFF, A_PNKIND, A_PNBODY, A_PNLVL, A_PNSTART, A_LISTID, A_LEVELNFC, A_LEVELSTART,
       A_OVERSTART, A_CHAR, A_FIELDCH, A_SECT, A_CELL, A_ROW, A_NESTCELL, A_TROWD, A_CELLX,
       A_TRLEFT, A_TRHDR, A_CP, A_ANSICPG, A_DEFF, A_UC, A_U, A_BIN, A_DEFTAB, A_PAGE,
       A_LANDSCAPE, A_SPAGE, A_SLAND, A_PGNSTARTS, A_PGNRESTART, A_SBKNONE, A_COLS, A_FAM,
       A_CHARSET, A_FPRQ, A_CPG, A_RGB, A_PICFMT, A_PICNUM, A_CS, A_FCLASS, A_AFONT };

typedef struct {
    const char *name;
    uint8_t  action;
    uint16_t arg;
} RWORD;

/* borders: which one the next line-style word is for */
enum { BT_NONE, BT_TOP, BT_BOTTOM, BT_LEFT, BT_RIGHT, BT_BOX, BT_TABLE };

static const RWORD rtf_words[] = {
    { "fonttbl", A_DEST, D_FONTTBL }, { "colortbl", A_DEST, D_COLORTBL },
    { "stylesheet", A_DEST, D_STYLESHEET }, { "info", A_DEST, D_INFO },
    { "listtable", A_DEST, D_LISTTABLE }, { "listoverridetable", A_DEST, D_OVERTABLE },
    { "title", A_INFO, 1 }, { "subject", A_INFO, 2 }, { "author", A_INFO, 3 },
    { "keywords", A_INFO, 4 },
    { "pict", A_PICT, 0 }, { "shppict", A_NOP, 0 }, { "nonshppict", A_SKIP, 0 },
    { "field", A_FIELD, 0 }, { "fldinst", A_FLDINST, 0 }, { "fldrslt", A_FLDRSLT, 0 },
    { "footnote", A_NOTE, 0 }, { "ftnalt", A_FTNALT, 0 },
    { "header", A_HEADER, 0 }, { "headerr", A_HEADER, 0 }, { "headerl", A_SKIP, 0 },
    { "headerf", A_SKIP, 0 }, { "footer", A_HEADER, 1 }, { "footerr", A_HEADER, 1 },
    { "footerl", A_SKIP, 0 }, { "footerf", A_SKIP, 0 },
    { "pntext", A_SKIP, 0 }, { "listtext", A_SKIP, 0 }, { "pn", A_PN, 0 },
    { "list", A_LIST, 0 }, { "listlevel", A_LISTLEVEL, 0 }, { "listoverride", A_OVERRIDE, 0 },
    { "lfolevel", A_LFOLEVEL, 0 }, { "upr", A_UPR, 0 }, { "ud", A_UD, 0 },
    { "shp", A_SHP, 0 }, { "shpgrp", A_SHP, 0 }, { "do", A_SHP, 0 },
    { "object", A_NOP, 0 }, { "result", A_NOP, 0 }, { "objdata", A_SKIP, 0 },
    { "xe", A_SKIP, 0 }, { "tc", A_SKIP, 0 }, { "annotation", A_ANNOT, 0 },
    { "atnid", A_SKIP, 0 }, { "atnauthor", A_SKIP, 0 }, { "falt", A_SKIP, 0 },
    { "panose", A_SKIP, 0 }, { "fname", A_SKIP, 0 }, { "fontemb", A_SKIP, 0 },
    { "ftnsep", A_SKIP, 0 }, { "ftnsepc", A_SKIP, 0 }, { "ftncn", A_SKIP, 0 },
    { "aftnsep", A_SKIP, 0 }, { "aftnsepc", A_SKIP, 0 }, { "aftncn", A_SKIP, 0 },
    { "bkmkstart", A_SKIP, 0 }, { "bkmkend", A_SKIP, 0 }, { "private", A_SKIP, 0 },
    { "leveltext", A_SKIP, 0 }, { "levelnumbers", A_SKIP, 0 }, { "listname", A_SKIP, 0 },
    { "nonesttables", A_SKIP, 0 },

    { "plain", A_PLAIN, 0 }, { "b", A_FLAG, CF_BOLD }, { "i", A_FLAG, CF_ITALIC },
    { "strike", A_FLAG, CF_STRIKE }, { "striked", A_FLAG, CF_STRIKE },
    { "outl", A_FLAG, CF_OUTLINE }, { "shad", A_FLAG, CF_SHADOW },
    { "scaps", A_FLAG, CF_SMALLCAPS }, { "caps", A_FLAG, CF_ALLCAPS }, { "v", A_FLAG, CF_HIDDEN },
    { "ul", A_UL, CF_UNDER }, { "uld", A_UL, CF_UNDER }, { "uldash", A_UL, CF_UNDER },
    { "ulth", A_UL, CF_UNDER }, { "ulwave", A_UL, CF_UNDER }, { "uldashd", A_UL, CF_UNDER },
    { "uldb", A_UL, CF_DUNDER }, { "ulw", A_UL, CF_WUNDER }, { "ulnone", A_UL, 0 },
    { "super", A_SUPER, CF_SUPER }, { "sub", A_SUPER, CF_SUB }, { "nosupersub", A_SUPER, 0 },
    { "f", A_FONT, 0 }, { "fs", A_FS, 0 }, { "cf", A_CF, 0 }, { "highlight", A_HILITE, 0 },
    { "deleted", A_DELETED, 0 }, { "revised", A_REVISED, 0 }, { "cs", A_CS, 0 },
    { "ts", A_CS, 0 }, { "ds", A_CS, 0 }, { "loch", A_FCLASS, 1 }, { "hich", A_FCLASS, 0 },
    { "dbch", A_FCLASS, 0 }, { "af", A_AFONT, 0 },

    { "par", A_PAR, 0 }, { "pard", A_PARD, 0 }, { "ql", A_ALIGN, AL_LEFT },
    { "qc", A_ALIGN, AL_CENTER }, { "qr", A_ALIGN, AL_RIGHT }, { "qj", A_ALIGN, AL_JUSTIFY },
    { "qd", A_ALIGN, AL_JUSTIFY }, { "li", A_LI, 0 }, { "lin", A_LI, 0 }, { "ri", A_RI, 0 },
    { "rin", A_RI, 0 }, { "fi", A_FI, 0 }, { "sb", A_SB, 0 }, { "sa", A_SA, 0 },
    { "sl", A_SL, 0 }, { "slmult", A_SLMULT, 0 }, { "keep", A_PFLAG, PF_KEEP },
    { "keepn", A_PFLAG, PF_KEEPNEXT }, { "pagebb", A_PFLAG, PF_PAGEBEFORE }, { "s", A_STYLE, 0 },
    { "intbl", A_INTBL, 0 }, { "itap", A_ITAP, 0 }, { "ls", A_LS, 0 }, { "ilvl", A_ILVL, 0 },
    { "tx", A_TX, 0 }, { "tb", A_TB, 0 }, { "tqr", A_TABKIND, TAB_RIGHT },
    { "tqc", A_TABKIND, TAB_CENTER }, { "tqdec", A_TABKIND, TAB_DECIMAL },
    { "tldot", A_TABLEAD, LEAD_DOTS }, { "tlmdot", A_TABLEAD, LEAD_DOTS },
    { "tlhyph", A_TABLEAD, LEAD_DASHES }, { "tlul", A_TABLEAD, LEAD_LINE },
    { "tlth", A_TABLEAD, LEAD_LINE }, { "tleq", A_TABLEAD, LEAD_LINE },
    { "brdrt", A_BRDR, BT_TOP }, { "brdrb", A_BRDR, BT_BOTTOM }, { "brdrl", A_BRDR, BT_LEFT },
    { "brdrr", A_BRDR, BT_RIGHT }, { "box", A_BRDR, BT_BOX }, { "brdrbtw", A_BRDR, BT_NONE },
    { "brdrbar", A_BRDR, BT_NONE }, { "clbrdrt", A_BRDR, BT_TABLE }, { "clbrdrl", A_BRDR, BT_TABLE },
    { "clbrdrb", A_BRDR, BT_TABLE }, { "clbrdrr", A_BRDR, BT_TABLE }, { "trbrdrt", A_BRDR, BT_TABLE },
    { "trbrdrl", A_BRDR, BT_TABLE }, { "trbrdrb", A_BRDR, BT_TABLE }, { "trbrdrr", A_BRDR, BT_TABLE },
    { "trbrdrh", A_BRDR, BT_TABLE }, { "trbrdrv", A_BRDR, BT_TABLE },
    { "brdrs", A_BRDRON, 0 }, { "brdrth", A_BRDRON, 0 }, { "brdrdb", A_BRDRON, 0 },
    { "brdrdot", A_BRDRON, 0 }, { "brdrdash", A_BRDRON, 0 }, { "brdrhair", A_BRDRON, 0 },
    { "brdrsh", A_BRDRON, 0 }, { "brdrdashsm", A_BRDRON, 0 }, { "brdrtriple", A_BRDRON, 0 },
    { "brdrwavy", A_BRDRON, 0 }, { "brdrinset", A_BRDRON, 0 }, { "brdroutset", A_BRDRON, 0 },
    { "brdrnone", A_BRDROFF, 0 }, { "brdrnil", A_BRDROFF, 0 }, { "brdrtbl", A_BRDROFF, 0 },
    { "pnlvlblt", A_PNKIND, LIST_BULLET }, { "pnlvlbody", A_PNBODY, 0 }, { "pnlvl", A_PNLVL, 0 },
    { "pndec", A_PNKIND, LIST_NUMBER }, { "pncard", A_PNKIND, LIST_NUMBER },
    { "pnord", A_PNKIND, LIST_NUMBER }, { "pnucrm", A_PNKIND, LIST_ROMAN_UPPER },
    { "pnlcrm", A_PNKIND, LIST_ROMAN_LOWER }, { "pnucltr", A_PNKIND, LIST_UPPER },
    { "pnlcltr", A_PNKIND, LIST_LOWER }, { "pnstart", A_PNSTART, 0 },
    { "listid", A_LISTID, 0 }, { "levelnfc", A_LEVELNFC, 0 }, { "levelnfcn", A_LEVELNFC, 0 },
    { "levelstartat", A_LEVELSTART, 0 }, { "listoverridestartat", A_OVERSTART, 0 },

    { "tab", A_CHAR, WCH_TAB }, { "line", A_CHAR, WCH_LINE }, { "page", A_CHAR, WCH_PAGE },
    { "column", A_CHAR, WCH_PAGE }, { "emdash", A_CHAR, 0x2014 }, { "endash", A_CHAR, 0x2013 },
    { "bullet", A_CHAR, 0x2022 }, { "lquote", A_CHAR, 0x2018 }, { "rquote", A_CHAR, 0x2019 },
    { "ldblquote", A_CHAR, 0x201C }, { "rdblquote", A_CHAR, 0x201D }, { "emspace", A_CHAR, 0x2003 },
    { "enspace", A_CHAR, 0x2002 }, { "qmspace", A_CHAR, 0x2005 },
    { "chpgn", A_FIELDCH, WCH_PAGENUM }, { "chdate", A_FIELDCH, WCH_DATE },
    { "chdpl", A_FIELDCH, WCH_DATE }, { "chdpa", A_FIELDCH, WCH_DATE },
    { "chtime", A_FIELDCH, WCH_TIME }, { "chftn", A_NOP, 0 },
    { "sect", A_SECT, 0 }, { "sectd", A_NOP, 0 }, { "cell", A_CELL, 0 }, { "row", A_ROW, 0 },
    { "nestcell", A_NESTCELL, 0 }, { "nestrow", A_NOP, 0 },
    { "trowd", A_TROWD, 0 }, { "cellx", A_CELLX, 0 }, { "trleft", A_TRLEFT, 0 },
    { "trhdr", A_TRHDR, 0 },

    { "ansi", A_CP, 1252 }, { "mac", A_CP, 10000 }, { "pc", A_CP, 437 }, { "pca", A_CP, 850 },
    { "ansicpg", A_ANSICPG, 0 }, { "deff", A_DEFF, 0 }, { "uc", A_UC, 0 }, { "u", A_U, 0 },
    { "bin", A_BIN, 0 }, { "deftab", A_DEFTAB, 0 },
    { "paperw", A_PAGE, 0 }, { "paperh", A_PAGE, 1 }, { "margl", A_PAGE, 2 },
    { "margr", A_PAGE, 3 }, { "margt", A_PAGE, 4 }, { "margb", A_PAGE, 5 },
    { "landscape", A_LANDSCAPE, 0 },
    { "pgwsxn", A_SPAGE, 0 }, { "pghsxn", A_SPAGE, 1 }, { "marglsxn", A_SPAGE, 2 },
    { "margrsxn", A_SPAGE, 3 }, { "margtsxn", A_SPAGE, 4 }, { "margbsxn", A_SPAGE, 5 },
    { "headery", A_SPAGE, 6 }, { "footery", A_SPAGE, 7 }, { "lndscpsxn", A_SLAND, 0 },
    { "pgnstarts", A_PGNSTARTS, 0 }, { "pgnrestart", A_PGNRESTART, 0 },
    { "sbknone", A_SBKNONE, 0 }, { "cols", A_COLS, 0 },
    { "fnil", A_FAM, FAM_ANY }, { "froman", A_FAM, FAM_ROMAN }, { "fswiss", A_FAM, FAM_SWISS },
    { "fmodern", A_FAM, FAM_MODERN }, { "fscript", A_FAM, FAM_SCRIPT },
    { "fdecor", A_FAM, FAM_DECOR }, { "ftech", A_FAM, FAM_SYMBOL }, { "fbidi", A_NOP, 0 },
    { "fcharset", A_CHARSET, 0 }, { "fprq", A_FPRQ, 0 }, { "cpg", A_CPG, 0 },
    { "red", A_RGB, 0 }, { "green", A_RGB, 1 }, { "blue", A_RGB, 2 },
    { "pngblip", A_PICFMT, IMG_PNG }, { "jpegblip", A_PICFMT, IMG_JPEG },
    { "emfblip", A_PICFMT, IMG_EMF }, { "wmetafile", A_PICFMT, IMG_WMF },
    { "dibitmap", A_PICFMT, IMG_DIB }, { "macpict", A_PICFMT, IMG_PICT },
    { "wbitmap", A_PICFMT, IMG_UNKNOWN },
    { "picwgoal", A_PICNUM, 0 }, { "pichgoal", A_PICNUM, 1 }, { "picscalex", A_PICNUM, 2 },
    { "picscaley", A_PICNUM, 3 }, { "picw", A_PICNUM, 4 }, { "pich", A_PICNUM, 5 }
};

#define RTF_NWORDS      (sizeof( rtf_words ) / sizeof( rtf_words[0] ))

/* the sixteen colours a highlighter has, in Word's order */
static const uint32_t rtf_ico[17] = {
    COLOR_AUTO, 0x000000UL, 0x0000FFUL, 0x00FFFFUL, 0x00FF00UL, 0xFF00FFUL, 0xFF0000UL,
    0xFFFF00UL, 0xFFFFFFUL, 0x000080UL, 0x008080UL, 0x008000UL, 0x800080UL, 0x800000UL,
    0x808000UL, 0x808080UL, 0xC0C0C0UL
};

/* everything a group saves and puts back */
typedef struct {
    CHP      chp;
    PAP      pap;               /* tabs and row stay NULL: the tabs are below */
    WTAB     tabs[MAX_TABS];
    int32_t  sl;                /* \sl as it was written */
    int32_t  ls;                /* the list it is in, 0 for none */
    int32_t  style;
    uint16_t codepage;          /* of the font in use; 2 for a font of symbols */
    uint16_t pn_start;
    uint8_t  dest;              /* D_* */
    uint8_t  owner;             /* the group that began its destination */
    uint8_t  is_field;
    uint8_t  uc;                /* stand-in characters after a \u */
    uint8_t  slmult;
    uint8_t  intbl;
    uint8_t  itap;
    uint8_t  ilvl;
    uint8_t  dead;              /* text a tracked change took out */
    uint8_t  tab_kind, tab_lead;
    uint8_t  brdr;              /* BT_* */
    uint8_t  pn_kind, pn_level;
    uint8_t  star;              /* a \* came before the next word */
    uint8_t  upr_dest;
    uint8_t  info;
    uint8_t  not_para;          /* a style that is not a paragraph's */
    uint8_t  loch;              /* \loch: the next \af is the font of plain letters */
} RSTATE;

typedef struct {
    int32_t  num;
    int      doc;               /* the document's font, -1 until it is used */
    int32_t  charset;
    uint16_t codepage;
    uint8_t  family;
    int8_t   fixed;
    char     name[FONT_NAME_LEN];
} RFONT;

typedef struct {
    int32_t      num;
    unsigned int doc;
} RSTYLE;

typedef struct {
    int32_t      id;
    unsigned int nlevels;
    uint8_t      nfc[MAX_LEVELS];
    uint16_t     start[MAX_LEVELS];
    uint8_t      seen[MAX_LEVELS], again[MAX_LEVELS];
    int32_t      last_ls[MAX_LEVELS];
} RLIST;

typedef struct {
    int32_t      ls;
    int32_t      id;
    unsigned int nlevels;
    uint16_t     over;
    uint16_t     start[MAX_LEVELS];
} ROVER;

typedef struct {
    uint16_t kind;              /* the character it is, or 0 for one kept as its result */
    uint8_t  put;
    uint8_t  len;
    int      font;              /* a symbol's font, or -1 */
    uint16_t size;              /* ...and its size, or 0 */
    char     inst[INST_LEN];
} RFIELD;

typedef struct {
    WDOC        *doc;
    const uint8_t *data;
    unsigned int len, pos;
    RSTATE      *st;            /* the group in hand */
    RSTATE      *stack;
    unsigned int depth, cap, over_deep;
    uint16_t    *hash;
    WBUILD       body, sub;
    WBUILD      *wb;
    int          in_sub;        /* 1 a header or footer, 2 a note */
    int          note_obj;
    uint16_t     def_cp;
    int32_t      deff;
    unsigned int skip;          /* stand-ins still to pass over */
    RFONT       *font;
    unsigned int nfont, font_cap;
    uint16_t    *font_index;    /* by the file's number: which font, + 1 */
    RFONT        new_font;
    char         name[NAME_LEN];
    unsigned int name_len;
    uint32_t    *color;
    unsigned int ncolor, color_cap;
    uint32_t     rgb[3];
    int          rgb_have;
    RSTYLE      *style;
    unsigned int nstyle, style_cap;
    uint16_t     info[INFO_LEN];
    unsigned int info_len;
    uint8_t     *pict;
    uint32_t     pict_len;
    unsigned int pict_cap;
    int          pict_nibble, pict_fmt, pict_bad;
    int32_t      pic_num[6];
    RFIELD       field[RTF_FIELDS];
    unsigned int nfield;
    TROW         row;
    unsigned int row_cells, cells;
    RLIST       *list;
    unsigned int nlist, list_cap;
    RLIST        new_list;
    ROVER       *over;
    unsigned int nover, over_cap;
    ROVER        new_over;
    uint32_t     lfo_start;
    int          lfo_set;
    unsigned int sect;
    WPARA       *sect_para;
    unsigned int sect_off;
    int          pgn_restart;
    uint32_t     pgn_starts;
    int          was_list;
} RTF;

/* ------------------------------------------------------------------ */
/* small things                                                        */
/* ------------------------------------------------------------------ */

/* room for one more in an array that doubles: 0 if there is no memory */
static int rtf_room( RTF *ctx, void **array, unsigned int *cap, unsigned int count, unsigned int size )
{
    void *fresh;
    unsigned int want;

    if ( count < *cap ) {
        return 1;
    }
    want = *cap ? *cap * 2 : 16;
    if ( want > 0x00100000UL ) {
        return 0;
    }
    fresh = try_realloc( *array, want * size );
    if ( fresh == NULL ) {
        ctx->doc->failed = 1;
        return 0;
    }
    *array = fresh;
    *cap = want;
    return 1;
}

static unsigned int word_hash( const char *name, unsigned int len )
{
    uint32_t hash = 5381;
    unsigned int index;

    for ( index = 0; index < len; index++ ) {
        hash = hash * 33 + (uint8_t)name[index];
    }
    return hash & (HASH_SIZE - 1);
}

static int rtf_hash_build( RTF *ctx )
{
    unsigned int index, slot;

    ctx->hash = (uint16_t *)try_alloc( HASH_SIZE * sizeof( uint16_t ) );
    if ( ctx->hash == NULL ) {
        return 0;
    }
    mem_set( ctx->hash, 0, HASH_SIZE * sizeof( uint16_t ) );
    for ( index = 0; index < RTF_NWORDS; index++ ) {
        slot = word_hash( rtf_words[index].name, str_len( rtf_words[index].name ) );
        while ( ctx->hash[slot] ) {
            slot = (slot + 1) & (HASH_SIZE - 1);
        }
        ctx->hash[slot] = (uint16_t)(index + 1);
    }
    return 1;
}

static const RWORD *rtf_lookup( const RTF *ctx, const char *name, unsigned int len )
{
    unsigned int slot = word_hash( name, len );
    const RWORD *word;

    while ( ctx->hash[slot] ) {
        word = &rtf_words[ctx->hash[slot] - 1];
        if ( str_len( word->name ) == len && mem_cmp( word->name, name, len ) == 0 ) {
            return word;
        }
        slot = (slot + 1) & (HASH_SIZE - 1);
    }
    return NULL;
}

static int is_text_dest( int dest )
{
    return dest == D_BODY || dest == D_NOTE || dest == D_HDRFTR;
}

/* the code page of a font's 8-bit text; 2 marks a font of symbols */
static uint16_t rtf_charset_cp( const RTF *ctx, int32_t charset )
{
    switch ( charset ) {
    case 0:   return CP_1252;
    case 2:   return 2;
    case 77:  return (uint16_t)CP_MAC;
    case 161: return CP_1253;
    case 162: return CP_1254;
    case 204: return CP_1251;
    case 238: return CP_1250;
    case 255: return CP_437;
    }
    return ctx->def_cp;
}

static RFONT *rtf_font_find( RTF *ctx, int32_t num )
{
    unsigned int index;

    if ( ctx->font_index == NULL || num < 0 || num > MAX_FONT_NUM ) {
        return NULL;
    }
    index = ctx->font_index[num];
    return index ? &ctx->font[index - 1] : NULL;
}

/* a font of the file's as the document's: added when it is first used */
static void rtf_font_use( RTF *ctx, RSTATE *st, int32_t num )
{
    RFONT *font = rtf_font_find( ctx, num );

    if ( font == NULL ) {
        st->chp.font = 0;
        st->codepage = ctx->def_cp;
        return;
    }
    if ( font->doc < 0 ) {
        font->doc = wd_font( ctx->doc, font->name, font->family, font->fixed );
    }
    st->chp.font = (uint16_t)font->doc;
    st->codepage = font->codepage;
}

static void rtf_plain( RTF *ctx, RSTATE *st )
{
    mem_set( &st->chp, 0, sizeof( CHP ) );
    st->chp.size = 24;
    st->chp.color = COLOR_AUTO;
    st->dead = 0;
    st->loch = 0;
    rtf_font_use( ctx, st, ctx->deff );
}

static void rtf_pard( RSTATE *st )
{
    pap_init( &st->pap );
    st->sl = 0;
    st->slmult = 0;
    st->ls = 0;
    st->ilvl = 0;
    st->style = 0;
    st->intbl = 0;
    st->itap = 0;
    st->tab_kind = TAB_LEFT;
    st->tab_lead = LEAD_NONE;
    st->brdr = BT_NONE;
    st->pn_kind = LIST_NONE;
    st->pn_level = 0;
    st->pn_start = 0;
}

static int rtf_nfc_list( uint32_t nfc )
{
    switch ( nfc ) {
    case 1:   return LIST_ROMAN_UPPER;
    case 2:   return LIST_ROMAN_LOWER;
    case 3:   return LIST_UPPER;
    case 4:   return LIST_LOWER;
    case 23:  return LIST_BULLET;
    case 255: return LIST_NONE;
    }
    return LIST_NUMBER;
}

/* a state's paragraph properties as the document's: all but the tabs */
static void rtf_pap_of( const RSTATE *st, PAP *pap )
{
    int32_t line = st->sl < 0 ? -st->sl : st->sl;

    *pap = st->pap;
    pap->tabs = NULL;
    pap->row = NULL;
    pap->ntabs = 0;
    if ( line > 0xFFFF ) {
        line = 0xFFFF;
    }
    if ( st->sl == 0 ) {
        pap->line_rule = LS_MULTIPLE;
        pap->line = 240;
    } else if ( st->slmult ) {
        pap->line_rule = LS_MULTIPLE;
        pap->line = (uint16_t)line;
    } else {
        pap->line_rule = (uint8_t)(st->sl < 0 ? LS_EXACT : LS_ATLEAST);
        pap->line = (uint16_t)line;
    }
}

/* the paragraph's properties handed to the builder, for wb_par */
static void rtf_set_pap( RTF *ctx, RSTATE *st, uint16_t extra )
{
    PAP *pap = &ctx->wb->pap;
    RLIST *list = NULL;
    ROVER *over = NULL;
    unsigned int index, level, deeper, start;
    int kind;

    pap_free( pap );
    rtf_pap_of( st, pap );
    if ( st->pap.ntabs && !pap_set_tabs( pap, st->tabs, st->pap.ntabs ) ) {
        ctx->doc->failed = 1;
    }
    pap->flags = (uint16_t)(st->pap.flags | extra | (st->intbl ? PF_INTABLE : 0));
    pap->style = 0;
    for ( index = 0; index < ctx->nstyle; index++ ) {
        if ( ctx->style[index].num == st->style ) {
            pap->style = (uint8_t)ctx->style[index].doc;
            break;
        }
    }
    pap->list = LIST_NONE;
    pap->level = 0;
    pap->list_start = 0;

    /* a list by the list table, or failing that by the paragraph's own
       numbering of the kind Word 6 wrote */
    if ( st->ls > 0 ) {
        for ( index = 0; index < ctx->nover; index++ ) {
            if ( ctx->over[index].ls == st->ls ) {
                over = &ctx->over[index];
                break;
            }
        }
        for ( index = 0; over != NULL && index < ctx->nlist; index++ ) {
            if ( ctx->list[index].id == over->id ) {
                list = &ctx->list[index];
                break;
            }
        }
    }
    if ( list != NULL && list->nlevels != 0 ) {
        level = st->ilvl < list->nlevels ? st->ilvl : list->nlevels - 1;
        kind = rtf_nfc_list( list->nfc[level] );
        if ( kind != LIST_NONE ) {
            pap->list = (uint8_t)kind;
            pap->level = (uint8_t)level;
            start = 0;
            if ( !list->seen[level] || list->again[level] ) {
                start = list->start[level];
                if ( !list->seen[level] && ((over->over >> level) & 1) ) {
                    start = over->start[level];
                }
            } else if ( ((over->over >> level) & 1) && list->last_ls[level] != st->ls ) {
                start = over->start[level];
            }
            list->seen[level] = 1;
            list->again[level] = 0;
            list->last_ls[level] = st->ls;
            for ( deeper = level + 1; deeper < MAX_LEVELS; deeper++ ) {
                list->again[deeper] = 1;
            }
            if ( kind != LIST_BULLET ) {
                pap->list_start = (uint16_t)start;
            }
        }
    } else if ( st->pn_kind != LIST_NONE ) {
        pap->list = st->pn_kind;
        pap->level = st->pn_level;
        if ( st->pn_kind != LIST_BULLET && !ctx->was_list ) {
            pap->list_start = (uint16_t)(st->pn_start ? st->pn_start : 1);
        }
    }
    ctx->was_list = pap->list != LIST_NONE;
}

static void rtf_par( RTF *ctx, uint16_t extra )
{
    if ( !is_text_dest( ctx->st->dest ) ) {
        return;
    }
    rtf_set_pap( ctx, ctx->st, extra );
    wb_par( ctx->wb );
    ctx->sect_para = NULL;
}

/* a unit of text put into the story, looking as the state says */
static void rtf_put( RTF *ctx, uint32_t unit )
{
    uint16_t one = (uint16_t)unit;

    ctx->wb->chp = ctx->st->chp;
    wb_text( ctx->wb, &one, 1 );
}

/* a field's own character, which may say its own font and size */
static void rtf_put_field( RTF *ctx, RFIELD *field )
{
    field->put = 1;
    ctx->wb->chp = ctx->st->chp;
    if ( field->font >= 0 ) {
        ctx->wb->chp.font = (uint16_t)field->font;
    }
    if ( field->size ) {
        ctx->wb->chp.size = field->size;
    }
    wb_text( ctx->wb, &field->kind, 1 );
}

/* ------------------------------------------------------------------ */
/* pictures, fonts, colours, styles: what a destination gathers        */
/* ------------------------------------------------------------------ */

static void rtf_pict_byte( RTF *ctx, uint32_t value )
{
    uint8_t *fresh;
    unsigned int want;

    if ( ctx->pict_bad ) {
        return;
    }
    if ( ctx->pict_len == ctx->pict_cap ) {
        want = ctx->pict_cap ? ctx->pict_cap * 2 : 4096;
        fresh = want > 0x40000000UL ? NULL : (uint8_t *)try_realloc( ctx->pict, want );
        if ( fresh == NULL ) {
            ctx->pict_bad = 1;          /* too big to keep: the picture is lost, not the file */
            return;
        }
        ctx->pict = fresh;
        ctx->pict_cap = want;
    }
    ctx->pict[ctx->pict_len++] = (uint8_t)value;
}

static void rtf_pict_hex( RTF *ctx, uint32_t ch )
{
    int digit = ch >= '0' && ch <= '9' ? (int)ch - '0' : ch >= 'a' && ch <= 'f' ? (int)ch - 'a' + 10 :
                ch >= 'A' && ch <= 'F' ? (int)ch - 'A' + 10 : -1;

    if ( digit < 0 ) {
        return;
    }
    if ( ctx->pict_nibble < 0 ) {
        ctx->pict_nibble = digit;
    } else {
        rtf_pict_byte( ctx, (uint32_t)(ctx->pict_nibble * 16 + digit) );
        ctx->pict_nibble = -1;
    }
}

/* a number from the file kept within what a size can sensibly be */
static int32_t rtf_clamp( int32_t value, int32_t low, int32_t high )
{
    return value < low ? low : value > high ? high : value;
}

/* the picture's group has closed: "outer" is the state it was in */
static void rtf_pict_end( RTF *ctx, const RSTATE *outer )
{
    int width, height;
    int obj;

    if ( ctx->pict_bad || ctx->pict_fmt == IMG_UNKNOWN || ctx->pict_len == 0 ) {
        ctx->doc->lost |= LOST_IMAGES;
        return;
    }
    width = rtf_clamp( ctx->pic_num[0], 0, 200000 );
    height = rtf_clamp( ctx->pic_num[1], 0, 200000 );
    if ( width == 0 || height == 0 ) {
        /* no size it is to be shown at: its own, at 96 dots to the inch */
        width = rtf_clamp( ctx->pic_num[4], 0, 10000 ) * 15;
        height = rtf_clamp( ctx->pic_num[5], 0, 10000 ) * 15;
    }
    width = width * rtf_clamp( ctx->pic_num[2], 1, 4000 ) / 100;
    height = height * rtf_clamp( ctx->pic_num[3], 1, 4000 ) / 100;
    obj = wd_image_new( ctx->doc, ctx->pict, ctx->pict_len, ctx->pict_fmt, width, height );
    if ( obj < 0 ) {
        ctx->doc->lost |= LOST_IMAGES;
        return;
    }
    if ( is_text_dest( outer->dest ) ) {
        ctx->wb->chp = outer->chp;
        wb_object( ctx->wb, obj );
    }
}

/* a font table entry's name has ended */
static void rtf_font_end( RTF *ctx )
{
    /* Word lists a font once for each alphabet it has, the alphabet
       added to its name: it is the same font */
    static const struct {
        int32_t charset;
        const char *tail;
    } tails[] = {
        { 238, " CE" }, { 204, " Cyr" }, { 161, " Greek" }, { 162, " Tur" }, { 186, " Baltic" },
        { 177, " (Hebrew)" }, { 178, " (Arabic)" }, { 163, " (Vietnamese)" }
    };
    RFONT *font;
    unsigned int len, index, tail_len;

    for ( index = 0; index < sizeof( tails ) / sizeof( tails[0] ); index++ ) {
        tail_len = str_len( tails[index].tail );
        if ( ctx->new_font.charset == tails[index].charset && ctx->name_len > tail_len &&
             mem_cmp( ctx->name + ctx->name_len - tail_len, tails[index].tail, tail_len ) == 0 ) {
            ctx->name_len -= tail_len;
        }
    }
    len = ctx->name_len < FONT_NAME_LEN - 1 ? ctx->name_len : FONT_NAME_LEN - 1;
    /* a font is found by its number, so only numbers that have a place
       in the table of them are kept */
    if ( ctx->new_font.num < 0 || ctx->new_font.num > MAX_FONT_NUM || ctx->nfont >= MAX_FONT_NUM ) {
        ctx->name_len = 0;
        return;
    }
    if ( ctx->font_index == NULL ) {
        ctx->font_index = (uint16_t *)try_alloc( (MAX_FONT_NUM + 1) * sizeof( uint16_t ) );
        if ( ctx->font_index == NULL ) {
            ctx->doc->failed = 1;
            return;
        }
        mem_set( ctx->font_index, 0, (MAX_FONT_NUM + 1) * sizeof( uint16_t ) );
    }
    if ( !rtf_room( ctx, (void **)&ctx->font, &ctx->font_cap, ctx->nfont, sizeof( RFONT ) ) ) {
        return;
    }
    ctx->font_index[ctx->new_font.num] = (uint16_t)(ctx->nfont + 1);
    font = &ctx->font[ctx->nfont++];
    *font = ctx->new_font;
    font->doc = -1;
    mem_cpy( font->name, ctx->name, len );
    font->name[len] = 0;
    ctx->name_len = 0;
}

static void rtf_color_end( RTF *ctx )
{
    if ( !rtf_room( ctx, (void **)&ctx->color, &ctx->color_cap, ctx->ncolor, sizeof( uint32_t ) ) ) {
        return;
    }
    ctx->color[ctx->ncolor++] = ctx->rgb_have ? RGB( ctx->rgb[0] & 0xFF, ctx->rgb[1] & 0xFF,
                                                       ctx->rgb[2] & 0xFF ) : COLOR_AUTO;
    ctx->rgb_have = 0;
    ctx->rgb[0] = ctx->rgb[1] = ctx->rgb[2] = 0;
}

/* the styles Word has of its own, by the names it writes for them */
static int rtf_style_kind( const char *name, const char **proper )
{
    static const struct {
        const char *name;
        const char *proper;
        int kind;
    } known[] = {
        { "Normal", "Normal", STY_NORMAL }, { "heading 1", "Heading 1", STY_HEADING1 },
        { "heading 2", "Heading 2", STY_HEADING2 }, { "heading 3", "Heading 3", STY_HEADING3 },
        { "heading 4", "Heading 4", STY_HEADING4 }, { "heading 5", "Heading 5", STY_HEADING5 },
        { "heading 6", "Heading 6", STY_HEADING6 }, { "Title", "Title", STY_TITLE },
        { "Subtitle", "Subtitle", STY_SUBTITLE }, { "header", "Header", STY_HEADER },
        { "footer", "Footer", STY_FOOTER }, { "footnote text", "Footnote Text", STY_FOOTNOTE },
        { "endnote text", "Endnote Text", STY_OTHER }, { "caption", "Caption", STY_OTHER },
        { "heading 7", "Heading 7", STY_OTHER }, { "heading 8", "Heading 8", STY_OTHER },
        { "heading 9", "Heading 9", STY_OTHER }
    };
    unsigned int index;

    for ( index = 0; index < sizeof( known ) / sizeof( known[0] ); index++ ) {
        if ( str_icmp( name, known[index].name ) == 0 ) {
            *proper = known[index].proper;
            return known[index].kind;
        }
    }
    *proper = name;
    return STY_OTHER;
}

/* a style sheet entry's group has closed */
static void rtf_style_end( RTF *ctx, const RSTATE *st )
{
    const char *proper;
    WSTYLE *made;
    int kind, doc_style;

    while ( ctx->name_len && ctx->name[ctx->name_len - 1] == ' ' ) {
        ctx->name_len--;
    }
    ctx->name[ctx->name_len] = 0;
    ctx->name_len = 0;
    if ( st->not_para || ctx->name[0] == 0 ) {
        return;
    }
    kind = rtf_style_kind( ctx->name, &proper );
    doc_style = wd_style( ctx->doc, proper, kind );
    if ( doc_style == 0 && kind != STY_NORMAL ) {
        return;                         /* no room for it: its look is in the paragraphs */
    }
    /* a number that is not in the list is the style Normal, so Normal
       itself need not be in it */
    if ( doc_style != 0 && ctx->nstyle < MAX_RSTYLES &&
         rtf_room( ctx, (void **)&ctx->style, &ctx->style_cap, ctx->nstyle, sizeof( RSTYLE ) ) ) {
        ctx->style[ctx->nstyle].num = st->style;
        ctx->style[ctx->nstyle].doc = (unsigned int)doc_style;
        ctx->nstyle++;
    }
    made = &ctx->doc->style[doc_style];
    made->chp = st->chp;
    rtf_pap_of( st, &made->pap );
    made->pap.style = 0;
    made->pap.list = LIST_NONE;
}

/* a number in a field's instruction at *at, which moves past it */
static uint32_t rtf_inst_number( const RFIELD *field, unsigned int *at )
{
    uint32_t value = 0;

    while ( *at < field->len && field->inst[*at] == ' ' ) {
        (*at)++;
    }
    while ( *at < field->len && field->inst[*at] >= '0' && field->inst[*at] <= '9' && value < 100000UL ) {
        value = value * 10 + (uint32_t)(field->inst[(*at)++] - '0');
    }
    return value;
}

/*
 * SYMBOL 97 \f "Symbol" \s 12: one character of a named font.  A font
 * of symbols has its characters where Word 97 keeps them, at F000h.
 */
static void rtf_field_symbol( RTF *ctx, RFIELD *field, unsigned int at )
{
    char name[FONT_NAME_LEN];
    uint32_t code = rtf_inst_number( field, &at );
    unsigned int len, size;
    int unicode = 0;

    while ( at + 1 < field->len ) {
        if ( field->inst[at] != '\\' ) {
            at++;
            continue;
        }
        at += 2;
        switch ( field->inst[at - 1] ) {
        case 'u':
            unicode = 1;
            break;
        case 's':
            size = rtf_inst_number( field, &at );
            if ( size >= 1 && size <= 500 ) {
                field->size = (uint16_t)(size * 2);
            }
            break;
        case 'f':
            while ( at < field->len && field->inst[at] == ' ' ) {
                at++;
            }
            if ( at < field->len && field->inst[at] == '"' ) {
                at++;
                for ( len = 0; at < field->len && field->inst[at] != '"'; at++ ) {
                    if ( len < FONT_NAME_LEN - 1 ) {
                        name[len++] = field->inst[at];
                    }
                }
                name[len] = 0;
                field->font = wd_font( ctx->doc, name, FAM_ANY, -1 );
            }
            break;
        }
    }
    if ( code == 0 || code > 0xFFFF ) {
        return;
    }
    if ( !unicode && code < 0x100 ) {
        if ( field->font >= 0 && ctx->doc->font[field->font].family == FAM_SYMBOL ) {
            code += 0xF000;
        } else if ( code >= 0x80 ) {
            code = cp_to_uni( CP_1252, (int)code );
        }
    }
    field->kind = (uint16_t)code;
}

/* which of the fields that are kept as themselves an instruction names */
static uint16_t rtf_field_kind( RTF *ctx, RFIELD *field )
{
    static const struct {
        const char *word;
        uint16_t code;
    } known[] = {
        { "PAGE", WCH_PAGENUM }, { "NUMPAGES", WCH_NUMPAGES }, { "DATE", WCH_DATE },
        { "TIME", WCH_TIME }, { "FILENAME", WCH_FILENAME }
    };
    char word[INST_LEN];
    unsigned int at = 0, len = 0, index;

    while ( at < field->len && field->inst[at] == ' ' ) {
        at++;
    }
    while ( at < field->len && ((field->inst[at] >= 'A' && field->inst[at] <= 'Z') ||
                                (field->inst[at] >= 'a' && field->inst[at] <= 'z')) ) {
        word[len++] = (char)ch_upper( field->inst[at++] );
    }
    word[len] = 0;
    for ( index = 0; index < sizeof( known ) / sizeof( known[0] ); index++ ) {
        if ( str_cmp( word, known[index].word ) == 0 ) {
            return known[index].code;
        }
    }
    if ( str_cmp( word, "SYMBOL" ) == 0 ) {
        rtf_field_symbol( ctx, field, at );
        return field->kind;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* characters                                                          */
/* ------------------------------------------------------------------ */

/* a character of the file: "byte" says it came as one, in the code
   page of the font in use, and not as Unicode */
static void rtf_char( RTF *ctx, uint32_t unit, int byte )
{
    RSTATE *st = ctx->st;
    RFIELD *field;

    if ( ctx->over_deep ) {
        return;
    }
    switch ( st->dest ) {
    case D_BODY:
    case D_NOTE:
    case D_HDRFTR:
        if ( st->dead ) {
            return;
        }
        if ( byte ) {
            if ( st->codepage == 2 ) {
                unit = unit >= 0x20 ? 0xF000 + unit : unit;
            } else if ( unit >= 0x80 ) {
                unit = cp_to_uni( st->codepage, (int)unit );
            }
        }
        if ( unit == 0 || (unit < 0x20 && unit != WCH_TAB && unit != WCH_LINE && unit != WCH_PAGE) ) {
            return;
        }
        if ( unit >= WCH_PAGENUM && unit <= WCH_FILENAME ) {
            unit = 0xFFFD;              /* the document's own field marks */
            ctx->doc->lost |= LOST_TEXT;
        }
        rtf_put( ctx, unit );
        return;
    case D_FLDHIDE:
        /* a result that is not kept: the field's own character instead,
           looking as the result did */
        if ( ctx->nfield ) {
            field = &ctx->field[ctx->nfield - 1];
            if ( field->kind && !field->put ) {
                rtf_put_field( ctx, field );
            }
        }
        return;
    case D_FLDINST:
        if ( ctx->nfield ) {
            field = &ctx->field[ctx->nfield - 1];
            if ( field->len < INST_LEN - 1 && unit < 0x80 ) {
                field->inst[field->len++] = (char)unit;
            }
        }
        return;
    case D_FONTTBL:
        if ( unit == ';' ) {
            rtf_font_end( ctx );
        } else if ( ctx->name_len < NAME_LEN - 1 && unit >= 0x20 && unit < 0x7F &&
                    (unit != ' ' || ctx->name_len != 0) ) {
            ctx->name[ctx->name_len++] = (char)unit;
        }
        return;
    case D_COLORTBL:
        if ( unit == ';' ) {
            rtf_color_end( ctx );
        }
        return;
    case D_STYLE:
        if ( unit != ';' && ctx->name_len < NAME_LEN - 1 && unit >= 0x20 &&
             (unit != ' ' || ctx->name_len != 0) ) {
            ctx->name[ctx->name_len++] = (char)(unit < 0x7F ? unit : '?');
        }
        return;
    case D_INFOTEXT:
        if ( byte && unit >= 0x80 ) {
            unit = cp_to_uni( st->codepage == 2 ? CP_1252 : st->codepage, (int)unit );
        }
        if ( ctx->info_len < INFO_LEN - 1 && unit >= 0x20 ) {
            ctx->info[ctx->info_len++] = (uint16_t)unit;
        }
        return;
    case D_PICT:
        rtf_pict_hex( ctx, unit );
        return;
    }
}

/* ------------------------------------------------------------------ */
/* groups                                                              */
/* ------------------------------------------------------------------ */

static void rtf_push( RTF *ctx )
{
    RSTATE *stack, *st;
    unsigned int want;

    ctx->skip = 0;
    if ( ctx->over_deep || ctx->depth + 1 >= MAX_DEPTH ) {
        ctx->over_deep++;
        return;
    }
    if ( ctx->depth + 1 >= ctx->cap ) {
        want = ctx->cap * 2;
        stack = (RSTATE *)try_realloc( ctx->stack, want * sizeof( RSTATE ) );
        if ( stack == NULL ) {
            ctx->doc->failed = 1;
            ctx->over_deep++;
            return;
        }
        ctx->stack = stack;
        ctx->cap = want;
    }
    ctx->stack[ctx->depth + 1] = ctx->stack[ctx->depth];
    ctx->depth++;
    st = ctx->st = &ctx->stack[ctx->depth];
    st->owner = 0;
    st->is_field = 0;
    st->star = 0;
    if ( st->dest == D_STYLESHEET ) {
        /* each group of the style sheet is one style, described in full */
        st->dest = D_STYLE;
        st->owner = 1;
        st->not_para = 0;
        rtf_pard( st );
        rtf_plain( ctx, st );
        ctx->name_len = 0;
    }
}

/* the note or header whose group is closing is finished */
static void rtf_sub_end( RTF *ctx, RSTATE *st )
{
    if ( ctx->sub.para != NULL ) {
        rtf_set_pap( ctx, st, 0 );
    }
    wb_done( &ctx->sub );
    ctx->wb = &ctx->body;
    ctx->in_sub = 0;
}

static void rtf_pop( RTF *ctx )
{
    RSTATE *st = ctx->st, *outer;
    RFIELD *field;
    char *dst;

    ctx->skip = 0;
    if ( ctx->over_deep ) {
        ctx->over_deep--;
        return;
    }
    if ( ctx->depth == 0 ) {
        ctx->pos = ctx->len;            /* more closed than were opened: the end */
        return;
    }
    outer = &ctx->stack[ctx->depth - 1];
    if ( st->owner ) {
        switch ( st->dest ) {
        case D_PICT:
            rtf_pict_end( ctx, outer );
            break;
        case D_STYLE:
            rtf_style_end( ctx, st );
            break;
        case D_INFOTEXT:
            dst = st->info == 1 ? ctx->doc->title : st->info == 2 ? ctx->doc->subject :
                  st->info == 3 ? ctx->doc->author : ctx->doc->keywords;
            utf8_copy( dst, INFO_LEN, ctx->info, ctx->info_len );
            break;
        case D_FLDINST:
            if ( ctx->nfield ) {
                ctx->field[ctx->nfield - 1].kind = rtf_field_kind( ctx, &ctx->field[ctx->nfield - 1] );
            }
            break;
        case D_NOTE:
            rtf_sub_end( ctx, st );
            if ( is_text_dest( outer->dest ) ) {
                ctx->wb->chp = outer->chp;
                wb_object( ctx->wb, ctx->note_obj );
            }
            break;
        case D_HDRFTR:
            rtf_sub_end( ctx, st );
            break;
        case D_PN:
            outer->pn_kind = st->pn_kind;
            outer->pn_level = st->pn_level;
            outer->pn_start = st->pn_start;
            break;
        case D_LISTLEVEL:
            if ( ctx->new_list.nlevels < MAX_LEVELS ) {
                ctx->new_list.nlevels++;
            }
            break;
        case D_LIST:
            if ( ctx->nlist < MAX_RLISTS &&
                 rtf_room( ctx, (void **)&ctx->list, &ctx->list_cap, ctx->nlist, sizeof( RLIST ) ) ) {
                ctx->list[ctx->nlist++] = ctx->new_list;
            }
            break;
        case D_LFOLEVEL:
            if ( ctx->new_over.nlevels < MAX_LEVELS ) {
                if ( ctx->lfo_set ) {
                    ctx->new_over.over |= (uint16_t)(1u << ctx->new_over.nlevels);
                    ctx->new_over.start[ctx->new_over.nlevels] = (uint16_t)ctx->lfo_start;
                }
                ctx->new_over.nlevels++;
            }
            break;
        case D_OVERRIDE:
            if ( ctx->nover < MAX_RLISTS &&
                 rtf_room( ctx, (void **)&ctx->over, &ctx->over_cap, ctx->nover, sizeof( ROVER ) ) ) {
                ctx->over[ctx->nover++] = ctx->new_over;
            }
            break;
        }
    }
    if ( st->is_field && ctx->nfield ) {
        /* a field with no result at all still has its character */
        field = &ctx->field[--ctx->nfield];
        if ( field->kind && !field->put && is_text_dest( outer->dest ) ) {
            ctx->st = outer;
            rtf_put_field( ctx, field );
        }
    }
    ctx->depth--;
    ctx->st = &ctx->stack[ctx->depth];
}

/* a destination begins in the group in hand */
static void rtf_dest( RSTATE *st, int dest )
{
    st->dest = (uint8_t)dest;
    st->owner = 1;
}

/* ------------------------------------------------------------------ */
/* tables and sections                                                 */
/* ------------------------------------------------------------------ */

/* \row: the paragraph before it is the last of the row */
static void rtf_row_end( RTF *ctx )
{
    STORY *story = ctx->wb->story;
    WPARA *prev;
    TROW row;
    unsigned int cells = ctx->cells, have = ctx->row_cells, index, width;

    ctx->cells = 0;
    if ( ctx->wb->para != NULL ) {
        rtf_par( ctx, PF_INTABLE );     /* text after the last \cell: kept */
    }
    if ( story->count == 0 || cells == 0 ) {
        return;
    }
    prev = story->para[story->count - 1];
    if ( !(prev->pap.flags & PF_INTABLE) ) {
        return;
    }
    if ( cells > MAX_CELLS ) {
        cells = MAX_CELLS;
        ctx->doc->lost |= LOST_TABLES;
    }
    if ( have > MAX_CELLS ) {
        have = MAX_CELLS;
    }
    row = ctx->row;
    if ( have == 0 ) {
        width = (unsigned int)(ctx->doc->page.width - ctx->doc->page.left - ctx->doc->page.right);
        if ( width < 1440 || width > 31680 ) {
            width = 8640;
        }
        row.left = 0;
        for ( index = 0; index < cells; index++ ) {
            row.edge[index] = (int)(width * (index + 1) / cells);
        }
    }
    for ( index = 0; index < cells; index++ ) {
        int before = index ? row.edge[index - 1] : row.left;

        if ( index >= have && have != 0 ) {
            row.edge[index] = before + 720;
        }
        if ( row.edge[index] <= before ) {
            row.edge[index] = before + 1;
        }
    }
    row.ncells = (uint8_t)cells;
    row.spare = 0;
    prev->pap.flags |= PF_CELLEND | PF_ROWEND;
    if ( !pap_set_row( &prev->pap, &row ) ) {
        ctx->doc->failed = 1;
    }
}

/* ------------------------------------------------------------------ */
/* control words                                                       */
/* ------------------------------------------------------------------ */

static void rtf_tab_add( RSTATE *st, int pos )
{
    unsigned int count = st->pap.ntabs, slot;

    for ( slot = 0; slot < count && st->tabs[slot].pos < pos; slot++ ) {
    }
    if ( slot == count || st->tabs[slot].pos != pos ) {
        if ( count >= MAX_TABS ) {
            return;
        }
        mem_cpy( &st->tabs[slot + 1], &st->tabs[slot], (count - slot) * sizeof( WTAB ) );
        st->pap.ntabs = (uint8_t)(count + 1);
    }
    st->tabs[slot].pos = pos;
    st->tabs[slot].kind = st->tab_kind;
    st->tabs[slot].lead = st->tab_lead;
    st->tabs[slot].spare = 0;
}

static void rtf_border( RTF *ctx, RSTATE *st, int on )
{
    uint16_t flags = 0;

    switch ( st->brdr ) {
    case BT_TOP:    flags = PF_BORDER_TOP; break;
    case BT_BOTTOM: flags = PF_BORDER_BOT; break;
    case BT_LEFT:   flags = PF_BORDER_LEFT; break;
    case BT_RIGHT:  flags = PF_BORDER_RIGHT; break;
    case BT_BOX:    flags = PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT; break;
    case BT_TABLE:
        if ( on ) {
            ctx->row.borders = 1;
        }
        return;
    }
    if ( on ) {
        st->pap.flags |= flags;
    } else {
        st->pap.flags &= (uint16_t)~flags;
    }
}

/* a number from the file as a length in twips that the document can hold */
static int rtf_twips( int32_t value )
{
    return rtf_clamp( value, -31680, 31680 );
}

static void rtf_word( RTF *ctx, const RWORD *word, int has, int32_t num )
{
    RSTATE *st = ctx->st;
    WDOC *doc = ctx->doc;
    int text = is_text_dest( st->dest ), obj;
    unsigned int index;

    switch ( word->action ) {
    case A_NOP:
        break;
    case A_SKIP:
        st->dest = D_SKIP;
        break;
    case A_SHP:
        st->dest = D_SKIP;
        doc->lost |= LOST_OBJECTS;
        break;
    case A_ANNOT:
        st->dest = D_SKIP;
        doc->lost |= LOST_NOTES;
        break;
    case A_DEST:
        if ( st->dest == D_BODY ) {
            rtf_dest( st, word->arg );
            if ( word->arg == D_COLORTBL ) {
                ctx->rgb_have = 0;
            } else if ( word->arg == D_FONTTBL ) {
                ctx->name_len = 0;
                mem_set( &ctx->new_font, 0, sizeof( RFONT ) );
                ctx->new_font.codepage = ctx->def_cp;
                ctx->new_font.fixed = -1;
            }
        } else {
            st->dest = D_SKIP;
        }
        break;
    case A_INFO:
        if ( st->dest == D_INFO ) {
            rtf_dest( st, D_INFOTEXT );
            st->info = (uint8_t)word->arg;
            ctx->info_len = 0;
        }
        break;
    case A_PICT:
        if ( text ) {
            rtf_dest( st, D_PICT );
            ctx->pict_len = 0;
            ctx->pict_nibble = -1;
            ctx->pict_fmt = IMG_UNKNOWN;
            ctx->pict_bad = 0;
            mem_set( ctx->pic_num, 0, sizeof( ctx->pic_num ) );
            ctx->pic_num[2] = ctx->pic_num[3] = 100;
        } else {
            st->dest = D_SKIP;
        }
        break;
    case A_FIELD:
        if ( text && ctx->nfield < RTF_FIELDS ) {
            mem_set( &ctx->field[ctx->nfield], 0, sizeof( RFIELD ) );
            ctx->field[ctx->nfield++].font = -1;
            st->is_field = 1;
        } else if ( text ) {
            st->dest = D_SKIP;          /* fields in fields, deeper than is kept count of */
        }
        break;
    case A_FLDINST:
        if ( text && ctx->nfield ) {
            rtf_dest( st, D_FLDINST );
        } else {
            st->dest = D_SKIP;
        }
        break;
    case A_FLDRSLT:
        if ( text && ctx->nfield ) {
            if ( ctx->field[ctx->nfield - 1].kind ) {
                st->dest = D_FLDHIDE;
            } else {
                doc->lost |= LOST_FIELDS;
            }
        }
        break;
    case A_NOTE:
        if ( st->dest == D_BODY && !ctx->in_sub ) {
            obj = wd_note_new( doc, NOTE_FOOT );
            if ( obj >= 0 ) {
                rtf_dest( st, D_NOTE );
                ctx->note_obj = obj;
                ctx->in_sub = 2;
                wb_init( &ctx->sub, doc, wd_note_story( doc, obj ) );
                ctx->wb = &ctx->sub;
                break;
            }
        }
        st->dest = D_SKIP;
        break;
    case A_FTNALT:
        if ( st->dest == D_NOTE && ctx->in_sub == 2 && (unsigned int)ctx->note_obj < doc->nobj ) {
            doc->obj[ctx->note_obj].fmt = NOTE_END;
        }
        break;
    case A_HEADER:
        if ( st->dest == D_BODY && !ctx->in_sub && (word->arg ? doc->footer.count : doc->header.count) == 0 ) {
            rtf_dest( st, D_HDRFTR );
            ctx->in_sub = 1;
            wb_init( &ctx->sub, doc, word->arg ? &doc->footer : &doc->header );
            ctx->wb = &ctx->sub;
        } else {
            st->dest = D_SKIP;
        }
        break;
    case A_PN:
        if ( text ) {
            rtf_dest( st, D_PN );
            st->pn_kind = LIST_NUMBER;
            st->pn_level = 0;
            st->pn_start = 0;
        } else {
            st->dest = D_SKIP;
        }
        break;
    case A_LIST:
        if ( st->dest == D_LISTTABLE ) {
            rtf_dest( st, D_LIST );
            mem_set( &ctx->new_list, 0, sizeof( RLIST ) );
        }
        break;
    case A_LISTLEVEL:
        if ( st->dest == D_LIST ) {
            rtf_dest( st, D_LISTLEVEL );
            if ( ctx->new_list.nlevels < MAX_LEVELS ) {
                ctx->new_list.nfc[ctx->new_list.nlevels] = 0;
                ctx->new_list.start[ctx->new_list.nlevels] = 1;
            }
        }
        break;
    case A_OVERRIDE:
        if ( st->dest == D_OVERTABLE ) {
            rtf_dest( st, D_OVERRIDE );
            mem_set( &ctx->new_over, 0, sizeof( ROVER ) );
        }
        break;
    case A_LFOLEVEL:
        if ( st->dest == D_OVERRIDE ) {
            rtf_dest( st, D_LFOLEVEL );
            ctx->lfo_set = 0;
            ctx->lfo_start = 1;
        }
        break;
    case A_UPR:
        /* the same text twice: the plain one is passed over */
        st->upr_dest = st->dest;
        st->dest = D_SKIP;
        break;
    case A_UD:
        st->dest = st->upr_dest;
        break;

    case A_PLAIN:
        rtf_plain( ctx, st );
        break;
    case A_FLAG:
        if ( has && num == 0 ) {
            st->chp.flags &= (uint16_t)~word->arg;
        } else {
            st->chp.flags |= word->arg;
        }
        break;
    case A_UL:
        st->chp.flags &= (uint16_t)~(CF_UNDER | CF_DUNDER | CF_WUNDER);
        if ( !(has && num == 0) ) {
            st->chp.flags |= word->arg;
        }
        break;
    case A_SUPER:
        st->chp.flags &= (uint16_t)~(CF_SUPER | CF_SUB);
        if ( !(has && num == 0) ) {
            st->chp.flags |= word->arg;
        }
        break;
    case A_FONT:
        if ( st->dest == D_FONTTBL ) {
            mem_set( &ctx->new_font, 0, sizeof( RFONT ) );
            ctx->new_font.num = num;
            ctx->new_font.codepage = ctx->def_cp;
            ctx->new_font.fixed = -1;
            ctx->name_len = 0;
        } else {
            rtf_font_use( ctx, st, num );
        }
        break;
    case A_FS:
        st->chp.size = (uint16_t)rtf_clamp( num, 2, 1000 );
        break;
    case A_CF:
        st->chp.color = num > 0 && (unsigned int)num < ctx->ncolor ? ctx->color[num] : COLOR_AUTO;
        break;
    case A_HILITE:
        st->chp.hilite = 0;
        if ( num > 0 && (unsigned int)num < ctx->ncolor ) {
            st->chp.hilite = 7;         /* a color the highlighter has not got: yellow */
            for ( index = 1; index <= 16; index++ ) {
                if ( rtf_ico[index] == ctx->color[num] ) {
                    st->chp.hilite = (uint8_t)index;
                }
            }
        }
        break;
    case A_DELETED:
        st->dead = (uint8_t)!(has && num == 0);
        doc->lost |= LOST_REVISIONS;
        break;
    case A_REVISED:
        doc->lost |= LOST_REVISIONS;
        break;
    case A_CS:
        if ( st->dest == D_STYLE ) {
            st->not_para = 1;
        }
        break;
    case A_FCLASS:
        st->loch = (uint8_t)word->arg;
        break;
    case A_AFONT:
        /* the font of plain letters, where a run names no font of its own */
        if ( st->loch && st->dest != D_FONTTBL ) {
            rtf_font_use( ctx, st, num );
        }
        break;

    case A_PAR:
        rtf_par( ctx, 0 );
        break;
    case A_PARD:
        rtf_pard( st );
        break;
    case A_ALIGN:
        st->pap.align = (uint8_t)word->arg;
        break;
    case A_LI:
        st->pap.left = rtf_twips( num );
        break;
    case A_RI:
        st->pap.right = rtf_twips( num );
        break;
    case A_FI:
        st->pap.first = rtf_twips( num );
        break;
    case A_SB:
        st->pap.before = (uint16_t)rtf_clamp( num, 0, 31680 );
        break;
    case A_SA:
        st->pap.after = (uint16_t)rtf_clamp( num, 0, 31680 );
        break;
    case A_SL:
        st->sl = rtf_twips( num );
        break;
    case A_SLMULT:
        st->slmult = (uint8_t)(num != 0);
        break;
    case A_PFLAG:
        if ( has && num == 0 ) {
            st->pap.flags &= (uint16_t)~word->arg;
        } else {
            st->pap.flags |= word->arg;
        }
        break;
    case A_STYLE:
        st->style = num;
        break;
    case A_INTBL:
        st->intbl = 1;
        break;
    case A_ITAP:
        st->itap = (uint8_t)rtf_clamp( num, 0, 255 );
        break;
    case A_LS:
        if ( st->dest == D_OVERRIDE ) {
            ctx->new_over.ls = num;
        } else {
            st->ls = num;
        }
        break;
    case A_ILVL:
        st->ilvl = (uint8_t)rtf_clamp( num, 0, MAX_LEVELS - 1 );
        break;
    case A_TX:
        rtf_tab_add( st, rtf_twips( num ) );
        st->tab_kind = TAB_LEFT;
        st->tab_lead = LEAD_NONE;
        break;
    case A_TB:
        st->tab_kind = TAB_LEFT;        /* a bar is not a place to tab to */
        st->tab_lead = LEAD_NONE;
        break;
    case A_TABKIND:
        st->tab_kind = (uint8_t)word->arg;
        break;
    case A_TABLEAD:
        st->tab_lead = (uint8_t)word->arg;
        break;
    case A_BRDR:
        st->brdr = (uint8_t)word->arg;
        break;
    case A_BRDRON:
        rtf_border( ctx, st, 1 );
        break;
    case A_BRDROFF:
        rtf_border( ctx, st, 0 );
        break;
    case A_PNKIND:
        st->pn_kind = (uint8_t)word->arg;
        break;
    case A_PNBODY:
        st->pn_level = 0;
        break;
    case A_PNLVL:
        st->pn_level = (uint8_t)rtf_clamp( num - 1, 0, MAX_LEVELS - 1 );
        break;
    case A_PNSTART:
        st->pn_start = (uint16_t)rtf_clamp( num, 0, 0xFFFF );
        break;
    case A_LISTID:
        if ( st->dest == D_LIST ) {
            ctx->new_list.id = num;
        } else if ( st->dest == D_OVERRIDE ) {
            ctx->new_over.id = num;
        }
        break;
    case A_LEVELNFC:
        if ( st->dest == D_LISTLEVEL && ctx->new_list.nlevels < MAX_LEVELS ) {
            ctx->new_list.nfc[ctx->new_list.nlevels] = (uint8_t)rtf_clamp( num, 0, 255 );
        }
        break;
    case A_LEVELSTART:
        if ( st->dest == D_LISTLEVEL && ctx->new_list.nlevels < MAX_LEVELS ) {
            ctx->new_list.start[ctx->new_list.nlevels] = (uint16_t)rtf_clamp( num, 0, 0xFFFF );
        } else if ( st->dest == D_LFOLEVEL ) {
            ctx->lfo_start = (uint32_t)rtf_clamp( num, 0, 0xFFFF );
        }
        break;
    case A_OVERSTART:
        if ( st->dest == D_LFOLEVEL ) {
            ctx->lfo_set = 1;
        }
        break;

    case A_CHAR:
        rtf_char( ctx, word->arg, 0 );
        break;
    case A_FIELDCH:
        if ( text && !st->dead ) {
            rtf_put( ctx, word->arg );
        }
        break;
    case A_SECT:
        if ( st->dest == D_BODY && !ctx->in_sub ) {
            /* a section's end is a paragraph's end, and what follows
               starts a page - unless the next section says it does not */
            rtf_put( ctx, WCH_PAGE );
            rtf_par( ctx, 0 );
            if ( ctx->body.story->count ) {
                ctx->sect_para = ctx->body.story->para[ctx->body.story->count - 1];
                ctx->sect_off = ctx->sect_para->len - 1;
            }
            ctx->sect++;
        }
        break;
    case A_SBKNONE:
        if ( ctx->sect_para != NULL && ctx->sect_off < ctx->sect_para->len &&
             ctx->sect_para->text[ctx->sect_off] == WCH_PAGE ) {
            wd_text_delete( doc, ctx->sect_para, ctx->sect_off, 1 );
        }
        ctx->sect_para = NULL;
        break;
    case A_CELL:
        if ( text ) {
            /* the cells past what a row can have are joined to the last */
            ctx->cells++;
            rtf_par( ctx, (uint16_t)(PF_INTABLE | (ctx->cells < MAX_CELLS ? PF_CELLEND : 0)) );
        }
        break;
    case A_NESTCELL:
        if ( text ) {
            doc->lost |= LOST_TABLES;   /* a table in a cell: its text is kept */
            rtf_par( ctx, PF_INTABLE );
        }
        break;
    case A_ROW:
        if ( text ) {
            rtf_row_end( ctx );
        }
        break;
    case A_TROWD:
        mem_set( &ctx->row, 0, sizeof( TROW ) );
        ctx->row_cells = 0;
        break;
    case A_CELLX:
        if ( ctx->row_cells < MAX_CELLS ) {
            ctx->row.edge[ctx->row_cells] = rtf_twips( num );
        }
        ctx->row_cells++;
        break;
    case A_TRLEFT:
        ctx->row.left = rtf_twips( num );
        break;
    case A_TRHDR:
        ctx->row.header = 1;
        break;

    case A_CP:
        ctx->def_cp = word->arg;
        st->codepage = ctx->def_cp;
        break;
    case A_ANSICPG:
        if ( num > 0 && num < 65536 && cp_known( (int)num ) && num != CP_UTF8 ) {
            ctx->def_cp = (uint16_t)num;
            st->codepage = ctx->def_cp;
        }
        break;
    case A_DEFF:
        ctx->deff = num;
        break;
    case A_UC:
        st->uc = (uint8_t)rtf_clamp( num, 0, 255 );
        break;
    case A_U:
        rtf_char( ctx, (uint32_t)num & 0xFFFF, 0 );
        ctx->skip = st->uc;
        return;                         /* the stand-ins are still to come */
    case A_BIN:
        break;                          /* rtf_control has passed over the bytes */
    case A_DEFTAB:
        if ( num > 0 && num <= 31680 ) {
            doc->deftab = num;
        }
        break;
    case A_PAGE:
    case A_SPAGE:
        if ( word->action == A_SPAGE && ctx->sect != 0 ) {
            break;                      /* the first section's page is the document's */
        }
        num = rtf_clamp( num, 0, 31680 );
        switch ( word->arg ) {
        case 0: doc->page.width = num; break;
        case 1: doc->page.height = num; break;
        case 2: doc->page.left = num; break;
        case 3: doc->page.right = num; break;
        case 4: doc->page.top = num; break;
        case 5: doc->page.bottom = num; break;
        case 6: doc->page.header = num; break;
        case 7: doc->page.footer = num; break;
        }
        break;
    case A_LANDSCAPE:
        doc->page.landscape = 1;
        break;
    case A_SLAND:
        if ( ctx->sect == 0 ) {
            doc->page.landscape = 1;
        }
        break;
    case A_PGNSTARTS:
        if ( ctx->sect == 0 ) {
            ctx->pgn_starts = (uint32_t)rtf_clamp( num, 0, 0xFFFF );
        }
        break;
    case A_PGNRESTART:
        if ( ctx->sect == 0 ) {
            ctx->pgn_restart = 1;
        }
        break;
    case A_COLS:
        if ( num > 1 ) {
            doc->lost |= LOST_LAYOUT;
        }
        break;
    case A_FAM:
        if ( st->dest == D_FONTTBL ) {
            ctx->new_font.family = (uint8_t)word->arg;
        }
        break;
    case A_CHARSET:
        if ( st->dest == D_FONTTBL ) {
            ctx->new_font.charset = num;
            ctx->new_font.codepage = rtf_charset_cp( ctx, num );
            if ( ctx->new_font.codepage == 2 ) {
                ctx->new_font.family = FAM_SYMBOL;
            }
        }
        break;
    case A_FPRQ:
        if ( st->dest == D_FONTTBL ) {
            ctx->new_font.fixed = (int8_t)(num == 1 ? 1 : num == 2 ? 0 : -1);
        }
        break;
    case A_CPG:
        if ( st->dest == D_FONTTBL && num > 0 && num < 65536 && cp_known( (int)num ) && num != CP_UTF8 ) {
            ctx->new_font.codepage = (uint16_t)num;
        }
        break;
    case A_RGB:
        if ( st->dest == D_COLORTBL ) {
            ctx->rgb[word->arg] = (uint32_t)rtf_clamp( num, 0, 255 );
            ctx->rgb_have = 1;
        }
        break;
    case A_PICFMT:
        if ( st->dest == D_PICT ) {
            ctx->pict_fmt = word->arg;
        }
        break;
    case A_PICNUM:
        if ( st->dest == D_PICT ) {
            ctx->pic_num[word->arg] = num;
        }
        break;
    }
    ctx->skip = 0;
}

/* what follows a backslash */
static void rtf_control( RTF *ctx )
{
    const RWORD *word;
    char name[WORD_LEN];
    unsigned int len = 0, ch, digits = 0, count;
    int32_t num = 0;
    int has = 0, minus = 0, high, low;

    if ( ctx->pos >= ctx->len ) {
        return;
    }
    ch = ctx->data[ctx->pos];
    if ( !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) ) {
        ctx->pos++;
        switch ( ch ) {
        case '\'':
            if ( ctx->len - ctx->pos >= 2 ) {
                high = ctx->data[ctx->pos];
                low = ctx->data[ctx->pos + 1];
                high = high >= '0' && high <= '9' ? high - '0' : (high | 0x20) >= 'a' && (high | 0x20) <= 'f' ?
                       (high | 0x20) - 'a' + 10 : -1;
                low = low >= '0' && low <= '9' ? low - '0' : (low | 0x20) >= 'a' && (low | 0x20) <= 'f' ?
                      (low | 0x20) - 'a' + 10 : -1;
                if ( high >= 0 && low >= 0 ) {
                    ctx->pos += 2;
                    if ( ctx->skip ) {
                        ctx->skip--;
                    } else {
                        rtf_char( ctx, (uint32_t)(high * 16 + low), 1 );
                    }
                }
            }
            return;
        case '*':
            if ( !ctx->over_deep ) {
                ctx->st->star = 1;
            }
            return;
        case '~':
            rtf_char( ctx, WCH_NBSP, 0 );
            return;
        case '-':
            rtf_char( ctx, WCH_SHY, 0 );
            return;
        case '_':
            rtf_char( ctx, WCH_NBHYPHEN, 0 );
            return;
        case '{':
        case '}':
        case '\\':
            if ( ctx->skip ) {
                ctx->skip--;
            } else {
                rtf_char( ctx, ch, 0 );
            }
            return;
        case '\r':
        case '\n':
            if ( !ctx->over_deep ) {
                rtf_par( ctx, 0 );
            }
            return;
        }
        return;
    }
    /* a word: letters, then a number that may be negative, then one
       space that is not text */
    while ( ctx->pos < ctx->len ) {
        ch = ctx->data[ctx->pos];
        if ( !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) ) {
            break;
        }
        if ( len < WORD_LEN - 1 ) {
            name[len++] = (char)ch;
        } else {
            len = WORD_LEN;             /* too long to be any word that is known */
        }
        ctx->pos++;
    }
    if ( ctx->pos < ctx->len && ctx->data[ctx->pos] == '-' && ctx->pos + 1 < ctx->len &&
         ctx->data[ctx->pos + 1] >= '0' && ctx->data[ctx->pos + 1] <= '9' ) {
        minus = 1;
        ctx->pos++;
    }
    while ( ctx->pos < ctx->len && ctx->data[ctx->pos] >= '0' && ctx->data[ctx->pos] <= '9' ) {
        if ( digits < 9 ) {
            num = num * 10 + (int32_t)(ctx->data[ctx->pos] - '0');
            digits++;
        }
        has = 1;
        ctx->pos++;
    }
    if ( minus ) {
        num = -num;
    }
    if ( ctx->pos < ctx->len && ctx->data[ctx->pos] == ' ' ) {
        ctx->pos++;
    }
    word = len < WORD_LEN ? rtf_lookup( ctx, name, len ) : NULL;
    /* bytes that are not text at all: passed over whatever they are in */
    if ( word != NULL && word->action == A_BIN && num > 0 ) {
        count = (unsigned int)num < ctx->len - ctx->pos ? (unsigned int)num : ctx->len - ctx->pos;
        if ( !ctx->over_deep && ctx->st->dest == D_PICT ) {
            while ( count-- ) {
                rtf_pict_byte( ctx, ctx->data[ctx->pos++] );
            }
        } else {
            ctx->pos += count;
        }
        return;
    }
    if ( ctx->over_deep ) {
        return;
    }
    if ( word == NULL ) {
        if ( ctx->st->star ) {
            ctx->st->dest = D_SKIP;     /* a destination nobody here knows */
        }
        ctx->st->star = 0;
        ctx->skip = 0;
        return;
    }
    ctx->st->star = 0;
    rtf_word( ctx, word, has, num );
}

/* ------------------------------------------------------------------ */
/* detect and load                                                     */
/* ------------------------------------------------------------------ */

static int rtf_detect( const uint8_t *data, uint32_t len, const char *ext )
{
    unsigned int pos = 0;

    (void)ext;
    if ( len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ) {
        pos = 3;
    }
    while ( pos < len && pos < 64 && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' ||
                                     data[pos] == '\n') ) {
        pos++;
    }
    return len - pos >= 5 && mem_cmp( data + pos, "{\\rtf", 5 ) == 0 ? 100 : 0;
}

static void rtf_free( RTF *ctx )
{
    xfree( ctx->stack );
    xfree( ctx->hash );
    xfree( ctx->font );
    xfree( ctx->font_index );
    xfree( ctx->color );
    xfree( ctx->style );
    xfree( ctx->pict );
    xfree( ctx->list );
    xfree( ctx->over );
    xfree( ctx );
}

static int rtf_load( const uint8_t *data, uint32_t len, WDOC *doc )
{
    RTF *ctx;
    RSTATE *st;
    uint32_t ch;

    ctx = (RTF *)try_alloc( sizeof( RTF ) );
    if ( ctx == NULL ) {
        return WE_NOMEM;
    }
    mem_set( ctx, 0, sizeof( RTF ) );
    ctx->doc = doc;
    ctx->data = data;
    ctx->len = len;
    ctx->def_cp = CP_1252;
    ctx->cap = 16;
    ctx->stack = (RSTATE *)try_alloc( ctx->cap * sizeof( RSTATE ) );
    if ( ctx->stack == NULL || !rtf_hash_build( ctx ) ) {
        rtf_free( ctx );
        return WE_NOMEM;
    }
    st = ctx->st = &ctx->stack[0];
    mem_set( st, 0, sizeof( RSTATE ) );
    st->dest = D_BODY;
    st->uc = 1;
    st->codepage = CP_1252;
    rtf_pard( st );
    rtf_plain( ctx, st );
    wb_init( &ctx->body, doc, &doc->body );
    ctx->wb = &ctx->body;

    while ( ctx->pos < ctx->len && !doc->failed ) {
        ch = ctx->data[ctx->pos++];
        if ( ch == '{' ) {
            rtf_push( ctx );
        } else if ( ch == '}' ) {
            rtf_pop( ctx );
        } else if ( ch == '\\' ) {
            rtf_control( ctx );
        } else if ( ch == '\r' || ch == '\n' || ch == 0 ) {
            continue;
        } else if ( ctx->skip ) {
            ctx->skip--;
        } else {
            rtf_char( ctx, ch, 1 );
        }
    }
    /* a file cut short: what was open is closed as it stands */
    if ( ctx->in_sub ) {
        rtf_sub_end( ctx, ctx->st );
    }
    if ( ctx->body.para != NULL ) {
        rtf_set_pap( ctx, ctx->st, 0 );
    }
    wb_done( &ctx->body );
    doc->page.first_page = (uint16_t)(ctx->pgn_restart && ctx->pgn_starts ? ctx->pgn_starts : 1);
    rtf_free( ctx );
    return wd_failed( doc ) ? WE_NOMEM : WE_OK;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

/* a list as the file has it: nine levels, of which the document has
   said what one or two are */
typedef struct {
    uint8_t  kind[MAX_LEVELS];  /* LIST_*, FFh where nothing has said */
    uint16_t start[MAX_LEVELS];
} WLIST;

typedef struct {
    WDOC        *doc;
    OUT         *out;
    uint32_t    *color;         /* the color table: [0] is "automatic" */
    unsigned int ncolor, color_cap;
    WLIST       *list;
    unsigned int nlist, list_cap;
    unsigned int cur[MAX_LEVELS]; /* the list each level is in, + 1 */
    unsigned int replay;        /* the second time through: lists met so far */
    int          second;
    unsigned int symbol;        /* the font the bullets are in */
    int          endnotes;
    int          failed;
} RTFW;

/*
 * The list a paragraph is in -> its number in the file.  A paragraph
 * that starts the numbers again, or is of another kind than the list
 * its level was in, begins a new one.  The lists are made in a first
 * pass over the paragraphs, because the list table comes before the
 * text; the second pass meets the same paragraphs in the same order
 * and so counts its way through the same lists.
 */
static unsigned int rtfw_list( RTFW *w, const PAP *pap )
{
    WLIST *list;
    unsigned int want, level = pap->level < MAX_LEVELS ? pap->level : MAX_LEVELS - 1;

    if ( pap->list_start == 0 && w->cur[level] != 0 && w->list[w->cur[level] - 1].kind[level] == pap->list ) {
        return w->cur[level];
    }
    if ( w->second ) {
        if ( w->replay < w->nlist ) {
            w->replay++;
        }
        w->cur[level] = w->replay;
        return w->replay;
    }
    if ( w->nlist == w->list_cap ) {
        want = w->list_cap ? w->list_cap * 2 : 16;
        list = want > 0x00010000UL ? NULL : (WLIST *)try_realloc( w->list, want * sizeof( WLIST ) );
        if ( list == NULL ) {
            w->failed = 1;
            return w->nlist ? w->nlist : 1;
        }
        w->list = list;
        w->list_cap = want;
    }
    list = &w->list[w->nlist++];
    mem_set( list->kind, 0xFF, MAX_LEVELS );
    mem_set( list->start, 0, sizeof( list->start ) );
    list->kind[level] = pap->list;
    list->start[level] = (uint16_t)(pap->list_start ? pap->list_start : 1);
    w->cur[level] = w->nlist;
    return w->nlist;
}

static int rtfw_is_list( const PAP *pap )
{
    return pap->list != LIST_NONE && pap->list <= LIST_ROMAN_UPPER;
}

/* the first pass: the lists of a story, and of the notes called from it */
static void rtfw_lists_story( RTFW *w, STORY *story, int depth )
{
    WPARA *para;
    WOBJ *obj;
    unsigned int index, anch;

    for ( index = 0; index < story->count; index++ ) {
        para = story->para[index];
        if ( rtfw_is_list( &para->pap ) ) {
            rtfw_list( w, &para->pap );
        }
        for ( anch = 0; depth == 0 && anch < para->nanch; anch++ ) {
            if ( para->anch[anch].obj < w->doc->nobj ) {
                obj = &w->doc->obj[para->anch[anch].obj];
                if ( obj->kind == OBJ_NOTE && obj->story != NULL ) {
                    if ( obj->fmt == NOTE_END ) {
                        w->endnotes = 1;
                    }
                    rtfw_lists_story( w, obj->story, 1 );
                }
            }
        }
    }
}

/* a color's place in the color table, added if it is new */
static unsigned int rtfw_color( RTFW *w, uint32_t color )
{
    uint32_t *fresh;
    unsigned int index, want;

    if ( color == COLOR_AUTO ) {
        return 0;
    }
    for ( index = 1; index < w->ncolor; index++ ) {
        if ( w->color[index] == color ) {
            return index;
        }
    }
    if ( w->second ) {
        return 0;                       /* the table is written: it has them all */
    }
    if ( w->ncolor == w->color_cap ) {
        want = w->color_cap ? w->color_cap * 2 : 32;
        fresh = want > 0x00010000UL ? NULL : (uint32_t *)try_realloc( w->color, want * sizeof( uint32_t ) );
        if ( fresh == NULL ) {
            w->failed = 1;
            return 0;
        }
        w->color = fresh;
        w->color_cap = want;
    }
    if ( w->ncolor == 0 ) {
        w->color[w->ncolor++] = COLOR_AUTO;
    }
    w->color[w->ncolor] = color;
    return w->ncolor++;
}

/* one unit of the document's text */
static void rtfw_unit( RTFW *w, uint32_t unit )
{
    OUT *out = w->out;
    int byte;

    if ( unit == '\\' || unit == '{' || unit == '}' ) {
        out_byte( out, '\\' );
        out_byte( out, (int)unit );
    } else if ( unit >= 0x20 && unit < 0x80 ) {
        out_byte( out, (int)unit );
    } else if ( unit == WCH_TAB ) {
        out_str( out, "\\tab " );
    } else if ( unit == WCH_LINE ) {
        out_str( out, "\\line " );
    } else if ( unit == WCH_PAGE ) {
        out_str( out, "\\page " );
    } else if ( unit == WCH_NBSP ) {
        out_str( out, "\\~" );
    } else if ( unit == WCH_SHY ) {
        out_str( out, "\\-" );
    } else if ( unit == WCH_NBHYPHEN ) {
        out_str( out, "\\_" );
    } else if ( unit >= 0x80 ) {
        /* as code page 1252 has it, or by its number with a stand-in
           for a reader that does not know numbers */
        byte = cp_from_uni( CP_1252, unit );
        if ( byte >= 0x80 ) {
            out_fmt( out, "\\'%02x", (uint32_t)byte );
        } else {
            out_fmt( out, "\\u%d?", unit > 32767 ? (int32_t)unit - 65536 : (int32_t)unit );
        }
    }
}

/* a name or a line of the summary: UTF-8, with nothing that ends it early */
static void rtfw_utf8( RTFW *w, const char *text )
{
    const uint8_t *cur = (const uint8_t *)text, *end = cur + str_len( text );
    uint32_t code;

    while ( cur < end ) {
        code = utf8_get( &cur, end );
        if ( code == ';' ) {
            code = ',';
        }
        if ( code >= 0x20 && code <= 0xFFFF ) {
            rtfw_unit( w, code );
        }
    }
}

/* a run's look: every property, since \plain has taken them all away */
static void rtfw_chp( RTFW *w, const CHP *chp )
{
    OUT *out = w->out;

    out_fmt( out, "\\f%u\\fs%u", (uint32_t)(chp->font < w->doc->nfont ? chp->font : 0), (uint32_t)chp->size );
    if ( chp->flags & CF_BOLD ) {
        out_str( out, "\\b" );
    }
    if ( chp->flags & CF_ITALIC ) {
        out_str( out, "\\i" );
    }
    if ( chp->flags & CF_DUNDER ) {
        out_str( out, "\\uldb" );
    } else if ( chp->flags & CF_WUNDER ) {
        out_str( out, "\\ulw" );
    } else if ( chp->flags & CF_UNDER ) {
        out_str( out, "\\ul" );
    }
    if ( chp->flags & CF_STRIKE ) {
        out_str( out, "\\strike" );
    }
    if ( chp->flags & CF_SUPER ) {
        out_str( out, "\\super" );
    } else if ( chp->flags & CF_SUB ) {
        out_str( out, "\\sub" );
    }
    if ( chp->flags & CF_HIDDEN ) {
        out_str( out, "\\v" );
    }
    if ( chp->flags & CF_SMALLCAPS ) {
        out_str( out, "\\scaps" );
    }
    if ( chp->flags & CF_ALLCAPS ) {
        out_str( out, "\\caps" );
    }
    if ( chp->flags & CF_OUTLINE ) {
        out_str( out, "\\outl" );
    }
    if ( chp->flags & CF_SHADOW ) {
        out_str( out, "\\shad" );
    }
    if ( chp->color != COLOR_AUTO ) {
        out_fmt( out, "\\cf%u", rtfw_color( w, chp->color ) );
    }
    if ( chp->hilite >= 1 && chp->hilite <= 16 ) {
        out_fmt( out, "\\highlight%u", rtfw_color( w, rtf_ico[chp->hilite] ) );
    }
    out_byte( out, ' ' );
}

/* a paragraph's look; "list" is the list it is in, 0 for none */
static void rtfw_pap( RTFW *w, const PAP *pap, int intbl, unsigned int list )
{
    static const char *const aligns[] = { "\\ql", "\\qc", "\\qr", "\\qj" };
    static const char *const kinds[] = { "", "\\tqc", "\\tqr", "\\tqdec" };
    static const char *const leads[] = { "", "\\tldot", "\\tlhyph", "\\tlul" };
    static const struct {
        uint16_t flag;
        const char *word;
    } sides[] = {
        { PF_BORDER_TOP, "\\brdrt" }, { PF_BORDER_BOT, "\\brdrb" }, { PF_BORDER_LEFT, "\\brdrl" },
        { PF_BORDER_RIGHT, "\\brdrr" }
    };
    OUT *out = w->out;
    unsigned int index;

    if ( pap->style != 0 && pap->style < w->doc->nstyle ) {
        out_fmt( out, "\\s%u", (uint32_t)pap->style );
    }
    out_str( out, aligns[pap->align <= AL_JUSTIFY ? pap->align : 0] );
    if ( pap->left || pap->right || pap->first ) {
        out_fmt( out, "\\li%d\\ri%d\\fi%d", pap->left, pap->right, pap->first );
    }
    if ( pap->before ) {
        out_fmt( out, "\\sb%u", (uint32_t)pap->before );
    }
    if ( pap->after ) {
        out_fmt( out, "\\sa%u", (uint32_t)pap->after );
    }
    if ( pap->line_rule == LS_MULTIPLE ) {
        if ( pap->line != 240 && pap->line != 0 ) {
            out_fmt( out, "\\sl%u\\slmult1", (uint32_t)pap->line );
        }
    } else if ( pap->line != 0 ) {
        out_fmt( out, pap->line_rule == LS_EXACT ? "\\sl-%u\\slmult0" : "\\sl%u\\slmult0", (uint32_t)pap->line );
    }
    if ( pap->flags & PF_KEEP ) {
        out_str( out, "\\keep" );
    }
    if ( pap->flags & PF_KEEPNEXT ) {
        out_str( out, "\\keepn" );
    }
    if ( pap->flags & PF_PAGEBEFORE ) {
        out_str( out, "\\pagebb" );
    }
    for ( index = 0; pap->tabs != NULL && index < pap->ntabs; index++ ) {
        out_fmt( out, "%s%s\\tx%d", kinds[pap->tabs[index].kind & 3], leads[pap->tabs[index].lead & 3],
                 pap->tabs[index].pos );
    }
    for ( index = 0; index < sizeof( sides ) / sizeof( sides[0] ); index++ ) {
        if ( pap->flags & sides[index].flag ) {
            out_fmt( out, "%s\\brdrs\\brdrw10\\brsp20", sides[index].word );
        }
    }
    if ( intbl ) {
        out_str( out, "\\intbl" );
    }
    if ( list ) {
        out_fmt( out, "\\ls%u\\ilvl%u", list, (uint32_t)(pap->level < MAX_LEVELS ? pap->level : MAX_LEVELS - 1) );
    }
}

/* a picture's size in dots, where its file says it in a place easy to find */
static void rtfw_pixels( const WOBJ *obj, uint32_t *wide, uint32_t *high )
{
    const uint8_t *data = obj->data;
    uint32_t len = obj->len, pos = 2, mark, seg;

    *wide = *high = 0;
    if ( obj->fmt == IMG_PNG && len >= 24 ) {
        *wide = BE32( data + 16 );
        *high = BE32( data + 20 );
    } else if ( obj->fmt == IMG_JPEG ) {
        /* the frame header is the first segment numbered C0h to CFh
           that is not a table */
        while ( IN_RANGE( pos, 9, len ) && data[pos] == 0xFF ) {
            mark = data[pos + 1];
            if ( mark >= 0xC0 && mark <= 0xCF && mark != 0xC4 && mark != 0xC8 && mark != 0xCC ) {
                *high = BE16( data + pos + 5 );
                *wide = BE16( data + pos + 7 );
                break;
            }
            seg = BE16( data + pos + 2 );
            if ( seg < 2 ) {
                break;
            }
            pos += 2 + seg;
        }
    }
    if ( *wide > 32767 || *high > 32767 ) {
        *wide = *high = 0;
    }
}

static void rtfw_image( RTFW *w, const WOBJ *obj )
{
    static const char hex[] = "0123456789abcdef";
    char line[130];
    OUT *out = w->out;
    const uint8_t *data = obj->data;
    const char *kind;
    uint32_t len = obj->len, wide, high, pos;
    unsigned int used;
    int width = obj->width, height = obj->height;

    switch ( obj->fmt ) {
    case IMG_PNG:   kind = "\\pngblip"; break;
    case IMG_JPEG:  kind = "\\jpegblip"; break;
    case IMG_EMF:   kind = "\\emfblip"; break;
    case IMG_WMF:   kind = "\\wmetafile8"; break;
    case IMG_PICT:  kind = "\\macpict"; break;
    case IMG_DIB:   kind = "\\dibitmap0"; break;
    case IMG_BMP:
        /* a bitmap file is 14 bytes about the file and then what RTF
           calls a device-independent bitmap */
        if ( len <= 14 ) {
            return;
        }
        kind = "\\dibitmap0";
        data += 14;
        len -= 14;
        break;
    default:
        return;                         /* a kind RTF has no word for */
    }
    if ( data == NULL || len == 0 ) {
        return;
    }
    width = width < 1 ? 1 : width > 31680 ? 31680 : width;
    height = height < 1 ? 1 : height > 31680 ? 31680 : height;
    rtfw_pixels( obj, &wide, &high );
    if ( obj->fmt == IMG_WMF ) {
        /* a metafile's own units: hundredths of a millimeter */
        wide = (uint32_t)width * 127 / 72;
        high = (uint32_t)height * 127 / 72;
    }
    out_fmt( out, "{\\pict%s", kind );
    if ( wide && high ) {
        out_fmt( out, "\\picw%u\\pich%u", wide, high );
    }
    out_fmt( out, "\\picwgoal%d\\pichgoal%d\r\n", width, height );
    for ( pos = 0; pos < len; ) {
        for ( used = 0; used < 128 && pos < len; pos++ ) {
            line[used++] = hex[data[pos] >> 4];
            line[used++] = hex[data[pos] & 15];
        }
        line[used++] = '\r';
        line[used++] = '\n';
        out_put( out, line, used );
    }
    out_str( out, "}" );
}

static void rtfw_story( RTFW *w, STORY *story, int kind );

/* a paragraph's start: its properties, and the look of its mark */
static void rtfw_para_start( RTFW *w, WPARA *para, int intbl )
{
    WDOC *doc = w->doc;
    const CHP *chp;
    unsigned int list = 0;

    if ( rtfw_is_list( &para->pap ) ) {
        list = rtfw_list( w, &para->pap );
    }
    out_str( w->out, "\\pard\\plain " );
    rtfw_pap( w, &para->pap, intbl, list );
    chp = para->len ? wd_chp_get( doc, para->attr[para->len - 1] )
                    : &doc->style[para->pap.style < doc->nstyle ? para->pap.style : 0].chp;
    rtfw_chp( w, chp );
}

/* a paragraph's text: each run in a group of its own.  "depth" is 0
   in the main text, where notes are called from. */
static void rtfw_para_text( RTFW *w, WPARA *para, int depth )
{
    WDOC *doc = w->doc;
    OUT *out = w->out;
    WOBJ *obj;
    unsigned int off, run, index;
    uint32_t unit;
    int which;

    for ( off = 0; off < para->len; off += run ) {
        run = wd_run( para, off );
        if ( run == 0 ) {
            break;
        }
        /* \plain first: what the paragraph's mark was given must not
           show through where the run says nothing */
        out_str( out, "{\\plain" );
        rtfw_chp( w, wd_chp_get( doc, para->attr[off] ) );
        if ( para->text[off] == WCH_OBJ ) {
            which = wd_obj_at( para, off );
            obj = which >= 0 && (unsigned int)which < doc->nobj ? &doc->obj[which] : NULL;
            if ( obj != NULL && obj->kind == OBJ_IMAGE ) {
                rtfw_image( w, obj );
            } else if ( obj != NULL && obj->kind == OBJ_NOTE && obj->story != NULL && depth == 0 ) {
                out_fmt( out, "\\chftn {\\footnote %s", obj->fmt == NOTE_END ? "\\ftnalt " : "" );
                rtfw_story( w, obj->story, 2 );
                out_byte( out, '}' );
            }
        } else {
            for ( index = 0; index < run; index++ ) {
                unit = para->text[off + index];
                if ( WCH_IS_FIELD( unit ) ) {
                    out_fmt( out, "{\\field{\\*\\fldinst  %s }{\\fldrslt %s}}",
                             unit == WCH_PAGENUM ? "PAGE" : unit == WCH_NUMPAGES ? "NUMPAGES" :
                             unit == WCH_DATE ? "DATE" : unit == WCH_TIME ? "TIME" : "FILENAME",
                             unit == WCH_PAGENUM || unit == WCH_NUMPAGES ? "1" : "" );
                } else {
                    rtfw_unit( w, unit );
                }
            }
        }
        out_byte( out, '}' );
    }
}

/* the last paragraph of the row that paragraph "index" is in */
static unsigned int rtfw_row_last( const STORY *story, unsigned int index )
{
    while ( index + 1 < story->count && !(story->para[index]->pap.flags & PF_ROWEND) &&
            (story->para[index + 1]->pap.flags & PF_INTABLE) ) {
        index++;
    }
    return index;
}

/* a row's cells, which the file wants said before the row's text */
static void rtfw_row_def( RTFW *w, const STORY *story, unsigned int first, unsigned int last )
{
    OUT *out = w->out;
    const TROW *row = story->para[last]->pap.row;
    unsigned int cells = 0, index, have;
    int edge = 0;

    for ( index = first; index <= last; index++ ) {
        if ( (story->para[index]->pap.flags & PF_CELLEND) || index == last ) {
            cells++;
        }
    }
    have = row != NULL ? (row->ncells <= MAX_CELLS ? row->ncells : MAX_CELLS) : 0;
    out_fmt( out, "\\trowd\\trgaph108\\trleft%d", row != NULL ? row->left : 0 );
    if ( row != NULL && row->header ) {
        out_str( out, "\\trhdr" );
    }
    if ( row != NULL ) {
        edge = row->left;
    }
    for ( index = 0; index < cells; index++ ) {
        if ( row != NULL && row->borders ) {
            out_str( out, "\\clbrdrt\\brdrs\\brdrw10\\clbrdrl\\brdrs\\brdrw10\\clbrdrb\\brdrs\\brdrw10"
                          "\\clbrdrr\\brdrs\\brdrw10" );
        }
        /* a cell the row does not place: the width between the margins
           shared out, or one more of half an inch */
        edge = index < have ? row->edge[index] : have ? edge + 720 : (int)(9000 * (index + 1) / cells);
        out_fmt( out, "\\cellx%d", edge );
    }
    out_str( out, "\r\n" );
}

/* a story: 0 the main text, 1 a header or footer, 2 a note */
static void rtfw_story( RTFW *w, STORY *story, int kind )
{
    OUT *out = w->out;
    WPARA *para;
    unsigned int index = 0, last;

    while ( index < story->count ) {
        para = story->para[index];
        if ( para->pap.flags & PF_INTABLE ) {
            last = rtfw_row_last( story, index );
            rtfw_row_def( w, story, index, last );
            for ( ; index <= last; index++ ) {
                para = story->para[index];
                rtfw_para_start( w, para, 1 );
                rtfw_para_text( w, para, kind == 0 ? 0 : 1 );
                out_str( out, (para->pap.flags & PF_CELLEND) || index == last ? "\\cell\r\n" : "\\par\r\n" );
            }
            out_str( out, "\\row\r\n" );
            continue;
        }
        rtfw_para_start( w, para, 0 );
        if ( kind == 2 && index == 0 ) {
            out_str( out, "{\\super \\chftn}" );
        }
        rtfw_para_text( w, para, kind == 0 ? 0 : 1 );
        /* a note's last paragraph ends with the note */
        if ( kind != 2 || index + 1 < story->count ) {
            out_str( out, "\\par\r\n" );
        }
        index++;
    }
}

/* the name Word writes for a style of its own */
static const char *rtfw_style_name( const WSTYLE *style )
{
    static const char *const names[] = {
        "Normal", "heading 1", "heading 2", "heading 3", "heading 4", "heading 5", "heading 6",
        "Title", "Subtitle", "header", "footer", "footnote text"
    };

    return style->kind < STY_OTHER ? names[style->kind] : style->name;
}

static void rtfw_list_table( RTFW *w )
{
    static const uint8_t nfc[] = { 0, 23, 0, 4, 3, 2, 1 };  /* by LIST_* */
    OUT *out = w->out;
    WLIST *list;
    unsigned int index, level, kind, first;

    out_str( out, "{\\*\\listtable" );
    for ( index = 0; index < w->nlist; index++ ) {
        list = &w->list[index];
        first = LIST_NUMBER;
        for ( level = MAX_LEVELS; level > 0; level-- ) {
            if ( list->kind[level - 1] != 0xFF ) {
                first = list->kind[level - 1];
            }
        }
        out_fmt( out, "\r\n{\\list\\listtemplateid%u\\listhybrid", index + 1 );
        for ( level = 0; level < MAX_LEVELS; level++ ) {
            kind = list->kind[level] != 0xFF ? list->kind[level] : first;
            if ( kind > LIST_ROMAN_UPPER ) {
                kind = LIST_NUMBER;
            }
            out_fmt( out, "{\\listlevel\\levelnfc%u\\levelnfcn%u\\leveljc0\\leveljcn0\\levelfollow0"
                          "\\levelstartat%u\\levelspace0\\levelindent0", (uint32_t)nfc[kind], (uint32_t)nfc[kind],
                     (uint32_t)(list->start[level] ? list->start[level] : 1) );
            if ( kind == LIST_BULLET ) {
                out_fmt( out, "{\\leveltext\\'01\\u-3913 ?;}{\\levelnumbers;}\\f%u", w->symbol );
            } else {
                out_fmt( out, "{\\leveltext\\'02\\'0%u.;}{\\levelnumbers\\'01;}", level );
            }
            out_fmt( out, "\\fi-360\\li%u }", 720 * (level + 1) );
        }
        out_fmt( out, "{\\listname ;}\\listid%u}", index + 1 );
    }
    out_str( out, "}\r\n{\\*\\listoverridetable" );
    for ( index = 0; index < w->nlist; index++ ) {
        out_fmt( out, "{\\listoverride\\listid%u\\listoverridecount0\\ls%u}", index + 1, index + 1 );
    }
    out_str( out, "}\r\n" );
}

static void rtfw_info( RTFW *w, const char *word, const char *text )
{
    if ( text[0] ) {
        out_fmt( w->out, "{\\%s ", word );
        rtfw_utf8( w, text );
        out_byte( w->out, '}' );
    }
}

static int rtf_save( WDOC *doc, OUT *out, int variant )
{
    static const char *const families[] = {
        "\\fnil", "\\froman", "\\fswiss", "\\fmodern", "\\fscript", "\\fdecor", "\\ftech"
    };
    RTFW *w;
    const WFONT *font;
    const WSTYLE *style;
    unsigned int index;
    int failed;

    (void)variant;
    w = (RTFW *)try_alloc( sizeof( RTFW ) );
    if ( w == NULL ) {
        return WE_NOMEM;
    }
    mem_set( w, 0, sizeof( RTFW ) );
    w->doc = doc;
    w->out = out;

    /* what the tables in front of the text have to hold: the colors
       in use and the lists */
    rtfw_color( w, 0 );
    for ( index = 0; index < doc->nchp; index++ ) {
        rtfw_color( w, doc->chp[index].color );
        if ( doc->chp[index].hilite >= 1 && doc->chp[index].hilite <= 16 ) {
            rtfw_color( w, rtf_ico[doc->chp[index].hilite] );
        }
    }
    for ( index = 0; index < doc->nstyle; index++ ) {
        rtfw_color( w, doc->style[index].chp.color );
    }
    rtfw_lists_story( w, &doc->header, 1 );
    rtfw_lists_story( w, &doc->footer, 1 );
    rtfw_lists_story( w, &doc->body, 0 );
    w->symbol = doc->nfont;
    for ( index = 0; index < doc->nfont; index++ ) {
        if ( str_icmp( doc->font[index].name, "Symbol" ) == 0 ) {
            w->symbol = index;
        }
    }
    w->second = 1;
    mem_set( w->cur, 0, sizeof( w->cur ) );

    out_str( out, "{\\rtf1\\ansi\\ansicpg1252\\uc1\\deff0\r\n{\\fonttbl" );
    for ( index = 0; index < doc->nfont; index++ ) {
        font = &doc->font[index];
        out_fmt( out, "{\\f%u%s\\fcharset%u\\fprq%u ", index, families[font->family <= FAM_SYMBOL ? font->family : 0],
                 font->family == FAM_SYMBOL ? 2UL : 0UL, font->fixed ? 1UL : 2UL );
        rtfw_utf8( w, font->name );
        out_str( out, ";}" );
    }
    if ( w->symbol == doc->nfont && w->nlist ) {
        out_fmt( out, "{\\f%u\\ftech\\fcharset2\\fprq2 Symbol;}", w->symbol );
    }
    out_str( out, "}\r\n{\\colortbl;" );
    for ( index = 1; index < w->ncolor; index++ ) {
        out_fmt( out, "\\red%u\\green%u\\blue%u;", (w->color[index] >> 16) & 0xFF, (w->color[index] >> 8) & 0xFF,
                 w->color[index] & 0xFF );
    }
    out_str( out, "}\r\n{\\stylesheet" );
    for ( index = 0; index < doc->nstyle; index++ ) {
        style = &doc->style[index];
        out_byte( out, '{' );
        if ( index ) {
            out_fmt( out, "\\s%u", index );
        }
        rtfw_pap( w, &style->pap, 0, 0 );
        rtfw_chp( w, &style->chp );
        out_fmt( out, index ? "\\sbasedon0\\snext%u " : "\\snext%u ", index );
        rtfw_utf8( w, rtfw_style_name( style ) );
        out_str( out, ";}\r\n" );
    }
    out_str( out, "}\r\n" );
    if ( w->nlist ) {
        rtfw_list_table( w );
    }
    out_str( out, "{\\info" );
    rtfw_info( w, "title", doc->title );
    rtfw_info( w, "subject", doc->subject );
    rtfw_info( w, "author", doc->author );
    rtfw_info( w, "keywords", doc->keywords );
    out_str( out, "}\r\n" );

    /* the page: for the document, and again for its one section.
       Footnotes are said to go at the foot of the page: RTF's own
       choice, when nothing is said, is the end of the section, and
       Word then takes every footnote for an endnote. */
    out_fmt( out, "\\paperw%d\\paperh%d\\margl%d\\margr%d\\margt%d\\margb%d\\deftab%d%s\\ftnbj\\aenddoc%s\r\n",
             doc->page.width, doc->page.height, doc->page.left, doc->page.right, doc->page.top,
             doc->page.bottom, doc->deftab, doc->page.landscape ? "\\landscape" : "",
             w->endnotes ? "\\fet2" : "" );
    out_fmt( out, "\\sectd\\pgwsxn%d\\pghsxn%d\\marglsxn%d\\margrsxn%d\\margtsxn%d\\margbsxn%d\\headery%d\\footery%d%s",
             doc->page.width, doc->page.height, doc->page.left, doc->page.right, doc->page.top,
             doc->page.bottom, doc->page.header, doc->page.footer, doc->page.landscape ? "\\lndscpsxn" : "" );
    if ( doc->page.first_page != 1 ) {
        out_fmt( out, "\\pgnrestart\\pgnstarts%u", (uint32_t)doc->page.first_page );
    }
    out_str( out, "\r\n" );
    if ( doc->header.count ) {
        out_str( out, "{\\header " );
        rtfw_story( w, &doc->header, 1 );
        out_str( out, "}\r\n" );
    }
    if ( doc->footer.count ) {
        out_str( out, "{\\footer " );
        rtfw_story( w, &doc->footer, 1 );
        out_str( out, "}\r\n" );
    }
    rtfw_story( w, &doc->body, 0 );
    out_str( out, "}\r\n" );

    failed = w->failed;
    xfree( w->color );
    xfree( w->list );
    xfree( w );
    if ( failed ) {
        return WE_NOMEM;
    }
    return out_error( out ) ? WE_IO : WE_OK;
}

const FORMAT fmt_rtf = {
    FMT_RTF, "Rich Text Format", "RTF", rtf_detect, rtf_load, rtf_save, NULL, 0
};
