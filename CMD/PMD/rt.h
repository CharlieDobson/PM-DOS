/*
 * RT.H - the minimal runtime PMD carries instead of a C library:
 * memory, strings, a heap and formatting.  EDIT's, with a small
 * printf: a program that is mostly columns of text needs one.
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

/* "needle" in "len" bytes at "hay", letters in either case: where, or -1 */
int   mem_ifind( const u8 *hay, u32 len, const char *needle );

void *try_alloc( u32 size );     /* NULL when out of memory            */
void *xalloc( u32 size );        /* never returns NULL (exits instead) */
void  xfree( void *ptr );

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

/* supplied by MAIN.C: out of memory */
void  out_of_memory( void );

/* the arguments of a function declared with "..." */
#define VA_SIZE( type )       ((sizeof( type ) + sizeof( int ) - 1) & ~(sizeof( int ) - 1))
#define VA_START( last )      ((char *)&(last) + VA_SIZE( last ))
#define VA_NEXT( args, type ) (*(type *)(((args) += VA_SIZE( type )) - VA_SIZE( type )))

#endif
