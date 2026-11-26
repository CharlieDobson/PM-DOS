/*
 * WAV.C - Windows wave files: the codec every other one is modelled
 * on (DEC.H).
 *
 * A WAVE FILE IS A RIFF FILE: "RIFF", a length, "WAVE", and then
 * chunks - each a four-letter name and a length.  "fmt " says how the
 * samples are coded, "data" holds them, "LIST" may hold an INFO list
 * with a title and an artist.  Anything else is stepped over.  A chunk
 * of odd length is followed by one byte of padding.
 *
 * WHAT IS PLAYED:
 *   PCM            8 bits unsigned, 16, 24 and 32 bits signed
 *   IEEE float     32 and 64 bits, turned into integers by taking the
 *                  numbers apart - there is no coprocessor to ask
 *   A-law, mu-law  the telephone codings, by their formulas
 *   IMA ADPCM      4 bits a sample, in blocks (Windows' code 11h)
 *   MS ADPCM       4 bits a sample, in blocks (code 2), with the
 *                  coefficients the file itself lists
 *   EXTENSIBLE     code FFFEh, which names one of the above by a GUID
 *
 * MORE THAN TWO CHANNELS ARE MIXED TO TWO: the even-numbered channels
 * to the left, the odd ones to the right.  Wave files keep left first,
 * then right, center, low frequency and the surrounds, so that puts
 * each surround on its own side and splits the center and the bass
 * between them.
 *
 * HALF AND QUARTER RATE are the average of two or four frames, which
 * costs almost nothing here and saves the player a resampler.  One
 * channel is the average of all of them.
 *
 * A DATA LENGTH OF 0 OR ONE PAST THE END OF THE FILE is what a program
 * writes when it records without knowing how long it will be.  Both
 * are taken to mean "to the end of the file".
 */
#include "rt.h"
#include "fix.h"
#include "dec.h"

#define WF_PCM          0x0001
#define WF_MSADPCM      0x0002
#define WF_FLOAT        0x0003
#define WF_ALAW         0x0006
#define WF_MULAW        0x0007
#define WF_IMAADPCM     0x0011
#define WF_EXTENSIBLE   0xFFFE

#define MAX_CHANNELS    8
#define MAX_COEFS       32
#define IN_SIZE         16384UL         /* bytes read at a time */

typedef struct {
    SRC     *src;
    uint32_t data_start, data_end;      /* the samples, as file positions */
    uint32_t frames;                    /* in the file, 0 when not known */
    uint32_t done;                      /* frames given out so far, at the file's rate */
    uint16_t tag;                       /* WF_*, never WF_EXTENSIBLE */
    uint16_t channels;
    uint16_t bits;                      /* container bits a sample */
    uint16_t block;                     /* block_align: bytes a frame, or an ADPCM block */
    uint32_t block_frames;              /* ADPCM: frames a block */
    uint8_t  down, mono;                /* what was asked for */
    uint8_t  out_ch;                    /* channels given out: 1 or 2 */
    uint32_t rate;
    /* MS ADPCM's coefficients */
    int      ncoefs;
    int16_t  coef1[MAX_COEFS], coef2[MAX_COEFS];
    /* the bytes read and not yet used */
    uint8_t *in;
    uint32_t in_len, in_used;
    /* ADPCM: one block decoded, frames waiting to go out */
    int16_t *blk;                       /* block_frames * channels */
    uint32_t blk_have, blk_used;
    /* half/quarter rate: frames waiting to be averaged */
    int32_t  acc[2];
    uint32_t acc_count;
} WAVSTATE;

static const uint16_t ima_step[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t ima_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static const uint16_t ms_adapt[16] = {
    230, 230, 230, 230, 307, 409, 512, 614,
    768, 614, 512, 409, 307, 230, 230, 230
};

static const int16_t ms_coef1[7] = { 256, 512, 0, 192, 240, 460, 392 };
static const int16_t ms_coef2[7] = { 0, -256, 0, 64, 0, -208, -232 };

/* the GUID of an EXTENSIBLE format, after its first two bytes (which
   are the old format code) */
static const uint8_t ksguid_tail[14] = {
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
    0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

/* ------------------------------------------------------------------ */
/* one sample of each coding                                           */
/* ------------------------------------------------------------------ */
static int32_t from_alaw( uint8_t byte )
{
    int32_t val, seg;

    byte ^= 0x55;
    val = (int32_t)(byte & 0x0F) << 4;
    seg = (byte >> 4) & 7;
    if ( seg == 0 ) {
        val += 8;
    } else {
        val = (val + 0x108) << (seg - 1);
    }
    return (byte & 0x80) ? val : -val;
}

static int32_t from_mulaw( uint8_t byte )
{
    int32_t val;

    byte = (uint8_t)~byte;
    val = (((int32_t)(byte & 0x0F) << 3) + 0x84) << ((byte >> 4) & 7);
    return (byte & 0x80) ? 0x84 - val : val - 0x84;
}

/* an IEEE single, its bits in "bits", as a 16-bit sample */
static int32_t from_float32( uint32_t bits )
{
    int exp = (int)((bits >> 23) & 0xFF);
    int32_t mant, shift, val;

    if ( exp == 0 ) {
        return 0;                       /* zero, or too small to hear */
    }
    if ( exp == 0xFF ) {
        return 0;                       /* not a number: silence rather than a click */
    }
    mant = (int32_t)((bits & 0x7FFFFF) | 0x800000);
    shift = exp - 135;                  /* value * 32768 = mant * 2^(exp - 127 - 23 + 15) */
    if ( shift >= 0 ) {
        val = 32767;
    } else if ( shift <= -24 ) {
        val = 0;
    } else {
        val = mant >> -shift;
        if ( val > 32767 ) {
            val = 32767;
        }
    }
    return (bits & 0x80000000UL) ? -val : val;
}

/* an IEEE double by its upper four bytes, which hold the sign, the
   exponent and 20 bits of the fraction - more than a 16-bit sample
   can show */
static int32_t from_float64( uint32_t hi )
{
    int exp = (int)((hi >> 20) & 0x7FF);
    int32_t mant, shift, val;

    if ( exp == 0 || exp == 0x7FF ) {
        return 0;
    }
    mant = (int32_t)((hi & 0xFFFFF) | 0x100000);
    shift = exp - 1028;                 /* mant * 2^(exp - 1023 - 20 + 15) */
    if ( shift >= 0 ) {
        val = 32767;
    } else if ( shift <= -21 ) {
        val = 0;
    } else {
        val = mant >> -shift;
        if ( val > 32767 ) {
            val = 32767;
        }
    }
    return (hi & 0x80000000UL) ? -val : val;
}

/* one channel's sample at "at", as 16 bits */
static int32_t pcm_sample( const WAVSTATE *wav, const uint8_t *at )
{
    switch ( wav->tag ) {
    case WF_PCM:
        switch ( wav->bits ) {
        case 8:
            return ((int32_t)at[0] - 128) << 8;
        case 16:
            return (int16_t)LE16( at );
        case 24:
            return (int16_t)LE16( at + 1 );
        default:
            return (int16_t)LE16( at + 2 );
        }
    case WF_FLOAT:
        return wav->bits == 32 ? from_float32( LE32( at ) ) : from_float64( LE32( at + 4 ) );
    case WF_ALAW:
        return from_alaw( at[0] );
    case WF_MULAW:
        return from_mulaw( at[0] );
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ADPCM blocks                                                        */
/* ------------------------------------------------------------------ */
static int32_t clamp16( int32_t val )
{
    return FX_SAT16( val );
}

/* An IMA block: a header of four bytes for each channel (the first
   sample and the step index), then the nibbles four bytes - eight
   samples - of one channel after four of the next.  -> frames made */
static unsigned int ima_block( WAVSTATE *wav, const uint8_t *data, unsigned int len )
{
    int32_t pred[MAX_CHANNELS];
    int index[MAX_CHANNELS];
    unsigned int ch, frame, nch = wav->channels, made = 0, group, nib, pos;
    int32_t step, diff;
    int code;

    if ( len < 4 * nch ) {
        return 0;
    }
    for ( ch = 0; ch < nch; ch++ ) {
        pred[ch] = (int16_t)LE16( data + 4 * ch );
        index[ch] = data[4 * ch + 2];
        if ( index[ch] > 88 ) {
            index[ch] = 88;
        }
        wav->blk[ch] = (int16_t)pred[ch];
    }
    made = 1;
    pos = 4 * nch;
    /* each pass takes eight frames: 4 bytes from every channel */
    for ( group = 0; made < wav->block_frames && pos + 4 * nch <= len; group++ ) {
        for ( ch = 0; ch < nch; ch++ ) {
            for ( nib = 0; nib < 8; nib++ ) {
                frame = made + nib;
                code = (data[pos + ch * 4 + nib / 2] >> ((nib & 1) * 4)) & 0x0F;
                step = ima_step[index[ch]];
                diff = step >> 3;
                if ( code & 4 ) {
                    diff += step;
                }
                if ( code & 2 ) {
                    diff += step >> 1;
                }
                if ( code & 1 ) {
                    diff += step >> 2;
                }
                pred[ch] = clamp16( (code & 8) ? pred[ch] - diff : pred[ch] + diff );
                index[ch] += ima_index[code & 7];
                if ( index[ch] < 0 ) {
                    index[ch] = 0;
                } else if ( index[ch] > 88 ) {
                    index[ch] = 88;
                }
                if ( frame < wav->block_frames ) {
                    wav->blk[frame * nch + ch] = (int16_t)pred[ch];
                }
            }
        }
        pos += 4 * nch;
        made += 8;
    }
    return made < wav->block_frames ? made : wav->block_frames;
}

/* An MS ADPCM block: for each channel a predictor number, then for
   each a delta, a first and a second sample (the second is the older
   one and is played first); then the nibbles, high one first,
   channels taking turns.  -> frames made */
static unsigned int ms_block( WAVSTATE *wav, const uint8_t *data, unsigned int len )
{
    int32_t c1[MAX_CHANNELS], c2[MAX_CHANNELS], delta[MAX_CHANNELS];
    int32_t s1[MAX_CHANNELS], s2[MAX_CHANNELS];
    unsigned int ch, nch = wav->channels, pos, made, nibbles, k;
    int pick, code;
    int32_t pred, val;

    if ( len < 7 * nch ) {
        return 0;
    }
    for ( ch = 0; ch < nch; ch++ ) {
        pick = data[ch];
        if ( pick >= wav->ncoefs ) {
            pick = 0;
        }
        c1[ch] = wav->coef1[pick];
        c2[ch] = wav->coef2[pick];
        delta[ch] = (int16_t)LE16( data + nch + 2 * ch );
        s1[ch] = (int16_t)LE16( data + 3 * nch + 2 * ch );
        s2[ch] = (int16_t)LE16( data + 5 * nch + 2 * ch );
        wav->blk[ch] = (int16_t)s2[ch];
        if ( wav->block_frames > 1 ) {
            wav->blk[nch + ch] = (int16_t)s1[ch];
        }
    }
    made = wav->block_frames > 1 ? 2 : 1;
    pos = 7 * nch;
    nibbles = (len - pos) * 2;
    for ( k = 0; k < nibbles && made < wav->block_frames; k++ ) {
        ch = k % nch;
        code = (data[pos + k / 2] >> ((k & 1) ? 0 : 4)) & 0x0F;
        pred = (s1[ch] * c1[ch] + s2[ch] * c2[ch]) >> 8;
        val = clamp16( pred + (int32_t)(code >= 8 ? code - 16 : code) * delta[ch] );
        s2[ch] = s1[ch];
        s1[ch] = val;
        delta[ch] = (ms_adapt[code] * delta[ch]) >> 8;
        if ( delta[ch] < 16 ) {
            delta[ch] = 16;
        } else if ( delta[ch] > 0x7FFFFFFFL / 768 ) {
            delta[ch] = 0x7FFFFFFFL / 768;  /* the next step's 768 x delta must fit */
        }
        wav->blk[made * nch + ch] = (int16_t)val;
        if ( ch == nch - 1 ) {
            made++;
        }
    }
    return made;
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */
/* At least "want" bytes in wav->in from in_used on, or as many as the
   data has left.  -> the bytes there */
static unsigned int fill_in( WAVSTATE *wav, unsigned int want )
{
    unsigned int keep = wav->in_len - wav->in_used, room;
    uint32_t left;
    unsigned int got;

    if ( keep >= want ) {
        return keep;
    }
    mem_cpy( wav->in, wav->in + wav->in_used, keep );
    wav->in_len = keep;
    wav->in_used = 0;
    left = wav->data_end > src_tell( wav->src ) ? wav->data_end - src_tell( wav->src ) : 0;
    room = IN_SIZE - keep;
    if ( room > left ) {
        room = left;
    }
    got = room ? src_read( wav->src, wav->in + keep, room ) : 0;
    wav->in_len += got;
    return wav->in_len;
}

/* The next frame of the file, mixed to the channels given out, into
   out[0] and out[1].  0 at the end */
static int next_frame( WAVSTATE *wav, int32_t *out )
{
    unsigned int nch = wav->channels, ch, have, made;
    const uint8_t *at;
    int32_t val, left = 0, right = 0;

    if ( wav->tag == WF_IMAADPCM || wav->tag == WF_MSADPCM ) {
        if ( wav->blk_used >= wav->blk_have ) {
            have = fill_in( wav, wav->block );
            if ( have == 0 ) {
                return 0;
            }
            if ( have > wav->block ) {
                have = wav->block;
            }
            at = wav->in + wav->in_used;
            made = wav->tag == WF_IMAADPCM ? ima_block( wav, at, have ) : ms_block( wav, at, have );
            wav->in_used += have;
            wav->blk_have = made;
            wav->blk_used = 0;
            if ( made == 0 ) {
                return 0;
            }
        }
        for ( ch = 0; ch < nch; ch++ ) {
            val = wav->blk[wav->blk_used * nch + ch];
            if ( ch & 1 ) {
                right += val;
            } else {
                left += val;
            }
        }
        wav->blk_used++;
    } else {
        have = fill_in( wav, wav->block );
        if ( have < wav->block ) {
            return 0;
        }
        at = wav->in + wav->in_used;
        for ( ch = 0; ch < nch; ch++ ) {
            val = pcm_sample( wav, at + ch * (wav->bits / 8) );
            if ( ch & 1 ) {
                right += val;
            } else {
                left += val;
            }
        }
        wav->in_used += wav->block;
    }
    if ( nch == 1 ) {
        right = left;
    } else if ( nch > 2 ) {
        left /= (int32_t)((nch + 1) / 2);
        right /= (int32_t)(nch / 2);
    }
    if ( wav->out_ch == 1 ) {
        out[0] = nch == 1 ? left : (left + right) >> 1;
    } else {
        out[0] = left;
        out[1] = right;
    }
    wav->done++;
    return 1;
}

/* The common case by the block: 8 or 16 bits, one or two channels,
   given out as they are.  A function call a sample is a fifth of a
   386's time at 44.1 kHz stereo, so this runs straight through. */
static int fast_pcm( WAVSTATE *wav, DECBUF *buf )
{
    int16_t *pcm = buf->pcm;
    const uint8_t *at;
    unsigned int have, count, i, total;

    while ( buf->frames < buf->max ) {
        have = fill_in( wav, wav->block ) / wav->block;
        if ( have == 0 ) {
            break;
        }
        count = buf->max - buf->frames;
        if ( count > have ) {
            count = have;
        }
        at = wav->in + wav->in_used;
        total = count * wav->channels;
        if ( wav->bits == 16 ) {
            if ( wav->block == 2 * wav->channels ) {
                mem_cpy( pcm, at, total * 2 );      /* little-endian on both sides */
            } else {
                for ( i = 0; i < count; i++ ) {
                    pcm[i * wav->channels] = (int16_t)LE16( at + i * wav->block );
                    if ( wav->channels == 2 ) {
                        pcm[i * 2 + 1] = (int16_t)LE16( at + i * wav->block + 2 );
                    }
                }
            }
        } else if ( wav->block == wav->channels ) {
            for ( i = 0; i < total; i++ ) {
                pcm[i] = (int16_t)(((int32_t)at[i] - 128) << 8);
            }
        } else {
            for ( i = 0; i < count; i++ ) {
                pcm[i * wav->channels] = (int16_t)(((int32_t)at[i * wav->block] - 128) << 8);
                if ( wav->channels == 2 ) {
                    pcm[i * 2 + 1] = (int16_t)(((int32_t)at[i * wav->block + 1] - 128) << 8);
                }
            }
        }
        pcm += total;
        wav->in_used += count * wav->block;
        wav->done += count;
        buf->frames += count;
    }
    return wav->src->error && buf->frames == 0 ? DE_READ : DE_OK;
}

static int wav_decode( void *state, DECBUF *buf )
{
    WAVSTATE *wav = (WAVSTATE *)state;
    int16_t *pcm = buf->pcm;
    int32_t frame[2];
    unsigned int per = 1UL << wav->down, ch;

    buf->frames = 0;
    buf->ev_count = 0;
    if ( wav->tag == WF_PCM && (wav->bits == 16 || wav->bits == 8) && wav->down == 0
         && wav->out_ch == wav->channels ) {
        return fast_pcm( wav, buf );
    }
    while ( buf->frames < buf->max ) {
        if ( !next_frame( wav, frame ) ) {
            if ( wav->src->error ) {
                return DE_READ;
            }
            break;
        }
        for ( ch = 0; ch < wav->out_ch; ch++ ) {
            wav->acc[ch] += frame[ch];
        }
        if ( ++wav->acc_count < per ) {
            continue;
        }
        for ( ch = 0; ch < wav->out_ch; ch++ ) {
            *pcm++ = (int16_t)clamp16( wav->acc[ch] >> wav->down );
            wav->acc[ch] = 0;
        }
        wav->acc_count = 0;
        buf->frames++;
    }
    return DE_OK;
}

/* ------------------------------------------------------------------ */
/* opening                                                             */
/* ------------------------------------------------------------------ */
static int wav_detect( SRC *src, const uint8_t *head, unsigned int len, const char *ext )
{
    (void)src;
    (void)ext;
    if ( len >= 12 && mem_cmp( head, "RIFF", 4 ) == 0 && mem_cmp( head + 8, "WAVE", 4 ) == 0 ) {
        return 100;
    }
    return 0;
}

/* The INFO list of a LIST chunk of "len" bytes at the file's position:
   the title (INAM), artist (IART) and album (IPRD) */
static void read_info( SRC *src, uint32_t len, DECINFO *info )
{
    uint8_t hdr[8], text[TAG_LEN];
    uint32_t end = src_tell( src ) + len, size;
    unsigned int take;

    if ( len < 4 || src_read( src, hdr, 4 ) != 4 || mem_cmp( hdr, "INFO", 4 ) != 0 ) {
        return;
    }
    while ( src_tell( src ) + 8 <= end ) {
        if ( src_read( src, hdr, 8 ) != 8 ) {
            return;
        }
        size = LE32( hdr + 4 );
        if ( size > end - src_tell( src ) ) {
            return;
        }
        take = size < TAG_LEN ? size : TAG_LEN;
        if ( src_read( src, text, take ) != take ) {
            return;
        }
        if ( mem_cmp( hdr, "INAM", 4 ) == 0 ) {
            tag_latin1( info->title, TAG_LEN, text, take );
        } else if ( mem_cmp( hdr, "IART", 4 ) == 0 ) {
            tag_latin1( info->artist, TAG_LEN, text, take );
        } else if ( mem_cmp( hdr, "IPRD", 4 ) == 0 ) {
            tag_latin1( info->album, TAG_LEN, text, take );
        }
        if ( src_seek( src, src_tell( src ) + (size - take) + (size & 1) ) != 0 ) {
            return;
        }
    }
}

/* What the format chunk says: -> DE_OK, DE_FORMAT, DE_UNSUPPORTED */
static int read_fmt( WAVSTATE *wav, const uint8_t *fmt, unsigned int len, DECINFO *info )
{
    uint32_t rate, extra;
    unsigned int i;
    uint16_t tag;

    if ( len < 16 ) {
        return DE_FORMAT;
    }
    tag = LE16( fmt );
    wav->channels = LE16( fmt + 2 );
    rate = LE32( fmt + 4 );
    wav->block = LE16( fmt + 12 );
    wav->bits = LE16( fmt + 14 );
    extra = len >= 18 ? LE16( fmt + 16 ) : 0;
    if ( extra > len - 18 ) {
        extra = len >= 18 ? len - 18 : 0;
    }
    if ( tag == WF_EXTENSIBLE ) {
        if ( extra < 22 || mem_cmp( fmt + 26, ksguid_tail, 14 ) != 0 ) {
            sfmt( info->format, sizeof( info->format ), "Wave, an unknown coding" );
            return DE_UNSUPPORTED;
        }
        /* the GUID begins with the old code.  Fewer valid bits than the
           container leaves the samples left-aligned in it, so the
           container size is the one that matters here */
        tag = LE16( fmt + 24 );
    }
    if ( wav->channels == 0 || wav->channels > MAX_CHANNELS || rate < 1000 || rate > 384000UL
         || wav->block == 0 ) {
        return DE_FORMAT;
    }
    wav->rate = rate;
    switch ( tag ) {
    case WF_PCM:
        if ( wav->bits != 8 && wav->bits != 16 && wav->bits != 24 && wav->bits != 32 ) {
            sfmt( info->format, sizeof( info->format ), "Wave, %u-bit PCM", wav->bits );
            return DE_UNSUPPORTED;
        }
        break;
    case WF_FLOAT:
        if ( wav->bits != 32 && wav->bits != 64 ) {
            return DE_FORMAT;
        }
        break;
    case WF_ALAW:
    case WF_MULAW:
        wav->bits = 8;
        break;
    case WF_IMAADPCM:
        if ( wav->bits != 4 || wav->block < 4 * wav->channels + 4 ) {
            sfmt( info->format, sizeof( info->format ), "Wave, %u-bit IMA ADPCM", wav->bits );
            return DE_UNSUPPORTED;
        }
        /* the header's sample, then two a byte of what is left */
        wav->block_frames = (uint32_t)(wav->block / wav->channels - 4) * 2 + 1;
        break;
    case WF_MSADPCM:
        if ( wav->bits != 4 || wav->block < 7 * wav->channels + 1 ) {
            return DE_FORMAT;
        }
        wav->block_frames = (uint32_t)(wav->block - 7 * wav->channels) * 2 / wav->channels + 2;
        /* the coefficients: the file's own when it lists them */
        wav->ncoefs = 7;
        for ( i = 0; i < 7; i++ ) {
            wav->coef1[i] = ms_coef1[i];
            wav->coef2[i] = ms_coef2[i];
        }
        if ( extra >= 4 ) {
            uint32_t n = LE16( fmt + 20 );
            if ( n >= 1 && n <= MAX_COEFS && 4 + n * 4 <= extra ) {
                wav->ncoefs = (int)n;
                for ( i = 0; i < n; i++ ) {
                    wav->coef1[i] = (int16_t)LE16( fmt + 22 + i * 4 );
                    wav->coef2[i] = (int16_t)LE16( fmt + 24 + i * 4 );
                }
            }
        }
        break;
    default:
        sfmt( info->format, sizeof( info->format ), "Wave, coding %04Xh", tag );
        return DE_UNSUPPORTED;
    }
    if ( (tag == WF_PCM || tag == WF_FLOAT || tag == WF_ALAW || tag == WF_MULAW)
         && wav->block < wav->channels * (wav->bits / 8) ) {
        return DE_FORMAT;
    }
    wav->tag = tag;
    return DE_OK;
}

static void wav_close( void *state )
{
    WAVSTATE *wav = (WAVSTATE *)state;

    if ( wav != NULL ) {
        if ( wav->in != NULL ) {
            xfree( wav->in );
        }
        if ( wav->blk != NULL ) {
            xfree( wav->blk );
        }
        xfree( wav );
    }
}

static int wav_open( SRC *src, const DECOPT *opt, DECINFO *info, void **state )
{
    WAVSTATE *wav;
    uint8_t hdr[12], fmt[64];
    uint32_t size;
    unsigned int take;
    uint32_t data_len = 0, pad;
    int have_fmt = 0, have_data = 0, rc;
    unsigned int chunks;
    uint32_t units;

    *state = NULL;
    if ( src_read( src, hdr, 12 ) != 12 || mem_cmp( hdr, "RIFF", 4 ) != 0
         || mem_cmp( hdr + 8, "WAVE", 4 ) != 0 ) {
        return DE_FORMAT;
    }
    wav = (WAVSTATE *)try_alloc( sizeof( WAVSTATE ) );
    if ( wav == NULL ) {
        return DE_NOMEM;
    }
    mem_set( wav, 0, sizeof( WAVSTATE ) );
    wav->src = src;
    /* the chunks, as far as the file goes; a chunk count bounds the walk
       through a file of many tiny chunks */
    for ( chunks = 0; chunks < 10000 && src_left( src ) >= 8; chunks++ ) {
        if ( src_read( src, hdr, 8 ) != 8 ) {
            break;
        }
        size = LE32( hdr + 4 );
        pad = size & 1;                 /* a chunk of odd size has a byte after it */
        if ( mem_cmp( hdr, "fmt ", 4 ) == 0 && !have_fmt ) {
            take = size < sizeof( fmt ) ? size : sizeof( fmt );
            if ( src_read( src, fmt, take ) != take ) {
                break;
            }
            rc = read_fmt( wav, fmt, take, info );
            if ( rc != DE_OK ) {
                wav_close( wav );
                return rc;
            }
            have_fmt = 1;
            size -= take;
        } else if ( mem_cmp( hdr, "data", 4 ) == 0 && !have_data ) {
            wav->data_start = src_tell( src );
            data_len = size == 0 || size > src_left( src ) ? src_left( src ) : size;
            wav->data_end = wav->data_start + data_len;
            have_data = 1;
            size = data_len;
        } else if ( mem_cmp( hdr, "LIST", 4 ) == 0 && size <= src_left( src ) ) {
            uint32_t at = src_tell( src );
            read_info( src, size, info );
            src_seek( src, at );
        }
        if ( size > src_left( src ) ) {
            break;
        }
        if ( src_skip( src, size + (pad && size < src_left( src ) ? 1 : 0) ) != 0 ) {
            break;
        }
    }
    if ( !have_fmt || !have_data ) {
        wav_close( wav );
        return DE_FORMAT;
    }
    if ( wav->tag == WF_IMAADPCM || wav->tag == WF_MSADPCM ) {
        wav->blk = (int16_t *)try_alloc( wav->block_frames * wav->channels * sizeof( int16_t ) );
        if ( wav->blk == NULL ) {
            wav_close( wav );
            return DE_NOMEM;
        }
        units = data_len / wav->block;
        /* over 4G frames (a file of several gigabytes) counts as 4G */
        wav->frames = units > 0xFFFFFFFFUL / wav->block_frames ? 0xFFFFFFFFUL : units * wav->block_frames;
    } else {
        wav->frames = data_len / wav->block;
    }
    if ( wav->block > IN_SIZE / 2 ) {
        wav_close( wav );
        return DE_FORMAT;
    }
    wav->in = (uint8_t *)try_alloc( IN_SIZE );
    if ( wav->in == NULL ) {
        wav_close( wav );
        return DE_NOMEM;
    }
    wav->down = opt->down > 2 ? 2 : opt->down;
    while ( wav->down > 0 && (wav->rate >> wav->down) < 8000 ) {
        wav->down--;                    /* not below 8 kHz */
    }
    wav->mono = opt->mono;
    wav->out_ch = (uint8_t)(wav->channels == 1 || opt->mono ? 1 : 2);

    info->file_rate = wav->rate;
    info->rate = wav->rate >> wav->down;
    info->file_channels = (uint8_t)wav->channels;
    info->channels = wav->out_ch;
    info->bits = (uint8_t)(wav->tag == WF_IMAADPCM || wav->tag == WF_MSADPCM ? 4 : wav->bits);
    info->flags = DIF_SEEK;
    info->length_ms = fx_umuldiv( wav->frames, 1000, wav->rate );
    /* bits a millisecond; a large file divided first, so the quotient
       fits 32 bits whatever the length */
    if ( info->length_ms == 0 ) {
        info->kbps = 0;
    } else if ( data_len < 0x20000000UL ) {
        info->kbps = fx_umuldiv( data_len, 8, info->length_ms );
    } else {
        info->kbps = data_len / info->length_ms * 8;
    }
    switch ( wav->tag ) {
    case WF_PCM:
        sfmt( info->format, sizeof( info->format ), "Wave, %u-bit PCM", wav->bits );
        break;
    case WF_FLOAT:
        sfmt( info->format, sizeof( info->format ), "Wave, %u-bit floating point", wav->bits );
        break;
    case WF_ALAW:
        sfmt( info->format, sizeof( info->format ), "Wave, A-law" );
        break;
    case WF_MULAW:
        sfmt( info->format, sizeof( info->format ), "Wave, mu-law" );
        break;
    case WF_IMAADPCM:
        sfmt( info->format, sizeof( info->format ), "Wave, IMA ADPCM" );
        break;
    default:
        sfmt( info->format, sizeof( info->format ), "Wave, Microsoft ADPCM" );
        break;
    }
    src_seek( src, wav->data_start );
    *state = wav;
    return DE_OK;
}

static int wav_seek( void *state, uint32_t ms, uint32_t *at_ms )
{
    WAVSTATE *wav = (WAVSTATE *)state;
    uint32_t frame, unit, pos;

    /* a time past what 32 bits of frames can say is the end anyway */
    if ( ms > 0xFFFFFFFFUL / wav->rate * 1000UL ) {
        ms = 0xFFFFFFFFUL / wav->rate * 1000UL;
    }
    frame = fx_umuldiv( ms, wav->rate, 1000 );
    if ( wav->frames && frame > wav->frames ) {
        frame = wav->frames;
    }
    if ( wav->tag == WF_IMAADPCM || wav->tag == WF_MSADPCM ) {
        unit = frame / wav->block_frames;           /* whole blocks only */
        frame = unit * wav->block_frames;
        pos = wav->data_start + unit * wav->block;
    } else {
        pos = wav->data_start + frame * wav->block;
    }
    if ( pos > wav->data_end ) {
        pos = wav->data_end;
    }
    src_seek( wav->src, pos );
    wav->in_len = wav->in_used = 0;
    wav->blk_have = wav->blk_used = 0;
    wav->acc[0] = wav->acc[1] = 0;
    wav->acc_count = 0;
    wav->done = frame;
    *at_ms = fx_umuldiv( frame, 1000, wav->rate );
    return DE_OK;
}

const CODEC codec_wav = {
    "Wave",
    "WAV",
    wav_detect,
    wav_open,
    wav_decode,
    wav_seek,
    wav_close
};
