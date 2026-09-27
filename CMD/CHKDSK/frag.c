/*
 * FRAG.C - CHKDSK [drive:][path]filename: report fragmented files.
 *
 * Runs after the status report, as in DOS.  Path components and the
 * file pattern may use either 8.3 or long names; wildcards follow FCB
 * rules against the 8.3 name and Windows 95 rules against the long name.
 * Like the DOS FCB search it replaces, only normal files are examined
 * (not hidden, system, directory or label entries).
 */
#include "chkdsk.h"

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

static int load_dir( DIRBUF *dir, u32 first )
{
    mem_set( dir, 0, sizeof( *dir ) );
    if ( first == 0 ) {
        return dir_load_root( dir );
    }
    return dir_load_chain( dir, first, 0 );
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
    if ( load_dir( &dir, *cur ) != 0 ) {
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

static u32 blocks( u32 clus )
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

void frag_check( void )
{
    DITER *it = (DITER *)xalloc( sizeof( DITER ) );
    const char *spec = opt.filespec;
    const char *slash = NULL, *pat;
    char *dirpart, *path, *full;
    char cwd[LFN_MAX + 1], num[40];
    u32 cur = 0, len, frag;
    int found = 0, fragmented = 0;
    DIRBUF dir;
    u8 *ent;

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

    out_blank();
    if ( dirpart[0] != '\\' &&
         sys_get_cwd( vol.drive, cwd, sizeof( cwd ) ) == 0 &&
         walk( &cur, &path, cwd, it ) != 0 ) {
        cur = 0;                            /* current directory is gone */
        path[3] = 0;
    }
    if ( walk( &cur, &path, dirpart, it ) != 0 ) {
        out_line( H_ERR, M_INVPATH );
        goto done;
    }

    if ( load_dir( &dir, cur ) == 0 ) {
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
            frag = blocks( entry_first( ent ) );
            if ( frag > 1 ) {
                fragmented = 1;
                full = path_join( path, it->lname[0] ? it->lname : it->sname );
                out_msg( H_OUT, M_EXTENT, full, fmt_num( frag, num ), NULL );
                xfree( full );
            }
        }
    }
    dir_free( &dir );
    if ( !found ) {
        out_line( H_ERR, M_OPNERR );
    } else if ( !fragmented ) {
        out_line( H_OUT, M_NOEXT );
    }
done:
    xfree( path );
    xfree( dirpart );
    xfree( it );
}
