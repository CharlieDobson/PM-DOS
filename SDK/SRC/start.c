/*
 * START.C - the startup's C half: the environment, the command line,
 * main() and exit()
 *
 * THE ENVIRONMENT is the block PSP+2Ch points at (an offset in the
 * slot, 0 when the program was given none): ASCIIZ strings ended by
 * an empty one.  environ points into it, so it costs one pointer per
 * variable.
 *
 * THE COMMAND LINE is the PSP's tail: a length byte at 80h and up to
 * 126 characters at 81h.  A longer line arrives as Windows 95 sends
 * one, a length of 7Fh and the whole line, program name first, in the
 * CMDLINE variable.  It is split the way Microsoft C splits a line:
 * spaces and tabs between arguments, double quotes around an argument
 * with spaces in it, \" for a quote inside one; a backslash anywhere
 * else is a backslash, so paths need no doubling.
 *
 * argv[0] is the program's full path, which the kernel puts behind the
 * environment as MS-DOS does: a word of 1 and the path.  Kernels up to
 * 0.69 put nothing there, and then argv[0] is the name from CMDLINE
 * when there is one, and otherwise "".
 */
#include "pmlib.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int    _argc;
char **_argv;
char **environ;

/* the clock at the start, for clock() (TIME.C): hundredths since
   midnight, and the date as year << 16 | month << 8 | day */
long __pm_clock0;
long __pm_day0;

static void read_clock( void )
{
    pm_regs regs = { 0 };

    __pm_dos( &regs, 0x2A00 );
    __pm_day0 = (long)(((regs.ecx & 0xFFFF) << 16) | (regs.edx & 0xFFFF));
    memset( &regs, 0, sizeof( regs ) );
    __pm_dos( &regs, 0x2C00 );
    __pm_clock0 = (long)((regs.ecx >> 8) & 0xFF) * 360000L + (long)(regs.ecx & 0xFF) * 6000L
                + (long)((regs.edx >> 8) & 0xFF) * 100L + (long)(regs.edx & 0xFF);
}

extern int main( int argc, char **argv, char **envp );

static char *no_args[2] = { "", NULL };
static char *no_env[1] = { NULL };

/* Split line into argv[] - the strings into buf, which is at least as
   long as line, and the pointers into argv, which has room for the
   most arguments line can hold.  Returns the count. */
static int split( const char *line, char *buf, char **argv )
{
    int count = 0;
    int quoted;
    int slashes;

    for ( ;; ) {
        while ( *line == ' ' || *line == '\t' ) {
            line++;
        }
        if ( *line == '\0' ) {
            break;
        }
        argv[count++] = buf;
        quoted = 0;
        while ( *line && (quoted || (*line != ' ' && *line != '\t')) ) {
            if ( *line == '\\' ) {
                slashes = 0;
                while ( *line == '\\' ) {
                    slashes++;
                    line++;
                }
                if ( *line == '"' ) {
                    /* 2n backslashes: n of them, and the quote is a
                       quote; 2n+1: n of them and a quote character */
                    while ( slashes >= 2 ) {
                        *buf++ = '\\';
                        slashes -= 2;
                    }
                    if ( slashes ) {
                        *buf++ = '"';
                        line++;
                    }
                } else {
                    while ( slashes-- ) {
                        *buf++ = '\\';
                    }
                }
            } else if ( *line == '"' ) {
                if ( quoted && line[1] == '"' ) {
                    *buf++ = '"';       /* "" inside quotes: one quote */
                    line++;
                } else {
                    quoted = !quoted;
                }
                line++;
            } else {
                *buf++ = *line++;
            }
        }
        *buf++ = '\0';
    }
    argv[count] = NULL;
    return count;
}

static void build_environ( void )
{
    const unsigned char *psp = pm_psp();
    char *block = (char *)*(const uint32_t *)(psp + 0x2C);
    char *p;
    char **list;
    int count = 0;

    if ( block == NULL ) {
        environ = no_env;
        return;
    }
    for ( p = block; *p; p += strlen( p ) + 1 ) {
        count++;
    }
    list = (char **)malloc( (count + 1) * sizeof( char * ) );
    if ( list == NULL ) {
        environ = no_env;
        return;
    }
    count = 0;
    for ( p = block; *p; p += strlen( p ) + 1 ) {
        list[count++] = p;
    }
    list[count] = NULL;
    environ = list;
}

/* the program's path behind the environment block, or NULL.  It has to
   look like one - a count of exactly 1, then "X:\", ending inside the
   block's 0F00h bytes - because an older kernel left whatever was in
   the slot there */
static char *prog_path( void )
{
    const unsigned char *psp = pm_psp();
    char *block = (char *)*(const uint32_t *)(psp + 0x2C);
    char *end;
    char *nul;
    char *p;

    if ( block == NULL ) {
        return NULL;
    }
    end = block + 0xF00;
    p = block;
    while ( p < end && *p ) {
        nul = (char *)memchr( p, '\0', (size_t)(end - p) );
        if ( nul == NULL ) {
            return NULL;
        }
        p = nul + 1;
    }
    if ( p >= end ) {
        return NULL;
    }
    p++;                                /* the empty string */
    if ( p + 6 > end || p[0] != 1 || p[1] != 0 ) {
        return NULL;
    }
    p += 2;
    if ( !isalpha( (unsigned char)p[0] ) || p[1] != ':' || p[2] != '\\' ) {
        return NULL;
    }
    if ( memchr( p, '\0', (size_t)(end - p) ) == NULL ) {
        return NULL;
    }
    return p;
}

static void build_argv( void )
{
    const unsigned char *psp = pm_psp();
    const char *full = NULL;
    const char *line;
    char tail[128];
    char *path;
    char *buf;
    char **argv;
    size_t len;
    unsigned tlen = psp[0x80];

    if ( tlen == 0x7F ) {
        full = getenv( "CMDLINE" );
    }
    if ( full != NULL ) {
        line = full;
    } else {
        if ( tlen > 126 ) {
            tlen = 126;
        }
        memcpy( tail, psp + 0x81, tlen );
        tail[tlen] = '\0';
        len = strcspn( tail, "\r" );    /* the tail ends with a CR */
        tail[len] = '\0';
        line = tail;
    }

    /* one more argument than the line could hold, for the "" argv[0]
       a PSP tail has no name for; each argument is at least one
       character and a separator */
    len = strlen( line );
    buf = (char *)malloc( len + 2 );
    argv = (char **)malloc( (len / 2 + 3) * sizeof( char * ) );
    if ( buf == NULL || argv == NULL ) {
        _argc = 1;
        _argv = no_args;
        return;
    }
    if ( full != NULL ) {
        _argc = split( line, buf, argv );
        if ( _argc == 0 ) {
            argv[0] = "";
            argv[1] = NULL;
            _argc = 1;
        }
    } else {
        argv[0] = "";
        _argc = 1 + split( line, buf, argv + 1 );
    }
    path = prog_path();
    if ( path != NULL ) {
        argv[0] = path;
    }
    _argv = argv;
}

void _PM_ASMCALL __pm_start( void )
{
    read_clock();
    build_environ();
    build_argv();
    exit( main( _argc, _argv, environ ) );
}
