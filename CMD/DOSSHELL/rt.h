/*
 * RT.H - the minimal runtime DOSSHELL carries instead of a C library:
 * memory, strings, a heap, numbers and file names.
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
void  str_fit( char *dst, const char *src, u32 max );     /* as much as fits */
int   str_cmp( const char *lhs, const char *rhs );
int   str_icmp( const char *lhs, const char *rhs );
int   str_nicmp( const char *lhs, const char *rhs, u32 len );
char *str_chr( const char *str, int ch );
char *str_rchr( const char *str, int ch );
char *str_dup( const char *str );           /* xalloc'd */
void  str_upper( char *str );
int   ch_upper( int ch );

void *try_alloc( u32 size );     /* NULL when out of memory            */
void *xalloc( u32 size );        /* never returns NULL (exits instead) */
void  xfree( void *ptr );

/* numbers: decimal, right-aligned in at least "width" characters; and
   the same with the country's separator between the thousands */
char *fmt_dec( u32 val, int width, char *buf );
char *fmt_num( u32 val, int width, char *buf );
char *fmt_dec2( u32 val, char *buf );       /* two digits, a leading zero */
void  fmt_group_set( int sep );

/* file names */
const char *path_name( const char *path );  /* what follows the last \ or : */
const char *path_ext( const char *path );   /* what follows the name's last dot, or "" */
int   path_join( char *dst, const char *dir, const char *name, u32 max );
int   wild_match( const char *pattern, const char *name );

/* supplied by MAIN.C: out of memory, said the way the screen allows */
void  out_of_memory( void );

#endif
