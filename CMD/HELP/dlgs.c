/*
 * DLGS.C - Find, Print, Printer Setup and About.
 *
 * FIND looks through every topic for a piece of text, from the cursor
 * on and round again to it, and shows the topic it is in with the
 * cursor on the match; F3 looks for the next one.  Match Upper/
 * Lowercase and Whole Word are the original's two options.
 *
 * PRINT writes the topic shown, as plain text, to a printer port or a
 * file; Printer Setup chooses the port.
 */
#include "help.h"

/* ------------------------------------------------------------------ */
/* Find                                                                */
/* ------------------------------------------------------------------ */

static char find_text[128];
static int  match_case, whole_word, have_find;

static int word_ch( int ch )
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
           || ch == '_' || ch >= 128;
}

/* "find_text" in "line" at "at" or after: the column, or -1 */
static int match_in( const char *line, int at )
{
    int len = (int)str_len( line ), need = (int)str_len( find_text ), col;

    for ( col = at; col + need <= len; col++ ) {
        if ( match_case ? mem_cmp( line + col, find_text, (u32)need ) != 0
                        : str_nicmp( line + col, find_text, (u32)need ) != 0 ) {
            continue;
        }
        if ( whole_word ) {
            if ( col > 0 && word_ch( line[col - 1] ) ) {
                continue;
            }
            if ( col + need < len && word_ch( line[col + need] ) ) {
                continue;
            }
        }
        return col;
    }
    return -1;
}

/* from just after the cursor, through every topic and round: where */
static int search( int *topic, u32 *line, int *col )
{
    static char text[RL_MAX + 1];
    int at_topic = view.topic < 0 ? 0 : view.topic, first = 1, found;
    u32 at_line = view.topic < 0 ? 0 : view.line, start_topic, start_line, count;
    int start_col = view.topic < 0 ? -1 : (int)view.col, begin;
    u32 visited = 0;

    if ( ntopics == 0 || find_text[0] == 0 ) {
        return 0;
    }
    start_topic = (u32)at_topic;
    start_line = at_line;
    for ( ;; ) {
        count = topics[at_topic].count;
        for ( ; at_line < count; at_line++ ) {
            hlp_plain( topics[at_topic].first + at_line, text, sizeof( text ) );
            begin = first ? start_col + 1 : 0;
            first = 0;
            found = match_in( text, begin );
            if ( found >= 0 ) {
                *topic = at_topic;
                *line = at_line;
                *col = found;
                return 1;
            }
            if ( (u32)at_topic == start_topic && at_line == start_line && visited ) {
                return 0;                       /* all the way round */
            }
            visited++;
        }
        at_topic = (at_topic + 1) % ntopics;
        at_line = 0;
        if ( (u32)at_topic == start_topic && visited > nhlines ) {
            return 0;
        }
        if ( visited > nhlines + 1 ) {
            return 0;
        }
    }
}

static void find_and_show( void )
{
    int topic, col;
    u32 line;

    app_status( "Searching - press Esc to cancel" );
    app_draw();
    scr_flush();
    if ( search( &topic, &line, &col ) ) {
        app_status( NULL );
        if ( topic != view.topic ) {
            app_show( topic );
        }
        view_goto( line, (u32)col );
        return;
    }
    app_status( NULL );
    msg_box( "Match not found", MB_OKHELP, CTX_FIND_DLG );
}

/* the word the cursor is on, or the one to its left */
static void word_at_cursor( char *out, u32 max )
{
    static char text[RL_MAX + 1];
    int at, start, end;

    out[0] = 0;
    if ( view.topic < 0 ) {
        return;
    }
    hlp_plain( topics[view.topic].first + view.line, text, sizeof( text ) );
    at = (int)view.col;
    if ( at > (int)str_len( text ) ) {
        at = (int)str_len( text );
    }
    if ( !(at < (int)str_len( text ) && word_ch( text[at] )) ) {
        while ( at > 0 && !word_ch( text[at - 1] ) ) {
            at--;
        }
        if ( at == 0 ) {
            return;
        }
        at--;
    }
    start = at;
    while ( start > 0 && word_ch( text[start - 1] ) ) {
        start--;
    }
    end = at;
    while ( text[end] && word_ch( text[end] ) ) {
        end++;
    }
    if ( (u32)(end - start) >= max ) {
        end = start + (int)max - 1;
    }
    mem_cpy( out, text + start, (u32)(end - start) );
    out[end - start] = 0;
}

static int find_press( DLG *dlg, int ctl )
{
    int id = dlg->ctl[ctl].id;

    if ( id == ID_CANCEL ) {
        return ID_CANCEL;
    }
    if ( find_text[0] == 0 ) {
        return 0;                               /* nothing to look for: stays open */
    }
    return id;
}

int dlg_find( void )
{
    CTL ctl[7];
    DLG dlg;
    char offer[128];
    int count = 0;

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    word_at_cursor( offer, sizeof( offer ) );
    if ( offer[0] ) {
        str_cpy( find_text, offer );
    }
    ctl[count].type = CT_LABEL; ctl[count].row = 2; ctl[count].col = 2;
    ctl[count++].text = "Find &What:";
    ctl[count].type = CT_EDIT; ctl[count].row = 2; ctl[count].col = 14; ctl[count].width = 43;
    ctl[count].buf = find_text; ctl[count++].max = sizeof( find_text );
    ctl[count].type = CT_CHECK; ctl[count].row = 5; ctl[count].col = 4;
    ctl[count].text = "&Match Upper/Lowercase"; ctl[count++].value = &match_case;
    ctl[count].type = CT_CHECK; ctl[count].row = 5; ctl[count].col = 38;
    ctl[count].text = "Whole W&ord"; ctl[count++].value = &whole_word;
    ctl[count].type = CT_BUTTON; ctl[count].row = 7; ctl[count].col = 8;
    ctl[count].text = "OK"; ctl[count].id = ID_OK; ctl[count++].is_default = 1;
    ctl[count].type = CT_BUTTON; ctl[count].row = 7; ctl[count].col = 24;
    ctl[count].text = "Cancel"; ctl[count++].id = ID_CANCEL;
    ctl[count].type = CT_BUTTON; ctl[count].row = 7; ctl[count].col = 42;
    ctl[count].text = "&Help"; ctl[count++].id = ID_HELP;
    dlg.title = "Find";
    dlg.rows = 9;
    dlg.cols = 59;
    dlg.ctl = ctl;
    dlg.nctl = count;
    dlg.focus = 1;
    dlg.help = CTX_FIND_DLG;
    dlg.activate = find_press;
    if ( dlg_run( &dlg ) != ID_OK || find_text[0] == 0 ) {
        return 0;
    }
    have_find = 1;
    find_and_show();
    return 1;
}

int find_can_repeat( void )
{
    return 1;
}

/* F3: the last text again; with none yet, the word under the cursor */
void find_again( void )
{
    char offer[128];

    if ( !have_find ) {
        word_at_cursor( offer, sizeof( offer ) );
        if ( offer[0] == 0 ) {
            dlg_find();
            return;
        }
        str_cpy( find_text, offer );
        have_find = 1;
    }
    find_and_show();
}

/* ------------------------------------------------------------------ */
/* Print                                                               */
/* ------------------------------------------------------------------ */

char print_port[8] = "LPT1";

static int  print_where;                /* 1 printer, 2 file */
static char print_file[PATH_MAX];
static int  port_choice;

static const char *const ports[] = { "LPT1", "LPT2", "LPT3", "COM1", "COM2" };

static void dlg_printer_setup( void )
{
    CTL ctl[8];
    DLG dlg;
    int count = 0, index;

    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    port_choice = 0;
    for ( index = 0; index < 5; index++ ) {
        if ( str_icmp( ports[index], print_port ) == 0 ) {
            port_choice = index;
        }
    }
    ctl[count].type = CT_LABEL; ctl[count].row = 2; ctl[count].col = 3;
    ctl[count++].text = "Use the printer connected to:";
    for ( index = 0; index < 5; index++ ) {
        ctl[count].type = CT_RADIO;
        ctl[count].row = 4 + (index < 3 ? index : index - 3);
        ctl[count].col = index < 3 ? 5 : 21;
        ctl[count].text = ports[index];
        ctl[count].value = &port_choice;
        ctl[count++].id = index;
    }
    ctl[count].type = CT_BUTTON; ctl[count].row = 8; ctl[count].col = 2;
    ctl[count].text = "OK"; ctl[count].id = ID_OK; ctl[count++].is_default = 1;
    ctl[count].type = CT_BUTTON; ctl[count].row = 8; ctl[count].col = 14;
    ctl[count].text = "Cancel"; ctl[count++].id = ID_CANCEL;
    ctl[count].type = CT_BUTTON; ctl[count].row = 8; ctl[count].col = 28;
    ctl[count].text = "&Help"; ctl[count++].id = ID_HELP;
    dlg.title = "Printer Setup";
    dlg.rows = 10;
    dlg.cols = 40;
    dlg.ctl = ctl;
    dlg.nctl = count;
    dlg.focus = 1 + port_choice;
    dlg.help = CTX_SETUP_DLG;
    if ( dlg_run( &dlg ) == ID_OK ) {
        str_cpy( print_port, ports[port_choice] );
    }
}

/* the file name field is reached only when File is chosen */
static void print_changed( DLG *dlg, int ctl )
{
    (void)ctl;
    dlg->ctl[4].off = print_where != 2;
}

static int print_press( DLG *dlg, int ctl )
{
    int id = dlg->ctl[ctl].id;

    if ( id == ID_YES ) {                       /* Printer Setup... */
        dlg_printer_setup();
        return 0;
    }
    if ( id == ID_OK && print_where == 2 && print_file[0] == 0 ) {
        return 0;
    }
    return id;
}

static int print_topic( const char *where, int is_file )
{
    static char text[RL_MAX + 3];
    u32 handle, done, line, count, len;
    int rc;

    rc = is_file ? sys_create( where, &handle ) : sys_open_write( where, &handle );
    if ( rc ) {
        return rc;
    }
    count = topics[view.topic].count;
    for ( line = 0; line < count && rc == 0; line++ ) {
        hlp_plain( topics[view.topic].first + line, text, RL_MAX + 1 );
        len = str_len( text );
        text[len++] = '\r';
        text[len++] = '\n';
        rc = sys_write( handle, text, len, &done );
    }
    if ( rc == 0 ) {
        rc = sys_write( handle, "\f", 1, &done );
    }
    sys_close( handle );
    return rc;
}

int dlg_print( void )
{
    CTL ctl[9];
    DLG dlg;
    static char label[24];
    int count = 0, rc;

    if ( view.topic < 0 ) {
        return 0;
    }
    mem_set( ctl, 0, sizeof( ctl ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    if ( print_where == 0 ) {
        print_where = 1;
    }
    str_cpy( label, "&Printer on " );
    str_catn( label, print_port, sizeof( label ) );
    ctl[count].type = CT_LABEL; ctl[count].row = 2; ctl[count].col = 4;
    ctl[count++].text = "Print the current topic to:";
    ctl[count].type = CT_RADIO; ctl[count].row = 4; ctl[count].col = 6;
    ctl[count].text = label; ctl[count].value = &print_where; ctl[count++].id = 1;
    ctl[count].type = CT_RADIO; ctl[count].row = 5; ctl[count].col = 6;
    ctl[count].text = "&File"; ctl[count].value = &print_where; ctl[count++].id = 2;
    ctl[count].type = CT_LABEL; ctl[count].row = 7; ctl[count].col = 4;
    ctl[count++].text = "File&name:";
    ctl[count].type = CT_EDIT; ctl[count].row = 7; ctl[count].col = 15; ctl[count].width = 32;
    ctl[count].buf = print_file; ctl[count].max = sizeof( print_file );
    ctl[count++].off = print_where != 2;
    ctl[count].type = CT_BUTTON; ctl[count].row = 9; ctl[count].col = 2;
    ctl[count].text = "OK"; ctl[count].id = ID_OK; ctl[count++].is_default = 1;
    ctl[count].type = CT_BUTTON; ctl[count].row = 9; ctl[count].col = 11;
    ctl[count].text = "Cancel"; ctl[count++].id = ID_CANCEL;
    ctl[count].type = CT_BUTTON; ctl[count].row = 9; ctl[count].col = 22;
    ctl[count].text = "Printer &Setup..."; ctl[count++].id = ID_YES;
    ctl[count].type = CT_BUTTON; ctl[count].row = 9; ctl[count].col = 43;
    ctl[count].text = "&Help"; ctl[count++].id = ID_HELP;
    dlg.title = "Print";
    dlg.rows = 11;
    dlg.cols = 54;
    dlg.ctl = ctl;
    dlg.nctl = count;
    dlg.focus = print_where == 2 ? 2 : 1;
    dlg.help = CTX_PRINT_DLG;
    dlg.changed = print_changed;
    dlg.activate = print_press;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return 0;
    }
    rc = print_topic( print_where == 2 ? print_file : print_port, print_where == 2 );
    if ( rc ) {
        msg_box2( print_where == 2 ? "The file could not be written." : "The printer could not be written to.",
                  err_text( rc ), MB_OK, NULL );
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* About                                                               */
/* ------------------------------------------------------------------ */

void dlg_about( void )
{
    msg_box2( "PM-DOS Help", "Version 1.0", MB_OK, NULL );
}
