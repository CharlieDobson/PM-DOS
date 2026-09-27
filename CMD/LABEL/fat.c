/*
 * FAT.C - volume geometry, sector I/O, the root directory and the boot
 * sector label field, for FAT12, FAT16 and FAT32.
 *
 * Only the root directory, the boot sector (and on FAT32 its backup
 * and the FSInfo sector) and, when a full FAT32 root directory has to
 * grow, two FAT entries are ever written.
 */
#include "label.h"

VOLUME vol;

/* no directory is read past 65536 entries, as in the DOS kernel */
#define MAX_ROOT_BYTES (65536ul * 32ul)

#define FAT32_EOC_MIN  0x0FFFFFF8ul
#define FAT32_EOC      0x0FFFFFFFul

/* FAT32: a window onto the FAT in use (see fat_window()) */
static u8  *fwin;
static u32  fwin_sec = 0xFFFFFFFFul;   /* first FAT sector in the window */
static u32  fwin_n;

/* ------------------------------------------------------------------ */
/* boot sector                                                         */
/* ------------------------------------------------------------------ */

/* offset of the extended boot signature (29h: serial, label, type) */
static u32 ext_sig_off( const u8 *boot )
{
    return RD16( boot + 22 ) == 0 ? 66 : 38;
}

static int parse_bpb( const u8 *boot )
{
    u32 bps = RD16( boot + 11 ), spc = boot[13], rsvd = RD16( boot + 14 );
    u32 nfats = boot[16], rootents = RD16( boot + 17 );
    u32 tot16 = RD16( boot + 19 ), fatsz16 = RD16( boot + 22 ),
        tot32 = RD32( boot + 32 );
    u32 fatsz, fats, tot, rootsecs, data, nclus, cap, maxv, ext, sig;
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
       sum can wrap and put the "root" or the data over the FATs */
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
    vol.nclus = nclus;
    vol.maxclus = nclus + 1;
    vol.mirror = 1;
    vol.active_fat = 0;
    vol.fsinfo_sec = 0;
    vol.root_clus = 0;

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
    sig = boot[ext_sig_off( boot )];
    vol.has_serial = sig == 0x29 || sig == 0x28;
    vol.serial = vol.has_serial ? RD32( boot + ext_sig_off( boot ) + 1 ) : 0;
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

/* 0 = ok; a read error; or DE_MEDIA when no usable FAT BPB is found */
int vol_open( int drive )
{
    u8 *boot = (u8 *)xzalloc( 4096 );
    u8 *dpb_bpb = (u8 *)xzalloc( 512 );
    int rc = 0, have_dpb, has_serial, err;
    u32 serial;

    /* a FAT window sized and read for an earlier disk is no use */
    xfree( fwin );
    fwin = NULL;
    fwin_sec = 0xFFFFFFFFul;

    vol.drive = drive;
    vol.boot_ok = 0;
    have_dpb = sys_get_bpb( drive, dpb_bpb ) == 0;
    err = sys_read_sec( drive, 0, 1, 512, boot );
    if ( err != 0 || parse_bpb( boot ) != 0 ) {
        if ( !have_dpb || parse_bpb( dpb_bpb ) != 0 ) {
            rc = err ? err : DE_MEDIA;
        }
    } else {
        vol.boot_ok = 1;
        if ( have_dpb && RD16( dpb_bpb + 11 ) != vol.bps ) {
            /* the system transfers sectors in its own (DPB) size; a
               boot sector that disagrees must not size the transfers,
               and without a usable DPB the size is not known at all */
            has_serial = vol.has_serial;
            serial = vol.serial;
            if ( parse_bpb( dpb_bpb ) == 0 ) {
                vol.has_serial = has_serial;
                vol.serial = serial;
            } else {
                rc = DE_MEDIA;
            }
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

static u32 per_xfer( void )
{
    u32 per = SYS_MAX_XFER / vol.bps;
    return per ? per : 1;
}

void read_secs( u32 lba, u32 count, void *buf )
{
    u32 per = per_xfer(), nsec;
    u8 *dst = (u8 *)buf;
    int err;

    while ( count ) {
        nsec = count < per ? count : per;
        err = sys_read_sec( vol.drive, lba, nsec, vol.bps, dst );
        if ( err != 0 ) {
            disk_error( err, 0 );
        }
        lba += nsec;
        count -= nsec;
        dst += nsec * vol.bps;
    }
}

void write_secs( u32 lba, u32 count, const void *buf, int kind )
{
    u32 per = per_xfer(), nsec;
    const u8 *src = (const u8 *)buf;
    int err;

    while ( count ) {
        nsec = count < per ? count : per;
        err = sys_write_sec( vol.drive, lba, nsec, vol.bps, src, kind );
        if ( err != 0 ) {
            disk_error( err, 1 );
        }
        lba += nsec;
        count -= nsec;
        src += nsec * vol.bps;
    }
}

/* ------------------------------------------------------------------ */
/* FAT32 table entries, through a window onto the FAT in use           */
/* ------------------------------------------------------------------ */

/* window holding entry clus; *off = its byte offset in the window */
static u8 *fat_window( u32 clus, u32 *off )
{
    u32 per = per_xfer(), sec = clus * 4 / vol.bps, first;

    if ( fwin == NULL ) {
        fwin = (u8 *)xalloc( per * vol.bps );
    }
    if ( fwin_sec == 0xFFFFFFFFul || sec < fwin_sec ||
         sec >= fwin_sec + fwin_n ) {
        first = sec - sec % per;
        fwin_n = vol.fatsz - first < per ? vol.fatsz - first : per;
        fwin_sec = 0xFFFFFFFFul;
        read_secs( vol.rsvd + vol.active_fat * vol.fatsz + first, fwin_n,
                   fwin );
        fwin_sec = first;
    }
    *off = clus * 4 - fwin_sec * vol.bps;
    return fwin;
}

static u32 fat_get( u32 clus )
{
    u32 off;
    u8 *win = fat_window( clus, &off );

    return RD32( win + off ) & 0x0FFFFFFFul;
}

/* set entry clus in every FAT in use; the top four bits are preserved */
static void fat_set( u32 clus, u32 val )
{
    u32 off, i, sec;
    u8 *win = fat_window( clus, &off ), *sec_buf;

    WR32( win + off,
          (RD32( win + off ) & 0xF0000000ul) | (val & 0x0FFFFFFFul) );
    sec = fwin_sec + off / vol.bps;
    sec_buf = win + (off / vol.bps) * vol.bps;
    for ( i = 0; i < vol.nfats; i++ ) {
        if ( vol.mirror || i == vol.active_fat ) {
            write_secs( vol.rsvd + i * vol.fatsz + sec, 1, sec_buf, WR_FAT );
        }
    }
}

/* ------------------------------------------------------------------ */
/* the root directory                                                  */
/* ------------------------------------------------------------------ */

static void root_alloc( ROOTDIR *root, u32 nsec )
{
    root->nsec = nsec;
    root->data = (u8 *)xzalloc( nsec ? nsec * vol.bps : 32 );
    root->dirty = (u8 *)xzalloc( nsec ? nsec : 1 );
}

void root_load( ROOTDIR *root )
{
    u32 clus, next, i;

    mem_set( root, 0, sizeof( *root ) );
    fwin_sec = 0xFFFFFFFFul;            /* read the FAT afresh as well */
    if ( vol.fattype != 32 ) {
        root_alloc( root, vol.root_secs );
        root->nent = vol.root_ents;
        read_secs( vol.root_sec, vol.root_secs, root->data );
        return;
    }

    /* the chain ends at an end mark; anything else (a free, bad or out
       of range link, a loop, or the size limit) also ends it, but then
       the directory is not grown */
    clus = vol.root_clus;
    while ( CLUS_OK( clus ) &&
            (root->clus.count + 1) * vol.bpc <= MAX_ROOT_BYTES ) {
        for ( i = 0; i < root->clus.count &&
                     root->clus.items[i] != clus; i++ ) {
        }
        if ( i < root->clus.count ) {
            break;
        }
        list_add( &root->clus, clus );
        next = fat_get( clus );
        if ( next >= FAT32_EOC_MIN ) {
            root->can_grow = 1;
            break;
        }
        clus = next;
    }
    root_alloc( root, root->clus.count * vol.spc );
    root->nent = root->clus.count * (vol.bpc / DE_LEN);
    for ( i = 0; i < root->clus.count; i++ ) {
        read_secs( clus_lba( root->clus.items[i] ), vol.spc,
                   root->data + i * vol.bpc );
    }
}

void root_free( ROOTDIR *root )
{
    xfree( root->data );
    xfree( root->dirty );
    list_free( &root->clus );
    mem_set( root, 0, sizeof( *root ) );
}

/* the volume label entries, in directory order, up to the end mark */
int root_next_label( ROOTDIR *root, u32 from )
{
    u32 i;
    const u8 *ent;

    for ( i = from; i < root->nent; i++ ) {
        ent = root->data + i * DE_LEN;
        if ( ent[0] == 0 ) {
            break;
        }
        if ( ent[0] == DEL_MARK || IS_LFN( ent ) ) {
            continue;
        }
        if ( (ent[DE_ATTR] & (A_VOLID | A_DIR)) == A_VOLID ) {
            return (int)i;
        }
    }
    return -1;
}

/* the first deleted or never used entry, as DOS picks for a new one */
int root_free_slot( ROOTDIR *root )
{
    u32 i;
    u8 ch;

    for ( i = 0; i < root->nent; i++ ) {
        ch = root->data[i * DE_LEN];
        if ( ch == 0 || ch == DEL_MARK ) {
            return (int)i;
        }
    }
    return -1;
}

void root_touch( ROOTDIR *root, u32 idx )
{
    root->dirty[idx * DE_LEN / vol.bps] = 1;
}

void root_write( ROOTDIR *root )
{
    u32 i, lba;

    for ( i = 0; i < root->nsec; i++ ) {
        if ( !root->dirty[i] ) {
            continue;
        }
        if ( vol.fattype == 32 ) {
            lba = clus_lba( root->clus.items[i / vol.spc] ) + i % vol.spc;
        } else {
            lba = vol.root_sec + i;
        }
        write_secs( lba, 1, root->data + i * vol.bps, WR_DIR );
        root->dirty[i] = 0;
    }
}

/*
 * FAT32: add a cluster to a full root directory, as the kernel does
 * when it creates an entry there.  The new cluster is zeroed on disk
 * before the FAT links it in, so an interruption leaves at worst an
 * empty directory cluster.  The FSInfo free count and hint follow.
 */
int root_grow( ROOTDIR *root )
{
    u8 *fsi = NULL, *nd, *ny;
    int fsi_ok = 0, found = 0;
    u32 hint = 0, start, clus, nsec, fc;

    if ( vol.fattype != 32 || !root->can_grow || root->clus.count == 0 ||
         (root->clus.count + 1) * vol.bpc > MAX_ROOT_BYTES ) {
        return -1;
    }
    if ( vol.fsinfo_sec && vol.bps >= 512 ) {
        fsi = (u8 *)xalloc( vol.bps );
        read_secs( vol.fsinfo_sec, 1, fsi );
        fsi_ok = RD32( fsi ) == 0x41615252ul &&
                 RD32( fsi + 484 ) == 0x61417272ul;
        if ( fsi_ok ) {
            hint = RD32( fsi + 492 );
        }
    }
    start = CLUS_OK( hint ) ? hint : 2;
    clus = start;
    do {
        if ( fat_get( clus ) == 0 ) {
            found = 1;
            break;
        }
        clus = clus < vol.maxclus ? clus + 1 : 2;
    } while ( clus != start );
    if ( !found ) {
        xfree( fsi );
        return -1;
    }

    nsec = root->nsec;
    nd = (u8 *)xzalloc( (nsec + vol.spc) * vol.bps );
    ny = (u8 *)xzalloc( nsec + vol.spc );
    mem_cpy( nd, root->data, nsec * vol.bps );
    mem_cpy( ny, root->dirty, nsec );

    write_secs( clus_lba( clus ), vol.spc, nd + nsec * vol.bps, WR_DIR );
    fat_set( clus, FAT32_EOC );
    fat_set( root->clus.items[root->clus.count - 1], clus );
    if ( fsi_ok ) {
        fc = RD32( fsi + 488 );
        if ( fc != 0xFFFFFFFFul && fc != 0 ) {
            WR32( fsi + 488, fc - 1 );
        }
        WR32( fsi + 492, clus );        /* most recently allocated */
        write_secs( vol.fsinfo_sec, 1, fsi, WR_UNKNOWN );
    }
    xfree( fsi );

    xfree( root->data );
    xfree( root->dirty );
    root->data = nd;
    root->dirty = ny;
    root->nsec = nsec + vol.spc;
    root->nent += vol.bpc / DE_LEN;
    list_add( &root->clus, clus );
    return 0;
}

/* ------------------------------------------------------------------ */
/* the boot sector label field                                         */
/* ------------------------------------------------------------------ */

/*
 * Keep the label copy in the extended BPB in step with the root
 * directory, as MS-DOS 5 and later do.  Boot sectors without the 29h
 * signature have no label field and are left alone.  On FAT32 the
 * backup boot sector is updated too, but only while it is still a true
 * copy of the primary.
 */
void boot_set_label( const u8 *label )
{
    u8 *boot, *backup, orig[90];
    u32 off, bk;

    if ( !vol.boot_ok ) {
        return;
    }
    boot = (u8 *)xalloc( vol.bps );
    read_secs( 0, 1, boot );
    off = ext_sig_off( boot );
    if ( boot[off] != 0x29 ||
         mem_cmp( boot + off + 5, label, LABEL_LEN ) == 0 ) {
        xfree( boot );
        return;
    }
    mem_cpy( orig, boot, sizeof( orig ) );
    mem_cpy( boot + off + 5, label, LABEL_LEN );
    write_secs( 0, 1, boot, WR_UNKNOWN );

    if ( off == 66 ) {
        bk = RD16( boot + 50 );
        if ( bk != 0 && bk != 0xFFFF && bk < vol.rsvd &&
             bk != vol.fsinfo_sec ) {
            backup = (u8 *)xalloc( vol.bps );
            read_secs( bk, 1, backup );
            if ( mem_cmp( backup, orig, sizeof( orig ) ) == 0 ) {
                mem_cpy( backup + off + 5, label, LABEL_LEN );
                write_secs( bk, 1, backup, WR_UNKNOWN );
            }
            xfree( backup );
        }
    }
    xfree( boot );
}
