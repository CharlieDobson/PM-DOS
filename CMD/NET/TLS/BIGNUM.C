/* ==========================================================================
 * BIGNUM.C - Fixed-width big-integer arithmetic + modular exponentiation.
 * Portable C89.  Little-endian 32-bit limbs.
 * ==========================================================================
 * Modular reduction is classical bit-by-bit long division, which keeps the
 * running remainder < mod and needs only a single conditional subtract per
 * bit (invariant: r < mod at each step -> after shift+bit, r < 2*mod).  It
 * is slow, so the hot paths avoid it: bn_modexp with an odd modulus (every
 * RSA modulus and curve order) and the curves' field arithmetic use
 * Montgomery multiplication on word arrays instead (the bn_w_ functions),
 * which reduces a word at a time.
 * ========================================================================== */
#include "bignum.h"

/* --- basic helpers -------------------------------------------------------- */
void bn_zero(bignum *a)
{
    int i;
    for (i = 0; i < BN_WORDS; i++)
        a->w[i] = 0;
}

void bn_set_word(bignum *a, uint32_t v)
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
    uint32_t top;
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

void bn_from_bytes(bignum *a, const uint8_t *bytes, int len)
{
    int i;
    bn_zero(a);
    /* bytes[len-1] is least-significant */
    for (i = 0; i < len; i++) {
        uint32_t b = bytes[len - 1 - i];
        a->w[i >> 2] |= b << ((i & 3) * 8);
    }
}

void bn_to_bytes(const bignum *a, uint8_t *bytes, int len)
{
    int i, pos;
    for (i = 0; i < len; i++) {
        pos = len - 1 - i;                          /* little-endian index */
        bytes[i] = (uint8_t)(a->w[pos >> 2] >> ((pos & 3) * 8));
    }
}

/* --- 32x32 -> 64 multiply-accumulate ------------------------------------- */
/* Returns the low word of a * b + c + d and puts the high word in *hi.  The
   sum never overflows 64 bits: (2^32-1)^2 + 2(2^32-1) = 2^64-1.  Open Watcom
   does it with one MUL; elsewhere it is four 16x16 products (no 64-bit type
   needed). */
#ifdef __WATCOMC__
uint32_t mac32(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t *hi);
#pragma aux mac32 =     \
    "mul edx"           \
    "add eax,ebx"       \
    "adc edx,0"         \
    "add eax,ecx"       \
    "adc edx,0"         \
    "mov [esi],edx"     \
    parm [eax] [edx] [ebx] [ecx] [esi] \
    value [eax]         \
    modify exact [eax edx];
#else
static uint32_t mac32(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t *hi)
{
    uint32_t al = a & 0xFFFF, ah = a >> 16;
    uint32_t bl = b & 0xFFFF, bh = b >> 16;
    uint32_t ll = al * bl;
    uint32_t t1 = ah * bl;
    uint32_t t2 = al * bh;
    uint32_t mid = t1 + t2;
    uint32_t midcarry = (mid < t1) ? 1u : 0u;       /* carry out of t1+t2 */
    uint32_t hh = ah * bh;
    uint32_t low = ll + (mid << 16);
    uint32_t loc = (low < ll) ? 1u : 0u;
    uint32_t high = hh + (mid >> 16) + loc + (midcarry << 16);
    low += c; if (low < c) high++;
    low += d; if (low < d) high++;
    *hi = high;
    return low;
}
#endif

/* p[0..an+bn-1] = a[0..an-1] * b[0..bn-1]  (schoolbook). */
static void umul(uint32_t *p, const uint32_t *a, int an, const uint32_t *b, int bn)
{
    int i, j;
    for (i = 0; i < an + bn; i++)
        p[i] = 0;
    for (i = 0; i < an; i++) {
        uint32_t carry = 0;
        for (j = 0; j < bn; j++)
            p[i + j] = mac32(a[i], b[j], p[i + j], carry, &carry);
        p[i + bn] = carry;                          /* column untouched so far */
    }
}

/* Compare magnitude of a[0..len-1] and b[0..len-1]. */
static int ucmp(const uint32_t *a, const uint32_t *b, int len)
{
    int i;
    for (i = len - 1; i >= 0; i--)
        if (a[i] != b[i])
            return (a[i] > b[i]) ? 1 : -1;
    return 0;
}

/* r[0..mlen] -= m[0..mlen-1]   (m zero-extended by one word; assumes r >= m). */
static void usub_ext(uint32_t *r, const uint32_t *m, int mlen)
{
    uint32_t borrow = 0;
    int i;
    for (i = 0; i < mlen; i++) {
        uint32_t tmp = m[i] + borrow;
        uint32_t nb  = (tmp < borrow) ? 1u : 0u;    /* m[i]+borrow overflowed */
        if (r[i] < tmp) nb = 1u;
        r[i] = r[i] - tmp;
        borrow = nb;
    }
    r[mlen] -= borrow;
}

/* r <<= 1 over len words. */
static void shl1(uint32_t *r, int len)
{
    uint32_t carry = 0;
    int i;
    for (i = 0; i < len; i++) {
        uint32_t nc = r[i] >> 31;
        r[i] = (r[i] << 1) | carry;
        carry = nc;
    }
}

/* dst = x[0..xwords-1] mod m[0..mlen-1]   (bit-by-bit long division). */
static void reduce(bignum *dst, const uint32_t *x, int xwords,
                   const uint32_t *m, int mlen)
{
    uint32_t r[BN_WORDS + 1];
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
    uint32_t prod[2 * BN_WORDS];
    int mlen = bn_words(m);
    if (mlen == 0) { bn_zero(out); return; }
    umul(prod, a->w, mlen, b->w, mlen);
    reduce(out, prod, 2 * mlen, m->w, mlen);
}

/* out = a mod m */
void bn_mod(bignum *out, const bignum *a, const bignum *m)
{
    int alen = bn_words(a), mlen = bn_words(m);
    if (mlen == 0 || alen == 0) { bn_zero(out); return; }
    reduce(out, a->w, alen, m->w, mlen);
}

/* out = (a + b) mod m   (a, b < m) */
void bn_addmod(bignum *out, const bignum *a, const bignum *b, const bignum *m)
{
    uint32_t t[BN_WORDS + 1];
    int i, mlen = bn_words(m);
    uint32_t carry = 0;
    if (mlen == 0) { bn_zero(out); return; }
    for (i = 0; i < mlen; i++) {
        uint32_t s = a->w[i] + carry;
        uint32_t c = (s < carry) ? 1u : 0u;
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
        uint32_t borrow = 0;
        for (i = 0; i < mlen; i++) {                 /* out may alias a: read */
            uint32_t bi = b->w[i] + borrow;          /* a[i] before writing   */
            uint32_t nb = (bi < borrow) ? 1u : 0u;
            if (a->w[i] < bi) nb = 1u;
            out->w[i] = a->w[i] - bi;
            borrow = nb;
        }
        for (; i < BN_WORDS; i++) out->w[i] = 0;     /* clear high words after */
    } else {                                         /* a < b: out = a + m - b */
        uint32_t t[BN_WORDS + 1];
        uint32_t carry = 0, borrow = 0;
        for (i = 0; i < mlen; i++) {
            uint32_t s = a->w[i] + carry;
            uint32_t c = (s < carry) ? 1u : 0u;
            s += m->w[i];
            if (s < m->w[i]) c = 1u;
            t[i] = s; carry = c;
        }
        t[mlen] = carry;
        for (i = 0; i < mlen; i++) {
            uint32_t bi = b->w[i] + borrow;
            uint32_t nb = (bi < borrow) ? 1u : 0u;
            if (t[i] < bi) nb = 1u;
            t[i] = t[i] - bi;
            borrow = nb;
        }
        t[mlen] -= borrow;
        reduce(out, t, mlen + 1, m->w, mlen);
    }
}

/* out = b^exp mod mod for an odd mod of mlen words, b already < mod, by
   Montgomery multiplication.  R = 2^(32 mlen).  R mod mod is 1 in that form,
   and the one slow (bit-by-bit) reduction; 2R is 2 in that form, and its
   power 32 mlen - that is, R - in that form is R^2 mod mod, which takes the
   numbers into the form. */
static void mont_modexp(bignum *out, const bignum *b, const bignum *exp,
                        const bignum *mod, int mlen)
{
    uint32_t big[BN_WORDS + 1];
    uint32_t rr[BN_WORDS], unit[BN_WORDS], one[BN_WORDS], two[BN_WORDS], bm[BN_WORDS], acc[BN_WORDS];
    uint32_t minv = bn_w_minv( mod->w[0] ), rbits = 32 * (uint32_t)mlen;
    bignum t;
    int i, ebits = 0;

    for ( i = 0; i < mlen; i++ ) big[i] = 0;
    big[mlen] = 1;                              /* R */
    reduce( &t, big, mlen + 1, mod->w, mlen );
    for ( i = 0; i < mlen; i++ ) {
        one[i] = t.w[i];
        unit[i] = 0;
    }
    unit[0] = 1;
    bn_w_addmod( two, one, one, mod->w, mlen );
    while ( ebits < 32 && ( rbits >> ebits ) != 0 ) ebits++;
    bn_w_mont_pow( rr, two, &rbits, ebits, mod->w, mlen, minv, one );
    bn_w_mont( bm, b->w, rr, mod->w, mlen, minv );      /* b R mod mod */
    bn_w_mont_pow( acc, bm, exp->w, bn_bitlen( exp ), mod->w, mlen, minv, one );
    bn_w_mont( acc, acc, unit, mod->w, mlen, minv );    /* back out of the form */
    bn_zero( out );
    for ( i = 0; i < mlen; i++ ) out->w[i] = acc[i];
}

/* out = base^exp mod mod. */
void bn_modexp(bignum *out, const bignum *base,
               const bignum *exp, const bignum *mod)
{
    bignum result, b;
    uint32_t prod[2 * BN_WORDS];
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

    if (mod->w[0] & 1) {                    /* odd: the fast way */
        mont_modexp(out, &b, exp, mod, mlen);
        return;
    }

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

/* --- word arrays (for the curves and mont_modexp) ------------------------ */
void bn_w_from_bytes( uint32_t *w, int nw, const uint8_t *bytes, int len )
{
    int i;
    for ( i = 0; i < nw; i++ ) w[i] = 0;
    for ( i = 0; i < len && i < nw * 4; i++ ) {
        w[i >> 2] |= (uint32_t)bytes[len - 1 - i] << ( ( i & 3 ) * 8 );
    }
}

void bn_w_to_bytes( const uint32_t *w, int nw, uint8_t *bytes, int len )
{
    int i, pos;
    for ( i = 0; i < len; i++ ) {
        pos = len - 1 - i;
        bytes[i] = pos < nw * 4 ? (uint8_t)( w[pos >> 2] >> ( ( pos & 3 ) * 8 ) ) : 0;
    }
}

int bn_w_cmp( const uint32_t *a, const uint32_t *b, int nw )
{
    return ucmp( a, b, nw );
}

int bn_w_is_zero( const uint32_t *a, int nw )
{
    int i;
    for ( i = 0; i < nw; i++ ) {
        if ( a[i] ) return 0;
    }
    return 1;
}

/* r = a + b mod m (a, b < m; r may be a or b) */
void bn_w_addmod( uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, int nw )
{
    uint32_t carry = 0, sum, ext[BN_WORDS + 1];
    int i;
    for ( i = 0; i < nw; i++ ) {
        sum = a[i] + carry;
        carry = sum < carry;
        ext[i] = sum + b[i];
        if ( ext[i] < sum ) carry = 1;
    }
    ext[nw] = carry;
    if ( carry || ucmp( ext, m, nw ) >= 0 ) usub_ext( ext, m, nw );
    for ( i = 0; i < nw; i++ ) r[i] = ext[i];
}

/* r = a - b mod m (a, b < m; r may be a or b) */
void bn_w_submod( uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, int nw )
{
    uint32_t borrow = 0, carry = 0, diff, sub, sum;
    int i;
    for ( i = 0; i < nw; i++ ) {
        sub = b[i] + borrow;
        borrow = sub < borrow;
        diff = a[i] - sub;
        if ( a[i] < sub ) borrow = 1;
        r[i] = diff;
    }
    if ( borrow ) {                             /* went below zero: add m back */
        for ( i = 0; i < nw; i++ ) {
            sum = r[i] + carry;
            carry = sum < carry;
            r[i] = sum + m[i];
            if ( r[i] < sum ) carry = 1;
        }
    }
}

/* -m0^-1 mod 2^32 for odd m0, by Newton's iteration: x = m0 is right to 3
   bits, and each step doubles that. */
uint32_t bn_w_minv( uint32_t m0 )
{
    uint32_t x = m0;
    int i;
    for ( i = 0; i < 4; i++ ) x *= 2 - m0 * x;
    return 0 - x;
}

/* r = a b / R mod m, R = 2^(32 nw): Montgomery multiplication, one word of
   b at a time, each step adding the multiple of m that clears the low word
   (Koc's CIOS).  a, b < m in, r < m out; r may be a or b. */
void bn_w_mont( uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, int nw, uint32_t minv )
{
    uint32_t t[BN_WORDS + 2];
    uint32_t carry, top, q;
    int i, j;

    for ( j = 0; j < nw + 2; j++ ) t[j] = 0;
    for ( i = 0; i < nw; i++ ) {
        carry = 0;
        for ( j = 0; j < nw; j++ ) {
            t[j] = mac32( a[j], b[i], t[j], carry, &carry );
        }
        top = t[nw] + carry;
        t[nw + 1] = top < carry;
        t[nw] = top;
        q = t[0] * minv;
        mac32( q, m[0], t[0], 0, &carry );      /* the low word becomes 0 */
        for ( j = 1; j < nw; j++ ) {
            t[j - 1] = mac32( q, m[j], t[j], carry, &carry );
        }
        top = t[nw] + carry;
        t[nw - 1] = top;
        t[nw] = t[nw + 1] + ( top < carry );
    }
    if ( t[nw] || ucmp( t, m, nw ) >= 0 ) usub_ext( t, m, nw );
    for ( j = 0; j < nw; j++ ) r[j] = t[j];
}

/* r = a^e in Montgomery form (a and one = R mod m in that form too), over
   the low ebits bits of e, most significant first; r may be a. */
void bn_w_mont_pow( uint32_t *r, const uint32_t *a, const uint32_t *e, int ebits,
                    const uint32_t *m, int nw, uint32_t minv, const uint32_t *one )
{
    uint32_t acc[BN_WORDS];
    int i;
    for ( i = 0; i < nw; i++ ) acc[i] = one[i];
    for ( i = ebits - 1; i >= 0; i-- ) {
        bn_w_mont( acc, acc, acc, m, nw, minv );
        if ( ( e[i >> 5] >> ( i & 31 ) ) & 1 ) bn_w_mont( acc, acc, a, m, nw, minv );
    }
    for ( i = 0; i < nw; i++ ) r[i] = acc[i];
}
