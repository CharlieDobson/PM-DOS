/*
 * MATHX.H - what the MATH*.C modules share
 *
 * Every function is written once, in long double, and the double and
 * float versions call it.  WRAP1 and WRAP2 make those for functions
 * whose result always fits; WRAPR1 and WRAPR2 also set ERANGE when a
 * long double result that is finite becomes an infinity in the
 * narrower type, or a result that is not zero becomes zero - exp(800)
 * is a long double, but not a double.
 */
#ifndef MATHX_H
#define MATHX_H

#include <math.h>
#include <errno.h>
#include <stdint.h>

/* the x87 instructions: GNU\X87.S, WATCOM\X87.ASM */
long double _PM_ASMCALL __pm_xsqrt( long double x );
long double _PM_ASMCALL __pm_xsin( long double x );
long double _PM_ASMCALL __pm_xcos( long double x );
long double _PM_ASMCALL __pm_xtan( long double x );
long double _PM_ASMCALL __pm_xtrigq( long double a, long double b, int op );
long double _PM_ASMCALL __pm_xatan2( long double y, long double x );
long double _PM_ASMCALL __pm_xyl2x( long double x, long double y );
long double _PM_ASMCALL __pm_xyl2xp1( long double x, long double y );
long double _PM_ASMCALL __pm_x2xm1( long double x );
long double _PM_ASMCALL __pm_xpow( long double x, long double y );
long double _PM_ASMCALL __pm_xscale( long double x, long double n );
long double _PM_ASMCALL __pm_xrndint( long double x, int mode );
long double _PM_ASMCALL __pm_xprem( long double x, long double y );
long double _PM_ASMCALL __pm_xprem1( long double x, long double y );

#define RND_NEAR    0
#define RND_DOWN    1
#define RND_UP      2
#define RND_ZERO    3

#if defined( __SIZEOF_LONG_DOUBLE__ ) && __SIZEOF_LONG_DOUBLE__ > 8
#define X87_LONG_DOUBLE 1
#define LD_MANT     64
#define LD_MAXEXP   16384
#define LD_MINEXP   (-16381)
#else
#define X87_LONG_DOUBLE 0
#define LD_MANT     53
#define LD_MAXEXP   1024
#define LD_MINEXP   (-1021)
#endif

#define LN2         0.693147180559945309417232121458176568L
#define LOG2E       1.442695040888963407359924681001892137L
#define LOG10_2     0.301029995663981195213738894724493027L
#define PI          3.141592653589793238462643383279502884L
#define PI_2        1.570796326794896619231321691639751442L

/* errno and the value to return */
long double __pm_domain( void );                /* EDOM, NaN */
long double __pm_range( long double r );        /* ERANGE, r */
long double __pm_nanl( void );
long double __pm_infl( void );

/* the range checks of the narrower types */
double __pm_to_double( long double r );
float __pm_to_float( long double r );

#define WRAP1( name ) \
    double name( double x ) { return (double)name ## l( x ); } \
    float name ## f( float x ) { return (float)name ## l( x ); }
#define WRAP2( name ) \
    double name( double x, double y ) { return (double)name ## l( x, y ); } \
    float name ## f( float x, float y ) { return (float)name ## l( x, y ); }
#define WRAPR1( name ) \
    double name( double x ) { return __pm_to_double( name ## l( x ) ); } \
    float name ## f( float x ) { return __pm_to_float( name ## l( x ) ); }
#define WRAPR2( name ) \
    double name( double x, double y ) { return __pm_to_double( name ## l( x, y ) ); } \
    float name ## f( float x, float y ) { return __pm_to_float( name ## l( x, y ) ); }

#endif
