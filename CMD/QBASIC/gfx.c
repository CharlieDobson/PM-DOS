/*
 * GFX.C - the graphics modes: SCREEN, the pages, the palette, the
 * coordinates, PSET and LINE, and the text drawn in a graphics mode.
 *
 * EVERY MODE IS A PICTURE IN MEMORY, a byte a pixel, a page at a time;
 * the drawing statements change the page being worked on, and gfx_flush
 * copies what changed on the page being shown to the screen in the form
 * the hardware mode wants.  So POINT and GET read memory, never video
 * memory, page flipping is a matter of which picture is copied, and the
 * output screen can be put back whole after the IDE has been over it.
 *
 * THE HARDWARE MODE is whichever shows the picture most simply:
 *
 *   SCREEN 1, 7, 13  320x200      BIOS mode 13h, a byte a pixel
 *   SCREEN 2, 8      640x200      BIOS mode 0Eh, four planes
 *   SCREEN 9, 10     640x350      BIOS mode 10h, four planes
 *   SCREEN 11, 12    640x480      BIOS mode 12h, four planes
 *
 * and the colours are the DAC's.  A four-plane mode is written a plane
 * at a time - the changed lines turned into planes in memory first - so
 * a whole screen costs four writes to the sequencer: a port written from
 * ring 3 is an instruction the kernel has to emulate.  This is DOSSHELL's
 * GFX.C's way, and so are the fonts: the video BIOS's own, read out of
 * the ROM where a mode set leaves interrupt vector 43h pointing.
 */
#include "qb.h"

typedef struct {
    int mode;
    int w, h;
    int colors;                 /* the highest attribute + 1 */
    int pages;
    int hw;                     /* the BIOS mode that shows it */
    int rows, cols, font;       /* the text */
} GMODE;

static const GMODE modes[] = {
    {  1, 320, 200,   4, 1, 0x13, 25, 40,  8 },
    {  2, 640, 200,   2, 1, 0x0E, 25, 80,  8 },
    {  7, 320, 200,  16, 8, 0x13, 25, 40,  8 },
    {  8, 640, 200,  16, 4, 0x0E, 25, 80,  8 },
    {  9, 640, 350,  16, 2, 0x10, 25, 80, 14 },
    { 10, 640, 350,   4, 2, 0x10, 25, 80, 14 },
    { 11, 640, 480,   2, 1, 0x12, 30, 80, 16 },
    { 12, 640, 480,  16, 1, 0x12, 30, 80, 16 },
    { 13, 320, 200, 256, 1, 0x13, 25, 40,  8 }
};

static const GMODE *gm;         /* NULL in SCREEN 0 */
static u8  *gpages[8];
static int  gapage, gvpage;
static int  text_rows, text_font;
static u32  vid_sel;
static int  hw_set;             /* the hardware mode is on the screen */

/* what changed on the page shown: a box */
static int  dx0 = 32767, dy0 = 32767, dx1 = -1, dy1 = -1;

/* colours */
static int  draw_fg, draw_bg;
static u8   rgb[256][3];        /* each attribute's colour, six bits a gun */
static int  pal1;               /* SCREEN 1's palette, 0 or 1 */

/* coordinates */
static int    vx1, vy1, vx2, vy2;       /* the viewport, physical */
static int    view_on, view_screen;
static double wx1, wy1, wx2, wy2;
static double wfx, wfy;             /* WINDOW's pixels per unit, */
static double wgx, wgy;             /* and units per pixel */
static int    win_on, win_screen;
static double lpx, lpy;                 /* the last point, logical */

/* the fonts */
static u8  font8[8 * 256], font14[14 * 256], font16[16 * 256];
static int have8, have14, have16;

static const u8 ega16[16][3] = {        /* red, green, blue */
    {  0,  0,  0 }, {  0,  0, 42 }, {  0, 42,  0 }, {  0, 42, 42 },
    { 42,  0,  0 }, { 42,  0, 42 }, { 42, 21,  0 }, { 42, 42, 42 },
    { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
    { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 }
};

/* the DAC entries the attribute controller's defaults send a planar
   mode's sixteen attributes to */
static const u8 ac_ega[16] = { 0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F };
static const u8 ac_cga[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17 };

int  con_set_pages( int active, int visual );
int  con_page_count( void );
int  con_copy_page( int from, int to );
void con_set_geometry( int rows, int cols, int pages );

int gfx_mode( void )
{
    return gm ? gm->mode : 0;
}

int gfx_view_on( void )
{
    return gm != NULL && view_on;
}

int gfx_width_px( void )
{
    return gm ? gm->w : 0;
}

int gfx_height_px( void )
{
    return gm ? gm->h : 0;
}

int gfx_colors( void )
{
    return gm ? gm->colors : 16;
}

u8 *gfx_page( int page )
{
    return gm && page >= 0 && page < gm->pages ? gpages[page] : NULL;
}

/* the end prompt goes on the page that is shown: -> the active page it
   replaced, to give back */
int gfx_draw_on_visible( void )
{
    int was = gapage;

    gapage = gvpage;
    return was;
}

void gfx_draw_on( int page )
{
    if ( gm != NULL && page >= 0 && page < gm->pages ) {
        gapage = page;
    }
}

int gfx_active_page( void )
{
    return gapage;
}

/* ------------------------------------------------------------------ */
/* colours                                                             */
/* ------------------------------------------------------------------ */

/* an EGA colour number 0-63 (rgbRGB) as six-bit guns */
static void ega64( int c, u8 *out )
{
    out[0] = (u8)(((c >> 2) & 1) * 42 + ((c >> 5) & 1) * 21);
    out[1] = (u8)(((c >> 1) & 1) * 42 + ((c >> 4) & 1) * 21);
    out[2] = (u8)((c & 1) * 42 + ((c >> 3) & 1) * 21);
}

/* the standard VGA 256-colour palette, made when the DAC cannot be read */
static void default256( void )
{
    static const u8 grey[16] = { 0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63 };
    static const u8 tops[3] = { 63, 28, 16 };
    static const u8 lows[3][3] = { { 0, 31, 45 }, { 0, 14, 20 }, { 0, 8, 11 } };
    int i, band, sat, hue, at = 32;
    u8 lo, hi, steps[5];

    for ( i = 0; i < 16; i++ ) {
        rgb[i][0] = ega16[i][0];
        rgb[i][1] = ega16[i][1];
        rgb[i][2] = ega16[i][2];
        rgb[16 + i][0] = rgb[16 + i][1] = rgb[16 + i][2] = grey[i];
    }
    for ( band = 0; band < 3; band++ ) {
        for ( sat = 0; sat < 3; sat++ ) {
            hi = tops[band];
            lo = lows[band][sat];
            for ( i = 0; i < 5; i++ ) {
                steps[i] = (u8)(lo + (hi - lo) * i / 4);
            }
            for ( hue = 0; hue < 24; hue++ ) {
                int seg = hue / 4, step = hue % 4;
                u8 r, g, b;
                switch ( seg ) {
                case 0:  r = steps[step]; g = lo; b = hi; break;           /* blue to magenta */
                case 1:  r = hi; g = lo; b = steps[4 - step]; break;       /* magenta to red */
                case 2:  r = hi; g = steps[step]; b = lo; break;           /* red to yellow */
                case 3:  r = steps[4 - step]; g = hi; b = lo; break;       /* yellow to green */
                case 4:  r = lo; g = hi; b = steps[step]; break;           /* green to cyan */
                default: r = lo; g = steps[4 - step]; b = hi; break;       /* cyan to blue */
                }
                rgb[at][0] = r;
                rgb[at][1] = g;
                rgb[at][2] = b;
                at++;
            }
        }
    }
    for ( ; at < 256; at++ ) {
        rgb[at][0] = rgb[at][1] = rgb[at][2] = 0;
    }
}

/* the mode's colours as the hardware should show them */
static void apply_palette( void )
{
    u8 entries[256 * 4];
    int i, count;
    const u8 *map = NULL;

    if ( gm == NULL ) {
        return;
    }
    count = gm->colors;
    if ( gm->hw == 0x10 || gm->hw == 0x12 ) {
        map = ac_ega;
    } else if ( gm->hw == 0x0E ) {
        map = ac_cga;
    }
    if ( map == NULL ) {
        for ( i = 0; i < count; i++ ) {
            entries[i * 4] = rgb[i][2];
            entries[i * 4 + 1] = rgb[i][1];
            entries[i * 4 + 2] = rgb[i][0];
            entries[i * 4 + 3] = 0;
        }
        sys_video_palette( 0, count, entries );
        return;
    }
    for ( i = 0; i < 16; i++ ) {
        int c = i < count ? i : 0;
        entries[0] = rgb[c][2];
        entries[1] = rgb[c][1];
        entries[2] = rgb[c][0];
        entries[3] = 0;
        sys_video_palette( map[i], 1, entries );
    }
}

/* the colours a mode starts with */
static void default_palette( void )
{
    int i;

    switch ( gm->mode ) {
    case 13:
        {
            u8 entries[256 * 4];
            if ( hw_set && sys_video_palette_get( 0, 256, entries ) ) {
                for ( i = 0; i < 256; i++ ) {
                    rgb[i][0] = entries[i * 4 + 2];
                    rgb[i][1] = entries[i * 4 + 1];
                    rgb[i][2] = entries[i * 4];
                }
            } else {
                default256();
            }
        }
        break;
    case 1:
        pal1 = 1;
        mem_cpy( rgb[0], ega16[0], 3 );
        mem_cpy( rgb[1], ega16[11], 3 );
        mem_cpy( rgb[2], ega16[13], 3 );
        mem_cpy( rgb[3], ega16[15], 3 );
        break;
    case 2:
    case 11:
        mem_cpy( rgb[0], ega16[0], 3 );
        mem_cpy( rgb[1], ega16[15], 3 );
        break;
    case 10:
        rgb[0][0] = rgb[0][1] = rgb[0][2] = 0;
        rgb[1][0] = rgb[1][1] = rgb[1][2] = 21;
        rgb[2][0] = rgb[2][1] = rgb[2][2] = 42;
        rgb[3][0] = rgb[3][1] = rgb[3][2] = 63;
        break;
    default:
        for ( i = 0; i < 16; i++ ) {
            mem_cpy( rgb[i], ega16[i], 3 );
        }
        break;
    }
}

/* PALETTE attribute, colour: the colour is the mode's own kind */
static void set_attr_color( int attr, s32 color )
{
    if ( gm == NULL ) {
        /* SCREEN 0: EGA colours through the text attributes */
        if ( attr < 0 || attr > 15 || color < -1 || color > 63 ) {
            vm_error( E_ILLEGAL_CALL );
        }
        return;
    }
    if ( attr < 0 || attr >= gm->colors ) {
        vm_error( E_ILLEGAL_CALL );
    }
    if ( color == -1 ) {
        return;                                 /* -1 leaves it as it is */
    }
    switch ( gm->mode ) {
    case 1:
    case 2:
    case 7:
    case 8:
        if ( color < 0 || color > 15 ) {
            vm_error( E_ILLEGAL_CALL );
        }
        mem_cpy( rgb[attr], ega16[color], 3 );
        break;
    case 9:
        if ( color < 0 || color > 63 ) {
            vm_error( E_ILLEGAL_CALL );
        }
        ega64( (int)color, rgb[attr] );
        break;
    case 10:
        if ( color < 0 || color > 8 ) {
            vm_error( E_ILLEGAL_CALL );
        }
        rgb[attr][0] = rgb[attr][1] = rgb[attr][2] = (u8)(color * 63 / 8);
        break;
    default:
        /* 65536 * blue + 256 * green + red, six bits each */
        if ( color < 0 || (color & 0xC0C0C0L) != 0 ) {
            vm_error( E_ILLEGAL_CALL );
        }
        rgb[attr][0] = (u8)(color & 63);
        rgb[attr][1] = (u8)((color >> 8) & 63);
        rgb[attr][2] = (u8)((color >> 16) & 63);
        break;
    }
}

/* PALETTE [attribute, colour]: mask (1 given), attribute, colour */
void rtf_PALETTE( void )
{
    int given = (int)POP_L();
    s32 color = POP_L(), attr = POP_L();

    if ( !given ) {
        if ( gm != NULL ) {
            default_palette();
            apply_palette();
        }
        return;
    }
    set_attr_color( (int)attr, color );
    if ( gm != NULL ) {
        apply_palette();
    }
}

/* PALETTE USING array(i): the array's slot and element (or 0) */
void rtf_PALUSING( void )
{
    u8 *elem = (u8 *)POP_A();
    ARR *arr = *(ARR **)POP_A();
    int i, count = gm ? gm->colors : 16;
    u8 *end;

    if ( arr == NULL ) {
        vm_error( E_SUBSCRIPT );
    }
    if ( elem == NULL ) {
        elem = arr->data;
    }
    end = arr->data + arr->count * arr->esize;
    if ( count > 256 ) {
        count = 256;
    }
    for ( i = 0; i < count; i++ ) {
        s32 color;
        if ( elem + arr->esize > end ) {
            vm_error( E_ILLEGAL_CALL );
        }
        color = arr->esize == 2 ? *(s16 *)elem : *(s32 *)elem;
        set_attr_color( i, color );
        elem += arr->esize;
    }
    if ( gm != NULL ) {
        apply_palette();
    }
}

/* COLOR in a graphics mode: what the arguments mean depends on the mode */
void gfx_set_colors( int fgc, int bgc )
{
    switch ( gm->mode ) {
    case 1:
        /* COLOR background, palette */
        if ( fgc >= 0 ) {
            if ( fgc > 255 ) {
                vm_error( E_ILLEGAL_CALL );
            }
            mem_cpy( rgb[0], ega16[fgc & 15], 3 );
        }
        if ( bgc >= 0 ) {
            pal1 = bgc & 1;
            if ( pal1 ) {
                mem_cpy( rgb[1], ega16[11], 3 );
                mem_cpy( rgb[2], ega16[13], 3 );
                mem_cpy( rgb[3], ega16[15], 3 );
            } else {
                mem_cpy( rgb[1], ega16[10], 3 );
                mem_cpy( rgb[2], ega16[12], 3 );
                mem_cpy( rgb[3], ega16[14], 3 );
            }
        }
        apply_palette();
        return;
    case 2:
    case 11:
        vm_error( E_ILLEGAL_CALL );
        return;
    case 12:
    case 13:
        if ( bgc >= 0 ) {
            vm_error( E_ILLEGAL_CALL );
        }
        if ( fgc >= 0 ) {
            if ( fgc >= gm->colors ) {
                vm_error( E_ILLEGAL_CALL );
            }
            draw_fg = fgc;
        }
        return;
    }
    /* 7, 8, 9, 10: COLOR foreground, background (the colour of 0) */
    if ( fgc >= 0 ) {
        if ( fgc >= gm->colors ) {
            vm_error( E_ILLEGAL_CALL );
        }
        draw_fg = fgc;
    }
    if ( bgc >= 0 ) {
        set_attr_color( 0, bgc );
        draw_bg = 0;
        apply_palette();
    }
}

int gfx_text_attr( int fgc, int bgc )
{
    (void)fgc;
    (void)bgc;
    return draw_fg & 0xFF;
}

/* a colour number as the mode takes it: SCREEN 13 wraps, the CGA modes
   go no higher than they can show, the rest wrap at 16 */
static int colour_of( s32 c )
{
    switch ( gm->mode ) {
    case 13:
        return (int)(c & 255);
    case 1:
    case 2:
    case 10:
    case 11:
        if ( c < 0 ) {
            return 0;
        }
        return c >= gm->colors ? gm->colors - 1 : (int)c;
    }
    return (int)(c & 15);
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

static void mark( int x0, int y0, int x1, int y1 )
{
    if ( x0 < dx0 ) {
        dx0 = x0;
    }
    if ( y0 < dy0 ) {
        dy0 = y0;
    }
    if ( x1 > dx1 ) {
        dx1 = x1;
    }
    if ( y1 > dy1 ) {
        dy1 = y1;
    }
}

static void mark_all( void )
{
    if ( gm != NULL ) {
        mark( 0, 0, gm->w - 1, gm->h - 1 );
    }
}

/* the planar modes' picture goes out in bands of rows, a plane at a
   time: the band's bytes for each plane are made here first */
#define BAND        60
static u8 *band_buf;            /* 4 planes x BAND rows x 80 bytes */

/* anything drawn since the last flush? */
int gfx_dirty( void )
{
    return dx1 >= dx0 && dy1 >= dy0;
}

/* the changed box of the page shown, onto the screen */
void gfx_flush( void )
{
    u8 *pic, *planes[4];
    int y, x, p, bx0, bx1, nb, top, bottom;
    u32 off;

    if ( gm == NULL || !hw_set || dx1 < dx0 || dy1 < dy0 ) {
        dx0 = dy0 = 32767;
        dx1 = dy1 = -1;
        return;
    }
    if ( dx0 < 0 ) {
        dx0 = 0;
    }
    if ( dy0 < 0 ) {
        dy0 = 0;
    }
    if ( dx1 >= gm->w ) {
        dx1 = gm->w - 1;
    }
    if ( dy1 >= gm->h ) {
        dy1 = gm->h - 1;
    }
    pic = gpages[gvpage];
    if ( gm->hw == 0x13 ) {
        for ( y = dy0; y <= dy1; y++ ) {
            off = (u32)y * 320 + (u32)dx0;
            vid_put( vid_sel, off, pic + off, (u32)(dx1 - dx0 + 1) );
        }
    } else {
        if ( band_buf == NULL ) {
            band_buf = (u8 *)try_alloc( 4UL * BAND * 80 );
            if ( band_buf == NULL ) {
                return;                         /* shown at the next flush */
            }
        }
        for ( p = 0; p < 4; p++ ) {
            planes[p] = band_buf + (u32)p * BAND * 80;
        }
        bx0 = dx0 >> 3;
        bx1 = dx1 >> 3;
        nb = bx1 - bx0 + 1;
        for ( top = dy0; top <= dy1; top += BAND ) {
            bottom = top + BAND - 1 < dy1 ? top + BAND - 1 : dy1;
            for ( y = top; y <= bottom; y++ ) {
                const u8 *row = pic + (u32)y * (u32)gm->w + (u32)bx0 * 8;
                u8 *dst0 = planes[0] + (y - top) * nb;
                u8 *dst1 = planes[1] + (y - top) * nb;
                u8 *dst2 = planes[2] + (y - top) * nb;
                u8 *dst3 = planes[3] + (y - top) * nb;
                for ( x = 0; x < nb; x++ ) {
                    u8 b0 = 0, b1 = 0, b2 = 0, b3 = 0, bit;
                    int i;
                    for ( i = 0, bit = 0x80; i < 8; i++, bit >>= 1 ) {
                        u8 c = row[x * 8 + i];
                        if ( c & 1 ) {
                            b0 |= bit;
                        }
                        if ( c & 2 ) {
                            b1 |= bit;
                        }
                        if ( c & 4 ) {
                            b2 |= bit;
                        }
                        if ( c & 8 ) {
                            b3 |= bit;
                        }
                    }
                    dst0[x] = b0;
                    dst1[x] = b1;
                    dst2[x] = b2;
                    dst3[x] = b3;
                }
            }
            for ( p = 0; p < 4; p++ ) {
                port_out( 0x3C4, 2 );
                port_out( 0x3C5, 1u << p );
                for ( y = top; y <= bottom; y++ ) {
                    vid_put( vid_sel, (u32)y * 80 + (u32)bx0, planes[p] + (y - top) * nb, (u32)nb );
                }
            }
        }
        port_out( 0x3C4, 2 );
        port_out( 0x3C5, 0x0F );
    }
    dx0 = dy0 = 32767;
    dx1 = dy1 = -1;
}

static void read_fonts( void );
static void need_font8( void );

/* the hardware mode on, the palette, the whole picture */
static int hw_on( void )
{
    u32 bytes, pitch;
    int rc;

    rc = sys_video_set( (u32)gm->hw, &vid_sel, &bytes, &pitch );
    if ( !rc ) {
        hw_set = 0;
        return 0;
    }
    hw_set = 1;
    read_fonts();
    return 1;
}

void gfx_show( void )
{
    if ( gm == NULL ) {
        return;
    }
    if ( hw_on() ) {
        apply_palette();
        mark_all();
        gfx_flush();
    }
}

/* back to text for the IDE: the picture stays in memory */
void gfx_restore( void )
{
    if ( hw_set ) {
        sys_video_restore();
        hw_set = 0;
    }
}

static void free_pages( void )
{
    int i;

    for ( i = 0; i < 8; i++ ) {
        xfree( gpages[i] );
        gpages[i] = NULL;
    }
}

/* a run starts in SCREEN 0, whatever the last one ended in */
void gfx_reset( void )
{
    free_pages();
    gm = NULL;
    gfx_restore();
}

static void reset_view( void )
{
    vx1 = vy1 = 0;
    vx2 = gm->w - 1;
    vy2 = gm->h - 1;
    view_on = view_screen = 0;
    win_on = win_screen = 0;
    lpx = gm->w / 2;
    lpy = gm->h / 2;
}

/* SCREEN 0: back to the text screen */
static void text_mode( void )
{
    free_pages();
    gm = NULL;
    gfx_restore();
    con_set_geometry( 25, 80, 8 );
    con_show();
}

static int set_mode( int mode )
{
    int i;
    u32 size;

    for ( i = 0; i < (int)(sizeof( modes ) / sizeof( modes[0] )); i++ ) {
        if ( modes[i].mode == mode ) {
            break;
        }
    }
    if ( i == (int)(sizeof( modes ) / sizeof( modes[0] )) ) {
        return 0;
    }
    free_pages();
    gm = &modes[i];
    size = (u32)gm->w * (u32)gm->h;
    for ( i = 0; i < gm->pages; i++ ) {
        gpages[i] = (u8 *)try_alloc( size );
        if ( gpages[i] == NULL ) {
            text_mode();                        /* not left in the old mode's state */
            vm_error( E_OUT_OF_MEMORY );
        }
        mem_set( gpages[i], 0, size );
    }
    gapage = gvpage = 0;
    text_rows = gm->rows;
    text_font = gm->font;
    draw_fg = gm->colors - 1;
    if ( draw_fg > 15 ) {
        draw_fg = 15;
    }
    draw_bg = 0;
    reset_view();
    con_set_geometry( text_rows, gm->cols, 1 );
    if ( !hw_on() ) {
        text_mode();                            /* the adapter has no such mode */
        vm_error( E_ILLEGAL_CALL );
    }
    default_palette();
    apply_palette();
    mark_all();
    return 1;
}

/* SCREEN mode, colour switch, apage, vpage: given ones in the mask */
int gfx_set( int mode, int apage, int vpage, int sw, int mask )
{
    (void)sw;
    if ( mask & 1 ) {
        if ( mode < 0 || mode == 3 || mode == 4 || mode == 5 || mode == 6 || mode > 13 ) {
            return 0;
        }
        if ( mode == 0 ) {
            if ( gm != NULL ) {
                text_mode();
            }
        } else if ( gm == NULL || gm->mode != mode ) {
            if ( !set_mode( mode ) ) {
                return 0;
            }
        } else {
            gapage = gvpage = 0;                /* the same mode: pages reset only */
        }
    }
    if ( !(mask & 4) && !(mask & 8) ) {
        if ( mask & 1 ) {
            if ( gm == NULL ) {
                con_set_pages( 0, 0 );
            }
        }
        return 1;
    }
    if ( !(mask & 4) ) {
        apage = gm ? gapage : 0;
    }
    if ( !(mask & 8) ) {
        vpage = (mask & 4) ? apage : (gm ? gvpage : 0);
    }
    if ( gm == NULL ) {
        return con_set_pages( apage, vpage );
    }
    if ( apage < 0 || apage >= gm->pages || vpage < 0 || vpage >= gm->pages ) {
        return 0;
    }
    gapage = apage;
    if ( vpage != gvpage ) {
        gvpage = vpage;
        mark_all();
    }
    return 1;
}

void rtf_SCREENS( void )
{
    int mask = (int)POP_L();
    s32 vpage = POP_L(), apage = POP_L(), sw = POP_L(), mode = POP_L();

    if ( !gfx_set( (int)mode, (int)apage, (int)vpage, (int)sw, mask ) ) {
        vm_error( E_ILLEGAL_CALL );
    }
}

/* WIDTH in a graphics mode: the text rows the mode allows */
int gfx_width( int new_cols, int new_rows )
{
    if ( new_cols != gm->cols ) {
        return 0;
    }
    switch ( gm->mode ) {
    case 9:
    case 10:
        if ( new_rows != 25 && new_rows != 43 ) {
            return 0;
        }
        text_font = new_rows == 43 ? 8 : 14;
        need_font8();
        break;
    case 11:
    case 12:
        if ( new_rows != 30 && new_rows != 60 ) {
            return 0;
        }
        text_font = new_rows == 60 ? 8 : 16;
        need_font8();
        break;
    default:
        if ( new_rows != 25 ) {
            return 0;
        }
        return 1;
    }
    if ( new_rows != text_rows ) {
        int page;
        /* another number of lines clears the screen, as in QBasic */
        for ( page = 0; page < gm->pages; page++ ) {
            mem_set( gpages[page], 0, (u32)gm->w * (u32)gm->h );
        }
        mark_all();
    }
    text_rows = new_rows;
    con_set_geometry( text_rows, gm->cols, 1 );
    return 1;
}

int gfx_text_rows( void )
{
    return text_rows;
}

int gfx_text_cols( void )
{
    return gm ? gm->cols : 80;
}

void gfx_pcopy( int from, int to )
{
    if ( gm == NULL ) {
        if ( !con_copy_page( from, to ) ) {
            vm_error( E_ILLEGAL_CALL );
        }
        return;
    }
    if ( from < 0 || from >= gm->pages || to < 0 || to >= gm->pages ) {
        vm_error( E_ILLEGAL_CALL );
    }
    mem_cpy( gpages[to], gpages[from], (u32)gm->w * (u32)gm->h );
    if ( to == gvpage ) {
        mark_all();
    }
}

void rtf_PCOPY( void )
{
    s32 to = POP_L(), from = POP_L();

    gfx_pcopy( (int)from, (int)to );
}

/* ------------------------------------------------------------------ */
/* the fonts                                                           */
/* ------------------------------------------------------------------ */

#define BDA_POINTS      0x485
#define VEC( n )        ((u32)(n) * 4)

/* a font table that is what it says: the space has nothing in it */
static int font_sane( const u8 *table, int height )
{
    int i;

    for ( i = 0; i < height; i++ ) {
        if ( table[' ' * height + i] != 0 ) {
            return 0;
        }
    }
    return table['A' * height + height / 2] != 0;
}

/* After a mode set the BIOS leaves vector 43h at the font of the mode's
   character height, and the height at 0040:0085. */
static void read_fonts( void )
{
    u16 vec[2], points;
    u32 addr;
    int i;

    if ( !sys_peek( BDA_POINTS, &points, 2 ) || !sys_peek( VEC( 0x43 ), vec, 4 ) ) {
        return;
    }
    addr = ((u32)vec[1] << 4) + vec[0];
    if ( points == 8 && !have8 ) {
        if ( sys_peek( addr, font8, 8 * 256 ) && font_sane( font8, 8 ) ) {
            have8 = 1;
        }
    } else if ( points == 14 && !have14 ) {
        if ( sys_peek( addr, font14, 14 * 256 ) && font_sane( font14, 14 ) ) {
            have14 = 1;
        }
    } else if ( points == 16 && !have16 ) {
        if ( sys_peek( addr, font16, 16 * 256 ) && font_sane( font16, 16 ) ) {
            have16 = 1;
        }
    }
    if ( !have14 && have16 ) {
        /* some VGA BIOSes have no 8x14: the 8x16 without its top and bottom lines */
        for ( i = 0; i < 256; i++ ) {
            mem_cpy( font14 + i * 14, font16 + i * 16 + 1, 14 );
        }
        have14 = 1;
    }
}

/* WIDTH 80, 43 or 60: the 8x8 characters, when no mode set has left them
   to read.  The PC BIOS keeps 0-127 at F000:FA6E and vector 1Fh points
   at 128-255; else the 8x16 ones, two lines made one. */
#define ROM_FONT8       0xFFA6EUL

static void need_font8( void )
{
    u16 vec[2];
    int i, line;

    if ( have8 ) {
        return;
    }
    if ( sys_peek( ROM_FONT8, font8, 8 * 128 ) && sys_peek( VEC( 0x1F ), vec, 4 ) && (vec[0] | vec[1]) != 0
      && sys_peek( ((u32)vec[1] << 4) + vec[0], font8 + 8 * 128, 8 * 128 ) && font_sane( font8, 8 ) ) {
        have8 = 1;
        return;
    }
    if ( have16 ) {
        for ( i = 0; i < 256; i++ ) {
            for ( line = 0; line < 8; line++ ) {
                font8[i * 8 + line] = (u8)(font16[i * 16 + line * 2] | font16[i * 16 + line * 2 + 1]);
            }
        }
        have8 = 1;
    }
}

static const u8 *glyph( int ch, int height )
{
    static const u8 blank[16];

    switch ( height ) {
    case 8:     return have8 ? font8 + ch * 8 : blank;
    case 14:    return have14 ? font14 + ch * 14 : blank;
    }
    return have16 ? font16 + ch * 16 : blank;
}

/* ------------------------------------------------------------------ */
/* text in the picture                                                 */
/* ------------------------------------------------------------------ */

void gfx_text_cell( int row, int col, int ch, int attr )
{
    u8 *pic = gpages[gapage];
    const u8 *g = glyph( ch & 0xFF, text_font );
    int x0 = (col - 1) * 8, y0 = (row - 1) * text_font, x, y;
    u8 fgc = (u8)colour_of( attr );

    if ( y0 + text_font > gm->h || x0 + 8 > gm->w ) {
        return;
    }
    for ( y = 0; y < text_font; y++ ) {
        u8 bits = g[y];
        u8 *dst = pic + (u32)(y0 + y) * (u32)gm->w + (u32)x0;
        for ( x = 0; x < 8; x++ ) {
            dst[x] = (bits & (0x80 >> x)) ? fgc : 0;
        }
    }
    if ( gapage == gvpage ) {
        mark( x0, y0, x0 + 7, y0 + text_font - 1 );
    }
}

void gfx_scroll_text( int top, int bottom, int attr )
{
    u8 *pic = gpages[gapage];
    u32 w = (u32)gm->w;
    int y0 = (top - 1) * text_font, y1 = bottom * text_font;

    (void)attr;
    if ( y1 > gm->h ) {
        y1 = gm->h;
    }
    if ( y1 - y0 > text_font ) {
        mem_cpy( pic + (u32)y0 * w, pic + (u32)(y0 + text_font) * w, (u32)(y1 - y0 - text_font) * w );
    }
    mem_set( pic + (u32)(y1 - text_font) * w, 0, (u32)text_font * w );
    if ( gapage == gvpage ) {
        mark( 0, y0, gm->w - 1, y1 - 1 );
    }
}

void gfx_clear_text( int top, int bottom, int attr )
{
    u8 *pic = gpages[gapage];
    u32 w = (u32)gm->w;
    int y0 = (top - 1) * text_font, y1 = bottom * text_font;

    (void)attr;
    if ( y1 > gm->h ) {
        y1 = gm->h;
    }
    if ( y1 > y0 ) {
        mem_set( pic + (u32)y0 * w, 0, (u32)(y1 - y0) * w );
    }
    if ( gapage == gvpage ) {
        mark( 0, y0, gm->w - 1, y1 - 1 );
    }
}

/* ------------------------------------------------------------------ */
/* coordinates                                                         */
/* ------------------------------------------------------------------ */

static int round_px( double v )
{
    double r = fp_round( v );

    if ( r < -32768.0 ) {
        return -32768;
    }
    if ( r > 32767.0 ) {
        return 32767;
    }
    if ( DBL_BAD( r ) ) {
        return -32768;                          /* not a number: off the screen */
    }
    return (int)r;
}

/* a logical point to the viewport's own pixels (0,0 its corner), to the
   nearest, rounded from the coprocessor's 64 bits */
static void to_view_px( double x, double y, int *px, int *py )
{
    if ( win_on ) {
        *px = round_px( fp_scale_round( x, wx1, wfx ) );
        *py = round_px( win_screen ? fp_scale_round( y, wy1, wfy ) : fp_scale_round( wy2, y, wfy ) );
        return;
    }
    *px = round_px( x );
    *py = round_px( y );
}

static void from_view( double px, double py, double *x, double *y )
{
    if ( win_on ) {
        *x = fp_offset( wx1, px, wgx );
        *y = win_screen ? fp_offset( wy1, py, wgy ) : fp_offset( wy2, -py, wgy );
        return;
    }
    *x = px;
    *y = py;
}

/* a logical point to a pixel of the screen */
void gfx_to_screen( double x, double y, int *sx, int *sy )
{
    to_view_px( x, y, sx, sy );
    if ( (view_on && !view_screen) || win_on ) {
        *sx += vx1;
        *sy += vy1;
    }
}

void gfx_need( void )
{
    if ( gm == NULL ) {
        vm_error( E_ILLEGAL_CALL );
    }
}

/* the point a statement names, with STEP from the last point */
void gfx_point_arg( double x, double y, int step, double *lx, double *ly )
{
    if ( step ) {
        x += lpx;
        y += lpy;
    }
    *lx = x;
    *ly = y;
}

void gfx_set_last( double x, double y )
{
    lpx = x;
    lpy = y;
}

void gfx_last( double *x, double *y )
{
    *x = lpx;
    *y = lpy;
}

int gfx_fg( void )
{
    return draw_fg;
}

/* the colour given, or the foreground */
int gfx_colour_arg( s32 c, int given )
{
    return given ? colour_of( c ) : draw_fg;
}

/* ------------------------------------------------------------------ */
/* pixels                                                              */
/* ------------------------------------------------------------------ */

/* one pixel of the working page, inside the viewport */
void gfx_plot( int x, int y, int c )
{
    if ( x < vx1 || x > vx2 || y < vy1 || y > vy2 ) {
        return;
    }
    gpages[gapage][(u32)y * (u32)gm->w + (u32)x] = (u8)c;
    if ( gapage == gvpage ) {
        mark( x, y, x, y );
    }
}

int gfx_get_px( int x, int y )
{
    if ( x < vx1 || x > vx2 || y < vy1 || y > vy2 ) {
        return -1;
    }
    return gpages[gapage][(u32)y * (u32)gm->w + (u32)x];
}

/* a run of one row, for BF and PAINT: x0 <= x1 */
void gfx_hline( int x0, int x1, int y, int c )
{
    if ( y < vy1 || y > vy2 ) {
        return;
    }
    if ( x0 < vx1 ) {
        x0 = vx1;
    }
    if ( x1 > vx2 ) {
        x1 = vx2;
    }
    if ( x0 > x1 ) {
        return;
    }
    mem_set( gpages[gapage] + (u32)y * (u32)gm->w + (u32)x0, c, (u32)(x1 - x0 + 1) );
    if ( gapage == gvpage ) {
        mark( x0, y, x1, y );
    }
}

void gfx_view_box( int *x1, int *y1, int *x2, int *y2 )
{
    *x1 = vx1;
    *y1 = vy1;
    *x2 = vx2;
    *y2 = vy2;
}

/* PSET/PRESET (x, y), colour: flags 1 STEP, 2 colour, 4 PRESET */
void rtf_PSET( void )
{
    int flags = (int)POP_L();
    s32 c = POP_L();
    double y = POP_D(), x = POP_D();
    int sx, sy, colour;

    gfx_need();
    gfx_point_arg( x, y, flags & 1, &x, &y );
    if ( flags & 2 ) {
        colour = colour_of( c );
    } else {
        colour = (flags & 4) ? draw_bg : draw_fg;
    }
    gfx_to_screen( x, y, &sx, &sy );
    gfx_plot( sx, sy, colour );
    gfx_set_last( x, y );
}

/* a line, with a style mask (0 for solid) that goes round as it draws:
   the mask starts at the first point inside the viewport */
static u32 line_style;
static int style_on;

static void style_plot( int x, int y, int c )
{
    if ( x < vx1 || x > vx2 || y < vy1 || y > vy2 ) {
        return;
    }
    if ( style_on ) {
        int bit = (line_style & 0x8000) != 0;
        line_style = ((line_style << 1) | (bit ? 1 : 0)) & 0xFFFF;
        if ( !bit ) {
            return;
        }
    }
    gfx_plot( x, y, c );
}

/* The pixels QBasic picks: the line goes from its left end, and along
   the longer axis the other one is (short * i + long * 3/4) / long,
   rounded down - found by asking the real one about every slope. */
void gfx_line( int x0, int y0, int x1, int y1, int c )
{
    u32 dx, dy, major, minor, start, i, step;  /* 65535 * 65535 fits */
    int sy, t;

    if ( x1 < x0 ) {
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    dx = (u32)(x1 - x0);
    sy = y1 < y0 ? -1 : 1;
    dy = (u32)(y1 < y0 ? y0 - y1 : y1 - y0);
    if ( dx >= dy ) {
        major = dx;
        minor = dy;
    } else {
        major = dy;
        minor = dx;
    }
    start = major * 3 / 4;
    for ( i = 0; i <= major; i++ ) {
        step = major ? (minor * i + start) / major : 0;
        if ( dx >= dy ) {
            style_plot( x0 + (int)i, y0 + sy * (int)step, c );
        } else {
            style_plot( x0 + (int)step, y0 + sy * (int)i, c );
        }
    }
}

/* LINE [(x1,y1)]-(x2,y2), colour, B|BF, style: flags 1 STEP first,
   2 STEP second, 4 first given, 8 colour, 16 B, 32 BF, 64 style */
void rtf_LINE( void )
{
    int flags = (int)POP_L();
    s32 style = POP_L(), c = POP_L();
    double y2 = POP_D(), x2 = POP_D(), y1 = POP_D(), x1 = POP_D();
    int sx1, sy1, sx2, sy2, colour, t;

    gfx_need();
    if ( flags & 4 ) {
        gfx_point_arg( x1, y1, flags & 1, &x1, &y1 );
    } else {
        x1 = lpx;
        y1 = lpy;
    }
    if ( flags & 2 ) {
        x2 += x1;                       /* STEP from the line's own first point */
        y2 += y1;
    }
    colour = (flags & 8) ? colour_of( c ) : draw_fg;
    gfx_to_screen( x1, y1, &sx1, &sy1 );
    gfx_to_screen( x2, y2, &sx2, &sy2 );
    style_on = (flags & 64) != 0;
    line_style = (u32)style & 0xFFFF;
    if ( flags & 32 ) {
        if ( sx1 > sx2 ) {
            t = sx1; sx1 = sx2; sx2 = t;
        }
        if ( sy1 > sy2 ) {
            t = sy1; sy1 = sy2; sy2 = t;
        }
        for ( t = sy1; t <= sy2; t++ ) {
            gfx_hline( sx1, sx2, t, colour );
        }
    } else if ( flags & 16 ) {
        /* the second y's side, the first y's, then the two ends: the
           order a style mask goes round them in QBasic */
        gfx_line( sx1, sy2, sx2, sy2, colour );
        gfx_line( sx1, sy1, sx2, sy1, colour );
        gfx_line( sx1, sy1, sx1, sy2, colour );
        gfx_line( sx2, sy1, sx2, sy2, colour );
    } else {
        gfx_line( sx1, sy1, sx2, sy2, colour );
    }
    style_on = 0;
    gfx_set_last( x2, y2 );
}

/* VIEW [[SCREEN] (x1,y1)-(x2,y2) [, fill [, border]]]: flags 1 given,
   2 SCREEN, 4 fill, 8 border */
/* WINDOW's scale for the viewport there is now.  A viewport one pixel
   wide or high has none (and no division by 0). */
static void fit_window( void )
{
    wfx = vx2 > vx1 ? (double)(vx2 - vx1) / (wx2 - wx1) : 0;
    wfy = vy2 > vy1 ? (double)(vy2 - vy1) / (wy2 - wy1) : 0;
    wgx = vx2 > vx1 ? (wx2 - wx1) / (double)(vx2 - vx1) : 0;
    wgy = vy2 > vy1 ? (wy2 - wy1) / (double)(vy2 - vy1) : 0;
}

void rtf_VIEW( void )
{
    int flags = (int)POP_L(), t, y;
    int had_win = win_on, had_screen = win_screen;
    s32 border = POP_L(), fill = POP_L();
    double y2 = POP_D(), x2 = POP_D(), y1 = POP_D(), x1 = POP_D();
    int ix1, iy1, ix2, iy2;

    gfx_need();
    if ( !(flags & 1) ) {
        /* the whole screen again; a WINDOW stays, fitted to it (QBasic) */
        reset_view();
        win_on = had_win;
        win_screen = had_screen;
        if ( win_on ) {
            fit_window();
        }
        return;
    }
    ix1 = round_px( x1 );
    iy1 = round_px( y1 );
    ix2 = round_px( x2 );
    iy2 = round_px( y2 );
    if ( ix1 > ix2 ) {
        t = ix1; ix1 = ix2; ix2 = t;
    }
    if ( iy1 > iy2 ) {
        t = iy1; iy1 = iy2; iy2 = t;
    }
    if ( ix1 < 0 || iy1 < 0 || ix2 >= gm->w || iy2 >= gm->h ) {
        vm_error( E_ILLEGAL_CALL );
    }
    /* the border first, on the whole screen, one pixel outside */
    vx1 = 0;
    vy1 = 0;
    vx2 = gm->w - 1;
    vy2 = gm->h - 1;
    if ( flags & 8 ) {
        int bc = colour_of( border );
        gfx_line( ix1 - 1, iy1 - 1, ix2 + 1, iy1 - 1, bc );
        gfx_line( ix1 - 1, iy2 + 1, ix2 + 1, iy2 + 1, bc );
        gfx_line( ix1 - 1, iy1 - 1, ix1 - 1, iy2 + 1, bc );
        gfx_line( ix2 + 1, iy1 - 1, ix2 + 1, iy2 + 1, bc );
    }
    vx1 = ix1;
    vy1 = iy1;
    vx2 = ix2;
    vy2 = iy2;
    view_on = 1;
    view_screen = (flags & 2) != 0;
    if ( flags & 4 ) {
        int fc = colour_of( fill );
        for ( y = vy1; y <= vy2; y++ ) {
            gfx_hline( vx1, vx2, y, fc );
        }
    }
    /* a WINDOW stays, fitted to the new viewport (QBasic) */
    if ( win_on ) {
        fit_window();
        from_view( (vx2 - vx1) / 2.0, (vy2 - vy1) / 2.0, &lpx, &lpy );
    } else {
        lpx = (vx2 - vx1) / 2;
        lpy = (vy2 - vy1) / 2;
    }
}

/* WINDOW [[SCREEN] (x1,y1)-(x2,y2)]: flags 1 given, 2 SCREEN */
void rtf_WINDOW( void )
{
    int flags = (int)POP_L();
    double y2 = POP_D(), x2 = POP_D(), y1 = POP_D(), x1 = POP_D(), t;

    gfx_need();
    if ( !(flags & 1) ) {
        win_on = 0;
        return;
    }
    if ( x1 == x2 || y1 == y2 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    if ( x1 > x2 ) {
        t = x1; x1 = x2; x2 = t;
    }
    if ( y1 > y2 ) {
        t = y1; y1 = y2; y2 = t;
    }
    wx1 = x1;
    wy1 = y1;
    wx2 = x2;
    wy2 = y2;
    fit_window();
    win_on = 1;
    win_screen = (flags & 2) != 0;
    from_view( (vx2 - vx1) / 2.0, (vy2 - vy1) / 2.0, &lpx, &lpy );
}

/* PMAP(v, n): 0 and 1 logical to physical, 2 and 3 back */
void rtf_PMAP( void )
{
    s32 n = POP_L();
    double v = POP_D(), x, y;
    int ipx, ipy;

    gfx_need();
    switch ( n ) {
    case 0:
        to_view_px( v, 0, &ipx, &ipy );
        PUSH_S( (double)ipx );
        return;
    case 1:
        to_view_px( 0, v, &ipx, &ipy );
        PUSH_S( (double)ipy );
        return;
    case 2:
        from_view( v, 0, &x, &y );
        PUSH_S( x );
        return;
    case 3:
        from_view( 0, v, &x, &y );
        PUSH_S( y );
        return;
    }
    vm_error( E_ILLEGAL_CALL );
}

/* POINT(n): the last point, physical (0, 1) or logical (2, 3) */
void rtf_POINT1( void )
{
    s32 n = POP_L();
    int px, py;

    gfx_need();
    to_view_px( lpx, lpy, &px, &py );
    switch ( n ) {
    case 0: PUSH_S( (double)px ); return;
    case 1: PUSH_S( (double)py ); return;
    case 2: PUSH_S( lpx ); return;
    case 3: PUSH_S( lpy ); return;
    }
    vm_error( E_ILLEGAL_CALL );
}

/* POINT(x, y): the colour there, -1 outside the viewport */
void rtf_POINT2( void )
{
    double y = POP_D(), x = POP_D();
    int sx, sy;

    gfx_need();
    gfx_to_screen( x, y, &sx, &sy );
    PUSH_L( gfx_get_px( sx, sy ) );
}

/* CLS in a graphics mode: 1 the viewport, 0 (or nothing) all of it */
void gfx_cls( int how )
{
    int y;

    if ( how == 1 || (how == -1 && view_on) ) {
        for ( y = vy1; y <= vy2; y++ ) {
            gfx_hline( vx1, vx2, y, 0 );
        }
    } else if ( how == 0 || how == -1 ) {
        mem_set( gpages[gapage], 0, (u32)gm->w * (u32)gm->h );
        if ( gapage == gvpage ) {
            mark_all();
        }
    }
    if ( how != 2 ) {
        if ( view_on && !view_screen ) {
            from_view( (vx2 - vx1) / 2.0, (vy2 - vy1) / 2.0, &lpx, &lpy );
        } else {
            from_view( (vx1 + vx2) / 2.0, (vy1 + vy2) / 2.0, &lpx, &lpy );
        }
    }
}

/* OUT to the DAC's ports, as a program's own palette code does it: the
   hardware's entry, and the colour of every attribute that shows it */
void gfx_dac_write( int index, const u8 *c )
{
    u8 entry[4];
    int i;

    entry[0] = c[2];
    entry[1] = c[1];
    entry[2] = c[0];
    entry[3] = 0;
    sys_video_palette( index, 1, entry );
    if ( gm == NULL ) {
        return;
    }
    if ( gm->hw == 0x13 ) {
        if ( index < gm->colors ) {
            mem_cpy( rgb[index], c, 3 );
        }
        return;
    }
    for ( i = 0; i < 16 && i < gm->colors; i++ ) {
        if ( (gm->hw == 0x0E ? ac_cga[i] : ac_ega[i]) == index ) {
            mem_cpy( rgb[i], c, 3 );
        }
    }
}

void gfx_dac_read( int index, u8 *c )
{
    u8 entry[4];
    int i;

    if ( gm != NULL && gm->hw == 0x13 && index < gm->colors ) {
        mem_cpy( c, rgb[index], 3 );
        return;
    }
    if ( gm != NULL ) {
        for ( i = 0; i < 16 && i < gm->colors; i++ ) {
            if ( (gm->hw == 0x0E ? ac_cga[i] : ac_ega[i]) == index ) {
                mem_cpy( c, rgb[i], 3 );
                return;
            }
        }
    }
    if ( sys_video_palette_get( index, 1, entry ) ) {
        c[0] = entry[2];
        c[1] = entry[1];
        c[2] = entry[0];
    } else {
        c[0] = c[1] = c[2] = 0;
    }
}
