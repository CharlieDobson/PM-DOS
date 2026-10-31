/*
 * MATHSPEC.C - erf, erfc, tgamma and lgamma
 *
 * erf(x) below 2 is the series 2x/sqrt(pi) e^-x^2 sum (2x^2)^n /
 * (1.3.5...(2n+1)), whose terms are all positive, so nothing cancels.
 * erfc(x) from 1/2 up is the continued fraction e^-x^2 / sqrt(pi) /
 * (x + (1/2)/(x + 1/(x + (3/2)/(x + ...)))), and erf from 2 up is
 * 1 - erfc.  1 - erf(x) would lose bits to the subtraction there.
 *
 * Below 20 the gamma functions move x to 2 + t, |t| <= 1/2, with
 * Gamma(x + 1) = x Gamma(x), and ln Gamma(2 + t) is the series in
 * t below.  That keeps every step small: shifting up to Stirling's
 * series and subtracting a large logarithm cost Open Watcom, whose
 * long double is a double, the last eight bits.  From 20 up it is
 * Stirling's series; tgamma multiplies the shift out up to 172.
 * Below 1/2 they reflect, Gamma(x) Gamma(1 - x) = pi / sin(pi x).
 */
#include "pmlib.h"
#include "mathx.h"

#define SQRT_PI     1.772453850905516027298167483341145183L
#define LN_SQRT_2PI 0.918938533204672741780329736405617640L

static int is_nan( long double x )  { return __pm_fpclassifyl( x ) == FP_NAN; }
static int is_inf( long double x )  { return __pm_fpclassifyl( x ) == FP_INFINITE; }

/*--- erf and erfc ----------------------------------------------------*/

static long double erf_series( long double x )
{
    long double x2 = x * x;
    long double term = 1.0L;
    long double sum = 1.0L;
    int n;

    for ( n = 0; n < 200; n++ ) {
        term *= 2.0L * x2 / (2 * n + 3);
        sum += term;
        if ( term < sum * 1e-21L ) {
            break;
        }
    }
    return 2.0L * x / SQRT_PI * expl( -x2 ) * sum;
}

/* erfc for x >= 1/2.  The fraction is worked from the bottom up, and
   the depth is what it takes for 64 bits at the bottom of each range */
static long double erfc_fraction( long double x )
{
    long double t = x;
    int k;

    for ( k = x < 1.0L ? 1200 : x < 2.0L ? 320 : 120; k >= 1; k-- ) {
        t = x + (k / 2.0L) / t;
    }
    return expl( -x * x ) / (SQRT_PI * t);
}

long double erfl( long double x )
{
    long double a = fabsl( x );
    long double r;

    if ( is_nan( x ) ) {
        return x;
    }
    if ( is_inf( x ) ) {
        return x < 0.0L ? -1.0L : 1.0L;
    }
    if ( a < 2.0L ) {
        return erf_series( x );
    }
    r = a > 110.0L ? 1.0L : 1.0L - erfc_fraction( a );
    return x < 0.0L ? -r : r;
}

long double erfcl( long double x )
{
    if ( is_nan( x ) ) {
        return x;
    }
    if ( is_inf( x ) ) {
        return x < 0.0L ? 2.0L : 0.0L;
    }
    if ( x < 0.5L ) {
        return x <= -2.0L ? 2.0L - erfc_fraction( -x ) : 1.0L - erf_series( x );
    }
    if ( x > 110.0L ) {
        return __pm_range( 0.0L );
    }
    return erfc_fraction( x );
}

WRAP1( erf )
WRAPR1( erfc )

/*--- the gamma functions ---------------------------------------------*/

/* ln Gamma(z) for z >= 20, Stirling's series */
static long double stirling( long double z )
{
    long double r = 1.0L / z;
    long double r2 = r * r;
    long double series;

    series = r * (1.0L / 12.0L
           + r2 * (-1.0L / 360.0L
           + r2 * (1.0L / 1260.0L
           + r2 * (-1.0L / 1680.0L
           + r2 * (1.0L / 1188.0L
           + r2 * (-691.0L / 360360.0L
           + r2 * (1.0L / 156.0L
           + r2 * (-3617.0L / 122400.0L))))))));
    return (z - 0.5L) * logl( z ) - z + LN_SQRT_2PI + series;
}

/* sin(pi x) without losing x's last bits to the multiplication: pi
   times the distance to the nearest whole number */
static long double sin_pi( long double x )
{
    long double n = __pm_xrndint( x, RND_NEAR );
    long double s = __pm_xsin( PI * (x - n) );

    return __pm_xprem( n, 2.0L ) != 0.0L ? -s : s;
}

/* (-1)^k (zeta(k) - 1) / k for k = 2 to 34; the terms they make are
   below 2^-70 of the sum from there on */
static const long double lg2_coef[] = {
     0.3224670334241132182362076L,     -0.06735230105319809513324605L,
     0.02058080842778454787900092L,    -0.007385551028673985266273097L,
     0.002890510330741523285752988L,   -0.001192753911703260977113936L,
     0.0005096695247430424223356548L,  -0.0002231547584535793797614188L,
     0.00009945751278180853371459589L, -0.00004492623673813314170020750L,
     0.00002050721277567069155316650L, -0.000009439488275268395903987425L,
     0.000004374866789907487804181793L, -0.000002039215753801366236781901L,
     9.551412130407419832857180e-7L,   -4.492469198764566043294290e-7L,
     2.120718480555466586923136e-7L,   -1.004322482396809960872083e-7L,
     4.769810169363980565760193e-8L,   -2.271109460894316491031998e-8L,
     1.083865921489695409107492e-8L,   -5.183475041970046655121249e-9L,
     2.483674543802478317185009e-9L,   -1.192140140586091207442548e-9L,
     5.731367241678862013330195e-10L,  -2.759522885124233145178150e-10L,
     1.330476437424448948149716e-10L,  -6.422964563838100022082448e-11L,
     3.104424774732227276239216e-11L,  -1.502138408075414217093301e-11L,
     7.275974480239079662504550e-12L,  -3.527742476575915083615072e-12L,
     1.711991790559617908601084e-12L
};
#define LG2_TERMS   (sizeof( lg2_coef ) / sizeof( lg2_coef[0] ))
#define ONE_MINUS_EULER 0.4227843350984671393934879099175975689578L

/* ln Gamma(2 + t), |t| <= 1/2: (1 - Euler's constant) t + the sum of
   (-1)^k (zeta(k) - 1) / k t^k from k = 2 */
static long double lgamma2( long double t )
{
    long double r = 0.0L;
    int i;

    for ( i = (int)LG2_TERMS - 1; i >= 0; i-- ) {
        r = (r + lg2_coef[i]) * t;
    }
    return (r + ONE_MINUS_EULER) * t;
}

/* x = m + t with m whole and |t| <= 1/2; then Gamma(x) = Gamma(2 + t)
   * up / down, where one of up and down is 1.  x is below 172 */
static long double shift( long double x, long double *up, long double *down )
{
    long double m = __pm_xrndint( x, RND_NEAR );
    long double t = x - m;              /* exact */
    long double above = 1.0L;
    long double below = 1.0L;
    long double j;

    for ( j = 2.0L; j < m; j += 1.0L ) {
        above *= j + t;
    }
    for ( j = m; j < 2.0L; j += 1.0L ) {
        below *= j + t;
    }
    *up = above;
    *down = below;
    return t;
}

/* ln Gamma(x) for x > 0 */
static long double lgamma_pos( long double x )
{
    long double t, up, down;

    if ( x >= 20.0L ) {
        return stirling( x );
    }
    t = shift( x, &up, &down );
    return lgamma2( t ) + (logl( up ) - logl( down ));
}

/* Gamma(x) for x > 0 */
static long double tgamma_pos( long double x )
{
    long double t, up, down;

    if ( x >= 172.0L ) {
        if ( x > LD_MAXEXP * 0.71L ) {
            return __pm_infl();
        }
        return expl( stirling( x ) );
    }
    t = shift( x, &up, &down );
    return expl( lgamma2( t ) ) * up / down;
}

long double lgammal( long double x )
{
    long double s;

    signgam = 1;
    if ( is_nan( x ) ) {
        return x;
    }
    if ( is_inf( x ) ) {
        return __pm_infl();
    }
    if ( x <= 0.0L && __pm_xrndint( x, RND_NEAR ) == x ) {
        return __pm_range( __pm_infl() );       /* a pole */
    }
    if ( x > 0.0L ) {
        return lgamma_pos( x );
    }
    s = sin_pi( x );
    if ( s < 0.0L ) {
        signgam = -1;
        s = -s;
    }
    return logl( PI / (s * -x) ) - lgamma_pos( -x );    /* Gamma(1-x) = -x Gamma(-x) */
}

long double tgammal( long double x )
{
    long double r;

    if ( is_nan( x ) ) {
        return x;
    }
    if ( x == 0.0L ) {
        return __pm_range( __pm_signbitl( x ) ? -__pm_infl() : __pm_infl() );
    }
    if ( is_inf( x ) ) {
        return x < 0.0L ? __pm_domain() : x;
    }
    if ( x < 0.0L && __pm_xrndint( x, RND_NEAR ) == x ) {
        return __pm_domain();           /* a negative whole number */
    }
    if ( x > 0.0L ) {
        r = tgamma_pos( x );
        return is_inf( r ) ? __pm_range( r ) : r;
    }
    if ( x < -LD_MAXEXP * 0.71L ) {
        return __pm_range( 0.0L );
    }
    /* reflect: Gamma(x) = pi / (sin(pi x) Gamma(1 - x)) */
    r = tgamma_pos( 1.0L - x );
    return PI / (sin_pi( x ) * r);
}

WRAP1( lgamma )
WRAPR1( tgamma )
