/*
 * STDIO.H - XARC's C library: buffered files over PM-DOS handles
 * (LIBIO.C).
 *
 * A FILE is a handle and a buffer.  "b" in the mode is binary; without
 * it a file is TEXT, as DOS C libraries have always had it: a '\n'
 * written goes out as CR LF, and CR is dropped before LF on the way in
 * (XARCHIVE.INI and an exclusion list are the text files here).  Names
 * may be long: LIBIO.C opens with 716Ch and falls back to the classic
 * calls when the kernel's long-name calls are switched off.
 */
#ifndef _XA_STDIO_H
#define _XA_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct _xa_file FILE;

#define EOF             (-1)
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2
#define BUFSIZ          16384
#define FILENAME_MAX    260

extern FILE *_xa_stdin, *_xa_stdout, *_xa_stderr;
#define stdin           _xa_stdin
#define stdout          _xa_stdout
#define stderr          _xa_stderr

FILE  *fopen( const char *path, const char *mode );
int    fclose( FILE *fp );
size_t fread( void *buf, size_t size, size_t count, FILE *fp );
size_t fwrite( const void *buf, size_t size, size_t count, FILE *fp );
int    fseek( FILE *fp, long offset, int whence );
long   ftell( FILE *fp );
void   rewind( FILE *fp );
int    fflush( FILE *fp );
int    fgetc( FILE *fp );
char  *fgets( char *buf, int size, FILE *fp );
int    fputc( int ch, FILE *fp );
int    fputs( const char *text, FILE *fp );
int    puts( const char *text );
int    feof( FILE *fp );
int    ferror( FILE *fp );
void   clearerr( FILE *fp );
int    fileno( FILE *fp );

#define getc( fp )          fgetc( fp )
#define putc( ch, fp )      fputc( ch, fp )
#define putchar( ch )       fputc( ch, stdout )
#define getchar()           fgetc( stdin )

int    printf( const char *fmt, ... );
int    fprintf( FILE *fp, const char *fmt, ... );
int    sprintf( char *buf, const char *fmt, ... );
int    _snprintf( char *buf, size_t size, const char *fmt, ... );
int    vprintf( const char *fmt, va_list args );
int    vfprintf( FILE *fp, const char *fmt, va_list args );
int    vsprintf( char *buf, const char *fmt, va_list args );
int    _vsnprintf( char *buf, size_t size, const char *fmt, va_list args );
#define snprintf    _snprintf
#define vsnprintf   _vsnprintf

int    remove( const char *path );
int    rename( const char *from, const char *to );

#endif
