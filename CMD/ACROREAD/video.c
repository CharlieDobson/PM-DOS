/*
 * VIDEO.C - the graphics screen: finding a mode, and putting lines of
 * pixels on it.
 *
 * TWO KINDS OF SCREEN, both 640 by 480.
 *
 * With VBE 2.0 or later in the video BIOS, the mode with the most
 * colours that has a linear frame buffer: 32 or 24 bits a pixel, else
 * 16 or 15, else 256 colours with a palette of this program's own.
 * A line for these is three bytes a pixel, and is turned into the
 * mode's own pixels on its way to the frame buffer.
 *
 * Without - a VGA and nothing more, or a BIOS whose VBE is 1.2 and has
 * banks instead of a frame buffer - mode 12h, sixteen colours, which
 * every VGA has.  Sixteen colours is not enough for a page in colour,
 * but sixteen greys is enough for one in grey: the palette is set to a
 * grey scale, a line is one byte a pixel, and a pattern of two
 * neighbouring greys makes up the levels between.  Text that has been
 * drawn with coverage needs those greys far more than it needs red.
 *
 * Mode 12h is four planes of one bit each, 80 bytes a line, behind one
 * 64K window.  A plane is chosen by the sequencer's map mask, which is
 * a port, and a port from ring 3 is a trap into the kernel - so the
 * screen is written a band of lines at a time: four planes' worth of
 * bits made in memory, then four masks and four copies.
 *
 * WHAT IS ON A LINE IS THE CALLER'S.  video_rows asks for each line as
 * it is about to go out (VIEW.C's compose), which is how the page, the
 * scroll bars, the bar at the foot and the mouse's pointer come to be
 * on one screen without any of them being drawn into another: moving
 * the pointer is the few lines under it asked for again.
 */
#include "gfx.h"
#include "view.h"

#define MODE_VGA16      0x12

VSCREEN screen;

static u8  *line;                   /* one scan line, or a band of plane lines */
static u32  line_cap;
static int  red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
static int  vbe_mode;               /* the one probe chose, 0 for none */
static int  vbe_bits;
static u32  vbe_pitch;

/* 4 by 4, 0 to 15: which of two neighbouring levels a pixel gets */
static const u8 bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 } };

/* For each of the pattern's sixteen places and each of 256 levels, the
   level a screen with fewer shows: of 16 (the greys), of 6 (a side of
   the colour cube) and of 40 (the cube's own greys).  Made once; a
   division a pixel is what they save. */
#define SHADES_16   0
#define SHADES_6    1
#define SHADES_40   2
static u8 *shades;

static void line_need( u32 bytes )
{
    if ( bytes > line_cap ) {
        if ( line ) {
            xfree( line );
        }
        line = (u8 *)xalloc( bytes );
        line_cap = bytes;
    }
}

static void shades_make( void )
{
    static const int tops[3] = { 15, 5, 39 };
    int kind, place, level;

    if ( shades ) {
        return;
    }
    shades = (u8 *)xalloc( 3 * 16 * 256 );
    for ( kind = 0; kind < 3; kind++ ) {
        for ( place = 0; place < 16; place++ ) {
            for ( level = 0; level < 256; level++ ) {
                shades[(kind * 16 + place) * 256 + level] =
                    (u8)((level * tops[kind] * 16 + place * 255) / (255 * 16));
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* finding a mode                                                      */
/* ------------------------------------------------------------------ */

/* Is there a VBE mode for this?  The bits a pixel of the best, or 0.
   SET ACROBITS=8 (or 15, 16, 24, 32) in the environment takes only a
   mode of that depth: for trying each kind of pixel on one card. */
int video_probe( void )
{
    static u8 info[VBE_INFO_LEN], mode_info[VBE_MODE_LEN];
    const u16 *list;
    const char *only = sys_getenv( "ACROBITS" );
    int version, count, index, bits, best = 0, rank, best_rank = 0, attr, ok, wanted = 0;

    if ( only ) {
        wanted = (int)dec_parse( only, &ok );
    }
    vbe_mode = 0;
    if ( !sys_vbe_info( info, &version, &count ) || version < 0x0200 ) {
        return 0;
    }
    list = (const u16 *)*(u32 *)(info + 0x0E);
    if ( list == NULL ) {
        return 0;
    }
    for ( index = 0; index < count && list[index] != 0xFFFF; index++ ) {
        if ( !sys_vbe_mode( list[index], mode_info ) ) {
            continue;
        }
        attr = *(u16 *)mode_info;
        bits = mode_info[0x19];
        /* there, a graphics mode, and with a linear frame buffer */
        if ( (attr & 0x91) != 0x91 || *(u16 *)(mode_info + 0x12) != 640 ||
             *(u16 *)(mode_info + 0x14) != 480 ) {
            continue;
        }
        if ( mode_info[0x1B] == 6 || (mode_info[0x1B] == 4 && bits > 8) ) {
            /* direct colour: 15 is sometimes called 16 with a five-bit green */
            if ( bits == 16 && mode_info[0x21] == 5 ) {
                bits = 15;
            }
            rank = bits == 32 ? 5 : (bits == 24 ? 4 : (bits == 16 ? 3 : (bits == 15 ? 2 : 0)));
        } else if ( mode_info[0x1B] == 4 && bits == 8 ) {
            rank = 1;
        } else {
            continue;
        }
        if ( wanted && bits != wanted ) {
            continue;
        }
        if ( rank > best_rank ) {
            best_rank = rank;
            best = bits;
            vbe_mode = list[index];
            vbe_bits = bits;
            red_size = mode_info[0x1F];
            red_pos = mode_info[0x20];
            green_size = mode_info[0x21];
            green_pos = mode_info[0x22];
            blue_size = mode_info[0x23];
            blue_pos = mode_info[0x24];
        }
    }
    return best;
}

static void vga_greys( void )
{
    /* the DAC entries mode 12h's sixteen colours point at */
    static const u8 dac_of[16] = { 0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F };
    int index, level;

    for ( index = 0; index < 16; index++ ) {
        level = index * 63 / 15;
        port_out( 0x3C8, dac_of[index] );
        port_out( 0x3C9, (unsigned)level );
        port_out( 0x3C9, (unsigned)level );
        port_out( 0x3C9, (unsigned)level );
    }
}

/* 256 colours: six levels each of red, green and blue, then greys */
static void cube_palette( void )
{
    static u8 entries[256 * 4];
    int index;

    for ( index = 0; index < 216; index++ ) {
        entries[index * 4] = (u8)((index % 6) * 63 / 5);
        entries[index * 4 + 1] = (u8)(((index / 6) % 6) * 63 / 5);
        entries[index * 4 + 2] = (u8)((index / 36) * 63 / 5);
    }
    for ( index = 216; index < 256; index++ ) {
        entries[index * 4] = entries[index * 4 + 1] = entries[index * 4 + 2] = (u8)((index - 216) * 63 / 39);
    }
    sys_gfx_palette( 0, 256, entries );
}

/* VS_VBE or VS_VGA: 1 if the screen is now that */
int video_set( int kind )
{
    u32 bytes = 0, pitch = 0;

    shades_make();
    if ( kind == VS_VBE ) {
        if ( vbe_mode == 0 || sys_gfx_set( vbe_mode, &bytes, &pitch ) != 0 || pitch == 0 ) {
            return 0;
        }
        vbe_pitch = pitch;
        screen.kind = VS_VBE;
        screen.bits = vbe_bits;
        screen.canvas_bpp = 3;
        if ( vbe_bits == 8 ) {
            cube_palette();
        }
        line_need( 640 * 4 + 640 * 3 );
    } else {
        if ( sys_gfx_set( MODE_VGA16, &bytes, &pitch ) != 0 ) {
            return 0;
        }
        screen.kind = VS_VGA;
        screen.bits = 4;
        screen.canvas_bpp = 1;
        vga_greys();
        line_need( VGA_BAND * 80 * 4 + 640 );
    }
    screen.width = 640;
    screen.height = 480;
    return 1;
}

void video_text( void )
{
    if ( screen.kind != VS_TEXT ) {
        sys_gfx_restore();
        screen.kind = VS_TEXT;
    }
}

/* ------------------------------------------------------------------ */
/* lines onto the screen                                               */
/* ------------------------------------------------------------------ */

/* one line of colour, 640 pixels as blue, green, red, to the screen's
   own kind of pixel at "out": how many bytes that made */
static u32 pack_line( const u8 *bgr, u8 *out, int ypos )
{
    const u8 *six, *forty;
    u32 pixel;
    int col, red, green, blue;

    switch ( screen.bits ) {
    case 24:
        if ( blue_pos == 0 && green_pos == 8 && red_pos == 16 ) {
            mem_cpy( out, bgr, 640 * 3 );
        } else {
            for ( col = 0; col < 640; col++, bgr += 3, out += 3 ) {
                pixel = ((u32)bgr[2] << red_pos) | ((u32)bgr[1] << green_pos) | ((u32)bgr[0] << blue_pos);
                out[0] = (u8)pixel;
                out[1] = (u8)(pixel >> 8);
                out[2] = (u8)(pixel >> 16);
            }
        }
        return 640 * 3;
    case 32:
        for ( col = 0; col < 640; col++, bgr += 3, out += 4 ) {
            *(u32 *)out = ((u32)bgr[2] << red_pos) | ((u32)bgr[1] << green_pos) | ((u32)bgr[0] << blue_pos);
        }
        return 640 * 4;
    case 15:
    case 16:
        for ( col = 0; col < 640; col++, bgr += 3, out += 2 ) {
            *(u16 *)out = (u16)((((u32)bgr[2] >> (8 - red_size)) << red_pos) |
                                (((u32)bgr[1] >> (8 - green_size)) << green_pos) |
                                (((u32)bgr[0] >> (8 - blue_size)) << blue_pos));
        }
        return 640 * 2;
    }
    /* 256 colours: the nearer of two cube levels, by the pattern */
    for ( col = 0; col < 640; col++, bgr += 3 ) {
        six = shades + (SHADES_6 * 16 + bayer[ypos & 3][col & 3]) * 256;
        blue = bgr[0];
        green = bgr[1];
        red = bgr[2];
        if ( red == green && green == blue ) {
            forty = six + (SHADES_40 - SHADES_6) * 16 * 256;
            out[col] = (u8)(216 + forty[red]);
        } else {
            out[col] = (u8)(six[red] * 36 + six[green] * 6 + six[blue]);
        }
    }
    return 640;
}

/*
 * Lines "top" up to "bottom" of the screen.  "compose" is asked for
 * each: 640 pixels into "row", a byte each of grey for the sixteen-
 * grey screen and three each - blue, green, red - for a VBE one
 * (screen.canvas_bpp says which).
 */
void video_rows( int top, int bottom, COMPOSE compose )
{
    const u8 *tab;
    u8 *row, *plane;
    int ypos, col, band = 0, plane_no, bit, level, bits0, bits1, bits2, bits3;

    if ( top < 0 ) {
        top = 0;
    }
    if ( bottom > 480 ) {
        bottom = 480;
    }
    if ( screen.kind == VS_VBE ) {
        row = line + 640 * 4;
        for ( ypos = top; ypos < bottom; ypos++ ) {
            compose( ypos, row );
            gfx_put( (u32)ypos * vbe_pitch, line, pack_line( row, line, ypos ) );
        }
        return;
    }
    if ( screen.kind != VS_VGA ) {
        return;
    }
    /* sixteen greys: each pixel's level, a bit into each plane */
    row = line + VGA_BAND * 80 * 4;
    for ( ypos = top; ypos < bottom; ypos++ ) {
        compose( ypos, row );
        plane = line + (u32)band * 320;
        for ( col = 0; col < 640; ) {
            bits0 = bits1 = bits2 = bits3 = 0;
            for ( bit = 0; bit < 8; bit++, col++ ) {
                tab = shades + (SHADES_16 * 16 + bayer[ypos & 3][col & 3]) * 256;
                level = tab[row[col]];
                bits0 = (bits0 << 1) | (level & 1);
                bits1 = (bits1 << 1) | ((level >> 1) & 1);
                bits2 = (bits2 << 1) | ((level >> 2) & 1);
                bits3 = (bits3 << 1) | (level >> 3);
            }
            plane[0] = (u8)bits0;
            plane[80] = (u8)bits1;
            plane[160] = (u8)bits2;
            plane[240] = (u8)bits3;
            plane++;
        }
        if ( ++band == VGA_BAND || ypos == bottom - 1 ) {
            /* the band, a plane at a time: a line's four planes lie
               together in "line", so each is copied line by line */
            for ( plane_no = 0; plane_no < 4; plane_no++ ) {
                port_out( 0x3C4, 2 );
                port_out( 0x3C5, 1U << plane_no );
                for ( col = 0; col < band; col++ ) {
                    gfx_put( (u32)(ypos - band + 1 + col) * 80, line + (u32)col * 320 + (u32)plane_no * 80, 80 );
                }
            }
            port_out( 0x3C5, 0x0F );
            band = 0;
        }
    }
}
