/* ==========================================================================
 * SMBSIGN.C - AES-128-CMAC (RFC 4493).  C89.
 * ==========================================================================
 * The block cipher is the TLS port's AES-128 (CMD\NET\TLS\AES.C).  Known-
 * answer tests (the RFC 4493 vectors) are in SMBCTEST.C.
 * ========================================================================== */
#include "smbsign.h"
#include "aes.h"
#include "sha.h"
#include <string.h>

/* one-bit left shift of a 16-byte big-endian value */
static void shift_left_1( const uint8_t *in, uint8_t *out )
{
    int           i;
    uint8_t       carry = 0;
    for ( i = 15; i >= 0; i-- ) {
        out[i] = (uint8_t)( ( in[i] << 1 ) | carry );
        carry = (uint8_t)( ( in[i] >> 7 ) & 1 );
    }
}

static void xor16( const uint8_t *a, const uint8_t *b, uint8_t *out )
{
    int i;
    for ( i = 0; i < 16; i++ ) out[i] = (uint8_t)( a[i] ^ b[i] );
}

void aes_cmac( const uint8_t key[16], const uint8_t *msg, int len,
               uint8_t mac[16] )
{
    uint8_t       rk[AES128_RK_BYTES];
    uint8_t       zero[16], ltab[16], k1[16], k2[16];
    uint8_t       block[16], last[16], x[16], y[16];
    int           complete, blocks, i, off;
    const uint8_t rb = 0x87;

    aes128_expand_key( key, rk );

    /* subkeys: L = AES(K, 0); K1 = L<<1 (^Rb if MSB set); K2 = K1<<1 (likewise) */
    memset( zero, 0, 16 );
    aes128_encrypt( rk, zero, ltab );
    shift_left_1( ltab, k1 );
    if ( ltab[0] & 0x80 ) k1[15] = (uint8_t)( k1[15] ^ rb );
    shift_left_1( k1, k2 );
    if ( k1[0] & 0x80 ) k2[15] = (uint8_t)( k2[15] ^ rb );

    /* number of blocks; a message of 0 bytes counts as one (incomplete) block */
    complete = ( len > 0 && ( len % 16 ) == 0 );
    blocks = ( len + 15 ) / 16;
    if ( blocks == 0 ) blocks = 1;

    /* last block: XOR K1 if the message filled it, else pad (0x80 00..) and K2 */
    off = ( blocks - 1 ) * 16;
    if ( complete ) {
        xor16( msg + off, k1, last );
    } else {
        int rem = len - off;
        memset( block, 0, 16 );
        for ( i = 0; i < rem; i++ ) block[i] = msg[off + i];
        block[rem] = 0x80;
        xor16( block, k2, last );
    }

    /* CBC-MAC with a zero IV; the last block is 'last' */
    memset( x, 0, 16 );
    for ( i = 0; i < blocks - 1; i++ ) {
        xor16( x, msg + i * 16, y );
        aes128_encrypt( rk, y, x );
    }
    xor16( x, last, y );
    aes128_encrypt( rk, y, mac );
}

void smb3_kdf( const uint8_t key[16],
               const uint8_t *label, int labellen,
               const uint8_t *context, int contextlen,
               uint8_t out[16] )
{
    uint8_t input[256];
    uint8_t mac[32];
    int pos = 0;

    input[pos++] = 0; input[pos++] = 0; input[pos++] = 0; input[pos++] = 1;   /* i = 1 */
    memcpy( input + pos, label, labellen ); pos += labellen;
    input[pos++] = 0;                                   /* SP800-108 separator */
    memcpy( input + pos, context, contextlen ); pos += contextlen;
    input[pos++] = 0; input[pos++] = 0; input[pos++] = 0; input[pos++] = 0x80; /* L = 128 */

    hmac_sha256( key, 16, input, pos, mac );
    memcpy( out, mac, 16 );
}
