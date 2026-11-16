/*
 * DCT.C - DCTDecode: a JPEG image, as a filter that gives its samples
 * a row at a time.
 *
 * What is read is what almost every JPEG in a PDF file is: sequential,
 * Huffman-coded, eight bits a sample, one scan with every component in
 * it (or one component).  A progressive JPEG, an arithmetic-coded one
 * and a twelve-bit one are not read; the image then gets a grey patch.
 *
 * The rows come out a band at a time - one row of the image's blocks
 * (eight or sixteen lines) is decoded into a plane for each component
 * and handed over line by line - so a picture of any size takes the
 * memory of one band.  Y, Cb and Cr are turned into red, green and
 * blue here, because PDF says the filter does it.
 */
#include "gfx.h"

#define FAST_BITS   9

typedef struct {
    u8  fast[1 << FAST_BITS];
    u16 code[256];
    u8  values[256];
    u8  size[257];
    u32 maxcode[18];
    s32 delta[17];
} JHUFF;

typedef struct {
    int  id, hs, vs, tq, td, ta;
    int  pred;
    int  plane_w;
    int  hshift, vshift;            /* how much coarser than the finest */
    u8  *plane;
} JCOMP;

typedef struct {
    JHUFF dc[4], ac[4];
    u16   quant[4][64];
    JCOMP comp[4];
    int   ncomp, width, height, hmax, vmax;
    int   mcu_w, mcu_h, mcus_x, mcus_y;
    int   restart, todo;
    u32   code_buf;
    int   code_bits, marker, nomore;
    int   mcu_row, line, lines_have, out_y;
    int   transform;                /* -1: by the number of components */
    int   adobe;                    /* an Adobe marker was there, and its transform */
    u8   *row;
} JPEG;

int dct_width, dct_height, dct_comps;

static const u8 dezigzag[64 + 15] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27,
    20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63 };

static int get8( IN *src )
{
    int ch = IN_GETC( src );

    return ch < 0 ? 0 : ch;
}

static int get16( IN *src )
{
    int hi = get8( src );

    return (hi << 8) | get8( src );
}

/* ------------------------------------------------------------------ */
/* Huffman                                                             */
/* ------------------------------------------------------------------ */

static int huff_build( JHUFF *huff, const int *counts )
{
    u32 code = 0;
    int index, len, total = 0, size, span;

    for ( index = 0; index < 16; index++ ) {
        for ( len = 0; len < counts[index]; len++ ) {
            if ( total >= 256 ) {
                return 0;
            }
            huff->size[total++] = (u8)(index + 1);
        }
    }
    huff->size[total] = 0;
    index = 0;
    for ( len = 1; len <= 16; len++ ) {
        huff->delta[len] = (s32)index - (s32)code;
        if ( huff->size[index] == len ) {
            while ( huff->size[index] == len ) {
                huff->code[index++] = (u16)code++;
            }
            if ( code - 1 >= (1UL << len) ) {
                return 0;
            }
        }
        huff->maxcode[len] = code << (16 - len);
        code <<= 1;
    }
    huff->maxcode[17] = 0xFFFFFFFFUL;
    mem_set( huff->fast, 255, sizeof( huff->fast ) );
    for ( index = 0; index < total; index++ ) {
        size = huff->size[index];
        if ( size <= FAST_BITS ) {
            span = 1 << (FAST_BITS - size);
            mem_set( huff->fast + ((u32)huff->code[index] << (FAST_BITS - size)), index, (u32)span );
        }
    }
    return 1;
}

static void grow( JPEG *jp, IN *src )
{
    int byte, next;

    do {
        byte = jp->nomore ? 0 : get8( src );
        if ( byte == 0xFF && !jp->nomore ) {
            next = get8( src );
            while ( next == 0xFF ) {
                next = get8( src );
            }
            if ( next != 0 ) {
                jp->marker = next;              /* the scan's data ends here */
                jp->nomore = 1;
                byte = 0;
            }
        }
        if ( src->at_eof ) {
            jp->nomore = 1;
        }
        jp->code_buf |= (u32)byte << (24 - jp->code_bits);
        jp->code_bits += 8;
    } while ( jp->code_bits <= 24 );
}

static int huff_decode( JPEG *jp, IN *src, const JHUFF *huff )
{
    u32 top;
    int look, len;

    if ( jp->code_bits < 16 ) {
        grow( jp, src );
    }
    look = huff->fast[jp->code_buf >> (32 - FAST_BITS)];
    if ( look < 255 ) {
        len = huff->size[look];
        if ( len > jp->code_bits ) {
            return -1;
        }
        jp->code_buf <<= len;
        jp->code_bits -= len;
        return huff->values[look];
    }
    top = jp->code_buf >> 16;
    for ( len = FAST_BITS + 1; top >= huff->maxcode[len]; len++ ) {
    }
    if ( len >= 17 || len > jp->code_bits ) {
        jp->code_bits = 0;
        return -1;
    }
    look = (int)((jp->code_buf >> (32 - len)) + (u32)huff->delta[len]) & 255;
    jp->code_buf <<= len;
    jp->code_bits -= len;
    return huff->values[look];
}

/* "count" more bits, as the signed number they stand for */
static int receive( JPEG *jp, IN *src, int count )
{
    int val;

    if ( jp->code_bits < count ) {
        grow( jp, src );
    }
    val = (int)(jp->code_buf >> (32 - count));
    jp->code_buf <<= count;
    jp->code_bits -= count;
    if ( val < (1 << (count - 1)) ) {
        val += (int)(0xFFFFFFFFUL << count) + 1;
    }
    return val;
}

/* ------------------------------------------------------------------ */
/* a block                                                             */
/* ------------------------------------------------------------------ */

static int clamp8( s32 val )
{
    return val < 0 ? 0 : (val > 255 ? 255 : (int)val);
}

#define IDCT_1D( s0, s1, s2, s3, s4, s5, s6, s7 )               \
    p2 = s2;                                                    \
    p3 = s6;                                                    \
    p1 = (p2 + p3) * 2217;                                      \
    t2 = p1 + p3 * -7568;                                       \
    t3 = p1 + p2 * 3135;                                        \
    p2 = s0;                                                    \
    p3 = s4;                                                    \
    t0 = (p2 + p3) * 4096;                                      \
    t1 = (p2 - p3) * 4096;                                      \
    x0 = t0 + t3;                                               \
    x3 = t0 - t3;                                               \
    x1 = t1 + t2;                                               \
    x2 = t1 - t2;                                               \
    t0 = s7;                                                    \
    t1 = s5;                                                    \
    t2 = s3;                                                    \
    t3 = s1;                                                    \
    p3 = t0 + t2;                                               \
    p4 = t1 + t3;                                               \
    p1 = t0 + t3;                                               \
    p2 = t1 + t2;                                               \
    p5 = (p3 + p4) * 4816;                                      \
    t0 = t0 * 1223;                                             \
    t1 = t1 * 8410;                                             \
    t2 = t2 * 12586;                                            \
    t3 = t3 * 6149;                                             \
    p1 = p5 + p1 * -3686;                                       \
    p2 = p5 + p2 * -10498;                                      \
    p3 = p3 * -8035;                                            \
    p4 = p4 * -1598;                                            \
    t3 += p1 + p4;                                              \
    t2 += p2 + p3;                                              \
    t1 += p2 + p4;                                              \
    t0 += p1 + p3;

/* 64 coefficients into 64 samples at "out", "stride" bytes a row */
static void idct_block( u8 *out, int stride, const s16 *data )
{
    s32 work[64], *col = work;
    s32 t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3, flat;
    const s16 *in = data;
    int index;

    for ( index = 0; index < 8; index++, in++, col++ ) {
        if ( in[8] == 0 && in[16] == 0 && in[24] == 0 && in[32] == 0 && in[40] == 0 && in[48] == 0 &&
             in[56] == 0 ) {
            flat = (s32)in[0] * 4;
            col[0] = col[8] = col[16] = col[24] = col[32] = col[40] = col[48] = col[56] = flat;
            continue;
        }
        IDCT_1D( in[0], in[8], in[16], in[24], in[32], in[40], in[48], in[56] )
        x0 += 512;
        x1 += 512;
        x2 += 512;
        x3 += 512;
        col[0] = (x0 + t3) >> 10;
        col[56] = (x0 - t3) >> 10;
        col[8] = (x1 + t2) >> 10;
        col[48] = (x1 - t2) >> 10;
        col[16] = (x2 + t1) >> 10;
        col[40] = (x2 - t1) >> 10;
        col[24] = (x3 + t0) >> 10;
        col[32] = (x3 - t0) >> 10;
    }
    for ( index = 0, col = work; index < 8; index++, col += 8, out += stride ) {
        IDCT_1D( col[0], col[1], col[2], col[3], col[4], col[5], col[6], col[7] )
        x0 += 65536L + (128L << 17);
        x1 += 65536L + (128L << 17);
        x2 += 65536L + (128L << 17);
        x3 += 65536L + (128L << 17);
        out[0] = (u8)clamp8( (x0 + t3) >> 17 );
        out[7] = (u8)clamp8( (x0 - t3) >> 17 );
        out[1] = (u8)clamp8( (x1 + t2) >> 17 );
        out[6] = (u8)clamp8( (x1 - t2) >> 17 );
        out[2] = (u8)clamp8( (x2 + t1) >> 17 );
        out[5] = (u8)clamp8( (x2 - t1) >> 17 );
        out[3] = (u8)clamp8( (x3 + t0) >> 17 );
        out[4] = (u8)clamp8( (x3 - t0) >> 17 );
    }
}

static int decode_block( JPEG *jp, IN *src, JCOMP *comp, s16 *data )
{
    const JHUFF *ac = &jp->ac[comp->ta];
    const u16 *quant = jp->quant[comp->tq];
    int sym, run, size, at = 1;

    mem_set( data, 0, 64 * sizeof( s16 ) );
    sym = huff_decode( jp, src, &jp->dc[comp->td] );
    if ( sym < 0 || sym > 15 ) {
        return 0;
    }
    comp->pred += sym ? receive( jp, src, sym ) : 0;
    data[0] = (s16)(comp->pred * quant[0]);
    do {
        sym = huff_decode( jp, src, ac );
        if ( sym < 0 ) {
            return 0;
        }
        size = sym & 15;
        run = sym >> 4;
        if ( size == 0 ) {
            if ( sym != 0xF0 ) {
                break;                              /* the rest are zero */
            }
            at += 16;
        } else {
            at += run;
            data[dezigzag[at]] = (s16)(receive( jp, src, size ) * quant[dezigzag[at]]);
            at++;
        }
    } while ( at < 64 );
    return 1;
}

/* one row of the image's blocks, into the components' planes */
static void decode_band( JPEG *jp, IN *src )
{
    s16 data[64];
    JCOMP *comp;
    int mcu, index, across, down, good = 1;

    for ( mcu = 0; mcu < jp->mcus_x; mcu++ ) {
        if ( jp->restart && jp->todo == 0 ) {
            /* a restart marker: the coder starts afresh */
            if ( jp->code_bits < 24 && !jp->nomore ) {
                grow( jp, src );
            }
            jp->code_buf = 0;
            jp->code_bits = 0;
            if ( jp->marker < 0xD0 || jp->marker > 0xD7 ) {
                good = 0;                           /* something else: the data is done */
            } else {
                jp->nomore = 0;
                jp->marker = 0;
            }
            for ( index = 0; index < jp->ncomp; index++ ) {
                jp->comp[index].pred = 0;
            }
            jp->todo = jp->restart;
        }
        if ( jp->restart ) {
            jp->todo--;
        }
        for ( index = 0; index < jp->ncomp; index++ ) {
            comp = &jp->comp[index];
            for ( down = 0; down < comp->vs; down++ ) {
                for ( across = 0; across < comp->hs; across++ ) {
                    if ( good && !decode_block( jp, src, comp, data ) ) {
                        good = 0;
                    }
                    if ( !good ) {
                        mem_set( data, 0, sizeof( data ) );
                    }
                    idct_block( comp->plane + (u32)(down * 8) * (u32)comp->plane_w +
                                (u32)((mcu * comp->hs + across) * 8), comp->plane_w, data );
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* the filter                                                          */
/* ------------------------------------------------------------------ */

static int dct_fill( IN *in )
{
    JPEG *jp = IN_STATE( in, JPEG );
    JCOMP *comp;
    u8 *out = jp->row;
    const u8 *line;
    s32 luma, cb, cr;
    int col, index;

    if ( jp->out_y >= jp->height ) {
        return 0;
    }
    if ( jp->line >= jp->lines_have ) {
        decode_band( jp, in->src );
        jp->lines_have = jp->mcu_h;
        jp->line = 0;
    }
    for ( index = 0; index < jp->ncomp; index++ ) {
        comp = &jp->comp[index];
        line = comp->plane + (u32)(jp->line >> comp->vshift) * (u32)comp->plane_w;
        out = jp->row + index;
        if ( comp->hshift == 0 ) {
            for ( col = 0; col < jp->width; col++, out += jp->ncomp ) {
                *out = line[col];
            }
        } else {
            for ( col = 0; col < jp->width; col++, out += jp->ncomp ) {
                *out = line[col >> comp->hshift];
            }
        }
    }
    if ( jp->transform && jp->ncomp >= 3 ) {
        out = jp->row;
        for ( col = 0; col < jp->width; col++, out += jp->ncomp ) {
            luma = (s32)out[0] << 16;
            cb = (s32)out[1] - 128;
            cr = (s32)out[2] - 128;
            out[0] = (u8)clamp8( (luma + 91881L * cr + 32768L) >> 16 );
            out[1] = (u8)clamp8( (luma - 22554L * cb - 46802L * cr + 32768L) >> 16 );
            out[2] = (u8)clamp8( (luma + 116130L * cb + 32768L) >> 16 );
        }
    }
    jp->line++;
    jp->out_y++;
    in->cur = jp->row;
    in->end = jp->row + (u32)jp->width * (u32)jp->ncomp;
    return 1;
}

static void dct_done( IN *in )
{
    JPEG *jp = IN_STATE( in, JPEG );
    int index;

    for ( index = 0; index < 4; index++ ) {
        pool_unbig( jp->comp[index].plane );
    }
    pool_unbig( jp->row );
}

/* the markers up to the scan: 0 if this is not a JPEG that is read */
static int read_header( JPEG *jp, IN *src )
{
    int counts[16];
    JHUFF *huff;
    JCOMP *comp;
    int marker, len, index, total, which, prec, pick, shift;

    if ( get8( src ) != 0xFF || get8( src ) != 0xD8 ) {
        return 0;
    }
    for ( ;; ) {
        marker = get8( src );
        if ( src->at_eof ) {
            return 0;
        }
        if ( marker != 0xFF ) {
            continue;
        }
        while ( marker == 0xFF ) {
            marker = get8( src );
        }
        if ( marker == 0 || (marker >= 0xD0 && marker <= 0xD8) ) {
            continue;
        }
        len = get16( src ) - 2;
        if ( len < 0 ) {
            return 0;
        }
        switch ( marker ) {
        case 0xDB:                                  /* quantisation tables */
            while ( len > 0 ) {
                which = get8( src );
                prec = which >> 4;
                which &= 3;
                for ( index = 0; index < 64; index++ ) {
                    jp->quant[which][dezigzag[index]] = (u16)(prec ? get16( src ) : get8( src ));
                }
                len -= prec ? 129 : 65;
            }
            break;
        case 0xC4:                                  /* Huffman tables */
            while ( len > 0 ) {
                which = get8( src );
                total = 0;
                for ( index = 0; index < 16; index++ ) {
                    counts[index] = get8( src );
                    total += counts[index];
                }
                if ( total > 256 || (which & 15) > 3 ) {
                    return 0;
                }
                huff = (which >> 4) ? &jp->ac[which & 3] : &jp->dc[which & 3];
                if ( !huff_build( huff, counts ) ) {
                    return 0;
                }
                for ( index = 0; index < total; index++ ) {
                    huff->values[index] = (u8)get8( src );
                }
                len -= 17 + total;
            }
            break;
        case 0xC0:                                  /* the frame: sequential */
        case 0xC1:
            if ( get8( src ) != 8 ) {
                return 0;
            }
            jp->height = get16( src );
            jp->width = get16( src );
            jp->ncomp = get8( src );
            if ( jp->ncomp < 1 || jp->ncomp > 4 || jp->width < 1 || jp->height < 1 ) {
                return 0;
            }
            for ( index = 0; index < jp->ncomp; index++ ) {
                comp = &jp->comp[index];
                comp->id = get8( src );
                which = get8( src );
                comp->hs = which >> 4;
                comp->vs = which & 15;
                comp->tq = get8( src ) & 3;
                if ( comp->hs < 1 || comp->hs > 4 || comp->vs < 1 || comp->vs > 4 ) {
                    return 0;
                }
            }
            len -= 6 + jp->ncomp * 3;
            in_skip( src, (u32)(len > 0 ? len : 0) );
            break;
        case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            return 0;                               /* progressive, lossless, arithmetic */
        case 0xDD:
            jp->restart = get16( src );
            in_skip( src, (u32)(len > 2 ? len - 2 : 0) );
            break;
        case 0xEE:                                  /* Adobe's: which colour transform */
            if ( len >= 12 ) {
                in_skip( src, 11 );
                jp->adobe = get8( src ) + 1;
                in_skip( src, (u32)(len - 12) );
            } else {
                in_skip( src, (u32)len );
            }
            break;
        case 0xDA:                                  /* the scan */
            total = get8( src );
            if ( jp->ncomp == 0 || total != jp->ncomp ) {
                return 0;
            }
            for ( index = 0; index < total; index++ ) {
                pick = get8( src );
                which = get8( src );
                for ( prec = 0; prec < jp->ncomp; prec++ ) {
                    if ( jp->comp[prec].id == pick ) {
                        jp->comp[prec].td = (which >> 4) & 3;
                        jp->comp[prec].ta = which & 3;
                    }
                }
            }
            in_skip( src, 3 );
            /* the layout of a band */
            jp->hmax = jp->vmax = 1;
            if ( jp->ncomp == 1 ) {
                jp->comp[0].hs = jp->comp[0].vs = 1;
            }
            for ( index = 0; index < jp->ncomp; index++ ) {
                if ( jp->comp[index].hs > jp->hmax ) {
                    jp->hmax = jp->comp[index].hs;
                }
                if ( jp->comp[index].vs > jp->vmax ) {
                    jp->vmax = jp->comp[index].vs;
                }
            }
            jp->mcu_w = jp->hmax * 8;
            jp->mcu_h = jp->vmax * 8;
            jp->mcus_x = (jp->width + jp->mcu_w - 1) / jp->mcu_w;
            jp->mcus_y = (jp->height + jp->mcu_h - 1) / jp->mcu_h;
            for ( index = 0; index < jp->ncomp; index++ ) {
                comp = &jp->comp[index];
                for ( shift = 0; (comp->hs << shift) < jp->hmax; shift++ ) {
                }
                comp->hshift = shift;
                for ( shift = 0; (comp->vs << shift) < jp->vmax; shift++ ) {
                }
                comp->vshift = shift;
                comp->plane_w = jp->mcus_x * comp->hs * 8;
            }
            jp->todo = jp->restart;
            return 1;
        default:
            in_skip( src, (u32)len );
            break;
        }
    }
}

IN *flt_dct( IN *src, DICT *parms )
{
    IN *in = in_new( sizeof( JPEG ), dct_fill, src );
    JPEG *jp = IN_STATE( in, JPEG );
    int index;

    in->done = dct_done;
    if ( !read_header( jp, src ) ) {
        in_close( in );
        return NULL;
    }
    for ( index = 0; index < jp->ncomp; index++ ) {
        jp->comp[index].plane = (u8 *)pool_big( doc.page_pool,
                                                (u32)jp->comp[index].plane_w * (u32)jp->comp[index].vs * 8 );
    }
    jp->row = (u8 *)pool_big( doc.page_pool, (u32)jp->width * (u32)jp->ncomp + 8 );
    /* Y, Cb, Cr unless something says the samples are already colours */
    jp->transform = (int)dict_int( parms, "ColorTransform", -1 );
    if ( jp->transform < 0 ) {
        if ( jp->adobe ) {
            jp->transform = jp->adobe - 1 != 0;
        } else {
            jp->transform = jp->ncomp == 3;
        }
    }
    if ( jp->ncomp == 3 && jp->comp[0].id == 'R' && jp->comp[1].id == 'G' && jp->comp[2].id == 'B' ) {
        jp->transform = 0;
    }
    dct_width = jp->width;
    dct_height = jp->height;
    dct_comps = jp->ncomp;
    return in;
}
