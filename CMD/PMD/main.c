/*
 * MAIN.C - PMD.EXE, the PM-DOS diagnostics: the command line, the main
 * screen and the commands.
 *
 * PMD IS MSD FOR PM-DOS: the same thirteen pages behind the same
 * thirteen buttons, the same menus, the same report and the same
 * switches.  What differs is where the facts come from.  MSD is a DOS
 * program and looks at the machine for itself; a native program here
 * runs at ring 3 inside its own slot and can look at nothing, so every
 * fact is asked of the kernel (SYS_D32.C says how) - and the kernel's
 * own answers, where it has them, are the truth about a machine it is
 * running: its memory, its drives, the interrupt lines it keeps.
 *
 *   PMD [/I]                    the screen
 *   PMD /F file                 who the report is for, then all of it
 *   PMD /P file                 all of it
 *   PMD /S [file]               the summary, on the screen if no file
 *
 * THE MAIN SCREEN is a button for each page with two lines beside it
 * saying the gist: press its highlighted letter, or click it.  Those
 * lines are found before the screen first appears, with a notice up
 * while they are - /I leaves each out until its page is opened.
 *
 * MSD's "Black & White" (/B, F5 and the Utilities menu) is left out: a
 * machine that runs PM-DOS is a 386 or later, and nearly every one of
 * those has an EGA, a VGA or better.
 */
#include "pmd.h"

int  view_count;
char view_names[MAX_VIEWS][14];
char view_paths[MAX_VIEWS][PATH_MAX];

static const char *const buttons[PG_COUNT] = {
    "Com&puter...", "&Memory...", "&Video...", "&Network...", "&OS Version...", "Mo&use...",
    "Other &Adapters...", "&Disk Drives...", "&LPT Ports...", "&COM Ports...", "IR&Q Status...",
    "&TSR Programs...", "Device D&rivers..."
};

#define LEFT_BUTTONS    7       /* the rest are in the right-hand column */
#define BUTTON_WIDTH    19

static char sums[PG_COUNT][2][SUM_WIDTH + 2];
static int  sums_have;
static int  no_detect;          /* /I */
static int  held = -1;          /* the button the mouse went down on */
static const char *status_text;
static int  screen_up;

/* ------------------------------------------------------------------ */
/* messages                                                            */
/* ------------------------------------------------------------------ */

const char *err_text( int rc )
{
    switch ( rc ) {
    case 2:   return "File not found";
    case 3:   return "Path not found";
    case 4:   return "Too many open files";
    case 5:   return "Access denied";
    case 8:   return "Insufficient memory";
    case 15:  return "Invalid drive";
    case 19:  return "The disk is write protected";
    case 21:  return "The drive is not ready";
    case 28:  return "The printer is out of paper";
    case 29:  return "Write fault";
    case 32:  return "The file is in use";
    case 112: return "The disk is full";
    }
    return "The file or device cannot be used";
}

static void say( const char *text )
{
    sys_stdout( text, str_len( text ) );
}

void out_of_memory( void )
{
    if ( screen_up ) {
        msg_box( "Insufficient memory", MB_OK );
    } else {
        say( "Insufficient memory\r\n" );
    }
}

void app_abandon( void )
{
    if ( screen_up ) {
        scr_done();
    }
    sys_exit( 0 );
}

/* ------------------------------------------------------------------ */
/* the main screen                                                     */
/* ------------------------------------------------------------------ */

static void button_place( int index, int *row, int *col )
{
    if ( index < LEFT_BUTTONS ) {
        *row = 2 + index * 3;
        *col = 2;
    } else {
        *row = 2 + (index - LEFT_BUTTONS) * 3;
        *col = 41;
    }
}

static int button_at( int row, int col )
{
    int index, brow, bcol;

    for ( index = 0; index < PG_COUNT; index++ ) {
        button_place( index, &brow, &bcol );
        if ( row == brow && col >= bcol && col < bcol + BUTTON_WIDTH ) {
            return index;
        }
    }
    return -1;
}

void app_status( const char *text )
{
    status_text = text;
}

void app_draw( void )
{
    int index, row, col, len;
    u8 attr;

    for ( row = 1; row < scr_rows - 1; row++ ) {
        scr_fill( row, 0, scr_cols, ' ', pal.desk );
    }
    menu_draw_bar( -1, 0 );
    for ( index = 0; index < PG_COUNT; index++ ) {
        button_place( index, &row, &col );
        attr = index == held ? pal.push_focus : pal.push;
        len = scr_hotlen( buttons[index] );
        scr_fill( row, col, BUTTON_WIDTH, ' ', attr );
        scr_hot( row, col + (BUTTON_WIDTH - len) / 2, buttons[index], attr,
                 index == held ? attr : pal.push_hot );
        /* the shadow a button casts: half a cell to its right, half a
           row below */
        scr_ch( row, col + BUTTON_WIDTH, 0xDC, pal.push_shadow );
        scr_fill( row + 1, col + 1, BUTTON_WIDTH, 0xDF, pal.push_shadow );
        scr_put( row, col + BUTTON_WIDTH + 2, sums[index][0], pal.summary );
        scr_put( row + 1, col + BUTTON_WIDTH + 2, sums[index][1], pal.summary );
    }
    scr_fill( scr_rows - 1, 0, scr_cols, ' ', pal.status );
    scr_put( scr_rows - 1, 1,
             status_text ? status_text
             : "Press ALT for menu, or press highlighted letter, or F3 to quit PMD.",
             pal.status );
}

/* a notice in the middle of the screen while something slow happens */
static void notice( const char *text )
{
    int len = (int)str_len( text ), top = scr_rows / 2 - 2, left = (scr_cols - len - 6) / 2;

    app_draw();
    scr_box( top, left, top + 4, left + len + 5, pal.dlg );
    scr_shadow( top, left, top + 4, left + len + 5 );
    scr_put( top + 2, left + 3, text, pal.dlg );
    scr_cursor( 0, 0, CUR_HIDE );
    scr_flush();
}

static void find_summaries( void )
{
    int page;

    notice( "PMD is examining your system ..." );
    for ( page = 0; page < PG_COUNT; page++ ) {
        page_summary( page, sums[page][0], sums[page][1] );
    }
    sums_have = 1;
}

static void show_page( int page )
{
    TEXT text;
    TEXTMARK mark;

    tx_init( &text );
    notice( "PMD is examining your system ..." );
    page_build( page, &text, 0 );
    if ( !sums_have ) {
        page_summary( page, sums[page][0], sums[page][1] );
    }
    if ( page == PG_MEMORY ) {
        /* the map: under the two lines of legend, between the addresses */
        mark.first = 2;
        mark.lines = MAP_ROWS;
        mark.col = 11;
        mark.width = MAP_COLS;
        mark.attr = pal.map;
        text_window( page_names[page], &text, &mark );
    } else {
        text_window( page_names[page], &text, NULL );
    }
    if ( text.failed ) {
        out_of_memory();
    }
    tx_free( &text );
}

/* nothing in PMD's menus is ever greyed */
int app_can( int cmd )
{
    return cmd != CMD_NONE;
}

/* 1 to leave */
static int app_command( int cmd )
{
    if ( cmd >= CMD_PAGE && cmd < CMD_PAGE + PG_COUNT ) {
        show_page( cmd - CMD_PAGE );
        return 0;
    }
    if ( cmd >= CMD_VIEW1 && cmd < CMD_VIEW1 + MAX_VIEWS ) {
        cmd_view( cmd - CMD_VIEW1 );
        return 0;
    }
    switch ( cmd ) {
    case CMD_FIND:    cmd_find_file();  break;
    case CMD_REPORT:  cmd_report();     break;
    case CMD_EXIT:    return 1;
    case CMD_BLOCKS:  cmd_blocks();     break;
    case CMD_BROWSE:  cmd_browse();     break;
    case CMD_INSERT:  cmd_insert();     break;
    case CMD_PRINTER: cmd_printer();    break;
    case CMD_ABOUT:   cmd_about();      break;
    }
    return 0;
}

static void run_screen( void )
{
    int key, cmd, index, letter;

    scr_init();
    screen_up = 1;
    key_init();
    mouse_init();
    views_find();
    if ( !no_detect ) {
        find_summaries();
    }
    for ( ;; ) {
        app_draw();
        scr_cursor( 0, 0, CUR_HIDE );
        scr_flush();
        key = key_get();
        cmd = CMD_NONE;
        if ( key == K_EOF ) {
            break;
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        if ( key == K_MOUSE ) {
            index = button_at( mouse.row, mouse.col );
            if ( mouse.kind == ME_UP ) {
                if ( held >= 0 && index == held ) {
                    cmd = CMD_PAGE + held;
                }
                held = -1;
            } else if ( mouse.kind == ME_DRAG ) {
                continue;
            } else if ( mouse.row == 0 ) {
                cmd = menu_run( K_MOUSE );
            } else {
                held = index;
            }
        } else if ( key == K_ALTTAP || key == K_F10 || key_alt_letter( key ) ) {
            cmd = menu_run( key );
        } else if ( key == K_F3 ) {
            cmd = CMD_EXIT;
        } else if ( key < 0x100 ) {
            letter = ch_upper( key );
            for ( index = 0; index < PG_COUNT; index++ ) {
                if ( scr_hotkey( buttons[index] ) == letter ) {
                    cmd = CMD_PAGE + index;
                    break;
                }
            }
        }
        if ( cmd != CMD_NONE && app_command( cmd ) ) {
            break;
        }
    }
    scr_done();
    screen_up = 0;
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

static const char help_text[] =
    "Shows what PM-DOS has found in this computer: the processor, memory,\r\n"
    "video, drives, ports and interrupt lines, and the programs and drivers\r\n"
    "that are in memory.\r\n"
    "\r\n"
    "PMD [/I]\r\n"
    "PMD /F[drive:][path]filename\r\n"
    "PMD /P[drive:][path]filename\r\n"
    "PMD /S[[drive:][path]filename]\r\n"
    "\r\n"
    "  /I   Specifies that no initial hardware detection be performed.\r\n"
    "  /F   Writes complete report to specified file.\r\n"
    "  /P   Writes complete report to specified file without asking\r\n"
    "       for user input.\r\n"
    "  /S   Writes the summary report to specified file.  If no filename\r\n"
    "       is specified, output will be to the screen.\r\n";

/* what follows a switch: joined to it, or the next word */
static const char *switch_name( const char *tail, char *name, u32 max )
{
    u32 len = 0;

    while ( *tail == ' ' || *tail == '\t' ) {
        tail++;
    }
    if ( *tail == '/' ) {
        name[0] = 0;
        return tail;
    }
    while ( *tail && *tail != ' ' && *tail != '\t' && *tail != '/' ) {
        if ( len + 1 < max ) {
            name[len++] = *tail;
        }
        tail++;
    }
    name[len] = 0;
    return tail;
}

static void ask_customer( CUSTOMER *who )
{
    int index;

    say( "\r\nWho is this report from?  Press ENTER to leave a line empty.\r\n\r\n" );
    for ( index = 0; index < 8; index++ ) {
        say( customer_labels[index] );
        sys_stdin_line( who->field[index], sizeof( who->field[index] ) );
    }
}

/* /S with no file: the summary, as lines on the screen */
static void show_summary( void )
{
    TEXT text;
    int index;

    tx_init( &text );
    report_summary_text( &text );
    for ( index = 0; index < text.count; index++ ) {
        say( text.line[index] );
        say( "\r\n" );
    }
    tx_free( &text );
}

void pmd_main( void )
{
    static u8 items[RPT_ITEMS];
    static CUSTOMER who;
    const char *tail;
    char name[PATH_MAX];
    int mode = 0, rc, index;

    sys_init();
    sys_break_install();
    name[0] = 0;
    for ( tail = sys_cmdline(); *tail; ) {
        if ( *tail != '/' ) {
            if ( *tail != ' ' && *tail != '\t' ) {
                say( "Invalid parameter - " );
                say( tail );
                say( "\r\n" );
                sys_exit( 1 );
            }
            tail++;
            continue;
        }
        switch ( ch_upper( tail[1] ) ) {
        case '?':
            say( help_text );
            sys_exit( 0 );
            break;
        case 'I':
            no_detect = 1;
            tail += 2;
            break;
        case 'F':
        case 'P':
        case 'S':
            mode = ch_upper( tail[1] );
            tail = switch_name( tail + 2, name, sizeof( name ) );
            break;
        default:
            say( "Invalid switch - " );
            say( tail );
            say( "\r\n" );
            sys_exit( 1 );
        }
    }

    if ( mode == 0 ) {
        run_screen();
        sys_exit( 0 );
    }
    if ( mode != 'S' && name[0] == 0 ) {
        say( "Required parameter missing\r\n" );
        sys_exit( 1 );
    }
    if ( mode == 'S' && name[0] == 0 ) {
        show_summary();
        sys_exit( 0 );
    }
    views_find();
    if ( mode == 'S' ) {
        items[RPT_SUMMARY] = 1;
    } else {
        for ( index = 0; index < RPT_ITEMS; index++ ) {
            items[index] = 1;
        }
        items[RPT_CUSTOMER] = 0;
        if ( mode == 'F' ) {
            ask_customer( &who );
            items[RPT_CUSTOMER] = 1;
        }
    }
    rc = report_write( name, items, &who );
    if ( rc ) {
        say( err_text( rc ) );
        say( " - " );
        say( name );
        say( "\r\n" );
        sys_exit( 1 );
    }
    sys_exit( 0 );
}
