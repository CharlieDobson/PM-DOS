/*
 * MAIN.C - HELP: the command line, the commands and the loop.
 *
 *   HELP [topic]        the help window, at the contents or at a topic
 *   HELP /?             a line about itself
 *
 * THE MODEL IS MS-DOS 6's HELP - QBASIC /QHELP and HELP.HLP: a menu
 * bar (File, Search, Help), a window with the topic in it, links in
 * brackets that Tab walks and Enter follows, Alt+C for the contents,
 * Alt+N for the next topic, Alt+B back, a Find across every topic, and
 * printing.  Its words are this program's own, and so is HELP.HLP's
 * text; its format is HELPMAKE's source (see HLP.C).
 *
 * ONE LOOP: draw everything (app_draw), put the cursor in the window,
 * flush, read a key, and either run a command with it or give it to
 * the window.  Menus and dialogs have loops of their own that draw the
 * same way and return a command or an answer.
 */
#include "help.h"

static const char *status_hint;
static const char  status_text[] = "<Alt+C=Contents> <Alt+N=Next> <Alt+B=Back>";
static int         ui_ready;
static char        asked[128];          /* the topic the command line named */
static char        hlp_path[PATH_MAX];

static const char usage[] =
    "Shows help about PM-DOS, its commands and CONFIG.SYS.\r\n"
    "\r\n"
    "HELP [topic]\r\n"
    "\r\n"
    "  topic   the command or subject to open at, such as DIR or COPY;\r\n"
    "          with none, the contents.\r\n"
    "\r\n"
    "The text comes from HELP.HLP, looked for in the current directory,\r\n"
    "beside HELP.EXE, and along the PATH.\r\n";

/* the topics visited: Alt+B goes back through them */
typedef struct {
    int topic;
    u32 top, line, col;
} PLACE;

#define HIST_MAX 32
static PLACE history[HIST_MAX];
static int   depth;

/* ------------------------------------------------------------------ */
/* odds and ends the rest of the program asks for                      */
/* ------------------------------------------------------------------ */

const char *err_text( int rc )
{
    switch ( rc ) {
    case ERR_NOFILE:    return "File not found.";
    case ERR_NOPATH:    return "Path not found.";
    case ERR_NOHANDLES: return "Too many files are open.";
    case ERR_ACCESS:    return "Access denied.";
    case ERR_BADDRIVE:  return "That drive does not exist.";
    case ERR_DISKFULL:  return "The disk is full.";
    case 19:            return "The disk is write-protected.";
    case 21:            return "The drive is not ready.";
    case ERR_NOMEM:
    case -1:            return "There is not enough memory.";
    }
    return "The disk could not be read or written.";
}

void out_of_memory( void )
{
    if ( ui_ready ) {
        msg_box( "There is not enough memory to do that.", MB_OK, NULL );
    } else {
        sys_stdout( "Not enough memory\r\n", 19 );
    }
}

void app_status( const char *text )
{
    status_hint = text;
}

void app_abandon( void )
{
    scr_done();
    sys_exit( 0 );
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

void app_draw( void )
{
    char right[16];
    const char *left;
    int row = scr_rows - 1, shift;

    menu_draw_bar( -1, 0 );
    view_draw();
    scr_fill( row, 0, scr_cols, ' ', pal.status );
    left = status_hint ? status_hint : status_text;
    scr_put( row, 1, left, pal.status );
    if ( (int)str_len( left ) > scr_cols - 22 ) {
        return;                             /* a long hint has the whole line */
    }
    scr_ch( row, scr_cols - 18, 0xB3, pal.status );
    shift = key_shift();
    if ( shift & 0x40 ) {
        scr_ch( row, scr_cols - 14, 'C', pal.status );
    }
    if ( shift & 0x20 ) {
        scr_ch( row, scr_cols - 12, 'N', pal.status );
    }
    sfmt( right, sizeof( right ), "%05u:%03u", view.topic < 0 ? 0 : view.line + 1,
          view.topic < 0 ? 0 : view.col + 1 );
    scr_put( row, scr_cols - 10, right, pal.status );
}

/* ------------------------------------------------------------------ */
/* topics                                                              */
/* ------------------------------------------------------------------ */

static void remember( void )
{
    if ( view.topic < 0 ) {
        return;
    }
    if ( depth == HIST_MAX ) {
        mem_cpy( history, history + 1, sizeof( PLACE ) * (HIST_MAX - 1) );
        depth--;
    }
    history[depth].topic = view.topic;
    history[depth].top = view.top;
    history[depth].line = view.line;
    history[depth].col = view.col;
    depth++;
}

void app_show( int topic )
{
    if ( topic < 0 || topic >= ntopics ) {
        return;
    }
    remember();
    view_show( topic );
}

void app_show_ctx( const char *context )
{
    int topic = hlp_find( context );

    if ( topic >= 0 ) {
        app_show( topic );
    }
}

static void go_back( void )
{
    PLACE *place;

    if ( depth == 0 ) {
        return;
    }
    place = &history[--depth];
    view_show( place->topic );
    view.top = place->top;
    view.line = place->line;
    view.col = place->col;
    view.want = place->col;
    view_keep_visible();
}

static int contents_topic( void )
{
    int topic = hlp_find( CTX_CONTENTS );

    return topic >= 0 ? topic : 0;
}

/* the link under the cursor, followed */
static void follow_link( void )
{
    HLINK link;
    RLINE *rl;
    char name[RL_MAX + 1];
    int topic, len;

    if ( !view_link_at_cursor( &link ) ) {
        return;
    }
    if ( link.ctx ) {
        topic = hlp_find_n( link.ctx, link.ctx_len );
        if ( topic < 0 ) {
            len = link.ctx_len < RL_MAX ? link.ctx_len : RL_MAX;
            mem_cpy( name, link.ctx, (u32)len );
            name[len] = 0;
        }
    } else {
        /* a link with no name of its own leads to the context its text names */
        rl = (RLINE *)xalloc( sizeof( RLINE ) );
        hlp_render( topics[view.topic].first + view.line, rl );
        len = link.end - link.start;
        mem_cpy( name, rl->ch + link.start, (u32)len );
        name[len] = 0;
        xfree( rl );
        topic = hlp_find( name );
    }
    if ( topic < 0 ) {
        msg_box2( "There is no topic named", name, MB_OK, NULL );
        return;
    }
    app_show( topic );
}

/* F1 in a menu or a dialog: the topic in a box over whatever is showing */
void help_show( const char *context )
{
    static char text[2200];
    static char title[90];
    static char line[RL_MAX + 1];
    CTL ctl[3];
    DLG dlg;
    int topic, rows, width = 64, len;
    u32 index, count, used = 0;

    topic = context ? hlp_find( context ) : -1;
    if ( topic < 0 ) {
        topic = hlp_find( CTX_HOWTO );
    }
    if ( topic < 0 ) {
        return;
    }
    count = topics[topic].count;
    if ( count > 14 ) {
        count = 14;
    }
    text[0] = 0;
    for ( index = 0; index < count; index++ ) {
        hlp_plain( topics[topic].first + index, line, sizeof( line ) );
        len = (int)str_len( line );
        if ( len > width - 8 ) {
            len = width - 8;
        }
        if ( used + (u32)len + 2 >= sizeof( text ) ) {
            break;
        }
        mem_cpy( text + used, line, (u32)len );
        used += (u32)len;
        text[used++] = '\n';
    }
    while ( used > 0 && text[used - 1] == '\n' ) {
        used--;
    }
    text[used] = 0;
    rows = 1;
    for ( index = 0; index < used; index++ ) {
        if ( text[index] == '\n' ) {
            rows++;
        }
    }
    str_cpy( title, "PM-DOS Help: " );
    str_catn( title, topics[topic].title ? topics[topic].title : "", sizeof( title ) );
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_BOX; ctl[0].row = 1; ctl[0].col = 2;
    ctl[0].width = width - 4; ctl[0].height = rows + 2;
    ctl[1].type = CT_TEXT; ctl[1].row = 2; ctl[1].col = 4; ctl[1].text = text;
    ctl[2].type = CT_BUTTON; ctl[2].row = rows + 4; ctl[2].col = (width - 6) / 2;
    ctl[2].text = "OK"; ctl[2].id = ID_OK; ctl[2].is_default = 1;
    dlg.title = title;
    dlg.rows = rows + 6;
    dlg.cols = width;
    dlg.ctl = ctl;
    dlg.nctl = 3;
    dlg.focus = 2;
    dlg.help = NULL;
    dlg_run( &dlg );
}

/* ------------------------------------------------------------------ */
/* the commands                                                        */
/* ------------------------------------------------------------------ */

int app_can( int cmd )
{
    switch ( cmd ) {
    case CMD_PRINT:
        return view.topic >= 0;
    case CMD_BACK:
        return depth > 0;
    }
    return 1;
}

void app_command( int cmd )
{
    switch ( cmd ) {
    case CMD_PRINT:
        dlg_print();
        break;
    case CMD_EXIT:
        scr_done();
        sys_exit( 0 );
        break;
    case CMD_FIND:
        dlg_find();
        break;
    case CMD_REPEAT:
        find_again();
        break;
    case CMD_HOWTO:
        app_show_ctx( CTX_HOWTO );
        break;
    case CMD_ABOUT:
        dlg_about();
        break;
    case CMD_CONTENTS:
        app_show( contents_topic() );
        break;
    case CMD_NEXT:
        if ( view.topic + 1 < ntopics ) {
            app_show( view.topic + 1 );
        }
        break;
    case CMD_PREV:
        if ( view.topic > 0 ) {
            app_show( view.topic - 1 );
        }
        break;
    case CMD_BACK:
        go_back();
        break;
    }
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

static void parse_line( const char *line )
{
    u32 used = 0;

    while ( *line == ' ' || *line == '\t' ) {
        line++;
    }
    if ( line[0] == '/' && line[1] == '?' ) {
        sys_stdout( usage, str_len( usage ) );
        sys_exit( 0 );
    }
    while ( *line && *line != '\r' && *line != '\n' ) {
        if ( *line != '"' && used < sizeof( asked ) - 1 ) {
            asked[used++] = *line;
        }
        line++;
    }
    while ( used > 0 && (asked[used - 1] == ' ' || asked[used - 1] == '\t') ) {
        used--;
    }
    asked[used] = 0;
}

/* HELP.HLP: in the current directory, beside HELP.EXE, then along the PATH */
static int find_help_file( void )
{
    const char *own = sys_program_path(), *path = sys_getenv( "PATH" ), *slash;
    u16 attr;
    u32 len, end;

    if ( sys_get_attr( "HELP.HLP", &attr ) == 0 ) {
        str_cpy( hlp_path, "HELP.HLP" );
        return 1;
    }
    slash = str_rchr( own, '\\' );
    if ( slash ) {
        len = (u32)(slash + 1 - own);
        if ( len < PATH_MAX - 10 ) {
            mem_cpy( hlp_path, own, len );
            str_cpy( hlp_path + len, "HELP.HLP" );
            if ( sys_get_attr( hlp_path, &attr ) == 0 ) {
                return 1;
            }
        }
    }
    while ( path && *path ) {
        for ( len = 0; path[len] && path[len] != ';'; len++ ) {
        }
        if ( len && len < PATH_MAX - 10 ) {
            mem_cpy( hlp_path, path, len );
            end = len;
            if ( hlp_path[end - 1] != '\\' && hlp_path[end - 1] != ':' ) {
                hlp_path[end++] = '\\';
            }
            str_cpy( hlp_path + end, "HELP.HLP" );
            if ( sys_get_attr( hlp_path, &attr ) == 0 ) {
                return 1;
            }
        }
        path += len;
        if ( *path == ';' ) {
            path++;
        }
    }
    hlp_path[0] = 0;
    return 0;
}

static void say( const char *text )
{
    sys_stdout( text, str_len( text ) );
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

static void app_frame( void )
{
    app_draw();
    view_place_cursor();
    scr_flush();
}

/* the next thing to happen while the button is held: K_MOUSE, a key, or
   K_TICK when "hundredths" pass - and the test file's two escapes seen to */
static int held_next( int hundredths )
{
    int key;

    for ( ;; ) {
        app_frame();
        key = key_get_timed( hundredths );
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key != K_DUMP ) {
            return key;
        }
        scr_dump();
    }
}

/* a scroll bar: its arrows and its track again and again while the button
   stays on them, and its box wherever it is dragged */
static void scroll_held( int part )
{
    int key, row, col, delay = 40, thumb = part == VH_VTHUMB;

    view_scroll_part( part, mouse.row, mouse.col );
    for ( ;; ) {
        key = held_next( delay );
        if ( key == K_MOUSE ) {
            if ( mouse.kind == ME_UP ) {
                return;
            }
            if ( mouse.kind == ME_DRAG && thumb ) {
                view_scroll_part( part, mouse.row, mouse.col );
            }
        } else if ( key == K_TICK && !thumb ) {
            mouse_where( &row, &col );
            if ( view_hit( row, col ) == part ) {
                view_scroll_part( part, row, col );
            }
            delay = 5;
        }
    }
}

/* the status bar's buttons, clicked, are the keys they name */
static int status_word( const char *word, int col )
{
    u32 len = str_len( word ), start;

    for ( start = 0; status_text[start]; start++ ) {
        if ( str_nicmp( status_text + start, word, len ) == 0 ) {
            return col >= 1 + (int)start && col < 1 + (int)(start + len);
        }
    }
    return 0;
}

static void app_mouse( void )
{
    int part, cmd;

    if ( mouse.kind != ME_DOWN && mouse.kind != ME_DOUBLE ) {
        return;                             /* nothing was pressed here */
    }
    if ( mouse.row == 0 ) {
        cmd = menu_run( K_MOUSE );
        if ( cmd ) {
            app_command( cmd );
        }
        return;
    }
    if ( mouse.row == scr_rows - 1 ) {
        if ( status_hint ) {
            return;
        }
        if ( status_word( "<Alt+C=Contents>", mouse.col ) ) {
            app_command( CMD_CONTENTS );
        } else if ( status_word( "<Alt+N=Next>", mouse.col ) ) {
            app_command( CMD_NEXT );
        } else if ( status_word( "<Alt+B=Back>", mouse.col ) ) {
            app_command( CMD_BACK );
        }
        return;
    }
    part = view_hit( mouse.row, mouse.col );
    if ( part == VH_TEXT ) {
        view_mouse_to( mouse.row, mouse.col );
        follow_link();                      /* a click on a link reads it */
    } else if ( part != VH_NONE ) {
        scroll_held( part );
    }
}

/* ------------------------------------------------------------------ */

static void global_key( int key )
{
    int cmd, letter;

    if ( key == K_MOUSE ) {
        app_mouse();
        return;
    }
    letter = key_alt_letter( key );
    if ( letter == 'C' ) {
        app_command( CMD_CONTENTS );
        return;
    }
    if ( letter == 'N' ) {
        app_command( CMD_NEXT );
        return;
    }
    if ( letter == 'B' ) {
        app_command( CMD_BACK );
        return;
    }
    if ( key == K_F10 || key == K_ALTTAP || letter ) {
        cmd = menu_run( key );
        if ( cmd ) {
            app_command( cmd );
        }
        return;
    }
    switch ( key ) {
    case K_F1:
    case K_SHF1:
        app_command( CMD_HOWTO );
        return;
    case K_F3:
        app_command( CMD_REPEAT );
        return;
    case K_CTRLF1:
        app_command( (key_shift() & SH_SHIFT) ? CMD_PREV : CMD_NEXT );
        return;
    case K_ENTER:
        follow_link();
        return;
    case K_ESC:
        return;
    }
    view_key( key );
}

void help_main( void )
{
    int key, rc, topic = -1;

    sys_init();
    sys_break_install();
    parse_line( sys_cmdline() );
    if ( !find_help_file() ) {
        say( "The help file HELP.HLP was not found.\r\n" );
        sys_exit( 1 );
    }
    rc = hlp_load( hlp_path );
    if ( rc == -1 ) {
        say( "Not enough memory\r\n" );
        sys_exit( 8 );
    }
    if ( rc == -2 ) {
        say( "HELP.HLP has no topics in it.\r\n" );
        sys_exit( 1 );
    }
    if ( rc ) {
        say( "HELP.HLP could not be read: " );
        say( err_text( rc ) );
        say( "\r\n" );
        sys_exit( 1 );
    }
    if ( asked[0] ) {
        topic = hlp_find( asked );
    }
    key_init();
    scr_init( 0, 0 );
    mouse_init();                           /* after the screen has its size */
    ui_ready = 1;
    view.topic = -1;
    if ( topic >= 0 ) {
        view_show( topic );
    } else if ( asked[0] ) {
        app_frame();
        msg_box( "Match not found", MB_OKHELP, CTX_HOWTO );
        view_show( contents_topic() );
    } else {
        view_show( contents_topic() );
    }
    for ( ;; ) {
        app_frame();
        key = key_get();
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        global_key( key );
    }
}
