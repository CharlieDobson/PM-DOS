/*
 * TYPES.H - SETUP's basic types, the same in both halves.
 *
 * Open Watcom's "char" is unsigned by default, 16-bit and 32-bit alike;
 * on-disk data is handled as uint8_t anyway.  <stdint.h> gives uint32_t
 * the same 32 bits in the 16-bit half and the 32-bit half.
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
