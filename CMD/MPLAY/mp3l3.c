/*
 * MP3L3.C - MPEG audio layer 3: side information, the bit reservoir,
 * scale factors, Huffman codes, requantization, stereo, and the
 * hybrid filter bank up to the subband samples (MP3.H).
 *
 * A FRAME'S DATA MAY START IN AN EARLIER FRAME.  The side information
 * says how many bytes back (main_data_begin), so the end of every
 * frame's data is kept in the reservoir: up to 511 bytes, the most the
 * field can say.  When the bytes asked for are not there - the first
 * frames after a seek, or after a damaged stretch - the frame plays as
 * silence and the next one carries on.
 *
 * REQUANTIZATION IS A TABLE LOOKUP.  A spectral value is |x|^(4/3)
 * times 2 to a power in quarter steps.  The power splits into whole
 * steps, which are a shift, and quarters, of which there are four;
 * x^(4/3) times each quarter is a table for x below 16, which is
 * nearly every value.  Larger ones (escape codes) take a multiply.
 *
 * STEREO IS DONE BY CHANGING THE POWER.  Mid/side needs a 1/sqrt(2):
 * that is two quarter steps less.  One channel out of two needs a half:
 * four quarter steps.  MPEG-2's intensity positions are powers of
 * 2^(1/4) too.  So in most bands stereo processing costs no multiply
 * at all, and a mono output from a mid/side band needs only the mid
 * channel requantized - the side channel is decoded (the bits must be
 * read) and then left alone.  Only MPEG-1's intensity ratios take a
 * multiply a line.
 *
 * ONE CHANNEL OUT IS ONE TRANSFORM.  When both channels have the same
 * block type the spectra are added before the inverse transform, which
 * then runs once.  When they differ, both are transformed and the
 * second is added into the first's output and overlap.
 *
 * NOTHING ABOVE THE LAST NONZERO LINE IS WORKED ON.  The Huffman
 * decoder says where the values stop; alias reduction and the inverse
 * transform run only that far (one subband further, where alias
 * reduction spills over), and the subbands above give only what the
 * last granule left in the overlap.  At half or quarter rate the lines
 * above the subbands kept are not even requantized.
 *
 * THE SPECTRUM AND THE TRANSFORM ARE Q26, two bits finer than the
 * subband samples they end as, and every multiply rounds (MP3.H).  With
 * that, every conformance stream comes out within one bit of the
 * reference; rounding down instead left a bias that made one sample in
 * fifty a bit low.
 *
 * MIXED BLOCKS GET NO ALIAS REDUCTION, not even between their two long
 * subbands: the conformance reference has none there.
 *
 * THE INVERSE MDCT is the 18-point DCT-IV the standard's formula comes
 * down to, split into two 9-point DCTs (20 multiplies each) and nine
 * scalings; the window, the sign of each output and the last scaling
 * are one table, so the window costs nothing extra.  Odd subbands have
 * every other sample turned over to undo the filter bank's frequency
 * inversion - that is in the tables too.  Short blocks are three
 * 6-point DCT-IVs the same way.
 *
 * TRUST NOTHING: big_values is held to 288 pairs, every granule's bits
 * end where part2_3_length says or where the data does, the Huffman
 * decoder stops at the first pair past that end, and a value read past
 * the data is a zero.
 */
#include "rt.h"
#include "mp3.h"

#define RES_KEEP        511             /* main_data_begin reaches back this far at most */
#define HUFF_SLACK      32              /* zeroed bytes behind the reservoir's data */
#define RES_SIZE        (RES_KEEP + MP3_MAXFRAME + HUFF_SLACK)
#define LONG_WIN        3               /* BAND.win of a long band */
#define XR_FRAC         26              /* the spectrum and the transform: Q26 */
#define TO_SB( val )    (((val) + 2) >> 2)  /* Q26 to the subband samples' Q24 */
#define MAX_BANDS       40

#define KIND_LONG       0
#define KIND_SHORT      1
#define KIND_MIXED      2

enum { ST_LR, ST_MS, ST_IS };

/* one granule of one channel, as the side information gives it */
typedef struct {
    uint16_t part23;                    /* part2_3_length: its bits */
    uint16_t big_values;                /* pairs, at most 288 */
    uint16_t gain;                      /* global_gain */
    uint16_t sfc;                       /* scalefac_compress */
    uint8_t  block_type;                /* 0 normal, 1 start, 2 short, 3 stop */
    uint8_t  mixed;                     /* the lowest two subbands take the normal long window */
    uint8_t  wsf;                       /* window_switching_flag */
    uint8_t  table[3];
    uint8_t  sbg[3];                    /* subblock_gain */
    uint8_t  region0, region1;
    uint8_t  preflag;
    uint8_t  sfscale;
    uint8_t  count1tab;
} GRAN;

/* a run of lines that shares a scale factor */
typedef struct {
    uint16_t start, end;                /* lines, in the order the Huffman code gives them */
    uint16_t freq;                      /* a short band: its first line within the window */
    uint8_t  sfb;
    uint8_t  win;                       /* 0..2 a short window, LONG_WIN a long band */
} BAND;

/* the requantization of one band: x^(4/3) * 2^(q/4) */
typedef struct {
    const int32_t *small;               /* x^(4/3) * 2^(f/4) for x < 16, Q25 */
    int32_t frac;                       /* 2^(f/4), Q30 */
    int     shift;                      /* the right shift of a small value: 0..45, 45 = 0 */
} RQ;

struct MP3L3 {
    uint32_t sfreq;
    uint8_t  lsf;                       /* MPEG-2 or 2.5: one granule, other scale factors */
    uint8_t  is_scale;                  /* MPEG-2 intensity_scale */
    uint8_t  ov_count[2];               /* subbands whose overlap may not be 0 */
    uint16_t mdb;                       /* main_data_begin */
    uint32_t res_len;
    uint8_t  res[RES_SIZE];
    int32_t  xr[2][576];                /* Huffman values, then the spectrum */
    int32_t  ov[2][32 * 18];            /* the overlap, by subband */
    int32_t  tmp[576];                  /* a short granule in subband order */
    uint8_t  scfsi[2][4];
    uint8_t  sfl[2][22];                /* long scale factors */
    uint8_t  sfs[2][13][3];             /* short scale factors */
    uint8_t  ismax_l[22];               /* MPEG-2: the intensity position that means "none" */
    uint8_t  ismax_s[13];
    GRAN     gr[2][2];
    BAND     band[3][MAX_BANDS];        /* long, short, mixed */
    int      nband[3];
    uint16_t map[2][576];               /* short and mixed: where each line goes in tmp */
    uint8_t  mode[MAX_BANDS];           /* the stereo plan of a granule */
    uint8_t  ispos[MAX_BANDS];
    uint32_t eq576[576];                /* the equalizer by long line, Q16 */
    uint32_t eq192[192];                /* and by short line */
};

static const uint8_t pretab[22] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0
};

/* MPEG-1 scalefac_compress: slen1 and slen2 */
static const uint8_t slen_mp1[2][16] = {
    { 0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4 },
    { 0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3 }
};

/* MPEG-2 (ISO 13818-3 table B.4, nr_of_sfb): how many scale factors
   each of the four slens covers, by table and by long, short, mixed */
static const uint8_t lsf_nsf[6][3][4] = {
    { {  6,  5,  5, 5 }, {  9,  9,  9, 9 }, {  6,  9,  9, 9 } },
    { {  6,  5,  7, 3 }, {  9,  9, 12, 6 }, {  6,  9, 12, 6 } },
    { { 11, 10,  0, 0 }, { 18, 18,  0, 0 }, { 15, 18,  0, 0 } },
    { {  7,  7,  7, 0 }, { 12, 12, 12, 0 }, {  6, 15, 12, 0 } },
    { {  6,  6,  6, 3 }, { 12,  9,  9, 6 }, {  6, 12,  9, 6 } },
    { {  8,  8,  5, 0 }, { 15, 12,  9, 0 }, {  6, 18,  9, 0 } }
};

static int band_kind( const GRAN *gr )
{
    if ( gr->block_type != 2 ) {
        return KIND_LONG;
    }
    return gr->mixed ? KIND_MIXED : KIND_SHORT;
}

/* ------------------------------------------------------------------ */
/* the bands of a rate                                                 */
/* ------------------------------------------------------------------ */
static void band_set( BAND *band, unsigned int start, unsigned int end, unsigned int freq, int sfb, int win )
{
    band->start = (uint16_t)start;
    band->end = (uint16_t)end;
    band->freq = (uint16_t)freq;
    band->sfb = (uint8_t)sfb;
    band->win = (uint8_t)win;
}

/* The bands of a granule in the order its lines come.  Mixed blocks
   are long up to line 36 (two subbands) and short above, where a short
   band that starts below a window's line 12 is cut there. */
static int make_bands( BAND *band, unsigned int sfreq, int kind )
{
    const uint16_t *sl = mp3_sfb_long + sfreq * 23;
    const uint16_t *ss = mp3_sfb_short + sfreq * 14;
    int count = 0, sfb, win;
    unsigned int line = 0, low, width;

    if ( kind == KIND_LONG ) {
        for ( sfb = 0; sfb < 22; sfb++ ) {
            band_set( &band[count++], sl[sfb], sl[sfb + 1], sl[sfb], sfb, LONG_WIN );
        }
        return count;
    }
    if ( kind == KIND_MIXED ) {
        for ( sfb = 0; sl[sfb + 1] <= 36; sfb++ ) {
            band_set( &band[count++], sl[sfb], sl[sfb + 1], sl[sfb], sfb, LONG_WIN );
        }
        line = sl[sfb];
    }
    for ( sfb = 0; sfb < 13 && count + 3 <= MAX_BANDS; sfb++ ) {
        low = ss[sfb];
        if ( kind == KIND_MIXED ) {
            if ( ss[sfb + 1] <= line / 3 ) {
                continue;
            }
            if ( low < line / 3 ) {
                low = line / 3;
            }
        }
        width = ss[sfb + 1] - low;
        for ( win = 0; win < 3; win++ ) {
            band_set( &band[count++], line, line + width, low, sfb, win );
            line += width;
        }
    }
    return count;
}

/* where each line of a short or mixed granule goes: subband, window,
   line within the window */
static void make_map( uint16_t *map, const BAND *band, int count )
{
    int bnd;
    unsigned int i, freq;

    for ( bnd = 0; bnd < count; bnd++ ) {
        for ( i = band[bnd].start; i < band[bnd].end && i < 576; i++ ) {
            if ( band[bnd].win == LONG_WIN ) {
                map[i] = (uint16_t)i;
            } else {
                freq = band[bnd].freq + (i - band[bnd].start);
                map[i] = (uint16_t)((freq / 6) * 18 + band[bnd].win * 6 + freq % 6);
            }
        }
    }
}

int mp3_l3_open( MP3DEC *dec, unsigned int sfreq )
{
    MP3L3 *l3;
    int kind;

    if ( sfreq > 8 ) {
        return DE_FORMAT;
    }
    l3 = (MP3L3 *)try_alloc( sizeof( MP3L3 ) );
    if ( l3 == NULL ) {
        return DE_NOMEM;
    }
    mem_set( l3, 0, sizeof( MP3L3 ) );
    l3->sfreq = sfreq;
    l3->lsf = (uint8_t)(sfreq >= 3);
    for ( kind = 0; kind < 3; kind++ ) {
        l3->nband[kind] = make_bands( l3->band[kind], sfreq, kind );
    }
    make_map( l3->map[0], l3->band[KIND_SHORT], l3->nband[KIND_SHORT] );
    make_map( l3->map[1], l3->band[KIND_MIXED], l3->nband[KIND_MIXED] );
    dec->l3 = l3;
    return DE_OK;
}

void mp3_l3_close( MP3DEC *dec )
{
    if ( dec->l3 != NULL ) {
        xfree( dec->l3 );
        dec->l3 = NULL;
    }
}

void mp3_l3_reset( MP3DEC *dec )
{
    MP3L3 *l3 = dec->l3;

    if ( l3 != NULL ) {
        l3->res_len = 0;
        mem_set( l3->ov, 0, sizeof( l3->ov ) );
        l3->ov_count[0] = l3->ov_count[1] = 0;
    }
}

void mp3_l3_eq( MP3DEC *dec, const EQSET *eq, uint32_t rate )
{
    if ( dec->l3 != NULL ) {
        eq_curve( eq, rate, 576, dec->l3->eq576 );
        eq_curve( eq, rate, 192, dec->l3->eq192 );
    }
}

/* ------------------------------------------------------------------ */
/* side information                                                    */
/* ------------------------------------------------------------------ */
/* -> its bytes, or -1 if the frame is shorter */
static int read_side( MP3L3 *l3, const MP3HDR *hdr, const uint8_t *data, unsigned int len )
{
    MP3BITS bits;
    GRAN *gr;
    int nch = hdr->channels, ngr = l3->lsf ? 1 : 2, size, gran, ch, i;

    if ( l3->lsf ) {
        size = nch == 1 ? 9 : 17;
    } else {
        size = nch == 1 ? 17 : 32;
    }
    if ( len < (unsigned int)size ) {
        return -1;
    }
    mp3_bits_init( &bits, data, (unsigned int)size, 0 );
    if ( l3->lsf ) {
        l3->mdb = (uint16_t)mp3_bits_get( &bits, 8 );
        mp3_bits_get( &bits, nch == 1 ? 1 : 2 );
        mem_set( l3->scfsi, 0, sizeof( l3->scfsi ) );
    } else {
        l3->mdb = (uint16_t)mp3_bits_get( &bits, 9 );
        mp3_bits_get( &bits, nch == 1 ? 5 : 3 );
        for ( ch = 0; ch < nch; ch++ ) {
            for ( i = 0; i < 4; i++ ) {
                l3->scfsi[ch][i] = (uint8_t)mp3_bits_get( &bits, 1 );
            }
        }
    }
    for ( gran = 0; gran < ngr; gran++ ) {
        for ( ch = 0; ch < nch; ch++ ) {
            gr = &l3->gr[gran][ch];
            gr->part23 = (uint16_t)mp3_bits_get( &bits, 12 );
            gr->big_values = (uint16_t)mp3_bits_get( &bits, 9 );
            if ( gr->big_values > 288 ) {
                gr->big_values = 288;
            }
            gr->gain = (uint16_t)mp3_bits_get( &bits, 8 );
            gr->sfc = (uint16_t)mp3_bits_get( &bits, l3->lsf ? 9 : 4 );
            gr->wsf = (uint8_t)mp3_bits_get( &bits, 1 );
            if ( gr->wsf ) {
                gr->block_type = (uint8_t)mp3_bits_get( &bits, 2 );
                gr->mixed = (uint8_t)mp3_bits_get( &bits, 1 );
                gr->table[0] = (uint8_t)mp3_bits_get( &bits, 5 );
                gr->table[1] = (uint8_t)mp3_bits_get( &bits, 5 );
                gr->table[2] = 0;
                for ( i = 0; i < 3; i++ ) {
                    gr->sbg[i] = (uint8_t)mp3_bits_get( &bits, 3 );
                }
                gr->region0 = 7;
                gr->region1 = 13;
            } else {
                gr->block_type = 0;
                gr->mixed = 0;
                for ( i = 0; i < 3; i++ ) {
                    gr->table[i] = (uint8_t)mp3_bits_get( &bits, 5 );
                    gr->sbg[i] = 0;
                }
                gr->region0 = (uint8_t)mp3_bits_get( &bits, 4 );
                gr->region1 = (uint8_t)mp3_bits_get( &bits, 3 );
            }
            gr->preflag = (uint8_t)(l3->lsf ? 0 : mp3_bits_get( &bits, 1 ));
            gr->sfscale = (uint8_t)mp3_bits_get( &bits, 1 );
            gr->count1tab = (uint8_t)mp3_bits_get( &bits, 1 );
        }
    }
    return size;
}

/* ------------------------------------------------------------------ */
/* scale factors                                                       */
/* ------------------------------------------------------------------ */
static void scale_mpeg1( MP3L3 *l3, MP3BITS *bits, const GRAN *gr, int ch, int granule )
{
    static const uint8_t group[5] = { 0, 6, 11, 16, 21 };
    int slen1 = slen_mp1[0][gr->sfc & 15], slen2 = slen_mp1[1][gr->sfc & 15];
    int sfb, win, part;

    if ( gr->block_type == 2 ) {
        sfb = 0;
        if ( gr->mixed ) {
            for ( sfb = 0; sfb < 8; sfb++ ) {
                l3->sfl[ch][sfb] = (uint8_t)mp3_bits_get( bits, slen1 );
            }
            sfb = 3;
        }
        for ( ; sfb < 12; sfb++ ) {
            for ( win = 0; win < 3; win++ ) {
                l3->sfs[ch][sfb][win] = (uint8_t)mp3_bits_get( bits, sfb < 6 ? slen1 : slen2 );
            }
        }
        for ( win = 0; win < 3; win++ ) {
            l3->sfs[ch][12][win] = 0;
        }
        return;
    }
    for ( part = 0; part < 4; part++ ) {
        if ( granule == 1 && l3->scfsi[ch][part] ) {
            continue;                       /* the same as the first granule's */
        }
        for ( sfb = group[part]; sfb < group[part + 1]; sfb++ ) {
            l3->sfl[ch][sfb] = (uint8_t)mp3_bits_get( bits, part < 2 ? slen1 : slen2 );
        }
    }
    l3->sfl[ch][21] = 0;
}

/* MPEG-2: four parts of the scale factors, each with its own size.  The
   right channel of an intensity stereo frame has other part sizes, and
   its values are intensity positions. */
static void scale_mpeg2( MP3L3 *l3, MP3BITS *bits, GRAN *gr, int ch, int is_right )
{
    uint32_t sfc = gr->sfc, rest;
    int slen[4], tab, kind, part, k, slot = 0, sfb, win, val, max;

    slen[2] = slen[3] = 0;
    if ( !is_right ) {
        if ( sfc < 400 ) {
            slen[0] = (int)((sfc >> 4) / 5);
            slen[1] = (int)((sfc >> 4) % 5);
            slen[2] = (int)((sfc & 15) >> 2);
            slen[3] = (int)(sfc & 3);
            tab = 0;
        } else if ( sfc < 500 ) {
            rest = sfc - 400;
            slen[0] = (int)((rest >> 2) / 5);
            slen[1] = (int)((rest >> 2) % 5);
            slen[2] = (int)(rest & 3);
            tab = 1;
        } else {
            rest = sfc - 500;
            slen[0] = (int)(rest / 3);
            slen[1] = (int)(rest % 3);
            tab = 2;
            gr->preflag = 1;
        }
    } else {
        rest = sfc >> 1;
        l3->is_scale = (uint8_t)(sfc & 1);
        if ( rest < 180 ) {
            slen[0] = (int)(rest / 36);
            slen[1] = (int)((rest % 36) / 6);
            slen[2] = (int)((rest % 36) % 6);
            tab = 3;
        } else if ( rest < 244 ) {
            rest -= 180;
            slen[0] = (int)((rest % 64) >> 4);
            slen[1] = (int)((rest % 16) >> 2);
            slen[2] = (int)(rest % 4);
            tab = 4;
        } else {
            rest -= 244;
            slen[0] = (int)(rest / 3);
            slen[1] = (int)(rest % 3);
            tab = 5;
        }
    }
    kind = band_kind( gr );
    mem_set( l3->sfl[ch], 0, sizeof( l3->sfl[ch] ) );
    mem_set( l3->sfs[ch], 0, sizeof( l3->sfs[ch] ) );
    for ( part = 0; part < 4; part++ ) {
        max = (1 << slen[part]) - 1;
        for ( k = 0; k < lsf_nsf[tab][kind][part]; k++, slot++ ) {
            val = (int)mp3_bits_get( bits, slen[part] );
            if ( kind == KIND_LONG || (kind == KIND_MIXED && slot < 6) ) {
                sfb = slot;
                if ( sfb < 22 ) {
                    l3->sfl[ch][sfb] = (uint8_t)val;
                    if ( is_right ) {
                        l3->ismax_l[sfb] = (uint8_t)max;
                    }
                }
            } else {
                sfb = kind == KIND_MIXED ? 3 + (slot - 6) / 3 : slot / 3;
                win = kind == KIND_MIXED ? (slot - 6) % 3 : slot % 3;
                if ( sfb < 13 ) {
                    l3->sfs[ch][sfb][win] = (uint8_t)val;
                    if ( is_right ) {
                        l3->ismax_s[sfb] = (uint8_t)max;
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Huffman decoding                                                    */
/* ------------------------------------------------------------------ */
/* the next bytes of the main data into the cache.  There is no test for
   the end of the data here: the loops stop soon after it, and the
   reservoir has HUFF_SLACK zeroed bytes behind its data for them */
#define FILL        while ( count <= 24 ) { \
                        cache |= (uint32_t)*at++ << (24 - count); \
                        count += 8; \
                    }

/* The granule's values into xr[0..575]: big values in pairs from up to
   three tables, then fours of 0 and 1 until the bits run out, or until
   line "stop" (at half and quarter rate the lines above are not heard,
   and the next granule starts where part2_3_length says).  -> how
   many lines may be nonzero.  Written for registers: this runs once
   for every pair of spectral lines. */
static unsigned int huffman( const MP3BITS *bits, unsigned int end, const GRAN *gr, unsigned int sfreq, int32_t *xr,
                             unsigned int stop )
{
    const uint16_t *sl = mp3_sfb_long + sfreq * 23;
    const uint8_t *at = bits->data + bits->pos, *beyond;
    const uint16_t *tab;
    const MP3HTAB *ht;
    int32_t *out = xr, *upto;
    uint32_t cache = bits->cache;
    unsigned int bigv = (unsigned int)gr->big_values * 2, reg1, reg2, limit;
    uint32_t entry, val;
    int count = bits->count, region, nbits, rs, lin, xval, yval, mask, damaged = 0;

    if ( gr->wsf ) {
        reg1 = gr->block_type == 2 ? 3 * (unsigned int)mp3_sfb_short[sfreq * 14 + 3] : sl[8];
        reg2 = 576;
    } else {
        region = gr->region0 + 1;
        reg1 = sl[region > 22 ? 22 : region];
        region = gr->region0 + gr->region1 + 2;
        reg2 = sl[region > 22 ? 22 : region];
    }
    if ( reg1 > bigv ) {
        reg1 = bigv;
    }
    if ( reg2 > bigv ) {
        reg2 = bigv;
    }
    /* past this byte the granule's bits have run out for certain */
    beyond = bits->data + ((end + 32) >> 3);
    for ( region = 0; region < 3 && !damaged; region++ ) {
        limit = region == 0 ? reg1 : (region == 1 ? reg2 : bigv);
        if ( limit > stop ) {
            limit = stop;
        }
        upto = xr + limit;
        ht = &mp3_htab[gr->table[region]];
        if ( ht->bits == 0 ) {
            while ( out < upto ) {
                *out++ = 0;
            }
            continue;
        }
        tab = mp3_huff + ht->first;
        rs = 32 - ht->bits;
        lin = ht->linbits;
        while ( out < upto ) {
            FILL
            entry = tab[cache >> rs];
            if ( entry & 0x8000 ) {
                cache <<= 32 - rs;
                count -= 32 - rs;
                for ( ;; ) {
                    nbits = (int)((entry >> 12) & 7);
                    entry = tab[(entry & 0x0FFF) + (cache >> (32 - nbits))];
                    if ( !(entry & 0x8000) ) {
                        break;
                    }
                    cache <<= nbits;
                    count -= nbits;
                }
            }
            nbits = (int)((entry >> 8) & 15);
            cache <<= nbits;
            count -= nbits;
            xval = (int)((entry >> 4) & 15);
            yval = (int)(entry & 15);
            /* a sign bit after each value that is not 0, after its linbits */
            if ( xval != 0 ) {
                if ( xval == 15 && lin ) {
                    FILL
                    xval += (int)(cache >> (32 - lin));
                    cache <<= lin;
                    count -= lin;
                }
                mask = (int)((int32_t)cache >> 31);
                xval = (xval ^ mask) - mask;
                cache <<= 1;
                count--;
            }
            if ( yval != 0 ) {
                if ( yval == 15 && lin ) {
                    FILL
                    yval += (int)(cache >> (32 - lin));
                    cache <<= lin;
                    count -= lin;
                }
                mask = (int)((int32_t)cache >> 31);
                yval = (yval ^ mask) - mask;
                cache <<= 1;
                count--;
            }
            out[0] = xval;
            out[1] = yval;
            out += 2;
            if ( at > beyond ) {
                damaged = 1;                /* the pairs ran past the granule */
                break;
            }
        }
    }
    if ( (unsigned int)(at - bits->data) * 8 - (unsigned int)count > end ) {
        damaged = 1;
    }
    /* count1: four values of 0 or 1 at a time */
    upto = xr + (stop < 576 ? stop : 576);
    while ( !damaged && out < upto && out <= xr + 572
            && (unsigned int)(at - bits->data) * 8 - (unsigned int)count < end ) {
        FILL
        if ( gr->count1tab ) {
            val = (~cache >> 28) & 15;
            nbits = 4;
        } else {
            entry = mp3_quad_a[cache >> 26];
            val = entry & 15;
            nbits = (int)(entry >> 4);
        }
        cache <<= nbits;
        count -= nbits;
        for ( region = 0; region < 4; region++ ) {
            if ( val & (8 >> region) ) {
                out[region] = (cache & 0x80000000UL) ? -1 : 1;
                cache <<= 1;
                count--;
            } else {
                out[region] = 0;
            }
        }
        if ( (unsigned int)(at - bits->data) * 8 - (unsigned int)count > end ) {
            break;                          /* a code cut off by the end: not one */
        }
        out += 4;
    }
    limit = (unsigned int)(out - xr);
    mem_set( out, 0, (576 - limit) * sizeof( int32_t ) );
    while ( limit > 0 && xr[limit - 1] == 0 ) {
        limit--;
    }
    return limit;
}

/* ------------------------------------------------------------------ */
/* requantization                                                      */
/* ------------------------------------------------------------------ */
static void rq_make( RQ *rq, int power )
{
    int whole, part;

    whole = power >= 0 ? power / 4 : -((3 - power) / 4);    /* floor( power / 4 ) */
    part = power - whole * 4;
    rq->small = mp3_pow_small + 16 * part;
    rq->frac = mp3_frac4[part];
    rq->shift = (25 - XR_FRAC) - whole;
    if ( rq->shift < 0 ) {
        rq->shift = 0;                      /* louder than full scale: held there */
    } else if ( rq->shift > 45 ) {
        rq->shift = 45;
    }
}

/* a value of 16 or more: x^(4/3) * 2^(f/4) in Q11, then shifted to Q26
   and held below 16 */
static int32_t rq_big( const RQ *rq, int32_t mag )
{
    int32_t val = fx_mulhi( (int32_t)mp3_pow43[mag], rq->frac );
    int left = 14 - rq->shift;

    if ( left >= 0 ) {
        return val >= (0x3FFFFFFFL >> left) ? 0x3FFFFFFFL : val << left;
    }
    left = -left;
    return left > 30 ? 0 : val >> left;
}

static int32_t rq_value( const RQ *rq, int32_t val )
{
    int32_t mag = val < 0 ? -val : val, res;

    if ( mag == 0 ) {
        return 0;
    }
    if ( mag < 16 ) {
        res = rq->shift < 31 ? rq->small[mag] >> rq->shift : 0;
    } else {
        res = rq_big( rq, mag );
    }
    return val < 0 ? -res : res;
}

/* One band: dst = the values of src requantized at power q, or added
   to dst.  Without add, src is dst.  The two loops are written apart and
   walk pointers, so that the table and the shift stay in registers: this
   runs once for every spectral line. */
static void rq_band( int32_t *dst, const int32_t *src, unsigned int start, unsigned int end, int power, int add )
{
    RQ rq;
    const int32_t *small, *at, *stop;
    int32_t *to;
    int32_t val;
    int shift;

    rq_make( &rq, power );
    to = dst + start;
    if ( rq.shift >= 45 ) {
        if ( !add ) {
            mem_set( to, 0, (end - start) * sizeof( int32_t ) );
        }
        return;
    }
    small = rq.small;
    shift = rq.shift < 31 ? rq.shift : 31;
    at = src + start;
    stop = src + end;
    if ( !add ) {
        for ( ; at < stop; at++, to++ ) {
            val = *at;
            if ( val > 0 ) {
                *to = val < 16 ? small[val] >> shift : rq_big( &rq, val );
            } else if ( val < 0 ) {
                *to = val > -16 ? -(small[-val] >> shift) : -rq_big( &rq, -val );
            }
        }
        return;
    }
    for ( ; at < stop; at++, to++ ) {
        val = *at;
        if ( val > 0 ) {
            *to += val < 16 ? small[val] >> shift : rq_big( &rq, val );
        } else if ( val < 0 ) {
            *to -= val > -16 ? small[-val] >> shift : rq_big( &rq, -val );
        }
    }
}

/* the power of a band: global gain, subblock gain, scale factor */
static int scale_q( const MP3L3 *l3, const GRAN *gr, int ch, const BAND *band )
{
    int power = (int)gr->gain - 210, sf;

    if ( band->win == LONG_WIN ) {
        sf = l3->sfl[ch][band->sfb];
        if ( gr->preflag ) {
            sf += pretab[band->sfb];
        }
    } else {
        sf = l3->sfs[ch][band->sfb][band->win];
        power -= 8 * gr->sbg[band->win];
    }
    return power - (sf << (1 + gr->sfscale));
}

/* every band of one channel below "top", the power moved by "extra" */
static void rq_channel( MP3L3 *l3, const GRAN *gr, int ch, unsigned int top, int extra )
{
    const BAND *band = l3->band[band_kind( gr )];
    int count = l3->nband[band_kind( gr )], bnd;

    for ( bnd = 0; bnd < count && band[bnd].start < top; bnd++ ) {
        rq_band( l3->xr[ch], l3->xr[ch], band[bnd].start, band[bnd].end,
                 scale_q( l3, gr, ch, &band[bnd] ) + extra, 0 );
    }
}

/* ------------------------------------------------------------------ */
/* stereo                                                              */
/* ------------------------------------------------------------------ */
/* MPEG-2 intensity: the left and right powers below the line's, in
   quarter steps */
static void lsf_is( const MP3L3 *l3, int pos, int *left, int *right )
{
    int step = l3->is_scale ? 2 : 1;

    *left = *right = 0;
    if ( pos & 1 ) {
        *left = ((pos + 1) / 2) * step;
    } else {
        *right = (pos / 2) * step;
    }
}

/* Which bands are intensity stereo: those above the last nonzero value
   of the right channel (in a short granule, of the right channel's
   window), whose position is not the one that means "none" */
static void make_plan( MP3L3 *l3, const BAND *band, int count, unsigned int nz1, int ms, int is )
{
    const int32_t *right = l3->xr[1];
    int bnd, seen_any = 0, seen[3], nonzero, sfb, pos, none;
    unsigned int i, stop;

    seen[0] = seen[1] = seen[2] = 0;
    for ( bnd = count - 1; bnd >= 0; bnd-- ) {
        l3->mode[bnd] = (uint8_t)(ms ? ST_MS : ST_LR);
        if ( !is ) {
            continue;
        }
        nonzero = 0;
        if ( band[bnd].start < nz1 ) {
            stop = band[bnd].end < nz1 ? band[bnd].end : nz1;
            for ( i = band[bnd].start; i < stop; i++ ) {
                if ( right[i] != 0 ) {
                    nonzero = 1;
                    break;
                }
            }
        }
        if ( band[bnd].win == LONG_WIN ) {
            none = nonzero || seen_any;
        } else {
            none = nonzero || seen[band[bnd].win];
        }
        if ( nonzero ) {
            seen_any = 1;
            if ( band[bnd].win != LONG_WIN ) {
                seen[band[bnd].win] = 1;
            }
        }
        if ( none ) {
            continue;
        }
        /* the top band has no scale factor of its own: it takes the one below */
        if ( band[bnd].win == LONG_WIN ) {
            sfb = band[bnd].sfb >= 21 ? 20 : band[bnd].sfb;
            pos = l3->sfl[1][sfb];
            none = l3->lsf ? pos == l3->ismax_l[sfb] : pos >= 7;
        } else {
            sfb = band[bnd].sfb >= 12 ? 11 : band[bnd].sfb;
            pos = l3->sfs[1][sfb][band[bnd].win];
            none = l3->lsf ? pos == l3->ismax_s[sfb] : pos >= 7;
        }
        if ( !none ) {
            l3->mode[bnd] = ST_IS;
            l3->ispos[bnd] = (uint8_t)pos;
        }
    }
}

/* Requantize and do the stereo of one granule.  -> the lines that may
   be nonzero; *both is 1 when xr[1] is a channel of its own to
   transform (stereo out, or one channel out of two whose block types
   differ) */
static unsigned int do_stereo( MP3DEC *dec, MP3L3 *l3, int gran, const unsigned int *nz, int *both )
{
    const MP3HDR *hdr = &dec->hdr;
    const GRAN *gr0 = &l3->gr[gran][0], *gr1 = &l3->gr[gran][1];
    const BAND *band;
    int32_t *xr0 = l3->xr[0], *xr1 = l3->xr[1];
    unsigned int lim = dec->nsb == 32 ? 576 : 18 * (unsigned int)dec->nsb + 18, top, i, start, end;
    int count, bnd, kind, ms = 0, is = 0, extra, q0, q1, dl, dr, combine;
    int32_t mid, side, val, part;
    RQ rql, rqr;

    *both = 0;
    if ( hdr->channels == 1 ) {
        top = nz[0] < lim ? nz[0] : lim;
        rq_channel( l3, gr0, 0, top, 0 );
        return top;
    }
    top = nz[0] > nz[1] ? nz[0] : nz[1];
    if ( top > lim ) {
        top = lim;
    }
    if ( hdr->mode == MODE_JOINT ) {
        ms = (hdr->mode_ext & 2) != 0;
        is = (hdr->mode_ext & 1) != 0;
    }
    kind = band_kind( gr0 );
    if ( kind != band_kind( gr1 ) ) {
        /* the channels' bands do not line up: each by its own bands, mid/side
           line by line, no intensity stereo */
        extra = (ms ? -2 : 0) + (dec->out_ch == 1 ? -4 : 0);
        rq_channel( l3, gr0, 0, top, extra );
        rq_channel( l3, gr1, 1, top, extra );
        if ( ms ) {
            for ( i = 0; i < top; i++ ) {
                mid = xr0[i];
                side = xr1[i];
                xr0[i] = mid + side;
                xr1[i] = mid - side;
            }
        }
        *both = 1;
        return top;
    }
    band = l3->band[kind];
    count = l3->nband[kind];
    make_plan( l3, band, count, nz[1], ms, is );
    combine = dec->out_ch == 1 && gr0->block_type == gr1->block_type && gr0->mixed == gr1->mixed;
    extra = dec->out_ch == 1 ? -4 : 0;
    for ( bnd = 0; bnd < count && band[bnd].start < top; bnd++ ) {
        start = band[bnd].start;
        end = band[bnd].end;
        q0 = scale_q( l3, gr0, 0, &band[bnd] );
        q1 = scale_q( l3, gr1, 1, &band[bnd] );
        if ( combine ) {
            /* one channel: (left + right) / 2 in the spectrum */
            if ( l3->mode[bnd] == ST_LR ) {
                rq_band( xr0, xr0, start, end, q0 - 4, 0 );
                rq_band( xr0, xr1, start, end, q1 - 4, 1 );
            } else if ( l3->mode[bnd] == ST_MS ) {
                rq_band( xr0, xr0, start, end, q0 - 2, 0 );     /* the mid channel is the sum */
            } else if ( !l3->lsf ) {
                rq_band( xr0, xr0, start, end, q0 - 4, 0 );     /* the two shares add to one */
            } else {
                lsf_is( l3, l3->ispos[bnd], &dl, &dr );
                rq_make( &rql, q0 - dl - 4 );
                rq_make( &rqr, q0 - dr - 4 );
                for ( i = start; i < end; i++ ) {
                    val = xr0[i];
                    xr0[i] = rq_value( &rql, val ) + rq_value( &rqr, val );
                }
            }
            continue;
        }
        if ( l3->mode[bnd] == ST_LR ) {
            rq_band( xr0, xr0, start, end, q0 + extra, 0 );
            rq_band( xr1, xr1, start, end, q1 + extra, 0 );
        } else if ( l3->mode[bnd] == ST_MS ) {
            rq_band( xr0, xr0, start, end, q0 - 2 + extra, 0 );
            rq_band( xr1, xr1, start, end, q1 - 2 + extra, 0 );
            for ( i = start; i < end; i++ ) {
                mid = xr0[i];
                side = xr1[i];
                xr0[i] = mid + side;
                xr1[i] = mid - side;
            }
        } else if ( !l3->lsf ) {
            rq_band( xr0, xr0, start, end, q0 + extra, 0 );
            part = mp3_is_ratio[l3->ispos[bnd] < 7 ? l3->ispos[bnd] : 6];
            for ( i = start; i < end; i++ ) {
                val = xr0[i];
                xr0[i] = mp3_mul30( part, val );
                xr1[i] = val - xr0[i];
            }
        } else {
            lsf_is( l3, l3->ispos[bnd], &dl, &dr );
            rq_make( &rql, q0 - dl + extra );
            rq_make( &rqr, q0 - dr + extra );
            for ( i = start; i < end; i++ ) {
                val = xr0[i];
                xr0[i] = rq_value( &rql, val );
                xr1[i] = rq_value( &rqr, val );
            }
        }
    }
    *both = !combine;
    return top;
}

/* the equalizer: every line times its gain */
static void do_eq( MP3L3 *l3, const GRAN *gr, int32_t *xr, unsigned int top )
{
    const BAND *band = l3->band[band_kind( gr )];
    const uint32_t *gain;
    int count = l3->nband[band_kind( gr )], bnd;
    unsigned int i;

    for ( bnd = 0; bnd < count && band[bnd].start < top; bnd++ ) {
        gain = band[bnd].win == LONG_WIN ? l3->eq576 + band[bnd].start : l3->eq192 + band[bnd].freq;
        for ( i = band[bnd].start; i < band[bnd].end; i++ ) {
            if ( xr[i] != 0 ) {
                xr[i] = mp3_mul16( xr[i], (int32_t)gain[i - band[bnd].start] );
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* the hybrid filter bank                                              */
/* ------------------------------------------------------------------ */
/* alias reduction across the first "bounds" subband edges */
static void alias( int32_t *xr, int bounds )
{
    const int32_t *coef;
    int32_t *lo, *hi;
    int32_t low, high, sum;
    int sb, i;

    for ( sb = 1; sb <= bounds; sb++ ) {
        lo = xr + 18 * sb - 1;
        hi = xr + 18 * sb;
        coef = mp3_alias;
        for ( i = 0; i < 8; i++, coef += 3 ) {
            low = lo[-i];
            high = hi[i];
            if ( (low | high) != 0 ) {
                sum = mp3_mul30( coef[0], low + high );
                lo[-i] = sum - mp3_mul30( coef[1], high );
                hi[i] = sum + mp3_mul30( coef[2], low );
            }
        }
    }
}

/* out[i] = sum of z[m] cos(pi (2i + 1) m / 18), i and m 0..8 */
static void dct9( const int32_t *in, int32_t *out, int fast )
{
    const int32_t *cs = mp3_c9;          /* cos 10, 20, 40, 50, 70, 80, 30 degrees */
    int32_t t3, base, ev1, ev3, ev5, ev7, od1, od3, od5, od7;

    t3 = mp3_mul31( cs[6], in[3] );
    base = in[0] + (in[6] >> 1);
    ev3 = in[0] + (in[2] >> 1) - (in[4] >> 1) - in[6] - (in[8] >> 1);
    od3 = mp3_mul31( cs[6], in[1] - in[5] - in[7] );
    if ( fast ) {
        /* cos 20 = cos 40 + cos 80 and cos 10 = cos 50 + cos 70, so each
           three-term sum takes two multiplies: 14 in all.  The 1 / (2 cos)
           scalings after this DCT magnify its rounding up to eleven times,
           so full quality keeps the plain way */
        ev1 = base + mp3_mul31( cs[2], in[2] + in[4] ) + mp3_mul31( cs[5], in[2] + in[8] );
        ev5 = base + mp3_mul31( cs[2], in[8] - in[4] ) - mp3_mul31( cs[5], in[2] + in[4] );
        ev7 = base - mp3_mul31( cs[2], in[2] + in[8] ) + mp3_mul31( cs[5], in[4] - in[8] );
        od1 = mp3_mul31( cs[3], in[1] + in[5] ) + mp3_mul31( cs[4], in[1] + in[7] ) + t3;
        od5 = mp3_mul31( cs[3], in[1] + in[7] ) + mp3_mul31( cs[4], in[7] - in[5] ) - t3;
        od7 = mp3_mul31( cs[4], in[1] + in[5] ) + mp3_mul31( cs[3], in[5] - in[7] ) - t3;
    } else {
        ev1 = base + mp3_mul31( cs[1], in[2] ) + mp3_mul31( cs[2], in[4] ) + mp3_mul31( cs[5], in[8] );
        ev5 = base - mp3_mul31( cs[5], in[2] ) - mp3_mul31( cs[1], in[4] ) + mp3_mul31( cs[2], in[8] );
        ev7 = base - mp3_mul31( cs[2], in[2] ) + mp3_mul31( cs[5], in[4] ) - mp3_mul31( cs[1], in[8] );
        od1 = mp3_mul31( cs[0], in[1] ) + t3 + mp3_mul31( cs[3], in[5] ) + mp3_mul31( cs[4], in[7] );
        od5 = mp3_mul31( cs[3], in[1] ) - t3 - mp3_mul31( cs[4], in[5] ) + mp3_mul31( cs[0], in[7] );
        od7 = mp3_mul31( cs[4], in[1] ) - t3 + mp3_mul31( cs[0], in[5] ) - mp3_mul31( cs[3], in[7] );
    }
    out[0] = ev1 + od1;
    out[8] = ev1 - od1;
    out[1] = ev3 + od3;
    out[7] = ev3 - od3;
    out[2] = ev5 + od5;
    out[6] = ev5 - od5;
    out[3] = ev7 + od7;
    out[5] = ev7 - od7;
    out[4] = in[0] - in[2] + in[4] - in[6] + in[8];
}

/* 18 lines into 36 windowed samples: the first 18 onto the overlap and
   out (every 32nd int32_t), the last 18 the new overlap.  add: onto out and
   the overlap as they are.  fast: the 9-point DCTs with fewer multiplies */
static void imdct36( const int32_t *in, int32_t *ov, int32_t *out, const int32_t *win, int add, int fast )
{
    int32_t even[9], odd[9], ev[9], od[9], cv[18], part, low, high, next0, next1;
    int i;

    even[0] = in[0];
    odd[0] = in[1] + in[0];
    for ( i = 1; i < 9; i++ ) {
        even[i] = in[2 * i] + in[2 * i - 1];
        odd[i] = in[2 * i + 1] + in[2 * i];
    }
    for ( i = 8; i > 0; i-- ) {
        odd[i] += odd[i - 1];
    }
    dct9( even, ev, fast );
    dct9( odd, od, fast );
    for ( i = 0; i < 9; i++ ) {
        part = mp3_mul28( mp3_q9[i], od[i] );
        cv[i] = ev[i] + part;
        cv[17 - i] = ev[i] - part;
    }
    for ( i = 0; i < 9; i++ ) {
        low = mp3_mul27( win[i], cv[9 + i] );
        high = mp3_mul27( win[17 - i], cv[9 + i] );
        next0 = mp3_mul27( win[18 + i], cv[8 - i] );
        next1 = mp3_mul27( win[35 - i], cv[8 - i] );
        if ( add ) {
            out[i * 32] += TO_SB( low );
            out[(17 - i) * 32] += TO_SB( high );
            ov[i] += next0;
            ov[17 - i] += next1;
        } else {
            out[i * 32] = TO_SB( low + ov[i] );
            out[(17 - i) * 32] = TO_SB( high + ov[17 - i] );
            ov[i] = next0;
            ov[17 - i] = next1;
        }
    }
}

/* one short window: 6 lines into 12 windowed samples */
static void imdct12_win( const int32_t *in, const int32_t *win, int32_t *out )
{
    int32_t z1, z2, z3, z4, z5, t2, t3, e0, e1, e2, o0, o1, o2, cv[6];
    int slot;

    z1 = in[1] + in[0];
    z2 = in[2] + in[1];
    z3 = in[3] + in[2];
    z4 = in[4] + in[3];
    z5 = in[5] + in[4];
    t2 = mp3_mul31( mp3_c6[0], z2 );
    e0 = in[0] + t2 + (z4 >> 1);
    e1 = in[0] - z4;
    e2 = in[0] - t2 + (z4 >> 1);
    t3 = mp3_mul31( mp3_c6[2], z3 );
    o0 = mp3_mul31( mp3_c6[1], z1 ) + t3 + mp3_mul31( mp3_c6[3], z5 );
    o1 = mp3_mul31( mp3_c6[2], z1 - z3 - z5 );
    o2 = mp3_mul31( mp3_c6[3], z1 ) - t3 + mp3_mul31( mp3_c6[1], z5 );
    cv[0] = e0 + o0;
    cv[5] = e0 - o0;
    cv[1] = e1 + o1;
    cv[4] = e1 - o1;
    cv[2] = e2 + o2;
    cv[3] = e2 - o2;
    for ( slot = 0; slot < 3; slot++ ) {
        out[slot] = mp3_mul28( win[slot], cv[slot + 3] );
        out[9 + slot] = mp3_mul28( win[9 + slot], cv[slot] );
    }
    for ( slot = 3; slot < 9; slot++ ) {
        out[slot] = mp3_mul28( win[slot], cv[8 - slot] );
    }
}

/* three short windows, overlapped at 6, 12 and 18 within the 36 */
static void imdct12( const int32_t *in, int32_t *ov, int32_t *out, int parity, int add )
{
    const int32_t *win = mp3_cws + parity * 12;
    int32_t win0[12], win1[12], win2[12], val;
    int slot;

    imdct12_win( in, win, win0 );
    imdct12_win( in + 6, win, win1 );
    imdct12_win( in + 12, win, win2 );
    for ( slot = 0; slot < 18; slot++ ) {
        if ( slot < 6 ) {
            val = 0;
        } else if ( slot < 12 ) {
            val = win0[slot - 6];
        } else {
            val = win0[slot - 6] + win1[slot - 12];
        }
        if ( add ) {
            out[slot * 32] += TO_SB( val );
        } else {
            out[slot * 32] = TO_SB( val + ov[slot] );
        }
    }
    for ( slot = 0; slot < 18; slot++ ) {
        if ( slot < 6 ) {
            val = win1[slot + 6] + win2[slot];
        } else if ( slot < 12 ) {
            val = win2[slot];
        } else {
            val = 0;
        }
        if ( add ) {
            ov[slot] += val;
        } else {
            ov[slot] = val;
        }
    }
}

/* One channel's spectrum (nonzero below "top") into 18 slots of
   subband samples, with the overlap "ov".  add: onto what another
   channel has put there. */
static void hybrid( MP3DEC *dec, MP3L3 *l3, int32_t *xr, const GRAN *gr, unsigned int top, int32_t *ov,
                    uint8_t *ov_count, int32_t *out, int add )
{
    const uint16_t *map;
    int32_t *data = xr, *ovs, *dst;
    unsigned int i, dest, high = 0, sbn, nsb = dec->nsb;
    int sb, slot, bt;

    sbn = (top + 17) / 18;
    if ( gr->block_type == 2 ) {
        /* short blocks into subband order */
        map = l3->map[gr->mixed ? 1 : 0];
        mem_set( l3->tmp, 0, 18 * nsb * sizeof( int32_t ) );
        for ( i = 0; i < top; i++ ) {
            dest = map[i];
            l3->tmp[dest] = xr[i];
            if ( dest > high ) {
                high = dest;
            }
        }
        sbn = top > 0 ? high / 18 + 1 : 0;
        data = l3->tmp;
        /* no alias reduction, not even between the two long subbands of a
           mixed block: the conformance streams' reference output has none
           (he_mode.bit matches to the last bit only without it) */
    } else if ( sbn > 0 ) {
        alias( data, (int)(sbn < nsb ? sbn : (nsb < 32 ? nsb : 31)) );
        if ( sbn < 32 ) {
            sbn++;
        }
    }
    if ( sbn > nsb ) {
        sbn = nsb;
    }
    for ( sb = 0; sb < (int)nsb; sb++ ) {
        ovs = ov + sb * 18;
        dst = out + sb;
        if ( (unsigned int)sb < sbn ) {
            if ( gr->block_type == 2 && !(gr->mixed && sb < 2) ) {
                imdct12( data + sb * 18, ovs, dst, sb & 1, add );
            } else {
                bt = gr->block_type == 2 || (gr->mixed && sb < 2) ? 0 : gr->block_type;
                imdct36( data + sb * 18, ovs, dst, mp3_cwl + (bt * 2 + (sb & 1)) * 36, add, dec->fast );
            }
        } else if ( add ) {
            break;                          /* nothing to add */
        } else if ( sb < *ov_count ) {
            for ( slot = 0; slot < 18; slot++ ) {
                dst[slot * 32] = TO_SB( ovs[slot] );
                ovs[slot] = 0;
            }
        } else {
            for ( slot = 0; slot < 18; slot++ ) {
                dst[slot * 32] = 0;
            }
        }
    }
    if ( !add || sbn > *ov_count ) {
        *ov_count = (uint8_t)sbn;
    }
}

/* ------------------------------------------------------------------ */
/* a frame                                                             */
/* ------------------------------------------------------------------ */
void mp3_layer3( MP3DEC *dec, const uint8_t *frame, unsigned int len )
{
    MP3L3 *l3 = dec->l3;
    const MP3HDR *hdr = &dec->hdr;
    MP3BITS bits;
    GRAN *gr;
    unsigned int head = 4 + (hdr->crc ? 2 : 0), mdlen = 0, start = 0, avail, at = 0, end, top, stop;
    unsigned int nz[2];
    int side = -1, ngr = l3->lsf ? 1 : 2, nch = hdr->channels, gran, ch, ok, both;
    int32_t *out;

    if ( len >= head ) {
        side = read_side( l3, hdr, frame + head, len - head );
    }
    ok = side >= 0;
    if ( ok ) {
        mdlen = len - head - (unsigned int)side;
        if ( mdlen > MP3_MAXFRAME ) {
            mdlen = MP3_MAXFRAME;
        }
    }
    /* the reservoir: what may still be needed, then this frame's data */
    if ( l3->res_len > RES_KEEP ) {
        mem_cpy( l3->res, l3->res + l3->res_len - RES_KEEP, RES_KEEP );
        l3->res_len = RES_KEEP;
    }
    if ( ok && l3->mdb > l3->res_len ) {
        ok = 0;                             /* its data began in a frame not here */
    }
    if ( ok ) {
        start = l3->res_len - l3->mdb;
    }
    if ( mdlen > 0 ) {
        mem_cpy( l3->res + l3->res_len, frame + head + (unsigned int)side, mdlen );
        l3->res_len += mdlen;
    }
    mem_set( l3->res + l3->res_len, 0, HUFF_SLACK );    /* what the Huffman reads may run into */
    avail = (l3->res_len - start) * 8;
    dec->slots = 18 * (unsigned int)ngr;
    for ( gran = 0; gran < ngr; gran++ ) {
        nz[0] = nz[1] = 0;
        for ( ch = 0; ch < nch && ok; ch++ ) {
            gr = &l3->gr[gran][ch];
            end = at + gr->part23;
            if ( end > avail ) {
                end = avail;
            }
            mp3_bits_init( &bits, l3->res + start, l3->res_len - start, at );
            if ( l3->lsf ) {
                scale_mpeg2( l3, &bits, gr, ch,
                             ch == 1 && hdr->mode == MODE_JOINT && (hdr->mode_ext & 1) );
            } else {
                scale_mpeg1( l3, &bits, gr, ch, gran );
            }
            if ( MP3_BITPOS( &bits ) <= end ) {
                /* the lines heard at this rate, but all of the right channel
                   when intensity stereo needs to know where it ends */
                stop = dec->nsb == 32 || (ch == 1 && hdr->mode == MODE_JOINT && (hdr->mode_ext & 1))
                       ? 576 : 18 * (unsigned int)dec->nsb + 18;
                nz[ch] = huffman( &bits, end, gr, l3->sfreq, l3->xr[ch], stop );
            } else {
                mem_set( l3->xr[ch], 0, sizeof( l3->xr[ch] ) );     /* damaged: silence */
            }
            at += gr->part23;
        }
        top = do_stereo( dec, l3, gran, nz, &both );
        if ( dec->eq_on && top > 0 ) {
            do_eq( l3, &l3->gr[gran][0], l3->xr[0], top );
            if ( both ) {
                do_eq( l3, &l3->gr[gran][1], l3->xr[1], top );
            }
        }
        out = dec->sb[0] + gran * 18 * 32;
        hybrid( dec, l3, l3->xr[0], &l3->gr[gran][0], top, l3->ov[0], &l3->ov_count[0], out, 0 );
        if ( both && dec->out_ch == 2 ) {
            hybrid( dec, l3, l3->xr[1], &l3->gr[gran][1], top, l3->ov[1], &l3->ov_count[1],
                    dec->sb[1] + gran * 18 * 32, 0 );
        } else if ( both ) {
            hybrid( dec, l3, l3->xr[1], &l3->gr[gran][1], top, l3->ov[0], &l3->ov_count[0], out, 1 );
        }
    }
}
