/*
 * VOL.C - a volume: its geometry, its sectors, its file allocation
 * table, and directories read into memory.  SCANDISK's VOL.C, with two
 * differences that follow from what DEFRAG does with a drive:
 *
 * THE WHOLE FAT IS IN MEMORY, whatever its size.  ScanDisk reads a
 * FAT32 FAT through a window, because it looks at most of it once;
 * DEFRAG asks about clusters all over the drive for every file it
 * moves, and a window would be reading the same blocks for ever.  Four
 * bytes a cluster, and a flag for each sector a move has changed, so
 * that fat_commit() writes those sectors and no others.
 *
 * THERE IS NO UNDO FILE.  A move is put on the disk in an order that
 * leaves every file whole wherever it is stopped (MOVE.C), which is the
 * only kind of undo a program that rewrites a whole drive can have.
 *
 * DFBAD=X:n[,n...] in the environment makes those sectors of drive X:
 * fail to read and write.  It is how the tests have a bad sector to
 * meet; there is no other way to give DOSBox-X or 86Box one.
 */
#include "defrag.h"

VOLUME vol;

/* no directory is read past 65536 entries, as in the DOS kernel */
#define MAX_DIR_BYTES (65536ul * 32ul)

/* ------------------------------------------------------------------ */
/* sectors that are to fail, for the tests                             */
/* ------------------------------------------------------------------ */

#define BAD_MAX 64

static u32 bad_secs[BAD_MAX];
static int bad_count;

static void bad_load( int drive )
{
    const char *list = sys_getenv( "DFBAD" );
    u32 val;

    bad_count = 0;
    if ( list == NULL || ch_upper( list[0] ) != 'A' + drive || list[1] != ':' ) {
        return;
    }
    list += 2;
    while ( *list >= '0' && *list <= '9' && bad_count < BAD_MAX ) {
        for ( val = 0; *list >= '0' && *list <= '9'; list++ ) {
            val = val * 10 + (u32)(*list - '0');
        }
        bad_secs[bad_count++] = val;
        if ( *list == ',' ) {
            list++;
        }
    }
}

static int bad_in( u32 lba, u32 count )
{
    int index;

    for ( index = 0; index < bad_count; index++ ) {
        if ( bad_secs[index] >= lba && bad_secs[index] - lba < count ) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* boot sector                                                         */
/* ------------------------------------------------------------------ */

static int parse_bpb( const u8 *boot )
{
    u32 bps = RD16( boot + 11 ), spc = boot[13], rsvd = RD16( boot + 14 );
    u32 nfats = boot[16], rootents = RD16( boot + 17 );
    u32 tot16 = RD16( boot + 19 ), fatsz16 = RD16( boot + 22 ),
        tot32 = RD32( boot + 32 );
    u32 fatsz, fats, tot, rootsecs, data, nclus, cap, maxv, ext;
    u64 fatbytes;

    if ( bps < 128 || bps > 4096 || (bps & (bps - 1)) ) {
        return -1;
    }
    if ( spc == 0 || (spc & (spc - 1)) ) {
        return -1;
    }
    if ( rsvd == 0 || nfats == 0 ) {
        return -1;
    }
    fatsz = fatsz16 ? fatsz16 : RD32( boot + 36 );
    tot = tot16 ? tot16 : tot32;
    if ( fatsz == 0 || tot == 0 || fatsz > 0xFFFFFFFFul / nfats ) {
        return -1;
    }
    rootsecs = (rootents * 32 + bps - 1) / bps;
    /* each region must fit in what is left of the volume, so that no
       sum can wrap and put the root or the data over the FATs */
    fats = nfats * fatsz;
    if ( rsvd >= tot || fats >= tot - rsvd || rootsecs >= tot - rsvd - fats ) {
        return -1;
    }
    data = rsvd + fats + rootsecs;
    nclus = (tot - data) / spc;
    if ( nclus == 0 ) {
        return -1;
    }

    /* a zero 16-bit FAT size marks the FAT32 BPB layout (as the DOS 7.1
       kernel decides); otherwise the cluster count picks FAT12/16 */
    if ( fatsz16 == 0 ) {
        vol.fattype = 32;
        maxv = 0x0FFFFFF6ul;
    } else if ( nclus < 4085 ) {
        vol.fattype = 12;
        maxv = 0xFF6;
    } else {
        vol.fattype = 16;
        maxv = 0xFFF6;
    }

    /* never trust more clusters than the FAT can describe */
    fatbytes = mul32( fatsz, bps );
    if ( hi32( fatbytes ) ) {
        cap = 0xFFFFFFFFul;
    } else if ( vol.fattype == 12 ) {
        cap = lo32( fatbytes ) / 3 * 2;
    } else {
        cap = lo32( fatbytes ) / (u32)(vol.fattype / 8);
    }
    if ( nclus + 1 > maxv ) {
        nclus = maxv - 1;
    }
    if ( nclus + 2 > cap ) {
        nclus = cap - 2;
    }

    vol.bps = bps;
    vol.spc = spc;
    vol.bpc = bps * spc;
    vol.rsvd = rsvd;
    vol.nfats = nfats;
    vol.fatsz = fatsz;
    vol.root_ents = rootents;
    vol.root_sec = rsvd + nfats * fatsz;
    vol.root_secs = rootsecs;
    vol.data_sec = data;
    vol.tot_sec = tot;
    vol.nclus = nclus;
    vol.maxclus = nclus + 1;
    vol.mirror = 1;
    vol.active_fat = 0;
    vol.fsinfo_sec = 0;
    vol.root_clus = 0;

    switch ( vol.fattype ) {
    case 12:
        vol.eoc_min = 0xFF8;    vol.bad_val = 0xFF7;    vol.eoc_val = 0xFFF;
        break;
    case 16:
        vol.eoc_min = 0xFFF8;   vol.bad_val = 0xFFF7;   vol.eoc_val = 0xFFFF;
        break;
    default:
        vol.eoc_min = 0x0FFFFFF8ul;
        vol.bad_val = 0x0FFFFFF7ul;
        vol.eoc_val = 0x0FFFFFFFul;
        break;
    }

    if ( vol.fattype == 32 ) {
        ext = RD16( boot + 40 );
        if ( ext & 0x80 ) {
            vol.mirror = 0;
            vol.active_fat = ext & 0x0F;
            if ( vol.active_fat >= nfats ) {
                vol.active_fat = 0;
            }
        }
        vol.root_clus = RD32( boot + 44 );
        vol.fsinfo_sec = RD16( boot + 48 );
        if ( vol.fsinfo_sec == 0xFFFF || vol.fsinfo_sec >= rsvd ) {
            vol.fsinfo_sec = 0;
        }
    }
    return 0;
}

/*
 * Build a boot sector BPB from an INT 21h/7302h extended DPB (the DPB
 * proper, past the buffer's length word).  Used only when the boot
 * sector itself carries no usable BPB.
 */
void bpb_from_dpb( const u8 *dpb, u8 *sector )
{
    u32 spc = (u32)dpb[0x04] + 1, fat16 = RD16( dpb + 0x0F );
    u32 first_data, maxclus, tot;

    mem_set( sector, 0, 512 );
    if ( fat16 ) {
        first_data = RD16( dpb + 0x0B );
        maxclus = RD16( dpb + 0x0D );
    } else {
        first_data = RD32( dpb + 0x29 );
        maxclus = RD32( dpb + 0x2D );
    }
    tot = first_data + (maxclus - 1) * spc;
    WR16( sector + 11, RD16( dpb + 0x02 ) );
    sector[13] = (u8)spc;
    WR16( sector + 14, RD16( dpb + 0x06 ) );
    sector[16] = dpb[0x08];
    WR16( sector + 17, RD16( dpb + 0x09 ) );
    if ( fat16 && tot < 0x10000ul ) {
        WR16( sector + 19, tot );
    } else {
        WR32( sector + 32, tot );
    }
    sector[21] = dpb[0x17];
    if ( fat16 ) {
        WR16( sector + 22, fat16 );
    } else {
        WR32( sector + 36, RD32( dpb + 0x31 ) );
        WR16( sector + 40, RD16( dpb + 0x23 ) );
        WR32( sector + 44, RD32( dpb + 0x35 ) );
        WR16( sector + 48, RD16( dpb + 0x25 ) );
    }
}

static u8  *sec_dirty;          /* a flag for each sector of the FAT */
static int  any_dirty;

int vol_open( int drive )
{
    u8 *boot = (u8 *)xzalloc( 4096 );
    u8 *dpb_bpb = (u8 *)xzalloc( 512 );
    int rc = 0, have_dpb;

    mem_set( &vol, 0, sizeof( vol ) );
    vol.drive = drive;
    bad_load( drive );
    have_dpb = sys_get_bpb( drive, dpb_bpb ) == 0;
    if ( sys_read_sec( drive, 0, 1, 512, boot ) != 0 || bad_in( 0, 1 ) ||
         parse_bpb( boot ) != 0 ) {
        if ( !have_dpb || parse_bpb( dpb_bpb ) != 0 ) {
            rc = -1;
        }
    } else if ( have_dpb && RD16( dpb_bpb + 11 ) != vol.bps ) {
        /* the system transfers sectors in its own (DPB) size; a boot
           sector that disagrees must not size the transfer buffers */
        if ( parse_bpb( dpb_bpb ) != 0 ) {
            rc = -1;
        }
    }
    xfree( dpb_bpb );
    xfree( boot );
    return rc;
}

void vol_close( void )
{
    xfree( vol.fat );
    vol.fat = NULL;
    xfree( sec_dirty );
    sec_dirty = NULL;
    any_dirty = 0;
}

/* ------------------------------------------------------------------ */
/* sector and cluster I/O                                              */
/* ------------------------------------------------------------------ */

u32 clus_lba( u32 clus )
{
    return vol.data_sec + (clus - 2) * vol.spc;
}

int read_secs( u32 lba, u32 count, void *buf )
{
    u32 per = SYS_MAX_XFER / vol.bps, nsec;
    u8 *dst = (u8 *)buf;
    int rc;

    if ( per == 0 ) {
        per = 1;
    }
    while ( count ) {
        nsec = count < per ? count : per;
        if ( bad_in( lba, nsec ) ) {
            return 23;                          /* a data error */
        }
        rc = sys_read_sec( vol.drive, lba, nsec, vol.bps, dst );
        if ( rc != 0 ) {
            return rc;
        }
        lba += nsec;
        count -= nsec;
        dst += nsec * vol.bps;
    }
    return 0;
}

int write_secs( u32 lba, u32 count, const void *buf, int kind )
{
    u32 per = SYS_MAX_XFER / vol.bps, nsec;
    const u8 *src = (const u8 *)buf;
    int rc;

    if ( per == 0 ) {
        per = 1;
    }
    while ( count ) {
        nsec = count < per ? count : per;
        if ( bad_in( lba, nsec ) ) {
            return 23;
        }
        rc = sys_write_sec( vol.drive, lba, nsec, vol.bps, src, kind );
        if ( rc != 0 ) {
            return rc;
        }
        lba += nsec;
        count -= nsec;
        src += nsec * vol.bps;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the file allocation table                                           */
/* ------------------------------------------------------------------ */

u32 fat_get( u32 clus )
{
    const u8 *src;
    u32 val;

    if ( clus > vol.maxclus ) {
        return vol.eoc_val;
    }
    switch ( vol.fattype ) {
    case 12:
        src = vol.fat + clus + (clus >> 1);
        val = src[0] | ((u32)src[1] << 8);
        return (clus & 1) ? val >> 4 : val & 0xFFF;
    case 16:
        return RD16( vol.fat + clus * 2 );
    default:
        return RD32( vol.fat + clus * 4 ) & 0x0FFFFFFFul;
    }
}

void fat_set( u32 clus, u32 val )
{
    u8 *dst;
    u32 old, off;

    if ( clus > vol.maxclus ) {
        return;
    }
    switch ( vol.fattype ) {
    case 12:
        off = clus + (clus >> 1);
        dst = vol.fat + off;
        if ( clus & 1 ) {
            dst[0] = (u8)((dst[0] & 0x0F) | ((val & 0x0F) << 4));
            dst[1] = (u8)(val >> 4);
        } else {
            dst[0] = (u8)val;
            dst[1] = (u8)((dst[1] & 0xF0) | ((val >> 8) & 0x0F));
        }
        sec_dirty[off / vol.bps] = 1;           /* an entry can lie across two */
        sec_dirty[(off + 1) / vol.bps] = 1;
        break;
    case 16:
        off = clus * 2;
        WR16( vol.fat + off, val );
        sec_dirty[off / vol.bps] = 1;
        break;
    default:
        off = clus * 4;
        dst = vol.fat + off;
        old = RD32( dst );
        WR32( dst, (old & 0xF0000000ul) | (val & 0x0FFFFFFFul) );
        sec_dirty[off / vol.bps] = 1;
        break;
    }
    any_dirty = 1;
}

/* how much of a FAT describes clusters, in whole sectors */
static void fat_extent( void )
{
    u32 need, secs;

    if ( vol.fattype == 12 ) {
        need = vol.maxclus + (vol.maxclus >> 1) + 2;
    } else {
        need = (vol.maxclus + 1) * (u32)(vol.fattype / 8);
    }
    secs = (need + vol.bps - 1) / vol.bps;
    if ( secs > vol.fatsz ) {
        secs = vol.fatsz;
    }
    vol.fatbytes = secs * vol.bps;
}

/* the copies the volume keeps: all of them, or FAT32's one */
static int copy_kept( u32 copy )
{
    return vol.mirror || copy == vol.active_fat;
}

/*
 * Entry 1 is not a cluster's: its top bits are the "shut down cleanly"
 * and "no disk errors" flags.  The kernel turns the first of them off
 * before the first sector is written behind its back, and on again at
 * the next commit point when nothing is owed - and after each write
 * that lands on the flag it reads the flag back, and turns it off once
 * more if what was written had it on.  So the copy in memory has it
 * off from the start, where the kernel keeps the flag at all: the
 * first sector of the FAT is the one a move near the front of the
 * drive rewrites again and again.
 */
static void flag_off( void )
{
    if ( !(sys_vol_flags( vol.drive ) & VLS_KEPT) ) {
        return;
    }
    if ( vol.fattype == 16 ) {
        vol.fat[3] &= 0x7F;
    } else if ( vol.fattype == 32 ) {
        vol.fat[7] &= 0xF7;
    }
}

int fat_load( void )
{
    u32 nsec, copy = vol.mirror ? 0 : vol.active_fat;

    vol_close();
    fat_extent();
    nsec = vol.fatbytes / vol.bps;
    vol.fat = (u8 *)try_alloc( vol.fatbytes + 2 );
    sec_dirty = (u8 *)try_alloc( nsec + 2 );
    if ( vol.fat == NULL || sec_dirty == NULL ) {
        out_of_memory();
    }
    mem_set( sec_dirty, 0, nsec + 2 );
    vol.fat[vol.fatbytes] = vol.fat[vol.fatbytes + 1] = 0;
    if ( read_secs( vol.rsvd + copy * vol.fatsz, nsec, vol.fat ) != 0 ) {
        return -1;
    }
    flag_off();
    return 0;
}

int fat_commit( void )
{
    u32 copy, index, first, run, nsec = vol.fatbytes / vol.bps;
    int bad = 0;

    if ( !any_dirty ) {
        return 0;
    }
    for ( copy = 0; copy < vol.nfats; copy++ ) {
        if ( !copy_kept( copy ) ) {
            continue;
        }
        first = vol.rsvd + copy * vol.fatsz;
        for ( index = 0; index < nsec; index += run ) {
            run = 1;
            if ( !sec_dirty[index] ) {
                continue;
            }
            while ( index + run < nsec && sec_dirty[index + run] ) {
                run++;
            }
            if ( write_secs( first + index, run, vol.fat + index * vol.bps, WR_FAT ) != 0 ) {
                bad = 1;
            }
        }
    }
    mem_set( sec_dirty, 0, nsec );
    any_dirty = 0;
    return bad ? -1 : 0;
}

/* The other copies, a piece at a time, against the one in memory: how
   many sectors of them say something else.  The flags of entry 1 are
   not part of what a FAT says. */
u32 fat_differing( void )
{
    u32 piece = 16384u - 16384u % vol.bps, off, len, copy, sec, nsec, differing = 0;
    u32 loaded = vol.mirror ? 0 : vol.active_fat;
    u8 *other, *mine = (u8 *)xalloc( vol.bps );

    other = (u8 *)xalloc( piece );
    for ( copy = 0; copy < vol.nfats; copy++ ) {
        if ( !copy_kept( copy ) || copy == loaded ) {
            continue;
        }
        for ( off = 0; off < vol.fatbytes; off += piece ) {
            len = vol.fatbytes - off < piece ? vol.fatbytes - off : piece;
            nsec = len / vol.bps;
            if ( read_secs( vol.rsvd + copy * vol.fatsz + off / vol.bps, nsec, other ) != 0 ) {
                differing += nsec;
                continue;
            }
            for ( sec = 0; sec < nsec; sec++ ) {
                mem_cpy( mine, vol.fat + off + sec * vol.bps, vol.bps );
                if ( off == 0 && sec == 0 ) {
                    if ( vol.fattype == 16 ) {
                        mine[3] = other[3];
                    } else if ( vol.fattype == 32 ) {
                        mine[7] = other[7];
                    }
                }
                if ( mem_cmp( mine, other + sec * vol.bps, vol.bps ) != 0 ) {
                    differing++;
                }
            }
        }
    }
    xfree( other );
    xfree( mine );
    return differing;
}

/* FAT32: the FSInfo sector's count of free clusters, and where a search
   for one might start - a drive that has been packed has them all
   behind its last file */
void fsinfo_update( u32 free_clusters, u32 next_free )
{
    u8 *fsi;

    /* the FSInfo layout assumes at least a 512-byte sector */
    if ( vol.fattype != 32 || vol.fsinfo_sec == 0 || vol.bps < 512 ) {
        return;
    }
    fsi = (u8 *)xalloc( vol.bps );
    if ( read_secs( vol.fsinfo_sec, 1, fsi ) == 0 &&
         RD32( fsi ) == 0x41615252ul && RD32( fsi + 484 ) == 0x61417272ul &&
         (RD32( fsi + 488 ) != free_clusters || RD32( fsi + 492 ) != next_free) ) {
        WR32( fsi + 488, free_clusters );
        WR32( fsi + 492, next_free );
        write_secs( vol.fsinfo_sec, 1, fsi, WR_UNKNOWN );
    }
    xfree( fsi );
}

/* ------------------------------------------------------------------ */
/* directory buffers                                                   */
/* ------------------------------------------------------------------ */

void dir_free( DIRBUF *dir )
{
    xfree( dir->data );
    dir->data = NULL;
    list_free( &dir->clus );
    dir->nent = 0;
}

int dir_load_list( DIRBUF *dir, const U32LIST *clus )
{
    u32 index, nclus = clus->count;

    mem_set( dir, 0, sizeof( *dir ) );
    if ( nclus > MAX_DIR_BYTES / vol.bpc ) {
        nclus = MAX_DIR_BYTES / vol.bpc;
    }
    for ( index = 0; index < nclus; index++ ) {
        list_add( &dir->clus, clus->items[index] );
    }
    dir->nent = nclus * (vol.bpc / 32);
    dir->data = (u8 *)try_alloc( nclus ? nclus * vol.bpc : 32 );
    if ( dir->data == NULL ) {
        out_of_memory();
    }
    for ( index = 0; index < nclus; index++ ) {
        if ( read_secs( clus_lba( clus->items[index] ), vol.spc,
                        dir->data + index * vol.bpc ) != 0 ) {
            return -1;
        }
    }
    return 0;
}

/* the FAT12/16 root: the sectors between the FATs and the clusters */
int dir_load_root( DIRBUF *dir )
{
    mem_set( dir, 0, sizeof( *dir ) );
    dir->fixed_root = 1;
    dir->nent = vol.root_ents;
    dir->data = (u8 *)try_alloc( vol.root_secs ? vol.root_secs * vol.bps : 32 );
    if ( dir->data == NULL ) {
        out_of_memory();
    }
    return read_secs( vol.root_sec, vol.root_secs, dir->data ) != 0 ? -1 : 0;
}

/* write the sector holding entry idx back to the disk */
int dir_write_entry( DIRBUF *dir, u32 idx )
{
    u32 off = idx * 32, lba;

    if ( dir->fixed_root ) {
        lba = vol.root_sec + off / vol.bps;
    } else {
        lba = clus_lba( dir->clus.items[off / vol.bpc] ) + (off % vol.bpc) / vol.bps;
    }
    return write_secs( lba, 1, dir->data + (off / vol.bps) * vol.bps, WR_DIR ) != 0 ? -1 : 0;
}

/* one sector of a directory out of "image", which is laid out as the
   directory's own copy is but need not be it */
int dir_write_sec( DIRBUF *dir, const u8 *image, u32 sec )
{
    u32 lba, per = vol.spc;

    if ( dir->fixed_root ) {
        lba = vol.root_sec + sec;
    } else {
        lba = clus_lba( dir->clus.items[sec / per] ) + sec % per;
    }
    return write_secs( lba, 1, image + sec * vol.bps, WR_DIR ) != 0 ? -1 : 0;
}

/* The whole directory, where it is, from its first sector to its last.
   That order matters: see DIRS.C. */
int dir_write_all( DIRBUF *dir )
{
    u32 index;

    if ( dir->fixed_root ) {
        return write_secs( vol.root_sec, vol.root_secs, dir->data, WR_DIR ) != 0 ? -1 : 0;
    }
    for ( index = 0; index < dir->clus.count; index++ ) {
        if ( write_secs( clus_lba( dir->clus.items[index] ), vol.spc,
                         dir->data + index * vol.bpc, WR_DIR ) != 0 ) {
            return -1;
        }
    }
    return 0;
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
