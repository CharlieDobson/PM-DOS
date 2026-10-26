/*
 * GFX.C - XARC's graphics screen: VGA mode 12h, 640x480 in sixteen
 * colours, eighty columns of thirty lines of the 8x16 font.  What the DOS
 * build's VGAGFX.CPP draws, in the way SCANDISK's GFX.C draws it.
 *
 * THE SCREEN IS STILL CELLS.  SCR.C hands over each cell that changed and
 * gfx_cell() paints it into "pic", 640 pixels across and a byte a pixel;
 * gfx_flush() turns the scan lines that changed into the four bit planes
 * in memory and writes each plane once - a port write from ring 3 is a
 * fault the kernel has to answer, so the sequencer's map mask is set four
 * times a flush and not once a byte.  A cell is a character in its two
 * colours, or what the grid's extra byte says instead:
 *
 *   an icon      eight pixels by sixteen, ARCICON.C's art: '.' is the
 *                cell's background and a hex digit an EGA colour
 *   a bar slice  part of a progress bar: a box two pixels in from the
 *                cell's top and bottom in the foreground colour, filled
 *                as far as the work has got, eight steps to a cell
 *
 * A drop shadow is a cell recoloured dark grey on black (SCR.C), so a
 * box casts a shadow over the list instead of a hole in it.
 *
 * THE KERNEL SETS THE MODE (21h/F1h AL=1Fh) and answers with a selector
 * for the video memory; VID_PUT in DOSINT.ASM writes through it.  THE FONT
 * IS THE VIDEO BIOS'S, read with 21h/F1h AL=20h from where a mode set
 * leaves vector 43h - mode 12h's is the 8x16.  A table is checked before
 * it is believed (font_sane): many VGA BIOSes have no 8x14 font and leave
 * the 8x16 where it should be, and a BIOS that does the opposite gets its
 * 8x14 font stretched by a line top and bottom.
 *
 * THE POINTER IS NOT IN THE PICTURE.  It is laid over each scan line as
 * the line is copied out, so moving it redraws nothing.  The kernel's
 * mouse driver stays hidden and is only asked where the mouse is.
 *
 * Only a VGA: an EGA has no 640x480, and the DOS build said so too.
 */
#include "xarc.h"

void __cdecl vid_put( u32 selector, u32 offset, const void *src, u32 len );
void __cdecl port_out( unsigned port, unsigned value );

#define GW              640
#define GH              480
#define LINE_BYTES      (GW / 8)
#define CELL_H          GFX_CELL_H

#define BDA_POINTS      0x485
#define SEQ_INDEX       0x3C4
#define SEQ_DATA        0x3C5

static u32 vid_sel, vid_bytes;
static int mode_on;
static const char *error_text = "";

static u8 *pic;                 /* the picture, a byte a pixel */
static u8 *planes;              /* changed lines as four planes */
static u8  font16[16 * 256], font14[14 * 256];
static u16 dirty_lo[GH], dirty_hi[GH];
static u32 c2p_tab[16];
static u8  blk[CELL_H][8];

static int ptr_x, ptr_y, ptr_on;

/* the icons, compiled from their art the first time each is drawn */
static u8  icon_px[ICON_MAX][CELL_H][8];
static u8  icon_done[ICON_MAX];

#define CLEAR   0xFF

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

/* ------------------------------------------------------------------ */
/* the kernel's video calls                                            */
/* ------------------------------------------------------------------ */

/* 21h/F1h AL=20h: bytes of the first megabyte */
static int peek( u32 addr, void *buf, u32 len )
{
    REGS regs;

    xa_clear( &regs );
    regs.ebx = addr;
    regs.ecx = len;
    regs.edi = (u32)buf;
    if ( xa_dos( &regs, 0xF120 ) ) {
        memset( buf, 0, len );
        return 0;
    }
    return 1;
}

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u16 es, ds;
    u32 flags;
} BREGS;

/* 21h/F1h AL=21h: one of the BIOS calls that only report */
static int bios( int vector, BREGS *bregs )
{
    REGS regs;

    bregs->es = bregs->ds = 0;
    bregs->flags = 0;
    xa_clear( &regs );
    regs.edx = (u32)vector;
    regs.esi = (u32)bregs;
    return xa_dos( &regs, 0xF121 ) == 0;
}

/* VGA, or not?  1Ah answers on a VGA BIOS with the display in BL. */
static int is_vga( void )
{
    BREGS regs;
    u8 probe;
    int code;

    if ( !peek( 0x449, &probe, 1 ) ) {
        return 0;                       /* no way to read a font */
    }
    memset( &regs, 0, sizeof( regs ) );
    regs.eax = 0x1A00;
    if ( !bios( 0x10, &regs ) ) {
        return 1;                       /* nobody to ask: what a 386 has */
    }
    if ( (regs.eax & 0xFF) != 0x1A ) {
        return 0;                       /* an EGA's BIOS does not know it */
    }
    code = (int)(regs.ebx & 0xFF);
    return code == 7 || code == 8;
}

/* 21h/F1h AL=1Fh, function 2: a BIOS mode, and its video memory's selector */
static int set_mode( u32 mode )
{
    REGS regs;

    xa_clear( &regs );
    regs.ebx = mode;
    if ( xa_dos( &regs, 0x0002F11FUL ) ) {
        return 0;
    }
    vid_sel = regs.eax & 0xFFFF;
    vid_bytes = regs.ecx;
    return 1;
}

/* function 3: the text mode the first mode set found, with its lines */
static void restore_mode( void )
{
    REGS regs;

    xa_clear( &regs );
    xa_dos( &regs, 0x0003F11FUL );
}

/* ------------------------------------------------------------------ */
/* the font                                                            */
/* ------------------------------------------------------------------ */

static int glyph_bits( const u8 *data, int height, int ch )
{
    int line, bits = 0;

    for ( line = 0; line < height; line++ ) {
        bits |= data[ch * height + line];
    }
    return bits;
}

/* a table of characters this many lines high has an empty space, and
   something in its capital A and in its full block; one of another
   height fails the first of those */
static int font_sane( const u8 *data, int height )
{
    return glyph_bits( data, height, ' ' ) == 0 && glyph_bits( data, height, 'A' ) != 0
           && glyph_bits( data, height, 0xDB ) != 0;
}

/* the table vector 43h points at, when the BIOS says its characters are
   that many lines high and the table looks it */
static int font_of_mode( int height, u8 *dst )
{
    u16 points;
    u32 vec, addr;

    if ( !peek( BDA_POINTS, &points, 2 ) || points != height || !peek( 0x43 * 4, &vec, 4 ) ) {
        return 0;
    }
    addr = ((vec >> 16) << 4) + (vec & 0xFFFF);
    if ( vec == 0 || addr + 256u * height > 0x100000UL ) {
        return 0;
    }
    return peek( addr, dst, 256u * height ) && font_sane( dst, height );
}

/* ------------------------------------------------------------------ */
/* in and out                                                          */
/* ------------------------------------------------------------------ */

static void clean_all( void )
{
    int line;

    for ( line = 0; line < GH; line++ ) {
        dirty_lo[line] = 1;
        dirty_hi[line] = 0;
    }
}

int gfx_start( void )
{
    int color, bit, line, ch;

    error_text = "";
    if ( !is_vga() ) {
        error_text = "Graphics mode needs a VGA adapter.";
        return 0;
    }
    if ( pic == NULL ) {
        pic = (u8 *)malloc( (u32)GW * GH );
        planes = (u8 *)malloc( 4u * LINE_BYTES * GH );
        if ( pic == NULL || planes == NULL ) {
            free( pic );
            free( planes );
            pic = planes = NULL;
            error_text = "Not enough memory for the graphics screen.";
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
    if ( !set_mode( 0x12 ) ) {
        error_text = "The adapter refused 640x480 in 16 colours.";
        return 0;
    }
    mode_on = 1;
    if ( !font_of_mode( 16, font16 ) ) {
        /* a BIOS with the 8x14 font where the 8x16 should be: the 8x14
           is there in mode 10h, and a line top and bottom make it do */
        if ( set_mode( 0x10 ) && font_of_mode( 14, font14 ) ) {
            for ( ch = 0; ch < 256; ch++ ) {
                for ( line = 0; line < 16; line++ ) {
                    font16[ch * 16 + line] = line == 0 || line == 15 ? 0
                                             : font14[ch * 14 + line - 1];
                }
            }
        } else {
            restore_mode();
            mode_on = 0;
            error_text = "Could not read the adapter's character font.";
            return 0;
        }
        if ( !set_mode( 0x12 ) ) {
            restore_mode();
            mode_on = 0;
            error_text = "The adapter refused 640x480 in 16 colours.";
            return 0;
        }
    }
    /* a mode set clears the screen, and so the picture starts the same */
    memset( pic, 0, (u32)GW * GH );
    clean_all();
    ptr_on = 0;
    return 1;
}

void gfx_stop( void )
{
    if ( mode_on ) {
        restore_mode();
        mode_on = 0;
    }
    ptr_on = 0;
}

const char *gfx_error( void )
{
    return error_text;
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
    if ( y1 > GH - 1 ) {
        y1 = GH - 1;
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

static int art_color( char ch )
{
    if ( ch >= '0' && ch <= '9' ) {
        return ch - '0';
    }
    if ( ch >= 'A' && ch <= 'F' ) {
        return ch - 'A' + 10;
    }
    if ( ch >= 'a' && ch <= 'f' ) {
        return ch - 'a' + 10;
    }
    return CLEAR;
}

static int icon_compile( int id )
{
    const char *const *art;
    int nrows, line, px;

    if ( id < 0 || id >= ICON_MAX ) {
        return 0;
    }
    if ( icon_done[id] ) {
        return 1;
    }
    art = df_icon_art( id, &nrows );
    if ( art == NULL ) {
        return 0;
    }
    memset( icon_px[id], CLEAR, sizeof( icon_px[id] ) );
    for ( line = 0; line < CELL_H && line < nrows; line++ ) {
        for ( px = 0; px < 8 && art[line] && art[line][px]; px++ ) {
            icon_px[id][line][px] = (u8)art_color( art[line][px] );
        }
    }
    icon_done[id] = 1;
    return 1;
}

static void paint_char( int ch, int fg, int bg )
{
    int line, px, bits;

    for ( line = 0; line < CELL_H; line++ ) {
        bits = font16[ch * CELL_H + line];
        for ( px = 0; px < 8; px++ ) {
            blk[line][px] = (u8)((bits & (0x80 >> px)) ? fg : bg);
        }
    }
}

static void paint_icon( int id, int bg )
{
    int line, px;
    u8 dot;

    if ( !icon_compile( id ) ) {
        paint_char( ' ', 0, bg );       /* never registered: background */
        return;
    }
    for ( line = 0; line < CELL_H; line++ ) {
        for ( px = 0; px < 8; px++ ) {
            dot = icon_px[id][line][px];
            blk[line][px] = dot == CLEAR ? (u8)bg : dot;
        }
    }
}

/* VGAGFX.CPP's bar: the frame's top and bottom three pixels in from the
   cell's, the fill between them, and an upright at either end */
static void paint_bar( u8 extra, int fg, int bg )
{
    int fill = extra & 0x0F, top = 3, bot = CELL_H - 4, line, px, on;

    if ( fill > 8 ) {
        fill = 8;
    }
    for ( line = 0; line < CELL_H; line++ ) {
        for ( px = 0; px < 8; px++ ) {
            if ( line == top || line == bot ) {
                on = 1;
            } else if ( line > top && line < bot ) {
                on = px < fill || ((extra & EXTRA_BAR_LEFT) && px == 0)
                     || ((extra & EXTRA_BAR_RIGHT) && px == 7);
            } else {
                on = 0;
            }
            blk[line][px] = (u8)(on ? fg : bg);
        }
    }
}

void gfx_cell( int row, int col, CELL cell )
{
    int left = col * 8, top = row * CELL_H, line;
    int fg = CELL_ATTR( cell ) & 0x0F, bg = (CELL_ATTR( cell ) >> 4) & 0x0F;
    u8 extra = CELL_EXTRA( cell );

    if ( pic == NULL || row < 0 || row >= GFX_ROWS || col < 0 || col >= GW / 8 ) {
        return;
    }
    if ( extra == 0 ) {
        paint_char( CELL_CHAR( cell ), fg, bg );
    } else if ( EXTRA_IS_BAR( extra ) ) {
        paint_bar( extra, fg, bg );
    } else if ( EXTRA_IS_ICON( extra ) ) {
        paint_icon( extra - 1, bg );
    } else {
        paint_char( ' ', fg, bg );
    }
    for ( line = 0; line < CELL_H; line++ ) {
        memcpy( pic + (u32)(top + line) * GW + left, blk[line], 8 );
    }
    mark( left, top, left + 7, top + CELL_H - 1 );
}

/* ------------------------------------------------------------------ */
/* the pointer                                                         */
/* ------------------------------------------------------------------ */

void gfx_pointer( int x, int y, int show )
{
    if ( ptr_on ) {
        if ( show && x == ptr_x && y == ptr_y ) {
            return;
        }
        mark( ptr_x, ptr_y, ptr_x + 15, ptr_y + 15 );
    }
    ptr_x = x;
    ptr_y = y;
    ptr_on = show;
    if ( ptr_on ) {
        mark( ptr_x, ptr_y, ptr_x + 15, ptr_y + 15 );
    }
}

/* a scan line as it goes to the screen: the picture's, with the pointer
   laid over the pixels from "lo" to "hi" */
static const u8 *compose( int line, int lo, int hi, u8 *buf )
{
    const char *shape;
    int px, at;

    if ( !ptr_on || line < ptr_y || line >= ptr_y + 16 || ptr_x > hi || ptr_x + 15 < lo ) {
        return pic + (u32)line * GW;
    }
    memcpy( buf + lo, pic + (u32)line * GW + lo, (u32)(hi - lo + 1) );
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
   "bhi" of each are made into four planes in memory, then each plane is
   written with the sequencer letting only that plane take it. */
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
            out[LINE_BYTES * GH] = (u8)(acc >> 8);
            out[2 * LINE_BYTES * GH] = (u8)(acc >> 16);
            out[3 * LINE_BYTES * GH] = (u8)(acc >> 24);
        }
    }
    for ( plane = 0; plane < 4; plane++ ) {
        port_out( SEQ_INDEX, 2 );
        port_out( SEQ_DATA, 1u << plane );
        for ( line = first; line <= last; line++ ) {
            vid_put( vid_sel, (u32)line * LINE_BYTES + blo,
                     planes + (u32)plane * LINE_BYTES * GH + (u32)line * LINE_BYTES + blo,
                     (u32)(bhi - blo + 1) );
        }
    }
    port_out( SEQ_INDEX, 2 );
    port_out( SEQ_DATA, 0x0F );
}

void gfx_flush( void )
{
    int line = 0, first, lo, hi;

    if ( pic == NULL || !mode_on ) {
        return;
    }
    while ( line < GH ) {
        if ( dirty_lo[line] > dirty_hi[line] ) {
            line++;
            continue;
        }
        first = line;
        lo = dirty_lo[line];
        hi = dirty_hi[line];
        while ( line < GH && dirty_lo[line] <= dirty_hi[line] ) {
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

/* a Windows bitmap, a byte a pixel: the picture with the pointer, which
   is what the screen was sent */
int gfx_snapshot( const char *path )
{
    static u8 head[54 + 1024], buf[GW];
    FILE *fp;
    int line, color, ok;

    if ( pic == NULL || (fp = fopen( path, "wb" )) == NULL ) {
        return 0;
    }
    memset( head, 0, sizeof( head ) );
    head[0] = 'B';
    head[1] = 'M';
    put32( head + 2, sizeof( head ) + (u32)GW * GH );
    put32( head + 10, sizeof( head ) );
    put32( head + 14, 40 );
    put32( head + 18, GW );
    put32( head + 22, GH );
    head[26] = 1;
    head[28] = 8;
    put32( head + 34, (u32)GW * GH );
    put32( head + 46, 256 );
    for ( color = 0; color < 16; color++ ) {
        head[54 + color * 4] = (u8)(vga16[color][2] << 2 | vga16[color][2] >> 4);
        head[54 + color * 4 + 1] = (u8)(vga16[color][1] << 2 | vga16[color][1] >> 4);
        head[54 + color * 4 + 2] = (u8)(vga16[color][0] << 2 | vga16[color][0] >> 4);
    }
    ok = fwrite( head, sizeof( head ), 1, fp ) == 1;
    for ( line = GH - 1; line >= 0 && ok; line-- ) {
        ok = fwrite( compose( line, 0, GW - 1, buf ), GW, 1, fp ) == 1;
    }
    if ( fclose( fp ) != 0 ) {
        ok = 0;
    }
    return ok;
}
