/*
 * RT.H - the minimal runtime PMWRITE carries instead of a C library:
 * memory, strings, a heap, pools and formatting.  ACROREAD's, with
 * pools (a PDF page's objects are freed all at once), a jump out of a
 * file that cannot be finished, and a block that can grow.
 *
 * mem_cpy copies either way round, so it is the one to use when the
 * two ranges overlap.
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
void         str_cpyt( char *dst, const char *src, unsigned int max );  /* what fits, and ended */
int          str_icmp( const char *lhs, const char *rhs );
int          str_nicmp( const char *lhs, const char *rhs, unsigned int len );
int          ch_upper( int ch );
int          ch_lower( int ch );
char        *str_rchr( const char *str, int ch );
char        *str_chr( const char *str, int ch );
int          str_cmp( const char *lhs, const char *rhs );
int          str_ncmp( const char *lhs, const char *rhs, unsigned int len );
int          mem_cmp( const void *lhs, const void *rhs, unsigned int len );
unsigned int dec_parse( const char *str, int *ok ); /* digits only; ok = 0 if none */

/* "needle" in "len" bytes at "hay": where, or -1.  Letters in either
   case for mem_ifind. */
int   mem_ifind( const uint8_t *hay, unsigned int len, const char *needle );
int   mem_find( const uint8_t *hay, unsigned int len, const char *needle );

void        *try_alloc( unsigned int size );  /* NULL when out of memory       */
void        *xalloc( unsigned int size );     /* never returns NULL (exits instead) */
void         xfree( void *ptr );
unsigned int heap_in_use( void );             /* bytes handed out and not given back */
void        *try_realloc( void *ptr, unsigned int size ); /* NULL, and ptr kept, if there is no room */
unsigned int mem_size( void *ptr );           /* what a block can hold */

/*
 * A POOL is memory with one lifetime: everything a page needs is taken
 * from the page's pool and given back together.  pool_big is a block
 * that can also go back early (pool_unbig) - a decompressor's window,
 * a row of an image.  pool_alloc and pool_big never return NULL: when
 * the heap is empty they call out_of_memory(), which does not return.
 */
typedef struct POOL POOL;
POOL *pool_new( void );
void *pool_alloc( POOL *pool, unsigned int size );    /* zeroed */
void *pool_big( POOL *pool, unsigned int size );      /* zeroed */
void *pool_try_big( POOL *pool, unsigned int size );  /* NULL if there is none */
void  pool_unbig( void *ptr );
void  pool_free( POOL *pool );
void  pool_reset( POOL *pool );                       /* empty, and ready to be filled again */
char *pool_str( POOL *pool, const char *str );

/* decimal, right-aligned in at least "width" characters */
char *fmt_dec( unsigned int val, int width, char *buf );

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
int   __cdecl sfmt( char *buf, unsigned int max, const char *fmt, ... );
int   vfmt( char *buf, unsigned int max, const char *fmt, char *args );
void  fmt_group_set( int ch );

/* supplied by the program.  mem_reclaim empties a cache and returns 1,
   or 0 when there is nothing left to empty; out_of_memory then gives up
   the page being shown and does not return. */
int   mem_reclaim( void );
void  out_of_memory( void );

/* JMP.ASM: setjmp and longjmp, by other names so that nothing mistakes
   them for a library's */
typedef struct {
    uint32_t regs[6];           /* EBX ESI EDI EBP ESP EIP */
} JMPBUF;
int   __cdecl rt_setjmp( JMPBUF *buf );
void  __cdecl rt_longjmp( JMPBUF *buf, int val );

/* the arguments of a function declared with "..." */
#define VA_SIZE( type )       ((sizeof( type ) + sizeof( int ) - 1) & ~(sizeof( int ) - 1))
#define VA_START( last )      ((char *)&(last) + VA_SIZE( last ))
#define VA_NEXT( args, type ) (*(type *)(((args) += VA_SIZE( type )) - VA_SIZE( type )))

#endif
