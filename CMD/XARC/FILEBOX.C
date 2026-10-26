/*
 * FILEBOX.C - the folder and file chooser: DOSForms' FILEBOX.CPP in C.
 *
 * A TREE of the drives and folders, a LIST of the files in the folder
 * the tree's bar is on (not for a folder), a FIELD with a path in it,
 * and OK and Cancel.  The field is the answer; the tree and the list are
 * ways of filling it in.  Enter and OK take what it says, and when that
 * is a folder and a file was wanted, the tree goes there instead.
 *
 *   FB_DIR    a folder, which need not exist: walk to where it should
 *             go and type "\NEW" on the end - Extract's question
 *   FB_SAVE   a file that need not exist - Compress's Browse
 *   FB_OPEN   a file that does
 *
 * Tab and Shift+Tab go round the tree, the file list and the field; the
 * arrows move in whichever list had the keyboard last; + and - (or the
 * arrows across) open and shut the folder under the bar; typing goes to
 * the field and takes the keyboard with it.  A drive is not read until
 * it is opened, so the bar passing over A: spins nothing up.
 *
 * A drive with no disk in it is asked about - "No disk in drive A:",
 * Retry or Cancel - from the critical-error notes the handler takes.
 * The current directory is not moved: everything is read by full path.
 */
#include "xarc.h"

#define FB_H            20
#define FB_W_TREE       60
#define FB_W_BOTH       76
#define FB_TREE_COLS    30
#define FB_PATHLEN      260

enum { AT_TREE, AT_FILES, AT_FIELD };

typedef struct {
    POPUP   box;
    DIRTREE tree;
    DIRSCAN files;
    LIST    tree_list, file_list;
    FIELD   field;
    BUTTON  ok, cancel;
    RECT    tree_box, file_box;
    char    prompt[128];
    char    start[FB_PATHLEN];
    char    name[FB_PATHLEN];
    char    tree_path[FB_PATHLEN];
    char    result[FB_PATHLEN];
    int     mode, box_w, box_h, done, pending_node, pending_file;
    int     focus, last_list, file_sel, drive_open;
} FILEBOX;

static FILEBOX fb;

static int is_sep( char ch )
{
    return ch == '\\' || ch == '/';
}

static int has_files( void )
{
    return fb.mode != FB_DIR;
}

static const char *field_label( void )
{
    return fb.mode == FB_DIR ? "Folder:" : "File:";
}

/* what was typed, fit to hand to DOS: no blanks round it, and never two
   separators together except at the front, where they are a network
   name */
static void tidy_path( char *dst, int size, const char *src )
{
    int used = 0;

    while ( *src == ' ' ) {
        src++;
    }
    for ( ; *src && used < size - 1; src++ ) {
        if ( is_sep( *src ) && used > 1 && is_sep( dst[used - 1] ) ) {
            continue;
        }
        dst[used++] = *src;
    }
    while ( used > 0 && dst[used - 1] == ' ' ) {
        used--;
    }
    dst[used] = 0;
}

static void join_path( char *dst, int size, const char *dir, const char *leaf )
{
    int used = 0;

    if ( size <= 0 ) {
        return;
    }
    for ( ; dir[used] && used < size - 2; used++ ) {
        dst[used] = dir[used];
    }
    if ( used > 0 && !is_sep( dst[used - 1] ) ) {
        dst[used++] = '\\';
    }
    while ( *leaf && used < size - 1 ) {
        dst[used++] = *leaf++;
    }
    dst[used] = 0;
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */

static void place( void )
{
    int rows = df_rows(), cols = df_cols(), want, high, left, top;

    want = has_files() ? FB_W_BOTH : FB_W_TREE;
    if ( (int)strlen( fb.prompt ) + 6 > want ) {
        want = (int)strlen( fb.prompt ) + 6;
    }
    if ( want > cols - 2 ) {
        want = cols - 2;
    }
    if ( want < 30 ) {
        want = 30;
    }
    high = rows - 4;
    if ( high > FB_H + 6 ) {
        high = FB_H + 6;
    }
    if ( high < 12 ) {
        high = 12;
    }
    fb.box_w = want;
    fb.box_h = high;
    left = (cols - want) / 2;
    top = (rows - high) / 2;
    if ( left < 0 ) {
        left = 0;
    }
    if ( top < 0 ) {
        top = 0;
    }
    popup_place( &fb.box, top, left, top + high - 1, left + want - 1 );
}

static void lay_out( void )
{
    int row0 = fb.box.at.row, col0 = fb.box.at.col;
    int btn_row = fb.box.at.end_row - 1, fld_row = fb.box.at.end_row - 3;
    int fld_col = col0 + 4 + (int)strlen( field_label() );
    int start = col0 + (fb.box_w - (6 + 4 + 10)) / 2;

    if ( start < col0 + 1 ) {
        start = col0 + 1;
    }
    fb.tree_box.row = row0 + (fb.prompt[0] ? 2 : 1);
    fb.tree_box.col = col0 + 2;
    fb.tree_box.end_row = fb.box.at.end_row - 4;
    fb.tree_box.end_col = fb.box.at.end_col - 2;
    fb.file_box = fb.tree_box;
    if ( has_files() ) {
        fb.tree_box.end_col = fb.tree_box.col + FB_TREE_COLS - 1;
        fb.file_box.col = fb.tree_box.end_col + 1;
    }
    list_region( &fb.tree_list, fb.tree_box.row, fb.tree_box.col, fb.tree_box.end_row,
                 fb.tree_box.end_col );
    list_region( &fb.file_list, fb.file_box.row, fb.file_box.col, fb.file_box.end_row,
                 fb.file_box.end_col );
    field_place( &fb.field, fld_row, fld_col, fb.box.at.end_col - 2 - fld_col );
    button_init( &fb.ok, FG_WHITE, BG_CYAN, "OK" );
    button_init( &fb.cancel, FG_WHITE, BG_CYAN, "Cancel" );
    button_row( &fb.ok, btn_row, start );
    button_row( &fb.cancel, btn_row, start + 6 + 4 );
}

/* ------------------------------------------------------------------ */
/* the models feed the lists                                           */
/* ------------------------------------------------------------------ */

static int tree_depth( void *user, int row )
{
    (void)user;
    return row >= 0 && row < fb.tree.count ? fb.tree.nodes[row].depth : 0;
}

static int tree_kids( void *user, int row )
{
    (void)user;
    return row >= 0 && row < fb.tree.count && (fb.tree.nodes[row].flags & DT_KIDS);
}

static int tree_is_open( void *user, int row )
{
    (void)user;
    return row >= 0 && row < fb.tree.count && (fb.tree.nodes[row].flags & DT_OPEN);
}

static int tree_is_drive( int row )
{
    return row >= 0 && row < fb.tree.count && (fb.tree.nodes[row].flags & DT_DRIVE);
}

/* a node opened, asking about a drive that will not be read */
static int open_node( int row )
{
    int count;

    for ( ;; ) {
        crit_clear();
        count = dirtree_open( &fb.tree, row );
        if ( !crit_hit() || !crit_ask() ) {
            break;
        }
    }
    return count;
}

static int tree_toggled( void *user, int row )
{
    (void)user;
    if ( tree_is_open( NULL, row ) ) {
        return dirtree_close( &fb.tree, row );
    }
    return open_node( row );
}

static void tree_name( void *user, int row, char *buf, int buflen )
{
    (void)user;
    buf[0] = 0;
    if ( row >= 0 && row < fb.tree.count ) {
        strncpy( buf, fb.tree.nodes[row].name, (size_t)buflen - 1 );
        buf[buflen - 1] = 0;
    }
}

/* the columns to the right of a folder's name: none */
static void tree_tail( void *user, int row, char *buf, int buflen )
{
    (void)user;
    (void)row;
    if ( buflen > 0 ) {
        buf[0] = 0;
    }
}

static int tree_icon( void *user, int row )
{
    (void)user;
    return tree_is_drive( row ) ? icon_drive() : icon_folder();
}

static void tree_twice( void *user, int row )
{
    (void)user;
    fb.pending_node = row;              /* done once the event is over */
}

/* name, size and day; column 0 left for the list's mark or icon */
static void file_text( void *user, int row, char *buf, int buflen )
{
    char size[16], date[20];

    (void)user;
    buf[0] = 0;
    if ( buflen < 40 ) {
        return;
    }
    sprintf( size, "%lu", dirscan_size( &fb.files, row ) );
    dirscan_date( &fb.files, row, date, sizeof( date ) );
    sprintf( buf, " %-12.12s %10.10s  %-10.10s", dirscan_name( &fb.files, row ), size, date );
}

static int file_icon( void *user, int row )
{
    (void)user;
    (void)row;
    return icon_file();
}

static void file_twice( void *user, int row )
{
    (void)user;
    fb.pending_file = row;
}

static int no_mark( void *user, int row )
{
    (void)user;
    (void)row;
    return 0;
}

/* ------------------------------------------------------------------ */
/* reading the disk                                                    */
/* ------------------------------------------------------------------ */

static int reveal_path( const char *path )
{
    int row;

    for ( ;; ) {
        crit_clear();
        row = dirtree_reveal( &fb.tree, path );
        if ( !crit_hit() || !crit_ask() ) {
            break;
        }
    }
    return row;
}

/* the files of the folder the bar is on - but not of a drive that has
   not been opened: passing over A: must not read it */
static void refill_files( void )
{
    int row = fb.tree_list.sel;

    fb.file_sel = -1;
    if ( !has_files() ) {
        return;
    }
    if ( !fb.tree_path[0] || (tree_is_drive( row ) && !tree_is_open( NULL, row )) ) {
        dirscan_empty( &fb.files );
    } else {
        for ( ;; ) {
            crit_clear();
            dirscan_path( &fb.files, fb.tree_path );
            if ( !crit_hit() ) {
                break;
            }
            if ( !crit_ask() ) {
                dirscan_empty( &fb.files );
                break;
            }
        }
    }
    list_data( &fb.file_list, fb.files.count, file_text, &fb );
    fb.file_list.can_mark = no_mark;
    fb.file_list.on_double = file_twice;
    fb.file_list.row_icon = file_icon;
    list_select( &fb.file_list, 0 );
}

/* does "path" exist, and is it a folder?  A drive's root is taken to be
   one without looking: whoever goes there next does the reading. */
static int look( const char *path, int *is_folder )
{
    char bare[FB_PATHLEN];
    struct stat info;
    int len, found;

    *is_folder = 0;
    strncpy( bare, path, sizeof( bare ) - 1 );
    bare[sizeof( bare ) - 1] = 0;
    len = (int)strlen( bare );
    while ( len > 0 && is_sep( bare[len - 1] ) ) {
        bare[--len] = 0;
    }
    if ( len == 0 || (len == 2 && bare[1] == ':') ) {
        *is_folder = 1;
        return 1;
    }
    for ( ;; ) {
        crit_clear();
        found = stat( bare, &info ) == 0;
        if ( !crit_hit() ) {
            break;
        }
        if ( !crit_ask() ) {
            found = 0;
            break;
        }
    }
    if ( !found ) {
        return 0;
    }
    *is_folder = S_ISDIR( info.st_mode );
    return 1;
}

static void compose_field( void )
{
    char text[FB_PATHLEN];

    if ( fb.mode == FB_DIR ) {
        strcpy( text, fb.tree_path );
    } else if ( fb.mode == FB_OPEN ) {
        join_path( text, sizeof( text ), fb.tree_path, "" );
    } else {
        join_path( text, sizeof( text ), fb.tree_path, fb.name );
    }
    field_set( &fb.field, text );
}

static void sync_from_files( int force );

static void fill( void )
{
    char full[FB_PATHLEN];
    int row, name_width = fb.tree_box.end_col - fb.tree_box.col - 2;

    list_data( &fb.tree_list, dirtree_build( &fb.tree ), tree_tail, &fb );
    list_tree( &fb.tree_list, tree_depth, tree_kids, tree_is_open, tree_toggled, tree_name,
               name_width );
    fb.tree_list.can_mark = no_mark;
    fb.tree_list.on_double = tree_twice;
    fb.tree_list.row_icon = tree_icon;
    row = reveal_path( fb.start[0] ? fb.start : "." );
    list_count( &fb.tree_list, fb.tree.count );
    if ( row < 0 ) {
        row = 0;
    }
    list_select( &fb.tree_list, row );
    dirtree_path( &fb.tree, row, fb.tree_path, sizeof( fb.tree_path ) );
    fb.drive_open = tree_is_drive( row ) ? tree_is_open( NULL, row ) : 1;
    refill_files();
    if ( fb.mode == FB_DIR ) {
        /* the field says what was asked for, in full; where the tree got
           all the way there, the tree's spelling of it */
        full[0] = 0;
        if ( fb.start[0] && !_fullpath( full, fb.start, sizeof( full ) ) ) {
            strcpy( full, fb.start );
        }
        if ( !full[0] || stricmp( full, fb.tree_path ) == 0 ) {
            strcpy( full, fb.tree_path );
        }
        field_set( &fb.field, full );
    } else {
        compose_field();
    }
    /* the keyboard starts where the choosing will be done */
    fb.focus = AT_TREE;
    fb.last_list = AT_TREE;
    if ( fb.mode == FB_SAVE ) {
        fb.focus = AT_FIELD;
    }
    if ( fb.mode == FB_OPEN && has_files() && fb.file_list.count > 0 ) {
        fb.focus = AT_FILES;
        fb.last_list = AT_FILES;
        sync_from_files( 1 );
    }
    fb.field.focus = fb.focus == AT_FIELD;
}

/* a list has a solid bar while it has the keyboard, coloured text when
   it has not */
static void paint_focus( void )
{
    fb.tree_list.sel_fg = fb.focus == AT_TREE ? FG_BLACK : FG_BLUE;
    fb.tree_list.sel_bg = fb.focus == AT_TREE ? BG_CYAN : BG_WHITE;
    fb.file_list.sel_fg = fb.focus == AT_FILES ? FG_BLACK : FG_BLUE;
    fb.file_list.sel_bg = fb.focus == AT_FILES ? BG_CYAN : BG_WHITE;
    list_draw( &fb.tree_list );
    if ( has_files() ) {
        list_draw( &fb.file_list );
    }
    field_draw( &fb.field );
}

static void paint( void )
{
    char shown[128];
    int room = fb.box_w - 6;

    if ( fb.prompt[0] && room > 0 ) {
        if ( room > (int)sizeof( shown ) - 1 ) {
            room = (int)sizeof( shown ) - 1;
        }
        strncpy( shown, fb.prompt, (size_t)room );
        shown[room] = 0;
        df_text( fb.box.at.row + 2, fb.box.at.col + 4, shown, FG_BLUE, BG_WHITE );
    }
    df_text( fb.box.at.end_row - 2, fb.box.at.col + 4, field_label(), FG_BLUE, BG_WHITE );
    paint_focus();
    button_draw( &fb.ok );
    button_draw( &fb.cancel );
}

/* ------------------------------------------------------------------ */
/* who has the keyboard                                                */
/* ------------------------------------------------------------------ */

static void set_focus( int where )
{
    if ( where == AT_FILES && (!has_files() || fb.file_list.count < 1) ) {
        where = AT_TREE;
    }
    if ( where == fb.focus ) {
        return;
    }
    fb.focus = where;
    if ( where != AT_FIELD ) {
        fb.last_list = where;
    }
    fb.field.focus = where == AT_FIELD;
    if ( where == AT_FILES ) {
        sync_from_files( 1 );
    }
    paint_focus();
}

/* FB_SAVE: a name typed into the field goes along to the next folder */
static void keep_typed_name( void )
{
    const char *text = fb.field.buffer, *leaf = text, *at;

    if ( !text[0] || stricmp( text, fb.tree_path ) == 0 ) {
        return;
    }
    for ( at = text; *at; at++ ) {
        if ( is_sep( *at ) || *at == ':' ) {
            leaf = at + 1;
        }
    }
    if ( !leaf[0] ) {
        return;
    }
    strncpy( fb.name, leaf, sizeof( fb.name ) - 1 );
    fb.name[sizeof( fb.name ) - 1] = 0;
}

static void sync_from_tree( int force )
{
    char now[FB_PATHLEN];
    int row = fb.tree_list.sel, open = tree_is_drive( row ) ? tree_is_open( NULL, row ) : 1, moved;

    dirtree_path( &fb.tree, row, now, sizeof( now ) );
    if ( !now[0] ) {
        return;
    }
    moved = force || strcmp( now, fb.tree_path ) != 0;
    if ( !moved && open == fb.drive_open ) {
        return;
    }
    if ( moved && fb.mode == FB_SAVE ) {
        keep_typed_name();
    }
    strcpy( fb.tree_path, now );
    fb.drive_open = open;
    if ( has_files() ) {
        refill_files();
        list_draw( &fb.file_list );
    }
    if ( moved ) {
        compose_field();
        field_draw( &fb.field );
    }
}

static void sync_from_files( int force )
{
    char text[FB_PATHLEN];
    int row = fb.file_list.sel;

    if ( fb.mode == FB_DIR || fb.file_list.count < 1 || (!force && row == fb.file_sel) ) {
        return;
    }
    fb.file_sel = row;
    strncpy( fb.name, dirscan_name( &fb.files, row ), sizeof( fb.name ) - 1 );
    fb.name[sizeof( fb.name ) - 1] = 0;
    join_path( text, sizeof( text ), fb.tree_path, fb.name );
    field_set( &fb.field, text );
    field_draw( &fb.field );
}

static void go_to( const char *path )
{
    int row = reveal_path( path );

    list_count( &fb.tree_list, fb.tree.count );
    if ( row >= 0 ) {
        list_select( &fb.tree_list, row );
    }
    field_set( &fb.field, "" );
    sync_from_tree( 1 );
    list_draw( &fb.tree_list );
}

static void accept( void )
{
    char text[FB_PATHLEN], full[FB_PATHLEN];
    int is_folder = 0, there, len;

    tidy_path( text, sizeof( text ), fb.field.buffer );
    if ( !text[0] ) {
        return;
    }
    if ( fb.mode == FB_DIR ) {
        strcpy( fb.result, text );
        fb.done = 1;
        return;
    }
    if ( text[1] == ':' || is_sep( text[0] ) ) {
        strcpy( full, text );
    } else {
        join_path( full, sizeof( full ), fb.tree_path, text );
    }
    there = look( full, &is_folder );
    if ( there && is_folder ) {
        go_to( full );
        return;
    }
    len = (int)strlen( full );
    if ( len < 1 || is_sep( full[len - 1] ) || (fb.mode == FB_OPEN && !there) ) {
        return;
    }
    strcpy( fb.result, full );
    fb.done = 1;
}

static void run_pending( void )
{
    int row;

    if ( fb.pending_node >= 0 ) {
        row = fb.pending_node;
        fb.pending_node = -1;
        list_toggle_node( &fb.tree_list, row );
        sync_from_tree( 0 );
    }
    if ( fb.pending_file >= 0 ) {
        fb.pending_file = -1;
        if ( fb.mode != FB_DIR ) {
            sync_from_files( 1 );
            accept();
        }
    }
}

static void key( int key )
{
    EVENT ev;
    int can_files = has_files() && fb.file_list.count > 0;

    ev.kind = EV_KEY;
    ev.key = key;
    ev.x = ev.y = ev.buttons = ev.press = 0;
    switch ( key ) {
    case KEY_ESC:
        fb.done = -1;
        return;
    case KEY_ENTER:
        accept();
        return;
    case KEY_TAB:
        if ( fb.focus == AT_TREE ) {
            set_focus( can_files ? AT_FILES : AT_FIELD );
        } else if ( fb.focus == AT_FILES ) {
            set_focus( AT_FIELD );
        } else {
            set_focus( AT_TREE );
        }
        return;
    case KEY_SHTAB:
        if ( fb.focus == AT_TREE ) {
            set_focus( AT_FIELD );
        } else if ( fb.focus == AT_FILES ) {
            set_focus( AT_TREE );
        } else {
            set_focus( can_files ? AT_FILES : AT_TREE );
        }
        return;
    case KEY_UP:
    case KEY_DOWN:
    case KEY_PGUP:
    case KEY_PGDN:
        /* nothing to a one-line field: they go to the list that had the
           keyboard last, and take it back */
        if ( fb.focus == AT_FIELD ) {
            set_focus( fb.last_list );
        }
        if ( fb.focus == AT_FILES ) {
            list_event( &fb.file_list, &ev );
            sync_from_files( 0 );
        } else {
            list_event( &fb.tree_list, &ev );
            sync_from_tree( 0 );
        }
        return;
    }
    if ( fb.focus == AT_FIELD ) {
        field_event( &fb.field, &ev );
        return;
    }
    if ( fb.focus == AT_TREE ) {
        if ( key == KEY_LEFT || key == KEY_RIGHT || key == KEY_HOME || key == KEY_END
             || key == '+' || key == '-' ) {
            list_event( &fb.tree_list, &ev );
            sync_from_tree( 0 );
            return;
        }
    } else {
        if ( key == KEY_HOME || key == KEY_END ) {
            list_event( &fb.file_list, &ev );
            sync_from_files( 0 );
            return;
        }
        if ( key == KEY_LEFT ) {
            set_focus( AT_TREE );
            return;
        }
        if ( key == KEY_RIGHT ) {
            return;
        }
    }
    /* typing belongs in the field, and the keyboard goes with it */
    if ( key == KEY_BS || key == KEY_DEL || (key >= 32 && key <= 255 && key != 127) ) {
        set_focus( AT_FIELD );
        field_event( &fb.field, &ev );
    }
}

static int inside( const RECT *box, int x, int y )
{
    return y >= box->row && y <= box->end_row && x >= box->col && x <= box->end_col;
}

static void click( const EVENT *ev )
{
    if ( button_hit( &fb.cancel, ev->x, ev->y ) ) {
        fb.done = -1;
        return;
    }
    if ( button_hit( &fb.ok, ev->x, ev->y ) ) {
        accept();
        return;
    }
    if ( field_hit( &fb.field, ev->x, ev->y ) ) {
        set_focus( AT_FIELD );
        field_event( &fb.field, ev );
        return;
    }
    if ( inside( &fb.tree_box, ev->x, ev->y ) ) {
        set_focus( AT_TREE );
        list_event( &fb.tree_list, ev );
        sync_from_tree( 0 );
        return;
    }
    if ( has_files() && inside( &fb.file_box, ev->x, ev->y ) ) {
        set_focus( AT_FILES );
        list_event( &fb.file_list, ev );
        sync_from_files( 0 );
    }
}

int file_box( const char *title, const char *prompt, int mode, const char *filter,
              const char *start, const char *name, char *out, int size )
{
    EVENT ev;
    int chosen;

    memset( &fb, 0, sizeof( fb ) );
    popup_init( &fb.box, FG_BLUE, BG_WHITE, title );
    fb.box.border = BORDER_DOUBLE;
    fb.box.just = JUST_CENTER;
    list_init( &fb.tree_list, FG_BLACK, BG_WHITE );
    list_init( &fb.file_list, FG_BLACK, BG_WHITE );
    field_init( &fb.field, FG_BRIGHT_WHITE, BG_BLUE );
    field_max( &fb.field, FB_PATHLEN - 1 );
    dirtree_init( &fb.tree );
    dirscan_init( &fb.files );
    fb.files.files_only = 1;
    if ( filter ) {
        strncpy( fb.files.filter, filter, sizeof( fb.files.filter ) - 1 );
    }
    fb.mode = mode;
    fb.pending_node = fb.pending_file = -1;
    fb.drive_open = 1;
    if ( prompt ) {
        strncpy( fb.prompt, prompt, sizeof( fb.prompt ) - 1 );
    }
    if ( start ) {
        strncpy( fb.start, start, sizeof( fb.start ) - 1 );
    }
    if ( name ) {
        strncpy( fb.name, name, sizeof( fb.name ) - 1 );
    }

    /* the disk is read BEFORE anything is painted, so the box arrives
       whole rather than as an empty frame waiting on a drive */
    place();
    lay_out();
    fill();
    popup_draw( &fb.box );
    paint();

    while ( !fb.done ) {
        ev_wait( &ev );
        if ( ev.kind == EV_KEY ) {
            key( ev.key );
        } else if ( ev.kind == EV_MOUSE && (ev.press & LEFT_BUTTON) ) {
            click( &ev );
        } else {
            continue;
        }
        run_pending();
    }
    popup_restore( &fb.box );
    popup_free( &fb.box );
    chosen = fb.done == 1 && fb.result[0];
    if ( chosen ) {
        strncpy( out, fb.result, (size_t)size - 1 );
        out[size - 1] = 0;
    }
    list_free( &fb.tree_list );
    list_free( &fb.file_list );
    dirtree_free( &fb.tree );
    dirscan_free( &fb.files );
    return chosen;
}
