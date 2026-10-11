/*
 * GFX.C - a graphics screen: the modes, the fonts, the picture, the
 * pointer.
 *
 * THREE KINDS OF SCREEN, ONE PICTURE.  The shell draws cells; this file
 * paints each cell that changed into "pic", 640 pixels across and a
 * byte a pixel, whatever the hardware is - and gfx_flush() copies the
 * scan lines that changed to the screen in the form the mode wants:
 *
 *   EGA 640x350 and VGA 640x480, 16 colours (BIOS modes 10h and 12h):
 *   four bit planes behind one 64K window.  The changed lines are
 *   turned into planes in memory first and then written a plane at a
 *   time, so a whole screen costs four writes to the sequencer's map
 *   mask and not one for every byte - which matters here, because a
 *   port write from ring 3 is a fault the kernel has to answer.
 *
 *   VBE 640x480, 256 colours (mode 101h, or whichever the BIOS lists
 *   for that): a linear frame buffer, a byte a pixel as the picture
 *   is, copied a line at a time.  It needs VBE 2.0, because a banked
 *   mode would want a trip to the BIOS for every 64K drawn - the
 *   kernel will not set one.  The first sixteen colours are the VGA's;
 *   the rest are shades that title bars fade through (CF_GRAD).
 *
 * THE KERNEL SETS THE MODE (21h/F1h AL=1Fh) and answers with a
 * selector for the frame buffer, which is all a native program is
 * given of video memory; VID_PUT in DOSINT.ASM writes through it.
 *
 * THE FONTS ARE THE VIDEO BIOS'S, read out of the ROM with 21h/F1h
 * AL=20h, since no native program can ask the BIOS where they are:
 * INT 10h/1130h answers in ES:BP and the kernel's list of BIOS calls a
 * program may make carries no pointers back.  But a mode set leaves the
 * font of that mode in interrupt vector 43h and the height of its
 * characters at 0040:0085 - the BIOS's own graphics-mode text needs
 * them there - so mode 12h is how the 8x16 font is found and mode 10h
 * the 8x14.  The 8x8 font's first half is at F000:FA6E in every BIOS
 * since the PC's, and vector 1Fh has its second half.  A font is read
 * once; a mode that uses another mode's font (34 lines on a VGA is the
 * 8x14 font in mode 12h) costs one extra mode set, the first time.
 *
 * NOT EVERY VGA BIOS HAS AN 8x14 FONT.  Cirrus Logic's ROMs for the
 * GD5436, GD5446 and GD5480 hold the 8x8 and the 8x16 and nothing
 * between: in mode 10h the BIOS says 14 lines at 0040:0085 and points
 * vector 43h at the 8x16 table.  Read as 14 lines a character, that
 * table is every character sliced across two - a screen of dashes and
 * fragments.  So a table is checked before it is believed (FONT_SANE:
 * a space with nothing in it), and a VGA without the 8x14 font gets
 * one made from the 8x16 by dropping the top and the bottom line of
 * every character.
 *
 * THE POINTER AND THE CURSOR ARE NOT IN THE PICTURE.  They are laid
 * over each scan line as it is copied out, so moving the pointer is
 * two small rectangles copied again and nothing redrawn.  The kernel's
 * mouse driver could draw a pointer in modes 10h and 12h but not in a
 * VBE mode, so the shell draws its own in all three and leaves the
 * driver's hidden.
 */
#include "shell.h"

#define GW              640
#define MAX_H           480
#define LINE_BYTES      (GW / 8)
#define RAMP_FIRST      16      /* the first DAC entry of the shades */
#define RAMP_STEPS      30
#define RAMP_COLORS     8       /* backgrounds 0 to 7 have a ramp each */

#define BDA_POINTS      0x485   /* a character's height, a word */
#define ROM_FONT8       0xFFA6EUL
#define VEC( n )        ((u32)(n) * 4)

#define SEQ_INDEX       0x3C4
#define SEQ_DATA        0x3C5
#define SEQ_MAPMASK     2

int gfx_adapter = VK_TEXT;
int gfx_vbe;
int gfx_colors = 16;

static u32 vid_sel, vid_bytes, vid_pitch;
static u32 mode_now = 0xFFFF;   /* the mode sys_video_set last set */
static int planar;
static int gheight = MAX_H;     /* scan lines */
static int cell_h = 16;
static int grows;

static u8 *pic;                 /* the picture */
static u8 *planes;              /* changed lines as four planes */
static u8  font8[8 * 256], font14[14 * 256], font16[16 * 256];
static int have8, have14, have16;
static int tried14, tried16;    /* the BIOS has been asked for that one */
static const u8 *font = font16;

/* a scan line's changed pixels, first and last: first > last for none */
static u16 dirty_lo[MAX_H], dirty_hi[MAX_H];

static int ptr_x, ptr_y, ptr_on;
static int car_row, car_col, car_kind = CUR_HIDE, car_color;

static u8  dac[256][4];         /* blue, green, red, 0: six bits each */
static u32 c2p_tab[16];

static const u8 vga16[16][3] = {        /* red, green, blue */
    {  0,  0,  0 }, {  0,  0, 42 }, {  0, 42,  0 }, {  0, 42, 42 },
    { 42,  0,  0 }, { 42,  0, 42 }, { 42, 21,  0 }, { 42, 42, 42 },
    { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
    { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 }
};

/* ------------------------------------------------------------------ */
/* what the machine has                                                */
/* ------------------------------------------------------------------ */

/* 640x480, a byte a pixel, through a linear frame buffer */
static int vbe_mode_fits( int mode )
{
    static u8 info[256];

    if ( !sys_vbe_mode( mode, info ) ) {
        return 0;
    }
    return (*(u16 *)info & 0x91) == 0x91        /* there, graphics, linear */
           && *(u16 *)(info + 0x12) == 640 && *(u16 *)(info + 0x14) == 480
           && info[0x19] == 8 && info[0x1B] == 4 && *(u32 *)(info + 0x28) != 0;
}

void gfx_probe( void )
{
    static u16 modes[112];
    BREGS regs;
    u8 probe;
    int count, index, code;

    gfx_adapter = VK_TEXT;
    gfx_vbe = 0;
    if ( !sys_peek( 0x449, &probe, 1 ) ) {
        return;                         /* no way to read a font: text only */
    }
    /* 1Ah: a VGA BIOS answers with the display in BL; an EGA's does not
       know the call, and 12h/BL=10h is how one of those is found */
    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0x1A00;
    if ( !sys_bios( 0x10, &regs ) ) {
        gfx_adapter = VK_VGA;           /* nobody to ask: what a 386 has */
    } else if ( (regs.eax & 0xFF) == 0x1A ) {
        code = (int)(regs.ebx & 0xFF);
        if ( code == 7 || code == 8 ) {
            gfx_adapter = VK_VGA;
        } else if ( code == 4 || code == 5 ) {
            gfx_adapter = VK_EGA;
        }
    } else {
        mem_set( &regs, 0, sizeof( regs ) );
        regs.eax = 0x1200;
        regs.ebx = 0x10;
        if ( sys_bios( 0x10, &regs ) && (regs.ebx & 0xFF) != 0x10 ) {
            gfx_adapter = VK_EGA;
        }
    }
    if ( gfx_adapter != VK_VGA ) {
        return;
    }
    if ( sys_vbe_version( modes, 112, &count ) < 0x0200 ) {
        return;
    }
    if ( vbe_mode_fits( 0x101 ) ) {
        gfx_vbe = 0x101;
        return;
    }
    for ( index = 0; index < count; index++ ) {
        if ( modes[index] >= 0x100 && vbe_mode_fits( modes[index] ) ) {
            gfx_vbe = modes[index];
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the fonts                                                           */
/* ------------------------------------------------------------------ */

static int set_mode( u32 mode )
{
    if ( mode == mode_now ) {
        return 1;
    }
    if ( sys_video_set( mode, &vid_sel, &vid_bytes, &vid_pitch ) != 0 ) {
        mode_now = 0xFFFF;
        return 0;
    }
    mode_now = mode;
    return 1;
}

static int read_far( u32 farptr, u8 *dst, u32 len )
{
    u32 addr = ((farptr >> 16) << 4) + (farptr & 0xFFFF);

    if ( farptr == 0 || addr + len > 0x100000UL ) {
        return 0;
    }
    return sys_peek( addr, dst, len );
}

/* every line of one character, ORed together */
static int glyph_bits( const u8 *data, int height, int ch )
{
    int line, bits = 0;

    for ( line = 0; line < height; line++ ) {
        bits |= data[ch * height + line];
    }
    return bits;
}

/* A table of characters that many lines high has an empty space,
   something in its capital A and something in its second half.  A
   table of another height fails the first of those: its space is read
   from the middle of some other character. */
static int font_sane( const u8 *data, int height )
{
    return glyph_bits( data, height, ' ' ) == 0 && glyph_bits( data, height, 'A' ) != 0
           && glyph_bits( data, height, 0xDB ) != 0;
}

/* the table vector 43h points at, if that is a font of this height */
static int font_at_vector( int height, u8 *dst )
{
    u32 vec;

    return sys_peek( VEC( 0x43 ), &vec, 4 ) && read_far( vec, dst, 256u * height )
           && font_sane( dst, height );
}

/* the font the BIOS left in vector 43h for the mode that is set */
static int font_of_mode( int height, u8 *dst )
{
    u16 points;

    if ( !sys_peek( BDA_POINTS, &points, 2 ) || points != height ) {
        return 0;
    }
    return font_at_vector( height, dst );
}

static void font_stretch( const u8 *src, int src_h, u8 *dst, int dst_h )
{
    int ch, line, from;

    for ( ch = 0; ch < 256; ch++ ) {
        for ( line = 0; line < dst_h; line++ ) {
            from = line * src_h / dst_h;
            if ( src_h == 16 && dst_h == 14 ) {
                from = line + 1;                /* the top and bottom lines go */
            } else if ( src_h == 14 && dst_h == 16 ) {
                from = line - 1;
            }
            dst[ch * dst_h + line] = from < 0 || from >= src_h ? 0 : src[ch * src_h + from];
        }
    }
}

/* The BIOS's 8x14 font, by way of mode 10h, asked for once.  A VGA
   BIOS that has none leaves its 8x16 font in the vector there, and
   that is taken while the mode is set. */
static void rom14( void )
{
    if ( have14 || tried14 ) {
        return;
    }
    tried14 = 1;
    if ( !set_mode( 0x10 ) ) {
        return;
    }
    if ( font_of_mode( 14, font14 ) ) {
        have14 = 1;
    } else if ( !have16 && font_at_vector( 16, font16 ) ) {
        have16 = 1;
    }
}

/* the BIOS's 8x16 font, by way of mode 12h, asked for once */
static void rom16( void )
{
    if ( have16 || tried16 ) {
        return;
    }
    tried16 = 1;
    if ( set_mode( 0x12 ) && font_of_mode( 16, font16 ) ) {
        have16 = 1;
    }
}

/* 1 = that font is in memory, the BIOS's own or one made from the
   BIOS's nearest.  May leave another mode set. */
static int font_load( int height )
{
    u32 vec;

    switch ( height ) {
    case 14:
        if ( !have14 ) {
            rom14();
            if ( !have14 && gfx_adapter == VK_VGA ) {
                rom16();
                if ( have16 ) {
                    font_stretch( font16, 16, font14, 14 );
                    have14 = 1;
                }
            }
        }
        return have14;
    case 16:
        if ( !have16 ) {
            rom16();
            if ( !have16 ) {
                rom14();
                if ( !have16 && have14 ) {
                    font_stretch( font14, 14, font16, 16 );
                    have16 = 1;
                }
            }
        }
        return have16;
    }
    if ( have8 ) {
        return 1;
    }
    sys_peek( ROM_FONT8, font8, 8 * 128 );
    if ( sys_peek( VEC( 0x1F ), &vec, 4 ) ) {
        read_far( vec, font8 + 8 * 128, 8 * 128 );
    }
    if ( font_sane( font8, 8 ) ) {
        have8 = 1;
    } else if ( set_mode( 0x06 ) && font_of_mode( 8, font8 ) ) {
        have8 = 1;      /* a BIOS without the PC's table: mode 6 uses the 8x8 font */
    } else if ( font_load( gfx_adapter == VK_VGA ? 16 : 14 ) ) {
        if ( gfx_adapter == VK_VGA ) {
            font_stretch( font16, 16, font8, 8 );
        } else {
            font_stretch( font14, 14, font8, 8 );
        }
        have8 = 1;
    }
    return have8;
}

/* ------------------------------------------------------------------ */
/* colours                                                             */
/* ------------------------------------------------------------------ */

/* A ramp for each of the eight plain colours: darker than the colour
   at the left of the screen, lighter at the right. */
static void build_dac( void )
{
    int color, step, part, dark, light, base;

    mem_set( dac, 0, sizeof( dac ) );
    for ( color = 0; color < 16; color++ ) {
        dac[color][0] = vga16[color][2];
        dac[color][1] = vga16[color][1];
        dac[color][2] = vga16[color][0];
    }
    for ( color = 0; color < RAMP_COLORS; color++ ) {
        for ( step = 0; step < RAMP_STEPS; step++ ) {
            for ( part = 0; part < 3; part++ ) {
                base = vga16[color][2 - part];
                dark = base * 5 / 8;
                light = base + (63 - base) * 3 / 8;
                dac[RAMP_FIRST + color * RAMP_STEPS + step][part] =
                    (u8)(dark + (light - dark) * step / (RAMP_STEPS - 1));
            }
        }
    }
}

static void mark( int x0, int y0, int x1, int y1 )
{
    int line;

    if ( x0 < 0 ) {
        x0 = 0;
    }
    if ( y0 < 0 ) {
        y0 = 0;
    }
    if ( x1 > GW - 1 ) {
        x1 = GW - 1;
    }
    if ( y1 > gheight - 1 ) {
        y1 = gheight - 1;
    }
    if ( x0 > x1 ) {
        return;
    }
    for ( line = y0; line <= y1; line++ ) {
        if ( dirty_lo[line] > dirty_hi[line] ) {
            dirty_lo[line] = (u16)x0;
            dirty_hi[line] = (u16)x1;
        } else {
            if ( x0 < dirty_lo[line] ) {
                dirty_lo[line] = (u16)x0;
            }
            if ( x1 > dirty_hi[line] ) {
                dirty_hi[line] = (u16)x1;
            }
        }
    }
}

static void clean_all( void )
{
    int line;

    for ( line = 0; line < MAX_H; line++ ) {
        dirty_lo[line] = 1;
        dirty_hi[line] = 0;
    }
}

/* A new scheme, or a new mode: the strip below the last line of text,
   which no cell covers, takes the colour of the screen behind the
   lists.  The DAC is set when the mode is. */
void gfx_scheme( int scheme )
{
    int top = grows * cell_h;

    (void)scheme;
    if ( pic == NULL || top >= gheight ) {
        return;
    }
    mem_set( pic + (u32)top * GW, pal.desk >> 4, (u32)(gheight - top) * GW );
    mark( 0, top, GW - 1, gheight - 1 );
}

/* ------------------------------------------------------------------ */
/* into and out of a mode                                              */
/* ------------------------------------------------------------------ */

int gfx_enter( const DMODE *mode )
{
    u32 vmode;
    int color, bit;

    if ( pic == NULL ) {
        pic = (u8 *)try_alloc( (u32)GW * MAX_H );
        planes = (u8 *)try_alloc( 4u * LINE_BYTES * MAX_H );
        if ( pic == NULL || planes == NULL ) {
            xfree( pic );
            xfree( planes );
            pic = planes = NULL;
            return 0;
        }
        for ( color = 0; color < 16; color++ ) {
            for ( bit = 0; bit < 4; bit++ ) {
                if ( color & (1 << bit) ) {
                    c2p_tab[color] |= 0x80UL << (8 * bit);
                }
            }
        }
    }
    vmode = mode->kind == VK_EGA ? 0x10 : mode->kind == VK_VGA ? 0x12
            : ((u32)gfx_vbe | VMODE_LFB);
    /* the font first: finding one may set a mode of its own */
    if ( !font_load( mode->font ) || !set_mode( vmode ) ) {
        gfx_leave();
        return 0;
    }
    font = mode->font == 8 ? font8 : mode->font == 14 ? font14 : font16;
    cell_h = mode->font;
    grows = mode->rows;
    planar = mode->kind != VK_VBE;
    gheight = mode->kind == VK_EGA ? 350 : 480;
    gfx_colors = planar ? 16 : 256;
    if ( !planar && vid_pitch < GW ) {
        gfx_leave();
        return 0;
    }
    build_dac();
    if ( !planar ) {
        sys_video_palette( 0, 256, &dac[0][0] );
    }
    /* a mode set clears the screen, and so the picture starts the same */
    mem_set( pic, 0, (u32)GW * MAX_H );
    clean_all();
    ptr_on = 0;
    car_kind = CUR_HIDE;
    return 1;
}

void gfx_leave( void )
{
    sys_video_restore();
    mode_now = 0xFFFF;
    ptr_on = 0;
}

/* ------------------------------------------------------------------ */
/* a cell into the picture                                             */
/* ------------------------------------------------------------------ */

/* the lines of the box-drawing characters: which of a cell's four
   arms each one has (1 up, 2 down, 4 left, 8 right).  They are drawn
   one pixel thick, where the font's are two and look like a text
   screen. */
static int box_arms( int ch )
{
    switch ( ch ) {
    case 0xB3: case 0xBA: return 1 | 2;
    case 0xC4: case 0xCD: return 4 | 8;
    case 0xDA: case 0xC9: return 2 | 8;
    case 0xBF: case 0xBB: return 2 | 4;
    case 0xC0: case 0xC8: return 1 | 8;
    case 0xD9: case 0xBC: return 1 | 4;
    case 0xC3: case 0xCC: return 1 | 2 | 8;
    case 0xB4: case 0xB9: return 1 | 2 | 4;
    case 0xC2: case 0xCB: return 2 | 4 | 8;
    case 0xC1: case 0xCA: return 1 | 4 | 8;
    case 0xC5: case 0xCE: return 1 | 2 | 4 | 8;
    }
    return 0;
}

static int icon_color( int ch, int fg, int bg )
{
    switch ( ch ) {
    case '#': return fg;
    case 'k': return 0;
    case 'b': return 1;
    case 'g': return 2;
    case 'c': return 3;
    case 'r': return 4;
    case 'm': return 5;
    case 'n': return 6;
    case 'w': return 7;
    case 'd': return 8;
    case 'B': return 9;
    case 'G': return 10;
    case 'C': return 11;
    case 'R': return 12;
    case 'M': return 13;
    case 'y': return 14;
    case 'W': return 15;
    }
    return bg;
}

/* which of an icon's sixteen lines a cell's line shows */
static int icon_line( int line )
{
    if ( cell_h == 16 ) {
        return line;
    }
    if ( cell_h == 14 ) {
        return line + 1;
    }
    return line * 2 + 1;
}

void gfx_cell( int row, int col, CELL cell )
{
    int x = col * 8, y = row * cell_h, line, px, bits, arms, mid = cell_h / 2;
    int ch = (int)(cell & 0xFF), fg = (int)((cell >> 8) & 0x0F), bg = (int)((cell >> 12) & 0x0F);
    int icon = (int)((cell >> 16) & 0xFF), slice = (int)((cell >> 24) & 3);
    int under = cell_h == 8 ? 7 : cell_h - 2, grad = 0;
    const char *src;
    u8 *dst, back[8];

    if ( pic == NULL || row < 0 || row >= grows || col < 0 || col >= GW / 8 ) {
        return;
    }
    if ( (cell & CF_GRAD) && gfx_colors == 256 && bg < RAMP_COLORS ) {
        grad = 1;
    }
    for ( px = 0; px < 8; px++ ) {
        back[px] = grad ? (u8)(RAMP_FIRST + bg * RAMP_STEPS + (x + px) * RAMP_STEPS / GW)
                   : (u8)bg;
    }
    arms = icon ? 0 : box_arms( ch );
    for ( line = 0; line < cell_h; line++ ) {
        dst = pic + (u32)(y + line) * GW + x;
        if ( icon && icon < IC_COUNT ) {
            src = icons[icon].rows[icon_line( line )] + slice * 8;
            for ( px = 0; px < 8; px++ ) {
                dst[px] = src[px] == '.' ? back[px] : (u8)icon_color( src[px], fg, bg );
            }
        } else if ( arms ) {
            for ( px = 0; px < 8; px++ ) {
                dst[px] = back[px];
            }
            if ( (line <= mid && (arms & 1)) || (line >= mid && (arms & 2)) ) {
                dst[3] = (u8)fg;
            }
            if ( line == mid ) {
                for ( px = 0; px < 8; px++ ) {
                    if ( (px <= 3 && (arms & 4)) || (px >= 3 && (arms & 8)) ) {
                        dst[px] = (u8)fg;
                    }
                }
            }
        } else {
            bits = font[ch * cell_h + line];
            if ( (cell & CF_UNDER) && line == under ) {
                bits = 0xFF;
            }
            for ( px = 0; px < 8; px++ ) {
                dst[px] = (bits & (0x80 >> px)) ? (u8)fg : back[px];
            }
        }
        /* an 8-line cell has no line to spare above its character */
        if ( ((cell & CF_TOP) && line == 0 && cell_h > 8)
             || ((cell & CF_BOTTOM) && line == cell_h - 1) ) {
            for ( px = 0; px < 8; px++ ) {
                dst[px] = (u8)fg;
            }
        }
        if ( cell & CF_LEFT ) {
            dst[0] = (u8)fg;
        }
        if ( cell & CF_RIGHT ) {
            dst[7] = (u8)fg;
        }
    }
    mark( x, y, x + 7, y + cell_h - 1 );
}

/* ------------------------------------------------------------------ */
/* the pointer and the cursor                                          */
/* ------------------------------------------------------------------ */

static void caret_rect( int *x0, int *y0, int *x1, int *y1 )
{
    *x0 = car_col * 8;
    *x1 = *x0 + 7;
    *y1 = car_row * cell_h + cell_h - 1;
    *y0 = car_kind == CUR_BLOCK ? car_row * cell_h : *y1 - 1;
}

void gfx_caret( int row, int col, int kind, int color )
{
    int x0, y0, x1, y1;

    if ( row < 0 || row >= grows || col < 0 || col >= GW / 8 ) {
        kind = CUR_HIDE;
    }
    if ( row == car_row && col == car_col && kind == car_kind && color == car_color ) {
        return;
    }
    if ( car_kind != CUR_HIDE ) {
        caret_rect( &x0, &y0, &x1, &y1 );
        mark( x0, y0, x1, y1 );
    }
    car_row = row;
    car_col = col;
    car_kind = kind;
    car_color = color;
    if ( car_kind != CUR_HIDE ) {
        caret_rect( &x0, &y0, &x1, &y1 );
        mark( x0, y0, x1, y1 );
    }
}

void gfx_pointer( int px, int py, int show )
{
    if ( ptr_on ) {
        if ( show && px == ptr_x && py == ptr_y ) {
            return;
        }
        mark( ptr_x, ptr_y, ptr_x + 15, ptr_y + 15 );
    }
    ptr_x = px;
    ptr_y = py;
    ptr_on = show;
    if ( ptr_on ) {
        mark( ptr_x, ptr_y, ptr_x + 15, ptr_y + 15 );
    }
}

/* a scan line as it goes to the screen: the picture's, with the cursor
   and then the pointer laid over the pixels from "lo" to "hi" */
static const u8 *compose( int line, int lo, int hi, u8 *buf )
{
    int x0, y0, x1, y1, px, at;
    const char *shape;
    int over = 0;

    if ( car_kind != CUR_HIDE ) {
        caret_rect( &x0, &y0, &x1, &y1 );
        if ( line >= y0 && line <= y1 && x0 <= hi && x1 >= lo ) {
            over |= 1;
        }
    }
    if ( ptr_on && line >= ptr_y && line < ptr_y + 16 && ptr_x <= hi && ptr_x + 15 >= lo ) {
        over |= 2;
    }
    if ( !over ) {
        return pic + (u32)line * GW;
    }
    mem_cpy( buf + lo, pic + (u32)line * GW + lo, (u32)(hi - lo + 1) );
    if ( over & 1 ) {
        for ( px = x0; px <= x1; px++ ) {
            if ( px >= lo && px <= hi ) {
                buf[px] = (u8)car_color;
            }
        }
    }
    if ( over & 2 ) {
        shape = pointer_shape[line - ptr_y];
        for ( px = 0; px < 16; px++ ) {
            at = ptr_x + px;
            if ( at >= lo && at <= hi && shape[px] != '.' ) {
                buf[at] = shape[px] == 'k' ? 0 : 15;
            }
        }
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* the picture to the screen                                           */
/* ------------------------------------------------------------------ */

/* Lines "first" to "last", every one of them changed, on a planar
   screen: bytes "blo" to "bhi" of each line are made into four planes
   in memory, and then each plane is written with the sequencer letting
   only that plane take the write. */
static void flush_band( int first, int last, int blo, int bhi )
{
    static u8 buf[GW];
    const u8 *src;
    u8 *out;
    u32 acc;
    int line, byte, plane;

    for ( line = first; line <= last; line++ ) {
        src = compose( line, blo * 8, bhi * 8 + 7, buf ) + blo * 8;
        out = planes + (u32)line * LINE_BYTES + blo;
        for ( byte = blo; byte <= bhi; byte++, src += 8, out++ ) {
            acc = c2p_tab[src[0] & 15] | (c2p_tab[src[1] & 15] >> 1)
                  | (c2p_tab[src[2] & 15] >> 2) | (c2p_tab[src[3] & 15] >> 3)
                  | (c2p_tab[src[4] & 15] >> 4) | (c2p_tab[src[5] & 15] >> 5)
                  | (c2p_tab[src[6] & 15] >> 6) | (c2p_tab[src[7] & 15] >> 7);
            out[0] = (u8)acc;
            out[LINE_BYTES * MAX_H] = (u8)(acc >> 8);
            out[2 * LINE_BYTES * MAX_H] = (u8)(acc >> 16);
            out[3 * LINE_BYTES * MAX_H] = (u8)(acc >> 24);
        }
    }
    for ( plane = 0; plane < 4; plane++ ) {
        port_out( SEQ_INDEX, SEQ_MAPMASK );
        port_out( SEQ_DATA, 1u << plane );
        for ( line = first; line <= last; line++ ) {
            vid_put( vid_sel, (u32)line * LINE_BYTES + blo,
                     planes + (u32)plane * LINE_BYTES * MAX_H + (u32)line * LINE_BYTES + blo,
                     (u32)(bhi - blo + 1) );
        }
    }
    port_out( SEQ_INDEX, SEQ_MAPMASK );
    port_out( SEQ_DATA, 0x0F );
}

void gfx_flush( void )
{
    static u8 buf[GW];
    int line, first, lo, hi;

    if ( pic == NULL || mode_now == 0xFFFF ) {
        return;
    }
    if ( !planar ) {
        for ( line = 0; line < gheight; line++ ) {
            lo = dirty_lo[line];
            hi = dirty_hi[line];
            if ( lo > hi ) {
                continue;
            }
            vid_put( vid_sel, (u32)line * vid_pitch + lo, compose( line, lo, hi, buf ) + lo,
                     (u32)(hi - lo + 1) );
            dirty_lo[line] = 1;
            dirty_hi[line] = 0;
        }
        return;
    }
    line = 0;
    while ( line < gheight ) {
        if ( dirty_lo[line] > dirty_hi[line] ) {
            line++;
            continue;
        }
        first = line;
        lo = dirty_lo[line];
        hi = dirty_hi[line];
        while ( line < gheight && dirty_lo[line] <= dirty_hi[line] ) {
            if ( dirty_lo[line] < lo ) {
                lo = dirty_lo[line];
            }
            if ( dirty_hi[line] > hi ) {
                hi = dirty_hi[line];
            }
            dirty_lo[line] = 1;
            dirty_hi[line] = 0;
            line++;
        }
        flush_band( first, line - 1, lo / 8, hi / 8 );
    }
}

/* ------------------------------------------------------------------ */
/* test mode: the picture as a file                                    */
/* ------------------------------------------------------------------ */

static void put32( u8 *at, u32 val )
{
    at[0] = (u8)val;
    at[1] = (u8)(val >> 8);
    at[2] = (u8)(val >> 16);
    at[3] = (u8)(val >> 24);
}

/* a Windows bitmap, a byte a pixel: what is in the picture, with the
   pointer and the cursor, which is what the screen was sent */
int gfx_snapshot( const char *path )
{
    static u8 head[54 + 1024], buf[GW];
    u32 handle, done;
    int line, color, rc;

    if ( pic == NULL || sys_create( path, &handle ) != 0 ) {
        return 0;
    }
    mem_set( head, 0, sizeof( head ) );
    head[0] = 'B';
    head[1] = 'M';
    put32( head + 2, sizeof( head ) + (u32)GW * gheight );
    put32( head + 10, sizeof( head ) );
    put32( head + 14, 40 );
    put32( head + 18, GW );
    put32( head + 22, (u32)gheight );
    head[26] = 1;
    head[28] = 8;
    put32( head + 34, (u32)GW * gheight );
    put32( head + 46, 256 );
    for ( color = 0; color < 256; color++ ) {
        head[54 + color * 4] = (u8)(dac[color][0] << 2 | dac[color][0] >> 4);
        head[54 + color * 4 + 1] = (u8)(dac[color][1] << 2 | dac[color][1] >> 4);
        head[54 + color * 4 + 2] = (u8)(dac[color][2] << 2 | dac[color][2] >> 4);
    }
    rc = sys_write( handle, head, sizeof( head ), &done );
    for ( line = gheight - 1; line >= 0 && rc == 0; line-- ) {
        rc = sys_write( handle, compose( line, 0, GW - 1, buf ), GW, &done );
    }
    sys_close( handle );
    return rc == 0;
}
