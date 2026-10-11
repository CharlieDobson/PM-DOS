/*
 * FRAG.C - SCANDISK /FRAGMENT [drive:][path]filename: which of the
 * named files are in more than one piece.  CHKDSK's fragmentation
 * report, on its own.
 *
 * Path components and the file pattern may use either 8.3 or long
 * names; wildcards follow FCB rules against the 8.3 name and Windows 95
 * rules against the long name.  Only ordinary files are looked at (not
 * hidden, system, directory or label entries).  Nothing is written.
 */
#include "scandisk.h"

typedef struct {
    DIRBUF *dir;
    u32     idx;
    LFNRUN  run;
    char    lname[LFN_MAX + 1];         /* long name, or "" */
    char    sname[13];                  /* 8.3 name */
} DITER;

/* next 8.3 entry of a directory, with its long name if it has one */
static u8 *dir_next( DITER *it )
{
    u8 *ent;

    while ( it->idx < it->dir->nent ) {
        ent = it->dir->data + it->idx * DE_LEN;
        it->idx++;
        if ( ent[0] == 0 ) {
            break;
        }
        if ( ent[0] == DEL_MARK ) {
            lfn_begin( &it->run );
            continue;
        }
        if ( IS_LFN( ent ) ) {
            if ( !lfn_add( &it->run, ent, it->idx - 1 ) ) {
                lfn_begin( &it->run );
                lfn_add( &it->run, ent, it->idx - 1 );
            }
            continue;
        }
        if ( it->run.active && lfn_match( &it->run, ent ) ) {
            lfn_oem( &it->run, it->lname );
        } else {
            it->lname[0] = 0;
        }
        lfn_begin( &it->run );
        sfn_name( ent, it->sname );
        return ent;
    }
    return NULL;
}

static void iter_init( DITER *it, DIRBUF *dir )
{
    it->dir = dir;
    it->idx = 0;
    it->lname[0] = 0;
    lfn_begin( &it->run );
}

/*
 * Step from directory *cur into component name.  *path grows by the
 * name as stored on disk.  Returns 0 on success.
 */
static int step( u32 *cur, char **path, const char *name, DITER *it )
{
    DIRBUF dir;
    u8 *ent;
    char *np;
    u32 clus;
    int found = 0;

    if ( str_icmp( name, "." ) == 0 ) {
        return 0;
    }
    if ( dir_load_first( &dir, *cur ) != 0 ) {
        dir_free( &dir );
        return -1;
    }
    iter_init( it, &dir );
    while ( (ent = dir_next( it )) != NULL ) {
        if ( !(ent[DE_ATTR] & A_DIR) || (ent[DE_ATTR] & A_VOLID) == A_VOLID ) {
            continue;
        }
        if ( str_icmp( name, ".." ) == 0 ) {
            if ( sfn_is_dot( ent ) != 2 ) {
                continue;
            }
        } else if ( str_icmp( name, it->sname ) != 0 &&
                    (!it->lname[0] || str_icmp( name, it->lname ) != 0) ) {
            continue;
        }
        clus = entry_first( ent );
        if ( vol.fattype == 32 && clus == vol.root_clus ) {
            clus = 0;
        }
        if ( str_icmp( name, ".." ) == 0 ) {
            /* drop the last component of the displayed path */
            np = *path + str_len( *path );
            while ( np > *path + 3 && np[-1] != '\\' ) {
                np--;
            }
            if ( np > *path + 3 ) {
                np--;
            }
            *np = 0;
        } else {
            np = path_join( *path, it->lname[0] ? it->lname : it->sname );
            xfree( *path );
            *path = np;
        }
        *cur = clus;
        found = 1;
        break;
    }
    dir_free( &dir );
    return found ? 0 : -1;
}

/* walk a "\"-separated list of components; 0 = all found */
static int walk( u32 *cur, char **path, const char *src, DITER *it )
{
    char comp[LFN_MAX + 1];
    int len;

    while ( *src ) {
        while ( *src == '\\' ) {
            src++;
        }
        len = 0;
        while ( *src && *src != '\\' && len < LFN_MAX ) {
            comp[len++] = *src++;
        }
        comp[len] = 0;
        if ( len && step( cur, path, comp, it ) != 0 ) {
            return -1;
        }
    }
    return 0;
}

static u32 pieces( u32 clus )
{
    u32 count = 1, steps = 0, next;

    if ( !CLUS_OK( clus ) ) {
        return 0;
    }
    for ( ;; ) {
        next = fat_get( clus );
        if ( IS_EOC( next ) || !CLUS_OK( next ) || steps++ > vol.nclus ) {
            break;
        }
        if ( next != clus + 1 ) {
            count++;
        }
        clus = next;
    }
    return count;
}

int frag_report( int drive, const char *spec )
{
    DITER *it;
    const char *slash = NULL, *pat;
    char *dirpart, *path, *full;
    char cwd[LFN_MAX + 1], line[LFN_MAX + 80];
    u32 cur = 0, len, count;
    int found = 0, fragmented = 0;
    DIRBUF dir;
    u8 *ent;

    if ( vol_open( drive ) != 0 || fat_load( vol.mirror ? 0 : vol.active_fat ) != 0 ) {
        say( "Invalid drive specification\r\n" );
        return 1;
    }
    it = (DITER *)xalloc( sizeof( DITER ) );
    for ( pat = spec; *pat; pat++ ) {
        if ( *pat == '\\' ) {
            slash = pat;
        }
    }
    len = slash ? (u32)(slash - spec) + 1 : 0;
    dirpart = (char *)xalloc( len + 1 );
    mem_cpy( dirpart, spec, len );
    dirpart[len] = 0;
    pat = slash ? slash + 1 : spec;

    path = (char *)xalloc( 4 );
    path[0] = (char)('A' + vol.drive);
    path[1] = ':';
    path[2] = '\\';
    path[3] = 0;

    if ( dirpart[0] != '\\' && sys_get_cwd( vol.drive, cwd, sizeof( cwd ) ) == 0 &&
         walk( &cur, &path, cwd, it ) != 0 ) {
        cur = 0;                            /* current directory is gone */
        path[3] = 0;
    }
    if ( walk( &cur, &path, dirpart, it ) != 0 ) {
        say( "Path not found\r\n" );
    } else {
        if ( dir_load_first( &dir, cur ) == 0 ) {
            iter_init( it, &dir );
            while ( (ent = dir_next( it )) != NULL ) {
                if ( ent[DE_ATTR] & (A_HIDDEN | A_SYSTEM | A_VOLID | A_DIR) ) {
                    continue;
                }
                if ( !fcb_match( pat, ent ) && !wild_match( pat, it->sname ) &&
                     !(it->lname[0] && wild_match( pat, it->lname )) ) {
                    continue;
                }
                found = 1;
                count = pieces( entry_first( ent ) );
                if ( count > 1 ) {
                    fragmented = 1;
                    full = path_join( path, it->lname[0] ? it->lname : it->sname );
                    sfmt( line, sizeof( line ), "%s is in %,u pieces\r\n", full, count );
                    say( line );
                    xfree( full );
                }
            }
        }
        dir_free( &dir );
        if ( !found ) {
            say( "File not found\r\n" );
        } else if ( !fragmented ) {
            say( "None of the files is fragmented\r\n" );
        }
    }
    xfree( path );
    xfree( dirpart );
    xfree( it );
    vol_close();
    return 0;
}
