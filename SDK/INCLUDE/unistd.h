/*
 * UNISTD.H - the POSIX calls that have a DOS meaning, for programs
 * written for Unix.  They are the same functions IO.H, DIRECT.H and
 * PROCESS.H declare.
 */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <_pmdefs.h>
#include <sys/types.h>
#include <io.h>

#ifndef STDIN_FILENO
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2
#endif

_PM_BEGIN_C

int     chdir( const char *path );
int     rmdir( const char *path );
char   *getcwd( char *buf, size_t size );
int     getpid( void );
unsigned sleep( unsigned seconds );
int     usleep( unsigned long microseconds );

_PM_END_C

#endif
