/*
 * SCAN.C - directory tree walk.
 *
 * Pass 1 walks every directory, claims each file's cluster chain in the
 * cluster map and repairs what it can, the way DOS CHKDSK's DIRPROC and
 * MARKFAT do.  A cluster claimed twice is flagged as crossed; pass 2
 * (run only when that happened) walks the tree again and names every
 * file whose chain touches a crossed cluster.
 *
 * Directories are read directly from disk rather than through the DOS
 * directory search calls, so long filename entries are seen and
 * checked, and the checker does not depend on CHDIR.
 */
#include "chkdsk.h"

#define NONE        0xFFFFFFFFul
#define MAX_DEPTH   256

#define DIR_OK      0           /* directory fine                          */
#define DIR_BAD     1           /* . unrecoverable, contents processed     */
#define DIR_DEAD    2           /* contents not processed                  */

#define CE_OK       0           /* chain walked                            */
#define CE_ZERO     1           /* first cluster 0, nothing to walk        */
#define CE_TRUNC    2           /* first cluster invalid, entry truncated  */

#define NOTE_LIMIT  1           /* pass 2: read only this many clusters    */
#define NOTE_SKIP   2           /* pass 2: do not descend                  */

typedef struct {
    u32 count;                  /* clusters claimed                        */
    int cross;                  /* ran into a cluster claimed earlier      */
    int badlink;                /* an invalid link was cut off             */
} CHAIN;

/* what pass 1 decided about a subdirectory, keyed by (parent, entry) */
typedef struct {
    u32 dir;
    u32 idx;
    int kind;
    u32 limit;
} DIRNOTE;

static DIRNOTE *notes;
static u32      nnotes, capnotes;
static int      pass;
static int      depth;
static int      fixmes_done;
static char     rootpath[4];

static int  process_dir( DIRBUF *dir, u32 self, u32 parent, const char *path,
                         int is_root );

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void note_add( u32 dir, u32 idx, int kind, u32 limit )
{
    DIRNOTE *nn;
    u32 i;

    for ( i = 0; i < nnotes; i++ ) {
        if ( notes[i].dir == dir && notes[i].idx == idx ) {
            if ( kind > notes[i].kind ) {
                notes[i].kind = kind;
                notes[i].limit = limit;
            }
            return;
        }
    }
    if ( nnotes == capnotes ) {
        capnotes = capnotes ? capnotes * 2 : 16;
        nn = (DIRNOTE *)xalloc( capnotes * sizeof( DIRNOTE ) );
        if ( nnotes ) {
            mem_cpy( nn, notes, nnotes * sizeof( DIRNOTE ) );
        }
        xfree( notes );
        notes = nn;
    }
    notes[nnotes].dir = dir;
    notes[nnotes].idx = idx;
    notes[nnotes].kind = kind;
    notes[nnotes].limit = limit;
    nnotes++;
}

static DIRNOTE *note_find( u32 dir, u32 idx )
{
    u32 i;

    for ( i = 0; i < nnotes; i++ ) {
        if ( notes[i].dir == dir && notes[i].idx == idx ) {
            return &notes[i];
        }
    }
    return NULL;
}

/* "Errors found, F parameter not specified" - once, and only without /F */
void nofix_notice( void )
{
    if ( fixmes_done ) {
        return;
    }
    fixmes_done = 1;
    if ( opt.dofix ) {
        return;
    }
    out_line( H_OUT, M_FIXMES );
    out_blank();
}

/* the object in error on one line, the complaint on the next */
void report_error( const char *path, const char *text )
{
    if ( pass == 2 ) {
        return;
    }
    nofix_notice();
    if ( path ) {
        out_line( H_OUT, path );
    }
    out_line( H_OUT, text );
}

int prompt_yn( const char *text )
{
    char buf[80];
    int len, ch;

    for ( ;; ) {
        out_text( H_OUT, text );
        out_flush();
        len = sys_read_line( buf, sizeof( buf ) );
        out_input_done();
        if ( len < 0 ) {
            return 0;
        }
        if ( len > 0 ) {
            ch = ch_upper( buf[0] );
            if ( ch == 'Y' ) {
                return 1;
            }
            if ( ch == 'N' ) {
                return 0;
            }
        }
    }
}

u32 entry_first( const u8 *ent )
{
    u32 clus = RD16( ent + DE_CLUSLO );

    if ( vol.fattype == 32 ) {
        clus |= (u32)RD16( ent + DE_CLUSHI ) << 16;
    }
    return clus;
}

void entry_set_first( u8 *ent, u32 clus )
{
    WR16( ent + DE_CLUSLO, clus );
    if ( vol.fattype == 32 ) {
        WR16( ent + DE_CLUSHI, clus >> 16 );
    }
}

void entry_stamp( u8 *ent )
{
    u16 date, time;

    sys_get_datetime( &date, &time );
    WR16( ent + DE_TIME, time );
    WR16( ent + DE_DATE, date );
    WR16( ent + DE_CTIME, time );
    WR16( ent + DE_CDATE, date );
    WR16( ent + DE_ADATE, date );
    ent[DE_CTENTH] = 0;
}

int dir_fix( DIRBUF *dir, u32 idx )
{
    return dir_write_entry( dir, idx );
}

/* ------------------------------------------------------------------ */
/* cluster chains                                                      */
/* ------------------------------------------------------------------ */

/*
 * Claim the chain starting at first (a valid cluster).  An invalid link,
 * or a link back into this same chain, is cut off with an end-of-chain
 * mark.  Running into a cluster some earlier chain owns marks it crossed
 * and stops, as MARKFAT does.
 */
static void claim_chain( u32 first, U32LIST *list, CHAIN *ch )
{
    u32 clus = first, prev = 0, next, i;

    ch->count = 0;
    ch->cross = 0;
    ch->badlink = 0;
    for ( ;; ) {
        if ( vol.map[clus] & MF_USED ) {
            if ( vol.map[clus] & MF_CUR ) {
                fat_set( prev, vol.eoc_val );
                ch->badlink = 1;
            } else {
                vol.map[clus] |= MF_CROSS;
                st.cross_cnt++;
                ch->cross = 1;
            }
            break;
        }
        vol.map[clus] |= MF_USED | MF_CUR;
        ch->count++;
        if ( list ) {
            list_add( list, clus );
        }
        next = fat_get( clus );
        if ( IS_EOC( next ) ) {
            break;
        }
        if ( !CLUS_OK( next ) ) {
            fat_set( clus, vol.eoc_val );
            ch->badlink = 1;
            break;
        }
        prev = clus;
        clus = next;
    }
    for ( clus = first, i = 0; i < ch->count; i++ ) {
        vol.map[clus] &= ~MF_CUR;
        clus = fat_get( clus );
        if ( !CLUS_OK( clus ) ) {
            break;
        }
    }
}

/* pass 1 checks of one entry's chain and size (DOS MARKFAT) */
static int check_entry( DIRBUF *dir, u32 idx, const char *path, int is_dir,
                        U32LIST *list, CHAIN *ch )
{
    u8 *ent = dir->data + idx * DE_LEN;
    u32 first = entry_first( ent ), size = RD32( ent + DE_SIZE );
    u64 alloc;

    ch->count = 0;
    ch->cross = 0;
    ch->badlink = 0;
    if ( !CLUS_OK( first ) ) {
        if ( first == 0 ) {
            if ( !is_dir && size != 0 ) {
                report_error( path, M_BADCLUS );
                WR32( ent + DE_SIZE, 0 );
                dir_fix( dir, idx );
            }
            return CE_ZERO;
        }
        report_error( path, M_NULNZ );
        entry_set_first( ent, 0 );
        WR32( ent + DE_SIZE, 0 );
        dir_fix( dir, idx );
        return CE_TRUNC;
    }
    claim_chain( first, list, ch );
    if ( ch->badlink ) {
        report_error( path, M_BADCHAIN );
    }
    if ( is_dir || ch->cross ) {
        return CE_OK;
    }

    /* size must fall inside the last allocation unit of the chain */
    alloc = mul32( ch->count, vol.bpc );
    if ( (u64)size > alloc || alloc - size >= (u64)vol.bpc ) {
        if ( !ch->badlink ) {
            report_error( path, M_BADCLUS );
        }
        WR32( ent + DE_SIZE, hi32( alloc ) ? 0xFFFFFFFFul : lo32( alloc ) );
        dir_fix( dir, idx );
    }
    return CE_OK;
}

/* pass 2: name the object if its chain touches a crossed cluster */
static void cross_chain( u32 clus, const char *path )
{
    u32 count = 0, next;
    char num[12];

    while ( CLUS_OK( clus ) && count++ <= vol.nclus ) {
        if ( vol.map[clus] & MF_CROSS ) {
            out_line( H_OUT, path );
            out_msg( H_OUT, M_CROSS, fmt_dec( clus, num ), NULL, NULL );
            return;
        }
        next = fat_get( clus );
        if ( IS_EOC( next ) ) {
            return;
        }
        clus = next;
    }
}

/* ------------------------------------------------------------------ */
/* . and ..                                                            */
/* ------------------------------------------------------------------ */

/* without /V all dot problems of a directory become one message */
static void sub_error( const char *path, int *errsub )
{
    if ( *errsub ) {
        return;
    }
    *errsub = 1;
    report_error( path, M_BADSUBDIR );
}

static void dot_error( const char *dotpath, const char *path,
                       const char *msg, int *errsub )
{
    if ( opt.noisy ) {
        report_error( dotpath, msg );
    } else {
        sub_error( path, errsub );
    }
}

static void dot_fields( DIRBUF *dir, u32 idx, u32 expect, const char *path,
                        int which, int *errsub )
{
    u8 *ent = dir->data + idx * DE_LEN;
    u32 clus = entry_first( ent );
    char *dotpath = path_join( path, which == 1 ? "." : ".." );

    if ( !(ent[DE_ATTR] & A_DIR) ) {
        dot_error( dotpath, path, M_BADATTR, errsub );
        ent[DE_ATTR] |= A_DIR;
        dir_fix( dir, idx );
    }
    /* FAT32 ".." of a root child may hold 0 or the root cluster */
    if ( clus != expect &&
         !(which == 2 && expect == 0 && vol.fattype == 32 &&
           clus == vol.root_clus) ) {
        dot_error( dotpath, path, M_BADLINK, errsub );
        entry_set_first( ent, expect );
        dir_fix( dir, idx );
    }
    if ( RD32( ent + DE_SIZE ) != 0 ) {
        dot_error( dotpath, path, M_BADSIZE, errsub );
        WR32( ent + DE_SIZE, 0 );
        dir_fix( dir, idx );
    }
    xfree( dotpath );
}

static void make_dot( DIRBUF *dir, u32 idx, int which, u32 clus )
{
    u8 *ent = dir->data + idx * DE_LEN;

    mem_set( ent, 0, DE_LEN );
    mem_set( ent, ' ', 11 );
    ent[0] = '.';
    if ( which == 2 ) {
        ent[1] = '.';
    }
    ent[DE_ATTR] = A_DIR;
    entry_stamp( ent );
    entry_set_first( ent, clus );
    dir_fix( dir, idx );
}

static int check_dots( DIRBUF *dir, u32 self, u32 parent, const char *path,
                       u32 *di, u32 *ddi )
{
    u8 *e0 = dir->data, *e1 = dir->nent > 1 ? dir->data + DE_LEN : NULL;
    u32 i, idx;
    int errsub = 0, status = DIR_OK, any = 0;
    char *dotpath;

    for ( i = 0; i < dir->nent; i++ ) {
        if ( dir->data[i * DE_LEN] == 0 ) {
            break;
        }
        if ( dir->data[i * DE_LEN] != DEL_MARK ) {
            any = 1;
            break;
        }
    }
    if ( !any ) {
        if ( opt.noisy ) {
            report_error( path, M_NULDIR );
            out_line( H_OUT, M_BADTARG2 );
        } else {
            sub_error( path, &errsub );
        }
        st.ftrunc = 1;
        return DIR_DEAD;
    }

    /* "." must be the first entry and point at this directory */
    if ( sfn_is_dot( e0 ) == 1 ) {
        *di = 0;
        dot_fields( dir, 0, self, path, 1, &errsub );
    } else {
        dotpath = path_join( path, "." );
        dot_error( dotpath, path, M_NDOT, &errsub );
        if ( e0[0] == DEL_MARK ) {
            make_dot( dir, 0, 1, self );
            *di = 0;
        } else {
            if ( opt.noisy ) {
                report_error( dotpath, M_NORECDOT );
            }
            status = DIR_BAD;
        }
        xfree( dotpath );
    }

    /* ".." must follow and point at the parent (0 for the root) */
    idx = NONE;
    if ( e1 && sfn_is_dot( e1 ) == 2 ) {
        idx = 1;
    } else if ( *di == NONE && sfn_is_dot( e0 ) == 2 ) {
        idx = 0;
    }
    if ( idx != NONE ) {
        *ddi = idx;
        dot_fields( dir, idx, parent, path, 2, &errsub );
        return status;
    }
    dotpath = path_join( path, ".." );
    dot_error( dotpath, path, M_NDOT, &errsub );
    if ( e1 && (e1[0] == DEL_MARK ||
                (e1[0] == 0 && (dir->nent < 3 || e1[DE_LEN] == 0))) ) {
        make_dot( dir, 1, 2, parent );
        *ddi = 1;
    } else {
        if ( opt.noisy ) {
            report_error( dotpath, M_NORECDDOT );
            out_line( H_OUT, M_BADTARG2 );
        }
        st.ftrunc = 1;
        status = DIR_DEAD;
    }
    xfree( dotpath );
    return status;
}

/* ------------------------------------------------------------------ */
/* entries                                                             */
/* ------------------------------------------------------------------ */

/* pass 1: a run of LFN entries that belongs to nothing is removed */
static void lfn_broken( DIRBUF *dir, LFNRUN *run, u32 end, const u8 *sfn,
                        const char *path )
{
    char nm[13];
    char *objpath;
    u8 *ent;
    u32 i;

    if ( pass != 1 || !run->active ) {
        return;
    }
    if ( sfn ) {
        sfn_name( sfn, nm );
        objpath = path_join( path, nm );
    } else {
        objpath = str_dup( path );
    }
    report_error( objpath, M_BADLFN );
    for ( i = run->start; i < end; i++ ) {
        ent = dir->data + i * DE_LEN;
        if ( IS_LFN( ent ) && ent[0] != DEL_MARK ) {
            ent[0] = DEL_MARK;
            dir_fix( dir, i );
        }
    }
    xfree( objpath );
}

static void ask_convert( DIRBUF *pd, u32 idx, u32 nclus )
{
    u8 *ent = pd->data + idx * DE_LEN;
    u64 sz;

    if ( !opt.dofix ) {
        return;
    }
    if ( opt.noisy ) {
        out_line( H_OUT, M_PTRANDIR );
    }
    if ( !prompt_yn( M_PTRANDIR2 ) ) {
        return;
    }
    sz = mul32( nclus, vol.bpc );
    ent[DE_ATTR] &= ~A_DIR;
    WR32( ent + DE_SIZE, hi32( sz ) ? 0xFFFFFFFFul : lo32( sz ) );
    dir_fix( pd, idx );
}

static void do_label( const char *path )
{
    if ( pass != 1 ) {
        return;
    }
    if ( opt.noisy ) {
        out_msg( H_OUT, M_NOISY, path, NULL, NULL );
    }
    st.hid_cnt++;               /* DOS counts the label as a hidden file */
}

static void do_file( DIRBUF *dir, u32 idx, const char *path )
{
    u8 *ent = dir->data + idx * DE_LEN;
    CHAIN ch;

    if ( pass == 2 ) {
        cross_chain( entry_first( ent ), path );
        return;
    }
    if ( opt.noisy ) {
        out_msg( H_OUT, M_NOISY, path, NULL, NULL );
    }
    check_entry( dir, idx, path, 0, NULL, &ch );
    if ( ent[DE_ATTR] & A_HIDDEN ) {
        st.hid_cnt++;
        st.hid_clus += ch.count;
    } else {
        st.fil_cnt++;
        st.fil_clus += ch.count;
    }
}

static void do_subdir( DIRBUF *pd, u32 pself, u32 idx, const char *path )
{
    u8 *ent = pd->data + idx * DE_LEN;
    u32 first = entry_first( ent );
    DIRBUF dir;
    DIRNOTE *note;
    U32LIST list;
    CHAIN ch;
    int rc, status;

    mem_set( &dir, 0, sizeof( dir ) );
    if ( pass == 2 ) {
        cross_chain( first, path );
        note = note_find( pself, idx );
        if ( (note && note->kind == NOTE_SKIP) || !CLUS_OK( first ) ||
             depth >= MAX_DEPTH ) {
            return;
        }
        if ( dir_load_chain( &dir, first, note ? note->limit : 0 ) == 0 ) {
            depth++;
            process_dir( &dir, first, pself, path, 0 );
            depth--;
        }
        dir_free( &dir );
        return;
    }

    mem_set( &list, 0, sizeof( list ) );
    rc = check_entry( pd, idx, path, 1, &list, &ch );
    st.dir_cnt++;
    st.dir_clus += ch.count;
    if ( rc == CE_TRUNC ) {                     /* becomes an empty file */
        ent[DE_ATTR] &= ~A_DIR;
        dir_fix( pd, idx );
        note_add( pself, idx, NOTE_SKIP, 0 );
        return;
    }
    if ( rc == CE_ZERO ) {
        report_error( path, M_BADSUBDIR );
        ask_convert( pd, idx, 0 );
        note_add( pself, idx, NOTE_SKIP, 0 );
        return;
    }
    if ( ch.count == 0 ) {                      /* crossed at first cluster */
        note_add( pself, idx, NOTE_SKIP, 0 );
        list_free( &list );
        return;
    }
    if ( ch.cross ) {
        note_add( pself, idx, NOTE_LIMIT, ch.count );
    }

    if ( depth >= MAX_DEPTH || dir_load_list( &dir, &list ) != 0 ) {
        nofix_notice();
        out_msg( H_OUT, M_BADTARG, path, NULL, NULL );
        st.ftrunc = 1;
        note_add( pself, idx, NOTE_SKIP, 0 );
    } else {
        depth++;
        status = process_dir( &dir, first, pself, path, 0 );
        depth--;
        if ( status == DIR_DEAD ) {
            note_add( pself, idx, NOTE_SKIP, 0 );
        }
        if ( status != DIR_OK ) {
            ask_convert( pd, idx, ch.count );
        }
    }
    dir_free( &dir );
    list_free( &list );
}

typedef struct {
    LFNRUN run;
    char   name[LFN_MAX + 1];
} WALKBUF;

/*
 * Process one directory held in dir.  self is its first cluster (0 for
 * the root), parent the first cluster its ".." should hold.
 */
static int process_dir( DIRBUF *dir, u32 self, u32 parent, const char *path,
                        int is_root )
{
    WALKBUF *walk = (WALKBUF *)xalloc( sizeof( WALKBUF ) );
    u32 i, di = NONE, ddi = NONE;
    int status = DIR_OK;
    char *child;
    u8 *ent;

    if ( pass == 1 && opt.noisy ) {
        out_msg( H_OUT, M_DIREC, path, NULL, NULL );
    }
    if ( !is_root ) {
        if ( pass == 1 ) {
            status = check_dots( dir, self, parent, path, &di, &ddi );
            if ( status == DIR_DEAD ) {
                xfree( walk );
                return status;
            }
        } else {
            if ( dir->nent > 0 && sfn_is_dot( dir->data ) == 1 ) {
                di = 0;
            }
            if ( dir->nent > 1 && sfn_is_dot( dir->data + DE_LEN ) == 2 ) {
                ddi = 1;
            } else if ( di == NONE && dir->nent > 0 &&
                        sfn_is_dot( dir->data ) == 2 ) {
                ddi = 0;
            }
        }
    }

    lfn_begin( &walk->run );
    for ( i = 0; i < dir->nent; i++ ) {
        ent = dir->data + i * DE_LEN;
        if ( ent[0] == 0 ) {
            break;
        }
        if ( ent[0] == DEL_MARK ) {
            lfn_broken( dir, &walk->run, i, NULL, path );
            lfn_begin( &walk->run );
            continue;
        }
        if ( IS_LFN( ent ) ) {
            if ( !lfn_add( &walk->run, ent, i ) ) {
                lfn_broken( dir, &walk->run, i, NULL, path );
                lfn_begin( &walk->run );
                lfn_add( &walk->run, ent, i );
            }
            continue;
        }
        if ( i == di || i == ddi || sfn_is_dot( ent ) ) {
            lfn_broken( dir, &walk->run, i, NULL, path );
            lfn_begin( &walk->run );
            continue;
        }
        if ( walk->run.active && lfn_match( &walk->run, ent ) ) {
            lfn_oem( &walk->run, walk->name );
        } else {
            lfn_broken( dir, &walk->run, i, ent, path );
            sfn_name( ent, walk->name );
        }
        lfn_begin( &walk->run );

        child = path_join( path, walk->name );
        if ( ent[DE_ATTR] & A_DIR ) {
            do_subdir( dir, self, i, child );
        } else if ( ent[DE_ATTR] & A_VOLID ) {
            do_label( child );
        } else {
            do_file( dir, i, child );
        }
        xfree( child );
    }
    lfn_broken( dir, &walk->run, i, NULL, path );
    xfree( walk );
    return status;
}

/* ------------------------------------------------------------------ */
/* entry points                                                        */
/* ------------------------------------------------------------------ */

static void root_failed( void )
{
    out_line( H_ERR, M_BADCD );
    out_line( H_ERR, M_FATAL );
    chkdsk_abort();
}

void scan_pass1( void )
{
    DIRBUF dir;
    U32LIST list;
    CHAIN ch;
    int rc;

    pass = 1;
    rootpath[0] = (char)('A' + vol.drive);
    rootpath[1] = ':';
    rootpath[2] = '\\';
    rootpath[3] = 0;
    mem_set( &list, 0, sizeof( list ) );
    mem_set( &dir, 0, sizeof( dir ) );
    if ( vol.fattype == 32 ) {
        if ( !CLUS_OK( vol.root_clus ) ) {
            root_failed();
        }
        claim_chain( vol.root_clus, &list, &ch );
        st.root_clus = ch.count;
        if ( ch.badlink ) {
            report_error( rootpath, M_BADCHAIN );
        }
        rc = dir_load_list( &dir, &list );
    } else {
        rc = dir_load_root( &dir );
    }
    if ( rc != 0 ) {
        root_failed();
    }
    process_dir( &dir, 0, 0, rootpath, 1 );
    dir_free( &dir );
    list_free( &list );
}

void scan_pass2( void )
{
    DIRBUF dir;

    pass = 2;
    mem_set( &dir, 0, sizeof( dir ) );
    if ( vol.fattype == 32 ) {
        cross_chain( vol.root_clus, rootpath );
    }
    if ( dir_load_root( &dir ) == 0 ) {
        process_dir( &dir, 0, 0, rootpath, 1 );
    }
    dir_free( &dir );
}
