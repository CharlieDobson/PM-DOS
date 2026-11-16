/*
 * FLATE.C - DEFLATE, read and written, memory to memory (FLATE.H).
 *
 * READING.  The stream is decoded straight into the block it is to
 * fill, so that block is also the window a match is copied from, and
 * no second copy of anything is kept.  A Huffman code is looked up
 * nine bits at a time in a table; the few codes longer than that are
 * walked a bit at a time through the counts of codes of each length,
 * which is all a canonical code needs.  Every table is built from the
 * lengths in the file and refused if they do not make a code: more
 * codes than the lengths have room for, or - but for the one-code
 * tables an encoder with a single distance writes - fewer.  A match
 * that reaches back before the first byte, a length or distance code
 * that does not exist, and a stream that stops early all end the
 * decoding with "damaged"; nothing is read past the input or written
 * past the output.
 *
 * WRITING.  The whole input is in memory, so there is no window to
 * slide: "head" has the newest place each three-byte hash was seen and
 * "prev" how far back the one before it is, for the last 32K places.
 * The parse is the lazy one every DEFLATE writer since PKZIP 2 uses -
 * a match is held back one byte to see whether the next place starts a
 * longer one.  THE SETTING is one a 486 can live with: chains followed
 * 64 deep, a quarter of that once 8 bytes are in hand, and no second
 * look at all past 16.  Symbols are gathered 16K at a time and each
 * block goes out the smallest of three ways: stored, with the fixed
 * codes, or with codes made for it.
 *
 * THE CODES are made without a sort from the library: the symbols in
 * use are put in order of frequency (a shell sort of at most 286) and
 * merged with two queues, which is all Huffman's method needs once
 * they are in order.  Lengths over the format's limit are cut to it
 * and the code made whole again by lengthening the cheapest others.
 *
 * MEMORY.  Decoding takes 4K; encoding about 250K, in one block that
 * is given back before the answer is.
 */
#include "rt.h"
#include "crc.h"
#include "flate.h"

#define RD16( ptr )     ((u32)(ptr)[0] | ((u32)(ptr)[1] << 8))

#define MAX_BITS        15
#define LIT_CODES       288     /* with the two that are never used */
#define DIST_CODES      32      /* likewise */
#define END_BLOCK       256

static const u16 len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
    115, 131, 163, 195, 227, 258
};
static const u8 len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const u16 dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
    1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const u8 dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
    12, 13, 13
};
/* the order a block's header gives the lengths of the code-length code in */
static const u8 clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static u32 bit_reverse( u32 code, int len )
{
    u32 back = 0;

    while ( len-- ) {
        back = (back << 1) | (code & 1);
        code >>= 1;
    }
    return back;
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

#define FAST_BITS       9
#define FAST_SIZE       (1 << FAST_BITS)

typedef struct {
    u16 fast[FAST_SIZE];        /* length << 9 | symbol; 0: a longer code */
    u16 count[MAX_BITS + 1];    /* how many codes of each length */
    u16 symbol[LIT_CODES];      /* the symbols in the order of their codes */
} HUFF;

typedef struct {
    const u8 *src;
    u32   src_len, pos;
    u32   bits;                 /* taken from src and not used yet */
    u32   nbits;
    u8   *dst;
    u32   used, cap;
    int   grows;                /* dst is a heap block: it may be made bigger */
    u32   max;                  /* ...up to this */
    HUFF  lit, dist;
    u8    lens[LIT_CODES + DIST_CODES];
} INFL;

/*
 * A decoding table from code lengths.  0: they are not a code.
 * Complete is the rule; a table with no code at all, or with one code
 * of one bit, is let through (the distance table of a block with no
 * match or one distance) unless "whole" asks for a complete one.
 */
static int huff_build( HUFF *huff, const u8 *lens, int count, int whole )
{
    u16 start[MAX_BITS + 2];
    u32 code, fill;
    int len, sym, left, index, codes = 0, from;

    for ( len = 0; len <= MAX_BITS; len++ ) {
        huff->count[len] = 0;
    }
    for ( sym = 0; sym < count; sym++ ) {
        if ( lens[sym] > MAX_BITS ) {
            return 0;
        }
        huff->count[lens[sym]]++;
    }
    codes = count - huff->count[0];
    huff->count[0] = 0;
    left = 1;
    for ( len = 1; len <= MAX_BITS; len++ ) {
        left = left * 2 - (int)huff->count[len];
        if ( left < 0 ) {
            return 0;                   /* more codes than there is room for */
        }
    }
    if ( left > 0 && (whole || codes > 1 || (codes == 1 && huff->count[1] != 1)) ) {
        return 0;                       /* fewer */
    }
    start[1] = 0;
    for ( len = 1; len <= MAX_BITS; len++ ) {
        start[len + 1] = (u16)(start[len] + huff->count[len]);
    }
    for ( sym = 0; sym < count; sym++ ) {
        if ( lens[sym] ) {
            huff->symbol[start[lens[sym]]++] = (u16)sym;
        }
    }
    mem_set( huff->fast, 0, sizeof( huff->fast ) );
    code = 0;
    index = 0;
    for ( len = 1; len <= FAST_BITS; len++ ) {
        for ( from = huff->count[len]; from > 0; from-- ) {
            for ( fill = bit_reverse( code, len ); fill < FAST_SIZE; fill += 1UL << len ) {
                huff->fast[fill] = (u16)(((u32)len << FAST_BITS) | huff->symbol[index]);
            }
            index++;
            code++;
        }
        code <<= 1;
    }
    return 1;
}

static void infl_fill( INFL *infl )
{
    while ( infl->nbits <= 24 && infl->pos < infl->src_len ) {
        infl->bits |= (u32)infl->src[infl->pos++] << infl->nbits;
        infl->nbits += 8;
    }
}

/* the next "count" bits, 16 at most; -1 when the input ends first */
static s32 infl_bits( INFL *infl, u32 count )
{
    u32 value;

    if ( infl->nbits < count ) {
        infl_fill( infl );
        if ( infl->nbits < count ) {
            return -1;
        }
    }
    value = infl->bits & ((1UL << count) - 1);
    infl->bits >>= count;
    infl->nbits -= count;
    return (s32)value;
}

/* the next symbol; -1 for bits that are no code, or no more input */
static int huff_decode( INFL *infl, const HUFF *huff )
{
    u32 entry, len, code, first, index, count, bits;

    if ( infl->nbits < MAX_BITS ) {
        infl_fill( infl );
    }
    entry = huff->fast[infl->bits & (FAST_SIZE - 1)];
    if ( entry ) {
        len = entry >> FAST_BITS;
        if ( len > infl->nbits ) {
            return -1;
        }
        infl->bits >>= len;
        infl->nbits -= len;
        return (int)(entry & (FAST_SIZE - 1));
    }
    code = first = index = 0;
    bits = infl->bits;
    for ( len = 1; len <= MAX_BITS; len++ ) {
        code |= bits & 1;
        bits >>= 1;
        count = huff->count[len];
        if ( code < first + count ) {
            if ( len > infl->nbits ) {
                return -1;
            }
            infl->bits >>= len;
            infl->nbits -= len;
            return (int)huff->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* room for "need" more bytes of output */
static int infl_room( INFL *infl, u32 need )
{
    u32 want;
    u8 *fresh;

    if ( need <= infl->cap - infl->used ) {
        return 1;
    }
    if ( !infl->grows || need > infl->max - infl->used ) {
        return 0;
    }
    want = infl->cap < 4096 ? 4096 : infl->cap > infl->max / 2 ? infl->max : infl->cap * 2;
    if ( want < infl->used + need ) {
        want = infl->used + need;
    }
    if ( want > infl->max ) {
        want = infl->max;
    }
    fresh = (u8 *)try_realloc( infl->dst, want );
    if ( fresh == NULL ) {
        return 0;
    }
    infl->dst = fresh;
    infl->cap = want;
    return 1;
}

static int infl_stored( INFL *infl )
{
    u32 len;

    /* back to whole bytes: what is left of the byte the bits stopped
       in is dropped, and the bytes taken after it are not taken */
    infl->nbits -= infl->nbits & 7;
    infl->pos -= infl->nbits >> 3;
    infl->bits = 0;
    infl->nbits = 0;
    if ( infl->src_len - infl->pos < 4 ) {
        return 0;
    }
    len = RD16( infl->src + infl->pos );
    if ( (len ^ 0xFFFF) != RD16( infl->src + infl->pos + 2 ) ) {
        return 0;
    }
    infl->pos += 4;
    if ( len > infl->src_len - infl->pos || !infl_room( infl, len ) ) {
        return 0;
    }
    mem_cpy( infl->dst + infl->used, infl->src + infl->pos, len );
    infl->used += len;
    infl->pos += len;
    return 1;
}

static int infl_fixed( INFL *infl )
{
    mem_set( infl->lens, 8, 144 );
    mem_set( infl->lens + 144, 9, 112 );
    mem_set( infl->lens + 256, 7, 24 );
    mem_set( infl->lens + 280, 8, 8 );
    mem_set( infl->lens + LIT_CODES, 5, DIST_CODES );
    return huff_build( &infl->lit, infl->lens, LIT_CODES, 1 ) &&
           huff_build( &infl->dist, infl->lens + LIT_CODES, DIST_CODES, 1 );
}

/* a block's own two codes, from its header */
static int infl_dynamic( INFL *infl )
{
    s32 hlit, hdist, hclen, value;
    u32 index, total, repeat;
    int sym, fill;

    hlit = infl_bits( infl, 5 );
    hdist = infl_bits( infl, 5 );
    hclen = infl_bits( infl, 4 );
    if ( hlit < 0 || hdist < 0 || hclen < 0 ) {
        return 0;
    }
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if ( hlit > 286 || hdist > 30 ) {
        return 0;
    }
    mem_set( infl->lens, 0, 19 );
    for ( index = 0; index < (u32)hclen; index++ ) {
        value = infl_bits( infl, 3 );
        if ( value < 0 ) {
            return 0;
        }
        infl->lens[clen_order[index]] = (u8)value;
    }
    /* the code the lengths are written in borrows the literals' table */
    if ( !huff_build( &infl->lit, infl->lens, 19, 1 ) ) {
        return 0;
    }
    total = (u32)hlit + (u32)hdist;
    index = 0;
    while ( index < total ) {
        sym = huff_decode( infl, &infl->lit );
        if ( sym < 0 ) {
            return 0;
        }
        if ( sym < 16 ) {
            infl->lens[index++] = (u8)sym;
            continue;
        }
        fill = 0;
        if ( sym == 16 ) {
            if ( index == 0 ) {
                return 0;               /* a repeat of nothing */
            }
            fill = infl->lens[index - 1];
            value = infl_bits( infl, 2 );
            repeat = 3;
        } else if ( sym == 17 ) {
            value = infl_bits( infl, 3 );
            repeat = 3;
        } else {
            value = infl_bits( infl, 7 );
            repeat = 11;
        }
        if ( value < 0 ) {
            return 0;
        }
        repeat += (u32)value;
        if ( repeat > total - index ) {
            return 0;
        }
        while ( repeat-- ) {
            infl->lens[index++] = (u8)fill;
        }
    }
    if ( infl->lens[END_BLOCK] == 0 ) {
        return 0;                       /* a block that could not end */
    }
    return huff_build( &infl->dist, infl->lens + hlit, (int)hdist, 0 ) &&
           huff_build( &infl->lit, infl->lens, (int)hlit, 0 );
}

/* literals and matches to the end of a block */
static int infl_codes( INFL *infl )
{
    u8 *out;
    const u8 *from;
    u32 len, dist;
    s32 extra;
    int sym;

    for ( ;; ) {
        sym = huff_decode( infl, &infl->lit );
        if ( sym < 0 ) {
            return 0;
        }
        if ( sym < END_BLOCK ) {
            if ( infl->used == infl->cap && !infl_room( infl, 1 ) ) {
                return 0;
            }
            infl->dst[infl->used++] = (u8)sym;
            continue;
        }
        if ( sym == END_BLOCK ) {
            return 1;
        }
        sym -= END_BLOCK + 1;
        if ( sym >= 29 ) {
            return 0;
        }
        extra = infl_bits( infl, len_extra[sym] );
        if ( extra < 0 ) {
            return 0;
        }
        len = len_base[sym] + (u32)extra;
        sym = huff_decode( infl, &infl->dist );
        if ( sym < 0 || sym >= 30 ) {
            return 0;
        }
        extra = infl_bits( infl, dist_extra[sym] );
        if ( extra < 0 ) {
            return 0;
        }
        dist = dist_base[sym] + (u32)extra;
        if ( dist > infl->used ) {
            return 0;                   /* from before the first byte */
        }
        if ( len > infl->cap - infl->used && !infl_room( infl, len ) ) {
            return 0;
        }
        out = infl->dst + infl->used;
        from = out - dist;
        infl->used += len;
        if ( dist >= len ) {
            mem_cpy( out, from, len );
        } else {
            /* the match runs into itself: a byte at a time repeats it */
            while ( len-- ) {
                *out++ = *from++;
            }
        }
    }
}

static int infl_run( INFL *infl )
{
    s32 last, type;
    int ok;

    do {
        last = infl_bits( infl, 1 );
        type = infl_bits( infl, 2 );
        if ( last < 0 || type < 0 ) {
            return 0;
        }
        if ( type == 0 ) {
            ok = infl_stored( infl );
        } else if ( type == 1 ) {
            ok = infl_fixed( infl ) && infl_codes( infl );
        } else if ( type == 2 ) {
            ok = infl_dynamic( infl ) && infl_codes( infl );
        } else {
            ok = 0;
        }
        if ( !ok ) {
            return 0;
        }
    } while ( !last );
    /* whole bytes that were taken and not used were not the stream's */
    infl->pos -= infl->nbits >> 3;
    return 1;
}

/*
 * The one decoder behind the four calls.  "dst" NULL: a heap block is
 * made, no bigger than "cap", and *made is it.  -> 1, and how many
 * bytes came out and how many of the input were the stream.
 */
static int infl_all( const u8 *src, u32 src_len, u8 *dst, u32 cap, u8 **made, u32 *out_len,
                     u32 *taken )
{
    INFL *infl = (INFL *)try_alloc( sizeof( INFL ) );
    int ok;

    if ( infl == NULL ) {
        return 0;
    }
    mem_set( infl, 0, sizeof( INFL ) );
    infl->src = src;
    infl->src_len = src_len;
    if ( dst != NULL ) {
        infl->dst = dst;
        infl->cap = cap;
    } else {
        infl->grows = 1;
        infl->max = cap;
    }
    ok = infl_run( infl );
    if ( ok && infl->grows && infl->dst == NULL ) {
        /* a stream of no bytes is still an answer, and NULL is not one */
        infl->dst = (u8 *)try_alloc( 1 );
        ok = infl->dst != NULL;
    }
    if ( ok ) {
        *out_len = infl->used;
        *taken = infl->pos;
        if ( infl->grows ) {
            *made = infl->dst;
        }
    } else if ( infl->grows ) {
        xfree( infl->dst );
    }
    xfree( infl );
    return ok;
}

int inflate_mem( const u8 *src, u32 src_len, u8 *dst, u32 dst_cap, u32 *dst_len )
{
    u32 taken;

    *dst_len = 0;
    if ( dst == NULL ) {
        return 0;
    }
    return infl_all( src, src_len, dst, dst_cap, NULL, dst_len, &taken );
}

u8 *inflate_alloc( const u8 *src, u32 src_len, u32 *out_len, u32 max_len )
{
    u8 *made = NULL;
    u32 taken;

    *out_len = 0;
    if ( !infl_all( src, src_len, NULL, max_len, &made, out_len, &taken ) ) {
        return NULL;
    }
    return made;
}

/* a zlib stream's two bytes: DEFLATE, a window of 32K at most, the
   check bits right, and no preset dictionary */
static int zlib_header( const u8 *src, u32 src_len )
{
    return src_len >= 6 && (src[0] & 0x0F) == 8 && (src[0] >> 4) <= 7 &&
           (((u32)src[0] << 8) | src[1]) % 31 == 0 && (src[1] & 0x20) == 0;
}

static int zlib_sum_ok( const u8 *src, u32 src_len, u32 taken, const u8 *data, u32 len )
{
    const u8 *sum = src + 2 + taken;

    if ( src_len - 2 - taken < 4 ) {
        return 0;
    }
    return adler32_update( 1, data, len ) ==
           (((u32)sum[0] << 24) | ((u32)sum[1] << 16) | ((u32)sum[2] << 8) | (u32)sum[3]);
}

int zlib_inflate_mem( const u8 *src, u32 src_len, u8 *dst, u32 dst_cap, u32 *dst_len )
{
    u32 taken;

    *dst_len = 0;
    if ( dst == NULL || !zlib_header( src, src_len ) ||
         !infl_all( src + 2, src_len - 2, dst, dst_cap, NULL, dst_len, &taken ) ) {
        return 0;
    }
    if ( !zlib_sum_ok( src, src_len, taken, dst, *dst_len ) ) {
        *dst_len = 0;
        return 0;
    }
    return 1;
}

u8 *zlib_inflate_alloc( const u8 *src, u32 src_len, u32 *out_len, u32 max_len )
{
    u8 *made = NULL;
    u32 taken;

    *out_len = 0;
    if ( !zlib_header( src, src_len ) ||
         !infl_all( src + 2, src_len - 2, NULL, max_len, &made, out_len, &taken ) ) {
        return NULL;
    }
    if ( !zlib_sum_ok( src, src_len, taken, made, *out_len ) ) {
        xfree( made );
        *out_len = 0;
        return NULL;
    }
    return made;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

#define WSIZE           32768UL
#define WMASK           (WSIZE - 1)
#define MIN_MATCH       3
#define MAX_MATCH       258
#define HASH_BITS       15
#define HASH_SIZE       (1UL << HASH_BITS)
#define HASH3( ptr ) \
    ((((u32)(ptr)[0] << 10) ^ ((u32)(ptr)[1] << 5) ^ (u32)(ptr)[2]) & (HASH_SIZE - 1))
#define MAX_CHAIN       64
#define GOOD_MATCH      8       /* this much in hand: a quarter of the chain */
#define LAZY_MATCH      16      /* this much in hand: the next place is not tried */
#define NICE_MATCH      128     /* this much found: that will do */
#define TOO_FAR         4096    /* three bytes from further back cost more than they save */
#define BLOCK_SYMS      16384UL
#define L_CODES         286
#define D_CODES         30
#define BL_CODES        19
#define MAX_BL_BITS     7
#define STORED_MAX      65535UL

typedef struct {
    u16 freq[LIT_CODES];
    u16 code[LIT_CODES];        /* as it goes out: first bit lowest */
    u8  len[LIT_CODES];
} TREE;

typedef struct {
    const u8 *src;
    u32   len;
    u8   *out;
    u32   used, cap;
    int   failed;               /* no memory for the output */
    u32   bits;
    u32   nbits;
    u32  *head;                 /* by hash: the newest place + 1, 0 for none */
    u16  *prev;                 /* by place: how far back the one before is */
    u16  *sym_dist;             /* a block's symbols: 0 for a literal */
    u8   *sym_lc;               /* ...the literal, or the length less 3 */
    u32   nsym;
    TREE  lit, dist, bl, fix_lit, fix_dist;
    u8    len_code[256];        /* a length less 3 -> its code less 257 */
    u8    dist_code[512];
    /* the code builder's */
    u32   weight[2 * LIT_CODES];
    u16   parent[2 * LIT_CODES];
    u16   depth[2 * LIT_CODES];
    u16   order[LIT_CODES];
    /* both codes' lengths as a block's header says them */
    u8    seq[L_CODES + D_CODES];
    u8    seq_extra[L_CODES + D_CODES];
    u32   nseq;
} DEFL;

#define DIST_CODE( defl, dist ) \
    ((dist) <= 256 ? (defl)->dist_code[(dist) - 1] : (defl)->dist_code[256 + (((dist) - 1) >> 7)])

static int defl_room( DEFL *defl, u32 need )
{
    u32 want;
    u8 *fresh;

    if ( defl->failed ) {
        return 0;
    }
    if ( need <= defl->cap - defl->used ) {
        return 1;
    }
    if ( need > 0x7FFFFFFFUL - defl->used || defl->cap > 0x3FFFFFFFUL ) {
        defl->failed = 1;
        return 0;
    }
    want = defl->cap * 2;
    if ( want < defl->used + need ) {
        want = defl->used + need;
    }
    fresh = (u8 *)try_realloc( defl->out, want );
    if ( fresh == NULL ) {
        defl->failed = 1;
        return 0;
    }
    defl->out = fresh;
    defl->cap = want;
    return 1;
}

/* "count" bits, 16 at most.  The caller has made room. */
static void defl_put( DEFL *defl, u32 value, u32 count )
{
    defl->bits |= value << defl->nbits;
    defl->nbits += count;
    while ( defl->nbits >= 8 ) {
        defl->out[defl->used++] = (u8)defl->bits;
        defl->bits >>= 8;
        defl->nbits -= 8;
    }
}

/* the codes that go with a tree's lengths */
static void defl_codes( TREE *tree, int count )
{
    u16 next[MAX_BITS + 2];
    u16 per[MAX_BITS + 1];
    u32 code = 0;
    int bits, sym;

    for ( bits = 0; bits <= MAX_BITS; bits++ ) {
        per[bits] = 0;
    }
    for ( sym = 0; sym < count; sym++ ) {
        per[tree->len[sym]]++;
    }
    per[0] = 0;
    for ( bits = 1; bits <= MAX_BITS; bits++ ) {
        code = (code + per[bits - 1]) << 1;
        next[bits] = (u16)code;
    }
    for ( sym = 0; sym < count; sym++ ) {
        tree->code[sym] = tree->len[sym] ?
                          (u16)bit_reverse( next[tree->len[sym]]++, tree->len[sym] ) : 0;
    }
}

/* what a symbol weighs: one that is only there to make up the two a
   tree must have counts as seen once */
#define WEIGH( tree, sym )  ((tree)->freq[sym] ? (u32)(tree)->freq[sym] : 1UL)
#define LIGHTER( tree, one, two ) \
    (WEIGH( tree, one ) < WEIGH( tree, two ) || \
     (WEIGH( tree, one ) == WEIGH( tree, two ) && (one) < (two)))

/*
 * A tree's lengths and codes from its frequencies, no code longer than
 * "max_bits".
 */
static void defl_tree( DEFL *defl, TREE *tree, int count, int max_bits )
{
    u32 *weight = defl->weight;
    u16 *parent = defl->parent, *depth = defl->depth, *order = defl->order;
    u16 per[MAX_BITS + 1];
    u32 total;
    u16 hold;
    int used = 0, index, gap, back, leaf, node, fresh, take, bits, pick[2];

    for ( index = 0; index < count; index++ ) {
        tree->len[index] = 0;
        if ( tree->freq[index] ) {
            order[used++] = (u16)index;
        }
    }
    /* two codes at least: a reader is owed a bit to read even when
       there is one thing it can mean */
    if ( used == 0 ) {
        order[used++] = 0;
        order[used++] = 1;
    } else if ( used == 1 ) {
        order[used++] = (u16)(order[0] == 0 ? 1 : 0);
    }
    for ( gap = used / 2; gap > 0; gap /= 2 ) {
        for ( index = gap; index < used; index++ ) {
            hold = order[index];
            for ( back = index; back >= gap && LIGHTER( tree, hold, order[back - gap] ); back -= gap ) {
                order[back] = order[back - gap];
            }
            order[back] = hold;
        }
    }
    /* the two lightest, again and again: the leaves are in order, and
       so are the nodes as they are made, so each is a queue */
    for ( index = 0; index < used; index++ ) {
        weight[index] = WEIGH( tree, order[index] );
    }
    leaf = 0;
    node = fresh = used;
    while ( fresh < 2 * used - 1 ) {
        for ( take = 0; take < 2; take++ ) {
            if ( leaf < used && (node >= fresh || weight[leaf] <= weight[node]) ) {
                pick[take] = leaf++;
            } else {
                pick[take] = node++;
            }
        }
        weight[fresh] = weight[pick[0]] + weight[pick[1]];
        parent[pick[0]] = parent[pick[1]] = (u16)fresh;
        fresh++;
    }
    depth[fresh - 1] = 0;
    for ( index = fresh - 2; index >= 0; index-- ) {
        depth[index] = (u16)(depth[parent[index]] + 1);
    }
    for ( bits = 0; bits <= MAX_BITS; bits++ ) {
        per[bits] = 0;
    }
    for ( index = 0; index < used; index++ ) {
        per[depth[index] > max_bits ? max_bits : depth[index]]++;
    }
    /* cutting the long ones short made more codes than there is room
       for: take one off the longest and split the next shortest */
    total = 0;
    for ( bits = 1; bits <= max_bits; bits++ ) {
        total += (u32)per[bits] << (max_bits - bits);
    }
    while ( total > (1UL << max_bits) ) {
        per[max_bits]--;
        for ( bits = max_bits - 1; bits > 0; bits-- ) {
            if ( per[bits] ) {
                per[bits]--;
                per[bits + 1] += 2;
                break;
            }
        }
        total--;
    }
    /* the rarest get the longest */
    index = 0;
    for ( bits = max_bits; bits >= 1; bits-- ) {
        for ( take = per[bits]; take > 0; take-- ) {
            tree->len[order[index++]] = (u8)bits;
        }
    }
    defl_codes( tree, count );
}

/* what never changes: the fixed codes, and which code a length and a
   distance have */
static void defl_tables( DEFL *defl )
{
    u32 code, value, step;

    mem_set( defl->fix_lit.len, 8, 144 );
    mem_set( defl->fix_lit.len + 144, 9, 112 );
    mem_set( defl->fix_lit.len + 256, 7, 24 );
    mem_set( defl->fix_lit.len + 280, 8, 8 );
    defl_codes( &defl->fix_lit, LIT_CODES );
    mem_set( defl->fix_dist.len, 5, DIST_CODES );
    defl_codes( &defl->fix_dist, DIST_CODES );

    for ( code = 0; code < 28; code++ ) {
        for ( value = 0; value < (1UL << len_extra[code]); value++ ) {
            defl->len_code[len_base[code] - MIN_MATCH + value] = (u8)code;
        }
    }
    defl->len_code[MAX_MATCH - MIN_MATCH] = 28;
    for ( code = 0; code < 16; code++ ) {
        for ( value = 0; value < (1UL << dist_extra[code]); value++ ) {
            defl->dist_code[dist_base[code] - 1 + value] = (u8)code;
        }
    }
    for ( code = 16; code < 30; code++ ) {
        step = (1UL << dist_extra[code]) >> 7;
        for ( value = 0; value < step; value++ ) {
            defl->dist_code[256 + ((dist_base[code] - 1) >> 7) + value] = (u8)code;
        }
    }
}

/* one length of the header's list, counted */
static void defl_seq( DEFL *defl, int sym, int extra )
{
    defl->seq[defl->nseq] = (u8)sym;
    defl->seq_extra[defl->nseq] = (u8)extra;
    defl->nseq++;
    defl->bl.freq[sym]++;
}

/* the lengths of both codes, runs of them folded as the format allows */
static void defl_lengths( DEFL *defl, int hlit, int hdist )
{
    u8 all[L_CODES + D_CODES];
    int total = hlit + hdist, index = 0, run, value, part;

    mem_cpy( all, defl->lit.len, (u32)hlit );
    mem_cpy( all + hlit, defl->dist.len, (u32)hdist );
    mem_set( defl->bl.freq, 0, sizeof( defl->bl.freq ) );
    defl->nseq = 0;
    while ( index < total ) {
        value = all[index];
        for ( run = 1; index + run < total && all[index + run] == value; run++ ) {
        }
        index += run;
        if ( value == 0 ) {
            while ( run >= 11 ) {
                part = run > 138 ? 138 : run;
                defl_seq( defl, 18, part - 11 );
                run -= part;
            }
            if ( run >= 3 ) {
                defl_seq( defl, 17, run - 3 );
                run = 0;
            }
        } else {
            defl_seq( defl, value, 0 );
            run--;
            while ( run >= 3 ) {
                part = run > 6 ? 6 : run;
                defl_seq( defl, 16, part - 3 );
                run -= part;
            }
        }
        while ( run-- > 0 ) {
            defl_seq( defl, value, 0 );
        }
    }
}

/* the symbols of the block in hand, in the two codes given */
static void defl_symbols( DEFL *defl, const TREE *lit, const TREE *dist )
{
    u32 index, far_back, code;

    for ( index = 0; index < defl->nsym; index++ ) {
        far_back = defl->sym_dist[index];
        if ( far_back == 0 ) {
            defl_put( defl, lit->code[defl->sym_lc[index]], lit->len[defl->sym_lc[index]] );
            continue;
        }
        code = defl->len_code[defl->sym_lc[index]];
        defl_put( defl, lit->code[END_BLOCK + 1 + code], lit->len[END_BLOCK + 1 + code] );
        if ( len_extra[code] ) {
            defl_put( defl, (u32)defl->sym_lc[index] + MIN_MATCH - len_base[code], len_extra[code] );
        }
        code = DIST_CODE( defl, far_back );
        defl_put( defl, dist->code[code], dist->len[code] );
        if ( dist_extra[code] ) {
            defl_put( defl, far_back - dist_base[code], dist_extra[code] );
        }
    }
    defl_put( defl, lit->code[END_BLOCK], lit->len[END_BLOCK] );
}

/*
 * The block in hand - its symbols, which stand for src[start..end) -
 * written the smallest way.
 */
static void defl_block( DEFL *defl, u32 start, u32 end, int final )
{
    u32 dyn_bits, fix_bits, extra_bits = 0, stored, size = end - start, part, index;
    int hlit, hdist, hclen, code;

    defl->lit.freq[END_BLOCK]++;
    defl_tree( defl, &defl->lit, L_CODES, MAX_BITS );
    defl_tree( defl, &defl->dist, D_CODES, MAX_BITS );
    for ( hlit = L_CODES; hlit > END_BLOCK + 1 && defl->lit.len[hlit - 1] == 0; hlit-- ) {
    }
    for ( hdist = D_CODES; hdist > 1 && defl->dist.len[hdist - 1] == 0; hdist-- ) {
    }
    defl_lengths( defl, hlit, hdist );
    defl_tree( defl, &defl->bl, BL_CODES, MAX_BL_BITS );
    for ( hclen = BL_CODES; hclen > 4 && defl->bl.len[clen_order[hclen - 1]] == 0; hclen-- ) {
    }

    dyn_bits = 3 + 14 + 3 * (u32)hclen;
    fix_bits = 3;
    for ( code = 0; code < BL_CODES; code++ ) {
        dyn_bits += (u32)defl->bl.freq[code] * defl->bl.len[code];
    }
    dyn_bits += (u32)defl->bl.freq[16] * 2 + (u32)defl->bl.freq[17] * 3 + (u32)defl->bl.freq[18] * 7;
    for ( code = 0; code < L_CODES; code++ ) {
        dyn_bits += (u32)defl->lit.freq[code] * defl->lit.len[code];
        fix_bits += (u32)defl->lit.freq[code] * defl->fix_lit.len[code];
        if ( code > END_BLOCK ) {
            extra_bits += (u32)defl->lit.freq[code] * len_extra[code - END_BLOCK - 1];
        }
    }
    for ( code = 0; code < D_CODES; code++ ) {
        dyn_bits += (u32)defl->dist.freq[code] * defl->dist.len[code];
        fix_bits += (u32)defl->dist.freq[code] * 5;
        extra_bits += (u32)defl->dist.freq[code] * dist_extra[code];
    }
    dyn_bits += extra_bits;
    fix_bits += extra_bits;
    /* stored: five bytes for every 64K, and up to a byte to line up */
    stored = (size + 5 * (size / STORED_MAX + 1) + 1) * 8;

    if ( stored <= dyn_bits && stored <= fix_bits ) {
        do {
            part = size > STORED_MAX ? STORED_MAX : size;
            size -= part;
            if ( !defl_room( defl, part + 8 ) ) {
                break;
            }
            defl_put( defl, (u32)(final && size == 0), 1 );
            defl_put( defl, 0, 2 );
            if ( defl->nbits ) {
                defl_put( defl, 0, 8 - defl->nbits );
            }
            defl->out[defl->used++] = (u8)part;
            defl->out[defl->used++] = (u8)(part >> 8);
            defl->out[defl->used++] = (u8)~part;
            defl->out[defl->used++] = (u8)(~part >> 8);
            mem_cpy( defl->out + defl->used, defl->src + start, part );
            defl->used += part;
            start += part;
        } while ( size );
    } else if ( fix_bits <= dyn_bits ) {
        if ( defl_room( defl, fix_bits / 8 + 8 ) ) {
            defl_put( defl, (u32)final, 1 );
            defl_put( defl, 1, 2 );
            defl_symbols( defl, &defl->fix_lit, &defl->fix_dist );
        }
    } else if ( defl_room( defl, dyn_bits / 8 + 8 ) ) {
        defl_put( defl, (u32)final, 1 );
        defl_put( defl, 2, 2 );
        defl_put( defl, (u32)hlit - 257, 5 );
        defl_put( defl, (u32)hdist - 1, 5 );
        defl_put( defl, (u32)hclen - 4, 4 );
        for ( code = 0; code < hclen; code++ ) {
            defl_put( defl, defl->bl.len[clen_order[code]], 3 );
        }
        for ( index = 0; index < defl->nseq; index++ ) {
            code = defl->seq[index];
            defl_put( defl, defl->bl.code[code], defl->bl.len[code] );
            if ( code == 16 ) {
                defl_put( defl, defl->seq_extra[index], 2 );
            } else if ( code == 17 ) {
                defl_put( defl, defl->seq_extra[index], 3 );
            } else if ( code == 18 ) {
                defl_put( defl, defl->seq_extra[index], 7 );
            }
        }
        defl_symbols( defl, &defl->lit, &defl->dist );
    }
    defl->nsym = 0;
    mem_set( defl->lit.freq, 0, sizeof( defl->lit.freq ) );
    mem_set( defl->dist.freq, 0, sizeof( defl->dist.freq ) );
}

/* a place put in its hash's chain -> how far back the one before it
   is, 0 for none in reach */
static u32 defl_insert( DEFL *defl, u32 pos )
{
    u32 hash = HASH3( defl->src + pos );
    u32 was = defl->head[hash];
    u32 back = was ? pos + 1 - was : 0;

    if ( back >= WSIZE ) {
        back = 0;
    }
    defl->prev[pos & WMASK] = (u16)back;
    defl->head[hash] = pos + 1;
    return back;
}

/*
 * The longest match for the bytes at "pos" that is longer than "best",
 * starting "back" bytes back and following the chain.  -> its length
 * and *dist, or 0 for none.
 */
static u32 defl_match( const DEFL *defl, u32 pos, u32 back, u32 best, u32 max_len, u32 chain,
                       u32 *dist )
{
    const u8 *scan = defl->src + pos, *cand;
    u32 found = 0, len, step;

    while ( back ) {
        cand = scan - back;
        if ( cand[best] == scan[best] && cand[0] == scan[0] && cand[1] == scan[1] ) {
            for ( len = 2; len < max_len && cand[len] == scan[len]; len++ ) {
            }
            if ( len > best ) {
                best = found = len;
                *dist = back;
                if ( len >= NICE_MATCH || len >= max_len ) {
                    break;
                }
            }
        }
        if ( --chain == 0 ) {
            break;
        }
        step = defl->prev[(pos - back) & WMASK];
        if ( step == 0 ) {
            break;
        }
        back += step;
        if ( back >= WSIZE ) {
            break;
        }
    }
    return found;
}

static void defl_literal( DEFL *defl, u32 value )
{
    defl->sym_dist[defl->nsym] = 0;
    defl->sym_lc[defl->nsym] = (u8)value;
    defl->nsym++;
    defl->lit.freq[value]++;
}

static void defl_run( DEFL *defl, int last )
{
    const u8 *src = defl->src;
    u32 len = defl->len, pos = 0, block = 0, back, max_len, stop;
    u32 cur_len, cur_dist = 0, held_len = 0, held_dist = 0;
    int held = 0;

    while ( pos < len ) {
        cur_len = 0;
        if ( len - pos >= MIN_MATCH ) {
            back = defl_insert( defl, pos );
            max_len = len - pos > MAX_MATCH ? MAX_MATCH : len - pos;
            if ( back && held_len < LAZY_MATCH && held_len < max_len ) {
                cur_len = defl_match( defl, pos, back, held_len < MIN_MATCH ? MIN_MATCH - 1 : held_len,
                                      max_len, held_len >= GOOD_MATCH ? MAX_CHAIN / 4 : MAX_CHAIN,
                                      &cur_dist );
                if ( cur_len == MIN_MATCH && cur_dist > TOO_FAR ) {
                    cur_len = 0;
                }
            }
        }
        if ( held && held_len >= MIN_MATCH && cur_len <= held_len ) {
            /* the match that was held back, which began a byte ago, stands */
            defl->sym_dist[defl->nsym] = (u16)held_dist;
            defl->sym_lc[defl->nsym] = (u8)(held_len - MIN_MATCH);
            defl->nsym++;
            defl->lit.freq[END_BLOCK + 1 + defl->len_code[held_len - MIN_MATCH]]++;
            defl->dist.freq[DIST_CODE( defl, held_dist )]++;
            stop = pos - 1 + held_len;
            for ( pos++; pos < stop; pos++ ) {
                if ( len - pos >= MIN_MATCH ) {
                    defl_insert( defl, pos );
                }
            }
            held = 0;
            held_len = 0;
        } else {
            if ( held ) {
                defl_literal( defl, src[pos - 1] );
            }
            held = 1;
            held_len = cur_len;
            held_dist = cur_dist;
            pos++;
        }
        if ( defl->nsym >= BLOCK_SYMS ) {
            defl_block( defl, block, pos - (u32)held, 0 );
            block = pos - (u32)held;
        }
    }
    if ( held ) {
        defl_literal( defl, src[len - 1] );
    }
    defl_block( defl, block, len, last );
}

/*
 * The one encoder behind both calls.  "last" 0 is a piece that more
 * will follow: none of its blocks says it is the final one, and an
 * empty stored block after them brings the stream to a whole byte so
 * that the next piece can be written straight after it.
 */
static u8 *defl_all( const u8 *src, u32 len, u32 *out_len, int zlib, int last )
{
    DEFL *defl;
    u8 *out;
    u32 work = sizeof( DEFL ) + HASH_SIZE * sizeof( u32 ) + WSIZE * sizeof( u16 ) +
               BLOCK_SYMS * (sizeof( u16 ) + 1);
    u32 sum;

    *out_len = 0;
    defl = (DEFL *)try_alloc( work );
    if ( defl == NULL ) {
        return NULL;
    }
    mem_set( defl, 0, work );
    defl->head = (u32 *)(defl + 1);
    defl->prev = (u16 *)(defl->head + HASH_SIZE);
    defl->sym_dist = defl->prev + WSIZE;
    defl->sym_lc = (u8 *)(defl->sym_dist + BLOCK_SYMS);
    defl->src = src;
    defl->len = len;
    defl->cap = len / 2 + 64;
    defl->out = (u8 *)try_alloc( defl->cap );
    if ( defl->out == NULL ) {
        xfree( defl );
        return NULL;
    }
    if ( zlib ) {
        defl->out[defl->used++] = 0x78;         /* DEFLATE, a 32K window */
        defl->out[defl->used++] = 0x9C;         /* ...and the check bits */
    }
    defl_tables( defl );
    defl_run( defl, last );
    if ( defl_room( defl, 16 ) ) {
        if ( !last ) {
            defl_put( defl, 0, 3 );
        }
        if ( defl->nbits ) {
            defl_put( defl, 0, 8 - defl->nbits );
        }
        if ( !last ) {
            defl->out[defl->used++] = 0;
            defl->out[defl->used++] = 0;
            defl->out[defl->used++] = 0xFF;
            defl->out[defl->used++] = 0xFF;
        }
        if ( zlib ) {
            sum = adler32_update( 1, src, len );
            defl->out[defl->used++] = (u8)(sum >> 24);
            defl->out[defl->used++] = (u8)(sum >> 16);
            defl->out[defl->used++] = (u8)(sum >> 8);
            defl->out[defl->used++] = (u8)sum;
        }
    }
    out = defl->out;
    if ( defl->failed ) {
        xfree( out );
        out = NULL;
    } else {
        *out_len = defl->used;
    }
    xfree( defl );
    return out;
}

u8 *deflate_alloc( const u8 *src, u32 len, u32 *out_len, int zlib )
{
    return defl_all( src, len, out_len, zlib, 1 );
}

u8 *deflate_piece( const u8 *src, u32 len, u32 *out_len, int last )
{
    return defl_all( src, len, out_len, 0, last );
}
