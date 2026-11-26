/*
 * FLAC.C - FLAC files: lossless sound, written from the format's
 * published description, RFC 9639 (DEC.H says what a codec is).
 *
 * A FLAC FILE is "fLaC", a run of metadata blocks, and then frames.  The
 * first block, STREAMINFO, gives the rate, the channels, the sample size
 * and the number of samples; a VORBIS_COMMENT block holds the title and
 * the artist; every other block is stepped over.  An ID3v2 tag that some
 * programs put in front is stepped over too.
 *
 * A FRAME IS ONE BLOCK OF SAMPLES.  It has a header that ends in a CRC-8,
 * one subframe for each channel, and a CRC-16 of all of that.  A subframe
 * is a constant, the samples as they are, or a predictor: a few samples
 * as they are, then for each of the rest only how far the prediction
 * from the samples before it was off.  Those differences are mostly
 * small and are stored as Rice codes.  Two channels may be stored as one
 * channel and the difference ("side"), which is one bit wider.
 *
 * NOTHING HERE COSTS LESS AT A LOWER QUALITY.  Every sample has to be
 * rebuilt for the next one to be right, so half rate and one channel are
 * averages made after the decoding.  They save the player's work, not
 * this file's.  The equalizer is the player's as well: there is no
 * spectrum here to apply one to.
 *
 * THE PREDICTOR FITS 32 BITS MORE OFTEN THAN IT SEEMS.  The sum of up to
 * 32 products can need 50 bits, but a good stream's sample is known to
 * fit "width" bits.  Arithmetic modulo 2^32 keeps the low 32 bits of the
 * sum, the shift by "shift" leaves 32 - shift good bits, and when those
 * cover the width, extending the sign from the width gives the exact
 * sample.  That is every 16-bit file, whatever its coefficients.  Only
 * wide samples with a large shift need the 64-bit sum.
 *
 * WHAT IS REFUSED, BY NAME: 32-bit samples, whose side channel would need
 * 33 bits, and rates outside 1 kHz to 384 kHz, which the player cannot
 * convert.  FLAC inside an Ogg file is not read.  A frame whose rate,
 * channels or sample size differ from STREAMINFO's is passed over.
 *
 * DAMAGE: a frame whose CRC-16 fails is dropped, and the search for the
 * next frame goes on from the byte after its first.  A frame header is
 * believed only if its CRC-8 is right and it agrees with STREAMINFO.  The
 * MD5 in STREAMINFO is not checked; that would take a machine this is
 * written for a good part of its time.
 *
 * SEEKING looks for the frame by its number: a guess in the file from
 * the position of the sound so far, the next frame header after the
 * guess, and closer guesses from what that header says.  A seek table
 * would save a few reads but cannot be trusted further than the frames
 * themselves, and not every file has one.
 */
#include "rt.h"
#include "fix.h"
#include "dec.h"

#define MAX_CH          8
#define MAX_ORDER       32
#define MAX_BLOCK       65535UL
#define IN_SIZE         16384UL         /* bytes read at a time */
#define HEAD_MIN        6               /* the shortest frame header */
#define HEAD_MAX        16              /* the longest */
#define FAST_RICE       17              /* the largest Rice parameter rice_fast takes */
#define FIELD_MAX       (8 + TAG_LEN * 4)   /* a comment's name and as much value as can show */
#define COMMENTS_MAX    4096UL          /* comments looked at in one block */
#define BLOCKS_MAX      65536UL         /* metadata blocks */
#define WORK_MAX        (8UL * 1024 * 1024) /* bytes read with no good frame: then it is the end */

/* a frame header, as parse_head finds it */
typedef struct {
    uint32_t block;                     /* samples in each channel */
    uint32_t number;                    /* the frame's number or its first sample's */
    uint8_t  var;                       /* the number is a sample number */
    uint8_t  big;                       /* the number needs more than 32 bits */
    uint8_t  assign;                    /* 0-7: that many channels less one; 8-10: stereo kinds */
    uint8_t  len;                       /* bytes, with the CRC-8 */
} FRHEAD;

typedef struct {
    SRC     *src;
    uint32_t rate, total;               /* total: samples of each channel, 0 when not known */
    uint32_t min_block, max_block;
    uint32_t fix_block;                 /* frame numbers count blocks of this; 0: sample numbers */
    uint32_t first;                     /* the file position of the first frame */
    uint32_t search_span;               /* how far to look for a frame header when opening */
    int      seekable;
    int      channels, bps;
    int      out_ch, down;
    int      cut, raise;                /* the shifts that make a sample 16 bits */
    /* the block decoded, and how much of it has gone out */
    int32_t *store;
    int32_t *samp[MAX_CH];
    uint32_t blk_len, blk_used;
    /* half and quarter rate: frames being averaged */
    int32_t  acc[2];
    uint32_t acc_count;
    /* the bytes read, and the bits taken from them */
    uint8_t *in;
    uint32_t in_len, pos;               /* bytes in "in"; the next one to use */
    uint32_t in_file;                   /* the file position of in[0] */
    uint32_t cache;                     /* the next bits, from the top down */
    int      bits;                      /* how many of them */
    int      pad;                       /* of those, the zeros put in after the last real byte */
    int      ended;                     /* no more bytes for this frame */
    int      over;                      /* the frame cannot be read */
    uint32_t crc, crc_at;               /* the CRC-16 so far, and in[] from where it is owed */
    uint32_t frame_at;                  /* the file position of this frame */
    uint32_t work;                      /* bytes read since the last good frame */
} FLAC;

/* ------------------------------------------------------------------ */
/* tables                                                              */
/* ------------------------------------------------------------------ */
static uint16_t crc16_tab[256];
static uint8_t  crc8_tab[256];
static uint8_t  lead0[256];             /* zero bits at the top of a byte: 8 for 0 */
static int      tables_made;

static void tables_make( void )
{
    uint32_t crc;
    int i, j;

    if ( tables_made ) {
        return;
    }
    for ( i = 0; i < 256; i++ ) {
        crc = (uint32_t)i << 8;
        for ( j = 0; j < 8; j++ ) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1;
        }
        crc16_tab[i] = (uint16_t)crc;
        crc = (uint32_t)i;
        for ( j = 0; j < 8; j++ ) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
        }
        crc8_tab[i] = (uint8_t)crc;
        j = 0;
        while ( j < 8 && !(i & (0x80 >> j)) ) {
            j++;
        }
        lead0[i] = (uint8_t)j;
    }
    tables_made = 1;
}

/* the weights for mixing three to eight channels to two, in halves.  The
   channels come in FLAC's order: left, right, center, bass, then the
   surrounds; the seven-channel layout has one rear channel, at index 4.
   Each row adds up to the number of channels, which is the divisor. */
static const uint8_t mix_left[6][MAX_CH] = {
    { 2, 0, 1 },
    { 2, 0, 2, 0 },
    { 2, 0, 1, 2, 0 },
    { 2, 0, 1, 1, 2, 0 },
    { 2, 0, 1, 1, 1, 2, 0 },
    { 2, 0, 1, 1, 2, 0, 2, 0 }
};
static const uint8_t mix_right[6][MAX_CH] = {
    { 0, 2, 1 },
    { 0, 2, 0, 2 },
    { 0, 2, 1, 0, 2 },
    { 0, 2, 1, 1, 0, 2 },
    { 0, 2, 1, 1, 1, 0, 2 },
    { 0, 2, 1, 1, 0, 2, 0, 2 }
};

/* acc[0..1], a 64-bit number, plus lhs * rhs */
static void mul_add( uint32_t *acc, int32_t lhs, int32_t rhs );
#pragma aux mul_add = \
    "imul edx" \
    "add [ebx],eax" \
    "adc [ebx+4],edx" \
    parm [ebx] [eax] [edx] modify exact [eax edx];

/* the 64-bit number hi:lo shifted right by 0 to 31 */
static int32_t acc_shift( uint32_t lo, uint32_t hi, int shift );
#pragma aux acc_shift = \
    "shrd eax,edx,cl" \
    parm [eax] [edx] [ecx] value [eax] modify exact [eax] nomemory;

/* ------------------------------------------------------------------ */
/* bytes                                                               */
/* ------------------------------------------------------------------ */
/* nothing in the cache, and nothing wrong yet */
static void bits_clear( FLAC *fl )
{
    fl->cache = 0;
    fl->bits = 0;
    fl->pad = 0;
    fl->ended = 0;
    fl->over = 0;
}

/* The next byte to use is the one at "file_pos".  Always true: the file
   is positioned at in_file + in_len. */
static void go_to( FLAC *fl, uint32_t file_pos )
{
    if ( file_pos > fl->src->size ) {
        file_pos = fl->src->size;
    }
    bits_clear( fl );
    if ( file_pos >= fl->in_file && file_pos - fl->in_file <= fl->in_len ) {
        fl->pos = file_pos - fl->in_file;
        return;
    }
    src_seek( fl->src, file_pos );
    fl->in_file = file_pos;
    fl->in_len = 0;
    fl->pos = 0;
}

/* At least "want" bytes (no more than IN_SIZE) from pos on, or all the
   file has left.  -> the bytes there.  Not while bits are in the cache. */
static unsigned int in_need( FLAC *fl, unsigned int want )
{
    unsigned int have = fl->in_len - fl->pos, got;

    if ( have >= want ) {
        return have;
    }
    mem_cpy( fl->in, fl->in + fl->pos, have );
    fl->in_file += fl->pos;
    fl->pos = 0;
    got = src_read( fl->src, fl->in + have, IN_SIZE - have );
    fl->work += got;
    fl->in_len = have + got;
    return fl->in_len;
}

/* The CRC-16 of in[crc_at..end) added to what it has so far */
static void crc_upto( FLAC *fl, unsigned int end )
{
    const uint8_t *at = fl->in + fl->crc_at, *stop = fl->in + end;
    uint32_t crc = fl->crc;

    while ( at < stop ) {
        crc = ((crc << 8) ^ crc16_tab[(crc >> 8) ^ *at++]) & 0xFFFF;
    }
    fl->crc = crc;
    fl->crc_at = end;
}

/* In the middle of a frame, the buffer used up: more of the file behind
   the bytes the cache still holds, which stay so that the end of the
   frame can be found in them.  -> 0 at the end of the file, or when so
   much has been read without a good frame that this one is not believed */
static int in_more( FLAC *fl )
{
    unsigned int used = fl->pos - (unsigned int)fl->bits / 8, keep, got;

    crc_upto( fl, used );
    keep = fl->in_len - used;
    mem_cpy( fl->in, fl->in + used, keep );
    fl->in_file += used;
    fl->in_len = keep;
    fl->pos = keep;
    fl->crc_at = 0;
    if ( fl->work > WORK_MAX ) {
        return 0;
    }
    got = src_read( fl->src, fl->in + keep, IN_SIZE - keep );
    fl->work += got;
    fl->in_len = keep + got;
    return got != 0;
}

/* ------------------------------------------------------------------ */
/* bits                                                                */
/* ------------------------------------------------------------------ */
/* At least 25 bits in the cache.  The cache reads ahead, so running out
   of bytes here is no fault: the last frame of every file does.  Zeros
   go in instead, and only using one of them loses the frame (is_over). */
static void bit_fill( FLAC *fl )
{
    while ( fl->bits <= 24 ) {
        if ( fl->pos >= fl->in_len && (fl->ended || !in_more( fl )) ) {
            fl->ended = 1;
            fl->pad += 8;
        } else {
            fl->cache |= (uint32_t)fl->in[fl->pos++] << (24 - fl->bits);
        }
        fl->bits += 8;
    }
}

/* the frame is lost: something failed, or bits were used that the file
   does not have */
static int is_over( const FLAC *fl )
{
    return fl->over || fl->bits < fl->pad;
}

/* the next "count" bits, 0 to 32 */
static uint32_t bits_get( FLAC *fl, int count )
{
    uint32_t val;

    if ( count == 0 ) {
        return 0;
    }
    if ( count > 24 ) {
        val = bits_get( fl, count - 16 );
        return (val << 16) | bits_get( fl, 16 );
    }
    if ( fl->bits < count ) {
        bit_fill( fl );
    }
    val = fl->cache >> (32 - count);
    fl->cache <<= count;
    fl->bits -= count;
    return val;
}

/* the next "count" bits, 1 to 32, as a signed number */
static int32_t bits_sget( FLAC *fl, int count )
{
    return (int32_t)(bits_get( fl, count ) << (32 - count)) >> (32 - count);
}

/* The zero bits before the next 1, and the 1.  More than "limit" of them
   cannot be a real number, and the frame is lost. */
static uint32_t bits_zeros( FLAC *fl, uint32_t limit )
{
    uint32_t count = 0;
    int zeros;

    for ( ;; ) {
        if ( fl->bits < 8 ) {
            bit_fill( fl );
        }
        zeros = lead0[fl->cache >> 24];
        if ( zeros < 8 ) {
            fl->cache <<= zeros + 1;
            fl->bits -= zeros + 1;
            return count + (uint32_t)zeros;
        }
        fl->cache <<= 8;
        fl->bits -= 8;
        count += 8;
        if ( count > limit || is_over( fl ) ) {
            fl->over = 1;
            return 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the differences                                                     */
/* ------------------------------------------------------------------ */
/* "count" Rice codes with a parameter of 0 to FAST_RICE: this is where
   most of the time goes.  After a fill there are 25 bits; a code of up
   to seven zeros takes 8 of them and leaves the parameter's 17. */
static void rice_fast( FLAC *fl, int32_t *out, unsigned int count, int param )
{
    const uint8_t *at = fl->in + fl->pos, *end = fl->in + fl->in_len;
    uint32_t cache = fl->cache, val, limit = 0xFFFFFFFFUL >> param;
    int bits = fl->bits, zeros, down = 31 - param;

    while ( count-- ) {
        while ( bits <= 24 ) {
            if ( at == end ) {
                fl->pos = (uint32_t)(at - fl->in);
                fl->cache = cache;
                fl->bits = bits;
                bit_fill( fl );
                at = fl->in + fl->pos;
                end = fl->in + fl->in_len;
                cache = fl->cache;
                bits = fl->bits;
                break;
            }
            cache |= (uint32_t)*at++ << (24 - bits);
            bits += 8;
        }
        zeros = lead0[cache >> 24];
        if ( zeros < 8 ) {
            cache <<= zeros + 1;
            bits -= zeros + 1;
            val = (uint32_t)zeros;
        } else {
            fl->pos = (uint32_t)(at - fl->in);
            fl->cache = cache;
            fl->bits = bits;
            val = bits_zeros( fl, limit );
            if ( fl->over ) {
                return;
            }
            if ( fl->bits < param ) {
                bit_fill( fl );
            }
            at = fl->in + fl->pos;
            end = fl->in + fl->in_len;
            cache = fl->cache;
            bits = fl->bits;
        }
        /* two shifts, because a shift by 32 would be a shift by 0 */
        val = (val << param) | ((cache >> 1) >> down);
        cache <<= param;
        bits -= param;
        *out++ = (int32_t)(val >> 1) ^ -(int32_t)(val & 1);
    }
    fl->pos = (uint32_t)(at - fl->in);
    fl->cache = cache;
    fl->bits = bits;
}

/* the same for the large parameters, which only wide samples use */
static void rice_slow( FLAC *fl, int32_t *out, unsigned int count, int param )
{
    uint32_t val, limit = 0xFFFFFFFFUL >> param;

    while ( count-- ) {
        val = bits_zeros( fl, limit );
        if ( fl->over ) {
            return;
        }
        val = (val << param) | bits_get( fl, param );
        *out++ = (int32_t)(val >> 1) ^ -(int32_t)(val & 1);
    }
}

/* The differences of a subframe: out[order] to out[block - 1].  -> 0 if
   they cannot be read */
static int read_residual( FLAC *fl, int32_t *out, unsigned int block, unsigned int order )
{
    uint32_t method, part_order;
    unsigned int part, parts, index, count, i;
    int param, raw, param_bits, escape;
    int32_t *at = out + order;

    method = bits_get( fl, 2 );
    if ( method > 1 ) {
        return 0;
    }
    param_bits = method ? 5 : 4;
    escape = method ? 31 : 15;
    part_order = bits_get( fl, 4 );
    part = block >> part_order;
    if ( (part << part_order) != block || part < order ) {
        return 0;
    }
    parts = 1UL << part_order;
    for ( index = 0; index < parts; index++ ) {
        count = index == 0 ? part - order : part;
        param = (int)bits_get( fl, param_bits );
        if ( param == escape ) {
            raw = (int)bits_get( fl, 5 );
            for ( i = 0; i < count; i++ ) {
                at[i] = raw ? bits_sget( fl, raw ) : 0;
            }
        } else if ( param <= FAST_RICE ) {
            rice_fast( fl, at, count, param );
        } else {
            rice_slow( fl, at, count, param );
        }
        if ( is_over( fl ) ) {
            return 0;
        }
        at += count;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* the predictors                                                      */
/* ------------------------------------------------------------------ */
/* Each turns the differences from out[order] on into samples.  The sums
   are unsigned so that a damaged frame merely gives wrong numbers, which
   its CRC then rejects. */
static void fixed_run( int32_t *out, unsigned int block, int order )
{
    unsigned int i;
    uint32_t one, two, three, four, val;

    switch ( order ) {
    case 1:
        one = (uint32_t)out[0];
        for ( i = 1; i < block; i++ ) {
            one += (uint32_t)out[i];
            out[i] = (int32_t)one;
        }
        break;
    case 2:
        one = (uint32_t)out[1];
        two = (uint32_t)out[0];
        for ( i = 2; i < block; i++ ) {
            val = (uint32_t)out[i] + 2 * one - two;
            out[i] = (int32_t)val;
            two = one;
            one = val;
        }
        break;
    case 3:
        one = (uint32_t)out[2];
        two = (uint32_t)out[1];
        three = (uint32_t)out[0];
        for ( i = 3; i < block; i++ ) {
            val = (uint32_t)out[i] + 3 * (one - two) + three;
            out[i] = (int32_t)val;
            three = two;
            two = one;
            one = val;
        }
        break;
    case 4:
        one = (uint32_t)out[3];
        two = (uint32_t)out[2];
        three = (uint32_t)out[1];
        four = (uint32_t)out[0];
        for ( i = 4; i < block; i++ ) {
            val = (uint32_t)out[i] + 4 * (one + three) - 6 * two - four;
            out[i] = (int32_t)val;
            four = three;
            three = two;
            two = one;
            one = val;
        }
        break;
    }
}

/* width + shift <= 32: the sum modulo 2^32 is enough (the top of this
   file says why) */
static void lpc_32( int32_t *out, unsigned int block, const int32_t *coef, int order, int shift, int width )
{
    const int32_t *hist;
    unsigned int i;
    uint32_t sum;
    int j, ext = 32 - width;

    for ( i = (unsigned int)order; i < block; i++ ) {
        sum = 0;
        hist = out + i;
        for ( j = 0; j < order; j++ ) {
            hist--;
            sum += (uint32_t)coef[j] * (uint32_t)*hist;
        }
        sum = (uint32_t)out[i] + (uint32_t)((int32_t)sum >> shift);
        out[i] = (int32_t)(sum << ext) >> ext;
    }
}

static void lpc_64( int32_t *out, unsigned int block, const int32_t *coef, int order, int shift )
{
    const int32_t *hist;
    unsigned int i;
    uint32_t acc[2];
    int j;

    for ( i = (unsigned int)order; i < block; i++ ) {
        acc[0] = acc[1] = 0;
        hist = out + i;
        for ( j = 0; j < order; j++ ) {
            hist--;
            mul_add( acc, coef[j], *hist );
        }
        out[i] = (int32_t)((uint32_t)out[i] + (uint32_t)acc_shift( acc[0], acc[1], shift ));
    }
}

/* ------------------------------------------------------------------ */
/* a subframe                                                          */
/* ------------------------------------------------------------------ */
/* One channel of a block into out[0..block).  "width" is its sample size
   in bits, 1 to 32.  -> 0 if it cannot be read */
static int read_subframe( FLAC *fl, int32_t *out, unsigned int block, int width )
{
    int32_t coef[MAX_ORDER], val;
    uint32_t head;
    unsigned int i;
    uint32_t wasted = 0;
    int type, order, precision, shift, j;

    head = bits_get( fl, 8 );
    if ( head & 0x80 ) {
        return 0;
    }
    type = (int)(head >> 1);
    if ( head & 1 ) {
        wasted = bits_zeros( fl, 32 ) + 1;
        if ( is_over( fl ) || wasted >= (uint32_t)width ) {
            return 0;
        }
        width -= (int)wasted;
    }
    if ( type == 0 ) {
        val = bits_sget( fl, width );
        for ( i = 0; i < block; i++ ) {
            out[i] = val;
        }
    } else if ( type == 1 ) {
        for ( i = 0; i < block; i++ ) {
            out[i] = bits_sget( fl, width );
        }
    } else if ( type >= 8 && type <= 12 ) {
        order = type - 8;
        if ( (unsigned int)order > block ) {
            return 0;
        }
        for ( j = 0; j < order; j++ ) {
            out[j] = bits_sget( fl, width );
        }
        if ( !read_residual( fl, out, block, (unsigned int)order ) ) {
            return 0;
        }
        fixed_run( out, block, order );
    } else if ( type >= 32 ) {
        order = type - 31;
        if ( (unsigned int)order > block ) {
            return 0;
        }
        for ( j = 0; j < order; j++ ) {
            out[j] = bits_sget( fl, width );
        }
        precision = (int)bits_get( fl, 4 ) + 1;
        shift = (int)bits_sget( fl, 5 );
        if ( precision == 16 || shift < 0 ) {
            return 0;
        }
        for ( j = 0; j < order; j++ ) {
            coef[j] = bits_sget( fl, precision );
        }
        if ( !read_residual( fl, out, block, (unsigned int)order ) ) {
            return 0;
        }
        if ( width + shift <= 32 ) {
            lpc_32( out, block, coef, order, shift, width );
        } else {
            lpc_64( out, block, coef, order, shift );
        }
    } else {
        return 0;
    }
    if ( is_over( fl ) ) {
        return 0;
    }
    if ( wasted ) {
        for ( i = 0; i < block; i++ ) {
            out[i] = (int32_t)((uint32_t)out[i] << wasted);
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* frames                                                              */
/* ------------------------------------------------------------------ */
/* The frame header at "at", "avail" bytes there.  -> its length, 0 if it
   is not a header of this stream */
static unsigned int parse_head( const FLAC *fl, const uint8_t *at, unsigned int avail, FRHEAD *hd )
{
    static const uint8_t size_bits[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };
    static const uint32_t rates[12] = { 0, 88200UL, 176400UL, 192000UL, 8000, 16000,
                                   22050, 24000, 32000, 44100UL, 48000UL, 96000UL };
    uint32_t val;
    unsigned int need;
    uint32_t block, rate, crc;
    unsigned int i, index;
    int code_size, code_rate, code_bits, extra, first, byte;

    if ( avail < HEAD_MIN || at[0] != 0xFF || (at[1] & 0xFE) != 0xF8 ) {
        return 0;
    }
    code_size = at[2] >> 4;
    code_rate = at[2] & 15;
    hd->assign = (uint8_t)(at[3] >> 4);
    code_bits = (at[3] >> 1) & 7;
    hd->var = (uint8_t)(at[1] & 1);
    hd->big = 0;
    if ( code_size == 0 || code_rate == 15 || hd->assign > 10 || code_bits == 3 || (at[3] & 1) ) {
        return 0;
    }
    if ( (hd->assign < 8 ? hd->assign + 1 : 2) != fl->channels ) {
        return 0;
    }
    if ( code_bits != 0 && size_bits[code_bits] != fl->bps ) {
        return 0;
    }
    /* the number: UTF-8's pattern, carried on to seven bytes */
    first = at[4];
    if ( first < 0x80 ) {
        extra = 0;
        val = (uint32_t)first;
    } else if ( first < 0xC0 ) {
        return 0;
    } else if ( first < 0xE0 ) {
        extra = 1;
        val = (uint32_t)first & 0x1F;
    } else if ( first < 0xF0 ) {
        extra = 2;
        val = (uint32_t)first & 0x0F;
    } else if ( first < 0xF8 ) {
        extra = 3;
        val = (uint32_t)first & 0x07;
    } else if ( first < 0xFC ) {
        extra = 4;
        val = (uint32_t)first & 0x03;
    } else if ( first < 0xFE ) {
        extra = 5;
        val = (uint32_t)first & 0x01;
    } else if ( first == 0xFE && hd->var ) {
        extra = 6;
        val = 0;
    } else {
        return 0;
    }
    need = 5 + (unsigned int)extra + 1;
    need += code_size == 6 ? 1 : code_size == 7 ? 2 : 0;
    need += code_rate == 12 ? 1 : (code_rate == 13 || code_rate == 14) ? 2 : 0;
    if ( avail < need ) {
        return 0;
    }
    index = 5;
    while ( extra-- > 0 ) {
        byte = at[index++];
        if ( (byte & 0xC0) != 0x80 ) {
            return 0;
        }
        if ( val & 0xFC000000UL ) {
            hd->big = 1;
        }
        val = (val << 6) | ((uint32_t)byte & 0x3F);
    }
    hd->number = val;
    if ( code_size == 1 ) {
        block = 192;
    } else if ( code_size <= 5 ) {
        block = 576UL << (code_size - 2);
    } else if ( code_size == 6 ) {
        block = (uint32_t)at[index++] + 1;
    } else if ( code_size == 7 ) {
        block = (uint32_t)BE16( at + index ) + 1;
        index += 2;
    } else {
        block = 256UL << (code_size - 8);
    }
    if ( code_rate == 12 ) {
        rate = (uint32_t)at[index++] * 1000;
    } else if ( code_rate == 13 ) {
        rate = BE16( at + index );
        index += 2;
    } else if ( code_rate == 14 ) {
        rate = (uint32_t)BE16( at + index ) * 10;
        index += 2;
    } else {
        rate = rates[code_rate];
    }
    if ( block > fl->max_block || (code_rate != 0 && rate != fl->rate) ) {
        return 0;
    }
    crc = 0;
    for ( i = 0; i < index; i++ ) {
        crc = crc8_tab[crc ^ at[i]];
    }
    if ( crc != at[index] ) {
        return 0;
    }
    hd->block = block;
    hd->len = (uint8_t)(index + 1);
    return index + 1;
}

/* From pos on, the next header that starts before file position "limit":
   pos is left on it.  -> 0 when there is none */
static int find_head( FLAC *fl, uint32_t limit, FRHEAD *hd )
{
    const uint8_t *at;
    unsigned int have, stop, i;

    for ( ;; ) {
        have = in_need( fl, HEAD_MAX );
        if ( have < HEAD_MIN || fl->work > WORK_MAX ) {
            return 0;
        }
        at = fl->in + fl->pos;
        stop = have - 1;
        i = 0;
        while ( i < stop && !(at[i] == 0xFF && (at[i + 1] & 0xFE) == 0xF8) ) {
            i++;
        }
        fl->pos += i;
        if ( fl->in_file + fl->pos >= limit ) {
            return 0;
        }
        if ( i == stop ) {
            continue;
        }
        have = in_need( fl, HEAD_MAX );
        if ( parse_head( fl, fl->in + fl->pos, have, hd ) != 0 ) {
            return 1;
        }
        fl->pos++;
    }
}

/* the first sample of the frame with this header.  -> 0 if that cannot be
   said in 32 bits */
static int head_sample( const FLAC *fl, const FRHEAD *hd, uint32_t *sample )
{
    if ( hd->big ) {
        return 0;
    }
    if ( hd->var || fl->fix_block == 0 ) {
        *sample = hd->number;
        return 1;
    }
    if ( hd->number > 0xFFFFFFFFUL / fl->fix_block ) {
        return 0;
    }
    *sample = hd->number * fl->fix_block;
    return 1;
}

/* The frame whose header is at pos, into samp[].  pos is left after it.
   -> 0 if it is damaged */
static int read_frame( FLAC *fl, const FRHEAD *hd )
{
    int32_t *one = fl->samp[0], *two = fl->samp[1], half;
    unsigned int block = hd->block, i, end;
    int ch, width, drop, assign = hd->assign;

    fl->frame_at = fl->in_file + fl->pos;
    fl->crc = 0;
    fl->crc_at = fl->pos;
    fl->pos += hd->len;
    bits_clear( fl );
    for ( ch = 0; ch < fl->channels; ch++ ) {
        width = fl->bps;
        if ( (assign == 8 && ch == 1) || (assign == 9 && ch == 0) || (assign == 10 && ch == 1) ) {
            width++;                    /* the side channel */
        }
        if ( !read_subframe( fl, fl->samp[ch], block, width ) ) {
            return 0;
        }
    }
    /* to the next whole byte, then the CRC-16, which the CRC of the frame
       taken through itself comes to 0 with */
    drop = fl->bits & 7;
    fl->cache <<= drop;
    fl->bits -= drop;
    (void)bits_get( fl, 16 );
    if ( is_over( fl ) ) {
        return 0;
    }
    end = fl->pos - (unsigned int)(fl->bits - fl->pad) / 8;
    crc_upto( fl, end );
    fl->pos = end;
    bits_clear( fl );
    if ( fl->crc != 0 ) {
        return 0;
    }
    switch ( assign ) {
    case 8:                             /* left, side */
        for ( i = 0; i < block; i++ ) {
            two[i] = (int32_t)((uint32_t)one[i] - (uint32_t)two[i]);
        }
        break;
    case 9:                             /* side, right */
        for ( i = 0; i < block; i++ ) {
            one[i] = (int32_t)((uint32_t)one[i] + (uint32_t)two[i]);
        }
        break;
    case 10:                            /* mid, side.  Mid lost its low bit, which is side's */
        for ( i = 0; i < block; i++ ) {
            half = (two[i] >> 1) + (two[i] & 1);
            half = (int32_t)((uint32_t)one[i] + (uint32_t)half);
            two[i] = (int32_t)((uint32_t)half - (uint32_t)two[i]);
            one[i] = half;
        }
        break;
    }
    return 1;
}

/* The next good frame into samp[].  -> 0 at the end */
static int next_frame( FLAC *fl )
{
    FRHEAD hd;

    for ( ;; ) {
        if ( !find_head( fl, 0xFFFFFFFFUL, &hd ) ) {
            return 0;
        }
        if ( read_frame( fl, &hd ) ) {
            fl->work = 0;
            fl->blk_len = hd.block;
            fl->blk_used = 0;
            return 1;
        }
        if ( fl->src->error ) {
            return 0;
        }
        /* a retry may read nothing new and still rebuild a whole block,
           so the block counts as work as well as the bytes do */
        fl->work += hd.block * (uint32_t)fl->channels;
        go_to( fl, fl->frame_at + 1 );
    }
}

/* ------------------------------------------------------------------ */
/* giving the samples out                                              */
/* ------------------------------------------------------------------ */
/* frame "at" of the block as 16-bit left and right */
static void frame_lr( const FLAC *fl, unsigned int at, int32_t *left, int32_t *right )
{
    const uint8_t *wl, *wr;
    int32_t val, sum_l = 0, sum_r = 0;
    int ch, nch = fl->channels;

    if ( nch <= 2 ) {
        *left = (fl->samp[0][at] >> fl->cut) << fl->raise;
        *right = nch == 1 ? *left : (fl->samp[1][at] >> fl->cut) << fl->raise;
        return;
    }
    wl = mix_left[nch - 3];
    wr = mix_right[nch - 3];
    for ( ch = 0; ch < nch; ch++ ) {
        val = (fl->samp[ch][at] >> fl->cut) << fl->raise;
        sum_l += val * wl[ch];
        sum_r += val * wr[ch];
    }
    *left = sum_l / nch;
    *right = sum_r / nch;
}

/* as much of the block as there is room for */
static void give( FLAC *fl, DECBUF *buf )
{
    int16_t *pcm = buf->pcm + buf->frames * fl->out_ch;
    const int32_t *one, *two;
    unsigned int count, i, used = fl->blk_used;
    int32_t left, right;
    int ch;

    if ( fl->bps == 16 && fl->down == 0 && fl->out_ch == fl->channels ) {
        count = fl->blk_len - used;
        if ( count > buf->max - buf->frames ) {
            count = buf->max - buf->frames;
        }
        one = fl->samp[0] + used;
        if ( fl->out_ch == 1 ) {
            for ( i = 0; i < count; i++ ) {
                pcm[i] = (int16_t)one[i];
            }
        } else {
            two = fl->samp[1] + used;
            for ( i = 0; i < count; i++ ) {
                pcm[0] = (int16_t)one[i];
                pcm[1] = (int16_t)two[i];
                pcm += 2;
            }
        }
        fl->blk_used = used + count;
        buf->frames += count;
        return;
    }
    while ( used < fl->blk_len && buf->frames < buf->max ) {
        frame_lr( fl, used, &left, &right );
        used++;
        if ( fl->out_ch == 1 ) {
            fl->acc[0] += fl->channels == 1 ? left : (left + right) >> 1;
        } else {
            fl->acc[0] += left;
            fl->acc[1] += right;
        }
        if ( ++fl->acc_count < (1UL << fl->down) ) {
            continue;
        }
        for ( ch = 0; ch < fl->out_ch; ch++ ) {
            left = fl->acc[ch] >> fl->down;
            *pcm++ = (int16_t)FX_SAT16( left );
            fl->acc[ch] = 0;
        }
        fl->acc_count = 0;
        buf->frames++;
    }
    fl->blk_used = used;
}

static int flac_decode( void *state, DECBUF *buf )
{
    FLAC *fl = (FLAC *)state;

    buf->frames = 0;
    buf->ev_count = 0;
    while ( buf->frames < buf->max ) {
        if ( fl->blk_used >= fl->blk_len && !next_frame( fl ) ) {
            break;
        }
        give( fl, buf );
    }
    return fl->src->error && buf->frames == 0 ? DE_READ : DE_OK;
}

/* ------------------------------------------------------------------ */
/* seeking                                                             */
/* ------------------------------------------------------------------ */
static int flac_seek( void *state, uint32_t ms, uint32_t *at_ms )
{
    FLAC *fl = (FLAC *)state;
    FRHEAD hd;
    uint32_t target, low = fl->first, high = fl->src->size, low_sample = 0, high_sample = fl->total;
    uint32_t best_pos = fl->first, best_sample = 0, guess, span, sample, found;
    int probe;

    if ( !fl->seekable ) {
        return DE_FORMAT;
    }
    if ( ms > 0xFFFFFFFFUL / fl->rate * 1000UL ) {
        ms = 0xFFFFFFFFUL / fl->rate * 1000UL;
    }
    target = fx_umuldiv( ms, fl->rate, 1000 );
    if ( fl->total && target >= fl->total ) {
        target = fl->total - 1;
    }
    /* low_sample <= target < high_sample at every step.  The first few
       guesses go by the sound's position, the rest halve what is left,
       which is what makes the number of steps certain.  Either way the
       stretch from low to high gets shorter every time. */
    for ( probe = 0; probe < 48 && low < high; probe++ ) {
        span = high - low;
        if ( probe < 6 && high_sample > low_sample ) {
            guess = low + fx_umuldiv( target - low_sample, span, high_sample - low_sample );
        } else {
            guess = low + span / 2;
        }
        fl->work = 0;
        go_to( fl, guess );
        if ( !find_head( fl, high, &hd ) || !head_sample( fl, &hd, &sample )
             || (fl->total && sample >= fl->total) ) {
            high = guess;               /* no frame from there up */
            continue;
        }
        found = fl->in_file + fl->pos;
        if ( sample > target ) {
            high = guess;
            high_sample = sample;
            continue;
        }
        best_pos = found;
        best_sample = sample;
        if ( target - sample < hd.block ) {
            break;                      /* the frame the target is in */
        }
        low = found + 1;
        low_sample = sample + hd.block;
    }
    fl->work = 0;
    go_to( fl, best_pos );
    fl->blk_len = fl->blk_used = 0;
    fl->acc[0] = fl->acc[1] = 0;
    fl->acc_count = 0;
    *at_ms = fx_umuldiv( best_sample, 1000, fl->rate );
    return DE_OK;
}

/* ------------------------------------------------------------------ */
/* opening                                                             */
/* ------------------------------------------------------------------ */
/* the whole size of an ID3v2 tag whose first 10 bytes are "head", or 0 */
static uint32_t id3_size( const uint8_t *head )
{
    uint32_t size;

    if ( mem_cmp( head, "ID3", 3 ) != 0
         || ((head[6] | head[7] | head[8] | head[9]) & 0x80) ) {
        return 0;
    }
    size = ((uint32_t)head[6] << 21) | ((uint32_t)head[7] << 14) | ((uint32_t)head[8] << 7) | head[9];
    return size + ((head[5] & 0x10) ? 20 : 10);
}

static int flac_detect( SRC *src, const uint8_t *head, unsigned int len, const char *ext )
{
    uint8_t mark[4];
    uint32_t start;

    (void)ext;
    if ( len >= 4 && mem_cmp( head, "fLaC", 4 ) == 0 ) {
        return 100;
    }
    if ( len < 10 ) {
        return 0;
    }
    start = id3_size( head );
    if ( start == 0 || src_seek( src, start ) != 0 || src_read( src, mark, 4 ) != 4 ) {
        return 0;
    }
    return mem_cmp( mark, "fLaC", 4 ) == 0 ? 100 : 0;
}

/* "NAME=" at the start of a comment, in either case: -> where the value
   starts, or 0 */
static unsigned int tag_value( const uint8_t *field, unsigned int len, const char *name )
{
    unsigned int size = str_len( name );

    if ( len > size && field[size] == '=' && str_nicmp( (const char *)field, name, size ) == 0 ) {
        return size + 1;
    }
    return 0;
}

/* The VORBIS_COMMENT block of "len" bytes at the file's position.  Its
   numbers are little-endian, unlike everything else in the file.  A long
   value (cover art has been put in one) is read only as far as can show. */
static void read_comments( SRC *src, uint32_t len, DECINFO *info )
{
    uint8_t field[FIELD_MAX], word[4];
    uint32_t end = src_tell( src ) + len, size;
    unsigned int take;
    uint32_t count;
    unsigned int i, val;

    if ( len < 8 || src_read( src, word, 4 ) != 4 ) {
        return;
    }
    size = LE32( word );
    if ( size > end - src_tell( src ) || src_skip( src, size ) != 0
         || end - src_tell( src ) < 4 || src_read( src, word, 4 ) != 4 ) {
        return;
    }
    count = LE32( word );
    for ( i = 0; i < count && i < COMMENTS_MAX; i++ ) {
        if ( end - src_tell( src ) < 4 || src_read( src, word, 4 ) != 4 ) {
            return;
        }
        size = LE32( word );
        if ( size > end - src_tell( src ) ) {
            return;
        }
        take = size < FIELD_MAX ? size : FIELD_MAX;
        if ( src_read( src, field, take ) != take ) {
            return;
        }
        if ( (val = tag_value( field, take, "TITLE" )) != 0 && info->title[0] == 0 ) {
            tag_utf8( info->title, TAG_LEN, field + val, take - val );
        } else if ( (val = tag_value( field, take, "ARTIST" )) != 0 && info->artist[0] == 0 ) {
            tag_utf8( info->artist, TAG_LEN, field + val, take - val );
        } else if ( (val = tag_value( field, take, "ALBUM" )) != 0 && info->album[0] == 0 ) {
            tag_utf8( info->album, TAG_LEN, field + val, take - val );
        }
        if ( info->title[0] && info->artist[0] && info->album[0] ) {
            return;
        }
        if ( src_skip( src, size - take ) != 0 ) {
            return;
        }
    }
}

/* What STREAMINFO says.  -> DE_OK, DE_FORMAT or DE_UNSUPPORTED */
static int read_streaminfo( FLAC *fl, const uint8_t *data, DECINFO *info )
{
    fl->min_block = BE16( data );
    fl->max_block = BE16( data + 2 );
    fl->rate = ((uint32_t)data[10] << 12) | ((uint32_t)data[11] << 4) | ((uint32_t)data[12] >> 4);
    fl->channels = ((data[12] >> 1) & 7) + 1;
    fl->bps = (((data[12] & 1) << 4) | (data[13] >> 4)) + 1;
    fl->total = BE32( data + 14 );
    fl->seekable = 1;
    if ( data[13] & 15 ) {
        fl->total = 0;                  /* past 32 bits: more than this can count */
        fl->seekable = 0;
    }
    if ( fl->max_block < 16 ) {
        fl->max_block = MAX_BLOCK;      /* not filled in: the most a frame can hold */
    }
    if ( fl->rate == 0 || fl->bps < 4 ) {
        return DE_FORMAT;
    }
    if ( fl->bps > 31 ) {
        sfmt( info->format, sizeof( info->format ), "FLAC, %u-bit", fl->bps );
        return DE_UNSUPPORTED;
    }
    if ( fl->rate < 1000 || fl->rate > 384000UL ) {
        sfmt( info->format, sizeof( info->format ), "FLAC, %u Hz", fl->rate );
        return DE_UNSUPPORTED;
    }
    return DE_OK;
}

/* The metadata blocks, from the file's position.  It is left on the
   first frame.  -> DE_OK or why not */
static int read_metadata( FLAC *fl, DECINFO *info )
{
    SRC *src = fl->src;
    uint8_t head[4], data[34];
    uint32_t len, next;
    unsigned int count;
    int type, last = 0, rc;

    for ( count = 0; !last; count++ ) {
        if ( count >= BLOCKS_MAX ) {
            return DE_FORMAT;
        }
        if ( src_read( src, head, 4 ) != 4 ) {
            return src->error ? DE_READ : DE_FORMAT;
        }
        last = head[0] & 0x80;
        type = head[0] & 0x7F;
        len = ((uint32_t)head[1] << 16) | ((uint32_t)head[2] << 8) | head[3];
        if ( len > src_left( src ) ) {
            return DE_FORMAT;
        }
        next = src_tell( src ) + len;
        if ( count == 0 ) {
            if ( type != 0 || len < 34 ) {
                return DE_FORMAT;
            }
            if ( src_read( src, data, 34 ) != 34 ) {
                return src->error ? DE_READ : DE_FORMAT;
            }
            rc = read_streaminfo( fl, data, info );
            if ( rc != DE_OK ) {
                return rc;
            }
        } else if ( type == 4 ) {
            read_comments( src, len, info );
        }
        src_seek( src, next );
    }
    return DE_OK;
}

/* where looking for a header from the first frame stops */
static uint32_t search_end( const FLAC *fl )
{
    return fl->search_span > 0xFFFFFFFFUL - fl->first ? 0xFFFFFFFFUL : fl->first + fl->search_span;
}

/* How the numbers in the frame headers count, which STREAMINFO settles
   when every block is the same size.  Otherwise a header with the
   variable-size bit clear is either a frame number or, in files from
   before the bit was defined, a sample number: the second frame's number
   is 1 in the first case and the first block's size in the second. */
static void find_numbering( FLAC *fl, const FRHEAD *first_head )
{
    FRHEAD hd;
    uint32_t limit;

    fl->fix_block = first_head->block;
    if ( first_head->var ) {
        fl->fix_block = 0;
        return;
    }
    if ( fl->min_block == fl->max_block ) {
        fl->fix_block = fl->max_block;
        return;
    }
    limit = search_end( fl );
    go_to( fl, fl->first + 1 );
    while ( find_head( fl, limit, &hd ) ) {
        if ( !hd.big && hd.number == 1 ) {
            return;
        }
        if ( !hd.big && hd.number == first_head->block ) {
            fl->fix_block = 0;
            return;
        }
        fl->pos++;
    }
}

/* STREAMINFO did not say how long the file is: the end of the last frame
   does.  A header counts when the one before it led up to it, so that a
   chance match among the sound's bytes has no say. */
static void find_total( FLAC *fl )
{
    FRHEAD hd;
    uint32_t size = fl->src->size, tail = fl->search_span * 2, sample, next = 0, total = 0;
    int seen = 0;

    fl->work = 0;
    go_to( fl, size - fl->first > tail ? size - tail : fl->first );
    while ( find_head( fl, size, &hd ) ) {
        if ( head_sample( fl, &hd, &sample ) ) {
            if ( !seen || sample == next ) {
                total = sample + hd.block;
            }
            next = sample + hd.block;
            seen = 1;
        }
        fl->pos++;
    }
    fl->total = total;
}

static void flac_close( void *state )
{
    FLAC *fl = (FLAC *)state;

    if ( fl != NULL ) {
        if ( fl->store != NULL ) {
            xfree( fl->store );
        }
        if ( fl->in != NULL ) {
            xfree( fl->in );
        }
        xfree( fl );
    }
}

static int flac_open( SRC *src, const DECOPT *opt, DECINFO *info, void **state )
{
    FLAC *fl;
    FRHEAD hd;
    uint8_t head[10];
    uint32_t start = 0, audio;
    int rc, ch;

    *state = NULL;
    tables_make();
    if ( src_read( src, head, 10 ) != 10 ) {
        return src->error ? DE_READ : DE_FORMAT;
    }
    start = id3_size( head );
    if ( src_seek( src, start ) != 0 || src_read( src, head, 4 ) != 4
         || mem_cmp( head, "fLaC", 4 ) != 0 ) {
        return src->error ? DE_READ : DE_FORMAT;
    }
    fl = (FLAC *)try_alloc( sizeof( FLAC ) );
    if ( fl == NULL ) {
        return DE_NOMEM;
    }
    mem_set( fl, 0, sizeof( FLAC ) );
    fl->src = src;
    rc = read_metadata( fl, info );
    if ( rc != DE_OK ) {
        flac_close( fl );
        return rc;
    }
    fl->first = src_tell( src );
    fl->in_file = fl->first;
    /* about the longest frame an encoder would choose to write: a sample
       of 31 bits takes four bytes, and the rest of a frame is small.  A
       frame can be longer, so this only bounds looking for a header. */
    fl->search_span = fl->max_block * (uint32_t)fl->channels * 5 + 65536UL;
    fl->in = (uint8_t *)try_alloc( IN_SIZE );
    fl->store = (int32_t *)try_alloc( fl->max_block * (unsigned int)fl->channels * sizeof( int32_t ) );
    if ( fl->in == NULL || fl->store == NULL ) {
        flac_close( fl );
        return DE_NOMEM;
    }
    for ( ch = 0; ch < fl->channels; ch++ ) {
        fl->samp[ch] = fl->store + (unsigned int)ch * fl->max_block;
    }
    if ( !find_head( fl, search_end( fl ), &hd ) ) {
        rc = src->error ? DE_READ : DE_FORMAT;
        flac_close( fl );
        return rc;
    }
    find_numbering( fl, &hd );
    if ( fl->total == 0 && fl->seekable ) {
        find_total( fl );
    }
    fl->work = 0;
    go_to( fl, fl->first );

    fl->down = opt->down > 2 ? 2 : opt->down;
    while ( fl->down > 0 && (fl->rate >> fl->down) < 8000 ) {
        fl->down--;                     /* not below 8 kHz */
    }
    fl->out_ch = fl->channels == 1 || opt->mono ? 1 : 2;
    fl->cut = fl->bps > 16 ? fl->bps - 16 : 0;
    fl->raise = fl->bps < 16 ? 16 - fl->bps : 0;

    info->file_rate = fl->rate;
    info->rate = fl->rate >> fl->down;
    info->file_channels = (uint8_t)fl->channels;
    info->channels = (uint8_t)fl->out_ch;
    info->bits = (uint8_t)fl->bps;
    info->flags = DIF_VBR | (fl->seekable ? DIF_SEEK : 0);
    info->length_ms = fl->total ? fx_umuldiv( fl->total, 1000, fl->rate ) : 0;
    audio = src->size - fl->first;
    if ( info->length_ms == 0 ) {
        info->kbps = 0;
    } else if ( audio < 0x20000000UL ) {
        info->kbps = fx_umuldiv( audio, 8, info->length_ms );
    } else {
        info->kbps = audio / info->length_ms * 8;
    }
    if ( fl->channels <= 2 ) {
        sfmt( info->format, sizeof( info->format ), "FLAC, %u-bit %s", fl->bps,
              fl->channels == 1 ? "mono" : "stereo" );
    } else {
        sfmt( info->format, sizeof( info->format ), "FLAC, %u-bit, %u channels", fl->bps,
              fl->channels );
    }
    *state = fl;
    return DE_OK;
}

const CODEC codec_flac = {
    "FLAC",
    "FLAC FLA",
    flac_detect,
    flac_open,
    flac_decode,
    flac_seek,
    flac_close
};
