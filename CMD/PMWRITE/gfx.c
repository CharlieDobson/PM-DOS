/*
 * GFX.C - a graphics screen: the modes, the font, the picture, what is
 * drawn into it and how it reaches the hardware.  DOSSHELL's GFX.C,
 * made to fit any of four sizes and given a way to draw in pixels.
 *
 * ONE PICTURE, THREE KINDS OF HARDWARE.  Everything is drawn into
 * "pic", a byte a pixel and as large as the screen, and gfx_flush()
 * copies the scan lines that changed to the screen in the form the mode
 * wants:
 *
 *   A VBE LINEAR FRAME BUFFER, A BYTE A PIXEL: the picture's own form,
 *   copied a line at a time.  The DAC is loaded with the palette DISP.H
 *   promises: the VGA's sixteen, a 6x6x6 cube, 24 greys.
 *
 *   A VBE LINEAR FRAME BUFFER OF 15, 16, 24 OR 32 BITS A PIXEL: the
 *   picture is still a byte a pixel, and each changed run of a line is
 *   turned into the screen's pixels through a table of 256 of them as
 *   it is copied.  A card that lists 1024x768 in high colour only is
 *   served this way, and the rest of the program never knows.
 *
 *   THE VGA'S PLANES, 16 COLOURS (BIOS mode 12h, 640x480, and 6Ah,
 *   800x600, on the cards that have it): four bit planes behind one
 *   64K window.  The changed lines are turned into planes in memory
 *   first and then written a plane at a time, so a whole screen costs
 *   four writes to the sequencer's map mask and not one for every
 *   byte - which matters here, because a port write from ring 3 is a
 *   fault the kernel has to answer.  The DAC is left as the BIOS set
 *   it.
 *
 * A VBE mode needs VBE 2.0, because a banked mode would want a trip to
 * the BIOS for every 64K drawn - the kernel will not set one.  MODES
 * ARE FOUND BY WHAT THEY ARE, never by their numbers: the BIOS's list
 * is read and each mode asked its size, its depth, its memory model
 * and whether it has a frame buffer.  A mode on the list can still
 * refuse to be set, and then gfx_enter says so and SCR.C sets another.
 *
 * THE KERNEL SETS THE MODE (21h/F1h AL=1Fh) and answers with a
 * selector for the frame buffer, which is all a native program is
 * given of video memory; VID_PUT in DOSINT.ASM writes through it.
 *
 * THE FONT IS THE VIDEO BIOS'S 8x16, read out of the ROM with 21h/F1h
 * AL=20h, since no native program can ask the BIOS where it is.  A
 * mode set leaves the font of that mode in interrupt vector 43h and
 * the height of its characters at 0040:0085, so mode 12h is set once,
 * before the mode that is wanted, and the table read from there.  It
 * is checked before it is believed (ROMFONT_SANE): DOSSHELL met BIOSes
 * that point that vector at a table of another height.  The 8x16 is
 * the one font every VGA BIOS has, and the only one used here.
 *
 * THE POINTER, THE CURSOR AND THE CARET ARE NOT IN THE PICTURE.  They
 * are laid over each scan line as it is copied out, so moving the
 * pointer is two small rectangles copied again and nothing redrawn.
 * The kernel's mouse driver draws no pointer in a VBE mode, so this
 * draws its own in every mode and leaves the driver's hidden.
 *
 * THE PICTURE IS ALLOCATED FOR THE MODE and given back when the mode
 * is left: 1280x1024 is 1.3 megabytes, and a machine with 8 has better
 * uses for it while the screen is 640x480.
 */
#include "dispint.h"

#define BDA_COLS        0x44A   /* the screen's columns, a word */
#define BDA_POINTS      0x485   /* a character's height, a word */
#define VEC( n )        ((u32)(n) * 4)

#define SEQ_INDEX       0x3C4
#define SEQ_DATA        0x3C5
#define SEQ_MAPMASK     2

#define NO_MODE         0xFFFFFFFFUL

/* tables too big to be in the image: one block, allocated once */
typedef struct {
    u8  font[16 * 256];
    u32 rgb[256];               /* a pixel number's colour, 00RRGGBB */
    u32 packed[256];            /* ...and as the screen's own pixel */
    u8  opposite[256];          /* gfx_invert's pairs */
    u8  dac[256][4];            /* blue, green, red, 0: six bits each */
    u16 vmodes[112];
    u8  info[256];
} TABLES;

int gfx_width, gfx_height;
int gfx_colors = 16;

static TABLES *tab;
static u32 vid_sel, vid_bytes, vid_pitch;
static u32 mode_set = NO_MODE;  /* the mode sys_video_set last set */
static int video_touched;       /* a mode was set since the last restore */
static int planar;
static int px_bytes = 1;        /* bytes a pixel on the screen */
static int gw, gh;              /* the picture's size */
static int grows, gcols;        /* ...in cells */
static int lbytes;              /* a planar line's bytes */

static u8  *pic;                /* the picture */
static u8  *planes;             /* changed lines as four planes */
static u8  *linebuf;            /* a line with the pointer on it */
static u8  *hibuf;              /* a line as the screen's pixels */
static u16 *dirty_lo, *dirty_hi;        /* a line's changed pixels: lo > hi for none */
static int have_font, tried_font;

static int clip_x0, clip_y0, clip_x1, clip_y1;
static int cover_cut = 128;    /* 16 colours: the coverage that is ink */

static int ptr_x, ptr_y, ptr_shape, ptr_on;
static int cur_row, cur_col, cur_kind = CUR_HIDE, cur_color;
static int car_x, car_y, car_h, car_on;

static u32 c2p_tab[16];

static const u8 vga16[16][3] = {        /* red, green, blue: six bits */
    {  0,  0,  0 }, {  0,  0, 42 }, {  0, 42,  0 }, {  0, 42, 42 },
    { 42,  0,  0 }, { 42,  0, 42 }, { 42, 21,  0 }, { 42, 42, 42 },
    { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
    { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 }
};

/* the pointers: k the black of each, W its white, and where each one
   points.  The arrow is DOSSHELL's. */
static const char *const arrow_rows[16] = {
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

static const char *const ibeam_rows[16] = {
    "...WWWWWWWWW....",
    "...WkkkWkkkW....",
    "...WWWWkWWWW....",
    "......WkW.......",
    "......WkW.......",
    "......WkW.......",
    "......WkW.......",
    "......WkW.......",
    "......WkW.......",
    "......WkW.......",
    "......WkW.......",
    "......WkW.......",
    "......WkW.......",
    "...WWWWkWWWW....",
    "...WkkkWkkkW....",
    "...WWWWWWWWW...."
};

static const char *const wait_rows[16] = {
    ".kkkkkkkkkkkkk..",
    ".kWWWWWWWWWWWk..",
    ".kkkkkkkkkkkkk..",
    "..kWWWWWWWWWk...",
    "..kWWWWWWWWWk...",
    "...kWkWkWkWk....",
    "....kWkWkWk.....",
    ".....kWkWk......",
    ".....kWWWk......",
    "....kWWkWWk.....",
    "...kWWWkWWWk....",
    "..kWWWkWkWWWk...",
    "..kWWkWkWkWWk...",
    ".kkkkkkkkkkkkk..",
    ".kWWWWWWWWWWWk..",
    ".kkkkkkkkkkkkk.."
};

typedef struct {
    const char *const *rows;
    int hot_x, hot_y;
} POINTER;

static const POINTER pointers[3] = {
    { arrow_rows, 0, 0 },
    { ibeam_rows, 7, 8 },
    { wait_rows,  7, 7 }
};

/* ------------------------------------------------------------------ */
/* the palette                                                         */
/* ------------------------------------------------------------------ */

static int tables_ready( void )
{
    int color, level, red, green, blue;

    if ( tab != NULL ) {
        return 1;
    }
    tab = (TABLES *)try_alloc( sizeof( TABLES ) );
    if ( tab == NULL ) {
        return 0;
    }
    mem_set( tab, 0, sizeof( TABLES ) );
    for ( color = 0; color < 16; color++ ) {
        red = vga16[color][0] << 2 | vga16[color][0] >> 4;
        green = vga16[color][1] << 2 | vga16[color][1] >> 4;
        blue = vga16[color][2] << 2 | vga16[color][2] >> 4;
        tab->rgb[color] = ((u32)red << 16) | ((u32)green << 8) | (u32)blue;
        tab->opposite[color] = (u8)(color ^ 15);
    }
    for ( color = 16; color < 232; color++ ) {
        red = ((color - 16) / 36) * 51;
        green = (((color - 16) / 6) % 6) * 51;
        blue = ((color - 16) % 6) * 51;
        tab->rgb[color] = ((u32)red << 16) | ((u32)green << 8) | (u32)blue;
        tab->opposite[color] = (u8)(247 - color);       /* across the cube */
    }
    for ( color = 232; color < 256; color++ ) {
        level = 8 + 10 * (color - 232);
        tab->rgb[color] = ((u32)level << 16) | ((u32)level << 8) | (u32)level;
        tab->opposite[color] = (u8)(487 - color);       /* the ramp's other end */
    }
    for ( color = 0; color < 256; color++ ) {
        tab->dac[color][0] = (u8)((tab->rgb[color] & 0xFF) >> 2);
        tab->dac[color][1] = (u8)(((tab->rgb[color] >> 8) & 0xFF) >> 2);
        tab->dac[color][2] = (u8)(((tab->rgb[color] >> 16) & 0xFF) >> 2);
        tab->dac[color][3] = 0;
    }
    for ( color = 0; color < 16; color++ ) {
        for ( level = 0; level < 4; level++ ) {
            if ( color & (1 << level) ) {
                c2p_tab[color] |= 0x80UL << (8 * level);
            }
        }
    }
    return 1;
}

static u32 sq( int val )
{
    return (u32)(val * val);
}

static u32 distance( u32 rgb, int red, int green, int blue )
{
    return sq( (int)((rgb >> 16) & 0xFF) - red ) + sq( (int)((rgb >> 8) & 0xFF) - green )
           + sq( (int)(rgb & 0xFF) - blue );
}

/* The nearest of the sixteen, and on a 256-colour screen the nearer
   still of the cube's and the grey ramp's.  One of the sixteen wins a
   tie, so that black, white and the VGA's own colours are themselves. */
int gfx_rgb( u32 rgb )
{
    int red = (int)((rgb >> 16) & 0xFF), green = (int)((rgb >> 8) & 0xFF), blue = (int)(rgb & 0xFF);
    int color, best = 0, index, level;
    u32 err, least = 0xFFFFFFFFUL;

    if ( tab == NULL ) {
        return 0;
    }
    for ( color = 0; color < 16; color++ ) {
        err = distance( tab->rgb[color], red, green, blue );
        if ( err < least ) {
            least = err;
            best = color;
        }
    }
    if ( gfx_colors != 256 || least == 0 ) {
        return best;
    }
    index = 16 + ((red + 25) / 51) * 36 + ((green + 25) / 51) * 6 + (blue + 25) / 51;
    err = distance( tab->rgb[index], red, green, blue );
    if ( err < least ) {
        least = err;
        best = index;
    }
    level = ((red + green + blue) / 3 - 8 + 5) / 10;
    if ( level < 0 ) {
        level = 0;
    }
    if ( level > 23 ) {
        level = 23;
    }
    err = distance( tab->rgb[232 + level], red, green, blue );
    if ( err < least ) {
        best = 232 + level;
    }
    return best;
}

u32 gfx_color_rgb( int color )
{
    return tab != NULL ? tab->rgb[color & 0xFF] : 0;
}

/* ------------------------------------------------------------------ */
/* what the machine has                                                */
/* ------------------------------------------------------------------ */

/* a number from the environment, or 0: PWVIDEO is the one kind of
   screen to be used, PWCUT a threshold for gfx_cover to be tried with */
static int env_number( const char *name )
{
    const char *text = sys_getenv( name );
    int ok;
    u32 val;

    if ( text == NULL ) {
        return 0;
    }
    val = dec_parse( text, &ok );
    return ok && val < 0x10000UL ? (int)val : 0;
}

/* a mode's depth as it is spoken of: 8, 15, 16, 24, 32 - or 4 for the
   planes, 0 for anything this cannot draw in */
static int depth_of( const u8 *info )
{
    int bits = info[0x19], model = info[0x1B];

    if ( model == 3 && bits == 4 ) {
        return 4;
    }
    if ( model == 4 && bits == 8 ) {
        return 8;
    }
    if ( model == 6 || (model == 4 && bits > 8) ) {
        /* direct colour: 15 is sometimes called 16 with a five-bit green */
        if ( bits == 16 && info[0x21] == 5 ) {
            return 15;
        }
        if ( bits == 15 || bits == 16 || bits == 24 || bits == 32 ) {
            return bits;
        }
    }
    return 0;
}

/* how good a depth is to have: a byte a pixel is the picture's own
   form, and after that the fewer bytes to push the better */
static int rank_of( int depth )
{
    switch ( depth ) {
    case 8:  return 5;
    case 16: return 4;
    case 15: return 3;
    case 32: return 2;
    case 24: return 1;
    }
    return 0;
}

static void add_mode( DISPMODE *mode, int backend, int depth, int width, int height, int vmode )
{
    mem_set( mode, 0, sizeof( *mode ) );
    mode->backend = (u8)backend;
    mode->bpp = (u8)depth;
    mode->width = (u16)width;
    mode->height = (u16)height;
    mode->cols = (u16)(width / CELL_W);
    mode->rows = (u16)(height / CELL_H);
    mode->colors = (u16)(backend == DB_PLANAR ? 16 : 256);
    mode->vmode = (u16)vmode;
    sfmt( mode->name, sizeof( mode->name ), "%d x %d, %d colors", width, height, mode->colors );
}

int gfx_probe( DISPMODE *list, int max )
{
    static const u16 sizes[4][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 } };
    BREGS regs;
    u8 probe;
    int count = 0, nvbe = 0, version, only, size, index, depth, rank, vga = 0;
    int best_rank[4], best_mode[4], best_depth[4], has_planar[4];
    const u8 *info;

    if ( !sys_peek( 0x449, &probe, 1 ) || !tables_ready() ) {
        return 0;                       /* no way to read a font: text only */
    }
    /* 1Ah: a VGA BIOS answers with the display in BL.  Nothing less
       than a VGA has a mode this program draws in. */
    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0x1A00;
    if ( !sys_bios( 0x10, &regs ) ) {
        vga = 1;                        /* nobody to ask: what a 386 has */
    } else if ( (regs.eax & 0xFF) == 0x1A && ((regs.ebx & 0xFF) == 7 || (regs.ebx & 0xFF) == 8) ) {
        vga = 1;
    }
    if ( !vga ) {
        return 0;
    }
    only = env_number( "PWVIDEO" );
    if ( env_number( "PWCUT" ) ) {
        gfx_cover_cut( env_number( "PWCUT" ) );
    }
    info = tab->info;
    for ( size = 0; size < 4; size++ ) {
        best_rank[size] = best_mode[size] = best_depth[size] = has_planar[size] = 0;
    }
    /* every mode the BIOS lists, asked what it is - once each: every
       question is a trip to the BIOS */
    version = sys_vbe_version( tab->vmodes, 112, &nvbe );
    for ( index = 0; index < nvbe; index++ ) {
        if ( tab->vmodes[index] < 0x100 || !sys_vbe_mode( tab->vmodes[index], tab->info ) ) {
            continue;
        }
        if ( (*(const u16 *)info & 0x11) != 0x11 ) {
            continue;                   /* not there, or text */
        }
        for ( size = 0; size < 4; size++ ) {
            if ( *(const u16 *)(info + 0x12) == sizes[size][0]
                 && *(const u16 *)(info + 0x14) == sizes[size][1] ) {
                break;
            }
        }
        if ( size == 4 ) {
            continue;                   /* another size */
        }
        depth = depth_of( info );
        if ( depth == 4 ) {
            has_planar[size] = 1;
            continue;
        }
        /* a frame buffer the kernel can hand over: VBE 2.0, the mode
           saying it has one, and an address for it */
        if ( version < 0x0200 || !(*(const u16 *)info & 0x80) || *(const u32 *)(info + 0x28) == 0 ) {
            continue;
        }
        rank = rank_of( depth );
        if ( only && depth != only ) {
            continue;
        }
        if ( rank > best_rank[size] ) {
            best_rank[size] = rank;
            best_mode[size] = tab->vmodes[index];
            best_depth[size] = depth;
        }
    }
    for ( size = 0; size < 4 && count < max; size++ ) {
        if ( best_rank[size] ) {
            add_mode( &list[count++], best_depth[size] == 8 ? DB_LFB8 : DB_LFBHI, best_depth[size],
                      sizes[size][0], sizes[size][1], best_mode[size] );
        } else if ( only == 0 || only == 4 ) {
            /* the planes.  Mode 12h is every VGA's.  Mode 6Ah is found
               out by setting it, and is offered only when the BIOS
               speaks of an 800x600 planar mode (VBE 1.2 is enough for
               that): a plain VGA is not shown a screen it has not got. */
            if ( size == 0 ) {
                add_mode( &list[count++], DB_PLANAR, 4, 640, 480, 0x12 );
            } else if ( size == 1 && has_planar[size] ) {
                add_mode( &list[count++], DB_PLANAR, 4, 800, 600, 0x6A );
            }
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* the font                                                            */
/* ------------------------------------------------------------------ */

static int set_mode( u32 mode )
{
    if ( mode == mode_set ) {
        return 1;
    }
    video_touched = 1;
    if ( sys_video_set( mode, &vid_sel, &vid_bytes, &vid_pitch ) != 0 ) {
        mode_set = NO_MODE;
        return 0;
    }
    mode_set = mode;
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
static int char_bits( const u8 *data, int ch )
{
    int line, bits = 0;

    for ( line = 0; line < 16; line++ ) {
        bits |= data[ch * 16 + line];
    }
    return bits;
}

/* A table of characters sixteen lines high has an empty space,
   something in its capital A and something in its second half.  A
   table of another height fails the first of those: its space is read
   from the middle of some other character. */
static int romfont_sane( const u8 *data )
{
    return char_bits( data, ' ' ) == 0 && char_bits( data, 'A' ) != 0 && char_bits( data, 0xDB ) != 0;
}

/* The BIOS's 8x16 font, by way of mode 12h, asked for once: 1 = it is
   in memory.  Leaves mode 12h set the first time. */
static int romfont_load( void )
{
    u32 vec;
    u16 points;

    if ( have_font || tried_font ) {
        return have_font;
    }
    tried_font = 1;
    if ( set_mode( 0x12 ) && sys_peek( BDA_POINTS, &points, 2 ) && points == 16
         && sys_peek( VEC( 0x43 ), &vec, 4 ) && read_far( vec, tab->font, 16 * 256 )
         && romfont_sane( tab->font ) ) {
        have_font = 1;
    }
    return have_font;
}

/* ------------------------------------------------------------------ */
/* what changed                                                        */
/* ------------------------------------------------------------------ */

static void mark( int x0, int y0, int x1, int y1 )
{
    int line;

    if ( pic == NULL ) {
        return;
    }
    if ( x0 < 0 ) {
        x0 = 0;
    }
    if ( y0 < 0 ) {
        y0 = 0;
    }
    if ( x1 > gw - 1 ) {
        x1 = gw - 1;
    }
    if ( y1 > gh - 1 ) {
        y1 = gh - 1;
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

void gfx_dirty( int x0, int y0, int x1, int y1 )
{
    mark( x0, y0, x1, y1 );
}

static void clean_all( void )
{
    int line;

    for ( line = 0; line < gh; line++ ) {
        dirty_lo[line] = 1;
        dirty_hi[line] = 0;
    }
}

/* the strip below the last line of cells, which no cell covers */
void gfx_strip( int color )
{
    int top = grows * CELL_H;

    if ( pic == NULL || top >= gh ) {
        return;
    }
    mem_set( pic + (u32)top * gw, color, (u32)(gh - top) * gw );
    mark( 0, top, gw - 1, gh - 1 );
}

/* ------------------------------------------------------------------ */
/* into and out of a mode                                              */
/* ------------------------------------------------------------------ */

static void free_picture( void )
{
    xfree( pic );
    xfree( planes );
    xfree( linebuf );
    xfree( hibuf );
    xfree( dirty_lo );
    xfree( dirty_hi );
    pic = planes = linebuf = hibuf = NULL;
    dirty_lo = dirty_hi = NULL;
    gfx_width = gfx_height = 0;
    gw = gh = grows = gcols = 0;
    ptr_on = 0;
    cur_kind = CUR_HIDE;
    car_on = 0;
}

/* one colour's bits into their place in a pixel: "size" bits of an
   eight-bit level, "pos" bits up */
static u32 field( u32 level, int size, int pos )
{
    if ( size < 1 || size > 8 || pos < 0 || pos + size > 32 ) {
        return 0;
    }
    return (level >> (8 - size)) << pos;
}

/* the 256 colours as this mode's pixels, from the mode's own account
   of where red, green and blue are */
static void pack_colors( const u8 *info, int depth )
{
    int rsize = info[0x1F], rpos = info[0x20], gsize = info[0x21], gpos = info[0x22];
    int bsize = info[0x23], bpos = info[0x24], color;

    if ( rsize < 1 || rsize > 8 || gsize < 1 || gsize > 8 || bsize < 1 || bsize > 8
         || rpos + rsize > 32 || gpos + gsize > 32 || bpos + bsize > 32 ) {
        /* a BIOS that does not say: the usual places */
        rsize = gsize = bsize = depth >= 24 ? 8 : 5;
        if ( depth == 16 ) {
            gsize = 6;
        }
        bpos = 0;
        gpos = bsize;
        rpos = bsize + gsize;
    }
    for ( color = 0; color < 256; color++ ) {
        tab->packed[color] = field( (tab->rgb[color] >> 16) & 0xFF, rsize, rpos )
                             | field( (tab->rgb[color] >> 8) & 0xFF, gsize, gpos )
                             | field( tab->rgb[color] & 0xFF, bsize, bpos );
    }
}

int gfx_enter( const DISPMODE *mode )
{
    u32 size = (u32)mode->width * mode->height;
    u16 cols = 0;

    free_picture();
    if ( !tables_ready() ) {
        return DE_NOMEM;
    }
    /* the font first: finding it sets a mode of its own */
    if ( !romfont_load() ) {
        return DE_NOFONT;
    }
    planar = mode->backend == DB_PLANAR;
    px_bytes = mode->bpp <= 8 ? 1 : (mode->bpp + 7) / 8;
    pic = (u8 *)try_alloc( size );
    linebuf = (u8 *)try_alloc( mode->width );
    dirty_lo = (u16 *)try_alloc( (u32)mode->height * sizeof( u16 ) );
    dirty_hi = (u16 *)try_alloc( (u32)mode->height * sizeof( u16 ) );
    if ( planar ) {
        planes = (u8 *)try_alloc( size / 2 );
    } else if ( px_bytes > 1 ) {
        hibuf = (u8 *)try_alloc( (u32)mode->width * 4 );
    }
    if ( pic == NULL || linebuf == NULL || dirty_lo == NULL || dirty_hi == NULL
         || (planar && planes == NULL) || (px_bytes > 1 && hibuf == NULL) ) {
        free_picture();
        return DE_NOMEM;
    }
    if ( !set_mode( planar ? (u32)mode->vmode : ((u32)mode->vmode | VMODE_LFB) ) ) {
        free_picture();
        return DE_REFUSED;
    }
    if ( planar ) {
        /* The kernel saw the BIOS take the mode's number.  A BIOS can
           take a number and set something else, so the screen's width
           is asked for as well - and the window has to hold a plane. */
        if ( !sys_peek( BDA_COLS, &cols, 2 ) || cols != mode->width / 8
             || vid_bytes < size / 8 ) {
            free_picture();
            mode_set = NO_MODE;
            return DE_REFUSED;
        }
    } else {
        if ( !sys_vbe_mode( mode->vmode, tab->info )
             || vid_pitch < (u32)mode->width * px_bytes
             || vid_bytes < vid_pitch * (mode->height - 1) + (u32)mode->width * px_bytes ) {
            free_picture();
            mode_set = NO_MODE;
            return DE_REFUSED;
        }
        if ( px_bytes == 1 ) {
            sys_video_palette( 0, 256, &tab->dac[0][0] );
        } else {
            pack_colors( tab->info, mode->bpp );
        }
    }
    gw = gfx_width = mode->width;
    gh = gfx_height = mode->height;
    gcols = mode->cols;
    grows = mode->rows;
    lbytes = gw / 8;
    gfx_colors = mode->colors;
    /* a mode set clears the screen, and so the picture starts the same */
    mem_set( pic, 0, size );
    clean_all();
    gfx_clip_all();
    return DE_NONE;
}

void gfx_leave( void )
{
    if ( video_touched ) {
        sys_video_restore();
        video_touched = 0;
    }
    mode_set = NO_MODE;
    free_picture();
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

void gfx_cell( int row, int col, CELL cell )
{
    int x = col * CELL_W, y = row * CELL_H, line, px, bits, arms, mid = CELL_H / 2;
    int ch = (int)(cell & 0xFF), fg = (int)((cell >> 8) & 0x0F), bg = (int)((cell >> 12) & 0x0F);
    int near_edge, far_edge;
    u8 *dst;

    if ( pic == NULL || row < 0 || row >= grows || col < 0 || col >= gcols ) {
        return;
    }
    arms = box_arms( ch );
    /* the edge lines: the text colour, or lit from the top left */
    near_edge = far_edge = fg;
    if ( (cell & CELLF_FRAME) == CELLF_FRAME ) {
        near_edge = far_edge = 0;
    } else if ( cell & CELLF_RAISE ) {
        near_edge = 15;
        far_edge = 8;
    } else if ( cell & CELLF_SINK ) {
        near_edge = 8;
        far_edge = 15;
    }
    for ( line = 0; line < CELL_H; line++ ) {
        dst = pic + (u32)(y + line) * gw + x;
        if ( arms ) {
            for ( px = 0; px < 8; px++ ) {
                dst[px] = (u8)bg;
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
            bits = tab->font[ch * 16 + line];
            if ( (cell & CELLF_UNDER) && line == CELL_H - 2 ) {
                bits = 0xFF;
            }
            for ( px = 0; px < 8; px++ ) {
                dst[px] = (bits & (0x80 >> px)) ? (u8)fg : (u8)bg;
            }
        }
        /* the bottom and the right over the top and the left, where
           they meet in a corner */
        if ( (cell & CELLF_TOP) && line == 0 ) {
            for ( px = 0; px < 8; px++ ) {
                dst[px] = (u8)near_edge;
            }
        }
        if ( cell & CELLF_LEFT ) {
            dst[0] = (u8)near_edge;
        }
        if ( cell & CELLF_RIGHT ) {
            dst[7] = (u8)far_edge;
        }
        if ( (cell & CELLF_BOTTOM) && line == CELL_H - 1 ) {
            for ( px = 0; px < 8; px++ ) {
                dst[px] = (u8)far_edge;
            }
        }
    }
    mark( x, y, x + 7, y + CELL_H - 1 );
}

/* ------------------------------------------------------------------ */
/* drawing in pixels                                                   */
/* ------------------------------------------------------------------ */

void gfx_clip( int x0, int y0, int x1, int y1 )
{
    clip_x0 = x0 < 0 ? 0 : x0;
    clip_y0 = y0 < 0 ? 0 : y0;
    clip_x1 = x1 > gw - 1 ? gw - 1 : x1;
    clip_y1 = y1 > gh - 1 ? gh - 1 : y1;
}

void gfx_clip_all( void )
{
    clip_x0 = clip_y0 = 0;
    clip_x1 = gw - 1;
    clip_y1 = gh - 1;
}

void gfx_clip_get( int *x0, int *y0, int *x1, int *y1 )
{
    *x0 = clip_x0;
    *y0 = clip_y0;
    *x1 = clip_x1;
    *y1 = clip_y1;
}

/* a rectangle cut to the clip rectangle: 0 when nothing of it is left */
static int cut( int *x0, int *y0, int *x1, int *y1 )
{
    if ( pic == NULL ) {
        return 0;
    }
    if ( *x0 < clip_x0 ) {
        *x0 = clip_x0;
    }
    if ( *y0 < clip_y0 ) {
        *y0 = clip_y0;
    }
    if ( *x1 > clip_x1 ) {
        *x1 = clip_x1;
    }
    if ( *y1 > clip_y1 ) {
        *y1 = clip_y1;
    }
    return *x0 <= *x1 && *y0 <= *y1;
}

void gfx_rect( int x, int y, int w, int h, int color )
{
    int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1, line;

    if ( w <= 0 || h <= 0 || !cut( &x0, &y0, &x1, &y1 ) ) {
        return;
    }
    for ( line = y0; line <= y1; line++ ) {
        mem_set( pic + (u32)line * gw + x0, color, (u32)(x1 - x0 + 1) );
    }
    mark( x0, y0, x1, y1 );
}

void gfx_hline( int x, int y, int len, int color )
{
    gfx_rect( x, y, len, 1, color );
}

void gfx_vline( int x, int y, int len, int color )
{
    gfx_rect( x, y, 1, len, color );
}

void gfx_outline( int x, int y, int w, int h, int color )
{
    gfx_rect( x, y, w, 1, color );
    gfx_rect( x, y + h - 1, w, 1, color );
    gfx_rect( x, y + 1, 1, h - 2, color );
    gfx_rect( x + w - 1, y + 1, 1, h - 2, color );
}

/* one ring of a frame: its top and left in one colour, its bottom and
   right in another */
static void ring( int x, int y, int w, int h, int top_left, int bottom_right )
{
    gfx_rect( x, y, w - 1, 1, top_left );
    gfx_rect( x, y + 1, 1, h - 2, top_left );
    gfx_rect( x, y + h - 1, w, 1, bottom_right );
    gfx_rect( x + w - 1, y, 1, h - 1, bottom_right );
}

void gfx_frame( int x, int y, int w, int h, int style )
{
    if ( w < 2 || h < 2 ) {
        return;
    }
    switch ( style ) {
    case FR_RAISED:
        ring( x, y, w, h, 15, 8 );
        break;
    case FR_SUNKEN:
        ring( x, y, w, h, 8, 15 );
        break;
    case FR_BUTTON:
        gfx_outline( x, y, w, h, 0 );
        ring( x + 1, y + 1, w - 2, h - 2, 15, 8 );
        if ( w > 6 && h > 6 ) {
            gfx_rect( x + 2, y + h - 3, w - 4, 1, 8 );
            gfx_rect( x + w - 3, y + 2, 1, h - 5, 8 );
        }
        break;
    case FR_PRESSED:
        gfx_outline( x, y, w, h, 0 );
        if ( w > 4 && h > 4 ) {
            gfx_rect( x + 1, y + 1, w - 2, 1, 8 );
            gfx_rect( x + 1, y + 2, 1, h - 3, 8 );
        }
        break;
    case FR_FIELD:
        ring( x, y, w, h, 8, 15 );
        if ( w > 4 && h > 4 ) {
            ring( x + 1, y + 1, w - 2, h - 2, 0, 7 );
        }
        break;
    }
}

void gfx_text( int x, int y, const char *text, int len, int fg, int bg )
{
    int x0 = x, y0 = y, x1 = x + len * 8 - 1, y1 = y + 15, line, pos, px, at, bits;
    u8 *dst;

    if ( len <= 0 || !cut( &x0, &y0, &x1, &y1 ) ) {
        return;
    }
    for ( line = y0; line <= y1; line++ ) {
        dst = pic + (u32)line * gw;
        for ( pos = (x0 - x) / 8; pos <= (x1 - x) / 8; pos++ ) {
            bits = tab->font[(u8)text[pos] * 16 + (line - y)];
            at = x + pos * 8;
            for ( px = 0; px < 8; px++, at++ ) {
                if ( at < x0 || at > x1 ) {
                    continue;
                }
                if ( bits & (0x80 >> px) ) {
                    dst[at] = (u8)fg;
                } else if ( bg >= 0 ) {
                    dst[at] = (u8)bg;
                }
            }
        }
    }
    mark( x0, y0, x1, y1 );
}

/* a letter of an icon as a colour: -1 for the pixel that is not there */
static int icon_color( int ch, int ink )
{
    switch ( ch ) {
    case '#': return ink;
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
    return -1;
}

static void dot( int x, int y, int color )
{
    if ( x >= clip_x0 && x <= clip_x1 && y >= clip_y0 && y <= clip_y1 ) {
        pic[(u32)y * gw + x] = (u8)color;
    }
}

void gfx_icon( int x, int y, const char *const *rows, int ink, int disabled )
{
    int line, px, color, pass;
    const char *row;

    if ( pic == NULL || rows == NULL ) {
        return;
    }
    /* embossed: the shape in white a pixel down and across, then in
       dark grey where it is */
    for ( pass = disabled ? 0 : 1; pass < 2; pass++ ) {
        for ( line = 0; line < 16; line++ ) {
            row = rows[line];
            for ( px = 0; px < 16 && row[px]; px++ ) {
                color = icon_color( row[px], ink );
                if ( color < 0 ) {
                    continue;
                }
                if ( !disabled ) {
                    dot( x + px, y + line, color );
                } else if ( row[px] != 'W' && row[px] != 'w' ) {
                    if ( pass == 0 ) {
                        dot( x + px + 1, y + line + 1, 15 );
                    } else {
                        dot( x + px, y + line, 8 );
                    }
                }
            }
        }
    }
    mark( x, y, x + 16, y + 16 );
}

void gfx_blit( int x, int y, int w, int h, const u8 *src, int stride )
{
    int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1, line;

    if ( w <= 0 || h <= 0 || src == NULL || !cut( &x0, &y0, &x1, &y1 ) ) {
        return;
    }
    for ( line = y0; line <= y1; line++ ) {
        mem_cpy( pic + (u32)line * gw + x0, src + (long)(line - y) * stride + (x0 - x),
                 (u32)(x1 - x0 + 1) );
    }
    mark( x0, y0, x1, y1 );
}

/* a colour "level" sixteenths of the way from one to another */
static int mix( u32 from, u32 to, int level )
{
    u32 red = (((from >> 16) & 0xFF) * (16 - level) + ((to >> 16) & 0xFF) * level + 8) / 16;
    u32 green = (((from >> 8) & 0xFF) * (16 - level) + ((to >> 8) & 0xFF) * level + 8) / 16;
    u32 blue = ((from & 0xFF) * (16 - level) + (to & 0xFF) * level + 8) / 16;

    return gfx_rgb( (red << 16) | (green << 8) | blue );
}

/* On a 16-colour screen a pixel is ink or it is not, and where the
   line is drawn decides what thin strokes do: at 128 a stem that falls
   across two pixels, half in each, is in neither and is gone; lower,
   it is in both and two pixels wide. */
void gfx_cover_cut( int level )
{
    cover_cut = level < 1 ? 1 : level > 255 ? 255 : level;
}

void gfx_cover( int x, int y, int w, int h, const u8 *cov, int stride, int fg, int bg )
{
    static int ramp_fg = -1, ramp_bg = -1, ramp_colors;
    static u8 ramp[17];
    int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1, line, at, level;
    const u8 *src;
    u8 *dst;

    if ( w <= 0 || h <= 0 || cov == NULL || !cut( &x0, &y0, &x1, &y1 ) ) {
        return;
    }
    fg &= 0xFF;
    if ( gfx_colors == 256 && bg >= 0
         && (fg != ramp_fg || bg != ramp_bg || ramp_colors != gfx_colors) ) {
        /* the sixteen steps from the background to the foreground,
           kept for the next run of the same text */
        for ( level = 0; level <= 16; level++ ) {
            ramp[level] = (u8)mix( tab->rgb[bg & 0xFF], tab->rgb[fg], level );
        }
        ramp[0] = (u8)bg;
        ramp[16] = (u8)fg;
        ramp_fg = fg;
        ramp_bg = bg;
        ramp_colors = gfx_colors;
    }
    for ( line = y0; line <= y1; line++ ) {
        src = cov + (long)(line - y) * stride + (x0 - x);
        dst = pic + (u32)line * gw + x0;
        for ( at = x0; at <= x1; at++, src++, dst++ ) {
            if ( gfx_colors != 256 ) {
                if ( *src >= cover_cut ) {
                    *dst = (u8)fg;
                } else if ( bg >= 0 ) {
                    *dst = (u8)bg;
                }
            } else if ( bg >= 0 ) {
                *dst = ramp[(*src * 16 + 127) / 255];
            } else {
                level = (*src * 16 + 127) / 255;
                if ( level == 16 ) {
                    *dst = (u8)fg;
                } else if ( level > 0 ) {
                    *dst = (u8)mix( tab->rgb[*dst], tab->rgb[fg], level );
                }
            }
        }
    }
    mark( x0, y0, x1, y1 );
}

void gfx_invert( int x, int y, int w, int h )
{
    int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1, line, at;
    u8 *dst;

    if ( w <= 0 || h <= 0 || !cut( &x0, &y0, &x1, &y1 ) ) {
        return;
    }
    for ( line = y0; line <= y1; line++ ) {
        dst = pic + (u32)line * gw + x0;
        for ( at = x0; at <= x1; at++, dst++ ) {
            *dst = gfx_colors == 256 ? tab->opposite[*dst] : (u8)((*dst & 15) ^ 15);
        }
    }
    mark( x0, y0, x1, y1 );
}

u8 *gfx_row( int y )
{
    return pic != NULL && y >= 0 && y < gh ? pic + (u32)y * gw : NULL;
}

/* ------------------------------------------------------------------ */
/* the pointer, the cursor and the caret                               */
/* ------------------------------------------------------------------ */

static void cursor_rect( int *x0, int *y0, int *x1, int *y1 )
{
    *x0 = cur_col * CELL_W;
    *x1 = *x0 + CELL_W - 1;
    *y1 = cur_row * CELL_H + CELL_H - 1;
    *y0 = cur_kind == CUR_BLOCK ? cur_row * CELL_H : *y1 - 1;
}

void gfx_cursor( int row, int col, int kind, int color )
{
    int x0, y0, x1, y1;

    if ( row < 0 || row >= grows || col < 0 || col >= gcols ) {
        kind = CUR_HIDE;
    }
    if ( row == cur_row && col == cur_col && kind == cur_kind && color == cur_color ) {
        return;
    }
    if ( cur_kind != CUR_HIDE ) {
        cursor_rect( &x0, &y0, &x1, &y1 );
        mark( x0, y0, x1, y1 );
    }
    cur_row = row;
    cur_col = col;
    cur_kind = kind;
    cur_color = color;
    if ( cur_kind != CUR_HIDE ) {
        cursor_rect( &x0, &y0, &x1, &y1 );
        mark( x0, y0, x1, y1 );
    }
}

void gfx_caret( int x, int y, int height, int show )
{
    if ( height <= 0 ) {
        show = 0;
    }
    if ( show == car_on && (!show || (x == car_x && y == car_y && height == car_h)) ) {
        return;
    }
    if ( car_on ) {
        mark( car_x, car_y, car_x + 1, car_y + car_h - 1 );
    }
    car_x = x;
    car_y = y;
    car_h = height;
    car_on = show;
    if ( car_on ) {
        mark( car_x, car_y, car_x + 1, car_y + car_h - 1 );
    }
}

void gfx_pointer( int x, int y, int shape, int show )
{
    const POINTER *ptr;

    if ( shape < 0 || shape > 2 ) {
        shape = MP_ARROW;
    }
    ptr = &pointers[shape];
    x -= ptr->hot_x;
    y -= ptr->hot_y;
    if ( ptr_on ) {
        if ( show && x == ptr_x && y == ptr_y && shape == ptr_shape ) {
            return;
        }
        mark( ptr_x, ptr_y, ptr_x + 15, ptr_y + 15 );
    }
    ptr_x = x;
    ptr_y = y;
    ptr_shape = shape;
    ptr_on = show;
    if ( ptr_on ) {
        mark( ptr_x, ptr_y, ptr_x + 15, ptr_y + 15 );
    }
}

/* a scan line as it goes to the screen: the picture's, with the cursor,
   the caret and then the pointer laid over the pixels from "lo" to
   "hi" */
static const u8 *compose( int line, int lo, int hi )
{
    int x0 = 0, y0, x1 = 0, y1, px, at;
    const char *shape;
    int over = 0;

    if ( cur_kind != CUR_HIDE ) {
        cursor_rect( &x0, &y0, &x1, &y1 );
        if ( line >= y0 && line <= y1 && x0 <= hi && x1 >= lo ) {
            over |= 1;
        }
    }
    if ( car_on && line >= car_y && line < car_y + car_h && car_x <= hi && car_x + 1 >= lo ) {
        over |= 2;
    }
    if ( ptr_on && line >= ptr_y && line < ptr_y + 16 && ptr_x <= hi && ptr_x + 15 >= lo ) {
        over |= 4;
    }
    if ( !over ) {
        return pic + (u32)line * gw;
    }
    mem_cpy( linebuf + lo, pic + (u32)line * gw + lo, (u32)(hi - lo + 1) );
    if ( over & 1 ) {
        for ( px = x0; px <= x1; px++ ) {
            if ( px >= lo && px <= hi ) {
                linebuf[px] = (u8)cur_color;
            }
        }
    }
    if ( over & 2 ) {
        for ( px = car_x; px <= car_x + 1; px++ ) {
            if ( px >= lo && px <= hi ) {
                linebuf[px] = gfx_colors == 256 ? tab->opposite[linebuf[px]]
                                                : (u8)((linebuf[px] & 15) ^ 15);
            }
        }
    }
    if ( over & 4 ) {
        shape = pointers[ptr_shape].rows[line - ptr_y];
        for ( px = 0; px < 16; px++ ) {
            at = ptr_x + px;
            if ( at >= lo && at <= hi && shape[px] != '.' ) {
                linebuf[at] = shape[px] == 'k' ? 0 : 15;
            }
        }
    }
    return linebuf;
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
    u32 psize = (u32)lbytes * gh;
    const u8 *src;
    u8 *out;
    u32 acc;
    int line, byte, plane;

    for ( line = first; line <= last; line++ ) {
        src = compose( line, blo * 8, bhi * 8 + 7 ) + blo * 8;
        out = planes + (u32)line * lbytes + blo;
        for ( byte = blo; byte <= bhi; byte++, src += 8, out++ ) {
            acc = c2p_tab[src[0] & 15] | (c2p_tab[src[1] & 15] >> 1)
                  | (c2p_tab[src[2] & 15] >> 2) | (c2p_tab[src[3] & 15] >> 3)
                  | (c2p_tab[src[4] & 15] >> 4) | (c2p_tab[src[5] & 15] >> 5)
                  | (c2p_tab[src[6] & 15] >> 6) | (c2p_tab[src[7] & 15] >> 7);
            out[0] = (u8)acc;
            out[psize] = (u8)(acc >> 8);
            out[2 * psize] = (u8)(acc >> 16);
            out[3 * psize] = (u8)(acc >> 24);
        }
    }
    for ( plane = 0; plane < 4; plane++ ) {
        port_out( SEQ_INDEX, SEQ_MAPMASK );
        port_out( SEQ_DATA, 1u << plane );
        for ( line = first; line <= last; line++ ) {
            vid_put( vid_sel, (u32)line * lbytes + blo,
                     planes + (u32)plane * psize + (u32)line * lbytes + blo,
                     (u32)(bhi - blo + 1) );
        }
    }
    port_out( SEQ_INDEX, SEQ_MAPMASK );
    port_out( SEQ_DATA, 0x0F );
}

/* pixels "lo" to "hi" of a line as the screen's own, two, three or
   four bytes each */
static void flush_high( int line, int lo, int hi )
{
    const u8 *src = compose( line, lo, hi ) + lo;
    u8 *out = hibuf;
    u32 pixel;
    int count = hi - lo + 1, at;

    switch ( px_bytes ) {
    case 2:
        for ( at = 0; at < count; at++ ) {
            *(u16 *)out = (u16)tab->packed[src[at]];
            out += 2;
        }
        break;
    case 3:
        for ( at = 0; at < count; at++ ) {
            pixel = tab->packed[src[at]];
            out[0] = (u8)pixel;
            out[1] = (u8)(pixel >> 8);
            out[2] = (u8)(pixel >> 16);
            out += 3;
        }
        break;
    default:
        for ( at = 0; at < count; at++ ) {
            *(u32 *)out = tab->packed[src[at]];
            out += 4;
        }
        break;
    }
    vid_put( vid_sel, (u32)line * vid_pitch + (u32)lo * px_bytes, hibuf, (u32)count * px_bytes );
}

void gfx_flush( void )
{
    int line, first, lo, hi;

    if ( pic == NULL || mode_set == NO_MODE ) {
        return;
    }
    if ( !planar ) {
        for ( line = 0; line < gh; line++ ) {
            lo = dirty_lo[line];
            hi = dirty_hi[line];
            if ( lo > hi ) {
                continue;
            }
            if ( px_bytes == 1 ) {
                vid_put( vid_sel, (u32)line * vid_pitch + lo, compose( line, lo, hi ) + lo,
                         (u32)(hi - lo + 1) );
            } else {
                flush_high( line, lo, hi );
            }
            dirty_lo[line] = 1;
            dirty_hi[line] = 0;
        }
        return;
    }
    line = 0;
    while ( line < gh ) {
        if ( dirty_lo[line] > dirty_hi[line] ) {
            line++;
            continue;
        }
        first = line;
        lo = dirty_lo[line];
        hi = dirty_hi[line];
        while ( line < gh && dirty_lo[line] <= dirty_hi[line] ) {
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
   pointer, the cursor and the caret, which is what the screen was
   sent.  A 16-colour screen shows the low four bits of a pixel, and so
   does this. */
int gfx_snapshot( const char *path )
{
    u8 *head;
    const u8 *src;
    u32 handle, done;
    int line, color, rc, at;

    if ( pic == NULL || (head = (u8 *)try_alloc( 54 + 1024 )) == NULL ) {
        return 0;
    }
    if ( sys_create( path, &handle ) != 0 ) {
        xfree( head );
        return 0;
    }
    mem_set( head, 0, 54 + 1024 );
    head[0] = 'B';
    head[1] = 'M';
    put32( head + 2, 54 + 1024 + (u32)gw * gh );
    put32( head + 10, 54 + 1024 );
    put32( head + 14, 40 );
    put32( head + 18, (u32)gw );
    put32( head + 22, (u32)gh );
    head[26] = 1;
    head[28] = 8;
    put32( head + 34, (u32)gw * gh );
    put32( head + 46, 256 );
    for ( color = 0; color < 256; color++ ) {
        put32( head + 54 + color * 4, tab->rgb[color] );        /* blue first, as the file wants */
    }
    rc = sys_write( handle, head, 54 + 1024, &done );
    xfree( head );
    for ( line = gh - 1; line >= 0 && rc == 0; line-- ) {
        src = compose( line, 0, gw - 1 );
        if ( gfx_colors != 256 ) {
            for ( at = 0; at < gw; at++ ) {
                linebuf[at] = src[at] & 15;
            }
            src = linebuf;
        }
        rc = sys_write( handle, src, (u32)gw, &done );
    }
    if ( sys_close( handle ) != 0 ) {
        rc = 1;
    }
    return rc == 0;
}
