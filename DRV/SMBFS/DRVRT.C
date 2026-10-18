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
typedef unsigned u_size;
typedef unsigned char u8;

void *memcpy( void *to, const void *from, u_size len )
{
    u8 *dst = (u8 *)to;
    const u8 *src = (const u8 *)from;
    while ( len-- ) *dst++ = *src++;
    return to;
}

void *memmove( void *to, const void *from, u_size len )
{
    u8 *dst = (u8 *)to;
    const u8 *src = (const u8 *)from;
    if ( dst < src ) {
        while ( len-- ) *dst++ = *src++;
    } else {
        dst += len; src += len;
        while ( len-- ) *--dst = *--src;
    }
    return to;
}

void *memset( void *to, int value, u_size len )
{
    u8 *dst = (u8 *)to;
    while ( len-- ) *dst++ = (u8)value;
    return to;
}

int memcmp( const void *aptr, const void *bptr, u_size len )
{
    const u8 *aa = (const u8 *)aptr, *bb = (const u8 *)bptr;
    while ( len-- ) {
        if ( *aa != *bb ) return (int)*aa - (int)*bb;
        aa++; bb++;
    }
    return 0;
}
