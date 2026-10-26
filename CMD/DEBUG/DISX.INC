/*
 * DISX.INC - the x87 and the 0F two-byte decoders, #included into DIS.C
 * so that they share its ModRM machinery and its output buffer.
 *
 * 6.22'S DEBUG KNOWS THE 8087 AND NOTHING LATER: it names the 8087's
 * instructions (and the aliases an 8087 obeys, DD C8+i being FXCH),
 * prints everything else in D8-DF as "ESC nn,operand", and every 0F is
 * "DB 0F".  16-bit code is shown its way (decode_8087).  A native
 * PM-DOS program is 32-bit code full of 387 and 0F instructions, so
 * there DEBUG decodes them by their real names (decode_fpu, decode_0f),
 * the way it shows the 32-bit registers 6.22 had no word for.
 */

static const char *st_i( int i )
{
    static char buf[8];
    buf[0] = 'S'; buf[1] = 'T'; buf[2] = '('; buf[3] = (char)( '0' + i ); buf[4] = ')'; buf[5] = 0;
    return buf;
}

/* the memory-form mnemonic and operand size for each D8-DF /reg */
static void fpu_mem( DEC *d, u8 op, MODRM *m, char *rmbuf )
{
    static const char *d8[] = { "FADD","FMUL","FCOM","FCOMP","FSUB","FSUBR","FDIV","FDIVR" };
    static const char *d9[] = { "FLD","???","FST","FSTP","FLDENV","FLDCW","FSTENV","FSTCW" };
    static const char *da[] = { "FIADD","FIMUL","FICOM","FICOMP","FISUB","FISUBR","FIDIV","FIDIVR" };
    static const char *db[] = { "FILD","???","FIST","FISTP","???","FLD","???","FSTP" };
    static const char *dc[] = { "FADD","FMUL","FCOM","FCOMP","FSUB","FSUBR","FDIV","FDIVR" };
    static const char *dd[] = { "FLD","???","FST","FSTP","FRSTOR","???","FSAVE","FSTSW" };
    static const char *de[] = { "FIADD","FIMUL","FICOM","FICOMP","FISUB","FISUBR","FIDIV","FIDIVR" };
    static const char *df[] = { "FILD","???","FIST","FISTP","FBLD","FILD","FBSTP","FISTP" };
    int reg = m->reg;
    const char *name = "???";
    const char *size = "";

    switch ( op ) {
    case 0xD8: name = d8[reg]; size = "DWORD PTR "; break;
    case 0xD9: name = d9[reg]; size = ( reg <= 3 ) ? "DWORD PTR " : ""; break;
    case 0xDA: name = da[reg]; size = "DWORD PTR "; break;
    case 0xDB: name = db[reg]; size = ( reg == 5 || reg == 7 ) ? "TBYTE PTR " : "DWORD PTR "; break;
    case 0xDC: name = dc[reg]; size = "QWORD PTR "; break;
    case 0xDD: name = dd[reg]; size = ( reg <= 3 ) ? "QWORD PTR " : ""; break;
    case 0xDE: name = de[reg]; size = "WORD PTR "; break;
    case 0xDF: name = df[reg]; size = ( reg == 4 || reg == 6 ) ? "TBYTE PTR " : ( reg >= 5 ) ? "QWORD PTR " : "WORD PTR "; break;
    }
    set_mnem( name );
    op_str( size );
    op_str( rmbuf );
}

/* "ESC nn,operand": nn is the opcode's low three bits and the reg field */
static void esc_form( u8 op, int reg, const char *opnd )
{
    set_mnem( "ESC" );
    op_hex( ( ( op & 7 ) << 3 ) | reg, 2 );
    op_str( "," );
    op_str( opnd );
}

/* 16-bit code: the 8087 as 6.22 decoded it.  Its names and operand forms
   (FCOM ST(1), FCOMPP ST(1)), the aliases the 8087 obeys, and ESC with a
   byte register (D8 DA DC DE) or a word one (D9 DB DD DF) for the rest -
   which takes in everything the 287 and 387 added.  A reserved memory
   form is ESC nn,[mem]; 6.22 read two bytes past those, which is not
   copied. */
static void decode_8087( DEC *d, u8 op )
{
    static const char *d8[] = { "FADD","FMUL","FCOM","FCOMP","FSUB","FSUBR","FDIV","FDIVR" };
    static const char *dc[] = { "FADD","FMUL","FCOM","FCOMP","FSUBR","FSUB","FDIVR","FDIV" };
    static const char *dd[] = { "FFREE","FXCH","FST","FSTP",NULL,NULL,NULL,NULL };
    static const char *de[] = { "FADDP","FMULP","FCOMP","FCOMPP","FSUBRP","FSUBP","FDIVRP","FDIVP" };
    MODRM m;
    char  rmbuf[40];
    int   save = oplen;
    u8    modrm = d->p[d->pos];
    int   reg = ( modrm >> 3 ) & 7;
    int   rm = modrm & 7;
    const char *name = NULL;
    int   form = 0;                 /* 0 none, 1 ST(i), 2 ST,ST(i), 3 ST(i),ST */

    decode_modrm( d, &m, 32 );
    mem_cpy( rmbuf, ops + save, oplen - save + 1 );
    oplen = save;
    ops[oplen] = 0;

    if ( m.is_mem ) {
        fpu_mem( d, op, &m, rmbuf );
        if ( mnem[0] == '?' ) {
            oplen = save;
            ops[oplen] = 0;
            esc_form( op, reg, rmbuf );
            return;
        }
        d->x87 = 1;
        note_mem( d, &m, op == 0xD8 || op == 0xDA ? 32 :
                         op == 0xDE ? 16 : op == 0xDC || ( op == 0xDD && reg <= 3 ) ? 64 : 32 );
        return;
    }

    switch ( op ) {
    case 0xD8:
        name = d8[reg];
        form = ( reg == 2 || reg == 3 ) ? 1 : 2;
        break;
    case 0xD9:
        if ( reg == 0 ) { name = "FLD"; form = 1; break; }
        if ( reg == 1 ) { name = "FXCH"; form = 1; break; }
        if ( reg == 3 ) { name = "FSTP"; form = 1; break; }
        switch ( modrm ) {
        case 0xD0: name = "FNOP"; break;
        case 0xE0: name = "FCHS"; break;
        case 0xE1: name = "FABS"; break;
        case 0xE4: name = "FTST"; break;
        case 0xE5: name = "FXAM"; break;
        case 0xE8: name = "FLD1"; break;
        case 0xE9: name = "FLDL2T"; break;
        case 0xEA: name = "FLDL2E"; break;
        case 0xEB: name = "FLDPI"; break;
        case 0xEC: name = "FLDLG2"; break;
        case 0xED: name = "FLDLN2"; break;
        case 0xEE: name = "FLDZ"; break;
        case 0xF0: name = "F2XM1"; break;
        case 0xF1: name = "FYL2X"; break;
        case 0xF2: name = "FPTAN"; break;
        case 0xF3: name = "FPATAN"; break;
        case 0xF4: name = "FXTRACT"; break;
        case 0xF6: name = "FDECSTP"; break;
        case 0xF7: name = "FINCSTP"; break;
        case 0xF8: name = "FPREM"; break;
        case 0xF9: name = "FYL2XP1"; break;
        case 0xFA: name = "FSQRT"; break;
        case 0xFC: name = "FRNDINT"; break;
        case 0xFD: name = "FSCALE"; break;
        }
        break;
    case 0xDB:
        switch ( modrm ) {
        case 0xE0: name = "FENI"; break;
        case 0xE1: name = "FDISI"; break;
        case 0xE2: name = "FCLEX"; break;
        case 0xE3: name = "FINIT"; break;
        }
        break;
    case 0xDC:
        name = dc[reg];
        form = ( reg == 2 || reg == 3 ) ? 1 : 3;
        break;
    case 0xDD:
    case 0xDF:
        name = dd[reg];
        form = 1;
        break;
    case 0xDE:
        name = de[reg];
        form = ( reg == 2 || reg == 3 ) ? 1 : 3;
        break;
    }
    if ( name == NULL ) {
        esc_form( op, reg, reg_name( rm, ( op & 1 ) ? 16 : 8 ) );
        return;
    }
    d->x87 = 1;
    set_mnem( name );
    if ( form == 1 ) {
        op_str( st_i( rm ) );
    } else if ( form == 2 ) {
        op_str( "ST," );
        op_str( st_i( rm ) );
    } else if ( form == 3 ) {
        op_str( st_i( rm ) );
        op_str( ",ST" );
    }
}

static void decode_fpu( DEC *d, u8 op )
{
    MODRM m;
    char  rmbuf[40];
    int   save = oplen;
    u8    modrm = d->p[d->pos];
    int   reg = ( modrm >> 3 ) & 7;
    int   rm = modrm & 7;

    if ( !d->use32 ) {
        decode_8087( d, op );
        return;
    }
    decode_modrm( d, &m, 32 );
    mem_cpy( rmbuf, ops + save, oplen - save + 1 );
    oplen = save;
    ops[oplen] = 0;

    if ( m.is_mem ) {
        fpu_mem( d, op, &m, rmbuf );
        note_mem( d, &m, op == 0xD8 || op == 0xDA ? 32 :
                         op == 0xDE ? 16 : op == 0xDC || ( op == 0xDD && reg <= 3 ) ? 64 : 32 );
        return;
    }

    /* register forms, by op and the whole modrm */
    switch ( op ) {
    case 0xD8: {
        static const char *n[] = { "FADD","FMUL","FCOM","FCOMP","FSUB","FSUBR","FDIV","FDIVR" };
        set_mnem( n[reg] ); op_str( "ST," ); op_str( st_i( rm ) ); return;
    }
    case 0xD9:
        if ( reg == 0 ) { set_mnem( "FLD" ); op_str( st_i( rm ) ); return; }
        if ( reg == 1 ) { set_mnem( "FXCH" ); op_str( st_i( rm ) ); return; }
        switch ( modrm ) {
        case 0xD0: set_mnem( "FNOP" ); return;
        case 0xE0: set_mnem( "FCHS" ); return;
        case 0xE1: set_mnem( "FABS" ); return;
        case 0xE4: set_mnem( "FTST" ); return;
        case 0xE5: set_mnem( "FXAM" ); return;
        case 0xE8: set_mnem( "FLD1" ); return;
        case 0xE9: set_mnem( "FLDL2T" ); return;
        case 0xEA: set_mnem( "FLDL2E" ); return;
        case 0xEB: set_mnem( "FLDPI" ); return;
        case 0xEC: set_mnem( "FLDLG2" ); return;
        case 0xED: set_mnem( "FLDLN2" ); return;
        case 0xEE: set_mnem( "FLDZ" ); return;
        case 0xF0: set_mnem( "F2XM1" ); return;
        case 0xF1: set_mnem( "FYL2X" ); return;
        case 0xF2: set_mnem( "FPTAN" ); return;
        case 0xF3: set_mnem( "FPATAN" ); return;
        case 0xF8: set_mnem( "FPREM" ); return;
        case 0xFA: set_mnem( "FSQRT" ); return;
        case 0xFC: set_mnem( "FRNDINT" ); return;
        case 0xFD: set_mnem( "FSCALE" ); return;
        case 0xFE: set_mnem( "FSIN" ); return;
        case 0xFF: set_mnem( "FCOS" ); return;
        }
        set_mnem( "???" ); return;
    case 0xDA:
        if ( modrm == 0xE9 ) { set_mnem( "FUCOMPP" ); return; }
        set_mnem( "???" ); return;
    case 0xDB:
        if ( modrm == 0xE2 ) { set_mnem( "FCLEX" ); return; }
        if ( modrm == 0xE3 ) { set_mnem( "FINIT" ); return; }
        set_mnem( "???" ); return;
    case 0xDC: {
        static const char *n[] = { "FADD","FMUL","FCOM","FCOMP","FSUBR","FSUB","FDIVR","FDIV" };
        set_mnem( n[reg] ); op_str( st_i( rm ) ); op_str( ",ST" ); return;
    }
    case 0xDD:
        if ( reg == 0 ) { set_mnem( "FFREE" ); op_str( st_i( rm ) ); return; }
        if ( reg == 2 ) { set_mnem( "FST" ); op_str( st_i( rm ) ); return; }
        if ( reg == 3 ) { set_mnem( "FSTP" ); op_str( st_i( rm ) ); return; }
        if ( reg == 4 ) { set_mnem( "FUCOM" ); op_str( st_i( rm ) ); return; }
        if ( reg == 5 ) { set_mnem( "FUCOMP" ); op_str( st_i( rm ) ); return; }
        set_mnem( "???" ); return;
    case 0xDE:
        if ( modrm == 0xD9 ) { set_mnem( "FCOMPP" ); return; }
        {
            static const char *n[] = { "FADDP","FMULP","???","???","FSUBRP","FSUBP","FDIVRP","FDIVP" };
            set_mnem( n[reg] ); op_str( st_i( rm ) ); op_str( ",ST" ); return;
        }
    case 0xDF:
        if ( modrm == 0xE0 ) { set_mnem( "FSTSW" ); op_str( "AX" ); return; }
        set_mnem( "???" ); return;
    }
    set_mnem( "???" );
}

/* the 0F two-byte opcodes a 386 program uses */
static const char *cc_name[] = {
    "O","NO","B","NB","Z","NZ","BE","A","S","NS","PE","PO","L","GE","LE","G"
};

static void decode_0f( DEC *d )
{
    u8  op = fetch( d );
    int sz = d->opsize;
    MODRM m;
    char  rmbuf[40];
    int   save;

    if ( op >= 0x80 && op <= 0x8F ) {       /* long conditional jumps */
        s32 disp = sz == 32 ? (s32)fetch32( d ) : (s32)(s16)fetch16( d );
        char nm[6];
        nm[0] = 'J';
        str_cpy( nm + 1, cc_name[op - 0x80] );
        set_mnem( nm );
        op_hex( rel_target( d, disp ) & ( sz == 32 ? 0xFFFFFFFFUL : 0xFFFF ), sz == 32 ? 8 : 4 );
        d->out->kind = DK_JUMP;
        return;
    }
    if ( op >= 0x90 && op <= 0x9F ) {       /* SETcc r/m8 */
        char nm[8];
        nm[0] = 'S'; nm[1] = 'E'; nm[2] = 'T';
        str_cpy( nm + 3, cc_name[op - 0x90] );
        save = oplen;
        decode_modrm( d, &m, 8 );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        set_mnem( nm );
        if ( m.is_mem ) { op_str( "BYTE PTR " ); op_str( rmbuf ); note_mem( d, &m, 8 ); }
        else op_str( reg_name( m.rm, 8 ) );
        return;
    }
    switch ( op ) {
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
        int srcsz = ( op & 1 ) ? 16 : 8;
        set_mnem( op < 0xBE ? "MOVZX" : "MOVSX" );
        save = oplen;
        decode_modrm( d, &m, srcsz );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        op_str( reg_name( m.reg, sz ) );
        op_str( "," );
        if ( m.is_mem ) { op_str( srcsz == 8 ? "BYTE PTR " : "WORD PTR " ); op_str( rmbuf ); note_mem( d, &m, srcsz ); }
        else op_str( reg_name( m.rm, srcsz ) );
        return;
    }
    case 0xAF: two_rm( d, "IMUL", 1, sz ); return;
    case 0xA3: two_rm( d, "BT", 0, sz ); return;
    case 0xAB: two_rm( d, "BTS", 0, sz ); return;
    case 0xB3: two_rm( d, "BTR", 0, sz ); return;
    case 0xBB: two_rm( d, "BTC", 0, sz ); return;
    case 0xBC: two_rm( d, "BSF", 1, sz ); return;
    case 0xBD: two_rm( d, "BSR", 1, sz ); return;
    case 0xA0: set_mnem( "PUSH" ); op_str( "FS" ); return;
    case 0xA1: set_mnem( "POP" ); op_str( "FS" ); return;
    case 0xA8: set_mnem( "PUSH" ); op_str( "GS" ); return;
    case 0xA9: set_mnem( "POP" ); op_str( "GS" ); return;
    case 0xA2: set_mnem( "CPUID" ); return;
    case 0x31: set_mnem( "RDTSC" ); return;
    case 0x05: set_mnem( "SYSCALL" ); return;
    case 0x0B: set_mnem( "UD2" ); return;
    case 0x08: set_mnem( "INVD" ); return;
    case 0x09: set_mnem( "WBINVD" ); return;
    case 0x06: set_mnem( "CLTS" ); return;
    case 0xA4: case 0xAC: {                 /* SHLD/SHRD rm,reg,imm8 */
        save = oplen;
        set_mnem( op == 0xA4 ? "SHLD" : "SHRD" );
        decode_modrm( d, &m, sz );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        if ( m.is_mem ) op_str( rmbuf ); else op_str( reg_name( m.rm, sz ) );
        op_str( "," ); op_str( reg_name( m.reg, sz ) );
        op_str( "," ); op_hex( fetch( d ), 2 );
        if ( m.is_mem ) note_mem( d, &m, sz );
        return;
    }
    case 0xA5: case 0xAD: {                 /* SHLD/SHRD rm,reg,CL */
        save = oplen;
        set_mnem( op == 0xA5 ? "SHLD" : "SHRD" );
        decode_modrm( d, &m, sz );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        if ( m.is_mem ) op_str( rmbuf ); else op_str( reg_name( m.rm, sz ) );
        op_str( "," ); op_str( reg_name( m.reg, sz ) );
        op_str( ",CL" );
        if ( m.is_mem ) note_mem( d, &m, sz );
        return;
    }
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: {   /* CMOVcc */
        char nm[8];
        nm[0] = 'C'; nm[1] = 'M'; nm[2] = 'O'; nm[3] = 'V';
        str_cpy( nm + 4, cc_name[op - 0x40] );
        set_mnem( nm );
        save = oplen;
        decode_modrm( d, &m, sz );
        mem_cpy( rmbuf, ops + save, oplen - save + 1 );
        oplen = save; ops[oplen] = 0;
        op_str( reg_name( m.reg, sz ) ); op_str( "," );
        if ( m.is_mem ) { op_str( rmbuf ); note_mem( d, &m, sz ); } else op_str( reg_name( m.rm, sz ) );
        return;
    }
    }
    /* one DEBUG does not name: show the two bytes */
    set_mnem( "DB" );
    op_hex( 0x0F, 2 );
    op_str( " " );
    op_hex( op, 2 );
    d->pos--;                               /* only the 0F and this byte consumed */
    d->pos++;
}
