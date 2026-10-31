/*
 * SIGNAL.C - signal(), raise() and abort()
 *
 * The first handler given for SIGINT puts __pm_int23 (SETJMP.ASM /
 * SETJMP.S) on INT 23h with 25h, and it stays there: 35h gives back a
 * flat address and 25h takes an offset in the slot, so the kernel's
 * own handler could not be put back anyway, and SIG_DFL is carried
 * out here instead - the end of the program with exit code 3, which
 * is what Microsoft's library did.
 */
#include "pmlib.h"
#include <signal.h>
#include <stdlib.h>

#define NSIG    23

typedef void (*HANDLER)( int );

static HANDLER handlers[NSIG];
static int hooked;

void _PM_ASMCALL __pm_int23( void );
void _PM_ASMCALL __pm_sigint( void );

void _PM_ASMCALL __pm_sigint( void )
{
    HANDLER h = handlers[SIGINT];

    if ( h == SIG_IGN ) {
        return;
    }
    if ( h == SIG_DFL ) {
        _exit( 3 );
    }
    h( SIGINT );
}

HANDLER signal( int sig, HANDLER func )
{
    HANDLER old;
    pm_regs regs;

    if ( sig <= 0 || sig >= NSIG || func == SIG_ERR ) {
        errno = EINVAL;
        return SIG_ERR;
    }
    old = handlers[sig];
    handlers[sig] = func;
    if ( (sig == SIGINT || sig == SIGBREAK) && !hooked && func != SIG_DFL ) {
        regs.ebx = regs.ecx = regs.esi = regs.edi = 0;
        regs.edx = (uint32_t)__pm_int23;
        __pm_dos( &regs, 0x2523 );
        hooked = 1;
    }
    if ( sig == SIGBREAK ) {
        handlers[SIGINT] = func;        /* DOS does not tell the two apart */
    }
    return old;
}

int raise( int sig )
{
    HANDLER h;

    if ( sig <= 0 || sig >= NSIG ) {
        return -1;
    }
    h = handlers[sig];
    if ( h == SIG_IGN ) {
        return 0;
    }
    if ( h == SIG_DFL ) {
        _exit( 3 );
    }
    h( sig );
    return 0;
}

void abort( void )
{
    static const char msg[] = "Abnormal program termination\r\n";
    pm_regs regs = { 0 };

    if ( handlers[SIGABRT] != SIG_DFL && handlers[SIGABRT] != SIG_IGN ) {
        handlers[SIGABRT]( SIGABRT );   /* it may not come back */
    }
    regs.ebx = 2;
    regs.ecx = sizeof( msg ) - 1;
    regs.edx = (uint32_t)msg;
    __pm_dos( &regs, 0x4000 );
    _exit( 3 );
}
