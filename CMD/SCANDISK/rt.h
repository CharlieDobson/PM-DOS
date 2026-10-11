/*
 * RT.H - the minimal runtime SCANDISK carries instead of a C library:
 * memory, strings, a heap, formatting, lists and 64-bit helpers.
 * PMD's, with CHKDSK's cluster lists and 64-bit arithmetic.
 *
 * Open Watcom compiles 64-bit add, subtract and compare inline, but
 * calls library helpers for multiply, divide and shifts.  Nothing is
 * linked from the C library, so code uses mul32() and div64() below
 * instead of those operators on u64 values.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, u32 len );
void  mem_cpy( void *dst, const void *src, u32 len );
int   mem_cmp( const void *lhs, const void *rhs, u32 len );

u32   str_len( const char *str );
char *str_cpy( char *dst, const char *src );
int   str_cpyn( char *dst, const char *src, u32 max );    /* 0 = it did not fit */
int   str_catn( char *dst, const char *src, u32 max );    /* 0 = it did not fit */
int   str_icmp( const char *lhs, const char *rhs );
int   str_nicmp( const char *lhs, const char *rhs, u32 len );
int   ch_upper( int ch );
char *str_chr( const char *str, int ch );
char *str_dup( const char *str );
char *path_join( const char *dir, const char *name );

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

/* lines of text: the log, a "More Info" page */
typedef struct {
    char **line;
    int    count, cap;
} TEXT;
void  tx_init( TEXT *text );
void  tx_free( TEXT *text );
void  tx_add( TEXT *text, const char *line );
void  __cdecl tx_addf( TEXT *text, const char *fmt, ... );
/* a paragraph broken into lines of at most "width" characters; '\n' in
   it starts a new line */
void  tx_wrap( TEXT *text, const char *para, int width );

typedef union {
    u64 whole;
    struct { u32 lo, hi; } parts;
} U64;

u64   mul32( u32 lhs, u32 rhs );       /* full 32x32 -> 64 product */
u32   div64( u64 *num, u32 divisor );  /* *num /= divisor, returns the
                                          remainder; divisor < 65536 */
u32   lo32( u64 val );
u32   hi32( u64 val );

/*
 * sfmt - printf into "buf", never more than "max" bytes with the NUL.
 *
 *   %s %c %d %u %x %X %%      d, u, x and X take an unsigned long or an
 *                             int, which are the same size here
 *   %-12s %5u %04X            a width, '-' to pad on the right, '0' to
 *                             pad a number with zeroes
 *   %,u                       grouped in threes with the country's
 *                             separator (fmt_group_set)
 *
 * Returns the length written.
 */
int   __cdecl sfmt( char *buf, u32 max, const char *fmt, ... );
int   vfmt( char *buf, u32 max, const char *fmt, char *args );
void  fmt_group_set( int ch );
char *fmt_bytes( u64 val, char *buf );  /* a 64-bit count, grouped */

/* supplied by MAIN.C: "Insufficient memory" and exit */
void  out_of_memory( void );

/* the arguments of a function declared with "..." */
#define VA_SIZE( type )       ((sizeof( type ) + sizeof( int ) - 1) & ~(sizeof( int ) - 1))
#define VA_START( last )      ((char *)&(last) + VA_SIZE( last ))
#define VA_NEXT( args, type ) (*(type *)(((args) += VA_SIZE( type )) - VA_SIZE( type )))

/* JMP.ASM: the way out of a check from wherever it has got to */
typedef struct {
    u32 reg[6];                 /* EBX ESI EDI EBP ESP EIP */
} JMPBUF;
int  __cdecl rt_setjmp( JMPBUF *buf );
void __cdecl rt_longjmp( JMPBUF *buf, int val );

#endif
