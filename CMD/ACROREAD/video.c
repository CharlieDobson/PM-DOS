/*
 * VIDEO.C - the graphics screen: finding a mode, and putting a canvas
 * on it.
 *
 * TWO KINDS OF SCREEN, both 640 by 480.
 *
 * With VBE 2.0 or later in the video BIOS, the mode with the most
 * colours that has a linear frame buffer: 32 or 24 bits a pixel, else
 * 16 or 15, else 256 colours with a palette of this program's own.
 * The canvas for these is three bytes a pixel, and a line of it is
 * turned into the mode's own pixels on its way to the frame buffer.
 *
 * Without - a VGA and nothing more, or a BIOS whose VBE is 1.2 and has
 * banks instead of a frame buffer - mode 12h, sixteen colours, which
 * every VGA has.  Sixteen colours is not enough for a page in colour,
 * but sixteen greys is enough for one in grey: the palette is set to a
 * grey scale, the canvas is one byte a pixel, and a pattern of two
 * neighbouring greys makes up the levels between.  Text that has been
 * drawn with coverage needs those greys far more than it needs red.
 *
 * Mode 12h is four planes of one bit each, 80 bytes a line, behind one
 * 64K window.  A plane is chosen by the sequencer's map mask, which is
 * a port, and a port from ring 3 is a trap into the kernel - so the
 * screen is written a band of lines at a time: four planes' worth of
 * bits made in memory, then four masks and four copies.
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
        line_need( 640 * 4 );
    } else {
        if ( sys_gfx_set( MODE_VGA16, &bytes, &pitch ) != 0 ) {
            return 0;
        }
        screen.kind = VS_VGA;
        screen.bits = 4;
        screen.canvas_bpp = 1;
        vga_greys();
        line_need( VGA_BAND * 80 * 4 );
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
/* a canvas onto the screen                                            */
/* ------------------------------------------------------------------ */

/* one line of colour, 640 pixels as blue, green, red, to the screen's
   own kind of pixel at "out": how many bytes that made */
static u32 pack_line( const u8 *bgr, u8 *out, int ypos )
{
    u32 pixel;
    int col, red, green, blue, spread;

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
        blue = bgr[0];
        green = bgr[1];
        red = bgr[2];
        spread = bayer[ypos & 3][col & 3];
        if ( red == green && green == blue ) {
            out[col] = (u8)(216 + (red * 39 * 16 + spread * 255) / (255 * 16));
        } else {
            out[col] = (u8)(((red * 5 * 16 + spread * 255) / (255 * 16)) * 36 +
                            ((green * 5 * 16 + spread * 255) / (255 * 16)) * 6 +
                            (blue * 5 * 16 + spread * 255) / (255 * 16));
        }
    }
    return 640;
}

/*
 * Lines "top" up to "bottom" of the screen, from the canvas: the
 * screen's column 0 is the canvas's column "from_x" and its line "top"
 * is the canvas's line "from_y", and where the canvas has nothing -
 * the page is narrower than the screen, or ends above its foot - there
 * is "back", a grey.
 */
void video_show( const CANVAS *cv, int from_x, int from_y, int top, int bottom, int back )
{
    static u8 row[640 * 3];
    const u8 *src;
    u8 *plane;
    int ypos, cy, col, first, last, level, band = 0, plane_no, bit;

    /* the columns of the screen the canvas covers */
    first = -from_x;
    last = cv->width - from_x;
    if ( first < 0 ) {
        first = 0;
    }
    if ( last > 640 ) {
        last = 640;
    }
    if ( screen.kind == VS_VGA ) {
        mem_set( line, 0, VGA_BAND * 80 * 4 );
    }
    for ( ypos = top; ypos < bottom; ypos++ ) {
        cy = from_y + (ypos - top);
        src = cy >= 0 && cy < cv->height && first < last
                  ? cv->pix + (u32)cy * cv->stride + (u32)(from_x + first) * (u32)cv->bpp : NULL;
        if ( screen.kind == VS_VBE ) {
            mem_set( row, back, sizeof( row ) );
            if ( src && cv->bpp == 3 ) {
                mem_cpy( row + first * 3, src, (u32)(last - first) * 3 );
            } else if ( src ) {
                for ( col = first; col < last; col++ ) {
                    row[col * 3] = row[col * 3 + 1] = row[col * 3 + 2] = src[col - first];
                }
            }
            gfx_put( (u32)ypos * vbe_pitch, line, pack_line( row, line, ypos ) );
            continue;
        }
        /* sixteen greys: each pixel's level, a bit into each plane */
        plane = line + (u32)band * 320;
        for ( col = 0; col < 640; col++ ) {
            level = back;
            if ( src && col >= first && col < last ) {
                level = cv->bpp == 1 ? src[col - first]
                                     : (src[(col - first) * 3 + 2] * 77 + src[(col - first) * 3 + 1] * 150 +
                                        src[(col - first) * 3] * 29) >> 8;
            }
            level = (level * 15 * 16 + bayer[ypos & 3][col & 3] * 255) / (255 * 16);
            bit = 0x80 >> (col & 7);
            if ( level & 1 ) {
                plane[col >> 3] |= (u8)bit;
            }
            if ( level & 2 ) {
                plane[80 + (col >> 3)] |= (u8)bit;
            }
            if ( level & 4 ) {
                plane[160 + (col >> 3)] |= (u8)bit;
            }
            if ( level & 8 ) {
                plane[240 + (col >> 3)] |= (u8)bit;
            }
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
            mem_set( line, 0, VGA_BAND * 80 * 4 );
            band = 0;
        }
    }
}
