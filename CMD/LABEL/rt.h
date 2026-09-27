/*
 * RT.H - minimal runtime: memory, strings, heap and 64-bit helpers.
 *
 * Open Watcom compiles 64-bit add, subtract and compare inline, but calls
 * library helpers (__U8M, __U8D, __U8LS, __U8RS) for multiply, divide and
 * shifts.  Because nothing is linked from the C library, code must use
 * mul32() below instead of those operators on u64 values.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, u32 len );
void  mem_cpy( void *dst, const void *src, u32 len );
int   mem_cmp( const void *lhs, const void *rhs, u32 len );

u32   str_len( const char *str );
char *str_cpy( char *dst, const char *src );
int   str_icmp( const char *lhs, const char *rhs );
int   ch_upper( int ch );

void *try_alloc( u32 size );     /* NULL when out of memory            */
void *xalloc( u32 size );        /* never returns NULL (exits instead) */
void *xzalloc( u32 size );       /* xalloc + zero fill                 */
void  xfree( void *ptr );

/* growable list of u32 (cluster lists) */
typedef struct {
    u32 *items;
    u32  count;
    u32  cap;
} U32LIST;
void  list_add( U32LIST *list, u32 val );
void  list_free( U32LIST *list );

typedef union {
    u64 whole;
    struct { u32 lo, hi; } parts;
} U64;

u64   mul32( u32 lhs, u32 rhs );  /* full 32x32 -> 64 product */
u32   lo32( u64 val );
u32   hi32( u64 val );

/* supplied by MAIN.C: "Insufficient memory" and exit */
void  out_of_memory( void );

#endif
