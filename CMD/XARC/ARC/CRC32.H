/*===========================================================================
 * CRC32.H  -  CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320)
 * Target: MSVC 2.2  Win32s
 *===========================================================================*/

#ifndef CRC32_H
#define CRC32_H

#include "arcdefs.h"

/* Running CRC.  Start with Crc32Init(), feed buffers, finish with
 * Crc32Done() which XORs with 0xFFFFFFFF. */
uint32_t Crc32Init( void );
uint32_t Crc32Update( uint32_t crc, const void *data, uint32_t len );
uint32_t Crc32Done( uint32_t crc );

/* One-shot helper: CRC of a single buffer. */
uint32_t Crc32Calc( const void *data, uint32_t len );

#endif /* CRC32_H */
