/*
 * STDARG.H - variable argument lists
 *
 * The GNU compilers have builtins for this.  Open Watcom passes every
 * argument of a variadic function on the stack, a dword or more each,
 * whatever -3r or -3s says, so va_list there is a pointer that walks
 * up the stack.
 */
#ifndef _STDARG_H
#define _STDARG_H

#ifdef __GNUC__

typedef __builtin_va_list va_list;
#define va_start( ap, last )    __builtin_va_start( ap, last )
#define va_arg( ap, type )      __builtin_va_arg( ap, type )
#define va_end( ap )            __builtin_va_end( ap )
#define va_copy( dst, src )     __builtin_va_copy( dst, src )

#else

typedef char *va_list[1];
#define __PM_VASIZE( type )     ((sizeof( type ) + sizeof( int ) - 1) & ~(sizeof( int ) - 1))
#define va_start( ap, last )    ((ap)[0] = (char *)&(last) + __PM_VASIZE( last ), (void)0)
#define va_arg( ap, type )      ((ap)[0] += __PM_VASIZE( type ), \
                                 *(type *)((ap)[0] - __PM_VASIZE( type )))
#define va_end( ap )            ((ap)[0] = 0, (void)0)
#define va_copy( dst, src )     ((dst)[0] = (src)[0], (void)0)

#endif

#endif
