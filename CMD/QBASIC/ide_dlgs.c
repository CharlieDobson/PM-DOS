/*
 * IDE_DLGS.C - the dialogs: Open, Save As, Print, New SUB and New
 * FUNCTION, SUBs, Find and Change, Display, Help Path, About, and the
 * box QBasic greets you with.  Each is where QBasic puts its own, with
 * the same controls in the same order; the sentences in them are this
 * program's.  EDIT's DLGS.C is where the file lists and the searching
 * come from.
 */
#include "ide.h"

char help_path[PATH_MAX];

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

/* a control at a place in its dialog */
static CTL *put( CTL *ctl, int type, int row, int col, int width, const char *text )
{
    mem_set( ctl, 0, sizeof( *ctl ) );
    ctl->type = type;
    ctl->row = row;
    ctl->col = col;
    ctl->width = width;
    ctl->text = text;
    return ctl;
}

static CTL *button( CTL *ctl, int row, int col, const char *text, int id, int is_default )
{
    put( ctl, CT_BUTTON, row, col, 0, text );
    ctl->id = id;
    ctl->is_default = is_default;
    return ctl;
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
    int     name_ctl, files_ctl, dirs_ctl;
} FDATA;

static void cur_dir( char *out )
{
    int drv = sys_get_drive();

    out[0] = (char)('A' + drv);
    out[1] = ':';
    out[2] = '\\';
    sys_get_cwd( drv, out + 3 );
}

/* the full name of what was typed: -> 0 if it will not fit a path */
static int make_path( const char *name, char *out )
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
        return str_catn( out, rest, PATH_MAX );
    }
    out[2] = '\\';
    out[3] = 0;
    if ( sys_get_cwd( drv, dir ) == 0 && dir[0] ) {
        if ( !str_catn( out, dir, PATH_MAX ) || !str_catn( out, "\\", PATH_MAX ) ) {
            return 0;
        }
    }
    return str_catn( out, rest, PATH_MAX );
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
                sl_add( &fd->files, rec.alias[0] ? rec.alias : rec.name );
            }
        } while ( sys_find_next( handle, &rec ) == 0 );
        sys_find_close( handle );
    }
    str_cpy( spec, "*.*" );
    if ( sys_find_first( spec, ATTR_DIR | (ATTR_DIR << 8), &rec, &handle ) == 0 ) {
        do {
            if ( (rec.attr & ATTR_DIR) && str_cmp( rec.name, "." ) != 0 ) {
                sl_add( &fd->dirs, rec.alias[0] ? rec.alias : rec.name );
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
    CTL *name = &dlg->ctl[fd->name_ctl];

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
        dlg->stay = 1;                          /* another directory: not OK yet */
        return 0;
    }
    if ( ctl == fd->files_ctl ) {
        file_changed( dlg, ctl );
    }
    if ( fd->name[0] == 0 ) {
        dlg->stay = 1;
        return 0;
    }
    if ( has_wild( fd->name ) ) {
        last = str_rchr( fd->name, '\\' );
        if ( last ) {
            str_cpyn( fd->pattern, last + 1, sizeof( fd->pattern ) );
            if ( last == fd->name + 2 && fd->name[1] == ':' ) {
                last[1] = 0;                    /* C:\*.BAS: the root, not C:'s directory */
            } else {
                *last = 0;
            }
            change_to( fd->name[0] ? fd->name : "\\" );
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
        dlg->stay = 1;
        return 0;
    }
    if ( (str_len( fd->name ) == 2 && fd->name[1] == ':')
         || (sys_get_attr( fd->name, &attr ) == 0 && (attr & ATTR_DIR)) ) {
        if ( change_to( fd->name ) ) {
            str_cpy( fd->name, with_files ? fd->pattern : "" );
            refresh( dlg, with_files );
        }
        dlg->stay = 1;
        return 0;
    }
    if ( !make_path( fd->name, fd->result ) ) {
        msg_box( "Bad file name", MB_OK, 1 );   /* too long a path */
        dlg->stay = 1;
        return 0;
    }
    /* a name with no extension is a .BAS file */
    last = str_rchr( fd->result, '\\' );
    if ( str_chr( last ? last : fd->result, '.' ) == NULL
      && !str_catn( fd->result, ".BAS", sizeof( fd->result ) ) ) {
        msg_box( "Bad file name", MB_OK, 1 );
        dlg->stay = 1;
        return 0;
    }
    return ID_OK;
}

int dlg_open( char *path )
{
    static FDATA fd;
    CTL ctl[9];
    DLG dlg;
    int answer;

    mem_set( &fd, 0, sizeof( fd ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    str_cpy( fd.pattern, "*.BAS" );
    str_cpy( fd.name, fd.pattern );
    fd.name_ctl = 1;
    fd.files_ctl = 3;
    fd.dirs_ctl = 4;
    put( &ctl[0], CT_LABEL, 2, 2, 0, "File &Name:" );
    put( &ctl[1], CT_EDIT, 2, 14, 50, NULL );
    ctl[1].buf = fd.name;
    ctl[1].max = PATH_MAX;
    put( &ctl[2], CT_TEXT, 4, 2, 0, fd.dirtext );
    put( &ctl[3], CT_LIST, 6, 2, 45, "&Files" );
    ctl[3].height = 10;
    ctl[3].columns = 3;
    ctl[3].item = files_item;
    put( &ctl[4], CT_LIST, 6, 49, 16, "&Dirs/Drives" );
    ctl[4].height = 10;
    ctl[4].item = dirs_item;
    button( &ctl[5], 18, 12, " OK ", ID_OK, 1 );
    button( &ctl[6], 18, 28, " Cancel ", ID_CANCEL, 0 );
    button( &ctl[7], 18, 48, " &Help ", ID_HELP, 0 );
    dlg.title = "Open";
    dlg.top = 2;
    dlg.left = 6;
    dlg.rows = 20;
    dlg.cols = 67;
    dlg.sep = 17;
    dlg.ctl = ctl;
    dlg.nctl = 8;
    dlg.focus = 1;
    dlg.help = "d.open";
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

int dlg_saveas( char *path )
{
    static FDATA fd;
    CTL ctl[7];
    DLG dlg;
    int answer;
    const char *last;

    mem_set( &fd, 0, sizeof( fd ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    if ( path[0] ) {
        last = str_rchr( path, '\\' );
        str_cpyn( fd.name, last ? last + 1 : path, sizeof( fd.name ) );
    }
    fd.name_ctl = 1;
    fd.files_ctl = -1;
    fd.dirs_ctl = 3;
    put( &ctl[0], CT_LABEL, 2, 2, 0, "File &Name:" );
    put( &ctl[1], CT_EDIT, 2, 14, 25, NULL );
    ctl[1].buf = fd.name;
    ctl[1].max = PATH_MAX;
    put( &ctl[2], CT_TEXT, 4, 2, 0, fd.dirtext );
    put( &ctl[3], CT_LIST, 6, 13, 16, "&Dirs/Drives" );
    ctl[3].height = 9;
    ctl[3].item = dirs_item;
    button( &ctl[4], 16, 7, " OK ", ID_OK, 1 );
    button( &ctl[5], 16, 17, " Cancel ", ID_CANCEL, 0 );
    button( &ctl[6], 16, 31, " &Help ", ID_HELP, 0 );
    dlg.title = "Save As";
    dlg.top = 3;
    dlg.left = 19;
    dlg.rows = 18;
    dlg.cols = 42;
    dlg.sep = 15;
    dlg.ctl = ctl;
    dlg.nctl = 7;
    dlg.focus = 1;
    dlg.help = "d.saveas";
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
/* Print                                                               */
/* ------------------------------------------------------------------ */

static int print_what = 3;

static int print_lines( u32 handle, DOC *doc, POS from, POS to )
{
    u32 line, start, end, done;
    LINE *text;
    int rc = 0;

    for ( line = from.line; line <= to.line && line < doc->count && rc == 0; line++ ) {
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
    return rc;
}

void dlg_print( void )
{
    CTL ctl[6];
    DLG dlg;
    VIEW *view = app_view();
    POS from, to;
    u32 handle, done;
    int rc = 0, t;

    mem_set( &dlg, 0, sizeof( dlg ) );
    print_what = view_has_sel( view ) ? 1 : 3;
    put( &ctl[0], CT_RADIO, 2, 7, 0, "&Selected Text Only" );
    ctl[0].value = &print_what;
    ctl[0].id = 1;
    put( &ctl[1], CT_RADIO, 3, 7, 0, "&Current Window" );
    ctl[1].value = &print_what;
    ctl[1].id = 2;
    put( &ctl[2], CT_RADIO, 4, 7, 0, "&Entire Program" );
    ctl[2].value = &print_what;
    ctl[2].id = 3;
    button( &ctl[3], 7, 3, " OK ", ID_OK, 1 );
    button( &ctl[4], 7, 11, " Cancel ", ID_CANCEL, 0 );
    button( &ctl[5], 7, 23, " &Help ", ID_HELP, 0 );
    dlg.title = "Print";
    dlg.top = 7;
    dlg.left = 20;
    dlg.rows = 9;
    dlg.cols = 34;
    dlg.sep = 6;
    dlg.ctl = ctl;
    dlg.nctl = 6;
    dlg.focus = print_what - 1;
    dlg.help = "d.print";
    if ( dlg_run( &dlg ) != ID_OK ) {
        return;
    }
    rc = sys_open_write( "PRN", &handle );
    if ( rc != 0 ) {
        msg_box( "The printer could not be reached.", MB_OK, 1 );
        return;
    }
    if ( print_what == 1 && view_has_sel( view ) ) {
        view_sel_range( view, &from, &to );
        rc = print_lines( handle, view->doc, from, to );
    } else if ( print_what == 2 || print_what == 1 ) {
        from.line = 0;
        from.col = 0;
        to.line = view->doc->count - 1;
        to.col = doc_line( view->doc, to.line )->len;
        rc = print_lines( handle, view->doc, from, to );
    } else {
        for ( t = 0; t < ptext.ntexts && rc == 0; t++ ) {
            DOC *doc = ptext.texts[t];
            from.line = 0;
            from.col = 0;
            to.line = doc->count - 1;
            to.col = doc_line( doc, to.line )->len;
            rc = print_lines( handle, doc, from, to );
        }
    }
    if ( rc == 0 ) {
        rc = sys_write( handle, "\f", 1, &done );
    }
    sys_close( handle );
    if ( rc != 0 ) {
        msg_box( "The printer could not be written to.", MB_OK, 1 );
    }
}

/* ------------------------------------------------------------------ */
/* New SUB, New FUNCTION                                               */
/* ------------------------------------------------------------------ */

int dlg_newproc( int kind, char *name )
{
    static char buf[NAME_MAX + 2];
    CTL ctl[5];
    DLG dlg;
    char word[NAME_MAX + 2];

    mem_set( &dlg, 0, sizeof( dlg ) );
    view_word_at( app_view(), word, sizeof( word ) );
    str_cpy( buf, kw_lookup( word, str_len( word ) ) < 0 ? word : "" );
    put( &ctl[0], CT_LABEL, 2, 2, 0, "&Name:" );
    put( &ctl[1], CT_EDIT, 2, 9, 26, NULL );
    ctl[1].buf = buf;
    ctl[1].max = NAME_MAX + 1;
    button( &ctl[2], 6, 3, " OK ", ID_OK, 1 );
    button( &ctl[3], 6, 13, " Cancel ", ID_CANCEL, 0 );
    button( &ctl[4], 6, 27, " &Help ", ID_HELP, 0 );
    dlg.title = kind == PK_SUB ? "New SUB" : "New FUNCTION";
    dlg.top = 8;
    dlg.left = 21;
    dlg.rows = 8;
    dlg.cols = 38;
    dlg.sep = 5;
    dlg.ctl = ctl;
    dlg.nctl = 5;
    dlg.focus = 1;
    dlg.help = kind == PK_SUB ? "d.newsub" : "d.newfunc";
    if ( dlg_run( &dlg ) != ID_OK || buf[0] == 0 ) {
        return 0;
    }
    str_cpy( name, buf );
    return 1;
}

/* ------------------------------------------------------------------ */
/* SUBs                                                                */
/* ------------------------------------------------------------------ */

static char subs_info[PATH_MAX + 64];
static char subs_line[NAME_MAX + 8];

static const char *subs_item( DLG *dlg, int index )
{
    (void)dlg;
    if ( index == 0 ) {
        return ptext.name;
    }
    str_cpy( subs_line, "  " );
    str_catn( subs_line, ptext.texts[index]->pname, sizeof( subs_line ) );
    return subs_line;
}

static void subs_changed( DLG *dlg, int ctl )
{
    int sel = dlg->ctl[1].sel;

    (void)ctl;
    if ( sel == 0 ) {
        str_cpyt( subs_info, ptext.name, sizeof( subs_info ) );
        str_catn( subs_info, " is the program's main module", sizeof( subs_info ) );
    } else {
        str_cpyt( subs_info, ptext.texts[sel]->pname, sizeof( subs_info ) );
        str_catn( subs_info, ptext.texts[sel]->pkind == PK_SUB ? " is a SUB in " : " is a FUNCTION in ",
                  sizeof( subs_info ) );
        str_catn( subs_info, ptext.name, sizeof( subs_info ) );
    }
}

static int subs_activate( DLG *dlg, int ctl )
{
    int sel = dlg->ctl[1].sel;
    char text[120];

    if ( dlg->ctl[ctl].type == CT_LIST ) {
        return ID_OK;
    }
    if ( dlg->ctl[ctl].id == ID_NO ) {              /* Delete */
        if ( sel == 0 ) {
            msg_box( "The main module cannot be deleted.", MB_OK, 0 );
            return 0;
        }
        str_cpy( text, "Delete " );
        str_catn( text, ptext.texts[sel]->pname, sizeof( text ) );
        str_catn( text, " and all of its lines?", sizeof( text ) );
        if ( msg_box( text, MB_OKCANCEL, 0 ) == ID_OK ) {
            int index;
            for ( index = 0; index < 3; index++ ) {
                if ( views[index].text == sel ) {
                    view_show_text( &views[index], 0 );
                } else if ( views[index].text > sel ) {
                    views[index].text--;
                }
            }
            prog_delete_proc( sel );
            app_edited();
            dlg->ctl[1].count = ptext.ntexts;
            if ( dlg->ctl[1].sel >= ptext.ntexts ) {
                dlg->ctl[1].sel = ptext.ntexts - 1;
            }
            subs_changed( dlg, 1 );
        }
        return 0;
    }
    return 0;
}

int dlg_subs( void )
{
    CTL ctl[8];
    DLG dlg;
    int answer;

    mem_set( &dlg, 0, sizeof( dlg ) );
    put( &ctl[0], CT_TEXT, 2, 2, 0, "Choose the part of the program to edit" );
    put( &ctl[1], CT_LIST, 3, 2, 70, NULL );
    ctl[1].height = 11;
    ctl[1].columns = 3;
    ctl[1].count = ptext.ntexts;
    ctl[1].item = subs_item;
    put( &ctl[2], CT_TEXT, 14, 2, 0, subs_info );
    button( &ctl[3], 16, 6, " Edit in Active ", ID_OK, 1 );
    button( &ctl[4], 16, 29, " &Delete ", ID_NO, 0 );
    button( &ctl[5], 16, 45, " Cancel ", ID_CANCEL, 0 );
    button( &ctl[6], 16, 60, " &Help ", ID_HELP, 0 );
    dlg.title = "SUBs";
    dlg.top = 2;
    dlg.left = 3;
    dlg.rows = 18;
    dlg.cols = 74;
    dlg.sep = 15;
    dlg.ctl = ctl;
    dlg.nctl = 7;
    dlg.focus = 1;
    dlg.help = "d.subs";
    dlg.changed = subs_changed;
    dlg.activate = subs_activate;
    subs_changed( &dlg, 1 );
    answer = dlg_run( &dlg );
    return answer == ID_OK ? ctl[1].sel : -1;
}

/* ------------------------------------------------------------------ */
/* Find and Change                                                     */
/* ------------------------------------------------------------------ */

static char find_text[128], change_text[128];
static int  match_case, whole_word, have_find;

int find_can_repeat( void )
{
    return 1;
}

static int word_ch( int ch )
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
           || ch == '_' || ch == '.';
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

/* the next match in a text at or after "from"; 1 = found */
static int search_text( DOC *doc, POS from, POS *found )
{
    u32 len = str_len( find_text ), line, at;
    LINE *text;

    if ( len == 0 ) {
        return 0;
    }
    for ( line = from.line; line < doc->count; line++ ) {
        text = doc_line( doc, line );
        for ( at = line == from.line ? from.col : 0; at + len <= text->len; at++ ) {
            if ( match_at( text, at, len ) ) {
                found->line = line;
                found->col = at;
                return 1;
            }
        }
    }
    return 0;
}

/* QBasic looks through the whole program: on from the cursor in this
   text, then through the texts that follow it, and round again to the
   cursor.  -> the text it is in, or -1 */
static int search_program( int text, POS from, POS *found )
{
    int t, tries;
    POS top;

    top.line = 0;
    top.col = 0;
    if ( search_text( ptext.texts[text], from, found ) ) {
        return text;
    }
    for ( tries = 1, t = text; tries <= ptext.ntexts; tries++ ) {
        t = (t + 1) % ptext.ntexts;
        if ( t == text ) {
            if ( search_text( ptext.texts[t], top, found )
                 && (found->line < from.line || (found->line == from.line && found->col < from.col)) ) {
                return t;
            }
            return -1;
        }
        if ( search_text( ptext.texts[t], top, found ) ) {
            return t;
        }
    }
    return -1;
}

static void select_match( VIEW *view, int text, POS at )
{
    LINE *line;

    if ( view->text != text ) {
        view_show_text( view, text );
    }
    line = doc_line( view->doc, at.line );
    view->sel = 1;
    view->sel_line = at.line;
    view->sel_col = doc_col_of( view->doc, line, at.col );
    view->line = at.line;
    view->col = doc_col_of( view->doc, line, at.col + str_len( find_text ) );
    view->want = view->col;
    /* the match a few lines from the top, as QBasic shows it */
    if ( view->line < view->top || view->line >= view->top + (u32)view_text_rows( view ) ) {
        view->top = view->line > 3 ? view->line - 3 : 0;
    }
    view_keep_visible( view );
}

static int find_from_cursor( void )
{
    VIEW *view = app_view();
    POS from, found;
    LINE *line;
    int text;

    if ( view->text < 0 ) {
        return 0;
    }
    from = view_pos( view );
    line = doc_line( view->doc, from.line );
    if ( view_has_sel( view ) ) {
        from.col++;                             /* past the match that is lit */
    }
    if ( from.col > line->len ) {
        from.col = line->len;
    }
    text = search_program( view->text, from, &found );
    if ( text < 0 ) {
        msg_box( "Match not found", MB_OK, 1 );
        return 0;
    }
    select_match( view, text, found );
    return 1;
}

void find_again( void )
{
    if ( !have_find || find_text[0] == 0 ) {
        dlg_find( 0 );
        return;
    }
    find_from_cursor();
}

/* one match: Change, Skip or Cancel, in a box low on the screen */
static int ask_change( void )
{
    CTL ctl[5];
    DLG dlg;

    mem_set( &dlg, 0, sizeof( dlg ) );
    put( &ctl[0], CT_TEXT, 1, 1, 0, "" );
    button( &ctl[1], 3, 4, " &Change ", ID_YES, 1 );
    button( &ctl[2], 3, 16, " &Skip ", ID_NO, 0 );
    button( &ctl[3], 3, 28, " Cancel ", ID_CANCEL, 0 );
    button( &ctl[4], 3, 41, " &Help ", ID_HELP, 0 );
    dlg.title = "Change";
    dlg.top = scr_rows - 6;
    dlg.left = 13;
    dlg.rows = 5;
    dlg.cols = 53;
    dlg.sep = 2;
    dlg.ctl = ctl;
    dlg.nctl = 5;
    dlg.focus = 1;
    dlg.help = "d.verify";
    return dlg_run( &dlg );
}

static void change_all( int verify )
{
    VIEW *view = app_view();
    int text = view->text, start_text = view->text, wrapped = 0, changes = 0, answer;
    POS at = view_pos( view ), found, end, start = at;
    u32 find_len = str_len( find_text ), change_len = str_len( change_text );
    DOC *doc;

    if ( text < 0 ) {
        return;
    }
    if ( at.col > doc_line( view->doc, at.line )->len ) {
        at.col = doc_line( view->doc, at.line )->len;
        start = at;
    }
    for ( ;; ) {
        doc = ptext.texts[text];
        if ( !search_text( doc, at, &found )
             || (wrapped && text == start_text
                 && (found.line > start.line || (found.line == start.line && found.col >= start.col))) ) {
            /* this text done: the next one, and round to where it began */
            if ( wrapped && text == start_text ) {
                break;
            }
            text = (text + 1) % ptext.ntexts;
            at.line = 0;
            at.col = 0;
            if ( text == start_text ) {
                wrapped = 1;
            }
            continue;
        }
        if ( verify ) {
            select_match( view, text, found );
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
        if ( text == start_text && found.line == start.line && found.col < start.col ) {
            start.col = start.col + change_len - find_len;
        }
        doc->modified = 1;
        ptext.modified = 1;
        app_edited();
        changes++;
    }
    view->sel = 0;
    view_fixup( view->doc );
    msg_box( changes ? "Change complete" : "Match not found", MB_OK, 1 );
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

/* the word at the cursor, or the selection on one line, to offer */
static void offer_word( char *out, u32 max )
{
    VIEW *view = app_view();
    POS from, to;
    LINE *line;

    out[0] = 0;
    if ( view->text < 0 ) {
        return;
    }
    if ( view_has_sel( view ) ) {
        view_sel_range( view, &from, &to );
        line = doc_line( view->doc, from.line );
        if ( from.line == to.line && to.col - from.col < max ) {
            mem_cpy( out, line->text + from.col, to.col - from.col );
            out[to.col - from.col] = 0;
        }
        return;
    }
    view_word_at( view, out, max );
}

int dlg_find( int change )
{
    CTL ctl[11];
    DLG dlg;
    int answer, count = 0;
    char offer[128];

    mem_set( &dlg, 0, sizeof( dlg ) );
    offer_word( offer, sizeof( offer ) );
    if ( offer[0] ) {
        str_cpy( find_text, offer );
    }
    put( &ctl[count++], CT_LABEL, 2, 2, 0, "Find &What:" );
    put( &ctl[count], CT_EDIT, 2, 14, change ? 41 : 42, NULL );
    ctl[count].buf = find_text;
    ctl[count++].max = sizeof( find_text );
    if ( change ) {
        put( &ctl[count++], CT_LABEL, 5, 2, 0, "&Change To:" );
        put( &ctl[count], CT_EDIT, 5, 14, 41, NULL );
        ctl[count].buf = change_text;
        ctl[count++].max = sizeof( change_text );
        put( &ctl[count], CT_CHECK, 9, 5, 0, "&Match Upper/Lowercase" );
        ctl[count++].value = &match_case;
        put( &ctl[count], CT_CHECK, 9, 37, 0, "&Whole Word" );
        ctl[count++].value = &whole_word;
        button( &ctl[count++], 12, 2, " Find and &Verify ", ID_YES, 1 );
        button( &ctl[count++], 12, 22, " Change &All ", ID_NO, 0 );
        button( &ctl[count++], 12, 37, " Cancel ", ID_CANCEL, 0 );
        button( &ctl[count++], 12, 48, " &Help ", ID_HELP, 0 );
        dlg.title = "Change";
        dlg.top = 5;
        dlg.left = 11;
        dlg.rows = 14;
        dlg.cols = 58;
        dlg.sep = 11;
        dlg.help = "d.change";
    } else {
        put( &ctl[count], CT_CHECK, 6, 5, 0, "&Match Upper/Lowercase" );
        ctl[count++].value = &match_case;
        put( &ctl[count], CT_CHECK, 6, 38, 0, "&Whole Word" );
        ctl[count++].value = &whole_word;
        button( &ctl[count++], 9, 9, " OK ", ID_OK, 1 );
        button( &ctl[count++], 9, 24, " Cancel ", ID_CANCEL, 0 );
        button( &ctl[count++], 9, 42, " &Help ", ID_HELP, 0 );
        dlg.title = "Find";
        dlg.top = 7;
        dlg.left = 10;
        dlg.rows = 11;
        dlg.cols = 59;
        dlg.sep = 8;
        dlg.help = "d.find";
    }
    dlg.ctl = ctl;
    dlg.nctl = count;
    dlg.focus = 1;
    dlg.activate = find_press;
    answer = dlg_run( &dlg );
    if ( answer == ID_CANCEL || find_text[0] == 0 ) {
        return 0;
    }
    have_find = 1;
    if ( !change ) {
        find_from_cursor();
        return 1;
    }
    change_all( answer == ID_YES );
    return 1;
}

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

static const char *colour_names[16] = {
    "Black", "Blue", "Green", "Cyan", "Red", "Magenta", "Brown", "White",
    "Gray", "BrBlue", "BrGreen", "BrCyan", "BrRed", "Pink", "Yellow", "BrWhite"
};

static const char *colour_item( DLG *dlg, int index )
{
    (void)dlg;
    return colour_names[index & 15];
}

static int  disp_part = 1;
static u8   disp_attr[4];           /* [1] normal, [2] current, [3] breakpoint */

static void display_pick( DLG *dlg, int ctl )
{
    if ( ctl >= 2 && ctl <= 4 ) {           /* another part: its colours in the lists */
        dlg->ctl[8].sel = disp_attr[disp_part] & 0x0F;
        dlg->ctl[9].sel = (disp_attr[disp_part] >> 4) & 0x07;
    } else if ( ctl == 8 || ctl == 9 ) {    /* a colour chosen: the sample shows it */
        disp_attr[disp_part] = (u8)((dlg->ctl[9].sel << 4) | dlg->ctl[8].sel);
    }
    dlg->ctl[5].attr = disp_attr[1];
    dlg->ctl[6].attr = disp_attr[2];
    dlg->ctl[7].attr = disp_attr[3];
}

void dlg_display( void )
{
    CTL ctl[17];
    DLG dlg;
    char tabs[8];
    int bars = scroll_bars, ok;
    u32 value;

    mem_set( &dlg, 0, sizeof( dlg ) );
    disp_attr[1] = pal.text;
    disp_attr[2] = pal.current;
    disp_attr[3] = pal.bkpt;
    disp_part = 1;
    num_fmt_l( tab_size, tabs );
    put( &ctl[0], CT_FRAME, 1, 2, 56, "Colors" );
    ctl[0].height = 14;
    put( &ctl[1], CT_FRAME, 16, 2, 56, "Display Options" );
    ctl[1].height = 3;
    put( &ctl[2], CT_RADIO, 4, 4, 0, "&1." );
    ctl[2].value = &disp_part;
    ctl[2].id = 1;
    put( &ctl[3], CT_RADIO, 6, 4, 0, "&2." );
    ctl[3].value = &disp_part;
    ctl[3].id = 2;
    put( &ctl[4], CT_RADIO, 8, 4, 0, "&3." );
    ctl[4].value = &disp_part;
    ctl[4].id = 3;
    put( &ctl[5], CT_SAMPLE, 4, 11, 0, " Normal Text       " );
    put( &ctl[6], CT_SAMPLE, 6, 11, 0, " Current Statement " );
    put( &ctl[7], CT_SAMPLE, 8, 11, 0, " Breakpoint Lines  " );
    put( &ctl[8], CT_LIST, 3, 31, 11, "&Foreground" );
    ctl[8].height = 10;
    ctl[8].count = 16;
    ctl[8].item = colour_item;
    put( &ctl[9], CT_LIST, 3, 44, 11, "&Background" );
    ctl[9].height = 10;
    ctl[9].count = 8;
    ctl[9].item = colour_item;
    put( &ctl[10], CT_CHECK, 17, 6, 0, "&Scroll Bars" );
    ctl[10].value = &bars;
    put( &ctl[11], CT_LABEL, 17, 39, 0, "&Tab Stops:" );
    put( &ctl[12], CT_EDIT, 17, 50, 3, NULL );
    ctl[12].height = 1;
    ctl[12].buf = tabs;
    ctl[12].max = 3;
    button( &ctl[13], 20, 10, " OK ", ID_OK, 1 );
    button( &ctl[14], 20, 24, " Cancel ", ID_CANCEL, 0 );
    button( &ctl[15], 20, 42, " &Help ", ID_HELP, 0 );
    dlg.title = "Display";
    dlg.top = 1;
    dlg.left = 10;
    dlg.rows = 22;
    dlg.cols = 60;
    dlg.sep = 19;
    dlg.ctl = ctl;
    dlg.nctl = 16;
    dlg.focus = 2;
    dlg.help = "d.display";
    dlg.changed = display_pick;
    display_pick( &dlg, 2 );
    for ( ;; ) {
        if ( dlg_run( &dlg ) != ID_OK ) {
            return;
        }
        value = dec_parse( tabs, &ok );
        if ( ok && value >= 1 && value <= 99 ) {
            break;
        }
        msg_box( "Tab stops go from 1 to 99.", MB_OK, 0 );
        dlg.focus = 12;
    }
    tab_size = (int)value;
    scroll_bars = bars;
    pal.text = disp_attr[1];
    pal.frame = disp_attr[1];
    pal.current = disp_attr[2];
    pal.bkpt = disp_attr[3];
    pal.bkpt_current = (u8)((disp_attr[3] & 0xF0) | (disp_attr[2] & 0x0F));
    pal.select = (u8)(((disp_attr[1] & 0x07) << 4) | ((disp_attr[1] >> 4) & 0x07));
    pal.title = pal.select;
    app_layout();
}

/* ------------------------------------------------------------------ */
/* Help Path, About, and the welcome                                   */
/* ------------------------------------------------------------------ */

void dlg_helppath( void )
{
    CTL ctl[5];
    DLG dlg;
    char path[PATH_MAX];

    mem_set( &dlg, 0, sizeof( dlg ) );
    str_cpy( path, help_path );
    put( &ctl[0], CT_TEXT, 2, 1, 53, "The directory QBASIC.HLP is in, if QBasic cannot find it:" );
    put( &ctl[1], CT_EDIT, 5, 3, 49, NULL );
    ctl[1].buf = path;
    ctl[1].max = PATH_MAX;
    button( &ctl[2], 8, 10, " OK ", ID_OK, 1 );
    button( &ctl[3], 8, 23, " Cancel ", ID_CANCEL, 0 );
    button( &ctl[4], 8, 40, " &Help ", ID_HELP, 0 );
    dlg.title = "Help Path";
    dlg.top = 7;
    dlg.left = 12;
    dlg.rows = 10;
    dlg.cols = 55;
    dlg.sep = 7;
    dlg.ctl = ctl;
    dlg.nctl = 5;
    dlg.focus = 1;
    dlg.help = "d.helppath";
    if ( dlg_run( &dlg ) == ID_OK ) {
        str_cpy( help_path, path );
        help_init();
    }
}

void dlg_about( void )
{
    CTL ctl[2];
    DLG dlg;

    mem_set( &dlg, 0, sizeof( dlg ) );
    put( &ctl[0], CT_TEXT, 2, 1, 51, "QBasic for PM-DOS\nVersion 1.0\nA BASIC interpreter, part of PM-DOS" );
    button( &ctl[1], 7, 22, "  OK  ", ID_OK, 1 );
    dlg.top = 8;
    dlg.left = 13;
    dlg.rows = 9;
    dlg.cols = 53;
    dlg.sep = 6;
    dlg.ctl = ctl;
    dlg.nctl = 2;
    dlg.focus = 1;
    dlg.help = "d.about";
    dlg_run( &dlg );
}

/* the box at the start: Enter for the Survival Guide, Esc to begin */
int dlg_welcome( void )
{
    static const char enter[] = " Press Enter to read Getting Started ";
    static const char esc[] = " Press Esc to start a program ";
    CTL ctl[3];
    DLG dlg;

    mem_set( &dlg, 0, sizeof( dlg ) );
    put( &ctl[0], CT_TEXT, 2, 1, 56, "Welcome to QBasic for PM-DOS\n\nA BASIC interpreter, part of PM-DOS" );
    button( &ctl[1], 7, (58 - (int)sizeof( enter ) - 1) / 2, enter, ID_OK, 1 );
    button( &ctl[2], 9, (58 - (int)sizeof( esc ) - 1) / 2, esc, ID_CANCEL, 0 );
    dlg.top = 5;
    dlg.left = 12;
    dlg.rows = 11;
    dlg.cols = 58;
    dlg.sep = 8;
    dlg.ctl = ctl;
    dlg.nctl = 3;
    dlg.focus = 1;
    dlg.help = "survival";
    return dlg_run( &dlg ) == ID_OK;
}
