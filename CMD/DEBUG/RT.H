/*
 * RT.H - the minimal runtime DEBUG carries instead of a C library:
 * memory, strings, a heap and the output line.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, unsigned int len );
void  mem_cpy( void *dst, const void *src, unsigned int len );
int   mem_cmp( const void *lhs, const void *rhs, unsigned int len );

unsigned int str_len( const char *str );
void         str_cpy( char *dst, const char *src );
int          str_icmp( const char *lhs, const char *rhs );
int          str_nicmp( const char *lhs, const char *rhs, unsigned int len );
int          ch_upper( int ch );

void *try_alloc( unsigned int size );    /* NULL when out of memory */
void *scratch_get( unsigned int size );  /* the same: one buffer, kept and reused */

/* THE OUTPUT LINE.  Everything DEBUG says is built here and goes to
   standard output in one write a line, as 6.22's did, so that a
   redirected session is a file of whole lines. */
void  out_ch( int ch );
void  out_str( const char *str );
void  out_hex( uint32_t val, int digits );
void  out_spaces( int count );
void  out_pad( int column );     /* spaces up to this column */
int   out_col( void );
void  out_nl( void );
void  out_err( const char *msg );   /* a line to standard error */
void  out_flush( void );

#endif
