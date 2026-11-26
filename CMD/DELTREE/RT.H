/*
 * RT.H - the minimal runtime DELTREE carries instead of a C library:
 * memory, strings, a heap and number formatting.  XCOPY's, cut down to
 * what this program uses.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, unsigned int len );
void  mem_cpy( void *dst, const void *src, unsigned int len );

unsigned int str_len( const char *str );
int          str_cpyn( char *dst, const char *src, unsigned int max );  /* 0 = it did not fit */
int          str_catn( char *dst, const char *src, unsigned int max );  /* 0 = it did not fit */
int          str_nicmp( const char *lhs, const char *rhs, unsigned int len );
int          ch_upper( int ch );

void *try_alloc( unsigned int size ); /* NULL when out of memory */
void  xfree( void *ptr );

/* decimal, right-aligned in at least "width" characters */
char *fmt_dec( unsigned int val, int width, char *buf );

#endif
