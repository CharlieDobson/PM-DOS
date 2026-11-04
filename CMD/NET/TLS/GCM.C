/* ==========================================================================
 * GCM.C - AES-GCM AEAD (see GCM.H).  C89; MSVC 2.2 + OW 1.9 clean.
 * ==========================================================================
 * GHASH multiplies by H four bits at a time (Shoup's method): a 16-entry
 * table of H's multiples is made once per record, and each block then takes
 * 32 table steps instead of 128 bit steps.  Values are held as four 32-bit
 * words, most significant first, so x^0 (GCM's first bit) is the top bit of
 * word 0, and multiplying by x shifts right.  The reduction polynomial is
 * R = 0xE1 || 0^120.  Counter mode is AES.C's aes_ctr32.  Lengths are TLS-
 * record sized, so 32-bit bit-lengths are enough (no 64-bit type needed).
 * ========================================================================== */
#include "gcm.h"
#include "aes.h"
#include <string.h>

typedef unsigned int u32;           /* 32-bit on all Win32 targets */

/* What multiplying by x^4 pushes off the end, folded back in by R: the top
   16 bits of word 0 for each value of the 4 bits shifted out. */
static const u32 last4[16] = {
    0x00000000UL, 0x1c200000UL, 0x38400000UL, 0x24600000UL,
    0x70800000UL, 0x6ca00000UL, 0x48c00000UL, 0x54e00000UL,
    0xe1000000UL, 0xfd200000UL, 0xd9400000UL, 0xc5600000UL,
    0x91800000UL, 0x8da00000UL, 0xa9c00000UL, 0xb5e00000UL
};

static u32 load_be( const unsigned char *p )
{
    return ( (u32)p[0] << 24 ) | ( (u32)p[1] << 16 ) | ( (u32)p[2] << 8 ) | (u32)p[3];
}

static void store_be( unsigned char *p, u32 word )
{
    p[0] = (unsigned char)( word >> 24 );
    p[1] = (unsigned char)( word >> 16 );
    p[2] = (unsigned char)( word >> 8 );
    p[3] = (unsigned char)word;
}

/* tab[n] = n . H for each 4-bit n, where n's top bit (8) is x^0 and its
   bottom bit (1) is x^3 - the order of a nibble's bits in a GCM block. */
static void ghash_table( const unsigned char H[16], u32 tab[16][4] )
{
    int i, j, w;
    u32 lsb;

    for ( w = 0; w < 4; w++ ) {
        tab[0][w] = 0;
        tab[8][w] = load_be( H + 4 * w );
    }
    for ( i = 4; i > 0; i >>= 1 ) {             /* tab[i] = tab[2i] . x */
        lsb = tab[2 * i][3] & 1;
        tab[i][3] = ( tab[2 * i][3] >> 1 ) | ( tab[2 * i][2] << 31 );
        tab[i][2] = ( tab[2 * i][2] >> 1 ) | ( tab[2 * i][1] << 31 );
        tab[i][1] = ( tab[2 * i][1] >> 1 ) | ( tab[2 * i][0] << 31 );
        tab[i][0] = ( tab[2 * i][0] >> 1 ) ^ ( lsb ? 0xe1000000UL : 0 );
    }
    for ( i = 2; i < 16; i <<= 1 ) {            /* the rest by sums */
        for ( j = 1; j < i; j++ ) {
            for ( w = 0; w < 4; w++ ) tab[i + j][w] = tab[i][w] ^ tab[j][w];
        }
    }
}

/* val = val . H.  The nibbles go last first (Horner's rule): word 3's low
   nibble is x^124..x^127, word 0's top nibble x^0..x^3. */
static void ghash_mult( u32 val[4], const u32 tab[16][4] )
{
    u32 z0 = 0, z1 = 0, z2 = 0, z3 = 0, word, rem;
    const u32 *add;
    int w, i;

    for ( w = 3; w >= 0; w-- ) {
        word = val[w];
        for ( i = 0; i < 8; i++ ) {
            rem = z3 & 0xf;                     /* z = z . x^4 */
            z3 = ( z3 >> 4 ) | ( z2 << 28 );
            z2 = ( z2 >> 4 ) | ( z1 << 28 );
            z1 = ( z1 >> 4 ) | ( z0 << 28 );
            z0 = ( z0 >> 4 ) ^ last4[rem];
            add = tab[word & 0xf];              /* z = z + nibble . H */
            z0 ^= add[0]; z1 ^= add[1]; z2 ^= add[2]; z3 ^= add[3];
            word >>= 4;
        }
    }
    val[0] = z0; val[1] = z1; val[2] = z2; val[3] = z3;
}

/* S = GHASH_H(S || data), zero-padding a final partial block. */
static void ghash_data( const u32 tab[16][4], const unsigned char *data, int len, u32 S[4] )
{
    unsigned char blk[16];
    const unsigned char *src;
    int i, w;

    for ( i = 0; i < len; i += 16 ) {
        src = data + i;
        if ( len - i < 16 ) {
            memset( blk, 0, 16 );
            memcpy( blk, src, len - i );
            src = blk;
        }
        for ( w = 0; w < 4; w++ ) S[w] ^= load_be( src + 4 * w );
        ghash_mult( S, tab );
    }
}

void gcm_calc_h( const unsigned char *rk, int nr, unsigned char H[16] )
{
    unsigned char zero[16];
    memset( zero, 0, 16 );
    aes_encrypt( rk, nr, zero, H );
}

/* J0 = iv || 0^31 || 1, the counter block the tag is masked with; the text
   uses the blocks after it. */
static void gcm_j0( const unsigned char iv[12], unsigned char J0[16] )
{
    memcpy( J0, iv, 12 );
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;
}

/* tag = GHASH over aad || cipher || the length block, masked with E(J0).
   The table is H's multiples, so it is wiped before return. */
static void gcm_tag( const unsigned char *rk, int nr, const unsigned char H[16],
                     const unsigned char J0[16],
                     const unsigned char *aad, int aadlen,
                     const unsigned char *cipher, int clen,
                     unsigned char tag[16] )
{
    u32 tab[16][4], S[4];
    unsigned char ek[16];
    int w;

    ghash_table( H, tab );
    S[0] = 0; S[1] = 0; S[2] = 0; S[3] = 0;
    ghash_data( tab, aad, aadlen, S );
    ghash_data( tab, cipher, clen, S );
    S[1] ^= (u32)aadlen * 8;                    /* [len(AAD)]64 || [len(C)]64 */
    S[3] ^= (u32)clen * 8;
    ghash_mult( S, tab );
    memset( tab, 0, sizeof( tab ) );

    aes_encrypt( rk, nr, J0, ek );
    for ( w = 0; w < 4; w++ ) store_be( tag + 4 * w, S[w] );
    for ( w = 0; w < 16; w++ ) tag[w] ^= ek[w];
}

void gcm_encrypt( const unsigned char *rk, int nr, const unsigned char H[16],
                  const unsigned char iv[12],
                  const unsigned char *aad, int aadlen,
                  const unsigned char *plain, int plen,
                  unsigned char *cipher, unsigned char tag[16] )
{
    unsigned char J0[16], icb[16];

    gcm_j0( iv, J0 );
    memcpy( icb, J0, 16 );
    icb[15] = 2;                                /* inc32(J0) */
    aes_ctr32( rk, nr, icb, plain, plen, cipher );
    gcm_tag( rk, nr, H, J0, aad, aadlen, cipher, plen, tag );
}

int gcm_decrypt( const unsigned char *rk, int nr, const unsigned char H[16],
                 const unsigned char iv[12],
                 const unsigned char *aad, int aadlen,
                 const unsigned char *cipher, int clen,
                 const unsigned char tag[16], unsigned char *plain )
{
    unsigned char J0[16], icb[16], want[16];
    int j, diff = 0;

    gcm_j0( iv, J0 );
    gcm_tag( rk, nr, H, J0, aad, aadlen, cipher, clen, want );
    for ( j = 0; j < 16; j++ ) diff |= ( want[j] ^ tag[j] );     /* constant-ish */
    if ( diff != 0 ) return -1;

    memcpy( icb, J0, 16 );
    icb[15] = 2;                                /* inc32(J0) */
    aes_ctr32( rk, nr, icb, cipher, clen, plain );
    return 0;
}
