/*
 * DRVRT.C - the little C runtime the SMBFS driver and the SMB2/crypto/TLS
 * modules it links need.
 *
 * A 32-bit DEVICE= driver has no C runtime linked (like a native program,
 * -zl), so the handful of standard functions the vendored SMB2 engine and
 * the crypto call - memcpy, memset and friends - are supplied here.  No
 * malloc: the driver's memory comes from NDS_ALLOC, and the SMB2 engine
 * keeps its buffers in its own fixed-size struct.
 */
#include <stddef.h>
#include <stdint.h>

void *memcpy( void *to, const void *from, size_t len )
{
    uint8_t *dst = (uint8_t *)to;
    const uint8_t *src = (const uint8_t *)from;
    while ( len-- ) *dst++ = *src++;
    return to;
}

void *memmove( void *to, const void *from, size_t len )
{
    uint8_t *dst = (uint8_t *)to;
    const uint8_t *src = (const uint8_t *)from;
    if ( dst < src ) {
        while ( len-- ) *dst++ = *src++;
    } else {
        dst += len; src += len;
        while ( len-- ) *--dst = *--src;
    }
    return to;
}

void *memset( void *to, int value, size_t len )
{
    uint8_t *dst = (uint8_t *)to;
    while ( len-- ) *dst++ = (uint8_t)value;
    return to;
}

int memcmp( const void *aptr, const void *bptr, size_t len )
{
    const uint8_t *aa = (const uint8_t *)aptr, *bb = (const uint8_t *)bptr;
    while ( len-- ) {
        if ( *aa != *bb ) return (int)*aa - (int)*bb;
        aa++; bb++;
    }
    return 0;
}
