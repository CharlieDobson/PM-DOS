/*
 * IO.C - files by handle (IO.H)
 *
 * Every call here is one INT 21h.  PM-DOS widens the registers: a name
 * is a pointer in EDX (or ESI for 716Ch), a count is ECX, and 42h
 * takes the whole offset in ECX and gives the position back in EAX.
 */
#include "pmlib.h"
#include <io.h>
#include <stdarg.h>
#include <string.h>

#define ACT_OPEN        0x01            /* 6Ch's DX: open it if it is there */
#define ACT_TRUNC       0x02            /* ...and empty it */
#define ACT_CREATE      0x10            /* create it if it is not */
#define ATTR_RDONLY     0x01

/* O_APPEND, by handle: write() goes to the end first */
static uint32_t append_bits[8];
#define APPENDING( h )  ((unsigned)(h) < 256 && (append_bits[(unsigned)(h) >> 5] & (1UL << ((h) & 31))))

int open( const char *name, int flags, ... )
{
    pm_regs regs;
    va_list args;
    int mode = 0666;
    uint32_t action = 0;

    if ( flags & O_CREAT ) {
        va_start( args, flags );
        mode = va_arg( args, int );
        va_end( args );
    }
    if ( flags & O_CREAT ) {
        action |= ACT_CREATE;
    }
    if ( !(flags & O_EXCL) || !(flags & O_CREAT) ) {
        action |= (flags & O_TRUNC) ? ACT_TRUNC : ACT_OPEN;
    }
    for ( ;; ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.ebx = (uint32_t)(flags & O_ACCMODE);
        regs.ecx = (mode & 0200) ? 0 : ATTR_RDONLY;
        regs.edx = action;
        regs.esi = (uint32_t)name;
        if ( !__pm_dos( &regs, __pm_lfn ? 0x716C : 0x6C00 ) ) {
            break;
        }
        if ( !__pm_lfn_missing( &regs ) ) {
            if ( (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL) && PM_AX( regs ) == 0x50 ) {
                _doserrno = 0x50;
                return __pm_errno( EEXIST );
            }
            return __pm_fail( &regs );
        }
    }
    if ( (flags & O_APPEND) && PM_AX( regs ) < 256 ) {
        append_bits[PM_AX( regs ) >> 5] |= 1UL << (PM_AX( regs ) & 31);
    }
    return (int)PM_AX( regs );
}

int creat( const char *name, int mode )
{
    return open( name, O_WRONLY | O_CREAT | O_TRUNC, mode );
}

int close( int handle )
{
    pm_regs regs = { 0 };

    if ( (unsigned)handle < 256 ) {
        append_bits[(unsigned)handle >> 5] &= ~(1UL << (handle & 31));
    }
    regs.ebx = (uint32_t)handle;
    return __pm_dos( &regs, 0x3E00 ) ? __pm_fail( &regs ) : 0;
}

int read( int handle, void *buf, unsigned count )
{
    pm_regs regs = { 0 };

    regs.ebx = (uint32_t)handle;
    regs.ecx = count;
    regs.edx = (uint32_t)buf;
    return __pm_dos( &regs, 0x3F00 ) ? __pm_fail( &regs ) : (int)regs.eax;
}

int write( int handle, const void *buf, unsigned count )
{
    pm_regs regs = { 0 };

    if ( APPENDING( handle ) && lseek( handle, 0, SEEK_END ) < 0 ) {
        return -1;
    }
    regs.ebx = (uint32_t)handle;
    regs.ecx = count;
    regs.edx = (uint32_t)buf;
    if ( __pm_dos( &regs, 0x4000 ) ) {
        return __pm_fail( &regs );
    }
    if ( count != 0 && regs.eax == 0 ) {
        _doserrno = 0x27;               /* DOS says nothing: the disk is full */
        return __pm_errno( ENOSPC );
    }
    return (int)regs.eax;
}

long lseek( int handle, long offset, int origin )
{
    pm_regs regs = { 0 };

    if ( origin < SEEK_SET || origin > SEEK_END ) {
        return __pm_errno( EINVAL );
    }
    regs.ebx = (uint32_t)handle;
    regs.ecx = (uint32_t)offset;
    if ( __pm_dos( &regs, 0x4200 | (uint32_t)origin ) ) {
        if ( isatty( handle ) ) {
            return __pm_errno( ESPIPE );
        }
        return __pm_fail( &regs );
    }
    return (long)regs.eax;
}

long tell( int handle )
{
    return lseek( handle, 0, SEEK_CUR );
}

long filelength( int handle )
{
    long here = tell( handle );
    long size;

    if ( here < 0 ) {
        return -1;
    }
    size = lseek( handle, 0, SEEK_END );
    lseek( handle, here, SEEK_SET );
    return size;
}

int eof( int handle )
{
    long here = tell( handle );
    long size = filelength( handle );

    if ( here < 0 || size < 0 ) {
        return -1;
    }
    return here >= size;
}

/* 4400h: bit 7 of DX is set for a device */
int isatty( int handle )
{
    pm_regs regs = { 0 };

    regs.ebx = (uint32_t)handle;
    if ( __pm_dos( &regs, 0x4400 ) ) {
        __pm_fail( &regs );
        return 0;
    }
    return (regs.edx & 0x80) != 0;
}

int dup( int handle )
{
    pm_regs regs = { 0 };

    regs.ebx = (uint32_t)handle;
    return __pm_dos( &regs, 0x4500 ) ? __pm_fail( &regs ) : (int)PM_AX( regs );
}

int dup2( int handle, int handle2 )
{
    pm_regs regs = { 0 };

    regs.ebx = (uint32_t)handle;
    regs.ecx = (uint32_t)handle2;
    return __pm_dos( &regs, 0x4600 ) ? __pm_fail( &regs ) : handle2;
}

int unlink( const char *name )
{
    pm_regs regs;

    for ( ;; ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)name;
        if ( !__pm_dos( &regs, __pm_lfn ? 0x7141 : 0x4100 ) ) {
            return 0;
        }
        if ( !__pm_lfn_missing( &regs ) ) {
            return __pm_fail( &regs );
        }
    }
}

/* 7143h BL=0 / 4300h: the attributes in CX */
int access( const char *name, int mode )
{
    pm_regs regs;

    for ( ;; ) {
        memset( &regs, 0, sizeof( regs ) );
        regs.edx = (uint32_t)name;
        if ( !__pm_dos( &regs, __pm_lfn ? 0x7143 : 0x4300 ) ) {
            break;
        }
        if ( !__pm_lfn_missing( &regs ) ) {
            return __pm_fail( &regs );
        }
    }
    if ( (mode & W_OK) && (regs.ecx & ATTR_RDONLY) ) {
        _doserrno = 0x05;
        return __pm_errno( EACCES );
    }
    return 0;
}

int setmode( int handle, int mode )
{
    (void)handle;
    (void)mode;
    return O_BINARY;                    /* handles never translate */
}
