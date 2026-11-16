/*
 * MP3SYN.C - the polyphase synthesis filter bank of MPEG audio, every
 * layer's last step (MP3.H).
 *
 * THE STANDARD'S RECIPE, REARRANGED.  ISO 11172-3 matrixes each slot's
 * 32 subband samples into 64 values V, keeps the last 16 such vectors,
 * and makes each output sample from 16 of them times 16 taps of a
 * 512-tap window D.  The 64 values are one 32-point DCT seen through
 * its symmetries: the first half F is the DCT's upper half, turned
 * over about its middle, and the second half G is the lower half,
 * mirrored.  So only 17 numbers a slot are kept, and output j and
 * output 32 - j are made from the same 16 history values.
 *
 * THE DCT is Byeong Gi Lee's split: the sums of mirrored inputs give
 * the even outputs, their differences scaled by 1 / (2 cos) give the
 * odd ones after one more addition.  80 multiplies for 32 points, 32
 * for 16, 12 for 8.
 *
 * THE HISTORY IS KEPT TWICE OVER.  The 16 values an output needs are
 * the newest vector and the 15 before it, alternately F and G.  Two
 * histories take turns: this slot's F goes into the one that is read
 * now and its G into the other, so whichever is read holds F at every
 * even age and G at every odd one.  Each is written at a position and
 * again 16 further on, so the 16 values are always in a row and the
 * window loop needs no wrapping.
 *
 * THE WINDOW IS 17-BIT WHOLE NUMBERS.  Every value of the standard's
 * table is a multiple of 2^-16, so the taps are exact integers, and
 * the processor's multiplier - which on a 386 or 486 finishes sooner
 * for a smaller operand - is given the tap.
 *
 * FEWER SUBBANDS, SAME CODE.  A 16- or 8-band synthesis with every
 * second or fourth tap of the window gives exactly every second or
 * fourth sample of the full one (with the upper subbands empty).  The
 * fast path leaves out the outermost tap at each end of the window,
 * where it is smallest: 14 multiplies a sample instead of 16, at a cost
 * of about -75 dB.
 *
 * THE 16 PRODUCTS ARE ADDED IN 64 BITS (MP3.H's mp3_dot16) and rounded
 * once.  Rounding each product down on its own made the output about a
 * fiftieth of a bit low, which shows as a sample in fifty one lower than
 * the reference decoder's.
 */
#include "rt.h"
#include "mp3.h"

#define HIST_ROW        32              /* a history row: 16 positions, twice */
#define HIST_SIZE       (17 * HIST_ROW) /* one history */

/* ------------------------------------------------------------------ */
/* the DCT: out[m] = sum of in[k] cos(m (2k + 1) pi / 2N)               */
/* ------------------------------------------------------------------ */
static void dct4( const s32 *in, s32 *out )
{
    s32 e0, e1, o0, o1, odd;

    e0 = in[0] + in[3];
    e1 = in[1] + in[2];
    o0 = mp3_mul27( mp3_lee4[0], in[0] - in[3] );
    o1 = mp3_mul27( mp3_lee4[1], in[1] - in[2] );
    out[0] = e0 + e1;
    out[2] = mp3_mul27( mp3_lee2[0], e0 - e1 );
    odd = mp3_mul27( mp3_lee2[0], o0 - o1 );
    out[1] = o0 + o1 + odd;
    out[3] = odd;
}

static void dct8( const s32 *in, s32 *out )
{
    s32 even[4], odd[4], lo[4], hi[4];
    int k;

    for ( k = 0; k < 4; k++ ) {
        even[k] = in[k] + in[7 - k];
        odd[k] = mp3_mul27( mp3_lee8[k], in[k] - in[7 - k] );
    }
    dct4( even, lo );
    dct4( odd, hi );
    for ( k = 0; k < 4; k++ ) {
        out[2 * k] = lo[k];
    }
    out[1] = hi[0] + hi[1];
    out[3] = hi[1] + hi[2];
    out[5] = hi[2] + hi[3];
    out[7] = hi[3];
}

static void dct16( const s32 *in, s32 *out )
{
    s32 even[8], odd[8], lo[8], hi[8];
    int k;

    for ( k = 0; k < 8; k++ ) {
        even[k] = in[k] + in[15 - k];
        odd[k] = mp3_mul27( mp3_lee16[k], in[k] - in[15 - k] );
    }
    dct8( even, lo );
    dct8( odd, hi );
    for ( k = 0; k < 7; k++ ) {
        out[2 * k] = lo[k];
        out[2 * k + 1] = hi[k] + hi[k + 1];
    }
    out[14] = lo[7];
    out[15] = hi[7];
}

static void dct32( const s32 *in, s32 *out )
{
    s32 even[16], odd[16], lo[16], hi[16];
    int k;

    for ( k = 0; k < 16; k++ ) {
        even[k] = in[k] + in[31 - k];
        odd[k] = mp3_mul27( mp3_lee32[k], in[k] - in[31 - k] );
    }
    dct16( even, lo );
    dct16( odd, hi );
    for ( k = 0; k < 15; k++ ) {
        out[2 * k] = lo[k];
        out[2 * k + 1] = hi[k] + hi[k + 1];
    }
    out[30] = lo[15];
    out[31] = hi[15];
}

/* ------------------------------------------------------------------ */
/* the state                                                           */
/* ------------------------------------------------------------------ */
int mp3_syn_open( MP3SYN *syn )
{
    syn->hist = (s32 *)try_alloc( 2 * HIST_SIZE * sizeof( s32 ) );
    if ( syn->hist == NULL ) {
        return DE_NOMEM;
    }
    mp3_syn_reset( syn );
    return DE_OK;
}

void mp3_syn_close( MP3SYN *syn )
{
    if ( syn->hist != NULL ) {
        xfree( syn->hist );
        syn->hist = NULL;
    }
}

void mp3_syn_reset( MP3SYN *syn )
{
    if ( syn->hist != NULL ) {
        mem_set( syn->hist, 0, 2 * HIST_SIZE * sizeof( s32 ) );
    }
    syn->pos = 0;
    syn->phase = 0;
}

/* ------------------------------------------------------------------ */
/* one slot                                                            */
/* ------------------------------------------------------------------ */
#define CLIP16( val )   ((s16)((val) > 32767 ? 32767 : ((val) < -32768 ? -32768 : (val))))

void mp3_synth( MP3SYN *syn, const s32 *sb, int nsb, s16 *out, int stride, int taps )
{
    s32 mat[32], pair[2];
    s32 *hf, *hg;
    const s32 *hv, *win;
    s16 *lo, *hi;
    s32 val;
    int half, pos, j;

    if ( nsb == 32 ) {
        dct32( sb, mat );
        win = mp3_win32;
    } else if ( nsb == 16 ) {
        dct16( sb, mat );
        win = mp3_win16;
    } else {
        nsb = 8;
        dct8( sb, mat );
        win = mp3_win8;
    }
    half = nsb >> 1;

    /* the new vector into both histories, twice */
    pos = (syn->pos - 1) & 15;
    syn->pos = pos;
    syn->phase ^= 1;
    hf = syn->hist + syn->phase * HIST_SIZE + pos;
    hg = syn->hist + (syn->phase ^ 1) * HIST_SIZE + pos;
    for ( j = 0; j < half; j++ ) {
        hf[0] = hf[16] = mat[half + j];
        hg[0] = hg[16] = -mat[half - j];
        hf += HIST_ROW;
        hg += HIST_ROW;
    }
    hf[0] = hf[16] = 0;
    hg[0] = hg[16] = -mat[0];

    /* the window: output 0, the pairs j and N - j, then output N/2, whose
       F values are 0 at every even age */
    hv = syn->hist + syn->phase * HIST_SIZE + pos;
    if ( taps > 14 ) {
        val = mp3_dot16( hv, win );
    } else if ( taps > 12 ) {
        val = mp3_dot14( hv, win );
    } else {
        val = mp3_dot12( hv, win );
    }
    out[0] = CLIP16( val );
    win += 16;
    lo = out + stride;
    hi = out + (nsb - 1) * stride;
    for ( j = 1; j < half; j++ ) {
        hv += HIST_ROW;
        if ( taps > 14 ) {
            mp3_pair8( hv, win, pair );
        } else if ( taps > 12 ) {
            mp3_pair7( hv, win, pair );
        } else {
            mp3_pair6( hv, win, pair );
        }
        val = (pair[0] + 2048) >> 12;           /* Q27 to a sample */
        *lo = CLIP16( val );
        val = (pair[1] + 2048) >> 12;
        *hi = CLIP16( val );
        win += 24;
        lo += stride;
        hi -= stride;
    }
    hv += HIST_ROW;
    if ( taps > 14 ) {
        val = mp3_odd8( hv, win );
    } else if ( taps > 12 ) {
        val = mp3_odd7( hv, win );
    } else {
        val = mp3_odd6( hv, win );
    }
    *lo = CLIP16( val );
}
