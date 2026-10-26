/*
 * SYS\TYPES.H - XARC's C library: the types SYS\STAT.H uses.
 */
#ifndef _XA_SYS_TYPES_H
#define _XA_SYS_TYPES_H

#ifndef _XA_TIME_T
#define _XA_TIME_T
typedef unsigned long time_t;      /* to 2106; DOS dates end in 2107 */
#endif

typedef long off_t;

#endif
