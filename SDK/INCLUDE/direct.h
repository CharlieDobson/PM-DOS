/*
 * DIRECT.H - directories and drives
 *
 * Drive numbers are 1 for A:, 2 for B: and so on, as in the other DOS
 * compilers' libraries.  Names may be long.
 */
#ifndef _DIRECT_H
#define _DIRECT_H

#include <_pmdefs.h>

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __pm_size_t size_t;
#endif

_PM_BEGIN_C

/* mkdir( path ) as DOS compilers have it, and mkdir( path, mode ) as
   POSIX has it, are both accepted; the mode is not used */
int     mkdir( const char *path, ... );
int     rmdir( const char *path );
int     chdir( const char *path );

/* the current directory with its drive, "C:\WORK"; buf may be NULL,
   and then it is malloc()ed, size bytes or as long as it needs */
char   *getcwd( char *buf, size_t size );
char   *_getdcwd( int drive, char *buf, size_t size );

int     _getdrive( void );
int     _chdrive( int drive );

_PM_END_C

#endif
