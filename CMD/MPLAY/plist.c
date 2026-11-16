/*
 * PLIST.C - the playlist: the files MPLAY plays one after another
 * (MPLAY.H).
 *
 * AN ENTRY IS A WHOLE PATH, made whole when it is added, so a list
 * still plays after the current directory has changed and a saved
 * list means the same files wherever it is opened.  Its title and
 * length are filled in the first time it plays: reading every file's
 * tags as it is added would make adding a folder from a floppy take
 * minutes.
 *
 * WHAT CAN BE ADDED: a file; a pattern (C:\MUSIC\*.MP3), each match in
 * name order; a directory, every file in it MPLAY knows by its
 * extension; and a playlist - .M3U (a path a line, # lines are
 * comments, a relative path is from the list's own directory) or .PLS
 * (FileN= lines) - whose entries are added in its place, one level
 * deep, so a list that names itself cannot loop.
 *
 * SHUFFLE IS AN ORDER, NOT A DIE: every entry is played once before
 * any is played again.  The order is made from a seed that changes
 * with the clock, by swapping, and made again when it runs out or the
 * list changes.
 */
#include "mplay.h"

PLENTRY *plist;
int plist_count;
int plist_now = -1;
int plist_sel;
int plist_top;

static int *order;                      /* the shuffled order: positions of plist */
static int order_pos;
static u32 seed = 12345;

static const char *const media_exts[] = {
    "WAV", "VOC", "MID", "MIDI", "RMI", "KAR", "MP3", "MP2", "MP1", "MPA", "OGG", "OGA",
#ifdef HAVE_WMA
    "WMA", "ASF",
#endif
    NULL
};

int plist_init( void )
{
    plist = (PLENTRY *)try_alloc( PL_MAX * sizeof( PLENTRY ) );
    order = (int *)try_alloc( PL_MAX * sizeof( int ) );
    if ( plist == NULL || order == NULL ) {
        return 0;
    }
    mem_set( plist, 0, PL_MAX * sizeof( PLENTRY ) );
    plist_count = 0;
    return 1;
}

static void entry_free( PLENTRY *entry )
{
    if ( entry->path != NULL ) {
        xfree( entry->path );
    }
    if ( entry->title != NULL ) {
        xfree( entry->title );
    }
    mem_set( entry, 0, sizeof( PLENTRY ) );
}

void plist_free( void )
{
    plist_clear();
    if ( plist != NULL ) {
        xfree( plist );
        plist = NULL;
    }
    if ( order != NULL ) {
        xfree( order );
        order = NULL;
    }
}

void plist_clear( void )
{
    int index;

    for ( index = 0; index < plist_count; index++ ) {
        entry_free( &plist[index] );
    }
    plist_count = 0;
    plist_now = -1;
    plist_sel = plist_top = 0;
    order_pos = 0;
}

/* the part after the last dot, in capitals, without the dot */
static void ext_of( const char *name, char *ext )
{
    const char *dot = NULL, *at;
    int len = 0;

    for ( at = name; *at; at++ ) {
        if ( *at == '.' ) {
            dot = at;
        } else if ( *at == '\\' || *at == '/' || *at == ':' ) {
            dot = NULL;
        }
    }
    if ( dot != NULL ) {
        for ( at = dot + 1; *at && len < 7; at++ ) {
            ext[len++] = (char)ch_upper( *at );
        }
    }
    ext[len] = 0;
}

int plist_is_media( const char *name )
{
    char ext[8];
    int index;

    ext_of( name, ext );
    for ( index = 0; media_exts[index] != NULL; index++ ) {
        if ( str_cmp( ext, media_exts[index] ) == 0 ) {
            return 1;
        }
    }
    return 0;
}

/* one file onto the end, its path made whole */
static int add_one( const char *path )
{
    char full[PATH_LEN];
    PLENTRY *entry;

    if ( plist_count >= PL_MAX ) {
        return 0;
    }
    if ( sys_truename( path, full, 2 ) != 0 || full[0] == 0 ) {
        str_cpyt( full, path, sizeof( full ) );
    }
    entry = &plist[plist_count];
    mem_set( entry, 0, sizeof( PLENTRY ) );
    entry->path = (char *)try_alloc( str_len( full ) + 1 );
    if ( entry->path == NULL ) {
        return 0;
    }
    str_cpy( entry->path, full );
    plist_count++;
    order_pos = 0;                      /* the shuffle starts again */
    return 1;
}

/* the directory part of a path, with its last backslash: "C:\MUSIC\" */
static void dir_of( const char *path, char *dir, u32 max )
{
    const char *last = NULL, *at;
    u32 len;

    for ( at = path; *at; at++ ) {
        if ( *at == '\\' || *at == '/' || *at == ':' ) {
            last = at;
        }
    }
    len = last != NULL ? (u32)(last - path) + 1 : 0;
    if ( len >= max ) {
        len = max - 1;
    }
    mem_cpy( dir, path, len );
    dir[len] = 0;
}

static int has_wild( const char *path )
{
    for ( ; *path; path++ ) {
        if ( *path == '*' || *path == '?' ) {
            return 1;
        }
    }
    return 0;
}

/* the names a pattern matches, sorted, added -> how many */
static int add_pattern( const char *pattern, int media_only )
{
    FINDREC rec;
    u32 handle;
    char dir[PATH_LEN], full[PATH_LEN];
    char **names = NULL;
    int count = 0, room = 0, added = 0, i, j;
    char *swap;

    dir_of( pattern, dir, sizeof( dir ) );
    if ( sys_find_first( pattern, 0, &rec, &handle ) != 0 ) {
        return 0;
    }
    do {
        if ( rec.attr & (ATTR_DIR | ATTR_VOLUME) ) {
            continue;
        }
        if ( media_only && !plist_is_media( rec.name ) ) {
            continue;
        }
        if ( count == room ) {
            char **more = (char **)try_realloc( names, (u32)(room + 64) * sizeof( char * ) );
            if ( more == NULL ) {
                break;
            }
            names = more;
            room += 64;
        }
        names[count] = (char *)try_alloc( str_len( rec.name ) + 1 );
        if ( names[count] == NULL ) {
            break;
        }
        str_cpy( names[count], rec.name );
        count++;
    } while ( count < PL_MAX && sys_find_next( handle, &rec ) == 0 );
    sys_find_close( handle );
    /* in name order: an insertion sort, for a directory's worth */
    for ( i = 1; i < count; i++ ) {
        swap = names[i];
        for ( j = i; j > 0 && str_icmp( names[j - 1], swap ) > 0; j-- ) {
            names[j] = names[j - 1];
        }
        names[j] = swap;
    }
    for ( i = 0; i < count; i++ ) {
        /* a name that would not fit whole is left out, not cut short */
        if ( str_len( dir ) + str_len( names[i] ) < sizeof( full ) ) {
            sfmt( full, sizeof( full ), "%s%s", dir, names[i] );
            added += add_one( full );
        }
        xfree( names[i] );
    }
    if ( names != NULL ) {
        xfree( names );
    }
    return added;
}

/* a playlist file's entries -> how many were added */
static int add_listfile( const char *path, int is_pls )
{
    SRC src;
    char line[PATH_LEN], dir[PATH_LEN], full[PATH_LEN];
    const char *name;
    u32 len;
    int ch, added = 0, lines = 0;
    u16 attr;

    if ( src_open( &src, path ) != 0 ) {
        return 0;
    }
    dir_of( path, dir, sizeof( dir ) );
    while ( lines++ < 100000 ) {
        len = 0;
        while ( (ch = src_byte( &src )) >= 0 && ch != '\n' ) {
            if ( ch != '\r' && len < sizeof( line ) - 1 ) {
                line[len++] = (char)ch;
            }
        }
        line[len] = 0;
        while ( len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t') ) {
            line[--len] = 0;
        }
        name = line;
        if ( is_pls ) {
            /* FileN=path */
            if ( str_nicmp( line, "File", 4 ) != 0 || str_chr( line, '=' ) == NULL ) {
                name = NULL;
            } else {
                name = str_chr( line, '=' ) + 1;
            }
        } else if ( line[0] == '#' ) {
            name = NULL;
        }
        /* a byte-order mark before the first line */
        if ( name != NULL && (u8)name[0] == 0xEF && (u8)name[1] == 0xBB && (u8)name[2] == 0xBF ) {
            name += 3;
        }
        if ( name != NULL && name[0] ) {
            /* a line that would not fit whole is left out, not cut short */
            full[0] = 0;
            if ( name[0] == '\\' || name[0] == '/' || (name[0] && name[1] == ':') ) {
                if ( str_len( name ) < sizeof( full ) ) {
                    str_cpy( full, name );
                }
            } else if ( str_len( dir ) + str_len( name ) < sizeof( full ) ) {
                sfmt( full, sizeof( full ), "%s%s", dir, name );
            }
            if ( full[0] && sys_get_attr( full, &attr ) == 0 && !(attr & ATTR_DIR) ) {
                added += add_one( full );
            }
        }
        if ( ch < 0 ) {
            break;
        }
    }
    src_close( &src );
    return added;
}

/* "C:\", "\" or "C:": a root, or a drive's current directory, which
   DOS has no attributes for */
static int is_drive_dir( const char *path )
{
    if ( path[0] && path[1] == ':' ) {
        path += 2;
    }
    return path[0] == 0 || ((path[0] == '\\' || path[0] == '/') && path[1] == 0);
}

int plist_add( const char *path )
{
    char ext[8], pattern[PATH_LEN];
    u16 attr;
    u32 len;

    if ( path == NULL || path[0] == 0 ) {
        return 0;
    }
    if ( has_wild( path ) ) {
        return add_pattern( path, 0 );
    }
    if ( sys_get_attr( path, &attr ) != 0 ) {
        if ( !is_drive_dir( path ) ) {
            return 0;
        }
        attr = ATTR_DIR;
    }
    if ( attr & ATTR_DIR ) {
        len = str_len( path );
        if ( len + 5 >= sizeof( pattern ) ) {
            return 0;                   /* no room for "\*.*" after the name */
        }
        sfmt( pattern, sizeof( pattern ), len && (path[len - 1] == '\\' || path[len - 1] == '/' || path[len - 1] == ':')
              ? "%s*.*" : "%s\\*.*", path );
        return add_pattern( pattern, 1 );
    }
    ext_of( path, ext );
    if ( str_cmp( ext, "M3U" ) == 0 || str_cmp( ext, "M3U8" ) == 0 ) {
        return add_listfile( path, 0 );
    }
    if ( str_cmp( ext, "PLS" ) == 0 ) {
        return add_listfile( path, 1 );
    }
    return add_one( path );
}

void plist_remove( int index )
{
    if ( index < 0 || index >= plist_count ) {
        return;
    }
    entry_free( &plist[index] );
    mem_cpy( &plist[index], &plist[index + 1], (u32)(plist_count - index - 1) * sizeof( PLENTRY ) );
    plist_count--;
    mem_set( &plist[plist_count], 0, sizeof( PLENTRY ) );
    if ( plist_now == index ) {
        plist_now = -1;
    } else if ( plist_now > index ) {
        plist_now--;
    }
    if ( plist_sel >= plist_count ) {
        plist_sel = plist_count > 0 ? plist_count - 1 : 0;
    }
    order_pos = 0;
}

int plist_save( const char *path )
{
    u32 handle, done;
    int index, rc;

    rc = sys_create( path, &handle );
    if ( rc != 0 ) {
        return rc;
    }
    sys_write( handle, "#EXTM3U\r\n", 9, &done );
    for ( index = 0; index < plist_count && rc == 0; index++ ) {
        rc = sys_write( handle, plist[index].path, str_len( plist[index].path ), &done );
        if ( rc == 0 ) {
            rc = sys_write( handle, "\r\n", 2, &done );
        }
    }
    sys_close( handle );
    return rc;
}

const char *plist_name( int index )
{
    const char *at, *name;

    if ( index < 0 || index >= plist_count ) {
        return "";
    }
    if ( plist[index].title != NULL && plist[index].title[0] ) {
        return plist[index].title;
    }
    name = plist[index].path;
    for ( at = name; *at; at++ ) {
        if ( *at == '\\' || *at == '/' || *at == ':' ) {
            name = at + 1;
        }
    }
    return name;
}

static u32 rnd( void )
{
    seed = seed * 1103515245UL + 12345UL;
    return (seed >> 8) & 0xFFFFFFUL;
}

void plist_reshuffle( void )
{
    int index, other, swap;

    seed ^= sys_hundredths();
    for ( index = 0; index < plist_count; index++ ) {
        order[index] = index;
    }
    for ( index = plist_count - 1; index > 0; index-- ) {
        other = (int)(rnd() % (u32)(index + 1));
        swap = order[index];
        order[index] = order[other];
        order[other] = swap;
    }
    order_pos = 1;                      /* 0 means "make it again" */
}

int plist_next( int from, int dir )
{
    int index;

    if ( plist_count == 0 ) {
        return -1;
    }
    if ( set.repeat == RP_ONE && from >= 0 && from < plist_count && dir == 0 ) {
        return from;
    }
    if ( dir == 0 ) {
        dir = 1;
    }
    if ( set.shuffle ) {
        if ( order_pos == 0 ) {
            plist_reshuffle();
            /* start from the first in the new order, unless it is the one just played */
            if ( plist_count > 1 && order[0] == from ) {
                index = order[0];
                order[0] = order[plist_count - 1];
                order[plist_count - 1] = index;
            }
            order_pos = 1;
            return order[0];
        }
        if ( dir > 0 ) {
            if ( order_pos >= plist_count ) {
                if ( set.repeat != RP_ALL ) {
                    return -1;
                }
                order_pos = 0;
                return plist_next( from, 1 );
            }
            return order[order_pos++];
        }
        if ( order_pos >= 2 ) {
            order_pos--;
            return order[order_pos - 1];
        }
        return order[0];
    }
    index = from + dir;
    if ( index >= plist_count ) {
        return set.repeat == RP_ALL ? 0 : -1;
    }
    if ( index < 0 ) {
        return set.repeat == RP_ALL ? plist_count - 1 : 0;
    }
    return index;
}
