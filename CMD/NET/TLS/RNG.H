/* ==========================================================================
 * RNG.H - Entropy gathering + SHA-256 hash-DRBG for NETSPLOR.
 * Win32s / MSVC 2.2 + Open Watcom 1.9.
 * ==========================================================================
 * Win32s has no CryptoAPI (CryptGenRandom), so we scrape what weak entropy
 * the platform offers (timers, IDs, cursor, memory status, uninitialised
 * stack) and whiten it through SHA-256, then run a simple hash-DRBG:
 *
 *     block_i = SHA256(key || counter_i)   ; rekey key = SHA256(key || 0x01)
 *
 * Used for client_random and the 48-byte premaster secret.  IMPORTANT: on a
 * real Windows 3.1x box the seed entropy is modest - callers SHOULD rng_stir()
 * in extra unpredictability as it becomes available (e.g. network packet
 * arrival timings during the handshake).
 * ========================================================================== */
#ifndef NETSPLOR_RNG_H
#define NETSPLOR_RNG_H

typedef struct {
    unsigned char key[32];
    unsigned int  counter;
} nst_rng;

/* Seed from gathered OS entropy (call once). */
void rng_init(nst_rng *r);

/* Seed deterministically from a caller-supplied buffer (mainly for tests). */
void rng_seed(nst_rng *r, const unsigned char *seed, int len);

/* Fold additional entropy into the state at any time. */
void rng_stir(nst_rng *r, const unsigned char *data, int len);

/* Produce 'len' pseudo-random bytes. */
void rng_bytes(nst_rng *r, unsigned char *out, int len);

#endif /* NETSPLOR_RNG_H */
