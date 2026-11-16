/*
 * VBDEC.C - Vorbis I: the three headers, the codebooks, floors,
 * residues and mappings they set up, and the decode of one audio
 * packet into finished samples (VB.H).
 *
 * A VORBIS STREAM BRINGS ITS OWN DECODER.  The third header lists the
 * Huffman codebooks, the floor and residue configurations and how the
 * channels use them; nothing about the coding is fixed in advance.  So
 * opening a stream is most of the work: every count and size in that
 * header is checked against a limit before anything is allocated, and
 * the tables the packets need are built then, once.
 *
 * A CODEBOOK IS DECODED BY TABLE.  The next ten bits of the packet
 * index a table that gives the entry and the length of every codeword
 * up to ten bits long - nearly all of them, by the nature of a Huffman
 * code.  A longer codeword sends the decoder to a sorted list of the
 * long ones, searched by halving.  The codewords are assigned as the
 * specification says, each the lowest free one of its length; a tree
 * that is overfull or has gaps is refused, except the single-entry book
 * that the specification's errata allow.
 *
 * VECTOR VALUES ARE MADE AT OPEN TIME, in the residue's fixed-point
 * format, so a residue partition decodes to table lookups and adds.
 * The format's binary point is chosen for each stream: as many fraction
 * bits as the largest sum the stream's residue books can make allows,
 * with room for the stereo uncoupling to double it.  (The specification
 * asks for 280 dB of residue range; 32 bits hold about 190, and real
 * encoders use far less than either.)
 *
 * THE FLOOR TIMES THE RESIDUE IS ONE MULTIPLY A LINE.  Floor 1's curve
 * is drawn with the specification's integer lines straight into the
 * product: each point of a line picks a value from the 140 dB table,
 * which carries its own binary point, and multiplies the residue line
 * under it.  A line whose residue is zero costs no multiply.  Floor 0
 * (line spectral pairs, rare) is worked out in a fixed-point number that
 * carries its own exponent, then put in the same form.
 *
 * MIXING HAPPENS IN THE SPECTRUM.  The inverse transform is linear, so
 * the channels of a 5.1 stream are weighted and added before it and only
 * two transforms run; one channel asked for means one transform.  The
 * weights are folded into each channel's copy of the floor table and
 * cost nothing per line.
 *
 * HALF AND QUARTER RATE keep the lowest half or quarter of the lines
 * and use a transform that much smaller.  The residue's codewords must
 * still all be read - they are packed one after another - but the lines
 * that are dropped are never added up.
 *
 * DAMAGE: a packet that ends early is normal in Vorbis (an encoder may
 * cut one short) and decodes as far as it goes.  A packet that is not
 * audio, or names a mode that does not exist, is not decoded at all, and
 * vb_block, which ogg.c uses to keep time, sees the same thing.
 */
#include "rt.h"
#include "fix.h"
#include "vb.h"

#define MAX_ENTRIES     (1UL << 18)     /* entries in one codebook */
#define MAX_VALUES      (1UL << 20)     /* entries times dimensions of one vector table */
#define SETUP_WORK      (1UL << 22)     /* entries, values and table slots one setup may build */
#define MAX_BARK        4096            /* floor 0's Bark map size: real streams use 64 to 256 */
#define LUT_BITS        10
#define LONG_MARK       63              /* a table slot that leads to the long codes */
#define F1_MAX          65              /* floor 1 points, as the specification limits them */
#define F0_MAX          255             /* floor 0's order is 8 bits */
#define MAX_COUPLE      256

/* the bits of a packet, least significant first */
typedef struct {
    const u8 *ptr, *end;
    u32  bits;
    int  have;                  /* bits held in "bits", 0 to 32 */
    int  eop;                   /* a read went past the end */
} BITS;

typedef struct {
    u32  entries, dims, used;
    u8   lookup, seq, lbits, maxlen;
    u32  lmask;
    u32 *lut;                   /* 1 << lbits slots: (entry << 6) | length */
    u32 *long_code;             /* the codes longer than lbits, left-aligned, ascending */
    u32 *long_val;              /* their (entry << 6) | length */
    u32  nlong;
    s32 *vals;                  /* entries * dims values, "vq" fraction bits */
    int  vq;
    u16 *mult;                  /* the header's multiplicands, until vals is made */
    u32  nmult, maxmult;
    s32  min_m, delta_m;
    int  min_e, delta_e;
    u8   in_res, in_f0;         /* read as vectors by a residue, by a floor 0 */
} BOOK;

typedef struct {
    u8   type;
    /* floor 1 */
    u8   parts, mult, nval;
    u8   pclass[31];
    u8   cdim[16], cbits[16];
    s16  cmaster[16];
    s16  subbook[16][8];
    u16  x[F1_MAX];
    u8   order[F1_MAX];         /* the points in ascending x */
    u8   lo[F1_MAX], hi[F1_MAX];
    /* floor 0 */
    u8   order0, ampbits, ampoff, nbooks;
    u16  rate, bark;
    u8   books[16];
    u16 *map[2];                /* the Bark map of each block size */
    s32 *cosw;                  /* cos( pi k / bark ), Q30 */
} FLOOR;

typedef struct {
    u8   type, classes, classbook, passes;
    u32  begin, end, psize, cdims;
    s16  books[64][8];
    u8  *ctab;                  /* each classbook entry's class numbers, cdims of them */
} RESID;

typedef struct {
    u8   submaps;
    u16  steps;
    u8   mag[MAX_COUPLE], ang[MAX_COUPLE];
    u8   mux[VB_MAX_CH];
    u8   floor[16], resid[16];
} MAP;

/* a number with its own exponent, for floor 0: m * 2^e, m from 2^31 up, or 0 */
typedef struct {
    u32  m;
    int  e;
} VBF;

struct VB {
    VBID    id;
    VBOPT   opt;
    int     nch;
    u32     n[2];               /* the block sizes */
    int     nbooks, nfloors, nres, nmaps, nmodes, mode_bits;
    BOOK   *books;
    FLOOR  *floors;
    RESID  *res;
    MAP    *maps;
    u8      mode_flag[64], mode_map[64];
    int     qr;                 /* the residue's fraction bits */
    u32     budget;             /* what is left of SETUP_WORK */
    /* each channel's floor table, the channel's weight folded in */
    u32    *fmant[VB_MAX_CH];
    u8     *fshift[VB_MAX_CH];
    s32     gain[VB_MAX_CH];    /* Q30 */
    u8      side[VB_MAX_CH];    /* 1: output 0, 2: output 1, 3: both */
    int     mixing;
    /* decoding */
    s32    *vec[VB_MAX_CH];     /* the residue, then the channel's spectrum */
    s32    *mixed[2];
    s32    *work;
    s32    *ubuf[2][2];
    int     cur;
    u32     prev_n;             /* the previous block's transform size, 0: none */
    VBMDCT  md[2];
    VBFFT   fft;
    u8     *cls;                /* classifications: nch rows of cls_room */
    u32     cls_room;
    s32     fy[VB_MAX_CH][F1_MAX];
    s32    *f0coef;             /* nch rows of F0_MAX, Q24 radians */
    s32     f0amp[VB_MAX_CH];
    u8      used[VB_MAX_CH];
    s16    *pcm;
    u32    *eqgain;
    u16     eq_serial;
    u8      eq_valid, use_eq;
};

/* ------------------------------------------------------------------ */
/* bits                                                                */
/* ------------------------------------------------------------------ */
static void bits_init( BITS *br, const u8 *data, u32 len )
{
    br->ptr = data;
    br->end = data + len;
    br->bits = 0;
    br->have = 0;
    br->eop = 0;
}

static void bits_fill( BITS *br )
{
    while ( br->have <= 24 && br->ptr < br->end ) {
        br->bits |= (u32)*br->ptr++ << br->have;
        br->have += 8;
    }
}

/* "count" bits, 0 to 24 */
static u32 bits_small( BITS *br, int count )
{
    u32 val;

    if ( br->have < count ) {
        bits_fill( br );
        if ( br->have < count ) {
            br->eop = 1;
            br->have = 0;
            br->bits = 0;
            return 0;
        }
    }
    val = br->bits & ((1UL << count) - 1);
    br->bits >>= count;
    br->have -= count;
    return val;
}

/* "count" bits, 0 to 32 */
static u32 bits_get( BITS *br, int count )
{
    u32 low;

    if ( count <= 24 ) {
        return bits_small( br, count );
    }
    low = bits_small( br, 16 );
    return low | (bits_small( br, count - 16 ) << 16);
}

/* the position of the highest bit set, 1 up; 0 for 0 */
static int ilog( u32 val )
{
    return val == 0 ? 0 : fx_topbit( val ) + 1;
}

static u32 rev32( u32 val )
{
    return ((u32)vb_rev8[val & 255] << 24) | ((u32)vb_rev8[(val >> 8) & 255] << 16)
           | ((u32)vb_rev8[(val >> 16) & 255] << 8) | vb_rev8[val >> 24];
}

/* ------------------------------------------------------------------ */
/* Huffman decoding                                                    */
/* ------------------------------------------------------------------ */
static s32 book_fail( BITS *br )
{
    br->eop = 1;
    br->have = 0;
    br->bits = 0;
    return -1;
}

/* a codeword longer than the table, or one that runs past the end */
static s32 book_long( const BOOK *bk, BITS *br, int len )
{
    u32 win, code, lo, hi, mid, slot;
    int avail;
    const u8 *at;

    if ( len != LONG_MARK || bk->nlong == 0 ) {
        return book_fail( br );
    }
    /* up to 32 bits ahead, without taking them */
    win = br->bits;
    avail = br->have;
    at = br->ptr;
    while ( avail < 32 && at < br->end ) {
        win |= (u32)*at++ << avail;
        avail += 8;
    }
    if ( avail > 32 ) {
        avail = 32;
    }
    code = rev32( win );
    lo = 0;
    hi = bk->nlong;
    while ( hi - lo > 1 ) {
        mid = lo + (hi - lo) / 2;
        if ( bk->long_code[mid] <= code ) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    if ( bk->long_code[lo] > code ) {
        return book_fail( br );
    }
    slot = bk->long_val[lo];
    len = (int)(slot & 63);
    if ( len > avail || len < 1 || ((code ^ bk->long_code[lo]) >> (32 - len)) != 0 ) {
        return book_fail( br );
    }
    bits_get( br, len );
    return (s32)(slot >> 6);
}

/* an entry number, or -1 at the end of the packet */
static s32 book_decode( const BOOK *bk, BITS *br )
{
    u32 slot;
    int len;

    if ( br->have < (int)bk->lbits ) {
        bits_fill( br );
    }
    slot = bk->lut[br->bits & bk->lmask];
    len = (int)(slot & 63);
    if ( len != 0 && len != LONG_MARK && len <= br->have ) {
        br->bits >>= len;
        br->have -= len;
        return (s32)(slot >> 6);
    }
    return book_long( bk, br, len );
}

/* ------------------------------------------------------------------ */
/* building a codebook                                                 */
/* ------------------------------------------------------------------ */
/* sort the long codes, codes and values together (heapsort: no
   recursion, n log n whatever the order) */
static void sift( u32 *code, u32 *val, u32 root, u32 count )
{
    u32 child, tc, tv;

    for ( ;; ) {
        child = 2 * root + 1;
        if ( child >= count ) {
            return;
        }
        if ( child + 1 < count && code[child + 1] > code[child] ) {
            child++;
        }
        if ( code[root] >= code[child] ) {
            return;
        }
        tc = code[root];
        code[root] = code[child];
        code[child] = tc;
        tv = val[root];
        val[root] = val[child];
        val[child] = tv;
        root = child;
    }
}

static void sort_codes( u32 *code, u32 *val, u32 count )
{
    u32 i, tc, tv;

    if ( count < 2 ) {
        return;
    }
    for ( i = count / 2; i-- > 0; ) {
        sift( code, val, i, count );
    }
    for ( i = count - 1; i > 0; i-- ) {
        tc = code[0];
        code[0] = code[i];
        code[i] = tc;
        tv = val[0];
        val[0] = val[i];
        val[i] = tv;
        sift( code, val, 0, i );
    }
}

/* The codewords from the lengths: each entry in turn takes the lowest
   free codeword of its length.  The free space is kept as at most one
   free node at each depth, the deeper ones to the left, so the deepest
   free node no deeper than the length is the lowest choice.  -> DE_OK
   or DE_FORMAT (overfull or unfinished) or DE_NOMEM */
static int book_tree( BOOK *bk, const u8 *len )
{
    u32 avail[33], *codes = NULL, i, code, slot, step, idx, k, nlong = 0;
    u8 have[33];
    int depth, length;

    bk->used = 0;
    bk->maxlen = 0;
    for ( i = 0; i < bk->entries; i++ ) {
        if ( len[i] != 0 ) {
            bk->used++;
            if ( len[i] > bk->maxlen ) {
                bk->maxlen = len[i];
            }
        }
    }
    if ( bk->used <= 1 ) {
        /* nothing, or the one entry, read as one bit of either value */
        bk->lbits = 1;
        bk->lmask = 1;
        bk->lut = (u32 *)try_alloc( 2 * sizeof( u32 ) );
        if ( bk->lut == NULL ) {
            return DE_NOMEM;
        }
        bk->lut[0] = bk->lut[1] = 0;
        for ( i = 0; i < bk->entries; i++ ) {
            if ( len[i] != 0 ) {
                bk->lut[0] = bk->lut[1] = (i << 6) | 1;
            }
        }
        return DE_OK;
    }
    codes = (u32 *)try_alloc( bk->entries * sizeof( u32 ) );
    if ( codes == NULL ) {
        return DE_NOMEM;
    }
    mem_set( have, 0, sizeof( have ) );
    have[0] = 1;
    avail[0] = 0;
    for ( i = 0; i < bk->entries; i++ ) {
        length = len[i];
        codes[i] = 0;
        if ( length == 0 ) {
            continue;
        }
        for ( depth = length; depth >= 0 && !have[depth]; depth-- ) {
        }
        if ( depth < 0 ) {
            xfree( codes );
            return DE_FORMAT;                   /* more codewords than the lengths allow */
        }
        code = avail[depth];
        have[depth] = 0;
        for ( k = (u32)depth + 1; k <= (u32)length; k++ ) {
            avail[k] = code + (1UL << (32 - k));
            have[k] = 1;
        }
        codes[i] = code;
    }
    for ( k = 0; k <= 32; k++ ) {
        if ( have[k] ) {
            xfree( codes );
            return DE_FORMAT;                   /* codewords missing */
        }
    }
    bk->lbits = (u8)(bk->maxlen < LUT_BITS ? bk->maxlen : LUT_BITS);
    bk->lmask = (1UL << bk->lbits) - 1;
    bk->lut = (u32 *)try_alloc( (bk->lmask + 1) * sizeof( u32 ) );
    if ( bk->lut == NULL ) {
        xfree( codes );
        return DE_NOMEM;
    }
    mem_set( bk->lut, 0, (bk->lmask + 1) * sizeof( u32 ) );
    for ( i = 0; i < bk->entries; i++ ) {
        length = len[i];
        if ( length == 0 ) {
            continue;
        }
        idx = rev32( codes[i] );
        if ( length <= bk->lbits ) {
            slot = (i << 6) | (u32)length;
            step = 1UL << length;
            for ( ; idx <= bk->lmask; idx += step ) {
                bk->lut[idx] = slot;
            }
        } else {
            bk->lut[idx & bk->lmask] = LONG_MARK;
            nlong++;
        }
    }
    if ( nlong > 0 ) {
        bk->long_code = (u32 *)try_alloc( nlong * sizeof( u32 ) );
        bk->long_val = (u32 *)try_alloc( nlong * sizeof( u32 ) );
        if ( bk->long_code == NULL || bk->long_val == NULL ) {
            xfree( codes );
            return DE_NOMEM;
        }
        for ( i = 0, k = 0; i < bk->entries; i++ ) {
            if ( len[i] > bk->lbits ) {
                bk->long_code[k] = codes[i];
                bk->long_val[k] = (i << 6) | len[i];
                k++;
            }
        }
        bk->nlong = nlong;
        sort_codes( bk->long_code, bk->long_val, nlong );
    }
    xfree( codes );
    return DE_OK;
}

/* the greatest r with r^dims <= entries */
static int pow_le( u32 base, u32 dims, u32 limit )
{
    u32 acc = 1, i;

    for ( i = 0; i < dims; i++ ) {
        if ( acc > limit / base ) {
            return 0;
        }
        acc *= base;
    }
    return 1;
}

static u32 lookup1_values( u32 entries, u32 dims )
{
    u32 lo = 1, hi = entries, mid;

    if ( dims == 1 ) {
        return entries;
    }
    if ( dims >= 32 ) {
        return 1;                       /* 2^32 is more than any entry count */
    }
    while ( lo < hi ) {
        mid = lo + (hi - lo + 1) / 2;
        if ( pow_le( mid, dims, entries ) ) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

static void float_unpack( u32 raw, s32 *mant, int *exp )
{
    *mant = (s32)(raw & 0x1FFFFFUL);
    if ( raw & 0x80000000UL ) {
        *mant = -*mant;
    }
    *exp = (int)((raw >> 21) & 0x3FF) - 788;
}

/* Take "amount" from the setup's work: a header can ask for vast tables
   in a few bytes, and building them all would hold the player up for
   minutes on a 486 before memory ran out.  -> 0 when it is used up */
static int setup_charge( u32 *work, u32 amount )
{
    if ( amount > *work ) {
        *work = 0;
        return 0;
    }
    *work -= amount;
    return 1;
}

static int book_read( BOOK *bk, BITS *br, u32 *work )
{
    u8 *len;
    u32 i, cur, num, length, raw;
    int rc, sparse;

    if ( bits_get( br, 24 ) != 0x564342UL ) {
        return DE_FORMAT;
    }
    bk->dims = bits_get( br, 16 );
    bk->entries = bits_get( br, 24 );
    if ( br->eop || bk->entries == 0 ) {
        return DE_FORMAT;
    }
    if ( bk->entries > MAX_ENTRIES || !setup_charge( work, bk->entries ) ) {
        return DE_UNSUPPORTED;
    }
    len = (u8 *)try_alloc( bk->entries );
    if ( len == NULL ) {
        return DE_NOMEM;
    }
    if ( !bits_get( br, 1 ) ) {
        sparse = (int)bits_get( br, 1 );
        for ( i = 0; i < bk->entries; i++ ) {
            if ( sparse && !bits_get( br, 1 ) ) {
                len[i] = 0;
            } else {
                len[i] = (u8)(bits_get( br, 5 ) + 1);
            }
        }
    } else {
        /* in ascending length: a count of entries for each length */
        cur = 0;
        length = bits_get( br, 5 ) + 1;
        while ( cur < bk->entries ) {
            num = bits_get( br, ilog( bk->entries - cur ) );
            if ( br->eop || num > bk->entries - cur || (num > 0 && length > 32) ) {
                xfree( len );
                return DE_FORMAT;
            }
            for ( i = 0; i < num; i++ ) {
                len[cur + i] = (u8)length;
            }
            cur += num;
            if ( ++length > 33 && cur < bk->entries ) {
                xfree( len );
                return DE_FORMAT;
            }
        }
    }
    if ( br->eop ) {
        xfree( len );
        return DE_FORMAT;
    }
    bk->lookup = (u8)bits_get( br, 4 );
    if ( bk->lookup > 2 ) {
        xfree( len );
        return DE_FORMAT;
    }
    if ( bk->lookup != 0 ) {
        raw = bits_get( br, 32 );
        float_unpack( raw, &bk->min_m, &bk->min_e );
        raw = bits_get( br, 32 );
        float_unpack( raw, &bk->delta_m, &bk->delta_e );
        num = bits_get( br, 4 ) + 1;
        bk->seq = (u8)bits_get( br, 1 );
        if ( bk->dims == 0 || br->eop ) {
            xfree( len );
            return DE_FORMAT;
        }
        if ( bk->dims > MAX_VALUES / bk->entries
             || !setup_charge( work, bk->entries * bk->dims ) ) {
            xfree( len );
            return DE_UNSUPPORTED;
        }
        bk->nmult = bk->lookup == 1 ? lookup1_values( bk->entries, bk->dims )
                                    : bk->entries * bk->dims;
        bk->mult = (u16 *)try_alloc( bk->nmult * sizeof( u16 ) );
        if ( bk->mult == NULL ) {
            xfree( len );
            return DE_NOMEM;
        }
        bk->maxmult = 0;
        for ( i = 0; i < bk->nmult && !br->eop; i++ ) {
            bk->mult[i] = (u16)bits_get( br, (int)num );
            if ( bk->mult[i] > bk->maxmult ) {
                bk->maxmult = bk->mult[i];
            }
        }
        if ( br->eop ) {
            xfree( len );
            return DE_FORMAT;
        }
    }
    rc = book_tree( bk, len );
    xfree( len );
    return rc;
}

/* ------------------------------------------------------------------ */
/* vector values in fixed point                                        */
/* ------------------------------------------------------------------ */
/* round( (hi:lo) * 2^shift ), a signed 64-bit number, cut to 32 bits */
static s32 fix_scale( s32 hi, u32 lo, int shift )
{
    u32 uh = (u32)hi, ul = lo, val, add;
    int neg = hi < 0, s;

    if ( neg ) {
        ul = ~ul + 1;
        uh = ~uh + (ul == 0 ? 1 : 0);
    }
    if ( uh == 0 && ul == 0 ) {
        return 0;
    }
    if ( shift >= 0 ) {
        if ( uh != 0 || shift >= 31 || ul > (0x7FFFFFFFUL >> shift) ) {
            val = 0x7FFFFFFFUL;
        } else {
            val = ul << shift;
        }
    } else {
        s = -shift;
        if ( s >= 64 ) {
            val = 0;
        } else {
            /* add half of the last place kept, then shift */
            if ( s - 1 >= 32 ) {
                uh += 1UL << (s - 1 - 32);
            } else {
                add = 1UL << (s - 1);
                ul += add;
                if ( ul < add ) {
                    uh++;
                }
            }
            if ( s >= 32 ) {
                val = uh >> (s - 32);
            } else if ( (uh >> s) != 0 ) {
                val = 0x7FFFFFFFUL;
            } else {
                val = (ul >> s) | (s > 0 ? uh << (32 - s) : 0);
            }
            if ( val > 0x7FFFFFFFUL ) {
                val = 0x7FFFFFFFUL;
            }
        }
    }
    return neg ? -(s32)val : (s32)val;
}

static s32 sat_add( s32 a, s32 b )
{
    s32 sum = (s32)((u32)a + (u32)b);

    if ( a >= 0 && b >= 0 && sum < 0 ) {
        return 0x7FFFFFFFL;
    }
    if ( a < 0 && b < 0 && sum >= 0 ) {
        return -0x7FFFFFFFL;
    }
    return sum;
}

/* multiplicand * delta + minimum, with "q" fraction bits */
static s32 book_value( const BOOK *bk, u32 mult, int q )
{
    s32 hi;
    u32 lo;

    lo = vb_mul64( (s32)mult, bk->delta_m, &hi );
    return sat_add( fix_scale( hi, lo, bk->delta_e + q ),
                    fix_scale( bk->min_m < 0 ? -1 : 0, (u32)bk->min_m, bk->min_e + q ) );
}

/* how many bits the book's values can need: |value| < 2^bits */
static int book_bits( const BOOK *bk )
{
    s32 lo = book_value( bk, 0, 8 ), hi = book_value( bk, bk->maxmult, 8 );
    u32 top;
    int bits;

    lo = lo < 0 ? -lo : lo;
    hi = hi < 0 ? -hi : hi;
    top = (u32)(lo > hi ? lo : hi);
    if ( top == 0 ) {
        return -8;
    }
    bits = fx_topbit( top ) + 1 - 8;
    if ( bk->seq ) {
        bits += ilog( bk->dims - 1 );
    }
    return bits;
}

static int book_values( BOOK *bk, int q )
{
    u32 e, i, idx, off, count = bk->entries * bk->dims;
    s32 last, val, *row;

    bk->vals = (s32 *)try_alloc( count * sizeof( s32 ) );
    if ( bk->vals == NULL ) {
        return DE_NOMEM;
    }
    bk->vq = q;
    row = bk->vals;
    for ( e = 0; e < bk->entries; e++ ) {
        last = 0;
        idx = e;
        for ( i = 0; i < bk->dims; i++ ) {
            if ( bk->lookup == 1 ) {
                off = idx % bk->nmult;
                idx /= bk->nmult;
            } else {
                off = e * bk->dims + i;
            }
            val = sat_add( book_value( bk, bk->mult[off], q ), last );
            if ( bk->seq ) {
                last = val;
            }
            *row++ = val;
        }
    }
    xfree( bk->mult );
    bk->mult = NULL;
    return DE_OK;
}

/* ------------------------------------------------------------------ */
/* floors: setup                                                       */
/* ------------------------------------------------------------------ */
static const u16 f1_range[4] = { 256, 128, 86, 64 };

static int floor1_read( VB *vb, FLOOR *fl, BITS *br )
{
    int i, j, k, maxclass = -1, count, rangebits;
    u32 book;

    fl->parts = (u8)bits_get( br, 5 );
    for ( i = 0; i < fl->parts; i++ ) {
        fl->pclass[i] = (u8)bits_get( br, 4 );
        if ( fl->pclass[i] > maxclass ) {
            maxclass = fl->pclass[i];
        }
    }
    for ( i = 0; i <= maxclass; i++ ) {
        fl->cdim[i] = (u8)(bits_get( br, 3 ) + 1);
        fl->cbits[i] = (u8)bits_get( br, 2 );
        fl->cmaster[i] = -1;
        if ( fl->cbits[i] ) {
            book = bits_get( br, 8 );
            if ( book >= (u32)vb->nbooks ) {
                return DE_FORMAT;
            }
            fl->cmaster[i] = (s16)book;
        }
        for ( j = 0; j < (1 << fl->cbits[i]); j++ ) {
            book = bits_get( br, 8 );
            if ( book > (u32)vb->nbooks ) {
                return DE_FORMAT;
            }
            fl->subbook[i][j] = (s16)book - 1;
        }
    }
    fl->mult = (u8)(bits_get( br, 2 ) + 1);
    rangebits = (int)bits_get( br, 4 );
    fl->x[0] = 0;
    fl->x[1] = (u16)(1U << rangebits);
    count = 2;
    for ( i = 0; i < fl->parts; i++ ) {
        k = fl->cdim[fl->pclass[i]];
        if ( count + k > F1_MAX ) {
            return DE_FORMAT;
        }
        for ( j = 0; j < k; j++ ) {
            fl->x[count++] = (u16)bits_get( br, rangebits );
        }
    }
    if ( br->eop ) {
        return DE_FORMAT;
    }
    fl->nval = (u8)count;
    /* the points in ascending x (all different), and each one's neighbors */
    for ( i = 0; i < count; i++ ) {
        for ( j = i; j > 0 && fl->x[fl->order[j - 1]] > fl->x[i]; j-- ) {
            fl->order[j] = fl->order[j - 1];
        }
        fl->order[j] = (u8)i;
    }
    for ( i = 1; i < count; i++ ) {
        if ( fl->x[fl->order[i]] == fl->x[fl->order[i - 1]] ) {
            return DE_FORMAT;
        }
    }
    for ( i = 2; i < count; i++ ) {
        fl->lo[i] = 0;
        fl->hi[i] = 1;
        for ( j = 0; j < i; j++ ) {
            if ( fl->x[j] < fl->x[i] && fl->x[j] > fl->x[fl->lo[i]] ) {
                fl->lo[i] = (u8)j;
            }
            if ( fl->x[j] > fl->x[i] && fl->x[j] < fl->x[fl->hi[i]] ) {
                fl->hi[i] = (u8)j;
            }
        }
    }
    return DE_OK;
}

/* atan( y / 2^24 ) in radians, Q24, by CORDIC (y >= 0, under 2^29) */
static u32 atan_q24( s32 y )
{
    s32 x = 1L << 24, nx;
    u32 angle = 0;
    int i;

    if ( y <= 0 ) {
        return 0;
    }
    for ( i = 0; i < 31; i++ ) {
        if ( y > 0 ) {
            nx = x + (y >> i);
            y -= x >> i;
            angle += vb_atan_turn[i];
        } else {
            nx = x - (y >> i);
            y += x >> i;
            angle -= vb_atan_turn[i];
        }
        x = nx;
    }
    if ( (s32)angle <= 0 ) {
        return 0;                       /* a tiny angle that came out just under zero */
    }
    return (u32)fx_mulhi( (s32)angle, 105414357L );    /* turns to radians */
}

/* bark( rate * i / (2 half) ) in Q24:
   13.1 atan(.00074 x) + 2.24 atan(.0000000185 x^2) + .0001 x.  The map
   is the floor of this scaled, so it is worked to about 1e-7 */
static u32 bark_q24( u32 rate, u32 i, u32 half )
{
    u32 ri = rate * i, xq12, t1, t2, lin;

    t1 = fx_umuldiv( ri, 1241514UL, 200 * half );      /* .00074 x, Q24 */
    xq12 = fx_umuldiv( ri, 4096, 2 * half );
    t2 = fx_umuldiv( xq12, xq12, 54054054UL );         /* .0000000185 x^2, Q24 */
    lin = fx_umuldiv( ri, 1UL << 24, 20000 * half );   /* .0001 x, Q24 */
    return fx_umuldiv( atan_q24( (s32)t1 ), 131, 10 ) + fx_umuldiv( atan_q24( (s32)t2 ), 224, 100 )
           + lin;
}

static int floor0_map( FLOOR *fl, int which, u32 half )
{
    u16 *map;
    u32 i, top, val;

    map = (u16 *)try_alloc( half * sizeof( u16 ) );
    if ( map == NULL ) {
        return DE_NOMEM;
    }
    fl->map[which] = map;
    top = bark_q24( fl->rate, half, half );
    for ( i = 0; i < half; i++ ) {
        val = top ? fx_umuldiv( bark_q24( fl->rate, i, half ), fl->bark, top ) : 0;
        map[i] = (u16)(val < fl->bark ? val : fl->bark - 1U);
    }
    return DE_OK;
}

static int floor0_cos( FLOOR *fl )
{
    u32 k;
    s32 sinw;

    fl->cosw = (s32 *)try_alloc( (u32)fl->bark * sizeof( s32 ) );
    if ( fl->cosw == NULL ) {
        return DE_NOMEM;
    }
    for ( k = 0; k < fl->bark; k++ ) {
        vb_sincos( fx_umuldiv( k, 0x80000000UL, fl->bark ), &fl->cosw[k], &sinw );
    }
    return DE_OK;
}

static int floor0_read( VB *vb, FLOOR *fl, BITS *br )
{
    int i;

    fl->order0 = (u8)bits_get( br, 8 );
    fl->rate = (u16)bits_get( br, 16 );
    fl->bark = (u16)bits_get( br, 16 );
    fl->ampbits = (u8)bits_get( br, 6 );
    fl->ampoff = (u8)bits_get( br, 8 );
    fl->nbooks = (u8)(bits_get( br, 4 ) + 1);
    for ( i = 0; i < fl->nbooks; i++ ) {
        fl->books[i] = (u8)bits_get( br, 8 );
        if ( fl->books[i] >= vb->nbooks || vb->books[fl->books[i]].lookup == 0 ) {
            return DE_FORMAT;
        }
        vb->books[fl->books[i]].in_f0 = 1;
    }
    if ( br->eop || fl->rate == 0 || fl->bark == 0 ) {
        return DE_FORMAT;
    }
    if ( fl->ampbits > 31 || fl->bark > MAX_BARK || !setup_charge( &vb->budget, fl->bark ) ) {
        return DE_UNSUPPORTED;
    }
    return DE_OK;
}

/* ------------------------------------------------------------------ */
/* residues and mappings: setup                                        */
/* ------------------------------------------------------------------ */
static int resid_read( VB *vb, RESID *rs, BITS *br )
{
    int i, j;
    u32 cascade[64], high, book;

    rs->begin = bits_get( br, 24 );
    rs->end = bits_get( br, 24 );
    rs->psize = bits_get( br, 24 ) + 1;
    rs->classes = (u8)(bits_get( br, 6 ) + 1);
    rs->classbook = (u8)bits_get( br, 8 );
    if ( rs->classbook >= vb->nbooks ) {
        return DE_FORMAT;
    }
    for ( i = 0; i < rs->classes; i++ ) {
        cascade[i] = bits_get( br, 3 );
        if ( bits_get( br, 1 ) ) {
            high = bits_get( br, 5 );
            cascade[i] |= high << 3;
        }
    }
    rs->passes = 0;
    for ( i = 0; i < rs->classes; i++ ) {
        for ( j = 0; j < 8; j++ ) {
            rs->books[i][j] = -1;
            if ( cascade[i] & (1U << j) ) {
                book = bits_get( br, 8 );
                if ( book >= (u32)vb->nbooks || vb->books[book].lookup == 0
                     || rs->psize % vb->books[book].dims != 0 ) {
                    return DE_FORMAT;
                }
                rs->books[i][j] = (s16)book;
                vb->books[book].in_res = 1;
                if ( j + 1 > rs->passes ) {
                    rs->passes = (u8)(j + 1);
                }
            }
        }
    }
    return br->eop ? DE_FORMAT : DE_OK;
}

/* each classbook entry's class numbers, the first partition's first */
static int resid_classes( VB *vb, RESID *rs )
{
    const BOOK *cb = &vb->books[rs->classbook];
    u32 e, i, val;

    rs->cdims = cb->dims;
    if ( cb->dims == 0 || cb->dims > 64 || !pow_le( rs->classes, cb->dims, cb->entries ) ) {
        return DE_FORMAT;
    }
    if ( cb->entries > MAX_VALUES / cb->dims
         || !setup_charge( &vb->budget, cb->entries * cb->dims ) ) {
        return DE_UNSUPPORTED;          /* a table of classes bigger than any real stream needs */
    }
    rs->ctab = (u8 *)try_alloc( cb->entries * cb->dims );
    if ( rs->ctab == NULL ) {
        return DE_NOMEM;
    }
    for ( e = 0; e < cb->entries; e++ ) {
        val = e;
        for ( i = cb->dims; i-- > 0; ) {
            rs->ctab[e * cb->dims + i] = (u8)(val % rs->classes);
            val /= rs->classes;
        }
    }
    return DE_OK;
}

static int map_read( VB *vb, MAP *mp, BITS *br )
{
    int i, bits = ilog( (u32)vb->nch - 1 );

    if ( bits_get( br, 16 ) != 0 ) {
        return DE_FORMAT;
    }
    mp->submaps = (u8)(bits_get( br, 1 ) ? bits_get( br, 4 ) + 1 : 1);
    mp->steps = 0;
    if ( bits_get( br, 1 ) ) {
        mp->steps = (u16)(bits_get( br, 8 ) + 1);
        for ( i = 0; i < mp->steps; i++ ) {
            mp->mag[i] = (u8)bits_get( br, bits );
            mp->ang[i] = (u8)bits_get( br, bits );
            if ( mp->mag[i] == mp->ang[i] || mp->mag[i] >= vb->nch || mp->ang[i] >= vb->nch ) {
                return DE_FORMAT;
            }
        }
    }
    if ( bits_get( br, 2 ) != 0 ) {
        return DE_FORMAT;
    }
    for ( i = 0; i < vb->nch; i++ ) {
        mp->mux[i] = 0;
        if ( mp->submaps > 1 ) {
            mp->mux[i] = (u8)bits_get( br, 4 );
            if ( mp->mux[i] >= mp->submaps ) {
                return DE_FORMAT;
            }
        }
    }
    for ( i = 0; i < mp->submaps; i++ ) {
        bits_get( br, 8 );
        mp->floor[i] = (u8)bits_get( br, 8 );
        mp->resid[i] = (u8)bits_get( br, 8 );
        if ( mp->floor[i] >= vb->nfloors || mp->resid[i] >= vb->nres ) {
            return DE_FORMAT;
        }
    }
    return br->eop ? DE_FORMAT : DE_OK;
}

/* ------------------------------------------------------------------ */
/* the headers                                                         */
/* ------------------------------------------------------------------ */
static int is_header( const u8 *pkt, u32 len, int type )
{
    return len >= 7 && pkt[0] == type && mem_cmp( pkt + 1, "vorbis", 6 ) == 0;
}

int vb_ident( VBID *id, const u8 *pkt, u32 len )
{
    mem_set( id, 0, sizeof( VBID ) );
    if ( len < 30 || !is_header( pkt, len, 1 ) ) {
        return DE_FORMAT;
    }
    id->channels = pkt[11];
    id->rate = LE32( pkt + 12 );
    id->br_max = (s32)LE32( pkt + 16 );
    id->br_nom = (s32)LE32( pkt + 20 );
    id->br_min = (s32)LE32( pkt + 24 );
    id->log0 = pkt[28] & 15;
    id->log1 = pkt[28] >> 4;
    if ( LE32( pkt + 7 ) != 0 || id->channels == 0 || id->rate == 0 || !(pkt[29] & 1)
         || id->log0 < VB_MIN_LOG || id->log1 > VB_MAX_LOG || id->log0 > id->log1 ) {
        return DE_FORMAT;
    }
    if ( id->channels > VB_MAX_CH || id->rate > 768000UL ) {
        return DE_UNSUPPORTED;
    }
    return DE_OK;
}

/* "NAME=" at the start of a comment, any case: -> where the value starts, or 0 */
static u32 tag_name( const u8 *field, u32 len, const char *name )
{
    u32 nl = str_len( name );

    if ( len > nl && field[nl] == '=' && str_nicmp( (const char *)field, name, nl ) == 0 ) {
        return nl + 1;
    }
    return 0;
}

void vb_comment( const u8 *pkt, u32 len, DECINFO *info )
{
    u32 at = 7, size, count, i, val;

    if ( !is_header( pkt, len, 3 ) || len < at + 4 ) {
        return;
    }
    size = LE32( pkt + at );
    if ( size > len - at - 4 ) {
        return;
    }
    at += 4 + size;
    if ( len - at < 4 ) {
        return;
    }
    count = LE32( pkt + at );
    at += 4;
    for ( i = 0; i < count && len - at >= 4; i++ ) {
        size = LE32( pkt + at );
        at += 4;
        if ( size > len - at ) {
            return;
        }
        if ( (val = tag_name( pkt + at, size, "TITLE" )) != 0 && info->title[0] == 0 ) {
            tag_utf8( info->title, TAG_LEN, pkt + at + val, size - val );
        } else if ( (val = tag_name( pkt + at, size, "ARTIST" )) != 0 && info->artist[0] == 0 ) {
            tag_utf8( info->artist, TAG_LEN, pkt + at + val, size - val );
        } else if ( (val = tag_name( pkt + at, size, "ALBUM" )) != 0 && info->album[0] == 0 ) {
            tag_utf8( info->album, TAG_LEN, pkt + at + val, size - val );
        }
        at += size;
    }
}

/* ------------------------------------------------------------------ */
/* opening and closing                                                 */
/* ------------------------------------------------------------------ */
void vb_close( VB *vb )
{
    int i;

    if ( vb == NULL ) {
        return;
    }
    if ( vb->books != NULL ) {
        for ( i = 0; i < vb->nbooks; i++ ) {
            xfree( vb->books[i].lut );
            xfree( vb->books[i].long_code );
            xfree( vb->books[i].long_val );
            xfree( vb->books[i].vals );
            xfree( vb->books[i].mult );
        }
        xfree( vb->books );
    }
    if ( vb->floors != NULL ) {
        for ( i = 0; i < vb->nfloors; i++ ) {
            xfree( vb->floors[i].map[0] );
            xfree( vb->floors[i].map[1] );
            xfree( vb->floors[i].cosw );
        }
        xfree( vb->floors );
    }
    if ( vb->res != NULL ) {
        for ( i = 0; i < vb->nres; i++ ) {
            xfree( vb->res[i].ctab );
        }
        xfree( vb->res );
    }
    xfree( vb->maps );
    for ( i = 0; i < VB_MAX_CH; i++ ) {
        xfree( vb->fmant[i] );
        xfree( vb->fshift[i] );
        xfree( vb->vec[i] );
    }
    for ( i = 0; i < 2; i++ ) {
        xfree( vb->mixed[i] );
        xfree( vb->ubuf[i][0] );
        xfree( vb->ubuf[i][1] );
        vb_mdct_free( &vb->md[i] );
    }
    vb_fft_free( &vb->fft );
    xfree( vb->work );
    xfree( vb->cls );
    xfree( vb->f0coef );
    xfree( vb->pcm );
    xfree( vb->eqgain );
    xfree( vb );
}

/* the weights for mixing a stream's channels to two, in halves: the
   channel orders of the specification's section 4.3.9 */
static const u8 mix_left[9][8] = {
    { 0 }, { 2 }, { 2, 0 }, { 2, 1, 0 }, { 2, 0, 2, 0 }, { 2, 1, 0, 2, 0 },
    { 2, 1, 0, 2, 0, 1 }, { 2, 1, 0, 2, 0, 1, 1 }, { 2, 1, 0, 2, 0, 2, 0, 1 }
};
static const u8 mix_right[9][8] = {
    { 0 }, { 2 }, { 0, 2 }, { 0, 1, 2 }, { 0, 2, 0, 2 }, { 0, 1, 2, 0, 2 },
    { 0, 1, 2, 0, 2, 1 }, { 0, 1, 2, 0, 2, 1, 1 }, { 0, 1, 2, 0, 2, 0, 2, 1 }
};

static void mix_setup( VB *vb )
{
    int ch, nch = vb->nch;
    u32 total = 0, wl, wr;

    if ( nch == vb->opt.out_ch ) {
        vb->mixing = 0;
        for ( ch = 0; ch < nch; ch++ ) {
            vb->gain[ch] = 0x40000000L;
            vb->side[ch] = (u8)(1 << ch);
        }
        return;
    }
    vb->mixing = 1;
    for ( ch = 0; ch < nch; ch++ ) {
        total += mix_left[nch][ch];
    }
    for ( ch = 0; ch < nch; ch++ ) {
        wl = mix_left[nch][ch];
        wr = mix_right[nch][ch];
        if ( vb->opt.out_ch == 1 ) {
            vb->gain[ch] = (s32)fx_umuldiv( wl + wr, 0x40000000UL, 2 * total );
            vb->side[ch] = 1;
        } else {
            vb->gain[ch] = (s32)fx_umuldiv( wl > wr ? wl : wr, 0x40000000UL, total );
            vb->side[ch] = (u8)((wl ? 1 : 0) | (wr ? 2 : 0));
        }
    }
}

/* each channel's floor 1 table: the standard's values, its weight in
   them, and the shift that leaves residue times value in Q24 */
static int floor_tables( VB *vb )
{
    int ch, y;
    u32 mant;
    int exp, shift;

    for ( ch = 0; ch < vb->nch; ch++ ) {
        vb->fmant[ch] = (u32 *)try_alloc( 256 * sizeof( u32 ) );
        vb->fshift[ch] = (u8 *)try_alloc( 256 );
        if ( vb->fmant[ch] == NULL || vb->fshift[ch] == NULL ) {
            return DE_NOMEM;
        }
        for ( y = 0; y < 256; y++ ) {
            mant = vb_floor1_mant[y];
            exp = vb_floor1_exp[y];
            if ( vb->gain[ch] != 0x40000000L ) {
                mant = (u32)vb_mul30r( (s32)mant, vb->gain[ch] );
                while ( mant != 0 && mant < 0x40000000UL ) {
                    mant <<= 1;
                    exp++;
                }
            }
            shift = 6 + vb->qr + exp;
            if ( mant == 0 || shift > 63 ) {
                mant = 0;
                shift = 32;
            }
            vb->fmant[ch][y] = mant;
            vb->fshift[ch][y] = (u8)shift;
        }
    }
    return DE_OK;
}

/* the residue's binary point: the largest sum a partition's passes can
   make, with two bits to spare for uncoupling */
static int choose_qr( VB *vb )
{
    int r, c, j, bits, most = -8, top, count, q;
    const RESID *rs;

    for ( r = 0; r < vb->nres; r++ ) {
        rs = &vb->res[r];
        for ( c = 0; c < rs->classes; c++ ) {
            top = -8;
            count = 0;
            for ( j = 0; j < 8; j++ ) {
                if ( rs->books[c][j] >= 0 ) {
                    bits = book_bits( &vb->books[rs->books[c][j]] );
                    if ( bits > top ) {
                        top = bits;
                    }
                    count++;
                }
            }
            if ( count > 0 ) {
                bits = top + ilog( (u32)count - 1 );
                if ( bits > most ) {
                    most = bits;
                }
            }
        }
    }
    q = 30 - 2 - most;
    return q < 0 ? 0 : (q > 24 ? 24 : q);
}

static int setup_read( VB *vb, BITS *br )
{
    int i, rc, type;
    u32 count;

    vb->nbooks = (int)bits_get( br, 8 ) + 1;
    vb->books = (BOOK *)try_alloc( vb->nbooks * sizeof( BOOK ) );
    if ( vb->books == NULL ) {
        vb->nbooks = 0;
        return DE_NOMEM;
    }
    mem_set( vb->books, 0, vb->nbooks * sizeof( BOOK ) );
    for ( i = 0; i < vb->nbooks; i++ ) {
        rc = book_read( &vb->books[i], br, &vb->budget );
        if ( rc != DE_OK ) {
            return rc;
        }
    }
    count = bits_get( br, 6 ) + 1;
    for ( i = 0; i < (int)count; i++ ) {
        if ( bits_get( br, 16 ) != 0 ) {
            return DE_FORMAT;
        }
    }
    vb->nfloors = (int)bits_get( br, 6 ) + 1;
    vb->floors = (FLOOR *)try_alloc( vb->nfloors * sizeof( FLOOR ) );
    if ( vb->floors == NULL ) {
        vb->nfloors = 0;
        return DE_NOMEM;
    }
    mem_set( vb->floors, 0, vb->nfloors * sizeof( FLOOR ) );
    for ( i = 0; i < vb->nfloors; i++ ) {
        type = (int)bits_get( br, 16 );
        vb->floors[i].type = (u8)type;
        if ( type == 0 ) {
            rc = floor0_read( vb, &vb->floors[i], br );
        } else if ( type == 1 ) {
            rc = floor1_read( vb, &vb->floors[i], br );
        } else {
            rc = DE_FORMAT;
        }
        if ( rc != DE_OK ) {
            return rc;
        }
    }
    vb->nres = (int)bits_get( br, 6 ) + 1;
    vb->res = (RESID *)try_alloc( vb->nres * sizeof( RESID ) );
    if ( vb->res == NULL ) {
        vb->nres = 0;
        return DE_NOMEM;
    }
    mem_set( vb->res, 0, vb->nres * sizeof( RESID ) );
    for ( i = 0; i < vb->nres; i++ ) {
        type = (int)bits_get( br, 16 );
        vb->res[i].type = (u8)type;
        if ( type > 2 ) {
            return DE_FORMAT;
        }
        rc = resid_read( vb, &vb->res[i], br );
        if ( rc != DE_OK ) {
            return rc;
        }
    }
    vb->nmaps = (int)bits_get( br, 6 ) + 1;
    vb->maps = (MAP *)try_alloc( vb->nmaps * sizeof( MAP ) );
    if ( vb->maps == NULL ) {
        vb->nmaps = 0;
        return DE_NOMEM;
    }
    mem_set( vb->maps, 0, vb->nmaps * sizeof( MAP ) );
    for ( i = 0; i < vb->nmaps; i++ ) {
        rc = map_read( vb, &vb->maps[i], br );
        if ( rc != DE_OK ) {
            return rc;
        }
    }
    vb->nmodes = (int)bits_get( br, 6 ) + 1;
    for ( i = 0; i < vb->nmodes; i++ ) {
        vb->mode_flag[i] = (u8)bits_get( br, 1 );
        if ( bits_get( br, 16 ) != 0 || bits_get( br, 16 ) != 0 ) {
            return DE_FORMAT;
        }
        vb->mode_map[i] = (u8)bits_get( br, 8 );
        if ( vb->mode_map[i] >= vb->nmaps ) {
            return DE_FORMAT;
        }
    }
    if ( !bits_get( br, 1 ) || br->eop ) {
        return DE_FORMAT;
    }
    vb->mode_bits = ilog( (u32)vb->nmodes - 1 );
    return DE_OK;
}

/* everything decoding needs, once the setup is read */
static int setup_build( VB *vb )
{
    int i, b, rc, down = vb->opt.down, out = vb->opt.out_ch;
    u32 lines = (vb->n[1] >> down) / 2, room, parts;
    const RESID *rs;

    vb->qr = choose_qr( vb );
    for ( i = 0; i < vb->nbooks; i++ ) {
        if ( vb->books[i].lookup != 0 && (vb->books[i].in_res || vb->books[i].in_f0) ) {
            rc = book_values( &vb->books[i], vb->books[i].in_res ? vb->qr : 24 );
            if ( rc != DE_OK ) {
                return rc;
            }
        }
        xfree( vb->books[i].mult );
        vb->books[i].mult = NULL;
    }
    room = 1;
    for ( i = 0; i < vb->nres; i++ ) {
        rc = resid_classes( vb, &vb->res[i] );
        if ( rc != DE_OK ) {
            return rc;
        }
        rs = &vb->res[i];
        parts = (rs->type == 2 ? (vb->n[1] / 2) * (u32)vb->nch : vb->n[1] / 2) / rs->psize;
        if ( parts > room ) {
            room = parts;
        }
    }
    vb->cls_room = room;
    vb->cls = (u8 *)try_alloc( room * (u32)vb->nch );
    if ( vb->cls == NULL ) {
        return DE_NOMEM;
    }
    for ( i = 0; i < vb->nfloors; i++ ) {
        if ( vb->floors[i].type == 0 ) {
            for ( b = 0; b < 2; b++ ) {
                rc = floor0_map( &vb->floors[i], b, vb->n[b] / 2 );
                if ( rc != DE_OK ) {
                    return rc;
                }
            }
            rc = floor0_cos( &vb->floors[i] );
            if ( rc != DE_OK ) {
                return rc;
            }
            if ( vb->f0coef == NULL ) {
                vb->f0coef = (s32 *)try_alloc( (u32)vb->nch * F0_MAX * sizeof( s32 ) );
                if ( vb->f0coef == NULL ) {
                    return DE_NOMEM;
                }
            }
        }
    }
    mix_setup( vb );
    rc = floor_tables( vb );
    if ( rc != DE_OK ) {
        return rc;
    }
    for ( i = 0; i < vb->nch; i++ ) {
        vb->vec[i] = (s32 *)try_alloc( lines * sizeof( s32 ) );
        if ( vb->vec[i] == NULL ) {
            return DE_NOMEM;
        }
    }
    for ( i = 0; i < out; i++ ) {
        if ( vb->mixing ) {
            vb->mixed[i] = (s32 *)try_alloc( lines * sizeof( s32 ) );
        }
        vb->ubuf[i][0] = (s32 *)try_alloc( lines * sizeof( s32 ) );
        vb->ubuf[i][1] = (s32 *)try_alloc( lines * sizeof( s32 ) );
        if ( (vb->mixing && vb->mixed[i] == NULL) || vb->ubuf[i][0] == NULL
             || vb->ubuf[i][1] == NULL ) {
            return DE_NOMEM;
        }
    }
    vb->work = (s32 *)try_alloc( lines * sizeof( s32 ) );
    vb->pcm = (s16 *)try_alloc( lines * (u32)out * sizeof( s16 ) );
    if ( vb->work == NULL || vb->pcm == NULL ) {
        return DE_NOMEM;
    }
    if ( vb_mdct_init( &vb->md[0], vb->id.log0 - down ) != 0
         || vb_mdct_init( &vb->md[1], vb->id.log1 - down ) != 0
         || vb_fft_init( &vb->fft, vb->id.log1 - down - 2 ) != 0 ) {
        return DE_NOMEM;
    }
    return DE_OK;
}

int vb_open( VB **vbp, const VBID *id, const u8 *pkt, u32 len, const VBOPT *opt )
{
    VB *vb;
    BITS br;
    int rc;

    *vbp = NULL;
    if ( !is_header( pkt, len, 5 ) ) {
        return DE_FORMAT;
    }
    vb = (VB *)try_alloc( sizeof( VB ) );
    if ( vb == NULL ) {
        return DE_NOMEM;
    }
    mem_set( vb, 0, sizeof( VB ) );
    vb->id = *id;
    vb->opt = *opt;
    vb->nch = (int)id->channels;
    vb->n[0] = 1UL << id->log0;
    vb->n[1] = 1UL << id->log1;
    vb->budget = SETUP_WORK;
    bits_init( &br, pkt + 7, len - 7 );
    rc = setup_read( vb, &br );
    if ( rc == DE_OK ) {
        rc = setup_build( vb );
    }
    if ( rc != DE_OK ) {
        vb_close( vb );
        return rc;
    }
    *vbp = vb;
    return DE_OK;
}

void vb_restart( VB *vb )
{
    vb->prev_n = 0;
}

/* ------------------------------------------------------------------ */
/* floor 1 in a packet                                                 */
/* ------------------------------------------------------------------ */
/* -> 1 with the points in y, or 0: the channel is silent */
static int floor1_decode( const VB *vb, const FLOOR *fl, BITS *br, s32 *y )
{
    int rbits = ilog( f1_range[fl->mult - 1] - 1U ), i, j, off = 2, cls, cbits, book;
    u32 cval, csub;
    s32 e;

    if ( !bits_small( br, 1 ) ) {
        return 0;
    }
    y[0] = (s32)bits_small( br, rbits );
    y[1] = (s32)bits_small( br, rbits );
    for ( i = 0; i < fl->parts; i++ ) {
        cls = fl->pclass[i];
        cbits = fl->cbits[cls];
        csub = (1UL << cbits) - 1;
        cval = 0;
        if ( cbits > 0 ) {
            e = book_decode( &vb->books[fl->cmaster[cls]], br );
            if ( e < 0 ) {
                return 0;
            }
            cval = (u32)e;
        }
        for ( j = 0; j < fl->cdim[cls]; j++ ) {
            book = fl->subbook[cls][cval & csub];
            cval >>= cbits;
            if ( book >= 0 ) {
                e = book_decode( &vb->books[book], br );
                if ( e < 0 ) {
                    return 0;
                }
                y[off + j] = e;
            } else {
                y[off + j] = 0;
            }
        }
        off += fl->cdim[cls];
    }
    return !br->eop;
}

/* the point on the line from (x0, y0) to (x1, y1) at x */
static s32 render_point( s32 x0, s32 y0, s32 x1, s32 y1, s32 x )
{
    s32 dy = y1 - y0, ady = dy < 0 ? -dy : dy, off;

    off = ady * (x - x0) / (x1 - x0);
    return dy < 0 ? y0 - off : y0 + off;
}

/* step 1: the points' final values, each kept in [0, range) as it is
   made (a valid stream never leaves it) */
static void floor1_final( const FLOOR *fl, s32 *y, u8 *flag )
{
    s32 range = f1_range[fl->mult - 1], pred, val, high, low, room;
    int i, lo, hi;

    flag[0] = flag[1] = 1;
    for ( i = 0; i < 2; i++ ) {
        if ( y[i] >= range ) {
            y[i] = range - 1;
        }
    }
    for ( i = 2; i < fl->nval; i++ ) {
        lo = fl->lo[i];
        hi = fl->hi[i];
        pred = render_point( fl->x[lo], y[lo], fl->x[hi], y[hi], fl->x[i] );
        val = y[i];
        high = range - pred;
        low = pred;
        room = (high < low ? high : low) * 2;
        if ( val != 0 ) {
            flag[lo] = flag[hi] = flag[i] = 1;
            if ( val >= room ) {
                val = high > low ? val - low + pred : pred - val + high - 1;
            } else if ( val & 1 ) {
                val = pred - (val + 1) / 2;
            } else {
                val = pred + val / 2;
            }
        } else {
            flag[i] = 0;
            val = pred;
        }
        y[i] = val < 0 ? 0 : (val >= range ? range - 1 : val);
    }
}

/* residue times floor value, back in Q24: rounded at the last place */
#define FMUL( res, mant, shift ) \
    ((shift) <= 32 ? (fx_mulsh( (s32)(mant), (res), (shift) - 1 ) + 1) >> 1 \
                   : ((fx_mulhi( (s32)(mant), (res) ) >> ((shift) - 33)) + 1) >> 1)

/* one of the specification's integer lines, from x0 up to x1 (not
   included), each point multiplying the residue line under it */
static void floor1_line( s32 *vec, u32 x0, s32 y0, u32 x1, s32 y1, u32 limit,
                         const u32 *mant, const u8 *shift )
{
    s32 dy = y1 - y0, base, step, y = y0, res;
    u32 adx = x1 - x0, ady, err = 0, x, end;
    int sh;

    if ( x0 >= limit || x1 <= x0 ) {
        return;
    }
    ady = (u32)(dy < 0 ? -dy : dy);
    base = (s32)(ady / adx);
    ady -= (u32)base * adx;
    if ( dy < 0 ) {
        base = -base;
        step = base - 1;
    } else {
        step = base + 1;
    }
    end = x1 < limit ? x1 : limit;
    for ( x = x0; ; ) {
        res = vec[x];
        if ( res != 0 ) {
            sh = shift[y];
            vec[x] = FMUL( res, mant[y], sh );
        }
        if ( ++x >= end ) {
            break;
        }
        err += ady;
        if ( err >= adx ) {
            err -= adx;
            y += step;
        } else {
            y += base;
        }
    }
}

/* step 2: the curve through the points that count, into the residue */
static void floor1_apply( const VB *vb, const FLOOR *fl, int ch, u32 half, u32 limit )
{
    s32 y[F1_MAX];
    u8 flag[F1_MAX];
    s32 lx = 0, ly, hx = 0, hy;
    int k, i;

    mem_cpy( y, vb->fy[ch], fl->nval * sizeof( s32 ) );
    floor1_final( fl, y, flag );
    ly = y[fl->order[0]] * fl->mult;
    hy = ly;
    for ( k = 1; k < fl->nval; k++ ) {
        i = fl->order[k];
        if ( flag[i] ) {
            hx = fl->x[i];
            hy = y[i] * fl->mult;
            floor1_line( vb->vec[ch], (u32)lx, ly, (u32)hx, hy, limit, vb->fmant[ch],
                         vb->fshift[ch] );
            lx = hx;
            ly = hy;
        }
    }
    if ( (u32)hx < half ) {
        floor1_line( vb->vec[ch], (u32)hx, hy, half, hy, limit, vb->fmant[ch], vb->fshift[ch] );
    }
}

/* ------------------------------------------------------------------ */
/* floor 0 in a packet                                                 */
/* ------------------------------------------------------------------ */
static int floor0_decode( const VB *vb, const FLOOR *fl, BITS *br, s32 *amp, s32 *coef )
{
    u32 bn, count, j;
    const BOOK *bk;
    const s32 *row;
    s32 e, last = 0, val;
    int up;

    *amp = (s32)bits_get( br, fl->ampbits );
    if ( *amp <= 0 || br->eop ) {
        return 0;
    }
    bn = bits_get( br, ilog( fl->nbooks ) );
    if ( br->eop || bn >= fl->nbooks ) {
        return 0;
    }
    bk = &vb->books[fl->books[bn]];
    up = 24 - bk->vq;
    count = 0;
    do {
        e = book_decode( bk, br );
        if ( e < 0 ) {
            return 0;
        }
        row = bk->vals + (u32)e * bk->dims;
        for ( j = 0; j < bk->dims; j++ ) {
            val = sat_add( up > 0 ? row[j] << up : row[j], last );
            if ( count + j < fl->order0 ) {
                coef[count + j] = val;
            }
            if ( j == bk->dims - 1 ) {
                last = val;
            }
        }
        count += bk->dims;
    } while ( count < fl->order0 );
    return 1;
}

static void vbf_set( VBF *f, u32 hi, u32 lo, int e )
{
    int top;

    if ( hi != 0 ) {
        top = fx_topbit( hi );
        f->m = top == 31 ? hi : (hi << (31 - top)) | (lo >> (top + 1));
        f->e = e + top + 1;
    } else if ( lo != 0 ) {
        top = fx_topbit( lo );
        f->m = lo << (31 - top);
        f->e = e + top - 31;
    } else {
        f->m = 0;
        f->e = 0;
    }
}

static void vbf_mul( VBF *f, const VBF *g )
{
    u32 hi, lo;

    if ( f->m == 0 || g->m == 0 ) {
        f->m = 0;
        return;
    }
    hi = vb_umulhi( f->m, g->m );
    lo = f->m * g->m;
    vbf_set( f, hi, lo, f->e + g->e );
}

static void vbf_add( VBF *f, const VBF *g )
{
    VBF big = *f, small = *g;
    u32 sum, part;
    int diff;

    if ( g->m == 0 ) {
        return;
    }
    if ( f->m == 0 || g->e > f->e ) {
        big = *g;
        small = *f;
    }
    if ( small.m == 0 ) {
        *f = big;
        return;
    }
    diff = big.e - small.e;
    part = diff >= 32 ? 0 : small.m >> diff;
    sum = big.m + part;
    if ( sum < big.m ) {
        f->m = (sum >> 1) | 0x80000000UL;
        f->e = big.e + 1;
    } else {
        f->m = sum;
        f->e = big.e;
    }
}

/* log2 in Q16, from the table: the eight bits under the leading one
   pick a step, the next sixteen say how far into it */
static s32 vbf_log2( const VBF *f )
{
    u32 idx = (f->m >> 23) & 255, part = (f->m >> 7) & 0xFFFF, frac;

    frac = vb_log2_tab[idx] + (((vb_log2_tab[idx + 1] - vb_log2_tab[idx]) * part) >> 16);
    return (s32)(((s32)f->e + 31) * 65536L + (s32)frac);
}

/* The product of |cos c - cos w| over every other coefficient from
   "first" on: one multiply a coefficient, kept normalized.  The square
   and the fours are put on afterwards. */
static void lsp_product( const s32 *cosc, u32 first, u32 order, s32 cosw, VBF *out )
{
    u32 k, m = 0x80000000UL, hi, d;
    int e = -31, top;
    s32 diff;

    for ( k = first; k < order; k += 2 ) {
        diff = (cosc[k] >> 1) - (cosw >> 1);            /* Q29 */
        d = (u32)(diff < 0 ? -diff : diff);
        hi = vb_umulhi( m, d );                         /* m 2^e d 2^-29 = hi 2^(e + 3) */
        if ( hi == 0 ) {
            out->m = 0;
            out->e = 0;
            return;
        }
        top = fx_topbit( hi );
        m = hi << (31 - top);
        e += 3 - (31 - top);
    }
    out->m = m;
    out->e = e;
}

/* 2^(f / 65536) for f in [0, 65536), Q30 */
static u32 exp2_q30( u32 frac )
{
    u32 idx = frac >> 8, part = frac & 255;

    return vb_exp2_tab[idx] + (((vb_exp2_tab[idx + 1] - vb_exp2_tab[idx]) * part) >> 8);
}

/* The curve at one Bark position as a mantissa and the FMUL shift, the
   channel's weight included:
   exp( .11512925 ( A / sqrt(p + q) - offset ) ), A = amp * offset / (2^bits - 1) */
static void floor0_value( const VB *vb, const FLOOR *fl, int ch, s32 amp, const VBF *sum,
                          u32 *mant_out, int *shift_out )
{
    s32 a16, half, neg, ip, term, diff, z;
    u32 m;
    int exp, shift;

    a16 = (s32)fx_umuldiv( (u32)amp, (u32)fl->ampoff << 16, (1UL << fl->ampbits) - 1 );
    if ( sum->m == 0 ) {
        term = 0x7FFFFFFFL;
    } else {
        half = vbf_log2( sum ) >> 1;
        neg = -half;
        ip = neg >> 16;
        term = fx_mul30( a16, (s32)exp2_q30( (u32)neg & 0xFFFF ) );
        if ( ip >= 0 ) {
            term = ip >= 15 || term > (0x7FFFFFFFL >> ip) ? 0x7FFFFFFFL : term << ip;
        } else {
            term = -ip >= 31 ? 0 : term >> -ip;
        }
    }
    diff = term - ((s32)fl->ampoff << 16);
    z = fx_mulhi( diff, 713378626L );                   /* log2(10) / 20 */
    if ( z > 4 * 65536L ) {
        z = 4 * 65536L;
    } else if ( z < -60 * 65536L ) {
        z = -60 * 65536L;
    }
    m = exp2_q30( (u32)z & 0xFFFF );
    exp = -(int)(z >> 16);
    if ( vb->gain[ch] != 0x40000000L ) {
        m = (u32)vb_mul30r( (s32)m, vb->gain[ch] );
        while ( m != 0 && m < 0x40000000UL ) {
            m <<= 1;
            exp++;
        }
    }
    shift = 6 + vb->qr + exp;
    if ( m == 0 || shift > 63 ) {
        m = 0;
        shift = 32;
    } else if ( shift < 2 ) {
        shift = 2;
    }
    *mant_out = m;
    *shift_out = shift;
}

/* the curve times the residue.  Runs of lines on one Bark position
   share a value, and a run whose residue is all zero needs none. */
static void floor0_apply( VB *vb, const FLOOR *fl, int ch, int which, u32 limit )
{
    const u16 *map = fl->map[which];
    const s32 *coef = vb->f0coef + ch * F0_MAX;
    s32 cosc[F0_MAX], cosw, sinw, res, any;
    VBF p, q, pre;
    u32 i, k, run, mv, mant, order = fl->order0;
    int shift;
    s32 *vec = vb->vec[ch];

    for ( k = 0; k < order; k++ ) {
        /* radians (Q24) to turns: times 2^8 / (2 pi) */
        vb_sincos( (u32)fx_mul16( coef[k], 2670177L ), &cosc[k], &sinw );
    }
    for ( i = 0; i < limit; ) {
        mv = map[i];
        any = 0;
        for ( run = i; run < limit && map[run] == mv; run++ ) {
            any |= vec[run];
        }
        if ( any == 0 ) {
            i = run;
            continue;
        }
        cosw = fl->cosw[mv];
        lsp_product( cosc, 1, order, cosw, &p );
        lsp_product( cosc, 0, order, cosw, &q );
        vbf_mul( &p, &p );
        vbf_mul( &q, &q );
        p.e += 2 * (int)(order / 2);
        q.e += 2 * (int)((order + 1) / 2);
        if ( order & 1 ) {
            /* p times 1 - cos^2 = sin^2, q times 1/4 */
            vbf_set( &pre, 0, (u32)(0x40000000L - vb_mul30r( cosw, cosw )), -30 );
            vbf_mul( &p, &pre );
            q.e -= 2;
        } else {
            /* p times (1 - cos) / 2, q times (1 + cos) / 2, made from
               halves so that cos = 1 cannot overflow */
            vbf_set( &pre, 0, (u32)(0x20000000L - (cosw >> 1)), -30 );
            vbf_mul( &p, &pre );
            vbf_set( &pre, 0, (u32)(0x20000000L + (cosw >> 1)), -30 );
            vbf_mul( &q, &pre );
        }
        vbf_add( &p, &q );
        floor0_value( vb, fl, ch, vb->f0amp[ch], &p, &mant, &shift );
        for ( ; i < run; i++ ) {
            res = vec[i];
            if ( res != 0 ) {
                vec[i] = FMUL( res, mant, shift );
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* residue in a packet                                                 */
/* ------------------------------------------------------------------ */
/* one partition of format 0 or 1, "len" lines from "at" */
static int part_decode( const BOOK *bk, BITS *br, s32 *vec, u32 at, u32 len, u32 limit,
                        int type )
{
    u32 dims = bk->dims, i, j, step, pos;
    const s32 *row;
    s32 e;

    if ( type == 0 ) {
        step = len / dims;
        for ( i = 0; i < step; i++ ) {
            e = book_decode( bk, br );
            if ( e < 0 ) {
                return -1;
            }
            row = bk->vals + (u32)e * dims;
            for ( j = 0, pos = at + i; j < dims; j++, pos += step ) {
                if ( pos < limit ) {
                    vec[pos] += row[j];
                }
            }
        }
        return 0;
    }
    for ( i = 0; i < len; i += dims ) {
        e = book_decode( bk, br );
        if ( e < 0 ) {
            return -1;
        }
        pos = at + i;
        if ( pos + dims <= limit ) {
            row = bk->vals + (u32)e * dims;
            for ( j = 0; j < dims; j++ ) {
                vec[pos + j] += row[j];
            }
        } else if ( pos < limit ) {
            row = bk->vals + (u32)e * dims;
            for ( j = 0; j < dims && pos + j < limit; j++ ) {
                vec[pos + j] += row[j];
            }
        }
    }
    return 0;
}

/* one partition of format 2: the channels' lines interleaved */
static int part_decode2( const BOOK *bk, BITS *br, s32 **vecs, u32 count, u32 at, u32 len,
                         u32 limit )
{
    u32 dims = bk->dims, i, j, ch, line;
    const s32 *row;
    s32 *vec0, *vec1;
    s32 e;

    if ( count == 2 && (at & 1) == 0 && (dims & 1) == 0 ) {
        /* stereo, the usual case: pairs of lines */
        line = at >> 1;
        vec0 = vecs[0];
        vec1 = vecs[1];
        for ( i = 0; i < len; i += dims ) {
            e = book_decode( bk, br );
            if ( e < 0 ) {
                return -1;
            }
            row = bk->vals + (u32)e * dims;
            if ( line + dims / 2 <= limit ) {
                for ( j = 0; j < dims; j += 2 ) {
                    vec0[line] += row[j];
                    vec1[line] += row[j + 1];
                    line++;
                }
            } else {
                for ( j = 0; j < dims; j += 2 ) {
                    if ( line < limit ) {
                        vecs[0][line] += row[j];
                        vecs[1][line] += row[j + 1];
                    }
                    line++;
                }
            }
        }
        return 0;
    }
    ch = at % count;
    line = at / count;
    for ( i = 0; i < len; i += dims ) {
        e = book_decode( bk, br );
        if ( e < 0 ) {
            return -1;
        }
        row = bk->vals + (u32)e * dims;
        for ( j = 0; j < dims; j++ ) {
            if ( line < limit ) {
                vecs[ch][line] += row[j];
            }
            if ( ++ch == count ) {
                ch = 0;
                line++;
            }
        }
    }
    return 0;
}

static void resid_decode( VB *vb, const RESID *rs, s32 **vecs, const u8 *skip, u32 count,
                          u32 half, u32 limit, BITS *br )
{
    u32 actual = rs->type == 2 ? half * count : half, begin, end, parts, pc, pass, i, ch;
    u32 rows = rs->type == 2 ? 1 : count, room = vb->cls_room, cdims = rs->cdims, at;
    const BOOK *cb = &vb->books[rs->classbook];
    const u8 *cl;
    u8 none = 0, *dst;
    int book;
    s32 e;

    if ( rs->type == 2 ) {
        for ( ch = 0; ch < count && skip[ch]; ch++ ) {
        }
        if ( ch == count ) {
            return;
        }
        skip = &none;
    }
    begin = rs->begin < actual ? rs->begin : actual;
    end = rs->end < actual ? rs->end : actual;
    if ( end <= begin ) {
        return;
    }
    parts = (end - begin) / rs->psize;
    if ( parts > room ) {
        parts = room;
    }
    for ( pass = 0; pass < rs->passes; pass++ ) {
        for ( pc = 0; pc < parts; ) {
            if ( pass == 0 ) {
                for ( ch = 0; ch < rows; ch++ ) {
                    if ( skip[ch] ) {
                        continue;
                    }
                    e = book_decode( cb, br );
                    if ( e < 0 ) {
                        return;
                    }
                    cl = rs->ctab + (u32)e * cdims;
                    dst = vb->cls + ch * room + pc;
                    for ( i = 0; i < cdims && pc + i < parts; i++ ) {
                        dst[i] = cl[i];
                    }
                }
            }
            for ( i = 0; i < cdims && pc < parts; i++, pc++ ) {
                at = begin + pc * rs->psize;
                for ( ch = 0; ch < rows; ch++ ) {
                    if ( skip[ch] ) {
                        continue;
                    }
                    book = rs->books[vb->cls[ch * room + pc]][pass];
                    if ( book < 0 ) {
                        continue;
                    }
                    if ( rs->type == 2 ) {
                        if ( part_decode2( &vb->books[book], br, vecs, count, at, rs->psize,
                                           limit ) != 0 ) {
                            return;
                        }
                    } else if ( part_decode( &vb->books[book], br, vecs[ch], at, rs->psize,
                                             limit, rs->type ) != 0 ) {
                        return;
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* a packet                                                            */
/* ------------------------------------------------------------------ */
/* the mode of an audio packet, or -1 */
static int packet_mode( const VB *vb, BITS *br )
{
    u32 mode;

    if ( bits_small( br, 1 ) != 0 ) {
        return -1;
    }
    mode = bits_small( br, vb->mode_bits );
    if ( br->eop || mode >= (u32)vb->nmodes ) {
        return -1;
    }
    if ( vb->mode_flag[mode] ) {
        bits_small( br, 2 );                /* the window flags: the sizes say the same */
        if ( br->eop ) {
            return -1;
        }
    }
    return (int)mode;
}

u32 vb_block( const VB *vb, const u8 *pkt, u32 len )
{
    BITS br;
    int mode;

    if ( len == 0 ) {
        return 0;
    }
    bits_init( &br, pkt, len );
    mode = packet_mode( vb, &br );
    return mode < 0 ? 0 : vb->n[vb->mode_flag[mode]];
}

static void eq_update( VB *vb )
{
    const EQSET *eq = vb->opt.eq;
    int b, flat;
    u32 i, half;

    if ( eq == NULL ) {
        vb->use_eq = 0;
        return;
    }
    if ( vb->eq_valid && eq->serial == vb->eq_serial ) {
        return;
    }
    vb->eq_valid = 1;
    vb->eq_serial = eq->serial;
    vb->use_eq = 0;
    if ( !eq->on ) {
        return;
    }
    if ( vb->eqgain == NULL ) {
        vb->eqgain = (u32 *)try_alloc( (vb->n[1] / 2) * sizeof( u32 ) );
        if ( vb->eqgain == NULL ) {
            return;
        }
    }
    flat = 1;
    for ( b = 0; b < 2; b++ ) {
        half = vb->n[b] / 2;
        eq_curve( eq, vb->id.rate, half, vb->eqgain );
        for ( i = 0; i < (half >> vb->opt.down); i++ ) {
            if ( vb->eqgain[i] != 65536UL ) {
                flat = 0;
            }
        }
        if ( vb_mdct_eq( &vb->md[b], vb->eqgain ) != 0 ) {
            return;
        }
    }
    vb->use_eq = (u8)!flat;
}

u32 vb_decode( VB *vb, const u8 *pkt, u32 len, const s16 **pcm )
{
    BITS br;
    int mode, ch, j, sub, small, which, oc, out = vb->opt.out_ch, nch = vb->nch, silent;
    const MAP *mp;
    const FLOOR *fl;
    u8 noresidue[VB_MAX_CH], skip[VB_MAX_CH];
    s32 *bundle[VB_MAX_CH], *m, *a, *dst, *src, mv, av, sv, sneg, apos;
    u32 half, limit, n, i, count, frames = 0;
    s32 *u, *spec;

    *pcm = vb->pcm;
    if ( len == 0 ) {
        return 0;
    }
    bits_init( &br, pkt, len );
    mode = packet_mode( vb, &br );
    if ( mode < 0 ) {
        return 0;
    }
    which = vb->mode_flag[mode];
    mp = &vb->maps[vb->mode_map[mode]];
    half = vb->n[which] / 2;
    n = vb->n[which] >> vb->opt.down;
    limit = n / 2;

    /* the floors, in channel order */
    for ( ch = 0; ch < nch; ch++ ) {
        fl = &vb->floors[mp->floor[mp->mux[ch]]];
        if ( fl->type == 1 ) {
            vb->used[ch] = (u8)floor1_decode( vb, fl, &br, vb->fy[ch] );
        } else {
            vb->used[ch] = (u8)floor0_decode( vb, fl, &br, &vb->f0amp[ch],
                                              vb->f0coef + ch * F0_MAX );
        }
        noresidue[ch] = (u8)!vb->used[ch];
    }
    for ( i = 0; i < mp->steps; i++ ) {
        if ( !noresidue[mp->mag[i]] || !noresidue[mp->ang[i]] ) {
            noresidue[mp->mag[i]] = noresidue[mp->ang[i]] = 0;
        }
    }
    for ( ch = 0; ch < nch; ch++ ) {
        mem_set( vb->vec[ch], 0, limit * sizeof( s32 ) );
    }

    /* the residues, by submap */
    for ( sub = 0; sub < mp->submaps; sub++ ) {
        count = 0;
        for ( ch = 0; ch < nch; ch++ ) {
            if ( mp->mux[ch] == sub ) {
                bundle[count] = vb->vec[ch];
                skip[count] = noresidue[ch];
                count++;
            }
        }
        if ( count > 0 ) {
            resid_decode( vb, &vb->res[mp->resid[sub]], bundle, skip, count, half, limit, &br );
        }
    }

    /* Uncoupling, last pair first.  With s the angle given the sign of
       the magnitude, a positive angle leaves (M, M - s) and any other
       (M + s, M): the specification's four cases, without a branch on
       data that a processor cannot predict. */
    for ( j = (int)mp->steps - 1; j >= 0; j-- ) {
        m = vb->vec[mp->mag[j]];
        a = vb->vec[mp->ang[j]];
        for ( i = 0; i < limit; i++ ) {
            mv = m[i];
            av = a[i];
            sneg = (mv - 1) >> 31;          /* -1 when mv <= 0 (mv is far from the s32 limit) */
            apos = (-av) >> 31;             /* -1 when av > 0 */
            sv = (av ^ sneg) - sneg;
            m[i] = mv + (sv & ~apos);
            a[i] = mv - (sv & apos);
        }
    }

    /* each channel's spectrum: residue times floor, in place */
    for ( ch = 0; ch < nch; ch++ ) {
        if ( !vb->used[ch] ) {
            if ( !vb->mixing ) {
                mem_set( vb->vec[ch], 0, limit * sizeof( s32 ) );
            }
            continue;
        }
        fl = &vb->floors[mp->floor[mp->mux[ch]]];
        if ( fl->type == 1 ) {
            floor1_apply( vb, fl, ch, half, limit );
        } else {
            floor0_apply( vb, fl, ch, which, limit );
        }
    }

    eq_update( vb );
    for ( oc = 0; oc < out; oc++ ) {
        silent = 1;
        if ( vb->mixing ) {
            spec = vb->mixed[oc];
            mem_set( spec, 0, limit * sizeof( s32 ) );
            for ( ch = 0; ch < nch; ch++ ) {
                if ( vb->used[ch] && (vb->side[ch] & (1 << oc)) ) {
                    src = vb->vec[ch];
                    dst = spec;
                    for ( i = 0; i < limit; i++ ) {
                        dst[i] += src[i];
                    }
                    silent = 0;
                }
            }
        } else {
            spec = vb->vec[oc];
            silent = !vb->used[oc];
        }
        u = vb->ubuf[oc][vb->cur ^ 1];
        if ( silent ) {
            mem_set( u, 0, limit * sizeof( s32 ) );
        } else {
            vb_imdct( &vb->md[which], &vb->fft, spec, vb->work, u, vb->opt.fast, vb->use_eq );
        }
        if ( vb->prev_n != 0 ) {
            small = vb->prev_n < n ? 0 : which;
            frames = vb_lap( vb->pcm + oc, out, vb->ubuf[oc][vb->cur], vb->prev_n, u, n,
                             &vb->md[small], vb->opt.fast );
        }
    }
    vb->cur ^= 1;
    vb->prev_n = n;
    return frames;
}
