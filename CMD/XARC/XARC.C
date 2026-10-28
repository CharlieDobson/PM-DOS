/*
 * XARC.C - XARC's text screen: the DOS build's XARCTUI.CPP in C.
 *
 *   the title (row 0) | the menu bar (row 1) | the list | the status line
 *
 * THE WINDOW IS THE BROWSER.  With no archive open the list is the disk -
 * the way up, the folders and files of the current directory, or above a
 * root the drives - and Enter on an archive opens it in the same list;
 * Backspace comes back out.  An archive's list is a FOLDER VIEW over its
 * flat entries (ARCVIEW.C): Enter opens a folder, Backspace goes up, and
 * Options > Folder View turns it off for the whole stored paths.
 *
 * COMPRESS AND EXTRACT EACH BELONG TO ONE SIDE.  On the disk, marked
 * files and folders (Space, Ins, the right button) go into a new .ZIP or
 * .7Z with F8, and F5 unpacks the highlighted or marked archive files
 * whole.  In an archive F5 extracts the marked entries, or all of them.
 * On the wrong side each key says which side it works on.
 *
 * An entry that is itself an archive or a disk image opens with Enter -
 * it is unpacked to TEMP and browsed in place, up to four deep - and a
 * text file is shown rather than extracted.  The keys are F1's.
 *
 * WHAT IS PM-DOS'S HERE AND NOT THE DOS BUILD'S: long names, both in the
 * disk browser and in what is extracted (the backend asks the volume, and
 * PM-DOS's take them); no line printed on starting; the memory advice
 * leaves out the DOS box and XMS a native program has nothing to do with;
 * and a new archive is named after the folder in full where long names
 * go, not cut to eight characters.
 */
#include "xarc.h"

#define NEST_MAX    4
#define STATUS_COLS 76

static LIST    list;
static ArcFile *g_arc;
static DIRSCAN g_dir;
static char    g_path[SZ_MAX_NAME];
static char    g_dest[SZ_MAX_NAME * 2];
static int     g_ask_names_off;
static char    g_password[AC_MAX_PW + 1];
static int     running;
static int     op_menu = -1;
static int     screen_open;

/* PMDOSPLT.C: does the volume holding "path" take 8.3 names only? */
int PmNamesNeed83( const char *path );

/* An archive in an archive: the level it came from, and the temp file
   and folder it was unpacked to, deleted when the level is left. */
typedef struct {
    ArcFile *arc;
    char     path[SZ_MAX_NAME];
    char     cwd[SZ_MAX_NAME];
    int      sel;
    char     tmp_file[SZ_MAX_NAME];
    char     tmp_dir[SZ_MAX_NAME];
    char     password[AC_MAX_PW + 1];
} NEST;

static NEST g_nest[NEST_MAX];
static int  g_nest_depth;

static void nest_unwind_all( void );
static void repaint( void );
static void update_status( void );

static int disk_mode( void )
{
    return g_arc == NULL;
}

static void msg( const char *title, const char *text )
{
    msg_box( FG_BLACK, BG_WHITE, title, text, DB_OK );
}

/* ------------------------------------------------------------------ */
/* the rows                                                            */
/* ------------------------------------------------------------------ */

/* 77 columns: a blank for the mark, 39 of name, a 13-column size, the
   date and the attributes - the list's inside is 78, and the scroll bar
   paints over the last of them.  The disk's rows have the same widths,
   so the window does not change shape when an archive opens. */
static void fill_row( void *user, int row, char *buf, int buflen )
{
    char name[64], date[24], attr[16], size[NUM_FMT_MAX];
    int idx = view_index( row );

    (void)user;
    view_row_name( row, name, sizeof( name ) );
    date[0] = attr[0] = 0;
    if ( idx >= 0 ) {
        ArcEntryDate( g_arc, idx, date, sizeof( date ) );
        ArcEntryAttr( g_arc, idx, attr, sizeof( attr ) );
    }
    if ( view_is_up( row ) ) {
        _snprintf( buf, (size_t)buflen, " %-39.39s %13s  %-16.16s %-4.4s", name, "<UP>", "", "" );
    } else if ( view_is_dir( row ) ) {
        _snprintf( buf, (size_t)buflen, " %-39.39s %13s  %-16.16s %-4.4s", name, "<DIR>", date, attr );
    } else {
        NumFmt( idx >= 0 ? ArcEntrySize( g_arc, idx ) : 0, size );
        _snprintf( buf, (size_t)buflen, " %-39.39s %13s  %-16.16s %-4.4s", name, size, date, attr );
    }
}

static void fill_disk_row( void *user, int row, char *buf, int buflen )
{
    char disp[300], date[24], attr[16], size[NUM_FMT_MAX];

    (void)user;
    dirscan_disp( &g_dir, row, disp, sizeof( disp ) );
    dirscan_date( &g_dir, row, date, sizeof( date ) );
    dirscan_attr( &g_dir, row, attr, sizeof( attr ) );
    if ( dirscan_is_up( &g_dir, row ) ) {
        _snprintf( buf, (size_t)buflen, " %-39.39s %13s  %-16.16s %-4.4s", disp, "<UP>", "", "" );
    } else if ( dirscan_kind( &g_dir, row ) == DS_DRIVE ) {
        _snprintf( buf, (size_t)buflen, " %-39.39s %13s  %-16.16s %-4.4s", disp, "<DRIVE>", "", "" );
    } else if ( dirscan_is_dir( &g_dir, row ) ) {
        _snprintf( buf, (size_t)buflen, " %-39.39s %13s  %-16.16s %-4.4s", disp, "<DIR>", date, attr );
    } else {
        NumFmt( dirscan_size( &g_dir, row ), size );
        _snprintf( buf, (size_t)buflen, " %-39.39s %13s  %-16.16s %-4.4s", disp, size, date, attr );
    }
}

static int arc_row_icon( void *user, int row )
{
    char name[SZ_MAX_NAME];             /* whole: the extension picks the icon */

    (void)user;
    if ( view_is_up( row ) ) {
        return icon_up();
    }
    if ( view_is_dir( row ) ) {
        return icon_folder();
    }
    view_row_name( row, name, sizeof( name ) );
    return icon_for_name( name );
}

static int disk_row_icon( void *user, int row )
{
    (void)user;
    if ( dirscan_is_up( &g_dir, row ) ) {
        return icon_up();
    }
    if ( dirscan_kind( &g_dir, row ) == DS_DRIVE ) {
        return icon_drive();
    }
    if ( dirscan_is_dir( &g_dir, row ) ) {
        return icon_folder();
    }
    return icon_for_name( dirscan_name( &g_dir, row ) );
}

static void on_marks_changed( void *user )
{
    (void)user;
    update_status();
}

/* ".." is a way out, not a thing to extract; on the disk the drives are
   not either - everything else takes a mark, folders included */
static int row_can_mark( void *user, int row )
{
    (void)user;
    if ( disk_mode() ) {
        return !dirscan_is_up( &g_dir, row ) && dirscan_kind( &g_dir, row ) != DS_DRIVE;
    }
    return !view_is_up( row );
}

/* ------------------------------------------------------------------ */
/* the title, the status line and the Options marks                    */
/* ------------------------------------------------------------------ */

static void update_title( void )
{
    char name[36], text[48];

    if ( g_arc == NULL || !g_path[0] ) {
        title_set( "XArchive" );
        return;
    }
    shorten_tail( name, 35 - 12, base_name( g_path ) );
    sprintf( text, "XArchive - %s", name );
    title_set( text );
}

/* Which archive, where in it, how much is marked - the right-hand end
   built first, so a deep folder cannot push "F1 Help" off the line. */
static void update_status( void )
{
    char line[STATUS_COLS + 40], right[64], name[STATUS_COLS + 2], where[STATUS_COLS + 2];
    int marked, room, nroom, wroom;

    if ( disk_mode() ) {
        if ( list.mark_count > 0 ) {
            sprintf( right, "  %d of %d marked%s   F1 Help ", list.mark_count,
                     dirscan_items( &g_dir ), g_dir.cut ? " (list full)" : "" );
        } else {
            sprintf( right, "  %d items%s   F1 Help ", dirscan_items( &g_dir ),
                     g_dir.cut ? " (list full)" : "" );
        }
        room = STATUS_COLS - (int)strlen( right ) - 2;
        if ( room < 8 ) {
            room = 8;
        }
        shorten_tail( where, room, g_dir.drives ? "Drives" : g_dir.path[0] ? g_dir.path : "?" );
        sprintf( line, " %s%s", where, right );
        status_set( line );
        return;
    }
    marked = list.mark_count;
    if ( marked > 0 ) {
        sprintf( right, "  %d of %d marked%s   F1 Help ", marked, ArcNumEntries( g_arc ),
                 ArcFlattenPaths() ? "  [flat]" : "" );
    } else if ( g_nest_depth > 0 ) {
        sprintf( right, "  %d entries  [%d deep]%s   F1 Help ", ArcNumEntries( g_arc ), g_nest_depth,
                 ArcFlattenPaths() ? "  [flat]" : "" );
    } else {
        sprintf( right, "  %d entries%s   F1 Help ", ArcNumEntries( g_arc ),
                 ArcFlattenPaths() ? "  [flat]" : "" );
    }
    room = STATUS_COLS - (int)strlen( right ) - 4;
    if ( room < 8 ) {
        room = 8;
    }
    wroom = room - 8;
    if ( wroom < 4 ) {
        wroom = 4;
    }
    shorten_tail( where, wroom, view_cwd()[0] ? view_cwd() : "\\" );
    nroom = room - (int)strlen( where );
    if ( nroom < 4 ) {
        nroom = 4;
    }
    shorten_tail( name, nroom, base_name( g_path ) );
    sprintf( line, " %s : %s%s", name, where, right );
    status_set( line );
}

enum { OP_FOLDER_VIEW, OP_KEEP_FOLDERS, OP_AUTO83, OP_GRAPHICS };

static void set_mark( int item, int on, const char *text )
{
    char label[26];

    if ( op_menu < 0 ) {
        return;
    }
    sprintf( label, "%s %s", on ? "*" : " ", text );
    menu_relabel( op_menu, item, label );
}

/* each a switch with a '*' while it is on; Graphics mode is marked from
   the PREFERENCE, which is what the item changes, not from the screen */
static void update_menu_marks( void )
{
    set_mark( OP_FOLDER_VIEW, view_is_folder_mode(), "Folder View" );
    set_mark( OP_KEEP_FOLDERS, !ArcFlattenPaths(), "Extract Pathnames" );
    set_mark( OP_AUTO83, !ArcPrefAskShortNames(), "Auto-rename to 8.3" );
    set_mark( OP_GRAPHICS, ArcPrefGraphics(), "Graphics mode" );
}

/* everything, and then the status line, which the drawing leaves blank */
static void repaint( void )
{
    update_title();
    df_clear( FG_WHITE, BG_BLUE );
    title_draw();
    list_draw( &list );
    menu_draw();
    status_draw();
    update_status();
}

/* the title row 0, the menu row 1, the status line the last, and the
   list everything between - the graphics screen has thirty rows */
static void apply_screen_layout( void )
{
    int rows = df_rows(), cols = df_cols();

    if ( rows < 5 ) {
        rows = 25;
    }
    if ( cols < 40 ) {
        cols = 80;
    }
    list_region( &list, 2, 0, rows - 2, cols - 1 );
}

/* ------------------------------------------------------------------ */
/* the disk, when it is not there                                      */
/* ------------------------------------------------------------------ */

static int disk_ask( void )
{
    if ( !crit_hit() ) {
        return 0;
    }
    return crit_ask();
}

static int rescan_disk( void )
{
    int was_drives = g_dir.drives, rows;

    for ( ;; ) {
        crit_clear();
        rows = dirscan_rescan( &g_dir );
        if ( !disk_ask() ) {
            break;
        }
        if ( !was_drives ) {
            dirscan_show_directory( &g_dir );
        }
    }
    return rows;
}

/* again only while the row is still what was asked for: a failure part
   way can leave another listing on show */
static int enter_disk( int row )
{
    char want[260];
    int kind = dirscan_kind( &g_dir, row ), changed;

    strncpy( want, dirscan_name( &g_dir, row ), sizeof( want ) - 1 );
    want[sizeof( want ) - 1] = 0;
    for ( ;; ) {
        crit_clear();
        changed = dirscan_enter( &g_dir, row );
        if ( !disk_ask() ) {
            break;
        }
        if ( dirscan_kind( &g_dir, row ) != kind || strcmp( dirscan_name( &g_dir, row ), want ) ) {
            break;
        }
    }
    return changed;
}

static int up_disk( void )
{
    int changed;

    for ( ;; ) {
        crit_clear();
        changed = dirscan_up( &g_dir );
        if ( !disk_ask() ) {
            break;
        }
    }
    return changed;
}

/* new rows drop the marks: a mark is a row number */
static void refresh_list( void )
{
    if ( g_arc ) {
        list_data( &list, view_rebuild(), fill_row, NULL );
        list.row_icon = arc_row_icon;
    } else {
        list_data( &list, rescan_disk(), fill_disk_row, &g_dir );
        list.row_icon = disk_row_icon;
    }
    list_select( &list, 0 );
    repaint();
}

/* ------------------------------------------------------------------ */
/* passwords and retries                                               */
/* ------------------------------------------------------------------ */

static int ask_password( int retry )
{
    char text[TF_TEXT_MAX];
    int ok;

    text[0] = 0;
    ok = input_box( FG_BLUE, BG_WHITE, "Password",
                    retry ? "That password was wrong.  Try again:"
                          : "This archive is encrypted.  Password:",
                    text, 0, IB_MASKED ) == IB_OK;
    repaint();
    if ( !ok ) {
        return 0;
    }
    strncpy( g_password, text, sizeof( g_password ) - 1 );
    g_password[sizeof( g_password ) - 1] = 0;
    return 1;
}

/* do { rc = ArcSomething(...); } while ( password_retry( rc ) ); */
static int password_retry( int rc )
{
    if ( rc != SZ_ERR_PASSWORD && rc != SZ_ERR_BADPASS ) {
        return 0;
    }
    if ( !ask_password( rc == SZ_ERR_BADPASS ) ) {
        return 0;
    }
    ArcSetPassword( g_arc, g_password );
    return 1;
}

/* again, if what stopped it was the disk and the answer was Retry */
static int disk_retry( int failed )
{
    if ( !failed || !disk_ask() ) {
        return 0;
    }
    crit_clear();
    return 1;
}

/* ------------------------------------------------------------------ */
/* the overwrite question                                              */
/* ------------------------------------------------------------------ */

static int overwrite_box( const char *path )
{
    POPUP box;
    BUTTON yes, no, yes_all, no_all;
    int rows = df_rows(), cols = df_cols(), bw = 66, bh = 7, left, top, room, row, col;
    int answer = -1;
    char shown[80];
    EVENT ev;

    if ( bw > cols ) {
        bw = cols;
    }
    left = (cols - bw) / 2;
    top = (rows - bh) / 2;
    room = bw - 6;
    if ( room > (int)sizeof( shown ) - 1 ) {
        room = (int)sizeof( shown ) - 1;
    }
    shorten_tail( shown, room, path );
    popup_init( &box, FG_BLACK, BG_WHITE, "File Exists" );
    box.border = BORDER_DOUBLE;
    box.just = JUST_CENTER;
    popup_place( &box, top, left, top + bh - 1, left + bw - 1 );
    popup_draw( &box );
    row = top + bh - 2;
    col = left + (bw - 40) / 2;
    button_init( &yes, FG_WHITE, BG_CYAN, "Yes" );
    button_init( &no, FG_WHITE, BG_CYAN, "No" );
    button_init( &yes_all, FG_WHITE, BG_CYAN, "Yes All" );
    button_init( &no_all, FG_WHITE, BG_CYAN, "No All" );
    button_row( &yes, row, col );
    col += 7 + 2;
    button_row( &no, row, col );
    col += 6 + 2;
    button_row( &yes_all, row, col );
    col += 11 + 2;
    button_row( &no_all, row, col );
    df_text( top + 2, left + 3, "This file already exists.  Overwrite it?", FG_BLACK, BG_WHITE );
    df_text( top + 3, left + 3, shown, FG_BLACK, BG_WHITE );
    button_draw( &yes );
    button_draw( &no );
    button_draw( &yes_all );
    button_draw( &no_all );
    while ( answer < 0 ) {
        ev_wait( &ev );
        if ( ev.kind == EV_KEY ) {
            if ( ev.key == 'y' || ev.key == 'Y' || ev.key == KEY_ENTER ) {
                answer = ARC_OW_YES;
            } else if ( ev.key == 'n' || ev.key == 'N' || ev.key == KEY_ESC ) {
                answer = ARC_OW_NO;
            } else if ( ev.key == 'a' || ev.key == 'A' ) {
                answer = ARC_OW_YESALL;
            } else if ( ev.key == 'o' || ev.key == 'O' ) {
                answer = ARC_OW_NOALL;
            }
        } else if ( ev.kind == EV_MOUSE && (ev.press & LEFT_BUTTON) ) {
            if ( button_hit( &yes, ev.x, ev.y ) ) {
                answer = ARC_OW_YES;
            } else if ( button_hit( &no, ev.x, ev.y ) ) {
                answer = ARC_OW_NO;
            } else if ( button_hit( &yes_all, ev.x, ev.y ) ) {
                answer = ARC_OW_YESALL;
            } else if ( button_hit( &no_all, ev.x, ev.y ) ) {
                answer = ARC_OW_NOALL;
            }
        }
    }
    popup_restore( &box );
    popup_free( &box );
    return answer;
}

static int tui_overwrite( void *user, const char *path )
{
    (void)user;
    return overwrite_box( path );
}

/* ------------------------------------------------------------------ */
/* progress                                                            */
/* ------------------------------------------------------------------ */

/* "12/345  " and as much of the name as is left of the box's width */
static int tui_progress( void *user, int idx, int total, const char *name )
{
    char line[PB_ROOM + 2], nm[PB_ROOM + 2];
    EVENT ev;
    int len;

    (void)user;
    if ( !prog_is_open() ) {
        /* no box, but still a job that Esc should be able to stop */
        while ( ev_poll( &ev ) ) {
            if ( ev.kind == EV_KEY && ev.key == KEY_ESC ) {
                return 0;
            }
        }
        return 1;
    }
    sprintf( line, "%d/%d  ", idx + 1, total );
    len = (int)strlen( line );
    shorten_tail( nm, PB_ROOM - len, name );
    strcat( line, nm );
    return prog_update( line, (u32)(idx + 1), (u32)(total > 0 ? total : 1) );
}

/* finding the files (no total yet), packing them by KB of input, and
   clearing the Archive attribute with the bar full */
static int tui_comp_progress( void *user, int idx, int total, const char *name,
                              UInt32 done_kb, UInt32 total_kb )
{
    char line[PB_ROOM + 2], nm[PB_ROOM + 2];
    int len;

    (void)user;
    if ( !prog_is_open() ) {
        return 1;
    }
    if ( total == ARC_COMP_CLEARING ) {
        strcpy( line, "Clearing Archive attribute: " );
    } else if ( total < 0 ) {
        sprintf( line, "Scanning: %d  ", idx + 1 );
    } else {
        sprintf( line, "%d/%d  ", idx + 1, total );
    }
    len = (int)strlen( line );
    shorten_tail( nm, PB_ROOM - len, name );
    strcat( line, nm );
    return prog_update( line, done_kb, total_kb > 0 ? total_kb : 1 );
}

static const char *mem_hint( int rc )
{
    return rc == SZ_ERR_MEMORY ? ArcMemFailHint() : ArcNoRamHint();
}

/* ------------------------------------------------------------------ */
/* opening and closing                                                 */
/* ------------------------------------------------------------------ */

static void open_path( const char *path )
{
    char text[192], shown[48];
    int rc;

    nest_unwind_all();
    view_attach( NULL );
    if ( g_arc ) {
        ArcClose( g_arc );
        g_arc = NULL;
    }
    g_password[0] = 0;              /* a different archive: forget the last one */
    crit_clear();
    do {
        wait_open( "Opening, please wait..." );
        rc = ArcOpen( path, &g_arc );
        wait_close();
    } while ( disk_retry( rc != SZ_OK ) );
    /* an archive whose directory is encrypted cannot be listed without
       the password, so this is asked before there is an archive */
    while ( rc == SZ_ERR_PASSWORD || rc == SZ_ERR_BADPASS ) {
        if ( !ask_password( rc == SZ_ERR_BADPASS ) ) {
            break;
        }
        wait_open( "Opening, please wait..." );
        rc = ArcOpenPw( path, g_password, &g_arc );
        wait_close();
    }
    if ( rc != SZ_OK ) {
        g_arc = NULL;
        if ( crit_hit() ) {
            /* the disk, and it has been said: Cancel was the answer */
        } else if ( rc == SZ_ERR_NORAM || rc == SZ_ERR_MEMORY ) {
            msg( "Open", mem_hint( rc ) );
        } else if ( rc == SZ_ERR_UNSUPPORTED ) {
            msg( "Open", ArcUnsupportedHint( NULL ) );
        } else if ( rc == SZ_ERR_SIG ) {
            shorten_tail( shown, 40, base_name( path ) );
            sprintf( text, "%s is not an archive or image file.", shown );
            msg( "Open", text );
        } else if ( rc == SZ_ERR_PASSWORD || rc == SZ_ERR_BADPASS ) {
            msg( "Open", "This archive is encrypted and was not opened." );
        } else {
            sprintf( text, "Cannot open archive:\n%s", ArcErrorText( rc ) );
            msg( "Open", text );
        }
        refresh_list();
        return;
    }
    strncpy( g_path, path, sizeof( g_path ) - 1 );
    g_path[sizeof( g_path ) - 1] = 0;
    view_attach( g_arc );
    refresh_list();
}

/* close whatever is open and show the disk again; on the disk, a rescan
   - which is what F2 was for after changing a floppy */
static void act_browse_disk( void )
{
    nest_unwind_all();
    view_attach( NULL );
    if ( g_arc ) {
        ArcClose( g_arc );
        g_arc = NULL;
        g_path[0] = 0;
    }
    refresh_list();
}

static void act_exit( void )
{
    running = 0;
}

/* ------------------------------------------------------------------ */
/* extracting                                                          */
/* ------------------------------------------------------------------ */

/* where the extract box starts: the last place an extraction went, or
   the current directory - which says where, where "." would not */
static void default_dest( char *dst, int size )
{
    if ( g_dest[0] ) {
        strncpy( dst, g_dest, (size_t)size - 1 );
        dst[size - 1] = 0;
        return;
    }
    if ( !getcwd( dst, (size_t)size ) || !dst[0] ) {
        strcpy( dst, "." );
    }
}

/* the extraction into g_dest, and the word about how it went */
static int run_extract( const int *items, int count )
{
    char text[192];
    int rc;

    g_ask_names_off = 0;
    prog_open( "Extracting" );
    crit_clear();
    do {
        rc = items ? ArcExtractItems( g_arc, items, count, g_dest, tui_progress, NULL )
                   : ArcExtractAll( g_arc, g_dest, tui_progress, NULL );
    } while ( password_retry( rc ) || disk_retry( rc != SZ_OK && rc != SZ_ERR_CANCEL ) );
    prog_close();
    repaint();
    if ( rc != SZ_OK && crit_hit() ) {
        rc = SZ_ERR_CANCEL;         /* the disk was asked about already */
    }
    if ( rc == SZ_OK ) {
        if ( items ) {
            sprintf( text, "Done.  %d entr%s extracted.", count, count == 1 ? "y" : "ies" );
        } else {
            strcpy( text, "Done.  All entries extracted." );
        }
        msg( "Extract", text );
    } else if ( rc == SZ_ERR_CANCEL ) {
        msg( "Extract", "Extraction cancelled." );
    } else if ( rc == SZ_ERR_UNSUPPORTED ) {
        msg( "Extract", ArcUnsupportedHint( g_arc ) );
    } else if ( rc == SZ_ERR_NORAM || rc == SZ_ERR_MEMORY ) {
        msg( "Extract", mem_hint( rc ) );
    } else {
        sprintf( text, "Extraction failed:\n%s", ArcErrorText( rc ) );
        msg( "Extract", text );
    }
    return rc;
}

/* asked with a tree of the drives and folders over the path it stands
   for; walk to a folder and type "\NEW" after it for one that is not
   there yet.  Enter still takes what the field says. */
static void do_extract( const int *items, int count, const char *prompt )
{
    char start[SZ_MAX_NAME * 2];

    default_dest( start, sizeof( start ) );
    if ( !file_box( "Extract", prompt, FB_DIR, NULL, start, NULL, g_dest, sizeof( g_dest ) ) ) {
        repaint();
        return;
    }
    run_extract( items, count );
}

/* the marked rows as archive entries; a folder row is everything under
   it.  A malloc'd array, or NULL with *count_out 0. */
static int *collect_marked( int *count_out )
{
    int n = list.mark_count, nrows, *rows, *idx, max = view_max_expanded();

    *count_out = 0;
    if ( n <= 0 ) {
        return NULL;
    }
    rows = (int *)malloc( (size_t)n * sizeof( int ) );
    if ( rows == NULL ) {
        return NULL;
    }
    nrows = list_get_marked( &list, rows, n );
    idx = (int *)malloc( (size_t)(max > 0 ? max : 1) * sizeof( int ) );
    if ( idx == NULL ) {
        free( rows );
        return NULL;
    }
    *count_out = view_expand( rows, nrows, idx, max );
    free( rows );
    if ( *count_out <= 0 ) {
        *count_out = 0;
        free( idx );
        return NULL;
    }
    return idx;
}

/* on the disk: the marked rows, or the highlighted one - not the way up
   or a drive, and with files_only not a folder either */
static int *collect_disk_rows( int *count, int files_only )
{
    int n = list.mark_count, at, kept = 0, row, *rows;

    *count = 0;
    if ( list.count <= 0 || g_dir.drives ) {
        return NULL;
    }
    rows = (int *)malloc( (size_t)(n > 0 ? n : 1) * sizeof( int ) );
    if ( rows == NULL ) {
        return NULL;
    }
    if ( n > 0 ) {
        n = list_get_marked( &list, rows, n );
    } else {
        rows[0] = list.sel;
        n = 1;
    }
    for ( at = 0; at < n; at++ ) {
        row = rows[at];
        if ( row < 0 || row >= g_dir.count || dirscan_is_up( &g_dir, row )
             || dirscan_kind( &g_dir, row ) == DS_DRIVE ) {
            continue;
        }
        if ( files_only && dirscan_is_dir( &g_dir, row ) ) {
            continue;
        }
        rows[kept++] = row;
    }
    if ( kept == 0 ) {
        free( rows );
        return NULL;
    }
    *count = kept;
    return rows;
}

/* F5 on the disk: the highlighted or marked archive files, whole, into
   one folder asked for once; one that turns out not to be an archive is
   counted and named at the end rather than stopping the rest */
static void extract_from_disk( void )
{
    char prompt[96], shown[48], first_bad[SZ_MAX_NAME], start[SZ_MAX_NAME * 2];
    char full[SZ_MAX_NAME], text[1024];
    int n = 0, at, done = 0, not_arc = 0, failed = 0, rc = SZ_OK, last_err = SZ_OK;
    int *rows = collect_disk_rows( &n, 1 );
    ArcFile *one;

    if ( rows == NULL ) {
        msg( "Extract", "Highlight an archive to extract it, or mark several.\n"
                        "\n"
                        "Enter opens an archive instead, to pick what to\n"
                        "take out of it." );
        return;
    }
    if ( n == 1 ) {
        shorten_tail( shown, 40, dirscan_name( &g_dir, rows[0] ) );
        sprintf( prompt, "Extract '%s' to which folder?", shown );
    } else {
        sprintf( prompt, "Extract %d archives to which folder?", n );
    }
    default_dest( start, sizeof( start ) );
    if ( !file_box( "Extract", prompt, FB_DIR, NULL, start, NULL, g_dest, sizeof( g_dest ) ) ) {
        free( rows );
        repaint();
        return;
    }
    first_bad[0] = 0;
    g_ask_names_off = 0;
    for ( at = 0; at < n && rc != SZ_ERR_CANCEL; at++ ) {
        one = NULL;
        dirscan_full( &g_dir, rows[at], full, sizeof( full ) );
        g_password[0] = 0;
        crit_clear();
        do {
            wait_open( "Opening, please wait..." );
            rc = ArcOpen( full, &one );
            wait_close();
        } while ( disk_retry( rc != SZ_OK ) );
        while ( rc == SZ_ERR_PASSWORD || rc == SZ_ERR_BADPASS ) {
            if ( !ask_password( rc == SZ_ERR_BADPASS ) ) {
                rc = SZ_ERR_CANCEL;
                break;
            }
            wait_open( "Opening, please wait..." );
            rc = ArcOpenPw( full, g_password, &one );
            wait_close();
        }
        if ( rc != SZ_OK ) {
            if ( rc == SZ_ERR_CANCEL ) {
                break;
            }
            if ( rc == SZ_ERR_SIG ) {
                not_arc++;
            } else {
                failed++;
                last_err = rc;
            }
            if ( !first_bad[0] ) {
                strcpy( first_bad, dirscan_name( &g_dir, rows[at] ) );
            }
            rc = SZ_OK;
            continue;
        }
        prog_open( "Extracting" );
        crit_clear();
        for ( ;; ) {
            rc = ArcExtractAll( one, g_dest, tui_progress, NULL );
            if ( rc == SZ_ERR_PASSWORD || rc == SZ_ERR_BADPASS ) {
                if ( !ask_password( rc == SZ_ERR_BADPASS ) ) {
                    rc = SZ_ERR_CANCEL;
                    break;
                }
                ArcSetPassword( one, g_password );
                continue;
            }
            if ( disk_retry( rc != SZ_OK && rc != SZ_ERR_CANCEL ) ) {
                continue;
            }
            break;
        }
        prog_close();
        ArcClose( one );
        if ( rc == SZ_OK ) {
            done++;
        } else if ( rc != SZ_ERR_CANCEL ) {
            failed++;
            last_err = rc;
            if ( !first_bad[0] ) {
                strcpy( first_bad, dirscan_name( &g_dir, rows[at] ) );
            }
        }
    }
    free( rows );
    g_password[0] = 0;
    refresh_list();                 /* the destination may be the folder on show */
    if ( rc == SZ_ERR_CANCEL ) {
        sprintf( text, "Extraction cancelled.  %d of %d archive%s done.", done, n, n == 1 ? "" : "s" );
    } else if ( n == 1 && done == 1 ) {
        strcpy( text, "Done.  All entries extracted." );
    } else if ( n == 1 && not_arc ) {
        shorten_tail( shown, 40, first_bad );
        sprintf( text, "%s is not an archive or image file.", shown );
    } else if ( n == 1 && last_err == SZ_ERR_UNSUPPORTED ) {
        strcpy( text, ArcUnsupportedHint( NULL ) );
    } else if ( n == 1 && (last_err == SZ_ERR_NORAM || last_err == SZ_ERR_MEMORY) ) {
        strcpy( text, mem_hint( last_err ) );
    } else if ( n == 1 ) {
        sprintf( text, "Extraction failed:\n%s", ArcErrorText( last_err ) );
    } else {
        sprintf( text, "Done.  %d of %d archives extracted.", done, n );
        if ( not_arc ) {
            sprintf( text + strlen( text ), "\n%d %s not an archive.", not_arc,
                     not_arc == 1 ? "was" : "were" );
        }
        if ( failed ) {
            sprintf( text + strlen( text ), "\n%d could not be extracted.", failed );
        }
        if ( first_bad[0] ) {
            shorten_tail( shown, 40, first_bad );
            sprintf( text + strlen( text ), "\nThe first was %s.", shown );
        }
    }
    msg( "Extract", text );
}

/* F5: the marked entries, or all of them */
static void act_extract( void )
{
    char prompt[80];
    int n = 0, *idx;

    if ( disk_mode() ) {
        extract_from_disk();
        return;
    }
    if ( list.mark_count <= 0 ) {
        do_extract( NULL, 0, "Extract all files to which folder?" );
        return;
    }
    /* marks that cannot be listed are not a reason to take everything */
    idx = collect_marked( &n );
    if ( idx == NULL ) {
        msg( "Extract", "Out of memory." );
        return;
    }
    sprintf( prompt, "Extract %d marked entr%s to which folder?", n, n == 1 ? "y" : "ies" );
    do_extract( idx, n, prompt );
    free( idx );
}

static void act_extract_all( void )
{
    if ( disk_mode() ) {
        extract_from_disk();
        return;
    }
    do_extract( NULL, 0, "Extract all files to which folder?" );
}

/* ------------------------------------------------------------------ */
/* compressing                                                         */
/* ------------------------------------------------------------------ */

/* the item's own name for one item, the folder's for several (ARCHIVE
   at a drive's root), in the folder on show - and never the name of
   something going into it.  Eight characters where 8.3 is all there is. */
static void default_archive_name( const int *rows, int n, int fmt, char *dst, int size )
{
    char base[SZ_MAX_NAME], cand[SZ_MAX_NAME + 8], *dot;
    const char *ext = fmt == ARC_CF_7Z ? ".7Z" : ".ZIP", *dir = g_dir.path;
    int at;

    strncpy( base, n == 1 ? dirscan_name( &g_dir, rows[0] ) : base_name( dir ), sizeof( base ) - 1 );
    base[sizeof( base ) - 1] = 0;
    dot = strrchr( base, '.' );
    if ( dot && dot != base ) {
        *dot = 0;
    }
    if ( !base[0] ) {
        strcpy( base, "ARCHIVE" );
    }
    if ( PmNamesNeed83( dir ) ) {
        base[8] = 0;
    }
    for ( at = 0; at < n; at++ ) {
        sprintf( cand, "%s%s", base, ext );
        if ( !stricmp( cand, dirscan_name( &g_dir, rows[at] ) ) ) {
            strcpy( base, "ARCHIVE" );
            break;
        }
    }
    /* the folder too when the whole of it fits, else the bare name (which
       act_compress puts in the folder, or says it cannot) */
    at = dir[0] && dir[strlen( dir ) - 1] != '\\';
    dst[0] = 0;
    if ( strlen( dir ) + at + strlen( base ) + strlen( ext ) < (size_t)size ) {
        strcpy( dst, dir );
        if ( at ) {
            strcat( dst, "\\" );
        }
    }
    if ( strlen( dst ) + strlen( base ) + strlen( ext ) < (size_t)size ) {
        strcat( dst, base );
        strcat( dst, ext );
    }
}

/* what happened, sizes and all, and anything that was left out - a file
   left out of a backup is one somebody believes they still have */
static void compress_report( ArcCompJob *job, const char *archive, int rc )
{
    char text[SZ_MAX_NAME * 2 + 640], shown[48], prob[52], b1[NUM_FMT_MAX], b2[NUM_FMT_MAX];
    UInt32 in, out, pct;
    int skipped;

    shorten_tail( shown, 40, base_name( archive ) );
    shorten_tail( prob, 46, ArcCompProblem( job ) );
    if ( rc == SZ_OK ) {
        in = ArcCompInBytes( job );
        out = ArcCompOutBytes( job );
        pct = in > 0 ? (in > 0x01000000UL ? out / (in / 100) : out * 100 / in) : 100;
        skipped = ArcCompSkipped( job );
        sprintf( text, "Done.  %s holds %d file%s.\n\n%s bytes uncompressed\n"
                       "%s bytes compressed (%lu%%).",
                 shown, ArcCompFiles( job ), ArcCompFiles( job ) == 1 ? "" : "s",
                 NumFmt( in, b1 ), NumFmt( out, b2 ), (unsigned long)pct );
        if ( ArcCompDictSize( job ) >= 1024UL * 1024 ) {
            sprintf( text + strlen( text ), "\n\nExtracting it will need about %lu MB of memory.",
                     (unsigned long)(ArcCompDictSize( job ) / (1024UL * 1024)) );
        }
        if ( skipped ) {
            sprintf( text + strlen( text ), "\n\n%d file%s left out:", skipped,
                     skipped == 1 ? " was" : "s were" );
            if ( ArcCompDuplicates( job ) ) {
                sprintf( text + strlen( text ), "\n  %d with a name already in it",
                         ArcCompDuplicates( job ) );
            }
            if ( ArcCompUnreadable( job ) ) {
                sprintf( text + strlen( text ), "\n  %d that could not be read", ArcCompUnreadable( job ) );
            }
            if ( ArcCompTooLong( job ) ) {
                sprintf( text + strlen( text ), "\n  %d with a name too long", ArcCompTooLong( job ) );
            }
            if ( ArcCompTooBig( job ) ) {
                sprintf( text + strlen( text ), "\n  %d of 2 GB or more", ArcCompTooBig( job ) );
            }
            sprintf( text + strlen( text ), "\nThe first was %s", prob );
        }
        msg( "Compress", text );
        return;
    }
    switch ( rc ) {
    case SZ_ERR_CANCEL:
        strcpy( text, "Compression cancelled.  Nothing was written." );
        break;
    case SZ_ERR_NOFILES:
        if ( ArcCompMissing( job ) ) {
            sprintf( text, "Nothing to compress: %s was not found.", prob );
        } else {
            strcpy( text, "Nothing to compress: everything was excluded." );
        }
        break;
    case SZ_ERR_OPEN:
        sprintf( text, "Cannot read the exclusion list\n%s", prob );
        break;
    case SZ_ERR_READ:
        sprintf( text, "A file could not be read part way through:\n%s\n\nNothing was written.", prob );
        break;
    case SZ_ERR_WRITE:
        sprintf( text, "Cannot write %s.\n\nThe disk may be full or write-protected.  "
                       "Nothing was written.", shown );
        break;
    case SZ_ERR_TOOBIG:
        strcpy( text, "Too much for one archive.  A zip holds at most 65,535 entries, "
                      "and XArc writes archives of up to 2 GB." );
        break;
    case SZ_ERR_MEMORY:
    case SZ_ERR_NORAM:
        strcpy( text, "Not enough memory to compress.  A zip needs about 300 KB.  A 7-Zip "
                      "archive needs at least 1.5 MB, and uses a bigger dictionary when "
                      "there is more." );
        break;
    default:
        sprintf( text, "Compression failed:\n%s", ArcErrorText( rc ) );
        break;
    }
    msg( "Compress", text );
}

static void act_compress( void )
{
    char what[96], shown[48], full[SZ_MAX_NAME], question[160], tmp[CD_PATH_MAX];
    const char *leaf;
    int n = 0, at, rc, sep, *rows;
    COMPRESULT res;
    ArcCompJob *job;
    FILE *there;

    if ( !disk_mode() ) {
        msg( "Compress", "Compress works on files on the disk, and this is an archive.\n\n"
                         "Press Backspace to leave it, mark the files and directories to "
                         "compress, and press F8." );
        return;
    }
    rows = collect_disk_rows( &n, 0 );
    if ( rows == NULL ) {
        msg( "Compress", "Mark the files and folders to compress with Space (or the right "
                         "mouse button), or highlight one, and press F8." );
        return;
    }
    /* a folder too big for the list: marking all of it is not all of it */
    if ( g_dir.cut && n > 1
         && msg_box( FG_BLACK, BG_WHITE, "Compress",
                     "The list holds only part of this folder, and only what is listed "
                     "goes into the archive.\n\nCompress the listed items anyway?",
                     DB_YESNO ) != ANS_YES ) {
        free( rows );
        repaint();
        return;
    }
    if ( n == 1 ) {
        shorten_tail( shown, 40, dirscan_name( &g_dir, rows[0] ) );
        sprintf( what, "%s %s", dirscan_is_dir( &g_dir, rows[0] ) ? "The folder" : "The file", shown );
    } else {
        shorten_tail( shown, 40, g_dir.path );
        sprintf( what, "%d marked items in %s", n, shown );
    }
    memset( &res, 0, sizeof( res ) );
    res.fmt = ArcPrefCompressFormat();
    res.keep_paths = ArcPrefCompressFolders();
    res.clear_arc = ArcPrefCompressClearArchive();
    default_archive_name( rows, n, res.fmt, res.archive, sizeof( res.archive ) );
    if ( !compress_dialog( what, &res ) ) {
        free( rows );
        repaint();
        return;
    }
    /* a name with no folder in it goes in the folder on show */
    if ( !strchr( res.archive, '\\' ) && !strchr( res.archive, ':' ) ) {
        sep = g_dir.path[0] && g_dir.path[strlen( g_dir.path ) - 1] != '\\';
        if ( strlen( g_dir.path ) + sep + strlen( res.archive ) >= CD_PATH_MAX ) {
            free( rows );
            msg( "Compress", "The folder and the archive's name together are too long "
                             "for a path." );
            return;
        }
        strcpy( tmp, res.archive );
        strcpy( res.archive, g_dir.path );
        if ( sep ) {
            strcat( res.archive, "\\" );
        }
        strcat( res.archive, tmp );
    }
    ArcPrefSetCompressFormat( res.fmt );
    ArcPrefSetCompressFolders( res.keep_paths );
    ArcPrefSetCompressClearArchive( res.clear_arc );
    ArcPrefSave();
    /* replacing an archive is the user's call */
    there = fopen( res.archive, "rb" );
    if ( there ) {
        fclose( there );
        shorten_tail( shown, 40, base_name( res.archive ) );
        sprintf( question, "%s already exists.\n\nReplace it with the new archive?", shown );
        if ( msg_box( FG_BLACK, BG_WHITE, "Compress", question, DB_YESNO ) != ANS_YES ) {
            free( rows );
            repaint();
            return;
        }
    }
    job = ArcCompCreate();
    if ( job == NULL ) {
        free( rows );
        msg( "Compress", "Out of memory." );
        return;
    }
    ArcCompSetFormat( job, res.fmt );
    ArcCompSetPaths( job, res.keep_paths );
    ArcCompSetClearArchive( job, res.clear_arc );
    rc = ArcCompExclude( job, res.exclude );
    for ( at = 0; at < n && rc == SZ_OK; at++ ) {
        dirscan_full( &g_dir, rows[at], full, sizeof( full ) );
        rc = ArcCompAdd( job, full );
    }
    free( rows );
    if ( rc == SZ_OK ) {
        prog_open( "Compressing" );
        crit_clear();
        rc = ArcCompScan( job, res.archive, tui_comp_progress, NULL );
        if ( rc == SZ_OK ) {
            rc = ArcCompRun( job, res.archive, tui_comp_progress, NULL );
        }
        prog_close();
    }
    if ( rc != SZ_OK && crit_hit() ) {
        rc = SZ_ERR_CANCEL;
    }
    /* the new archive is in the folder on show, usually: the bar on it */
    refresh_list();
    if ( rc == SZ_OK ) {
        leaf = base_name( res.archive );
        for ( at = 0; at < g_dir.count; at++ ) {
            if ( !stricmp( dirscan_name( &g_dir, at ), leaf ) ) {
                list_select( &list, at );
                break;
            }
        }
        repaint();
    }
    compress_report( job, res.archive, rc );
    ArcCompFree( job );
    /* and open it: what was just made is what will be looked at next */
    if ( rc == SZ_OK ) {
        open_path( res.archive );
    }
}

/* ------------------------------------------------------------------ */
/* archives in archives                                                */
/* ------------------------------------------------------------------ */

/* TEMP, then TMP, then the current directory */
static void temp_root( char *dst, int size )
{
    const char *env = getenv( "TEMP" );
    int len;

    if ( env == NULL || !env[0] ) {
        env = getenv( "TMP" );
    }
    if ( env == NULL || !env[0] ) {
        env = ".";
    }
    strncpy( dst, env, (size_t)size - 1 );
    dst[size - 1] = 0;
    len = (int)strlen( dst );
    while ( len > 1 && (dst[len - 1] == '\\' || dst[len - 1] == '/') ) {
        dst[--len] = 0;
    }
}

/* worth opening rather than extracting with Enter.  .EXE is left out on
   purpose: a self-extractor opens, but so does every other program, and
   File > Browse Into is there for those. */
static int looks_like_archive( const char *name )
{
    static const char *const known[] = {
        ".7Z", ".ZIP", ".RAR", ".CAB", ".IMZ", ".IMG", ".IMA", ".DSK", NULL
    };
    const char *dot = NULL;
    char up[8];
    int at, len;

    if ( name == NULL ) {
        return 0;
    }
    for ( at = 0; name[at]; at++ ) {
        if ( name[at] == '.' ) {
            dot = name + at;
        }
    }
    if ( dot == NULL ) {
        return 0;
    }
    for ( len = 0; len < 7 && dot[len]; len++ ) {
        up[len] = (char)toupper( (u8)dot[len] );
    }
    up[len] = 0;
    for ( at = 0; known[at]; at++ ) {
        if ( !strcmp( up, known[at] ) ) {
            return 1;
        }
    }
    return len >= 2 && up[len - 1] == '_';     /* COMPRESS.EXE's: SETUP.EX_ */
}

/* one entry into "dir", and the name it was given - ArcFsName decides
   that, so it is asked afterwards.  Folders are left out for the length
   of it, and nobody is asked to name a temp file. */
static int extract_one_to( int index, const char *dir, char *out_name, int out_size )
{
    int one = index, saved_flat = ArcFlattenPaths(), saved_ask = g_ask_names_off, rc;
    const char *stored = ArcEntryName( g_arc, index );

    ArcSetFlattenPaths( 1 );
    g_ask_names_off = 1;
    do {
        rc = ArcExtractItems( g_arc, &one, 1, dir, tui_progress, NULL );
    } while ( password_retry( rc ) );
    if ( rc == SZ_OK ) {
        ArcFsName( out_name, out_size, stored ? stored : "", ArcEntryIsDir( g_arc, index ) );
    }
    g_ask_names_off = saved_ask;
    ArcSetFlattenPaths( saved_flat );
    return rc;
}

static void nest_drop_temp( NEST *level )
{
    if ( level->tmp_file[0] ) {
        remove( level->tmp_file );
    }
    if ( level->tmp_dir[0] ) {
        _rmdir( level->tmp_dir );
    }
    level->tmp_file[0] = level->tmp_dir[0] = 0;
}

/* out one level: to the archive AND the folder in it that was left, the
   bar on the row that was opened */
static void nest_back( void )
{
    NEST *level;

    if ( g_nest_depth <= 0 ) {
        return;
    }
    level = &g_nest[--g_nest_depth];
    view_attach( NULL );
    if ( g_arc ) {
        ArcClose( g_arc );                  /* the temp file's handle first */
    }
    nest_drop_temp( level );
    g_arc = level->arc;
    strcpy( g_path, level->path );
    strcpy( g_password, level->password );  /* each level has its own */
    level->arc = NULL;
    view_attach( g_arc );
    view_set_cwd( level->cwd );
    refresh_list();
    list_select( &list, level->sel );
    repaint();
}

static void nest_unwind_all( void )
{
    while ( g_nest_depth > 0 ) {
        nest_back();
    }
}

static void browse_into( int index )
{
    char root[SZ_MAX_NAME], dir[SZ_MAX_NAME], name[SZ_MAX_NAME], full[SZ_MAX_NAME * 2];
    char outer_pw[AC_MAX_PW + 1], text[40 + 64 + 160], shown[48];
    ArcFile *child = NULL;
    NEST *level;
    int rc;

    if ( g_arc == NULL || index < 0 || index >= ArcNumEntries( g_arc ) ) {
        return;
    }
    if ( ArcEntryIsDir( g_arc, index ) ) {
        msg( "Browse", "That entry is a folder, not an archive.\n\n"
                       "Folders inside an archive are already shown in the list, with their "
                       "contents beneath them." );
        return;
    }
    if ( g_nest_depth >= NEST_MAX ) {
        msg( "Browse", "That is as deep as this goes.\n\n"
                       "Extract the archive you are in and open the copy if you need to go "
                       "further." );
        return;
    }
    temp_root( root, sizeof( root ) );
    if ( strlen( root ) + 12 > sizeof( dir ) ) {
        msg( "Browse", "The TEMP folder's path is too long." );
        return;
    }
    sprintf( dir, "%s\\XARCN%d.TMP", root, g_nest_depth );
    _mkdir( dir );                          /* already there is not an error */
    prog_open( "Opening" );
    rc = extract_one_to( index, dir, name, sizeof( name ) );
    prog_close();
    repaint();
    if ( rc != SZ_OK ) {
        _rmdir( dir );
        if ( rc == SZ_ERR_NORAM || rc == SZ_ERR_MEMORY ) {
            msg( "Browse", mem_hint( rc ) );
        } else if ( rc == SZ_ERR_UNSUPPORTED ) {
            msg( "Browse", ArcUnsupportedHint( g_arc ) );
        } else if ( rc == SZ_ERR_CANCEL ) {
            msg( "Browse", "Cancelled." );
        } else {
            sprintf( text, "Cannot unpack that entry to look inside it:\n%s", ArcErrorText( rc ) );
            msg( "Browse", text );
        }
        return;
    }
    sprintf( full, "%s\\%s", dir, name );
    if ( strlen( full ) >= sizeof( level->tmp_file ) ) {
        remove( full );
        _rmdir( dir );
        msg( "Browse", "That entry's name is too long to unpack here." );
        return;
    }
    /* the outer archive's password as it now stands, for the way back */
    strcpy( outer_pw, g_password );
    wait_open( "Opening, please wait..." );
    rc = ArcOpen( full, &child );
    wait_close();
    repaint();
    /* an archive inside may have a password of its own */
    while ( rc == SZ_ERR_PASSWORD || rc == SZ_ERR_BADPASS ) {
        char inner[AC_MAX_PW + 1];

        if ( !ask_password( rc == SZ_ERR_BADPASS ) ) {
            break;
        }
        strcpy( inner, g_password );
        wait_open( "Opening, please wait..." );
        rc = ArcOpenPw( full, inner, &child );
        wait_close();
        repaint();
    }
    if ( rc != SZ_OK ) {
        remove( full );
        _rmdir( dir );
        shorten_tail( shown, 40, ArcEntryName( g_arc, index ) );
        sprintf( text, "'%s' is not an archive this program can open.\n%s", shown, ArcErrorText( rc ) );
        msg( "Browse", text );
        return;
    }
    level = &g_nest[g_nest_depth++];
    level->arc = g_arc;
    strcpy( level->path, g_path );
    strcpy( level->password, outer_pw );
    strcpy( level->cwd, view_cwd() );
    level->sel = list.sel;
    strcpy( level->tmp_file, full );
    strcpy( level->tmp_dir, dir );
    g_arc = child;
    strncpy( g_path, full, sizeof( g_path ) - 1 );
    g_path[sizeof( g_path ) - 1] = 0;
    view_attach( g_arc );
    refresh_list();
}

/* ------------------------------------------------------------------ */
/* reading a text file without extracting it                           */
/* ------------------------------------------------------------------ */

/* a readme is for reading.  No extension at all - README, MAKEFILE -
   is nearly always text. */
static int is_probably_text( const char *name )
{
    static const char *const known[] = {
        ".TXT", ".DOC", ".NFO", ".ME", ".1ST", ".DIZ", ".ASC",
        ".LOG", ".INI", ".BAT", ".CFG", ".C", ".H", ".ASM", NULL
    };
    const char *dot = NULL;
    char up[8];
    int at, len;

    if ( name == NULL ) {
        return 0;
    }
    for ( at = 0; name[at]; at++ ) {
        if ( name[at] == '.' ) {
            dot = name + at;
        }
    }
    if ( dot == NULL ) {
        return 1;
    }
    for ( len = 0; len < 7 && dot[len]; len++ ) {
        up[len] = (char)toupper( (u8)dot[len] );
    }
    up[len] = 0;
    for ( at = 0; known[at]; at++ ) {
        if ( !strcmp( up, known[at] ) ) {
            return 1;
        }
    }
    return 0;
}

/* F2: the archive's comment, in the viewer - only zip, RAR4 and KWAJ
   have anywhere to keep one */
static void act_comment( void )
{
    char title[34 + 10 + 1];
    const char *text;

    if ( g_arc == NULL ) {
        msg( "Comment", "Open an archive first." );
        return;
    }
    text = ArcComment( g_arc );
    if ( text == NULL ) {
        msg( "Comment", "This archive has no comment." );
        return;
    }
    shorten_tail( title, 34, base_name( g_path ) );
    strcat( title, " - comment" );
    if ( view_text( text, title ) < 0 ) {
        msg( "Comment", "There was not enough memory to show it." );
    }
    repaint();
}

static void view_disk_file( const char *path, const char *shown )
{
    char title[40];

    shorten_tail( title, 34, base_name( shown ? shown : path ) );
    if ( view_file( path, title ) < 0 ) {
        msg( "View", "That file could not be read, or there was not enough memory to hold it." );
    }
    repaint();
}

/* unpacked to TEMP, shown, and deleted again */
static void view_entry( int index )
{
    char root[SZ_MAX_NAME], dir[SZ_MAX_NAME], name[SZ_MAX_NAME], full[SZ_MAX_NAME * 2], text[192];
    int rc;

    if ( g_arc == NULL || index < 0 || index >= ArcNumEntries( g_arc ) ) {
        return;
    }
    if ( ArcEntryIsDir( g_arc, index ) ) {
        msg( "View", "That entry is a folder, not a file." );
        return;
    }
    temp_root( root, sizeof( root ) );
    if ( strlen( root ) + 11 > sizeof( dir ) ) {
        msg( "View", "The TEMP folder's path is too long." );
        return;
    }
    sprintf( dir, "%s\\XARCV.TMP", root );
    _mkdir( dir );
    prog_open( "Reading" );
    rc = extract_one_to( index, dir, name, sizeof( name ) );
    prog_close();
    repaint();
    if ( rc != SZ_OK ) {
        _rmdir( dir );
        if ( rc == SZ_ERR_NORAM || rc == SZ_ERR_MEMORY ) {
            msg( "View", mem_hint( rc ) );
        } else if ( rc == SZ_ERR_UNSUPPORTED ) {
            msg( "View", ArcUnsupportedHint( g_arc ) );
        } else if ( rc == SZ_ERR_CANCEL ) {
            msg( "View", "Cancelled." );
        } else {
            sprintf( text, "Could not read that entry:\n%s", ArcErrorText( rc ) );
            msg( "View", text );
        }
        return;
    }
    sprintf( full, "%s\\%s", dir, name );
    view_disk_file( full, ArcEntryName( g_arc, index ) );
    remove( full );
    _rmdir( dir );
    repaint();
}

/* F3: on the disk too - the readme beside an archive is the same
   question as the one inside it */
static void act_view( void )
{
    char full[SZ_MAX_NAME];
    int row, idx;

    if ( list.count <= 0 ) {
        return;
    }
    row = list.sel;
    if ( disk_mode() ) {
        if ( dirscan_is_dir( &g_dir, row ) ) {
            msg( "View", "That row is a folder, not a file." );
            return;
        }
        dirscan_full( &g_dir, row, full, sizeof( full ) );
        if ( full[0] ) {
            view_disk_file( full, dirscan_name( &g_dir, row ) );
        }
        return;
    }
    idx = view_index( row );
    if ( idx < 0 ) {
        msg( "View", "Nothing to view on that row." );
        return;
    }
    view_entry( idx );
}

static void act_browse_into( void )
{
    int row, idx;

    if ( g_arc == NULL ) {
        msg( "Browse", "Open an archive first." );
        return;
    }
    if ( list.count <= 0 ) {
        return;
    }
    row = list.sel;
    if ( view_is_up( row ) ) {
        return;
    }
    idx = view_index( row );
    if ( idx < 0 ) {
        msg( "Browse", "That is a folder inside this archive, not a file.\n\nPress Enter to open." );
        return;
    }
    browse_into( idx );
}

/* Backspace: out of a folder, out of a nested archive, out of the
   outermost archive to the disk it came from, or up on the disk */
static void act_back( void )
{
    if ( disk_mode() ) {
        if ( up_disk() ) {
            refresh_list();
        }
        return;
    }
    if ( view_cwd()[0] ) {
        if ( view_up() ) {
            refresh_list();
        }
        return;
    }
    if ( g_nest_depth > 0 ) {
        nest_back();
        return;
    }
    act_browse_disk();
}

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

static void act_folder_view( void )
{
    view_folder_mode( !view_is_folder_mode() );
    ArcPrefSetFolderView( view_is_folder_mode() );
    ArcPrefSave();
    update_menu_marks();
    if ( g_arc ) {
        refresh_list();
    } else {
        repaint();
    }
}

/* The PREFERENCE, taking the screen with it when the screen is not
   already there.  A refusal - not a VGA - is said once and the file is
   left alone: what is in it should be something that works. */
static void set_graphics_screen( int on )
{
    on = on != 0;
    if ( on != df_is_graphics() ) {
        if ( !scr_graphics( on ) ) {
            msg( "Graphics Mode", gfx_error() );
            update_menu_marks();
            repaint();
            return;
        }
        apply_screen_layout();
    }
    ArcPrefSetGraphics( on );
    ArcPrefSave();
    update_menu_marks();
    repaint();
}

static void act_graphics( void )
{
    set_graphics_screen( !ArcPrefGraphics() );
}

static void marks_refresh( void )
{
    list_draw( &list );
    update_status();
}

static void act_mark_all( void )
{
    if ( list.count > 0 ) {
        list_mark_all( &list );
        marks_refresh();
    }
}

static void act_unmark_all( void )
{
    if ( list.count > 0 ) {
        list_unmark_all( &list );
        marks_refresh();
    }
}

static void act_invert_marks( void )
{
    if ( list.count > 0 ) {
        list_invert( &list );
        marks_refresh();
    }
}

static void act_toggle_mark( void )
{
    if ( list.count > 0 ) {
        list_mark( &list, list.sel, !list_marked( &list, list.sel ) );
        marks_refresh();
    }
}

static void act_keep_folders( void )
{
    ArcSetFlattenPaths( !ArcFlattenPaths() );
    ArcPrefSave();
    update_menu_marks();
    repaint();
}

/* Naming a shortened file yourself, where the destination takes 8.3
   names only: the automatic name offered, and typed over; Skip (F3)
   leaves one file out, Cancel stops the extraction. */
static int tui_short_name( void *user, const char *parent, const char *stored,
                           char *suggested, int size )
{
    char prompt[112], shown[52], into[24], text[TF_TEXT_MAX];
    int answer;

    (void)user;
    if ( g_ask_names_off ) {
        return ARC_SN_AUTO;
    }
    shorten_tail( shown, 44, stored ? stored : "" );
    if ( parent && parent[0] ) {
        shorten_tail( into, 16, parent );
        sprintf( prompt, "8.3 name for '%s' in %s?", shown, into );
    } else {
        sprintf( prompt, "8.3 name for '%s'?", shown );
    }
    strncpy( text, suggested, sizeof( text ) - 1 );
    text[sizeof( text ) - 1] = 0;
    answer = input_box( FG_BLUE, BG_WHITE, "Short Name", prompt, text, 0, IB_SKIPBTN );
    if ( answer == IB_CANCEL ) {
        return ARC_SN_ABORT;
    }
    if ( answer == IB_SKIP ) {
        return ARC_SN_SKIP;
    }
    if ( !text[0] ) {
        return ARC_SN_AUTO;
    }
    strncpy( suggested, text, (size_t)size - 1 );
    suggested[size - 1] = 0;
    return ARC_SN_USE;
}

static void apply_short_name_pref( void )
{
    ArcSetShortNamePrompt( ArcPrefAskShortNames() ? tui_short_name : NULL, NULL );
}

static void act_toggle_ask_names( void )
{
    ArcPrefSetAskShortNames( !ArcPrefAskShortNames() );
    ArcPrefSave();
    apply_short_name_pref();
    update_menu_marks();
    repaint();
}

/* ------------------------------------------------------------------ */
/* Test, Info, About, Full Name, Help                                  */
/* ------------------------------------------------------------------ */

static void act_test( void )
{
    char text[192];
    int rc;

    if ( g_arc == NULL ) {
        msg( "Test", "Open an archive first." );
        return;
    }
    prog_open( "Testing" );
    do {
        rc = ArcTestAll( g_arc, tui_progress, NULL );
    } while ( password_retry( rc ) );
    prog_close();
    repaint();
    if ( rc == SZ_OK ) {
        msg( "Test", test_passed_text( g_arc ) );
    } else if ( rc == SZ_ERR_CANCEL ) {
        msg( "Test", "Test cancelled." );
    } else if ( rc == SZ_ERR_UNSUPPORTED ) {
        msg( "Test", ArcUnsupportedHint( g_arc ) );
    } else if ( rc == SZ_ERR_NORAM || rc == SZ_ERR_MEMORY ) {
        msg( "Test", mem_hint( rc ) );
    } else {
        sprintf( text, "Integrity test FAILED:\n%s", ArcErrorText( rc ) );
        msg( "Test", text );
    }
}

/* the totals are 64-bit integers: an archive can hold more than four
   gigabytes even when no one entry does */
static void act_info( void )
{
    char info[640], vol_line[48], mem_line[96], tot[NUM_FMT_MAX], pk[NUM_FMT_MAX];
    char nb[NUM_FMT_MAX], hb[NUM_FMT_MAX];
    u64 total = 0, packed = 0;
    UInt32 need_kb = 0, have_kb = 0, size;
    int at, count, dirs = 0, vols;

    if ( g_arc == NULL ) {
        msg( "Info", "Open an archive first." );
        return;
    }
    count = ArcNumEntries( g_arc );
    for ( at = 0; at < count; at++ ) {
        if ( ArcEntryIsDir( g_arc, at ) ) {
            dirs++;
            continue;
        }
        total += ArcEntrySize( g_arc, at );
        size = ArcEntryPacked( g_arc, at );
        if ( size != 0xFFFFFFFFUL ) {
            packed += size;
        }
    }
    vols = ArcVolumeCount( g_arc );
    if ( vols > 1 ) {
        sprintf( vol_line, "Volumes : %d\n", vols );
    } else {
        vol_line[0] = 0;
    }
    if ( ArcMemCheck( g_arc, &need_kb, &have_kb ) == SZ_OK ) {
        sprintf( mem_line, "\nMemory  : %s KB needed to extract", NumFmt( need_kb, nb ) );
    } else {
        sprintf( mem_line, "\nMemory  : %s KB needed, only %s KB free", NumFmt( need_kb, nb ),
                 NumFmt( have_kb, hb ) );
    }
    fmt_total( total, tot );
    fmt_total( packed, pk );
    _snprintf( info, sizeof( info ),
               "Archive : %s\nFormat  : %s\n%sEntries : %d  (%d folders)\n"
               "Total   : %s bytes\nPacked  : %s bytes%s",
               g_path, ArcFormatName( g_arc ), vol_line, count, dirs, tot, pk, mem_line );
    msg( "Archive Info", info );
}

static void act_about( void )
{
    char text[400];

    sprintf( text, "XArchive v2.1\n\nCreates Zip and 7-Zip archives.\n\n"
                   "Compression and extraction utility.\n\n"
                   "Memory available to this archive: %lu MB\n\n"
                   "Copyright (c) 2026 Charlie Dobson",
             (unsigned long)ArcMemLimitMB() );
    msg( "About XArchive", text );
}

/* the whole stored path of the row: a name is cut at 39 columns in the
   list, and this is where the rest of it can be read */
static void act_full_name( void )
{
    char text[SZ_MAX_NAME * 3 + 160], leaf[SZ_MAX_NAME];
    int row, idx;

    if ( g_arc == NULL ) {
        msg( "Full Name", "Open an archive first." );
        return;
    }
    if ( list.count <= 0 ) {
        return;
    }
    row = list.sel;
    if ( view_is_up( row ) ) {
        msg( "Full Name", "That row goes back up a folder." );
        return;
    }
    view_row_name( row, leaf, sizeof( leaf ) );
    idx = view_index( row );
    if ( idx >= 0 ) {
        sprintf( text, "Folder:\n%s\n\nName:\n%s\n\nStored as:\n%s",
                 view_cwd()[0] ? view_cwd() : "\\", leaf, ArcEntryName( g_arc, idx ) );
    } else {
        sprintf( text, "Folder:\n%s\n\nName:\n%s\n\n(a folder implied by the entries inside "
                       "it - the archive stores no entry of its own for it)",
                 view_cwd()[0] ? view_cwd() : "\\", leaf );
    }
    msg( "Full Name", text );
}

static void act_help( void )
{
    msg_block( "Keyboard Help",
               "F1  this help          F2  archive comment\n"
               "F3  view as text       F4  archive information\n"
               "F5  extract            F6  full name of this row\n"
               "F7  test integrity     F8  compress\n"
               "\n"
               "Enter    Opens a drive, archive, or text file.  When in\n"
               "         an archive, it extracts all or selected files.\n"
               "Bksp     Go back one level.\n"
               "Spc/Ins  Mark or unmark      +  mark all\n"
               "*        Invert the marks    -  unmark all\n"
               "Esc      Quit\n"
               "\n"
               "In an archive, F5 extracts the marked rows, or all of it\n"
               "when nothing is marked.  Marking a folder marks all of it.\n"
               "On the disk, F8 packs the marked files and folders into a\n"
               "new .ZIP or .7Z, and F5 unpacks the highlighted archive." );
}

/* ------------------------------------------------------------------ */
/* Enter and the double click                                          */
/* ------------------------------------------------------------------ */

/* on the disk: into a folder, or the file shown or opened - any file
   that is not text is offered to ArcOpen, whatever it is called */
static void act_enter_disk( int row )
{
    char full[SZ_MAX_NAME];

    if ( row < 0 || row >= g_dir.count ) {
        return;
    }
    if ( dirscan_is_dir( &g_dir, row ) ) {
        if ( enter_disk( row ) ) {
            refresh_list();
        }
        return;
    }
    dirscan_full( &g_dir, row, full, sizeof( full ) );
    if ( !full[0] ) {
        return;
    }
    if ( is_probably_text( dirscan_name( &g_dir, row ) ) ) {
        view_disk_file( full, dirscan_name( &g_dir, row ) );
        return;
    }
    open_path( full );
}

/* a double click navigates: into a folder, or an archive opened - a
   plain file is left alone, because Enter on one puts a box up */
static void on_double( void *user, int row )
{
    int idx;

    (void)user;
    if ( disk_mode() ) {
        act_enter_disk( row );
        return;
    }
    if ( view_is_up( row ) || (view_is_folder_mode() && view_is_dir( row )) ) {
        if ( view_enter( row ) ) {
            refresh_list();
        }
        return;
    }
    idx = view_index( row );
    if ( idx >= 0 && !ArcEntryIsDir( g_arc, idx ) && looks_like_archive( ArcEntryName( g_arc, idx ) ) ) {
        browse_into( idx );
    }
}

static void on_activate( void *user, int row )
{
    char prompt[96], shown[48];
    int idx, count, *items, max;

    (void)user;
    if ( disk_mode() ) {
        act_enter_disk( row );
        return;
    }
    if ( view_is_up( row ) || (view_is_folder_mode() && view_is_dir( row )) ) {
        if ( view_enter( row ) ) {
            refresh_list();
        }
        return;
    }
    if ( list.mark_count > 0 ) {
        act_extract();
        return;
    }
    idx = view_index( row );
    if ( idx < 0 ) {
        return;
    }
    if ( !ArcEntryIsDir( g_arc, idx ) && looks_like_archive( ArcEntryName( g_arc, idx ) ) ) {
        browse_into( idx );
        return;
    }
    if ( !ArcEntryIsDir( g_arc, idx ) && is_probably_text( ArcEntryName( g_arc, idx ) ) ) {
        view_entry( idx );
        return;
    }
    max = view_max_expanded();
    items = (int *)malloc( (size_t)(max > 0 ? max : 1) * sizeof( int ) );
    if ( items == NULL ) {
        return;
    }
    count = view_expand_one( row, items, max );
    shorten_tail( shown, 40, ArcEntryName( g_arc, idx ) );
    sprintf( prompt, "Extract '%s' to which folder?", shown );
    if ( count > 0 ) {
        do_extract( items, count, prompt );
    }
    free( items );
}

/* ------------------------------------------------------------------ */
/* the window                                                          */
/* ------------------------------------------------------------------ */

static void screen_back( void )
{
    if ( screen_open ) {
        screen_open = 0;
        scr_close();
    }
}

static void handle( const EVENT *ev )
{
    /* the close box in the corner, as File > Exit */
    if ( ev->kind == EV_MOUSE && (ev->press & LEFT_BUTTON) && ev->y == 0 && ev->x <= 2 ) {
        act_exit();
        return;
    }
    if ( menu_event( ev ) || list_event( &list, ev ) ) {
        return;
    }
    if ( ev->kind != EV_KEY ) {
        return;
    }
    switch ( ev->key ) {
    case KEY_BS:    act_back();         break;
    case KEY_F1:    act_help();         break;
    case KEY_F2:    act_comment();      break;
    case KEY_F3:    act_view();         break;
    case KEY_F4:    act_info();         break;
    case KEY_F5:    act_extract();      break;
    case KEY_F6:    act_full_name();    break;
    case KEY_F7:    act_test();         break;
    case KEY_F8:    act_compress();     break;
    case KEY_ESC:   running = 0;        break;
    }
}

int tui_run( const char *open_first, int graphics )
{
    char hdr[96], warn[400];
    int menu;
    EVENT ev;

    crit_install();
    ev_break_off();
    if ( scr_open( graphics ) < 0 ) {
        printf( "Not enough memory.\n" );
        return 3;
    }
    screen_open = 1;
    xa_on_exit( screen_back );
    ev_init();
    icons_register();
    dirscan_init( &g_dir );

    list_init( &list, FG_LIGHT_CYAN, BG_BLUE );
    menu_row( 1 );
    menu = menu_add( "File", 'F' );
    menu_item( menu, "Browse Into   Ent", 'B', act_browse_into );
    menu_item( menu, "Back Up      Bksp", 'K', act_back );
    menu_item( menu, "Comment        F2", 'C', act_comment );
    menu_item( menu, "View Text      F3", 'V', act_view );
    menu_item( menu, "Info           F4", 'I', act_info );
    menu_item( menu, "Extract...     F5", 'E', act_extract );
    menu_item( menu, "Extract All...",    'A', act_extract_all );
    menu_item( menu, "Compress...    F8", 'P', act_compress );
    menu_item( menu, "Full Name      F6", 'N', act_full_name );
    menu_item( menu, "Test           F7", 'T', act_test );
    menu_item( menu, "Exit",              'X', act_exit );
    menu = menu_add( "Select", 'S' );
    menu_item( menu, "Mark/Unmark  Spc", 'M', act_toggle_mark );
    menu_item( menu, "Mark All       +", 'A', act_mark_all );
    menu_item( menu, "Unmark All     -", 'U', act_unmark_all );
    menu_item( menu, "Invert Marks   *", 'I', act_invert_marks );
    /* in the order OP_* lists them: update_menu_marks writes the labels */
    op_menu = menu_add( "Options", 'O' );
    menu_item( op_menu, "Folder View",        'F', act_folder_view );
    menu_item( op_menu, "Extract Pathnames",  'E', act_keep_folders );
    menu_item( op_menu, "Auto-rename to 8.3", 'A', act_toggle_ask_names );
    menu_item( op_menu, "Graphics mode",      'G', act_graphics );
    menu = menu_add( "Help", 'H' );
    menu_item( menu, "Keys       F1", 'K', act_help );
    menu_item( menu, "About",         'A', act_about );
    update_menu_marks();

    apply_screen_layout();
    sprintf( hdr, " %-39.39s %13s  %-16.16s %-4.4s", "Name", "Size", "Modified", "Attr" );
    list_header( &list, hdr );
    list.on_activate = on_activate;
    list.on_double = on_double;
    list.on_marks = on_marks_changed;
    list.can_mark = row_can_mark;
    list.mark_icon = icon_mark();
    apply_short_name_pref();

    /* before anything is opened, so the first thing said explains what
       follows - not in a test run, which has no one to answer it */
    if ( !ArcMemStartupOk( NULL, NULL ) && !ev_testing() ) {
        mem_warn_text( warn, sizeof( warn ) );
        repaint();
        msg( "Not Enough Memory", warn );
    }
    if ( open_first ) {
        repaint();
        open_path( open_first );
    } else {
        refresh_list();
    }
    ArcSetOverwritePrompt( tui_overwrite, NULL );

    repaint();
    running = 1;
    while ( running ) {
        ev_wait( &ev );
        handle( &ev );
    }

    nest_unwind_all();
    view_attach( NULL );
    if ( g_arc ) {
        ArcClose( g_arc );
        g_arc = NULL;
    }
    screen_back();
    list_free( &list );
    dirscan_free( &g_dir );
    return 0;
}
