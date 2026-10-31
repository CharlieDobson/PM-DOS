/*
 * MATH.H - mathematics
 *
 * The C99 set, in double, float (the f names) and long double (the l
 * names).  They are built on the x87's own instructions, so a program
 * that uses them needs a coprocessor: a 486DX, or a 386 or 486SX with
 * a 387 or 487 beside it.  PM-DOS has no emulator for native programs.
 *
 * Errors are reported in errno (math_errhandling is MATH_ERRNO): EDOM
 * for an argument outside the function's domain, ERANGE for a result
 * too large or too small.
 *
 * Under Open Watcom long double is the same as double.
 */
#ifndef _MATH_H
#define _MATH_H

#include <_pmdefs.h>

#define HUGE_VAL        (__pm_inf())
#define HUGE_VALF       ((float)__pm_inf())
#define HUGE_VALL       ((long double)__pm_inf())
#define INFINITY        ((float)__pm_inf())
#define NAN             ((float)__pm_nan())

#define FP_NAN          0
#define FP_INFINITE     1
#define FP_ZERO         2
#define FP_SUBNORMAL    3
#define FP_NORMAL       4

#define FP_ILOGB0       (-2147483647 - 1)
#define FP_ILOGBNAN     (-2147483647 - 1)

#define MATH_ERRNO      1
#define MATH_ERREXCEPT  2
#define math_errhandling MATH_ERRNO

typedef double          double_t;
typedef double          float_t;

/* not C, but every DOS compiler had them */
#define M_E             2.7182818284590452354
#define M_LOG2E         1.4426950408889634074
#define M_LOG10E        0.43429448190325182765
#define M_LN2           0.69314718055994530942
#define M_LN10          2.30258509299404568402
#define M_PI            3.14159265358979323846
#define M_PI_2          1.57079632679489661923
#define M_PI_4          0.78539816339744830962
#define M_1_PI          0.31830988618379067154
#define M_2_PI          0.63661977236758134308
#define M_2_SQRTPI      1.12837916709551257390
#define M_SQRT2         1.41421356237309504880
#define M_SQRT1_2       0.70710678118654752440

_PM_BEGIN_C

double __pm_inf( void );
double __pm_nan( void );
int __pm_fpclassifyf( float x );
int __pm_fpclassify( double x );
int __pm_fpclassifyl( long double x );
int __pm_signbitf( float x );
int __pm_signbit( double x );
int __pm_signbitl( long double x );

/* the classification macros take any floating type */
#define __PM_FPTYPE( x, f, d, l ) \
    (sizeof( x ) == sizeof( float ) ? f( (float)(x) ) : \
     sizeof( x ) == sizeof( double ) ? d( (double)(x) ) : l( (long double)(x) ))
#define fpclassify( x ) __PM_FPTYPE( x, __pm_fpclassifyf, __pm_fpclassify, __pm_fpclassifyl )
#define signbit( x )    __PM_FPTYPE( x, __pm_signbitf, __pm_signbit, __pm_signbitl )
#define isnan( x )      (fpclassify( x ) == FP_NAN)
#define isinf( x )      (fpclassify( x ) == FP_INFINITE)
#define isfinite( x )   (fpclassify( x ) >= FP_ZERO)
#define isnormal( x )   (fpclassify( x ) == FP_NORMAL)

#define isunordered( x, y )     (isnan( x ) || isnan( y ))
#define isgreater( x, y )       (!isunordered( x, y ) && (x) > (y))
#define isgreaterequal( x, y )  (!isunordered( x, y ) && (x) >= (y))
#define isless( x, y )          (!isunordered( x, y ) && (x) < (y))
#define islessequal( x, y )     (!isunordered( x, y ) && (x) <= (y))
#define islessgreater( x, y )   (!isunordered( x, y ) && ((x) < (y) || (x) > (y)))

/* lgamma() puts the sign of gamma(x) here */
extern int signgam;

#define __PM_MATH1( name ) \
    double name( double x ); float name ## f( float x ); long double name ## l( long double x );
#define __PM_MATH2( name ) \
    double name( double x, double y ); float name ## f( float x, float y ); \
    long double name ## l( long double x, long double y );

__PM_MATH1( acos )   __PM_MATH1( asin )   __PM_MATH1( atan )   __PM_MATH2( atan2 )
__PM_MATH1( cos )    __PM_MATH1( sin )    __PM_MATH1( tan )
__PM_MATH1( acosh )  __PM_MATH1( asinh )  __PM_MATH1( atanh )
__PM_MATH1( cosh )   __PM_MATH1( sinh )   __PM_MATH1( tanh )
__PM_MATH1( exp )    __PM_MATH1( exp2 )   __PM_MATH1( expm1 )
__PM_MATH1( log )    __PM_MATH1( log10 )  __PM_MATH1( log1p )  __PM_MATH1( log2 )
__PM_MATH1( logb )   __PM_MATH1( cbrt )   __PM_MATH1( fabs )   __PM_MATH1( sqrt )
__PM_MATH2( hypot )  __PM_MATH2( pow )
__PM_MATH1( erf )    __PM_MATH1( erfc )   __PM_MATH1( lgamma ) __PM_MATH1( tgamma )
__PM_MATH1( ceil )   __PM_MATH1( floor )  __PM_MATH1( nearbyint )
__PM_MATH1( rint )   __PM_MATH1( round )  __PM_MATH1( trunc )
__PM_MATH2( fmod )   __PM_MATH2( remainder ) __PM_MATH2( copysign )
__PM_MATH2( nextafter ) __PM_MATH2( fdim ) __PM_MATH2( fmax )   __PM_MATH2( fmin )

double frexp( double x, int *exp );
float frexpf( float x, int *exp );
long double frexpl( long double x, int *exp );
double ldexp( double x, int exp );
float ldexpf( float x, int exp );
long double ldexpl( long double x, int exp );
double scalbn( double x, int n );
float scalbnf( float x, int n );
long double scalbnl( long double x, int n );
double scalbln( double x, long n );
float scalblnf( float x, long n );
long double scalblnl( long double x, long n );
int ilogb( double x );
int ilogbf( float x );
int ilogbl( long double x );
double modf( double x, double *ip );
float modff( float x, float *ip );
long double modfl( long double x, long double *ip );
double remquo( double x, double y, int *quo );
float remquof( float x, float y, int *quo );
long double remquol( long double x, long double y, int *quo );
double fma( double x, double y, double z );
float fmaf( float x, float y, float z );
long double fmal( long double x, long double y, long double z );
double nexttoward( double x, long double y );
float nexttowardf( float x, long double y );
long double nexttowardl( long double x, long double y );
double nan( const char *tag );
float nanf( const char *tag );
long double nanl( const char *tag );
long lrint( double x );
long lrintf( float x );
long lrintl( long double x );
long long llrint( double x );
long long llrintf( float x );
long long llrintl( long double x );
long lround( double x );
long lroundf( float x );
long lroundl( long double x );
long long llround( double x );
long long llroundf( float x );
long long llroundl( long double x );

_PM_END_C

#endif
