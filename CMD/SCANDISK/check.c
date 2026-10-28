/*
 * CHECK.C - the first three checks: the media descriptor, the file
 * allocation tables, and the directory structure.
 *
 * THE DIRECTORY WALK is CHKDSK's (SCAN.C there), which is DOS CHKDSK's
 * DIRPROC and MARKFAT: every directory is read off the disk rather than
 * searched through DOS, so long file name entries are seen, and every
 * file's chain is claimed in the cluster map, where a cluster claimed
 * twice shows as a cross-link and one never claimed as lost.
 *
 * What differs is that nothing is put right until it has been asked
 * about.  Each thing found wrong is a problem (PROBLEM.C); the answer
 * says whether the repair is made, and a repair is on the disk before
 * the walk goes on.  A problem left alone is left alone in memory too,
 * so a chain with a bad link is simply claimed as far as the link.
 *
 * What ScanDisk calls a problem, and what is done about it:
 *
 *   a name with characters a name cannot have     the characters replaced
 *   attribute bits that mean nothing              cleared
 *   a size the chain does not bear out            the size set from the chain
 *   a "." or ".." that is missing or wrong        written again
 *   long name entries with no file                removed
 *   a chain with a link that goes nowhere         ended before the link
 *   a file starting at a cluster that is not one  made an empty file
 *   a directory whose clusters are not a          its entry deleted (and its
 *     directory, or are another's                 clusters, if any, lost)
 */
#include "scandisk.h"

#define NONE        0xFFFFFFFFul
#define MAX_DEPTH   128

typedef struct {
    u32 count;                  /* clusters claimed                        */
    u32 last;                   /* the last of them                        */
    int cross;                  /* ran into a cluster claimed earlier      */
    u32 cross_clus;
    int badlink;                /* the link out of "last" goes nowhere     */
    u32 badval;
} CHAIN;

CROSSREC *cross_list;
u32       cross_count;

static u32  cross_cap;
static int  depth;
static u32  claimed;            /* clusters claimed so far: the progress */
static char rootpath[4];

static void walk_dir( DIRBUF *dir, u32 self, u32 parent, const char *path, int is_root );

void check_reset( void )
{
    u32 index;

    for ( index = 0; index < cross_count; index++ ) {
        xfree( cross_list[index].path );
    }
    xfree( cross_list );
    cross_list = NULL;
    cross_count = cross_cap = 0;
    claimed = 0;
    depth = 0;
    mem_set( &st, 0, sizeof( st ) );
}

/* ------------------------------------------------------------------ */
/* the media descriptor                                                */
/* ------------------------------------------------------------------ */

static int media_valid( u8 id )
{
    return id == 0xF0 || id >= 0xF8;
}

/* entry 0 of a FAT: the descriptor, and every other bit set */
static int entry0_ok( const u8 *sec )
{
    switch ( vol.fattype ) {
    case 12: return (sec[1] & 0x0F) == 0x0F;
    case 16: return sec[1] == 0xFF;
    }
    return sec[1] == 0xFF && sec[2] == 0xFF && (sec[3] & 0x0F) == 0x0F;
}

static void entry0_set( u8 *sec, u8 id )
{
    sec[0] = id;
    switch ( vol.fattype ) {
    case 12:
        sec[1] |= 0x0F;
        break;
    case 16:
        sec[1] = 0xFF;
        break;
    default:
        sec[1] = sec[2] = 0xFF;
        sec[3] |= 0x0F;
        break;
    }
}

void check_media( void )
{
    u8 *sec = (u8 *)xalloc( vol.bps );
    u32 copy, lba;
    u8 id = 0, want;
    int have = 0, bad = 0;

    fe_progress( 0, 1 );
    for ( copy = 0; copy < vol.nfats && !have; copy++ ) {
        if ( !vol.mirror && copy != vol.active_fat ) {
            continue;
        }
        if ( read_secs( vol.rsvd + copy * vol.fatsz, 1, sec ) == 0 ) {
            have = 1;
            id = sec[0];
        }
    }
    /* with no FAT to read there is nothing to compare: the next check
       says so */
    if ( have && !(media_valid( vol.media ) && id == vol.media && entry0_ok( sec )) ) {
        want = media_valid( vol.media ) ? vol.media
               : media_valid( id ) ? id
               : sys_drive_floppy( vol.drive ) ? 0xF0 : 0xF8;
        if ( pb_found( PB_MEDIA,
                       "The media descriptor of drive %c: is wrong.  The boot sector says %02Xh "
                       "and the file allocation table begins with %02Xh; both should be %02Xh.",
                       'A' + vol.drive, vol.media, id, want ) == ACT_FIX ) {
            for ( copy = 0; copy < vol.nfats; copy++ ) {
                if ( !vol.mirror && copy != vol.active_fat ) {
                    continue;
                }
                lba = vol.rsvd + copy * vol.fatsz;
                if ( read_secs( lba, 1, sec ) != 0 ) {
                    continue;                   /* the next check's business */
                }
                entry0_set( sec, want );
                if ( write_secs( lba, 1, sec, WR_FAT ) != 0 ) {
                    bad = 1;
                }
            }
            if ( vol.media != want && vol.boot_ok ) {
                if ( read_secs( 0, 1, sec ) == 0 ) {
                    sec[21] = want;
                    if ( write_secs( 0, 1, sec, WR_UNKNOWN ) != 0 ) {
                        bad = 1;
                    }
                }
                if ( vol.backup_sec && read_secs( vol.backup_sec, 1, sec ) == 0 &&
                     RD16( sec + 11 ) == vol.bps && sec[13] == vol.spc ) {
                    sec[21] = want;
                    write_secs( vol.backup_sec, 1, sec, WR_UNKNOWN );
                }
                vol.media = want;
            }
            if ( bad ) {
                pb_failed( "A sector could not be written." );
            } else {
                pb_fixed();
            }
        }
    }
    xfree( sec );
    fe_progress( 1, 1 );
}

/* ------------------------------------------------------------------ */
/* the file allocation tables                                          */
/* ------------------------------------------------------------------ */

void check_fats( void )
{
    FATSTAT *stat = (FATSTAT *)xzalloc( vol.nfats * sizeof( FATSTAT ) );
    u32 differing = 0, copy, best = NONE, primary = NONE, kept = 0, readable = 0, clus, val;
    u32 unread = NONE;
    int act;

    fat_compare( stat, &differing );
    for ( copy = 0; copy < vol.nfats; copy++ ) {
        if ( !vol.mirror && copy != vol.active_fat ) {
            continue;
        }
        kept++;
        if ( primary == NONE ) {
            primary = copy;
        }
        if ( stat[copy].unreadable ) {
            if ( unread == NONE ) {
                unread = copy;
            }
            continue;
        }
        readable++;
        if ( best == NONE || stat[copy].invalid < stat[best].invalid ) {
            best = copy;
        }
    }
    if ( readable == 0 ) {
        pb_found( PB_FATREAD,
                  "No copy of the file allocation table of drive %c: can be read.  "
                  "The drive cannot be checked.", 'A' + vol.drive );
        xfree( stat );
        scan_quit( Q_DRIVE );
    }

    if ( readable < kept ) {
        act = pb_found( PB_FATREAD,
                        "Copy %u of the file allocation table of drive %c: cannot be read.  "
                        "Copy %u can be, and ScanDisk can write copy %u again from it.",
                        unread + 1, 'A' + vol.drive, best + 1, unread + 1 );
        if ( fat_load( best ) != 0 ) {
            xfree( stat );
            scan_quit( Q_DRIVE );
        }
        if ( act == ACT_FIX ) {
            if ( fat_rewrite() == 0 ) {
                pb_fixed();
            } else {
                pb_failed( "Copy %u still cannot be written.  The disk is failing.", unread + 1 );
            }
        }
    } else if ( differing ) {
        act = pb_found( PB_FATMISMATCH,
                        "The copies of the file allocation table of drive %c: do not match: "
                        "%,u sector%s.  Copy %u has the fewest invalid entries, and "
                        "ScanDisk can make the other%s the same as copy %u.",
                        'A' + vol.drive, differing, differing == 1 ? " differs" : "s differ", best + 1,
                        kept > 2 ? "s" : "", best + 1 );
        if ( fat_load( act == ACT_FIX ? best : primary ) != 0 ) {
            xfree( stat );
            scan_quit( Q_DRIVE );
        }
        if ( act == ACT_FIX ) {
            if ( fat_rewrite() == 0 ) {
                pb_fixed();
            } else {
                pb_failed( "A sector of a file allocation table could not be written." );
            }
        }
    } else if ( fat_load( primary ) != 0 ) {
        xfree( stat );
        scan_quit( Q_DRIVE );
    }
    xfree( stat );

    /* the map, and what the FAT itself says is in use */
    vol.map = (u8 *)try_alloc( vol.maxclus + 1 );
    if ( vol.map == NULL ) {
        out_of_memory();
    }
    mem_set( vol.map, 0, vol.maxclus + 1 );
    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        val = fat_get( clus );
        if ( val == vol.bad_val ) {
            vol.map[clus] |= MF_BAD;
            st.bad_clus++;
        } else if ( val != 0 ) {
            st.used_clus++;
        }
        if ( (clus & 0x3FFF) == 0 ) {
            scan_tick();
        }
    }
}

/* ------------------------------------------------------------------ */
/* cluster chains                                                      */
/* ------------------------------------------------------------------ */

/* a cluster a chain can hold: in range, and neither free nor bad */
static int clus_live( u32 clus )
{
    u32 val;

    if ( !CLUS_OK( clus ) ) {
        return 0;
    }
    val = fat_get( clus );
    return val != 0 && val != vol.bad_val;
}

/*
 * Claim the chain starting at first (a live cluster).  It ends at an
 * end-of-chain mark, at a link that goes nowhere - out of range, to a
 * free or a bad cluster, or back into this same chain - or at a cluster
 * some earlier chain owns, which is marked crossed.
 */
static void claim_chain( u32 first, U32LIST *list, CHAIN *ch )
{
    u32 clus = first, next, index;

    mem_set( ch, 0, sizeof( *ch ) );
    for ( ;; ) {
        if ( vol.map[clus] & MF_USED ) {
            if ( vol.map[clus] & MF_CUR ) {
                ch->badlink = 1;
                ch->badval = clus;
            } else {
                vol.map[clus] |= MF_CROSS;
                ch->cross = 1;
                ch->cross_clus = clus;
            }
            break;
        }
        vol.map[clus] |= MF_USED | MF_CUR;
        ch->count++;
        ch->last = clus;
        if ( list ) {
            list_add( list, clus );
        }
        next = fat_get( clus );
        if ( IS_EOC( next ) ) {
            break;
        }
        if ( !clus_live( next ) ) {
            ch->badlink = 1;
            ch->badval = next;
            break;
        }
        clus = next;
    }
    for ( clus = first, index = 0; index < ch->count; index++ ) {
        vol.map[clus] &= ~MF_CUR;
        clus = fat_get( clus );
    }
    claimed += ch->count;
}

/* give back what claim_chain took: the entry is going, its clusters stay */
static void unclaim_chain( u32 first, u32 count )
{
    u32 clus = first, index;

    for ( index = 0; index < count && CLUS_OK( clus ); index++ ) {
        vol.map[clus] &= ~MF_USED;
        clus = fat_get( clus );
    }
    claimed -= index;
}

static void cross_add( u32 dir_first, u32 idx, const CHAIN *ch, int is_dir, const char *path )
{
    CROSSREC *grown;

    if ( cross_count == cross_cap ) {
        cross_cap = cross_cap ? cross_cap * 2 : 8;
        grown = (CROSSREC *)xalloc( cross_cap * sizeof( CROSSREC ) );
        if ( cross_count ) {
            mem_cpy( grown, cross_list, cross_count * sizeof( CROSSREC ) );
        }
        xfree( cross_list );
        cross_list = grown;
    }
    cross_list[cross_count].dir_first = dir_first;
    cross_list[cross_count].idx = idx;
    cross_list[cross_count].clus = ch->cross_clus;
    cross_list[cross_count].prev = ch->count ? ch->last : 0;
    cross_list[cross_count].is_dir = is_dir;
    cross_list[cross_count].path = str_dup( path );
    cross_count++;
}

/* ------------------------------------------------------------------ */
/* writing entries back                                                */
/* ------------------------------------------------------------------ */

/* the sectors holding entries from..to-1, each once; 0 = all written */
static int write_entries( DIRBUF *dir, u32 from, u32 to )
{
    u32 idx, sec, last = NONE;
    int bad = 0;

    for ( idx = from; idx < to; idx++ ) {
        sec = idx * DE_LEN / vol.bps;
        if ( sec != last ) {
            last = sec;
            if ( dir_write_entry( dir, idx ) != 0 ) {
                bad = 1;
            }
        }
    }
    return bad ? -1 : 0;
}

static void outcome( int rc )
{
    if ( rc == 0 ) {
        pb_fixed();
    } else {
        pb_failed( "A sector could not be written." );
    }
}

/* an entry and the long name in front of it, marked deleted */
static int delete_entry( DIRBUF *dir, u32 lfn_first, u32 idx )
{
    u32 cur;

    for ( cur = lfn_first; cur <= idx; cur++ ) {
        dir->data[cur * DE_LEN] = DEL_MARK;
    }
    return write_entries( dir, lfn_first, idx + 1 );
}

/* ------------------------------------------------------------------ */
/* names and attributes                                                */
/* ------------------------------------------------------------------ */

static int char_bad( u8 ch )
{
    static const char never[] = "\"*+,./:;<=>?[\\]|";

    return ch < 0x20 || ch == 0x7F || str_chr( never, ch ) != NULL;
}

/* an 8.3 name as the directory holds it: 0 = nothing wrong with it */
static int name_bad( const u8 *ent )
{
    int pos;
    u8 ch;

    if ( ent[0] == ' ' ) {
        return 1;
    }
    for ( pos = 0; pos < 11; pos++ ) {
        ch = ent[pos];
        if ( pos == 0 && ch == 0x05 ) {
            continue;                           /* E5h, a real first character */
        }
        if ( char_bad( ch ) || (ch >= 'a' && ch <= 'z') ) {
            return 1;
        }
    }
    return 0;
}

/* some other entry of the directory already has those eleven bytes */
static int name_taken( DIRBUF *dir, u32 self, const u8 *name )
{
    const u8 *ent;
    u32 idx;

    for ( idx = 0; idx < dir->nent; idx++ ) {
        ent = dir->data + idx * DE_LEN;
        if ( ent[0] == 0 ) {
            break;
        }
        if ( idx != self && ent[0] != DEL_MARK && !IS_LFN( ent ) &&
             mem_cmp( ent, name, 11 ) == 0 ) {
            return 1;
        }
    }
    return 0;
}

/* The name made one DOS can use: small letters made capitals, anything
   else that cannot be in a name made an underscore, and the last
   character of the name varied until no other entry has the result.
   The long name in front of it keeps the entry, so its checksum
   follows. */
static int name_repair( DIRBUF *dir, u32 lfn_first, u32 idx )
{
    static const char vary[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    u8 *ent = dir->data + idx * DE_LEN, name[11];
    int pos, tries;
    u32 cur;
    u8 sum;

    mem_cpy( name, ent, 11 );
    for ( pos = 0; pos < 11; pos++ ) {
        if ( pos == 0 && name[0] == 0x05 ) {
            continue;
        }
        if ( name[pos] >= 'a' && name[pos] <= 'z' ) {
            name[pos] -= 'a' - 'A';
        } else if ( char_bad( name[pos] ) || (pos == 0 && name[pos] == ' ') ) {
            name[pos] = '_';
        }
    }
    for ( tries = 0; name_taken( dir, idx, name ); tries++ ) {
        if ( tries == sizeof( vary ) - 1 ) {
            return -1;
        }
        name[7] = (u8)vary[tries];
    }
    mem_cpy( ent, name, 11 );
    sum = lfn_checksum( ent );
    for ( cur = lfn_first; cur < idx; cur++ ) {
        dir->data[cur * DE_LEN + 13] = sum;
    }
    return write_entries( dir, lfn_first, idx + 1 );
}

/* ------------------------------------------------------------------ */
/* sizes                                                               */
/* ------------------------------------------------------------------ */

/* the size a chain of "count" clusters bears out, when "size" is not it */
static int size_wrong( u32 size, u32 count, u32 *right )
{
    u64 alloc = mul32( count, vol.bpc );

    if ( (u64)size > alloc || alloc - size >= (u64)vol.bpc ) {
        *right = hi32( alloc ) ? 0xFFFFFFFFul : lo32( alloc );
        return *right != size;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* . and ..                                                            */
/* ------------------------------------------------------------------ */

static void make_dot( u8 *ent, int which, u32 clus )
{
    if ( sfn_is_dot( ent ) != which ) {
        mem_set( ent, 0, DE_LEN );
        mem_set( ent, ' ', 11 );
        ent[0] = '.';
        if ( which == 2 ) {
            ent[1] = '.';
        }
        entry_stamp( ent );
    }
    ent[DE_ATTR] = (u8)((ent[DE_ATTR] & 0x3F & ~A_VOLID) | A_DIR);
    entry_set_first( ent, clus );
    WR32( ent + DE_SIZE, 0 );
}

static int dot_wrong( const u8 *ent, int which, u32 expect )
{
    u32 clus = entry_first( ent );

    if ( sfn_is_dot( ent ) != which || !(ent[DE_ATTR] & A_DIR) || RD32( ent + DE_SIZE ) != 0 ) {
        return 1;
    }
    /* FAT32: the ".." of a directory in the root may hold 0 or the
       root's own cluster */
    if ( clus != expect &&
         !(which == 2 && expect == 0 && vol.fattype == 32 && clus == vol.root_clus) ) {
        return 1;
    }
    return 0;
}

/* 0 = it is a directory (put right if it had to be, and was allowed
   to be); 1 = its first two entries are something else altogether */
static int check_dots( DIRBUF *dir, u32 self, u32 parent, const char *path )
{
    u8 *e0 = dir->data, *e1 = dir->nent > 1 ? dir->data + DE_LEN : NULL;
    int ended;

    if ( e1 == NULL ) {
        return 1;
    }
    if ( sfn_is_dot( e0 ) != 1 && e0[0] != DEL_MARK && e0[0] != 0 ) {
        return 1;
    }
    if ( sfn_is_dot( e1 ) != 2 && e1[0] != DEL_MARK && e1[0] != 0 ) {
        return 1;
    }
    if ( !dot_wrong( e0, 1, self ) && !dot_wrong( e1, 2, parent ) ) {
        return 0;
    }
    if ( pb_found( PB_DOTS,
                   "The directory %s has a \".\" or \"..\" entry that is missing or wrong.",
                   path ) == ACT_FIX ) {
        /* an entry made where the directory ended must not uncover
           whatever lies beyond the end */
        ended = e0[0] == 0 || e1[0] == 0;
        make_dot( e0, 1, self );
        make_dot( e1, 2, parent );
        if ( ended && dir->nent > 2 ) {
            dir->data[2 * DE_LEN] = 0;
        }
        outcome( write_entries( dir, 0, dir->nent > 2 ? 3 : 2 ) );
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* entries                                                             */
/* ------------------------------------------------------------------ */

/* a run of long name entries that no file follows */
static void lfn_orphan( DIRBUF *dir, LFNRUN *run, u32 end, const char *path )
{
    u32 idx, count = 0;
    u8 *ent;

    if ( !run->active || !opt.lfncheck ) {
        return;
    }
    for ( idx = run->start; idx < end; idx++ ) {
        ent = dir->data + idx * DE_LEN;
        if ( IS_LFN( ent ) && ent[0] != DEL_MARK ) {
            count++;
        }
    }
    if ( count == 0 ) {
        return;
    }
    if ( pb_found( PB_LFN,
                   "The directory %s has %u long file name entr%s that belong%s to no file.",
                   path, count, count == 1 ? "y" : "ies", count == 1 ? "s" : "" ) == ACT_FIX ) {
        for ( idx = run->start; idx < end; idx++ ) {
            ent = dir->data + idx * DE_LEN;
            if ( IS_LFN( ent ) && ent[0] != DEL_MARK ) {
                ent[0] = DEL_MARK;
            }
        }
        outcome( write_entries( dir, run->start, end ) );
    }
}

static void do_label( DIRBUF *dir, u32 idx, int is_root )
{
    u8 *ent = dir->data + idx * DE_LEN;
    char name[13];
    int pos, bad = 0;

    if ( !opt.labelcheck || !is_root ) {
        return;
    }
    for ( pos = 0; pos < 11; pos++ ) {
        if ( char_bad( ent[pos] ) || (ent[pos] >= 'a' && ent[pos] <= 'z') ) {
            bad = 1;
        }
    }
    if ( !bad ) {
        return;
    }
    mem_cpy( name, ent, 11 );
    name[11] = 0;
    for ( pos = 0; pos < 11; pos++ ) {
        if ( (u8)name[pos] < 0x20 ) {
            name[pos] = '?';
        }
    }
    if ( pb_found( PB_LABEL,
                   "The volume label of drive %c:, \"%s\", has characters a label cannot have.",
                   'A' + vol.drive, name ) == ACT_FIX ) {
        for ( pos = 0; pos < 11; pos++ ) {
            if ( ent[pos] >= 'a' && ent[pos] <= 'z' ) {
                ent[pos] -= 'a' - 'A';
            } else if ( char_bad( ent[pos] ) ) {
                ent[pos] = '_';
            }
        }
        outcome( dir_write_entry( dir, idx ) );
    }
}

static void do_file( DIRBUF *dir, u32 self, u32 idx, const char *path )
{
    u8 *ent = dir->data + idx * DE_LEN;
    u32 first = entry_first( ent ), size = RD32( ent + DE_SIZE ), right;
    CHAIN ch;

    mem_set( &ch, 0, sizeof( ch ) );
    if ( first == 0 ) {
        if ( size != 0 &&
             pb_found( PB_SIZE,
                       "The file %s is recorded as %,u byte%s long, but no space on the disk "
                       "belongs to it.", path, size, size == 1 ? "" : "s" ) == ACT_FIX ) {
            WR32( ent + DE_SIZE, 0 );
            outcome( dir_write_entry( dir, idx ) );
        }
    } else if ( !clus_live( first ) ) {
        if ( pb_found( PB_FIRST,
                       "The file %s is recorded as starting at cluster %,u, which is not a "
                       "cluster a file can be in.", path, first ) == ACT_FIX ) {
            entry_set_first( ent, 0 );
            WR32( ent + DE_SIZE, 0 );
            outcome( dir_write_entry( dir, idx ) );
        }
    } else {
        claim_chain( first, NULL, &ch );
        if ( ch.cross ) {
            cross_add( self, idx, &ch, 0, path );
        } else if ( ch.badlink ) {
            if ( pb_found( PB_CHAIN,
                           "The file %s is damaged: after %,u cluster%s the chain of clusters "
                           "the file is in goes to cluster %,u, which is not part of any file.",
                           path, ch.count, ch.count == 1 ? "" : "s", ch.badval ) == ACT_FIX ) {
                fat_set( ch.last, vol.eoc_val );
                if ( size_wrong( size, ch.count, &right ) ) {
                    WR32( ent + DE_SIZE, right );
                }
                outcome( fat_commit() | dir_write_entry( dir, idx ) );
            }
        } else if ( size_wrong( size, ch.count, &right ) &&
                    pb_found( PB_SIZE,
                              "The file %s is recorded as %,u byte%s long, but the space on "
                              "the disk that belongs to it holds %,u bytes.", path, size,
                              size == 1 ? "" : "s", right ) == ACT_FIX ) {
            WR32( ent + DE_SIZE, right );
            outcome( dir_write_entry( dir, idx ) );
        }
    }
    if ( ent[DE_ATTR] & A_HIDDEN ) {
        st.hid_cnt++;
        st.hid_clus += ch.count;
    } else {
        st.fil_cnt++;
        st.fil_clus += ch.count;
    }
}

static void do_subdir( DIRBUF *pd, u32 pself, u32 lfn_first, u32 idx, const char *path )
{
    u8 *ent = pd->data + idx * DE_LEN;
    u32 first = entry_first( ent );
    DIRBUF dir;
    U32LIST list;
    CHAIN ch;

    mem_set( &dir, 0, sizeof( dir ) );
    mem_set( &list, 0, sizeof( list ) );
    mem_set( &ch, 0, sizeof( ch ) );

    if ( !clus_live( first ) ) {
        if ( pb_found( PB_BADDIR,
                       "The entry for the directory %s points at cluster %,u, which is not a "
                       "cluster a directory can be in.  Nothing can be found through this entry.",
                       path, first ) == ACT_DELETE ) {
            outcome( delete_entry( pd, lfn_first, idx ) );
        }
        return;
    }
    claim_chain( first, &list, &ch );
    if ( ch.count == 0 ) {
        /* its first cluster is somebody else's: a second name for a
           directory already walked, or a way round in a circle */
        if ( pb_found( PB_BADDIR,
                       "The entry for the directory %s points at cluster %,u, which already "
                       "belongs to another file or directory.", path, first ) == ACT_DELETE ) {
            outcome( delete_entry( pd, lfn_first, idx ) );
        }
        return;
    }
    st.dir_cnt++;
    st.dir_clus += ch.count;
    if ( ch.cross ) {
        cross_add( pself, idx, &ch, 1, path );
    } else if ( ch.badlink &&
                pb_found( PB_CHAIN,
                          "The directory %s is damaged: after %,u cluster%s the chain of "
                          "clusters the directory is in goes to cluster %,u, which is not part "
                          "of any file.", path, ch.count, ch.count == 1 ? "" : "s",
                          ch.badval ) == ACT_FIX ) {
        fat_set( ch.last, vol.eoc_val );
        outcome( fat_commit() );
    }
    if ( RD32( ent + DE_SIZE ) != 0 &&
         pb_found( PB_SIZE, "The entry for the directory %s gives a size.  A directory's "
                   "entry has none.", path ) == ACT_FIX ) {
        WR32( ent + DE_SIZE, 0 );
        outcome( dir_write_entry( pd, idx ) );
    }

    if ( depth >= MAX_DEPTH ) {
        pb_found( PB_DIRREAD, "The directory %s is too many directories deep to be checked.",
                  path );
        st.ftrunc = 1;
    } else if ( dir_load_list( &dir, &list ) != 0 ) {
        pb_found( PB_DIRREAD,
                  "The directory %s cannot be read.  The files in the directory, and in any "
                  "directory below, have not been checked.", path );
        st.ftrunc = 1;
    } else if ( check_dots( &dir, first, pself, path ) != 0 ) {
        if ( pb_found( PB_BADDIR,
                       "The entry for %s says it is a directory, but the clusters it points at "
                       "do not hold one.", path ) == ACT_DELETE ) {
            /* the entry goes; the clusters stay in use, and the file
               system check finds them with no owner */
            unclaim_chain( first, ch.count );
            st.dir_cnt--;
            st.dir_clus -= ch.count;
            outcome( delete_entry( pd, lfn_first, idx ) );
        }
    } else {
        depth++;
        walk_dir( &dir, first, pself, path, 0 );
        depth--;
    }
    dir_free( &dir );
    list_free( &list );
}

typedef struct {
    LFNRUN run;
    char   name[LFN_MAX + 1];
} WALKBUF;

/*
 * Go through one directory held in dir.  self is its first cluster (0
 * for the root), parent the cluster its ".." holds.
 */
static void walk_dir( DIRBUF *dir, u32 self, u32 parent, const char *path, int is_root )
{
    WALKBUF *walk = (WALKBUF *)xalloc( sizeof( WALKBUF ) );
    u32 idx, lfn_first;
    char *child;
    u8 *ent;

    (void)parent;
    lfn_begin( &walk->run );
    for ( idx = 0; idx < dir->nent; idx++ ) {
        ent = dir->data + idx * DE_LEN;
        if ( ent[0] == 0 ) {
            break;
        }
        if ( ent[0] == DEL_MARK ) {
            lfn_orphan( dir, &walk->run, idx, path );
            lfn_begin( &walk->run );
            continue;
        }
        if ( IS_LFN( ent ) ) {
            if ( !lfn_add( &walk->run, ent, idx ) ) {
                lfn_orphan( dir, &walk->run, idx, path );
                lfn_begin( &walk->run );
                lfn_add( &walk->run, ent, idx );
            }
            continue;
        }
        if ( sfn_is_dot( ent ) ) {
            lfn_orphan( dir, &walk->run, idx, path );
            lfn_begin( &walk->run );
            continue;
        }
        lfn_first = idx;
        if ( walk->run.active && lfn_match( &walk->run, ent ) ) {
            lfn_oem( &walk->run, walk->name );
            lfn_first = walk->run.start;
        } else {
            lfn_orphan( dir, &walk->run, idx, path );
            sfn_name( ent, walk->name );
        }
        lfn_begin( &walk->run );
        child = path_join( path, walk->name );

        if ( (ent[DE_ATTR] & 0xC0) || (ent[DE_ATTR] & (A_VOLID | A_DIR)) == (A_VOLID | A_DIR) ) {
            if ( pb_found( PB_ATTR, "The entry for %s has attributes that mean nothing.",
                           child ) == ACT_FIX ) {
                ent[DE_ATTR] &= 0x3F;
                if ( ent[DE_ATTR] & A_DIR ) {
                    ent[DE_ATTR] &= ~A_VOLID;
                }
                outcome( dir_write_entry( dir, idx ) );
            }
        }
        if ( (ent[DE_ATTR] & (A_VOLID | A_DIR)) == A_VOLID ) {
            do_label( dir, idx, is_root );
        } else {
            if ( name_bad( ent ) ) {
                if ( pb_found( PB_NAME,
                               "The name of %s has characters a name cannot have, and DOS "
                               "cannot open the %s.", child,
                               (ent[DE_ATTR] & A_DIR) ? "directory" : "file" ) == ACT_FIX ) {
                    if ( name_repair( dir, lfn_first, idx ) == 0 ) {
                        pb_fixed();
                    } else {
                        pb_failed( "No name could be made that the directory does not "
                                   "already have." );
                    }
                }
            }
            if ( ent[DE_ATTR] & A_DIR ) {
                do_subdir( dir, self, lfn_first, idx, child );
            } else {
                do_file( dir, self, idx, child );
            }
        }
        xfree( child );
        fe_progress( claimed, st.used_clus );
    }
    lfn_orphan( dir, &walk->run, idx, path );
    xfree( walk );
}

void check_dirs( void )
{
    DIRBUF dir;
    U32LIST list;
    CHAIN ch;
    int rc;

    rootpath[0] = (char)('A' + vol.drive);
    rootpath[1] = ':';
    rootpath[2] = '\\';
    rootpath[3] = 0;
    mem_set( &list, 0, sizeof( list ) );
    mem_set( &dir, 0, sizeof( dir ) );
    fe_progress( 0, st.used_clus );
    if ( vol.fattype == 32 ) {
        if ( !clus_live( vol.root_clus ) ) {
            pb_found( PB_DIRREAD,
                      "The root directory of drive %c: is recorded as starting at cluster %,u, "
                      "which is not a cluster a directory can be in.  The drive cannot be "
                      "checked.", 'A' + vol.drive, vol.root_clus );
            scan_quit( Q_DRIVE );
        }
        claim_chain( vol.root_clus, &list, &ch );
        st.root_clus = ch.count;
        if ( ch.badlink &&
             pb_found( PB_CHAIN,
                       "The root directory of drive %c: is damaged: after %,u cluster%s the "
                       "chain of clusters the directory is in goes to cluster %,u, which is "
                       "not part of any file.", 'A' + vol.drive, ch.count,
                       ch.count == 1 ? "" : "s", ch.badval ) == ACT_FIX ) {
            fat_set( ch.last, vol.eoc_val );
            outcome( fat_commit() );
        }
        rc = dir_load_list( &dir, &list );
    } else {
        rc = dir_load_root( &dir );
    }
    if ( rc != 0 ) {
        /* nothing below it can be found; a surface scan can still say
           where the trouble is, and move what it can */
        pb_found( PB_DIRREAD,
                  "The root directory of drive %c: cannot be read.  No file or directory on "
                  "the drive has been checked.", 'A' + vol.drive );
        st.ftrunc = 1;
    } else {
        walk_dir( &dir, 0, 0, rootpath, 1 );
    }
    dir_free( &dir );
    list_free( &list );
    fe_progress( 1, 1 );
}

/* ------------------------------------------------------------------ */
/* who has a cluster                                                   */
/* ------------------------------------------------------------------ */

int dir_load_first( DIRBUF *dir, u32 first )
{
    mem_set( dir, 0, sizeof( *dir ) );
    return first == 0 ? dir_load_root( dir ) : dir_load_chain( dir, first );
}

static u8  *seen;               /* a bit for each directory gone into */
static u32  want_clus;

static int seen_take( u32 clus )
{
    if ( seen[clus >> 3] & (1 << (clus & 7)) ) {
        return 1;
    }
    seen[clus >> 3] |= (u8)(1 << (clus & 7));
    return 0;
}

/* is want_clus in the chain from first?  *prev = the one before it */
static int chain_has( u32 first, u32 *prev )
{
    u32 clus = first, next, steps = 0;

    *prev = 0;
    while ( CLUS_OK( clus ) && steps++ <= vol.nclus ) {
        if ( clus == want_clus ) {
            return 1;
        }
        next = fat_get( clus );
        if ( IS_EOC( next ) ) {
            break;
        }
        *prev = clus;
        clus = next;
    }
    return 0;
}

static int own_walk( u32 dir_first, const char *path, OWNER *own, int level )
{
    WALKBUF *walk;
    DIRBUF dir;
    u32 idx, first, prev;
    char *child;
    int found = 0;
    u8 *ent;

    if ( level > MAX_DEPTH ) {
        return 0;
    }
    if ( dir_load_first( &dir, dir_first ) != 0 ) {
        dir_free( &dir );
        return 0;
    }
    walk = (WALKBUF *)xalloc( sizeof( WALKBUF ) );
    lfn_begin( &walk->run );
    for ( idx = 0; idx < dir.nent && !found; idx++ ) {
        ent = dir.data + idx * DE_LEN;
        if ( ent[0] == 0 ) {
            break;
        }
        if ( ent[0] == DEL_MARK ) {
            lfn_begin( &walk->run );
            continue;
        }
        if ( IS_LFN( ent ) ) {
            if ( !lfn_add( &walk->run, ent, idx ) ) {
                lfn_begin( &walk->run );
                lfn_add( &walk->run, ent, idx );
            }
            continue;
        }
        if ( sfn_is_dot( ent ) || (ent[DE_ATTR] & (A_VOLID | A_DIR)) == A_VOLID ) {
            lfn_begin( &walk->run );
            continue;
        }
        if ( walk->run.active && lfn_match( &walk->run, ent ) ) {
            lfn_oem( &walk->run, walk->name );
        } else {
            sfn_name( ent, walk->name );
        }
        lfn_begin( &walk->run );
        first = entry_first( ent );
        if ( !CLUS_OK( first ) ) {
            continue;
        }
        child = path_join( path, walk->name );
        if ( chain_has( first, &prev ) ) {
            own->found = 1;
            own->dir_first = dir_first;
            own->idx = idx;
            own->prev = prev;
            own->is_dir = (ent[DE_ATTR] & A_DIR) != 0;
            own->path = child;
            found = 1;
            break;
        }
        if ( (ent[DE_ATTR] & A_DIR) && !seen_take( first ) ) {
            found = own_walk( first, child, own, level + 1 );
        }
        xfree( child );
        scan_tick();
    }
    xfree( walk );
    dir_free( &dir );
    return found;
}

/* The first file or directory, in the order the tree is walked, whose
   chain holds the cluster.  A whole walk for one cluster: bad clusters
   and cross-links are few. */
void owner_find( u32 clus, OWNER *own )
{
    u32 prev;

    mem_set( own, 0, sizeof( *own ) );
    want_clus = clus;
    if ( vol.fattype == 32 && chain_has( vol.root_clus, &prev ) ) {
        own->found = 1;
        own->is_root = 1;
        own->is_dir = 1;
        own->prev = prev;
        own->path = str_dup( rootpath );
        return;
    }
    seen = (u8 *)xzalloc( (vol.maxclus >> 3) + 1 );
    own_walk( 0, rootpath, own, 0 );
    xfree( seen );
    seen = NULL;
}

/* the ".." of every directory in the directory at "first" names "to" */
static int children_repoint( u32 first, u32 to )
{
    DIRBUF dir;
    u8 *sec, *ent;
    u32 idx, child;
    int bad = 0;

    if ( dir_load_first( &dir, first ) != 0 ) {
        dir_free( &dir );
        return -1;
    }
    sec = (u8 *)xalloc( vol.bps );
    for ( idx = 0; idx < dir.nent; idx++ ) {
        ent = dir.data + idx * DE_LEN;
        if ( ent[0] == 0 ) {
            break;
        }
        if ( ent[0] == DEL_MARK || IS_LFN( ent ) || sfn_is_dot( ent ) ||
             !(ent[DE_ATTR] & A_DIR) || (ent[DE_ATTR] & A_VOLID) ) {
            continue;
        }
        child = entry_first( ent );
        if ( !CLUS_OK( child ) || read_secs( clus_lba( child ), 1, sec ) != 0 ) {
            continue;
        }
        if ( vol.bps >= 2 * DE_LEN && sfn_is_dot( sec + DE_LEN ) == 2 &&
             entry_first( sec + DE_LEN ) != to ) {
            entry_set_first( sec + DE_LEN, to );
            if ( write_secs( clus_lba( child ), 1, sec, WR_DIR ) != 0 ) {
                bad = 1;
            }
        }
    }
    xfree( sec );
    dir_free( &dir );
    return bad ? -1 : 0;
}

/*
 * The cluster "from" has become "to": whatever named it names "to".
 * The caller has already written to's data and linked to onward, and
 * commits the FAT afterwards.
 */
int owner_repoint( const OWNER *own, u32 from, u32 to )
{
    DIRBUF dir;
    u8 *sec;
    int rc = 0;

    (void)from;
    if ( own->prev != 0 ) {
        fat_set( own->prev, to );
        return 0;
    }
    if ( own->is_root ) {
        /* the FAT32 root starts where the boot sector says */
        sec = (u8 *)xalloc( vol.bps );
        if ( read_secs( 0, 1, sec ) != 0 ) {
            xfree( sec );
            return -1;
        }
        WR32( sec + 44, to );
        rc = write_secs( 0, 1, sec, WR_UNKNOWN ) != 0 ? -1 : 0;
        if ( rc == 0 && vol.backup_sec && read_secs( vol.backup_sec, 1, sec ) == 0 &&
             RD32( sec + 44 ) == from ) {
            WR32( sec + 44, to );
            write_secs( vol.backup_sec, 1, sec, WR_UNKNOWN );
        }
        xfree( sec );
        if ( rc == 0 ) {
            vol.root_clus = to;
            rc = children_repoint( to, 0 );
        }
        return rc;
    }
    if ( dir_load_first( &dir, own->dir_first ) != 0 || own->idx >= dir.nent ) {
        dir_free( &dir );
        return -1;
    }
    entry_set_first( dir.data + own->idx * DE_LEN, to );
    rc = dir_write_entry( &dir, own->idx );
    dir_free( &dir );
    if ( rc == 0 && own->is_dir ) {
        /* a directory's first cluster holds its own "." */
        sec = (u8 *)xalloc( vol.bps );
        if ( read_secs( clus_lba( to ), 1, sec ) == 0 && sfn_is_dot( sec ) == 1 ) {
            entry_set_first( sec, to );
            if ( write_secs( clus_lba( to ), 1, sec, WR_DIR | WR_NOUNDO ) != 0 ) {
                rc = -1;
            }
        }
        xfree( sec );
        if ( children_repoint( to, to ) != 0 ) {
            rc = -1;
        }
    }
    return rc;
}
