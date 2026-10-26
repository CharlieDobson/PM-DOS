/*
 * SYS\STAT.H - XARC's C library: what a file is (LIBIO.C).  stat finds
 * the file with a directory search, so its size, its attributes and the
 * time it was last written come from the directory entry and the file
 * is not opened.
 */
#ifndef _XA_SYS_STAT_H
#define _XA_SYS_STAT_H

#include <sys/types.h>

#define S_IFMT      0xF000
#define S_IFDIR     0x4000
#define S_IFREG     0x8000
#define S_ISDIR( m )    (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG( m )    (((m) & S_IFMT) == S_IFREG)

struct stat {
    unsigned  st_mode;      /* S_IFDIR or S_IFREG */
    unsigned  st_attr;      /* the DOS attribute byte */
    off_t     st_size;
    time_t    st_mtime;     /* local, as TIME.H says */
    time_t    st_atime;
    time_t    st_ctime;
};

int stat( const char *path, struct stat *info );

#endif
