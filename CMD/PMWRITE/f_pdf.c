/*
 * F_PDF.C - a PDF file as a document: read for its text, and written
 * by PDFW.C.
 *
 * READ.  A PDF page has no paragraphs, no lines and no words: it has
 * characters, each put where the typesetter wanted it, in any order.
 * PMWRITE shows a PDF's real pages with the PDF code (PDFVIEW.H); what
 * is done here is a CONVERSION, for someone who wants the text of a
 * PDF in a form that can be edited.  Each page is run with nothing
 * drawn and every character noted with its place, its font and its
 * color (the second text sink, PDF\GFX.H), and then:
 *
 *   characters on one baseline are a line, put in order across; a gap
 *   between two is a space, and a wide one a tab;
 *
 *   a page whose lines nearly all have a gap at the same place is in
 *   two columns: the left column is read first, then the right, and
 *   a line that runs across both is read where it stands;
 *
 *   a line carries on the paragraph of the line above it when it is as
 *   far below as the page's lines usually are, is in the same size of
 *   type, starts where the paragraph's lines start, and when its first
 *   word would not have fitted on the line above.  That last is what
 *   tells a paragraph's short last line from a line that was simply
 *   full;
 *
 *   a hyphen that ends a line in the middle of a word is taken out and
 *   the word joined;
 *
 *   a paragraph all in type well above the size most of the document
 *   is in is a heading: the largest such size is Heading 1, the next
 *   Heading 2, the rest Heading 3.
 *
 * WHAT IS LOST.  Pictures and drawings (LOST_IMAGES when a page had an
 * image); text set at an angle; the page's layout - tables come out as
 * tabbed lines, and a paragraph that runs on over a page or a column
 * is two.  A hyphen that belonged to the word ("well-known", broken
 * after the hyphen) is taken out with the others.
 *
 * A file that wants a password is WE_PASSWORD unless one has been
 * given with fmt_pdf_password (PDFVIEW.H); an empty one opens it
 * without asking.  One PDF is open at a time, so reading one here
 * closes whatever PDFVIEW.H had open.
 *
 * MEMORY.  A page's characters are in the page's pool and go with it;
 * the PDF code leaves by pdf_fail when it has none, and that is caught
 * here and answered as WE_NOMEM.
 */
#include "fmt.h"
#include "gfx.h"
#include "pdfview.h"

/* PDFW.C: the writer */
extern int pdfw_save( WDOC *doc, OUT *out, int variant );
extern const char *const pdfw_variants[];

#define UNIT        64                  /* places are in 64ths of a point */
#define PTS( count ) ((int)(count) * UNIT)
#define TWIPS( val ) ((int)(val) * 5 / 16)

#define PAGE_CHARS  400000UL            /* of one page: the rest are let go */
#define LOOKS       256                 /* fonts in their styles and colors */

/* a character as it lay on the page */
typedef struct {
    int      xpos, ypos;
    uint16_t uni;
    uint16_t wide;                      /* how far it moved the pen */
    uint16_t size;                      /* of its type */
    uint16_t look;
} PCH;

/* a font in a style and a color, as the file names it */
typedef struct {
    char     name[FONT_NAME_LEN];
    uint32_t rgb;
    uint8_t  flags;                     /* FF_* */
    uint8_t  made;                      /* "font" has been asked for */
    uint16_t font;                      /* the document's number for it */
} LOOK;

/* a line, or the part of one that is in a column */
typedef struct {
    unsigned int first, count;          /* its characters, in order across */
    int          left, right, base;
    int          col_left, col_right;   /* of the column it is in */
    uint16_t     size;                  /* the largest type in it */
    uint16_t     flow;                  /* pieces of one flow may be one paragraph */
    uint8_t      joins;                 /* the next piece carries the paragraph on */
    uint8_t      alone;                 /* a line of a table or of a list of contents */
    uint16_t     spare;
} PIECE;

/* the reading in hand.  Static: a jump out of the PDF code puts the
   registers back, and nothing here may be lost to that. */
static struct {
    WDOC        *wdoc;
    WBUILD       wb;
    LOOK        *looks;
    unsigned int nlooks;
    PCH         *chars;                 /* the page's, in its pool */
    unsigned int count, cap;
    const char  *last_name;             /* the font of the character before: */
    uint32_t     last_rgb;              /* most characters are in the same one */
    int          last_flags;
    uint16_t     last_look;
    int          page;
    int          turned;                /* text at an angle was passed over */
    int          columns;               /* a page was in two columns */
    int          images;
    int          unknown;               /* characters with no Unicode */
    int          min_left, max_right;   /* of all the text, for the margins */
    int          min_top, max_bottom;
    int          page_wide, page_high;  /* the first page */
} job;

static const char *load_password;

void fmt_pdf_password( const char *text )
{
    load_password = text;
}

static int fp_detect( const uint8_t *data, uint32_t len, const char *ext )
{
    (void)ext;
    return mem_find( data, len < 1024 ? len : 1024, "%PDF-" ) >= 0 ? 100 : 0;
}

/* ------------------------------------------------------------------ */
/* fonts                                                               */
/* ------------------------------------------------------------------ */

static int is_upper( int ch )
{
    return ch >= 'A' && ch <= 'Z';
}

static int is_lower( int ch )
{
    return ch >= 'a' && ch <= 'z';
}

/*
 * A PDF's name for a font as a word processor's: "TimesNewRomanPS-
 * BoldItalicMT" is Times New Roman.  The style after a hyphen or a
 * comma goes (the font's flags say it), then the foundry's "MT" and
 * "PS", and a capital after a small letter starts a new word.
 */
static void name_clean( const char *raw, char *out )
{
    char work[FONT_NAME_LEN];
    unsigned int len = 0, index, put = 0;

    for ( index = 0; raw[index] && raw[index] != '-' && raw[index] != ',' && len < FONT_NAME_LEN - 1;
          index++ ) {
        if ( raw[index] > 0x20 && raw[index] < 0x7F ) {
            work[len++] = raw[index];
        }
    }
    if ( len > 4 && work[len - 2] == 'M' && work[len - 1] == 'T' ) {
        len -= 2;
    }
    if ( len > 4 && work[len - 2] == 'P' && work[len - 1] == 'S' ) {
        len -= 2;
    }
    for ( index = 0; index < len && put < FONT_NAME_LEN - 2; index++ ) {
        if ( index && is_upper( work[index] ) && is_lower( work[index - 1] ) ) {
            out[put++] = ' ';
        }
        out[put++] = work[index];
    }
    out[put] = 0;
}

/* which look a character has: found, or added while there is room */
static uint16_t look_of( const TEXTCH *ch )
{
    char name[FONT_NAME_LEN];
    LOOK *look;
    unsigned int index;
    int flags = ch->flags & (FF_FIXED | FF_SERIF | FF_ITALIC | FF_BOLD | FF_SYMBOL | FF_DINGBATS);

    if ( ch->name == job.last_name && ch->rgb == job.last_rgb && flags == job.last_flags ) {
        return job.last_look;
    }
    name_clean( ch->name, name );
    for ( index = 0; index < job.nlooks; index++ ) {
        look = &job.looks[index];
        if ( look->rgb == ch->rgb && look->flags == flags && str_cmp( look->name, name ) == 0 ) {
            break;
        }
    }
    if ( index == job.nlooks ) {
        if ( job.nlooks == LOOKS ) {
            index = LOOKS - 1;          /* more than there is room for: as the last one */
        } else {
            look = &job.looks[job.nlooks++];
            mem_set( look, 0, sizeof( *look ) );
            str_cpy( look->name, name );
            look->rgb = ch->rgb;
            look->flags = (uint8_t)flags;
        }
    }
    job.last_name = ch->name;
    job.last_rgb = ch->rgb;
    job.last_flags = flags;
    job.last_look = (uint16_t)index;
    return (uint16_t)index;
}

/* the character properties of a look at a size */
static void look_chp( uint16_t which, uint16_t size, CHP *chp )
{
    LOOK *look = &job.looks[which < job.nlooks ? which : 0];
    unsigned int half = ((unsigned int)size + UNIT / 4) / (UNIT / 2);

    if ( !look->made ) {
        look->made = 1;
        if ( look->flags & (FF_SYMBOL | FF_DINGBATS) ) {
            /* Symbol's and Dingbats' characters have been made Unicode
               by the PDF code, and a font of that name in a document
               means codes of the font's own (TF.C): so the document's
               first font, which shows what it can of them */
            look->font = 0;
        } else if ( look->name[0] ) {
            look->font = (uint16_t)wd_font( job.wdoc, look->name,
                                            (look->flags & FF_FIXED) ? FAM_MODERN
                                            : (look->flags & FF_SERIF) ? FAM_ROMAN : FAM_ANY,
                                            (look->flags & FF_FIXED) ? 1 : 0 );
        } else {
            look->font = (uint16_t)((look->flags & FF_FIXED) ? wd_font( job.wdoc, "Courier New", FAM_MODERN, 1 )
                                                        : 0);
        }
    }
    chp->font = look->font;
    chp->size = (uint16_t)(half < 2 ? 2 : (half > 3276 ? 3276 : half));
    chp->flags = (uint16_t)(((look->flags & FF_BOLD) ? CF_BOLD : 0) | ((look->flags & FF_ITALIC) ? CF_ITALIC : 0));
    chp->color = (look->rgb & 0xFFFFFFUL) ? (look->rgb & 0xFFFFFFUL) : COLOR_AUTO;
    chp->hilite = 0;
    chp->spare = 0;
}

/* ------------------------------------------------------------------ */
/* a page's characters                                                 */
/* ------------------------------------------------------------------ */

/* the sink: called from inside the PDF code, once a character */
static void note( const TEXTCH *ch )
{
    PCH *grown, *put;
    unsigned int cap;

    if ( ch->uni < 0x20 && ch->uni != 0x09 ) {
        return;
    }
    if ( ch->dir_x < 63500L ) {
        job.turned++;                   /* not across the page, give or take a few degrees */
        return;
    }
    if ( ch->xpos < -I2FX( 100 ) || ch->ypos < -I2FX( 100 ) || ch->xpos > I2FX( 20000 ) ||
         ch->ypos > I2FX( 20000 ) || job.count >= PAGE_CHARS ) {
        return;
    }
    if ( job.count == job.cap ) {
        cap = job.cap ? job.cap * 2 : 2048;
        grown = (PCH *)pool_big( doc.page_pool, cap * sizeof( PCH ) );
        if ( job.chars ) {
            mem_cpy( grown, job.chars, job.count * sizeof( PCH ) );
            pool_unbig( job.chars );
        }
        job.chars = grown;
        job.cap = cap;
    }
    put = &job.chars[job.count];
    put->xpos = ch->xpos >> 10;
    put->ypos = ch->ypos >> 10;
    /* the later characters of a ligature have no place of their own:
       each is put a hair after the one before, to keep their order */
    if ( ch->advance == 0 && job.count ) {
        put->xpos = put[-1].xpos + 1;
        put->ypos = put[-1].ypos;
    }
    put->uni = (uint16_t)(ch->uni == 0x09 || ch->uni == 0xA0 ? 0x20 : ch->uni);
    put->wide = (uint16_t)(ch->advance > I2FX( 1000 ) ? 64000U : (uint32_t)(ch->advance < 0 ? 0 : ch->advance >> 10));
    put->size = (uint16_t)(ch->size > I2FX( 1000 ) ? 64000U : (uint32_t)(ch->size < FX_ONE ? UNIT : ch->size >> 10));
    put->look = look_of( ch );
    if ( ch->uni == 0xFFFD ) {
        job.unknown++;
    }
    job.count++;
}

/* by height, or across: a shell sort, as everywhere in the PDF code */
static void sort_chars( PCH *list, unsigned int count, int across )
{
    PCH hold;
    unsigned int gap, at, back;

    for ( gap = 1; gap < count / 3; gap = gap * 3 + 1 ) {
    }
    for ( ; gap; gap /= 3 ) {
        for ( at = gap; at < count; at++ ) {
            hold = list[at];
            for ( back = at; back >= gap &&
                  (across ? list[back - gap].xpos > hold.xpos : list[back - gap].ypos > hold.ypos);
                  back -= gap ) {
                list[back] = list[back - gap];
            }
            list[back] = hold;
        }
    }
}

/* a piece of the characters first to first + count: 0 if it is all
   spaces */
static int piece_make( PIECE *piece, unsigned int first, unsigned int count )
{
    PCH *ch;
    unsigned int index, dots = 0;
    int end;

    while ( count && job.chars[first].uni == 0x20 ) {
        first++;
        count--;
    }
    while ( count && job.chars[first + count - 1].uni == 0x20 ) {
        count--;
    }
    if ( count == 0 ) {
        return 0;
    }
    mem_set( piece, 0, sizeof( *piece ) );
    piece->first = first;
    piece->count = count;
    piece->left = job.chars[first].xpos;
    piece->base = job.chars[first].ypos;
    piece->right = piece->left;
    for ( index = first; index < first + count; index++ ) {
        ch = &job.chars[index];
        end = ch->xpos + ch->wide;
        if ( ch->uni != 0x20 ) {
            /* a gap of two ems, or a row of dots, is a table's or a
               contents list's: such a line is no part of a paragraph */
            if ( ch->xpos - piece->right >= 2 * (int)ch->size && index > first ) {
                piece->alone = 1;
            }
            dots = ch->uni == '.' ? dots + 1 : 0;
            if ( dots >= 5 ) {
                piece->alone = 1;
            }
            if ( end > piece->right ) {
                piece->right = end;
            }
            if ( ch->size > piece->size ) {
                piece->size = ch->size;
            }
        }
    }
    return 1;
}

/* the widest gap in a line with a dozen characters either side of it:
   0 if it has none of a quarter of an inch.  *split is the first
   character after it. */
static int line_gap( unsigned int first, unsigned int count, int *lo, int *hi, unsigned int *split )
{
    PCH *ch;
    unsigned int index, seen = 0, at = 0;
    int end = 0, gap, widest = 0;

    for ( index = first; index < first + count; index++ ) {
        ch = &job.chars[index];
        if ( ch->uni == 0x20 ) {
            continue;
        }
        gap = ch->xpos - end;
        if ( seen >= 12 && gap > widest && gap >= PTS( 18 ) ) {
            widest = gap;
            *lo = end;
            *hi = ch->xpos;
            at = index;
        }
        if ( seen == 0 || ch->xpos + ch->wide > end ) {
            end = ch->xpos + ch->wide;
        }
        seen++;
    }
    if ( widest == 0 ) {
        return 0;
    }
    seen = 0;
    for ( index = at; index < first + count; index++ ) {
        seen += job.chars[index].uni != 0x20;
    }
    *split = at;
    return seen >= 12 ? widest : 0;
}

/* how wide the first word of a piece is */
static int first_word( const PIECE *piece )
{
    PCH *ch;
    unsigned int index;
    int end = piece->left;

    for ( index = piece->first; index < piece->first + piece->count; index++ ) {
        ch = &job.chars[index];
        if ( ch->uni == 0x20 || ch->xpos - end > (int)ch->size / 5 ) {
            break;
        }
        end = ch->xpos + ch->wide;
    }
    return end - piece->left;
}

static int is_letter( uint32_t code )
{
    return is_upper( (int)code ) || is_lower( (int)code ) || (code >= 0xC0 && code <= 0x24F && code != 0xD7 &&
           code != 0xF7);
}

/* ------------------------------------------------------------------ */
/* pieces into paragraphs                                              */
/* ------------------------------------------------------------------ */

/* which pieces carry on the paragraph of the piece before: the rules
   at the top of the file */
static void join_pieces( PIECE *pieces, unsigned int count )
{
    uint16_t leads[256];
    PIECE *cur, *next;
    unsigned int index, samples = 0, most = 0;
    int lead, usual = 0, limit, big, small, body_left = 0;
    int lines = 1;

    /* how far apart this page's lines usually are */
    mem_set( leads, 0, sizeof( leads ) );
    for ( index = 0; index + 1 < count; index++ ) {
        lead = pieces[index + 1].base - pieces[index].base;
        if ( pieces[index + 1].flow == pieces[index].flow && lead > 0 && lead < 4 * (int)pieces[index].size &&
             lead / 16 < 256 ) {
            leads[lead / 16]++;
            samples++;
        }
    }
    if ( samples >= 3 ) {
        for ( index = 1; index < 256; index++ ) {
            if ( leads[index] > leads[most] ) {
                most = index;
            }
        }
        usual = (int)most * 16 + 8;
    }

    for ( index = 0; index + 1 < count; index++ ) {
        cur = &pieces[index];
        next = cur + 1;
        lead = next->base - cur->base;
        big = cur->size > next->size ? cur->size : next->size;
        small = cur->size > next->size ? next->size : cur->size;
        limit = usual ? usual * 13 / 10 + 16 : big * 8 / 5;
        cur->joins = (uint8_t)(next->flow == cur->flow && !cur->alone && !next->alone && lead > 0 && lead <= limit &&
                               big * 10 <= small * 11 &&
                               cur->right + big / 4 + first_word( next ) >= cur->col_right - big / 8);
        if ( cur->joins && lines >= 2 &&
             (next->left > body_left + big * 3 / 4 || next->left < body_left - big * 3 / 4) ) {
            cur->joins = 0;             /* indented, or outdented: a paragraph's first line */
        }
        if ( cur->joins ) {
            if ( lines == 1 ) {
                body_left = next->left;
            }
            lines++;
        } else {
            lines = 1;
        }
    }
}

static void emit_char( PCH *ch )
{
    CHP chp;

    look_chp( ch->look, ch->size, &chp );
    job.wb.chp = chp;
    wb_char( &job.wb, ch->uni );
}

/* a paragraph: the pieces first to last */
static void emit_para( PIECE *pieces, unsigned int first, unsigned int last )
{
    PIECE *piece, *lead = &pieces[first];
    PCH *ch, *prev;
    unsigned int at, index, end;
    int reach, gap, body_left, middle;
    int space, centered = 1, flush = last > first;

    for ( at = first; at <= last; at++ ) {
        piece = &pieces[at];
        end = piece->first + piece->count;
        /* a hyphen that breaks a word across two lines goes */
        if ( at < last && piece->count >= 2 ) {
            ch = &job.chars[end - 1];
            if ( (ch->uni == '-' || ch->uni == 0xAD || ch->uni == 0x2010) && is_letter( ch[-1].uni ) &&
                 is_lower( job.chars[pieces[at + 1].first].uni ) ) {
                end--;
            }
        }
        reach = piece->left;
        space = 0;
        prev = NULL;
        for ( index = piece->first; index < end; index++ ) {
            ch = &job.chars[index];
            if ( ch->uni == 0x20 ) {
                space = prev != NULL;
                continue;
            }
            gap = ch->xpos - reach;
            /* the same character struck twice in one place is a bold
               made by hand: once is enough */
            if ( prev && prev->uni == ch->uni && ch->xpos - prev->xpos < (int)ch->size / 10 &&
                 ch->xpos >= prev->xpos && ch->wide ) {
                continue;
            }
            if ( prev && gap >= 2 * (int)ch->size ) {
                wb_char( &job.wb, WCH_TAB );
            } else if ( prev && (space || gap > (int)ch->size / 5) ) {
                wb_char( &job.wb, ' ' );
            }
            space = 0;
            emit_char( ch );
            if ( ch->xpos + ch->wide > reach ) {
                reach = ch->xpos + ch->wide;
            }
            prev = ch;
        }
        if ( at < last && end == piece->first + piece->count ) {
            wb_char( &job.wb, ' ' );    /* the line's end was a space between words */
        }
        middle = (piece->left + piece->right) / 2 - (piece->col_left + piece->col_right) / 2;
        if ( middle > (int)piece->size / 2 || middle < -(int)piece->size / 2 ||
             piece->left < piece->col_left + (int)piece->size ) {
            centered = 0;
        }
        if ( at < last && piece->right < piece->col_right - (int)piece->size / 3 ) {
            flush = 0;
        }
    }
    body_left = last > first ? pieces[first + 1].left : lead->left;
    job.wb.pap.align = (uint8_t)(centered ? AL_CENTER : (flush ? AL_JUSTIFY : AL_LEFT));
    job.wb.pap.left = 0;
    job.wb.pap.first = 0;
    if ( !centered ) {
        if ( body_left - lead->col_left >= PTS( 9 ) ) {
            job.wb.pap.left = TWIPS( body_left - lead->col_left );
        }
        if ( lead->left - body_left >= PTS( 7 ) || body_left - lead->left >= PTS( 7 ) ) {
            job.wb.pap.first = TWIPS( lead->left - body_left );
        }
    }
    wb_par( &job.wb );
}

/* ------------------------------------------------------------------ */
/* a page                                                              */
/* ------------------------------------------------------------------ */

/* the page's lines as pieces in the order they are read, a page of two
   columns' left column before its right: how many */
static unsigned int page_pieces( PIECE *pieces, PIECE *held )
{
    PIECE whole;
    unsigned int start, end, count = 0, nheld = 0, lines = 0, hits = 0, misses = 0, split = 0, index, run;
    int top, tall, lo = 0, hi = 0, gut_lo = 0, gut_hi = 0, gutter = 0, left, right;
    uint16_t flow = 1;
    int two;

    /* lines: everything whose baseline is within a third of the type's
       height of the first's, put in order across */
    sort_chars( job.chars, job.count, 0 );
    for ( start = 0; start < job.count; start = end ) {
        top = job.chars[start].ypos;
        tall = job.chars[start].size;
        for ( end = start + 1; end < job.count; end++ ) {
            if ( job.chars[end].size > tall && job.chars[end].size < 64000U ) {
                tall = job.chars[end].size;
            }
            if ( job.chars[end].ypos - top > (tall < PTS( 4 ) ? PTS( 1 ) : tall / 3) ) {
                break;
            }
        }
        sort_chars( job.chars + start, end - start, 1 );
        /* the same baseline for all of it, so that lines compare */
        for ( index = start; index < end; index++ ) {
            job.chars[index].ypos = top;
        }
        lines++;
        if ( line_gap( start, end - start, &lo, &hi, &split ) ) {
            if ( hits == 0 ) {
                gut_lo = lo;
                gut_hi = hi;
                hits = 1;
            } else if ( lo < gut_hi && hi > gut_lo ) {
                gut_lo = lo > gut_lo ? lo : gut_lo;
                gut_hi = hi < gut_hi ? hi : gut_hi;
                hits++;
            } else {
                misses++;
            }
        }
    }
    two = hits >= 6 && hits >= misses * 3 && hits * 5 >= lines * 2 && gut_hi - gut_lo >= PTS( 4 );
    if ( two ) {
        gutter = (gut_lo + gut_hi) / 2;
        job.columns = 1;
    }

    for ( start = 0; start < job.count; start = end ) {
        for ( end = start + 1; end < job.count && job.chars[end].ypos == job.chars[start].ypos; end++ ) {
        }
        if ( !piece_make( &whole, start, end - start ) ) {
            continue;
        }
        split = start;
        if ( two ) {
            for ( split = whole.first; split < whole.first + whole.count && job.chars[split].xpos < gutter;
                  split++ ) {
            }
        }
        if ( two && split > whole.first && split < whole.first + whole.count &&
             job.chars[split].xpos - (job.chars[split - 1].xpos + job.chars[split - 1].wide) >= PTS( 3 ) &&
             piece_make( &pieces[count], whole.first, split - whole.first ) &&
             piece_make( &held[nheld], split, whole.first + whole.count - split ) ) {
            /* a part in each column: the left is read now, the right
               when the column's end is reached */
            pieces[count++].flow = flow;
            held[nheld++].flow = (uint16_t)(flow + 1);
        } else if ( two && whole.right <= gutter ) {
            whole.flow = flow;
            pieces[count++] = whole;
        } else if ( two && whole.left >= gutter ) {
            whole.flow = (uint16_t)(flow + 1);
            held[nheld++] = whole;
        } else {
            /* a line across the page: the columns above it end here */
            mem_cpy( &pieces[count], held, nheld * sizeof( PIECE ) );
            count += nheld;
            nheld = 0;
            whole.flow = two ? (uint16_t)(flow + 2) : flow;
            pieces[count++] = whole;
            if ( two ) {
                flow = (uint16_t)(flow + 3);
            }
        }
    }
    mem_cpy( &pieces[count], held, nheld * sizeof( PIECE ) );
    count += nheld;

    /* how wide each column is: a flow of one line is as wide as the page's text */
    left = 0x7FFFFFFFL;
    right = -0x7FFFFFFFL;
    for ( index = 0; index < count; index++ ) {
        left = pieces[index].left < left ? pieces[index].left : left;
        right = pieces[index].right > right ? pieces[index].right : right;
    }
    for ( start = 0; start < count; start = end ) {
        lo = pieces[start].left;
        hi = pieces[start].right;
        for ( end = start + 1; end < count && pieces[end].flow == pieces[start].flow; end++ ) {
            lo = pieces[end].left < lo ? pieces[end].left : lo;
            hi = pieces[end].right > hi ? pieces[end].right : hi;
        }
        run = end - start;
        for ( index = start; index < end; index++ ) {
            pieces[index].col_left = run > 1 ? lo : left;
            pieces[index].col_right = run > 1 ? hi : right;
        }
    }
    /* what the margins are made from */
    for ( index = 0; index < count; index++ ) {
        top = pieces[index].base - (int)pieces[index].size * 5 / 6;
        tall = pieces[index].base + (int)pieces[index].size / 4;
        job.min_top = top < job.min_top ? top : job.min_top;
        job.max_bottom = tall > job.max_bottom ? tall : job.max_bottom;
    }
    if ( count ) {
        job.min_left = left < job.min_left ? left : job.min_left;
        job.max_right = right > job.max_right ? right : job.max_right;
    }
    return count;
}

/* the page job.page, inside the trap and between pdf_page_begin and
   pdf_page_end */
static void page_convert( void )
{
    PAGE page;
    VIEW view;
    PIECE *pieces, *held;
    unsigned int count, start, end;
    fx wide, high, swap;

    job.chars = NULL;
    job.count = job.cap = 0;
    job.last_name = NULL;
    job.last_look = 0;
    job.last_rgb = 0xFFFFFFFFUL;
    if ( !pdf_page( job.page, &page ) ) {
        return;
    }
    if ( job.page == 0 ) {
        wide = real_to_fx( page.box[2] ) - real_to_fx( page.box[0] );
        high = real_to_fx( page.box[3] ) - real_to_fx( page.box[1] );
        if ( page.rotate == 90 || page.rotate == 270 ) {
            swap = wide;
            wide = high;
            high = swap;
        }
        job.page_wide = wide >> 10;
        job.page_high = high >> 10;
    }
    view_setup( &view, &page, FX_ONE, 0 );
    text_sink2 = note;
    page_text( &page, &view );
    text_sink2 = NULL;
    job.images += text_images;

    /* from here on nothing jumps: the document is added to */
    if ( job.page > 0 ) {
        wb_char( &job.wb, WCH_PAGE );
    }
    if ( job.count == 0 ) {
        if ( job.page > 0 ) {
            wb_par( &job.wb );
        }
        return;
    }
    pieces = (PIECE *)pool_big( doc.page_pool, (job.count + 2) * sizeof( PIECE ) );
    held = (PIECE *)pool_big( doc.page_pool, (job.count + 2) * sizeof( PIECE ) );
    count = page_pieces( pieces, held );
    join_pieces( pieces, count );
    for ( start = 0; start < count; start = end + 1 ) {
        for ( end = start; end + 1 < count && pieces[end].joins; end++ ) {
        }
        emit_para( pieces, start, end );
    }
    if ( count == 0 && job.page > 0 ) {
        wb_par( &job.wb );
    }
}

/* ------------------------------------------------------------------ */
/* after the last page                                                 */
/* ------------------------------------------------------------------ */

/* a string of the file's /Info as UTF-8: PDF's own encoding, taken as
   Latin-1, or UTF-16 after its mark */
static void info_text( DICT *info, const char *key, char *out )
{
    OBJ *val = dict_get( info, key );
    const uint8_t *text;
    unsigned int len, index, put = 0;
    uint32_t code;

    if ( val->type != T_STR ) {
        return;
    }
    text = val->u.str.ptr;
    len = val->u.str.len;
    if ( len >= 2 && text[0] == 0xFE && text[1] == 0xFF ) {
        for ( index = 2; index + 1 < len && put + 4 < INFO_LEN; index += 2 ) {
            code = ((uint32_t)text[index] << 8) | text[index + 1];
            if ( code >= 0x20 && (code < 0xD800 || code > 0xDFFF) ) {
                put += (unsigned int)utf8_put( (uint8_t *)out + put, code );
            }
        }
    } else {
        for ( index = 0; index < len && put + 4 < INFO_LEN; index++ ) {
            if ( text[index] >= 0x20 ) {
                put += (unsigned int)utf8_put( (uint8_t *)out + put, text[index] );
            }
        }
    }
    out[put] = 0;
}

/* the smallest type in a paragraph, in half points: 0 if it has no
   text to speak of */
static unsigned int para_size( WDOC *wdoc, const WPARA *para )
{
    unsigned int off, least = 0, size;

    for ( off = 0; off < para->len; off++ ) {
        if ( para->text[off] <= 0x20 ) {
            continue;
        }
        size = wd_chp_get( wdoc, para->attr[off] )->size;
        if ( least == 0 || size < least ) {
            least = size;
        }
    }
    return least;
}

#define SIZES       512                 /* half points counted one by one */

/* The size most of the text is in; paragraphs all in larger type made
   headings; Normal given the look most of the text has.  0 if there
   was no memory for the counting, and then none of it is done. */
static int headings( WDOC *wdoc )
{
    unsigned int *by_size, *by_chp;
    WPARA *para;
    unsigned int index, off, body = 0, best = 0, size, tops[3], level;
    int style;

    by_size = (unsigned int *)try_alloc( (SIZES + wdoc->nchp + 1) * sizeof( unsigned int ) );
    if ( by_size == NULL ) {
        return 0;
    }
    by_chp = by_size + SIZES;
    mem_set( by_size, 0, (SIZES + wdoc->nchp + 1) * sizeof( unsigned int ) );
    for ( index = 0; index < wdoc->body.count; index++ ) {
        para = wdoc->body.para[index];
        for ( off = 0; off < para->len; off++ ) {
            if ( para->text[off] > 0x20 && para->attr[off] < wdoc->nchp ) {
                size = wd_chp_get( wdoc, para->attr[off] )->size;
                by_size[size < SIZES ? size : SIZES - 1]++;
                by_chp[para->attr[off]]++;
            }
        }
    }
    for ( index = 1; index < SIZES; index++ ) {
        if ( by_size[index] > by_size[body] ) {
            body = index;
        }
    }
    for ( index = 1; index < wdoc->nchp; index++ ) {
        if ( by_chp[index] > by_chp[best] ) {
            best = index;
        }
    }
    xfree( by_size );
    if ( body == 0 ) {
        return 1;
    }
    wdoc->style[0].chp = *wd_chp_get( wdoc, (uint16_t)best );
    wdoc->style[0].chp.flags = 0;

    /* the three largest sizes that whole short paragraphs are in */
    tops[0] = tops[1] = tops[2] = 0;
    for ( index = 0; index < wdoc->body.count; index++ ) {
        para = wdoc->body.para[index];
        size = para_size( wdoc, para );
        if ( para->len > 200 || size * 20 < body * 23 || size == tops[0] || size == tops[1] ||
             size == tops[2] ) {
            continue;
        }
        if ( size > tops[0] ) {
            tops[2] = tops[1];
            tops[1] = tops[0];
            tops[0] = size;
        } else if ( size > tops[1] ) {
            tops[2] = tops[1];
            tops[1] = size;
        } else if ( size > tops[2] ) {
            tops[2] = size;
        }
    }
    for ( index = 0; index < wdoc->body.count; index++ ) {
        para = wdoc->body.para[index];
        size = para_size( wdoc, para );
        if ( para->len > 200 || size * 20 < body * 23 ) {
            continue;
        }
        level = size >= tops[0] ? 0 : (size >= tops[1] ? 1 : 2);
        style = wd_style( wdoc, level == 0 ? "Heading 1" : (level == 1 ? "Heading 2" : "Heading 3"),
                          STY_HEADING1 + (int)level );
        if ( style > 0 && style < 256 ) {
            para->pap.style = (uint8_t)style;
        }
    }
    return 1;
}

/* the paper from the first page, and the margins from where the text
   was: never less than a quarter of an inch, nor more than a third of
   the paper */
static int margin( int found, int paper )
{
    int twips = TWIPS( found );

    if ( twips < 360 ) {
        twips = 360;
    }
    return twips > paper / 3 ? paper / 3 : twips;
}

static void page_setup( WDOC *wdoc )
{
    PAGESETUP *page = &wdoc->page;

    if ( job.page_wide < PTS( 72 ) || job.page_high < PTS( 72 ) || job.page_wide > PTS( 5000 ) ||
         job.page_high > PTS( 5000 ) ) {
        return;                         /* nothing to go by: the document's own */
    }
    page->width = TWIPS( job.page_wide );
    page->height = TWIPS( job.page_high );
    page->landscape = (uint8_t)(page->width > page->height);
    if ( job.max_right > job.min_left ) {
        page->left = margin( job.min_left, page->width );
        page->right = margin( job.page_wide - job.max_right, page->width );
        page->top = margin( job.min_top, page->height );
        page->bottom = margin( job.page_high - job.max_bottom, page->height );
        /* text that stops well short of the right or the bottom is a
           short line or a short page, not a wide margin */
        if ( page->right > page->left * 2 ) {
            page->right = page->left;
        }
        if ( page->bottom > page->top * 2 ) {
            page->bottom = page->top;
        }
    }
}

/* ------------------------------------------------------------------ */

static int reason( int why )
{
    switch ( why ) {
    case PV_OK:       return WE_OK;
    case PV_NOMEM:    return WE_NOMEM;
    case PV_PASSWORD: return WE_PASSWORD;
    case PV_CRYPT:    return WE_UNSUPPORTED;
    case PV_IO:       return WE_IO;
    }
    return WE_FORMAT;
}

static int fp_load( const uint8_t *data, uint32_t len, WDOC *wdoc )
{
    static JMPBUF trap;
    DICT *info;
    int why, pages, result = WE_OK;

    why = pdfview_open_mem( data, len );
    if ( why == PV_PASSWORD && load_password != NULL ) {
        why = pdfview_password( load_password, str_len( load_password ) );
    }
    if ( why != PV_OK ) {
        pdfview_close();
        return reason( why );
    }
    mem_set( &job, 0, sizeof( job ) );
    job.wdoc = wdoc;
    job.min_left = job.min_top = 0x7FFFFFFFL;
    job.max_right = job.max_bottom = -0x7FFFFFFFL;
    job.looks = (LOOK *)try_alloc( LOOKS * sizeof( LOOK ) );
    if ( job.looks == NULL ) {
        pdfview_close();
        return WE_NOMEM;
    }
    wb_init( &job.wb, wdoc, &wdoc->body );
    pages = pdfview_pages();
    for ( job.page = 0; job.page < pages; job.page++ ) {
        pdf_trap = &trap;
        why = rt_setjmp( &trap );
        if ( why == 0 ) {
            pdf_page_begin();
            page_convert();
        }
        text_sink2 = NULL;
        pdf_page_end();
        pdf_trap = NULL;
        if ( why == PE_NOMEM || wd_failed( wdoc ) ) {
            result = WE_NOMEM;
            break;
        }
        /* a page that is damaged is passed over: the rest is still text */
    }
    wb_done( &job.wb );

    /* what the file says of itself */
    pdf_trap = &trap;
    if ( rt_setjmp( &trap ) == 0 ) {
        pdf_page_begin();
        info = obj_dict( dict_get( doc.trailer, "Info" ) );
        if ( info ) {
            info_text( info, "Title", wdoc->title );
            info_text( info, "Author", wdoc->author );
            info_text( info, "Subject", wdoc->subject );
            info_text( info, "Keywords", wdoc->keywords );
        }
    }
    pdf_page_end();
    pdf_trap = NULL;
    pdfview_close();
    xfree( job.looks );
    job.looks = NULL;

    if ( result == WE_OK ) {
        page_setup( wdoc );
        if ( !headings( wdoc ) ) {
            result = WE_NOMEM;
        }
        if ( job.images ) {
            wdoc->lost |= LOST_IMAGES;
        }
        if ( job.columns || job.turned ) {
            wdoc->lost |= LOST_LAYOUT;
        }
        if ( job.unknown ) {
            wdoc->lost |= LOST_TEXT;
        }
    }
    return wd_failed( wdoc ) ? WE_NOMEM : result;
}

const FORMAT fmt_pdf = {
    FMT_PDF, "PDF", "PDF", fp_detect, fp_load, pdfw_save, pdfw_variants, 4
};
