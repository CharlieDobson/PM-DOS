/*
 * F_WW2.C - Microsoft Word for Windows 1.x and 2.x: read only.
 *
 * THE FILE.  These are the Word files from before Word 6 put its file
 * inside a compound file.  A header (the FIB) of 384 bytes says where
 * everything is: the text, which is 8-bit Windows characters with CR LF
 * at each paragraph's end, and after it the tables, each found by a
 * place and a length in the header.  How the text looks is in pages of
 * 512 bytes, as in Word for DOS but twice over: a run of characters has
 * a CHPX, the properties that differ from its paragraph's style, and a
 * paragraph has a style's number and a list of changes to that style
 * (sprms).  A file saved "fast" has its text in pieces, listed in a
 * piece table, and a piece can carry changes of its own.
 *
 * TWO VERSIONS.  Word 1 (A59Bh) and Word 2 (A5DBh) differ in three
 * places that matter here: the CHPX's layout, where the page's size is
 * kept (Word 1: with the document's properties; Word 2: with the
 * section's), and one entry of the font table.  The header's list of
 * places is the same but for two that Word 2 added.
 *
 * WHY THERE IS NO WRITER.  Nothing that could be had describes this
 * format whole.  The layouts here were taken from Word 1.1's published
 * headers and from other readers, and corrected against five real
 * files where those disagreed (one reader's header list is short by an
 * entry; another's CHPX is a different edition's).  That is enough to
 * read with care and not enough to write a file Word 2 must accept:
 * the document properties past their first bytes, the paragraph-height
 * and page tables, and the print-setup blocks are not described
 * anywhere, and there is no Word 2 here to try a written file on.
 */
#include "fmt.h"

#define W2_PAGE         512         /* a page of formatting */
#define W2_STYLES       256
#define W2_STC_NONE     222         /* "based on nothing" */
#define W2_FIELDS       16          /* fields inside fields that are followed */
#define W2_NONE         0xFFFFFFFFUL

/* the header's places, by their number in a Word 2 file */
enum { W2_STSHF = 1, W2_FNDREF = 2, W2_FNDTXT = 3, W2_SED = 6, W2_HDD = 11, W2_BTECHPX = 12,
       W2_BTEPAPX = 13, W2_FFN = 15, W2_FLDMOM = 16, W2_FLDHDR = 17, W2_FLDFTN = 18, W2_DOP = 31,
       W2_ASSOC = 32, W2_CLX = 33 };

/* how many bytes follow each sprm: 7 a length byte and that many, 8 a
   length word and one fewer, 9 one that is not known - the list ends */
static const u8 w2_sprm_size[165] = {
    0, 9, 1, 7, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 7, 2, 2, 2, 2,      /*   0 */
    2, 2, 2, 7, 1, 1, 2, 2, 2, 1,  2, 2, 2, 2, 2, 2, 2, 1, 2, 2,      /*  20 */
    2, 2, 2, 2, 1, 2, 2, 2, 2, 2,  1, 1, 0, 1, 1, 1, 9, 7, 0, 9,      /*  40 */
    1, 1, 1, 1, 1, 1, 1, 1, 2, 1,  3, 2, 2, 1, 1, 1, 1, 1, 7, 9,      /*  60 */
    1, 1, 2, 2, 1, 1, 1, 1, 9, 9,  9, 9, 9, 9, 1, 7, 2, 2, 2, 2,      /*  80 */
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9,  9, 9, 1, 9, 1, 2, 2, 1, 1, 2,      /* 100 */
    2, 1, 1, 2, 2, 1, 1, 1, 1, 2,  2, 2, 2, 1, 1, 2, 2, 1, 1, 2,      /* 120 */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 1,  9, 9, 8, 2, 8, 7, 9, 5, 4, 2,      /* 140 */
    4, 2, 2, 5, 4                                                      /* 160 */
};

static const u32 w2_colors[17] = {
    COLOR_AUTO, 0x000000UL, 0x0000FFUL, 0x00FFFFUL, 0x00FF00UL, 0xFF00FFUL, 0xFF0000UL, 0xFFFF00UL,
    0xFFFFFFUL, 0x000080UL, 0x008080UL, 0x008000UL, 0x800080UL, 0x800000UL, 0x808000UL, 0x808080UL,
    0xC0C0C0UL
};

typedef struct {
    u32 cp, cp_end;     /* the characters it holds */
    u32 fc;             /* where in the file they are */
    u32 prm;            /* the changes it carries */
} W2PIECE;

/* character properties while they are worked out */
typedef struct {
    CHP chp;
    u8  spec;           /* a special character: a picture, a field's mark, a note's number */
    u8  gone;           /* deleted while changes were marked: left out */
    u16 spare;
    u32 pic;            /* where the picture is, for the character that is one */
} W2CHP;

/* ...and paragraph properties */
typedef struct {
    PAP  pap;           /* its tabs are in "tabs" below, never its own block */
    WTAB tabs[MAX_TABS];
    u32  ntabs;
    u8   stc;
    u8   in_table;
    u8   ttp;           /* the mark that ends a table's row */
    u8   has_row;
    TROW row;
} W2PAP;

/* what a section's sprms say */
typedef struct {
    s32 width, height, left, right, top, bottom, header, footer;
    u32 first_page;
    u32 grpf;           /* which running heads it has */
    u32 bkc;            /* how it starts: 0 on the same page */
    u32 columns;
} W2SEP;

typedef struct {
    u8  known;
    u8  base;
    u8  style;          /* the document's style it became */
    u8  spare;
    const u8 *chpx;
    u32 chpx_len;
    const u8 *papx;     /* its sprms */
    u32 papx_len;
} W2STYLE;

typedef struct {
    u16 kind;           /* 0 one kept as its result; a WCH_* field; 1 a symbol */
    u8  result;         /* its separator has been met */
    u8  spare;
} W2FIELD;

typedef struct {
    const u8 *data;
    u32   len;
    WDOC *doc;
    WBUILD wb;
    int   ww1;
    u32   ccp_text, ccp_ftn, ccp_hdr;
    u32   cp_max;       /* how many characters the pieces hold */
    W2PIECE *piece;
    u32   npiece;
    const u8 *clx;
    u32   clx_len;
    const u8 *bte[2];   /* the bin tables: CHPX, PAPX */
    u32   nbte[2];
    u32   pn_first[2], pn_count[2];     /* ...and the pages in a row they may not list */
    u32   run_from[2], run_to[2];       /* the run last found of each kind */
    const u8 *run_prop[2];
    u32   run_len[2];
    W2STYLE *style;
    W2PAP *xpap;        /* the paragraph in hand's properties */
    u16  *font;
    u32   nfont;
    const u8 *sed;      /* the section table: CPs, then where each one's sprms are */
    u32   nsed;
    const u8 *fnd_ref;  /* the notes: where each mark is, where each text is */
    const u8 *fnd_txt;
    u32   nnote, ref_next;
    int  *note_obj;
    const u8 *fld;      /* the story in hand's field table */
    u32   nfld, fld_next, fld_base;
    W2FIELD field[W2_FIELDS];
    u32   nfield, over, mute;
    char  code[64];     /* the innermost field's instructions */
    u32   code_len;
} W2CTX;

static void w2_sprms( W2CTX *ctx, const u8 *grpprl, u32 len, W2PAP *pap, W2CHP *chp, const W2CHP *base,
                      W2SEP *sep );

static s32 w2_s16( const u8 *ptr )
{
    u32 value = LE16( ptr );

    return value >= 0x8000UL ? (s32)value - 0x10000L : (s32)value;
}

/* one of the header's places -> 1 if there is something there */
static int w2_pair( const W2CTX *ctx, u32 which, u32 *fc, u32 *lcb )
{
    u32 at;

    if ( ctx->ww1 && which >= 27 ) {
        /* Word 1 has one print-setup block where Word 2 has three */
        if ( which < 30 ) {
            return 0;
        }
        which -= 2;
    }
    at = 0x58 + 6 * which;
    *fc = LE32( ctx->data + at );
    *lcb = LE16( ctx->data + at + 4 );
    return *lcb != 0 && IN_RANGE( *fc, *lcb, ctx->len );
}

static int w2_detect( const u8 *data, u32 len, const char *ext )
{
    u32 ident, fib, fc_min, fc_mac;

    (void)ext;
    if ( len < 0x180 ) {
        return 0;
    }
    ident = LE16( data );
    fib = LE16( data + 2 );
    fc_min = LE32( data + 0x18 );
    fc_mac = LE32( data + 0x1C );
    if ( (ident != 0xA59B && ident != 0xA5DB) || fib < 20 || fib > 99 ) {
        return 0;                       /* 101 and up is Word 6, in its compound file */
    }
    if ( fc_min < 0x100 || fc_mac < fc_min || fc_mac > len ) {
        return 0;
    }
    return 100;
}

/* ---- where the text is ---- */

static int w2_pieces( W2CTX *ctx, u32 fc_min, u32 fc_mac, int complex )
{
    const u8 *body = NULL, *pcd;
    u32 pos = 0, size = 0, count = 0, index, cp, cp_end, fc;

    if ( complex && ctx->clx != NULL ) {
        while ( pos + 3 <= ctx->clx_len ) {
            size = LE16( ctx->clx + pos + 1 );
            if ( ctx->clx[pos] == 2 ) {
                body = ctx->clx + pos + 3;
                if ( size > ctx->clx_len - pos - 3 ) {
                    size = ctx->clx_len - pos - 3;
                }
                break;
            }
            if ( ctx->clx[pos] != 1 ) {
                break;
            }
            pos += 3 + size;            /* a list of changes: met again by its number */
        }
        if ( body != NULL && size >= 16 ) {
            count = (size - 4) / 12;
        }
    }
    ctx->piece = (W2PIECE *)try_alloc( (count ? count : 1) * sizeof( W2PIECE ) );
    if ( ctx->piece == NULL ) {
        return 0;
    }
    if ( count == 0 ) {
        ctx->piece[0].cp = 0;
        ctx->piece[0].cp_end = fc_mac - fc_min;
        ctx->piece[0].fc = fc_min;
        ctx->piece[0].prm = 0;
        ctx->npiece = 1;
        ctx->cp_max = fc_mac - fc_min;
        return 1;
    }
    /* the pieces follow one another from the first character, and all
       of them together hold no more than the file has bytes: a table
       that says otherwise is cut where it starts to */
    for ( index = 0; index < count; index++ ) {
        cp = LE32( body + 4 * index );
        cp_end = LE32( body + 4 * index + 4 );
        pcd = body + 4 * (count + 1) + 8 * index;
        fc = LE32( pcd + 2 );
        if ( cp != ctx->cp_max || cp_end <= cp || fc >= ctx->len || cp_end - cp > ctx->len - fc ||
             cp_end > ctx->len ) {
            break;
        }
        ctx->piece[ctx->npiece].cp = cp;
        ctx->piece[ctx->npiece].cp_end = cp_end;
        ctx->piece[ctx->npiece].fc = fc;
        ctx->piece[ctx->npiece].prm = LE16( pcd + 6 );
        ctx->npiece++;
        ctx->cp_max = cp_end;
    }
    if ( ctx->npiece == 0 ) {
        /* a piece table with nothing in it that can be used: the text
           as the header says it lies */
        ctx->piece[0].cp = 0;
        ctx->piece[0].cp_end = fc_mac - fc_min;
        ctx->piece[0].fc = fc_min;
        ctx->piece[0].prm = 0;
        ctx->npiece = 1;
        ctx->cp_max = fc_mac - fc_min;
    }
    return 1;
}

/* where the character "cp" is in the file; "*hint" is the piece it was
   last found in, and is set to the one it is in */
static u32 w2_fc( const W2CTX *ctx, u32 cp, u32 *hint )
{
    u32 low = 0, high = ctx->npiece, mid;

    if ( *hint < ctx->npiece && cp >= ctx->piece[*hint].cp && cp < ctx->piece[*hint].cp_end ) {
        return ctx->piece[*hint].fc + (cp - ctx->piece[*hint].cp);
    }
    while ( low < high ) {
        mid = (low + high) / 2;
        if ( cp < ctx->piece[mid].cp ) {
            high = mid;
        } else if ( cp >= ctx->piece[mid].cp_end ) {
            low = mid + 1;
        } else {
            *hint = mid;
            return ctx->piece[mid].fc + (cp - ctx->piece[mid].cp);
        }
    }
    return W2_NONE;
}

/* ---- the pages of formatting ---- */

/* is "fc" in this page's runs?  -> the page, or NULL */
static const u8 *w2_fkp( const W2CTX *ctx, u32 pn, u32 fc )
{
    const u8 *page;
    u32 crun;

    if ( pn >= ctx->len / W2_PAGE ) {
        return NULL;
    }
    page = ctx->data + pn * W2_PAGE;
    crun = page[W2_PAGE - 1];
    if ( crun == 0 || 5 * crun + 4 > W2_PAGE - 1 ) {
        return NULL;
    }
    return fc >= LE32( page ) && fc < LE32( page + 4 * crun ) ? page : NULL;
}

/* the run that holds "fc", of character properties ("kind" 0) or of
   paragraph properties (1): ctx->run_*[kind] */
static void w2_run( W2CTX *ctx, u32 fc, int kind )
{
    const u8 *page = NULL, *bte = ctx->bte[kind];
    u32 count = ctx->nbte[kind], index, crun, off, size;

    if ( fc >= ctx->run_from[kind] && fc < ctx->run_to[kind] ) {
        return;
    }
    for ( index = 0; index < count && page == NULL; index++ ) {
        if ( fc >= LE32( bte + 4 * index ) && fc < LE32( bte + 4 * index + 4 ) ) {
            page = w2_fkp( ctx, LE16( bte + 4 * (count + 1) + 2 * index ), fc );
        }
    }
    /* a file saved whole may list only the first page: the rest follow it */
    for ( index = 0; index < ctx->pn_count[kind] && ctx->pn_first[kind] + index < ctx->len / W2_PAGE &&
          page == NULL; index++ ) {
        page = w2_fkp( ctx, ctx->pn_first[kind] + index, fc );
    }
    ctx->run_prop[kind] = NULL;
    ctx->run_len[kind] = 0;
    if ( page == NULL ) {
        /* nothing says how this looks: standard, for a stretch long
           enough that a damaged file is not searched byte by byte */
        ctx->run_from[kind] = fc;
        ctx->run_to[kind] = fc + 4096;
        return;
    }
    crun = page[W2_PAGE - 1];
    for ( index = 0; index + 1 < crun && fc >= LE32( page + 4 * index + 4 ); index++ ) {
    }
    ctx->run_from[kind] = LE32( page + 4 * index );
    ctx->run_to[kind] = LE32( page + 4 * index + 4 );
    if ( ctx->run_to[kind] <= fc ) {
        ctx->run_from[kind] = fc;       /* a page whose runs are out of order */
        ctx->run_to[kind] = fc + 1;
        return;
    }
    off = 2 * (u32)page[4 * (crun + 1) + index];
    if ( off == 0 || off >= W2_PAGE - 1 ) {
        return;
    }
    size = page[off];
    if ( kind ) {
        size *= 2;                      /* a PAPX counts words */
    }
    if ( size > W2_PAGE - 1 - (off + 1) ) {
        size = W2_PAGE - 1 - (off + 1);
    }
    ctx->run_prop[kind] = page + off + 1;
    ctx->run_len[kind] = size;
}

/* ---- properties ---- */

static u16 w2_font( const W2CTX *ctx, u32 ftc )
{
    return ctx->font != NULL && ftc < ctx->nfont ? ctx->font[ftc] : 0;
}

static void w2_toggle( W2CHP *chp, u16 flag, int on )
{
    if ( on ) {
        chp->chp.flags ^= flag;
    }
}

static void w2_underline( W2CHP *chp, u32 kul )
{
    chp->chp.flags &= ~(CF_UNDER | CF_WUNDER | CF_DUNDER);
    if ( kul == 2 ) {
        chp->chp.flags |= CF_WUNDER;
    } else if ( kul == 3 ) {
        chp->chp.flags |= CF_DUNDER;
    } else if ( kul ) {
        chp->chp.flags |= CF_UNDER;
    }
}

static void w2_position( W2CHP *chp, u32 pos )
{
    chp->chp.flags &= ~(CF_SUPER | CF_SUB);
    if ( pos ) {
        chp->chp.flags |= pos < 128 ? CF_SUPER : CF_SUB;
    }
}

/* a CHPX laid over the properties it is a change to: its flags say
   "not what the style has", and its other fields count only where a
   bit says they are set */
static void w2_chpx( W2CTX *ctx, const u8 *chpx, u32 len, W2CHP *chp )
{
    u8 raw[24];
    u32 sets, ftc, hps, pos, ico, kul;

    if ( chpx == NULL || len == 0 ) {
        return;
    }
    mem_set( raw, 0, sizeof( raw ) );
    mem_cpy( raw, chpx, len < sizeof( raw ) ? len : sizeof( raw ) );
    w2_toggle( chp, CF_BOLD, raw[0] & 0x01 );
    w2_toggle( chp, CF_ITALIC, raw[0] & 0x02 );
    w2_toggle( chp, CF_OUTLINE, raw[0] & 0x08 );
    w2_toggle( chp, CF_SMALLCAPS, raw[0] & 0x20 );
    w2_toggle( chp, CF_ALLCAPS, raw[0] & 0x40 );
    w2_toggle( chp, CF_HIDDEN, raw[0] & 0x80 );
    if ( raw[1] & 0x01 ) {
        ctx->doc->lost |= LOST_REVISIONS;       /* typed while changes were marked: kept */
    }
    chp->spec = (u8)((raw[1] & 0x02) != 0);
    if ( ctx->ww1 ) {
        w2_toggle( chp, CF_STRIKE, raw[0] & 0x04 );
        sets = raw[1] >> 2;
        ftc = LE16( raw + 2 );
        hps = raw[4];
        pos = raw[5];
        ico = raw[7] & 0x0F;
        kul = (raw[7] >> 4) & 7;
        chp->pic = LE32( raw + 8 );
    } else {
        if ( raw[0] & 0x04 ) {
            chp->gone ^= 1;
            ctx->doc->lost |= LOST_REVISIONS;
        }
        w2_toggle( chp, CF_STRIKE, raw[1] & 0x04 );
        sets = raw[2];
        ftc = LE16( raw + 4 );
        hps = raw[6];
        pos = raw[10];
        ico = raw[9] & 0x1F;
        kul = raw[9] >> 5;
        /* the picture's place is after the language; an edition with
           right-to-left text has six bytes more before it */
        chp->pic = len >= 24 ? LE32( raw + 20 ) : LE32( raw + 14 );
    }
    if ( sets & 0x01 ) {
        chp->chp.color = w2_colors[ico <= 16 ? ico : 0];
    }
    if ( sets & 0x02 ) {
        chp->chp.font = w2_font( ctx, ftc );
    }
    if ( (sets & 0x04) && hps ) {
        chp->chp.size = (u16)hps;
    }
    if ( sets & 0x08 ) {
        w2_underline( chp, kul );
    }
    if ( sets & 0x10 ) {
        w2_position( chp, pos );
    }
}

/* a sprm that sets a character flag: 0 off, 1 on, 128 as the style has
   it, 129 not as the style has it */
static void w2_flag( W2CHP *chp, const W2CHP *base, u16 flag, u32 value )
{
    int on = (value & 1) != 0;

    if ( value & 0x80 ) {
        on ^= base != NULL && (base->chp.flags & flag) != 0;
    }
    chp->chp.flags = (u16)(on ? chp->chp.flags | flag : chp->chp.flags & ~flag);
}

static void w2_border( W2PAP *pap, u16 flag, const u8 *arg )
{
    pap->pap.flags = (u16)(LE16( arg ) ? pap->pap.flags | flag : pap->pap.flags & ~flag);
}

/* tab stops taken away and tab stops set, on top of the style's */
static void w2_tabs( W2PAP *pap, const u8 *arg, u32 len )
{
    u32 gone, added, index, at, pos = 0, kind;
    s32 where;

    if ( len < 1 ) {
        return;
    }
    gone = arg[pos++];
    if ( 2 * gone > len - pos ) {
        return;
    }
    for ( index = 0; index < gone; index++ ) {
        where = w2_s16( arg + pos + 2 * index );
        for ( at = 0; at < pap->ntabs; at++ ) {
            if ( pap->tabs[at].pos == where ) {
                pap->tabs[at] = pap->tabs[--pap->ntabs];
                break;
            }
        }
    }
    pos += 2 * gone;
    if ( pos >= len ) {
        return;
    }
    added = arg[pos++];
    if ( 3 * added > len - pos ) {
        return;
    }
    for ( index = 0; index < added; index++ ) {
        where = w2_s16( arg + pos + 2 * index );
        kind = arg[pos + 2 * added + index];
        if ( (kind & 7) > 3 || where <= 0 ) {
            continue;                   /* a bar down the page is not a stop */
        }
        for ( at = 0; at < pap->ntabs && pap->tabs[at].pos != where; at++ ) {
        }
        if ( at == pap->ntabs ) {
            if ( pap->ntabs == MAX_TABS ) {
                continue;
            }
            pap->ntabs++;
        }
        mem_set( &pap->tabs[at], 0, sizeof( WTAB ) );
        pap->tabs[at].pos = where;
        pap->tabs[at].kind = (u8)(kind & 7);            /* TAB_*'s order */
        pap->tabs[at].lead = (u8)((kind >> 3) & 3);     /* LEAD_*'s order */
    }
}

/* a table row's cells: how many, and where each one's edge is */
static void w2_row( W2PAP *pap, const u8 *arg, u32 len )
{
    u32 cells, index;

    if ( len < 3 ) {
        return;
    }
    cells = arg[0];
    if ( cells > MAX_CELLS ) {
        cells = MAX_CELLS;
    }
    if ( 2 * (cells + 1) > len - 1 ) {
        cells = (len - 1) / 2 - 1;
    }
    mem_set( &pap->row, 0, sizeof( TROW ) );
    pap->row.ncells = (u8)cells;
    pap->row.left = w2_s16( arg + 1 );
    for ( index = 0; index < cells; index++ ) {
        pap->row.edge[index] = w2_s16( arg + 3 + 2 * index );
    }
    pap->has_row = 1;
}

static void w2_sprm( W2CTX *ctx, u32 op, const u8 *arg, u32 len, W2PAP *pap, W2CHP *chp,
                     const W2CHP *base, W2SEP *sep )
{
    u32 value = len >= 2 ? LE16( arg ) : len ? arg[0] : 0;
    s32 line;

    if ( pap != NULL ) {
        switch ( op ) {
        case 5:
            pap->pap.align = (u8)(value & 3);   /* left, centered, right, both: AL_*'s order */
            break;
        case 7:
            pap->pap.flags = (u16)(value ? pap->pap.flags | PF_KEEP : pap->pap.flags & ~PF_KEEP);
            break;
        case 8:
            pap->pap.flags = (u16)(value ? pap->pap.flags | PF_KEEPNEXT : pap->pap.flags & ~PF_KEEPNEXT);
            break;
        case 9:
            pap->pap.flags = (u16)(value ? pap->pap.flags | PF_PAGEBEFORE : pap->pap.flags & ~PF_PAGEBEFORE);
            break;
        case 15:
            w2_tabs( pap, arg, len );
            break;
        case 16:
            pap->pap.right = w2_s16( arg );
            break;
        case 17:
            pap->pap.left = w2_s16( arg );
            break;
        case 18:
            pap->pap.left += w2_s16( arg );
            break;
        case 19:
            pap->pap.first = w2_s16( arg );
            break;
        case 20:
            /* a height: at least so much, or - below zero - exactly */
            line = w2_s16( arg );
            pap->pap.line_rule = (u8)(line == 0 ? LS_MULTIPLE : line > 0 ? LS_ATLEAST : LS_EXACT);
            pap->pap.line = (u16)(line == 0 ? 240 : line > 0 ? line : -line);
            break;
        case 21:
            pap->pap.before = (u16)value;
            break;
        case 22:
            pap->pap.after = (u16)value;
            break;
        case 24:
            pap->in_table = (u8)(value != 0);
            break;
        case 25:
            pap->ttp = (u8)(value != 0);
            break;
        case 30:
        case 38:
            w2_border( pap, PF_BORDER_TOP, arg );
            break;
        case 31:
        case 39:
            w2_border( pap, PF_BORDER_LEFT, arg );
            break;
        case 32:
        case 40:
            w2_border( pap, PF_BORDER_BOT, arg );
            break;
        case 33:
        case 41:
            w2_border( pap, PF_BORDER_RIGHT, arg );
            break;
        case 26:
        case 27:
        case 28:
            ctx->doc->lost |= LOST_LAYOUT;      /* set at a place on the page */
            break;
        case 152:
        case 154:
            w2_row( pap, arg, len );
            break;
        default:
            break;
        }
    }
    if ( chp != NULL ) {
        switch ( op ) {
        case 60:
            w2_flag( chp, base, CF_BOLD, value );
            break;
        case 61:
            w2_flag( chp, base, CF_ITALIC, value );
            break;
        case 62:
            w2_flag( chp, base, CF_STRIKE, value );
            break;
        case 63:
            w2_flag( chp, base, CF_OUTLINE, value );
            break;
        case 65:
            w2_flag( chp, base, CF_SMALLCAPS, value );
            break;
        case 66:
            w2_flag( chp, base, CF_ALLCAPS, value );
            break;
        case 67:
            w2_flag( chp, base, CF_HIDDEN, value );
            break;
        case 68:
            chp->chp.font = w2_font( ctx, value );
            break;
        case 69:
            w2_underline( chp, value );
            break;
        case 73:
            chp->chp.color = w2_colors[value <= 16 ? value : 0];
            break;
        case 74:
            if ( value & 0xFF ) {
                chp->chp.size = (u16)(value & 0xFF);
            }
            break;
        case 76:
            w2_position( chp, value & 0xFF );
            break;
        default:
            break;
        }
    }
    if ( sep != NULL ) {
        switch ( op ) {
        case 117:
            sep->bkc = value;
            break;
        case 119:
            sep->columns = value + 1;
            break;
        case 128:
            sep->grpf = value;
            break;
        case 131:
            sep->header = (s32)value;
            break;
        case 132:
            sep->footer = (s32)value;
            break;
        case 136:
            sep->first_page = value;
            break;
        case 139:
            sep->width = (s32)value;
            break;
        case 140:
            sep->height = (s32)value;
            break;
        case 141:
            sep->left = (s32)value;
            break;
        case 142:
            sep->right = (s32)value;
            break;
        case 143:
            sep->top = (s32)value;
            break;
        case 144:
            sep->bottom = (s32)value;
            break;
        default:
            break;
        }
    }
}

/* a list of sprms: each a number and its bytes */
static void w2_sprms( W2CTX *ctx, const u8 *grpprl, u32 len, W2PAP *pap, W2CHP *chp, const W2CHP *base,
                      W2SEP *sep )
{
    u32 pos = 0, op, size;

    while ( grpprl != NULL && pos < len ) {
        op = grpprl[pos++];
        if ( op == 0 ) {
            continue;
        }
        size = op < sizeof( w2_sprm_size ) ? w2_sprm_size[op] : 9;
        if ( size == 9 ) {
            return;
        }
        if ( size == 7 ) {
            if ( pos >= len ) {
                return;
            }
            size = grpprl[pos++];
            if ( size == 255 ) {
                return;                 /* a tab list too long to count this way */
            }
        } else if ( size == 8 ) {
            if ( len - pos < 2 ) {
                return;
            }
            size = LE16( grpprl + pos );
            pos += 2;
            if ( size == 0 ) {
                return;
            }
            size--;
        }
        if ( size > len - pos ) {
            /* cut off by the end: a row's cells may still be read from
               what there is, anything else is dropped */
            if ( op != 152 && op != 154 ) {
                return;
            }
            size = len - pos;
        }
        w2_sprm( ctx, op, grpprl + pos, size, pap, chp, base, sep );
        pos += size;
    }
}

/* the changes a piece carries: one sprm with one byte, or the number of
   a list kept with the piece table */
static void w2_prm( W2CTX *ctx, u32 prm, W2PAP *pap, W2CHP *chp, const W2CHP *base )
{
    u8 one[2];
    u32 pos = 0, want, size;

    if ( prm == 0 ) {
        return;
    }
    if ( !(prm & 1) ) {
        one[0] = (u8)((prm >> 1) & 0x7F);
        one[1] = (u8)(prm >> 8);
        if ( w2_sprm_size[one[0]] == 1 ) {
            w2_sprms( ctx, one, 2, pap, chp, base, NULL );
        }
        return;
    }
    want = prm >> 1;
    while ( ctx->clx != NULL && pos + 3 <= ctx->clx_len && ctx->clx[pos] == 1 ) {
        size = LE16( ctx->clx + pos + 1 );
        if ( size > ctx->clx_len - pos - 3 ) {
            return;
        }
        if ( want == 0 ) {
            w2_sprms( ctx, ctx->clx + pos + 3, size, pap, chp, base, NULL );
            return;
        }
        want--;
        pos += 3 + size;
    }
}

/* what a style gives a paragraph and its text: its base's, then its own */
static void w2_style_props( W2CTX *ctx, u32 stc, W2PAP *pap, W2CHP *chp )
{
    u8 chain[16];
    u32 depth = 0;
    const W2STYLE *style;

    pap_init( &pap->pap );
    pap->ntabs = 0;
    pap->stc = (u8)stc;
    pap->in_table = pap->ttp = pap->has_row = 0;
    mem_set( chp, 0, sizeof( W2CHP ) );
    chp->chp.font = w2_font( ctx, 0 );
    chp->chp.size = 20;                 /* these versions start at 10 point */
    chp->chp.color = COLOR_AUTO;
    while ( depth < sizeof( chain ) && stc != W2_STC_NONE && stc < W2_STYLES && ctx->style[stc].known ) {
        chain[depth++] = (u8)stc;
        stc = ctx->style[stc].base;
    }
    while ( depth ) {
        style = &ctx->style[chain[--depth]];
        w2_sprms( ctx, style->papx, style->papx_len, pap, NULL, NULL, NULL );
        w2_chpx( ctx, style->chpx, style->chpx_len, chp );
        chp->spec = 0;
        chp->gone = 0;
    }
    pap->pap.style = ctx->style[pap->stc].style;
}

/* the paragraph whose mark is at "fc", in piece "piece": its style, the
   changes stored with it, the changes its piece carries */
static void w2_para_props( W2CTX *ctx, u32 fc, u32 piece, W2PAP *pap, W2CHP *base )
{
    const u8 *papx;
    u32 len, stc = 0;

    w2_run( ctx, fc, 1 );
    papx = ctx->run_prop[1];
    len = ctx->run_len[1];
    if ( papx != NULL && len >= 1 ) {
        stc = papx[0];
    }
    w2_style_props( ctx, stc, pap, base );
    if ( papx != NULL && len > 7 ) {
        w2_sprms( ctx, papx + 7, len - 7, pap, NULL, NULL, NULL );     /* past the style and a height */
    }
    w2_prm( ctx, ctx->piece[piece].prm, pap, NULL, NULL );
}

/* ---- the tables read once ---- */

static const char *w2_style_name( u32 stc, int *kind )
{
    static const char *const heads[9] = {
        "Heading 1", "Heading 2", "Heading 3", "Heading 4", "Heading 5", "Heading 6", "Heading 7",
        "Heading 8", "Heading 9"
    };

    *kind = STY_OTHER;
    if ( stc == 0 ) {
        *kind = STY_NORMAL;
        return "Normal";
    }
    if ( stc >= 246 && stc <= 254 ) {
        if ( stc >= 249 ) {
            *kind = STY_HEADING1 + (int)(254 - stc);
        }
        return heads[254 - stc];
    }
    switch ( stc ) {
    case 245:
        *kind = STY_FOOTNOTE;
        return "Footnote Text";
    case 243:
        *kind = STY_HEADER;
        return "Header";
    case 242:
        *kind = STY_FOOTER;
        return "Footer";
    case 255:
        return "Normal Indent";
    case 244:
        return "Footnote Reference";
    default:
        break;
    }
    return NULL;
}

/* the style sheet: three lists side by side - names, CHPXs, PAPXs -
   and a fourth of what each is based on.  A style's number is its
   place in the lists less the count of built-in ones, so that the
   built-in ones count down from 255. */
static void w2_styles( W2CTX *ctx )
{
    char name[STYLE_NAME_LEN];
    const u8 *sheet;
    const char *given;
    W2PAP *pap = ctx->xpap;
    W2CHP chp;
    u32 fc, lcb, std, pos, end, index, size, count, stc, used, at;
    int kind, style;

    if ( !w2_pair( ctx, W2_STSHF, &fc, &lcb ) || lcb < 4 ) {
        return;
    }
    sheet = ctx->data + fc;
    std = LE16( sheet );

    /* names */
    pos = 2;
    size = LE16( sheet + pos );
    end = pos + size <= lcb ? pos + size : lcb;
    pos += 2;
    for ( index = 0; pos < end && index < W2_STYLES; index++ ) {
        stc = (index - std) & 255;
        size = sheet[pos++];
        if ( size == 0xFF ) {
            continue;
        }
        if ( size > end - pos ) {
            size = end - pos;
        }
        ctx->style[stc].known = 1;
        ctx->style[stc].base = W2_STC_NONE;
        given = w2_style_name( stc, &kind );
        if ( size ) {
            used = 0;
            for ( at = 0; at < size && used < sizeof( name ) - 1; at++ ) {
                name[used++] = (char)(sheet[pos + at] >= 0x20 && sheet[pos + at] < 0x7F ? sheet[pos + at] : '_');
            }
            name[used] = 0;
            given = name;
            kind = STY_OTHER;
        } else if ( given == NULL ) {
            sfmt( name, sizeof( name ), "Style %u", stc );
            given = name;
        }
        style = wd_style( ctx->doc, given, kind );
        ctx->style[stc].style = (u8)(style > 0 ? style : 0);
        pos += size;
    }
    count = index;

    /* CHPXs */
    pos = end;
    if ( lcb - pos < 2 ) {
        return;
    }
    size = LE16( sheet + pos );
    end = size <= lcb - pos ? pos + size : lcb;
    pos += 2;
    for ( index = 0; pos < end && index < count; index++ ) {
        stc = (index - std) & 255;
        size = sheet[pos++];
        if ( size == 0xFF ) {
            continue;
        }
        if ( size > end - pos ) {
            size = end - pos;
        }
        ctx->style[stc].chpx = sheet + pos;
        ctx->style[stc].chpx_len = size;
        pos += size;
    }

    /* PAPXs: a style's number, seven bytes with it, then its sprms */
    pos = end;
    if ( lcb - pos < 2 ) {
        return;
    }
    size = LE16( sheet + pos );
    end = size <= lcb - pos ? pos + size : lcb;
    pos += 2;
    for ( index = 0; pos < end && index < count; index++ ) {
        stc = (index - std) & 255;
        size = sheet[pos++];
        if ( size == 0xFF ) {
            continue;
        }
        if ( size > end - pos ) {
            size = end - pos;
        }
        if ( size > 7 ) {
            ctx->style[stc].papx = sheet + pos + 7;
            ctx->style[stc].papx_len = size - 7;
        }
        pos += size;
    }

    /* what each is based on */
    pos = end;
    if ( lcb - pos >= 2 ) {
        size = LE16( sheet + pos );
        pos += 2;
        for ( index = 0; index < size && index < count && lcb - pos >= 2; index++ ) {
            ctx->style[(index - std) & 255].base = sheet[pos + 1];
            pos += 2;
        }
    }

    /* the look each one starts a paragraph with */
    for ( stc = 0; stc < W2_STYLES; stc++ ) {
        if ( ctx->style[stc].known && (stc == 0 || ctx->style[stc].style != 0) ) {
            w2_style_props( ctx, stc, pap, &chp );
            style = ctx->style[stc].style;
            ctx->doc->style[style].chp = chp.chp;
            pap->pap.style = (u8)style;
            pap->pap.ntabs = 0;
            ctx->doc->style[style].pap = pap->pap;
        }
    }
}

/* the font names, in the order the font numbers count them */
static int w2_fonts( W2CTX *ctx )
{
    static const char *const usual[3] = { "Times New Roman", "Symbol", "Arial" };
    char name[FONT_NAME_LEN];
    const u8 *table = NULL;
    u32 fc, lcb = 0, pos, size, count = 0, used, at, lead = ctx->ww1 ? 2 : 3;
    int family, font;

    if ( w2_pair( ctx, W2_FFN, &fc, &lcb ) && lcb > 2 ) {
        table = ctx->data + fc;
        for ( pos = 2; pos < lcb; pos += 1 + table[pos] ) {
            count++;
        }
    }
    ctx->font = (u16 *)try_alloc( (count > 3 ? count : 3) * sizeof( u16 ) );
    if ( ctx->font == NULL ) {
        return 0;
    }
    /* with no table, the three every Windows had */
    for ( at = 0; at < 3; at++ ) {
        font = wd_font( ctx->doc, usual[at], FAM_ANY, -1 );
        ctx->font[at] = (u16)font;
    }
    ctx->nfont = 3;
    if ( count == 0 ) {
        return 1;
    }
    ctx->nfont = 0;
    for ( pos = 2; pos < lcb && ctx->nfont < count; pos += 1 + size ) {
        size = table[pos];
        if ( size > lcb - pos - 1 ) {
            size = lcb - pos - 1;
        }
        used = 0;
        for ( at = lead; at <= size && table[pos + at] != 0; at++ ) {
            if ( used < sizeof( name ) - 1 ) {
                name[used++] = (char)table[pos + at];
            }
        }
        name[used] = 0;
        /* the old names of the fonts Windows 3.1 renamed */
        if ( str_icmp( name, "Tms Rmn" ) == 0 || str_icmp( name, "TmsRmn" ) == 0 ) {
            str_cpy( name, "Times New Roman" );
        } else if ( str_icmp( name, "Helv" ) == 0 ) {
            str_cpy( name, "Arial" );
        }
        family = size >= 1 ? (table[pos + 1] >> 4) & 7 : 0;
        font = wd_font( ctx->doc, used ? name : usual[0],
                        family == 1 ? FAM_ROMAN : family == 2 ? FAM_SWISS : family == 3 ? FAM_MODERN :
                        family == 4 ? FAM_SCRIPT : family == 5 ? FAM_DECOR : FAM_ANY,
                        size >= 1 && (table[pos + 1] & 3) == 1 ? 1 : -1 );
        ctx->font[ctx->nfont++] = (u16)font;
    }
    return 1;
}

/* a section's sprms -> 1 if it has any stored */
static int w2_sepx( W2CTX *ctx, u32 index, W2SEP *sep )
{
    u32 fc, size;

    if ( index >= ctx->nsed ) {
        return 0;
    }
    fc = LE32( ctx->sed + 4 * (ctx->nsed + 1) + 6 * index + 2 );
    if ( fc >= ctx->len ) {
        return 0;                       /* FFFFFFFFh: a standard section */
    }
    size = ctx->data[fc];
    if ( size > ctx->len - fc - 1 ) {
        size = ctx->len - fc - 1;
    }
    w2_sprms( ctx, ctx->data + fc + 1, size, NULL, NULL, NULL, sep );
    return 1;
}

/* a line of the summary: the n-th string of a table of them */
static void w2_info( const W2CTX *ctx, const u8 *table, u32 lcb, u32 which, char *dst )
{
    u16 wide[INFO_LEN];
    u32 pos = 2, size, used = 0, index;

    while ( pos < lcb ) {
        size = table[pos++];
        if ( size > lcb - pos ) {
            size = lcb - pos;
        }
        if ( which == 0 ) {
            for ( index = 0; index < size && used < INFO_LEN - 1; index++ ) {
                wide[used++] = cp_to_uni( CP_1252, table[pos + index] );
            }
            utf8_copy( dst, INFO_LEN, wide, used );
            return;
        }
        which--;
        pos += size;
    }
    (void)ctx;
}

/* ---- pictures ---- */

/* the picture a picture character stands for: a header, then - of the
   kinds that can be kept - a Windows metafile */
static int w2_picture( W2CTX *ctx, u32 fc )
{
    const u8 *pic;
    u32 size, head, mode, scale;
    s32 width, height;

    if ( !IN_RANGE( fc, 46, ctx->len ) ) {
        return -1;
    }
    pic = ctx->data + fc;
    size = LE32( pic );
    head = LE16( pic + 4 );
    mode = LE16( pic + 6 );
    if ( size > ctx->len - fc || head < 46 || head >= size ) {
        return -1;
    }
    if ( mode == 99 || mode == 98 || size - head < 18 || LE16( pic + head + 2 ) != 9 ) {
        return -1;                      /* a bitmap of the screen's own kind, or a file's name */
    }
    scale = LE16( pic + 32 );
    width = (s32)(LE16( pic + 28 ) * (scale ? scale : 1000) / 1000);
    scale = LE16( pic + 34 );
    height = (s32)(LE16( pic + 30 ) * (scale ? scale : 1000) / 1000);
    if ( width == 0 || height == 0 ) {
        width = (s32)(LE16( pic + 8 ) * 72 / 127);      /* hundredths of a millimeter */
        height = (s32)(LE16( pic + 10 ) * 72 / 127);
    }
    return wd_image_new( ctx->doc, pic + head, size - head, IMG_WMF, width, height );
}

/* ---- the text ---- */

static void w2_put( W2CTX *ctx, u32 code )
{
    if ( ctx->mute == 0 && ctx->over == 0 ) {
        wb_char( &ctx->wb, code );
    }
}

/* the paragraph in hand ends, with these properties */
static void w2_par( W2CTX *ctx, const W2PAP *pap, u16 flags )
{
    PAP *dst = &ctx->wb.pap;

    pap_free( dst );
    *dst = pap->pap;
    dst->tabs = NULL;
    dst->row = NULL;
    dst->ntabs = 0;
    dst->flags |= flags;
    if ( pap->in_table ) {
        dst->flags |= PF_INTABLE;
    }
    if ( pap->ntabs && !pap_set_tabs( dst, pap->tabs, (int)pap->ntabs ) ) {
        ctx->doc->failed = 1;
    }
    wb_par( &ctx->wb );
}

/* what kind of field the instructions are for */
static u16 w2_field_kind( const W2CTX *ctx, u32 flt )
{
    static const struct {
        u8 flt;
        u16 kind;
        const char *word;
    } kinds[] = {
        { 33, WCH_PAGENUM, "PAGE" }, { 26, WCH_NUMPAGES, "NUMPAGES" }, { 31, WCH_DATE, "DATE" },
        { 32, WCH_TIME, "TIME" }, { 29, WCH_FILENAME, "FILENAME" }, { 57, 1, "SYMBOL" }
    };
    const char *code = ctx->code;
    u32 index, size;

    while ( *code == ' ' ) {
        code++;
    }
    for ( index = 0; index < sizeof( kinds ) / sizeof( kinds[0] ); index++ ) {
        size = str_len( kinds[index].word );
        if ( flt == kinds[index].flt ||
             (flt == 0 && str_nicmp( code, kinds[index].word, size ) == 0 && (u8)code[size] <= ' ') ) {
            return kinds[index].kind;
        }
    }
    return 0;
}

/* the field type the field table gives the mark at "cp"; 0 for none */
static u32 w2_field_flt( W2CTX *ctx, u32 cp )
{
    u32 where;

    while ( ctx->fld_next < ctx->nfld ) {
        where = LE32( ctx->fld + 4 * ctx->fld_next ) + ctx->fld_base;
        if ( where > cp ) {
            break;
        }
        ctx->fld_next++;
        if ( where == cp ) {
            return ctx->fld[4 * (ctx->nfld + 1) + 2 * (ctx->fld_next - 1) + 1];
        }
    }
    return 0;
}

/* a field's mark: 13h starts it, 14h parts its instructions from its
   result, 15h ends it.  The instructions are never shown; the result
   is, unless the field is one the document makes itself. */
static void w2_field( W2CTX *ctx, u32 ch, u32 cp )
{
    W2FIELD *field;
    u32 value = 0, index;
    int ok;

    if ( ch == 0x13 ) {
        if ( ctx->nfield == W2_FIELDS ) {
            ctx->over++;
            return;
        }
        field = &ctx->field[ctx->nfield++];
        field->kind = 0;
        field->result = 0;
        field->spare = (u8)w2_field_flt( ctx, cp );
        ctx->mute++;
        ctx->code_len = 0;
        ctx->code[0] = 0;
        return;
    }
    if ( ctx->over ) {
        if ( ch == 0x15 ) {
            ctx->over--;
        }
        return;
    }
    if ( ctx->nfield == 0 ) {
        return;
    }
    field = &ctx->field[ctx->nfield - 1];
    if ( !field->result ) {
        field->kind = w2_field_kind( ctx, field->spare );
    }
    if ( ch == 0x14 ) {
        if ( !field->result ) {
            field->result = 1;
            if ( field->kind == 0 ) {
                ctx->mute--;            /* its result is text like any other */
                ctx->doc->lost |= LOST_FIELDS;
            }
        }
        return;
    }
    if ( !field->result || field->kind != 0 ) {
        ctx->mute--;
    }
    ctx->nfield--;
    if ( field->kind == 1 ) {
        /* SYMBOL n: the character with that number; the bullet of the
           Symbol font is the one that is met */
        for ( index = 0; ctx->code[index] && (ctx->code[index] < '0' || ctx->code[index] > '9'); index++ ) {
        }
        ok = 0;
        for ( ; ctx->code[index] >= '0' && ctx->code[index] <= '9' && value < 0x10000UL; index++ ) {
            value = value * 10 + (u32)(ctx->code[index] - '0');
            ok = 1;
        }
        if ( ok && value == 183 ) {
            w2_put( ctx, 0x2022 );
        } else if ( ok && value >= 0x20 && value < 0x100 ) {
            w2_put( ctx, cp_to_uni( CP_1252, (int)value ) );
        }
    } else if ( field->kind ) {
        w2_put( ctx, field->kind );
    }
    ctx->code_len = 0;
    ctx->code[0] = 0;
}

/* the mark that ends the paragraph "cp" is in.  A paragraph's end is
   two characters: CR LF, or in a table CR and the cell mark; the second
   is the mark.  A section's or a page's end is one. */
static u32 w2_para_mark( const W2CTX *ctx, u32 cp, u32 cp_end, u32 *hint )
{
    u32 fc;
    int ch;

    for ( ; cp + 1 < cp_end; cp++ ) {
        fc = w2_fc( ctx, cp, hint );
        if ( fc == W2_NONE ) {
            continue;
        }
        ch = ctx->data[fc];
        if ( ch == 0x07 || ch == 0x0C ) {
            break;
        }
        if ( ch == 0x0D ) {
            fc = w2_fc( ctx, cp + 1, hint );
            if ( fc != W2_NONE && (ctx->data[fc] == 0x0A || ctx->data[fc] == 0x07) ) {
                cp++;
            }
            break;
        }
    }
    return cp;
}

/* does the section mark at "cp" start a page?  It does unless the
   section after it says it runs on, or nothing is after it. */
static int w2_page_after( W2CTX *ctx, u32 cp, u32 cp_end )
{
    W2SEP sep;
    u32 index;

    if ( cp + 1 >= cp_end ) {
        return 0;
    }
    for ( index = 1; index <= ctx->nsed; index++ ) {
        if ( LE32( ctx->sed + 4 * index ) == cp + 1 ) {
            mem_set( &sep, 0, sizeof( sep ) );
            sep.bkc = 2;
            w2_sepx( ctx, index, &sep );
            if ( sep.columns > 1 ) {
                ctx->doc->lost |= LOST_LAYOUT;
            }
            return sep.bkc != 0;
        }
    }
    return 1;                           /* a page break that is only that */
}

/* a story's text, from "cp" to "cp_end": the body ("kind" 0), a note
   (1), a running head (2) */
static void w2_story( W2CTX *ctx, STORY *story, u32 cp, u32 cp_end, int kind )
{
    W2PAP *pap = ctx->xpap;
    W2CHP base, cur;
    WPARA *last_cell = NULL;
    TROW row;
    u32 mark, fc, piece = 0, chp_piece = W2_NONE, row_cells = 0, fld_fc, fld_lcb, index;
    int ch, chp_valid, obj;

    if ( cp_end > ctx->cp_max ) {
        cp_end = ctx->cp_max;
    }
    wb_story( &ctx->wb, story );
    ctx->nfield = ctx->over = ctx->mute = 0;
    ctx->fld = NULL;
    ctx->nfld = ctx->fld_next = 0;
    if ( w2_pair( ctx, kind == 0 ? W2_FLDMOM : kind == 1 ? W2_FLDFTN : W2_FLDHDR, &fld_fc, &fld_lcb ) &&
         fld_lcb >= 10 ) {
        ctx->fld = ctx->data + fld_fc;
        ctx->nfld = (fld_lcb - 4) / 6;
        ctx->fld_base = kind == 0 ? 0 : kind == 1 ? ctx->ccp_text : ctx->ccp_text + ctx->ccp_ftn;
    }
    mem_set( &cur, 0, sizeof( cur ) );

    while ( cp < cp_end ) {
        mark = w2_para_mark( ctx, cp, cp_end, &piece );
        fc = w2_fc( ctx, mark, &piece );
        if ( fc == W2_NONE ) {
            break;
        }
        w2_para_props( ctx, fc, piece, pap, &base );
        chp_valid = 0;
        for ( ; cp <= mark; cp++ ) {
            fc = w2_fc( ctx, cp, &piece );
            if ( fc == W2_NONE ) {
                continue;
            }
            if ( !chp_valid || piece != chp_piece || fc < ctx->run_from[0] || fc >= ctx->run_to[0] ) {
                w2_run( ctx, fc, 0 );
                cur = base;
                w2_chpx( ctx, ctx->run_prop[0], ctx->run_len[0], &cur );
                w2_prm( ctx, ctx->piece[piece].prm, NULL, &cur, &base );
                ctx->wb.chp = cur.chp;
                chp_piece = piece;
                chp_valid = 1;
            }
            ch = ctx->data[fc];

            /* a note's mark in the body: the note stands here, and its
               text is read when the body is done */
            while ( kind == 0 && ctx->ref_next < ctx->nnote &&
                    LE32( ctx->fnd_ref + 4 * ctx->ref_next ) <= cp ) {
                if ( LE32( ctx->fnd_ref + 4 * ctx->ref_next ) == cp && ctx->mute == 0 && ctx->over == 0 ) {
                    obj = wd_note_new( ctx->doc, NOTE_FOOT );
                    ctx->note_obj[ctx->ref_next] = obj;
                    wb_object( &ctx->wb, obj );
                }
                ctx->ref_next++;
            }

            if ( cur.spec && ch != 0x0D && ch != 0x07 && ch != 0x0C ) {
                if ( ch >= 0x13 && ch <= 0x15 ) {
                    w2_field( ctx, (u32)ch, cp );
                } else if ( ch == 0x01 ) {
                    obj = ctx->mute || ctx->over ? -2 : w2_picture( ctx, cur.pic );
                    if ( obj >= 0 ) {
                        wb_object( &ctx->wb, obj );
                    } else if ( obj == -1 ) {
                        ctx->doc->lost |= LOST_IMAGES;
                    }
                } else if ( ch == 0x00 ) {
                    w2_put( ctx, WCH_PAGENUM );
                }
                continue;               /* the rest are a note's number, a line's */
            }
            switch ( ch ) {
            case 0x0D:
            case 0x0A:
                if ( cp == mark ) {
                    w2_par( ctx, pap, 0 );      /* the half of a paragraph's end that is its mark */
                }
                break;
            case 0x07:
                if ( !pap->in_table ) {
                    w2_put( ctx, WCH_TAB );
                } else if ( !pap->ttp ) {
                    w2_par( ctx, pap, PF_CELLEND );
                    last_cell = story->count ? story->para[story->count - 1] : NULL;
                    row_cells++;
                } else {
                    /* the row's own mark: where its cells are goes to
                       the last cell's last paragraph */
                    if ( last_cell != NULL ) {
                        if ( pap->has_row ) {
                            row = pap->row;
                        } else {
                            mem_set( &row, 0, sizeof( row ) );
                            row.ncells = (u8)(row_cells > MAX_CELLS ? MAX_CELLS : row_cells);
                            for ( index = 0; index < row.ncells; index++ ) {
                                row.edge[index] = (s32)((index + 1) * 8640 / row.ncells);
                            }
                            ctx->doc->lost |= LOST_TABLES;
                        }
                        last_cell->pap.flags |= PF_ROWEND;
                        if ( !pap_set_row( &last_cell->pap, &row ) ) {
                            ctx->doc->failed = 1;
                        }
                    }
                    last_cell = NULL;
                    row_cells = 0;
                }
                break;
            case 0x0C:
                if ( kind != 0 || w2_page_after( ctx, cp, cp_end ) ) {
                    wb_char( &ctx->wb, WCH_PAGE );
                }
                w2_par( ctx, pap, 0 );
                break;
            case 0x09:
                w2_put( ctx, WCH_TAB );
                break;
            case 0x0B:
                w2_put( ctx, WCH_LINE );
                break;
            case 0x0E:
                w2_put( ctx, WCH_PAGE );        /* a new column, of which there is one */
                break;
            case 0x1E:
                w2_put( ctx, WCH_NBHYPHEN );
                break;
            case 0x1F:
                w2_put( ctx, WCH_SHY );
                break;
            default:
                if ( ch < 0x20 || cur.gone ) {
                    break;
                }
                if ( ctx->nfield && !ctx->over && !ctx->field[ctx->nfield - 1].result ) {
                    if ( ctx->code_len < sizeof( ctx->code ) - 1 ) {
                        ctx->code[ctx->code_len++] = (char)(ch < 0x7F ? ch : '?');
                        ctx->code[ctx->code_len] = 0;
                    }
                    break;
                }
                w2_put( ctx, cp_to_uni( CP_1252, ch ) );
                break;
            }
        }
    }
    wb_story( &ctx->wb, story );        /* text with no mark after it is a paragraph too */
}

/* one running head: the "which"-th of the header story's parts */
static void w2_head( W2CTX *ctx, const u8 *plc, u32 count, u32 which, STORY *story )
{
    u32 base = ctx->ccp_text + ctx->ccp_ftn, from, to;

    if ( which + 1 >= count ) {
        return;
    }
    from = LE32( plc + 4 * which );
    to = LE32( plc + 4 * which + 4 );
    if ( to <= from || to > ctx->ccp_hdr ) {
        return;
    }
    w2_story( ctx, story, base + from, base + to, 2 );
    /* a part ends with a paragraph mark that is the part's, not the text's */
    if ( story->count > 1 && story->para[story->count - 1]->len == 0 ) {
        wd_para_remove( ctx->doc, story, story->count - 1 );
    }
}

static u32 w2_bits_below( u32 value, u32 bit )
{
    u32 count = 0, mask;

    for ( mask = 1; mask < bit; mask <<= 1 ) {
        if ( value & mask ) {
            count++;
        }
    }
    return count;
}

static void w2_free( W2CTX *ctx )
{
    xfree( ctx->piece );
    xfree( ctx->style );
    xfree( ctx->xpap );
    xfree( ctx->font );
    xfree( ctx->note_obj );
}

static int w2_load( const u8 *data, u32 len, WDOC *doc )
{
    W2CTX ctx;
    W2SEP sep;
    PAGESETUP *page = &doc->page;
    const u8 *dop = NULL, *plc;
    u32 flags, fc_min, fc_mac, fc, lcb, tail, index, count, first, from, to, done, dop_grpf = 0, bit;
    STORY *story;

    if ( !w2_detect( data, len, "" ) ) {
        return WE_FORMAT;
    }
    flags = LE16( data + 0x0A );
    if ( flags & 0x0100 ) {
        return WE_PASSWORD;
    }
    mem_set( &ctx, 0, sizeof( ctx ) );
    ctx.data = data;
    ctx.len = len;
    ctx.doc = doc;
    ctx.ww1 = LE16( data ) == 0xA59B;
    fc_min = LE32( data + 0x18 );
    fc_mac = LE32( data + 0x1C );
    ctx.ccp_text = LE32( data + 0x34 );
    ctx.ccp_ftn = LE32( data + 0x38 );
    ctx.ccp_hdr = LE32( data + 0x3C );

    ctx.style = (W2STYLE *)try_alloc( W2_STYLES * sizeof( W2STYLE ) );
    ctx.xpap = (W2PAP *)try_alloc( sizeof( W2PAP ) );
    if ( ctx.style == NULL || ctx.xpap == NULL ) {
        w2_free( &ctx );
        return WE_NOMEM;
    }
    mem_set( ctx.style, 0, W2_STYLES * sizeof( W2STYLE ) );
    mem_set( ctx.xpap, 0, sizeof( W2PAP ) );

    if ( w2_pair( &ctx, W2_CLX, &fc, &lcb ) ) {
        ctx.clx = data + fc;
        ctx.clx_len = lcb;
    }
    if ( !w2_pieces( &ctx, fc_min, fc_mac, (flags & 0x0004) != 0 ) || !w2_fonts( &ctx ) ) {
        w2_free( &ctx );
        return WE_NOMEM;
    }
    /* the three stories follow one another, and none is past the text there is */
    if ( ctx.ccp_text > ctx.cp_max ) {
        ctx.ccp_text = ctx.cp_max;
    }
    if ( ctx.ccp_ftn > ctx.cp_max - ctx.ccp_text ) {
        ctx.ccp_ftn = ctx.cp_max - ctx.ccp_text;
    }
    if ( ctx.ccp_hdr > ctx.cp_max - ctx.ccp_text - ctx.ccp_ftn ) {
        ctx.ccp_hdr = ctx.cp_max - ctx.ccp_text - ctx.ccp_ftn;
    }

    /* the bin tables, and after the header's places the pages in a row */
    for ( index = 0; index < 2; index++ ) {
        if ( w2_pair( &ctx, W2_BTECHPX + index, &fc, &lcb ) && lcb >= 10 ) {
            ctx.bte[index] = data + fc;
            ctx.nbte[index] = (lcb - 4) / 6;
        }
    }
    tail = 0x58 + 6 * (ctx.ww1 ? 36 : 38) + 2;
    ctx.pn_first[0] = LE16( data + tail );
    ctx.pn_first[1] = LE16( data + tail + 2 );
    ctx.pn_count[0] = LE16( data + tail + 4 );
    ctx.pn_count[1] = LE16( data + tail + 6 );

    w2_styles( &ctx );
    wb_init( &ctx.wb, doc, &doc->body );

    /* the page: Word 1 keeps it with the document, Word 2 with the section */
    if ( w2_pair( &ctx, W2_DOP, &fc, &lcb ) && lcb >= 2 ) {
        dop = data + fc;
        dop_grpf = dop[1];
        if ( ctx.ww1 && lcb >= 0x1A ) {
            page->height = (s32)LE16( dop + 0x0A );
            page->width = (s32)LE16( dop + 0x0C );
            page->top = w2_s16( dop + 0x0E );
            page->left = (s32)LE16( dop + 0x10 );
            page->bottom = w2_s16( dop + 0x12 );
            page->right = (s32)LE16( dop + 0x14 );
            if ( page->top < 0 ) {
                page->top = -page->top;         /* below zero: "exactly", which is not kept */
            }
            if ( page->bottom < 0 ) {
                page->bottom = -page->bottom;
            }
            doc->deftab = (s32)LE16( dop + 0x18 );
        } else if ( !ctx.ww1 && lcb >= 0x0C ) {
            doc->deftab = (s32)LE16( dop + 0x0A );
        }
    }
    if ( w2_pair( &ctx, W2_SED, &fc, &lcb ) && lcb >= 10 ) {
        ctx.sed = data + fc;
        ctx.nsed = (lcb - 4) / 10;
    }
    mem_set( &sep, 0, sizeof( sep ) );
    sep.width = page->width;
    sep.height = page->height;
    sep.left = page->left;
    sep.right = page->right;
    sep.top = page->top;
    sep.bottom = page->bottom;
    sep.header = page->header;
    sep.footer = page->footer;
    w2_sepx( &ctx, 0, &sep );
    page->width = sep.width;
    page->height = sep.height;
    page->left = sep.left;
    page->right = sep.right;
    page->top = sep.top;
    page->bottom = sep.bottom;
    page->header = sep.header;
    page->footer = sep.footer;
    page->landscape = (u8)(page->width > page->height);
    if ( sep.first_page ) {
        page->first_page = (u16)sep.first_page;
    }
    if ( sep.columns > 1 ) {
        doc->lost |= LOST_LAYOUT;
    }

    if ( w2_pair( &ctx, W2_ASSOC, &fc, &lcb ) ) {
        w2_info( &ctx, data + fc, lcb, 2, doc->title );
        w2_info( &ctx, data + fc, lcb, 3, doc->subject );
        w2_info( &ctx, data + fc, lcb, 4, doc->keywords );
        w2_info( &ctx, data + fc, lcb, 6, doc->author );
    }

    /* the notes: where each mark is, and where each one's text starts */
    if ( ctx.ccp_ftn && w2_pair( &ctx, W2_FNDREF, &fc, &lcb ) && lcb >= 10 ) {
        ctx.fnd_ref = data + fc;
        ctx.nnote = (lcb - 4) / 6;
        if ( w2_pair( &ctx, W2_FNDTXT, &fc, &lcb ) && lcb >= 8 ) {
            ctx.fnd_txt = data + fc;
            if ( ctx.nnote > lcb / 4 - 1 ) {
                ctx.nnote = lcb / 4 - 1;
            }
        } else {
            ctx.nnote = 0;
        }
        if ( ctx.nnote ) {
            ctx.note_obj = (int *)try_alloc( ctx.nnote * sizeof( int ) );
            if ( ctx.note_obj == NULL ) {
                wb_done( &ctx.wb );
                w2_free( &ctx );
                return WE_NOMEM;
            }
            for ( index = 0; index < ctx.nnote; index++ ) {
                ctx.note_obj[index] = -1;
            }
        }
    }

    w2_story( &ctx, &doc->body, 0, ctx.ccp_text, 0 );
    done = 0;
    for ( index = 0; index < ctx.nnote; index++ ) {
        from = LE32( ctx.fnd_txt + 4 * index );
        to = LE32( ctx.fnd_txt + 4 * index + 4 );
        story = ctx.note_obj[index] >= 0 ? wd_note_story( doc, ctx.note_obj[index] ) : NULL;
        if ( from < done || to <= from || to > ctx.ccp_ftn || story == NULL ) {
            doc->lost |= LOST_NOTES;    /* a note with no mark, or out of its place */
            continue;
        }
        w2_story( &ctx, story, ctx.ccp_text + from, ctx.ccp_text + to, 1 );
        done = to;
    }

    /* the running heads: the header story's parts are, in order, those
       the document has (a note's separators) and those the section has;
       the ordinary pages' are kept, or the even pages', or the first's */
    if ( ctx.ccp_hdr && w2_pair( &ctx, W2_HDD, &fc, &lcb ) && lcb >= 8 ) {
        plc = data + fc;
        count = lcb / 4;
        first = w2_bits_below( dop_grpf, 0x100 );
        bit = (sep.grpf & 0x02) ? 0x02 : (sep.grpf & 0x01) ? 0x01 : (sep.grpf & 0x10) ? 0x10 : 0;
        if ( bit ) {
            w2_head( &ctx, plc, count, first + w2_bits_below( sep.grpf, bit ), &doc->header );
        }
        bit = (sep.grpf & 0x08) ? 0x08 : (sep.grpf & 0x04) ? 0x04 : (sep.grpf & 0x20) ? 0x20 : 0;
        if ( bit ) {
            w2_head( &ctx, plc, count, first + w2_bits_below( sep.grpf, bit ), &doc->footer );
        }
    }
    wb_story( &ctx.wb, &doc->body );
    wb_done( &ctx.wb );
    w2_free( &ctx );
    doc->variant = 0;
    return wd_failed( doc ) ? WE_NOMEM : WE_OK;
}

const FORMAT fmt_winword = {
    FMT_WINWORD, "Word for Windows 2.0", "DOC", w2_detect, w2_load, NULL, NULL, 0
};
