/*
 * HOST.C - QBHOST.EXE: a QBasic program run from a Windows console, for
 * checking the interpreter against real QBasic's results.
 *
 *   QBHOST [-root dir] [-keys file] [-screen file] [-list] [-pack]
 *          [-make out.exe qbrun.exe [-nodebug]] program.bas
 *
 * -root    the folder that stands for C:\ (default: the current one)
 * -keys    the keys a program reads, one a line (SYS_NT.C says how)
 * -screen  the text screen at the end, written to a file, a row a line
 * -list    each line as QBasic would show it, instead of running
 * -pack    the compiled program packed as Make EXE File packs it, and
 *          what PACK.C reads back run in its place
 * -make    out.exe qbrun.exe: what Make EXE File writes, written, and
 *          nothing run (-nodebug: without Produce Debug Code)
 *
 * A compile error is printed with its procedure, line and column, and
 * the exit code is 2; a run-time error the program did not trap is
 * printed and the exit code is 1.
 *
 * Not built by BUILD.CMD; MKHOST.CMD builds it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qb.h"

int opt_mbf;
int ide_trace;                  /* no IDE here: TRON changes nothing */

void host_set_root( const char *root );
void host_set_keys( const char *path );
u16 *host_screen( int *rows, int *cols );

void out_of_memory( void )
{
    fprintf( stderr, "qbhost: out of memory\n" );
    exit( 4 );
}

static const char *text_name( int text )
{
    return text == 0 ? "(module)" : ptext.texts[text]->pname;
}

static void dump_screen( const char *path )
{
    FILE *fp = fopen( path, "w" );
    int rows, cols, r, c;
    u16 *cells;

    if ( fp == NULL ) {
        return;
    }
    con_flush();
    cells = host_screen( &rows, &cols );
    for ( r = 0; r < rows; r++ ) {
        for ( c = 0; c < cols; c++ ) {
            int ch = cells[r * cols + c] & 0xFF;
            fputc( ch ? ch : ' ', fp );
        }
        fputc( '\n', fp );
    }
    fclose( fp );
}

/* -pack: the program through PACK.C and back, the compiled one let go */
static PROGRAM *round_trip( PROGRAM *prog )
{
    PROGRAM *back;
    u8 *blob;
    u32 len, flags;
    int rc;
    char module[PATH_MAX];

    blob = pack_program( prog, PK_DEBUG, "HOST", &len );
    comp_free( prog );
    if ( blob == NULL ) {
        out_of_memory();
    }
    back = pack_read( blob, len, &flags, module, sizeof( module ), &rc );
    xfree( blob );
    if ( back == NULL ) {
        fprintf( stderr, "qbhost: the packed program did not read back (%d)\n", rc );
        exit( 3 );
    }
    return back;
}

/* -make: the program packed behind a copy of QBRUN.EXE, as Make EXE File
   writes it -> the exit code */
static int make_file( const char *file, const char *exe, const char *runtime, int debug )
{
    FILE *in, *out;
    PROGRAM *prog;
    CERR err;
    u8 *blob;
    u32 len;
    char module[PATH_MAX], *dot;
    const char *base = file, *scan;
    int ch;

    for ( scan = file; *scan; scan++ ) {
        if ( *scan == '\\' || *scan == '/' || *scan == ':' ) {
            base = scan + 1;
        }
    }
    str_cpyt( module, base, sizeof( module ) );
    dot = str_rchr( module, '.' );
    if ( dot != NULL ) {
        *dot = 0;
    }
    prog = comp_program( &ptext, &err );
    if ( prog == NULL ) {
        fprintf( stderr, "%s line %lu col %d: %s\n", text_name( err.text ), (unsigned long)err.line + 1,
                 err.col + 1, err.msg );
        return 2;
    }
    blob = pack_program( prog, (opt_mbf ? PK_MBF : 0) | (debug ? PK_DEBUG : 0), module, &len );
    comp_free( prog );
    if ( blob == NULL ) {
        out_of_memory();
    }
    in = fopen( runtime, "rb" );
    out = fopen( exe, "wb" );
    if ( in == NULL || out == NULL ) {
        fprintf( stderr, "qbhost: cannot open %s or %s\n", runtime, exe );
        return 2;
    }
    while ( (ch = fgetc( in )) != EOF ) {
        fputc( ch, out );
    }
    fwrite( blob, 1, len, out );
    fclose( in );
    if ( fclose( out ) != 0 ) {
        fprintf( stderr, "qbhost: cannot write %s\n", exe );
        return 2;
    }
    xfree( blob );
    return 0;
}

static void prog_free( PROGRAM *prog, int packed )
{
    if ( packed ) {
        pack_free( prog );
    } else {
        comp_free( prog );
    }
}

static int list_program( void )
{
    int t;
    u32 l;
    char out[MAX_LINE + 64];
    CERR err;

    for ( t = 0; t < ptext.ntexts; t++ ) {
        DOC *doc = ptext.texts[t];
        for ( l = 0; l < doc->count; l++ ) {
            LINE *ln = &doc->lines[l];
            if ( comp_check_line( &ptext, t, l, ln->text ? ln->text : "", ln->len, out, sizeof( out ), &err ) ) {
                printf( "%s\n", out );
            } else {
                printf( "%.*s    <-- %s at %d\n", ln->len, ln->text ? ln->text : "", err.msg, err.col + 1 );
            }
        }
    }
    return 0;
}

int main( int argc, char **argv )
{
    const char *file = NULL, *screen_file = NULL;
    const char *make_exe = NULL, *make_rt = NULL;
    int i, rc, why, list = 0, pack = 0;
    int debug = 1;
    u32 run_at = 0, n;
    PROGRAM *prog;
    CERR err;
    STMTLOC *loc;

    for ( i = 1; i < argc; i++ ) {
        if ( strcmp( argv[i], "-root" ) == 0 && i + 1 < argc ) {
            host_set_root( argv[++i] );
        } else if ( strcmp( argv[i], "-keys" ) == 0 && i + 1 < argc ) {
            host_set_keys( argv[++i] );
        } else if ( strcmp( argv[i], "-screen" ) == 0 && i + 1 < argc ) {
            screen_file = argv[++i];
        } else if ( strcmp( argv[i], "-list" ) == 0 ) {
            list = 1;
        } else if ( strcmp( argv[i], "-pack" ) == 0 ) {
            pack = 1;
        } else if ( strcmp( argv[i], "-make" ) == 0 && i + 2 < argc ) {
            make_exe = argv[++i];
            make_rt = argv[++i];
        } else if ( strcmp( argv[i], "-nodebug" ) == 0 ) {
            debug = 0;
        } else {
            file = argv[i];
        }
    }
    if ( file == NULL ) {
        fprintf( stderr, "usage: qbhost [-root dir] [-keys file] [-screen file] [-list] [-pack]\n"
                         "              [-make out.exe qbrun.exe [-nodebug]] program.bas\n" );
        return 2;
    }
    fp_init();
    prog_clear();
    rc = prog_load( file );
    if ( rc != 0 ) {
        fprintf( stderr, "qbhost: cannot read %s (%d)\n", file, rc );
        return 2;
    }
    if ( list ) {
        return list_program();
    }
    if ( make_exe != NULL ) {
        return make_file( file, make_exe, make_rt, debug );
    }
    con_capture_dos();
    for ( ;; ) {
        prog = comp_program( &ptext, &err );
        if ( prog == NULL ) {
            fprintf( stderr, "%s line %lu col %d: %s\n", text_name( err.text ), (unsigned long)err.line + 1,
                     err.col + 1, err.msg );
            return 2;
        }
        if ( pack ) {
            prog = round_trip( prog );
        }
        if ( !vm_begin( prog ) ) {
            fprintf( stderr, "qbhost: no memory for the run\n" );
            return 4;
        }
        if ( run_at != 0 ) {
            /* RUN n: from the line's code */
            vm.pc = prog->code + run_at - 1;
            for ( n = 0; n < prog->nlocs; n++ ) {
                if ( prog->locs[n].pc == run_at - 1 ) {
                    vm.stmt = n;
                    break;
                }
            }
            run_at = 0;
        }
        con_reset();
        vm_common_give();
        why = vm_go( GO_RUN );
        if ( why == RUN_CHAIN && vm.chain_to[0] ) {
            char next[PATH_MAX];
            strcpy( next, vm.chain_to );
            if ( vm.chain_keep ) {
                vm_common_take();
            }
            vm_end();
            prog_free( prog, pack );
            prog_clear();
            if ( prog_load( next ) != 0 ) {
                fprintf( stderr, "qbhost: cannot read %s\n", next );
                return 2;
            }
            continue;
        }
        if ( why == RUN_CHAIN ) {
            run_at = vm.chain_at;
            vm_end();
            prog_free( prog, pack );
            continue;
        }
        break;
    }
    rc = 0;
    if ( why == RUN_ERROR ) {
        if ( vm.fatal_data != NULL ) {
            fprintf( stderr, "%s line %lu col %u: %s (%d)\n", text_name( vm.fatal_data->in_text ),
                     (unsigned long)vm.fatal_data->line + 1, vm.fatal_data->col + 1, err_message( vm.fatal ), vm.fatal );
        } else {
            loc = &prog->locs[vm.fatal_stmt];
            fprintf( stderr, "%s line %lu col %u: %s (%d)\n", text_name( loc->text ), (unsigned long)loc->line + 1,
                     loc->col + 1, err_message( vm.fatal ), vm.fatal );
        }
        rc = 1;
    } else if ( why == RUN_STOP ) {
        fprintf( stderr, "qbhost: STOP\n" );
    } else if ( why == RUN_BREAK ) {
        fprintf( stderr, "qbhost: Break\n" );
    }
    file_flush_all();
    if ( screen_file != NULL ) {
        dump_screen( screen_file );
    }
    vm_end();
    prog_free( prog, pack );
    return rc;
}
