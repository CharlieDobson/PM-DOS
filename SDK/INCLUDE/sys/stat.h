/*
 * SYS/STAT.H - what a file is
 *
 * DOS keeps one time per file, the last write, in local time and to
 * two seconds; st_mtime, st_atime and st_ctime all give it.  A file
 * is writable unless it is read-only, and "executable" when its name
 * ends in .EXE, .COM or .BAT.
 */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>

struct stat {
    dev_t   st_dev;                     /* the drive: 0 for A: */
    ino_t   st_ino;
    mode_t  st_mode;
    nlink_t st_nlink;
    uid_t   st_uid;
    gid_t   st_gid;
    dev_t   st_rdev;
    off_t   st_size;
    time_t  st_atime;
    time_t  st_mtime;
    time_t  st_ctime;
};

#define S_IFMT          0xF000
#define S_IFDIR         0x4000
#define S_IFCHR         0x2000
#define S_IFREG         0x8000
#define S_IREAD         0x0100
#define S_IWRITE        0x0080
#define S_IEXEC         0x0040
#define S_IRUSR         S_IREAD
#define S_IWUSR         S_IWRITE
#define S_IXUSR         S_IEXEC
#define S_IRWXU         (S_IREAD | S_IWRITE | S_IEXEC)

#define S_ISDIR( m )    (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG( m )    (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR( m )    (((m) & S_IFMT) == S_IFCHR)

_PM_BEGIN_C

int     stat( const char *path, struct stat *st );
int     fstat( int handle, struct stat *st );
int     chmod( const char *path, int mode );
int     mkdir( const char *path, ... );
mode_t  umask( mode_t mask );

_PM_END_C

#endif
