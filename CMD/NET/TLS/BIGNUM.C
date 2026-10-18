/* ==========================================================================
 * BIGNUM.C - Fixed-width big-integer arithmetic + modular exponentiation.
 * Portable C89.  Little-endian 32-bit limbs.
 * ==========================================================================
 * Modular reduction is classical bit-by-bit long division, which keeps the
 * running remainder < mod and needs only a single conditional subtract per
 * bit (invariant: r < mod at each step -> after shift+bit, r < 2*mod).
 * ========================================================================== */
#include "bignum.h"

/* --- basic helpers -------------------------------------------------------- */
void bn_zero(bignum *a)
{
    int i;
    for (i = 0; i < BN_WORDS; i++)
        a->w[i] = 0;
}

void bn_set_word(bignum *a, u32 v)
{
    bn_zero(a);
    a->w[0] = v;
}

static int bn_words(const bignum *a)
{
    int i;
    for (i = BN_WORDS - 1; i >= 0; i--)
        if (a->w[i])
            return i + 1;
    return 0;
}

int bn_cmp(const bignum *a, const bignum *b)
{
    int i;
    for (i = BN_WORDS - 1; i >= 0; i--) {
        if (a->w[i] != b->w[i])
            return (a->w[i] > b->w[i]) ? 1 : -1;
    }
    return 0;
}

int bn_bitlen(const bignum *a)
{
    int wl = bn_words(a), b;
    u32 top;
    if (wl == 0)
        return 0;
    top = a->w[wl - 1];
    b = 0;
    while (top) { b++; top >>= 1; }
    return (wl - 1) * 32 + b;
}

static int bn_getbit(const bignum *a, int i)
{
    return (int)((a->w[i >> 5] >> (i & 31)) & 1);
}

void bn_from_bytes(bignum *a, const unsigned char *bytes, int len)
{
    int i;
    bn_zero(a);
    /* bytes[len-1] is least-significant */
    for (i = 0; i < len; i++) {
        u32 b = bytes[len - 1 - i];
        a->w[i >> 2] |= b << ((i & 3) * 8);
    }
}

void bn_to_bytes(const bignum *a, unsigned char *bytes, int len)
{
    int i, pos;
    for (i = 0; i < len; i++) {
        pos = len - 1 - i;                          /* little-endian index */
        bytes[i] = (unsigned char)(a->w[pos >> 2] >> ((pos & 3) * 8));
    }
}

/* --- 32x32 -> 64 multiply (no 64-bit type needed) ------------------------- */
static void mul32(u32 a, u32 b, u32 *lo, u32 *hi)
{
    u32 al = a & 0xFFFF, ah = a >> 16;
    u32 bl = b & 0xFFFF, bh = b >> 16;
    u32 ll = al * bl;
    u32 t1 = ah * bl;
    u32 t2 = al * bh;
    u32 mid = t1 + t2;
    u32 midcarry = (mid < t1) ? 1u : 0u;            /* carry out of t1+t2 */
    u32 hh = ah * bh;
    u32 low = ll + (mid << 16);
    u32 loc = (low < ll) ? 1u : 0u;
    *lo = low;
    *hi = hh + (mid >> 16) + loc + (midcarry << 16);
}

/* p[0..an+bn-1] = a[0..an-1] * b[0..bn-1]  (schoolbook). */
static void umul(u32 *p, const u32 *a, int an, const u32 *b, int bn)
{
    int i, j;
    for (i = 0; i < an + bn; i++)
        p[i] = 0;
    for (i = 0; i < an; i++) {
        u32 carry = 0;
        for (j = 0; j < bn; j++) {
            u32 lo, hi, s, c1, c2;
            mul32(a[i], b[j], &lo, &hi);
            s = p[i + j] + lo;      c1 = (s < p[i + j]) ? 1u : 0u;
            s = s + carry;          c2 = (s < carry)    ? 1u : 0u;
            p[i + j] = s;
            carry = hi + c1 + c2;                   /* proven not to overflow */
        }
        p[i + bn] = carry;                          /* column untouched so far */
    }
}

/* Compare magnitude of a[0..len-1] and b[0..len-1]. */
static int ucmp(const u32 *a, const u32 *b, int len)
{
    int i;
    for (i = len - 1; i >= 0; i--)
        if (a[i] != b[i])
            return (a[i] > b[i]) ? 1 : -1;
    return 0;
}

/* r[0..mlen] -= m[0..mlen-1]   (m zero-extended by one word; assumes r >= m). */
static void usub_ext(u32 *r, const u32 *m, int mlen)
{
    u32 borrow = 0;
    int i;
    for (i = 0; i < mlen; i++) {
        u32 tmp = m[i] + borrow;
        u32 nb  = (tmp < borrow) ? 1u : 0u;         /* m[i]+borrow overflowed */
        if (r[i] < tmp) nb = 1u;
        r[i] = r[i] - tmp;
        borrow = nb;
    }
    r[mlen] -= borrow;
}

/* r <<= 1 over len words. */
static void shl1(u32 *r, int len)
{
    u32 carry = 0;
    int i;
    for (i = 0; i < len; i++) {
        u32 nc = r[i] >> 31;
        r[i] = (r[i] << 1) | carry;
        carry = nc;
    }
}

/* dst = x[0..xwords-1] mod m[0..mlen-1]   (bit-by-bit long division). */
static void reduce(bignum *dst, const u32 *x, int xwords,
                   const u32 *m, int mlen)
{
    u32 r[BN_WORDS + 1];
    int i;

    for (i = 0; i <= mlen; i++)
        r[i] = 0;
    for (i = xwords * 32 - 1; i >= 0; i--) {
        shl1(r, mlen + 1);
        r[0] |= (x[i >> 5] >> (i & 31)) & 1;
        if (r[mlen] != 0 || ucmp(r, m, mlen) >= 0)
            usub_ext(r, m, mlen);
    }
    bn_zero(dst);
    for (i = 0; i < mlen; i++)
        dst->w[i] = r[i];
}

/* --- convenience + modular arithmetic (for ECP256.C) --------------------- */
int bn_is_zero(const bignum *a)
{
    int i;
    for (i = 0; i < BN_WORDS; i++)
        if (a->w[i]) return 0;
    return 1;
}

int bn_bit(const bignum *a, int i)
{
    if (i < 0 || i >= BN_WORDS * 32) return 0;
    return (int)((a->w[i >> 5] >> (i & 31)) & 1);
}

void bn_copy(bignum *dst, const bignum *src)
{
    int i;
    for (i = 0; i < BN_WORDS; i++)
        dst->w[i] = src->w[i];
}

/* out = (a * b) mod m */
void bn_mulmod(bignum *out, const bignum *a, const bignum *b, const bignum *m)
{
    u32 prod[2 * BN_WORDS];
    int mlen = bn_words(m);
    if (mlen == 0) { bn_zero(out); return; }
    umul(prod, a->w, mlen, b->w, mlen);
    reduce(out, prod, 2 * mlen, m->w, mlen);
}

/* out = (a + b) mod m   (a, b < m) */
void bn_addmod(bignum *out, const bignum *a, const bignum *b, const bignum *m)
{
    u32 t[BN_WORDS + 1];
    int i, mlen = bn_words(m);
    u32 carry = 0;
    if (mlen == 0) { bn_zero(out); return; }
    for (i = 0; i < mlen; i++) {
        u32 s = a->w[i] + carry;
        u32 c = (s < carry) ? 1u : 0u;
        s += b->w[i];
        if (s < b->w[i]) c = 1u;
        t[i] = s; carry = c;
    }
    t[mlen] = carry;
    reduce(out, t, mlen + 1, m->w, mlen);
}

/* out = (a - b) mod m   (a, b < m) */
void bn_submod(bignum *out, const bignum *a, const bignum *b, const bignum *m)
{
    int i, mlen = bn_words(m);
    if (mlen == 0) { bn_zero(out); return; }
    if (bn_cmp(a, b) >= 0) {                         /* a >= b: direct */
        u32 borrow = 0;
        for (i = 0; i < mlen; i++) {                 /* out may alias a: read */
            u32 bi = b->w[i] + borrow;               /* a[i] before writing   */
            u32 nb = (bi < borrow) ? 1u : 0u;
            if (a->w[i] < bi) nb = 1u;
            out->w[i] = a->w[i] - bi;
            borrow = nb;
        }
        for (; i < BN_WORDS; i++) out->w[i] = 0;     /* clear high words after */
    } else {                                         /* a < b: out = a + m - b */
        u32 t[BN_WORDS + 1];
        u32 carry = 0, borrow = 0;
        for (i = 0; i < mlen; i++) {
            u32 s = a->w[i] + carry;
            u32 c = (s < carry) ? 1u : 0u;
            s += m->w[i];
            if (s < m->w[i]) c = 1u;
            t[i] = s; carry = c;
        }
        t[mlen] = carry;
        for (i = 0; i < mlen; i++) {
            u32 bi = b->w[i] + borrow;
            u32 nb = (bi < borrow) ? 1u : 0u;
            if (t[i] < bi) nb = 1u;
            t[i] = t[i] - bi;
            borrow = nb;
        }
        t[mlen] -= borrow;
        reduce(out, t, mlen + 1, m->w, mlen);
    }
}

/* out = base^exp mod mod. */
void bn_modexp(bignum *out, const bignum *base,
               const bignum *exp, const bignum *mod)
{
    bignum result, b;
    u32 prod[2 * BN_WORDS];
    int mlen = bn_words(mod);
    int bits = bn_bitlen(exp);
    int i, blen;

    if (mlen == 0) {                        /* mod == 0: undefined; be safe */
        bn_zero(out);
        return;
    }

    /* b = base mod mod */
    blen = bn_words(base);
    if (blen == 0)
        bn_zero(&b);
    else
        reduce(&b, base->w, blen, mod->w, mlen);

    bn_set_word(&result, 1);

    for (i = bits - 1; i >= 0; i--) {
        /* result = result^2 mod mod */
        umul(prod, result.w, mlen, result.w, mlen);
        reduce(&result, prod, 2 * mlen, mod->w, mlen);
        if (bn_getbit(exp, i)) {
            /* result = result * b mod mod */
            umul(prod, result.w, mlen, b.w, mlen);
            reduce(&result, prod, 2 * mlen, mod->w, mlen);
        }
    }
    *out = result;
}
