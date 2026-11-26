/*
 * F_WDOS.C - Microsoft Word for MS-DOS, and Windows Write.
 *
 * ONE FAMILY.  Write is the first Word for DOS moved to Windows, and it
 * kept the file: a header of 128 bytes, the text from byte 128 on, and
 * after the text pages of 128 bytes that say how the text looks.  So
 * the two formats are read by one reader and written by one writer,
 * with a flag for the places where they part.  They are told apart by
 * the header word at byte 96: Write keeps its page count there, and
 * Word leaves it zero.
 *
 * THE PAGES.  First the character pages, then the paragraph pages.
 * Each lists runs of text - where the run ends, and where in the page
 * its properties are - and a run's properties are only the first bytes
 * of the full set: what is not stored is standard.  After those come
 * the footnote table (Word), the division's properties and the table
 * that says which text they cover, and at the end Write's font names
 * or Word's summary sheet.
 *
 * WHAT WORD FOR DOS DOES NOT SAY.  Its fonts are numbers that only the
 * printer driver could name, so a number becomes the face its range
 * usually was: Courier, Helvetica, Times.  Its styles are in a style
 * sheet that is another file; the file says which, the document is
 * marked LOST_STYLES, and the formatting stored with the text is used.
 *
 * WRITING.  A Write file is laid out as Write 3.1 lays its own out,
 * and a Word file as Word 5.5 does, down to the byte that fills unused
 * room (DCh in Word).  The Word file says "5.0, made by another
 * program" (version 3), which tells Word not to trust the paragraph
 * heights; the room for them is still left in each paragraph page, as
 * Word leaves it.  Write has no footnotes: a note's text follows the
 * paragraph it was called from.  Word for DOS has no pictures in its
 * file, so pictures are left out of that one.
 */
#include "fmt.h"

#define WD_PAGE         128         /* the file is pages of this; the text starts on the second */
#define WD_FODS         20          /* the most runs a page can list */
#define WD_PAP_MAX      108         /* the longest paragraph properties */
#define WD_SEP_MAX      34          /* ...and division properties */
#define WD_NONE         0xFFFFFFFFUL
#define WD_FILL         0xDC        /* what Word leaves in bytes it does not use */
#define WD_WRITE_FONTS  512         /* a font code in Write is nine bits */

static const char *const worddos_variants[] = { "Word 5.0", NULL };
static const char *const write_variants[] = { "Write 3.1", NULL };

/* the colors of Word 5: black is the window's own */
static const uint32_t wdos_colors[8] = {
    COLOR_AUTO, 0xFF0000UL, 0x00FF00UL, 0x0000FFUL, 0x800080UL, 0xFF00FFUL, 0xFFFF00UL, 0xFFFFFFUL
};

/* what a range of Word's font numbers usually was */
typedef struct {
    uint8_t last;       /* the last number of the range */
    uint8_t family;
    uint8_t fixed;
    const char *name;
} WDFACE;

static const WDFACE wdos_faces[] = {
    {  7, FAM_MODERN, 1, "Courier New" },
    { 15, FAM_SWISS,  0, "Arial" },
    { 31, FAM_ROMAN,  0, "Times New Roman" },
    { 39, FAM_SCRIPT, 0, "Script" },
    { 47, FAM_ROMAN,  0, "Times New Roman" },
    { 55, FAM_DECOR,  0, "Old English" },
    { 63, FAM_SYMBOL, 0, "Symbol" }
};

static void put16( uint8_t *ptr, uint32_t value )
{
    ptr[0] = (uint8_t)value;
    ptr[1] = (uint8_t)(value >> 8);
}

static void put32( uint8_t *ptr, uint32_t value )
{
    put16( ptr, value );
    put16( ptr + 2, value >> 16 );
}

static int get_s16( const uint8_t *ptr )
{
    uint32_t value = LE16( ptr );

    return value >= 0x8000UL ? (int)value - 0x10000L : (int)value;
}

/* 0 neither, 1 a Word document, 2 a Write document */
static int wdos_kind( const uint8_t *data, uint32_t len )
{
    uint32_t ident;

    if ( len < WD_PAGE ) {
        return 0;
    }
    ident = LE16( data );
    if ( (ident != 0xBE31 && ident != 0xBE32) || LE16( data + 4 ) != 0xAB00 ) {
        return 0;
    }
    /* a glossary, a style sheet and a printer driver carry the same
       signature and are not documents */
    if ( LE16( data + 2 ) != 0 || LE32( data + 14 ) < WD_PAGE ) {
        return 0;
    }
    return LE16( data + 96 ) != 0 ? 2 : 1;
}

static int worddos_detect( const uint8_t *data, uint32_t len, const char *ext )
{
    (void)ext;
    return wdos_kind( data, len ) == 1 ? 100 : 0;
}

static int write_detect( const uint8_t *data, uint32_t len, const char *ext )
{
    (void)ext;
    return wdos_kind( data, len ) == 2 ? 100 : 0;
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

/* a place in the character pages, or in the paragraph pages */
typedef struct {
    uint32_t     pn, pn_end;  /* the page in hand, and the one after the last */
    unsigned int index;       /* the next run of that page */
    uint32_t     lim;         /* where the run in hand ends */
    const uint8_t *prop;      /* its stored properties, NULL for the standard ones */
    uint32_t     prop_len;
} FKPCUR;

typedef struct {
    const uint8_t *data;
    uint32_t     len;
    WDOC        *doc;
    WBUILD       wb;
    int          write;         /* a Write file */
    int          codepage;
    uint32_t     fc_mac;        /* where the text ends */
    FKPCUR       chp_cur, pap_cur;
    CHP          base;          /* what a run with no properties of its own has */
    int          special;       /* the run in hand is of field characters */
    int          picture;       /* the paragraph in hand is a picture */
    int          skip;          /* ...or a running head that is not kept */
    int          page_pending;  /* a division ended: what follows starts a page */
    uint32_t     rhc_was;       /* the running-head code of the paragraph before */
    uint32_t     rhc_top, rhc_bot; /* ...and of the heads that were kept */
    uint16_t    *font_map;      /* Write: the document's font for each font code */
    unsigned int nfont_map;
    uint16_t     font_of[64];   /* Word: the same; 0xFFFF for one not met yet */
    const uint8_t *fnd;         /* the footnote table's entries */
    unsigned int nnote;         /* how many notes it lists */
    unsigned int ref_next;      /* the next note whose mark is to be met */
    int         *note_obj;      /* the object made for each */
    const uint8_t *sed;         /* the section table's entries */
    unsigned int nsed, sed_next;
} WDOS;

/* the run that holds "fc": the pages are walked forward only, and a
   damaged page costs a run, never a step back */
static void fkp_seek( const WDOS *wd, FKPCUR *cur, uint32_t fc )
{
    const uint8_t *page, *fod;
    uint32_t count, off, cch;

    while ( cur->lim <= fc ) {
        if ( cur->pn >= cur->pn_end ) {
            cur->lim = WD_NONE;         /* nothing more is said: standard to the end */
            cur->prop = NULL;
            cur->prop_len = 0;
            return;
        }
        page = wd->data + cur->pn * WD_PAGE;
        count = page[WD_PAGE - 1];
        if ( count > WD_FODS ) {
            count = WD_FODS;
        }
        if ( cur->index >= count ) {
            cur->pn++;
            cur->index = 0;
            continue;
        }
        fod = page + 4 + 6 * cur->index;
        cur->index++;
        cur->lim = LE32( fod );
        off = LE16( fod + 4 );
        cur->prop = NULL;
        cur->prop_len = 0;
        if ( off < WD_PAGE - 6 ) {
            /* a length, then that many bytes, all before the page's count */
            cch = page[4 + off];
            if ( cch > WD_PAGE - 6 - off ) {
                cch = WD_PAGE - 6 - off;
            }
            cur->prop = page + 5 + off;
            cur->prop_len = cch;
        }
    }
}

static uint16_t wdos_font( WDOS *wd, uint32_t ftc )
{
    const WDFACE *face = wdos_faces;
    int font;

    if ( wd->write ) {
        if ( wd->nfont_map == 0 ) {
            return 0;
        }
        return wd->font_map[ftc < wd->nfont_map ? ftc : 0];
    }
    ftc &= 63;
    if ( wd->font_of[ftc] == 0xFFFF ) {
        while ( face->last < ftc ) {
            face++;
        }
        font = wd_font( wd->doc, face->name, face->family, face->fixed );
        wd->font_of[ftc] = (uint16_t)(font < 0 ? 0 : font);
    }
    return wd->font_of[ftc];
}

/* the character properties of the run in hand */
static void wdos_chp( WDOS *wd )
{
    uint8_t raw[8];
    CHP *chp = &wd->wb.chp;
    uint32_t take = wd->chp_cur.prop_len, ftc, kind;

    mem_set( raw, 0, sizeof( raw ) );
    raw[0] = 1;
    raw[2] = 24;
    if ( take > sizeof( raw ) ) {
        take = sizeof( raw );
    }
    if ( take ) {
        mem_cpy( raw, wd->chp_cur.prop, take );
    }
    *chp = wd->base;
    if ( raw[1] & 1 ) {
        chp->flags |= CF_BOLD;
    }
    if ( raw[1] & 2 ) {
        chp->flags |= CF_ITALIC;
    }
    ftc = raw[1] >> 2;
    if ( wd->write ) {
        ftc |= (uint32_t)(raw[4] & 7) << 6;
    }
    chp->font = wdos_font( wd, ftc );
    if ( raw[2] ) {
        chp->size = raw[2];
    }
    if ( raw[3] & 1 ) {
        chp->flags |= CF_UNDER;
    }
    if ( raw[5] ) {
        chp->flags |= raw[5] < 128 ? CF_SUPER : CF_SUB;
    }
    wd->special = (raw[3] & 0x40) != 0;
    if ( wd->write ) {
        return;                         /* the rest Write does not use */
    }
    if ( raw[3] & 2 ) {
        chp->flags |= CF_STRIKE;
    }
    if ( raw[3] & 4 ) {
        chp->flags = (uint16_t)((chp->flags & ~CF_UNDER) | CF_DUNDER);
    }
    if ( raw[3] & 8 ) {
        wd->doc->lost |= LOST_REVISIONS;        /* typed while changes were marked: kept */
    }
    kind = (raw[3] >> 4) & 3;
    if ( kind == 2 ) {
        chp->flags |= CF_SMALLCAPS;
    } else if ( kind ) {
        chp->flags |= CF_ALLCAPS;
    }
    if ( raw[3] & 0x80 ) {
        chp->flags |= CF_HIDDEN;
    }
    chp->color = wdos_colors[raw[6] & 7];
}

/* the paragraph properties of the paragraph in hand, and - in the body -
   which story its text belongs to */
static void wdos_pap( WDOS *wd, int body )
{
    uint8_t raw[WD_PAP_MAX + 4];
    char name[16];
    WTAB tabs[20];
    PAP *pap = &wd->wb.pap;
    WDOC *doc = wd->doc;
    const uint8_t *tbd;
    STORY *story;
    uint32_t take = wd->pap_cur.prop_len, rhc, kind, pos, most, line, stc, *kept;
    int count = 0, index, style;

    mem_set( raw, 0, sizeof( raw ) );
    raw[0] = 61;
    raw[2] = 30;
    raw[10] = 240;
    if ( take > WD_PAP_MAX ) {
        take = WD_PAP_MAX;
    }
    if ( take ) {
        mem_cpy( raw, wd->pap_cur.prop, take );
    }

    pap_free( pap );
    pap_init( pap );
    pap->align = (uint8_t)(raw[1] & 3);  /* left, centered, right, both: AL_*'s order */
    if ( raw[1] & 4 ) {
        pap->flags |= PF_KEEP;
    }
    if ( raw[1] & 8 ) {
        pap->flags |= PF_KEEPNEXT;
    }
    pap->right = get_s16( raw + 4 );
    pap->left = get_s16( raw + 6 );
    pap->first = get_s16( raw + 8 );
    line = LE16( raw + 10 );
    if ( line == 0 || line >= 0x8000UL ) {
        line = 240;                      /* Word's "auto" is -80 */
    } else if ( !wd->write ) {
        pap->line_rule = LS_ATLEAST;     /* Word's is a height; Write's counts lines */
    }
    pap->line = (uint16_t)line;
    pap->before = (uint16_t)LE16( raw + 12 );
    pap->after = (uint16_t)LE16( raw + 14 );

    most = wd->write ? 14 : 20;
    mem_set( tabs, 0, sizeof( tabs ) );
    for ( index = 0; (uint32_t)index < most; index++ ) {
        tbd = raw + 22 + 4 * index;
        pos = LE16( tbd );
        if ( pos == 0 ) {
            break;
        }
        kind = tbd[2] & 7;
        if ( kind > 3 ) {
            continue;                   /* a bar down the page is not a stop */
        }
        tabs[count].pos = (int)pos;
        if ( wd->write ) {
            tabs[count].kind = (uint8_t)(kind == 3 ? TAB_DECIMAL : TAB_LEFT);
        } else {
            tabs[count].kind = (uint8_t)kind;                /* TAB_*'s order */
            tabs[count].lead = (uint8_t)((tbd[2] >> 3) & 3); /* LEAD_*'s order */
        }
        count++;
    }
    if ( count && !pap_set_tabs( pap, tabs, count ) ) {
        doc->failed = 1;
    }

    rhc = raw[16] & 0x0F;
    wd->picture = 0;
    if ( wd->write ) {
        wd->picture = (raw[16] & 0x10) != 0;
        if ( !(rhc & 6) ) {
            rhc = 0;
        }
    } else {
        kind = (raw[16] >> 4) & 3;
        if ( kind == 1 ) {
            pap->flags |= PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT;
        } else if ( kind == 2 ) {
            if ( raw[17] & 1 ) {
                pap->flags |= PF_BORDER_LEFT;
            }
            if ( raw[17] & 2 ) {
                pap->flags |= PF_BORDER_RIGHT;
            }
            if ( raw[17] & 4 ) {
                pap->flags |= PF_BORDER_TOP;
            }
            if ( raw[17] & 8 ) {
                pap->flags |= PF_BORDER_BOT;
            }
        }
        if ( (raw[1] & 0x10) || mem_cmp( raw + 102, "\0\0\0\0\0\0", 6 ) != 0 ) {
            doc->lost |= LOST_LAYOUT;   /* side by side, or set at a place on the page */
        }
        /* the heading levels are the one thing a style's number says
           without its sheet */
        stc = raw[0] >> 1;
        if ( stc >= 88 && stc <= 93 ) {
            sfmt( name, sizeof( name ), "Heading %u", stc - 87 );
            style = wd_style( doc, name, STY_HEADING1 + (int)(stc - 88) );
            if ( style > 0 ) {
                pap->style = (uint8_t)style;
            }
        }
    }
    if ( !body ) {
        return;                         /* a note's text is all the note's */
    }

    if ( rhc == 0 ) {
        wd->skip = 0;
        story = &doc->body;
    } else {
        story = (rhc & 1) ? &doc->footer : &doc->header;
        kept = (rhc & 1) ? &wd->rhc_bot : &wd->rhc_top;
        if ( rhc != wd->rhc_was ) {
            /* a new running head.  The document keeps one for the top
               and one for the bottom: the first, unless a later one is
               for the ordinary pages and the first was not. */
            if ( *kept == 0 ) {
                wd->skip = 0;
                *kept = rhc;
            } else if ( !(*kept & 2) && (rhc & 2) ) {
                wb_story( &wd->wb, &doc->body );
                while ( story->count ) {
                    wd_para_remove( doc, story, story->count - 1 );
                }
                wd->skip = 0;
                *kept = rhc;
            } else {
                wd->skip = 1;
            }
        }
        /* a running head's indents are from the paper's edge unless it
           says they are from the margins */
        if ( !(raw[1] & 0x20) ) {
            pap->left -= doc->page.left;
            pap->right -= doc->page.right;
            if ( pap->left < 0 ) {
                pap->left = 0;
            }
            if ( pap->right < 0 ) {
                pap->right = 0;
            }
        }
    }
    wd->rhc_was = rhc;
    if ( wd->wb.story != story ) {
        wb_story( &wd->wb, story );
    }
}

/* a division's properties: the standard ones with what the file stores
   laid over them -> how many bytes the file had */
static uint32_t wdos_sep( const WDOS *wd, unsigned int index, uint8_t *sep )
{
    uint32_t fc, cch;

    mem_set( sep, 0, WD_SEP_MAX );
    sep[1] = 2;
    put16( sep + 2, 15840 );
    put16( sep + 4, 12240 );
    put16( sep + 6, 0xFFFF );
    put16( sep + 8, 1440 );
    put16( sep + 10, 12960 );
    put16( sep + 12, 1800 );
    put16( sep + 14, 8640 );
    sep[17] = 1;
    put16( sep + 18, wd->write ? 1080 : 720 );
    put16( sep + 20, wd->write ? 14760 : 15120 );
    put16( sep + 22, 720 );
    if ( index >= wd->nsed ) {
        return 0;
    }
    fc = LE32( wd->sed + 10 * index + 6 );
    if ( fc >= wd->len ) {
        return 0;                       /* FFFFFFFFh: the standard division */
    }
    cch = wd->data[fc];
    if ( cch > WD_SEP_MAX ) {
        cch = WD_SEP_MAX;
    }
    if ( cch > wd->len - fc - 1 ) {
        cch = wd->len - fc - 1;
    }
    mem_cpy( sep, wd->data + fc + 1, cch );
    return cch;
}

/* the first division's page is the document's */
static void wdos_page( WDOS *wd )
{
    uint8_t sep[WD_SEP_MAX];
    PAGESETUP *page = &wd->doc->page;
    uint32_t cch;
    int width, height, text, columns, value;

    if ( wd->write ) {
        page->header = page->footer = 1080;
    }
    if ( wd->nsed == 0 ) {
        return;
    }
    cch = wdos_sep( wd, 0, sep );
    height = (int)LE16( sep + 2 );
    width = (int)LE16( sep + 4 );
    page->height = height;
    page->width = width;
    page->landscape = (uint8_t)(width > height);
    page->top = (int)LE16( sep + 8 );
    value = height - page->top - (int)LE16( sep + 10 );
    if ( value > 0 ) {
        page->bottom = value;
    }
    page->left = (int)LE16( sep + 12 );
    text = (int)LE16( sep + 14 );
    columns = sep[17];
    if ( columns > 1 ) {
        text = text * columns + (int)LE16( sep + 22 ) * (columns - 1);
        wd->doc->lost |= LOST_LAYOUT;
    }
    value = width - page->left - text;
    if ( value > 0 ) {
        page->right = value;
    }
    page->header = (int)LE16( sep + 18 );
    if ( cch >= 22 ) {
        value = height - (int)LE16( sep + 20 );
        if ( value > 0 ) {
            page->footer = value;
        }
    }
    value = (int)LE16( sep + 6 );
    if ( value != 0 && value != 0xFFFF ) {
        page->first_page = (uint16_t)value;
    }
}

/* is the form feed at "pos" the end of a division?  The section table
   says where each one ends. */
static int wdos_division( WDOS *wd, uint32_t pos )
{
    uint32_t cp = pos - WD_PAGE + 1, lim;

    while ( wd->sed_next < wd->nsed ) {
        lim = LE32( wd->sed + 10 * wd->sed_next );
        if ( lim > cp ) {
            return 0;
        }
        wd->sed_next++;
        if ( lim == cp ) {
            return 1;
        }
    }
    return 0;
}

static int wdos_open( WDOS *wd )
{
    if ( wd->skip ) {
        return 0;
    }
    if ( wd->page_pending && wd->wb.story == &wd->doc->body ) {
        wd->page_pending = 0;
        wb_char( &wd->wb, WCH_PAGE );
    }
    return 1;
}

static void wdos_put( WDOS *wd, uint32_t code )
{
    if ( wdos_open( wd ) ) {
        wb_char( &wd->wb, code );
    }
}

static void wdos_plain( WDOS *wd, const uint8_t *text, unsigned int count )
{
    if ( count && wdos_open( wd ) ) {
        wb_bytes( &wd->wb, text, count, wd->codepage );
    }
}

/* C4h is Word's hyphen that no line breaks at - and the line-drawing
   set's horizontal rule.  Word takes it for the rule when a drawing
   character is beside it, and so is it taken here. */
static int wdos_is_rule( const WDOS *wd, uint32_t pos )
{
    int before = pos > WD_PAGE ? wd->data[pos - 1] : 0;
    int after = pos + 1 < wd->fc_mac ? wd->data[pos + 1] : 0;

    return (before >= 0xB3 && before <= 0xDA) || (after >= 0xB3 && after <= 0xDA);
}

/* text with one set of character properties, in one paragraph */
static void wdos_text( WDOS *wd, uint32_t fc, uint32_t lim, int body )
{
    const uint8_t *data = wd->data;
    uint8_t sep[WD_SEP_MAX];
    uint32_t pos, start = fc, code;
    int ch;

    for ( pos = fc; pos < lim; pos++ ) {
        ch = data[pos];
        if ( ch >= 0x20 && (wd->write || (ch != 0xC4 && ch != 0xFF)) ) {
            continue;                   /* a letter: handed over with its neighbors */
        }
        wdos_plain( wd, data + start, pos - start );
        start = pos + 1;
        code = 0;
        if ( wd->special && ch >= 1 && ch <= 8 ) {
            if ( ch == 1 ) {
                code = WCH_PAGENUM;
            } else if ( ch == 2 ) {
                code = WCH_DATE;
            } else if ( ch == 3 ) {
                code = WCH_TIME;
            } else if ( ch >= 7 ) {
                wd->doc->lost |= LOST_FIELDS;   /* a sequence, or a reference to one */
            }
            /* 4 and 5 are a note's number: the note's object stands there */
        } else if ( ch == 0x09 ) {
            code = WCH_TAB;
        } else if ( ch == 0x0A ) {
            if ( !wd->skip ) {
                wb_par( &wd->wb );
            }
        } else if ( ch == 0x0B ) {
            code = WCH_LINE;
        } else if ( ch == 0x0C ) {
            if ( body && wdos_division( wd, pos ) ) {
                /* a division's mark ends a paragraph that has no end of
                   its own, and what follows starts a page unless its
                   own division says it runs on */
                if ( wd->wb.para != NULL && !wd->skip ) {
                    wb_par( &wd->wb );
                }
                if ( wd->sed_next < wd->nsed && pos + 1 < wd->fc_mac ) {
                    wdos_sep( wd, wd->sed_next, sep );
                    if ( sep[1] & 7 ) {
                        wd->page_pending = 1;
                    }
                    if ( sep[17] > 1 ) {
                        wd->doc->lost |= LOST_LAYOUT;   /* it is in columns */
                    }
                }
            } else {
                code = WCH_PAGE;
            }
        } else if ( ch == 0x0E ) {
            code = WCH_PAGE;            /* a new column, of which there is one */
            wd->doc->lost |= LOST_LAYOUT;
        } else if ( ch == 0x0F ) {
            code = 0x2014;              /* Word's long dash */
        } else if ( ch == 0x1F ) {
            code = wd->write ? 0 : WCH_SHY;
        } else if ( ch == 0xC4 ) {
            code = wdos_is_rule( wd, pos ) ? 0x2500 : WCH_NBHYPHEN;
        } else if ( ch == 0xFF ) {
            code = WCH_NBSP;
        }
        if ( code ) {
            wdos_put( wd, code );
        }
    }
    wdos_plain( wd, data + start, lim - start );
}

/* ---- Write's pictures ---- */

/* bytes read in order, none past the end */
typedef struct {
    const uint8_t *ptr;
    uint32_t left;
} WDCUR;

static int cur_u32( WDCUR *cur, uint32_t *value )
{
    if ( cur->left < 4 ) {
        return 0;
    }
    *value = LE32( cur->ptr );
    cur->ptr += 4;
    cur->left -= 4;
    return 1;
}

static int cur_skip( WDCUR *cur, uint32_t count )
{
    if ( cur->left < count ) {
        return 0;
    }
    cur->ptr += count;
    cur->left -= count;
    return 1;
}

/* a string with its length before it; the length counts its NUL */
static int cur_string( WDCUR *cur, const uint8_t **text, uint32_t *len )
{
    if ( !cur_u32( cur, len ) || cur->left < *len ) {
        return 0;
    }
    *text = cur->ptr;
    return cur_skip( cur, *len );
}

static int name_is( const uint8_t *name, uint32_t len, const char *want )
{
    return len == str_len( want ) + 1 && mem_cmp( name, want, len ) == 0;
}

/* a device-independent bitmap, which is a .BMP file without its first
   fourteen bytes: they are put back */
static int wdos_dib( WDOS *wd, const uint8_t *dib, uint32_t size, int width, int height )
{
    uint32_t head, bits, colors, table;
    uint8_t *file;
    int obj;

    if ( size < 12 ) {
        return -1;
    }
    head = LE32( dib );
    if ( head == 12 ) {
        bits = LE16( dib + 10 );
        table = bits <= 8 ? (3UL << bits) : 0;
    } else if ( head >= 40 && head <= size ) {
        bits = LE16( dib + 14 );
        colors = LE32( dib + 32 );
        if ( colors == 0 && bits <= 8 ) {
            colors = 1UL << bits;
        }
        if ( colors > 256 ) {
            return -1;
        }
        table = colors * 4;
        if ( head == 40 && LE32( dib + 16 ) == 3 ) {
            table += 12;                /* three masks say where the colors are */
        }
    } else {
        return -1;
    }
    if ( table > size - head ) {
        return -1;
    }
    file = (uint8_t *)try_alloc( size + 14 );
    if ( file == NULL ) {
        return -1;
    }
    file[0] = 'B';
    file[1] = 'M';
    put32( file + 2, size + 14 );
    put32( file + 6, 0 );
    put32( file + 10, head + table + 14 );
    mem_cpy( file + 14, dib, size );
    obj = wd_image_new( wd->doc, file, size + 14, IMG_BMP, width, height );
    xfree( file );
    return obj;
}

/* a bitmap as Windows 3.0 held one for the screen: rows from the top,
   each a whole number of words, and no colors said.  Only one of two
   colors can be read without guessing; it becomes a .BMP file. */
static int wdos_ddb( WDOS *wd, const uint8_t *bits, uint32_t size, uint32_t across, uint32_t down, uint32_t row_bytes,
                     uint32_t planes, uint32_t depth, int width, int height )
{
    uint32_t stride, total, row, used;
    uint8_t *file;
    int obj;

    if ( planes != 1 || depth != 1 || across == 0 || down == 0 || row_bytes == 0 ) {
        return -1;
    }
    used = (across + 7) / 8;
    if ( used > row_bytes || size / row_bytes < down ) {
        return -1;                      /* its rows are not all there */
    }
    stride = ((across + 31) / 32) * 4;
    total = 62 + stride * down;         /* at most 8192 by 65535: it cannot wrap */
    file = (uint8_t *)try_alloc( total );
    if ( file == NULL ) {
        return -1;
    }
    mem_set( file, 0, total );
    file[0] = 'B';
    file[1] = 'M';
    put32( file + 2, total );
    put32( file + 10, 62 );
    put32( file + 14, 40 );
    put32( file + 18, across );
    put32( file + 22, down );
    put16( file + 26, 1 );
    put16( file + 28, 1 );
    put32( file + 34, stride * down );
    file[58] = file[59] = file[60] = 0xFF;      /* a bit that is set is white */
    for ( row = 0; row < down; row++ ) {
        mem_cpy( file + 62 + (down - 1 - row) * stride, bits + row * row_bytes, used );
    }
    obj = wd_image_new( wd->doc, file, total, IMG_BMP, width, height );
    xfree( file );
    return obj;
}

/* the picture OLE keeps to show an object by: its kind by name, eight
   bytes of size, its length, its bytes */
static int wdos_shown( WDOS *wd, WDCUR *cur, int width, int height )
{
    const uint8_t *name, *body;
    uint32_t name_len, size;

    if ( !cur_string( cur, &name, &name_len ) || !cur_skip( cur, 8 ) || !cur_u32( cur, &size ) ||
         cur->left < size ) {
        return -1;
    }
    body = cur->ptr;
    cur_skip( cur, size );
    if ( name_is( name, name_len, "DIB" ) ) {
        return wdos_dib( wd, body, size, width, height );
    }
    if ( name_is( name, name_len, "BITMAP" ) && size > 10 ) {
        return wdos_ddb( wd, body + 10, size - 10, LE16( body + 2 ), LE16( body + 4 ), LE16( body + 6 ),
                         body[8], body[9], width, height );
    }
    if ( name_is( name, name_len, "METAFILEPICT" ) && size > 8 ) {
        return wd_image_new( wd->doc, body + 8, size - 8, IMG_WMF, width, height );
    }
    return -1;
}

/* an OLE 1 object: a picture alone, or another program's data with a
   picture of it after.  Paintbrush's data is a .BMP file and is kept
   as that; anything else is kept as its picture. */
static int wdos_ole( WDOS *wd, const uint8_t *data, uint32_t size, int width, int height )
{
    WDCUR cur;
    const uint8_t *name, *text;
    uint32_t version, kind, name_len, text_len, native;
    int obj;

    cur.ptr = data;
    cur.left = size;
    if ( !cur_u32( &cur, &version ) || version != 0x501 || !cur_u32( &cur, &kind ) ) {
        return -1;
    }
    if ( kind == 3 ) {
        return wdos_shown( wd, &cur, width, height );
    }
    if ( (kind != 1 && kind != 2) || !cur_string( &cur, &name, &name_len ) ||
         !cur_string( &cur, &text, &text_len ) || !cur_string( &cur, &text, &text_len ) ) {
        return -1;
    }
    if ( kind == 1 ) {
        /* a link: the network name, and eight bytes on how it is kept fresh */
        if ( !cur_string( &cur, &text, &text_len ) || !cur_skip( &cur, 8 ) ) {
            return -1;
        }
    } else {
        if ( !cur_u32( &cur, &native ) || cur.left < native ) {
            return -1;
        }
        if ( (name_is( name, name_len, "PBrush" ) || name_is( name, name_len, "Paint.Picture" )) &&
             native > 14 && cur.ptr[0] == 'B' && cur.ptr[1] == 'M' ) {
            obj = wd_image_new( wd->doc, cur.ptr, native, IMG_BMP, width, height );
            if ( obj >= 0 ) {
                return obj;
            }
        }
        cur_skip( &cur, native );
    }
    if ( cur_u32( &cur, &version ) && version == 0x501 && cur_u32( &cur, &kind ) && kind == 5 ) {
        return wdos_shown( wd, &cur, width, height );
    }
    return -1;
}

/* a paragraph of Write's that is a picture: forty bytes that describe
   it, then a metafile, a bitmap, or an OLE object */
static void wdos_picture( WDOS *wd, uint32_t fc, uint32_t lim )
{
    const uint8_t *pic = wd->data + fc;
    uint32_t size = lim - fc, mode = 0, head, count, scale;
    int width, height;
    int obj = -1;

    if ( wd->skip ) {
        return;
    }
    if ( size >= 40 ) {
        mode = LE16( pic );
        head = LE16( pic + 30 );
        if ( head < 40 || head > size ) {
            head = 40;
        }
        /* the size it has, by the thousandths it was stretched to */
        scale = LE16( pic + 36 );
        width = (int)(LE16( pic + 10 ) * (scale ? scale : 1000) / 1000);
        scale = LE16( pic + 38 );
        height = (int)(LE16( pic + 12 ) * (scale ? scale : 1000) / 1000);
        if ( mode == 0xE4 ) {
            count = LE32( pic + 16 );
            if ( count > size - head ) {
                count = size - head;
            }
            obj = wdos_ole( wd, pic + head, count, width, height );
        } else {
            count = LE32( pic + 32 );
            if ( count == 0 ) {
                count = LE16( pic + 14 );
            }
            if ( count > size - head ) {
                count = size - head;
            }
            if ( mode == 0xE3 ) {
                obj = wdos_ddb( wd, pic + head, count, LE16( pic + 18 ), LE16( pic + 20 ),
                                LE16( pic + 22 ), pic[24], pic[25], width, height );
            } else if ( count >= 18 && LE16( pic + head + 2 ) == 9 ) {
                /* a metafile: its own header is nine words long */
                if ( width == 0 ) {
                    width = (int)(LE16( pic + 2 ) * 72 / 127); /* hundredths of a millimeter */
                    height = (int)(LE16( pic + 4 ) * 72 / 127);
                }
                obj = wd_image_new( wd->doc, pic + head, count, IMG_WMF, width, height );
            }
        }
    }
    if ( obj >= 0 ) {
        wb_object( &wd->wb, obj );
    } else {
        wd->doc->lost |= mode == 0xE4 ? LOST_OBJECTS : LOST_IMAGES;
    }
    wb_par( &wd->wb );
}

/* a CP - a count of characters from the start of the text - as a place
   in the file; the text's end for one that is past it */
static uint32_t wdos_fc_of( const WDOS *wd, uint32_t cp )
{
    return cp < wd->fc_mac - WD_PAGE ? cp + WD_PAGE : wd->fc_mac;
}

/* the text from "fc" to "end", which is the body's (running heads,
   notes' marks and divisions among it) or one note's */
static void wdos_range( WDOS *wd, uint32_t fc, uint32_t end, int body )
{
    uint32_t lim, ref;
    int obj;

    while ( fc < end ) {
        if ( fc >= wd->pap_cur.lim ) {
            fkp_seek( wd, &wd->pap_cur, fc );
            wdos_pap( wd, body );
        }
        if ( wd->picture ) {
            lim = wd->pap_cur.lim < end ? wd->pap_cur.lim : end;
            wdos_picture( wd, fc, lim );
            fc = lim;
            continue;
        }
        if ( fc >= wd->chp_cur.lim ) {
            fkp_seek( wd, &wd->chp_cur, fc );
            wdos_chp( wd );
        }
        lim = end;
        if ( wd->pap_cur.lim < lim ) {
            lim = wd->pap_cur.lim;
        }
        if ( wd->chp_cur.lim < lim ) {
            lim = wd->chp_cur.lim;
        }
        while ( body && wd->ref_next < wd->nnote ) {
            ref = wdos_fc_of( wd, LE32( wd->fnd + 8 * wd->ref_next ) );
            if ( ref > fc ) {
                if ( ref < lim ) {
                    lim = ref;
                }
                break;
            }
            if ( ref == fc && wdos_open( wd ) ) {
                obj = wd_note_new( wd->doc, NOTE_FOOT );
                wd->note_obj[wd->ref_next] = obj;
                wb_object( &wd->wb, obj );
            }
            wd->ref_next++;
        }
        wdos_text( wd, fc, lim, body );
        fc = lim;
    }
}

/* Write's font names, in the order the font codes count them */
static int wdos_fonts( WDOS *wd, uint32_t pn, uint32_t pn_end )
{
    const uint8_t *data = wd->data;
    char name[FONT_NAME_LEN];
    uint32_t pos = pn * WD_PAGE, end = pn_end * WD_PAGE, want, size;
    unsigned int used, index, guard, before;
    int family, font;

    if ( pn >= pn_end || end - pos < 4 ) {
        return 1;
    }
    want = LE16( data + pos );
    pos += 2;
    if ( want > WD_WRITE_FONTS ) {
        want = WD_WRITE_FONTS;
    }
    if ( want == 0 ) {
        return 1;
    }
    wd->font_map = (uint16_t *)try_alloc( want * sizeof( uint16_t ) );
    if ( wd->font_map == NULL ) {
        return 0;
    }
    /* every turn takes a name or moves to the next page, so the pages
       there are bound it; the guard is for the reader of this code */
    for ( guard = 0; wd->nfont_map < want && guard < 2 * WD_WRITE_FONTS && end - pos >= 2; guard++ ) {
        size = LE16( data + pos );
        if ( size == 0xFFFF ) {
            pos = (pos / WD_PAGE + 1) * WD_PAGE;        /* the names go on in the next page */
            if ( pos > end ) {
                break;
            }
            continue;
        }
        if ( size == 0 || size > end - pos - 2 ) {
            break;
        }
        family = data[pos + 2] >> 4;
        used = 0;
        for ( index = 1; index < size && data[pos + 2 + index] != 0; index++ ) {
            if ( used < sizeof( name ) - 1 ) {
                name[used++] = (char)(data[pos + 2 + index] < 0x80 && data[pos + 2 + index] >= 0x20 ?
                                      data[pos + 2 + index] : '?');
            }
        }
        name[used] = 0;
        /* the name says what kind of face it is, if it is a name that is
           known; the file's own word for it is wrong too often (Arial is
           "fixed pitch" in Windows 3.1's own WININI.WRI) and is used for
           the names that say nothing */
        family = family == 1 ? FAM_ROMAN : family == 2 ? FAM_SWISS : family == 3 ? FAM_MODERN :
                 family == 4 ? FAM_SCRIPT : family == 5 ? FAM_DECOR : FAM_ANY;
        before = wd->doc->nfont;
        font = wd_font( wd->doc, used ? name : "Arial", FAM_ANY, -1 );
        if ( (unsigned int)font == before && before < wd->doc->nfont && family != FAM_ANY &&
             wd->doc->font[font].family == FAM_ANY ) {
            wd->doc->font[font].family = (uint8_t)family;
            wd->doc->font[font].fixed = (uint8_t)(family == FAM_MODERN);
        }
        wd->font_map[wd->nfont_map++] = (uint16_t)(font < 0 ? 0 : font);
        pos += 2 + size;
    }
    return 1;
}

/* one line of Word's summary sheet */
static void wdos_info( const WDOS *wd, uint32_t base, uint32_t end, int which, char *dst )
{
    uint16_t wide[INFO_LEN];
    uint32_t pos;
    unsigned int used = 0;

    pos = base + LE16( wd->data + base + 2 * which );
    while ( pos < end && wd->data[pos] != 0 && used < INFO_LEN - 1 ) {
        wide[used++] = cp_to_uni( wd->codepage, wd->data[pos++] );
    }
    utf8_copy( dst, INFO_LEN, wide, used );
}

static uint32_t page_clamp( uint32_t pn, uint32_t low, uint32_t high )
{
    return pn < low ? low : pn > high ? high : pn;
}

static int wdos_load( const uint8_t *data, uint32_t len, WDOC *doc, int write )
{
    WDOS wd;
    uint32_t pages, pn_char, pn_para, pn_fntb, pn_sep, pn_setb, pn_next, pn_last, pn_mac;
    uint32_t count, room;
    unsigned int index;
    uint32_t from, to, done, value;
    STORY *story;

    if ( wdos_kind( data, len ) != (write ? 2 : 1) ) {
        return WE_FORMAT;
    }
    if ( !write && (data[117] & 2) ) {
        return WE_UNSUPPORTED;          /* saved in a hurry: the text is in pieces */
    }
    mem_set( &wd, 0, sizeof( wd ) );
    wd.data = data;
    wd.len = len;
    wd.doc = doc;
    wd.write = write;
    mem_set( wd.font_of, 0xFF, sizeof( wd.font_of ) );

    pages = len / WD_PAGE;
    wd.fc_mac = LE32( data + 14 );
    if ( wd.fc_mac > len ) {
        wd.fc_mac = len;
    }
    pn_char = (wd.fc_mac + WD_PAGE - 1) / WD_PAGE;
    if ( pn_char > pages ) {
        pn_char = pages;                /* the text runs into a last page that is not whole */
    }
    pn_para = page_clamp( LE16( data + 18 ), pn_char, pages );
    pn_fntb = page_clamp( LE16( data + 20 ), pn_para, pages );
    pn_sep = page_clamp( LE16( data + 22 ), pn_fntb, pages );
    pn_setb = LE16( data + 24 );
    pn_next = LE16( data + 26 );
    pn_last = LE16( data + 28 );
    pn_mac = page_clamp( LE16( data + (write ? 96 : 106) ), 0, pages );
    wd.chp_cur.pn = pn_char;
    wd.chp_cur.pn_end = pn_para;
    wd.pap_cur.pn = pn_para;
    wd.pap_cur.pn_end = pn_fntb;

    if ( write ) {
        wd.codepage = CP_1252;
    } else {
        wd.codepage = (int)LE16( data + 126 );
        if ( wd.codepage == 0 || !cp_known( wd.codepage ) ) {
            wd.codepage = CP_437;
        }
        value = LE16( data + 112 );
        if ( value ) {
            doc->deftab = (int)value;
        }
        if ( data[30] != 0 ) {
            doc->lost |= LOST_STYLES;   /* it names a style sheet, which is another file */
        }
        if ( pn_last < pn_mac && pn_mac - pn_last >= 1 ) {
            from = pn_last * WD_PAGE;
            to = pn_mac * WD_PAGE;
            wdos_info( &wd, from, to, 0, doc->title );
            wdos_info( &wd, from, to, 1, doc->author );
            wdos_info( &wd, from, to, 3, doc->keywords );
        }
    }

    /* the section table: what the divisions are, and the first one's page */
    if ( pn_setb != pn_next && pn_setb < pages ) {
        count = LE16( data + pn_setb * WD_PAGE );
        room = ((pages - pn_setb) * WD_PAGE - 4) / 10;
        wd.sed = data + pn_setb * WD_PAGE + 4;
        wd.nsed = count < room ? count : room;
    }
    wdos_page( &wd );

    /* the footnote table: one entry more than there are notes */
    if ( !write && pn_fntb < pn_sep ) {
        count = LE16( data + pn_fntb * WD_PAGE );
        room = ((pn_sep - pn_fntb) * WD_PAGE - 4) / 8;
        if ( count > room ) {
            count = room;
        }
        if ( count >= 2 ) {
            wd.fnd = data + pn_fntb * WD_PAGE + 4;
            wd.nnote = count - 1;
            wd.note_obj = (int *)try_alloc( wd.nnote * sizeof( int ) );
            if ( wd.note_obj == NULL ) {
                return WE_NOMEM;
            }
            for ( index = 0; index < wd.nnote; index++ ) {
                wd.note_obj[index] = -1;
            }
        }
    }

    if ( write && !wdos_fonts( &wd, pn_last < pn_mac ? pn_last : pn_mac, pn_mac ) ) {
        xfree( wd.note_obj );
        return WE_NOMEM;
    }
    wd.base = doc->style[0].chp;
    wd.base.font = wdos_font( &wd, 0 );
    wd.base.size = 24;
    doc->style[0].chp = wd.base;
    wb_init( &wd.wb, doc, &doc->body );

    /* the body, then each note's text, which is after it */
    done = wd.nnote ? wdos_fc_of( &wd, LE32( wd.fnd + 4 ) ) : wd.fc_mac;
    wdos_range( &wd, WD_PAGE, done, 1 );
    for ( index = 0; index < wd.nnote; index++ ) {
        from = wdos_fc_of( &wd, LE32( wd.fnd + 8 * index + 4 ) );
        to = wdos_fc_of( &wd, LE32( wd.fnd + 8 * index + 12 ) );
        story = wd.note_obj[index] >= 0 ? wd_note_story( doc, wd.note_obj[index] ) : NULL;
        if ( from < done || to <= from || story == NULL ) {
            if ( to > from ) {
                doc->lost |= LOST_NOTES;        /* a note with no mark, or out of its place */
            }
            continue;
        }
        wb_story( &wd.wb, story );
        wd.skip = 0;
        wd.picture = 0;
        wdos_range( &wd, from, to, 0 );
        done = to;
    }
    wb_done( &wd.wb );
    xfree( wd.font_map );
    xfree( wd.note_obj );
    doc->variant = 0;
    return wd_failed( doc ) ? WE_NOMEM : WE_OK;
}

static int worddos_load( const uint8_t *data, uint32_t len, WDOC *doc )
{
    return wdos_load( data, len, doc, 0 );
}

static int write_load( const uint8_t *data, uint32_t len, WDOC *doc )
{
    return wdos_load( data, len, doc, 1 );
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

/* the character pages, or the paragraph pages, as they fill */
typedef struct {
    OUT     *out;           /* the pages that are full */
    uint8_t  page[WD_PAGE]; /* the one being filled */
    uint32_t count;         /* its runs */
    uint32_t low;           /* its properties are packed down from the end: the lowest */
    uint32_t fc_done;       /* where the last run ended */
    uint32_t last_off;      /* the properties the last run has, to be shared */
    uint32_t last_len;
    uint32_t run_size;      /* a run's room in the list: 6, or 12 where Word
                               keeps a paragraph's height beside it */
    int      fill;          /* what unused bytes hold */
} FKPB;

static void fkpb_init( FKPB *fkp, OUT *out, uint32_t run_size, int fill )
{
    mem_set( fkp, 0, sizeof( FKPB ) );
    fkp->out = out;
    fkp->run_size = run_size;
    fkp->fill = fill;
    fkp->low = WD_PAGE - 1;
    fkp->fc_done = WD_PAGE;
    put32( fkp->page, WD_PAGE );
}

/* the page in hand is full, or is the last */
static void fkpb_flush( FKPB *fkp )
{
    uint32_t used = 4 + 6 * fkp->count;
    unsigned int index;

    if ( fkp->count == 0 ) {
        return;
    }
    if ( fkp->run_size > 6 ) {
        /* a height for each paragraph, marked "not known" */
        for ( index = 0; index < fkp->count; index++ ) {
            fkp->page[used] = 1;
            used += 6;
        }
    }
    if ( fkp->low > used ) {
        mem_set( fkp->page + used, fkp->fill, fkp->low - used );
    }
    fkp->page[WD_PAGE - 1] = (uint8_t)fkp->count;
    out_put( fkp->out, fkp->page, WD_PAGE );
    mem_set( fkp->page, 0, WD_PAGE );
    put32( fkp->page, fkp->fc_done );
    fkp->count = 0;
    fkp->low = WD_PAGE - 1;
    fkp->last_len = 0;
}

/* a run that ends at "fc_lim"; "len" 0 is the standard properties,
   which take no room */
static void fkpb_add( FKPB *fkp, uint32_t fc_lim, const uint8_t *prop, uint32_t len )
{
    uint8_t *fod;
    uint32_t need;
    int share;

    if ( fc_lim <= fkp->fc_done ) {
        return;
    }
    share = len == 0 || (fkp->count && fkp->last_len == len &&
                         mem_cmp( fkp->page + fkp->last_off + 1, prop, len ) == 0);
    need = share ? 0 : len + 1;
    if ( 4 + fkp->run_size * (fkp->count + 1) + need > fkp->low ) {
        fkpb_flush( fkp );
        share = len == 0;
        need = share ? 0 : len + 1;
    }
    if ( !share ) {
        fkp->low -= need;
        fkp->page[fkp->low] = (uint8_t)len;
        mem_cpy( fkp->page + fkp->low + 1, prop, len );
        fkp->last_off = fkp->low;
        fkp->last_len = len;
    }
    fod = fkp->page + 4 + 6 * fkp->count;
    put32( fod, fc_lim );
    put16( fod + 4, len ? fkp->last_off - 4 : 0xFFFF );
    fkp->count++;
    fkp->fc_done = fc_lim;
}

/* a note of Word's: where its mark is, and where its text starts */
typedef struct {
    uint32_t     ref, text;
    unsigned int obj;
} WSNOTE;

typedef struct {
    WDOC        *doc;
    OUT         *text;        /* the text, which starts at byte 128 of the file */
    FKPB         chp, pap;
    int          write;
    int          codepage;
    uint32_t     fc;          /* where the next byte of text will be */
    uint32_t     para_from;   /* where the paragraph in hand starts */
    uint8_t      run[8];      /* the properties of the characters being written */
    uint32_t     run_len;
    uint8_t      last_pap[WD_PAP_MAX + 4];  /* ...and of the last paragraph ended */
    uint32_t     last_pap_len;
    WSNOTE      *note;
    unsigned int nnote, note_cap;
    int          failed;      /* no memory for the notes' list */
} WSAVE;

static void ws_byte( WSAVE *ws, int value )
{
    out_byte( ws->text, value );
    ws->fc++;
}

/* the bytes that follow have these character properties */
static void ws_run( WSAVE *ws, const uint8_t *raw, uint32_t len )
{
    if ( len == ws->run_len && mem_cmp( raw, ws->run, len ) == 0 ) {
        return;
    }
    fkpb_add( &ws->chp, ws->fc, ws->run, ws->run_len );
    mem_cpy( ws->run, raw, len );
    ws->run_len = len;
}

/* how many bytes of "raw" must be stored: up to the last that is not
   what "standard" has */
static uint32_t raw_trim( const uint8_t *raw, const uint8_t *standard, uint32_t len )
{
    while ( len > 1 && raw[len - 1] == standard[len - 1] ) {
        len--;
    }
    return len;
}

static uint32_t ws_font_code( const WSAVE *ws, const CHP *chp )
{
    const WFONT *font;

    if ( chp->font >= ws->doc->nfont ) {
        return ws->write ? 0 : 16;
    }
    if ( ws->write ) {
        return chp->font < WD_WRITE_FONTS ? chp->font : 0;
    }
    /* Word has no names, only kinds: the first of each is the one
       every printer driver had */
    font = &ws->doc->font[chp->font];
    if ( font->fixed ) {
        return 0;
    }
    switch ( font->family ) {
    case FAM_SWISS:
        return 8;
    case FAM_SCRIPT:
        return 32;
    case FAM_DECOR:
        return 48;
    case FAM_SYMBOL:
        return 56;
    default:
        break;
    }
    return 16;
}

static uint32_t ws_color_code( uint32_t color )
{
    uint32_t index, bits;

    if ( color == COLOR_AUTO ) {
        return 0;
    }
    for ( index = 1; index < 8; index++ ) {
        if ( wdos_colors[index] == color ) {
            return index;
        }
    }
    /* the nearest of the eight, by which of red, green and blue are bright */
    bits = ((color >> 16) & 0x80 ? 4 : 0) | ((color >> 8) & 0x80 ? 2 : 0) | (color & 0x80 ? 1 : 0);
    switch ( bits ) {
    case 4:
        return 1;
    case 2:
        return 2;
    case 1:
    case 3:
        return 3;
    case 5:
        return 5;
    case 6:
        return 6;
    case 7:
        return 7;
    default:
        break;
    }
    return 0;
}

/* a CHP as the file stores one -> how many bytes */
static uint32_t ws_chp_raw( const WSAVE *ws, const CHP *chp, int special, uint8_t *raw )
{
    static const uint8_t standard[8] = { 1, 0, 24, 0, 0, 0, 0, 0 };
    uint32_t ftc = ws_font_code( ws, chp ), size = chp->size, len;

    mem_set( raw, 0, 8 );
    raw[1] = (uint8_t)((chp->flags & CF_BOLD ? 1 : 0) | (chp->flags & CF_ITALIC ? 2 : 0) | ((ftc & 63) << 2));
    if ( size < 4 ) {
        size = 4;
    }
    raw[2] = (uint8_t)(size > 254 ? 254 : size);
    if ( chp->flags & (CF_UNDER | CF_WUNDER) ) {
        raw[3] |= 1;
    }
    if ( special ) {
        raw[3] |= 0x40;
    }
    if ( chp->flags & CF_SUPER ) {
        raw[5] = 12;
    } else if ( chp->flags & CF_SUB ) {
        raw[5] = 244;
    }
    if ( ws->write ) {
        raw[4] = (uint8_t)(ftc >> 6);
    } else {
        if ( chp->flags & CF_STRIKE ) {
            raw[3] |= 2;
        }
        if ( chp->flags & CF_DUNDER ) {
            raw[3] = (uint8_t)((raw[3] & ~1) | 4);
        }
        if ( chp->flags & CF_SMALLCAPS ) {
            raw[3] |= 0x20;
        } else if ( chp->flags & CF_ALLCAPS ) {
            raw[3] |= 0x10;
        }
        if ( chp->flags & CF_HIDDEN ) {
            raw[3] |= 0x80;
        }
        raw[6] = (uint8_t)ws_color_code( chp->color );
    }
    len = raw_trim( raw, standard, 7 );
    /* text that is standard in everything has no properties stored in a
       Word file; Write stores some for every run */
    return len == 1 && !ws->write ? 0 : len;
}

/* a PAP as the file stores one -> how many bytes.  "rhc" is the
   running-head code, 0 for the body's; "extra" is Write's picture bit. */
static uint32_t ws_pap_raw( const WSAVE *ws, const PAP *pap, uint32_t rhc, uint32_t extra, uint8_t *raw )
{
    uint8_t standard[WD_PAP_MAX + 4];
    const PAGESETUP *page = &ws->doc->page;
    int left = pap->left, right = pap->right, first = pap->first;
    uint32_t line = pap->line, most = ws->write ? 13 : 19;
    unsigned int index, count = 0;
    uint32_t len, kind;
    uint8_t *tbd;

    mem_set( raw, 0, WD_PAP_MAX + 4 );
    mem_set( standard, 0, sizeof( standard ) );
    standard[0] = 61;
    standard[2] = 30;
    standard[10] = 240;
    raw[0] = 0x3C;                      /* the standard paragraph, formatted here */
    if ( !ws->write && pap->style < ws->doc->nstyle ) {
        kind = ws->doc->style[pap->style].kind;
        if ( kind >= STY_HEADING1 && kind <= STY_HEADING6 ) {
            raw[0] = (uint8_t)((88 + kind - STY_HEADING1) << 1); /* a heading, by its level */
        }
    }
    raw[2] = 30;
    raw[1] = (uint8_t)(pap->align & 3);
    if ( !ws->write ) {
        if ( pap->flags & PF_KEEP ) {
            raw[1] |= 4;
        }
        if ( pap->flags & PF_KEEPNEXT ) {
            raw[1] |= 8;
        }
    }
    if ( rhc ) {
        left += page->left;             /* a running head's are from the paper's edge */
        right += page->right;
    }
    if ( left < 0 ) {
        left = 0;
    }
    if ( left > 32000 ) {
        left = 32000;
    }
    if ( right < 0 ) {
        right = 0;
    }
    if ( right > 32000 ) {
        right = 32000;
    }
    if ( first < -32000 ) {
        first = -32000;
    }
    if ( first > 32000 ) {
        first = 32000;
    }
    put16( raw + 4, (uint32_t)right );
    put16( raw + 6, (uint32_t)left );
    put16( raw + 8, (uint32_t)first & 0xFFFF );
    if ( line < 120 ) {
        line = 240;
    }
    if ( line > 32000 ) {
        line = 32000;
    }
    if ( !ws->write && pap->line_rule == LS_MULTIPLE && line == 240 ) {
        line = 0xFFB0;                  /* Word's "auto" */
    }
    put16( raw + 10, line );
    put16( raw + 12, pap->before );
    put16( raw + 14, pap->after );
    raw[16] = (uint8_t)(rhc | extra);
    if ( !ws->write && (pap->flags & (PF_BORDER_TOP | PF_BORDER_BOT | PF_BORDER_LEFT | PF_BORDER_RIGHT)) ) {
        if ( (pap->flags & PF_BORDER_TOP) && (pap->flags & PF_BORDER_BOT) &&
             (pap->flags & PF_BORDER_LEFT) && (pap->flags & PF_BORDER_RIGHT) ) {
            raw[16] |= 0x10;            /* a box */
        } else {
            raw[16] |= 0x20;            /* the sides named */
            raw[17] = (uint8_t)((pap->flags & PF_BORDER_LEFT ? 1 : 0) | (pap->flags & PF_BORDER_RIGHT ? 2 : 0) |
                                (pap->flags & PF_BORDER_TOP ? 4 : 0) | (pap->flags & PF_BORDER_BOT ? 8 : 0));
        }
    }
    for ( index = 0; index < pap->ntabs && pap->tabs != NULL && count < most; index++ ) {
        if ( pap->tabs[index].pos <= 0 || pap->tabs[index].pos > 32000 ) {
            continue;
        }
        tbd = raw + 22 + 4 * count;
        put16( tbd, (uint32_t)pap->tabs[index].pos );
        kind = pap->tabs[index].kind;
        if ( ws->write ) {
            tbd[2] = (uint8_t)(kind == TAB_DECIMAL ? 3 : 0); /* Write has the two kinds */
        } else {
            tbd[2] = (uint8_t)((kind & 3) | ((pap->tabs[index].lead & 3) << 3));
        }
        count++;
    }
    if ( count ) {
        if ( ws->write ) {
            return 22 + 4 * count + 4;  /* Write stores the empty stop that ends them */
        }
        put16( raw + 20, 240 );         /* as Word has it wherever it stores this far */
        len = 22 + 4 * count;
        while ( len > 22 + 4 * count - 2 && raw[len - 1] == 0 ) {
            len--;                      /* ...and it cuts the last stop short */
        }
        return len;
    }
    len = raw_trim( raw, standard, 22 );
    if ( !ws->write && len < 12 ) {
        len = 12;                       /* Word always stores as far as the line spacing */
    }
    return len;
}

/* the paragraph in hand ends here */
static void ws_par_end( WSAVE *ws, const uint8_t *raw, uint32_t len )
{
    ws_byte( ws, '\r' );
    ws_byte( ws, '\n' );
    fkpb_add( &ws->pap, ws->fc, raw, len );
    ws->para_from = ws->fc;
    mem_cpy( ws->last_pap, raw, len );
    ws->last_pap_len = len;
}

/* a character that is itself only with a property set: a page number,
   a note's number */
static void ws_special( WSAVE *ws, const CHP *chp, const uint8_t *raw, uint32_t len, int value )
{
    uint8_t mine[8];
    uint32_t mine_len = ws_chp_raw( ws, chp, 1, mine );

    ws_run( ws, mine, mine_len );
    ws_byte( ws, value );
    ws_run( ws, raw, len );
}

static void ws_char( WSAVE *ws, const CHP *chp, const uint8_t *raw, uint32_t len, uint32_t code )
{
    char text[8];
    int count, index;

    switch ( code ) {
    case WCH_TAB:
        ws_byte( ws, 0x09 );
        return;
    case WCH_LINE:
        ws_byte( ws, 0x0B );
        return;
    case WCH_PAGE:
        ws_byte( ws, 0x0C );
        return;
    case WCH_PAGENUM:
        ws_special( ws, chp, raw, len, 1 );
        return;
    case WCH_DATE:
    case WCH_TIME:
        if ( !ws->write ) {
            ws_special( ws, chp, raw, len, code == WCH_DATE ? 2 : 3 );
        }
        return;
    case WCH_NUMPAGES:
    case WCH_FILENAME:
    case WCH_OBJ:
        return;
    case WCH_SHY:
        if ( !ws->write ) {
            ws_byte( ws, 0x1F );
        }
        return;
    case WCH_NBHYPHEN:
        ws_byte( ws, ws->write ? '-' : 0xC4 );
        return;
    case 0x2014:
        if ( !ws->write ) {
            ws_byte( ws, 0x0F );
            return;
        }
        break;
    default:
        break;
    }
    if ( code < 0x20 ) {
        return;
    }
    count = cp_put( ws->codepage, code, text );
    for ( index = 0; index < count; index++ ) {
        if ( (uint8_t)text[index] >= 0x20 ) {
            ws_byte( ws, (uint8_t)text[index] );
        }
    }
}

/* a picture as a paragraph of Write's.  A metafile goes as it is; other
   kinds have no place in a Write 3.0 picture and are left out. */
static int ws_picture( WSAVE *ws, const WOBJ *obj, const PAP *pap )
{
    uint8_t head[40], raw[WD_PAP_MAX + 4], plain[8];
    const uint8_t *data = obj->data;
    uint32_t size = obj->len, len, width, height;
    CHP chp;

    if ( !ws->write || obj->fmt != IMG_WMF || data == NULL ) {
        return 0;
    }
    if ( size > 22 && LE32( data ) == 0x9AC6CDD7UL ) {
        data += 22;                     /* a "placeable" metafile: Write wants it bare */
        size -= 22;
    }
    if ( size < 18 || LE16( data + 2 ) != 9 ) {
        return 0;
    }
    width = obj->width > 0 ? (uint32_t)obj->width : 1440;
    height = obj->height > 0 ? (uint32_t)obj->height : 1440;
    if ( width > 32000 ) {
        width = 32000;
    }
    if ( height > 32000 ) {
        height = 32000;
    }
    mem_set( head, 0, sizeof( head ) );
    put16( head, 0x88 );                /* a metafile, drawn to any size */
    put16( head + 2, width * 127 / 72 > 0xFFFF ? 0xFFFF : width * 127 / 72 );
    put16( head + 4, height * 127 / 72 > 0xFFFF ? 0xFFFF : height * 127 / 72 );
    put16( head + 10, width );
    put16( head + 12, height );
    put16( head + 30, 40 );
    put32( head + 32, size );
    put16( head + 36, 1000 );
    put16( head + 38, 1000 );

    mem_set( &chp, 0, sizeof( chp ) );
    chp.size = 24;
    chp.color = COLOR_AUTO;
    len = ws_chp_raw( ws, &chp, 0, plain );
    ws_run( ws, plain, len );
    out_put( ws->text, head, sizeof( head ) );
    out_put( ws->text, data, size );
    ws->fc += sizeof( head ) + size;
    len = ws_pap_raw( ws, pap, 0, 0x10, raw );
    if ( len < 17 ) {
        len = 17;                       /* as far as the bit that says "picture" */
    }
    fkpb_add( &ws->pap, ws->fc, raw, len );
    ws->para_from = ws->fc;
    mem_cpy( ws->last_pap, raw, len );
    ws->last_pap_len = len;
    return 1;
}

static void ws_note( WSAVE *ws, unsigned int obj )
{
    WSNOTE *grown;

    if ( ws->nnote == ws->note_cap ) {
        grown = (WSNOTE *)try_realloc( ws->note, (ws->note_cap + 32) * sizeof( WSNOTE ) );
        if ( grown == NULL ) {
            ws->failed = 1;
            return;
        }
        ws->note = grown;
        ws->note_cap += 32;
    }
    ws->note[ws->nnote].ref = ws->fc - WD_PAGE;
    ws->note[ws->nnote].text = 0;
    ws->note[ws->nnote].obj = obj;
    ws->nnote++;
}

static void ws_story( WSAVE *ws, const STORY *story, uint32_t rhc, int body );

/* a paragraph.  "body" says its notes and pictures can be written. */
static void ws_para( WSAVE *ws, const WPARA *para, uint32_t rhc, int body )
{
    uint8_t raw[WD_PAP_MAX + 4], run[8];
    const CHP *chp;
    const WOBJ *obj;
    uint32_t len, run_len;
    unsigned int off = 0, end, pos;
    uint32_t code, index;
    int which, pictures = 0;

    len = ws_pap_raw( ws, &para->pap, rhc, 0, raw );
    if ( (para->pap.flags & PF_PAGEBEFORE) && rhc == 0 && body ) {
        ws_byte( ws, 0x0C );            /* neither format has "starts a page": a page break does it */
    }
    while ( off < para->len ) {
        end = off + wd_run( para, off );
        if ( end <= off || end > para->len ) {
            break;
        }
        chp = wd_chp_get( ws->doc, para->attr[off] );
        run_len = ws_chp_raw( ws, chp, 0, run );
        ws_run( ws, run, run_len );
        while ( off < end ) {
            pos = off;
            code = utf16_get( para->text, end, &off );
            if ( code != WCH_OBJ ) {
                ws_char( ws, chp, run, run_len, code );
                continue;
            }
            which = wd_obj_at( para, pos );
            if ( which < 0 || (unsigned int)which >= ws->doc->nobj ) {
                continue;
            }
            obj = &ws->doc->obj[which];
            if ( obj->kind == OBJ_IMAGE && ws->write && body ) {
                /* a picture is a paragraph of its own: the text before
                   it ends one, and the text after starts another */
                if ( ws->fc > ws->para_from ) {
                    ws_par_end( ws, raw, len );
                }
                if ( ws_picture( ws, obj, &para->pap ) ) {
                    pictures++;
                    ws_run( ws, run, run_len );
                }
            } else if ( obj->kind == OBJ_NOTE && !ws->write && body && rhc == 0 ) {
                ws_note( ws, (unsigned int)which );
                ws_special( ws, chp, run, run_len, 5 );
            }
        }
    }
    if ( ws->fc > ws->para_from || !pictures ) {
        ws_par_end( ws, raw, len );
    }
    /* Write has no notes: what a note says follows the paragraph that
       called it */
    if ( ws->write && body ) {
        for ( index = 0; index < para->nanch; index++ ) {
            which = (int)para->anch[index].obj;
            if ( (unsigned int)which < ws->doc->nobj && ws->doc->obj[which].kind == OBJ_NOTE &&
                 ws->doc->obj[which].story != NULL ) {
                ws_story( ws, ws->doc->obj[which].story, rhc, 0 );
            }
        }
    }
}

static void ws_story( WSAVE *ws, const STORY *story, uint32_t rhc, int body )
{
    unsigned int index;

    for ( index = 0; index < story->count; index++ ) {
        ws_para( ws, story->para[index], rhc, body );
    }
}

/* a part of the file from memory to the file, filled out to whole
   pages; the OUT in memory is closed -> how many pages */
static uint32_t ws_emit( OUT *out, OUT *part, int fill )
{
    uint8_t *data;
    uint32_t len = 0, pages;

    data = out_data( part, &len );
    if ( out_error( part ) ) {
        out_fail( out, out_error( part ) );
    }
    pages = (len + WD_PAGE - 1) / WD_PAGE;
    if ( len ) {
        out_put( out, data, len );
        out_fill( out, fill, pages * WD_PAGE - len );
    }
    out_close( part );
    return pages;
}

/* an ASCIIZ line of Word's summary sheet, in the document's code page */
static void ws_info( const WSAVE *ws, OUT *part, const char *text, uint32_t most )
{
    const uint8_t *cur = (const uint8_t *)text, *end = cur + str_len( text );
    char bytes[8];
    uint32_t used = 0;
    int count, index;

    while ( cur < end ) {
        count = cp_put( ws->codepage, utf8_get( &cur, end ), bytes );
        if ( used + (uint32_t)count > most ) {
            break;
        }
        for ( index = 0; index < count; index++ ) {
            out_byte( part, (uint8_t)bytes[index] >= 0x20 ? (uint8_t)bytes[index] : ' ' );
        }
        used += (uint32_t)count;
    }
    out_byte( part, 0 );
}

static int wdos_save( WDOC *doc, OUT *out, int write )
{
    WSAVE ws;
    uint8_t head[WD_PAGE], sep[104], plain[8], std_pap[WD_PAP_MAX + 4];
    const PAGESETUP *page = &doc->page;
    OUT *fntb = NULL, *fonts = NULL, *sumd = NULL;
    CHP chp;
    PAP std;
    uint32_t plain_len, std_len;
    uint32_t fc_mac, cp_body, pn, pn_para, pn_fntb, pn_sep, pn_setb, pn_last, pn_mac;
    uint32_t text_pages, chp_pages, pap_pages, fntb_pages = 0, last_pages = 0, len;
    unsigned int index, family;
    uint32_t size;
    uint32_t ptr[9], pos, sep_len;
    int fill = write ? 0 : WD_FILL, result = WE_OK;

    mem_set( &ws, 0, sizeof( ws ) );
    ws.doc = doc;
    ws.write = write;
    ws.codepage = write ? CP_1252 : CP_437;
    ws.fc = ws.para_from = WD_PAGE;
    ws.text = out_mem();
    ws.chp.out = out_mem();
    ws.pap.out = out_mem();
    if ( ws.text == NULL || ws.chp.out == NULL || ws.pap.out == NULL ) {
        result = WE_NOMEM;
        goto done;
    }
    fkpb_init( &ws.chp, ws.chp.out, 6, fill );
    fkpb_init( &ws.pap, ws.pap.out, write ? 6 : 12, fill );
    mem_set( &chp, 0, sizeof( chp ) );
    chp.size = 24;
    chp.color = COLOR_AUTO;
    plain_len = write ? ws_chp_raw( &ws, &chp, 0, plain ) : 0;
    mem_cpy( ws.run, plain, 8 );
    ws.run_len = plain_len;
    pap_init( &std );
    std_len = ws_pap_raw( &ws, &std, 0, 0, std_pap );

    /* the running heads come first, as Write needs them and Word allows;
       then the body; then, in Word, a mark that ends the division, and
       the notes' text after it */
    ws_story( &ws, &doc->header, 0x0E, 0 );
    ws_story( &ws, &doc->footer, 0x0F, 0 );
    ws_story( &ws, &doc->body, 0, 1 );
    if ( ws.para_from == WD_PAGE ) {
        ws_par_end( &ws, std_pap, std_len );    /* a file has a paragraph */
    }
    if ( !write ) {
        ws_run( &ws, plain, plain_len );
        ws_byte( &ws, 0x0C );
        fkpb_add( &ws.pap, ws.fc, NULL, 0 );
        ws.para_from = ws.fc;
    }
    cp_body = ws.fc - WD_PAGE;
    for ( index = 0; index < ws.nnote; index++ ) {
        ws.note[index].text = ws.fc - WD_PAGE;
        ws_special( &ws, &chp, plain, plain_len, 5 );
        if ( doc->obj[ws.note[index].obj].story != NULL ) {
            ws_story( &ws, doc->obj[ws.note[index].obj].story, 0, 0 );
        }
        if ( ws.para_from != ws.fc ) {
            ws_par_end( &ws, std_pap, std_len );        /* a note ends with a paragraph's end */
        }
    }
    fc_mac = ws.fc;
    fkpb_add( &ws.chp, fc_mac, ws.run, ws.run_len );
    /* both programs list one paragraph more than the text has: the mark
       at the end, which is not stored */
    if ( write ) {
        fkpb_add( &ws.pap, fc_mac + 2, ws.last_pap, ws.last_pap_len );
    } else {
        fkpb_add( &ws.pap, fc_mac + 1, NULL, 0 );
    }
    fkpb_flush( &ws.chp );
    fkpb_flush( &ws.pap );
    if ( ws.failed ) {
        result = WE_NOMEM;
        goto done;
    }

    /* Word's footnote table: where each mark is and where each text
       starts, and one entry more for where the last text ends */
    if ( ws.nnote ) {
        fntb = out_mem();
        if ( fntb == NULL ) {
            result = WE_NOMEM;
            goto done;
        }
        out_le16( fntb, ws.nnote + 1 );
        out_le16( fntb, ws.nnote + 1 );
        for ( index = 0; index < ws.nnote; index++ ) {
            out_le32( fntb, ws.note[index].ref );
            out_le32( fntb, ws.note[index].text );
        }
        out_le32( fntb, fc_mac - WD_PAGE );
        out_le32( fntb, fc_mac - WD_PAGE );
        fntb_pages = (out_size( fntb ) + WD_PAGE - 1) / WD_PAGE;
    }

    if ( write ) {
        /* the font names: a name is whole in its page */
        fonts = out_mem();
        if ( fonts == NULL ) {
            result = WE_NOMEM;
            goto done;
        }
        len = doc->nfont < WD_WRITE_FONTS ? doc->nfont : WD_WRITE_FONTS;
        out_le16( fonts, len );
        for ( index = 0; index < len; index++ ) {
            size = str_len( doc->font[index].name ) + 2;        /* the family, the name, its NUL */
            if ( out_size( fonts ) % WD_PAGE + 2 + size + 2 > WD_PAGE ) {
                out_le16( fonts, 0xFFFF );
                out_fill( fonts, 0, WD_PAGE - out_size( fonts ) % WD_PAGE );
            }
            family = doc->font[index].family;
            out_le16( fonts, size );
            out_byte( fonts, doc->font[index].fixed || family == FAM_MODERN ? 0x30 :
                             family == FAM_ROMAN || family == FAM_SYMBOL ? 0x10 : family == FAM_SWISS ? 0x20 :
                             family == FAM_SCRIPT ? 0x40 : family == FAM_DECOR ? 0x50 : 0x00 );
            out_put( fonts, doc->font[index].name, size - 1 );
        }
        out_le16( fonts, 0 );
        last_pages = (out_size( fonts ) + WD_PAGE - 1) / WD_PAGE;
    } else if ( doc->title[0] || doc->author[0] || doc->keywords[0] ) {
        /* the summary sheet: nine places, then what is at them */
        sumd = out_mem();
        if ( sumd == NULL ) {
            result = WE_NOMEM;
            goto done;
        }
        out_fill( sumd, 0, 20 );
        ptr[0] = out_size( sumd );
        ws_info( &ws, sumd, doc->title, 40 );
        ptr[1] = out_size( sumd );
        ws_info( &ws, sumd, doc->author, 40 );
        ptr[2] = out_size( sumd );
        out_byte( sumd, 0 );            /* who typed it */
        ptr[3] = out_size( sumd );
        ws_info( &ws, sumd, doc->keywords, 80 );
        ptr[4] = out_size( sumd );
        out_byte( sumd, 0 );            /* comments */
        ptr[5] = out_size( sumd );
        out_byte( sumd, 0 );            /* its version */
        ptr[6] = out_size( sumd );
        out_fill( sumd, ' ', 8 );       /* the day it was changed */
        ptr[7] = out_size( sumd );
        out_fill( sumd, ' ', 8 );       /* ...and made */
        ptr[8] = out_size( sumd );
        out_le32( sumd, fc_mac - WD_PAGE );
        pos = out_tell( sumd );
        out_seek( sumd, 0 );
        for ( index = 0; index < 9; index++ ) {
            out_le16( sumd, ptr[index] );
        }
        out_seek( sumd, pos );
        last_pages = (out_size( sumd ) + WD_PAGE - 1) / WD_PAGE;
    }

    /* where everything is */
    text_pages = (fc_mac - 1) / WD_PAGE;        /* the pages after the header that hold text */
    chp_pages = out_size( ws.chp.out ) / WD_PAGE;
    pap_pages = out_size( ws.pap.out ) / WD_PAGE;
    pn = 1 + text_pages;
    pn_para = pn + chp_pages;
    pn_fntb = pn_para + pap_pages;
    pn_sep = pn_fntb + fntb_pages;
    pn_setb = pn_sep + 1;
    pn_last = pn_setb + 1;
    pn_mac = pn_last + last_pages;
    if ( pn_mac > 0xFFFF ) {
        result = WE_UNSUPPORTED;        /* the format counts its pages in sixteen bits */
        goto done;
    }

    mem_set( head, 0, sizeof( head ) );
    put16( head, 0xBE31 );
    put16( head + 4, 0xAB00 );
    put32( head + 14, fc_mac );
    put16( head + 18, pn_para );
    put16( head + 20, pn_fntb );
    put16( head + 22, pn_sep );
    put16( head + 24, pn_setb );
    put16( head + 26, pn_last );
    put16( head + 28, pn_last );
    if ( write ) {
        put16( head + 96, pn_mac );
    } else {
        put16( head + 106, pn_mac );
        put16( head + 108, 0x40 );      /* widows and orphans are kept from page ends */
        put16( head + 112, doc->deftab > 0 && doc->deftab < 32000 ? (uint32_t)doc->deftab : 720 );
        head[116] = 3;                  /* Word 5.0's format, made by another program */
        put16( head + 120, pn_sep );    /* no bookmarks: they would end where they start */
        put16( head + 126, 437 );
    }
    out_put( out, head, sizeof( head ) );
    ws_emit( out, ws.text, fill );
    ws.text = NULL;
    ws_emit( out, ws.chp.out, fill );
    ws.chp.out = NULL;
    ws_emit( out, ws.pap.out, fill );
    ws.pap.out = NULL;
    if ( fntb != NULL ) {
        ws_emit( out, fntb, fill );
        fntb = NULL;
    }

    /* the division: the page, and the table that says it covers all the text */
    mem_set( sep, 0, sizeof( sep ) );
    sep[1] = (uint8_t)(write ? 0x00 : 0xD2);
    sep[2] = 0x02;
    put16( sep + 3, (uint32_t)page->height );
    put16( sep + 5, (uint32_t)page->width );
    put16( sep + 7, write || page->first_page > 1 ? page->first_page : 0xFFFF );
    put16( sep + 9, (uint32_t)page->top );
    put16( sep + 11, (uint32_t)(page->height - page->top - page->bottom) );
    put16( sep + 13, (uint32_t)page->left );
    put16( sep + 15, (uint32_t)(page->width - page->left - page->right) );
    sep[18] = 1;
    put16( sep + 19, (uint32_t)page->header );
    put16( sep + 21, (uint32_t)(page->height - page->footer) );
    if ( write ) {
        put16( sep + 23, 720 );
        put16( sep + 27, 1080 );
        put16( sep + 29, 10440 );
        sep_len = 102;
    } else {
        sep_len = 22;
    }
    sep[0] = (uint8_t)sep_len;
    out_put( out, sep, sep_len + 1 );
    out_fill( out, fill, WD_PAGE - sep_len - 1 );

    out_le16( out, 2 );
    out_le16( out, 2 );
    out_le32( out, cp_body );
    out_le16( out, 2 );
    out_le32( out, pn_sep * WD_PAGE );
    out_le32( out, cp_body + 1 );
    out_le16( out, write ? 0x7FFF : 0xFFFF );
    out_le32( out, WD_NONE );
    out_fill( out, fill, WD_PAGE - 24 );

    if ( fonts != NULL ) {
        ws_emit( out, fonts, 0 );
        fonts = NULL;
    }
    if ( sumd != NULL ) {
        ws_emit( out, sumd, fill );
        sumd = NULL;
    }
    if ( out_error( out ) ) {
        result = out_error( out ) == WE_NOMEM ? WE_NOMEM : WE_IO;
    }

done:
    if ( ws.text != NULL ) {
        out_close( ws.text );
    }
    if ( ws.chp.out != NULL ) {
        out_close( ws.chp.out );
    }
    if ( ws.pap.out != NULL ) {
        out_close( ws.pap.out );
    }
    if ( fntb != NULL ) {
        out_close( fntb );
    }
    if ( fonts != NULL ) {
        out_close( fonts );
    }
    if ( sumd != NULL ) {
        out_close( sumd );
    }
    xfree( ws.note );
    return result;
}

static int worddos_save( WDOC *doc, OUT *out, int variant )
{
    (void)variant;
    return wdos_save( doc, out, 0 );
}

static int write_save( WDOC *doc, OUT *out, int variant )
{
    (void)variant;
    return wdos_save( doc, out, 1 );
}

const FORMAT fmt_worddos = {
    FMT_WORDDOS, "Word for MS-DOS", "DOC", worddos_detect, worddos_load, worddos_save, worddos_variants, 0
};

const FORMAT fmt_write = {
    FMT_WRITE, "Windows Write", "WRI", write_detect, write_load, write_save, write_variants, 0
};
