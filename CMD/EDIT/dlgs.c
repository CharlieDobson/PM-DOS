/*
 * DLGS.C - the dialogs: Open, Save As, Find, Replace, Print, Settings,
 * Colors, About, and moving the line between two windows.
 */
#include "edit.h"

/* ------------------------------------------------------------------ */
/* lists of names                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char **item;
    int    count, cap;
} STRLIST;

static void sl_free( STRLIST *list )
{
    int index;

    for ( index = 0; index < list->count; index++ ) {
        xfree( list->item[index] );
    }
    xfree( list->item );
    list->item = NULL;
    list->count = list->cap = 0;
}

static int sl_add( STRLIST *list, const char *text )
{
    char **grown, *copy;

    if ( list->count == list->cap ) {
        int cap = list->cap ? list->cap * 2 : 32;
        grown = (char **)try_alloc( (u32)cap * sizeof( char * ) );
        if ( grown == NULL ) {
            return 0;
        }
        if ( list->count ) {
            mem_cpy( grown, list->item, (u32)list->count * sizeof( char * ) );
        }
        xfree( list->item );
        list->item = grown;
        list->cap = cap;
    }
    copy = (char *)try_alloc( str_len( text ) + 1 );
    if ( copy == NULL ) {
        return 0;
    }
    str_cpy( copy, text );
    list->item[list->count++] = copy;
    return 1;
}

/* by name, ignoring case; ".." first, drives last */
static int sl_before( const char *lhs, const char *rhs )
{
    int lrank = lhs[0] == '.' ? 0 : lhs[0] == '[' ? 2 : 1;
    int rrank = rhs[0] == '.' ? 0 : rhs[0] == '[' ? 2 : 1;

    if ( lrank != rrank ) {
        return lrank < rrank;
    }
    return str_icmp( lhs, rhs ) < 0;
}

static void sl_sort( STRLIST *list )
{
    int index, back;
    char *hold;

    for ( index = 1; index < list->count; index++ ) {
        hold = list->item[index];
        for ( back = index; back > 0 && sl_before( hold, list->item[back - 1] ); back-- ) {
            list->item[back] = list->item[back - 1];
        }
        list->item[back] = hold;
    }
}

/* ------------------------------------------------------------------ */
/* Open and Save As                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    STRLIST files, dirs;
    char    name[PATH_MAX];
    char    pattern[PATH_MAX];
    char    dirtext[PATH_MAX];
    char    result[PATH_MAX];
    int     files_ctl, dirs_ctl, ok_ctl;
} FDATA;

static void cur_dir( char *out )
{
    int drv = sys_get_drive();

    out[0] = (char)('A' + drv);
    out[1] = ':';
    out[2] = '\\';
    sys_get_cwd( drv, out + 3 );
}

/* a name as typed, made whole: drive, directory and all */
static void make_path( const char *name, char *out )
{
    int drv;
    const char *rest = name;
    char dir[PATH_MAX];

    if ( name[0] && name[1] == ':' ) {
        drv = ch_upper( name[0] ) - 'A';
        rest = name + 2;
    } else {
        drv = sys_get_drive();
    }
    out[0] = (char)('A' + drv);
    out[1] = ':';
    out[2] = 0;
    if ( rest[0] == '\\' || rest[0] == '/' ) {
        str_catn( out, rest, PATH_MAX );
        return;
    }
    out[2] = '\\';
    out[3] = 0;
    if ( sys_get_cwd( drv, dir ) == 0 && dir[0] ) {
        str_catn( out, dir, PATH_MAX );
        str_catn( out, "\\", PATH_MAX );
    }
    str_catn( out, rest, PATH_MAX );
}

static void fill_lists( FDATA *fd, int with_files )
{
    FINDREC rec;
    u32 handle;
    char spec[PATH_MAX], drive[8];
    int drv;

    sl_free( &fd->files );
    sl_free( &fd->dirs );
    cur_dir( fd->dirtext );
    if ( with_files && sys_find_first( fd->pattern, ATTR_READONLY | ATTR_ARCHIVE, &rec, &handle ) == 0 ) {
        do {
            if ( !(rec.attr & ATTR_DIR) ) {
                sl_add( &fd->files, short_names ? rec.alias : rec.name );
            }
        } while ( sys_find_next( handle, &rec ) == 0 );
        sys_find_close( handle );
    }
    str_cpy( spec, "*.*" );
    if ( sys_find_first( spec, ATTR_DIR | (ATTR_DIR << 8), &rec, &handle ) == 0 ) {
        do {
            if ( (rec.attr & ATTR_DIR) && str_cmp( rec.name, "." ) != 0 ) {
                sl_add( &fd->dirs, short_names ? rec.alias : rec.name );
            }
        } while ( sys_find_next( handle, &rec ) == 0 );
        sys_find_close( handle );
    }
    for ( drv = 0; drv < 26; drv++ ) {
        if ( sys_drive_valid( drv ) ) {
            drive[0] = '[';
            drive[1] = '-';
            drive[2] = (char)('A' + drv);
            drive[3] = '-';
            drive[4] = ']';
            drive[5] = 0;
            sl_add( &fd->dirs, drive );
        }
    }
    sl_sort( &fd->files );
    sl_sort( &fd->dirs );
}

static const char *files_item( DLG *dlg, int index )
{
    FDATA *fd = (FDATA *)dlg->data;

    return index < fd->files.count ? fd->files.item[index] : "";
}

static const char *dirs_item( DLG *dlg, int index )
{
    FDATA *fd = (FDATA *)dlg->data;

    return index < fd->dirs.count ? fd->dirs.item[index] : "";
}

static void refresh( DLG *dlg, int with_files )
{
    FDATA *fd = (FDATA *)dlg->data;

    fill_lists( fd, with_files );
    if ( fd->files_ctl >= 0 ) {
        dlg->ctl[fd->files_ctl].count = fd->files.count;
        dlg->ctl[fd->files_ctl].sel = 0;
        dlg->ctl[fd->files_ctl].top = 0;
    }
    dlg->ctl[fd->dirs_ctl].count = fd->dirs.count;
    dlg->ctl[fd->dirs_ctl].sel = 0;
    dlg->ctl[fd->dirs_ctl].top = 0;
}

/* "C:\", "[-A-]", ".." or a directory: go there.  1 = went */
static int change_to( const char *where )
{
    char path[PATH_MAX];
    u32 len;

    if ( where[0] == '[' && where[1] == '-' ) {
        sys_set_drive( ch_upper( where[2] ) - 'A' );
        return 1;
    }
    str_cpyn( path, where, sizeof( path ) );
    len = str_len( path );
    if ( len == 2 && path[1] == ':' ) {
        if ( !sys_drive_valid( ch_upper( path[0] ) - 'A' ) ) {
            return 0;
        }
        sys_set_drive( ch_upper( path[0] ) - 'A' );
        return 1;
    }
    if ( path[1] == ':' && sys_drive_valid( ch_upper( path[0] ) - 'A' ) ) {
        sys_set_drive( ch_upper( path[0] ) - 'A' );
    }
    return sys_chdir( path ) == 0;
}

static void file_changed( DLG *dlg, int ctl )
{
    FDATA *fd = (FDATA *)dlg->data;
    CTL *name = &dlg->ctl[1];

    if ( ctl == fd->files_ctl && fd->files.count ) {
        str_cpyn( fd->name, fd->files.item[dlg->ctl[ctl].sel], sizeof( fd->name ) );
        name->caret = (int)str_len( fd->name );
        name->fresh = 1;
    }
}

static int has_wild( const char *name )
{
    return str_chr( name, '*' ) != NULL || str_chr( name, '?' ) != NULL;
}

/* OK, or Enter on a list: what the typed name means */
static int file_activate( DLG *dlg, int ctl )
{
    FDATA *fd = (FDATA *)dlg->data;
    u16 attr;
    char *last;
    int with_files = fd->files_ctl >= 0;

    if ( dlg->ctl[ctl].type == CT_BUTTON && dlg->ctl[ctl].id == ID_CANCEL ) {
        return ID_CANCEL;
    }
    if ( ctl == fd->dirs_ctl ) {
        if ( fd->dirs.count && change_to( fd->dirs.item[dlg->ctl[ctl].sel] ) ) {
            refresh( dlg, with_files );
        }
        return 0;
    }
    if ( ctl == fd->files_ctl ) {
        file_changed( dlg, ctl );
    }
    if ( fd->name[0] == 0 ) {
        return 0;
    }
    if ( has_wild( fd->name ) ) {                  /* a new pattern, maybe elsewhere */
        last = str_rchr( fd->name, '\\' );
        if ( last ) {
            *last = 0;
            change_to( fd->name[0] ? fd->name : "\\" );
            str_cpyn( fd->pattern, last + 1, sizeof( fd->pattern ) );
        } else if ( fd->name[1] == ':' ) {
            char drive[3];
            drive[0] = fd->name[0];
            drive[1] = ':';
            drive[2] = 0;
            change_to( drive );
            str_cpyn( fd->pattern, fd->name + 2, sizeof( fd->pattern ) );
        } else {
            str_cpyn( fd->pattern, fd->name, sizeof( fd->pattern ) );
        }
        str_cpy( fd->name, fd->pattern );
        refresh( dlg, with_files );
        return 0;
    }
    if ( (str_len( fd->name ) == 2 && fd->name[1] == ':')
         || (sys_get_attr( fd->name, &attr ) == 0 && (attr & ATTR_DIR)) ) {
        if ( change_to( fd->name ) ) {
            str_cpy( fd->name, with_files ? fd->pattern : "" );
            refresh( dlg, with_files );
        }
        return 0;
    }
    make_path( fd->name, fd->result );
    return ID_OK;
}

int dlg_open( char *path )
{
    static FDATA fd;
    CTL ctl[8];
    DLG dlg;
    int answer;

    mem_set( &fd, 0, sizeof( fd ) );
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    str_cpy( fd.pattern, "*.TXT" );
    str_cpy( fd.name, fd.pattern );
    fd.files_ctl = 3;
    fd.dirs_ctl = 4;
    ctl[0].type = CT_LABEL; ctl[0].row = 2; ctl[0].col = 2; ctl[0].text = "File &Name:";
    ctl[1].type = CT_EDIT;  ctl[1].row = 2; ctl[1].col = 14; ctl[1].width = 50;
    ctl[1].buf = fd.name;   ctl[1].max = PATH_MAX;
    ctl[2].type = CT_TEXT;  ctl[2].row = 4; ctl[2].col = 2; ctl[2].text = fd.dirtext;
    ctl[3].type = CT_LIST;  ctl[3].row = 5; ctl[3].col = 2; ctl[3].width = 44; ctl[3].height = 11;
    ctl[3].text = "&Files"; ctl[3].item = files_item;
    ctl[4].type = CT_LIST;  ctl[4].row = 5; ctl[4].col = 48; ctl[4].width = 18; ctl[4].height = 11;
    ctl[4].text = "&Dirs/Drives"; ctl[4].item = dirs_item;
    ctl[5].type = CT_BUTTON; ctl[5].row = 17; ctl[5].col = 14; ctl[5].text = "OK";
    ctl[5].id = ID_OK; ctl[5].is_default = 1;
    ctl[6].type = CT_BUTTON; ctl[6].row = 17; ctl[6].col = 26; ctl[6].text = "Cancel";
    ctl[6].id = ID_CANCEL;
    ctl[7].type = CT_BUTTON; ctl[7].row = 17; ctl[7].col = 42; ctl[7].text = "&Help";
    ctl[7].id = ID_HELP;
    dlg.title = "Open";
    dlg.rows = 19;
    dlg.cols = 68;
    dlg.ctl = ctl;
    dlg.nctl = 8;
    dlg.focus = 1;
    dlg.help = HELP_OPEN;
    dlg.data = &fd;
    dlg.changed = file_changed;
    dlg.activate = file_activate;
    refresh( &dlg, 1 );
    answer = dlg_run( &dlg );
    if ( answer == ID_OK ) {
        str_cpy( path, fd.result );
    }
    sl_free( &fd.files );
    sl_free( &fd.dirs );
    return answer == ID_OK;
}

int dlg_saveas( DOC *doc, char *path )
{
    static FDATA fd;
    CTL ctl[7];
    DLG dlg;
    int answer;
    const char *last;

    mem_set( &fd, 0, sizeof( fd ) );
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    if ( doc->path[0] ) {
        last = str_rchr( doc->path, '\\' );
        str_cpyn( fd.name, last ? last + 1 : doc->path, sizeof( fd.name ) );
    }
    fd.files_ctl = -1;
    fd.dirs_ctl = 3;
    ctl[0].type = CT_LABEL; ctl[0].row = 2; ctl[0].col = 2; ctl[0].text = "File &Name:";
    ctl[1].type = CT_EDIT;  ctl[1].row = 2; ctl[1].col = 14; ctl[1].width = 40;
    ctl[1].buf = fd.name;   ctl[1].max = PATH_MAX;
    ctl[2].type = CT_TEXT;  ctl[2].row = 4; ctl[2].col = 2; ctl[2].text = fd.dirtext;
    ctl[3].type = CT_LIST;  ctl[3].row = 5; ctl[3].col = 2; ctl[3].width = 30; ctl[3].height = 11;
    ctl[3].text = "&Dirs/Drives"; ctl[3].item = dirs_item;
    ctl[4].type = CT_BUTTON; ctl[4].row = 7; ctl[4].col = 38; ctl[4].text = "OK";
    ctl[4].id = ID_OK; ctl[4].is_default = 1;
    ctl[5].type = CT_BUTTON; ctl[5].row = 10; ctl[5].col = 38; ctl[5].text = "Cancel";
    ctl[5].id = ID_CANCEL;
    ctl[6].type = CT_BUTTON; ctl[6].row = 13; ctl[6].col = 38; ctl[6].text = "&Help";
    ctl[6].id = ID_HELP;
    dlg.title = "Save As";
    dlg.rows = 18;
    dlg.cols = 58;
    dlg.ctl = ctl;
    dlg.nctl = 7;
    dlg.focus = 1;
    dlg.help = HELP_SAVE;
    dlg.data = &fd;
    dlg.changed = file_changed;
    dlg.activate = file_activate;
    refresh( &dlg, 0 );
    answer = dlg_run( &dlg );
    if ( answer == ID_OK ) {
        str_cpy( path, fd.result );
    }
    sl_free( &fd.files );
    sl_free( &fd.dirs );
    return answer == ID_OK;
}

/* ------------------------------------------------------------------ */
/* Find and Replace                                                    */
/* ------------------------------------------------------------------ */

static char find_text[128], change_text[128];
static int  match_case, whole_word, have_find;

int find_can_repeat( void )
{
    return have_find && find_text[0];
}

static int word_ch( int ch )
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
           || ch == '_';
}

static int match_at( LINE *line, u32 at, u32 len )
{
    u32 pos;
    int lhs, rhs;

    if ( at + len > line->len ) {
        return 0;
    }
    for ( pos = 0; pos < len; pos++ ) {
        lhs = (u8)line->text[at + pos];
        rhs = (u8)find_text[pos];
        if ( !match_case ) {
            lhs = ch_upper( lhs );
            rhs = ch_upper( rhs );
        }
        if ( lhs != rhs ) {
            return 0;
        }
    }
    if ( whole_word ) {
        if ( at > 0 && word_ch( (u8)line->text[at - 1] ) ) {
            return 0;
        }
        if ( at + len < line->len && word_ch( (u8)line->text[at + len] ) ) {
            return 0;
        }
    }
    return 1;
}

/* the next match at or after "from", before "limit"; 1 = found */
static int search_range( DOC *doc, POS from, POS limit, POS *found )
{
    u32 len = str_len( find_text ), line, at, end;
    LINE *text;

    if ( len == 0 ) {
        return 0;
    }
    for ( line = from.line; line < doc->count && line <= limit.line; line++ ) {
        text = doc_line( doc, line );
        at = line == from.line ? from.col : 0;
        end = line == limit.line ? limit.col : text->len;
        for ( ; at + len <= text->len && at <= end; at++ ) {
            if ( line == limit.line && at + len > end && end < text->len ) {
                break;
            }
            if ( match_at( text, at, len ) ) {
                found->line = line;
                found->col = at;
                return 1;
            }
        }
    }
    return 0;
}

/* from "from" to the end, and then round from the top to "from" */
static int search_wrap( DOC *doc, POS from, POS *found )
{
    POS end, top;

    end.line = doc->count - 1;
    end.col = doc_line( doc, end.line )->len;
    if ( search_range( doc, from, end, found ) ) {
        return 1;
    }
    top.line = 0;
    top.col = 0;
    return search_range( doc, top, from, found );
}

static void select_match( VIEW *view, POS at )
{
    LINE *line = doc_line( view->doc, at.line );

    view->sel = 1;
    view->sel_line = at.line;
    view->sel_col = doc_col_of( view->doc, line, at.col );
    view->line = at.line;
    view->col = doc_col_of( view->doc, line, at.col + str_len( find_text ) );
    view->want = view->col;
    view_keep_visible( view );
}

void find_again( void )
{
    VIEW *view = app_view();
    POS from = view_pos( view ), found;
    LINE *line = doc_line( view->doc, from.line );

    if ( !find_can_repeat() ) {
        dlg_find( 0 );
        return;
    }
    if ( from.col > line->len ) {
        from.col = line->len;
    }
    if ( search_wrap( view->doc, from, &found ) ) {
        select_match( view, found );
    } else {
        msg_box( "Match not found.", MB_OK );
    }
}

/* the word under the cursor, to offer in the Find field */
static void word_at_cursor( char *out, u32 max )
{
    VIEW *view = app_view();
    LINE *line = doc_line( view->doc, view->line );
    POS from, to;
    u32 start, end;

    out[0] = 0;
    if ( view_has_sel( view ) ) {
        view_sel_range( view, &from, &to );
        if ( from.line == to.line && to.col - from.col < max ) {
            mem_cpy( out, line->text + from.col, to.col - from.col );
            out[to.col - from.col] = 0;
        }
        return;
    }
    start = doc_index_of( view->doc, line, view->col );
    if ( start >= line->len || !word_ch( (u8)line->text[start] ) ) {
        return;
    }
    while ( start > 0 && word_ch( (u8)line->text[start - 1] ) ) {
        start--;
    }
    end = start;
    while ( end < line->len && word_ch( (u8)line->text[end] ) ) {
        end++;
    }
    if ( end - start < max ) {
        mem_cpy( out, line->text + start, end - start );
        out[end - start] = 0;
    }
}

/* one match: Change, Skip or Cancel */
static int ask_change( void )
{
    CTL ctl[5];
    DLG dlg;

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl[0].type = CT_TEXT; ctl[0].row = 2; ctl[0].col = 1; ctl[0].width = 44;
    ctl[0].text = "Change this one?";
    ctl[1].type = CT_BUTTON; ctl[1].row = 4; ctl[1].col = 3; ctl[1].text = "&Change";
    ctl[1].id = ID_YES; ctl[1].is_default = 1;
    ctl[2].type = CT_BUTTON; ctl[2].row = 4; ctl[2].col = 15; ctl[2].text = "&Skip";
    ctl[2].id = ID_NO;
    ctl[3].type = CT_BUTTON; ctl[3].row = 4; ctl[3].col = 25; ctl[3].text = "Cancel";
    ctl[3].id = ID_CANCEL;
    ctl[4].type = CT_BUTTON; ctl[4].row = 4; ctl[4].col = 36; ctl[4].text = "&Help";
    ctl[4].id = ID_HELP;
    dlg.title = "Change";
    dlg.rows = 6;
    dlg.cols = 46;
    dlg.top = scr_rows - 8;                 /* low, out of the match's way */
    dlg.ctl = ctl;
    dlg.nctl = 5;
    dlg.focus = 1;
    dlg.help = HELP_REPLACE;
    return dlg_run( &dlg );
}

static void replace_all( int verify )
{
    VIEW *view = app_view();
    DOC *doc = view->doc;
    POS at = view_pos( view ), limit, found, end;
    u32 find_len = str_len( find_text ), change_len = str_len( change_text );
    int wrapped = 0, changed = 0, answer;
    LINE *line = doc_line( doc, at.line );

    if ( at.col > line->len ) {
        at.col = line->len;
    }
    limit = at;                             /* where it started, to stop at */
    for ( ;; ) {
        end.line = doc->count - 1;
        end.col = doc_line( doc, end.line )->len;
        if ( !search_range( doc, at, wrapped ? limit : end, &found ) ) {
            if ( wrapped ) {
                break;
            }
            wrapped = 1;
            at.line = 0;
            at.col = 0;
            continue;
        }
        if ( verify ) {
            select_match( view, found );
            answer = ask_change();
            if ( answer == ID_CANCEL ) {
                break;
            }
            if ( answer == ID_NO ) {
                at = found;
                at.col += find_len ? find_len : 1;
                continue;
            }
        }
        end = found;
        end.col += find_len;
        if ( !doc_delete( doc, found, end ) ) {
            out_of_memory();
            break;
        }
        at = found;
        if ( change_len && !doc_insert( doc, &at, change_text, change_len ) ) {
            out_of_memory();
            break;
        }
        /* a change before the starting point, on its line, moves it */
        if ( found.line == limit.line && found.col < limit.col ) {
            limit.col = limit.col + change_len - find_len;
        }
        changed++;
    }
    view->sel = 0;
    view_fixup( doc );
    if ( changed ) {
        view_goto( view, at.line, doc_col_of( doc, doc_line( doc, at.line ), at.col ), 0 );
    }
    msg_box( changed ? "Change complete." : "Match not found.", MB_OK );
}

static int find_press( DLG *dlg, int ctl )
{
    int id = dlg->ctl[ctl].id;

    if ( id == ID_CANCEL ) {
        return ID_CANCEL;
    }
    if ( find_text[0] == 0 ) {
        return 0;
    }
    return id;
}

int dlg_find( int replace )
{
    CTL ctl[10];
    DLG dlg;
    int answer, row, count = 0;
    char offer[128];

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    word_at_cursor( offer, sizeof( offer ) );
    if ( offer[0] ) {
        str_cpy( find_text, offer );
    }
    ctl[count].type = CT_LABEL; ctl[count].row = 2; ctl[count].col = 2;
    ctl[count++].text = "Find &What:";
    ctl[count].type = CT_EDIT; ctl[count].row = 2; ctl[count].col = 15; ctl[count].width = 40;
    ctl[count].buf = find_text; ctl[count++].max = sizeof( find_text );
    row = 4;
    if ( replace ) {
        ctl[count].type = CT_LABEL; ctl[count].row = 4; ctl[count].col = 2;
        ctl[count++].text = "&Change To:";
        ctl[count].type = CT_EDIT; ctl[count].row = 4; ctl[count].col = 15; ctl[count].width = 40;
        ctl[count].buf = change_text; ctl[count++].max = sizeof( change_text );
        row = 6;
    }
    ctl[count].type = CT_CHECK; ctl[count].row = row; ctl[count].col = 2;
    ctl[count].text = "Match &Upper/Lowercase"; ctl[count++].value = &match_case;
    ctl[count].type = CT_CHECK; ctl[count].row = row; ctl[count].col = 34;
    ctl[count].text = "Whole Wor&d"; ctl[count++].value = &whole_word;
    if ( replace ) {
        ctl[count].type = CT_BUTTON; ctl[count].row = row + 2; ctl[count].col = 2;
        ctl[count].text = "Find and &Verify"; ctl[count].id = ID_YES; ctl[count++].is_default = 1;
        ctl[count].type = CT_BUTTON; ctl[count].row = row + 2; ctl[count].col = 23;
        ctl[count].text = "Change &All"; ctl[count++].id = ID_NO;
        ctl[count].type = CT_BUTTON; ctl[count].row = row + 2; ctl[count].col = 38;
        ctl[count].text = "Cancel"; ctl[count++].id = ID_CANCEL;
        ctl[count].type = CT_BUTTON; ctl[count].row = row + 2; ctl[count].col = 49;
        ctl[count].text = "&Help"; ctl[count++].id = ID_HELP;
    } else {
        ctl[count].type = CT_BUTTON; ctl[count].row = row + 2; ctl[count].col = 12;
        ctl[count].text = "OK"; ctl[count].id = ID_OK; ctl[count++].is_default = 1;
        ctl[count].type = CT_BUTTON; ctl[count].row = row + 2; ctl[count].col = 24;
        ctl[count].text = "Cancel"; ctl[count++].id = ID_CANCEL;
        ctl[count].type = CT_BUTTON; ctl[count].row = row + 2; ctl[count].col = 38;
        ctl[count].text = "&Help"; ctl[count++].id = ID_HELP;
    }
    dlg.title = replace ? "Change" : "Find";
    dlg.rows = row + 4;
    dlg.cols = replace ? 60 : 58;
    dlg.ctl = ctl;
    dlg.nctl = count;
    dlg.focus = 1;
    dlg.help = replace ? HELP_REPLACE : HELP_FIND;
    dlg.activate = find_press;
    answer = dlg_run( &dlg );
    if ( answer == ID_CANCEL || find_text[0] == 0 ) {
        return 0;
    }
    have_find = 1;
    if ( !replace ) {
        VIEW *view = app_view();
        POS from = view_pos( view ), found;
        LINE *line = doc_line( view->doc, from.line );
        if ( from.col > line->len ) {
            from.col = line->len;
        }
        if ( search_wrap( view->doc, from, &found ) ) {
            select_match( view, found );
        } else {
            msg_box( "Match not found.", MB_OK );
        }
        return 1;
    }
    replace_all( answer == ID_YES );
    return 1;
}

/* ------------------------------------------------------------------ */
/* Print                                                               */
/* ------------------------------------------------------------------ */

static int print_what;

int dlg_print( void )
{
    CTL ctl[5];
    DLG dlg;
    VIEW *view = app_view();
    DOC *doc = view->doc;
    POS from, to;
    u32 handle, done, line, start, end;
    LINE *text;
    int rc;

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    print_what = view_has_sel( view ) ? 1 : 2;
    ctl[0].type = CT_RADIO; ctl[0].row = 2; ctl[0].col = 4; ctl[0].text = "&Selected Text Only";
    ctl[0].value = &print_what; ctl[0].id = 1;
    ctl[1].type = CT_RADIO; ctl[1].row = 3; ctl[1].col = 4; ctl[1].text = "&Complete Document";
    ctl[1].value = &print_what; ctl[1].id = 2;
    ctl[2].type = CT_BUTTON; ctl[2].row = 5; ctl[2].col = 4; ctl[2].text = "OK";
    ctl[2].id = ID_OK; ctl[2].is_default = 1;
    ctl[3].type = CT_BUTTON; ctl[3].row = 5; ctl[3].col = 14; ctl[3].text = "Cancel";
    ctl[3].id = ID_CANCEL;
    ctl[4].type = CT_BUTTON; ctl[4].row = 5; ctl[4].col = 26; ctl[4].text = "&Help";
    ctl[4].id = ID_HELP;
    dlg.title = "Print";
    dlg.rows = 7;
    dlg.cols = 38;
    dlg.ctl = ctl;
    dlg.nctl = 5;
    dlg.focus = print_what == 1 ? 0 : 1;
    dlg.help = HELP_PRINT;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return 0;
    }
    if ( print_what == 1 && view_has_sel( view ) ) {
        view_sel_range( view, &from, &to );
    } else {
        from.line = 0;
        from.col = 0;
        to.line = doc->count - 1;
        to.col = doc_line( doc, to.line )->len;
    }
    rc = sys_open_write( print_port, &handle );
    if ( rc != 0 ) {
        msg_box2( print_port, err_text( rc ), MB_OK );
        return 0;
    }
    for ( line = from.line; line <= to.line && rc == 0; line++ ) {
        text = doc_line( doc, line );
        start = line == from.line ? from.col : 0;
        end = line == to.line ? to.col : text->len;
        if ( end > text->len ) {
            end = text->len;
        }
        if ( end > start ) {
            rc = sys_write( handle, text->text + start, end - start, &done );
        }
        if ( rc == 0 ) {
            rc = sys_write( handle, "\r\n", 2, &done );
        }
    }
    if ( rc == 0 ) {
        rc = sys_write( handle, "\f", 1, &done );
    }
    sys_close( handle );
    if ( rc != 0 ) {
        msg_box2( "The printer could not be written to.", err_text( rc ), MB_OK );
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

static const char *ports[] = { "PRN", "LPT1", "LPT2", "LPT3", "COM1", "COM2" };

void dlg_settings( void )
{
    CTL ctl[12];
    DLG dlg;
    char tabs[8];
    int port = 0, index, ok;
    u32 value;

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    fmt_dec( (u32)tab_size, 0, tabs );
    for ( index = 0; index < 6; index++ ) {
        if ( str_icmp( print_port, ports[index] ) == 0 ) {
            port = index;
        }
    }
    ctl[0].type = CT_LABEL; ctl[0].row = 2; ctl[0].col = 3; ctl[0].text = "&Tab Stops:";
    ctl[1].type = CT_EDIT;  ctl[1].row = 2; ctl[1].col = 15; ctl[1].width = 4;
    ctl[1].buf = tabs; ctl[1].max = 3;
    ctl[2].type = CT_LABEL; ctl[2].row = 4; ctl[2].col = 3; ctl[2].text = "Print Port:";
    for ( index = 0; index < 6; index++ ) {
        ctl[3 + index].type = CT_RADIO;
        ctl[3 + index].row = 5 + index % 3;
        ctl[3 + index].col = 5 + (index / 3) * 14;
        ctl[3 + index].text = index == 0 ? "&PRN" : ports[index];
        ctl[3 + index].value = &port;
        ctl[3 + index].id = index;
    }
    ctl[9].type = CT_BUTTON; ctl[9].row = 9; ctl[9].col = 5; ctl[9].text = "OK";
    ctl[9].id = ID_OK; ctl[9].is_default = 1;
    ctl[10].type = CT_BUTTON; ctl[10].row = 9; ctl[10].col = 15; ctl[10].text = "Cancel";
    ctl[10].id = ID_CANCEL;
    ctl[11].type = CT_BUTTON; ctl[11].row = 9; ctl[11].col = 27; ctl[11].text = "&Help";
    ctl[11].id = ID_HELP;
    dlg.title = "Settings";
    dlg.rows = 11;
    dlg.cols = 38;
    dlg.ctl = ctl;
    dlg.nctl = 12;
    dlg.focus = 1;
    dlg.help = HELP_SETTINGS;
    for ( ;; ) {
        if ( dlg_run( &dlg ) != ID_OK ) {
            return;
        }
        value = dec_parse( tabs, &ok );
        if ( ok && value >= 1 && value <= 99 ) {
            break;
        }
        msg_box( "Tab stops have to be from 1 to 99.", MB_OK );
        dlg.focus = 1;
    }
    tab_size = (int)value;
    str_cpy( print_port, ports[port] );
    for ( index = 0; index < nviews; index++ ) {
        view_keep_visible( &views[index] );
    }
}

/* ------------------------------------------------------------------ */
/* Colors                                                              */
/* ------------------------------------------------------------------ */

static const char *colours[16] = {
    "Black", "Blue", "Green", "Cyan", "Red", "Magenta", "Brown", "White",
    "Gray", "Bright Blue", "Bright Green", "Bright Cyan", "Bright Red", "Pink",
    "Yellow", "Bright White"
};

static const char *colour_item( DLG *dlg, int index )
{
    (void)dlg;
    return colours[index & 15];
}

static int colour_part;

static void colour_pick( DLG *dlg, int ctl )
{
    u8 attr = colour_part == 0 ? pal.text : colour_part == 1 ? pal.menu : pal.status;

    if ( ctl == 3 || ctl == 4 || ctl == 5 ) {   /* another part: show its colours */
        dlg->ctl[7].sel = attr & 0x0F;
        dlg->ctl[8].sel = (attr >> 4) & 0x07;
    }
}

static void colour_apply( int part, int fg, int bg )
{
    u8 attr = (u8)((bg << 4) | fg);
    u8 inverse = (u8)(((fg & 7) << 4) | bg);

    if ( part == 0 ) {
        pal.text = attr;
        pal.frame = attr;
        pal.select = inverse;
        pal.title = inverse;
    } else if ( part == 1 ) {
        pal.menu = attr;
        pal.dlg = attr;
        pal.button = attr;
        pal.menu_hot = (u8)((bg << 4) | 0x0F);
        pal.dlg_hot = pal.menu_hot;
        pal.button_focus = pal.menu_hot;
        pal.menu_sel = inverse;
        pal.menu_sel_hot = (u8)(inverse | 0x08);
        pal.menu_off = (u8)((bg << 4) | 0x08);
        pal.scroll = attr;
    } else {
        pal.status = attr;
        pal.status_hot = (u8)((bg << 4) | 0x0F);
    }
}

void dlg_colors( void )
{
    CTL ctl[12];
    DLG dlg;
    PALETTE before = pal;
    int answer;

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    colour_part = 0;
    ctl[0].type = CT_TEXT; ctl[0].row = 2; ctl[0].col = 3; ctl[0].text = "Choose a part of the screen and its colours.";
    ctl[1].type = CT_TEXT; ctl[1].row = 3; ctl[1].col = 3; ctl[1].text = "They last until the editor is left.";
    ctl[2].type = CT_LABEL; ctl[2].row = 5; ctl[2].col = 3; ctl[2].text = "Part:";
    ctl[3].type = CT_RADIO; ctl[3].row = 6; ctl[3].col = 4; ctl[3].text = "&Text";
    ctl[3].value = &colour_part; ctl[3].id = 0;
    ctl[4].type = CT_RADIO; ctl[4].row = 7; ctl[4].col = 4; ctl[4].text = "&Menus and dialogs";
    ctl[4].value = &colour_part; ctl[4].id = 1;
    ctl[5].type = CT_RADIO; ctl[5].row = 8; ctl[5].col = 4; ctl[5].text = "&Status bar";
    ctl[5].value = &colour_part; ctl[5].id = 2;
    ctl[6].type = CT_TEXT; ctl[6].row = 10; ctl[6].col = 3; ctl[6].text = "";
    ctl[7].type = CT_LIST; ctl[7].row = 5; ctl[7].col = 28; ctl[7].width = 18; ctl[7].height = 8;
    ctl[7].text = "&Foreground"; ctl[7].count = 16; ctl[7].item = colour_item;
    ctl[8].type = CT_LIST; ctl[8].row = 5; ctl[8].col = 48; ctl[8].width = 18; ctl[8].height = 8;
    ctl[8].text = "&Background"; ctl[8].count = 8; ctl[8].item = colour_item;
    ctl[9].type = CT_BUTTON; ctl[9].row = 14; ctl[9].col = 14; ctl[9].text = "OK";
    ctl[9].id = ID_OK; ctl[9].is_default = 1;
    ctl[10].type = CT_BUTTON; ctl[10].row = 14; ctl[10].col = 26; ctl[10].text = "Cancel";
    ctl[10].id = ID_CANCEL;
    ctl[11].type = CT_BUTTON; ctl[11].row = 14; ctl[11].col = 40; ctl[11].text = "&Help";
    ctl[11].id = ID_HELP;
    dlg.title = "Colors";
    dlg.rows = 16;
    dlg.cols = 70;
    dlg.ctl = ctl;
    dlg.nctl = 12;
    dlg.focus = 3;
    dlg.help = HELP_COLORS;
    dlg.changed = colour_pick;
    colour_pick( &dlg, 3 );
    for ( ;; ) {
        answer = dlg_run( &dlg );
        if ( answer != ID_OK ) {
            pal = before;
            return;
        }
        if ( (ctl[7].sel & 7) == ctl[8].sel ) {
            msg_box( "The text would not show: its colour is the background's.", MB_OK );
            continue;
        }
        colour_apply( colour_part, ctl[7].sel, ctl[8].sel );
        return;
    }
}

/* ------------------------------------------------------------------ */
/* About, and the line between the windows                             */
/* ------------------------------------------------------------------ */

void dlg_about( void )
{
    msg_box2( "PM-DOS Editor, version 1.0", "Part of PM-DOS.  A text editor for DOS files.",
              MB_OK );
}

void dlg_size_window( void )
{
    int key;

    app_status( "Up and Down, or the mouse, move the line between the windows; Enter when done" );
    for ( ;; ) {
        app_layout();
        app_draw();
        scr_cursor( 0, 0, CUR_HIDE );
        scr_flush();
        key = key_get();
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        if ( key == K_UP ) {
            split_at--;
        } else if ( key == K_DOWN ) {
            split_at++;
        } else if ( key == K_MOUSE ) {
            /* the line follows the pointer while the button is down, and
               letting go puts it there */
            if ( mouse.kind != ME_UP ) {
                split_at = mouse.row - 1;
            } else {
                break;
            }
        } else if ( key == K_ENTER || key == K_ESC ) {
            break;
        }
    }
    app_status( NULL );
}
