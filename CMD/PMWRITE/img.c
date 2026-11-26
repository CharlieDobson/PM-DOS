/*
 * IMG.C - a picture file's kind, size and pixels (IMG.H).
 *
 * PNG is read whole: its IDAT chunks are joined, inflated in one piece
 * (FLATE.C checks the stream's own checksum), and each line's filter
 * undone in place.  An interlaced picture is seven smaller pictures,
 * each with lines of its own, spread over the big one.  Whatever the
 * file's depth and colour type, a pixel comes out as blue, green, red;
 * a pixel that is partly clear is mixed with white, which is the paper
 * it will be shown on.
 *
 * EVERY SIZE IN THE FILE IS CHECKED before memory is asked for or a
 * byte is read: a picture is refused when a side is over IMG_MAX_SIDE,
 * and the inflated data must be exactly as long as the header says the
 * lines are.
 */
#include "fmt.h"
#include "img.h"
#include "flate.h"
#include "pdfview.h"

/* ------------------------------------------------------------------ */
/* what it is                                                          */
/* ------------------------------------------------------------------ */

static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };

/* a DIB's header starts with its own length */
static int dib_header( const uint8_t *data, uint32_t len )
{
    uint32_t size;

    if ( len < 16 ) {
        return 0;
    }
    size = LE32( data );
    return size == 12 || size == 40 || size == 52 || size == 56 || size == 108 || size == 124;
}

int img_kind( const uint8_t *data, uint32_t len )
{
    if ( len >= 8 && mem_cmp( data, png_sig, 8 ) == 0 ) {
        return IMG_PNG;
    }
    if ( len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF ) {
        return IMG_JPEG;
    }
    if ( len >= 26 && data[0] == 'B' && data[1] == 'M' && dib_header( data + 14, len - 14 ) ) {
        return IMG_BMP;
    }
    if ( len >= 6 && mem_cmp( data, "GIF8", 4 ) == 0 ) {
        return IMG_GIF;
    }
    if ( len >= 4 && ((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) ||
                      (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42)) ) {
        return IMG_TIFF;
    }
    if ( len >= 4 && LE32( data ) == 0x9AC6CDD7UL ) {
        return IMG_WMF;                 /* with the header that says how big it is */
    }
    if ( len >= 44 && LE32( data ) == 1 && LE32( data + 40 ) == 0x464D4520UL ) {
        return IMG_EMF;
    }
    if ( len >= 18 && LE16( data ) <= 2 && LE16( data ) >= 1 && LE16( data + 2 ) == 9 &&
         (LE16( data + 4 ) == 0x0300 || LE16( data + 4 ) == 0x0100) ) {
        return IMG_WMF;
    }
    if ( dib_header( data, len ) ) {
        return IMG_DIB;
    }
    return IMG_UNKNOWN;
}

static int jpeg_size( const uint8_t *data, uint32_t len, uint32_t *width, uint32_t *height, uint32_t *dpi_x, uint32_t *dpi_y )
{
    uint32_t pos = 2, seg;

    while ( IN_RANGE( pos, 4, len ) ) {
        if ( data[pos] != 0xFF ) {
            return 0;
        }
        if ( data[pos + 1] == 0xFF ) {
            pos++;
            continue;
        }
        seg = BE16( data + pos + 2 );
        if ( seg < 2 || !IN_RANGE( pos + 2, seg, len ) ) {
            return 0;
        }
        if ( data[pos + 1] == 0xE0 && seg >= 16 && mem_cmp( data + pos + 4, "JFIF", 5 ) == 0 ) {
            if ( data[pos + 11] == 1 ) {
                *dpi_x = BE16( data + pos + 12 );
                *dpi_y = BE16( data + pos + 14 );
            } else if ( data[pos + 11] == 2 ) {         /* to the centimeter */
                *dpi_x = BE16( data + pos + 12 ) * 254 / 100;
                *dpi_y = BE16( data + pos + 14 ) * 254 / 100;
            }
        }
        if ( data[pos + 1] >= 0xC0 && data[pos + 1] <= 0xCF && data[pos + 1] != 0xC4 &&
             data[pos + 1] != 0xC8 && data[pos + 1] != 0xCC ) {
            if ( seg < 7 ) {
                return 0;
            }
            *height = BE16( data + pos + 5 );
            *width = BE16( data + pos + 7 );
            return *width != 0 && *height != 0;
        }
        if ( data[pos + 1] == 0xD9 || data[pos + 1] == 0xDA ) {
            return 0;
        }
        pos += 2 + seg;
    }
    return 0;
}

int img_size( const uint8_t *data, uint32_t len, uint32_t *width, uint32_t *height, uint32_t *dpi_x, uint32_t *dpi_y )
{
    const uint8_t *dib;
    uint32_t pos, clen, left;
    int32_t high;

    *width = *height = *dpi_x = *dpi_y = 0;
    switch ( img_kind( data, len ) ) {
    case IMG_PNG:
        if ( len < 33 || BE32( data + 8 ) != 13 || mem_cmp( data + 12, "IHDR", 4 ) != 0 ) {
            return 0;
        }
        *width = BE32( data + 16 );
        *height = BE32( data + 20 );
        /* pHYs, if it comes before the picture itself: pixels to the meter */
        for ( pos = 33; IN_RANGE( pos, 12, len ); pos += 12 + clen ) {
            clen = BE32( data + pos );
            if ( clen > len || !IN_RANGE( pos + 8, clen, len ) || mem_cmp( data + pos + 4, "IDAT", 4 ) == 0 ) {
                break;
            }
            if ( mem_cmp( data + pos + 4, "pHYs", 4 ) == 0 && clen == 9 && data[pos + 16] == 1 ) {
                *dpi_x = (BE32( data + pos + 8 ) / 10 * 254 + 500) / 1000;
                *dpi_y = (BE32( data + pos + 12 ) / 10 * 254 + 500) / 1000;
            }
        }
        return *width != 0 && *height != 0 && *width < 0x10000UL && *height < 0x10000UL;
    case IMG_JPEG:
        return jpeg_size( data, len, width, height, dpi_x, dpi_y );
    case IMG_BMP:
    case IMG_DIB:
        dib = data[0] == 'B' && !dib_header( data, len ) ? data + 14 : data;
        left = len - (uint32_t)(dib - data);
        if ( LE32( dib ) == 12 ) {
            *width = LE16( dib + 4 );
            *height = LE16( dib + 6 );
        } else {
            if ( left < 40 ) {
                return 0;
            }
            high = (int32_t)LE32( dib + 8 );
            *width = LE32( dib + 4 );
            *height = high < 0 ? (uint32_t)0 - (uint32_t)high : (uint32_t)high;
            *dpi_x = (LE32( dib + 24 ) / 10 * 254 + 500) / 1000;
            *dpi_y = (LE32( dib + 28 ) / 10 * 254 + 500) / 1000;
        }
        return *width != 0 && *height != 0 && *width < 0x10000UL && *height < 0x10000UL;
    case IMG_GIF:
        if ( len < 10 ) {
            return 0;
        }
        *width = LE16( data + 6 );
        *height = LE16( data + 8 );
        return *width != 0 && *height != 0;
    }
    return 0;
}

void img_natural( const uint8_t *data, uint32_t len, int *width, int *height )
{
    uint32_t pix_w, pix_h, dpi_x, dpi_y;

    *width = *height = 1440;
    if ( !img_size( data, len, &pix_w, &pix_h, &dpi_x, &dpi_y ) ) {
        return;
    }
    if ( dpi_x < 30 || dpi_x > 2400 || dpi_y < 30 || dpi_y > 2400 ) {
        dpi_x = dpi_y = 96;
    }
    if ( pix_w > 30000 ) {
        pix_w = 30000;
    }
    if ( pix_h > 30000 ) {
        pix_h = 30000;
    }
    *width = (int)(pix_w * 1440 / dpi_x);
    *height = (int)(pix_h * 1440 / dpi_y);
    if ( *width > 8640 ) {
        /* six inches at most, and in proportion */
        *height = (int)((unsigned int)*height * 864 / ((unsigned int)*width / 10 + 1));
        *width = 8640;
    }
    if ( *width < 20 ) {
        *width = 20;
    }
    if ( *height < 20 ) {
        *height = 20;
    }
}

/* ------------------------------------------------------------------ */
/* PNG                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t width, height;
    int      depth, color, interlace;
    int      channels;          /* samples a pixel */
    int      bpp;               /* bytes a pixel, for the filters: 1 at least */
    uint8_t  pal[256][3];       /* red, green, blue */
    uint8_t  pal_alpha[256];
    int      npal;
    int      has_key;           /* one colour is clear */
    uint32_t key[3];
} PNG;

static uint32_t png_row_bytes( const PNG *png, uint32_t width )
{
    return (width * (uint32_t)png->channels * (uint32_t)png->depth + 7) / 8;
}

static int paeth( int left, int up, int corner )
{
    int guess = left + up - corner;
    int dl = guess > left ? guess - left : left - guess;
    int du = guess > up ? guess - up : up - guess;
    int dc = guess > corner ? guess - corner : corner - guess;

    if ( dl <= du && dl <= dc ) {
        return left;
    }
    return du <= dc ? up : corner;
}

/* a line's filter undone in place: 0 if it names a filter there is not */
static int png_unfilter( int filter, uint8_t *row, const uint8_t *prev, uint32_t bytes, int bpp )
{
    unsigned int index;
    int left, up, corner;

    for ( index = 0; index < bytes; index++ ) {
        left = index >= (unsigned int)bpp ? row[index - (unsigned int)bpp] : 0;
        up = prev != NULL ? prev[index] : 0;
        corner = prev != NULL && index >= (unsigned int)bpp ? prev[index - (unsigned int)bpp] : 0;
        switch ( filter ) {
        case 0:
            return 1;
        case 1:
            row[index] = (uint8_t)(row[index] + left);
            break;
        case 2:
            row[index] = (uint8_t)(row[index] + up);
            break;
        case 3:
            row[index] = (uint8_t)(row[index] + ((left + up) >> 1));
            break;
        case 4:
            row[index] = (uint8_t)(row[index] + paeth( left, up, corner ));
            break;
        default:
            return 0;
        }
    }
    return 1;
}

/* sample "index" of a line, as 0 to 255 whatever the depth */
static uint32_t png_sample( const PNG *png, const uint8_t *row, unsigned int index, uint32_t *raw )
{
    uint32_t value;
    unsigned int shift;

    switch ( png->depth ) {
    case 8:
        *raw = row[index];
        return row[index];
    case 16:
        *raw = BE16( row + index * 2 );
        return row[index * 2];
    }
    shift = 8 - (uint32_t)png->depth - (index * (uint32_t)png->depth) % 8;
    value = (row[index * (uint32_t)png->depth / 8] >> shift) & ((1u << png->depth) - 1);
    *raw = value;
    return value * 255 / ((1u << png->depth) - 1);
}

/* pixel "col" of a decoded line, written as blue, green, red */
static void png_pixel( const PNG *png, const uint8_t *row, unsigned int col, uint8_t *bgr )
{
    uint32_t red, green, blue, alpha = 255, raw[4];

    switch ( png->color ) {
    case 0:                             /* grey */
        red = green = blue = png_sample( png, row, col, &raw[0] );
        if ( png->has_key && raw[0] == png->key[0] ) {
            alpha = 0;
        }
        break;
    case 2:                             /* red, green, blue */
        red = png_sample( png, row, col * 3, &raw[0] );
        green = png_sample( png, row, col * 3 + 1, &raw[1] );
        blue = png_sample( png, row, col * 3 + 2, &raw[2] );
        if ( png->has_key && raw[0] == png->key[0] && raw[1] == png->key[1] && raw[2] == png->key[2] ) {
            alpha = 0;
        }
        break;
    case 3:                             /* a palette */
        png_sample( png, row, col, &raw[0] );
        raw[0] &= 0xFF;
        red = png->pal[raw[0]][0];
        green = png->pal[raw[0]][1];
        blue = png->pal[raw[0]][2];
        alpha = png->pal_alpha[raw[0]];
        break;
    case 4:                             /* grey, and how solid */
        red = green = blue = png_sample( png, row, col * 2, &raw[0] );
        alpha = png_sample( png, row, col * 2 + 1, &raw[1] );
        break;
    default:                            /* 6: red, green, blue, and how solid */
        red = png_sample( png, row, col * 4, &raw[0] );
        green = png_sample( png, row, col * 4 + 1, &raw[1] );
        blue = png_sample( png, row, col * 4 + 2, &raw[2] );
        alpha = png_sample( png, row, col * 4 + 3, &raw[3] );
        break;
    }
    if ( alpha != 255 ) {
        /* on white */
        red = (red * alpha + 255 * (255 - alpha) + 127) / 255;
        green = (green * alpha + 255 * (255 - alpha) + 127) / 255;
        blue = (blue * alpha + 255 * (255 - alpha) + 127) / 255;
    }
    bgr[0] = (uint8_t)blue;
    bgr[1] = (uint8_t)green;
    bgr[2] = (uint8_t)red;
}

static uint8_t *png_decode( const uint8_t *data, uint32_t len, uint32_t *width, uint32_t *height )
{
    static const uint8_t pass_x0[7] = { 0, 4, 0, 2, 0, 1, 0 }, pass_dx[7] = { 8, 8, 4, 4, 2, 2, 1 };
    static const uint8_t pass_y0[7] = { 0, 0, 4, 0, 2, 0, 1 }, pass_dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    PNG *png;
    uint8_t *idat = NULL, *raw = NULL, *out = NULL, *row, *prev;
    uint32_t pos, clen, idat_len = 0, used = 0, raw_len, need = 0, pass_w, pass_h;
    unsigned int line, col;
    uint32_t bytes;
    int pass, passes, index, good = 0;

    png = (PNG *)try_alloc( sizeof( PNG ) );
    if ( png == NULL ) {
        return NULL;
    }
    mem_set( png, 0, sizeof( PNG ) );
    mem_set( png->pal_alpha, 255, sizeof( png->pal_alpha ) );
    if ( len < 33 || BE32( data + 8 ) != 13 || mem_cmp( data + 12, "IHDR", 4 ) != 0 ) {
        goto done;
    }
    png->width = BE32( data + 16 );
    png->height = BE32( data + 20 );
    png->depth = data[24];
    png->color = data[25];
    png->interlace = data[28];
    if ( png->width == 0 || png->height == 0 || png->width > IMG_MAX_SIDE || png->height > IMG_MAX_SIDE ||
         data[26] != 0 || data[27] != 0 || png->interlace > 1 ) {
        goto done;
    }
    switch ( png->color ) {
    case 0:
        png->channels = 1;
        good = png->depth == 1 || png->depth == 2 || png->depth == 4 || png->depth == 8 || png->depth == 16;
        break;
    case 2:
        png->channels = 3;
        good = png->depth == 8 || png->depth == 16;
        break;
    case 3:
        png->channels = 1;
        good = png->depth == 1 || png->depth == 2 || png->depth == 4 || png->depth == 8;
        break;
    case 4:
        png->channels = 2;
        good = png->depth == 8 || png->depth == 16;
        break;
    case 6:
        png->channels = 4;
        good = png->depth == 8 || png->depth == 16;
        break;
    }
    if ( !good ) {
        goto done;
    }
    good = 0;
    png->bpp = png->channels * png->depth / 8;
    if ( png->bpp < 1 ) {
        png->bpp = 1;
    }
    /* the chunks: once to add up the picture's data, once to join it */
    for ( pass = 0; pass < 2; pass++ ) {
        for ( pos = 33; IN_RANGE( pos, 12, len ); pos += 12 + clen ) {
            clen = BE32( data + pos );
            if ( clen > len || !IN_RANGE( pos + 8, clen, len ) ) {
                break;
            }
            if ( mem_cmp( data + pos + 4, "IDAT", 4 ) == 0 ) {
                if ( pass == 0 ) {
                    idat_len += clen;
                } else if ( clen <= idat_len - used ) {
                    mem_cpy( idat + used, data + pos + 8, clen );
                    used += clen;
                }
            } else if ( pass == 0 && mem_cmp( data + pos + 4, "PLTE", 4 ) == 0 ) {
                png->npal = (int)(clen / 3 > 256 ? 256 : clen / 3);
                for ( index = 0; index < png->npal; index++ ) {
                    mem_cpy( png->pal[index], data + pos + 8 + (uint32_t)index * 3, 3 );
                }
            } else if ( pass == 0 && mem_cmp( data + pos + 4, "tRNS", 4 ) == 0 ) {
                if ( png->color == 3 ) {
                    mem_cpy( png->pal_alpha, data + pos + 8, clen > 256 ? 256 : clen );
                } else if ( png->color == 0 && clen >= 2 ) {
                    png->has_key = 1;
                    png->key[0] = BE16( data + pos + 8 );
                } else if ( png->color == 2 && clen >= 6 ) {
                    png->has_key = 1;
                    png->key[0] = BE16( data + pos + 8 );
                    png->key[1] = BE16( data + pos + 10 );
                    png->key[2] = BE16( data + pos + 12 );
                }
            } else if ( mem_cmp( data + pos + 4, "IEND", 4 ) == 0 ) {
                break;
            }
        }
        if ( pass == 0 ) {
            if ( idat_len == 0 ) {
                goto done;
            }
            idat = (uint8_t *)try_alloc( idat_len );
            if ( idat == NULL ) {
                goto done;
            }
        }
    }
    /* how long the lines are altogether, each with its filter byte */
    passes = png->interlace ? 7 : 1;
    for ( pass = 0; pass < passes; pass++ ) {
        pass_w = png->interlace ? (png->width + pass_dx[pass] - 1 - pass_x0[pass]) / pass_dx[pass] : png->width;
        pass_h = png->interlace ? (png->height + pass_dy[pass] - 1 - pass_y0[pass]) / pass_dy[pass] : png->height;
        if ( pass_w && pass_h ) {
            need += (png_row_bytes( png, pass_w ) + 1) * pass_h;
        }
    }
    raw = zlib_inflate_alloc( idat, used, &raw_len, need );
    xfree( idat );
    idat = NULL;
    if ( raw == NULL || raw_len != need ) {
        goto done;
    }
    out = (uint8_t *)try_alloc( png->width * png->height * 3 );
    if ( out == NULL ) {
        goto done;
    }
    mem_set( out, 255, png->width * png->height * 3 );
    row = raw;
    for ( pass = 0; pass < passes; pass++ ) {
        pass_w = png->interlace ? (png->width + pass_dx[pass] - 1 - pass_x0[pass]) / pass_dx[pass] : png->width;
        pass_h = png->interlace ? (png->height + pass_dy[pass] - 1 - pass_y0[pass]) / pass_dy[pass] : png->height;
        if ( pass_w == 0 || pass_h == 0 ) {
            continue;
        }
        bytes = png_row_bytes( png, pass_w );
        prev = NULL;
        for ( line = 0; line < pass_h; line++ ) {
            if ( !png_unfilter( row[0], row + 1, prev, bytes, png->bpp ) ) {
                goto done;
            }
            for ( col = 0; col < pass_w; col++ ) {
                if ( png->interlace ) {
                    png_pixel( png, row + 1, col, out + ((pass_y0[pass] + line * pass_dy[pass]) * png->width +
                               pass_x0[pass] + col * pass_dx[pass]) * 3 );
                } else {
                    png_pixel( png, row + 1, col, out + (line * png->width + col) * 3 );
                }
            }
            prev = row + 1;
            row += bytes + 1;
        }
    }
    good = 1;
    *width = png->width;
    *height = png->height;
done:
    xfree( idat );
    xfree( raw );
    xfree( png );
    if ( !good ) {
        xfree( out );
        out = NULL;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* BMP and DIB                                                         */
/* ------------------------------------------------------------------ */

static uint8_t *dib_decode( const uint8_t *dib, uint32_t len, const uint8_t *bits, uint32_t bits_len, uint32_t *width, uint32_t *height )
{
    uint8_t *out, *dst;
    const uint8_t *row, *pal;
    uint32_t head = LE32( dib ), wide, high, depth, compress = 0, colors = 0, stride;
    unsigned int line, col;
    uint32_t value, entry;
    uint32_t pal_len;
    int32_t signed_high;
    int top_down = 0, five_six = 0;

    if ( head == 12 ) {
        wide = LE16( dib + 4 );
        high = LE16( dib + 6 );
        depth = LE16( dib + 10 );
        entry = 3;
    } else {
        if ( len < 40 ) {
            return NULL;
        }
        wide = LE32( dib + 4 );
        signed_high = (int32_t)LE32( dib + 8 );
        top_down = signed_high < 0;
        high = top_down ? (uint32_t)0 - (uint32_t)signed_high : (uint32_t)signed_high;
        depth = LE16( dib + 14 );
        compress = LE32( dib + 16 );
        colors = LE32( dib + 32 );
        entry = 4;
    }
    if ( wide == 0 || high == 0 || wide > IMG_MAX_SIDE || high > IMG_MAX_SIDE || head > len ||
         (depth != 1 && depth != 4 && depth != 8 && depth != 16 && depth != 24 && depth != 32) ) {
        return NULL;
    }
    /* plain rows only; a 16 or 32 bit one may name its colour masks, of
       which the usual two layouts are known */
    if ( compress == 3 && (depth == 16 || depth == 32) ) {
        if ( head == 40 && IN_RANGE( 40, 12, len ) ) {
            five_six = depth == 16 && LE32( dib + 44 ) == 0x07E0;
            head += 12;
        } else if ( head >= 52 ) {
            five_six = depth == 16 && LE32( dib + 44 ) == 0x07E0;
        }
    } else if ( compress != 0 ) {
        return NULL;
    }
    pal = dib + head;
    pal_len = 0;
    if ( depth <= 8 ) {
        if ( colors == 0 || colors > (1u << depth) ) {
            colors = 1u << depth;
        }
        pal_len = colors * entry;
        if ( !IN_RANGE( head, pal_len, len ) ) {
            return NULL;
        }
    }
    if ( bits == NULL ) {
        bits = pal + pal_len;
        bits_len = len - head - pal_len;
    }
    stride = ((wide * depth + 31) / 32) * 4;
    if ( bits_len / stride < high ) {
        return NULL;
    }
    out = (uint8_t *)try_alloc( wide * high * 3 );
    if ( out == NULL ) {
        return NULL;
    }
    for ( line = 0; line < high; line++ ) {
        row = bits + (top_down ? line : high - 1 - line) * stride;
        dst = out + line * wide * 3;
        for ( col = 0; col < wide; col++, dst += 3 ) {
            switch ( depth ) {
            case 24:
                mem_cpy( dst, row + col * 3, 3 );
                continue;
            case 32:
                mem_cpy( dst, row + col * 4, 3 );
                continue;
            case 16:
                value = LE16( row + col * 2 );
                if ( five_six ) {
                    dst[0] = (uint8_t)((value & 0x1F) * 255 / 31);
                    dst[1] = (uint8_t)(((value >> 5) & 0x3F) * 255 / 63);
                    dst[2] = (uint8_t)(((value >> 11) & 0x1F) * 255 / 31);
                } else {
                    dst[0] = (uint8_t)((value & 0x1F) * 255 / 31);
                    dst[1] = (uint8_t)(((value >> 5) & 0x1F) * 255 / 31);
                    dst[2] = (uint8_t)(((value >> 10) & 0x1F) * 255 / 31);
                }
                continue;
            case 8:
                value = row[col];
                break;
            case 4:
                value = (row[col / 2] >> ((col & 1) ? 0 : 4)) & 15;
                break;
            default:
                value = (row[col / 8] >> (7 - (col & 7))) & 1;
                break;
            }
            if ( value >= colors ) {
                value = 0;
            }
            mem_cpy( dst, pal + value * entry, 3 );
        }
    }
    *width = wide;
    *height = high;
    return out;
}

uint8_t *img_decode( const uint8_t *data, uint32_t len, uint32_t *width, uint32_t *height )
{
    uint32_t start;

    *width = *height = 0;
    switch ( img_kind( data, len ) ) {
    case IMG_PNG:
        return png_decode( data, len, width, height );
    case IMG_JPEG:
        return pdfview_jpeg( data, len, width, height );
    case IMG_BMP:
        /* the file header says where the rows are, when it can be believed */
        start = LE32( data + 10 );
        if ( start >= 14 + 12 && start < len ) {
            return dib_decode( data + 14, len - 14, data + start, len - start, width, height );
        }
        return dib_decode( data + 14, len - 14, NULL, 0, width, height );
    case IMG_DIB:
        return dib_decode( data, len, NULL, 0, width, height );
    }
    return NULL;
}
