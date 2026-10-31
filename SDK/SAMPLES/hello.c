/*
 * HELLO.C - the smallest PM-DOS program worth building: it says which
 * compiler made it, and what it was given on its command line.
 */
#include <stdio.h>
#include <pmdos.h>

#if defined( __WATCOMC__ )
#define COMPILER    "Open Watcom"
#elif defined( __DJGPP__ )
#define COMPILER    "DJGPP"
#elif defined( __clang__ )
#define COMPILER    "Clang"
#else
#define COMPILER    "GCC"
#endif

int main( int argc, char **argv )
{
    int i;
    int ver = pm_version();

    printf( "Hello from PM-DOS %d.%02d, built with %s.\n", ver / 100, ver % 100, COMPILER );
    for ( i = 1; i < argc; i++ ) {
        printf( "Argument %d: \"%s\"\n", i, argv[i] );
    }
    return 0;
}
