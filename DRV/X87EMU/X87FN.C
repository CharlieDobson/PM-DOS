/*
 * X87FN.C - the 387's transcendental functions, on the working number
 *
 * Each is reduced to a small argument and summed as a series, in 128
 * bits, and the series stops once a term is 2^-80 of the sum: sixteen
 * bits past what a register holds, so that the one rounding the caller
 * does is nearly always the right one.  The 387 itself promises less -
 * an error under one unit in the last place.
 *
 * The constants were made with Math::BigFloat to 70 digits and rounded
 * to 128 bits.  As { the four significand words, least first; e }.
 */
#include "x87emu.h"

#define STOP            80      /* a term this far below the sum ends it */
#define DIVBITS         127     /* bits a quotient here is taken to */

static const uint32_t k_log2_10[5] = { 0x4DAFDB4D, 0x492BF6FF, 0xCD1B8AFE, 0xD49A784B, (uint32_t)1 };
static const uint32_t k_log2_e[5]  = { 0x691D3E89, 0xBE87FED0, 0x5C17F0BB, 0xB8AA3B29, (uint32_t)0 };
static const uint32_t k_pi[5]      = { 0x80DC1CD1, 0xC4C6628B, 0x2168C234, 0xC90FDAA2, (uint32_t)1 };
static const uint32_t k_log10_2[5] = { 0x0B7C9178, 0x8F8959AC, 0xFBCFF798, 0x9A209A84, (uint32_t)-2 };
static const uint32_t k_ln_2[5]    = { 0x03F2F6AF, 0xC9E3B398, 0xD1CF79AB, 0xB17217F7, (uint32_t)-1 };
static const uint32_t k_pi_2[5]    = { 0x80DC1CD1, 0xC4C6628B, 0x2168C234, 0xC90FDAA2, (uint32_t)0 };
static const uint32_t k_2_pi[5]    = { 0xF534DDC1, 0xFC2757D1, 0x4E441529, 0xA2F9836E, (uint32_t)-1 };
static const uint32_t k_sqrt2[5]   = { 0x754ABE9F, 0x597D89B3, 0xF9DE6484, 0xB504F333, (uint32_t)0 };

/* pi/2 in three parts of 64 bits, so that q times the first is exact */
static const uint32_t k_pio2_1[5]  = { 0x00000000, 0x00000000, 0x2168C234, 0xC90FDAA2, (uint32_t)0 };
static const uint32_t k_pio2_2[5]  = { 0x00000000, 0x00000000, 0x80DC1CD1, 0xC4C6628B, (uint32_t)-64 };
static const uint32_t k_pio2_3[5]  = { 0x00000000, 0x00000000, 0x299F31D0, 0xA4093822, (uint32_t)-130 };

/* atan( k / 8 ), k = 1 to 8 */
static const uint32_t k_atan[8][5] = {
    { 0x3E888EF8, 0xC897989F, 0x617B6E32, 0xFEADD4D5, (uint32_t)-4 },
    { 0xF7A217E6, 0x6DC79EF5, 0x6406EB15, 0xFADBAFC9, (uint32_t)-3 },
    { 0xDCFE4483, 0x8AA32122, 0x26F78473, 0xB7B0CA0F, (uint32_t)-2 },
    { 0xBC3A8D03, 0x6FE445EC, 0x0DDA7B45, 0xED63382B, (uint32_t)-2 },
    { 0x65C43748, 0x5C835E16, 0xF7F59F9B, 0x8F005D5E, (uint32_t)-1 },
    { 0x457DAC9F, 0x19A87F2A, 0x34F70924, 0xA4BC7D19, (uint32_t)-1 },
    { 0x10A4443D, 0xCB2DA552, 0xC2319E73, 0xB8053E2B, (uint32_t)-1 },
    { 0x80DC1CD1, 0xC4C6628B, 0x2168C234, 0xC90FDAA2, (uint32_t)-1 }
};

/* FLDL2T, FLDL2E, FLDPI, FLDLG2, FLDLN2 - unrounded; the caller rounds
   them by RC, as a 387 does */
void fn_const( wide *w, int which )
{
    switch ( which ) {
    case KC_LOG2_10:
        w_set( w, k_log2_10 );
        break;
    case KC_LOG2_E:
        w_set( w, k_log2_e );
        break;
    case KC_PI:
        w_set( w, k_pi );
        break;
    case KC_LOG10_2:
        w_set( w, k_log10_2 );
        break;
    default:
        w_set( w, k_ln_2 );
        break;
    }
}

/* the series has gone far enough */
static int small( const wide *term, const wide *sum )
{
    return term->kind == K_ZERO || term->e < sum->e - STOP;
}

/* 2 atanh( num / den ) = ln( (den + num) / (den - num) ), for a ratio
   well inside +-1 */
static void ln_ratio( wide *res, const wide *num, const wide *den )
{
    wide ratio, sq, power, term, sum;
    unsigned int n;

    w_div( &ratio, num, den, DIVBITS );
    if ( ratio.kind == K_ZERO ) {
        *res = ratio;
        return;
    }
    w_mul( &sq, &ratio, &ratio );
    sum = ratio;
    power = ratio;
    for ( n = 3; n < 400; n += 2 ) {
        w_mul( &power, &power, &sq );
        term = power;
        w_divs( &term, n );
        if ( small( &term, &sum ) ) {
            break;
        }
        w_add( &sum, &sum, &term );
    }
    sum.e++;
    *res = sum;
}

/* log2 x, x finite and above zero */
void fn_log2( wide *res, const wide *x )
{
    wide mant, num, den, one, k, whole;
    int32_t ex = x->e;

    /* x = 2^ex m, m from sqrt(1/2) to sqrt(2) */
    mant = *x;
    mant.e = 0;
    mant.sign = 0;
    mant.denorm = 0;
    w_set( &k, k_sqrt2 );
    if ( w_cmpmag( &mant, &k ) > 0 ) {
        mant.e = -1;
        ex++;
    }
    w_one( &one );
    w_sub( &num, &mant, &one );
    w_add( &den, &mant, &one );
    ln_ratio( res, &num, &den );
    w_set( &k, k_log2_e );
    w_mul( res, res, &k );
    if ( ex != 0 ) {
        w_int( &whole, (uint32_t)( ex < 0 ? -ex : ex ) );
        whole.sign = (unsigned char)( ex < 0 );
        w_add( res, &whole, res );
    }
}

/* log2( 1 + x ), x finite and above -1.  Near zero the 1 is never added,
   so that a small x keeps all of its bits. */
void fn_log2p1( wide *res, const wide *x )
{
    wide den, two, k;

    if ( x->kind == K_ZERO ) {
        *res = *x;
        return;
    }
    if ( x->e < -2 ) {
        w_int( &two, 2 );
        w_add( &den, &two, x );
        ln_ratio( res, x, &den );
        w_set( &k, k_log2_e );
        w_mul( res, res, &k );
        return;
    }
    w_one( &two );
    w_add( &den, &two, x );
    fn_log2( res, &den );
}

/* 2^x - 1, x finite */
void fn_exp2m1( wide *res, const wide *x )
{
    wide arg, term, sum, two, k;
    int32_t halvings;
    int n;

    if ( x->kind == K_ZERO ) {
        *res = *x;
        return;
    }
    if ( x->e >= 15 ) {
        /* past any exponent: 2^x overflows, or is -1 less a little */
        w_zero( res );
        res->kind = K_FIN;
        if ( !x->sign ) {
            res->m[3] = 0x80000000;
            res->e = 0x10000;
        } else {
            res->m[0] = 0xFFFFFFFF;
            res->m[1] = 0xFFFFFFFF;
            res->m[2] = 0xFFFFFFFF;
            res->m[3] = 0xFFFFFFFF;
            res->e = -1;
            res->sign = 1;
        }
        return;
    }
    /* arg = x ln 2, halved until it is under 2^-8; e^arg - 1 by its
       series; then doubled back: e^2a - 1 = (e^a - 1)(e^a - 1 + 2) */
    w_set( &k, k_ln_2 );
    w_mul( &arg, x, &k );
    halvings = arg.e + 9;
    if ( halvings < 0 ) {
        halvings = 0;
    }
    arg.e -= halvings;
    sum = arg;
    term = arg;
    for ( n = 2; n < 60; n++ ) {
        w_mul( &term, &term, &arg );
        w_divs( &term, (uint32_t)n );
        if ( small( &term, &sum ) ) {
            break;
        }
        w_add( &sum, &sum, &term );
    }
    w_int( &two, 2 );
    for ( n = 0; n < halvings; n++ ) {
        w_add( &term, &sum, &two );
        w_mul( &sum, &sum, &term );
    }
    *res = sum;
}

/* atan u for a small u */
static void atan_small( wide *res, const wide *u )
{
    wide sq, power, term, sum;
    unsigned int n;

    if ( u->kind == K_ZERO ) {
        *res = *u;
        return;
    }
    w_mul( &sq, u, u );
    sq.sign = 1;                        /* the terms alternate */
    sum = *u;
    power = *u;
    for ( n = 3; n < 400; n += 2 ) {
        w_mul( &power, &power, &sq );
        term = power;
        w_divs( &term, n );
        if ( small( &term, &sum ) ) {
            break;
        }
        w_add( &sum, &sum, &term );
    }
    *res = sum;
}

/* atan( y / x ) in the quadrant the two signs say, both finite */
void fn_atan2( wide *res, const wide *y, const wide *x )
{
    wide num, den, ratio, near8, part, one, k;
    unsigned int eighths = 0;
    int swap;

    num = *y;
    num.sign = 0;
    den = *x;
    den.sign = 0;
    if ( num.kind == K_ZERO ) {
        if ( x->sign ) {
            w_set( res, k_pi );
        } else {
            w_zero( res );
        }
        res->sign = y->sign;
        return;
    }
    if ( den.kind == K_ZERO ) {
        w_set( res, k_pi_2 );
        res->sign = y->sign;
        return;
    }
    /* the ratio from 0 to 1 */
    swap = w_cmpmag( &num, &den ) > 0;
    if ( swap ) {
        w_div( &ratio, &den, &num, DIVBITS );
    } else {
        w_div( &ratio, &num, &den, DIVBITS );
    }
    /* atan r = atan c + atan( (r - c) / (1 + r c) ), c the nearest
       eighth, which leaves at most a sixteenth for the series */
    near8 = ratio;
    near8.e += 3;
    w_rint( &near8, RC_NEAR );
    if ( near8.kind == K_FIN && near8.e < 4 ) {
        eighths = near8.m[3] >> ( 31 - near8.e );
    }
    if ( eighths == 0 ) {
        atan_small( res, &ratio );
    } else {
        near8.e -= 3;
        w_sub( &num, &ratio, &near8 );
        w_mul( &den, &ratio, &near8 );
        w_one( &one );
        w_add( &den, &den, &one );
        w_div( &part, &num, &den, DIVBITS );
        atan_small( &part, &part );
        w_set( &k, k_atan[eighths - 1] );
        w_add( res, &k, &part );
    }
    if ( swap ) {
        w_set( &k, k_pi_2 );
        w_sub( res, &k, res );
    }
    if ( x->sign ) {
        w_set( &k, k_pi );
        w_sub( res, &k, res );
    }
    res->sign = y->sign;
}

/* sin r and cos r for |r| up to about pi/4 */
static void sincos_small( wide *sinv, wide *cosv, const wide *r )
{
    wide sq, term, sum;
    unsigned int n;

    w_mul( &sq, r, r );
    sq.sign = 1;
    sum = *r;
    term = *r;
    for ( n = 2; n < 80; n += 2 ) {
        w_mul( &term, &term, &sq );
        w_divs( &term, n * ( n + 1 ) );
        if ( small( &term, &sum ) ) {
            break;
        }
        w_add( &sum, &sum, &term );
    }
    *sinv = sum;
    w_one( &sum );
    w_one( &term );
    for ( n = 1; n < 80; n += 2 ) {
        w_mul( &term, &term, &sq );
        w_divs( &term, n * ( n + 1 ) );
        if ( small( &term, &sum ) ) {
            break;
        }
        w_add( &sum, &sum, &term );
    }
    *cosv = sum;
}

/* sin x and cos x, x finite and under 2^63.  -> the quadrant. */
int fn_sincos( wide *sinv, wide *cosv, const wide *x )
{
    wide mag, quot, red, part, prod, s, c;
    unsigned int quad = 0;

    mag = *x;
    mag.sign = 0;
    mag.denorm = 0;
    if ( mag.kind == K_ZERO ) {
        *sinv = *x;
        w_one( cosv );
        return 0;
    }
    /* quot = the nearest whole number to |x| 2/pi; then
       red = |x| - quot pi/2, a part of pi/2 at a time */
    w_set( &part, k_2_pi );
    w_mul( &quot, &mag, &part );
    w_rint( &quot, RC_NEAR );
    red = mag;
    if ( quot.kind == K_FIN ) {
        quad = w_bit( &quot, 127 - quot.e ) | ( w_bit( &quot, 128 - quot.e ) << 1 );
        w_set( &part, k_pio2_1 );
        w_mul( &prod, &quot, &part );
        w_sub( &red, &red, &prod );
        w_set( &part, k_pio2_2 );
        w_mul( &prod, &quot, &part );
        w_sub( &red, &red, &prod );
        w_set( &part, k_pio2_3 );
        w_mul( &prod, &quot, &part );
        w_sub( &red, &red, &prod );
    }
    sincos_small( &s, &c, &red );
    switch ( quad ) {
    case 0:
        *sinv = s;
        *cosv = c;
        break;
    case 1:
        *sinv = c;
        *cosv = s;
        cosv->sign ^= 1;
        break;
    case 2:
        *sinv = s;
        sinv->sign ^= 1;
        *cosv = c;
        cosv->sign ^= 1;
        break;
    default:
        *sinv = c;
        sinv->sign ^= 1;
        *cosv = s;
        break;
    }
    if ( x->sign ) {
        sinv->sign ^= 1;
    }
    return (int)quad;
}
