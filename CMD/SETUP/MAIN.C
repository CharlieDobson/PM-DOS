/*
 * MAIN.C - Setup, from the first screen to the restart.
 *
 * THE ORDER IS 6.22 SETUP'S: the welcome; the hard disk made ready if
 * it needs to be (the unallocated space set up and the machine
 * restarted, or the drive formatted); the system settings; the
 * directory; the copying; "remove the disks"; "Setup complete" and a
 * restart.  An upgrade of another DOS adds what 6.x's Upgrade added -
 * a last question before anything changes, and the Uninstall disk.
 *
 * WHAT IS PM-DOS'S OWN:
 *   - the 32-bit half, which is the one a machine started from the
 *     Setup disk runs, can leave Setup for the prompt to change the
 *     partitions with FDISK whenever the disk has partitions already,
 *     and it installs on a FAT drive with nothing on it;
 *   - a PM-DOS already on the disk is reinstalled over;
 *   - the system files go in the root and everything else in the DOS
 *     directory, with SHELL= saying where (see CFG.C).
 *
 * WHAT IS LEFT OUT: 6.22's F5 and /B (no monochrome screens on PM-DOS;
 * /B is still taken, so old batch files run), /E (there are no Windows
 * programs to set up), DriveSpace.
 */
#include "setup.h"
#include "text.h"

CHOICES opt;
static char title[64];
static int  made_partition;             /* Setup made it this run, or the */
                                        /* run before the restart         */

const char *setup_title( void )
{
    return title;
}

void leave( int code )
{
    scr_close();
    plat_exit( code );
}

void fatal( const char *text )
{
    ui_notice( tx_cannot_title, text, A_BAR, tx_keys_e );
    leave( 1 );
}

/* a line before the screen opens, and out */
static void say_and_go( const char *text )
{
    plat_line( text );
    plat_exit( 1 );
}

/* the box 6.22 called Setup Cannot Continue, and out */
static void cannot_text( const char *text )
{
    static const int keys[] = { K_ENTER, 0 };
    DLG dlg;

    dlg_start( &dlg, tx_cannot_title, 52 );
    dlg_blank( &dlg );
    dlg_para( &dlg, text );
    dlg_blank( &dlg );
    dlg_bullet( &dlg, tx_leave_enter );
    dlg_blank( &dlg );
    dlg_run( &dlg, A_BAR, tx_keys_e, keys, 0 );
    leave( 1 );
}

/* the same, the text naming the drive (%c) */
static void cannot( const char *fmt )
{
    char text[300];

    sfmt( text, sizeof( text ), fmt, 'A' + (tgt.drive >= 0 ? tgt.drive : 2) );
    cannot_text( text );
}

static void restart( void )
{
    if ( plat_getenv( "SETUPNOBOOT" ) ) {
        leave( 98 );                    /* the test rigs restart it */
    }
    scr_close();
    plat_flush();
    plat_restart();
}

/* ------------------------------------------------------------------ */
/* the first screen                                                    */
/* ------------------------------------------------------------------ */

static int floppy_confirm( void )
{
    static const int keys[] = { K_ENTER, K_ESC, 0 };
    DLG dlg;
    int key;

    for ( ;; ) {
        dlg_start( &dlg, tx_fl_confirm_title, 52 );
        dlg_blank( &dlg );
        dlg_para( &dlg, tx_fl_confirm );
        dlg_blank( &dlg );
        dlg_bullet( &dlg, tx_fl_confirm_enter );
        dlg_blank( &dlg );
        dlg_bullet( &dlg, tx_fl_confirm_esc );
        dlg_blank( &dlg );
        key = dlg_run( &dlg, A_BAR, tx_keys_ehxp, keys, 1 );
        if ( key == K_F1 ) {
            ui_help( tx_h_flconfirm, tx_h_flconfirm_text );
            continue;
        }
        return key == K_ENTER;
    }
}

/* 1 = the hard disk, 2 = a floppy (F7) */
static int welcome( void )
{
    char line[100];
    int row, key;

    for ( ;; ) {
        scr_desk( title );
        scr_para( 4, tx_wel_1 );
        sfmt( line, sizeof( line ), tx_wel_2, inf.version );
        row = 6 + scr_para( 6, line ) + 1;
        row += scr_bullet( row, tx_wel_enter ) + 1;
        row += scr_bullet( row, tx_wel_f1 ) + 1;
        row += scr_bullet( row, tx_wel_f3 ) + 1;
        row += scr_note( row, tx_note, tx_wel_note ) + 1;
        scr_para( row, tx_wel_last );
        scr_status( tx_keys_welcome, NULL );
        key = key_get();
        switch ( key ) {
        case K_ENTER:
            return 1;
        case K_F1:
            ui_help( tx_h_using, tx_h_using_text );
            break;
        case K_F3:
            ui_exit_box();
            break;
        case K_F7:
            if ( floppy_confirm() ) {
                return 2;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the pages that are a paragraph or two and a short list              */
/* ------------------------------------------------------------------ */

/* The text from row 4, the list one row below it, the two lines about
   the keys one row below that.  -> the item chosen */
static int choice_page( const char *para1, const char *para2,
                        const char **items, int count,
                        const char *keys, const char *help_head,
                        const char *help_text )
{
    LIST list;
    int row;

    scr_desk( title );
    row = 4 + scr_para( 4, para1 ) + 1;
    if ( para2 ) {
        row += scr_para( row, para2 ) + 1;
    }
    mem_set( &list, 0, sizeof( list ) );
    list.top = row;
    list.rows = count;
    list.items = items;
    list.count = count;
    list.sel = 0;
    list.first = 0;
    list.flags = LS_NOESC;
    list.help_head = help_head;
    list.help_text = help_text;
    row += count + 3;
    scr_para( row, tx_list_accept );
    scr_para( row + 1, tx_list_change );
    scr_status( keys, NULL );
    return ui_list( &list );
}

/* the restart after the partition is written */
static void restart_box( void )
{
    static const int keys[] = { K_ENTER, 0 };
    DLG dlg;

    dlg_start( &dlg, "", 44 );
    dlg_blank( &dlg );
    dlg_center( &dlg, tx_restart_1 );
    dlg_center( &dlg, src_removable ? tx_restart_2 : tx_restart_2x );
    dlg_blank( &dlg );
    dlg_bullet( &dlg, tx_restart_go );
    dlg_blank( &dlg );
    dlg_run( &dlg, A_NOTE, tx_keys_e, keys, 0 );
    restart();
}

static void format_progress( u32 done, u32 total )
{
    static int last = -1;
    char line1[40], line2[40];
    int pct = total ? (int)(done / (total / 100 + 1)) : 0;
    DLG dlg;

    if ( done >= total ) {
        pct = 100;
    }
    if ( pct == last ) {
        return;
    }
    last = pct;
    sfmt( line1, sizeof( line1 ), tx_fmt_drive, 'A' + tgt.drive );
    sfmt( line2, sizeof( line2 ), tx_fmt_pct, pct );
    /* one length, so the two lines start in one column */
    while ( str_len( line1 ) < str_len( line2 ) ) {
        str_catn( line1, " ", sizeof( line1 ) );
    }
    dlg_start( &dlg, "", 30 );
    dlg_blank( &dlg );
    dlg_center( &dlg, line1 );
    dlg_center( &dlg, line2 );
    dlg_blank( &dlg );
    dlg_draw( &dlg, A_NOTE );
    scr_flush();
}

static void format_drive( void )
{
    scr_desk( title );
    scr_para( 4, tx_fmt_head );
    scr_para( 6, tx_fmt_text );
    scr_status( "", NULL );
    format_progress( 0, 1 );
    if ( disk_format( format_progress ) ) {
        cannot( tx_cant_format );
    }
}

/* "1,234 KB", or "1,234 MB" from ten megabytes up */
static void size_text( char *buf, int max, u32 kb )
{
    if ( kb >= 10240ul ) {
        sfmt( buf, max, "%,lu MB", kb / 1024 );
    } else {
        sfmt( buf, max, "%,lu KB", kb );
    }
}

/* Get drive C: ready, or decide it is.  Comes back when the settings
   are next; restarts or leaves otherwise. */
static void prepare_disk( void )
{
    const char *items[2];
    char item0[70], para[300];
    int full_32 = (plat_caps & PC_PARTITION) != 0;

    ui_working( tx_checking );
    disk_examine();

    switch ( tgt.state ) {
    case TG_NODISK:
        cannot( full_32 ? tx_cant_nodisk : tx_cant_nodos );
        break;
    case TG_EMPTY:
    case TG_FREE:
        if ( !full_32 ) {
            cannot( tx_cant_nodos );
        }
        items[0] = tx_unal_item;
        items[1] = tgt.state == TG_EMPTY ? tx_item_exit : tx_item_fdisk;
        if ( choice_page( tx_unal_1, tx_unal_2, items, 2, tx_keys_ehx,
                          tx_h_unal, tx_h_unal_text ) != 0 ) {
            leave( 2 );
        }
        if ( disk_partition() ) {
            cannot( tx_cant_part );
        }
        restart_box();
        break;
    case TG_FULL:
        cannot( full_32 ? tx_cant_full : tx_cant_nodos );
        break;
    case TG_RAW:
        if ( !full_32 ) {
            cannot( tx_cant_nodos );
        }
        made_partition = disk_marked();
        if ( !made_partition ) {
            sfmt( para, sizeof( para ), tx_raw_1, 'A' + tgt.drive );
            sfmt( item0, sizeof( item0 ), tx_raw_item, 'A' + tgt.drive );
            items[0] = item0;
            items[1] = tx_item_exit;
            if ( choice_page( para, NULL, items, 2, tx_keys_ehx,
                              tx_h_raw, tx_h_raw_text ) != 0 ) {
                leave( 2 );
            }
        }
        format_drive();
        break;
    case TG_BLANK:
        if ( full_32 ) {
            char free_text[24];
            size_text( free_text, sizeof( free_text ), tgt.free_kb );
            sfmt( para, sizeof( para ), tx_inst_1, 'A' + tgt.drive, free_text );
            sfmt( item0, sizeof( item0 ), tx_inst_item, 'A' + tgt.drive );
            items[0] = item0;
            items[1] = tx_item_fdisk;
            if ( choice_page( para, tx_inst_2, items, 2, tx_keys_ehx,
                              tx_h_inst, tx_h_inst_text ) != 0 ) {
                leave( 2 );
            }
        }
        break;
    case TG_MSDOS: {
        char para2[300];
        sfmt( para, sizeof( para ), tx_upg_1, tgt.sysname, 'A' + tgt.drive );
        sfmt( para2, sizeof( para2 ), opt.no_uninstall ? tx_upg_2g : tx_upg_2,
              "OLD_DOS" );
        sfmt( item0, sizeof( item0 ), tx_upg_item, tgt.sysname );
        items[0] = item0;
        items[1] = full_32 ? tx_item_fdisk : tx_item_exit;
        if ( choice_page( para, para2, items, 2, tx_keys_ehx,
                          tx_h_upg, tx_h_upg_text ) != 0 ) {
            leave( 2 );
        }
        opt.upgrade = 1;
        break;
    }
    case TG_PMDOS:
        sfmt( para, sizeof( para ), tx_re_1, 'A' + tgt.drive );
        items[0] = tx_re_item;
        items[1] = full_32 ? tx_item_fdisk : tx_item_exit;
        if ( choice_page( para, NULL, items, 2, tx_keys_ehx,
                          tx_h_re, tx_h_re_text ) != 0 ) {
            leave( 2 );
        }
        opt.upgrade = 1;
        break;
    case TG_WIN9X:
        cannot( tx_cant_win9x );
        break;
    case TG_PACKED:
        cannot( tx_cant_packed );
        break;
    default:
        cannot( tx_cant_foreign );
        break;
    }
}

/* ------------------------------------------------------------------ */
/* the settings                                                        */
/* ------------------------------------------------------------------ */

/* "10/24/26" in the country's order and separator */
static void date_text( char *buf, int max, int y, int m, int d )
{
    const COUNTRYDEF *c = opt.country;
    char sep = c ? c->date_sep : '/';
    int order = c ? c->date_order : 0;

    y %= 100;
    if ( order == 1 ) {
        sfmt( buf, max, "%02d%c%02d%c%02d", d, sep, m, sep, y );
    } else if ( order == 2 ) {
        sfmt( buf, max, "%02d%c%02d%c%02d", y, sep, m, sep, d );
    } else {
        sfmt( buf, max, "%02d%c%02d%c%02d", m, sep, d, sep, y );
    }
}

static void date_form( char *buf, int max )
{
    const COUNTRYDEF *c = opt.country;
    char sep = c ? c->date_sep : '/';
    int order = c ? c->date_order : 0;

    if ( order == 1 ) {
        sfmt( buf, max, "dd%cmm%cyy", sep, sep );
    } else if ( order == 2 ) {
        sfmt( buf, max, "yy%cmm%cdd", sep, sep );
    } else {
        sfmt( buf, max, "mm%cdd%cyy", sep, sep );
    }
}

static void time_text( char *buf, int max, int h, int mi )
{
    sfmt( buf, max, "%02d%c%02d", h, opt.country ? opt.country->time_sep : ':', mi );
}

/* three numbers, whatever separates them */
static int numbers( const char *text, int *val, int count )
{
    int n = 0;
    const char *end;

    while ( *text && n < count ) {
        if ( *text >= '0' && *text <= '9' ) {
            val[n++] = (int)parse_u32( text, &end );
            text = end;
        } else {
            text++;
        }
    }
    return n;
}

static int parse_date( const char *text )
{
    int v[3], y, m, d, order = opt.country ? opt.country->date_order : 0;
    static const u8 days[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if ( numbers( text, v, 3 ) != 3 ) {
        return 1;
    }
    if ( order == 1 ) {
        d = v[0]; m = v[1]; y = v[2];
    } else if ( order == 2 ) {
        y = v[0]; m = v[1]; d = v[2];
    } else {
        m = v[0]; d = v[1]; y = v[2];
    }
    if ( y < 100 ) {
        y += y >= 80 ? 1900 : 2000;
    }
    if ( y < 1980 || y > 2079 || m < 1 || m > 12 || d < 1 || d > days[m - 1] ||
         (m == 2 && d == 29 && (y % 4)) ) {
        return 1;
    }
    opt.year = y;
    opt.month = m;
    opt.day = d;
    return 0;
}

static int parse_time( const char *text )
{
    int v[2];

    if ( numbers( text, v, 2 ) != 2 || v[0] > 23 || v[1] > 59 ) {
        return 1;
    }
    opt.hour = v[0];
    opt.minute = v[1];
    return 0;
}

static void bad_value( const char *title_text, const char *fmt, const char *form )
{
    char text[100];

    sfmt( text, sizeof( text ), fmt, form );
    ui_notice( title_text, text, A_BAR, tx_keys_ex );
}

static void date_time_page( void )
{
    char date[12], time[8], form[12], help[300];
    FIELD fld;
    int which = 0, key, y, m, d, h, mi;

    date_text( date, sizeof( date ), opt.year, opt.month, opt.day );
    time_text( time, sizeof( time ), opt.hour, opt.minute );
    for ( ;; ) {
        scr_desk( title );
        scr_para( 4, tx_dt_1 );
        scr_box( 6, 10, 9, 35, A_DESK, NULL );
        scr_put( 7, 12, A_DESK, tx_dt_date );
        scr_put( 8, 12, A_DESK, tx_dt_time );
        scr_put( 7, 25, A_DESK, date );
        scr_put( 8, 25, A_DESK, time );
        scr_para( 11, tx_dt_2 );
        scr_para( 13, tx_dt_3 );
        scr_status( tx_keys_ehxp, NULL );
        mem_set( &fld, 0, sizeof( fld ) );
        fld.row = which ? 8 : 7;
        fld.col = 25;
        fld.width = which ? 5 : 8;
        fld.buf = which ? time : date;
        fld.max = which ? 6 : 9;
        fld.pos = -1;
        fld.accept = "0123456789/-.:";
        key = ui_field( &fld );
        switch ( key ) {
        case K_UP:
        case K_DOWN:
            which = !which;
            break;
        case K_ESC:
            return;
        case K_F1:
            if ( which ) {
                char sep[2];
                sep[0] = opt.country ? opt.country->time_sep : ':';
                sep[1] = 0;
                sfmt( help, sizeof( help ), tx_h_time_text, sep, sep[0] );
                ui_help( tx_h_time, help );
            } else {
                date_form( form, sizeof( form ) );
                sfmt( help, sizeof( help ), tx_h_date_text, form );
                ui_help( tx_h_date, help );
            }
            break;
        case K_F3:
            ui_exit_box();
            break;
        case K_ENTER:
            y = opt.year; m = opt.month; d = opt.day; h = opt.hour; mi = opt.minute;
            if ( parse_date( date ) ) {
                date_form( form, sizeof( form ) );
                bad_value( tx_baddate_title, tx_baddate, form );
                which = 0;
                break;
            }
            if ( parse_time( time ) ) {
                opt.year = y; opt.month = m; opt.day = d;
                time_text( form, sizeof( form ), 0, 0 );
                form[0] = form[1] = 'h';
                form[3] = form[4] = 'm';
                bad_value( tx_badtime_title, tx_badtime, form );
                which = 1;
                break;
            }
            if ( opt.year != y || opt.month != m || opt.day != d ) {
                plat_set_date( opt.year, opt.month, opt.day );
            }
            if ( opt.hour != h || opt.minute != mi ) {
                plat_set_time( opt.hour, opt.minute );
            }
            (void)mi;
            return;
        }
    }
}

static int shares_page( const u16 *one, int n1, const u16 *two, int n2 )
{
    int i, j;

    for ( i = 0; i < n1; i++ ) {
        for ( j = 0; j < n2; j++ ) {
            if ( one[i] == two[j] ) {
                return 1;
            }
        }
    }
    return 0;
}

/* the country list, or the keyboard list */
static void nls_page( int keyboards )
{
    const char **names;
    LIST list;
    int count = keyboards ? inf.nkeyboards : inf.ncountries, i, pick;

    names = (const char **)xalloc( sizeof( char * ) * (u32)count );
    mem_set( &list, 0, sizeof( list ) );
    for ( i = 0; i < count; i++ ) {
        names[i] = keyboards ? inf.keyboard[i].name : inf.country[i].name;
        if ( keyboards ? &inf.keyboard[i] == opt.keyboard : &inf.country[i] == opt.country ) {
            list.sel = i;
        }
    }
    list.first = -1;
    for ( ;; ) {
        scr_desk( title );
        scr_para( 4, keyboards ? tx_kb_1 : tx_ctry_1 );
        scr_para( 18, tx_list_accept );
        scr_para( 19, tx_list_change );
        scr_status( tx_keys_ehxp, NULL );
        list.top = 7;
        list.rows = count < 8 ? count : 8;
        list.items = names;
        list.count = count;
        list.help_head = keyboards ? tx_h_kb : tx_h_ctry;
        list.help_text = keyboards ? tx_h_kb_text : tx_h_ctry_text;
        pick = ui_list( &list );
        if ( pick < 0 ) {
            break;
        }
        if ( keyboards ) {
            const KEYBDEF *kb = &inf.keyboard[pick];
            if ( opt.country && !shares_page( kb->page, kb->npages,
                                              opt.country->page, opt.country->npages ) ) {
                ui_notice( tx_cp_title, tx_cp_kb, A_NOTE, tx_keys_e );
                continue;
            }
            opt.keyboard = kb;
        } else {
            const COUNTRYDEF *ctry = &inf.country[pick];
            if ( opt.keyboard && !shares_page( ctry->page, ctry->npages,
                                               opt.keyboard->page, opt.keyboard->npages ) ) {
                ui_notice( tx_cp_title, tx_cp_ctry, A_NOTE, tx_keys_e );
                continue;
            }
            opt.country = ctry;
        }
        break;
    }
    xfree( (void *)names );
}

static void floppy_drive_page( void )
{
    static char item_a[12], item_b[12];
    const char *items[2];
    LIST list;
    int count = 1, pick;

    sfmt( item_a, sizeof( item_a ), tx_fl_drive_item, 'A' );
    sfmt( item_b, sizeof( item_b ), tx_fl_drive_item, 'B' );
    items[0] = item_a;
    items[1] = item_b;
    if ( plat_fd_type( 1 ) != FDT_NONE ) {
        count = 2;
    }
    mem_set( &list, 0, sizeof( list ) );
    scr_desk( title );
    scr_para( 4, tx_fl_drive_1 );
    scr_status( tx_keys_ehxp, NULL );
    list.top = 6;
    list.rows = count;
    list.items = items;
    list.count = count;
    list.sel = floppy_drive();
    list.help_head = tx_h_fldrive;
    list.help_text = tx_h_fldrive_text;
    pick = ui_list( &list );
    if ( pick >= 0 ) {
        floppy_target( pick );
    }
}

/* The settings page: the highlight starts on "The settings are
   correct." and goes back there after every choice, as in 6.22. */
static void settings( int floppy )
{
    static char line[4][70];
    const char *items[5];
    char value[40];
    LIST list;
    int count, pick, changed = 0;
    const COUNTRYDEF *was_country = opt.country;
    const KEYBDEF *was_keyboard = opt.keyboard;

    for ( ;; ) {
        plat_get_date( &opt.year, &opt.month, &opt.day );
        plat_get_time( &opt.hour, &opt.minute );
        date_text( value, sizeof( value ), opt.year, opt.month, opt.day );
        sfmt( line[0], sizeof( line[0] ), "%-18s%s  ", tx_set_date, value );
        time_text( value, sizeof( value ), opt.hour, opt.minute );
        str_catn( line[0], value, sizeof( line[0] ) );
        if ( floppy ) {
            sfmt( line[1], sizeof( line[1] ), "%-18s%c:\\", tx_set_to, 'A' + floppy_drive() );
            items[0] = line[0];
            items[1] = line[1];
            items[2] = "";
            items[3] = tx_set_ok;
            count = 4;
        } else {
            sfmt( line[1], sizeof( line[1] ), "%-18s%s", tx_set_country,
                  opt.country ? opt.country->name : "" );
            sfmt( line[2], sizeof( line[2] ), "%-18s%s", tx_set_keyb,
                  opt.keyboard ? opt.keyboard->name : "" );
            items[0] = line[0];
            items[1] = line[1];
            items[2] = line[2];
            items[3] = "";
            items[4] = tx_set_ok;
            count = 5;
        }
        scr_desk( title );
        scr_para( 4, tx_set_1 );
        scr_para( 6 + count + 2, tx_set_2 );
        scr_para( 6 + count + 4, tx_set_3 );
        scr_status( tx_keys_ehx, NULL );
        mem_set( &list, 0, sizeof( list ) );
        list.top = 6;
        list.rows = count;
        list.items = items;
        list.count = count;
        list.sel = count - 1;
        list.first = 0;
        list.flags = LS_NOESC;
        list.help_head = floppy ? tx_h_flset : tx_h_set;
        list.help_text = floppy ? tx_h_flset_text : tx_h_set_text;
        pick = ui_list( &list );
        if ( pick == count - 1 ) {
            break;
        }
        if ( pick == 0 ) {
            date_time_page();
        } else if ( floppy ) {
            floppy_drive_page();
        } else if ( pick == 1 ) {
            nls_page( 0 );
        } else if ( pick == 2 ) {
            nls_page( 1 );
        }
    }
    changed = opt.country != was_country || opt.keyboard != was_keyboard;
    opt.nls_changed = changed;
}

/* ------------------------------------------------------------------ */
/* the directory                                                       */
/* ------------------------------------------------------------------ */

/* every level of it; 0 = it is there */
/* The directories make_dir made, as PM-DOS will name them, shallowest
   first: an Uninstall takes away the ones it leaves empty */
char made_dirs[MADE_DIRS][PATHLEN];
int  nmade_dirs;

static int make_dir( const char *final_path )
{
    char path[PATHLEN];
    u16 attr;
    int i;

    str_cpyn( path, final_path, sizeof( path ) );
    path[0] = (char)('A' + tgt.drive);
    for ( i = 3; ; i++ ) {
        if ( path[i] == '\\' || path[i] == 0 ) {
            char keep = path[i];
            path[i] = 0;
            if ( plat_get_attr( path, &attr ) ) {
                if ( plat_mkdir( path ) ) {
                    return 1;
                }
                if ( nmade_dirs < MADE_DIRS ) {      /* for Uninstall */
                    str_cpyn( made_dirs[nmade_dirs], final_path, i + 1 );
                    nmade_dirs++;
                }
            } else if ( !(attr & FA_DIR) ) {
                return 1;
            }
            path[i] = keep;
            if ( keep == 0 ) {
                return 0;
            }
        }
    }
}

static void directory_page( void )
{
    static const int keys[] = { K_ENTER, 0 };
    char path[PATHLEN], text[120];
    FIELD fld;
    DLG dlg;
    int key;

    str_cpyn( path, opt.dosdir, sizeof( path ) );
    mem_set( &fld, 0, sizeof( fld ) );
    fld.pos = -1;
    for ( ;; ) {
        scr_desk( title );
        scr_para( 4, tx_dir_1 );
        scr_para( 9, tx_dir_2 );
        scr_para( 11, tx_dir_3 );
        scr_status( tx_keys_ehx, NULL );
        fld.row = 7;
        fld.col = 10;
        fld.width = DIRFIELD;
        fld.buf = path;
        fld.max = DIRFIELD + 1;
        fld.fixed = 3;
        key = ui_field( &fld );
        if ( key == K_F1 ) {
            ui_help( tx_h_dir, tx_h_dir_text );
            continue;
        }
        if ( key == K_F3 ) {
            ui_exit_box();
            continue;
        }
        if ( key != K_ENTER ) {
            continue;
        }
        if ( !path_valid( path ) ) {
            dlg_start( &dlg, tx_baddir_title, 36 );
            dlg_blank( &dlg );
            dlg_center( &dlg, tx_baddir_1 );
            dlg_center( &dlg, tx_baddir_2 );
            dlg_center( &dlg, tx_baddir_3 );
            dlg_blank( &dlg );
            dlg_run( &dlg, A_BAR, tx_keys_ex, keys, 1 );
            continue;
        }
        if ( make_dir( path ) ) {
            sfmt( text, sizeof( text ), tx_mkdir, path );
            ui_notice( tx_mkdir_title, text, A_BAR, tx_keys_ex );
            continue;
        }
        str_cpyn( opt.dosdir, path, sizeof( opt.dosdir ) );
        return;
    }
}

/* ------------------------------------------------------------------ */
/* the end                                                             */
/* ------------------------------------------------------------------ */

static void remove_disks( void )
{
    static const int keys[] = { K_ENTER, 0 };
    DLG dlg;

    dlg_start( &dlg, "", 44 );
    dlg_blank( &dlg );
    dlg_para( &dlg, tx_remove );
    dlg_blank( &dlg );
    dlg_run( &dlg, A_NOTE, "", keys, 0 );
}

static void complete( int floppy )
{
    static const int keys[] = { K_ENTER, 0 };
    char line[80], old[100];
    DLG dlg;

    dlg_start( &dlg, floppy ? "" : tx_done_title, 48 );
    dlg_blank( &dlg );
    if ( floppy ) {
        dlg_para( &dlg, tx_fl_done );
        dlg_blank( &dlg );
        dlg_bullet( &dlg, tx_leave_enter );
    } else {
        sfmt( line, sizeof( line ), tx_done_1, inf.version );
        dlg_para( &dlg, line );
        if ( tgt.state == TG_MSDOS ) {
            sfmt( old, sizeof( old ), tx_done_old, undo_dir() );
            dlg_blank( &dlg );
            dlg_para( &dlg, old );
        }
        dlg_blank( &dlg );
        dlg_bullet( &dlg, tx_done_enter );
        dlg_blank( &dlg );
        dlg_bullet( &dlg, tx_done_help );
    }
    dlg_blank( &dlg );
    dlg_run( &dlg, A_NOTE, tx_keys_e, keys, 0 );
}

static int commit( void )
{
    static const int keys[] = { 'Y', 'y', 0 };
    char text[200];
    DLG dlg;

    sfmt( text, sizeof( text ), tx_commit, 'A' + tgt.drive );
    dlg_start( &dlg, tx_commit_title, 48 );
    dlg_blank( &dlg );
    dlg_para( &dlg, text );
    dlg_blank( &dlg );
    dlg_bullet( &dlg, tx_commit_y );
    dlg_blank( &dlg );
    dlg_bullet( &dlg, tx_commit_f3 );
    dlg_blank( &dlg );
    dlg_run( &dlg, A_BAR, tx_keys_commit, keys, 1 );
    return 1;
}

static void space_check( void )
{
    char have[24], need_text[24];
    u32 need = copy_total( 0 ), cluster = 4096;

    /* each file takes up to a cluster more than its bytes */
    need += (u32)inf.nfiles * cluster;
    if ( tgt.state == TG_PMDOS ) {
        return;                         /* the old files make room */
    }
    if ( !disk_space_ok( need ) ) {
        size_text( have, sizeof( have ), tgt.free_kb );
        size_text( need_text, sizeof( need_text ), need / 1024 + 1 );
        {
            char text[200];
            sfmt( text, sizeof( text ), tx_cant_space, 'A' + tgt.drive, have, need_text );
            cannot_text( text );
        }
    }
}

/* ------------------------------------------------------------------ */
/* the program                                                         */
/* ------------------------------------------------------------------ */

static void switches( int *floppy )
{
    const char *tail = plat_tail();
    char word[20];
    int len;

    for ( ;; ) {
        tail = skip_blanks( tail );
        if ( !*tail ) {
            return;
        }
        len = 0;
        word[len++] = *tail++;
        while ( *tail && *tail != ' ' && *tail != '\t' && *tail != '/' && len < 19 ) {
            word[len++] = *tail++;
        }
        word[len] = 0;
        if ( word[0] == '/' && len == 2 ) {
            switch ( ch_upper( (u8)word[1] ) ) {
            case 'F':
                *floppy = 1;
                continue;
            case 'G':
                opt.no_uninstall = 1;
                continue;
            case 'B':
                continue;               /* monochrome: not on PM-DOS */
            case '?':
                plat_line( tx_usage );
                plat_exit( 0 );
            }
        }
        {
            char line[60];
            sfmt( line, sizeof( line ), tx_badswitch, word );
            say_and_go( line );
        }
    }
}

void setup_main( void )
{
    char name[16];
    int floppy = 0, rc, resumed;

    test_mode = plat_getenv( "SETUPTEST" ) != NULL;
    mem_set( &opt, 0, sizeof( opt ) );
    path_name( name, plat_self(), sizeof( name ) );
    if ( !str_icmp( name, "UNINSTAL.EXE" ) ) {
        plat_exit( uninstall_main() );
    }
    switches( &floppy );

    if ( !(plat_caps & PC_32BIT) ) {
        if ( !plat_cpu386() ) {
            say_and_go( tx_need386 );
        }
        if ( plat_windows() ) {
            say_and_go( tx_inwin );
        }
        if ( plat_pmdos() ) {
            say_and_go( tx_onrmdos );   /* PM-DOS's real-mode DOS */
        }
    }
    if ( source_find() ) {
        say_and_go( tx_noinf );
    }
    {
        char path[PATHLEN];
        path_join( path, srcdir, "SETUP.INF", sizeof( path ) );
        if ( inf_load( path ) ) {
            say_and_go( tx_badinf );
        }
    }
    source_better();
    if ( disk_load_code() ) {
        say_and_go( tx_badinf );
    }
    str_cpyn( opt.dosdir, inf.dosdir, sizeof( opt.dosdir ) );
    opt.country = inf_country( "001" );
    {
        char code[8];
        sfmt( code, sizeof( code ), "%d", plat_country_code() );
        if ( inf_country( code ) ) {
            opt.country = inf_country( code );
        }
    }
    if ( opt.country == NULL ) {
        opt.country = &inf.country[0];
    }
    opt.keyboard = inf_keyboard( "US", 0 );
    if ( opt.keyboard == NULL ) {
        opt.keyboard = &inf.keyboard[0];
    }
    floppy_target( 0 );

    sfmt( title, sizeof( title ), floppy ? tx_title_floppy : tx_title,
          inf.product, inf.version );
    scr_open();

    /* Started again after the restart that follows the partitioning:
       Setup's own partition, not yet formatted, goes straight to the
       format, without the welcome - as 6.22's second pass did */
    resumed = 0;
    if ( !floppy && (plat_caps & PC_PARTITION) ) {
        disk_examine();
        resumed = tgt.state == TG_RAW && disk_marked();
    }
    if ( !floppy && !resumed && welcome() == 2 ) {
        floppy = 1;
        sfmt( title, sizeof( title ), tx_title_floppy, inf.product, inf.version );
    }
    if ( floppy ) {
        settings( 1 );
        if ( floppy_install() ) {
            leave( 1 );
        }
        complete( 1 );
        leave( 0 );
    }

    prepare_disk();
    if ( tgt.state == TG_MSDOS || tgt.state == TG_PMDOS ) {
        cfg_read_current();
        if ( tgt.olddir[0] ) {
            str_cpyn( opt.dosdir, tgt.olddir, sizeof( opt.dosdir ) );
        }
    }
    settings( 0 );
    if ( tgt.state != TG_PMDOS ) {
        directory_page();
    }
    space_check();

    if ( tgt.state == TG_MSDOS ) {
        commit();
        if ( undo_prepare() ) {
            cannot( tx_cant_boot );
        }
        if ( !opt.no_uninstall ) {
            undo_disk();
        }
    }
    copy_all();
    if ( tgt.state == TG_MSDOS ) {
        rc = cfg_upgrade( 1 );
    } else if ( tgt.state == TG_PMDOS ) {
        rc = opt.nls_changed ? cfg_upgrade( 0 ) : 0;
    } else {
        rc = cfg_write_new();
    }
    (void)rc;
    if ( disk_bootable() ) {
        cannot( tx_cant_boot );
    }
    disk_activate( made_partition );
    undo_finish();
    plat_flush();
    remove_disks();
    complete( 0 );
    restart();
}
