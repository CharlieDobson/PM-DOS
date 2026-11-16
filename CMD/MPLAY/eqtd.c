/*
 * EQTD.C - the equalizer on samples: a peaking filter for each band
 * that is off zero (EQTD.H).
 *
 * THIS IS THE EXPENSIVE WAY, for the codecs with no spectrum in hand:
 * wave files, VOC and MIDI's synthesizer.  MP3, Vorbis and WMA apply
 * the curve to their spectral lines (MPEG layers 1 and 2 to their
 * subbands) for one multiply a line, and never come here.
 *
 * ONE BIQUAD A BAND, the "peaking" kind every studio equalizer is made
 * of: a bell an octave wide at the band's center, raised or lowered by
 * the slider, flat elsewhere.  Bands at 0 dB are left out altogether,
 * so a curve with three sliders moved costs three filters, and a flat
 * one costs nothing.  The preamp is one multiply at the end.
 *
 * THE COEFFICIENTS ARE Q28 and the samples carry eight bits below the
 * 16-bit ones through the filters.  A 31 Hz filter at 44.1 kHz has its
 * poles a whisker inside the unit circle - 1 - 0.0016 - and with fewer
 * bits than that it hums or drifts; with these it does neither.  Each
 * multiply is one IMUL and a SHRD (fx_mul28): five a band a sample.
 *
 * The formulas are the familiar ones from Robert Bristow-Johnson's
 * audio-EQ notes: A = 10^(dB/40), alpha = sin(w0) / 2Q with Q = 1.414
 * for an octave, b = 1 + alpha A, -2 cos w0, 1 - alpha A over a = 1 +
 * alpha / A, -2 cos w0, 1 - alpha / A.
 */
#include "rt.h"
#include "fix.h"
#include "trig.h"
#include "eqtd.h"

#define MAX_FRAMES      4608            /* the most frames eqtd_apply takes at once */
#define INV_2Q_Q30      379625062L      /* 1 / (2 * 1.41421) in Q30 */

typedef struct {
    s32 b0, b1, b2, a1, a2;             /* Q28, divided by a0 */
} BIQUAD;

static BIQUAD filt[EQ_BANDS];
static int active[EQ_BANDS];
static int nactive;
static s32 hist_x1[EQ_BANDS][2], hist_x2[EQ_BANDS][2];
static s32 hist_y1[EQ_BANDS][2], hist_y2[EQ_BANDS][2];
static u32 pre_gain = 65536UL;          /* Q16 */
static int nch = 2;
static u32 rate_now;
static s32 *work;

void eqtd_init( void )
{
    if ( work == NULL ) {
        work = (s32 *)try_alloc( MAX_FRAMES * 2 * sizeof( s32 ) );
    }
}

void eqtd_done( void )
{
    if ( work != NULL ) {
        xfree( work );
        work = NULL;
    }
}

void eqtd_set( const EQSET *eq, u32 rate, int channels )
{
    s32 sinv, cosv, alpha, alpha_a, alpha_div, a0, b0, b2, a2;
    u32 turn, gain;
    int band;

    if ( rate != rate_now || channels != nch ) {
        mem_set( hist_x1, 0, sizeof( hist_x1 ) );
        mem_set( hist_x2, 0, sizeof( hist_x2 ) );
        mem_set( hist_y1, 0, sizeof( hist_y1 ) );
        mem_set( hist_y2, 0, sizeof( hist_y2 ) );
    }
    rate_now = rate;
    nch = channels == 1 ? 1 : 2;
    nactive = 0;
    pre_gain = 65536UL;
    if ( eq == NULL || !eq->on || rate < 8000 ) {
        return;
    }
    pre_gain = eq_db_gain( (s32)eq->preamp * 128 );
    for ( band = 0; band < EQ_BANDS; band++ ) {
        if ( eq->band[band] == 0 ) {
            continue;
        }
        /* the center, 31.25 Hz times 2^band, as a fraction of a turn at
           this rate: (125 << band) / 4 Hz * 2^32 / rate */
        if ( (125UL << band) / 4 >= rate * 45 / 100 ) {
            continue;                   /* too near half the rate to be a bell */
        }
        turn = fx_umuldiv( 125UL << band, 1UL << 30, rate );
        fx_sincos( turn, &sinv, &cosv );
        gain = eq_db_gain( (s32)eq->band[band] * 64 );          /* A: half the decibels */
        alpha = fx_mul30( sinv, INV_2Q_Q30 );
        alpha_a = fx_mul16( alpha, (s32)gain );
        alpha_div = fx_muldiv( alpha, 65536L, (s32)gain );
        a0 = 0x40000000L + alpha_div;
        b0 = 0x40000000L + alpha_a;
        b2 = 0x40000000L - alpha_a;
        a2 = 0x40000000L - alpha_div;
        filt[nactive].b0 = fx_muldiv( b0, 1L << 28, a0 );
        filt[nactive].b1 = fx_muldiv( -cosv, 1L << 29, a0 );    /* -2 cos / a0 */
        filt[nactive].b2 = fx_muldiv( b2, 1L << 28, a0 );
        filt[nactive].a1 = filt[nactive].b1;
        filt[nactive].a2 = fx_muldiv( a2, 1L << 28, a0 );
        active[nactive] = band;
        nactive++;
    }
}

int eqtd_cost( void )
{
    return nactive * 5 + (pre_gain != 65536UL ? 1 : 0);
}

void eqtd_apply( s16 *pcm, u32 frames )
{
    u32 i, total, ch;
    int k, band;
    const BIQUAD *bq;
    s32 x, y, x1, x2, y1, y2, val;

    if ( work == NULL || (nactive == 0 && pre_gain == 65536UL) ) {
        return;
    }
    while ( frames > 0 ) {
        u32 now = frames < MAX_FRAMES ? frames : MAX_FRAMES;

        total = now * (u32)nch;
        for ( i = 0; i < total; i++ ) {
            work[i] = (s32)pcm[i] << 8;
        }
        for ( k = 0; k < nactive; k++ ) {
            bq = &filt[k];
            band = active[k];
            for ( ch = 0; ch < (u32)nch; ch++ ) {
                x1 = hist_x1[band][ch];
                x2 = hist_x2[band][ch];
                y1 = hist_y1[band][ch];
                y2 = hist_y2[band][ch];
                for ( i = ch; i < total; i += (u32)nch ) {
                    x = work[i];
                    y = fx_mul28( bq->b0, x ) + fx_mul28( bq->b1, x1 ) + fx_mul28( bq->b2, x2 )
                        - fx_mul28( bq->a1, y1 ) - fx_mul28( bq->a2, y2 );
                    x2 = x1;
                    x1 = x;
                    y2 = y1;
                    y1 = y;
                    work[i] = y;
                }
                hist_x1[band][ch] = x1;
                hist_x2[band][ch] = x2;
                hist_y1[band][ch] = y1;
                hist_y2[band][ch] = y2;
            }
        }
        for ( i = 0; i < total; i++ ) {
            val = pre_gain == 65536UL ? work[i] >> 8 : fx_mulsh( work[i], (s32)pre_gain, 24 );
            pcm[i] = (s16)FX_SAT16( val );
        }
        pcm += total;
        frames -= now;
    }
}
