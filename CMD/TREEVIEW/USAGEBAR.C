/*
 * USAGEBAR.C - the left pane: DiskStat's USAGEBAR.CPP.  The disk as a
 * bar of three colours - blue used, yellow the folder the cursor is on,
 * green free - over a legend, and on the graphics screen the Windows
 * DiskStat's 3-D pie in the bar's place.  It only draws: FOLDTREE.C
 * asks it to whenever the cursor moves.
 *
 * THE PIE IS WORKED OUT IN WHOLE NUMBERS.  The DOS program did it in
 * floating point, which a native program here cannot count on: the
 * kernel has no coprocessor emulator, and a 386 without a 387 would
 * stop at the first instruction.  The method is the DOS program's,
 * step for step - see "the pie" below - with the angles in 65,536ths of
 * a turn, sine and cosine from a table of a quarter turn in 16,384ths,
 * and every comparison against a slice's edge made by cross
 * multiplication, so no division is rounded where the DOS program
 * compared exactly.
 */
#include "treeview.h"

#define BAR_GLYPH       '\xDB'

/* the three parts, in the legend's colours */
#define COL_USED        FG_LIGHT_BLUE
#define COL_SEL         FG_YELLOW
#define COL_FREE        FG_LIGHT_GREEN

static RECT region;                     /* from 0, the frame included */

void usage_region( int row, int col, int end_row, int end_col )
{
    region.row = row;
    region.col = col;
    region.end_row = end_row;
    region.end_col = end_col;
}

/* ------------------------------------------------------------------ */
/* the pie - graphics screen only                                      */
/*                                                                     */
/* The Windows DiskStat's pie (DRAW.CPP) in the sixteen EGA colours: a */
/* top face squashed to half its width for the tilt, a side wall under */
/* its front half in a darker shade of each slice, black round the top */
/* face and along the radii between slices, dark grey along the side   */
/* wall's edges.  Slices run anticlockwise from three o'clock in the   */
/* order Used, the selected folder, Free, and the selected folder      */
/* never gets thinner than 1.5% of the pie, so a small folder still    */
/* shows.                                                              */
/*                                                                     */
/* It is drawn a ROW OF PIXELS AT A TIME, because it is drawn again on */
/* every move of the cursor.  Along a row of the top face the angle    */
/* only falls (above the centre) or rises (below it), so each row is   */
/* at most three runs of colour, and where a run ends is one division  */
/* per slice edge.                                                     */
/* ------------------------------------------------------------------ */

#define PIE_ROWS        9               /* cells tall: 144 pixels on the 8x16 screen */
#define PIE_DEPTH       14              /* pixels of side wall */
#define PIE_MAX_RY      120             /* bound for the half-width table */
#define PIE_TURN        65536L          /* the angles: a turn */
#define PIE_MIN_SEL     983L            /* 1.5% of a turn */
#define PIE_ONE         16384L          /* sine and cosine: 1 */

#define PIE_SIDE_USED   1               /* blue       - under COL_USED */
#define PIE_SIDE_SEL    6               /* brown      - under COL_SEL */
#define PIE_SIDE_FREE   2               /* green      - under COL_FREE */
#define PIE_INK         0               /* black      - the top face's outline */
#define PIE_EDGE        8               /* dark grey  - the side wall's edges */

/* sin( i/256 of a quarter turn ) * 16384 */
static const short quarter_sine[257] = {
    0, 101, 201, 302, 402, 503, 603, 704, 804, 904, 1005, 1105, 1205, 1306,
    1406, 1506, 1606, 1706, 1806, 1906, 2006, 2105, 2205, 2305, 2404, 2503,
    2603, 2702, 2801, 2900, 2999, 3098, 3196, 3295, 3393, 3492, 3590, 3688,
    3786, 3883, 3981, 4078, 4176, 4273, 4370, 4467, 4563, 4660, 4756, 4852,
    4948, 5044, 5139, 5235, 5330, 5425, 5520, 5614, 5708, 5803, 5897, 5990,
    6084, 6177, 6270, 6363, 6455, 6547, 6639, 6731, 6823, 6914, 7005, 7096,
    7186, 7276, 7366, 7456, 7545, 7635, 7723, 7812, 7900, 7988, 8076, 8163,
    8250, 8337, 8423, 8509, 8595, 8680, 8765, 8850, 8935, 9019, 9102, 9186,
    9269, 9352, 9434, 9516, 9598, 9679, 9760, 9841, 9921, 10001, 10080,
    10159, 10238, 10316, 10394, 10471, 10549, 10625, 10702, 10778, 10853,
    10928, 11003, 11077, 11151, 11224, 11297, 11370, 11442, 11514, 11585,
    11656, 11727, 11797, 11866, 11935, 12004, 12072, 12140, 12207, 12274,
    12340, 12406, 12472, 12537, 12601, 12665, 12729, 12792, 12854, 12916,
    12978, 13039, 13100, 13160, 13219, 13279, 13337, 13395, 13453, 13510,
    13567, 13623, 13678, 13733, 13788, 13842, 13896, 13949, 14001, 14053,
    14104, 14155, 14206, 14256, 14305, 14354, 14402, 14449, 14497, 14543,
    14589, 14635, 14680, 14724, 14768, 14811, 14854, 14896, 14937, 14978,
    15019, 15059, 15098, 15137, 15175, 15213, 15250, 15286, 15322, 15357,
    15392, 15426, 15460, 15493, 15525, 15557, 15588, 15619, 15649, 15679,
    15707, 15736, 15763, 15791, 15817, 15843, 15868, 15893, 15917, 15941,
    15964, 15986, 16008, 16029, 16049, 16069, 16088, 16107, 16125, 16143,
    16160, 16176, 16192, 16207, 16221, 16235, 16248, 16261, 16273, 16284,
    16295, 16305, 16315, 16324, 16332, 16340, 16347, 16353, 16359, 16364,
    16369, 16373, 16376, 16379, 16381, 16383, 16384, 16384
};

/* the table between its entries, a part in 64 */
static long sine_part( long within )    /* 0..16384 of a quarter turn */
{
    long idx = within >> 6, frac = within & 63, lo = quarter_sine[idx];

    if ( frac == 0 ) {
        return lo;
    }
    return lo + ((quarter_sine[idx + 1] - lo) * frac) / 64;
}

static long pie_sin( long angle )
{
    long within;

    angle &= PIE_TURN - 1;
    within = angle & 16383;
    switch ( angle >> 14 ) {
    case 0:  return sine_part( within );
    case 1:  return sine_part( 16384 - within );
    case 2:  return -sine_part( within );
    default: return -sine_part( 16384 - within );
    }
}

static long pie_cos( long angle )
{
    return pie_sin( angle + PIE_TURN / 4 );
}

/* a / b rounded towards minus infinity and plus infinity, b not 0 */
static long floor_div( long a, long b )
{
    long q = a / b;

    if ( (a % b != 0) && ((a < 0) != (b < 0)) ) {
        q--;
    }
    return q;
}

static long ceil_div( long a, long b )
{
    long q = a / b;

    if ( (a % b != 0) && ((a < 0) == (b < 0)) ) {
        q++;
    }
    return q;
}

static u32 isqrt( u32 val )
{
    u32 root = 0, bit = 1UL << 30;

    while ( bit > val ) {
        bit >>= 2;
    }
    while ( bit ) {
        if ( val >= root + bit ) {
            val -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

typedef struct {
    int  top, side;             /* EGA colours */
    long start;                 /* 65536ths of a turn anticlockwise from 3 o'clock */
} PIESLICE;

/* where slice k+1 begins: the edge x on a row dy above the centre is
   cx + dy * num_per_row / den, and on the front rim cx + rimnum / 16384 */
typedef struct {
    int  half;                  /* +1 above the centre, -1 below, 0 at a half turn */
    long num_per_row;           /* rx * cos */
    long den;                   /* ry * sin */
    long rimnum;                /* rx * cos, for the rim */
} PIEBOUND;

typedef struct {
    int cx, cy, rx, ry, depth;
    int hw[PIE_MAX_RY + 1];     /* the top face's half-width, by rows off the centre */
} PIEGEOM;

/* x - cx  <=  the edge, on a row dy_up above the centre; the same test
   on either side of the centre once both sides are multiplied by den */
static int left_of_edge( const PIEBOUND *bd, int dx, int dy_up )
{
    return (long)dx * bd->den <= (long)dy_up * bd->num_per_row;
}

/* the top face on picture row y */
static int top_span( const PIEGEOM *g, int y, int *l, int *r )
{
    int dy = y - g->cy;

    if ( dy < -g->ry || dy > g->ry ) {
        return 0;
    }
    *l = g->cx - g->hw[dy < 0 ? -dy : dy];
    *r = g->cx + g->hw[dy < 0 ? -dy : dy];
    return 1;
}

/* the whole solid - top face and side wall - on picture row y: the top
   half of the top face, full width down through the depth, then the
   bottom half of the base */
static int solid_span( const PIEGEOM *g, int y, int *l, int *r )
{
    int dy = y - g->cy, w;

    if ( dy < -g->ry || dy > g->ry + g->depth ) {
        return 0;
    }
    if ( dy <= 0 ) {
        w = g->hw[-dy];
    } else if ( dy <= g->depth ) {
        w = g->hw[0];
    } else {
        w = g->hw[dy - g->depth];
    }
    *l = g->cx - w;
    *r = g->cx + w;
    return 1;
}

static int shape_span( const PIEGEOM *g, int solid, int y, int *l, int *r )
{
    return solid ? solid_span( g, y, l, r ) : top_span( g, y, l, r );
}

static void pie_span( int y, int x0, int x1, int color )
{
    if ( x0 <= x1 ) {
        df_picture_span( y, x0, x1, color );
    }
}

/* a pixel column, kept inside what the runs can use: an edge a hair off
   the horizontal is a very long way off on a row near the centre */
static int clamp_x( long x )
{
    if ( x < -30000L ) {
        return -30000;
    }
    if ( x > 30000L ) {
        return 30000;
    }
    return (int)x;
}

/* Which slice a point of the top face is in: the number of slice edges
   at or below its angle.  Above the centre the angle falls from left to
   right, below it rises, and on the centre row it is a half turn to the
   left of the centre and 0 from it rightwards. */
static int top_slice_at( const PIEGEOM *g, const PIEBOUND *bd, int nb, int x, int y )
{
    int dy_up = g->cy - y, k, n = 0;

    for ( k = 0; k < nb; k++ ) {
        if ( dy_up > 0 ) {
            if ( bd[k].half > 0 && left_of_edge( &bd[k], x - g->cx, dy_up ) ) {
                n++;
            }
        } else if ( dy_up < 0 ) {
            if ( bd[k].half >= 0 || left_of_edge( &bd[k], x - g->cx, dy_up ) ) {
                n++;
            }
        } else if ( x < g->cx && bd[k].half >= 0 ) {
            n++;
        }
    }
    return n;
}

/* Which slice a column of the side wall is in: the front rim runs from
   a half turn at the left to a whole one at the right. */
static int side_slice_at( const PIEGEOM *g, const PIEBOUND *bd, int nb, int x )
{
    int k, n = 0;

    for ( k = 0; k < nb; k++ ) {
        if ( bd[k].half >= 0 || (long)(x - g->cx) * PIE_ONE >= bd[k].rimnum ) {
            n++;
        }
    }
    return n;
}

/* paint [l, r] of picture row y, split where the slice changes */
static void pie_run( const PIEGEOM *g, const PIESLICE *sl, const PIEBOUND *bd,
                     int nb, int y, int l, int r, int side )
{
    int cut[4], n = 0, k, i, j, x, end, s, hold;
    int dy_up = g->cy - y;

    if ( l > r ) {
        return;
    }
    for ( k = 0; k < nb; k++ ) {
        if ( side ) {
            if ( bd[k].half < 0 ) {
                cut[n++] = clamp_x( g->cx + ceil_div( bd[k].rimnum, PIE_ONE ) );
            }
        } else if ( dy_up > 0 && bd[k].half > 0 ) {
            cut[n++] = clamp_x( g->cx + floor_div( (long)dy_up * bd[k].num_per_row, bd[k].den ) + 1 );
        } else if ( dy_up < 0 && bd[k].half < 0 ) {
            cut[n++] = clamp_x( g->cx + ceil_div( (long)dy_up * bd[k].num_per_row, bd[k].den ) );
        }
    }
    if ( !side && dy_up == 0 ) {
        cut[n++] = g->cx;
    }
    for ( i = 1; i < n; i++ ) {         /* three at most: insertion sort */
        for ( j = i; j > 0 && cut[j - 1] > cut[j]; j-- ) {
            hold = cut[j];
            cut[j] = cut[j - 1];
            cut[j - 1] = hold;
        }
    }
    x = l;
    for ( i = 0; i <= n && x <= r; i++ ) {
        end = i < n ? cut[i] - 1 : r;
        if ( end > r ) {
            end = r;
        }
        if ( end < x ) {
            continue;
        }
        s = side ? side_slice_at( g, bd, nb, x ) : top_slice_at( g, bd, nb, x, y );
        pie_span( y, x, end, side ? sl[s].side : sl[s].top );
        x = end + 1;
    }
}

/* the outline of the top face or of the whole solid on row y: the
   pixels of the row whose neighbour above, below, left or right is
   outside the shape */
static void pie_edge_row( const PIEGEOM *g, int solid, int y, int color )
{
    int l, r, il, ir, nl, nr;

    if ( !shape_span( g, solid, y, &l, &r ) ) {
        return;
    }
    il = l + 1;
    ir = r - 1;
    if ( shape_span( g, solid, y - 1, &nl, &nr ) ) {
        if ( nl > il ) {
            il = nl;
        }
        if ( nr < ir ) {
            ir = nr;
        }
    } else {
        ir = il - 1;
    }
    if ( shape_span( g, solid, y + 1, &nl, &nr ) ) {
        if ( nl > il ) {
            il = nl;
        }
        if ( nr < ir ) {
            ir = nr;
        }
    } else {
        ir = il - 1;
    }
    if ( il > ir ) {
        pie_span( y, l, r, color );
    } else {
        pie_span( y, l, il - 1, color );
        pie_span( y, ir + 1, r, color );
    }
}

static void pie_line( int x0, int y0, int x1, int y1, int color )
{
    int dx = abs( x1 - x0 ), sx = x0 < x1 ? 1 : -1;
    int dy = -abs( y1 - y0 ), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    for ( ;; ) {
        pie_span( y0, x0, x0, color );
        if ( x0 == x1 && y0 == y1 ) {
            break;
        }
        e2 = 2 * err;
        if ( e2 >= dy ) {
            err += dy;
            x0 += sx;
        }
        if ( e2 <= dx ) {
            err += dx;
            y0 += sy;
        }
    }
}

/* the pie into cells (top, left)-(bottom, right), 1-based; 0 if the
   screen gave no picture, and the caller draws the bar */
static int draw_pie( int top, int left, int bottom, int right,
                     u64 other_used, u64 sel_size, int have_sel )
{
    static PIEGEOM g;
    PIESLICE sl[3];
    PIEBOUND bd[2];
    int m = 0, nb, k, y, l, r, tl, tr, w, h, two_d;
    long a_other, a_sel, a_free, a, s, c;

    if ( total_disk_space == 0 ) {
        return 0;
    }
    w = (right - left + 1) * df_cell_w();
    h = (bottom - top + 1) * df_cell_h();

    g.depth = PIE_DEPTH;
    g.rx = w / 2 - 10;
    if ( g.rx > 2 * PIE_MAX_RY ) {
        g.rx = 2 * PIE_MAX_RY;
    }
    g.ry = g.rx / 2;                    /* the tilt: half as tall */
    if ( g.ry > (h - g.depth - 6) / 2 ) {
        g.ry = (h - g.depth - 6) / 2;
    }
    if ( g.ry > PIE_MAX_RY ) {
        g.ry = PIE_MAX_RY;
    }
    if ( g.rx < 16 || g.ry < 8 ) {
        return 0;
    }
    g.cx = w / 2;
    g.cy = (h - (2 * g.ry + g.depth)) / 2 + g.ry;
    /* Measured half a pixel further out than ry: the row at exactly ry
       would otherwise be one pixel wide, a spike on the top of the
       outline.  rx * sqrt( 1 - k^2 / (ry + 1/2)^2 ), to the nearest:
       with D = 2ry + 1 that is sqrt( rx^2 (D^2 - 4k^2) ) / D. */
    two_d = 2 * (2 * g.ry + 1);
    for ( k = 0; k <= g.ry; k++ ) {
        u32 d = (u32)(2 * g.ry + 1);
        u32 under = (u32)g.rx * (u32)g.rx * (d * d - 4UL * (u32)k * (u32)k);

        g.hw[k] = (int)((2 * isqrt( under ) + d) / (u32)two_d);
    }

    /* the angles, as the Windows pie works them out */
    a_free = PIE_TURN - (long)per_whole( used_disk_space, total_disk_space, (u32)PIE_TURN );
    a_sel = 0;
    if ( have_sel && sel_size > 0 ) {
        a_sel = (long)per_whole( sel_size, total_disk_space, (u32)PIE_TURN );
        if ( a_sel < PIE_MIN_SEL ) {
            a_sel = PIE_MIN_SEL;
        }
    }
    a_other = PIE_TURN - a_sel - a_free;
    if ( a_other < 0 ) {
        a_free += a_other;
        a_other = 0;
    }
    if ( a_free < 0 ) {
        a_free = 0;
    }

    /* An empty slice is left out, and whatever angle rounding gave it
       goes to its neighbour - the first slice always starts at three
       o'clock. */
    a = 0;
    if ( other_used > 0 && a_other > 0 ) {
        sl[m].top = COL_USED; sl[m].side = PIE_SIDE_USED; sl[m].start = a; m++;
    }
    a += a_other;
    if ( a_sel > 0 ) {
        sl[m].top = COL_SEL; sl[m].side = PIE_SIDE_SEL; sl[m].start = a; m++;
    }
    a += a_sel;
    if ( free_disk_space > 0 && a_free > 0 ) {
        sl[m].top = COL_FREE; sl[m].side = PIE_SIDE_FREE; sl[m].start = a; m++;
    }
    if ( m == 0 ) {
        sl[m].top = COL_USED; sl[m].side = PIE_SIDE_USED; sl[m].start = 0; m++;
    }
    sl[0].start = 0;

    nb = m - 1;
    for ( k = 0; k < nb; k++ ) {
        s = pie_sin( sl[k + 1].start );
        c = pie_cos( sl[k + 1].start );
        bd[k].half = s > 0 ? 1 : s < 0 ? -1 : 0;
        bd[k].num_per_row = (long)g.rx * c;
        bd[k].den = (long)g.ry * s;
        bd[k].rimnum = (long)g.rx * c;
    }

    if ( !df_picture_open( top, left, bottom, right, BG_WHITE ) ) {
        return 0;
    }

    /* 1. the side wall: the solid, less the top face */
    for ( y = 0; y < h; y++ ) {
        if ( !solid_span( &g, y, &l, &r ) ) {
            continue;
        }
        if ( top_span( &g, y, &tl, &tr ) ) {
            pie_run( &g, sl, bd, nb, y, l, tl - 1, 1 );
            pie_run( &g, sl, bd, nb, y, tr + 1, r, 1 );
        } else {
            pie_run( &g, sl, bd, nb, y, l, r, 1 );
        }
    }

    /* 2. its edges (the part that falls on the top face is painted over next) */
    for ( y = 0; y < h; y++ ) {
        pie_edge_row( &g, 1, y, PIE_EDGE );
    }

    /* 3. the top face, and 4. its outline */
    for ( y = 0; y < h; y++ ) {
        if ( top_span( &g, y, &tl, &tr ) ) {
            pie_run( &g, sl, bd, nb, y, tl, tr, 0 );
        }
    }
    for ( y = 0; y < h; y++ ) {
        pie_edge_row( &g, 0, y, PIE_INK );
    }

    /* 5. the radii where slices meet - including three o'clock, where the
       last meets the first - and 6. the same meetings down the side wall */
    if ( m > 1 ) {
        for ( k = 0; k < m; k++ ) {
            int ex, ey;

            s = pie_sin( sl[k].start );
            c = pie_cos( sl[k].start );
            ex = g.cx + (int)floor_div( 2L * g.rx * c + PIE_ONE, 2 * PIE_ONE );
            ey = g.cy - (int)floor_div( 2L * g.ry * s + PIE_ONE, 2 * PIE_ONE );
            pie_line( g.cx, g.cy, ex, ey, PIE_INK );
            if ( s < 0 && ex > g.cx - g.rx && ex < g.cx + g.rx ) {
                pie_line( ex, ey + 1, ex, ey + g.depth, PIE_EDGE );
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* the pane                                                            */
/* ------------------------------------------------------------------ */

/* "{swatch} Label  xx.x%  (1.2 MB)" on one row: the label is cut where
   it would push the line past the pane, which the DOS pane let a long
   folder name do */
static void legend_entry( int row, int col, int width, int swatch, const char *label, u64 size )
{
    char pct[16], sz[24], line[80], name[40], glyph[2];
    int room;

    glyph[0] = BAR_GLYPH;
    glyph[1] = 0;
    df_text( row, col, glyph, swatch, BG_WHITE );

    format_pct( size, total_disk_space, pct );
    format_size( size, sz );
    room = width - 1 - (1 + 1 + ((int)strlen( pct ) > 6 ? (int)strlen( pct ) : 6) + 3
                        + (int)strlen( sz ) + 1);
    if ( room > (int)sizeof( name ) - 1 ) {
        room = (int)sizeof( name ) - 1;
    }
    if ( room < 1 ) {
        room = 1;
    }
    shorten_name( name, room, label );
    /* DiskStat's " %-8s %6s  (%s)", the label padded only as far as
       there is room for, so a long size keeps its closing bracket */
    sprintf( line, " %-*s %6s  (%s)", room < 8 ? room : 8, name, pct, sz );
    if ( width - 1 >= 0 && width - 1 < (int)sizeof( line ) ) {
        line[width - 1] = 0;
    }
    df_text( row, col + 1, line, FG_BLACK, BG_WHITE );
}

void usage_draw( void )
{
    char head[32], run[134];
    int top1 = region.row + 1, left1 = region.col + 1;
    int bot1 = region.end_row + 1, right1 = region.end_col + 1;
    int in_left = left1 + 1, in_w = right1 - left1 - 1, have_sel, lrow;
    u64 sel_size, other_used;

    if ( region.end_row <= region.row || region.end_col <= region.col ) {
        return;
    }
    draw_box( top1, left1, bot1, right1, BORDER_SINGLE, FG_BLACK, BG_WHITE );
    df_fill( top1 + 1, left1 + 1, bot1 - 1, right1 - 1, BG_WHITE );
    if ( in_w < 4 ) {
        return;
    }

    sprintf( head, "Disk Usage  -  Drive %c:", 'A' + current_drive - 1 );
    df_text( top1 + 1, in_left, head, FG_BLACK, BG_WHITE );

    if ( !data_valid ) {
        /* there is always a scan coming: the heading names the drive */
        df_text( top1 + 3, in_left, "Scanning, please wait...", FG_BLACK, BG_WHITE );
        return;
    }
    if ( total_disk_space == 0 ) {
        df_text( top1 + 3, in_left, "No disk information.", FG_BLACK, BG_WHITE );
        return;
    }

    /* the used / selected / free split */
    have_sel = selected_node >= 0 && selected_node < tree_count;
    sel_size = have_sel ? tree_nodes[selected_node].total_size : 0;
    if ( sel_size > used_disk_space ) {
        sel_size = used_disk_space;
    }
    other_used = used_disk_space - sel_size;

    /* the pie, on a screen that can draw one and a pane that fits it:
       PIE_ROWS of pie two rows under the heading, a blank row, then the
       three legend rows */
    if ( df_has_pictures() && bot1 - top1 >= PIE_ROWS + 7
         && draw_pie( top1 + 3, in_left, top1 + 2 + PIE_ROWS, in_left + in_w - 1,
                      other_used, sel_size, have_sel ) ) {
        lrow = top1 + 4 + PIE_ROWS;
    } else {
        int bar_w = in_w, sel_cells, free_cells, other_cells, col = in_left;

        if ( bar_w > (int)sizeof( run ) - 1 ) {
            bar_w = (int)sizeof( run ) - 1;
        }
        /* to the nearest cell */
        sel_cells = (int)((per_whole( sel_size, total_disk_space, (u32)bar_w * 2 ) + 1) / 2);
        free_cells = (int)((per_whole( free_disk_space, total_disk_space, (u32)bar_w * 2 ) + 1) / 2);

        /* a sliver at least for a folder with anything in it, and what is
           left over to the rest of the used part */
        if ( have_sel && sel_size > 0 && sel_cells < 1 ) {
            sel_cells = 1;
        }
        if ( sel_cells + free_cells > bar_w ) {
            if ( free_cells > sel_cells ) {
                free_cells = bar_w - sel_cells;
            } else {
                sel_cells = bar_w - free_cells;
            }
        }
        other_cells = bar_w - sel_cells - free_cells;
        if ( other_cells < 0 ) {
            other_cells = 0;
        }
        if ( other_cells > 0 ) {
            memset( run, BAR_GLYPH, (size_t)other_cells );
            run[other_cells] = 0;
            df_text( top1 + 3, col, run, COL_USED, BG_WHITE );
            col += other_cells;
        }
        if ( sel_cells > 0 ) {
            memset( run, BAR_GLYPH, (size_t)sel_cells );
            run[sel_cells] = 0;
            df_text( top1 + 3, col, run, COL_SEL, BG_WHITE );
            col += sel_cells;
        }
        if ( free_cells > 0 ) {
            memset( run, BAR_GLYPH, (size_t)free_cells );
            run[free_cells] = 0;
            df_text( top1 + 3, col, run, COL_FREE, BG_WHITE );
        }
        lrow = top1 + 5;
    }

    /* the legend */
    legend_entry( lrow, in_left, in_w, COL_USED, "Used", have_sel ? other_used : used_disk_space );
    if ( have_sel ) {
        lrow++;
        legend_entry( lrow, in_left, in_w, COL_SEL, tree_nodes[selected_node].name,
                      tree_nodes[selected_node].total_size );
    }
    lrow++;
    legend_entry( lrow, in_left, in_w, COL_FREE, "Free", free_disk_space );
}
