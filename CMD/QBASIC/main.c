/*
 * MAIN.C - QBASIC.EXE: the command line, the editor's windows, its
 * commands, and running and stopping the program.
 *
 *   QBASIC [/B] [/EDITOR] [/G] [/H] [/MBF] [/NOHI] [[/RUN] [path]file]
 *
 * THE SCREEN IS QBASIC 1.1's: the menu bar, the program's window with
 * the Immediate window under it, the status line with the cursor's line
 * and column at the right; the help window above them when it is open.
 * A run puts the program's own screen up (CONS.C and GFX.C keep it in
 * memory the whole time, so F4 can show it again) and the editor comes
 * back when the program ends, stops or fails.
 *
 * STOPPED PART WAY - by Ctrl+Break, STOP, a breakpoint, a step or an
 * error - the program keeps everything, and Continue, Step and
 * Procedure Step go on from the statement lit in the window.  Changing
 * the program then means starting again, and QBasic says so first.
 *
 * QBasic needs a coprocessor here: the numbers are the x87's, and
 * PM-DOS does not emulate one.
 */
#include "ide.h"

VIEW  views[3];
int   nviews = 1;
int   active;                   /* 0, 1 the editor's windows, 2 Immediate, 3 help */

static char       *clip;
static u32         clip_len;
static const char *status_hint;
static int         status_wide;
static int         opt_lines, opt_run, opt_editor;
static int         maximized;   /* Ctrl+F10: the active window has the screen */
static int         ui_ready;
static DOC        *imm_doc;     /* the Immediate window's lines */
static int         trace_on;
static int         grow[3];     /* Alt+Plus and Alt+Minus: rows taken from the default */

/* the program run or stopped */
static PROGRAM    *prog;
static int         stopped;     /* a run is part way: Continue goes on */
static int         need_restart;    /* changed since it was compiled */
static int         stop_text = -1;  /* where the current statement is */
static int         idle;        /* prog is only the Immediate window's: not started */

int opt_mbf;                    /* /MBF: MKS$ and friends in Microsoft's old format */
int ide_trace = 1;              /* TRON lights the statements, as Trace On does */

static void say( const char *text )
{
    sys_stdout( text, str_len( text ) );
}

const char *err_text( int rc )
{
    switch ( rc ) {
    case ERR_NOFILE:    return "File not found";
    case ERR_NOPATH:    return "Path not found";
    case ERR_NOHANDLES: return "Too many files";
    case ERR_ACCESS:    return "Permission denied";
    case ERR_BADDRIVE:  return "Bad file name";
    case ERR_DISKFULL:  return "Disk full";
    case 19:            return "Disk write-protected";
    case 21:            return "Disk not ready";
    case ERR_NOMEM:
    case -1:            return "Out of memory";
    }
    return "Device I/O error";
}

void out_of_memory( void )
{
    if ( ui_ready ) {
        msg_box( "Out of memory", MB_OK, 1 );
    } else {
        say( "Not enough memory for QBasic.\r\n" );
        sys_exit( 7 );
    }
}

void app_status( const char *text )
{
    status_hint = text;
    status_wide = 0;
}

void app_status_wide( const char *text )
{
    status_hint = text;
    status_wide = 1;
}

VIEW *app_view( void )
{
    return active < 3 ? &views[active] : &views[0];
}

int clip_has( void )
{
    return clip != NULL && clip_len > 0;
}

void clip_set( char *text, u32 len )
{
    xfree( clip );
    clip = text;
    clip_len = len;
}

const char *clip_get( u32 *len )
{
    *len = clip_len;
    return clip;
}

int app_running( void )
{
    return stopped && !need_restart;
}

/* the title of a text's window: the program's name, and a procedure's */
const char *app_title( int text, char *buf, u32 max )
{
    str_cpyt( buf, ptext.name, max );           /* a long name: as much as fits */
    if ( text > 0 && text < ptext.ntexts ) {
        str_catn( buf, ":", max );
        str_catn( buf, ptext.texts[text]->pname, max );
    }
    return buf;
}

/* the statement the program is stopped at, or NULL (a program with no
   statements has none) */
static STMTLOC *stop_loc( void )
{
    if ( prog == NULL || vm.stmt >= prog->nlocs ) {
        return NULL;
    }
    return &prog->locs[vm.stmt];
}

int app_current( int text, u32 line, int *col, int *len )
{
    STMTLOC *loc;

    if ( !stopped || prog == NULL || need_restart ) {
        return 0;
    }
    loc = stop_loc();
    if ( loc == NULL || loc->text != text || loc->line != line ) {
        return 0;
    }
    *col = loc->col;
    *len = loc->len;
    return 1;
}

int app_checked( int cmd )
{
    return (cmd == CMD_TRACE && trace_on) || (cmd == CMD_SYNTAX && syntax_check);
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

static int is_shown( int index )
{
    if ( maximized ) {
        return index == active;
    }
    return index == 2 || index < nviews;
}

void app_layout( void )
{
    int top = 1, bottom = scr_rows - 2, imm, edit, index, help = help_rows(), avail;

    for ( index = 0; index < 3; index++ ) {
        views[index].bars = 0;
    }
    if ( maximized && active < 3 ) {
        views[active].row = 1;
        views[active].height = bottom;
    } else {
        if ( help ) {
            top = 1 + help;
        }
        avail = bottom - top + 1;
        if ( help ) {
            imm = avail >= 5 ? 3 : avail >= 4 ? 2 : 1;
        } else {
            imm = 3 + grow[2];
            if ( imm < 1 ) {
                imm = 1;
            }
            if ( imm > avail - 2 ) {
                imm = avail - 2;
            }
        }
        edit = avail - imm;
        if ( nviews == 2 ) {
            int first = edit / 2 + grow[0];
            if ( first < 2 ) {
                first = 2;
            }
            if ( first > edit - 2 ) {
                first = edit - 2;
            }
            views[0].row = top;
            views[0].height = first;
            views[1].row = top + first;
            views[1].height = edit - first;
        } else {
            views[0].row = top;
            views[0].height = edit;
        }
        views[2].row = top + edit;
        views[2].height = imm;
    }
    if ( active < 3 && scroll_bars && views[active].height >= 5 ) {
        views[active].bars = 1;
    }
    for ( index = 0; index < 3; index++ ) {
        if ( views[index].doc ) {
            view_keep_visible( &views[index] );
        }
    }
}

static void draw_status( void )
{
    char right[16], num[12];
    const char *left;
    int row = scr_rows - 1, len, pad, wide;
    VIEW *view;

    if ( status_hint ) {
        left = status_hint;
        wide = status_wide || (int)str_len( left ) > 61;
    } else if ( active == 3 ) {
        left = " <Shift+F1=Help> <F6=Window> <Esc=Cancel> <Ctrl+F1=Next> <Alt+F1=Back>";
        scr_fill( row, 0, scr_cols, ' ', pal.status );
        scr_put( row, 0, left, pal.status );
        scr_ch( row, 72, 0xB3, pal.status_right );
        scr_fill( row, 73, 6, ' ', pal.status_right );
        return;
    } else if ( active == 2 ) {
        left = " <Shift+F1=Help> <F6=Window> <Enter=Execute Line>";
        wide = 0;
    } else if ( stopped && !need_restart ) {
        left = " <Shift+F1=Help> <F5=Continue> <F9=Toggle Bkpt> <F8=Step>";
        wide = 0;
    } else {
        left = " <Shift+F1=Help> <F6=Window> <F2=Subs> <F5=Run> <F8=Step>";
        wide = 0;
    }
    scr_fill( row, 0, scr_cols, ' ', pal.status );
    scr_put( row, 0, left, pal.status );
    if ( wide ) {
        return;
    }
    scr_ch( row, 62, 0xB3, pal.status_right );
    scr_fill( row, 63, scr_cols - 63, ' ', pal.status_right );
    view = active < 3 ? &views[active] : &views[0];
    /* "00001:001": the line in five digits, the column in three */
    num_fmt_l( (s32)(view->line + 1), num );
    len = (int)str_len( num );
    for ( pad = 0; pad < 5 - len; pad++ ) {
        right[pad] = '0';
    }
    str_cpy( right + pad, num );
    str_catn( right, ":", sizeof( right ) );
    num_fmt_l( (s32)(view->col + 1), num );
    len = (int)str_len( num );
    for ( pad = 0; pad < 3 - len; pad++ ) {
        str_catn( right, "0", sizeof( right ) );
    }
    str_catn( right, num, sizeof( right ) );
    scr_put( row, 70, right, pal.status_right );
}

void app_draw( void )
{
    int index;

    menu_draw_bar( -1, key_alt_held(), 0 );
    if ( help_shown() && !(maximized && active < 3) ) {
        help_draw( active == 3 );
    }
    for ( index = 0; index < 3; index++ ) {
        if ( is_shown( index ) ) {
            view_draw( &views[index], index == active );
        }
    }
    draw_status();
}

static void place_cursor( void )
{
    if ( active == 3 ) {
        help_draw( 1 );                         /* puts the cursor */
        return;
    }
    view_place_cursor( &views[active] );
}

static void frame( void )
{
    app_draw();
    place_cursor();
    scr_flush();
}

/* ------------------------------------------------------------------ */
/* the program's texts                                                 */
/* ------------------------------------------------------------------ */

static void show_text_everywhere( int text )
{
    int index;

    for ( index = 0; index < 2; index++ ) {
        views[index].doc = NULL;
        view_show_text( &views[index], text );
    }
}

/* a new program, or one loaded: every window back on the module */
static void program_changed_whole( void )
{
    int index;

    for ( index = 0; index < ptext.ntexts; index++ ) {
        ptext.texts[index]->at_line = 0;
        ptext.texts[index]->at_col = 0;
        ptext.texts[index]->at_top = 0;
        ptext.texts[index]->at_left = 0;
    }
    show_text_everywhere( 0 );
    app_layout();
}

/* the line typed on, checked before anything else happens: 0 = it has
   an error, and the command does not go ahead */
static int settle( void )
{
    if ( active < 2 && views[active].dirty ) {
        return view_leave_line( &views[active] );
    }
    return 1;
}

static void end_run( void );

static int ask_save( void );
static void app_exit( void );

static int save_program( int ask_name )
{
    char path[PATH_MAX];
    int rc;
    u32 count;

    str_cpy( path, ptext.path );
    if ( ask_name || path[0] == 0 ) {
        if ( !dlg_saveas( path ) ) {
            return 0;
        }
    }
    count = ptext.texts[0]->count;
    rc = prog_save( path );
    if ( ptext.texts[0]->count > count ) {
        /* DECLARE lines went in at the top of the module: its windows
           keep their places in the text, and a stopped program's lines
           are not the ones it was compiled from */
        u32 added = ptext.texts[0]->count - count;
        int i;
        for ( i = 0; i < 2; i++ ) {
            if ( views[i].text == 0 ) {
                views[i].top += added;
                views[i].line += added;
                views[i].sel_line += added;
            }
        }
        app_edited();
    }
    if ( rc != 0 ) {
        msg_box( err_text( rc ), MB_OK, 1 );
        return 0;
    }
    return 1;
}

/* changes not saved: Yes saves, No goes on without, Cancel stops.
   -> 0 to stop */
static int ask_save( void )
{
    int answer;

    if ( !ptext.modified ) {
        return 1;
    }
    answer = msg_box( "The program has changes that are not saved. Save them now?", MB_YESNOCANCEL, 1 );
    if ( answer == ID_YES ) {
        return save_program( 0 );
    }
    return answer == ID_NO;
}

static void new_program( void )
{
    end_run();
    prog_clear();
    names_reset();
    program_changed_whole();
}

static int load_program( const char *path )
{
    int rc = prog_load( path );

    if ( rc != 0 ) {
        /* a file that would not open leaves the program as it was;
           prog_load() empties it after a failure part way */
        program_changed_whole();
        if ( ui_ready ) {
            msg_box( err_text( rc ), MB_OK, 1 );
        }
        return 0;
    }
    view_learn_program();
    ptext.modified = 0;
    program_changed_whole();
    return 1;
}

/* ------------------------------------------------------------------ */
/* running                                                             */
/* ------------------------------------------------------------------ */

static void end_run( void )
{
    if ( prog != NULL ) {
        if ( stopped || idle ) {
            vm_end();
        }
        comp_free( prog );
        prog = NULL;
    }
    stopped = 0;
    idle = 0;
    need_restart = 0;
    stop_text = -1;
}

/* the program has ended (END, SYSTEM): its variables stay for the
   Immediate window until the program is changed or run again */
static void finish_run( void )
{
    if ( prog == NULL || !(stopped || idle) || need_restart ) {
        end_run();
        return;
    }
    vm_finish();
    stopped = 0;
    idle = 1;
    stop_text = -1;
}

void app_edited( void )
{
    if ( stopped ) {
        need_restart = 1;
    } else if ( idle ) {
        end_run();                              /* the Immediate window's names go with the old text */
    }
}

/* every breakpoint on its statements: a line's mark is the first of
   the statements that start on it */
static void sync_breakpoints( void )
{
    u32 index;
    STMTLOC *loc;
    LINE *line;

    for ( index = 0; index < prog->nlocs; index++ ) {
        loc = &prog->locs[index];
        loc->flags &= (u16)~SL_BREAK;
        if ( loc->text < ptext.ntexts && loc->line < ptext.texts[loc->text]->count ) {
            line = doc_line( ptext.texts[loc->text], loc->line );
            if ( line->flags & LF_BREAK ) {
                loc->flags |= SL_BREAK;
            }
        }
    }
}

/* the window onto the text, line and column given, a token selected */
static void show_place( int text, u32 line, int col, int len, int select )
{
    VIEW *view;
    LINE *ln;

    if ( active > 1 ) {
        active = 0;
    }
    help_close();
    maximized = 0;
    app_layout();
    view = &views[active];
    if ( view->text != text ) {
        view_show_text( view, text );
    }
    if ( line >= view->doc->count ) {
        line = view->doc->count - 1;
    }
    ln = doc_line( view->doc, line );
    view->sel = 0;
    if ( select && len > 0 ) {
        view->sel = 1;
        view->sel_line = line;
        view->sel_col = doc_col_of( view->doc, ln, (u32)col );
        view_goto( view, line, doc_col_of( view->doc, ln, (u32)(col + len) ), 1 );
    } else {
        view_goto( view, line, doc_col_of( view->doc, ln, (u32)col ), 0 );
    }
    view->want = view->col;
}

/* compiled, and the run made ready: 0 if it has an error (shown) */
static int start_run( void )
{
    CERR err;

    end_run();
    prog = comp_program( &ptext, &err );
    if ( prog == NULL ) {
        show_place( err.text, err.line, err.col, err.len > 0 ? err.len : 1, 1 );
        msg_box( err.msg, MB_OK, 1 );
        views[active].sel = 0;
        return 0;
    }
    if ( !vm_begin( prog ) ) {
        comp_free( prog );
        prog = NULL;
        msg_box( "Out of memory", MB_OK, 1 );
        return 0;
    }
    con_reset();
    stopped = 1;
    need_restart = 0;
    return 1;
}

/* the editor back: its screen, the mouse */
static void back_to_editor( void )
{
    gfx_restore();
    scr_invalidate();
    mouse_hide( 0 );
}

/* the program's screen until a key; "prompt" first, at its foot */
static void output_until_key( const char *prompt )
{
    int key;

    mouse_hide( 1 );
    con_show();
    if ( prompt ) {
        con_end_prompt( prompt );
    }
    con_flush_keys();
    do {
        key = key_get();
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key == K_DUMP ) {
            scr_dump_output();
        }
    } while ( key == K_TICK || key == K_ALTTAP || key == K_DUMP
              || (key == K_MOUSE && mouse.kind != ME_DOWN) );
    back_to_editor();
}

static void show_stop( void )
{
    STMTLOC *loc = stop_loc();

    if ( loc == NULL ) {
        stop_text = -1;
        return;
    }
    stop_text = loc->text;
    show_place( loc->text, loc->line, loc->col, 0, 0 );
}

/* how: GO_RUN, GO_STEP, GO_PSTEP.  From a stop, or from the top */
static u32 run_at;                      /* RUN n from the Immediate window */

/* RUN n: the new run starts at the line's code, not the top */
static void run_from( u32 at )
{
    u32 i;

    if ( at == 0 || prog == NULL ) {
        return;
    }
    for ( i = 0; i < prog->nlocs; i++ ) {
        if ( at - 1 < prog->code_len && prog->locs[i].pc == at - 1 ) {
            vm.pc = prog->code + at - 1;
            vm.stmt = i;
            break;
        }
    }
}

static void run_program( int how, int fresh )
{
    int why;
    STMTLOC *loc;

    if ( !settle() ) {
        return;
    }
    if ( stopped && need_restart && !fresh ) {
        if ( msg_box( "The program has been changed, so it starts again from the beginning.",
                      MB_OKCANCEL, 1 ) != ID_OK ) {
            return;
        }
        fresh = 1;
    }
    if ( fresh || !stopped ) {
        if ( !start_run() ) {
            vm_common_drop();
            return;
        }
        vm_common_give();                       /* CHAIN's COMMON, if one came */
        run_from( run_at );
    }
    for ( ;; ) {
        sync_breakpoints();
        mouse_hide( 1 );
        con_show();
        if ( (trace_on || vm.tron) && how == GO_RUN ) {
            /* each statement lit as it comes: a step at a time (Trace On,
               or TRON in the program until its TROFF) */
            for ( ;; ) {
                why = vm_go( GO_STEP );
                if ( why != RUN_STEP ) {
                    break;
                }
                if ( !trace_on && !vm.tron ) {
                    why = vm_go( GO_RUN );      /* TROFF: full speed again */
                    break;
                }
                back_to_editor();
                show_stop();
                frame();
                if ( key_get_timed( 5 ) == K_BREAK ) {
                    why = RUN_BREAK;
                    break;
                }
                mouse_hide( 1 );
                con_show();
            }
        } else {
            why = vm_go( how );
        }
        if ( why == RUN_STEP && how == GO_RUN ) {
            continue;                           /* TRON: from here a step at a time */
        }
        if ( why == RUN_CHAIN ) {
            char next[PATH_MAX];
            int named = vm.chain_to[0] != 0;
            u32 at = vm.chain_at;
            str_cpy( next, vm.chain_to );
            file_flush_all();
            if ( vm.chain_keep ) {
                vm_common_take();               /* CHAIN: the COMMON and the files go on */
            }
            end_run();
            if ( named ) {
                /* the editor's screen first: a question or a message may
                   come, and unsaved changes are asked about */
                back_to_editor();
                if ( !ask_save() || !load_program( next ) ) {
                    vm_common_drop();
                    return;
                }
            }
            if ( !start_run() ) {
                vm_common_drop();
                back_to_editor();
                return;
            }
            vm_common_give();
            run_from( at );
            how = GO_RUN;
            continue;
        }
        break;
    }
    file_flush_all();
    con_flush();
    switch ( why ) {
    case RUN_END:
    case RUN_SYSTEM:
        if ( opt_run && why == RUN_SYSTEM ) {
            /* QBASIC /RUN and the program says SYSTEM: back to DOS */
            mouse_done();
            con_exit();
            sys_exit( 0 );
        }
        finish_run();
        output_until_key( "Press any key to continue" );
        return;
    case RUN_ERROR:
        back_to_editor();
        if ( vm.fatal_data != NULL ) {
            show_place( vm.fatal_data->in_text, vm.fatal_data->line, vm.fatal_data->col, 1, 1 );
        } else {
            loc = &prog->locs[vm.fatal_stmt];
            show_place( loc->text, loc->line, loc->col, loc->len, 1 );
        }
        msg_box( err_message( vm.fatal ), MB_OK, 1 );
        views[active].sel = 0;
        show_stop();
        return;
    default:                                    /* STOP, Break, a breakpoint, a step */
        back_to_editor();
        show_stop();
        return;
    }
}

/* F7: the program runs on - from the start, if it is not stopped - to
   the first statement on the cursor's line */
static void run_to_cursor( void )
{
    VIEW *view = &views[active < 2 ? active : 0];
    u32 index;

    if ( !settle() ) {
        return;
    }
    if ( !stopped || need_restart ) {
        if ( stopped && msg_box( "The program has been changed, so it starts again from the beginning.",
                                 MB_OKCANCEL, 1 ) != ID_OK ) {
            return;
        }
        if ( !start_run() ) {
            return;
        }
    }
    for ( index = 0; index < prog->nlocs; index++ ) {
        if ( prog->locs[index].text == view->text && prog->locs[index].line == view->line ) {
            prog->locs[index].flags |= SL_HERE;
        }
    }
    run_program( GO_RUN, 0 );
    if ( prog != NULL ) {
        for ( index = 0; index < prog->nlocs; index++ ) {
            prog->locs[index].flags &= (u16)~SL_HERE;
        }
    }
}

/* Set Next Statement: the first statement on the cursor's line, in the
   procedure the program is stopped in */
static void set_next( void )
{
    VIEW *view = &views[active < 2 ? active : 0];
    u32 index;
    STMTLOC *loc, *now;

    if ( !stopped || need_restart ) {
        return;
    }
    now = stop_loc();
    if ( now == NULL ) {
        return;
    }
    for ( index = 0; index < prog->nlocs; index++ ) {
        loc = &prog->locs[index];
        if ( loc->text == view->text && loc->line == view->line ) {
            if ( loc->proc != now->proc ) {
                msg_box( "The statement has to be in the procedure the program is stopped in.", MB_OK, 1 );
                return;
            }
            vm.pc = prog->code + loc->pc;
            vm.stmt = index;
            vm.skip_stop = 1;
            return;
        }
    }
    msg_box( "There is no statement on that line.", MB_OK, 1 );
}

/* The Immediate window's lines run in the program: where it stopped, or
   in the program made ready but not started, so that what one line sets
   is there for the next and the program's procedures can be called.  A
   program that does not compile gives way to an empty one. */
static int immediate_context( void )
{
    PROGTEXT *save;
    CERR err;

    if ( stopped || idle ) {
        return 1;
    }
    end_run();
    prog = comp_program( &ptext, &err );
    if ( prog == NULL ) {
        save = (PROGTEXT *)try_alloc( sizeof( PROGTEXT ) );
        if ( save != NULL ) {
            mem_cpy( save, &ptext, sizeof( PROGTEXT ) );
            ptext.texts[0] = doc_new();
            ptext.ntexts = 1;
            if ( ptext.texts[0] != NULL ) {
                prog = comp_program( &ptext, &err );
                doc_free( ptext.texts[0] );
            }
            mem_cpy( &ptext, save, sizeof( PROGTEXT ) );
            xfree( save );
        }
    }
    if ( prog == NULL || !vm_begin( prog ) ) {
        comp_free( prog );
        prog = NULL;
        out_of_memory();
        return 0;
    }
    idle = 1;
    return 1;
}

/* a line of the Immediate window, run: 0 if it failed, and the cursor
   stays on it */
static int immediate( void )
{
    VIEW *view = &views[2];
    LINE *ln = doc_line( view->doc, view->line );
    u32 code_len, nlocs, narrinit, clears;
    int proc = -1, why, shown;
    s32 entry;
    CERR err;

    if ( ln->len == 0 ) {
        return 1;
    }
    if ( !immediate_context() ) {
        return 0;
    }
    if ( stopped && stop_loc() != NULL ) {
        proc = (int)stop_loc()->proc - 1;
    }
    code_len = prog->code_len;
    nlocs = prog->nlocs;
    narrinit = prog->narrinit;
    clears = vm.clears;
    entry = comp_immediate( prog, proc, ln->text, ln->len, vm.gcap, &err );
    if ( entry < 0 ) {
        msg_box( err.msg, MB_OK, 1 );
        return 0;
    }
    /* the output screen comes up only if the line writes on it */
    mouse_hide( 1 );
    con_show_on_output();
    why = vm_immediate( (u32)entry, narrinit );
    file_flush_all();
    con_flush();
    comp_immediate_done( prog, code_len, nlocs );
    shown = con_output_shown();
    if ( vm.clears != clears && why == RUN_IMMEDIATE ) {
        /* CLEAR: the stopped program has no stack or variables left to
           go on with */
        why = RUN_END;
    }
    switch ( why ) {
    case -1:
        back_to_editor();
        out_of_memory();
        return 0;
    case RUN_ERROR:
        back_to_editor();
        msg_box( err_message( vm.fatal ), MB_OK, 1 );
        if ( vm.clears != clears ) {
            finish_run();
        }
        return 0;
    case RUN_BREAK:
        back_to_editor();
        if ( vm.clears != clears ) {
            finish_run();
        }
        return 1;
    case RUN_IMMGOTO:
        /* GOTO or RETURN: the program goes on from there */
        stopped = 1;
        idle = 0;
        run_program( GO_RUN, 0 );
        return 1;
    case RUN_SYSTEM:
        /* SYSTEM typed here leaves QBasic */
        back_to_editor();
        finish_run();
        app_exit();
        return 1;
    case RUN_CHAIN:
        {
            /* RUN, or RUN or CHAIN with a file: that program, from the top */
            char next[PATH_MAX];
            int named = vm.chain_to[0] != 0;
            str_cpy( next, vm.chain_to );
            run_at = need_restart ? 0 : vm.chain_at; /* changed: the line's code has moved */
            if ( vm.chain_keep ) {
                vm_common_take();
            }
            end_run();
            back_to_editor();
            if ( named && (!ask_save() || !load_program( next )) ) {
                vm_common_drop();
                run_at = 0;
                return 1;
            }
            run_program( GO_RUN, 1 );
            run_at = 0;
        }
        return 1;
    case RUN_END:
        finish_run();
        break;
    }
    if ( shown ) {
        output_until_key( "Press any key to continue" );
    } else {
        back_to_editor();
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Make EXE File: the program compiled, PACK.C's block of it put       */
/* behind a copy of QBRUN.EXE                                          */
/* ------------------------------------------------------------------ */

/* QBRUN.EXE in that directory -> its path in "out", 1 if it is there */
static int runtime_in( const char *dir, u32 dlen, char *out )
{
    u32 handle;

    if ( dlen + 12 >= PATH_MAX ) {
        return 0;
    }
    mem_cpy( out, dir, dlen );
    out[dlen] = 0;
    if ( dlen && out[dlen - 1] != '\\' && out[dlen - 1] != ':' ) {
        str_catn( out, "\\", PATH_MAX );
    }
    str_catn( out, "QBRUN.EXE", PATH_MAX );
    if ( sys_open_read( out, &handle ) != 0 ) {
        return 0;
    }
    sys_close( handle );
    return 1;
}

/* QBRUN.EXE is looked for as QBASIC.HLP is: beside PM-DOS's shell, where
   SETUP puts QBASIC (a native program is not told its own path), where
   the help is, in the current directory, and along the PATH */
static int find_runtime( char *out )
{
    const char *shell = sys_shell(), *scan, *last = NULL, *path, *end;

    for ( scan = shell; *scan; scan++ ) {
        if ( *scan == '\\' || *scan == ':' ) {
            last = scan;
        }
    }
    if ( last != NULL && runtime_in( shell, (u32)(last - shell + 1), out ) ) {
        return 1;
    }
    if ( help_path[0] && runtime_in( help_path, str_len( help_path ), out ) ) {
        return 1;
    }
    if ( runtime_in( ".", 1, out ) ) {
        return 1;
    }
    path = sys_getenv( "PATH" );
    while ( path && *path ) {
        for ( end = path; *end && *end != ';'; end++ ) {
        }
        if ( end > path && runtime_in( path, (u32)(end - path), out ) ) {
            return 1;
        }
        path = *end ? end + 1 : end;
    }
    return 0;
}

/* a whole file into memory -> it (xfree it), or NULL and *rc */
static u8 *read_file( const char *path, u32 *len, int *rc )
{
    u32 handle, size, done;
    u8 *buf = NULL;

    *rc = sys_open_read( path, &handle );
    if ( *rc != 0 ) {
        return NULL;
    }
    *rc = sys_file_size( handle, &size );
    if ( *rc == 0 ) {
        buf = (u8 *)try_alloc( size ? size : 1 );
        if ( buf == NULL ) {
            *rc = ERR_NOMEM;
        }
    }
    if ( *rc == 0 ) {
        *rc = sys_read( handle, buf, size, &done );
        if ( *rc == 0 && done != size ) {
            *rc = ERR_ACCESS;
        }
    }
    sys_close( handle );
    if ( *rc != 0 ) {
        xfree( buf );
        return NULL;
    }
    *len = size;
    return buf;
}

/* the runtime, then the program: a file that is not whole is deleted */
static int write_exe( const char *path, const u8 *runtime, u32 rlen, const u8 *blob, u32 blen )
{
    u32 handle, done;
    int rc, closed;

    rc = sys_create( path, &handle );
    if ( rc != 0 ) {
        return rc;
    }
    rc = sys_write( handle, runtime, rlen, &done );
    if ( rc == 0 ) {
        rc = sys_write( handle, blob, blen, &done );
    }
    closed = sys_close( handle );
    if ( rc == 0 ) {
        rc = closed;
    }
    if ( rc != 0 ) {
        sys_delete( path );
    }
    return rc;
}

/* "name" without its directory or its extension */
static void base_name( const char *path, char *out, u32 max )
{
    const char *scan, *start = path;
    char *dot;

    for ( scan = path; *scan; scan++ ) {
        if ( *scan == '\\' || *scan == ':' ) {
            start = scan + 1;
        }
    }
    str_cpyt( out, start, max );
    dot = str_rchr( out, '.' );
    if ( dot != NULL ) {
        *dot = 0;
    }
}

static void make_exe( void )
{
    char exe[PATH_MAX], runtime[PATH_MAX], module[PATH_MAX], full_exe[PATH_MAX], full_rt[PATH_MAX];
    const char *last;
    char *dot;
    int answer, debug, rc;
    u32 blen, rlen, at, from;
    u8 *blob, *body;
    PROGRAM *made;
    CERR err;

    /* the name it offers: the program's file, .EXE for its extension */
    exe[0] = 0;
    if ( ptext.path[0] ) {
        str_cpy( exe, ptext.path );
        last = str_rchr( exe, '\\' );
        dot = str_rchr( last ? last : exe, '.' );
        if ( dot != NULL ) {
            *dot = 0;
        }
        str_catn( exe, ".EXE", sizeof( exe ) );
    }
    answer = dlg_makeexe( exe, &debug );
    if ( answer == 0 ) {
        return;
    }
    /* the name as typed, without the blanks round it; .EXE if it has
       no extension */
    for ( from = 0; exe[from] == ' '; from++ ) {
    }
    for ( at = 0; exe[from + at]; at++ ) {
        exe[at] = exe[from + at];
    }
    while ( at > 0 && exe[at - 1] == ' ' ) {
        at--;
    }
    exe[at] = 0;
    if ( exe[0] == 0 ) {
        msg_box( "Bad file name", MB_OK, 1 );
        return;
    }
    last = str_rchr( exe, '\\' );
    if ( str_chr( last ? last : exe, '.' ) == NULL ) {
        str_catn( exe, ".EXE", sizeof( exe ) );
    }
    base_name( ptext.path[0] ? ptext.path : exe, module, sizeof( module ) );

    made = comp_program( &ptext, &err );
    if ( made == NULL ) {
        show_place( err.text, err.line, err.col, err.len > 0 ? err.len : 1, 1 );
        msg_box( err.msg, MB_OK, 1 );
        views[active].sel = 0;
        return;
    }
    blob = pack_program( made, (opt_mbf ? PK_MBF : 0) | (debug ? PK_DEBUG : 0), module, &blen );
    comp_free( made );
    if ( blob == NULL ) {
        out_of_memory();
        return;
    }
    if ( !find_runtime( runtime ) ) {
        xfree( blob );
        msg_box2( "QBRUN.EXE cannot be found.", "It has to be in QBasic's directory or on the PATH.", MB_OK, 0 );
        return;
    }
    body = read_file( runtime, &rlen, &rc );
    if ( body == NULL ) {
        xfree( blob );
        msg_box( err_text( rc ), MB_OK, 1 );
        return;
    }
    if ( !pack_runtime_ok( body, rlen ) ) {
        xfree( blob );
        xfree( body );
        msg_box( "QBRUN.EXE does not belong to this version of QBasic.", MB_OK, 0 );
        return;
    }
    if ( sys_truename( exe, full_exe, 0 ) == 0 && sys_truename( runtime, full_rt, 0 ) == 0
      && str_icmp( full_exe, full_rt ) == 0 ) {
        rc = ERR_ACCESS;                        /* QBRUN.EXE itself is not written over */
    } else {
        rc = write_exe( exe, body, rlen, blob, blen );
    }
    xfree( blob );
    xfree( body );
    if ( rc != 0 ) {
        msg_box( err_text( rc ), MB_OK, 1 );
        return;
    }
    if ( answer == ID_MAKEEXIT ) {
        app_exit();
    }
}

/* ------------------------------------------------------------------ */
/* the commands                                                        */
/* ------------------------------------------------------------------ */

int app_can( int cmd )
{
    VIEW *view = app_view();

    switch ( cmd ) {
    case CMD_CUT:
    case CMD_CLEAR:
    case CMD_COPY:
        return active < 3 && view_has_sel( view );
    case CMD_PASTE:
        return active < 3 && clip_has();
    case CMD_SETNEXT:
        return stopped && !need_restart;
    case CMD_TOPIC:
        return active < 3 && help_has_keyword( view );
    }
    return 1;
}

static void copy_sel( int cut )
{
    VIEW *view = app_view();
    POS from, to;
    char *text;
    u32 len;

    if ( !view_has_sel( view ) ) {
        return;
    }
    view_sel_range( view, &from, &to );
    text = doc_extract( view->doc, from, to, &len );
    if ( text == NULL ) {
        out_of_memory();
        return;
    }
    clip_set( text, len );
    if ( cut ) {
        view_delete_sel( view );
    }
}

static void split( void )
{
    if ( nviews == 2 ) {
        if ( active == 1 ) {
            views[0] = views[1];
        }
        views[1].doc = NULL;
        nviews = 1;
        if ( active == 1 ) {
            active = 0;
        }
    } else {
        views[1] = views[0];
        nviews = 2;
        grow[0] = 0;
    }
    maximized = 0;
    app_layout();
}

static void new_procedure( int kind )
{
    char name[NAME_MAX + 2];
    int text;
    VIEW *view = &views[active < 2 ? active : 0];

    if ( !dlg_newproc( kind, name ) ) {
        return;
    }
    if ( prog_find( name ) >= 0 ) {
        msg_box( "Duplicate definition", MB_OK, 1 );
        return;
    }
    text = prog_add_proc( name, kind );
    if ( text < 0 ) {
        out_of_memory();
        return;
    }
    app_edited();
    if ( active > 1 ) {
        active = 0;
    }
    ptext.texts[text]->at_line = 0;
    ptext.texts[text]->at_col = 0;
    view = &views[active];
    view_show_text( view, text );
    /* the cursor at the end of the heading, as QBasic leaves it */
    {
        u32 line;
        for ( line = 0; line < view->doc->count; line++ ) {
            LINE *ln = doc_line( view->doc, line );
            if ( ln->len && (ch_upper( ln->text[0] ) == 'S' || ch_upper( ln->text[0] ) == 'F') ) {
                view_goto( view, line, doc_col_of( view->doc, ln, ln->len ), 0 );
                view->want = view->col;
                break;
            }
        }
    }
}

static void next_procedure( int step )
{
    VIEW *view = &views[active < 2 ? active : 0];
    int text;

    if ( ptext.ntexts < 2 ) {
        return;
    }
    text = (view->text + step + ptext.ntexts) % ptext.ntexts;
    view_show_text( view, text );
}

static void toggle_breakpoint( void )
{
    VIEW *view = &views[active < 2 ? active : 0];
    LINE *line = doc_line( view->doc, view->line );

    line->flags ^= LF_BREAK;
}

static void clear_breakpoints( void )
{
    int t;
    u32 line;

    for ( t = 0; t < ptext.ntexts; t++ ) {
        for ( line = 0; line < ptext.texts[t]->count; line++ ) {
            doc_line( ptext.texts[t], line )->flags &= (u8)~LF_BREAK;
        }
    }
}

static void app_exit( void )
{
    if ( !ask_save() ) {
        return;
    }
    end_run();
    mouse_done();
    scr_done();
    con_exit();                                 /* the DOS screen, as the program left it */
    sys_exit( 0 );
}

void app_abandon( void )
{
    mouse_done();
    scr_done();
    sys_exit( 0 );
}

void app_command( int cmd )
{
    VIEW *view = app_view();
    char path[PATH_MAX];
    u32 len;
    const char *text;
    int pick;

    if ( cmd != CMD_EXIT && !settle() ) {
        return;
    }
    switch ( cmd ) {
    case CMD_NEW:
        if ( ask_save() ) {
            new_program();
        }
        break;
    case CMD_OPEN:
        if ( ask_save() && dlg_open( path ) ) {
            end_run();
            load_program( path );
        }
        break;
    case CMD_SAVE:
        save_program( 0 );
        break;
    case CMD_SAVEAS:
        save_program( 1 );
        break;
    case CMD_PRINT:
        dlg_print();
        break;
    case CMD_EXIT:
        if ( settle() ) {
            app_exit();
        }
        break;
    case CMD_CUT:
        copy_sel( 1 );
        break;
    case CMD_COPY:
        copy_sel( 0 );
        break;
    case CMD_PASTE:
        text = clip_get( &len );
        if ( text && active < 3 ) {
            view_insert_text( view, text, len );
        }
        break;
    case CMD_CLEAR:
        view_delete_sel( view );
        break;
    case CMD_NEWSUB:
        new_procedure( PK_SUB );
        break;
    case CMD_NEWFUNC:
        new_procedure( PK_FUNC );
        break;
    case CMD_SUBS:
        pick = dlg_subs();
        if ( pick >= 0 ) {
            if ( active > 1 ) {
                active = 0;
            }
            view_show_text( &views[active], pick );
        }
        break;
    case CMD_SPLIT:
        split();
        break;
    case CMD_OUTPUT:
        output_until_key( NULL );
        break;
    case CMD_FIND:
        dlg_find( 0 );
        break;
    case CMD_REPEAT:
        find_again();
        break;
    case CMD_CHANGE:
        dlg_find( 1 );
        break;
    case CMD_START:
        run_program( GO_RUN, 1 );
        break;
    case CMD_RESTART:
        end_run();
        if ( start_run() ) {
            /* lit: the module's first statement; with none, nothing runs */
            u32 first;
            for ( first = 0; first < prog->nlocs && prog->locs[first].proc != 0; first++ ) {
            }
            if ( first == prog->nlocs ) {
                finish_run();
            } else {
                vm.stmt = first;
                show_stop();
            }
        }
        break;
    case CMD_CONTINUE:
        run_program( GO_RUN, 0 );
        break;
    case CMD_MAKEEXE:
        make_exe();
        break;
    case CMD_STEP:
        run_program( GO_STEP, 0 );
        break;
    case CMD_PSTEP:
        run_program( GO_PSTEP, 0 );
        break;
    case CMD_TRACE:
        trace_on = !trace_on;
        break;
    case CMD_BREAKPOINT:
        toggle_breakpoint();
        break;
    case CMD_CLEARBP:
        clear_breakpoints();
        break;
    case CMD_SETNEXT:
        set_next();
        break;
    case CMD_DISPLAY:
        dlg_display();
        break;
    case CMD_HELPPATH:
        dlg_helppath();
        break;
    case CMD_SYNTAX:
        syntax_check = !syntax_check;
        break;
    case CMD_INDEX:
        help_topic( "index" );
        active = help_shown() ? 3 : active;
        break;
    case CMD_CONTENTS:
        help_topic( "contents" );
        active = help_shown() ? 3 : active;
        break;
    case CMD_TOPIC:
        if ( active < 3 ) {
            help_keyword( view );
        }
        break;
    case CMD_USINGHELP:
        help_topic( "usinghelp" );
        active = help_shown() ? 3 : active;
        break;
    case CMD_ABOUT:
        dlg_about();
        break;
    }
    app_layout();
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */

static int held_next( int hundredths )
{
    int key;

    for ( ;; ) {
        frame();
        key = key_get_timed( hundredths );
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key != K_DUMP ) {
            return key;
        }
        scr_dump();
    }
}

static void text_drag( VIEW *view )
{
    int key, row, col, rows = view_text_rows( view ), first = view->row + 1;

    for ( ;; ) {
        key = held_next( 8 );
        if ( key == K_MOUSE ) {
            if ( mouse.kind == ME_UP ) {
                return;
            }
            if ( mouse.kind == ME_DRAG ) {
                view_mouse_to( view, mouse.row, mouse.col, 1 );
            }
        } else if ( key == K_TICK ) {
            mouse_where( &row, &col );
            if ( row < first && view->top > 0 ) {
                view->top--;
                view_mouse_to( view, first, col, 1 );
            } else if ( row >= first + rows && view->top + (u32)rows < view->doc->count ) {
                view->top++;
                view_mouse_to( view, first + rows - 1, col, 1 );
            }
        }
    }
}

static void scroll_held( VIEW *view, int part )
{
    int key, row, col, delay = 40, thumb = part == VH_VTHUMB || part == VH_HTHUMB;

    view_scroll_part( view, part, mouse.row, mouse.col );
    for ( ;; ) {
        key = held_next( delay );
        if ( key == K_MOUSE ) {
            if ( mouse.kind == ME_UP ) {
                return;
            }
            if ( mouse.kind == ME_DRAG && thumb ) {
                view_scroll_part( view, part, mouse.row, mouse.col );
            }
        } else if ( key == K_TICK && !thumb ) {
            mouse_where( &row, &col );
            if ( view_hit( view, row, col ) == part ) {
                view_scroll_part( view, part, row, col );
            }
            delay = 5;
        }
    }
}

/* the status line's keys, clicked, are those keys */
static int status_click( int col )
{
    static const struct {
        const char *word;
        int key;
    } words[] = {
        { "<Shift+F1=Help>", K_SHF1 }, { "<F6=Window>", K_F6 }, { "<F2=Subs>", K_F2 },
        { "<F5=Run>", K_SHF5 }, { "<F8=Step>", K_F8 }, { "<F5=Continue>", K_F5 },
        { "<F9=Toggle Bkpt>", K_F9 }, { "<Enter=Execute Line>", K_ENTER },
        { "<Esc=Cancel>", K_ESC }, { "<Ctrl+F1=Next>", K_CTRLF1 }, { "<Alt+F1=Back>", K_ALTF1 }
    };
    char line[100];
    int index, at, filled;
    u32 len;

    for ( at = 0; at < scr_cols && at < (int)sizeof( line ) - 1; at++ ) {
        line[at] = (char)(scr_cell( scr_rows - 1, at ) & 0xFF);
    }
    line[at] = 0;
    filled = at;
    for ( index = 0; index < (int)(sizeof( words ) / sizeof( words[0] )); index++ ) {
        len = str_len( words[index].word );
        for ( at = 0; at + (int)len <= filled; at++ ) {
            if ( str_nicmp( line + at, words[index].word, len ) == 0 ) {
                if ( col >= at && col < at + (int)len ) {
                    return words[index].key;
                }
                break;
            }
        }
    }
    return 0;
}

static void global_key( int key );

static void app_mouse( void )
{
    VIEW *view;
    int index, part, cmd, key;

    if ( mouse.kind != ME_DOWN && mouse.kind != ME_DOUBLE ) {
        return;
    }
    if ( mouse.row == 0 ) {
        if ( !settle() ) {
            return;
        }
        cmd = menu_run( K_MOUSE );
        if ( cmd ) {
            app_command( cmd );
        }
        return;
    }
    if ( mouse.row == scr_rows - 1 ) {
        key = status_hint ? 0 : status_click( mouse.col );
        if ( key ) {
            global_key( key );
        }
        return;
    }
    if ( help_shown() && !maximized && mouse.row <= help_rows() ) {
        if ( !settle() ) {
            return;
        }
        active = 3;
        help_mouse();
        return;
    }
    for ( index = 0; index < 3; index++ ) {
        view = &views[index];
        if ( !is_shown( index ) || mouse.row < view->row || mouse.row >= view->row + view->height ) {
            continue;
        }
        if ( index != active || (view->dirty && view_hit( view, mouse.row, mouse.col ) == VH_TEXT
                                 && view->top + (u32)(mouse.row - view->row - 1) != view->line) ) {
            if ( !settle() ) {
                return;
            }
        }
        active = index;
        app_layout();
        part = view_hit( view, mouse.row, mouse.col );
        if ( part == VH_MAXBOX ) {
            maximized = !maximized;
            app_layout();
        } else if ( part == VH_TEXT ) {
            if ( mouse.kind == ME_DOUBLE ) {
                view_mouse_to( view, mouse.row, mouse.col, 0 );
                view_select_word( view );
            } else {
                view_mouse_to( view, mouse.row, mouse.col, (mouse.shift & SH_SHIFT) != 0 );
                text_drag( view );
            }
        } else if ( part != VH_NONE && part != VH_TITLE ) {
            scroll_held( view, part );
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* keys                                                                */
/* ------------------------------------------------------------------ */

static void next_window( int step )
{
    int order[4], count = 0, index, at = 0;

    if ( !settle() ) {
        return;
    }
    if ( help_shown() ) {
        order[count++] = 3;
    }
    for ( index = 0; index < nviews; index++ ) {
        order[count++] = index;
    }
    order[count++] = 2;
    for ( index = 0; index < count; index++ ) {
        if ( order[index] == active ) {
            at = index;
        }
    }
    active = order[(at + step + count) % count];
    if ( maximized && active == 3 ) {
        maximized = 0;
    }
    app_layout();
}

static void global_key( int key )
{
    int cmd;

    if ( key == K_MOUSE ) {
        app_mouse();
        return;
    }
    if ( key == K_TICK ) {
        return;                                 /* Alt down: the bar's letters show */
    }
    if ( key == K_ALTTAP || key_alt_letter( key ) ) {
        if ( !settle() ) {
            return;
        }
        cmd = menu_run( key );
        if ( cmd ) {
            app_command( cmd );
        }
        return;
    }
    switch ( key ) {
    case K_F1:
        if ( active < 3 ) {
            if ( settle() ) {
                help_keyword( app_view() );
            }
        } else {
            help_key( K_ENTER );                /* in the help: the link at the cursor */
        }
        return;
    case K_SHF1:
        app_command( CMD_USINGHELP );
        return;
    case K_F2:
        app_command( CMD_SUBS );
        return;
    case K_SHF2:
        if ( settle() ) {
            next_procedure( 1 );
        }
        return;
    case K_CTRLF2:
        if ( settle() ) {
            next_procedure( -1 );
        }
        return;
    case K_F3:
        app_command( CMD_REPEAT );
        return;
    case K_F4:
        app_command( CMD_OUTPUT );
        return;
    case K_F5:
        app_command( CMD_CONTINUE );
        return;
    case K_SHF5:
        app_command( CMD_START );
        return;
    case K_F6:
        next_window( 1 );
        return;
    case K_SHF6:
        next_window( -1 );
        return;
    case K_F7:
        run_to_cursor();
        return;
    case K_F8:
        app_command( CMD_STEP );
        return;
    case K_F10:
        app_command( CMD_PSTEP );
        return;
    case K_F9:
        app_command( CMD_BREAKPOINT );
        return;
    case K_CTRLF10:
        maximized = !maximized;
        if ( active == 3 ) {
            maximized = 0;
        }
        app_layout();
        return;
    case K_ALTPLUS:
    case K_ALTMINUS:
        if ( active < 3 && !maximized ) {
            int target = active == 2 ? 2 : 0;
            grow[target] += (key == K_ALTPLUS) == (active != 1) ? 1 : -1;
            app_layout();
        }
        return;
    case K_DEL | K_SHIFT:
        app_command( CMD_CUT );
        return;
    case K_CINS:
        app_command( CMD_COPY );
        return;
    case K_INS | K_SHIFT:
        app_command( CMD_PASTE );
        return;
    case K_BREAK:
        return;
    }
    if ( active == 3 ) {
        if ( help_key( key ) ) {
            if ( !help_shown() ) {
                active = 0;
            }
            return;
        }
        if ( key >= ' ' && key < 0x100 ) {
            active = 0;                         /* typing goes to the program */
        } else {
            return;
        }
    }
    if ( key == K_ESC ) {
        if ( help_shown() ) {
            help_close();
        }
        return;
    }
    if ( active == 2 && key == K_ENTER ) {
        VIEW *view = &views[2];
        if ( !immediate() ) {
            view_goto( view, view->line, 0, 0 );  /* the line that failed, from its start */
            view->want = 0;
            return;
        }
        /* the next line, a new one at the end */
        if ( view->line + 1 >= view->doc->count ) {
            doc_append( view->doc, "", 0 );
        }
        view_goto( view, view->line + 1, 0, 0 );
        view->want = 0;
        return;
    }
    view_key( &views[active], key );
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

static void usage( void )
{
    say( "Starts QBasic, which writes, runs and debugs BASIC programs.\r\n"
         "\r\n"
         "QBASIC [/B] [/EDITOR] [/G] [/H] [/MBF] [/NOHI] [[/RUN] [drive:][path]filename]\r\n"
         "\r\n"
         "  /B      Accepted, for a black-and-white screen; the colours stay.\r\n"
         "  /EDITOR Runs EDIT, the text editor, on the file.\r\n"
         "  /G      Accepted; the screen is always written at full speed.\r\n"
         "  /H      Shows as many lines as the display can.\r\n"
         "  /MBF    MKS$, MKD$, CVS and CVD use Microsoft Binary Format.\r\n"
         "  /NOHI   Accepted, for a screen without bright colours.\r\n"
         "  /RUN    Runs the program before showing it.\r\n"
         "  filename The program to load.\r\n" );
    sys_exit( 0 );
}

static int blank( int ch )
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int is_switch( const char *start, u32 len, const char *name, u32 least )
{
    u32 nlen = str_len( name );

    return len >= least && len <= nlen && str_nicmp( start, name, len ) == 0;
}

static void parse_line( const char *line, char *file )
{
    const char *start;
    u32 len, used;
    int quoted;

    file[0] = 0;
    for ( ;; ) {
        while ( blank( *line ) ) {
            line++;
        }
        if ( *line == 0 ) {
            return;
        }
        if ( *line == '/' ) {
            start = ++line;
            while ( *line && !blank( *line ) && *line != '/' ) {
                line++;
            }
            len = (u32)(line - start);
            if ( len == 1 && *start == '?' ) {
                usage();
            } else if ( is_switch( start, len, "RUN", 3 ) ) {
                opt_run = 1;
            } else if ( is_switch( start, len, "EDITOR", 2 ) ) {
                opt_editor = 1;
            } else if ( is_switch( start, len, "H", 1 ) ) {
                opt_lines = 50;
            } else if ( is_switch( start, len, "MBF", 3 ) ) {
                opt_mbf = 1;
            } else if ( is_switch( start, len, "B", 1 ) || is_switch( start, len, "G", 1 )
                        || is_switch( start, len, "NOHI", 4 ) ) {
                /* accepted; nothing to change here */
            } else {
                char text[60];
                str_cpy( text, "Invalid switch - /" );
                if ( len > 30 ) {
                    len = 30;
                }
                mem_cpy( text + 18, start, len );
                text[18 + len] = 0;
                str_catn( text, "\r\n", sizeof( text ) );
                say( text );
                sys_exit( 1 );
            }
            continue;
        }
        used = 0;
        quoted = 0;
        while ( *line && (quoted || (!blank( *line ) && *line != '/')) ) {
            if ( *line == '"' ) {
                quoted = !quoted;
            } else if ( used < PATH_MAX - 5 ) {
                file[used++] = *line;
            }
            line++;
        }
        file[used] = 0;
    }
}

/* QBASIC /EDITOR is EDIT */
static void run_editor( const char *file )
{
    char tail[PATH_MAX + 8];
    const char *comspec = sys_getenv( "COMSPEC" );

    str_cpy( tail, "/C EDIT " );
    str_catn( tail, file, sizeof( tail ) );
    sys_exit( sys_exec( comspec ? comspec : sys_shell(), tail, NULL ) == 0 ? 0 : 1 );
}

void qbasic_main( void )
{
    char file[PATH_MAX];
    int key, has_file;

    sys_init();
    sys_break_install();
    parse_line( sys_cmdline(), file );
    if ( opt_editor ) {
        run_editor( file );
    }
    if ( !sys_fpu_present() ) {
        say( "QBasic needs a math coprocessor: a 486DX or better, or a 386 with a 387.\r\n" );
        sys_exit( 1 );
    }
    fp_init();
    has_file = file[0] != 0;
    if ( has_file && str_chr( str_rchr( file, '\\' ) ? str_rchr( file, '\\' ) : file, '.' ) == NULL ) {
        str_catn( file, ".BAS", sizeof( file ) );
    }
    con_capture_dos();
    key_init();
    scr_init( opt_lines );
    mouse_init();
    imm_doc = doc_new();
    if ( imm_doc == NULL ) {
        out_of_memory();
    }
    doc_append( imm_doc, "", 0 );
    views[2].text = -1;
    views[2].doc = imm_doc;
    prog_clear();
    if ( has_file ) {
        if ( prog_load( file ) != 0 ) {
            if ( opt_run ) {
                scr_done();
                say( "File not found\r\n" );
                sys_exit( 1 );
            }
            prog_clear();
            str_cpyn( ptext.path, file, sizeof( ptext.path ) );
            {
                const char *slash = str_rchr( file, '\\' );
                str_cpyn( ptext.name, slash ? slash + 1 : file, sizeof( ptext.name ) );
            }
        } else {
            view_learn_program();
            ptext.modified = 0;
        }
    }
    ui_ready = 1;
    active = 0;
    program_changed_whole();
    views[2].line = 0;
    view_keep_visible( &views[2] );
    app_layout();
    if ( opt_run && has_file ) {
        frame();
        run_program( GO_RUN, 1 );
        opt_run = 0;
    } else if ( !has_file ) {
        frame();
        if ( dlg_welcome() ) {
            help_topic( "survival" );
            if ( help_shown() ) {
                active = 3;
            }
            app_layout();
        }
    }
    for ( ;; ) {
        frame();
        key = key_get();
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        global_key( key );
    }
}
