/*
 * TREE.C - every file and directory of a drive, who has each cluster,
 * and whether the file system is sound.
 *
 * A DRIVE IS READ ONCE, WHOLE.  Every directory is read into memory and
 * stays there (DIRINFO), every entry that names a file or a directory
 * becomes a NODE, and every chain is followed through the FAT: each
 * cluster of it is given its owner (own[]) and the cluster before it in
 * the chain (prev[]).  With those two tables any cluster on the drive
 * can be moved without looking anything up - who it belongs to, and
 * which FAT entry, or which directory entry, points at it.
 *
 * READING IS ALSO THE CHECK.  A chain that leaves the drive or runs
 * into a free cluster, two entries that reach the same cluster, a size
 * that disagrees with the chain, a directory without its "." and "..",
 * clusters in use that nothing reached, FAT copies that differ: each is
 * a thing a program must not rearrange a drive on top of, and each is
 * something ScanDisk repairs.  They are counted in dst.errors, and
 * MAIN.C offers ScanDisk when there are any.  Nothing is moved on a
 * drive that has one.
 *
 * WHAT IS NOT MOVED (NF_FIXED, the map's X): a file or directory with
 * the System attribute, always - that is what a boot file has on every
 * DOS there is; one with the Hidden attribute unless /H was given; and
 * the FAT32 root directory, whose first cluster the boot sector names.
 */
#include "defrag.h"

NODE     *nodes;
u32       nnodes;
DIRINFO  *dirs;
u32       ndirs;
u32      *own;
u32      *prev;
DRIVESTAT dst;
BLOCK    *blocks;
u32       nblocks, per_block = 1;

static u32 node_cap, dir_cap;

/* ------------------------------------------------------------------ */
/* the tables                                                          */
/* ------------------------------------------------------------------ */

static u32 node_new( void )
{
    NODE *grown;

    if ( nnodes == node_cap ) {
        node_cap = node_cap ? node_cap * 2 : 512;
        grown = (NODE *)xalloc( node_cap * sizeof( NODE ) );
        if ( nnodes ) {
            mem_cpy( grown, nodes, nnodes * sizeof( NODE ) );
        }
        xfree( nodes );
        nodes = grown;
    }
    mem_set( &nodes[nnodes], 0, sizeof( NODE ) );
    return nnodes++;
}

static u32 dir_new( u32 node )
{
    DIRINFO *grown;

    if ( ndirs == dir_cap ) {
        dir_cap = dir_cap ? dir_cap * 2 : 64;
        grown = (DIRINFO *)xalloc( dir_cap * sizeof( DIRINFO ) );
        if ( ndirs ) {
            mem_cpy( grown, dirs, ndirs * sizeof( DIRINFO ) );
        }
        xfree( dirs );
        dirs = grown;
    }
    mem_set( &dirs[ndirs], 0, sizeof( DIRINFO ) );
    dirs[ndirs].node = node;
    nodes[node].dir = ndirs;
    return ndirs++;
}

void tree_free( void )
{
    u32 index;

    for ( index = 0; index < ndirs; index++ ) {
        dir_free( &dirs[index].buf );
        xfree( dirs[index].who );
    }
    xfree( dirs );
    xfree( nodes );
    xfree( own );
    xfree( prev );
    xfree( blocks );
    dir_forget();
    dirs = NULL;
    nodes = NULL;
    own = prev = NULL;
    blocks = NULL;
    ndirs = dir_cap = nnodes = node_cap = nblocks = 0;
    per_block = 1;
    mem_set( &dst, 0, sizeof( dst ) );
}

/* ------------------------------------------------------------------ */
/* a chain                                                             */
/* ------------------------------------------------------------------ */

/* Follow the chain that starts at "first", giving each cluster to
   "node"; the clusters go into "list" when one is wanted.  It stops at
   the first thing wrong and says so in dst.errors. */
static void chain_claim( u32 node, u32 first, U32LIST *list )
{
    u32 clus = first, before = 0, val, count = 0, runs = 0;

    for ( ;; ) {
        if ( !CLUS_OK( clus ) ) {
            dst.errors |= FSE_CHAIN;
            break;
        }
        if ( own[clus] != N_NONE ) {
            /* somebody has it already - or this chain has, and is a ring */
            dst.errors |= FSE_CROSS;
            dst.cross++;
            break;
        }
        val = fat_get( clus );
        if ( val == 0 || val == vol.bad_val ) {
            dst.errors |= FSE_CHAIN;
            break;
        }
        own[clus] = node;
        prev[clus] = before;
        if ( before == 0 || clus != before + 1 ) {
            runs++;
        }
        count++;
        if ( list ) {
            list_add( list, clus );
        }
        if ( IS_EOC( val ) ) {
            break;
        }
        before = clus;
        clus = val;
    }
    nodes[node].nclus = count;
    if ( runs > 1 ) {
        nodes[node].flags |= NF_FRAG;
    }
}

/* ------------------------------------------------------------------ */
/* a directory                                                         */
/* ------------------------------------------------------------------ */

static int is_dot( const u8 *ent, int dots )
{
    static const char names[2][12] = { ".          ", "..         " };

    return mem_cmp( ent, names[dots - 1], 11 ) == 0 && (ent[DE_ATTR] & A_DIR);
}

const char *node_name( u32 node, char *buf )
{
    const u8 *ent;
    int pos, len = 0;

    if ( node == N_ROOT || node == N_NONE ) {
        buf[0] = '\\';
        buf[1] = 0;
        return buf;
    }
    ent = dirs[nodes[nodes[node].parent].dir].buf.data + nodes[node].ent * 32;
    for ( pos = 0; pos < 8 && ent[pos] != ' '; pos++ ) {
        buf[len++] = (char)ent[pos];
    }
    if ( ent[8] != ' ' ) {
        buf[len++] = '.';
        for ( pos = 8; pos < 11 && ent[pos] != ' '; pos++ ) {
            buf[len++] = (char)ent[pos];
        }
    }
    buf[len] = 0;
    if ( (u8)buf[0] == 0x05 ) {
        buf[0] = (char)0xE5;                /* how a name that starts E5h is kept */
    }
    return buf;
}

/* One directory, already in memory: a node for everything in it, and a
   DIRINFO for each directory.  0, or DFE_READ when one of those would
   not read. */
static int dir_scan( u32 dirno )
{
    u32 nent = dirs[dirno].buf.nent, me = dirs[dirno].node, idx, node, first, size, want, sub;
    u32 parent_first;
    U32LIST chain;
    const u8 *ent;
    char name[13];
    u8 attr;
    int rc;

    dirs[dirno].who = (u32 *)xzalloc( (nent ? nent : 1) * sizeof( u32 ) );
    fe_reading( node_name( me, name ) );

    /* a directory that is not the root starts with itself and its parent */
    if ( dirno != 0 ) {
        ent = dirs[dirno].buf.data;
        parent_first = nodes[me].parent == N_ROOT ? 0 : nodes[nodes[me].parent].first;
        if ( nent < 2 || !is_dot( ent, 1 ) || !is_dot( ent + 32, 2 )
             || entry_first( ent ) != nodes[me].first || entry_first( ent + 32 ) != parent_first ) {
            dst.errors |= FSE_DIR;
        }
    }

    for ( idx = 0; idx < nent; idx++ ) {
        ent = dirs[dirno].buf.data + idx * 32;
        if ( ent[0] == 0 ) {
            break;                          /* nothing from here on */
        }
        if ( ent[0] == DEL_MARK ) {
            dirs[dirno].dead++;
            continue;
        }
        attr = ent[DE_ATTR];
        if ( IS_LFN( ent ) || (attr & A_VOLID) ) {
            continue;
        }
        if ( ent[0] == '.' ) {
            if ( dirno == 0 || idx > 1 ) {
                dst.errors |= FSE_DIR;      /* a "." where there is none */
            }
            continue;
        }
        first = entry_first( ent );
        size = RD32( ent + DE_SIZE );
        node = node_new();
        nodes[node].parent = me;
        nodes[node].ent = idx;
        nodes[node].first = first;
        nodes[node].attr = attr;
        dirs[dirno].who[idx] = node;
        if ( (attr & A_SYSTEM) || ((attr & A_HIDDEN) && !opt.hidden) ) {
            nodes[node].flags |= NF_FIXED;
        }
        if ( attr & A_DIR ) {
            nodes[node].flags |= NF_DIR;
            dst.folders++;
            mem_set( &chain, 0, sizeof( chain ) );
            chain_claim( node, first, &chain );
            if ( chain.count == 0 || nodes[node].first != chain.items[0] ) {
                dst.errors |= FSE_DIR;      /* a directory with nothing in it
                                               cannot be a directory */
                list_free( &chain );
                continue;
            }
            sub = dir_new( node );
            rc = dir_load_list( &dirs[sub].buf, &chain );
            list_free( &chain );
            if ( rc != 0 ) {
                return DFE_READ;
            }
            continue;
        }
        dst.files++;
        if ( size == 0 ) {
            if ( first != 0 ) {
                dst.errors |= FSE_SIZE;     /* an empty file has no clusters */
            }
            nodes[node].first = 0;
            continue;
        }
        chain_claim( node, first, NULL );
        want = size / vol.bpc + (size % vol.bpc ? 1 : 0);
        if ( nodes[node].nclus != want ) {
            dst.errors |= FSE_SIZE;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the drive                                                           */
/* ------------------------------------------------------------------ */

int tree_read( void )
{
    U32LIST chain;
    u32 clus, val, dirno;
    int rc;

    tree_free();
    own = (u32 *)try_alloc( (vol.maxclus + 1) * sizeof( u32 ) );
    prev = (u32 *)try_alloc( (vol.maxclus + 1) * sizeof( u32 ) );
    if ( own == NULL || prev == NULL ) {
        return DFE_NOMEM;
    }
    mem_set( own, 0, (vol.maxclus + 1) * sizeof( u32 ) );
    mem_set( prev, 0, (vol.maxclus + 1) * sizeof( u32 ) );

    node_new();                             /* N_NONE: nobody */
    node_new();                             /* N_ROOT */
    nodes[N_ROOT].flags = NF_DIR | NF_FIXED;
    nodes[N_ROOT].attr = A_DIR;
    dir_new( N_ROOT );
    fe_reading( NULL );
    if ( vol.fattype == 32 ) {
        mem_set( &chain, 0, sizeof( chain ) );
        nodes[N_ROOT].first = vol.root_clus;
        chain_claim( N_ROOT, vol.root_clus, &chain );
        nodes[N_ROOT].flags &= ~NF_FRAG;    /* it is what it is */
        if ( chain.count == 0 ) {
            dst.errors |= FSE_DIR;
            list_free( &chain );
            return 0;
        }
        rc = dir_load_list( &dirs[0].buf, &chain );
        list_free( &chain );
    } else {
        rc = dir_load_root( &dirs[0].buf );
    }
    if ( rc != 0 ) {
        return DFE_READ;
    }

    /* every directory in the order it was found: a directory's own
       comes before the directories inside it */
    for ( dirno = 0; dirno < ndirs; dirno++ ) {
        rc = dir_scan( dirno );
        if ( rc != 0 ) {
            return rc;
        }
        if ( fe_stop_asked() ) {
            return DFE_USER;
        }
    }

    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        val = fat_get( clus );
        if ( val != 0 && val != vol.bad_val && own[clus] == N_NONE ) {
            dst.lost++;
        }
    }
    if ( dst.lost ) {
        dst.errors |= FSE_LOST;
    }
    if ( fat_differing() ) {
        dst.errors |= FSE_FATDIFF;
    }
    tree_stats();
    return 0;
}

/* The counts, from the tree and the FAT as they stand - which after a
   run is not as they were read. */
void tree_stats( void )
{
    u32 clus, val, node, before, runs;

    dst.used = dst.free = dst.bad = dst.fixed = dst.movable = 0;
    dst.frag_items = dst.gaps = 0;
    dst.top = 1;
    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        val = fat_get( clus );
        if ( val == 0 ) {
            dst.free++;
        } else if ( val == vol.bad_val ) {
            dst.bad++;
        } else {
            dst.used++;
            dst.top = clus;
            if ( nodes[own[clus]].flags & NF_FIXED ) {
                dst.fixed++;
            } else if ( own[clus] != N_NONE ) {
                dst.movable++;
            }
        }
    }
    for ( clus = 2; clus < dst.top; clus++ ) {
        if ( fat_get( clus ) == 0 ) {
            dst.gaps++;
        }
    }
    for ( node = N_ROOT + 1; node < nnodes; node++ ) {
        nodes[node].flags &= ~NF_FRAG;
        if ( nodes[node].nclus < 2 || !CLUS_OK( nodes[node].first ) ) {
            continue;
        }
        runs = 1;
        before = nodes[node].first;
        for ( clus = fat_get( before ); CLUS_OK( clus ) && own[clus] == node && runs < 2;
              clus = fat_get( clus ) ) {
            if ( clus != before + 1 ) {
                runs++;
            }
            before = clus;
        }
        if ( runs > 1 ) {
            nodes[node].flags |= NF_FRAG;
            /* what is never moved is never going to be in one piece,
               and is left out of what there is to do */
            if ( !(nodes[node].flags & NF_FIXED) ) {
                dst.frag_items++;
            }
        }
    }
}

/* How much of the drive is not fragmented: of the files and directories
   that have clusters and may be moved, the share that are in one piece. */
int tree_percent( void )
{
    u32 node, items = 0;

    for ( node = N_ROOT + 1; node < nnodes; node++ ) {
        if ( nodes[node].nclus && !(nodes[node].flags & NF_FIXED) ) {
            items++;
        }
    }
    if ( items == 0 ) {
        return 100;
    }
    return (int)((items - dst.frag_items) * 100 / items);
}

/*
 * What to suggest.  A drive with nothing in pieces and its files
 * already together needs nothing; one with only a little in pieces is
 * quickest put right file by file; anything else gets the full run.
 * The numbers are this program's own: nothing says what MS-DOS's are.
 */
int tree_recommend( void )
{
    int percent = tree_percent();

    if ( dst.frag_items == 0 ) {
        return dst.gaps > dst.used / 10 ? METHOD_FULL : METHOD_NONE;
    }
    return percent >= 95 ? METHOD_UNFRAG : METHOD_FULL;
}

/* ------------------------------------------------------------------ */
/* the map                                                             */
/* ------------------------------------------------------------------ */

void map_build( u32 cells )
{
    u32 clus, val, blk;

    xfree( blocks );
    if ( cells == 0 ) {
        cells = 1;
    }
    per_block = vol.nclus / cells + (vol.nclus % cells ? 1 : 0);
    if ( per_block == 0 ) {
        per_block = 1;
    }
    nblocks = vol.nclus / per_block + (vol.nclus % per_block ? 1 : 0);
    blocks = (BLOCK *)xzalloc( (nblocks ? nblocks : 1) * sizeof( BLOCK ) );
    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        val = fat_get( clus );
        blk = BLOCK_OF( clus );
        if ( val == vol.bad_val ) {
            blocks[blk].bad++;
        } else if ( val != 0 ) {
            blocks[blk].used++;
            /* what nothing owns is not going anywhere either */
            if ( own == NULL || own[clus] == N_NONE || (nodes[own[clus]].flags & NF_FIXED) ) {
                blocks[blk].fixed++;
            } else if ( nodes[own[clus]].flags & NF_PLACED ) {
                blocks[blk].done++;
            }
        }
    }
}

void map_count( u32 clus, int used, int fixed, int done )
{
    BLOCK *blk;

    if ( blocks == NULL || !CLUS_OK( clus ) ) {
        return;
    }
    blk = &blocks[BLOCK_OF( clus )];
    blk->used += (u32)used;
    blk->fixed += (u32)fixed;
    blk->done += (u32)done;
}

/* A run is starting: whatever an earlier run of this drive put in its
   final place is only where it happens to be. */
void tree_unplace( void )
{
    u32 index;

    for ( index = 0; index < nnodes; index++ ) {
        nodes[index].flags &= ~NF_PLACED;
    }
    for ( index = 0; index < nblocks; index++ ) {
        blocks[index].done = 0;
    }
}
