/*
 * MP3L12.C - MPEG audio layers 1 and 2: bit allocation, scale factors
 * and samples, into subband samples for the synthesis (MP3.H).
 *
 * THE TWO LAYERS ARE ONE IDEA.  For each subband the frame says how
 * many levels its samples are quantized to (the allocation) and by how
 * much they are scaled (a scale factor, 2^(1 - i/3)); then come the
 * samples themselves, plain numbers of a few bits.  Layer 1 has 12
 * samples a subband and one scale factor; layer 2 has 36, three scale
 * factors that may be shared (scfsi says how), allocation tables that
 * depend on the rate, and the 3-, 5- and 9-level quantizations pack
 * three samples into one code.
 *
 * A SAMPLE IS ONE MULTIPLY.  The standard's dequantization comes down
 * to (2c + 1 - levels) / levels times the scale factor.  The scale
 * factor over the levels is made once for each subband and part of the
 * frame (a fixed-point number normalized to keep its precision), and
 * every sample is the small odd integer 2c + 1 - levels times that.
 * The equalizer's gain for the subband and the half for a mono mix are
 * folded into the same number.
 *
 * INTENSITY STEREO above the bound means one set of samples for both
 * channels, each with its own scale factors.  Mixed to one channel the
 * two scale factors add, so it costs a single multiply.
 *
 * THE ALLOCATION TABLES are ISO 11172-3 tables B.2a to B.2d and ISO
 * 13818-3 table B.1, given here as the list of quantizations each
 * allocation value means.
 */
#include "rt.h"
#include "mp3.h"

/* the 17 quantizations: levels, and bits a sample (or three grouped) */
static const uint32_t q_levels[17] = {
    3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535
};
static const uint8_t q_bits[17] = {
    5, 7, 3, 10, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
};

/* what each allocation value from 1 on means, as a quantization */
static const uint8_t q_a[15] = { 0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint8_t q_b[15] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16 };
static const uint8_t q_c[7] = { 0, 1, 2, 3, 4, 5, 16 };
static const uint8_t q_d[3] = { 0, 1, 16 };
static const uint8_t q_e[15] = { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static const uint8_t q_f[7] = { 0, 1, 3, 4, 5, 6, 7 };
static const uint8_t q_g[15] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
static const uint8_t q_h[3] = { 0, 1, 3 };

/* a run of subbands with the same allocation: from "first" on */
typedef struct {
    uint8_t first;
    uint8_t nbal;                       /* bits of the allocation value */
    const uint8_t *quant;
} RANGE;

static const RANGE tab_a[] = {          /* B.2a: 27 subbands */
    { 0, 4, q_a }, { 3, 4, q_b }, { 11, 3, q_c }, { 23, 2, q_d }, { 27, 0, NULL }
};
static const RANGE tab_b[] = {          /* B.2b: 30 subbands */
    { 0, 4, q_a }, { 3, 4, q_b }, { 11, 3, q_c }, { 23, 2, q_d }, { 30, 0, NULL }
};
static const RANGE tab_c[] = {          /* B.2c: 8 subbands */
    { 0, 4, q_e }, { 2, 3, q_f }, { 8, 0, NULL }
};
static const RANGE tab_d[] = {          /* B.2d: 12 subbands */
    { 0, 4, q_e }, { 2, 3, q_f }, { 12, 0, NULL }
};
static const RANGE tab_lsf[] = {        /* 13818-3 B.1: 30 subbands */
    { 0, 4, q_g }, { 4, 3, q_f }, { 11, 2, q_h }, { 30, 0, NULL }
};

/* The scale factor (an index 0..63) over the levels of quantization
   "quant", with the subband's equalizer gain, halved when two channels
   are mixed into one: the number each sample's odd integer is
   multiplied by (and then shifted by mp3_l12_shift[quant]) */
static int32_t factor( const MP3DEC *dec, int index, int quant, int sb, int half )
{
    int32_t val = fx_mulhi( mp3_l12_sf[index & 63], mp3_l12_inv[quant] );

    if ( dec->eq_on ) {
        val = mp3_mul16( val, (int32_t)dec->eq32[sb] );
    }
    return half ? val >> 1 : val;
}

static void clear_sb( MP3DEC *dec, unsigned int slots )
{
    unsigned int ch;

    for ( ch = 0; ch < dec->out_ch; ch++ ) {
        mem_set( dec->sb[ch], 0, slots * MP3_SB * sizeof( int32_t ) );
    }
    dec->slots = slots;
}

/* the bound of intensity stereo: subbands from here on share samples */
static unsigned int stereo_bound( const MP3HDR *hdr, unsigned int limit )
{
    unsigned int bound = limit;

    if ( hdr->mode == MODE_JOINT ) {
        bound = 4 + 4 * (unsigned int)hdr->mode_ext;
    }
    return bound < limit ? bound : limit;
}

/* ------------------------------------------------------------------ */
/* layer 1                                                             */
/* ------------------------------------------------------------------ */
int mp3_layer1( MP3DEC *dec, const uint8_t *frame, unsigned int len )
{
    const MP3HDR *hdr = &dec->hdr;
    MP3BITS bits;
    uint8_t alloc[2][32], quant[2][32];
    int32_t fac[2][32];
    unsigned int head = 4 + (hdr->crc ? 2 : 0), nch = hdr->channels, bound, sb, ch, slot, nb;
    uint32_t code;
    unsigned int nsb = dec->nsb;
    int mix = nch == 2 && dec->out_ch == 1;
    int32_t odd, *out0, *out1;

    clear_sb( dec, 12 );
    if ( len < head ) {
        return -1;
    }
    mp3_bits_init( &bits, frame + head, len - head, 0 );
    bound = stereo_bound( hdr, 32 );
    for ( sb = 0; sb < 32; sb++ ) {
        for ( ch = 0; ch < nch; ch++ ) {
            if ( sb < bound || ch == 0 ) {
                alloc[ch][sb] = (uint8_t)mp3_bits_get( &bits, 4 );
                if ( alloc[ch][sb] == 15 ) {
                    alloc[ch][sb] = 0;          /* not allowed */
                }
            } else {
                alloc[ch][sb] = alloc[0][sb];
            }
            /* nb bits a sample: 2^nb - 1 levels */
            nb = (unsigned int)alloc[ch][sb] + 1;
            quant[ch][sb] = (uint8_t)(nb == 2 ? 0 : (nb == 3 ? 2 : (nb == 4 ? 4 : nb)));
        }
    }
    for ( sb = 0; sb < 32; sb++ ) {
        for ( ch = 0; ch < nch; ch++ ) {
            if ( alloc[ch][sb] != 0 ) {
                fac[ch][sb] = factor( dec, (int)mp3_bits_get( &bits, 6 ), quant[ch][sb], (int)sb, mix );
            } else {
                fac[ch][sb] = 0;
            }
        }
    }
    for ( slot = 0; slot < 12; slot++ ) {
        out0 = dec->sb[0] + slot * MP3_SB;
        out1 = dec->out_ch == 2 ? dec->sb[1] + slot * MP3_SB : out0;
        for ( sb = 0; sb < 32; sb++ ) {
            for ( ch = 0; ch < nch; ch++ ) {
                if ( alloc[ch][sb] == 0 || (sb >= bound && ch == 1) ) {
                    continue;
                }
                nb = (unsigned int)alloc[ch][sb] + 1;
                code =mp3_bits_get( &bits, (int)nb );
                if ( sb >= nsb ) {
                    continue;                   /* read, but not heard at this rate */
                }
                odd = (int32_t)(2 * code + 2) - (int32_t)(1UL << nb);
                if ( sb >= bound && nch == 2 ) {
                    /* one sample, both channels' scale factors */
                    if ( mix ) {
                        out0[sb] = fx_mulsh( fac[0][sb] + fac[1][sb], odd, mp3_l12_shift[quant[0][sb]] );
                    } else {
                        out0[sb] = fx_mulsh( fac[0][sb], odd, mp3_l12_shift[quant[0][sb]] );
                        out1[sb] = fx_mulsh( fac[1][sb], odd, mp3_l12_shift[quant[0][sb]] );
                    }
                } else if ( mix ) {
                    out0[sb] += fx_mulsh( fac[ch][sb], odd, mp3_l12_shift[quant[ch][sb]] );
                } else {
                    (ch == 0 ? out0 : out1)[sb] = fx_mulsh( fac[ch][sb], odd,
                                                             mp3_l12_shift[quant[ch][sb]] );
                }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* layer 2                                                             */
/* ------------------------------------------------------------------ */
/* the allocation table of a frame */
static const RANGE *l2_table( const MP3HDR *hdr )
{
    uint32_t per_ch;

    if ( hdr->version != MV_1 ) {
        return tab_lsf;
    }
    if ( hdr->br_index == 0 ) {
        per_ch = 999;                           /* free format: the high-rate tables */
    } else {
        per_ch = hdr->channels == 1 ? hdr->kbps : hdr->kbps / 2;
    }
    if ( per_ch <= 48 ) {
        return hdr->rate == 32000 ? tab_d : tab_c;
    }
    if ( per_ch <= 80 || hdr->rate == 48000 ) {
        return tab_a;
    }
    return tab_b;
}

/* three samples of one quantization into vals[0..2] as 2c + 1 - levels */
static void l2_samples( MP3BITS *bits, int quant, int32_t *vals )
{
    const int8_t *group;
    uint32_t code, levels = q_levels[quant];
    int k;

    if ( quant == 0 || quant == 1 || quant == 3 ) {
        code = mp3_bits_get( bits, q_bits[quant] );
        if ( quant == 0 ) {
            group = mp3_group3 + code * 3;
        } else if ( quant == 1 ) {
            group = mp3_group5 + code * 3;
        } else {
            group = mp3_group9 + code * 3;
        }
        vals[0] = group[0];
        vals[1] = group[1];
        vals[2] = group[2];
        return;
    }
    for ( k = 0; k < 3; k++ ) {
        code = mp3_bits_get( bits, q_bits[quant] );
        vals[k] = (int32_t)(2 * code + 1) - (int32_t)levels;
    }
}

int mp3_layer2( MP3DEC *dec, const uint8_t *frame, unsigned int len )
{
    const MP3HDR *hdr = &dec->hdr;
    const RANGE *table, *range;
    MP3BITS bits;
    uint8_t nbal[32], alloc[2][32], scfsi[2][32], sfi[2][32][3], quant[2][32];
    const uint8_t *qlist[32];
    int32_t fac[2][32], vals[2][3], *out0, *out1;
    unsigned int head = 4 + (hdr->crc ? 2 : 0), nch = hdr->channels, limit, bound, sb, ch, gr, part, k;
    unsigned int nsb = dec->nsb, slot;
    int mix = nch == 2 && dec->out_ch == 1, shift;

    clear_sb( dec, 36 );
    if ( len < head ) {
        return -1;
    }
    mp3_bits_init( &bits, frame + head, len - head, 0 );
    table = l2_table( hdr );
    range = table;
    for ( sb = 0; range->nbal != 0 && sb < 32; sb++ ) {
        if ( range[1].first <= sb ) {
            range++;
            if ( range->nbal == 0 ) {
                break;
            }
        }
        nbal[sb] = range->nbal;
        qlist[sb] = range->quant;
    }
    limit = sb;
    bound = stereo_bound( hdr, limit );

    /* allocation, then which scale factors are shared, then the scale factors */
    for ( sb = 0; sb < limit; sb++ ) {
        for ( ch = 0; ch < nch; ch++ ) {
            if ( sb < bound || ch == 0 ) {
                alloc[ch][sb] = (uint8_t)mp3_bits_get( &bits, nbal[sb] );
            } else {
                alloc[ch][sb] = alloc[0][sb];
            }
            quant[ch][sb] = (uint8_t)(alloc[ch][sb] ? qlist[sb][alloc[ch][sb] - 1] : 0);
        }
    }
    for ( sb = 0; sb < limit; sb++ ) {
        for ( ch = 0; ch < nch; ch++ ) {
            scfsi[ch][sb] = (uint8_t)(alloc[ch][sb] ? mp3_bits_get( &bits, 2 ) : 0);
        }
    }
    for ( sb = 0; sb < limit; sb++ ) {
        for ( ch = 0; ch < nch; ch++ ) {
            if ( alloc[ch][sb] == 0 ) {
                continue;
            }
            sfi[ch][sb][0] = (uint8_t)mp3_bits_get( &bits, 6 );
            switch ( scfsi[ch][sb] ) {
            case 0:
                sfi[ch][sb][1] = (uint8_t)mp3_bits_get( &bits, 6 );
                sfi[ch][sb][2] = (uint8_t)mp3_bits_get( &bits, 6 );
                break;
            case 1:
                sfi[ch][sb][1] = sfi[ch][sb][0];
                sfi[ch][sb][2] = (uint8_t)mp3_bits_get( &bits, 6 );
                break;
            case 2:
                sfi[ch][sb][1] = sfi[ch][sb][2] = sfi[ch][sb][0];
                break;
            default:
                sfi[ch][sb][1] = (uint8_t)mp3_bits_get( &bits, 6 );
                sfi[ch][sb][2] = sfi[ch][sb][1];
                break;
            }
        }
    }

    /* twelve granules of three samples; the scale factor changes every four */
    for ( gr = 0; gr < 12; gr++ ) {
        part = gr / 4;
        if ( (gr & 3) == 0 ) {
            for ( sb = 0; sb < limit && sb < nsb; sb++ ) {
                for ( ch = 0; ch < nch; ch++ ) {
                    fac[ch][sb] = alloc[ch][sb]
                                  ? factor( dec, sfi[ch][sb][part], quant[ch][sb], (int)sb, mix ) : 0;
                }
            }
        }
        for ( sb = 0; sb < limit; sb++ ) {
            for ( ch = 0; ch < nch; ch++ ) {
                if ( alloc[ch][sb] == 0 || (sb >= bound && ch == 1) ) {
                    continue;
                }
                l2_samples( &bits, quant[ch][sb], vals[ch] );
                if ( sb >= nsb ) {
                    continue;
                }
                shift = mp3_l12_shift[quant[ch][sb]];
                for ( k = 0; k < 3; k++ ) {
                    slot = gr * 3 + k;
                    out0 = dec->sb[0] + slot * MP3_SB;
                    out1 = dec->out_ch == 2 ? dec->sb[1] + slot * MP3_SB : out0;
                    if ( sb >= bound && nch == 2 ) {
                        if ( mix ) {
                            out0[sb] = fx_mulsh( fac[0][sb] + fac[1][sb], vals[0][k], shift );
                        } else {
                            out0[sb] = fx_mulsh( fac[0][sb], vals[0][k], shift );
                            out1[sb] = fx_mulsh( fac[1][sb], vals[0][k], shift );
                        }
                    } else if ( mix ) {
                        out0[sb] += fx_mulsh( fac[ch][sb], vals[ch][k], shift );
                    } else {
                        (ch == 0 ? out0 : out1)[sb] = fx_mulsh( fac[ch][sb], vals[ch][k], shift );
                    }
                }
            }
        }
    }
    return 0;
}
