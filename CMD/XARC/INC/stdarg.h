/*
 * STDARG.H - XARC's C library: the arguments of a function declared
 * with "...".  Open Watcom passes every argument of such a function on
 * the stack, each widened to a multiple of an int, so they follow the
 * last named one in memory.
 */
#ifndef _XA_STDARG_H
#define _XA_STDARG_H

typedef char *va_list;

#define _XA_VA_SIZE( type )     ((sizeof( type ) + sizeof( int ) - 1) & ~(sizeof( int ) - 1))
#define va_start( ap, last )    ((ap) = (char *)&(last) + _XA_VA_SIZE( last ))
#define va_arg( ap, type )      (*(type *)(((ap) += _XA_VA_SIZE( type )) - _XA_VA_SIZE( type )))
#define va_end( ap )            ((ap) = (char *)0)

#endif
