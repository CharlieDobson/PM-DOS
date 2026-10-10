/*
 * TREE.C - a drive's directories.
 *
 * A DRIVE IS READ ONCE, all of its directories, the first time it is
 * shown - the way the MS-DOS Shell reads one, and for the same reason:
 * the tree can then say which directories have more inside them before
 * anybody opens one.  Only directories are kept; a directory's files
 * are read when it is the one being shown (FILES.C).  The tree stays
 * until Refresh, or until something the shell itself does changes it -
 * a directory made or deleted goes in or comes out without the disk
 * being read again.
 *
 * THE NODES ARE AN ARRAY IN THE ORDER THE SCREEN LISTS THEM: a
 * directory, then everything inside it, then its next sibling.  So the
 * lines that show are a walk down the array that skips what is under a
 * closed node (vis_build), and a node's children are the nodes after it
 * that name it as their parent.
 *
 * ONE DIRECTORY'S NAMES ARE ALL TAKEN BEFORE ANY OF THEM IS ENTERED.
 * The classic search calls keep their place in one transfer area, so
 * two searches cannot be in progress at once - and the names want
 * sorting anyway.
 */
#include "shell.h"

static DTREE   trees[26];
static FINDREC rec;
static char    scan_path[PATH_MAX + 16];
static u32     scan_count;
static int     scan_failed;

/* ------------------------------------------------------------------ */
/* the array                                                           */
/* ------------------------------------------------------------------ */

static void tree_free( DTREE *tree )
{
    int index;

    for ( index = 0; index < tree->count; index++ ) {
        xfree( tree->node[index].name );
    }
    xfree( tree->node );
    xfree( tree->vis );
    tree->node = NULL;
    tree->vis = NULL;
    tree->count = tree->cap = tree->nvis = 0;
    tree->valid = 0;
}

/* room for one more node at "at", the ones from there on moved up */
static int node_insert( DTREE *tree, int at, const char *name, int parent, int depth )
{
    TNODE *bigger;
    int index;

    if ( tree->count == tree->cap ) {
        bigger = (TNODE *)xalloc( (u32)(tree->cap + 64) * sizeof( TNODE ) );
        if ( tree->count ) {
            mem_cpy( bigger, tree->node, (u32)tree->count * sizeof( TNODE ) );
        }
        xfree( tree->node );
        tree->node = bigger;
        tree->cap += 64;
        xfree( tree->vis );
        tree->vis = (int *)xalloc( (u32)tree->cap * sizeof( int ) );
    }
    if ( at < tree->count ) {
        mem_cpy( tree->node + at + 1, tree->node + at, (u32)(tree->count - at) * sizeof( TNODE ) );
        for ( index = at + 1; index <= tree->count; index++ ) {
            if ( tree->node[index].parent >= at ) {
                tree->node[index].parent++;
            }
        }
    }
    tree->count++;
    tree->node[at].name = str_dup( name );
    tree->node[at].parent = parent;
    tree->node[at].depth = (u8)depth;
    tree->node[at].flags = 0;
    return at;
}

/* the index just past a node and everything inside it */
static int subtree_end( DTREE *tree, int node )
{
    int index = node + 1;

    while ( index < tree->count && tree->node[index].depth > tree->node[node].depth ) {
        index++;
    }
    return index;
}

/* which of a node's children is the last: the tree's lines need it */
static void fix_last( DTREE *tree, int parent )
{
    int index, last = -1, end = subtree_end( tree, parent );

    for ( index = parent + 1; index < end; index++ ) {
        if ( tree->node[index].parent == parent ) {
            tree->node[index].flags &= ~TN_LAST;
            last = index;
        }
    }
    if ( last >= 0 ) {
        tree->node[last].flags |= TN_LAST;
        tree->node[parent].flags |= TN_KIDS;
    } else {
        tree->node[parent].flags &= ~(TN_KIDS | TN_OPEN);
    }
}

static void vis_build( DTREE *tree )
{
    int index, depth, closed = 0x7FFF;

    tree->nvis = 0;
    for ( index = 0; index < tree->count; index++ ) {
        depth = tree->node[index].depth;
        if ( depth > closed ) {
            continue;                   /* under a node that is shut */
        }
        tree->vis[tree->nvis++] = index;
        closed = (tree->node[index].flags & TN_OPEN) ? 0x7FFF : depth;
    }
}

/* ------------------------------------------------------------------ */
/* reading a drive                                                     */
/* ------------------------------------------------------------------ */

static void sort_names( char **names, int count )
{
    int gap, index, at;
    char *held;

    for ( gap = count / 2; gap > 0; gap /= 2 ) {
        for ( index = gap; index < count; index++ ) {
            held = names[index];
            for ( at = index; at >= gap && str_icmp( names[at - gap], held ) > 0; at -= gap ) {
                names[at] = names[at - gap];
            }
            names[at] = held;
        }
    }
}

static void scan_note( void )
{
    char line[48], num[16];

    str_cpy( line, "Directories read: " );
    str_catn( line, fmt_num( scan_count, 0, num ), sizeof( line ) );
    busy_show( "Reading disk information...", line );
}

/* The directories inside "node", whose path with a backslash after it
   is in scan_path, and theirs. */
static void scan_dir( DTREE *tree, int node )
{
    char **names = NULL, **bigger;
    int count = 0, cap = 0, index, child, rc;
    u32 handle, len = str_len( scan_path );
    u16 attrs = (u16)(0x1010 | (opt.show_hidden ? ATTR_HIDDEN | ATTR_SYSTEM : 0));

    if ( len > PATH_MAX - 16 ) {
        return;                         /* deeper than a path can say */
    }
    str_cpy( scan_path + len, "*.*" );
    rc = sys_find_first( scan_path, attrs, &rec, &handle );
    if ( rc == 0 ) {
        do {
            if ( !(rec.attr & ATTR_DIR) || rec.name[0] == '.' ) {
                continue;
            }
            if ( count == cap ) {
                bigger = (char **)xalloc( (u32)(cap + 32) * sizeof( char * ) );
                if ( count ) {
                    mem_cpy( bigger, names, (u32)count * sizeof( char * ) );
                }
                xfree( names );
                names = bigger;
                cap += 32;
            }
            names[count++] = str_dup( rec.name );
        } while ( sys_find_next( handle, &rec ) == 0 );
        sys_find_close( handle );
    } else if ( node == 0 && rc != ERR_NOMORE && rc != ERR_NOFILE ) {
        scan_failed = rc;
    }
    if ( sys_crit_seen() ) {
        scan_failed = ERR_NOTREADY;
    }
    scan_path[len] = 0;
    sort_names( names, count );
    for ( index = 0; index < count && !scan_failed; index++ ) {
        child = node_insert( tree, tree->count, names[index], node,
                             tree->node[node].depth + 1 );
        if ( (++scan_count & 31) == 0 ) {
            scan_note();
        }
        if ( len + str_len( names[index] ) + 2 < sizeof( scan_path ) ) {
            str_cpy( scan_path + len, names[index] );
            str_catn( scan_path, "\\", sizeof( scan_path ) );
            scan_dir( tree, child );
            scan_path[len] = 0;
        }
        if ( index == count - 1 ) {
            tree->node[child].flags |= TN_LAST;
        }
    }
    if ( count ) {
        tree->node[node].flags |= TN_KIDS;
    }
    for ( index = 0; index < count; index++ ) {
        xfree( names[index] );
    }
    xfree( names );
}

void tree_space( DTREE *tree )
{
    sys_volume_label( tree->drive, tree->label );
    sys_disk_space( tree->drive, &tree->free_kb, &tree->total_kb );
    sys_crit_seen();
}

/* A drive's tree: read now if it has not been, or if "reread".  NULL
   when the drive cannot be read - no disk in it, usually. */
DTREE *tree_get( int drive, int reread )
{
    DTREE *tree;

    if ( drive < 0 || drive >= 26 ) {
        return NULL;
    }
    tree = &trees[drive];
    if ( tree->valid && !reread ) {
        return tree;
    }
    tree_free( tree );
    tree->drive = drive;
    scan_path[0] = (char)('A' + drive);
    str_cpy( scan_path + 1, ":\\" );
    scan_count = 0;
    scan_failed = 0;
    sys_crit_seen();
    node_insert( tree, 0, "", -1, 0 );
    busy_show( "Reading disk information...", NULL );
    scan_dir( tree, 0 );
    if ( scan_failed ) {
        tree_free( tree );
        return NULL;
    }
    tree->node[0].flags |= TN_OPEN;
    vis_build( tree );
    tree_space( tree );
    tree->valid = 1;
    return tree;
}

/* ------------------------------------------------------------------ */
/* asking it things                                                    */
/* ------------------------------------------------------------------ */

void tree_path( DTREE *tree, int node, char *buf )
{
    int chain[80], count = 0, index;

    buf[0] = (char)('A' + tree->drive);
    buf[1] = ':';
    buf[2] = '\\';
    buf[3] = 0;
    for ( index = node; index > 0 && count < 80; index = tree->node[index].parent ) {
        chain[count++] = index;
    }
    while ( count ) {
        str_catn( buf, tree->node[chain[--count]].name, PATH_MAX );
        if ( count ) {
            str_catn( buf, "\\", PATH_MAX );
        }
    }
}

static int child_named( DTREE *tree, int parent, const char *name, u32 len )
{
    int index, end = subtree_end( tree, parent );

    for ( index = parent + 1; index < end; index++ ) {
        if ( tree->node[index].parent == parent && str_len( tree->node[index].name ) == len
             && str_nicmp( tree->node[index].name, name, len ) == 0 ) {
            return index;
        }
    }
    return -1;
}

/* the node a path names, or the last directory along it that exists */
int tree_find( DTREE *tree, const char *path )
{
    int node = 0, child;
    u32 len;

    if ( path[0] && path[1] == ':' ) {
        path += 2;
    }
    for ( ;; ) {
        while ( *path == '\\' ) {
            path++;
        }
        if ( *path == 0 ) {
            return node;
        }
        for ( len = 0; path[len] && path[len] != '\\'; len++ ) {
        }
        child = child_named( tree, node, path, len );
        if ( child < 0 ) {
            return node;
        }
        node = child;
        path += len;
    }
}

/* the line a node is on, with whatever hid it opened */
int tree_vis_of( DTREE *tree, int node )
{
    int index, changed = 0;

    for ( index = tree->node[node].parent; index >= 0; index = tree->node[index].parent ) {
        if ( !(tree->node[index].flags & TN_OPEN) ) {
            tree->node[index].flags |= TN_OPEN;
            changed = 1;
        }
    }
    if ( changed ) {
        vis_build( tree );
    }
    for ( index = 0; index < tree->nvis; index++ ) {
        if ( tree->vis[index] == node ) {
            return index;
        }
    }
    return 0;
}

void tree_open( DTREE *tree, int node, int whole_branch )
{
    int index, end = subtree_end( tree, node );

    if ( tree->node[node].flags & TN_KIDS ) {
        tree->node[node].flags |= TN_OPEN;
    }
    if ( whole_branch ) {
        for ( index = node + 1; index < end; index++ ) {
            if ( tree->node[index].flags & TN_KIDS ) {
                tree->node[index].flags |= TN_OPEN;
            }
        }
    }
    vis_build( tree );
}

void tree_open_all( DTREE *tree )
{
    tree_open( tree, 0, 1 );
}

void tree_close( DTREE *tree, int node )
{
    tree->node[node].flags &= ~TN_OPEN;
    vis_build( tree );
}

/* ------------------------------------------------------------------ */
/* what the shell itself changed                                       */
/* ------------------------------------------------------------------ */

/* a directory made inside "parent": into the tree where its name sorts */
int tree_add( DTREE *tree, int parent, const char *name )
{
    int index, end = subtree_end( tree, parent ), at = end;

    for ( index = parent + 1; index < end; index++ ) {
        if ( tree->node[index].parent == parent && str_icmp( tree->node[index].name, name ) > 0 ) {
            at = index;
            break;
        }
    }
    at = node_insert( tree, at, name, parent, tree->node[parent].depth + 1 );
    fix_last( tree, parent );
    tree->node[parent].flags |= TN_OPEN;
    vis_build( tree );
    return at;
}

/* a directory deleted: out, with whatever the tree had inside it */
void tree_remove( DTREE *tree, int node )
{
    int end = subtree_end( tree, node ), gone = end - node, index;
    int parent = tree->node[node].parent;

    if ( node <= 0 ) {
        return;
    }
    for ( index = node; index < end; index++ ) {
        xfree( tree->node[index].name );
    }
    mem_cpy( tree->node + node, tree->node + end, (u32)(tree->count - end) * sizeof( TNODE ) );
    tree->count -= gone;
    for ( index = node; index < tree->count; index++ ) {
        if ( tree->node[index].parent >= end ) {
            tree->node[index].parent -= gone;
        }
    }
    fix_last( tree, parent );
    vis_build( tree );
}
