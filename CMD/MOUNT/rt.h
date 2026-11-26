/*
 * RT.H - the minimal runtime MOUNT carries instead of a C library:
 * memory, strings, a heap and number formatting.  DELTREE's, with the
 * calls PMWRITE's FLATE.C needs declared exactly as PMWRITE's RT.H
 * declares them (mem_set, mem_cpy, try_alloc, xfree), so the one
 * FLATE.C links against either program's runtime.
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
void *try_realloc( void *ptr, unsigned int size ); /* NULL, and ptr kept, if there is no room */

/* decimal, right-aligned in at least "width" characters */
char *fmt_dec( unsigned int val, int width, char *buf );
/* the same with "sep" between each three digits (0 = none) */
char *fmt_grp( unsigned int val, int width, int sep, char *buf );

#endif
