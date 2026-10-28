/*
 * RUN.C - G, T and P: running the held program and tracing it.
 *
 * These work only in MODE_NAT, on the D32 program the kernel holds for
 * DEBUG (21h/F1h AL=29h).  A 16-bit work area cannot be run here: the
 * kernel has no way to enter V86 code on DEBUG's behalf, and 6.22's
 * trick of planting an INT 3 and jumping to the code is exactly what
 * ring 3 exists to stop.  So G, T and P on a 16-bit area say so.
 *
 *   G [=start] [breakpoint...]   run; stop at a breakpoint or when it
 *                                ends.  Up to ten breakpoints, set by
 *                                an INT 3 written over the code and
 *                                taken out again afterwards
 *   T [=start] [count]           one instruction, or COUNT of them,
 *                                each shown; into a CALL or an INT
 *   P [=start] [count]           the same, but over a CALL, INT, LOOP
 *                                or a repeated string instruction
 *
 * When the program stops, its registers and the next instruction are
 * shown, as 6.22 showed them.  When it ends, "Program ended, exit code
 * NN" and DEBUG has nothing held until the next L.
 */
#include "debug.h"

#define MAX_BP 10

static void to_regs( DRX *regs )
{
    *regs = cpu;
}

static void from_regs( const DRX *regs )
{
    u32 limit = cpu.limit;
    cpu = *regs;
    if ( regs->vec != DRX_NOVEC && regs->limit == 0 ) {
        cpu.limit = limit;              /* a trap left the old one */
    }
    /* DEBUG owns the trace flag: each T sets it fresh and G must not
       inherit it, so it never stays in the registers between commands. */
    cpu.efl &= ~0x100UL;
}

/* only a native program runs; a 16-bit area is told so, and a native
   program that has ended has to be loaded again */
static int runnable( void )
{
    if ( mode != MODE_NAT || !prog_held ) {
        out_str( mode == MODE_NAT ? msg_noprog : msg_norun );
        out_nl();
        return 0;
    }
    return 1;
}

/* what the program stopped on, after a DBF_GO that stopped */
static void report_stop( void )
{
    out_nl();
    if ( cpu.vec == 1 ) {
        /* a trace or breakpoint step: nothing to say, just the state */
    } else if ( cpu.vec == 3 || ( cpu.vec == 13 && cpu.err == 0x1A ) ) {
        /* an INT 3 - a breakpoint of ours, already taken out */
    } else if ( cpu.vec == 13 ) {
        out_str( "Protection fault" );
        out_nl();
    } else if ( cpu.vec == 14 ) {
        out_str( cpu.addr < 0x1000 ? "Null pointer" : "Page fault" );
        out_str( " at " );
        out_hex( cpu.addr, 8 );
        out_nl();
    } else if ( cpu.vec == 0 ) {
        out_str( "Divide error" );
        out_nl();
    } else if ( cpu.vec == 6 ) {
        out_str( "Invalid opcode" );
        out_nl();
    }
    show_regs();
}

/* the program has ended: 4Dh says with what */
static void report_end( void )
{
    int code = sys_exit_code();

    prog_held = 0;
    out_nl();
    out_str( msg_ended );
    out_hex( code & 0xFF, 2 );
    out_nl();
}

/* run until it stops or ends; returns 1 if it is still held, -1 if the
   kernel refused the registers (a bad CS, SS or EIP) and nothing ran */
static int go_once( DRX *regs, int *ended )
{
    int stopped;

    *ended = 0;
    if ( sys_dbg_go( regs, &stopped ) ) {
        return -1;
    }
    if ( !stopped ) {
        *ended = 1;
        return 0;
    }
    return 1;
}

void cmd_go( void )
{
    DRX  regs;
    ADDR start, bp[MAX_BP];
    u8   saved[MAX_BP];
    int  nbp = 0, i, ended, refused;
    u8   cc = 0xCC;

    if ( !runnable() ) {
        return;
    }
    skip_blanks();
    if ( *lp == '=' ) {
        lp++;
        get_addr( &start, cpu.cs );
        cpu.eip = start.off;
    }
    while ( !at_end() && nbp < MAX_BP ) {
        get_addr( &bp[nbp], cpu.cs );
        nbp++;
        skip_blanks();
    }
    need_end();

    for ( i = 0; i < nbp; i++ ) {
        if ( sys_dbg_read( bp[i].off, &saved[i], 1 ) == 0 ) {
            sys_dbg_write( bp[i].off, &cc, 1 );
        } else {
            saved[i] = 0;
        }
    }
    to_regs( &regs );
    break_quiet = 0;
    refused = go_once( &regs, &ended ) < 0;
    if ( !ended && !refused ) {
        from_regs( &regs );
        /* an INT 3 stops one byte past it: step EIP back onto the
           breakpoint, as DEBUG left CS:IP on the planted byte */
        if ( cpu.vec == 3 || ( cpu.vec == 13 && cpu.err == 0x1A ) ) {
            for ( i = 0; i < nbp; i++ ) {
                if ( bp[i].off == cpu.eip - 1 ) {
                    cpu.eip--;
                    break;
                }
            }
        }
    }
    /* backwards: a breakpoint given twice saved the INT 3 of the first */
    for ( i = nbp - 1; i >= 0; i-- ) {
        sys_dbg_write( bp[i].off, &saved[i], 1 );
    }
    if ( refused ) {
        syntax_error();
    }
    if ( ended ) {
        report_end();
        return;
    }
    report_stop();
}

/* one instruction with the trap flag */
static int trace_flag_step( DRX *regs, int *ended )
{
    int held;

    regs->efl |= 0x100;
    held = go_once( regs, ended );
    if ( held < 0 ) {
        syntax_error();
    }
    return held;
}

/* step OVER the instruction at EIP with a breakpoint on the one after
   it, run without the trace flag.  Returns 0 and *ended if it never
   comes back (the instruction ended the program or jumped away). */
static int step_over( DRX *regs, u32 len, int *ended )
{
    u8  cc = 0xCC, saved;
    u32 after = regs->eip + len;
    int stopped, refused;

    if ( sys_dbg_read( after, &saved, 1 ) ) {
        return trace_flag_step( regs, ended );      /* cannot plant it */
    }
    sys_dbg_write( after, &cc, 1 );
    regs->efl &= ~0x100UL;
    refused = sys_dbg_go( regs, &stopped );
    sys_dbg_write( after, &saved, 1 );
    if ( refused ) {
        syntax_error();
    }
    if ( !stopped ) {
        *ended = 1;
        return 0;
    }
    if ( regs->eip == after + 1 ) {
        regs->eip = after;              /* back onto the planted byte */
    }
    *ended = 0;
    return 1;
}

/* one step.  OVER: step over a CALL, LOOP or REP (that is P).  An INT
   is ALWAYS stepped over, by both T and P: its handler is the kernel
   at ring 0, which a ring-3 program cannot trace into - so tracing one
   would clear TF on the way in and stop a whole instruction late. */
static int one_step( DRX *regs, int over, int *ended )
{
    u8      code[DIS_MAX];
    DISASM  dis;
    ADDR    at;

    at.seg = (u16)regs->cs;
    at.off = regs->eip;
    mem_read( at, code, DIS_MAX );
    disasm( at, code, 1, &dis );
    if ( dis.kind == DK_INT || dis.kind == DK_INT3 ||
         ( over && ( dis.kind == DK_CALL || dis.kind == DK_LOOP || dis.kind == DK_REP ) ) ) {
        return step_over( regs, dis.len, ended );
    }
    return trace_flag_step( regs, ended );
}

void cmd_trace( int proceed )
{
    DRX  regs;
    ADDR start;
    u32  count = 1;
    int  ended;

    if ( !runnable() ) {
        return;
    }
    skip_blanks();
    if ( *lp == '=' ) {
        lp++;
        get_addr( &start, cpu.cs );
        cpu.eip = start.off;
    }
    skip_blanks();
    if ( !at_end() ) {
        count = get_hex( 8 );
    }
    need_end();
    if ( count == 0 ) {
        return;
    }

    break_quiet = 0;
    while ( count-- ) {
        to_regs( &regs );
        one_step( &regs, proceed, &ended );
        if ( ended ) {
            report_end();
            return;
        }
        from_regs( &regs );
        report_stop();
        if ( break_hit ) {
            break;
        }
    }
}

void prog_release( void )
{
    if ( prog_held ) {
        sys_dbg_kill();
        prog_held = 0;
    }
}
