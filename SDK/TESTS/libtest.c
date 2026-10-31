/*
 * LIBTEST.C - the SDK's C library, checked on PM-DOS
 *
 * Every build prints the same lines, whichever compiler made it, so
 * the four outputs are compared with each other; the floating-point
 * lines are also compared with what the host's printf makes of the
 * same formats (TESTS\FLTREF.PL).  A line that starts "FAIL" is a
 * check that went wrong.  The last line is "END <failures>".
 *
 * Run it in a directory it may write in: it makes and removes
 * LT*.TMP files there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <io.h>

static int failures;
static volatile double zero = 0.0;      /* for the infinities */

#define CHECK( cond ) \
    do { if ( !(cond) ) { printf( "FAIL line %d: %s\n", __LINE__, #cond ); failures++; } } while ( 0 )

static void ints( void )
{
    char buf[64];

    printf( "[%d] [%5d] [%-5d] [%05d] [%+d] [% d] [%.3d] [%x] [%#X] [%#o] [%u]\n",
            42, 42, 42, -42, 42, 42, 7, 255, 255, 8, 4000000000U );
    printf( "[%hhd] [%hd] [%ld] [%lld] [%llu] [%llx] [%zu] [%c] [%5s] [%-5s|] [%.2s]\n",
            300, 70000, -123456789L, -9223372036854775807LL - 1, 18446744073709551615ULL,
            0x123456789ABCDEFULL, (size_t)99, 'A', "ab", "ab", "abcdef" );
    printf( "[%*d] [%-*d] [%.*s] [%%] [%.0d] [%#.0o] [%#x]\n", 6, 1, 6, 2, 3, "xyzw", 0, 0, 0 );
    CHECK( snprintf( buf, 8, "%s", "0123456789" ) == 10 );
    CHECK( strcmp( buf, "0123456" ) == 0 );
    CHECK( snprintf( NULL, 0, "%d", 12345 ) == 5 );
    CHECK( sprintf( buf, "%p", (void *)0x2000 ) == 8 && strcmp( buf, "00002000" ) == 0 );
}

static const double fvals[] = {
    0.0, 0.1, 0.5, 1.5, 2.5, 3.5, 1.0 / 3.0, 2.0 / 3.0, 123456789.125, 1e22, 1e23,
    1e300, 1e-300, 4.9406564584124654e-324, 1.7976931348623157e308, 9.5, 99.95, 0.000123456,
    123456.0, 1e-5, 1e15, 1e16, 1e17, -2.75
};
static const char *ffmts[] = {
    "%f", "%.0f", "%.3f", "%.10f", "%e", "%.0e", "%.10e", "%g", "%.3g", "%.17g", "%#g",
    "%+010.2f", "%-12.3e|", "%G", "%E", "%.20f"
};

static void floats( void )
{
    size_t v, f;

    for ( v = 0; v < sizeof( fvals ) / sizeof( fvals[0] ); v++ ) {
        for ( f = 0; f < sizeof( ffmts ) / sizeof( ffmts[0] ); f++ ) {
            if ( fvals[v] > 1e100 && (strcmp( ffmts[f], "%.10f" ) == 0 || strcmp( ffmts[f], "%.20f" ) == 0) ) {
                continue;               /* 300 digits; checked once below */
            }
            printf( "F %d %s ", (int)v, ffmts[f] );
            printf( ffmts[f], fvals[v] );
            printf( "\n" );
        }
    }
    printf( "F big %.0f\n", 1.7976931348623157e308 );
    printf( "F tiny %.330f\n", 4.9406564584124654e-324 );
    printf( "S %f %F %e %g [%5f] [%-5f] [%05f]\n", 1.0 / zero, -1.0 / zero, 1.0 / zero, -1.0 / zero,
            1.0 / zero, 1.0 / zero, 1.0 / zero );
    printf( "S %a %a %a %.3a %a %A %.0a %a\n", 1.0, 0.5, 1.5, 1.0 / 3.0, 0.0, 255.0, 1.5, -0.1 );
    printf( "S %.1f %.1f %.2f %.0f %.0f\n", 0.05, 0.15, 0.125, 0.5, 1.5 );
}

static int cmp_int( const void *a, const void *b )
{
    int x = *(const int *)a;
    int y = *(const int *)b;

    return x < y ? -1 : x > y;
}

static void strings( void )
{
    char buf[64];
    char *end;
    char *tok;
    int arr[1000];
    int i, sorted;
    unsigned long sum = 0, sum2 = 0;

    CHECK( strlen( "hello" ) == 5 );
    CHECK( strcmp( "abc", "abd" ) < 0 && strcmp( "b", "a" ) > 0 && strcmp( "x", "x" ) == 0 );
    CHECK( strncmp( "abcX", "abcY", 3 ) == 0 );
    CHECK( stricmp( "Hello", "hELLO" ) == 0 );
    CHECK( strcmp( strchr( "a.b.c", '.' ), ".b.c" ) == 0 );
    CHECK( strcmp( strrchr( "a.b.c", '.' ), ".c" ) == 0 );
    CHECK( strstr( "haystack", "st" ) != NULL && strstr( "haystack", "xy" ) == NULL );
    CHECK( strspn( "  x", " " ) == 2 && strcspn( "ab,cd", "," ) == 2 );
    strcpy( buf, "one, two,,three" );
    tok = strtok( buf, ", " );
    CHECK( tok != NULL && strcmp( tok, "one" ) == 0 );
    tok = strtok( NULL, ", " );
    CHECK( tok != NULL && strcmp( tok, "two" ) == 0 );
    tok = strtok( NULL, ", " );
    CHECK( tok != NULL && strcmp( tok, "three" ) == 0 );
    CHECK( strtok( NULL, ", " ) == NULL );
    CHECK( strtol( "  -123xyz", &end, 10 ) == -123 && *end == 'x' );
    CHECK( strtol( "0x1F", NULL, 0 ) == 31 && strtol( "017", NULL, 0 ) == 15 );
    errno = 0;
    CHECK( strtol( "99999999999", NULL, 10 ) == LONG_MAX && errno == ERANGE );
    errno = 0;
    CHECK( strtol( "-2147483648", NULL, 10 ) == LONG_MIN && errno == 0 );
    CHECK( strtoul( "4294967295", NULL, 10 ) == 4294967295UL );
    CHECK( strtoll( "-9223372036854775808", NULL, 10 ) == LLONG_MIN );
    CHECK( strtoull( "ffffffffffffffff", NULL, 16 ) == 18446744073709551615ULL );
    CHECK( strtol( "xyz", &end, 10 ) == 0 && strcmp( end, "xyz" ) == 0 );
    CHECK( atoi( "42" ) == 42 );
    CHECK( strcmp( itoa( -255, buf, 10 ), "-255" ) == 0 && strcmp( itoa( 255, buf, 16 ), "ff" ) == 0 );
    CHECK( isalpha( 'a' ) && !isalpha( '1' ) && isdigit( '7' ) && isspace( '\t' ) && !isprint( '\n' ) );
    CHECK( !isalpha( (char)0xE9 ) && !isalpha( EOF ) && toupper( 'q' ) == 'Q' );
    CHECK( strcmp( strrev( strcpy( buf, "abc" ) ), "cba" ) == 0 );
    CHECK( memcmp( "ab\0c", "ab\0d", 4 ) < 0 );
    memset( buf, 'x', 10 );
    memmove( buf + 2, buf, 5 );
    buf[10] = 0;
    CHECK( strcmp( buf, "xxxxxxxxxx" ) == 0 );
    strcpy( buf, "123456789" );
    memmove( buf + 1, buf, 8 );
    CHECK( strcmp( buf, "112345678" ) == 0 );
    memmove( buf, buf + 1, 8 );
    CHECK( strcmp( buf, "123456788" ) == 0 );

    srand( 7 );
    for ( i = 0; i < 1000; i++ ) {
        arr[i] = rand() - 16384;
        sum += (unsigned long)arr[i];
    }
    qsort( arr, 1000, sizeof( int ), cmp_int );
    sorted = 1;
    for ( i = 0; i < 1000; i++ ) {
        sum2 += (unsigned long)arr[i];
        if ( i && arr[i - 1] > arr[i] ) {
            sorted = 0;
        }
    }
    CHECK( sorted && sum == sum2 );
    CHECK( bsearch( &arr[500], arr, 1000, sizeof( int ), cmp_int ) != NULL );
    printf( "R %d %d %d\n", arr[0], arr[500], arr[999] );
}

static void heap( void )
{
    static unsigned char *blocks[200];
    static size_t sizes[200];
    unsigned seed = 12345;
    int round, i, ok = 1;
    size_t j;
    unsigned char *p;

    for ( round = 0; round < 3000; round++ ) {
        seed = seed * 1103515245U + 12345U;
        i = (int)((seed >> 16) % 200);
        if ( blocks[i] != NULL ) {
            for ( j = 0; j < sizes[i]; j++ ) {
                if ( blocks[i][j] != (unsigned char)(i + j) ) {
                    ok = 0;
                }
            }
            if ( (seed >> 8) & 1 ) {
                free( blocks[i] );
                blocks[i] = NULL;
                continue;
            }
            p = (unsigned char *)realloc( blocks[i], sizes[i] * 2 + 1 );
            CHECK( p != NULL );
            for ( j = sizes[i]; j < sizes[i] * 2 + 1; j++ ) {
                p[j] = (unsigned char)(i + j);
            }
            blocks[i] = p;
            sizes[i] = sizes[i] * 2 + 1;
            continue;
        }
        sizes[i] = (seed >> 4) % 3000 + 1;
        blocks[i] = (unsigned char *)malloc( sizes[i] );
        CHECK( blocks[i] != NULL && ((unsigned long)blocks[i] & 15) == 0 );
        for ( j = 0; j < sizes[i]; j++ ) {
            blocks[i][j] = (unsigned char)(i + j);
        }
    }
    for ( i = 0; i < 200; i++ ) {
        free( blocks[i] );
    }
    CHECK( ok );
    p = (unsigned char *)malloc( 300000 );
    CHECK( p != NULL );
    if ( p != NULL ) {
        memset( p, 0xAA, 300000 );
        CHECK( p[299999] == 0xAA );
        free( p );
    }
    p = (unsigned char *)calloc( 1000, 4 );
    CHECK( p != NULL && p[3999] == 0 );
    free( p );
    CHECK( calloc( 0x10000, 0x10000 ) == NULL && errno == ENOMEM );
    printf( "H done\n" );
}

static void files( void )
{
    FILE *fp;
    char line[80];
    unsigned char raw[32];
    size_t n;
    long pos;
    int c, fd;

    remove( "LT1.TMP" );
    remove( "LT2.TMP" );
    fp = fopen( "LT1.TMP", "w" );
    CHECK( fp != NULL );
    if ( fp == NULL ) {
        return;
    }
    fprintf( fp, "first line\nsecond %d\n", 2 );
    fputs( "third", fp );
    CHECK( fclose( fp ) == 0 );

    fp = fopen( "LT1.TMP", "rb" );
    CHECK( fp != NULL );
    n = fread( raw, 1, sizeof( raw ), fp );
    CHECK( n == 27 );
    CHECK( memcmp( raw, "first line\r\nsecond 2\r\nthird", 26 ) == 0 );
    CHECK( feof( fp ) || fgetc( fp ) == EOF );
    fclose( fp );

    fp = fopen( "LT1.TMP", "r" );
    CHECK( fgets( line, sizeof( line ), fp ) != NULL && strcmp( line, "first line\n" ) == 0 );
    pos = ftell( fp );
    CHECK( pos == 12 );
    CHECK( fgets( line, sizeof( line ), fp ) != NULL && strcmp( line, "second 2\n" ) == 0 );
    CHECK( fseek( fp, pos, SEEK_SET ) == 0 );
    c = fgetc( fp );
    CHECK( c == 's' );
    CHECK( ungetc( 'Z', fp ) == 'Z' && fgetc( fp ) == 'Z' && fgetc( fp ) == 'e' );
    CHECK( fgets( line, sizeof( line ), fp ) != NULL );
    CHECK( fgets( line, sizeof( line ), fp ) != NULL && strcmp( line, "third" ) == 0 );
    CHECK( fgets( line, sizeof( line ), fp ) == NULL && feof( fp ) );
    fclose( fp );

    fp = fopen( "LT1.TMP", "a" );
    fputs( "\nfourth\n", fp );
    fclose( fp );
    fp = fopen( "LT1.TMP", "rb" );
    fseek( fp, 0, SEEK_END );
    CHECK( ftell( fp ) == 27 + 10 );
    fclose( fp );

    CHECK( rename( "LT1.TMP", "LT2.TMP" ) == 0 );
    CHECK( fopen( "LT1.TMP", "r" ) == NULL && errno == ENOENT );
    fp = fopen( "LT2.TMP", "r+b" );
    CHECK( fp != NULL );
    fseek( fp, 6, SEEK_SET );
    fputs( "LINE", fp );
    fseek( fp, 0, SEEK_SET );
    n = fread( raw, 1, 12, fp );
    CHECK( n == 12 && memcmp( raw, "first LINE\r\n", 12 ) == 0 );
    fclose( fp );
    CHECK( remove( "LT2.TMP" ) == 0 && remove( "LT2.TMP" ) == -1 );

    fd = open( "LT3.TMP", O_RDWR | O_CREAT | O_EXCL | O_BINARY, 0666 );
    CHECK( fd >= 0 );
    CHECK( write( fd, "12345", 5 ) == 5 );
    CHECK( lseek( fd, 0, SEEK_END ) == 5 && filelength( fd ) == 5 );
    close( fd );
    CHECK( open( "LT3.TMP", O_RDWR | O_CREAT | O_EXCL, 0666 ) < 0 && errno == EEXIST );
    CHECK( unlink( "LT3.TMP" ) == 0 );

    fp = tmpfile();
    CHECK( fp != NULL );
    if ( fp != NULL ) {
        fputs( "scratch", fp );
        rewind( fp );
        CHECK( fgets( line, sizeof( line ), fp ) != NULL && strcmp( line, "scratch" ) == 0 );
        fclose( fp );
    }
    CHECK( isatty( 1 ) == 0 );          /* the test's output is a file */
    printf( "I done\n" );
}

static void env( void )
{
    static char setting[] = "SDKTEST=yes";
    static char removing[] = "SDKTEST=";

    CHECK( getenv( "SDKTEST" ) == NULL );
    CHECK( putenv( setting ) == 0 );
    CHECK( getenv( "SDKTEST" ) != NULL && strcmp( getenv( "sdktest" ), "yes" ) == 0 );
    CHECK( putenv( removing ) == 0 && getenv( "SDKTEST" ) == NULL );
    CHECK( strcmp( strerror( ENOENT ), "No such file or directory" ) == 0 );
}

static void last( void )
{
    printf( "END %d\n", failures );
}

static void second( void )
{
    printf( "atexit second\n" );
}

static void first( void )
{
    printf( "atexit first\n" );
}

int main( int argc, char **argv )
{
    (void)argv;
    atexit( last );
    atexit( first );
    atexit( second );
    printf( "argc %d\n", argc );
    ints();
    floats();
    strings();
    heap();
    files();
    env();
    return failures ? 1 : 0;
}
