/*
 * TYPES.H - basic types for LABEL/32
 *
 * The program is built without any C runtime library, so everything it
 * needs is declared here or in RT.H.  Note that Open Watcom's "char" is
 * unsigned by default; on-disk data is always handled as u8 anyway.
 */
#ifndef TYPES_H
#define TYPES_H

typedef unsigned char    u8;
typedef unsigned short   u16;
typedef unsigned long    u32;
typedef long             s32;
typedef unsigned __int64 u64;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* little-endian field access for on-disk structures (no packing games) */
#define RD16( ptr )      ((u16)(((const u8 *)(ptr))[0] | \
                                ((u16)((const u8 *)(ptr))[1] << 8)))
#define RD32( ptr )      ((u32)RD16( ptr ) | \
                          ((u32)RD16( (const u8 *)(ptr) + 2 ) << 16))
#define WR16( ptr, val ) (((u8 *)(ptr))[0] = (u8)(val), \
                          ((u8 *)(ptr))[1] = (u8)((u16)(val) >> 8))
#define WR32( ptr, val ) (WR16( (ptr), (u16)(val) ), \
                          WR16( (u8 *)(ptr) + 2, (u16)((u32)(val) >> 16) ))

#endif
