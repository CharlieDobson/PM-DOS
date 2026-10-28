/*
 * VOL.C - a volume: its geometry, its sectors, its file allocation
 * table, and directories read into memory.  CHKDSK's FAT.C, changed in
 * the one way that matters:
 *
 * NOTHING IS CORRECTED IN MEMORY THAT IS NOT GOING TO THE DISK.  CHKDSK
 * puts its corrections into the FAT it holds whether or not it may
 * write, and works the rest of its report out from that.  ScanDisk asks
 * about each problem, and "Don't Fix It" has to leave the disk AND the
 * rest of the check with the volume as it is - so the FAT held here is
 * the disk's, a repair changes it and fat_commit() writes exactly the
 * sectors that repair touched, to every copy the volume keeps.  A
 * check can therefore be left at any point between two repairs.
 *
 * EVERY WRITE GOES THROUGH write_secs(), which hands the Undo file what
 * is about to be written over before it writes (UNDO.C).
 *
 * SDBAD=X:n[,n...] in the environment makes those sectors of drive X:
 * fail to read and write.  It is how the tests have a bad sector to
 * find; there is no other way to give DOSBox-X or 86Box one.
 */
#include "scandisk.h"

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
    const char *list = sys_getenv( "SDBAD" );
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
    const u8 *lab = NULL;
    u64 fatbytes;
    int index;

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
    vol.backup_sec = 0;
    vol.root_clus = 0;
    vol.has_serial = 0;
    vol.label[0] = 0;

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
        vol.backup_sec = RD16( boot + 50 );
        if ( vol.backup_sec == 0xFFFF || vol.backup_sec >= rsvd ) {
            vol.backup_sec = 0;
        }
        if ( boot[66] == 0x29 || boot[66] == 0x28 ) {
            vol.has_serial = 1;
            vol.serial = RD32( boot + 67 );
            if ( boot[66] == 0x29 ) {
                lab = boot + 71;
            }
        }
    } else if ( boot[38] == 0x29 || boot[38] == 0x28 ) {
        vol.has_serial = 1;
        vol.serial = RD32( boot + 39 );
        if ( boot[38] == 0x29 ) {
            lab = boot + 43;
        }
    }
    if ( lab ) {
        mem_cpy( vol.label, lab, 11 );
        vol.label[11] = 0;
        for ( index = 10; index >= 0 && vol.label[index] == ' '; index-- ) {
            vol.label[index] = 0;
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

static void fat_drop( void );

int vol_open( int drive )
{
    u8 *boot = (u8 *)xzalloc( 4096 );
    u8 *dpb_bpb = (u8 *)xzalloc( 512 );
    int rc = 0, have_dpb, has_serial;
    u32 serial;
    char label[12];

    mem_set( &vol, 0, sizeof( vol ) );
    vol.drive = drive;
    vol.boot_ok = 1;
    bad_load( drive );
    have_dpb = sys_get_bpb( drive, dpb_bpb ) == 0;
    if ( sys_read_sec( drive, 0, 1, 512, boot ) != 0 || bad_in( 0, 1 ) ||
         parse_bpb( boot ) != 0 ) {
        vol.boot_ok = 0;
        if ( !have_dpb || parse_bpb( dpb_bpb ) != 0 ) {
            rc = -1;
        }
#ifndef HOSTBUILD
    } else if ( !have_dpb ) {
        /* the system describes every FAT volume it has mounted; without
           that the size it transfers sectors in is not known */
        rc = -1;
#endif
    } else if ( have_dpb && RD16( dpb_bpb + 11 ) != vol.bps ) {
        /* the system transfers sectors in its own (DPB) size; a boot
           sector that disagrees must not size the transfer buffers */
        has_serial = vol.has_serial;
        serial = vol.serial;
        str_cpy( label, vol.label );
        if ( parse_bpb( dpb_bpb ) == 0 ) {
            vol.has_serial = has_serial;
            vol.serial = serial;
            str_cpy( vol.label, label );
        } else {
            rc = -1;
        }
    }
    xfree( dpb_bpb );
    xfree( boot );
    return rc;
}

void vol_close( void )
{
    fat_drop();
    xfree( vol.map );
    vol.map = NULL;
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
    if ( !(kind & WR_NOUNDO) ) {
        undo_note( lba, count, buf );
    }
    while ( count ) {
        nsec = count < per ? count : per;
        if ( bad_in( lba, nsec ) ) {
            return 23;
        }
        rc = sys_write_sec( vol.drive, lba, nsec, vol.bps, src, kind & 0xFF );
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

/*
 * A FAT12 or FAT16 FAT is held whole - 6K and 128K at the most - with a
 * flag for each of its sectors that a repair has changed.  A FAT32 FAT
 * has no such ceiling, so it is read through a WINDOW of FW_BLOCK byte
 * blocks, and the only table that grows with the volume is the cluster
 * map, at one byte a cluster.  A block that has been changed is never
 * dropped from the window; fat_commit() after each repair is what keeps
 * those few.
 */
#define FW_BLOCK    4096u               /* a multiple of any sector size */
#define FW_CLEAN    64u                 /* blocks held before one is reused */
#define FW_NONE     0xFFFFFFFFul

typedef struct {
    u32  off;                   /* FAT byte offset of the block             */
    u32  used;                  /* last use, for least recently used        */
    int  dirty;
    u8  *data;
} FWBLK;

static FWBLK *fw;
static u32    fw_n, fw_cap, fw_tick, fw_last;
static u32    fw_copy;          /* the FAT copy the working FAT was read from */
static u8    *sec_dirty;        /* FAT12/16: a flag for each FAT sector     */
static int    any_dirty;

static void fat_drop( void )
{
    u32 index;

    for ( index = 0; index < fw_n; index++ ) {
        xfree( fw[index].data );
    }
    xfree( fw );
    fw = NULL;
    fw_n = fw_cap = fw_tick = fw_last = 0;
    xfree( vol.fat );
    vol.fat = NULL;
    xfree( sec_dirty );
    sec_dirty = NULL;
    any_dirty = 0;
}

/* sectors in the block at byte offset off: the last one may be short */
static u32 fw_secs( u32 off )
{
    u32 len = vol.fatbytes - off;

    return (len < FW_BLOCK ? len : FW_BLOCK) / vol.bps;
}

/* the block holding FAT byte off, read in if need be */
static FWBLK *fw_block( u32 off )
{
    u32 base = off - off % FW_BLOCK, index, victim = FW_NONE, oldest = FW_NONE;
    FWBLK *blk;

    fw_tick++;
    if ( fw_last < fw_n && fw[fw_last].off == base ) {
        fw[fw_last].used = fw_tick;
        return &fw[fw_last];
    }
    for ( index = 0; index < fw_n; index++ ) {
        if ( fw[index].off == base ) {
            fw[index].used = fw_tick;
            fw_last = index;
            return &fw[index];
        }
        if ( !fw[index].dirty && fw[index].used < oldest ) {
            oldest = fw[index].used;
            victim = index;
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
    /* check_fats() read this copy through once already, so a failure
       here is a disk that has changed under us */
    if ( read_secs( vol.rsvd + fw_copy * vol.fatsz + base / vol.bps,
                    fw_secs( base ), blk->data ) != 0 ) {
        mem_set( blk->data, 0, FW_BLOCK );
        blk->off = FW_NONE;                     /* not one to find again */
        log_line( "A file allocation table of drive %c: stopped answering.", 'A' + vol.drive );
        scan_quit( Q_DRIVE );
    }
    return blk;
}

/* FAT16/32: the bytes of FAT entry clus; a write marks them changed */
static u8 *fat_entry( u32 clus, int write )
{
    u32 off = clus * (u32)(vol.fattype / 8);
    FWBLK *blk;

    if ( vol.fat != NULL ) {
        if ( write ) {
            sec_dirty[off / vol.bps] = 1;
        }
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
        WR16( fat_entry( clus, 1 ), val );
        break;
    default:
        dst = fat_entry( clus, 1 );
        old = RD32( dst );
        WR32( dst, (old & 0xF0000000ul) | (val & 0x0FFFFFFFul) );
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

int fat_load( u32 copy )
{
    fat_drop();
    fat_extent();
    fw_copy = copy;
    if ( vol.fattype == 32 ) {
        return 0;                               /* a block at a time, as asked */
    }
    vol.fat = (u8 *)try_alloc( vol.fatbytes + 2 );
    sec_dirty = (u8 *)try_alloc( vol.fatbytes / vol.bps + 2 );
    if ( vol.fat == NULL || sec_dirty == NULL ) {
        out_of_memory();
    }
    mem_set( sec_dirty, 0, vol.fatbytes / vol.bps + 2 );
    vol.fat[vol.fatbytes] = vol.fat[vol.fatbytes + 1] = 0;
    return read_secs( vol.rsvd + copy * vol.fatsz, vol.fatbytes / vol.bps, vol.fat ) != 0 ? -1 : 0;
}

/* the copies the volume keeps: all of them, or FAT32's one */
static int copy_kept( u32 copy )
{
    return vol.mirror || copy == vol.active_fat;
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
        if ( vol.fat != NULL ) {
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
        } else {
            for ( index = 0; index < fw_n; index++ ) {
                if ( fw[index].dirty && fw[index].off != FW_NONE &&
                     write_secs( first + fw[index].off / vol.bps, fw_secs( fw[index].off ),
                                 fw[index].data, WR_FAT ) != 0 ) {
                    bad = 1;
                }
            }
        }
    }
    if ( sec_dirty ) {
        mem_set( sec_dirty, 0, nsec );
    }
    for ( index = 0; index < fw_n; index++ ) {
        fw[index].dirty = 0;
    }
    any_dirty = 0;
    return bad ? -1 : 0;
}

void fat_forget( void )
{
    u32 index;

    if ( !any_dirty ) {
        return;
    }
    /* what was changed and not written is read again when it is wanted */
    if ( vol.fat != NULL ) {
        read_secs( vol.rsvd + fw_copy * vol.fatsz, vol.fatbytes / vol.bps, vol.fat );
        mem_set( sec_dirty, 0, vol.fatbytes / vol.bps );
    }
    for ( index = 0; index < fw_n; index++ ) {
        if ( fw[index].dirty ) {
            fw[index].dirty = 0;
            fw[index].off = FW_NONE;
        }
    }
    any_dirty = 0;
}

/* a piece of the working FAT, "len" bytes from byte "off" */
static const u8 *fat_piece( u32 off )
{
    return vol.fat != NULL ? vol.fat + off : fw_block( off )->data;
}

/* Entry 1 is not a cluster's: its top bits are the "shut down cleanly"
   and "no disk errors" flags, which the kernel turns off and on as the
   volume is written to.  Two copies that differ only there are the
   same FAT, so both get the bits set before they are compared. */
static void flags_mask( u8 *piece, u32 off )
{
    if ( off != 0 ) {
        return;
    }
    if ( vol.fattype == 16 ) {
        piece[3] |= 0xC0;
    } else if ( vol.fattype == 32 ) {
        piece[7] |= 0x0C;
    }
}

/* entries of a piece of FAT that point at nothing a chain could hold */
static u32 count_invalid( const u8 *piece, u32 off, u32 len )
{
    u32 clus, last, val, bad = 0;

    if ( vol.fattype == 12 ) {
        for ( clus = 2; clus <= vol.maxclus; clus++ ) {
            val = piece[clus + (clus >> 1)] | ((u32)piece[clus + (clus >> 1) + 1] << 8);
            val = (clus & 1) ? val >> 4 : val & 0xFFF;
            if ( val != 0 && val < vol.bad_val && !CLUS_OK( val ) ) {
                bad++;
            }
        }
        return bad;
    }
    clus = off / (u32)(vol.fattype / 8);
    last = (off + len) / (u32)(vol.fattype / 8);
    for ( ; clus < last && clus <= vol.maxclus; clus++, piece += vol.fattype / 8 ) {
        if ( clus < 2 ) {
            continue;
        }
        val = vol.fattype == 16 ? RD16( piece ) : RD32( piece ) & 0x0FFFFFFFul;
        if ( val != 0 && val < vol.bad_val && !CLUS_OK( val ) ) {
            bad++;
        }
    }
    return bad;
}

/*
 * Every copy read through, a piece at a time, and each compared with
 * the first: how many sectors differ, which copies will not read, and
 * how many entries of each point nowhere - which is how the better of
 * two that disagree is told.  A FAT12 FAT is one piece, because its
 * entries do not end where sectors do.
 */
int fat_compare( FATSTAT stat[], u32 *differing )
{
    u32 piece, off, len, copy, sec, nsec;
    u8 *base, *other;
    int base_ok;

    fat_extent();
    piece = vol.fattype == 12 ? vol.fatbytes : FW_BLOCK;
    base = (u8 *)try_alloc( piece + 2 );
    other = (u8 *)try_alloc( piece + 2 );
    if ( base == NULL || other == NULL ) {
        out_of_memory();
    }
    *differing = 0;
    for ( copy = 0; copy < vol.nfats; copy++ ) {
        stat[copy].unreadable = 0;
        stat[copy].invalid = 0;
    }
    for ( off = 0; off < vol.fatbytes; off += piece ) {
        len = vol.fatbytes - off < piece ? vol.fatbytes - off : piece;
        nsec = len / vol.bps;
        base_ok = 0;
        for ( copy = 0; copy < vol.nfats; copy++ ) {
            u8 *buf = base_ok ? other : base;

            if ( !copy_kept( copy ) ) {
                continue;
            }
            buf[len] = buf[len + 1] = 0;
            if ( read_secs( vol.rsvd + copy * vol.fatsz + off / vol.bps, nsec, buf ) != 0 ) {
                stat[copy].unreadable = 1;
                continue;
            }
            stat[copy].invalid += count_invalid( buf, off, len );
            flags_mask( buf, off );
            if ( !base_ok ) {
                base_ok = 1;
                continue;
            }
            for ( sec = 0; sec < nsec; sec++ ) {
                if ( mem_cmp( base + sec * vol.bps, other + sec * vol.bps, vol.bps ) != 0 ) {
                    (*differing)++;
                }
            }
        }
        fe_progress( off + len, vol.fatbytes );
    }
    xfree( other );
    xfree( base );
    return 0;
}

/* Every copy made the same as the working FAT: only the sectors that
   differ are written, so the Undo file keeps only those. */
int fat_rewrite( void )
{
    u32 piece = vol.fattype == 12 ? vol.fatbytes : FW_BLOCK;
    u32 off, len, copy, sec, nsec, lba;
    u8 *mine, *theirs;
    int bad = 0, same;

    mine = (u8 *)try_alloc( piece );
    theirs = (u8 *)try_alloc( piece );
    if ( mine == NULL || theirs == NULL ) {
        out_of_memory();
    }
    for ( off = 0; off < vol.fatbytes; off += piece ) {
        len = vol.fatbytes - off < piece ? vol.fatbytes - off : piece;
        nsec = len / vol.bps;
        for ( copy = 0; copy < vol.nfats; copy++ ) {
            if ( !copy_kept( copy ) ) {
                continue;
            }
            for ( sec = 0; sec < nsec; sec++ ) {
                lba = vol.rsvd + copy * vol.fatsz + off / vol.bps + sec;
                mem_cpy( mine, fat_piece( off ) + sec * vol.bps, vol.bps );
                same = 0;
                if ( read_secs( lba, 1, theirs ) == 0 ) {
                    if ( off == 0 && sec == 0 ) {
                        /* the flags stay as that copy has them */
                        if ( vol.fattype == 16 ) {
                            mine[3] = theirs[3];
                        } else if ( vol.fattype == 32 ) {
                            mine[7] = theirs[7];
                        }
                    }
                    same = mem_cmp( mine, theirs, vol.bps ) == 0;
                }
                if ( !same && write_secs( lba, 1, mine, WR_FAT ) != 0 ) {
                    bad = 1;
                }
            }
        }
        scan_tick();
    }
    xfree( theirs );
    xfree( mine );
    return bad ? -1 : 0;
}

/* the next cluster after "clus" that the FAT has free and nothing in
   the map has a claim on; round to the start once */
u32 fat_free_after( u32 clus )
{
    u32 tried, cur = clus;

    for ( tried = 0; tried < vol.nclus; tried++ ) {
        cur++;
        if ( cur > vol.maxclus || cur < 2 ) {
            cur = 2;
        }
        if ( fat_get( cur ) == 0 &&
             (vol.map == NULL || !(vol.map[cur] & (MF_USED | MF_LOST | MF_BAD))) ) {
            return cur;
        }
    }
    return 0;
}

u32 fat_count_free( void )
{
    u32 clus, nfree = 0;

    for ( clus = 2; clus <= vol.maxclus; clus++ ) {
        if ( fat_get( clus ) == 0 ) {
            nfree++;
        }
    }
    return nfree;
}

/* FAT32: keep the FSInfo free cluster count honest after a repair */
void fsinfo_update( u32 free_clusters )
{
    u8 *fsi;

    /* the FSInfo layout assumes at least a 512-byte sector */
    if ( vol.fattype != 32 || vol.fsinfo_sec == 0 || vol.bps < 512 ) {
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

/* follow a chain through the FAT as it stands */
int dir_load_chain( DIRBUF *dir, u32 first )
{
    U32LIST chain;
    u32 clus = first, next;
    int rc;

    mem_set( &chain, 0, sizeof( chain ) );
    while ( CLUS_OK( clus ) && chain.count < vol.nclus ) {
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
        return dir_load_chain( dir, vol.root_clus );
    }
    mem_set( dir, 0, sizeof( *dir ) );
    dir->fixed_root = 1;
    dir->nent = vol.root_ents;
    dir->data = (u8 *)xalloc( vol.root_secs ? vol.root_secs * vol.bps : 32 );
    return read_secs( vol.root_sec, vol.root_secs, dir->data ) != 0 ? -1 : 0;
}

/* FAT32 only: append a zeroed cluster to a directory (the root) */
int dir_extend( DIRBUF *dir )
{
    u32 clus, nclus = dir->clus.count;
    u8 *grown;

    if ( vol.fattype != 32 || nclus == 0 ) {
        return -1;
    }
    if ( !IS_EOC( fat_get( dir->clus.items[nclus - 1] ) ) ) {
        return -1;                              /* more chain than was loaded */
    }
    clus = fat_free_after( dir->clus.items[nclus - 1] );
    if ( clus == 0 ) {
        return -1;
    }
    grown = (u8 *)try_alloc( (nclus + 1) * vol.bpc );
    if ( grown == NULL ) {
        return -1;
    }
    mem_cpy( grown, dir->data, nclus * vol.bpc );
    mem_set( grown + nclus * vol.bpc, 0, vol.bpc );
    if ( write_secs( clus_lba( clus ), vol.spc, grown + nclus * vol.bpc,
                     WR_DIR | WR_NOUNDO ) != 0 ) {
        xfree( grown );
        return -1;
    }
    fat_set( clus, vol.eoc_val );
    fat_set( dir->clus.items[nclus - 1], clus );
    if ( vol.map ) {
        vol.map[clus] |= MF_USED;
    }
    xfree( dir->data );
    dir->data = grown;
    list_add( &dir->clus, clus );
    dir->nent += vol.bpc / 32;
    return 0;
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
