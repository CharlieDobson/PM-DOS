/*
 * ARCVIEW.C - a browsable folder view over an archive's flat entry list:
 * the DOS build's ARCVIEW.CPP in C.
 *
 * Every backend hands the front end ONE flat list of entries whose names
 * are whole stored paths.  This keeps a current folder inside the archive
 * and works out what is directly in it: the files whose stored path has
 * no separator left, and one row per sub-folder, whether or not the
 * archive stored an entry for it (zip usually does, 7z often does not).
 * Flat mode is the old listing, one row per entry with its whole path.
 *
 * Stored names use '/' or '\', sometimes both in one archive, and every
 * comparison treats them as one; the current folder is held with '\'.
 * A row's name points INTO the archive's own entry table, so the view is
 * only good while that archive is open: view_attach( NULL ) first.
 */
#include "xarc.h"

typedef struct {
    const char *name;           /* into the archive's table, NOT terminated */
    int         len;            /* the leaf's length within it */
    int         index;          /* the entry, or -1 for a folder nobody stored */
    int         is_dir;
    int         is_up;          /* the ".." row */
} VROW;

static ArcFile *arc;
static VROW    *rows;
static int      nrows, cap;
static int      folder_mode = 1;
static char     cwd[SZ_MAX_NAME];   /* "" at the root, else ends with '\' */
static int      cwd_len;

static int fold( int ch )
{
    if ( ch == '/' ) {
        return '\\';
    }
    return toupper( ch );
}

static int is_sep( int ch )
{
    return ch == '/' || ch == '\\';
}

/* a stored name as the folders see it: "/DOCS/X" is in DOCS, like "DOCS/X" */
static const char *past_seps( const char *name )
{
    while ( is_sep( (u8)*name ) ) {
        name++;
    }
    return name;
}

static int has_prefix( const char *name, const char *prefix, int len )
{
    int at;

    for ( at = 0; at < len; at++ ) {
        if ( !name[at] || fold( (u8)name[at] ) != fold( (u8)prefix[at] ) ) {
            return 0;
        }
    }
    return 1;
}

static int leaf_cmp( const char *one, int one_len, const char *two, int two_len )
{
    int at, len = one_len < two_len ? one_len : two_len, lhs, rhs;

    for ( at = 0; at < len; at++ ) {
        lhs = fold( (u8)one[at] );
        rhs = fold( (u8)two[at] );
        if ( lhs != rhs ) {
            return lhs - rhs;
        }
    }
    return one_len - two_len;
}

/* ".." first, then folders, then files, each by name */
static int row_cmp( const void *pa, const void *pb )
{
    const VROW *one = (const VROW *)pa, *two = (const VROW *)pb;

    if ( one->is_up != two->is_up ) {
        return two->is_up - one->is_up;
    }
    if ( one->is_dir != two->is_dir ) {
        return two->is_dir - one->is_dir;
    }
    return leaf_cmp( one->name, one->len, two->name, two->len );
}

void view_attach( ArcFile *archive )
{
    arc = archive;
    cwd[0] = 0;
    cwd_len = 0;
    nrows = 0;
}

void view_folder_mode( int on )
{
    folder_mode = on ? 1 : 0;
    if ( !folder_mode ) {
        cwd[0] = 0;
        cwd_len = 0;
    }
}

int view_is_folder_mode( void )
{
    return folder_mode;
}

static void add_row( const char *name, int len, int index, int is_dir, int is_up )
{
    if ( nrows >= cap ) {
        int want = cap ? cap * 2 : 64;
        VROW *grown = (VROW *)realloc( rows, (size_t)want * sizeof( VROW ) );

        if ( grown == NULL ) {
            return;                     /* out of memory: the row is dropped */
        }
        rows = grown;
        cap = want;
    }
    rows[nrows].name = name;
    rows[nrows].len = len;
    rows[nrows].index = index;
    rows[nrows].is_dir = is_dir;
    rows[nrows].is_up = is_up;
    nrows++;
}

int view_rebuild( void )
{
    const char *name, *rest;
    int at, count, k, rest_len, out;

    nrows = 0;
    if ( arc == NULL ) {
        return 0;
    }
    count = ArcNumEntries( arc );
    if ( !folder_mode ) {
        for ( at = 0; at < count; at++ ) {
            name = ArcEntryName( arc, at );
            if ( name ) {
                add_row( name, (int)strlen( name ), at, ArcEntryIsDir( arc, at ), 0 );
            }
        }
        return nrows;
    }
    if ( cwd_len > 0 ) {
        add_row( "..", 2, -1, 1, 1 );
    }
    for ( at = 0; at < count; at++ ) {
        name = ArcEntryName( arc, at );
        if ( name == NULL ) {
            continue;
        }
        rest = past_seps( name );
        if ( cwd_len > 0 ) {
            if ( !has_prefix( rest, cwd, cwd_len ) ) {
                continue;
            }
            rest += cwd_len;
        }
        if ( !*rest ) {
            continue;                   /* the current folder's own entry */
        }
        for ( k = 0; rest[k] && !is_sep( (u8)rest[k] ); k++ ) {
        }
        rest_len = (int)strlen( rest );
        if ( rest[k] == 0 ) {
            add_row( rest, k, at, ArcEntryIsDir( arc, at ), 0 );
        } else if ( k + 1 == rest_len ) {
            add_row( rest, k, at, 1, 0 );   /* "NAME\": a stored folder */
        } else {
            add_row( rest, k, -1, 1, 0 );   /* implied by something under it */
        }
    }
    if ( nrows > 1 ) {
        qsort( rows, (size_t)nrows, sizeof( VROW ), row_cmp );
    }
    /* every entry under a sub-folder made a row for it; the sort put them
       together, and a stored folder entry wins over the implied ones */
    out = 0;
    for ( at = 0; at < nrows; at++ ) {
        if ( out > 0 && rows[out - 1].is_dir && rows[at].is_dir && !rows[at].is_up
             && !rows[out - 1].is_up
             && leaf_cmp( rows[out - 1].name, rows[out - 1].len, rows[at].name, rows[at].len ) == 0 ) {
            if ( rows[out - 1].index < 0 ) {
                rows[out - 1].index = rows[at].index;
            }
            continue;
        }
        rows[out++] = rows[at];
    }
    nrows = out;
    return nrows;
}

int view_count( void )          { return nrows; }
const char *view_cwd( void )    { return cwd; }

int view_is_up( int row )
{
    return row >= 0 && row < nrows ? rows[row].is_up : 0;
}

int view_is_dir( int row )
{
    return row >= 0 && row < nrows ? rows[row].is_dir : 0;
}

int view_index( int row )
{
    return row >= 0 && row < nrows ? rows[row].index : -1;
}

void view_row_name( int row, char *dst, int size )
{
    int len;

    if ( size <= 0 ) {
        return;
    }
    dst[0] = 0;
    if ( row < 0 || row >= nrows ) {
        return;
    }
    if ( rows[row].is_up ) {
        strncpy( dst, "..", (size_t)size - 1 );
        dst[size - 1] = 0;
        return;
    }
    len = rows[row].len;
    if ( len > size - 1 ) {
        len = size - 1;
    }
    memcpy( dst, rows[row].name, (size_t)len );
    dst[len] = 0;
}

int view_up( void )
{
    int at;

    if ( cwd_len <= 0 ) {
        return 0;
    }
    cwd_len--;                          /* off the trailing '\' */
    for ( at = cwd_len - 1; at >= 0 && cwd[at] != '\\'; at-- ) {
    }
    cwd_len = at + 1;
    cwd[cwd_len] = 0;
    return 1;
}

/* cwd + name + '\' - refused, not cut, if it will not fit: half a
   prefix would match the wrong entries */
int view_enter( int row )
{
    int len;

    if ( !folder_mode || row < 0 || row >= nrows ) {
        return 0;
    }
    if ( rows[row].is_up ) {
        return view_up();
    }
    if ( !rows[row].is_dir ) {
        return 0;
    }
    len = rows[row].len;
    if ( cwd_len + len + 2 > (int)sizeof( cwd ) ) {
        return 0;
    }
    memcpy( cwd + cwd_len, rows[row].name, (size_t)len );
    cwd_len += len;
    cwd[cwd_len++] = '\\';
    cwd[cwd_len] = 0;
    return 1;
}

void view_set_cwd( const char *path )
{
    int len;

    cwd[0] = 0;
    cwd_len = 0;
    if ( !folder_mode || path == NULL || !path[0] ) {
        return;
    }
    len = (int)strlen( path );
    if ( len + 2 > (int)sizeof( cwd ) ) {
        return;
    }
    memcpy( cwd, path, (size_t)len );
    if ( !is_sep( (u8)cwd[len - 1] ) ) {
        cwd[len++] = '\\';
    }
    cwd[len] = 0;
    cwd_len = len;
}

int view_max_expanded( void )
{
    return arc ? ArcNumEntries( arc ) : 0;
}

/* one entry into the list, unless "seen" says it is in it already */
static int take( int index, int *out, int *got, int max, u8 *seen )
{
    if ( *got >= max ) {
        return 0;
    }
    if ( seen ) {
        if ( seen[index] ) {
            return 1;
        }
        seen[index] = 1;
    }
    out[(*got)++] = index;
    return 1;
}

/* a row as entries: a folder means everything under it */
static int expand_row( int row, int *out, int max, u8 *seen )
{
    char prefix[SZ_MAX_NAME];
    const char *name;
    int plen, at, count, got = 0;

    if ( row < 0 || row >= nrows || max <= 0 || rows[row].is_up ) {
        return 0;
    }
    if ( rows[row].index >= 0 ) {
        take( rows[row].index, out, &got, max, seen );
    }
    if ( !rows[row].is_dir ) {
        return got;
    }
    plen = cwd_len;
    if ( plen + rows[row].len + 2 > (int)sizeof( prefix ) ) {
        return got;
    }
    memcpy( prefix, cwd, (size_t)plen );
    memcpy( prefix + plen, rows[row].name, (size_t)rows[row].len );
    plen += rows[row].len;
    prefix[plen++] = '\\';
    prefix[plen] = 0;
    count = arc ? ArcNumEntries( arc ) : 0;
    for ( at = 0; at < count; at++ ) {
        name = ArcEntryName( arc, at );
        if ( name && at != rows[row].index && has_prefix( past_seps( name ), prefix, plen )
             && !take( at, out, &got, max, seen ) ) {
            break;
        }
    }
    return got;
}

int view_expand_one( int row, int *out, int max )
{
    return expand_row( row, out, max, NULL );
}

/* Several rows as entries, each entry once: in the flat list a marked
   folder and the marked files under it would otherwise name the same
   entries twice, and fill "out" before the last of them were in it.
   -1 when there is no memory to keep count with. */
int view_expand( const int *vrows, int count, int *out, int max )
{
    int at, got = 0, entries = arc ? ArcNumEntries( arc ) : 0;
    u8 *seen;

    if ( entries <= 0 ) {
        return 0;
    }
    seen = (u8 *)calloc( (size_t)entries, 1 );
    if ( seen == NULL ) {
        return -1;
    }
    for ( at = 0; at < count && got < max; at++ ) {
        got += expand_row( vrows[at], out + got, max - got, seen );
    }
    free( seen );
    return got;
}
