/*
 * RT.H - the minimal runtime EDIT carries instead of a C library:
 * memory, strings, a heap and number formatting.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, u32 len );
void  mem_cpy( void *dst, const void *src, u32 len );

u32   str_len( const char *str );
char *str_cpy( char *dst, const char *src );
int   str_cpyn( char *dst, const char *src, u32 max );    /* 0 = it did not fit */
int   str_catn( char *dst, const char *src, u32 max );    /* 0 = it did not fit */
int   str_icmp( const char *lhs, const char *rhs );
int   str_nicmp( const char *lhs, const char *rhs, u32 len );
int   ch_upper( int ch );
int   ch_lower( int ch );
char *str_rchr( const char *str, int ch );
char *str_chr( const char *str, int ch );
int   str_cmp( const char *lhs, const char *rhs );
int   mem_cmp( const void *lhs, const void *rhs, u32 len );
u32   dec_parse( const char *str, int *ok );  /* digits only; ok = 0 if none */

void *try_alloc( u32 size );     /* NULL when out of memory            */
void *xalloc( u32 size );        /* never returns NULL (exits instead) */
void  xfree( void *ptr );

/* decimal, right-aligned in at least "width" characters */
char *fmt_dec( u32 val, int width, char *buf );

/* supplied by MAIN.C: out of memory, with the files it can still save */
void  out_of_memory( void );

#endif
