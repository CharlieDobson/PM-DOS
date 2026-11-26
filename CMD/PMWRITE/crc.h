/*
 * CRC.H - the two checksums the file formats use.
 *
 * CRC-32 is the one ZIP and PNG keep (the reflected polynomial
 * EDB88320h); Adler-32 is the one at the end of a zlib stream, which
 * is what PDF's FlateDecode and PNG's IDAT hold.  Both are given the
 * sum so far and return the sum with "len" more bytes in it, so a
 * file can be summed a piece at a time.
 */
#ifndef CRC_H
#define CRC_H

#include "types.h"

/* start with 0 */
uint32_t crc32_update( uint32_t crc, const uint8_t *data, uint32_t len );
/* start with 1 */
uint32_t adler32_update( uint32_t adler, const uint8_t *data, uint32_t len );

#endif
