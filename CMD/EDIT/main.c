/*
 * MAIN.C - EDIT: the command line, the windows, the commands and the
 * loop that runs them.
 *
 *   EDIT [/B] [/H] [/R] [/S] [/nnn] [/?] [file...]
 *
 * THE MODEL IS DOS 7's EDIT - the MS-DOS Editor of Windows 95 and 98:
 * a menu bar, up to nine files open with a window onto one of them or
 * two, a status bar with the line and column, the File, Edit, Search,
 * View, Options and Help menus, their keys, the WordStar keys, and the
 * same switches.  Its words are this program's own.
 *
 * ONE LOOP: draw everything (app_draw), put the cursor in the active
 * window, flush, read a key, and either run a command with it or give
 * it to the window.  Menus and dialogs have loops of their own that
 * draw the same way and return a command or an answer.
 *
 * THE MOUSE comes through the same loop as K_MOUSE (see SCR.C), and a
 * press starts a little loop of its own for as long as the button is
 * held: selecting as it drags, repeating a scroll arrow, moving the
 * line between two windows (app_mouse).
 */
#include "edit.h"

DOC  *docs[MAX_DOCS];
int   ndocs;
VIEW  views[2];
int   nviews;
int   active;
int   short_names;
int   split_at;                 /* the top window's height when split */
char  print_port[8] = "PRN";

static char       *clip;
static u32         clip_len;
static const char *status_hint;
static const char  status_text[] =
    "PM-DOS Editor   F1=Help   F10 or Alt=Menus   F6=Next Window";
static int         next_untitled = 1;
static int         opt_binary, opt_readonly, opt_mono, opt_lines;
static int         ui_ready;
static char        pending[160];    /* a complaint from the command line */

static const char usage[] =
    "Edits text files.\r\n"
    "\r\n"
    "EDIT [/B] [/H] [/R] [/S] [/nnn] [/?] [file...]\r\n"
    "\r\n"
    "  /B     Uses black and white: for a monochrome screen.\r\n"
    "  /H     Shows as many lines as the display can: 43 or 50.\r\n"
    "  /R     Opens the files read-only: they can be looked at and not changed.\r\n"
    "  /S     Shows files by their short (8.3) names.\r\n"
    "  /nnn   Opens binary files, showing nnn bytes to a line.\r\n"
    "  file   The files to open - up to nine; wildcards are allowed.\r\n";

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
        msg_box( "There is not enough memory to do that.", MB_OK );
    } else {
        sys_stdout( "Not enough memory\r\n", 19 );
    }
}

void app_status( const char *text )
{
    status_hint = text;
}

DOC *app_doc( void )
{
    return views[active].doc;
}

VIEW *app_view( void )
{
    return &views[active];
}

int clip_has( void )
{
    return clip != NULL && clip_len > 0;
}

void clip_set( char *text, u32 len )
{
    xfree( clip );
    clip = text;
    clip_len = len;
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

void app_layout( void )
{
    int total = scr_rows - 2, index;

    if ( nviews == 1 ) {
        views[0].row = 1;
        views[0].height = total;
    } else {
        if ( split_at > total - 3 ) {
            split_at = total - 3;
        }
        if ( split_at < 3 ) {
            split_at = 3;
        }
        views[0].row = 1;
        views[0].height = split_at;
        views[1].row = 1 + split_at;
        views[1].height = total - split_at;
    }
    for ( index = 0; index < nviews; index++ ) {
        view_keep_visible( &views[index] );
    }
}

void app_draw( void )
{
    char right[40], num[12];
    const char *left;
    int index, len, row = scr_rows - 1;
    VIEW *view = &views[active];

    menu_draw_bar( -1, 1 );
    for ( index = 0; index < nviews; index++ ) {
        view_draw( &views[index], index == active );
    }
    scr_fill( row, 0, scr_cols, ' ', pal.status );
    left = status_hint ? status_hint : status_text;
    scr_put( row, 1, left, pal.status );
    if ( (int)str_len( left ) > scr_cols - 22 ) {
        return;                             /* a long hint has the whole line */
    }
    str_cpy( right, " Line:" );
    str_catn( right, fmt_dec( view->line + 1, 0, num ), sizeof( right ) );
    str_catn( right, "  Col:", sizeof( right ) );
    str_catn( right, fmt_dec( view->col + 1, 0, num ), sizeof( right ) );
    str_catn( right, " ", sizeof( right ) );
    len = (int)str_len( right );
    scr_ch( row, scr_cols - 20, 0xB3, pal.status );
    scr_put( row, scr_cols - 19, "                   ", pal.status );
    scr_put( row, scr_cols - 1 - len, right, pal.status );
}

/* ------------------------------------------------------------------ */
/* documents and windows                                               */
/* ------------------------------------------------------------------ */

static void remember( VIEW *view )
{
    if ( view->doc ) {
        view->doc->at_line = view->line;
        view->doc->at_col = view->col;
        view->doc->at_top = view->top;
        view->doc->at_left = view->left;
    }
}

void app_show_doc( VIEW *view, DOC *doc )
{
    remember( view );
    view->doc = doc;
    view->line = doc->at_line < doc->count ? doc->at_line : doc->count - 1;
    view->col = doc->at_col;
    view->top = doc->at_top < doc->count ? doc->at_top : 0;
    view->left = doc->at_left;
    view->want = view->col;
    view->sel = 0;
    view_keep_visible( view );
}

static int add_doc( DOC *doc )
{
    if ( ndocs == MAX_DOCS ) {
        return 0;
    }
    docs[ndocs++] = doc;
    return 1;
}

static DOC *new_untitled( void )
{
    DOC *doc;
    char num[12];

    if ( ndocs == MAX_DOCS ) {
        msg_box( "Nine files are open already: close one first.", MB_OK );
        return NULL;
    }
    doc = doc_new();
    if ( doc == NULL ) {
        out_of_memory();
        return NULL;
    }
    doc->untitled = next_untitled++;
    str_cpy( doc->name, "UNTITLED" );
    str_catn( doc->name, fmt_dec( (u32)doc->untitled, 0, num ), sizeof( doc->name ) );
    add_doc( doc );
    return doc;
}

static int find_doc( const char *full )
{
    int index;

    for ( index = 0; index < ndocs; index++ ) {
        if ( docs[index]->path[0] && str_icmp( docs[index]->path, full ) == 0 ) {
            return index;
        }
    }
    return -1;
}

/* the full path of a name, as 7160h has it - or the name as it is */
static void full_name( const char *path, char *out )
{
    if ( sys_truename( path, out, 2 ) != 0 && sys_truename( path, out, 0 ) != 0 ) {
        str_cpyn( out, path, PATH_MAX );
    }
}

/* open "path" in the active window.  From the command line a name that
   is not there is a new file of that name; from the dialog it is an
   error.  1 = the window shows it now. */
static int open_file( const char *path, int from_cmdline )
{
    char full[PATH_MAX];
    DOC *doc;
    int rc, index;

    full_name( path, full );
    index = find_doc( full );
    if ( index >= 0 ) {
        if ( ui_ready ) {
            app_show_doc( &views[active], docs[index] );
        }
        return 1;
    }
    if ( ndocs == MAX_DOCS ) {
        if ( ui_ready ) {
            msg_box( "Nine files are open already: close one first.", MB_OK );
        }
        return 0;
    }
    doc = doc_new();
    if ( doc == NULL ) {
        out_of_memory();
        return 0;
    }
    doc->binary = opt_binary;
    doc->readonly = opt_readonly;
    rc = doc_load( doc, full );
    if ( rc == ERR_NOFILE && from_cmdline ) {
        doc_set_path( doc, full );              /* a new file, by name */
        rc = 0;
    }
    if ( rc != 0 ) {
        doc_free( doc );
        if ( ui_ready ) {
            msg_box2( path, err_text( rc ), MB_OK );
        } else if ( pending[0] == 0 ) {
            str_cpyn( pending, path, sizeof( pending ) );
            str_catn( pending, "\n", sizeof( pending ) );
            str_catn( pending, err_text( rc ), sizeof( pending ) );
        }
        return 0;
    }
    add_doc( doc );
    if ( ui_ready ) {
        app_show_doc( &views[active], doc );
    }
    if ( doc->split ) {
        if ( ui_ready ) {
            msg_box2( "Lines longer than 4096 characters have been split.",
                      "Saving the file will keep them split.", MB_OK );
        } else if ( pending[0] == 0 ) {
            str_cpyn( pending, doc->name, sizeof( pending ) );
            str_catn( pending, ": lines longer than 4096 characters\nhave been split.",
                      sizeof( pending ) );
        }
    }
    return 1;
}

int app_open_file( const char *path )
{
    return open_file( path, 0 );
}

/* 1 = saved (or there was nothing to save) */
int app_save( DOC *doc, int ask_name )
{
    char path[PATH_MAX];
    u16 attr;
    int rc;

    if ( ask_name || doc->path[0] == 0 || doc->readonly ) {
        str_cpy( path, doc->path );
        if ( !dlg_saveas( doc, path ) ) {
            return 0;
        }
        if ( str_icmp( path, doc->path ) != 0 && sys_get_attr( path, &attr ) == 0 ) {
            if ( attr & ATTR_DIR ) {
                msg_box2( path, "is a directory.", MB_OK );
                return 0;
            }
            if ( msg_box2( path, "exists already.  Replace it?", MB_YESNO ) != ID_YES ) {
                return 0;
            }
        }
        if ( find_doc( path ) >= 0 && docs[find_doc( path )] != doc ) {
            msg_box2( path, "is open in another window: close it first.", MB_OK );
            return 0;
        }
    } else {
        str_cpy( path, doc->path );
    }
    rc = doc_save( doc, path );
    if ( rc != 0 ) {
        msg_box2( path, err_text( rc ), MB_OK );
        return 0;
    }
    doc->readonly = 0;
    doc->untitled = 0;
    return 1;
}

/* a file with changes: save them, lose them, or stop.  0 = stop */
static int ask_save( DOC *doc )
{
    int answer;

    if ( !doc->modified ) {
        return 1;
    }
    answer = msg_box2( doc->name, "has changes that are not saved.  Save them now?",
                       MB_YESNOCANCEL );
    if ( answer == ID_YES ) {
        return app_save( doc, 0 );
    }
    return answer == ID_NO;
}

static void close_doc( DOC *doc )
{
    int index, at = -1;
    DOC *next;

    if ( !ask_save( doc ) ) {
        return;
    }
    for ( index = 0; index < ndocs; index++ ) {
        if ( docs[index] == doc ) {
            at = index;
        }
    }
    if ( at < 0 ) {
        return;
    }
    for ( index = at; index + 1 < ndocs; index++ ) {
        docs[index] = docs[index + 1];
    }
    ndocs--;
    next = ndocs ? docs[at < ndocs ? at : ndocs - 1] : new_untitled();
    for ( index = 0; index < nviews; index++ ) {
        if ( views[index].doc == doc ) {
            views[index].doc = NULL;            /* nothing to remember */
            app_show_doc( &views[index], next );
        }
    }
    doc_free( doc );
}

static void app_exit( void )
{
    int index;

    for ( index = 0; index < ndocs; index++ ) {
        if ( docs[index]->modified ) {
            app_show_doc( &views[active], docs[index] );
            if ( !ask_save( docs[index] ) ) {
                return;
            }
        }
    }
    scr_done();
    sys_exit( 0 );
}

void app_abandon( void )
{
    scr_done();
    sys_exit( 0 );
}

/* ------------------------------------------------------------------ */
/* the commands                                                        */
/* ------------------------------------------------------------------ */

int app_can( int cmd )
{
    VIEW *view = &views[active];

    switch ( cmd ) {
    case CMD_CUT:
    case CMD_CLEAR:
        return view_has_sel( view ) && !view->doc->readonly;
    case CMD_COPY:
        return view_has_sel( view );
    case CMD_PASTE:
        return clip_has() && !view->doc->readonly;
    case CMD_REPEAT:
        return find_can_repeat();
    case CMD_REPLACE:
        return !view->doc->readonly;
    case CMD_SIZE:
        return nviews == 2;
    }
    if ( cmd >= CMD_DOC1 ) {
        return cmd - CMD_DOC1 < ndocs;
    }
    return 1;
}

static void copy_sel( int cut )
{
    VIEW *view = &views[active];
    POS from, to;
    char *text;
    u32 len;

    if ( !view_has_sel( view ) ) {
        return;
    }
    view_sel_range( view, &from, &to );
    text = doc_extract( view->doc, from, to, &len );
    if ( text == NULL ) {
        out_of_memory();
        return;
    }
    clip_set( text, len );
    if ( cut ) {
        view_delete_sel( view );
    }
}

static void split( void )
{
    if ( nviews == 2 ) {                        /* back to one: the active one */
        remember( &views[active == 0 ? 1 : 0] );
        if ( active == 1 ) {
            views[0] = views[1];
        }
        nviews = 1;
        active = 0;
        app_layout();
        return;
    }
    if ( scr_rows - 2 < 6 ) {
        return;
    }
    views[1] = views[0];
    nviews = 2;
    split_at = (scr_rows - 2) / 2;
    active = 1;
    app_layout();
}

void app_command( int cmd )
{
    VIEW *view = &views[active];
    char path[PATH_MAX];
    DOC *doc;

    switch ( cmd ) {
    case CMD_NEW:
        doc = new_untitled();
        if ( doc ) {
            app_show_doc( view, doc );
        }
        break;
    case CMD_OPEN:
        path[0] = 0;
        if ( dlg_open( path ) ) {
            open_file( path, 0 );
        }
        break;
    case CMD_SAVE:
        app_save( view->doc, 0 );
        break;
    case CMD_SAVEAS:
        app_save( view->doc, 1 );
        break;
    case CMD_CLOSE:
        close_doc( view->doc );
        break;
    case CMD_PRINT:
        dlg_print();
        break;
    case CMD_EXIT:
        app_exit();
        break;
    case CMD_CUT:
        if ( !view->doc->readonly ) {
            copy_sel( 1 );
        }
        break;
    case CMD_COPY:
        copy_sel( 0 );
        break;
    case CMD_PASTE:
        if ( clip_has() ) {
            view_insert_text( view, clip, clip_len );
        }
        break;
    case CMD_CLEAR:
        if ( !view->doc->readonly ) {
            view_delete_sel( view );
        }
        break;
    case CMD_FIND:
        dlg_find( 0 );
        break;
    case CMD_REPEAT:
        find_again();
        break;
    case CMD_REPLACE:
        if ( !view->doc->readonly ) {
            dlg_find( 1 );
        }
        break;
    case CMD_SPLIT:
        split();
        break;
    case CMD_SIZE:
        if ( nviews == 2 ) {
            dlg_size_window();
            app_layout();
        }
        break;
    case CMD_CLOSEWIN:
        if ( nviews == 2 ) {
            active = active == 0 ? 1 : 0;       /* keep the other one */
            split();
        } else {
            close_doc( view->doc );
        }
        break;
    case CMD_OUTPUT:
        scr_show_dos();
        break;
    case CMD_SETTINGS:
        dlg_settings();
        break;
    case CMD_COLORS:
        dlg_colors();
        break;
    case CMD_HELPCMDS:
        help_show( HELP_INDEX );
        break;
    case CMD_HELPKEYS:
        help_show( HELP_KEYS );
        break;
    case CMD_ABOUT:
        dlg_about();
        break;
    default:
        if ( cmd >= CMD_DOC1 && cmd - CMD_DOC1 < ndocs ) {
            app_show_doc( view, docs[cmd - CMD_DOC1] );
        }
        break;
    }
    app_layout();
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

static int blank( int ch )
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void bad_switch( const char *start, u32 len )
{
    char text[60];

    str_cpy( text, "Invalid switch - /" );
    if ( len > 30 ) {
        len = 30;
    }
    mem_cpy( text + 18, start, len );
    text[18 + len] = 0;
    str_catn( text, "\r\n", sizeof( text ) );
    sys_stdout( text, str_len( text ) );
    sys_exit( 1 );
}

static void one_switch( const char *sw, u32 len )
{
    char digits[8];
    u32 width;
    int ok;

    if ( len == 1 ) {
        switch ( ch_upper( sw[0] ) ) {
        case 'B': opt_mono = 1;     return;
        case 'H': opt_lines = 50;   return;
        case 'R': opt_readonly = 1; return;
        case 'S': short_names = 1;  return;
        case '?':
            sys_stdout( usage, str_len( usage ) );
            sys_exit( 0 );
        }
    }
    if ( len >= 1 && len <= 4 && sw[0] >= '0' && sw[0] <= '9' ) {
        mem_cpy( digits, sw, len );
        digits[len] = 0;
        width = dec_parse( digits, &ok );
        if ( ok && width >= 10 && width <= 1024 ) {
            opt_binary = (int)width;
            return;
        }
    }
    bad_switch( sw, len );
}

/* one name from the command line: every file a wildcard matches */
static void open_arg( const char *arg )
{
    char dir[PATH_MAX], path[PATH_MAX];
    const char *last = arg, *scan;
    FINDREC rec;
    u32 handle, len;

    for ( scan = arg; *scan; scan++ ) {
        if ( *scan == '\\' || *scan == '/' || (*scan == ':' && scan == arg + 1) ) {
            last = scan + 1;
        }
        if ( *scan == '*' || *scan == '?' ) {
            break;
        }
    }
    if ( *scan == 0 ) {
        open_file( arg, 1 );
        return;
    }
    len = (u32)(last - arg);
    mem_cpy( dir, arg, len );
    dir[len] = 0;
    if ( sys_find_first( arg, ATTR_READONLY | ATTR_ARCHIVE, &rec, &handle ) != 0 ) {
        if ( pending[0] == 0 ) {
            str_cpyn( pending, arg, sizeof( pending ) );
            str_catn( pending, "\nNo files match it.", sizeof( pending ) );
        }
        return;
    }
    do {
        if ( !(rec.attr & ATTR_DIR) && ndocs < MAX_DOCS ) {
            str_cpyn( path, dir, sizeof( path ) );
            str_catn( path, short_names ? rec.alias : rec.name, sizeof( path ) );
            open_file( path, 1 );
        }
    } while ( sys_find_next( handle, &rec ) == 0 );
    sys_find_close( handle );
}

static void parse_line( const char *line )
{
    char arg[PATH_MAX];
    const char *start;
    u32 used;
    int quoted;

    for ( ;; ) {
        while ( blank( *line ) ) {
            line++;
        }
        if ( *line == 0 ) {
            return;
        }
        if ( *line == '/' ) {
            start = ++line;
            while ( *line && !blank( *line ) && *line != '/' ) {
                line++;
            }
            one_switch( start, (u32)(line - start) );
            continue;
        }
        used = 0;
        quoted = 0;
        while ( *line && (quoted || (!blank( *line ) && *line != '/')) ) {
            if ( *line == '"' ) {
                quoted = !quoted;
            } else if ( used < PATH_MAX - 1 ) {
                arg[used++] = *line;
            }
            line++;
        }
        arg[used] = 0;
        if ( used ) {
            open_arg( arg );
        }
    }
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

static void app_frame( void )
{
    app_draw();
    view_place_cursor( &views[active] );
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

/* pressed in the text: the cursor goes there, and dragging selects from
   there - past the top or the bottom of the window, which scrolls it */
static void text_drag( VIEW *view )
{
    int key, row, col, rows = view_text_rows( view ), first = view->row + 1;

    for ( ;; ) {
        key = held_next( 8 );
        if ( key == K_MOUSE ) {
            if ( mouse.kind == ME_UP ) {
                return;
            }
            if ( mouse.kind == ME_DRAG ) {
                view_mouse_to( view, mouse.row, mouse.col, 1 );
            }
        } else if ( key == K_TICK ) {
            mouse_where( &row, &col );
            if ( row < first && view->top > 0 ) {
                view->top--;
                view_mouse_to( view, first, col, 1 );
            } else if ( row >= first + rows && view->top + (u32)rows < view->doc->count ) {
                view->top++;
                view_mouse_to( view, first + rows - 1, col, 1 );
            }
        }
    }
}

/* a scroll bar: its arrows and its track again and again while the button
   stays on them, and its box wherever it is dragged */
static void scroll_held( VIEW *view, int part )
{
    int key, row, col, delay = 40, thumb = part == VH_VTHUMB || part == VH_HTHUMB;

    view_scroll_part( view, part, mouse.row, mouse.col );
    for ( ;; ) {
        key = held_next( delay );
        if ( key == K_MOUSE ) {
            if ( mouse.kind == ME_UP ) {
                return;
            }
            if ( mouse.kind == ME_DRAG && thumb ) {
                view_scroll_part( view, part, mouse.row, mouse.col );
            }
        } else if ( key == K_TICK && !thumb ) {
            mouse_where( &row, &col );
            if ( view_hit( view, row, col ) == part ) {
                view_scroll_part( view, part, row, col );
            }
            delay = 5;
        }
    }
}

/* the lower window's title bar, dragged: the line between the two */
static void split_drag( void )
{
    int key;

    for ( ;; ) {
        key = held_next( -1 );
        if ( key == K_MOUSE ) {
            if ( mouse.kind == ME_UP ) {
                return;
            }
            if ( mouse.kind == ME_DRAG ) {
                split_at = mouse.row - 1;
                app_layout();
            }
        }
    }
}

/* the status bar's own words, clicked, are the keys they name */
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
    VIEW *view;
    int index, part, cmd;

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
        if ( status_word( "F1=Help", mouse.col ) ) {
            app_command( CMD_HELPKEYS );
        } else if ( status_word( "F10 or Alt=Menus", mouse.col ) ) {
            cmd = menu_run( K_F10 );
            if ( cmd ) {
                app_command( cmd );
            }
        } else if ( status_word( "F6=Next Window", mouse.col ) && nviews == 2 ) {
            active = active == 0 ? 1 : 0;
        }
        return;
    }
    for ( index = 0; index < nviews; index++ ) {
        view = &views[index];
        if ( mouse.row < view->row || mouse.row >= view->row + view->height ) {
            continue;
        }
        active = index;
        part = view_hit( view, mouse.row, mouse.col );
        if ( part == VH_TITLE ) {
            if ( index == 1 ) {
                split_drag();
            }
        } else if ( part == VH_TEXT ) {
            if ( mouse.kind == ME_DOUBLE ) {
                view_mouse_to( view, mouse.row, mouse.col, 0 );
                view_select_word( view );
            } else {
                view_mouse_to( view, mouse.row, mouse.col, (mouse.shift & SH_SHIFT) != 0 );
                text_drag( view );
            }
        } else if ( part != VH_NONE ) {
            scroll_held( view, part );
        }
        return;
    }
}

/* ------------------------------------------------------------------ */

static void global_key( int key )
{
    int cmd;

    if ( key == K_MOUSE ) {
        app_mouse();
        return;
    }
    if ( key == K_F10 || key == K_ALTTAP || key_alt_letter( key ) ) {
        cmd = menu_run( key );
        if ( cmd ) {
            app_command( cmd );
        }
        return;
    }
    switch ( key ) {
    case K_F1:          app_command( CMD_HELPKEYS ); return;
    case K_F3:          app_command( CMD_REPEAT );   return;
    case K_F4:          app_command( CMD_OUTPUT );   return;
    case K_CTRLF4:      app_command( CMD_CLOSEWIN ); return;
    case K_CTRLF6:      app_command( CMD_SPLIT );    return;
    case K_CTRLF8:      app_command( CMD_SIZE );     return;
    case K_DEL | K_SHIFT: app_command( CMD_CUT );    return;
    case K_CINS:        app_command( CMD_COPY );     return;
    case K_INS | K_SHIFT: app_command( CMD_PASTE );  return;
    case K_F6:
        if ( nviews == 2 ) {
            active = active == 0 ? 1 : 0;
        }
        return;
    }
    view_key( &views[active], key );
}

void edit_main( void )
{
    int key;

    sys_init();
    sys_break_install();
    parse_line( sys_cmdline() );
    key_init();
    scr_init( opt_mono, opt_lines );
    mouse_init();                           /* after the screen has its size */
    ui_ready = 1;
    if ( ndocs == 0 ) {
        new_untitled();
    }
    if ( ndocs == 0 ) {                         /* not even one: no memory */
        scr_done();
        sys_stdout( "Not enough memory\r\n", 19 );
        sys_exit( 8 );
    }
    nviews = 1;
    active = 0;
    app_show_doc( &views[0], docs[0] );
    app_layout();
    if ( pending[0] ) {
        char *brk = str_chr( pending, '\n' );
        if ( brk ) {
            *brk = 0;
        }
        msg_box2( pending, brk ? brk + 1 : NULL, MB_OK );
    }
    for ( ;; ) {
        app_draw();
        view_place_cursor( &views[active] );
        scr_flush();
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
