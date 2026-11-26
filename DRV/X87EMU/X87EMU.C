/*
 * X87EMU.C - the coprocessor emulator's way in: the instruction is
 *            read, its operand found, and it is carried out
 *
 * x87_trap is called by fpu_nm (IO\FPU.INC) for exception 7 out of a
 * native program, with the fault frame, the program's six segments as
 * PMIO looked them up, and the state.  It answers TRAP_DONE, with the
 * frame's EIP moved past the instruction, or the exception the program
 * is to take instead - and then nothing of the state has changed.
 *
 * EVERY BYTE IT TOUCHES FOR THE PROGRAM IS ASKED ABOUT FIRST.  The
 * instruction and its operand are read and written through flat ring-0
 * pointers, so an offset is checked against its segment's limit and
 * the range against the page tables (fpu_uok) before any of it is used:
 * an operand the program could not have reached itself is a general
 * protection fault in the program, not a store into the kernel.
 *
 * WHAT IS NOT DONE.  FWAIT does not fault with EM set, so an unmasked
 * exception is reported at the next coprocessor instruction rather than
 * at an FWAIT in between.  FISTTP is the SSE3 instruction it looks like
 * and is an invalid opcode, as it is on every coprocessor a 386 or a
 * 486 can have.  An unmasked overflow or underflow is delivered as the
 * masked one would be - the program is about to be ended for it.
 */
#include "x87emu.h"

#define MAXLEN          15      /* the longest instruction there is */

/* FPU.INC reserves X87_STATE bytes for the state and X87_SEGREC for
   each segment; either array has a negative size if they disagree */
typedef char state_fits[sizeof( x87state ) <= X87_STATE ? 1 : -1];
typedef char seg_fits[sizeof( x87seg ) == 12 ? 1 : -1];

/*---------------------------------------------------------------------
 * The program's memory
 *-------------------------------------------------------------------*/

/* A flat pointer to len bytes at sreg:off, or 0 if the program could
   not reach every one of them itself */
static uint8_t *reach( x87ctx *c, int sreg, uint32_t off, unsigned int len )
{
    x87seg *seg = &c->segs[sreg];
    uint32_t lin;

    if ( !seg->ok || off > seg->limit || seg->limit - off < len - 1 ) {
        return 0;
    }
    lin = seg->base + off;
    if ( !fpu_uok( lin, len ) ) {
        return 0;
    }
    return (uint8_t *)lin;
}

/* The memory operand, len bytes of it */
uint8_t *xs_mem( x87ctx *c, unsigned int len )
{
    uint8_t *ptr = reach( c, c->easeg, c->ea, len );

    if ( !ptr ) {
        c->fault = TRAP_GP;
    }
    return ptr;
}

/* The next byte of the instruction */
static int fetch( x87ctx *c, uint8_t *byte )
{
    uint8_t *ptr;

    if ( c->next - c->start >= MAXLEN ) {
        return 0;
    }
    ptr = reach( c, SEG_CS, c->next, 1 );
    if ( !ptr ) {
        return 0;
    }
    *byte = *ptr;
    c->next++;
    return 1;
}

static int fetch_disp( x87ctx *c, uint32_t *value, int size )
{
    uint8_t byte;
    int i;

    *value = 0;
    for ( i = 0; i < size; i++ ) {
        if ( !fetch( c, &byte ) ) {
            return 0;
        }
        *value |= (uint32_t)byte << ( 8 * i );
    }
    if ( size == 1 && ( *value & 0x80 ) ) {
        *value |= 0xFFFFFF00;
    } else if ( size == 2 && ( *value & 0x8000 ) ) {
        *value |= 0xFFFF0000;
    }
    return 1;
}

/* A general register by its number in an instruction */
static uint32_t gpr( x87ctx *c, int num )
{
    x87frame *fr = c->frame;

    switch ( num ) {
    case 0:
        return fr->eax;
    case 1:
        return fr->ecx;
    case 2:
        return fr->edx;
    case 3:
        return fr->ebx;
    case 4:
        return fr->esp;                 /* the program's, not PUSHAD's */
    case 5:
        return fr->ebp;
    case 6:
        return fr->esi;
    }
    return fr->edi;
}

/* The operand's offset and segment, from ModRM and what follows it */
static int decode_ea( x87ctx *c )
{
    unsigned int mod = c->modrm >> 6;
    unsigned int rm = c->modrm & 7;
    uint32_t addr = 0, disp;
    int defseg = SEG_DS;
    uint8_t sib;

    if ( c->a16 ) {
        if ( mod == 0 && rm == 6 ) {
            if ( !fetch_disp( c, &addr, 2 ) ) {
                return 0;
            }
        } else {
            switch ( rm ) {
            case 0:
                addr = gpr( c, 3 ) + gpr( c, 6 );
                break;
            case 1:
                addr = gpr( c, 3 ) + gpr( c, 7 );
                break;
            case 2:
                addr = gpr( c, 5 ) + gpr( c, 6 );
                defseg = SEG_SS;
                break;
            case 3:
                addr = gpr( c, 5 ) + gpr( c, 7 );
                defseg = SEG_SS;
                break;
            case 4:
                addr = gpr( c, 6 );
                break;
            case 5:
                addr = gpr( c, 7 );
                break;
            case 6:
                addr = gpr( c, 5 );
                defseg = SEG_SS;
                break;
            default:
                addr = gpr( c, 3 );
                break;
            }
        }
        if ( mod == 1 || mod == 2 ) {
            if ( !fetch_disp( c, &disp, (int)mod ) ) {
                return 0;
            }
            addr += disp;
        }
        addr &= 0xFFFF;
    } else {
        if ( rm == 4 ) {
            unsigned int index, base;

            if ( !fetch( c, &sib ) ) {
                return 0;
            }
            index = ( sib >> 3 ) & 7;
            base = sib & 7;
            if ( index != 4 ) {
                addr = gpr( c, (int)index ) << ( sib >> 6 );
            }
            if ( base == 5 && mod == 0 ) {
                if ( !fetch_disp( c, &disp, 4 ) ) {
                    return 0;
                }
                addr += disp;
            } else {
                addr += gpr( c, (int)base );
                if ( base == 4 || base == 5 ) {
                    defseg = SEG_SS;
                }
            }
        } else if ( rm == 5 && mod == 0 ) {
            if ( !fetch_disp( c, &addr, 4 ) ) {
                return 0;
            }
        } else {
            addr = gpr( c, (int)rm );
            if ( rm == 5 ) {
                defseg = SEG_SS;
            }
        }
        if ( mod == 1 || mod == 2 ) {
            if ( !fetch_disp( c, &disp, mod == 1 ? 1 : 4 ) ) {
                return 0;
            }
            addr += disp;
        }
    }
    c->ea = addr;
    c->easeg = c->sreg >= 0 ? c->sreg : defseg;
    return 1;
}

/*---------------------------------------------------------------------
 * The stack and the status word
 *-------------------------------------------------------------------*/

static int top( x87ctx *c )
{
    return ( c->st->sw >> 11 ) & 7;
}

static int phys( x87ctx *c, int i )
{
    return ( top( c ) + i ) & 7;
}

static void set_top( x87ctx *c, int value )
{
    c->st->sw = (uint16_t)( ( c->st->sw & ~SW_TOP ) | ( ( value & 7 ) << 11 ) );
}

/* Raise status bits.  -> 1 if any of them is unmasked, which for IE, DE
   and ZE means the instruction must leave everything as it was. */
int xs_stop( x87ctx *c, uint32_t bits )
{
    c->exc |= bits;
    return ( bits & SW_EXC & ~c->st->cw ) != 0;
}

int xs_rc( x87ctx *c )
{
    return ( c->st->cw >> 10 ) & 3;
}

/* The precision control, in bits; the reserved setting is taken as 64 */
int xs_pc( x87ctx *c )
{
    switch ( ( c->st->cw >> 8 ) & 3 ) {
    case 0:
        return 24;
    case 2:
        return 53;
    }
    return 64;
}

int xs_empty( x87ctx *c, int i )
{
    return ( c->st->empty >> phys( c, i ) ) & 1;
}

void xs_get( x87ctx *c, int i, wide *w )
{
    x_unpack( w, &c->st->st[phys( c, i )] );
}

void xs_getraw( x87ctx *c, int i, xreg *x )
{
    *x = c->st->st[phys( c, i )];
}

void xs_put( x87ctx *c, int i, const wide *w )
{
    int reg = phys( c, i );

    x_pack( &c->st->st[reg], w );
    c->st->empty &= (uint8_t)~( 1 << reg );
}

void xs_putraw( x87ctx *c, int i, const xreg *x )
{
    int reg = phys( c, i );

    c->st->st[reg] = *x;
    c->st->empty &= (uint8_t)~( 1 << reg );
}

void xs_pop( x87ctx *c )
{
    c->st->empty |= (uint8_t)( 1 << phys( c, 0 ) );
    set_top( c, top( c ) + 1 );
}

/* The quiet NaN an invalid operation gives */
void xs_indef( wide *w )
{
    w_zero( w );
    w->sign = 1;
    w->kind = K_QNAN;
    w->m[3] = 0xC0000000;
}

/* Make room on the stack.  -> 1 if ST(0) is the caller's to write.  A
   full stack is overflow: masked, the push happens and the value is the
   indefinite NaN; unmasked, nothing happens.  Either way -> 0. */
int xs_push( x87ctx *c )
{
    wide indef;

    if ( !xs_empty( c, 7 ) ) {
        c->st->sw |= SW_C1;
        if ( xs_stop( c, SW_IE | SW_SF ) ) {
            return 0;
        }
        set_top( c, top( c ) - 1 );
        xs_indef( &indef );
        xs_put( c, 0, &indef );
        return 0;
    }
    c->st->sw &= ~SW_C1;
    set_top( c, top( c ) - 1 );
    return 1;
}

/* An operand register was empty.  -> 1 if that is masked, and the
   caller delivers the indefinite NaN. */
int xs_under( x87ctx *c )
{
    c->st->sw &= ~SW_C1;
    return !xs_stop( c, SW_IE | SW_SF );
}

/* C0, C1, C2 and C3 together */
void xs_cc( x87ctx *c, uint32_t cc )
{
    c->st->sw = (uint16_t)( ( c->st->sw & ~SW_CC ) | ( cc & SW_CC ) );
}

/* A result into ST(i): fitted to prec bits and the register's range,
   and C1 said for whether it was rounded up */
void xs_result( x87ctx *c, int i, wide *r, int prec )
{
    uint32_t flags = 0;

    if ( r->kind == K_FIN ) {
        flags = w_fit( r, prec, EXT_EMIN, EXT_EMAX, xs_rc( c ) );
        if ( ( flags & F_TINY ) && !( c->st->cw & CW_UM ) ) {
            flags |= SW_UE;
        }
    }
    c->exc |= flags & SW_EXC;
    c->st->sw = (uint16_t)( ( c->st->sw & ~SW_C1 ) | ( flags & SW_C1 ) );
    xs_put( c, i, r );
}

/*---------------------------------------------------------------------
 * The environment: FNSTENV, FLDENV, FNSAVE, FRSTOR
 *-------------------------------------------------------------------*/

static uint32_t tag_word( x87ctx *c )
{
    uint32_t tags = 0, tag;
    wide w;
    int reg;

    for ( reg = 0; reg < 8; reg++ ) {
        if ( ( c->st->empty >> reg ) & 1 ) {
            tag = 3;
        } else {
            x_unpack( &w, &c->st->st[reg] );
            if ( w.kind == K_ZERO ) {
                tag = 1;
            } else if ( w.kind == K_FIN && !w.denorm ) {
                tag = 0;
            } else {
                tag = 2;
            }
        }
        tags |= tag << ( 2 * reg );
    }
    return tags;
}

static void put16( uint8_t *ptr, uint32_t value )
{
    ptr[0] = (uint8_t)value;
    ptr[1] = (uint8_t)( value >> 8 );
}

static void put32( uint8_t *ptr, uint32_t value )
{
    put16( ptr, value );
    put16( ptr + 2, value >> 16 );
}

static uint32_t get16( const uint8_t *ptr )
{
    return (uint32_t)ptr[0] | ( (uint32_t)ptr[1] << 8 );
}

static uint32_t get32( const uint8_t *ptr )
{
    return get16( ptr ) | ( get16( ptr + 2 ) << 16 );
}

/* The environment's length in the layout the operand size picks */
static unsigned int env_len( x87ctx *c )
{
    return c->o16 ? 14 : 28;
}

static void store_env( x87ctx *c, uint8_t *ptr )
{
    x87state *st = c->st;

    if ( c->o16 ) {
        put16( ptr, st->cw );
        put16( ptr + 2, st->sw );
        put16( ptr + 4, tag_word( c ) );
        put16( ptr + 6, st->fip );
        put16( ptr + 8, st->fcs );
        put16( ptr + 10, st->fdp );
        put16( ptr + 12, st->fds );
        return;
    }
    put32( ptr, 0xFFFF0000 | st->cw );
    put32( ptr + 4, 0xFFFF0000 | st->sw );
    put32( ptr + 8, 0xFFFF0000 | tag_word( c ) );
    put32( ptr + 12, st->fip );
    put32( ptr + 16, st->fcs | ( (uint32_t)( st->fop & 0x7FF ) << 16 ) );
    put32( ptr + 20, st->fdp );
    put32( ptr + 24, 0xFFFF0000 | st->fds );
}

static void load_env( x87ctx *c, const uint8_t *ptr )
{
    x87state *st = c->st;
    uint32_t tags;
    int reg;

    if ( c->o16 ) {
        st->cw = (uint16_t)get16( ptr );
        st->sw = (uint16_t)get16( ptr + 2 );
        tags = get16( ptr + 4 );
        st->fip = get16( ptr + 6 );
        st->fcs = (uint16_t)get16( ptr + 8 );
        st->fdp = get16( ptr + 10 );
        st->fds = (uint16_t)get16( ptr + 12 );
        st->fop = 0;
    } else {
        st->cw = (uint16_t)get32( ptr );
        st->sw = (uint16_t)get32( ptr + 4 );
        tags = get32( ptr + 8 );
        st->fip = get32( ptr + 12 );
        st->fcs = (uint16_t)get32( ptr + 16 );
        st->fop = (uint16_t)( ( get32( ptr + 16 ) >> 16 ) & 0x7FF );
        st->fdp = get32( ptr + 20 );
        st->fds = (uint16_t)get32( ptr + 24 );
    }
    st->cw = (uint16_t)( ( st->cw & 0x1F7F ) | 0x0040 );
    /* only "empty" is kept of the tags: the 387 works the rest out
       again from what is in the register */
    st->empty = 0;
    for ( reg = 0; reg < 8; reg++ ) {
        if ( ( ( tags >> ( 2 * reg ) ) & 3 ) == 3 ) {
            st->empty |= (uint8_t)( 1 << reg );
        }
    }
}

void x87_init( x87state *st )
{
    st->cw = CW_INIT;
    st->sw = 0;
    st->empty = 0xFF;
    st->fip = 0;
    st->fcs = 0;
    st->fop = 0;
    st->fdp = 0;
    st->fds = 0;
}

static void op_env( x87ctx *c, int save, int regs )
{
    unsigned int len = env_len( c ) + ( regs ? 80 : 0 );
    uint8_t *ptr = xs_mem( c, len );
    xreg *x;
    int i;

    if ( !ptr ) {
        return;
    }
    if ( save ) {
        store_env( c, ptr );
        if ( regs ) {
            for ( i = 0; i < 8; i++ ) {
                x = &c->st->st[phys( c, i )];
                put32( ptr + env_len( c ) + 10 * i, x->lo );
                put32( ptr + env_len( c ) + 10 * i + 4, x->hi );
                put16( ptr + env_len( c ) + 10 * i + 8, x->se );
            }
            x87_init( c->st );
        } else {
            c->st->cw |= 0x003F;        /* FNSTENV then masks everything */
        }
        return;
    }
    load_env( c, ptr );
    if ( regs ) {
        for ( i = 0; i < 8; i++ ) {
            x = &c->st->st[phys( c, i )];
            x->lo = get32( ptr + env_len( c ) + 10 * i );
            x->hi = get32( ptr + env_len( c ) + 10 * i + 4 );
            x->se = (uint16_t)get16( ptr + env_len( c ) + 10 * i + 8 );
            x->pad = 0;
        }
    }
}

/*---------------------------------------------------------------------
 * The rest of the stack instructions
 *-------------------------------------------------------------------*/

/* FLD ST(i): a copy, bits and all */
static void op_ldreg( x87ctx *c, int src )
{
    xreg x;
    wide w;

    if ( xs_empty( c, src ) ) {
        if ( !xs_under( c ) ) {
            return;
        }
        xs_indef( &w );
        if ( xs_push( c ) ) {
            xs_put( c, 0, &w );
        }
        return;
    }
    xs_getraw( c, src, &x );
    if ( xs_push( c ) ) {
        xs_putraw( c, 0, &x );
    }
}

/* FST and FSTP ST(i) */
static void op_streg( x87ctx *c, int dst, int pops )
{
    xreg x;
    wide w;

    c->st->sw &= ~SW_C1;
    if ( xs_empty( c, 0 ) ) {
        if ( !xs_under( c ) ) {
            return;
        }
        xs_indef( &w );
        xs_put( c, dst, &w );
    } else {
        xs_getraw( c, 0, &x );
        xs_putraw( c, dst, &x );
    }
    if ( pops ) {
        xs_pop( c );
    }
}

static void op_xch( x87ctx *c, int other )
{
    xreg a, b;
    wide w;

    c->st->sw &= ~SW_C1;
    if ( xs_empty( c, 0 ) || xs_empty( c, other ) ) {
        if ( !xs_under( c ) ) {
            return;
        }
        xs_indef( &w );
        if ( xs_empty( c, 0 ) ) {
            xs_put( c, 0, &w );
        }
        if ( xs_empty( c, other ) ) {
            xs_put( c, other, &w );
        }
    }
    xs_getraw( c, 0, &a );
    xs_getraw( c, other, &b );
    xs_putraw( c, 0, &b );
    xs_putraw( c, other, &a );
}

/* FCMOVcc: the condition from EFLAGS, as the ModRM byte names it */
static void op_cmov( x87ctx *c, int negate )
{
    uint32_t flags = c->frame->eflags;
    int cond;
    xreg x;
    wide w;

    switch ( ( c->modrm >> 3 ) & 3 ) {
    case 0:
        cond = ( flags & EFL_CF ) != 0;
        break;
    case 1:
        cond = ( flags & EFL_ZF ) != 0;
        break;
    case 2:
        cond = ( flags & ( EFL_CF | EFL_ZF ) ) != 0;
        break;
    default:
        cond = ( flags & EFL_PF ) != 0;
        break;
    }
    if ( negate ) {
        cond = !cond;
    }
    c->st->sw &= ~SW_C1;
    if ( xs_empty( c, 0 ) || xs_empty( c, c->modrm & 7 ) ) {
        if ( xs_under( c ) ) {
            xs_indef( &w );
            xs_put( c, 0, &w );
        }
        return;
    }
    if ( cond ) {
        xs_getraw( c, c->modrm & 7, &x );
        xs_putraw( c, 0, &x );
    }
}

/*---------------------------------------------------------------------
 * Which instructions are which
 *-------------------------------------------------------------------*/

/* The ones that do not look for an exception left outstanding: FNINIT,
   FNCLEX, FNSTSW, FNSTCW, FNSTENV, FNSAVE, and the three a 387 treats
   as no operation */
static int no_wait( x87ctx *c )
{
    unsigned int reg = ( c->modrm >> 3 ) & 7;

    if ( c->modrm < 0xC0 ) {
        return ( c->esc == 1 && reg >= 6 ) || ( c->esc == 5 && reg >= 6 );
    }
    return ( c->esc == 3 && c->modrm >= 0xE0 && c->modrm <= 0xE4 )
        || ( c->esc == 7 && c->modrm == 0xE0 );
}

/* ...and those, with FLDCW, FLDENV and FRSTOR, are the control
   instructions, which leave the last instruction pointer alone */
static int control( x87ctx *c )
{
    unsigned int reg = ( c->modrm >> 3 ) & 7;

    if ( no_wait( c ) ) {
        return 1;
    }
    return c->modrm < 0xC0
        && ( ( c->esc == 1 && ( reg == 4 || reg == 5 ) )
          || ( c->esc == 5 && reg == 4 ) );
}

/*---------------------------------------------------------------------
 * The eight escapes
 *-------------------------------------------------------------------*/
static void run( x87ctx *c )
{
    unsigned int reg = ( c->modrm >> 3 ) & 7;
    int rm = c->modrm & 7;
    int mem = c->modrm < 0xC0;
    uint8_t *ptr;

    switch ( c->esc ) {
    case 0:                             /* D8: m32 arithmetic, ST(0) op ST(i) */
        if ( mem ) {
            op_mathmem( c, (int)reg, FMT_F32 );
        } else if ( reg == OP_COM || reg == OP_COMP ) {
            op_compare( c, rm, 0, 0, reg == OP_COMP );
        } else {
            op_math( c, (int)reg, 0, rm, 0 );
        }
        return;

    case 1:                             /* D9 */
        if ( mem ) {
            switch ( reg ) {
            case 0:
                op_load( c, FMT_F32 );
                return;
            case 2:
            case 3:
                op_store( c, FMT_F32, reg == 3 );
                return;
            case 4:
                op_env( c, 0, 0 );
                return;
            case 5:
                ptr = xs_mem( c, 2 );
                if ( ptr ) {
                    c->st->cw = (uint16_t)( ( get16( ptr ) & 0x1F7F ) | 0x0040 );
                }
                return;
            case 6:
                op_env( c, 1, 0 );
                return;
            case 7:
                ptr = xs_mem( c, 2 );
                if ( ptr ) {
                    put16( ptr, c->st->cw );
                }
                return;
            }
            break;
        }
        switch ( reg ) {
        case 0:
            op_ldreg( c, rm );
            return;
        case 1:
            op_xch( c, rm );
            return;
        case 2:
            if ( rm == 0 ) {            /* FNOP */
                return;
            }
            break;
        case 3:                         /* FSTP1, an old alias */
            op_streg( c, rm, 1 );
            return;
        }
        if ( c->modrm == 0xF6 || c->modrm == 0xF7 ) {
            /* FDECSTP, FINCSTP: the top moves, the tags stay */
            set_top( c, top( c ) + ( c->modrm == 0xF7 ? 1 : -1 ) );
            c->st->sw &= ~SW_C1;
            return;
        }
        op_fn( c );
        return;
        break;

    case 2:                             /* DA: m32int arithmetic, FCMOV */
        if ( mem ) {
            op_mathmem( c, (int)reg, FMT_I32 );
            return;
        }
        if ( reg < 4 ) {
            op_cmov( c, 0 );
            return;
        }
        if ( c->modrm == 0xE9 ) {       /* FUCOMPP */
            op_compare( c, 1, 0, CMP_QUIET, 2 );
            return;
        }
        break;

    case 3:                             /* DB */
        if ( mem ) {
            switch ( reg ) {
            case 0:
                op_load( c, FMT_I32 );
                return;
            case 2:
            case 3:
                op_store( c, FMT_I32, reg == 3 );
                return;
            case 5:
                op_load( c, FMT_F80 );
                return;
            case 7:
                op_store( c, FMT_F80, 1 );
                return;
            }
            break;
        }
        if ( reg < 4 ) {
            op_cmov( c, 1 );
            return;
        }
        switch ( c->modrm ) {
        case 0xE0:                      /* FENI, FDISI, FSETPM */
        case 0xE1:
        case 0xE4:
            return;
        case 0xE2:                      /* FNCLEX */
            c->st->sw &= ~( SW_EXC | SW_SF | SW_ES | SW_B );
            return;
        case 0xE3:                      /* FNINIT */
            x87_init( c->st );
            return;
        }
        if ( reg == 5 ) {               /* FUCOMI */
            op_compare( c, rm, 0, CMP_QUIET | CMP_EFLAGS, 0 );
            return;
        }
        if ( reg == 6 ) {               /* FCOMI */
            op_compare( c, rm, 0, CMP_EFLAGS, 0 );
            return;
        }
        break;

    case 4:                             /* DC: m64 arithmetic, ST(i) op ST(0) */
        if ( mem ) {
            op_mathmem( c, (int)reg, FMT_F64 );
        } else if ( reg == OP_COM || reg == OP_COMP ) {
            op_compare( c, rm, 0, 0, reg == OP_COMP );
        } else {
            op_math( c, (int)( reg >= 4 ? reg ^ 1 : reg ), rm, 0, 0 );
        }
        return;

    case 5:                             /* DD */
        if ( mem ) {
            switch ( reg ) {
            case 0:
                op_load( c, FMT_F64 );
                return;
            case 2:
            case 3:
                op_store( c, FMT_F64, reg == 3 );
                return;
            case 4:
                op_env( c, 0, 1 );
                return;
            case 6:
                op_env( c, 1, 1 );
                return;
            case 7:
                ptr = xs_mem( c, 2 );
                if ( ptr ) {
                    put16( ptr, c->st->sw );
                }
                return;
            }
            break;
        }
        switch ( reg ) {
        case 0:                         /* FFREE */
            c->st->empty |= (uint8_t)( 1 << phys( c, rm ) );
            return;
        case 1:                         /* FXCH4 */
            op_xch( c, rm );
            return;
        case 2:
        case 3:
            op_streg( c, rm, reg == 3 );
            return;
        case 4:
        case 5:                         /* FUCOM, FUCOMP */
            op_compare( c, rm, 0, CMP_QUIET, reg == 5 );
            return;
        }
        break;

    case 6:                             /* DE: m16int arithmetic, and pop */
        if ( mem ) {
            op_mathmem( c, (int)reg, FMT_I16 );
            return;
        }
        if ( reg == OP_COM ) {          /* FCOMP5 */
            op_compare( c, rm, 0, 0, 1 );
            return;
        }
        if ( reg == OP_COMP ) {
            if ( rm == 1 ) {            /* FCOMPP */
                op_compare( c, 1, 0, 0, 2 );
                return;
            }
            break;
        }
        op_math( c, (int)( reg >= 4 ? reg ^ 1 : reg ), rm, 0, 1 );
        return;

    case 7:                             /* DF */
        if ( mem ) {
            switch ( reg ) {
            case 0:
                op_load( c, FMT_I16 );
                return;
            case 2:
            case 3:
                op_store( c, FMT_I16, reg == 3 );
                return;
            case 4:
                op_load( c, FMT_BCD );
                return;
            case 5:
                op_load( c, FMT_I64 );
                return;
            case 6:
                op_store( c, FMT_BCD, 1 );
                return;
            case 7:
                op_store( c, FMT_I64, 1 );
                return;
            }
            break;
        }
        switch ( reg ) {
        case 0:                         /* FFREEP */
            c->st->empty |= (uint8_t)( 1 << phys( c, rm ) );
            xs_pop( c );
            return;
        case 1:                         /* FXCH7 */
            op_xch( c, rm );
            return;
        case 2:
        case 3:                         /* FSTP8, FSTP9 */
            op_streg( c, rm, 1 );
            return;
        case 4:
            if ( rm == 0 ) {            /* FNSTSW AX */
                c->frame->eax = ( c->frame->eax & 0xFFFF0000 ) | c->st->sw;
                return;
            }
            break;
        case 5:                         /* FUCOMIP */
            op_compare( c, rm, 0, CMP_QUIET | CMP_EFLAGS, 1 );
            return;
        case 6:                         /* FCOMIP */
            op_compare( c, rm, 0, CMP_EFLAGS, 1 );
            return;
        }
        break;
    }
    c->fault = TRAP_UD;
}

/*---------------------------------------------------------------------
 * The way in
 *-------------------------------------------------------------------*/
uint32_t x87_trap( x87frame *frame, x87seg *segs, x87state *st )
{
    x87ctx ctx;
    x87ctx *c = &ctx;
    x87state before;
    uint8_t byte;

    c->frame = frame;
    c->segs = segs;
    c->st = st;
    c->start = frame->eip;
    c->next = frame->eip;
    c->ea = 0;
    c->easeg = SEG_DS;
    c->sreg = -1;
    c->exc = 0;
    c->o16 = 0;
    c->a16 = 0;
    c->fault = 0;

    for ( ;; ) {
        if ( !fetch( c, &byte ) ) {
            return TRAP_GP;
        }
        switch ( byte ) {
        case 0x26:
            c->sreg = SEG_ES;
            continue;
        case 0x2E:
            c->sreg = SEG_CS;
            continue;
        case 0x36:
            c->sreg = SEG_SS;
            continue;
        case 0x3E:
            c->sreg = SEG_DS;
            continue;
        case 0x64:
            c->sreg = SEG_FS;
            continue;
        case 0x65:
            c->sreg = SEG_GS;
            continue;
        case 0x66:
            c->o16 = 1;
            continue;
        case 0x67:
            c->a16 = 1;
            continue;
        case 0xF2:
        case 0xF3:
            continue;
        }
        break;
    }
    if ( byte < 0xD8 || byte > 0xDF ) {
        return TRAP_UD;                 /* nothing else raises 7 with EM */
    }
    c->esc = (uint8_t)( byte - 0xD8 );
    if ( !fetch( c, &c->modrm ) ) {
        return TRAP_GP;
    }
    if ( c->modrm < 0xC0 && !decode_ea( c ) ) {
        return TRAP_GP;
    }

    /* an unmasked exception is outstanding: this instruction is where
       the program hears of it */
    if ( ( st->sw & SW_ES ) && !no_wait( c ) ) {
        return TRAP_MF;
    }

    before = *st;
    run( c );
    if ( c->fault ) {
        *st = before;
        return c->fault;
    }
    if ( !control( c ) ) {
        st->fip = c->start;
        st->fcs = (uint16_t)segs[SEG_CS].sel;
        st->fop = (uint16_t)( ( (uint32_t)c->esc << 8 ) | c->modrm );
        if ( c->modrm < 0xC0 ) {
            st->fdp = c->ea;
            st->fds = segs[c->easeg].sel;
        }
    }
    st->sw |= (uint16_t)( c->exc & ( SW_EXC | SW_SF ) );
    if ( st->sw & ~st->cw & SW_EXC ) {
        st->sw |= SW_ES | SW_B;
    } else {
        st->sw &= ~( SW_ES | SW_B );
    }
    frame->eip = c->next;
    return TRAP_DONE;
}
