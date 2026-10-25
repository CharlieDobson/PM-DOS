/*
 * FLOPPY.C - diskettes: formatting one, and the startup floppy (F7).
 *
 * A DISKETTE IS FORMATTED AT WHAT ITS DRIVE TAKES - 1.44MB in a 3.5"
 * high-density drive (a 2.88MB drive too, so the disk reads anywhere),
 * 1.2MB or 720KB in theirs.  A disk that already has that format is
 * only cleared, as 6.22 did with one it could use.  The tracks are laid
 * down by the platform: 21h/F1h AL=14h on PM-DOS, DOS's own format
 * IOCTLs on MS-DOS (440Dh, 0840h and 0842h), so DOS knows the disk it
 * has afterwards.  Then a boot sector, two FATs and an empty root.
 *
 * THE STARTUP FLOPPY is PM-DOS and the files SETUP.INF marks F, made
 * after 6.22's /F: a settings screen with Date/Time and Install To, a
 * page saying how to label the disk, the copy, the done box.  When the
 * Setup disks are read in the same drive the disk is made in, the files
 * are read into memory first, as much as fits, then written, and so
 * on until all are across.
 */
#include "setup.h"
#include "text.h"

typedef struct {
    u8  type;                           /* FDT_*                         */
    u16 total;
    u8  spt, heads, cyls, spc;
    u16 rootents;
    u8  spf, media;
} FLGEO;

static const FLGEO geos[] = {
    { FDT_1440, 2880, 18, 2, 80, 1, 224, 9, 0xF0 },
    { FDT_2880, 2880, 18, 2, 80, 1, 224, 9, 0xF0 },     /* as a 1.44MB */
    { FDT_1200, 2400, 15, 2, 80, 1, 224, 7, 0xF9 },
    { FDT_720,  1440,  9, 2, 80, 2, 112, 3, 0xF9 }
};

static const FLGEO *geo_of( int drive )
{
    int type = plat_fd_type( drive ), i;

    for ( i = 0; i < (int)(sizeof( geos ) / sizeof( geos[0] )); i++ ) {
        if ( geos[i].type == type ) {
            return &geos[i];
        }
    }
    return NULL;
}

/* sectors of a diskette being made: by drive on PM-DOS (it has no
   letter that works until it is finished), by letter on MS-DOS */
static int fd_write( int drive, u32 lba, u16 count, const void *buf )
{
    if ( plat_caps & PC_32BIT ) {
        return plat_disk_write( drive, lba, count, buf );
    }
    return plat_vol_write( drive, lba, count, buf );
}

static int fd_read( int drive, u32 lba, u16 count, void *buf )
{
    if ( plat_caps & PC_32BIT ) {
        return plat_disk_read( drive, lba, count, buf );
    }
    return plat_vol_read( drive, lba, count, buf );
}

/* 0 = done; 2 = write-protected; 1 = the disk can't be used */
int floppy_format( int drive, const char *label,
                   void (*progress)( u32 done, u32 total ) )
{
    const FLGEO *geo = geo_of( drive );
    u8 *sec, *zeros;
    int track, tracks, rc, i, f, len;
    u32 lba, done = 0, total;
    u16 rootsecs;

    if ( geo == NULL ) {
        return 1;
    }
    sec = (u8 *)xalloc( 512 );
    zeros = (u8 *)xalloc( 18 * 512 );
    mem_set( zeros, 0, 18 * 512 );
    tracks = geo->cyls * geo->heads;
    rootsecs = (u16)(geo->rootents * 32 / 512);
    total = (u32)tracks + geo->spf * 2 + rootsecs;

    /* the format that is there, if it is this one */
    plat_vol_remount( drive );
    rc = fd_read( drive, 0, 1, sec );
    if ( rc == 0 && RD16( sec + 510 ) == 0xAA55 && RD16( sec + 0x0B ) == 512 &&
         RD16( sec + 0x13 ) == geo->total && sec[0x15] == geo->media &&
         RD16( sec + 0x16 ) == geo->spf && RD16( sec + 0x11 ) == geo->rootents ) {
        done = tracks;
    } else {
        rc = plat_fd_begin( drive, geo->spt, geo->cyls, geo->heads, geo->total );
        if ( rc ) {
            xfree( sec );
            xfree( zeros );
            return rc == 3 ? 2 : 1;
        }
        for ( track = 0; track < tracks; track++ ) {
            rc = plat_fd_track( drive, track / geo->heads, track % geo->heads, geo->spt );
            if ( rc ) {
                plat_fd_end( drive, geo->spt, geo->heads, geo->total );
                xfree( sec );
                xfree( zeros );
                return rc == 3 ? 2 : 1;
            }
            if ( progress ) {
                progress( ++done, total );
            }
        }
        plat_fd_end( drive, geo->spt, geo->heads, geo->total );
    }

    /* the boot sector: PM-DOS's (the Uninstall disk puts another over it) */
    mem_cpy( sec, code_floppy, 512 );
    WR16( sec + 0x0B, 512 );
    sec[0x0D] = geo->spc;
    WR16( sec + 0x0E, 1 );
    sec[0x10] = 2;
    WR16( sec + 0x11, geo->rootents );
    WR16( sec + 0x13, geo->total );
    sec[0x15] = geo->media;
    WR16( sec + 0x16, geo->spf );
    WR16( sec + 0x18, geo->spt );
    WR16( sec + 0x1A, geo->heads );
    WR32( sec + 0x1C, 0 );
    WR32( sec + 0x20, 0 );
    sec[0x24] = 0;
    sec[0x26] = 0x29;
    {
        int y, m, d, h, mi;
        plat_get_date( &y, &m, &d );
        plat_get_time( &h, &mi );
        WR32( sec + 0x27, ((u32)(m * 256 + d) << 16) + (u32)(h * 256 + mi) + (u32)y );
    }
    mem_set( sec + 0x2B, ' ', 11 );
    len = str_len( label );
    mem_cpy( sec + 0x2B, label, (u16)(len < 11 ? len : 11) );
    mem_cpy( sec + 0x36, "FAT12   ", 8 );
    WR16( sec + 510, 0xAA55 );
    if ( fd_write( drive, 0, 1, sec ) ) {
        xfree( sec );
        xfree( zeros );
        return 1;
    }
    /* two FATs, empty but for the first entries */
    lba = 1;
    for ( f = 0; f < 2; f++ ) {
        mem_set( sec, 0, 512 );
        sec[0] = geo->media;
        sec[1] = 0xFF;
        sec[2] = 0xFF;
        if ( fd_write( drive, lba, 1, sec ) || fd_write( drive, lba + 1, geo->spf - 1, zeros ) ) {
            xfree( sec );
            xfree( zeros );
            return 1;
        }
        lba += geo->spf;
        done += geo->spf;
        if ( progress ) {
            progress( done, total );
        }
    }
    /* the root */
    for ( i = 0; i < rootsecs; i += 7 ) {
        u16 step = (u16)(rootsecs - i < 7 ? rootsecs - i : 7);
        if ( fd_write( drive, lba + i, step, zeros ) ) {
            xfree( sec );
            xfree( zeros );
            return 1;
        }
    }
    if ( progress ) {
        progress( total, total );
    }
    xfree( sec );
    xfree( zeros );
    plat_vol_remount( drive );
    return 0;
}

/* Is a Setup disk in the drive?  tag NULL = any of them */
int floppy_has_tag( int drive, const char *tag )
{
    char path[20];
    u16 attr;
    int disk;

    for ( disk = 1; disk <= inf.ndisks; disk++ ) {
        if ( tag && str_icmp( tag, inf.disktag[disk] ) ) {
            continue;
        }
        sfmt( path, sizeof( path ), "%c:\\%s", 'A' + drive, inf.disktag[disk] );
        if ( plat_get_attr( path, &attr ) == 0 ) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the startup floppy                                                  */
/* ------------------------------------------------------------------ */

static int target = 0;                  /* the drive the disk is made in */

static void format_box( u32 done, u32 total )
{
    (void)done;
    (void)total;
}

/* the insert box for the disk being made; 0 = it is in and formatted */
static int startup_disk( void )
{
    static const int keys[] = { K_ENTER, 0 };
    char line[60];
    DLG dlg;
    int rc;

    for ( ;; ) {
        sfmt( line, sizeof( line ), tx_ud_insert, tx_fl_label_name, 'A' + target );
        dlg_start( &dlg, "", 42 );
        dlg_blank( &dlg );
        dlg_center( &dlg, line );
        dlg_blank( &dlg );
        dlg_center( &dlg, tx_ud_erase );
        dlg_blank( &dlg );
        dlg_center( &dlg, tx_ud_ready );
        dlg_blank( &dlg );
        dlg_run( &dlg, A_NOTE, tx_keys_ex, keys, 1 );
        plat_vol_remount( target );     /* a different disk now */
        if ( floppy_has_tag( target, NULL ) ) {
            ui_notice( tx_error_title, tx_bad_setupdisk, A_BAR, tx_keys_ex );
            continue;
        }
        ui_working( tx_formatting_fl );
        rc = floppy_format( target, "STARTUP", format_box );
        if ( rc == 2 ) {
            sfmt( line, sizeof( line ), tx_bad_wp, 'A' + target );
            ui_notice( tx_wp_title, line, A_BAR, tx_keys_ex );
            continue;
        }
        if ( rc ) {
            ui_notice( tx_error_title, tx_bad_disk, A_BAR, tx_keys_ex );
            continue;
        }
        return 0;
    }
}

static void copy_page( void )
{
    scr_desk( setup_title() );
    scr_para( 4, tx_copying_fl );
    scr_gauge( 0, 1 );
    scr_status( "", NULL );
}

/* The files, in rounds of as many as memory holds when the Setup disks
   are read in the same drive; one at a time straight across when not */
int floppy_install( void )
{
    HELD *held;
    int *which, nheld, next = 0, i, rc, formatted = 0, same;
    char path[20];
    INFFILE *file;

    label_page( tx_fl_label_1, tx_fl_label_name, NULL, tx_h_flconfirm,
                tx_h_flconfirm_text );
    same = src_removable && src_drive == target;
    held = (HELD *)xalloc( sizeof( HELD ) * MAX_FILES );
    which = (int *)xalloc( sizeof( int ) * MAX_FILES );
    gauge_start( copy_total( 1 ) * 2 );
    copy_page();
    if ( !same ) {
        if ( startup_disk() ) {
            return 1;
        }
        formatted = 1;
        copy_page();
    }
    while ( next < inf.nfiles ) {
        /* read */
        nheld = 0;
        for ( ; next < inf.nfiles; next++ ) {
            file = &inf.file[next];
            if ( !(file->flags & FF_FLOPPY) || file->where == WH_SETUP ) {
                continue;
            }
            if ( disk_wanted( file->disk ) ) {
                copy_error_box( tx_cperr_read, inf.disktag[file->disk] );
                return 1;
            }
            copy_page();
            rc = held_copy( &held[nheld], file );
            if ( rc == 3 && nheld ) {
                break;                  /* memory is full: write these */
            }
            if ( rc ) {
                copy_error_box( tx_cperr_read, file->src );
                continue;
            }
            which[nheld++] = next;
        }
        /* write */
        if ( same || !formatted ) {
            if ( !formatted ) {
                if ( startup_disk() ) {
                    return 1;
                }
                formatted = 1;
            } else {
                /* the startup disk back in, after a Setup disk */
                while ( !floppy_is_startup( target ) ) {
                    if ( startup_again() ) {
                        return 1;
                    }
                }
            }
            disk_forget();
            copy_page();
        }
        for ( i = 0; i < nheld; i++ ) {
            file = &inf.file[which[i]];
            sfmt( path, sizeof( path ), "%c:\\%s", 'A' + target, file->dst );
            if ( held_write( &held[i], path,
                             (file->flags & FF_SYSTEM) ? FA_RDONLY | FA_HIDDEN | FA_SYSTEM : 0 ) ) {
                copy_error_box( tx_cperr_write, path );
            }
            held_free( &held[i] );
        }
    }
    if ( same && !floppy_is_startup( target ) ) {
        while ( !floppy_is_startup( target ) ) {
            if ( startup_again() ) {
                return 1;
            }
        }
    }
    cfg_floppy( target );
    plat_flush();
    gauge_finish();
    xfree( held );
    xfree( which );
    return 0;
}

/* the disk being made is known by the PMIO.SYS on it and no tag */
int floppy_is_startup( int drive )
{
    char path[20];
    u16 attr;

    plat_vol_remount( drive );
    sfmt( path, sizeof( path ), "%c:\\PMIO.SYS", 'A' + drive );
    return plat_get_attr( path, &attr ) == 0 && !floppy_has_tag( drive, NULL );
}

/* put the startup disk back in: 0 = done */
int startup_again( void )
{
    static const int keys[] = { K_ENTER, 0 };
    char line[60];
    DLG dlg;

    sfmt( line, sizeof( line ), tx_ud_insert, tx_fl_label_name, 'A' + target );
    dlg_start( &dlg, "", 42 );
    dlg_blank( &dlg );
    dlg_center( &dlg, line );
    dlg_blank( &dlg );
    dlg_center( &dlg, tx_ud_ready );
    dlg_blank( &dlg );
    dlg_run( &dlg, A_NOTE, tx_keys_ex, keys, 1 );
    plat_vol_remount( target );
    return 0;
}

void floppy_target( int drive )
{
    target = drive;
}

int floppy_drive( void )
{
    return target;
}
