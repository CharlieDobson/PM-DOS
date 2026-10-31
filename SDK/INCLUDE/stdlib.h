/*
 * STDLIB.H - general utilities
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <_pmdefs.h>

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __pm_size_t size_t;
#endif
#if !defined( __cplusplus ) && !defined( _WCHAR_T_DEFINED )
#define _WCHAR_T_DEFINED
typedef __pm_wchar_t wchar_t;
#endif

#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1
#define RAND_MAX        0x7FFF
#define MB_CUR_MAX      1

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

_PM_BEGIN_C

void   *malloc( size_t size );
void   *calloc( size_t count, size_t size );
void   *realloc( void *block, size_t size );
void    free( void *block );

_PM_NORETURN void exit( int status );
_PM_NORETURN void _Exit( int status );
_PM_NORETURN void _exit( int status );
_PM_NORETURN void abort( void );
#ifdef __WATCOMC__
#pragma aux exit aborts;
#pragma aux _Exit aborts;
#pragma aux _exit aborts;
#pragma aux abort aborts;
#endif
int     atexit( void (*func)( void ) );

char   *getenv( const char *name );
int     putenv( const char *entry );
int     system( const char *command );

int     abs( int n );
long    labs( long n );
long long llabs( long long n );
div_t   div( int num, int den );
ldiv_t  ldiv( long num, long den );
lldiv_t lldiv( long long num, long long den );

int     atoi( const char *s );
long    atol( const char *s );
long long atoll( const char *s );
long    strtol( const char *s, char **end, int base );
unsigned long strtoul( const char *s, char **end, int base );
long long strtoll( const char *s, char **end, int base );
unsigned long long strtoull( const char *s, char **end, int base );
double  atof( const char *s );
double  strtod( const char *s, char **end );
float   strtof( const char *s, char **end );
long double strtold( const char *s, char **end );

void    qsort( void *base, size_t count, size_t size,
               int (*compare)( const void *, const void * ) );
void   *bsearch( const void *key, const void *base, size_t count, size_t size,
                 int (*compare)( const void *, const void * ) );

int     rand( void );
void    srand( unsigned seed );

int     mblen( const char *s, size_t n );
int     mbtowc( wchar_t *wc, const char *s, size_t n );
int     wctomb( char *s, wchar_t wc );
size_t  mbstowcs( wchar_t *dst, const char *src, size_t n );
size_t  wcstombs( char *dst, const wchar_t *src, size_t n );

/* not in the C standard, but in every DOS compiler's library */
char   *itoa( int value, char *buf, int radix );
char   *ltoa( long value, char *buf, int radix );
char   *ultoa( unsigned long value, char *buf, int radix );

/* the command line and the environment, as main() was given them */
extern int    _argc;
extern char **_argv;
extern char **environ;

_PM_END_C

#endif
