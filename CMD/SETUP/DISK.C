/*
 * DISK.C - the hard disk: what is on it, a partition for PM-DOS, the
 * format, and the boot code.
 *
 * WHICH DISK.  PM-DOS goes on the first hard disk, as MS-DOS did - but
 * a machine started from a USB stick has the stick as its first BIOS
 * disk, so when Setup itself came from BIOS disk 80h the target is 81h.
 * Its letter is whatever PM-DOS gave it now (D: in that case), and C:
 * once the stick is gone; tgt.drive and tgt.final keep the two apart.
 *
 * THE PARTITION is the one 6.22 Setup would make: from the first track
 * boundary of the largest free area to the last whole cylinder but one,
 * written inactive and made active only when everything is installed,
 * so a machine stopped half way does not start from a half-made drive.
 * Its second sector gets a signed record, which is how Setup, started
 * again after the restart, knows the unformatted drive is its own and
 * formats it without asking.  (6.22 did exactly that.)
 *
 * THE FORMAT is Setup's own, not FORMAT.COM's: FORMAT asks its
 * questions on the console, and gives a FAT16 drive one reserved
 * sector where the PM-DOS boot needs nine.  FAT16 below 512MB, with
 * the smallest cluster that keeps the count inside FAT16; FAT32 from
 * 512MB, as Windows 98 does.
 *
 * THE BOOT CODE.  The volume boot record is stage 1 (BOOT\BOOTHD.ASM);
 * all it does is read the 8 sectors of stage 2, which walks the FAT to
 * PMIO.SYS.  Stage 2 lives in reserved sectors 1-8 (9-16 on FAT32) of a
 * drive PM-DOS formatted.  A drive MS-DOS formatted has one reserved
 * sector, so there stage 2 goes into a file instead, PMBOOT.SYS -
 * read-only, hidden and system, which DEFRAG never moves - laid out in
 * one piece, and the VBR is told where: the dword at 1F8h is the first
 * sector of stage 2, counted from the start of the volume.  The old
 * BPB, and the old OEM name with it, stay as they were.
 */
#include "setup.h"
#include "text.h"

TARGET tgt;

#define SECSIZE         512
#define S2_SECTORS      8
#define S2_START16      1
#define S2_START32      9
#define VBR_S2          0x1F8           /* BOOTHD.ASM's s2_where         */
#define MARK            "PM-DOS Setup 1"

/* the boot code, read from the Setup disk when Setup starts */
static u8 *code_mbr, *code_fat16, *code_fat32, *code_s2;
u8 *code_floppy;
static u16 len_s2;

static u8 sec[SECSIZE];

static int is_fat( u8 type )
{
    return type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B ||
           type == 0x0C || type == 0x0E;
}

static int is_ext( u8 type )
{
    return type == 0x05 || type == 0x0F || type == 0x85;
}

/* ------------------------------------------------------------------ */
/* the boot code from the Setup disk                                   */
/* ------------------------------------------------------------------ */

static u8 *load_code( const char *name, u16 want, u16 *got )
{
    INFFILE *file = inf_file( name );
    char path[PATHLEN];
    u16 handle, done;
    u8 *data;

    if ( file == NULL ) {
        return NULL;
    }
    path_join( path, srcdir, file->src, sizeof( path ) );
    if ( plat_open( path, 0, &handle ) ) {
        return NULL;
    }
    data = (u8 *)xalloc( want );
    mem_set( data, 0, want );
    if ( plat_read( handle, data, want, &done ) || done == 0 ) {
        plat_close( handle );
        xfree( data );
        return NULL;
    }
    plat_close( handle );
    if ( got ) {
        *got = done;
    }
    return data;
}

/* 0 = all of it was found */
int disk_load_code( void )
{
    code_mbr = load_code( "MBRBOOT.BIN", SECSIZE, NULL );
    code_fat16 = load_code( "BOOT16.BIN", SECSIZE, NULL );
    code_fat32 = load_code( "BOOT32.BIN", SECSIZE, NULL );
    code_floppy = load_code( "BOOT.BIN", SECSIZE, NULL );
    code_s2 = load_code( "BOOT2.BIN", S2_SECTORS * SECSIZE, &len_s2 );
    return !(code_mbr && code_fat16 && code_fat32 && code_floppy && code_s2);
}

/* ------------------------------------------------------------------ */
/* the BPB                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    u16 bps, rsvd, rootents, spt, heads;
    u8  spc, nfats, media;
    u32 total, spf, hidden, rootclus;
    u32 fatlba, rootlba, datalba, clusters;
    int fat;
} BPB;

/* 0 = a FAT boot sector this code can use */
static int bpb_read( const u8 *boot, BPB *bpb )
{
    u32 rootsecs;

    if ( RD16( boot + 510 ) != 0xAA55 ) {
        return 1;
    }
    bpb->bps = RD16( boot + 0x0B );
    bpb->spc = boot[0x0D];
    bpb->rsvd = RD16( boot + 0x0E );
    bpb->nfats = boot[0x10];
    bpb->rootents = RD16( boot + 0x11 );
    bpb->total = RD16( boot + 0x13 );
    if ( bpb->total == 0 ) {
        bpb->total = RD32( boot + 0x20 );
    }
    bpb->media = boot[0x15];
    bpb->spf = RD16( boot + 0x16 );
    bpb->spt = RD16( boot + 0x18 );
    bpb->heads = RD16( boot + 0x1A );
    bpb->hidden = RD32( boot + 0x1C );
    bpb->rootclus = 0;
    if ( bpb->spf == 0 ) {
        bpb->spf = RD32( boot + 0x24 );
        bpb->rootclus = RD32( boot + 0x2C );
    }
    if ( bpb->bps != SECSIZE || bpb->spc == 0 || (bpb->spc & (bpb->spc - 1)) ||
         bpb->rsvd == 0 || bpb->nfats == 0 || bpb->nfats > 2 ||
         bpb->media < 0xF0 || bpb->spf == 0 || bpb->total == 0 ) {
        return 1;
    }
    rootsecs = ((u32)bpb->rootents * 32 + SECSIZE - 1) / SECSIZE;
    bpb->fatlba = bpb->rsvd;
    bpb->rootlba = bpb->rsvd + (u32)bpb->nfats * bpb->spf;
    bpb->datalba = bpb->rootlba + rootsecs;
    if ( bpb->datalba >= bpb->total ) {
        return 1;
    }
    bpb->clusters = (bpb->total - bpb->datalba) / bpb->spc;
    bpb->fat = bpb->clusters < 4085 ? 12 : bpb->clusters < 65525 ? 16 : 32;
    if ( bpb->fat == 32 && bpb->rootents ) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* what is on the disk                                                 */
/* ------------------------------------------------------------------ */

static int root_has( const char *name )
{
    char path[PATHLEN];
    u16 attr;

    path[0] = (char)('A' + tgt.drive);
    path[1] = ':';
    path[2] = '\\';
    str_cpyn( path + 3, name, sizeof( path ) - 3 );
    return plat_get_attr( path, &attr ) == 0;
}

/* MSDOS.SYS of Windows 95 and 98 is a page of settings, not a program */
static int msdos_sys_is_text( void )
{
    char path[16];
    u8 head[16];
    u16 handle, done;
    int i, text = 1;

    sfmt( path, sizeof( path ), "%c:\\MSDOS.SYS", 'A' + tgt.drive );
    if ( plat_open( path, 0, &handle ) ) {
        return 0;
    }
    if ( plat_read( handle, head, sizeof( head ), &done ) || done == 0 ) {
        text = 0;
    }
    plat_close( handle );
    for ( i = 0; i < (int)done; i++ ) {
        if ( head[i] < 9 || head[i] > 0x7E ) {
            text = 0;
        }
    }
    return text;
}

/* What is installed on the volume, now that it has a letter and a file
   system */
static void examine_volume( void )
{
    int major, minor;
    u32 cluster;

    if ( plat_disk_free( tgt.drive, &tgt.free_kb, &cluster ) ) {
        tgt.state = TG_RAW;
        return;
    }
    tgt.state = TG_BLANK;
    if ( root_has( "DBLSPACE.000" ) || root_has( "DRVSPACE.000" ) ) {
        tgt.state = TG_PACKED;
    } else if ( root_has( "PMIO.SYS" ) && root_has( "PMDOS.SYS" ) ) {
        tgt.state = TG_PMDOS;
        str_cpyn( tgt.sysname, "PM-DOS", sizeof( tgt.sysname ) );
    } else if ( root_has( "IO.SYS" ) && root_has( "MSDOS.SYS" ) ) {
        if ( msdos_sys_is_text() ) {
            tgt.state = TG_WIN9X;
        } else {
            tgt.state = TG_MSDOS;
            str_cpyn( tgt.sysfile[0], "IO.SYS", 13 );
            str_cpyn( tgt.sysfile[1], "MSDOS.SYS", 13 );
            str_cpyn( tgt.sysname, "MS-DOS", sizeof( tgt.sysname ) );
            /* the 16-bit half was started by that DOS: it knows which */
            if ( !(plat_caps & PC_32BIT) && plat_boot_drive() == tgt.drive ) {
                plat_dosver( &major, &minor );
                sfmt( tgt.sysname, sizeof( tgt.sysname ), "MS-DOS %d.%d",
                      major, minor );
                if ( minor % 10 == 0 && minor ) {
                    sfmt( tgt.sysname, sizeof( tgt.sysname ), "MS-DOS %d.%d",
                          major, minor / 10 );
                }
            }
        }
    } else if ( root_has( "IBMBIO.COM" ) && root_has( "IBMDOS.COM" ) ) {
        tgt.state = TG_MSDOS;
        str_cpyn( tgt.sysfile[0], "IBMBIO.COM", 13 );
        str_cpyn( tgt.sysfile[1], "IBMDOS.COM", 13 );
        str_cpyn( tgt.sysname, "PC DOS", sizeof( tgt.sysname ) );
    }
}

/* The largest free area of the disk outside every primary entry (an
   extended partition counts as used).  The first area starts on track
   1, as the MBR has track 0; the others on a cylinder.  Every area
   ends on a cylinder, and the disk's last cylinder is left unused, as
   6.22 left it. */
static void find_gap( const u8 *mbr, int table )
{
    u32 start[5], end[5], lo, hi, best = 0, best_lba = 0, cyl, disk_end, tmp;
    int n = 0, i, j, slot = -1;

    for ( i = 0; i < 4; i++ ) {
        const u8 *ent = mbr + 0x1BE + i * 16;
        if ( !table || ent[4] == 0 ) {
            if ( slot < 0 ) {
                slot = i;
            }
            continue;
        }
        start[n] = RD32( ent + 8 );
        end[n] = start[n] + RD32( ent + 12 );
        n++;
    }
    /* in order of where they start */
    for ( i = 0; i < n; i++ ) {
        for ( j = i + 1; j < n; j++ ) {
            if ( start[j] < start[i] ) {
                tmp = start[i]; start[i] = start[j]; start[j] = tmp;
                tmp = end[i]; end[i] = end[j]; end[j] = tmp;
            }
        }
    }
    cyl = (u32)tgt.spt * tgt.heads;
    disk_end = tgt.disk_secs / cyl;
    disk_end = (disk_end > 1 ? disk_end - 1 : disk_end) * cyl;
    start[n] = disk_end;                /* the end of the disk closes the */
    end[n] = disk_end;                  /* last area                      */
    lo = 0;
    for ( i = 0; i <= n; i++ ) {
        hi = start[i];
        if ( lo == 0 ) {
            lo = tgt.spt;
        } else if ( lo % cyl ) {
            lo += cyl - lo % cyl;
        }
        hi -= hi % cyl;
        if ( hi > lo && hi - lo > best ) {
            best = hi - lo;
            best_lba = lo;
        }
        if ( end[i] > lo ) {
            lo = end[i];
        }
    }
    tgt.gap_lba = best_lba;
    tgt.gap_secs = best;
    tgt.slot = slot;
}

/* the drive letter PM-DOS gave the partition starting at that sector */
static int letter_of( int bios, u32 start )
{
    DRIVEINFO di;
    int drive;

    for ( drive = 2; drive < 26; drive++ ) {
        if ( plat_drive( drive, &di ) == 0 && (di.flags & DI_PRESENT) &&
             di.bios == bios && di.start == start ) {
            return drive;
        }
    }
    return -1;
}

void disk_examine( void )
{
    DRIVEINFO di;
    DISKINFO dk;
    BPB bpb;
    int i, table;
    const u8 *ent;

    mem_set( &tgt, 0, sizeof( tgt ) );
    tgt.final = 'C';
    tgt.drive = 2;
    tgt.slot = -1;

    if ( !(plat_caps & PC_PARTITION) ) {
        /* the 16-bit half: drive C:, as DOS sees it */
        if ( plat_drive( 2, &di ) || !(di.flags & DI_PRESENT) ) {
            tgt.state = TG_NODISK;
            return;
        }
        if ( !(di.flags & DI_FIXED) ) {
            tgt.state = TG_FOREIGN;
            return;
        }
        if ( plat_vol_read( 2, 0, 1, sec ) || bpb_read( sec, &bpb ) ) {
            tgt.state = TG_RAW;
            return;
        }
        tgt.reserved = bpb.rsvd;
        tgt.fat = bpb.fat;
        tgt.part_lba = bpb.hidden;
        tgt.part_secs = bpb.total;
        examine_volume();
        return;
    }

    /* the 32-bit half: the disk, its table, then the volume */
    /* The Setup files in the root of BIOS disk 80h are a USB stick or a
       Zip disk the machine started from: the hard disk is the next one */
    tgt.bios = 0x80;
    if ( plat_drive( src_drive, &di ) == 0 && di.bios == 0x80 &&
         str_len( srcdir ) == 3 ) {
        tgt.bios = 0x81;
    }
    if ( plat_disk( tgt.bios, &dk ) || !dk.present || dk.total == 0 ) {
        tgt.state = TG_NODISK;
        return;
    }
    tgt.disk_secs = dk.total;
    tgt.spt = dk.spt ? dk.spt : 63;
    tgt.heads = dk.heads ? dk.heads : 16;
    if ( plat_disk_read( tgt.bios, 0, 1, sec ) ) {
        tgt.state = TG_NODISK;
        return;
    }
    table = RD16( sec + 510 ) == 0xAA55;
    for ( i = 0; i < 4 && table; i++ ) {
        ent = sec + 0x1BE + i * 16;
        if ( is_fat( ent[4] ) ) {
            tgt.part_lba = RD32( ent + 8 );
            tgt.part_secs = RD32( ent + 12 );
            tgt.slot = i;
            break;
        }
    }
    if ( i == 4 || !table ) {
        /* no DOS partition: is there room for one? */
        int used = 0;
        for ( i = 0; i < 4 && table; i++ ) {
            if ( sec[0x1BE + i * 16 + 4] ) {
                used++;
            }
        }
        find_gap( sec, table && used );
        tgt.drive = -1;
        if ( !table || used == 0 ) {
            tgt.state = TG_EMPTY;
        } else if ( tgt.slot >= 0 && tgt.gap_secs >= 16384ul ) {
            tgt.state = TG_FREE;
        } else {
            tgt.state = TG_FULL;
        }
        if ( tgt.state != TG_FULL && tgt.gap_secs < 16384ul ) {
            tgt.state = TG_FULL;        /* not even 8MB */
        }
        return;
    }
    (void)is_ext;
    tgt.drive = letter_of( tgt.bios, tgt.part_lba );
    tgt.final = 'C';
    if ( tgt.drive < 0 ) {
        tgt.state = TG_FOREIGN;
        tgt.drive = 2;
        return;
    }
    if ( plat_disk_read( tgt.bios, tgt.part_lba, 1, sec ) || bpb_read( sec, &bpb ) ) {
        tgt.state = TG_RAW;
        return;
    }
    tgt.reserved = bpb.rsvd;
    tgt.fat = bpb.fat;
    examine_volume();
}

/* nonzero when the unformatted partition is the one Setup made */
int disk_marked( void )
{
    if ( !(plat_caps & PC_PARTITION) || tgt.part_secs < 2 ||
         plat_disk_read( tgt.bios, tgt.part_lba + 1, 1, sec ) ) {
        return 0;
    }
    return mem_cmp( sec, MARK, sizeof( MARK ) ) == 0;
}

/* ------------------------------------------------------------------ */
/* the partition                                                       */
/* ------------------------------------------------------------------ */

static void put_chs( u8 *at, u32 lba )
{
    u32 cyl = lba / ((u32)tgt.spt * tgt.heads);
    u32 rest = lba % ((u32)tgt.spt * tgt.heads);
    u32 head = rest / tgt.spt, secno = rest % tgt.spt + 1;

    if ( cyl > 1023 ) {                 /* past what CHS can say */
        cyl = 1023;
        head = tgt.heads - 1;
        secno = tgt.spt;
    }
    at[0] = (u8)head;
    at[1] = (u8)(secno | ((cyl >> 2) & 0xC0));
    at[2] = (u8)cyl;
}

static u8 part_type( u32 lba, u32 secs )
{
    u32 chs_end = 1024ul * tgt.heads * tgt.spt;
    int beyond = lba + secs > chs_end;

    if ( secs >= 1048576ul ) {          /* 512MB: FAT32 */
        return (u8)(beyond ? 0x0C : 0x0B);
    }
    if ( beyond ) {
        return 0x0E;
    }
    return (u8)(secs < 65536ul ? 0x04 : 0x06);
}

/* Write the partition into the gap disk_examine found.  0 = done */
int disk_partition( void )
{
    u8 *ent;
    u32 lba = tgt.gap_lba, secs = tgt.gap_secs;

    if ( tgt.slot < 0 || secs == 0 ) {
        return 1;
    }
    if ( plat_disk_read( tgt.bios, 0, 1, sec ) ) {
        return 1;
    }
    if ( RD16( sec + 510 ) != 0xAA55 ) {
        /* no table at all: PM-DOS's boot code and an empty one */
        mem_set( sec, 0, SECSIZE );
        mem_cpy( sec, code_mbr, 0x1B8 );
        WR16( sec + 510, 0xAA55 );
    }
    ent = sec + 0x1BE + tgt.slot * 16;
    mem_set( ent, 0, 16 );
    ent[0] = 0;                         /* active at the very end */
    put_chs( ent + 1, lba );
    ent[4] = part_type( lba, secs );
    put_chs( ent + 5, lba + secs - 1 );
    WR32( ent + 8, lba );
    WR32( ent + 12, secs );
    if ( plat_disk_write( tgt.bios, 0, 1, sec ) ) {
        return 1;
    }
    /* no old boot sector to be mistaken for a format, and Setup's mark */
    mem_set( sec, 0, SECSIZE );
    if ( plat_disk_write( tgt.bios, lba, 1, sec ) ) {
        return 1;
    }
    mem_cpy( sec, MARK, sizeof( MARK ) );
    if ( plat_disk_write( tgt.bios, lba + 1, 1, sec ) ) {
        return 1;
    }
    tgt.part_lba = lba;
    tgt.part_secs = secs;
    return 0;
}

/* The partition becomes the one the machine starts from, if Setup made
   it or none is marked.  0 = done or nothing to do */
int disk_activate( int made )
{
    int i, active = -1, ours = -1;
    u8 *ent;

    if ( !(plat_caps & PC_PARTITION) ) {
        return 0;
    }
    if ( plat_disk_read( tgt.bios, 0, 1, sec ) || RD16( sec + 510 ) != 0xAA55 ) {
        return 1;
    }
    for ( i = 0; i < 4; i++ ) {
        ent = sec + 0x1BE + i * 16;
        if ( ent[0] & 0x80 ) {
            active = i;
        }
        if ( RD32( ent + 8 ) == tgt.part_lba && ent[4] ) {
            ours = i;
        }
    }
    if ( ours < 0 || active == ours || (active >= 0 && !made) ) {
        return 0;
    }
    for ( i = 0; i < 4; i++ ) {
        sec[0x1BE + i * 16] = (u8)(i == ours ? 0x80 : 0);
    }
    return plat_disk_write( tgt.bios, 0, 1, sec );
}

/* ------------------------------------------------------------------ */
/* the format                                                          */
/* ------------------------------------------------------------------ */

static void bpb_write( u8 *boot, const BPB *bpb, int fat32, u32 serial )
{
    static const char oem[] = "MSDOS5.0";
    char label[12];
    int i, at = fat32 ? 0x40 : 0x24;

    mem_cpy( boot + 3, oem, 8 );
    WR16( boot + 0x0B, SECSIZE );
    boot[0x0D] = bpb->spc;
    WR16( boot + 0x0E, bpb->rsvd );
    boot[0x10] = 2;
    WR16( boot + 0x11, bpb->rootents );
    WR16( boot + 0x13, bpb->total < 65536ul ? (u16)bpb->total : 0 );
    boot[0x15] = 0xF8;
    WR16( boot + 0x16, fat32 ? 0 : (u16)bpb->spf );
    WR16( boot + 0x18, bpb->spt );
    WR16( boot + 0x1A, bpb->heads );
    WR32( boot + 0x1C, bpb->hidden );
    WR32( boot + 0x20, bpb->total < 65536ul ? 0 : bpb->total );
    if ( fat32 ) {
        WR32( boot + 0x24, bpb->spf );
        WR16( boot + 0x28, 0 );
        WR16( boot + 0x2A, 0 );
        WR32( boot + 0x2C, 2 );         /* the root starts at cluster 2 */
        WR16( boot + 0x30, 1 );         /* FSInfo */
        WR16( boot + 0x32, 6 );         /* the backup boot sector */
        mem_set( boot + 0x34, 0, 12 );
    }
    boot[at] = 0x80;
    boot[at + 1] = 0;
    boot[at + 2] = 0x29;
    WR32( boot + at + 3, serial );
    mem_set( label, ' ', 11 );
    for ( i = 0; i < 11 && inf.label[i]; i++ ) {
        label[i] = (char)ch_upper( (u8)inf.label[i] );
    }
    mem_cpy( boot + at + 7, label, 11 );
    mem_cpy( boot + at + 18, fat32 ? "FAT32   " : "FAT16   ", 8 );
}

static u32 serial_now( void )
{
    int y, m, d, h, mi;

    plat_get_date( &y, &m, &d );
    plat_get_time( &h, &mi );
    return ((u32)(m * 256 + d) << 16) + (u32)y + (u32)(h * 256 + mi) * 31ul;
}

/* the layout of a new volume of "total" sectors */
static int layout( BPB *bpb, u32 total )
{
    u32 rootsecs, nclus, spf = 1;
    int n, fat32 = total >= 1048576ul;

    mem_set( bpb, 0, sizeof( *bpb ) );
    bpb->total = total;
    bpb->spt = tgt.spt;
    bpb->heads = tgt.heads;
    bpb->hidden = tgt.part_lba;
    bpb->nfats = 2;
    if ( fat32 ) {
        bpb->rsvd = 32;
        bpb->rootents = 0;
        bpb->spc = total <= 16777216ul ? 8 : total <= 33554432ul ? 16
                 : total <= 67108864ul ? 32 : 64;
        for ( n = 0; n < 8; n++ ) {
            nclus = (total - bpb->rsvd - 2 * spf) / bpb->spc;
            spf = ((nclus + 2) * 4 + SECSIZE - 1) / SECSIZE;
        }
        nclus = (total - bpb->rsvd - 2 * spf) / bpb->spc;
        if ( nclus < 65525ul ) {
            return 1;
        }
        bpb->fat = 32;
    } else {
        bpb->rsvd = 1 + S2_SECTORS;
        bpb->rootents = 512;
        rootsecs = 32;
        for ( bpb->spc = 2; ; bpb->spc *= 2 ) {
            spf = 1;
            for ( n = 0; n < 8; n++ ) {
                nclus = (total - bpb->rsvd - 2 * spf - rootsecs) / bpb->spc;
                spf = ((nclus + 2) * 2 + SECSIZE - 1) / SECSIZE;
            }
            nclus = (total - bpb->rsvd - 2 * spf - rootsecs) / bpb->spc;
            if ( nclus <= 65524ul || bpb->spc >= 64 ) {
                break;
            }
        }
        if ( nclus < 4085 || nclus > 65524ul ) {
            return 1;
        }
        bpb->fat = 16;
    }
    bpb->spf = spf;
    bpb->fatlba = bpb->rsvd;
    bpb->rootlba = bpb->rsvd + 2 * spf;
    bpb->datalba = bpb->rootlba + (fat32 ? 0 : 32);
    bpb->clusters = nclus;
    return 0;
}

static int write_secs( u32 lba, u32 count, const u8 *data, u8 *zeros,
                       void (*progress)( u32, u32 ), u32 *done, u32 total )
{
    u16 step;

    while ( count ) {
        step = count > 32 ? 32 : (u16)count;
        if ( plat_disk_write( tgt.bios, tgt.part_lba + lba, step,
                              data ? data : zeros ) ) {
            return 1;
        }
        lba += step;
        count -= step;
        if ( data ) {
            data += (u32)step * SECSIZE;
        }
        *done += step;
        if ( progress ) {
            progress( *done, total );
        }
    }
    return 0;
}

/* Lay a new FAT file system over the partition, the boot code with it.
   0 = done */
int disk_format( void (*progress)( u32 done, u32 total ) )
{
    BPB bpb;
    u8 *zeros, *boot;
    u32 total, done = 0, serial = serial_now();
    int fat32, f;
    u16 date, time;
    int y, m, d, h, mi;

    if ( layout( &bpb, tgt.part_secs ) ) {
        return 1;
    }
    fat32 = bpb.fat == 32;
    zeros = (u8 *)xalloc( 32ul * SECSIZE );
    mem_set( zeros, 0, (u16)(32 * SECSIZE) );
    boot = (u8 *)xalloc( SECSIZE );
    total = 2 * bpb.spf + (fat32 ? bpb.spc : 32) + bpb.rsvd;

    /* the reserved area first: zeros, then what belongs there */
    if ( write_secs( 0, bpb.rsvd, NULL, zeros, progress, &done, total ) ) {
        return 1;
    }
    mem_cpy( boot, fat32 ? code_fat32 : code_fat16, SECSIZE );
    bpb_write( boot, &bpb, fat32, serial );
    WR32( boot + VBR_S2, fat32 ? S2_START32 : S2_START16 );
    if ( plat_disk_write( tgt.bios, tgt.part_lba, 1, boot ) ) {
        return 1;
    }
    if ( fat32 ) {
        if ( plat_disk_write( tgt.bios, tgt.part_lba + 6, 1, boot ) ) {
            return 1;
        }
        mem_set( sec, 0, SECSIZE );
        WR32( sec, 0x41615252ul );
        WR32( sec + 484, 0x61417272ul );
        WR32( sec + 488, bpb.clusters - 1 );    /* the root has one */
        WR32( sec + 492, 3 );
        WR16( sec + 510, 0xAA55 );
        if ( plat_disk_write( tgt.bios, tgt.part_lba + 1, 1, sec ) ||
             plat_disk_write( tgt.bios, tgt.part_lba + 7, 1, sec ) ) {
            return 1;
        }
    }
    if ( plat_disk_write( tgt.bios, tgt.part_lba + (fat32 ? S2_START32 : S2_START16),
                          S2_SECTORS, code_s2 ) ) {
        return 1;
    }

    /* the FATs: the media byte and the end marks, then zeros */
    for ( f = 0; f < 2; f++ ) {
        mem_set( sec, 0, SECSIZE );
        if ( fat32 ) {
            WR32( sec, 0x0FFFFFF8ul );
            WR32( sec + 4, 0x0FFFFFFFul );
            WR32( sec + 8, 0x0FFFFFFFul );      /* cluster 2, the root */
        } else {
            WR16( sec, 0xFFF8 );
            WR16( sec + 2, 0xFFFF );
        }
        if ( plat_disk_write( tgt.bios, tgt.part_lba + bpb.fatlba + f * bpb.spf, 1, sec ) ) {
            return 1;
        }
        done++;
        if ( write_secs( bpb.fatlba + f * bpb.spf + 1, bpb.spf - 1, NULL, zeros,
                         progress, &done, total ) ) {
            return 1;
        }
    }

    /* the root: empty but for the volume label */
    if ( write_secs( bpb.rootlba, fat32 ? bpb.spc : 32, NULL, zeros,
                     progress, &done, total ) ) {
        return 1;
    }
    mem_set( sec, 0, SECSIZE );
    mem_set( sec, ' ', 11 );
    for ( f = 0; f < 11 && inf.label[f]; f++ ) {
        sec[f] = (u8)ch_upper( (u8)inf.label[f] );
    }
    sec[11] = FA_LABEL;
    plat_get_date( &y, &m, &d );
    plat_get_time( &h, &mi );
    date = (u16)(((y - 1980) << 9) | (m << 5) | d);
    time = (u16)((h << 11) | (mi << 5));
    WR16( sec + 22, time );
    WR16( sec + 24, date );
    if ( plat_disk_write( tgt.bios, tgt.part_lba + bpb.rootlba, 1, sec ) ) {
        return 1;
    }
    xfree( zeros );
    xfree( boot );
    if ( progress ) {
        progress( total, total );
    }
    plat_vol_remount( tgt.drive );
    tgt.reserved = bpb.rsvd;
    tgt.fat = bpb.fat;
    tgt.state = TG_BLANK;
    plat_disk_free( tgt.drive, &tgt.free_kb, &done );
    return 0;
}

/* ------------------------------------------------------------------ */
/* the boot code on an existing volume                                 */
/* ------------------------------------------------------------------ */

/* the FAT entry of a cluster, read through the volume */
static u32 fat_entry( const BPB *bpb, u32 clus, u8 *buf )
{
    u32 off, lba;

    off = bpb->fat == 12 ? clus + clus / 2 : clus * (bpb->fat == 16 ? 2 : 4);
    lba = bpb->fatlba + off / SECSIZE;
    off %= SECSIZE;
    /* two sectors: a FAT12 entry can straddle them */
    if ( plat_vol_read( tgt.drive, lba, 2, buf ) ) {
        return 0;
    }
    if ( bpb->fat == 12 ) {
        u16 val = RD16( buf + off );
        return (clus & 1) ? val >> 4 : val & 0xFFF;
    }
    if ( bpb->fat == 16 ) {
        return RD16( buf + off );
    }
    return RD32( buf + off ) & 0x0FFFFFFFul;
}

/* the first cluster of a root entry, found by reading the root */
static u32 root_cluster( const BPB *bpb, const char *name83, u8 *buf )
{
    u32 lba, last = bpb->datalba;
    int i;

    for ( lba = bpb->rootlba; lba < last; lba++ ) {
        if ( plat_vol_read( tgt.drive, lba, 1, buf ) ) {
            return 0;
        }
        for ( i = 0; i < SECSIZE; i += 32 ) {
            if ( buf[i] == 0 ) {
                return 0;
            }
            if ( mem_cmp( buf + i, name83, 11 ) == 0 && !(buf[i + 11] & FA_LABEL) ) {
                return RD16( buf + i + 26 ) | ((u32)RD16( buf + i + 20 ) << 16);
            }
        }
    }
    return 0;
}

/* Stage 2 as the file PMBOOT.SYS, all in one piece.  -> the sector it
   starts at, counted from the volume's start, or 0 */
static u32 stage2_file( const BPB *bpb )
{
    char path[16], temp[16];
    u8 *buf = (u8 *)xalloc( 2 * SECSIZE );
    u16 handle, done;
    u32 first, clus, next, need, lba = 0;
    int tries, n, temps = 0;

    sfmt( path, sizeof( path ), "%c:\\PMBOOT.SYS", 'A' + tgt.drive );
    need = (S2_SECTORS + bpb->spc - 1) / bpb->spc;          /* clusters */
    for ( tries = 0; tries < 8 && lba == 0; tries++ ) {
        plat_set_attr( path, 0 );
        plat_delete( path );
        if ( plat_create( path, &handle ) ) {
            break;
        }
        if ( plat_write( handle, code_s2, S2_SECTORS * SECSIZE, &done ) ||
             done != S2_SECTORS * SECSIZE ) {
            plat_close( handle );
            break;
        }
        plat_close( handle );
        plat_flush();
        plat_vol_remount( tgt.drive );
        first = root_cluster( bpb, "PMBOOT  SYS", buf );
        if ( first < 2 ) {
            break;
        }
        /* in one piece? */
        clus = first;
        for ( n = 1; n < (int)need; n++ ) {
            next = fat_entry( bpb, clus, buf );
            if ( next != clus + 1 ) {
                break;
            }
            clus = next;
        }
        if ( n == (int)need ) {
            lba = bpb->datalba + (first - 2) * bpb->spc;
            break;
        }
        /* not: keep those clusters taken and try again further on */
        sfmt( temp, sizeof( temp ), "%c:\\PMBOOT.%03d", 'A' + tgt.drive, temps++ );
        plat_rename( path, temp );
    }
    while ( temps > 0 ) {
        sfmt( temp, sizeof( temp ), "%c:\\PMBOOT.%03d", 'A' + tgt.drive, --temps );
        plat_delete( temp );
    }
    if ( lba ) {
        plat_set_attr( path, FA_RDONLY | FA_HIDDEN | FA_SYSTEM );
    } else {
        plat_delete( path );
    }
    xfree( buf );
    return lba;
}

static int all_zero( const u8 *data, u16 len )
{
    while ( len-- ) {
        if ( *data++ ) {
            return 0;
        }
    }
    return 1;
}

/* Make the volume start PM-DOS: stage 2 where there is room for it,
   then stage 1 with the volume's own BPB.  0 = done */
int disk_bootable( void )
{
    BPB bpb;
    u8 *old = (u8 *)xalloc( SECSIZE ), *s2 = (u8 *)xalloc( S2_SECTORS * SECSIZE );
    u32 where = 0;
    int fat32, rc = 1, span;

    plat_flush();
    if ( plat_vol_read( tgt.drive, 0, 1, old ) || bpb_read( old, &bpb ) ) {
        goto out;
    }
    fat32 = bpb.fat == 32;
    span = fat32 ? 0x5A : 0x3E;
    /* the reserved sectors stage 2 would use, if they are there and free
       (or hold stage 2 already) */
    if ( bpb.rsvd >= (fat32 ? S2_START32 : S2_START16) + S2_SECTORS ) {
        where = fat32 ? S2_START32 : S2_START16;
        if ( plat_vol_read( tgt.drive, where, S2_SECTORS, s2 ) ) {
            goto out;
        }
        if ( !all_zero( s2, S2_SECTORS * SECSIZE ) &&
             mem_cmp( s2, code_s2, len_s2 ) ) {
            where = 0;                  /* somebody else's: leave them */
        }
        if ( where && plat_vol_write( tgt.drive, where, S2_SECTORS, code_s2 ) ) {
            goto out;
        }
    }
    if ( where == 0 ) {
        if ( fat32 ) {
            goto out;                   /* no FAT32 without the room */
        }
        where = stage2_file( &bpb );
        if ( where == 0 ) {
            goto out;
        }
    }
    /* stage 1: our code round the volume's BPB and OEM name */
    mem_cpy( sec, fat32 ? code_fat32 : code_fat16, SECSIZE );
    mem_cpy( sec + 3, old + 3, (u16)(span - 3) );
    sec[fat32 ? 0x40 : 0x24] = 0x80;
    WR32( sec + VBR_S2, where );
    if ( plat_vol_write( tgt.drive, 0, 1, sec ) ) {
        goto out;
    }
    if ( fat32 && RD16( old + 0x32 ) && RD16( old + 0x32 ) < bpb.rsvd ) {
        plat_vol_write( tgt.drive, RD16( old + 0x32 ), 1, sec );
    }
    plat_flush();
    rc = 0;
out:
    xfree( old );
    xfree( s2 );
    return rc;
}

/* room for "need" bytes on the target, counted in its clusters */
int disk_space_ok( u32 need )
{
    u32 cluster, free_kb;

    if ( plat_disk_free( tgt.drive, &free_kb, &cluster ) ) {
        return 0;
    }
    tgt.free_kb = free_kb;
    return free_kb >= need / 1024 + 1;
}
