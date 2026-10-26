/*
 * REGS.C - R: the registers shown, and one of them changed.
 *
 * 6.22's display is kept to the column: eight registers, then the
 * segments, IP and the eight flags by their two-letter names, then the
 * instruction at CS:IP with the memory it would touch.  A native
 * program's registers are 32-bit and need three lines; the names are
 * the same, with E in front.
 *
 *   R          show them
 *   R name     show one, and take a new value for it after ":"
 *   R F        show the flags, and take names to set after "-"
 *
 * A wrong name is "br Error"; a wrong flag "bf Error"; a flag given
 * twice "df Error".  In a native program AX names the low half of EAX,
 * as it does in the instruction set.
 */
#include "debug.h"

static const char flag_set[]   = "OVDNEINGZRACPECY";
static const char flag_clear[] = "NVUPDIPLNZNAPONC";
static const u32  flag_bits[]  = { 0x800, 0x400, 0x200, 0x80, 0x40, 0x10, 0x04, 0x01 };

typedef struct {
    const char *name;
    int  slot;                  /* index in the u32 part of DRX, or SEG_n */
    int  width;                 /* 16 or 32 */
} REGNAME;

#define R_EAX 0
#define R_EBX 1
#define R_ECX 2
#define R_EDX 3
#define R_ESI 4
#define R_EDI 5
#define R_EBP 6
#define R_ESP 7
#define R_EIP 8
#define S_CS  20
#define S_DS  21
#define S_ES  22
#define S_SS  23
#define S_FS  24
#define S_GS  25

static const REGNAME names[] = {
    { "AX", R_EAX, 16 }, { "BX", R_EBX, 16 }, { "CX", R_ECX, 16 }, { "DX", R_EDX, 16 },
    { "SP", R_ESP, 16 }, { "BP", R_EBP, 16 }, { "SI", R_ESI, 16 }, { "DI", R_EDI, 16 },
    { "IP", R_EIP, 16 }, { "PC", R_EIP, 16 },
    { "DS", S_DS, 16 },  { "ES", S_ES, 16 },  { "SS", S_SS, 16 },  { "CS", S_CS, 16 },
    { "EAX", R_EAX, 32 }, { "EBX", R_EBX, 32 }, { "ECX", R_ECX, 32 }, { "EDX", R_EDX, 32 },
    { "ESP", R_ESP, 32 }, { "EBP", R_EBP, 32 }, { "ESI", R_ESI, 32 }, { "EDI", R_EDI, 32 },
    { "EIP", R_EIP, 32 }, { "FS", S_FS, 16 },  { "GS", S_GS, 16 },
    { NULL, 0, 0 }
};

static u32 *gpr( int slot )
{
    switch ( slot ) {
    case R_EAX: return &cpu.eax;
    case R_EBX: return &cpu.ebx;
    case R_ECX: return &cpu.ecx;
    case R_EDX: return &cpu.edx;
    case R_ESI: return &cpu.esi;
    case R_EDI: return &cpu.edi;
    case R_EBP: return &cpu.ebp;
    case R_ESP: return &cpu.esp;
    }
    return &cpu.eip;
}

static u16 *sreg( int slot )
{
    switch ( slot ) {
    case S_CS: return &cpu.cs;
    case S_DS: return &cpu.ds;
    case S_ES: return &cpu.es;
    case S_SS: return &cpu.ss;
    case S_FS: return &cpu.fs;
    }
    return &cpu.gs;
}

/* the registers DOS starts a .COM with, in the work area */
void regs_reset( void )
{
    mem_set( &cpu, 0, sizeof( cpu ) );
    cpu.esp = 0xFFEE;
    cpu.eip = 0x0100;
    cpu.efl = 0x0202;
    cpu.cs = cpu.ds = cpu.es = cpu.ss = work_seg;
    cpu.limit = 0x10000UL;
    cpu.vec = DRX_NOVEC;
}

static void out_flags( void )
{
    int i;

    for ( i = 0; i < 8; i++ ) {
        const char *src = ( cpu.efl & flag_bits[i] ) ? flag_set : flag_clear;
        out_ch( src[i * 2] );
        out_ch( src[i * 2 + 1] );
        if ( i < 7 ) out_ch( ' ' );
    }
}

static void out_reg( const char *name, u32 val, int digits )
{
    out_str( name );
    out_ch( '=' );
    out_hex( val, digits );
    out_str( "  " );
}

void show_regs( void )
{
    ADDR at;

    if ( mode == MODE_NAT ) {
        out_reg( "EAX", cpu.eax, 8 );
        out_reg( "EBX", cpu.ebx, 8 );
        out_reg( "ECX", cpu.ecx, 8 );
        out_reg( "EDX", cpu.edx, 8 );
        out_reg( "ESP", cpu.esp, 8 );
        out_nl();
        out_reg( "EBP", cpu.ebp, 8 );
        out_reg( "ESI", cpu.esi, 8 );
        out_reg( "EDI", cpu.edi, 8 );
        out_reg( "DS", cpu.ds, 4 );
        out_reg( "ES", cpu.es, 4 );
        out_reg( "SS", cpu.ss, 4 );
        out_reg( "CS", cpu.cs, 4 );
        out_nl();
        out_reg( "FS", cpu.fs, 4 );
        out_reg( "GS", cpu.gs, 4 );
        out_reg( "EIP", cpu.eip, 8 );
        out_ch( ' ' );
        out_flags();
        out_nl();
    } else {
        out_reg( "AX", cpu.eax, 4 );
        out_reg( "BX", cpu.ebx, 4 );
        out_reg( "CX", cpu.ecx, 4 );
        out_reg( "DX", cpu.edx, 4 );
        out_reg( "SP", cpu.esp, 4 );
        out_reg( "BP", cpu.ebp, 4 );
        out_reg( "SI", cpu.esi, 4 );
        out_reg( "DI", cpu.edi, 4 );
        out_nl();
        out_reg( "DS", cpu.ds, 4 );
        out_reg( "ES", cpu.es, 4 );
        out_reg( "SS", cpu.ss, 4 );
        out_reg( "CS", cpu.cs, 4 );
        out_reg( "IP", cpu.eip, 4 );
        out_ch( ' ' );
        out_flags();
        out_ch( ' ' );
        out_nl();
    }
    at.seg = cpu.cs;
    at.off = cpu.eip;
    show_insn( at, 1 );
    next_unasm = at;                    /* a bare U goes on from here */
}

/* a new value after ":" - nothing typed leaves it as it was */
static int read_value( u32 *val, int digits )
{
    static char buf[80];
    const char *save_line = line;
    const char *save_lp = lp;
    int got;

    out_ch( ':' );
    out_flush();
    if ( sys_read_line( buf, sizeof( buf ) ) < 0 ) {
        quit( 0 );
    }
    line = buf;
    lp = buf;
    prompt_len = 1;
    got = try_hex( val, digits );
    need_end();
    line = save_line;
    lp = save_lp;
    return got;
}

static void reg_flags( void )
{
    static char buf[80];
    u32 set = 0, clear = 0, bit;
    int i, found;

    out_flags();
    out_str( "  -" );
    out_flush();
    if ( sys_read_line( buf, sizeof( buf ) ) < 0 ) {
        quit( 0 );
    }
    line = buf;
    lp = buf;
    for ( ;; ) {
        skip_blanks();
        if ( at_end() ) {
            break;
        }
        found = 0;
        for ( i = 0; i < 8 && !found; i++ ) {
            bit = flag_bits[i];
            if ( ch_upper( lp[0] ) == flag_set[i * 2] && ch_upper( lp[1] ) == flag_set[i * 2 + 1] ) {
                if ( ( set | clear ) & bit ) coded_error( "df" );
                set |= bit;
                found = 1;
            } else if ( ch_upper( lp[0] ) == flag_clear[i * 2] && ch_upper( lp[1] ) == flag_clear[i * 2 + 1] ) {
                if ( ( set | clear ) & bit ) coded_error( "df" );
                clear |= bit;
                found = 1;
            }
        }
        if ( !found ) {
            coded_error( "bf" );
        }
        lp += 2;
    }
    cpu.efl = ( cpu.efl & ~clear ) | set;
}

void cmd_register( void )
{
    char name[4];
    int len = 0;
    const REGNAME *reg;
    u32 val, *full;
    u16 *seg;

    skip_blanks();
    while ( len < 3 && ( ( *lp >= 'A' && *lp <= 'Z' ) || ( *lp >= 'a' && *lp <= 'z' ) ) ) {
        name[len++] = (char)ch_upper( *lp++ );
    }
    name[len] = 0;
    if ( len == 0 ) {
        need_end();
        show_regs();
        return;
    }
    if ( !at_end() ) {
        coded_error( "br" );
    }
    if ( len == 1 && name[0] == 'F' ) {
        reg_flags();
        return;
    }
    for ( reg = names; reg->name; reg++ ) {
        if ( str_icmp( reg->name, name ) == 0 ) {
            break;
        }
    }
    if ( reg->name == NULL ) {
        coded_error( "br" );
    }
    if ( mode != MODE_NAT && ( reg->width == 32 || reg->slot == S_FS || reg->slot == S_GS ) ) {
        coded_error( "br" );
    }

    out_str( reg->name );
    out_ch( ' ' );
    if ( reg->slot >= S_CS ) {
        seg = sreg( reg->slot );
        out_hex( *seg, 4 );
        out_nl();
        if ( read_value( &val, 4 ) ) {
            *seg = (u16)val;
        }
        return;
    }
    full = gpr( reg->slot );
    if ( reg->width == 32 ) {
        out_hex( *full, 8 );
        out_nl();
        if ( read_value( &val, 8 ) ) {
            *full = val;
        }
        return;
    }
    out_hex( *full & 0xFFFF, 4 );
    out_nl();
    if ( read_value( &val, 4 ) ) {
        *full = ( *full & 0xFFFF0000UL ) | val;
    }
}
