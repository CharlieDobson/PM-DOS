/*
 * SCREEN.C - DEFRAG's screen, and the questions it asks.
 *
 * THE SCREEN IS MS-DOS DEFRAG'S, measured from the real program: a
 * white line along the top with the one menu in it, the map of the
 * drive under that, two boxes under the map - how far the work has got,
 * and what the map's symbols mean - and a line along the bottom that
 * says what can be done next.  The boxes that open over it are where
 * that program opens them and the size it makes them.  A screen with
 * more than twenty-five lines gives the extra lines to the map.
 *
 * THE MAP is the drive cut into as many blocks as there are cells for
 * it.  A block is "used" when every cluster of it is in use - which is
 * MS-DOS DEFRAG's rule, found by comparing its map with a drive whose
 * every cluster was known - and it is an X if anything in it is never
 * moved and a B if anything in it is bad.  A block whose every cluster
 * has reached its final place is yellow.  While clusters are being
 * read or written their blocks are r and W.
 *
 * ON A TEXT SCREEN the map is characters, as MS-DOS DEFRAG's is under
 * /G0.  ON A GRAPHICS SCREEN (DEFRAG /G) a block is a tile, the bar is
 * drawn in pixels and the round buttons are round (GFX.C).
 *
 * EVERYTHING IS PAINTED EVERY TIME (SCR.C sends what changed): paint()
 * is the whole screen as it stands, and a box is drawn over it by its
 * own loop.  So the work is never told about boxes, and a box never
 * has to put back what it covered.
 */
#include "ui.h"
#include "text.h"

#define MAP_TOP         2
#define MAP_LEFT        2
#define MAP_COLS        76

#define STAT_LEFT       3
#define STAT_RIGHT      40
#define LEG_LEFT        42
#define LEG_RIGHT       78
#define BAR_CELLS       34

static int  map_rows = 16;
static int  have_drive;
static int  running;
static int  reading;            /* the box that says a drive is being read */
static char reading_name[16];
static int  act;                /* ACT_*: what the drive is doing */
static u32  act_clus, act_count;
static u32  shown_clus = 2;
static u32  prog_done, prog_total;
static u32  run_began, run_secs;
static const char *run_hint;

static const BTN btn_ok[] = { { tx_btn_ok, 1 } };
static const BTN btn_ok_cancel[] = { { tx_btn_ok, 1 }, { tx_btn_cancel, 2 } };

/* ------------------------------------------------------------------ */
/* the screen as it stands                                             */
/* ------------------------------------------------------------------ */

static void draw_block( int row, int col, int kind )
{
    static const u8 as_text[] = { 0xB1, 0x08, 0x08, 'X', 'B', 'r', 'W' };
    u8 attr = kind == BK_DONE ? pal.done : pal.desk;

    if ( gfx_on && kind <= BK_DONE ) {
        /* free space is a quiet pattern, not a white one */
        scr_pic( row, col, PIC_BLOCK, kind, kind == BK_UNUSED ? (u8)((pal.desk & 0xF0) | 0x07) : attr );
    } else {
        scr_ch( row, col, as_text[kind], attr );
    }
}

static int block_kind( u32 blk )
{
    u32 first = 2 + blk * per_block, size = vol.maxclus + 1 - first;
    const BLOCK *info = &blocks[blk];

    if ( size > per_block ) {
        size = per_block;
    }
    if ( act_count && act_clus < first + size && act_clus + act_count > first ) {
        if ( act == ACT_READ ) {
            return BK_READ;
        }
        if ( act == ACT_WRITE ) {
            return BK_WRITE;
        }
    }
    if ( info->bad ) {
        return BK_BAD;
    }
    if ( info->fixed ) {
        return BK_FIXED;
    }
    if ( info->used >= size ) {
        return info->done >= size ? BK_DONE : BK_USED;
    }
    return BK_UNUSED;
}

static void paint_map( void )
{
    u32 blk, cells = (u32)MAP_COLS * (u32)map_rows;

    if ( !have_drive || blocks == NULL ) {
        return;
    }
    for ( blk = 0; blk < nblocks && blk < cells; blk++ ) {
        draw_block( MAP_TOP + (int)(blk / MAP_COLS), MAP_LEFT + (int)(blk % MAP_COLS), block_kind( blk ) );
    }
}

/* a box on the screen itself, with its title in the top of it */
static void desk_box( int top, int left, int right, const char *title )
{
    int len = (int)str_len( title ), wide = right - left + 1;

    scr_box( top, left, top + 5, right, pal.desk, 0 );
    scr_ch( top, left + (wide - len - 2) / 2, ' ', pal.desk );
    scr_put( top, left + (wide - len - 2) / 2 + 1, title, pal.desk_title );
    scr_ch( top, left + (wide - len - 2) / 2 + 1 + len, ' ', pal.desk );
}

static void paint_status( int top )
{
    char line[48];
    u32 eighths = 0, percent = 0;
    int cell, len, fill;

    desk_box( top, STAT_LEFT, STAT_RIGHT, tx_status );
    sfmt( line, sizeof( line ), tx_cluster, shown_clus );
    scr_put( top + 1, STAT_LEFT + 2, line, pal.desk );
    if ( prog_total ) {
        percent = prog_done >= prog_total ? 100 : lo32( mul32( prog_done, 100 ) ) / prog_total;
        if ( prog_done < 0x00F00000ul ) {
            eighths = prog_done >= prog_total ? BAR_CELLS * 8 : prog_done * (BAR_CELLS * 8) / prog_total;
        } else {
            eighths = percent * (BAR_CELLS * 8) / 100;
        }
    }
    len = sfmt( line, sizeof( line ), "%u%%", percent );
    scr_put( top + 1, STAT_RIGHT - 1 - len, line, pal.desk );
    for ( cell = 0; cell < BAR_CELLS; cell++ ) {
        fill = (int)eighths - cell * 8;
        fill = fill < 0 ? 0 : fill > 8 ? 8 : fill;
        if ( gfx_on ) {
            scr_pic( top + 2, STAT_LEFT + 2 + cell, PIC_BAR, fill | ((pal.desk >> 4) << 4), 0x1F );
        } else {
            scr_ch( top + 2, STAT_LEFT + 2 + cell, fill >= 4 ? 0xDB : 0xB0, pal.desk );
        }
    }
    scr_flag( top + 2, STAT_LEFT + 2, 1, CF_LEFT );
    scr_flag( top + 2, STAT_LEFT + 1 + BAR_CELLS, 1, CF_RIGHT );
    sfmt( line, sizeof( line ), tx_elapsed, run_secs / 3600, run_secs / 60 % 60, run_secs % 60 );
    scr_center( top + 3, STAT_LEFT + 1, STAT_RIGHT - STAT_LEFT - 1, line, pal.desk );
    scr_center( top + 4, STAT_LEFT + 1, STAT_RIGHT - STAT_LEFT - 1, tx_method[opt.method], pal.desk );
}

/* a symbol of the map and what it stands for */
static void legend_item( int row, int col, int kind, const char *what )
{
    draw_block( row, col, kind );
    scr_put( row, col + 2, "- ", pal.desk );
    scr_put( row, col + 4, what, pal.desk );
}

static void paint_legend( int top )
{
    char line[48];

    desk_box( top, LEG_LEFT, LEG_RIGHT, tx_legend );
    legend_item( top + 1, LEG_LEFT + 2, BK_USED, tx_used );
    legend_item( top + 1, LEG_LEFT + 19, BK_UNUSED, tx_unused );
    legend_item( top + 2, LEG_LEFT + 2, BK_READ, tx_reading );
    legend_item( top + 2, LEG_LEFT + 19, BK_WRITE, tx_writing );
    legend_item( top + 3, LEG_LEFT + 2, BK_BAD, tx_bad );
    legend_item( top + 3, LEG_LEFT + 19, BK_FIXED, tx_fixed );
    if ( have_drive ) {
        sfmt( line, sizeof( line ), tx_scale, 'A' + vol.drive, per_block, per_block == 1 ? "" : "s" );
        scr_put( top + 4, LEG_LEFT + 2, line, pal.desk );
    }
}

static void paint_reading( void )
{
    int wide = 38, high = 7, top = (scr_rows - high) / 2, left = (79 - wide) / 2;

    ui_frame( top, left, top + high - 1, left + wide - 1, NULL, 0 );
    scr_center( top + 2, left + 1, wide - 2, reading_name[0] ? tx_analyzing : tx_reading_drive, pal.dlg );
    scr_center( top + 4, left + 1, wide - 2, reading_name, pal.dlg );
}

static void paint( void )
{
    const char *hint = ui_hint_now();
    int row, bottom = scr_rows - 1;

    for ( row = 1; row < bottom; row++ ) {
        scr_fill( row, 0, scr_cols, ' ', pal.desk );
    }
    scr_fill( 0, 0, scr_cols, ' ', pal.bar );
    ui_menu_bar( 0 );
    if ( running ) {
        scr_fill( 0, 59, 19, ' ', pal.bar_sel );
        scr_put( 0, 61, tx_esc_stops, pal.bar_sel );
    } else {
        scr_put( 0, 70, tx_f1, pal.bar );
    }
    paint_map();
    paint_status( scr_rows - 7 );
    paint_legend( scr_rows - 7 );
    if ( hint == NULL ) {
        hint = running && run_hint ? run_hint : tx_hint_menu;
    }
    scr_fill( bottom, 0, scr_cols, ' ', pal.hint );
    scr_put( bottom, 1, hint, pal.hint );
    scr_ch( bottom, 61, 0xB3, pal.hint_rule );
    scr_put( bottom, 63, tx_program, pal.hint );
    if ( reading ) {
        paint_reading();
    }
    scr_cursor( 0, 0, CUR_HIDE );
}

static void show( void )
{
    paint();
    scr_flush();
}

/* ------------------------------------------------------------------ */
/* the screen taken and given back                                     */
/* ------------------------------------------------------------------ */

void fe_start( void )
{
    key_init();
    scr_init( opt.graphics );
    mouse_init();
    map_rows = scr_rows - 9;
    if ( map_rows < 1 ) {
        map_rows = 1;
    }
    ui_backdrop( paint );
    show();
}

void fe_finish( void )
{
    scr_done();
}

void fe_suspend( void )
{
    scr_done();
}

void fe_resume( void )
{
    scr_init( opt.graphics );
    mouse_init();
    map_rows = scr_rows - 9;
    if ( map_rows < 1 ) {
        map_rows = 1;
    }
    show();
}

int fe_log_drive( void )
{
    return key_log_drive();
}

/* ------------------------------------------------------------------ */
/* a drive, and the work on it                                         */
/* ------------------------------------------------------------------ */

void fe_drive( int loaded )
{
    reading = 0;
    have_drive = loaded;
    act = ACT_IDLE;
    act_count = 0;
    prog_done = prog_total = 0;
    shown_clus = 2;
    if ( loaded ) {
        map_build( (u32)MAP_COLS * (u32)map_rows );
    }
    show();
}

void fe_reading( const char *what )
{
    reading = 1;
    str_cpyn( reading_name, what ? what : "", sizeof( reading_name ) );
    show();
}

static void clock_now( void )
{
    u32 now = sys_hundredths();

    if ( now < run_began ) {
        now += 8640000ul;               /* through midnight */
    }
    run_secs = (now - run_began) / 100;
}

void fe_run( int on )
{
    running = on;
    act = ACT_IDLE;
    act_count = 0;
    if ( on ) {
        run_began = sys_hundredths();
        run_secs = 0;
        prog_done = prog_total = 0;
        shown_clus = 2;
        run_hint = tx_act_read;
    }
    show();
}

void fe_activity( int what, u32 clus, u32 count )
{
    act = what;
    act_clus = clus;
    act_count = what == ACT_IDLE ? 0 : count;
    if ( what == ACT_IDLE || what == ACT_FAT ) {
        return;                         /* nothing new to see */
    }
    if ( count ) {
        shown_clus = clus;
    }
    run_hint = what == ACT_READ ? tx_act_read : what == ACT_SORT ? tx_act_sort : tx_act_write;
    if ( running ) {
        clock_now();
    }
    show();
}

void fe_progress( u32 done, u32 total )
{
    prog_done = done;
    prog_total = total;
}

/* Between two moves: has anyone asked for the work to stop?  Esc and
   ^C do, and so does the mouse on the words that say Esc does. */
int fe_stop_asked( void )
{
    static const BTN choice[] = { { tx_btn_cancel, 1 }, { tx_btn_resume, 2 } };
    char text[120];
    int key = sys_break_take() ? K_ESC : key_poll(), stop;

    if ( key == K_TICK || ui_common_key( key ) ) {
        return 0;
    }
    if ( key == K_MOUSE ) {
        if ( !running || mouse.row != 0 || mouse.col < 59
             || (mouse.kind != ME_DOWN && mouse.kind != ME_DOUBLE) ) {
            return 0;
        }
        key = K_ESC;
    }
    if ( key != K_ESC && key != K_CTRLC ) {
        return 0;
    }
    act_count = 0;
    sfmt( text, sizeof( text ), "%s\n\n%s", tx_stop_1, tx_stop_2 );
    ui_hint( tx_hint_button );
    stop = ui_ask( NULL, text, choice, 2, 10, 0, 2, 0 ) == 1;
    ui_hint( NULL );
    show();
    return stop;
}

/* ------------------------------------------------------------------ */
/* the menu                                                            */
/* ------------------------------------------------------------------ */

int fe_menu( void )
{
    int open = 1, key, go, letter;

    reading = 0;
    for ( ;; ) {
        if ( open ) {
            go = ui_menu( 1 );
            ui_hint( NULL );
            if ( go ) {
                show();
                return go;
            }
            open = 0;
        }
        show();
        key = key_get();
        if ( ui_common_key( key ) ) {
            continue;
        }
        if ( key == K_ALTTAP || key == K_F10 || (key == K_MOUSE && ui_menu_hit()) ) {
            open = 1;
            continue;
        }
        letter = key_alt_letter( key );
        if ( letter == 'O' ) {
            open = 1;
        } else if ( letter == 'B' ) {
            return GO_BEGIN;
        } else if ( letter == 'X' ) {
            return GO_EXIT;
        }
    }
}

/* ------------------------------------------------------------------ */
/* which drive                                                         */
/* ------------------------------------------------------------------ */

#define LIST_ROWS       5

int fe_pick_drive( int now )
{
    BTNROW btns;
    int drives[26], kinds[26], count = 0, sel = 0, first = 0, focus = 0;
    int wide = 42, high = 11, top = (scr_rows - high) / 2, left = (79 - wide) / 2;
    int drive, index, row, key, letter, answer = 0;
    char name[4];
    u8 attr;

    reading = 0;
    for ( drive = 0; drive < 26; drive++ ) {
        index = sys_drive_kind( drive );
        if ( index != DK_NONE ) {
            if ( drive == now ) {
                sel = count;
            }
            drives[count] = drive;
            kinds[count++] = index;
        }
    }
    if ( count == 0 ) {
        return -1;
    }
    btn_set( &btns, btn_ok_cancel, 2, 10, 0 );
    ui_hint( tx_hint_drive );
    name[1] = ':';
    name[2] = 0;

    while ( answer == 0 ) {
        if ( sel < first ) {
            first = sel;
        }
        if ( sel >= first + LIST_ROWS ) {
            first = sel - LIST_ROWS + 1;
        }
        ui_paint_under();
        ui_frame( top, left, top + high - 1, left + wide - 1, NULL, 0 );
        scr_put( top + 1, left + 2, tx_drive_title, pal.dlg );
        scr_box( top + 3, left + 4, top + 9, left + 17, pal.dlg, 0 );
        for ( row = 0; row < LIST_ROWS; row++ ) {
            index = first + row;
            if ( index >= count ) {
                break;
            }
            attr = index == sel ? pal.list_sel : pal.dlg;
            scr_fill( top + 4 + row, left + 5, 12, ' ', attr );
            if ( gfx_on ) {
                for ( drive = 0; drive < 3; drive++ ) {
                    scr_pic( top + 4 + row, left + 6 + drive, PIC_DRIVE,
                             drive | (kinds[index] == DK_FLOPPY ? 4 : 0), attr );
                }
            }
            name[0] = (char)('A' + drives[index]);
            scr_put( top + 4 + row, left + 11, name, attr );
        }
        scr_ch( top + 4, left + 17, 0x18, pal.track );
        for ( row = 1; row < LIST_ROWS - 1; row++ ) {
            scr_ch( top + 4 + row, left + 17, ' ', pal.track );
        }
        scr_ch( top + 3 + LIST_ROWS, left + 17, 0x19, pal.track );
        btns.focus = focus == 1;
        btn_draw_at( &btns, 0, top + 5, left + 24, pal.dlg );
        btn_draw_at( &btns, 1, top + 8, left + 24, pal.dlg );
        scr_cursor( 0, 0, CUR_HIDE );
        scr_flush();

        key = key_get();
        if ( ui_common_key( key ) ) {
            continue;
        }
        if ( key == K_ESC ) {
            answer = 2;
            break;
        }
        if ( key == K_TAB || key == K_SHTAB ) {
            focus = !focus;
            continue;
        }
        if ( key == K_MOUSE ) {
            row = mouse.row - (top + 4);
            if ( (mouse.kind == ME_DOWN || mouse.kind == ME_DOUBLE) && row >= 0 && row < LIST_ROWS ) {
                if ( mouse.col == left + 17 ) {
                    sel += row == 0 ? -1 : row == LIST_ROWS - 1 ? 1 : 0;
                } else if ( mouse.col > left + 4 && mouse.col < left + 17 && first + row < count ) {
                    sel = first + row;
                    focus = 0;
                    if ( mouse.kind == ME_DOUBLE ) {
                        answer = 1;
                    }
                }
            } else {
                answer = btn_key( &btns, key );
                if ( btns.armed >= 0 ) {
                    focus = 1;
                }
            }
        } else if ( focus == 1 ) {
            answer = btn_key( &btns, key );
        } else {
            switch ( key ) {
            case K_UP:      sel--;                  break;
            case K_DOWN:    sel++;                  break;
            case K_PGUP:    sel -= LIST_ROWS - 1;   break;
            case K_PGDN:    sel += LIST_ROWS - 1;   break;
            case K_HOME:    sel = 0;                break;
            case K_END:     sel = count - 1;        break;
            case K_ENTER:   answer = 1;             break;
            default:
                letter = key > ' ' && key < 0x100 ? ch_upper( key ) : 0;
                for ( index = 0; letter && index < count; index++ ) {
                    if ( drives[index] == letter - 'A' ) {
                        sel = index;
                    }
                }
                break;
            }
        }
        if ( sel < 0 ) {
            sel = 0;
        }
        if ( sel >= count ) {
            sel = count - 1;
        }
    }
    ui_hint( NULL );
    return answer == 1 ? drives[sel] : -1;
}

/* ------------------------------------------------------------------ */
/* which method, and what order                                        */
/* ------------------------------------------------------------------ */

int fe_pick_method( void )
{
    BTNROW btns;
    RADIO grp;
    TEXT about;
    int wide = 58, high = 11, top = (scr_rows - high) / 2, left = (79 - wide) / 2;
    int focus = 0, index, line, key, answer = 0;

    grp.label = tx_method;
    grp.count = 2;
    grp.chosen = grp.at = opt.method == METHOD_UNFRAG ? 1 : 0;
    grp.row = top + 2;
    grp.col = left + 2;
    grp.step = 3;
    grp.gap = 4;
    grp.wide = 24;
    btn_set( &btns, btn_ok_cancel, 2, 10, 0 );
    ui_hint( tx_hint_method );

    while ( answer == 0 ) {
        ui_paint_under();
        ui_frame( top, left, top + high - 1, left + wide - 1, tx_method_title, 0 );
        radio_draw( &grp, focus == 0, pal.dlg );
        for ( index = 0; index < 2; index++ ) {
            tx_init( &about );
            tx_wrap( &about, tx_method_about[index], 26 );
            for ( line = 0; line < about.count && line < 3; line++ ) {
                scr_put( top + 2 + index * 3 + line, left + 30, about.line[line], pal.dlg );
            }
            tx_free( &about );
        }
        btns.focus = focus == 1;
        btn_draw( &btns, top + 8, left + 1, wide - 2, 3, pal.dlg );
        scr_flush();

        key = key_get();
        if ( ui_common_key( key ) ) {
            continue;
        }
        if ( key == K_ESC || (key == K_MOUSE && ui_close_hit( top, left )) ) {
            answer = 2;
        } else if ( key == K_TAB || key == K_SHTAB ) {
            focus = !focus;
        } else if ( key == K_MOUSE ) {
            if ( radio_mouse( &grp ) ) {
                focus = 0;
            } else {
                answer = btn_key( &btns, key );
                if ( btns.armed >= 0 ) {
                    focus = 1;
                }
            }
        } else if ( focus == 0 && radio_key( &grp, key ) ) {
            /* moved, or chosen */
        } else {
            answer = btn_key( &btns, key );
        }
    }
    ui_hint( NULL );
    if ( answer != 1 ) {
        return 0;
    }
    opt.method = grp.chosen ? METHOD_UNFRAG : METHOD_FULL;
    return 1;
}

/* the line round a group of round buttons, and the group's name */
static void group_box( int top, int left, int bottom, int right, const char *title )
{
    scr_box( top, left, bottom, right, pal.dlg, 0 );
    scr_ch( top, left + 1, ' ', pal.dlg );
    scr_put( top, left + 2, title, pal.dlg_title );
    scr_ch( top, left + 2 + (int)str_len( title ), ' ', pal.dlg );
}

void fe_pick_sort( void )
{
    BTNROW btns;
    RADIO crit, order, *part;
    TEXT about;
    int wide = 34, high = 20, top = (scr_rows - high) / 2, left = (79 - wide) / 2;
    int focus = 0, line, key, answer = 0;

    crit.label = tx_sort;
    crit.count = 5;
    crit.chosen = crit.at = opt.sort;
    crit.row = top + 6;
    crit.col = left + 9;
    crit.step = 1;
    crit.gap = 5;
    crit.wide = 17;
    order.label = tx_order;
    order.count = 2;
    order.chosen = order.at = opt.descending ? 1 : 0;
    order.row = top + 13;
    order.col = left + 9;
    order.step = 1;
    order.gap = 5;
    order.wide = 17;
    btn_set( &btns, btn_ok_cancel, 2, 10, 0 );
    ui_hint( tx_hint_sort );
    tx_init( &about );
    tx_wrap( &about, tx_sort_about, wide - 4 );

    while ( answer == 0 ) {
        ui_paint_under();
        ui_frame( top, left, top + high - 1, left + wide - 1, tx_sort_title, 0 );
        for ( line = 0; line < about.count && line < 2; line++ ) {
            scr_center( top + 2 + line, left + 1, wide - 2, about.line[line], pal.dlg );
        }
        group_box( top + 5, left + 7, top + 11, left + 26, tx_sort_by );
        radio_draw( &crit, focus == 0, pal.dlg );
        group_box( top + 12, left + 7, top + 15, left + 26, tx_sort_order );
        radio_draw( &order, focus == 1, pal.dlg );
        btns.focus = focus == 2;
        btn_draw( &btns, top + 17, left + 1, wide - 2, 3, pal.dlg );
        scr_flush();

        key = key_get();
        if ( ui_common_key( key ) ) {
            continue;
        }
        part = focus == 0 ? &crit : &order;
        if ( key == K_ESC || (key == K_MOUSE && ui_close_hit( top, left )) ) {
            answer = 2;
        } else if ( key == K_TAB ) {
            focus = (focus + 1) % 3;
        } else if ( key == K_SHTAB ) {
            focus = (focus + 2) % 3;
        } else if ( key == K_MOUSE ) {
            if ( radio_mouse( &crit ) ) {
                focus = 0;
            } else if ( radio_mouse( &order ) ) {
                focus = 1;
            } else {
                answer = btn_key( &btns, key );
                if ( btns.armed >= 0 ) {
                    focus = 2;
                }
            }
        } else if ( focus < 2 && radio_key( part, key ) ) {
            /* moved, or chosen */
        } else {
            answer = btn_key( &btns, key );
        }
    }
    tx_free( &about );
    ui_hint( NULL );
    if ( answer == 1 ) {
        opt.sort = crit.chosen;
        opt.descending = order.chosen;
    }
}

/* ------------------------------------------------------------------ */
/* things that are only said                                           */
/* ------------------------------------------------------------------ */

void fe_legend( void )
{
    static const int kinds[7] = { BK_USED, BK_DONE, BK_UNUSED, BK_FIXED, BK_BAD, BK_READ, BK_WRITE };
    BTNROW btns;
    int wide = 57, high = 14, top = (scr_rows - high) / 2, left = (79 - wide) / 2;
    int index, row, key, answer = 0;

    btn_set( &btns, btn_ok, 1, 10, 0 );
    ui_hint( tx_hint_legend );
    while ( answer == 0 ) {
        ui_paint_under();
        /* this one is in the screen's own colours */
        if ( gfx_on ) {
            for ( row = top; row < top + high; row++ ) {
                scr_fill( row, left, wide, ' ', pal.desk );
                scr_flag( row, left, 1, CF_LEFT );
                scr_flag( row, left + wide - 1, 1, CF_RIGHT );
            }
            scr_flag( top, left, wide, CF_TOP );
            scr_flag( top + high - 1, left, wide, CF_BOTTOM );
        } else {
            scr_box( top, left, top + high - 1, left + wide - 1, pal.desk, 1 );
        }
        scr_shadow( top, left, top + high - 1, left + wide - 1 );
        scr_center( top + 1, left + 1, wide - 2, tx_legend_title, pal.desk_title );
        scr_fill( top + 2, left + 1, wide - 3, 0xC4, pal.desk );
        for ( index = 0; index < 7; index++ ) {
            draw_block( top + 3 + index, left + 3, kinds[index] );
            scr_put( top + 3 + index, left + 6, "- ", pal.desk );
            scr_put( top + 3 + index, left + 8, tx_legend_line[index], pal.desk );
        }
        btn_draw( &btns, top + 11, left + 1, wide - 2, 3, pal.desk );
        scr_flush();

        key = key_get();
        if ( ui_common_key( key ) ) {
            continue;
        }
        answer = key == K_ESC ? 1 : btn_key( &btns, key );
    }
    ui_hint( NULL );
}

void fe_about( void )
{
    char text[200];

    sfmt( text, sizeof( text ), "%s\n\n%s\n%s", tx_about_line[0], tx_about_line[1], tx_about_line[2] );
    ui_hint( tx_hint_button );
    ui_ask( tx_about_title, text, btn_ok, 1, 10, 0, 1, 0 );
    ui_hint( NULL );
}

int fe_recommend( int percent, int method )
{
    static const BTN choice[] = { { tx_btn_optimize, 1 }, { tx_btn_configure, 2 } };
    BTNROW btns;
    char line[60];
    int wide = 38, high = 10, top = (scr_rows - high) / 2, left = (79 - wide) / 2;
    int key, answer = 0;

    reading = 0;
    btn_set( &btns, choice, 2, 13, 0 );
    ui_hint( tx_hint_button );
    while ( answer == 0 ) {
        ui_paint_under();
        ui_frame( top, left, top + high - 1, left + wide - 1, tx_recommend_title, 0 );
        sfmt( line, sizeof( line ), tx_recommend_1, (u32)percent, 'A' + vol.drive );
        scr_center( top + 2, left + 1, wide - 2, line, pal.dlg );
        scr_center( top + 4, left + 1, wide - 2, tx_recommend_2, pal.dlg );
        /* half a cell to the right of the middle, where MS-DOS has it */
        scr_center( top + 5, left + 2, wide - 2, tx_method[method], pal.dlg_title );
        btn_draw( &btns, top + 7, left + 1, wide - 2, 3, pal.dlg );
        scr_flush();

        key = key_get();
        if ( ui_common_key( key ) ) {
            continue;
        }
        if ( key == K_ESC || (key == K_MOUSE && ui_close_hit( top, left )) ) {
            answer = 2;
        } else {
            answer = btn_key( &btns, key );
        }
    }
    ui_hint( NULL );
    return answer == 1;
}

int fe_fs_error( const char *text )
{
    static const BTN choice[] = { { tx_btn_scandisk, 1 }, { tx_btn_leave, 2 } };
    int answer;

    reading = 0;
    ui_hint( tx_hint_button );
    answer = ui_ask( tx_error_title, text, choice, 2, 12, 0, 2, 1 );
    ui_hint( NULL );
    return answer == 1;
}

void fe_message( const char *text, int error )
{
    reading = 0;
    ui_hint( tx_hint_button );
    ui_ask( error ? tx_error_title : NULL, text, btn_ok, 1, 10, 0, 1, error );
    ui_hint( NULL );
    show();
}

void fe_finished( void )
{
    act = ACT_IDLE;
    act_count = 0;
    if ( opt.unattended ) {
        show();
        return;                         /* nobody is asked: MAIN.C leaves */
    }
    ui_hint( tx_act_done );
    ui_ask( NULL, tx_finished, btn_ok, 1, 10, 0, 1, 0 );
    ui_hint( NULL );
}

int fe_done( void )
{
    static const BTN choice[] = {
        { tx_btn_another, 1 }, { tx_btn_configure, 2 }, { tx_btn_exit, 3 }
    };
    char text[240];
    int answer;

    sfmt( text, sizeof( text ), tx_done_1, 'A' + vol.drive );
    str_catn( text, "\n\n", sizeof( text ) );
    str_catn( text, tx_done_2, sizeof( text ) );
    ui_hint( tx_hint_button );
    answer = ui_ask( NULL, text, choice, 3, 19, 0, 2, 0 );
    ui_hint( NULL );
    return answer == 1 ? DONE_ANOTHER : answer == 3 ? DONE_EXIT : DONE_CONFIGURE;
}
