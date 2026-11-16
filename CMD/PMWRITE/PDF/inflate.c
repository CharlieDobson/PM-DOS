/*
 * INFLATE.C - FlateDecode: zlib's deflate, undone.
 *
 * The 32K window that a match copies out of is also the buffer the
 * reader is handed, so nothing is copied twice: each call decodes on
 * from where the window stands to its end, the reader takes that, and
 * the next call starts again at the front.  A match that runs past the
 * end is finished by the call after.
 *
 * Codes are looked up nine bits at a time; a longer one is found by
 * its length, as the canonical ordering allows.
 */
#include "pdf.h"

#define WSIZE       32768u
#define FAST_BITS   9
#define FAST_MASK   ((1 << FAST_BITS) - 1)

typedef struct {
    u16 fast[1 << FAST_BITS];       /* length << 9 | symbol, 0 if longer */
    u16 firstcode[16];
    s32 maxcode[17];
    u16 firstsymbol[16];
    u8  size[288];
    u16 value[288];
} ZHUFF;

#define M_HEAD      0
#define M_STORED    1
#define M_CODES     2
#define M_DONE      3

typedef struct {
    u32   bitbuf;
    int   nbits;
    int   mode, last, started;
    int   src_ended;
    u32   stored_left;
    u32   copy_len, copy_dist;      /* a match not yet all copied */
    u32   wpos;
    ZHUFF zlen, zdist;
    u8    window[WSIZE];
} INFL;

static const u16 len_base[31] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
    131, 163, 195, 227, 258, 0, 0 };
static const u8 len_extra[31] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0, 0 };
static const u16 dist_base[32] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
    2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 0, 0 };
static const u8 dist_extra[32] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
    13, 13, 0, 0 };

static int bit_reverse( int val, int bits )
{
    int out = 0, index;

    for ( index = 0; index < bits; index++ ) {
        out = (out << 1) | (val & 1);
        val >>= 1;
    }
    return out;
}

static int huff_build( ZHUFF *huff, const u8 *sizes, int count )
{
    int next_code[16], counts[17];
    int index, code = 0, sym = 0, len, slot, rev;

    mem_set( counts, 0, sizeof( counts ) );
    mem_set( huff->fast, 0, sizeof( huff->fast ) );
    for ( index = 0; index < count; index++ ) {
        counts[sizes[index]]++;
    }
    counts[0] = 0;
    for ( index = 1; index < 16; index++ ) {
        next_code[index] = code;
        huff->firstcode[index] = (u16)code;
        huff->firstsymbol[index] = (u16)sym;
        code += counts[index];
        if ( counts[index] && code - 1 >= (1 << index) ) {
            return 0;
        }
        huff->maxcode[index] = (s32)code << (16 - index);
        code <<= 1;
        sym += counts[index];
    }
    huff->maxcode[16] = 0x10000L;
    for ( index = 0; index < count; index++ ) {
        len = sizes[index];
        if ( len == 0 ) {
            continue;
        }
        slot = next_code[len] - huff->firstcode[len] + huff->firstsymbol[len];
        huff->size[slot] = (u8)len;
        huff->value[slot] = (u16)index;
        if ( len <= FAST_BITS ) {
            for ( rev = bit_reverse( next_code[len], len ); rev < (1 << FAST_BITS);
                  rev += 1 << len ) {
                huff->fast[rev] = (u16)((len << 9) | index);
            }
        }
        next_code[len]++;
    }
    return 1;
}

static void need_bits( INFL *st, IN *src, int count )
{
    int ch;

    while ( st->nbits < count ) {
        ch = IN_GETC( src );
        if ( ch < 0 ) {
            ch = 0;
            st->src_ended++;
        }
        st->bitbuf |= (u32)ch << st->nbits;
        st->nbits += 8;
    }
}

static u32 get_bits( INFL *st, IN *src, int count )
{
    u32 val;

    if ( count == 0 ) {
        return 0;
    }
    need_bits( st, src, count );
    val = st->bitbuf & ((1UL << count) - 1);
    st->bitbuf >>= count;
    st->nbits -= count;
    return val;
}

static int huff_decode( INFL *st, IN *src, ZHUFF *huff )
{
    int entry, len, rev, slot;

    need_bits( st, src, 16 );
    entry = huff->fast[st->bitbuf & FAST_MASK];
    if ( entry ) {
        len = entry >> 9;
        st->bitbuf >>= len;
        st->nbits -= len;
        return entry & 511;
    }
    rev = bit_reverse( (int)(st->bitbuf & 0xFFFF), 16 );
    for ( len = FAST_BITS + 1; rev >= huff->maxcode[len]; len++ ) {
    }
    if ( len >= 16 ) {
        return -1;
    }
    slot = (rev >> (16 - len)) - huff->firstcode[len] + huff->firstsymbol[len];
    if ( slot < 0 || slot >= 288 || huff->size[slot] != len ) {
        return -1;
    }
    st->bitbuf >>= len;
    st->nbits -= len;
    return huff->value[slot];
}

static int dynamic_tables( INFL *st, IN *src )
{
    static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    ZHUFF *codes = &st->zdist;      /* borrowed: it is built last */
    u8 lens[288 + 32 + 138], clens[19];
    int hlit = (int)get_bits( st, src, 5 ) + 257;
    int hdist = (int)get_bits( st, src, 5 ) + 1;
    int hclen = (int)get_bits( st, src, 4 ) + 4;
    int index, sym, fill, repeat, total = hlit + hdist;

    mem_set( clens, 0, sizeof( clens ) );
    for ( index = 0; index < hclen; index++ ) {
        clens[order[index]] = (u8)get_bits( st, src, 3 );
    }
    if ( !huff_build( codes, clens, 19 ) ) {
        return 0;
    }
    for ( index = 0; index < total; ) {
        sym = huff_decode( st, src, codes );
        if ( sym < 0 || sym >= 19 ) {
            return 0;
        }
        if ( sym < 16 ) {
            lens[index++] = (u8)sym;
            continue;
        }
        fill = 0;
        if ( sym == 16 ) {
            if ( index == 0 ) {
                return 0;
            }
            repeat = (int)get_bits( st, src, 2 ) + 3;
            fill = lens[index - 1];
        } else if ( sym == 17 ) {
            repeat = (int)get_bits( st, src, 3 ) + 3;
        } else {
            repeat = (int)get_bits( st, src, 7 ) + 11;
        }
        if ( index + repeat > total ) {
            return 0;
        }
        mem_set( lens + index, fill, (u32)repeat );
        index += repeat;
    }
    return huff_build( &st->zlen, lens, hlit ) && huff_build( &st->zdist, lens + hlit, hdist );
}

static void fixed_tables( INFL *st )
{
    u8 lens[288];

    mem_set( lens, 8, 144 );
    mem_set( lens + 144, 9, 112 );
    mem_set( lens + 256, 7, 24 );
    mem_set( lens + 280, 8, 8 );
    huff_build( &st->zlen, lens, 288 );
    mem_set( lens, 5, 32 );
    huff_build( &st->zdist, lens, 32 );
}

static int infl_fill( IN *in )
{
    INFL *st = IN_STATE( in, INFL );
    IN *src = in->src;
    u8 *win = st->window;
    u32 wpos, start, from, count;
    int sym, ch, first, second;

    if ( !st->started ) {
        /* zlib's two bytes, if they are there: a stream without them is
           read as bare deflate, which is what some writers produce */
        st->started = 1;
        first = IN_GETC( src );
        second = IN_GETC( src );
        if ( first < 0 || second < 0 ) {
            return 0;
        }
        if ( (first & 0x0F) != 8 || ((first << 8) | second) % 31 != 0 ) {
            st->bitbuf = (u32)first | ((u32)second << 8);
            st->nbits = 16;
        } else if ( second & 0x20 ) {
            in_skip( src, 4 );
        }
    }
    if ( st->wpos >= WSIZE ) {
        st->wpos = 0;
    }
    wpos = start = st->wpos;

    while ( wpos < WSIZE && st->mode != M_DONE ) {
        if ( st->copy_len ) {
            from = (wpos - st->copy_dist) & (WSIZE - 1);
            do {
                win[wpos++] = win[from];
                from = (from + 1) & (WSIZE - 1);
            } while ( --st->copy_len && wpos < WSIZE );
            continue;
        }
        if ( st->src_ended > 8 ) {
            st->mode = M_DONE;              /* the data stopped short */
            break;
        }
        switch ( st->mode ) {
        case M_HEAD:
            if ( st->last ) {
                st->mode = M_DONE;
                break;
            }
            st->last = (int)get_bits( st, src, 1 );
            switch ( get_bits( st, src, 2 ) ) {
            case 0:
                st->bitbuf >>= st->nbits & 7;       /* to a byte boundary */
                st->nbits &= ~7;
                st->stored_left = get_bits( st, src, 16 );
                get_bits( st, src, 16 );
                st->mode = M_STORED;
                break;
            case 1:
                fixed_tables( st );
                st->mode = M_CODES;
                break;
            case 2:
                st->mode = dynamic_tables( st, src ) ? M_CODES : M_DONE;
                break;
            default:
                st->mode = M_DONE;
                break;
            }
            break;

        case M_STORED:
            if ( st->stored_left == 0 ) {
                st->mode = M_HEAD;
                break;
            }
            if ( st->nbits ) {              /* what the bit reader had taken */
                win[wpos++] = (u8)get_bits( st, src, 8 );
                st->stored_left--;
                break;
            }
            count = st->stored_left < WSIZE - wpos ? st->stored_left : WSIZE - wpos;
            count = in_read( src, win + wpos, count );
            if ( count == 0 ) {
                st->mode = M_DONE;
                break;
            }
            wpos += count;
            st->stored_left -= count;
            break;

        case M_CODES:
            /* the hot loop: literals until the window is full or a
               match comes */
            for ( ;; ) {
                if ( st->nbits < 16 ) {
                    need_bits( st, src, 16 );
                }
                sym = st->zlen.fast[st->bitbuf & FAST_MASK];
                if ( sym ) {
                    ch = sym >> 9;
                    st->bitbuf >>= ch;
                    st->nbits -= ch;
                    sym &= 511;
                } else {
                    sym = huff_decode( st, src, &st->zlen );
                }
                if ( sym >= 256 || sym < 0 ) {
                    break;
                }
                win[wpos++] = (u8)sym;
                if ( wpos == WSIZE ) {
                    sym = -2;
                    break;
                }
            }
            if ( sym == -2 ) {
                break;
            }
            if ( sym == 256 ) {
                st->mode = M_HEAD;
                break;
            }
            if ( sym < 0 || sym > 285 ) {
                st->mode = M_DONE;
                break;
            }
            sym -= 257;
            st->copy_len = len_base[sym] + get_bits( st, src, len_extra[sym] );
            sym = huff_decode( st, src, &st->zdist );
            if ( sym < 0 || sym > 29 ) {
                st->copy_len = 0;
                st->mode = M_DONE;
                break;
            }
            st->copy_dist = dist_base[sym] + get_bits( st, src, dist_extra[sym] );
            break;
        }
    }
    st->wpos = wpos;
    in->cur = win + start;
    in->end = win + wpos;
    return wpos > start;
}

IN *flt_inflate( IN *src )
{
    return in_new( sizeof( INFL ), infl_fill, src );
}
