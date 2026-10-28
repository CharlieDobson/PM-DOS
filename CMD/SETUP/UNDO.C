/*
 * UNDO.C - keeping the old DOS, and putting it back.
 *
 * NOTHING OF THE OLD DOS IS DELETED.  An upgrade makes C:\OLD_DOS.n
 * (the first n free, as 6.x did) and puts in it:
 *   BOOTSECT.DOS   drive C:'s boot sector as it was
 *   CONFIG.DAT     CONFIG.SYS and AUTOEXEC.BAT as they were, before CFG.C
 *   AUTOEXEC.DAT   edits them (6.x's names for them, on its Uninstall disk)
 *   every old command a PM-DOS file would hide - the same name, or the
 *     same name with .COM, .EXE or .BAT - MOVED there, not copied.  A
 *     name SETUP.INF installs in the directory itself is left for its
 *     own turn: by the time another file's turn comes it may be the new
 *     one already
 *   UNINSTAL.DAT   what Uninstall needs (below)
 *   README.NOW     what the directory is
 * Setup's own files have names no DOS command has, so an old file of the
 * same name is never taken for one of them.
 * The old system files (IO.SYS, MSDOS.SYS and the old COMMAND.COM in
 * the root) are not touched at all: PM-DOS starts from its own, and
 * the old boot sector still finds them.  Putting the old DOS back is
 * therefore the old boot sector, the moved commands, the two files,
 * and deleting what PM-DOS added.
 *
 * THE UNINSTALL DISK starts the OLD DOS, not PM-DOS, so that it works
 * on a machine PM-DOS cannot run on: its boot sector is drive C:'s old
 * one with the diskette's BPB in it, and IO.SYS and MSDOS.SYS (or the
 * IBM names) are the first files written to a freshly cleared disk,
 * so they are the first two entries and IO.SYS starts the data area,
 * which is what that boot code wants.  Then the old COMMAND.COM, this
 * program as UNINSTAL.EXE, UNINSTAL.DAT and an AUTOEXEC.BAT that runs
 * it.  Started under the name UNINSTAL, SETUP.EXE's 16-bit half is
 * Uninstall (uninstall_main).
 *
 * UNINSTAL.DAT is text: product=, olddos=, dosdir=, one del= line for
 * every file PM-DOS put on the drive, then an rmdir= line for each level
 * of the PM-DOS directory Setup made, the deepest first.
 */
#include "setup.h"
#include "text.h"

static char olddos[PATHLEN];            /* "C:\OLD_DOS.1", as now        */
static int  had_config, had_autoexec;   /* before PM-DOS came */

static int file_copy( const char *from, const char *to )
{
    u8 *buf = (u8 *)xalloc( COPYBUF );
    u16 in, out, got, done;
    int rc = 0;

    if ( plat_open( from, 0, &in ) ) {
        xfree( buf );
        return 1;
    }
    if ( plat_create( to, &out ) ) {
        plat_close( in );
        xfree( buf );
        return 1;
    }
    for ( ;; ) {
        if ( plat_read( in, buf, COPYBUF, &got ) ) {
            rc = 1;
            break;
        }
        if ( got == 0 ) {
            break;
        }
        if ( plat_write( out, buf, got, &done ) || done != got ) {
            rc = 1;
            break;
        }
    }
    plat_close( in );
    if ( plat_close( out ) ) {
        rc = 1;
    }
    xfree( buf );
    return rc;
}

static int write_text( const char *path, const char *text )
{
    u16 handle, done;
    int len = str_len( text ), rc;

    if ( plat_create( path, &handle ) ) {
        return 1;
    }
    rc = plat_write( handle, text, (u16)len, &done ) || done != (u16)len;
    return plat_close( handle ) || rc;
}

/* the lines of UNINSTAL.DAT, built once: a line for every file and
   eight more, each no longer than a path and its "del=" */
static char *dat_text( void )
{
    u16 size = (u16)((inf.nfiles + 8) * (PATHLEN + 8));
    char *dat = (char *)xalloc( size ), line[PATHLEN + 8], dest[PATHLEN];
    int i;

    dat[0] = 0;
    sfmt( line, sizeof( line ), "product=%s\r\n", inf.product );
    str_catn( dat, line, size );
    sfmt( line, sizeof( line ), "olddos=%c%s\r\n", tgt.final, olddos + 1 );
    str_catn( dat, line, size );
    sfmt( line, sizeof( line ), "dosdir=%s\r\n", opt.dosdir );
    str_catn( dat, line, size );
    for ( i = 0; i < inf.nfiles; i++ ) {
        if ( inf.file[i].where == WH_SETUP ) {
            continue;
        }
        copy_dest( &inf.file[i], dest, sizeof( dest ) );
        dest[0] = tgt.final;
        sfmt( line, sizeof( line ), "del=%s\r\n", dest );
        str_catn( dat, line, size );
    }
    sfmt( line, sizeof( line ), "del=%c:\\PMBOOT.SYS\r\n", tgt.final );
    str_catn( dat, line, size );
    /* the two files Setup wrote where there were none go too */
    if ( !had_config ) {
        sfmt( line, sizeof( line ), "del=%c:\\CONFIG.SYS\r\n", tgt.final );
        str_catn( dat, line, size );
    }
    if ( !had_autoexec ) {
        sfmt( line, sizeof( line ), "del=%c:\\AUTOEXEC.BAT\r\n", tgt.final );
        str_catn( dat, line, size );
    }
    for ( i = nmade_dirs - 1; i >= 0; i-- ) {
        sfmt( line, sizeof( line ), "rmdir=%s\r\n", made_dirs[i] );
        str_catn( dat, line, size );
    }
    return dat;
}

/* OLD_DOS.n with the boot sector and the two files in it.  0 = ready */
int undo_prepare( void )
{
    char path[PATHLEN], dir[PATHLEN], *dat;
    u8 *boot;
    u16 attr, handle, done;
    int n, rc;

    for ( n = 1; n < 100; n++ ) {
        sfmt( olddos, sizeof( olddos ), "%c:\\OLD_DOS.%d", 'A' + tgt.drive, n );
        if ( plat_get_attr( olddos, &attr ) ) {
            break;
        }
    }
    if ( n == 100 || plat_mkdir( olddos ) ) {
        return 1;
    }

    boot = (u8 *)xalloc( 512 );
    if ( plat_vol_read( tgt.drive, 0, 1, boot ) ) {
        return 1;
    }
    path_join( path, olddos, "BOOTSECT.DOS", sizeof( path ) );
    if ( plat_create( path, &handle ) ) {
        return 1;
    }
    rc = plat_write( handle, boot, 512, &done ) || done != 512;
    if ( plat_close( handle ) || rc ) {
        return 1;
    }
    xfree( boot );

    sfmt( dir, sizeof( dir ), "%c:\\CONFIG.SYS", 'A' + tgt.drive );
    path_join( path, olddos, "CONFIG.DAT", sizeof( path ) );
    had_config = plat_get_attr( dir, &attr ) == 0;
    if ( had_config && file_copy( dir, path ) ) {
        return 1;
    }
    sfmt( dir, sizeof( dir ), "%c:\\AUTOEXEC.BAT", 'A' + tgt.drive );
    path_join( path, olddos, "AUTOEXEC.DAT", sizeof( path ) );
    had_autoexec = plat_get_attr( dir, &attr ) == 0;
    if ( had_autoexec && file_copy( dir, path ) ) {
        return 1;
    }
    dat = dat_text();
    path_join( path, olddos, "UNINSTAL.DAT", sizeof( path ) );
    rc = write_text( path, dat );
    xfree( dat );
    path_join( path, olddos, "README.NOW", sizeof( path ) );
    rc |= write_text( path,
        "This directory holds what PM-DOS Setup replaced: the old DOS\r\n"
        "commands, CONFIG.SYS and AUTOEXEC.BAT (as CONFIG.DAT and\r\n"
        "AUTOEXEC.DAT) and the old boot sector (BOOTSECT.DOS).  The Uninstall\r\n"
        "disk puts them back.  Once PM-DOS works on this computer and you\r\n"
        "have no further use for the Uninstall disk, you can delete this\r\n"
        "directory.\r\n" );
    return rc;
}

/* the directory OLD_DOS.n is, as PM-DOS will see it */
const char *undo_dir( void )
{
    static char final_dir[PATHLEN];

    sfmt( final_dir, sizeof( final_dir ), "%c%s", tgt.final, olddos + 1 );
    return final_dir;
}

/* Before "dest" is written: an old file that it replaces, or that
   would run in its place (the same name as a .COM, .EXE or .BAT),
   goes into OLD_DOS.n. */
int undo_save( const char *dest )
{
    static const char *exts[] = { "", ".COM", ".EXE", ".BAT" };
    char base[PATHLEN], path[PATHLEN], name[16], into[PATHLEN];
    char *dot, *slash;
    u16 attr;
    int i;

    if ( !olddos[0] ) {
        return 0;
    }
    str_cpyn( base, dest, sizeof( base ) );
    slash = str_rchr( base, '\\' );
    dot = str_rchr( base, '.' );
    if ( dot && dot > slash ) {
        *dot = 0;
    }
    for ( i = 0; i < 4; i++ ) {
        if ( i == 0 ) {
            str_cpyn( path, dest, sizeof( path ) );
        } else {
            sfmt( path, sizeof( path ), "%s%s", base, exts[i] );
            if ( !str_icmp( path, dest ) ) {
                continue;
            }
        }
        if ( plat_get_attr( path, &attr ) || (attr & FA_DIR) ) {
            continue;
        }
        path_name( name, path, sizeof( name ) );
        /* SCANDISK.INI's turn must not take SCANDISK.EXE: a name Setup
           installs here is saved on its own turn, and may be ours now */
        if ( i > 0 ) {
            INFFILE *file = inf_file( name );
            if ( file && file->where == WH_DOS ) {
                continue;
            }
        }
        path_join( into, olddos, name, sizeof( into ) );
        if ( plat_get_attr( into, &attr ) == 0 ) {
            continue;                   /* saved already (a second run) */
        }
        plat_rename( path, into );
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the Uninstall disk                                                  */
/* ------------------------------------------------------------------ */

/* the disk's own BPB under the old DOS's boot code */
static void old_boot( u8 *boot, const u8 *disk_boot )
{
    mem_cpy( boot + 0x0B, disk_boot + 0x0B, 0x3E - 0x0B );
    boot[0x24] = 0;                     /* a diskette */
    mem_cpy( boot + 0x2B, "UNINSTALL  ", 11 );
}

/* The page that says how to label a disk before Setup asks for it, as
   6.22 had for its STARTUP and UNINSTALL disks: what the disk is for,
   "label a floppy disk as follows:", the label at column 14, a note,
   "press ENTER" */
void label_page( const char *intro, const char *name, const char *note,
                 const char *help_head, const char *help_text )
{
    int row, key;

    for ( ;; ) {
        scr_desk( setup_title() );
        row = 4 + scr_para( 4, intro ) + 1;
        scr_para( row++, tx_ud_label );
        scr_put( ++row, 14, A_DESK, name );
        row += 2;
        if ( note ) {
            row += scr_para( row, note ) + 1;
        }
        scr_para( row, tx_ud_3 );
        scr_status( tx_keys_ehx, NULL );
        key = key_get();
        if ( key == K_ENTER ) {
            return;
        }
        if ( key == K_F1 ) {
            ui_help( help_head, help_text );
        } else if ( key == K_F3 ) {
            ui_exit_box();
        }
    }
}

/* 0 = made; 1 = the user went on without one */
int undo_disk( void )
{
    static const int keys[] = { K_ENTER, 0 };
    char path[PATHLEN], self[PATHLEN], from[PATHLEN], line[60];
    HELD prog;
    u8 *boot, *disk_boot;
    DLG dlg;
    int drive = 0, rc, held, i;
    char *dat;

    label_page( tx_ud_1, tx_ud_name, tx_ud_2, tx_h_ud, tx_h_ud_text );

    /* This program goes on the disk as UNINSTAL.EXE.  When it was
       started from the drive the disk is made in, it is read in first;
       the old DOS's files come from drive C:, which stays where it is. */
    str_cpyn( self, plat_self(), sizeof( self ) );
    if ( !self[0] ) {
        path_join( self, srcdir, "SETUP.EXE", sizeof( self ) );
    }
    held = self[0] - 'A' == drive;
    if ( held ) {
        ui_working( tx_ud_write );
        if ( held_read( &prog, self ) ) {
            return copy_error_box( tx_cperr_read, self );
        }
    }
    boot = (u8 *)xalloc( 512 );
    disk_boot = (u8 *)xalloc( 512 );
    path_join( path, olddos, "BOOTSECT.DOS", sizeof( path ) );
    {
        u16 handle, got;
        if ( plat_open( path, 0, &handle ) || plat_read( handle, boot, 512, &got ) || got != 512 ) {
            return copy_error_box( tx_cperr_read, path );
        }
        plat_close( handle );
    }

    for ( ;; ) {
        /* the disk in the drive */
        sfmt( line, sizeof( line ), tx_ud_insert, tx_ud_name, 'A' + drive );
        dlg_start( &dlg, "", 42 );
        dlg_blank( &dlg );
        dlg_center( &dlg, line );
        dlg_blank( &dlg );
        dlg_center( &dlg, tx_ud_erase );
        dlg_blank( &dlg );
        dlg_center( &dlg, tx_ud_ready );
        dlg_blank( &dlg );
        dlg_run( &dlg, A_NOTE, tx_keys_ex, keys, 1 );
        plat_vol_remount( drive );      /* a different disk now: as 6.22, a */
                                        /* reset after every prompt      */
        if ( floppy_has_tag( drive, NULL ) ) {
            ui_notice( tx_error_title, tx_bad_setupdisk, A_BAR, tx_keys_ex );
            continue;
        }
        rc = floppy_format( drive, "UNINSTALL", NULL );
        if ( rc == 2 ) {
            sfmt( line, sizeof( line ), tx_bad_wp, 'A' + drive );
            ui_notice( tx_wp_title, line, A_BAR, tx_keys_ex );
            continue;
        }
        if ( rc ) {
            ui_notice( tx_error_title, tx_bad_disk, A_BAR, tx_keys_ex );
            continue;
        }
        ui_working( tx_ud_write );
        /* the old boot code round this disk's BPB */
        if ( plat_vol_read( drive, 0, 1, disk_boot ) ) {
            ui_notice( tx_error_title, tx_bad_disk, A_BAR, tx_keys_ex );
            continue;
        }
        old_boot( boot, disk_boot );
        rc = plat_vol_write( drive, 0, 1, boot );
        /* the system files first, so they are the first two entries and
           IO.SYS starts the data area */
        for ( i = 0; i < 3; i++ ) {
            const char *name = i < 2 ? tgt.sysfile[i] : "COMMAND.COM";
            sfmt( from, sizeof( from ), "%c:\\%s", 'A' + tgt.drive, name );
            sfmt( path, sizeof( path ), "%c:\\%s", 'A' + drive, name );
            rc |= file_copy( from, path );
            if ( i < 2 ) {
                plat_set_attr( path, FA_RDONLY | FA_HIDDEN | FA_SYSTEM );
            }
        }
        sfmt( path, sizeof( path ), "%c:\\UNINSTAL.EXE", 'A' + drive );
        rc |= held ? held_write( &prog, path, 0 ) : file_copy( self, path );
        dat = dat_text();
        sfmt( path, sizeof( path ), "%c:\\UNINSTAL.DAT", 'A' + drive );
        rc |= write_text( path, dat );
        xfree( dat );
        sfmt( path, sizeof( path ), "%c:\\AUTOEXEC.BAT", 'A' + drive );
        rc |= write_text( path, "@ECHO OFF\r\nUNINSTAL\r\n" );
        plat_flush();
        if ( rc ) {
            ui_notice( tx_error_title, tx_bad_room, A_BAR, tx_keys_ex );
            continue;
        }
        break;
    }
    if ( held ) {
        held_free( &prog );
    }
    xfree( boot );
    xfree( disk_boot );
    /* the Setup disk goes back in for the copying: disk_wanted asks */
    if ( src_removable && src_drive == drive ) {
        disk_forget();
    }
    return 0;
}

int undo_finish( void )
{
    plat_flush();
    return 0;
}

/* ------------------------------------------------------------------ */
/* UNINSTAL                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char product[16];
    char olddos[PATHLEN];
    char dosdir[PATHLEN];
    char *del[MAX_FILES + 8];
    int  ndel;
    char *rmdir[MADE_DIRS];             /* deepest first */
    int  nrmdir;
} UNDAT;

static int read_dat( UNDAT *un, const char *path )
{
    u16 handle, got;
    char *text, *line, *end, *val;

    mem_set( un, 0, sizeof( *un ) );
    if ( plat_open( path, 0, &handle ) ) {
        return 1;
    }
    text = (char *)xalloc( 16384 );
    if ( plat_read( handle, text, 16383, &got ) ) {
        plat_close( handle );
        return 1;
    }
    plat_close( handle );
    text[got] = 0;
    for ( line = text; *line; line = end ) {
        end = line;
        while ( *end && *end != '\n' ) {
            end++;
        }
        if ( *end ) {
            *end++ = 0;
        }
        if ( str_len( line ) && line[str_len( line ) - 1] == '\r' ) {
            line[str_len( line ) - 1] = 0;
        }
        val = str_chr( line, '=' );
        if ( !val ) {
            continue;
        }
        *val++ = 0;
        if ( !str_icmp( line, "product" ) ) {
            str_cpyn( un->product, val, sizeof( un->product ) );
        } else if ( !str_icmp( line, "olddos" ) ) {
            str_cpyn( un->olddos, val, sizeof( un->olddos ) );
        } else if ( !str_icmp( line, "dosdir" ) ) {
            str_cpyn( un->dosdir, val, sizeof( un->dosdir ) );
        } else if ( !str_icmp( line, "rmdir" ) && un->nrmdir < MADE_DIRS ) {
            un->rmdir[un->nrmdir++] = val;
        } else if ( !str_icmp( line, "del" ) && un->ndel < MAX_FILES + 8 ) {
            un->del[un->ndel++] = val;
        }
    }
    /* Setup writes the old DOS's directory as one on C: */
    un->olddos[0] = (char)ch_upper( (u8)un->olddos[0] );
    return !(un->olddos[0] == 'C' && path_valid( un->olddos ) &&
             str_len( un->olddos ) <= PATHLEN - 1 - 13 &&
             un->dosdir[0] && path_valid( un->dosdir ));
}

static int is_ours( const char *name )
{
    static const char *own[] = {
        "BOOTSECT.DOS", "UNINSTAL.DAT", "README.NOW"
    };
    int i;

    for ( i = 0; i < 3; i++ ) {
        if ( !str_icmp( name, own[i] ) ) {
            return 1;
        }
    }
    return 0;
}

/* put the old DOS back.  NULL = done, else what went wrong */
static const char *put_back( UNDAT *un )
{
    static char path[PATHLEN], into[PATHLEN];  /* returned when they fail */
    char spec[PATHLEN];
    FINDDATA fd;
    u8 *boot = (u8 *)xalloc( 512 );
    u16 handle, got;
    int i, rc;

    /* the old boot sector first: read it before anything changes */
    path_join( path, un->olddos, "BOOTSECT.DOS", sizeof( path ) );
    if ( plat_open( path, 0, &handle ) ) {
        xfree( boot );
        return path;
    }
    rc = plat_read( handle, boot, 512, &got ) || got != 512 || RD16( boot + 510 ) != 0xAA55;
    plat_close( handle );
    if ( rc ) {
        xfree( boot );
        return path;
    }
    /* PM-DOS's files go */
    for ( i = 0; i < un->ndel; i++ ) {
        plat_set_attr( un->del[i], 0 );
        plat_delete( un->del[i] );
    }
    /* the old commands come back */
    path_join( spec, un->olddos, "*.*", sizeof( spec ) );
    if ( plat_find_first( spec, FA_RDONLY | FA_HIDDEN | FA_SYSTEM | FA_ARCH, &fd ) == 0 ) {
        do {
            if ( fd.attr & FA_DIR ) {
                continue;
            }
            path_join( path, un->olddos, fd.name, sizeof( path ) );
            if ( !str_icmp( fd.name, "CONFIG.DAT" ) ) {
                sfmt( into, sizeof( into ), "%c:\\CONFIG.SYS", un->olddos[0] );
            } else if ( !str_icmp( fd.name, "AUTOEXEC.DAT" ) ) {
                sfmt( into, sizeof( into ), "%c:\\AUTOEXEC.BAT", un->olddos[0] );
            } else if ( is_ours( fd.name ) ) {
                continue;
            } else {
                path_join( into, un->dosdir, fd.name, sizeof( into ) );
            }
            plat_set_attr( into, 0 );
            plat_delete( into );
            if ( plat_rename( path, into ) ) {
                xfree( boot );
                return into;
            }
        } while ( plat_find_next( &fd ) == 0 );
    }
    /* the boot sector */
    if ( plat_vol_write( un->olddos[0] - 'A', 0, 1, boot ) ) {
        xfree( boot );
        return tx_cant_boot;
    }
    /* OLD_DOS.n itself */
    for ( i = 0; i < 3; i++ ) {
        static const char *left[] = { "BOOTSECT.DOS", "UNINSTAL.DAT", "README.NOW" };
        path_join( path, un->olddos, left[i], sizeof( path ) );
        plat_delete( path );
    }
    plat_rmdir( un->olddos );
    /* the levels of the PM-DOS directory Setup made, if they are empty */
    for ( i = 0; i < un->nrmdir; i++ ) {
        plat_rmdir( un->rmdir[i] );
    }
    plat_flush();
    xfree( boot );
    return NULL;
}

int uninstall_main( void )
{
    static const int go[] = { K_ENTER, 0 };
    UNDAT un;
    char path[PATHLEN], text[300], title[40];
    const char *self = plat_self(), *failed;
    char *slash;
    int key, row;

    str_cpyn( path, self, sizeof( path ) );
    slash = str_rchr( path, '\\' );
    if ( slash ) {
        slash[1] = 0;
    }
    str_catn( path, "UNINSTAL.DAT", sizeof( path ) );
    if ( read_dat( &un, path ) ) {
        plat_line( tx_badinf );
        return 1;
    }
    sfmt( title, sizeof( title ), tx_title_uninst, un.product );
    scr_open();
    for ( ;; ) {
        scr_desk( title );
        sfmt( text, sizeof( text ), tx_un_1, un.olddos[0] );
        row = 4 + scr_para( 4, text ) + 1;
        row += scr_bullet( row, tx_un_enter ) + 1;
        scr_bullet( row, tx_un_f3 );
        scr_status( tx_keys_ehx, NULL );
        key = key_get();
        if ( key == K_ENTER ) {
            break;
        }
        if ( key == K_F1 ) {
            ui_help( tx_h_un, tx_h_un_text );
        } else if ( key == K_F3 ) {
            leave( 1 );
        }
    }
    path_join( path, un.olddos, "BOOTSECT.DOS", sizeof( path ) );
    {
        u16 attr;
        if ( plat_get_attr( path, &attr ) ) {
            sfmt( text, sizeof( text ), tx_un_nodata, un.olddos[0], un.olddos );
            ui_notice( tx_cannot_title, text, A_BAR, tx_keys_e );
            leave( 1 );
        }
    }
    scr_desk( title );
    scr_status( "", NULL );
    ui_working( tx_un_working );
    failed = put_back( &un );
    if ( failed ) {
        sfmt( text, sizeof( text ), tx_un_failed, failed );
        ui_notice( tx_cannot_title, text, A_BAR, tx_keys_e );
        leave( 1 );
    }
    scr_desk( title );
    sfmt( text, sizeof( text ), tx_un_done, un.olddos[0] );
    ui_notice( "", text, A_NOTE, tx_keys_e );
    (void)go;
    scr_close();
    plat_restart();
    return 0;
}
