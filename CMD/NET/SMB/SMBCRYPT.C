/* ==========================================================================
 * SMBCRYPT.C - MD4, MD5, HMAC-MD5, RC4 and the NTLMv2 helpers.  C89.
 * ==========================================================================
 * See SMBCRYPT.H.  All four are the textbook algorithms (RFC 1320, 1321,
 * 2104, and Rivest's RC4); the word type is 32-bit unsigned long, which it
 * is on every target this builds for (Open Watcom, 16- and 32-bit DOS and
 * Win32).  Known-answer tests live in SMBCTEST.C.
 * ========================================================================== */
#include "smbcrypt.h"
#include <string.h>

#define ROTL( x, n )  ( ( (x) << (n) ) | ( (x) >> ( 32 - (n) ) ) )

/* read 'count' little-endian 32-bit words out of bytes */
static void decode_le( unsigned long *out, const unsigned char *in, int count )
{
    int i;
    for ( i = 0; i < count; i++ ) {
        out[i] = (unsigned long)in[i * 4] |
                 ( (unsigned long)in[i * 4 + 1] << 8 ) |
                 ( (unsigned long)in[i * 4 + 2] << 16 ) |
                 ( (unsigned long)in[i * 4 + 3] << 24 );
    }
}

/* write 'count' 32-bit words out as little-endian bytes */
static void encode_le( unsigned char *out, const unsigned long *in, int count )
{
    int i;
    for ( i = 0; i < count; i++ ) {
        out[i * 4]     = (unsigned char)( in[i] & 0xFF );
        out[i * 4 + 1] = (unsigned char)( ( in[i] >> 8 ) & 0xFF );
        out[i * 4 + 2] = (unsigned char)( ( in[i] >> 16 ) & 0xFF );
        out[i * 4 + 3] = (unsigned char)( ( in[i] >> 24 ) & 0xFF );
    }
}

/* ==========================================================================
 * MD4
 * ========================================================================== */
#define MD4_F( x, y, z )  ( ( (x) & (y) ) | ( ~(x) & (z) ) )
#define MD4_G( x, y, z )  ( ( (x) & (y) ) | ( (x) & (z) ) | ( (y) & (z) ) )
#define MD4_H( x, y, z )  ( (x) ^ (y) ^ (z) )
#define MD4_FF( a, b, c, d, x, s )  { (a) += MD4_F( (b), (c), (d) ) + (x); (a) = ROTL( (a), (s) ); }
#define MD4_GG( a, b, c, d, x, s )  { (a) += MD4_G( (b), (c), (d) ) + (x) + 0x5A827999UL; (a) = ROTL( (a), (s) ); }
#define MD4_HH( a, b, c, d, x, s )  { (a) += MD4_H( (b), (c), (d) ) + (x) + 0x6ED9EBA1UL; (a) = ROTL( (a), (s) ); }

static void md4_block( unsigned long state[4], const unsigned char block[64] )
{
    unsigned long aa = state[0], bb = state[1], cc = state[2], dd = state[3];
    unsigned long x[16];

    decode_le( x, block, 16 );

    MD4_FF( aa, bb, cc, dd, x[0],  3 )  MD4_FF( dd, aa, bb, cc, x[1],  7 )
    MD4_FF( cc, dd, aa, bb, x[2], 11 )  MD4_FF( bb, cc, dd, aa, x[3], 19 )
    MD4_FF( aa, bb, cc, dd, x[4],  3 )  MD4_FF( dd, aa, bb, cc, x[5],  7 )
    MD4_FF( cc, dd, aa, bb, x[6], 11 )  MD4_FF( bb, cc, dd, aa, x[7], 19 )
    MD4_FF( aa, bb, cc, dd, x[8],  3 )  MD4_FF( dd, aa, bb, cc, x[9],  7 )
    MD4_FF( cc, dd, aa, bb, x[10],11 )  MD4_FF( bb, cc, dd, aa, x[11],19 )
    MD4_FF( aa, bb, cc, dd, x[12], 3 )  MD4_FF( dd, aa, bb, cc, x[13], 7 )
    MD4_FF( cc, dd, aa, bb, x[14],11 )  MD4_FF( bb, cc, dd, aa, x[15],19 )

    MD4_GG( aa, bb, cc, dd, x[0],  3 )  MD4_GG( dd, aa, bb, cc, x[4],  5 )
    MD4_GG( cc, dd, aa, bb, x[8],  9 )  MD4_GG( bb, cc, dd, aa, x[12],13 )
    MD4_GG( aa, bb, cc, dd, x[1],  3 )  MD4_GG( dd, aa, bb, cc, x[5],  5 )
    MD4_GG( cc, dd, aa, bb, x[9],  9 )  MD4_GG( bb, cc, dd, aa, x[13],13 )
    MD4_GG( aa, bb, cc, dd, x[2],  3 )  MD4_GG( dd, aa, bb, cc, x[6],  5 )
    MD4_GG( cc, dd, aa, bb, x[10], 9 )  MD4_GG( bb, cc, dd, aa, x[14],13 )
    MD4_GG( aa, bb, cc, dd, x[3],  3 )  MD4_GG( dd, aa, bb, cc, x[7],  5 )
    MD4_GG( cc, dd, aa, bb, x[11], 9 )  MD4_GG( bb, cc, dd, aa, x[15],13 )

    MD4_HH( aa, bb, cc, dd, x[0],  3 )  MD4_HH( dd, aa, bb, cc, x[8],  9 )
    MD4_HH( cc, dd, aa, bb, x[4], 11 )  MD4_HH( bb, cc, dd, aa, x[12],15 )
    MD4_HH( aa, bb, cc, dd, x[2],  3 )  MD4_HH( dd, aa, bb, cc, x[10], 9 )
    MD4_HH( cc, dd, aa, bb, x[6], 11 )  MD4_HH( bb, cc, dd, aa, x[14],15 )
    MD4_HH( aa, bb, cc, dd, x[1],  3 )  MD4_HH( dd, aa, bb, cc, x[9],  9 )
    MD4_HH( cc, dd, aa, bb, x[5], 11 )  MD4_HH( bb, cc, dd, aa, x[13],15 )
    MD4_HH( aa, bb, cc, dd, x[3],  3 )  MD4_HH( dd, aa, bb, cc, x[11], 9 )
    MD4_HH( cc, dd, aa, bb, x[7], 11 )  MD4_HH( bb, cc, dd, aa, x[15],15 )

    state[0] += aa; state[1] += bb; state[2] += cc; state[3] += dd;
}

void md4_init( MD4_CTX *ctx )
{
    ctx->state[0] = 0x67452301UL; ctx->state[1] = 0xEFCDAB89UL;
    ctx->state[2] = 0x98BADCFEUL; ctx->state[3] = 0x10325476UL;
    ctx->count[0] = ctx->count[1] = 0;
}

/* the fill/flush loop is identical for MD4 and MD5; share it */
static void md_absorb( unsigned long count[2], unsigned char buffer[64],
                       unsigned long state[4], const unsigned char *data,
                       unsigned int len,
                       void ( *block )( unsigned long[4], const unsigned char[64] ) )
{
    unsigned int index = (unsigned int)( ( count[0] >> 3 ) & 0x3F );
    unsigned int partlen, i;

    count[0] += (unsigned long)len << 3;
    if ( count[0] < ( (unsigned long)len << 3 ) ) count[1]++;
    count[1] += (unsigned long)len >> 29;

    partlen = 64 - index;
    if ( len >= partlen ) {
        memcpy( buffer + index, data, partlen );
        block( state, buffer );
        for ( i = partlen; i + 63 < len; i += 64 ) block( state, data + i );
        index = 0;
    } else {
        i = 0;
    }
    memcpy( buffer + index, data + i, len - i );
}

static void md_pad_final( unsigned long count[2], unsigned char buffer[64],
                          unsigned long state[4], unsigned char *digest,
                          void ( *block )( unsigned long[4], const unsigned char[64] ) )
{
    unsigned char bits[8];
    unsigned char pad[64];
    unsigned int  index = (unsigned int)( ( count[0] >> 3 ) & 0x3F );
    unsigned int  padlen;
    unsigned long saved[2];

    saved[0] = count[0]; saved[1] = count[1];
    encode_le( bits, saved, 2 );

    memset( pad, 0, sizeof( pad ) );
    pad[0] = 0x80;
    padlen = ( index < 56 ) ? ( 56 - index ) : ( 120 - index );
    md_absorb( count, buffer, state, pad, padlen, block );
    md_absorb( count, buffer, state, bits, 8, block );
    encode_le( digest, state, 4 );
}

void md4_update( MD4_CTX *ctx, const unsigned char *data, unsigned int len )
{
    md_absorb( ctx->count, ctx->buffer, ctx->state, data, len, md4_block );
}

void md4_final( MD4_CTX *ctx, unsigned char digest[16] )
{
    md_pad_final( ctx->count, ctx->buffer, ctx->state, digest, md4_block );
    memset( ctx, 0, sizeof( *ctx ) );
}

void md4( const unsigned char *data, unsigned int len, unsigned char digest[16] )
{
    MD4_CTX ctx;
    md4_init( &ctx );
    md4_update( &ctx, data, len );
    md4_final( &ctx, digest );
}

/* ==========================================================================
 * MD5
 * ========================================================================== */
#define MD5_F( x, y, z )  ( ( (x) & (y) ) | ( ~(x) & (z) ) )
#define MD5_G( x, y, z )  ( ( (x) & (z) ) | ( (y) & ~(z) ) )
#define MD5_H( x, y, z )  ( (x) ^ (y) ^ (z) )
#define MD5_I( x, y, z )  ( (y) ^ ( (x) | ~(z) ) )
#define MD5_FF( a, b, c, d, x, s, ac )  { (a) += MD5_F( (b), (c), (d) ) + (x) + (unsigned long)(ac); (a) = ROTL( (a), (s) ); (a) += (b); }
#define MD5_GG( a, b, c, d, x, s, ac )  { (a) += MD5_G( (b), (c), (d) ) + (x) + (unsigned long)(ac); (a) = ROTL( (a), (s) ); (a) += (b); }
#define MD5_HH( a, b, c, d, x, s, ac )  { (a) += MD5_H( (b), (c), (d) ) + (x) + (unsigned long)(ac); (a) = ROTL( (a), (s) ); (a) += (b); }
#define MD5_II( a, b, c, d, x, s, ac )  { (a) += MD5_I( (b), (c), (d) ) + (x) + (unsigned long)(ac); (a) = ROTL( (a), (s) ); (a) += (b); }

static void md5_block( unsigned long state[4], const unsigned char block[64] )
{
    unsigned long aa = state[0], bb = state[1], cc = state[2], dd = state[3];
    unsigned long x[16];

    decode_le( x, block, 16 );

    MD5_FF( aa, bb, cc, dd, x[0],  7, 0xD76AA478 )  MD5_FF( dd, aa, bb, cc, x[1], 12, 0xE8C7B756 )
    MD5_FF( cc, dd, aa, bb, x[2], 17, 0x242070DB )  MD5_FF( bb, cc, dd, aa, x[3], 22, 0xC1BDCEEE )
    MD5_FF( aa, bb, cc, dd, x[4],  7, 0xF57C0FAF )  MD5_FF( dd, aa, bb, cc, x[5], 12, 0x4787C62A )
    MD5_FF( cc, dd, aa, bb, x[6], 17, 0xA8304613 )  MD5_FF( bb, cc, dd, aa, x[7], 22, 0xFD469501 )
    MD5_FF( aa, bb, cc, dd, x[8],  7, 0x698098D8 )  MD5_FF( dd, aa, bb, cc, x[9], 12, 0x8B44F7AF )
    MD5_FF( cc, dd, aa, bb, x[10],17, 0xFFFF5BB1 )  MD5_FF( bb, cc, dd, aa, x[11],22, 0x895CD7BE )
    MD5_FF( aa, bb, cc, dd, x[12], 7, 0x6B901122 )  MD5_FF( dd, aa, bb, cc, x[13],12, 0xFD987193 )
    MD5_FF( cc, dd, aa, bb, x[14],17, 0xA679438E )  MD5_FF( bb, cc, dd, aa, x[15],22, 0x49B40821 )

    MD5_GG( aa, bb, cc, dd, x[1],  5, 0xF61E2562 )  MD5_GG( dd, aa, bb, cc, x[6],  9, 0xC040B340 )
    MD5_GG( cc, dd, aa, bb, x[11],14, 0x265E5A51 )  MD5_GG( bb, cc, dd, aa, x[0], 20, 0xE9B6C7AA )
    MD5_GG( aa, bb, cc, dd, x[5],  5, 0xD62F105D )  MD5_GG( dd, aa, bb, cc, x[10], 9, 0x02441453 )
    MD5_GG( cc, dd, aa, bb, x[15],14, 0xD8A1E681 )  MD5_GG( bb, cc, dd, aa, x[4], 20, 0xE7D3FBC8 )
    MD5_GG( aa, bb, cc, dd, x[9],  5, 0x21E1CDE6 )  MD5_GG( dd, aa, bb, cc, x[14], 9, 0xC33707D6 )
    MD5_GG( cc, dd, aa, bb, x[3], 14, 0xF4D50D87 )  MD5_GG( bb, cc, dd, aa, x[8], 20, 0x455A14ED )
    MD5_GG( aa, bb, cc, dd, x[13], 5, 0xA9E3E905 )  MD5_GG( dd, aa, bb, cc, x[2],  9, 0xFCEFA3F8 )
    MD5_GG( cc, dd, aa, bb, x[7], 14, 0x676F02D9 )  MD5_GG( bb, cc, dd, aa, x[12],20, 0x8D2A4C8A )

    MD5_HH( aa, bb, cc, dd, x[5],  4, 0xFFFA3942 )  MD5_HH( dd, aa, bb, cc, x[8], 11, 0x8771F681 )
    MD5_HH( cc, dd, aa, bb, x[11],16, 0x6D9D6122 )  MD5_HH( bb, cc, dd, aa, x[14],23, 0xFDE5380C )
    MD5_HH( aa, bb, cc, dd, x[1],  4, 0xA4BEEA44 )  MD5_HH( dd, aa, bb, cc, x[4], 11, 0x4BDECFA9 )
    MD5_HH( cc, dd, aa, bb, x[7], 16, 0xF6BB4B60 )  MD5_HH( bb, cc, dd, aa, x[10],23, 0xBEBFBC70 )
    MD5_HH( aa, bb, cc, dd, x[13], 4, 0x289B7EC6 )  MD5_HH( dd, aa, bb, cc, x[0], 11, 0xEAA127FA )
    MD5_HH( cc, dd, aa, bb, x[3], 16, 0xD4EF3085 )  MD5_HH( bb, cc, dd, aa, x[6], 23, 0x04881D05 )
    MD5_HH( aa, bb, cc, dd, x[9],  4, 0xD9D4D039 )  MD5_HH( dd, aa, bb, cc, x[12],11, 0xE6DB99E5 )
    MD5_HH( cc, dd, aa, bb, x[15],16, 0x1FA27CF8 )  MD5_HH( bb, cc, dd, aa, x[2], 23, 0xC4AC5665 )

    MD5_II( aa, bb, cc, dd, x[0],  6, 0xF4292244 )  MD5_II( dd, aa, bb, cc, x[7], 10, 0x432AFF97 )
    MD5_II( cc, dd, aa, bb, x[14],15, 0xAB9423A7 )  MD5_II( bb, cc, dd, aa, x[5], 21, 0xFC93A039 )
    MD5_II( aa, bb, cc, dd, x[12], 6, 0x655B59C3 )  MD5_II( dd, aa, bb, cc, x[3], 10, 0x8F0CCC92 )
    MD5_II( cc, dd, aa, bb, x[10],15, 0xFFEFF47D )  MD5_II( bb, cc, dd, aa, x[1], 21, 0x85845DD1 )
    MD5_II( aa, bb, cc, dd, x[8],  6, 0x6FA87E4F )  MD5_II( dd, aa, bb, cc, x[15],10, 0xFE2CE6E0 )
    MD5_II( cc, dd, aa, bb, x[6], 15, 0xA3014314 )  MD5_II( bb, cc, dd, aa, x[13],21, 0x4E0811A1 )
    MD5_II( aa, bb, cc, dd, x[4],  6, 0xF7537E82 )  MD5_II( dd, aa, bb, cc, x[11],10, 0xBD3AF235 )
    MD5_II( cc, dd, aa, bb, x[2], 15, 0x2AD7D2BB )  MD5_II( bb, cc, dd, aa, x[9], 21, 0xEB86D391 )

    state[0] += aa; state[1] += bb; state[2] += cc; state[3] += dd;
}

void md5_init( MD5_CTX *ctx )
{
    ctx->state[0] = 0x67452301UL; ctx->state[1] = 0xEFCDAB89UL;
    ctx->state[2] = 0x98BADCFEUL; ctx->state[3] = 0x10325476UL;
    ctx->count[0] = ctx->count[1] = 0;
}

void md5_update( MD5_CTX *ctx, const unsigned char *data, unsigned int len )
{
    md_absorb( ctx->count, ctx->buffer, ctx->state, data, len, md5_block );
}

void md5_final( MD5_CTX *ctx, unsigned char digest[16] )
{
    md_pad_final( ctx->count, ctx->buffer, ctx->state, digest, md5_block );
    memset( ctx, 0, sizeof( *ctx ) );
}

void md5( const unsigned char *data, unsigned int len, unsigned char digest[16] )
{
    MD5_CTX ctx;
    md5_init( &ctx );
    md5_update( &ctx, data, len );
    md5_final( &ctx, digest );
}

/* ==========================================================================
 * HMAC-MD5 (RFC 2104)
 * ========================================================================== */
void hmac_md5( const unsigned char *key, int keylen,
               const unsigned char *data, int datalen,
               unsigned char out[16] )
{
    unsigned char kpad[64];
    unsigned char tk[16];
    unsigned char inner[16];
    MD5_CTX       ctx;
    int           i;

    if ( keylen > 64 ) {
        md5( key, (unsigned int)keylen, tk );
        key = tk;
        keylen = 16;
    }

    memset( kpad, 0x36, sizeof( kpad ) );
    for ( i = 0; i < keylen; i++ ) kpad[i] = (unsigned char)( kpad[i] ^ key[i] );
    md5_init( &ctx );
    md5_update( &ctx, kpad, 64 );
    md5_update( &ctx, data, (unsigned int)datalen );
    md5_final( &ctx, inner );

    memset( kpad, 0x5C, sizeof( kpad ) );
    for ( i = 0; i < keylen; i++ ) kpad[i] = (unsigned char)( kpad[i] ^ key[i] );
    md5_init( &ctx );
    md5_update( &ctx, kpad, 64 );
    md5_update( &ctx, inner, 16 );
    md5_final( &ctx, out );
}

/* ==========================================================================
 * RC4
 * ========================================================================== */
void rc4_init( RC4_CTX *ctx, const unsigned char *key, int keylen )
{
    int i, j;
    unsigned char tmp;

    for ( i = 0; i < 256; i++ ) ctx->s[i] = (unsigned char)i;
    j = 0;
    for ( i = 0; i < 256; i++ ) {
        j = ( j + ctx->s[i] + key[i % keylen] ) & 0xFF;
        tmp = ctx->s[i]; ctx->s[i] = ctx->s[j]; ctx->s[j] = tmp;
    }
    ctx->i = ctx->j = 0;
}

void rc4_crypt( RC4_CTX *ctx, const unsigned char *in, unsigned char *out, int len )
{
    int k;
    unsigned char tmp;

    for ( k = 0; k < len; k++ ) {
        ctx->i = ( ctx->i + 1 ) & 0xFF;
        ctx->j = ( ctx->j + ctx->s[ctx->i] ) & 0xFF;
        tmp = ctx->s[ctx->i]; ctx->s[ctx->i] = ctx->s[ctx->j]; ctx->s[ctx->j] = tmp;
        out[k] = (unsigned char)( in[k] ^ ctx->s[( ctx->s[ctx->i] + ctx->s[ctx->j] ) & 0xFF] );
    }
}

/* ==========================================================================
 * NTLMv2 helpers
 * ========================================================================== */
int utf16le( const char *ascii, unsigned char *out, int outcap )
{
    int i = 0;
    while ( ascii[i] && ( i * 2 + 1 ) < outcap ) {
        out[i * 2]     = (unsigned char)ascii[i];
        out[i * 2 + 1] = 0;
        i++;
    }
    return i * 2;
}

void nt_hash( const char *password, unsigned char out[16] )
{
    unsigned char wide[512];
    int           widelen = utf16le( password, wide, sizeof( wide ) );
    md4( wide, (unsigned int)widelen, out );
}

void ntowf_v2( const char *password, const char *user, const char *domain,
               unsigned char out[16] )
{
    unsigned char nt[16];
    unsigned char wide[1024];
    int           pos = 0;
    int           i;

    nt_hash( password, nt );

    /* UTF-16LE of uppercase(user) followed by domain, unchanged */
    for ( i = 0; user[i] && pos + 1 < (int)sizeof( wide ); i++ ) {
        char upper = user[i];
        if ( upper >= 'a' && upper <= 'z' ) upper = (char)( upper - 32 );
        wide[pos++] = (unsigned char)upper;
        wide[pos++] = 0;
    }
    for ( i = 0; domain[i] && pos + 1 < (int)sizeof( wide ); i++ ) {
        wide[pos++] = (unsigned char)domain[i];
        wide[pos++] = 0;
    }

    hmac_md5( nt, 16, wide, pos, out );
}

/* ==========================================================================
 * DES (FIPS 46-3) and the LM / NTLMv1 responses
 * ==========================================================================
 * A plain, table-driven DES-ECB on bit arrays - correctness over speed, as
 * only a handful of blocks are ever enciphered.  A bit array is 0/1 ints,
 * most significant bit first, the way the FIPS tables are numbered.
 * ========================================================================== */

static const int DES_IP[64] = {
    58,50,42,34,26,18,10,2, 60,52,44,36,28,20,12,4,
    62,54,46,38,30,22,14,6, 64,56,48,40,32,24,16,8,
    57,49,41,33,25,17,9,1,  59,51,43,35,27,19,11,3,
    61,53,45,37,29,21,13,5, 63,55,47,39,31,23,15,7 };

static const int DES_FP[64] = {
    40,8,48,16,56,24,64,32, 39,7,47,15,55,23,63,31,
    38,6,46,14,54,22,62,30, 37,5,45,13,53,21,61,29,
    36,4,44,12,52,20,60,28, 35,3,43,11,51,19,59,27,
    34,2,42,10,50,18,58,26, 33,1,41,9,49,17,57,25 };

static const int DES_E[48] = {
    32,1,2,3,4,5,   4,5,6,7,8,9,     8,9,10,11,12,13,
    12,13,14,15,16,17, 16,17,18,19,20,21,
    20,21,22,23,24,25, 24,25,26,27,28,29, 28,29,30,31,32,1 };

static const int DES_P[32] = {
    16,7,20,21,29,12,28,17, 1,15,23,26,5,18,31,10,
    2,8,24,14,32,27,3,9,    19,13,30,6,22,11,4,25 };

static const int DES_PC1[56] = {
    57,49,41,33,25,17,9,  1,58,50,42,34,26,18,
    10,2,59,51,43,35,27,  19,11,3,60,52,44,36,
    63,55,47,39,31,23,15, 7,62,54,46,38,30,22,
    14,6,61,53,45,37,29,  21,13,5,28,20,12,4 };

static const int DES_PC2[48] = {
    14,17,11,24,1,5,    3,28,15,6,21,10,
    23,19,12,4,26,8,    16,7,27,20,13,2,
    41,52,31,37,47,55,  30,40,51,45,33,48,
    44,49,39,56,34,53,  46,42,50,36,29,32 };

static const int DES_SHIFT[16] = { 1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1 };

static const int DES_SBOX[8][64] = {
    { 14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,
      0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
      4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,
      15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13 },
    { 15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,
      3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
      0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,
      13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9 },
    { 10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,
      13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
      13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,
      1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12 },
    { 7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,
      13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
      10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,
      3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14 },
    { 2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,
      14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
      4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,
      11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3 },
    { 12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,
      10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
      9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,
      4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13 },
    { 4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,
      13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
      1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,
      6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12 },
    { 13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,
      1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
      7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,
      2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11 } };

static void bytes_to_bits( const unsigned char *in, int nbytes, int *bits )
{
    int i, b;
    for ( i = 0; i < nbytes; i++ )
        for ( b = 0; b < 8; b++ )
            bits[i * 8 + b] = ( in[i] >> ( 7 - b ) ) & 1;
}

static void bits_to_bytes( const int *bits, int nbytes, unsigned char *out )
{
    int i, b;
    for ( i = 0; i < nbytes; i++ ) {
        unsigned char v = 0;
        for ( b = 0; b < 8; b++ ) v = (unsigned char)( ( v << 1 ) | bits[i * 8 + b] );
        out[i] = v;
    }
}

static void des_permute( const int *in, int *out, const int *table, int n )
{
    int i;
    for ( i = 0; i < n; i++ ) out[i] = in[table[i] - 1];
}

/* expand 7 packed bytes into an 8-byte DES key (the LM/NTLM str_to_key) */
static void str_to_key( const unsigned char *str, unsigned char *key )
{
    int i;
    key[0] = (unsigned char)( str[0] >> 1 );
    key[1] = (unsigned char)( ( ( str[0] & 0x01 ) << 6 ) | ( str[1] >> 2 ) );
    key[2] = (unsigned char)( ( ( str[1] & 0x03 ) << 5 ) | ( str[2] >> 3 ) );
    key[3] = (unsigned char)( ( ( str[2] & 0x07 ) << 4 ) | ( str[3] >> 4 ) );
    key[4] = (unsigned char)( ( ( str[3] & 0x0F ) << 3 ) | ( str[4] >> 5 ) );
    key[5] = (unsigned char)( ( ( str[4] & 0x1F ) << 2 ) | ( str[5] >> 6 ) );
    key[6] = (unsigned char)( ( ( str[5] & 0x3F ) << 1 ) | ( str[6] >> 7 ) );
    key[7] = (unsigned char)( str[6] & 0x7F );
    for ( i = 0; i < 8; i++ ) key[i] = (unsigned char)( key[i] << 1 );   /* parity bit, ignored */
}

static void des_key_schedule( const unsigned char key[8], int subkeys[16][48] )
{
    int keybits[64], pc1[56], c[28], d[28], round, i;

    bytes_to_bits( key, 8, keybits );
    des_permute( keybits, pc1, DES_PC1, 56 );
    for ( i = 0; i < 28; i++ ) { c[i] = pc1[i]; d[i] = pc1[i + 28]; }

    for ( round = 0; round < 16; round++ ) {
        int cd[56], s, t;
        for ( s = 0; s < DES_SHIFT[round]; s++ ) {
            t = c[0]; for ( i = 0; i < 27; i++ ) c[i] = c[i + 1]; c[27] = t;
            t = d[0]; for ( i = 0; i < 27; i++ ) d[i] = d[i + 1]; d[27] = t;
        }
        for ( i = 0; i < 28; i++ ) { cd[i] = c[i]; cd[i + 28] = d[i]; }
        des_permute( cd, subkeys[round], DES_PC2, 48 );
    }
}

static void des_crypt( const int subkeys[16][48], const unsigned char in[8],
                       unsigned char out[8] )
{
    int block[64], perm[64], l[32], r[32], round, i;

    bytes_to_bits( in, 8, block );
    des_permute( block, perm, DES_IP, 64 );
    for ( i = 0; i < 32; i++ ) { l[i] = perm[i]; r[i] = perm[i + 32]; }

    for ( round = 0; round < 16; round++ ) {
        int er[48], sout[32], pout[32], nl[32], box, bit;
        des_permute( r, er, DES_E, 48 );
        for ( i = 0; i < 48; i++ ) er[i] ^= subkeys[round][i];
        for ( box = 0; box < 8; box++ ) {
            int *g = er + box * 6;
            int row = ( g[0] << 1 ) | g[5];
            int col = ( g[1] << 3 ) | ( g[2] << 2 ) | ( g[3] << 1 ) | g[4];
            int val = DES_SBOX[box][row * 16 + col];
            for ( bit = 0; bit < 4; bit++ )
                sout[box * 4 + bit] = ( val >> ( 3 - bit ) ) & 1;
        }
        des_permute( sout, pout, DES_P, 32 );
        for ( i = 0; i < 32; i++ ) nl[i] = l[i] ^ pout[i];
        for ( i = 0; i < 32; i++ ) { l[i] = r[i]; r[i] = nl[i]; }
    }

    /* the final block is R16 L16 (the halves are swapped back) */
    for ( i = 0; i < 32; i++ ) { block[i] = r[i]; block[i + 32] = l[i]; }
    des_permute( block, perm, DES_FP, 64 );
    bits_to_bytes( perm, 8, out );
}

void des_encrypt_block( const unsigned char key[8], const unsigned char in[8],
                        unsigned char out[8] )
{
    int subkeys[16][48];
    des_key_schedule( key, subkeys );
    des_crypt( subkeys, in, out );
}

void lm_hash( const char *password, unsigned char out[16] )
{
    static const unsigned char kgs[8] = { 0x4B,0x47,0x53,0x21,0x40,0x23,0x24,0x25 };
    unsigned char up[14], key[8];
    int i;

    memset( up, 0, sizeof( up ) );
    for ( i = 0; i < 14 && password[i]; i++ ) {
        char ch = password[i];
        if ( ch >= 'a' && ch <= 'z' ) ch = (char)( ch - 32 );
        up[i] = (unsigned char)ch;
    }
    str_to_key( up,     key ); des_encrypt_block( key, kgs, out );
    str_to_key( up + 7, key ); des_encrypt_block( key, kgs, out + 8 );
}

void desl( const unsigned char hash[16], const unsigned char challenge[8],
           unsigned char out[24] )
{
    unsigned char k21[21], key[8];

    memset( k21, 0, sizeof( k21 ) );
    memcpy( k21, hash, 16 );
    str_to_key( k21,      key ); des_encrypt_block( key, challenge, out );
    str_to_key( k21 + 7,  key ); des_encrypt_block( key, challenge, out + 8 );
    str_to_key( k21 + 14, key ); des_encrypt_block( key, challenge, out + 16 );
}
