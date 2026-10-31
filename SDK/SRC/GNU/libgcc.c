/*
 * LIBGCC.C - the rest of what GCC and Clang call for i386 code
 *
 * Unsigned 64-bit integers to and from floating point, and a few bit
 * operations the compilers do not always open-code.  DIVDI.S has the
 * division.  Every name here is one the compilers emit calls to; none
 * is for programs.
 *
 * The conversions go through the x87's signed 64-bit load and store,
 * which the compilers do inline, and correct for the top bit.  An
 * unsigned value of 2^63 or more read as signed is 2^64 too small, and
 * a long double holds that sum exactly, so the result is rounded once.
 */
typedef unsigned long long  u64;
typedef long long           s64;

#define TWO63   9223372036854775808.0L
#define TWO64   18446744073709551616.0L

u64 __fixunsxfdi( long double a );
u64 __fixunsdfdi( double a );
u64 __fixunssfdi( float a );
long double __floatundixf( u64 a );
double __floatundidf( u64 a );
float __floatundisf( u64 a );
int __popcountsi2( unsigned a );
int __popcountdi2( u64 a );
int __clzsi2( unsigned a );
int __ctzsi2( unsigned a );
int __clzdi2( u64 a );
int __ctzdi2( u64 a );
unsigned __bswapsi2( unsigned a );
u64 __bswapdi2( u64 a );

u64 __fixunsxfdi( long double a )
{
    if ( a >= TWO63 ) {
        return (u64)(s64)(a - TWO63) ^ 0x8000000000000000ULL;
    }
    return (u64)(s64)a;
}

u64 __fixunsdfdi( double a )
{
    return __fixunsxfdi( a );
}

u64 __fixunssfdi( float a )
{
    return __fixunsxfdi( a );
}

long double __floatundixf( u64 a )
{
    long double r = (long double)(s64)a;

    if ( (s64)a < 0 ) {
        r += TWO64;
    }
    return r;
}

double __floatundidf( u64 a )
{
    return (double)__floatundixf( a );
}

float __floatundisf( u64 a )
{
    return (float)__floatundixf( a );
}

int __popcountsi2( unsigned a )
{
    a = a - ((a >> 1) & 0x55555555U);
    a = (a & 0x33333333U) + ((a >> 2) & 0x33333333U);
    a = (a + (a >> 4)) & 0x0F0F0F0FU;
    return (int)((a * 0x01010101U) >> 24);
}

int __popcountdi2( u64 a )
{
    return __popcountsi2( (unsigned)a ) + __popcountsi2( (unsigned)(a >> 32) );
}

int __clzsi2( unsigned a )
{
    int n = 0;

    if ( a == 0 ) {
        return 32;
    }
    while ( !(a & 0x80000000U) ) {
        a <<= 1;
        n++;
    }
    return n;
}

int __ctzsi2( unsigned a )
{
    int n = 0;

    if ( a == 0 ) {
        return 32;
    }
    while ( !(a & 1) ) {
        a >>= 1;
        n++;
    }
    return n;
}

int __clzdi2( u64 a )
{
    unsigned high = (unsigned)(a >> 32);

    return high ? __clzsi2( high ) : 32 + __clzsi2( (unsigned)a );
}

int __ctzdi2( u64 a )
{
    unsigned low = (unsigned)a;

    return low ? __ctzsi2( low ) : 32 + __ctzsi2( (unsigned)(a >> 32) );
}

unsigned __bswapsi2( unsigned a )
{
    return (a >> 24) | ((a >> 8) & 0xFF00U) | ((a << 8) & 0xFF0000U) | (a << 24);
}

u64 __bswapdi2( u64 a )
{
    return ((u64)__bswapsi2( (unsigned)a ) << 32) | __bswapsi2( (unsigned)(a >> 32) );
}
