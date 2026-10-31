/*
 * IO.H - files by handle
 *
 * These are the DOS handle calls with C names.  They never translate
 * line ends: O_TEXT and O_BINARY are accepted and make no difference.
 * Use a stream (STDIO.H) for text.
 *
 * Names may be long: the long-name calls are used first, and the
 * classic ones when the kernel has them switched off (LFN=OFF).
 */
#ifndef _IO_H
#define _IO_H

#include <_pmdefs.h>
#include <fcntl.h>

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __pm_size_t size_t;
#endif

#define F_OK    0       /* access(): the file exists */
#define W_OK    2       /* it can be written */
#define R_OK    4       /* it can be read */

#ifndef SEEK_SET
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2
#endif

_PM_BEGIN_C

/* the third argument is used only with O_CREAT, and only to make the
   file read-only when it has no write permission (0200) in it */
int     open( const char *name, int flags, ... );
int     creat( const char *name, int mode );
int     close( int handle );
int     read( int handle, void *buf, unsigned count );
int     write( int handle, const void *buf, unsigned count );
long    lseek( int handle, long offset, int origin );
long    tell( int handle );
long    filelength( int handle );
int     eof( int handle );
int     isatty( int handle );
int     dup( int handle );
int     dup2( int handle, int handle2 );
int     unlink( const char *name );
int     access( const char *name, int mode );
int     setmode( int handle, int mode );

_PM_END_C

#endif
