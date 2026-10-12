/*
 * MAIN.C - DEFRAG: the command line, the order of work and the exit
 * code.
 *
 * DEFRAG IS MS-DOS 6.22's DEFRAG FOR PM-DOS: the same screen, the same
 * menu, the same two methods, the same switches and exit codes - and
 * FAT32 and long file names, which that one never met.
 *
 *   DEFRAG [drive:] [/F] [/S[:]order] [/H] [/B] [/G]
 *   DEFRAG [drive:] [/U] [/H] [/B] [/G]
 *
 * WITH /F OR /U it reads the drive, optimizes it and ends, asking
 * nothing, as MS-DOS's does - a batch file reads the exit code.
 * Without them it reads the drive, says how fragmented it is and what
 * it suggests, and waits at its menu.
 *
 * A DRIVE WITH ERRORS IS NOT OPTIMIZED.  Reading a drive is also a
 * check of it (TREE.C).  MS-DOS's DEFRAG stops at the first thing wrong
 * and says to run ScanDisk; this one offers to run it, and when
 * ScanDisk comes back reads the drive again.  That is asked even with
 * /F or /U: it is not a question a default can answer.
 *
 * WHILE A DRIVE IS LOADED its current directory is the root.  The
 * kernel knows a current directory by its first cluster as well as by
 * its name, and that cluster is about to be somebody else's; the root
 * never moves.  The directory is set again, by name, when the drive is
 * let go - and by then the kernel has been told to forget everything it
 * had read from the drive (sys_reset_drive).
 *
 * A FILE OPEN ON THE DRIVE would be as wrong afterwards as the current
 * directory, and nothing can put that right, so DEFRAG does not run
 * with its own input or output redirected to a file.  (The kernel does
 * not say which drive a handle's file is on.)  The test mode's key file
 * and log are the exception, and are kept on another drive.
 *
 * THE EXIT CODE, as MS-DOS has it:
 *
 *     0   the drive was optimized
 *     1   something that should not be possible
 *     2   the drive has no free cluster to work with
 *     3   stopped at the keyboard
 *     4   the command line was wrong, or the drive cannot be used
 *     5   a cluster could not be read
 *     6   a cluster could not be written
 *     7   the file system has errors (run ScanDisk)
 *     9   not enough memory for this drive
 *
 * /SKIPHIGH, /LCD, /BW and /G0 are MS-DOS's and mean nothing here: they
 * are taken and do nothing, so that a batch file written for MS-DOS
 * runs.  There is no black and white screen.  /G is this program's own:
 * the same screen in a graphics mode (GFX.C).
 */
#include "defrag.h"
#include "text.h"

OPTIONS opt;
COUNTRY country;

static int  screen_up;
static int  loaded = -1;        /* the drive that is read, and locked */
static char old_cwd[PATH_MAX];
static int  cwd_kept;
static int  exit_code;
static int  optimized;          /* a run has finished: /B has a reason */
static int  method_set;         /* by a switch or in its box: what DEFRAG
                                   suggests does not change it */

enum { ST_PICK, ST_READ, ST_MENU, ST_RUN };

void say( const char *text )
{
    sys_stdout( text, str_len( text ) );
}

static void release_drive( void )
{
    if ( loaded < 0 ) {
        return;
    }
    fat_commit();
    sys_reset_drive( loaded );
    if ( cwd_kept ) {
        sys_chdir( loaded, old_cwd );
        cwd_kept = 0;
    }
    sys_unlock( loaded, vol.fattype == 32 );
    tree_free();
    vol_close();
    loaded = -1;
    if ( screen_up ) {
        fe_drive( 0 );
    }
}

void leave( int code )
{
    release_drive();
    if ( screen_up ) {
        fe_finish();
        screen_up = 0;
    }
    if ( opt.restart && optimized ) {
        sys_restart();
        say( tx_no_restart );
        say( "\r\n" );
    }
    sys_exit( code );
}

void out_of_memory( void )
{
    if ( screen_up ) {
        fe_finish();
        screen_up = 0;
    }
    say( tx_no_memory );
    say( "\r\n" );
    opt.restart = 0;
    leave( DFE_NOMEM );
}

static void usage_error( const char *text, const char *what )
{
    say( text );
    if ( what ) {
        say( " - " );
        say( what );
    }
    say( "\r\n" );
    sys_exit( DFE_GENERAL );
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

/* /S, /S:N, /SN-, ...: what follows the S */
static void sort_switch( const char *tok, const char *whole )
{
    static const char letters[] = "NEDS";
    const char *found;

    opt.sort = SORT_NAME;
    opt.descending = 0;
    if ( *tok == ':' ) {
        tok++;
    }
    if ( *tok ) {
        found = str_chr( letters, ch_upper( *tok ) );
        if ( found == NULL ) {
            usage_error( tx_bad_order, whole );
        }
        opt.sort = SORT_NAME + (int)(found - letters);
        tok++;
        if ( *tok == '-' ) {
            opt.descending = 1;
            tok++;
        }
        if ( *tok ) {
            usage_error( tx_bad_order, whole );
        }
    }
}

static void parse_cmdline( void )
{
    const char *cur = sys_cmdline();
    char tok[48];
    int len, index, full = 0, unfrag = 0;

    for ( index = 0; cur[index]; index++ ) {
        if ( cur[index] == '/' && cur[index + 1] == '?' ) {
            say( help_text );
            sys_exit( 0 );
        }
    }
    opt.drive = -1;
    opt.method = METHOD_FULL;
    while ( *cur ) {
        while ( *cur == ' ' || *cur == '\t' || *cur == ',' || *cur == ';' ) {
            cur++;
        }
        if ( *cur == 0 ) {
            break;
        }
        len = 0;
        tok[len++] = *cur++;
        while ( *cur && *cur != ' ' && *cur != '\t' && *cur != '/' && len < 40 ) {
            tok[len++] = *cur++;
        }
        tok[len] = 0;
        if ( tok[0] != '/' ) {
            if ( len != 2 || tok[1] != ':' || ch_upper( tok[0] ) < 'A' || ch_upper( tok[0] ) > 'Z' ) {
                usage_error( tx_too_many, tok );
            }
            if ( opt.drive >= 0 ) {
                usage_error( tx_too_many, tok );
            }
            opt.drive = ch_upper( tok[0] ) - 'A';
            continue;
        }
        if ( str_icmp( tok + 1, "F" ) == 0 ) {
            full = 1;
        } else if ( str_icmp( tok + 1, "U" ) == 0 ) {
            unfrag = 1;
        } else if ( str_icmp( tok + 1, "H" ) == 0 ) {
            opt.hidden = 1;
        } else if ( str_icmp( tok + 1, "B" ) == 0 ) {
            opt.restart = 1;
        } else if ( str_icmp( tok + 1, "G" ) == 0 ) {
            opt.graphics = 1;
        } else if ( str_icmp( tok + 1, "SKIPHIGH" ) == 0 || str_icmp( tok + 1, "LCD" ) == 0
                    || str_icmp( tok + 1, "BW" ) == 0 || str_icmp( tok + 1, "G0" ) == 0 ) {
            /* MS-DOS's: nothing they could mean here */
        } else if ( ch_upper( tok[1] ) == 'S' ) {
            sort_switch( tok + 2, tok );
        } else {
            usage_error( tx_bad_switch, tok );
        }
    }
    if ( full && unfrag ) {
        usage_error( tx_both_methods, NULL );
    }
    if ( full || unfrag ) {
        opt.method = unfrag ? METHOD_UNFRAG : METHOD_FULL;
        opt.unattended = 1;
        method_set = 1;
    }
}

/* ------------------------------------------------------------------ */
/* a drive                                                             */
/* ------------------------------------------------------------------ */

static void __cdecl tell( const char *fmt, ... )
{
    char line[200];

    vfmt( line, sizeof( line ), fmt, VA_START( fmt ) );
    fe_message( line, 1 );
}

/* Read a drive and make it the one being worked on.  0 = it is loaded
   (dst.errors says whether it is sound), or the DFE_* that was said. */
static int load_drive( int drive )
{
    int rc;

    release_drive();
    if ( sys_drive_kind( drive ) == DK_NONE ) {
        tell( tx_not_local, 'A' + drive );
        return DFE_GENERAL;
    }
    if ( fe_log_drive() == drive ) {
        tell( tx_test_same_drive, 'A' + drive );
        return DFE_GENERAL;
    }
    sys_reset_drive( drive );
    if ( vol_open( drive ) != 0 ) {
        tell( tx_cannot_read, 'A' + drive );
        return DFE_GENERAL;
    }
    if ( sys_lock( drive, vol.fattype == 32 ) != 0 ) {
        tell( tx_in_use, 'A' + drive );
        return DFE_GENERAL;
    }
    loaded = drive;
    cwd_kept = sys_get_cwd( drive, old_cwd, sizeof( old_cwd ) ) == 0;
    sys_chdir( drive, "" );

    fe_reading( NULL );
    rc = fat_load() != 0 ? DFE_READ : tree_read();
    if ( rc == DFE_NOMEM ) {
        out_of_memory();
    }
    if ( rc != 0 ) {
        release_drive();
        if ( rc == DFE_READ ) {
            tell( tx_cannot_read, 'A' + drive );
        }
        return rc;
    }
    fe_drive( 1 );
    return 0;
}

/* what is wrong with the drive, and what can be done about it */
static void fs_text( char *buf, u32 max, int drive, int again )
{
    int bit, left = dst.errors;

    sfmt( buf, max, again ? tx_fs_again : tx_fs_found, 'A' + drive );
    for ( bit = 0; bit < 6; bit++ ) {
        if ( dst.errors & (1 << bit) ) {
            left &= ~(1 << bit);
            str_catn( buf, " ", max );
            str_catn( buf, tx_fs_error[bit], max );
            str_catn( buf, left ? ";" : ".", max );
        }
    }
    str_catn( buf, "\n\n", max );
    str_catn( buf, tx_fs_offer, max );
}

/* SCANDISK.EXE, where the shell would find it: here, along PATH, in
   the root of the boot drive.  A native program is not told where it
   was loaded from. */
static int find_scandisk( char *path, u32 max )
{
    static const char name[] = "SCANDISK.EXE";
    const char *list = sys_getenv( "PATH" );
    char dir[PATH_MAX];
    u32 len;

    if ( sys_exists( name ) ) {
        str_cpyn( path, name, max );
        return 1;
    }
    while ( list && *list ) {
        for ( len = 0; list[len] && list[len] != ';'; len++ ) {
        }
        if ( len && len < sizeof( dir ) - 16 ) {
            mem_cpy( dir, list, len );
            if ( dir[len - 1] != '\\' && dir[len - 1] != ':' ) {
                dir[len++] = '\\';
            }
            str_cpy( dir + len, name );
            if ( sys_exists( dir ) ) {
                str_cpyn( path, dir, max );
                return 1;
            }
        }
        list += len;
        if ( *list == ';' ) {
            list++;
        }
    }
    sfmt( path, max, "%c:\\%s", 'A' + sys_boot_drive(), name );
    return sys_exists( path );
}

/* 1 = ScanDisk ran, whatever it found */
static int run_scandisk( int drive )
{
    char path[PATH_MAX], tail[16];
    int code, error = 0;

    if ( !find_scandisk( path, sizeof( path ) ) ) {
        fe_message( tx_no_scandisk, 1 );
        return 0;
    }
    sfmt( tail, sizeof( tail ), " %c:%s", 'A' + drive, opt.graphics ? " /G" : "" );
    fe_suspend();
    code = sys_exec( path, tail, &error );
    fe_resume();
    if ( code < 0 ) {
        tell( tx_scandisk_failed, path, (u32)error );
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* the order of work                                                   */
/* ------------------------------------------------------------------ */

static void session( void )
{
    char text[400];
    int state = opt.drive >= 0 ? ST_READ : ST_PICK, drive = opt.drive, again = 0;
    int rc, pick, suggest;

    for ( ;; ) {
        switch ( state ) {
        case ST_PICK:
            pick = fe_pick_drive( drive >= 0 ? drive : sys_get_drive() );
            if ( pick < 0 ) {
                state = ST_MENU;
                break;
            }
            drive = pick;
            again = 0;
            state = ST_READ;
            break;

        case ST_READ:
            rc = load_drive( drive );
            if ( rc != 0 ) {
                if ( opt.unattended ) {
                    leave( rc );
                }
                state = ST_MENU;
                break;
            }
            if ( dst.errors ) {
                fs_text( text, sizeof( text ), drive, again );
                if ( !fe_fs_error( text ) ) {
                    leave( DFE_ALLOC );
                }
                release_drive();
                if ( !run_scandisk( drive ) ) {
                    leave( DFE_ALLOC );
                }
                again = 1;
                break;                      /* and read it again */
            }
            again = 0;
            if ( opt.unattended ) {
                state = ST_RUN;
                break;
            }
            suggest = tree_recommend();
            if ( !method_set && suggest != METHOD_NONE ) {
                opt.method = suggest;
            }
            state = fe_recommend( tree_percent(), suggest ) ? ST_RUN : ST_MENU;
            break;

        case ST_MENU:
            switch ( fe_menu() ) {
            case GO_BEGIN:
                state = loaded >= 0 ? ST_RUN : ST_PICK;
                break;
            case GO_DRIVE:
                state = ST_PICK;
                break;
            case GO_METHOD:
                if ( fe_pick_method() ) {
                    method_set = 1;
                }
                break;
            case GO_SORT:
                fe_pick_sort();
                break;
            case GO_LEGEND:
                fe_legend();
                break;
            case GO_ABOUT:
                fe_about();
                break;
            case GO_EXIT:
                leave( exit_code );
            }
            break;

        case ST_RUN:
            fe_run( 1 );
            rc = optimize();
            fe_run( 0 );
            if ( rc == DFE_OK ) {
                optimized = 1;
                exit_code = 0;
                fe_finished();
                if ( opt.unattended ) {
                    leave( 0 );
                }
                switch ( fe_done() ) {
                case DONE_ANOTHER:      state = ST_PICK;    break;
                case DONE_CONFIGURE:    state = ST_MENU;    break;
                default:                leave( 0 );
                }
                break;
            }
            if ( rc == DFE_USER ) {
                exit_code = DFE_USER;
                if ( opt.unattended ) {
                    leave( DFE_USER );
                }
                state = ST_MENU;
                break;
            }
            /* a disk that is failing is not one to carry on with */
            if ( rc == DFE_NOFREE ) {
                tell( tx_no_free, 'A' + drive );
            } else if ( rc == DFE_READ ) {
                tell( tx_read_error, move_error_clus, 'A' + drive );
            } else if ( rc == DFE_WRITE && move_error_clus < 2 ) {
                tell( tx_system_error, 'A' + drive );
            } else if ( rc == DFE_WRITE ) {
                tell( tx_write_error, move_error_clus, 'A' + drive );
            } else {
                tell( tx_internal, 'A' + drive );
            }
            opt.restart = 0;
            leave( rc );
        }
    }
}

void defrag_main( void )
{
    char line[120];

    if ( sys_init() != 0 ) {
        sys_exit( DFE_MEMORY );
    }
    country.date_fmt = 0;
    country.date_sep = '-';
    country.time_sep = ':';
    country.thou_sep = ',';
    country.time_24 = 0;
    sys_get_country( &country );
    fmt_group_set( country.thou_sep );
    parse_cmdline();
    sys_break_install();

#ifndef HOSTBUILD
    if ( sys_getenv( "DFTEST" ) == NULL && (sys_handle_is_file( 0 ) || sys_handle_is_file( 1 )) ) {
        say( tx_redirected );
        say( "\r\n" );
        sys_exit( DFE_GENERAL );
    }
#endif
    if ( opt.drive >= 0 && sys_drive_kind( opt.drive ) == DK_NONE ) {
        sfmt( line, sizeof( line ), tx_bad_drive, 'A' + opt.drive );
        say( line );
        say( "\r\n" );
        sys_exit( DFE_GENERAL );
    }
    fe_start();
    screen_up = 1;
    session();
}
