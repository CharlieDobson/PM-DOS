/*
 * X87OPS.C - what the emulated instructions do: the arithmetic and the
 *            comparisons, loads and stores in every format, and the
 *            instructions of the D9 E0-FF row
 *
 * EACH INSTRUCTION GOES THE SAME WAY.  An empty operand register is
 * stack underflow.  A NaN goes through as a quiet NaN, a signaling one
 * raising IE on the way, and an operand in a format the 387 refuses is
 * IE and the indefinite NaN.  Then a denormal operand is DE, and then
 * the cases with an infinity or a zero in them, and only then is any
 * arithmetic done.  An unmasked IE, DE or ZE stops the instruction
 * there with nothing changed, which is what lets a handler find the
 * operands as they were.
 */
#include "x87emu.h"

#define RESBITS         70      /* a quotient or root bound for a register:
                                   64 bits, two to round by, and more */

static const u32 sizes[7] = { 4, 8, 10, 2, 4, 8, 10 };     /* by FMT_* */

static u32 rd16( const u8 *ptr )
{
    return (u32)ptr[0] | ( (u32)ptr[1] << 8 );
}

static u32 rd32( const u8 *ptr )
{
    return rd16( ptr ) | ( rd16( ptr + 2 ) << 16 );
}

static void wr16( u8 *ptr, u32 value )
{
    ptr[0] = (u8)value;
    ptr[1] = (u8)( value >> 8 );
}

static void wr32( u8 *ptr, u32 value )
{
    wr16( ptr, value );
    wr16( ptr + 2, value >> 16 );
}

/* An unmasked IE, DE or ZE has been raised: the instruction stops */
static int halted( x87ctx *c )
{
    return ( c->exc & ( SW_IE | SW_DE | SW_ZE ) & ~c->st->cw ) != 0;
}

/* A NaN, or a format nobody can compute with, among the operands
   (b may be 0).  -> 1, and r is what the result is. */
static int nan_in( x87ctx *c, wide *r, const wide *a, const wide *b )
{
    int anan = a->kind >= K_QNAN;
    int bnan = b && b->kind >= K_QNAN;

    if ( !anan && !bnan ) {
        return 0;
    }
    if ( a->kind == K_BAD || ( b && b->kind == K_BAD ) ) {
        xs_stop( c, SW_IE );
        xs_indef( r );
        return 1;
    }
    if ( a->kind == K_SNAN || ( b && b->kind == K_SNAN ) ) {
        xs_stop( c, SW_IE );
    }
    if ( anan && bnan ) {
        /* the one with the larger significand */
        u32 ahi = a->m[3] | 0x40000000;
        u32 bhi = b->m[3] | 0x40000000;

        *r = ( bhi > ahi || ( bhi == ahi && b->m[2] > a->m[2] ) ) ? *b : *a;
    } else {
        *r = anan ? *a : *b;
    }
    r->kind = K_QNAN;
    r->m[3] |= 0x40000000;
    r->denorm = 0;
    return 1;
}

static int is_one( const wide *w )
{
    return w->kind == K_FIN && w->e == 0 && w->m[3] == 0x80000000
        && ( w->m[2] | w->m[1] | w->m[0] ) == 0;
}

static void signed_kind( wide *w, int kind, int sign )
{
    w_zero( w );
    w->kind = (u8)kind;
    w->sign = (u8)sign;
    if ( kind == K_INF ) {
        w->m[3] = 0x80000000;
    }
}

/*---------------------------------------------------------------------
 * Add, multiply, subtract, divide
 *-------------------------------------------------------------------*/

/* r = a op b, op one of OP_ADD, OP_MUL, OP_SUB, OP_DIV.  -> 0 if the
   instruction stops. */
static int arith( x87ctx *c, int op, wide *r, wide *a, wide *b )
{
    int rc = xs_rc( c );
    int sign;

    if ( nan_in( c, r, a, b ) ) {
        return !halted( c );
    }
    if ( ( a->denorm || b->denorm ) && xs_stop( c, SW_DE ) ) {
        return 0;
    }
    sign = a->sign ^ b->sign;
    switch ( op ) {
    case OP_SUB:
        b->sign ^= 1;
        /* and add */
    case OP_ADD:
        if ( a->kind == K_INF || b->kind == K_INF ) {
            if ( a->kind == b->kind && a->sign != b->sign ) {
                xs_stop( c, SW_IE );
                xs_indef( r );
            } else {
                *r = a->kind == K_INF ? *a : *b;
            }
        } else if ( a->kind == K_ZERO && b->kind == K_ZERO ) {
            *r = *a;
            if ( a->sign != b->sign ) {
                r->sign = (u8)( rc == RC_DOWN );
            }
        } else {
            w_add( r, a, b );
            if ( r->kind == K_ZERO ) {
                r->sign = (u8)( rc == RC_DOWN );
            }
        }
        break;
    case OP_MUL:
        if ( a->kind == K_INF || b->kind == K_INF ) {
            if ( a->kind == K_ZERO || b->kind == K_ZERO ) {
                xs_stop( c, SW_IE );
                xs_indef( r );
            } else {
                signed_kind( r, K_INF, sign );
            }
        } else if ( a->kind == K_ZERO || b->kind == K_ZERO ) {
            signed_kind( r, K_ZERO, sign );
        } else {
            w_mul( r, a, b );
        }
        break;
    default:
        if ( a->kind == K_INF ) {
            if ( b->kind == K_INF ) {
                xs_stop( c, SW_IE );
                xs_indef( r );
            } else {
                signed_kind( r, K_INF, sign );
            }
        } else if ( b->kind == K_INF ) {
            signed_kind( r, K_ZERO, sign );
        } else if ( b->kind == K_ZERO ) {
            if ( a->kind == K_ZERO ) {
                xs_stop( c, SW_IE );
                xs_indef( r );
            } else {
                xs_stop( c, SW_ZE );
                signed_kind( r, K_INF, sign );
            }
        } else if ( a->kind == K_ZERO ) {
            signed_kind( r, K_ZERO, sign );
        } else {
            w_div( r, a, b, RESBITS );
        }
        break;
    }
    return !halted( c );
}

/* ST(dst) = ST(dst) op ST(src), then pops */
void op_math( x87ctx *c, int op, int dst, int src, int pops )
{
    wide a, b, r;

    if ( xs_empty( c, dst ) || xs_empty( c, src ) ) {
        if ( !xs_under( c ) ) {
            return;
        }
        xs_indef( &r );
        xs_put( c, dst, &r );
    } else {
        xs_get( c, dst, &a );
        xs_get( c, src, &b );
        if ( op == OP_SUBR || op == OP_DIVR ) {
            r = a;
            a = b;
            b = r;
            op--;
        }
        if ( !arith( c, op, &r, &a, &b ) ) {
            return;
        }
        xs_result( c, dst, &r, xs_pc( c ) );
    }
    if ( pops ) {
        xs_pop( c );
    }
}

/*---------------------------------------------------------------------
 * Memory operands
 *-------------------------------------------------------------------*/

/* Eighteen digits, two to a byte from the lowest, and the sign in the
   tenth byte.  Built a byte at a time; under 2^60, so it is exact. */
static void bcd_in( wide *w, const u8 *ptr )
{
    wide part;
    int i;

    w_zero( w );
    for ( i = 8; i >= 0; i-- ) {
        if ( w->kind == K_FIN ) {
            w_muls( w, 100 );
        }
        w_int( &part, ( ptr[i] >> 4 ) * 10 + ( ptr[i] & 0x0F ) );
        w_add( w, w, &part );
    }
    w->sign = (u8)( ptr[9] >> 7 );
}

/* The memory operand, as a number.  -> 0 if it could not be reached. */
static int read_mem( x87ctx *c, int fmt, wide *w )
{
    u8 *ptr = xs_mem( c, sizes[fmt] );
    xreg x;
    u32 lo;

    if ( !ptr ) {
        return 0;
    }
    switch ( fmt ) {
    case FMT_F32:
        f_unpack( w, rd32( ptr ) );
        break;
    case FMT_F64:
        d_unpack( w, rd32( ptr ), rd32( ptr + 4 ) );
        break;
    case FMT_F80:
        x.lo = rd32( ptr );
        x.hi = rd32( ptr + 4 );
        x.se = (u16)rd16( ptr + 8 );
        x.pad = 0;
        x_unpack( w, &x );
        break;
    case FMT_I16:
        lo = rd16( ptr );
        lo = ( lo ^ 0x8000 ) - 0x8000;
        i_unpack( w, lo, ( lo & 0x80000000 ) ? 0xFFFFFFFF : 0 );
        break;
    case FMT_I32:
        lo = rd32( ptr );
        i_unpack( w, lo, ( lo & 0x80000000 ) ? 0xFFFFFFFF : 0 );
        break;
    case FMT_I64:
        i_unpack( w, rd32( ptr ), rd32( ptr + 4 ) );
        break;
    default:
        bcd_in( w, ptr );
        break;
    }
    return 1;
}

/* ST(0) = ST(0) op the memory operand, or the comparison */
void op_mathmem( x87ctx *c, int op, int fmt )
{
    wide a, b, r;

    if ( !read_mem( c, fmt, &b ) ) {
        return;
    }
    if ( op == OP_COM || op == OP_COMP ) {
        op_compare( c, 0, &b, 0, op == OP_COMP );
        return;
    }
    if ( xs_empty( c, 0 ) ) {
        if ( xs_under( c ) ) {
            xs_indef( &r );
            xs_put( c, 0, &r );
        }
        return;
    }
    xs_get( c, 0, &a );
    if ( op == OP_SUBR || op == OP_DIVR ) {
        r = a;
        a = b;
        b = r;
        op--;
    }
    if ( arith( c, op, &r, &a, &b ) ) {
        xs_result( c, 0, &r, xs_pc( c ) );
    }
}

void op_load( x87ctx *c, int fmt )
{
    u8 *ptr;
    xreg x;
    wide w;

    if ( fmt == FMT_F80 ) {
        /* ten bytes go in exactly as they are, whatever they are */
        ptr = xs_mem( c, 10 );
        if ( !ptr ) {
            return;
        }
        x.lo = rd32( ptr );
        x.hi = rd32( ptr + 4 );
        x.se = (u16)rd16( ptr + 8 );
        x.pad = 0;
        if ( xs_push( c ) ) {
            xs_putraw( c, 0, &x );
        }
        return;
    }
    if ( !read_mem( c, fmt, &w ) ) {
        return;
    }
    if ( w.kind == K_SNAN ) {
        if ( xs_stop( c, SW_IE ) ) {
            return;
        }
        w.kind = K_QNAN;
        w.m[3] |= 0x40000000;
    }
    if ( w.denorm ) {
        if ( xs_stop( c, SW_DE ) ) {
            return;
        }
        w.denorm = 0;                   /* in a register it is normal */
    }
    if ( xs_push( c ) ) {
        xs_put( c, 0, &w );
    }
}

/* ST(0), rounded to a whole number and stored as BCD; 0 if it has more
   than eighteen digits */
static int bcd_out( u8 *ptr, const wide *w )
{
    u32 lo, hi, top9, low9;
    int i;

    if ( !i_pack( w, &lo, &hi ) ) {
        return 0;
    }
    if ( hi & 0x80000000 ) {
        lo = ~lo + 1;
        hi = ~hi + ( lo == 0 );
    }
    /* under 10^18, which is 0DE0B6B3A7640000h */
    if ( hi > 0x0DE0B6B3 || ( hi == 0x0DE0B6B3 && lo >= 0xA7640000 ) ) {
        return 0;
    }
    top9 = div32( lo, hi, 1000000000, &low9 );
    for ( i = 0; i < 9; i++ ) {
        u32 *from = i < 4 ? &low9 : &top9;
        u32 pair;

        if ( i == 4 ) {
            /* byte 4 is digit 9 of the top half and digit 8 of the low */
            pair = ( ( top9 % 10 ) << 4 ) | ( low9 % 10 );
            top9 /= 10;
            low9 /= 10;
            ptr[i] = (u8)pair;
            continue;
        }
        pair = *from % 10;
        *from /= 10;
        pair |= ( *from % 10 ) << 4;
        *from /= 10;
        ptr[i] = (u8)pair;
    }
    ptr[9] = (u8)( w->sign << 7 );
    return 1;
}

void op_store( x87ctx *c, int fmt, int pops )
{
    u8 *ptr = xs_mem( c, sizes[fmt] );
    u32 flags = 0, round = 0, lo = 0, hi = 0;
    int fits;
    xreg x;
    wide w;

    if ( !ptr ) {
        return;
    }
    c->st->sw &= ~SW_C1;
    if ( xs_empty( c, 0 ) ) {
        if ( !xs_under( c ) ) {
            return;
        }
        xs_indef( &w );
    } else {
        xs_get( c, 0, &w );
    }

    switch ( fmt ) {
    case FMT_F80:
        if ( xs_empty( c, 0 ) ) {
            x_pack( &x, &w );
        } else {
            xs_getraw( c, 0, &x );
        }
        wr32( ptr, x.lo );
        wr32( ptr + 4, x.hi );
        wr16( ptr + 8, x.se );
        break;

    case FMT_F32:
    case FMT_F64:
        if ( w.kind == K_BAD ) {
            if ( xs_stop( c, SW_IE ) ) {
                return;
            }
            xs_indef( &w );
        } else if ( w.kind == K_SNAN ) {
            if ( xs_stop( c, SW_IE ) ) {
                return;
            }
            w.m[3] |= 0x40000000;
        } else if ( w.kind == K_FIN ) {
            if ( w.denorm && xs_stop( c, SW_DE ) ) {
                return;
            }
            if ( fmt == FMT_F32 ) {
                flags = w_fit( &w, 24, FLT_EMIN, FLT_EMAX, xs_rc( c ) );
            } else {
                flags = w_fit( &w, 53, DBL_EMIN, DBL_EMAX, xs_rc( c ) );
            }
            if ( ( flags & F_TINY ) && !( c->st->cw & CW_UM ) ) {
                flags |= SW_UE;
            }
            /* unmasked, these leave memory as it was */
            if ( xs_stop( c, flags & ( SW_OE | SW_UE ) ) ) {
                return;
            }
            c->exc |= flags & SW_PE;
            c->st->sw |= (u16)( flags & SW_C1 );
        }
        if ( fmt == FMT_F32 ) {
            wr32( ptr, f_pack( &w ) );
        } else {
            d_pack( &w, &lo, &hi );
            wr32( ptr, lo );
            wr32( ptr + 4, hi );
        }
        break;

    default:
        /* the integers and BCD: a whole number by RC, then its range */
        if ( w.kind == K_FIN ) {
            round = w_rint( &w, xs_rc( c ) );
        }
        fits = w.kind == K_ZERO || w.kind == K_FIN;
        if ( fmt == FMT_BCD ) {
            if ( !fits || !bcd_out( ptr, &w ) ) {
                if ( xs_stop( c, SW_IE ) ) {
                    return;
                }
                /* the BCD indefinite */
                wr32( ptr, 0 );
                wr16( ptr + 4, 0 );
                ptr[6] = 0;
                ptr[7] = 0xC0;
                ptr[8] = 0xFF;
                ptr[9] = 0xFF;
                break;
            }
        } else if ( fits ) {
            fits = i_pack( &w, &lo, &hi );
            if ( fits && fmt != FMT_I64 ) {
                fits = hi == ( ( lo & 0x80000000 ) ? 0xFFFFFFFF : 0 );
                if ( fits && fmt == FMT_I16 ) {
                    fits = lo + 0x8000 < 0x10000;
                }
            }
        }
        if ( !fits ) {
            if ( xs_stop( c, SW_IE ) ) {
                return;
            }
            /* the integer indefinite: the most negative one */
            round = 0;
            lo = fmt == FMT_I16 ? 0x8000 : fmt == FMT_I32 ? 0x80000000 : 0;
            hi = 0x80000000;
        }
        if ( round & R_INEXACT ) {
            c->exc |= SW_PE;
        }
        if ( round & R_UP ) {
            c->st->sw |= SW_C1;
        }
        if ( fmt == FMT_I16 ) {
            wr16( ptr, lo );
        } else if ( fmt == FMT_I32 ) {
            wr32( ptr, lo );
        } else if ( fmt == FMT_I64 ) {
            wr32( ptr, lo );
            wr32( ptr + 4, hi );
        }
        break;
    }
    if ( pops ) {
        xs_pop( c );
    }
}

/*---------------------------------------------------------------------
 * Comparisons
 *-------------------------------------------------------------------*/

/* a against b, neither a NaN: -1, 0 or 1 */
static int compare( const wide *a, const wide *b )
{
    int mag;

    if ( a->kind == K_ZERO && b->kind == K_ZERO ) {
        return 0;
    }
    if ( a->kind == K_ZERO ) {
        return b->sign ? 1 : -1;
    }
    if ( b->kind == K_ZERO ) {
        return a->sign ? -1 : 1;
    }
    if ( a->sign != b->sign ) {
        return a->sign ? -1 : 1;
    }
    if ( a->kind == K_INF || b->kind == K_INF ) {
        mag = a->kind == b->kind ? 0 : a->kind == K_INF ? 1 : -1;
    } else {
        mag = w_cmpmag( a, b );
    }
    return a->sign ? -mag : mag;
}

/* ST(0) against b, or against ST(src) when b is 0; pops 0, 1 or 2 */
void op_compare( x87ctx *c, int src, const wide *b, int mode, int pops )
{
    wide a, other;
    int order = 2;                      /* 2: unordered */
    u32 *flags = &c->frame->eflags;

    if ( xs_empty( c, 0 ) || ( !b && xs_empty( c, src ) ) ) {
        if ( !xs_under( c ) ) {
            return;
        }
    } else {
        xs_get( c, 0, &a );
        if ( b ) {
            other = *b;
        } else {
            xs_get( c, src, &other );
        }
        if ( a.kind >= K_QNAN || other.kind >= K_QNAN ) {
            /* FUCOM lets a quiet NaN be compared; nothing else does */
            if ( !( mode & CMP_QUIET ) || a.kind > K_QNAN || other.kind > K_QNAN ) {
                if ( xs_stop( c, SW_IE ) ) {
                    return;
                }
            }
        } else {
            if ( ( a.denorm || other.denorm ) && xs_stop( c, SW_DE ) ) {
                return;
            }
            order = compare( &a, &other );
        }
    }
    if ( mode & CMP_EFLAGS ) {
        *flags &= ~( EFL_CF | EFL_PF | EFL_AF | EFL_ZF | EFL_SF | EFL_OF );
        if ( order == 2 ) {
            *flags |= EFL_ZF | EFL_PF | EFL_CF;
        } else if ( order == 0 ) {
            *flags |= EFL_ZF;
        } else if ( order < 0 ) {
            *flags |= EFL_CF;
        }
        c->st->sw &= ~SW_C1;
    } else if ( order == 2 ) {
        xs_cc( c, SW_C3 | SW_C2 | SW_C0 );
    } else if ( order == 0 ) {
        xs_cc( c, SW_C3 );
    } else {
        xs_cc( c, order < 0 ? SW_C0 : 0 );
    }
    while ( pops-- > 0 ) {
        xs_pop( c );
    }
}

/*---------------------------------------------------------------------
 * The D9 E0-FF row
 *-------------------------------------------------------------------*/

/* ST(0) for a one-operand instruction.  -> 0 if it was empty, which is
   then dealt with. */
static int one_in( x87ctx *c, wide *x )
{
    wide r;

    if ( xs_empty( c, 0 ) ) {
        if ( xs_under( c ) ) {
            xs_indef( &r );
            xs_put( c, 0, &r );
        }
        return 0;
    }
    xs_get( c, 0, x );
    return 1;
}

/* ST(0) and ST(1), for the ones whose result goes to ST(1) */
static int two_in( x87ctx *c, wide *x, wide *y )
{
    wide r;

    if ( xs_empty( c, 0 ) || xs_empty( c, 1 ) ) {
        if ( xs_under( c ) ) {
            xs_indef( &r );
            xs_put( c, 1, &r );
            xs_pop( c );
        }
        return 0;
    }
    xs_get( c, 0, x );
    xs_get( c, 1, y );
    return 1;
}

static void op_fxam( x87ctx *c )
{
    xreg x;
    wide w;
    u32 cc;

    xs_getraw( c, 0, &x );
    if ( xs_empty( c, 0 ) ) {
        cc = SW_C3 | SW_C0;
    } else {
        x_unpack( &w, &x );
        switch ( w.kind ) {
        case K_ZERO:
            cc = SW_C3;
            break;
        case K_FIN:
            cc = w.denorm ? SW_C3 | SW_C2 : SW_C2;
            break;
        case K_INF:
            cc = SW_C2 | SW_C0;
            break;
        case K_QNAN:
        case K_SNAN:
            cc = SW_C0;
            break;
        default:
            cc = 0;
            break;
        }
    }
    if ( x.se & 0x8000 ) {
        cc |= SW_C1;
    }
    xs_cc( c, cc );
}

static void op_const( x87ctx *c, int which )
{
    wide w;

    if ( which < 0 ) {
        w_one( &w );
    } else if ( which > KC_LN_2 ) {
        w_zero( &w );
    } else {
        fn_const( &w, which );
        w_round( &w, 64, xs_rc( c ) );
    }
    if ( xs_push( c ) ) {
        xs_put( c, 0, &w );
    }
}

static void op_sqrt( x87ctx *c )
{
    wide x, r;

    if ( !one_in( c, &x ) ) {
        return;
    }
    if ( !nan_in( c, &r, &x, 0 ) ) {
        if ( x.denorm && xs_stop( c, SW_DE ) ) {
            return;
        }
        if ( x.kind == K_ZERO ) {
            r = x;
        } else if ( x.sign ) {
            xs_stop( c, SW_IE );
            xs_indef( &r );
        } else if ( x.kind == K_INF ) {
            r = x;
        } else {
            w_sqrt( &r, &x, RESBITS );
        }
    }
    if ( !halted( c ) ) {
        xs_result( c, 0, &r, xs_pc( c ) );
    }
}

static void op_rndint( x87ctx *c )
{
    wide x;
    u32 round = 0;

    if ( !one_in( c, &x ) ) {
        return;
    }
    if ( nan_in( c, &x, &x, 0 ) ) {
        if ( !halted( c ) ) {
            xs_put( c, 0, &x );
        }
        return;
    }
    if ( x.denorm && xs_stop( c, SW_DE ) ) {
        return;
    }
    if ( x.kind == K_FIN ) {
        round = w_rint( &x, xs_rc( c ) );
    }
    if ( round & R_INEXACT ) {
        c->exc |= SW_PE;
    }
    c->st->sw &= ~SW_C1;
    if ( round & R_UP ) {
        c->st->sw |= SW_C1;
    }
    xs_put( c, 0, &x );
}

static void op_scale( x87ctx *c )
{
    wide x, s, r, t;
    s32 count = 0;

    if ( !xs_empty( c, 0 ) && !xs_empty( c, 1 ) ) {
        xs_get( c, 0, &x );
        xs_get( c, 1, &s );
    } else {
        if ( xs_under( c ) ) {
            xs_indef( &r );
            xs_put( c, 0, &r );
        }
        return;
    }
    if ( !nan_in( c, &r, &x, &s ) ) {
        if ( ( x.denorm || s.denorm ) && xs_stop( c, SW_DE ) ) {
            return;
        }
        r = x;
        if ( s.kind == K_INF ) {
            if ( ( !s.sign && x.kind == K_ZERO ) || ( s.sign && x.kind == K_INF ) ) {
                xs_stop( c, SW_IE );
                xs_indef( &r );
            } else if ( x.kind == K_FIN ) {
                signed_kind( &r, s.sign ? K_ZERO : K_INF, x.sign );
            }
        } else if ( x.kind == K_FIN && s.kind == K_FIN ) {
            t = s;
            w_rint( &t, RC_CHOP );
            if ( t.kind == K_FIN ) {
                count = t.e > 20 ? 0x200000 : (s32)( t.m[3] >> ( 31 - t.e ) );
                if ( t.sign ) {
                    count = -count;
                }
            }
            r.e += count;
        }
    }
    if ( !halted( c ) ) {
        xs_result( c, 0, &r, 64 );
    }
}

static void op_xtract( x87ctx *c )
{
    wide x, expo, sig;

    if ( !one_in( c, &x ) ) {
        return;
    }
    if ( nan_in( c, &expo, &x, 0 ) ) {
        sig = expo;
    } else {
        if ( x.denorm && xs_stop( c, SW_DE ) ) {
            return;
        }
        sig = x;
        sig.denorm = 0;
        if ( x.kind == K_ZERO ) {
            if ( xs_stop( c, SW_ZE ) ) {
                return;
            }
            signed_kind( &expo, K_INF, 1 );
        } else if ( x.kind == K_INF ) {
            signed_kind( &expo, K_INF, 0 );
        } else {
            w_int( &expo, (u32)( x.e < 0 ? -x.e : x.e ) );
            if ( x.e < 0 ) {
                expo.sign = 1;
            }
            sig.e = 0;
        }
    }
    if ( halted( c ) ) {
        return;
    }
    xs_put( c, 0, &expo );
    if ( xs_push( c ) ) {
        xs_put( c, 0, &sig );
    }
}

static void op_prem( x87ctx *c, int ieee )
{
    wide x, y, r, t, ymag;
    u32 quot = 0, cc = 0;
    s32 diff;

    if ( !xs_empty( c, 0 ) && !xs_empty( c, 1 ) ) {
        xs_get( c, 0, &x );
        xs_get( c, 1, &y );
    } else {
        if ( xs_under( c ) ) {
            xs_indef( &r );
            xs_put( c, 0, &r );
        }
        return;
    }
    if ( nan_in( c, &r, &x, &y ) ) {
        if ( !halted( c ) ) {
            xs_put( c, 0, &r );
        }
        return;
    }
    if ( x.kind == K_INF || y.kind == K_ZERO ) {
        if ( !xs_stop( c, SW_IE ) ) {
            xs_indef( &r );
            xs_put( c, 0, &r );
        }
        return;
    }
    if ( ( x.denorm || y.denorm ) && xs_stop( c, SW_DE ) ) {
        return;
    }
    r = x;
    if ( x.kind == K_FIN && y.kind == K_FIN ) {
        ymag = y;
        ymag.sign = 0;
        diff = x.e - y.e;
        if ( diff >= 64 ) {
            /* a part of the way, as a 387 does it: C2 says to go again */
            w_prem( &r, &x, &y, 32 + (int)( diff % 32 ) );
            cc = SW_C2;
        } else {
            if ( diff >= 0 ) {
                quot = w_prem( &r, &x, &y, (int)diff );
            }
            if ( ieee && r.kind == K_FIN ) {
                /* the nearest quotient, not the truncated one: if what
                   is left is over half of y, one y more */
                int side;

                t = r;
                t.sign = 0;
                t.e++;
                side = w_cmpmag( &t, &ymag );
                if ( side > 0 || ( side == 0 && ( quot & 1 ) ) ) {
                    t = r;
                    t.sign = 0;
                    w_sub( &r, &ymag, &t );
                    r.sign = (u8)( x.sign ^ 1 );
                    quot++;
                }
            }
            if ( quot & 4 ) {
                cc |= SW_C0;
            }
            if ( quot & 2 ) {
                cc |= SW_C3;
            }
            if ( quot & 1 ) {
                cc |= SW_C1;
            }
        }
        if ( r.kind == K_ZERO ) {
            r.sign = x.sign;
        }
    }
    xs_result( c, 0, &r, 64 );
    xs_cc( c, cc );
}

static void op_exp2m1( x87ctx *c )
{
    wide x, r;

    if ( !one_in( c, &x ) ) {
        return;
    }
    if ( !nan_in( c, &r, &x, 0 ) ) {
        if ( x.denorm && xs_stop( c, SW_DE ) ) {
            return;
        }
        if ( x.kind == K_INF ) {
            r = x;
            if ( x.sign ) {
                w_one( &r );
                r.sign = 1;
            }
        } else {
            fn_exp2m1( &r, &x );
        }
    }
    if ( !halted( c ) ) {
        xs_result( c, 0, &r, 64 );
    }
}

/* FYL2X (plus1 = 0) and FYL2XP1: ST(1) = ST(1) log2 ST(0), or of
   ST(0) + 1, and pop */
static void op_ylog( x87ctx *c, int plus1 )
{
    wide x, y, r, lg;
    int below, at;

    if ( !two_in( c, &x, &y ) ) {
        return;
    }
    if ( nan_in( c, &r, &x, &y ) ) {
        goto done;
    }
    if ( ( x.denorm || y.denorm ) && xs_stop( c, SW_DE ) ) {
        return;
    }
    /* where the logarithm's argument is against its pole: below it
       (invalid), at it (log2 0), or above */
    if ( plus1 ) {
        w_one( &lg );
        lg.sign = 1;
        at = x.kind == K_FIN && compare( &x, &lg ) == 0;
        below = x.sign && ( x.kind == K_INF || ( x.kind == K_FIN && compare( &x, &lg ) < 0 ) );
    } else {
        at = x.kind == K_ZERO;
        below = x.sign && x.kind != K_ZERO;
    }
    if ( below ) {
        xs_stop( c, SW_IE );
        xs_indef( &r );
    } else if ( at ) {
        if ( y.kind == K_ZERO ) {
            xs_stop( c, SW_IE );
            xs_indef( &r );
        } else {
            if ( y.kind != K_INF && xs_stop( c, SW_ZE ) ) {
                return;
            }
            signed_kind( &r, K_INF, y.sign ^ 1 );
        }
    } else if ( x.kind == K_INF ) {
        if ( y.kind == K_ZERO ) {
            xs_stop( c, SW_IE );
            xs_indef( &r );
        } else {
            signed_kind( &r, K_INF, y.sign );
        }
    } else if ( ( plus1 && x.kind == K_ZERO ) || ( !plus1 && is_one( &x ) ) ) {
        /* the logarithm is 0 */
        if ( y.kind == K_INF ) {
            xs_stop( c, SW_IE );
            xs_indef( &r );
        } else {
            signed_kind( &r, K_ZERO, y.sign ^ ( plus1 ? x.sign : 0 ) );
        }
    } else {
        if ( plus1 ) {
            fn_log2p1( &lg, &x );
        } else {
            fn_log2( &lg, &x );
        }
        if ( y.kind == K_INF || y.kind == K_ZERO ) {
            signed_kind( &r, y.kind, y.sign ^ lg.sign );
        } else {
            w_mul( &r, &y, &lg );
        }
    }
done:
    if ( halted( c ) ) {
        return;
    }
    xs_result( c, 1, &r, 64 );
    xs_pop( c );
}

static void op_atan( x87ctx *c )
{
    wide x, y, r, quarter;

    if ( !two_in( c, &x, &y ) ) {
        return;
    }
    if ( !nan_in( c, &r, &y, &x ) ) {
        if ( ( x.denorm || y.denorm ) && xs_stop( c, SW_DE ) ) {
            return;
        }
        if ( y.kind == K_INF ) {
            fn_const( &r, KC_PI );
            if ( x.kind != K_INF ) {
                r.e--;                  /* pi/2 */
            } else if ( !x.sign ) {
                r.e -= 2;               /* pi/4 */
            } else {
                quarter = r;            /* 3 pi/4 */
                quarter.e -= 2;
                w_sub( &r, &r, &quarter );
            }
            r.sign = y.sign;
        } else if ( x.kind == K_INF ) {
            if ( x.sign ) {
                fn_const( &r, KC_PI );
            } else {
                w_zero( &r );
            }
            r.sign = y.sign;
        } else {
            fn_atan2( &r, &y, &x );
        }
    }
    if ( halted( c ) ) {
        return;
    }
    xs_result( c, 1, &r, 64 );
    xs_pop( c );
}

/* FSIN (want 1), FCOS (2), FSINCOS (3) and FPTAN (4) */
static void op_trig( x87ctx *c, int want )
{
    wide x, s, co, r;

    if ( !one_in( c, &x ) ) {
        return;
    }
    if ( x.kind == K_FIN && x.e >= 63 ) {
        c->st->sw |= SW_C2;             /* out of range: nothing done */
        return;
    }
    c->st->sw &= ~SW_C2;
    if ( nan_in( c, &s, &x, 0 ) ) {
        co = s;
    } else if ( x.kind == K_INF ) {
        xs_stop( c, SW_IE );
        xs_indef( &s );
        co = s;
    } else {
        if ( x.denorm && xs_stop( c, SW_DE ) ) {
            return;
        }
        fn_sincos( &s, &co, &x );
    }
    if ( halted( c ) ) {
        return;
    }
    switch ( want ) {
    case 1:
        xs_result( c, 0, &s, 64 );
        break;
    case 2:
        xs_result( c, 0, &co, 64 );
        break;
    case 3:
        xs_result( c, 0, &s, 64 );
        if ( xs_push( c ) ) {
            xs_result( c, 0, &co, 64 );
        }
        break;
    default:
        if ( s.kind == K_FIN ) {
            w_div( &r, &s, &co, 127 );
        } else {
            r = s;
        }
        xs_result( c, 0, &r, 64 );
        if ( xs_push( c ) ) {
            w_one( &r );
            xs_put( c, 0, &r );
        }
        break;
    }
}

void op_fn( x87ctx *c )
{
    wide w;

    switch ( c->modrm ) {
    case 0xE0:                          /* FCHS */
    case 0xE1:                          /* FABS */
        if ( one_in( c, &w ) ) {
            xreg x;

            xs_getraw( c, 0, &x );
            if ( c->modrm == 0xE0 ) {
                x.se ^= 0x8000;
            } else {
                x.se &= 0x7FFF;
            }
            xs_putraw( c, 0, &x );
            c->st->sw &= ~SW_C1;
        }
        return;
    case 0xE4:                          /* FTST */
        w_zero( &w );
        op_compare( c, 0, &w, 0, 0 );
        return;
    case 0xE5:
        op_fxam( c );
        return;
    case 0xE8:                          /* FLD1 */
        op_const( c, -1 );
        return;
    case 0xE9:
        op_const( c, KC_LOG2_10 );
        return;
    case 0xEA:
        op_const( c, KC_LOG2_E );
        return;
    case 0xEB:
        op_const( c, KC_PI );
        return;
    case 0xEC:
        op_const( c, KC_LOG10_2 );
        return;
    case 0xED:
        op_const( c, KC_LN_2 );
        return;
    case 0xEE:                          /* FLDZ */
        op_const( c, KC_LN_2 + 1 );
        return;
    case 0xF0:
        op_exp2m1( c );
        return;
    case 0xF1:
        op_ylog( c, 0 );
        return;
    case 0xF2:
        op_trig( c, 4 );
        return;
    case 0xF3:
        op_atan( c );
        return;
    case 0xF4:
        op_xtract( c );
        return;
    case 0xF5:
        op_prem( c, 1 );
        return;
    case 0xF8:
        op_prem( c, 0 );
        return;
    case 0xF9:
        op_ylog( c, 1 );
        return;
    case 0xFA:
        op_sqrt( c );
        return;
    case 0xFB:
        op_trig( c, 3 );
        return;
    case 0xFC:
        op_rndint( c );
        return;
    case 0xFD:
        op_scale( c );
        return;
    case 0xFE:
        op_trig( c, 1 );
        return;
    case 0xFF:
        op_trig( c, 2 );
        return;
    }
    c->fault = TRAP_UD;
}
