/*
 * VIEW.C - MPLAY's screen: the plan, the drawing, and where the mouse
 * is (MPLAY.H).
 *
 * ONE PLAN FOR EVERY SCREEN.  From the top: the menu bar (under a
 * title bar on a graphics screen), three rows of what is playing, the
 * analyzer, the seek bar, the buttons, and then the equalizer beside
 * the playlist, down to the status bar.  The analyzer and the lower
 * part share what rows there are, two to three, so 25 lines of text
 * and 1280x1024 are the same screen at two sizes.
 *
 * ON A GRAPHICS SCREEN THE MOVING PARTS ARE PANES: the time, the
 * analyzer, the seek bar, the buttons and the equalizer are painted
 * in pixels, and only what changed is painted again, a tenth of a
 * second at a time.  Everything else is cells, and so is the whole of
 * a text screen, which draws the same things with block characters.
 *
 * THE ANALYZER LOOKS LIKE A HI-FI'S: bars of lit segments, green, then
 * yellow, then red near the top, with the unlit ones dim; each bar
 * falls back slowly, and a peak mark waits at the highest it reached
 * before it falls too.  The oscilloscope draws the wave itself.
 *
 * THE EQUALIZER SHOWS ITS CURVE.  Behind the eleven sliders - the
 * preamp and the ten bands - is the response they make, drawn from
 * the same eq_gain_at the codecs use, so what is seen is what is
 * heard.
 */
#include "mplay.h"
#include "fix.h"

PLAN plan;
int focus = FO_LIST;
int eq_pick = 1;

enum { PN_LCD, PN_AN, PN_SEEK, PN_TRANS, PN_EQ };

#define BARS_MAX        64
#define LCD_COLS        18              /* the time's width, in cells */

static u8  bar_now[BARS_MAX];           /* the height shown, 0..255 */
static u8  bar_peak[BARS_MAX];
static u8  bar_hold[BARS_MAX];          /* frames the peak waits */
static int bar_count = 32;
static s16 scope[640];
static int scope_count;
static const char *status_text;
static u32 shown_sec = 0xFFFFFFFFUL;
static int shown_seek = -1;
static int shown_state = -1;
static u32 sig_eq, sig_tr, sig_lcd;

/* colors, chosen for the screen in view_layout */
static struct {
    u8 an_bg, an_grid;
    u8 lit[3], dim[3];                  /* green, yellow, red */
    u8 peak;
    u8 scope;
    u8 lcd_bg, lcd_on, lcd_off;
    u8 groove, fill, curve, zero;
} col;

/* the transport buttons, left to right */
static void paint_lcd( int pane, int x0, int y0, int x1, int y1 );
static void paint_an( int pane, int x0, int y0, int x1, int y1 );
static void paint_seek( int pane, int x0, int y0, int x1, int y1 );
static void paint_trans( int pane, int x0, int y0, int x1, int y1 );
static void paint_eq( int pane, int x0, int y0, int x1, int y1 );

static const int tr_cmds[] = { CM_PREV, CM_BACK, CM_PLAYPAUSE, CM_STOP, CM_FWD, CM_NEXT, CM_REPEAT_CYCLE, CM_SHUFFLE };
#define TR_COUNT        8
#define TR_WIDTH        5               /* cells a button, with a gap */

static const char *const eq_label[EQ_BANDS + 1] = {
    "Pre", "31", "62", "125", "250", "500", "1K", "2K", "4K", "8K", "16K"
};

/* ------------------------------------------------------------------ */
/* the plan                                                            */
/* ------------------------------------------------------------------ */
void view_init( void )
{
    mem_set( bar_now, 0, sizeof( bar_now ) );
    mem_set( bar_peak, 0, sizeof( bar_peak ) );
    pane_define( PN_LCD, paint_lcd );
    pane_define( PN_AN, paint_an );
    pane_define( PN_SEEK, paint_seek );
    pane_define( PN_TRANS, paint_trans );
    pane_define( PN_EQ, paint_eq );
}

static void pick_colors( void )
{
    if ( gfx_on && gfx_colors == 256 ) {
        col.an_bg = (u8)gfx_rgb( 0x000000UL );
        col.an_grid = (u8)gfx_rgb( 0x0A1A0AUL );
        col.lit[0] = (u8)gfx_rgb( 0x33FF33UL );
        col.lit[1] = (u8)gfx_rgb( 0xFFFF33UL );
        col.lit[2] = (u8)gfx_rgb( 0xFF3333UL );
        col.dim[0] = (u8)gfx_rgb( 0x003300UL );
        col.dim[1] = (u8)gfx_rgb( 0x333300UL );
        col.dim[2] = (u8)gfx_rgb( 0x330000UL );
        col.peak = (u8)gfx_rgb( 0xFFFFFFUL );
        col.scope = (u8)gfx_rgb( 0x33FFFFUL );
        col.lcd_bg = (u8)gfx_rgb( 0x000000UL );
        col.lcd_on = (u8)gfx_rgb( 0x33FF66UL );
        col.lcd_off = (u8)gfx_rgb( 0x002200UL );
        col.groove = (u8)gfx_rgb( 0x333333UL );
        col.fill = (u8)gfx_rgb( 0x0066CCUL );
        col.curve = (u8)gfx_rgb( 0x0099FFUL );
        col.zero = (u8)gfx_rgb( 0x999999UL );
    } else {
        col.an_bg = 0;
        col.an_grid = 0;
        col.lit[0] = 10;
        col.lit[1] = 14;
        col.lit[2] = 12;
        col.dim[0] = 2;
        col.dim[1] = 6;
        col.dim[2] = 4;
        col.peak = 15;
        col.scope = 11;
        col.lcd_bg = 0;
        col.lcd_on = 10;
        col.lcd_off = 2;
        col.groove = 8;
        col.fill = 1;
        col.curve = 9;
        col.zero = 8;
    }
}

void view_layout( void )
{
    int row = 0, fixed, flex, an, low;

    pick_colors();
    plan.title_row = gfx_on ? row++ : -1;
    plan.menu_row = row++;
    plan.info_top = row;
    plan.info_rows = 3;
    row += 3;
    plan.status_row = scr_rows - 1;
    plan.tr_rows = gfx_on ? 2 : 1;
    fixed = row + 1 + plan.tr_rows + 1;        /* the seek bar, the buttons, the status bar */
    flex = scr_rows - fixed;
    if ( flex < 6 ) {
        flex = 6;
    }
    an = flex * 2 / 5;
    if ( set.analyzer == AN_OFF ) {
        an = 0;
    } else if ( an < 4 ) {
        an = flex < 8 ? flex / 2 : 4;
    }
    if ( !set.show_eq && !set.show_list && set.analyzer != AN_OFF ) {
        an = flex;                              /* nothing below: the analyzer has it all */
    }
    low = flex - an;
    plan.an_top = row;
    plan.an_rows = an;
    row += an;
    plan.seek_row = row++;
    plan.tr_top = row;
    row += plan.tr_rows;
    plan.low_top = row;
    plan.low_rows = low;
    plan.eq_cols = 0;
    plan.pl_cols = 0;
    if ( low >= 4 ) {
        if ( set.show_eq && set.show_list ) {
            plan.eq_cols = scr_cols * 11 / 20;
            if ( plan.eq_cols < 50 ) {
                plan.eq_cols = scr_cols >= 80 ? 50 : scr_cols / 2;
            }
            plan.pl_cols = scr_cols - plan.eq_cols;
        } else if ( set.show_eq ) {
            plan.eq_cols = scr_cols;
        } else if ( set.show_list ) {
            plan.pl_cols = scr_cols;
        }
    }
    plan.eq_left = 0;
    plan.pl_left = plan.eq_cols;
    if ( focus == FO_EQ && plan.eq_cols == 0 ) {
        focus = FO_LIST;
    }
    if ( focus == FO_LIST && plan.pl_cols == 0 && plan.eq_cols ) {
        focus = FO_EQ;
    }
    /* the analyzer's bars: as many as fit at a sensible width */
    if ( gfx_on ) {
        bar_count = (scr_cols * CELL_W - 16) / 10;
    } else {
        bar_count = (scr_cols - 2) / 3;
    }
    if ( bar_count > BARS_MAX ) {
        bar_count = BARS_MAX;
    }
    if ( bar_count < 8 ) {
        bar_count = 8;
    }
}

void view_status( const char *text )
{
    status_text = text;
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */
static void time_text( char *buf, u32 max, u32 ms )
{
    u32 sec = ms / 1000;

    if ( sec >= 3600 ) {
        sfmt( buf, max, "%u:%02u:%02u", sec / 3600, (sec / 60) % 60, sec % 60 );
    } else {
        sfmt( buf, max, "%u:%02u", sec / 60, sec % 60 );
    }
}

static int is_playing( void )
{
    PLSTAT st;

    pl_status( &st );
    return st.state == PS_PLAYING;
}

/* a button's cells on a text screen */
static void text_button( int row, int col, const char *label, int lit )
{
    u8 attr = lit ? pal.menu_sel : pal.button;

    scr_ch( row, col, '[', pal.desk );
    scr_putw( row, col + 1, label, 3, attr );
    scr_ch( row, col + 4, ']', pal.desk );
}

/* the value of a slider row on a text screen: top row +12 dB, bottom -12 */
static int db_of_row( int row, int top, int rows )
{
    int span = rows - 1, at = row - top;

    if ( span < 1 ) {
        return 0;
    }
    if ( at < 0 ) {
        at = 0;
    }
    if ( at > span ) {
        at = span;
    }
    return (span - 2 * at) * EQ_MAXDB * 2 / span;      /* half decibels */
}

static int row_of_db( int half_db, int top, int rows )
{
    int span = rows - 1;

    return top + (EQ_MAXDB * 2 - half_db) * span / (EQ_MAXDB * 4);
}

/* ------------------------------------------------------------------ */
/* the time, as an LCD (graphics)                                      */
/* ------------------------------------------------------------------ */
/* the seven segments of 0-9, a bit each: a top, b top right, c bottom
   right, d bottom, e bottom left, f top left, g middle */
static const u8 seg_of[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

static void lcd_digit( int x, int y, int w, int h, int digit )
{
    int t = w / 5 > 2 ? w / 5 : 2, half = h / 2, seg, i;
    u8 on;

    seg = digit >= 0 && digit <= 9 ? seg_of[digit] : 0;
    for ( i = 0; i < 7; i++ ) {
        on = (seg >> i) & 1 ? col.lcd_on : col.lcd_off;
        switch ( i ) {
        case 0: gfx_rect( x + t, y, w - 2 * t, t, on ); break;
        case 1: gfx_rect( x + w - t, y + t, t, half - t, on ); break;
        case 2: gfx_rect( x + w - t, y + half + 1, t, half - t - 1, on ); break;
        case 3: gfx_rect( x + t, y + h - t, w - 2 * t, t, on ); break;
        case 4: gfx_rect( x, y + half + 1, t, half - t - 1, on ); break;
        case 5: gfx_rect( x, y + t, t, half - t, on ); break;
        default: gfx_rect( x + t, y + half - t / 2, w - 2 * t, t, on ); break;
        }
    }
}

static void paint_lcd( int pane, int x0, int y0, int x1, int y1 )
{
    int x, y, w, h, dw, dh, i, len;
    PLSTAT st;
    char text[16];

    (void)x0; (void)y0; (void)x1; (void)y1;
    if ( !pane_rect( pane, &x, &y, &w, &h ) ) {
        return;
    }
    gfx_rect( x, y, w, h, col.lcd_bg );
    gfx_frame( x, y, w, h, FR_SUNKEN );
    pl_status( &st );
    time_text( text, sizeof( text ), st.pos_ms );
    len = (int)str_len( text );
    dh = h - 12;
    dw = dh / 2;
    x += 8;
    y += 6;
    for ( i = 0; i < len; i++ ) {
        if ( text[i] == ':' ) {
            gfx_rect( x + 2, y + dh / 3, 3, 3, col.lcd_on );
            gfx_rect( x + 2, y + dh * 2 / 3, 3, 3, col.lcd_on );
            x += 8;
        } else {
            lcd_digit( x, y, dw, dh, text[i] - '0' );
            x += dw + 5;
        }
    }
    /* the state, small, at the right */
    x = x + 4;
    if ( st.state == PS_PLAYING ) {
        for ( i = 0; i < 10; i++ ) {
            gfx_vline( x + i, y + dh / 2 - (10 - i) / 2, 10 - i, col.lcd_on );
        }
    } else if ( st.state == PS_PAUSED ) {
        gfx_rect( x, y + dh / 2 - 5, 3, 10, col.lcd_on );
        gfx_rect( x + 6, y + dh / 2 - 5, 3, 10, col.lcd_on );
    } else {
        gfx_rect( x, y + dh / 2 - 4, 9, 9, col.lcd_off );
    }
}

/* ------------------------------------------------------------------ */
/* the analyzer                                                        */
/* ------------------------------------------------------------------ */
static void paint_an( int pane, int x0, int y0, int x1, int y1 )
{
    int x, y, w, h, bar, bw, gap, bx, row, seg, segs, lit, peak, band, i, py, prev;
    int margin;
    u8 *line, color;
    int left, right;

    if ( !pane_rect( pane, &x, &y, &w, &h ) ) {
        return;
    }
    if ( set.analyzer == AN_SCOPE ) {
        gfx_rect( x0, y0, x1 - x0 + 1, y1 - y0 + 1, col.an_bg );
        gfx_hline( x, y + h / 2, w, col.an_grid ? col.an_grid : 8 );
        if ( scope_count > 1 ) {
            prev = y + h / 2;
            for ( i = 0; i < w - 4; i++ ) {
                py = y + h / 2 - (int)(((long)scope[(long)i * scope_count / (w - 4)] * (h / 2 - 2)) / 32768L);
                if ( i > 0 ) {
                    gfx_vline( x + 2 + i, py < prev ? py : prev, (py < prev ? prev - py : py - prev) + 1, col.scope );
                }
                prev = py;
            }
        }
        return;
    }
    /* segments of 4 pixels, three lit and one gap, from the bottom up */
    segs = (h - 4) / 4;
    gap = 2;
    bw = (w - 8) / bar_count - gap;
    if ( bw < 2 ) {
        bw = 2;
    }
    /* the bars centered: what does not divide evenly is shared by the two ends */
    margin = (w - bar_count * (bw + gap) + gap) / 2;
    if ( margin < 4 ) {
        margin = 4;
    }
    for ( row = y0; row <= y1; row++ ) {
        line = gfx_row( row );
        if ( line == NULL ) {
            continue;
        }
        left = x0;
        right = x1;
        if ( left < 0 ) {
            left = 0;
        }
        if ( right >= gfx_width ) {
            right = gfx_width - 1;
        }
        mem_set( line + left, col.an_bg, (u32)(right - left + 1) );
        seg = (y + h - 3 - row) / 4;                    /* the segment this row is in */
        if ( row > y + h - 3 || row < y + 2 || seg >= segs || ((y + h - 3 - row) & 3) == 3 ) {
            continue;                                   /* a gap between segments */
        }
        band = seg * 100 / (segs > 0 ? segs : 1);
        band = band < 60 ? 0 : band < 85 ? 1 : 2;
        for ( bar = 0; bar < bar_count; bar++ ) {
            bx = x + margin + bar * (bw + gap);
            if ( bx > right || bx + bw - 1 < left ) {
                continue;
            }
            lit = bar_now[bar] * segs / 256;
            peak = bar_peak[bar] * segs / 256;
            if ( seg < lit ) {
                color = col.lit[band];
            } else if ( set.analyzer == AN_PEAKS && seg == peak && peak > 0 ) {
                color = col.peak;
            } else {
                color = col.dim[band];
            }
            for ( i = bx; i < bx + bw; i++ ) {
                if ( i >= left && i <= right ) {
                    line[i] = color;
                }
            }
        }
    }
    gfx_dirty( x0, y0, x1, y1 );
}

/* the bars on a text screen: a block a level, half blocks at the top */
static void draw_an_text( void )
{
    int bar, row, col0, rows = plan.an_rows, level, cells, band;
    u8 attr;

    for ( row = 0; row < rows; row++ ) {
        scr_fill( plan.an_top + row, 0, scr_cols, ' ', ATTR( 7, 0 ) );
    }
    if ( set.analyzer == AN_SCOPE ) {
        for ( col0 = 1; col0 < scr_cols - 1; col0++ ) {
            level = scope_count ? scope[(long)(col0 - 1) * scope_count / (scr_cols - 2)] : 0;
            row = rows / 2 - (int)((long)level * rows / 65536L);
            if ( row < 0 ) {
                row = 0;
            }
            if ( row >= rows ) {
                row = rows - 1;
            }
            scr_ch( plan.an_top + row, col0, 0xFE, ATTR( 11, 0 ) );
        }
        return;
    }
    for ( bar = 0; bar < bar_count; bar++ ) {
        col0 = 1 + bar * 3;
        level = bar_now[bar] * rows * 2 / 256;          /* half cells */
        for ( row = 0; row < rows; row++ ) {
            cells = row * 2;                            /* half cells below this row */
            band = row * 100 / rows;
            band = band < 60 ? 0 : band < 85 ? 1 : 2;
            if ( level >= cells + 2 ) {
                attr = ATTR( col.lit[band], 0 );
                scr_fill( plan.an_top + rows - 1 - row, col0, 2, 0xDB, attr );
            } else if ( level == cells + 1 ) {
                attr = ATTR( col.lit[band], 0 );
                scr_fill( plan.an_top + rows - 1 - row, col0, 2, 0xDC, attr );
            } else if ( set.analyzer == AN_PEAKS && bar_peak[bar] * rows / 256 == row && bar_peak[bar] > 8 ) {
                scr_fill( plan.an_top + rows - 1 - row, col0, 2, 0xDC, ATTR( 15, 0 ) );
            } else {
                scr_fill( plan.an_top + rows - 1 - row, col0, 2, 0xFA, ATTR( col.dim[band], 0 ) );
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* the seek bar                                                        */
/* ------------------------------------------------------------------ */
/* where in the bar's width the position is, 0..width */
static int seek_at( int width )
{
    PLSTAT st;

    pl_status( &st );
    if ( st.len_ms == 0 || width <= 0 ) {
        return 0;
    }
    if ( st.pos_ms >= st.len_ms ) {
        return width;
    }
    return (int)fx_umuldiv( st.pos_ms, (u32)width, st.len_ms );
}

static void paint_seek( int pane, int x0, int y0, int x1, int y1 )
{
    int x, y, w, h, at, knob;

    (void)x0; (void)y0; (void)x1; (void)y1;
    if ( !pane_rect( pane, &x, &y, &w, &h ) ) {
        return;
    }
    gfx_rect( x, y, w, h, pal.px_face );
    gfx_frame( x + 4, y + 5, w - 8, h - 10, FR_SUNKEN );
    gfx_rect( x + 5, y + 6, w - 10, h - 12, col.groove );
    at = seek_at( w - 10 );
    if ( at > 0 ) {
        gfx_rect( x + 5, y + 6, at, h - 12, col.fill );
    }
    knob = x + 5 + at - 4;
    if ( knob < x + 4 ) {
        knob = x + 4;
    }
    gfx_rect( knob, y + 2, 9, h - 4, pal.px_face );
    gfx_frame( knob, y + 2, 9, h - 4, FR_BUTTON );
}

static void draw_seek_text( void )
{
    int width = scr_cols - 2, at = seek_at( width - 1 );

    scr_fill( plan.seek_row, 0, scr_cols, ' ', pal.desk );
    scr_fill( plan.seek_row, 1, width, 0xB0, ATTR( 8, 0 ) );
    if ( at > 0 ) {
        scr_fill( plan.seek_row, 1, at, 0xDB, ATTR( 9, 0 ) );
    }
    scr_ch( plan.seek_row, 1 + at, 0xDB, ATTR( 15, 0 ) );
}

/* ------------------------------------------------------------------ */
/* the buttons                                                         */
/* ------------------------------------------------------------------ */
static const char *const icon_play[16] = {
    "................", "................", "....##..........", "....####........",
    "....######......", "....########....", "....##########..", "....############",
    "....############", "....##########..", "....########....", "....######......",
    "....####........", "....##..........", "................", "................"
};
static const char *const icon_pause[16] = {
    "................", "................", "...####..####...", "...####..####...",
    "...####..####...", "...####..####...", "...####..####...", "...####..####...",
    "...####..####...", "...####..####...", "...####..####...", "...####..####...",
    "...####..####...", "...####..####...", "................", "................"
};
static const char *const icon_stop[16] = {
    "................", "................", "................", "...##########...",
    "...##########...", "...##########...", "...##########...", "...##########...",
    "...##########...", "...##########...", "...##########...", "...##########...",
    "...##########...", "................", "................", "................"
};
static const char *const icon_prev[16] = {
    "................", "................", "..##.........##.", "..##.......####.",
    "..##.....######.", "..##...########.", "..##.##########.", "..#############.",
    "..#############.", "..##.##########.", "..##...########.", "..##.....######.",
    "..##.......####.", "..##.........##.", "................", "................"
};
static const char *const icon_next[16] = {
    "................", "................", ".##.........##..", ".####.......##..",
    ".######.....##..", ".########...##..", ".##########.##..", ".#############..",
    ".#############..", ".##########.##..", ".########...##..", ".######.....##..",
    ".####.......##..", ".##.........##..", "................", "................"
};
static const char *const icon_back[16] = {
    "................", "................", "......##......##", "....####....####",
    "..######..######", "################", "################", "################",
    "################", "################", "..######..######", "....####....####",
    "......##......##", "................", "................", "................"
};
static const char *const icon_fwd[16] = {
    "................", "................", "##......##......", "####....####....",
    "######..######..", "################", "################", "################",
    "################", "################", "######..######..", "####....####....",
    "##......##......", "................", "................", "................"
};
static const char *const icon_repeat[16] = {
    "................", "................", "....########....", "...#........#...",
    "..#..........#..", "..#..........#..", "..#.........###.", "..#..........#..",
    "..###.........#.", "...#..........#.", "...#..........#.", "....#........#..",
    ".....########...", "................", "................", "................"
};
static const char *const icon_shuffle[16] = {
    "................", "................", "............#...", "##.........###..",
    "..##.....##..#..", "....##.##.......", ".....###........", ".....###........",
    "....##.##.......", "..##.....##..#..", "##.........###..", "............#...",
    "................", "................", "................", "................"
};

static const char *const *const tr_icons[TR_COUNT] = {
    icon_prev, icon_back, icon_play, icon_stop, icon_fwd, icon_next, icon_repeat, icon_shuffle
};

static int tr_lit( int index )
{
    if ( tr_cmds[index] == CM_REPEAT_CYCLE ) {
        return set.repeat != RP_OFF;
    }
    if ( tr_cmds[index] == CM_SHUFFLE ) {
        return set.shuffle;
    }
    return 0;
}

/* the volume slider: from this column to the end of the row */
static int vol_col( void )
{
    return 2 + TR_COUNT * TR_WIDTH + 2;
}

static void paint_trans( int pane, int x0, int y0, int x1, int y1 )
{
    int x, y, w, h, index, bx, vx, vw, at, playing = is_playing();

    (void)x0; (void)y0; (void)x1; (void)y1;
    if ( !pane_rect( pane, &x, &y, &w, &h ) ) {
        return;
    }
    gfx_rect( x, y, w, h, pal.px_face );
    for ( index = 0; index < TR_COUNT; index++ ) {
        bx = x + (2 + index * TR_WIDTH) * CELL_W;
        gfx_rect( bx, y + 2, (TR_WIDTH - 1) * CELL_W, h - 4, pal.px_face );
        gfx_frame( bx, y + 2, (TR_WIDTH - 1) * CELL_W, h - 4, tr_lit( index ) ? FR_PRESSED : FR_BUTTON );
        gfx_icon( bx + ((TR_WIDTH - 1) * CELL_W - 16) / 2, y + (h - 16) / 2,
                  tr_cmds[index] == CM_PLAYPAUSE && playing ? icon_pause : tr_icons[index], 0, 0 );
    }
    /* the volume: a groove that fills, and a knob */
    vx = x + vol_col() * CELL_W + 7 * CELL_W;
    vw = w - (vx - x) - 2 * CELL_W;
    gfx_text( x + vol_col() * CELL_W, y + (h - CELL_H) / 2, "Volume", 6, 0, -1 );
    if ( vw > 20 ) {
        gfx_frame( vx, y + h / 2 - 3, vw, 6, FR_SUNKEN );
        at = set.volume * (vw - 10) / 100;
        gfx_rect( vx + 1, y + h / 2 - 2, at + 4, 4, col.fill );
        gfx_rect( vx + at, y + 4, 10, h - 8, pal.px_face );
        gfx_frame( vx + at, y + 4, 10, h - 8, FR_BUTTON );
    }
}

static void draw_trans_text( void )
{
    static const char *const labels[TR_COUNT] = { "|<", "<<", ">", "#", ">>", ">|", "Rpt", "Shf" };
    int index, playing = is_playing(), at, width;
    char text[16];

    scr_fill( plan.tr_top, 0, scr_cols, ' ', pal.desk );
    for ( index = 0; index < TR_COUNT; index++ ) {
        text_button( plan.tr_top, 2 + index * TR_WIDTH,
                     tr_cmds[index] == CM_PLAYPAUSE && playing ? "||" : labels[index], tr_lit( index ) );
    }
    scr_put( plan.tr_top, vol_col(), "Vol", pal.desk );
    width = scr_cols - vol_col() - 10;
    if ( width > 4 ) {
        at = set.volume * width / 100;
        scr_fill( plan.tr_top, vol_col() + 4, width, 0xFA, ATTR( 8, 0 ) );
        scr_fill( plan.tr_top, vol_col() + 4, at, 0xFE, ATTR( 10, 0 ) );
        sfmt( text, sizeof( text ), "%3u%%", (u32)set.volume );
        scr_put( plan.tr_top, vol_col() + 5 + width, text, pal.desk );
    }
}

/* ------------------------------------------------------------------ */
/* the equalizer                                                       */
/* ------------------------------------------------------------------ */
/* the slider columns: the first cell of each, in the equalizer's area */
static int eq_slider_col( int index )
{
    /* spread over the width there is: the preamp at column 5, the 16 kHz
       band four from the right edge, a column more between the preamp
       and the bands; at least three columns apart, for "125" */
    int span = plan.eq_cols - 10;

    if ( span < EQ_BANDS * 3 ) {
        span = EQ_BANDS * 3;
    }
    return plan.eq_left + 5 + index * span / EQ_BANDS + (index > 0 ? 1 : 0);
}

static int eq_value( int index )
{
    return index == 0 ? set.eq.preamp : set.eq.band[index - 1];
}

static int eq_track_top( void )
{
    return plan.low_top + 1;
}

static int eq_track_rows( void )
{
    return plan.low_rows - 3;
}

static void paint_eq( int pane, int x0, int y0, int x1, int y1 )
{
    int x, y, w, h, index, sx, sy, top, bottom, mid, k, prev_y, py, rows;
    u32 hz, gain;
    s32 db256;
    int knob_w = 14;

    (void)x0; (void)y0; (void)x1; (void)y1;
    if ( !pane_rect( pane, &x, &y, &w, &h ) ) {
        return;
    }
    gfx_rect( x, y, w, h, pal.px_face );
    rows = eq_track_rows();
    top = eq_track_top() * CELL_H + 4;
    bottom = (eq_track_top() + rows) * CELL_H - 4;
    mid = (top + bottom) / 2;
    /* the display behind the sliders: dark, with the 0 dB line and the curve */
    gfx_rect( x + 4 * CELL_W, top - 2, w - 5 * CELL_W, bottom - top + 5, col.lcd_bg );
    gfx_frame( x + 4 * CELL_W, top - 2, w - 5 * CELL_W, bottom - top + 5, FR_SUNKEN );
    gfx_hline( x + 4 * CELL_W + 2, mid, w - 5 * CELL_W - 4, col.zero );
    gfx_text( x + 4, top - 2, "+12", 3, 0, -1 );
    gfx_text( x + 4, mid - 8, "  0", 3, 0, -1 );
    gfx_text( x + 4, bottom - 14, "-12", 3, 0, -1 );
    /* the curve, from 31 Hz to 16 kHz across the band sliders */
    prev_y = mid;
    for ( k = 0; k <= 200; k++ ) {
        sx = eq_slider_col( 1 ) * CELL_W + CELL_W
             + k * (eq_slider_col( EQ_BANDS ) - eq_slider_col( 1 )) * CELL_W / 200;
        /* 31.25 Hz * 2^(9k/200) */
        hz = (u32)31 << ((k * 9) / 200);
        hz += hz * (u32)((k * 9) % 200) / 200;
        gain = eq_gain_at( &set.eq, hz );
        /* the gain back to decibels: 20 log10 by steps of a quarter of
           a decibel, which is finer than a pixel of the display */
        db256 = 0;
        if ( gain > 65536UL ) {
            while ( gain > 65536UL && db256 < 12 * 256 ) {
                gain = (u32)fx_umuldiv( gain, 63676UL, 65536UL );      /* -0.25 dB */
                db256 += 64;
            }
        } else {
            while ( gain < 65536UL && db256 > -12 * 256 ) {
                gain = (u32)fx_umuldiv( gain, 67450UL, 65536UL );      /* +0.25 dB */
                db256 -= 64;
            }
        }
        py = mid - (int)(db256 * (mid - top) / (12 * 256));
        if ( k > 0 ) {
            gfx_vline( sx, py < prev_y ? py : prev_y, (py < prev_y ? prev_y - py : py - prev_y) + 1, col.curve );
            gfx_vline( sx + 1, py < prev_y ? py : prev_y, (py < prev_y ? prev_y - py : py - prev_y) + 1, col.curve );
        }
        prev_y = py;
    }
    /* the sliders */
    for ( index = 0; index <= EQ_BANDS; index++ ) {
        sx = eq_slider_col( index ) * CELL_W + CELL_W - 2;
        gfx_rect( sx, top, 4, bottom - top, col.groove );
        sy = mid - eq_value( index ) * (mid - top) / (EQ_MAXDB * 2);
        gfx_rect( sx - knob_w / 2 + 2, sy - 4, knob_w, 9, pal.px_face );
        gfx_frame( sx - knob_w / 2 + 2, sy - 4, knob_w, 9,
                   focus == FO_EQ && eq_pick == index ? FR_PRESSED : FR_BUTTON );
        gfx_hline( sx - knob_w / 2 + 5, sy, knob_w - 6, set.eq.on ? col.lit[index == 0 ? 1 : 0] : 8 );
    }
}

static void draw_eq_cells( void )
{
    int index, row, c, rows = eq_track_rows(), top = eq_track_top(), at, zero, len;
    char text[48];
    u8 attr, frame_attr = focus == FO_EQ ? pal.menu_sel : pal.dlg;

    for ( row = plan.low_top; row < plan.low_top + plan.low_rows; row++ ) {
        scr_fill( row, plan.eq_left, plan.eq_cols, ' ', gfx_on ? pal.dlg : pal.desk );
    }
    sfmt( text, sizeof( text ), " Equalizer %s  %s ", set.eq.on ? "On" : "Off",
          set.preset >= 0 && set.preset < eq_preset_count ? eq_presets[set.preset].name : "Custom" );
    scr_putw( plan.low_top, plan.eq_left, text, plan.eq_cols, frame_attr );
    /* the labels under the sliders */
    for ( index = 0; index <= EQ_BANDS; index++ ) {
        c = eq_slider_col( index );
        len = (int)str_len( eq_label[index] );
        scr_put( plan.low_top + plan.low_rows - 1, c + 1 - (len - 1) / 2, eq_label[index],
                 focus == FO_EQ && eq_pick == index ? pal.menu_sel : (gfx_on ? pal.dlg : pal.desk) );
    }
    if ( gfx_on ) {
        return;                             /* the rest is the pane's */
    }
    /* the sliders in characters */
    scr_put( top, plan.eq_left + 1, "+12", pal.desk );
    scr_put( row_of_db( 0, top, rows ), plan.eq_left + 1, "  0", pal.desk );
    scr_put( top + rows - 1, plan.eq_left + 1, "-12", pal.desk );
    for ( index = 0; index <= EQ_BANDS; index++ ) {
        c = eq_slider_col( index ) + 1;
        at = row_of_db( eq_value( index ), top, rows );
        zero = row_of_db( 0, top, rows );
        for ( row = top; row < top + rows; row++ ) {
            attr = ATTR( 8, 0 );
            scr_ch( row, c, row == zero ? 0xC5 : 0xB3, attr );
        }
        attr = set.eq.on ? (focus == FO_EQ && eq_pick == index ? ATTR( 15, 1 ) : ATTR( 14, 0 )) : ATTR( 7, 0 );
        scr_ch( at, c, 0xDB, attr );
    }
}

/* ------------------------------------------------------------------ */
/* the playlist                                                        */
/* ------------------------------------------------------------------ */
static int list_rows( void )
{
    return plan.low_rows - 1;
}

void view_list_show( int index )
{
    int rows = list_rows();

    if ( rows < 1 ) {
        return;
    }
    if ( index < plist_top ) {
        plist_top = index;
    }
    if ( index >= plist_top + rows ) {
        plist_top = index - rows + 1;
    }
    if ( plist_top > plist_count - rows ) {
        plist_top = plist_count - rows;
    }
    if ( plist_top < 0 ) {
        plist_top = 0;
    }
}

static void draw_list( void )
{
    int row, index, rows = list_rows(), left = plan.pl_left, width = plan.pl_cols - 1, len;
    char text[200], time_buf[16];
    u8 attr;

    sfmt( text, sizeof( text ), " Playlist  %u file%s ", (u32)plist_count, plist_count == 1 ? "" : "s" );
    scr_putw( plan.low_top, left, text, plan.pl_cols, focus == FO_LIST ? pal.menu_sel : pal.dlg );
    for ( row = 0; row < rows; row++ ) {
        index = plist_top + row;
        attr = pal.list;
        if ( index >= plist_count ) {
            scr_fill( plan.low_top + 1 + row, left, width, ' ', attr );
            continue;
        }
        if ( index == plist_sel && focus == FO_LIST ) {
            attr = pal.select;
        } else if ( index == plist_now ) {
            attr = pal.menu_hot;
        }
        time_buf[0] = 0;
        if ( plist[index].length_ms ) {
            time_text( time_buf, sizeof( time_buf ), plist[index].length_ms );
        }
        sfmt( text, sizeof( text ), "%c%3u. %s", index == plist_now ? 0x10 : ' ', (u32)index + 1,
              plist[index].failed ? "(will not play) " : "" );
        len = (int)str_len( text );
        /* as much of the name as fits (the row cuts it shorter anyway) */
        str_cpyt( text + len, plist_name( index ), sizeof( text ) - (u32)len );
        if ( time_buf[0] && width > len + 10 ) {
            /* the name cut short of the time, with a blank between */
            scr_putw( plan.low_top + 1 + row, left, text, width - (int)str_len( time_buf ) - 2, attr );
            scr_putw( plan.low_top + 1 + row, left + width - (int)str_len( time_buf ) - 2, " ", 1, attr );
            scr_put( plan.low_top + 1 + row, left + width - (int)str_len( time_buf ) - 1, time_buf, attr );
        } else {
            scr_putw( plan.low_top + 1 + row, left, text, width, attr );
        }
    }
    if ( rows > 0 ) {
        sbar_draw( plan.low_top + 1, plan.low_top + rows, left + width, plist_top, rows, plist_count );
    }
}

/* ------------------------------------------------------------------ */
/* the information rows and the status bar                             */
/* ------------------------------------------------------------------ */
static void draw_info( void )
{
    PLSTAT st;
    char text[200], pos[16], len[16];
    int left = gfx_on ? LCD_COLS + 1 : 1, row, width;
    u8 attr = gfx_on ? pal.dlg : pal.desk;
    const char *name = plist_now >= 0 ? plist_name( plist_now ) : "";

    pl_status( &st );
    for ( row = 0; row < plan.info_rows; row++ ) {
        scr_fill( plan.info_top + row, 0, scr_cols, ' ', attr );
    }
    width = scr_cols - left - 1;
    if ( now_error[0] ) {
        scr_putw( plan.info_top, left, now_error, width, ATTR( 12, gfx_on ? 7 : 0 ) );
    } else if ( now_open ) {
        if ( now_info.title[0] ) {
            sfmt( text, sizeof( text ), "%s%s%s", now_info.title, now_info.artist[0] ? " - " : "",
                  now_info.artist );
        } else {
            sfmt( text, sizeof( text ), "%s", name );
        }
        scr_putw( plan.info_top, left, text, width, gfx_on ? ATTR( 1, 7 ) : ATTR( 15, 0 ) );
        sfmt( text, sizeof( text ), "%s, ", now_info.format );
        if ( now_info.kbps ) {
            sfmt( text + str_len( text ), sizeof( text ) - str_len( text ), "%u kbps, ", now_info.kbps );
        }
        sfmt( text + str_len( text ), sizeof( text ) - str_len( text ), "%u Hz %s",
              now_info.file_rate, now_info.file_channels == 1 ? "mono" : now_info.file_channels == 2 ? "stereo"
              : "multichannel" );
        scr_putw( plan.info_top + 1, left, text, width, attr );
        if ( now_info.album[0] ) {
            sfmt( text, sizeof( text ), "%s", now_info.album );
        } else {
            text[0] = 0;
        }
        if ( !gfx_on ) {
            time_text( pos, sizeof( pos ), st.pos_ms );
            time_text( len, sizeof( len ), st.len_ms );
            sfmt( text, sizeof( text ), "%s %s / %s   %s", st.state == PS_PLAYING ? "\x10" :
                  st.state == PS_PAUSED ? "\xBA" : "\xFE", pos, st.len_ms ? len : "-:--",
                  now_info.album );
        }
        scr_putw( plan.info_top + 2, left, text, width, gfx_on ? attr : ATTR( 10, 0 ) );
    } else {
        scr_putw( plan.info_top, left, plist_count ? "Choose a file in the playlist and press Enter."
                  : "Press Ctrl+O to open a file, or Ins to add files to the playlist.", width, attr );
    }
}

static void draw_status( void )
{
    PLSTAT st;
    const PLCARD *card = pl_card();
    char text[200];

    pl_status( &st );
    if ( status_text != NULL ) {
        scr_putw( plan.status_row, 0, status_text, scr_cols, pal.status );
        return;
    }
    if ( !card->present ) {
        sfmt( text, sizeof( text ), " No sound card driver is loaded" );
    } else if ( st.state == PS_PLAYING || st.state == PS_PAUSED ) {
        sfmt( text, sizeof( text ), " %s | %s | processor %d%%", card->name, st.quality, st.load );
        if ( st.underruns ) {
            sfmt( text + str_len( text ), sizeof( text ) - str_len( text ), " | dropouts: %u", st.underruns );
        }
    } else {
        sfmt( text, sizeof( text ), " %s", card->name );
    }
    if ( key_testing() && card->present ) {
        /* for the test log: what the driver and the kernel say */
        u32 regs[6];

        mem_set( regs, 0, sizeof( regs ) );
        regs[3] = card->irq;
        if ( card->irq == 0xFF || !sys_bus( 0x20, regs ) ) {
            regs[0] = regs[1] = 0xFFFFFFFFUL;
        }
        sfmt( text, sizeof( text ), " T %u ms, card irqs %u, driver state %u, dropouts %u, line flags %X taken %u",
              st.pos_ms, st.irqs, (u32)st.drv_state, st.underruns, regs[0], regs[1] );
    }
    scr_putw( plan.status_row, 0, text, scr_cols, pal.status );
    if ( set.eq.on && scr_cols > 40 ) {
        scr_put( plan.status_row, scr_cols - 4, " EQ", pal.status_hot );
    }
}

/* ------------------------------------------------------------------ */
/* the whole screen                                                    */
/* ------------------------------------------------------------------ */
/* a pane whose state is not what it last showed is painted again: its
   state, folded into a number */
static void repaint_changed( void )
{
    PLSTAT st;
    u32 sig;
    int index;

    sig = (u32)set.eq.on | ((u32)focus << 1) | ((u32)eq_pick << 2) | ((u32)(u8)set.eq.preamp << 8);
    for ( index = 0; index < EQ_BANDS; index++ ) {
        sig = sig * 31 + (u8)set.eq.band[index];
    }
    if ( sig != sig_eq ) {
        sig_eq = sig;
        pane_damage_all( PN_EQ );
    }
    pl_status( &st );
    sig = (u32)set.volume | ((u32)set.repeat << 8) | ((u32)set.shuffle << 10) | ((u32)st.state << 12);
    if ( sig != sig_tr ) {
        sig_tr = sig;
        pane_damage_all( PN_TRANS );
    }
    sig = (st.pos_ms / 1000) | ((u32)st.state << 28);
    if ( sig != sig_lcd ) {
        sig_lcd = sig;
        pane_damage_all( PN_LCD );
        pane_damage_all( PN_SEEK );
    }
}

void view_draw( void )
{
    int row;

    view_layout();
    if ( plan.title_row >= 0 ) {
        char title[120];
        sfmt( title, sizeof( title ), "MPLAY%s%s", plist_now >= 0 ? " - " : "",
              plist_now >= 0 ? plist_name( plist_now ) : "" );
        scr_fill( plan.title_row, 0, scr_cols, ' ', pal.title );
        scr_put( plan.title_row, (scr_cols - (int)str_len( title )) / 2 > 0 ? (scr_cols - (int)str_len( title )) / 2 : 0,
                 title, pal.title );
    }
    draw_info();
    if ( gfx_on ) {
        pane_place( PN_LCD, plan.info_top, 0, plan.info_rows, LCD_COLS );
    }
    if ( plan.an_rows > 0 ) {
        if ( !pane_place( PN_AN, plan.an_top, 0, plan.an_rows, scr_cols ) ) {
            draw_an_text();
        }
    }
    if ( !pane_place( PN_SEEK, plan.seek_row, 0, 1, scr_cols ) ) {
        draw_seek_text();
    }
    if ( !pane_place( PN_TRANS, plan.tr_top, 0, plan.tr_rows, scr_cols ) ) {
        draw_trans_text();
    }
    if ( plan.eq_cols > 0 ) {
        draw_eq_cells();
        if ( gfx_on ) {
            pane_place( PN_EQ, plan.low_top + 1, plan.eq_left, plan.low_rows - 2, plan.eq_cols );
        }
    }
    if ( plan.pl_cols > 0 ) {
        draw_list();
    }
    if ( plan.low_rows > 0 && plan.eq_cols == 0 && plan.pl_cols == 0 ) {
        for ( row = plan.low_top; row < plan.low_top + plan.low_rows; row++ ) {
            scr_fill( row, 0, scr_cols, ' ', pal.desk );
        }
    }
    draw_status();
    scr_cursor( 0, 0, CUR_HIDE );
    caret_off();
    if ( gfx_on ) {
        repaint_changed();
    }
}

/* the moving parts: new levels, falling bars and peaks, and on a
   graphics screen the panes that changed told so */
void view_animate( void )
{
    u8 fresh[BARS_MAX];
    int bar, changed = 0, sec_now, seek_now, state_now;
    PLSTAT st;

    if ( set.analyzer == AN_SCOPE ) {
        scope_count = gfx_on ? (scr_cols * CELL_W < 640 ? scr_cols * CELL_W : 640) : scr_cols;
        pl_wave( scope, scope_count );
        changed = 1;
    } else if ( set.analyzer != AN_OFF ) {
        pl_levels( fresh, bar_count );
        for ( bar = 0; bar < bar_count; bar++ ) {
            if ( fresh[bar] >= bar_now[bar] ) {
                if ( fresh[bar] != bar_now[bar] ) {
                    changed = 1;
                }
                bar_now[bar] = fresh[bar];
            } else {
                bar_now[bar] = (u8)(bar_now[bar] > 20 ? bar_now[bar] - 20 : 0);
                changed = 1;
            }
            if ( bar_now[bar] >= bar_peak[bar] ) {
                bar_peak[bar] = bar_now[bar];
                bar_hold[bar] = 8;
            } else if ( bar_hold[bar] > 0 ) {
                bar_hold[bar]--;
            } else if ( bar_peak[bar] > 0 ) {
                bar_peak[bar] = (u8)(bar_peak[bar] > 8 ? bar_peak[bar] - 8 : 0);
                changed = 1;
            }
        }
    }
    if ( gfx_on ) {
        if ( changed ) {
            pane_damage_all( PN_AN );
        }
        pl_status( &st );
        sec_now = (int)(st.pos_ms / 1000);
        state_now = st.state;
        if ( (u32)sec_now != shown_sec || state_now != shown_state ) {
            shown_sec = (u32)sec_now;
            shown_state = state_now;
            pane_damage_all( PN_LCD );
            pane_damage_all( PN_TRANS );
        }
        seek_now = seek_at( scr_cols * CELL_W - 10 );
        if ( seek_now != shown_seek ) {
            shown_seek = seek_now;
            pane_damage_all( PN_SEEK );
        }
    }
}

/* ------------------------------------------------------------------ */
/* the mouse                                                           */
/* ------------------------------------------------------------------ */
/* a slider held: the band follows the pointer until the button is let go */
static void eq_drag( int index )
{
    int key, value, top = eq_track_top(), rows = eq_track_rows();
    int ytop, ybot;

    ytop = top * CELL_H + 4;
    ybot = (top + rows) * CELL_H - 4;
    for ( ;; ) {
        if ( gfx_on ) {
            value = (ybot + ytop) / 2 - mouse.y;
            value = value * EQ_MAXDB * 2 / ((ybot - ytop) / 2 > 0 ? (ybot - ytop) / 2 : 1);
        } else {
            value = db_of_row( mouse.row, top, rows );
        }
        if ( value > EQ_MAXDB * 2 ) {
            value = EQ_MAXDB * 2;
        }
        if ( value < -EQ_MAXDB * 2 ) {
            value = -EQ_MAXDB * 2;
        }
        value &= ~1;                    /* whole decibels from the mouse */
        if ( index == 0 ) {
            set.eq.preamp = (s8)value;
        } else {
            set.eq.band[index - 1] = (s8)value;
        }
        set.preset = -1;
        eq_apply();
        if ( gfx_on ) {
            pane_damage_all( PN_EQ );
        }
        ui_refresh();
        scr_flush();
        if ( !(mouse_buttons() & MBTN_LEFT) ) {
            break;
        }
        key = key_get();
        if ( key != K_MOUSE || mouse.kind == ME_UP ) {
            break;
        }
    }
}

static void seek_drag( void )
{
    PLSTAT st;
    int key, width = scr_cols * CELL_W - 10, x;

    pl_status( &st );
    if ( st.len_ms == 0 ) {
        return;
    }
    for ( ;; ) {
        x = mouse.x - 5;
        if ( x < 0 ) {
            x = 0;
        }
        if ( x > width ) {
            x = width;
        }
        if ( mouse.kind == ME_UP || !(mouse_buttons() & MBTN_LEFT) ) {
            seek_to( fx_umuldiv( (u32)x, st.len_ms, (u32)width ) );
            break;
        }
        key = key_get();
        if ( key != K_MOUSE ) {
            break;
        }
    }
}

static void vol_drag( void )
{
    int key, x0 = (vol_col() + (gfx_on ? 7 : 4)) * CELL_W, width;

    width = gfx_on ? scr_cols * CELL_W - x0 - 2 * CELL_W - 10 : (scr_cols - vol_col() - 10) * CELL_W;
    if ( width < 8 ) {
        return;
    }
    for ( ;; ) {
        set_volume( (mouse.x - x0) * 100 / width );
        if ( gfx_on ) {
            pane_damage_all( PN_TRANS );
        }
        ui_refresh();
        scr_flush();
        if ( !(mouse_buttons() & MBTN_LEFT) ) {
            break;
        }
        key = key_get();
        if ( key != K_MOUSE || mouse.kind == ME_UP ) {
            break;
        }
    }
}

int view_mouse( void )
{
    int index, row = mouse.row, c = mouse.col, rows, hit;

    if ( mouse.kind == ME_WHEEL ) {
        if ( row >= plan.tr_top && row < plan.tr_top + plan.tr_rows ) {
            set_volume( set.volume - mouse.wheel * 5 );
        } else if ( plan.pl_cols && c >= plan.pl_left ) {
            plist_top += mouse.wheel * 3;
            if ( plist_top > plist_count - list_rows() ) {
                plist_top = plist_count - list_rows();
            }
            if ( plist_top < 0 ) {
                plist_top = 0;
            }
        }
        return 0;
    }
    if ( mouse.kind != ME_DOWN && mouse.kind != ME_DOUBLE ) {
        return 0;
    }
    if ( mouse.button != MBTN_LEFT ) {
        return 0;
    }
    /* the buttons and the volume */
    if ( row >= plan.tr_top && row < plan.tr_top + plan.tr_rows ) {
        for ( index = 0; index < TR_COUNT; index++ ) {
            if ( c >= 2 + index * TR_WIDTH && c < 2 + index * TR_WIDTH + TR_WIDTH - 1 ) {
                return tr_cmds[index];
            }
        }
        if ( c >= vol_col() + (gfx_on ? 7 : 4) ) {
            vol_drag();
        }
        return 0;
    }
    if ( row == plan.seek_row ) {
        seek_drag();
        return 0;
    }
    if ( row >= plan.low_top && row < plan.low_top + plan.low_rows ) {
        if ( plan.eq_cols && c >= plan.eq_left && c < plan.eq_left + plan.eq_cols ) {
            focus = FO_EQ;
            if ( row == plan.low_top ) {
                return CM_EQ_ON;            /* the title: switch it on or off */
            }
            for ( index = 0; index <= EQ_BANDS; index++ ) {
                if ( c >= eq_slider_col( index ) && c <= eq_slider_col( index ) + 2 ) {
                    eq_pick = index;
                    eq_drag( index );
                    return 0;
                }
            }
            return 0;
        }
        if ( plan.pl_cols && c >= plan.pl_left ) {
            focus = FO_LIST;
            rows = list_rows();
            if ( row == plan.low_top ) {
                return 0;
            }
            if ( c == plan.pl_left + plan.pl_cols - 1 ) {
                hit = sbar_hit( plan.low_top + 1, plan.low_top + rows, plist_top, rows, plist_count, row );
                switch ( hit ) {
                case SB_UP:
                    plist_top--;
                    break;
                case SB_DOWN:
                    plist_top++;
                    break;
                case SB_PGUP:
                    plist_top -= rows;
                    break;
                case SB_PGDN:
                    plist_top += rows;
                    break;
                case SB_THUMB:
                    plist_top = sbar_drag( plan.low_top + 1, plan.low_top + rows, rows, plist_count, row );
                    break;
                }
                if ( plist_top > plist_count - rows ) {
                    plist_top = plist_count - rows;
                }
                if ( plist_top < 0 ) {
                    plist_top = 0;
                }
                return 0;
            }
            index = plist_top + row - plan.low_top - 1;
            if ( index >= 0 && index < plist_count ) {
                plist_sel = index;
                if ( mouse.kind == ME_DOUBLE ) {
                    return CM_PLAYSEL;
                }
            }
            return 0;
        }
    }
    return 0;
}
