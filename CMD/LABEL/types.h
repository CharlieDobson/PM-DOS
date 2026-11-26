/*
 * TYPES.H - basic types for LABEL/32
 *
 * The program is built without any C runtime library, so everything it
 * needs is declared here or in RT.H.  Note that Open Watcom's "char" is
 * unsigned by default; on-disk data is always handled as uint8_t anyway.
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

/* little-endian field access for on-disk structures (no packing games) */
#define RD16( ptr )      ((uint16_t)(((const uint8_t *)(ptr))[0] | \
                                     ((uint16_t)((const uint8_t *)(ptr))[1] << 8)))
#define RD32( ptr )      ((uint32_t)RD16( ptr ) | \
                          ((uint32_t)RD16( (const uint8_t *)(ptr) + 2 ) << 16))
#define WR16( ptr, val ) (((uint8_t *)(ptr))[0] = (uint8_t)(val), \
                          ((uint8_t *)(ptr))[1] = (uint8_t)((uint16_t)(val) >> 8))
#define WR32( ptr, val ) (WR16( (ptr), (uint16_t)(val) ), \
                          WR16( (uint8_t *)(ptr) + 2, (uint16_t)((uint32_t)(val) >> 16) ))

#endif
