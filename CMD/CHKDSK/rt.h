/*
 * RT.H - minimal runtime: memory, strings, heap and 64-bit helpers.
 *
 * Open Watcom compiles 64-bit add, subtract and compare inline, but calls
 * library helpers (__U8M, __U8D, __U8LS, __U8RS) for multiply, divide and
 * shifts.  Because nothing is linked from the C library, code must use
 * mul32() and div64() below instead of those operators on uint64_t values.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, unsigned int len );
void  mem_cpy( void *dst, const void *src, unsigned int len );
int   mem_cmp( const void *lhs, const void *rhs, unsigned int len );

unsigned int str_len( const char *str );
char        *str_cpy( char *dst, const char *src );
char        *str_cat( char *dst, const char *src );
int          str_icmp( const char *lhs, const char *rhs );
int          ch_upper( int ch );
char        *str_dup( const char *str );
char        *path_join( const char *dir, const char *name );

void *try_alloc( unsigned int size );  /* NULL when out of memory          */
void *xalloc( unsigned int size );     /* never returns NULL (exits instead) */
void *xzalloc( unsigned int size );    /* xalloc + zero fill               */
void  xfree( void *ptr );

/* growable list of uint32_t (cluster lists) */
typedef struct {
    uint32_t    *items;
    unsigned int count;
    unsigned int cap;
} U32LIST;
void  list_add( U32LIST *list, uint32_t val );
void  list_free( U32LIST *list );

typedef union {
    uint64_t whole;
    struct { uint32_t lo, hi; } parts;
} U64;

uint64_t mul32( uint32_t lhs, uint32_t rhs );       /* full 32x32 -> 64 product */
uint32_t div64( uint64_t *num, uint32_t divisor );  /* *num /= divisor, returns the
                                                       remainder; divisor < 65536 */
uint32_t lo32( uint64_t val );
uint32_t hi32( uint64_t val );

/* supplied by MAIN.C: "Insufficient memory" and exit */
void  out_of_memory( void );

#endif
