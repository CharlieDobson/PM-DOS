/*
 * IMAGE.C - a sampled image onto the canvas.
 *
 * AN IMAGE IS NEVER HELD WHOLE.  A scanned page is eight million
 * samples and the screen is three hundred thousand pixels, so the
 * image is read a row at a time, each row is boiled down (or spread
 * out) to as many pixels across as it will cover, rows that land on
 * the same line of the screen are averaged together, and each
 * finished line goes to the canvas and is forgotten.  What is kept is
 * one row of the source and one of the destination.
 *
 * That works when the image's rows lie along the screen's rows or its
 * columns - upright, on its side, or mirrored - which is every image
 * but a tilted one.  A tilted one gets a grey patch where it would be.
 *
 * A soft mask or a stencil mask is a second image read alongside the
 * first, through a second SCALER working to the same grid.
 */
#include "gfx.h"

#define K_COLOUR    0           /* blue, green, red (and alpha, if keyed) */
#define K_STENCIL   1           /* one bit: paint or not */
#define K_ALPHA     2           /* a soft mask: how much */

typedef struct {
    IN     *in;
    int     width, height, bpc, ncomp, kind;
    CSPACE *cs;
    int     invert;
    int     has_key;
    int     key[MAX_COMPS * 2];
    fx      dec[MAX_COMPS * 2];
    u32    *lut;                /* one component: each value's colour */
    u32     rowbytes;
    u8     *raw, *pix;
    int     chans;
    int     dw, dh, col0, col1;
    u16    *xmap;               /* source column to destination column */
    u16    *xcount;             /* how many source columns each destination has */
    u32    *acc;
    u8     *done;
    int     src_row, out_row, rows_acc, repeat;
} SCALER;

/* ------------------------------------------------------------------ */
/* a source row into channel bytes                                     */
/* ------------------------------------------------------------------ */

static u32 sample( const u8 *raw, u32 index, int bpc )
{
    switch ( bpc ) {
    case 8:
        return raw[index];
    case 1:
        return (raw[index >> 3] >> (7 - (index & 7))) & 1;
    case 2:
        return (raw[index >> 2] >> (6 - (index & 3) * 2)) & 3;
    case 4:
        return (raw[index >> 1] >> (index & 1 ? 0 : 4)) & 15;
    }
    return ((u32)raw[index * 2] << 8) | raw[index * 2 + 1];
}

static void decode_row( SCALER *sc )
{
    const u8 *raw = sc->raw;
    u8 *pix = sc->pix;
    fx comps[MAX_COMPS];
    u32 rgb, val, peak = sc->bpc == 16 ? 65535UL : (1UL << sc->bpc) - 1;
    int col, comp, keep, hit, width = sc->width;

    if ( sc->kind == K_STENCIL ) {
        hit = sc->invert ? 1 : 0;
        for ( col = 0; col < width; col++ ) {
            pix[col] = (u8)(((raw[col >> 3] >> (7 - (col & 7))) & 1) == hit ? 255 : 0);
        }
        return;
    }
    if ( sc->kind == K_ALPHA ) {
        for ( col = 0; col < width; col++ ) {
            val = sample( raw, (u32)col * (u32)sc->ncomp, sc->bpc );
            val = sc->bpc == 16 ? val >> 8 : (val * 255) / peak;
            pix[col] = (u8)(sc->invert ? 255 - val : val);
        }
        return;
    }
    if ( sc->lut ) {
        for ( col = 0; col < width; col++, pix += sc->chans ) {
            val = sample( raw, (u32)col, sc->bpc );
            rgb = sc->lut[sc->bpc == 16 ? val >> 8 : val];
            pix[0] = (u8)rgb;
            pix[1] = (u8)(rgb >> 8);
            pix[2] = (u8)(rgb >> 16);
            if ( sc->has_key ) {
                pix[3] = (u8)((int)val >= sc->key[0] && (int)val <= sc->key[1] ? 0 : 255);
            }
        }
        return;
    }
    if ( sc->bpc == 8 && !sc->has_key && sc->ncomp == 3 && sc->cs->kind == CS_RGB ) {
        for ( col = 0; col < width; col++, pix += 3, raw += 3 ) {
            if ( sc->invert ) {
                pix[0] = (u8)(255 - raw[2]);
                pix[1] = (u8)(255 - raw[1]);
                pix[2] = (u8)(255 - raw[0]);
            } else {
                pix[0] = raw[2];
                pix[1] = raw[1];
                pix[2] = raw[0];
            }
        }
        return;
    }
    if ( sc->bpc == 8 && !sc->has_key && sc->ncomp == 4 && sc->cs->kind == CS_CMYK ) {
        for ( col = 0; col < width; col++, pix += 3, raw += 4 ) {
            keep = sc->invert ? raw[3] : 255 - raw[3];
            pix[2] = (u8)((sc->invert ? raw[0] : 255 - raw[0]) * keep / 255);
            pix[1] = (u8)((sc->invert ? raw[1] : 255 - raw[1]) * keep / 255);
            pix[0] = (u8)((sc->invert ? raw[2] : 255 - raw[2]) * keep / 255);
        }
        return;
    }
    /* any space, any depth: each sample through its decode, each pixel
       through the colour space */
    for ( col = 0; col < width; col++, pix += sc->chans ) {
        hit = sc->has_key;
        for ( comp = 0; comp < sc->ncomp; comp++ ) {
            val = sample( raw, (u32)col * (u32)sc->ncomp + (u32)comp, sc->bpc );
            if ( hit && ((int)val < sc->key[comp * 2] || (int)val > sc->key[comp * 2 + 1]) ) {
                hit = 0;
            }
            comps[comp] = sc->dec[comp * 2] +
                          mul_div( sc->dec[comp * 2 + 1] - sc->dec[comp * 2], (s32)val, (s32)peak );
        }
        rgb = cs_rgb( sc->cs, comps );
        pix[0] = (u8)rgb;
        pix[1] = (u8)(rgb >> 8);
        pix[2] = (u8)(rgb >> 16);
        if ( sc->has_key ) {
            pix[3] = (u8)(hit ? 0 : 255);
        }
    }
}

/* ------------------------------------------------------------------ */
/* rows of the source into rows of the destination                     */
/* ------------------------------------------------------------------ */

static void scaler_open( SCALER *sc, IN *in, int width, int height, int bpc, CSPACE *cs, int kind,
                         ARR *decode, int dw, int dh )
{
    fx comps[MAX_COMPS];
    u32 peak, val;
    int index, col, next;

    mem_set( sc, 0, sizeof( *sc ) );
    sc->in = in;
    sc->width = width;
    sc->height = height;
    sc->bpc = bpc;
    sc->cs = cs;
    sc->kind = kind;
    sc->ncomp = kind == K_COLOUR ? cs->ncomp : 1;
    if ( kind == K_ALPHA && cs ) {
        sc->ncomp = cs->ncomp;                      /* a soft mask written in colour */
    }
    if ( sc->ncomp < 1 ) {
        sc->ncomp = 1;
    }
    sc->chans = kind == K_COLOUR ? 3 : 1;
    sc->dw = dw;
    sc->dh = dh;
    sc->col1 = dw;
    sc->rowbytes = ((u32)width * (u32)sc->ncomp * (u32)bpc + 7) / 8;

    /* the decode array: what the least and the greatest sample mean */
    for ( index = 0; index < sc->ncomp && index < MAX_COMPS; index++ ) {
        sc->dec[index * 2] = 0;
        sc->dec[index * 2 + 1] = FX_ONE;
    }
    if ( kind == K_COLOUR && cs->kind == CS_INDEXED ) {
        sc->dec[1] = I2FX( (1L << (bpc > 8 ? 8 : bpc)) - 1 );
    } else if ( kind == K_COLOUR && cs->kind == CS_LAB ) {
        sc->dec[1] = 100 * FX_ONE;
        sc->dec[2] = cs->range[0];
        sc->dec[3] = cs->range[1];
        sc->dec[4] = cs->range[2];
        sc->dec[5] = cs->range[3];
    }
    if ( decode ) {
        for ( index = 0; index < sc->ncomp * 2 && index < MAX_COMPS * 2 && (u32)index < decode->count; index++ ) {
            sc->dec[index] = obj_fx( arr_get( decode, (u32)index ) );
        }
    }
    sc->invert = sc->dec[0] > sc->dec[1];

    if ( kind == K_COLOUR && sc->ncomp == 1 ) {
        /* one component: every value it can have, worked out once */
        peak = (1UL << (bpc > 8 ? 8 : bpc)) - 1;
        sc->lut = (u32 *)pool_big( doc.page_pool, (peak + 1) * sizeof( u32 ) );
        for ( val = 0; val <= peak; val++ ) {
            comps[0] = sc->dec[0] + mul_div( sc->dec[1] - sc->dec[0], (s32)val, (s32)peak );
            sc->lut[val] = cs_rgb( cs, comps );
        }
    }
    sc->raw = (u8 *)pool_big( doc.page_pool, sc->rowbytes + 8 );
    sc->pix = (u8 *)pool_big( doc.page_pool, (u32)width * 4 + 8 );
    sc->xmap = (u16 *)pool_big( doc.page_pool, ((u32)width + 1) * sizeof( u16 ) );
    sc->xcount = (u16 *)pool_big( doc.page_pool, ((u32)dw + 1) * sizeof( u16 ) );
    sc->acc = (u32 *)pool_big( doc.page_pool, ((u32)dw + 1) * 4 * sizeof( u32 ) );
    sc->done = (u8 *)pool_big( doc.page_pool, ((u32)dw + 1) * 4 );
    for ( col = 0; col <= width; col++ ) {
        sc->xmap[col] = (u16)(((u32)col * (u32)dw) / (u32)width);
    }
    for ( col = 0; col < width; col++ ) {
        next = sc->xmap[col + 1] > sc->xmap[col] ? sc->xmap[col + 1] : sc->xmap[col] + 1;
        for ( index = sc->xmap[col]; index < next && index < dw; index++ ) {
            sc->xcount[index]++;
        }
    }
}

static void scaler_close( SCALER *sc )
{
    if ( sc->in ) {
        in_close( sc->in );
    }
    pool_unbig( sc->lut );
    pool_unbig( sc->raw );
    pool_unbig( sc->pix );
    pool_unbig( sc->xmap );
    pool_unbig( sc->xcount );
    pool_unbig( sc->acc );
    pool_unbig( sc->done );
    mem_set( sc, 0, sizeof( *sc ) );
}

/* The next row of the destination, in sc->done: 0 when the image has
   run out.  "wanted" 0 reads past it without the work of making it. */
static int scaler_next( SCALER *sc, int wanted )
{
    u32 *acc, got, div, last_div = 0, recip = 0;
    const u8 *pix;
    int chans = sc->chans, col, dest, next, chan, row_lo, row_hi;

    if ( sc->repeat > 0 ) {
        sc->repeat--;
        sc->out_row++;
        return 1;
    }
    for ( ;; ) {
        if ( sc->src_row >= sc->height ) {
            return 0;
        }
        got = in_read( sc->in, sc->raw, sc->rowbytes );
        if ( got == 0 && sc->src_row > 0 ) {
            return 0;                               /* the data stopped short */
        }
        if ( got < sc->rowbytes ) {
            mem_set( sc->raw + got, 0, sc->rowbytes - got );
        }
        row_lo = (int)(((u32)sc->src_row * (u32)sc->dh) / (u32)sc->height);
        row_hi = (int)((((u32)sc->src_row + 1) * (u32)sc->dh) / (u32)sc->height);
        sc->src_row++;
        if ( wanted ) {
            decode_row( sc );
            pix = sc->pix;
            if ( sc->rows_acc == 0 ) {
                mem_set( sc->acc + sc->col0 * chans, 0, (u32)(sc->col1 - sc->col0) * (u32)chans * sizeof( u32 ) );
            }
            for ( col = 0; col < sc->width; col++, pix += chans ) {
                dest = sc->xmap[col];
                next = sc->xmap[col + 1] > dest ? sc->xmap[col + 1] : dest + 1;
                for ( ; dest < next; dest++ ) {
                    if ( dest < sc->col0 || dest >= sc->col1 ) {
                        continue;
                    }
                    acc = sc->acc + dest * chans;
                    for ( chan = 0; chan < chans; chan++ ) {
                        acc[chan] += pix[chan];
                    }
                }
            }
            sc->rows_acc++;
        }
        if ( row_hi <= row_lo ) {
            continue;                               /* the next source row lands here too */
        }
        if ( wanted ) {
            for ( dest = sc->col0; dest < sc->col1; dest++ ) {
                div = (u32)sc->xcount[dest] * (u32)sc->rows_acc;
                acc = sc->acc + dest * chans;
                if ( div <= 1 ) {
                    for ( chan = 0; chan < chans; chan++ ) {
                        sc->done[dest * chans + chan] = (u8)acc[chan];
                    }
                    continue;
                }
                if ( div != last_div ) {
                    last_div = div;
                    recip = 65536UL / div;
                }
                for ( chan = 0; chan < chans; chan++ ) {
                    got = (acc[chan] * recip + 0x8000UL) >> 16;
                    sc->done[dest * chans + chan] = (u8)(got > 255 ? 255 : got);
                }
            }
        }
        sc->rows_acc = 0;
        sc->repeat = row_hi - row_lo - 1;
        sc->out_row = row_lo + 1;
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* an image                                                            */
/* ------------------------------------------------------------------ */

static fx fx_abs( fx val )
{
    return val < 0 ? -val : val;
}

/* a patch of grey where an image is that cannot be drawn */
static void placeholder( IMGREQ *req )
{
    PAINT grey;
    const FMAT *fm = req->fm;

    mem_set( &grey, 0, sizeof( grey ) );
    grey.rgb = 0xD0D0D0UL;
    grey.alpha = 255;
    ras_begin();
    ras_line( fm->e, fm->f, fm->e + fm->a, fm->f + fm->b );
    ras_line( fm->e + fm->a, fm->f + fm->b, fm->e + fm->a + fm->c, fm->f + fm->b + fm->d );
    ras_line( fm->e + fm->a + fm->c, fm->f + fm->b + fm->d, fm->e + fm->c, fm->f + fm->d );
    ras_line( fm->e + fm->c, fm->f + fm->d, fm->e, fm->f );
    ras_fill( req->cv, req->clip, 0, &grey );
}

/* The filter that makes the image's pixels, number "last" of the
   holder's, put on "in": NULL if it is one this cannot undo.  The
   depth and the components may be the filter's to say. */
static IN *open_data( OBJ *holder, IN *in, int last, int *bpc, CSPACE **cs, int stencil )
{
    DICT *parms;
    const char *name;

    if ( in == NULL || last < 0 ) {
        return in;
    }
    name = pdf_filter_name( holder, last );
    parms = pdf_filter_parms( holder, last );
    if ( str_cmp( name, "DCTDecode" ) == 0 || str_cmp( name, "DCT" ) == 0 ) {
        in = flt_dct( in, parms );
        if ( in ) {
            *bpc = 8;
            if ( !stencil && (*cs == NULL || (*cs)->ncomp != dct_comps) ) {
                *cs = dct_comps == 1 ? &cs_devgray : (dct_comps == 4 ? &cs_devcmyk : &cs_devrgb);
            }
        }
        return in;
    }
    if ( str_cmp( name, "CCITTFaxDecode" ) == 0 || str_cmp( name, "CCF" ) == 0 ) {
        *bpc = 1;
        return flt_ccitt( in, parms );
    }
    in_close( in );
    return NULL;
}

void image_draw( IMGREQ *req )
{
    static SCALER colour, mask;
    DICT *dict = req->dict, *mdict;
    OBJ holder, *obj, *mobj;
    ARR *decode, *key;
    CSPACE *cs = NULL, *mcs;
    IN *in, *min;
    const FMAT *fm = req->fm;
    PAINT plain;
    u8 *bgr, *alpha, *row, hold;
    fx big;
    int width = (int)obj_int( dict_get2( dict, "Width", "W" ) );
    int height = (int)obj_int( dict_get2( dict, "Height", "H" ) );
    int bpc = (int)obj_int( dict_get2( dict, "BitsPerComponent", "BPC" ) );
    int stencil = (int)obj_int( dict_get2( dict, "ImageMask", "IM" ) );
    int mbpc, side, dw, dh, left, top, flip_x, flip_y, first, last, count, index, ypos, xpos, dest;
    int have_mask = 0, global = req->fill->alpha, filtered;

    /* a page that was given up may have left these as they were */
    mem_set( &colour, 0, sizeof( colour ) );
    mem_set( &mask, 0, sizeof( mask ) );
    if ( width < 1 || height < 1 || width > 32000 || height > 32000 ) {
        return;
    }
    if ( stencil || bpc < 1 ) {
        bpc = stencil ? 1 : 8;
    }
    if ( !stencil ) {
        cs = cs_load( dict_get2( dict, "ColorSpace", "CS" ), req->res );
        if ( cs->kind == CS_PATTERN ) {
            cs = &cs_devgray;
        }
    }
    holder.type = T_DICT;
    holder.u.dict = dict;
    filtered = pdf_filter_count( &holder ) != 0;

    if ( req->cv == NULL || req->clip->x0 >= req->clip->x1 || req->clip->y0 >= req->clip->y1 ) {
        /* nothing will be seen of it; but an inline image's data is in
           the way of what follows, and has to be stepped over */
        if ( req->stm == NULL && !filtered ) {
            in_skip( req->inline_in, (((u32)width * (u32)(cs ? cs->ncomp : 1) * (u32)bpc + 7) / 8) * (u32)height );
        }
        return;
    }

    /* which way it lies: along the rows, along the columns, or neither */
    big = fx_abs( fm->a ) + fx_abs( fm->b ) + fx_abs( fm->c ) + fx_abs( fm->d );
    if ( fx_abs( fm->b ) + fx_abs( fm->c ) <= big / 64 ) {
        side = 0;
        dw = (int)FX_ROUND( fx_abs( fm->a ) );
        dh = (int)FX_ROUND( fx_abs( fm->d ) );
        left = (int)FX_ROUND( fm->a < 0 ? fm->e + fm->a : fm->e );
        top = (int)FX_ROUND( fm->d < 0 ? fm->f + fm->d : fm->f );
        flip_x = fm->a < 0;
        flip_y = fm->d > 0;
    } else if ( fx_abs( fm->a ) + fx_abs( fm->d ) <= big / 64 ) {
        side = 1;
        dw = (int)FX_ROUND( fx_abs( fm->b ) );      /* the source's columns run down the screen */
        dh = (int)FX_ROUND( fx_abs( fm->c ) );
        left = (int)FX_ROUND( fm->c < 0 ? fm->e + fm->c : fm->e );
        top = (int)FX_ROUND( fm->b < 0 ? fm->f + fm->b : fm->f );
        flip_x = fm->c > 0;                         /* the first row is at the far side */
        flip_y = fm->b < 0;
    } else {
        if ( req->stm == NULL && !filtered ) {
            in_skip( req->inline_in, (((u32)width * (u32)(cs ? cs->ncomp : 1) * (u32)bpc + 7) / 8) * (u32)height );
        }
        placeholder( req );
        return;
    }
    if ( dw < 1 ) {
        dw = 1;
    }
    if ( dh < 1 ) {
        dh = 1;
    }
    if ( dw > 16000 || dh > 16000 ) {
        return;
    }

    if ( req->stm ) {
        in = pdf_stream_raw( req->stm, &index );
        in = open_data( req->stm, in, index, &bpc, &cs, stencil );
    } else {
        in = in_borrow( req->inline_in, filtered ? 0xFFFFFFFFUL :
                        (((u32)width * (u32)(cs ? cs->ncomp : 1) * (u32)bpc + 7) / 8) * (u32)height );
        in = pdf_filter_chain( in, &holder, 1, &index );
        in = open_data( &holder, in, index, &bpc, &cs, stencil );
    }
    if ( in == NULL ) {
        placeholder( req );
        return;
    }
    decode = dict_get2( dict, "Decode", "D" )->type == T_ARR ? dict_get2( dict, "Decode", "D" )->u.arr : NULL;
    scaler_open( &colour, in, width, height, bpc, cs, stencil ? K_STENCIL : K_COLOUR, decode, dw, dh );

    /* a mask of its own: soft, a stencil, or a range of colours */
    mobj = dict_get( dict, "SMask" );
    obj = dict_get( dict, "Mask" );
    if ( !stencil && mobj->type != T_STREAM && obj->type == T_STREAM ) {
        mobj = obj;
    }
    if ( !stencil && mobj->type == T_STREAM ) {
        mdict = mobj->u.dict;
        mbpc = (int)dict_int( mdict, "BitsPerComponent", 1 );
        mcs = dict_int( mdict, "ImageMask", 0 ) ? NULL : cs_load( dict_get( mdict, "ColorSpace" ), req->res );
        min = pdf_stream_raw( mobj, &index );
        min = open_data( mobj, min, index, &mbpc, &mcs, mcs == NULL );
        if ( min ) {
            key = dict_arr( mdict, "Decode" );
            scaler_open( &mask, min, (int)dict_int( mdict, "Width", 1 ), (int)dict_int( mdict, "Height", 1 ),
                         mbpc, mcs, mcs ? K_ALPHA : K_STENCIL, key, dw, dh );
            if ( mask.width >= 1 && mask.height >= 1 ) {
                have_mask = 1;
            } else {
                scaler_close( &mask );
            }
        }
    } else if ( !stencil && obj->type == T_ARR ) {
        key = obj->u.arr;
        colour.has_key = 1;
        colour.chans = 4;
        for ( index = 0; index < colour.ncomp * 2 && index < MAX_COMPS * 2; index++ ) {
            colour.key[index] = (int)obj_int( arr_get( key, (u32)index ) );
        }
    }

    /* the part of the destination that the clip lets through */
    if ( side == 0 ) {
        first = req->clip->x0 - left;
        last = req->clip->x1 - left;
    } else {
        first = req->clip->y0 - top;
        last = req->clip->y1 - top;
    }
    if ( side == 0 ? flip_x : flip_y ) {
        index = first;
        first = dw - last;
        last = dw - index;
    }
    if ( first < 0 ) {
        first = 0;
    }
    if ( last > dw ) {
        last = dw;
    }
    count = last - first;
    if ( count > 0 ) {
        colour.col0 = mask.col0 = first;
        colour.col1 = mask.col1 = last;
        bgr = (u8 *)pool_big( doc.page_pool, (u32)dw * 3 + 8 );
        alpha = (u8 *)pool_big( doc.page_pool, (u32)dw + 8 );
        plain = *req->fill;
        for ( dest = 0; dest < dh; dest++ ) {
            /* where this row of the image lands, and whether it shows */
            if ( side == 0 ) {
                ypos = flip_y ? top + dh - 1 - dest : top + dest;
                index = ypos >= req->clip->y0 && ypos < req->clip->y1;
                if ( !index && (flip_y ? ypos < req->clip->y0 : ypos >= req->clip->y1) ) {
                    break;                          /* and none after it will */
                }
            } else {
                xpos = flip_x ? left + dh - 1 - dest : left + dest;
                index = xpos >= req->clip->x0 && xpos < req->clip->x1;
                if ( !index && (flip_x ? xpos < req->clip->x0 : xpos >= req->clip->x1) ) {
                    break;
                }
                ypos = 0;
            }
            if ( !scaler_next( &colour, index ) ) {
                break;
            }
            if ( have_mask && !scaler_next( &mask, index ) ) {
                have_mask = 0;
            }
            if ( !index ) {
                continue;
            }
            /* the row as the canvas wants it: colours, and how much of each */
            row = colour.done + first * colour.chans;
            for ( index = 0; index < count; index++, row += colour.chans ) {
                if ( stencil ) {
                    alpha[index] = row[0];
                } else {
                    bgr[index * 3] = row[0];
                    bgr[index * 3 + 1] = row[1];
                    bgr[index * 3 + 2] = row[2];
                    alpha[index] = colour.has_key ? row[3] : 255;
                }
                if ( have_mask ) {
                    alpha[index] = (u8)((alpha[index] * mask.done[first + index] + 127) / 255);
                }
                if ( global != 255 && !stencil ) {
                    alpha[index] = (u8)((alpha[index] * global + 127) / 255);
                }
            }
            if ( side == 0 ? flip_x : flip_y ) {
                for ( index = 0; index < count / 2; index++ ) {
                    hold = alpha[index];
                    alpha[index] = alpha[count - 1 - index];
                    alpha[count - 1 - index] = hold;
                    for ( xpos = 0; xpos < 3 && !stencil; xpos++ ) {
                        hold = bgr[index * 3 + xpos];
                        bgr[index * 3 + xpos] = bgr[(count - 1 - index) * 3 + xpos];
                        bgr[(count - 1 - index) * 3 + xpos] = hold;
                    }
                }
                xpos = flip_x && side == 0 ? left + dw - last : (side == 0 ? left + first : 0);
                index = side == 1 ? top + dw - last : 0;
            } else {
                xpos = left + first;
                index = top + first;
            }
            if ( side == 0 ) {
                if ( stencil ) {
                    paint_row( req->cv, req->clip, &plain, ypos, xpos, xpos + count, alpha );
                } else {
                    paint_pixels( req->cv, req->clip, ypos, xpos, count, bgr,
                                  have_mask || colour.has_key || global != 255 ? alpha : NULL,
                                  req->fill->blend );
                }
            } else {
                /* on its side: this row of the image is a column of the
                   screen, a pixel at a time */
                xpos = flip_x ? left + dh - 1 - dest : left + dest;
                for ( ypos = 0; ypos < count; ypos++ ) {
                    if ( stencil ) {
                        paint_row( req->cv, req->clip, &plain, index + ypos, xpos, xpos + 1, alpha + ypos );
                    } else {
                        paint_pixels( req->cv, req->clip, index + ypos, xpos, 1, bgr + ypos * 3,
                                      alpha + ypos, req->fill->blend );
                    }
                }
            }
        }
        pool_unbig( bgr );
        pool_unbig( alpha );
    }
    if ( have_mask || mask.raw ) {
        scaler_close( &mask );
    }
    scaler_close( &colour );
}
