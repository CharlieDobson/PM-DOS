/*
 * FILESYS.C - the fourth check: what the directory walk left over.
 *
 * LOST CLUSTERS are CHKDSK's (LOST.C there: DOS CHKMAP, FINDCHAIN and
 * CHAINREC).  Anything the FAT shows in use that no directory entry
 * claimed is lost; the lost clusters are grouped into chains, and each
 * chain can be made a FILEnnnn.CHK in the root directory, or freed.
 * When part of the tree could not be read, "lost" may only mean "in a
 * directory nobody could look in", and nothing is offered.
 *
 * CROSS-LINKED FILES are two entries whose chains run into the same
 * clusters.  CHKDSK names them and leaves them; ScanDisk gives the
 * second one clusters of its own, a copy of what the two shared, so
 * that each is a whole file again and the one whose data it really is
 * has it.  A directory that runs into another file's clusters is ended
 * where it does instead: a copy of a file is no part of a directory.
 */
#include "scandisk.h"

/* ------------------------------------------------------------------ */
/* lost clusters                                                       */
/* ------------------------------------------------------------------ */

/*
 * Follow each lost chain from its head: it ends where it leaves the
 * lost set, loops, or merges into a chain already seen.  A chain that
 * runs into another chain's head absorbs it.  With "cut" the FAT is
 * made to say so; without, the chains are only counted.
 */
static u32 find_chains( int cut )
{
    u32 clus, cur, next, chains = 0;

    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        vol.map[clus] &= ~(MF_SEEN | MF_HEAD);
    }
    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        if ( !(vol.map[clus] & MF_LOST) || (vol.map[clus] & MF_SEEN) ) {
            continue;
        }
        vol.map[clus] |= MF_HEAD | MF_SEEN;
        chains++;
        for ( cur = clus;; ) {
            next = fat_get( cur );
            if ( IS_EOC( next ) ) {
                break;
            }
            if ( !CLUS_OK( next ) || next == cur || next == clus ||
                 !(vol.map[next] & MF_LOST) ||
                 ((vol.map[next] & MF_SEEN) && !(vol.map[next] & MF_HEAD)) ) {
                if ( cut ) {
                    fat_set( cur, vol.eoc_val );
                }
                break;
            }
            if ( vol.map[next] & MF_HEAD ) {
                vol.map[next] &= ~MF_HEAD;
                chains--;
                break;
            }
            vol.map[next] |= MF_SEEN;
            cur = next;
        }
    }
    return chains;
}

static u32 chain_length( u32 clus )
{
    u32 len = 0;

    while ( CLUS_OK( clus ) && len <= vol.nclus ) {
        len++;
        clus = fat_get( clus );
    }
    return len;
}

/* next FILEnnnn.CHK name not already present in the root */
static int next_name( DIRBUF *root, u8 *name, u32 *seq )
{
    u32 idx, num;
    const u8 *ent;
    int used;

    for ( ; *seq < 10000; (*seq)++ ) {
        mem_cpy( name, "FILE0000CHK", 11 );
        num = *seq;
        name[7] = (u8)('0' + num % 10);  num /= 10;
        name[6] = (u8)('0' + num % 10);  num /= 10;
        name[5] = (u8)('0' + num % 10);  num /= 10;
        name[4] = (u8)('0' + num % 10);
        used = 0;
        for ( idx = 0; idx < root->nent; idx++ ) {
            ent = root->data + idx * DE_LEN;
            if ( ent[0] == 0 ) {
                break;
            }
            if ( ent[0] != DEL_MARK && !IS_LFN( ent ) && mem_cmp( ent, name, 11 ) == 0 ) {
                used = 1;
                break;
            }
        }
        if ( !used ) {
            (*seq)++;
            return 1;
        }
    }
    return 0;
}

/* a free slot in the root: a deleted entry, or the end marker */
static u32 free_slot( DIRBUF *root, u32 *pos )
{
    u32 idx;
    u8 first;

    for ( ;; ) {
        for ( idx = *pos; idx < root->nent; idx++ ) {
            first = root->data[idx * DE_LEN];
            if ( first == DEL_MARK || first == 0 ) {
                *pos = idx + 1;
                /* the entry after a used end marker is the end now */
                if ( first == 0 && idx + 1 < root->nent ) {
                    root->data[(idx + 1) * DE_LEN] = 0;
                }
                return idx;
            }
        }
        *pos = root->nent;
        if ( dir_extend( root ) != 0 ) {        /* FAT32 only */
            return 0xFFFFFFFFul;
        }
        st.root_clus++;
    }
}

/* each chain a file in the root; -> how many were made */
static u32 recover( u32 chains )
{
    DIRBUF root;
    u32 clus, slot, pos = 0, seq = 0, made = 0, len, walk;
    u64 size;
    u8 *ent, name[11];
    int bad = 0;

    if ( dir_load_root( &root ) != 0 ) {
        dir_free( &root );
        return 0;
    }
    for ( clus = 2; clus <= vol.maxclus && made < chains; clus++ ) {
        if ( (vol.map[clus] & (MF_HEAD | MF_LOST)) != (MF_HEAD | MF_LOST) ) {
            continue;
        }
        slot = free_slot( &root, &pos );
        if ( slot == 0xFFFFFFFFul || !next_name( &root, name, &seq ) ) {
            break;
        }
        ent = root.data + slot * DE_LEN;
        mem_set( ent, 0, DE_LEN );
        mem_cpy( ent, name, 11 );
        ent[DE_ATTR] = A_ARCH;
        entry_stamp( ent );
        entry_set_first( ent, clus );
        len = chain_length( clus );
        size = mul32( len, vol.bpc );
        WR32( ent + DE_SIZE, hi32( size ) ? 0xFFFFFFFFul : lo32( size ) );
        if ( dir_write_entry( &root, slot ) != 0 ||
             (slot + 1 < root.nent && (slot + 1) * DE_LEN / vol.bps != slot * DE_LEN / vol.bps &&
              dir_write_entry( &root, slot + 1 ) != 0) ) {
            bad = 1;
            break;
        }
        for ( walk = clus; CLUS_OK( walk ) && len; len-- ) {
            vol.map[walk] = (u8)((vol.map[walk] & ~(MF_LOST | MF_HEAD)) | MF_USED);
            walk = fat_get( walk );
        }
        st.orph_clus += chain_length( clus );
        made++;
        scan_tick();
    }
    dir_free( &root );
    (void)bad;
    return made;
}

static void lost_clusters( void )
{
    u32 clus, val, lost = 0, chains, made;
    char bytes[40];
    int act;

    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        if ( !(vol.map[clus] & (MF_USED | MF_BAD)) ) {
            val = fat_get( clus );
            if ( val != 0 && val != vol.bad_val ) {
                vol.map[clus] |= MF_LOST;
                lost++;
            }
        }
        if ( (clus & 0xFFF) == 0 ) {
            fe_progress( clus, vol.maxclus * 2 );
        }
    }
    if ( lost == 0 ) {
        return;
    }
    if ( st.ftrunc ) {
        /* a directory nobody could read may be where they belong */
        st.lost_kept = lost;
        log_line( "" );
        log_line( "%,u clusters of drive %c: are in use by nothing that could be read.",
                  lost, 'A' + vol.drive );
        log_para( "Part of the directory tree could not be read, so the clusters may belong "
                  "to files there and have been left as they are." );
        return;
    }

    chains = find_chains( 0 );
    fmt_bytes( mul32( lost, vol.bpc ), bytes );
    act = pb_found( PB_LOST,
                    "Drive %c: has %,u lost cluster%s in %,u chain%s: %s bytes that the file "
                    "allocation table shows as in use, and that no file or directory has.",
                    'A' + vol.drive, lost, lost == 1 ? "" : "s", chains, chains == 1 ? "" : "s",
                    bytes );
    if ( act == ACT_SAVE ) {
        chains = find_chains( 1 );
        if ( fat_commit() != 0 ) {
            pb_failed( "A sector of the file allocation table could not be written." );
            st.lost_kept = lost;
            return;
        }
        made = recover( chains );
        fat_commit();                           /* a FAT32 root that grew */
        st.orph_cnt = made;
        st.lost_kept = lost - st.orph_clus;
        if ( made == chains ) {
            pb_fixed();
            log_line( "   Saved as %,u file%s named FILEnnnn.CHK in the root directory.",
                      made, made == 1 ? "" : "s" );
        } else {
            pb_failed( "The root directory has room for only %,u of the %,u files.  Move "
                       "some files out of the root directory and run ScanDisk again.",
                       made, chains );
        }
    } else if ( act == ACT_DELETE ) {
        for ( clus = 2; clus <= vol.maxclus; clus++ ) {
            if ( vol.map[clus] & MF_LOST ) {
                fat_set( clus, 0 );
                vol.map[clus] &= ~(MF_LOST | MF_HEAD | MF_SEEN);
            }
        }
        if ( fat_commit() == 0 ) {
            pb_fixed();
        } else {
            pb_failed( "A sector of the file allocation table could not be written." );
        }
    } else {
        st.lost_kept = lost;
    }
}

/* ------------------------------------------------------------------ */
/* cross-linked files                                                  */
/* ------------------------------------------------------------------ */

/* the entry is still what the walk saw: its chain reaches the shared
   cluster the same way */
static int cross_stands( const CROSSREC *cr, DIRBUF *dir )
{
    const u8 *ent;

    if ( dir_load_first( dir, cr->dir_first ) != 0 || cr->idx >= dir->nent ) {
        return 0;
    }
    ent = dir->data + cr->idx * DE_LEN;
    if ( ent[0] == 0 || ent[0] == DEL_MARK || IS_LFN( ent ) ) {
        return 0;
    }
    return cr->prev ? fat_get( cr->prev ) == cr->clus : entry_first( ent ) == cr->clus;
}

/*
 * The second file given clusters of its own; NULL, or why not.
 *
 * It gets as many of the shared clusters as its size says it has, and
 * no more: a file of ten clusters that runs into the middle of a file
 * of a thousand is a file of ten clusters afterwards, not one that has
 * grown the other's tail.  When its own clusters already hold all its
 * size asks for, the chain is simply ended before the shared one.
 */
static const char *cross_copy( const CROSSREC *cr, DIRBUF *dir )
{
    U32LIST src, dst;
    u32 clus, index, fresh = 1, own = 0, need, take, size, right;
    u8 *buf, *ent = dir->data + cr->idx * DE_LEN;
    const char *why = NULL;
    u64 bytes;

    mem_set( &src, 0, sizeof( src ) );
    mem_set( &dst, 0, sizeof( dst ) );
    size = RD32( ent + DE_SIZE );
    if ( cr->prev ) {
        for ( clus = entry_first( ent ); CLUS_OK( clus ) && own <= vol.nclus; clus = fat_get( clus ) ) {
            own++;
            if ( clus == cr->prev ) {
                break;
            }
        }
    }
    need = size / vol.bpc + (size % vol.bpc ? 1 : 0);
    take = need > own ? need - own : 0;
    for ( clus = cr->clus; CLUS_OK( clus ) && src.count < take; clus = fat_get( clus ) ) {
        if ( vol.map[clus] & MF_CUR ) {
            break;                              /* the chain came round again */
        }
        vol.map[clus] |= MF_CUR;
        list_add( &src, clus );
        if ( IS_EOC( fat_get( clus ) ) ) {
            break;
        }
    }
    for ( index = 0; index < src.count; index++ ) {
        vol.map[src.items[index]] &= ~MF_CUR;
    }
    for ( index = 0; index < src.count; index++ ) {
        fresh = fat_free_after( fresh );
        if ( fresh == 0 ) {
            why = "There is not enough free space on the drive for a copy of the clusters.";
            break;
        }
        vol.map[fresh] |= MF_USED;              /* not to be handed out twice */
        list_add( &dst, fresh );
    }
    buf = (u8 *)try_alloc( vol.bpc );
    if ( buf == NULL ) {
        out_of_memory();
    }
    for ( index = 0; why == NULL && index < src.count; index++ ) {
        if ( read_secs( clus_lba( src.items[index] ), vol.spc, buf ) != 0 ) {
            mem_set( buf, 0, vol.bpc );         /* the surface scan's to report */
        }
        if ( write_secs( clus_lba( dst.items[index] ), vol.spc, buf, WR_DATA | WR_NOUNDO ) != 0 ) {
            why = "A cluster of the copy could not be written.";
        }
        scan_tick();
    }
    xfree( buf );
    if ( why == NULL ) {
        for ( index = 0; index < dst.count; index++ ) {
            fat_set( dst.items[index], index + 1 < dst.count ? dst.items[index + 1] : vol.eoc_val );
        }
        if ( cr->prev ) {
            fat_set( cr->prev, dst.count ? dst.items[0] : vol.eoc_val );
        } else {
            entry_set_first( ent, dst.count ? dst.items[0] : 0 );
        }
        /* and its size, should the clusters it has now not bear it out */
        bytes = mul32( own + dst.count, vol.bpc );
        if ( (u64)size > bytes || bytes - size >= (u64)vol.bpc ) {
            right = hi32( bytes ) ? 0xFFFFFFFFul : lo32( bytes );
            WR32( ent + DE_SIZE, right );
        }
        if ( fat_commit() != 0 || dir_write_entry( dir, cr->idx ) != 0 ) {
            why = "A sector could not be written.";
        }
    } else {
        for ( index = 0; index < dst.count; index++ ) {
            vol.map[dst.items[index]] &= ~MF_USED;
        }
    }
    list_free( &dst );
    list_free( &src );
    return why;
}

static void cross_links( void )
{
    CROSSREC *cr;
    DIRBUF dir;
    OWNER own;
    const char *why;
    u32 index;

    for ( index = 0; index < cross_count; index++ ) {
        cr = &cross_list[index];
        fe_progress( cross_count + index, cross_count * 2 );
        if ( !cross_stands( cr, &dir ) ) {
            dir_free( &dir );
            continue;
        }
        owner_find( cr->clus, &own );
        if ( !own.found || (own.dir_first == cr->dir_first && own.idx == cr->idx) ) {
            xfree( own.path );                  /* the other one has gone */
            dir_free( &dir );
            continue;
        }
        if ( cr->is_dir ) {
            if ( pb_found( PB_CROSS,
                           "The directory %s and %s are cross-linked: from cluster %,u on, "
                           "both are recorded as being in the same clusters.  The directory "
                           "can be ended before them.", cr->path, own.path,
                           cr->clus ) == ACT_FIX ) {
                fat_set( cr->prev, vol.eoc_val );
                if ( fat_commit() == 0 ) {
                    pb_fixed();
                } else {
                    pb_failed( "A sector of the file allocation table could not be written." );
                }
            }
        } else if ( pb_found( PB_CROSS,
                              "The files %s and %s are cross-linked: from cluster %,u on, both "
                              "are recorded as being in the same clusters.  At most one of "
                              "them is whole.", own.path, cr->path, cr->clus ) == ACT_FIX ) {
            why = cross_copy( cr, &dir );
            if ( why == NULL ) {
                pb_fixed();
                log_para( "Each file has clusters of its own now.  Look at both: the one the "
                          "clusters did not belong to has another file's data from that "
                          "point on." );
            } else {
                pb_failed( "%s", why );
            }
        }
        xfree( own.path );
        dir_free( &dir );
    }
}

void check_filesys( void )
{
    fe_progress( 0, 1 );
    lost_clusters();
    cross_links();
    if ( st.fixed && vol.fattype == 32 ) {
        fsinfo_update( fat_count_free() );
    }
    fe_progress( 1, 1 );
}
