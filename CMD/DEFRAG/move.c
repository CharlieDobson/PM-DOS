/*
 * MOVE.C - clusters moved from one place on a drive to another.  The
 * only code in DEFRAG that changes a disk.
 *
 * A MOVE IS FOUR WRITES, IN AN ORDER THAT CAN BE STOPPED ANYWHERE.  A
 * drive being optimized is a drive on which a power cut is a matter of
 * time, so each step leaves every file whole:
 *
 *   1  the data, into clusters the FAT calls free.  Stopped here,
 *      nothing has happened.
 *   2  the FAT entries of the new clusters: a chain, ending where the
 *      old clusters' chain went on.  Stopped here, the new clusters
 *      are in use by nobody - lost clusters, which ScanDisk frees.
 *   3  the one link that pointed at the old clusters: the FAT entry of
 *      the cluster before them, or the directory entry when they were
 *      the file's first.  Stopped here, the file is its new clusters
 *      and it is the old ones nobody uses.
 *   4  the FAT entries of the old clusters, freed.
 *
 * There is no moment at which a file's chain leads into a free cluster
 * or stops short, which are the two ways of losing what is in it.
 * Every step is on the disk, in every copy of the FAT, before the next
 * begins (fat_commit).
 *
 * A DIRECTORY'S FIRST CLUSTER is named in three kinds of place: its
 * entry in the directory it is in, its own "." and the ".." of every
 * directory inside it.  The first is the link of step 3.  The "." goes
 * out with the data in step 1, already changed.  The ".." entries are
 * written after step 3 and before step 4: a ".." that names the old
 * cluster for a moment is wrong in a way nothing follows, since DOS
 * finds a parent by its path.
 *
 * WHAT IS IN MEMORY IS KEPT AS THE DISK IS: own[] and prev[], each
 * node's first cluster, and for a directory its list of clusters and
 * the entries themselves.
 */
#include "defrag.h"

#define MOVE_BYTES      (128u * 1024u)  /* the most one move carries */

u32 move_error_clus;

static u8  *xbuf;
static u32  xbuf_clus;          /* how many clusters it holds */

/* how many clusters one move may be of */
u32 move_limit( void )
{
    if ( xbuf == NULL ) {
        xbuf_clus = MOVE_BYTES / vol.bpc;
        if ( xbuf_clus == 0 ) {
            xbuf_clus = 1;
        }
        xbuf = (u8 *)xalloc( xbuf_clus * vol.bpc );
    }
    return xbuf_clus;
}

void move_done( void )
{
    xfree( xbuf );
    xbuf = NULL;
    xbuf_clus = 0;
}

/* a node is leaving its final place after all: the map is told */
static void unplace( u32 node )
{
    u32 clus;

    if ( !(nodes[node].flags & NF_PLACED) ) {
        return;
    }
    nodes[node].flags &= ~NF_PLACED;
    for ( clus = nodes[node].first; CLUS_OK( clus ) && own[clus] == node; clus = fat_get( clus ) ) {
        map_count( clus, 0, 0, -1 );
    }
}

/* the directory entry that names a node's first cluster, and its sector
   written */
static int link_entry( u32 node, u32 first )
{
    DIRBUF *dir = &dirs[nodes[nodes[node].parent].dir].buf;

    entry_set_first( dir->data + nodes[node].ent * 32, first );
    return dir_write_entry( dir, nodes[node].ent );
}

/* the ".." of every directory inside directory "dirno" */
static int link_children( u32 dirno, u32 first )
{
    const DIRINFO *di = &dirs[dirno];
    DIRBUF *sub;
    u32 idx, node;
    int bad = 0;

    for ( idx = 0; idx < di->buf.nent; idx++ ) {
        node = di->who[idx];
        if ( node == N_NONE || !(nodes[node].flags & NF_DIR) || nodes[node].nclus == 0 ) {
            continue;
        }
        sub = &dirs[nodes[node].dir].buf;
        entry_set_first( sub->data + 32, first );
        if ( dir_write_entry( sub, 1 ) != 0 ) {
            bad = 1;
        }
    }
    return bad ? -1 : 0;
}

/* steps 2 to 4, and the tables: "count" clusters of "node" that were
   src[] and followed "before" are dst.. now */
static int relink( u32 node, u32 before, const u32 *src, u32 count, u32 dst )
{
    u32 after = fat_get( src[count - 1] ), index;
    int bad = 0;

    for ( index = 0; index < count; index++ ) {
        fat_set( dst + index, index + 1 < count ? dst + index + 1 : after );
    }
    if ( fat_commit() != 0 ) {
        bad = 1;
    }
    if ( before == 0 ) {
        if ( link_entry( node, dst ) != 0 ) {
            bad = 1;
        }
        nodes[node].first = dst;
        if ( (nodes[node].flags & NF_DIR) && link_children( nodes[node].dir, dst ) != 0 ) {
            bad = 1;
        }
    } else {
        fat_set( before, dst );
        if ( fat_commit() != 0 ) {
            bad = 1;
        }
    }
    for ( index = 0; index < count; index++ ) {
        fat_set( src[index], 0 );
    }
    if ( fat_commit() != 0 ) {
        bad = 1;
    }

    for ( index = 0; index < count; index++ ) {
        own[src[index]] = N_NONE;
        prev[src[index]] = 0;
        map_count( src[index], -1, 0, 0 );
    }
    for ( index = 0; index < count; index++ ) {
        own[dst + index] = node;
        prev[dst + index] = index ? dst + index - 1 : before;
        map_count( dst + index, 1, 0, 0 );
    }
    if ( CLUS_OK( after ) ) {
        prev[after] = dst + count - 1;
    }
    if ( bad ) {
        move_error_clus = dst;
        return DFE_WRITE;
    }
    return 0;
}

/*
 * Move "count" clusters of "node" - src[], one after another in its
 * chain, the first of them following cluster "before" (0 = they start
 * the chain) - into the free clusters dst to dst+count-1.  No more than
 * move_limit() at a time.
 */
int move_run( u32 node, u32 before, const u32 *src, u32 count, u32 dst )
{
    DIRBUF *dir = NULL;
    u32 index, run, at = 0;

    move_limit();
    if ( count == 0 || count > xbuf_clus || node == N_NONE || (nodes[node].flags & NF_FIXED) ) {
        return DFE_INTERNAL;
    }
    unplace( node );

    /* the data out of where it is, a run of neighbours at a time */
    for ( index = 0; index < count; index += run ) {
        for ( run = 1; index + run < count && src[index + run] == src[index] + run; run++ ) {
        }
        fe_activity( ACT_READ, src[index], run );
        if ( read_secs( clus_lba( src[index] ), run * vol.spc, xbuf + index * vol.bpc ) != 0 ) {
            move_error_clus = src[index];
            fe_activity( ACT_IDLE, 0, 0 );
            return DFE_READ;
        }
    }
    if ( nodes[node].flags & NF_DIR ) {
        dir = &dirs[nodes[node].dir].buf;
        for ( at = 0; at < dir->clus.count && dir->clus.items[at] != src[0]; at++ ) {
        }
        if ( at + count > dir->clus.count ) {
            return DFE_INTERNAL;
        }
        if ( before == 0 ) {
            entry_set_first( xbuf, dst );           /* its "." */
        }
    }

    fe_activity( ACT_WRITE, dst, count );
    if ( write_secs( clus_lba( dst ), count * vol.spc, xbuf, WR_DATA ) != 0 ) {
        move_error_clus = dst;
        fe_activity( ACT_IDLE, 0, 0 );
        return DFE_WRITE;
    }
    if ( dir ) {
        if ( before == 0 ) {
            entry_set_first( dir->data, dst );
        }
        for ( index = 0; index < count; index++ ) {
            dir->clus.items[at + index] = dst + index;
        }
    }
    fe_activity( ACT_FAT, dst, count );
    index = (u32)relink( node, before, src, count, dst );
    fe_activity( ACT_IDLE, 0, 0 );
    return (int)index;
}

/*
 * A whole directory out of memory into the free clusters from dst on,
 * and changed over to.  This is how a directory that has been arranged
 * reaches the disk without ever being half one thing and half the
 * other: the old one is there until the new one is complete.
 */
int move_dir( u32 dirno, u32 dst )
{
    DIRINFO *di = &dirs[dirno];
    u32 node = di->node, count = di->buf.clus.count, old_first = nodes[node].first, index;
    u32 *src;
    int rc;

    if ( count == 0 || node == N_ROOT || (nodes[node].flags & NF_FIXED) ) {
        return DFE_INTERNAL;
    }
    unplace( node );
    entry_set_first( di->buf.data, dst );
    fe_activity( ACT_WRITE, dst, count );
    if ( write_secs( clus_lba( dst ), count * vol.spc, di->buf.data, WR_DIR ) != 0 ) {
        entry_set_first( di->buf.data, old_first );
        move_error_clus = dst;
        fe_activity( ACT_IDLE, 0, 0 );
        return DFE_WRITE;
    }
    src = (u32 *)xalloc( count * sizeof( u32 ) );
    mem_cpy( src, di->buf.clus.items, count * sizeof( u32 ) );
    for ( index = 0; index < count; index++ ) {
        di->buf.clus.items[index] = dst + index;
    }
    fe_activity( ACT_FAT, dst, count );
    rc = relink( node, 0, src, count, dst );
    xfree( src );
    fe_activity( ACT_IDLE, 0, 0 );
    return rc;
}

/*
 * A SORTED directory written where it lies - which a root directory
 * always is, having nowhere else to be.  Sorting moves entries both
 * ways, so no one pass over the sectors is safe: an entry going from
 * the first sector to the last would be in neither for a while.  So
 * the directory is made to hold its entries TWICE before it holds them
 * differently:
 *
 *   1  the new order is written BEHIND the old entries, in the unused
 *      part of the directory, with deleted entries filling the old
 *      last sector up to it.  Last sector first: until the sector with
 *      the old end mark is written - the last write of this step, and
 *      one sector - a search still stops where it always did.
 *   2  the new order is written from the front.  Whatever a sector
 *      loses is still there behind, and the sector that holds the new
 *      end mark is the last one written.
 *   3  what is behind that end mark is cleared.
 *
 * Stopped anywhere, every entry is in the directory at least once.
 * It takes room for both, and without it (0 is the answer) the caller
 * writes the directory straight through.
 */
static int rewrite_twice( DIRBUF *dir, const u8 *old, u32 old_end, u32 new_end )
{
    u32 per = vol.bps / 32;
    u32 bytes = dir->fixed_root ? vol.root_secs * vol.bps : dir->nent * 32;
    u32 behind = (old_end + per - 1) / per * per;       /* where the copy starts */
    u32 first_sec = old_end / per, last_sec, head_secs, sec, idx;
    u8 *both;
    int bad = 0;

    if ( new_end == 0 || behind + new_end > dir->nent ) {
        return 0;
    }
    /* the sector the copy's own end mark is in goes out too, and first:
       what lies past the old end mark is not to be seen for a moment */
    last_sec = behind + new_end < dir->nent ? (behind + new_end) / per
                                            : (behind + new_end - 1) / per;
    head_secs = (new_end + per - 1) / per;
    both = (u8 *)xzalloc( bytes );
    mem_cpy( both, old, old_end * 32 );
    for ( idx = old_end; idx < behind; idx++ ) {
        both[idx * 32] = DEL_MARK;
    }
    mem_cpy( both + behind * 32, dir->data, new_end * 32 );
    for ( sec = last_sec + 1; sec > first_sec && !bad; sec-- ) {
        bad = dir_write_sec( dir, both, sec - 1 ) != 0;
    }
    xfree( both );
    for ( sec = 0; sec < head_secs && !bad; sec++ ) {
        bad = dir_write_sec( dir, dir->data, sec ) != 0;
    }
    for ( sec = head_secs; sec <= last_sec && !bad; sec++ ) {
        bad = dir_write_sec( dir, dir->data, sec ) != 0;
    }
    return bad ? -1 : 1;
}

/* a directory written where it is */
int dir_rewrite( u32 dirno )
{
    DIRBUF *dir = &dirs[dirno].buf;
    u32 first = dir->fixed_root ? 0 : dir->clus.count ? dir->clus.items[0] : 0;
    int rc = 0;

    fe_activity( ACT_SORT, first, dir->fixed_root ? 0 : dir->clus.count );
    if ( arranged.old != NULL && arranged.dirno == dirno && arranged.sorted ) {
        rc = rewrite_twice( dir, arranged.old, arranged.old_end, arranged.new_end );
    }
    /* squeezed only, or no room for two: first sector to last (DIRS.C) */
    rc = rc == 0 ? dir_write_all( dir ) : rc < 0 ? -1 : 0;
    fe_activity( ACT_IDLE, 0, 0 );
    if ( rc != 0 ) {
        move_error_clus = first;
        return DFE_WRITE;
    }
    return 0;
}
