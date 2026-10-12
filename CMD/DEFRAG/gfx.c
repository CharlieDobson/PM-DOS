/*
 * GFX.C - DEFRAG's screen in a graphics mode: 640x480 on a VGA (mode
 * 12h, thirty lines of the 8x16 font), 640x350 on an EGA (mode 10h,
 * twenty-five lines of the 8x14).  DOSSHELL's GFX.C by way of
 * SCANDISK's, cut down to the two planar modes and given DEFRAG's own
 * things to draw.
 *
 * THE SCREEN IS STILL CELLS.  SCR.C keeps eighty columns of them and
 * hands over each one that changed; gfx_cell() paints it into "pic",
 * 640 pixels across and a byte a pixel, and gfx_flush() sends the scan
 * lines that changed to the screen.  So every screen DEFRAG has is the
 * same screen in both modes, and what a graphics mode adds is in the
 * bits of a cell a text screen never sees (UI.H):
 *
 *   a block      a block of the map: a tile with a gap round it, so
 *                that the map is a grid and not a smear - white for
 *                space in use, yellow once it has been optimized, a
 *                faint pattern for space that is free
 *   the bar      the progress bar, moving a pixel at a time
 *   a button     the round kind, drawn round
 *   a line       along an edge of a cell: a box has one all the way
 *                round, where a text screen spends a row and two
 *                columns of characters on its frame
 *   pictures     a drive in the list of drives, the small box in the
 *                corner of a title
 *
 * MS-DOS's DEFRAG gets its look by loading shapes of its own into the
 * text screen's font.  This does the same drawing with pixels, which a
 * native program can reach and a font it cannot.
 *
 * THE KERNEL SETS THE MODE (21h/F1h AL=1Fh) and answers with a selector
 * for the video memory; VID_PUT in DOSINT.ASM writes through it.  The
 * four bit planes are written one at a time: the changed lines are
 * turned into planes in memory first, so a whole screen costs four
 * writes to the sequencer's map mask and not one for every byte - a
 * port write from ring 3 is a fault the kernel has to answer.
 *
 * THE FONTS ARE THE VIDEO BIOS'S, read out of the ROM with 21h/F1h
 * AL=20h.  A mode set leaves the font of that mode in interrupt vector
 * 43h and the height of its characters at 0040:0085, so mode 12h is how
 * the 8x16 font is found and mode 10h the 8x14.  NOT EVERY VGA BIOS HAS
 * AN 8x14 FONT (Cirrus Logic's have none, and point the vector at the
 * 8x16 in mode 10h), so a table is checked before it is believed -
 * FONT_SANE - and a font the BIOS has not got is made from the one it
 * has.
 *
 * THE POINTER IS NOT IN THE PICTURE.  It is laid over each scan line as
 * the line is copied out, so moving it redraws nothing.  The kernel's
 * mouse driver stays hidden and is only asked where the mouse is.
 */
#include "ui.h"

#define GW              640
#define MAX_H           480
#define LINE_BYTES      (GW / 8)

#define BDA_POINTS      0x485   /* a character's height, a word */
#define VEC( n )        ((u32)(n) * 4)

#define SEQ_INDEX       0x3C4
#define SEQ_DATA        0x3C5
#define SEQ_MAPMASK     2

#define OUTLINE         0

int gfx_rows = 30;
int gfx_cell_h = 16;

static int adapter = -1;
static u32 vid_sel, vid_bytes;
static u32 mode_now = 0xFFFF;   /* the mode sys_video_set last set */
static int gheight = MAX_H;     /* scan lines */
static int cell_h = 16;
static int grows = 30;

static u8 *pic;                 /* the picture */
static u8 *planes;              /* changed lines as four planes */
static u8  font14[14 * 256], font16[16 * 256];
static int have14, have16;
static int tried14, tried16;    /* the BIOS has been asked for that one */
static const u8 *font = font16;

/* a scan line's changed pixels, first and last: first > last for none */
static u16 dirty_lo[MAX_H], dirty_hi[MAX_H];

static int ptr_x, ptr_y, ptr_on;
static u32 c2p_tab[16];
static u8  blk[16][8];          /* the cell being painted */

static const u8 vga16[16][3] = {        /* red, green, blue: six bits each */
    {  0,  0,  0 }, {  0,  0, 42 }, {  0, 42,  0 }, {  0, 42, 42 },
    { 42,  0,  0 }, { 42,  0, 42 }, { 42, 21,  0 }, { 42, 42, 42 },
    { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
    { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 }
};

static const char *const pointer_shape[16] = {
    "k...............",
    "kk..............",
    "kWk.............",
    "kWWk............",
    "kWWWk...........",
    "kWWWWk..........",
    "kWWWWWk.........",
    "kWWWWWWk........",
    "kWWWWWWWk.......",
    "kWWWWWWWWk......",
    "kWWWWWkkkkk.....",
    "kWWkWWk.........",
    "kWk.kWWk........",
    "kk..kWWk........",
    "k....kWWk.......",
    ".....kkkk......."
};

/* a round button, sixteen pixels across: its ring, and the dot of the
   one that is chosen */
static const char *const radio_shape[12] = {
    "....#####.......",
    "..##.....##.....",
    ".#.........#....",
    ".#...ooo...#....",
    "#...ooooo...#...",
    "#...ooooo...#...",
    "#...ooooo...#...",
    "#...ooooo...#...",
    ".#...ooo...#....",
    ".#.........#....",
    "..##.....##.....",
    "....#####......."
};

/* the box in the corner of a title */
static const char *const close_shape[10] = {
    "kkkkkkkkkkkkkkkk",
    "kkkkkkkkkkkkkkkk",
    "kkkkkkkkkkkkkkkk",
    "kkkkkkkkkkkkkkkk",
    "kkWWWWWWWWWWWWkk",
    "kkWWWWWWWWWWWWkk",
    "kkkkkkkkkkkkkkkk",
    "kkkkkkkkkkkkkkkk",
    "kkkkkkkkkkkkkkkk",
    "kkkkkkkkkkkkkkkk"
};

/* a drive, twenty-four pixels across: a hard disk and a diskette drive */
static const char *const drive_shape[2][10] = {
    {
        "........................",
        "..####################..",
        "..#..................#..",
        "..#..................#..",
        "..#...............##.#..",
        "..#...............##.#..",
        "..#..................#..",
        "..####################..",
        "........................",
        "........................"
    },
    {
        "........................",
        "..####################..",
        "..#..................#..",
        "..#...############...#..",
        "..#..................#..",
        "..#.......####.......#..",
        "..#.......####.......#..",
        "..####################..",
        "........................",
        "........................"
    }
};

/* ------------------------------------------------------------------ */
/* what the machine has
                                                */
/* ------------------------------------------------------------------ */

int gfx_probe( void )
{
    BREGS regs;
    u8 probe;
    int code;

    if ( adapter >= 0 ) {
        return adapter;
    }
    adapter = GA_NONE;
    if ( !sys_peek( 0x449, &probe, 1 ) ) {
        return adapter;                 /* no way to read a font: text only */
    }
    /* 1Ah: a VGA BIOS answers with the display in BL; an EGA's does not
       know the call, and 12h/BL=10h is how one of those is found */
    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0x1A00;
    if ( !sys_bios( 0x10, &regs ) ) {
        adapter = GA_VGA;               /* nobody to ask: what a 386 has */
    } else if ( (regs.eax & 0xFF) == 0x1A ) {
        code = (int)(regs.ebx & 0xFF);
        if ( code == 7 || code == 8 ) {
            adapter = GA_VGA;
        } else if ( code == 4 || code == 5 ) {
            adapter = GA_EGA;
        }
    } else {
        mem_set( &regs, 0, sizeof( regs ) );
        regs.eax = 0x1200;
        regs.ebx = 0x10;
        if ( sys_bios( 0x10, &regs ) && (regs.ebx & 0xFF) != 0x10 ) {
            adapter = GA_EGA;
        }
    }
    return adapter;
}

/* ------------------------------------------------------------------ */
/* the fonts                                                           */
/* ------------------------------------------------------------------ */

static int set_mode( u32 mode )
{
    if ( mode == mode_now ) {
        return 1;
    }
    if ( sys_video_set( mode, &vid_sel, &vid_bytes ) != 0 ) {
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

/* a font two lines taller or shorter: the top and the bottom line */
static void font_stretch( const u8 *src, int src_h, u8 *dst, int dst_h )
{
    int ch, line, from;

    for ( ch = 0; ch < 256; ch++ ) {
        for ( line = 0; line < dst_h; line++ ) {
            from = src_h > dst_h ? line + 1 : line - 1;
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
   BIOS's other.  May leave another mode set. */
static int font_load( int height )
{
    if ( height == 14 ) {
        if ( !have14 ) {
            rom14();
            if ( !have14 && adapter == GA_VGA ) {
                rom16();
                if ( have16 ) {
                    font_stretch( font16, 16, font14, 14 );
                    have14 = 1;
                }
            }
        }
        return have14;
    }
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

/* ------------------------------------------------------------------ */
/* into and out of the mode                                            */
/* ------------------------------------------------------------------ */

static void clean_all( void )
{
    int line;

    for ( line = 0; line < MAX_H; line++ ) {
        dirty_lo[line] = 1;
        dirty_hi[line] = 0;
    }
}

int gfx_enter( void )
{
    int color, bit, height;

    if ( gfx_probe() == GA_NONE ) {
        return 0;
    }
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
    height = adapter == GA_VGA ? 16 : 14;
    /* the font first: finding one may set a mode of its own */
    if ( !font_load( height ) || !set_mode( adapter == GA_VGA ? 0x12 : 0x10 ) ) {
        gfx_leave();
        return 0;
    }
    font = height == 16 ? font16 : font14;
    cell_h = gfx_cell_h = height;
    gheight = adapter == GA_VGA ? 480 : 350;
    grows = gfx_rows = gheight / height;
    /* a mode set clears the screen, and so the picture starts the same */
    mem_set( pic, 0, (u32)GW * MAX_H );
    clean_all();
    ptr_on = 0;
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

static void paint_char( int ch, int fg, int bg, int under )
{
    int line, px, bits, arms = box_arms( ch ), mid = cell_h / 2;

    for ( line = 0; line < cell_h; line++ ) {
        if ( arms ) {
            for ( px = 0; px < 8; px++ ) {
                blk[line][px] = (u8)bg;
            }
            if ( (line <= mid && (arms & 1)) || (line >= mid && (arms & 2)) ) {
                blk[line][3] = (u8)fg;
            }
            if ( line == mid ) {
                for ( px = 0; px < 8; px++ ) {
                    if ( (px <= 3 && (arms & 4)) || (px >= 3 && (arms & 8)) ) {
                        blk[line][px] = (u8)fg;
                    }
                }
            }
            continue;
        }
        bits = font[ch * cell_h + line];
        if ( under && line == cell_h - 2 ) {
            bits = 0xFF;
        }
        for ( px = 0; px < 8; px++ ) {
            blk[line][px] = (u8)((bits & (0x80 >> px)) ? fg : bg);
        }
    }
}

/* a piece of the progress bar: "done" of its columns in one colour and
   the rest in the other, between two lines, with the screen's colour
   above and below them */
static void paint_bar( int data, int fg, int bg, CELL cell )
{
    int done = data & 0x0F, outside = (data >> 4) & 0x0F, line, px, color;

    for ( line = 0; line < cell_h; line++ ) {
        for ( px = 0; px < 8; px++ ) {
            if ( line < 2 || line > cell_h - 3 ) {
                color = outside;
            } else if ( line == 2 || line == cell_h - 3 || ((cell & CF_LEFT) && px == 0)
                        || ((cell & CF_RIGHT) && px == 7) ) {
                color = OUTLINE;
            } else {
                color = px < done ? fg : bg;
            }
            blk[line][px] = (u8)color;
        }
    }
}

/*
 * A block of the map: a tile seven pixels wide with a line of the
 * screen's colour above and below it and a column of it to the right,
 * so that neighbours do not run together.  Space in use is a solid
 * tile with a dot in its middle, as MS-DOS DEFRAG's is; free space is
 * every other pixel of the same tile.
 */
static void paint_block( int kind, int fg, int bg )
{
    int line, px, mid = cell_h / 2, on;

    for ( line = 0; line < cell_h; line++ ) {
        for ( px = 0; px < 8; px++ ) {
            on = px < 7 && line >= 1 && line < cell_h - 1;
            if ( on && kind == BK_UNUSED ) {
                on = ((px + line) & 1) == 0;
            } else if ( on && px >= 3 && px <= 4 && (line == mid - 1 || line == mid) ) {
                on = 0;                         /* the dot */
            }
            blk[line][px] = (u8)(on ? fg : bg);
        }
    }
}

/* one cell of a picture that is "wide" pixels across and "high" lines:
   '#' the cell's own colour, 'o' the same when "dot" is set, 'k' black,
   'W' white, anything else what is behind */
static void paint_shape( const char *const shape[], int high, int left, int dot, int fg, int bg )
{
    int line, px, row, first = (cell_h - high) / 2;
    char ch;

    for ( line = 0; line < cell_h; line++ ) {
        row = line - first;
        for ( px = 0; px < 8; px++ ) {
            ch = row < 0 || row >= high ? '.' : shape[row][left + px];
            blk[line][px] = (u8)(ch == '#' || (ch == 'o' && dot) ? fg : ch == 'k' ? 0
                                 : ch == 'W' ? 15 : bg);
        }
    }
}

/* a line along an edge of the cell, in its own colour */
static void paint_edges( CELL cell, int fg )
{
    int line, px;

    for ( px = 0; px < 8; px++ ) {
        if ( cell & CF_TOP ) {
            blk[0][px] = (u8)fg;
        }
        if ( cell & CF_BOTTOM ) {
            blk[cell_h - 1][px] = (u8)fg;
        }
    }
    for ( line = 0; line < cell_h; line++ ) {
        if ( cell & CF_LEFT ) {
            blk[line][0] = (u8)fg;
        }
        if ( cell & CF_RIGHT ) {
            blk[line][7] = (u8)fg;
        }
    }
}

void gfx_cell( int row, int col, CELL cell )
{
    int left = col * 8, top = row * cell_h, line;
    int ch = (int)(cell & 0xFF), fg = (int)((cell >> 8) & 0x0F), bg = (int)((cell >> 12) & 0x0F);
    int kind = (int)((cell & CF_PIC_MASK) >> CF_PIC_SHIFT);

    if ( pic == NULL || row < 0 || row >= grows || col < 0 || col >= GW / 8 ) {
        return;
    }
    switch ( kind ) {
    case PIC_BAR:
        paint_bar( ch, fg, bg, cell );
        break;
    case PIC_BLOCK:
        paint_block( ch, fg, bg );
        break;
    case PIC_RADIO:
        paint_shape( radio_shape, 12, (ch & 1) * 8, ch & 2, fg, bg );
        break;
    case PIC_CLOSE:
        paint_shape( close_shape, 10, (ch & 1) * 8, 0, fg, bg );
        break;
    case PIC_DRIVE:
        paint_shape( drive_shape[(ch >> 2) & 1], 10, (ch & 3) * 8, 0, fg, bg );
        break;
    default:
        paint_char( ch, fg, bg, (cell & CF_UNDER) != 0 );
        break;
    }
    if ( kind != PIC_BAR && (cell & (CF_TOP | CF_BOTTOM | CF_LEFT | CF_RIGHT)) ) {
        paint_edges( cell, fg );
    }
    for ( line = 0; line < cell_h; line++ ) {
        mem_cpy( pic + (u32)(top + line) * GW + left, blk[line], 8 );
    }
    mark( left, top, left + 7, top + cell_h - 1 );
}

/* ------------------------------------------------------------------ */
/* the pointer                                                         */
/* ------------------------------------------------------------------ */

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

/* a scan line as it goes to the screen: the picture's, with the
   pointer laid over the pixels from "lo" to "hi" */
static const u8 *compose( int line, int lo, int hi, u8 *buf )
{
    const char *shape;
    int px, at;

    if ( !ptr_on || line < ptr_y || line >= ptr_y + 16 || ptr_x > hi || ptr_x + 15 < lo ) {
        return pic + (u32)line * GW;
    }
    mem_cpy( buf + lo, pic + (u32)line * GW + lo, (u32)(hi - lo + 1) );
    shape = pointer_shape[line - ptr_y];
    for ( px = 0; px < 16; px++ ) {
        at = ptr_x + px;
        if ( at >= lo && at <= hi && shape[px] != '.' ) {
            buf[at] = (u8)(shape[px] == 'k' ? 0 : 15);
        }
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* the picture to the screen                                           */
/* ------------------------------------------------------------------ */

/* Lines "first" to "last", every one of them changed: bytes "blo" to
   "bhi" of each line are made into four planes in memory, and then
   each plane is written with the sequencer letting only that plane
   take the write. */
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
    int line = 0, first, lo, hi;

    if ( pic == NULL || mode_now == 0xFFFF ) {
        return;
    }
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
   pointer, which is what the screen was sent */
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
    for ( color = 0; color < 16; color++ ) {
        head[54 + color * 4] = (u8)(vga16[color][2] << 2 | vga16[color][2] >> 4);
        head[54 + color * 4 + 1] = (u8)(vga16[color][1] << 2 | vga16[color][1] >> 4);
        head[54 + color * 4 + 2] = (u8)(vga16[color][0] << 2 | vga16[color][0] >> 4);
    }
    rc = sys_write( handle, head, sizeof( head ), &done );
    for ( line = gheight - 1; line >= 0 && rc == 0; line-- ) {
        rc = sys_write( handle, compose( line, 0, GW - 1, buf ), GW, &done );
    }
    sys_close( handle );
    return rc == 0;
}
