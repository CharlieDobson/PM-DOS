/*
 * FILES.C - the files of a directory, or of a whole drive.
 *
 * A LIST IS READ WHEN IT IS WANTED: one directory's files for the
 * ordinary views, every directory's for All Files and for a search of
 * the whole disk - a walk down the tree TREE.C already has, so no
 * directory is looked for twice.  Directories themselves are not
 * listed; the tree is where those are.
 *
 * WHICH FILES AND IN WHAT ORDER is the File Display Options dialog's
 * business: a name with wildcards that a file must match, whether
 * hidden and system files show, and the key they are sorted by.
 */
#include "shell.h"

static FINDREC rec;

/* ------------------------------------------------------------------ */
/* sizes past four gigabytes: kilobytes, and the bytes left over       */
/* ------------------------------------------------------------------ */

void big_add( BIGSIZE *sum, u32 bytes )
{
    sum->kb += bytes >> 10;
    sum->rest += bytes & 1023;
    sum->kb += sum->rest >> 10;
    sum->rest &= 1023;
}

/* in bytes while a 32-bit number holds them, in K after that */
char *big_fmt( const BIGSIZE *sum, char *buf )
{
    if ( sum->kb < 4000000UL ) {
        return fmt_num( sum->kb * 1024 + sum->rest, 0, buf );
    }
    fmt_num( sum->kb, 0, buf );
    str_catn( buf, "K", 20 );
    return buf;
}

/* ------------------------------------------------------------------ */
/* dates and times, the country's way                                  */
/* ------------------------------------------------------------------ */

void fmt_date( u16 date, char *buf )
{
    u32 part[3];
    char two[4];
    int index;
    u32 day = date & 31, month = (date >> 5) & 15, year = ((date >> 9) + 80) % 100;

    switch ( country.date_order ) {
    case 1:  part[0] = day;   part[1] = month; part[2] = year; break;
    case 2:  part[0] = year;  part[1] = month; part[2] = day;  break;
    default: part[0] = month; part[1] = day;   part[2] = year; break;
    }
    buf[0] = 0;
    for ( index = 0; index < 3; index++ ) {
        str_catn( buf, fmt_dec2( part[index], two ), 12 );
        if ( index < 2 ) {
            two[0] = country.date_sep;
            two[1] = 0;
            str_catn( buf, two, 12 );
        }
    }
}

void fmt_time( u16 time, char *buf )
{
    u32 hour = time >> 11, minute = (time >> 5) & 63;
    char two[4];
    int pm = hour >= 12;

    if ( !country.clock24 ) {
        hour %= 12;
        if ( hour == 0 ) {
            hour = 12;
        }
    }
    fmt_dec( hour, 2, buf );
    two[0] = country.time_sep;
    two[1] = 0;
    str_catn( buf, two, 12 );
    str_catn( buf, fmt_dec2( minute, two ), 12 );
    if ( !country.clock24 ) {
        str_catn( buf, pm ? "p" : "a", 12 );
    }
}

/* ------------------------------------------------------------------ */
/* a list                                                              */
/* ------------------------------------------------------------------ */

void flist_free( FLIST *list )
{
    int index;

    for ( index = 0; index < list->count; index++ ) {
        xfree( list->ent[index].name );
    }
    xfree( list->ent );
    mem_set( list, 0, sizeof( *list ) );
}

static int flist_add( FLIST *list, int dir )
{
    FENT *bigger, *ent;
    char *name;

    if ( list->count == list->cap ) {
        bigger = (FENT *)try_alloc( (u32)(list->cap + 128) * sizeof( FENT ) );
        if ( bigger == NULL ) {
            return 0;
        }
        if ( list->count ) {
            mem_cpy( bigger, list->ent, (u32)list->count * sizeof( FENT ) );
        }
        xfree( list->ent );
        list->ent = bigger;
        list->cap += 128;
    }
    name = (char *)try_alloc( str_len( rec.name ) + 1 );
    if ( name == NULL ) {
        return 0;
    }
    str_cpy( name, rec.name );
    ent = &list->ent[list->count];
    ent->name = name;
    ent->size = rec.size;
    ent->date = rec.date;
    ent->time = rec.time;
    ent->attr = rec.attr;
    ent->sel = 0;
    ent->dir = dir;
    ent->order = (u32)list->count;
    list->count++;
    big_add( &list->total, rec.size );
    return 1;
}

/* one directory's files onto the end of the list: 0, a DOS error, or
   -1 for no memory */
static int read_into( FLIST *list, DTREE *tree, int node, const char *filter )
{
    char path[PATH_MAX + 8];
    u32 handle;
    int rc;
    u16 attrs = (u16)(ATTR_READONLY | ATTR_ARCHIVE
                      | (opt.show_hidden ? ATTR_HIDDEN | ATTR_SYSTEM : 0));

    tree_path( tree, node, path );
    if ( !path_join( path, path, "*.*", sizeof( path ) ) ) {
        return 0;
    }
    rc = sys_find_first( path, attrs, &rec, &handle );
    if ( rc ) {
        return rc == ERR_NOMORE || rc == ERR_NOFILE ? 0 : rc;
    }
    do {
        if ( rec.attr & (ATTR_DIR | ATTR_VOLUME) ) {
            continue;
        }
        if ( !wild_match( filter, rec.name ) && !wild_match( filter, rec.alias ) ) {
            continue;
        }
        if ( !flist_add( list, node ) ) {
            sys_find_close( handle );
            return -1;
        }
    } while ( sys_find_next( handle, &rec ) == 0 );
    sys_find_close( handle );
    return 0;
}

int flist_read_dir( FLIST *list, DTREE *tree, int node, const char *filter )
{
    int rc;

    flist_free( list );
    sys_crit_seen();
    rc = read_into( list, tree, node, filter );
    if ( sys_crit_seen() && rc == 0 ) {
        rc = ERR_NOTREADY;
    }
    flist_sort( list );
    return rc;
}

/* every directory from "from_node" down - the whole drive when that is
   the root */
int flist_read_all( FLIST *list, DTREE *tree, const char *filter, int from_node )
{
    int index, rc = 0, depth = tree->node[from_node].depth;
    char line[48], num[16];

    flist_free( list );
    sys_crit_seen();
    for ( index = from_node; index < tree->count && rc == 0; index++ ) {
        if ( index > from_node && tree->node[index].depth <= depth ) {
            break;
        }
        rc = read_into( list, tree, index, filter );
        if ( (index & 15) == 0 ) {
            str_cpy( line, "Files read: " );
            str_catn( line, fmt_num( (u32)list->count, 0, num ), sizeof( line ) );
            busy_show( "Reading disk information...", line );
        }
    }
    if ( sys_crit_seen() && rc == 0 ) {
        rc = ERR_NOTREADY;
    }
    flist_sort( list );
    return rc;
}

/* ------------------------------------------------------------------ */
/* order                                                               */
/* ------------------------------------------------------------------ */

static int compare( const FENT *lhs, const FENT *rhs )
{
    int diff = 0;

    switch ( opt.sort_key ) {
    case SORT_EXT:
        diff = str_icmp( path_ext( lhs->name ), path_ext( rhs->name ) );
        break;
    case SORT_DATE:
        diff = lhs->date != rhs->date ? (lhs->date < rhs->date ? -1 : 1)
               : lhs->time != rhs->time ? (lhs->time < rhs->time ? -1 : 1) : 0;
        break;
    case SORT_SIZE:
        diff = lhs->size != rhs->size ? (lhs->size < rhs->size ? -1 : 1) : 0;
        break;
    case SORT_DISK:
        diff = lhs->order < rhs->order ? -1 : 1;
        break;
    }
    if ( diff == 0 ) {
        diff = str_icmp( lhs->name, rhs->name );
    }
    if ( diff == 0 ) {
        diff = lhs->order < rhs->order ? -1 : lhs->order > rhs->order;
    }
    return opt.sort_desc ? -diff : diff;
}

void flist_sort( FLIST *list )
{
    int gap, index, at;
    FENT held;

    for ( gap = list->count / 2; gap > 0; gap /= 2 ) {
        for ( index = gap; index < list->count; index++ ) {
            held = list->ent[index];
            for ( at = index; at >= gap && compare( &list->ent[at - gap], &held ) > 0; at -= gap ) {
                list->ent[at] = list->ent[at - gap];
            }
            list->ent[at] = held;
        }
    }
}

void flist_count( FLIST *list )
{
    int index;

    list->nsel = 0;
    list->selected.kb = list->selected.rest = 0;
    for ( index = 0; index < list->count; index++ ) {
        if ( list->ent[index].sel ) {
            list->nsel++;
            big_add( &list->selected, list->ent[index].size );
        }
    }
}

void flist_path( DTREE *tree, const FENT *ent, char *buf )
{
    tree_path( tree, ent->dir, buf );
    path_join( buf, buf, ent->name, PATH_MAX );
}
