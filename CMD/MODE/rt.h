/*
 * RT.H - the minimal runtime MODE carries instead of a C library:
 * memory, strings and numbers.  MODE allocates nothing.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, u32 len );
void  mem_cpy( void *dst, const void *src, u32 len );

u32   str_len( const char *str );
int   str_cpyn( char *dst, const char *src, u32 max );    /* 0 = it did not fit */
int   str_icmp( const char *lhs, const char *rhs );
int   str_nicmp( const char *lhs, const char *rhs, u32 len );
int   ch_upper( int ch );

/* decimal, no sign, into at least 11 bytes; the length is returned */
u32   num_str( char *buf, u32 val );

#endif
