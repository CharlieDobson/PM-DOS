/*
 * SYS/TYPES.H - the types SYS/STAT.H and other POSIX headers use
 */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <_pmdefs.h>

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __pm_size_t size_t;
#endif

typedef __pm_off_t      off_t;
#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
typedef __pm_time_t     time_t;
#endif
typedef int             ssize_t;
typedef unsigned short  mode_t;
typedef short           dev_t;
typedef unsigned short  ino_t;
typedef short           nlink_t;
typedef short           uid_t;
typedef short           gid_t;
typedef int             pid_t;

#endif
