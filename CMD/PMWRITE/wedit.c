/*
 * WEDIT.C - changes to a document that can be taken back (WEDIT.H).
 *
 * SIX THINGS can be done to a story, and everything else is made of
 * them: text put into a paragraph, text taken out of one, a paragraph
 * split in two, two joined, the looks of some characters changed, a
 * paragraph's own properties changed.  Each is a function here (p_*)
 * that does its one thing and writes down the opposite: the opposite
 * of putting text in is "take this many out from here", the opposite
 * of taking it out is the text itself.  Undo does what the records
 * say, last one first, through the same six functions - which write
 * down the opposite again, and that is Redo.
 *
 * A RECORD IS MADE BEFORE THE CHANGE, and a change whose record
 * cannot be made is not made.  So whatever has happened when memory
 * runs out has its records, and we_end can take it back.
 *
 * WHERE A TABLE CELL ENDS is a paragraph property, on the last
 * paragraph of the cell.  Splitting that paragraph moves the marks to
 * the second half, and joining two moves the second one's marks to
 * the one that is left, so a cell keeps its end whichever side of it
 * the typing is on.
 */
#include "fmt.h"
#include "wedit.h"

#define UNDO_STEPS      200     /* actions that can be taken back */

enum { UR_DELETE, UR_INSERT, UR_JOIN, UR_SPLIT, UR_ATTR, UR_PAP };

typedef struct UREC {
    struct UREC *next;
    int    op;                  /* UR_*: what to do, to undo */
    u32    para, off, len;
    u16   *text, *attr;         /* UR_INSERT; UR_ATTR has attr alone */
    WANCH *anch;                /* UR_INSERT: counted from "off" */
    u32    nanch;
    PAP    pap;                 /* UR_SPLIT: the second half's; UR_PAP */
    int    has_pap;
} UREC;

typedef struct UGROUP {
    struct UGROUP *next;
    UREC  *recs;                /* the last change first */
    STORY *story;
    WPOS   before, after;
    int    kind;                /* we_begin_more's: 0 for an action on its own */
} UGROUP;

typedef struct {
    UGROUP *undo, *redo;
    UGROUP *open;               /* the action being done */
    int     depth;              /* we_begin inside we_begin */
    int     failed;             /* a record could not be made */
    int     unsure;             /* a change was made with no record */
    u32     nundo;
} UNDO;

static void drop_all( WDOC *doc );

static UNDO *undo_of( WDOC *doc )
{
    UNDO *undo = (UNDO *)doc->undo;

    if ( undo == NULL ) {
        undo = (UNDO *)try_alloc( sizeof( UNDO ) );
        if ( undo != NULL ) {
            mem_set( undo, 0, sizeof( UNDO ) );
            doc->undo = undo;
            wd_undo_drop = drop_all;
        }
    }
    return undo;
}

/* ------------------------------------------------------------------ */
/* places                                                              */
/* ------------------------------------------------------------------ */

int wpos_cmp( WPOS lhs, WPOS rhs )
{
    if ( lhs.para != rhs.para ) {
        return lhs.para < rhs.para ? -1 : 1;
    }
    if ( lhs.off != rhs.off ) {
        return lhs.off < rhs.off ? -1 : 1;
    }
    return 0;
}

WPOS wpos_end( const STORY *story )
{
    WPOS pos;

    pos.para = story->count ? story->count - 1 : 0;
    pos.off = story->count ? story->para[pos.para]->len : 0;
    return pos;
}

WPOS wpos_clamp( const STORY *story, WPOS pos )
{
    if ( story->count == 0 ) {
        pos.para = pos.off = 0;
    } else if ( pos.para >= story->count ) {
        pos = wpos_end( story );
    } else if ( pos.off > story->para[pos.para]->len ) {
        pos.off = story->para[pos.para]->len;
    }
    return pos;
}

/* ------------------------------------------------------------------ */
/* records                                                             */
/* ------------------------------------------------------------------ */

static void rec_free( UREC *rec )
{
    xfree( rec->text );
    xfree( rec->attr );
    xfree( rec->anch );
    if ( rec->has_pap ) {
        pap_free( &rec->pap );
    }
    xfree( rec );
}

static void group_free( UGROUP *group )
{
    UREC *rec, *next;

    if ( group == NULL ) {
        return;
    }
    for ( rec = group->recs; rec != NULL; rec = next ) {
        next = rec->next;
        rec_free( rec );
    }
    xfree( group );
}

static void stack_free( UGROUP **stack )
{
    UGROUP *group, *next;

    for ( group = *stack; group != NULL; group = next ) {
        next = group->next;
        group_free( group );
    }
    *stack = NULL;
}

void we_forget( WDOC *doc )
{
    UNDO *undo = (UNDO *)doc->undo;

    if ( undo != NULL ) {
        stack_free( &undo->undo );
        stack_free( &undo->redo );
        undo->nundo = 0;
    }
}

static void drop_all( WDOC *doc )
{
    UNDO *undo = (UNDO *)doc->undo;

    if ( undo != NULL ) {
        we_forget( doc );
        group_free( undo->open );
        xfree( undo );
        doc->undo = NULL;
    }
}

/*
 * Is a record wanted, and can it be had?  -> 1 with *out the record
 * (NULL when none is wanted: nothing is being recorded); 0 when one
 * was wanted and there is no memory, and then the change is not made.
 */
static int rec_new( WDOC *doc, int op, u32 para, u32 off, u32 len, UREC **out )
{
    UNDO *undo = (UNDO *)doc->undo;
    UREC *rec;

    *out = NULL;
    if ( undo == NULL || undo->open == NULL ) {
        return 1;
    }
    rec = (UREC *)try_alloc( sizeof( UREC ) );
    if ( rec == NULL ) {
        undo->failed = 1;
        return 0;
    }
    mem_set( rec, 0, sizeof( UREC ) );
    rec->op = op;
    rec->para = para;
    rec->off = off;
    rec->len = len;
    *out = rec;
    return 1;
}

static void rec_keep( WDOC *doc, UREC *rec )
{
    UNDO *undo = (UNDO *)doc->undo;

    doc->serial++;
    if ( rec != NULL ) {
        rec->next = undo->open->recs;
        undo->open->recs = rec;
    }
}

static void rec_fail( WDOC *doc, UREC *rec )
{
    UNDO *undo = (UNDO *)doc->undo;

    if ( rec != NULL ) {
        rec_free( rec );
    }
    if ( undo != NULL ) {
        undo->failed = 1;
    }
}

/* ------------------------------------------------------------------ */
/* the six things                                                      */
/* ------------------------------------------------------------------ */

/* the objects in a stretch of a paragraph, counted from its start */
static u32 anch_count( const WPARA *para, u32 off, u32 len )
{
    u32 index, count = 0;

    for ( index = 0; index < para->nanch; index++ ) {
        if ( para->anch[index].off >= off && para->anch[index].off - off < len ) {
            count++;
        }
    }
    return count;
}

static void anch_take( const WPARA *para, u32 off, u32 len, WANCH *out )
{
    u32 index, count = 0;

    for ( index = 0; index < para->nanch; index++ ) {
        if ( para->anch[index].off >= off && para->anch[index].off - off < len ) {
            out[count].off = para->anch[index].off - off;
            out[count].obj = para->anch[index].obj;
            count++;
        }
    }
}

/* text in.  "attr" NULL: all of it "chp", and no objects. */
static int p_insert( WDOC *doc, STORY *story, u32 pi, u32 off, const u16 *text, const u16 *attr,
                     u16 chp, u32 len, const WANCH *anch, u32 nanch )
{
    WPARA *para = story->para[pi];
    UREC *rec;
    int done;

    if ( len == 0 ) {
        return 1;
    }
    if ( !rec_new( doc, UR_DELETE, pi, off, len, &rec ) ) {
        return 0;
    }
    if ( attr == NULL ) {
        done = wd_text_insert( doc, para, off, text, len, chp );
    } else {
        done = wd_text_restore( doc, para, off, text, attr, len, anch, nanch );
    }
    if ( !done ) {
        rec_fail( doc, rec );
        return 0;
    }
    rec_keep( doc, rec );
    return 1;
}

/* text out */
static int p_delete( WDOC *doc, STORY *story, u32 pi, u32 off, u32 len )
{
    WPARA *para = story->para[pi];
    UREC *rec;

    if ( off >= para->len || len == 0 ) {
        return 1;
    }
    if ( len > para->len - off ) {
        len = para->len - off;
    }
    if ( !rec_new( doc, UR_INSERT, pi, off, len, &rec ) ) {
        return 0;
    }
    if ( rec != NULL ) {
        rec->nanch = anch_count( para, off, len );
        rec->text = (u16 *)try_alloc( len * sizeof( u16 ) );
        rec->attr = (u16 *)try_alloc( len * sizeof( u16 ) );
        rec->anch = rec->nanch ? (WANCH *)try_alloc( rec->nanch * sizeof( WANCH ) ) : NULL;
        if ( rec->text == NULL || rec->attr == NULL || (rec->nanch && rec->anch == NULL) ) {
            rec_fail( doc, rec );
            return 0;
        }
        mem_cpy( rec->text, para->text + off, len * sizeof( u16 ) );
        mem_cpy( rec->attr, para->attr + off, len * sizeof( u16 ) );
        anch_take( para, off, len, rec->anch );
    }
    wd_text_delete( doc, para, off, len );
    rec_keep( doc, rec );
    return 1;
}

#define CELL_MARKS      (PF_CELLEND | PF_ROWEND)

/* a paragraph ended at "off": what was after it is a new paragraph,
   with "second" for its properties or a copy of the first one's */
static int p_split( WDOC *doc, STORY *story, u32 pi, u32 off, const PAP *second )
{
    WPARA *para = story->para[pi], *fresh;
    WANCH *moved = NULL;
    UREC *rec;
    u32 tail, count;

    if ( off > para->len ) {
        off = para->len;
    }
    tail = para->len - off;
    count = anch_count( para, off, tail );
    if ( !rec_new( doc, UR_JOIN, pi, off, 0, &rec ) ) {
        return 0;
    }
    if ( count ) {
        moved = (WANCH *)try_alloc( count * sizeof( WANCH ) );
        if ( moved == NULL ) {
            rec_fail( doc, rec );
            return 0;
        }
        anch_take( para, off, tail, moved );
    }
    fresh = wd_para_insert( doc, story, pi + 1, second != NULL ? second : &para->pap );
    if ( fresh == NULL ||
         !wd_text_restore( doc, fresh, 0, para->text + off, para->attr + off, tail, moved, count ) ) {
        if ( fresh != NULL ) {
            wd_para_remove( doc, story, pi + 1 );
        }
        xfree( moved );
        rec_fail( doc, rec );
        return 0;
    }
    xfree( moved );
    wd_text_delete( doc, para, off, tail );
    /* a cell's end is where the second half ends now */
    para->pap.flags &= (u16)~CELL_MARKS;
    xfree( para->pap.row );
    para->pap.row = NULL;
    wd_para_touch( para );
    rec_keep( doc, rec );
    return 1;
}

static int p_pap( WDOC *doc, STORY *story, u32 pi, const PAP *pap );

/* a paragraph joined to the one after it, which goes */
static int p_join( WDOC *doc, STORY *story, u32 pi )
{
    WPARA *para, *next;
    WANCH *moved = NULL;
    UREC *rec;
    PAP plain;
    u32 count;
    int done;

    if ( pi + 1 >= story->count ) {
        return 1;
    }
    para = story->para[pi];
    next = story->para[pi + 1];
    if ( (para->pap.flags & CELL_MARKS) || para->pap.row != NULL ) {
        /* the first one is the end of a cell, and will not be: that is
           a change of its own, written down as one, so that taking the
           join back puts the cell's end back as well */
        if ( !pap_copy( &plain, &para->pap ) ) {
            doc->failed = 1;
            return 0;
        }
        plain.flags &= (u16)~CELL_MARKS;
        xfree( plain.row );
        plain.row = NULL;
        done = p_pap( doc, story, pi, &plain );
        pap_free( &plain );
        if ( !done ) {
            return 0;
        }
    }
    if ( !rec_new( doc, UR_SPLIT, pi, para->len, 0, &rec ) ) {
        return 0;
    }
    if ( rec != NULL ) {
        if ( !pap_copy( &rec->pap, &next->pap ) ) {
            rec_fail( doc, rec );
            return 0;
        }
        rec->has_pap = 1;
    }
    count = next->nanch;
    if ( count ) {
        moved = (WANCH *)try_alloc( count * sizeof( WANCH ) );
        if ( moved == NULL ) {
            rec_fail( doc, rec );
            return 0;
        }
        anch_take( next, 0, next->len, moved );
    }
    if ( !wd_text_restore( doc, para, para->len, next->text, next->attr, next->len, moved, count ) ) {
        xfree( moved );
        rec_fail( doc, rec );
        return 0;
    }
    xfree( moved );
    /* the cell's end, if the paragraph that goes was one */
    para->pap.flags = (u16)((para->pap.flags & ~CELL_MARKS) | (next->pap.flags & CELL_MARKS));
    xfree( para->pap.row );
    para->pap.row = next->pap.row;
    next->pap.row = NULL;
    wd_para_touch( para );
    wd_para_remove( doc, story, pi + 1 );
    rec_keep( doc, rec );
    return 1;
}

/* the looks of "len" characters: "attr" has the new CHP numbers */
static int p_attr( WDOC *doc, STORY *story, u32 pi, u32 off, u32 len, const u16 *attr )
{
    WPARA *para = story->para[pi];
    UREC *rec;
    u32 index;

    if ( off >= para->len || len == 0 ) {
        return 1;
    }
    if ( len > para->len - off ) {
        len = para->len - off;
    }
    if ( mem_cmp( para->attr + off, attr, len * sizeof( u16 ) ) == 0 ) {
        return 1;
    }
    if ( !rec_new( doc, UR_ATTR, pi, off, len, &rec ) ) {
        return 0;
    }
    if ( rec != NULL ) {
        rec->attr = (u16 *)try_alloc( len * sizeof( u16 ) );
        if ( rec->attr == NULL ) {
            rec_fail( doc, rec );
            return 0;
        }
        mem_cpy( rec->attr, para->attr + off, len * sizeof( u16 ) );
    }
    for ( index = 0; index < len; index++ ) {
        para->attr[off + index] = (u16)(attr[index] < doc->nchp ? attr[index] : 0);
    }
    wd_para_touch( para );
    rec_keep( doc, rec );
    return 1;
}

/* a paragraph's own properties */
static int p_pap( WDOC *doc, STORY *story, u32 pi, const PAP *pap )
{
    WPARA *para = story->para[pi];
    UREC *rec;
    PAP fresh;

    if ( !rec_new( doc, UR_PAP, pi, 0, 0, &rec ) ) {
        return 0;
    }
    if ( !pap_copy( &fresh, pap ) ) {
        rec_fail( doc, rec );
        return 0;
    }
    if ( fresh.style >= doc->nstyle ) {
        fresh.style = 0;
    }
    if ( rec != NULL ) {
        rec->pap = para->pap;           /* the old one, tabs and row and all */
        rec->has_pap = 1;
    } else {
        pap_free( &para->pap );
    }
    para->pap = fresh;
    wd_para_touch( para );
    rec_keep( doc, rec );
    return 1;
}

/* what a record says */
static void rec_apply( WDOC *doc, STORY *story, UREC *rec )
{
    if ( rec->para >= story->count ) {
        return;                         /* cannot be: the records are in step */
    }
    switch ( rec->op ) {
    case UR_DELETE:
        p_delete( doc, story, rec->para, rec->off, rec->len );
        break;
    case UR_INSERT:
        p_insert( doc, story, rec->para, rec->off, rec->text, rec->attr, 0, rec->len, rec->anch,
                  rec->nanch );
        break;
    case UR_JOIN:
        p_join( doc, story, rec->para );
        break;
    case UR_SPLIT:
        p_split( doc, story, rec->para, rec->off, rec->has_pap ? &rec->pap : NULL );
        break;
    case UR_ATTR:
        p_attr( doc, story, rec->para, rec->off, rec->len, rec->attr );
        break;
    case UR_PAP:
        p_pap( doc, story, rec->para, &rec->pap );
        break;
    }
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

void we_begin( WDOC *doc, STORY *story, WPOS caret )
{
    UNDO *undo = undo_of( doc );
    UGROUP *group;

    if ( undo == NULL ) {
        return;                         /* no memory even for that: no history */
    }
    if ( undo->depth++ > 0 ) {
        return;
    }
    undo->failed = 0;
    doc->failed = 0;
    stack_free( &undo->redo );
    group = (UGROUP *)try_alloc( sizeof( UGROUP ) );
    if ( group == NULL ) {
        undo->unsure = 1;               /* done, but with nothing to undo it by */
        return;
    }
    mem_set( group, 0, sizeof( UGROUP ) );
    group->story = story;
    group->before = caret;
    undo->open = group;
}

void we_begin_more( WDOC *doc, STORY *story, WPOS caret, int kind )
{
    UNDO *undo = undo_of( doc );
    UGROUP *last;

    if ( undo != NULL && undo->depth == 0 && kind != 0 && undo->redo == NULL && undo->undo != NULL ) {
        last = undo->undo;
        if ( last->kind == kind && last->story == story && wpos_cmp( last->after, caret ) == 0 ) {
            /* the action before, opened again */
            undo->undo = last->next;
            if ( undo->nundo ) {
                undo->nundo--;
            }
            last->next = NULL;
            undo->open = last;
            undo->depth = 1;
            undo->failed = 0;
            doc->failed = 0;
            return;
        }
    }
    we_begin( doc, story, caret );
    if ( undo != NULL && undo->depth == 1 && undo->open != NULL ) {
        undo->open->kind = kind;
    }
}

/* the oldest action forgotten, when there are too many */
static void trim( UNDO *undo )
{
    UGROUP **link = &undo->undo;
    u32 kept = 0;

    while ( *link != NULL && kept < UNDO_STEPS ) {
        link = &(*link)->next;
        kept++;
    }
    stack_free( link );
    undo->nundo = kept;
}

int we_end( WDOC *doc, WPOS caret )
{
    UNDO *undo = (UNDO *)doc->undo;
    UGROUP *group;
    UREC *rec;
    int bad;

    if ( undo == NULL ) {
        bad = doc->failed;
        doc->failed = 0;
        return !bad;
    }
    if ( undo->depth > 0 && --undo->depth > 0 ) {
        return 1;
    }
    group = undo->open;
    undo->open = NULL;
    bad = undo->failed || doc->failed;
    doc->failed = 0;
    undo->failed = 0;
    if ( bad ) {
        /* take back what was done, with nothing written down */
        if ( group != NULL ) {
            for ( rec = group->recs; rec != NULL; rec = rec->next ) {
                rec_apply( doc, group->story, rec );
            }
        }
        doc->failed = 0;
        group_free( group );
        we_forget( doc );
        undo->unsure = 0;
        return 0;
    }
    if ( undo->unsure ) {
        undo->unsure = 0;
        group_free( group );
        we_forget( doc );
        return 1;
    }
    if ( group != NULL && group->recs != NULL ) {
        group->after = caret;
        group->next = undo->undo;
        undo->undo = group;
        undo->nundo++;
        if ( undo->nundo > UNDO_STEPS ) {
            trim( undo );
        }
    } else {
        group_free( group );
    }
    return 1;
}

int we_can_undo( const WDOC *doc )
{
    const UNDO *undo = (const UNDO *)doc->undo;

    return undo != NULL && undo->undo != NULL;
}

int we_can_redo( const WDOC *doc )
{
    const UNDO *undo = (const UNDO *)doc->undo;

    return undo != NULL && undo->redo != NULL;
}

u32 we_serial( const WDOC *doc )
{
    return doc->serial;
}

/* one action off one stack, done, and its opposite put on the other */
static int replay( WDOC *doc, UGROUP **from, UGROUP **onto, STORY **story, WPOS *caret )
{
    UNDO *undo = (UNDO *)doc->undo;
    UGROUP *group, *back;
    UREC *rec;

    if ( undo == NULL || *from == NULL || undo->open != NULL ) {
        return 0;
    }
    group = *from;
    *from = group->next;
    back = (UGROUP *)try_alloc( sizeof( UGROUP ) );
    if ( back != NULL ) {
        mem_set( back, 0, sizeof( UGROUP ) );
        back->story = group->story;
        back->before = group->after;
        back->after = group->before;
    }
    undo->open = back;                  /* NULL: done, with nothing written down */
    undo->failed = 0;
    doc->failed = 0;
    for ( rec = group->recs; rec != NULL; rec = rec->next ) {
        rec_apply( doc, group->story, rec );
    }
    undo->open = NULL;
    *story = group->story;
    *caret = wpos_clamp( group->story, group->before );
    group_free( group );
    if ( back == NULL || undo->failed || doc->failed ) {
        /* it was done as far as it could be, and that is all that is
           known: nothing before or after it can be trusted */
        doc->failed = 0;
        undo->failed = 0;
        group_free( back );
        we_forget( doc );
        return 1;
    }
    back->next = *onto;
    *onto = back;
    return 1;
}

int we_undo( WDOC *doc, STORY **story, WPOS *caret )
{
    UNDO *undo = (UNDO *)doc->undo;

    if ( undo == NULL || !replay( doc, &undo->undo, &undo->redo, story, caret ) ) {
        return 0;
    }
    if ( undo->nundo ) {
        undo->nundo--;
    }
    return 1;
}

int we_redo( WDOC *doc, STORY **story, WPOS *caret )
{
    UNDO *undo = (UNDO *)doc->undo;

    if ( undo == NULL || !replay( doc, &undo->redo, &undo->undo, story, caret ) ) {
        return 0;
    }
    undo->nundo++;
    return 1;
}

/* ------------------------------------------------------------------ */
/* what the commands are made of                                       */
/* ------------------------------------------------------------------ */

void we_insert( WDOC *doc, STORY *story, WPOS *pos, const u16 *text, u32 len, u16 chp )
{
    if ( story->count == 0 ) {
        return;
    }
    *pos = wpos_clamp( story, *pos );
    if ( p_insert( doc, story, pos->para, pos->off, text, NULL, chp, len, NULL, 0 ) ) {
        pos->off += len;
    }
}

void we_insert_obj( WDOC *doc, STORY *story, WPOS *pos, int obj, u16 chp )
{
    WANCH anch;
    u16 unit = WCH_OBJ;

    if ( story->count == 0 || obj < 0 || (u32)obj >= doc->nobj ) {
        return;
    }
    *pos = wpos_clamp( story, *pos );
    anch.off = 0;
    anch.obj = (u32)obj;
    if ( p_insert( doc, story, pos->para, pos->off, &unit, &chp, 0, 1, &anch, 1 ) ) {
        pos->off++;
    }
}

void we_split( WDOC *doc, STORY *story, WPOS *pos )
{
    if ( story->count == 0 ) {
        return;
    }
    *pos = wpos_clamp( story, *pos );
    if ( p_split( doc, story, pos->para, pos->off, NULL ) ) {
        pos->para++;
        pos->off = 0;
    }
}

void we_delete( WDOC *doc, STORY *story, WPOS from, WPOS to )
{
    WPOS hold;
    u32 gone;

    from = wpos_clamp( story, from );
    to = wpos_clamp( story, to );
    if ( wpos_cmp( from, to ) > 0 ) {
        hold = from;
        from = to;
        to = hold;
    }
    if ( story->count == 0 || wpos_cmp( from, to ) == 0 ) {
        return;
    }
    if ( from.para == to.para ) {
        p_delete( doc, story, from.para, from.off, to.off - from.off );
        return;
    }
    if ( !p_delete( doc, story, from.para, from.off, story->para[from.para]->len - from.off ) ) {
        return;
    }
    /* each paragraph after it emptied as far as the piece goes, and
       joined on: the last one keeps what was after the piece */
    for ( gone = from.para + 1; gone <= to.para; gone++ ) {
        if ( !p_delete( doc, story, from.para + 1, 0,
                        gone < to.para ? story->para[from.para + 1]->len : to.off ) ||
             !p_join( doc, story, from.para ) ) {
            return;
        }
    }
}

void we_insert_table( WDOC *doc, STORY *story, WPOS *pos, int rows, int cols, s32 width )
{
    PAP cell, rest;
    TROW row;
    u32 at;
    int down, across, done;

    if ( story->count == 0 || rows < 1 || rows > 500 || cols < 1 || cols > MAX_CELLS || width < 720 ||
         width > 31680 ) {
        return;
    }
    *pos = wpos_clamp( story, *pos );
    if ( story->para[pos->para]->pap.flags & PF_INTABLE ) {
        return;
    }
    /* the table starts a paragraph of its own: what is before the place
       stays above it */
    if ( pos->off > 0 ) {
        if ( !p_split( doc, story, pos->para, pos->off, NULL ) ) {
            return;
        }
        pos->para++;
        pos->off = 0;
    }
    at = pos->para;
    mem_set( &row, 0, sizeof( row ) );
    row.ncells = (u8)cols;
    row.borders = 1;
    for ( across = 0; across < cols; across++ ) {
        row.edge[across] = (s32)((u32)width * (u32)(across + 1) / (u32)cols);
    }
    for ( down = 0; down < rows; down++ ) {
        for ( across = 0; across < cols; across++ ) {
            /* an empty paragraph in front of the one at "at", which goes
               on as it was: the empty one is the cell */
            if ( !pap_copy( &rest, &story->para[at]->pap ) ) {
                doc->failed = 1;
                return;
            }
            done = p_split( doc, story, at, 0, &rest );
            pap_free( &rest );
            if ( !done ) {
                return;
            }
            pap_init( &cell );
            cell.flags = PF_INTABLE | PF_CELLEND;
            if ( across == cols - 1 ) {
                cell.flags |= PF_ROWEND;
                cell.row = &row;        /* p_pap keeps a copy of its own */
            }
            if ( !p_pap( doc, story, at, &cell ) ) {
                return;
            }
            at++;
        }
    }
}

/*
 * An empty row after the row that paragraph "row_end" ends, with as many
 * cells and the same edges.  Each new cell is made by ending the last
 * paragraph of the row once more: the new paragraph takes over being
 * the row's end, and the one before it is given back what it was - the
 * old row's end the first time, a cell's end and no more after that.
 */
void we_add_row( WDOC *doc, STORY *story, u32 row_end, WPOS *pos )
{
    const WPARA *para;
    PAP whole, cell;
    u32 ncells, index;
    int done = 1;

    if ( row_end >= story->count ) {
        return;
    }
    para = story->para[row_end];
    if ( !(para->pap.flags & PF_ROWEND) || para->pap.row == NULL || para->pap.row->ncells == 0 ) {
        return;
    }
    ncells = para->pap.row->ncells;
    if ( !pap_copy( &whole, &para->pap ) ) {
        doc->failed = 1;
        return;
    }
    if ( !pap_copy( &cell, &para->pap ) ) {
        pap_free( &whole );
        doc->failed = 1;
        return;
    }
    cell.flags &= (u16)~PF_ROWEND;
    xfree( cell.row );
    cell.row = NULL;
    for ( index = 0; index < ncells && done; index++ ) {
        done = p_split( doc, story, row_end + index, story->para[row_end + index]->len, NULL ) &&
               p_pap( doc, story, row_end + index, index == 0 ? &whole : &cell );
    }
    pap_free( &whole );
    pap_free( &cell );
    if ( done ) {
        pos->para = row_end + 1;
        pos->off = 0;
    }
}

/* ------------------------------------------------------------------ */
/* a table's rows and columns                                          */
/* ------------------------------------------------------------------ */
/*
 * A table is nothing but paragraphs: PF_INTABLE on each, PF_CELLEND on
 * the last of a cell, PF_ROWEND and the row's TROW on the last of a
 * row.  So a row or a column goes, or comes, by the same six changes
 * everything else is made of, and Undo takes each of these back whole.
 * The commands leave a table alone when a row of it is not as they
 * expect - cells without ends, or more ends than the row's TROW has
 * edges - which no table made here is, and a file's may be.
 */

int we_row_of( const STORY *story, u32 pi, u32 *first, u32 *end )
{
    const WPARA *para;
    u32 low = pi, high = pi;

    if ( pi >= story->count || !(story->para[pi]->pap.flags & PF_INTABLE) ) {
        return 0;
    }
    while ( low > 0 && (story->para[low - 1]->pap.flags & (PF_INTABLE | PF_ROWEND)) == PF_INTABLE ) {
        low--;
    }
    while ( high < story->count && (story->para[high]->pap.flags & (PF_INTABLE | PF_ROWEND)) == PF_INTABLE ) {
        high++;
    }
    if ( high >= story->count ) {
        return 0;
    }
    para = story->para[high];
    if ( (para->pap.flags & (PF_INTABLE | PF_ROWEND)) != (PF_INTABLE | PF_ROWEND) || para->pap.row == NULL ||
         para->pap.row->ncells == 0 || para->pap.row->ncells > MAX_CELLS ) {
        return 0;
    }
    *first = low;
    *end = high;
    return 1;
}

/* how many cells the row "first" to "end" has, by their ends; the last
   paragraph ends one whether it says so or not */
static u32 row_cells( const STORY *story, u32 first, u32 end )
{
    u32 scan, count = 1;

    for ( scan = first; scan < end; scan++ ) {
        if ( story->para[scan]->pap.flags & PF_CELLEND ) {
            count++;
        }
    }
    return count;
}

/* the paragraphs of the row's cell "which", counted from 0: 0 if the
   row has not so many */
static int cell_span( const STORY *story, u32 first, u32 end, u32 which, u32 *from, u32 *to )
{
    u32 scan, start = first, cell = 0;

    for ( scan = first; scan <= end; scan++ ) {
        if ( scan == end || (story->para[scan]->pap.flags & PF_CELLEND) ) {
            if ( cell == which ) {
                *from = start;
                *to = scan;
                return 1;
            }
            cell++;
            start = scan + 1;
        }
    }
    return 0;
}

int we_cell_of( const STORY *story, u32 pi )
{
    u32 first, end, scan, cell = 0;

    if ( !we_row_of( story, pi, &first, &end ) ) {
        return -1;
    }
    for ( scan = first; scan < pi; scan++ ) {
        if ( story->para[scan]->pap.flags & PF_CELLEND ) {
            cell++;
        }
    }
    return (int)cell;
}

/* the table paragraph "pi" is in: its first row's first paragraph and
   its last row's last.  0 if "pi" is not in a row. */
static int table_of( const STORY *story, u32 pi, u32 *first, u32 *last )
{
    u32 low, high, from, to;

    if ( !we_row_of( story, pi, &low, &high ) ) {
        return 0;
    }
    while ( low > 0 && we_row_of( story, low - 1, &from, &to ) ) {
        low = from;
    }
    while ( high + 1 < story->count && we_row_of( story, high + 1, &from, &to ) ) {
        high = to;
    }
    *first = low;
    *last = high;
    return 1;
}

/* is every row of the table as the commands expect: as many ends of
   cells as its TROW has edges? */
static int table_sound( const STORY *story, u32 first, u32 last )
{
    u32 from, to, at = first;

    while ( at <= last ) {
        if ( !we_row_of( story, at, &from, &to ) || from != at ||
             row_cells( story, from, to ) != story->para[to]->pap.row->ncells ) {
            return 0;
        }
        at = to + 1;
    }
    return 1;
}

/*
 * Paragraphs "from" to "to" taken out whole, with what is in them.  A
 * join leaves the first paragraph's properties and the last one's cell
 * marks, which is right for text and wrong here: the paragraph that
 * stays - the one before, or failing that the one after - is given its
 * own properties back when the joining is done.  0 if there was not the
 * memory to finish; what was done can be undone.
 */
static int paras_remove( WDOC *doc, STORY *story, u32 from, u32 to )
{
    PAP keep;
    WPOS start, stop;
    u32 want = story->count - (to - from + 1);
    int done;

    if ( from > 0 ) {
        if ( !pap_copy( &keep, &story->para[from - 1]->pap ) ) {
            doc->failed = 1;
            return 0;
        }
        start.para = from - 1;
        start.off = story->para[from - 1]->len;
        stop.para = to;
        stop.off = story->para[to]->len;
        we_delete( doc, story, start, stop );
        done = p_pap( doc, story, from - 1, &keep ) && story->count == want;
        pap_free( &keep );
        return done;
    }
    if ( to + 1 < story->count ) {
        if ( !pap_copy( &keep, &story->para[to + 1]->pap ) ) {
            doc->failed = 1;
            return 0;
        }
        start.para = 0;
        start.off = 0;
        stop.para = to + 1;
        stop.off = 0;
        we_delete( doc, story, start, stop );
        /* only when all of it went is the first paragraph the one that
           was after the piece */
        done = story->count == want && p_pap( doc, story, 0, &keep );
        pap_free( &keep );
        return done;
    }
    /* the piece is all there is: one empty paragraph is left */
    start.para = 0;
    start.off = 0;
    stop.para = to;
    stop.off = story->para[to]->len;
    we_delete( doc, story, start, stop );
    if ( story->count != 1 ) {
        return 0;
    }
    pap_init( &keep );
    return p_pap( doc, story, 0, &keep );
}

void we_delete_rows( WDOC *doc, STORY *story, u32 from, u32 to, WPOS *pos )
{
    u32 first, last, low, high;

    if ( from > to ) {
        low = from;
        from = to;
        to = low;
    }
    if ( !we_row_of( story, from, &first, &high ) || !we_row_of( story, to, &low, &last ) ) {
        return;
    }
    /* one table: nothing between the two rows that is not a row */
    if ( !table_of( story, from, &low, &high ) || last > high ) {
        return;
    }
    if ( paras_remove( doc, story, first, last ) ) {
        pos->para = first < story->count ? first : story->count - 1;
        pos->off = 0;
    }
}

/* paragraph "pi" given the row's ends again: "row" for its TROW */
static int row_set( WDOC *doc, STORY *story, u32 pi, const TROW *row, u16 more_flags )
{
    PAP fresh;
    int done;

    if ( !pap_copy( &fresh, &story->para[pi]->pap ) ) {
        doc->failed = 1;
        return 0;
    }
    fresh.flags |= more_flags;
    if ( row != NULL && !pap_set_row( &fresh, row ) ) {
        pap_free( &fresh );
        doc->failed = 1;
        return 0;
    }
    done = p_pap( doc, story, pi, &fresh );
    pap_free( &fresh );
    return done;
}

/* value * num / den, for a value no more than "den" and a "num" under
   32768: the two are halved together until the product fits */
static s32 scaled( s32 value, s32 num, s32 den )
{
    if ( value < 0 || num < 0 ) {
        return 0;
    }
    while ( den > 0xFFFF ) {
        value >>= 1;
        den >>= 1;
    }
    return den > 0 ? (s32)((u32)value * (u32)num / (u32)den) : 0;
}

/* how wide the row's cell "which" is */
static s32 cell_wide( const TROW *row, u32 which )
{
    return row->edge[which] - (which ? row->edge[which - 1] : row->left);
}

void we_insert_col( WDOC *doc, STORY *story, u32 pi, int after, s32 limit, WPOS *pos )
{
    TROW row;
    u32 first, last, from, to, at, cells, where, like, index, mine_first, mine_end, rows_before = 0;
    u32 fresh = 0;
    s32 wide, span;
    int cell = we_cell_of( story, pi ), have = 0;

    if ( cell < 0 || !table_of( story, pi, &first, &last ) || !table_sound( story, first, last ) ||
         !we_row_of( story, pi, &mine_first, &mine_end ) ) {
        return;
    }
    /* room in every row for one more */
    for ( at = first; at <= last; at = to + 1 ) {
        we_row_of( story, at, &from, &to );
        if ( story->para[to]->pap.row->ncells >= MAX_CELLS ) {
            return;
        }
    }
    /* from the last row to the first, so that the rows still to do are
       where they were */
    at = last;
    for ( ;; ) {
        we_row_of( story, at, &from, &to );
        row = *story->para[to]->pap.row;
        cells = row.ncells;
        where = (u32)cell + (after ? 1 : 0);
        if ( where > cells ) {
            where = cells;
        }
        /* as wide as the column the caret is in */
        like = (u32)cell < cells ? (u32)cell : cells - 1;
        wide = cell_wide( &row, like );
        if ( wide < 144 ) {
            wide = 144;
        }
        for ( index = cells; index > where; index-- ) {
            row.edge[index] = row.edge[index - 1] + wide;
        }
        row.edge[where] = (where ? row.edge[where - 1] : row.left) + wide;
        row.ncells = (u8)(cells + 1);
        /* held to the width of the text: every column gives a little */
        span = row.edge[cells] - row.left;
        if ( limit > row.left && row.edge[cells] > limit && span > 0 ) {
            for ( index = 0; index <= cells; index++ ) {
                row.edge[index] = row.left + scaled( row.edge[index] - row.left, limit - row.left, span );
            }
        }
        if ( where < cells ) {
            /* an empty paragraph in front of the cell that is there: a
               cell by itself */
            cell_span( story, from, to, where, &fresh, &index );
            if ( !p_split( doc, story, fresh, 0, NULL ) || !row_set( doc, story, fresh, NULL, PF_CELLEND ) ||
                 !row_set( doc, story, to + 1, &row, 0 ) ) {
                return;
            }
        } else {
            /* after the last: the row's end moves into the new one */
            if ( !p_split( doc, story, to, story->para[to]->len, NULL ) ||
                 !row_set( doc, story, to, NULL, PF_CELLEND ) || !row_set( doc, story, to + 1, &row, 0 ) ) {
                return;
            }
            fresh = to + 1;
        }
        if ( from == mine_first ) {
            pos->para = fresh;
            pos->off = 0;
            have = 1;
        } else if ( have ) {
            rows_before++;
        }
        if ( from == first ) {
            break;
        }
        at = from - 1;
    }
    if ( have ) {
        pos->para += rows_before;       /* each row above has a paragraph more */
    }
}

void we_delete_col( WDOC *doc, STORY *story, u32 pi, WPOS *pos )
{
    TROW row;
    u32 first, last, from, to, at, cells, index, cell_from, cell_to, mine_first, mine_end, before;
    s32 wide;
    int cell = we_cell_of( story, pi ), have = 0, done;

    if ( cell < 0 || !table_of( story, pi, &first, &last ) || !table_sound( story, first, last ) ||
         !we_row_of( story, pi, &mine_first, &mine_end ) ) {
        return;
    }
    at = last;
    for ( ;; ) {
        we_row_of( story, at, &from, &to );
        row = *story->para[to]->pap.row;
        cells = row.ncells;
        before = story->count;
        done = 1;
        if ( (u32)cell < cells && cells == 1 ) {
            /* its only column: the row goes */
            done = paras_remove( doc, story, from, to );
            if ( from == mine_first ) {
                pos->para = from;
            }
        } else if ( (u32)cell < cells ) {
            wide = cell_wide( &row, (u32)cell );
            for ( index = (u32)cell; index + 1 < cells; index++ ) {
                row.edge[index] = row.edge[index + 1] - wide;
            }
            row.edge[cells - 1] = 0;
            row.ncells = (u8)(cells - 1);
            cell_span( story, from, to, (u32)cell, &cell_from, &cell_to );
            if ( (u32)cell + 1 < cells ) {
                /* the cell after it moves up; the row's end is told the
                   new edges */
                done = paras_remove( doc, story, cell_from, cell_to ) &&
                       row_set( doc, story, to - (cell_to - cell_from + 1), &row, 0 );
                if ( from == mine_first ) {
                    pos->para = cell_from;
                }
            } else {
                /* the last cell: the one before it ends the row now.
                   paras_remove gives that paragraph its own properties
                   back, and then the row's end is put on it. */
                done = paras_remove( doc, story, cell_from, cell_to ) &&
                       row_set( doc, story, cell_from - 1, &row, PF_CELLEND | PF_ROWEND );
                if ( from == mine_first ) {
                    pos->para = cell_from - 1;
                }
            }
        } else if ( from == mine_first ) {
            pos->para = pi;
        }
        if ( !done ) {
            return;
        }
        if ( from == mine_first ) {
            pos->off = 0;
            have = 1;
        } else if ( have ) {
            pos->para -= before - story->count;         /* a row above lost paragraphs */
        }
        if ( from == first ) {
            break;
        }
        at = from - 1;
    }
    if ( pos->para >= story->count ) {
        pos->para = story->count - 1;
    }
}

s32 we_col_width( const STORY *story, u32 pi )
{
    u32 first, end;
    int cell = we_cell_of( story, pi );

    if ( cell < 0 || !we_row_of( story, pi, &first, &end ) || (u32)cell >= story->para[end]->pap.row->ncells ) {
        return 0;
    }
    return cell_wide( story->para[end]->pap.row, (u32)cell );
}

void we_set_col_width( WDOC *doc, STORY *story, u32 pi, s32 width )
{
    TROW row;
    u32 first, last, from, to, at, index;
    s32 change;
    int cell = we_cell_of( story, pi );

    if ( cell < 0 || width < 144 || width > 31680 || !table_of( story, pi, &first, &last ) ) {
        return;
    }
    for ( at = first; at <= last; at = to + 1 ) {
        if ( !we_row_of( story, at, &from, &to ) ) {
            return;
        }
        row = *story->para[to]->pap.row;
        if ( (u32)cell >= row.ncells ) {
            continue;
        }
        change = width - cell_wide( &row, (u32)cell );
        if ( change == 0 ) {
            continue;
        }
        for ( index = (u32)cell; index < row.ncells; index++ ) {
            row.edge[index] += change;
        }
        if ( !row_set( doc, story, to, &row, 0 ) ) {
            return;
        }
    }
}

/* an empty row above the row paragraph "pi" is in */
void we_add_row_above( WDOC *doc, STORY *story, u32 pi, WPOS *pos )
{
    TROW row;
    PAP cell;
    u32 first, end, index, cells;
    int done = 1;

    if ( !we_row_of( story, pi, &first, &end ) ) {
        return;
    }
    row = *story->para[end]->pap.row;
    cells = row.ncells;
    for ( index = 0; index < cells && done; index++ ) {
        /* an empty paragraph in front of the row's first, which goes on
           as it was: the empty one is a cell, the last of them the row's
           end */
        if ( !p_split( doc, story, first + index, 0, NULL ) ) {
            return;
        }
        pap_init( &cell );
        cell.flags = PF_INTABLE | PF_CELLEND;
        if ( index + 1 == cells ) {
            cell.flags |= PF_ROWEND;
            cell.row = &row;            /* p_pap keeps a copy of its own */
        }
        done = p_pap( doc, story, first + index, &cell );
    }
    if ( done ) {
        pos->para = first;
        pos->off = 0;
    }
}

void chp_apply( CHP *chp, u32 mask, const CHP *value, u16 flag_mask )
{
    if ( mask & CM_FONT ) {
        chp->font = value->font;
    }
    if ( mask & CM_SIZE ) {
        chp->size = value->size;
    }
    if ( mask & CM_COLOR ) {
        chp->color = value->color;
    }
    if ( mask & CM_HILITE ) {
        chp->hilite = value->hilite;
    }
    if ( mask & CM_FLAGS ) {
        chp->flags = (u16)((chp->flags & ~flag_mask) | (value->flags & flag_mask));
    }
}

/* the stretch of one paragraph that lies between two places */
static int span_of( const STORY *story, u32 pi, WPOS from, WPOS to, u32 *start, u32 *len )
{
    u32 end;

    *start = pi == from.para ? from.off : 0;
    end = pi == to.para ? to.off : story->para[pi]->len;
    if ( end <= *start ) {
        *len = 0;
        return 0;
    }
    *len = end - *start;
    return 1;
}

static void order( const STORY *story, WPOS *from, WPOS *to )
{
    WPOS hold;

    *from = wpos_clamp( story, *from );
    *to = wpos_clamp( story, *to );
    if ( wpos_cmp( *from, *to ) > 0 ) {
        hold = *from;
        *from = *to;
        *to = hold;
    }
}

void we_set_chp( WDOC *doc, STORY *story, WPOS from, WPOS to, u32 mask, const CHP *value,
                 u16 flag_mask )
{
    WPARA *para;
    CHP chp;
    u16 *attr;
    u16 was, now;
    u32 pi, start, len, index;

    if ( story->count == 0 ) {
        return;
    }
    order( story, &from, &to );
    for ( pi = from.para; pi <= to.para; pi++ ) {
        if ( !span_of( story, pi, from, to, &start, &len ) ) {
            continue;
        }
        para = story->para[pi];
        attr = (u16 *)try_alloc( len * sizeof( u16 ) );
        if ( attr == NULL ) {
            doc->failed = 1;
            return;
        }
        was = now = 0;
        for ( index = 0; index < len; index++ ) {
            /* one CHP is looked up once for the run that has it */
            if ( index == 0 || para->attr[start + index] != was ) {
                was = para->attr[start + index];
                chp = *wd_chp_get( doc, was );
                chp_apply( &chp, mask, value, flag_mask );
                now = wd_chp( doc, &chp );
            }
            attr[index] = now;
        }
        index = (u32)p_attr( doc, story, pi, start, len, attr );
        xfree( attr );
        if ( !index ) {
            return;
        }
    }
}

int we_all_have( WDOC *doc, STORY *story, WPOS from, WPOS to, u16 flags )
{
    WPARA *para;
    u32 pi, start, len, index, seen = 0;

    if ( story->count == 0 ) {
        return 0;
    }
    order( story, &from, &to );
    for ( pi = from.para; pi <= to.para; pi++ ) {
        if ( !span_of( story, pi, from, to, &start, &len ) ) {
            continue;
        }
        para = story->para[pi];
        for ( index = 0; index < len; index++ ) {
            if ( (wd_chp_get( doc, para->attr[start + index] )->flags & flags) != flags ) {
                return 0;
            }
            seen = 1;
        }
    }
    return (int)seen;
}

void we_set_pap( WDOC *doc, STORY *story, u32 first, u32 last, u32 mask, const PAP *value,
                 u16 flag_mask )
{
    PAP pap;
    u32 pi;
    int done;

    if ( story->count == 0 ) {
        return;
    }
    if ( last >= story->count ) {
        last = story->count - 1;
    }
    /* where a table's cells end is not something a command sets */
    flag_mask &= (u16)~(PF_INTABLE | PF_CELLEND | PF_ROWEND);
    for ( pi = first; pi <= last; pi++ ) {
        if ( !pap_copy( &pap, &story->para[pi]->pap ) ) {
            doc->failed = 1;
            return;
        }
        if ( mask & PM_ALIGN ) {
            pap.align = value->align;
        }
        if ( mask & PM_LEFT ) {
            pap.left = value->left;
        }
        if ( mask & PM_RIGHT ) {
            pap.right = value->right;
        }
        if ( mask & PM_FIRST ) {
            pap.first = value->first;
        }
        if ( mask & PM_BEFORE ) {
            pap.before = value->before;
        }
        if ( mask & PM_AFTER ) {
            pap.after = value->after;
        }
        if ( mask & PM_LINE ) {
            pap.line = value->line;
            pap.line_rule = value->line_rule;
        }
        if ( mask & PM_LIST ) {
            pap.list = value->list;
            pap.level = value->level;
            pap.list_start = value->list_start;
        }
        if ( mask & PM_STYLE ) {
            pap.style = value->style;
        }
        if ( mask & PM_FLAGS ) {
            pap.flags = (u16)((pap.flags & ~flag_mask) | (value->flags & flag_mask));
        }
        done = 1;
        if ( mask & PM_TABS ) {
            done = pap_set_tabs( &pap, value->tabs, value->ntabs );
        }
        if ( done ) {
            done = p_pap( doc, story, pi, &pap );
        } else {
            doc->failed = 1;
        }
        pap_free( &pap );
        if ( !done ) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* a piece of one document, in another                                 */
/* ------------------------------------------------------------------ */

static void copy_story( WDOC *dst, STORY *dst_story, WDOC *src, STORY *src_story, int depth );

/* a CHP of one document as the same look in another */
static u16 map_chp( WDOC *dst, WDOC *src, u16 id )
{
    CHP chp = *wd_chp_get( src, id );
    const WFONT *font = &src->font[chp.font < src->nfont ? chp.font : 0];

    if ( dst == src ) {
        return id;
    }
    chp.font = (u16)wd_font( dst, font->name, font->family, font->fixed );
    return wd_chp( dst, &chp );
}

static u8 map_style( WDOC *dst, WDOC *src, u8 id )
{
    const WSTYLE *from;
    WSTYLE *made;
    u32 had = dst->nstyle;
    int found;

    if ( dst == src || id == 0 || id >= src->nstyle ) {
        return dst == src ? id : 0;
    }
    from = &src->style[id];
    found = wd_style( dst, from->name, from->kind );
    if ( (u32)found >= had && found != 0 ) {
        /* new here: it brings its look with it */
        made = &dst->style[found];
        made->chp = *wd_chp_get( dst, map_chp( dst, src, wd_chp( src, &from->chp ) ) );
        made->pap = from->pap;
        made->pap.tabs = NULL;
        made->pap.row = NULL;
        made->pap.ntabs = 0;
        made->pap.style = 0;
    }
    return (u8)found;
}

/* an object of one document made again in another: its number, or -1 */
static int map_obj( WDOC *dst, WDOC *src, u32 which, int depth )
{
    const WOBJ *obj;
    STORY *note;
    int made;

    if ( which >= src->nobj ) {
        return -1;
    }
    obj = &src->obj[which];
    if ( obj->kind == OBJ_IMAGE ) {
        return wd_image_new( dst, obj->data, obj->len, obj->fmt, obj->width, obj->height );
    }
    if ( obj->kind == OBJ_NOTE && depth < 1 && obj->story != NULL ) {
        /* what is wanted of it is read first: a new object can move the
           list "obj" points into, when both documents are the same one */
        note = obj->story;
        made = wd_note_new( dst, obj->fmt );
        if ( made >= 0 ) {
            copy_story( dst, dst->obj[made].story, src, note, depth + 1 );
        }
        return made;
    }
    return -1;
}

/*
 * A stretch of a paragraph ready to go into another document: the
 * CHP numbers that document has for its looks, and its objects made
 * there.  -> 0 if there was no memory.  The caller frees both arrays.
 */
static int map_text( WDOC *dst, WDOC *src, const WPARA *para, u32 start, u32 len, u16 **attr_out,
                     WANCH **anch_out, u32 *nanch_out, int depth )
{
    u16 *attr = NULL;
    WANCH *anch = NULL;
    u16 was = 0, now = 0;
    u32 index, count, made = 0;
    int obj;

    *attr_out = NULL;
    *anch_out = NULL;
    *nanch_out = 0;
    if ( len == 0 ) {
        return 1;
    }
    count = anch_count( para, start, len );
    attr = (u16 *)try_alloc( len * sizeof( u16 ) );
    if ( count ) {
        anch = (WANCH *)try_alloc( count * sizeof( WANCH ) );
    }
    if ( attr == NULL || (count && anch == NULL) ) {
        xfree( attr );
        xfree( anch );
        return 0;
    }
    for ( index = 0; index < len; index++ ) {
        if ( index == 0 || para->attr[start + index] != was ) {
            was = para->attr[start + index];
            now = map_chp( dst, src, was );
        }
        attr[index] = now;
        if ( para->text[start + index] == WCH_OBJ && made < count ) {
            /* a copy of the picture or note, the other document's own */
            obj = wd_obj_at( para, start + index );
            obj = obj < 0 ? -1 : map_obj( dst, src, (u32)obj, depth );
            if ( obj >= 0 ) {
                anch[made].off = index;
                anch[made].obj = (u32)obj;
                made++;
            }
        }
    }
    *attr_out = attr;
    *anch_out = anch;
    *nanch_out = made;
    return 1;
}

static void map_pap( WDOC *dst, WDOC *src, PAP *pap )
{
    pap->style = map_style( dst, src, pap->style );
}

static void copy_story( WDOC *dst, STORY *dst_story, WDOC *src, STORY *src_story, int depth )
{
    WPARA *from, *made;
    u16 *attr;
    WANCH *anch;
    u32 pi, nanch;

    for ( pi = 0; pi < src_story->count && !dst->failed; pi++ ) {
        from = src_story->para[pi];
        made = wd_para_add( dst, dst_story, &from->pap );
        if ( made == NULL ) {
            return;
        }
        map_pap( dst, src, &made->pap );
        if ( !map_text( dst, src, from, 0, from->len, &attr, &anch, &nanch, depth ) ) {
            dst->failed = 1;
            return;
        }
        wd_text_restore( dst, made, 0, from->text, attr, from->len, anch, nanch );
        xfree( attr );
        xfree( anch );
    }
}

WDOC *we_copy( WDOC *doc, STORY *story, WPOS from, WPOS to )
{
    WDOC *piece;
    WPARA *para, *made;
    u16 *attr;
    WANCH *anch;
    u32 pi, start, len, nanch;

    if ( story->count == 0 ) {
        return NULL;
    }
    order( story, &from, &to );
    piece = wd_new();
    if ( piece == NULL ) {
        return NULL;
    }
    piece->page = doc->page;
    piece->deftab = doc->deftab;
    for ( pi = from.para; pi <= to.para && !piece->failed; pi++ ) {
        para = story->para[pi];
        span_of( story, pi, from, to, &start, &len );
        made = wd_para_add( piece, &piece->body, &para->pap );
        if ( made == NULL ) {
            break;
        }
        map_pap( piece, doc, &made->pap );
        if ( pi == to.para ) {
            /* the last is text without its paragraph's end: no cell ends in it */
            made->pap.flags &= (u16)~CELL_MARKS;
            xfree( made->pap.row );
            made->pap.row = NULL;
        }
        if ( !map_text( piece, doc, para, start, len, &attr, &anch, &nanch, 0 ) ) {
            piece->failed = 1;
            break;
        }
        wd_text_restore( piece, made, 0, para->text + start, attr, len, anch, nanch );
        xfree( attr );
        xfree( anch );
    }
    if ( piece->failed ) {
        wd_free( piece );
        return NULL;
    }
    return piece;
}

void we_paste( WDOC *doc, STORY *story, WPOS *pos, WDOC *piece )
{
    WPARA *from;
    PAP pap;
    u16 *attr;
    WANCH *anch;
    u32 pi, nanch;
    int done;

    if ( story->count == 0 || piece == NULL ) {
        return;
    }
    *pos = wpos_clamp( story, *pos );
    for ( pi = 0; pi < piece->body.count; pi++ ) {
        from = piece->body.para[pi];
        if ( pi > 0 ) {
            /* the paragraph before ended in the piece: it ends here too,
               and with the properties it had there */
            if ( !p_split( doc, story, pos->para, pos->off, NULL ) ) {
                return;
            }
            if ( !pap_copy( &pap, &piece->body.para[pi - 1]->pap ) ) {
                doc->failed = 1;
                return;
            }
            map_pap( doc, piece, &pap );
            /* the cell it is pasted into keeps its own end */
            if ( story->para[pos->para + 1]->pap.flags & PF_INTABLE ) {
                pap.flags &= (u16)~CELL_MARKS;
                pap.flags |= PF_INTABLE;
                xfree( pap.row );
                pap.row = NULL;
            }
            done = p_pap( doc, story, pos->para, &pap );
            pap_free( &pap );
            if ( !done ) {
                return;
            }
            pos->para++;
            pos->off = 0;
        }
        if ( from->len == 0 ) {
            continue;
        }
        if ( !map_text( doc, piece, from, 0, from->len, &attr, &anch, &nanch, 0 ) ) {
            doc->failed = 1;
            return;
        }
        done = p_insert( doc, story, pos->para, pos->off, from->text, attr, 0, from->len, anch, nanch );
        xfree( attr );
        xfree( anch );
        if ( !done ) {
            return;
        }
        pos->off += from->len;
    }
}

/* ------------------------------------------------------------------ */
/* looking for text                                                    */
/* ------------------------------------------------------------------ */

/* a letter as its small one: ASCII, Latin-1, Greek and Cyrillic, which
   is what the code pages PMWRITE reads have letters from */
static u32 fold( u32 code )
{
    if ( code >= 'A' && code <= 'Z' ) {
        return code + 0x20;
    }
    if ( (code >= 0xC0 && code <= 0xDE && code != 0xD7) || (code >= 0x391 && code <= 0x3A9) ||
         (code >= 0x410 && code <= 0x42F) ) {
        return code + 0x20;
    }
    if ( code >= 0x400 && code <= 0x40F ) {
        return code + 0x50;
    }
    if ( code >= 0x100 && code <= 0x17F && code != 0x130 && code != 0x131 && code != 0x138 &&
         code != 0x149 && code != 0x178 && code != 0x17F ) {
        /* Latin Extended-A goes capital, small, capital, small - with
           the pairs the other way round in two stretches of it */
        if ( (code >= 0x139 && code <= 0x148) || (code >= 0x179 && code <= 0x17E) ) {
            return (code & 1) ? code + 1 : code;
        }
        return (code & 1) ? code : code + 1;
    }
    return code;
}

static int is_word( u32 code )
{
    return (code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') ||
           (code >= 'a' && code <= 'z') || code == '_' || (code >= 0xC0 && code != 0xD7 && code != 0xF7 &&
           code < 0x2000);
}

static int match_at( const WPARA *para, u32 off, const u16 *what, u32 len, int flags )
{
    u32 index;

    if ( len > para->len - off ) {
        return 0;
    }
    for ( index = 0; index < len; index++ ) {
        if ( (flags & WF_CASE) ? para->text[off + index] != what[index]
                               : fold( para->text[off + index] ) != fold( what[index] ) ) {
            return 0;
        }
    }
    if ( flags & WF_WORD ) {
        if ( off > 0 && is_word( para->text[off - 1] ) ) {
            return 0;
        }
        if ( off + len < para->len && is_word( para->text[off + len] ) ) {
            return 0;
        }
    }
    return 1;
}

int we_find( WDOC *doc, STORY *story, WPOS from, const u16 *what, u32 len, int flags, WPOS *at )
{
    const WPARA *para;
    u32 pi, off;

    (void)doc;
    if ( story->count == 0 || len == 0 ) {
        return 0;
    }
    from = wpos_clamp( story, from );
    if ( !(flags & WF_BACK) ) {
        for ( pi = from.para; pi < story->count; pi++ ) {
            para = story->para[pi];
            for ( off = pi == from.para ? from.off : 0; off < para->len; off++ ) {
                if ( match_at( para, off, what, len, flags ) ) {
                    at->para = pi;
                    at->off = off;
                    return 1;
                }
            }
        }
        return 0;
    }
    /* backwards: a match that ends at or before the place */
    pi = from.para;
    for ( ;; ) {
        para = story->para[pi];
        off = pi == from.para ? from.off : para->len;
        while ( off >= len ) {
            if ( match_at( para, off - len, what, len, flags ) ) {
                at->para = pi;
                at->off = off - len;
                return 1;
            }
            off--;
        }
        if ( pi == 0 ) {
            return 0;
        }
        pi--;
    }
}
