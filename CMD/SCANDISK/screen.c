/*
 * SCREEN.C - ScanDisk's screens: the front end the checks talk to.
 *
 * THEY ARE MS-DOS SCANDISK'S SCREENS, put where that program puts them
 * (the wording is this program's own):
 *
 *   the checks     the five areas of the drive, a tick against each
 *                  one done and a mark against the one being checked;
 *                  under them Pause, More Info and Exit
 *   the surface    a box over the checks: a map of the drive, a block
 *                  for so many clusters - unused, partly used, used,
 *                  bad - changing colour as the scan reads, the counts
 *                  beside it, and More Info and Exit
 *   the summary    what was found on the drive, and View Log
 *
 * with the title and a rule at the top, and at the bottom another rule
 * and one line that says how far the work has got, as a number and as a
 * bar.  The buttons work while a check runs, because the checks call
 * fe_progress() between one piece of work and the next and that is
 * where the keyboard and the mouse are looked at.  How often is paced
 * by the clock, not by the calls: a look costs a call into the kernel,
 * and a directory walk asks thousands of times a second.
 *
 * A problem is a box over whichever screen is up: what was found, and
 * Fix It, Don't Fix It, More Info.  Ctrl+C in any box is Exit.
 *
 * ON A GRAPHICS SCREEN (SCANDISK /G) every screen is the same screen;
 * SCR.C and GFX.C draw it.  What differs here is small: the map has two
 * rows of blocks to a row of text, the bar moves a pixel at a time, and
 * the ticks and the signs in the boxes are pictures.
 *
 * Leaving is scan_quit(): the check is abandoned from wherever it has
 * got to, between two repairs and never inside one.
 */
#include "ui.h"

enum { V_BLANK, V_CHECKS, V_SURFACE, V_SUMMARY, V_BUSY };
enum { ID_PAUSE = 1, ID_INFO, ID_EXIT, ID_FIX, ID_SKIP, ID_SAVE, ID_DELETE, ID_YES, ID_NO,
       ID_OK, ID_LOG, ID_NEXT, ID_DRIVE_A, ID_DRIVE_B, ID_SAVELOG, ID_BACK };

#define MARGIN          5       /* the column everything starts in */
#define BAR_COL         20      /* ...and the progress bar */

static int  view = V_BLANK;
static int  cur_phase = -1;
static int  paused;
static u32  bar_done, bar_total;
static BTNROW run_row;          /* under the checks */
static BTNROW surf_row;         /* in the Surface Scan box */

static const BTN run_btns[] = { { "&Pause", ID_PAUSE }, { "&More Info", ID_INFO },
                                { "E&xit", ID_EXIT } };
static const BTN held_btns[] = { { "&Continue", ID_PAUSE }, { "&More Info", ID_INFO },
                                 { "E&xit", ID_EXIT } };
static const BTN surf_btns[] = { { "&More Info", ID_INFO }, { "E&xit", ID_EXIT } };
static const BTN ok_btn[] = { { "&OK", ID_OK } };
static const BTN yes_no[] = { { "&Yes", ID_YES }, { "&No", ID_NO } };

/* the summary screen, and the screen of a job that is not a check */
static TEXT   sum_text;
static BTNROW sum_row;
static char   busy_head[80];

/* ------------------------------------------------------------------ */
/* the surface map                                                     */
/* ------------------------------------------------------------------ */

#define MAP_MAX     (100 * 56)

static u8   blocks[MAP_MAX];    /* MB_* */
static u32  nblocks, per_block, now_block;
static int  box_top, box_left, box_bottom, box_right, box_split;
static int  map_cols, map_rows; /* in cells */
static int  block_rows;         /* rows of blocks: two to a cell in graphics */

/* The box is most of the screen: the map on the left of a line down
   it, the counts on the right. */
static void map_build( void )
{
    u32 clus, index, used, val, room;
    int bad;

    box_top = 2;
    box_left = 3;
    box_bottom = scr_rows - 5;
    box_right = scr_cols - 6;
    box_split = box_right - 24;
    map_cols = box_split - box_left - 3;
    map_rows = box_bottom - box_top - 3;
    if ( map_cols > 100 ) {
        map_cols = 100;
    }
    block_rows = gfx_on ? map_rows * 2 : map_rows;
    if ( block_rows > 56 ) {
        block_rows = 56;
    }
    room = (u32)(map_cols * block_rows);
    per_block = (vol.nclus + room - 1) / room;
    if ( per_block == 0 ) {
        per_block = 1;
    }
    nblocks = (vol.nclus + per_block - 1) / per_block;
    now_block = 0;
    for ( index = 0; index < nblocks; index++ ) {
        used = 0;
        bad = 0;
        for ( clus = 2 + index * per_block; clus < 2 + (index + 1) * per_block &&
                                            clus <= vol.maxclus; clus++ ) {
            val = fat_get( clus );
            if ( val == vol.bad_val ) {
                bad = 1;
            } else if ( val != 0 ) {
                used++;
            }
        }
        blocks[index] = (u8)((bad ? MB_BAD : 0) | (used == 0 ? 0 : used >= per_block ? MB_ALL
                             : MB_SOME));
        /* the last block may be a short one */
        if ( used && index == nblocks - 1 && used >= vol.nclus - index * per_block ) {
            blocks[index] = (u8)((blocks[index] & MB_BAD) | MB_ALL);
        }
    }
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

static int percent( void )
{
    static const int from[PH_COUNT + 1] = { 0, 2, 10, 60, 100, 100 };
    u32 done = bar_done, total = bar_total, part;

    if ( view == V_SURFACE ) {
        done = st.surf_examined;
        total = vol.nclus;
    } else if ( view == V_CHECKS && cur_phase < 0 ) {
        return 0;
    } else if ( view == V_CHECKS && cur_phase >= PH_SURFACE ) {
        return 100;
    }
    while ( total > 0x00FFFFFFul ) {
        total >>= 4;
        done >>= 4;
    }
    part = total ? done * 100 / total : 0;
    if ( part > 100 ) {
        part = 100;
    }
    if ( view != V_CHECKS ) {
        return (int)part;
    }
    return from[cur_phase] + (int)((u32)(from[cur_phase + 1] - from[cur_phase]) * part / 100);
}

/* the title and the two rules */
static void paint_frame( void )
{
    int row, wide = scr_cols - 2 * MARGIN;

    for ( row = 0; row < scr_rows; row++ ) {
        scr_fill( row, 0, scr_cols, ' ', pal.desk );
    }
    scr_put( 2, MARGIN, "ScanDisk", pal.bright );
    scr_fill( 3, MARGIN, wide, 0xC4, pal.rule );
    scr_fill( scr_rows - 3, MARGIN, wide, 0xC4, pal.rule );
    scr_cursor( 0, 0, CUR_HIDE );
}

/* the line at the bottom: how far, as a number and as a bar */
static void paint_status( void )
{
    int row = scr_rows - 2, wide = scr_cols - MARGIN - BAR_COL, pct = percent(), fill, col, part;
    char text[24];

    sfmt( text, sizeof( text ), "%u%% complete", (u32)pct );
    scr_put( row, MARGIN + 1, text, pal.bright );
    if ( !gfx_on ) {
        fill = wide * pct / 100;
        scr_fill( row, BAR_COL, fill, 0xDB, pal.bar_done );
        scr_fill( row, BAR_COL + fill, wide - fill, 0xB0, pal.bar_todo );
        return;
    }
    fill = wide * 8 * pct / 100;
    for ( col = 0; col < wide; col++ ) {
        part = fill - col * 8;
        scr_pic( row, BAR_COL + col, PIC_BAR,
                 (part < 0 ? 0 : part > 8 ? 8 : part) | ((pal.desk >> 4) << 4), pal.bar_done );
    }
    scr_flag( row, BAR_COL, 1, CF_LEFT );
    scr_flag( row, BAR_COL + wide - 1, 1, CF_RIGHT );
}

/* on the title line, where no box ever covers them */
static void paint_counts( void )
{
    char line[80];
    int len;

    if ( st.found ) {
        len = sfmt( line, sizeof( line ), "Problems found: %,u   Repaired: %,u", st.found,
                    st.fixed );
        scr_put( 2, scr_cols - MARGIN - len, line, pal.desk );
    }
}

static void paint_checks( int with_buttons )
{
    char line[80];
    int phase;

    paint_frame();
    sfmt( line, sizeof( line ), "ScanDisk is checking these areas of drive %c:", 'A' + vol.drive );
    scr_put( 5, MARGIN, line, pal.desk );
    for ( phase = 0; phase < PH_COUNT; phase++ ) {
        if ( phase < cur_phase ) {
            scr_ch( 7 + phase, 8, 0xFB, pal.mark );
            scr_icon( 7 + phase, 8, PIC_TICK, pal.mark );
        } else if ( phase == cur_phase ) {
            scr_ch( 7 + phase, 8, 0xAF, pal.bright );
            scr_icon( 7 + phase, 8, PIC_NOW, pal.bright );
        }
        scr_put( 7 + phase, 13, phase_name( phase ), pal.desk );
    }
    paint_counts();
    if ( with_buttons ) {
        btn_draw_from( &run_row, scr_rows - 5, MARGIN );
    }
    paint_status();
}

/* one block of the map as a text screen shows it */
static void text_block( int row, int col, u32 block )
{
    u8 state = blocks[block];
    u8 attr = (state & MB_BAD) ? pal.map_bad : (state & MB_READ) ? pal.map_read : pal.map;

    scr_ch( row, col, (state & MB_BAD) ? 'B' : (state & MB_ALL) ? 0xDB : (state & MB_SOME) ? 0xB1
            : 0xB0, attr );
}

/* a sample for the legend: a block in that state, as the map draws one */
static void sample( int row, int col, int state )
{
    if ( gfx_on ) {
        scr_pic( row, col, PIC_MAP, state | (state << 4), MC_UPPER | MC_LOWER );
    } else {
        scr_ch( row, col, (state & MB_BAD) ? 'B' : (state & MB_ALL) ? 0xDB
                : (state & MB_SOME) ? 0xB1 : 0xB0, (state & MB_BAD) ? pal.map_bad : pal.map );
    }
}

static void paint_surface( void )
{
    static const struct {
        int state;
        const char *what;
    } legend[4] = {
        { 0, "unused clusters" }, { MB_SOME, "some used clusters" }, { MB_ALL, "used clusters" },
        { MB_BAD, "some bad clusters" }
    };
    static const char title[] = "Surface Scan";
    char line[80];
    int row, col, side = box_split + 2, index, flags, upper_now, lower_now;
    u32 upper, lower;

    paint_checks( 0 );
    ui_frame( box_top, box_left, box_bottom, box_right, gfx_on ? title : "" );
    if ( !gfx_on ) {
        /* the title over the map, and the line down the box joined to
           the lines it meets */
        col = box_left + (box_split - box_left - (int)sizeof( title ) - 1) / 2;
        scr_ch( box_top, col, ' ', pal.dlg_frame );
        scr_put( box_top, col + 1, title, pal.dlg_frame );
        scr_ch( box_top, col + (int)sizeof( title ), ' ', pal.dlg_frame );
        scr_ch( box_top, box_split, 0xC2, pal.dlg_frame );
    }
    for ( row = box_top + 1; row < box_bottom - 2; row++ ) {
        scr_ch( row, box_split, 0xB3, pal.dlg_frame );
    }
    scr_ch( box_bottom - 2, box_split, 0xC1, pal.dlg_frame );

    for ( row = 0; row < map_rows; row++ ) {
        for ( col = 0; col < map_cols; col++ ) {
            if ( !gfx_on ) {
                upper = (u32)(row * map_cols + col);
                if ( upper < nblocks ) {
                    text_block( box_top + 1 + row, box_left + 2 + col, upper );
                }
                continue;
            }
            upper = (u32)(row * 2 * map_cols + col);
            lower = upper + (u32)map_cols;
            upper_now = upper == now_block && st.surface == 2;
            lower_now = lower == now_block && st.surface == 2;
            flags = (upper < nblocks ? MC_UPPER : 0) | (lower < nblocks ? MC_LOWER : 0)
                    | (upper_now ? MC_UPPER_NOW : 0) | (lower_now ? MC_LOWER_NOW : 0);
            scr_pic( box_top + 1 + row, box_left + 2 + col, PIC_MAP,
                     (upper < nblocks ? blocks[upper] : 0) | ((lower < nblocks ? blocks[lower] : 0) << 4),
                     (u8)flags );
        }
    }

    sfmt( line, sizeof( line ), "Drive %c:", 'A' + vol.drive );
    scr_put( box_top + 2, side, line, pal.dlg );
    sfmt( line, sizeof( line ), "%,11u clusters", vol.nclus );
    scr_put( box_top + 4, side, line, pal.dlg );
    sfmt( line, sizeof( line ), "%,11u examined", st.surf_examined );
    scr_put( box_top + 5, side, line, pal.dlg );
    sfmt( line, sizeof( line ), "%,11u found bad", st.surf_bad );
    scr_put( box_top + 6, side, line, pal.dlg );
    if ( box_top + 8 < box_bottom - 2 ) {
        sample( box_top + 8, side, MB_SOME );
        sfmt( line, sizeof( line ), "= %,u cluster%s", per_block, per_block == 1 ? "" : "s" );
        scr_put( box_top + 8, side + 2, line, pal.dlg );
    }
    for ( index = 0; index < 4 && box_top + 10 + index < box_bottom - 2; index++ ) {
        sample( box_top + 10 + index, side, legend[index].state );
        scr_put( box_top + 10 + index, side + 2, legend[index].what, pal.dlg );
    }
    btn_draw( &surf_row, box_bottom - 1, box_left + 1, box_right - box_left - 1 );
}

/* a screen of text: its first lines, up to an empty one, stand out */
static void paint_summary( void )
{
    int index, bright = 1;

    paint_frame();
    for ( index = 0; index < sum_text.count && 5 + index < scr_rows - 6; index++ ) {
        if ( sum_text.line[index][0] == 0 ) {
            bright = 0;
        }
        scr_put( 5 + index, MARGIN, sum_text.line[index], bright ? pal.bright : pal.desk );
    }
    btn_draw_from( &sum_row, scr_rows - 5, MARGIN );
}

static void paint( void )
{
    switch ( view ) {
    case V_CHECKS:
        paint_checks( 1 );
        break;
    case V_SURFACE:
        paint_surface();
        break;
    case V_SUMMARY:
        paint_summary();
        break;
    case V_BUSY:
        paint_frame();
        scr_put( 5, MARGIN, busy_head, pal.desk );
        paint_status();
        break;
    default:
        paint_frame();
        break;
    }
}

static void show( void )
{
    paint();
    scr_flush();
}

/* ------------------------------------------------------------------ */
/* while a check runs                                                  */
/* ------------------------------------------------------------------ */

void app_abandon( void )
{
    leave( 3 );
}

static int confirm_exit( void )
{
    char text[200];

    sfmt( text, sizeof( text ),
          "ScanDisk has not finished checking drive %c:.\n\nExit ScanDisk?", 'A' + vol.drive );
    return ui_ask( "Exit", text, yes_no, 2, 1, 0, PIC_ASK ) == ID_YES;
}

/* Exit, or Ctrl+C.  A surface scan is left at once, as MS-DOS
   ScanDisk's is: nothing is half done, and what it found stays found.
   The other checks ask first. */
static void want_out( void )
{
    if ( view == V_SURFACE || confirm_exit() ) {
        scan_quit( Q_USER );
    }
}

static void run_keys( void )
{
    int key, id;

    for ( ;; ) {
        key = paused ? key_get() : key_poll();
        if ( key == K_TICK ) {
            return;
        }
        if ( key == K_EOF ) {
            if ( paused ) {
                app_abandon();
            }
            return;
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        if ( key == K_SHOT ) {
            scr_shot();
            continue;
        }
        if ( view == V_BUSY ) {
            continue;                   /* not a job to be left half done */
        }
        id = key == K_CTRLC ? ID_EXIT : key == K_F1 ? ID_INFO
             : btn_key( view == V_SURFACE ? &surf_row : &run_row, key );
        switch ( id ) {
        case ID_PAUSE:
            /* held, the row is an ordinary one: Continue is lit */
            paused = !paused;
            run_row.btn = paused ? held_btns : run_btns;
            run_row.focus = paused ? 0 : -1;
            break;
        case ID_INFO:
            ui_info( phase_name( cur_phase < PH_COUNT ? cur_phase : PH_SURFACE ),
                     phase_info( cur_phase < PH_COUNT ? cur_phase : PH_SURFACE ), ok_btn, 1,
                     0, ID_OK );
            break;
        case ID_EXIT:
            want_out();
            break;
        }
        show();
    }
}

/* The screen brought up to date and the keys looked at - but only when
   a twentieth of a second or so has gone by since the last time.  The
   gap between looks is counted in calls, and the count is made longer
   or shorter as the clock shows the calls coming faster or slower. */
static u32 pace_calls, pace_every = 1;

static void pace( void )
{
    static u32 last;
    u32 now;

    if ( ++pace_calls < pace_every ) {
        return;
    }
    pace_calls = 0;
    now = sys_hundredths();
    if ( now - last < 4 && pace_every < 4096 ) {
        pace_every *= 2;
    } else if ( now - last > 12 && pace_every > 1 ) {
        pace_every /= 2;
    }
    last = now;
    show();
    if ( sys_break_take() && view != V_BUSY ) {
        want_out();
    }
    run_keys();
}

void fe_progress( u32 done, u32 total )
{
    if ( total ) {
        bar_done = done;
        bar_total = total;
    }
    pace();
}

/* ------------------------------------------------------------------ */
/* the front end                                                       */
/* ------------------------------------------------------------------ */

void fe_start( void )
{
    scr_init( opt.graphics );
    key_init();
    if ( opt.mouse ) {
        mouse_init();
    }
    ui_backdrop( paint );
    tx_init( &sum_text );
    view = V_BLANK;
    show();
}

void fe_finish( void )
{
    scr_done();
}

void fe_drive_begin( void )
{
    view = V_CHECKS;
    cur_phase = -1;
    paused = 0;
    bar_done = bar_total = 0;
    btn_set( &run_row, run_btns, 3, -1, 1 );
    show();
}

void fe_phase( int phase )
{
    cur_phase = phase;
    key_poll_phase( phase );
    pace_every = 1;                     /* a short check is looked in on too */
    bar_done = bar_total = 0;
    if ( phase >= PH_COUNT && view == V_SURFACE ) {
        view = V_CHECKS;                /* the scan is over, and its box goes */
    }
    show();
}

/* a job that is not one of the checks: what it is, and a bar that
   fe_progress() moves */
void fe_busy( const char *text )
{
    str_cpyn( busy_head, text, sizeof( busy_head ) );
    view = V_BUSY;
    paused = 0;
    pace_every = 1;
    bar_done = bar_total = 0;
    key_poll_phase( -1 );
    show();
}

/* "More Info" from a problem's box: the problem again, what it means,
   and the same choices with a way back */
static int problem_info( int kind, const char *text, const BTN *btn, int count )
{
    TEXT lines;
    int id, width = scr_cols - 16;

    tx_init( &lines );
    tx_wrap( &lines, text, width > 64 ? 64 : width );
    tx_add( &lines, "" );
    tx_wrap( &lines, pb_info( kind ), width > 64 ? 64 : width );
    id = ui_text( "More Information", &lines, btn, count, ID_EXIT, ID_BACK );
    tx_free( &lines );
    return id;
}

int fe_problem( int kind, const char *text )
{
    static const BTN fix[] = { { "&Fix It", ID_FIX }, { "&Don't Fix It", ID_SKIP },
                               { "&More Info", ID_INFO } };
    static const BTN fix_info[] = { { "&Fix It", ID_FIX }, { "&Don't Fix It", ID_SKIP },
                                    { "&Previous Screen", ID_BACK } };
    static const BTN del[] = { { "&Delete", ID_DELETE }, { "Do&n't Delete", ID_SKIP },
                               { "&More Info", ID_INFO } };
    static const BTN del_info[] = { { "&Delete", ID_DELETE }, { "Do&n't Delete", ID_SKIP },
                                    { "&Previous Screen", ID_BACK } };
    static const BTN save[] = { { "&Save", ID_SAVE }, { "&Delete", ID_DELETE },
                                { "Do&n't Fix It", ID_SKIP }, { "&More Info", ID_INFO } };
    static const BTN save_info[] = { { "&Save", ID_SAVE }, { "&Delete", ID_DELETE },
                                     { "Do&n't Fix It", ID_SKIP },
                                     { "&Previous Screen", ID_BACK } };
    static const BTN none[] = { { "&OK", ID_SKIP }, { "&More Info", ID_INFO } };
    static const BTN none_info[] = { { "&OK", ID_SKIP }, { "&Previous Screen", ID_BACK } };
    const BTN *btn = fix, *info = fix_info;
    int count = 3, id;

    switch ( pb_choices( kind ) ) {
    case CH_DELETE:  btn = del;   info = del_info;                break;
    case CH_SAVEDEL: btn = save;  info = save_info;  count = 4;   break;
    case CH_NONE:    btn = none;  info = none_info;  count = 2;   break;
    }
    for ( ;; ) {
        id = ui_ask( "Problem Found", text, btn, count, 0, ID_EXIT, PIC_WARN );
        if ( id == ID_INFO ) {
            id = problem_info( kind, text, info, count );
        }
        switch ( id ) {
        case ID_BACK:
            break;
        case ID_EXIT:
            if ( confirm_exit() ) {
                scan_quit( Q_USER );
            }
            break;
        case ID_FIX:    show();  return ACT_FIX;
        case ID_DELETE: show();  return ACT_DELETE;
        case ID_SAVE:   show();  return ACT_SAVE;
        default:        show();  return ACT_SKIP;
        }
    }
}

/* A problem nobody is asked about is in the counts on the title line
   and in the log.  A repair that was asked for and could not be made
   has to be said. */
void fe_outcome( int news, const char *text )
{
    if ( news == 2 && opt.mode != MODE_AUTOFIX ) {
        fe_message( text );
    }
}

int fe_ask_surface( void )
{
    char text[400];
    int id;

    sfmt( text, sizeof( text ),
          "ScanDisk has finished checking the files and directories of drive %c:.\n\n"
          "A surface scan reads every cluster of the drive to find the ones that cannot be "
          "read.  On a large drive the scan takes a long time; Exit stops it.\n\n"
          "Scan the surface of drive %c: now?", 'A' + vol.drive, 'A' + vol.drive );
    for ( ;; ) {
        id = ui_ask( "", text, yes_no, 2, 0, ID_EXIT, PIC_ASK );
        if ( id != ID_EXIT ) {
            show();
            return id == ID_YES;
        }
        if ( confirm_exit() ) {
            scan_quit( Q_USER );
        }
    }
}

int fe_ask_undo( int have_a, int have_b )
{
    static const BTN both[] = { { "Drive &A", ID_DRIVE_A }, { "Drive &B", ID_DRIVE_B },
                                { "&Skip Undo", ID_SKIP }, { "&More Info", ID_INFO } };
    static const BTN only_a[] = { { "Drive &A", ID_DRIVE_A }, { "&Skip Undo", ID_SKIP },
                                  { "&More Info", ID_INFO } };
    static const BTN only_b[] = { { "Drive &B", ID_DRIVE_B }, { "&Skip Undo", ID_SKIP },
                                  { "&More Info", ID_INFO } };
    char text[400];
    int id;

    sfmt( text, sizeof( text ),
          "ScanDisk is about to repair drive %c:.\n\n"
          "It can first save what the repairs change on a diskette, so that SCANDISK /UNDO "
          "can put the drive back as it was.\n\n"
          "Put a formatted diskette with free space in a drive and choose that drive, or "
          "choose Skip Undo.", 'A' + vol.drive );
    for ( ;; ) {
        id = ui_ask( "Create Undo Disk", text, have_a && have_b ? both : have_a ? only_a : only_b,
                     have_a && have_b ? 4 : 3, 0, ID_EXIT, PIC_ASK );
        if ( id == ID_INFO ) {
            ui_info( "More Information",
                     "An Undo disk holds a file, SCANDISK.UND, with every sector ScanDisk is "
                     "about to change as the sector was before the change.\n\n"
                     "If a repair turns out worse than the problem, SCANDISK /UNDO writes "
                     "those sectors back.  That is safe only while nothing else has written "
                     "to the drive, so ScanDisk checks first and refuses if the drive has "
                     "been used since.\n\n"
                     "The file needs about as much room as the repairs change: a few "
                     "thousand bytes for most, more when a file allocation table is copied.",
                     ok_btn, 1, 0, ID_OK );
            continue;
        }
        if ( id == ID_EXIT ) {
            if ( confirm_exit() ) {
                scan_quit( Q_USER );
            }
            continue;
        }
        show();
        return id == ID_DRIVE_A ? 0 : id == ID_DRIVE_B ? 1 : -1;
    }
}

void fe_message( const char *text )
{
    ui_ask( "", text, ok_btn, 1, 0, ID_OK, PIC_INFO );
    show();
}

int fe_confirm( const char *title, const char *text )
{
    int id = ui_ask( title, text, yes_no, 2, 0, ID_NO, PIC_ASK );

    show();
    return id == ID_YES;
}

void fe_surface_begin( void )
{
    map_build();
    view = V_SURFACE;
    paused = 0;
    btn_set( &surf_row, surf_btns, 2, 0, 0 );
    show();
}

void fe_surface_at( u32 clus, u32 count, int bad )
{
    u32 block = (clus - 2) / per_block, last = (clus - 2 + count - 1) / per_block;

    if ( block >= nblocks ) {
        return;
    }
    if ( bad ) {
        blocks[block] |= MB_BAD;
        show();                         /* at once: a problem box follows */
        return;
    }
    for ( ; block <= last && block < nblocks; block++ ) {
        blocks[block] |= MB_READ;
        now_block = block;
    }
    pace();
}

/* the log, with a way to keep it */
static void view_log( void )
{
    static const BTN btn[] = { { "&OK", ID_OK }, { "&Save Log", ID_SAVELOG } };
    char where[24], text[120];

    while ( ui_text( "ScanDisk Log", &log_text, btn, 2, 0, ID_OK ) == ID_SAVELOG ) {
        if ( log_save( 0, where ) == 0 ) {
            sfmt( text, sizeof( text ), "The log was saved as %s.", where );
        } else {
            sfmt( text, sizeof( text ), "The log could not be saved as %s.", where );
        }
        fe_message( text );
    }
}

/* a screen of text with View Log and a way on */
static void summary_screen( const BTN *btn )
{
    int key, id;

    view = V_SUMMARY;
    btn_set( &sum_row, btn, 2, 0, 1 );
    for ( ;; ) {
        show();
        key = key_get();
        if ( key == K_EOF ) {
            app_abandon();
        }
        if ( key == K_DUMP ) {
            scr_dump();
            continue;
        }
        if ( key == K_SHOT ) {
            scr_shot();
            continue;
        }
        id = key == K_ESC || key == K_CTRLC ? ID_NEXT : btn_key( &sum_row, key );
        if ( id == ID_LOG ) {
            view_log();
        } else if ( id == ID_NEXT ) {
            break;
        }
    }
    view = V_BLANK;
    show();
}

void fe_drive_end( int more )
{
    static const BTN on[] = { { "&View Log", ID_LOG }, { "&Continue", ID_NEXT } };
    static const BTN out[] = { { "&View Log", ID_LOG }, { "E&xit", ID_NEXT } };

    tx_free( &sum_text );
    summary_text( &sum_text );
    summary_screen( more ? on : out );
}

void fe_all_end( int drives, u32 found, u32 fixed )
{
    static const BTN btn[] = { { "&View Log", ID_LOG }, { "E&xit", ID_NEXT } };

    tx_free( &sum_text );
    if ( found == 0 ) {
        tx_addf( &sum_text, "ScanDisk checked %u drives and found no problems.", (u32)drives );
    } else {
        tx_addf( &sum_text, "ScanDisk checked %u drives and found %,u problem%s.", (u32)drives,
                 found, found == 1 ? "" : "s" );
        tx_addf( &sum_text, "%,u %s repaired.", fixed, fixed == 1 ? "was" : "were" );
    }
    summary_screen( btn );
}
