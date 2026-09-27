/*
 * LOST.C - lost allocation units (DOS CHKMAP, FINDCHAIN and CHAINREC).
 *
 * Anything the FAT shows as allocated that no directory entry claimed is
 * lost.  Lost clusters are grouped into chains; with /F the user may
 * turn every chain into a FILEnnnn.CHK file in the root directory, or
 * free them.  Without /F (or when part of the tree could not be read,
 * so "lost" may not really be lost) nothing is changed and CHKDSK only
 * reports how much space would be freed.
 */
#include "chkdsk.h"

/*
 * Make every lost chain well formed: follow each chain from its head,
 * end it where it leaves the lost set, loops, or merges into a chain
 * already seen.  A chain that runs into another chain's head absorbs
 * it; one that runs back into its own head is a loop and is cut.
 * Returns the number of chains.
 */
static u32 find_chains( int simulate )
{
    u32 clus, cur, next, chains = 0;

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
                if ( !simulate ) {
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
static void next_name( DIRBUF *root, u8 *name, u32 *seq )
{
    u32 i, num;
    int used;

    for ( ; *seq < 10000; (*seq)++ ) {
        mem_cpy( name, "FILE0000CHK", 11 );
        num = *seq;
        name[7] = (u8)('0' + num % 10);  num /= 10;
        name[6] = (u8)('0' + num % 10);  num /= 10;
        name[5] = (u8)('0' + num % 10);  num /= 10;
        name[4] = (u8)('0' + num % 10);
        used = 0;
        for ( i = 0; i < root->nent; i++ ) {
            const u8 *ent = root->data + i * DE_LEN;
            if ( ent[0] == 0 ) {
                break;
            }
            if ( ent[0] != DEL_MARK && !IS_LFN( ent ) &&
                 mem_cmp( ent, name, 11 ) == 0 ) {
                used = 1;
                break;
            }
        }
        if ( !used ) {
            (*seq)++;
            return;
        }
    }
}

/* free slot in the root: deleted, or the end marker (DOS CHAINREC) */
static u32 free_slot( DIRBUF *root, u32 *pos )
{
    u32 i;

    for ( ;; ) {
        for ( i = *pos; i < root->nent; i++ ) {
            u8 ch = root->data[i * DE_LEN];
            if ( ch == DEL_MARK || ch == 0 ) {
                *pos = i + 1;
                return i;
            }
        }
        *pos = root->nent;
        if ( dir_extend( root ) != 0 ) {          /* FAT32 only */
            return 0xFFFFFFFFul;
        }
        st.root_clus++;
    }
}

static void recover( u32 chains, u32 lost )
{
    DIRBUF root;
    u32 clus, slot, pos = 0, seq = 0, made = 0;
    u64 sz;
    u8 *ent, name[11];

    st.orph_clus = lost;
    if ( dir_load_root( &root ) != 0 ) {
        dir_free( &root );
        out_blank();
        out_line( H_OUT, M_CREAT );
        return;
    }
    for ( clus = 2; clus <= vol.maxclus && made < chains; clus++ ) {
        if ( (vol.map[clus] & (MF_HEAD | MF_LOST)) != (MF_HEAD | MF_LOST) ) {
            continue;
        }
        slot = free_slot( &root, &pos );
        if ( slot == 0xFFFFFFFFul ) {
            out_blank();
            out_line( H_OUT, M_CREAT );
            break;
        }
        next_name( &root, name, &seq );
        ent = root.data + slot * DE_LEN;
        mem_set( ent, 0, DE_LEN );
        mem_cpy( ent, name, 11 );
        entry_stamp( ent );
        entry_set_first( ent, clus );
        sz = mul32( chain_length( clus ), vol.bpc );
        WR32( ent + DE_SIZE, hi32( sz ) ? 0xFFFFFFFFul : lo32( sz ) );
        dir_fix( &root, slot );
        made++;
    }
    dir_free( &root );
    st.orph_cnt = made;
}

void lost_chains( void )
{
    u32 clus, val, lost = 0, chains;
    int simulate = !opt.dofix || st.ftrunc;
    char num[40], num2[40], pad[48];

    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        if ( vol.map[clus] & MF_USED ) {
            continue;
        }
        val = fat_get( clus );
        if ( val == 0 ) {
            continue;
        }
        if ( val == vol.bad_val ) {
            vol.map[clus] |= MF_BAD;
            st.bad_clus++;
        } else {
            vol.map[clus] |= MF_LOST;
            lost++;
        }
    }
    if ( lost == 0 ) {
        return;
    }

    nofix_notice();
    out_blank();
    chains = find_chains( simulate );
    out_msg( H_OUT, M_ORPH, fmt_num( lost, num ), fmt_num( chains, num2 ),
             NULL );

    if ( !simulate && prompt_yn( M_FREEMES ) ) {
        recover( chains, lost );
        return;
    }
    if ( !simulate ) {
        for ( clus = 2; clus <= vol.maxclus; clus++ ) {
            if ( vol.map[clus] & MF_LOST ) {
                fat_set( clus, 0 );
            }
        }
    } else {
        st.lost_kept = lost;
    }
    fmt_right( fmt_num( mul32( lost, vol.bpc ), num ), 13, pad );
    out_msg( H_OUT, simulate ? M_WOULDFREE : M_FREED, pad, NULL, NULL );
}
