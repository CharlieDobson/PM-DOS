/*
 * ASSERT.C - what a failed assert() does: say so on stderr and abort()
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void __pm_assert( const char *expr, const char *file, int line )
{
    fflush( stdout );
    fprintf( stderr, "Assertion failed: %s, file %s, line %d\n", expr, file, line );
    abort();
}
