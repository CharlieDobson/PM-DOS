/* ==========================================================================
 * RNG.C - Entropy gathering + SHA-256 hash-DRBG.  C89.
 * ==========================================================================
 * The DRBG core (rng_seed/rng_stir/rng_bytes) is pure and deterministic and
 * has a KAT (RNGTEST.C).  Only rng_init() touches the OS for its seed.
 * ========================================================================== */
#include "rng.h"
#include "sha.h"
#include <string.h>

/* PM-DOS has no Windows entropy; the CPU time-stamp counter, sampled
   across a little jitter, is the modest seed the header warns about */
static unsigned long rdtsc_lo( void );
#pragma aux rdtsc_lo = 0x0F 0x31 value [eax] modify exact [eax edx];

void rng_seed(nst_rng *r, const unsigned char *seed, int len)
{
    sha256(seed, (unsigned int)len, r->key);
    r->counter = 0;
}

void rng_stir(nst_rng *r, const unsigned char *data, int len)
{
    SHA256_CTX c;
    unsigned char out[32];
    sha256_init(&c);
    sha256_update(&c, r->key, 32);
    sha256_update(&c, data, (unsigned int)len);
    sha256_final(&c, out);
    memcpy(r->key, out, 32);
}

void rng_bytes(nst_rng *r, unsigned char *out, int len)
{
    unsigned char in[36], blk[32], kin[33];
    int n;

    while (len > 0) {
        memcpy(in, r->key, 32);
        in[32] = (unsigned char)(r->counter >> 24);
        in[33] = (unsigned char)(r->counter >> 16);
        in[34] = (unsigned char)(r->counter >> 8);
        in[35] = (unsigned char)(r->counter);
        sha256(in, 36, blk);
        r->counter++;

        n = (len < 32) ? len : 32;
        memcpy(out, blk, n);
        out += n;
        len -= n;
    }

    /* Rekey so past output can't be recovered from the current state. */
    memcpy(kin, r->key, 32);
    kin[32] = 0x01;
    sha256(kin, 33, r->key);
}

void rng_init(nst_rng *r)
{
    SHA256_CTX    c;
    unsigned char junk[32];             /* uninitialised: extra noise */
    void         *self = (void *)&c;
    volatile int  spin;
    unsigned long t;
    int           i, k;

    sha256_init(&c);
    sha256_update(&c, &self, sizeof(self));         /* stack address */
    sha256_update(&c, junk, sizeof(junk));          /* uninitialised bytes */

    /* sixteen time-stamp samples, each after a data-dependent spin so the
       low bits of the counter differ unpredictably between reads */
    for (k = 0; k < 16; k++) {
        t = rdtsc_lo();
        sha256_update(&c, &t, sizeof(t));
        spin = 0;
        for (i = 0; i < 97 + (int)(t & 63); i++)
            spin += i * 3;
        sha256_update(&c, (void *)&spin, sizeof(spin));
    }

    sha256_final(&c, r->key);
    r->counter = 0;
}
