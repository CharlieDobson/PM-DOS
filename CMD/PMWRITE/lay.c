/*
 * LAY.C - a story broken into lines and pages (LAY.H).
 *
 * HOW A LINE IS BROKEN.  Characters are added until one does not fit;
 * the line then ends after the last space, hyphen or tab that was
 * passed, or, if the line is one long word, just before the character
 * that did not fit.  Spaces at a line's end hang off it: they never
 * push a word to the next line and are not counted in its width.
 *
 * ONE FUNCTION SAYS HOW WIDE A THING IS (unit_width), and the breaking
 * (scan_line) and everything that later asks where a character is
 * (line_walk: drawing, the caret, a click) both go through it, so they
 * cannot disagree.  A tab's width depends on where it starts, which is
 * why a line with a tab in it is never centered or set right: the
 * breaking measured it from the left.
 *
 * A TABLE ROW is paragraphs side by side.  Each is set in its cell's
 * column and the row is as tall as its tallest cell; a row is never
 * split between pages.
 *
 * WHAT IS NOT DONE: widow and orphan lines are not kept together,
 * "keep with next" and "keep lines together" are not honoured, and
 * the text screen does not spread a justified line.
 */
#include "fmt.h"
#include "wedit.h"
#include "lay.h"
#include "tf.h"

#define MIN_WIDTH       720     /* a line is never narrower than this */
#define CELL_PAD        72      /* between a cell's edge and its text */
#define LABEL_GAP       120     /* after a list's number, when nothing hangs */
#define MADE_MAX        24

typedef struct {
    u32 start, len;
    s32 x, y, width;
    s32 extra;                  /* added to each stretchable space */
    u16 height, base;
    u8  flags;                  /* LL_* */
    u8  tabs;                   /* it has a tab in it */
    u16 spare;
} LLINE;

typedef struct {
    u32    owner;               /* the LAY, and the measuring, it is from */
    LLINE *line;
    u32    nlines;
    s32    col_x, col_w;
    s32    height;              /* with the space above and below */
    s32    top;
    s32    block_top, block_h;
    u32    number;              /* in a numbered list */
    u32    row_first, row_last; /* the row it is in */
    u8     in_row;
    u8     spare[3];
} LPARA;

struct LAY {
    WDOC  *doc;
    STORY *story;
    int    grid;
    int    hidden;
    s32    width_set, width;
    u32    owner;
    u32    serial;
    u32    epoch;               /* lay_epoch when it was last brought up to date */
    int    fresh;               /* nothing has changed since lay_update */
    s32    height;
    LPAGE *page;
    u32    npages, page_cap;
    u32    page_no, page_count;
    u32   *note_no;             /* what number each note is shown as */
    u32    note_cap;
    u16    made[MADE_MAX];
};

/* what a run of characters with one look measures as */
typedef struct {
    TFACE *face;
    s32    size;                /* twips */
    s32    rise;
    s32    asc, desc;
    u8     upper, small, hidden, spare;
} RUNM;

enum { U_CHAR, U_TAB, U_OBJECT, U_MADE, U_SKIP };

typedef struct {
    u32  off, units;
    s32  x, w;
    int  kind;                  /* U_* */
    u16  attr;
    u8   lead;
    u8   upper;
    s32  size, rise;
    int  obj;
    s32  obj_h;
    u32  made;                  /* U_MADE: units in lay->made */
} LUNIT;

static u32 next_owner;

/*
 * A paragraph keeps one set of lines, and two LAYs of the same story -
 * the screen's, and the one a PDF or a printing is made from - take it
 * from each other.  Every taking counts one here, and a LAY that was up
 * to date at another count is not: so the screen's lines are made again
 * after a PDF has been written, without anyone having to say so.
 */
static u32 lay_epoch;

/* ------------------------------------------------------------------ */
/* small things                                                        */
/* ------------------------------------------------------------------ */

static s32 grid_x( const LAY *lay, s32 val )
{
    if ( !lay->grid ) {
        return val;
    }
    if ( val < 0 ) {
        return -((-val + GRID_W / 2) / GRID_W * GRID_W);
    }
    return (val + GRID_W / 2) / GRID_W * GRID_W;
}

static s32 grid_y( const LAY *lay, s32 val )
{
    return lay->grid ? (val + GRID_H / 2) / GRID_H * GRID_H : val;
}

static LPARA *lp_of( const LAY *lay, u32 pi )
{
    LPARA *lp;

    if ( pi >= lay->story->count ) {
        return NULL;
    }
    lp = (LPARA *)lay->story->para[pi]->lay;
    return lp != NULL && lp->owner == lay->owner ? lp : NULL;
}

static void lay_drop( WPARA *para )
{
    LPARA *lp = (LPARA *)para->lay;

    if ( lp != NULL ) {
        xfree( lp->line );
        xfree( lp );
        para->lay = NULL;
    }
}

static u32 to_upper( u32 code )
{
    if ( (code >= 'a' && code <= 'z') || (code >= 0xE0 && code <= 0xFE && code != 0xF7) ||
         (code >= 0x3B1 && code <= 0x3C9 && code != 0x3C2) || (code >= 0x430 && code <= 0x44F) ) {
        return code - 0x20;
    }
    return code;
}

static int is_lower( u32 code )
{
    return to_upper( code ) != code;
}

static void run_metrics( LAY *lay, const CHP *chp, RUNM *rm )
{
    int asc, desc, gap;

    mem_set( rm, 0, sizeof( RUNM ) );
    rm->hidden = (u8)((chp->flags & CF_HIDDEN) && !lay->hidden);
    if ( lay->grid ) {
        rm->size = GRID_H;
        rm->asc = GRID_H * 4 / 5;
        rm->desc = GRID_H / 5;
        rm->upper = (u8)((chp->flags & (CF_ALLCAPS | CF_SMALLCAPS)) != 0);
        return;
    }
    rm->face = tf_face( &lay->doc->font[chp->font < lay->doc->nfont ? chp->font : 0], chp->flags );
    rm->size = (s32)chp->size * 10;
    if ( chp->flags & (CF_SUPER | CF_SUB) ) {
        rm->rise = (chp->flags & CF_SUPER) ? rm->size / 3 : -(rm->size / 6);
        rm->size = rm->size * 2 / 3;
    }
    rm->upper = (u8)((chp->flags & CF_ALLCAPS) != 0);
    rm->small = (u8)((chp->flags & CF_SMALLCAPS) != 0);
    tf_metrics( rm->face, &asc, &desc, &gap );
    rm->asc = (s32)asc * rm->size / 1000;
    rm->desc = (s32)(desc + gap) * rm->size / 1000;
    if ( rm->rise > 0 ) {
        rm->asc += rm->rise;
    } else {
        rm->desc -= rm->rise;
    }
}

/* how wide one character is in a run, and how it is to be drawn */
static s32 char_adv( LAY *lay, const RUNM *rm, u32 code, s32 *size, int *upper )
{
    *size = rm->size;
    *upper = rm->upper;
    if ( lay->grid ) {
        return GRID_W;
    }
    if ( rm->small && is_lower( code ) ) {
        *size = rm->size * 4 / 5;
        *upper = 1;
    }
    if ( *upper ) {
        code = to_upper( code );
    }
    if ( code == WCH_NBSP ) {
        code = ' ';
    } else if ( code == WCH_NBHYPHEN ) {
        code = '-';
    }
    return (s32)((tf_advance( rm->face, code ) * (u32)*size + 500) / 1000);
}

static s32 made_width( LAY *lay, const RUNM *rm, u32 count )
{
    s32 total = 0, size;
    u32 index;
    int upper;

    for ( index = 0; index < count; index++ ) {
        total += char_adv( lay, rm, lay->made[index], &size, &upper );
    }
    return total;
}

static u32 made_put( LAY *lay, u32 used, const char *text )
{
    while ( *text && used < MADE_MAX ) {
        lay->made[used++] = (u16)(u8)*text++;
    }
    return used;
}

static u32 made_number( LAY *lay, u32 used, u32 value )
{
    char digits[12];

    fmt_dec( value, 0, digits );
    return made_put( lay, used, digits );
}

/* a field's value into lay->made -> how many units */
static u32 field_text( LAY *lay, u32 code )
{
    switch ( code ) {
    case WCH_PAGENUM:
        return made_number( lay, 0, lay->page_no );
    case WCH_NUMPAGES:
        return made_number( lay, 0, lay->page_count );
    case WCH_DATE:
        return made_put( lay, 0, "(date)" );
    case WCH_TIME:
        return made_put( lay, 0, "(time)" );
    case WCH_FILENAME:
        return made_put( lay, 0, "(file)" );
    }
    return 0;
}

/* a list's bullet or number into lay->made -> how many units */
static u32 label_text( LAY *lay, const PAP *pap, u32 number )
{
    static const char *const roman[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    static const u16 worth[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    char text[MADE_MAX];
    u32 used = 0, index, value;
    int capital = pap->list == LIST_UPPER || pap->list == LIST_ROMAN_UPPER;

    switch ( pap->list ) {
    case LIST_BULLET:
        lay->made[0] = (u16)(lay->grid ? 0x00B7 : 0x2022);
        return 1;
    case LIST_NUMBER:
        used = made_number( lay, 0, number );
        break;
    case LIST_LOWER:
    case LIST_UPPER:
        /* a to z, then aa to zz, as Word numbers them */
        value = number ? number - 1 : 0;
        for ( index = 0; index <= value / 26 && used < 8; index++ ) {
            lay->made[used++] = (u16)('a' + value % 26);
        }
        break;
    case LIST_ROMAN_LOWER:
    case LIST_ROMAN_UPPER:
        value = number > 3999 ? 3999 : number;
        text[0] = 0;
        for ( index = 0; index < 13 && str_len( text ) < MADE_MAX - 4; index++ ) {
            while ( value >= worth[index] && str_len( text ) < MADE_MAX - 4 ) {
                str_cpy( text + str_len( text ), roman[index] );
                value -= worth[index];
            }
        }
        used = made_put( lay, 0, text );
        break;
    default:
        return 0;
    }
    if ( capital ) {
        for ( index = 0; index < used; index++ ) {
            lay->made[index] = (u16)to_upper( lay->made[index] );
        }
    }
    if ( used < MADE_MAX ) {
        lay->made[used++] = '.';
    }
    return used;
}

/* the look a paragraph's own furniture has: its first character's, or
   its style's when it has no text */
static u16 para_chp( LAY *lay, const WPARA *para )
{
    if ( para->len ) {
        return para->attr[0];
    }
    return wd_chp( lay->doc, &lay->doc->style[para->pap.style < lay->doc->nstyle ? para->pap.style : 0].chp );
}

/* ------------------------------------------------------------------ */
/* one unit: a character, or whatever stands where one is              */
/* ------------------------------------------------------------------ */

/*
 * What is at "off", for everything but a tab: its kind, how many units
 * of text it takes, and its width.  "room" is the widest a picture may
 * be.  A note's number or a field's value is left in lay->made.
 */
static s32 unit_width( LAY *lay, const WPARA *para, u32 off, const RUNM *rm, s32 room, LUNIT *unit )
{
    u32 code = para->text[off];
    const WOBJ *obj;
    RUNM note;
    s32 width, size;
    int which, upper;

    unit->off = off;
    unit->units = 1;
    unit->kind = U_CHAR;
    unit->attr = para->attr[off];
    unit->lead = 0;
    unit->size = rm->size;
    unit->rise = rm->rise;
    unit->upper = rm->upper;
    unit->obj = -1;
    unit->obj_h = 0;
    unit->made = 0;
    if ( rm->hidden || code == WCH_LINE || code == WCH_PAGE || code == WCH_SHY ) {
        unit->kind = U_SKIP;
        return 0;
    }
    if ( code == WCH_OBJ ) {
        which = wd_obj_at( para, off );
        if ( which < 0 || (u32)which >= lay->doc->nobj ) {
            unit->kind = U_SKIP;
            return 0;
        }
        obj = &lay->doc->obj[which];
        unit->obj = which;
        if ( obj->kind == OBJ_NOTE ) {
            /* its number, raised and small */
            unit->kind = U_MADE;
            unit->made = made_number( lay, 0, (u32)which < lay->note_cap ? lay->note_no[which] : 0 );
            if ( lay->grid ) {
                return (s32)unit->made * GRID_W;
            }
            note = *rm;
            note.size = rm->size * 2 / 3;
            unit->size = note.size;
            unit->rise = rm->size / 3;
            return made_width( lay, &note, unit->made );
        }
        unit->kind = U_OBJECT;
        if ( lay->grid ) {
            unit->obj_h = GRID_H;
            return GRID_W;
        }
        width = obj->width > 0 ? obj->width : 1440;
        unit->obj_h = obj->height > 0 ? obj->height : 1440;
        if ( width > 200000L ) {        /* no paper is so big: and the sums below stay in 32 bits */
            width = 200000L;
        }
        if ( unit->obj_h > 200000L ) {
            unit->obj_h = 200000L;
        }
        if ( room > 30000 ) {
            room = 30000;
        }
        if ( room < MIN_WIDTH ) {
            room = MIN_WIDTH;
        }
        if ( width > room ) {
            /* as wide as there is room for, and in proportion */
            unit->obj_h = (s32)((u32)unit->obj_h / 4 * (u32)room / ((u32)width / 4 + 1));
            width = room;
        }
        if ( unit->obj_h > 30000 ) {
            unit->obj_h = 30000;
        }
        if ( unit->obj_h < 20 ) {
            unit->obj_h = 20;
        }
        return width;
    }
    if ( WCH_IS_FIELD( code ) ) {
        unit->kind = U_MADE;
        unit->made = field_text( lay, code );
        return made_width( lay, rm, unit->made );
    }
    if ( code >= 0xD800 && code <= 0xDBFF && off + 1 < para->len && para->text[off + 1] >= 0xDC00 &&
         para->text[off + 1] <= 0xDFFF ) {
        code = 0x10000UL + ((code - 0xD800) << 10) + (para->text[off + 1] - 0xDC00);
        unit->units = 2;
    }
    width = char_adv( lay, rm, code, &size, &upper );
    unit->size = size;
    unit->upper = (u8)upper;
    return width;
}

/* where the pen is after a tab met at "x" */
static s32 tab_target( LAY *lay, const WPARA *para, u32 off, s32 x, u8 *lead_out )
{
    const PAP *pap = &para->pap;
    RUNM rm;
    LUNIT unit;
    s32 stop = -1, deftab, piece = 0, before = -1, width, target;
    int kind = TAB_LEFT, index;
    u16 attr = 0;
    u32 scan;
    u8 lead = LEAD_NONE;

    for ( index = 0; index < pap->ntabs && pap->tabs != NULL; index++ ) {
        if ( grid_x( lay, pap->tabs[index].pos ) > x ) {
            stop = grid_x( lay, pap->tabs[index].pos );
            kind = pap->tabs[index].kind;
            lead = pap->tabs[index].lead;
            break;
        }
    }
    if ( stop < 0 && pap->first < 0 && x < grid_x( lay, pap->left ) ) {
        stop = grid_x( lay, pap->left );        /* where a hanging indent's text starts */
    }
    if ( stop < 0 ) {
        deftab = grid_x( lay, lay->doc->deftab );
        if ( deftab < GRID_W ) {
            deftab = 720;
        }
        stop = (x / deftab + 1) * deftab;
    }
    if ( lead_out != NULL ) {
        *lead_out = lead;
    }
    if ( kind == TAB_LEFT ) {
        return stop;
    }
    /* the text up to the next tab is set against the stop */
    unit.units = 1;
    for ( scan = off + 1; scan < para->len; scan += unit.units ) {
        if ( para->text[scan] == WCH_TAB || para->text[scan] == WCH_LINE || para->text[scan] == WCH_PAGE ) {
            break;
        }
        if ( scan == off + 1 || para->attr[scan] != attr ) {
            attr = para->attr[scan];
            run_metrics( lay, wd_chp_get( lay->doc, attr ), &rm );
        }
        if ( before < 0 && (para->text[scan] == '.' || para->text[scan] == ',') ) {
            before = piece;
        }
        width = unit_width( lay, para, scan, &rm, MIN_WIDTH, &unit );
        if ( piece > 30000 - width ) {
            break;                      /* further than any page is wide */
        }
        piece += width;
    }
    if ( kind == TAB_RIGHT ) {
        target = stop - piece;
    } else if ( kind == TAB_CENTER ) {
        target = stop - piece / 2;
    } else {
        target = stop - (before >= 0 ? before : piece);
    }
    target = grid_x( lay, target );
    return target > x ? target : x;
}

/* ------------------------------------------------------------------ */
/* breaking                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    u32 end;                    /* the first unit that is not on the line */
    s32 width;                  /* the pen after the last thing that shows */
    s32 asc, desc;
    u32 gaps;                   /* spaces between words, after the last tab */
    int hard;                   /* it ended at a line or page break */
    int page;                   /* ...a page break */
    int tabs;
    int hyphen;                 /* it ends at a soft hyphen, which shows */
} SCAN;

static void scan_line( LAY *lay, const WPARA *para, u32 start, s32 x0, s32 right, SCAN *out )
{
    RUNM rm;
    LUNIT unit;
    SCAN at_break;
    s32 x = x0, width, hyphen;
    u32 off = start, brk = 0, pending = 0, code;
    u16 attr = 0;
    int have_rm = 0, space;

    mem_set( out, 0, sizeof( SCAN ) );
    mem_set( &at_break, 0, sizeof( SCAN ) );
    out->width = x0;
    while ( off < para->len ) {
        code = para->text[off];
        if ( !have_rm || para->attr[off] != attr ) {
            attr = para->attr[off];
            run_metrics( lay, wd_chp_get( lay->doc, attr ), &rm );
            have_rm = 1;
        }
        if ( code == WCH_LINE || code == WCH_PAGE ) {
            if ( out->asc + out->desc == 0 ) {
                out->asc = rm.asc;
                out->desc = rm.desc;
            }
            out->hard = 1;
            out->page = code == WCH_PAGE;
            out->end = off + 1;
            return;
        }
        if ( code == WCH_TAB && !rm.hidden ) {
            width = tab_target( lay, para, off, x, NULL );
            if ( width > right && off > start ) {
                out->end = off;         /* the tab starts the next line */
                return;
            }
            x = width;
            out->width = x;
            out->gaps = 0;
            pending = 0;
            out->tabs = 1;
            if ( rm.asc > out->asc ) {
                out->asc = rm.asc;
            }
            if ( rm.desc > out->desc ) {
                out->desc = rm.desc;
            }
            off++;
            brk = off;
            at_break = *out;
            continue;
        }
        width = unit_width( lay, para, off, &rm, right - x0, &unit );
        space = code == ' ' && unit.kind == U_CHAR;
        if ( !space && width > 0 && x + width > right && off > start ) {
            if ( brk > start ) {
                *out = at_break;
                out->end = brk;
            } else {
                out->end = off;         /* one long word: cut where it stops fitting */
            }
            return;
        }
        x += width;
        if ( unit.kind != U_SKIP ) {
            if ( unit.kind == U_OBJECT ) {
                if ( unit.obj_h > out->asc ) {
                    out->asc = unit.obj_h;
                }
            } else {
                if ( rm.asc > out->asc ) {
                    out->asc = rm.asc;
                }
                if ( rm.desc > out->desc ) {
                    out->desc = rm.desc;
                }
            }
        }
        if ( space ) {
            pending++;
        } else if ( width > 0 ) {
            out->width = x;
            out->gaps += pending;
            pending = 0;
        }
        off += unit.units;
        if ( space || code == '-' ) {
            brk = off;
            at_break = *out;
        } else if ( code == WCH_SHY && !rm.hidden ) {
            /* broken here, it shows as a hyphen - so only if that fits */
            hyphen = lay->grid ? GRID_W : (s32)((tf_advance( rm.face, '-' ) * (u32)rm.size + 500) / 1000);
            if ( out->width + hyphen <= right ) {
                brk = off;
                at_break = *out;
                at_break.width = out->width + hyphen;
                at_break.hyphen = 1;
            }
        }
    }
    out->end = para->len;
}

/* where a paragraph's lines start and how far right they may go */
static void para_bounds( LAY *lay, const WPARA *para, const LPARA *lp, int first, s32 label_w, s32 *x0,
                         s32 *right )
{
    const PAP *pap = &para->pap;
    s32 left = grid_x( lay, pap->left ), start;

    if ( left < 0 ) {
        left = 0;
    }
    *right = lp->col_w - grid_x( lay, pap->right );
    start = left;
    if ( first ) {
        start = left + grid_x( lay, pap->first );
        if ( start < 0 ) {
            start = 0;
        }
        if ( label_w > 0 ) {
            /* the number hangs in the indent when there is one, and the
               text starts where the other lines do */
            start += label_w + (lay->grid ? GRID_W : LABEL_GAP);
            if ( pap->first < 0 && start < left ) {
                start = left;
            }
        }
    }
    if ( *right > lp->col_w ) {
        *right = lp->col_w;
    }
    if ( *right - start < MIN_WIDTH ) {
        start = *right - MIN_WIDTH;
        if ( start < 0 ) {
            start = 0;
            *right = MIN_WIDTH;
        }
    }
    *x0 = start;
}

static s32 label_width( LAY *lay, const WPARA *para, const LPARA *lp )
{
    RUNM rm;
    u32 count;

    if ( para->pap.list == LIST_NONE ) {
        return 0;
    }
    count = label_text( lay, &para->pap, lp->number );
    if ( count == 0 ) {
        return 0;
    }
    run_metrics( lay, wd_chp_get( lay->doc, para_chp( lay, para ) ), &rm );
    rm.hidden = 0;
    return made_width( lay, &rm, count );
}

/* a paragraph's lines, made: 0 if there was no memory */
static int para_break( LAY *lay, WPARA *para, LPARA *lp )
{
    const PAP *pap = &para->pap;
    LLINE *line, *grown;
    RUNM rm;
    SCAN scan;
    s32 x0, right, slack, natural, pitch, y, label_w;
    u32 start = 0, count = 0, cap = 0, mult;
    int more = 1;

    xfree( lp->line );
    lp->line = NULL;
    lp->nlines = 0;
    label_w = label_width( lay, para, lp );
    y = grid_y( lay, pap->before );
    while ( more ) {
        if ( count == cap ) {
            cap = cap ? cap * 2 : 4;
            grown = (LLINE *)try_realloc( lp->line, cap * sizeof( LLINE ) );
            if ( grown == NULL ) {
                xfree( lp->line );
                lp->line = NULL;
                lp->nlines = 0;
                return 0;
            }
            lp->line = grown;
        }
        line = &lp->line[count];
        mem_set( line, 0, sizeof( LLINE ) );
        para_bounds( lay, para, lp, count == 0, label_w, &x0, &right );
        scan_line( lay, para, start, x0, right, &scan );
        if ( scan.end <= start && start < para->len ) {
            scan.end = start + 1;       /* cannot be, but a line always takes something */
        }
        if ( scan.asc + scan.desc == 0 ) {
            /* nothing on it that has a height: the look of what is next
               to it, or the paragraph's own */
            run_metrics( lay, wd_chp_get( lay->doc, para->len ?
                         para->attr[start < para->len ? start : para->len - 1] : para_chp( lay, para ) ), &rm );
            scan.asc = rm.asc;
            scan.desc = rm.desc;
        }
        line->start = start;
        line->len = scan.end - start;
        line->width = scan.width - x0;
        line->tabs = (u8)scan.tabs;
        line->x = x0;
        more = scan.end < para->len || (scan.hard && scan.end == para->len && !scan.page);
        if ( scan.page ) {
            line->flags |= LL_PAGE;
            more = scan.end < para->len;
        }
        if ( !more ) {
            line->flags |= LL_LAST;
        }
        slack = right - scan.width;
        if ( slack > 0 && !scan.tabs ) {
            if ( pap->align == AL_CENTER ) {
                line->x = x0 + (lay->grid ? slack / 2 / GRID_W * GRID_W : slack / 2);
            } else if ( pap->align == AL_RIGHT ) {
                line->x = x0 + slack;
            }
        }
        if ( slack > 0 && pap->align == AL_JUSTIFY && !lay->grid && more && !scan.hard && scan.gaps ) {
            line->extra = slack / (s32)scan.gaps;
        }
        /* how far apart the lines are */
        natural = scan.asc + scan.desc;
        if ( lay->grid ) {
            mult = ((u32)pap->line + GRID_H / 2) / GRID_H;
            pitch = GRID_H * (s32)(mult ? mult : 1);
            line->base = (u16)(pitch - GRID_H / 5);
        } else if ( pap->line_rule == LS_EXACT && pap->line >= 20 ) {
            pitch = pap->line;
            line->base = (u16)(pitch * 4 / 5);
        } else {
            if ( pap->line_rule == LS_ATLEAST ) {
                pitch = natural > (s32)pap->line ? natural : (s32)pap->line;
            } else {
                pitch = (s32)((u32)natural * (pap->line ? pap->line : 240) / 240);
            }
            if ( pitch < 20 ) {
                pitch = 20;
            }
            if ( pitch > 60000 ) {
                pitch = 60000;          /* before the baseline is worked out from it */
            }
            line->base = (u16)(pitch - scan.desc > 0 ? pitch - scan.desc : pitch);
        }
        if ( pitch > 60000 ) {
            pitch = 60000;
        }
        line->height = (u16)pitch;
        line->y = y;
        y += pitch;
        start = scan.end;
        count++;
        if ( count > 100000UL ) {
            break;                      /* a paragraph of a hundred thousand lines is no paragraph */
        }
    }
    lp->nlines = count;
    lp->height = y + grid_y( lay, pap->after );
    return 1;
}

/* ------------------------------------------------------------------ */
/* walking a line                                                      */
/* ------------------------------------------------------------------ */

typedef int (*UNITFN)( void *ctx, const LUNIT *unit );     /* 0 = that will do */

static void line_walk( LAY *lay, const WPARA *para, const LPARA *lp, u32 li, UNITFN fn, void *ctx )
{
    const LLINE *line = &lp->line[li];
    RUNM rm;
    LUNIT unit;
    s32 x = line->x, right, x0, width;
    u32 off = line->start, end = line->start + line->len, last_tab = 0, last_ink = 0, scan;
    u16 attr = 0;
    int have_rm = 0, has_tab = 0;

    if ( end > para->len ) {
        end = para->len;                /* cannot be: lines are dropped when text changes */
    }
    para_bounds( lay, para, lp, li == 0, 0, &x0, &right );
    if ( line->extra ) {
        /* which spaces stretch: those after the last tab and before the
           last thing that shows */
        for ( scan = off; scan < end; scan++ ) {
            if ( para->text[scan] == WCH_TAB ) {
                last_tab = scan;
                has_tab = 1;
            } else if ( para->text[scan] != ' ' ) {
                last_ink = scan;
            }
        }
    }
    while ( off < end ) {
        if ( !have_rm || para->attr[off] != attr ) {
            attr = para->attr[off];
            run_metrics( lay, wd_chp_get( lay->doc, attr ), &rm );
            have_rm = 1;
        }
        if ( para->text[off] == WCH_TAB && !rm.hidden ) {
            mem_set( &unit, 0, sizeof( unit ) );
            unit.off = off;
            unit.units = 1;
            unit.kind = U_TAB;
            unit.attr = attr;
            unit.size = rm.size;
            unit.obj = -1;
            width = tab_target( lay, para, off, x, &unit.lead ) - x;
        } else if ( para->text[off] == WCH_SHY && off + 1 == end && !(line->flags & LL_LAST) && !rm.hidden ) {
            /* the line was broken at it: it shows */
            mem_set( &unit, 0, sizeof( unit ) );
            unit.off = off;
            unit.units = 1;
            unit.kind = U_MADE;
            unit.attr = attr;
            unit.size = rm.size;
            unit.rise = rm.rise;
            unit.obj = -1;
            lay->made[0] = '-';
            unit.made = 1;
            width = made_width( lay, &rm, 1 );
        } else {
            width = unit_width( lay, para, off, &rm, right - x0, &unit );
        }
        unit.x = x;
        unit.w = width;
        if ( !fn( ctx, &unit ) ) {
            return;
        }
        x += width;
        if ( line->extra && para->text[off] == ' ' && off < last_ink && (!has_tab || off > last_tab) ) {
            x += line->extra;
        }
        off += unit.units;
    }
}

/* ------------------------------------------------------------------ */
/* the whole story                                                     */
/* ------------------------------------------------------------------ */

LAY *lay_new( WDOC *doc, STORY *story, int grid )
{
    LAY *lay = (LAY *)try_alloc( sizeof( LAY ) );

    if ( lay == NULL ) {
        return NULL;
    }
    mem_set( lay, 0, sizeof( LAY ) );
    lay->doc = doc;
    lay->story = story;
    lay->grid = grid;
    lay->owner = ++next_owner;
    lay->page_no = lay->page_count = 1;
    wd_lay_drop = lay_drop;
    return lay;
}

void lay_free( LAY *lay )
{
    u32 pi;

    if ( lay == NULL ) {
        return;
    }
    for ( pi = 0; pi < lay->story->count; pi++ ) {
        if ( lp_of( lay, pi ) != NULL ) {
            lay_drop( lay->story->para[pi] );
        }
    }
    xfree( lay->page );
    xfree( lay->note_no );
    xfree( lay );
}

void lay_stale( LAY *lay )
{
    lay->owner = ++next_owner;
    lay->fresh = 0;
}

void lay_set_width( LAY *lay, s32 width )
{
    if ( width != lay->width_set ) {
        lay->width_set = width;
        lay_stale( lay );
    }
}

void lay_show_hidden( LAY *lay, int show )
{
    if ( (show != 0) != lay->hidden ) {
        lay->hidden = show != 0;
        lay_stale( lay );
    }
}

void lay_set_page( LAY *lay, u32 page, u32 pages )
{
    if ( page != lay->page_no || pages != lay->page_count ) {
        lay->page_no = page;
        lay->page_count = pages;
        lay_stale( lay );
    }
}

s32 lay_width( LAY *lay )
{
    return lay->width;
}

s32 lay_height( LAY *lay )
{
    return lay->height;
}

s32 lay_page_height( LAY *lay )
{
    const PAGESETUP *page = &lay->doc->page;
    s32 height = page->height - page->top - page->bottom;

    if ( height < 1440 ) {
        height = 1440;
    }
    return lay->grid ? height / GRID_H * GRID_H : height;
}

static int page_start( LAY *lay, u32 para, u32 line, s32 y )
{
    LPAGE *grown;
    u32 cap;

    if ( lay->npages == lay->page_cap ) {
        cap = lay->page_cap ? lay->page_cap * 2 : 16;
        grown = (LPAGE *)try_realloc( lay->page, cap * sizeof( LPAGE ) );
        if ( grown == NULL ) {
            return 0;
        }
        lay->page = grown;
        lay->page_cap = cap;
    }
    lay->page[lay->npages].para = para;
    lay->page[lay->npages].line = line;
    lay->page[lay->npages].y = y;
    lay->npages++;
    return 1;
}

/* the paragraph's lines brought up to date for a column */
static LPARA *para_measure( LAY *lay, u32 pi, s32 col_x, s32 col_w, u32 number )
{
    WPARA *para = lay->story->para[pi];
    LPARA *lp = (LPARA *)para->lay;
    int again = 0;

    if ( lp == NULL ) {
        lp = (LPARA *)try_alloc( sizeof( LPARA ) );
        if ( lp == NULL ) {
            return NULL;
        }
        mem_set( lp, 0, sizeof( LPARA ) );
        para->lay = lp;
        again = 1;
    }
    if ( lp->owner != lay->owner || lp->col_w != col_w || lp->number != number || lp->line == NULL ) {
        again = 1;
    }
    if ( lp->owner != lay->owner && lp->owner != 0 ) {
        lay_epoch++;                    /* taken from the LAY that had it */
    }
    lp->owner = lay->owner;
    lp->col_x = col_x;
    lp->col_w = col_w;
    lp->number = number;
    lp->in_row = 0;
    if ( again && !para_break( lay, para, lp ) ) {
        lay_drop( para );
        return NULL;
    }
    return lp;
}

/* the number each note is shown as: the order they are met in */
static void number_notes( LAY *lay )
{
    WDOC *doc = lay->doc;
    WPARA *para;
    u32 *grown;
    u32 pi, index, next = 1, obj;

    if ( doc->nobj > lay->note_cap ) {
        grown = (u32 *)try_realloc( lay->note_no, doc->nobj * sizeof( u32 ) );
        if ( grown == NULL ) {
            return;
        }
        mem_set( grown + lay->note_cap, 0, (doc->nobj - lay->note_cap) * sizeof( u32 ) );
        lay->note_no = grown;
        lay->note_cap = doc->nobj;
    }
    for ( pi = 0; pi < doc->body.count; pi++ ) {
        para = doc->body.para[pi];
        for ( index = 0; index < para->nanch; index++ ) {
            obj = para->anch[index].obj;
            if ( obj < lay->note_cap && doc->obj[obj].kind == OBJ_NOTE ) {
                lay->note_no[obj] = next++;
            }
        }
    }
}

/*
 * Where a row's cells are on this device: "rule" gets the left of the
 * first and the right of each, and the count of cells comes back.  A
 * cell too narrow for a line of text is as wide as the narrowest line,
 * and the cells after it start that much further right: at first they
 * stayed where the file put them, and the text of a narrow column was
 * laid over the column beside it.
 */
static int row_edges( const LAY *lay, const TROW *row, s32 *rule )
{
    int count = row->ncells > MAX_CELLS ? MAX_CELLS : row->ncells, index;
    s32 edge;

    rule[0] = grid_x( lay, row->left );
    for ( index = 0; index < count; index++ ) {
        edge = grid_x( lay, row->edge[index] );
        if ( edge - rule[index] < MIN_WIDTH + 2 * CELL_PAD ) {
            edge = rule[index] + MIN_WIDTH + 2 * CELL_PAD;
        }
        rule[index + 1] = edge;
    }
    return count;
}

/* the last paragraph of the table row that starts at "pi", when the
   row is whole and says where its cells are: else "pi" itself */
static u32 row_end( const STORY *story, u32 pi )
{
    u32 scan;

    for ( scan = pi; scan < story->count && (story->para[scan]->pap.flags & PF_INTABLE); scan++ ) {
        if ( story->para[scan]->pap.flags & PF_ROWEND ) {
            if ( story->para[scan]->pap.row != NULL && story->para[scan]->pap.row->ncells > 0 ) {
                return scan;
            }
            break;
        }
        if ( scan - pi > 4096 ) {
            break;                      /* no row has so many paragraphs */
        }
    }
    return pi;
}

int lay_update( LAY *lay )
{
    WDOC *doc = lay->doc;
    STORY *story = lay->story;
    WPARA *para;
    LPARA *lp;
    const TROW *row;
    LLINE *line;
    u32 counters[9];
    s32 rule[MAX_CELLS + 1];
    s32 width, y = 0, used = 0, page_h, cell_y, row_h, left, edge, bottom, block;
    u32 pi = 0, last, scan, li, number, level, cell, index, cells = 1;
    int ok = 1, forced = 0, is_row;

    width = lay->width_set ? lay->width_set : doc->page.width - doc->page.left - doc->page.right;
    if ( lay->grid ) {
        width = width / GRID_W * GRID_W;
    }
    if ( width < 1440 ) {
        width = 1440;
    }
    if ( width != lay->width ) {
        lay->width = width;
        lay->owner = ++next_owner;
        lay->fresh = 0;
    }
    if ( lay->fresh && lay->serial == doc->serial && lay->epoch == lay_epoch ) {
        return 1;
    }
    number_notes( lay );
    mem_set( counters, 0, sizeof( counters ) );
    page_h = lay_page_height( lay );
    lay->npages = 0;
    if ( !page_start( lay, 0, 0, 0 ) ) {
        ok = 0;
    }
    while ( pi < story->count ) {
        para = story->para[pi];
        last = (para->pap.flags & PF_INTABLE) ? row_end( story, pi ) : pi;
        is_row = last != pi || ((para->pap.flags & PF_ROWEND) && para->pap.row != NULL &&
                                para->pap.row->ncells > 0);
        row = is_row ? story->para[last]->pap.row : NULL;
        cell = 0;
        cell_y = 0;
        row_h = 0;
        for ( scan = pi; scan <= last; scan++ ) {
            para = story->para[scan];
            /* its number, if it is in a numbered list */
            number = 0;
            if ( para->pap.list >= LIST_NUMBER && para->pap.list <= LIST_ROMAN_UPPER ) {
                level = para->pap.level < 9 ? para->pap.level : 8;
                counters[level] = para->pap.list_start ? para->pap.list_start : counters[level] + 1;
                for ( index = level + 1; index < 9; index++ ) {
                    counters[index] = 0;
                }
                number = counters[level];
            } else if ( para->pap.list == LIST_NONE && para->len ) {
                mem_set( counters, 0, sizeof( counters ) );
            }
            if ( row != NULL ) {
                if ( scan == pi ) {
                    cells = (u32)row_edges( lay, row, rule );
                }
                index = cell < cells ? cell : cells - 1;
                left = rule[index];
                edge = rule[index + 1];
                lp = para_measure( lay, scan, left + (lay->grid ? GRID_W : CELL_PAD),
                                   edge - left - 2 * (lay->grid ? GRID_W : CELL_PAD), number );
            } else {
                lp = para_measure( lay, scan, 0, width, number );
            }
            if ( lp == NULL ) {
                ok = 0;
                continue;
            }
            lp->top = y + cell_y;
            lp->in_row = (u8)(row != NULL);
            lp->row_first = pi;
            lp->row_last = last;
            cell_y += lp->height;
            if ( cell_y > row_h ) {
                row_h = cell_y;
            }
            if ( row != NULL && (para->pap.flags & PF_CELLEND) ) {
                cell++;
                cell_y = 0;
            }
        }
        for ( scan = pi; scan <= last; scan++ ) {
            lp = lp_of( lay, scan );
            if ( lp != NULL ) {
                lp->block_top = y;
                lp->block_h = row_h;
            }
        }
        /* pages */
        para = story->para[pi];
        lp = lp_of( lay, pi );
        if ( (forced || ((para->pap.flags & PF_PAGEBEFORE) && used > 0)) && lay->npages > 0 &&
             y > lay->page[lay->npages - 1].y ) {
            if ( !page_start( lay, pi, 0, y ) ) {
                ok = 0;
            }
            used = 0;
        }
        forced = 0;
        if ( row != NULL || lp == NULL ) {
            if ( used + row_h > page_h && used > 0 ) {
                if ( !page_start( lay, pi, 0, y ) ) {
                    ok = 0;
                }
                used = 0;
            }
            used += row_h;
        } else {
            bottom = 0;
            for ( li = 0; li < lp->nlines; li++ ) {
                line = &lp->line[li];
                block = line->y + line->height - bottom;
                if ( forced || (used + block > page_h && used > 0) ) {
                    if ( !page_start( lay, pi, li, y + line->y ) ) {
                        ok = 0;
                    }
                    used = 0;
                    block = line->height;
                    forced = 0;
                }
                used += block;
                bottom = line->y + line->height;
                if ( line->flags & LL_PAGE ) {
                    forced = 1;
                }
            }
            used += lp->height - bottom;
        }
        y += row_h;
        pi = last + 1;
    }
    lay->height = y;
    lay->serial = doc->serial;
    lay->epoch = lay_epoch;
    lay->fresh = ok;
    return ok;
}

/* ------------------------------------------------------------------ */
/* asking                                                              */
/* ------------------------------------------------------------------ */

u32 lay_lines( LAY *lay, u32 para )
{
    LPARA *lp = lp_of( lay, para );

    return lp == NULL ? 0 : lp->nlines;
}

int lay_line( LAY *lay, u32 para, u32 line, LINFO *out )
{
    LPARA *lp = lp_of( lay, para );
    const LLINE *from;

    if ( lp == NULL || line >= lp->nlines ) {
        return 0;
    }
    from = &lp->line[line];
    out->start = from->start;
    out->len = from->len;
    out->x = lp->col_x + from->x;
    out->y = lp->top + from->y;
    out->width = from->width;
    out->height = from->height;
    out->base = from->base;
    out->flags = from->flags;
    return 1;
}

void lay_para_box( LAY *lay, u32 para, s32 *top, s32 *height, s32 *block_top, s32 *block_height )
{
    LPARA *lp = lp_of( lay, para );

    *top = lp == NULL ? 0 : lp->top;
    *height = lp == NULL ? 0 : lp->height;
    *block_top = lp == NULL ? 0 : lp->block_top;
    *block_height = lp == NULL ? 0 : lp->block_h;
}

void lay_para_col( LAY *lay, u32 para, s32 *col_x, s32 *col_width )
{
    LPARA *lp = lp_of( lay, para );

    *col_x = lp == NULL ? 0 : lp->col_x;
    *col_width = lp == NULL ? lay->width : lp->col_w;
}

u32 lay_para_at( LAY *lay, s32 y )
{
    LPARA *lp;
    u32 pi;

    for ( pi = 0; pi < lay->story->count; pi++ ) {
        lp = lp_of( lay, pi );
        if ( lp != NULL && lp->block_top + lp->block_h > y ) {
            return pi;
        }
    }
    return lay->story->count;
}

u32 lay_line_of( LAY *lay, WPOS pos )
{
    LPARA *lp = lp_of( lay, pos.para );
    u32 li;

    if ( lp == NULL || lp->nlines == 0 ) {
        return 0;
    }
    for ( li = lp->nlines - 1; li > 0; li-- ) {
        if ( lp->line[li].start <= pos.off ) {
            break;
        }
    }
    return li;
}

typedef struct {
    u32 off;
    s32 x;
    int found;
} CARETCTX;

static int caret_unit( void *ctx, const LUNIT *unit )
{
    CARETCTX *caret = (CARETCTX *)ctx;

    if ( unit->off >= caret->off ) {
        caret->x = unit->x;
        caret->found = 1;
        return 0;
    }
    caret->x = unit->x + unit->w;
    return 1;
}

void lay_caret( LAY *lay, WPOS pos, s32 *x, s32 *y, s32 *height )
{
    LPARA *lp = lp_of( lay, pos.para );
    CARETCTX caret;
    u32 li;

    *x = *y = 0;
    *height = lay->grid ? GRID_H : 240;
    if ( lp == NULL || lp->nlines == 0 ) {
        return;
    }
    li = lay_line_of( lay, pos );
    caret.off = pos.off;
    caret.x = lp->line[li].x;
    caret.found = 0;
    line_walk( lay, lay->story->para[pos.para], lp, li, caret_unit, &caret );
    *x = lp->col_x + caret.x;
    *y = lp->top + lp->line[li].y;
    *height = lp->line[li].height;
}

typedef struct {
    s32 x;
    u32 off;
} HITCTX;

static int hit_unit( void *ctx, const LUNIT *unit )
{
    HITCTX *hit = (HITCTX *)ctx;

    if ( unit->kind == U_SKIP && unit->w == 0 ) {
        return 1;
    }
    if ( hit->x < unit->x + unit->w / 2 ) {
        hit->off = unit->off;
        return 0;
    }
    hit->off = unit->off + unit->units;
    return 1;
}

WPOS lay_hit_line( LAY *lay, u32 para, u32 line, s32 x )
{
    LPARA *lp = lp_of( lay, para );
    const WPARA *text;
    const LLINE *ln;
    HITCTX hit;
    WPOS pos;
    u32 end;

    pos.para = para;
    pos.off = 0;
    if ( lp == NULL || lp->nlines == 0 ) {
        return pos;
    }
    if ( line >= lp->nlines ) {
        line = lp->nlines - 1;
    }
    text = lay->story->para[para];
    ln = &lp->line[line];
    hit.x = x - lp->col_x;
    hit.off = ln->start;
    line_walk( lay, text, lp, line, hit_unit, &hit );
    /* the end of a line that is not the paragraph's last is the start
       of the next: the place before what ends it is the furthest */
    end = ln->start + ln->len;
    if ( end > text->len ) {
        end = text->len;                /* cannot be: lines are dropped when text changes */
    }
    if ( hit.off > text->len ) {
        hit.off = text->len;
    }
    if ( !(ln->flags & LL_LAST) && hit.off >= end && end > ln->start ) {
        hit.off = end - 1;
        if ( hit.off > ln->start && text->text[hit.off] >= 0xDC00 && text->text[hit.off] <= 0xDFFF ) {
            hit.off--;
        }
    }
    pos.off = hit.off;
    return pos;
}

WPOS lay_hit( LAY *lay, s32 x, s32 y )
{
    LPARA *lp, *best_lp = NULL;
    WPOS pos;
    u32 pi, scan, best = 0, li;
    s32 gap, best_gap = 0x7FFFFFFFL;

    pi = lay_para_at( lay, y );
    if ( pi >= lay->story->count ) {
        return wpos_end( lay->story );
    }
    lp = lp_of( lay, pi );
    if ( lp == NULL ) {
        pos.para = pi;
        pos.off = 0;
        return pos;
    }
    best = pi;
    best_lp = lp;
    if ( lp->in_row ) {
        /* the paragraph of the row that the point is in, or nearest to:
           by its column first, then by its height */
        for ( scan = lp->row_first; scan <= lp->row_last && scan < lay->story->count; scan++ ) {
            lp = lp_of( lay, scan );
            if ( lp == NULL ) {
                continue;
            }
            gap = 0;
            if ( x < lp->col_x ) {
                gap = (lp->col_x - x) * 4;
            } else if ( x >= lp->col_x + lp->col_w ) {
                gap = (x - (lp->col_x + lp->col_w) + 1) * 4;
            }
            if ( y < lp->top ) {
                gap += lp->top - y;
            } else if ( y >= lp->top + lp->height ) {
                gap += y - (lp->top + lp->height) + 1;
            }
            if ( gap < best_gap ) {
                best_gap = gap;
                best = scan;
                best_lp = lp;
            }
        }
    }
    for ( li = 0; li + 1 < best_lp->nlines; li++ ) {
        if ( best_lp->top + best_lp->line[li].y + best_lp->line[li].height > y ) {
            break;
        }
    }
    return lay_hit_line( lay, best, li, x );
}

u32 lay_pages( LAY *lay )
{
    return lay->npages ? lay->npages : 1;
}

const LPAGE *lay_page( LAY *lay, u32 index )
{
    static const LPAGE none = { 0, 0, 0 };

    if ( index >= lay->npages ) {
        return lay->npages ? &lay->page[lay->npages - 1] : &none;
    }
    return &lay->page[index];
}

u32 lay_page_of( LAY *lay, WPOS pos )
{
    u32 line = lay_line_of( lay, pos ), index;

    for ( index = lay->npages; index > 1; index-- ) {
        if ( lay->page[index - 1].para < pos.para ||
             (lay->page[index - 1].para == pos.para && lay->page[index - 1].line <= line) ) {
            return index - 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* pieces to draw                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    LAY        *lay;
    const WPARA *para;
    LAYSEG      fn;
    void       *ctx;
    LSEG        seg;
    int         open;           /* a SEG_TEXT is being gathered */
    s32         col_x;          /* the column's left: a line's own x counts from there */
} SEGCTX;

static void seg_flush( SEGCTX *sc )
{
    if ( sc->open ) {
        sc->fn( sc->ctx, &sc->seg );
        sc->open = 0;
    }
}

static int seg_unit( void *ctx, const LUNIT *unit )
{
    SEGCTX *sc = (SEGCTX *)ctx;
    LSEG *seg = &sc->seg;

    if ( unit->kind == U_CHAR ) {
        if ( sc->open && (seg->chp != unit->attr || seg->size != unit->size || seg->upper != unit->upper ||
                          seg->x + seg->width != sc->col_x + unit->x || seg->off + seg->len != unit->off) ) {
            seg_flush( sc );
        }
        if ( !sc->open ) {
            mem_set( seg, 0, sizeof( LSEG ) );
            seg->kind = SEG_TEXT;
            seg->x = sc->col_x + unit->x;
            seg->off = unit->off;
            seg->text = sc->para->text + unit->off;
            seg->chp = unit->attr;
            seg->size = unit->size;
            seg->rise = unit->rise;
            seg->upper = unit->upper;
            seg->obj = -1;
            sc->open = 1;
        }
        seg->width += unit->w;
        seg->len += unit->units;
        seg->count = seg->len;
        return 1;
    }
    seg_flush( sc );
    if ( unit->kind == U_SKIP ) {
        return 1;
    }
    mem_set( seg, 0, sizeof( LSEG ) );
    seg->x = sc->col_x + unit->x;
    seg->width = unit->w;
    seg->off = unit->off;
    seg->len = unit->units;
    seg->chp = unit->attr;
    seg->size = unit->size;
    seg->rise = unit->rise;
    seg->obj = -1;
    if ( unit->kind == U_TAB ) {
        seg->kind = SEG_TAB;
        seg->lead = unit->lead;
    } else if ( unit->kind == U_OBJECT ) {
        seg->kind = SEG_OBJECT;
        seg->obj = unit->obj;
        seg->height = unit->obj_h;
    } else {
        seg->kind = SEG_MADE;
        seg->text = sc->lay->made;
        seg->count = unit->made;
        seg->obj = unit->obj;
    }
    sc->fn( sc->ctx, seg );
    return 1;
}

void lay_segs( LAY *lay, u32 para, u32 line, LAYSEG fn, void *ctx )
{
    LPARA *lp = lp_of( lay, para );
    const WPARA *text;
    SEGCTX sc;
    RUNM rm;
    LSEG label;
    s32 x0, right;
    u32 count;

    if ( lp == NULL || line >= lp->nlines ) {
        return;
    }
    text = lay->story->para[para];
    if ( line == 0 && text->pap.list != LIST_NONE ) {
        /* the list's bullet or number, where the first line would have
           started without one */
        count = label_text( lay, &text->pap, lp->number );
        if ( count ) {
            mem_set( &label, 0, sizeof( label ) );
            label.kind = SEG_MADE;
            label.chp = para_chp( lay, text );
            run_metrics( lay, wd_chp_get( lay->doc, label.chp ), &rm );
            rm.hidden = 0;
            para_bounds( lay, text, lp, 1, 0, &x0, &right );
            label.x = lp->col_x + x0;
            label.width = made_width( lay, &rm, count );
            label.off = 0;
            label.text = lay->made;
            label.count = count;
            label.size = rm.size;
            label.rise = rm.rise;
            label.obj = -1;
            fn( ctx, &label );
        }
    }
    mem_set( &sc, 0, sizeof( sc ) );
    sc.lay = lay;
    sc.para = text;
    sc.fn = fn;
    sc.ctx = ctx;
    sc.col_x = lp->col_x;
    line_walk( lay, text, lp, line, seg_unit, &sc );
    seg_flush( &sc );
}

int lay_row_rules( LAY *lay, u32 para, s32 *top, s32 *height, s32 *edges, int *count )
{
    LPARA *lp = lp_of( lay, para );
    const TROW *row;

    if ( lp == NULL || !lp->in_row || lp->row_last >= lay->story->count ) {
        return 0;
    }
    row = lay->story->para[lp->row_last]->pap.row;
    if ( row == NULL || !row->borders || row->ncells == 0 ) {
        return 0;
    }
    *top = lp->block_top;
    *height = lp->block_h;
    *count = row_edges( lay, row, edges );
    return 1;
}
