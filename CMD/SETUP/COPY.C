/*
 * COPY.C - where the Setup files are, the disks they are on, and
 * copying them.
 *
 * THE SOURCE.  The 16-bit half is told its own path by DOS (the name
 * after the environment).  A native PM-DOS program is not, so the
 * 32-bit half looks: the current directory, the root of the drive the
 * machine started from, and then every drive that is a diskette, a CD,
 * a USB disk or a network drive.  Of the places with a SETUP.INF the
 * one holding the most disks' tag files wins - which is how a machine
 * started from the CD (the BIOS shows disk 1 as drive A:) ends up
 * reading everything from the disc instead of asking for disk 2.
 *
 * THE DISKS.  Every Setup disk carries a file named in SETUP.INF's
 * [disks], DISK1 on disk 1 and so on, and that is what Setup looks for.
 * A wrong disk brings the same request back, as 6.22's did.  A source
 * that is not a diskette has all the tags or is not a Setup source.
 *
 * PACKED FILES are SZDD, the format MS-DOS 5's EXPAND reads: a 14-byte
 * header, then LZSS with a 4K window that starts full of spaces and is
 * written from 4096-16, a flag byte read from its low bit up, 1 for a
 * byte as it is and 0 for a 12-bit position and a 4-bit length less 3.
 *
 * THE GAUGE counts bytes as they are written, against the size of all
 * that will be written, and stays at 99% until the last file is shut.
 */
#include "setup.h"
#include "text.h"

char srcdir[PATHLEN];
int  src_drive;
int  src_removable;

static u8 *buf_in, *buf_out;
static u8 window[4096];
static u32 gauge_done, gauge_total;
static char status_text[40];

/* ------------------------------------------------------------------ */
/* the source                                                          */
/* ------------------------------------------------------------------ */

static int tags_in( const char *dir )
{
    char path[PATHLEN];
    u16 attr;
    int disk, have = 0;

    for ( disk = 1; disk <= inf.ndisks; disk++ ) {
        path_join( path, dir, inf.disktag[disk], sizeof( path ) );
        if ( plat_get_attr( path, &attr ) == 0 ) {
            have++;
        }
    }
    return have;
}

static int has_inf( const char *dir )
{
    char path[PATHLEN];
    u16 attr;

    path_join( path, dir, "SETUP.INF", sizeof( path ) );
    return plat_get_attr( path, &attr ) == 0;
}

static void set_source( const char *dir )
{
    DRIVEINFO di;

    str_cpyn( srcdir, dir, sizeof( srcdir ) );
    if ( srcdir[0] && srcdir[str_len( srcdir ) - 1] != '\\' ) {
        str_catn( srcdir, "\\", sizeof( srcdir ) );
    }
    str_upper( srcdir );
    src_drive = srcdir[0] - 'A';
    src_removable = 0;
    if ( plat_drive( src_drive, &di ) == 0 && (di.flags & DI_FLOPPY) ) {
        src_removable = 1;
    }
}

/* Where the Setup files are.  -> 0 when SETUP.INF was found (srcdir is
   set; inf is NOT loaded yet). */
int source_find( void )
{
    char dir[PATHLEN];
    const char *self = plat_self();
    char *slash;
    DRIVEINFO di;
    int drive;

    if ( self[0] ) {
        str_cpyn( dir, self, sizeof( dir ) );
        slash = str_rchr( dir, '\\' );
        if ( slash ) {
            slash[1] = 0;
            if ( has_inf( dir ) ) {
                set_source( dir );
                return 0;
            }
        }
    }
    /* the current directory, then the root Setup started from */
    if ( plat_current_dir( dir, sizeof( dir ) ) == 0 && has_inf( dir ) ) {
        set_source( dir );
        return 0;
    }
    dir[0] = (char)('A' + plat_boot_drive());
    dir[1] = ':';
    dir[2] = '\\';
    dir[3] = 0;
    if ( has_inf( dir ) ) {
        set_source( dir );
        return 0;
    }
    for ( drive = 0; drive < 26; drive++ ) {
        if ( plat_drive( drive, &di ) || !(di.flags & DI_PRESENT) ||
             (di.flags & DI_FIXED) ) {
            continue;
        }
        if ( (di.flags & DI_FLOPPY) && drive != plat_boot_drive() ) {
            continue;                   /* no need to rattle B: */
        }
        dir[0] = (char)('A' + drive);
        if ( has_inf( dir ) ) {
            set_source( dir );
            return 0;
        }
    }
    return 1;
}

/* With SETUP.INF read: if the source lacks disks another drive holds
   all of (a CD after a start from its floppy image), use that one. */
void source_better( void )
{
    char dir[4];
    DRIVEINFO di;
    int drive, have = tags_in( srcdir );

    if ( have >= inf.ndisks ) {
        return;
    }
    dir[1] = ':';
    dir[2] = '\\';
    dir[3] = 0;
    for ( drive = 2; drive < 26; drive++ ) {
        if ( drive == src_drive || plat_drive( drive, &di ) ||
             !(di.flags & DI_PRESENT) || !(di.flags & (DI_CDROM | DI_REMOVABLE | DI_REMOTE)) ) {
            continue;
        }
        dir[0] = (char)('A' + drive);
        if ( has_inf( dir ) && tags_in( dir ) == inf.ndisks ) {
            set_source( dir );
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the disks                                                           */
/* ------------------------------------------------------------------ */

static int disk_now;

/* the next disk_wanted asks, whatever was in the drive before */
void disk_forget( void )
{
    disk_now = 0;
}

/* 0 = that disk is where Setup can read it */
int disk_wanted( int disk )
{
    static const int keys[] = { K_ENTER, 0 };
    char path[PATHLEN], line[60];
    u16 attr;
    DLG dlg;

    for ( ;; ) {
        path_join( path, srcdir, inf.disktag[disk], sizeof( path ) );
        if ( plat_get_attr( path, &attr ) == 0 ) {
            disk_now = disk;
            return 0;
        }
        if ( !src_removable ) {
            return 1;
        }
        sfmt( line, sizeof( line ), tx_insert_1, srcdir[0] );
        dlg_start( &dlg, "", 44 );
        dlg_blank( &dlg );
        dlg_center( &dlg, line );
        dlg_blank( &dlg );
        dlg_center( &dlg, inf.diskname[disk] );
        dlg_blank( &dlg );
        dlg_center( &dlg, tx_insert_3 );
        dlg_blank( &dlg );
        dlg_run( &dlg, A_NOTE, tx_keys_ex, keys, 1 );
        plat_vol_remount( src_drive );
    }
}

/* ------------------------------------------------------------------ */
/* copying                                                             */
/* ------------------------------------------------------------------ */

static void show( const char *what, const char *name )
{
    char lower[16];
    int i;

    str_cpyn( lower, name, sizeof( lower ) );
    for ( i = 0; lower[i]; i++ ) {
        if ( lower[i] >= 'A' && lower[i] <= 'Z' ) {
            lower[i] = (char)(lower[i] + 32);
        }
    }
    sfmt( status_text, sizeof( status_text ), "%s%s", what, lower );
    scr_status( "", status_text );
}

static void tick( u32 bytes )
{
    gauge_done += bytes;
    if ( gauge_total ) {
        scr_gauge( gauge_done, gauge_total );
        scr_flush();
    }
}

/* Where copied bytes go: a file, or memory (a startup floppy made in
   the drive the Setup disks are read from has to be read in first) */
typedef struct {
    u16   handle;
    HELD *held;                         /* NULL = the file               */
} SINK;

/* Memory runs out as a write error would: the caller then writes what
   it holds and comes back for the rest.  The budget leaves the rest of
   Setup room to work in - 256K in the 16-bit half, under MS-DOS. */
static u32 held_total;

static int hold_bytes( HELD *held, const u8 *data, u16 len )
{
    u32 budget = (plat_caps & PC_32BIT) ? 0x800000ul : 0x40000ul;
    u16 room, step;

    while ( len ) {
        if ( held->n == 0 || held->len[held->n - 1] == COPYBUF ) {
            if ( held->n == HELD_PIECES || held_total + COPYBUF > budget ) {
                return 1;
            }
            held->piece[held->n] = (u8 *)plat_alloc( COPYBUF );
            if ( held->piece[held->n] == NULL ) {
                return 1;
            }
            held_total += COPYBUF;
            held->len[held->n++] = 0;
        }
        room = COPYBUF - held->len[held->n - 1];
        step = len < room ? len : room;
        mem_cpy( held->piece[held->n - 1] + held->len[held->n - 1], data, step );
        held->len[held->n - 1] += step;
        data += step;
        len -= step;
    }
    return 0;
}

static int write_all( SINK *sink, const u8 *data, u16 len )
{
    u16 done;

    if ( len == 0 ) {
        return 0;
    }
    if ( sink->held ) {
        if ( hold_bytes( sink->held, data, len ) ) {
            return 1;
        }
    } else if ( plat_write( sink->handle, data, len, &done ) || done != len ) {
        return 1;
    }
    tick( len );
    return 0;
}

/* SZDD in, plain out.  -> 0, or 1 for a bad file, 2 for a write error */
static int expand_into( u16 in, SINK *out, u32 *written )
{
    u16 have = 0, at = 0, outlen = 0, got;
    u32 total = 0, want;
    int pos = 4096 - 16, bits = 0, flags = 0, i, len, from;
    u8 byte, low;

#define NEXT( var ) \
    if ( at == have ) { \
        if ( plat_read( in, buf_in, COPYBUF, &got ) ) return 1; \
        have = got; at = 0; \
        if ( have == 0 ) goto ended; \
    } \
    var = buf_in[at++]

#define EMIT( val ) \
    buf_out[outlen++] = (val); \
    window[pos] = (val); \
    pos = (pos + 1) & 4095; \
    total++; \
    if ( outlen == COPYBUF ) { \
        if ( write_all( out, buf_out, outlen ) ) return 2; \
        outlen = 0; \
    }

    if ( plat_read( in, buf_in, 14, &got ) || got != 14 ||
         mem_cmp( buf_in, "SZDD\x88\xF0\x27\x33", 8 ) || buf_in[8] != 'A' ) {
        return 1;
    }
    want = RD32( buf_in + 10 );
    mem_set( window, ' ', sizeof( window ) );
    for ( ;; ) {
        if ( bits == 0 ) {
            NEXT( byte );
            flags = byte;
            bits = 8;
        }
        if ( flags & 1 ) {
            NEXT( byte );
            EMIT( byte );
        } else {
            NEXT( low );
            NEXT( byte );
            from = low | ((byte & 0xF0) << 4);
            len = (byte & 0x0F) + 3;
            for ( i = 0; i < len; i++ ) {
                u8 val = window[(from + i) & 4095];
                EMIT( val );
            }
        }
        flags >>= 1;
        bits--;
    }
ended:
    if ( write_all( out, buf_out, outlen ) ) {
        return 2;
    }
    *written = total;
    return total == want ? 0 : 1;
#undef NEXT
#undef EMIT
}

int expand_file( u16 in, u16 out, u32 *written )
{
    SINK sink;

    sink.handle = out;
    sink.held = NULL;
    return expand_into( in, &sink, written );
}

static int raw_copy( u16 in, SINK *out, u32 *written )
{
    u16 got;
    u32 total = 0;

    for ( ;; ) {
        if ( plat_read( in, buf_in, COPYBUF, &got ) ) {
            return 1;
        }
        if ( got == 0 ) {
            break;
        }
        if ( write_all( out, buf_in, got ) ) {
            return 2;
        }
        total += got;
    }
    *written = total;
    return 0;
}

/* The file's whole path on the target drive */
void copy_dest( const INFFILE *file, char *dest, int max )
{
    char dir[PATHLEN];

    if ( file->where == WH_ROOT ) {
        dir[0] = (char)('A' + tgt.drive);
        dir[1] = ':';
        dir[2] = '\\';
        dir[3] = 0;
    } else {
        str_cpyn( dir, opt.dosdir, sizeof( dir ) );
        dir[0] = (char)('A' + tgt.drive);       /* C: when PM-DOS runs */
    }
    path_join( dest, dir, file->dst, max );
}

int copy_error_box( const char *fmt, const char *name )
{
    static const int keys[] = { K_ENTER, 0 };
    char line[80];
    DLG dlg;

    sfmt( line, sizeof( line ), fmt, name );
    dlg_start( &dlg, tx_cperr_title, 44 );
    dlg_blank( &dlg );
    dlg_para( &dlg, line );
    dlg_blank( &dlg );
    dlg_para( &dlg, tx_cperr_go );
    dlg_blank( &dlg );
    dlg_run( &dlg, A_BAR, tx_keys_ex, keys, 1 );
    return 1;
}

/* One file from the source to "dest", expanded on the way if packed.
   -> 0 done, 1 not done (the user was told unless "quiet") */
int copy_one( const INFFILE *file, const char *dest, int quiet )
{
    char src[PATHLEN];
    u16 in, out, date, time, attr;
    u32 written = 0;
    SINK sink;
    int rc;

    if ( buf_in == NULL ) {
        buf_in = (u8 *)xalloc( COPYBUF );
        buf_out = (u8 *)xalloc( COPYBUF );
    }
    path_join( src, srcdir, file->src, sizeof( src ) );
    if ( !quiet ) {
        show( tx_reading, file->dst );
    }
    if ( plat_open( src, 0, &in ) ) {
        return quiet ? 1 : copy_error_box( tx_cperr_read, file->src );
    }
    if ( plat_get_ftime( in, &date, &time ) ) {
        date = time = 0;
    }
    /* a read-only, hidden or system file of that name would refuse 3Ch */
    if ( plat_get_attr( dest, &attr ) == 0 && (attr & (FA_RDONLY | FA_HIDDEN | FA_SYSTEM)) ) {
        plat_set_attr( dest, 0 );
    }
    if ( plat_create( dest, &out ) ) {
        plat_close( in );
        return quiet ? 1 : copy_error_box( tx_cperr_write, dest );
    }
    if ( !quiet ) {
        show( tx_writing, file->dst );
    }
    sink.handle = out;
    sink.held = NULL;
    rc = (file->flags & FF_PACKED) ? expand_into( in, &sink, &written )
                                   : raw_copy( in, &sink, &written );
    if ( rc == 0 && date ) {
        plat_set_ftime( out, date, time );
    }
    plat_close( in );
    if ( plat_close( out ) && rc == 0 ) {
        rc = 2;
    }
    if ( rc == 0 && file->size && written != file->size ) {
        rc = 1;
    }
    if ( rc ) {
        plat_delete( dest );
        return quiet ? 1 : copy_error_box( rc == 2 ? tx_cperr_write : tx_cperr_read,
                                       rc == 2 ? dest : file->src );
    }
    if ( file->flags & FF_SYSTEM ) {
        plat_set_attr( dest, FA_RDONLY | FA_HIDDEN | FA_SYSTEM );
    }
    return 0;
}

/* the bytes the gauge counts to */
u32 copy_total( int floppy )
{
    u32 total = 0;
    int i;

    for ( i = 0; i < inf.nfiles; i++ ) {
        if ( inf.file[i].where == WH_SETUP ) {
            continue;
        }
        if ( floppy && !(inf.file[i].flags & FF_FLOPPY) ) {
            continue;
        }
        total += inf.file[i].size;
    }
    return total;
}

static void copy_screen( void )
{
    scr_desk( setup_title() );
    scr_para( 4, tx_copying );
    scr_gauge( gauge_done, gauge_total );
    scr_status( "", status_text );
    scr_flush();
}

/* Every file to drive C:, disk by disk.  The directory is made first;
   on an upgrade the old command each new one replaces is moved out of
   the way (UNDO.C) before the new one is written.  -> 0 when all went */
int copy_all( void )
{
    char dest[PATHLEN];
    int disk, i;
    INFFILE *file;

    gauge_done = 0;
    gauge_total = copy_total( 0 );
    status_text[0] = 0;
    copy_screen();
    for ( disk = 1; disk <= inf.ndisks; disk++ ) {
        for ( i = 0; i < inf.nfiles; i++ ) {
            file = &inf.file[i];
            if ( file->disk != disk || file->where == WH_SETUP ) {
                continue;
            }
            if ( disk_now != disk ) {
                if ( disk_wanted( disk ) ) {
                    copy_error_box( tx_cperr_read, inf.disktag[disk] );
                    return 1;
                }
                copy_screen();
            }
            copy_dest( file, dest, sizeof( dest ) );
            if ( opt.upgrade && file->where == WH_DOS ) {
                undo_save( dest );
            }
            if ( copy_one( file, dest, 0 ) ) {
                gauge_done += file->size;       /* skipped: count it anyway */
            }
            copy_screen();
        }
    }
    gauge_done = gauge_total;
    copy_screen();
    return 0;
}

/* ------------------------------------------------------------------ */
/* files held in memory                                                */
/* ------------------------------------------------------------------ */

void held_free( HELD *held )
{
    while ( held->n > 0 ) {
        held->n--;
        xfree( held->piece[held->n] );
        held_total -= COPYBUF;
    }
}

/* A file read whole, as it is.  0 = read */
int held_read( HELD *held, const char *path )
{
    SINK sink;
    u16 in;
    u32 written;
    int rc;

    if ( buf_in == NULL ) {
        buf_in = (u8 *)xalloc( COPYBUF );
        buf_out = (u8 *)xalloc( COPYBUF );
    }
    held->n = 0;
    held->date = held->time = 0;
    if ( plat_open( path, 0, &in ) ) {
        return 1;
    }
    plat_get_ftime( in, &held->date, &held->time );
    sink.held = held;
    rc = raw_copy( in, &sink, &written );
    plat_close( in );
    if ( rc ) {
        held_free( held );
    }
    return rc;
}

/* A file from the Setup disk, expanded.  0 = read, 1 = it could not
   be, 3 = there is no memory left for it */
int held_copy( HELD *held, const INFFILE *file )
{
    char src[PATHLEN];
    SINK sink;
    u16 in;
    u32 written;
    int rc;

    if ( buf_in == NULL ) {
        buf_in = (u8 *)xalloc( COPYBUF );
        buf_out = (u8 *)xalloc( COPYBUF );
    }
    held->n = 0;
    path_join( src, srcdir, file->src, sizeof( src ) );
    show( tx_reading, file->dst );
    if ( plat_open( src, 0, &in ) ) {
        return 1;
    }
    if ( plat_get_ftime( in, &held->date, &held->time ) ) {
        held->date = held->time = 0;
    }
    sink.held = held;
    rc = (file->flags & FF_PACKED) ? expand_into( in, &sink, &written )
                                   : raw_copy( in, &sink, &written );
    plat_close( in );
    if ( rc ) {
        held_free( held );
        return rc == 2 ? 3 : 1;
    }
    return 0;
}

/* What is held, written out as "path".  0 = written */
int held_write( HELD *held, const char *path, u16 attr )
{
    u16 handle, done, old;
    int i, rc = 0;

    if ( plat_get_attr( path, &old ) == 0 && (old & (FA_RDONLY | FA_HIDDEN | FA_SYSTEM)) ) {
        plat_set_attr( path, 0 );
    }
    if ( plat_create( path, &handle ) ) {
        return 1;
    }
    for ( i = 0; i < held->n && rc == 0; i++ ) {
        if ( held->len[i] &&
             (plat_write( handle, held->piece[i], held->len[i], &done ) || done != held->len[i]) ) {
            rc = 1;
        } else {
            tick( held->len[i] );
        }
    }
    if ( rc == 0 && held->date ) {
        plat_set_ftime( handle, held->date, held->time );
    }
    if ( plat_close( handle ) ) {
        rc = 1;
    }
    if ( rc == 0 && attr ) {
        plat_set_attr( path, attr );
    }
    if ( rc ) {
        plat_delete( path );
    }
    return rc;
}

void gauge_start( u32 total )
{
    gauge_done = 0;
    gauge_total = total;
    status_text[0] = 0;
}

void gauge_finish( void )
{
    gauge_done = gauge_total;
    scr_gauge( gauge_done, gauge_total );
    scr_status( "", NULL );
    scr_flush();
}
