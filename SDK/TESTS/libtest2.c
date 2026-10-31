/*
 * LIBTEST2.C - the SDK's C library, second part: strtod, scanf,
 * math.h, time.h, setjmp, signal, inttypes
 *
 * Lines that start "D " are strtod results, "M " math results, both
 * printed with %.17g, which gives a double back exactly; TESTS\
 * MATHREF.PL works out what each should be and says how many units in
 * the last place each is out.  "FAIL" lines are checks made here.  The
 * last line is "END <failures>".
 *
 * Open Watcom compares floating-point values without looking at the
 * x87's "unordered" answer, so x != x is false for a NaN there, and it
 * folds the constant -0.0 to 0.0: the checks use isnan() and a zero
 * negated at run time, which work with every compiler.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <locale.h>
#include <inttypes.h>
#include <float.h>
#include <limits.h>

static int failures;
static volatile double zero = 0.0;      /* -zero is -0.0 at run time */

#define CHECK( cond ) \
    do { if ( !(cond) ) { printf( "FAIL line %d: %s\n", __LINE__, #cond ); failures++; } } while ( 0 )

static const char *dec_inputs[] = {
    "0.1", "0.3", "1e23", "8.98846567431158e307", "2.2250738585072011e-308",
    "2.2250738585072012e-308", "4.9e-324", "2.4703282292062327e-324",
    "2.4703282292062328e-324", "1.7976931348623157e308", "1.7976931348623158e308",
    "123456789012345678901234567890", "9007199254740993",
    "9007199254740993.0000000000000000000001", "0x1.fffffffffffffp1023", "0x1p-1074",
    "0x1.000000000000080000000001p0", "0x1.0000000000000800p0", "1e-400", "-0", "3.14159265358979323846264338327950288",
    "0.000000000000000000000000000000000000000000001e45", "100000000000000000000000",
    "7.2057594037927933e16", "1.00000000000000011102230246251565404236316680908203125",
    "1.00000000000000011102230246251565404236316680908203124",
    "1.00000000000000011102230246251565404236316680908203126",
    "5e-324", "1e308", "1.5e-323", "123.456e-2", "1e0", ".5", "5."
};

static void strtod_tests( void )
{
    char *end;
    char longnum[1200];
    size_t i;
    double d;
    float f;

    for ( i = 0; i < sizeof( dec_inputs ) / sizeof( dec_inputs[0] ); i++ ) {
        d = strtod( dec_inputs[i], &end );
        printf( "D %s %.17g\n", dec_inputs[i], d );
        CHECK( *end == '\0' );
    }
    /* a 1 at the 900th decimal place: past every double, gone to zero */
    memset( longnum, '0', 1000 );
    memcpy( longnum, "0.", 2 );
    longnum[900] = '1';
    longnum[901] = '\0';
    errno = 0;
    CHECK( strtod( longnum, NULL ) == 0.0 && errno == ERANGE );
    errno = 0;
    CHECK( strtod( "1e400", &end ) == HUGE_VAL && errno == ERANGE && *end == '\0' );
    CHECK( strtod( "-inf", NULL ) == -HUGE_VAL && strtod( "INFINITY", NULL ) == HUGE_VAL );
    d = strtod( "nan(123)", &end );
    CHECK( isnan( d ) && *end == '\0' );    /* not d != d: see the note at the top */
    d = strtod( "  +.5e1xyz", &end );
    CHECK( d == 5.0 && strcmp( end, "xyz" ) == 0 );
    d = strtod( "1e+", &end );
    CHECK( d == 1.0 && strcmp( end, "e+" ) == 0 );
    d = strtod( "abc", &end );
    CHECK( d == 0.0 && strcmp( end, "abc" ) == 0 );
    f = strtof( "16777217", NULL );
    CHECK( f == 16777216.0f );
    f = strtof( "16777219", NULL );
    CHECK( f == 16777220.0f );
    f = strtof( "3.4028235e38", NULL );
    CHECK( f == FLT_MAX );
    CHECK( strtof( "1e-46", NULL ) == 0.0f );
    CHECK( atof( "2.5" ) == 2.5 );
    CHECK( strtold( "0.5", NULL ) == 0.5L );
}

static void scanf_tests( void )
{
    int a = 0, b = 0, c = 0, d = 0, n = 0, got;
    char word[32], set[32], ch = 0;
    double x = 0;
    float y = 0;
    unsigned u = 0;
    long long ll = 0;
    short sh = 0;

    got = sscanf( "  42 0x1F 017 -9 hello Zabc123 3.25 1e-2 rest",
                  "%d %x %o %i %31s %c%31[a-z]%*d %lf %f%n",
                  &a, &b, &c, &d, word, &ch, set, &x, &y, &n );
    CHECK( got == 9 );
    CHECK( a == 42 && b == 31 && c == 15 && d == -9 );
    CHECK( strcmp( word, "hello" ) == 0 && ch == 'Z' && strcmp( set, "abc" ) == 0 );
    CHECK( x == 3.25 && y == 0.01f && n == 40 );
    got = sscanf( "18446744073709551615 -5 70000", "%llu %u %hd", (unsigned long long *)&ll, &u, &sh );
    CHECK( got == 3 && (unsigned long long)ll == 18446744073709551615ULL && u == 0xFFFFFFFBU );
    CHECK( sscanf( "", "%d", &a ) == EOF );
    CHECK( sscanf( "x", "%d", &a ) == 0 );
    CHECK( sscanf( "12abc", "%d%s", &a, word ) == 2 && a == 12 && strcmp( word, "abc" ) == 0 );
    CHECK( sscanf( "100%", "%d%%", &a ) == 1 && a == 100 );
    CHECK( sscanf( "a]b", "%[]a]", set ) == 1 && strcmp( set, "a]" ) == 0 );
    CHECK( sscanf( "abc def", "%[^ ]", set ) == 1 && strcmp( set, "abc" ) == 0 );
    CHECK( sscanf( "0x1.8p1 inf", "%lf %lf", &x, &x ) == 2 && x == HUGE_VAL );
    CHECK( sscanf( "12345", "%3d%d", &a, &b ) == 2 && a == 123 && b == 45 );
}

static void m1( const char *name, double arg, double r )
{
    printf( "M %s %.17g %.17g\n", name, arg, r );
}

static void m2( const char *name, double a1, double a2, double r )
{
    printf( "M %s %.17g,%.17g %.17g\n", name, a1, a2, r );
}

static void math_tests( void )
{
    static const double args[] = { 0.5, 1.0, 2.0, 3.0, 10.0, 0.001, 100.0, 1e-8, 7.5, 0.75, 1e5, -2.5 };
    size_t i;
    double x;
    int e;

    for ( i = 0; i < sizeof( args ) / sizeof( args[0] ); i++ ) {
        x = args[i];
        m1( "sin", x, sin( x ) );
        m1( "cos", x, cos( x ) );
        m1( "tan", x, tan( x ) );
        m1( "atan", x, atan( x ) );
        m1( "exp", x, exp( x ) );
        m1( "sinh", x, sinh( x ) );
        m1( "cosh", x, cosh( x ) );
        m1( "tanh", x, tanh( x ) );
        m1( "asinh", x, asinh( x ) );
        m1( "cbrt", x, cbrt( x ) );
        m1( "expm1", x, expm1( x ) );
        if ( x > 0 ) {
            m1( "log", x, log( x ) );
            m1( "log2", x, log2( x ) );
            m1( "log10", x, log10( x ) );
            m1( "log1p", x, log1p( x ) );
            m1( "sqrt", x, sqrt( x ) );
            m2( "pow", x, 1.7, pow( x, 1.7 ) );
            m2( "pow", x, -3.0, pow( x, -3.0 ) );
        }
        if ( x >= 1.0 ) {
            m1( "acosh", x, acosh( x ) );
        }
        if ( x < 1.0 && x > -1.0 ) {
            m1( "asin", x, asin( x ) );
            m1( "acos", x, acos( x ) );
            m1( "atanh", x, atanh( x ) );
        }
        m2( "atan2", x, -1.5, atan2( x, -1.5 ) );
        m2( "hypot", x, 3.0, hypot( x, 3.0 ) );
        m1( "erf", x, erf( x ) );
        m1( "erfc", x, erfc( x ) );
        if ( x > -2.0 && x < 100.0 ) {
            m1( "tgamma", x, tgamma( x ) );
            m1( "lgamma", x, lgamma( x ) );
        }
    }
    m1( "exp", 700.0, exp( 700.0 ) );
    m1( "exp", -700.0, exp( -700.0 ) );
    m1( "sin", 1e10, sin( 1e10 ) );
    m1( "cos", 1e10, cos( 1e10 ) );
    m1( "cos", 1600001.0, cos( 1600001.0 ) );
    m1( "sin", -1e15, sin( -1e15 ) );
    m1( "sin", 1e22, sin( 1e22 ) );
    m1( "tan", 1e100, tan( 1e100 ) );
    m1( "cos", 1e300, cos( 1e300 ) );
    m1( "sin", DBL_MAX, sin( DBL_MAX ) );
    m1( "cos", -DBL_MAX, cos( -DBL_MAX ) );
    /* the double nearest a multiple of pi/2: 6381956970095103 * 2^797 */
    m1( "sin", 5.3193726483265414e255, sin( 5.3193726483265414e255 ) );
    m1( "cos", 5.3193726483265414e255, cos( 5.3193726483265414e255 ) );
    m1( "erfc", 5.0, erfc( 5.0 ) );
    m1( "erfc", 20.0, erfc( 20.0 ) );
    m1( "tgamma", 170.5, tgamma( 170.5 ) );
    m1( "tgamma", 171.5, tgamma( 171.5 ) );
    m1( "tgamma", 1.25, tgamma( 1.25 ) );
    m1( "tgamma", 25.5, tgamma( 25.5 ) );
    m1( "lgamma", 19.5, lgamma( 19.5 ) );
    m1( "lgamma", 20.5, lgamma( 20.5 ) );
    m1( "lgamma", 1.25, lgamma( 1.25 ) );
    m1( "erfc", 0.6, erfc( 0.6 ) );
    m1( "erfc", 1.5, erfc( 1.5 ) );
    m1( "tgamma", -1.5, tgamma( -1.5 ) );
    m2( "pow", 2.0, 0.5, pow( 2.0, 0.5 ) );
    m2( "pow", 10.0, 22.0, pow( 10.0, 22.0 ) );

    /* exact ones, checked here */
    CHECK( sqrt( 16.0 ) == 4.0 && cbrt( 27.0 ) == 3.0 && cbrt( -8.0 ) == -2.0 );
    CHECK( hypot( 3.0, 4.0 ) == 5.0 && pow( -2.0, 3.0 ) == -8.0 && pow( 2.0, 10.0 ) == 1024.0 );
    CHECK( pow( 10.0, -3.0 ) == 0.001 && pow( 0.0, 0.0 ) == 1.0 && pow( 1.0, HUGE_VAL ) == 1.0 );
    CHECK( floor( -2.5 ) == -3.0 && ceil( -2.5 ) == -2.0 && trunc( -2.5 ) == -2.0 );
    CHECK( round( 2.5 ) == 3.0 && round( -2.5 ) == -3.0 && rint( 2.5 ) == 2.0 && rint( 3.5 ) == 4.0 );
    CHECK( round( 0.49999999999999994 ) == 0.0 && lround( 1.5 ) == 2 && llround( -1.5 ) == -2 );
    CHECK( fmod( 7.5, 2.0 ) == 1.5 && fmod( -7.5, 2.0 ) == -1.5 && remainder( 7.5, 2.0 ) == -0.5 );
    CHECK( frexp( 8.0, &e ) == 0.5 && e == 4 && ldexp( 0.5, 4 ) == 8.0 );
    CHECK( frexp( 4.9406564584124654e-324, &e ) == 0.5 && e == -1073 );
    CHECK( ilogb( 1024.0 ) == 10 && logb( 0.25 ) == -2.0 );
    CHECK( modf( -3.75, &x ) == -0.75 && x == -3.0 );
    CHECK( nextafter( 1.0, 2.0 ) == 1.0 + DBL_EPSILON && nextafter( 0.0, -1.0 ) == -4.9406564584124654e-324 );
    CHECK( copysign( 3.0, -zero ) == -3.0 && signbit( -zero ) && !signbit( zero ) );
    CHECK( fmax( 1.0, NAN ) == 1.0 && fmin( NAN, 2.0 ) == 2.0 && fdim( 5.0, 3.0 ) == 2.0 );
    CHECK( isnan( NAN ) && isinf( HUGE_VAL ) && isfinite( 1.0 ) && !isfinite( HUGE_VAL ) );
    CHECK( fpclassify( 0.0 ) == FP_ZERO && fpclassify( 1e-310 ) == FP_SUBNORMAL && isnormal( 1.0 ) );
    CHECK( fpclassify( 1.0f ) == FP_NORMAL && fpclassify( 1e-40f ) == FP_SUBNORMAL );
    CHECK( tgamma( 5.0 ) == 24.0 && tgamma( 1.0 ) == 1.0 && lgamma( 1.0 ) == 0.0 );
    errno = 0;
    CHECK( isnan( sqrt( -1.0 ) ) && errno == EDOM );
    errno = 0;
    CHECK( isnan( log( -1.0 ) ) && errno == EDOM );
    errno = 0;
    CHECK( log( 0.0 ) == -HUGE_VAL && errno == ERANGE );
    errno = 0;
    CHECK( exp( 800.0 ) == HUGE_VAL && errno == ERANGE );
    errno = 0;
    CHECK( exp( -800.0 ) == 0.0 && errno == ERANGE );
    errno = 0;
    CHECK( pow( 10.0, 400.0 ) == HUGE_VAL && errno == ERANGE );
    errno = 0;
    CHECK( isnan( pow( -8.0, 1.0 / 3.0 ) ) && errno == EDOM );
    CHECK( sinf( 0.5f ) > 0.479425f && sinf( 0.5f ) < 0.479426f && fabsf( -2.0f ) == 2.0f );
    CHECK( sqrtl( 2.0L ) * sqrtl( 2.0L ) > 1.9999999L );
    {
        int q;
        double r = remquo( 10.0, 3.0, &q );

        CHECK( r == 1.0 && (q & 7) == 3 );
        r = remquo( -10.0, 3.0, &q );
        CHECK( r == -1.0 && q < 0 && ((-q) & 7) == 3 );
    }
}

static void time_tests( void )
{
    static char tz_est[] = "TZ=EST5EDT";
    static char tz_cet[] = "TZ=CET-1CEST,M3.5.0,M10.5.0/3";
    static char tz_off[] = "TZ=";
    time_t t = 0;
    struct tm *tm;
    struct tm build;
    char buf[200];
    time_t now;

    tm = gmtime( &t );
    CHECK( tm->tm_year == 70 && tm->tm_mon == 0 && tm->tm_mday == 1 && tm->tm_wday == 4 );
    t = 951782400L;                     /* 2000-02-29 00:00 UTC */
    tm = gmtime( &t );
    CHECK( tm->tm_year == 100 && tm->tm_mon == 1 && tm->tm_mday == 29 && tm->tm_yday == 59 );
    strftime( buf, sizeof( buf ), "%a %A %b %B %d %e %H %I %j %m %M %p %S %u %w %y %Y %C %D %F %T %R %V %G %g %U %W %%", tm );
    printf( "T %s\n", buf );
    CHECK( strcmp( asctime( tm ), "Tue Feb 29 00:00:00 2000\n" ) == 0 );

    memset( &build, 0, sizeof( build ) );
    build.tm_year = 121;                /* 2021-01-03: ISO week 53 of 2020 */
    build.tm_mon = 0;
    build.tm_mday = 3;
    build.tm_isdst = -1;
    t = mktime( &build );
    strftime( buf, sizeof( buf ), "%G-W%V-%u %a %j", &build );
    CHECK( strcmp( buf, "2020-W53-7 Sun 003" ) == 0 );
    build.tm_year = 126;                /* 2026-12-31: week 53 of 2026 */
    build.tm_mon = 11;
    build.tm_mday = 31;
    mktime( &build );
    strftime( buf, sizeof( buf ), "%G-W%V", &build );
    CHECK( strcmp( buf, "2026-W53" ) == 0 );
    build.tm_year = 99;                 /* months past the end carry */
    build.tm_mon = 13;
    build.tm_mday = 1;
    mktime( &build );
    CHECK( build.tm_year == 100 && build.tm_mon == 1 );

    putenv( tz_est );
    tzset();
    CHECK( timezone == 18000 && daylight == 1 && strcmp( tzname[0], "EST" ) == 0 );
    t = 1625140800L;                    /* 2021-07-01 12:00 UTC */
    tm = localtime( &t );
    CHECK( tm->tm_hour == 8 && tm->tm_isdst == 1 );
    strftime( buf, sizeof( buf ), "%H:%M %Z %z", tm );
    CHECK( strcmp( buf, "08:00 EDT -0400" ) == 0 );
    t = 1609502400L;                    /* 2021-01-01 12:00 UTC */
    tm = localtime( &t );
    CHECK( tm->tm_hour == 7 && tm->tm_isdst == 0 );
    build = *tm;
    CHECK( mktime( &build ) == 1609502400L );
    t = 1615705200L;                    /* 2021-03-14 07:00 UTC = 03:00 EDT, just after the change */
    tm = localtime( &t );
    CHECK( tm->tm_hour == 3 && tm->tm_isdst == 1 );
    putenv( tz_cet );
    tzset();
    t = 1625140800L;
    tm = localtime( &t );
    CHECK( tm->tm_hour == 14 && tm->tm_isdst == 1 );
    putenv( tz_off );
    tzset();
    CHECK( timezone == 0 && daylight == 0 );

    now = time( NULL );
    CHECK( now > 1700000000L );
    CHECK( clock() >= 0 );
    CHECK( difftime( 10, 4 ) == 6.0 );
}

static jmp_buf jb;
static int depth;

static void jumper( int n )
{
    if ( n < 0 ) {
        return;                         /* never: n starts at 5 */
    }
    if ( n == 0 ) {
        longjmp( jb, 0 );               /* comes back as 1 */
    }
    depth++;
    jumper( n - 1 );
}

static volatile sig_atomic_t got_signal;

static void on_signal( int sig )
{
    got_signal = sig;
}

static void misc_tests( void )
{
    volatile int count = 0;
    int r;
    char buf[64];

    r = setjmp( jb );
    count++;
    if ( r == 0 ) {
        jumper( 5 );
    }
    CHECK( r == 1 && count == 2 && depth == 5 );

    CHECK( signal( SIGTERM, on_signal ) == SIG_DFL );
    CHECK( raise( SIGTERM ) == 0 && got_signal == SIGTERM );
    CHECK( signal( SIGTERM, SIG_IGN ) == on_signal );
    got_signal = 0;
    CHECK( raise( SIGTERM ) == 0 && got_signal == 0 );
    CHECK( signal( 99, on_signal ) == SIG_ERR );
    CHECK( signal( SIGINT, on_signal ) == SIG_DFL );    /* hooks INT 23h */
    CHECK( signal( SIGINT, SIG_DFL ) == on_signal );

    CHECK( strcmp( setlocale( LC_ALL, NULL ), "C" ) == 0 && setlocale( LC_ALL, "fr_FR" ) == NULL );
    CHECK( strcmp( localeconv()->decimal_point, "." ) == 0 );

    sprintf( buf, "%" PRId64 " %" PRIu32 " %" PRIxPTR, (int64_t)-5, (uint32_t)7, (uintptr_t)255 );
    CHECK( strcmp( buf, "-5 7 ff" ) == 0 );
    CHECK( strtoimax( "-123", NULL, 10 ) == -123 && imaxabs( -4 ) == 4 );
}

#if INT32_MAX != 2147483647L || UINT64_MAX != 18446744073709551615ULL
#error The limits in STDINT.H are wrong
#endif

int main( void )
{
    strtod_tests();
    scanf_tests();
    math_tests();
    time_tests();
    misc_tests();
    printf( "END %d\n", failures );
    return failures ? 1 : 0;
}
