/*
 * SIGNAL.H - signals
 *
 * SIGINT is Ctrl+C (and Ctrl+Break): a handler given to signal() is
 * called when DOS sees one, and when it returns the program carries on
 * where it was.  The handler stays in place afterwards.  With SIG_DFL
 * the program ends with exit code 3; with SIG_IGN Ctrl+C is ignored.
 *
 * The others come only from raise() and abort(): PM-DOS ends a native
 * program itself on a fault, a coprocessor error included.
 */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <_pmdefs.h>

typedef int sig_atomic_t;

#define SIGINT          2
#define SIGILL          4
#define SIGFPE          8
#define SIGSEGV         11
#define SIGTERM         15
#define SIGBREAK        21
#define SIGABRT         22

#define SIG_DFL         ((void (*)( int ))0)
#define SIG_IGN         ((void (*)( int ))1)
#define SIG_ERR         ((void (*)( int ))-1)

_PM_BEGIN_C

void ( *signal( int sig, void (*func)( int ) ) )( int );
int raise( int sig );

_PM_END_C

#endif
