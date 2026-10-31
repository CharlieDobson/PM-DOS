/*
 * EXIT.C - exit(), atexit() and _exit() (abort() is SIGNAL.C's)
 */
#include "pmlib.h"
#include <stdlib.h>

#define ATEXIT_MAX  32                  /* C asks for at least 32 */

static void (*handlers[ATEXIT_MAX])( void );
static int count;

int atexit( void (*func)( void ) )
{
    if ( count >= ATEXIT_MAX ) {
        return -1;
    }
    handlers[count++] = func;
    return 0;
}

void _exit( int status )
{
    pm_regs regs = { 0 };

    for ( ;; ) {
        __pm_dos( &regs, 0x4C00 | ((uint32_t)status & 0xFF) );
    }
}

void _Exit( int status )
{
    _exit( status );
}

void exit( int status )
{
    /* a handler that calls exit() again carries on from the next one */
    while ( count > 0 ) {
        handlers[--count]();
    }
    if ( __pm_exit_flush != NULL ) {
        __pm_exit_flush();
    }
    _exit( status );
}
