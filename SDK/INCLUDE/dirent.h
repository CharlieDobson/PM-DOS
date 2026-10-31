/*
 * DIRENT.H - reading a directory
 *
 *   DIR *d = opendir( "C:\\DOCS" );
 *   struct dirent *e;
 *   while ( (e = readdir( d )) != NULL ) printf( "%s\n", e->d_name );
 *   closedir( d );
 *
 * Every entry is listed, hidden and system ones and subdirectories
 * included, "." and ".." too where the directory has them.  d_name is
 * the long name when there is one.  The other fields are DOS's.
 */
#ifndef _DIRENT_H
#define _DIRENT_H

#include <_pmdefs.h>

struct dirent {
    char            d_name[260];
    char            d_short[14];        /* the 8.3 name */
    unsigned char   d_attr;             /* _A_* bits (DOS.H) */
    unsigned short  d_time;             /* DOS time and date */
    unsigned short  d_date;
    unsigned long   d_size;
};

typedef struct __pm_dir DIR;

_PM_BEGIN_C

DIR    *opendir( const char *path );
struct dirent *readdir( DIR *dir );
void    rewinddir( DIR *dir );
int     closedir( DIR *dir );

_PM_END_C

#endif
