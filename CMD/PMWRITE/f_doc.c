/*
 * F_DOC.C - Microsoft Word's binary .DOC: Word 6.0 and 95, and Word 97
 * to 2003.
 *
 * THE FILE is an OLE2 compound file (CFB.C).  Its "WordDocument"
 * stream starts with the FIB, a table of where everything else is, and
 * holds the text.  Word 97 keeps the other tables in a second stream,
 * "0Table" or "1Table", and pictures in "Data"; Word 6.0 and 95 keep
 * all of it in WordDocument.  The two generations differ in little
 * else that matters here: where the FIB's entries are, the width of a
 * few fields, and the numbers of the property codes (one byte each in
 * Word 6, two in Word 97).  So there is ONE reader, which turns either
 * generation's codes into Word 97's as it meets them.
 *
 * THE TEXT is found through the piece table: a list that says which
 * run of character positions is at which place in the file, and
 * whether it is in 8 bits (code page 1252) or 16.  A file Word saved
 * quickly has many pieces, in any order; one saved whole has one.
 *
 * PROPERTIES hang on FILE OFFSETS, not on character positions.  For a
 * character at an offset, the "bin table" says which 512-byte page
 * (FKP) covers that offset, and the page says where the run of like
 * characters starts and ends and what changes it makes to the style -
 * a list of codes (sprms) with their values.  Paragraphs are the same,
 * and a paragraph's properties are the ones filed under the offset of
 * its END mark; so this reader works a paragraph at a time: it asks
 * where the paragraph that holds a character ends, takes that mark's
 * properties, and then goes through the characters up to it.
 *
 * WHAT IS KEPT is what WDOC.H can hold; the rest is dropped and said
 * in "lost".  Shapes that float (a text box, a picture with text
 * round it) are not read: only pictures in the line of text.  A row's
 * mark makes no paragraph of its own: it puts PF_ROWEND and the row's
 * cell edges on the paragraph before it.
 *
 * NOTHING IN THE FILE IS TRUSTED.  Every offset and count is checked
 * against the stream it points into before it is used; chains of
 * styles are followed sixteen deep and no further; fields nest
 * sixteen deep and no further.
 */
#include "fmt.h"
#include "cfb.h"
#include "flate.h"

#define NO_FC           0xFFFFFFFFUL
#define FKP_LEN         512
#define STYLE_DEPTH     16
#define FIELD_DEPTH     16
#define FIELD_CODE      24
#define MAX_INFLATE     0x02000000UL    /* a packed picture grows to 32M at most */
#define MAX_PARA_FAILS  64
#define MAX_LISTS       2047            /* a paragraph's list number has eleven bits */

/* the FIB's (offset, length) pairs, by their place in Word 97's list */
#define FIB_STSHF       1
#define FIB_FNDREF      2
#define FIB_FNDTXT      3
#define FIB_SED         6
#define FIB_HDD         11
#define FIB_BTECHPX     12
#define FIB_BTEPAPX     13
#define FIB_FFN         15
#define FIB_FLDMOM      16
#define FIB_FLDHDR      17
#define FIB_FLDFTN      18
#define FIB_DOP         31
#define FIB_CLX         33
#define FIB_ENDREF      46
#define FIB_ENDTXT      47
#define FIB_FLDEDN      48
#define FIB_PLFLST      73
#define FIB_PLFLFO      74

/* Word 97's codes for the properties that are read.  Word 6's are
   turned into these (w6_map). */
#define SPRM_CFRMARKDEL 0x0800
#define SPRM_CFRMARK    0x0801
#define SPRM_CPICLOC    0x6A03
#define SPRM_CFDATA     0x0806
#define SPRM_CSYMBOL    0x6A09
#define SPRM_CHIGHLIGHT 0x2A0C
#define SPRM_CISTD      0x4A30
#define SPRM_CPLAIN     0x2A33
#define SPRM_CFBOLD     0x0835
#define SPRM_CFITALIC   0x0836
#define SPRM_CFSTRIKE   0x0837
#define SPRM_CFOUTLINE  0x0838
#define SPRM_CFSHADOW   0x0839
#define SPRM_CFSMALLCAPS 0x083A
#define SPRM_CFCAPS     0x083B
#define SPRM_CFVANISH   0x083C
#define SPRM_CKUL       0x2A3E
#define SPRM_CICO       0x2A42
#define SPRM_CHPS       0x4A43
#define SPRM_CISS       0x2A48
#define SPRM_CRGFTC0    0x4A4F
#define SPRM_CFDSTRIKE  0x2A53
#define SPRM_CFSPEC     0x0855
#define SPRM_CFOBJ      0x0856
#define SPRM_CCV        0x6870
#define SPRM_PISTD      0x4600
#define SPRM_PJC80      0x2403
#define SPRM_PFKEEP     0x2405
#define SPRM_PFKEEPFOLLOW 0x2406
#define SPRM_PFPAGEBREAK 0x2407
#define SPRM_PILVL      0x260A
#define SPRM_PILFO      0x460B
#define SPRM_PCHGTABSPAPX 0xC60D
#define SPRM_PDXARIGHT80 0x840E
#define SPRM_PDXALEFT80 0x840F
#define SPRM_PDXALEFT180 0x8411
#define SPRM_PDYALINE   0x6412
#define SPRM_PDYABEFORE 0xA413
#define SPRM_PDYAAFTER  0xA414
#define SPRM_PCHGTABS   0xC615
#define SPRM_PFINTABLE  0x2416
#define SPRM_PFTTP      0x2417
#define SPRM_PBRCTOP80  0x6424
#define SPRM_PBRCLEFT80 0x6425
#define SPRM_PBRCBOTTOM80 0x6426
#define SPRM_PBRCRIGHT80 0x6427
#define SPRM_PITAP      0x6649
#define SPRM_PHUGEPAPX  0x6646
#define SPRM_PHUGEPAPX97 0x6645
#define SPRM_PFINNERCELL 0x244B
#define SPRM_PFINNERTTP 0x244C
#define SPRM_PBRCTOP    0xC64E
#define SPRM_PBRCLEFT   0xC64F
#define SPRM_PBRCBOTTOM 0xC650
#define SPRM_PBRCRIGHT  0xC651
#define SPRM_PDXARIGHT  0x845D
#define SPRM_PDXALEFT   0x845E
#define SPRM_PDXALEFT1  0x8460
#define SPRM_PJC        0x2461
#define SPRM_SBKC       0x3009
#define SPRM_SFTITLEPAGE 0x300A
#define SPRM_SCCOLUMNS  0x500B
#define SPRM_SFPGNRESTART 0x3011
#define SPRM_SDYAHDRTOP 0xB017
#define SPRM_SDYAHDRBOTTOM 0xB018
#define SPRM_SPGNSTART97 0x501C
#define SPRM_SBORIENTATION 0x301D
#define SPRM_SXAPAGE    0xB01F
#define SPRM_SYAPAGE    0xB020
#define SPRM_SDXALEFT   0xB021
#define SPRM_SDXARIGHT  0xB022
#define SPRM_SDYATOP    0x9023
#define SPRM_SDYABOTTOM 0x9024
#define SPRM_SPGNSTART  0x7044
#define SPRM_TTABLEHEADER 0x3404
#define SPRM_TTABLEBORDERS80 0xD605
#define SPRM_TDEFTABLE10 0xD606
#define SPRM_TDEFTABLE  0xD608
#define SPRM_TTABLEBORDERS 0xD613
/* Word 6's own, which Word 97 has nothing like: numbers no real code has */
#define SPRM6_PANLD     0x0001
#define SPRM6_PNLVLANM  0x0002
#define SPRM6_SGPRFIHDT 0x0003

typedef struct {
    uint32_t cp, cp_end;        /* the character positions it holds */
    uint32_t fc;                /* where in WordDocument the first is */
    uint8_t  wide;              /* 16 bits a character */
    uint8_t  spare;
    uint16_t prm;               /* a change made to the whole piece */
} PIECE;

typedef struct {
    uint32_t    *fc;            /* count + 1 offsets */
    uint32_t    *pn;            /* the page that covers each stretch */
    unsigned int count;
} BINTAB;

/* one run of a page: the offsets it covers and the changes it makes */
typedef struct {
    uint32_t fc_first, fc_lim;
    const uint8_t *data;
    uint32_t len;
    uint32_t istd;
} RUN;

typedef struct {
    CHP      chp;
    uint16_t ftc;               /* the file's own number for the font */
    uint16_t sym_font, sym_char;
    uint8_t  spec, data, dead, sym;
    uint16_t spare;
    uint32_t pic;               /* where its picture is, or NO_FC */
} XCHP;

typedef struct {
    PAP      pap;               /* tabs and row stay NULL: the tabs are below */
    WTAB     tabs[MAX_TABS];
    uint32_t istd;
    uint32_t itap;
    uint16_t ilfo;
    uint8_t  ilvl;
    uint8_t  in_table, ttp, inner_cell, inner_ttp;
    uint8_t  anm;               /* Word 6: 10 a numbered list, 11 a bulleted one */
    uint8_t  anm_nfc;
    uint8_t  spare;
    uint16_t anm_start;
} XPAP;

typedef struct {
    PAGESETUP page;
    uint16_t pgn_start;
    uint8_t  bkc, restart, orient, ihdt, cols, spare;
} XSEP;

typedef struct {
    uint8_t  kind;              /* 0 none, 1 paragraph, 2 character, 3 table, 4 list */
    uint8_t  done;
    uint16_t sti;
    uint32_t base;
    const uint8_t *papx;
    uint32_t papx_len;
    const uint8_t *chpx;
    uint32_t chpx_len;
    int      doc_style;
    char     name[STYLE_NAME_LEN];
    XPAP     pap;
    XCHP     chp;
} DSTY;

typedef struct {
    uint8_t  nfc;
    uint32_t start;
    const uint8_t *papx;
    uint32_t papx_len;
} DLVL;

typedef struct {
    uint32_t lsid;
    uint32_t nlvl;
    DLVL     lvl[9];
    uint8_t  seen[9], again[9];
    uint32_t last_lfo[9];
} DLIST;

typedef struct {
    int      list;              /* which DLIST, or -1 */
    uint16_t over;              /* a bit for each level it starts afresh */
    uint16_t start[9];
} DLFO;

typedef struct {
    WDOC        *doc;
    const uint8_t *wd, *tbl, *dat;
    uint32_t     wd_len, tbl_len, dat_len;
    uint8_t     *wd_own, *tbl_own, *dat_own;
    int          w6;                 /* Word 6.0 or 95 */
    int          wide6;              /* ...with 16-bit text */
    uint32_t     pairs_at, npairs;
    uint32_t     ccp[8];
    uint32_t     cp_total;
    PIECE       *piece;
    unsigned int npiece, piece_hint;
    const uint8_t *clx;
    uint32_t     clx_len;
    BINTAB       chp_bin, pap_bin;
    unsigned int para_fails;
    uint16_t    *font, *font_cp;
    unsigned int nfont;
    uint32_t     def_ftc;
    DSTY        *sty;
    unsigned int nsty;
    uint16_t     base_flags;         /* the style's, for a code that says "as the style" */
    CHP          base_chp;
    uint32_t    *sect_cp, *sect_fc;
    unsigned int nsect;
    const uint8_t *ref[2], *txt[2];  /* footnotes, endnotes */
    unsigned int nref[2], ntxt[2];
    uint32_t     note_base[2];
    uint32_t     note_done[2];       /* how far into each kind's text the notes have got */
    DLIST       *list;
    DLFO        *lfo;
    unsigned int nlist, nlfo;
    TROW         row;                /* what the paragraph in hand says of its row */
    int          row_cells;
    uint8_t      dop_ihdt;
} DCTX;

typedef struct {
    uint8_t  state;             /* FS_* */
    uint8_t  code_len;
    uint16_t pending;           /* the WCH_ character still to be put */
    char     code[FIELD_CODE];
} FIELD;

enum { FS_CODE, FS_SHOW, FS_HIDE };

/* one story being read */
typedef struct {
    DCTX        *ctx;
    WBUILD       wb;
    int          body;          /* the main text: it has notes and sections */
    XPAP         pap;
    XCHP         chp;
    FIELD        field[FIELD_DEPTH];
    unsigned int nfield, fld_over, fld_hide;
    unsigned int cells;         /* cell marks so far in the row */
    unsigned int next_ref[2];
    unsigned int sect;
    int          was_list;
} WALK;

static void doc_story( DCTX *ctx, STORY *story, uint32_t cp, uint32_t cp_lim, int body );
static void sprm_apply( DCTX *ctx, const uint8_t *grpprl, uint32_t len, XPAP *pap, XCHP *chp, XSEP *sep, int depth );

/* ------------------------------------------------------------------ */
/* the FIB                                                             */
/* ------------------------------------------------------------------ */

/* one of the FIB's pairs, if it has it and it lies in the table stream */
static int fib_pair( const DCTX *ctx, unsigned int which, uint32_t *fc, uint32_t *lcb )
{
    uint32_t pos;

    if ( ctx->w6 ) {
        if ( which >= 50 ) {
            return 0;
        }
        pos = which < 38 ? 0x58 + 8 * which : 0x192 + 8 * (which - 38);
    } else {
        if ( which >= ctx->npairs ) {
            return 0;
        }
        pos = ctx->pairs_at + 8 * which;
    }
    if ( !IN_RANGE( pos, 8, ctx->wd_len ) ) {
        return 0;
    }
    *fc = LE32( ctx->wd + pos );
    *lcb = LE32( ctx->wd + pos + 4 );
    return *lcb != 0 && IN_RANGE( *fc, *lcb, ctx->tbl_len );
}

/* ------------------------------------------------------------------ */
/* pieces                                                              */
/* ------------------------------------------------------------------ */

static int piece_one( DCTX *ctx, uint32_t cp, uint32_t cp_end, uint32_t fc, int wide, uint32_t prm )
{
    PIECE *piece = &ctx->piece[ctx->npiece];
    uint32_t size = wide ? 2 : 1, room;

    if ( cp_end <= cp || fc >= ctx->wd_len ) {
        return 0;
    }
    /* no more of it than the stream has */
    room = (ctx->wd_len - fc) / size;
    if ( cp_end - cp > room ) {
        cp_end = cp + room;
    }
    if ( cp_end <= cp ) {
        return 0;
    }
    piece->cp = cp;
    piece->cp_end = cp_end;
    piece->fc = fc;
    piece->wide = (uint8_t)wide;
    piece->spare = 0;
    piece->prm = (uint16_t)prm;
    ctx->npiece++;
    return 1;
}

static int doc_pieces( DCTX *ctx, uint32_t fc_min, int ext_char )
{
    uint32_t fc, lcb, pos, end;
    unsigned int count = 0, index;
    uint32_t raw, cp, cp_end;
    const uint8_t *plc = NULL, *pcd;

    if ( fib_pair( ctx, FIB_CLX, &fc, &lcb ) ) {
        ctx->clx = ctx->tbl + fc;
        ctx->clx_len = lcb;
        pos = 0;
        end = lcb;
        while ( pos < end ) {
            if ( ctx->clx[pos] == 1 && end - pos >= 3 ) {
                pos += 3 + LE16( ctx->clx + pos + 1 );      /* changes a piece can name */
            } else if ( ctx->clx[pos] == 2 && end - pos >= 5 ) {
                raw = LE32( ctx->clx + pos + 1 );
                if ( raw >= 16 && raw <= end - pos - 5 ) {
                    plc = ctx->clx + pos + 5;
                    count = (raw - 4) / 12;
                }
                break;
            } else {
                break;
            }
        }
    }
    ctx->piece = (PIECE *)try_alloc( (count ? count : 1) * sizeof( PIECE ) );
    if ( ctx->piece == NULL ) {
        return 0;
    }
    for ( index = 0; index < count; index++ ) {
        cp = LE32( plc + index * 4 );
        cp_end = LE32( plc + index * 4 + 4 );
        pcd = plc + (count + 1) * 4 + index * 8;
        raw = LE32( pcd + 2 );
        if ( ctx->npiece && cp < ctx->piece[ctx->npiece - 1].cp_end ) {
            break;                      /* they must follow one another */
        }
        if ( ctx->w6 ) {
            piece_one( ctx, cp, cp_end, raw, ctx->wide6, LE16( pcd + 6 ) );
        } else if ( raw & 0x40000000UL ) {
            piece_one( ctx, cp, cp_end, (raw & 0x3FFFFFFFUL) / 2, 0, LE16( pcd + 6 ) );
        } else {
            piece_one( ctx, cp, cp_end, raw & 0x3FFFFFFFUL, 1, LE16( pcd + 6 ) );
        }
    }
    if ( count == 0 ) {
        /* no piece table: the text is in one stretch after the FIB */
        piece_one( ctx, 0, ctx->cp_total, fc_min, ctx->w6 ? ctx->wide6 : ext_char, 0 );
    }
    return 1;
}

/* the first piece that ends after "cp": its number, or npiece */
static unsigned int piece_index( DCTX *ctx, uint32_t cp )
{
    unsigned int low = 0, high = ctx->npiece, mid;

    if ( ctx->piece_hint < ctx->npiece && ctx->piece[ctx->piece_hint].cp <= cp &&
         cp < ctx->piece[ctx->piece_hint].cp_end ) {
        return ctx->piece_hint;
    }
    while ( low < high ) {
        mid = low + (high - low) / 2;
        if ( ctx->piece[mid].cp_end <= cp ) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    ctx->piece_hint = low;
    return low;
}

/* ------------------------------------------------------------------ */
/* bin tables and their pages                                          */
/* ------------------------------------------------------------------ */

static const uint8_t *fkp_page( const DCTX *ctx, uint32_t pn )
{
    if ( pn > 0x003FFFFFUL || !IN_RANGE( pn * FKP_LEN, FKP_LEN, ctx->wd_len ) ) {
        return NULL;
    }
    return ctx->wd + pn * FKP_LEN;
}

/*
 * A bin table.  Word 6 wrote a short one when it saved quickly, and
 * the FIB then says how many pages there really are ("want") and that
 * they follow one another from "first_pn"; the table is made from the
 * pages themselves in that case.
 */
static int bin_load( DCTX *ctx, unsigned int which, uint32_t first_pn, uint32_t want, BINTAB *bin )
{
    uint32_t fc, lcb;
    unsigned int count = 0, index;
    uint32_t unit = ctx->w6 ? 2 : 4, crun;
    const uint8_t *plc = NULL, *page;

    if ( fib_pair( ctx, which, &fc, &lcb ) && lcb >= 8 + unit ) {
        plc = ctx->tbl + fc;
        count = (lcb - 4) / (4 + unit);
    }
    if ( ctx->w6 && want > count ) {
        if ( want > ctx->wd_len / FKP_LEN ) {
            want = ctx->wd_len / FKP_LEN;
        }
        count = want;
        plc = NULL;
    }
    if ( count == 0 ) {
        return 1;
    }
    bin->fc = (uint32_t *)try_alloc( (count + 1) * sizeof( uint32_t ) );
    bin->pn = (uint32_t *)try_alloc( count * sizeof( uint32_t ) );
    if ( bin->fc == NULL || bin->pn == NULL ) {
        return 0;
    }
    if ( plc != NULL ) {
        for ( index = 0; index <= count; index++ ) {
            bin->fc[index] = LE32( plc + index * 4 );
        }
        for ( index = 0; index < count; index++ ) {
            bin->pn[index] = unit == 2 ? LE16( plc + (count + 1) * 4 + index * 2 )
                                       : (LE32( plc + (count + 1) * 4 + index * 4 ) & 0x003FFFFFUL);
        }
        bin->count = count;
        return 1;
    }
    for ( index = 0; index < count; index++ ) {
        page = fkp_page( ctx, first_pn + index );
        if ( page == NULL ) {
            break;
        }
        crun = page[FKP_LEN - 1];
        if ( (crun + 1) * 4 > FKP_LEN - 1 ) {
            break;
        }
        bin->pn[index] = first_pn + index;
        bin->fc[index] = LE32( page );
        bin->fc[index + 1] = LE32( page + crun * 4 );
    }
    bin->count = index;
    return 1;
}

/* the run that covers a file offset: 0 if none does */
static int run_find( const DCTX *ctx, const BINTAB *bin, uint32_t fc, int papx, RUN *run )
{
    unsigned int low = 0, high = bin->count, mid;
    uint32_t crun, bx, index, off, pos, cb;
    const uint8_t *page;

    if ( bin->count == 0 || fc < bin->fc[0] || fc >= bin->fc[bin->count] ) {
        return 0;
    }
    while ( high - low > 1 ) {
        mid = low + (high - low) / 2;
        if ( bin->fc[mid] <= fc ) {
            low = mid;
        } else {
            high = mid;
        }
    }
    page = fkp_page( ctx, bin->pn[low] );
    if ( page == NULL ) {
        return 0;
    }
    crun = page[FKP_LEN - 1];
    bx = papx ? (ctx->w6 ? 7 : 13) : 1;
    if ( crun == 0 || (crun + 1) * 4 + crun * bx > FKP_LEN - 1 ) {
        return 0;
    }
    for ( index = 0; index < crun; index++ ) {
        if ( LE32( page + index * 4 ) <= fc && fc < LE32( page + index * 4 + 4 ) ) {
            break;
        }
    }
    if ( index == crun ) {
        return 0;
    }
    run->fc_first = LE32( page + index * 4 );
    run->fc_lim = LE32( page + index * 4 + 4 );
    run->data = NULL;
    run->len = 0;
    run->istd = 0;
    off = (uint32_t)page[(crun + 1) * 4 + index * bx] * 2;
    if ( off == 0 || off >= FKP_LEN - 1 ) {
        return 1;                       /* nothing but the style */
    }
    cb = page[off];
    pos = off + 1;
    if ( !papx ) {
        if ( cb > FKP_LEN - 1 - pos ) {
            cb = FKP_LEN - 1 - pos;
        }
        run->data = page + pos;
        run->len = cb;
        return 1;
    }
    if ( ctx->w6 ) {
        cb *= 2;
    } else if ( cb == 0 ) {
        cb = (uint32_t)page[pos] * 2;
        pos++;
    } else {
        cb = cb * 2 - 1;
    }
    if ( pos >= FKP_LEN - 1 ) {
        return 1;
    }
    if ( cb > FKP_LEN - 1 - pos ) {
        cb = FKP_LEN - 1 - pos;
    }
    if ( cb >= 2 ) {
        run->istd = LE16( page + pos );
        run->data = page + pos + 2;
        run->len = cb - 2;
    }
    return 1;
}

/*
 * Where the paragraph that holds "cp" ends: the position of its mark.
 * The run of paragraph properties that covers the character may end
 * in a later piece, so the pieces are followed until one holds the
 * end.  "run" is the mark's own; "prm" its piece's.
 */
static uint32_t para_find( DCTX *ctx, uint32_t cp, RUN *run, uint32_t *prm )
{
    const PIECE *piece;
    uint32_t start = cp;
    unsigned int index, steps;
    uint32_t size, fc, fc_mac;

    run->data = NULL;
    run->len = 0;
    run->istd = 0;
    *prm = 0;
    if ( ctx->para_fails > MAX_PARA_FAILS ) {
        return ctx->cp_total;           /* the tables are no use: all one */
    }
    index = piece_index( ctx, cp );
    for ( steps = 0; index < ctx->npiece && steps <= ctx->npiece; steps++, index++ ) {
        piece = &ctx->piece[index];
        size = piece->wide ? 2 : 1;
        if ( cp < piece->cp ) {
            cp = piece->cp;
        }
        fc = piece->fc + (cp - piece->cp) * size;
        fc_mac = piece->fc + (piece->cp_end - piece->cp) * size;
        if ( run_find( ctx, &ctx->pap_bin, fc, 1, run ) && run->fc_lim > fc && run->fc_lim <= fc_mac ) {
            *prm = piece->prm;
            return piece->cp + (run->fc_lim - piece->fc) / size - 1;
        }
        cp = piece->cp_end;
    }
    /* nothing says where it ends: the piece it is in, with no changes */
    ctx->para_fails++;
    run->data = NULL;
    run->len = 0;
    run->istd = 0;
    index = piece_index( ctx, start );
    if ( index < ctx->npiece && ctx->piece[index].cp <= start ) {
        return ctx->piece[index].cp_end - 1;
    }
    return start;
}

/* ------------------------------------------------------------------ */
/* properties                                                          */
/* ------------------------------------------------------------------ */

static const uint32_t ico_rgb[17] = {
    COLOR_AUTO, 0x000000UL, 0x0000FFUL, 0x00FFFFUL, 0x00FF00UL, 0xFF00FFUL, 0xFF0000UL,
    0xFFFF00UL, 0xFFFFFFUL, 0x000080UL, 0x008080UL, 0x008000UL, 0x800080UL, 0x800000UL,
    0x808000UL, 0x808080UL, 0xC0C0C0UL
};

/* Word 6's codes: how many bytes follow each.  F0h: a length byte and
   that many; F1h: a length word, which counts one more than follows;
   F2h: the tab change, whose length byte can be a lie. */
static const uint8_t w6_size[208] = {
    0, 0, 2, 0xF0, 1, 1, 1, 1, 1, 1,                    /*   0 */
    1, 1, 0xF0, 1, 1, 0xF0, 2, 2, 2, 2,                 /*  10 */
    4, 2, 2, 0xF2, 1, 1, 2, 2, 2, 1,                    /*  20 */
    2, 2, 2, 2, 2, 2, 2, 1, 2, 2,                       /*  30 */
    2, 2, 2, 2, 1, 2, 2, 2, 2, 2,                       /*  40 */
    1, 1, 0, 0, 0, 0, 0, 0, 0, 0,                       /*  50 */
    0, 0, 0, 0, 0xF0, 1, 1, 1, 0xF0, 2,                 /*  60 */
    4, 1, 2, 3, 0xF0, 1, 0, 0xF0, 0, 0xF0,              /*  70 */
    2, 0xF0, 0xF0, 0, 0, 1, 1, 1, 1, 1,                 /*  80 */
    1, 1, 1, 2, 1, 3, 2, 2, 1, 2,                       /*  90 */
    1, 2, 1, 0xF0, 1, 0xF0, 0xF0, 2, 0xF0, 2,           /* 100 */
    2, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 1, 1, 1,     /* 110 */
    0xF0, 2, 2, 2, 2, 0, 0, 0, 0, 0,                    /* 120 */
    0, 1, 1, 0xF0, 0, 0, 3, 3, 1, 1,                    /* 130 */
    2, 2, 1, 1, 2, 2, 1, 1, 2, 2,                       /* 140 */
    1, 1, 1, 1, 2, 2, 2, 2, 1, 1,                       /* 150 */
    2, 2, 1, 0, 2, 2, 2, 2, 2, 2,                       /* 160 */
    2, 2, 0, 0, 0, 0, 0, 0, 0, 0xF0,                    /* 170 */
    0, 0xF0, 2, 2, 2, 1, 1, 12, 0xF1, 2,                /* 180 */
    0xF1, 0xF0, 4, 5, 4, 2, 4, 2, 2, 5,                 /* 190 */
    4, 0, 0, 0, 0, 0, 0, 0xF0                           /* 200 */
};

/* ...and what each one that is read is in Word 97 */
static const struct {
    uint8_t  w6;
    uint16_t w97;
} w6_map[] = {
    { 2, SPRM_PISTD }, { 5, SPRM_PJC80 }, { 7, SPRM_PFKEEP }, { 8, SPRM_PFKEEPFOLLOW },
    { 9, SPRM_PFPAGEBREAK }, { 12, SPRM6_PANLD }, { 13, SPRM6_PNLVLANM },
    { 15, SPRM_PCHGTABSPAPX }, { 16, SPRM_PDXARIGHT80 }, { 17, SPRM_PDXALEFT80 },
    { 19, SPRM_PDXALEFT180 }, { 20, SPRM_PDYALINE }, { 21, SPRM_PDYABEFORE },
    { 22, SPRM_PDYAAFTER }, { 23, SPRM_PCHGTABS }, { 24, SPRM_PFINTABLE }, { 25, SPRM_PFTTP },
    { 38, SPRM_PBRCTOP80 }, { 39, SPRM_PBRCLEFT80 }, { 40, SPRM_PBRCBOTTOM80 },
    { 41, SPRM_PBRCRIGHT80 }, { 65, SPRM_CFRMARKDEL }, { 66, SPRM_CFRMARK },
    { 68, SPRM_CPICLOC }, { 71, SPRM_CFDATA }, { 74, SPRM_CSYMBOL }, { 80, SPRM_CISTD },
    { 83, SPRM_CPLAIN }, { 85, SPRM_CFBOLD }, { 86, SPRM_CFITALIC }, { 87, SPRM_CFSTRIKE },
    { 88, SPRM_CFOUTLINE }, { 89, SPRM_CFSHADOW }, { 90, SPRM_CFSMALLCAPS }, { 91, SPRM_CFCAPS },
    { 92, SPRM_CFVANISH }, { 93, SPRM_CRGFTC0 }, { 94, SPRM_CKUL }, { 98, SPRM_CICO },
    { 99, SPRM_CHPS }, { 104, SPRM_CISS }, { 117, SPRM_CFSPEC }, { 118, SPRM_CFOBJ },
    { 142, SPRM_SBKC }, { 143, SPRM_SFTITLEPAGE }, { 144, SPRM_SCCOLUMNS },
    { 150, SPRM_SFPGNRESTART }, { 153, SPRM6_SGPRFIHDT }, { 156, SPRM_SDYAHDRTOP },
    { 157, SPRM_SDYAHDRBOTTOM }, { 161, SPRM_SPGNSTART97 }, { 162, SPRM_SBORIENTATION },
    { 164, SPRM_SXAPAGE }, { 165, SPRM_SYAPAGE }, { 166, SPRM_SDXALEFT }, { 167, SPRM_SDXARIGHT },
    { 168, SPRM_SDYATOP }, { 169, SPRM_SDYABOTTOM }, { 186, SPRM_TTABLEHEADER },
    { 187, SPRM_TTABLEBORDERS80 }, { 190, SPRM_TDEFTABLE }
};

static uint32_t w6_to_w97( uint32_t code )
{
    unsigned int index;

    for ( index = 0; index < sizeof( w6_map ) / sizeof( w6_map[0] ); index++ ) {
        if ( w6_map[index].w6 == code ) {
            return w6_map[index].w97;
        }
    }
    return 0;
}

static uint16_t font_of( const DCTX *ctx, uint32_t ftc )
{
    return (uint16_t)(ftc < ctx->nfont ? ctx->font[ftc] : 0);
}

static void chp_flag( const DCTX *ctx, XCHP *chp, uint16_t flag, uint32_t val )
{
    int on;

    if ( val == 0x80 ) {
        on = (ctx->base_flags & flag) != 0;     /* as the style has it */
    } else if ( val == 0x81 ) {
        on = (ctx->base_flags & flag) == 0;     /* the other way from the style */
    } else {
        on = val != 0;
    }
    if ( on ) {
        chp->chp.flags |= flag;
    } else {
        chp->chp.flags &= (uint16_t)~flag;
    }
}

/* a paragraph's tabs changed: some taken out, some put in */
static void tabs_change( XPAP *pap, const uint8_t *arg, uint32_t len, int close )
{
    uint32_t del, add, pos;
    unsigned int index, slot, count = pap->pap.ntabs;
    uint32_t kind, lead;
    int where, slack;

    if ( len < 1 ) {
        return;
    }
    del = arg[0];
    pos = 1;
    if ( del * (close ? 4UL : 2UL) > len - pos ) {
        return;
    }
    for ( index = 0; index < del; index++ ) {
        where = (int16_t)LE16( arg + pos + index * 2 );
        slack = close ? (int16_t)LE16( arg + pos + del * 2 + index * 2 ) : 0;
        for ( slot = 0; slot < count; slot++ ) {
            if ( pap->tabs[slot].pos >= where - slack && pap->tabs[slot].pos <= where + slack ) {
                mem_cpy( &pap->tabs[slot], &pap->tabs[slot + 1], (count - slot - 1) * sizeof( WTAB ) );
                count--;
                slot--;
            }
        }
    }
    pos += del * (close ? 4UL : 2UL);
    if ( pos < len ) {
        add = arg[pos++];
        if ( add * 3 <= len - pos ) {
            for ( index = 0; index < add; index++ ) {
                where = (int16_t)LE16( arg + pos + index * 2 );
                kind = arg[pos + add * 2 + index] & 7;
                lead = (arg[pos + add * 2 + index] >> 3) & 7;
                if ( kind > 3 ) {
                    continue;           /* a bar is not a place to tab to */
                }
                for ( slot = 0; slot < count && pap->tabs[slot].pos < where; slot++ ) {
                }
                if ( slot == count || pap->tabs[slot].pos != where ) {
                    if ( count >= MAX_TABS ) {
                        continue;
                    }
                    mem_cpy( &pap->tabs[slot + 1], &pap->tabs[slot], (count - slot) * sizeof( WTAB ) );
                    count++;
                }
                pap->tabs[slot].pos = where;
                pap->tabs[slot].kind = (uint8_t)kind;
                pap->tabs[slot].lead = (uint8_t)(lead == 0 ? LEAD_NONE : lead == 2 ? LEAD_DASHES :
                                                 lead == 3 || lead == 4 ? LEAD_LINE : LEAD_DOTS);
                pap->tabs[slot].spare = 0;
            }
        }
    }
    pap->pap.ntabs = (uint8_t)count;
}

/* 1 if a border of "size" bytes at "brc" is a line */
static int brc_is_line( const uint8_t *brc, uint32_t size )
{
    if ( size >= 8 ) {
        return brc[5] != 0 && brc[5] != 0xFF;
    }
    if ( size >= 4 ) {
        return brc[1] != 0 && brc[1] != 0xFF;
    }
    if ( size >= 2 ) {
        return (LE16( brc ) & 0x1F) != 0 && LE16( brc ) != 0xFFFF;
    }
    return 0;
}

static void pap_side( XPAP *pap, uint16_t flag, const uint8_t *arg, uint32_t len )
{
    if ( brc_is_line( arg, len ) ) {
        pap->pap.flags |= flag;
    } else {
        pap->pap.flags &= (uint16_t)~flag;
    }
}

/* a row's cells: how many, and where each one's edges are */
static void row_define( DCTX *ctx, const uint8_t *arg, uint32_t len )
{
    uint32_t cells;
    unsigned int index;
    uint32_t tc_size = ctx->w6 ? 10 : 20, side;
    const uint8_t *tc;

    if ( len < 1 ) {
        return;
    }
    cells = arg[0];
    if ( len < 1 + (cells + 1) * 2 ) {
        return;
    }
    ctx->row_cells = (int)cells;
    ctx->row.left = (int16_t)LE16( arg + 1 );
    for ( index = 0; index < cells && index < MAX_CELLS; index++ ) {
        ctx->row.edge[index] = (int16_t)LE16( arg + 3 + index * 2 );
    }
    /* each cell's own four sides, as far as they were written */
    tc = arg + 1 + (cells + 1) * 2;
    for ( index = 0; index < cells && (index + 1) * tc_size <= len - 1 - (cells + 1) * 2; index++ ) {
        for ( side = 0; side < 4; side++ ) {
            if ( ctx->w6 ? brc_is_line( tc + 2 + side * 2, 2 ) : brc_is_line( tc + 4 + side * 4, 4 ) ) {
                ctx->row.borders = 1;
            }
        }
        tc += tc_size;
    }
}

static void style_chpx( DCTX *ctx, uint32_t istd, XCHP *chp, int depth );

/* one code and its value */
static void sprm_one( DCTX *ctx, uint32_t op, const uint8_t *arg, uint32_t len, XPAP *pap, XCHP *chp, XSEP *sep,
                      int depth )
{
    uint32_t val = len >= 4 ? LE32( arg ) : len >= 2 ? LE16( arg ) : len ? arg[0] : 0;
    int sval = (int16_t)(val & 0xFFFF);
    uint32_t index;

    if ( chp != NULL ) {
        switch ( op ) {
        case SPRM_CFBOLD:       chp_flag( ctx, chp, CF_BOLD, val ); return;
        case SPRM_CFITALIC:     chp_flag( ctx, chp, CF_ITALIC, val ); return;
        case SPRM_CFSTRIKE:     chp_flag( ctx, chp, CF_STRIKE, val ); return;
        case SPRM_CFDSTRIKE:    chp_flag( ctx, chp, CF_STRIKE, val ); return;
        case SPRM_CFOUTLINE:    chp_flag( ctx, chp, CF_OUTLINE, val ); return;
        case SPRM_CFSHADOW:     chp_flag( ctx, chp, CF_SHADOW, val ); return;
        case SPRM_CFSMALLCAPS:  chp_flag( ctx, chp, CF_SMALLCAPS, val ); return;
        case SPRM_CFCAPS:       chp_flag( ctx, chp, CF_ALLCAPS, val ); return;
        case SPRM_CFVANISH:     chp_flag( ctx, chp, CF_HIDDEN, val ); return;
        case SPRM_CKUL:
            chp->chp.flags &= (uint16_t)~(CF_UNDER | CF_DUNDER | CF_WUNDER);
            if ( val != 0 ) {
                chp->chp.flags |= (uint16_t)(val == 2 ? CF_WUNDER : val == 3 ? CF_DUNDER : CF_UNDER);
            }
            return;
        case SPRM_CICO:
            chp->chp.color = ico_rgb[val <= 16 ? val : 0];
            return;
        case SPRM_CCV:
            if ( len >= 4 ) {
                chp->chp.color = arg[3] == 0xFF ? COLOR_AUTO : RGB( arg[0], arg[1], arg[2] );
            }
            return;
        case SPRM_CHPS:
            if ( val != 0 ) {
                chp->chp.size = (uint16_t)val;
            }
            return;
        case SPRM_CISS:
            chp->chp.flags &= (uint16_t)~(CF_SUPER | CF_SUB);
            if ( val == 1 || val == 2 ) {
                chp->chp.flags |= (uint16_t)(val == 1 ? CF_SUPER : CF_SUB);
            }
            return;
        case SPRM_CHIGHLIGHT:
            chp->chp.hilite = (uint8_t)(val <= 16 ? val : 0);
            return;
        case SPRM_CRGFTC0:
            chp->ftc = (uint16_t)val;
            chp->chp.font = font_of( ctx, val );
            return;
        case SPRM_CISTD:
            if ( depth < 2 ) {
                style_chpx( ctx, val, chp, depth );
            }
            return;
        case SPRM_CPLAIN:
            chp->chp = ctx->base_chp;
            return;
        case SPRM_CFSPEC:       chp->spec = (uint8_t)(val != 0); return;
        case SPRM_CFDATA:       chp->data = (uint8_t)(val != 0); return;
        case SPRM_CFOBJ:        return;
        case SPRM_CPICLOC:
            if ( len >= 4 ) {
                chp->pic = LE32( arg );
            }
            return;
        case SPRM_CFRMARKDEL:
            chp->dead = (uint8_t)(val != 0);
            if ( val != 0 ) {
                ctx->doc->lost |= LOST_REVISIONS;
            }
            return;
        case SPRM_CFRMARK:
            if ( val != 0 ) {
                ctx->doc->lost |= LOST_REVISIONS;
            }
            return;
        case SPRM_CSYMBOL:
            if ( len >= 3 ) {
                chp->sym = 1;
                chp->sym_font = font_of( ctx, LE16( arg ) );
                chp->sym_char = (uint16_t)(len >= 4 ? LE16( arg + 2 ) : 0xF000 + arg[2]);
            }
            return;
        }
    }
    if ( pap != NULL ) {
        switch ( op ) {
        case SPRM_PISTD:        pap->istd = val; return;
        case SPRM_PJC80:
        case SPRM_PJC:
            pap->pap.align = (uint8_t)(val == 0 ? AL_LEFT : val == 1 ? AL_CENTER : val == 2 ? AL_RIGHT : AL_JUSTIFY);
            return;
        case SPRM_PDXALEFT80:
        case SPRM_PDXALEFT:     pap->pap.left = sval; return;
        case SPRM_PDXARIGHT80:
        case SPRM_PDXARIGHT:    pap->pap.right = sval; return;
        case SPRM_PDXALEFT180:
        case SPRM_PDXALEFT1:    pap->pap.first = sval; return;
        case SPRM_PDYABEFORE:   pap->pap.before = (uint16_t)val; return;
        case SPRM_PDYAAFTER:    pap->pap.after = (uint16_t)val; return;
        case SPRM_PDYALINE:
            if ( len >= 4 ) {
                sval = (int16_t)LE16( arg );
                if ( LE16( arg + 2 ) != 0 ) {
                    pap->pap.line_rule = LS_MULTIPLE;
                    pap->pap.line = (uint16_t)(sval > 0 ? sval : 240);
                } else if ( sval < 0 ) {
                    pap->pap.line_rule = LS_EXACT;
                    pap->pap.line = (uint16_t)-sval;
                } else if ( sval > 0 ) {
                    pap->pap.line_rule = LS_ATLEAST;
                    pap->pap.line = (uint16_t)sval;
                } else {
                    pap->pap.line_rule = LS_MULTIPLE;
                    pap->pap.line = 240;
                }
            }
            return;
        case SPRM_PFKEEP:
            pap->pap.flags = (uint16_t)(val ? pap->pap.flags | PF_KEEP : pap->pap.flags & ~PF_KEEP);
            return;
        case SPRM_PFKEEPFOLLOW:
            pap->pap.flags = (uint16_t)(val ? pap->pap.flags | PF_KEEPNEXT : pap->pap.flags & ~PF_KEEPNEXT);
            return;
        case SPRM_PFPAGEBREAK:
            pap->pap.flags = (uint16_t)(val ? pap->pap.flags | PF_PAGEBEFORE : pap->pap.flags & ~PF_PAGEBEFORE);
            return;
        case SPRM_PFINTABLE:    pap->in_table = (uint8_t)(val != 0); return;
        case SPRM_PFTTP:        pap->ttp = (uint8_t)(val != 0); return;
        case SPRM_PITAP:        pap->itap = val; return;
        case SPRM_PFINNERCELL:  pap->inner_cell = (uint8_t)(val != 0); return;
        case SPRM_PFINNERTTP:   pap->inner_ttp = (uint8_t)(val != 0); return;
        case SPRM_PCHGTABSPAPX: tabs_change( pap, arg, len, 0 ); return;
        case SPRM_PCHGTABS:     tabs_change( pap, arg, len, 1 ); return;
        case SPRM_PILVL:        pap->ilvl = (uint8_t)val; return;
        case SPRM_PILFO:        pap->ilfo = (uint16_t)val; return;
        case SPRM_PBRCTOP80:
        case SPRM_PBRCTOP:      pap_side( pap, PF_BORDER_TOP, arg, len ); return;
        case SPRM_PBRCLEFT80:
        case SPRM_PBRCLEFT:     pap_side( pap, PF_BORDER_LEFT, arg, len ); return;
        case SPRM_PBRCBOTTOM80:
        case SPRM_PBRCBOTTOM:   pap_side( pap, PF_BORDER_BOT, arg, len ); return;
        case SPRM_PBRCRIGHT80:
        case SPRM_PBRCRIGHT:    pap_side( pap, PF_BORDER_RIGHT, arg, len ); return;
        case SPRM6_PANLD:
            if ( len >= 12 ) {
                pap->anm_nfc = arg[0];
                pap->anm_start = (uint16_t)LE16( arg + 10 );
            }
            return;
        case SPRM6_PNLVLANM:    pap->anm = (uint8_t)val; return;
        case SPRM_PHUGEPAPX:
        case SPRM_PHUGEPAPX97:
            /* properties too long for a page of them: they are in the
               Data stream, a length and then the codes - which is where
               the mark of a row of many cells keeps its own */
            if ( depth < 2 && len >= 4 && ctx->dat != NULL && IN_RANGE( val, 2, ctx->dat_len ) ) {
                index = LE16( ctx->dat + val );
                if ( index > ctx->dat_len - val - 2 ) {
                    index = ctx->dat_len - val - 2;
                }
                sprm_apply( ctx, ctx->dat + val + 2, index, pap, chp, sep, depth + 1 );
            }
            return;
        case SPRM_TDEFTABLE:
        case SPRM_TDEFTABLE10:  row_define( ctx, arg, len ); return;
        case SPRM_TTABLEHEADER: ctx->row.header = (uint8_t)(val != 0); return;
        case SPRM_TTABLEBORDERS80:
        case SPRM_TTABLEBORDERS:
            for ( index = 0; index < 6 && len >= 12; index++ ) {
                if ( brc_is_line( arg + index * (len / 6), len / 6 ) ) {
                    ctx->row.borders = 1;
                }
            }
            return;
        }
    }
    if ( sep != NULL ) {
        switch ( op ) {
        case SPRM_SBKC:         sep->bkc = (uint8_t)val; return;
        case SPRM_SCCOLUMNS:    sep->cols = (uint8_t)(val != 0); return;
        case SPRM_SFPGNRESTART: sep->restart = (uint8_t)(val != 0); return;
        case SPRM_SPGNSTART97:
        case SPRM_SPGNSTART:    sep->pgn_start = (uint16_t)val; return;
        case SPRM_SBORIENTATION: sep->orient = (uint8_t)val; return;
        case SPRM6_SGPRFIHDT:   sep->ihdt = (uint8_t)val; return;
        case SPRM_SXAPAGE:      sep->page.width = (int)(val & 0xFFFF); return;
        case SPRM_SYAPAGE:      sep->page.height = (int)(val & 0xFFFF); return;
        case SPRM_SDXALEFT:     sep->page.left = (int)(val & 0xFFFF); return;
        case SPRM_SDXARIGHT:    sep->page.right = (int)(val & 0xFFFF); return;
        case SPRM_SDYATOP:      sep->page.top = sval < 0 ? -sval : sval; return;
        case SPRM_SDYABOTTOM:   sep->page.bottom = sval < 0 ? -sval : sval; return;
        case SPRM_SDYAHDRTOP:   sep->page.header = (int)(val & 0xFFFF); return;
        case SPRM_SDYAHDRBOTTOM: sep->page.footer = (int)(val & 0xFFFF); return;
        }
    }
}

/* a list of codes, applied in order.  Each step takes at least one
   byte, so it ends. */
static void sprm_apply( DCTX *ctx, const uint8_t *grpprl, uint32_t len, XPAP *pap, XCHP *chp, XSEP *sep, int depth )
{
    uint32_t pos = 0, op, size, skip, left, del, add;

    if ( grpprl == NULL ) {
        return;
    }
    while ( pos < len ) {
        skip = 0;
        if ( ctx->w6 ) {
            op = grpprl[pos++];
            size = op < sizeof( w6_size ) ? w6_size[op] : 0;
            op = w6_to_w97( op );
            left = len - pos;
            if ( size == 0xF0 ) {
                size = left ? grpprl[pos] : 0;
                skip = 1;
            } else if ( size == 0xF1 ) {
                size = left >= 2 ? LE16( grpprl + pos ) : 0;
                size = size ? size - 1 : 0;
                skip = 2;
            } else if ( size == 0xF2 ) {
                size = left ? grpprl[pos] : 0;
                skip = 1;
                if ( size == 255 && left >= 2 ) {
                    del = grpprl[pos + 1];
                    add = 2 + del * 4 < left ? grpprl[pos + 2 + del * 4] : 0;
                    size = 2 + del * 4 + add * 3;
                }
            }
        } else {
            if ( len - pos < 2 ) {
                return;
            }
            op = LE16( grpprl + pos );
            pos += 2;
            left = len - pos;
            switch ( op >> 13 ) {
            case 0:
            case 1:
                size = 1;
                break;
            case 2:
            case 4:
            case 5:
                size = 2;
                break;
            case 3:
                size = 4;
                break;
            case 7:
                size = 3;
                break;
            default:
                if ( op == SPRM_TDEFTABLE || op == SPRM_TDEFTABLE10 ) {
                    size = left >= 2 ? LE16( grpprl + pos ) : 0;
                    size = size ? size - 1 : 0;
                    skip = 2;
                } else {
                    size = left ? grpprl[pos] : 0;
                    skip = 1;
                    if ( op == SPRM_PCHGTABS && size == 255 && left >= 2 ) {
                        del = grpprl[pos + 1];
                        add = 2 + del * 4 < left ? grpprl[pos + 2 + del * 4] : 0;
                        size = 2 + del * 4 + add * 3;
                    }
                }
                break;
            }
        }
        if ( skip > left ) {
            return;
        }
        pos += skip;
        if ( size > len - pos ) {
            return;                     /* it says more than there is */
        }
        if ( op != 0 ) {
            sprm_one( ctx, op, grpprl + pos, size, pap, chp, sep, depth );
        }
        pos += size;
    }
}

/* the change a piece makes to all of its text: one code and a byte, or
   one of the lists in front of the piece table */
static void prm_apply( DCTX *ctx, uint32_t prm, XPAP *pap, XCHP *chp )
{
    uint32_t want, pos = 0, cb;
    uint8_t one[1];

    if ( prm == 0 ) {
        return;
    }
    if ( (prm & 1) == 0 ) {
        /* the code is Word 6's number, in both generations */
        one[0] = (uint8_t)(prm >> 8);
        want = w6_to_w97( (prm >> 1) & 0x7F );
        if ( want != 0 && w6_size[(prm >> 1) & 0x7F] == 1 ) {
            sprm_one( ctx, want, one, 1, pap, chp, NULL, 0 );
        }
        return;
    }
    want = prm >> 1;
    while ( ctx->clx != NULL && pos < ctx->clx_len && ctx->clx[pos] == 1 && ctx->clx_len - pos >= 3 ) {
        cb = LE16( ctx->clx + pos + 1 );
        if ( cb > ctx->clx_len - pos - 3 ) {
            return;
        }
        if ( want == 0 ) {
            sprm_apply( ctx, ctx->clx + pos + 3, cb, pap, chp, NULL, 0 );
            return;
        }
        want--;
        pos += 3 + cb;
    }
}

/* ------------------------------------------------------------------ */
/* styles                                                              */
/* ------------------------------------------------------------------ */

static const DLVL *list_level( const DCTX *ctx, uint32_t ilfo, uint32_t ilvl )
{
    const DLIST *list;

    if ( ilfo == 0 || ilfo > ctx->nlfo || ctx->lfo[ilfo - 1].list < 0 ) {
        return NULL;
    }
    list = &ctx->list[ctx->lfo[ilfo - 1].list];
    return &list->lvl[ilvl < list->nlvl ? ilvl : list->nlvl - 1];
}

/*
 * A paragraph's own changes, on top of its style.  If they put it in a
 * list, the list level's indents come between the two: under what the
 * paragraph says for itself, over what the style says.
 */
static void papx_apply( DCTX *ctx, XPAP *pap, const uint8_t *grpprl, uint32_t len )
{
    const DLVL *lvl;
    uint16_t was_ilfo = pap->ilfo;
    uint8_t was_ilvl = pap->ilvl;
    XPAP *first;

    first = (XPAP *)try_alloc( sizeof( XPAP ) );
    if ( first == NULL ) {
        ctx->doc->failed = 1;
        return;
    }
    *first = *pap;
    sprm_apply( ctx, grpprl, len, first, NULL, NULL, 0 );
    lvl = list_level( ctx, first->ilfo, first->ilvl );
    if ( lvl != NULL && lvl->papx_len != 0 && (first->ilfo != was_ilfo || first->ilvl != was_ilvl) ) {
        sprm_apply( ctx, lvl->papx, lvl->papx_len, pap, NULL, NULL, 0 );
        sprm_apply( ctx, grpprl, len, pap, NULL, NULL, 0 );
    } else {
        *pap = *first;
    }
    xfree( first );
}

static void xchp_default( const DCTX *ctx, XCHP *chp )
{
    mem_set( chp, 0, sizeof( XCHP ) );
    chp->chp.size = 20;                 /* Word's own: ten point */
    chp->chp.color = COLOR_AUTO;
    chp->ftc = (uint16_t)ctx->def_ftc;
    chp->chp.font = font_of( ctx, ctx->def_ftc );
    chp->pic = NO_FC;
}

/* a style worked out: the one it is based on first, then its own */
static void style_resolve( DCTX *ctx, uint32_t istd )
{
    uint32_t chain[STYLE_DEPTH];
    unsigned int count = 0;
    uint32_t cur = istd;
    DSTY *sty;

    while ( count < STYLE_DEPTH && cur < ctx->nsty && !ctx->sty[cur].done ) {
        chain[count++] = cur;
        cur = ctx->sty[cur].base;
    }
    while ( count-- ) {
        sty = &ctx->sty[chain[count]];
        if ( sty->base < ctx->nsty && ctx->sty[sty->base].done && sty->base != chain[count] ) {
            sty->pap = ctx->sty[sty->base].pap;
            sty->chp = ctx->sty[sty->base].chp;
        } else {
            mem_set( &sty->pap, 0, sizeof( XPAP ) );
            pap_init( &sty->pap.pap );
            xchp_default( ctx, &sty->chp );
        }
        sty->pap.istd = chain[count];
        ctx->base_flags = sty->chp.chp.flags;
        ctx->base_chp = sty->chp.chp;
        if ( sty->kind == 1 ) {
            papx_apply( ctx, &sty->pap, sty->papx, sty->papx_len );
        }
        if ( sty->kind == 1 || sty->kind == 2 ) {
            sprm_apply( ctx, sty->chpx, sty->chpx_len, NULL, &sty->chp, NULL, 2 );
        }
        sty->done = 1;
    }
}

/* a character style named in a run: its changes, the oldest first */
static void style_chpx( DCTX *ctx, uint32_t istd, XCHP *chp, int depth )
{
    uint32_t chain[STYLE_DEPTH];
    unsigned int count = 0;

    while ( count < STYLE_DEPTH && istd < ctx->nsty && ctx->sty[istd].kind == 2 ) {
        chain[count++] = istd;
        istd = ctx->sty[istd].base;
    }
    while ( count-- ) {
        sprm_apply( ctx, ctx->sty[chain[count]].chpx, ctx->sty[chain[count]].chpx_len, NULL, chp, NULL,
                    depth + 1 );
    }
}

/* the name a style of Word's own goes by here, whatever case the file
   wrote it in; NULL for a style that is not one of those */
static const char *sti_name( uint32_t sti )
{
    static const char *const headings[] = {
        "Normal", "Heading 1", "Heading 2", "Heading 3", "Heading 4", "Heading 5", "Heading 6",
        "Heading 7", "Heading 8", "Heading 9"
    };

    if ( sti <= 9 ) {
        return headings[sti];
    }
    return sti == 62 ? "Title" : sti == 74 ? "Subtitle" : sti == 31 ? "Header" : sti == 32 ? "Footer" :
           sti == 29 ? "Footnote Text" : sti == 43 ? "Endnote Text" : sti == 34 ? "Caption" : NULL;
}

static int sti_kind( uint32_t sti )
{
    if ( sti == 0 ) {
        return STY_NORMAL;
    }
    if ( sti >= 1 && sti <= 6 ) {
        return STY_HEADING1 + (int)sti - 1;
    }
    return sti == 62 ? STY_TITLE : sti == 74 ? STY_SUBTITLE : sti == 31 ? STY_HEADER :
           sti == 32 ? STY_FOOTER : sti == 29 ? STY_FOOTNOTE : STY_OTHER;
}

static int doc_styles( DCTX *ctx )
{
    uint32_t fc, lcb, pos, cb_stshi, cstd = 0, cb_base = 0;
    unsigned int index;
    uint32_t cb, at, cch, upx, cupx, cbu, unit;
    const uint8_t *data = NULL, *std;
    DSTY *sty;
    WSTYLE *made;

    if ( fib_pair( ctx, FIB_STSHF, &fc, &lcb ) && lcb >= 6 ) {
        data = ctx->tbl + fc;
        cb_stshi = LE16( data );
        if ( cb_stshi >= 4 && cb_stshi <= lcb - 2 ) {
            cstd = LE16( data + 2 );
            cb_base = LE16( data + 4 );
            if ( cb_stshi >= 14 ) {
                ctx->def_ftc = LE16( data + 2 + 12 );
            }
            if ( cstd > (lcb - 2 - cb_stshi) / 2 ) {
                cstd = (lcb - 2 - cb_stshi) / 2;
            }
            if ( cstd > 4095 ) {
                cstd = 4095;
            }
        } else {
            data = NULL;
        }
    }
    /* there is always a first one, for a file with none */
    ctx->nsty = cstd ? cstd : 1;
    ctx->sty = (DSTY *)try_alloc( ctx->nsty * sizeof( DSTY ) );
    if ( ctx->sty == NULL ) {
        ctx->nsty = 0;
        return 0;
    }
    mem_set( ctx->sty, 0, ctx->nsty * sizeof( DSTY ) );
    for ( index = 0; index < ctx->nsty; index++ ) {
        ctx->sty[index].base = 0x0FFF;
    }
    pos = data != NULL ? 2 + LE16( data ) : 0;
    for ( index = 0; index < cstd; index++ ) {
        if ( !IN_RANGE( pos, 2, lcb ) ) {
            break;
        }
        cb = LE16( data + pos );
        pos += 2;
        if ( cb == 0 ) {
            continue;                   /* a number no style has */
        }
        if ( !IN_RANGE( pos, cb, lcb ) ) {
            break;
        }
        std = data + pos;
        pos += cb;
        if ( cb_base < 6 || cb < cb_base ) {
            continue;
        }
        sty = &ctx->sty[index];
        sty->sti = (uint16_t)(LE16( std ) & 0x0FFF);
        sty->kind = (uint8_t)(LE16( std + 2 ) & 0x0F);
        sty->base = LE16( std + 2 ) >> 4;
        cupx = LE16( std + 4 ) & 0x0F;
        /* its name: a count, the letters, and a zero */
        at = cb_base;
        unit = ctx->w6 ? 1 : 2;
        if ( at + unit > cb ) {
            continue;
        }
        cch = ctx->w6 ? std[at] : LE16( std + at );
        at += unit;
        if ( cch > (cb - at) / unit ) {
            cch = (cb - at) / unit;
        }
        for ( upx = 0; upx < cch && upx < STYLE_NAME_LEN - 1; upx++ ) {
            uint32_t code = ctx->w6 ? std[at + upx] : LE16( std + at + upx * 2 );

            sty->name[upx] = (char)(code >= 0x20 && code < 0x7F ? code : '?');
        }
        at += (cch + 1) * unit;
        /* then what it changes: a paragraph style its paragraph's and
           its characters', a character style only those */
        for ( upx = 0; upx < cupx; upx++ ) {
            at += at & 1;
            if ( at + 2 > cb ) {
                break;
            }
            cbu = LE16( std + at );
            at += 2;
            if ( cbu > cb - at ) {
                cbu = cb - at;
            }
            if ( sty->kind == 1 && upx == 0 ) {
                if ( cbu >= 2 ) {
                    sty->papx = std + at + 2;
                    sty->papx_len = cbu - 2;
                }
            } else if ( (sty->kind == 1 && upx == 1) || (sty->kind == 2 && upx == 0) ) {
                sty->chpx = std + at;
                sty->chpx_len = cbu;
            }
            at += cbu;
        }
    }
    if ( cstd == 0 ) {
        ctx->sty[0].kind = 1;
        str_cpy( ctx->sty[0].name, "Normal" );
    }
    for ( index = 0; index < ctx->nsty; index++ ) {
        style_resolve( ctx, index );
    }
    /* the paragraph styles are the document's, in the file's order */
    for ( index = 0; index < ctx->nsty; index++ ) {
        sty = &ctx->sty[index];
        if ( sty->kind != 1 || sty->name[0] == 0 ) {
            continue;
        }
        sty->doc_style = wd_style( ctx->doc, sti_name( sty->sti ) != NULL ? sti_name( sty->sti ) : sty->name,
                                   sti_kind( sty->sti ) );
        if ( sty->doc_style == 0 && sty->sti != 0 ) {
            continue;                   /* no room for it: its look is kept */
        }
        made = &ctx->doc->style[sty->doc_style];
        made->chp = sty->chp.chp;
        made->pap = sty->pap.pap;
        made->pap.flags &= PF_KEEP | PF_KEEPNEXT | PF_PAGEBEFORE | PF_BORDER_TOP | PF_BORDER_BOT |
                           PF_BORDER_LEFT | PF_BORDER_RIGHT;
        made->pap.tabs = NULL;
        made->pap.row = NULL;
        made->pap.ntabs = 0;
        made->pap.style = 0;
    }
    return 1;
}

static const DSTY *style_of( const DCTX *ctx, uint32_t istd )
{
    if ( istd >= ctx->nsty || ctx->sty[istd].kind != 1 ) {
        istd = 0;
    }
    return &ctx->sty[istd];
}

/* ------------------------------------------------------------------ */
/* fonts                                                               */
/* ------------------------------------------------------------------ */

/* the code page a font's 8-bit text is in.  2 is not one: it marks a
   font of symbols, whose bytes are not letters at all. */
static uint16_t chs_codepage( uint32_t chs )
{
    switch ( chs ) {
    case 2:   return 2;
    case 77:  return (uint16_t)CP_MAC;
    case 161: return CP_1253;
    case 162: return CP_1254;
    case 204: return CP_1251;
    case 238: return CP_1250;
    case 255: return CP_437;
    }
    return CP_1252;
}

static int doc_fonts( DCTX *ctx )
{
    char name[FONT_NAME_LEN];
    uint32_t fc, lcb, pos, max, total, at;
    unsigned int len;
    uint32_t code, flags, want = 0xFFFFFFFFUL;
    const uint8_t *data, *ffn;
    int family, fixed;

    if ( !fib_pair( ctx, FIB_FFN, &fc, &lcb ) || lcb < 4 ) {
        return 1;
    }
    data = ctx->tbl + fc;
    if ( ctx->w6 ) {
        pos = 2;
    } else {
        want = LE16( data );
        pos = 4;
    }
    max = lcb / 6 + 1;
    ctx->font = (uint16_t *)try_alloc( max * sizeof( uint16_t ) );
    ctx->font_cp = (uint16_t *)try_alloc( max * sizeof( uint16_t ) );
    if ( ctx->font == NULL || ctx->font_cp == NULL ) {
        return 0;
    }
    while ( pos < lcb && ctx->nfont < max && ctx->nfont < want ) {
        total = (uint32_t)data[pos] + 1;
        if ( total > lcb - pos ) {
            break;
        }
        ffn = data + pos;
        pos += total;
        len = 0;
        flags = total > 1 ? ffn[1] : 0;
        at = ctx->w6 ? 6 : 40;
        while ( at + (ctx->w6 ? 1 : 2) <= total && len < FONT_NAME_LEN - 1 ) {
            code = ctx->w6 ? ffn[at] : LE16( ffn + at );
            at += ctx->w6 ? 1 : 2;
            if ( code == 0 ) {
                break;
            }
            name[len++] = (char)(code >= 0x20 && code < 0x7F ? code : '?');
        }
        name[len] = 0;
        family = (int)((flags >> 4) & 7);
        family = family == 1 ? FAM_ROMAN : family == 2 ? FAM_SWISS : family == 3 ? FAM_MODERN :
                 family == 4 ? FAM_SCRIPT : family == 5 ? FAM_DECOR : FAM_ANY;
        fixed = (flags & 3) == 1 ? 1 : (flags & 3) == 2 ? 0 : -1;
        ctx->font_cp[ctx->nfont] = chs_codepage( total > 4 ? ffn[4] : 0 );
        if ( ctx->font_cp[ctx->nfont] == 2 ) {
            family = FAM_SYMBOL;
        }
        ctx->font[ctx->nfont] = (uint16_t)wd_font( ctx->doc, name, family, fixed );
        ctx->nfont++;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* lists                                                               */
/* ------------------------------------------------------------------ */

/* one level of a list, at *pos in the table stream, which moves past it */
static int lvl_read( const DCTX *ctx, uint32_t *pos, DLVL *lvl )
{
    const uint8_t *raw;
    uint32_t at = *pos, cb_chpx, cb_papx, cch;

    if ( !IN_RANGE( at, 28, ctx->tbl_len ) ) {
        return 0;
    }
    raw = ctx->tbl + at;
    cb_chpx = raw[24];
    cb_papx = raw[25];
    at += 28;
    if ( !IN_RANGE( at, cb_papx + cb_chpx + 2, ctx->tbl_len ) ) {
        return 0;
    }
    cch = LE16( ctx->tbl + at + cb_papx + cb_chpx );
    if ( !IN_RANGE( at + cb_papx + cb_chpx + 2, cch * 2, ctx->tbl_len ) ) {
        return 0;
    }
    if ( lvl != NULL ) {
        lvl->start = LE32( raw );
        lvl->nfc = raw[4];
        lvl->papx = ctx->tbl + at;
        lvl->papx_len = cb_papx;
    }
    *pos = at + cb_papx + cb_chpx + 2 + cch * 2;
    return 1;
}

static int doc_lists( DCTX *ctx )
{
    uint32_t fc, lcb, count;
    unsigned int index, level;
    uint32_t pos, other, clfolvl, flags;
    const uint8_t *data;

    if ( ctx->w6 || !fib_pair( ctx, FIB_PLFLST, &fc, &lcb ) || lcb < 2 ) {
        return 1;
    }
    data = ctx->tbl + fc;
    count = LE16( data );
    if ( count > (lcb - 2) / 28 ) {
        count = (lcb - 2) / 28;
    }
    if ( count > MAX_LISTS ) {
        count = MAX_LISTS;
    }
    if ( count == 0 ) {
        return 1;
    }
    ctx->list = (DLIST *)try_alloc( count * sizeof( DLIST ) );
    if ( ctx->list == NULL ) {
        return 0;
    }
    mem_set( ctx->list, 0, count * sizeof( DLIST ) );
    for ( index = 0; index < count; index++ ) {
        ctx->list[index].lsid = LE32( data + 2 + index * 28 );
        ctx->list[index].nlvl = (data[2 + index * 28 + 26] & 1) ? 1 : 9;
        for ( level = 0; level < 9; level++ ) {
            ctx->list[index].lvl[level].nfc = 0xFF;
            ctx->list[index].lvl[level].start = 1;
        }
    }
    ctx->nlist = count;
    /* the levels of all of them follow the last, list by list */
    pos = fc + 2 + count * 28;
    for ( index = 0; index < count; index++ ) {
        for ( level = 0; level < ctx->list[index].nlvl; level++ ) {
            if ( !lvl_read( ctx, &pos, &ctx->list[index].lvl[level] ) ) {
                index = count;
                break;
            }
        }
    }
    /* what a paragraph names is not a list but a use of one */
    if ( !fib_pair( ctx, FIB_PLFLFO, &fc, &lcb ) || lcb < 4 ) {
        return 1;
    }
    data = ctx->tbl + fc;
    count = LE32( data );
    if ( count > (lcb - 4) / 16 ) {
        count = (lcb - 4) / 16;
    }
    if ( count > MAX_LISTS ) {
        count = MAX_LISTS;
    }
    if ( count == 0 ) {
        return 1;
    }
    ctx->lfo = (DLFO *)try_alloc( count * sizeof( DLFO ) );
    if ( ctx->lfo == NULL ) {
        return 0;
    }
    mem_set( ctx->lfo, 0, count * sizeof( DLFO ) );
    for ( index = 0; index < count; index++ ) {
        ctx->lfo[index].list = -1;
        for ( other = 0; other < ctx->nlist; other++ ) {
            if ( ctx->list[other].lsid == LE32( data + 4 + index * 16 ) ) {
                ctx->lfo[index].list = (int)other;
                break;
            }
        }
    }
    ctx->nlfo = count;
    /* and after them, for each, the levels it starts at another number */
    pos = fc + 4 + count * 16;
    for ( index = 0; index < count; index++ ) {
        clfolvl = data[4 + index * 16 + 12];
        if ( !IN_RANGE( pos, 4, ctx->tbl_len ) ) {
            break;
        }
        pos += 4;
        for ( other = 0; other < clfolvl; other++ ) {
            if ( !IN_RANGE( pos, 8, ctx->tbl_len ) ) {
                return 1;
            }
            flags = LE32( ctx->tbl + pos + 4 );
            level = flags & 0x0F;
            if ( (flags & 0x10) && level < 9 ) {
                ctx->lfo[index].over |= (uint16_t)(1u << level);
                ctx->lfo[index].start[level] = (uint16_t)LE32( ctx->tbl + pos );
            }
            pos += 8;
            if ( (flags & 0x20) && !lvl_read( ctx, &pos, NULL ) ) {
                return 1;
            }
        }
    }
    return 1;
}

static int nfc_list( uint32_t nfc )
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

/* ------------------------------------------------------------------ */
/* sections                                                            */
/* ------------------------------------------------------------------ */

static int doc_sections( DCTX *ctx )
{
    unsigned int fc, lcb, count, index;
    const uint8_t *plc;

    if ( !fib_pair( ctx, FIB_SED, &fc, &lcb ) || lcb < 20 ) {
        return 1;
    }
    plc = ctx->tbl + fc;
    count = (lcb - 4) / 16;
    ctx->sect_cp = (uint32_t *)try_alloc( (count + 1) * sizeof( uint32_t ) );
    ctx->sect_fc = (uint32_t *)try_alloc( count * sizeof( uint32_t ) );
    if ( ctx->sect_cp == NULL || ctx->sect_fc == NULL ) {
        return 0;
    }
    for ( index = 0; index <= count; index++ ) {
        ctx->sect_cp[index] = LE32( plc + index * 4 );
    }
    for ( index = 0; index < count; index++ ) {
        ctx->sect_fc[index] = LE32( plc + (count + 1) * 4 + index * 12 + 2 );
    }
    ctx->nsect = count;
    return 1;
}

static void sep_load( DCTX *ctx, unsigned int index, XSEP *sep )
{
    uint32_t fc, cb;

    mem_set( sep, 0, sizeof( XSEP ) );
    sep->page = ctx->doc->page;         /* wd_new's: Word's own too */
    sep->bkc = 2;
    sep->pgn_start = 1;
    if ( index >= ctx->nsect ) {
        return;
    }
    fc = ctx->sect_fc[index];
    if ( fc == NO_FC || !IN_RANGE( fc, 2, ctx->wd_len ) ) {
        return;
    }
    cb = LE16( ctx->wd + fc );
    if ( cb > ctx->wd_len - fc - 2 ) {
        cb = ctx->wd_len - fc - 2;
    }
    sprm_apply( ctx, ctx->wd + fc + 2, cb, NULL, NULL, sep, 0 );
    if ( sep->cols ) {
        ctx->doc->lost |= LOST_LAYOUT;
    }
}

/* ------------------------------------------------------------------ */
/* pictures                                                            */
/* ------------------------------------------------------------------ */

/* one blip record -> an object, or -1 */
static int blip_object( DCTX *ctx, uint32_t type, uint32_t inst, const uint8_t *body, uint32_t len, int width, int height )
{
    uint32_t skip = 16, two, size, saved, made;
    uint8_t *plain;
    int fmt, meta = 0, obj;

    switch ( type ) {
    case 0xF01A: fmt = IMG_EMF;  meta = 1; two = 0x3D5; break;
    case 0xF01B: fmt = IMG_WMF;  meta = 1; two = 0x217; break;
    case 0xF01C: fmt = IMG_PICT; meta = 1; two = 0x543; break;
    case 0xF01D: fmt = IMG_JPEG; two = 0x46B; break;
    case 0xF02A: fmt = IMG_JPEG; two = 0x6E3; break;
    case 0xF01E: fmt = IMG_PNG;  two = 0x6E1; break;
    case 0xF01F: fmt = IMG_DIB;  two = 0x7A9; break;
    case 0xF029: fmt = IMG_TIFF; two = 0x6E5; break;
    default:     return -1;
    }
    /* one checksum of 16 bytes, or two */
    if ( inst == two || (type == 0xF01D && inst == 0x6E3) ) {
        skip = 32;
    }
    if ( !meta ) {
        skip += 1;
        if ( len <= skip ) {
            return -1;
        }
        return wd_image_new( ctx->doc, body + skip, len - skip, fmt, width, height );
    }
    /* a metafile: 34 bytes about it, and it may be packed */
    if ( len < skip + 34 ) {
        return -1;
    }
    size = LE32( body + skip );
    saved = LE32( body + skip + 28 );
    if ( saved > len - skip - 34 ) {
        saved = len - skip - 34;
    }
    if ( saved == 0 ) {
        return -1;
    }
    if ( body[skip + 32] != 0 ) {
        return wd_image_new( ctx->doc, body + skip + 34, saved, fmt, width, height );
    }
    if ( size == 0 || size > MAX_INFLATE ) {
        size = MAX_INFLATE;
    }
    plain = zlib_inflate_alloc( body + skip + 34, saved, &made, size );
    if ( plain == NULL || made == 0 ) {
        xfree( plain );
        return -1;
    }
    obj = wd_image_new( ctx->doc, plain, made, fmt, width, height );
    xfree( plain );
    return obj;
}

/* the picture a character stands for -> an object, or -1 */
static int doc_picture( DCTX *ctx, uint32_t fc )
{
    const uint8_t *base = ctx->w6 ? ctx->wd : ctx->dat, *pic, *body;
    uint32_t have = ctx->w6 ? ctx->wd_len : ctx->dat_len, lcb, header, mm, pos, len, type, inst, rec;
    unsigned int steps;
    int width, height;
    int obj = -1;

    if ( base == NULL || !IN_RANGE( fc, 0x24, have ) ) {
        return -1;
    }
    pic = base + fc;
    lcb = LE32( pic );
    header = LE16( pic + 4 );
    if ( header < 0x24 || lcb <= header || lcb > have - fc ) {
        return -1;
    }
    mm = LE16( pic + 6 );
    /* the size it was given, in twips, and how far it is scaled: both
       are 16 bits, so the product fits */
    width = (int)(LE16( pic + 0x1C ) * LE16( pic + 0x20 ) / 1000);
    height = (int)(LE16( pic + 0x1E ) * LE16( pic + 0x22 ) / 1000);
    body = pic + header;
    len = lcb - header;
    if ( mm == 8 ) {
        obj = wd_image_new( ctx->doc, body, len, IMG_WMF, width, height );
    } else if ( mm == 0x64 || mm == 0x66 ) {
        pos = 0;
        if ( mm == 0x66 ) {
            pos = (uint32_t)body[0] + 1; /* a linked file's name comes first */
        }
        for ( steps = 0; steps < 64 && IN_RANGE( pos, 8, len ); steps++ ) {
            inst = LE16( body + pos ) >> 4;
            type = LE16( body + pos + 2 );
            rec = LE32( body + pos + 4 );
            pos += 8;
            if ( type == 0xF007 ) {
                /* a store entry: 36 bytes and a name, then the picture itself */
                if ( !IN_RANGE( pos, 36, len ) ) {
                    break;
                }
                pos += 36 + body[pos + 33];
                continue;
            }
            if ( rec > len - pos ) {
                rec = len - pos;
            }
            if ( type >= 0xF018 && type <= 0xF117 ) {
                obj = blip_object( ctx, type, inst, body + pos, rec, width, height );
                break;
            }
            pos += rec;
        }
    }
    if ( obj < 0 ) {
        ctx->doc->lost |= LOST_IMAGES;
    }
    return obj;
}

/* ------------------------------------------------------------------ */
/* a story, character by character                                     */
/* ------------------------------------------------------------------ */

static void walk_put( WALK *wk, uint32_t code )
{
    uint16_t unit = (uint16_t)code;

    wk->wb.chp = wk->chp.chp;
    if ( code >= 0x10000UL ) {
        wb_char( &wk->wb, code );
    } else if ( code != 0 ) {
        /* a unit at a time, so that the two halves of a pair stay two */
        if ( code >= WCH_PAGENUM && code <= WCH_FILENAME ) {
            unit = 0xFFFD;              /* the document's own field marks */
            wk->ctx->doc->lost |= LOST_TEXT;
        }
        wb_text( &wk->wb, &unit, 1 );
    }
}

/* a field character put in as itself, past walk_put's guard */
static void walk_put_field( WALK *wk, uint32_t code )
{
    uint16_t unit = (uint16_t)code;

    wk->wb.chp = wk->chp.chp;
    wb_text( &wk->wb, &unit, 1 );
}

/* the paragraph's properties handed to the builder, for wb_par */
static void walk_set_pap( WALK *wk, uint16_t flags )
{
    DCTX *ctx = wk->ctx;
    PAP *pap = &wk->wb.pap;
    const XPAP *xp = &wk->pap;
    DLIST *list;
    DLFO *lfo;
    const DLVL *lvl;
    unsigned int level, deeper;
    uint32_t start;
    int kind;

    pap_free( pap );
    *pap = xp->pap;
    pap->tabs = NULL;
    pap->row = NULL;
    pap->ntabs = 0;
    if ( xp->pap.ntabs && !pap_set_tabs( pap, xp->tabs, xp->pap.ntabs ) ) {
        ctx->doc->failed = 1;
    }
    pap->flags = flags;
    pap->style = (uint8_t)style_of( ctx, xp->istd )->doc_style;
    pap->list = LIST_NONE;
    pap->level = 0;
    pap->list_start = 0;

    if ( ctx->w6 ) {
        /* Word 6's own numbering: the paragraph says what it wants */
        if ( xp->anm == 10 || xp->anm == 11 ) {
            kind = xp->anm == 11 ? LIST_BULLET : nfc_list( xp->anm_nfc );
            if ( kind == LIST_NONE ) {
                kind = LIST_NUMBER;
            }
            pap->list = (uint8_t)kind;
            if ( kind != LIST_BULLET && !wk->was_list ) {
                pap->list_start = (uint16_t)(xp->anm_start ? xp->anm_start : 1);
            }
        }
        wk->was_list = pap->list != LIST_NONE;
        return;
    }
    lvl = list_level( ctx, xp->ilfo, xp->ilvl );
    if ( lvl == NULL ) {
        return;
    }
    kind = nfc_list( lvl->nfc );
    if ( kind == LIST_NONE ) {
        return;
    }
    lfo = &ctx->lfo[xp->ilfo - 1];
    list = &ctx->list[lfo->list];
    level = xp->ilvl < list->nlvl ? xp->ilvl : list->nlvl - 1;
    pap->list = (uint8_t)kind;
    pap->level = (uint8_t)level;
    /* the number it starts again with: when its level is first met, or
       met again after a shallower one, or when another use of the list
       says so */
    start = 0;
    if ( !list->seen[level] || list->again[level] ) {
        start = lvl->start;
        if ( !list->seen[level] && ((lfo->over >> level) & 1) ) {
            start = lfo->start[level];
        }
    } else if ( ((lfo->over >> level) & 1) && list->last_lfo[level] != xp->ilfo ) {
        start = lfo->start[level];
    }
    list->seen[level] = 1;
    list->again[level] = 0;
    list->last_lfo[level] = xp->ilfo;
    for ( deeper = level + 1; deeper < 9; deeper++ ) {
        list->again[deeper] = 1;
    }
    if ( kind != LIST_BULLET ) {
        pap->list_start = (uint16_t)(start > 0xFFFF ? 0xFFFF : start);
    }
}

/* a row's mark: the paragraph before it is the last of the row */
static void walk_row_end( WALK *wk )
{
    DCTX *ctx = wk->ctx;
    STORY *story = wk->wb.story;
    WPARA *prev;
    TROW row;
    unsigned int cells = wk->cells, index, have, width;

    wk->cells = 0;
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
    row = ctx->row;
    have = ctx->row_cells > 0 ? (unsigned int)ctx->row_cells : 0;
    if ( have > MAX_CELLS ) {
        have = MAX_CELLS;
    }
    if ( have == 0 ) {
        /* nothing said where the cells are: the width between the
           margins, shared out */
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

/* the paragraph ends: "mark" is what ended it */
static void walk_par( WALK *wk, uint32_t mark )
{
    DCTX *ctx = wk->ctx;
    const XPAP *xp = &wk->pap;
    uint16_t flags = (uint16_t)(xp->pap.flags & (PF_KEEP | PF_KEEPNEXT | PF_PAGEBEFORE | PF_BORDER_TOP |
                                                 PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT));

    if ( xp->in_table || xp->itap != 0 ) {
        if ( xp->itap > 1 ) {
            ctx->doc->lost |= LOST_TABLES;      /* a table in a cell: its text is kept */
        }
        if ( xp->inner_ttp && wk->wb.para == NULL ) {
            return;
        }
        if ( xp->ttp && !xp->inner_ttp ) {
            if ( wk->wb.para != NULL ) {
                /* text where a row's mark should have none: a cell of its own */
                wk->cells++;
                walk_set_pap( wk, (uint16_t)(flags | PF_INTABLE | (wk->cells < MAX_CELLS ? PF_CELLEND : 0)) );
                wb_par( &wk->wb );
            }
            walk_row_end( wk );
            return;
        }
        flags |= PF_INTABLE;
        if ( mark == 0x07 && xp->itap <= 1 ) {
            /* the cells past what a row can have are joined to the
               last: only the row's mark gives that one its end */
            wk->cells++;
            if ( wk->cells < MAX_CELLS ) {
                flags |= PF_CELLEND;
            }
        }
    }
    walk_set_pap( wk, flags );
    wb_par( &wk->wb );
}

/* which of the fields that are kept as themselves a field's code names */
static uint16_t field_kind( const FIELD *field )
{
    static const struct {
        const char *word;
        uint16_t code;
    } known[] = {
        { "PAGE", WCH_PAGENUM }, { "NUMPAGES", WCH_NUMPAGES }, { "DATE", WCH_DATE },
        { "TIME", WCH_TIME }, { "FILENAME", WCH_FILENAME }
    };
    char word[FIELD_CODE];
    unsigned int at = 0, len = 0, index;

    while ( at < field->code_len && field->code[at] == ' ' ) {
        at++;
    }
    while ( at < field->code_len && ((field->code[at] >= 'A' && field->code[at] <= 'Z') ||
                                     (field->code[at] >= 'a' && field->code[at] <= 'z')) ) {
        word[len++] = (char)ch_upper( field->code[at++] );
    }
    word[len] = 0;
    for ( index = 0; index < sizeof( known ) / sizeof( known[0] ); index++ ) {
        if ( str_cmp( word, known[index].word ) == 0 ) {
            return known[index].code;
        }
    }
    return 0;
}

/* one of the three characters a field is made with */
static void walk_field( WALK *wk, uint32_t mark )
{
    FIELD *top = wk->nfield ? &wk->field[wk->nfield - 1] : NULL;
    uint16_t kind;

    if ( mark == 0x13 ) {
        if ( wk->nfield >= FIELD_DEPTH ) {
            wk->fld_over++;
            return;
        }
        top = &wk->field[wk->nfield++];
        top->state = FS_CODE;
        top->code_len = 0;
        top->pending = 0;
        wk->fld_hide++;
        return;
    }
    if ( wk->fld_over ) {
        if ( mark == 0x15 ) {
            wk->fld_over--;
        }
        return;
    }
    if ( top == NULL ) {
        return;
    }
    if ( mark == 0x14 ) {
        if ( top->state != FS_CODE ) {
            return;
        }
        kind = field_kind( top );
        if ( kind != 0 ) {
            top->state = FS_HIDE;       /* its last result is not wanted */
            top->pending = kind;
        } else {
            top->state = FS_SHOW;
            wk->fld_hide--;
            wk->ctx->doc->lost |= LOST_FIELDS;
        }
        return;
    }
    /* the end */
    if ( top->state == FS_CODE ) {
        top->pending = field_kind( top );
    }
    if ( top->state != FS_SHOW ) {
        wk->fld_hide--;
    }
    wk->nfield--;
    if ( top->pending != 0 && wk->fld_hide == 0 ) {
        walk_put_field( wk, top->pending );
    }
}

/* a character inside a field's code, or of a result that is not kept */
static void walk_field_text( WALK *wk, uint32_t unit )
{
    FIELD *top = wk->nfield ? &wk->field[wk->nfield - 1] : NULL;

    if ( top == NULL || wk->fld_over ) {
        return;
    }
    if ( top->state == FS_CODE ) {
        if ( top->code_len < FIELD_CODE - 1 && unit < 0x80 ) {
            top->code[top->code_len++] = (char)unit;
        }
    } else if ( top->state == FS_HIDE && top->pending != 0 && wk->fld_hide == 1 ) {
        /* the field's own character, looking as its result did */
        walk_put_field( wk, top->pending );
        top->pending = 0;
    }
}

/* a footnote or endnote called from here: its text, and its mark */
static void walk_note( WALK *wk, int kind, unsigned int which )
{
    DCTX *ctx = wk->ctx;
    uint32_t from, to;
    int obj;

    if ( which + 1 >= ctx->ntxt[kind] ) {
        return;
    }
    from = LE32( ctx->txt[kind] + which * 4 );
    to = LE32( ctx->txt[kind] + which * 4 + 4 );
    if ( from >= to || to > ctx->ccp[kind == NOTE_FOOT ? 1 : 5] ) {
        return;
    }
    /* each note's text follows the one before: a table that says
       otherwise would have the same text read over and over */
    if ( from < ctx->note_done[kind] ) {
        return;
    }
    ctx->note_done[kind] = to;
    obj = wd_note_new( ctx->doc, kind );
    if ( obj < 0 ) {
        return;
    }
    doc_story( ctx, wd_note_story( ctx->doc, obj ), ctx->note_base[kind] + from,
               ctx->note_base[kind] + to, 0 );
    wk->wb.chp = wk->chp.chp;
    wb_object( &wk->wb, obj );
}

static void walk_char( WALK *wk, uint32_t cp, uint32_t unit, int wide )
{
    DCTX *ctx = wk->ctx;
    XCHP *xc = &wk->chp;
    XSEP sep;
    uint32_t codepage;
    int kind, obj;

    if ( unit >= 0x13 && unit <= 0x15 ) {
        walk_field( wk, unit );
        return;
    }
    if ( wk->fld_hide || wk->fld_over ) {
        walk_field_text( wk, unit );
        return;
    }
    if ( xc->dead ) {
        return;                         /* text a tracked change took out */
    }
    if ( wk->body ) {
        for ( kind = 0; kind < 2; kind++ ) {
            while ( wk->next_ref[kind] < ctx->nref[kind] &&
                    LE32( ctx->ref[kind] + wk->next_ref[kind] * 4 ) < cp ) {
                wk->next_ref[kind]++;
            }
            if ( wk->next_ref[kind] < ctx->nref[kind] &&
                 LE32( ctx->ref[kind] + wk->next_ref[kind] * 4 ) == cp ) {
                walk_note( wk, kind, wk->next_ref[kind] );
                wk->next_ref[kind]++;
            }
        }
    }
    if ( xc->sym && unit == 0x28 ) {
        /* a symbol: the character is in the run's properties, with its font */
        wk->wb.chp = xc->chp;
        wk->wb.chp.font = xc->sym_font;
        wb_text( &wk->wb, &xc->sym_char, 1 );
        return;
    }
    if ( xc->spec ) {
        switch ( unit ) {
        case 0x00:
            walk_put_field( wk, WCH_PAGENUM );  /* Word 6's page number */
            break;
        case 0x01:
            if ( !xc->data && xc->pic != NO_FC ) {
                obj = doc_picture( ctx, xc->pic );
                if ( obj >= 0 ) {
                    wk->wb.chp = xc->chp;
                    wb_object( &wk->wb, obj );
                }
            }
            break;
        case 0x05:
            ctx->doc->lost |= LOST_NOTES;       /* a comment */
            break;
        case 0x08:
            ctx->doc->lost |= LOST_OBJECTS;     /* a shape that floats */
            break;
        }
        return;
    }
    switch ( unit ) {
    case 0x0D:
        walk_par( wk, 0x0D );
        return;
    case 0x07:
        if ( wk->pap.in_table ) {
            walk_par( wk, 0x07 );
        }
        return;
    case 0x0C:
        if ( wk->body ) {
            while ( wk->sect + 1 < ctx->nsect && cp >= ctx->sect_cp[wk->sect + 1] ) {
                wk->sect++;
            }
            if ( wk->sect + 1 < ctx->nsect && cp + 1 == ctx->sect_cp[wk->sect + 1] ) {
                /* a section ends here, and its mark ends the paragraph */
                sep_load( ctx, wk->sect + 1, &sep );
                if ( sep.bkc != 0 ) {
                    walk_put( wk, WCH_PAGE );
                }
                walk_par( wk, 0x0D );
                return;
            }
        }
        walk_put( wk, WCH_PAGE );
        return;
    case 0x0E:
        walk_put( wk, WCH_PAGE );       /* a new column: with one column, a new page */
        return;
    case 0x0B:
        walk_put( wk, WCH_LINE );
        return;
    case 0x09:
        walk_put( wk, WCH_TAB );
        return;
    case 0x1E:
        walk_put( wk, WCH_NBHYPHEN );
        return;
    case 0x1F:
        walk_put( wk, WCH_SHY );
        return;
    }
    if ( unit < 0x20 ) {
        return;
    }
    if ( !wide ) {
        codepage = CP_1252;
        if ( ctx->w6 && xc->ftc < ctx->nfont ) {
            codepage = ctx->font_cp[xc->ftc];
        }
        if ( codepage == 2 ) {
            unit += 0xF000;             /* a symbol font's: where Word 97 keeps them */
        } else if ( unit >= 0x80 ) {
            unit = cp_to_uni( (int)codepage, (int)unit );
        }
    }
    walk_put( wk, unit );
}

/* the properties of the paragraph whose mark "run" is filed under */
static void para_props( DCTX *ctx, const RUN *run, uint32_t prm, XPAP *pap )
{
    const DSTY *sty = style_of( ctx, run->istd );

    *pap = sty->pap;
    pap->istd = (uint32_t)(sty - ctx->sty);
    mem_set( &ctx->row, 0, sizeof( TROW ) );
    ctx->row_cells = -1;
    papx_apply( ctx, pap, run->data, run->len );
    prm_apply( ctx, prm, pap, NULL );
}

static void doc_story( DCTX *ctx, STORY *story, uint32_t cp, uint32_t cp_lim, int body )
{
    WALK *wk;
    RUN prun, crun;
    const PIECE *piece, *crun_piece = NULL;
    const DSTY *sty;
    uint32_t last, prm;
    unsigned int index;
    uint32_t size, fc, unit;
    int have_chpx;

    if ( story == NULL || cp >= cp_lim ) {
        return;
    }
    wk = (WALK *)try_alloc( sizeof( WALK ) );
    if ( wk == NULL ) {
        ctx->doc->failed = 1;
        return;
    }
    mem_set( wk, 0, sizeof( WALK ) );
    mem_set( &crun, 0, sizeof( crun ) );
    wk->ctx = ctx;
    wk->body = body;
    wb_init( &wk->wb, ctx->doc, story );
    while ( cp < cp_lim && !ctx->doc->failed ) {
        last = para_find( ctx, cp, &prun, &prm );
        if ( last >= cp_lim ) {
            last = cp_lim - 1;
        }
        if ( last < cp ) {
            last = cp;
        }
        para_props( ctx, &prun, prm, &wk->pap );
        sty = style_of( ctx, wk->pap.istd );
        have_chpx = 0;
        while ( cp <= last ) {
            index = piece_index( ctx, cp );
            if ( index >= ctx->npiece ) {
                cp = cp_lim;            /* no more text is anywhere */
                break;
            }
            piece = &ctx->piece[index];
            if ( piece->cp > cp ) {
                cp = piece->cp;         /* positions no piece holds */
                continue;
            }
            size = piece->wide ? 2 : 1;
            fc = piece->fc + (cp - piece->cp) * size;
            if ( !have_chpx || piece != crun_piece || fc < crun.fc_first || fc >= crun.fc_lim ) {
                if ( !run_find( ctx, &ctx->chp_bin, fc, 0, &crun ) ) {
                    crun.fc_first = fc;
                    crun.fc_lim = fc + size;
                    crun.data = NULL;
                    crun.len = 0;
                }
                wk->chp = sty->chp;
                ctx->base_flags = sty->chp.chp.flags;
                ctx->base_chp = sty->chp.chp;
                sprm_apply( ctx, crun.data, crun.len, NULL, &wk->chp, NULL, 0 );
                prm_apply( ctx, piece->prm, NULL, &wk->chp );
                crun_piece = piece;
                have_chpx = 1;
            }
            unit = piece->wide ? LE16( ctx->wd + fc ) : ctx->wd[fc];
            walk_char( wk, cp, unit, piece->wide );
            cp++;
        }
    }
    wb_done( &wk->wb );
    xfree( wk );
}

/* ------------------------------------------------------------------ */
/* the header and footer, and what the file says of itself             */
/* ------------------------------------------------------------------ */

static unsigned int bits_set( uint32_t value )
{
    unsigned int count = 0;

    for ( ; value; value >>= 1 ) {
        count += value & 1;
    }
    return count;
}

/* story "which" of the header text into "story", if the file has it */
static void doc_hdd_story( DCTX *ctx, const uint8_t *plc, unsigned int count, unsigned int which, STORY *story )
{
    uint32_t from, to, base = ctx->ccp[0] + ctx->ccp[1];

    if ( which + 1 >= count ) {
        return;
    }
    from = LE32( plc + which * 4 );
    to = LE32( plc + which * 4 + 4 );
    if ( from >= to || to > ctx->ccp[2] ) {
        return;
    }
    /* its last mark is not its own: it only closes the story */
    doc_story( ctx, story, base + from, base + to - 1, 0 );
}

static void doc_hdrftr( DCTX *ctx, const XSEP *sep )
{
    uint32_t fc, lcb;
    unsigned int count, skip;
    const uint8_t *plc;

    if ( ctx->ccp[2] == 0 || !fib_pair( ctx, FIB_HDD, &fc, &lcb ) || lcb < 8 ) {
        return;
    }
    plc = ctx->tbl + fc;
    count = lcb / 4;
    if ( !ctx->w6 ) {
        /* six for the notes' rules, then for each section the even
           header, the odd one, the two footers, and the first page's */
        doc_hdd_story( ctx, plc, count, 7, &ctx->doc->header );
        doc_hdd_story( ctx, plc, count, 9, &ctx->doc->footer );
        return;
    }
    /* Word 6 files only the ones there are, and says which */
    skip = bits_set( ctx->dop_ihdt );
    if ( sep->ihdt & 0x02 ) {
        doc_hdd_story( ctx, plc, count, skip + (sep->ihdt & 1), &ctx->doc->header );
    }
    if ( sep->ihdt & 0x08 ) {
        doc_hdd_story( ctx, plc, count, skip + bits_set( sep->ihdt & 0x07 ), &ctx->doc->footer );
    }
}

/* a property of the summary as UTF-8 */
static void info_text( char *dst, const uint8_t *src, uint32_t len, uint32_t codepage, int wide )
{
    uint16_t units[INFO_LEN];
    const uint8_t *cur = src, *end = src + len;
    unsigned int count = 0;
    uint32_t code;

    while ( count < INFO_LEN - 1 && cur < end ) {
        if ( wide ) {
            if ( end - cur < 2 ) {
                break;
            }
            code = LE16( cur );
            cur += 2;
        } else if ( codepage == CP_UTF8 ) {
            code = utf8_get( &cur, end );
            if ( code > 0xFFFF ) {
                code = 0xFFFD;
            }
        } else {
            code = *cur++;
            if ( code >= 0x80 ) {
                code = cp_to_uni( cp_known( (int)codepage ) ? (int)codepage : CP_1252, (int)code );
            }
        }
        if ( code == 0 ) {
            break;
        }
        units[count++] = (uint16_t)(code < 0x20 ? ' ' : code);
    }
    utf8_copy( dst, INFO_LEN, units, count );
}

static void doc_summary( WDOC *doc, CFB *cfb )
{
    uint8_t *data;
    const uint8_t *sec, *prop;
    uint32_t len, at, count;
    unsigned int index;
    uint32_t id, off, type, size, codepage = CP_1252;
    char *dst;
    int entry;

    entry = cfb_find( cfb, "\005SummaryInformation" );
    if ( entry < 0 ) {
        return;
    }
    data = cfb_read( cfb, entry, &len );
    if ( data == NULL ) {
        return;
    }
    if ( len >= 48 && LE32( data + 24 ) >= 1 ) {
        at = LE32( data + 44 );
        if ( IN_RANGE( at, 8, len ) ) {
            sec = data + at;
            count = LE32( sec + 4 );
            if ( count > (len - at - 8) / 8 ) {
                count = (len - at - 8) / 8;
            }
            if ( count > 64 ) {
                count = 64;
            }
            /* the code page comes first, whatever order they are in */
            for ( index = 0; index < count * 2; index++ ) {
                id = LE32( sec + 8 + (index % count) * 8 );
                off = LE32( sec + 8 + (index % count) * 8 + 4 );
                if ( !IN_RANGE( off, 8, len - at ) ) {
                    continue;
                }
                prop = sec + off;
                type = LE32( prop ) & 0xFFFF;
                if ( index < count ) {
                    if ( id == 1 && type == 2 ) {
                        codepage = LE16( prop + 4 );
                    }
                    continue;
                }
                dst = id == 2 ? doc->title : id == 3 ? doc->subject : id == 4 ? doc->author :
                      id == 5 ? doc->keywords : NULL;
                if ( dst == NULL || (type != 30 && type != 31) ) {
                    continue;
                }
                size = LE32( prop + 4 );
                if ( type == 31 ) {
                    if ( size > (len - at - off - 8) / 2 ) {
                        size = (len - at - off - 8) / 2;
                    }
                    size *= 2;
                } else if ( size > len - at - off - 8 ) {
                    size = len - at - off - 8;
                }
                info_text( dst, prop + 8, size, codepage, type == 31 );
            }
        }
    }
    xfree( data );
}

/* ------------------------------------------------------------------ */
/* detect and load                                                     */
/* ------------------------------------------------------------------ */

static int doc_detect( const uint8_t *data, uint32_t len, const char *ext )
{
    CFB *cfb;
    int sure = 0;

    (void)ext;
    if ( !cfb_is( data, len ) ) {
        return 0;
    }
    cfb = cfb_open( data, len );
    if ( cfb == NULL ) {
        return 0;
    }
    if ( cfb_find( cfb, "WordDocument" ) >= 0 ) {
        sure = 100;
    } else if ( cfb_find( cfb, "EncryptedPackage" ) >= 0 ) {
        sure = 90;                      /* a .docx with a password on it */
    }
    cfb_close( cfb );
    return sure;
}

static void doc_ctx_free( DCTX *ctx )
{
    xfree( ctx->wd_own );
    xfree( ctx->tbl_own );
    xfree( ctx->dat_own );
    xfree( ctx->piece );
    xfree( ctx->chp_bin.fc );
    xfree( ctx->chp_bin.pn );
    xfree( ctx->pap_bin.fc );
    xfree( ctx->pap_bin.pn );
    xfree( ctx->font );
    xfree( ctx->font_cp );
    xfree( ctx->sty );
    xfree( ctx->sect_cp );
    xfree( ctx->sect_fc );
    xfree( ctx->list );
    xfree( ctx->lfo );
    xfree( ctx );
}

/* the two tables that say where the notes are called from and where
   their text is */
static void doc_notes( DCTX *ctx, int kind, unsigned int ref_pair, unsigned int txt_pair, uint32_t base )
{
    uint32_t fc, lcb;

    ctx->note_base[kind] = base;
    if ( fib_pair( ctx, ref_pair, &fc, &lcb ) && lcb >= 10 ) {
        ctx->ref[kind] = ctx->tbl + fc;
        ctx->nref[kind] = (lcb - 4) / 6;
    }
    if ( fib_pair( ctx, txt_pair, &fc, &lcb ) && lcb >= 8 ) {
        ctx->txt[kind] = ctx->tbl + fc;
        ctx->ntxt[kind] = lcb / 4;
    } else {
        ctx->nref[kind] = 0;
    }
}

static int doc_read( DCTX *ctx, CFB *cfb )
{
    WDOC *doc = ctx->doc;
    XSEP sep;
    uint32_t nfib, flags, pos, count, fc, lcb;
    unsigned int index;
    uint32_t ccp_at;
    int entry;

    /* a .docx with a password on it is a compound file too, and Office
       2007 puts an empty WordDocument beside the locked package */
    if ( cfb_find( cfb, "EncryptedPackage" ) >= 0 ) {
        return WE_PASSWORD;
    }
    entry = cfb_find( cfb, "WordDocument" );
    if ( entry < 0 ) {
        return WE_FORMAT;
    }
    ctx->wd_own = cfb_read( cfb, entry, &ctx->wd_len );
    if ( ctx->wd_own == NULL ) {
        return cfb_size( cfb, entry ) > 0x00100000UL ? WE_NOMEM : WE_FORMAT;
    }
    ctx->wd = ctx->wd_own;
    if ( ctx->wd_len < 0x60 ) {
        return WE_FORMAT;
    }
    nfib = LE16( ctx->wd + 2 );
    flags = LE16( ctx->wd + 0x0A );
    if ( nfib < 101 || (nfib > 105 && nfib < 190) ) {
        return WE_UNSUPPORTED;
    }
    if ( flags & 0x0100 ) {
        return WE_PASSWORD;
    }
    ctx->w6 = nfib <= 105;
    if ( ctx->w6 ) {
        if ( ctx->wd_len < 0x2AA ) {
            return WE_FORMAT;
        }
        ctx->wide6 = (flags & 0x1000) != 0;
        ctx->tbl = ctx->wd;
        ctx->tbl_len = ctx->wd_len;
        ccp_at = 0x34;
    } else {
        /* three arrays, each with its count in front: the third is the pairs */
        pos = 0x20;
        count = LE16( ctx->wd + pos );
        pos += 2 + count * 2;
        if ( !IN_RANGE( pos, 2, ctx->wd_len ) ) {
            return WE_FORMAT;
        }
        count = LE16( ctx->wd + pos );
        ccp_at = pos + 2 + 3 * 4;
        pos += 2 + count * 4;
        if ( count < 11 || !IN_RANGE( pos, 2, ctx->wd_len ) ) {
            return WE_FORMAT;
        }
        ctx->npairs = LE16( ctx->wd + pos );
        ctx->pairs_at = pos + 2;
        entry = cfb_find( cfb, (flags & 0x0200) ? "1Table" : "0Table" );
        if ( entry < 0 ) {
            entry = cfb_find( cfb, (flags & 0x0200) ? "0Table" : "1Table" );
        }
        if ( entry < 0 ) {
            return WE_FORMAT;
        }
        ctx->tbl_own = cfb_read( cfb, entry, &ctx->tbl_len );
        if ( ctx->tbl_own == NULL ) {
            return cfb_size( cfb, entry ) > 0x00100000UL ? WE_NOMEM : WE_FORMAT;
        }
        ctx->tbl = ctx->tbl_own;
        entry = cfb_find( cfb, "Data" );
        if ( entry >= 0 ) {
            ctx->dat_own = cfb_read( cfb, entry, &ctx->dat_len );
            ctx->dat = ctx->dat_own;
        }
    }
    /* how many characters each part of the text has */
    for ( index = 0; index < 8; index++ ) {
        ctx->ccp[index] = LE32( ctx->wd + ccp_at + index * 4 );
        if ( ctx->ccp[index] > 0x0FFFFFFFUL ) {
            ctx->ccp[index] = 0;
        }
        ctx->cp_total += ctx->ccp[index];
    }
    /* Word 6 says in the FIB where its pages start and how many there are */
    if ( !doc_pieces( ctx, LE32( ctx->wd + 0x18 ), (flags & 0x1000) != 0 ) ||
         !bin_load( ctx, FIB_BTECHPX, ctx->w6 ? LE16( ctx->wd + 0x18A ) : 0,
                    ctx->w6 ? LE16( ctx->wd + 0x18E ) : 0, &ctx->chp_bin ) ||
         !bin_load( ctx, FIB_BTEPAPX, ctx->w6 ? LE16( ctx->wd + 0x18C ) : 0,
                    ctx->w6 ? LE16( ctx->wd + 0x190 ) : 0, &ctx->pap_bin ) ||
         !doc_fonts( ctx ) || !doc_lists( ctx ) || !doc_styles( ctx ) || !doc_sections( ctx ) ) {
        return WE_NOMEM;
    }
    if ( fib_pair( ctx, FIB_DOP, &fc, &lcb ) && lcb >= 12 ) {
        ctx->dop_ihdt = ctx->tbl[fc + 1];
        pos = LE16( ctx->tbl + fc + 10 );
        if ( pos > 0 && pos <= 31680 ) {
            doc->deftab = (int)pos;
        }
    }
    doc_notes( ctx, NOTE_FOOT, FIB_FNDREF, FIB_FNDTXT, ctx->ccp[0] );
    doc_notes( ctx, NOTE_END, FIB_ENDREF, FIB_ENDTXT,
               ctx->ccp[0] + ctx->ccp[1] + ctx->ccp[2] + ctx->ccp[3] + ctx->ccp[4] );

    /* the first section's page is the document's */
    sep_load( ctx, 0, &sep );
    doc->page = sep.page;
    doc->page.landscape = (uint8_t)(sep.orient == 2 || (sep.orient == 0 && sep.page.width > sep.page.height));
    doc->page.first_page = (uint16_t)(sep.restart && sep.pgn_start ? sep.pgn_start : 1);
    doc->page.spare = 0;

    doc_story( ctx, &doc->body, 0, ctx->ccp[0], 1 );
    doc_hdrftr( ctx, &sep );
    if ( ctx->ccp[4] != 0 ) {
        doc->lost |= LOST_NOTES;        /* comments */
    }
    if ( ctx->ccp[6] != 0 || ctx->ccp[7] != 0 ) {
        doc->lost |= LOST_LAYOUT;       /* text boxes */
    }
    doc_summary( doc, cfb );
    return wd_failed( doc ) ? WE_NOMEM : WE_OK;
}

static int doc_load( const uint8_t *data, uint32_t len, WDOC *doc )
{
    DCTX *ctx;
    CFB *cfb;
    int rc;

    cfb = cfb_open( data, len );
    if ( cfb == NULL ) {
        return WE_FORMAT;
    }
    ctx = (DCTX *)try_alloc( sizeof( DCTX ) );
    if ( ctx == NULL ) {
        cfb_close( cfb );
        return WE_NOMEM;
    }
    mem_set( ctx, 0, sizeof( DCTX ) );
    ctx->doc = doc;
    rc = doc_read( ctx, cfb );
    doc_ctx_free( ctx );
    cfb_close( cfb );
    return rc;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

/*
 * WHAT IS WRITTEN is the file Word 97 wrote: a FIB of nFib 00C1h with
 * 93 pairs, the text in one piece (8 bits a character when code page
 * 1252 has every one, 16 otherwise), and after it the pages of
 * character and paragraph properties; in "1Table" the styles, fonts,
 * the section, the piece table and the rest; pictures in "Data".
 *
 * The text is gathered first, part by part - the main text, the
 * footnotes, the header and footer, the endnotes - each with the runs
 * of properties that lie on it, because where a part starts in the
 * file is not known until the parts before it are whole.  A run's
 * properties are a list of codes that say how it differs from its
 * paragraph's style, and every style is written whole and based on
 * nothing, so what is read back is what was written.
 */
#define TEXT_AT         0x400           /* where the text starts in WordDocument */
#define FIB_PAIRS       93
#define DOP_LEN         500
#define ISTD_FONT       10              /* "Default Paragraph Font", which Word wants there */
#define ISTD_FIRST      15              /* a style Word keeps no place for starts here */
#define NO_STYLE        0x0FFF

enum { PART_MAIN, PART_FTN, PART_HDD, PART_EDN, PART_COUNT };

typedef struct {
    uint32_t end;               /* the character after its last */
    uint32_t off, len;          /* its properties, in the pool */
} WRUN;

typedef struct {
    uint32_t cp;
    uint8_t  ch, flt;
    uint16_t spare;
} WFLD;

typedef struct {
    uint16_t    *text;
    unsigned int len, cap;
    WRUN        *chp, *pap;
    unsigned int nchp, chp_cap, npap, pap_cap;
    WFLD        *fld;
    unsigned int nfld, fld_cap;
    uint32_t    *ref, *txt;     /* notes: where each is called from, where its text starts */
    unsigned int nnote, ref_cap, txt_cap;
} WPART;

typedef struct {
    uint8_t  kind[9];
    uint8_t  spare;
    uint16_t start[9];
} WDLIST;

typedef struct {
    uint32_t    *fc, *pn;
    unsigned int count, fc_cap, pn_cap;
    uint32_t     fc_lim;
} WBIN;

typedef struct {
    WDOC        *doc;
    WPART        part[PART_COUNT];
    OUT         *pool;                /* the runs' properties */
    OUT         *data;                /* the Data stream: pictures */
    OUT         *tbl;                 /* the table stream */
    OUT         *wd;                  /* WordDocument */
    uint32_t     pool_at;             /* where the pool ends */
    uint32_t     last_off, last_len;  /* the pool's last record */
    uint16_t     istd[MAX_STYLES];
    uint32_t     nistd;
    WDLIST      *list;
    uint32_t     nlist;
    unsigned int list_cap;
    uint32_t     cur[9];
    uint32_t     hdd_at[12];
    uint32_t     base[PART_COUNT];
    uint32_t     total;               /* characters in all the parts, and the one after them */
    uint32_t     symbol;              /* the font of bullets */
    uint32_t     npic;
    int          wide, failed, guard;
    uint32_t     pair[FIB_PAIRS][2];
} DOCW;

static const uint8_t word_clsid[16] = {
    0x06, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
};

static void dw_story( DOCW *w, int which, STORY *story );

static void put16( uint8_t *at, uint32_t value )
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put32( uint8_t *at, uint32_t value )
{
    put16( at, value );
    put16( at + 2, value >> 16 );
}

/* room for one more in an array that doubles */
static int dw_room( DOCW *w, void **array, unsigned int *cap, unsigned int count, unsigned int size )
{
    void *fresh;
    unsigned int want;

    if ( count < *cap ) {
        return 1;
    }
    want = *cap ? *cap * 2 : 64;
    fresh = want > 0x10000000UL / size ? NULL : try_realloc( *array, want * size );
    if ( fresh == NULL ) {
        w->failed = 1;
        return 0;
    }
    *array = fresh;
    *cap = want;
    return 1;
}

static void sp8( OUT *out, uint32_t op, uint32_t value )
{
    out_le16( out, op );
    out_byte( out, (int)value );
}

static void sp16( OUT *out, uint32_t op, uint32_t value )
{
    out_le16( out, op );
    out_le16( out, value );
}

static void sp32( OUT *out, uint32_t op, uint32_t value )
{
    out_le16( out, op );
    out_le32( out, value );
}

/* how "chp" differs from "base", as codes.  "whole" writes the font
   and size whatever they are: a style is based on nothing. */
static void dw_chpx( OUT *out, const CHP *chp, const CHP *base, int whole )
{
    static const struct {
        uint16_t flag;
        uint16_t op;
    } toggles[] = {
        { CF_BOLD, SPRM_CFBOLD }, { CF_ITALIC, SPRM_CFITALIC }, { CF_STRIKE, SPRM_CFSTRIKE },
        { CF_OUTLINE, SPRM_CFOUTLINE }, { CF_SHADOW, SPRM_CFSHADOW }, { CF_SMALLCAPS, SPRM_CFSMALLCAPS },
        { CF_ALLCAPS, SPRM_CFCAPS }, { CF_HIDDEN, SPRM_CFVANISH }
    };
    unsigned int index;
    uint32_t lines = CF_UNDER | CF_DUNDER | CF_WUNDER, raised = CF_SUPER | CF_SUB;

    if ( whole || chp->font != base->font ) {
        sp16( out, SPRM_CRGFTC0, chp->font );
        sp16( out, SPRM_CRGFTC0 + 1, chp->font );
        sp16( out, SPRM_CRGFTC0 + 2, chp->font );
    }
    if ( whole || chp->size != base->size ) {
        sp16( out, SPRM_CHPS, chp->size );
    }
    for ( index = 0; index < sizeof( toggles ) / sizeof( toggles[0] ); index++ ) {
        if ( (chp->flags ^ base->flags) & toggles[index].flag ) {
            sp8( out, toggles[index].op, (chp->flags & toggles[index].flag) != 0 );
        }
    }
    if ( (chp->flags ^ base->flags) & lines ) {
        sp8( out, SPRM_CKUL, chp->flags & CF_DUNDER ? 3 : chp->flags & CF_WUNDER ? 2 : chp->flags & CF_UNDER ? 1 : 0 );
    }
    if ( (chp->flags ^ base->flags) & raised ) {
        sp8( out, SPRM_CISS, chp->flags & CF_SUPER ? 1 : chp->flags & CF_SUB ? 2 : 0 );
    }
    if ( chp->color != base->color ) {
        /* one of the sixteen for a Word that knows no others, then the
           color itself */
        for ( index = 0; index <= 16; index++ ) {
            if ( ico_rgb[index] == chp->color ) {
                sp8( out, SPRM_CICO, index );
            }
        }
        out_le16( out, SPRM_CCV );
        out_byte( out, (int)((chp->color >> 16) & 0xFF) );
        out_byte( out, (int)((chp->color >> 8) & 0xFF) );
        out_byte( out, (int)(chp->color & 0xFF) );
        out_byte( out, chp->color == COLOR_AUTO ? 0xFF : 0 );
    }
    if ( chp->hilite != base->hilite ) {
        sp8( out, SPRM_CHIGHLIGHT, chp->hilite <= 16 ? chp->hilite : 0 );
    }
}

/* how "pap" differs from "base", as codes: all but its place in a
   table and a list */
static void dw_papx( OUT *out, const PAP *pap, const PAP *base )
{
    static const struct {
        uint16_t flag;
        uint16_t op;
    } flags[] = {
        { PF_KEEP, SPRM_PFKEEP }, { PF_KEEPNEXT, SPRM_PFKEEPFOLLOW }, { PF_PAGEBEFORE, SPRM_PFPAGEBREAK }
    }, sides[] = {
        { PF_BORDER_TOP, SPRM_PBRCTOP80 }, { PF_BORDER_LEFT, SPRM_PBRCLEFT80 },
        { PF_BORDER_BOT, SPRM_PBRCBOTTOM80 }, { PF_BORDER_RIGHT, SPRM_PBRCRIGHT80 }
    };
    unsigned int index, count;

    if ( pap->align != base->align ) {
        sp8( out, SPRM_PJC80, pap->align == AL_CENTER ? 1 : pap->align == AL_RIGHT ? 2 :
                              pap->align == AL_JUSTIFY ? 3 : 0 );
    }
    if ( pap->left != base->left ) {
        sp16( out, SPRM_PDXALEFT80, (uint32_t)pap->left & 0xFFFF );
    }
    if ( pap->right != base->right ) {
        sp16( out, SPRM_PDXARIGHT80, (uint32_t)pap->right & 0xFFFF );
    }
    if ( pap->first != base->first ) {
        sp16( out, SPRM_PDXALEFT180, (uint32_t)pap->first & 0xFFFF );
    }
    if ( pap->before != base->before ) {
        sp16( out, SPRM_PDYABEFORE, pap->before );
    }
    if ( pap->after != base->after ) {
        sp16( out, SPRM_PDYAAFTER, pap->after );
    }
    if ( pap->line != base->line || pap->line_rule != base->line_rule ) {
        /* the height, negative for "exactly", and whether it counts
           in lines */
        out_le16( out, SPRM_PDYALINE );
        out_le16( out, pap->line_rule == LS_EXACT ? (uint32_t)(0x10000UL - pap->line) & 0xFFFF : pap->line );
        out_le16( out, pap->line_rule == LS_MULTIPLE ? 1 : 0 );
    }
    for ( index = 0; index < sizeof( flags ) / sizeof( flags[0] ); index++ ) {
        if ( (pap->flags ^ base->flags) & flags[index].flag ) {
            sp8( out, flags[index].op, (pap->flags & flags[index].flag) != 0 );
        }
    }
    for ( index = 0; index < sizeof( sides ) / sizeof( sides[0] ); index++ ) {
        if ( (pap->flags ^ base->flags) & sides[index].flag ) {
            /* a line half a point thick, or none */
            sp32( out, sides[index].op, pap->flags & sides[index].flag ? 0x01000104UL : 0 );
        }
    }
    count = pap->tabs != NULL ? pap->ntabs : 0;
    if ( count > MAX_TABS ) {
        count = MAX_TABS;
    }
    if ( count ) {
        out_le16( out, SPRM_PCHGTABSPAPX );
        out_byte( out, (int)(2 + count * 3) );
        out_byte( out, 0 );
        out_byte( out, (int)count );
        for ( index = 0; index < count; index++ ) {
            out_le16( out, (uint32_t)pap->tabs[index].pos & 0xFFFF );
        }
        for ( index = 0; index < count; index++ ) {
            out_byte( out, (pap->tabs[index].kind & 3) |
                           ((pap->tabs[index].lead == LEAD_DOTS ? 1 : pap->tabs[index].lead == LEAD_DASHES ? 2 :
                             pap->tabs[index].lead == LEAD_LINE ? 3 : 0) << 3) );
        }
    }
}

/*
 * A record in the pool is begun, and ended: one that is the same as
 * the one before it is not kept twice.
 */
static OUT *dw_record( DOCW *w )
{
    out_seek( w->pool, w->pool_at );
    return w->pool;
}

static void dw_record_end( DOCW *w, uint32_t *off, uint32_t *len )
{
    uint8_t *bytes;
    uint32_t have, made = out_tell( w->pool ) - w->pool_at;

    bytes = out_data( w->pool, &have );
    if ( bytes != NULL && made == w->last_len && made != 0 && w->pool_at >= w->last_off + made &&
         mem_cmp( bytes + w->last_off, bytes + w->pool_at, made ) == 0 ) {
        *off = w->last_off;
        *len = made;
        return;
    }
    *off = made ? w->pool_at : 0;
    *len = made;
    if ( made ) {
        w->last_off = w->pool_at;
        w->last_len = made;
        w->pool_at += made;
    }
}

/* a run's properties: "spec" for a character that is not a letter,
   "pic" where its picture is */
static void dw_chp_record( DOCW *w, const CHP *chp, const CHP *base, int spec, uint32_t pic, uint32_t *off, uint32_t *len )
{
    OUT *out = dw_record( w );

    dw_chpx( out, chp, base, 0 );
    if ( pic != NO_FC ) {
        sp32( out, SPRM_CPICLOC, pic );
    }
    if ( spec ) {
        sp8( out, SPRM_CFSPEC, 1 );
    }
    dw_record_end( w, off, len );
}

/* a character, with the properties that lie on it */
static void dw_put( DOCW *w, WPART *part, uint32_t unit, uint32_t off, uint32_t len )
{
    WRUN *run;

    if ( !dw_room( w, (void **)&part->text, &part->cap, part->len, sizeof( uint16_t ) ) ) {
        return;
    }
    if ( len == 0 ) {
        off = 0;
    }
    if ( part->nchp && part->chp[part->nchp - 1].off == off && part->chp[part->nchp - 1].len == len ) {
        part->chp[part->nchp - 1].end = part->len + 1;
    } else {
        if ( !dw_room( w, (void **)&part->chp, &part->chp_cap, part->nchp, sizeof( WRUN ) ) ) {
            return;
        }
        run = &part->chp[part->nchp++];
        run->end = part->len + 1;
        run->off = off;
        run->len = len;
    }
    part->text[part->len++] = (uint16_t)unit;
}

/* a paragraph ends with "mark": everything since the last one that
   ended has the properties at "off" */
static void dw_par( DOCW *w, WPART *part, uint32_t mark, uint32_t chp_off, uint32_t chp_len, uint32_t off, uint32_t len )
{
    WRUN *run;

    dw_put( w, part, mark, chp_off, chp_len );
    if ( !dw_room( w, (void **)&part->pap, &part->pap_cap, part->npap, sizeof( WRUN ) ) ) {
        return;
    }
    run = &part->pap[part->npap++];
    run->end = part->len;
    run->off = off;
    run->len = len;
}

/* a mark with nothing to say for itself: the style Normal, no changes */
static void dw_plain_mark( DOCW *w, WPART *part )
{
    OUT *out = dw_record( w );
    uint32_t off, len;

    out_le16( out, 0 );
    dw_record_end( w, &off, &len );
    dw_par( w, part, 0x0D, 0, 0, off, len );
}

static void dw_field_mark( DOCW *w, WPART *part, uint32_t ch, uint32_t flt, uint32_t off, uint32_t len )
{
    WFLD *fld;

    if ( dw_room( w, (void **)&part->fld, &part->fld_cap, part->nfld, sizeof( WFLD ) ) ) {
        fld = &part->fld[part->nfld++];
        fld->cp = part->len;
        fld->ch = (uint8_t)ch;
        fld->flt = (uint8_t)flt;
        fld->spare = 0;
    }
    dw_put( w, part, ch, off, len );
}

/* a field: its three marks, its instruction, and a result to show
   until Word works it out again */
static void dw_field( DOCW *w, WPART *part, uint32_t unit, const CHP *chp, const CHP *base, uint32_t off, uint32_t len )
{
    const char *name = unit == WCH_PAGENUM ? " PAGE " : unit == WCH_NUMPAGES ? " NUMPAGES " :
                       unit == WCH_DATE ? " DATE " : unit == WCH_TIME ? " TIME " : " FILENAME ";
    uint32_t flt = unit == WCH_PAGENUM ? 33 : unit == WCH_NUMPAGES ? 26 : unit == WCH_DATE ? 31 :
              unit == WCH_TIME ? 32 : 29;
    uint32_t spec_off, spec_len;

    dw_chp_record( w, chp, base, 1, NO_FC, &spec_off, &spec_len );
    dw_field_mark( w, part, 0x13, flt, spec_off, spec_len );
    for ( ; *name; name++ ) {
        dw_put( w, part, (uint8_t)*name, off, len );
    }
    dw_field_mark( w, part, 0x14, 0xFF, spec_off, spec_len );
    if ( unit == WCH_PAGENUM || unit == WCH_NUMPAGES ) {
        dw_put( w, part, '1', off, len );
    }
    dw_field_mark( w, part, 0x15, 0x80, spec_off, spec_len );
}

/* sixteen bytes that are different for different pictures */
static void dw_uid( const uint8_t *data, uint32_t len, uint8_t *uid )
{
    uint32_t sums[4];
    unsigned int index;

    sums[0] = 2166136261UL;
    sums[1] = 5381;
    sums[2] = len;
    sums[3] = 0x9E3779B9UL;
    for ( index = 0; index < len; index++ ) {
        sums[0] = (sums[0] ^ data[index]) * 16777619UL;
        sums[1] = sums[1] * 33 + data[index];
        sums[2] = (sums[2] << 5) + (sums[2] >> 27) + data[index];
        sums[3] = (sums[3] ^ (data[index] + index)) * 2654435761UL;
    }
    for ( index = 0; index < 4; index++ ) {
        put32( uid + index * 4, sums[index] );
    }
}

/* a picture into the Data stream -> where it is, or NO_FC for a kind
   that is not written */
static uint32_t dw_picture( DOCW *w, const WOBJ *obj )
{
    OUT *out;
    const uint8_t *data = obj->data;
    uint8_t uid[16];
    uint32_t len = obj->len, at, type, inst, kind, blip, fbse, shape, wide, high;

    switch ( obj->fmt ) {
    case IMG_JPEG: kind = 5; type = 0xF01D; inst = 0x46A; break;
    case IMG_PNG:  kind = 6; type = 0xF01E; inst = 0x6E0; break;
    case IMG_DIB:  kind = 7; type = 0xF01F; inst = 0x7A8; break;
    case IMG_BMP:
        if ( len <= 14 ) {
            return NO_FC;
        }
        kind = 7;
        type = 0xF01F;
        inst = 0x7A8;
        data += 14;                     /* past what is about the file, not the picture */
        len -= 14;
        break;
    default:
        return NO_FC;
    }
    if ( data == NULL || len == 0 || len > 0x3F000000UL ) {
        return NO_FC;
    }
    if ( w->data == NULL ) {
        w->data = out_mem();
        if ( w->data == NULL ) {
            w->failed = 1;
            return NO_FC;
        }
    }
    out = w->data;
    at = out_tell( out );
    dw_uid( data, len, uid );
    blip = 8 + 17 + len;
    fbse = 36 + blip;
    shape = 16 + 8 + 12 + 12;
    wide = obj->width > 0 ? (obj->width < 32767 ? (uint32_t)obj->width : 32767) : 1;
    high = obj->height > 0 ? (obj->height < 32767 ? (uint32_t)obj->height : 32767) : 1;

    /* what Word says of the picture: how long all this is, that it is
       a shape, and the size it is shown at */
    out_le32( out, 68 + 8 + shape + 8 + fbse );
    out_le16( out, 0x44 );
    out_le16( out, 0x64 );
    out_fill( out, 0, 6 + 14 );
    out_le16( out, wide );
    out_le16( out, high );
    out_le16( out, 1000 );
    out_le16( out, 1000 );
    out_fill( out, 0, 30 + 2 );
    /* the shape: a picture frame that shows the first picture after it */
    out_le16( out, 0x000F );
    out_le16( out, 0xF004 );
    out_le32( out, shape );
    out_le16( out, 0x04B2 );
    out_le16( out, 0xF00A );
    out_le32( out, 8 );
    out_le32( out, 0x0400 + ++w->npic );
    out_le32( out, 0x0A00 );
    out_le16( out, 0x0023 );
    out_le16( out, 0xF00B );
    out_le32( out, 12 );
    out_le16( out, 0x4104 );
    out_le32( out, 1 );
    out_le16( out, 0x01FF );
    out_le32( out, 0x00080000UL );
    out_le16( out, 0x0000 );
    out_le16( out, 0xF010 );
    out_le32( out, 4 );
    out_le32( out, 0x80000000UL );
    /* the picture store's entry, and in it the picture */
    out_le16( out, 0x0002 | (kind << 4) );
    out_le16( out, 0xF007 );
    out_le32( out, fbse );
    out_byte( out, (int)kind );
    out_byte( out, (int)kind );
    out_put( out, uid, 16 );
    out_le16( out, 0x00FF );
    out_le32( out, blip );
    out_le32( out, 1 );
    out_le32( out, 0 );
    out_le32( out, 0 );
    out_le16( out, inst << 4 );
    out_le16( out, type );
    out_le32( out, 17 + len );
    out_put( out, uid, 16 );
    out_byte( out, 0xFF );
    out_put( out, data, len );
    return at;
}

/* the list a paragraph is in -> its number in the file (F_RTF.C's
   rule, and F_DOCX.C's) */
static uint32_t dw_list( DOCW *w, const PAP *pap )
{
    WDLIST *list;
    unsigned int level = pap->level < 9 ? pap->level : 8;

    if ( pap->list_start == 0 && w->cur[level] != 0 && w->list[w->cur[level] - 1].kind[level] == pap->list ) {
        return w->cur[level];
    }
    if ( w->nlist >= 2000 || !dw_room( w, (void **)&w->list, &w->list_cap, w->nlist, sizeof( WDLIST ) ) ) {
        return w->nlist;                /* no more can be made: the last one */
    }
    list = &w->list[w->nlist++];
    mem_set( list, 0, sizeof( WDLIST ) );
    mem_set( list->kind, 0xFF, 9 );
    list->kind[level] = pap->list;
    list->start[level] = (uint16_t)(pap->list_start ? pap->list_start : 1);
    w->cur[level] = w->nlist;
    return w->nlist;
}

/* a paragraph's properties as a record: its style, then the codes */
static void dw_pap_record( DOCW *w, const PAP *pap, int intbl, uint32_t *off, uint32_t *len )
{
    WDOC *doc = w->doc;
    OUT *out = dw_record( w );
    unsigned int style = pap->style < doc->nstyle ? pap->style : 0;
    uint32_t list;

    out_le16( out, w->istd[style] );
    /* the list first: Word puts the list's own indents in when it
       meets it, over any that came before */
    if ( pap->list != LIST_NONE && pap->list <= LIST_ROMAN_UPPER ) {
        list = dw_list( w, pap );
        if ( list ) {
            sp8( out, SPRM_PILVL, pap->level < 9 ? pap->level : 8 );
            sp16( out, SPRM_PILFO, list );
        }
    }
    dw_papx( out, pap, &doc->style[style].pap );
    if ( intbl ) {
        sp8( out, SPRM_PFINTABLE, 1 );
        sp32( out, SPRM_PITAP, 1 );
    }
    dw_record_end( w, off, len );
}

/* the mark that ends a row, which says where the row's cells are */
static void dw_row_mark( DOCW *w, WPART *part, const TROW *row, unsigned int cells )
{
    OUT *out = dw_record( w );
    uint32_t off, len;
    unsigned int index, have = row != NULL ? (row->ncells <= MAX_CELLS ? row->ncells : MAX_CELLS) : 0;
    int edge = row != NULL ? row->left : 0;

    if ( cells > 63 ) {
        cells = 63;
    }
    out_le16( out, 0 );
    sp8( out, SPRM_PFINTABLE, 1 );
    sp8( out, SPRM_PFTTP, 1 );
    sp32( out, SPRM_PITAP, 1 );
    sp16( out, 0x9602, 108 );           /* the room between a cell's edge and its text */
    if ( row != NULL && row->header ) {
        sp8( out, SPRM_TTABLEHEADER, 1 );
    }
    if ( row != NULL && row->borders ) {
        out_le16( out, SPRM_TTABLEBORDERS80 );
        out_byte( out, 24 );
        for ( index = 0; index < 6; index++ ) {
            out_le32( out, 0x00000104UL );
        }
    }
    out_le16( out, SPRM_TDEFTABLE );
    out_le16( out, 1 + (cells + 1) * 2 + 1 );
    out_byte( out, (int)cells );
    out_le16( out, (uint32_t)edge & 0xFFFF );
    for ( index = 0; index < cells; index++ ) {
        /* a cell the row does not place: the width between the margins
           shared out, or one more of half an inch */
        edge = index < have ? row->edge[index] : have ? edge + 720 : (int)(9000 * (index + 1) / cells);
        out_le16( out, (uint32_t)edge & 0xFFFF );
    }
    dw_record_end( w, &off, &len );
    dw_par( w, part, 0x07, 0, 0, off, len );
}

/* a footnote or endnote: its mark in the main text, and its own text */
static void dw_note( DOCW *w, const WOBJ *obj, const CHP *chp, const CHP *base )
{
    WDOC *doc = w->doc;
    int which = obj->fmt == NOTE_END ? PART_EDN : PART_FTN;
    WPART *part = &w->part[which], *text = &w->part[PART_MAIN];
    const CHP *note_base = base;
    uint32_t off, len;
    unsigned int style;

    if ( !dw_room( w, (void **)&part->ref, &part->ref_cap, part->nnote, sizeof( uint32_t ) ) ||
         !dw_room( w, (void **)&part->txt, &part->txt_cap, part->nnote, sizeof( uint32_t ) ) ) {
        return;
    }
    part->ref[part->nnote] = text->len;
    part->txt[part->nnote] = part->len;
    part->nnote++;
    dw_chp_record( w, chp, base, 1, NO_FC, &off, &len );
    dw_put( w, text, 0x02, off, len );
    /* the same mark starts the note's text, in the note's own style */
    if ( obj->story->count ) {
        style = obj->story->para[0]->pap.style < doc->nstyle ? obj->story->para[0]->pap.style : 0;
        note_base = &doc->style[style].chp;
    }
    dw_chp_record( w, chp, note_base, 1, NO_FC, &off, &len );
    dw_put( w, part, 0x02, off, len );
    if ( obj->story->count ) {
        dw_story( w, which, obj->story );
    } else {
        dw_plain_mark( w, part );
    }
}

/* one paragraph: its text, then "mark" */
static void dw_para( DOCW *w, int which, WPARA *para, int intbl, uint32_t mark )
{
    WDOC *doc = w->doc;
    WPART *part = &w->part[which];
    const CHP *base = &doc->style[para->pap.style < doc->nstyle ? para->pap.style : 0].chp, *chp;
    const WOBJ *obj;
    unsigned int at, run, index;
    uint32_t unit, off = 0, len = 0, spec_off, spec_len, pic;
    int found;

    for ( at = 0; at < para->len; at += run ) {
        run = wd_run( para, at );
        if ( run == 0 ) {
            break;
        }
        chp = wd_chp_get( doc, para->attr[at] );
        dw_chp_record( w, chp, base, 0, NO_FC, &off, &len );
        if ( para->text[at] == WCH_OBJ ) {
            found = wd_obj_at( para, at );
            obj = found >= 0 && (unsigned int)found < doc->nobj ? &doc->obj[found] : NULL;
            if ( obj != NULL && obj->kind == OBJ_IMAGE ) {
                pic = dw_picture( w, obj );
                if ( pic != NO_FC ) {
                    dw_chp_record( w, chp, base, 1, pic, &spec_off, &spec_len );
                    dw_put( w, part, 0x01, spec_off, spec_len );
                }
            } else if ( obj != NULL && obj->kind == OBJ_NOTE && obj->story != NULL && which == PART_MAIN ) {
                dw_note( w, obj, chp, base );
            }
            continue;
        }
        for ( index = 0; index < run; index++ ) {
            unit = para->text[at + index];
            if ( WCH_IS_FIELD( unit ) ) {
                dw_field( w, part, unit, chp, base, off, len );
            } else if ( unit == WCH_SHY ) {
                dw_put( w, part, 0x1F, off, len );
            } else if ( unit == WCH_NBHYPHEN ) {
                dw_put( w, part, 0x1E, off, len );
            } else if ( unit >= 0x20 || unit == WCH_TAB || unit == WCH_LINE || unit == WCH_PAGE ) {
                dw_put( w, part, unit, off, len );
            }
        }
    }
    /* the mark looks as the paragraph's last run does */
    spec_off = off;
    spec_len = len;
    dw_pap_record( w, &para->pap, intbl, &off, &len );
    dw_par( w, part, mark, spec_off, spec_len, off, len );
}

/* the last paragraph of the row that paragraph "index" is in */
static unsigned int dw_row_last( const STORY *story, unsigned int index )
{
    while ( index + 1 < story->count && !(story->para[index]->pap.flags & PF_ROWEND) &&
            (story->para[index + 1]->pap.flags & PF_INTABLE) ) {
        index++;
    }
    return index;
}

static void dw_story( DOCW *w, int which, STORY *story )
{
    WPARA *para;
    unsigned int index = 0, last, cells;
    int cell_end;

    while ( index < story->count && !w->failed ) {
        para = story->para[index];
        if ( para->pap.flags & PF_INTABLE ) {
            last = dw_row_last( story, index );
            cells = 0;
            for ( ; index <= last; index++ ) {
                para = story->para[index];
                cell_end = (para->pap.flags & PF_CELLEND) || index == last;
                dw_para( w, which, para, 1, cell_end ? 0x07 : 0x0D );
                cells += (unsigned int)cell_end;
            }
            dw_row_mark( w, &w->part[which], story->para[last]->pap.row, cells );
            continue;
        }
        dw_para( w, which, para, 0, 0x0D );
        index++;
    }
}

/* ------------------------------------------------------------------ */
/* the pages of properties                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  page[FKP_LEN];
    uint32_t fc[104];
    uint8_t  off[104];
    uint32_t crun, top;
    const uint8_t *last;        /* the body put in last, for the next that is the same */
    uint32_t last_len;
    uint8_t  last_off;
} WFKP;

static void dw_fkp_flush( DOCW *w, WFKP *fkp, int papx, WBIN *bin )
{
    unsigned int index;
    uint32_t bx = papx ? 13 : 1;

    if ( fkp->crun == 0 ) {
        return;
    }
    for ( index = 0; index <= fkp->crun; index++ ) {
        put32( fkp->page + index * 4, fkp->fc[index] );
    }
    for ( index = 0; index < fkp->crun; index++ ) {
        fkp->page[(fkp->crun + 1) * 4 + index * bx] = fkp->off[index];
    }
    fkp->page[FKP_LEN - 1] = (uint8_t)fkp->crun;
    if ( dw_room( w, (void **)&bin->fc, &bin->fc_cap, bin->count, sizeof( uint32_t ) ) &&
         dw_room( w, (void **)&bin->pn, &bin->pn_cap, bin->count, sizeof( uint32_t ) ) ) {
        bin->fc[bin->count] = fkp->fc[0];
        bin->pn[bin->count] = out_tell( w->wd ) / FKP_LEN;
        bin->count++;
    }
    bin->fc_lim = fkp->fc[fkp->crun];
    out_put( w->wd, fkp->page, FKP_LEN );
    mem_set( fkp->page, 0, FKP_LEN );
    fkp->crun = 0;
    fkp->top = FKP_LEN - 1;
    fkp->last = NULL;
}

/* one run for the page in hand: the offsets it covers and its bytes */
static void dw_fkp_add( DOCW *w, WFKP *fkp, int papx, WBIN *bin, uint32_t fc_first, uint32_t fc_lim, const uint8_t *bytes,
                        uint32_t len )
{
    uint32_t bx = papx ? 13 : 1, body, top;
    int same;

    if ( fc_lim <= fc_first ) {
        return;
    }
    if ( len > 480 ) {
        len = 480;                      /* more than a page holds: never made here */
    }
    body = papx ? ((len & 1) ? 1 + len : 2 + len) : (len ? 1 + len : 0);
    same = body && fkp->last != NULL && len == fkp->last_len && mem_cmp( bytes, fkp->last, len ) == 0;
    top = same || !body ? fkp->top : (fkp->top - body) & ~1UL;
    if ( fkp->crun && (top < (fkp->crun + 2) * 4 + (fkp->crun + 1) * bx || fkp->crun >= 100) ) {
        dw_fkp_flush( w, fkp, papx, bin );
        same = 0;
        top = body ? (fkp->top - body) & ~1UL : fkp->top;
    }
    fkp->fc[fkp->crun] = fc_first;
    fkp->fc[fkp->crun + 1] = fc_lim;
    if ( !body ) {
        fkp->off[fkp->crun] = 0;
    } else if ( same ) {
        fkp->off[fkp->crun] = fkp->last_off;
    } else {
        if ( !papx ) {
            fkp->page[top] = (uint8_t)len;
            mem_cpy( fkp->page + top + 1, bytes, len );
        } else if ( len & 1 ) {
            fkp->page[top] = (uint8_t)((len + 1) / 2);
            mem_cpy( fkp->page + top + 1, bytes, len );
        } else {
            fkp->page[top] = 0;
            fkp->page[top + 1] = (uint8_t)(len / 2);
            mem_cpy( fkp->page + top + 2, bytes, len );
        }
        fkp->top = top;
        fkp->off[fkp->crun] = (uint8_t)(top / 2);
        fkp->last = bytes;
        fkp->last_len = len;
        fkp->last_off = fkp->off[fkp->crun];
    }
    fkp->crun++;
}

/* where in WordDocument the character at a position of the whole text is */
static uint32_t dw_fc( const DOCW *w, uint32_t cp )
{
    return TEXT_AT + cp * (w->wide ? 2UL : 1UL);
}

/* all the pages of one kind, and the table that says which is which */
static void dw_fkps( DOCW *w, int papx, WBIN *bin )
{
    static const uint8_t plain[2] = { 0, 0 };
    WFKP *fkp;
    const WPART *part;
    const WRUN *run;
    const uint8_t *pool;
    uint32_t have;
    unsigned int which, index, count;
    uint32_t from;

    fkp = (WFKP *)try_alloc( sizeof( WFKP ) );
    if ( fkp == NULL ) {
        w->failed = 1;
        return;
    }
    mem_set( fkp, 0, sizeof( WFKP ) );
    fkp->top = FKP_LEN - 1;
    pool = out_data( w->pool, &have );
    for ( which = 0; which < PART_COUNT; which++ ) {
        part = &w->part[which];
        count = papx ? part->npap : part->nchp;
        from = 0;
        for ( index = 0; index < count; index++ ) {
            run = papx ? &part->pap[index] : &part->chp[index];
            dw_fkp_add( w, fkp, papx, bin, dw_fc( w, w->base[which] + from ), dw_fc( w, w->base[which] + run->end ),
                        pool != NULL && IN_RANGE( run->off, run->len, have ) ? pool + run->off : plain,
                        pool != NULL && IN_RANGE( run->off, run->len, have ) ? run->len : (papx ? 2 : 0) );
            from = run->end;
        }
    }
    if ( w->guard ) {
        /* the one mark after every part */
        dw_fkp_add( w, fkp, papx, bin, dw_fc( w, w->total - 1 ), dw_fc( w, w->total ), plain, papx ? 2 : 0 );
    }
    dw_fkp_flush( w, fkp, papx, bin );
    xfree( fkp );
}

/* ------------------------------------------------------------------ */
/* the table stream                                                    */
/* ------------------------------------------------------------------ */

/* a piece of the table stream begins, and ends: the FIB is told */
static uint32_t dw_piece( DOCW *w )
{
    return out_tell( w->tbl );
}

static void dw_piece_end( DOCW *w, unsigned int which, uint32_t start )
{
    w->pair[which][0] = start;
    w->pair[which][1] = out_tell( w->tbl ) - start;
}

static const char *dw_style_name( const WSTYLE *style )
{
    static const char *const names[] = {
        "Normal", "heading 1", "heading 2", "heading 3", "heading 4", "heading 5", "heading 6",
        "Title", "Subtitle", "header", "footer", "footnote text"
    };

    return style->kind < STY_OTHER ? names[style->kind] : style->name;
}

/* 7 to 9 if a style is one of the headings the document has no kind for */
static uint32_t dw_heading_other( const WSTYLE *style )
{
    if ( style->kind == STY_OTHER && str_nicmp( style->name, "Heading ", 8 ) == 0 &&
         style->name[8] >= '7' && style->name[8] <= '9' && style->name[9] == 0 ) {
        return (uint32_t)(style->name[8] - '0');
    }
    return 0;
}

static uint32_t dw_style_sti( const WSTYLE *style )
{
    static const uint16_t sti[] = { 0, 1, 2, 3, 4, 5, 6, 62, 74, 31, 32, 29 };

    if ( style->kind < STY_OTHER ) {
        return sti[style->kind];
    }
    if ( dw_heading_other( style ) ) {
        return dw_heading_other( style );
    }
    return str_icmp( style->name, "Endnote Text" ) == 0 ? 43 : str_icmp( style->name, "Caption" ) == 0 ? 34 : 0x0FFE;
}

/* each of the document's styles is given its number in the file: the
   ones Word keeps places for in those places, the rest after */
static void dw_style_numbers( DOCW *w )
{
    WDOC *doc = w->doc;
    uint8_t taken[ISTD_FIRST];
    unsigned int index;
    uint32_t next = ISTD_FIRST, want;

    mem_set( taken, 0, sizeof( taken ) );
    taken[0] = 1;
    w->istd[0] = 0;
    for ( index = 1; index < doc->nstyle && index < MAX_STYLES; index++ ) {
        want = dw_style_sti( &doc->style[index] );
        if ( want >= 1 && want <= 9 && !taken[want] ) {
            taken[want] = 1;
            w->istd[index] = (uint16_t)want;
        } else {
            w->istd[index] = (uint16_t)next++;
        }
    }
    w->nistd = next;
}

static void dw_std_name( OUT *out, const char *name )
{
    unsigned int len = str_len( name ), index;

    out_le16( out, len );
    for ( index = 0; index < len; index++ ) {
        out_le16( out, (uint8_t)name[index] );
    }
    out_le16( out, 0 );
}

static void dw_styles( DOCW *w )
{
    static const PAP no_pap = { AL_LEFT, 0, 0, 0, 0, 0, 0, 0, 240, LS_MULTIPLE, 0, 0, 0, 0, NULL, NULL };
    static const CHP no_chp = { 0, 0, 0, 0, 0, COLOR_AUTO };
    WDOC *doc = w->doc;
    OUT *out = w->tbl, *std;
    const WSTYLE *style;
    uint8_t *bytes;
    uint32_t start = dw_piece( w ), istd;
    unsigned int index;
    uint32_t at, len, sti;

    out_le16( out, 18 );
    out_le16( out, w->nistd );
    out_le16( out, 10 );
    out_le16( out, 1 );
    out_le16( out, 0x005B );
    out_le16( out, ISTD_FIRST );
    out_le16( out, 0 );
    out_fill( out, 0, 6 );
    for ( istd = 0; istd < w->nistd; istd++ ) {
        style = NULL;
        for ( index = 0; index < doc->nstyle && index < MAX_STYLES; index++ ) {
            if ( w->istd[index] == istd ) {
                style = &doc->style[index];
            }
        }
        if ( style == NULL && istd != ISTD_FONT ) {
            out_le16( out, 0 );         /* a number no style has */
            continue;
        }
        std = out_mem();
        if ( std == NULL ) {
            w->failed = 1;
            return;
        }
        if ( style == NULL ) {
            /* the character style every run is in that names no other */
            out_le16( std, 65 );
            out_le16( std, 2 | (NO_STYLE << 4) );
            out_le16( std, 1 | (ISTD_FONT << 4) );
            out_le16( std, 0 );
            out_le16( std, 0 );
            dw_std_name( std, "Default Paragraph Font" );
            out_le16( std, 0 );
        } else {
            sti = dw_style_sti( style );
            out_le16( std, sti );
            out_le16( std, 1 | (NO_STYLE << 4) );
            out_le16( std, 2 | (istd << 4) );
            out_le16( std, 0 );
            out_le16( std, 0 );
            dw_std_name( std, dw_style_name( style ) );
            /* what it makes of a paragraph: a length, its own number, the codes */
            at = out_tell( std );
            out_le16( std, 0 );
            out_le16( std, istd );
            dw_papx( std, &style->pap, &no_pap );
            len = out_tell( std ) - at - 2;
            out_seek( std, at );
            out_le16( std, len );
            out_seek( std, at + 2 + len );
            if ( len & 1 ) {
                out_byte( std, 0 );
            }
            /* ...and of its characters */
            at = out_tell( std );
            out_le16( std, 0 );
            dw_chpx( std, &style->chp, &no_chp, 1 );
            len = out_tell( std ) - at - 2;
            out_seek( std, at );
            out_le16( std, len );
            out_seek( std, at + 2 + len );
            if ( len & 1 ) {
                out_byte( std, 0 );
            }
        }
        len = out_tell( std );
        out_seek( std, 6 );
        out_le16( std, len );
        bytes = out_data( std, &at );
        if ( out_error( std ) || bytes == NULL ) {
            w->failed = 1;
        } else {
            out_le16( out, len );
            out_put( out, bytes, len );
        }
        out_close( std );
    }
    dw_piece_end( w, FIB_STSHF, start );
    w->pair[0][0] = w->pair[FIB_STSHF][0];
    w->pair[0][1] = w->pair[FIB_STSHF][1];
}

static void dw_fonts( DOCW *w )
{
    WDOC *doc = w->doc;
    OUT *out = w->tbl;
    const WFONT *font;
    const char *name;
    uint32_t start = dw_piece( w );
    unsigned int index, len, pos, count = doc->nfont + (w->symbol == doc->nfont ? 1 : 0);
    int family, fixed;

    out_le16( out, count );
    out_le16( out, 0 );
    for ( index = 0; index < count; index++ ) {
        font = index < doc->nfont ? &doc->font[index] : NULL;
        name = font != NULL ? font->name : "Symbol";
        family = font != NULL ? font->family : FAM_SYMBOL;
        fixed = font != NULL && font->fixed;
        len = str_len( name );
        out_byte( out, (int)(39 + (len + 1) * 2) );
        out_byte( out, (fixed ? 1 : 2) | 4 |
                       ((family == FAM_ROMAN ? 1 : family == FAM_SWISS ? 2 : family == FAM_MODERN ? 3 :
                         family == FAM_SCRIPT ? 4 : family == FAM_DECOR || family == FAM_SYMBOL ? 5 : 0) << 4) );
        out_le16( out, 400 );
        out_byte( out, family == FAM_SYMBOL ? 2 : 0 );
        out_byte( out, 0 );
        out_fill( out, 0, 10 + 24 );
        for ( pos = 0; pos <= len; pos++ ) {
            out_le16( out, (uint8_t)name[pos] );
        }
    }
    dw_piece_end( w, FIB_FFN, start );
}

/* a table of positions with nothing beside them */
static void dw_cps( OUT *out, const uint32_t *cps, unsigned int count )
{
    unsigned int index;

    for ( index = 0; index < count; index++ ) {
        out_le32( out, cps[index] );
    }
}

static void dw_notes( DOCW *w, int which, unsigned int ref_pair, unsigned int txt_pair )
{
    const WPART *part = &w->part[which];
    OUT *out = w->tbl;
    uint32_t start;
    unsigned int index;

    if ( part->nnote == 0 ) {
        return;
    }
    start = dw_piece( w );
    dw_cps( out, part->ref, part->nnote );
    out_le32( out, w->total );
    for ( index = 0; index < part->nnote; index++ ) {
        out_le16( out, index + 1 );     /* numbered by Word, not marked by hand */
    }
    dw_piece_end( w, ref_pair, start );
    start = dw_piece( w );
    dw_cps( out, part->txt, part->nnote );
    out_le32( out, part->len - 1 );
    out_le32( out, part->len );
    dw_piece_end( w, txt_pair, start );
}

static void dw_fields( DOCW *w, int which, unsigned int pair )
{
    const WPART *part = &w->part[which];
    OUT *out = w->tbl;
    uint32_t start;
    unsigned int index;

    if ( part->nfld == 0 ) {
        return;
    }
    start = dw_piece( w );
    for ( index = 0; index < part->nfld; index++ ) {
        out_le32( out, part->fld[index].cp );
    }
    out_le32( out, part->len );
    for ( index = 0; index < part->nfld; index++ ) {
        out_byte( out, part->fld[index].ch );
        out_byte( out, part->fld[index].flt );
    }
    dw_piece_end( w, pair, start );
}

static void dw_bin( DOCW *w, const WBIN *bin, unsigned int pair )
{
    OUT *out = w->tbl;
    uint32_t start = dw_piece( w );

    dw_cps( out, bin->fc, bin->count );
    out_le32( out, bin->fc_lim );
    dw_cps( out, bin->pn, bin->count );
    dw_piece_end( w, pair, start );
}

static void dw_lists( DOCW *w )
{
    static const uint8_t nfc[] = { 0, 23, 0, 4, 3, 2, 1 };  /* by LIST_* */
    OUT *out = w->tbl;
    const WDLIST *list;
    uint32_t start;
    unsigned int index, level, kind, first;

    if ( w->nlist == 0 ) {
        return;
    }
    start = dw_piece( w );
    out_le16( out, w->nlist );
    for ( index = 0; index < w->nlist; index++ ) {
        out_le32( out, 0x10000001UL + index );
        out_le32( out, 0x04090001UL + index );
        for ( level = 0; level < 9; level++ ) {
            out_le16( out, NO_STYLE );
        }
        out_le16( out, 0 );
    }
    dw_piece_end( w, FIB_PLFLST, start );
    /* the levels of each, which the table's length does not count */
    for ( index = 0; index < w->nlist; index++ ) {
        list = &w->list[index];
        first = LIST_NUMBER;
        for ( level = 9; level > 0; level-- ) {
            if ( list->kind[level - 1] != 0xFF ) {
                first = list->kind[level - 1];
            }
        }
        for ( level = 0; level < 9; level++ ) {
            kind = list->kind[level] != 0xFF ? list->kind[level] : first;
            if ( kind > LIST_ROMAN_UPPER || kind == LIST_NONE ) {
                kind = LIST_NUMBER;
            }
            out_le32( out, list->start[level] ? list->start[level] : 1 );
            out_byte( out, nfc[kind] );
            out_byte( out, 0 );
            out_byte( out, kind == LIST_BULLET ? 0 : 1 );   /* where in its text the number is */
            out_fill( out, 0, 8 + 1 + 4 + 4 );
            out_byte( out, kind == LIST_BULLET ? 12 : 0 );
            out_byte( out, 0 );
            out_le16( out, 0 );
            if ( kind == LIST_BULLET ) {
                sp16( out, SPRM_CRGFTC0, w->symbol );
                sp16( out, SPRM_CRGFTC0 + 1, w->symbol );
                sp16( out, SPRM_CRGFTC0 + 2, w->symbol );
                out_le16( out, 1 );
                out_le16( out, 0xF0B7 );
            } else {
                out_le16( out, 2 );
                out_le16( out, level );
                out_le16( out, '.' );
            }
        }
    }
    start = dw_piece( w );
    out_le32( out, w->nlist );
    for ( index = 0; index < w->nlist; index++ ) {
        out_le32( out, 0x10000001UL + index );
        out_fill( out, 0, 12 );
    }
    for ( index = 0; index < w->nlist; index++ ) {
        out_le32( out, 0xFFFFFFFFUL );
    }
    dw_piece_end( w, FIB_PLFLFO, start );
    start = dw_piece( w );
    out_le16( out, 0xFFFF );
    out_le16( out, w->nlist );
    out_le16( out, 0 );
    out_fill( out, 0, w->nlist * 2 );
    dw_piece_end( w, 91, start );
}

static void dw_dop( DOCW *w )
{
    uint8_t *dop = (uint8_t *)try_alloc( DOP_LEN );
    uint32_t start = dw_piece( w );

    if ( dop == NULL ) {
        w->failed = 1;
        return;
    }
    mem_set( dop, 0, DOP_LEN );
    dop[0] = 0x22;                      /* widows kept off; footnotes at the page's foot */
    put16( dop + 2, 1 << 2 );           /* footnotes are numbered from 1 */
    put16( dop + 10, (uint32_t)w->doc->deftab );
    put16( dop + 14, 360 );
    put16( dop + 32, 1 );
    put16( dop + 52, 1 << 2 );          /* ...and endnotes */
    put16( dop + 54, 3 );               /* which come at the document's end */
    put16( dop + 82, 1 | (100 << 3) );
    put16( dop + 410, 0x1812 );
    put16( dop + 494, 2 );              /* endnotes in small Roman numbers */
    out_put( w->tbl, dop, DOP_LEN );
    xfree( dop );
    dw_piece_end( w, FIB_DOP, start );
}

/* the summary: title, subject, author and keywords, as OLE keeps them */
static void dw_summary( DOCW *w, OUT *out )
{
    static const uint8_t fmtid[16] = {
        0xE0, 0x85, 0x9F, 0xF2, 0xF9, 0x4F, 0x68, 0x10, 0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9
    };
    const char *text[4];
    uint32_t ids[4];
    unsigned int count = 0, index;
    uint32_t at, size, len;

    text[0] = w->doc->title;
    text[1] = w->doc->subject;
    text[2] = w->doc->author;
    text[3] = w->doc->keywords;
    for ( index = 0; index < 4; index++ ) {
        if ( text[index][0] ) {
            text[count] = text[index];
            ids[count++] = index + 2;
        }
    }
    out_le16( out, 0xFFFE );
    out_le16( out, 0 );
    out_le32( out, 0x00020006UL );
    out_fill( out, 0, 16 );
    out_le32( out, 1 );
    out_put( out, fmtid, 16 );
    out_le32( out, 48 );
    /* the section: its length, how many, where each is; then each.
       The strings are UTF-8 and the first property says so. */
    size = 8 + (count + 1) * 8 + 8;
    for ( index = 0; index < count; index++ ) {
        size += 8 + ((str_len( text[index] ) + 1 + 3) & ~3UL);
    }
    out_le32( out, size );
    out_le32( out, count + 1 );
    at = 8 + (count + 1) * 8;
    out_le32( out, 1 );
    out_le32( out, at );
    at += 8;
    for ( index = 0; index < count; index++ ) {
        out_le32( out, ids[index] );
        out_le32( out, at );
        at += 8 + ((str_len( text[index] ) + 1 + 3) & ~3UL);
    }
    out_le32( out, 2 );
    out_le32( out, CP_UTF8 );
    for ( index = 0; index < count; index++ ) {
        len = str_len( text[index] ) + 1;
        out_le32( out, 30 );
        out_le32( out, len );
        out_put( out, text[index], len );
        out_fill( out, 0, ((len + 3) & ~3UL) - len );
    }
}

/* ------------------------------------------------------------------ */
/* save                                                                */
/* ------------------------------------------------------------------ */

static void dw_free( DOCW *w )
{
    unsigned int index;

    for ( index = 0; index < PART_COUNT; index++ ) {
        xfree( w->part[index].text );
        xfree( w->part[index].chp );
        xfree( w->part[index].pap );
        xfree( w->part[index].fld );
        xfree( w->part[index].ref );
        xfree( w->part[index].txt );
    }
    xfree( w->list );
    if ( w->pool != NULL ) {
        out_close( w->pool );
    }
    if ( w->data != NULL ) {
        out_close( w->data );
    }
    if ( w->tbl != NULL ) {
        out_close( w->tbl );
    }
    if ( w->wd != NULL ) {
        out_close( w->wd );
    }
    xfree( w );
}

/* the text of every part into WordDocument, and whether it needs 16 bits */
static void dw_text( DOCW *w )
{
    const WPART *part;
    unsigned int which, index;
    uint32_t unit;
    int byte;

    w->wide = 0;
    for ( which = 0; which < PART_COUNT && !w->wide; which++ ) {
        part = &w->part[which];
        for ( index = 0; index < part->len; index++ ) {
            if ( part->text[index] >= 0x80 && cp_from_uni( CP_1252, part->text[index] ) < 0x80 ) {
                w->wide = 1;
                break;
            }
        }
    }
    for ( which = 0; which < PART_COUNT; which++ ) {
        part = &w->part[which];
        for ( index = 0; index < part->len; index++ ) {
            unit = part->text[index];
            if ( w->wide ) {
                out_le16( w->wd, unit );
            } else {
                byte = unit < 0x80 ? (int)unit : cp_from_uni( CP_1252, unit );
                out_byte( w->wd, byte );
            }
        }
    }
    if ( w->guard ) {
        if ( w->wide ) {
            out_le16( w->wd, 0x0D );
        } else {
            out_byte( w->wd, 0x0D );
        }
    }
}

static int doc_write( DOCW *w, OUT *file )
{
    WDOC *doc = w->doc;
    WBIN chp_bin, pap_bin;
    CFBW *cfb;
    OUT *summary = NULL;
    uint8_t *fib, *wd_bytes, *tbl_bytes, *data_bytes = NULL, *sum_bytes = NULL;
    unsigned int index;
    uint32_t start, text_end, sepx_at, wd_len, tbl_len, data_len = 0, sum_len = 0;
    int ok = 0;

    mem_set( &chp_bin, 0, sizeof( chp_bin ) );
    mem_set( &pap_bin, 0, sizeof( pap_bin ) );
    w->pool = out_mem();
    w->tbl = out_mem();
    w->wd = out_mem();
    if ( w->pool == NULL || w->tbl == NULL || w->wd == NULL ) {
        return WE_NOMEM;
    }
    dw_style_numbers( w );
    w->symbol = doc->nfont;
    for ( index = 0; index < doc->nfont; index++ ) {
        if ( str_icmp( doc->font[index].name, "Symbol" ) == 0 ) {
            w->symbol = index;
        }
    }

    /* the text, part by part */
    dw_story( w, PART_MAIN, &doc->body );
    if ( w->part[PART_MAIN].len == 0 ) {
        dw_plain_mark( w, &w->part[PART_MAIN] );
    }
    if ( doc->header.count || doc->footer.count ) {
        /* twelve stories: six for the rules above notes, then the even
           pages' header, the odd pages', the two footers, and the
           first page's two.  Each ends with a mark that is not its own. */
        for ( index = 0; index < 12; index++ ) {
            w->hdd_at[index] = w->part[PART_HDD].len;
            if ( index == 7 && doc->header.count ) {
                dw_story( w, PART_HDD, &doc->header );
                dw_plain_mark( w, &w->part[PART_HDD] );
            } else if ( index == 9 && doc->footer.count ) {
                dw_story( w, PART_HDD, &doc->footer );
                dw_plain_mark( w, &w->part[PART_HDD] );
            }
        }
    }
    /* every part but the main text ends with one more mark, and so
       does the whole if there is any such part */
    for ( index = PART_FTN; index < PART_COUNT; index++ ) {
        if ( w->part[index].len ) {
            dw_plain_mark( w, &w->part[index] );
            w->guard = 1;
        }
    }
    w->total = 0;
    for ( index = 0; index < PART_COUNT; index++ ) {
        w->base[index] = w->total;
        w->total += w->part[index].len;
    }
    w->total += (uint32_t)w->guard;
    if ( w->failed || w->total > 0x1FFFFFFFUL ) {
        return WE_NOMEM;
    }

    /* WordDocument: room for the FIB, the text, the section's
       properties, then the pages */
    out_fill( w->wd, 0, TEXT_AT );
    dw_text( w );
    text_end = out_tell( w->wd );
    sepx_at = text_end;
    out_le16( w->wd, 0 );
    sp8( w->wd, SPRM_SBORIENTATION, doc->page.landscape ? 2 : 1 );
    sp16( w->wd, SPRM_SXAPAGE, (uint32_t)doc->page.width & 0xFFFF );
    sp16( w->wd, SPRM_SYAPAGE, (uint32_t)doc->page.height & 0xFFFF );
    sp16( w->wd, SPRM_SDXALEFT, (uint32_t)doc->page.left & 0xFFFF );
    sp16( w->wd, SPRM_SDXARIGHT, (uint32_t)doc->page.right & 0xFFFF );
    sp16( w->wd, SPRM_SDYATOP, (uint32_t)doc->page.top & 0xFFFF );
    sp16( w->wd, SPRM_SDYABOTTOM, (uint32_t)doc->page.bottom & 0xFFFF );
    sp16( w->wd, SPRM_SDYAHDRTOP, (uint32_t)doc->page.header & 0xFFFF );
    sp16( w->wd, SPRM_SDYAHDRBOTTOM, (uint32_t)doc->page.footer & 0xFFFF );
    if ( doc->page.first_page != 1 ) {
        sp8( w->wd, SPRM_SFPGNRESTART, 1 );
        sp16( w->wd, SPRM_SPGNSTART97, doc->page.first_page );
    }
    start = out_tell( w->wd );
    out_seek( w->wd, sepx_at );
    out_le16( w->wd, start - sepx_at - 2 );
    out_seek( w->wd, start );
    out_fill( w->wd, 0, (FKP_LEN - start % FKP_LEN) % FKP_LEN );
    dw_fkps( w, 0, &chp_bin );
    dw_fkps( w, 1, &pap_bin );
    wd_len = out_tell( w->wd );

    /* the table stream */
    dw_styles( w );
    dw_notes( w, PART_FTN, FIB_FNDREF, FIB_FNDTXT );
    start = dw_piece( w );
    out_le32( w->tbl, 0 );
    out_le32( w->tbl, w->total );
    out_le16( w->tbl, 0 );
    out_le32( w->tbl, sepx_at );
    out_le16( w->tbl, 0 );
    out_le32( w->tbl, 0xFFFFFFFFUL );
    dw_piece_end( w, FIB_SED, start );
    if ( w->part[PART_HDD].len ) {
        start = dw_piece( w );
        dw_cps( w->tbl, w->hdd_at, 12 );
        out_le32( w->tbl, w->part[PART_HDD].len - 1 );
        out_le32( w->tbl, w->part[PART_HDD].len );
        dw_piece_end( w, FIB_HDD, start );
    }
    dw_bin( w, &chp_bin, FIB_BTECHPX );
    dw_bin( w, &pap_bin, FIB_BTEPAPX );
    dw_fonts( w );
    dw_fields( w, PART_MAIN, FIB_FLDMOM );
    dw_fields( w, PART_HDD, FIB_FLDHDR );
    dw_fields( w, PART_FTN, FIB_FLDFTN );
    dw_dop( w );
    start = dw_piece( w );
    out_byte( w->tbl, 2 );
    out_le32( w->tbl, 16 );
    out_le32( w->tbl, 0 );
    out_le32( w->tbl, w->total );
    out_le16( w->tbl, 0 );
    out_le32( w->tbl, w->wide ? TEXT_AT : (TEXT_AT * 2) | 0x40000000UL );
    out_le16( w->tbl, 0 );
    dw_piece_end( w, FIB_CLX, start );
    dw_notes( w, PART_EDN, FIB_ENDREF, FIB_ENDTXT );
    dw_fields( w, PART_EDN, FIB_FLDEDN );
    dw_lists( w );

    /* the FIB, now that everything it points at is where it is */
    fib = (uint8_t *)try_alloc( TEXT_AT );
    if ( fib != NULL ) {
        mem_set( fib, 0, TEXT_AT );
        put16( fib, 0xA5EC );
        put16( fib + 2, 0x00C1 );
        put16( fib + 6, 0x0409 );
        put16( fib + 0x0A, 0x1200 | (w->data != NULL ? 0x0008 : 0) );
        put16( fib + 0x0C, 0x00BF );
        fib[0x13] = 0x10;
        put32( fib + 0x18, TEXT_AT );
        put32( fib + 0x1C, text_end );
        put16( fib + 0x20, 14 );
        put16( fib + 0x3C, 0x0409 );
        put16( fib + 0x3E, 22 );
        put32( fib + 0x40, wd_len );
        put32( fib + 0x4C, w->part[PART_MAIN].len );
        put32( fib + 0x50, w->part[PART_FTN].len );
        put32( fib + 0x54, w->part[PART_HDD].len );
        put32( fib + 0x60, w->part[PART_EDN].len );
        put32( fib + 0x40 + 13 * 4, chp_bin.count );
        put32( fib + 0x40 + 16 * 4, pap_bin.count );
        put16( fib + 0x98, FIB_PAIRS );
        for ( index = 0; index < FIB_PAIRS; index++ ) {
            put32( fib + 0x9A + index * 8, w->pair[index][0] );
            put32( fib + 0x9A + index * 8 + 4, w->pair[index][1] );
        }
        out_seek( w->wd, 0 );
        out_put( w->wd, fib, TEXT_AT );
        out_seek( w->wd, wd_len );
        xfree( fib );
    } else {
        w->failed = 1;
    }
    xfree( chp_bin.fc );
    xfree( chp_bin.pn );
    xfree( pap_bin.fc );
    xfree( pap_bin.pn );

    if ( doc->title[0] || doc->subject[0] || doc->author[0] || doc->keywords[0] ) {
        summary = out_mem();
        if ( summary != NULL ) {
            dw_summary( w, summary );
            sum_bytes = out_data( summary, &sum_len );
        }
    }
    wd_bytes = out_data( w->wd, &wd_len );
    tbl_bytes = out_data( w->tbl, &tbl_len );
    if ( w->data != NULL ) {
        data_bytes = out_data( w->data, &data_len );
    }
    cfb = cfbw_new();
    if ( cfb != NULL && !w->failed && !out_error( w->wd ) && !out_error( w->tbl ) && !out_error( w->pool ) &&
         (w->data == NULL || !out_error( w->data )) && wd_bytes != NULL && tbl_bytes != NULL ) {
        cfbw_clsid( cfb, word_clsid );
        ok = cfbw_add( cfb, "WordDocument", wd_bytes, wd_len ) && cfbw_add( cfb, "1Table", tbl_bytes, tbl_len );
        if ( ok && data_bytes != NULL && data_len ) {
            ok = cfbw_add( cfb, "Data", data_bytes, data_len );
        }
        if ( ok && sum_bytes != NULL && sum_len && !out_error( summary ) ) {
            ok = cfbw_add( cfb, "\005SummaryInformation", sum_bytes, sum_len );
        }
        if ( ok ) {
            ok = cfbw_write( cfb, file );
        }
    }
    cfbw_free( cfb );
    if ( summary != NULL ) {
        out_close( summary );
    }
    if ( !ok ) {
        return WE_NOMEM;
    }
    return out_error( file ) ? WE_IO : WE_OK;
}

static int doc_save( WDOC *doc, OUT *out, int variant )
{
    DOCW *w;
    int rc;

    (void)variant;
    w = (DOCW *)try_alloc( sizeof( DOCW ) );
    if ( w == NULL ) {
        return WE_NOMEM;
    }
    mem_set( w, 0, sizeof( DOCW ) );
    w->doc = doc;
    rc = doc_write( w, out );
    dw_free( w );
    return rc;
}

const FORMAT fmt_doc = {
    FMT_DOC, "Word 97-2003 Document", "DOC;DOT", doc_detect, doc_load, doc_save, NULL, 0
};
