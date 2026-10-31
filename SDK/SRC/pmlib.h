/*
 * PMLIB.H - what the SDK's library modules share with each other
 *
 * Not installed with the headers: nothing here is for programs.
 */
#ifndef PMLIB_H
#define PMLIB_H

#include <pmdos.h>
#include <stddef.h>
#include <errno.h>

/* Put eax in, clear the carry, issue INT 21h with the rest of *regs as
   the caller filled it.  Returns the carry flag. */
int __pm_dos( pm_regs *regs, uint32_t eax );

/* A failed call: record AX as _doserrno, set errno from it, and give
   back -1 so that callers can "return __pm_fail( &regs );". */
int __pm_fail( const pm_regs *regs );

/* Set errno to e and give back -1. */
int __pm_errno( int e );

/* 0 once the kernel has answered a long-name call with 7100h ("not
   supported", which is LFN=OFF in CONFIG.SYS): from then on the
   classic calls are used. */
extern int __pm_lfn;

/* A long-name call came back: if it said "not supported", switch the
   library to the classic calls and return 1, so the caller retries. */
int __pm_lfn_missing( const pm_regs *regs );

/* Run by exit() after the atexit functions: STDIO.C sets it to flush
   every stream, so that a program that never uses stdio does not
   bring stdio in. */
extern void (*__pm_exit_flush)( void );

/* START.C: the startup's C half, called by the assembly entry point */
void _PM_ASMCALL __pm_start( void );

#endif
