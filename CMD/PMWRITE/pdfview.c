/*
 * PDFVIEW.C - a PDF's pages as pictures and as text (PDFVIEW.H): the
 * PDF code in PDF\ with its jumps, pools and canvases kept out of
 * sight.
 *
 * HOW A CALL IS KEPT FROM ENDING THE PROGRAM.  The PDF code leaves a
 * page it cannot finish by pdf_fail, which jumps to wherever pdf_trap
 * points and, when it points nowhere, exits.  So every call here that
 * reads the file sets the trap first, and what the page had allocated
 * goes with the page's pool when pdf_page_end is called after it.  A
 * shortage of memory comes the same way, as long as the program's
 * out_of_memory calls pdfview_out_of_memory.
 *
 * What is changed after the trap is set and wanted after a jump is
 * kept in statics: a jump puts the registers back as they were.
 *
 * A JPEG FILE is decoded by the PDF code's own decoder (PDF\DCT.C),
 * which takes its memory from the page's pool.  pdfview_jpeg lends it
 * a pool of its own for the length of the call and puts the page's
 * back afterwards, so a PDF that is open - even one whose page is
 * being drawn - is as it was.
 */
#include "pdfview.h"
#include "gfx.h"

#define JPEG_SIDE   4096                    /* pixels, either way */
#define JPEG_BYTES  (16UL * 1024 * 1024)    /* of the picture decoded */

static int     waiting;             /* open as far as its password */
static int   (*poll_fn)( void );
static JMPBUF  trap;
static JMPBUF  jpeg_trap;
static u8     *jpeg_out;
static POOL   *jpeg_pool;

/* ------------------------------------------------------------------ */
/* the program's part                                                  */
/* ------------------------------------------------------------------ */

int pdfview_reclaim( void )
{
    return pdf_reclaim();
}

void pdfview_out_of_memory( void )
{
    if ( pdf_trap ) {
        pdf_fail( PE_NOMEM );
    }
}

/* after a call that ran out of memory: the fonts and the glyphs are
   the largest things kept, and the program wants the room more */
static int settle( int why )
{
    if ( why == PV_NOMEM ) {
        glyph_page_begin();
        font_reclaim( 1 );
    }
    return why;
}

/* ------------------------------------------------------------------ */
/* opening and closing                                                 */
/* ------------------------------------------------------------------ */

int pdfview_open( const char *path )
{
    int why = pdf_open( path );

    waiting = why == PE_PASSWORD;
    return why;
}

int pdfview_open_mem( const u8 *data, u32 len )
{
    int why = pdf_open_mem( data, len );

    waiting = why == PE_PASSWORD;
    return why;
}

int pdfview_password( const char *text, u32 len )
{
    static int good;
    int why;

    if ( !waiting || doc.root == NULL || text == NULL ) {
        return PV_PARAM;
    }
    /* the newest kind of password is hashed in memory from the heap */
    good = 0;
    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        good = pdf_password( text, len );
    }
    pdf_trap = NULL;
    if ( why != 0 ) {
        return settle( why );
    }
    if ( !good ) {
        return PV_PASSWORD;
    }
    waiting = 0;
    return settle( pdf_open_finish() );
}

void pdfview_close( void )
{
    waiting = 0;
    pdf_close();
}

int pdfview_is_open( void )
{
    return doc.root != NULL && !waiting;
}

int pdfview_pages( void )
{
    return pdfview_is_open() ? doc.page_count : 0;
}

/* ------------------------------------------------------------------ */
/* a page                                                              */
/* ------------------------------------------------------------------ */

/* 0, 90, 180 or 270, whatever was asked */
static int turn_of( int turn )
{
    turn = (turn % 360 + 360) % 360;
    return (turn + 45) / 90 * 90 % 360;
}

static s32 zoom_of( s32 zoom )
{
    if ( zoom < FX_ONE / 16 ) {
        return FX_ONE / 16;
    }
    return zoom > 64 * FX_ONE ? 64 * FX_ONE : zoom;
}

int pdfview_page_size( int page, s32 *width, s32 *height )
{
    static s32 wide, high;
    PAGE info;
    int why;

    *width = *height = 0;
    if ( !pdfview_is_open() ) {
        return PV_NOPAGE;
    }
    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        if ( !pdf_page( page, &info ) ) {
            why = PV_NOPAGE;
        } else {
            wide = real_to_fx( info.box[2] ) - real_to_fx( info.box[0] );
            high = real_to_fx( info.box[3] ) - real_to_fx( info.box[1] );
            if ( info.rotate == 90 || info.rotate == 270 ) {
                *width = high;
                *height = wide;
            } else {
                *width = wide;
                *height = high;
            }
        }
    }
    pdf_page_end();
    pdf_trap = NULL;
    return settle( why );
}

int pdfview_page_pixels( int page, s32 zoom, int turn, int *width, int *height )
{
    PAGE info;
    VIEW view;
    int why;

    *width = *height = 0;
    if ( !pdfview_is_open() ) {
        return PV_NOPAGE;
    }
    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        if ( !pdf_page( page, &info ) ) {
            why = PV_NOPAGE;
        } else {
            view_setup( &view, &info, zoom_of( zoom ), turn_of( turn ) );
            *width = view.width;
            *height = view.height;
        }
    }
    pdf_page_end();
    pdf_trap = NULL;
    return settle( why );
}

void pdfview_poll( int (*poll)( void ) )
{
    poll_fn = poll;
}

int pdfview_render( int page, s32 zoom, int turn, const PVBLOCK *block )
{
    PAGE info;
    VIEW view;
    CANVAS canvas;
    int why, row;

    if ( block == NULL || block->pix == NULL || block->width <= 0 || block->height <= 0 ||
         block->width > 32000 || block->height > 32000 || (block->bpp != 1 && block->bpp != 3) ||
         block->stride < (u32)block->width * (u32)block->bpp ||
         block->stride > 0x7FFFFFFFUL / (u32)block->height || block->org_x < -1000000L ||
         block->org_x > 1000000L || block->org_y < -1000000L || block->org_y > 1000000L ) {
        return PV_PARAM;
    }
    /* paper, whatever comes of the page */
    for ( row = 0; row < block->height; row++ ) {
        mem_set( block->pix + (u32)row * block->stride, 0xFF, (u32)block->width * (u32)block->bpp );
    }
    if ( !pdfview_is_open() ) {
        return PV_NOPAGE;
    }
    canvas.pix = block->pix;
    canvas.width = block->width;
    canvas.height = block->height;
    canvas.bpp = block->bpp;
    canvas.stride = block->stride;
    canvas.org_x = block->org_x;
    canvas.org_y = block->org_y;

    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        if ( !pdf_page( page, &info ) ) {
            why = PV_NOPAGE;
        } else {
            view_setup( &view, &info, zoom_of( zoom ), turn_of( turn ) );
            render_poll = poll_fn;
            page_render( &info, &view, &canvas );
        }
    }
    render_poll = NULL;
    pdf_page_end();
    pdf_trap = NULL;
    return settle( why );
}

int pdfview_text( int page, char ***lines, int *count )
{
    static char **block;
    TEXTPAGE *text;
    char *put;
    u32 bytes, len;
    int why, index;

    *lines = NULL;
    *count = 0;
    if ( !pdfview_is_open() ) {
        return PV_NOPAGE;
    }
    block = NULL;
    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        text = text_extract( page );
        if ( text == NULL ) {
            why = PV_NOPAGE;
        } else if ( text->count > 0 ) {
            bytes = (u32)text->count * sizeof( char * );
            for ( index = 0; index < text->count; index++ ) {
                bytes += str_len( text->lines[index] ) + 1;
            }
            block = (char **)try_alloc( bytes );
            if ( block == NULL ) {
                why = PV_NOMEM;
            } else {
                put = (char *)(block + text->count);
                for ( index = 0; index < text->count; index++ ) {
                    len = str_len( text->lines[index] ) + 1;
                    mem_cpy( put, text->lines[index], len );
                    block[index] = put;
                    put += len;
                }
                *lines = block;
                *count = text->count;
            }
        }
    }
    text_sink = NULL;                   /* a page given up may have left it set */
    pdf_page_end();
    pdf_trap = NULL;
    return settle( why );
}

/* ------------------------------------------------------------------ */
/* a JPEG file                                                         */
/* ------------------------------------------------------------------ */

/*
 * The markers of a JPEG file up to its picture: how large it is, how
 * many components, and whether Adobe's marker is there - which is what
 * says that four components are inks written upside down, 255 for
 * none.  0 if it is not a JPEG of the kind DCT.C reads.
 */
static int jpeg_look( const u8 *data, u32 len, u32 *width, u32 *height, int *comps, int *adobe )
{
    u32 pos = 2, size;
    int marker;

    *adobe = 0;
    if ( len < 4 || data[0] != 0xFF || data[1] != 0xD8 ) {
        return 0;
    }
    while ( pos + 4 <= len ) {
        if ( data[pos] != 0xFF ) {
            return 0;
        }
        marker = data[pos + 1];
        if ( marker == 0xFF ) {
            pos++;                              /* fill bytes before a marker */
            continue;
        }
        if ( marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) ) {
            pos += 2;                           /* these have nothing after them */
            continue;
        }
        size = ((u32)data[pos + 2] << 8) | data[pos + 3];
        if ( size < 2 || size > len - pos - 2 ) {
            return 0;
        }
        if ( marker == 0xC0 || marker == 0xC1 ) {
            if ( size < 8 || data[pos + 4] != 8 ) {
                return 0;                       /* twelve bits a sample */
            }
            *height = ((u32)data[pos + 5] << 8) | data[pos + 6];
            *width = ((u32)data[pos + 7] << 8) | data[pos + 8];
            *comps = data[pos + 9];
            return *width != 0 && *height != 0 && (*comps == 1 || *comps == 3 || *comps == 4);
        }
        if ( (marker >= 0xC2 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) ||
             marker == 0xDA || marker == 0xD9 ) {
            return 0;                           /* progressive, lossless, arithmetic; or no frame */
        }
        if ( marker == 0xEE && size >= 14 && mem_cmp( data + pos + 4, "Adobe", 5 ) == 0 ) {
            *adobe = 1;
        }
        pos += 2 + size;
    }
    return 0;
}

u8 *pdfview_jpeg( const u8 *data, u32 len, u32 *width, u32 *height )
{
    static IN *in;
    static int good;
    JMPBUF *outer_trap = pdf_trap;
    POOL *outer_pool = doc.page_pool;
    u32 wide, high, row, col;
    u8 *line, *pix;
    int comps, adobe, keep;

    *width = *height = 0;
    if ( data == NULL || !jpeg_look( data, len, &wide, &high, &comps, &adobe ) ||
         wide > JPEG_SIDE || high > JPEG_SIDE || wide * high * 3 > JPEG_BYTES ) {
        return NULL;
    }
    jpeg_out = (u8 *)try_alloc( wide * high * 3 );
    if ( jpeg_out == NULL ) {
        return NULL;
    }
    jpeg_pool = NULL;
    in = NULL;
    good = 0;
    pdf_trap = &jpeg_trap;
    if ( rt_setjmp( &jpeg_trap ) == 0 ) {
        /* the decoder's memory is the page's pool: it is lent one */
        jpeg_pool = pool_new();
        doc.page_pool = jpeg_pool;
        in = flt_dct( in_mem( data, len ), NULL );
        if ( in != NULL && (u32)dct_width == wide && (u32)dct_height == high && dct_comps == comps ) {
            line = (u8 *)pool_big( jpeg_pool, wide * 4 );
            good = 1;
            for ( row = 0; row < high; row++ ) {
                if ( in_read( in, line, wide * (u32)comps ) != wide * (u32)comps ) {
                    good = 0;                   /* it stops short */
                    break;
                }
                pix = jpeg_out + row * wide * 3;
                for ( col = 0; col < wide; col++, pix += 3 ) {
                    if ( comps == 1 ) {
                        pix[0] = pix[1] = pix[2] = line[col];
                    } else if ( comps == 3 ) {
                        pix[0] = line[col * 3 + 2];
                        pix[1] = line[col * 3 + 1];
                        pix[2] = line[col * 3];
                    } else {
                        /* four inks, as IMAGE.C turns them into light */
                        keep = adobe ? line[col * 4 + 3] : 255 - line[col * 4 + 3];
                        pix[2] = (u8)((adobe ? line[col * 4] : 255 - line[col * 4]) * keep / 255);
                        pix[1] = (u8)((adobe ? line[col * 4 + 1] : 255 - line[col * 4 + 1]) * keep / 255);
                        pix[0] = (u8)((adobe ? line[col * 4 + 2] : 255 - line[col * 4 + 2]) * keep / 255);
                    }
                }
            }
        }
    } else {
        good = 0;                               /* damaged, or no memory for the decoding */
    }
    /* here by the straight way or by a jump: the lent pool goes, with
       everything the decoder had, and the page's own is put back */
    doc.page_pool = outer_pool;
    pdf_trap = outer_trap;
    if ( jpeg_pool ) {
        pool_free( jpeg_pool );
        jpeg_pool = NULL;
    }
    if ( !good ) {
        xfree( jpeg_out );
        jpeg_out = NULL;
        return NULL;
    }
    *width = wide;
    *height = high;
    return jpeg_out;
}
