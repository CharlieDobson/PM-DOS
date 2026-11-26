/*
 * TYPES.H - basic types for MOUNT.
 *
 * The program is built without any C runtime library, so everything it
 * needs is declared here or in RT.H.  Open Watcom's "char" is unsigned
 * by default.  The same as PMWRITE's TYPES.H, because PMWRITE's FLATE.C
 * and CRC.C are compiled into MOUNT as they stand.
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif
