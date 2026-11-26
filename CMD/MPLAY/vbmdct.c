/*
 * VBMDCT.C - the Vorbis decoder's inverse transform: the inverse MDCT,
 * the window and the overlap of each block with the one before (VB.H).
 *
 * AN INVERSE MDCT OF n SAMPLES IS A DCT-IV OF n/2, UNFOLDED.  Writing
 * M = n/2 and u for the DCT-IV of the n/2 spectral lines, the block is
 * u[M/2 + i] for its first quarter, -u[3M/2 - 1 - i] for the middle
 * half and -u[i - 3M/2] for the last quarter.  So u alone describes
 * the whole block: its upper half makes the block's left half, which
 * meets the previous block, and its lower half makes the right half,
 * which waits for the next one.  Nothing is unfolded into a buffer;
 * vb_lap reads u where it needs it.
 *
 * THE DCT-IV IS A COMPLEX FFT OF n/4 POINTS between two twiddles.
 * Pairing line 2m with line M-1-2m as one complex number, turning it
 * by e^(j 2 pi (m + 1/8) / n), transforming, and turning each result
 * by the same table again gives two values of u at once.  The FFT is
 * radix-2^2: the arithmetic of radix 4 with the bit-reversed order of
 * radix 2, which a table of n/4 indices undoes on the way out.
 *
 * THE WINDOW IS APPLIED DURING THE OVERLAP.  Where two blocks overlap,
 * a pair of values - one from each - makes two output samples with
 * four multiplies; where only one block sounds the window is 1 and the
 * value is copied.  The overlap is set by the two blocks' actual
 * sizes: the smaller one's window, centered on the larger one's
 * quarter point.  The specification allows that, and it means a
 * damaged window flag cannot make the halves disagree.
 *
 * THE EQUALIZER RIDES ON THE FIRST TWIDDLE.  Each line's gain is
 * multiplied into the twiddle that line meets, so a curve costs one
 * more multiply for two lines instead of one for each.
 *
 * THE FAST PATH (VBFFT.H, compiled twice) uses 16-bit twiddles and
 * window; a 486 multiplies by a 16-bit number in about two thirds of
 * the time a full 32-bit one takes.
 */
#include "rt.h"
#include "fix.h"
#include "vb.h"

void vb_sincos( uint32_t phase, int32_t *cosv, int32_t *sinv )
{
    uint32_t coarse = phase >> 24, fine = (phase >> 16) & 255, rest = phase & 0xFFFF;
    int32_t cos_a = vb_cos_coarse[coarse], sin_a = vb_cos_coarse[(coarse + 192) & 255];
    int32_t cos_b = vb_cos_fine[fine], sin_b = vb_sin_fine[fine];
    int32_t cos_x, sin_x, delta, half;

    cos_x = vb_mul30r( cos_a, cos_b ) - vb_mul30r( sin_a, sin_b );
    sin_x = vb_mul30r( sin_a, cos_b ) + vb_mul30r( cos_a, sin_b );
    if ( rest == 0 ) {
        *cosv = cos_x;
        *sinv = sin_x;
        return;
    }
    /* the remainder is under 1/65536 turn: two terms of its series do */
    delta = vb_mul30r( (int32_t)rest, 1686629713L );    /* rest * 2 pi / 2^32, Q30 */
    half = vb_mul30r( delta, delta ) >> 1;
    *cosv = cos_x - vb_mul30r( sin_x, delta ) - vb_mul30r( cos_x, half );
    *sinv = sin_x + vb_mul30r( cos_x, delta ) - vb_mul30r( sin_x, half );
}

static int16_t to_q14( int32_t q30 )
{
    int32_t val = (q30 + 32768L) >> 16;

    return (int16_t)(val > 32767 ? 32767 : val);
}

void vb_mdct_free( VBMDCT *md )
{
    xfree( md->tw );
    xfree( md->tw16 );
    xfree( md->rev );
    xfree( md->win );
    xfree( md->win16 );
    xfree( md->eqtw );
    mem_set( md, 0, sizeof( VBMDCT ) );
}

int vb_mdct_init( VBMDCT *md, int log2n )
{
    unsigned int n = 1UL << log2n, quarter = n >> 2, half = n >> 1, m, rev, bit;
    int32_t cos_v, sin_v, sq, win;
    int bits = log2n - 2, k;

    mem_set( md, 0, sizeof( VBMDCT ) );
    md->log2n = log2n;
    md->n = n;
    md->tw = (int32_t *)try_alloc( quarter * 2 * sizeof( int32_t ) );
    md->tw16 = (int16_t *)try_alloc( quarter * 2 * sizeof( int16_t ) );
    md->rev = (uint16_t *)try_alloc( quarter * sizeof( uint16_t ) );
    md->win = (int32_t *)try_alloc( half * sizeof( int32_t ) );
    md->win16 = (int16_t *)try_alloc( half * sizeof( int16_t ) );
    if ( md->tw == NULL || md->tw16 == NULL || md->rev == NULL || md->win == NULL
         || md->win16 == NULL ) {
        vb_mdct_free( md );
        return -1;
    }
    for ( m = 0; m < quarter; m++ ) {
        vb_sincos( (8 * m + 1) << (29 - log2n), &cos_v, &sin_v );
        md->tw[2 * m] = cos_v;
        md->tw[2 * m + 1] = sin_v;
        md->tw16[2 * m] = to_q14( cos_v );
        md->tw16[2 * m + 1] = to_q14( sin_v );
        rev = 0;
        for ( k = 0, bit = m; k < bits; k++, bit >>= 1 ) {
            rev = (rev << 1) | (bit & 1);
        }
        md->rev[m] = (uint16_t)rev;
    }
    /* sin( pi/2 * sin^2( (j + 1/2) / half * pi/2 ) ) */
    for ( m = 0; m < half; m++ ) {
        vb_sincos( (2 * m + 1) << (30 - log2n), &cos_v, &sin_v );
        sq = vb_mul30r( sin_v, sin_v );
        vb_sincos( (uint32_t)sq, &cos_v, &sin_v );
        win = sin_v >= 0x40000000L ? 0x7FFFFFFFL : sin_v * 2;
        md->win[m] = win;
        rev = ((uint32_t)win + 32768UL) >> 16;
        md->win16[m] = (int16_t)(rev > 32767 ? 32767 : rev);
    }
    return 0;
}

void vb_fft_free( VBFFT *fft )
{
    xfree( fft->tw );
    xfree( fft->tw16 );
    mem_set( fft, 0, sizeof( VBFFT ) );
}

int vb_fft_init( VBFFT *fft, int log2k )
{
    unsigned int count = (3UL << log2k) / 4, t;
    int32_t cos_v, sin_v;

    mem_set( fft, 0, sizeof( VBFFT ) );
    fft->log2k = log2k;
    fft->tw = (int32_t *)try_alloc( count * 2 * sizeof( int32_t ) );
    fft->tw16 = (int16_t *)try_alloc( count * 2 * sizeof( int16_t ) );
    if ( fft->tw == NULL || fft->tw16 == NULL ) {
        vb_fft_free( fft );
        return -1;
    }
    for ( t = 0; t < count; t++ ) {
        vb_sincos( t << (32 - log2k), &cos_v, &sin_v );
        fft->tw[2 * t] = cos_v;
        fft->tw[2 * t + 1] = sin_v;
        fft->tw16[2 * t] = to_q14( cos_v );
        fft->tw16[2 * t + 1] = to_q14( sin_v );
    }
    return 0;
}

int vb_mdct_eq( VBMDCT *md, const uint32_t *gain )
{
    unsigned int quarter = md->n >> 2, half = md->n >> 1, m;
    int32_t *eq, c, s, gain_a, gain_b;

    if ( md->eqtw == NULL ) {
        md->eqtw = (int32_t *)try_alloc( quarter * 4 * sizeof( int32_t ) );
        if ( md->eqtw == NULL ) {
            return -1;
        }
    }
    eq = md->eqtw;
    for ( m = 0; m < quarter; m++ ) {
        c = md->tw[2 * m];
        s = md->tw[2 * m + 1];
        gain_a = (int32_t)gain[2 * m];
        gain_b = (int32_t)gain[half - 1 - 2 * m];
        eq[0] = fx_mulsh( c, gain_a, 18 );
        eq[1] = fx_mulsh( s, gain_b, 18 );
        eq[2] = fx_mulsh( s, gain_a, 18 );
        eq[3] = fx_mulsh( c, gain_b, 18 );
        eq += 4;
    }
    return 0;
}

/* the first twiddle with the equalizer's gains in it: four multiplies */
static void pre_eq( const VBMDCT *md, const int32_t *spec, int32_t *work )
{
    unsigned int count = md->n >> 2, m;
    const int32_t *eq = md->eqtw;
    const int32_t *top = spec + (md->n >> 1) - 1;
    int32_t a, b;

    for ( m = 0; m < count; m++ ) {
        a = spec[0];
        b = top[0];
        work[0] = vb_mul28r( eq[0], a ) + vb_mul28r( eq[1], b );
        work[1] = vb_mul28r( eq[2], a ) - vb_mul28r( eq[3], b );
        spec += 2;
        top -= 2;
        eq += 4;
        work += 2;
    }
}

/* full precision: Q30 twiddles, the sample as the multiplier (it
   usually has fewer significant bits than the twiddle) */
#define TW_TYPE         int32_t
#define TW_FIELD        tw
#define TW_MUL( tw, x ) vb_mul30r( (tw), (x) )
#define PRE_NAME        pre_full
#define FFT_NAME        fft_full
#define POST_NAME       post_full
#include "vbfft.h"
#undef TW_TYPE
#undef TW_FIELD
#undef TW_MUL
#undef PRE_NAME
#undef FFT_NAME
#undef POST_NAME

/* the fast path: Q14 twiddles as the multiplier */
#define TW_TYPE         int16_t
#define TW_FIELD        tw16
#define TW_MUL( tw, x ) vb_mul14r( (x), (tw) )
#define PRE_NAME        pre_fast
#define FFT_NAME        fft_fast
#define POST_NAME       post_fast
#include "vbfft.h"
#undef TW_TYPE
#undef TW_FIELD
#undef TW_MUL
#undef PRE_NAME
#undef FFT_NAME
#undef POST_NAME

void vb_imdct( const VBMDCT *md, const VBFFT *fft, const int32_t *spec, int32_t *work, int32_t *u,
               int fast, int use_eq )
{
    if ( use_eq && md->eqtw != NULL ) {
        pre_eq( md, spec, work );
    } else if ( fast ) {
        pre_fast( md, spec, work );
    } else {
        pre_full( md, spec, work );
    }
    if ( fast ) {
        fft_fast( work, md->n >> 2, fft );
        post_fast( md, work, u );
    } else {
        fft_full( work, md->n >> 2, fft );
        post_full( md, work, u );
    }
}

/* a Q24 sample as 16 bits */
#define OUT24( val )    FX_SAT16( ((val) + 256) >> 9 )

unsigned int vb_lap( int16_t *pcm, int stride, const int32_t *prev, unsigned int pn, const int32_t *cur,
                     unsigned int n, const VBMDCT *small, int fast )
{
    unsigned int pm = pn >> 1, m = n >> 1, w = (pn < n ? pn : n) >> 1, hw = w >> 1;
    unsigned int count_a = (pm >> 1) - hw, j, pos;
    int16_t *out = pcm, *lo, *hi;
    const int32_t *win = small->win;
    const int16_t *win16 = small->win16;
    int32_t val, pv, cv, wl, wr, t;

    /* the previous block alone, where its window is 1 */
    for ( j = 0; j < count_a; j++ ) {
        val = -prev[(pm >> 1) - 1 - j];
        *out = (int16_t)OUT24( val );
        out += stride;
    }
    /* The overlap: frames j and w - 1 - j from one value of each block,
       pv wr + cv wl and pv wl - cv wr.  The two slopes are a cosine and
       a sine (wr^2 + wl^2 = 1), so this is a rotation, and a rotation
       takes three multiplies like any complex product. */
    lo = out;
    hi = out + (w - 1) * stride;
    if ( fast ) {
        for ( j = 0; j < hw; j++ ) {
            pv = -prev[hw - 1 - j];
            cv = cur[m - hw + j];
            wl = win16[j];
            wr = win16[w - 1 - j];
            t = fx_mulsh( pv - cv, wr, 15 );
            val = t + fx_mulsh( cv, wr + wl, 15 );
            *lo = (int16_t)OUT24( val );
            val = t + fx_mulsh( pv, wl - wr, 15 );
            *hi = (int16_t)OUT24( val );
            lo += stride;
            hi -= stride;
        }
    } else {
        /* the window as Q30 here, so that wr + wl fits; the products are Q22 */
        for ( j = 0; j < hw; j++ ) {
            pv = -prev[hw - 1 - j];
            cv = cur[m - hw + j];
            wl = win[j] >> 1;
            wr = win[w - 1 - j] >> 1;
            t = fx_mulhi( wr, pv - cv );
            val = t + fx_mulhi( wr + wl, cv );
            *lo = (int16_t)FX_SAT16( (val + 64) >> 7 );
            val = t + fx_mulhi( wl - wr, pv );
            *hi = (int16_t)FX_SAT16( (val + 64) >> 7 );
            lo += stride;
            hi -= stride;
        }
    }
    out += w * stride;
    /* this block alone, where its window is 1 */
    for ( pos = (m >> 1) + hw; pos < m; pos++ ) {
        val = -cur[3 * (m >> 1) - 1 - pos];
        *out = (int16_t)OUT24( val );
        out += stride;
    }
    return count_a + w + (m >> 1) - hw;
}
