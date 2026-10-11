/*
 * UNDO.C - the Undo file: what every repair wrote over, kept on another
 * drive, and SCANDISK /UNDO, which puts it back - on ScanDisk's screen,
 * with a box that says what is about to be undone and asks first.
 *
 * SCANDISK.UND, in the root of the Undo drive:
 *
 *   header   'PMSDUNDO', version, length (64), DOS date and time
 *   'UV'     a volume: letter, FAT type, bytes a sector, serial number,
 *            sectors in all, reserved sectors, sectors a FAT, FATs,
 *            label - once, before the first sectors of that volume
 *   'US'     sectors: letter, count, first sector; then a flag and a
 *            CRC for each sector, then the sectors AS THEY WERE
 *
 * THE RECORD IS ON THE UNDO DISK BEFORE THE SECTOR IS WRITTEN.  Each one
 * is committed as it is made, so a machine switched off in the middle
 * of a repair has an Undo file that describes everything that reached
 * the disk - and at most one record more, for a write that never
 * happened, which puts back what is there already.  For the same
 * reason a record that cannot be written stops the repairs: an Undo
 * file with a change missing would put back half a repair.
 *
 * THE CRC IS OF THE SECTOR AS IT WAS WRITTEN, and it is how /UNDO knows
 * the drive has not been used since: every sector must still hold what
 * ScanDisk left in it - the LAST thing ScanDisk left in it, when a
 * sector was written twice.  If one does not, something else has
 * written to the drive and putting old sectors back over it would do
 * more harm than the repair did; nothing is changed.  One thing may
 * differ: the two flag bits in entry 1 of each FAT, which the kernel
 * turns off and on by itself as the volume is used.
 *
 * Sectors written into clusters that were free are not kept (WR_NOUNDO):
 * with the FAT and the directories as they were, those clusters are
 * free again and what is in them does not matter.
 */
#include "scandisk.h"

#define UND_NAME        "SCANDISK.UND"
#define UND_HEAD        64
#define UV_LEN          38
#define US_HEAD         10
#define UF_UNREAD       0x01    /* the sector could not be read before */

static u32  und_handle;
static int  und_open;
static int  und_drive = -1;
static int  und_asked;          /* the question is asked once a run */
static int  und_vol_noted;      /* this volume's 'UV' is in the file */

/* ------------------------------------------------------------------ */
/* CRC-32                                                              */
/* ------------------------------------------------------------------ */

static u32 crc_table[256];

static u32 crc32( const u8 *data, u32 len )
{
    u32 crc = 0xFFFFFFFFul, index, bit, val;

    if ( crc_table[1] == 0 ) {
        for ( index = 0; index < 256; index++ ) {
            val = index;
            for ( bit = 0; bit < 8; bit++ ) {
                val = (val & 1) ? (val >> 1) ^ 0xEDB88320ul : val >> 1;
            }
            crc_table[index] = val;
        }
    }
    while ( len-- ) {
        crc = crc_table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/* a sector's CRC with the kernel's two flag bits taken out of it */
static u32 sector_crc( u32 lba, const u8 *data, int fattype, u32 rsvd, u32 fatsz, u32 nfats,
                       u32 bps )
{
    u8 first[8];
    u32 crc;

    if ( fattype != 12 && lba >= rsvd && lba < rsvd + nfats * fatsz &&
         (lba - rsvd) % fatsz == 0 && bps >= 8 ) {
        mem_cpy( first, data, 8 );
        if ( fattype == 16 ) {
            first[3] |= 0xC0;
        } else {
            first[7] |= 0x0C;
        }
        crc = crc32( first, 8 );
        return crc ^ crc32( data + 8, bps - 8 );
    }
    return crc32( data, bps );
}

/* ------------------------------------------------------------------ */
/* making the file                                                     */
/* ------------------------------------------------------------------ */

static void und_path( int drive, char *path )
{
    path[0] = (char)('A' + drive);
    path[1] = ':';
    path[2] = '\\';
    str_cpy( path + 3, UND_NAME );
}

static int und_put( const void *buf, u32 len )
{
    u32 done;

    return sys_write( und_handle, buf, len, &done ) != 0 || done != len ? -1 : 0;
}

int undo_active( void )
{
    return und_open;
}

int undo_create( int drive )
{
    char path[20];
    u8 head[UND_HEAD];
    u16 date, time;

    und_path( drive, path );
    if ( sys_create( path, &und_handle ) != 0 ) {
        return -1;
    }
    mem_set( head, 0, sizeof( head ) );
    mem_cpy( head, "PMSDUNDO", 8 );
    WR16( head + 8, 1 );
    WR16( head + 10, UND_HEAD );
    sys_get_datetime( &date, &time );
    WR16( head + 12, date );
    WR16( head + 14, time );
    und_open = 1;
    if ( und_put( head, sizeof( head ) ) != 0 || sys_commit( und_handle ) != 0 ) {
        sys_close( und_handle );
        und_open = 0;
        return -1;
    }
    und_drive = drive;
    und_vol_noted = 0;
    return 0;
}

void undo_close( void )
{
    if ( und_open ) {
        sys_close( und_handle );
        und_open = 0;
    }
}

void undo_drive_done( void )
{
    und_vol_noted = 0;
}

/* the drive about to be checked cannot be the one the Undo file is on:
   the file is closed, and the next repair asks again */
void undo_avoid( int drive )
{
    if ( und_open && und_drive == drive ) {
        undo_close();
        und_asked = 0;
    }
}

/* The file cannot take a record.  Nothing more is written to the
   volume: see the top of this file. */
static void und_lost( void )
{
    char text[200];

    undo_close();
    sfmt( text, sizeof( text ),
          "The Undo file on drive %c: cannot be written to.  Is the disk full, or out of the "
          "drive?  ScanDisk has stopped repairing drive %c:; what was repaired can still be "
          "undone.", 'A' + und_drive, 'A' + vol.drive );
    log_line( "" );
    log_para( text );
    fe_message( text );
    scan_quit( Q_DRIVE );
}

int undo_note( u32 lba, u32 count, const void *after )
{
    u8 rec[UV_LEN], *old, *sums;
    u32 sec;

    if ( !und_open ) {
        return 0;
    }
    if ( !und_vol_noted ) {
        mem_set( rec, 0, sizeof( rec ) );
        rec[0] = 'U';
        rec[1] = 'V';
        rec[2] = (u8)('A' + vol.drive);
        rec[3] = (u8)vol.fattype;
        WR16( rec + 4, vol.bps );
        WR32( rec + 6, vol.has_serial ? vol.serial : 0 );
        WR32( rec + 10, vol.tot_sec );
        WR32( rec + 14, vol.rsvd );
        WR32( rec + 18, vol.fatsz );
        WR32( rec + 22, vol.nfats );
        mem_cpy( rec + 26, vol.label, 12 );
        if ( und_put( rec, UV_LEN ) != 0 ) {
            und_lost();
        }
        und_vol_noted = 1;
    }
    old = (u8 *)try_alloc( count * vol.bps );
    sums = (u8 *)try_alloc( count * 5 );
    if ( old == NULL || sums == NULL ) {
        out_of_memory();
    }
    for ( sec = 0; sec < count; sec++ ) {
        sums[sec * 5] = 0;
        if ( read_secs( lba + sec, 1, old + sec * vol.bps ) != 0 ) {
            mem_set( old + sec * vol.bps, 0, vol.bps );
            sums[sec * 5] = UF_UNREAD;
        }
        WR32( sums + sec * 5 + 1,
              sector_crc( lba + sec, (const u8 *)after + sec * vol.bps, vol.fattype, vol.rsvd,
                          vol.fatsz, vol.nfats, vol.bps ) );
    }
    rec[0] = 'U';
    rec[1] = 'S';
    rec[2] = (u8)('A' + vol.drive);
    rec[3] = 0;
    WR16( rec + 4, count );
    WR32( rec + 6, lba );
    if ( und_put( rec, US_HEAD ) != 0 || und_put( sums, count * 5 ) != 0 ||
         und_put( old, count * vol.bps ) != 0 || sys_commit( und_handle ) != 0 ) {
        xfree( sums );
        xfree( old );
        und_lost();
    }
    xfree( sums );
    xfree( old );
    return 0;
}

/* Before the first repair of a run: is there to be an Undo file?  As
   MS-DOS has it, /NOSUMMARY is also "do not ask", and with /CUSTOM the
   Undo setting says. */
void undo_want( void )
{
    int have_a, have_b, drive;
    char text[160];

    if ( und_open || und_asked ) {
        return;
    }
    und_asked = 1;
    if ( opt.nosummary || (opt.mode == MODE_CUSTOM && !opt.undo_prompt) ) {
        return;
    }
    have_a = sys_drive_floppy( 0 ) && vol.drive != 0;
    have_b = sys_drive_floppy( 1 ) && vol.drive != 1;
    while ( have_a || have_b ) {
        drive = fe_ask_undo( have_a, have_b );
        if ( drive < 0 ) {
            break;
        }
        if ( undo_create( drive ) == 0 ) {
            log_line( "Undo information is in %c:\\%s.", 'A' + drive, UND_NAME );
            log_line( "" );
            return;
        }
        sfmt( text, sizeof( text ),
              "The Undo file cannot be made on the disk in drive %c:.  The disk has to be "
              "formatted, not write-protected, and have room.", 'A' + drive );
        fe_message( text );
    }
}

/* ------------------------------------------------------------------ */
/* SCANDISK /UNDO                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    u32 pos;                    /* where its sectors are in the file */
    u32 lba;
    u16 count;
    u8  drive;                  /* 0 = A: */
    u8  vol;                    /* which 'UV' it belongs to */
    u8 *sums;                   /* count * 5 */
} UREC;

typedef struct {
    u8  drive, fattype;
    u32 bps, serial, tot_sec, rsvd, fatsz, nfats;
} UVOL;

static UREC *urec;
static u32   nrec, caprec;
static UVOL  uvol[26];
static u32   nvol;

static int und_get( void *buf, u32 len )
{
    u32 done;

    return sys_read( und_handle, buf, len, &done ) != 0 || done != len ? -1 : 0;
}

/* the records, read through once: -1 = that is not an Undo file */
static int und_index( u32 size )
{
    u8 head[UND_HEAD], rec[UV_LEN];
    u32 pos = UND_HEAD, bps = 0, count;
    UREC *grown;

    if ( und_get( head, UND_HEAD ) != 0 || mem_cmp( head, "PMSDUNDO", 8 ) != 0 ||
         RD16( head + 8 ) != 1 || RD16( head + 10 ) != UND_HEAD ) {
        return -1;
    }
    for ( ;; ) {
        if ( pos + 2 > size || sys_seek( und_handle, pos ) != 0 || und_get( rec, 2 ) != 0 ) {
            break;
        }
        if ( rec[0] != 'U' ) {
            return -1;
        }
        if ( rec[1] == 'V' ) {
            if ( pos + UV_LEN > size || und_get( rec + 2, UV_LEN - 2 ) != 0 ) {
                break;                          /* cut short: never used */
            }
            if ( nvol == 26 || rec[2] < 'A' || rec[2] > 'Z' ) {
                return -1;
            }
            uvol[nvol].drive = (u8)(rec[2] - 'A');
            uvol[nvol].fattype = rec[3];
            uvol[nvol].bps = bps = RD16( rec + 4 );
            uvol[nvol].serial = RD32( rec + 6 );
            uvol[nvol].tot_sec = RD32( rec + 10 );
            uvol[nvol].rsvd = RD32( rec + 14 );
            uvol[nvol].fatsz = RD32( rec + 18 );
            uvol[nvol].nfats = RD32( rec + 22 );
            if ( bps < 128 || bps > 4096 || uvol[nvol].fatsz == 0 ) {
                return -1;
            }
            nvol++;
            pos += UV_LEN;
            continue;
        }
        if ( rec[1] != 'S' || nvol == 0 ) {
            return -1;
        }
        if ( pos + US_HEAD > size || und_get( rec + 2, US_HEAD - 2 ) != 0 ) {
            break;
        }
        count = RD16( rec + 4 );
        if ( count == 0 || rec[2] != 'A' + uvol[nvol - 1].drive ) {
            return -1;
        }
        if ( pos + US_HEAD + count * 5 + count * bps > size ) {
            break;                              /* a record the power cut short:
                                                   its write never happened */
        }
        if ( nrec == caprec ) {
            caprec = caprec ? caprec * 2 : 32;
            grown = (UREC *)xalloc( caprec * sizeof( UREC ) );
            if ( nrec ) {
                mem_cpy( grown, urec, nrec * sizeof( UREC ) );
            }
            xfree( urec );
            urec = grown;
        }
        urec[nrec].sums = (u8 *)xalloc( count * 5 );
        if ( und_get( urec[nrec].sums, count * 5 ) != 0 ) {
            break;
        }
        urec[nrec].pos = pos + US_HEAD + count * 5;
        urec[nrec].lba = RD32( rec + 6 );
        urec[nrec].count = (u16)count;
        urec[nrec].drive = uvol[nvol - 1].drive;
        urec[nrec].vol = (u8)(nvol - 1);
        nrec++;
        pos += US_HEAD + count * 5 + count * bps;
    }
    return 0;
}

/* was this sector of this drive written again by a later record? */
static int later_write( u32 from, u8 drive, u32 lba )
{
    u32 index;

    for ( index = from + 1; index < nrec; index++ ) {
        if ( urec[index].drive == drive && lba >= urec[index].lba &&
             lba - urec[index].lba < urec[index].count ) {
            return 1;
        }
    }
    return 0;
}

/* every sector still holds the last thing ScanDisk wrote to it */
static int und_unchanged( u32 which )
{
    const UVOL *uv = &uvol[which];
    u8 *sec = (u8 *)xalloc( uv->bps );
    u32 index, off, lba;
    int same = 1;

    for ( index = 0; index < nrec && same; index++ ) {
        if ( urec[index].vol != which ) {
            continue;
        }
        for ( off = 0; off < urec[index].count && same; off++ ) {
            lba = urec[index].lba + off;
            if ( later_write( index, uv->drive, lba ) ) {
                continue;
            }
            if ( sys_read_sec( uv->drive, lba, 1, uv->bps, sec ) != 0 ||
                 sector_crc( lba, sec, uv->fattype, uv->rsvd, uv->fatsz, uv->nfats,
                             uv->bps ) != RD32( urec[index].sums + off * 5 + 1 ) ) {
                same = 0;
            }
        }
    }
    xfree( sec );
    return same;
}

/* what went wrong, said in a box; nothing has been changed */
static int und_refuse( const char *text )
{
    fe_message( text );
    undo_close();
    return 255;
}

int undo_restore( int drive )
{
    char path[20], text[400], names[120], date[16], time[16];
    u8 head[UND_HEAD], *buf;
    u32 size, index, which, off;
    int bad = 0, len = 0;

    und_path( drive, path );
    if ( sys_open_read( path, &und_handle ) != 0 || sys_file_size( und_handle, &size ) != 0 ) {
        sfmt( text, sizeof( text ), "There is no Undo file (%s) on the disk in drive %c:.",
              UND_NAME, 'A' + drive );
        fe_message( text );
        return 255;
    }
    und_open = 1;
    if ( und_index( size ) != 0 ) {
        sfmt( text, sizeof( text ), "%s is not a ScanDisk Undo file, or is damaged.", path );
        return und_refuse( text );
    }
    if ( nrec == 0 ) {
        fe_message( "The Undo file records no changes.  There is nothing to undo." );
        undo_close();
        return 0;
    }
    sys_seek( und_handle, 0 );
    und_get( head, UND_HEAD );

    /* nothing is put back unless all of it can be */
    names[0] = 0;
    for ( which = 0; which < nvol; which++ ) {
        if ( uvol[which].drive == drive ) {
            sfmt( text, sizeof( text ),
                  "The Undo file is on drive %c:, which is a drive it has changes for.",
                  'A' + drive );
            return und_refuse( text );
        }
        if ( vol_open( uvol[which].drive ) != 0 || vol.bps != uvol[which].bps ||
             vol.tot_sec != uvol[which].tot_sec ||
             (vol.has_serial ? vol.serial : 0) != uvol[which].serial ) {
            sfmt( text, sizeof( text ),
                  "Drive %c: is not the volume the Undo file was made for.",
                  'A' + uvol[which].drive );
            return und_refuse( text );
        }
        if ( !und_unchanged( which ) ) {
            sfmt( text, sizeof( text ),
                  "Drive %c: has been written to since ScanDisk repaired it.  Putting the old "
                  "sectors back now would damage the drive, so nothing has been changed.",
                  'A' + uvol[which].drive );
            return und_refuse( text );
        }
        len += sfmt( names + len, sizeof( names ) - (u32)len, "%s%c:",
                     which == 0 ? "" : which + 1 == nvol ? " and " : ", ",
                     'A' + uvol[which].drive );
    }

    sfmt( text, sizeof( text ),
          "The Undo file on drive %c: was made on %s at %s.  It holds what ScanDisk's repairs "
          "changed on drive%s %s.\n\nPut %s back as %s before the repairs?",
          'A' + drive, fmt_date( RD16( head + 12 ), date ), fmt_time( RD16( head + 14 ), time ),
          nvol == 1 ? "" : "s", names, nvol == 1 ? "the drive" : "the drives",
          nvol == 1 ? "the drive was" : "the drives were" );
    if ( !fe_confirm( "Undo", text ) ) {
        undo_close();
        return 3;
    }

    /* last written, first put back */
    fe_busy( "ScanDisk is putting the sectors back as they were." );
    for ( index = nrec; index-- > 0; ) {
        const UVOL *uv = &uvol[urec[index].vol];

        buf = (u8 *)xalloc( (u32)urec[index].count * uv->bps );
        if ( sys_seek( und_handle, urec[index].pos ) != 0 ||
             und_get( buf, (u32)urec[index].count * uv->bps ) != 0 ) {
            bad = 1;
        } else {
            for ( off = 0; off < urec[index].count; off++ ) {
                if ( urec[index].sums[off * 5] & UF_UNREAD ) {
                    continue;                   /* nobody knows what was there */
                }
                if ( sys_write_sec( uv->drive, urec[index].lba + off, 1, uv->bps,
                                    buf + off * uv->bps, WR_UNKNOWN ) != 0 ) {
                    bad = 1;
                }
            }
        }
        xfree( buf );
        fe_progress( nrec - index, nrec );
    }
    for ( which = 0; which < nvol; which++ ) {
        sys_reset_drive( uvol[which].drive );
    }
    undo_close();
    if ( bad ) {
        fe_message( "Some sectors could not be written back.  Run ScanDisk on the drive." );
        return 255;
    }
    fe_message( "The repairs have been undone." );
    return 0;
}
