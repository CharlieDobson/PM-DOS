/*
 * STDLIB.H - XARC's C library: memory, sorting, the environment.
 *
 * MALLOC IS THE KERNEL'S HEAP.  A native program's slot has a heap that
 * INT 21h/48h carves (first fit, and the slot grows into the arena when
 * the heap is short), so malloc is 48h with a size kept in front of the
 * block for realloc, free is 49h and realloc tries 4Ah in place first.
 * Nothing is kept back by the library, so a block of a gigabyte freed is
 * a gigabyte the next 48h can have - which is what XArchive's memory
 * probe measures.
 */
#ifndef _XA_STDLIB_H
#define _XA_STDLIB_H

#include <stddef.h>

#define _MAX_PATH       260
#define _MAX_DRIVE      3
#define _MAX_DIR        256
#define _MAX_FNAME      256
#define _MAX_EXT        256

#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1

void  *malloc( size_t size );
void  *calloc( size_t count, size_t size );
void  *realloc( void *ptr, size_t size );
void   free( void *ptr );

void   qsort( void *base, size_t count, size_t size,
              int (*compare)( const void *, const void * ) );

char  *getenv( const char *name );
void   exit( int code );

int    atoi( const char *text );
long   atol( const char *text );
unsigned long strtoul( const char *text, char **end, int base );
long   strtol( const char *text, char **end, int base );
int    abs( int value );
long   labs( long value );

/* The whole path of 'path', with the current drive and directory put in
   front of a relative one and "." and ".." taken out.  Nothing is
   looked up, so the file need not exist.  NULL if it does not fit. */
char  *_fullpath( char *buf, const char *path, size_t size );

#endif
