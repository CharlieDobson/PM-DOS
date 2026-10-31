/*
 * DOS.C - the calls into the kernel every other module makes, and
 * errno
 */
#include "pmlib.h"

int errno;
int _doserrno;
int __pm_lfn = 1;
void (*__pm_exit_flush)( void );

int __pm_dos( pm_regs *regs, uint32_t eax )
{
    regs->eax = eax;
    regs->flags = 0;
    return pm_int21( regs );
}

int __pm_fail( const pm_regs *regs )
{
    _doserrno = (int)PM_AX( *regs );
    errno = pm_errno_of( _doserrno );
    return -1;
}

int __pm_errno( int e )
{
    errno = e;
    return -1;
}

int __pm_lfn_missing( const pm_regs *regs )
{
    if ( PM_CF( *regs ) && PM_AX( *regs ) == 0x7100 ) {
        __pm_lfn = 0;
        return 1;
    }
    return 0;
}

int pm_errno_of( int doserr )
{
    switch ( doserr ) {
    case 0x01:                          /* invalid function */
        return ENOSYS;
    case 0x02:                          /* file not found */
    case 0x03:                          /* path not found */
    case 0x12:                          /* no more files */
        return ENOENT;
    case 0x04:                          /* no handles left */
        return EMFILE;
    case 0x05:                          /* access denied */
    case 0x20:                          /* sharing violation */
    case 0x21:                          /* lock violation */
    case 0x52:                          /* cannot make the directory */
        return EACCES;
    case 0x06:                          /* invalid handle */
        return EBADF;
    case 0x07:                          /* memory blocks destroyed */
    case 0x08:                          /* not enough memory */
        return ENOMEM;
    case 0x0B:                          /* not a program */
        return ENOEXEC;
    case 0x0F:                          /* no such drive */
    case 0x14:                          /* unknown unit */
        return ENODEV;
    case 0x10:                          /* the current directory */
        return EBUSY;
    case 0x11:                          /* not the same drive */
        return EXDEV;
    case 0x13:                          /* write-protected */
        return EROFS;
    case 0x27:                          /* disk full */
    case 0x70:
        return ENOSPC;
    case 0x50:                          /* file exists */
    case 0xB7:                          /* (Windows 95's number for it) */
        return EEXIST;
    }
    if ( doserr >= 0x15 && doserr <= 0x1F ) {
        return EIO;                     /* not ready, CRC, seek, write fault... */
    }
    return EINVAL;
}

int pm_version( void )
{
    pm_regs regs = { 0 };

    __pm_dos( &regs, 0xF100 );
    return (int)((regs.ebx >> 8) & 0xFF) * 100 + (int)(regs.ebx & 0xFF);
}
