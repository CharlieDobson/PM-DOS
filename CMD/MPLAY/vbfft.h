/*
 * VBFFT.H - the body of the inverse transform, included twice by
 * vbmdct.c: once with 32-bit twiddles and once with 16-bit ones.
 *
 * ONE SOURCE, TWO PRECISIONS.  On a 386 or 486 a multiply takes longer
 * the more significant bits its multiplier has, so the fast path is
 * the same arithmetic with the twiddles cut to 16 bits and passed as
 * the multiplier.  The includer defines:
 *
 *   TW_TYPE            s32 or s16, the type of a twiddle
 *   TW_FIELD           tw or tw16, where the tables are
 *   TW_MUL( tw, x )    a twiddle times a sample, back in the sample's format
 *   PRE_NAME FFT_NAME POST_NAME    the names of the three functions
 *
 * A COMPLEX PRODUCT IS THREE MULTIPLIES, not four: (p + jq)(c + js) is
 * t - q(c + s) and t + p(s - c), with t = c(p + q).  c + s and s - c
 * are at most the square root of two, which both formats hold.
 */

/* z[m] = (X[2m] - j X[M-1-2m]) * e^(j 2 pi (m + 1/8) / n), for m < n/4 */
static void PRE_NAME( const VBMDCT *md, const s32 *spec, s32 *work )
{
    u32 count = md->n >> 2, m;
    const TW_TYPE *tw = md->TW_FIELD;
    const s32 *top = spec + (md->n >> 1) - 1;
    s32 a, b, t, c, s;

    for ( m = 0; m < count; m++ ) {
        a = spec[0];
        b = top[0];
        c = tw[0];
        s = tw[1];
        t = TW_MUL( c, a - b );
        work[0] = t + TW_MUL( c + s, b );
        work[1] = t + TW_MUL( s - c, a );
        spec += 2;
        top -= 2;
        tw += 2;
        work += 2;
    }
}

/* An inverse FFT of "count" complex values in place (a power of two,
   at least 4), by radix-2^2 decimation in frequency: two radix-2 stages
   at a time, with three twiddle products per four values.  The result
   is in bit-reversed order. */
static void FFT_NAME( s32 *data, u32 count, const VBFFT *fft )
{
    u32 span, quarter, group, i, step;
    const TW_TYPE *tw1, *tw2, *tw3;
    s32 *p0, *p1, *p2, *p3;
    s32 ar, ai, br, bi, cr, ci, dr, di, xr, xi, t, c, s;

    for ( span = count; span >= 4; span >>= 2 ) {
        quarter = span >> 2;
        step = ((1UL << fft->log2k) / span) * 2;
        for ( group = 0; group < count; group += span ) {
            p0 = data + 2 * group;
            p1 = p0 + 2 * quarter;
            p2 = p1 + 2 * quarter;
            p3 = p2 + 2 * quarter;
            /* the first of each group: every twiddle is 1 */
            ar = p0[0] + p2[0];
            ai = p0[1] + p2[1];
            br = p0[0] - p2[0];
            bi = p0[1] - p2[1];
            cr = p1[0] + p3[0];
            ci = p1[1] + p3[1];
            dr = p1[0] - p3[0];
            di = p1[1] - p3[1];
            p0[0] = ar + cr;
            p0[1] = ai + ci;
            p1[0] = ar - cr;
            p1[1] = ai - ci;
            p2[0] = br - di;
            p2[1] = bi + dr;
            p3[0] = br + di;
            p3[1] = bi - dr;
            tw1 = fft->TW_FIELD + step;
            tw2 = tw1 + step;
            tw3 = tw2 + step;
            for ( i = 1; i < quarter; i++ ) {
                p0 += 2;
                p1 += 2;
                p2 += 2;
                p3 += 2;
                ar = p0[0] + p2[0];
                ai = p0[1] + p2[1];
                br = p0[0] - p2[0];
                bi = p0[1] - p2[1];
                cr = p1[0] + p3[0];
                ci = p1[1] + p3[1];
                dr = p1[0] - p3[0];
                di = p1[1] - p3[1];
                p0[0] = ar + cr;
                p0[1] = ai + ci;
                /* (a - c) times W^2i */
                xr = ar - cr;
                xi = ai - ci;
                c = tw2[0];
                s = tw2[1];
                t = TW_MUL( c, xr + xi );
                p1[0] = t - TW_MUL( c + s, xi );
                p1[1] = t + TW_MUL( s - c, xr );
                /* (b + jd) times W^i */
                xr = br - di;
                xi = bi + dr;
                c = tw1[0];
                s = tw1[1];
                t = TW_MUL( c, xr + xi );
                p2[0] = t - TW_MUL( c + s, xi );
                p2[1] = t + TW_MUL( s - c, xr );
                /* (b - jd) times W^3i */
                xr = br + di;
                xi = bi - dr;
                c = tw3[0];
                s = tw3[1];
                t = TW_MUL( c, xr + xi );
                p3[0] = t - TW_MUL( c + s, xi );
                p3[1] = t + TW_MUL( s - c, xr );
                tw1 += step;
                tw2 += 2 * step;
                tw3 += 3 * step;
            }
        }
    }
    if ( span == 2 ) {
        /* an odd power of two ends with one radix-2 stage, whose twiddle is 1 */
        for ( p0 = data, i = 0; i < count; i += 2, p0 += 4 ) {
            xr = p0[0];
            xi = p0[1];
            p0[0] = xr + p0[2];
            p0[1] = xi + p0[3];
            p0[2] = xr - p0[2];
            p0[3] = xi - p0[3];
        }
    }
}

/* C[q] = Z[q] * e^(j 2 pi (q + 1/8) / n); u[2q] = Re C, u[M-1-2q] = Im C */
static void POST_NAME( const VBMDCT *md, const s32 *work, s32 *u )
{
    u32 count = md->n >> 2, q;
    const TW_TYPE *tw = md->TW_FIELD;
    const u16 *rev = md->rev;
    s32 *top = u + (md->n >> 1) - 1;
    const s32 *z;
    s32 zr, zi, t, c, s;

    for ( q = 0; q < count; q++ ) {
        z = work + 2 * (u32)rev[q];
        zr = z[0];
        zi = z[1];
        c = tw[0];
        s = tw[1];
        t = TW_MUL( c, zr + zi );
        u[0] = t - TW_MUL( c + s, zi );
        top[0] = t + TW_MUL( s - c, zr );
        tw += 2;
        u += 2;
        top -= 2;
    }
}
