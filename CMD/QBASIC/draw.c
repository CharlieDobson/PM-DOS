/*
 * DRAW.C - CIRCLE, PAINT, DRAW, and the graphics GET and PUT.
 *
 * CIRCLE is QBasic's: a midpoint circle in the larger of the two radii,
 * the other axis scaled by the aspect and rounded, so its pixels are the
 * ones QBasic sets.  The default aspect is the screen's (4/3 of height
 * over width) worked out in single precision, which is why it is a hair
 * under 5/6 in SCREEN 13 - enough to decide which way a few pixels go.
 * An arc keeps the points whose angle is between the start and the end;
 * a negative angle draws a radius to that end too.
 *
 * GET AND PUT keep QBasic's array layout, because programs BSAVE what
 * GET made and work out array sizes from it: two words, the width in
 * bits and the height, then each row - a byte a pixel in SCREEN 13,
 * packed bits in SCREEN 1 and 2, and four planes a row in the EGA and
 * VGA modes.
 */
#include "qb.h"

int  gfx_width_px( void );
int  gfx_height_px( void );
int  gfx_colors( void );
u8  *gfx_page( int page );
int  gfx_active_page( void );
void gfx_need( void );
void gfx_point_arg( double x, double y, int step, double *lx, double *ly );
void gfx_set_last( double x, double y );
void gfx_last( double *x, double *y );
void gfx_to_screen( double x, double y, int *sx, int *sy );
int  gfx_colour_arg( s32 c, int given );
void gfx_plot( int x, int y, int c );
int  gfx_get_px( int x, int y );
void gfx_hline( int x0, int x1, int y, int c );
void gfx_line( int x0, int y0, int x1, int y1, int c );
void gfx_view_box( int *x1, int *y1, int *x2, int *y2 );
int  gfx_fg( void );

#define PI2     6.283185307179586

/* ------------------------------------------------------------------ */
/* CIRCLE                                                              */
/* ------------------------------------------------------------------ */

static int half_up( double v )
{
    return (int)fp_floor( v + 0.5 );
}

static double default_aspect( void )
{
    /* 4/3 of the height over the width, in 256ths: QBasic keeps it so,
       and a circle's rows (or a turned DRAW move) that fall on a half
       go the way that fraction takes them */
    u32 num = (u32)gfx_height_px() * 4UL * 256UL, den = (u32)gfx_width_px() * 3UL;

    return (double)((num + den / 2) / den) / 256.0;
}

/* is an angle (0 to 2*pi) on the arc from start to end? */
static int on_arc( double angle, double start, double end )
{
    if ( start <= end ) {
        return angle >= start && angle <= end;
    }
    return angle >= start || angle <= end;
}

static double angle_of( double x, double y )
{
    double a;

    if ( x == 0 ) {
        return y >= 0 ? PI2 / 4 : PI2 * 3 / 4;
    }
    a = fp_atn( y / x );
    if ( x < 0 ) {
        a += PI2 / 2;
    } else if ( y < 0 ) {
        a += PI2;
    }
    return a;
}

/* CIRCLE (x, y), r, colour, start, end, aspect: flags 1 STEP, 2 colour,
   4 start, 8 end, 16 aspect */
void rtf_CIRCLE( void )
{
    int flags = (int)POP_L();
    double aspect = POP_D(), end = POP_D(), start = POP_D();
    s32 c = POP_L();
    double r = POP_D(), y = POP_D(), x = POP_D();
    int cx, cy, colour, arc, line_start = 0, line_end = 0, big, a, b, d;
    double rx, ry, sx_scale, sy_scale, ex, ey;
    int ix, iy;

    gfx_need();
    gfx_point_arg( x, y, flags & 1, &x, &y );
    colour = gfx_colour_arg( c, flags & 2 );
    if ( !(flags & 16) ) {
        aspect = default_aspect();
    }
    if ( r < 0 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    arc = (flags & (4 | 8)) != 0;
    if ( !(flags & 4) ) {
        start = 0;
    }
    if ( !(flags & 8) ) {
        end = PI2;
    }
    if ( start < -PI2 || start > PI2 || end < -PI2 || end > PI2 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    if ( start < 0 ) {
        line_start = 1;
        start = -start;
    }
    if ( end < 0 ) {
        line_end = 1;
        end = -end;
    }
    gfx_to_screen( x, y, &cx, &cy );
    /* the radius in pixels: the window's x scale applies */
    gfx_to_screen( x + r, y, &ix, &iy );
    r = (double)(ix - cx);
    if ( r < 0 ) {
        r = -r;
    }
    if ( DBL_BAD( aspect ) ) {
        vm_error( E_ILLEGAL_CALL );
    }
    if ( aspect < 0 ) {
        aspect = -aspect;
    }
    if ( aspect <= 1 ) {
        rx = r;
        ry = r * aspect;
        sx_scale = 1;
        sy_scale = aspect;
    } else {
        ry = r;
        rx = r / aspect;
        sx_scale = 1 / aspect;
        sy_scale = 1;
    }
    big = half_up( r );
    /* the midpoint circle in the larger radius, one octant, each point
       reflected eight ways and squashed by the aspect */
    a = 0;
    b = big;
    d = 1 - big;
    while ( a <= b ) {
        int pts[8][2], i;
        pts[0][0] = a;  pts[0][1] = b;
        pts[1][0] = -a; pts[1][1] = b;
        pts[2][0] = a;  pts[2][1] = -b;
        pts[3][0] = -a; pts[3][1] = -b;
        pts[4][0] = b;  pts[4][1] = a;
        pts[5][0] = -b; pts[5][1] = a;
        pts[6][0] = b;  pts[6][1] = -a;
        pts[7][0] = -b; pts[7][1] = -a;
        for ( i = 0; i < 8; i++ ) {
            int px = pts[i][0], py = pts[i][1];
            if ( arc && !on_arc( angle_of( (double)px, (double)py ), start, end ) ) {
                continue;
            }
            gfx_plot( cx + (px < 0 ? -half_up( -px * sx_scale ) : half_up( px * sx_scale )),
                      cy - (py < 0 ? -half_up( -py * sy_scale ) : half_up( py * sy_scale )), colour );
        }
        if ( d < 0 ) {
            d += 2 * a + 3;
        } else {
            d += 2 * (a - b) + 5;
            b--;
        }
        a++;
    }
    if ( line_start ) {
        ex = rx * fp_cos( start );
        ey = ry * fp_sin( start );
        gfx_line( cx, cy, cx + half_up( ex ), cy - half_up( ey ), colour );
    }
    if ( line_end ) {
        ex = rx * fp_cos( end );
        ey = ry * fp_sin( end );
        gfx_line( cx, cy, cx + half_up( ex ), cy - half_up( ey ), colour );
    }
    gfx_set_last( x, y );
}

/* ------------------------------------------------------------------ */
/* PAINT                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    s16 x0, x1, y;
} SPAN;

static u8   *tile;
static int   tile_rows;
static int   tile_mode;

/* the colour a tile gives a pixel */
static int tile_colour( int x, int y )
{
    const u8 *row = tile + (y % tile_rows) * (tile_mode == 3 ? 4 : tile_mode == 4 ? 2 : 1);
    int bit;

    switch ( tile_mode ) {
    case 0:                                     /* a byte a pixel row: the colour */
        return row[0];
    case 1:                                     /* 2 bits a pixel, 4 to the byte */
        return (row[0] >> (6 - (x & 3) * 2)) & 3;
    case 2:                                     /* a bit a pixel */
        return (row[0] >> (7 - (x & 7))) & 1;
    }
    bit = 7 - (x & 7);
    if ( tile_mode == 4 ) {                     /* two planes (SCREEN 10) */
        return ((row[0] >> bit) & 1) | (((row[1] >> bit) & 1) << 1);
    }
    /* four planes */
    return ((row[0] >> bit) & 1) | (((row[1] >> bit) & 1) << 1)
         | (((row[2] >> bit) & 1) << 2) | (((row[3] >> bit) & 1) << 3);
}

/* PAINT (x, y), colour or tile$, border, background$: flags 1 STEP,
   2 paint given, 4 a tile, 8 border given, 16 background given */
void rtf_PAINT( void )
{
    int flags;
    STR *bgs, *tiles;
    s32 border, c;
    double y, x;
    int w, h, sx, sy, fill, stop, vx1, vy1, vx2, vy2, nspans = 0, cap = 1024;
    u8 *seen;
    SPAN *spans;

    gfx_need();                                 /* first: its error leaves the strings on the stack, released */
    flags = (int)POP_L();
    bgs = POP_STR();
    border = POP_L();
    tiles = POP_STR();
    c = POP_L();
    y = POP_D();
    x = POP_D();
    qs_release( bgs );
    gfx_point_arg( x, y, flags & 1, &x, &y );
    w = gfx_width_px();
    h = gfx_height_px();
    tile = NULL;
    fill = (flags & 2) && !(flags & 4) ? gfx_colour_arg( c, 1 ) : gfx_fg();
    if ( flags & 4 ) {
        int per = 1;
        tile_mode = gfx_mode() == 10 ? 4 : gfx_colors() == 256 ? 0 : gfx_colors() == 4 ? 1 : gfx_colors() == 2 ? 2 : 3;
        per = tile_mode == 3 ? 4 : tile_mode == 4 ? 2 : 1;
        if ( QS_LEN( tiles ) == 0 || QS_LEN( tiles ) > 64 ) {        /* 64 bytes in every mode, as QBasic */
            qs_release( tiles );
            vm_error( E_ILLEGAL_CALL );
        }
        tile_rows = (int)((QS_LEN( tiles ) + (u32)per - 1) / (u32)per);
        tile = (u8 *)try_alloc( (u32)(tile_rows * per) );
        if ( tile == NULL ) {
            qs_release( tiles );
            vm_error( E_OUT_OF_MEMORY );
        }
        mem_set( tile, 0, (u32)(tile_rows * per) );
        mem_cpy( tile, tiles->text, tiles->len );
    }
    qs_release( tiles );
    stop = (flags & 8) ? gfx_colour_arg( border, 1 ) : fill;
    gfx_to_screen( x, y, &sx, &sy );
    gfx_set_last( x, y );
    gfx_view_box( &vx1, &vy1, &vx2, &vy2 );
    if ( sx < vx1 || sx > vx2 || sy < vy1 || sy > vy2 || gfx_get_px( sx, sy ) == stop ) {
        xfree( tile );
        return;
    }
    seen = (u8 *)try_alloc( (u32)w * (u32)h / 8 + 1 );
    spans = (SPAN *)try_alloc( (u32)cap * sizeof( SPAN ) );
    if ( seen == NULL || spans == NULL ) {
        xfree( seen );
        xfree( spans );
        xfree( tile );
        vm_error( E_OUT_OF_MEMORY );
    }
    mem_set( seen, 0, (u32)w * (u32)h / 8 + 1 );
    spans[nspans].x0 = (s16)sx;
    spans[nspans].x1 = (s16)sx;
    spans[nspans].y = (s16)sy;
    nspans++;
#define SEEN( px, py )  (seen[((u32)(py) * (u32)w + (u32)(px)) >> 3] & (1 << (((u32)(py) * (u32)w + (u32)(px)) & 7)))
#define SETSEEN( px, py ) (seen[((u32)(py) * (u32)w + (u32)(px)) >> 3] |= (u8)(1 << (((u32)(py) * (u32)w + (u32)(px)) & 7)))
    while ( nspans > 0 ) {
        SPAN s = spans[--nspans];
        int left, right, px, ny, dir;
        if ( SEEN( s.x0, s.y ) || gfx_get_px( s.x0, s.y ) == stop ) {
            continue;
        }
        /* the whole run of this row the seed is in */
        left = s.x0;
        while ( left > vx1 && !SEEN( left - 1, s.y ) && gfx_get_px( left - 1, s.y ) != stop ) {
            left--;
        }
        right = s.x0;
        while ( right < vx2 && !SEEN( right + 1, s.y ) && gfx_get_px( right + 1, s.y ) != stop ) {
            right++;
        }
        for ( px = left; px <= right; px++ ) {
            SETSEEN( px, s.y );
            gfx_plot( px, s.y, tile ? tile_colour( px, s.y ) : fill );
        }
        /* seeds in the rows above and below */
        for ( dir = -1; dir <= 1; dir += 2 ) {
            ny = s.y + dir;
            if ( ny < vy1 || ny > vy2 ) {
                continue;
            }
            px = left;
            while ( px <= right ) {
                while ( px <= right && (SEEN( px, ny ) || gfx_get_px( px, ny ) == stop) ) {
                    px++;
                }
                if ( px > right ) {
                    break;
                }
                if ( nspans == cap ) {
                    SPAN *grown;
                    cap *= 2;
                    grown = (SPAN *)try_realloc( spans, (u32)cap * sizeof( SPAN ) );
                    if ( grown == NULL ) {
                        xfree( seen );
                        xfree( spans );
                        xfree( tile );
                        vm_error( E_OUT_OF_MEMORY );
                    }
                    spans = grown;
                }
                spans[nspans].x0 = (s16)px;
                spans[nspans].x1 = (s16)px;
                spans[nspans].y = (s16)ny;
                nspans++;
                while ( px <= right && !SEEN( px, ny ) && gfx_get_px( px, ny ) != stop ) {
                    px++;
                }
            }
        }
    }
#undef SEEN
#undef SETSEEN
    xfree( seen );
    xfree( spans );
    xfree( tile );
    tile = NULL;
}

/* ------------------------------------------------------------------ */
/* DRAW                                                                */
/* ------------------------------------------------------------------ */

STR **mem_varptr_string( const u8 *bytes, QTYPE *type, void **addr );

static int draw_colour;
static int draw_angle;          /* A: 0-3 */
static double draw_turn;        /* TA, degrees */
static int draw_scale = 4;

void draw_reset( void )
{
    draw_angle = 0;
    draw_turn = 0;
    draw_scale = 4;
}

static void skip_blanks( MACRO *cur )
{
    while ( cur->pos < cur->len && (cur->text[cur->pos] == ' ' || cur->text[cur->pos] == ';'
                                    || cur->text[cur->pos] == '\t') ) {
        cur->pos++;
    }
}

/* a number in a DRAW or PLAY string: digits, or = and a VARPTR$ */
int macro_number( MACRO *cur, double *val, int allow_sign )
{
    int neg = 0, any = 0;
    double v = 0;

    skip_blanks( cur );
    if ( cur->pos < cur->len && cur->text[cur->pos] == '=' ) {
        QTYPE type;
        void *addr;
        cur->pos++;
        if ( cur->pos + 3 > cur->len ) {
            vm_error( E_ILLEGAL_CALL );
        }
        mem_varptr_string( (const u8 *)cur->text + cur->pos, &type, &addr );
        cur->pos += 3;
        switch ( TKIND( type ) ) {
        case TY_INT:    *val = *(s16 *)addr; break;
        case TY_LONG:   *val = *(s32 *)addr; break;
        case TY_SINGLE: *val = *(float *)addr; break;
        case TY_DOUBLE: *val = *(double *)addr; break;
        default:        vm_error( E_ILLEGAL_CALL );
        }
        if ( cur->pos < cur->len && cur->text[cur->pos] == ';' ) {
            cur->pos++;
        }
        return 1;
    }
    if ( allow_sign && cur->pos < cur->len && (cur->text[cur->pos] == '-' || cur->text[cur->pos] == '+') ) {
        neg = cur->text[cur->pos] == '-';
        cur->pos++;
    }
    while ( cur->pos < cur->len && cur->text[cur->pos] >= '0' && cur->text[cur->pos] <= '9' ) {
        v = v * 10 + (cur->text[cur->pos] - '0');
        cur->pos++;
        any = 1;
    }
    *val = neg ? -v : v;
    return any;
}

/* a move of (dx, dy) in drawing units from the current point, turned by
   A and TA; blank: no line; back: return to where it started */
static void draw_move( double dx, double dy, int blank, int back, int relative, double ax, double ay )
{
    double lx, ly, nx, ny, rad, cs, sn, tx;
    int sx0, sy0, sx1, sy1;

    gfx_last( &lx, &ly );
    if ( relative ) {
        /* S scales to whole units, cut towards zero */
        dx = fp_trunc( dx * draw_scale / 4 );
        dy = fp_trunc( dy * draw_scale / 4 );
        rad = (draw_angle * 90 + draw_turn) * PI2 / 360;
        if ( rad != 0 ) {
            /* turned on a square grid: y is in the screen's aspect */
            double aspect = default_aspect();
            cs = fp_cos( rad );
            sn = fp_sin( rad );
            dy = dy / aspect;
            tx = dx * cs + dy * sn;
            dy = (dy * cs - dx * sn) * aspect;
            dx = tx;
        }
        nx = lx + dx;
        ny = ly + dy;
    } else {
        nx = ax;
        ny = ay;
    }
    if ( !blank ) {
        gfx_to_screen( lx, ly, &sx0, &sy0 );
        gfx_to_screen( nx, ny, &sx1, &sy1 );
        gfx_line( sx0, sy0, sx1, sy1, draw_colour );
    }
    if ( !back ) {
        gfx_set_last( fp_round( nx ), fp_round( ny ) );
    }
}

static void draw_string( const char *text, u32 len, int depth )
{
    MACRO cur;
    int ch, blank = 0, back = 0;
    double n, n2;

    if ( depth > 8 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    cur.text = text;
    cur.len = len;
    cur.pos = 0;
    for ( ;; ) {
        skip_blanks( &cur );
        if ( cur.pos >= cur.len ) {
            break;
        }
        ch = ch_upper( cur.text[cur.pos++] );
        switch ( ch ) {
        case 'B':
            blank = 1;
            continue;
        case 'N':
            back = 1;
            continue;
        case 'U': case 'D': case 'L': case 'R':
        case 'E': case 'F': case 'G': case 'H':
            if ( !macro_number( &cur, &n, 0 ) ) {
                n = 1;
            }
            switch ( ch ) {
            case 'U': draw_move( 0, -n, blank, back, 1, 0, 0 ); break;
            case 'D': draw_move( 0, n, blank, back, 1, 0, 0 ); break;
            case 'L': draw_move( -n, 0, blank, back, 1, 0, 0 ); break;
            case 'R': draw_move( n, 0, blank, back, 1, 0, 0 ); break;
            case 'E': draw_move( n, -n, blank, back, 1, 0, 0 ); break;
            case 'F': draw_move( n, n, blank, back, 1, 0, 0 ); break;
            case 'G': draw_move( -n, n, blank, back, 1, 0, 0 ); break;
            case 'H': draw_move( -n, -n, blank, back, 1, 0, 0 ); break;
            }
            break;
        case 'M': {
            int relative;
            skip_blanks( &cur );
            relative = cur.pos < cur.len && (cur.text[cur.pos] == '+' || cur.text[cur.pos] == '-');
            if ( !macro_number( &cur, &n, 1 ) ) {
                vm_error( E_ILLEGAL_CALL );
            }
            skip_blanks( &cur );
            if ( cur.pos >= cur.len || cur.text[cur.pos] != ',' ) {
                vm_error( E_ILLEGAL_CALL );
            }
            cur.pos++;
            if ( !macro_number( &cur, &n2, 1 ) ) {
                vm_error( E_ILLEGAL_CALL );
            }
            if ( relative ) {
                draw_move( n, n2, blank, back, 1, 0, 0 );
            } else {
                draw_move( 0, 0, blank, back, 0, n, n2 );
            }
            break;
        }
        case 'A':
            if ( !macro_number( &cur, &n, 0 ) || n < 0 || n > 3 ) {
                vm_error( E_ILLEGAL_CALL );
            }
            draw_angle = (int)n;
            draw_turn = 0;
            break;
        case 'T':
            skip_blanks( &cur );
            if ( cur.pos >= cur.len || ch_upper( cur.text[cur.pos] ) != 'A' ) {
                vm_error( E_ILLEGAL_CALL );
            }
            cur.pos++;
            if ( !macro_number( &cur, &n, 1 ) || n < -360 || n > 360 ) {
                vm_error( E_ILLEGAL_CALL );
            }
            draw_turn = n;
            draw_angle = 0;
            break;
        case 'C':
            if ( !macro_number( &cur, &n, 0 ) ) {
                vm_error( E_ILLEGAL_CALL );
            }
            draw_colour = gfx_colour_arg( (s32)n, 1 );
            break;
        case 'S':
            if ( !macro_number( &cur, &n, 0 ) || n < 1 || n > 255 ) {
                vm_error( E_ILLEGAL_CALL );
            }
            draw_scale = (int)n;
            break;
        case 'P': {
            double lx, ly;
            if ( !macro_number( &cur, &n, 0 ) ) {
                vm_error( E_ILLEGAL_CALL );
            }
            skip_blanks( &cur );
            if ( cur.pos >= cur.len || cur.text[cur.pos] != ',' ) {
                vm_error( E_ILLEGAL_CALL );
            }
            cur.pos++;
            if ( !macro_number( &cur, &n2, 0 ) ) {
                vm_error( E_ILLEGAL_CALL );
            }
            gfx_last( &lx, &ly );
            PUSH_D( lx );
            PUSH_D( ly );
            PUSH_L( (s32)n );
            PUSH_STR( NULL );
            PUSH_L( (s32)n2 );
            PUSH_STR( NULL );
            PUSH_L( 2 | 8 );
            rtf_PAINT();
            break;
        }
        case 'X': {
            QTYPE type;
            void *addr;
            STR **slot;
            STR *sub;
            if ( cur.pos + 3 > cur.len ) {
                vm_error( E_ILLEGAL_CALL );
            }
            slot = mem_varptr_string( (const u8 *)cur.text + cur.pos, &type, &addr );
            cur.pos += 3;
            if ( slot == NULL ) {
                vm_error( E_ILLEGAL_CALL );
            }
            sub = qs_retain( *slot );
            PUSH_STR( sub );                    /* held on the stack: an error in it frees it */
            draw_string( QS_TEXT( sub ), QS_LEN( sub ), depth + 1 );
            qs_release( POP_STR() );
            if ( cur.pos < cur.len && cur.text[cur.pos] == ';' ) {
                cur.pos++;
            }
            break;
        }
        default:
            vm_error( E_ILLEGAL_CALL );
        }
        blank = back = 0;
    }
}

void rtf_DRAW( void )
{
    STR *s = vm.sp[-1].str;              /* left on the stack: an error frees it */

    gfx_need();
    draw_colour = gfx_fg();
    draw_string( QS_TEXT( s ), QS_LEN( s ), 0 );
    qs_release( POP_STR() );
}

/* ------------------------------------------------------------------ */
/* GET and PUT                                                         */
/* ------------------------------------------------------------------ */

/* bits a pixel, and planes */
static void layout( int *bpp, int *planes )
{
    if ( gfx_mode() == 10 ) {
        *bpp = 1;                       /* two planes, as the EGA keeps them */
        *planes = 2;
        return;
    }
    switch ( gfx_colors() ) {
    case 256:   *bpp = 8; *planes = 1; return;
    case 4:     *bpp = 2; *planes = 1; return;
    case 2:     *bpp = 1; *planes = 1; return;
    }
    *bpp = 1;
    *planes = 4;
}

/* the bytes from "elem" to the end of the array */
static u32 room_in( ARR *arr, u8 *elem )
{
    u8 *end = arr->data + arr->count * arr->esize;

    return elem >= arr->data && elem <= end ? (u32)(end - elem) : 0;
}

/* GET (x1,y1)-(x2,y2), array(i): flags 1 STEP first, 2 STEP second */
void rtf_GGET( void )
{
    int flags = (int)POP_L();
    u8 *elem = (u8 *)POP_A();
    ARR *arr = *(ARR **)POP_A();
    double y2 = POP_D(), x2 = POP_D(), y1 = POP_D(), x1 = POP_D();
    int sx1, sy1, sx2, sy2, t, w, h, bpp, planes, rowbytes, x, y, p;
    u32 need;
    u8 *out, *pic;

    gfx_need();
    if ( arr == NULL ) {
        vm_error( E_SUBSCRIPT );
    }
    if ( TKIND( arr->type ) == TY_STR ) {
        vm_error( E_TYPE_MISMATCH );            /* its elements are string pointers */
    }
    if ( elem == NULL ) {
        elem = arr->data;
    }
    gfx_point_arg( x1, y1, flags & 1, &x1, &y1 );
    gfx_point_arg( x2, y2, flags & 2, &x2, &y2 );
    gfx_to_screen( x1, y1, &sx1, &sy1 );
    gfx_to_screen( x2, y2, &sx2, &sy2 );
    if ( sx1 > sx2 ) {
        t = sx1; sx1 = sx2; sx2 = t;
    }
    if ( sy1 > sy2 ) {
        t = sy1; sy1 = sy2; sy2 = t;
    }
    if ( sx1 < 0 || sy1 < 0 || sx2 >= gfx_width_px() || sy2 >= gfx_height_px() ) {
        vm_error( E_ILLEGAL_CALL );
    }
    w = sx2 - sx1 + 1;
    h = sy2 - sy1 + 1;
    layout( &bpp, &planes );
    rowbytes = (w * bpp + 7) / 8;
    need = 4 + (u32)rowbytes * (u32)planes * (u32)h;
    if ( need > room_in( arr, elem ) ) {
        vm_error( E_ILLEGAL_CALL );
    }
    ((u16 *)elem)[0] = (u16)(w * bpp);
    ((u16 *)elem)[1] = (u16)h;
    out = elem + 4;
    pic = gfx_page( gfx_active_page() );
    for ( y = 0; y < h; y++ ) {
        const u8 *row = pic + (u32)(sy1 + y) * (u32)gfx_width_px() + (u32)sx1;
        for ( p = 0; p < planes; p++ ) {
            mem_set( out, 0, (u32)rowbytes );
            for ( x = 0; x < w; x++ ) {
                int c = row[x];
                if ( bpp == 8 ) {
                    out[x] = (u8)c;
                } else if ( planes > 1 ) {
                    if ( (c >> p) & 1 ) {
                        out[x >> 3] |= (u8)(0x80 >> (x & 7));
                    }
                } else {
                    int bit = x * bpp;
                    out[bit >> 3] |= (u8)((c & ((1 << bpp) - 1)) << (8 - bpp - (bit & 7)));
                }
            }
            out += rowbytes;
        }
    }
}

/* PUT (x, y), array(i), action: 0 PSET, 1 PRESET, 2 AND, 3 OR, 4 XOR */
void rtf_GPUT( void )
{
    int flags = (int)POP_L(), action = (int)POP_L();
    u8 *elem = (u8 *)POP_A();
    ARR *arr = *(ARR **)POP_A();
    double y = POP_D(), x = POP_D();
    int sx, sy, w, h, bpp, planes, rowbytes, px, py, p, mask, c, old;
    const u8 *in;
    u32 need;

    gfx_need();
    if ( arr == NULL ) {
        vm_error( E_SUBSCRIPT );
    }
    if ( TKIND( arr->type ) == TY_STR ) {
        vm_error( E_TYPE_MISMATCH );            /* its elements are string pointers */
    }
    if ( elem == NULL ) {
        elem = arr->data;
    }
    if ( room_in( arr, elem ) < 4 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    layout( &bpp, &planes );
    mask = bpp == 8 ? 255 : planes > 1 ? (1 << planes) - 1 : (1 << bpp) - 1;
    w = ((u16 *)elem)[0] / bpp;
    h = ((u16 *)elem)[1];
    rowbytes = (w * bpp + 7) / 8;
    need = 4 + (u32)rowbytes * (u32)planes * (u32)h;
    if ( w <= 0 || h <= 0 || need > room_in( arr, elem ) ) {
        vm_error( E_ILLEGAL_CALL );
    }
    gfx_point_arg( x, y, flags & 1, &x, &y );
    gfx_to_screen( x, y, &sx, &sy );
    if ( sx < 0 || sy < 0 || sx + w > gfx_width_px() || sy + h > gfx_height_px() ) {
        vm_error( E_ILLEGAL_CALL );
    }
    in = elem + 4;
    for ( py = 0; py < h; py++ ) {
        for ( px = 0; px < w; px++ ) {
            if ( bpp == 8 ) {
                c = in[px];
            } else if ( planes > 1 ) {
                c = 0;
                for ( p = 0; p < planes; p++ ) {
                    if ( in[p * rowbytes + (px >> 3)] & (0x80 >> (px & 7)) ) {
                        c |= 1 << p;
                    }
                }
            } else {
                int bit = px * bpp;
                c = (in[bit >> 3] >> (8 - bpp - (bit & 7))) & mask;
            }
            old = gfx_get_px( sx + px, sy + py );
            if ( old < 0 ) {
                continue;
            }
            switch ( action ) {
            case 0:                     break;
            case 1: c = ~c & mask;      break;
            case 2: c = old & c;        break;
            case 3: c = old | c;        break;
            default: c = old ^ c;       break;
            }
            gfx_plot( sx + px, sy + py, c );
        }
        in += rowbytes * planes;
    }
}
