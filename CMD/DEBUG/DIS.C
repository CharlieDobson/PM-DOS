/*
 * DIS.C - the disassembler, and U.
 *
 * It decodes one 8086/80386 instruction into DISASM: the bytes it took,
 * the text as 6.22's DEBUG printed it (mnemonic in capitals, padded to
 * a tab stop, then the operands), the memory operand it touches (for
 * R's "DS:xxxx=yy" line), and a kind for P to know a CALL from a jump.
 *
 * The text is measured against 6.22's own, by feeding it every opcode
 * and reading what it said: the group mnemonics, the "+78" of an 83
 * sign-extended byte, "BYTE PTR"/"WORD PTR" only where the size is not
 * otherwise plain, "FAR" on the far indirect calls, "???" for an
 * encoding it does not name.  A native (32-bit) program is decoded with
 * the operand and address size reversed, so EAX and [EBX+ESI] come out
 * where a 16-bit area shows AX and [BX+SI].
 */
#include "debug.h"

typedef struct {
    const u8 *p;            /* the bytes */
    ADDR      at;           /* where they are */
    u32       pos;          /* how many used */
    int       opsize;       /* 16 or 32: operand size now */
    int       adsize;       /* 16 or 32: address size now */
    int       seg;          /* a segment-override register, or -1 */
    int       rep;          /* 0, 0xF2, 0xF3 */
    int       use32;        /* a native program: 386 decoding */
    int       x87;          /* 16-bit: an 8087 instruction 6.22 names */
    DISASM   *out;
} DEC;

static const char *r8[]  = { "AL","CL","DL","BL","AH","CH","DH","BH" };
static const char *r16[] = { "AX","CX","DX","BX","SP","BP","SI","DI" };
static const char *r32[] = { "EAX","ECX","EDX","EBX","ESP","EBP","ESI","EDI" };
static const char *sreg[]= { "ES","CS","SS","DS","FS","GS","??","??" };
static const char *base16[] = { "BX+SI","BX+DI","BP+SI","BP+DI","SI","DI","BP","BX" };

static void decode_fpu( DEC *d, u8 op );
static void decode_0f( DEC *d );

/* a general register's current value, for an effective address */
static u32 reg_val( int which )
{
    switch ( which ) {
    case 0: return cpu.eax;
    case 1: return cpu.ecx;
    case 2: return cpu.edx;
    case 3: return cpu.ebx;
    case 4: return cpu.esp;
    case 5: return cpu.ebp;
    case 6: return cpu.esi;
    }
    return cpu.edi;
}

static u8 fetch( DEC *d )
{
    return d->p[d->pos++];
}

static u16 fetch16( DEC *d )
{
    u16 v = (u16)( d->p[d->pos] | ( d->p[d->pos + 1] << 8 ) );
    d->pos += 2;
    return v;
}

static u32 fetch32( DEC *d )
{
    u32 v = d->p[d->pos] | ( (u32)d->p[d->pos+1] << 8 ) |
            ( (u32)d->p[d->pos+2] << 16 ) | ( (u32)d->p[d->pos+3] << 24 );
    d->pos += 4;
    return v;
}

/* ------------------------------------------------------------------ */
/* building the text                                                   */
/* ------------------------------------------------------------------ */

static char  mnem[16];
static char  ops[64];
static int   oplen;

static void op_str( const char *s )
{
    while ( *s && oplen < (int)sizeof( ops ) - 1 ) {
        ops[oplen++] = *s++;
    }
    ops[oplen] = 0;
}

static void op_hex( u32 val, int digits )
{
    int shift;

    for ( shift = ( digits - 1 ) * 4; shift >= 0; shift -= 4 ) {
        if ( oplen < (int)sizeof( ops ) - 1 ) {
            ops[oplen++] = "0123456789ABCDEF"[( val >> shift ) & 15];
        }
    }
    ops[oplen] = 0;
}

static void set_mnem( const char *s )
{
    int i = 0;
    while ( s[i] && i < 15 ) {
        mnem[i] = s[i];
        i++;
    }
    mnem[i] = 0;
}

/* ------------------------------------------------------------------ */
/* ModRM                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int reg;                /* the reg field */
    int is_mem;
    int rm;                 /* mod==11: the register */
    int seg;                /* a memory operand's segment register */
    u32 off;                /* ...its displacement or address */
    int has_off;            /* ...there is a usable effective offset */
} MODRM;

static int default_seg( int rm16, int mod )
{
    /* BP-based addressing defaults to SS */
    if ( rm16 == 2 || rm16 == 3 ) return 2;         /* [BP+SI],[BP+DI] -> SS */
    if ( rm16 == 6 && mod != 0 ) return 2;          /* [BP+disp] -> SS */
    return 3;                                        /* else DS */
}

/* A memory operand's "[", with a native instruction's segment override
   in front of it - "ES:[EBX]".  16-bit code shows an override as a line
   of its own, as 6.22 did, so there is none here. */
static void open_mem( DEC *d )
{
    if ( d->use32 && d->seg >= 0 ) {
        op_str( sreg[d->seg] );
        op_str( ":" );
    }
    op_str( "[" );
}

static void decode_modrm( DEC *d, MODRM *m, int size )
{
    u8  modrm = fetch( d );
    int mod = modrm >> 6;
    int rm = modrm & 7;

    m->reg = ( modrm >> 3 ) & 7;
    m->has_off = 0;
    m->seg = -1;

    if ( mod == 3 ) {
        m->is_mem = 0;
        m->rm = rm;
        return;
    }
    m->is_mem = 1;

    if ( d->adsize == 32 ) {
        int base = -1, index = -1, scale = 0;
        u32 disp = 0;
        if ( rm == 4 ) {                /* SIB */
            u8 sib = fetch( d );
            scale = sib >> 6;
            index = ( sib >> 3 ) & 7;
            base = sib & 7;
            if ( index == 4 ) index = -1;
            if ( base == 5 && mod == 0 ) {
                base = -1;
                disp = fetch32( d );
            }
        } else if ( rm == 5 && mod == 0 ) {
            disp = fetch32( d );
            m->off = disp;
            m->has_off = 1;
            m->seg = d->seg >= 0 ? d->seg : 3;
            m->rm = -1;
            m->off = disp;
            goto disp_done;
        } else {
            base = rm;
        }
        if ( mod == 1 ) disp = (u32)(s32)(s8)fetch( d );
        else if ( mod == 2 ) disp = fetch32( d );
        m->seg = d->seg >= 0 ? d->seg : ( base == 5 || base == 4 ? 2 : 3 );
        m->off = disp + ( base >= 0 ? reg_val( base ) : 0 ) +
                 ( index >= 0 ? ( reg_val( index ) << scale ) : 0 );
        m->has_off = 1;
        /* the text */
        open_mem( d );
        {
            int first = 1;
            if ( base >= 0 ) { op_str( r32[base] ); first = 0; }
            if ( index >= 0 ) {
                if ( !first ) op_str( "+" );
                op_str( r32[index] );
                if ( scale ) { op_str( "*" ); op_hex( 1 << scale, 1 ); }
                first = 0;
            }
            if ( mod == 1 && !first && (s32)disp < 0 ) {
                op_str( "-" );                  /* signed, as in 16-bit code */
                op_hex( (u32)( -(s32)disp ), 2 );
            } else if ( mod == 1 || mod == 2 || first || base < 0 ) {
                if ( !first ) op_str( "+" );
                op_hex( mod == 1 ? disp & 0xFF : disp, mod == 1 ? 2 : 8 );
            }
        }
        op_str( "]" );
        m->rm = -2;                     /* text already placed */
        return;
disp_done:
        open_mem( d );
        op_hex( disp, 8 );
        op_str( "]" );
        m->rm = -2;
        return;
    }

    /* 16-bit addressing */
    if ( mod == 0 && rm == 6 ) {
        m->off = fetch16( d );
        m->has_off = 1;
        m->seg = d->seg >= 0 ? d->seg : 3;
        open_mem( d );
        op_hex( m->off, 4 );
        op_str( "]" );
        m->rm = -2;
        return;
    }
    m->seg = d->seg >= 0 ? d->seg : default_seg( rm, mod );
    open_mem( d );
    op_str( base16[rm] );
    {
        /* the effective offset, from the registers as they are now, so
           R and U can show "seg:off=value" as 6.22 does */
        u32 ea = 0;
        switch ( rm ) {
        case 0: ea = reg_val( 3 ) + reg_val( 6 ); break;    /* BX+SI */
        case 1: ea = reg_val( 3 ) + reg_val( 7 ); break;    /* BX+DI */
        case 2: ea = reg_val( 5 ) + reg_val( 6 ); break;    /* BP+SI */
        case 3: ea = reg_val( 5 ) + reg_val( 7 ); break;    /* BP+DI */
        case 4: ea = reg_val( 6 ); break;                   /* SI */
        case 5: ea = reg_val( 7 ); break;                   /* DI */
        case 6: ea = reg_val( 5 ); break;                   /* BP */
        case 7: ea = reg_val( 3 ); break;                   /* BX */
        }
        if ( mod == 1 ) {
            u8 b = fetch( d );
            /* a byte displacement is signed: [BP+DI-02], as 6.22 shows */
            if ( b >= 0x80 ) {
                op_str( "-" );
                op_hex( 0x100 - b, 2 );
            } else {
                op_str( "+" );
                op_hex( b, 2 );
            }
            ea += (u32)(s32)(s8)b;
        } else if ( mod == 2 ) {
            u16 w = fetch16( d );
            op_str( "+" );
            op_hex( w, 4 );
            ea += (u32)(s32)(s16)w;
        }
        m->off = ea & 0xFFFF;
        m->has_off = 1;
    }
    op_str( "]" );
    m->rm = -2;
    (void)size;
}

/* a register operand of SIZE bits */
static const char *reg_name( int which, int size )
{
    if ( size == 8 ) return r8[which];
    if ( size == 32 ) return r32[which];
    return r16[which];
}

/* ------------------------------------------------------------------ */
/* the decode                                                          */
/* ------------------------------------------------------------------ */

static const char *grp1[] = { "ADD","OR","ADC","SBB","AND","SUB","XOR","CMP" };
static const char *grp2[] = { "ROL","ROR","RCL","RCR","SHL","SHR","???","SAR" };
static const char *grp3[] = { "TEST","???","NOT","NEG","MUL","IMUL","DIV","IDIV" };
static const char *jcc[]  = { "JO","JNO","JB","JNB","JZ","JNZ","JBE","JA",
                              "JS","JNS","JPE","JPO","JL","JGE","JLE","JG" };

/* record a memory operand for R's "seg:off=val" line */
static void note_mem( DEC *d, MODRM *m, int size )
{
    if ( m->is_mem && m->seg >= 0 ) {
        d->out->mem_seg = m->seg;
        d->out->mem_off = m->off;
        d->out->mem_size = size / 8;
    }
}

/* "op  reg,rm" / "op rm,reg" / etc.  DIR 0: rm,reg.  DIR 1: reg,rm. */
static void two_rm( DEC *d, const char *name, int dir, int size )
{
    MODRM m;
    char  rmbuf[40];
    int   save;

    set_mnem( name );
    save = oplen;
    decode_modrm( d, &m, size );
    /* the r/m text is in ops[save..]; register goes before or after */
    mem_cpy( rmbuf, ops + save, oplen - save + 1 );
    oplen = save;
    ops[oplen] = 0;
    if ( dir ) {
        op_str( reg_name( m.reg, size ) );
        op_str( "," );
        if ( m.is_mem ) op_str( rmbuf );
        else op_str( reg_name( m.rm, size ) );
    } else {
        if ( m.is_mem ) op_str( rmbuf );
        else op_str( reg_name( m.rm, size ) );
        op_str( "," );
        op_str( reg_name( m.reg, size ) );
    }
    if ( m.is_mem ) note_mem( d, &m, size );
}

static void imm_rm( DEC *d, const char *name, int size, int immsize, int sign )
{
    MODRM m;
    char  rmbuf[40];
    int   save = oplen;
    u32   imm;

    set_mnem( name );
    decode_modrm( d, &m, size );
    mem_cpy( rmbuf, ops + save, oplen - save + 1 );
    oplen = save;
    ops[oplen] = 0;
    if ( m.is_mem ) {
        op_str( size == 8 ? "BYTE PTR " : ( size == 32 ? "DWORD PTR " : "WORD PTR " ) );
        op_str( rmbuf );
    } else {
        op_str( reg_name( m.rm, size ) );
    }
    op_str( "," );
    if ( immsize == 8 ) {
        imm = fetch( d );
        if ( sign ) {
            /* 83h's byte is signed: CMP WORD PTR [BX],-02, as 6.22 shows */
            if ( imm >= 0x80 ) {
                op_str( "-" );
                op_hex( 0x100 - imm, 2 );
            } else {
                op_str( "+" );
                op_hex( imm, 2 );
            }
        } else {
            op_hex( imm, 2 );
        }
    } else if ( immsize == 32 ) {
        imm = fetch32( d );
        op_hex( imm, 8 );
    } else {
        imm = fetch16( d );
        op_hex( imm, 4 );
    }
    if ( m.is_mem ) note_mem( d, &m, size );
}

static u32 rel_target( DEC *d, s32 disp )
{
    return ( d->at.off + d->pos + disp );
}

static void two_rm( DEC *d, const char *name, int dir, int size );
static void note_mem( DEC *d, MODRM *m, int size );
static const char *reg_name( int which, int size );

#include "disx.inc"

static void decode_one( DEC *d )
{
    u8  op;
    MODRM m;
    int sz;

    mnem[0] = 0;
    ops[0] = 0;
    oplen = 0;

    /* PREFIXES.  6.22'S DEBUG DECODES EACH AS ITS OWN ONE-BYTE
       INSTRUCTION - a segment override is a line "ES:" of its own, a
       REP is "REPZ", a LOCK is "LOCK" - and does not know 64/65/66/67
       at all.  A native program is 32-bit code where 66 and 67 change
       the instruction that follows and the prefixes belong with it, so
       there they are folded (d->opsize/adsize/seg/rep) and the
       instruction is decoded whole. */
    if ( !d->use32 ) {
        const char *pre = NULL;

        op = d->p[d->pos];
        switch ( op ) {
        case 0x26: pre = "ES:"; break;
        case 0x2E: pre = "CS:"; break;
        case 0x36: pre = "SS:"; break;
        case 0x3E: pre = "DS:"; break;
        case 0xF0: pre = "LOCK"; break;
        case 0xF2: pre = "REPNZ"; break;
        case 0xF3: pre = "REPZ"; break;
        }
        if ( pre ) {
            d->pos++;
            set_mnem( pre );
            d->out->kind = DK_PREFIX;   /* U shows what follows with it */
            goto done;
        }
    } else {
        for ( ;; ) {
            op = d->p[d->pos];
            if ( op == 0x66 ) { d->opsize = d->opsize == 16 ? 32 : 16; d->pos++; continue; }
            if ( op == 0x67 ) { d->adsize = d->adsize == 16 ? 32 : 16; d->pos++; continue; }
            if ( op == 0x26 ) { d->seg = 0; d->pos++; continue; }
            if ( op == 0x2E ) { d->seg = 1; d->pos++; continue; }
            if ( op == 0x36 ) { d->seg = 2; d->pos++; continue; }
            if ( op == 0x3E ) { d->seg = 3; d->pos++; continue; }
            if ( op == 0x64 ) { d->seg = 4; d->pos++; continue; }
            if ( op == 0x65 ) { d->seg = 5; d->pos++; continue; }
            if ( op == 0xF0 ) { set_mnem( "LOCK" ); d->pos++; d->out->len = d->pos; d->out->kind = DK_OTHER; return; }
            if ( op == 0xF2 || op == 0xF3 ) { d->rep = op; d->pos++; continue; }
            break;
        }
    }

    sz = d->opsize;
    op = fetch( d );

    /* the eight arithmetic families 00-3F share a pattern */
    if ( op < 0x40 && ( op & 7 ) < 6 && ( ( op >> 3 ) != 4 || ( op & 7 ) < 6 ) &&
         op != 0x0F && ( op & 0xC0 ) == 0 ) {
        int family = op >> 3;
        int form = op & 7;
        const char *name = grp1[family];
        if ( form == 0 ) { two_rm( d, name, 0, 8 ); goto done; }
        if ( form == 1 ) { two_rm( d, name, 0, sz ); goto done; }
        if ( form == 2 ) { two_rm( d, name, 1, 8 ); goto done; }
        if ( form == 3 ) { two_rm( d, name, 1, sz ); goto done; }
        if ( form == 4 ) { set_mnem( name ); op_str( "AL," ); op_hex( fetch( d ), 2 ); goto done; }
        if ( form == 5 ) { set_mnem( name ); op_str( sz == 32 ? "EAX," : "AX," );
                           if ( sz == 32 ) op_hex( fetch32( d ), 8 ); else op_hex( fetch16( d ), 4 ); goto done; }
    }

    switch ( op ) {
    case 0x06: set_mnem( "PUSH" ); op_str( "ES" ); break;
    case 0x07: set_mnem( "POP" );  op_str( "ES" ); break;
    case 0x0E: set_mnem( "PUSH" ); op_str( "CS" ); break;
    case 0x16: set_mnem( "PUSH" ); op_str( "SS" ); break;
    case 0x17: set_mnem( "POP" );  op_str( "SS" ); break;
    case 0x1E: set_mnem( "PUSH" ); op_str( "DS" ); break;
    case 0x1F: set_mnem( "POP" );  op_str( "DS" ); break;
    case 0x27: set_mnem( "DAA" ); break;
    case 0x2F: set_mnem( "DAS" ); break;
    case 0x37: set_mnem( "AAA" ); break;
    case 0x3F: set_mnem( "AAS" ); break;
    }

    if ( mnem[0] ) goto done;

    if ( op >= 0x40 && op <= 0x47 ) { set_mnem( "INC" ); op_str( reg_name( op - 0x40, sz ) ); goto done; }
    if ( op >= 0x48 && op <= 0x4F ) { set_mnem( "DEC" ); op_str( reg_name( op - 0x48, sz ) ); goto done; }
    if ( op >= 0x50 && op <= 0x57 ) { set_mnem( "PUSH" ); op_str( reg_name( op - 0x50, sz ) ); goto done; }
    if ( op >= 0x58 && op <= 0x5F ) { set_mnem( "POP" ); op_str( reg_name( op - 0x58, sz ) ); goto done; }
    if ( op >= 0x70 && op <= 0x7F ) {
        s8 disp = (s8)fetch( d );
        set_mnem( jcc[op - 0x70] );
        op_hex( rel_target( d, disp ) & ( sz == 32 ? 0xFFFFFFFFUL : 0xFFFF ), sz == 32 ? 8 : 4 );
        d->out->kind = DK_JUMP;
        d->out->target = rel_target( d, disp );
        goto done;
    }
    if ( op >= 0x91 && op <= 0x97 ) { set_mnem( "XCHG" ); op_str( reg_name( op - 0x90, sz ) ); op_str( sz == 32 ? ",EAX" : ",AX" ); goto done; }
    if ( op >= 0xB0 && op <= 0xB7 ) { set_mnem( "MOV" ); op_str( r8[op - 0xB0] ); op_str( "," ); op_hex( fetch( d ), 2 ); goto done; }
    if ( op >= 0xB8 && op <= 0xBF ) {
        set_mnem( "MOV" ); op_str( reg_name( op - 0xB8, sz ) ); op_str( "," );
        if ( sz == 32 ) op_hex( fetch32( d ), 8 ); else op_hex( fetch16( d ), 4 );
        goto done;
    }

    /* the rest, by opcode */
    switch ( op ) {
    case 0x80: case 0x81: case 0x82: case 0x83: {
        /* group 1: the family is the reg field */
        int save = oplen, immsz;
        u8  modrm = d->p[d->pos];
        const char *name = grp1[( modrm >> 3 ) & 7];
        oplen = save;
        if ( op == 0x81 ) immsz = sz;
        else immsz = 8;
        imm_rm( d, name, op == 0x80 || op == 0x82 ? 8 : sz, immsz, op == 0x83 );
        break;
    }
    case 0x84: two_rm( d, "TEST", 1, 8 ); break;
    case 0x85: two_rm( d, "TEST", 1, sz ); break;
    case 0x86: two_rm( d, "XCHG", 1, 8 ); break;
    case 0x87: two_rm( d, "XCHG", 1, sz ); break;
    case 0x88: two_rm( d, "MOV", 0, 8 ); break;
    case 0x89: two_rm( d, "MOV", 0, sz ); break;
    case 0x8A: two_rm( d, "MOV", 1, 8 ); break;
    case 0x8B: two_rm( d, "MOV", 1, sz ); break;
    case 0x8C: {
        int save = oplen; char rmbuf[40];
        set_mnem( "MOV" );
        decode_modrm( d, &m, 16 );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        if ( m.is_mem ) op_str( rmbuf ); else op_str( reg_name( m.rm, 16 ) );
        op_str( "," ); op_str( sreg[m.reg] );
        note_mem( d, &m, 16 );
        break;
    }
    case 0x8D: two_rm( d, "LEA", 1, sz ); break;
    case 0x8E: {
        int save = oplen; char rmbuf[40];
        set_mnem( "MOV" );
        decode_modrm( d, &m, 16 );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        op_str( sreg[m.reg] ); op_str( "," );
        if ( m.is_mem ) op_str( rmbuf ); else op_str( reg_name( m.rm, 16 ) );
        note_mem( d, &m, 16 );
        break;
    }
    case 0x8F: {
        int save = oplen; char rmbuf[40];
        set_mnem( "POP" );
        decode_modrm( d, &m, sz );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        if ( m.is_mem ) op_str( rmbuf ); else op_str( reg_name( m.rm, sz ) );
        note_mem( d, &m, sz );
        break;
    }
    case 0x90: set_mnem( "NOP" ); break;
    case 0x98: set_mnem( sz == 32 ? "CWDE" : "CBW" ); break;
    case 0x99: set_mnem( sz == 32 ? "CDQ" : "CWD" ); break;
    case 0x9A: {
        u32 off = sz == 32 ? fetch32( d ) : fetch16( d );
        u16 seg = fetch16( d );
        set_mnem( "CALL" );
        op_hex( seg, 4 ); op_str( ":" ); op_hex( off, sz == 32 ? 8 : 4 );
        d->out->kind = DK_CALL;
        break;
    }
    case 0x9B: set_mnem( "WAIT" ); break;
    case 0x9C: set_mnem( sz == 32 ? "PUSHFD" : "PUSHF" ); break;
    case 0x9D: set_mnem( sz == 32 ? "POPFD" : "POPF" ); break;
    case 0x9E: set_mnem( "SAHF" ); break;
    case 0x9F: set_mnem( "LAHF" ); break;
    case 0xA0: set_mnem( "MOV" ); op_str( "AL," ); open_mem( d ); op_hex( d->adsize == 32 ? fetch32( d ) : fetch16( d ), d->adsize == 32 ? 8 : 4 ); op_str( "]" ); break;
    case 0xA1: set_mnem( "MOV" ); op_str( sz == 32 ? "EAX," : "AX," ); open_mem( d ); op_hex( d->adsize == 32 ? fetch32( d ) : fetch16( d ), d->adsize == 32 ? 8 : 4 ); op_str( "]" ); break;
    case 0xA2: set_mnem( "MOV" ); open_mem( d ); op_hex( d->adsize == 32 ? fetch32( d ) : fetch16( d ), d->adsize == 32 ? 8 : 4 ); op_str( "]," ); op_str( "AL" ); break;
    case 0xA3: set_mnem( "MOV" ); open_mem( d ); op_hex( d->adsize == 32 ? fetch32( d ) : fetch16( d ), d->adsize == 32 ? 8 : 4 ); op_str( "]," ); op_str( sz == 32 ? "EAX" : "AX" ); break;
    case 0xA4: set_mnem( "MOVSB" ); break;
    case 0xA5: set_mnem( sz == 32 ? "MOVSD" : "MOVSW" ); break;
    case 0xA6: set_mnem( "CMPSB" ); break;
    case 0xA7: set_mnem( sz == 32 ? "CMPSD" : "CMPSW" ); break;
    case 0xA8: set_mnem( "TEST" ); op_str( "AL," ); op_hex( fetch( d ), 2 ); break;
    case 0xA9: set_mnem( "TEST" ); op_str( sz == 32 ? "EAX," : "AX," ); if ( sz == 32 ) op_hex( fetch32( d ), 8 ); else op_hex( fetch16( d ), 4 ); break;
    case 0xAA: set_mnem( "STOSB" ); break;
    case 0xAB: set_mnem( sz == 32 ? "STOSD" : "STOSW" ); break;
    case 0xAC: set_mnem( "LODSB" ); break;
    case 0xAD: set_mnem( sz == 32 ? "LODSD" : "LODSW" ); break;
    case 0xAE: set_mnem( "SCASB" ); break;
    case 0xAF: set_mnem( sz == 32 ? "SCASD" : "SCASW" ); break;
    case 0xC2: set_mnem( "RET" ); op_hex( fetch16( d ), 4 ); break;
    case 0xC3: set_mnem( "RET" ); break;
    case 0xC4: two_rm( d, "LES", 1, sz ); break;
    case 0xC5: two_rm( d, "LDS", 1, sz ); break;
    case 0xC6: imm_rm( d, "MOV", 8, 8, 0 ); break;
    case 0xC7: imm_rm( d, "MOV", sz, sz, 0 ); break;
    case 0xCA: set_mnem( "RETF" ); op_hex( fetch16( d ), 4 ); break;
    case 0xCB: set_mnem( "RETF" ); break;
    case 0xCC: set_mnem( "INT" ); op_str( "3" ); d->out->kind = DK_INT3; break;
    case 0xCD: set_mnem( "INT" ); op_hex( fetch( d ), 2 ); d->out->kind = DK_INT; break;
    case 0xCE: set_mnem( "INTO" ); d->out->kind = DK_INT; break;
    case 0xCF: set_mnem( sz == 32 ? "IRETD" : "IRET" ); break;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int save = oplen; char rmbuf[40];
        u8  modrm = d->p[d->pos];
        const char *name = grp2[( modrm >> 3 ) & 7];
        int osz = ( op & 1 ) ? sz : 8;
        set_mnem( name );
        decode_modrm( d, &m, osz );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        if ( m.is_mem ) { op_str( osz == 8 ? "BYTE PTR " : ( osz == 32 ? "DWORD PTR " : "WORD PTR " ) ); op_str( rmbuf ); }
        else op_str( reg_name( m.rm, osz ) );
        op_str( "," );
        op_str( ( op & 2 ) ? "CL" : "1" );
        note_mem( d, &m, osz );
        break;
    }
    /* base 10 is shown bare, as 6.22 shows it; any other base after it */
    case 0xD4: { u8 b = fetch( d ); set_mnem( "AAM" ); if ( b != 0x0A ) op_hex( b, 2 ); break; }
    case 0xD5: { u8 b = fetch( d ); set_mnem( "AAD" ); if ( b != 0x0A ) op_hex( b, 2 ); break; }
    case 0xD7: set_mnem( "XLAT" ); break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        decode_fpu( d, op );
        break;
    case 0xE0: set_mnem( "LOOPNZ" ); op_hex( rel_target( d, (s8)fetch( d ) ), 4 ); d->out->kind = DK_LOOP; break;
    case 0xE1: set_mnem( "LOOPZ" ); op_hex( rel_target( d, (s8)fetch( d ) ), 4 ); d->out->kind = DK_LOOP; break;
    case 0xE2: set_mnem( "LOOP" ); op_hex( rel_target( d, (s8)fetch( d ) ), 4 ); d->out->kind = DK_LOOP; break;
    case 0xE3: set_mnem( sz == 32 ? "JECXZ" : "JCXZ" ); op_hex( rel_target( d, (s8)fetch( d ) ), 4 ); d->out->kind = DK_JUMP; break;
    case 0xE4: set_mnem( "IN" ); op_str( "AL," ); op_hex( fetch( d ), 2 ); break;
    case 0xE5: set_mnem( "IN" ); op_str( sz == 32 ? "EAX," : "AX," ); op_hex( fetch( d ), 2 ); break;
    case 0xE6: set_mnem( "OUT" ); op_hex( fetch( d ), 2 ); op_str( ",AL" ); break;
    case 0xE7: set_mnem( "OUT" ); op_hex( fetch( d ), 2 ); op_str( sz == 32 ? ",EAX" : ",AX" ); break;
    case 0xE8: {
        s32 disp = sz == 32 ? (s32)fetch32( d ) : (s32)(s16)fetch16( d );
        set_mnem( "CALL" );
        op_hex( rel_target( d, disp ) & ( sz == 32 ? 0xFFFFFFFFUL : 0xFFFF ), sz == 32 ? 8 : 4 );
        d->out->kind = DK_CALL;
        d->out->target = rel_target( d, disp );
        break;
    }
    case 0xE9: {
        s32 disp = sz == 32 ? (s32)fetch32( d ) : (s32)(s16)fetch16( d );
        set_mnem( "JMP" );
        op_hex( rel_target( d, disp ) & ( sz == 32 ? 0xFFFFFFFFUL : 0xFFFF ), sz == 32 ? 8 : 4 );
        d->out->kind = DK_JUMP;
        break;
    }
    case 0xEA: {
        u32 off = sz == 32 ? fetch32( d ) : fetch16( d );
        u16 seg = fetch16( d );
        set_mnem( "JMP" );
        op_hex( seg, 4 ); op_str( ":" ); op_hex( off, sz == 32 ? 8 : 4 );
        d->out->kind = DK_JUMP;
        break;
    }
    case 0xEB: set_mnem( "JMP" ); op_hex( rel_target( d, (s8)fetch( d ) ) & 0xFFFF, 4 ); d->out->kind = DK_JUMP; break;
    case 0xEC: set_mnem( "IN" ); op_str( "AL,DX" ); break;
    case 0xED: set_mnem( "IN" ); op_str( sz == 32 ? "EAX,DX" : "AX,DX" ); break;
    case 0xEE: set_mnem( "OUT" ); op_str( "DX,AL" ); break;
    case 0xEF: set_mnem( "OUT" ); op_str( sz == 32 ? "DX,EAX" : "DX,AX" ); break;
    case 0xF1: set_mnem( "DB" ); op_hex( 0xF1, 2 ); break;
    case 0xF4: set_mnem( "HLT" ); break;
    case 0xF5: set_mnem( "CMC" ); break;
    case 0xF6: case 0xF7: {
        int save = oplen; char rmbuf[40];
        u8  modrm = d->p[d->pos];
        int reg = ( modrm >> 3 ) & 7;
        int osz = ( op & 1 ) ? sz : 8;
        set_mnem( grp3[reg] );
        decode_modrm( d, &m, osz );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        if ( m.is_mem ) { op_str( osz == 8 ? "BYTE PTR " : ( osz == 32 ? "DWORD PTR " : "WORD PTR " ) ); op_str( rmbuf ); }
        else op_str( reg_name( m.rm, osz ) );
        if ( reg == 0 ) {               /* TEST takes an immediate */
            op_str( "," );
            if ( osz == 8 ) op_hex( fetch( d ), 2 );
            else if ( osz == 32 ) op_hex( fetch32( d ), 8 );
            else op_hex( fetch16( d ), 4 );
        }
        note_mem( d, &m, osz );
        break;
    }
    case 0xF8: set_mnem( "CLC" ); break;
    case 0xF9: set_mnem( "STC" ); break;
    case 0xFA: set_mnem( "CLI" ); break;
    case 0xFB: set_mnem( "STI" ); break;
    case 0xFC: set_mnem( "CLD" ); break;
    case 0xFD: set_mnem( "STD" ); break;
    case 0xFE: case 0xFF: {
        int save = oplen; char rmbuf[40];
        u8  modrm = d->p[d->pos];
        int reg = ( modrm >> 3 ) & 7;
        int osz = ( op & 1 ) ? sz : 8;
        static const char *g5[] = { "INC","DEC","CALL","CALL","JMP","JMP","PUSH","???" };
        set_mnem( g5[reg] );
        decode_modrm( d, &m, osz );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        if ( reg == 3 || reg == 5 ) op_str( "FAR " );
        if ( m.is_mem ) {
            if ( reg <= 1 ) op_str( osz == 8 ? "BYTE PTR " : ( osz == 32 ? "DWORD PTR " : "WORD PTR " ) );
            op_str( rmbuf );
        } else {
            op_str( reg_name( m.rm, osz ) );
        }
        if ( reg == 2 || reg == 3 ) d->out->kind = DK_CALL;
        else if ( reg == 4 || reg == 5 ) d->out->kind = DK_JUMP;
        note_mem( d, &m, osz );
        break;
    }
    case 0x0F:
        if ( d->use32 ) { decode_0f( d ); }
        else { set_mnem( "DB" ); op_hex( 0x0F, 2 ); d->pos = 1; }
        break;
    default:
        if ( !mnem[0] ) {
            set_mnem( "DB" );
            op_hex( op, 2 );
        }
        break;
    }

done:
    d->out->len = d->pos;
    if ( d->rep && ( d->out->kind == DK_OTHER ) ) {
        d->out->kind = DK_REP;
    }
}

/* ------------------------------------------------------------------ */
/* the public call                                                     */
/* ------------------------------------------------------------------ */

void disasm( ADDR addr, const u8 *code, int use32, DISASM *dis )
{
    DEC d;

    mem_set( dis, 0, sizeof( *dis ) );
    dis->mem_seg = -1;
    dis->kind = DK_OTHER;
    d.p = code;
    d.at = addr;
    d.pos = 0;
    d.opsize = use32 ? 32 : 16;
    d.adsize = use32 ? 32 : 16;
    d.seg = -1;
    d.rep = 0;
    d.use32 = use32;
    d.x87 = 0;
    d.out = dis;
    decode_one( &d );
    if ( dis->len == 0 || dis->len > DIS_MAX ) {
        dis->len = 1;
    }

    /* The text 6.22 prints: the mnemonic, a TAB - always, even with no
       operands - and the operands.  An 8087 instruction it names has a
       TAB in front as well, and a TAB after only when operands follow.
       A native program's REP is folded in front, "REPZ MOVSB", and its
       TAB only comes with operands.  show_insn pads the line. */
    {
        int col = 0;
        char *t = dis->text;
        const char *s;
        if ( d.rep == 0xF3 ) { s = "REPZ "; while ( *s ) t[col++] = *s++; }
        else if ( d.rep == 0xF2 ) { s = "REPNZ "; while ( *s ) t[col++] = *s++; }
        if ( d.x87 ) {
            t[col++] = '\t';
        }
        s = mnem;
        while ( *s ) t[col++] = *s++;
        if ( ops[0] || ( !use32 && !d.x87 ) ) {
            t[col++] = '\t';
            s = ops;
            while ( *s ) t[col++] = *s++;
        }
        t[col] = 0;
    }
}

/* U [range].  A bare U carries on from the last one, or from CS:IP after
   the registers were shown (REGS.C, show_regs). */
void cmd_unassemble( void )
{
    ADDR addr;
    u32  len, done = 0;

    addr = next_unasm;
    get_range( &addr, &len, cpu.cs, 0x20, 0 );
    need_end();
    /* 64K was a count of 0 to 6.22's U, which stopped after the first
       instruction: U 100 L 0 and U 0 FFFF show one line */
    if ( len == 0x10000UL && mem_is_low( addr ) ) {
        len = 1;
    }
    {
        int prefix = 0;
        int run = 0;
        /* a prefix's line and the instruction it is for are one step to
           6.22's U: U 100 L 1 on "26 06" shows ES: and PUSH ES.  Fifteen
           prefixes is the most one instruction can have, and a segment
           full of them must not go round for ever. */
        while ( done < len || ( prefix && run < 15 ) ) {
            u8 code[DIS_MAX];
            DISASM dis;

            show_insn( addr, 0 );
            mem_read( addr, code, DIS_MAX );
            disasm( addr, code, mode == MODE_NAT, &dis );
            addr_add( &addr, dis.len );
            done += dis.len;
            prefix = ( dis.kind == DK_PREFIX );
            run = prefix ? run + 1 : 0;
            if ( !mem_is_low( addr ) && addr.off >= mem_seg_end( addr ) ) {
                break;
            }
        }
    }
    next_unasm = addr;
}

/* one line: address, the bytes, the text; WITH_MEM adds R's operand
   value */
void show_insn( ADDR addr, int with_mem )
{
    u8     code[DIS_MAX];
    DISASM dis;
    u32    i;
    int    mnem_col = mode == MODE_NAT ? 40 : 24;   /* where the mnemonic sits */
    int    end_col  = mnem_col + 39;                /* and where the line ends */

    mem_read( addr, code, DIS_MAX );
    disasm( addr, code, mode == MODE_NAT, &dis );
    /* 6.22 pads the operands after the TAB to 35 bytes, so its lines
       grow with the mnemonic (RET's is 63, XCHG's 64) - and an 8087
       line, with its TAB in front, to 60 in all */
    if ( mode != MODE_NAT ) {
        u32 k = 0;
        while ( dis.text[k] && dis.text[k] != '\t' ) {
            k++;
        }
        end_col = dis.text[0] == '\t' ? 60 : mnem_col + (int)k + 1 + 35;
    }
    out_addr( addr );
    out_ch( ' ' );
    for ( i = 0; i < dis.len; i++ ) {
        out_hex( code[i], 2 );
    }
    out_pad( mnem_col );
    out_str( dis.text );
    out_pad( end_col );
    if ( with_mem && dis.mem_seg >= 0 ) {
        ADDR m;
        u8   v[4], ok[4];
        m.seg = dis.mem_seg == 0 ? cpu.es : dis.mem_seg == 1 ? cpu.cs :
                dis.mem_seg == 2 ? cpu.ss : dis.mem_seg == 3 ? cpu.ds :
                dis.mem_seg == 4 ? cpu.fs : cpu.gs;
        m.off = dis.mem_off;
        out_str( sreg[dis.mem_seg] );
        out_ch( ':' );
        out_hex( dis.mem_off, mode == MODE_NAT ? 8 : 4 );
        out_ch( '=' );
        mem_get( m, v, dis.mem_size, ok );
        if ( dis.mem_size == 1 ) out_hex( v[0], 2 );
        else if ( dis.mem_size == 2 ) out_hex( v[0] | ( v[1] << 8 ), 4 );
        else out_hex( v[0] | ( (u32)v[1] << 8 ) | ( (u32)v[2] << 16 ) | ( (u32)v[3] << 24 ), 8 );
    }
    out_nl();
}
