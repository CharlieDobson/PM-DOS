/*
 * DLGS.C - the colour schemes, and the dialogs that are not about one
 * file operation: Confirmation, File Display Options, Display, Colors,
 * Show Information, Search, Run, Associate and About.
 *
 * A COLOUR SCHEME IS TWO PALETTES, one for a text screen and one for a
 * graphics screen, because the two are not the same kind of thing: a
 * text screen has eight background colours and marks an access key by
 * its colour; a graphics screen has sixteen, so the lists can be black
 * on white, and underlines the key instead.  Most of the schemes are
 * one design with a different strong colour in it - the blue of the
 * first is taken out and green, cyan, red or magenta put in.  The last
 * scheme is black and white, for a screen that shows colours as greys.
 */
#include "shell.h"

/* ------------------------------------------------------------------ */
/* the schemes                                                         */
/* ------------------------------------------------------------------ */

/* in PALETTE's order: desk title | menu hot sel sel_hot off | pane
   pane_title pane_focus select select_off drive_sel | scroll thumb |
   status hot | dlg hot title field field_sel button focus shadow */
static const PALETTE basic_text = {
    0x70, 0x1F,  0x70, 0x7F, 0x1F, 0x1E, 0x78,
    0x70, 0x30, 0x1F, 0x1F, 0x30, 0x1F,  0x70, 0x71,
    0x70, 0x74,  0x70, 0x7F, 0x1F, 0x07, 0x70, 0x70, 0x1F, 0x08
};
static const PALETTE basic_gfx = {
    0xF0, 0x1F,  0xF0, 0xF1, 0x1F, 0x1E, 0xF7,
    0xF0, 0x70, 0x1F, 0x1F, 0x70, 0x1F,  0x70, 0xF0,
    0x70, 0x71,  0xF0, 0xF1, 0x1F, 0xF0, 0x1F, 0x70, 0x1F, 0x87
};
static const PALETTE ocean_text = {
    0x30, 0x1F,  0x30, 0x3F, 0x1F, 0x1E, 0x38,
    0x17, 0x30, 0x70, 0x71, 0x30, 0x1F,  0x31, 0x13,
    0x30, 0x3F,  0x30, 0x3F, 0x1F, 0x1F, 0x71, 0x30, 0x1F, 0x08
};
static const PALETTE ocean_gfx = {
    0x30, 0x1F,  0x30, 0x3F, 0x1F, 0x1E, 0x38,
    0x1F, 0x30, 0x70, 0x71, 0x30, 0x1F,  0x30, 0xB0,
    0x30, 0x3F,  0x30, 0x3F, 0x1F, 0xF0, 0x1F, 0x70, 0x1F, 0x87
};
static const PALETTE reverse_text = {
    0x07, 0x3F,  0x07, 0x0F, 0x30, 0x3F, 0x08,
    0x07, 0x70, 0x30, 0x30, 0x70, 0x30,  0x07, 0x03,
    0x07, 0x0F,  0x07, 0x0F, 0x30, 0x70, 0x07, 0x07, 0x30, 0x08
};
static const PALETTE reverse_gfx = {
    0x07, 0x3F,  0x07, 0x0F, 0x30, 0x3F, 0x08,
    0x07, 0x70, 0x30, 0x30, 0x70, 0x30,  0x07, 0x70,
    0x07, 0x0F,  0x07, 0x0F, 0x30, 0x70, 0x07, 0x07, 0x30, 0x08
};
static const PALETTE mono_text = {
    0x07, 0x70,  0x70, 0x7F, 0x07, 0x0F, 0x70,
    0x07, 0x70, 0x7F, 0x70, 0x0F, 0x70,  0x07, 0x0F,
    0x70, 0x7F,  0x70, 0x7F, 0x07, 0x07, 0x70, 0x70, 0x07, 0x07
};
static const PALETTE mono_gfx = {
    0xF0, 0x0F,  0xF0, 0xF8, 0x0F, 0x07, 0xF7,
    0xF0, 0xF0, 0x0F, 0x0F, 0x70, 0x0F,  0xF0, 0x0F,
    0xF0, 0xF8,  0xF0, 0xF8, 0x0F, 0xF0, 0x0F, 0xF0, 0x0F, 0x87
};

/* "strong" is the colour that stands where the first scheme has blue;
   0 for a scheme that is its own two tables */
static const struct {
    const char    *name;
    const PALETTE *text, *gfx;
    int            strong;
} schemes[] = {
    { "Basic Blue", &basic_text,   &basic_gfx,   0 },
    { "Ocean",      &ocean_text,   &ocean_gfx,   0 },
    { "Emerald",    &basic_text,   &basic_gfx,   2 },
    { "Turquoise",  &basic_text,   &basic_gfx,   3 },
    { "Ruby",       &basic_text,   &basic_gfx,   4 },
    { "Violet",     &basic_text,   &basic_gfx,   5 },
    { "Reverse",    &reverse_text, &reverse_gfx, 0 },
    { "Monochrome", &mono_text,    &mono_gfx,    0 }
};
#define NSCHEMES    (int)(sizeof( schemes ) / sizeof( schemes[0] ))

int scheme_count( void )
{
    return NSCHEMES;
}

const char *scheme_name( int index )
{
    return schemes[index < 0 || index >= NSCHEMES ? 0 : index].name;
}

void scheme_fill( int index, int graphics, PALETTE *out )
{
    u8 *attr = (u8 *)out;
    u32 at;
    int strong;

    if ( index < 0 || index >= NSCHEMES ) {
        index = 0;
    }
    *out = graphics ? *schemes[index].gfx : *schemes[index].text;
    strong = schemes[index].strong;
    for ( at = 0; strong && at < sizeof( PALETTE ); at++ ) {
        if ( (attr[at] & 0xF0) == 0x10 ) {
            attr[at] = (u8)((attr[at] & 0x0F) | (strong << 4));
        }
        if ( (attr[at] & 0x0F) == 0x01 ) {
            attr[at] = (u8)((attr[at] & 0xF0) | strong);
        }
    }
}

/* ------------------------------------------------------------------ */
/* one field and a question                                            */
/* ------------------------------------------------------------------ */

/* 1 = OK, and "buf" is what was typed */
int input_box( const char *title, const char *info, const char *label,
               char *buf, int max, int width, int help, int hidden )
{
    CTL ctl[6];
    DLG dlg;
    int cols, at = 3 + scr_hotlen( label ) + 2;

    cols = at + width + 4;
    if ( cols < (int)str_len( info ) + 6 ) {
        cols = (int)str_len( info ) + 6;
    }
    if ( cols < 46 ) {
        cols = 46;
    }
    if ( cols > scr_cols - 2 ) {
        cols = scr_cols - 2;
    }
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_text( &ctl[0], 2, 3, 0, info );
    ctl_label( &ctl[1], 4, 3, label );
    ctl_edit( &ctl[2], 4, at, width, buf, max );
    ctl[2].hidden = hidden;
    ctl_button( &ctl[3], 6, cols / 2 - 17, "OK", ID_OK, 1 );
    ctl_button( &ctl[4], 6, cols / 2 - 6, "Cancel", ID_CANCEL, 0 );
    ctl_button( &ctl[5], 6, cols / 2 + 9, "&Help", ID_HELP, 0 );
    dlg.title = title;
    dlg.rows = 8;
    dlg.cols = cols;
    dlg.ctl = ctl;
    dlg.nctl = 6;
    dlg.focus = 2;
    dlg.help = help;
    return dlg_run( &dlg ) == ID_OK;
}

/* OK, Cancel and Help along the bottom of a box */
static int std_buttons( CTL *ctl, int row, int cols )
{
    ctl_button( &ctl[0], row, cols / 2 - 17, "OK", ID_OK, 1 );
    ctl_button( &ctl[1], row, cols / 2 - 6, "Cancel", ID_CANCEL, 0 );
    ctl_button( &ctl[2], row, cols / 2 + 9, "&Help", ID_HELP, 0 );
    return 3;
}

/* ------------------------------------------------------------------ */
/* Options: Confirmation, File Display Options                         */
/* ------------------------------------------------------------------ */

void dlg_confirmation( void )
{
    CTL ctl[6];
    DLG dlg;
    int on_delete = opt.confirm_delete, on_replace = opt.confirm_replace;
    int on_mouse = opt.confirm_mouse;

    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_check( &ctl[0], 2, 4, "Confirm on &Delete", &on_delete );
    ctl_check( &ctl[1], 3, 4, "Confirm on &Replace", &on_replace );
    ctl_check( &ctl[2], 4, 4, "Confirm on &Mouse Operation", &on_mouse );
    std_buttons( &ctl[3], 6, 42 );
    dlg.title = "Confirmation";
    dlg.rows = 8;
    dlg.cols = 42;
    dlg.ctl = ctl;
    dlg.nctl = 6;
    dlg.help = HELP_COMMANDS;
    if ( dlg_run( &dlg ) == ID_OK ) {
        opt.confirm_delete = on_delete;
        opt.confirm_replace = on_replace;
        opt.confirm_mouse = on_mouse;
    }
}

void dlg_file_options( void )
{
    static char filter[64];
    CTL ctl[14];
    DLG dlg;
    int hidden = opt.show_hidden, desc = opt.sort_desc, key = opt.sort_key;

    str_fit( filter, opt.filter, sizeof( filter ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_label( &ctl[0], 2, 3, "&Name:" );
    ctl_edit( &ctl[1], 2, 10, 24, filter, sizeof( filter ) );
    ctl_check( &ctl[2], 4, 3, "Display &hidden/system files", &hidden );
    ctl_check( &ctl[3], 5, 3, "&Descending order", &desc );
    ctl_text( &ctl[4], 2, 40, 0, "Sort by:" );
    ctl_radio( &ctl[5], 3, 41, "N&ame", &key, SORT_NAME );
    ctl_radio( &ctl[6], 4, 41, "&Extension", &key, SORT_EXT );
    ctl_radio( &ctl[7], 5, 41, "Da&te", &key, SORT_DATE );
    ctl_radio( &ctl[8], 6, 41, "&Size", &key, SORT_SIZE );
    ctl_radio( &ctl[9], 7, 41, "D&isk order", &key, SORT_DISK );
    std_buttons( &ctl[10], 9, 60 );
    dlg.title = "File Display Options";
    dlg.rows = 11;
    dlg.cols = 60;
    dlg.ctl = ctl;
    dlg.nctl = 13;
    dlg.focus = 1;
    dlg.help = HELP_COMMANDS;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return;
    }
    str_fit( opt.filter, filter[0] ? filter : "*.*", sizeof( opt.filter ) );
    opt.sort_desc = desc;
    opt.sort_key = key;
    wins_stale_all();
    if ( hidden != opt.show_hidden ) {
        /* hidden directories come and go with hidden files */
        opt.show_hidden = hidden;
        wins_reread( wins[0].drive );
        if ( nwins > 1 && wins[1].drive != wins[0].drive ) {
            wins_reread( wins[1].drive );
        }
    }
}

/* ------------------------------------------------------------------ */
/* Options: Display, Colors                                            */
/*                                                                     */
/* Both are a list with a Preview button: the screen changes behind    */
/* the box, and Cancel puts back what was there before.                */
/* ------------------------------------------------------------------ */

#define ID_PREVIEW      ID_USER

static int mode_list[NDMODES], nmode_list;

static const char *mode_item( DLG *dlg, int index )
{
    static char text[64];
    const DMODE *mode = &dmodes[mode_list[index]];

    (void)dlg;
    str_cpy( text, mode->kind_name );
    while ( str_len( text ) < 10 ) {
        str_catn( text, " ", sizeof( text ) );
    }
    str_catn( text, mode->res_name, sizeof( text ) );
    /* the 8x8 font makes 43 lines of an EGA's 350 */
    if ( mode->kind == VK_TEXT && mode->rows == 50 && gfx_adapter == VK_EGA ) {
        text[10] = '4';
        text[11] = '3';
    }
    return text;
}

static void show_mode( DLG *dlg, int index )
{
    if ( index == scr_mode ) {
        return;
    }
    if ( !scr_set_mode( index ) ) {
        msg_box( "The display could not be put in that mode.", MB_OK );
    }
    app_mode_changed();
    dlg->top = (scr_rows - dlg->rows) / 2;
    dlg->left = (scr_cols - dlg->cols) / 2;
}

static int display_activate( DLG *dlg, int ctl )
{
    if ( dlg->ctl[ctl].type == CT_LIST ) {
        return ID_OK;
    }
    if ( dlg->ctl[ctl].id == ID_PREVIEW ) {
        show_mode( dlg, mode_list[dlg->ctl[0].sel] );
    }
    return 0;
}

void dlg_display( void )
{
    CTL ctl[5];
    DLG dlg;
    int index, was = scr_mode, answer;

    nmode_list = 0;
    mem_set( &dlg, 0, sizeof( dlg ) );
    mem_set( ctl, 0, sizeof( ctl ) );
    for ( index = 0; index < NDMODES; index++ ) {
        if ( scr_mode_ok( index ) ) {
            if ( index == scr_mode ) {
                ctl[0].sel = nmode_list;
            }
            mode_list[nmode_list++] = index;
        }
    }
    ctl[0].type = CT_LIST;
    ctl[0].row = 2;
    ctl[0].col = 3;
    ctl[0].width = 50;
    ctl[0].height = nmode_list + 2 > 12 ? 12 : nmode_list + 2;
    ctl[0].text = "Screen &modes";
    ctl[0].count = nmode_list;
    ctl[0].item = mode_item;
    index = ctl[0].height + 3;
    ctl_button( &ctl[1], index, 4, "OK", ID_OK, 1 );
    ctl_button( &ctl[2], index, 13, "&Preview", ID_PREVIEW, 0 );
    ctl_button( &ctl[3], index, 27, "Cancel", ID_CANCEL, 0 );
    ctl_button( &ctl[4], index, 40, "&Help", ID_HELP, 0 );
    dlg.title = "Screen Display Mode";
    dlg.rows = index + 2;
    dlg.cols = 56;
    dlg.ctl = ctl;
    dlg.nctl = 5;
    dlg.help = HELP_DISPLAY;
    dlg.activate = display_activate;
    answer = dlg_run( &dlg );
    if ( answer == ID_OK ) {
        show_mode( &dlg, mode_list[ctl[0].sel] );
    } else {
        show_mode( &dlg, was );
    }
    opt.display = scr_mode;
}

static const char *scheme_item( DLG *dlg, int index )
{
    (void)dlg;
    return scheme_name( index );
}

static int colors_activate( DLG *dlg, int ctl )
{
    if ( dlg->ctl[ctl].type == CT_LIST ) {
        return ID_OK;
    }
    if ( dlg->ctl[ctl].id == ID_PREVIEW ) {
        scr_palette( dlg->ctl[0].sel );
    }
    return 0;
}

void dlg_colors( void )
{
    CTL ctl[5];
    DLG dlg;

    mem_set( &dlg, 0, sizeof( dlg ) );
    mem_set( ctl, 0, sizeof( ctl ) );
    ctl[0].type = CT_LIST;
    ctl[0].row = 2;
    ctl[0].col = 3;
    ctl[0].width = 30;
    ctl[0].height = NSCHEMES + 2;
    ctl[0].text = "&Schemes";
    ctl[0].count = NSCHEMES;
    ctl[0].item = scheme_item;
    ctl[0].sel = opt.scheme;
    ctl_button( &ctl[1], 3, 37, "OK", ID_OK, 1 );
    ctl_button( &ctl[2], 5, 37, "&Preview", ID_PREVIEW, 0 );
    ctl_button( &ctl[3], 7, 37, "Cancel", ID_CANCEL, 0 );
    ctl_button( &ctl[4], 9, 37, "&Help", ID_HELP, 0 );
    dlg.title = "Color Scheme";
    dlg.rows = NSCHEMES + 5;
    dlg.cols = 52;
    dlg.ctl = ctl;
    dlg.nctl = 5;
    dlg.help = HELP_DISPLAY;
    dlg.activate = colors_activate;
    if ( dlg_run( &dlg ) == ID_OK ) {
        opt.scheme = ctl[0].sel;
    }
    scr_palette( opt.scheme );           /* Cancel: what was there before a preview */
}

/* ------------------------------------------------------------------ */
/* Options: Show Information                                           */
/* ------------------------------------------------------------------ */

static void info_pair( char *buf, u32 max, const char *label, const char *value )
{
    str_fit( buf, label, max );
    str_catn( buf, value, max );
}

/* Line "line" of what there is to say about the file the keys are on,
   the selection, its directory and the disk.  0 past the last line. */
int info_line( FWIN *win, int line, char *buf, u32 max )
{
    char num[24], attrs[8];
    FENT *ent = win->files.count ? &win->files.ent[win->cur] : NULL;
    BIGSIZE sum;
    int index, count = 0, dir;

    buf[0] = 0;
    if ( !win_ok( win ) ) {
        return 0;
    }
    switch ( line ) {
    case 0:
        str_fit( buf, "File", max );
        return 1;
    case 1:
        info_pair( buf, max, "  Name   : ", ent ? ent->name : "" );
        return 1;
    case 2:
        attrs[0] = ent && (ent->attr & ATTR_READONLY) ? 'r' : '.';
        attrs[1] = ent && (ent->attr & ATTR_HIDDEN) ? 'h' : '.';
        attrs[2] = ent && (ent->attr & ATTR_SYSTEM) ? 's' : '.';
        attrs[3] = ent && (ent->attr & ATTR_ARCHIVE) ? 'a' : '.';
        attrs[4] = 0;
        info_pair( buf, max, "  Attr   : ", ent ? attrs : "" );
        return 1;
    case 3:
        str_fit( buf, "Selected", max );
        return 1;
    case 4:
        info_pair( buf, max, "  Number : ",
                   fmt_num( (u32)(win->files.nsel + kept_count()), 0, num ) );
        return 1;
    case 5:
        info_pair( buf, max, "  Size   : ", big_fmt( &win->files.selected, num ) );
        return 1;
    case 6:
        str_fit( buf, "Directory", max );
        return 1;
    case 7:
        dir = ent ? ent->dir : win->node;
        info_pair( buf, max, "  Name   : ", dir ? win->tree->node[dir].name : "\\" );
        return 1;
    case 8:
    case 9:
        /* the directory of the file the keys are on: in All Files and in
           a search's answer that is only some of the list */
        dir = ent ? ent->dir : win->node;
        sum.kb = sum.rest = 0;
        for ( index = 0; index < win->files.count; index++ ) {
            if ( win->files.ent[index].dir == dir ) {
                big_add( &sum, win->files.ent[index].size );
                count++;
            }
        }
        if ( line == 8 ) {
            info_pair( buf, max, "  Size   : ", big_fmt( &sum, num ) );
        } else {
            info_pair( buf, max, "  Files  : ", fmt_num( (u32)count, 0, num ) );
        }
        return 1;
    case 10:
        str_fit( buf, "Disk", max );
        return 1;
    case 11:
        info_pair( buf, max, "  Name   : ", win->tree->label[0] ? win->tree->label : "none" );
        return 1;
    case 12:
        sum.kb = win->tree->total_kb;
        sum.rest = 0;
        info_pair( buf, max, "  Size   : ", big_fmt( &sum, num ) );
        return 1;
    case 13:
        sum.kb = win->tree->free_kb;
        sum.rest = 0;
        info_pair( buf, max, "  Avail  : ", big_fmt( &sum, num ) );
        return 1;
    case 14:
        info_pair( buf, max, "  Dirs   : ", fmt_num( (u32)win->tree->count - 1, 0, num ) );
        return 1;
    }
    return 0;
}

void dlg_show_info( void )
{
    static char text[16 * 64];
    char line[64];
    int index;

    text[0] = 0;
    for ( index = 0; info_line( app_win(), index, line, sizeof( line ) ); index++ ) {
        if ( index ) {
            str_catn( text, "\n", sizeof( text ) );
        }
        while ( str_len( line ) < 40 ) {        /* one width, so the box sets them flush left */
            str_catn( line, " ", sizeof( line ) );
        }
        str_catn( text, line, sizeof( text ) );
    }
    if ( text[0] == 0 ) {
        str_cpy( text, "There is no drive to describe." );
    }
    msg_text( "Show Information", text, MB_OK, HELP_COMMANDS );
}

/* ------------------------------------------------------------------ */
/* File: Search, Run, Associate                                        */
/* ------------------------------------------------------------------ */

void dlg_search( void )
{
    static char pattern[64] = "*.*";
    FWIN *win = app_win();
    CTL ctl[7];
    DLG dlg;
    char info[PATH_MAX + 24];
    int whole = 1;

    if ( !win_ok( win ) ) {
        return;
    }
    str_cpy( info, "Current directory is " );
    tree_path( win->tree, win->node, info + str_len( info ) );
    if ( str_len( info ) > 60 ) {
        info[60] = 0;
    }
    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_text( &ctl[0], 2, 3, 0, info );
    ctl_label( &ctl[1], 4, 3, "&Search for:" );
    ctl_edit( &ctl[2], 4, 16, 30, pattern, sizeof( pattern ) );
    ctl_check( &ctl[3], 6, 3, "Search &entire disk", &whole );
    std_buttons( &ctl[4], 8, 68 );
    dlg.title = "Search File";
    dlg.rows = 10;
    dlg.cols = 68;
    dlg.ctl = ctl;
    dlg.nctl = 7;
    dlg.focus = 2;
    dlg.help = HELP_COMMANDS;
    if ( dlg_run( &dlg ) != ID_OK || pattern[0] == 0 ) {
        return;
    }
    str_fit( win->search_for, pattern, sizeof( win->search_for ) );
    win->search = whole ? 2 : 1;
    win->stale = 1;
    win->cur = win->top = win->anchor = 0;
    focus_area = AREA_FILES;
}

void dlg_run_line( void )
{
    static char line[128];

    if ( input_box( "Run", "Type the command as you would at the prompt.", "&Command line:",
                    line, sizeof( line ), 40, HELP_COMMANDS, 0 ) && line[0] ) {
        run_command( line, NULL, 1 );
    }
}

/* Associate, on a program: the extensions tied to it, typed with
   blanks between them.  On any other file: the program its extension
   is tied to. */
void dlg_associate( void )
{
    FWIN *win = app_win();
    char path[PATH_MAX], info[PATH_MAX + 40], field[128], ext[12];
    const char *found, *cur;
    int index, len;

    if ( win->files.count == 0 ) {
        return;
    }
    flist_path( win->tree, &win->files.ent[win->cur], path );
    str_fit( ext, path_ext( path ), sizeof( ext ) );
    str_upper( ext );
    field[0] = 0;
    if ( str_cmp( ext, "EXE" ) != 0 && str_cmp( ext, "COM" ) != 0 && str_cmp( ext, "BAT" ) != 0
         && str_cmp( ext, "CMD" ) != 0 ) {
        if ( ext[0] == 0 ) {
            msg_box( "A file with no extension cannot be tied to a program.", MB_OK );
            return;
        }
        found = assoc_program( ext );
        str_fit( field, found ? found : "", sizeof( field ) );
        str_cpy( info, "Files ending in ." );
        str_catn( info, ext, sizeof( info ) );
        str_catn( info, " are opened with:", sizeof( info ) );
        if ( input_box( "Associate File", info, "&Program:", field, 80, 40, HELP_COMMANDS, 0 ) ) {
            assoc_set( ext, field );
        }
        return;
    }
    for ( index = 0; index < assoc_count(); index++ ) {
        if ( str_icmp( assoc_prog_at( index ), path ) == 0 ) {
            if ( field[0] ) {
                str_catn( field, " ", sizeof( field ) );
            }
            str_catn( field, assoc_ext( index ), sizeof( field ) );
        }
    }
    str_cpy( info, "Extensions opened with " );
    str_catn( info, path_name( path ), sizeof( info ) );
    str_catn( info, ":", sizeof( info ) );
    if ( !input_box( "Associate File", info, "&Extensions:", field, sizeof( field ), 40,
                     HELP_COMMANDS, 0 ) ) {
        return;
    }
    /* the ones that were this program's and are not in the list any
       more are untied; the ones in the list are tied */
    for ( index = assoc_count() - 1; index >= 0; index-- ) {
        if ( str_icmp( assoc_prog_at( index ), path ) == 0 ) {
            assoc_set( assoc_ext( index ), "" );
        }
    }
    for ( cur = field; *cur; cur += len ) {
        while ( *cur == ' ' || *cur == '.' || *cur == ',' ) {
            cur++;
        }
        for ( len = 0; cur[len] && cur[len] != ' ' && cur[len] != ','; len++ ) {
        }
        if ( len && len < (int)sizeof( ext ) ) {
            mem_cpy( ext, cur, (u32)len );
            ext[len] = 0;
            assoc_set( ext, path );
        }
    }
}

/* ------------------------------------------------------------------ */
/* Help: About Shell                                                   */
/* ------------------------------------------------------------------ */

void dlg_about( void )
{
    char text[200], num[12];
    int major, minor;

    sys_version( &major, &minor );
    str_cpy( text, "PM-DOS Shell\n\nFiles, directories and programs\nin lists and menus.\n\nPM-DOS version " );
    str_catn( text, fmt_dec( (u32)major, 0, num ), sizeof( text ) );
    str_catn( text, ".", sizeof( text ) );
    str_catn( text, fmt_dec2( (u32)minor, num ), sizeof( text ) );
    msg_text( "About Shell", text, MB_OK, HELP_INDEX );
}
