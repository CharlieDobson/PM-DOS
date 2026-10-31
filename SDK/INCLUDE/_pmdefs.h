/*
 * _PMDEFS.H - what every PM-DOS SDK header shares
 *
 * Not for programs to include: the standard headers include it.  It
 * names the compiler, gives the types more than one header needs
 * (size_t, wchar_t, NULL) one definition, and spells the attributes
 * the four compilers write differently.
 *
 * The compilers: Open Watcom 1.9 (__WATCOMC__), GCC for i686-elf
 * (__GNUC__), Clang for i386-none-elf (__clang__, which also defines
 * __GNUC__), and DJGPP's GCC (__DJGPP__ and __GNUC__).  What follows
 * tests __WATCOMC__ and treats everything else as GNU.
 */
#ifndef _PMDEFS_H
#define _PMDEFS_H

#if !defined( __WATCOMC__ ) && !defined( __GNUC__ )
#error The PM-DOS SDK supports Open Watcom, GCC, Clang and DJGPP.
#endif

#if !defined( __386__ ) && !defined( __i386__ )
#error PM-DOS programs are 32-bit x86 code.
#endif

#ifdef __cplusplus
#define _PM_BEGIN_C     extern "C" {
#define _PM_END_C       }
#else
#define _PM_BEGIN_C
#define _PM_END_C
#endif

/* _PM_ASMCALL is on every function written in assembly, so that one
   copy of the assembly serves both Open Watcom libraries: -3r passes
   arguments in registers and -3s on the stack, and __cdecl is the
   same in both. */
#ifdef __WATCOMC__
#define _PM_ASMCALL     __cdecl
#define _PM_NORETURN                    /* #pragma aux name aborts */
#define _PM_PRINTF( f, a )
#define _PM_INLINE      static __inline
#else
#define _PM_ASMCALL
#define _PM_NORETURN    __attribute__(( __noreturn__ ))
#define _PM_PRINTF( f, a ) __attribute__(( __format__( __printf__, f, a ) ))
#define _PM_INLINE      static __inline__
#endif

/* The GNU compilers know these types already, and their builtins
   (memcpy, strlen) use them: take theirs, or a prototype here and a
   builtin there disagree.  DJGPP's size_t is unsigned long, the
   others' unsigned int; both are 32 bits. */
#ifdef __SIZE_TYPE__
typedef __SIZE_TYPE__           __pm_size_t;
typedef __PTRDIFF_TYPE__        __pm_ptrdiff_t;
#else
typedef unsigned int            __pm_size_t;
typedef int                     __pm_ptrdiff_t;
#endif
#ifdef __WCHAR_TYPE__
typedef __WCHAR_TYPE__          __pm_wchar_t;
#else
typedef unsigned short          __pm_wchar_t;
#endif
typedef long                    __pm_off_t;
typedef long                    __pm_time_t;

#ifndef NULL
#ifdef __cplusplus
#define NULL    0
#else
#define NULL    ((void *)0)
#endif
#endif

#endif
