/*
 * RT.H - the minimal runtime QBASIC carries instead of a C library:
 * memory, strings, a heap, pools and a jump out of deep code.  EDIT's,
 * with a heap built for an interpreter (MEM.C).
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
void  str_cpyt( char *dst, const char *src, u32 max );    /* what fits, ended */
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
void *try_realloc( void *ptr, u32 size );   /* NULL (and ptr kept) if no room */
u32   mem_avail( void );         /* the most one allocation could have now */
u32   mem_size( void *ptr );     /* what a block can hold */

/* A POOL is memory with one lifetime: the compiler takes its names and
   records from one and gives them back together.  pool_new, pool_alloc
   and pool_strn return NULL when memory runs out. */
typedef struct POOL POOL;
POOL *pool_new( void );
void *pool_alloc( POOL *pool, u32 size );       /* zeroed */
char *pool_strn( POOL *pool, const char *text, u32 len );
void  pool_free( POOL *pool );

/* decimal, right-aligned in at least "width" characters */
char *fmt_dec( u32 val, int width, char *buf );

/* supplied by the program: out of memory, said, and not returning */
void  out_of_memory( void );

typedef struct {
    u32 regs[6];
} JMPBUF;

int  __cdecl rt_setjmp( JMPBUF *buf );
void __cdecl rt_longjmp( JMPBUF *buf, int val );

#endif
