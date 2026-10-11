/*
 * CRYPT.C - encrypted files: the standard security handler, every
 * revision of it.
 *
 *   R2      PDF 1.1   RC4, a 40-bit key
 *   R3      PDF 1.4   RC4, up to 128 bits
 *   R4      PDF 1.5   crypt filters: RC4 or AES-128 (CBC), and streams
 *                     and strings may differ
 *   R5      (Adobe's extension to 1.7)  AES-256, the key behind SHA-256
 *   R6      PDF 2.0   AES-256, the key behind a hash that mixes SHA-256,
 *                     SHA-384 and SHA-512
 *
 * Most encrypted files have an empty user password - they are
 * encrypted to carry permissions, not to be kept from being read - so
 * that is tried first and nobody is asked.  The permissions themselves
 * are not this program's business: it prints nothing and copies
 * nothing.
 *
 * The ciphers and hashes are the plain textbook forms.  They run once
 * a file or once a stream; none of them is where the time goes.
 */
#include "pdf.h"

static const u8 pad_string[32] = {
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
    0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A };

#define ROL( val, bits )    (((val) << (bits)) | ((val) >> (32 - (bits))))
#define ROR( val, bits )    (((val) >> (bits)) | ((val) << (32 - (bits))))

/* ------------------------------------------------------------------ */
/* MD5                                                                 */
/* ------------------------------------------------------------------ */

static void md5_block( u32 *state, const u8 *data )
{
    static const u8 shifts[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
    static const u32 sines[64] = {
        0xd76aa478UL, 0xe8c7b756UL, 0x242070dbUL, 0xc1bdceeeUL, 0xf57c0fafUL, 0x4787c62aUL,
        0xa8304613UL, 0xfd469501UL, 0x698098d8UL, 0x8b44f7afUL, 0xffff5bb1UL, 0x895cd7beUL,
        0x6b901122UL, 0xfd987193UL, 0xa679438eUL, 0x49b40821UL, 0xf61e2562UL, 0xc040b340UL,
        0x265e5a51UL, 0xe9b6c7aaUL, 0xd62f105dUL, 0x02441453UL, 0xd8a1e681UL, 0xe7d3fbc8UL,
        0x21e1cde6UL, 0xc33707d6UL, 0xf4d50d87UL, 0x455a14edUL, 0xa9e3e905UL, 0xfcefa3f8UL,
        0x676f02d9UL, 0x8d2a4c8aUL, 0xfffa3942UL, 0x8771f681UL, 0x6d9d6122UL, 0xfde5380cUL,
        0xa4beea44UL, 0x4bdecfa9UL, 0xf6bb4b60UL, 0xbebfbc70UL, 0x289b7ec6UL, 0xeaa127faUL,
        0xd4ef3085UL, 0x04881d05UL, 0xd9d4d039UL, 0xe6db99e5UL, 0x1fa27cf8UL, 0xc4ac5665UL,
        0xf4292244UL, 0x432aff97UL, 0xab9423a7UL, 0xfc93a039UL, 0x655b59c3UL, 0x8f0ccc92UL,
        0xffeff47dUL, 0x85845dd1UL, 0x6fa87e4fUL, 0xfe2ce6e0UL, 0xa3014314UL, 0x4e0811a1UL,
        0xf7537e82UL, 0xbd3af235UL, 0x2ad7d2bbUL, 0xeb86d391UL };
    u32 words[16], va = state[0], vb = state[1], vc = state[2], vd = state[3], func, temp;
    int index, pick;

    for ( index = 0; index < 16; index++ ) {
        words[index] = (u32)data[index * 4] | ((u32)data[index * 4 + 1] << 8) |
                       ((u32)data[index * 4 + 2] << 16) | ((u32)data[index * 4 + 3] << 24);
    }
    for ( index = 0; index < 64; index++ ) {
        if ( index < 16 ) {
            func = (vb & vc) | (~vb & vd);
            pick = index;
        } else if ( index < 32 ) {
            func = (vd & vb) | (~vd & vc);
            pick = (5 * index + 1) & 15;
        } else if ( index < 48 ) {
            func = vb ^ vc ^ vd;
            pick = (3 * index + 5) & 15;
        } else {
            func = vc ^ (vb | ~vd);
            pick = (7 * index) & 15;
        }
        temp = vd;
        vd = vc;
        vc = vb;
        func += va + sines[index] + words[pick];
        vb += ROL( func, shifts[(index >> 4) * 4 + (index & 3)] );
        va = temp;
    }
    state[0] += va;
    state[1] += vb;
    state[2] += vc;
    state[3] += vd;
}

void md5_init( MD5CTX *ctx )
{
    ctx->state[0] = 0x67452301UL;
    ctx->state[1] = 0xefcdab89UL;
    ctx->state[2] = 0x98badcfeUL;
    ctx->state[3] = 0x10325476UL;
    ctx->count = 0;
}

void md5_update( MD5CTX *ctx, const u8 *data, u32 len )
{
    u32 have = ctx->count & 63, take;

    ctx->count += len;
    while ( len ) {
        take = 64 - have < len ? 64 - have : len;
        mem_cpy( ctx->buf + have, data, take );
        have += take;
        data += take;
        len -= take;
        if ( have == 64 ) {
            md5_block( ctx->state, ctx->buf );
            have = 0;
        }
    }
}

void md5_final( MD5CTX *ctx, u8 *digest )
{
    static const u8 pad[64] = { 0x80 };
    u8 bits[8];
    u32 count = ctx->count;
    int index;

    for ( index = 0; index < 8; index++ ) {
        bits[index] = index < 4 ? (u8)((count << 3) >> (index * 8))
                                : (index == 4 ? (u8)(count >> 29) : 0);
    }
    md5_update( ctx, pad, ((119 - (count & 63)) & 63) + 1 );
    md5_update( ctx, bits, 8 );
    for ( index = 0; index < 16; index++ ) {
        digest[index] = (u8)(ctx->state[index >> 2] >> ((index & 3) * 8));
    }
}

void md5( const u8 *data, u32 len, u8 *digest )
{
    MD5CTX ctx;

    md5_init( &ctx );
    md5_update( &ctx, data, len );
    md5_final( &ctx, digest );
}

/* ------------------------------------------------------------------ */
/* RC4                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    u8  perm[256];
    int ia, ib;
} RC4;

static void rc4_init( RC4 *rc4, const u8 *key, u32 keylen )
{
    int index, mix = 0;
    u8 swap;

    for ( index = 0; index < 256; index++ ) {
        rc4->perm[index] = (u8)index;
    }
    for ( index = 0; index < 256; index++ ) {
        mix = (mix + rc4->perm[index] + key[(u32)index % keylen]) & 255;
        swap = rc4->perm[index];
        rc4->perm[index] = rc4->perm[mix];
        rc4->perm[mix] = swap;
    }
    rc4->ia = rc4->ib = 0;
}

static void rc4_run( RC4 *rc4, u8 *data, u32 len )
{
    int ia = rc4->ia, ib = rc4->ib;
    u8 swap;

    while ( len-- ) {
        ia = (ia + 1) & 255;
        ib = (ib + rc4->perm[ia]) & 255;
        swap = rc4->perm[ia];
        rc4->perm[ia] = rc4->perm[ib];
        rc4->perm[ib] = swap;
        *data++ ^= rc4->perm[(rc4->perm[ia] + rc4->perm[ib]) & 255];
    }
    rc4->ia = ia;
    rc4->ib = ib;
}

typedef struct {
    RC4 rc4;
    u8  buf[1024];
} RC4IN;

static int rc4_fill( IN *in )
{
    RC4IN *st = IN_STATE( in, RC4IN );
    u32 got = in_read( in->src, st->buf, sizeof( st->buf ) );

    rc4_run( &st->rc4, st->buf, got );
    in->cur = st->buf;
    in->end = st->buf + got;
    return got != 0;
}

IN *flt_rc4( IN *src, const u8 *key, u32 keylen )
{
    IN *in = in_new( sizeof( RC4IN ), rc4_fill, src );

    rc4_init( &IN_STATE( in, RC4IN )->rc4, key, keylen );
    return in;
}

/* ------------------------------------------------------------------ */
/* AES                                                                 */
/* ------------------------------------------------------------------ */

static u8  sbox[256], rsbox[256];
static int sbox_made;

typedef struct {
    u8  round_key[240];
    int rounds;
} AES;

#define XTIME( val )    ((u8)(((val) << 1) ^ (((val) & 0x80) ? 0x1B : 0)))

/* the S-box is the inverse in GF(2^8) put through an affine map; made
   here by walking the field with a generator, 3, and its inverse */
static void sbox_make( void )
{
    u8 gen = 1, inv = 1, val;

    do {
        gen = (u8)(gen ^ XTIME( gen ));
        inv ^= (u8)(inv << 1);
        inv ^= (u8)(inv << 2);
        inv ^= (u8)(inv << 4);
        if ( inv & 0x80 ) {
            inv ^= 0x09;
        }
        val = (u8)(inv ^ ((inv << 1) | (inv >> 7)) ^ ((inv << 2) | (inv >> 6)) ^
                   ((inv << 3) | (inv >> 5)) ^ ((inv << 4) | (inv >> 4)));
        sbox[gen] = (u8)(val ^ 0x63);
    } while ( gen != 1 );
    sbox[0] = 0x63;
    for ( gen = 0; ; gen++ ) {
        rsbox[sbox[gen]] = gen;
        if ( gen == 255 ) {
            break;
        }
    }
    sbox_made = 1;
}

static void aes_key( AES *aes, const u8 *key, u32 keylen )
{
    u32 words = keylen / 4, total, at;
    u8 temp[4], swap, rcon = 1;
    int index;

    if ( !sbox_made ) {
        sbox_make();
    }
    aes->rounds = (int)words + 6;
    total = 4 * ((u32)aes->rounds + 1);
    mem_cpy( aes->round_key, key, keylen );
    for ( at = words; at < total; at++ ) {
        mem_cpy( temp, aes->round_key + (at - 1) * 4, 4 );
        if ( at % words == 0 ) {
            swap = temp[0];
            temp[0] = (u8)(sbox[temp[1]] ^ rcon);
            temp[1] = sbox[temp[2]];
            temp[2] = sbox[temp[3]];
            temp[3] = sbox[swap];
            rcon = XTIME( rcon );
        } else if ( words > 6 && at % words == 4 ) {
            for ( index = 0; index < 4; index++ ) {
                temp[index] = sbox[temp[index]];
            }
        }
        for ( index = 0; index < 4; index++ ) {
            aes->round_key[at * 4 + index] = (u8)(aes->round_key[(at - words) * 4 + index] ^ temp[index]);
        }
    }
}

static void add_round_key( const AES *aes, u8 *block, int round )
{
    const u8 *key = aes->round_key + round * 16;
    int index;

    for ( index = 0; index < 16; index++ ) {
        block[index] ^= key[index];
    }
}

static u8 gmul( u8 val, int by )
{
    u8 out = 0;

    while ( by ) {
        if ( by & 1 ) {
            out ^= val;
        }
        val = XTIME( val );
        by >>= 1;
    }
    return out;
}

static void aes_decrypt( const AES *aes, u8 *block )
{
    u8 tmp[16];
    int round, col, index;

    add_round_key( aes, block, aes->rounds );
    for ( round = aes->rounds - 1; ; round-- ) {
        /* the rows shifted back, and the bytes substituted back */
        for ( index = 0; index < 16; index++ ) {
            tmp[(index + (index & 3) * 4) & 15] = rsbox[block[index]];
        }
        mem_cpy( block, tmp, 16 );
        add_round_key( aes, block, round );
        if ( round == 0 ) {
            break;
        }
        for ( col = 0; col < 16; col += 4 ) {
            tmp[0] = block[col];
            tmp[1] = block[col + 1];
            tmp[2] = block[col + 2];
            tmp[3] = block[col + 3];
            block[col]     = (u8)(gmul( tmp[0], 14 ) ^ gmul( tmp[1], 11 ) ^ gmul( tmp[2], 13 ) ^ gmul( tmp[3], 9 ));
            block[col + 1] = (u8)(gmul( tmp[0], 9 ) ^ gmul( tmp[1], 14 ) ^ gmul( tmp[2], 11 ) ^ gmul( tmp[3], 13 ));
            block[col + 2] = (u8)(gmul( tmp[0], 13 ) ^ gmul( tmp[1], 9 ) ^ gmul( tmp[2], 14 ) ^ gmul( tmp[3], 11 ));
            block[col + 3] = (u8)(gmul( tmp[0], 11 ) ^ gmul( tmp[1], 13 ) ^ gmul( tmp[2], 9 ) ^ gmul( tmp[3], 14 ));
        }
    }
}

static void aes_encrypt( const AES *aes, u8 *block )
{
    u8 tmp[16], all, first;
    int round, col, index;

    add_round_key( aes, block, 0 );
    for ( round = 1; ; round++ ) {
        for ( index = 0; index < 16; index++ ) {
            tmp[index] = sbox[block[(index + (index & 3) * 4) & 15]];
        }
        mem_cpy( block, tmp, 16 );
        if ( round < aes->rounds ) {
            for ( col = 0; col < 16; col += 4 ) {
                first = block[col];
                all = (u8)(block[col] ^ block[col + 1] ^ block[col + 2] ^ block[col + 3]);
                block[col]     ^= (u8)(XTIME( (u8)(block[col] ^ block[col + 1]) ) ^ all);
                block[col + 1] ^= (u8)(XTIME( (u8)(block[col + 1] ^ block[col + 2]) ) ^ all);
                block[col + 2] ^= (u8)(XTIME( (u8)(block[col + 2] ^ block[col + 3]) ) ^ all);
                block[col + 3] ^= (u8)(XTIME( (u8)(block[col + 3] ^ first) ) ^ all);
            }
        }
        add_round_key( aes, block, round );
        if ( round == aes->rounds ) {
            break;
        }
    }
}

/* CBC, in place; "iv" is left ready for the block after */
static void aes_cbc_decrypt( const AES *aes, u8 *iv, u8 *data, u32 len )
{
    u8 keep[16];
    int index;

    for ( ; len >= 16; len -= 16, data += 16 ) {
        mem_cpy( keep, data, 16 );
        aes_decrypt( aes, data );
        for ( index = 0; index < 16; index++ ) {
            data[index] ^= iv[index];
        }
        mem_cpy( iv, keep, 16 );
    }
}

static void aes_cbc_encrypt( const AES *aes, u8 *iv, u8 *data, u32 len )
{
    int index;

    for ( ; len >= 16; len -= 16, data += 16 ) {
        for ( index = 0; index < 16; index++ ) {
            data[index] ^= iv[index];
        }
        aes_encrypt( aes, data );
        mem_cpy( iv, data, 16 );
    }
}

/* the padding off the end: 1 to 16 bytes, each saying how many */
static u32 aes_unpad( const u8 *data, u32 len )
{
    u32 pad;

    if ( len == 0 ) {
        return 0;
    }
    pad = data[len - 1];
    return pad >= 1 && pad <= 16 && pad <= len ? len - pad : len;
}

typedef struct {
    AES aes;
    u8  iv[16];
    int started;
    u8  buf[1024];
} AESIN;

static int aes_fill( IN *in )
{
    AESIN *st = IN_STATE( in, AESIN );
    IN *src = in->src;
    u32 got;

    if ( !st->started ) {
        st->started = 1;
        if ( in_read( src, st->iv, 16 ) != 16 ) {
            return 0;
        }
    }
    got = in_read( src, st->buf, sizeof( st->buf ) ) & ~15UL;
    aes_cbc_decrypt( &st->aes, st->iv, st->buf, got );
    if ( in_peek( src ) < 0 ) {
        got = aes_unpad( st->buf, got );
    }
    in->cur = st->buf;
    in->end = st->buf + got;
    return got != 0;
}

IN *flt_aes( IN *src, const u8 *key, u32 keylen )
{
    IN *in = in_new( sizeof( AESIN ), aes_fill, src );

    aes_key( &IN_STATE( in, AESIN )->aes, key, keylen );
    return in;
}

/* ------------------------------------------------------------------ */
/* SHA-256                                                             */
/* ------------------------------------------------------------------ */

static const u32 sha_k[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL,
    0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL,
    0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL, 0xa2bfe8a1UL, 0xa81a664bUL,
    0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL,
    0x5b9cca4fUL, 0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL };

static void sha256_block( u32 *state, const u8 *data )
{
    u32 words[64], reg[8], sum1, sum2;
    int index;

    for ( index = 0; index < 16; index++ ) {
        words[index] = ((u32)data[index * 4] << 24) | ((u32)data[index * 4 + 1] << 16) |
                       ((u32)data[index * 4 + 2] << 8) | (u32)data[index * 4 + 3];
    }
    for ( ; index < 64; index++ ) {
        sum1 = ROR( words[index - 15], 7 ) ^ ROR( words[index - 15], 18 ) ^ (words[index - 15] >> 3);
        sum2 = ROR( words[index - 2], 17 ) ^ ROR( words[index - 2], 19 ) ^ (words[index - 2] >> 10);
        words[index] = words[index - 16] + sum1 + words[index - 7] + sum2;
    }
    mem_cpy( reg, state, sizeof( reg ) );
    for ( index = 0; index < 64; index++ ) {
        sum1 = reg[7] + (ROR( reg[4], 6 ) ^ ROR( reg[4], 11 ) ^ ROR( reg[4], 25 )) +
               ((reg[4] & reg[5]) ^ (~reg[4] & reg[6])) + sha_k[index] + words[index];
        sum2 = (ROR( reg[0], 2 ) ^ ROR( reg[0], 13 ) ^ ROR( reg[0], 22 )) +
               ((reg[0] & reg[1]) ^ (reg[0] & reg[2]) ^ (reg[1] & reg[2]));
        reg[7] = reg[6];
        reg[6] = reg[5];
        reg[5] = reg[4];
        reg[4] = reg[3] + sum1;
        reg[3] = reg[2];
        reg[2] = reg[1];
        reg[1] = reg[0];
        reg[0] = sum1 + sum2;
    }
    for ( index = 0; index < 8; index++ ) {
        state[index] += reg[index];
    }
}

void sha256( const u8 *data, u32 len, u8 *digest )
{
    u32 state[8], at;
    u8 last[128];
    u32 rest, total;
    int index;

    state[0] = 0x6a09e667UL;
    state[1] = 0xbb67ae85UL;
    state[2] = 0x3c6ef372UL;
    state[3] = 0xa54ff53aUL;
    state[4] = 0x510e527fUL;
    state[5] = 0x9b05688cUL;
    state[6] = 0x1f83d9abUL;
    state[7] = 0x5be0cd19UL;
    for ( at = 0; at + 64 <= len; at += 64 ) {
        sha256_block( state, data + at );
    }
    rest = len - at;
    mem_set( last, 0, sizeof( last ) );
    mem_cpy( last, data + at, rest );
    last[rest] = 0x80;
    total = rest < 56 ? 64 : 128;
    last[total - 4] = (u8)(len >> 21);
    last[total - 3] = (u8)(len >> 13);
    last[total - 2] = (u8)(len >> 5);
    last[total - 1] = (u8)(len << 3);
    sha256_block( state, last );
    if ( total == 128 ) {
        sha256_block( state, last + 64 );
    }
    for ( index = 0; index < 32; index++ ) {
        digest[index] = (u8)(state[index >> 2] >> (24 - (index & 3) * 8));
    }
}

/* ------------------------------------------------------------------ */
/* SHA-384 and SHA-512                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    u32 hi, lo;
} U64;

/* the low halves of the round constants; the high halves are sha_k's
   for the first 64, and the last 16 are here whole */
static const u32 sha_k_lo[64] = {
    0xd728ae22UL, 0x23ef65cdUL, 0xec4d3b2fUL, 0x8189dbbcUL, 0xf348b538UL, 0xb605d019UL,
    0xaf194f9bUL, 0xda6d8118UL, 0xa3030242UL, 0x45706fbeUL, 0x4ee4b28cUL, 0xd5ffb4e2UL,
    0xf27b896fUL, 0x3b1696b1UL, 0x25c71235UL, 0xcf692694UL, 0x9ef14ad2UL, 0x384f25e3UL,
    0x8b8cd5b5UL, 0x77ac9c65UL, 0x592b0275UL, 0x6ea6e483UL, 0xbd41fbd4UL, 0x831153b5UL,
    0xee66dfabUL, 0x2db43210UL, 0x98fb213fUL, 0xbeef0ee4UL, 0x3da88fc2UL, 0x930aa725UL,
    0xe003826fUL, 0x0a0e6e70UL, 0x46d22ffcUL, 0x5c26c926UL, 0x5ac42aedUL, 0x9d95b3dfUL,
    0x8baf63deUL, 0x3c77b2a8UL, 0x47edaee6UL, 0x1482353bUL, 0x4cf10364UL, 0xbc423001UL,
    0xd0f89791UL, 0x0654be30UL, 0xd6ef5218UL, 0x5565a910UL, 0x5771202aUL, 0x32bbd1b8UL,
    0xb8d2d0c8UL, 0x5141ab53UL, 0xdf8eeb99UL, 0xe19b48a8UL, 0xc5c95a63UL, 0xe3418acbUL,
    0x7763e373UL, 0xd6b2b8a3UL, 0x5defb2fcUL, 0x43172f60UL, 0xa1f0ab72UL, 0x1a6439ecUL,
    0x23631e28UL, 0xde82bde9UL, 0xb2c67915UL, 0xe372532bUL };
static const U64 sha_k_tail[16] = {
    { 0xca273eceUL, 0xea26619cUL }, { 0xd186b8c7UL, 0x21c0c207UL }, { 0xeada7dd6UL, 0xcde0eb1eUL },
    { 0xf57d4f7fUL, 0xee6ed178UL }, { 0x06f067aaUL, 0x72176fbaUL }, { 0x0a637dc5UL, 0xa2c898a6UL },
    { 0x113f9804UL, 0xbef90daeUL }, { 0x1b710b35UL, 0x131c471bUL }, { 0x28db77f5UL, 0x23047d84UL },
    { 0x32caab7bUL, 0x40c72493UL }, { 0x3c9ebe0aUL, 0x15c9bebcUL }, { 0x431d67c4UL, 0x9c100d4cUL },
    { 0x4cc5d4beUL, 0xcb3e42b6UL }, { 0x597f299cUL, 0xfc657e2aUL }, { 0x5fcb6fabUL, 0x3ad6faecUL },
    { 0x6c44198cUL, 0x4a475817UL } };

static U64 add64( U64 lhs, U64 rhs )
{
    U64 out;

    out.lo = lhs.lo + rhs.lo;
    out.hi = lhs.hi + rhs.hi + (out.lo < lhs.lo);
    return out;
}

static U64 ror64( U64 val, int bits )
{
    U64 out;
    u32 swap;

    if ( bits >= 32 ) {
        swap = val.hi;
        val.hi = val.lo;
        val.lo = swap;
        bits -= 32;
    }
    if ( bits == 0 ) {
        return val;
    }
    out.lo = (val.lo >> bits) | (val.hi << (32 - bits));
    out.hi = (val.hi >> bits) | (val.lo << (32 - bits));
    return out;
}

static U64 shr64( U64 val, int bits )       /* fewer than 32 */
{
    U64 out;

    out.lo = (val.lo >> bits) | (val.hi << (32 - bits));
    out.hi = val.hi >> bits;
    return out;
}

static U64 xor3( U64 one, U64 two, U64 three )
{
    U64 out;

    out.hi = one.hi ^ two.hi ^ three.hi;
    out.lo = one.lo ^ two.lo ^ three.lo;
    return out;
}

static void sha512_block( U64 *state, const u8 *data )
{
    static U64 words[80];
    U64 reg[8], sum1, sum2, pick, konst;
    int index;

    for ( index = 0; index < 16; index++ ) {
        words[index].hi = ((u32)data[index * 8] << 24) | ((u32)data[index * 8 + 1] << 16) |
                          ((u32)data[index * 8 + 2] << 8) | (u32)data[index * 8 + 3];
        words[index].lo = ((u32)data[index * 8 + 4] << 24) | ((u32)data[index * 8 + 5] << 16) |
                          ((u32)data[index * 8 + 6] << 8) | (u32)data[index * 8 + 7];
    }
    for ( ; index < 80; index++ ) {
        sum1 = xor3( ror64( words[index - 15], 1 ), ror64( words[index - 15], 8 ),
                     shr64( words[index - 15], 7 ) );
        sum2 = xor3( ror64( words[index - 2], 19 ), ror64( words[index - 2], 61 ),
                     shr64( words[index - 2], 6 ) );
        words[index] = add64( add64( words[index - 16], sum1 ), add64( words[index - 7], sum2 ) );
    }
    mem_cpy( reg, state, sizeof( reg ) );
    for ( index = 0; index < 80; index++ ) {
        if ( index < 64 ) {
            konst.hi = sha_k[index];
            konst.lo = sha_k_lo[index];
        } else {
            konst = sha_k_tail[index - 64];
        }
        pick.hi = (reg[4].hi & reg[5].hi) ^ (~reg[4].hi & reg[6].hi);
        pick.lo = (reg[4].lo & reg[5].lo) ^ (~reg[4].lo & reg[6].lo);
        sum1 = add64( add64( reg[7], xor3( ror64( reg[4], 14 ), ror64( reg[4], 18 ), ror64( reg[4], 41 ) ) ),
                      add64( add64( pick, konst ), words[index] ) );
        pick.hi = (reg[0].hi & reg[1].hi) ^ (reg[0].hi & reg[2].hi) ^ (reg[1].hi & reg[2].hi);
        pick.lo = (reg[0].lo & reg[1].lo) ^ (reg[0].lo & reg[2].lo) ^ (reg[1].lo & reg[2].lo);
        sum2 = add64( xor3( ror64( reg[0], 28 ), ror64( reg[0], 34 ), ror64( reg[0], 39 ) ), pick );
        reg[7] = reg[6];
        reg[6] = reg[5];
        reg[5] = reg[4];
        reg[4] = add64( reg[3], sum1 );
        reg[3] = reg[2];
        reg[2] = reg[1];
        reg[1] = reg[0];
        reg[0] = add64( sum1, sum2 );
    }
    for ( index = 0; index < 8; index++ ) {
        state[index] = add64( state[index], reg[index] );
    }
}

/* "bytes" of digest: 64 for SHA-512, 48 for SHA-384 */
void sha512( const u8 *data, u32 len, u8 *digest, int bytes )
{
    static const U64 init512[8] = {
        { 0x6a09e667UL, 0xf3bcc908UL }, { 0xbb67ae85UL, 0x84caa73bUL }, { 0x3c6ef372UL, 0xfe94f82bUL },
        { 0xa54ff53aUL, 0x5f1d36f1UL }, { 0x510e527fUL, 0xade682d1UL }, { 0x9b05688cUL, 0x2b3e6c1fUL },
        { 0x1f83d9abUL, 0xfb41bd6bUL }, { 0x5be0cd19UL, 0x137e2179UL } };
    static const U64 init384[8] = {
        { 0xcbbb9d5dUL, 0xc1059ed8UL }, { 0x629a292aUL, 0x367cd507UL }, { 0x9159015aUL, 0x3070dd17UL },
        { 0x152fecd8UL, 0xf70e5939UL }, { 0x67332667UL, 0xffc00b31UL }, { 0x8eb44a87UL, 0x68581511UL },
        { 0xdb0c2e0dUL, 0x64f98fa7UL }, { 0x47b5481dUL, 0xbefa4fa4UL } };
    U64 state[8];
    u8 last[256];
    u32 at, rest, total;
    int index;

    mem_cpy( state, bytes == 48 ? init384 : init512, sizeof( state ) );
    for ( at = 0; at + 128 <= len; at += 128 ) {
        sha512_block( state, data + at );
    }
    rest = len - at;
    mem_set( last, 0, sizeof( last ) );
    mem_cpy( last, data + at, rest );
    last[rest] = 0x80;
    total = rest < 112 ? 128 : 256;
    last[total - 4] = (u8)(len >> 21);
    last[total - 3] = (u8)(len >> 13);
    last[total - 2] = (u8)(len >> 5);
    last[total - 1] = (u8)(len << 3);
    sha512_block( state, last );
    if ( total == 256 ) {
        sha512_block( state, last + 128 );
    }
    for ( index = 0; index < bytes; index++ ) {
        digest[index] = (u8)((index & 4 ? state[index >> 3].lo : state[index >> 3].hi) >>
                             (24 - (index & 3) * 8));
    }
}

/* ------------------------------------------------------------------ */
/* the standard security handler                                       */
/* ------------------------------------------------------------------ */

static u8  str_o[48], str_u[48], str_oe[32], str_ue[32];
static u32 perms;

/* PDF 2.0's hash (algorithm 2.B): SHA-256 to start, then rounds of
   AES-128 over the password, the hash so far and the owner's 48 bytes,
   sixty-four times over, with the cipher text choosing the next hash */
static void hash_r6( const u8 *pw, u32 pwlen, const u8 *salt, const u8 *udata, u8 *out )
{
    u8 *work;
    AES aes;
    u8 key[64], input[127 + 8 + 48], iv[16];
    u32 inlen = 0, klen = 32, piece, total, sum;
    int round, index;

    mem_cpy( input, pw, pwlen );
    inlen = pwlen;
    mem_cpy( input + inlen, salt, 8 );
    inlen += 8;
    if ( udata ) {
        mem_cpy( input + inlen, udata, 48 );
        inlen += 48;
    }
    sha256( input, inlen, key );
    if ( doc.crypt_rev < 6 ) {
        mem_cpy( out, key, 32 );
        return;
    }
    work = (u8 *)xalloc( (127 + 64 + 48) * 64 );
    for ( round = 0; ; round++ ) {
        piece = 0;
        mem_cpy( work, pw, pwlen );
        piece += pwlen;
        mem_cpy( work + piece, key, klen );
        piece += klen;
        if ( udata ) {
            mem_cpy( work + piece, udata, 48 );
            piece += 48;
        }
        for ( index = 1; index < 64; index++ ) {
            mem_cpy( work + (u32)index * piece, work, piece );
        }
        total = piece * 64;
        aes_key( &aes, key, 16 );
        mem_cpy( iv, key + 16, 16 );
        aes_cbc_encrypt( &aes, iv, work, total );
        sum = 0;
        for ( index = 0; index < 16; index++ ) {
            sum += work[index];
        }
        switch ( sum % 3 ) {
        case 0:
            sha256( work, total, key );
            klen = 32;
            break;
        case 1:
            sha512( work, total, key, 48 );
            klen = 48;
            break;
        default:
            sha512( work, total, key, 64 );
            klen = 64;
            break;
        }
        if ( round >= 63 && work[total - 1] <= (u32)(round - 31) ) {
            break;
        }
    }
    mem_cpy( out, key, 32 );
    xfree( work );
}

static void pad_password( const u8 *pw, u32 len, u8 *out )
{
    if ( len > 32 ) {
        len = 32;
    }
    mem_cpy( out, pw, len );
    mem_cpy( out + len, pad_string, 32 - len );
}

/* revisions 2 to 4: does this padded user password open the file?  The
   key is left in doc.key if so. */
static int user_r4( const u8 *padded )
{
    MD5CTX ctx;
    RC4 rc4;
    u8 digest[16], test[32], key[16], pbytes[4];
    u32 keylen = doc.key_len;
    int round, index;

    pbytes[0] = (u8)perms;
    pbytes[1] = (u8)(perms >> 8);
    pbytes[2] = (u8)(perms >> 16);
    pbytes[3] = (u8)(perms >> 24);
    md5_init( &ctx );
    md5_update( &ctx, padded, 32 );
    md5_update( &ctx, str_o, 32 );
    md5_update( &ctx, pbytes, 4 );
    md5_update( &ctx, doc.file_id, doc.file_id_len );
    if ( doc.crypt_rev >= 4 && !doc.crypt_meta ) {
        mem_set( pbytes, 0xFF, 4 );
        md5_update( &ctx, pbytes, 4 );
    }
    md5_final( &ctx, digest );
    if ( doc.crypt_rev >= 3 ) {
        for ( round = 0; round < 50; round++ ) {
            md5( digest, keylen, digest );
        }
    }
    mem_cpy( doc.key, digest, keylen );

    if ( doc.crypt_rev == 2 ) {
        mem_cpy( test, pad_string, 32 );
        rc4_init( &rc4, doc.key, keylen );
        rc4_run( &rc4, test, 32 );
        return mem_cmp( test, str_u, 32 ) == 0;
    }
    md5_init( &ctx );
    md5_update( &ctx, pad_string, 32 );
    md5_update( &ctx, doc.file_id, doc.file_id_len );
    md5_final( &ctx, test );
    for ( round = 0; round < 20; round++ ) {
        for ( index = 0; index < (int)keylen; index++ ) {
            key[index] = (u8)(doc.key[index] ^ round);
        }
        rc4_init( &rc4, key, keylen );
        rc4_run( &rc4, test, 16 );
    }
    return mem_cmp( test, str_u, 16 ) == 0;
}

/* the owner's password, revisions 2 to 4: it decrypts /O into the
   user's */
static int owner_r4( const u8 *pw, u32 len )
{
    RC4 rc4;
    u8 padded[32], digest[16], key[16], user[32];
    u32 keylen = doc.key_len;
    int round, index;

    pad_password( pw, len, padded );
    md5( padded, 32, digest );
    if ( doc.crypt_rev >= 3 ) {
        for ( round = 0; round < 50; round++ ) {
            md5( digest, 16, digest );
        }
    }
    mem_cpy( user, str_o, 32 );
    if ( doc.crypt_rev == 2 ) {
        rc4_init( &rc4, digest, keylen );
        rc4_run( &rc4, user, 32 );
    } else {
        for ( round = 19; round >= 0; round-- ) {
            for ( index = 0; index < (int)keylen; index++ ) {
                key[index] = (u8)(digest[index] ^ round);
            }
            rc4_init( &rc4, key, keylen );
            rc4_run( &rc4, user, 32 );
        }
    }
    return user_r4( user );
}

static int password_r6( const u8 *pw, u32 len )
{
    AES aes;
    u8 hash[32], iv[16];

    if ( len > 127 ) {
        len = 127;
    }
    hash_r6( pw, len, str_u + 32, NULL, hash );
    if ( mem_cmp( hash, str_u, 32 ) == 0 ) {
        hash_r6( pw, len, str_u + 40, NULL, hash );
        mem_cpy( doc.key, str_ue, 32 );
    } else {
        hash_r6( pw, len, str_o + 32, str_u, hash );
        if ( mem_cmp( hash, str_o, 32 ) != 0 ) {
            return 0;
        }
        hash_r6( pw, len, str_o + 40, str_u, hash );
        mem_cpy( doc.key, str_oe, 32 );
    }
    aes_key( &aes, hash, 32 );
    mem_set( iv, 0, 16 );
    aes_cbc_decrypt( &aes, iv, doc.key, 32 );
    return 1;
}

int pdf_password( const char *pw, u32 len )
{
    u8 padded[32];
    int good;

    if ( doc.crypt_rev >= 5 ) {
        good = password_r6( (const u8 *)pw, len );
    } else {
        pad_password( (const u8 *)pw, len, padded );
        good = user_r4( padded ) || owner_r4( (const u8 *)pw, len );
    }
    doc.encrypted = good;
    return good;
}

static void copy_str( DICT *dict, const char *key, u8 *out, u32 max )
{
    OBJ *val = dict_get( dict, key );
    u32 len;

    mem_set( out, 0, max );
    if ( val->type == T_STR ) {
        len = val->u.str.len < max ? val->u.str.len : max;
        mem_cpy( out, val->u.str.ptr, len );
    }
}

static int method_of( DICT *filters, const char *name )
{
    const char *cfm;

    if ( name[0] == 0 || str_cmp( name, "Identity" ) == 0 ) {
        return CRYPT_NONE;
    }
    cfm = dict_name( dict_dict( filters, name ), "CFM" );
    if ( str_cmp( cfm, "V2" ) == 0 ) {
        return CRYPT_RC4;
    }
    if ( str_cmp( cfm, "AESV2" ) == 0 ) {
        return CRYPT_AES;
    }
    if ( str_cmp( cfm, "AESV3" ) == 0 ) {
        return CRYPT_AES256;
    }
    return str_cmp( cfm, "None" ) == 0 || cfm[0] == 0 ? CRYPT_NONE : -1;
}

int crypt_setup( DICT *enc )
{
    int version = (int)dict_int( enc, "V", 0 );
    s32 bits = dict_int( enc, "Length", 40 );
    DICT *filters;

    if ( str_cmp( dict_name( enc, "Filter" ), "Standard" ) != 0 ) {
        return PE_CRYPT;
    }
    doc.crypt_rev = (int)dict_int( enc, "R", 2 );
    perms = (u32)dict_int( enc, "P", 0 );
    doc.crypt_meta = (int)dict_int( enc, "EncryptMetadata", 1 );
    copy_str( enc, "O", str_o, sizeof( str_o ) );
    copy_str( enc, "U", str_u, sizeof( str_u ) );
    copy_str( enc, "OE", str_oe, sizeof( str_oe ) );
    copy_str( enc, "UE", str_ue, sizeof( str_ue ) );
    if ( bits < 40 || bits > 256 || (bits & 7) ) {
        bits = 40;
    }
    switch ( version ) {
    case 1:
        bits = 40;
        doc.stm_crypt = doc.str_crypt = CRYPT_RC4;
        break;
    case 2:
    case 3:
        doc.stm_crypt = doc.str_crypt = CRYPT_RC4;
        break;
    case 4:
    case 5:
        filters = dict_dict( enc, "CF" );
        doc.stm_crypt = method_of( filters, dict_name( enc, "StmF" ) );
        doc.str_crypt = method_of( filters, dict_name( enc, "StrF" ) );
        if ( doc.stm_crypt < 0 || doc.str_crypt < 0 ) {
            return PE_CRYPT;
        }
        bits = version == 5 ? 256 : 128;
        break;
    default:
        return PE_CRYPT;
    }
    if ( doc.crypt_rev < 2 || doc.crypt_rev > 6 || (doc.crypt_rev >= 5) != (version == 5) ) {
        return PE_CRYPT;
    }
    if ( doc.crypt_rev == 2 ) {
        bits = 40;
    }
    doc.key_len = (u32)bits / 8;
    if ( doc.key_len > 16 && version != 5 ) {
        doc.key_len = 16;
    }
    return pdf_password( "", 0 ) ? PE_NONE : PE_PASSWORD;
}

void crypt_object_key( u32 num, u32 gen, int aes, u8 *key, u32 *keylen )
{
    MD5CTX ctx;
    u8 tail[5], digest[16];

    if ( doc.crypt_rev >= 5 ) {
        mem_cpy( key, doc.key, 32 );
        *keylen = 32;
        return;
    }
    tail[0] = (u8)num;
    tail[1] = (u8)(num >> 8);
    tail[2] = (u8)(num >> 16);
    tail[3] = (u8)gen;
    tail[4] = (u8)(gen >> 8);
    md5_init( &ctx );
    md5_update( &ctx, doc.key, doc.key_len );
    md5_update( &ctx, tail, 5 );
    if ( aes ) {
        md5_update( &ctx, (const u8 *)"sAlT", 4 );
    }
    md5_final( &ctx, digest );
    *keylen = doc.key_len + 5 > 16 ? 16 : doc.key_len + 5;
    mem_cpy( key, digest, *keylen );
}

void crypt_string( u32 num, u32 gen, u8 *data, u32 *len )
{
    RC4 rc4;
    AES aes;
    u8 key[32], iv[16];
    u32 keylen, body;

    if ( doc.str_crypt == CRYPT_NONE || *len == 0 ) {
        return;
    }
    crypt_object_key( num, gen, doc.str_crypt == CRYPT_AES, key, &keylen );
    if ( doc.str_crypt == CRYPT_RC4 ) {
        rc4_init( &rc4, key, keylen );
        rc4_run( &rc4, data, *len );
        return;
    }
    if ( *len < 32 ) {
        *len = 0;
        return;
    }
    body = (*len - 16) & ~15UL;
    mem_cpy( iv, data, 16 );
    aes_key( &aes, key, keylen );
    mem_cpy( data, data + 16, body );
    aes_cbc_decrypt( &aes, iv, data, body );
    *len = aes_unpad( data, body );
    data[*len] = 0;
}

IN *crypt_stream( IN *src, u32 num, u32 gen )
{
    u8 key[32];
    u32 keylen;

    crypt_object_key( num, gen, doc.stm_crypt == CRYPT_AES, key, &keylen );
    if ( doc.stm_crypt == CRYPT_RC4 ) {
        return flt_rc4( src, key, keylen );
    }
    return flt_aes( src, key, keylen );
}

/* known answers, for the test program: 0 if every one comes out */
int crypt_selftest( void )
{
    static const u8 md5_abc[4] = { 0x90, 0x01, 0x50, 0x98 };
    static const u8 sha256_abc[4] = { 0xba, 0x78, 0x16, 0xbf };
    static const u8 sha512_abc[4] = { 0xdd, 0xaf, 0x35, 0xa1 };
    static const u8 sha512_end[4] = { 0xa5, 0x4c, 0xa4, 0x9f };
    static const u8 sha384_abc[4] = { 0xcb, 0x00, 0x75, 0x3f };
    static const u8 aes_out[4] = { 0x69, 0xc4, 0xe0, 0xd8 };
    static const u8 aes256_out[4] = { 0x8e, 0xa2, 0xb7, 0xca };
    static const u8 rc4_out[4] = { 0xbb, 0xf3, 0x16, 0xe8 };
    AES aes;
    RC4 rc4;
    u8 digest[64], block[16], key[32];
    int index, bad = 0;

    md5( (const u8 *)"abc", 3, digest );
    bad |= mem_cmp( digest, md5_abc, 4 ) != 0;
    sha256( (const u8 *)"abc", 3, digest );
    bad |= (mem_cmp( digest, sha256_abc, 4 ) != 0) << 1;
    sha512( (const u8 *)"abc", 3, digest, 64 );
    bad |= (mem_cmp( digest, sha512_abc, 4 ) != 0 || mem_cmp( digest + 60, sha512_end, 4 ) != 0) << 2;
    sha512( (const u8 *)"abc", 3, digest, 48 );
    bad |= (mem_cmp( digest, sha384_abc, 4 ) != 0) << 3;
    for ( index = 0; index < 32; index++ ) {
        key[index] = (u8)index;
    }
    for ( index = 0; index < 16; index++ ) {
        block[index] = (u8)(index * 17);
    }
    aes_key( &aes, key, 16 );
    aes_encrypt( &aes, block );
    bad |= (mem_cmp( block, aes_out, 4 ) != 0) << 4;
    aes_decrypt( &aes, block );
    bad |= (block[1] != 0x11 || block[15] != 0xFF) << 5;
    aes_key( &aes, key, 32 );
    aes_encrypt( &aes, block );
    bad |= (mem_cmp( block, aes256_out, 4 ) != 0) << 6;
    aes_decrypt( &aes, block );
    bad |= (block[1] != 0x11 || block[15] != 0xFF) << 7;
    mem_cpy( block, "Plaintext", 9 );
    rc4_init( &rc4, (const u8 *)"Key", 3 );
    rc4_run( &rc4, block, 9 );
    bad |= (mem_cmp( block, rc4_out, 4 ) != 0) << 8;
    return bad;
}
