/*
 * TYPES.H - basic types for DOSSHELL.
 *
 * The program is built without any C runtime library, so everything it
 * needs is declared here or in RT.H.  Open Watcom's "char" is unsigned
 * by default.
 */
#ifndef TYPES_H
#define TYPES_H

typedef unsigned char    u8;
typedef unsigned short   u16;
typedef unsigned long    u32;
typedef long             s32;

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif
