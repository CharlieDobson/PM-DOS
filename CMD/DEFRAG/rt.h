/*
 * RT.H - the minimal runtime DEFRAG carries instead of a C library:
 * memory, strings, a heap, formatting, lists and 64-bit helpers.
 * PMD's, with CHKDSK's cluster lists and 64-bit arithmetic.
 *
 * Open Watcom compiles 64-bit add, subtract and compare inline, but
 * calls library helpers for multiply, divide and shifts.  Nothing is
 * linked from the C library, so code uses mul32() and div64() below
 * instead of those operators on uint64_t values.
 */
#ifndef RT_H
#define RT_H

#include "types.h"

void  mem_set( void *dst, int val, unsigned int len );
void  mem_cpy( void *dst, const void *src, unsigned int len );
int   mem_cmp( const void *lhs, const void *rhs, unsigned int len );

unsigned int str_len( const char *str );
char        *str_cpy( char *dst, const char *src );
int          str_cpyn( char *dst, const char *src, unsigned int max );  /* 0 = it did not fit */
int          str_catn( char *dst, const char *src, unsigned int max );  /* 0 = it did not fit */
int          str_icmp( const char *lhs, const char *rhs );
int          str_nicmp( const char *lhs, const char *rhs, unsigned int len );
int          ch_upper( int ch );
char        *str_chr( const char *str, int ch );
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

/* lines of text: a box, a page of help */
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
    uint64_t whole;
    struct { uint32_t lo, hi; } parts;
} U64;

uint64_t mul32( uint32_t lhs, uint32_t rhs );       /* full 32x32 -> 64 product */
uint32_t div64( uint64_t *num, uint32_t divisor );  /* *num /= divisor, returns the
                                                       remainder; divisor < 65536 */
uint32_t lo32( uint64_t val );
uint32_t hi32( uint64_t val );

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
int   __cdecl sfmt( char *buf, unsigned int max, const char *fmt, ... );
int   vfmt( char *buf, unsigned int max, const char *fmt, char *args );
void  fmt_group_set( int ch );
char *fmt_bytes( uint64_t val, char *buf ); /* a 64-bit count, grouped */

/* supplied by MAIN.C: "Insufficient memory" and exit */
void  out_of_memory( void );

/* the arguments of a function declared with "..." */
#define VA_SIZE( type )       ((sizeof( type ) + sizeof( int ) - 1) & ~(sizeof( int ) - 1))
#define VA_START( last )      ((char *)&(last) + VA_SIZE( last ))
#define VA_NEXT( args, type ) (*(type *)(((args) += VA_SIZE( type )) - VA_SIZE( type )))

/* JMP.ASM: the way out of a run from wherever it has got to */
typedef struct {
    uint32_t reg[6];            /* EBX ESI EDI EBP ESP EIP */
} JMPBUF;
int  __cdecl rt_setjmp( JMPBUF *buf );
void __cdecl rt_longjmp( JMPBUF *buf, int val );

#endif
