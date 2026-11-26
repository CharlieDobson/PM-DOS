/*
 * IMZ.H - a .IMZ: a diskette image in a ZIP archive, the way WinImage
 * keeps one - a single member, the .IMA, deflated or stored.
 *
 * Both directions work on whole blocks in memory.  The packing itself
 * is PMWRITE's FLATE.C and the CRC is its CRC.C.
 */
#ifndef IMZ_H
#define IMZ_H

#include "types.h"

/* why imz_unpack gave NULL */
#define IMZ_OK          0
#define IMZ_NOMEM       1       /* no memory */
#define IMZ_NOTZIP      2       /* not a ZIP archive, or one that does not add up */
#define IMZ_UNSUPP      3       /* encrypted, zip64, split, or not stored or deflated */
#define IMZ_EMPTY       4       /* no file in it */
#define IMZ_TOOBIG      5       /* the image is bigger than "max" */
#define IMZ_BADCRC      6       /* unpacked, and the CRC says it is wrong */

#define IMZ_NAME_MAX    256     /* a member's name, NUL included */

/*
 * The image in the "len" bytes of a .IMZ at "zip": a heap block for the
 * caller to xfree, *img_len long and no more than "max".  The first
 * file in the archive is the image; "name" (IMZ_NAME_MAX bytes) gets
 * its name, and *others is how many more files the archive holds.
 * NULL, and *why says why.
 */
uint8_t *imz_unpack( const uint8_t *zip, uint32_t len, uint32_t max, uint32_t *img_len,
                     char *name, unsigned int *others, int *why );

/*
 * A .IMZ holding the "len" bytes at "img" as the member "name": a heap
 * block for the caller to xfree, *zip_len long.  Deflated, or stored
 * when deflating does not make it smaller.  NULL if there is no memory.
 */
uint8_t *imz_pack( const uint8_t *img, uint32_t len, const char *name, uint32_t *zip_len );

/*
 * The first file's name from the end of an archive: "len" bytes at
 * "tail" that were read from offset "base" of the file, which hold the
 * central directory and the record after it.  1 and "name" filled in
 * (IMZ_NAME_MAX bytes) when it is there.
 */
int  imz_name( const uint8_t *tail, uint32_t len, uint32_t base, char *name );

#endif
