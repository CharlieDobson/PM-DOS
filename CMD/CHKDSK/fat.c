/*
 * FAT.C - volume geometry, FAT access and directory buffers.
 */
#include "chkdsk.h"

VOLUME vol;

/* no directory is read past 65536 entries, as in the DOS kernel */
#define MAX_DIR_BYTES (65536ul * 32ul)

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
    vol.media = boot[21];
    vol.mirror = 1;
    vol.active_fat = 0;
    vol.fsinfo_sec = 0;
    vol.root_clus = 0;
    vol.has_serial = 0;

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
        if ( boot[66] == 0x29 || boot[66] == 0x28 ) {
            vol.has_serial = 1;
            vol.serial = RD32( boot + 67 );
        }
    } else if ( boot[38] == 0x29 || boot[38] == 0x28 ) {
        vol.has_serial = 1;
        vol.serial = RD32( boot + 39 );
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

int vol_open( int drive )
{
    u8 *boot = (u8 *)xzalloc( 4096 );
    u8 *dpb_bpb = (u8 *)xzalloc( 512 );
    int rc = 0, have_dpb, has_serial;
    u32 serial;

    vol.drive = drive;
    have_dpb = sys_get_bpb( drive, dpb_bpb ) == 0;
    if ( sys_read_sec( drive, 0, 1, 512, boot ) != 0 ||
         parse_bpb( boot ) != 0 ) {
        if ( !have_dpb || parse_bpb( dpb_bpb ) != 0 ) {
            rc = -1;
        }
    } else if ( have_dpb && RD16( dpb_bpb + 11 ) != vol.bps ) {
        /* the system transfers sectors in its own (DPB) size; a boot
           sector that disagrees must not size the transfer buffers, and
           without a usable DPB the size is not known at all */
        has_serial = vol.has_serial;
        serial = vol.serial;
        if ( parse_bpb( dpb_bpb ) == 0 ) {
            vol.has_serial = has_serial;
            vol.serial = serial;
        } else {
            rc = -1;
        }
    }
    xfree( dpb_bpb );
    xfree( boot );
    return rc;
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

    if ( per == 0 ) {
        per = 1;
    }
    while ( count ) {
        nsec = count < per ? count : per;
        if ( sys_read_sec( vol.drive, lba, nsec, vol.bps, dst ) != 0 ) {
            return -1;
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

    if ( per == 0 ) {
        per = 1;
    }
    while ( count ) {
        nsec = count < per ? count : per;
        if ( sys_write_sec( vol.drive, lba, nsec, vol.bps, src, kind ) != 0 ) {
            return -1;
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

/*
 * A FAT12 or FAT16 FAT is held whole - 6K and 128K at the most.  A FAT32
 * FAT has no such ceiling: it is four bytes a cluster, so an 80,000
 * cluster volume has a 320K FAT, and a program on PM-DOS had a 512K slot
 * for everything (48h grows the slot now).  So a FAT32 FAT is read
 * through a WINDOW of FW_BLOCK
 * byte blocks instead, and the only table that grows with the volume is
 * the cluster map, at one byte a cluster.
 *
 * A block that has been CHANGED is never evicted.  CHKDSK corrects the
 * FAT in memory whether or not it may write - without /F the corrected
 * FAT is what the rest of the check and the report are worked out from,
 * and it is never written - so until fat_write() a changed block is the
 * only copy of the change there is.  When every block in the window has
 * been changed the window grows instead, which takes a FAT in a very bad
 * way and costs no more than holding it whole did.
 */
#define FW_BLOCK    4096u               /* a multiple of any sector size */
#define FW_CLEAN    16u                 /* blocks held before one is reused */
#define FW_NONE     0xFFFFFFFFul

typedef struct {
    u32  off;                   /* FAT byte offset of the block             */
    u32  used;                  /* last use, for least recently used        */
    int  dirty;
    u8  *data;
} FWBLK;

static FWBLK *fw;
static u32    fw_n, fw_cap, fw_tick, fw_last;
static u32    fw_copy;          /* the FAT copy blocks are read from        */

/* sectors in the block at byte offset off: the last one may be short */
static u32 fw_secs( u32 off )
{
    u32 len = vol.fatbytes - off;

    return (len < FW_BLOCK ? len : FW_BLOCK) / vol.bps;
}

/* the block holding FAT byte off, read in if need be */
static FWBLK *fw_block( u32 off )
{
    u32 base = off - off % FW_BLOCK, i, victim = FW_NONE, oldest = FW_NONE;
    FWBLK *blk;
    char num[12];

    fw_tick++;
    if ( fw_last < fw_n && fw[fw_last].off == base ) {
        fw[fw_last].used = fw_tick;
        return &fw[fw_last];
    }
    for ( i = 0; i < fw_n; i++ ) {
        if ( fw[i].off == base ) {
            fw[i].used = fw_tick;
            fw_last = i;
            return &fw[i];
        }
        if ( !fw[i].dirty && fw[i].used < oldest ) {
            oldest = fw[i].used;
            victim = i;
        }
    }
    if ( fw_n < FW_CLEAN || victim == FW_NONE ) {
        if ( fw_n == fw_cap ) {
            fw_cap = fw_cap ? fw_cap * 2 : FW_CLEAN;
            blk = (FWBLK *)xalloc( fw_cap * sizeof( FWBLK ) );
            if ( fw_n ) {
                mem_cpy( blk, fw, fw_n * sizeof( FWBLK ) );
            }
            xfree( fw );
            fw = blk;
        }
        victim = fw_n++;
        fw[victim].data = (u8 *)xalloc( FW_BLOCK );
    }
    blk = &fw[victim];
    blk->off = base;
    blk->used = fw_tick;
    blk->dirty = 0;
    fw_last = victim;
    /* fat_load() read this copy through once already, so a failure here
       is a disk that has changed under us */
    if ( read_secs( vol.rsvd + fw_copy * vol.fatsz + base / vol.bps,
                    fw_secs( base ), blk->data ) != 0 ) {
        out_msg( H_ERR, M_BADR, fmt_dec( fw_copy + 1, num ), NULL, NULL );
        chkdsk_abort();
    }
    return blk;
}

/* FAT16/32: the bytes of FAT entry clus; a write marks its block dirty */
static u8 *fat_entry( u32 clus, int write )
{
    u32 off = clus * (u32)(vol.fattype / 8);
    FWBLK *blk;

    if ( vol.fat != NULL ) {
        return vol.fat + off;
    }
    blk = fw_block( off );
    if ( write ) {
        blk->dirty = 1;
    }
    return blk->data + (off - blk->off);
}

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
        return RD16( fat_entry( clus, 0 ) );
    default:
        return RD32( fat_entry( clus, 0 ) ) & 0x0FFFFFFFul;
    }
}

void fat_set( u32 clus, u32 val )
{
    u8 *dst;
    u32 old;

    if ( clus > vol.maxclus ) {
        return;
    }
    switch ( vol.fattype ) {
    case 12:
        dst = vol.fat + clus + (clus >> 1);
        if ( clus & 1 ) {
            dst[0] = (u8)((dst[0] & 0x0F) | ((val & 0x0F) << 4));
            dst[1] = (u8)(val >> 4);
        } else {
            dst[0] = (u8)val;
            dst[1] = (u8)((dst[1] & 0xF0) | ((val >> 8) & 0x0F));
        }
        break;
    case 16:
        WR16( fat_entry( clus, 1 ), val );
        break;
    default:
        dst = fat_entry( clus, 1 );
        old = RD32( dst );
        WR32( dst, (old & 0xF0000000ul) | (val & 0x0FFFFFFFul) );
        break;
    }
    vol.fat_dirty = 1;
}

/* read the first usable copy of the FAT; messages as DOS CHKDSK.  A
   FAT32 copy is read through as well, a block at a time, so that a copy
   with an unreadable sector is passed over here exactly as it is when
   the FAT is held whole - but nothing of it is kept but the window. */
int fat_load( void )
{
    u32 need, secs, i, copy, off;
    u8 *buf = NULL;
    int bad;
    char num[12];

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
    if ( vol.fattype == 32 ) {
        vol.fat = NULL;
        buf = (u8 *)xalloc( FW_BLOCK );
    } else {
        vol.fat = (u8 *)try_alloc( vol.fatbytes );
        if ( vol.fat == NULL ) {
            out_of_memory();
        }
    }

    for ( i = 0; i < vol.nfats; i++ ) {
        copy = (vol.active_fat + i) % vol.nfats;
        if ( buf != NULL ) {
            bad = 0;
            for ( off = 0; off < vol.fatbytes && !bad; off += FW_BLOCK ) {
                bad = read_secs( vol.rsvd + copy * vol.fatsz + off / vol.bps,
                                 fw_secs( off ), buf ) != 0;
            }
        } else {
            bad = read_secs( vol.rsvd + copy * vol.fatsz, secs,
                             vol.fat ) != 0;
        }
        if ( !bad ) {
            if ( !vol.mirror ) {
                vol.active_fat = copy;
            }
            fw_copy = copy;
            xfree( buf );
            return 0;
        }
        out_msg( H_ERR, M_BADR, fmt_dec( copy + 1, num ), NULL, NULL );
    }
    xfree( buf );
    return -1;
}

/* one FAT copy: the whole table, or every changed block of the window */
static int fat_write_copy( u32 copy )
{
    u32 i, first = vol.rsvd + copy * vol.fatsz;

    if ( vol.fat != NULL ) {
        return write_secs( first, vol.fatbytes / vol.bps, vol.fat, WR_FAT );
    }
    for ( i = 0; i < fw_n; i++ ) {
        if ( fw[i].dirty &&
             write_secs( first + fw[i].off / vol.bps, fw_secs( fw[i].off ),
                         fw[i].data, WR_FAT ) != 0 ) {
            return -1;
        }
    }
    return 0;
}

/* write the FAT back to every copy in use; -1 if no copy was written */
int fat_write( void )
{
    u32 i, count = 0, bad = 0;
    char num[12];

    for ( i = 0; i < vol.nfats; i++ ) {
        if ( !vol.mirror && i != vol.active_fat ) {
            continue;
        }
        count++;
        if ( fat_write_copy( i ) != 0 ) {
            out_msg( H_ERR, M_BADW, fmt_dec( i + 1, num ), NULL, NULL );
            bad++;
        }
    }
    for ( i = 0; i < fw_n; i++ ) {
        fw[i].dirty = 0;
    }
    vol.fat_dirty = 0;
    return (count && bad == count) ? -1 : 0;
}

/* FAT32: keep the FSInfo free cluster count honest after a /F run */
void fsinfo_update( u32 free_clusters )
{
    u8 *fsi;

    /* the FSInfo layout assumes at least a 512-byte sector */
    if ( vol.fattype != 32 || vol.fsinfo_sec == 0 || !vol.writes ||
         vol.bps < 512 ) {
        return;
    }
    fsi = (u8 *)xalloc( vol.bps );
    if ( read_secs( vol.fsinfo_sec, 1, fsi ) == 0 &&
         RD32( fsi ) == 0x41615252ul && RD32( fsi + 484 ) == 0x61417272ul &&
         RD32( fsi + 488 ) != free_clusters ) {
        WR32( fsi + 488, free_clusters );
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
    u32 i, nclus = clus->count;

    mem_set( dir, 0, sizeof( *dir ) );
    if ( nclus > MAX_DIR_BYTES / vol.bpc ) {
        nclus = MAX_DIR_BYTES / vol.bpc;
    }
    for ( i = 0; i < nclus; i++ ) {
        list_add( &dir->clus, clus->items[i] );
    }
    dir->nent = nclus * (vol.bpc / 32);
    dir->data = (u8 *)xalloc( nclus ? nclus * vol.bpc : 32 );
    for ( i = 0; i < nclus; i++ ) {
        if ( read_secs( clus_lba( clus->items[i] ), vol.spc,
                        dir->data + i * vol.bpc ) != 0 ) {
            return -1;
        }
    }
    return 0;
}

/* follow a chain through the in-memory FAT (only clusters already
   claimed by the tree walk; limit 0 = whole chain) */
int dir_load_chain( DIRBUF *dir, u32 first, u32 limit )
{
    U32LIST chain;
    u32 clus = first, next;
    int rc;

    mem_set( &chain, 0, sizeof( chain ) );
    while ( CLUS_OK( clus ) && chain.count < vol.nclus ) {
        if ( limit && chain.count >= limit ) {
            break;
        }
        if ( vol.map && !(vol.map[clus] & MF_USED) ) {
            break;
        }
        if ( chain.count * vol.bpc >= MAX_DIR_BYTES ) {
            break;
        }
        list_add( &chain, clus );
        next = fat_get( clus );
        if ( IS_EOC( next ) ) {
            break;
        }
        clus = next;
    }
    rc = dir_load_list( dir, &chain );
    list_free( &chain );
    return rc;
}

int dir_load_root( DIRBUF *dir )
{
    if ( vol.fattype == 32 ) {
        return dir_load_chain( dir, vol.root_clus, 0 );
    }
    mem_set( dir, 0, sizeof( *dir ) );
    dir->fixed_root = 1;
    dir->nent = vol.root_ents;
    dir->data = (u8 *)xalloc( vol.root_secs ? vol.root_secs * vol.bps : 32 );
    return read_secs( vol.root_sec, vol.root_secs, dir->data );
}

/* FAT32 only: append a zeroed cluster to a directory (the root) */
int dir_extend( DIRBUF *dir )
{
    u32 clus, nclus = dir->clus.count;
    u8 *nd;

    if ( vol.fattype != 32 || nclus == 0 ) {
        return -1;
    }
    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        if ( fat_get( clus ) == 0 &&
             !(vol.map[clus] & (MF_USED | MF_LOST | MF_BAD)) ) {
            break;
        }
    }
    if ( clus > vol.maxclus ) {
        return -1;
    }
    nd = (u8 *)try_alloc( (nclus + 1) * vol.bpc );
    if ( nd == NULL ) {
        return -1;
    }
    mem_cpy( nd, dir->data, nclus * vol.bpc );
    mem_set( nd + nclus * vol.bpc, 0, vol.bpc );
    if ( vol.writes &&
         write_secs( clus_lba( clus ), vol.spc, nd + nclus * vol.bpc,
                     WR_DIR ) != 0 ) {
        xfree( nd );
        return -1;
    }
    fat_set( clus, vol.eoc_val );
    fat_set( dir->clus.items[nclus - 1], clus );
    vol.map[clus] |= MF_USED;
    xfree( dir->data );
    dir->data = nd;
    list_add( &dir->clus, clus );
    dir->nent += vol.bpc / 32;
    return 0;
}

/* write the sector holding entry idx back to disk (only with /F) */
int dir_write_entry( DIRBUF *dir, u32 idx )
{
    u32 off = idx * 32, lba;
    char drv[4];

    if ( !vol.writes ) {
        return 0;
    }
    if ( dir->fixed_root ) {
        lba = vol.root_sec + off / vol.bps;
    } else {
        lba = clus_lba( dir->clus.items[off / vol.bpc] ) +
              (off % vol.bpc) / vol.bps;
    }
    if ( write_secs( lba, 1, dir->data + (off / vol.bps) * vol.bps,
                     WR_DIR ) != 0 ) {
        drv[0] = (char)('A' + vol.drive);
        drv[1] = 0;
        out_msg( H_ERR, M_WRFAULT, drv, NULL, NULL );
        return -1;
    }
    return 0;
}
