/*
 * F_LWP.C - Lotus Word Pro (.LWP), releases 97 and Millennium: read only.
 *
 * THE FILE IS A STORE OF OBJECTS.  It starts "WordPro", and at 10h has
 * a header object "LWP7" with the file's revision and the place of the
 * index.  The index - a root and its leaves - says where every object
 * is by its number (a time stamp and a count).  An object has a short
 * header (its kind, its number, its size) and then its fields, with
 * runs of zero bytes packed away in most of them.
 *
 * WHAT IS READ.  A story (kind 13) is a list of paragraphs (kind 0),
 * each a list of "fribs": text, a tab, a break, a table, a footnote.
 * Every story says which layout holds it, and the layout's kind says
 * what the story is: the page's is the body, a header's or footer's
 * is that, a table cell's belongs to a table, a footnote cell's to a
 * footnote.  So the stories are found from the index alone, and a
 * table's cells by walking the layouts under the table frib's layout.
 * A paragraph's look comes from its style (kind 23): the font, and the
 * alignment, indent and spacing objects the style points at.  Fonts
 * are two tables inside the document object, behind a long row of
 * other fields; if that row cannot be followed the text is still read,
 * in one font.
 *
 * WHAT IS NOT.  Releases before 97 (file revision under 0Bh) number
 * and pack their objects another way and no file of that kind was at
 * hand: WE_UNSUPPORTED.  Files Word Pro compressed as a whole are not
 * recognized.  A paragraph's own changes to its style, bullets and
 * numbers, tab stops, pictures and frames are passed over; a table's
 * columns are set out evenly; a footnote's text is matched to its call
 * by order.  The facts are from another program's reader and from
 * seven real files.
 *
 * NOT WRITTEN.  Nobody has described how Word Pro checks a file it
 * opens - the index, the time table, the forty-odd kinds of object a
 * document must have - and there is no Word Pro here to try one in, so
 * a file from this code could not be shown to open.  PMWRITE writes
 * Ami Pro, Word and RTF files, all of which Word Pro reads.
 */
#include "fmt.h"

#define LWP_BASE        0x10UL      /* offsets in the file count from here */
#define LWP_EXPAND      0x10000UL   /* the most an object unpacks to */
#define LWP_MAX_OBJS    1000000UL
#define LWP_MAX_DEPTH   8
#define LWP_MAX_STYLES  256

/* kinds of object */
#define VO_PARA         0
#define VO_DOCUMENT     5
#define VO_STORY        13
#define VO_CELL         18
#define VO_PARASTYLE    23
#define VO_CONNCELL     28
#define VO_ROW          32
#define VO_FNROW        59
#define VO_FNCELL       61
#define VO_HEADER       139
#define VO_FOOTER       140
#define VO_PAGE         143
#define VO_CONTON       145
#define VO_CONTFROM     146

enum { LWP_OTHER, LWP_BODY, LWP_HEADER, LWP_FOOTER, LWP_CELL, LWP_NOTE };

typedef struct {
    u32 low, high;
} LWPID;
#define LWP_NULL( id )  ((id).low == 0 && (id).high == 0)

typedef struct {
    u32 low;
    u16 high, spare;
    u32 pos;
} LWPKEY;

typedef struct {
    const u8 *ptr;
    u32   len, pos;
    int   bad;                  /* something was asked for past the end */
    u8   *heap;                 /* the unpacked bytes, when it was packed */
    u32   type;
} LWPCUR;

typedef struct {
    LWPID id, layout, first;
    u8    kind, used;
} LWPSTORY;

typedef struct {
    LWPID id;
    CHP   chp;
    PAP   pap;                  /* tabs and row are NULL */
} LWPSTYLE;

typedef struct {
    u8  bits;                   /* 01h it sets the size, 02h the color, 10h the face */
    u8  spare;
    u16 size, face;
    u32 color;
} LWPFNAME;

typedef struct {
    u16 bits, mask;
    u8  cap, under;
} LWPFATTR;

typedef struct {
    const u8 *data;
    u32   len;
    WDOC *doc;
    u32   rev;
    LWPKEY *key;
    u32   nkey, key_cap;
    u32  *time;
    u32   ntime;
    LWPSTORY *story;
    u32   nstory, story_cap;
    LWPSTYLE *style;
    u32   nstyle;
    LWPFNAME *fname;
    u32   nfname;
    LWPFATTR *fattr;
    u32   nfattr;
    u16  *face;                 /* the document's number for each face */
    u32   nface;
    u32   budget;               /* objects that may still be opened */
    u32   depth;
    u32   next_note;
    s32   text_width;
    int   nomem;
} LWP;

/* ------------------------------------------------------------------ */
/* an object's bytes                                                   */
/* ------------------------------------------------------------------ */

static u32 lc_8( LWPCUR *cur )
{
    if ( cur->bad || cur->pos >= cur->len ) {
        cur->bad = 1;
        return 0;
    }
    return cur->ptr[cur->pos++];
}

static u32 lc_16( LWPCUR *cur )
{
    u32 value;

    if ( cur->bad || !IN_RANGE( cur->pos, 2, cur->len ) ) {
        cur->bad = 1;
        return 0;
    }
    value = LE16( cur->ptr + cur->pos );
    cur->pos += 2;
    return value;
}

static u32 lc_32( LWPCUR *cur )
{
    u32 value;

    if ( cur->bad || !IN_RANGE( cur->pos, 4, cur->len ) ) {
        cur->bad = 1;
        return 0;
    }
    value = LE32( cur->ptr + cur->pos );
    cur->pos += 4;
    return value;
}

static void lc_skip( LWPCUR *cur, u32 count )
{
    if ( cur->bad || !IN_RANGE( cur->pos, count, cur->len ) ) {
        cur->bad = 1;
        return;
    }
    cur->pos += count;
}

/* what a later release added to a record: words, to one that is zero */
static void lc_extra( LWPCUR *cur )
{
    while ( lc_16( cur ) != 0 ) {
    }
}

/* an object's number as objects name each other: a byte that picks a
   time stamp from the table, or 0 and the stamp itself; then the count */
static void lc_id( const LWP *lwp, LWPCUR *cur, LWPID *id )
{
    u32 index = lc_8( cur );

    if ( index != 0 ) {
        id->low = index <= lwp->ntime ? lwp->time[index - 1] : 0xFFFFFFFFUL;
    } else {
        id->low = lc_32( cur );
    }
    id->high = lc_16( cur );
}

/* a head and, when there is one, a tail */
static void lc_headtail( const LWP *lwp, LWPCUR *cur, LWPID *head )
{
    LWPID tail;

    lc_id( lwp, cur, head );
    if ( !LWP_NULL( *head ) ) {
        lc_id( lwp, cur, &tail );
    }
}

/* a string: its size on disk, its length, its bytes.  "out" may be
   NULL; what is kept is the ASCII of it. */
static void lc_atom( LWPCUR *cur, char *out, u32 max )
{
    u32 disk = lc_16( cur ), len = lc_16( cur ), index, have = 0;

    if ( out != NULL ) {
        out[0] = 0;
    }
    if ( len == 0 || disk < 2 || cur->bad ) {
        return;
    }
    disk -= 2;
    if ( !IN_RANGE( cur->pos, disk, cur->len ) ) {
        cur->bad = 1;
        return;
    }
    for ( index = 0; out != NULL && index < disk && have + 1 < max; index++ ) {
        if ( cur->ptr[cur->pos + index] >= 0x20 && cur->ptr[cur->pos + index] < 0x7F ) {
            out[have++] = (char)cur->ptr[cur->pos + index];
        }
    }
    if ( out != NULL ) {
        out[have] = 0;
    }
    cur->pos += disk;
}

/* a string nobody wants: a size and that many bytes */
static void lc_path( LWPCUR *cur )
{
    lc_skip( cur, lc_16( cur ) );
}

static void lc_close( LWPCUR *cur )
{
    xfree( cur->heap );
    cur->heap = NULL;
}

/* runs of zeros put back.  The top two bits of a byte say what follows:
   00 zeros; 01 zeros and then bytes; 10 one zero and bytes; 11 bytes. */
static int lwp_expand( const u8 *src, u32 len, u8 *dst, u32 *out_len )
{
    u32 pos = 0, out = 0, code, zeros, count;

    while ( pos < len ) {
        code = src[pos++];
        zeros = 0;
        count = 0;
        switch ( code & 0xC0 ) {
        case 0x00:
            zeros = (code & 0x3F) + 1;
            break;
        case 0x40:
            zeros = ((code & 0x38) >> 3) + 1;
            count = (code & 7) + 1;
            break;
        case 0x80:
            zeros = 1;
            count = (code & 0x3F) + 1;
            break;
        default:
            count = (code & 0x3F) + 1;
            break;
        }
        if ( !IN_RANGE( pos, count, len ) || !IN_RANGE( out, zeros + count, LWP_EXPAND ) ) {
            return 0;
        }
        mem_set( dst + out, 0, zeros );
        mem_cpy( dst + out + zeros, src + pos, count );
        out += zeros + count;
        pos += count;
    }
    *out_len = out;
    return 1;
}

/* the object whose header is at "at" in the file -> 1 and a cursor on
   its fields, to be closed */
static int lwp_object( LWP *lwp, u32 at, LWPCUR *cur )
{
    u32 flags, pos, size, need;

    mem_set( cur, 0, sizeof( LWPCUR ) );
    if ( !IN_RANGE( at, 4, lwp->len ) ) {
        return 0;
    }
    cur->type = LE16( lwp->data + at );
    flags = lwp->data[at + 2];
    /* its number, its version and the one before, each as long as the
       flags say; then its size */
    pos = at + 4 + (lwp->data[at + 3] ? 0 : 4) + 2;
    pos += (flags & 3) == 3 ? 4 : (flags & 3);
    pos += (flags & 0x0C) == 4 ? 1 : (flags & 0x0C) == 8 ? 2 : 4;
    pos += flags & 0x40 ? 4 : 0;
    need = (flags & 0x30) == 0x10 ? 1 : (flags & 0x30) == 0x20 ? 2 : 4;
    if ( !IN_RANGE( pos, need, lwp->len ) ) {
        return 0;
    }
    size = need == 1 ? lwp->data[pos] : need == 2 ? LE16( lwp->data + pos ) : LE32( lwp->data + pos );
    pos += need;
    if ( !IN_RANGE( pos, size, lwp->len ) ) {
        return 0;
    }
    if ( !(flags & 0x80) ) {
        cur->ptr = lwp->data + pos;
        cur->len = size;
        return 1;
    }
    cur->heap = (u8 *)try_alloc( LWP_EXPAND );
    if ( cur->heap == NULL ) {
        lwp->nomem = 1;
        return 0;
    }
    if ( !lwp_expand( lwp->data + pos, size, cur->heap, &cur->len ) ) {
        lc_close( cur );
        return 0;
    }
    cur->ptr = cur->heap;
    return 1;
}

/* ------------------------------------------------------------------ */
/* the index                                                           */
/* ------------------------------------------------------------------ */

/* "count" numbers, the first whole and the rest as what they add to the
   one before, and then where each object is */
static int lwp_keys( LWP *lwp, LWPCUR *cur, u32 count )
{
    LWPKEY *fresh;
    u32 index, low = 0, high = 0, step, cap;

    if ( count == 0 ) {
        return 1;
    }
    if ( count > LWP_MAX_OBJS - lwp->nkey ) {
        return 0;
    }
    if ( lwp->nkey + count > lwp->key_cap ) {
        cap = lwp->nkey + count + 256;
        fresh = (LWPKEY *)try_realloc( lwp->key, cap * sizeof( LWPKEY ) );
        if ( fresh == NULL ) {
            lwp->nomem = 1;
            return 0;
        }
        lwp->key = fresh;
        lwp->key_cap = cap;
    }
    for ( index = 0; index < count && !cur->bad; index++ ) {
        step = index == 0 ? 255 : lc_8( cur );
        if ( step == 255 ) {
            low = lc_32( cur );
            high = lc_16( cur );
        } else {
            high = (high + step + 1) & 0xFFFF;
        }
        lwp->key[lwp->nkey + index].low = low;
        lwp->key[lwp->nkey + index].high = (u16)high;
        lwp->key[lwp->nkey + index].spare = 0;
    }
    for ( index = 0; index < count && !cur->bad; index++ ) {
        lwp->key[lwp->nkey + index].pos = lc_32( cur );
    }
    if ( cur->bad ) {
        return 0;
    }
    lwp->nkey += count;
    return 1;
}

static int lwp_times( LWP *lwp, LWPCUR *cur )
{
    u32 count = lc_16( cur ), index;

    if ( cur->bad || !IN_RANGE( cur->pos, count * 4, cur->len ) ) {
        return 0;
    }
    lwp->time = (u32 *)try_alloc( (count + 1) * sizeof( u32 ) );
    if ( lwp->time == NULL ) {
        lwp->nomem = 1;
        return 0;
    }
    for ( index = 0; index < count; index++ ) {
        lwp->time[index] = lc_32( cur );
    }
    lwp->ntime = count;
    return 1;
}

static int lwp_key_less( const LWPKEY *lhs, const LWPKEY *rhs )
{
    return lhs->low < rhs->low || (lhs->low == rhs->low && lhs->high < rhs->high);
}

static void lwp_sort( LWPKEY *key, u32 count )
{
    LWPKEY hold;
    u32 gap, index, back;

    for ( gap = count / 2; gap > 0; gap = gap == 2 ? 1 : gap * 5 / 11 ) {
        for ( index = gap; index < count; index++ ) {
            hold = key[index];
            for ( back = index; back >= gap && lwp_key_less( &hold, &key[back - gap] ); back -= gap ) {
                key[back] = key[back - gap];
            }
            key[back] = hold;
        }
    }
}

static int lwp_index( LWP *lwp, u32 root )
{
    LWPCUR cur, leaf;
    u32 count, index, at, mark;
    int ok = 0;

    if ( root > lwp->len || !lwp_object( lwp, root + LWP_BASE, &cur ) ) {
        return 0;
    }
    if ( cur.type == 0xFFFB ) {
        /* a small file: the root is the only leaf */
        ok = lwp_keys( lwp, &cur, lc_16( &cur ) ) && lwp_times( lwp, &cur );
    } else if ( cur.type == 0xFFFC ) {
        count = lc_16( &cur );
        ok = lwp_keys( lwp, &cur, count );
        mark = cur.pos;
        if ( ok && count != 0 ) {
            lc_skip( &cur, (count + 1) * 4 );
        }
        ok = ok && lwp_times( lwp, &cur );
        for ( index = 0; ok && count != 0 && index <= count; index++ ) {
            at = LE32( cur.ptr + mark + index * 4 );
            if ( at > lwp->len || !lwp_object( lwp, at + LWP_BASE, &leaf ) ) {
                ok = 0;
                break;
            }
            ok = leaf.type == 0xFFFE && lwp_keys( lwp, &leaf, lc_16( &leaf ) );
            lc_close( &leaf );
        }
    }
    lc_close( &cur );
    if ( ok ) {
        lwp_sort( lwp->key, lwp->nkey );
    }
    return ok;
}

/* where an object is: its place among the keys, or -1 */
static s32 lwp_find( const LWP *lwp, const LWPID *id )
{
    u32 low = 0, high = lwp->nkey, mid;
    LWPKEY want;

    want.low = id->low;
    want.high = (u16)id->high;
    while ( low < high ) {
        mid = low + (high - low) / 2;
        if ( lwp_key_less( &lwp->key[mid], &want ) ) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if ( low < lwp->nkey && lwp->key[low].low == id->low && lwp->key[low].high == id->high && id->high <= 0xFFFF ) {
        return (s32)low;
    }
    return -1;
}

static int lwp_open( LWP *lwp, const LWPID *id, LWPCUR *cur )
{
    s32 where = LWP_NULL( *id ) ? -1 : lwp_find( lwp, id );

    mem_set( cur, 0, sizeof( LWPCUR ) );
    if ( where < 0 || lwp->budget == 0 || lwp->key[where].pos > lwp->len ) {
        return 0;
    }
    lwp->budget--;
    return lwp_object( lwp, lwp->key[where].pos + LWP_BASE, cur );
}

static u32 lwp_type( const LWP *lwp, const LWPID *id )
{
    s32 where = LWP_NULL( *id ) ? -1 : lwp_find( lwp, id );

    if ( where < 0 || lwp->key[where].pos > lwp->len || !IN_RANGE( lwp->key[where].pos + LWP_BASE, 2, lwp->len ) ) {
        return 0xFFFF;
    }
    return LE16( lwp->data + lwp->key[where].pos + LWP_BASE );
}

/* ------------------------------------------------------------------ */
/* records many objects start with                                     */
/* ------------------------------------------------------------------ */

/* a member of a list that has children and a name: the next one, the
   one before, the first child (and the last, if there is a first), the
   parent, the name */
static void lwp_list( const LWP *lwp, LWPCUR *cur, LWPID *next, LWPID *child, char *name, u32 max )
{
    LWPID skip;

    lc_id( lwp, cur, next );
    lc_id( lwp, cur, &skip );
    lc_headtail( lwp, cur, child );
    lc_id( lwp, cur, &skip );
    lc_atom( cur, name, max );
}

/* ...and one that may also have properties */
static void lwp_plist( const LWP *lwp, LWPCUR *cur, LWPID *next, LWPID *child, char *name, u32 max )
{
    LWPID skip;

    lwp_list( lwp, cur, next, child, name, max );
    if ( lc_8( cur ) != 0 ) {
        lc_id( lwp, cur, &skip );
    }
    lc_extra( cur );
}

/* the three words every "override" starts with */
static void lwp_common( LWPCUR *cur )
{
    lc_skip( cur, 6 );
    lc_extra( cur );
}

static s32 lwp_twips( u32 units )
{
    s32 value = (s32)units;
    int minus = value < 0;
    u32 size = (u32)(minus ? -value : value);

    size = (size / 16384) * 5 + ((size % 16384) * 5) / 16384;
    if ( size > 31680 ) {
        size = 31680;
    }
    return minus ? -(s32)size : (s32)size;
}

/* ------------------------------------------------------------------ */
/* fonts                                                               */
/* ------------------------------------------------------------------ */

/* a font number is two: which size-color-face in the top word, which
   set of attributes in the bottom one, both counted from 1.  Each says
   which of its values it sets; the rest stay as they are. */
static void lwp_font( const LWP *lwp, u32 number, CHP *chp )
{
    static const struct {
        u16 bit, flag;
    } attrs[] = {
        { 0x0001, CF_BOLD }, { 0x0002, CF_ITALIC }, { 0x0004, CF_STRIKE },
        { 0x0100, CF_SUPER }, { 0x0200, CF_SUB }, { 0x0400, CF_SMALLCAPS }
    };
    u32 name = number >> 16, attr = number & 0xFFFF, index;
    const LWPFNAME *fname;
    const LWPFATTR *fattr;

    if ( name >= 1 && name <= lwp->nfname ) {
        fname = &lwp->fname[name - 1];
        if ( (fname->bits & 0x01) && fname->size >= 2 ) {
            chp->size = fname->size;
        }
        if ( fname->bits & 0x02 ) {
            chp->color = fname->color;
        }
        if ( (fname->bits & 0x10) && fname->face >= 1 && fname->face <= lwp->nface ) {
            chp->font = lwp->face[fname->face - 1];
        }
    }
    if ( attr >= 1 && attr <= lwp->nfattr ) {
        fattr = &lwp->fattr[attr - 1];
        for ( index = 0; index < sizeof( attrs ) / sizeof( attrs[0] ); index++ ) {
            if ( fattr->mask & attrs[index].bit ) {
                chp->flags &= (u16)~attrs[index].flag;
                if ( fattr->bits & attrs[index].bit ) {
                    chp->flags |= attrs[index].flag;
                }
            }
        }
        if ( fattr->under >= 1 && fattr->under <= 5 ) {
            chp->flags &= (u16)~(CF_UNDER | CF_DUNDER | CF_WUNDER);
            chp->flags |= (u16)(fattr->under == 2 ? CF_UNDER : fattr->under == 3 || fattr->under == 5 ? CF_DUNDER
                                : fattr->under == 4 ? CF_WUNDER : 0);
        }
        if ( fattr->cap >= 1 && fattr->cap <= 4 ) {
            chp->flags &= (u16)~CF_ALLCAPS;
            if ( fattr->cap == 2 ) {
                chp->flags |= CF_ALLCAPS;
            }
        }
    }
}

/* the font tables, at the cursor: sizes, colors and faces; the faces'
   names; the sets of attributes -> 1 if all of it was there */
static int lwp_font_tables( LWP *lwp, LWPCUR *cur )
{
    char name[FONT_NAME_LEN];
    u32 count, index, red, green, blue, kind, size;

    count = lc_16( cur );
    if ( cur->bad || count > 8192 ) {
        return 0;
    }
    lwp->fname = (LWPFNAME *)try_alloc( (count + 1) * sizeof( LWPFNAME ) );
    if ( lwp->fname == NULL ) {
        lwp->nomem = 1;
        return 0;
    }
    for ( index = 0; index < count && !cur->bad; index++ ) {
        lwp->fname[index].bits = (u8)lc_8( cur );
        lc_8( cur );
        size = lc_32( cur ) / 32768UL;      /* 65536 to the point, kept as half points */
        lwp->fname[index].size = (u16)(size > 1638 ? 0 : size);
        lc_skip( cur, 4 );
        red = lc_16( cur ) & 255;
        green = lc_16( cur ) & 255;
        blue = lc_16( cur ) & 255;
        kind = lc_16( cur );
        lwp->fname[index].color = kind != 0 || (red | green | blue) == 0 ? COLOR_AUTO : RGB( red, green, blue );
        lc_skip( cur, 8 );
        lc_extra( cur );
        lwp->fname[index].face = (u16)lc_16( cur );
        lc_16( cur );
        lc_extra( cur );
        lwp->fname[index].spare = 0;
    }
    lwp->nfname = cur->bad ? 0 : count;
    count = lc_16( cur );
    if ( cur->bad || count > 8192 ) {
        return 0;
    }
    lwp->face = (u16 *)try_alloc( (count + 1) * sizeof( u16 ) );
    if ( lwp->face == NULL ) {
        lwp->nomem = 1;
        return 0;
    }
    for ( index = 0; index < count && !cur->bad; index++ ) {
        lc_atom( cur, name, FONT_NAME_LEN );
        lc_skip( cur, 10 );
        lc_extra( cur );
        lwp->face[index] = (u16)wd_font( lwp->doc, name[0] ? name : "Times New Roman", FAM_ANY, -1 );
    }
    lwp->nface = cur->bad ? 0 : count;
    lc_extra( cur );
    lc_extra( cur );
    count = lc_16( cur );
    if ( cur->bad || count > 8192 ) {
        return 0;
    }
    lwp->fattr = (LWPFATTR *)try_alloc( (count + 1) * sizeof( LWPFATTR ) );
    if ( lwp->fattr == NULL ) {
        lwp->nomem = 1;
        return 0;
    }
    for ( index = 0; index < count && !cur->bad; index++ ) {
        lwp->fattr[index].bits = (u16)lc_16( cur );
        lwp->fattr[index].mask = (u16)lc_16( cur );
        lc_skip( cur, 4 );
        lwp->fattr[index].cap = (u8)lc_8( cur );
        lwp->fattr[index].under = (u8)lc_8( cur );
        lc_extra( cur );
    }
    lwp->nfattr = cur->bad ? 0 : count;
    return !cur->bad && lwp->nface > 0;
}

static void lwp_fonts_drop( LWP *lwp )
{
    xfree( lwp->fname );
    xfree( lwp->face );
    xfree( lwp->fattr );
    lwp->fname = NULL;
    lwp->face = NULL;
    lwp->fattr = NULL;
    lwp->nfname = lwp->nface = lwp->nfattr = 0;
}

/* the way through a document object to its font tables: every record
   before them, in the order Word Pro writes them */
static int lwp_document( LWP *lwp, LWPCUR *cur )
{
    LWPID id, child;
    u32 flags, count, index, tag;

    lwp_plist( lwp, cur, &id, &child, NULL, 0 );
    lc_id( lwp, cur, &id );                 /* the socket, flags */
    lc_16( cur );
    lc_extra( cur );
    flags = lc_32( cur );
    lc_skip( cur, 5 );                      /* how it is sorted: three keys */
    for ( index = 0; index < 3; index++ ) {
        lc_skip( cur, 6 );
        lc_extra( cur );
    }
    lc_extra( cur );
    if ( lc_16( cur ) != 0 ) {              /* named properties: none is known */
        return 0;
    }
    lc_extra( cur );
    lc_path( cur );                         /* macros run at open, close, new */
    lc_path( cur );
    lc_path( cur );
    lc_16( cur );
    lc_extra( cur );
    lc_path( cur );                         /* merge: two files, a filter, a kind */
    lc_path( cur );
    lc_atom( cur, NULL, 0 );
    if ( lc_16( cur ) != 0 ) {
        return 0;
    }
    lc_16( cur );
    lc_extra( cur );
    lc_path( cur );                         /* the style sheet's path, flags */
    lc_16( cur );
    if ( lc_16( cur ) != 0 ) {
        lc_atom( cur, NULL, 0 );
        lc_extra( cur );
    }
    lc_skip( cur, 14 );                     /* line numbering */
    lc_extra( cur );
    count = lc_16( cur );                   /* the user's dictionaries */
    for ( index = 0; index < count && !cur->bad; index++ ) {
        lc_atom( cur, NULL, 0 );
        lc_extra( cur );
    }
    lc_extra( cur );
    if ( !(flags & 0x0800) ) {
        /* not a division of another document: the printer, the versions */
        lc_skip( cur, lc_32( cur ) & 0xFFFF );
        lc_skip( cur, 18 );
        for ( index = 0; index < 5; index++ ) {
            lc_path( cur );
        }
        count = lc_16( cur );
        for ( index = 0; index < count && !cur->bad; index++ ) {
            lc_path( cur );
            lc_extra( cur );
        }
        lc_extra( cur );
        lc_32( cur );
        count = lc_16( cur );
        for ( index = 0; index < count && !cur->bad; index++ ) {
            tag = lc_32( cur );
            lc_skip( cur, lc_16( cur ) );
            if ( tag != 0x72655655UL ) {    /* "UVer" */
                lc_extra( cur );
            }
        }
        lc_extra( cur );
    }
    lc_skip( cur, 6 );                      /* the object manager */
    lc_id( lwp, cur, &id );
    lc_extra( cur );
    lc_id( lwp, cur, &id );                 /* markers, footnotes */
    lc_id( lwp, cur, &id );
    lc_id( lwp, cur, &id );                 /* numbers, bullets */
    lc_extra( cur );
    lc_id( lwp, cur, &id );
    lc_extra( cur );
    /* sections, the layout, fourteen kinds of style, seven lists */
    for ( index = 0; index < 23; index++ ) {
        lc_id( lwp, cur, &id );
    }
    lc_32( cur );
    for ( index = 0; index < 9; index++ ) { /* smart text, the contents */
        lc_id( lwp, cur, &id );
    }
    lc_extra( cur );
    return !cur->bad && lwp_font_tables( lwp, cur );
}

static void lwp_fonts( LWP *lwp )
{
    LWPCUR cur;
    u32 index;
    int ok;

    for ( index = 0; index < lwp->nkey; index++ ) {
        if ( lwp->key[index].pos > lwp->len || !IN_RANGE( lwp->key[index].pos + LWP_BASE, 2, lwp->len ) ||
             LE16( lwp->data + lwp->key[index].pos + LWP_BASE ) != VO_DOCUMENT ) {
            continue;
        }
        if ( !lwp_object( lwp, lwp->key[index].pos + LWP_BASE, &cur ) ) {
            continue;
        }
        ok = lwp_document( lwp, &cur );
        lc_close( &cur );
        if ( ok ) {
            return;
        }
        lwp_fonts_drop( lwp );
    }
}

/* ------------------------------------------------------------------ */
/* styles                                                              */
/* ------------------------------------------------------------------ */

/* a piece of a style is an object of its own: two list pointers, then
   a flag that says whether anything is set, and the three words */
static int lwp_piece( LWP *lwp, const LWPID *id, LWPCUR *cur )
{
    LWPID skip;

    if ( !lwp_open( lwp, id, cur ) ) {
        return 0;
    }
    lc_id( lwp, cur, &skip );
    lc_id( lwp, cur, &skip );
    if ( lc_16( cur ) == 0 || cur->bad ) {
        lc_close( cur );
        return 0;
    }
    lwp_common( cur );
    return 1;
}

/* one of a spacing piece's four records -> what kind (0 a multiple of
   the line, 1 extra, 2 a height) and the two values */
static u32 lwp_space( LWPCUR *cur, u32 *amount, u32 *multiple )
{
    u32 kind = 3;

    *amount = 0;
    *multiple = 0;
    if ( lc_16( cur ) != 0 ) {
        lwp_common( cur );
        kind = lc_16( cur ) & 3;
        *amount = lc_32( cur );
        *multiple = lc_32( cur );
    }
    lc_extra( cur );
    return cur->bad ? 3 : kind;
}

static u16 lwp_gap( u32 kind, u32 amount, u32 multiple )
{
    s32 value = kind == 0 ? (s32)((multiple >> 8) * 240 >> 8) : kind == 3 ? 0 : lwp_twips( amount );

    return (u16)(value < 0 ? 0 : value > 31680 ? 31680 : value);
}

/* what a style or a paragraph says of its alignment, its indents and
   its spacing: each is an object, or none */
static void lwp_pieces( LWP *lwp, PAP *pap, const LWPID *align, const LWPID *indent, const LWPID *spacing )
{
    LWPCUR piece;
    u32 kind, amount, multiple;
    s32 every, first, rest;

    if ( lwp_piece( lwp, align, &piece ) ) {
        static const u8 sides[8] = { AL_LEFT, AL_RIGHT, AL_CENTER, AL_JUSTIFY, AL_JUSTIFY, AL_LEFT, AL_RIGHT, AL_LEFT };

        pap->align = sides[lc_8( &piece ) & 7];
        lc_close( &piece );
    }
    if ( lwp_piece( lwp, indent, &piece ) ) {
        every = lwp_twips( lc_32( &piece ) );
        first = lwp_twips( lc_32( &piece ) );
        rest = lwp_twips( lc_32( &piece ) );
        pap->right = lwp_twips( lc_32( &piece ) );
        if ( !piece.bad ) {
            pap->left = every + rest;
            pap->first = first - rest;
        } else {
            pap->right = 0;
        }
        lc_close( &piece );
    }
    if ( lwp_piece( lwp, spacing, &piece ) ) {
        kind = lwp_space( &piece, &amount, &multiple );
        if ( kind == 0 && multiple != 0 ) {
            pap->line = (u16)((multiple >> 8) * 240 >> 8);
            if ( pap->line < 120 || pap->line > 1200 ) {
                pap->line = 240;
            }
        } else if ( kind == 2 && lwp_twips( amount ) >= 20 ) {
            pap->line_rule = LS_EXACT;
            pap->line = (u16)lwp_twips( amount );
        }
        if ( lwp->rev >= 0x0D ) {
            lwp_space( &piece, &amount, &multiple );
        }
        kind = lwp_space( &piece, &amount, &multiple );
        pap->before = lwp_gap( kind, amount, multiple );
        kind = lwp_space( &piece, &amount, &multiple );
        pap->after = lwp_gap( kind, amount, multiple );
        lc_close( &piece );
    }
}

static const LWPSTYLE *lwp_style( LWP *lwp, const LWPID *id )
{
    static const struct {
        const char *name;
        int kind;
    } known[] = {
        { "Default Text", STY_NORMAL }, { "Title", STY_TITLE },
        { "Subhead", STY_SUBTITLE }, { "Heading 1", STY_HEADING1 }, { "Heading 2", STY_HEADING2 },
        { "Heading 3", STY_HEADING3 }, { "Heading 4", STY_HEADING4 }, { "Heading 5", STY_HEADING5 },
        { "Heading 6", STY_HEADING6 }
    };
    char name[STYLE_NAME_LEN];
    LWPSTYLE *style;
    LWPCUR cur;
    LWPID next, child, skip, align, spacing, indent;
    u32 index, font, final, count;
    int number, what = STY_OTHER;

    for ( index = 0; index < lwp->nstyle; index++ ) {
        if ( lwp->style[index].id.low == id->low && lwp->style[index].id.high == id->high ) {
            return &lwp->style[index];
        }
    }
    if ( lwp->nstyle >= LWP_MAX_STYLES || !lwp_open( lwp, id, &cur ) ) {
        return NULL;
    }
    if ( cur.type != VO_PARASTYLE ) {
        lc_close( &cur );
        return NULL;
    }
    style = &lwp->style[lwp->nstyle];
    mem_set( style, 0, sizeof( LWPSTYLE ) );
    style->id = *id;
    style->chp = lwp->doc->style[0].chp;
    pap_init( &style->pap );
    lwp_plist( lwp, &cur, &next, &child, name, STYLE_NAME_LEN );
    font = lc_32( &cur );
    final = lc_32( &cur );
    lc_skip( &cur, 6 );
    lc_atom( &cur, NULL, 0 );
    if ( lc_16( &cur ) != 0 ) {             /* the language */
        lwp_common( &cur );
        lc_16( &cur );
    }
    lc_extra( &cur );
    if ( lc_16( &cur ) != 0 ) {             /* hidden, and the base line */
        lwp_common( &cur );
        lc_skip( &cur, 6 );
    }
    lc_extra( &cur );
    lc_id( lwp, &cur, &skip );
    lc_id( lwp, &cur, &skip );
    count = lc_16( &cur );
    for ( index = 0; index < (count > 1 ? 6U : 1U); index++ ) {
        lc_id( lwp, &cur, &skip );
    }
    if ( lc_16( &cur ) != 0 ) {
        lc_32( &cur );
        if ( lc_16( &cur ) != 0 ) {
            lc_16( &cur );
            lc_extra( &cur );
        }
    }
    lc_id( lwp, &cur, &align );
    lc_id( lwp, &cur, &spacing );
    lc_id( lwp, &cur, &indent );
    if ( cur.bad ) {
        mem_set( &align, 0, sizeof( LWPID ) );
        spacing = align;
        indent = align;
    } else {
        lwp_font( lwp, final != 0 ? final : font, &style->chp );
    }
    lc_close( &cur );
    lwp_pieces( lwp, &style->pap, &align, &indent, &spacing );
    for ( index = 0; index < sizeof( known ) / sizeof( known[0] ); index++ ) {
        if ( str_icmp( name, known[index].name ) == 0 ) {
            what = known[index].kind;
        }
    }
    number = name[0] ? wd_style( lwp->doc, name, what ) : 0;
    if ( number != 0 || what == STY_NORMAL ) {
        style->pap.style = (u8)number;
        lwp->doc->style[number].chp = style->chp;
        lwp->doc->style[number].pap = style->pap;
    }
    lwp->nstyle++;
    return style;
}

/* ------------------------------------------------------------------ */
/* stories, paragraphs, tables                                         */
/* ------------------------------------------------------------------ */

static void lwp_story( LWP *lwp, WBUILD *wb, u32 which );

/* text as it is packed: bytes of a code page, and where a zero byte
   comes, UTF-16 to a zero word */
static void lwp_text( WBUILD *wb, const u8 *text, u32 len, int codepage )
{
    u32 pos = 0, code;
    int wide = 0;

    while ( pos < len ) {
        if ( !wide ) {
            code = text[pos++];
            if ( code == 0 ) {
                wide = 1;
            } else if ( code >= 0x20 ) {
                wb_char( wb, code < 0x80 ? code : cp_to_uni( codepage, (int)code ) );
            }
        } else {
            if ( pos + 2 > len ) {
                break;
            }
            code = LE16( text + pos );
            pos += 2;
            if ( code == 0 ) {
                wide = 0;
            } else if ( code >= 0x20 ) {
                wb_char( wb, code );
            }
        }
    }
}

/* the story a layout holds, or -1 */
static s32 lwp_story_of( const LWP *lwp, const LWPID *layout )
{
    u32 index;

    for ( index = 0; index < lwp->nstory; index++ ) {
        if ( lwp->story[index].layout.low == layout->low && lwp->story[index].layout.high == layout->high ) {
            return (s32)index;
        }
    }
    return -1;
}

/* a row of a table: each cell layout under it, and the cell's story */
static void lwp_row( LWP *lwp, WBUILD *wb, const LWPID *first_cell )
{
    TROW row;
    LWPCUR cur;
    LWPID id = *first_cell, next, child;
    WPARA *para = NULL;
    u32 start, index, cells = 0, type;
    s32 which;

    while ( !LWP_NULL( id ) && cells < MAX_CELLS && lwp_open( lwp, &id, &cur ) ) {
        type = cur.type;
        lwp_list( lwp, &cur, &next, &child, NULL, 0 );
        lc_close( &cur );
        if ( type == VO_CELL || type == VO_CONNCELL || type == VO_FNCELL ) {
            start = wb->story->count;
            which = lwp_story_of( lwp, &id );
            if ( which >= 0 && !lwp->story[which].used ) {
                lwp_story( lwp, wb, (u32)which );
            }
            if ( wb->story->count == start ) {
                wd_para_add( lwp->doc, wb->story, NULL );
            }
            if ( wb->story->count == start ) {
                return;                     /* no memory */
            }
            for ( index = start; index < wb->story->count; index++ ) {
                para = wb->story->para[index];
                para->pap.flags |= PF_INTABLE;
            }
            para->pap.flags |= PF_CELLEND;
            cells++;
        }
        if ( cur.bad ) {
            break;
        }
        id = next;
    }
    if ( cells == 0 || para == NULL ) {
        return;
    }
    mem_set( &row, 0, sizeof( row ) );
    row.ncells = (u8)cells;
    row.borders = 1;
    for ( index = 0; index < cells; index++ ) {
        row.edge[index] = lwp->text_width / (s32)cells * (s32)(index + 1);
    }
    para->pap.flags |= PF_ROWEND;
    if ( !pap_set_row( &para->pap, &row ) ) {
        lwp->nomem = 1;
    }
}

/* the layouts under a table's: rows are set out, anything else is
   looked into */
static void lwp_layouts( LWP *lwp, WBUILD *wb, const LWPID *first, u32 depth )
{
    LWPCUR cur;
    LWPID id = *first, next, child;
    u32 type;

    while ( !LWP_NULL( id ) && depth < LWP_MAX_DEPTH && lwp_open( lwp, &id, &cur ) ) {
        type = cur.type;
        lwp_list( lwp, &cur, &next, &child, NULL, 0 );
        if ( cur.bad ) {
            lc_close( &cur );
            break;
        }
        lc_close( &cur );
        if ( type == VO_ROW || type == VO_FNROW ) {
            lwp_row( lwp, wb, &child );
        } else {
            lwp_layouts( lwp, wb, &child, depth + 1 );
        }
        id = next;
    }
}

static void lwp_table( LWP *lwp, WBUILD *wb, const LWPID *layout )
{
    LWPCUR cur;
    LWPID next, child;

    if ( wb->para != NULL ) {
        wb_par( wb );
    }
    if ( !lwp_open( lwp, layout, &cur ) ) {
        return;
    }
    lwp_list( lwp, &cur, &next, &child, NULL, 0 );
    if ( !cur.bad ) {
        lwp->doc->lost |= LOST_TABLES;      /* the columns' widths are not read */
        lwp_layouts( lwp, wb, &child, 0 );
    }
    lc_close( &cur );
}

/* a footnote's call: the next footnote story there is, by order */
static void lwp_note( LWP *lwp, WBUILD *wb )
{
    WBUILD note;
    int obj;

    while ( lwp->next_note < lwp->nstory &&
            (lwp->story[lwp->next_note].kind != LWP_NOTE || lwp->story[lwp->next_note].used) ) {
        lwp->next_note++;
    }
    if ( lwp->next_note >= lwp->nstory || lwp->depth >= LWP_MAX_DEPTH ) {
        lwp->doc->lost |= LOST_NOTES;
        return;
    }
    obj = wd_note_new( lwp->doc, NOTE_FOOT );
    if ( obj < 0 ) {
        return;
    }
    wb_object( wb, obj );
    wb_init( &note, lwp->doc, wd_note_story( lwp->doc, obj ) );
    lwp_story( lwp, &note, lwp->next_note );
    wb_done( &note );
}

/* a paragraph -> the next one's number in "next" */
static void lwp_para( LWP *lwp, WBUILD *wb, const LWPID *id, LWPID *next )
{
    LWPCUR cur;
    LWPID skip, style_id, table[4], own[3];
    const LWPSTYLE *style;
    CHP base;
    u32 flag, tag, type, mod, len, font, codepage, end, ntable = 0, index;
    int ended = 0;

    mem_set( next, 0, sizeof( LWPID ) );
    if ( !lwp_open( lwp, id, &cur ) ) {
        return;
    }
    if ( cur.type != VO_PARA ) {
        lc_close( &cur );
        return;
    }
    lc_id( lwp, &cur, next );
    lc_id( lwp, &cur, &skip );
    flag = lc_8( &cur );
    if ( !(flag & 1) ) {
        lc_32( &cur );
        if ( flag & 2 ) {
            lc_id( lwp, &cur, &skip );
            lc_extra( &cur );
            lc_id( lwp, &cur, &skip );
            lc_extra( &cur );
        }
    }
    lc_16( &cur );
    lc_id( lwp, &cur, &style_id );
    if ( !(flag & 1) ) {
        lc_skip( &cur, 8 );
    }
    lc_id( lwp, &cur, &skip );
    if ( !(flag & 1) ) {
        lc_16( &cur );
    }
    style = cur.bad ? NULL : lwp_style( lwp, &style_id );
    pap_free( &wb->pap );
    if ( style != NULL ) {
        wb->pap = style->pap;
        base = style->chp;
    } else {
        wb->pap = lwp->doc->style[0].pap;
        base = lwp->doc->style[0].chp;
    }
    wb->pap.tabs = NULL;
    wb->pap.row = NULL;
    wb->pap.ntabs = 0;
    while ( !cur.bad ) {
        tag = lc_8( &cur );
        type = tag & 0x3F;
        if ( type == 0 ) {
            ended = !cur.bad;
            break;
        }
        lc_8( &cur );                       /* who edited it */
        font = 0;
        codepage = 0;
        if ( tag & 0x80 ) {
            /* what is changed for this piece: a kind, a length, a value */
            while ( !cur.bad && (mod = lc_8( &cur )) != 0 ) {
                len = lc_8( &cur );
                if ( mod == 1 && len == 4 ) {
                    font = lc_32( &cur );
                } else if ( mod == 8 && len == 2 ) {
                    codepage = lc_16( &cur );
                } else if ( mod == 3 ) {
                    lc_id( lwp, &cur, &skip );
                } else if ( mod == 5 || mod == 4 ) {
                    if ( lc_16( &cur ) != 0 ) {
                        lwp_common( &cur );
                        lc_skip( &cur, mod == 5 ? 2 : 6 );
                    }
                    lc_extra( &cur );
                } else if ( mod == 2 ) {
                    lc_8( &cur );
                    lwp->doc->lost |= LOST_REVISIONS;
                } else {
                    lc_skip( &cur, len );
                }
            }
        }
        len = lc_16( &cur );
        if ( cur.bad || !IN_RANGE( cur.pos, len, cur.len ) ) {
            break;
        }
        end = cur.pos + len;
        wb->chp = base;
        if ( font != 0 ) {
            lwp_font( lwp, font, &wb->chp );
        }
        switch ( type ) {
        case 2:                             /* text */
        case 13:
        case 14:
        case 15:
            lwp_text( wb, cur.ptr + cur.pos, len, tag & 0x40 ? CP_LATIN1 : codepage != 0 && cp_known( (int)codepage ) ?
                                                                          (int)codepage : CP_1252 );
            break;
        case 3:                             /* a table */
            if ( ntable < 4 ) {
                lc_id( lwp, &cur, &table[ntable] );
                ntable += cur.bad ? 0 : 1;
            }
            break;
        case 4:
        case 19:
            wb_char( wb, WCH_TAB );
            break;
        case 5:
            wb_char( wb, WCH_PAGE );
            break;
        case 6:                             /* a frame: its text comes after the body's */
            lwp->doc->lost |= LOST_LAYOUT;
            break;
        case 7:
            lwp_note( lwp, wb );
            break;
        case 9:
            wb_char( wb, WCH_LINE );
            break;
        case 10:
            wb_char( wb, WCH_NBSP );
            break;
        case 11:
            wb_char( wb, WCH_SHY );
            break;
        case 20:
            wb_char( wb, WCH_PAGENUM );
            break;
        case 22:
        case 25:
            lwp->doc->lost |= LOST_FIELDS;
            break;
        default:
            break;
        }
        cur.pos = end;
    }
    /* after the fribs, what this paragraph alone changes: a tag, a
       length, and for these three the number of an object */
    mem_set( own, 0, sizeof( own ) );
    while ( ended && !cur.bad ) {
        tag = lc_32( &cur );
        if ( cur.bad || tag == 0x646E4553UL ) {         /* "SEnd" */
            break;
        }
        len = lc_16( &cur );
        if ( cur.bad || !IN_RANGE( cur.pos, len, cur.len ) ) {
            break;
        }
        end = cur.pos + len;
        index = tag == 0x6E6C4150UL ? 0 : tag == 0x74644950UL ? 1 : tag == 0x63705350UL ? 2 : 3;
        if ( index < 3 ) {                              /* "PAln", "PIdt", "PSpc" */
            lc_id( lwp, &cur, &own[index] );
        }
        cur.pos = end;
    }
    if ( cur.bad ) {
        mem_set( own, 0, sizeof( own ) );
    }
    lc_close( &cur );
    lwp_pieces( lwp, &wb->pap, &own[0], &own[1], &own[2] );
    if ( ntable == 0 || wb->para != NULL ) {
        wb_par( wb );
    }
    for ( index = 0; index < ntable; index++ ) {
        lwp_table( lwp, wb, &table[index] );
    }
}

static void lwp_story( LWP *lwp, WBUILD *wb, u32 which )
{
    LWPID id, next;

    if ( which >= lwp->nstory || lwp->story[which].used || lwp->depth >= LWP_MAX_DEPTH ) {
        return;
    }
    lwp->story[which].used = 1;
    lwp->depth++;
    id = lwp->story[which].first;
    while ( !LWP_NULL( id ) && lwp->budget != 0 && !wd_failed( lwp->doc ) ) {
        lwp_para( lwp, wb, &id, &next );
        id = next;
    }
    lwp->depth--;
}

/* every story there is, and what the layout that holds it says it is */
static void lwp_stories( LWP *lwp )
{
    LWPSTORY *fresh, *story;
    LWPCUR cur;
    LWPID next, child, only, head, skip;
    u32 index, cap, type;

    for ( index = 0; index < lwp->nkey; index++ ) {
        if ( lwp->key[index].pos > lwp->len || !IN_RANGE( lwp->key[index].pos + LWP_BASE, 2, lwp->len ) ||
             LE16( lwp->data + lwp->key[index].pos + LWP_BASE ) != VO_STORY ) {
            continue;
        }
        if ( !lwp_object( lwp, lwp->key[index].pos + LWP_BASE, &cur ) ) {
            continue;
        }
        if ( lwp->nstory >= lwp->story_cap ) {
            cap = lwp->story_cap + 64;
            fresh = (LWPSTORY *)try_realloc( lwp->story, cap * sizeof( LWPSTORY ) );
            if ( fresh == NULL ) {
                lwp->nomem = 1;
                lc_close( &cur );
                return;
            }
            lwp->story = fresh;
            lwp->story_cap = cap;
        }
        lwp_list( lwp, &cur, &next, &child, NULL, 0 );
        lc_id( lwp, &cur, &only );          /* the layouts it is in */
        lc_headtail( lwp, &cur, &head );
        lc_extra( &cur );
        lc_16( &cur );
        lc_atom( &cur, NULL, 0 );
        lc_id( lwp, &cur, &skip );
        lc_id( lwp, &cur, &skip );
        if ( lc_8( &cur ) != 0 ) {
            lc_id( lwp, &cur, &skip );
            lc_extra( &cur );
        }
        lc_extra( &cur );
        story = &lwp->story[lwp->nstory];
        mem_set( story, 0, sizeof( LWPSTORY ) );
        lc_headtail( lwp, &cur, &story->first );
        if ( !cur.bad ) {
            story->id.low = lwp->key[index].low;
            story->id.high = lwp->key[index].high;
            story->layout = LWP_NULL( only ) ? head : only;
            type = lwp_type( lwp, &story->layout );
            story->kind = (u8)(type == VO_PAGE ? LWP_BODY : type == VO_HEADER ? LWP_HEADER : type == VO_FOOTER ? LWP_FOOTER
                               : type == VO_CELL || type == VO_CONNCELL ? LWP_CELL : type == VO_FNCELL ? LWP_NOTE
                               : LWP_OTHER);
            /* "continued on the next page" and its twin are Word Pro's
               own words, not the document's */
            story->used = (u8)(type == VO_CONTON || type == VO_CONTFROM);
            lwp->nstory++;
        }
        lc_close( &cur );
    }
}

/* the page: a layout names an object with its size and one with its
   margins, or the layout it is based on does */
static void lwp_page( LWP *lwp, const LWPID *layout )
{
    PAGESETUP *page = &lwp->doc->page;
    LWPCUR cur, piece;
    LWPID id = *layout, next, child, skip, based, geometry, margins;
    u32 depth, got;
    int have_size = 0, have_margins = 0;

    for ( depth = 0; depth < 4 && !(have_size && have_margins) && lwp_open( lwp, &id, &cur ); depth++ ) {
        lwp_plist( lwp, &cur, &next, &child, NULL, 0 );
        lc_skip( &cur, 20 );
        lc_id( lwp, &cur, &skip );
        lc_id( lwp, &cur, &skip );
        lc_extra( &cur );
        lc_atom( &cur, NULL, 0 );
        lc_extra( &cur );
        lc_id( lwp, &cur, &skip );          /* what it holds */
        lc_id( lwp, &cur, &based );
        lc_id( lwp, &cur, &skip );
        got = lc_8( &cur );
        if ( got & 1 ) {
            lc_32( &cur );
            lc_atom( &cur, NULL, 0 );
            if ( lc_16( &cur ) != 0 ) {
                lc_16( &cur );
                lc_extra( &cur );
            }
        }
        if ( got & 2 ) {
            lc_skip( &cur, 6 );
            lc_atom( &cur, NULL, 0 );
            lc_extra( &cur );
        }
        lc_id( lwp, &cur, &geometry );
        lc_id( lwp, &cur, &skip );
        lc_id( lwp, &cur, &margins );
        if ( cur.bad ) {
            lc_close( &cur );
            return;
        }
        lc_close( &cur );
        if ( !have_size && lwp_open( lwp, &geometry, &piece ) ) {
            lc_id( lwp, &piece, &skip );
            lc_id( lwp, &piece, &skip );
            page->width = lwp_twips( lc_32( &piece ) );
            page->height = lwp_twips( lc_32( &piece ) );
            have_size = !piece.bad;
            lc_close( &piece );
        }
        if ( !have_margins && lwp_open( lwp, &margins, &piece ) ) {
            lc_id( lwp, &piece, &skip );
            lc_id( lwp, &piece, &skip );
            page->left = lwp_twips( lc_32( &piece ) );
            page->top = lwp_twips( lc_32( &piece ) );
            page->right = lwp_twips( lc_32( &piece ) );
            page->bottom = lwp_twips( lc_32( &piece ) );
            have_margins = !piece.bad;
            lc_close( &piece );
        }
        id = based;
    }
    page->landscape = (u8)(page->width > page->height);
}

/* ------------------------------------------------------------------ */
/* the format                                                          */
/* ------------------------------------------------------------------ */

static int lwp_detect( const u8 *data, u32 len, const char *ext )
{
    (void)ext;
    if ( len < 0x46 || mem_cmp( data, "WordPro", 8 ) != 0 || mem_cmp( data + 0x10, "LWP7", 4 ) != 0 ) {
        return 0;
    }
    return 100;
}

static void lwp_free( LWP *lwp )
{
    lwp_fonts_drop( lwp );
    xfree( lwp->key );
    xfree( lwp->time );
    xfree( lwp->story );
    xfree( lwp->style );
    xfree( lwp );
}

static int lwp_load( const u8 *data, u32 len, WDOC *doc )
{
    LWP *lwp;
    WBUILD wb;
    u32 index, kind;
    int rc;

    if ( lwp_detect( data, len, "" ) == 0 ) {
        return WE_FORMAT;
    }
    /* the header's fields start at 2Eh: the program's revision, the
       file's, three more words, the document's number, the index */
    if ( LE16( data + 0x30 ) < 0x0B ) {
        return WE_UNSUPPORTED;
    }
    lwp = (LWP *)try_alloc( sizeof( LWP ) );
    if ( lwp == NULL ) {
        return WE_NOMEM;
    }
    mem_set( lwp, 0, sizeof( LWP ) );
    lwp->data = data;
    lwp->len = len;
    lwp->doc = doc;
    lwp->rev = LE16( data + 0x30 );
    lwp->style = (LWPSTYLE *)try_alloc( LWP_MAX_STYLES * sizeof( LWPSTYLE ) );
    if ( lwp->style == NULL ) {
        lwp_free( lwp );
        return WE_NOMEM;
    }
    if ( !lwp_index( lwp, LE32( data + 0x3E ) ) || lwp->nkey == 0 ) {
        rc = lwp->nomem ? WE_NOMEM : WE_FORMAT;
        lwp_free( lwp );
        return rc;
    }
    /* nothing is opened more often than a tangled file could ask for */
    lwp->budget = lwp->nkey * 2 + 256;
    lwp_fonts( lwp );
    lwp_stories( lwp );
    lwp->text_width = 9360;
    for ( index = 0; index < lwp->nstory; index++ ) {
        if ( lwp->story[index].kind == LWP_BODY ) {
            lwp_page( lwp, &lwp->story[index].layout );
            if ( doc->page.width - doc->page.left - doc->page.right >= 1440 ) {
                lwp->text_width = doc->page.width - doc->page.left - doc->page.right;
            }
            break;
        }
    }
    /* the body, then what no table or footnote asked for, so that no
       text is left out; a header and a footer, the first of each */
    for ( kind = LWP_BODY; kind <= LWP_NOTE + 1; kind++ ) {
        for ( index = 0; index < lwp->nstory; index++ ) {
            if ( lwp->story[index].used || (kind <= LWP_NOTE && lwp->story[index].kind != kind) ) {
                continue;
            }
            if ( kind == LWP_HEADER || kind == LWP_FOOTER ) {
                if ( (kind == LWP_HEADER ? doc->header.count : doc->footer.count) != 0 ) {
                    lwp->story[index].used = 1;
                    continue;
                }
                wb_init( &wb, doc, kind == LWP_HEADER ? &doc->header : &doc->footer );
            } else {
                if ( kind != LWP_BODY ) {
                    doc->lost |= LOST_LAYOUT;
                }
                wb_init( &wb, doc, &doc->body );
            }
            lwp_story( lwp, &wb, index );
            wb_done( &wb );
        }
    }
    rc = lwp->nomem || wd_failed( doc ) ? WE_NOMEM : WE_OK;
    lwp_free( lwp );
    return rc;
}

const FORMAT fmt_wordpro = {
    FMT_WORDPRO, "Lotus Word Pro", "LWP;MWP", lwp_detect, lwp_load, NULL, NULL, 0
};
