/*
 * MAIN.C - where XARC.EXE starts: the arguments, the preferences, and
 * which of the two programs it is this time.
 *
 * A lone L, T, X or A is the command line (CLI.C); /? the usage; MEM the
 * memory report; anything else, or nothing, the screen (XARC.C), with
 * the first argument opened if there is one.  /G and /T ask for the
 * graphics or the text screen for this run, wherever they are written,
 * and are not saved - Options > Graphics mode is what changes the
 * default in XARCHIVE.INI.
 *
 * XARCHIVE.INI is beside XARC.EXE.  A native PM-DOS program is not told
 * where it was run from, so XARC looks where the shell would have found
 * it - the current directory, then the PATH - as HELP does, and then in
 * the system directory.  Nothing is printed on starting: a PM-DOS
 * command begins with its work.
 */
#include "xarc.h"

#define MAX_ARGS    32

static char *args[MAX_ARGS + 1];
static char  arg_text[300];
static char  self[260];

/* the tail into words; quotes keep a name with spaces in it whole */
static int split_args( const char *tail )
{
    char *out = arg_text;
    int count = 1, quoted;

    args[0] = self;
    while ( *tail && count < MAX_ARGS ) {
        while ( *tail == ' ' || *tail == '\t' ) {
            tail++;
        }
        /* a word starts only where there is room for its terminator */
        if ( !*tail || out >= arg_text + sizeof( arg_text ) - 1 ) {
            break;
        }
        args[count++] = out;
        quoted = 0;
        while ( *tail && (quoted || (*tail != ' ' && *tail != '\t')) ) {
            if ( *tail == '"' ) {
                quoted = !quoted;
            } else if ( out < arg_text + sizeof( arg_text ) - 1 ) {
                *out++ = *tail;
            }
            tail++;
        }
        *out++ = 0;
    }
    args[count] = NULL;
    return count;
}

static int is_file( const char *path )
{
    struct stat info;

    return stat( path, &info ) == 0 && !S_ISDIR( info.st_mode );
}

/* "dir" (len characters) + "\XARC.EXE" into trial; 0 when too long */
static int self_in( char *trial, int size, const char *dir, int len )
{
    if ( len <= 0 || len > size - 10 ) {
        return 0;
    }
    memcpy( trial, dir, (size_t)len );
    if ( trial[len - 1] != '\\' && trial[len - 1] != ':' ) {
        trial[len++] = '\\';
    }
    strcpy( trial + len, "XARC.EXE" );
    return 1;
}

/* XARC.EXE: here, along the PATH, or in the system directory - the
   interpreter's, which INT 21h F1h AL=03h names, and where SETUP puts
   XARC.  Found nowhere, the system directory is still where XARCHIVE.INI
   goes, rather than whichever directory XARC was started from. */
static void find_self( void )
{
    char trial[260], shell[80];
    const char *path = getenv( "PATH" );
    REGS regs;
    int len;

    if ( is_file( "XARC.EXE" ) && _fullpath( self, "XARC.EXE", sizeof( self ) ) ) {
        return;
    }
    while ( path && *path ) {
        for ( len = 0; path[len] && path[len] != ';'; len++ ) {
        }
        if ( self_in( trial, sizeof( trial ), path, len ) && is_file( trial )
             && _fullpath( self, trial, sizeof( self ) ) ) {
            return;
        }
        path += len;
        if ( *path == ';' ) {
            path++;
        }
    }
    xa_clear( &regs );
    regs.edx = (u32)shell;
    shell[0] = 0;
    if ( xa_dos( &regs, 0xF103 ) == 0 && shell[0] ) {
        shell[sizeof( shell ) - 1] = 0;
        len = (int)(base_name( shell ) - shell);
        if ( self_in( trial, sizeof( trial ), shell, len )
             && _fullpath( self, trial, sizeof( self ) ) ) {
            return;
        }
    }
    strcpy( self, "XARC.EXE" );
}

void __cdecl dos_entry( void )
{
    int argc, graphics, in, out, code;

    xa_crt_init();
    find_self();
    argc = split_args( xa_cmdtail() );

    /* the preferences first, so the command line honours them too */
    ArcPrefSetFileFromExe( self );
    ArcPrefLoad();
    view_folder_mode( ArcPrefFolderView() );
    graphics = ArcPrefGraphics();

    /* /G and /T out of the arguments before anything else reads them */
    for ( in = out = 1; in < argc; in++ ) {
        if ( !cli_video_switch( args[in], &graphics ) ) {
            args[out++] = args[in];
        }
    }
    argc = out;
    args[argc] = NULL;

    if ( argc >= 2 && cli_is_help( args[1] ) ) {
        cli_usage();
        code = 0;
    } else if ( argc >= 2 && cli_is_mem( args[1] ) ) {
        code = cli_memory();
    } else if ( argc >= 2 && cli_is_cmd( args[1] ) ) {
        code = cli_run( argc, args );
    } else {
        code = tui_run( argc >= 2 ? args[1] : NULL, graphics );
    }
    exit( code );
}
