/*
 * DIR.C - directories and drives (DIRECT.H), reading a directory
 * (DIRENT.H), stat() and chmod() (SYS/STAT.H)
 *
 * The long-name calls first, as everywhere in the library: 7139h,
 * 713Ah, 713Bh, 7147h, 7143h, and the 714Eh/714Fh/71A1h finds, whose
 * record is 318 bytes - attributes at 0, the last write at 14h (DOS
 * time in its low word, DOS date in its high one: the kernel always
 * gives DOS times), the size at 20h, the long name at 2Ch and the 8.3
 * one at 130h.  With LFN=OFF the classic calls, and 4Eh/4Fh's DTA:
 * attributes at 15h, time 16h, date 18h, size 1Ah, name 1Eh.
 */
#include "pmlib.h"
#include <direct.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <io.h>

#define ATTR_ALL    0x37                /* everything but volume labels */
#define ATTR_RDONLY 0x01
#define ATTR_DIR    0x10

/* one call with a name in EDX, long form first */
static int name_call( uint32_t lfn, uint32_t classic, const char *path )
{
    pm_regs regs;

    for ( ;; ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)path;
        if ( !__pm_dos( &regs, __pm_lfn ? lfn : classic ) ) {
            return 0;
        }
        if ( !__pm_lfn_missing( &regs ) ) {
            return __pm_fail( &regs );
        }
    }
}

int mkdir( const char *path, ... )
{
    return name_call( 0x7139, 0x3900, path );
}

int rmdir( const char *path )
{
    return name_call( 0x713A, 0x3A00, path );
}

int chdir( const char *path )
{
    return name_call( 0x713B, 0x3B00, path );
}

int _getdrive( void )
{
    pm_regs regs = { 0 };

    __pm_dos( &regs, 0x1900 );
    return (int)(regs.eax & 0xFF) + 1;
}

int _chdrive( int drive )
{
    pm_regs regs = { 0 };

    regs.edx = (uint32_t)(drive - 1);
    __pm_dos( &regs, 0x0E00 );
    return _getdrive() == drive ? 0 : __pm_errno( ENODEV );
}

char *_getdcwd( int drive, char *buf, size_t size )
{
    char path[264];
    pm_regs regs;
    size_t len;

    if ( drive == 0 ) {
        drive = _getdrive();
    }
    for ( ;; ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)drive;
        regs.esi = (uint32_t)(path + 3);
        if ( !__pm_dos( &regs, __pm_lfn ? 0x7147 : 0x4700 ) ) {
            break;
        }
        if ( !__pm_lfn_missing( &regs ) ) {
            __pm_fail( &regs );
            return NULL;
        }
    }
    path[0] = (char)('A' + drive - 1);
    path[1] = ':';
    path[2] = '\\';
    len = strlen( path ) + 1;
    if ( buf == NULL ) {
        buf = (char *)malloc( size > len ? size : len );
        if ( buf == NULL ) {
            errno = ENOMEM;
            return NULL;
        }
    } else if ( size < len ) {
        errno = ERANGE;
        return NULL;
    }
    memcpy( buf, path, len );
    return buf;
}

char *getcwd( char *buf, size_t size )
{
    return _getdcwd( 0, buf, size );
}

/*--- finding ----------------------------------------------------------*/

/* the parts of a find record or DTA the library uses */
typedef struct {
    unsigned char  attr;
    unsigned short time, date;
    unsigned long  size;
    const char    *name;
    const char    *shortname;
} FOUND;

static void from_record( const unsigned char *rec, int lfn, FOUND *f )
{
    if ( lfn ) {
        f->attr = rec[0];
        f->time = (unsigned short)(rec[0x14] | (rec[0x15] << 8));
        f->date = (unsigned short)(rec[0x16] | (rec[0x17] << 8));
        f->size = (unsigned long)rec[0x20] | ((unsigned long)rec[0x21] << 8)
                | ((unsigned long)rec[0x22] << 16) | ((unsigned long)rec[0x23] << 24);
        f->name = (const char *)rec + 0x2C;
        f->shortname = rec[0x130] ? (const char *)rec + 0x130 : f->name;
    } else {
        f->attr = rec[0x15];
        f->time = (unsigned short)(rec[0x16] | (rec[0x17] << 8));
        f->date = (unsigned short)(rec[0x18] | (rec[0x19] << 8));
        f->size = (unsigned long)rec[0x1A] | ((unsigned long)rec[0x1B] << 8)
                | ((unsigned long)rec[0x1C] << 16) | ((unsigned long)rec[0x1D] << 24);
        f->name = (const char *)rec + 0x1E;
        f->shortname = f->name;
    }
}

struct __pm_dir {
    int            lfn;                 /* found with 714Eh */
    int            handle;              /* 714Eh's, or -1 */
    int            pending;             /* rec holds an entry not yet read */
    char           pattern[264];
    unsigned char  rec[320];
    struct dirent  ent;
};

/* first match of pattern into d->rec: 0, or a DOS error code */
static int find_first( DIR *d )
{
    pm_regs regs;

    for ( ;; ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)d->pattern;
        regs.ecx = ATTR_ALL;
        if ( __pm_lfn ) {
            regs.edi = (uint32_t)d->rec;
            regs.esi = 1;               /* DOS times */
            if ( !__pm_dos( &regs, 0x714E ) ) {
                d->lfn = 1;
                d->handle = (int)PM_AX( regs );
                return 0;
            }
            if ( __pm_lfn_missing( &regs ) ) {
                continue;
            }
            return (int)PM_AX( regs );
        }
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)d->rec;
        __pm_dos( &regs, 0x1A00 );      /* the DTA */
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)d->pattern;
        regs.ecx = ATTR_ALL;
        d->lfn = 0;
        d->handle = -1;
        return __pm_dos( &regs, 0x4E00 ) ? (int)PM_AX( regs ) : 0;
    }
}

static int find_next( DIR *d )
{
    pm_regs regs;

    memset( &regs, 0, sizeof( regs ) );
    if ( d->lfn ) {
        regs.ebx = (uint32_t)d->handle;
        regs.edi = (uint32_t)d->rec;
        regs.esi = 1;
        return __pm_dos( &regs, 0x714F ) ? (int)PM_AX( regs ) : 0;
    }
    regs.edx = (uint32_t)d->rec;
    __pm_dos( &regs, 0x1A00 );          /* another find may have moved it */
    memset( &regs, 0, sizeof( regs ) );
    return __pm_dos( &regs, 0x4F00 ) ? (int)PM_AX( regs ) : 0;
}

static void find_close( DIR *d )
{
    pm_regs regs;

    if ( d->lfn && d->handle >= 0 ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.ebx = (uint32_t)d->handle;
        __pm_dos( &regs, 0x71A1 );
        d->handle = -1;
    }
}

DIR *opendir( const char *path )
{
    DIR *d;
    size_t len = strlen( path );
    int err;

    if ( len + 5 > sizeof( d->pattern ) ) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    d = (DIR *)calloc( 1, sizeof( DIR ) );
    if ( d == NULL ) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy( d->pattern, path, len );
    if ( len && path[len - 1] != '\\' && path[len - 1] != '/' && path[len - 1] != ':' ) {
        d->pattern[len++] = '\\';
    }
    memcpy( d->pattern + len, "*.*", 4 );
    d->handle = -1;
    err = find_first( d );
    if ( err == 0 ) {
        d->pending = 1;
    } else if ( err != 0x12 && err != 0x02 ) {
        /* "no more files", or PM-DOS's "file not found" in its place:
           empty, but there.  A directory that is not there is 3 */
        _doserrno = err;
        errno = pm_errno_of( err );
        free( d );
        return NULL;
    }
    return d;
}

struct dirent *readdir( DIR *d )
{
    FOUND f;

    if ( !d->pending ) {
        if ( (d->handle < 0 && d->lfn) || find_next( d ) != 0 ) {
            return NULL;
        }
    }
    d->pending = 0;
    from_record( d->rec, d->lfn, &f );
    strncpy( d->ent.d_name, f.name, sizeof( d->ent.d_name ) - 1 );
    d->ent.d_name[sizeof( d->ent.d_name ) - 1] = '\0';
    strncpy( d->ent.d_short, f.shortname, sizeof( d->ent.d_short ) - 1 );
    d->ent.d_short[sizeof( d->ent.d_short ) - 1] = '\0';
    d->ent.d_attr = f.attr;
    d->ent.d_time = f.time;
    d->ent.d_date = f.date;
    d->ent.d_size = f.size;
    return &d->ent;
}

void rewinddir( DIR *d )
{
    find_close( d );
    d->pending = find_first( d ) == 0;
}

int closedir( DIR *d )
{
    if ( d == NULL ) {
        return __pm_errno( EBADF );
    }
    find_close( d );
    free( d );
    return 0;
}

/*--- stat ---------------------------------------------------------------*/

/* a DOS date and time, local, as a time_t */
static time_t dos_time( unsigned date, unsigned tim )
{
    struct tm tm;

    memset( &tm, 0, sizeof( tm ) );
    tm.tm_year = (int)((date >> 9) & 0x7F) + 80;
    tm.tm_mon = (int)((date >> 5) & 0x0F) - 1;
    tm.tm_mday = (int)(date & 0x1F);
    tm.tm_hour = (int)((tim >> 11) & 0x1F);
    tm.tm_min = (int)((tim >> 5) & 0x3F);
    tm.tm_sec = (int)(tim & 0x1F) * 2;
    tm.tm_isdst = -1;
    return mktime( &tm );
}

static int exe_name( const char *name )
{
    const char *dot = strrchr( name, '.' );

    return dot != NULL && (stricmp( dot, ".exe" ) == 0 || stricmp( dot, ".com" ) == 0
                           || stricmp( dot, ".bat" ) == 0);
}

int stat( const char *path, struct stat *st )
{
    DIR *d;
    FOUND f;
    size_t len = strlen( path );
    int err;

    memset( st, 0, sizeof( *st ) );
    st->st_nlink = 1;
    st->st_dev = (dev_t)((len >= 2 && path[1] == ':') ? (path[0] | 0x20) - 'a' : _getdrive() - 1);
    if ( len == 0 || strpbrk( path, "*?" ) != NULL ) {
        return __pm_errno( ENOENT );
    }
    if ( len + 1 > sizeof( d->pattern ) ) {
        return __pm_errno( ENAMETOOLONG );
    }
    d = (DIR *)calloc( 1, sizeof( DIR ) );
    if ( d == NULL ) {
        return __pm_errno( ENOMEM );
    }
    memcpy( d->pattern, path, len + 1 );
    d->handle = -1;
    err = find_first( d );
    find_close( d );
    if ( err == 0 ) {
        from_record( d->rec, d->lfn, &f );
        st->st_mode = (f.attr & ATTR_DIR) ? (S_IFDIR | S_IEXEC) : S_IFREG;
        st->st_mode |= S_IREAD;
        if ( !(f.attr & ATTR_RDONLY) ) {
            st->st_mode |= S_IWRITE;
        }
        if ( exe_name( f.name ) ) {
            st->st_mode |= S_IEXEC;
        }
        st->st_size = (off_t)f.size;
        st->st_mtime = st->st_atime = st->st_ctime = dos_time( f.date, f.time );
        free( d );
        return 0;
    }
    free( d );
    /* a root, ".", ".." and the like cannot be found, only looked in */
    d = opendir( path );
    if ( d != NULL ) {
        closedir( d );
        st->st_mode = S_IFDIR | S_IREAD | S_IWRITE | S_IEXEC;
        return 0;
    }
    _doserrno = err;
    return __pm_errno( pm_errno_of( err ) );
}

int fstat( int handle, struct stat *st )
{
    pm_regs regs = { 0 };
    long size;

    memset( st, 0, sizeof( *st ) );
    st->st_nlink = 1;
    if ( isatty( handle ) ) {
        st->st_mode = S_IFCHR | S_IREAD | S_IWRITE;
        return 0;
    }
    size = filelength( handle );
    if ( size < 0 ) {
        return -1;
    }
    st->st_mode = S_IFREG | S_IREAD | S_IWRITE;
    st->st_size = size;
    regs.ebx = (uint32_t)handle;
    if ( !__pm_dos( &regs, 0x5700 ) ) {
        st->st_mtime = st->st_atime = st->st_ctime
            = dos_time( regs.edx & 0xFFFF, regs.ecx & 0xFFFF );
    }
    return 0;
}

int chmod( const char *path, int mode )
{
    pm_regs regs;
    uint32_t attr;

    for ( ;; ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)path;
        if ( !__pm_dos( &regs, __pm_lfn ? 0x7143 : 0x4300 ) ) {
            break;
        }
        if ( !__pm_lfn_missing( &regs ) ) {
            return __pm_fail( &regs );
        }
    }
    attr = regs.ecx & 0xFF & ~(uint32_t)ATTR_DIR;
    attr = (mode & S_IWRITE) ? attr & ~(uint32_t)ATTR_RDONLY : attr | ATTR_RDONLY;
    memset( &regs, 0, sizeof( regs ) );
    regs.edx = (uint32_t)path;
    regs.ebx = 1;                       /* 7143h: BL = 1 sets */
    regs.ecx = attr;
    return __pm_dos( &regs, __pm_lfn ? 0x7143 : 0x4301 ) ? __pm_fail( &regs ) : 0;
}

mode_t umask( mode_t mask )
{
    (void)mask;
    return 0;                           /* DOS has no permissions to mask */
}
