/*
 * PROCESS.C - system() and the spawn and exec families
 *
 * 4Bh takes the program's path in EDX and a parameter block in EBX:
 * a pointer to the environment (ASCIIZ strings, an empty one at the
 * end) and one to the command tail (a length, the text, a CR).  The
 * kernel copies both into the child.  4Dh gives the exit code after.
 *
 * A LINE LONGER THAN 126 CHARACTERS goes the Windows 95 way: the tail
 * has its first 126 and a length of 7Fh, and the whole line, program
 * name first, is CMDLINE in the child's environment.  START.C reads it
 * back.  The kernel keeps 0F00h bytes of environment and gives a child
 * none at all when the block is bigger, so a block that will not fit
 * is E2BIG here instead.
 */
#include "pmlib.h"
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <io.h>

#define TAIL_MAX    126
#define ARGS_MAX    64
#define ENV_MAX     0xF00               /* the kernel's room for a block */

/* one argument after a space, quoted where it has a space or a tab or
   is empty, at line + at (NULL line: only measured); the new length */
static size_t put_arg( char *line, size_t at, const char *arg )
{
    size_t n = strlen( arg );
    int quote = (n == 0 || strpbrk( arg, " \t" ) != NULL);

    if ( line != NULL ) {
        line[at] = ' ';
        if ( quote ) {
            line[at + 1] = '"';
        }
        memcpy( line + at + 1 + quote, arg, n );
        if ( quote ) {
            line[at + 2 + n] = '"';
        }
    }
    return at + 1 + n + (quote ? 2 : 0);
}

/* the arguments after argv[0], malloc()ed, its length in *len; NULL
   and E2BIG when no environment could hold them, or ENOMEM */
static char *make_args( char *const argv[], size_t *len )
{
    size_t total = 0;
    char *line;
    int i;

    for ( i = 1; argv[i] != NULL; i++ ) {
        total = put_arg( NULL, total, argv[i] );
        if ( total > ENV_MAX ) {
            errno = E2BIG;
            return NULL;
        }
    }
    line = (char *)malloc( total + 1 );
    if ( line == NULL ) {
        errno = ENOMEM;
        return NULL;
    }
    total = 0;
    for ( i = 1; argv[i] != NULL; i++ ) {
        total = put_arg( line, total, argv[i] );
    }
    line[total] = '\0';
    *len = total;
    return line;
}

/* the PSP's tail from the arguments: a length, the text, a CR.  Past
   126 it keeps 126 and says 7Fh */
static void make_tail( const char *args, size_t len, unsigned char *tail )
{
    if ( len > TAIL_MAX ) {
        tail[0] = 0x7F;
        len = TAIL_MAX;
    } else {
        tail[0] = (unsigned char)len;
    }
    memcpy( tail + 1, args, len );
    tail[1 + len] = '\r';
}

/* CMDLINE's value: the program's name, then the arguments; malloc()ed,
   or NULL and ENOMEM */
static char *make_cmdline( const char *name, const char *args, size_t len )
{
    size_t at = put_arg( NULL, 0, name );
    char *line = (char *)malloc( at + len + 1 );

    if ( line == NULL ) {
        errno = ENOMEM;
        return NULL;
    }
    put_arg( line, 0, name );
    memmove( line, line + 1, at - 1 );  /* no space before the name */
    memcpy( line + at - 1, args, len );
    line[at - 1 + len] = '\0';
    return line;
}

static int is_cmdline( const char *var )
{
    return strnicmp( var, "CMDLINE=", 8 ) == 0;
}

/* the environment block for envp, malloc()ed; NULL envp: environ.
   With cmdline, a CMDLINE the caller had is replaced by that one.
   NULL and E2BIG when the kernel would not take the block */
static char *make_env( char *const envp[], const char *cmdline )
{
    size_t total = 1;
    char *block, *p;
    int i;

    if ( envp == NULL ) {
        envp = environ;
    }
    for ( i = 0; envp != NULL && envp[i] != NULL; i++ ) {
        if ( cmdline == NULL || !is_cmdline( envp[i] ) ) {
            total += strlen( envp[i] ) + 1;
        }
    }
    if ( cmdline != NULL ) {
        total += 8 + strlen( cmdline ) + 1;
    }
    if ( total > ENV_MAX - 2 ) {
        errno = E2BIG;
        return NULL;
    }
    block = (char *)malloc( total + 1 );
    if ( block == NULL ) {
        errno = ENOMEM;
        return NULL;
    }
    p = block;
    for ( i = 0; envp != NULL && envp[i] != NULL; i++ ) {
        if ( cmdline == NULL || !is_cmdline( envp[i] ) ) {
            strcpy( p, envp[i] );
            p += strlen( p ) + 1;
        }
    }
    if ( cmdline != NULL ) {
        memcpy( p, "CMDLINE=", 8 );
        strcpy( p + 8, cmdline );
        p += strlen( p ) + 1;
    }
    *p++ = '\0';
    *p = '\0';
    return block;
}

static int has_ext( const char *path )
{
    const char *base = path;
    const char *p;

    for ( p = path; *p; p++ ) {
        if ( *p == '\\' || *p == '/' || *p == ':' ) {
            base = p + 1;
        }
    }
    return strchr( base, '.' ) != NULL;
}

static int is_bat( const char *path )
{
    const char *dot = strrchr( path, '.' );

    return dot != NULL && stricmp( dot, ".bat" ) == 0;
}

/* name as given, or with .COM, .EXE, .BAT if it has no extension, in
   dir (or as it is when dir is NULL): into out, 1 if found */
static int try_names( const char *dir, size_t dirlen, const char *name, char *out )
{
    static const char *const exts[3] = { ".COM", ".EXE", ".BAT" };
    size_t len;
    int i;

    len = 0;
    if ( dir != NULL && dirlen ) {
        if ( dirlen > 200 ) {
            return 0;
        }
        memcpy( out, dir, dirlen );
        len = dirlen;
        if ( out[len - 1] != '\\' && out[len - 1] != '/' ) {
            out[len++] = '\\';
        }
    }
    if ( len + strlen( name ) + 5 > 260 ) {
        return 0;
    }
    strcpy( out + len, name );
    if ( has_ext( name ) ) {
        return access( out, F_OK ) == 0;
    }
    for ( i = 0; i < 3; i++ ) {
        strcpy( out + len + strlen( name ), exts[i] );
        if ( access( out, F_OK ) == 0 ) {
            return 1;
        }
    }
    return 0;
}

/* where the shell would find name; 1 and the path in out, or 0 */
static int search( const char *name, int use_path, char *out )
{
    const char *path;
    const char *end;

    if ( try_names( NULL, 0, name, out ) ) {
        return 1;
    }
    if ( !use_path || strpbrk( name, "\\/:" ) != NULL ) {
        return 0;
    }
    path = getenv( "PATH" );
    while ( path != NULL && *path ) {
        end = strchr( path, ';' );
        if ( end == NULL ) {
            end = path + strlen( path );
        }
        if ( end > path && try_names( path, (size_t)(end - path), name, out ) ) {
            return 1;
        }
        path = *end ? end + 1 : end;
    }
    return 0;
}

static int run_tail( int mode, const char *path, const unsigned char *tail, char *const envp[],
                     const char *cmdline );

static int run( int mode, const char *path, char *const argv[], char *const envp[], int use_path )
{
    char found[264];
    unsigned char tail[TAIL_MAX + 2];
    const char *shell;
    const char *bat_argv[ARGS_MAX + 3];
    char *args;
    char *cmdline = NULL;
    size_t len = 0;
    int code;
    int i;

    if ( mode == P_NOWAIT ) {
        return __pm_errno( EINVAL );
    }
    if ( !search( path, use_path, found ) ) {
        return __pm_errno( ENOENT );
    }
    if ( is_bat( found ) ) {
        /* a batch file: COMSPEC /C file args */
        shell = getenv( "COMSPEC" );
        if ( shell == NULL ) {
            return __pm_errno( ENOENT );
        }
        bat_argv[0] = shell;
        bat_argv[1] = "/C";
        bat_argv[2] = found;
        for ( i = 1; argv[i] != NULL && i < ARGS_MAX; i++ ) {
            bat_argv[i + 2] = argv[i];
        }
        bat_argv[i + 2] = NULL;
        return run( mode, shell, (char *const *)bat_argv, envp, 0 );
    }
    args = make_args( argv, &len );
    if ( args == NULL ) {
        return -1;
    }
    make_tail( args, len, tail );
    if ( len > TAIL_MAX ) {
        cmdline = make_cmdline( argv[0] != NULL ? argv[0] : path, args, len );
        if ( cmdline == NULL ) {
            free( args );
            return -1;
        }
    }
    code = run_tail( mode, found, tail, envp, cmdline );
    free( cmdline );
    free( args );
    return code;
}

/* EXEC the program at path with a ready-made tail, and CMDLINE in its
   environment when cmdline is not NULL */
static int run_tail( int mode, const char *path, const unsigned char *tail, char *const envp[],
                     const char *cmdline )
{
    uint32_t block[2];
    char *env;
    pm_regs regs;
    int code;

    env = make_env( envp, cmdline );
    if ( env == NULL ) {
        return -1;
    }
    block[0] = (uint32_t)env;
    block[1] = (uint32_t)tail;
    if ( __pm_exit_flush != NULL ) {
        __pm_exit_flush();              /* what the caller printed goes first */
    }
    memset( &regs, 0, sizeof( regs ) );
    regs.edx = (uint32_t)path;
    regs.ebx = (uint32_t)block;
    if ( __pm_dos( &regs, 0x4B00 ) ) {
        free( env );
        return __pm_fail( &regs );
    }
    free( env );
    memset( &regs, 0, sizeof( regs ) );
    __pm_dos( &regs, 0x4D00 );
    code = (int)(regs.eax & 0xFF);
    if ( mode == P_OVERLAY ) {
        exit( code );
    }
    return code;
}

int spawnv( int mode, const char *path, char *const argv[] )
{
    return run( mode, path, argv, NULL, 0 );
}

int spawnve( int mode, const char *path, char *const argv[], char *const envp[] )
{
    return run( mode, path, argv, envp, 0 );
}

int spawnvp( int mode, const char *path, char *const argv[] )
{
    return run( mode, path, argv, NULL, 1 );
}

int spawnvpe( int mode, const char *path, char *const argv[], char *const envp[] )
{
    return run( mode, path, argv, envp, 1 );
}

/* the l forms: the arguments into an array, up to the NULL (and, for
   the e forms, the environment after it) */
static int gather( const char *arg0, va_list args, const char **argv, char *const **envp )
{
    int n = 0;
    const char *a = arg0;

    while ( a != NULL ) {
        if ( n >= ARGS_MAX ) {
            errno = E2BIG;
            return -1;
        }
        argv[n++] = a;
        a = va_arg( args, const char * );
    }
    argv[n] = NULL;
    if ( envp != NULL ) {
        *envp = va_arg( args, char *const * );
    }
    return n;
}

int spawnl( int mode, const char *path, const char *arg0, ... )
{
    const char *argv[ARGS_MAX + 1];
    va_list args;
    int n;

    va_start( args, arg0 );
    n = gather( arg0, args, argv, NULL );
    va_end( args );
    return n < 0 ? -1 : run( mode, path, (char *const *)argv, NULL, 0 );
}

int spawnle( int mode, const char *path, const char *arg0, ... )
{
    const char *argv[ARGS_MAX + 1];
    char *const *envp;
    va_list args;
    int n;

    va_start( args, arg0 );
    n = gather( arg0, args, argv, &envp );
    va_end( args );
    return n < 0 ? -1 : run( mode, path, (char *const *)argv, envp, 0 );
}

int spawnlp( int mode, const char *path, const char *arg0, ... )
{
    const char *argv[ARGS_MAX + 1];
    va_list args;
    int n;

    va_start( args, arg0 );
    n = gather( arg0, args, argv, NULL );
    va_end( args );
    return n < 0 ? -1 : run( mode, path, (char *const *)argv, NULL, 1 );
}

int spawnlpe( int mode, const char *path, const char *arg0, ... )
{
    const char *argv[ARGS_MAX + 1];
    char *const *envp;
    va_list args;
    int n;

    va_start( args, arg0 );
    n = gather( arg0, args, argv, &envp );
    va_end( args );
    return n < 0 ? -1 : run( mode, path, (char *const *)argv, envp, 1 );
}

int execl( const char *path, const char *arg0, ... )
{
    const char *argv[ARGS_MAX + 1];
    va_list args;
    int n;

    va_start( args, arg0 );
    n = gather( arg0, args, argv, NULL );
    va_end( args );
    return n < 0 ? -1 : run( P_OVERLAY, path, (char *const *)argv, NULL, 0 );
}

int execv( const char *path, char *const argv[] )
{
    return run( P_OVERLAY, path, argv, NULL, 0 );
}

int execvp( const char *path, char *const argv[] )
{
    return run( P_OVERLAY, path, argv, NULL, 1 );
}

int execve( const char *path, char *const argv[], char *const envp[] )
{
    return run( P_OVERLAY, path, argv, envp, 0 );
}

/* COMSPEC /C command, the command as it is: quoting it would hand the
   quotes to the shell, which would keep them (a > into "x.txt"") */
int system( const char *command )
{
    const char *shell = getenv( "COMSPEC" );
    unsigned char tail[TAIL_MAX + 2];
    size_t len;

    if ( command == NULL ) {
        return shell != NULL;
    }
    if ( shell == NULL ) {
        return __pm_errno( ENOENT );
    }
    len = strlen( command );
    if ( len + 4 > TAIL_MAX ) {
        return __pm_errno( E2BIG );
    }
    memcpy( tail + 1, " /C ", 4 );
    memcpy( tail + 5, command, len );
    tail[0] = (unsigned char)(len + 4);
    tail[5 + len] = '\r';
    return run_tail( P_WAIT, shell, tail, NULL, NULL );
}

int getpid( void )
{
    pm_regs regs = { 0 };

    __pm_dos( &regs, 0x6200 );
    return (int)regs.ebx;
}
