/*
 * RT.H - the minimal runtime ACROREAD carries instead of a C library:
 * memory, strings, a heap, pools and formatting.  PMD's, with pools (a
 * page's objects are freed all at once) and a jump out of a page that
 * cannot be finished.
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
int   str_ncmp( const char *lhs, const char *rhs, u32 len );
int   mem_cmp( const void *lhs, const void *rhs, u32 len );
u32   dec_parse( const char *str, int *ok );  /* digits only; ok = 0 if none */

/* "needle" in "len" bytes at "hay": where, or -1.  Letters in either
   case for mem_ifind. */
int   mem_ifind( const u8 *hay, u32 len, const char *needle );
int   mem_find( const u8 *hay, u32 len, const char *needle );

void *try_alloc( u32 size );     /* NULL when out of memory            */
void *xalloc( u32 size );        /* never returns NULL (exits instead) */
void  xfree( void *ptr );
u32   heap_in_use( void );       /* bytes handed out and not given back */

/*
 * A POOL is memory with one lifetime: everything a page needs is taken
 * from the page's pool and given back together.  pool_big is a block
 * that can also go back early (pool_unbig) - a decompressor's window,
 * a row of an image.  pool_alloc and pool_big never return NULL: when
 * the heap is empty they call out_of_memory(), which does not return.
 */
typedef struct POOL POOL;
POOL *pool_new( void );
void *pool_alloc( POOL *pool, u32 size );       /* zeroed */
void *pool_big( POOL *pool, u32 size );         /* zeroed */
void *pool_try_big( POOL *pool, u32 size );     /* NULL if there is none */
void  pool_unbig( void *ptr );
void  pool_free( POOL *pool );
void  pool_reset( POOL *pool );                 /* empty, and ready to be filled again */
char *pool_str( POOL *pool, const char *str );

/* decimal, right-aligned in at least "width" characters */
char *fmt_dec( u32 val, int width, char *buf );

/*
 * sfmt - printf into "buf", never more than "max" bytes with the NUL.
 *
 *   %s %c %d %u %x %X %%      d, u, x and X take an unsigned long or an
 *                             int, which are the same size here
 *   %-12s %5u %04X            a width, '-' to pad on the right, '0' to
 *                             pad a number with zeroes
 *   %*s                       the width from the arguments
 *   %,u                       grouped in threes with the country's
 *                             separator (fmt_group_set)
 *
 * Returns the length written.
 */
int   __cdecl sfmt( char *buf, u32 max, const char *fmt, ... );
int   vfmt( char *buf, u32 max, const char *fmt, char *args );
void  fmt_group_set( int ch );

/* supplied by the program.  mem_reclaim empties a cache and returns 1,
   or 0 when there is nothing left to empty; out_of_memory then gives up
   the page being shown and does not return. */
int   mem_reclaim( void );
void  out_of_memory( void );

/* JMP.ASM: setjmp and longjmp, by other names so that nothing mistakes
   them for a library's */
typedef struct {
    u32 regs[6];                /* EBX ESI EDI EBP ESP EIP */
} JMPBUF;
int   __cdecl rt_setjmp( JMPBUF *buf );
void  __cdecl rt_longjmp( JMPBUF *buf, int val );

/* the arguments of a function declared with "..." */
#define VA_SIZE( type )       ((sizeof( type ) + sizeof( int ) - 1) & ~(sizeof( int ) - 1))
#define VA_START( last )      ((char *)&(last) + VA_SIZE( last ))
#define VA_NEXT( args, type ) (*(type *)(((args) += VA_SIZE( type )) - VA_SIZE( type )))

#endif
