/*
 * DIRS.C - the disk as XARC browses it: a directory search a disk error
 * cannot lie to, whether a drive is there, a directory as a list of
 * rows (DOSForms' DIRSCAN.CPP) and the drives and folders as a tree
 * (DIRTREE.CPP).
 *
 * LONG NAMES.  The DOS program lived in eight-and-three; PM-DOS has long
 * names, and the 71h search gives them, so a row's name is whatever the
 * search said and may be 255 characters.  Names go one after another in
 * a pool that grows, and there may be 16,384 rows to a directory where
 * the DOS program stopped at 1,024.  When CONFIG.SYS has LFN=OFF the
 * classic search is used, and the names are the 8.3 ones.
 *
 * A SEARCH THAT MET A CRITICAL ERROR HAS FAILED, whatever came back.  The
 * handler (DOSINT.ASM) fails the call and counts it, and a search is
 * more than one read: find_first and find_next look at the count before
 * and after, as the toolkit's DfFindFirst did, and the caller asks the
 * question (crit_ask, in UI.C) and tries again.
 *
 * HIDDEN FILES ARE NOT LISTED: the attribute mask leaves the hidden bit
 * out, and the kernel returns a file only when every bit it carries is
 * one the mask allows - the DOS rule.  The things wearing that bit are
 * the system's, and this program has no business with them.
 *
 * NAVIGATION MOVES THE PROCESS'S CURRENT DIRECTORY AND DRIVE, as it did:
 * "where am I?" then has one answer, getcwd.  Above a drive's root is
 * the DRIVE LIST, the only place drives appear.
 */
#include "xarc.h"

#define DS_MAX_ENTRIES  16384
#define DS_FIND_ATTR    0x15            /* folders, system, read-only */

/* ------------------------------------------------------------------ */
/* the search                                                          */
/* ------------------------------------------------------------------ */

static void find_fill( FIND *fd )
{
    u32 stamp;

    if ( fd->lfn ) {
        strncpy( fd->name, (const char *)fd->rec + 0x2C, sizeof( fd->name ) - 1 );
        fd->attr = (u8)RD32( fd->rec );
        stamp = RD32( fd->rec + 0x14 );
        fd->size = RD32( fd->rec + 0x20 );
    } else {
        strncpy( fd->name, (const char *)fd->rec + 0x1E, 13 );
        fd->attr = fd->rec[0x15];
        stamp = ((u32)RD16( fd->rec + 0x18 ) << 16) | RD16( fd->rec + 0x16 );
        fd->size = RD32( fd->rec + 0x1A );
    }
    fd->name[sizeof( fd->name ) - 1] = 0;
    fd->date = (u16)(stamp >> 16);
    fd->time = (u16)(stamp & 0xFFFF);
}

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

void find_close( FIND *fd )
{
    REGS regs;

    if ( fd->lfn && fd->handle != 0xFFFFFFFFUL ) {
        xa_clear( &regs );
        regs.ebx = fd->handle;
        xa_dos( &regs, 0x71A1 );
    }
    fd->handle = 0xFFFFFFFFUL;
}

int find_first( const char *pattern, int attrs, FIND *fd )
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
        regs.ecx = (u32)attrs;
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
        rc = classic_find( fd, pattern, attrs );
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

int find_next( FIND *fd )
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

/* 4408h asks the drive table, not the disk, so it spins nothing up:
   0Fh is "no such drive", anything else says it is there */
int drive_exists( int drive )
{
    REGS regs;
    int rc;

    xa_clear( &regs );
    regs.ebx = (u32)drive;
    rc = xa_dos( &regs, 0x4408 );
    return rc != 0x0F;
}

/* ------------------------------------------------------------------ */
/* a directory as rows (DIRSCAN.CPP)                                   */
/* ------------------------------------------------------------------ */

static const DIRSCAN *sorting;

static int entry_cmp( const void *one, const void *two )
{
    const DSENT *lhs = (const DSENT *)one, *rhs = (const DSENT *)two;

    if ( lhs->kind != rhs->kind ) {
        return lhs->kind - rhs->kind;
    }
    return stricmp( sorting->pool + lhs->name, sorting->pool + rhs->name );
}

static void sort_entries( DIRSCAN *ds )
{
    sorting = ds;
    qsort( ds->ents, (size_t)ds->count, sizeof( DSENT ), entry_cmp );
}

void dirscan_init( DIRSCAN *ds )
{
    memset( ds, 0, sizeof( *ds ) );
}

void dirscan_free( DIRSCAN *ds )
{
    free( ds->ents );
    free( ds->pool );
    memset( ds, 0, sizeof( *ds ) );
}

/* room for one more row and its name; 0 when there is none */
static DSENT *add_entry( DIRSCAN *ds, const char *name, int kind )
{
    u32 len = (u32)strlen( name ) + 1;
    DSENT *ent;

    if ( ds->count >= DS_MAX_ENTRIES ) {
        return NULL;
    }
    if ( ds->count >= ds->cap ) {
        int cap = ds->cap ? ds->cap * 2 : 256;
        DSENT *grown = (DSENT *)realloc( ds->ents, (size_t)cap * sizeof( DSENT ) );

        if ( grown == NULL ) {
            return NULL;
        }
        ds->ents = grown;
        ds->cap = cap;
    }
    if ( ds->pool_used + len > ds->pool_cap ) {
        u32 cap = ds->pool_cap ? ds->pool_cap * 2 : 8192;
        char *grown;

        while ( cap < ds->pool_used + len ) {
            cap *= 2;
        }
        grown = (char *)realloc( ds->pool, cap );
        if ( grown == NULL ) {
            return NULL;
        }
        ds->pool = grown;
        ds->pool_cap = cap;
    }
    ent = &ds->ents[ds->count++];
    memset( ent, 0, sizeof( *ent ) );
    ent->name = ds->pool_used;
    ent->kind = kind;
    memcpy( ds->pool + ds->pool_used, name, len );
    ds->pool_used += len;
    return ent;
}

static void reset( DIRSCAN *ds )
{
    ds->count = 0;
    ds->pool_used = 0;
    ds->cut = 0;
}

/* '*' and '?' as DOS means them, over the whole name */
static int glob_match( const char *pat, const char *name )
{
    while ( *pat ) {
        if ( *pat == '*' ) {
            pat++;
            if ( !*pat ) {
                return 1;
            }
            for ( ; *name; name++ ) {
                if ( glob_match( pat, name ) ) {
                    return 1;
                }
            }
            return glob_match( pat, name );
        }
        if ( !*name ) {
            return 0;
        }
        if ( *pat != '?' && toupper( (u8)*pat ) != toupper( (u8)*name ) ) {
            return 0;
        }
        pat++;
        name++;
    }
    return *name == 0;
}

static int filter_accepts( const DIRSCAN *ds, const char *name )
{
    const char *at = ds->filter;
    char one[64];
    int len;

    if ( !ds->filter[0] ) {
        return 1;
    }
    while ( *at ) {
        for ( len = 0; *at && *at != ';' && len < (int)sizeof( one ) - 1; len++ ) {
            one[len] = *at++;
        }
        one[len] = 0;
        while ( *at == ';' ) {
            at++;
        }
        if ( len > 0 && glob_match( one, name ) ) {
            return 1;
        }
    }
    return 0;
}

static void add_parent( DIRSCAN *ds )
{
    add_entry( ds, "..", DS_PARENT );
}

static void read_entries( DIRSCAN *ds, const char *pattern )
{
    FIND fd;
    DSENT *ent;
    int kind;

    if ( find_first( pattern, DS_FIND_ATTR, &fd ) != 0 ) {
        return;
    }
    do {
        if ( fd.attr & 0x10 ) {
            if ( ds->files_only || fd.name[0] == '.' ) {
                continue;
            }
            kind = DS_DIR;
        } else {
            if ( !filter_accepts( ds, fd.name ) ) {
                continue;
            }
            kind = DS_FILE;
        }
        ent = add_entry( ds, fd.name, kind );
        if ( ent == NULL ) {
            ds->cut = 1;                /* said on the status line */
            break;
        }
        ent->size = fd.size;
        ent->date = fd.date;
        ent->time = fd.time;
        ent->attr = fd.attr;
    } while ( find_next( &fd ) == 0 );
    find_close( &fd );
}

static int path_is_root( const char *path )
{
    return path[0] && path[1] == ':' && path[2] == '\\' && path[3] == 0;
}

/* The current directory, or the drives in the drive list.  A directory
   that cannot be read - the drive we stand on has gone, an empty floppy
   being the usual way - falls back to the drive list, the one view that
   is always there to go somewhere from. */
int dirscan_rescan( DIRSCAN *ds )
{
    char name[4];
    int drive;

    reset( ds );
    if ( !ds->drives && !getcwd( ds->path, sizeof( ds->path ) ) ) {
        ds->drives = 1;
    }
    if ( ds->drives ) {
        ds->path[0] = 0;
        for ( drive = 1; drive <= 26; drive++ ) {
            if ( drive_exists( drive ) ) {
                name[0] = (char)('A' + drive - 1);
                name[1] = ':';
                name[2] = 0;
                add_entry( ds, name, DS_DRIVE );
            }
        }
        sort_entries( ds );
        return ds->count;
    }
    add_parent( ds );
    read_entries( ds, "*.*" );
    sort_entries( ds );
    return ds->count;
}

/* by name, without moving: FileBox's file list */
int dirscan_path( DIRSCAN *ds, const char *dir )
{
    char pattern[DS_PATHLEN + 8];
    int len;

    reset( ds );
    ds->drives = 0;
    ds->path[0] = 0;
    if ( dir == NULL || !dir[0] ) {
        return 0;
    }
    strncpy( ds->path, dir, sizeof( ds->path ) - 1 );
    ds->path[sizeof( ds->path ) - 1] = 0;
    strcpy( pattern, ds->path );
    len = (int)strlen( pattern );
    if ( len > 0 && pattern[len - 1] != '\\' && pattern[len - 1] != '/' ) {
        pattern[len++] = '\\';
    }
    strcpy( pattern + len, "*.*" );
    if ( !ds->files_only ) {
        add_parent( ds );
    }
    read_entries( ds, pattern );
    sort_entries( ds );
    return ds->count;
}

void dirscan_empty( DIRSCAN *ds )
{
    reset( ds );
    ds->drives = 0;
    ds->path[0] = 0;
}

void dirscan_show_directory( DIRSCAN *ds )
{
    ds->drives = 0;
}

int dirscan_items( const DIRSCAN *ds )
{
    int at, items = 0;

    for ( at = 0; at < ds->count; at++ ) {
        if ( ds->ents[at].kind != DS_PARENT ) {
            items++;
        }
    }
    return items;
}

int dirscan_kind( const DIRSCAN *ds, int row )
{
    return row >= 0 && row < ds->count ? ds->ents[row].kind : DS_FILE;
}

const char *dirscan_name( const DIRSCAN *ds, int row )
{
    return row >= 0 && row < ds->count ? ds->pool + ds->ents[row].name : "";
}

/* what the list shows: "[..]"'s plain "..", "[-C-]", "[SUB]", "FOO.ZIP" */
void dirscan_disp( const DIRSCAN *ds, int row, char *buf, int buflen )
{
    const char *name = dirscan_name( ds, row );

    if ( buflen < 8 ) {
        return;
    }
    switch ( dirscan_kind( ds, row ) ) {
    case DS_DRIVE:
        sprintf( buf, "[-%c-]", name[0] );
        break;
    case DS_DIR:
        _snprintf( buf, (size_t)buflen, "[%s]", name );
        break;
    default:
        _snprintf( buf, (size_t)buflen, "%s", name );
        break;
    }
}

u32 dirscan_size( const DIRSCAN *ds, int row )
{
    return row >= 0 && row < ds->count ? ds->ents[row].size : 0;
}

int dirscan_is_dir( const DIRSCAN *ds, int row )
{
    int kind = dirscan_kind( ds, row );

    return kind == DS_PARENT || kind == DS_DRIVE || kind == DS_DIR;
}

int dirscan_is_up( const DIRSCAN *ds, int row )
{
    return dirscan_kind( ds, row ) == DS_PARENT;
}

/* "YYYY-MM-DD HH:MM", as ArcEntryDate writes it; files only */
void dirscan_date( const DIRSCAN *ds, int row, char *buf, int buflen )
{
    unsigned date, clock;

    if ( buflen <= 0 ) {
        return;
    }
    buf[0] = 0;
    if ( row < 0 || row >= ds->count || ds->ents[row].kind != DS_FILE || buflen < 17 ) {
        return;
    }
    date = ds->ents[row].date;
    clock = ds->ents[row].time;
    if ( !date ) {
        return;
    }
    sprintf( buf, "%04u-%02u-%02u %02u:%02u", 1980 + ((date >> 9) & 0x7F), (date >> 5) & 0x0F,
             date & 0x1F, (clock >> 11) & 0x1F, (clock >> 5) & 0x3F );
}

void dirscan_attr( const DIRSCAN *ds, int row, char *buf, int buflen )
{
    int used = 0;
    u8 attr;

    if ( buflen <= 0 ) {
        return;
    }
    buf[0] = 0;
    if ( row < 0 || row >= ds->count ) {
        return;
    }
    attr = ds->ents[row].attr;
    if ( (attr & 0x10) && used < buflen - 1 ) buf[used++] = 'D';
    if ( (attr & 0x01) && used < buflen - 1 ) buf[used++] = 'R';
    if ( (attr & 0x02) && used < buflen - 1 ) buf[used++] = 'H';
    if ( (attr & 0x04) && used < buflen - 1 ) buf[used++] = 'S';
    if ( (attr & 0x20) && used < buflen - 1 ) buf[used++] = 'A';
    buf[used] = 0;
}

void dirscan_full( const DIRSCAN *ds, int row, char *dst, int size )
{
    const char *name = dirscan_name( ds, row );
    int used = 0, at;

    if ( size <= 0 ) {
        return;
    }
    dst[0] = 0;
    if ( row < 0 || row >= ds->count ) {
        return;
    }
    if ( ds->drives ) {
        for ( ; name[used] && used < size - 1; used++ ) {
            dst[used] = name[used];
        }
        dst[used] = 0;
        return;
    }
    for ( ; ds->path[used] && used < size - 2; used++ ) {
        dst[used] = ds->path[used];
    }
    /* never two separators: real DOS reads "\\" as a network name */
    if ( used > 0 && dst[used - 1] != '\\' && dst[used - 1] != '/' ) {
        dst[used++] = '\\';
    }
    for ( at = 0; name[at] && used < size - 1; at++ ) {
        dst[used++] = name[at];
    }
    dst[used] = 0;
}

/* Into row "row": the way up, a drive, or a folder.  1 when the view
   changed - which is asked of where we ARE afterwards, not of what the
   calls said: an empty floppy drive is changed to and then cannot say
   where it is. */
int dirscan_enter( DIRSCAN *ds, int row )
{
    char before[DS_PATHLEN], probe[DS_PATHLEN], name[DS_PATHLEN];
    int was_drives = ds->drives, was_drive;

    if ( row < 0 || row >= ds->count || !dirscan_is_dir( ds, row ) ) {
        return 0;
    }
    if ( !getcwd( before, sizeof( before ) ) ) {
        before[0] = 0;
    }
    strncpy( name, dirscan_name( ds, row ), sizeof( name ) - 1 );
    name[sizeof( name ) - 1] = 0;
    switch ( ds->ents[row].kind ) {
    case DS_PARENT:
        if ( path_is_root( ds->path ) ) {
            ds->drives = 1;
        } else {
            chdir( ".." );
        }
        break;
    case DS_DRIVE:
        was_drive = _getdrive();
        _chdrive( toupper( (u8)name[0] ) - 'A' + 1 );
        if ( !getcwd( probe, sizeof( probe ) ) ) {
            _chdrive( was_drive );
            break;
        }
        ds->drives = 0;
        break;
    default:
        chdir( name );
        break;
    }
    dirscan_rescan( ds );
    return ds->drives != was_drives || strcmp( ds->path, before ) != 0;
}

/* up one: a folder to its parent, a root to the drive list, and the
   drive list nowhere - it is the top */
int dirscan_up( DIRSCAN *ds )
{
    char before[DS_PATHLEN];

    if ( ds->drives ) {
        return 0;
    }
    if ( !getcwd( before, sizeof( before ) ) ) {
        before[0] = 0;
    }
    if ( path_is_root( before ) ) {
        ds->drives = 1;
        dirscan_rescan( ds );
        return 1;
    }
    chdir( ".." );
    dirscan_rescan( ds );
    return strcmp( ds->path, before ) != 0;
}

/* ------------------------------------------------------------------ */
/* the drives and folders as a tree (DIRTREE.CPP)                      */
/* ------------------------------------------------------------------ */

static int is_sep( char ch )
{
    return ch == '\\' || ch == '/';
}

static int node_cmp( const void *one, const void *two )
{
    return stricmp( ((const DTNODE *)one)->name, ((const DTNODE *)two)->name );
}

static int make_pattern( char *dst, int size, const char *dir )
{
    int len = (int)strlen( dir );

    if ( len + 5 > size ) {
        return 0;
    }
    strcpy( dst, dir );
    if ( len > 0 && !is_sep( dst[len - 1] ) ) {
        dst[len++] = '\\';
    }
    strcpy( dst + len, "*.*" );
    return 1;
}

/* is there a folder inside "dir"?  It stops at the first. */
static int has_folders( const char *dir )
{
    char pattern[DS_PATHLEN + 8];
    FIND fd;
    int found = 0;

    if ( !make_pattern( pattern, sizeof( pattern ), dir ) ) {
        return 0;
    }
    if ( find_first( pattern, DS_FIND_ATTR, &fd ) != 0 ) {
        return 0;
    }
    do {
        if ( (fd.attr & 0x10) && fd.name[0] != '.' ) {
            found = 1;
            break;
        }
    } while ( find_next( &fd ) == 0 );
    find_close( &fd );
    return found;
}

void dirtree_init( DIRTREE *dt )
{
    dt->count = 0;
    dt->nodes = (DTNODE *)malloc( (size_t)DT_MAX_NODES * sizeof( DTNODE ) );
}

void dirtree_free( DIRTREE *dt )
{
    free( dt->nodes );
    dt->nodes = NULL;
    dt->count = 0;
}

/* the drives, every one shut, and none read */
int dirtree_build( DIRTREE *dt )
{
    int drive;

    dt->count = 0;
    if ( dt->nodes == NULL ) {
        return 0;
    }
    for ( drive = 1; drive <= 26 && dt->count < DT_MAX_NODES; drive++ ) {
        if ( !drive_exists( drive ) ) {
            continue;
        }
        dt->nodes[dt->count].name[0] = (char)('A' + drive - 1);
        dt->nodes[dt->count].name[1] = ':';
        dt->nodes[dt->count].name[2] = 0;
        dt->nodes[dt->count].depth = 0;
        dt->nodes[dt->count].flags = DT_DRIVE | DT_KIDS;   /* a guess: see the header */
        dt->count++;
    }
    return dt->count;
}

/* "C:\" for a drive, "C:\GAMES\DOOM" for a folder: a row's ancestors are
   the nearest rows above it at each shallower depth */
void dirtree_path( const DIRTREE *dt, int row, char *dst, int size )
{
    int chain[DT_MAX_DEPTH], want, at, level, used = 0;
    const char *part;

    if ( size <= 0 ) {
        return;
    }
    dst[0] = 0;
    if ( row < 0 || row >= dt->count ) {
        return;
    }
    want = dt->nodes[row].depth;
    if ( want >= DT_MAX_DEPTH ) {
        return;
    }
    chain[want] = row;
    for ( at = row - 1; at >= 0 && want > 0; at-- ) {
        if ( dt->nodes[at].depth == want - 1 ) {
            chain[--want] = at;
        }
    }
    if ( want != 0 ) {
        return;
    }
    for ( level = 0; level <= dt->nodes[row].depth; level++ ) {
        part = dt->nodes[chain[level]].name;
        if ( level > 1 && used < size - 1 ) {
            dst[used++] = '\\';
        }
        for ( at = 0; part[at] && used < size - 1; at++ ) {
            dst[used++] = part[at];
        }
        if ( level == 0 && used < size - 1 ) {
            dst[used++] = '\\';
        }
    }
    dst[used] = 0;
}

/* the folders inside "row", put straight after it, sorted, each looked
   into far enough to say whether it has folders of its own */
static int dirtree_scan( DIRTREE *dt, int row )
{
    char path[DS_PATHLEN], pattern[DS_PATHLEN + 8];
    FIND fd;
    DTNODE *kid;
    int below, room, base, kids = 0, at;

    if ( dt->nodes == NULL || row < 0 || row >= dt->count
         || dt->nodes[row].depth + 1 >= DT_MAX_DEPTH ) {
        return 0;
    }
    dirtree_path( dt, row, path, sizeof( path ) );
    if ( !path[0] || !make_pattern( pattern, sizeof( pattern ), path ) ) {
        return 0;
    }
    below = dt->count - (row + 1);
    room = DT_MAX_NODES - dt->count;
    if ( room < 1 ) {
        return -1;                      /* the table is full, not the folder empty */
    }
    if ( below > 0 ) {
        memmove( dt->nodes + DT_MAX_NODES - below, dt->nodes + row + 1,
                 (size_t)below * sizeof( DTNODE ) );
    }
    kid = dt->nodes + row + 1;
    if ( find_first( pattern, DS_FIND_ATTR, &fd ) == 0 ) {
        do {
            if ( !(fd.attr & 0x10) || fd.name[0] == '.' ) {
                continue;
            }
            if ( kids >= room ) {
                break;
            }
            strncpy( kid[kids].name, fd.name, sizeof( kid[kids].name ) - 1 );
            kid[kids].name[sizeof( kid[kids].name ) - 1] = 0;
            kid[kids].depth = (u8)(dt->nodes[row].depth + 1);
            kid[kids].flags = 0;
            kids++;
        } while ( find_next( &fd ) == 0 );
        find_close( &fd );
    }
    if ( kids > 1 ) {
        qsort( kid, (size_t)kids, sizeof( DTNODE ), node_cmp );
    }
    /* after the search has finished: two at once is not something to
       ask of a DOS */
    base = (int)strlen( path );
    if ( base > 0 && !is_sep( path[base - 1] ) ) {
        path[base++] = '\\';
    }
    for ( at = 0; at < kids; at++ ) {
        if ( base + (int)strlen( kid[at].name ) < (int)sizeof( path ) ) {
            strcpy( path + base, kid[at].name );
            if ( has_folders( path ) ) {
                kid[at].flags |= DT_KIDS;
            }
        }
    }
    if ( below > 0 ) {
        memmove( kid + kids, dt->nodes + DT_MAX_NODES - below, (size_t)below * sizeof( DTNODE ) );
    }
    dt->count += kids;
    return kids;
}

/* Opening a folder that turns out to be empty takes its [+] away; a
   drive keeps its [+] whatever was found, since nothing found is most
   often no disk in it, and the user has to be able to try again. */
int dirtree_open( DIRTREE *dt, int row )
{
    int got;

    if ( row < 0 || row >= dt->count || (dt->nodes[row].flags & DT_OPEN)
         || !(dt->nodes[row].flags & DT_KIDS) ) {
        return dt->count;
    }
    /* -1: no room in the table just now - the [+] stays, for when other
       branches have been shut */
    got = dirtree_scan( dt, row );
    if ( got > 0 ) {
        dt->nodes[row].flags |= DT_OPEN;
    } else if ( got == 0 && !(dt->nodes[row].flags & DT_DRIVE) ) {
        dt->nodes[row].flags &= (u8)~DT_KIDS;
    }
    return dt->count;
}

/* shutting forgets what was under it: the rows held are the rows shown */
int dirtree_close( DIRTREE *dt, int row )
{
    int end, gone;

    if ( row < 0 || row >= dt->count || !(dt->nodes[row].flags & DT_OPEN) ) {
        return dt->count;
    }
    for ( end = row + 1; end < dt->count && dt->nodes[end].depth > dt->nodes[row].depth; end++ ) {
    }
    gone = end - (row + 1);
    if ( gone > 0 && end < dt->count ) {
        memmove( dt->nodes + row + 1, dt->nodes + end, (size_t)(dt->count - end) * sizeof( DTNODE ) );
    }
    dt->count -= gone;
    dt->nodes[row].flags &= (u8)~DT_OPEN;
    return dt->count;
}

static int find_child( const DIRTREE *dt, int row, const char *name )
{
    int at;

    for ( at = row + 1; at < dt->count && dt->nodes[at].depth > dt->nodes[row].depth; at++ ) {
        if ( dt->nodes[at].depth == dt->nodes[row].depth + 1
             && stricmp( dt->nodes[at].name, name ) == 0 ) {
            return at;
        }
    }
    return -1;
}

/* Open the tree down to "path" and answer the row it is on.  A path that
   is not there all the way down - a folder about to be made - gets as
   far as the part that is.  -1 if not even the drive is there. */
int dirtree_reveal( DIRTREE *dt, const char *path )
{
    char full[DS_PATHLEN], part[256];
    const char *at;
    int row = -1, i, len, child;

    if ( path == NULL || !path[0] ) {
        return -1;
    }
    if ( !_fullpath( full, path, sizeof( full ) ) ) {
        strncpy( full, path, sizeof( full ) - 1 );
        full[sizeof( full ) - 1] = 0;
    }
    if ( !full[0] || full[1] != ':' ) {
        return -1;
    }
    for ( i = 0; i < dt->count; i++ ) {
        if ( (dt->nodes[i].flags & DT_DRIVE)
             && toupper( (u8)dt->nodes[i].name[0] ) == toupper( (u8)full[0] ) ) {
            row = i;
            break;
        }
    }
    if ( row < 0 ) {
        return -1;
    }
    at = full + 2;
    for ( ;; ) {
        while ( is_sep( *at ) ) {
            at++;
        }
        if ( !*at ) {
            break;
        }
        for ( len = 0; at[len] && !is_sep( at[len] ); len++ ) {
        }
        if ( len >= (int)sizeof( part ) ) {
            break;
        }
        memcpy( part, at, (size_t)len );
        part[len] = 0;
        at += len;
        dirtree_open( dt, row );
        child = find_child( dt, row, part );
        if ( child < 0 ) {
            break;
        }
        row = child;
    }
    return row;
}
