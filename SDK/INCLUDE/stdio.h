/*
 * STDIO.H - streams
 *
 * A stream opened without "b" in its mode is a text stream, as on
 * MS-DOS: CR LF in the file is \n to the program, \n written is CR LF,
 * and Ctrl+Z (1Ah) read from a file is the end of it.  stdin, stdout
 * and stderr start as text streams.
 */
#ifndef _STDIO_H
#define _STDIO_H

#include <_pmdefs.h>
#include <stdarg.h>

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __pm_size_t size_t;
#endif

/* what a FILE holds is the library's business; programs use FILE * */
typedef struct __pm_file FILE;
typedef long fpos_t;

#define EOF             (-1)
#define BUFSIZ          4096
#define FOPEN_MAX       20
#define FILENAME_MAX    260
#define L_tmpnam        260
#define TMP_MAX         32767

#define _IOFBF          0
#define _IOLBF          1
#define _IONBF          2

#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

_PM_BEGIN_C

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE   *fopen( const char *name, const char *mode );
FILE   *freopen( const char *name, const char *mode, FILE *fp );
FILE   *fdopen( int handle, const char *mode );
int     fclose( FILE *fp );
int     fflush( FILE *fp );
int     setvbuf( FILE *fp, char *buf, int mode, size_t size );
void    setbuf( FILE *fp, char *buf );
int     fileno( FILE *fp );

int     fgetc( FILE *fp );
int     getc( FILE *fp );
int     getchar( void );
char   *fgets( char *buf, int size, FILE *fp );
int     ungetc( int c, FILE *fp );
int     fputc( int c, FILE *fp );
int     putc( int c, FILE *fp );
int     putchar( int c );
int     fputs( const char *s, FILE *fp );
int     puts( const char *s );
size_t  fread( void *buf, size_t size, size_t count, FILE *fp );
size_t  fwrite( const void *buf, size_t size, size_t count, FILE *fp );

int     fseek( FILE *fp, long offset, int origin );
long    ftell( FILE *fp );
void    rewind( FILE *fp );
int     fgetpos( FILE *fp, fpos_t *pos );
int     fsetpos( FILE *fp, const fpos_t *pos );

int     feof( FILE *fp );
int     ferror( FILE *fp );
void    clearerr( FILE *fp );
void    perror( const char *s );

int     printf( const char *fmt, ... ) _PM_PRINTF( 1, 2 );
int     fprintf( FILE *fp, const char *fmt, ... ) _PM_PRINTF( 2, 3 );
int     sprintf( char *buf, const char *fmt, ... ) _PM_PRINTF( 2, 3 );
int     snprintf( char *buf, size_t size, const char *fmt, ... ) _PM_PRINTF( 3, 4 );
int     vprintf( const char *fmt, va_list args );
int     vfprintf( FILE *fp, const char *fmt, va_list args );
int     vsprintf( char *buf, const char *fmt, va_list args );
int     vsnprintf( char *buf, size_t size, const char *fmt, va_list args );

int     scanf( const char *fmt, ... );
int     fscanf( FILE *fp, const char *fmt, ... );
int     sscanf( const char *s, const char *fmt, ... );
int     vscanf( const char *fmt, va_list args );
int     vfscanf( FILE *fp, const char *fmt, va_list args );
int     vsscanf( const char *s, const char *fmt, va_list args );

int     remove( const char *name );
int     rename( const char *from, const char *to );
FILE   *tmpfile( void );
char   *tmpnam( char *buf );

_PM_END_C

#endif
