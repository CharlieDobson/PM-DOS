/*
 * OPTIM.C - where everything on a drive is to go, and the going there.
 *
 * FULL OPTIMIZATION lays the drive out from its first cluster: every
 * directory, one after another, and then every file, directory by
 * directory in the order the directories list them - each in one piece,
 * with nothing between one and the next.  What cannot be moved (the
 * map's X) and clusters marked bad stay where they are and the layout
 * goes round them: a gap in front of one that is too small for the next
 * file is given to the largest file still to come that fits it.  Then
 * each thing is put in its place, lowest first.  Whatever is in the way
 * is moved out - past the end of the layout when there is room there,
 * so that it is not in the way again - and anything already in place is
 * never touched again.
 *
 * UNFRAGMENT FILES ONLY leaves a file that is in one piece where it
 * is.  One that is not is given the lowest stretch of the drive that it
 * fits into, counting its own clusters as room; largest files first,
 * because they are the hardest to find room for.  The free space is
 * left in whatever holes that makes.
 *
 * A FILE IS PUT IN PLACE A PIECE AT A TIME (place_file): a piece is as
 * many of its next clusters as one move carries and as there are free
 * clusters for at the place they are going.  When that place is taken,
 * what is there is moved away first.  When what is there is the file's
 * own later clusters - a file being moved a little way down the drive
 * lies across its own destination - they are moved away like anyone
 * else's if the free stretch in front of them is short, and left if it
 * is long enough to be worth sliding into.  With one free cluster on
 * the drive and nothing else to work with this still finishes, a
 * cluster at a time.
 *
 * A DIRECTORY IS PUT IN PLACE WHOLE (place_dir).  Its place is cleared
 * first, then the directory is arranged in memory (DIRS.C) and written
 * there, and changed over to (MOVE.C: move_dir).  A directory that is
 * already in its place and only needs its entries squeezed is written
 * where it is, which squeezing allows; one that has to be sorted is
 * written somewhere else, changed over to, and moved back.
 *
 * NOTHING HERE WRITES TO THE DISK.  MOVE.C does, a move at a time, and
 * between any two moves the drive is one a user could be handed.  That
 * is where a request to stop is taken.
 */
#include "defrag.h"

static U32LIST plan;            /* what has a place to go to, lowest first */
static U32LIST rest;            /* what could not be given one */
static u32  boundary = 2;       /* the first cluster past the layout */
static u32  hint;               /* where the last look for free space ended */
static u32  work_total, work_done;
static u32 *tmp_src;

static int is_free( u32 clus )
{
    return fat_get( clus ) == 0;
}

/* a cluster the layout may use: not bad, and not under something that
   is never moved */
static int usable( u32 clus )
{
    return fat_get( clus ) != vol.bad_val && !(nodes[own[clus]].flags & NF_FIXED);
}

static int in_place( u32 node, u32 target )
{
    u32 at, clus = nodes[node].first;

    for ( at = 0; at < nodes[node].nclus; at++, clus = fat_get( clus ) ) {
        if ( clus != target + at ) {
            return 0;
        }
    }
    return 1;
}

static int in_pieces( u32 node )
{
    u32 at, clus = nodes[node].first;

    for ( at = 1; at < nodes[node].nclus; at++ ) {
        if ( fat_get( clus ) != clus + 1 ) {
            return 1;
        }
        clus++;
    }
    return 0;
}

static void worked( u32 clusters )
{
    work_done += clusters;
    if ( work_done > work_total ) {
        work_done = work_total;             /* a second try at something */
    }
    fe_progress( work_done, work_total );
}

/* it is where it is going to stay */
static int placed( u32 node )
{
    u32 at, clus = nodes[node].first;

    if ( !(nodes[node].flags & NF_PLACED) ) {
        nodes[node].flags |= NF_PLACED;
        for ( at = 0; at < nodes[node].nclus; at++, clus = fat_get( clus ) ) {
            map_count( clus, 0, 0, 1 );
        }
    }
    nodes[node].flags &= ~NF_FRAG;
    worked( nodes[node].nclus );
    return 0;
}

/* which sort a directory gets: none in a run that only unfragments, and
   none for a directory that is itself never moved */
static int sort_of( u32 dirno )
{
    (void)dirno;
    if ( opt.method != METHOD_FULL ) {
        return SORT_NONE;
    }
    return opt.sort;
}

static int left_alone( u32 dirno )
{
    return dirno != 0 && (nodes[dirs[dirno].node].flags & NF_FIXED);
}

/* ------------------------------------------------------------------ */
/* free space                                                          */
/* ------------------------------------------------------------------ */

/*
 * A run of free clusters outside [lo, hi): "want" of them, or - unless
 * "whole" - as many as the first run found has.  Past the layout is
 * looked at first, carrying on from where the last look ended; then the
 * layout's own far end, and last the holes below.
 */
static u32 find_free( u32 want, u32 lo, u32 hi, int whole, u32 *len )
{
    u32 from = boundary > hi ? boundary : hi, begin[4], finish[4], clus, run, last;
    int pass;

    if ( hint < from || hint > vol.maxclus ) {
        hint = from;
    }
    begin[0] = hint;    finish[0] = vol.maxclus + 1;
    begin[1] = from;    finish[1] = hint;
    begin[2] = hi;      finish[2] = from;
    begin[3] = 2;       finish[3] = lo;
    for ( pass = 0; pass < 4; pass++ ) {
        last = finish[pass];
        for ( clus = begin[pass]; clus < last; clus++ ) {
            if ( !is_free( clus ) ) {
                continue;
            }
            for ( run = 1; run < want && clus + run < last && is_free( clus + run ); run++ ) {
            }
            if ( whole && run < want ) {
                clus += run;
                continue;
            }
            *len = run;
            if ( pass < 2 ) {
                hint = clus + run;
            }
            return clus;
        }
    }
    return 0;
}

/* how many of the clusters from "clus" on are free, up to "most" */
static u32 free_lead( u32 clus, u32 most )
{
    u32 lead;

    for ( lead = 0; lead < most && is_free( clus + lead ); lead++ ) {
    }
    return lead;
}

/* ------------------------------------------------------------------ */
/* out of the way                                                      */
/* ------------------------------------------------------------------ */

/* "count" clusters of "node" - src[], following "before" in its chain -
   into free space outside [lo, hi): as many of them as one free run
   takes.  *moved = 0 when there is nowhere. */
static int evict( u32 node, u32 before, const u32 *src, u32 count, u32 lo, u32 hi, u32 *moved,
                  u32 *where )
{
    u32 dst, len;
    int rc;

    *moved = 0;
    if ( node == N_NONE || (nodes[node].flags & NF_FIXED) ) {
        return DFE_INTERNAL;
    }
    dst = find_free( count, lo, hi, 0, &len );
    if ( dst == 0 ) {
        return 0;
    }
    rc = move_run( node, before, src, len, dst );
    if ( rc == 0 ) {
        *moved = len;
        *where = dst;
    }
    return rc;
}

/* whatever lies at "clus", which is somebody else's: the clusters from
   there on that are the same owner's and follow one another in its
   chain as they do on the drive, no further than "end" */
static int evict_at( u32 clus, u32 end, u32 lo, u32 hi, u32 *moved )
{
    u32 node = own[clus], limit = move_limit(), run, index, where;

    for ( run = 1; run < limit && clus + run < end && own[clus + run] == node
                   && prev[clus + run] == clus + run - 1; run++ ) {
    }
    for ( index = 0; index < run; index++ ) {
        tmp_src[index] = clus + index;
    }
    return evict( node, prev[clus], tmp_src, run, lo, hi, moved, &where );
}

/* every cluster of [lo, lo+count) made free.  DFE_NOFREE when there is
   nowhere to put what is there. */
static int region_clear( u32 lo, u32 count )
{
    u32 end = lo + count, cur = lo, moved;
    int rc;

    while ( cur < end ) {
        if ( is_free( cur ) ) {
            cur++;
            continue;
        }
        rc = evict_at( cur, end, lo, end, &moved );
        if ( rc != 0 ) {
            return rc;
        }
        if ( moved == 0 ) {
            return DFE_NOFREE;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* a file into its place                                               */
/* ------------------------------------------------------------------ */

/*
 * While a file is being placed its clusters carry their NUMBER IN THE
 * CHAIN in prev[], not the cluster before them: one of them met in the
 * way has to be known for which it is, and pos[] turns a number back
 * into a cluster.  renumber() puts that right for clusters a move has
 * just written its own idea of prev[] into; place_file() puts the links
 * back when it is done.
 */
static void renumber( u32 *pos, u32 count, u32 idx, u32 moved, u32 where )
{
    u32 index;

    for ( index = 0; index < moved; index++ ) {
        pos[idx + index] = where + index;
        prev[where + index] = idx + index;
    }
    if ( idx + moved < count ) {
        prev[pos[idx + moved]] = idx + moved;
    }
}

/* the file's own clusters number idx to idx+run-1 out of the way */
static int evict_own( u32 node, u32 *pos, u32 count, u32 idx, u32 run, u32 lo, u32 hi, u32 *moved )
{
    u32 where = 0;
    int rc;

    rc = evict( node, idx ? pos[idx - 1] : 0, pos + idx, run, lo, hi, moved, &where );
    if ( rc == 0 && *moved ) {
        renumber( pos, count, idx, *moved, where );
    }
    return rc;
}

/* as much of [want, want+most) made free as there is room elsewhere for */
static int make_room( u32 node, u32 *pos, u32 count, u32 target, u32 at, u32 most )
{
    u32 want = target + at, stop = want + most, end = target + count, limit = move_limit();
    u32 cur = want, idx, run, moved;
    int rc;

    while ( cur < stop ) {
        if ( is_free( cur ) ) {
            cur++;
            continue;
        }
        if ( own[cur] == node ) {
            idx = prev[cur];
            if ( idx == cur - target ) {
                break;                      /* where it belongs: no further */
            }
            for ( run = 1; run < limit && cur + run < stop && own[cur + run] == node
                           && prev[cur + run] == idx + run; run++ ) {
            }
            rc = evict_own( node, pos, count, idx, run, target, end, &moved );
        } else {
            rc = evict_at( cur, stop, target, end, &moved );
        }
        if ( rc != 0 ) {
            return rc;
        }
        if ( moved == 0 ) {
            break;
        }
    }
    return 0;
}

/* Nothing is free outside the file's place at all: the one cluster that
   is wanted is cleared into any free cluster there is. */
static int last_resort( u32 node, u32 *pos, u32 count, u32 want )
{
    u32 dst, len, idx;
    int rc;

    dst = find_free( 1, want, want + 1, 0, &len );
    if ( dst == 0 ) {
        return DFE_NOFREE;
    }
    if ( own[want] == node ) {
        idx = prev[want];
        rc = move_run( node, idx ? pos[idx - 1] : 0, pos + idx, 1, dst );
        if ( rc == 0 ) {
            renumber( pos, count, idx, 1, dst );
        }
        return rc;
    }
    if ( own[want] == N_NONE || (nodes[own[want]].flags & NF_FIXED) ) {
        return DFE_INTERNAL;
    }
    tmp_src[0] = want;
    return move_run( own[want], prev[want], tmp_src, 1, dst );
}

static int place_file( u32 node, u32 target )
{
    u32 count = nodes[node].nclus, limit = move_limit(), slide = (limit + 1) / 2;
    u32 *pos, at, clus, want, most, room, cur, index;
    int rc = 0;

    if ( in_place( node, target ) ) {
        return placed( node );
    }
    pos = (u32 *)xalloc( count * sizeof( u32 ) );
    for ( at = 0, clus = nodes[node].first; at < count; at++, clus = fat_get( clus ) ) {
        pos[at] = clus;
    }
    for ( at = 0; at < count; at++ ) {
        prev[pos[at]] = at;
    }

    at = 0;
    while ( at < count ) {
        want = target + at;
        if ( pos[at] == want ) {
            at++;
            continue;
        }
        if ( fe_stop_asked() ) {
            rc = DFE_USER;
            break;
        }
        most = count - at < limit ? count - at : limit;
        room = free_lead( want, most );
        if ( room < most ) {
            cur = want + room;              /* the first thing in the way */
            if ( own[cur] == node && prev[cur] == cur - target ) {
                /* its own, and where it belongs: the piece ends before it */
            } else if ( own[cur] == node && room >= slide ) {
                /* its own later clusters, and a fair step towards them:
                   the file is sliding down the drive */
            } else {
                rc = make_room( node, pos, count, target, at, most );
                if ( rc != 0 ) {
                    break;
                }
                room = free_lead( want, most );
                if ( room == 0 ) {
                    rc = last_resort( node, pos, count, want );
                    if ( rc != 0 ) {
                        break;
                    }
                    room = free_lead( want, 1 );
                }
            }
        }
        if ( room == 0 ) {
            rc = DFE_INTERNAL;
            break;
        }
        rc = move_run( node, at ? want - 1 : 0, pos + at, room, want );
        if ( rc != 0 ) {
            break;
        }
        renumber( pos, count, at, room, want );
        at += room;
    }

    for ( index = 0; index < count; index++ ) {
        prev[pos[index]] = index ? pos[index - 1] : 0;
    }
    xfree( pos );
    return rc != 0 ? rc : placed( node );
}

/* ------------------------------------------------------------------ */
/* a directory into its place                                          */
/* ------------------------------------------------------------------ */

/* Put in order where it lies.  This is what is left when there is not
   the room to write a directory whole somewhere else: its entries are
   arranged on the spot, and after that its clusters can be moved a
   piece at a time like a file's, because what is in memory is what is
   on the disk again. */
static int settle( u32 dirno )
{
    return dir_arrange( dirno, sort_of( dirno ) ) ? dir_rewrite( dirno ) : 0;
}

static int place_dir( u32 dirno, u32 target )
{
    u32 node = dirs[dirno].node, count = nodes[node].nclus, end = target + count;
    u32 at, clus, dst, len;
    int rc, sort = sort_of( dirno ), inside = 0;

    if ( in_place( node, target ) ) {
        if ( !dir_arrange( dirno, sort ) ) {
            return placed( node );
        }
        /* squeezed: it can be written where it is.  Sorted: the new one
           is written somewhere else and comes back, so that there is
           always a whole directory on the disk. */
        dst = sort == SORT_NONE ? 0 : find_free( count, target, end, 1, &len );
        if ( dst == 0 ) {
            rc = dir_rewrite( dirno );
            return rc != 0 ? rc : placed( node );
        }
        rc = move_dir( dirno, dst );
        if ( rc == 0 ) {
            rc = move_dir( dirno, target );
        }
        return rc != 0 ? rc : placed( node );
    }

    /* clusters of its own in the place it is going to: all of it is
       moved clear first, as it is on the disk */
    for ( at = 0, clus = nodes[node].first; at < count; at++, clus = fat_get( clus ) ) {
        if ( clus >= target && clus < end ) {
            inside = 1;
        }
    }
    if ( inside ) {
        dst = find_free( count, target, end, 1, &len );
        if ( dst == 0 ) {
            rc = settle( dirno );
            return rc != 0 ? rc : place_file( node, target );
        }
        rc = move_dir( dirno, dst );
        if ( rc != 0 ) {
            return rc;
        }
    }
    rc = region_clear( target, count );
    if ( rc == DFE_NOFREE ) {
        rc = settle( dirno );
        return rc != 0 ? rc : place_file( node, target );
    }
    if ( rc != 0 ) {
        return rc;
    }
    dir_arrange( dirno, sort );
    rc = move_dir( dirno, target );
    return rc != 0 ? rc : placed( node );
}

/* ------------------------------------------------------------------ */
/* the layout of a full optimization                                   */
/* ------------------------------------------------------------------ */

static u32 *by_size;            /* seq's indexes, smallest first */
static u32  by_count;

static void assign( u32 node, u32 target )
{
    nodes[node].target = target;
    list_add( &plan, node );
}

/* the largest thing still without a place that is no more than "room"
   clusters, or N_NONE */
static u32 pick_fit( const U32LIST *seq, u32 room )
{
    u32 lo = 0, hi = by_count, mid;

    while ( lo < hi ) {                     /* the first that is too big */
        mid = (lo + hi) / 2;
        if ( nodes[seq->items[by_size[mid]]].nclus <= room ) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    while ( lo > 0 ) {
        lo--;
        if ( nodes[seq->items[by_size[lo]]].target == 0 ) {
            return seq->items[by_size[lo]];
        }
    }
    return N_NONE;
}

static void sort_by_size( const U32LIST *seq )
{
    u32 *spare, *swap, width, lo, mid, hi, left, right, out, index;

    by_count = seq->count;
    by_size = (u32 *)xalloc( (by_count ? by_count : 1) * sizeof( u32 ) );
    spare = (u32 *)xalloc( (by_count ? by_count : 1) * sizeof( u32 ) );
    for ( index = 0; index < by_count; index++ ) {
        by_size[index] = index;
    }
    for ( width = 1; width < by_count; width *= 2 ) {
        for ( lo = 0; lo < by_count; lo += 2 * width ) {
            mid = lo + width < by_count ? lo + width : by_count;
            hi = lo + 2 * width < by_count ? lo + 2 * width : by_count;
            left = lo;
            right = mid;
            for ( out = lo; out < hi; out++ ) {
                if ( left < mid && (right >= hi || nodes[seq->items[by_size[left]]].nclus
                                                   <= nodes[seq->items[by_size[right]]].nclus) ) {
                    spare[out] = by_size[left++];
                } else {
                    spare[out] = by_size[right++];
                }
            }
        }
        swap = by_size;
        by_size = spare;
        spare = swap;
    }
    xfree( spare );
}

static void plan_full( void )
{
    U32LIST seq, kids, walk, files;
    u32 at, dirno, index, node, cursor = 2, need, run, fill;

    mem_set( &seq, 0, sizeof( seq ) );
    mem_set( &kids, 0, sizeof( kids ) );
    mem_set( &walk, 0, sizeof( walk ) );
    mem_set( &files, 0, sizeof( files ) );
    plan.count = rest.count = 0;
    for ( node = 0; node < nnodes; node++ ) {
        nodes[node].target = 0;
    }
    /*
     * What is to lie where: the directories, then each one's files.
     * The directories are taken in the order their entries WILL be in,
     * a directory's own before the ones inside them - not in the order
     * they were read, which a sort is about to change.  Otherwise a
     * second run would lay the same drive out differently, and move
     * everything again.
     */
    list_add( &walk, 0 );
    for ( at = 0; at < walk.count; at++ ) {
        dirno = walk.items[at];
        kids.count = 0;
        dir_children( dirno, left_alone( dirno ) ? SORT_NONE : sort_of( dirno ), &kids );
        for ( index = 0; index < kids.count; index++ ) {
            node = kids.items[index];
            if ( nodes[node].flags & NF_DIR ) {
                if ( nodes[node].nclus ) {
                    list_add( &walk, nodes[node].dir );
                    if ( !(nodes[node].flags & NF_FIXED) ) {
                        list_add( &seq, node );
                    }
                }
            } else if ( !(nodes[node].flags & NF_FIXED) && nodes[node].nclus ) {
                list_add( &files, node );
            }
        }
    }
    for ( index = 0; index < files.count; index++ ) {
        list_add( &seq, files.items[index] );
    }
    list_free( &kids );
    list_free( &walk );
    list_free( &files );
    sort_by_size( &seq );

    for ( index = 0; index < seq.count; index++ ) {
        node = seq.items[index];
        if ( nodes[node].target != 0 ) {
            continue;                       /* it went into a gap earlier */
        }
        need = nodes[node].nclus;
        for ( ;; ) {
            while ( cursor <= vol.maxclus && !usable( cursor ) ) {
                cursor++;
            }
            if ( cursor > vol.maxclus ) {
                break;
            }
            for ( run = 1; run < need && cursor + run <= vol.maxclus && usable( cursor + run ); run++ ) {
            }
            if ( run >= need ) {
                assign( node, cursor );
                cursor += need;
                break;
            }
            /* too small for it: something later that does fit, so that
               the gap is not left */
            fill = pick_fit( &seq, run );
            if ( fill != N_NONE ) {
                assign( fill, cursor );
                cursor += nodes[fill].nclus;
            } else {
                cursor += run;
            }
        }
        if ( nodes[node].target == 0 ) {
            list_add( &rest, node );        /* the drive ran out */
        }
    }
    boundary = cursor;
    xfree( by_size );
    by_size = NULL;
    list_free( &seq );
}

/* ------------------------------------------------------------------ */
/* unfragmenting                                                       */
/* ------------------------------------------------------------------ */

/* the lowest "count" clusters in a row that are free or the node's own;
   0 = there are none */
static u32 region_for( u32 node, u32 count )
{
    u32 start = 2, clus;

    while ( start + count <= vol.maxclus + 1 ) {
        for ( clus = start + count; clus > start; clus-- ) {
            if ( !is_free( clus - 1 ) && own[clus - 1] != node ) {
                break;
            }
        }
        if ( clus == start ) {
            return start;
        }
        start = clus;                       /* past the cluster that is in the way */
    }
    return 0;
}

/* a file that is in pieces, into one; 0 as well when there is nowhere */
static int unfragment( u32 node )
{
    u32 target = region_for( node, nodes[node].nclus );

    if ( target == 0 ) {
        worked( nodes[node].nclus );
        return 0;
    }
    return place_file( node, target );
}

static int unfragment_dir( u32 dirno )
{
    u32 node = dirs[dirno].node, count = nodes[node].nclus, dst, len;
    int rc;

    if ( !in_pieces( node ) ) {
        return dir_arrange( dirno, SORT_NONE ) ? dir_rewrite( dirno ) : 0;
    }
    hint = 0;                               /* the lowest there is */
    dst = find_free( count, 2, 2, 1, &len );
    if ( dst == 0 ) {
        rc = settle( dirno );
        return rc != 0 ? rc : unfragment( node );
    }
    dir_arrange( dirno, SORT_NONE );
    rc = move_dir( dirno, dst );
    return rc != 0 ? rc : placed( node );
}

/* the fragmented files, largest first */
static void plan_unfrag( void )
{
    u32 node, index, hole, held;

    plan.count = rest.count = 0;
    boundary = 2;
    for ( node = N_ROOT + 1; node < nnodes; node++ ) {
        nodes[node].target = 0;
        if ( !(nodes[node].flags & (NF_DIR | NF_FIXED)) && in_pieces( node ) ) {
            list_add( &plan, node );
        }
    }
    for ( index = 1; index < plan.count; index++ ) {
        held = plan.items[index];
        for ( hole = index; hole > 0 && nodes[plan.items[hole - 1]].nclus < nodes[held].nclus; hole-- ) {
            plan.items[hole] = plan.items[hole - 1];
        }
        plan.items[hole] = held;
    }
}

/* ------------------------------------------------------------------ */
/* a run                                                               */
/* ------------------------------------------------------------------ */

/* 1 = the method that is set would change something on this drive */
int optimize_needed( void )
{
    u32 dirno, index;

    for ( dirno = 0; dirno < ndirs; dirno++ ) {
        if ( !left_alone( dirno ) && dir_check( dirno, sort_of( dirno ) ) ) {
            return 1;
        }
    }
    if ( opt.method != METHOD_FULL ) {
        return dst.frag_items != 0;
    }
    plan_full();
    for ( index = 0; index < plan.count; index++ ) {
        if ( !in_place( plan.items[index], nodes[plan.items[index]].target ) ) {
            return 1;
        }
    }
    for ( index = 0; index < rest.count; index++ ) {
        if ( in_pieces( rest.items[index] ) ) {
            return 1;
        }
    }
    return 0;
}

static int run_full( void )
{
    u32 index, node;
    int rc = 0;

    plan_full();
    work_total = work_done = 0;
    for ( index = 0; index < plan.count; index++ ) {
        work_total += nodes[plan.items[index]].nclus;
    }
    for ( index = 0; index < rest.count; index++ ) {
        work_total += nodes[rest.items[index]].nclus;
    }
    fe_progress( 0, work_total );

    if ( dir_arrange( 0, sort_of( 0 ) ) ) {
        rc = dir_rewrite( 0 );
    }
    for ( index = 0; index < plan.count && rc == 0; index++ ) {
        if ( fe_stop_asked() ) {
            return DFE_USER;
        }
        node = plan.items[index];
        if ( nodes[node].flags & NF_DIR ) {
            rc = place_dir( nodes[node].dir, nodes[node].target );
        } else {
            rc = place_file( node, nodes[node].target );
        }
    }
    /* what there was no place for: in one piece at least, where it is */
    for ( index = 0; index < rest.count && rc == 0; index++ ) {
        if ( fe_stop_asked() ) {
            return DFE_USER;
        }
        node = rest.items[index];
        if ( nodes[node].flags & NF_DIR ) {
            rc = settle( nodes[node].dir );
        }
        if ( rc != 0 ) {
            break;
        }
        if ( in_pieces( node ) ) {
            rc = unfragment( node );
        } else {
            worked( nodes[node].nclus );
        }
    }
    return rc;
}

static int run_unfrag( void )
{
    u32 index, dirno, left, before = 0;
    int rc = 0, pass;

    plan_unfrag();
    work_total = work_done = 0;
    for ( index = 0; index < plan.count; index++ ) {
        work_total += nodes[plan.items[index]].nclus;
    }
    for ( dirno = 1; dirno < ndirs; dirno++ ) {
        if ( !left_alone( dirno ) && in_pieces( dirs[dirno].node ) ) {
            work_total += nodes[dirs[dirno].node].nclus;
        }
    }
    fe_progress( 0, work_total );

    if ( dir_arrange( 0, SORT_NONE ) ) {
        rc = dir_rewrite( 0 );
    }
    for ( dirno = 1; dirno < ndirs && rc == 0; dirno++ ) {
        if ( fe_stop_asked() ) {
            return DFE_USER;
        }
        if ( !left_alone( dirno ) ) {
            rc = unfragment_dir( dirno );
        }
    }
    /* A file there was no room for may have room once others have been
       gathered up: again, for as long as that gets anywhere. */
    for ( pass = 0; pass < 4 && rc == 0; pass++ ) {
        left = 0;
        for ( index = 0; index < plan.count && rc == 0; index++ ) {
            if ( fe_stop_asked() ) {
                return DFE_USER;
            }
            if ( pass == 0 || in_pieces( plan.items[index] ) ) {
                rc = unfragment( plan.items[index] );
                if ( rc == 0 && in_pieces( plan.items[index] ) ) {
                    left++;
                }
            }
        }
        if ( left == 0 || left == before ) {
            break;
        }
        before = left;
    }
    return rc;
}

int optimize( void )
{
    u32 clus;
    int rc;

    if ( dst.free == 0 ) {
        return DFE_NOFREE;
    }
    hint = 0;
    tree_unplace();
    tmp_src = (u32 *)xalloc( move_limit() * sizeof( u32 ) );
    rc = opt.method == METHOD_FULL ? run_full() : run_unfrag();
    xfree( tmp_src );
    tmp_src = NULL;
    move_done();
    list_free( &plan );
    list_free( &rest );
    fe_activity( ACT_IDLE, 0, 0 );

    /* the drive as it is now, and the FAT32 notes about its free space */
    tree_stats();
    for ( clus = 2; clus <= vol.maxclus && !is_free( clus ); clus++ ) {
    }
    fsinfo_update( dst.free, clus <= vol.maxclus ? clus : 0xFFFFFFFFul );
    if ( rc == 0 ) {
        fe_progress( work_total ? work_total : 1, work_total ? work_total : 1 );
    }
    return rc;
}
