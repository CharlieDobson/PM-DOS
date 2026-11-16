/*
 * WDOC.C - the document: made, added to, and given back (WDOC.H).
 *
 * This is the part every reader of a file uses, so it is the part
 * that must not mind what it is handed: an offset past a paragraph's
 * end is the end, a font or style number that does not exist is the
 * first, and running out of memory marks the document and leaves it
 * whole.  The editing commands, which undo what they do, are WEDIT.C's
 * and are built on what is here.
 */
#include "fmt.h"

void (*wd_lay_drop)( WPARA *para );
void (*wd_undo_drop)( WDOC *doc );

/* ------------------------------------------------------------------ */
/* small things                                                        */
/* ------------------------------------------------------------------ */

static void fail( WDOC *doc )
{
    doc->failed = 1;
}

int wd_failed( const WDOC *doc )
{
    return doc == NULL || doc->failed;
}

/* room for "need" items of "size" bytes in an array that doubles */
static int grow( WDOC *doc, void **array, u32 *cap, u32 need, u32 size )
{
    u32 want;
    void *fresh;

    if ( need <= *cap ) {
        return 1;
    }
    want = *cap ? *cap : 8;
    while ( want < need ) {
        if ( want > 0x7FFFFFFFUL / size / 2 ) {
            fail( doc );
            return 0;
        }
        want *= 2;
    }
    fresh = try_realloc( *array, want * size );
    if ( fresh == NULL ) {
        fail( doc );
        return 0;
    }
    *array = fresh;
    *cap = want;
    return 1;
}

/* ------------------------------------------------------------------ */
/* paragraph properties                                                */
/* ------------------------------------------------------------------ */

void pap_init( PAP *pap )
{
    mem_set( pap, 0, sizeof( PAP ) );
    pap->align = AL_LEFT;
    pap->line = 240;
    pap->line_rule = LS_MULTIPLE;
}

void pap_free( PAP *pap )
{
    xfree( pap->tabs );
    xfree( pap->row );
    pap->tabs = NULL;
    pap->row = NULL;
    pap->ntabs = 0;
}

int pap_set_tabs( PAP *pap, const WTAB *tabs, int count )
{
    WTAB *fresh = NULL, hold;
    int index, back;

    if ( count > MAX_TABS ) {
        count = MAX_TABS;
    }
    if ( count > 0 ) {
        fresh = (WTAB *)try_alloc( (u32)count * sizeof( WTAB ) );
        if ( fresh == NULL ) {
            return 0;
        }
        mem_cpy( fresh, tabs, (u32)count * sizeof( WTAB ) );
        /* in order of position: there are few, and they mostly are */
        for ( index = 1; index < count; index++ ) {
            hold = fresh[index];
            for ( back = index; back > 0 && fresh[back - 1].pos > hold.pos; back-- ) {
                fresh[back] = fresh[back - 1];
            }
            fresh[back] = hold;
        }
    } else {
        count = 0;
    }
    xfree( pap->tabs );
    pap->tabs = fresh;
    pap->ntabs = (u8)count;
    return 1;
}

int pap_set_row( PAP *pap, const TROW *row )
{
    TROW *fresh = NULL;

    if ( row != NULL ) {
        fresh = (TROW *)try_alloc( sizeof( TROW ) );
        if ( fresh == NULL ) {
            return 0;
        }
        *fresh = *row;
        if ( fresh->ncells > MAX_CELLS ) {
            fresh->ncells = MAX_CELLS;
        }
    }
    xfree( pap->row );
    pap->row = fresh;
    return 1;
}

int pap_copy( PAP *dst, const PAP *src )
{
    *dst = *src;
    dst->tabs = NULL;
    dst->row = NULL;
    dst->ntabs = 0;
    if ( src->tabs != NULL && src->ntabs ) {
        if ( !pap_set_tabs( dst, src->tabs, src->ntabs ) ) {
            return 0;
        }
    }
    if ( src->row != NULL ) {
        if ( !pap_set_row( dst, src->row ) ) {
            pap_free( dst );
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* the document                                                        */
/* ------------------------------------------------------------------ */

WDOC *wd_new( void )
{
    WDOC *doc = (WDOC *)try_alloc( sizeof( WDOC ) );
    CHP chp;

    if ( doc == NULL ) {
        return NULL;
    }
    mem_set( doc, 0, sizeof( WDOC ) );
    doc->page.width = 12240;            /* 8.5 by 11 inches */
    doc->page.height = 15840;
    doc->page.left = doc->page.right = 1800;
    doc->page.top = doc->page.bottom = 1440;
    doc->page.header = doc->page.footer = 720;
    doc->page.first_page = 1;
    doc->deftab = 720;

    wd_font( doc, "Times New Roman", FAM_ROMAN, 0 );
    mem_set( &chp, 0, sizeof( chp ) );
    chp.size = 24;
    chp.color = COLOR_AUTO;
    wd_chp( doc, &chp );

    doc->style = (WSTYLE *)try_alloc( MAX_STYLES * sizeof( WSTYLE ) );
    if ( doc->style == NULL || doc->failed ) {
        wd_free( doc );
        return NULL;
    }
    mem_set( doc->style, 0, MAX_STYLES * sizeof( WSTYLE ) );
    str_cpy( doc->style[0].name, "Normal" );
    doc->style[0].kind = STY_NORMAL;
    doc->style[0].chp = chp;
    pap_init( &doc->style[0].pap );
    doc->nstyle = 1;
    return doc;
}

static void para_free( WPARA *para )
{
    if ( para == NULL ) {
        return;
    }
    if ( para->lay != NULL && wd_lay_drop != NULL ) {
        wd_lay_drop( para );
    }
    pap_free( &para->pap );
    xfree( para->text );
    xfree( para->attr );
    xfree( para->anch );
    xfree( para );
}

static void story_free( STORY *story )
{
    u32 index;

    for ( index = 0; index < story->count; index++ ) {
        para_free( story->para[index] );
    }
    xfree( story->para );
    story->para = NULL;
    story->count = story->cap = 0;
}

void wd_free( WDOC *doc )
{
    u32 index;

    if ( doc == NULL ) {
        return;
    }
    if ( doc->aux != NULL && doc->aux_free != NULL ) {
        doc->aux_free( doc->aux );
    }
    if ( doc->undo != NULL && wd_undo_drop != NULL ) {
        wd_undo_drop( doc );
    }
    story_free( &doc->body );
    story_free( &doc->header );
    story_free( &doc->footer );
    for ( index = 0; index < doc->nobj; index++ ) {
        xfree( doc->obj[index].data );
        if ( doc->obj[index].story != NULL ) {
            story_free( doc->obj[index].story );
            xfree( doc->obj[index].story );
        }
    }
    xfree( doc->obj );
    xfree( doc->chp );
    xfree( doc->chp_hash );
    xfree( doc->font );
    xfree( doc->style );
    xfree( doc );
}

void wd_set_aux( WDOC *doc, void *aux, void (*aux_free)( void *aux ) )
{
    if ( doc->aux != NULL && doc->aux_free != NULL && doc->aux != aux ) {
        doc->aux_free( doc->aux );
    }
    doc->aux = aux;
    doc->aux_free = aux_free;
}

/* the largest paper there is: 22 inches, which is Word's limit too */
#define PAPER_MOST      31680
#define PAPER_LEAST     1440

/* two margins that leave an inch of paper between them, at least */
static void margins_fit( s32 *lead, s32 *trail, s32 paper, s32 usual )
{
    if ( *lead < 0 || *lead > PAPER_MOST ) {
        *lead = usual;
    }
    if ( *trail < 0 || *trail > PAPER_MOST ) {
        *trail = usual;
    }
    if ( *lead + *trail > paper - PAPER_LEAST ) {
        *lead = *trail = (paper - PAPER_LEAST) / 2 < usual ? (paper - PAPER_LEAST) / 2 : usual;
    }
}

void wd_finish( WDOC *doc )
{
    PAGESETUP *page = &doc->page;
    u32 index;

    /* a file can say anything about its paper: what is kept is a sheet
       that exists, with room on it for a line of text */
    if ( page->width < PAPER_LEAST || page->width > PAPER_MOST ) {
        page->width = 12240;
    }
    if ( page->height < PAPER_LEAST || page->height > PAPER_MOST ) {
        page->height = 15840;
    }
    margins_fit( &page->left, &page->right, page->width, 1800 );
    margins_fit( &page->top, &page->bottom, page->height, 1440 );
    if ( page->header < 0 || page->header > page->height ) {
        page->header = 720;
    }
    if ( page->footer < 0 || page->footer > page->height ) {
        page->footer = 720;
    }
    if ( doc->deftab < 72 || doc->deftab > 14400 ) {
        doc->deftab = 720;
    }
    if ( doc->body.count == 0 ) {
        wd_para_add( doc, &doc->body, NULL );
    }
    for ( index = 0; index < doc->nobj; index++ ) {
        if ( doc->obj[index].story != NULL && doc->obj[index].story->count == 0 ) {
            wd_para_add( doc, doc->obj[index].story, NULL );
        }
    }
}

/* ------------------------------------------------------------------ */
/* fonts                                                               */
/* ------------------------------------------------------------------ */

static int name_has( const char *name, const char *part )
{
    return mem_ifind( (const u8 *)name, str_len( name ), part ) >= 0;
}

static int guess_fixed( const char *name )
{
    static const char *const fixed[] = {
        "Courier", "Mono", "Consol", "Fixed", "Terminal", "Letter Gothic",
        "Prestige", "Line Printer", "Typewriter", "OCR", "Elite", "Pica"
    };
    u32 index;

    for ( index = 0; index < sizeof( fixed ) / sizeof( fixed[0] ); index++ ) {
        if ( name_has( name, fixed[index] ) ) {
            return 1;
        }
    }
    return 0;
}

static int guess_family( const char *name, int fixed )
{
    static const char *const swiss[] = {
        "Arial", "Helv", "Swiss", "Sans", "Univers", "Verdana", "Tahoma",
        "Calibri", "Segoe", "Futura", "Gothic", "Avant", "Trebuchet", "Geneva"
    };
    static const char *const roman[] = {
        "Times", "Roman", "Serif", "Garamond", "Book", "Georgia", "Palatino",
        "Century", "Cambria", "Dutch", "Tms", "Charter", "Bodoni", "Caslon"
    };
    u32 index;

    if ( name_has( name, "Symbol" ) || name_has( name, "Dingbat" ) || name_has( name, "Wingding" ) ) {
        return FAM_SYMBOL;
    }
    if ( fixed ) {
        return FAM_MODERN;
    }
    for ( index = 0; index < sizeof( swiss ) / sizeof( swiss[0] ); index++ ) {
        if ( name_has( name, swiss[index] ) ) {
            return FAM_SWISS;
        }
    }
    for ( index = 0; index < sizeof( roman ) / sizeof( roman[0] ); index++ ) {
        if ( name_has( name, roman[index] ) ) {
            return FAM_ROMAN;
        }
    }
    return FAM_ANY;
}

int wd_font( WDOC *doc, const char *name, int family, int fixed )
{
    char clean[FONT_NAME_LEN];
    WFONT *font;
    u32 index, len = 0;

    /* ASCII, no control characters, no more than fits */
    while ( name != NULL && *name && len < FONT_NAME_LEN - 1 ) {
        if ( *name >= 0x20 && *name < 0x7F ) {
            clean[len++] = *name;
        }
        name++;
    }
    while ( len && clean[len - 1] == ' ' ) {
        len--;
    }
    clean[len] = 0;
    if ( len == 0 ) {
        return 0;
    }
    for ( index = 0; index < doc->nfont; index++ ) {
        if ( str_icmp( doc->font[index].name, clean ) == 0 ) {
            return (int)index;
        }
    }
    if ( doc->nfont >= 0xFFFF ||
         !grow( doc, (void **)&doc->font, &doc->font_cap, doc->nfont + 1, sizeof( WFONT ) ) ) {
        return 0;
    }
    font = &doc->font[doc->nfont];
    mem_set( font, 0, sizeof( WFONT ) );
    str_cpy( font->name, clean );
    if ( fixed < 0 ) {
        fixed = family == FAM_MODERN || guess_fixed( clean );
    }
    font->fixed = (u8)(fixed != 0);
    if ( family <= FAM_ANY || family > FAM_SYMBOL ) {
        family = guess_family( clean, fixed );
    }
    font->family = (u8)family;
    return (int)doc->nfont++;
}

/* ------------------------------------------------------------------ */
/* character properties, each kept once                                */
/* ------------------------------------------------------------------ */

static u32 chp_hash_of( const CHP *chp )
{
    u32 hash = 2166136261UL;
    const u8 *raw = (const u8 *)chp;
    u32 index;

    for ( index = 0; index < sizeof( CHP ); index++ ) {
        hash = (hash ^ raw[index]) * 16777619UL;
    }
    return hash;
}

static void hash_place( WDOC *doc, u32 id )
{
    u32 mask = doc->chp_hash_cap - 1;
    u32 slot = chp_hash_of( &doc->chp[id] ) & mask;

    while ( doc->chp_hash[slot] ) {
        slot = (slot + 1) & mask;
    }
    doc->chp_hash[slot] = (u16)(id + 1);
}

static int hash_room( WDOC *doc )
{
    u16 *fresh;
    u32 cap, index;

    if ( doc->chp_hash_cap >= (doc->nchp + 1) * 2 ) {
        return 1;
    }
    cap = doc->chp_hash_cap ? doc->chp_hash_cap * 2 : 64;
    fresh = (u16 *)try_alloc( cap * sizeof( u16 ) );
    if ( fresh == NULL ) {
        fail( doc );
        return 0;
    }
    mem_set( fresh, 0, cap * sizeof( u16 ) );
    xfree( doc->chp_hash );
    doc->chp_hash = fresh;
    doc->chp_hash_cap = cap;
    for ( index = 0; index < doc->nchp; index++ ) {
        hash_place( doc, index );
    }
    return 1;
}

u16 wd_chp( WDOC *doc, const CHP *chp )
{
    CHP clean = *chp;
    u32 mask, slot, id;

    clean.spare = 0;
    if ( clean.font >= doc->nfont ) {
        clean.font = 0;
    }
    if ( clean.size < 2 ) {             /* a point at least, 500 at most */
        clean.size = 2;
    } else if ( clean.size > 1000 ) {
        clean.size = 1000;
    }
    if ( clean.color != COLOR_AUTO ) {
        clean.color &= 0x00FFFFFFUL;
    }
    if ( doc->chp_hash_cap ) {
        mask = doc->chp_hash_cap - 1;
        slot = chp_hash_of( &clean ) & mask;
        while ( doc->chp_hash[slot] ) {
            id = (u32)doc->chp_hash[slot] - 1;
            if ( mem_cmp( &doc->chp[id], &clean, sizeof( CHP ) ) == 0 ) {
                return (u16)id;
            }
            slot = (slot + 1) & mask;
        }
    }
    if ( doc->nchp >= 0xFFFE ) {        /* every number a u16 has is in use */
        doc->lost |= LOST_TEXT;
        return 0;
    }
    if ( !grow( doc, (void **)&doc->chp, &doc->chp_cap, doc->nchp + 1, sizeof( CHP ) ) ||
         !hash_room( doc ) ) {
        return 0;
    }
    id = doc->nchp++;
    doc->chp[id] = clean;
    hash_place( doc, id );
    return (u16)id;
}

const CHP *wd_chp_get( const WDOC *doc, u16 id )
{
    return &doc->chp[id < doc->nchp ? id : 0];
}

/* ------------------------------------------------------------------ */
/* styles                                                              */
/* ------------------------------------------------------------------ */

int wd_style( WDOC *doc, const char *name, int kind )
{
    static const u16 head_size[6] = { 32, 28, 26, 24, 24, 24 };
    WSTYLE *style;
    u32 index;

    if ( kind < 0 || kind > STY_OTHER ) {
        kind = STY_OTHER;
    }
    for ( index = 0; index < doc->nstyle; index++ ) {
        if ( kind != STY_OTHER ? doc->style[index].kind == kind
                               : (name != NULL && str_icmp( doc->style[index].name, name ) == 0) ) {
            return (int)index;
        }
    }
    if ( doc->nstyle >= MAX_STYLES || name == NULL ) {
        return 0;
    }
    style = &doc->style[doc->nstyle];
    *style = doc->style[0];
    style->pap.tabs = NULL;
    style->pap.row = NULL;
    style->pap.ntabs = 0;
    mem_set( style->name, 0, STYLE_NAME_LEN );
    for ( index = 0; index < STYLE_NAME_LEN - 1 && name[index]; index++ ) {
        style->name[index] = (char)(name[index] >= 0x20 && name[index] < 0x7F ? name[index] : '?');
    }
    style->kind = (u8)kind;
    if ( kind >= STY_HEADING1 && kind <= STY_HEADING6 ) {
        style->chp.flags |= CF_BOLD;
        style->chp.size = head_size[kind - STY_HEADING1];
        style->pap.before = 240;
        style->pap.after = 60;
        style->pap.flags |= PF_KEEPNEXT;
    } else if ( kind == STY_TITLE ) {
        style->chp.flags |= CF_BOLD;
        style->chp.size = 36;
        style->pap.align = AL_CENTER;
        style->pap.after = 120;
    } else if ( kind == STY_SUBTITLE ) {
        style->chp.flags |= CF_ITALIC;
        style->chp.size = 28;
        style->pap.align = AL_CENTER;
        style->pap.after = 120;
    } else if ( kind == STY_FOOTNOTE ) {
        style->chp.size = 20;
    }
    return (int)doc->nstyle++;
}

/* ------------------------------------------------------------------ */
/* paragraphs                                                          */
/* ------------------------------------------------------------------ */

WPARA *wd_para_insert( WDOC *doc, STORY *story, u32 index, const PAP *pap )
{
    WPARA *para;

    if ( doc->failed ) {
        return NULL;
    }
    if ( index > story->count ) {
        index = story->count;
    }
    if ( !grow( doc, (void **)&story->para, &story->cap, story->count + 1, sizeof( WPARA * ) ) ) {
        return NULL;
    }
    para = (WPARA *)try_alloc( sizeof( WPARA ) );
    if ( para == NULL ) {
        fail( doc );
        return NULL;
    }
    mem_set( para, 0, sizeof( WPARA ) );
    if ( pap == NULL ) {
        pap_init( &para->pap );
    } else if ( !pap_copy( &para->pap, pap ) ) {
        xfree( para );
        fail( doc );
        return NULL;
    }
    if ( para->pap.style >= doc->nstyle ) {
        para->pap.style = 0;
    }
    mem_cpy( story->para + index + 1, story->para + index,
             (story->count - index) * sizeof( WPARA * ) );
    story->para[index] = para;
    story->count++;
    return para;
}

WPARA *wd_para_add( WDOC *doc, STORY *story, const PAP *pap )
{
    return wd_para_insert( doc, story, story->count, pap );
}

void wd_para_remove( WDOC *doc, STORY *story, u32 index )
{
    (void)doc;
    if ( index >= story->count ) {
        return;
    }
    para_free( story->para[index] );
    mem_cpy( story->para + index, story->para + index + 1,
             (story->count - index - 1) * sizeof( WPARA * ) );
    story->count--;
}

static int para_room( WDOC *doc, WPARA *para, u32 need )
{
    u16 *text, *attr;
    u32 cap;

    if ( need <= para->cap ) {
        return 1;
    }
    cap = para->cap ? para->cap : 32;
    while ( cap < need ) {
        if ( cap >= 0x10000000UL ) {
            fail( doc );
            return 0;
        }
        cap *= 2;
    }
    /* both or neither: one that grew alone is only a bigger block */
    text = (u16 *)try_realloc( para->text, cap * sizeof( u16 ) );
    if ( text == NULL ) {
        fail( doc );
        return 0;
    }
    para->text = text;
    attr = (u16 *)try_realloc( para->attr, cap * sizeof( u16 ) );
    if ( attr == NULL ) {
        fail( doc );
        return 0;
    }
    para->attr = attr;
    para->cap = cap;
    return 1;
}

static void lay_stale( WPARA *para )
{
    if ( para->lay != NULL && wd_lay_drop != NULL ) {
        wd_lay_drop( para );
    }
}

/* room made at "off" for "len" units, the objects after it moved along */
static int text_open( WDOC *doc, WPARA *para, u32 *off, u32 len )
{
    u32 index, tail;

    if ( doc->failed || para == NULL ) {
        return 0;
    }
    if ( *off > para->len ) {
        *off = para->len;
    }
    if ( len > 0x10000000UL - para->len || !para_room( doc, para, para->len + len ) ) {
        return 0;
    }
    tail = para->len - *off;
    mem_cpy( para->text + *off + len, para->text + *off, tail * sizeof( u16 ) );
    mem_cpy( para->attr + *off + len, para->attr + *off, tail * sizeof( u16 ) );
    para->len += len;
    for ( index = 0; index < para->nanch; index++ ) {
        if ( para->anch[index].off >= *off ) {
            para->anch[index].off += len;
        }
    }
    lay_stale( para );
    return 1;
}

/* a WCH_OBJ that comes in as text has no object behind it: it is the
   "no such character" mark instead */
static u16 plain_unit( u16 unit )
{
    return unit == WCH_OBJ ? (u16)0xFFFD : unit;
}

int wd_text_insert( WDOC *doc, WPARA *para, u32 off, const u16 *text, u32 len, u16 chp )
{
    u32 index;

    if ( len == 0 ) {
        return !doc->failed;
    }
    if ( !text_open( doc, para, &off, len ) ) {
        return 0;
    }
    if ( chp >= doc->nchp ) {
        chp = 0;
    }
    for ( index = 0; index < len; index++ ) {
        para->text[off + index] = plain_unit( text[index] );
        para->attr[off + index] = chp;
    }
    return 1;
}

int wd_text_insert2( WDOC *doc, WPARA *para, u32 off, const u16 *text, const u16 *attr, u32 len )
{
    u32 index;

    if ( len == 0 ) {
        return !doc->failed;
    }
    if ( !text_open( doc, para, &off, len ) ) {
        return 0;
    }
    for ( index = 0; index < len; index++ ) {
        para->text[off + index] = plain_unit( text[index] );
        para->attr[off + index] = (u16)(attr[index] < doc->nchp ? attr[index] : 0);
    }
    return 1;
}

void wd_text_delete( WDOC *doc, WPARA *para, u32 off, u32 len )
{
    u32 index, kept = 0, end;

    (void)doc;
    if ( para == NULL || off >= para->len || len == 0 ) {
        return;
    }
    if ( len > para->len - off ) {
        len = para->len - off;
    }
    end = off + len;
    mem_cpy( para->text + off, para->text + end, (para->len - end) * sizeof( u16 ) );
    mem_cpy( para->attr + off, para->attr + end, (para->len - end) * sizeof( u16 ) );
    para->len -= len;
    for ( index = 0; index < para->nanch; index++ ) {
        if ( para->anch[index].off >= end ) {
            para->anch[index].off -= len;
            para->anch[kept++] = para->anch[index];
        } else if ( para->anch[index].off < off ) {
            para->anch[kept++] = para->anch[index];
        }
    }
    para->nanch = kept;
    lay_stale( para );
}

int wd_text_restore( WDOC *doc, WPARA *para, u32 off, const u16 *text, const u16 *attr,
                     u32 len, const WANCH *anch, u32 nanch )
{
    u32 index, place, which = 0;

    if ( len == 0 ) {
        return !doc->failed;
    }
    if ( !grow( doc, (void **)&para->anch, &para->anch_cap, para->nanch + nanch, sizeof( WANCH ) ) ) {
        return 0;
    }
    if ( !text_open( doc, para, &off, len ) ) {
        return 0;
    }
    /* where in the paragraph's own list the first of them goes */
    for ( place = 0; place < para->nanch && para->anch[place].off < off; place++ ) {
    }
    for ( index = 0; index < len; index++ ) {
        para->attr[off + index] = (u16)(attr[index] < doc->nchp ? attr[index] : 0);
        if ( text[index] != WCH_OBJ ) {
            para->text[off + index] = text[index];
            continue;
        }
        /* an object character with no object behind it is not put back as one */
        while ( which < nanch && anch[which].off < index ) {
            which++;
        }
        if ( which >= nanch || anch[which].off != index || anch[which].obj >= doc->nobj ) {
            para->text[off + index] = 0xFFFD;
            continue;
        }
        para->text[off + index] = WCH_OBJ;
        mem_cpy( para->anch + place + 1, para->anch + place, (para->nanch - place) * sizeof( WANCH ) );
        para->anch[place].off = off + index;
        para->anch[place].obj = anch[which].obj;
        para->nanch++;
        place++;
        which++;
    }
    return 1;
}

void wd_para_touch( WPARA *para )
{
    lay_stale( para );
}

u32 wd_run( const WPARA *para, u32 off )
{
    u32 end = off;

    if ( off >= para->len ) {
        return 0;
    }
    /* an object is a run of its own, whatever is beside it */
    if ( para->text[off] == WCH_OBJ ) {
        return 1;
    }
    while ( end < para->len && para->attr[end] == para->attr[off] && para->text[end] != WCH_OBJ ) {
        end++;
    }
    return end - off;
}

/* ------------------------------------------------------------------ */
/* objects                                                             */
/* ------------------------------------------------------------------ */

static int obj_new( WDOC *doc )
{
    if ( doc->failed ||
         !grow( doc, (void **)&doc->obj, &doc->obj_cap, doc->nobj + 1, sizeof( WOBJ ) ) ) {
        return -1;
    }
    mem_set( &doc->obj[doc->nobj], 0, sizeof( WOBJ ) );
    return (int)doc->nobj++;
}

int wd_image_new( WDOC *doc, const u8 *data, u32 len, int fmt, s32 width, s32 height )
{
    u8 *copy;
    int obj;

    if ( doc->failed || data == NULL || len == 0 ) {
        return -1;
    }
    copy = (u8 *)try_alloc( len );
    if ( copy == NULL ) {
        /* a picture too big to keep is a picture lost, not a document lost */
        doc->lost |= LOST_IMAGES;
        return -1;
    }
    mem_cpy( copy, data, len );
    obj = obj_new( doc );
    if ( obj < 0 ) {
        xfree( copy );
        return -1;
    }
    doc->obj[obj].kind = OBJ_IMAGE;
    doc->obj[obj].fmt = (u8)fmt;
    doc->obj[obj].data = copy;
    doc->obj[obj].len = len;
    doc->obj[obj].width = width > 0 ? width : 1440;
    doc->obj[obj].height = height > 0 ? height : 1440;
    return obj;
}

int wd_note_new( WDOC *doc, int kind )
{
    STORY *story;
    int obj;

    if ( doc->failed ) {
        return -1;
    }
    story = (STORY *)try_alloc( sizeof( STORY ) );
    if ( story == NULL ) {
        fail( doc );
        return -1;
    }
    mem_set( story, 0, sizeof( STORY ) );
    obj = obj_new( doc );
    if ( obj < 0 ) {
        xfree( story );
        return -1;
    }
    doc->obj[obj].kind = OBJ_NOTE;
    doc->obj[obj].fmt = (u8)kind;
    doc->obj[obj].story = story;
    return obj;
}

STORY *wd_note_story( WDOC *doc, int obj )
{
    if ( obj < 0 || (u32)obj >= doc->nobj || doc->obj[obj].kind != OBJ_NOTE ) {
        return NULL;
    }
    return doc->obj[obj].story;
}

int wd_obj_put( WDOC *doc, WPARA *para, u32 off, int obj, u16 chp )
{
    u32 index;

    if ( obj < 0 || (u32)obj >= doc->nobj ) {
        return 0;
    }
    if ( !grow( doc, (void **)&para->anch, &para->anch_cap, para->nanch + 1, sizeof( WANCH ) ) ) {
        return 0;
    }
    if ( !text_open( doc, para, &off, 1 ) ) {
        return 0;
    }
    para->text[off] = WCH_OBJ;
    para->attr[off] = (u16)(chp < doc->nchp ? chp : 0);
    for ( index = para->nanch; index > 0 && para->anch[index - 1].off > off; index-- ) {
        para->anch[index] = para->anch[index - 1];
    }
    para->anch[index].off = off;
    para->anch[index].obj = (u32)obj;
    para->nanch++;
    return 1;
}

int wd_obj_at( const WPARA *para, u32 off )
{
    u32 index;

    for ( index = 0; index < para->nanch; index++ ) {
        if ( para->anch[index].off == off ) {
            return (int)para->anch[index].obj;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* a reader's helper                                                   */
/* ------------------------------------------------------------------ */

void wb_init( WBUILD *wb, WDOC *doc, STORY *story )
{
    mem_set( wb, 0, sizeof( WBUILD ) );
    wb->doc = doc;
    wb->story = story;
    wb->chp = doc->style[0].chp;
    pap_init( &wb->pap );
    pap_copy( &wb->pap, &doc->style[0].pap );
}

static int wb_open( WBUILD *wb )
{
    if ( wb->para == NULL ) {
        wb->para = wd_para_add( wb->doc, wb->story, NULL );
    }
    return wb->para != NULL;
}

static u16 wb_chp( WBUILD *wb )
{
    wb->chp.spare = 0;
    if ( !wb->chp_known || mem_cmp( &wb->chp, &wb->chp_was, sizeof( CHP ) ) != 0 ) {
        wb->chp_id = wd_chp( wb->doc, &wb->chp );
        wb->chp_was = wb->chp;
        wb->chp_known = 1;
    }
    return wb->chp_id;
}

void wb_text( WBUILD *wb, const u16 *text, u32 len )
{
    if ( len && wb_open( wb ) ) {
        wd_text_insert( wb->doc, wb->para, wb->para->len, text, len, wb_chp( wb ) );
    }
}

void wb_char( WBUILD *wb, u32 code )
{
    u16 pair[2];

    if ( code == 0 ) {
        return;                         /* no file means a NUL as text */
    }
    if ( code < 0x10000UL ) {
        if ( code >= 0xD800 && code <= 0xDFFF ) {
            code = 0xFFFD;
        }
        pair[0] = (u16)code;
        wb_text( wb, pair, 1 );
    } else if ( code <= 0x10FFFFUL ) {
        code -= 0x10000UL;
        pair[0] = (u16)(0xD800 + (code >> 10));
        pair[1] = (u16)(0xDC00 + (code & 0x3FF));
        wb_text( wb, pair, 2 );
    }
}

void wb_utf8( WBUILD *wb, const u8 *text, u32 len )
{
    const u8 *end = text + len;

    while ( text < end ) {
        wb_char( wb, utf8_get( &text, end ) );
    }
}

void wb_bytes( WBUILD *wb, const u8 *text, u32 len, int codepage )
{
    u16 run[128];
    u32 used = 0;

    if ( codepage == CP_UTF8 ) {
        wb_utf8( wb, text, len );
        return;
    }
    while ( len-- ) {
        run[used] = cp_to_uni( codepage, *text++ );
        if ( run[used] != 0 ) {
            used++;
        }
        if ( used == sizeof( run ) / sizeof( run[0] ) ) {
            wb_text( wb, run, used );
            used = 0;
        }
    }
    wb_text( wb, run, used );
}

void wb_par( WBUILD *wb )
{
    PAP pap;

    if ( !wb_open( wb ) ) {
        return;
    }
    if ( wb->pap.style >= wb->doc->nstyle ) {
        wb->pap.style = 0;
    }
    if ( pap_copy( &pap, &wb->pap ) ) {
        pap_free( &wb->para->pap );
        wb->para->pap = pap;
    } else {
        wb->doc->failed = 1;
    }
    wb->para = NULL;
}

void wb_object( WBUILD *wb, int obj )
{
    if ( obj >= 0 && wb_open( wb ) ) {
        wd_obj_put( wb->doc, wb->para, wb->para->len, obj, wb_chp( wb ) );
    }
}

void wb_story( WBUILD *wb, STORY *story )
{
    if ( wb->para != NULL ) {
        wb_par( wb );
    }
    if ( story != NULL ) {
        wb->story = story;
    }
}

void wb_done( WBUILD *wb )
{
    if ( wb->para != NULL ) {
        wb_par( wb );
    }
    pap_free( &wb->pap );
}
