/*
 * MAIN.C - where TREEVIEW.EXE starts: DiskStat's DOSMAIN.CPP.  The
 * switches, the screen, the menus, and the loop that hands each key and
 * click to whatever wants it:
 *
 *   Row 1      the title, and the close box in its corner
 *   Row 2      the menu bar: File / Drive / Help
 *   Rows 3..   the disk's usage (left) | the folders (right)
 *   Last row   the status line: Total / Free
 *
 * TREEVIEW /G puts the same screen up in VGA graphics (640x480, the
 * card's own 8x16 font).  That grid is 80x30, not 80x25, so the panes
 * run to row 29 and the status line sits on row 30: nothing here may
 * assume 25 rows - ask df_rows(), which also gives a 43- or 50-line text
 * screen its extra rows.  The switch is the only way in, as it was.
 *
 * Nothing is printed on starting: a PM-DOS command begins with its work,
 * so DiskStat's banner line is not here.
 */
#include "treeview.h"

#define MAX_ARGS    16
#define MAX_DRIVES  24

static char *args[MAX_ARGS + 1];
static char  arg_text[300];

static int  drive_ids[MAX_DRIVES];
static int  partial;            /* the figures on the screen are short */
static int  running;

/* the tail into words; quotes keep a word with spaces in it whole */
static int split_args( const char *tail )
{
    char *out = arg_text;
    int count = 1, quoted;

    args[0] = "TREEVIEW";
    while ( *tail && count < MAX_ARGS ) {
        while ( *tail == ' ' || *tail == '\t' ) {
            tail++;
        }
        if ( !*tail || out >= arg_text + sizeof( arg_text ) - 1 ) {
            break;
        }
        args[count++] = out;
        quoted = 0;
        while ( *tail && (quoted || (*tail != ' ' && *tail != '\t')) ) {
            if ( *tail == '"' ) {
                quoted = !quoted;
            } else if ( out < arg_text + sizeof( arg_text ) - 1 ) {
                *out++ = *tail;
            }
            tail++;
        }
        *out++ = 0;
    }
    args[count] = NULL;
    return count;
}

/* "/G" or "-G", either case: the VGA graphics screen */
static int is_graphics_switch( const char *arg )
{
    if ( arg == NULL || (arg[0] != '/' && arg[0] != '-') ) {
        return 0;
    }
    return (arg[1] == 'g' || arg[1] == 'G') && arg[2] == 0;
}

static int is_help( const char *arg )
{
    return strcmp( arg, "?" ) == 0 || strcmp( arg, "/?" ) == 0 || strcmp( arg, "-?" ) == 0;
}

static void usage( void )
{
    puts( "Displays information about a drive's disk usage by directory." );
    puts( " TREEVIEW [drive] [/G]\n" );
    puts( "Parameters:" );
    puts( " drive       Specifies a drive to show. Default is drive C:\\.\n" );
    puts( "Switches:" );
    puts( " /G          Uses a VGA graphics screen instead of text mode.\n" );
}

/* ------------------------------------------------------------------ */
/* the status line and the whole screen                                */
/* ------------------------------------------------------------------ */

static void update_status( void )
{
    char text[120], total_text[24], free_text[24];
    char letter = (char)('A' + current_drive - 1);

    if ( data_valid && total_disk_space == 0 ) {
        sprintf( text, "  Drive %c:    No disk information", letter );     /* as the left pane says */
    } else if ( data_valid ) {
        format_size( total_disk_space, total_text );
        format_size( free_disk_space, free_text );
        sprintf( text, "  Drive %c:    Total: %s    Free: %s%s", letter, total_text, free_text,
                 partial ? "    Scan incomplete" : "" );
    } else {
        sprintf( text, "  Scanning drive %c: ...", letter );
    }
    status_set( text );
}

/* the desktop, then each part in the order the DOS program added them */
static void draw_all( void )
{
    df_clear( FG_WHITE, BG_BLUE );
    title_draw();
    usage_draw();
    tree_draw();
    status_draw();
    menu_draw();
    update_status();
}

/* scan "drive" and draw the panes and the status line again */
static void show_drive( int drive )
{
    /* asked BEFORE the old figures are thrown away: with no disk in the
       drive and Cancel for an answer, the screen keeps the drive it had */
    if ( !drive_ready( drive ) ) {
        return;
    }
    current_drive = drive;

    /* the panes in their "scanning" state first, and sent, so the notice
       is on the screen while the scan runs rather than after it */
    data_valid = 0;
    usage_draw();
    tree_draw();
    update_status();
    df_flush();

    partial = !scan_drive_ask();

    tree_after_scan();
    usage_draw();
    tree_draw();
    update_status();
}

/* ------------------------------------------------------------------ */
/* the menus                                                           */
/* ------------------------------------------------------------------ */

static void act_drive( int drive )
{
    show_drive( drive );
}

static void act_refresh( int arg )
{
    (void)arg;
    show_drive( current_drive );
}

static void act_exit( int arg )
{
    (void)arg;
    running = 0;
}

static void act_about( int arg )
{
    RECT at;
    int dy = df_rows() > 25 ? (df_rows() - 25) / 2 : 0;

    (void)arg;
    /* bright text on blue in a double frame, so the box stands out from
       the grey panes; placed for 80x25 and moved down by half the extra
       rows of a taller screen, so it stays in the middle */
    at.row = 8 + dy;
    at.col = 20;
    at.end_row = 17 + dy;
    at.end_col = 59;
    msg_ok( FG_BRIGHT_WHITE, BG_BLUE, "About " APP_NAME,
            APP_NAME " - Dobson Utilities.  A drive space and folder-size visualizer.  "
            "(C) 2026 Charlie Dobson.", &at );
    update_status();
}

static void build_menus( void )
{
    char label[4];
    int menu, ndrv, at, room;

    menu_row( 1 );                      /* under the title */

    menu = menu_add( "File", 'F' );
    menu_item( menu, "Refresh  F5", 'R', act_refresh, 0 );
    menu_item( menu, "Exit", 'X', act_exit, 0 );

    /* a drive each, C: and up, as many as the screen has rows for */
    ndrv = detect_drives( drive_ids, MAX_DRIVES );
    room = df_rows() - 3;
    if ( ndrv > room ) {
        ndrv = room;
    }
    menu = menu_add( "Drive", 'D' );
    for ( at = 0; at < ndrv; at++ ) {
        label[0] = (char)('A' + drive_ids[at] - 1);
        label[1] = ':';
        label[2] = 0;
        menu_item( menu, label, label[0], act_drive, drive_ids[at] );
    }
    if ( ndrv > 0 ) {
        current_drive = drive_ids[0];
    }

    menu = menu_add( "Help", 'H' );
    menu_item( menu, "About", 'A', act_about, 0 );
}

/* ------------------------------------------------------------------ */
/* the start                                                           */
/* ------------------------------------------------------------------ */

void __cdecl dos_entry( void )
{
    int argc, in, out, want_gfx = 0, gfx_ok, last;
    char why[160];
    EVENT ev;

    xa_crt_init();
    argc = split_args( xa_cmdtail() );

    /* /G out of the arguments before anything reads them, so it can go
       anywhere on the line and the drive is still args[1] */
    for ( in = out = 1; in < argc; in++ ) {
        if ( is_graphics_switch( args[in] ) ) {
            want_gfx = 1;
        } else {
            args[out++] = args[in];
        }
    }
    argc = out;
    args[argc] = NULL;

    if ( argc == 2 && is_help( args[1] ) ) {
        usage();
        exit( 0 );
    }

    /* the graphics screen goes up before the first thing is drawn, so
       the grid is 80x30 from the start, and before the mouse is reset,
       which takes its size from the screen */
    gfx_ok = scr_open( want_gfx );
    if ( gfx_ok < 0 ) {
        scr_close();
        puts( "Not enough memory." );
        exit( 1 );
    }
    ev_init();
    ev_break_off();

    /* an empty drive or a bad disk is a box of ours from here on, not
       the kernel's "Abort, Retry, Fail?" across the screen */
    crit_install();

    title_set( APP_NAME " - Disk Statistics" );
    build_menus();

    /* down to the row above the status line of THIS screen */
    last = df_rows() - 2;
    usage_region( 2, 1, last, 33 );
    tree_region( 2, 35, last, 78 );

    /* "TREEVIEW D:" or "TREEVIEW d": the drive scanned first */
    if ( argc > 1 && args[1][0] ) {
        char letter = (char)toupper( (u8)args[1][0] );

        if ( letter >= 'A' && letter <= 'Z' ) {
            current_drive = letter - 'A' + 1;
        }
    }

    /* the whole screen at once, the panes saying "Scanning...", so it is
       not blank while the first scan runs */
    draw_all();
    df_flush();

    /* /G on a card that cannot do it: the switch is the only way to ask,
       so this is the only place to answer, over text mode underneath */
    if ( want_gfx && !gfx_ok ) {
        sprintf( why, "%s\n\n" APP_NAME " will use text mode.", gfx_error() );
        msg_ok( FG_BRIGHT_WHITE, BG_BLUE, "Graphics Mode", why, NULL );
    }

    /* A first drive with no disk in it, and Cancel, leaves the panes
       empty: there is no drive before it to go back to, and the Drive
       menu is a key away. */
    if ( drive_ready( current_drive ) ) {
        partial = !scan_drive_ask();
    } else {
        clear_scan();
    }
    tree_after_scan();
    draw_all();

    running = 1;
    while ( running ) {
        ev_wait( &ev );
        if ( ev.kind == EV_KEY && ev.key == KEY_F5 ) {
            show_drive( current_drive );
            continue;
        }
        /* the "[-]" in the title's corner is a close box */
        if ( ev.kind == EV_MOUSE && (ev.press & LEFT_BUTTON) && ev.y == 0 && ev.x < 3 ) {
            break;
        }
        if ( menu_event( &ev ) || tree_event( &ev ) ) {
            continue;
        }
        if ( ev.kind == EV_KEY && ev.key == KEY_ESC ) {
            break;
        }
    }

    scr_close();
    exit( 0 );
}
