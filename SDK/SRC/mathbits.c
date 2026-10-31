/*
 * MATHBITS.C - the math functions that work on a number's bits:
 * classification, signs, frexp, ldexp, nextafter and their kind
 *
 * float is 1 + 8 + 23 bits, double 1 + 11 + 52; long double is the
 * x87's 80-bit format under the GNU compilers (1 + 15 bits, then a
 * 64-bit mantissa with its leading 1 in it) and double under Open
 * Watcom.
 */
#include "pmlib.h"
#include "mathx.h"
#include <string.h>

typedef unsigned long long u64;

typedef union { float v; uint32_t bits; } FBITS;
typedef union { double v; u64 bits; } DBITS;
#if X87_LONG_DOUBLE
typedef union { long double v; struct { uint32_t lo, hi; uint16_t se; } w; } LBITS;
#endif

int signgam;

long double __pm_domain( void )
{
    errno = EDOM;
    return __pm_nanl();
}

long double __pm_range( long double r )
{
    errno = ERANGE;
    return r;
}

double __pm_to_double( long double r )
{
    double d = (double)r;

    if ( (__pm_fpclassify( d ) == FP_INFINITE && __pm_fpclassifyl( r ) != FP_INFINITE)
         || (__pm_fpclassify( d ) == FP_ZERO && __pm_fpclassifyl( r ) != FP_ZERO) ) {
        errno = ERANGE;
    }
    return d;
}

float __pm_to_float( long double r )
{
    float f = (float)r;

    if ( (__pm_fpclassifyf( f ) == FP_INFINITE && __pm_fpclassifyl( r ) != FP_INFINITE)
         || (__pm_fpclassifyf( f ) == FP_ZERO && __pm_fpclassifyl( r ) != FP_ZERO) ) {
        errno = ERANGE;
    }
    return f;
}

double __pm_inf( void )
{
    DBITS u;

    u.bits = 0x7FF0000000000000ULL;
    return u.v;
}

double __pm_nan( void )
{
    DBITS u;

    u.bits = 0x7FF8000000000000ULL;
    return u.v;
}

long double __pm_infl( void )
{
    return __pm_inf();
}

long double __pm_nanl( void )
{
    return __pm_nan();
}

/*--- classification -------------------------------------------------*/

int __pm_fpclassifyf( float x )
{
    FBITS u;
    uint32_t e, m;

    u.v = x;
    e = (u.bits >> 23) & 0xFF;
    m = u.bits & 0x7FFFFF;
    if ( e == 0xFF ) {
        return m ? FP_NAN : FP_INFINITE;
    }
    if ( e == 0 ) {
        return m ? FP_SUBNORMAL : FP_ZERO;
    }
    return FP_NORMAL;
}

int __pm_fpclassify( double x )
{
    DBITS u;
    unsigned e;
    u64 m;

    u.v = x;
    e = (unsigned)(u.bits >> 52) & 0x7FF;
    m = u.bits & 0xFFFFFFFFFFFFFULL;
    if ( e == 0x7FF ) {
        return m ? FP_NAN : FP_INFINITE;
    }
    if ( e == 0 ) {
        return m ? FP_SUBNORMAL : FP_ZERO;
    }
    return FP_NORMAL;
}

int __pm_fpclassifyl( long double x )
{
#if X87_LONG_DOUBLE
    LBITS u;
    unsigned e;

    u.v = x;
    e = u.w.se & 0x7FFF;
    if ( e == 0x7FFF ) {
        return ((u.w.hi & 0x7FFFFFFFUL) || u.w.lo) ? FP_NAN : FP_INFINITE;
    }
    if ( e == 0 ) {
        return (u.w.hi || u.w.lo) ? FP_SUBNORMAL : FP_ZERO;
    }
    return FP_NORMAL;
#else
    return __pm_fpclassify( x );
#endif
}

int __pm_signbitf( float x )
{
    FBITS u;

    u.v = x;
    return (int)(u.bits >> 31);
}

int __pm_signbit( double x )
{
    DBITS u;

    u.v = x;
    return (int)(u.bits >> 63);
}

int __pm_signbitl( long double x )
{
#if X87_LONG_DOUBLE
    LBITS u;

    u.v = x;
    return (u.w.se >> 15) & 1;
#else
    return __pm_signbit( x );
#endif
}

/*--- signs -----------------------------------------------------------*/

long double fabsl( long double x )
{
    return __pm_signbitl( x ) ? -x : x;
}

long double copysignl( long double x, long double y )
{
    if ( __pm_signbitl( x ) != __pm_signbitl( y ) ) {
        return -x;
    }
    return x;
}

WRAP1( fabs )
WRAP2( copysign )

/*--- exponents -------------------------------------------------------*/

long double frexpl( long double x, int *exp )
{
    int e = 0;

    *exp = 0;
    if ( fpclassify( x ) < FP_SUBNORMAL ) {
        return x;                       /* 0, an infinity, a NaN */
    }
    if ( fpclassify( x ) == FP_SUBNORMAL ) {
        x = __pm_xscale( x, 64.0L );
        e = -64;
    }
    /* now normal: x = m * 2^n with 1 <= |m| < 2 */
    while ( fabsl( x ) >= 18446744073709551616.0L ) {
        x = __pm_xscale( x, -64.0L );
        e += 64;
    }
    while ( fabsl( x ) < 1.0L / 18446744073709551616.0L ) {
        x = __pm_xscale( x, 64.0L );
        e -= 64;
    }
    while ( fabsl( x ) >= 1.0L ) {
        x /= 2.0L;
        e++;
    }
    while ( fabsl( x ) < 0.5L ) {
        x *= 2.0L;
        e--;
    }
    *exp = e;
    return x;
}

double frexp( double x, int *exp )
{
    return (double)frexpl( x, exp );
}

float frexpf( float x, int *exp )
{
    return (float)frexpl( x, exp );
}

long double scalblnl( long double x, long n )
{
    long double r;

    if ( fpclassify( x ) < FP_SUBNORMAL ) {
        return x;
    }
    if ( n > 40000 ) {
        n = 40000;
    }
    if ( n < -40000 ) {
        n = -40000;
    }
    r = __pm_xscale( x, (long double)n );
    if ( __pm_fpclassifyl( r ) == FP_INFINITE || __pm_fpclassifyl( r ) == FP_ZERO ) {
        errno = ERANGE;
    }
    return r;
}

long double scalbnl( long double x, int n )
{
    return scalblnl( x, n );
}

long double ldexpl( long double x, int n )
{
    return scalblnl( x, n );
}

double scalbln( double x, long n )  { return __pm_to_double( scalblnl( x, n ) ); }
float scalblnf( float x, long n )   { return __pm_to_float( scalblnl( x, n ) ); }
double scalbn( double x, int n )    { return __pm_to_double( scalblnl( x, n ) ); }
float scalbnf( float x, int n )     { return __pm_to_float( scalblnl( x, n ) ); }
double ldexp( double x, int n )     { return __pm_to_double( scalblnl( x, n ) ); }
float ldexpf( float x, int n )      { return __pm_to_float( scalblnl( x, n ) ); }

int ilogbl( long double x )
{
    int e;

    if ( fpclassify( x ) == FP_ZERO ) {
        return FP_ILOGB0;
    }
    if ( fpclassify( x ) == FP_NAN ) {
        return FP_ILOGBNAN;
    }
    if ( fpclassify( x ) == FP_INFINITE ) {
        return 2147483647;
    }
    frexpl( x, &e );
    return e - 1;
}

int ilogb( double x )   { return ilogbl( x ); }
int ilogbf( float x )   { return ilogbl( x ); }

long double logbl( long double x )
{
    if ( fpclassify( x ) == FP_ZERO ) {
        return __pm_range( -__pm_infl() );
    }
    if ( fpclassify( x ) == FP_INFINITE ) {
        return __pm_infl();
    }
    if ( fpclassify( x ) == FP_NAN ) {
        return x;
    }
    return (long double)ilogbl( x );
}

WRAP1( logb )

long double modfl( long double x, long double *ip )
{
    long double t;

    if ( fpclassify( x ) == FP_INFINITE ) {
        *ip = x;
        return copysignl( 0.0L, x );
    }
    t = __pm_xrndint( x, RND_ZERO );
    *ip = t;
    return copysignl( x - t, x );       /* -3.0 gives -0.0, as C asks */
}

double modf( double x, double *ip )
{
    long double i;
    double r = (double)modfl( x, &i );

    *ip = (double)i;
    return r;
}

float modff( float x, float *ip )
{
    long double i;
    float r = (float)modfl( x, &i );

    *ip = (float)i;
    return r;
}

/*--- neighbours ------------------------------------------------------*/

float nextafterf( float x, float y )
{
    FBITS u;

    if ( __pm_fpclassifyf( x ) == FP_NAN || __pm_fpclassifyf( y ) == FP_NAN ) {
        return x + y;
    }
    if ( x == y ) {
        return y;
    }
    u.v = x;
    if ( x == 0.0f ) {
        u.bits = 1;                     /* the smallest subnormal */
        if ( y < 0.0f ) {
            u.bits |= 0x80000000UL;
        }
    } else if ( (x < y) == !(u.bits >> 31) ) {
        u.bits++;                       /* away from zero */
    } else {
        u.bits--;
    }
    if ( fpclassify( u.v ) == FP_INFINITE || fpclassify( u.v ) == FP_SUBNORMAL ) {
        errno = ERANGE;
    }
    return u.v;
}

double nextafter( double x, double y )
{
    DBITS u;

    if ( __pm_fpclassify( x ) == FP_NAN || __pm_fpclassify( y ) == FP_NAN ) {
        return x + y;
    }
    if ( x == y ) {
        return y;
    }
    u.v = x;
    if ( x == 0.0 ) {
        u.bits = 1;
        if ( y < 0.0 ) {
            u.bits |= 0x8000000000000000ULL;
        }
    } else if ( (x < y) == !(u.bits >> 63) ) {
        u.bits++;
    } else {
        u.bits--;
    }
    if ( fpclassify( u.v ) == FP_INFINITE || fpclassify( u.v ) == FP_SUBNORMAL ) {
        errno = ERANGE;
    }
    return u.v;
}

long double nextafterl( long double x, long double y )
{
#if X87_LONG_DOUBLE
    LBITS u;
    u64 m;
    unsigned e;
    int up;

    if ( __pm_fpclassifyl( x ) == FP_NAN || __pm_fpclassifyl( y ) == FP_NAN ) {
        return x + y;
    }
    if ( x == y ) {
        return y;
    }
    memset( &u, 0, sizeof( u ) );
    u.v = x;
    if ( x == 0.0L ) {
        u.w.lo = 1;
        u.w.hi = 0;
        u.w.se = (uint16_t)(y < 0.0L ? 0x8000 : 0);
        errno = ERANGE;
        return u.v;
    }
    /* the magnitude goes up when y is further from zero than x */
    up = (x < y) == !(u.w.se >> 15);
    e = u.w.se & 0x7FFF;
    m = ((u64)u.w.hi << 32) | u.w.lo;
    if ( up ) {
        m++;
        if ( m == 0 ) {                 /* past the top: 1.000... one higher */
            m = 0x8000000000000000ULL;
            e++;
        } else if ( e == 0 && m == 0x8000000000000000ULL ) {
            e = 1;                      /* subnormal to normal */
        }
    } else {
        if ( e > 0 && m == 0x8000000000000000ULL ) {
            e--;
            m = e ? 0xFFFFFFFFFFFFFFFFULL : 0x7FFFFFFFFFFFFFFFULL;
        } else {
            m--;
        }
    }
    u.w.lo = (uint32_t)m;
    u.w.hi = (uint32_t)(m >> 32);
    u.w.se = (uint16_t)((u.w.se & 0x8000) | e);
    if ( fpclassify( u.v ) == FP_INFINITE || fpclassify( u.v ) == FP_SUBNORMAL ) {
        errno = ERANGE;
    }
    return u.v;
#else
    return nextafter( x, y );
#endif
}

double nexttoward( double x, long double y )
{
    if ( (long double)x == y || __pm_fpclassifyl( y ) == FP_NAN ) {
        return (double)y;
    }
    return nextafter( x, (long double)x < y ? __pm_inf() : -__pm_inf() );
}

float nexttowardf( float x, long double y )
{
    if ( (long double)x == y || __pm_fpclassifyl( y ) == FP_NAN ) {
        return (float)y;
    }
    return nextafterf( x, (long double)x < y ? (float)__pm_inf() : -(float)__pm_inf() );
}

long double nexttowardl( long double x, long double y )
{
    return nextafterl( x, y );
}

long double nanl( const char *tag )
{
    (void)tag;
    return __pm_nanl();
}

double nan( const char *tag )
{
    (void)tag;
    return __pm_nan();
}

float nanf( const char *tag )
{
    (void)tag;
    return (float)__pm_nan();
}

/*--- the rest --------------------------------------------------------*/

long double fdiml( long double x, long double y )
{
    if ( __pm_fpclassifyl( x ) == FP_NAN || __pm_fpclassifyl( y ) == FP_NAN ) {
        return x + y;
    }
    return x > y ? x - y : 0.0L;
}

long double fmaxl( long double x, long double y )
{
    if ( __pm_fpclassifyl( x ) == FP_NAN ) {
        return y;                       /* a NaN loses to a number */
    }
    if ( __pm_fpclassifyl( y ) == FP_NAN ) {
        return x;
    }
    if ( x == y ) {
        return __pm_signbitl( x ) ? y : x;      /* +0 over -0 */
    }
    return x > y ? x : y;
}

long double fminl( long double x, long double y )
{
    if ( __pm_fpclassifyl( x ) == FP_NAN ) {
        return y;
    }
    if ( __pm_fpclassifyl( y ) == FP_NAN ) {
        return x;
    }
    if ( x == y ) {
        return __pm_signbitl( x ) ? x : y;
    }
    return x < y ? x : y;
}

WRAPR2( fdim )
WRAP2( fmax )
WRAP2( fmin )

/* x * y + z in long double: not fused, so for double the product is
   rounded to 64 bits before the add */
long double fmal( long double x, long double y, long double z )
{
    return x * y + z;
}

double fma( double x, double y, double z )
{
    return __pm_to_double( fmal( x, y, z ) );
}

float fmaf( float x, float y, float z )
{
    return (float)((double)x * y + z);  /* exact product in double */
}
