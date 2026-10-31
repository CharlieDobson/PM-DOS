/*
 * ERRNO.H - error numbers
 *
 * The values are the usual POSIX ones.  The DOS error code behind
 * the last failure is in _doserrno (PMDOS.H).
 */
#ifndef _ERRNO_H
#define _ERRNO_H

#include <_pmdefs.h>

_PM_BEGIN_C
extern int errno;
_PM_END_C

#define EPERM           1       /* not allowed */
#define ENOENT          2       /* no such file or directory */
#define ESRCH           3
#define EINTR           4       /* stopped by Ctrl+C */
#define EIO             5       /* the device failed */
#define ENXIO           6
#define E2BIG           7       /* argument list too long */
#define ENOEXEC         8       /* not a program */
#define EBADF           9       /* not an open handle */
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12      /* out of memory */
#define EACCES          13      /* access denied */
#define EFAULT          14
#define EBUSY           16      /* in use */
#define EEXIST          17      /* already exists */
#define EXDEV           18      /* not the same drive */
#define ENODEV          19      /* no such drive */
#define ENOTDIR         20      /* not a directory */
#define EISDIR          21      /* is a directory */
#define EINVAL          22      /* invalid argument */
#define ENFILE          23
#define EMFILE          24      /* too many open files */
#define ENOTTY          25
#define EFBIG           27      /* file too large */
#define ENOSPC          28      /* disk full */
#define ESPIPE          29      /* cannot seek on a device */
#define EROFS           30      /* write-protected */
#define EMLINK          31
#define EPIPE           32
#define EDOM            33      /* math argument out of range */
#define ERANGE          34      /* result out of range */
#define EDEADLK         36
#define ENAMETOOLONG    38      /* name too long */
#define ENOLCK          39
#define ENOSYS          40      /* not supported */
#define ENOTEMPTY       41      /* directory not empty */
#define EILSEQ          42      /* bad multibyte sequence */

#endif
