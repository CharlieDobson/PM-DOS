/*
 * VOC.C - Creative Voice files, the Sound Blaster's own format (DEC.H).
 *
 * A VOC FILE IS A HEADER AND A CHAIN OF BLOCKS: a type byte, a 24-bit
 * length, and the block.  Type 1 is sound (a time constant, a packing,
 * the samples), 2 is more sound like the last, 3 is silence, 5 is
 * text, 6 and 7 bracket a part that repeats, 8 says the next sound
 * block is stereo or faster than a time constant can say, and 9 (from
 * version 1.20) is sound with its rate, size and coding said outright.
 * 0 ends the file.  Markers and anything unknown are stepped over.
 *
 * THE SOUND BLASTER'S ADPCM is decoded here as its DSP decodes it: the
 * first byte of a sound block is the starting sample, and every code
 * after it is a step up or down whose size depends on how big the
 * last steps were - four sizes for the 4-bit coding, five for the
 * 2.6-bit (three codes a byte) and six for the 2-bit (four a byte).
 * A big step makes the next ones bigger; a code of nothing makes them
 * smaller.  The samples are 8 bits, as the card plays them.
 *
 * THE 2.6-BIT CODING IS NOT PLAYED.  The two descriptions of it
 * disagree, and neither decodes the one real file to hand into
 * the same speech its 4- and 2-bit versions hold - a wrong decoding
 * of ADPCM is loud noise, not a quieter version of the sound.  Such a
 * file is refused by name until it can be checked against a card.
 *
 * ONE FORMAT AT A TIME.  A file whose later blocks change the rate or
 * the channels is played at the first block's: the samples keep
 * coming, a little fast or slow, which is better than stopping.  A
 * repeat that says "for ever" is played eight times, so a file always
 * ends.
 *
 * SEEKING IS BY BLOCKS: the blocks are walked from the start, counting
 * their samples, and the one the time falls in is played from its
 * start - or from inside it, for plain 8- and 16-bit sound.
 */
#include "rt.h"
#include "fix.h"
#include "dec.h"

#define CODE_PCM8       0x0000
#define CODE_ADPCM4     0x0001
#define CODE_ADPCM3     0x0002
#define CODE_ADPCM2     0x0003
#define CODE_PCM16      0x0004
#define CODE_ALAW       0x0006
#define CODE_MULAW      0x0007

#define IN_SIZE         8192UL
#define MAX_REPEAT      8
#define BLOCK_LIMIT     100000UL        /* blocks walked, at most: a bound on a damaged chain */

typedef struct {
    SRC     *src;
    uint32_t first_block;               /* the file position of the first block */
    uint32_t rate;                      /* the first sound block's */
    uint8_t  channels;
    uint8_t  out_ch;
    uint8_t  down;
    uint8_t  bits;                      /* 8 or 16 */
    /* the block being played */
    uint32_t left;                      /* its bytes not yet read */
    uint32_t silence;                   /* or frames of silence still to give */
    uint16_t code;                      /* CODE_* */
    uint8_t  blk_channels;
    uint8_t  need_ref;                  /* ADPCM: the next byte is the starting sample */
    int32_t  ref;                       /* ADPCM: the sample, 0..255 */
    int32_t  level;                     /* ADPCM: the step size, 0.. */
    /* the next block's format, from a type 8 block */
    uint8_t  ext_pending;
    uint8_t  ext_channels;
    uint32_t ext_rate;
    /* repeats */
    uint32_t rep_pos;                   /* the file position after the type 6 block */
    uint32_t rep_count;                 /* times still to go, 0 for none */
    uint32_t blocks;
    /* bytes read and not yet used */
    uint8_t *in;
    uint32_t in_len, in_used;
    /* samples made and not yet given, mono or stereo frames of int16_t */
    int16_t  pend[8];
    uint32_t pend_have, pend_used;
    /* half/quarter rate */
    int32_t  acc[2];
    uint32_t acc_count;
    uint32_t frames_total;              /* from the walk at open, for the length */
    uint16_t first_code;                /* the first sound block's coding */
} VOCSTATE;

/* ------------------------------------------------------------------ */
/* the DSP's ADPCM                                                     */
/* ------------------------------------------------------------------ */
/* One code of "bits" (4, 3 or 2): a sign bit and a magnitude.  At step
   level L the step is (magnitude << L) + (L ? 1 << (L - 1) : 0); the
   2.6-bit coding's top level steps by tens.  The level goes up after
   the largest magnitudes and down after a magnitude of nothing. */
static int32_t adpcm_step( VOCSTATE *voc, int code, int bits )
{
    int sign_bit = 1 << (bits - 1), mag = code & (sign_bit - 1), top, up_at;
    int32_t step, level = voc->level;

    if ( bits == 4 ) {
        top = 3;
        up_at = 5;
    } else if ( bits == 3 ) {
        top = 4;
        up_at = 3;
    } else {
        top = 5;
        up_at = 1;
    }
    if ( bits == 3 && level == 4 ) {
        step = mag * 10 + 5;
    } else {
        step = ((int32_t)mag << level) + (level ? 1L << (level - 1) : 0);
    }
    voc->ref += (code & sign_bit) ? -step : step;
    if ( voc->ref < 0 ) {
        voc->ref = 0;
    } else if ( voc->ref > 255 ) {
        voc->ref = 255;
    }
    if ( mag >= up_at && level < top ) {
        voc->level++;
    } else if ( mag == 0 && level > 0 ) {
        voc->level--;
    }
    return voc->ref;
}

static int32_t from_alaw( uint8_t byte )
{
    int32_t val, seg;

    byte ^= 0x55;
    val = (int32_t)(byte & 0x0F) << 4;
    seg = (byte >> 4) & 7;
    val = seg == 0 ? val + 8 : (val + 0x108) << (seg - 1);
    return (byte & 0x80) ? val : -val;
}

static int32_t from_mulaw( uint8_t byte )
{
    int32_t val;

    byte = (uint8_t)~byte;
    val = (((int32_t)(byte & 0x0F) << 3) + 0x84) << ((byte >> 4) & 7);
    return (byte & 0x80) ? 0x84 - val : val - 0x84;
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */
static int next_byte( VOCSTATE *voc )
{
    unsigned int room, got;

    if ( voc->left == 0 ) {
        return -1;
    }
    if ( voc->in_used >= voc->in_len ) {
        room = voc->left < IN_SIZE ? voc->left : IN_SIZE;
        got = src_read( voc->src, voc->in, room );
        if ( got == 0 ) {
            voc->left = 0;
            return -1;
        }
        voc->in_len = got;
        voc->in_used = 0;
    }
    voc->left--;
    return voc->in[voc->in_used++];
}

/* the header of the next block that holds sound or silence: its state
   set up, everything before it stepped over.  0 at the end */
static int next_block( VOCSTATE *voc )
{
    uint8_t hdr[16];
    uint32_t len, pos, rate;
    int type;

    for ( ;; ) {
        if ( voc->blocks++ > BLOCK_LIMIT ) {
            return 0;
        }
        /* what is left of the last block is stepped over */
        if ( voc->left ) {
            if ( src_skip( voc->src, voc->left - (voc->in_len - voc->in_used) > voc->left
                           ? 0 : voc->left - (voc->in_len - voc->in_used) ) != 0 ) {
                return 0;
            }
        }
        voc->left = 0;
        voc->in_len = voc->in_used = 0;
        type = src_byte( voc->src );
        if ( type <= 0 ) {
            return 0;                   /* the end, or the file is */
        }
        if ( src_read( voc->src, hdr, 3 ) != 3 ) {
            return 0;
        }
        len = hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16);
        pos = src_tell( voc->src );
        if ( len > src_left( voc->src ) ) {
            len = src_left( voc->src );
        }
        switch ( type ) {
        case 1:                         /* sound: time constant, packing */
            if ( len < 2 || src_read( voc->src, hdr, 2 ) != 2 ) {
                return 0;
            }
            voc->left = len - 2;
            voc->code = hdr[1] <= 3 ? hdr[1] : CODE_PCM8;
            voc->blk_channels = voc->ext_pending ? voc->ext_channels : 1;
            voc->ext_pending = 0;
            voc->need_ref = voc->code != CODE_PCM8;
            voc->level = 0;
            return 1;
        case 2:                         /* more of the same */
            voc->left = len;
            return 1;
        case 3:                         /* silence: a length and a time constant */
            if ( len < 3 || src_read( voc->src, hdr, 3 ) != 3 ) {
                return 0;
            }
            voc->silence = (uint32_t)LE16( hdr ) + 1;
            src_seek( voc->src, pos + len );
            return 1;
        case 6:                         /* repeat from here */
            if ( len >= 2 && src_read( voc->src, hdr, 2 ) == 2 ) {
                voc->rep_count = LE16( hdr ) == 0xFFFF ? MAX_REPEAT : (uint32_t)LE16( hdr ) + 1;
                if ( voc->rep_count > MAX_REPEAT ) {
                    voc->rep_count = MAX_REPEAT;
                }
                voc->rep_pos = pos + len;
            }
            src_seek( voc->src, pos + len );
            break;
        case 7:                         /* the end of a repeat */
            src_seek( voc->src, pos + len );
            if ( voc->rep_count > 1 ) {
                voc->rep_count--;
                src_seek( voc->src, voc->rep_pos );
            } else {
                voc->rep_count = 0;
            }
            break;
        case 8:                         /* the next sound block's real format */
            if ( len >= 4 && src_read( voc->src, hdr, 4 ) == 4 ) {
                voc->ext_channels = (uint8_t)(hdr[3] ? 2 : 1);
                rate = 65536UL - LE16( hdr );
                voc->ext_rate = rate ? 256000000UL / rate / voc->ext_channels : 0;
                voc->ext_pending = 1;
            }
            src_seek( voc->src, pos + len );
            break;
        case 9:                         /* sound, said in full */
            if ( len < 12 || src_read( voc->src, hdr, 12 ) != 12 ) {
                return 0;
            }
            voc->left = len - 12;
            voc->code = LE16( hdr + 6 );
            voc->blk_channels = (uint8_t)(hdr[5] == 2 ? 2 : 1);
            if ( voc->code == CODE_PCM8 && hdr[4] == 16 ) {
                voc->code = CODE_PCM16;
            }
            voc->need_ref = voc->code >= CODE_ADPCM4 && voc->code <= CODE_ADPCM2;
            voc->level = 0;
            return 1;
        default:                        /* text, markers, anything else */
            src_seek( voc->src, pos + len );
            break;
        }
    }
}

/* the next frame of the file into out[0..1]; 0 at the end */
static int next_frame( VOCSTATE *voc, int32_t *out )
{
    int byte = 0, byte2, k, ch;
    int32_t val[2];

    for ( ;; ) {
        if ( voc->pend_used < voc->pend_have ) {
            val[0] = voc->pend[voc->pend_used++];
            val[1] = val[0];
            break;
        }
        if ( voc->silence ) {
            voc->silence--;
            val[0] = val[1] = 0;
            break;
        }
        if ( voc->left == 0 ) {
            if ( !next_block( voc ) ) {
                return 0;
            }
            continue;
        }
        switch ( voc->code ) {
        case CODE_ADPCM4:
        case CODE_ADPCM3:
        case CODE_ADPCM2:
            byte = next_byte( voc );
            if ( byte < 0 ) {
                continue;
            }
            voc->pend_have = voc->pend_used = 0;
            if ( voc->need_ref ) {
                voc->ref = byte;
                voc->need_ref = 0;
                voc->pend[voc->pend_have++] = (int16_t)((byte - 128) << 8);
            } else if ( voc->code == CODE_ADPCM4 ) {
                voc->pend[voc->pend_have++] = (int16_t)((adpcm_step( voc, byte >> 4, 4 ) - 128) << 8);
                voc->pend[voc->pend_have++] = (int16_t)((adpcm_step( voc, byte & 15, 4 ) - 128) << 8);
            } else if ( voc->code == CODE_ADPCM3 ) {
                voc->pend[voc->pend_have++] = (int16_t)((adpcm_step( voc, (byte >> 5) & 7, 3 ) - 128) << 8);
                voc->pend[voc->pend_have++] = (int16_t)((adpcm_step( voc, (byte >> 2) & 7, 3 ) - 128) << 8);
                voc->pend[voc->pend_have++] = (int16_t)((adpcm_step( voc, (byte & 3) << 1, 3 ) - 128) << 8);
            } else {
                for ( k = 6; k >= 0; k -= 2 ) {
                    voc->pend[voc->pend_have++] = (int16_t)((adpcm_step( voc, (byte >> k) & 3, 2 ) - 128) << 8);
                }
            }
            continue;
        default:
            for ( ch = 0; ch < voc->blk_channels; ch++ ) {
                switch ( voc->code ) {
                case CODE_PCM16:
                    byte = next_byte( voc );
                    byte2 = next_byte( voc );
                    val[ch] = byte2 < 0 ? 0 : (int16_t)(byte | (byte2 << 8));
                    break;
                case CODE_ALAW:
                    byte = next_byte( voc );
                    val[ch] = byte < 0 ? 0 : from_alaw( (uint8_t)byte );
                    break;
                case CODE_MULAW:
                    byte = next_byte( voc );
                    val[ch] = byte < 0 ? 0 : from_mulaw( (uint8_t)byte );
                    break;
                case CODE_PCM8:
                    byte = next_byte( voc );
                    val[ch] = byte < 0 ? 0 : ((int32_t)byte - 128) << 8;
                    break;
                default:
                    voc->left = 0;          /* a coding this does not know: the block is passed */
                    val[ch] = 0;
                    break;
                }
            }
            if ( voc->blk_channels == 1 ) {
                val[1] = val[0];
            }
            if ( voc->left == 0 && byte < 0 ) {
                continue;
            }
            break;
        }
        break;
    }
    if ( voc->out_ch == 1 ) {
        out[0] = (val[0] + val[1]) >> 1;
    } else {
        out[0] = val[0];
        out[1] = val[1];
    }
    return 1;
}

static int voc_decode( void *state, DECBUF *buf )
{
    VOCSTATE *voc = (VOCSTATE *)state;
    int16_t *pcm = buf->pcm;
    int32_t frame[2];
    unsigned int per = 1UL << voc->down, ch;

    buf->frames = 0;
    buf->ev_count = 0;
    while ( buf->frames < buf->max ) {
        if ( !next_frame( voc, frame ) ) {
            break;
        }
        for ( ch = 0; ch < voc->out_ch; ch++ ) {
            voc->acc[ch] += frame[ch];
        }
        if ( ++voc->acc_count < per ) {
            continue;
        }
        for ( ch = 0; ch < voc->out_ch; ch++ ) {
            *pcm++ = (int16_t)FX_SAT16( voc->acc[ch] >> voc->down );
            voc->acc[ch] = 0;
        }
        voc->acc_count = 0;
        buf->frames++;
    }
    return buf->frames == 0 && voc->src->error ? DE_READ : DE_OK;
}

/* ------------------------------------------------------------------ */
/* opening                                                             */
/* ------------------------------------------------------------------ */
static const char voc_sig[] = "Creative Voice File\x1A";

static int voc_detect( SRC *src, const uint8_t *head, unsigned int len, const char *ext )
{
    (void)src;
    (void)ext;
    if ( len >= 26 && mem_cmp( head, voc_sig, 20 ) == 0 ) {
        return 100;
    }
    return 0;
}

static void voc_close( void *state )
{
    VOCSTATE *voc = (VOCSTATE *)state;

    if ( voc != NULL ) {
        if ( voc->in != NULL ) {
            xfree( voc->in );
        }
        xfree( voc );
    }
}

/* the blocks from the start: the first one's format, and the frames in
   all of them for the length.  The file is left at the first block */
static int walk( VOCSTATE *voc, DECINFO *info )
{
    uint8_t hdr[16];
    uint32_t len, pos, frames = 0;
    unsigned int count;
    uint32_t rate_got = 0, chans = 1, ext_chans = 1, ext_rate = 0;
    uint16_t code;
    int type, bits = 8, ext = 0;

    src_seek( voc->src, voc->first_block );
    for ( count = 0; count < BLOCK_LIMIT; count++ ) {
        type = src_byte( voc->src );
        if ( type <= 0 || src_read( voc->src, hdr, 3 ) != 3 ) {
            break;
        }
        len = hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16);
        pos = src_tell( voc->src );
        if ( len > src_left( voc->src ) ) {
            len = src_left( voc->src );
        }
        if ( type == 1 && len >= 2 && src_read( voc->src, hdr, 2 ) == 2 ) {
            code = hdr[1];
            if ( !rate_got ) {
                chans = ext ? ext_chans : 1;
                voc->first_code = code;
                rate_got = ext ? ext_rate : (hdr[0] < 255 ? 1000000UL / (256 - hdr[0]) : 0);
                sfmt( info->format, sizeof( info->format ), "%s",
                      code == 1 ? "Creative Voice, 4-bit ADPCM" : code == 2 ? "Creative Voice, 2.6-bit ADPCM"
                      : code == 3 ? "Creative Voice, 2-bit ADPCM" : "Creative Voice, 8-bit PCM" );
                info->bits = (uint8_t)(code == 1 ? 4 : code == 2 ? 3 : code == 3 ? 2 : 8);
            }
            ext = 0;
            /* frames: the reference byte, then 2, 3 or 4 samples a byte */
            frames += code == 1 ? (len - 2) * 2 : code == 2 ? (len - 2) * 3 : code == 3 ? (len - 2) * 4
                      : (len - 2) / chans;
        } else if ( type == 2 ) {
            frames += len / chans;
        } else if ( type == 3 && len >= 3 && src_read( voc->src, hdr, 3 ) == 3 ) {
            frames += (uint32_t)LE16( hdr ) + 1;
        } else if ( type == 8 && len >= 4 && src_read( voc->src, hdr, 4 ) == 4 ) {
            ext = 1;
            ext_chans = hdr[3] ? 2 : 1;
            ext_rate = 65536UL - LE16( hdr ) ? 256000000UL / (65536UL - LE16( hdr )) / ext_chans : 0;
        } else if ( type == 9 && len >= 12 && src_read( voc->src, hdr, 12 ) == 12 ) {
            code = LE16( hdr + 6 );
            if ( !rate_got ) {
                rate_got = LE32( hdr );
                chans = hdr[5] == 2 ? 2 : 1;
                bits = hdr[4];
                sfmt( info->format, sizeof( info->format ), "Creative Voice, %s",
                      code == CODE_ALAW ? "A-law" : code == CODE_MULAW ? "mu-law"
                      : code == CODE_PCM16 || bits == 16 ? "16-bit PCM" : code == 0 ? "8-bit PCM" : "ADPCM" );
                info->bits = (uint8_t)(code == CODE_PCM16 || bits == 16 ? 16 : 8);
            }
            frames += (code == CODE_PCM16 || bits == 16 ? (len - 12) / 2 : len - 12) / chans;
        }
        src_seek( voc->src, pos + len );
    }
    voc->rate = rate_got;
    voc->channels = (uint8_t)chans;
    voc->frames_total = frames;
    src_seek( voc->src, voc->first_block );
    return rate_got >= 2000 && rate_got <= 96000UL;
}

static int voc_open( SRC *src, const DECOPT *opt, DECINFO *info, void **state )
{
    VOCSTATE *voc;
    uint8_t hdr[26];
    uint32_t hdr_size;

    *state = NULL;
    if ( src_read( src, hdr, 26 ) != 26 || mem_cmp( hdr, voc_sig, 20 ) != 0 ) {
        return DE_FORMAT;
    }
    hdr_size = LE16( hdr + 20 );
    if ( hdr_size < 26 || hdr_size > src->size ) {
        return DE_FORMAT;
    }
    voc = (VOCSTATE *)try_alloc( sizeof( VOCSTATE ) );
    if ( voc == NULL ) {
        return DE_NOMEM;
    }
    mem_set( voc, 0, sizeof( VOCSTATE ) );
    voc->src = src;
    voc->first_block = hdr_size;
    voc->in = (uint8_t *)try_alloc( IN_SIZE );
    if ( voc->in == NULL ) {
        voc_close( voc );
        return DE_NOMEM;
    }
    if ( !walk( voc, info ) ) {
        voc_close( voc );
        return DE_FORMAT;
    }
    if ( voc->first_code == CODE_ADPCM3 ) {
        voc_close( voc );
        return DE_UNSUPPORTED;          /* info->format says which */
    }
    voc->down = opt->down > 2 ? 2 : opt->down;
    while ( voc->down > 0 && (voc->rate >> voc->down) < 8000 ) {
        voc->down--;
    }
    voc->out_ch = (uint8_t)(voc->channels == 1 || opt->mono ? 1 : 2);
    info->file_rate = voc->rate;
    info->rate = voc->rate >> voc->down;
    info->file_channels = voc->channels;
    info->channels = voc->out_ch;
    info->flags = DIF_SEEK;
    info->length_ms = fx_umuldiv( voc->frames_total, 1000, voc->rate );
    /* a large file divided first, so the quotient fits 32 bits */
    if ( info->length_ms == 0 ) {
        info->kbps = 0;
    } else if ( src->size < 0x20000000UL ) {
        info->kbps = fx_umuldiv( src->size, 8, info->length_ms );
    } else {
        info->kbps = src->size / info->length_ms * 8;
    }
    *state = voc;
    return DE_OK;
}

static int voc_seek( void *state, uint32_t ms, uint32_t *at_ms )
{
    VOCSTATE *voc = (VOCSTATE *)state;
    uint32_t want, done = 0;
    int32_t frame[2];

    /* a time past what 32 bits of frames can say is the end anyway */
    if ( ms > 0xFFFFFFFFUL / voc->rate * 1000UL ) {
        ms = 0xFFFFFFFFUL / voc->rate * 1000UL;
    }
    want = fx_umuldiv( ms, voc->rate, 1000 );

    /* from the start, frame by frame: a VOC file is small, and its
       ADPCM has to be followed from a block's start anyway */
    src_seek( voc->src, voc->first_block );
    voc->left = voc->silence = 0;
    voc->in_len = voc->in_used = 0;
    voc->pend_have = voc->pend_used = 0;
    voc->rep_count = 0;
    voc->blocks = 0;
    voc->ext_pending = 0;
    voc->acc[0] = voc->acc[1] = 0;
    voc->acc_count = 0;
    while ( done < want && next_frame( voc, frame ) ) {
        done++;
    }
    *at_ms = fx_umuldiv( done, 1000, voc->rate );
    return DE_OK;
}

const CODEC codec_voc = {
    "Creative Voice",
    "VOC",
    voc_detect,
    voc_open,
    voc_decode,
    voc_seek,
    voc_close
};
