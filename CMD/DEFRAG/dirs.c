/*
 * DIRS.C - a directory put in order: its deleted entries squeezed out,
 * and, when a sort was asked for, its files sorted.
 *
 * A DIRECTORY IS ARRANGED IN MEMORY, and written by whoever asked: in
 * its new place when it is being moved (MOVE.C), where it is when it is
 * not.  It is arranged only at the moment it is about to be written
 * whole - until then the copy in memory is what is on the disk, and a
 * single entry of it can be changed and its sector written (which is
 * what moving a file does to the directory the file is in).
 *
 * A LONG NAME GOES WITH THE ENTRY IT IS IN FRONT OF.  The entries of a
 * long name and the 8.3 entry after them are one group, and a group is
 * moved whole.  Long name entries in front of a deleted entry, or of
 * nothing, belonged to something that is gone and go with it.  Nothing
 * here works out whether a long name is the right one for its entry:
 * what was next to what stays next to it.
 *
 * WHAT STAYS AT THE TOP: "." and "..", then the volume label and
 * anything that is never moved (the boot files have always been found
 * by some loader or other "first in the root"), in the order they were
 * in.  Everything else follows - directories among the files, as in
 * MS-DOS's DEFRAG - sorted by the 8.3 name, the extension, the date and
 * time or the size.  Equal keys are told apart by name, and equal names
 * by where they were.
 *
 * SQUEEZING ONLY MOVES ENTRIES TOWARDS THE FRONT, so a directory that
 * is squeezed and not sorted can be written where it lies, first sector
 * to last, and be whole at every moment: an entry is in the part
 * already written, or still in the part not yet, or for a moment in
 * both - never in neither.  Sorting moves entries both ways and has no
 * such order; OPTIM.C writes a sorted directory somewhere else and
 * changes over to it.
 */
#include "defrag.h"

typedef struct {
    u32 first;                  /* the first entry: of the long name, or
                                   the 8.3 entry itself */
    u32 count;                  /* entries: the long name's and the 8.3 one */
    u32 node;                   /* N_NONE: a volume label, a "." */
    u32 klass;                  /* 0 ".", 1 "..", 2 kept at the top, 3 the rest */
} GROUP;

ARRANGED arranged;

static GROUP *groups;
static u32    ngroups, group_cap;
static const u8 *sort_data;     /* the directory being sorted */
static int    sort_by;

static void group_add( u32 first, u32 count, u32 node, u32 klass )
{
    GROUP *grown;

    if ( ngroups == group_cap ) {
        group_cap = group_cap ? group_cap * 2 : 256;
        grown = (GROUP *)xalloc( group_cap * sizeof( GROUP ) );
        if ( ngroups ) {
            mem_cpy( grown, groups, ngroups * sizeof( GROUP ) );
        }
        xfree( groups );
        groups = grown;
    }
    groups[ngroups].first = first;
    groups[ngroups].count = count;
    groups[ngroups].node = node;
    groups[ngroups].klass = klass;
    ngroups++;
}

/* the groups of a directory, in the order they lie; -> the entries in
   use before the end mark, deleted ones among them */
static u32 groups_of( u32 dirno )
{
    const DIRINFO *di = &dirs[dirno];
    const u8 *ent;
    u32 idx, node, start = 0, klass;
    int pending = 0;

    ngroups = 0;
    for ( idx = 0; idx < di->buf.nent; idx++ ) {
        ent = di->buf.data + idx * 32;
        if ( ent[0] == 0 ) {
            break;
        }
        if ( ent[0] == DEL_MARK ) {
            pending = 0;
            continue;
        }
        if ( IS_LFN( ent ) ) {
            if ( !pending ) {
                pending = 1;
                start = idx;
            }
            continue;
        }
        node = di->who[idx];
        if ( dirno != 0 && idx < 2 && ent[0] == '.' ) {
            klass = idx;
        } else if ( (ent[DE_ATTR] & A_VOLID) || node == N_NONE || (nodes[node].flags & NF_FIXED) ) {
            klass = 2;
        } else {
            klass = 3;
        }
        group_add( pending ? start : idx, pending ? idx - start + 1 : 1, node, klass );
        pending = 0;
    }
    return idx;
}

/* the 8.3 entry of a group */
static const u8 *group_ent( const GROUP *grp )
{
    return sort_data + (grp->first + grp->count - 1) * 32;
}

static int by_name( const u8 *lhs, const u8 *rhs )
{
    int diff = mem_cmp( lhs, rhs, 8 );

    return diff ? diff : mem_cmp( lhs + 8, rhs + 8, 3 );
}

static int key_cmp( u32 lhs, u32 rhs )
{
    return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
}

/* < 0: group "one" comes before group "two" */
static int group_cmp( u32 one, u32 two )
{
    const GROUP *lhs = &groups[one], *rhs = &groups[two];
    const u8 *lent, *rent;
    int diff = 0;

    if ( lhs->klass != rhs->klass ) {
        return lhs->klass < rhs->klass ? -1 : 1;
    }
    if ( lhs->klass == 3 ) {
        lent = group_ent( lhs );
        rent = group_ent( rhs );
        switch ( sort_by ) {
        case SORT_NAME:
            diff = by_name( lent, rent );
            break;
        case SORT_EXT:
            diff = mem_cmp( lent + 8, rent + 8, 3 );
            break;
        case SORT_DATE:
            diff = key_cmp( RD16( lent + DE_DATE ), RD16( rent + DE_DATE ) );
            if ( diff == 0 ) {
                diff = key_cmp( RD16( lent + DE_TIME ), RD16( rent + DE_TIME ) );
            }
            break;
        case SORT_SIZE:
            diff = key_cmp( RD32( lent + DE_SIZE ), RD32( rent + DE_SIZE ) );
            break;
        }
        if ( opt.descending ) {
            diff = -diff;
        }
        if ( diff == 0 ) {
            diff = by_name( lent, rent );
        }
    }
    return diff ? diff : key_cmp( one, two );
}

/* the groups' numbers in the order they are to be in: as they lie, or
   sorted - a merge sort, so that what compares equal keeps its order */
static u32 *order_of( u32 dirno, int sort )
{
    u32 *order, *spare, *swap, width, lo, mid, hi, left, right, out, index;

    order = (u32 *)xalloc( (ngroups ? ngroups : 1) * sizeof( u32 ) );
    for ( index = 0; index < ngroups; index++ ) {
        order[index] = index;
    }
    if ( sort == SORT_NONE || ngroups < 2 ) {
        return order;
    }
    sort_data = dirs[dirno].buf.data;
    sort_by = sort;
    spare = (u32 *)xalloc( ngroups * sizeof( u32 ) );
    for ( width = 1; width < ngroups; width *= 2 ) {
        for ( lo = 0; lo < ngroups; lo += 2 * width ) {
            mid = lo + width < ngroups ? lo + width : ngroups;
            hi = lo + 2 * width < ngroups ? lo + 2 * width : ngroups;
            left = lo;
            right = mid;
            for ( out = lo; out < hi; out++ ) {
                if ( left < mid && (right >= hi || group_cmp( order[left], order[right] ) <= 0) ) {
                    spare[out] = order[left++];
                } else {
                    spare[out] = order[right++];
                }
            }
        }
        swap = order;
        order = spare;
        spare = swap;
    }
    xfree( spare );
    return order;
}

/* The files and directories of a directory in the order arranging it
   would put them, without arranging it: where things are to go is
   worked out before anything is written. */
void dir_children( u32 dirno, int sort, U32LIST *out )
{
    u32 *order, index;

    groups_of( dirno );
    order = order_of( dirno, sort );
    for ( index = 0; index < ngroups; index++ ) {
        if ( groups[order[index]].node != N_NONE ) {
            list_add( out, groups[order[index]].node );
        }
    }
    xfree( order );
}

/* Arrange the copy in memory - or, with "apply" 0, only say whether
   arranging it would change anything.  1 = it is, or would be, not what
   is on the disk any more. */
static int arrange( u32 dirno, int sort, int apply )
{
    DIRINFO *di = &dirs[dirno];
    u32 bytes = di->buf.nent * 32, end, at = 0, index, *order, *who;
    const GROUP *grp;
    u8 *image;
    int changed;

    end = groups_of( dirno );
    order = order_of( dirno, sort );
    image = (u8 *)xzalloc( bytes ? bytes : 32 );
    who = (u32 *)xzalloc( (di->buf.nent ? di->buf.nent : 1) * sizeof( u32 ) );
    for ( index = 0; index < ngroups; index++ ) {
        grp = &groups[order[index]];
        mem_cpy( image + at * 32, di->buf.data + grp->first * 32, grp->count * 32 );
        who[at + grp->count - 1] = grp->node;
        at += grp->count;
    }
    xfree( order );
    /* what lies past the end mark was nobody's and is not compared */
    changed = at != end || mem_cmp( image, di->buf.data, end * 32 ) != 0;
    if ( !changed || !apply ) {
        xfree( image );
        xfree( who );
        return changed;
    }
    /* the directory as it was is kept until the next one is arranged:
       whoever writes this one where it lies needs both (MOVE.C) */
    xfree( arranged.old );
    arranged.old = di->buf.data;
    arranged.dirno = dirno;
    arranged.sorted = sort != SORT_NONE;
    arranged.old_end = end;
    arranged.new_end = at;
    di->buf.data = image;
    xfree( di->who );
    di->who = who;
    for ( index = 0; index < di->buf.nent; index++ ) {
        if ( who[index] != N_NONE ) {
            nodes[who[index]].ent = index;
        }
    }
    di->dead = 0;
    return 1;
}

int dir_arrange( u32 dirno, int sort )
{
    return arrange( dirno, sort, 1 );
}

int dir_check( u32 dirno, int sort )
{
    return arrange( dirno, sort, 0 );
}

/* the drive is being let go: nothing is remembered of its directories */
void dir_forget( void )
{
    xfree( arranged.old );
    mem_set( &arranged, 0, sizeof( arranged ) );
    arranged.dirno = 0xFFFFFFFFul;
}
