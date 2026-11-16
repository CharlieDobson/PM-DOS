/*
 * FILTER.C - the small filters: ASCIIHex, ASCII85, RunLength, LZW, and
 * the predictors that Flate and LZW may have been helped by.
 *
 * Each is an IN whose fill() reads the IN behind it.  INFLATE.C,
 * CCITT.C and DCT.C are the large ones.
 */
#include "pdf.h"

/* ------------------------------------------------------------------ */
/* ASCIIHexDecode                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int ended;
    u8  buf[512];
} AHEX;

static int ahex_fill( IN *in )
{
    AHEX *st = IN_STATE( in, AHEX );
    IN *src = in->src;
    u32 len = 0;
    int ch, val, have = -1;

    while ( len < sizeof( st->buf ) && !st->ended ) {
        ch = IN_GETC( src );
        if ( ch < 0 || ch == '>' ) {
            st->ended = 1;
            if ( have >= 0 ) {
                st->buf[len++] = (u8)(have << 4);
            }
            break;
        }
        if ( ch >= '0' && ch <= '9' ) {
            val = ch - '0';
        } else if ( ch >= 'a' && ch <= 'f' ) {
            val = ch - 'a' + 10;
        } else if ( ch >= 'A' && ch <= 'F' ) {
            val = ch - 'A' + 10;
        } else {
            continue;
        }
        if ( have < 0 ) {
            have = val;
        } else {
            st->buf[len++] = (u8)((have << 4) | val);
            have = -1;
        }
    }
    in->cur = st->buf;
    in->end = st->buf + len;
    return len != 0;
}

IN *flt_ahex( IN *src )
{
    return in_new( sizeof( AHEX ), ahex_fill, src );
}

/* ------------------------------------------------------------------ */
/* ASCII85Decode                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int ended;
    u8  buf[512];
} A85;

static int a85_fill( IN *in )
{
    A85 *st = IN_STATE( in, A85 );
    IN *src = in->src;
    u32 len = 0, word = 0;
    int ch, have = 0, index;

    while ( len + 4 <= sizeof( st->buf ) && !st->ended ) {
        ch = IN_GETC( src );
        if ( ch < 0 || ch == '~' ) {
            st->ended = 1;
            if ( have > 1 ) {               /* a last group, short */
                for ( index = have; index < 5; index++ ) {
                    word = word * 85 + 84;
                }
                for ( index = 0; index < have - 1; index++ ) {
                    st->buf[len++] = (u8)(word >> (24 - index * 8));
                }
            }
            break;
        }
        if ( ch == 'z' && have == 0 ) {
            st->buf[len++] = 0;
            st->buf[len++] = 0;
            st->buf[len++] = 0;
            st->buf[len++] = 0;
            continue;
        }
        if ( ch < '!' || ch > 'u' ) {
            continue;
        }
        word = word * 85 + (u32)(ch - '!');
        if ( ++have == 5 ) {
            st->buf[len++] = (u8)(word >> 24);
            st->buf[len++] = (u8)(word >> 16);
            st->buf[len++] = (u8)(word >> 8);
            st->buf[len++] = (u8)word;
            word = 0;
            have = 0;
        }
    }
    in->cur = st->buf;
    in->end = st->buf + len;
    return len != 0;
}

IN *flt_a85( IN *src )
{
    return in_new( sizeof( A85 ), a85_fill, src );
}

/* ------------------------------------------------------------------ */
/* RunLengthDecode                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int ended;
    u8  buf[512];
} RLE;

static int rle_fill( IN *in )
{
    RLE *st = IN_STATE( in, RLE );
    IN *src = in->src;
    u32 len = 0, count;
    int ch, val;

    while ( len + 128 <= sizeof( st->buf ) && !st->ended ) {
        ch = IN_GETC( src );
        if ( ch < 0 || ch == 128 ) {
            st->ended = 1;
            break;
        }
        if ( ch < 128 ) {
            count = in_read( src, st->buf + len, (u32)ch + 1 );
            len += count;
            if ( count != (u32)ch + 1 ) {
                st->ended = 1;
            }
        } else {
            val = IN_GETC( src );
            if ( val < 0 ) {
                st->ended = 1;
                break;
            }
            mem_set( st->buf + len, val, (u32)(257 - ch) );
            len += (u32)(257 - ch);
        }
    }
    in->cur = st->buf;
    in->end = st->buf + len;
    return len != 0;
}

IN *flt_rle( IN *src )
{
    return in_new( sizeof( RLE ), rle_fill, src );
}

/* ------------------------------------------------------------------ */
/* LZWDecode                                                           */
/* ------------------------------------------------------------------ */

#define LZW_OUT     8192

typedef struct {
    u32 bits;
    int nbits, width, next, prev, early, ended;
    u16 prefix[4096];
    u8  suffix[4096];
    u8  first[4096];                /* the first byte of each string */
    u8  rev[4096];
    u8  buf[LZW_OUT + 4096];
} LZW;

static int lzw_fill( IN *in )
{
    LZW *st = IN_STATE( in, LZW );
    IN *src = in->src;
    u32 len = 0;
    int ch, code, walk, count;

    while ( len < LZW_OUT && !st->ended ) {
        while ( st->nbits < st->width ) {
            ch = IN_GETC( src );
            if ( ch < 0 ) {
                st->ended = 1;
                break;
            }
            st->bits = (st->bits << 8) | (u32)ch;
            st->nbits += 8;
        }
        if ( st->ended ) {
            break;
        }
        code = (int)((st->bits >> (st->nbits - st->width)) & ((1UL << st->width) - 1));
        st->nbits -= st->width;
        if ( code == 256 ) {
            st->width = 9;
            st->next = 258;
            st->prev = -1;
            continue;
        }
        if ( code == 257 ) {
            st->ended = 1;
            break;
        }
        if ( st->prev < 0 ) {
            if ( code > 255 ) {
                st->ended = 1;
                break;
            }
            st->buf[len++] = (u8)code;
            st->prev = code;
            continue;
        }
        if ( code > st->next ) {
            st->ended = 1;                  /* a code not yet made */
            break;
        }
        /* the string for "code", backwards; a code being made right now
           is the previous string and its own first byte */
        count = 0;
        walk = code;
        if ( code == st->next ) {
            st->rev[count++] = st->prev < 256 ? (u8)st->prev : st->first[st->prev];
            walk = st->prev;
        }
        while ( walk > 255 && count < 4095 ) {
            st->rev[count++] = st->suffix[walk];
            walk = st->prefix[walk];
        }
        st->rev[count++] = (u8)walk;
        if ( st->next < 4096 ) {
            st->prefix[st->next] = (u16)st->prev;
            st->suffix[st->next] = (u8)walk;
            st->first[st->next] = st->prev < 256 ? (u8)st->prev : st->first[st->prev];
            st->next++;
        }
        while ( count ) {
            st->buf[len++] = st->rev[--count];
        }
        if ( st->next + st->early >= (1 << st->width) && st->width < 12 ) {
            st->width++;
        }
        st->prev = code;
    }
    in->cur = st->buf;
    in->end = st->buf + len;
    return len != 0;
}

IN *flt_lzw( IN *src, int early )
{
    IN *in = in_new( sizeof( LZW ), lzw_fill, src );
    LZW *st = IN_STATE( in, LZW );

    st->width = 9;
    st->next = 258;
    st->prev = -1;
    st->early = early ? 1 : 0;
    return in;
}

/* ------------------------------------------------------------------ */
/* predictors                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int predictor, bpp, bpc;
    u32 rowlen;
    u8 *prev, *row;
} PRED;

static int paeth( int left, int up, int upleft )
{
    int guess = left + up - upleft;
    int dl = guess > left ? guess - left : left - guess;
    int du = guess > up ? guess - up : up - guess;
    int dul = guess > upleft ? guess - upleft : upleft - guess;

    if ( dl <= du && dl <= dul ) {
        return left;
    }
    return du <= dul ? up : upleft;
}

static int pred_fill( IN *in )
{
    PRED *st = IN_STATE( in, PRED );
    IN *src = in->src;
    u8 *row = st->row, *prev = st->prev, *swap;
    u32 got, at;
    int tag = 0, bpp = st->bpp, left, upleft, sum;

    if ( st->predictor >= 10 ) {
        tag = IN_GETC( src );
        if ( tag < 0 ) {
            return 0;
        }
    }
    got = in_read( src, row, st->rowlen );
    if ( got == 0 ) {
        return 0;
    }
    if ( got < st->rowlen ) {
        mem_set( row + got, 0, st->rowlen - got );
    }
    if ( st->predictor >= 10 ) {
        switch ( tag ) {
        case 1:
            for ( at = (u32)bpp; at < st->rowlen; at++ ) {
                row[at] = (u8)(row[at] + row[at - bpp]);
            }
            break;
        case 2:
            for ( at = 0; at < st->rowlen; at++ ) {
                row[at] = (u8)(row[at] + prev[at]);
            }
            break;
        case 3:
            for ( at = 0; at < st->rowlen; at++ ) {
                left = at >= (u32)bpp ? row[at - bpp] : 0;
                row[at] = (u8)(row[at] + ((left + prev[at]) >> 1));
            }
            break;
        case 4:
            for ( at = 0; at < st->rowlen; at++ ) {
                left = at >= (u32)bpp ? row[at - bpp] : 0;
                upleft = at >= (u32)bpp ? prev[at - bpp] : 0;
                row[at] = (u8)(row[at] + paeth( left, prev[at], upleft ));
            }
            break;
        }
    } else if ( st->bpc == 8 ) {            /* TIFF's: each sample from the one before */
        for ( at = (u32)bpp; at < st->rowlen; at++ ) {
            row[at] = (u8)(row[at] + row[at - bpp]);
        }
    } else if ( st->bpc == 16 ) {
        for ( at = (u32)bpp; at + 1 < st->rowlen; at += 2 ) {
            sum = ((row[at] << 8) | row[at + 1]) + ((row[at - bpp] << 8) | row[at - bpp + 1]);
            row[at] = (u8)(sum >> 8);
            row[at + 1] = (u8)sum;
        }
    }
    in->cur = row;
    in->end = row + got;
    swap = st->prev;
    st->prev = st->row;
    st->row = swap;
    return 1;
}

IN *flt_predict( IN *src, int predictor, int colors, int bpc, int columns )
{
    IN *in;
    PRED *st;
    u32 rowlen;

    if ( colors < 1 || colors > 32 || columns < 1 || columns > 1000000L ||
         (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) ||
         (predictor != 2 && predictor < 10) ) {
        return src;
    }
    rowlen = ((u32)columns * (u32)colors * (u32)bpc + 7) / 8;
    in = in_new( sizeof( PRED ) + rowlen * 2, pred_fill, src );
    st = IN_STATE( in, PRED );
    st->predictor = predictor;
    st->bpc = bpc;
    st->bpp = (colors * bpc + 7) / 8;
    st->rowlen = rowlen;
    st->prev = (u8 *)(st + 1);
    st->row = st->prev + rowlen;
    return in;
}
