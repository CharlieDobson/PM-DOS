/*
 * ASM.C - A: the line assembler.
 *
 *   A [address]        then a line an instruction, each shown at its
 *                      address and assembled where the last one ended,
 *                      until a blank line
 *
 * It takes what 6.22's A took: the 8086 set, the 8087's, the prefixes
 * on a line of their own or in front of their instruction (REP MOVSB,
 * ES:), DB and DW, the sizes BYTE/WORD/DWORD/QWORD/TBYTE PTR and FAR
 * for JMP and CALL through memory - with 6.22's choice of encoding
 * where there are two (83h for a word immediate a signed byte holds,
 * AX's short forms first, 87h for XCHG).  For a native program the
 * same instructions take the 386's 32-bit registers and addressing,
 * [EBX+ESI*4+10] with its SIB byte.  The 8087 instructions are found
 * by trying encodings until the disassembler reads one back as the
 * line, so A and U cannot disagree.  A line it cannot assemble is
 * "^ Error" with the caret where the parser stopped, as the command
 * line's errors are, and the address does not move.
 *
 * Not taken: the 386's own new instructions (MOVZX, BSF, SETcc and the
 * rest), and a segment override inside the brackets (MOV AX,ES:[DI]),
 * which 6.22 refused too.
 */
#include "debug.h"

/* an operand, once parsed */
#define O_NONE   0
#define O_REG8   1
#define O_REG16  2
#define O_REG32  3
#define O_SREG   4
#define O_IMM    5
#define O_MEM    6
#define O_FAR    7              /* seg:off */

typedef struct {
    int  kind;
    int  reg;                  /* O_REG*, O_SREG: which */
    u32  imm;                  /* O_IMM, O_MEM disp, O_FAR off */
    u16  seg;                  /* O_FAR */
    int  mem_mod;              /* O_MEM: the mod field it needs */
    int  mem_rm;               /* ...and r/m */
    int  mem_has_disp;
    int  size;                 /* 0 unknown, else 8/16/32/64/80 - from PTR words */
    int  is_far;               /* FAR in front: JMP FAR [BX] */
    int  base, index;          /* 32-bit O_MEM, -1 none */
    int  has_sib;              /* 32-bit O_MEM: a SIB byte follows ModRM */
    u8   sib;
    int  disp32;               /* 32-bit O_MEM: the displacement is four bytes */
} OPND;

static int use32;
static u8  code[DIS_MAX];
static int clen;
static ADDR asm_at;

static void emit( u8 b )
{
    if ( clen < DIS_MAX ) {
        code[clen++] = b;
    }
}

static void emit16( u16 w )
{
    emit( (u8)w );
    emit( (u8)( w >> 8 ) );
}

static void emit32( u32 v )
{
    emit( (u8)v );
    emit( (u8)( v >> 8 ) );
    emit( (u8)( v >> 16 ) );
    emit( (u8)( v >> 24 ) );
}

/* the parser reads from lp, with syntax_error() on anything wrong */

static const char *reg8n[]  = { "AL","CL","DL","BL","AH","CH","DH","BH" };
static const char *reg16n[] = { "AX","CX","DX","BX","SP","BP","SI","DI" };
static const char *reg32n[] = { "EAX","ECX","EDX","EBX","ESP","EBP","ESI","EDI" };
static const char *sregn[]  = { "ES","CS","SS","DS","FS","GS" };

static int match_word( const char *w )
{
    u32 n = str_len( w );

    skip_blanks();
    if ( str_nicmp( lp, w, n ) == 0 ) {
        int after = lp[n];
        if ( !( ( after >= 'A' && after <= 'Z' ) || ( after >= 'a' && after <= 'z' ) ||
                ( after >= '0' && after <= '9' ) ) ) {
            lp += n;
            return 1;
        }
    }
    return 0;
}

static int find_reg( const char **tab, int count )
{
    int i;

    for ( i = 0; i < count; i++ ) {
        if ( match_word( tab[i] ) ) {
            return i;
        }
    }
    return -1;
}

/* a register inside [] for 16-bit addressing: returns the base16 index
   by building it from the names, or -1 */
static int addr16_form( void )
{
    if ( match_word( "BX+SI" ) || match_word( "SI+BX" ) ) return 0;
    if ( match_word( "BX+DI" ) || match_word( "DI+BX" ) ) return 1;
    if ( match_word( "BP+SI" ) || match_word( "SI+BP" ) ) return 2;
    if ( match_word( "BP+DI" ) || match_word( "DI+BP" ) ) return 3;
    if ( match_word( "SI" ) ) return 4;
    if ( match_word( "DI" ) ) return 5;
    if ( match_word( "BP" ) ) return 6;
    if ( match_word( "BX" ) ) return 7;
    return -1;
}

/* [...] in 32-bit code: a base, an index with a scale and a displacement
   in any order, as [EBX+ESI*4+10], and the ModRM, SIB and displacement
   the 386 needs for them */
static void parse_mem32( OPND *o )
{
    int base = -1, index = -1, scale = 0;
    u32 disp = 0;
    int have_disp = 0;
    int neg = 0;
    int r;

    lp++;                       /* the [ */
    o->kind = O_MEM;
    for ( ;; ) {
        skip_blanks();
        if ( ( r = find_reg( reg32n, 8 ) ) >= 0 ) {
            skip_blanks();
            if ( neg ) {
                syntax_error();         /* a register cannot be taken away */
            }
            if ( *lp == '*' ) {
                u32 s;
                lp++;
                skip_blanks();
                s = get_hex( 1 );
                scale = s == 1 ? 0 : s == 2 ? 1 : s == 4 ? 2 : s == 8 ? 3 : -1;
                if ( scale < 0 || index >= 0 || r == 4 ) {
                    syntax_error();     /* ESP is never an index */
                }
                index = r;
            } else if ( base < 0 ) {
                base = r;
            } else if ( index < 0 && r != 4 ) {
                index = r;
            } else {
                syntax_error();
            }
        } else {
            u32 v = get_hex( 8 );
            disp += neg ? (u32)( -(s32)v ) : v;
            have_disp = 1;
        }
        skip_blanks();
        if ( *lp != '+' && *lp != '-' ) {
            break;
        }
        neg = ( *lp == '-' );
        lp++;
    }
    if ( *lp != ']' ) {
        syntax_error();
    }
    lp++;

    o->base = base;
    o->index = index;
    o->imm = disp;
    o->mem_has_disp = 1;
    if ( base < 0 && index < 0 ) {      /* [disp32] */
        o->mem_mod = 0;
        o->mem_rm = 5;
        o->disp32 = 1;
        return;
    }
    if ( index >= 0 || base == 4 ) {    /* ESP as a base needs a SIB too */
        o->has_sib = 1;
        o->sib = (u8)( ( scale << 6 ) | ( ( index >= 0 ? index : 4 ) << 3 ) | ( base >= 0 ? base : 5 ) );
        o->mem_rm = 4;
        if ( base < 0 ) {               /* [index*scale+disp32] */
            o->mem_mod = 0;
            o->disp32 = 1;
            return;
        }
    } else {
        o->mem_rm = base;
    }
    if ( !have_disp && base != 5 ) {
        o->mem_mod = 0;
        o->mem_has_disp = 0;
    } else if ( !have_disp || disp < 0x80 || disp >= 0xFFFFFF80UL ) {
        o->mem_mod = 1;                 /* [EBP] takes a disp8 of 0 */
    } else {
        o->mem_mod = 2;
        o->disp32 = 1;
    }
}

static void parse_mem( OPND *o )
{
    int form;
    u32 disp = 0;
    int have_disp = 0;
    int neg = 0;

    o->base = o->index = -1;
    o->has_sib = 0;
    o->disp32 = 0;
    if ( use32 ) {
        parse_mem32( o );
        return;
    }
    lp++;                       /* the [ */
    o->kind = O_MEM;
    form = addr16_form();
    skip_blanks();
    if ( *lp == '+' || *lp == '-' ) {
        neg = ( *lp == '-' );
        lp++;
        disp = get_hex( 8 );
        have_disp = 1;
    } else if ( form < 0 ) {
        disp = get_hex( 8 );
        have_disp = 1;
    }
    skip_blanks();
    if ( *lp != ']' ) {
        syntax_error();
    }
    lp++;
    if ( neg ) {
        disp = (u32)( -(s32)disp );
    }

    if ( form < 0 ) {           /* [disp] direct */
        o->mem_mod = 0;
        o->mem_rm = 6;
        o->imm = disp;
        o->mem_has_disp = 1;
        return;
    }
    o->mem_rm = form;
    o->imm = disp;
    if ( !have_disp ) {
        o->mem_mod = ( form == 6 ) ? 1 : 0;     /* [BP] needs a 0 disp8 */
        o->mem_has_disp = ( form == 6 );
        o->imm = 0;
    } else if ( disp < 0x80 || disp >= 0xFFFFFF80UL ) {
        o->mem_mod = 1;
        o->mem_has_disp = 1;
    } else {
        o->mem_mod = 2;
        o->mem_has_disp = 1;
    }
}

static int parse_operand( OPND *o )
{
    int r;

    o->kind = O_NONE;
    o->size = 0;
    o->is_far = 0;
    skip_blanks();
    if ( at_end() ) {
        return 0;
    }
    if ( match_word( "FAR" ) ) { match_word( "PTR" ); o->is_far = 1; }
    if ( match_word( "BYTE" ) )  { match_word( "PTR" ); o->size = 8; }
    else if ( match_word( "WORD" ) )  { match_word( "PTR" ); o->size = 16; }
    else if ( match_word( "DWORD" ) ) { match_word( "PTR" ); o->size = 32; }
    else if ( match_word( "QWORD" ) ) { match_word( "PTR" ); o->size = 64; }
    else if ( match_word( "TBYTE" ) ) { match_word( "PTR" ); o->size = 80; }

    skip_blanks();
    if ( *lp == '[' ) {
        parse_mem( o );
        return 1;
    }
    if ( o->size || o->is_far ) {
        syntax_error();         /* a size with no memory operand */
    }
    if ( ( r = find_reg( reg32n, 8 ) ) >= 0 && use32 ) { o->kind = O_REG32; o->reg = r; return 1; }
    if ( ( r = find_reg( reg16n, 8 ) ) >= 0 ) { o->kind = O_REG16; o->reg = r; return 1; }
    if ( ( r = find_reg( reg8n, 8 ) ) >= 0 )  { o->kind = O_REG8; o->reg = r; return 1; }
    if ( ( r = find_reg( sregn, use32 ? 6 : 4 ) ) >= 0 ) { o->kind = O_SREG; o->reg = r; return 1; }

    /* a number: an immediate, or seg:off if a colon follows.  A minus
       sign makes a negative immediate, CMP AX,-1 being CMP AX,FFFF. */
    if ( *lp == '-' ) {
        lp++;
        o->kind = O_IMM;
        o->imm = (u32)( -(s32)get_hex( 8 ) );
        return 1;
    }
    {
        u32 v = get_hex( 8 );
        skip_blanks();
        if ( *lp == ':' ) {
            lp++;
            o->kind = O_FAR;
            o->seg = (u16)v;
            o->imm = get_hex( 8 );
            return 1;
        }
        o->kind = O_IMM;
        o->imm = v;
    }
    return 1;
}

/* a memory/register operand's ModRM, with REG as the reg field */
static void emit_modrm( OPND *o, int reg )
{
    if ( o->kind != O_MEM ) {
        emit( (u8)( 0xC0 | ( reg << 3 ) | o->reg ) );
        return;
    }
    emit( (u8)( ( o->mem_mod << 6 ) | ( reg << 3 ) | o->mem_rm ) );
    if ( use32 ) {
        if ( o->has_sib ) {
            emit( o->sib );
        }
        if ( o->mem_mod == 1 ) {
            emit( (u8)o->imm );
        } else if ( o->disp32 ) {
            emit32( o->imm );
        }
        return;
    }
    if ( o->mem_rm == 6 && o->mem_mod == 0 ) {
        emit16( (u16)o->imm );
    } else if ( o->mem_mod == 1 ) {
        emit( (u8)o->imm );
    } else if ( o->mem_mod == 2 ) {
        emit16( (u16)o->imm );
    }
}

static int op_size( OPND *o )
{
    if ( o->kind == O_REG8 ) return 8;
    if ( o->kind == O_REG16 ) return 16;
    if ( o->kind == O_REG32 ) return 32;
    return o->size;
}

/* the arithmetic group, MOV, TEST, XCHG and the like: a reg/mem pair or
   an immediate.  OP0 is the "rm,reg" byte-form opcode base. */
static int asm_arith( const char *mnem, int family, OPND *a, OPND *b )
{
    int sz;

    (void)mnem;
    /* reg,reg or reg,mem or mem,reg */
    if ( ( a->kind == O_MEM || a->kind >= O_REG8 ) && ( b->kind >= O_REG8 && b->kind <= O_REG32 ) ) {
        sz = op_size( b );
        if ( sz == 8 ) emit( (u8)( family * 8 + 0 ) );
        else { if ( sz == 32 && !use32 ) emit( 0x66 ); emit( (u8)( family * 8 + 1 ) ); }
        emit_modrm( a, b->reg );
        return 1;
    }
    if ( a->kind >= O_REG8 && a->kind <= O_REG32 && b->kind == O_MEM ) {
        sz = op_size( a );
        if ( sz == 8 ) emit( (u8)( family * 8 + 2 ) );
        else { if ( sz == 32 && !use32 ) emit( 0x66 ); emit( (u8)( family * 8 + 3 ) ); }
        emit_modrm( b, a->reg );
        return 1;
    }
    /* something,imm */
    if ( b->kind == O_IMM ) {
        sz = op_size( a );
        if ( sz == 0 ) {
            return 0;           /* a size is needed */
        }
        /* AL/AX/EAX,imm short forms */
        if ( a->kind == O_REG8 && a->reg == 0 ) { emit( (u8)( family * 8 + 4 ) ); emit( (u8)b->imm ); return 1; }
        if ( ( a->kind == O_REG16 || a->kind == O_REG32 ) && a->reg == 0 ) {
            if ( sz == 32 && !use32 ) emit( 0x66 );
            emit( (u8)( family * 8 + 5 ) );
            if ( sz == 32 ) emit32( b->imm ); else emit16( (u16)b->imm );
            return 1;
        }
        if ( sz == 8 ) {
            emit( 0x80 ); emit_modrm( a, family ); emit( (u8)b->imm );
        } else {
            /* a value a signed byte holds takes 83h, as 6.22 wrote SBB
               WORD PTR [BX],1 (83 1F 01) */
            u32 v = sz == 32 ? b->imm : b->imm & 0xFFFF;
            u32 top = sz == 32 ? 0xFFFFFF80UL : 0xFF80UL;
            if ( sz == 32 && !use32 ) emit( 0x66 );
            if ( v < 0x80 || v >= top ) {
                emit( 0x83 ); emit_modrm( a, family ); emit( (u8)v );
            } else {
                emit( 0x81 ); emit_modrm( a, family );
                if ( sz == 32 ) emit32( b->imm ); else emit16( (u16)b->imm );
            }
        }
        return 1;
    }
    return 0;
}

static const char *grp1[] = { "ADD","OR","ADC","SBB","AND","SUB","XOR","CMP" };

/* a short/near relative target */
static void emit_jcc( int cc, u32 target )
{
    s32 rel = (s32)( target - ( asm_at.off + clen + 2 ) );

    if ( rel >= -128 && rel < 128 ) {
        emit( (u8)( 0x70 + cc ) );
        emit( (u8)rel );
    } else {
        emit( 0x0F );
        emit( (u8)( 0x80 + cc ) );
        rel = (s32)( target - ( asm_at.off + clen + ( use32 ? 4 : 2 ) ) );
        if ( use32 ) emit32( (u32)rel ); else emit16( (u16)rel );
    }
}

static const char *ccn[] = {
    "JO","JNO","JB","JNB","JZ","JNZ","JBE","JA","JS","JNS","JPE","JPO","JL","JGE","JLE","JG"
};
static const char *ccalt[] = {
    "JO","JNO","JC","JNC","JE","JNE","JNA","JNBE","JS","JNS","JP","JNP","JNGE","JNL","JNG","JNLE"
};

static int which_jcc( const char *m )
{
    int i;
    for ( i = 0; i < 16; i++ ) {
        if ( str_icmp( m, ccn[i] ) == 0 || str_icmp( m, ccalt[i] ) == 0 ) {
            return i;
        }
    }
    return -1;
}

/* the text as the disassembler writes it: upper case, no blanks */
static void squeeze( const char *src, char *dst, u32 max )
{
    u32 n = 0;

    for ( ; *src && n + 1 < max; src++ ) {
        if ( *src != ' ' && *src != '\t' ) {
            dst[n++] = (char)ch_upper( *src );
        }
    }
    dst[n] = 0;
}

/* An 8087 or 387 instruction.  Its encoding is the one whose disassembly
   reads as the line does, found by trying them all (D8h to DFh, each
   reg field or register ModRM), so that A and U cannot disagree.  M is
   the mnemonic and LP the rest of the line; what has been emitted
   already (a prefix) stays in front. */
static int asm_fpu( const char *m )
{
    char   want[64], got[80];
    u8     one[DIS_MAX];        /* the encoding being tried, on its own */
    DISASM dis;
    int    base = clen;
    int    op, i;
    u32    n;

    skip_blanks();
    for ( i = 0; lp[i] && lp[i] != '[' && lp[i] != '\r'; i++ ) {
        ;
    }
    if ( lp[i] == '[' ) {
        /* a memory operand: MNEMONIC and its size words must match the
           disassembly up to the [ */
        OPND o;
        char head[64];
        int  reg;
        u32  take = (u32)i;

        n = str_len( m );
        if ( take > sizeof( got ) - n - 1 ) {
            take = sizeof( got ) - n - 1;
        }
        mem_cpy( got, m, n );
        mem_cpy( got + n, lp, take );
        got[n + take] = 0;
        squeeze( got, head, sizeof( head ) );
        if ( !parse_operand( &o ) || o.kind != O_MEM ) {
            return 0;
        }
        for ( op = 0xD8; op <= 0xDF; op++ ) {
            for ( reg = 0; reg < 8; reg++ ) {
                char *br;
                clen = base;
                emit( (u8)op );
                emit_modrm( &o, reg );
                mem_set( one, 0, sizeof( one ) );
                mem_cpy( one, code + base, (u32)( clen - base ) );
                disasm( asm_at, one, use32, &dis );
                squeeze( dis.text, got, sizeof( got ) );
                br = got;
                while ( *br && *br != '[' ) {
                    br++;
                }
                *br = 0;
                if ( dis.len == (u32)( clen - base ) && str_icmp( got, head ) == 0 ) {
                    return 1;
                }
            }
        }
        clen = base;
        return 0;
    }

    /* a register form, or none: every register ModRM.  With nothing
       after the mnemonic, ST(1) is tried as well (FCOMPP is DE D9, which
       reads FCOMPP ST(1)). */
    n = str_len( m );
    mem_cpy( got, m, n );
    for ( i = 0; lp[i] && lp[i] != '\r' && n + 1 < sizeof( got ); i++ ) {
        got[n++] = lp[i];
    }
    got[n] = 0;
    lp += i;
    squeeze( got, want, sizeof( want ) - 6 );
    for ( i = 0; i < 2; i++ ) {
        int modrm;
        if ( i == 1 ) {
            if ( str_icmp( want, m ) != 0 ) {
                break;
            }
            mem_cpy( want + str_len( want ), "ST(1)", 6 );
        }
        for ( op = 0xD8; op <= 0xDF; op++ ) {
            for ( modrm = 0xC0; modrm <= 0xFF; modrm++ ) {
                mem_set( one, 0, sizeof( one ) );
                one[0] = (u8)op;
                one[1] = (u8)modrm;
                disasm( asm_at, one, use32, &dis );
                squeeze( dis.text, got, sizeof( got ) );
                if ( dis.len == 2 && str_icmp( got, want ) == 0 ) {
                    clen = base;
                    emit( (u8)op );
                    emit( (u8)modrm );
                    return 1;
                }
            }
        }
    }
    clen = base;
    return 0;
}

static int assemble_insn( void );

/* assemble one line into code[]/clen; 0 if it cannot */
static int assemble_line( void )
{
    clen = 0;
    return assemble_insn();
}

static int assemble_insn( void )
{
    char m[8];
    int  mlen = 0;
    OPND a, b;
    int  na, nb, i;

    skip_blanks();
    while ( mlen < 7 && ( ( *lp >= 'A' && *lp <= 'Z' ) || ( *lp >= 'a' && *lp <= 'z' ) || ( *lp >= '0' && *lp <= '9' ) ) ) {
        m[mlen++] = (char)ch_upper( *lp++ );
    }
    m[mlen] = 0;
    if ( mlen == 0 ) {
        return 0;
    }

    /* a prefix, on a line of its own or in front of its instruction:
       REP MOVSB, LOCK, ES: */
    {
        static const struct { const char *n; u8 b; } pre[] = {
            {"REP",0xF3},{"REPE",0xF3},{"REPZ",0xF3},{"REPNE",0xF2},{"REPNZ",0xF2},
            {"LOCK",0xF0},{NULL,0}
        };
        static const u8 segpre[] = { 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65 };

        for ( i = 0; pre[i].n; i++ ) {
            if ( str_icmp( m, pre[i].n ) == 0 ) {
                emit( pre[i].b );
                return at_end() ? 1 : assemble_insn();
            }
        }
        if ( mlen == 2 && *lp == ':' ) {
            for ( i = 0; i < ( use32 ? 6 : 4 ); i++ ) {
                if ( str_icmp( m, sregn[i] ) == 0 ) {
                    lp++;
                    emit( segpre[i] );
                    return at_end() ? 1 : assemble_insn();
                }
            }
        }
    }

    /* RET n and RETF n, before the RET of the table below */
    if ( ( str_icmp( m, "RET" ) == 0 || str_icmp( m, "RETF" ) == 0 ) && !at_end() ) {
        emit( m[3] ? 0xCA : 0xC2 );
        emit16( (u16)get_hex( 4 ) );
        return 1;
    }

    /* AAM and AAD, base 10 unless one is given */
    if ( str_icmp( m, "AAM" ) == 0 || str_icmp( m, "AAD" ) == 0 ) {
        emit( m[2] == 'M' ? 0xD4 : 0xD5 );
        emit( at_end() ? 0x0A : (u8)get_hex( 2 ) );
        return 1;
    }

    /* DB / DW */
    if ( str_icmp( m, "DB" ) == 0 ) {
        clen = (int)get_list( code, DIS_MAX );
        return 1;
    }
    if ( str_icmp( m, "DW" ) == 0 ) {
        do {
            emit16( (u16)get_hex( 4 ) );
            skip_blanks();
        } while ( !at_end() );
        return 1;
    }

    /* the arithmetic group */
    for ( i = 0; i < 8; i++ ) {
        if ( str_icmp( m, grp1[i] ) == 0 ) {
            na = parse_operand( &a );
            skip_blanks();
            if ( *lp == ',' ) lp++;
            nb = parse_operand( &b );
            if ( !na || !nb ) return 0;
            return asm_arith( m, i, &a, &b );
        }
    }

    /* MOV, TEST, XCHG, LEA, LES, LDS reuse the two-operand shapes */
    if ( str_icmp( m, "MOV" ) == 0 ) {
        na = parse_operand( &a );
        skip_blanks(); if ( *lp == ',' ) lp++;
        nb = parse_operand( &b );
        if ( !na || !nb ) return 0;
        if ( a.kind == O_SREG && ( b.kind == O_MEM || b.kind == O_REG16 ) ) {
            emit( 0x8E ); emit_modrm( &b, a.reg ); return 1;
        }
        if ( b.kind == O_SREG && ( a.kind == O_MEM || a.kind == O_REG16 ) ) {
            emit( 0x8C ); emit_modrm( &a, b.reg ); return 1;
        }
        if ( ( a.kind >= O_REG8 && a.kind <= O_REG32 ) && b.kind == O_IMM ) {
            int sz = op_size( &a );
            if ( sz == 8 ) { emit( (u8)( 0xB0 + a.reg ) ); emit( (u8)b.imm ); return 1; }
            if ( sz == 32 && !use32 ) emit( 0x66 );
            emit( (u8)( 0xB8 + a.reg ) );
            if ( sz == 32 ) emit32( b.imm ); else emit16( (u16)b.imm );
            return 1;
        }
        if ( a.kind == O_MEM && b.kind == O_IMM ) {
            int sz = a.size;
            if ( sz == 0 ) return 0;
            if ( sz == 8 ) { emit( 0xC6 ); emit_modrm( &a, 0 ); emit( (u8)b.imm ); return 1; }
            if ( sz == 32 && !use32 ) emit( 0x66 );
            emit( 0xC7 ); emit_modrm( &a, 0 );
            if ( sz == 32 ) emit32( b.imm ); else emit16( (u16)b.imm );
            return 1;
        }
        /* AL or AX and a plain address: A0h-A3h, as 6.22 wrote MOV AX,[1234]
           (A1 34 12) */
        {
            OPND *r = NULL, *mem = NULL;
            int to_mem = 0;
            if ( a.kind >= O_REG8 && a.kind <= O_REG32 && a.reg == 0 && b.kind == O_MEM ) { r = &a; mem = &b; }
            if ( b.kind >= O_REG8 && b.kind <= O_REG32 && b.reg == 0 && a.kind == O_MEM ) { r = &b; mem = &a; to_mem = 1; }
            if ( r && mem->mem_mod == 0 && !mem->has_sib && mem->mem_rm == ( use32 ? 5 : 6 ) ) {
                int sz = op_size( r );
                if ( sz == 32 && !use32 ) emit( 0x66 );
                emit( (u8)( 0xA0 + ( to_mem ? 2 : 0 ) + ( sz == 8 ? 0 : 1 ) ) );
                if ( use32 ) emit32( mem->imm ); else emit16( (u16)mem->imm );
                return 1;
            }
        }
        if ( ( a.kind == O_MEM || ( a.kind >= O_REG8 && a.kind <= O_REG32 ) ) && ( b.kind >= O_REG8 && b.kind <= O_REG32 ) ) {
            int sz = op_size( &b );
            if ( sz == 8 ) emit( 0x88 ); else { if ( sz == 32 && !use32 ) emit( 0x66 ); emit( 0x89 ); }
            emit_modrm( &a, b.reg ); return 1;
        }
        if ( ( a.kind >= O_REG8 && a.kind <= O_REG32 ) && b.kind == O_MEM ) {
            int sz = op_size( &a );
            if ( sz == 8 ) emit( 0x8A ); else { if ( sz == 32 && !use32 ) emit( 0x66 ); emit( 0x8B ); }
            emit_modrm( &b, a.reg ); return 1;
        }
        return 0;
    }

    /* INT, INT3 */
    if ( str_icmp( m, "INT" ) == 0 ) {
        u32 n = get_hex( 2 );
        if ( n == 3 ) emit( 0xCC );
        else { emit( 0xCD ); emit( (u8)n ); }
        return 1;
    }
    if ( str_icmp( m, "INTO" ) == 0 ) { emit( 0xCE ); return 1; }
    if ( str_icmp( m, "IRET" ) == 0 ) { emit( 0xCF ); return 1; }

    /* the zero-operand instructions */
    {
        static const struct { const char *n; u8 b; } simple[] = {
            {"NOP",0x90},{"RET",0xC3},{"RETF",0xCB},{"CLC",0xF8},{"STC",0xF9},
            {"CLI",0xFA},{"STI",0xFB},{"CLD",0xFC},{"STD",0xFD},{"CMC",0xF5},
            {"HLT",0xF4},{"WAIT",0x9B},{"FWAIT",0x9B},{"PUSHF",0x9C},{"POPF",0x9D},
            {"SAHF",0x9E},{"LAHF",0x9F},{"CBW",0x98},{"CWD",0x99},{"XLAT",0xD7},
            {"MOVSB",0xA4},{"MOVSW",0xA5},{"CMPSB",0xA6},{"CMPSW",0xA7},
            {"STOSB",0xAA},{"STOSW",0xAB},{"LODSB",0xAC},{"LODSW",0xAD},
            {"SCASB",0xAE},{"SCASW",0xAF},{"DAA",0x27},{"DAS",0x2F},
            {"AAA",0x37},{"AAS",0x3F},{NULL,0}
        };
        for ( i = 0; simple[i].n; i++ ) {
            if ( str_icmp( m, simple[i].n ) == 0 ) { emit( simple[i].b ); return 1; }
        }
    }

    /* PUSH / POP */
    if ( str_icmp( m, "PUSH" ) == 0 || str_icmp( m, "POP" ) == 0 ) {
        int push = str_icmp( m, "PUSH" ) == 0;
        if ( !parse_operand( &a ) ) return 0;
        if ( a.kind == O_REG16 || a.kind == O_REG32 ) {
            if ( a.kind == O_REG32 && !use32 ) emit( 0x66 );
            emit( (u8)( ( push ? 0x50 : 0x58 ) + a.reg ) ); return 1;
        }
        if ( a.kind == O_SREG ) {
            static const u8 ps[] = { 0x06,0x0E,0x16,0x1E };
            static const u8 po[] = { 0x07,0x00,0x17,0x1F };
            if ( a.reg < 4 ) { emit( push ? ps[a.reg] : po[a.reg] ); return 1; }
            emit( 0x0F ); emit( push ? ( a.reg == 4 ? 0xA0 : 0xA8 ) : ( a.reg == 4 ? 0xA1 : 0xA9 ) ); return 1;
        }
        if ( a.kind == O_MEM ) {
            if ( push ) { emit( 0xFF ); emit_modrm( &a, 6 ); } else { emit( 0x8F ); emit_modrm( &a, 0 ); }
            return 1;
        }
        return 0;
    }

    /* INC / DEC */
    if ( str_icmp( m, "INC" ) == 0 || str_icmp( m, "DEC" ) == 0 ) {
        int dec = str_icmp( m, "DEC" ) == 0;
        if ( !parse_operand( &a ) ) return 0;
        if ( a.kind == O_REG16 || a.kind == O_REG32 ) {
            if ( a.kind == O_REG32 && !use32 ) emit( 0x66 );
            emit( (u8)( ( dec ? 0x48 : 0x40 ) + a.reg ) ); return 1;
        }
        {
            int sz = op_size( &a );
            if ( sz == 0 ) return 0;
            if ( sz == 8 ) { emit( 0xFE ); emit_modrm( &a, dec ? 1 : 0 ); }
            else { if ( sz == 32 && !use32 ) emit( 0x66 ); emit( 0xFF ); emit_modrm( &a, dec ? 1 : 0 ); }
            return 1;
        }
    }

    /* the string/flag already handled; JMP and CALL */
    if ( str_icmp( m, "JMP" ) == 0 || str_icmp( m, "CALL" ) == 0 ) {
        int call = str_icmp( m, "CALL" ) == 0;
        if ( !parse_operand( &a ) ) return 0;
        if ( a.kind == O_IMM ) {
            s32 rel = (s32)( a.imm - ( asm_at.off + clen + 2 ) );
            if ( !call && rel >= -128 && rel < 128 ) { emit( 0xEB ); emit( (u8)rel ); return 1; }
            emit( call ? 0xE8 : 0xE9 );
            rel = (s32)( a.imm - ( asm_at.off + clen + ( use32 ? 4 : 2 ) ) );
            if ( use32 ) emit32( (u32)rel ); else emit16( (u16)rel );
            return 1;
        }
        if ( a.kind == O_FAR ) {
            emit( call ? 0x9A : 0xEA );
            if ( use32 ) emit32( a.imm ); else emit16( (u16)a.imm );
            emit16( a.seg );
            return 1;
        }
        if ( a.kind == O_MEM && a.is_far ) {           /* JMP FAR [BX]: FF /5 */
            emit( 0xFF ); emit_modrm( &a, call ? 3 : 5 ); return 1;
        }
        if ( a.kind == O_MEM || a.kind == O_REG16 || a.kind == O_REG32 ) {
            emit( 0xFF ); emit_modrm( &a, call ? 2 : 4 ); return 1;
        }
        return 0;
    }

    /* Jcc, LOOP, JCXZ */
    {
        int cc = which_jcc( m );
        if ( cc >= 0 ) {
            if ( !parse_operand( &a ) || a.kind != O_IMM ) return 0;
            emit_jcc( cc, a.imm );
            return 1;
        }
    }
    if ( str_icmp( m, "LOOP" ) == 0 || str_icmp( m, "LOOPZ" ) == 0 ||
         str_icmp( m, "LOOPNZ" ) == 0 || str_icmp( m, "JCXZ" ) == 0 ) {
        u8 b = str_icmp( m, "LOOPNZ" ) == 0 ? 0xE0 : str_icmp( m, "LOOPZ" ) == 0 ? 0xE1 :
               str_icmp( m, "LOOP" ) == 0 ? 0xE2 : 0xE3;
        s32 rel;
        if ( !parse_operand( &a ) || a.kind != O_IMM ) return 0;
        rel = (s32)( a.imm - ( asm_at.off + clen + 2 ) );
        if ( rel < -128 || rel >= 128 ) return 0;
        emit( b ); emit( (u8)rel ); return 1;
    }

    /* IN / OUT */
    if ( str_icmp( m, "IN" ) == 0 ) {
        if ( !parse_operand( &a ) ) return 0;
        skip_blanks(); if ( *lp == ',' ) lp++;
        if ( !parse_operand( &b ) ) return 0;
        if ( b.kind == O_IMM ) { emit( a.kind == O_REG8 ? 0xE4 : 0xE5 ); emit( (u8)b.imm ); return 1; }
        emit( a.kind == O_REG8 ? 0xEC : 0xED ); return 1;
    }
    if ( str_icmp( m, "OUT" ) == 0 ) {
        if ( !parse_operand( &a ) ) return 0;
        skip_blanks(); if ( *lp == ',' ) lp++;
        if ( !parse_operand( &b ) ) return 0;
        if ( a.kind == O_IMM ) { emit( b.kind == O_REG8 ? 0xE6 : 0xE7 ); emit( (u8)a.imm ); return 1; }
        emit( b.kind == O_REG8 ? 0xEE : 0xEF ); return 1;
    }

    /* INT already; TEST, XCHG */
    if ( str_icmp( m, "TEST" ) == 0 ) {
        na = parse_operand( &a ); skip_blanks(); if ( *lp == ',' ) lp++; nb = parse_operand( &b );
        if ( !na || !nb ) return 0;
        if ( b.kind >= O_REG8 && b.kind <= O_REG32 ) {
            int sz = op_size( &b );
            if ( sz == 8 ) emit( 0x84 ); else { if ( sz == 32 && !use32 ) emit( 0x66 ); emit( 0x85 ); }
            emit_modrm( &a, b.reg ); return 1;
        }
        if ( b.kind == O_IMM ) {
            int sz = op_size( &a );
            if ( sz == 0 ) return 0;
            if ( a.kind == O_REG8 && a.reg == 0 ) { emit( 0xA8 ); emit( (u8)b.imm ); return 1; }
            if ( ( a.kind == O_REG16 || a.kind == O_REG32 ) && a.reg == 0 ) {
                if ( sz == 32 && !use32 ) emit( 0x66 ); emit( 0xA9 );
                if ( sz == 32 ) emit32( b.imm ); else emit16( (u16)b.imm ); return 1;
            }
            if ( sz == 8 ) { emit( 0xF6 ); emit_modrm( &a, 0 ); emit( (u8)b.imm ); }
            else { if ( sz == 32 && !use32 ) emit( 0x66 ); emit( 0xF7 ); emit_modrm( &a, 0 );
                   if ( sz == 32 ) emit32( b.imm ); else emit16( (u16)b.imm ); }
            return 1;
        }
        return 0;
    }
    if ( str_icmp( m, "XCHG" ) == 0 ) {
        /* 86h/87h every time, with a register in the reg field - the
           first one when both are: 6.22 wrote XCHG AX,BX as 87 C3 */
        OPND *r, *rm;
        int sz;
        na = parse_operand( &a ); skip_blanks(); if ( *lp == ',' ) lp++; nb = parse_operand( &b );
        if ( !na || !nb ) return 0;
        if ( a.kind >= O_REG8 && a.kind <= O_REG32 && ( b.kind == O_MEM || ( b.kind >= O_REG8 && b.kind <= O_REG32 ) ) ) {
            r = &a; rm = &b;
        } else if ( a.kind == O_MEM && b.kind >= O_REG8 && b.kind <= O_REG32 ) {
            r = &b; rm = &a;
        } else {
            return 0;
        }
        sz = op_size( r );
        if ( rm->kind != O_MEM && op_size( rm ) != sz ) return 0;
        if ( sz == 32 && !use32 ) emit( 0x66 );
        emit( sz == 8 ? 0x86 : 0x87 ); emit_modrm( rm, r->reg ); return 1;
    }

    /* LEA, LES, LDS */
    if ( str_icmp( m, "LEA" ) == 0 || str_icmp( m, "LES" ) == 0 || str_icmp( m, "LDS" ) == 0 ) {
        na = parse_operand( &a ); skip_blanks(); if ( *lp == ',' ) lp++; nb = parse_operand( &b );
        if ( !na || !nb || !( a.kind == O_REG16 || a.kind == O_REG32 ) || b.kind != O_MEM ) return 0;
        emit( str_icmp( m, "LEA" ) == 0 ? 0x8D : str_icmp( m, "LES" ) == 0 ? 0xC4 : 0xC5 );
        emit_modrm( &b, a.reg ); return 1;
    }

    /* NOT, NEG, MUL, IMUL, DIV, IDIV, and the shifts */
    {
        static const struct { const char *n; int reg; } g3[] = {
            {"NOT",2},{"NEG",3},{"MUL",4},{"IMUL",5},{"DIV",6},{"IDIV",7},{NULL,0}
        };
        for ( i = 0; g3[i].n; i++ ) {
            if ( str_icmp( m, g3[i].n ) == 0 ) {
                int sz;
                if ( !parse_operand( &a ) ) return 0;
                sz = op_size( &a );
                if ( sz == 0 ) return 0;
                if ( sz == 8 ) emit( 0xF6 ); else { if ( sz == 32 && !use32 ) emit( 0x66 ); emit( 0xF7 ); }
                emit_modrm( &a, g3[i].reg );
                return 1;
            }
        }
    }
    {
        static const struct { const char *n; int reg; } g2[] = {
            {"ROL",0},{"ROR",1},{"RCL",2},{"RCR",3},{"SHL",4},{"SHR",5},{"SAR",7},{NULL,0}
        };
        for ( i = 0; g2[i].n; i++ ) {
            if ( str_icmp( m, g2[i].n ) == 0 ) {
                int sz;
                na = parse_operand( &a ); skip_blanks(); if ( *lp == ',' ) lp++; nb = parse_operand( &b );
                if ( !na || !nb ) return 0;
                sz = op_size( &a );
                if ( sz == 0 ) return 0;
                if ( b.kind == O_IMM && b.imm == 1 ) {
                    if ( sz == 8 ) emit( 0xD0 ); else { if ( sz == 32 && !use32 ) emit( 0x66 ); emit( 0xD1 ); }
                    emit_modrm( &a, g2[i].reg ); return 1;
                }
                if ( b.kind == O_REG8 && b.reg == 1 ) {     /* CL */
                    if ( sz == 8 ) emit( 0xD2 ); else { if ( sz == 32 && !use32 ) emit( 0x66 ); emit( 0xD3 ); }
                    emit_modrm( &a, g2[i].reg ); return 1;
                }
                return 0;
            }
        }
    }

    /* everything else beginning with F is the 8087's */
    if ( m[0] == 'F' ) {
        return asm_fpu( m );
    }
    return 0;
}

void cmd_assemble( void )
{
    static char buf[80];
    ADDR addr;
    u32  jb[3];

    use32 = ( mode == MODE_NAT );
    if ( at_end() ) {
        addr = next_asm;                /* where the last A stopped */
    } else {
        addr.seg = cpu.cs;
        get_addr( &addr, cpu.cs );
        need_end();
    }

    /* A reads its own lines, and a bad one re-prompts at the same
       address instead of ending A: the parser's errors come back here
       rather than to the command loop (MAIN.C, syntax_error). */
    for ( ;; ) {
        next_asm = addr;
        out_addr( addr );
        out_ch( ' ' );
        out_flush();
        if ( sys_read_line( buf, sizeof( buf ) ) < 0 ) {
            error_jmp = NULL;
            quit( 0 );
        }
        line = buf;
        lp = buf;
        prompt_len = addr_width( addr ) + 1;    /* the address and a blank */
        skip_blanks();
        if ( at_end() ) {
            error_jmp = NULL;
            return;
        }
        asm_at = addr;
        if ( dbg_setjmp( jb ) == 0 ) {
            error_jmp = jb;
            if ( !assemble_line() || clen == 0 ) {
                lp = buf;
                skip_blanks();
                syntax_error();
            }
            need_end();
            /* in the first megabyte the code goes round the segment, as
               6.22's did; a slot ends where it ends */
            if ( !mem_is_low( addr ) && ( addr.off > mem_seg_end( addr ) || (u32)clen > mem_seg_end( addr ) - addr.off ) ) {
                lp = buf;
                syntax_error();
            }
            if ( mem_write( addr, code, clen ) ) {
                coded_error( "df" );    /* and the same address again */
            }
            addr_add( &addr, (u32)clen );
        }
        error_jmp = NULL;
    }
}
