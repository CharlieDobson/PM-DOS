/*
 * RTMAIN.C - QBRUN.EXE: the part of QBASIC that runs a program, without
 * the editor or the compiler.  Make EXE File writes a copy of QBRUN.EXE
 * with the compiled program behind it (PACK.C); the kernel loads the
 * whole file, so the program is at d32_tail when the copy starts.
 * QBRUN.EXE with nothing behind it says what it is for and ends.
 *
 * IT ENDS AS A PROGRAM QUICKBASIC 4.5 COMPILED DOES:
 *   - END and SYSTEM go straight back to DOS; a graphics screen is
 *     cleared to text on the way.
 *   - An error nothing traps, STOP, and Ctrl+Break print their line on
 *     the program's screen, then "Hit any key to return to system", and
 *     the exit code is 0 all the same.
 *   - Only a program made with Produce Debug Code stops for Ctrl+Break,
 *     and only its messages give the BASIC line number (as ERL would).
 *     QuickBASIC's "at address" means nothing here and is left out.
 *   - CHAIN and RUN with a name go to another .EXE.  One that Make EXE
 *     File made for this instruction set is read and run here, keeping
 *     the COMMON and the open files as CHAIN does in QBasic; any other
 *     program is started as SHELL starts one, and this one ends.
 * One difference stays: subscripts are always checked, as in QBasic.
 */
#include "qb.h"

extern u8 d32_tail[];           /* DOSINT.ASM: the first byte past the image */

int opt_mbf;
int ide_trace;                  /* no editor: TRON changes nothing */

static char module[PATH_MAX];   /* the .BAS file's name, for the messages */
static int  debug_code;
static int  screen_kept;         /* con_capture_dos() has run */

static void say( const char *text )
{
    sys_stdout( text, str_len( text ) );
}

static void con_say( const char *text )
{
    con_write( text, str_len( text ) );
}

void out_of_memory( void )
{
    if ( screen_kept ) {
        con_exit();
    }
    say( "Out of memory\r\n" );
    sys_exit( 7 );
}

/* back to DOS: the files closed, the screen as DOS takes it */
static void leave( void )
{
    file_flush_all();
    vm_end();
    con_exit();
    sys_exit( 0 );
}

/* the BASIC line number of a statement, for a message: 0 for none */
static s32 line_of( PROGRAM *prog, u32 stmt )
{
    if ( !debug_code || prog == NULL || stmt >= prog->nlocs ) {
        return 0;
    }
    return prog->locs[stmt].lineno;
}

/* the run stopped (an error, STOP, Ctrl+Break): its line, a key, DOS */
static void report( int why, int code, s32 lineno )
{
    char num[12];

    file_flush_all();
    con_no_break = 1;
    if ( con_col() != 1 ) {
        con_newline();
    }
    if ( why == RUN_BREAK ) {
        con_say( "*Break*" );
    } else {
        con_newline();
        if ( why == RUN_STOP ) {
            con_newline();
            con_say( "STOP" );
        } else {
            con_say( err_message( code ) );
        }
        if ( lineno > 0 ) {
            num_fmt_l( lineno, num );
            con_say( " in line " );
            con_say( num );
            con_say( " of module " );
        } else {
            con_say( " in module " );
        }
        con_say( module );
        con_newline();
    }
    con_newline();
    con_say( "Hit any key to return to system" );
    con_flush();
    con_flush_keys();
    while ( con_key( 1 ) < 0 ) {
    }
    con_newline();                              /* CMD.EXE's prompt goes under it */
    leave();
}

/* The program behind another file (CHAIN, RUN "name") -> its block, to
   xfree(); NULL and *rc a DOS error, or -1 when the file has no program
   this runtime can run */
static u8 *read_program( const char *path, u32 *len, int *rc )
{
    u32 handle, size, done, blen = 0;
    u8 tail[8], *blob = NULL;

    *rc = sys_open_read( path, &handle );
    if ( *rc != 0 ) {
        return NULL;
    }
    *rc = sys_file_size( handle, &size );
    if ( *rc == 0 && size < sizeof( tail ) ) {
        *rc = -1;
    }
    if ( *rc == 0 ) {
        *rc = sys_seek( handle, size - sizeof( tail ) );
    }
    if ( *rc == 0 ) {
        *rc = sys_read( handle, tail, sizeof( tail ), &done );
        if ( *rc == 0 && done != sizeof( tail ) ) {
            *rc = -1;
        }
    }
    if ( *rc == 0 ) {
        blen = pack_tail_length( tail );
        if ( blen <= sizeof( tail ) || blen > size ) {
            *rc = -1;
        }
    }
    if ( *rc == 0 ) {
        *rc = sys_seek( handle, size - blen );
    }
    if ( *rc == 0 ) {
        blob = (u8 *)try_alloc( blen );
        if ( blob == NULL ) {
            *rc = ERR_NOMEM;
        }
    }
    if ( *rc == 0 ) {
        *rc = sys_read( handle, blob, blen, &done );
        if ( *rc == 0 && done != blen ) {
            *rc = -1;
        }
    }
    sys_close( handle );
    if ( *rc == 0 && pack_length( blob, blen ) != blen ) {
        *rc = -1;
    }
    if ( *rc != 0 ) {
        xfree( blob );
        return NULL;
    }
    *len = blen;
    return blob;
}

void qbasic_main( void )
{
    const u8 *blob = d32_tail;
    u8 *owned = NULL, *next_blob;
    u32 blen, next_len = 0, flags, run_at = 0, i;
    PROGRAM *prog;
    int why, rc;
    s32 lineno;
    char next[PATH_MAX];

    sys_init();
    sys_break_install();
    blen = pack_length( d32_tail, 0x7FFFFFFFUL );
    if ( blen == 0 ) {
        say( "QBRUN.EXE is used by QBasic's Make EXE File command and does not run by itself.\r\n" );
        sys_exit( 1 );
    }
    if ( !sys_fpu_present() ) {
        say( "This program needs a math coprocessor: a 486DX or better, or a 386 with a 387.\r\n" );
        sys_exit( 1 );
    }
    fp_init();
    chain_ext = ".EXE";
    con_capture_dos();
    screen_kept = 1;
    for ( ;; ) {
        prog = pack_read( blob, blen, &flags, module, sizeof( module ), &rc );
        if ( prog == NULL ) {
            con_exit();
            say( rc == PK_NOMEM ? "Out of memory\r\n" : "The program in this file is damaged.\r\n" );
            sys_exit( 1 );
        }
        opt_mbf = (flags & PK_MBF) != 0;
        debug_code = (flags & PK_DEBUG) != 0;
        con_no_break = !debug_code;
        if ( !vm_begin( prog ) ) {
            pack_free( prog );
            out_of_memory();
        }
        if ( run_at != 0 ) {
            /* RUN n: from the line's code */
            vm.pc = prog->code + run_at - 1;
            for ( i = 0; i < prog->nlocs; i++ ) {
                if ( prog->locs[i].pc == run_at - 1 ) {
                    vm.stmt = i;
                    break;
                }
            }
            run_at = 0;
        }
        con_reset();
        vm_common_give();
        why = vm_go( GO_RUN );
        if ( why != RUN_CHAIN ) {
            break;
        }
        file_flush_all();
        if ( vm.chain_to[0] == 0 ) {
            /* RUN, RUN n: this program again, from nothing */
            run_at = vm.chain_at;
            vm_end();
            pack_free( prog );
            continue;
        }
        str_cpy( next, vm.chain_to );
        lineno = line_of( prog, vm.stmt );
        if ( vm.chain_keep ) {
            vm_common_take();
        }
        next_blob = read_program( next, &next_len, &rc );
        vm_end();
        pack_free( prog );
        if ( next_blob == NULL ) {
            vm_common_drop();
            if ( rc == -1 ) {
                /* not one of ours: started as SHELL starts a program, and
                   this one is over */
                con_exit();
                rc = sys_exec( next, "", NULL );
                if ( rc == 0 ) {
                    sys_exit( 0 );
                }
                con_show();
            }
            report( RUN_ERROR, rc == ERR_NOMEM ? E_OUT_OF_MEMORY : rc == ERR_NOFILE ? E_FILE_NOT_FOUND : E_BAD_FILENAME,
                    lineno );
        }
        xfree( owned );
        owned = next_blob;
        blob = owned;
        blen = next_len;
    }
    switch ( why ) {
    case RUN_ERROR:
        report( RUN_ERROR, vm.fatal, line_of( prog, vm.fatal_stmt ) );
        break;
    case RUN_STOP:
        report( RUN_STOP, 0, line_of( prog, vm.stmt ) );
        break;
    case RUN_BREAK:
        report( RUN_BREAK, 0, 0 );
        break;
    }
    leave();
}
