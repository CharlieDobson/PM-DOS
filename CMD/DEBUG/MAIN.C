/*
 * MAIN.C - DEBUG: the start, the command loop and the parser every
 * command reads its line with.
 *
 *   DEBUG [[drive:][path]filename [parameters]]
 *
 * MS-DOS 6.22'S DEBUG IS THE MODEL, measured by running it under
 * DOSBox-X's own DOS (VER=6.22) with its commands fed from a file:
 *
 *   - the prompt is a hyphen; a command is one letter, in either case,
 *     and its parameters may be run together with it ("d100") or
 *     separated by blanks or commas
 *   - numbers are hexadecimal, four digits at most for an offset or a
 *     segment; the fifth is where the error is
 *   - an error is "^ Error" with the caret under the character DEBUG
 *     could not take, which is often the end of the line; R's are two
 *     letters in front ("br Error", "bf Error", "df Error")
 *   - a range is an address and either a second offset or L and a
 *     length, and may not run off the end of its segment - but L 0 is
 *     64K from wherever it starts, going round the segment, and C's and
 *     M's second addresses go round it too
 *   - with no file the work area is a .COM's: a PSP at offset 0, the
 *     registers at their DOS values, IP 0100, SP FFEE
 *   - a file named on the command line is loaded as L would load it,
 *     and what follows its name is the program's command tail and its
 *     two FCBs
 *
 * WHAT IS PM-DOS'S: the program DEBUG can run is a native one (DEBUG.H
 * has the two modes), its registers are 32-bit and its offsets eight
 * digits; the first megabyte can be read and not written; and at the
 * end of a file of commands DEBUG ends, where 6.22's waited for ever.
 */
#include "debug.h"

int   mode = MODE_16;
DRX   cpu;
u16   work_seg;
u32   work_len;
u8   *work_buf;
int   prog_held;
char  file_name[128];
int   file_kind;
ADDR  next_dump;                /* where a bare D, U and A carry on */
ADDR  next_unasm;
ADDR  next_asm;

const char *line;
const char *lp;
int   prompt_len;
u32  *error_jmp;

static char cmd_line[256];

void __cdecl loop_run( void (*body)( void ) );

void quit( int code )
{
    out_flush();
    prog_release();
    sys_exit( code );
}

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

void syntax_error( void )
{
    out_flush();
    out_spaces( prompt_len + (int)( lp - line ) );
    out_str( msg_error );
    out_nl();
    if ( error_jmp ) {
        dbg_longjmp( error_jmp, 1 );
    }
    cmd_abort();
}

void coded_error( const char *code )
{
    out_flush();
    out_str( code );
    out_ch( ' ' );
    out_str( msg_error + 2 );       /* "Error", without "^ " */
    out_nl();
    if ( error_jmp ) {
        dbg_longjmp( error_jmp, 1 );
    }
    cmd_abort();
}

/* ------------------------------------------------------------------ */
/* the parser                                                          */
/* ------------------------------------------------------------------ */

static int is_blank( int ch )
{
    return ch == ' ' || ch == '\t' || ch == ',';
}

void skip_blanks( void )
{
    while ( is_blank( *lp ) ) {
        lp++;
    }
}

int at_end( void )
{
    skip_blanks();
    return *lp == 0 || *lp == '\r' || *lp == '\n';
}

void need_end( void )
{
    if ( !at_end() ) {
        syntax_error();
    }
}

int peek_upper( void )
{
    return ch_upper( *lp );
}

static int hex_value( int ch )
{
    ch = ch_upper( ch );
    if ( ch >= '0' && ch <= '9' ) {
        return ch - '0';
    }
    if ( ch >= 'A' && ch <= 'F' ) {
        return ch - 'A' + 10;
    }
    return -1;
}

int off_digits( void )
{
    return mode == MODE_NAT ? 8 : 4;
}

/* A hex number of at most MAX_DIGITS digits.  0 if there is none here
   (lp unmoved); a digit too many is an error, under that digit. */
int try_hex( u32 *val, int max_digits )
{
    int digits = 0;
    int dig;

    skip_blanks();
    *val = 0;
    while ( ( dig = hex_value( *lp ) ) >= 0 ) {
        if ( digits == max_digits ) {
            syntax_error();
        }
        *val = ( *val << 4 ) | (u32)dig;
        digits++;
        lp++;
    }
    return digits != 0;
}

u32 get_hex( int max_digits )
{
    u32 val;

    if ( !try_hex( &val, max_digits ) ) {
        syntax_error();
    }
    return val;
}

/* A segment register's name -> its value.  FS and GS are a native
   program's only. */
int reg_seg_value( const char *name, u16 *val )
{
    int ch0 = ch_upper( name[0] );

    if ( ch_upper( name[1] ) != 'S' ) {
        return 0;
    }
    switch ( ch0 ) {
    case 'C': *val = cpu.cs; return 1;
    case 'D': *val = cpu.ds; return 1;
    case 'E': *val = cpu.es; return 1;
    case 'S': *val = cpu.ss; return 1;
    case 'F': if ( mode != MODE_NAT ) return 0; *val = cpu.fs; return 1;
    case 'G': if ( mode != MODE_NAT ) return 0; *val = cpu.gs; return 1;
    }
    return 0;
}

/* [segment:]offset.  The segment is a register's name or a number, and
   an offset alone takes DEF_SEG.  0 if there is no address here at all,
   and ADDR is left as it was. */
int try_addr( ADDR *addr, u16 def_seg )
{
    u32 val;
    u16 seg;
    const char *start;

    skip_blanks();
    start = lp;
    if ( lp[0] && lp[1] && lp[2] == ':' && reg_seg_value( lp, &seg ) ) {
        addr->seg = seg;
        lp += 3;
        addr->off = get_hex( mem_is_low( *addr ) ? 4 : off_digits() );
        return 1;
    }
    if ( !try_hex( &val, off_digits() ) ) {
        return 0;
    }
    if ( *lp == ':' ) {
        if ( val > 0xFFFF ) {
            lp = start + 4;
            syntax_error();
        }
        lp++;
        addr->seg = (u16)val;
        addr->off = get_hex( mem_is_low( *addr ) ? 4 : off_digits() );
        return 1;
    }
    addr->seg = def_seg;                /* an offset alone */
    addr->off = val;
    if ( mem_is_low( *addr ) && val > 0xFFFF ) {
        syntax_error();
    }
    return 1;
}

void get_addr( ADDR *addr, u16 def_seg )
{
    if ( !try_addr( addr, def_seg ) ) {
        syntax_error();
    }
}

/* address [offset | L length].  NEED: there must be an address.  With
   none, ADDR is left as the caller set it and LEN is DEF_LEN.  A range
   stops at the end of its segment unless it was asked to go past it,
   which is an error - except L 0 in the first megabyte, which is 64K
   from any offset, round the segment, because to 6.22 a length of 0 was
   a count of 64K that its end check could not see. */
void get_range( ADDR *addr, u32 *len, u16 def_seg, u32 def_len, int need )
{
    u32 end, val, seg_end;
    int have;

    have = try_addr( addr, def_seg );
    if ( !have && need ) {
        syntax_error();
    }
    seg_end = mem_seg_end( *addr );
    if ( addr->off >= seg_end ) {
        syntax_error();
    }
    skip_blanks();
    if ( have && ch_upper( *lp ) == 'L' ) {
        lp++;
        val = get_hex( off_digits() );
        if ( val == 0 ) {
            val = 0x10000UL;
            if ( mem_is_low( *addr ) ) {
                *len = val;
                return;
            }
        }
        if ( val > seg_end - addr->off ) {
            syntax_error();
        }
        *len = val;
        return;
    }
    if ( have && try_hex( &end, off_digits() ) ) {
        if ( end < addr->off || end >= seg_end ) {
            syntax_error();
        }
        *len = end - addr->off + 1;
        return;
    }
    *len = def_len;
    if ( *len > seg_end - addr->off ) {
        *len = seg_end - addr->off;
    }
}

/* Bytes and strings: "12 34 'text' "more"", into BUF.  The count. */
u32 get_list( u8 *buf, u32 max )
{
    u32 count = 0;
    u32 val;
    int quote;

    for ( ;; ) {
        skip_blanks();
        if ( at_end() ) {
            break;
        }
        if ( *lp == '\'' || *lp == '"' ) {
            quote = *lp++;
            for ( ;; ) {
                if ( *lp == 0 ) {
                    syntax_error();
                }
                if ( *lp == quote ) {
                    if ( lp[1] != quote ) {
                        lp++;
                        break;
                    }
                    lp++;
                }
                if ( count == max ) {
                    syntax_error();
                }
                buf[count++] = (u8)*lp++;
            }
            continue;
        }
        if ( !try_hex( &val, 2 ) ) {
            syntax_error();
        }
        if ( count == max ) {
            syntax_error();
        }
        buf[count++] = (u8)val;
    }
    if ( count == 0 ) {
        syntax_error();
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* the work area, and the name for L and W                             */
/* ------------------------------------------------------------------ */

static u8 prog_tail[128];       /* the tail as DOS keeps it, for a native load */

const u8 *native_tail( void )
{
    return prog_tail;
}

/* One name from TEXT into a FCB of 16 bytes: a drive, eight and three
   characters, blank filled, * as question marks.  The text after it. */
static const char *make_fcb( const char *text, u8 *fcb )
{
    int pos;

    mem_set( fcb, 0, 16 );
    mem_set( fcb + 1, ' ', 11 );
    while ( *text == ' ' || *text == '\t' ) {
        text++;
    }
    if ( text[0] && text[1] == ':' ) {
        int drv = ch_upper( text[0] );
        if ( drv >= 'A' && drv <= 'Z' ) {
            fcb[0] = (u8)( drv - 'A' + 1 );
        }
        text += 2;
    }
    for ( pos = 0; *text && *text != ' ' && *text != '\t' && *text != '.' && *text != '/'; text++ ) {
        if ( *text == '\\' || *text == ':' ) {
            pos = 0;                    /* a path: the name is its last part */
            mem_set( fcb + 1, ' ', 8 );
            continue;
        }
        if ( *text == '*' ) {
            while ( pos < 8 ) fcb[1 + pos++] = '?';
            continue;
        }
        if ( pos < 8 ) {
            fcb[1 + pos++] = (u8)ch_upper( *text );
        }
    }
    if ( *text == '.' ) {
        text++;
        for ( pos = 0; *text && *text != ' ' && *text != '\t' && *text != '/'; text++ ) {
            if ( *text == '*' ) {
                while ( pos < 3 ) fcb[9 + pos++] = '?';
                continue;
            }
            if ( pos < 3 ) {
                fcb[9 + pos++] = (u8)ch_upper( *text );
            }
        }
    }
    while ( *text && *text != ' ' && *text != '\t' ) {
        text++;
    }
    return text;
}

static int kind_of( const char *name )
{
    const char *dot = NULL;
    const char *cur;

    for ( cur = name; *cur; cur++ ) {
        if ( *cur == '.' ) {
            dot = cur;
        } else if ( *cur == '\\' || *cur == '/' || *cur == ':' ) {
            dot = NULL;
        }
    }
    if ( dot == NULL ) {
        return KIND_DATA;
    }
    if ( str_icmp( dot, ".EXE" ) == 0 ) return KIND_EXE;
    if ( str_icmp( dot, ".COM" ) == 0 ) return KIND_COM;
    if ( str_icmp( dot, ".HEX" ) == 0 ) return KIND_HEX;
    return KIND_DATA;
}

/* The PSP's FCBs and tail from ARGS, as DOS would build them for the
   program; and into the program's own PSP when one is held. */
static void put_tail( const char *args )
{
    u8 fcbs[0x24];
    u8 tail[128];
    const char *rest;
    u32 len;
    ADDR at;

    rest = make_fcb( args, fcbs );
    make_fcb( rest, fcbs + 0x10 );
    len = str_len( args );
    if ( len > 126 ) {
        len = 126;
    }
    tail[0] = (u8)len;
    mem_cpy( tail + 1, args, len );
    tail[1 + len] = '\r';
    mem_set( tail + 2 + len, 0, 126 - len );   /* to tail[127] */
    mem_cpy( prog_tail, tail, sizeof( prog_tail ) );

    /* A native program's PSP has no FCBs: 5Ch to 7Fh stay zero there,
       because there is no FCB API to read them (PMDOS.INC, PSP_). */
    if ( mode == MODE_NAT ) {
        if ( prog_held ) {
            at.seg = SEL_PROG_DS;
            at.off = SLOT_PSP + 0x80;
            mem_write( at, tail, 128 );
        }
        return;
    }
    at.seg = work_seg;
    at.off = 0x5C;
    mem_write( at, fcbs, 0x20 );        /* 5Ch and 6Ch, overlapping as DOS's */
    at.off = 0x80;
    mem_write( at, tail, 128 );
}

/* N's text, or the command line's: the name to load and write, then the
   tail.  From N the whole text is the tail and the FCBs are made from
   the name and the word after it; from the command line the tail is
   what follows the name, as DOS hands a program its arguments. */
void set_file_name( const char *text )
{
    const char *cur = text;
    u32 len = 0;

    while ( *cur == ' ' || *cur == '\t' ) {
        cur++;
    }
    while ( *cur && *cur != ' ' && *cur != '\t' && len + 1 < sizeof( file_name ) ) {
        file_name[len++] = *cur++;
    }
    file_name[len] = 0;
    file_kind = kind_of( file_name );
    put_tail( text );
}

static void set_args( const char *args )
{
    put_tail( args );
}

/* A fresh work area: zeroes, a PSP at offset 0, the registers DOS would
   start a .COM with. */
void new_work( void )
{
    mode = MODE_16;
    work_reset();
    regs_reset();
    positions_at_ip();
}

/* D, U and A carry on from CS:IP, as 6.22's do at the start and after a
   program is loaded.  A data file's load leaves them where they were. */
void positions_at_ip( void )
{
    next_dump.seg = next_unasm.seg = next_asm.seg = cpu.cs;
    next_dump.off = next_unasm.off = next_asm.off = cpu.eip;
}

/* ------------------------------------------------------------------ */
/* the command loop                                                    */
/* ------------------------------------------------------------------ */

void __cdecl break_reset( void )
{
    /* DOS has said ^C; the line it was in is finished with */
    break_quiet = 0;
    out_nl();
}

static void command_once( void )
{
    int len, cmd;

    /* A command that ended through cmd_abort - Ctrl+C at A's prompt is
       one - left the error_jmp of a frame that is gone */
    error_jmp = NULL;
    break_quiet = 0;
    break_hit = 0;
    prompt_len = 1;
    out_ch( '-' );
    out_flush();
    len = sys_read_line( cmd_line, sizeof( cmd_line ) );
    if ( len < 0 ) {
        quit( 0 );                      /* the commands have run out */
    }
    line = cmd_line;
    lp = cmd_line;
    skip_blanks();
    if ( at_end() ) {
        return;
    }
    cmd = ch_upper( *lp++ );
    switch ( cmd ) {
    case 'A': cmd_assemble();       break;
    case 'C': cmd_compare();        break;
    case 'D': cmd_dump();           break;
    case 'E': cmd_enter();          break;
    case 'F': cmd_fill();           break;
    case 'G': cmd_go();             break;
    case 'H': cmd_hex();            break;
    case 'I': cmd_input();          break;
    case 'L': cmd_load();           break;
    case 'M': cmd_move();           break;
    case 'N': cmd_name();           break;
    case 'O': cmd_output();         break;
    case 'P': cmd_trace( 1 );       break;
    case 'Q': need_end(); quit( 0 ); break;
    case 'R': cmd_register();       break;
    case 'S': cmd_search();         break;
    case 'T': cmd_trace( 0 );       break;
    case 'U': cmd_unassemble();     break;
    case 'W': cmd_write();          break;
    case 'X': cmd_ems();            break;
    case '?': cmd_help();           break;
    default:
        lp--;
        syntax_error();
    }
}

void __cdecl dos_entry( void )
{
    const char *args;
    const char *rest;

    sys_init();

    /* The work area reaches from where a DOS box's program would be
       loaded to the top of conventional memory, as 6.22's reached to
       the top of the free memory it was given. */
    work_seg = sys_conv_seg();
    work_len = 0xA0000UL - (u32)work_seg * 16;
    work_buf = (u8 *)try_alloc( work_len );
    if ( work_buf == NULL ) {
        work_len = 0x20000UL;
        work_buf = (u8 *)try_alloc( work_len );
    }
    if ( work_buf == NULL ) {
        out_err( msg_nomem );
        sys_exit( 1 );
    }
    sys_break_install();
    new_work();

    args = sys_cmdline();
    while ( *args == ' ' || *args == '\t' ) {
        args++;
    }
    if ( *args ) {
        set_file_name( args );
        for ( rest = args; *rest && *rest != ' ' && *rest != '\t'; rest++ ) {
        }
        set_args( rest );
        line = "";
        lp = line;
        prompt_len = 0;
        load_file( NULL );
        if ( mode == MODE_NAT ) {
            set_args( rest );           /* into the program's own PSP */
        }
    }
    loop_run( command_once );
}
