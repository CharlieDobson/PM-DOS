/*
 * RT.H - the minimal runtime MODE carries instead of a C library:
 * memory, strings and numbers.  MODE allocates nothing.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, unsigned int len );
void  mem_cpy( void *dst, const void *src, unsigned int len );

unsigned int str_len( const char *str );
int          str_cpyn( char *dst, const char *src, unsigned int max ); /* 0 = it did not fit */
int          str_icmp( const char *lhs, const char *rhs );
int          str_nicmp( const char *lhs, const char *rhs, unsigned int len );
int          ch_upper( int ch );

/* decimal, no sign, into at least 11 bytes; the length is returned */
unsigned int num_str( char *buf, unsigned int val );

#endif
