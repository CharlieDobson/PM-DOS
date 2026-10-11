/*
 * OPS.C - what is done to files: copy, move, delete, rename, change
 * attributes, make a directory, print.
 *
 * EVERY COMMAND STARTS BY TAKING ITS PICKS: the selected files of the
 * window the keys are in - with any that Select Across Directories is
 * keeping from other directories - or, with nothing selected, the file
 * the keys are on; or the directory, when the keys are in the tree.
 * The picks are whole paths, so nothing after that depends on which
 * list is showing.
 *
 * A MOVE ON ONE DRIVE IS A RENAME, as it is for DOS's MOVE: the file's
 * data stays where it is.  Across drives it is a copy and, when the
 * copy is whole, a delete.  A copy keeps the file's date, time and
 * attributes.
 *
 * WHAT THE SHELL ASKS FIRST is Options, Confirmation: before deleting,
 * before writing over a file that is there, and before acting on a
 * drag of the mouse.
 */
#include "shell.h"

#define COPY_BUF        (32u * 1024u)

static FINDREC rec;

/* ------------------------------------------------------------------ */
/* the picks                                                           */
/* ------------------------------------------------------------------ */

static void pick_add( PICKS *picks, const char *path, u32 size, u16 date, u16 time,
                      u8 attr, int is_dir )
{
    PICK *bigger, *pick;

    if ( (picks->count & 31) == 0 ) {
        bigger = (PICK *)xalloc( (u32)(picks->count + 32) * sizeof( PICK ) );
        if ( picks->count ) {
            mem_cpy( bigger, picks->item, (u32)picks->count * sizeof( PICK ) );
        }
        xfree( picks->item );
        picks->item = bigger;
    }
    pick = &picks->item[picks->count++];
    pick->path = str_dup( path );
    pick->size = size;
    pick->date = date;
    pick->time = time;
    pick->attr = attr;
    pick->is_dir = (u8)is_dir;
}

int picks_take( PICKS *picks )
{
    FWIN *win = app_win();
    char path[PATH_MAX];
    FENT *ent;
    u32 handle;
    int index;

    picks->item = NULL;
    picks->count = 0;
    if ( focus_area == AREA_PROGS || !win_ok( win ) ) {
        return 0;
    }
    if ( focus_area == AREA_TREE ) {
        if ( win->node <= 0 ) {
            return 0;
        }
        tree_path( win->tree, win->node, path );
        pick_add( picks, path, 0, 0, 0, ATTR_DIR, 1 );
        return 1;
    }
    for ( index = 0; index < win->files.count; index++ ) {
        ent = &win->files.ent[index];
        if ( ent->sel ) {
            flist_path( win->tree, ent, path );
            pick_add( picks, path, ent->size, ent->date, ent->time, ent->attr, 0 );
        }
    }
    /* the ones kept from other directories: looked up, since the list
       they were selected in is gone */
    for ( index = 0; index < kept_count(); index++ ) {
        if ( sys_find_first( kept_path( index ), 0x27, &rec, &handle ) == 0 ) {
            sys_find_close( handle );
            pick_add( picks, kept_path( index ), rec.size, rec.date, rec.time, rec.attr, 0 );
        }
    }
    if ( picks->count == 0 && focus_area == AREA_FILES && win->files.count ) {
        ent = &win->files.ent[win->cur];
        flist_path( win->tree, ent, path );
        pick_add( picks, path, ent->size, ent->date, ent->time, ent->attr, 0 );
    }
    return picks->count > 0;
}

void picks_free( PICKS *picks )
{
    int index;

    for ( index = 0; index < picks->count; index++ ) {
        xfree( picks->item[index].path );
    }
    xfree( picks->item );
    picks->item = NULL;
    picks->count = 0;
}

/* their names in a row, for a dialog to show: as many as fit */
static void picks_names( PICKS *picks, char *buf, u32 max )
{
    int index;

    buf[0] = 0;
    for ( index = 0; index < picks->count; index++ ) {
        if ( index && !str_catn( buf, " ", max ) ) {
            break;
        }
        if ( !str_catn( buf, path_name( picks->item[index].path ), max ) ) {
            break;
        }
    }
    if ( index < picks->count && max > 4 ) {
        str_cpy( buf + (str_len( buf ) > max - 4 ? max - 4 : str_len( buf )), "..." );
    }
}

/* everything the selection was is done with: both lists are read again */
static void picks_done( PICKS *picks )
{
    FWIN *win = app_win();

    picks_free( picks );
    kept_clear();
    wins_stale_all();
    if ( win_ok( win ) ) {
        tree_space( win->tree );
    }
    busy_show( NULL, NULL );
}

/* "NAME   (2 of 7)" */
static void progress( const char *doing, PICKS *picks, int index )
{
    char line[PATH_MAX + 32], num[12];

    str_fit( line, path_name( picks->item[index].path ), 48 );
    if ( picks->count > 1 ) {
        str_catn( line, "   (", sizeof( line ) );
        str_catn( line, fmt_dec( (u32)index + 1, 0, num ), sizeof( line ) );
        str_catn( line, " of ", sizeof( line ) );
        str_catn( line, fmt_dec( (u32)picks->count, 0, num ), sizeof( line ) );
        str_catn( line, ")", sizeof( line ) );
    }
    busy_show( doing, line );
}

/* ------------------------------------------------------------------ */
/* copy and move                                                       */
/* ------------------------------------------------------------------ */

int copy_file( const char *from, const char *to, u16 attr )
{
    static u8 *buf;
    u32 in, out, got, put;
    u16 time, date;
    int rc;

    if ( buf == NULL ) {
        buf = (u8 *)try_alloc( COPY_BUF );
        if ( buf == NULL ) {
            return -1;
        }
    }
    sys_crit_seen();
    rc = sys_open_read( from, &in );
    if ( rc ) {
        return rc;
    }
    rc = sys_create( to, &out );
    if ( rc ) {
        sys_close( in );
        return rc;
    }
    for ( ;; ) {
        rc = sys_read( in, buf, COPY_BUF, &got );
        if ( rc || got == 0 ) {
            break;
        }
        rc = sys_write( out, buf, got, &put );
        if ( rc ) {
            break;
        }
    }
    if ( rc == 0 && sys_get_ftime( in, &time, &date ) == 0 ) {
        sys_set_ftime( out, time, date );
    }
    sys_close( in );
    if ( sys_close( out ) && rc == 0 ) {
        rc = ERR_DISKFULL;
    }
    if ( sys_crit_seen() && rc == 0 ) {
        rc = ERR_NOTREADY;
    }
    if ( rc ) {
        sys_delete( to );
    } else {
        sys_set_attr( to, (u16)(attr & 0x27) );
    }
    return rc;
}

static int is_dir_path( const char *path )
{
    u32 len = str_len( path );
    u16 attr;

    if ( len == 0 ) {
        return 0;
    }
    if ( path[len - 1] == '\\' || path[len - 1] == ':' ) {
        return 1;
    }
    return sys_get_attr( path, &attr ) == 0 && (attr & ATTR_DIR);
}

static int same_file( const char *one, const char *other )
{
    char full1[PATH_MAX], full2[PATH_MAX];

    if ( sys_truename( one, full1, 0 ) || sys_truename( other, full2, 0 ) ) {
        return str_icmp( one, other ) == 0;
    }
    return str_icmp( full1, full2 ) == 0;
}

/* "C:\DIR\NAME.EXT   10-09-26   1,234" */
static void describe( const char *path, u32 size, u16 date, char *buf, u32 max )
{
    char num[20], day[12];
    const char *shown = path;

    if ( str_len( path ) > 40 ) {
        shown = path + str_len( path ) - 40;
    }
    fmt_date( date, day );
    str_fit( buf, shown, max );
    str_catn( buf, "   ", max );
    str_catn( buf, day, max );
    str_catn( buf, "   ", max );
    str_catn( buf, fmt_num( size, 0, num ), max );
}

static int replace_ask( PICK *pick, const char *target )
{
    char text[400], line[160];
    u32 handle;

    str_cpy( text, "Replace file:\n  " );
    if ( sys_find_first( target, 0x27, &rec, &handle ) == 0 ) {
        sys_find_close( handle );
        describe( target, rec.size, rec.date, line, sizeof( line ) );
    } else {
        str_fit( line, target, sizeof( line ) );
    }
    str_catn( text, line, sizeof( text ) );
    str_catn( text, "\n\nWith file:\n  ", sizeof( text ) );
    describe( pick->path, pick->size, pick->date, line, sizeof( line ) );
    str_catn( text, line, sizeof( text ) );
    return msg_text( "Replace File Confirmation", text, MB_YESNOCANCEL, HELP_FILES );
}

/* The picks to "dest": into it when it is a directory, and as it when
   it is a name and there is one file. */
static void transfer( PICKS *picks, const char *dest, int move )
{
    char target[PATH_MAX], full[PATH_MAX];
    PICK *pick;
    int index, into_dir, rc, answer, exists;
    u16 attr;

    if ( sys_truename( dest, full, 0 ) != 0 || full[0] == 0 ) {
        str_fit( full, dest, sizeof( full ) );
    }
    into_dir = is_dir_path( full ) || is_dir_path( dest );
    if ( !into_dir && picks->count > 1 ) {
        msg_box2( "The files cannot all be given one name.  This is not a directory:",
                  dest, MB_OK );
        return;
    }
    for ( index = 0; index < picks->count; index++ ) {
        pick = &picks->item[index];
        if ( pick->is_dir ) {
            continue;
        }
        if ( into_dir ) {
            if ( !path_join( target, full, path_name( pick->path ), sizeof( target ) ) ) {
                msg_box2( "The name is too long:", pick->path, MB_OK );
                break;
            }
        } else {
            str_fit( target, full, sizeof( target ) );
        }
        if ( same_file( pick->path, target ) ) {
            msg_box2( "A file cannot be copied or moved onto itself:", pick->path, MB_OK );
            break;
        }
        exists = sys_get_attr( target, &attr ) == 0;
        if ( exists && (attr & ATTR_DIR) ) {
            msg_box2( "There is a directory with that name:", target, MB_OK );
            break;
        }
        if ( exists && opt.confirm_replace ) {
            answer = replace_ask( pick, target );
            if ( answer == ID_NO ) {
                continue;
            }
            if ( answer != ID_YES ) {
                break;
            }
        }
        progress( move ? "Moving file:" : "Copying file:", picks, index );
        if ( move && ch_upper( target[0] ) == ch_upper( pick->path[0] ) ) {
            rc = exists ? sys_delete( target ) : 0;
            if ( rc == 0 ) {
                rc = sys_rename( pick->path, target );
            }
        } else {
            rc = copy_file( pick->path, target, pick->attr );
            if ( rc == 0 && move ) {
                rc = sys_delete( pick->path );
            }
        }
        if ( rc ) {
            msg_error( pick->path, rc );
            break;
        }
    }
}

void op_copy( int move )
{
    static char from[200], to[PATH_MAX];
    PICKS picks;
    CTL ctl[7];
    DLG dlg;

    if ( !picks_take( &picks ) ) {
        return;
    }
    if ( picks.item[0].is_dir ) {
        msg_box( "A directory can be renamed or deleted, but not copied or moved.", MB_OK );
        picks_free( &picks );
        return;
    }
    app_chdir();
    picks_names( &picks, from, 53 );
    tree_path( app_win()->tree, app_win()->node, to );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_label( &ctl[0], 2, 3, "From:" );
    ctl_text( &ctl[1], 2, 10, 0, from );
    ctl_label( &ctl[2], 4, 3, "&To:" );
    ctl_edit( &ctl[3], 4, 10, 52, to, sizeof( to ) );
    ctl_button( &ctl[4], 6, 14, "OK", ID_OK, 1 );
    ctl_button( &ctl[5], 6, 27, "Cancel", ID_CANCEL, 0 );
    ctl_button( &ctl[6], 6, 44, "&Help", ID_HELP, 0 );
    dlg.title = move ? "Move File" : "Copy File";
    dlg.rows = 8;
    dlg.cols = 66;
    dlg.ctl = ctl;
    dlg.nctl = 7;
    dlg.focus = 3;
    dlg.help = HELP_FILES;
    if ( dlg_run( &dlg ) == ID_OK && to[0] ) {
        transfer( &picks, to, move );
    }
    picks_done( &picks );
}

/* the mouse let go of the picks over a directory or a drive */
void op_drop( PICKS *picks, const char *dest_dir, int move )
{
    char text[PATH_MAX + 80];

    if ( picks->count == 0 || picks->item[0].is_dir ) {
        return;
    }
    if ( opt.confirm_mouse ) {
        str_cpy( text, move ? "Move the selected files to\n" : "Copy the selected files to\n" );
        str_catn( text, dest_dir, sizeof( text ) );
        str_catn( text, " ?", sizeof( text ) );
        if ( msg_text( "Confirm Mouse Operation", text, MB_YESNO, HELP_FILES ) != ID_YES ) {
            return;
        }
    }
    transfer( picks, dest_dir, move );
    kept_clear();
    wins_stale_all();
    if ( win_ok( app_win() ) ) {
        tree_space( app_win()->tree );
    }
}

/* ------------------------------------------------------------------ */
/* delete                                                              */
/* ------------------------------------------------------------------ */

static void delete_dir( PICK *pick )
{
    FWIN *win = app_win();
    char root[4];
    int node = win->node, rc;

    if ( opt.confirm_delete
         && msg_box2( "Delete this directory?", pick->path, MB_YESNO ) != ID_YES ) {
        return;
    }
    /* DOS will not remove the directory it is standing in */
    root[0] = pick->path[0];
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    sys_chdir( root );
    rc = sys_rmdir( pick->path );
    if ( rc ) {
        msg_box2( pick->path, rc == ERR_ACCESS || rc == 16
                  ? "The directory is not empty, or it cannot be removed."
                  : err_text( rc ), MB_OK );
        return;
    }
    win_set_node( win, win->tree->node[node].parent );
    wins_mark( win->drive );
    tree_remove( win->tree, node );
    wins_restore( win->drive );
}

void op_delete( void )
{
    PICKS picks;
    char text[PATH_MAX + 40];
    int index, rc, answer;

    if ( !picks_take( &picks ) ) {
        return;
    }
    if ( picks.item[0].is_dir ) {
        delete_dir( &picks.item[0] );
        picks_done( &picks );
        return;
    }
    for ( index = 0; index < picks.count; index++ ) {
        if ( opt.confirm_delete ) {
            str_cpy( text, "Delete " );
            str_catn( text, picks.item[index].path, sizeof( text ) );
            str_catn( text, " ?", sizeof( text ) );
            answer = msg_text( "Delete File Confirmation", text, MB_YESNOCANCEL, HELP_FILES );
            if ( answer == ID_NO ) {
                continue;
            }
            if ( answer != ID_YES ) {
                break;
            }
        }
        progress( "Deleting file:", &picks, index );
        rc = sys_delete( picks.item[index].path );
        if ( rc ) {
            msg_error( picks.item[index].path, rc );
            break;
        }
    }
    picks_done( &picks );
}

/* ------------------------------------------------------------------ */
/* rename                                                              */
/* ------------------------------------------------------------------ */

void op_rename( void )
{
    PICKS picks;
    char name[PATH_MAX], target[PATH_MAX], info[PATH_MAX + 24], root[4];
    PICK *pick;
    int index, rc, drive = app_win()->drive, dir_renamed = 0;

    if ( !picks_take( &picks ) ) {
        return;
    }
    for ( index = 0; index < picks.count; index++ ) {
        pick = &picks.item[index];
        str_cpy( info, "Current name:  " );
        str_catn( info, path_name( pick->path ), sizeof( info ) );
        str_fit( name, path_name( pick->path ), sizeof( name ) );
        if ( !input_box( pick->is_dir ? "Rename Directory" : "Rename File", info,
                         "&New name:", name, 128, 34, HELP_FILES, 0 ) ) {
            break;
        }
        if ( name[0] == 0 || str_chr( name, '\\' ) || str_chr( name, ':' ) ) {
            msg_box( "Type a name without a drive or a path.", MB_OK );
            index--;
            continue;
        }
        str_fit( target, pick->path, sizeof( target ) );
        *(char *)path_name( target ) = 0;
        str_catn( target, name, sizeof( target ) );
        if ( pick->is_dir ) {                   /* not from inside the directory */
            root[0] = pick->path[0];
            root[1] = ':';
            root[2] = '\\';
            root[3] = 0;
            sys_chdir( root );
        }
        rc = sys_rename( pick->path, target );
        if ( rc ) {
            msg_error( pick->path, rc );
            break;
        }
        dir_renamed |= pick->is_dir;
    }
    picks_done( &picks );
    if ( dir_renamed ) {
        wins_reread( drive );
    }
}

/* ------------------------------------------------------------------ */
/* attributes                                                          */
/* ------------------------------------------------------------------ */

void op_attrib( void )
{
    static char what[80];
    PICKS picks;
    CTL ctl[8];
    DLG dlg;
    char num[12];
    int hidden, system, archive, readonly, index, rc;
    u16 attr;

    if ( !picks_take( &picks ) ) {
        return;
    }
    if ( picks.item[0].is_dir ) {
        picks_free( &picks );
        return;
    }
    attr = picks.item[0].attr;
    hidden = (attr & ATTR_HIDDEN) != 0;
    system = (attr & ATTR_SYSTEM) != 0;
    archive = (attr & ATTR_ARCHIVE) != 0;
    readonly = (attr & ATTR_READONLY) != 0;
    if ( picks.count == 1 ) {
        str_cpy( what, "File:  " );
        str_catn( what, path_name( picks.item[0].path ), 60 );
    } else {
        fmt_dec( (u32)picks.count, 0, num );
        str_cpy( what, num );
        str_catn( what, " files: the marks below go on every one.", sizeof( what ) );
    }
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_text( &ctl[0], 2, 3, 0, what );
    ctl_check( &ctl[1], 4, 5, "&Hidden", &hidden );
    ctl_check( &ctl[2], 5, 5, "&System", &system );
    ctl_check( &ctl[3], 6, 5, "&Archive", &archive );
    ctl_check( &ctl[4], 7, 5, "&Read only", &readonly );
    ctl_button( &ctl[5], 9, 6, "OK", ID_OK, 1 );
    ctl_button( &ctl[6], 9, 18, "Cancel", ID_CANCEL, 0 );
    ctl_button( &ctl[7], 9, 33, "H&elp", ID_HELP, 0 );
    dlg.title = "Change Attributes";
    dlg.rows = 11;
    dlg.cols = 48;
    dlg.ctl = ctl;
    dlg.nctl = 8;
    dlg.focus = 1;
    dlg.help = HELP_FILES;
    if ( dlg_run( &dlg ) == ID_OK ) {
        attr = (u16)((hidden ? ATTR_HIDDEN : 0) | (system ? ATTR_SYSTEM : 0)
                     | (archive ? ATTR_ARCHIVE : 0) | (readonly ? ATTR_READONLY : 0));
        for ( index = 0; index < picks.count; index++ ) {
            rc = sys_set_attr( picks.item[index].path, attr );
            if ( rc ) {
                msg_error( picks.item[index].path, rc );
                break;
            }
        }
    }
    picks_done( &picks );
}

/* ------------------------------------------------------------------ */
/* a new directory                                                     */
/* ------------------------------------------------------------------ */

void op_mkdir( void )
{
    FWIN *win = app_win();
    char parent[PATH_MAX], info[PATH_MAX + 20], name[PATH_MAX], path[PATH_MAX];
    int rc;

    if ( !win_ok( win ) ) {
        return;
    }
    tree_path( win->tree, win->node, parent );
    str_cpy( info, "Parent name:  " );
    str_catn( info, parent, sizeof( info ) );
    name[0] = 0;
    for ( ;; ) {
        if ( !input_box( "Create Directory", info, "&New directory name:", name, 128, 30,
                         HELP_FILES, 0 ) || name[0] == 0 ) {
            return;
        }
        if ( str_chr( name, '\\' ) || str_chr( name, ':' ) ) {
            msg_box( "Type a name without a drive or a path.", MB_OK );
            continue;
        }
        break;
    }
    if ( !path_join( path, parent, name, sizeof( path ) ) ) {
        msg_box( "The name is too long.", MB_OK );
        return;
    }
    rc = sys_mkdir( path );
    if ( rc ) {
        msg_error( path, rc == ERR_ACCESS ? ERR_EXISTS : rc );
        return;
    }
    if ( !sys_lfn() ) {
        str_upper( name );
    }
    wins_mark( win->drive );
    tree_add( win->tree, win->node, name );
    wins_restore( win->drive );
    tree_space( win->tree );
}

/* ------------------------------------------------------------------ */
/* print                                                               */
/* ------------------------------------------------------------------ */

/* The picks to the printer, as COPY file PRN would send them, with a
   form feed after each. */
void op_print( void )
{
    static u8 buf[2048];
    PICKS picks;
    u32 in, out, got, put;
    int index, rc = 0;

    if ( !picks_take( &picks ) ) {
        return;
    }
    if ( picks.item[0].is_dir ) {
        picks_free( &picks );
        return;
    }
    sys_crit_seen();
    rc = sys_open_write( "PRN", &out );
    for ( index = 0; index < picks.count && rc == 0; index++ ) {
        progress( "Printing file:", &picks, index );
        rc = sys_open_read( picks.item[index].path, &in );
        if ( rc ) {
            break;
        }
        while ( (rc = sys_read( in, buf, sizeof( buf ), &got )) == 0 && got ) {
            rc = sys_write( out, buf, got, &put );
            if ( rc || sys_crit_seen() ) {
                rc = rc ? rc : ERR_NOTREADY;
                break;
            }
        }
        sys_close( in );
        if ( rc == 0 ) {
            sys_write( out, "\x0C", 1, &put );
        }
    }
    sys_close( out );
    if ( rc || sys_crit_seen() ) {
        msg_box2( "The files could not all be printed.",
                  rc == ERR_DISKFULL || rc == 0 || rc == ERR_NOTREADY
                  ? "The printer is not ready." : err_text( rc ), MB_OK );
    }
    picks_free( &picks );
    busy_show( NULL, NULL );
}
