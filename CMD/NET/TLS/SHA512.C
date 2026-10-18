/* ==========================================================================
 * SHA512.C - SHA-384 via the SHA-512 compression function (FIPS 180-4).
 * 64-bit words are emulated as {hi,lo} 32-bit pairs (no __int64), so this is
 * MSVC 2.2 / Win32s clean.  One-shot only (inputs are small: a cert's tbs).
 * ========================================================================== */
#include "sha512.h"
#include <string.h>

#ifndef NST_U32_DEFINED
#define NST_U32_DEFINED
typedef unsigned int u32;
#endif

typedef struct { u32 hi, lo; } u64;

static u64 mk(u32 hi, u32 lo)          { u64 r; r.hi = hi; r.lo = lo; return r; }
static u64 add64(u64 a, u64 b)         { u64 r; u32 lo = a.lo + b.lo;
                                         r.lo = lo; r.hi = a.hi + b.hi + (lo < a.lo ? 1u : 0u);
                                         return r; }
static u64 xor64(u64 a, u64 b)         { return mk(a.hi ^ b.hi, a.lo ^ b.lo); }
static u64 and64(u64 a, u64 b)         { return mk(a.hi & b.hi, a.lo & b.lo); }
static u64 not64(u64 a)                { return mk(~a.hi, ~a.lo); }
static u64 or64(u64 a, u64 b)          { return mk(a.hi | b.hi, a.lo | b.lo); }

static u64 shr64(u64 x, int n)
{
    if (n == 0)      return x;
    if (n < 32)      return mk(x.hi >> n, (x.lo >> n) | (x.hi << (32 - n)));
    if (n < 64)      return mk(0u, x.hi >> (n - 32));
    return mk(0u, 0u);
}
static u64 shl64(u64 x, int n)
{
    if (n == 0)      return x;
    if (n < 32)      return mk((x.hi << n) | (x.lo >> (32 - n)), x.lo << n);
    if (n < 64)      return mk(x.lo << (n - 32), 0u);
    return mk(0u, 0u);
}
static u64 rotr64(u64 x, int n)        { return or64(shr64(x, n), shl64(x, 64 - n)); }

#define Ch(x,y,z)   xor64(and64(x,y), and64(not64(x),z))
#define Maj(x,y,z)  xor64(xor64(and64(x,y), and64(x,z)), and64(y,z))
#define BSIG0(x)    xor64(xor64(rotr64(x,28), rotr64(x,34)), rotr64(x,39))
#define BSIG1(x)    xor64(xor64(rotr64(x,14), rotr64(x,18)), rotr64(x,41))
#define SSIG0(x)    xor64(xor64(rotr64(x,1),  rotr64(x,8)),  shr64(x,7))
#define SSIG1(x)    xor64(xor64(rotr64(x,19), rotr64(x,61)), shr64(x,6))

static const u32 K[80][2] = {
 {0x428a2f98,0xd728ae22},{0x71374491,0x23ef65cd},{0xb5c0fbcf,0xec4d3b2f},{0xe9b5dba5,0x8189dbbc},
 {0x3956c25b,0xf348b538},{0x59f111f1,0xb605d019},{0x923f82a4,0xaf194f9b},{0xab1c5ed5,0xda6d8118},
 {0xd807aa98,0xa3030242},{0x12835b01,0x45706fbe},{0x243185be,0x4ee4b28c},{0x550c7dc3,0xd5ffb4e2},
 {0x72be5d74,0xf27b896f},{0x80deb1fe,0x3b1696b1},{0x9bdc06a7,0x25c71235},{0xc19bf174,0xcf692694},
 {0xe49b69c1,0x9ef14ad2},{0xefbe4786,0x384f25e3},{0x0fc19dc6,0x8b8cd5b5},{0x240ca1cc,0x77ac9c65},
 {0x2de92c6f,0x592b0275},{0x4a7484aa,0x6ea6e483},{0x5cb0a9dc,0xbd41fbd4},{0x76f988da,0x831153b5},
 {0x983e5152,0xee66dfab},{0xa831c66d,0x2db43210},{0xb00327c8,0x98fb213f},{0xbf597fc7,0xbeef0ee4},
 {0xc6e00bf3,0x3da88fc2},{0xd5a79147,0x930aa725},{0x06ca6351,0xe003826f},{0x14292967,0x0a0e6e70},
 {0x27b70a85,0x46d22ffc},{0x2e1b2138,0x5c26c926},{0x4d2c6dfc,0x5ac42aed},{0x53380d13,0x9d95b3df},
 {0x650a7354,0x8baf63de},{0x766a0abb,0x3c77b2a8},{0x81c2c92e,0x47edaee6},{0x92722c85,0x1482353b},
 {0xa2bfe8a1,0x4cf10364},{0xa81a664b,0xbc423001},{0xc24b8b70,0xd0f89791},{0xc76c51a3,0x0654be30},
 {0xd192e819,0xd6ef5218},{0xd6990624,0x5565a910},{0xf40e3585,0x5771202a},{0x106aa070,0x32bbd1b8},
 {0x19a4c116,0xb8d2d0c8},{0x1e376c08,0x5141ab53},{0x2748774c,0xdf8eeb99},{0x34b0bcb5,0xe19b48a8},
 {0x391c0cb3,0xc5c95a63},{0x4ed8aa4a,0xe3418acb},{0x5b9cca4f,0x7763e373},{0x682e6ff3,0xd6b2b8a3},
 {0x748f82ee,0x5defb2fc},{0x78a5636f,0x43172f60},{0x84c87814,0xa1f0ab72},{0x8cc70208,0x1a6439ec},
 {0x90befffa,0x23631e28},{0xa4506ceb,0xde82bde9},{0xbef9a3f7,0xb2c67915},{0xc67178f2,0xe372532b},
 {0xca273ece,0xea26619c},{0xd186b8c7,0x21c0c207},{0xeada7dd6,0xcde0eb1e},{0xf57d4f7f,0xee6ed178},
 {0x06f067aa,0x72176fba},{0x0a637dc5,0xa2c898a6},{0x113f9804,0xbef90dae},{0x1b710b35,0x131c471b},
 {0x28db77f5,0x23047d84},{0x32caab7b,0x40c72493},{0x3c9ebe0a,0x15c9bebc},{0x431d67c4,0x9c100d4c},
 {0x4cc5d4be,0xcb3e42b6},{0x597f299c,0xfc657e2a},{0x5fcb6fab,0x3ad6faec},{0x6c44198c,0x4a475817}
};

/* SHA-384 initial hash value. */
static const u32 H0[8][2] = {
 {0xcbbb9d5d,0xc1059ed8},{0x629a292a,0x367cd507},{0x9159015a,0x3070dd17},{0x152fecd8,0xf70e5939},
 {0x67332667,0xffc00b31},{0x8eb44a87,0x68581511},{0xdb0c2e0d,0x64f98fa7},{0x47b5481d,0xbefa4fa4}
};

static void compress(u64 H[8], const unsigned char *p)
{
    u64 w[80], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++) {
        w[i].hi = ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|(u32)p[3];
        w[i].lo = ((u32)p[4]<<24)|((u32)p[5]<<16)|((u32)p[6]<<8)|(u32)p[7];
        p += 8;
    }
    for (i = 16; i < 80; i++)
        w[i] = add64(add64(SSIG1(w[i-2]), w[i-7]), add64(SSIG0(w[i-15]), w[i-16]));

    a = H[0]; b = H[1]; c = H[2]; d = H[3];
    e = H[4]; f = H[5]; g = H[6]; h = H[7];

    for (i = 0; i < 80; i++) {
        t1 = add64(add64(h, BSIG1(e)), add64(Ch(e,f,g), add64(mk(K[i][0],K[i][1]), w[i])));
        t2 = add64(BSIG0(a), Maj(a,b,c));
        h = g; g = f; f = e; e = add64(d, t1);
        d = c; c = b; b = a; a = add64(t1, t2);
    }
    H[0]=add64(H[0],a); H[1]=add64(H[1],b); H[2]=add64(H[2],c); H[3]=add64(H[3],d);
    H[4]=add64(H[4],e); H[5]=add64(H[5],f); H[6]=add64(H[6],g); H[7]=add64(H[7],h);
}

void sha384(const void *data, unsigned int len, unsigned char out[48])
{
    u64 H[8];
    unsigned char block[128];
    const unsigned char *p = (const unsigned char *)data;
    unsigned int i, rem = len;

    for (i = 0; i < 8; i++) H[i] = mk(H0[i][0], H0[i][1]);

    while (rem >= 128) { compress(H, p); p += 128; rem -= 128; }

    memset(block, 0, sizeof(block));
    memcpy(block, p, rem);
    block[rem] = 0x80;
    if (rem >= 112) {                       /* length won't fit; emit an extra block */
        compress(H, block);
        memset(block, 0, sizeof(block));
    }
    /* 128-bit big-endian bit length; our inputs are small so only the low
       ~35 bits are ever nonzero (len < 2^29 bytes). */
    {
        u32 bits_lo = len << 3;
        u32 bits_hi = len >> 29;            /* carry of the <<3 into bits 32.. */
        block[120] = (unsigned char)(bits_hi >> 24);
        block[121] = (unsigned char)(bits_hi >> 16);
        block[122] = (unsigned char)(bits_hi >> 8);
        block[123] = (unsigned char)(bits_hi);
        block[124] = (unsigned char)(bits_lo >> 24);
        block[125] = (unsigned char)(bits_lo >> 16);
        block[126] = (unsigned char)(bits_lo >> 8);
        block[127] = (unsigned char)(bits_lo);
    }
    compress(H, block);

    for (i = 0; i < 6; i++) {               /* SHA-384 = first 6 of 8 words */
        out[i*8+0] = (unsigned char)(H[i].hi >> 24);
        out[i*8+1] = (unsigned char)(H[i].hi >> 16);
        out[i*8+2] = (unsigned char)(H[i].hi >> 8);
        out[i*8+3] = (unsigned char)(H[i].hi);
        out[i*8+4] = (unsigned char)(H[i].lo >> 24);
        out[i*8+5] = (unsigned char)(H[i].lo >> 16);
        out[i*8+6] = (unsigned char)(H[i].lo >> 8);
        out[i*8+7] = (unsigned char)(H[i].lo);
    }
}

/* ==========================================================================
 * Streaming SHA-384 (same compression function; state kept as hi/lo pairs).
 * ========================================================================== */
static void ctx_compress(SHA384_CTX *c, const unsigned char *p)
{
    u64 H[8];
    int i;
    for (i = 0; i < 8; i++) H[i] = mk(c->h[2*i], c->h[2*i+1]);
    compress(H, p);
    for (i = 0; i < 8; i++) { c->h[2*i] = H[i].hi; c->h[2*i+1] = H[i].lo; }
}

void sha384_init(SHA384_CTX *c)
{
    int i;
    for (i = 0; i < 8; i++) { c->h[2*i] = H0[i][0]; c->h[2*i+1] = H0[i][1]; }
    c->nbuf = 0; c->lenlo = 0; c->lenhi = 0;
}

void sha384_update(SHA384_CTX *c, const void *data, unsigned int len)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned int old = c->lenlo;
    c->lenlo += len;
    if (c->lenlo < old) c->lenhi++;             /* 64-bit byte counter */
    while (len > 0) {
        if (c->nbuf == 0 && len >= 128) {
            ctx_compress(c, p); p += 128; len -= 128;
        } else {
            unsigned int take = 128 - c->nbuf;
            if (take > len) take = len;
            memcpy(c->buf + c->nbuf, p, take);
            c->nbuf += take; p += take; len -= take;
            if (c->nbuf == 128) { ctx_compress(c, c->buf); c->nbuf = 0; }
        }
    }
}

void sha384_final(SHA384_CTX *c, unsigned char out[48])
{
    unsigned char block[128];
    unsigned int rem = c->nbuf, i;
    u32 bitlo = c->lenlo << 3;
    u32 bitmid = (c->lenhi << 3) | (c->lenlo >> 29);   /* low 64 bits of bitlen */

    memset(block, 0, sizeof(block));
    memcpy(block, c->buf, rem);
    block[rem] = 0x80;
    if (rem >= 112) {                           /* length won't fit: extra block */
        ctx_compress(c, block);
        memset(block, 0, sizeof(block));
    }
    block[120] = (unsigned char)(bitmid >> 24);
    block[121] = (unsigned char)(bitmid >> 16);
    block[122] = (unsigned char)(bitmid >> 8);
    block[123] = (unsigned char)(bitmid);
    block[124] = (unsigned char)(bitlo >> 24);
    block[125] = (unsigned char)(bitlo >> 16);
    block[126] = (unsigned char)(bitlo >> 8);
    block[127] = (unsigned char)(bitlo);
    ctx_compress(c, block);

    for (i = 0; i < 6; i++) {                   /* first 6 of 8 words */
        out[i*8+0] = (unsigned char)(c->h[2*i]   >> 24);
        out[i*8+1] = (unsigned char)(c->h[2*i]   >> 16);
        out[i*8+2] = (unsigned char)(c->h[2*i]   >> 8);
        out[i*8+3] = (unsigned char)(c->h[2*i]);
        out[i*8+4] = (unsigned char)(c->h[2*i+1] >> 24);
        out[i*8+5] = (unsigned char)(c->h[2*i+1] >> 16);
        out[i*8+6] = (unsigned char)(c->h[2*i+1] >> 8);
        out[i*8+7] = (unsigned char)(c->h[2*i+1]);
    }
}

void hmac_sha384(const unsigned char *key, int keylen,
                 const unsigned char *msg, int msglen, unsigned char out[48])
{
    unsigned char k0[128], pad[128], inner[48], kh[48];
    SHA384_CTX c;
    int i;

    if (keylen > 128) { sha384(key, keylen, kh); key = kh; keylen = 48; }
    memset(k0, 0, 128);
    memcpy(k0, key, keylen);

    for (i = 0; i < 128; i++) pad[i] = (unsigned char)(k0[i] ^ 0x36);
    sha384_init(&c);
    sha384_update(&c, pad, 128);
    sha384_update(&c, msg, msglen);
    sha384_final(&c, inner);

    for (i = 0; i < 128; i++) pad[i] = (unsigned char)(k0[i] ^ 0x5c);
    sha384_init(&c);
    sha384_update(&c, pad, 128);
    sha384_update(&c, inner, 48);
    sha384_final(&c, out);
}

/* --------------------------------------------------------------------------
 * SHA-512: the same core as SHA-384 (same context, same compression and the
 * same streaming sha384_update), differing only in the initial hash value
 * and the full 8-word (64-byte) output.  SMB 3.1.1's pre-auth integrity hash
 * is SHA-512.
 * ------------------------------------------------------------------------ */
static const u32 H0_512[8][2] = {
 {0x6a09e667,0xf3bcc908},{0xbb67ae85,0x84caa73b},{0x3c6ef372,0xfe94f82b},{0xa54ff53a,0x5f1d36f1},
 {0x510e527f,0xade682d1},{0x9b05688c,0x2b3e6c1f},{0x1f83d9ab,0xfb41bd6b},{0x5be0cd19,0x137e2179}
};

void sha512_init(SHA384_CTX *c)
{
    int i;
    for (i = 0; i < 8; i++) { c->h[2*i] = H0_512[i][0]; c->h[2*i+1] = H0_512[i][1]; }
    c->nbuf = 0; c->lenlo = 0; c->lenhi = 0;
}

void sha512_final(SHA384_CTX *c, unsigned char out[64])
{
    unsigned char block[128];
    unsigned int rem = c->nbuf, i;
    u32 bitlo = c->lenlo << 3;
    u32 bitmid = (c->lenhi << 3) | (c->lenlo >> 29);

    memset(block, 0, sizeof(block));
    memcpy(block, c->buf, rem);
    block[rem] = 0x80;
    if (rem >= 112) {
        ctx_compress(c, block);
        memset(block, 0, sizeof(block));
    }
    block[120] = (unsigned char)(bitmid >> 24);
    block[121] = (unsigned char)(bitmid >> 16);
    block[122] = (unsigned char)(bitmid >> 8);
    block[123] = (unsigned char)(bitmid);
    block[124] = (unsigned char)(bitlo >> 24);
    block[125] = (unsigned char)(bitlo >> 16);
    block[126] = (unsigned char)(bitlo >> 8);
    block[127] = (unsigned char)(bitlo);
    ctx_compress(c, block);

    for (i = 0; i < 8; i++) {
        out[i*8+0] = (unsigned char)(c->h[2*i]   >> 24);
        out[i*8+1] = (unsigned char)(c->h[2*i]   >> 16);
        out[i*8+2] = (unsigned char)(c->h[2*i]   >> 8);
        out[i*8+3] = (unsigned char)(c->h[2*i]);
        out[i*8+4] = (unsigned char)(c->h[2*i+1] >> 24);
        out[i*8+5] = (unsigned char)(c->h[2*i+1] >> 16);
        out[i*8+6] = (unsigned char)(c->h[2*i+1] >> 8);
        out[i*8+7] = (unsigned char)(c->h[2*i+1]);
    }
}

void sha512(const void *data, unsigned int len, unsigned char out[64])
{
    SHA384_CTX c;
    sha512_init(&c);
    sha384_update(&c, data, len);
    sha512_final(&c, out);
}
