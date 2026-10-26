/*
 * SCAN.C - TreeView's disk scanner: DiskStat's SCAN.CPP, the DOS half,
 * with what a native program on PM-DOS can have that the DOS one could
 * not.  The tree is DiskStat's flat array - a row per folder, its depth
 * and whether it is open - kept in step with what is on the screen, and
 * a folder's size is the bytes in every file under it.
 *
 * WHAT CHANGED FROM THE DOS PROGRAM, and why:
 *
 *   - NO LIMITS BUT MEMORY.  The DOS program kept 256 rows and read at
 *     most 24 subfolders of a folder; the 25th and on were not counted
 *     in its size at all.  The rows are on the heap here and grow, and
 *     every subfolder is read.
 *
 *   - LONG NAMES.  The 71h search gives a folder its long name (LFN=OFF
 *     in CONFIG.SYS: the classic search, and 8.3 names).  A path is
 *     held to the kernel's 259 characters; a folder whose path would be
 *     longer is not reached, and the scan says it is incomplete.
 *
 *   - SIZES ARE 64 BITS.  A FAT32 or exFAT drive, and a folder on one,
 *     can hold more than 4 GB; the free space comes from 7303h, whose
 *     counts are 32 bits, before 36h, whose counts stop at 65,535.
 *
 *   - NO RECURSION.  DiskStat's ScanDirectory called itself a level at a
 *     time; here the walk keeps its place in a table, a folder's
 *     subfolder names in each entry.  As there, the names are read and
 *     the search closed before any of them is gone into, so no more
 *     than one search is open at a time - the kernel has 32 handles for
 *     the 71h searches, for every program at once, and a tree can be
 *     deeper than that.
 *
 *   - HIDDEN AND SYSTEM FILES COUNT, as they did: they take room.
 *
 * A SEARCH THAT MET A CRITICAL ERROR HAS FAILED, whatever came back: the
 * handler (DOSINT.ASM) fails the call and counts it, find_first and
 * find_next look at the count before and after, as DiskStat's ScanFirst
 * and ScanNext did, and the *_ask functions put the question once the
 * work is back - Retry does it again, Cancel keeps what was read.
 */
#include "treeview.h"

#define SCAN_PATH       260             /* the kernel's longest path */
#define SCAN_DEPTH      130             /* "\x" a level: 259 characters */
#define FIND_ATTR       0x37            /* everything but volume labels */
#define ATTR_DIR        0x10

int   current_drive = 3;
u64   total_disk_space;
u64   free_disk_space;
u64   used_disk_space;
NODE *tree_nodes;
int   tree_count;
int   selected_node = -1;
int   data_valid;

static int  node_cap;
static int  scan_cut;           /* a folder too deep, or no memory for a name */
static int  in_scan;

/* ------------------------------------------------------------------ */
/* numbers as the country writes them                                  */
/* ------------------------------------------------------------------ */

static char thou_sep[3] = ",";
static char dec_sep[3] = ".";
static int  country_read;

/* 21h/38h: the thousands separator at offset 7 and the decimal one at
   9, each a two-byte ASCIIZ field believed only when it is a string
   that is not empty - the comma and the point when not */
static void country_init( void )
{
    REGS regs;
    u8 block[64];
    int at, len;

    if ( country_read ) {
        return;
    }
    country_read = 1;
    memset( block, 0, sizeof( block ) );
    xa_clear( &regs );
    regs.edx = (u32)block;
    if ( xa_dos( &regs, 0x3800 ) != 0 ) {
        return;
    }
    for ( at = 7; at <= 9; at += 2 ) {
        for ( len = 0; len < 2 && block[at + len]; len++ ) {
        }
        if ( len > 0 && block[at + len] == 0 ) {
            memcpy( at == 7 ? thou_sep : dec_sep, block + at, (size_t)len );
            (at == 7 ? thou_sep : dec_sep)[len] = 0;
        }
    }
}

/* "1,234,567"; "out" holds 17 (ten digits, three separators of two) */
static void group_u32( u32 val, char *out )
{
    char digits[12];
    int count = 0, at;

    do {
        digits[count++] = (char)('0' + val % 10);
        val /= 10;
    } while ( val );
    for ( at = count - 1; at >= 0; at-- ) {
        *out++ = digits[at];
        if ( at > 0 && at % 3 == 0 ) {
            strcpy( out, thou_sep );
            out += strlen( thou_sep );
        }
    }
    *out = 0;
}

static u64 half64( u64 val )
{
    u32 hi = xa_hi32( val ), lo = xa_lo32( val );

    return xa_make64( hi >> 1, (lo >> 1) | (hi << 31) );
}

/* part * scale / whole, rounded down, with "part" held to "whole": the
   two are halved together until "whole" fits in 32 bits, which costs
   nothing a screen can show - except that a part a hair under the
   whole can halve to equal it, and less than all is never all */
u32 per_whole( u64 part, u64 whole, u32 scale )
{
    u64 prod;
    u32 result;

    if ( whole == 0 ) {
        return 0;
    }
    if ( part >= whole ) {
        return scale;
    }
    while ( xa_hi32( whole ) != 0 ) {
        part = half64( part );
        whole = half64( whole );
    }
    prod = xa_mul32( xa_lo32( part ), scale );
    xa_div64( &prod, xa_lo32( whole ) );
    result = xa_lo32( prod );
    return result >= scale && scale > 0 ? scale - 1 : result;
}

/* DiskStat's FormatSize - bytes under a kilobyte, kilobytes under a
   megabyte, then megabytes and a tenth - with the digits grouped, and
   gigabytes and terabytes past 9,999.9 of the one before, which a DOS
   drive never reached */
void format_size( u64 bytes, char *buf )
{
    static const char *const unit[] = { "MB", "GB", "TB" };
    char whole_text[24];                /* ten digits and three two-byte separators */
    u64 whole;
    u32 rem, tenth;
    int at;

    country_init();
    if ( xa_hi32( bytes ) == 0 && xa_lo32( bytes ) < 1024 ) {
        group_u32( xa_lo32( bytes ), whole_text );
        sprintf( buf, "%s bytes", whole_text );
        return;
    }
    if ( xa_hi32( bytes ) == 0 && xa_lo32( bytes ) < 1048576UL ) {
        group_u32( xa_lo32( bytes ) / 1024, whole_text );
        sprintf( buf, "%s KB", whole_text );
        return;
    }
    whole = bytes;
    rem = xa_div64( &whole, 1048576UL );
    tenth = rem * 10 / 1048576UL;       /* rem is under 2^20: no overflow */
    for ( at = 0; at < 2 && (xa_hi32( whole ) != 0 || xa_lo32( whole ) >= 10000); at++ ) {
        rem = xa_div64( &whole, 1024 );
        tenth = rem * 10 / 1024;
    }
    group_u32( xa_lo32( whole ), whole_text );
    sprintf( buf, "%s%s%u %s", whole_text, dec_sep, tenth, unit[at] );
}

/* "12.3%" of "whole", as DiskStat worked it out: tenths, rounded down */
void format_pct( u64 part, u64 whole, char *buf )
{
    u32 pct10 = per_whole( part, whole, 1000 );

    country_init();
    sprintf( buf, "%u%s%u%%", pct10 / 10, dec_sep, pct10 % 10 );
}

/* a name cut to "room" characters, its end shown as "..." when it was
   cut; "dst" holds room + 1 */
void shorten_name( char *dst, int room, const char *name )
{
    int len = (int)strlen( name );

    if ( room < 1 ) {
        dst[0] = 0;
        return;
    }
    if ( len <= room ) {
        strcpy( dst, name );
        return;
    }
    if ( room < 4 ) {
        memcpy( dst, name, (size_t)room );
        dst[room] = 0;
        return;
    }
    memcpy( dst, name, (size_t)(room - 3) );
    strcpy( dst + room - 3, "..." );
}

/* ------------------------------------------------------------------ */
/* the search (XARC's DIRS.C)                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int  lfn;
    u32  handle;
    u8   rec[320];
    char name[260];
    u8   attr;
    u64  size;
} FIND;

static void find_fill( FIND *fd )
{
    if ( fd->lfn ) {
        strncpy( fd->name, (const char *)fd->rec + 0x2C, sizeof( fd->name ) - 1 );
        fd->attr = (u8)RD32( fd->rec );
        fd->size = xa_make64( RD32( fd->rec + 0x1C ), RD32( fd->rec + 0x20 ) );
    } else {
        strncpy( fd->name, (const char *)fd->rec + 0x1E, 13 );
        fd->attr = fd->rec[0x15];
        fd->size = xa_make64( 0, RD32( fd->rec + 0x1A ) );
    }
    fd->name[sizeof( fd->name ) - 1] = 0;
}

/* 4Eh and 4Fh with the record as the DTA, so a search's place is its
   own record's and not the program's one DTA */
static int classic_find( FIND *fd, const char *pattern, int attrs )
{
    REGS regs;

    xa_clear( &regs );
    regs.edx = (u32)fd->rec;
    xa_dos( &regs, 0x1A00 );
    xa_clear( &regs );
    if ( pattern ) {
        regs.edx = (u32)pattern;
        regs.ecx = (u32)attrs;
        return xa_dos( &regs, 0x4E00 );
    }
    return xa_dos( &regs, 0x4F00 );
}

static void find_close( FIND *fd )
{
    REGS regs;

    if ( fd->lfn && fd->handle != 0xFFFFFFFFUL ) {
        xa_clear( &regs );
        regs.ebx = fd->handle;
        xa_dos( &regs, 0x71A1 );
    }
    fd->handle = 0xFFFFFFFFUL;
}

/* 0 = found */
static int find_first( const char *pattern, FIND *fd )
{
    REGS regs;
    u32 before = crit_hits;
    int rc = -1;

    fd->lfn = 0;
    fd->handle = 0xFFFFFFFFUL;
    if ( xa_lfn ) {
        xa_clear( &regs );
        regs.edx = (u32)pattern;
        regs.edi = (u32)fd->rec;
        regs.ecx = FIND_ATTR;
        regs.esi = 1;
        regs.flags = 1;
        rc = xa_dos( &regs, 0x714E );
        if ( rc == 0x7100 ) {
            xa_lfn = 0;
            rc = -1;
        } else if ( rc == 0 ) {
            fd->lfn = 1;
            fd->handle = AX( regs );
        }
    }
    if ( rc < 0 ) {
        rc = classic_find( fd, pattern, FIND_ATTR );
    }
    if ( crit_hits != before ) {
        find_close( fd );
        return 1;
    }
    if ( rc ) {
        return 1;
    }
    find_fill( fd );
    return 0;
}

static int find_next( FIND *fd )
{
    REGS regs;
    u32 before = crit_hits;
    int rc;

    if ( fd->lfn ) {
        xa_clear( &regs );
        regs.ebx = fd->handle;
        regs.edi = (u32)fd->rec;
        regs.esi = 1;
        rc = xa_dos( &regs, 0x714F );
    } else {
        rc = classic_find( fd, NULL, 0 );
    }
    if ( rc || crit_hits != before ) {
        return 1;
    }
    find_fill( fd );
    return 0;
}

static int is_dot( const char *name )
{
    return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

/* ------------------------------------------------------------------ */
/* one folder: its files added up, its subfolders' names kept          */
/* ------------------------------------------------------------------ */

typedef struct {
    char *names;                /* one after another, each with its NUL */
    u32   used, cap;
    u32   next;                 /* where the next one to go into starts */
    int   len;                  /* the length of this folder's path */
} LEVEL;

static FIND finder;             /* one search at a time: see the top */

static void level_add( LEVEL *lv, const char *name )
{
    u32 len = (u32)strlen( name ) + 1;

    if ( lv->used + len > lv->cap ) {
        u32 cap = lv->cap ? lv->cap * 2 : 512;
        char *grown;

        while ( cap < lv->used + len ) {
            cap *= 2;
        }
        grown = (char *)realloc( lv->names, cap );
        if ( grown == NULL ) {
            scan_cut = 1;
            return;
        }
        lv->names = grown;
        lv->cap = cap;
    }
    memcpy( lv->names + lv->used, name, len );
    lv->used += len;
}

/* "path" is a folder ("C:" for the root): its files' bytes onto *total,
   its subfolders into "lv" (which may be NULL: then only whether there
   is one, in the return) */
static int read_dir( const char *path, LEVEL *lv, u64 *total )
{
    char pattern[SCAN_PATH + 8];
    int len = (int)strlen( path ), kids = 0;

    if ( lv ) {
        lv->used = lv->next = 0;
        lv->len = len;
    }
    if ( len + 4 >= SCAN_PATH ) {
        scan_cut = 1;
        return 0;
    }
    memcpy( pattern, path, (size_t)len );
    strcpy( pattern + len, "\\*.*" );
    if ( find_first( pattern, &finder ) != 0 ) {
        return 0;
    }
    do {
        if ( finder.attr & ATTR_DIR ) {
            if ( !is_dot( finder.name ) ) {
                kids = 1;
                if ( lv ) {
                    level_add( lv, finder.name );
                } else {
                    break;
                }
            }
        } else if ( total ) {
            *total += finder.size;
        }
    } while ( find_next( &finder ) == 0 );
    find_close( &finder );
    return kids;
}

static LEVEL levels[SCAN_DEPTH];

/* The bytes in every file under "start", and (*kids) whether it has a
   subfolder.  A depth-first walk: each level holds its folder's
   subfolder names and how far through them it is. */
static u64 scan_tree( const char *start, int *kids )
{
    char path[SCAN_PATH + 2];
    u64 total = 0;
    int top = 0, len;
    const char *name;
    LEVEL *lv;

    strncpy( path, start, SCAN_PATH - 1 );
    path[SCAN_PATH - 1] = 0;
    *kids = read_dir( path, &levels[0], &total );
    while ( top >= 0 ) {
        lv = &levels[top];
        if ( lv->next >= lv->used ) {
            top--;
            continue;
        }
        name = lv->names + lv->next;
        len = (int)strlen( name );
        lv->next += (u32)len + 1;
        if ( top + 1 >= SCAN_DEPTH || lv->len + 1 + len >= SCAN_PATH ) {
            scan_cut = 1;
            continue;
        }
        path[lv->len] = '\\';
        strcpy( path + lv->len + 1, name );
        top++;
        read_dir( path, &levels[top], &total );
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* the drive                                                           */
/* ------------------------------------------------------------------ */

/* 7303h first - its counts are 32 bits - then 36h.  PM-DOS's record
   starts with its version word, two bytes ahead of DOS 7.1's, which
   starts with its size: a first word of 0 says which (SETUP's way) */
static void get_disk_space( int drive )
{
    REGS regs;
    u8 rec[44], *at;
    char root[4];
    u32 before = crit_hits, cluster;

    total_disk_space = free_disk_space = used_disk_space = 0;
    root[0] = (char)('A' + drive - 1);
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    memset( rec, 0, sizeof( rec ) );
    rec[0] = sizeof( rec );
    xa_clear( &regs );
    regs.edx = (u32)root;
    regs.edi = (u32)rec;
    regs.ecx = sizeof( rec );
    regs.flags = 1;
    if ( xa_dos( &regs, 0x7303 ) == 0 && crit_hits == before ) {
        at = RD16( rec ) == 0 ? rec + 2 : rec + 4;
        cluster = RD32( at ) * RD32( at + 4 );
        total_disk_space = xa_mul32( cluster, RD32( at + 12 ) );
        free_disk_space = xa_mul32( cluster, RD32( at + 8 ) );
    } else if ( crit_hits == before ) {
        xa_clear( &regs );
        regs.edx = (u32)drive;
        xa_dos( &regs, 0x3600 );
        if ( AX( regs ) != 0xFFFF && crit_hits == before ) {
            cluster = AX( regs ) * (regs.ecx & 0xFFFF);
            total_disk_space = xa_mul32( cluster, regs.edx & 0xFFFF );
            free_disk_space = xa_mul32( cluster, regs.ebx & 0xFFFF );
        }
    }
    if ( free_disk_space > total_disk_space ) {
        free_disk_space = total_disk_space;
    }
    used_disk_space = total_disk_space - free_disk_space;
}

/* 4408h asks the drive table, not the disk, so it spins nothing up:
   0Fh is "no such drive", anything else says it is there */
static int drive_exists( int drive )
{
    REGS regs;

    xa_clear( &regs );
    regs.ebx = (u32)drive;
    return xa_dos( &regs, 0x4408 ) != 0x0F;
}

/* C: and up, as DiskStat listed them: the floppies are left out, as
   the Windows DiskStat left them out */
int detect_drives( int *drives, int max )
{
    int drive, count = 0;

    for ( drive = 3; drive <= 26 && count < max; drive++ ) {
        if ( drive_exists( drive ) ) {
            drives[count++] = drive;
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* the rows                                                            */
/* ------------------------------------------------------------------ */

static int nodes_room( int more )
{
    NODE *grown;
    int cap;

    if ( tree_count + more <= node_cap ) {
        return 1;
    }
    cap = node_cap ? node_cap : 256;
    while ( cap < tree_count + more ) {
        cap *= 2;
    }
    grown = (NODE *)realloc( tree_nodes, (size_t)cap * sizeof( NODE ) );
    if ( grown == NULL ) {
        return 0;
    }
    tree_nodes = grown;
    node_cap = cap;
    return 1;
}

static void free_rows( int first, int count )
{
    int at;

    for ( at = first; at < first + count; at++ ) {
        free( tree_nodes[at].name );
        tree_nodes[at].name = NULL;
    }
}

/* "X:\a\b" for a row: its ancestors are the nearest rows above it at
   each shallower depth.  0 when it would not fit. */
static int node_path( int idx, char *path )
{
    int chain[SCAN_DEPTH], want = tree_nodes[idx].depth, at, level, len = 2;

    if ( want >= SCAN_DEPTH ) {
        return 0;
    }
    chain[want] = idx;
    for ( at = idx - 1; at >= 0 && want > 0; at-- ) {
        if ( tree_nodes[at].depth == want - 1 ) {
            chain[--want] = at;
        }
    }
    path[0] = (char)('A' + current_drive - 1);
    path[1] = ':';
    path[2] = 0;
    for ( level = 0; level <= (int)tree_nodes[idx].depth; level++ ) {
        int part = (int)strlen( tree_nodes[chain[level]].name );

        if ( len + 1 + part >= SCAN_PATH ) {
            return 0;
        }
        path[len++] = '\\';
        strcpy( path + len, tree_nodes[chain[level]].name );
        len += part;
    }
    return 1;
}

/* rows first..first+count-1 by size, the largest first; a stable
   insertion sort, so folders of one size keep the order the disk has
   them in, as DiskStat's bubble sort kept it */
static void sort_rows( int first, int count )
{
    int at, back;
    NODE hold;

    for ( at = first + 1; at < first + count; at++ ) {
        hold = tree_nodes[at];
        for ( back = at; back > first && tree_nodes[back - 1].total_size < hold.total_size; back-- ) {
            tree_nodes[back] = tree_nodes[back - 1];
        }
        tree_nodes[back] = hold;
    }
}

/* the rows made from a folder's subfolder names, sized; how many */
static int fill_rows( int insert_at, const LEVEL *names, const char *parent, u8 depth, int count )
{
    char path[SCAN_PATH + 2];
    const char *name;
    int done = 0, kids, plen = (int)strlen( parent );
    u32 at = 0;

    while ( done < count && at < names->used ) {
        NODE *node = &tree_nodes[insert_at + done];

        name = names->names + at;
        at += (u32)strlen( name ) + 1;
        node->name = strdup( name );
        if ( node->name == NULL ) {
            scan_cut = 1;
            break;
        }
        node->depth = depth;
        node->flags = 0;
        node->total_size = 0;
        if ( plen + 1 + (int)strlen( name ) < SCAN_PATH ) {
            memcpy( path, parent, (size_t)plen );
            path[plen] = '\\';
            strcpy( path + plen + 1, name );
            node->total_size = scan_tree( path, &kids );
            if ( kids ) {
                node->flags |= NODE_HAS_CHILDREN;
            }
        } else {
            scan_cut = 1;
        }
        done++;
    }
    return done;
}

static int count_names( const LEVEL *names )
{
    u32 at = 0;
    int count = 0;

    while ( at < names->used ) {
        at += (u32)strlen( names->names + at ) + 1;
        count++;
    }
    return count;
}

/* DiskStat's ScanDrive: the free space, the root's folders as rows,
   each sized, the largest first */
static void scan_drive( void )
{
    LEVEL root;
    char root_path[4];
    int count;

    if ( in_scan ) {
        return;
    }
    in_scan = 1;
    data_valid = 0;
    free_rows( 0, tree_count );
    tree_count = 0;
    selected_node = -1;
    scan_cut = 0;

    get_disk_space( current_drive );

    root_path[0] = (char)('A' + current_drive - 1);
    root_path[1] = ':';
    root_path[2] = 0;
    memset( &root, 0, sizeof( root ) );
    read_dir( root_path, &root, NULL );
    count = count_names( &root );
    while ( count > 0 && !nodes_room( count ) ) {
        count /= 2;                     /* what memory there is */
        scan_cut = 1;
    }
    tree_count = fill_rows( 0, &root, root_path, 0, count );
    free( root.names );
    sort_rows( 0, tree_count );

    data_valid = 1;
    in_scan = 0;
}

/* DiskStat's ExpandNode: a folder's subfolders as rows under it */
static void expand_node( int idx )
{
    char path[SCAN_PATH + 2];
    LEVEL names;
    int count, made, insert_at;

    if ( idx < 0 || idx >= tree_count ) {
        return;
    }
    if ( (tree_nodes[idx].flags & NODE_EXPANDED) || !(tree_nodes[idx].flags & NODE_HAS_CHILDREN) ) {
        return;
    }
    if ( !node_path( idx, path ) ) {
        return;
    }
    memset( &names, 0, sizeof( names ) );
    read_dir( path, &names, NULL );
    count = count_names( &names );
    if ( count == 0 ) {
        tree_nodes[idx].flags &= (u8)~NODE_HAS_CHILDREN;
        free( names.names );
        return;
    }
    while ( count > 0 && !nodes_room( count ) ) {
        count /= 2;
    }
    if ( count == 0 ) {
        free( names.names );
        return;
    }

    /* room for the children, straight under their folder */
    insert_at = idx + 1;
    if ( insert_at < tree_count ) {
        memmove( &tree_nodes[insert_at + count], &tree_nodes[insert_at],
                 sizeof( NODE ) * (size_t)(tree_count - insert_at) );
    }
    made = fill_rows( insert_at, &names, path, (u8)(tree_nodes[idx].depth + 1), count );
    free( names.names );
    if ( made < count ) {
        memmove( &tree_nodes[insert_at + made], &tree_nodes[insert_at + count],
                 sizeof( NODE ) * (size_t)(tree_count - insert_at) );
    }
    tree_count += made;
    if ( selected_node >= insert_at ) {
        selected_node += made;
    }
    sort_rows( insert_at, made );
    tree_nodes[idx].flags |= NODE_EXPANDED;
}

/* DiskStat's CollapseNode: a folder's rows below it go; the cursor, if
   it was on one of them, goes back to the folder */
void collapse_node( int idx )
{
    int end_idx, remove_count;
    u8 depth;

    if ( idx < 0 || idx >= tree_count || !(tree_nodes[idx].flags & NODE_EXPANDED) ) {
        return;
    }
    depth = tree_nodes[idx].depth;
    end_idx = idx + 1;
    while ( end_idx < tree_count && tree_nodes[end_idx].depth > depth ) {
        end_idx++;
    }
    remove_count = end_idx - (idx + 1);
    tree_nodes[idx].flags &= (u8)~NODE_EXPANDED;
    if ( remove_count <= 0 ) {
        return;
    }
    if ( selected_node > idx && selected_node < end_idx ) {
        selected_node = idx;
    } else if ( selected_node >= end_idx ) {
        selected_node -= remove_count;
    }
    free_rows( idx + 1, remove_count );
    if ( end_idx < tree_count ) {
        memmove( &tree_nodes[idx + 1], &tree_nodes[end_idx],
                 sizeof( NODE ) * (size_t)(tree_count - end_idx) );
    }
    tree_count -= remove_count;
}

/* ------------------------------------------------------------------ */
/* a drive that cannot be read (DiskStat's DOS half)                   */
/* ------------------------------------------------------------------ */

/* 0 when the drive could not be read and the answer was Cancel: asked
   BEFORE a scan, which throws the old figures away.  A drive the kernel
   does not have at all is "ready": there is nothing to wait for, and
   the scan finds nothing, as it always has. */
int drive_ready( int drive )
{
    REGS regs;

    for ( ;; ) {
        crit_clear();
        xa_clear( &regs );
        regs.edx = (u32)drive;
        xa_dos( &regs, 0x3600 );
        if ( !crit_hit() ) {
            return 1;
        }
        if ( !crit_ask() ) {
            return 0;
        }
    }
}

/* scan_drive, and again for as long as the answer is Retry.  0 when an
   error was left standing or a folder could not be reached: the figures
   are short by what could not be read. */
int scan_drive_ask( void )
{
    for ( ;; ) {
        crit_clear();
        scan_drive();
        if ( !crit_hit() ) {
            return !scan_cut;
        }
        if ( !crit_ask() ) {
            return 0;
        }
    }
}

/* expand_node, and again for as long as the answer is Retry.  A folder
   that could not be read is put back the way it was, closed and with
   its [+]: expand_node takes the marker off a folder it found nothing
   in, and this one it never got to look in. */
void expand_node_ask( int idx )
{
    for ( ;; ) {
        crit_clear();
        expand_node( idx );
        if ( !crit_hit() ) {
            return;
        }
        collapse_node( idx );
        tree_nodes[idx].flags |= NODE_HAS_CHILDREN;
        if ( !crit_ask() ) {
            return;
        }
    }
}

/* the model of a drive with nothing to show, without touching the
   drive: what the panes draw when the first drive asked for is empty */
void clear_scan( void )
{
    free_rows( 0, tree_count );
    tree_count = 0;
    selected_node = -1;
    total_disk_space = free_disk_space = used_disk_space = 0;
    data_valid = 1;
}
