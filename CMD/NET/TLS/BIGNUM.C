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

/* --- 32x32 -> 64 multiply-accumulate ------------------------------------- */
/* Returns the low word of a * b + c + d and puts the high word in *hi.  The
   sum never overflows 64 bits: (2^32-1)^2 + 2(2^32-1) = 2^64-1.  Open Watcom
   does it with one MUL; elsewhere it is four 16x16 products (no 64-bit type
   needed). */
#ifdef __WATCOMC__
u32 mac32(u32 a, u32 b, u32 c, u32 d, u32 *hi);
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
static u32 mac32(u32 a, u32 b, u32 c, u32 d, u32 *hi)
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
    u32 high = hh + (mid >> 16) + loc + (midcarry << 16);
    low += c; if (low < c) high++;
    low += d; if (low < d) high++;
    *hi = high;
    return low;
}
#endif

/* p[0..an+bn-1] = a[0..an-1] * b[0..bn-1]  (schoolbook). */
static void umul(u32 *p, const u32 *a, int an, const u32 *b, int bn)
{
    int i, j;
    for (i = 0; i < an + bn; i++)
        p[i] = 0;
    for (i = 0; i < an; i++) {
        u32 carry = 0;
        for (j = 0; j < bn; j++)
            p[i + j] = mac32(a[i], b[j], p[i + j], carry, &carry);
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

/* out = b^exp mod mod for an odd mod of mlen words, b already < mod, by
   Montgomery multiplication.  R = 2^(32 mlen).  R mod mod is 1 in that form,
   and the one slow (bit-by-bit) reduction; 2R is 2 in that form, and its
   power 32 mlen - that is, R - in that form is R^2 mod mod, which takes the
   numbers into the form. */
static void mont_modexp(bignum *out, const bignum *b, const bignum *exp,
                        const bignum *mod, int mlen)
{
    u32 big[BN_WORDS + 1];
    u32 rr[BN_WORDS], unit[BN_WORDS], one[BN_WORDS], two[BN_WORDS], bm[BN_WORDS], acc[BN_WORDS];
    u32 minv = bn_w_minv( mod->w[0] ), rbits = 32 * (u32)mlen;
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
void bn_w_from_bytes( u32 *w, int nw, const unsigned char *bytes, int len )
{
    int i;
    for ( i = 0; i < nw; i++ ) w[i] = 0;
    for ( i = 0; i < len && i < nw * 4; i++ ) {
        w[i >> 2] |= (u32)bytes[len - 1 - i] << ( ( i & 3 ) * 8 );
    }
}

void bn_w_to_bytes( const u32 *w, int nw, unsigned char *bytes, int len )
{
    int i, pos;
    for ( i = 0; i < len; i++ ) {
        pos = len - 1 - i;
        bytes[i] = pos < nw * 4 ? (unsigned char)( w[pos >> 2] >> ( ( pos & 3 ) * 8 ) ) : 0;
    }
}

int bn_w_cmp( const u32 *a, const u32 *b, int nw )
{
    return ucmp( a, b, nw );
}

int bn_w_is_zero( const u32 *a, int nw )
{
    int i;
    for ( i = 0; i < nw; i++ ) {
        if ( a[i] ) return 0;
    }
    return 1;
}

/* r = a + b mod m (a, b < m; r may be a or b) */
void bn_w_addmod( u32 *r, const u32 *a, const u32 *b, const u32 *m, int nw )
{
    u32 carry = 0, sum, ext[BN_WORDS + 1];
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
void bn_w_submod( u32 *r, const u32 *a, const u32 *b, const u32 *m, int nw )
{
    u32 borrow = 0, carry = 0, diff, sub, sum;
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
u32 bn_w_minv( u32 m0 )
{
    u32 x = m0;
    int i;
    for ( i = 0; i < 4; i++ ) x *= 2 - m0 * x;
    return 0 - x;
}

/* r = a b / R mod m, R = 2^(32 nw): Montgomery multiplication, one word of
   b at a time, each step adding the multiple of m that clears the low word
   (Koc's CIOS).  a, b < m in, r < m out; r may be a or b. */
void bn_w_mont( u32 *r, const u32 *a, const u32 *b, const u32 *m, int nw, u32 minv )
{
    u32 t[BN_WORDS + 2];
    u32 carry, top, q;
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
void bn_w_mont_pow( u32 *r, const u32 *a, const u32 *e, int ebits,
                    const u32 *m, int nw, u32 minv, const u32 *one )
{
    u32 acc[BN_WORDS];
    int i;
    for ( i = 0; i < nw; i++ ) acc[i] = one[i];
    for ( i = ebits - 1; i >= 0; i-- ) {
        bn_w_mont( acc, acc, acc, m, nw, minv );
        if ( ( e[i >> 5] >> ( i & 31 ) ) & 1 ) bn_w_mont( acc, acc, a, m, nw, minv );
    }
    for ( i = 0; i < nw; i++ ) r[i] = acc[i];
}
