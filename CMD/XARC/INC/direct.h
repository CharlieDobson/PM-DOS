/*
 * DIRECT.H - XARC's C library: folders and the current directory
 * (LIBIO.C).  Long names go through 7139h/713Ah/713Bh/7147h when the
 * kernel has its long-name calls switched on.
 */
#ifndef _XA_DIRECT_H
#define _XA_DIRECT_H

#include <stddef.h>

int    _mkdir( const char *path );
int    _rmdir( const char *path );
int    chdir( const char *path );
char  *getcwd( char *buf, size_t size );    /* "C:\DIR", the long form */
int    _chdrive( int drive );                /* 1 = A: */
int    _getdrive( void );                    /* 1 = A: */

#define mkdir( path )   _mkdir( path )
#define rmdir( path )   _rmdir( path )
#define _chdir          chdir
#define _getcwd         getcwd

#endif
