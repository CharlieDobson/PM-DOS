/*
 * RT.H - the minimal runtime XCOPY carries instead of a C library:
 * memory, strings, a heap and number formatting.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, unsigned int len );
void  mem_cpy( void *dst, const void *src, unsigned int len );

unsigned int str_len( const char *str );
char        *str_cpy( char *dst, const char *src );
int          str_cpyn( char *dst, const char *src, unsigned int max );  /* 0 = it did not fit */
int          str_catn( char *dst, const char *src, unsigned int max );  /* 0 = it did not fit */
int          str_icmp( const char *lhs, const char *rhs );
int          str_nicmp( const char *lhs, const char *rhs, unsigned int len );
int          ch_upper( int ch );
int          ch_lower( int ch );
char        *str_rchr( const char *str, int ch );

void *try_alloc( unsigned int size );  /* NULL when out of memory          */
void *xalloc( unsigned int size );     /* never returns NULL (exits instead) */
void  xfree( void *ptr );

/* decimal, right-aligned in at least "width" characters */
char *fmt_dec( unsigned int val, int width, char *buf );

/* supplied by MAIN.C: "Insufficient memory" and exit */
void  out_of_memory( void );

#endif
