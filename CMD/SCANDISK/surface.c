/*
 * SURFACE.C - the surface scan: every sector of the volume is read,
 * and a cluster that will not read is taken out of use.
 *
 * THE SYSTEM AREA comes first - the boot record, the FATs, a FAT12 or
 * FAT16 root directory.  A sector that cannot be read there cannot be
 * moved anywhere: where those are is what makes the disk a volume.  It
 * is reported, and that is all.
 *
 * THE CLUSTERS are read a run at a time, as many as fit in one transfer,
 * NumPasses times over; only when a run fails is each cluster in it
 * tried alone, twice.  With ScanTimeOut a read that takes longer than
 * SLOW_READ counts as a failed one - a drive that needs many retries to
 * get a sector back still returns it, and only the clock shows that.
 *
 * A BAD CLUSTER NOTHING USES is marked bad in the FAT.  One a file is in
 * has what can still be read - sector by sector - copied to a free
 * cluster, which takes its place in the chain, and then is marked bad.
 * The sectors that could not be read are zeroes in the copy, and the
 * log says which file and how much.
 *
 * The scan only reads.  Nothing is written to a cluster that reads.
 */
#include "scandisk.h"

#define SLOW_READ   200         /* hundredths of a second */

static u8  *run_buf;
static u32  moved_clusters;

/* 0 = read, and in good time */
static int timed_read( u32 lba, u32 count )
{
    u32 start, took;
    int rc;

    if ( !opt.timeout ) {
        return read_secs( lba, count, run_buf );
    }
    start = sys_hundredths();
    rc = read_secs( lba, count, run_buf );
    took = sys_hundredths();
    took = took >= start ? took - start : took + 8640000ul - start;     /* past midnight */
    if ( rc == 0 && took > SLOW_READ ) {
        rc = 21;                                /* the drive is not ready */
    }
    return rc;
}

/* a cluster on its own: bad when it fails twice running */
static int cluster_bad( u32 clus )
{
    u32 pass;

    for ( pass = 0; pass < opt.passes; pass++ ) {
        if ( timed_read( clus_lba( clus ), vol.spc ) != 0 &&
             timed_read( clus_lba( clus ), vol.spc ) != 0 ) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the system area                                                     */
/* ------------------------------------------------------------------ */

static const char *area_name( u32 lba, char *buf, u32 max )
{
    if ( lba < vol.rsvd ) {
        return lba == 0 ? "the boot sector" : "the reserved sectors";
    }
    if ( lba < vol.rsvd + vol.nfats * vol.fatsz ) {
        sfmt( buf, max, "copy %u of the file allocation table", (lba - vol.rsvd) / vol.fatsz + 1 );
        return buf;
    }
    return "the root directory";
}

static void system_area( void )
{
    u32 per = SYS_MAX_XFER / vol.bps, lba, count, sec, bad = 0, first_bad = 0;
    char where[60];

    for ( lba = 0; lba < vol.data_sec; lba += count ) {
        count = vol.data_sec - lba < per ? vol.data_sec - lba : per;
        if ( timed_read( lba, count ) != 0 ) {
            for ( sec = lba; sec < lba + count; sec++ ) {
                if ( timed_read( sec, 1 ) != 0 && timed_read( sec, 1 ) != 0 ) {
                    if ( bad++ == 0 ) {
                        first_bad = sec;
                    }
                }
            }
        }
        scan_tick();
    }
    if ( bad ) {
        pb_found( PB_SYSAREA,
                  "%,u sector%s in the system area of drive %c: cannot be read.  The first is "
                  "sector %,u, in %s.", bad, bad == 1 ? "" : "s", 'A' + vol.drive, first_bad,
                  area_name( first_bad, where, sizeof( where ) ) );
    }
}

/* ------------------------------------------------------------------ */
/* a bad cluster                                                       */
/* ------------------------------------------------------------------ */

/* what can be read of "clus" into a free cluster, which takes its
   place; NULL, or why it could not be done */
static const char *relocate( u32 clus, const OWNER *own, u32 *lost_secs )
{
    u8 *buf = (u8 *)try_alloc( vol.bpc );
    u32 sec, fresh = clus, tries, next = fat_get( clus ), scan;
    const char *why = NULL;

    if ( buf == NULL ) {
        out_of_memory();
    }
    *lost_secs = 0;
    for ( sec = 0; sec < vol.spc; sec++ ) {
        if ( read_secs( clus_lba( clus ) + sec, 1, buf + sec * vol.bps ) != 0 &&
             read_secs( clus_lba( clus ) + sec, 1, buf + sec * vol.bps ) != 0 ) {
            mem_set( buf + sec * vol.bps, 0, vol.bps );
            (*lost_secs)++;
        }
    }
    for ( tries = 0;; tries++ ) {
        fresh = fat_free_after( fresh );
        if ( fresh == 0 ) {
            why = "There is no free cluster on the drive to move the data to.";
            break;
        }
        if ( write_secs( clus_lba( fresh ), vol.spc, buf, WR_DATA | WR_NOUNDO ) == 0 &&
             read_secs( clus_lba( fresh ), vol.spc, run_buf ) == 0 &&
             mem_cmp( buf, run_buf, vol.bpc ) == 0 ) {
            break;
        }
        /* a free cluster that will not take the data is no better */
        fat_set( fresh, vol.bad_val );
        vol.map[fresh] |= MF_BAD;
        st.bad_clus++;
        if ( tries == 8 ) {
            why = "No free cluster that was tried could be written.";
            break;
        }
    }
    xfree( buf );
    if ( why != NULL ) {
        fat_commit();
        return why;
    }

    fat_set( fresh, next );
    fat_set( clus, vol.bad_val );
    vol.map[fresh] = (u8)(vol.map[clus] & ~MF_BAD);
    vol.map[clus] = MF_BAD;
    st.bad_clus++;
    if ( own->found ) {
        if ( owner_repoint( own, clus, fresh ) != 0 ) {
            why = "A sector could not be written.";
        }
    } else {
        /* a cluster of a lost chain: whatever links to it, if anything */
        for ( scan = 2; scan <= vol.maxclus; scan++ ) {
            if ( scan != fresh && fat_get( scan ) == clus ) {
                fat_set( scan, fresh );
                break;
            }
        }
    }
    if ( fat_commit() != 0 ) {
        why = "A sector of the file allocation table could not be written.";
    }
    moved_clusters++;
    return why;
}

static void bad_cluster( u32 clus )
{
    u32 val = fat_get( clus ), lost_secs;
    const char *why;
    OWNER own;

    st.surf_bad++;
    fe_surface_at( clus, 1, 1 );
    if ( val == 0 ) {
        if ( pb_found( PB_BADCLUS,
                       "Cluster %,u of drive %c: cannot be read.  Nothing is stored in the "
                       "cluster; it can be marked as bad so that nothing ever is.",
                       clus, 'A' + vol.drive ) == ACT_FIX ) {
            fat_set( clus, vol.bad_val );
            vol.map[clus] |= MF_BAD;
            st.bad_clus++;
            moved_clusters++;                   /* the free count has changed */
            if ( fat_commit() == 0 ) {
                pb_fixed();
            } else {
                pb_failed( "A sector of the file allocation table could not be written." );
            }
        }
        return;
    }
    owner_find( clus, &own );
    if ( pb_found( PB_BADCLUS,
                   "Cluster %,u of drive %c: cannot be read.  The cluster is part of %s.",
                   clus, 'A' + vol.drive,
                   own.found ? own.path : "a chain of lost clusters" ) == ACT_FIX ) {
        why = relocate( clus, &own, &lost_secs );
        if ( why != NULL ) {
            pb_failed( "%s", why );
        } else {
            pb_fixed();
            if ( lost_secs == 0 ) {
                log_para( "All of the cluster was read in the end, and nothing was lost." );
            } else {
                log_line( "   %,u bytes could not be read and are zeroes now.",
                          lost_secs * vol.bps );
            }
        }
    }
    xfree( own.path );
}

/* ------------------------------------------------------------------ */
/* the scan                                                            */
/* ------------------------------------------------------------------ */

void surface_scan( void )
{
    u32 per = SYS_MAX_XFER / vol.bpc, clus, count, pass, index;
    int failed;

    if ( per == 0 ) {
        per = 1;
    }
    run_buf = (u8 *)try_alloc( per * vol.bpc > SYS_MAX_XFER ? per * vol.bpc : SYS_MAX_XFER );
    if ( run_buf == NULL ) {
        out_of_memory();
    }
    moved_clusters = 0;
    st.surface = 2;                             /* until it reaches the end */
    fe_surface_begin();
    system_area();

    for ( clus = 2; clus <= vol.maxclus; clus += count ) {
        /* a cluster already marked bad is not read again */
        if ( vol.map[clus] & MF_BAD ) {
            count = 1;
            st.surf_examined++;
            fe_surface_at( clus, 1, 0 );
            continue;
        }
        for ( count = 1; count < per && clus + count <= vol.maxclus &&
                         !(vol.map[clus + count] & MF_BAD); count++ ) {
        }
        failed = 0;
        for ( pass = 0; pass < opt.passes && !failed; pass++ ) {
            failed = timed_read( clus_lba( clus ), count * vol.spc ) != 0;
        }
        if ( failed ) {
            for ( index = 0; index < count; index++ ) {
                if ( cluster_bad( clus + index ) ) {
                    bad_cluster( clus + index );
                }
            }
        }
        st.surf_examined += count;
        fe_surface_at( clus, count, 0 );
    }
    if ( moved_clusters && vol.fattype == 32 ) {
        fsinfo_update( fat_count_free() );
    }
    xfree( run_buf );
    run_buf = NULL;
    st.surface = 1;
}
