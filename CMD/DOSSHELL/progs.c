/*
 * PROGS.C - program groups, DOSSHELL.INI, and running a program.
 *
 * THE PROGRAM LIST IS A TREE OF GROUPS, as the MS-DOS Shell's is: the
 * top group, "Main", holds programs and other groups.  A program is a
 * title and the commands that title runs - several, with semicolons
 * between them - and where to run them, whether to wait for a key
 * afterwards, a password, and for each of %1 to %3 in the commands the
 * box the shell asks with before it runs them.
 *
 * AN ITEM KEEPS ITS PLACE IN THE ARRAY for as long as it lives, so an
 * index into items[] is a name that deleting or reordering something
 * else does not change; the order a group shows its items in is their
 * "order" numbers, not where they sit.
 *
 * DOSSHELL.INI is beside the program, and it is rewritten whenever the
 * shell is left: [savestate] for the screen and the options,
 * [associations] for the extensions tied to programs, [programstarter]
 * for the groups - "key = value" lines, and a key with braces after it
 * for something with parts, laid out like the file the MS-DOS Shell
 * kept.  A file that is missing is not an error: the shell starts with
 * a Main group of its own making.
 *
 * A PROGRAM IS RUN BY THE COMMAND INTERPRETER, "/C" and the command,
 * because it is the interpreter that knows PATH, batch files and
 * redirection.  The screen is given back to DOS first - text, cleared -
 * and taken again afterwards; the kernel gives the program a slot of
 * its own, so the shell stays where it is in memory and nothing is
 * swapped to disk.
 */
#include "shell.h"

#define MAX_ASSOC       48

typedef struct {
    char ext[12];
    char program[80];
} ASSOC;

PITEM *items;
int    nitems;                  /* slots in use or once used */
int    prog_group;
int    prog_cur, prog_top;

static u8    used[MAX_ITEMS + 1];     /* one past the end: a scratch item, for */
static int   order[MAX_ITEMS + 1];    /* reading a file with too many */
static ASSOC assoc[MAX_ASSOC];
static int   nassoc;

/* ------------------------------------------------------------------ */
/* the items                                                           */
/* ------------------------------------------------------------------ */

/* a group's items, in the order it shows them: -> how many */
static int kids( int group, int *out )
{
    int index, count = 0, at, held;

    for ( index = 1; index < nitems; index++ ) {
        if ( used[index] && items[index].parent == group ) {
            held = index;
            for ( at = count; at > 0 && order[out[at - 1]] > order[held]; at-- ) {
                out[at] = out[at - 1];
            }
            out[at] = held;
            count++;
        }
    }
    return count;
}

static int item_new( int parent, int is_group )
{
    int index, list[MAX_ITEMS], count;

    for ( index = 1; index < MAX_ITEMS && index < nitems && used[index]; index++ ) {
    }
    if ( index >= MAX_ITEMS ) {
        return -1;
    }
    if ( index >= nitems ) {
        nitems = index + 1;
    }
    count = parent >= 0 ? kids( parent, list ) : 0;
    mem_set( &items[index], 0, sizeof( PITEM ) );
    items[index].is_group = is_group;
    items[index].parent = parent;
    used[index] = 1;
    order[index] = count ? order[list[count - 1]] + 1 : 0;
    return index;
}

int prog_count( void )
{
    int list[MAX_ITEMS];

    return kids( prog_group, list ) + (prog_group != 0);
}

/* the item on a line of the list; -1 for the line that leads back out
   of a group */
int prog_at( int line )
{
    int list[MAX_ITEMS], count = kids( prog_group, list );

    if ( prog_group != 0 ) {
        if ( line == 0 ) {
            return -1;
        }
        line--;
    }
    return line >= 0 && line < count ? list[line] : -1;
}

void prog_back( void )
{
    int was = prog_group, list[MAX_ITEMS], count, index;

    if ( prog_group == 0 ) {
        return;
    }
    prog_group = items[was].parent;
    count = kids( prog_group, list );
    prog_cur = prog_top = 0;
    for ( index = 0; index < count; index++ ) {
        if ( list[index] == was ) {
            prog_cur = index + (prog_group != 0);
        }
    }
    app_layout();
}

static int password_ok( int item )
{
    char typed[PASS_MAX];

    if ( items[item].password[0] == 0 ) {
        return 1;
    }
    typed[0] = 0;
    if ( !input_box( "Password", "This entry is protected by a password.", "&Password:",
                     typed, sizeof( typed ), 20, HELP_PROGRAMS, 1 ) ) {
        return 0;
    }
    if ( str_cmp( typed, items[item].password ) != 0 ) {
        msg_box( "That is not the password.", MB_OK );
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* running                                                             */
/* ------------------------------------------------------------------ */

static void say( const char *text )
{
    sys_stdout( text, str_len( text ) );
}

static void wait_key( void )
{
    int shift = sys_shift();

    if ( key_testing() ) {
        return;
    }
    while ( sys_key_wait( shift, &shift, 0 ) != 1 ) {
    }
    sys_key_read( &shift );
}

/* what a program may have changed is read again: the directories of
   the drives that are showing, and their files */
static void after_run( void )
{
    scr_resume();
    wins_reread( wins[0].drive );
    if ( nwins > 1 && wins[1].drive != wins[0].drive ) {
        wins_reread( wins[1].drive );
    }
}

int run_command( const char *cmds, const char *dir, int pause )
{
    char shell[96], tail[136], one[CMDS_MAX];
    const char *cur = cmds;
    int error = 0, code = 0, len;

    app_chdir();
    if ( dir && dir[0] ) {
        if ( dir[1] == ':' ) {
            sys_set_drive( ch_upper( dir[0] ) - 'A' );
        }
        sys_chdir( dir );
    }
    sys_shell_path( shell, sizeof( shell ) );
    scr_suspend();
    while ( *cur ) {
        for ( len = 0; cur[len] && cur[len] != ';'; len++ ) {
        }
        str_fit( one, cur, (u32)len + 1 < sizeof( one ) ? (u32)len + 1 : sizeof( one ) );
        cur += len + (cur[len] == ';');
        for ( len = 0; one[len] == ' '; len++ ) {
        }
        if ( one[len] == 0 ) {
            continue;
        }
        str_cpy( tail, " /C " );
        str_fit( tail + 4, one + len, 122 );
        code = sys_exec( shell, tail, &error );
        if ( error ) {
            say( "The command could not be run: " );
            say( err_text( error ) );
            say( "\r\n" );
            pause = 1;
            break;
        }
    }
    if ( pause ) {
        say( "\r\nPress any key to return to the PM-DOS Shell . . . " );
        wait_key();
    }
    after_run();
    return code;
}

void run_prompt( void )
{
    char shell[96];
    int error;

    app_chdir();
    sys_shell_path( shell, sizeof( shell ) );
    scr_suspend();
    say( "Type EXIT to return to the PM-DOS Shell.\r\n" );
    sys_exec( shell, "", &error );
    if ( error ) {
        say( "The command prompt could not be started: " );
        say( err_text( error ) );
        say( "\r\n\r\nPress any key to return to the PM-DOS Shell . . . " );
        wait_key();
    }
    after_run();
}

static int is_program( const char *name )
{
    const char *ext = path_ext( name );

    return str_icmp( ext, "EXE" ) == 0 || str_icmp( ext, "COM" ) == 0
           || str_icmp( ext, "BAT" ) == 0 || str_icmp( ext, "CMD" ) == 0;
}

/* a name on a command line: in quotes when it has a blank in it */
static void quote_onto( char *line, const char *name, u32 max )
{
    int quoted = str_chr( name, ' ' ) != NULL;

    if ( quoted ) {
        str_catn( line, "\"", max );
    }
    str_catn( line, name, max );
    if ( quoted ) {
        str_catn( line, "\"", max );
    }
}

/* File, Open: a program is run; any other file is handed to the
   program its extension is tied to */
void run_file( const char *path )
{
    char line[CMDS_MAX];
    const char *program;

    line[0] = 0;
    if ( is_program( path ) ) {
        quote_onto( line, path, sizeof( line ) );
        run_command( line, NULL, 1 );
        return;
    }
    program = assoc_program( path_ext( path ) );
    if ( program == NULL ) {
        msg_box2( "No program is tied to files of this kind.",
                  "File, Associate ties an extension to a program.", MB_OK );
        return;
    }
    str_fit( line, program, sizeof( line ) );
    str_catn( line, " ", sizeof( line ) );
    quote_onto( line, path, sizeof( line ) );
    run_command( line, NULL, 0 );
}

/* %1 to %3 in a program's commands: each one asked for, once, and put
   in wherever it stands.  0 = a box was cancelled. */
static int fill_parameters( const PITEM *item, char *out, u32 max )
{
    char value[MAX_PROMPTS][64];
    int asked[MAX_PROMPTS], which;
    const char *src;
    const PPROMPT *prompt;
    u32 len = 0, add;

    mem_set( asked, 0, sizeof( asked ) );
    for ( src = item->cmds; *src; src++ ) {
        which = src[0] == '%' && src[1] >= '1' && src[1] <= '0' + MAX_PROMPTS ? src[1] - '1' : -1;
        if ( which < 0 ) {
            if ( len + 1 < max ) {
                out[len++] = *src;
            }
            continue;
        }
        if ( !asked[which] ) {
            prompt = &item->prompt[which];
            str_fit( value[which], prompt->value, sizeof( value[which] ) );
            if ( !input_box( prompt->title[0] ? prompt->title : item->title,
                             prompt->info[0] ? prompt->info : "Type what the program is to be given.",
                             prompt->label[0] ? prompt->label : "Parameters:",
                             value[which], sizeof( value[which] ), 30, HELP_PROGRAMS, 0 ) ) {
                return 0;
            }
            asked[which] = 1;
        }
        add = str_len( value[which] );
        if ( len + add < max ) {
            mem_cpy( out + len, value[which], add );
            len += add;
        }
        src++;
    }
    out[len] = 0;
    return 1;
}

void prog_open( void )
{
    char line[CMDS_MAX + 200];
    int item;

    if ( prog_count() == 0 ) {
        return;
    }
    item = prog_at( prog_cur );
    if ( item < 0 ) {
        prog_back();
        return;
    }
    if ( !password_ok( item ) ) {
        return;
    }
    if ( items[item].is_group ) {
        prog_group = item;
        prog_cur = prog_top = 0;
        return;
    }
    if ( !fill_parameters( &items[item], line, sizeof( line ) ) ) {
        return;
    }
    if ( line[0] == 0 ) {
        msg_box( "This entry has no commands.  File, Properties gives it some.", MB_OK );
        return;
    }
    run_command( line, items[item].dir, items[item].pause );
}

/* F1 on an entry that has help text of its own: the text in a box,
   in lines of fifty characters or so broken at a blank.  0 = the entry
   has none, and F1 is the shell's own help. */
int prog_help( void )
{
    char text[HELP_MAX + 8];
    int item = prog_count() ? prog_at( prog_cur ) : -1;
    int at, col = 0, blank = -1;

    if ( item < 0 || items[item].help[0] == 0 ) {
        return 0;
    }
    str_fit( text, items[item].help, sizeof( text ) );
    for ( at = 0; text[at]; at++ ) {
        if ( text[at] == ' ' ) {
            blank = at;
        }
        if ( ++col > 50 && blank >= 0 ) {
            text[blank] = '\n';
            col = at - blank;
            blank = -1;
        }
    }
    msg_text( items[item].title, text, MB_OK, HELP_PROGRAMS );
    return 1;
}

/* ------------------------------------------------------------------ */
/* changing the list                                                   */
/* ------------------------------------------------------------------ */

static void row_of( CTL *label, CTL *edit, int row, const char *text, char *buf, int max, int width )
{
    ctl_label( label, row, 3, text );
    ctl_edit( edit, row, 26, width, buf, max );
}

/* do the commands say %1, or %2, or whichever "digit" is? */
static int has_parameter( const char *cmds, int digit )
{
    for ( ; cmds[0]; cmds++ ) {
        if ( cmds[0] == '%' && cmds[1] == digit ) {
            return 1;
        }
    }
    return 0;
}

/* the box %n is asked with, for each %n the commands have */
static void edit_prompts( PITEM *item )
{
    static char info[60];
    CTL ctl[12];
    DLG dlg;
    PPROMPT *prompt;
    char mark[3];
    int which;

    for ( which = 0; which < MAX_PROMPTS; which++ ) {
        mark[0] = '%';
        mark[1] = (char)('1' + which);
        mark[2] = 0;
        prompt = &item->prompt[which];
        if ( !has_parameter( item->cmds, mark[1] ) ) {
            continue;
        }
        str_cpy( info, "The box the shell asks with where the commands say " );
        str_catn( info, mark, sizeof( info ) );
        mem_set( &dlg, 0, sizeof( dlg ) );
        ctl_text( &ctl[0], 2, 3, 0, info );
        row_of( &ctl[1], &ctl[2], 4, "Window &Title", prompt->title, sizeof( prompt->title ), 30 );
        row_of( &ctl[3], &ctl[4], 5, "Program &Information", prompt->info, sizeof( prompt->info ), 30 );
        row_of( &ctl[5], &ctl[6], 6, "&Prompt Message", prompt->label, sizeof( prompt->label ), 30 );
        row_of( &ctl[7], &ctl[8], 7, "&Default Parameters", prompt->value, sizeof( prompt->value ), 30 );
        ctl_button( &ctl[9], 9, 12, "OK", ID_OK, 1 );
        ctl_button( &ctl[10], 9, 25, "Cancel", ID_CANCEL, 0 );
        ctl_button( &ctl[11], 9, 42, "&Help", ID_HELP, 0 );
        dlg.title = "Program Item Properties";
        dlg.rows = 11;
        dlg.cols = 62;
        dlg.ctl = ctl;
        dlg.nctl = 12;
        dlg.focus = 2;
        dlg.help = HELP_PROGRAMS;
        if ( dlg_run( &dlg ) != ID_OK ) {
            return;
        }
    }
}

/* a program's or a group's particulars: 1 = OK was pressed */
static int edit_item( PITEM *item, int is_new )
{
    PITEM work = *item;
    CTL ctl[14];
    DLG dlg;
    int count = 0, row = 2;

    mem_set( &dlg, 0, sizeof( dlg ) );
    row_of( &ctl[count], &ctl[count + 1], row++,
            work.is_group ? "&Title" : "Program &Title", work.title, sizeof( work.title ), 30 );
    count += 2;
    if ( !work.is_group ) {
        row_of( &ctl[count], &ctl[count + 1], row++, "&Commands", work.cmds, sizeof( work.cmds ), 30 );
        count += 2;
        row_of( &ctl[count], &ctl[count + 1], row++, "Startup &Directory", work.dir,
                sizeof( work.dir ), 30 );
        count += 2;
    }
    row_of( &ctl[count], &ctl[count + 1], row++, "Help Te&xt", work.help, sizeof( work.help ), 30 );
    count += 2;
    row_of( &ctl[count], &ctl[count + 1], row++, "Pass&word", work.password,
            sizeof( work.password ), 20 );
    count += 2;
    if ( !work.is_group ) {
        row++;
        ctl_check( &ctl[count++], row++, 3, "&Pause after exit", &work.pause );
    }
    row++;
    ctl_button( &ctl[count++], row, 12, "OK", ID_OK, 1 );
    ctl_button( &ctl[count++], row, 25, "Cancel", ID_CANCEL, 0 );
    ctl_button( &ctl[count++], row, 42, "&Help", ID_HELP, 0 );
    dlg.title = work.is_group ? (is_new ? "Add Group" : "Group Properties")
                              : (is_new ? "Add Program" : "Program Item Properties");
    dlg.rows = row + 2;
    dlg.cols = 62;
    dlg.ctl = ctl;
    dlg.nctl = count;
    dlg.focus = 1;
    dlg.help = HELP_PROGRAMS;
    for ( ;; ) {
        if ( dlg_run( &dlg ) != ID_OK ) {
            return 0;
        }
        if ( work.title[0] == 0 ) {
            msg_box( "The entry needs a title.", MB_OK );
            continue;
        }
        break;
    }
    if ( !work.is_group ) {
        edit_prompts( &work );
    }
    *item = work;
    return 1;
}

void prog_new( void )
{
    CTL ctl[5];
    DLG dlg;
    int kind = 0, item;
    PITEM fresh;

    mem_set( &dlg, 0, sizeof( dlg ) );
    ctl_radio( &ctl[0], 2, 4, "Program &Group", &kind, 1 );
    ctl_radio( &ctl[1], 3, 4, "Program &Item", &kind, 0 );
    ctl_button( &ctl[2], 5, 3, "OK", ID_OK, 1 );
    ctl_button( &ctl[3], 5, 12, "Cancel", ID_CANCEL, 0 );
    ctl_button( &ctl[4], 5, 25, "&Help", ID_HELP, 0 );
    dlg.title = "New Program Object";
    dlg.rows = 7;
    dlg.cols = 36;
    dlg.ctl = ctl;
    dlg.nctl = 5;
    dlg.focus = 1;
    dlg.help = HELP_PROGRAMS;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return;
    }
    mem_set( &fresh, 0, sizeof( fresh ) );
    fresh.is_group = kind;
    fresh.parent = prog_group;
    fresh.pause = 1;
    if ( !edit_item( &fresh, 1 ) ) {
        return;
    }
    item = item_new( prog_group, kind );
    if ( item < 0 ) {
        msg_box( "The program list is full.", MB_OK );
        return;
    }
    items[item] = fresh;
    prog_cur = prog_count() - 1;
    app_layout();
}

void prog_props( void )
{
    int item = prog_at( prog_cur );

    if ( item >= 0 && password_ok( item ) ) {
        edit_item( &items[item], 0 );
    }
}

void prog_delete( void )
{
    int item = prog_at( prog_cur ), list[MAX_ITEMS];

    if ( item < 0 || !password_ok( item ) ) {
        return;
    }
    if ( items[item].is_group && kids( item, list ) ) {
        msg_box( "A group has to be empty before it can be deleted.", MB_OK );
        return;
    }
    if ( msg_box2( items[item].is_group ? "Delete this group?" : "Delete this entry?",
                   items[item].title, MB_YESNO ) != ID_YES ) {
        return;
    }
    used[item] = 0;
    if ( prog_cur >= prog_count() && prog_cur > 0 ) {
        prog_cur--;
    }
    app_layout();
}

/* every group, for the Copy box to choose among */
static int group_list[MAX_ITEMS], ngroups;

static const char *group_item( DLG *dlg, int index )
{
    (void)dlg;
    return items[group_list[index]].title;
}

static int copy_activate( DLG *dlg, int ctl )
{
    return dlg->ctl[ctl].type == CT_LIST ? ID_OK : 0;
}

void prog_copy( void )
{
    int item = prog_at( prog_cur ), index, copy;
    CTL ctl[4];
    DLG dlg;

    if ( item < 0 || items[item].is_group ) {
        return;
    }
    ngroups = 0;
    group_list[ngroups++] = 0;
    for ( index = 1; index < nitems; index++ ) {
        if ( used[index] && items[index].is_group ) {
            group_list[ngroups++] = index;
        }
    }
    mem_set( &dlg, 0, sizeof( dlg ) );
    mem_set( ctl, 0, sizeof( ctl ) );
    ctl[0].type = CT_LIST;
    ctl[0].row = 2;
    ctl[0].col = 3;
    ctl[0].width = 34;
    ctl[0].height = 8;
    ctl[0].text = "Copy to &group";
    ctl[0].count = ngroups;
    ctl[0].item = group_item;
    ctl_button( &ctl[1], 11, 4, "OK", ID_OK, 1 );
    ctl_button( &ctl[2], 11, 13, "Cancel", ID_CANCEL, 0 );
    ctl_button( &ctl[3], 11, 26, "&Help", ID_HELP, 0 );
    dlg.title = "Copy Program";
    dlg.rows = 13;
    dlg.cols = 40;
    dlg.ctl = ctl;
    dlg.nctl = 4;
    dlg.help = HELP_PROGRAMS;
    dlg.activate = copy_activate;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return;
    }
    copy = item_new( group_list[ctl[0].sel], 0 );
    if ( copy < 0 ) {
        msg_box( "The program list is full.", MB_OK );
        return;
    }
    items[copy] = items[item];
    items[copy].parent = group_list[ctl[0].sel];
}

/* Reorder: the entry follows the arrow keys up and down the list until
   Enter says where it stays */
void prog_reorder( void )
{
    int key, list[MAX_ITEMS], count, at, other, held;

    if ( prog_at( prog_cur ) < 0 ) {
        return;
    }
    for ( ;; ) {
        app_status( "Up and Down move the entry.  Enter leaves it where it is." );
        scr_cursor( 0, 0, CUR_HIDE );
        app_draw();
        scr_flush();
        key = key_get();
        if ( key_service( key ) ) {
            continue;
        }
        count = kids( prog_group, list );
        at = prog_cur - (prog_group != 0);
        other = key == K_UP ? at - 1 : key == K_DOWN ? at + 1 : -1;
        if ( key == K_ENTER || key == K_ESC || (key == K_MOUSE && mouse.kind == ME_DOWN) ) {
            break;
        }
        if ( other < 0 || other >= count ) {
            continue;
        }
        held = order[list[at]];
        order[list[at]] = order[list[other]];
        order[list[other]] = held;
        prog_cur += other - at;
        app_layout();
    }
    app_status( NULL );
}

/* ------------------------------------------------------------------ */
/* extensions tied to programs                                         */
/* ------------------------------------------------------------------ */

const char *assoc_program( const char *ext )
{
    int index;

    for ( index = 0; index < nassoc; index++ ) {
        if ( str_icmp( assoc[index].ext, ext ) == 0 ) {
            return assoc[index].program;
        }
    }
    return NULL;
}

/* an empty program unties the extension */
void assoc_set( const char *ext, const char *program )
{
    int index;

    for ( index = 0; index < nassoc && str_icmp( assoc[index].ext, ext ) != 0; index++ ) {
    }
    if ( program[0] == 0 ) {
        if ( index < nassoc ) {
            assoc[index] = assoc[--nassoc];
        }
        return;
    }
    if ( index == nassoc ) {
        if ( nassoc == MAX_ASSOC ) {
            return;
        }
        nassoc++;
        str_fit( assoc[index].ext, ext, sizeof( assoc[index].ext ) );
        str_upper( assoc[index].ext );
    }
    str_fit( assoc[index].program, program, sizeof( assoc[index].program ) );
}

int assoc_count( void )
{
    return nassoc;
}

const char *assoc_ext( int index )
{
    return assoc[index].ext;
}

const char *assoc_prog_at( int index )
{
    return assoc[index].program;
}

/* ------------------------------------------------------------------ */
/* DOSSHELL.INI: reading                                               */
/* ------------------------------------------------------------------ */

static char *ini_buf;
static u32   ini_len, ini_pos;

/* The next line that says something: a key and its value, with "[" for
   a section (its name the value), "{" and "}" for themselves.  0 at
   the end of the file. */
static int ini_next( char **key, char **value )
{
    char *line, *end, *eq;

    while ( ini_pos < ini_len ) {
        line = ini_buf + ini_pos;
        for ( end = line; end < ini_buf + ini_len && *end != '\n'; end++ ) {
        }
        ini_pos = (u32)(end - ini_buf) + 1;
        while ( end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t') ) {
            end--;
        }
        *end = 0;
        while ( *line == ' ' || *line == '\t' ) {
            line++;
        }
        if ( *line == 0 || *line == ';' ) {
            continue;
        }
        if ( *line == '[' ) {
            eq = str_chr( line, ']' );
            if ( eq ) {
                *eq = 0;
            }
            *key = "[";
            *value = line + 1;
            return 1;
        }
        if ( *line == '{' || *line == '}' ) {
            *key = *line == '{' ? "{" : "}";
            *value = "";
            return 1;
        }
        eq = str_chr( line, '=' );
        if ( eq == NULL ) {
            continue;
        }
        *value = eq + 1;
        while ( **value == ' ' || **value == '\t' ) {
            (*value)++;
        }
        while ( eq > line && (eq[-1] == ' ' || eq[-1] == '\t') ) {
            eq--;
        }
        *eq = 0;
        *key = line;
        return 1;
    }
    return 0;
}

static int is_on( const char *value )
{
    return str_icmp( value, "enabled" ) == 0 || str_icmp( value, "on" ) == 0
           || str_icmp( value, "1" ) == 0;
}

/* past a "{" that belongs to the key just read */
static void ini_open( const char *value )
{
    char *key, *more;
    u32 at = ini_pos;

    if ( value[0] == '{' ) {
        return;
    }
    if ( !ini_next( &key, &more ) || key[0] != '{' ) {
        ini_pos = at;                   /* no brace: the next line is its own */
    }
}

static void parse_dialog( PITEM *item )
{
    char *key, *value;
    PPROMPT scratch, *prompt = &scratch;

    mem_set( &scratch, 0, sizeof( scratch ) );
    while ( ini_next( &key, &value ) && key[0] != '}' ) {
        if ( str_icmp( key, "parameter" ) == 0 ) {
            if ( value[0] == '%' && value[1] >= '1' && value[1] <= '0' + MAX_PROMPTS ) {
                item->prompt[value[1] - '1'] = scratch;
                prompt = &item->prompt[value[1] - '1'];
            }
        } else if ( str_icmp( key, "title" ) == 0 ) {
            str_fit( prompt->title, value, sizeof( prompt->title ) );
        } else if ( str_icmp( key, "info" ) == 0 ) {
            str_fit( prompt->info, value, sizeof( prompt->info ) );
        } else if ( str_icmp( key, "prompt" ) == 0 ) {
            str_fit( prompt->label, value, sizeof( prompt->label ) );
        } else if ( str_icmp( key, "default" ) == 0 ) {
            str_fit( prompt->value, value, sizeof( prompt->value ) );
        }
    }
}

static void parse_group( int group );

static void parse_program( int item )
{
    char *key, *value;
    PITEM *prog = &items[item];

    while ( ini_next( &key, &value ) && key[0] != '}' ) {
        if ( str_icmp( key, "title" ) == 0 ) {
            str_fit( prog->title, value, sizeof( prog->title ) );
        } else if ( str_icmp( key, "command" ) == 0 ) {
            str_fit( prog->cmds, value, sizeof( prog->cmds ) );
        } else if ( str_icmp( key, "help" ) == 0 ) {
            str_fit( prog->help, value, sizeof( prog->help ) );
        } else if ( str_icmp( key, "directory" ) == 0 ) {
            str_fit( prog->dir, value, sizeof( prog->dir ) );
        } else if ( str_icmp( key, "password" ) == 0 ) {
            str_fit( prog->password, value, sizeof( prog->password ) );
        } else if ( str_icmp( key, "pause" ) == 0 ) {
            prog->pause = is_on( value );
        } else if ( str_icmp( key, "dialog" ) == 0 ) {
            ini_open( value );
            parse_dialog( prog );
        }
    }
}

static void parse_group( int group )
{
    char *key, *value;
    int item;

    while ( ini_next( &key, &value ) && key[0] != '}' ) {
        if ( str_icmp( key, "title" ) == 0 ) {
            str_fit( items[group].title, value, sizeof( items[group].title ) );
        } else if ( str_icmp( key, "help" ) == 0 ) {
            str_fit( items[group].help, value, sizeof( items[group].help ) );
        } else if ( str_icmp( key, "password" ) == 0 ) {
            str_fit( items[group].password, value, sizeof( items[group].password ) );
        } else if ( str_icmp( key, "program" ) == 0 || str_icmp( key, "group" ) == 0 ) {
            item = item_new( group, ch_upper( key[0] ) == 'G' );
            ini_open( value );
            if ( item < 0 ) {
                item = MAX_ITEMS;               /* full: read into the scratch item */
            }
            if ( ch_upper( key[0] ) == 'G' ) {
                parse_group( item );
            } else {
                parse_program( item );
            }
        }
    }
}

static void parse_state( const char *key, const char *value )
{
    static const char *const views[] = { "single", "dual", "all", "program/file", "program" };
    static const char *const sorts[] = { "name", "extension", "date", "size", "diskorder" };
    int index;

    if ( str_icmp( key, "display" ) == 0 ) {
        for ( index = 0; index < NDMODES; index++ ) {
            if ( str_icmp( value, dmodes[index].tag ) == 0 ) {
                opt.display = index;
            }
        }
    } else if ( str_icmp( key, "scheme" ) == 0 ) {
        for ( index = 0; index < scheme_count(); index++ ) {
            if ( str_icmp( value, scheme_name( index ) ) == 0 ) {
                opt.scheme = index;
            }
        }
    } else if ( str_icmp( key, "view" ) == 0 ) {
        for ( index = 0; index < 5; index++ ) {
            if ( str_icmp( value, views[index] ) == 0 ) {
                opt.view = index;
            }
        }
    } else if ( str_icmp( key, "sortkey" ) == 0 ) {
        for ( index = 0; index < 5; index++ ) {
            if ( str_icmp( value, sorts[index] ) == 0 ) {
                opt.sort_key = index;
            }
        }
    } else if ( str_icmp( key, "sortorder" ) == 0 ) {
        opt.sort_desc = str_icmp( value, "descending" ) == 0;
    } else if ( str_icmp( key, "filter" ) == 0 ) {
        if ( value[0] ) {
            str_fit( opt.filter, value, sizeof( opt.filter ) );
        }
    } else if ( str_icmp( key, "hidden" ) == 0 ) {
        opt.show_hidden = is_on( value );
    } else if ( str_icmp( key, "confirmdelete" ) == 0 ) {
        opt.confirm_delete = is_on( value );
    } else if ( str_icmp( key, "confirmreplace" ) == 0 ) {
        opt.confirm_replace = is_on( value );
    } else if ( str_icmp( key, "confirmmouse" ) == 0 ) {
        opt.confirm_mouse = is_on( value );
    } else if ( str_icmp( key, "selectacross" ) == 0 ) {
        opt.select_across = is_on( value );
    }
}

/* what the list holds when there is no file to say */
static void default_items( void )
{
    static const struct {
        int group;              /* 0 Main, 1 Disk Utilities */
        const char *title, *cmds, *help;
        const char *ask_title, *ask_info, *ask_label, *ask_value;
        int pause;
    } builtin[] = {
        { 0, "Command Prompt", "CMD", "Starts the command prompt.  Type EXIT to come back.",
          NULL, NULL, NULL, NULL, 0 },
        { 0, "Editor", "EDIT %1", "Starts the PM-DOS Editor.",
          "File to Edit", "Type the name of a file, or press Enter for a new one.",
          "File to edit?", "", 0 },
        { 1, "Disk Copy", "DISKCOPY %1", "Copies one diskette onto another.",
          "Disk Copy", "Type the drive to copy from and the drive to copy to.",
          "Drives:", "A: A:", 1 },
        { 1, "Quick Format", "FORMAT %1 /Q", "Empties a disk that is already formatted.",
          "Quick Format", "Type the drive to format.", "Drive:", "A:", 1 },
        { 1, "Format", "FORMAT %1", "Formats a disk.",
          "Format", "Type the drive to format.", "Drive:", "A:", 1 },
        { 1, "Check Disk", "CHKDSK %1", "Checks a disk and reports what is on it.",
          "Check Disk", "Type the drive to check.", "Drive:", "C:", 1 },
        { 1, "Undelete", "UNDELETE %1", "Brings back files that were deleted.",
          "Undelete", "Type a file name, or press Enter for the current directory.",
          "File:", "", 1 },
        { 1, "Fixed Disk Setup", "FDISK", "Shows and changes the partitions of a hard disk.",
          NULL, NULL, NULL, NULL, 0 }
    };
    int index, item, utils;
    PITEM *prog;

    utils = item_new( 0, 1 );
    str_cpy( items[utils].title, "Disk Utilities" );
    str_cpy( items[utils].help, "Programs that work on whole disks." );
    for ( index = 0; index < (int)(sizeof( builtin ) / sizeof( builtin[0] )); index++ ) {
        item = item_new( builtin[index].group ? utils : 0, 0 );
        prog = &items[item];
        str_cpy( prog->title, builtin[index].title );
        str_cpy( prog->cmds, builtin[index].cmds );
        str_cpy( prog->help, builtin[index].help );
        prog->pause = builtin[index].pause;
        if ( builtin[index].ask_title ) {
            str_cpy( prog->prompt[0].title, builtin[index].ask_title );
            str_cpy( prog->prompt[0].info, builtin[index].ask_info );
            str_cpy( prog->prompt[0].label, builtin[index].ask_label );
            str_cpy( prog->prompt[0].value, builtin[index].ask_value );
        }
    }
    /* Main shows its programs first and the group after them */
    order[utils] = 100;
    assoc_set( "TXT", "EDIT" );
}

/* Where the file is: 1, or 0 for no file at all.  A test run - DSTEST
   set and the keys coming from a file - has none, so that every test
   starts from the same place and the user's own settings are left
   alone; DSINI names a file for a test, or for anybody, to use
   instead. */
static int ini_path( char *path, u32 max )
{
    const char *named = sys_getenv( "DSINI" );
    const char *test = sys_getenv( "DSTEST" );

    if ( named && *named ) {
        str_fit( path, named, max );
        return 1;
    }
    if ( test && *test && !sys_stdin_is_console() ) {
        return 0;
    }
    str_fit( path, sys_program_dir(), max );
    str_catn( path, "DOSSHELL.INI", max );
    return 1;
}

void ini_load( void )
{
    char path[PATH_MAX], *key, *value;
    u32 handle, size = 0, done = 0;
    int section = 0, have_groups = 0;

    items = (PITEM *)xalloc( (u32)(MAX_ITEMS + 1) * sizeof( PITEM ) );
    mem_set( items, 0, (u32)(MAX_ITEMS + 1) * sizeof( PITEM ) );
    nitems = 1;
    used[0] = 1;
    items[0].is_group = 1;
    items[0].parent = -1;
    str_cpy( items[0].title, "Main" );

    if ( ini_path( path, sizeof( path ) ) && sys_open_read( path, &handle ) == 0 ) {
        if ( sys_file_size( handle, &size ) == 0 && size > 0 && size < 0x40000UL ) {
            ini_buf = (char *)try_alloc( size + 1 );
            if ( ini_buf ) {
                sys_read( handle, ini_buf, size, &done );
            }
        }
        sys_close( handle );
    }
    sys_crit_seen();
    ini_len = done;
    ini_pos = 0;
    while ( ini_buf && ini_next( &key, &value ) ) {
        if ( key[0] == '[' ) {
            section = str_icmp( value, "savestate" ) == 0 ? 1
                      : str_icmp( value, "associations" ) == 0 ? 2
                      : str_icmp( value, "programstarter" ) == 0 ? 3 : 0;
        } else if ( section == 1 ) {
            parse_state( key, value );
        } else if ( section == 2 ) {
            assoc_set( key, value );
        } else if ( section == 3 && str_icmp( key, "group" ) == 0 ) {
            ini_open( value );
            parse_group( 0 );
            have_groups = 1;
        }
    }
    xfree( ini_buf );
    ini_buf = NULL;
    if ( !have_groups ) {
        default_items();
    }
}

/* ------------------------------------------------------------------ */
/* DOSSHELL.INI: writing                                               */
/* ------------------------------------------------------------------ */

static u32 out_handle;
static int out_failed;

static void out( const char *text )
{
    u32 done;

    if ( !out_failed && sys_write( out_handle, text, str_len( text ), &done ) != 0 ) {
        out_failed = 1;
    }
}

static void out_line( int indent, const char *key, const char *value )
{
    while ( indent-- > 0 ) {
        out( "    " );
    }
    out( key );
    if ( value ) {
        out( " = " );
        out( value );
    }
    out( "\r\n" );
}

static void out_flag( int indent, const char *key, int on )
{
    out_line( indent, key, on ? "enabled" : "disabled" );
}

static void write_item( int item, int indent )
{
    PITEM *it = &items[item];
    int list[MAX_ITEMS], count, index;
    char mark[3];

    out_line( indent, it->is_group ? "group =" : "program =", NULL );
    out_line( indent, "{", NULL );
    out_line( indent + 1, "title", it->title );
    if ( it->help[0] ) {
        out_line( indent + 1, "help", it->help );
    }
    if ( it->password[0] ) {
        out_line( indent + 1, "password", it->password );
    }
    if ( it->is_group ) {
        count = kids( item, list );
        for ( index = 0; index < count; index++ ) {
            write_item( list[index], indent + 1 );
        }
    } else {
        out_line( indent + 1, "command", it->cmds );
        if ( it->dir[0] ) {
            out_line( indent + 1, "directory", it->dir );
        }
        out_flag( indent + 1, "pause", it->pause );
        for ( index = 0; index < MAX_PROMPTS; index++ ) {
            if ( it->prompt[index].title[0] == 0 && it->prompt[index].info[0] == 0
                 && it->prompt[index].label[0] == 0 && it->prompt[index].value[0] == 0 ) {
                continue;
            }
            mark[0] = '%';
            mark[1] = (char)('1' + index);
            mark[2] = 0;
            out_line( indent + 1, "dialog =", NULL );
            out_line( indent + 1, "{", NULL );
            out_line( indent + 2, "parameter", mark );
            out_line( indent + 2, "title", it->prompt[index].title );
            out_line( indent + 2, "info", it->prompt[index].info );
            out_line( indent + 2, "prompt", it->prompt[index].label );
            out_line( indent + 2, "default", it->prompt[index].value );
            out_line( indent + 1, "}", NULL );
        }
    }
    out_line( indent, "}", NULL );
}

void ini_save( void )
{
    static const char *const views[] = { "single", "dual", "all", "program/file", "program" };
    static const char *const sorts[] = { "name", "extension", "date", "size", "diskorder" };
    char path[PATH_MAX];
    int index;

    if ( !ini_path( path, sizeof( path ) ) ) {
        return;
    }
    sys_crit_seen();
    if ( sys_create( path, &out_handle ) != 0 ) {
        sys_crit_seen();
        return;                         /* a disk that cannot be written: no matter */
    }
    out_failed = 0;
    out( "; PM-DOS Shell settings.  The shell writes this file when it is left.\r\n" );
    out( "[savestate]\r\n" );
    out_line( 0, "display", dmodes[opt.display].tag );
    out_line( 0, "scheme", scheme_name( opt.scheme ) );
    out_line( 0, "view", views[opt.view] );
    out_line( 0, "sortkey", sorts[opt.sort_key] );
    out_line( 0, "sortorder", opt.sort_desc ? "descending" : "ascending" );
    out_line( 0, "filter", opt.filter );
    out_flag( 0, "hidden", opt.show_hidden );
    out_flag( 0, "confirmdelete", opt.confirm_delete );
    out_flag( 0, "confirmreplace", opt.confirm_replace );
    out_flag( 0, "confirmmouse", opt.confirm_mouse );
    out_flag( 0, "selectacross", opt.select_across );
    out( "\r\n[associations]\r\n" );
    for ( index = 0; index < nassoc; index++ ) {
        out_line( 0, assoc[index].ext, assoc[index].program );
    }
    out( "\r\n[programstarter]\r\n" );
    write_item( 0, 0 );
    sys_close( out_handle );
    sys_crit_seen();
}
