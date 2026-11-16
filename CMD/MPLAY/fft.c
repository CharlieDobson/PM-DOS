/*
 * FFT.C - the spectrum analyzer: half a second's last 512 samples
 * turned into bars (FFT.H).
 *
 * A 512-POINT FFT TEN TIMES A SECOND IS NOTHING EVEN ON A 386: nine
 * passes of 256 butterflies, four multiplies each, is under ten
 * thousand multiplies a picture.  So this is the plain radix-2 kind,
 * in 32-bit integers with Q30 twiddles made by CORDIC once; the
 * samples are 16 bits and nine passes add at most nine bits, so
 * nothing needs scaling between the passes.
 *
 * A HANN WINDOW FIRST, or the edges of the 512 samples would spread
 * every tone across the whole picture.
 *
 * THE LOUDNESS OF A BIN without a square root: the larger of the two
 * parts plus three eighths of the smaller is within four per cent of
 * the true length, which is far less than a bar's step.  Then decibels
 * by the position of the top bit and a small table for the bits under
 * it, sixty decibels from the top of a 16-bit sample to the bottom of
 * the bar.
 *
 * THE BARS ARE SPACED BY OCTAVES, as an ear hears pitch, so a low bar
 * may cover less than one bin: it then takes the nearest bin's value,
 * which is why the left end of an analyzer always moves in steps.
 */
#include "rt.h"
#include "fix.h"
#include "trig.h"
#include "fft.h"

#define FFT_BITS        9

static s32 *tw_cos, *tw_sin;            /* FFT_SIZE / 2 each, Q30 */
static s16 *hann;                       /* FFT_SIZE, Q15 */
static s32 *re, *im;
static u16 *rev;                        /* bit-reversed indexes */
static int ready;

/* 20 log10( 1 + i / 16 ) in sixteenths of a decibel, for the four bits
   under the top one; made with [Math] in PowerShell */
static const u8 db_frac[17] = {
     0,  8, 16, 24, 31, 38, 44, 50, 56, 62, 67, 73, 78, 83, 87, 92, 96
};

/* log2( 1 + i / 32 ) and 2^( i / 16 ), both in Q16 */
static const u32 log2_frac[33] = {
         0,  2909,  5732,  8473, 11136, 13727, 16248, 18704,
     21098, 23433, 25711, 27936, 30109, 32234, 34312, 36346,
     38336, 40286, 42196, 44068, 45904, 47705, 49472, 51207,
     52911, 54584, 56229, 57845, 59434, 60997, 62534, 64047,
     65536
};
static const u32 exp2_frac[17] = {
     65536,  68438,  71468,  74632,  77936,  81386,  84990,  88752,
     92682,  96785, 101070, 105545, 110218, 115098, 120194, 125515,
    131072
};

/* log2( x ) in Q16, x > 0 */
static u32 log2_q16( u32 x )
{
    int top = fx_topbit( x );
    u32 mant, index, part;

    mant = top < 31 ? x << (31 - top) : x;
    mant <<= 1;
    index = mant >> 27;
    part = (mant >> 11) & 0xFFFF;
    return ((u32)top << 16) + log2_frac[index]
           + (((log2_frac[index + 1] - log2_frac[index]) * part) >> 16);
}

/* 2^( e / 65536 ) in Q16, e below 10 octaves */
static u32 exp2_q16( u32 e )
{
    u32 whole = e >> 16, index = (e >> 12) & 15, part = e & 0xFFF, val;

    val = exp2_frac[index] + (((exp2_frac[index + 1] - exp2_frac[index]) * part) >> 12);
    return whole < 10 ? val << whole : val << 10;
}

/* the edge "n" of "count" bars from 40 Hz to top: 40 * (top / 40)^(n / count) */
static u32 edge_hz( u32 n, u32 count, u32 top )
{
    u32 octaves = log2_q16( top ) - log2_q16( 40 );

    return (40 * exp2_q16( fx_umuldiv( octaves, n, count ) )) >> 16;
}

/* the tables given back: at the end, and when not all of them could be
   had (the next picture tries again, from nothing) */
void fft_done( void )
{
    if ( tw_cos != NULL ) xfree( tw_cos );
    if ( tw_sin != NULL ) xfree( tw_sin );
    if ( hann != NULL ) xfree( hann );
    if ( re != NULL ) xfree( re );
    if ( im != NULL ) xfree( im );
    if ( rev != NULL ) xfree( rev );
    tw_cos = tw_sin = re = im = NULL;
    hann = NULL;
    rev = NULL;
    ready = 0;
}

static int make_tables( void )
{
    u32 i, j, bits;
    s32 sinv, cosv, val;

    tw_cos = (s32 *)try_alloc( FFT_SIZE / 2 * sizeof( s32 ) );
    tw_sin = (s32 *)try_alloc( FFT_SIZE / 2 * sizeof( s32 ) );
    hann = (s16 *)try_alloc( FFT_SIZE * sizeof( s16 ) );
    re = (s32 *)try_alloc( FFT_SIZE * sizeof( s32 ) );
    im = (s32 *)try_alloc( FFT_SIZE * sizeof( s32 ) );
    rev = (u16 *)try_alloc( FFT_SIZE * sizeof( u16 ) );
    if ( tw_cos == NULL || tw_sin == NULL || hann == NULL || re == NULL || im == NULL || rev == NULL ) {
        fft_done();
        return 0;
    }
    for ( i = 0; i < FFT_SIZE / 2; i++ ) {
        fx_sincos( i << (32 - FFT_BITS), &sinv, &cosv );
        tw_cos[i] = cosv;
        tw_sin[i] = sinv;
    }
    for ( i = 0; i < FFT_SIZE; i++ ) {
        /* 0.5 - 0.5 cos( 2 pi i / N ), from the same twiddles */
        cosv = i < FFT_SIZE / 2 ? tw_cos[i] : -tw_cos[i - FFT_SIZE / 2];
        /* halved first: CORDIC's cos( 0 ) may come out a little over
           1.0, and 1.0 - (-1.0) would not fit; the top is 32767 */
        val = (0x20000000L - (cosv >> 1)) >> 15;
        hann[i] = (s16)(val > 32767 ? 32767 : val < 0 ? 0 : val);
        j = 0;
        for ( bits = 0; bits < FFT_BITS; bits++ ) {
            j |= ((i >> bits) & 1) << (FFT_BITS - 1 - bits);
        }
        rev[i] = (u16)j;
    }
    ready = 1;
    return 1;
}

/* the level of a length, 0..255 over sixty decibels below a full-scale
   sine's bin (whose length is about 32767 * N / 4) */
static u8 level_of( u32 len )
{
    int top, step;
    u32 frac;
    s32 db16;

    if ( len < 2 ) {
        return 0;
    }
    top = fx_topbit( len );
    frac = top >= 4 ? (len >> (top - 4)) & 15 : (len << (4 - top)) & 15;
    /* 20 log10( 2^top ) = 6.02 dB a bit, in sixteenths: 96.3 a bit */
    db16 = (s32)top * 963 / 10 + db_frac[frac];
    /* full scale: log2( 32767 * 512 / 4 ) = 22, so 22 bits is the top */
    db16 -= 22 * 963 / 10;
    step = (int)((db16 + 60 * 16) * 255 / (60 * 16));
    return (u8)(step < 0 ? 0 : step > 255 ? 255 : step);
}

int fft_bars( const s16 *samples, u32 rate, u8 *bars, int count )
{
    u32 i, j, k, half, span, step, top_hz, lo_bin, hi_bin, bin;
    s32 tr, ti, wr, wi, ar, ai;
    u32 mag, best, big, small;
    u32 lo_hz, hi_hz;
    int bar;

    if ( !ready && !make_tables() ) {
        mem_set( bars, 0, (u32)count );
        return 0;
    }
    for ( i = 0; i < FFT_SIZE; i++ ) {
        re[rev[i]] = ((s32)samples[i] * hann[i]) >> 15;
        im[rev[i]] = 0;
    }
    for ( span = 1; span < FFT_SIZE; span <<= 1 ) {
        step = FFT_SIZE / 2 / span;
        for ( k = 0; k < span; k++ ) {
            wr = tw_cos[k * step];
            wi = -tw_sin[k * step];
            for ( i = k; i < FFT_SIZE; i += span * 2 ) {
                j = i + span;
                ar = re[j];
                ai = im[j];
                tr = fx_mul30( ar, wr ) - fx_mul30( ai, wi );
                ti = fx_mul30( ar, wi ) + fx_mul30( ai, wr );
                re[j] = re[i] - tr;
                im[j] = im[i] - ti;
                re[i] += tr;
                im[i] += ti;
            }
        }
    }
    /* the bars: 40 Hz to the top, a fixed ratio from each to the next */
    half = FFT_SIZE / 2;
    top_hz = rate / 2 < 16000 ? rate / 2 : 16000;
    if ( top_hz <= 40 || count <= 0 ) {
        mem_set( bars, 0, (u32)(count > 0 ? count : 0) );
        return 1;
    }
    /* the length of every bin, once (re[] takes them: it is not needed
       again) */
    for ( bin = 0; bin < half; bin++ ) {
        big = (u32)(re[bin] < 0 ? -re[bin] : re[bin]);
        small = (u32)(im[bin] < 0 ? -im[bin] : im[bin]);
        if ( small > big ) {
            mag = big;
            big = small;
            small = mag;
        }
        re[bin] = (s32)(big + (small >> 2) + (small >> 3));
    }
    for ( bar = 0; bar < count; bar++ ) {
        /* the bar's edges, a fixed ratio apart */
        lo_hz = edge_hz( (u32)bar, (u32)count, top_hz );
        hi_hz = edge_hz( (u32)bar + 1, (u32)count, top_hz );
        lo_bin = fx_umuldiv( lo_hz, FFT_SIZE, rate );
        hi_bin = fx_umuldiv( hi_hz, FFT_SIZE, rate );
        if ( hi_bin >= half ) {
            hi_bin = half - 1;
        }
        if ( hi_bin > lo_bin && lo_bin >= 1 ) {
            /* a bar wider than a bin: its loudest bin */
            best = 0;
            for ( bin = lo_bin; bin <= hi_bin; bin++ ) {
                if ( (u32)re[bin] > best ) {
                    best = (u32)re[bin];
                }
            }
        } else {
            /* narrower than a bin, as the low bars are: the length at the
               bar's middle, between the two bins either side of it, so
               neighboring bars rise and fall in a slope and not in a
               block */
            mag = fx_umuldiv( (lo_hz + hi_hz) / 2, FFT_SIZE * 256UL, rate );
            bin = mag >> 8;
            if ( bin < 1 ) {
                bin = 1;
            }
            if ( bin >= half - 1 ) {
                bin = half - 2;
            }
            mag &= 255;
            best = (u32)(((u32)re[bin] >> 8) * (256 - mag) + ((u32)re[bin + 1] >> 8) * mag);
        }
        bars[bar] = level_of( best );
    }
    return 1;
}
