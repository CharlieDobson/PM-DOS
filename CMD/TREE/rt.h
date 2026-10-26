/*
 * RT.H - the minimal runtime TREE carries instead of a C library:
 * memory, strings and a heap.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, u32 len );
void  mem_cpy( void *dst, const void *src, u32 len );

u32   str_len( const char *str );
int   str_cpyn( char *dst, const char *src, u32 max );    /* 0 = it did not fit */
int   str_catn( char *dst, const char *src, u32 max );    /* 0 = it did not fit */
void  str_field( char *dst, const char *src, u32 len, u32 max );
int   str_icmp( const char *lhs, const char *rhs );
int   str_nicmp( const char *lhs, const char *rhs, u32 len );
int   ch_upper( int ch );

void *try_alloc( u32 size );     /* NULL when out of memory            */
void *xalloc( u32 size );        /* never returns NULL (exits instead) */
void  xfree( void *ptr );

/* supplied by MAIN.C: "Insufficient memory" and exit */
void  out_of_memory( void );

#endif
