/*
 * MAIN.C - DOSSHELL: the command line, the lists on the screen, and
 * the loop that runs them.
 *
 *   DOSSHELL [/T[:res]] [/G[:res]] [/256] [/?]
 *
 * THE MODEL IS THE MS-DOS SHELL of DOS 5 and 6: a title, a menu bar, a
 * line of drives, a tree of the drive's directories beside the files
 * of the one that is selected, a list of programs in groups, and a
 * status bar - shown as text or drawn in a graphics mode.  Its menus,
 * its keys and its views are that program's; its words are this one's
 * own, and so is every line of it: a native PM-DOS program, flat and
 * 32-bit, that reaches the screen, the disk and other programs through
 * the kernel.  What that buys is long file names in every list, a
 * 640x480 screen in 256 colours where the video BIOS has VBE 2.0, and
 * no swapping to disk to make room for a program - the kernel gives
 * each one a slot of its own.  What it costs is the Task Swapper,
 * which was the MS-DOS Shell freezing one DOS program to run another:
 * PM-DOS runs one program at a time, and nothing here pretends
 * otherwise.
 *
 * ONE LOOP, as in EDIT: read again whatever list has gone stale, draw
 * everything (app_draw), flush, read a key, and either run a command
 * with the key or give the key to the list that has the focus.  Menus
 * and dialogs have loops of their own that draw the same way.
 *
 * A FILE WINDOW (FWIN) is a drive, the directory selected in its tree
 * and that directory's files; there is one, or two in the Dual File
 * Lists view.  "Stale" on a window means its files are to be read
 * again, which the loop does - never the drawing code, because reading
 * a disk puts a box on the screen and that draws.
 */
#include "shell.h"

#define TREE_W          32      /* columns the tree's side of a window takes */

OPTIONS opt;
FWIN    wins[2];
int     nwins;
int     focus_win, focus_area;

static const char *status_hint;
static int  running = 1;
static int  ui_ready;
static int  prog_head = -1, prog_first, prog_last;  /* the program list's lines */
static int  drives[26], ndrives;                    /* the letters there are */
static int  drop_win = -1, drop_node = -1, drop_drive = -1; /* where a drag points */

static const char usage[] =
    "Starts the PM-DOS Shell: files, directories and programs in lists\r\n"
    "and menus.\r\n"
    "\r\n"
    "DOSSHELL [/T[:res]] [/G[:res]] [/256]\r\n"
    "\r\n"
    "  /T[:res]  Starts in text mode.  res is L for 25 lines or H for 50\r\n"
    "            (43 on an EGA).\r\n"
    "  /G[:res]  Starts in graphics mode.  res is L for 25 lines, M1 for\r\n"
    "            30, M2 for 34, H1 for 43 or H2 for 60.  L and H1 are\r\n"
    "            640x350 and run on an EGA; the others are 640x480 and\r\n"
    "            need a VGA.\r\n"
    "  /256      With /G:M1, /G:M2 or /G:H2: 256 colors.  Needs VBE 2.0.\r\n"
    "\r\n"
    "With no switch the shell starts the way it was last left.\r\n";

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
    case 16:            return "The directory is not empty, or is in use.";
    case 17:            return "The two names are on different drives.";
    case ERR_EXISTS:    return "A file with that name already exists.";
    case ERR_DISKFULL:  return "The disk is full.";
    case ERR_WRPROTECT: return "The disk is write-protected.";
    case ERR_NOTREADY:  return "The drive is not ready.";
    case 32:            return "The file is in use.";
    case ERR_NOMEM:
    case -1:            return "There is not enough memory.";
    }
    return "The disk could not be read or written.";
}

void out_of_memory( void )
{
    if ( ui_ready ) {
        scr_done();
    }
    sys_stdout( "DOSSHELL: not enough memory\r\n", 29 );
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

int win_ok( const FWIN *win )
{
    return win->tree != NULL && win->tree->valid;
}

FWIN *app_win( void )
{
    return &wins[focus_area == AREA_PROGS || focus_win >= nwins ? 0 : focus_win];
}

int app_files_menu( void )
{
    return focus_area != AREA_PROGS;
}

static int win_rows( const FWIN *win )
{
    return win->row_bot - win->row_top + 1;
}

/* a list's first line moved so that line "cur" shows */
static void list_show( int *top, int cur, int rows )
{
    if ( rows < 1 ) {
        rows = 1;
    }
    if ( cur < *top ) {
        *top = cur;
    }
    if ( cur >= *top + rows ) {
        *top = cur - rows + 1;
    }
    if ( *top < 0 ) {
        *top = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Select Across Directories                                           */
/*                                                                     */
/* A file window holds one directory's files, so a file selected in    */
/* another directory has to be remembered somewhere else: here, by its */
/* whole path, from when its directory stops showing until that        */
/* directory shows again or a command uses the selection.              */
/* ------------------------------------------------------------------ */

static char **kept;
static int    nkept, kept_cap;

int kept_count( void )
{
    return nkept;
}

const char *kept_path( int index )
{
    return kept[index];
}

void kept_clear( void )
{
    while ( nkept ) {
        xfree( kept[--nkept] );
    }
}

static void kept_add( const char *path )
{
    char **bigger;
    int index;

    for ( index = 0; index < nkept; index++ ) {
        if ( str_icmp( kept[index], path ) == 0 ) {
            return;
        }
    }
    if ( nkept == kept_cap ) {
        bigger = (char **)xalloc( (u32)(kept_cap + 64) * sizeof( char * ) );
        if ( nkept ) {
            mem_cpy( bigger, kept, (u32)nkept * sizeof( char * ) );
        }
        xfree( kept );
        kept = bigger;
        kept_cap += 64;
    }
    kept[nkept++] = str_dup( path );
}

/* 1 if "path" was kept - and it is kept no longer */
static int kept_take( const char *path )
{
    int index;

    for ( index = 0; index < nkept; index++ ) {
        if ( str_icmp( kept[index], path ) == 0 ) {
            xfree( kept[index] );
            kept[index] = kept[--nkept];
            return 1;
        }
    }
    return 0;
}

/* the window's selected files, before its list is given up */
static void keep_selection( FWIN *win )
{
    char path[PATH_MAX];
    int index;

    if ( !opt.select_across || !win_ok( win ) ) {
        return;
    }
    for ( index = 0; index < win->files.count; index++ ) {
        if ( win->files.ent[index].sel ) {
            flist_path( win->tree, &win->files.ent[index], path );
            kept_add( path );
        }
    }
}

/* ------------------------------------------------------------------ */
/* the drives                                                          */
/* ------------------------------------------------------------------ */

static void drives_find( void )
{
    int drive;

    ndrives = 0;
    for ( drive = 0; drive < 26; drive++ ) {
        if ( sys_drive_kind( drive ) != DK_NONE ) {
            drives[ndrives++] = drive;
        }
    }
}

static int drive_width( void )
{
    int width = gfx_on ? 7 : 5;

    return ndrives * width > scr_cols - 2 ? 3 : width;
}

/* which of drives[] is at a column of the drive line, or -1 */
static int drive_at( int col )
{
    int width = drive_width(), index = (col - 1) / width;

    if ( col < 1 || index >= ndrives || (col - 1) % width >= width - 1 ) {
        return -1;
    }
    return index;
}

static int drive_icon( int drive )
{
    switch ( sys_drive_kind( drive ) ) {
    case DK_FLOPPY: return IC_FLOPPY;
    case DK_CDROM:  return IC_CDROM;
    case DK_OTHER:  return IC_OTHER;
    }
    return IC_FIXED;
}

/* ------------------------------------------------------------------ */
/* a file window                                                       */
/* ------------------------------------------------------------------ */

static void sel_clear( FWIN *win )
{
    int index;

    for ( index = 0; index < win->files.count; index++ ) {
        win->files.ent[index].sel = 0;
    }
    flist_count( &win->files );
}

static void sel_only( FWIN *win, int index )
{
    sel_clear( win );
    if ( index >= 0 && index < win->files.count ) {
        win->files.ent[index].sel = 1;
        win->anchor = index;
    }
    flist_count( &win->files );
}

static void sel_range( FWIN *win, int from, int to )
{
    int index, low = from < to ? from : to, high = from < to ? to : from;

    if ( !win->add_mode ) {
        sel_clear( win );
    }
    for ( index = low; index <= high; index++ ) {
        if ( index >= 0 && index < win->files.count ) {
            win->files.ent[index].sel = 1;
        }
    }
    flist_count( &win->files );
}

/* The window's files, read now.  A search's answer is asked for again;
   All Files is the whole drive. */
void win_reload( FWIN *win )
{
    char path[PATH_MAX];
    int rc, index;

    win->stale = 0;
    if ( !win_ok( win ) ) {
        flist_free( &win->files );
        return;
    }
    if ( win->node >= win->tree->count ) {
        win->node = 0;
    }
    if ( win->search == 2 ) {
        rc = flist_read_all( &win->files, win->tree, win->search_for, 0 );
    } else if ( win->search ) {
        rc = flist_read_dir( &win->files, win->tree, win->node, win->search_for );
    } else if ( opt.view == VIEW_ALL ) {
        rc = flist_read_all( &win->files, win->tree, opt.filter, 0 );
    } else {
        rc = flist_read_dir( &win->files, win->tree, win->node, opt.filter );
    }
    if ( rc ) {
        msg_error( "The files could not all be read.", rc );
    }
    for ( index = 0; nkept && index < win->files.count; index++ ) {
        flist_path( win->tree, &win->files.ent[index], path );
        if ( kept_take( path ) ) {
            win->files.ent[index].sel = 1;
        }
    }
    flist_count( &win->files );
    if ( win->cur >= win->files.count ) {
        win->cur = win->files.count ? win->files.count - 1 : 0;
    }
    list_show( &win->top, win->cur, win_rows( win ) );
    if ( win->top > win->cur ) {
        win->top = win->cur;
    }
}

void win_set_node( FWIN *win, int node )
{
    if ( !win_ok( win ) || node < 0 || node >= win->tree->count ) {
        return;
    }
    keep_selection( win );
    win->node = node;
    win->stale = 1;
    win->search = 0;
    win->cur = win->top = win->anchor = 0;
    list_show( &win->tree_top, tree_vis_of( win->tree, node ), win_rows( win ) );
}

/* Another drive in the window: its directories read if they have not
   been, and the directory DOS has as that drive's current one shown. */
void win_set_drive( FWIN *win, int drive )
{
    DTREE *tree;
    char cwd[PATH_MAX], path[PATH_MAX];

    keep_selection( win );
    tree = tree_get( drive, 0 );
    if ( tree == NULL ) {
        path[0] = (char)('A' + drive);
        str_cpy( path + 1, ": cannot be read." );
        msg_box2( path, "The drive is not ready, or there is no disk in the drive.", MB_OK );
        return;
    }
    win->drive = win->drive_cur = drive;
    win->tree = tree;
    win->node = 0;
    win->tree_top = 0;
    win->search = 0;
    win->stale = 1;
    win->cur = win->top = win->anchor = 0;
    flist_free( &win->files );
    if ( sys_get_cwd( drive, cwd ) == 0 && cwd[0] ) {
        path[0] = (char)('A' + drive);
        str_cpy( path + 1, ":\\" );
        str_catn( path, cwd, sizeof( path ) );
        win->node = tree_find( tree, path );
        list_show( &win->tree_top, tree_vis_of( tree, win->node ), win_rows( win ) );
    }
    sys_crit_seen();
}

/* A drive's directories read again, and every window that shows the
   drive put back on the directory it had, or the nearest that is left. */
void wins_reread( int drive )
{
    char paths[2][PATH_MAX];
    DTREE *tree;
    int index;

    for ( index = 0; index < 2; index++ ) {
        paths[index][0] = 0;
        if ( wins[index].drive == drive && win_ok( &wins[index] ) ) {
            tree_path( wins[index].tree, wins[index].node, paths[index] );
        }
    }
    tree = tree_get( drive, 1 );
    for ( index = 0; index < 2; index++ ) {
        if ( wins[index].drive != drive ) {
            continue;
        }
        wins[index].tree = tree;
        wins[index].stale = 1;
        wins[index].node = tree && paths[index][0] ? tree_find( tree, paths[index] ) : 0;
        if ( tree ) {
            list_show( &wins[index].tree_top, tree_vis_of( tree, wins[index].node ),
                       win_rows( &wins[index] ) );
        }
    }
    if ( tree == NULL ) {
        msg_box( "The drive is not ready.", MB_OK );
    }
}

void wins_stale_all( void )
{
    wins[0].stale = wins[1].stale = 1;
}

/* A directory going into or out of a tree moves the ones listed after
   it, and a window knows its directory by where the tree lists it.  So
   round such a change: wins_mark() notes each window's directory by
   name, and wins_restore() finds them again. */
static char marked[2][PATH_MAX];

void wins_mark( int drive )
{
    int index;

    for ( index = 0; index < 2; index++ ) {
        marked[index][0] = 0;
        if ( wins[index].drive == drive && win_ok( &wins[index] ) ) {
            tree_path( wins[index].tree, wins[index].node, marked[index] );
        }
    }
}

void wins_restore( int drive )
{
    int index;

    for ( index = 0; index < 2; index++ ) {
        if ( wins[index].drive == drive && marked[index][0] && win_ok( &wins[index] ) ) {
            wins[index].node = tree_find( wins[index].tree, marked[index] );
            wins[index].stale = 1;
            list_show( &wins[index].tree_top,
                       tree_vis_of( wins[index].tree, wins[index].node ), win_rows( &wins[index] ) );
        }
    }
}

/* DOS's current drive and directory made the ones the keys are in, so
   that a name typed without a path means what the screen suggests and
   a program starts where the user was looking */
void app_chdir( void )
{
    FWIN *win = app_win();
    char path[PATH_MAX];

    if ( !win_ok( win ) ) {
        return;
    }
    tree_path( win->tree, win->node, path );
    sys_set_drive( win->drive );
    sys_chdir( path );
    sys_crit_seen();
}

static void wins_refresh( void )
{
    int index;

    for ( index = 0; index < nwins; index++ ) {
        if ( wins[index].stale ) {
            win_reload( &wins[index] );
        }
    }
}

/* ------------------------------------------------------------------ */
/* where everything goes                                               */
/* ------------------------------------------------------------------ */

static void win_place( FWIN *win, int top, int bottom )
{
    win->row_path = top;
    win->row_drives = top + 1;
    win->row_head = top + 2;
    win->row_top = top + 3;
    win->row_bot = bottom;
    win->tree_left = 0;
    win->tree_right = TREE_W - 1;
    win->file_left = TREE_W;
    win->file_right = scr_cols - 1;
}

void app_layout( void )
{
    int first = MENU_ROW + 1, last = scr_rows - 2, mid = first + (last - first + 1) / 2;
    int index;

    nwins = 0;
    prog_head = -1;
    switch ( opt.view ) {
    case VIEW_DUAL:
        nwins = 2;
        win_place( &wins[0], first, mid - 1 );
        win_place( &wins[1], mid, last );
        break;
    case VIEW_PROGFILE:
        nwins = 1;
        win_place( &wins[0], first, mid - 1 );
        prog_head = mid;
        break;
    case VIEW_PROGS:
        prog_head = first;
        break;
    default:
        nwins = 1;
        win_place( &wins[0], first, last );
        break;
    }
    prog_first = prog_head + 1;
    prog_last = last;
    for ( index = 0; index < nwins; index++ ) {
        if ( win_ok( &wins[index] ) ) {
            list_show( &wins[index].tree_top,
                       tree_vis_of( wins[index].tree, wins[index].node ), win_rows( &wins[index] ) );
        }
        list_show( &wins[index].top, wins[index].cur, win_rows( &wins[index] ) );
    }
    if ( prog_head >= 0 ) {
        list_show( &prog_top, prog_cur, prog_last - prog_first + 1 );
    }
}

void app_mode_changed( void )
{
    app_layout();
}

/* the stops Tab makes, in order: (window, area) pairs */
static int focus_stops( int stops[][2] )
{
    int count = 0, index;

    for ( index = 0; index < nwins; index++ ) {
        stops[count][0] = index;
        stops[count++][1] = AREA_DRIVES;
        if ( opt.view != VIEW_ALL ) {
            stops[count][0] = index;
            stops[count++][1] = AREA_TREE;
        }
        stops[count][0] = index;
        stops[count++][1] = AREA_FILES;
    }
    if ( prog_head >= 0 ) {
        stops[count][0] = 0;
        stops[count++][1] = AREA_PROGS;
    }
    return count;
}

static void focus_step( int step )
{
    int stops[8][2], count = focus_stops( stops ), index, at = 0;

    for ( index = 0; index < count; index++ ) {
        if ( stops[index][1] == focus_area
             && (focus_area == AREA_PROGS || stops[index][0] == focus_win) ) {
            at = index;
        }
    }
    at = (at + step + count) % count;
    focus_win = stops[at][0];
    focus_area = stops[at][1];
}

void app_set_view( int view )
{
    int was = opt.view;

    opt.view = view;
    if ( view == VIEW_DUAL && !win_ok( &wins[1] ) ) {
        wins[1] = wins[0];              /* the second list starts as the first */
        mem_set( &wins[1].files, 0, sizeof( FLIST ) );
        wins[1].stale = 1;
    }
    if ( view == VIEW_ALL || was == VIEW_ALL ) {
        wins[0].stale = 1;
        wins[0].search = 0;
        wins[0].cur = wins[0].top = 0;
    }
    app_layout();
    if ( view == VIEW_PROGS ) {
        focus_area = AREA_PROGS;
    } else if ( focus_area == AREA_PROGS && prog_head < 0 ) {
        focus_area = AREA_FILES;
    } else if ( view == VIEW_ALL && focus_area == AREA_TREE ) {
        focus_area = AREA_FILES;
    }
    if ( focus_win >= nwins ) {
        focus_win = 0;
    }
    wins[0].stale = 1;
    if ( nwins > 1 ) {
        wins[1].stale = 1;
    }
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

/* a list's title bar: lit when the keys go to that list */
static void pane_head( int row, int left, int right, const char *title, int focused )
{
    int width = right - left + 1, len = (int)str_len( title );
    u8 attr = focused ? pal.pane_focus : pal.pane_title;

    scr_fill( row, left, width, ' ', attr );
    if ( len > width - 2 ) {
        title += len - (width - 2);     /* the end of a path says more */
        len = width - 2;
    }
    scr_putn( row, left + (width - len) / 2, title, len, attr );
    scr_flag( row, left, width, scr_bar_edges() | (focused ? CF_GRAD : 0) );
    scr_flag( row, left, 1, CF_LEFT );
    scr_flag( row, right, 1, CF_RIGHT );
}

static void put_clip( int row, int col, int ch, u8 attr, int limit )
{
    if ( col <= limit ) {
        scr_ch( row, col, ch, attr );
    }
}

static void draw_drives( FWIN *win, int focused, int windex )
{
    int width = drive_width(), index, col = 1, drive, row = win->row_drives;
    char text[8];
    u8 attr;

    scr_fill( row, 0, scr_cols, ' ', pal.desk );
    for ( index = 0; index < ndrives; index++, col += width ) {
        drive = drives[index];
        attr = drive == win->drive ? pal.drive_sel : pal.desk;
        if ( drop_win == windex && drop_drive == drive ) {
            attr = pal.select;
        }
        text[0] = '[';
        text[1] = (char)('A' + drive);
        text[2] = ':';
        text[3] = ']';
        text[4] = 0;
        if ( width == 7 ) {
            scr_icon( row, col, drive_icon( drive ), pal.desk );
            scr_putn( row, col + 4, text + 1, 2, attr );
        } else if ( width == 5 ) {
            scr_put( row, col, text, attr );
        } else {
            scr_putn( row, col, text + 1, 2, attr );
        }
        if ( focused && drive == win->drive_cur ) {
            scr_cursor( row, col + (width == 7 ? 4 : width == 5 ? 1 : 0), CUR_LINE );
        }
    }
}

/* One line of the tree.  The lines that join it up: its own corner,
   and for each directory above it a bar when that one has a sibling
   still to come further down. */
static void draw_tree_line( FWIN *win, int row, int node, int focused, int windex )
{
    DTREE *tree = win->tree;
    TNODE *nd = &tree->node[node];
    int base = win->tree_left + 1, limit = win->tree_right - 1;
    int depth = nd->depth, level, above = node, box, col, len;
    const char *text;
    char root[4];
    u8 attr;

    for ( level = depth; level >= 1; level-- ) {
        col = base + 1 + (level - 1) * 3;
        if ( level == depth ) {
            put_clip( row, col, (nd->flags & TN_LAST) ? 0xC0 : 0xC3, pal.pane, limit );
            put_clip( row, col + 1, 0xC4, pal.pane, limit );
        } else if ( !(tree->node[above].flags & TN_LAST) ) {
            put_clip( row, col, 0xB3, pal.pane, limit );
        }
        above = tree->node[above].parent;
    }
    box = base + depth * 3;
    if ( gfx_on ) {
        if ( box + 1 <= limit ) {
            scr_icon( row, box, !(nd->flags & TN_KIDS) ? IC_DIR
                      : (nd->flags & TN_OPEN) ? IC_DIR_MINUS : IC_DIR_PLUS, pal.pane );
        }
        col = box + 3;
    } else {
        put_clip( row, box, '[', pal.pane, limit );
        put_clip( row, box + 1, !(nd->flags & TN_KIDS) ? ' ' : (nd->flags & TN_OPEN) ? '-' : '+',
                  pal.pane, limit );
        put_clip( row, box + 2, ']', pal.pane, limit );
        col = box + 4;
    }
    if ( depth ) {
        text = nd->name;
    } else {
        root[0] = (char)('A' + tree->drive);
        root[1] = ':';
        root[2] = '\\';
        root[3] = 0;
        text = root;
    }
    attr = node == win->node ? (focused ? pal.select : pal.select_off) : pal.pane;
    if ( drop_win == windex && drop_node == node ) {
        attr = pal.select;
    }
    len = (int)str_len( text );
    if ( len > limit - col + 1 ) {
        len = limit - col + 1;
    }
    if ( len > 0 ) {
        scr_putn( row, col, text, len, attr );
    }
}

static void draw_tree( FWIN *win, int focused, int windex )
{
    int rows = win_rows( win ), line, row, left = win->tree_left, right = win->tree_right;
    int total = win_ok( win ) ? win->tree->nvis : 0;

    pane_head( win->row_head, left, right, "Directory Tree", focused );
    for ( line = 0; line < rows; line++ ) {
        row = win->row_top + line;
        scr_ch( row, left, 0xB3, pal.pane );
        scr_fill( row, left + 1, right - left - 1, ' ', pal.pane );
        if ( win->tree_top + line < total ) {
            draw_tree_line( win, row, win->tree->vis[win->tree_top + line], focused, windex );
        }
    }
    sbar_draw( win->row_top, win->row_bot, right, win->tree_top, rows, total );
}

/* All Files has no tree: what stands in its place says what the file,
   the selection, the directory and the disk amount to */
static void draw_info( FWIN *win )
{
    int rows = win_rows( win ), line, row, left = win->tree_left, right = win->tree_right;
    char text[64];

    pane_head( win->row_head, left, right, "Information", 0 );
    for ( line = 0; line < rows; line++ ) {
        row = win->row_top + line;
        scr_ch( row, left, 0xB3, pal.pane );
        scr_fill( row, left + 1, right - left, ' ', pal.pane );
        if ( info_line( win, line, text, sizeof( text ) ) ) {
            scr_putn( row, left + 2, text,
                      (int)str_len( text ) < right - left - 2 ? (int)str_len( text ) : right - left - 2,
                      pal.pane );
        }
    }
}

static int is_program( const char *name )
{
    const char *ext = path_ext( name );

    return str_icmp( ext, "EXE" ) == 0 || str_icmp( ext, "COM" ) == 0
           || str_icmp( ext, "BAT" ) == 0 || str_icmp( ext, "CMD" ) == 0;
}

static void draw_file_line( FWIN *win, int row, int index, int focused )
{
    FENT *ent = &win->files.ent[index];
    int left = win->file_left + 1, right = win->file_right - 1, width = right - left + 1;
    int date_col = right - 8, size_col, len, room;
    char size[20], date[12];
    u8 attr = ent->sel ? (focused ? pal.select : pal.select_off) : pal.pane;

    scr_fill( row, left, width, ' ', attr );
    if ( gfx_on ) {
        scr_icon( row, left, is_program( ent->name ) ? IC_PROG : IC_FILE, attr );
    }
    if ( focused && index == win->cur ) {
        if ( gfx_on ) {
            scr_flag( row, left, width, CF_TOP | CF_BOTTOM );
        } else {
            scr_ch( row, left + 1, 0x10, attr );
        }
    }
    fmt_date( ent->date, date );
    scr_put( row, date_col, date, attr );
    fmt_num( ent->size, 0, size );
    size_col = date_col - 2 - (int)str_len( size );
    scr_put( row, size_col, size, attr );
    room = size_col - 1 - (left + 3);
    len = (int)str_len( ent->name );
    if ( len > room ) {
        len = room;
    }
    if ( len > 0 ) {
        scr_putn( row, left + 3, ent->name, len, attr );
    }
}

static void draw_files( FWIN *win, int focused )
{
    int rows = win_rows( win ), line, row, left = win->file_left, right = win->file_right;
    char title[PATH_MAX + 80];
    const char *empty;

    if ( !win_ok( win ) ) {
        title[0] = 0;
    } else if ( win->search ) {
        str_cpy( title, "Search Results for: " );
        str_catn( title, win->search_for, sizeof( title ) );
    } else if ( opt.view == VIEW_ALL ) {
        title[0] = (char)('A' + win->drive);
        str_cpy( title + 1, ":  " );
        str_catn( title, opt.filter, sizeof( title ) );
    } else {
        tree_path( win->tree, win->node, title );
        path_join( title, title, opt.filter, sizeof( title ) );
    }
    pane_head( win->row_head, left, right, title, focused );
    for ( line = 0; line < rows; line++ ) {
        row = win->row_top + line;
        scr_ch( row, left, 0xB3, pal.pane );
        scr_fill( row, left + 1, right - left - 1, ' ', pal.pane );
        if ( win->top + line < win->files.count ) {
            draw_file_line( win, row, win->top + line, focused );
        }
    }
    if ( win->files.count == 0 ) {
        empty = !win_ok( win ) ? "The drive is not ready."
                : win->search ? "No files were found."
                : "No files in this directory.";
        scr_put( win->row_top, left + 3, empty, pal.pane );
    }
    sbar_draw( win->row_top, win->row_bot, right, win->top, rows, win->files.count );
}

static void draw_win( FWIN *win, int windex )
{
    char path[PATH_MAX];
    int here = focus_area != AREA_PROGS && focus_win == windex;

    scr_fill( win->row_path, 0, scr_cols, ' ', pal.desk );
    if ( win_ok( win ) ) {
        tree_path( win->tree, win->node, path );
        scr_putn( win->row_path, 1, path,
                  (int)str_len( path ) < scr_cols - 2 ? (int)str_len( path ) : scr_cols - 2, pal.desk );
    }
    draw_drives( win, here && focus_area == AREA_DRIVES, windex );
    if ( opt.view == VIEW_ALL ) {
        draw_info( win );
    } else {
        draw_tree( win, here && focus_area == AREA_TREE, windex );
    }
    draw_files( win, here && focus_area == AREA_FILES );
}

static void draw_progs( void )
{
    int rows = prog_last - prog_first + 1, line, row, item, total = prog_count();
    int focused = focus_area == AREA_PROGS, right = scr_cols - 1, col;
    char text[TITLE_MAX + 4];
    u8 attr;

    pane_head( prog_head, 0, right, items[prog_group].title, focused );
    for ( line = 0; line < rows; line++ ) {
        row = prog_first + line;
        scr_ch( row, 0, 0xB3, pal.pane );
        scr_fill( row, 1, right - 1, ' ', pal.pane );
        if ( prog_top + line >= total ) {
            continue;
        }
        item = prog_at( prog_top + line );
        attr = prog_top + line == prog_cur ? (focused ? pal.select : pal.select_off) : pal.pane;
        /* a group in brackets on a text screen; an icon says which on a
           graphics one.  The first line inside a group leads back out. */
        if ( item < 0 || items[item].is_group ) {
            text[0] = 0;
            if ( !gfx_on ) {
                str_cpy( text, "[" );
            }
            str_catn( text, item < 0 ? items[items[prog_group].parent].title
                            : items[item].title, sizeof( text ) );
            if ( !gfx_on ) {
                str_catn( text, "]", sizeof( text ) );
            }
        } else {
            str_fit( text, items[item].title, sizeof( text ) );
        }
        col = 2;
        if ( gfx_on ) {
            scr_icon( row, col, item < 0 || items[item].is_group ? IC_GROUP : IC_ITEM, pal.pane );
            col += 3;
        }
        scr_put( row, col, text, attr );
        if ( prog_top + line == prog_cur ) {
            scr_recolor( row, col - 1, 1, attr );
            scr_recolor( row, col + (int)str_len( text ), 1, attr );
        }
    }
    sbar_draw( prog_first, prog_last, right, prog_top, rows, total );
}

static void draw_status( void )
{
    int row = scr_rows - 1;
    DATETIME now;
    char clock[12];

    scr_fill( row, 0, scr_cols, ' ', pal.status );
    if ( status_hint ) {
        scr_putw( row, 1, status_hint, scr_cols - 2, pal.status );
        return;
    }
    scr_put( row, 1, "F10", pal.status_hot );
    scr_put( row, 4, "=Actions", pal.status );
    scr_put( row, 14, "Shift+F9", pal.status_hot );
    scr_put( row, 22, "=Command Prompt", pal.status );
    if ( focus_area == AREA_FILES && app_win()->add_mode ) {
        scr_put( row, 40, "ADD", pal.status_hot );
    }
    sys_now( &now );
    fmt_time( (u16)((now.hour << 11) | (now.minute << 5)), clock );
    scr_put( row, scr_cols - 1 - (int)str_len( clock ), clock, pal.status );
}

void app_draw( void )
{
    static const char name[] = "PM-DOS Shell";
    int index;

    scr_fill( 0, 0, scr_cols, ' ', pal.title );
    scr_put( 0, (scr_cols - (int)sizeof( name ) + 1) / 2, name, pal.title );
    scr_flag( 0, 0, scr_cols, CF_GRAD );
    menu_draw_bar( -1, 0 );
    for ( index = 0; index < nwins; index++ ) {
        draw_win( &wins[index], index );
    }
    if ( prog_head >= 0 ) {
        draw_progs();
    }
    draw_status();
}

/* ------------------------------------------------------------------ */
/* what can be done just now                                           */
/* ------------------------------------------------------------------ */

static int on_file( void )
{
    return focus_area == AREA_FILES && app_win()->files.count > 0;
}

static int on_program( void )
{
    return focus_area == AREA_PROGS && prog_count() > 0 && prog_at( prog_cur ) >= 0;
}

int app_can( int cmd )
{
    FWIN *win = app_win();

    switch ( cmd ) {
    case CMD_OPEN: case CMD_ASSOCIATE: case CMD_VIEWFILE:
        return on_file();
    case CMD_PRINT: case CMD_MOVE: case CMD_COPY: case CMD_ATTRIB:
        return on_file() || (focus_area != AREA_PROGS && win->files.nsel + kept_count() > 0);
    case CMD_DELETE: case CMD_RENAME:
        return on_file() || (focus_area == AREA_TREE && win_ok( win ) && win->node > 0);
    case CMD_MKDIR: case CMD_SEARCH:
        return focus_area != AREA_PROGS && win_ok( win );
    case CMD_SELALL: case CMD_DESELALL:
        return focus_area != AREA_PROGS && win->files.count > 0;
    case CMD_EXPAND1: case CMD_EXPANDBR: case CMD_EXPANDALL: case CMD_COLLAPSE:
        return focus_area != AREA_PROGS && opt.view != VIEW_ALL && win_ok( win );
    case CMD_POPEN: case CMD_PDELETE: case CMD_PPROPS: case CMD_PREORDER:
        return on_program();
    case CMD_PCOPY:
        return on_program() && !items[prog_at( prog_cur )].is_group;
    }
    return 1;
}

int app_checked( int cmd )
{
    switch ( cmd ) {
    case CMD_ACROSS:    return opt.select_across;
    case CMD_VSINGLE:   return opt.view == VIEW_SINGLE;
    case CMD_VDUAL:     return opt.view == VIEW_DUAL;
    case CMD_VALL:      return opt.view == VIEW_ALL;
    case CMD_VPROGFILE: return opt.view == VIEW_PROGFILE;
    case CMD_VPROGS:    return opt.view == VIEW_PROGS;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_file_path( char *path )
{
    FWIN *win = app_win();

    flist_path( win->tree, &win->files.ent[win->cur], path );
}

void app_command( int cmd )
{
    FWIN *win = app_win();
    char path[PATH_MAX];
    int index;

    if ( cmd == CMD_NONE || !app_can( cmd ) ) {
        return;
    }
    switch ( cmd ) {
    case CMD_OPEN:
        cmd_file_path( path );
        run_file( path );
        break;
    case CMD_VIEWFILE:
        cmd_file_path( path );
        view_file( path );
        break;
    case CMD_RUN:       dlg_run_line();     break;
    case CMD_PRINT:     op_print();         break;
    case CMD_ASSOCIATE: dlg_associate();    break;
    case CMD_SEARCH:    dlg_search();       break;
    case CMD_MOVE:      op_copy( 1 );       break;
    case CMD_COPY:      op_copy( 0 );       break;
    case CMD_DELETE:    op_delete();        break;
    case CMD_RENAME:    op_rename();        break;
    case CMD_ATTRIB:    op_attrib();        break;
    case CMD_MKDIR:     op_mkdir();         break;
    case CMD_SELALL:
        for ( index = 0; index < win->files.count; index++ ) {
            win->files.ent[index].sel = 1;
        }
        flist_count( &win->files );
        break;
    case CMD_DESELALL:
        sel_clear( win );
        kept_clear();
        break;
    case CMD_EXIT:
        running = 0;
        break;
    case CMD_PNEW:      prog_new();         break;
    case CMD_POPEN:     prog_open();        break;
    case CMD_PCOPY:     prog_copy();        break;
    case CMD_PDELETE:   prog_delete();      break;
    case CMD_PPROPS:    prog_props();       break;
    case CMD_PREORDER:  prog_reorder();     break;
    case CMD_CONFIRM:   dlg_confirmation(); break;
    case CMD_FILEOPTS:  dlg_file_options(); break;
    case CMD_ACROSS:
        opt.select_across = !opt.select_across;
        if ( !opt.select_across ) {
            kept_clear();
        }
        break;
    case CMD_INFO:      dlg_show_info();    break;
    case CMD_DISPLAY:   dlg_display();      break;
    case CMD_COLORS:    dlg_colors();       break;
    case CMD_VSINGLE:   app_set_view( VIEW_SINGLE );   break;
    case CMD_VDUAL:     app_set_view( VIEW_DUAL );     break;
    case CMD_VALL:      app_set_view( VIEW_ALL );      break;
    case CMD_VPROGFILE: app_set_view( VIEW_PROGFILE ); break;
    case CMD_VPROGS:    app_set_view( VIEW_PROGS );    break;
    case CMD_REPAINT:
        scr_repaint();
        break;
    case CMD_REFRESH:
        drives_find();
        wins_reread( wins[0].drive );
        if ( nwins > 1 && wins[1].drive != wins[0].drive ) {
            wins_reread( wins[1].drive );
        }
        break;
    case CMD_EXPAND1:
        tree_open( win->tree, win->node, 0 );
        break;
    case CMD_EXPANDBR:
        tree_open( win->tree, win->node, 1 );
        break;
    case CMD_EXPANDALL:
        tree_open_all( win->tree );
        break;
    case CMD_COLLAPSE:
        tree_close( win->tree, win->node );
        break;
    case CMD_HINDEX:    help_show( HELP_INDEX );    break;
    case CMD_HKEYS:     help_show( HELP_KEYS );     break;
    case CMD_HBASICS:   help_show( HELP_BASICS );   break;
    case CMD_HCOMMANDS: help_show( HELP_COMMANDS ); break;
    case CMD_HPROCS:    help_show( HELP_PROCS );    break;
    case CMD_HUSING:    help_show( HELP_USING );    break;
    case CMD_ABOUT:     dlg_about();        break;
    case CMD_PROMPT:    run_prompt();       break;
    }
}

/* ------------------------------------------------------------------ */
/* keys                                                                */
/* ------------------------------------------------------------------ */

static void keys_drives( FWIN *win, int key )
{
    int index, at = 0;

    for ( index = 0; index < ndrives; index++ ) {
        if ( drives[index] == win->drive_cur ) {
            at = index;
        }
    }
    switch ( key & ~K_SHIFT ) {
    case K_LEFT:
        win->drive_cur = drives[(at + ndrives - 1) % ndrives];
        return;
    case K_RIGHT:
        win->drive_cur = drives[(at + 1) % ndrives];
        return;
    case K_ENTER:
    case ' ':
        win_set_drive( win, win->drive_cur );
        return;
    }
    if ( key < 0x100 && ch_upper( key ) >= 'A' && ch_upper( key ) <= 'Z' ) {
        for ( index = 0; index < ndrives; index++ ) {
            if ( drives[index] == ch_upper( key ) - 'A' ) {
                win_set_drive( win, drives[index] );
            }
        }
    }
}

static void keys_tree( FWIN *win, int key )
{
    DTREE *tree = win->tree;
    int line, rows = win_rows( win ), index, at, letter;
    TNODE *nd;

    if ( !win_ok( win ) ) {
        return;
    }
    line = tree_vis_of( tree, win->node );
    nd = &tree->node[win->node];
    switch ( key & ~K_SHIFT ) {
    case K_UP:    line--;             break;
    case K_DOWN:  line++;             break;
    case K_PGUP:  line -= rows - 1;   break;
    case K_PGDN:  line += rows - 1;   break;
    case K_HOME:  line = 0;           break;
    case K_END:   line = tree->nvis - 1; break;
    case K_LEFT:
        if ( nd->flags & TN_OPEN ) {
            tree_close( tree, win->node );
        } else if ( nd->parent >= 0 ) {
            win_set_node( win, nd->parent );
        }
        return;
    case K_RIGHT:
        if ( (nd->flags & TN_KIDS) && !(nd->flags & TN_OPEN) ) {
            tree_open( tree, win->node, 0 );
        } else if ( nd->flags & TN_KIDS ) {
            win_set_node( win, win->node + 1 );
        }
        return;
    case '+':
        app_command( CMD_EXPAND1 );
        return;
    case '*':
        app_command( CMD_EXPANDBR );
        return;
    case '-':
        app_command( CMD_COLLAPSE );
        return;
    case K_CSTAR:
        app_command( CMD_EXPANDALL );
        return;
    default:
        if ( key < ' ' || key >= 0x100 ) {
            return;
        }
        /* a letter: the next directory showing that starts with it */
        letter = ch_upper( key );
        for ( index = 1; index <= tree->nvis; index++ ) {
            at = (line + index) % tree->nvis;
            if ( ch_upper( tree->node[tree->vis[at]].name[0] ) == letter ) {
                line = at;
                break;
            }
        }
        break;
    }
    if ( line < 0 ) {
        line = 0;
    }
    if ( line > tree->nvis - 1 ) {
        line = tree->nvis - 1;
    }
    if ( tree->vis[line] != win->node ) {
        win_set_node( win, tree->vis[line] );
    }
}

/* The file the keys are on becomes "index".  Without Shift that file
   is the selection, unless Shift+F8 has said moving is only moving;
   with Shift everything from where the selection began to here is. */
static void file_goto( FWIN *win, int index, int extend )
{
    if ( win->files.count == 0 ) {
        return;
    }
    if ( index < 0 ) {
        index = 0;
    }
    if ( index > win->files.count - 1 ) {
        index = win->files.count - 1;
    }
    win->cur = index;
    if ( extend ) {
        sel_range( win, win->anchor, index );
    } else if ( !win->add_mode ) {
        sel_only( win, index );
    }
    list_show( &win->top, win->cur, win_rows( win ) );
}

static void keys_files( FWIN *win, int key )
{
    int rows = win_rows( win ), extend = (key & K_SHIFT) != 0, index, at, letter;
    FENT *ent;

    switch ( key & ~K_SHIFT ) {
    case K_UP:    file_goto( win, win->cur - 1, extend );           return;
    case K_DOWN:  file_goto( win, win->cur + 1, extend );           return;
    case K_PGUP:  file_goto( win, win->cur - (rows - 1), extend );  return;
    case K_PGDN:  file_goto( win, win->cur + (rows - 1), extend );  return;
    case K_HOME:  file_goto( win, 0, extend );                      return;
    case K_END:   file_goto( win, win->files.count - 1, extend );   return;
    case K_ENTER:
        app_command( CMD_OPEN );
        return;
    case K_SHF8:
        win->add_mode = !win->add_mode;
        return;
    case K_ESC:                         /* out of a search's answer */
        if ( win->search ) {
            win->search = 0;
            win->stale = 1;
            win->cur = win->top = 0;
        }
        return;
    case ' ':
        if ( win->files.count ) {
            ent = &win->files.ent[win->cur];
            ent->sel = !ent->sel;
            win->anchor = win->cur;
            flist_count( &win->files );
        }
        return;
    }
    if ( key < ' ' || key >= 0x100 || win->files.count == 0 ) {
        return;
    }
    letter = ch_upper( key );
    for ( index = 1; index <= win->files.count; index++ ) {
        at = (win->cur + index) % win->files.count;
        if ( ch_upper( win->files.ent[at].name[0] ) == letter ) {
            file_goto( win, at, 0 );
            return;
        }
    }
}

static void keys_progs( int key )
{
    int total = prog_count(), rows = prog_last - prog_first + 1, index, at, item, letter;

    switch ( key & ~K_SHIFT ) {
    case K_UP:    prog_cur--;             break;
    case K_DOWN:  prog_cur++;             break;
    case K_PGUP:  prog_cur -= rows - 1;   break;
    case K_PGDN:  prog_cur += rows - 1;   break;
    case K_HOME:  prog_cur = 0;           break;
    case K_END:   prog_cur = total - 1;   break;
    case K_ENTER:
        prog_open();
        return;
    case K_ESC:
        prog_back();
        return;
    default:
        if ( key < ' ' || key >= 0x100 ) {
            return;
        }
        letter = ch_upper( key );
        for ( index = 1; index <= total; index++ ) {
            at = (prog_cur + index) % total;
            item = prog_at( at );
            if ( item >= 0 && ch_upper( items[item].title[0] ) == letter ) {
                prog_cur = at;
                break;
            }
        }
        break;
    }
    if ( prog_cur > total - 1 ) {
        prog_cur = total - 1;
    }
    if ( prog_cur < 0 ) {
        prog_cur = 0;
    }
    list_show( &prog_top, prog_cur, rows );
}

static void app_key( int key )
{
    FWIN *win = app_win();
    int index;

    if ( key == K_F10 || key == K_ALTTAP || key_alt_letter( key ) ) {
        app_command( menu_run( key ) );
        return;
    }
    switch ( key ) {
    case K_F1:
        if ( focus_area != AREA_PROGS || !prog_help() ) {
            help_show( HELP_INDEX );
        }
        return;
    case K_F3:
    case K_ALTF4:   app_command( CMD_EXIT );        return;
    case K_F5:      app_command( CMD_REFRESH );     return;
    case K_SHF5:    app_command( CMD_REPAINT );     return;
    case K_SHF9:    app_command( CMD_PROMPT );      return;
    case K_F7:      app_command( CMD_MOVE );        return;
    case K_F8:      app_command( CMD_COPY );        return;
    case K_F9:      app_command( CMD_VIEWFILE );    return;
    case K_DEL:
        app_command( focus_area == AREA_PROGS ? CMD_PDELETE : CMD_DELETE );
        return;
    case K_TAB:     focus_step( 1 );                return;
    case K_SHTAB:   focus_step( -1 );               return;
    case K_CSLASH:  app_command( CMD_SELALL );      return;
    case K_CBSLASH: app_command( CMD_DESELALL );    return;
    }
    if ( (key & 0xFF00) == 0x400 ) {            /* Ctrl and a letter: that drive */
        for ( index = 0; index < ndrives && focus_area != AREA_PROGS; index++ ) {
            if ( drives[index] == (key & 0xFF) - 'A' ) {
                win_set_drive( win, drives[index] );
            }
        }
        return;
    }
    switch ( focus_area ) {
    case AREA_DRIVES: keys_drives( win, key ); break;
    case AREA_TREE:   keys_tree( win, key );   break;
    case AREA_FILES:  keys_files( win, key );  break;
    case AREA_PROGS:  keys_progs( key );       break;
    }
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

static void redraw_now( void )
{
    scr_cursor( 0, 0, CUR_HIDE );
    app_draw();
    scr_flush();
}

/* The next thing the mouse does while its button is held: 1 with
   "mouse" a drag, 2 when a time limit ran out with the button still
   down, 0 when the button was let go. */
static int held_next( int hundredths )
{
    int key;

    for ( ;; ) {
        key = hundredths >= 0 ? key_get_timed( hundredths ) : key_get();
        if ( key_service( key ) ) {
            continue;
        }
        if ( key == K_TICK ) {
            return 2;
        }
        if ( key != K_MOUSE ) {
            continue;
        }
        if ( mouse.kind == ME_DRAG ) {
            return 1;
        }
        return 0;
    }
}

/* A press on a scroll bar, which moves the list's first line: an arrow
   again and again while it is held, a page for the track, and the box
   wherever it is dragged. */
static void scroll_press( int top, int bottom, int *first, int rows, int total )
{
    int part = sbar_hit( top, bottom, 0, *first, rows, total, mouse.row );
    int limit = total - rows, step, got;

    if ( limit < 0 ) {
        limit = 0;
    }
    if ( part == SB_NONE ) {
        return;
    }
    for ( ;; ) {
        step = part == SB_UP ? -1 : part == SB_DOWN ? 1 : part == SB_PGUP ? -(rows - 1)
               : part == SB_PGDN ? rows - 1 : 0;
        *first += step;
        if ( *first > limit ) {
            *first = limit;
        }
        if ( *first < 0 ) {
            *first = 0;
        }
        redraw_now();
        got = held_next( part == SB_THUMB ? -1 : 8 );
        if ( got == 0 ) {
            return;
        }
        if ( part == SB_THUMB && got == 1 ) {
            *first = sbar_drag( top, bottom, rows, total, mouse.row );
        } else if ( part == SB_PGUP || part == SB_PGDN ) {
            held_next( -1 );            /* one page a press */
            return;
        }
    }
}

/* the directory or the drive under the pointer, as somewhere files
   could be dropped: 1 if there is one, and the drop_ variables say */
static int drop_find( void )
{
    int index, line, at;
    FWIN *win;

    drop_win = drop_node = drop_drive = -1;
    for ( index = 0; index < nwins; index++ ) {
        win = &wins[index];
        if ( mouse.row == win->row_drives ) {
            at = drive_at( mouse.col );
            if ( at >= 0 ) {
                drop_win = index;
                drop_drive = drives[at];
                return 1;
            }
        }
        if ( opt.view != VIEW_ALL && win_ok( win ) && mouse.row >= win->row_top
             && mouse.row <= win->row_bot && mouse.col > win->tree_left
             && mouse.col < win->tree_right ) {
            line = win->tree_top + mouse.row - win->row_top;
            if ( line < win->tree->nvis ) {
                drop_win = index;
                drop_node = win->tree->vis[line];
                return 1;
            }
        }
    }
    return 0;
}

/* A press on a file.  It selects - Shift a range, Ctrl one more - and
   then, for as long as the button is held, the selected files can be
   carried to a directory in a tree or to a drive: moved there when it
   is the same drive and copied when it is another, unless Ctrl says
   copy or Alt says move. */
static void files_press( FWIN *win, int item )
{
    int shift = mouse.shift, was_sel, dragged = 0, got, row = mouse.row, col = mouse.col;
    int move, have;
    char dest[PATH_MAX], cwd[PATH_MAX];
    PICKS picks;

    was_sel = win->files.ent[item].sel;
    win->cur = item;
    if ( mouse.kind == ME_DOUBLE ) {
        sel_only( win, item );
        app_command( CMD_OPEN );
        return;
    }
    if ( shift & SH_SHIFT ) {
        sel_range( win, win->anchor, item );
    } else if ( shift & SH_CTRL ) {
        win->files.ent[item].sel = !was_sel;
        win->anchor = item;
        flist_count( &win->files );
    } else if ( !was_sel ) {
        sel_only( win, item );
    }
    for ( ;; ) {
        redraw_now();
        got = held_next( -1 );
        if ( got == 0 ) {
            break;
        }
        if ( mouse.row != row || mouse.col != col ) {
            dragged = 1;
        }
        if ( dragged ) {
            app_status( drop_find() ? "Let go to put the selected files there"
                                    : "Drag the files to a directory or a drive" );
        }
    }
    app_status( NULL );
    have = dragged && drop_find();
    if ( !have ) {
        drop_win = drop_node = drop_drive = -1;
        if ( !dragged && was_sel && !(shift & (SH_SHIFT | SH_CTRL)) ) {
            sel_only( win, item );
        }
        return;
    }
    if ( drop_node >= 0 ) {
        tree_path( wins[drop_win].tree, drop_node, dest );
    } else {
        dest[0] = (char)('A' + drop_drive);
        str_cpy( dest + 1, ":\\" );
        if ( sys_get_cwd( drop_drive, cwd ) == 0 ) {
            str_catn( dest, cwd, sizeof( dest ) );
        }
    }
    move = ch_upper( dest[0] ) - 'A' == win->drive;
    if ( mouse.shift & SH_CTRL ) {
        move = 0;
    } else if ( mouse.shift & SH_ALT ) {
        move = 1;
    }
    drop_win = drop_node = drop_drive = -1;
    if ( picks_take( &picks ) ) {
        op_drop( &picks, dest, move );
        picks_free( &picks );
    }
}

/* a press somewhere in a file window: 1 if it was this window's */
static int win_press( FWIN *win, int windex )
{
    int line, node, box, at, rows = win_rows( win );
    DTREE *tree = win->tree;

    if ( mouse.row == win->row_drives ) {
        at = drive_at( mouse.col );
        focus_win = windex;
        focus_area = AREA_DRIVES;
        if ( at >= 0 ) {
            win_set_drive( win, drives[at] );
        }
        return 1;
    }
    if ( mouse.row < win->row_head || mouse.row > win->row_bot ) {
        return mouse.row == win->row_path;
    }
    focus_win = windex;
    if ( mouse.col >= win->file_left ) {
        focus_area = AREA_FILES;
        if ( mouse.row < win->row_top ) {
            return 1;
        }
        if ( mouse.col == win->file_right ) {
            scroll_press( win->row_top, win->row_bot, &win->top, rows, win->files.count );
            return 1;
        }
        line = win->top + mouse.row - win->row_top;
        if ( line < win->files.count ) {
            files_press( win, line );
        }
        return 1;
    }
    if ( opt.view == VIEW_ALL ) {
        return 1;
    }
    focus_area = AREA_TREE;
    if ( mouse.row < win->row_top || !win_ok( win ) ) {
        return 1;
    }
    if ( mouse.col == win->tree_right ) {
        scroll_press( win->row_top, win->row_bot, &win->tree_top, rows, tree->nvis );
        return 1;
    }
    line = win->tree_top + mouse.row - win->row_top;
    if ( line >= tree->nvis ) {
        return 1;
    }
    /* on the box or the folder: open or shut, and that is all.  Anywhere
       else on the line: that directory's files, and twice to open or
       shut the directory as well. */
    node = tree->vis[line];
    box = win->tree_left + 1 + tree->node[node].depth * 3;
    at = win->tree_top;
    if ( mouse.col < box || mouse.col > box + 2 ) {
        if ( node != win->node ) {
            win_set_node( win, node );
        }
        if ( mouse.kind != ME_DOUBLE ) {
            win->tree_top = at;
            return 1;
        }
    }
    if ( tree->node[node].flags & TN_OPEN ) {
        /* shut over the selected directory: the one shut becomes it,
           since a selection nobody can see is no selection */
        for ( line = win->node; line > 0 && line != node; line = tree->node[line].parent ) {
        }
        if ( line == node && node != win->node ) {
            win_set_node( win, node );
        }
        tree_close( tree, node );
    } else {
        tree_open( tree, node, 0 );
    }
    win->tree_top = at;
    if ( win->tree_top > tree->nvis - 1 ) {
        win->tree_top = tree->nvis > rows ? tree->nvis - rows : 0;
    }
    return 1;
}

static void progs_press( void )
{
    int rows = prog_last - prog_first + 1, line;

    focus_area = AREA_PROGS;
    if ( mouse.row < prog_first ) {
        return;
    }
    if ( mouse.col == scr_cols - 1 ) {
        scroll_press( prog_first, prog_last, &prog_top, rows, prog_count() );
        return;
    }
    line = prog_top + mouse.row - prog_first;
    if ( line < prog_count() ) {
        prog_cur = line;
        if ( mouse.kind == ME_DOUBLE ) {
            prog_open();
        }
    }
}

static void app_mouse( void )
{
    int index;

    if ( mouse.kind != ME_DOWN && mouse.kind != ME_DOUBLE ) {
        return;
    }
    if ( mouse.row == MENU_ROW ) {
        app_command( menu_run( K_MOUSE ) );
        return;
    }
    if ( prog_head >= 0 && mouse.row >= prog_head && mouse.row <= prog_last ) {
        progs_press();
        return;
    }
    for ( index = 0; index < nwins; index++ ) {
        if ( mouse.row >= wins[index].row_path && mouse.row <= wins[index].row_bot
             && win_press( &wins[index], index ) ) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the command line, and the loop                                      */
/* ------------------------------------------------------------------ */

static const char *dmode_by_tag( const char *tag, int *index )
{
    int at;

    for ( at = 0; at < NDMODES; at++ ) {
        if ( str_icmp( dmodes[at].tag, tag ) == 0 ) {
            *index = at;
            return dmodes[at].tag;
        }
    }
    return NULL;
}

/* 0 = go on; 1 = it was /? or a mistake, and that has been said */
static int parse_args( const char *args )
{
    char word[24];
    int len, want256 = 0, chosen = -1;
    const char *tag;

    while ( *args ) {
        while ( *args == ' ' || *args == '\t' ) {
            args++;
        }
        if ( *args == 0 ) {
            break;
        }
        for ( len = 0; args[len] && args[len] != ' ' && args[len] != '\t'
              && (len == 0 || args[len] != '/'); len++ ) {
        }
        str_fit( word, args, (u32)(len < 23 ? len + 1 : 24) );
        str_upper( word );
        args += len;
        tag = NULL;
        if ( str_cmp( word, "/?" ) == 0 ) {
            sys_stdout( usage, sizeof( usage ) - 1 );
            return 1;
        } else if ( str_cmp( word, "/256" ) == 0 ) {
            want256 = 1;
            continue;
        } else if ( str_cmp( word, "/T" ) == 0 || str_cmp( word, "/T:L" ) == 0 ) {
            tag = "text25";
        } else if ( str_nicmp( word, "/T:H", 4 ) == 0 ) {
            tag = "text50";
        } else if ( str_cmp( word, "/G" ) == 0 || str_cmp( word, "/G:L" ) == 0 ) {
            tag = "ega25";
        } else if ( str_cmp( word, "/G:M" ) == 0 || str_cmp( word, "/G:M1" ) == 0 ) {
            tag = "vga30";
        } else if ( str_cmp( word, "/G:M2" ) == 0 ) {
            tag = "vga34";
        } else if ( str_cmp( word, "/G:H" ) == 0 || str_cmp( word, "/G:H1" ) == 0 ) {
            tag = "ega43";
        } else if ( str_cmp( word, "/G:H2" ) == 0 ) {
            tag = "vga60";
        }
        if ( tag == NULL || dmode_by_tag( tag, &chosen ) == NULL ) {
            sys_stdout( "Invalid switch - ", 17 );
            sys_stdout( word, str_len( word ) );
            sys_stdout( "\r\n", 2 );
            return 1;
        }
    }
    if ( chosen >= 0 ) {
        if ( want256 && dmodes[chosen].kind == VK_VGA ) {
            chosen += 3;                /* the same lines, in 256 colours */
        }
        opt.display = chosen;
    } else if ( want256 ) {
        dmode_by_tag( "vbe30", &opt.display );
    }
    return 0;
}

void shell_main( void )
{
    char cwd[PATH_MAX], path[PATH_MAX];
    int key, drive;

    sys_init();
    opt.view = VIEW_PROGFILE;
    opt.sort_key = SORT_NAME;
    str_cpy( opt.filter, "*.*" );
    opt.confirm_delete = opt.confirm_replace = opt.confirm_mouse = 1;
    ini_load();
    if ( parse_args( sys_cmdline() ) ) {
        sys_exit( 1 );
    }
    key_init();
    opt.display = scr_init( opt.display );
    ui_ready = 1;
    scr_palette( opt.scheme );
    drives_find();
    app_layout();

    drive = sys_get_drive();
    wins[0].drive = wins[0].drive_cur = drive;
    wins[0].tree = tree_get( drive, 0 );
    if ( win_ok( &wins[0] ) && sys_get_cwd( drive, cwd ) == 0 ) {
        path[0] = (char)('A' + drive);
        str_cpy( path + 1, ":\\" );
        str_catn( path, cwd, sizeof( path ) );
        wins[0].node = tree_find( wins[0].tree, path );
    }
    wins[0].stale = 1;
    focus_area = AREA_TREE;
    app_set_view( opt.view );

    while ( running ) {
        wins_refresh();
        scr_cursor( 0, 0, CUR_HIDE );
        app_draw();
        scr_flush();
        key = key_get();
        if ( key_service( key ) ) {
            continue;
        }
        if ( key == K_MOUSE ) {
            app_mouse();
        } else {
            app_key( key );
        }
    }
    ini_save();
    scr_done();
    sys_exit( 0 );
}
