/*
 * COMPDLG.C - File > Compress: the DOS build's COMPDLG.CPP in C.
 *
 *     +-- Compress --------------------------------------------------+
 *     |  3 marked items in C:\WORK                                     |
 *     |  Archive:  C:\WORK\WORK.ZIP____________________  [ Browse ]   |
 *     |  Format:   (*) Zip         Options:  [X] Store folder names    |
 *     |            ( ) 7-Zip                 [ ] Clear Archive attribute
 *     |  Exclude:  *.BAK;*.TMP______________________________________   |
 *     |            Names separated by ';'.  @FILE exclusion file.      |
 *     |                         [ OK ]    [ Cancel ]                   |
 *     +----------------------------------------------------------------+
 *
 * Two of the controls are fields, and a field that has the keyboard gets
 * every letter typed, so the rest answer to Alt+letter (Alt+B Browse,
 * Alt+Z Zip, Alt+7 7-Zip, Alt+F folder names, Alt+C Clear Archive
 * attribute, Alt+A and Alt+E the two fields) and to the mouse.  Tab
 * moves between the fields; Enter is OK and Esc is Cancel.  Picking the
 * format changes the name's extension to match, and a name that says
 * .7Z or .ZIP has the last word on the format.
 */
#include "xarc.h"

#define CD_W        72
#define CD_H        14
#define CD_FIELDCOL 13
#define CD_OPTCOL   39

#define ALT_B   KEY_EXT( 0x30 )
#define ALT_Z   KEY_EXT( 0x2C )
#define ALT_7   KEY_EXT( 0x7E )
#define ALT_F   KEY_EXT( 0x21 )
#define ALT_C   KEY_EXT( 0x2E )
#define ALT_A   KEY_EXT( 0x1E )
#define ALT_E   KEY_EXT( 0x12 )

static char *ext_of( char *path )
{
    char *at, *dot = NULL;

    for ( at = path; *at; at++ ) {
        if ( *at == '\\' || *at == '/' || *at == ':' ) {
            dot = NULL;
        } else if ( *at == '.' ) {
            dot = at;
        }
    }
    return dot;
}

/* 2 for .7Z, 1 for .ZIP, 0 for anything else */
static int format_of_name( char *path )
{
    char *ext = ext_of( path );

    if ( ext == NULL ) {
        return 0;
    }
    if ( !stricmp( ext, ".7z" ) ) {
        return 2;
    }
    if ( !stricmp( ext, ".zip" ) ) {
        return 1;
    }
    return 0;
}

/* the format's extension on the name - unless it ends in something else
   altogether, which was typed on purpose (a .JAR is a zip) */
static void match_extension( char *path, int size, int fmt )
{
    char *ext = ext_of( path );
    const char *want = fmt == 2 ? ".7Z" : ".ZIP";
    int len;

    if ( ext && format_of_name( path ) == 0 ) {
        return;
    }
    if ( ext ) {
        *ext = 0;
    }
    len = (int)strlen( path );
    if ( len == 0 || path[len - 1] == '\\' || path[len - 1] == ':' ) {
        return;
    }
    if ( len + (int)strlen( want ) < size ) {
        strcat( path, want );
    }
}

static void focus_fields( FIELD *arc, FIELD *excl, FIELD *focus )
{
    arc->focus = focus == arc;
    excl->focus = focus == excl;
    field_draw( arc );
    field_draw( excl );
}

/* Browse: walk to a folder and name the archive there.  The box opens on
   the folder the field names, with the name filled in. */
static void browse( FIELD *arc, RADIO *format, int *last_fmt )
{
    char start[CD_PATH_MAX], leaf[CD_PATH_MAX], chosen[CD_PATH_MAX + 8];
    const char *text = arc->buffer, *at, *cut = text;
    int fmt;

    for ( at = text; *at; at++ ) {
        if ( *at == '\\' || *at == '/' || *at == ':' ) {
            cut = at + 1;
        }
    }
    strncpy( leaf, cut, sizeof( leaf ) - 1 );
    leaf[sizeof( leaf ) - 1] = 0;
    memcpy( start, text, (size_t)(cut - text) );
    start[cut - text] = 0;
    if ( file_box( "Save Archive As", "Pick the folder, and type the archive's name:", FB_SAVE,
                   "*.ZIP;*.7Z", start[0] ? start : NULL, leaf, chosen, CD_PATH_MAX ) ) {
        fmt = format_of_name( chosen );
        if ( fmt ) {
            format->chosen = fmt - 1;
            *last_fmt = fmt - 1;
        } else {
            match_extension( chosen, CD_PATH_MAX, *last_fmt + 1 );
        }
        field_set( arc, chosen );
    }
    field_draw( arc );
    radio_draw( format );
}

int compress_dialog( const char *what, COMPRESULT *res )
{
    POPUP box;
    FIELD arc, excl, *focus = &arc;
    RADIO format;
    CHECK folders, clear_arc;
    BUTTON ok, cancel, browse_btn;
    int rows = df_rows(), cols = df_cols(), w = CD_W, h = CD_H, left, top;
    int done = 0, last_fmt, want_browse, fmt;
    char shown[CD_W], buf[CD_PATH_MAX + 8];
    EVENT ev;

    if ( w > cols ) {
        w = cols;
    }
    left = (cols - w) / 2;
    top = (rows - h) / 2;
    popup_init( &box, FG_BLACK, BG_WHITE, "Compress" );
    box.border = BORDER_DOUBLE;
    popup_place( &box, top, left, top + h - 1, left + w - 1 );

    field_init( &arc, FG_BRIGHT_WHITE, BG_BLUE );
    field_max( &arc, CD_PATH_MAX - 1 );
    field_set( &arc, res->archive );
    field_place( &arc, top + 4, left + CD_FIELDCOL, w - CD_FIELDCOL - 15 );
    arc.focus = 1;
    button_init( &browse_btn, FG_WHITE, BG_CYAN, "Browse" );
    button_row( &browse_btn, top + 4, left + w - 12 );
    radio_init( &format, FG_BLACK, BG_WHITE, top + 6, left + CD_FIELDCOL );
    radio_add( &format, "Zip" );
    radio_add( &format, "7-Zip" );
    format.chosen = res->fmt == ARC_CF_7Z ? 1 : 0;
    last_fmt = format.chosen;
    check_init( &folders, FG_BLACK, BG_WHITE, "Store folder names", top + 6, left + CD_OPTCOL );
    folders.checked = res->keep_paths != 0;
    check_init( &clear_arc, FG_BLACK, BG_WHITE, "Clear Archive attribute", top + 7,
                left + CD_OPTCOL );
    clear_arc.checked = res->clear_arc != 0;
    field_init( &excl, FG_BRIGHT_WHITE, BG_BLUE );
    field_max( &excl, TF_TEXT_MAX - 1 );
    field_set( &excl, res->exclude );
    field_place( &excl, top + 9, left + CD_FIELDCOL, w - CD_FIELDCOL - 3 );
    button_init( &ok, FG_WHITE, BG_CYAN, "OK" );
    button_init( &cancel, FG_WHITE, BG_CYAN, "Cancel" );
    button_row( &ok, top + h - 2, left + (w - 20) / 2 );
    button_row( &cancel, top + h - 2, left + (w - 20) / 2 + 10 );

    popup_draw( &box );
    strncpy( shown, what ? what : "", (size_t)(w - 6) );
    shown[w - 6] = 0;
    df_text( top + 3, left + 4, shown, FG_BLACK, BG_WHITE );
    df_text( top + 5, left + 4, "Archive:", FG_BLACK, BG_WHITE );
    df_text( top + 7, left + 4, "Format:", FG_BLACK, BG_WHITE );
    df_text( top + 7, left + CD_OPTCOL - 9, "Options:", FG_BLACK, BG_WHITE );
    df_text( top + 10, left + 4, "Exclude:", FG_BLACK, BG_WHITE );
    df_text( top + 11, left + CD_FIELDCOL + 1, "Names separated by ';'.  @FILE exclusion file.",
             FG_BLUE, BG_WHITE );
    field_draw( &arc );
    button_draw( &browse_btn );
    radio_draw( &format );
    check_draw( &folders );
    check_draw( &clear_arc );
    field_draw( &excl );
    button_draw( &ok );
    button_draw( &cancel );

    while ( !done ) {
        want_browse = 0;
        ev_wait( &ev );
        if ( ev.kind == EV_KEY ) {
            if ( ev.key == KEY_ESC ) {
                done = -1;
                break;
            }
            if ( ev.key == KEY_ENTER ) {
                done = 1;
                break;
            }
            if ( ev.key == KEY_TAB || ev.key == KEY_SHTAB ) {
                focus = focus == &arc ? &excl : &arc;
                focus_fields( &arc, &excl, focus );
                continue;
            }
            if ( ev.key == ALT_A || ev.key == ALT_E ) {
                focus = ev.key == ALT_A ? &arc : &excl;
                focus_fields( &arc, &excl, focus );
                continue;
            }
            if ( ev.key == ALT_Z || ev.key == ALT_7 ) {
                format.chosen = ev.key == ALT_Z ? 0 : 1;
                radio_draw( &format );
            } else if ( ev.key == ALT_F ) {
                folders.checked = !folders.checked;
                check_draw( &folders );
            } else if ( ev.key == ALT_C ) {
                clear_arc.checked = !clear_arc.checked;
                check_draw( &clear_arc );
            } else if ( ev.key == ALT_B ) {
                want_browse = 1;
            } else if ( !field_event( focus, &ev ) ) {
                radio_event( &format, &ev );
            }
        } else if ( ev.kind == EV_MOUSE && (ev.press & LEFT_BUTTON) ) {
            if ( button_hit( &ok, ev.x, ev.y ) ) {
                done = 1;
                break;
            }
            if ( button_hit( &cancel, ev.x, ev.y ) ) {
                done = -1;
                break;
            }
            if ( button_hit( &browse_btn, ev.x, ev.y ) ) {
                want_browse = 1;
            } else if ( field_hit( &arc, ev.x, ev.y ) || field_hit( &excl, ev.x, ev.y ) ) {
                focus = field_hit( &arc, ev.x, ev.y ) ? &arc : &excl;
                arc.focus = excl.focus = 0;
                field_event( focus, &ev );
                field_draw( &arc );
                field_draw( &excl );
            } else if ( !radio_event( &format, &ev ) && !check_event( &folders, &ev ) ) {
                check_event( &clear_arc, &ev );
            }
        } else {
            continue;
        }
        /* a change of format puts the matching extension on the name */
        if ( format.chosen != last_fmt ) {
            last_fmt = format.chosen;
            strncpy( buf, arc.buffer, CD_PATH_MAX - 1 );
            buf[CD_PATH_MAX - 1] = 0;
            match_extension( buf, CD_PATH_MAX, last_fmt + 1 );
            field_set( &arc, buf );
            field_draw( &arc );
        }
        if ( want_browse ) {
            browse( &arc, &format, &last_fmt );
        }
    }
    popup_restore( &box );
    popup_free( &box );
    if ( done != 1 ) {
        return 0;
    }
    strncpy( res->archive, arc.buffer, CD_PATH_MAX - 1 );
    res->archive[CD_PATH_MAX - 1] = 0;
    res->fmt = format.chosen + 1;
    fmt = format_of_name( res->archive );
    if ( fmt ) {
        res->fmt = fmt;
    } else {
        match_extension( res->archive, CD_PATH_MAX, res->fmt );
    }
    res->keep_paths = folders.checked;
    res->clear_arc = clear_arc.checked;
    strncpy( res->exclude, excl.buffer, TF_TEXT_MAX - 1 );
    res->exclude[TF_TEXT_MAX - 1] = 0;
    return 1;
}
