/*
 * MAIN.C - SCANDISK: the command line, the drives, the order of work
 * and the exit code.
 *
 * SCANDISK IS MS-DOS 6.22's ScanDisk for PM-DOS: the same five checks
 * in the same order, the same switches, the same SCANDISK.INI, the same
 * exit codes - and FAT32 and long file names, which that one never met.
 *
 *   SCANDISK [drive: [drive: ...] | /ALL] [/CHECKONLY | /AUTOFIX [/NOSAVE] | /CUSTOM]
 *            [/SURFACE] [/G] [/NOSUMMARY]
 *   SCANDISK /FRAGMENT [drive:][path]filename
 *   SCANDISK /UNDO [undo-drive:] [/G]
 *
 * IT IS A PROGRAM OF SCREENS, as that one is: the checks, the questions
 * and /UNDO are all on ScanDisk's own screen, whatever standard output
 * and standard input have been redirected to.  Only what is said before
 * there is a screen - the help, a command line that is wrong - and the
 * answer to /FRAGMENT are lines on standard output, and those are lines
 * in MS-DOS too.  /G, which MS-DOS has not got, is the same screens in
 * a graphics mode (GFX.C).  /MONO, which MS-DOS has, is not here: there
 * is no black and white screen.  The switch is still taken, and does
 * nothing, so that a batch file written for MS-DOS runs.
 *
 * For each drive: the media descriptor, the file allocation tables, the
 * directory structure, the file system (CHECK.C, FILESYS.C), and then,
 * if it is asked for, the surface scan (SURFACE.C).  Each repair is on
 * the disk before the next thing is looked at, so a check can be left
 * at any time; rt_longjmp back to run_drive() is how it is left.
 *
 * THE EXIT CODE, as MS-DOS has it:
 *
 *     0   no problems found
 *     1   the command line was wrong, or a drive cannot be checked
 *     2   out of memory
 *     3   left before the checks had finished
 *     4   every check finished, but a surface scan was left early
 *   254   problems found, and all of them repaired
 *   255   problems found, and not all of them repaired
 *
 * WHAT IS NOT HERE: DoubleSpace and DriveSpace.  PM-DOS has no
 * compressed volumes, so "SCANDISK volume-name" is refused and the
 * settings about them are read and ignored.
 */
#include "scandisk.h"

JMPBUF quit_jmp;

static int  drives[26], ndrives;
static int  want_all, want_frag, want_undo;
static int  undo_drive = 0;             /* /UNDO with no drive: A: */
static char frag_spec[PATH_MAX];
static int  frag_drive = -1;
static int  screen_up;
static int  locked = -1;

void say( const char *text )
{
    sys_stdout( text, str_len( text ) );
}

void leave( int code )
{
    if ( screen_up ) {
        fe_finish();
        screen_up = 0;
    }
    undo_close();
    if ( locked >= 0 ) {
        sys_reset_drive( locked );
        sys_unlock( locked, vol.fattype == 32 );
        locked = -1;
    }
    sys_exit( code );
}

void out_of_memory( void )
{
    if ( screen_up ) {
        fe_finish();
        screen_up = 0;
    }
    say( "Insufficient memory\r\n" );
    leave( 2 );
}

static void usage_error( const char *text, const char *what )
{
    say( text );
    if ( what ) {
        say( " - " );
        say( what );
    }
    say( "\r\n" );
    sys_exit( 1 );
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

static void parse_cmdline( void )
{
    static const char *const names[] = {
        "ALL", "CHECKONLY", "AUTOFIX", "NOSAVE", "CUSTOM", "SURFACE", "MONO", "NOSUMMARY",
        "FRAGMENT", "UNDO", "G"
    };
    const char *cur = sys_cmdline();
    char tok[PATH_MAX];
    int len, index, checkonly = 0, autofix = 0, custom = 0, others = 0;

    for ( index = 0; cur[index]; index++ ) {
        if ( cur[index] == '/' && cur[index + 1] == '?' ) {
            say( help_text );
            sys_exit( 0 );
        }
    }
    while ( *cur ) {
        while ( *cur == ' ' || *cur == '\t' || *cur == ',' || *cur == ';' ) {
            cur++;
        }
        if ( *cur == 0 ) {
            break;
        }
        len = 0;
        if ( *cur == '/' ) {
            tok[len++] = *cur++;
            while ( *cur && *cur != ' ' && *cur != '\t' && *cur != '/' && len < 40 ) {
                tok[len++] = *cur++;
            }
            tok[len] = 0;
            for ( index = 0; index < 11 && str_icmp( tok + 1, names[index] ) != 0; index++ ) {
            }
            switch ( index ) {
            case 0: want_all = 1;      others = 1;  break;
            case 1: checkonly = 1;     others = 1;  break;
            case 2: autofix = 1;       others = 1;  break;
            case 3: opt.nosave = 1;    others = 1;  break;
            case 4: custom = 1;        others = 1;  break;
            case 5: opt.surface = 1;   others = 1;  break;
            case 6:                                 break;  /* /MONO */
            case 7: opt.nosummary = 1; others = 1;  break;
            case 8: want_frag = 1;                  break;
            case 9: want_undo = 1;                  break;
            case 10: opt.graphics = 1;              break;
            default:
                usage_error( "Invalid switch", tok );
            }
            continue;
        }
        if ( *cur == '"' ) {
            cur++;
            while ( *cur && *cur != '"' && len < PATH_MAX - 1 ) {
                tok[len++] = *cur++;
            }
            if ( *cur == '"' ) {
                cur++;
            }
        } else {
            while ( *cur && *cur != ' ' && *cur != '\t' && *cur != '/' && len < PATH_MAX - 1 ) {
                tok[len++] = *cur++;
            }
        }
        tok[len] = 0;
        if ( want_frag ) {
            /* [drive:][path]filename */
            if ( frag_spec[0] || frag_drive >= 0 ) {
                usage_error( "Too many parameters", tok );
            }
            if ( tok[0] && tok[1] == ':' ) {
                if ( ch_upper( tok[0] ) < 'A' || ch_upper( tok[0] ) > 'Z' ) {
                    usage_error( "Invalid drive specification", NULL );
                }
                frag_drive = ch_upper( tok[0] ) - 'A';
                str_cpy( frag_spec, tok + 2 );
            } else {
                frag_drive = sys_get_drive();
                str_cpy( frag_spec, tok );
            }
            continue;
        }
        if ( len == 2 && tok[1] == ':' && ch_upper( tok[0] ) >= 'A' && ch_upper( tok[0] ) <= 'Z' ) {
            if ( ndrives == 26 ) {
                usage_error( "Too many parameters", tok );
            }
            drives[ndrives++] = ch_upper( tok[0] ) - 'A';
            continue;
        }
        if ( len == 2 && tok[1] == ':' ) {
            usage_error( "Invalid drive specification", NULL );
        }
        /* MS-DOS takes the name of a compressed volume file here */
        usage_error( "ScanDisk checks drives.  PM-DOS has no compressed volume files", tok );
    }

    if ( want_frag ) {
        if ( want_undo || others || ndrives ) {
            usage_error( "/FRAGMENT takes a file name and nothing else", NULL );
        }
        if ( frag_spec[0] == 0 ) {
            usage_error( "Required parameter missing", NULL );
        }
        return;
    }
    if ( want_undo ) {
        if ( others || ndrives > 1 ) {
            usage_error( "/UNDO takes the Undo disk's drive and nothing else", NULL );
        }
        if ( ndrives ) {
            undo_drive = drives[0];
        }
        return;
    }
    if ( checkonly + autofix + custom > 1 ) {
        usage_error( "/CHECKONLY, /AUTOFIX and /CUSTOM cannot be used together", NULL );
    }
    if ( opt.nosave && !autofix ) {
        usage_error( "/NOSAVE needs /AUTOFIX", NULL );
    }
    if ( want_all && ndrives ) {
        usage_error( "/ALL cannot be used with a drive letter", NULL );
    }
    opt.mode = checkonly ? MODE_CHECKONLY : autofix ? MODE_AUTOFIX : custom ? MODE_CUSTOM : MODE_ASK;
}

static void pick_drives( void )
{
    char name[4];
    int drive, index, type;

    name[1] = ':';
    name[2] = 0;
    if ( want_all ) {
        for ( drive = 2; drive < 26; drive++ ) {
            if ( sys_drive_fixed( drive ) ) {
                drives[ndrives++] = drive;
            }
        }
        if ( ndrives == 0 ) {
            usage_error( "There are no hard disk drives to check", NULL );
        }
        return;
    }
    if ( ndrives == 0 ) {
        drives[ndrives++] = sys_get_drive();
    }
    for ( index = 0; index < ndrives; index++ ) {
        name[0] = (char)('A' + drives[index]);
        type = sys_drive_type( drives[index] );
        if ( type == DRV_INVALID ) {
            usage_error( "Invalid drive specification", name );
        }
        if ( type == DRV_REMOTE ) {
            usage_error( "ScanDisk cannot check a network drive or a CD-ROM", name );
        }
        if ( type == DRV_EXFAT ) {
            usage_error( "ScanDisk does not check exFAT drives", name );
        }
        if ( type == DRV_SUBST || (sys_truename_drive( drives[index] ) >= 0 &&
                                   sys_truename_drive( drives[index] ) != drives[index]) ) {
            usage_error( "ScanDisk cannot check a SUBSTed drive", name );
        }
    }
}

/* ------------------------------------------------------------------ */
/* one drive                                                           */
/* ------------------------------------------------------------------ */

static int surface_wanted( void )
{
    if ( opt.surface ) {
        return 1;
    }
    switch ( opt.mode ) {
    case MODE_ASK:
        return fe_ask_surface();
    case MODE_CUSTOM:
        if ( opt.cust_surface == SURF_ALWAYS ) {
            return 1;
        }
        if ( opt.cust_surface == SURF_PROMPT ) {
            return fe_ask_surface();
        }
        break;
    }
    return 0;
}

/* a drive that could not even be started on */
static int no_drive( const char *fmt, int drive )
{
    char line[120];

    sfmt( line, sizeof( line ), fmt, 'A' + drive );
    st.found = 1;
    log_line( "" );
    log_line( "%s", line );
    fe_message( line );
    return Q_DRIVE;
}

/* 0 = the checks ran to their end, or Q_* for why they did not */
static int run_drive( int drive )
{
    char date[16], time[16];
    u16 dos_date, dos_time;
    int why;

    check_reset();
    if ( log_text.count ) {
        log_line( "" );
        log_line( "----------------------------------------------------------------" );
        log_line( "" );
    }
    sys_get_datetime( &dos_date, &dos_time );
    log_line( "\1ScanDisk log for drive %c:   %s  %s", 'A' + drive, fmt_date( dos_date, date ),
              fmt_time( dos_time, time ) );
    sys_reset_drive( drive );
    undo_avoid( drive );
    if ( vol_open( drive ) != 0 ) {
        vol.drive = drive;
        return no_drive( "Drive %c: cannot be read, or is not formatted for DOS.", drive );
    }
    if ( sys_lock( drive, vol.fattype == 32 ) != 0 ) {
        return no_drive( "Drive %c: is in use and cannot be locked.", drive );
    }
    locked = drive;

    why = rt_setjmp( &quit_jmp );
    if ( why == 0 ) {
        fe_drive_begin();
        fe_phase( PH_MEDIA );
        check_media();
        fe_phase( PH_FAT );
        check_fats();
        fe_phase( PH_DIRS );
        check_dirs();
        fe_phase( PH_FILESYS );
        check_filesys();
        st.logical_done = 1;
        if ( surface_wanted() ) {
            fe_phase( PH_SURFACE );
            surface_scan();
        }
        fe_phase( PH_COUNT );
    }

    /* however it ended: the kernel reads the volume afresh, and is told
       when it can be trusted again */
    fat_forget();
    sys_reset_drive( drive );
    if ( st.logical_done && st.found == st.fixed && !st.ftrunc ) {
        sys_mark_checked( drive );
    }
    sys_unlock( drive, vol.fattype == 32 );
    locked = -1;
    undo_drive_done();
    return why;
}

static int summary_wanted( int setting, u32 found )
{
    if ( opt.nosummary ) {
        return 0;
    }
    if ( opt.mode != MODE_CUSTOM || setting == TRI_ON ) {
        return 1;
    }
    return setting == TRI_AUTO && found != 0;
}

static void finish_log( void )
{
    TEXT text;
    int index;

    tx_init( &text );
    summary_text( &text );
    log_line( "" );
    for ( index = 0; index < text.count; index++ ) {
        tx_add( &log_text, text.line[index] );
    }
    tx_free( &text );
}

static void check_drives( void )
{
    u32 found = 0, fixed = 0;
    int index, why = 0, left_logical = 0, left_surface = 0, stopped = 0, done = 0, more;
    char where[24];

    fe_start();
    screen_up = 1;
    for ( index = 0; index < ndrives && !stopped; index++ ) {
        why = run_drive( drives[index] );
        done++;
        finish_log();
        found += st.found;
        fixed += st.fixed;
        if ( why == Q_NOMEM ) {
            out_of_memory();
        }
        if ( why == Q_USER ) {
            if ( st.logical_done ) {
                left_surface = 1;
            } else {
                left_logical = 1;
            }
            stopped = 1;
        } else if ( why == Q_CUSTOM ) {
            stopped = 1;
        }
        /* a drive left in the middle of its checks has no summary; one
           whose surface scan was cut short has, as it has in MS-DOS */
        if ( why != Q_USER || st.logical_done ) {
            more = !stopped && (index + 1 < ndrives ||
                                (done > 1 && summary_wanted( opt.all_summary, found )));
            if ( summary_wanted( opt.drive_summary, st.found ) ) {
                fe_drive_end( more );
            }
#ifdef HOSTBUILD
            else {
                say( "\r\n" );
                say( drive_verdict() );
                say( "\r\n" );
            }
#endif
        }
        vol_close();
    }
    if ( done > 1 && !stopped && summary_wanted( opt.all_summary, found ) ) {
        fe_all_end( done, found, fixed );
    }
    fe_finish();
    screen_up = 0;
    undo_close();

    if ( opt.mode == MODE_CUSTOM && opt.save_log != LOG_OFF ) {
        if ( log_save( opt.save_log == LOG_APPEND, where ) != 0 ) {
            say( "The log could not be saved as " );
            say( where );
            say( "\r\n" );
        }
    }
    if ( left_logical ) {
        sys_exit( 3 );
    }
    if ( left_surface ) {
        sys_exit( 4 );
    }
    sys_exit( found == 0 ? 0 : found == fixed ? 254 : 255 );
}

void scandisk_main( void )
{
    int why, code, cmd_graphics;

    if ( sys_init() != 0 ) {
        sys_exit( 2 );
    }
    country.date_fmt = 0;
    country.date_sep = '-';
    country.time_sep = ':';
    country.thou_sep = ',';
    country.time_24 = 0;
    sys_get_country( &country );
    fmt_group_set( country.thou_sep );
    tx_init( &log_text );
    ini_defaults();
    parse_cmdline();
    sys_break_install();

    /* lines, as it is in MS-DOS */
    if ( want_frag ) {
        why = rt_setjmp( &quit_jmp );
        if ( why != 0 ) {
            say( why == Q_USER ? "\r\n" : "The drive cannot be read.\r\n" );
            sys_exit( why == Q_USER ? 3 : 255 );
        }
        sys_exit( frag_report( frag_drive, frag_spec ) );
    }

    cmd_graphics = opt.graphics;
    ini_load();
    opt.graphics |= cmd_graphics;       /* /G, or Display = Graphics */

    if ( want_undo ) {
        fe_start();
        screen_up = 1;
        why = rt_setjmp( &quit_jmp );
        if ( why == Q_NOMEM ) {
            out_of_memory();
        }
        code = why == 0 ? undo_restore( undo_drive ) : why == Q_USER ? 3 : 255;
        leave( code );
    }

    pick_drives();
    check_drives();
}
