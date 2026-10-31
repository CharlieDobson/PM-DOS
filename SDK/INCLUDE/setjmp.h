/*
 * SETJMP.H - non-local jumps
 */
#ifndef _SETJMP_H
#define _SETJMP_H

#include <_pmdefs.h>

typedef int jmp_buf[8];

_PM_BEGIN_C

#ifdef __GNUC__
int setjmp( jmp_buf env ) __attribute__(( __returns_twice__ ));
#else
int _PM_ASMCALL setjmp( jmp_buf env );
#endif
_PM_NORETURN void _PM_ASMCALL longjmp( jmp_buf env, int value );

_PM_END_C

#endif
