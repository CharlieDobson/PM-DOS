/*
 * CFG.C - CONFIG.SYS and AUTOEXEC.BAT.
 *
 * A NEW SYSTEM gets SETUP.INF's [config] and [autoexec] with %DOSDIR%
 * filled in.  A line that is %COUNTRY% or %KEYB% by itself becomes the
 * COUNTRY= line or the KEYB line for what was chosen, or goes, for the
 * United States.  A CONFIG.SYS or AUTOEXEC.BAT already there is kept as
 * CONFIG.OLD / AUTOEXEC.OLD, as 6.22 kept them.
 *
 * AN UPGRADE keeps the user's own files and changes only what PM-DOS
 * needs (the copies as they were are in OLD_DOS, see UNDO.C):
 *   - a line loading a program SETUP.INF names in [config.remove] or
 *     [autoexec.remove] becomes "REM " and the line - PM-DOS does the
 *     work of HIMEM, EMM386, SMARTDRV, SHARE, MSCDEX and the rest itself;
 *   - SHELL= becomes PM-DOS's interpreter, which is what tells the
 *     kernel where the rest of the system is;
 *   - the COUNTRY= and KEYB lines become what was chosen;
 *   - MODE CON CODEPAGE lines go (PM-DOS has no screen code pages), and
 *     SET COMSPEC= too (the kernel sets it);
 *   - PATH gets the DOS directory if it lacks it;
 *   - the lines of [config.add] are added if missing.
 * New lines go at the top of CONFIG.SYS, before any [menu] block, where
 * every configuration reads them.
 *
 * A REINSTALL of PM-DOS changes only the COUNTRY= and KEYB lines, and
 * only when they were changed on the settings screen.
 */
#include "setup.h"
#include "text.h"

/* A file's lines, each as long as it is: a CONFIG.SYS line Setup does
   not change goes back exactly as it came, however long, and however
   many lines there are.  (They were fixed buffers of LINELEN until a
   review found that a long line came back cut short.) */
typedef struct {
    char **line;
    int  n, cap;
} LINES;

static void lines_init( LINES *lines, int cap )
{
    lines->n = 0;
    lines->cap = cap < 16 ? 16 : cap;
    lines->line = (char **)xalloc( sizeof( char * ) * (u32)lines->cap );
}

static void lines_free( LINES *lines )
{
    int i;

    for ( i = 0; i < lines->n; i++ ) {
        xfree( lines->line[i] );
    }
    xfree( lines->line );
    lines->line = NULL;
    lines->n = lines->cap = 0;
}

static void lines_room( LINES *lines )
{
    char **more;
    int i;

    if ( lines->n < lines->cap ) {
        return;
    }
    more = (char **)xalloc( sizeof( char * ) * (u32)lines->cap * 2 );
    for ( i = 0; i < lines->n; i++ ) {
        more[i] = lines->line[i];
    }
    xfree( lines->line );
    lines->line = more;
    lines->cap *= 2;
}

static void lines_insert( LINES *lines, int at, const char *text )
{
    int i;

    lines_room( lines );
    for ( i = lines->n; i > at; i-- ) {
        lines->line[i] = lines->line[i - 1];
    }
    lines->line[at] = str_dup( text );
    lines->n++;
}

static void lines_add( LINES *lines, const char *text )
{
    lines_insert( lines, lines->n, text );
}

/* a line replaced by another, which may be longer */
static void lines_set( LINES *lines, int at, const char *text )
{
    xfree( lines->line[at] );
    lines->line[at] = str_dup( text );
}

/* a file's lines; 0 = read (an absent file is no lines, and read) */
static int lines_read( LINES *lines, const char *path )
{
    u16 handle, done, i, count = 1;
    u32 size;
    char *text, *line, *end;

    lines_init( lines, 16 );
    if ( plat_open( path, 0, &handle ) ) {
        return 0;
    }
    if ( plat_size( handle, &size ) || size > 30000u ) {
        plat_close( handle );
        return 1;
    }
    text = (char *)xalloc( size + 1 );
    if ( plat_read( handle, text, (u16)size, &done ) ) {
        plat_close( handle );
        xfree( text );
        return 1;
    }
    plat_close( handle );
    text[done] = 0;
    for ( i = 0; i < done; i++ ) {
        if ( text[i] == '\n' ) {
            count++;
        }
    }
    lines_free( lines );
    lines_init( lines, count + 16 );    /* and room for what is added */
    for ( line = text; *line && *line != 0x1A; line = end ) {
        end = line;
        while ( *end && *end != '\n' && *end != 0x1A ) {
            end++;
        }
        if ( *end == '\n' ) {
            *end++ = 0;
        } else {
            *end = 0;
        }
        if ( str_len( line ) && line[str_len( line ) - 1] == '\r' ) {
            line[str_len( line ) - 1] = 0;
        }
        lines_add( lines, line );
    }
    xfree( text );
    return 0;
}

static int lines_write( LINES *lines, const char *path )
{
    u16 handle, done;
    int i, rc = 0;

    if ( plat_create( path, &handle ) ) {
        return 1;
    }
    for ( i = 0; i < lines->n && rc == 0; i++ ) {
        int len = str_len( lines->line[i] );
        if ( plat_write( handle, lines->line[i], (u16)len, &done ) || done != (u16)len ||
             plat_write( handle, "\r\n", 2, &done ) || done != 2 ) {
            rc = 1;
        }
    }
    if ( plat_close( handle ) ) {
        rc = 1;
    }
    return rc;
}

/* "C:\CONFIG.SYS" on the drive as it is now */
static void root_path( char *path, const char *name )
{
    sfmt( path, PATHLEN, "%c:\\%s", 'A' + tgt.drive, name );
}

/* ------------------------------------------------------------------ */
/* the pieces of a line                                                */
/* ------------------------------------------------------------------ */

/* CONFIG.SYS: the keyword, upper case ("DEVICEHIGH") */
static void cfg_keyword( const char *line, char *word, int max )
{
    int len = 0;

    line = skip_blanks( line );
    while ( *line && *line != '=' && *line != ' ' && *line != '\t' && len < max - 1 ) {
        word[len++] = (char)ch_upper( (u8)*line++ );
    }
    word[len] = 0;
}

/* the program a DEVICE=, INSTALL= or batch line runs: its base name,
   upper case, without the path ("HIMEM.SYS", or "SMARTDRV" in a batch) */
static void program_of( const char *cmd, char *name, int max, int keep_ext )
{
    char word[PATHLEN];
    int len = 0;
    char *slash, *dot;

    cmd = skip_blanks( cmd );
    while ( *cmd && *cmd != ' ' && *cmd != '\t' && *cmd != '/' && len < (int)sizeof( word ) - 1 ) {
        word[len++] = *cmd++;
    }
    word[len] = 0;
    str_upper( word );
    slash = str_rchr( word, '\\' );
    if ( slash == NULL ) {
        slash = str_rchr( word, ':' );
    }
    str_cpyn( name, slash ? slash + 1 : word, max );
    if ( !keep_ext ) {
        dot = str_chr( name, '.' );
        if ( dot ) {
            *dot = 0;
        }
    }
}

/* what follows the keyword and its '=' - and past DEVICEHIGH's own
   switches (/L:... /S, SIZE=n) to the file name */
static const char *cfg_value( const char *line )
{
    const char *eq;

    eq = str_chr( line, '=' );
    if ( eq == NULL ) {
        return "";
    }
    eq = skip_blanks( eq + 1 );
    for ( ;; ) {
        if ( *eq == '/' ) {             /* DEVICEHIGH /L:1,12345 = ... */
            while ( *eq && *eq != ' ' && *eq != '=' ) {
                eq++;
            }
            eq = skip_blanks( eq );
            if ( *eq == '=' ) {
                eq = skip_blanks( eq + 1 );
            }
            continue;
        }
        if ( !str_nicmp( eq, "SIZE=", 5 ) ) {
            eq += 5;
            while ( *eq && *eq != ' ' ) {
                eq++;
            }
            eq = skip_blanks( eq );
            continue;
        }
        return eq;
    }
}

/* the command of a batch line, past '@' and LH / LOADHIGH and theirs */
static const char *bat_command( const char *line )
{
    line = skip_blanks( line );
    if ( *line == '@' ) {
        line = skip_blanks( line + 1 );
    }
    if ( (!str_nicmp( line, "LH", 2 ) && (line[2] == ' ' || line[2] == '\t')) ||
         (!str_nicmp( line, "LOADHIGH", 8 ) && (line[8] == ' ' || line[8] == '\t')) ) {
        line = skip_blanks( line + (line[2] == ' ' || line[2] == '\t' ? 2 : 8) );
        while ( *line == '/' ) {
            while ( *line && *line != ' ' ) {
                line++;
            }
            line = skip_blanks( line );
        }
    }
    return line;
}

static int named_in( char **list, int count, const char *name )
{
    char base[16], entry[16];
    char *dot;
    int i;

    str_cpyn( base, name, sizeof( base ) );
    dot = str_chr( base, '.' );
    for ( i = 0; i < count; i++ ) {
        str_cpyn( entry, list[i], sizeof( entry ) );
        str_upper( entry );
        if ( !str_icmp( entry, base ) ) {
            return 1;
        }
        /* "SMARTDRV" in the list stands for SMARTDRV.EXE as well */
        if ( dot && !str_chr( entry, '.' ) && str_len( entry ) == (int)(dot - base) &&
             !str_nicmp( entry, base, (int)(dot - base) ) ) {
            return 1;
        }
    }
    return 0;
}

static void rem_out( LINES *lines, int at )
{
    int len = str_len( lines->line[at] ) + 5;
    char *line = (char *)xalloc( (u32)len );

    str_cpyn( line, "REM ", len );
    str_catn( line, lines->line[at], len );
    xfree( lines->line[at] );
    lines->line[at] = line;
}

/* "COUNTRY=049" for what was chosen, "" for the United States on its
   first code page */
static void country_line( char *line, int max )
{
    const COUNTRYDEF *ctry = opt.country;

    line[0] = 0;
    if ( ctry && parse_u32( ctry->code, NULL ) != 1 ) {
        sfmt( line, max, "COUNTRY=%s", ctry->code );
    }
}

/* "KEYB GR", with the code page the country uses when the layout has
   more than one, and /ID: when the code has more than one layout */
static void keyb_line( char *line, int max )
{
    const KEYBDEF *kb = opt.keyboard;
    char id[12];

    line[0] = 0;
    if ( kb == NULL || !str_icmp( kb->code, "US" ) ) {
        return;
    }
    id[0] = 0;
    if ( kb->id && inf_keyboard( kb->code, 0 ) != kb ) {
        sfmt( id, sizeof( id ), " /ID:%u", kb->id );
    }
    sfmt( line, max, "KEYB %s%s", kb->code, id );
}

static void fill_dosdir( char *out, const char *in, int max )
{
    int len = 0;

    while ( *in && len < max - 1 ) {
        if ( in[0] == '%' && !str_nicmp( in, "%DOSDIR%", 8 ) ) {
            const char *dir = opt.dosdir;
            while ( *dir && len < max - 1 ) {
                out[len++] = *dir++;
            }
            in += 8;
        } else {
            out[len++] = *in++;
        }
    }
    out[len] = 0;
}

/* ------------------------------------------------------------------ */
/* what the system being upgraded uses                                 */
/* ------------------------------------------------------------------ */

static int dir_exists( const char *dir )
{
    u16 attr;

    return plat_get_attr( dir, &attr ) == 0 && (attr & FA_DIR);
}

static int has_command( const char *dir, const char *name )
{
    char path[PATHLEN];
    u16 attr;

    path_join( path, dir, name, sizeof( path ) );
    return plat_get_attr( path, &attr ) == 0;
}

/* The country and keyboard the system uses now, and where its commands
   are - the defaults for the settings and the directory screens */
void cfg_read_current( void )
{
    LINES lines;
    char path[PATHLEN], word[24], dir[PATHLEN], name[16];
    const char *val;
    char *slash;
    int i;

    tgt.olddir[0] = 0;
    root_path( path, "CONFIG.SYS" );
    lines_read( &lines, path );
    for ( i = 0; i < lines.n; i++ ) {
        cfg_keyword( lines.line[i], word, sizeof( word ) );
        val = cfg_value( lines.line[i] );
        if ( !str_icmp( word, "COUNTRY" ) ) {
            const COUNTRYDEF *ctry = inf_country( val );
            if ( ctry ) {
                opt.country = ctry;
            }
        } else if ( !str_icmp( word, "SHELL" ) ) {
            program_of( val, name, sizeof( name ), 1 );
            str_cpyn( dir, skip_blanks( val ), sizeof( dir ) );
            for ( slash = dir; *slash && *slash != ' ' && *slash != '/'; slash++ ) {
            }
            *slash = 0;
            slash = str_rchr( dir, '\\' );
            if ( slash && slash > dir + 2 ) {   /* not the root */
                *slash = 0;
                str_upper( dir );
                str_cpyn( tgt.olddir, dir, sizeof( tgt.olddir ) );
            }
        }
    }
    lines_free( &lines );

    root_path( path, "AUTOEXEC.BAT" );
    lines_read( &lines, path );
    for ( i = 0; i < lines.n; i++ ) {
        const char *cmd = bat_command( lines.line[i] );
        program_of( cmd, name, sizeof( name ), 0 );
        if ( !str_icmp( name, "KEYB" ) ) {
            const KEYBDEF *kb;
            char code[4];
            int n = 0;
            cmd = skip_blanks( cmd + 4 );
            while ( n < 2 && *cmd && *cmd != ',' && *cmd != ' ' ) {
                code[n++] = (char)ch_upper( (u8)*cmd++ );
            }
            code[n] = 0;
            kb = inf_keyboard( code, 0 );
            if ( kb ) {
                opt.keyboard = kb;
            }
        } else if ( !tgt.olddir[0] && (!str_icmp( name, "PATH" ) ||
                    (!str_icmp( name, "SET" ) && !str_nicmp( skip_blanks( cmd + 3 ), "PATH", 4 ))) ) {
            /* the first directory of the PATH with the DOS commands */
            const char *list = str_chr( cmd, '=' );
            list = list ? list + 1 : skip_blanks( cmd + 4 );
            while ( *list && !tgt.olddir[0] ) {
                int len = 0;
                list = skip_blanks( list );
                while ( *list && *list != ';' && len < (int)sizeof( dir ) - 1 ) {
                    dir[len++] = *list++;
                }
                dir[len] = 0;
                if ( *list == ';' ) {
                    list++;
                }
                str_upper( dir );
                if ( len > 3 && dir[len - 1] == '\\' ) {
                    dir[len - 1] = 0;
                }
                if ( dir[0] == tgt.final && dir[1] == ':' && len > 3 ) {
                    char here[PATHLEN];
                    str_cpyn( here, dir, sizeof( here ) );
                    here[0] = (char)('A' + tgt.drive);
                    if ( has_command( here, "FORMAT.COM" ) || has_command( here, "CMD.EXE" ) ) {
                        str_cpyn( tgt.olddir, dir, sizeof( tgt.olddir ) );
                    }
                }
            }
        }
    }
    lines_free( &lines );

    if ( !tgt.olddir[0] ) {
        sfmt( dir, sizeof( dir ), "%c:\\DOS", 'A' + tgt.drive );
        if ( dir_exists( dir ) ) {
            str_cpyn( tgt.olddir, inf.dosdir, sizeof( tgt.olddir ) );
        }
    }
    if ( tgt.olddir[0] ) {
        tgt.olddir[0] = tgt.final;
        if ( !path_valid( tgt.olddir ) ) {
            tgt.olddir[0] = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* a new system                                                        */
/* ------------------------------------------------------------------ */

static int write_template( char **tmpl, int count, const char *name, int is_cfg )
{
    LINES lines;
    char path[PATHLEN], old[PATHLEN], line[LINELEN];
    int i, rc;
    u16 attr;

    lines_init( &lines, 16 );
    for ( i = 0; i < count; i++ ) {
        if ( !str_icmp( tmpl[i], "%COUNTRY%" ) ) {
            country_line( line, sizeof( line ) );
        } else if ( !str_icmp( tmpl[i], "%KEYB%" ) ) {
            keyb_line( line, sizeof( line ) );
        } else {
            fill_dosdir( line, tmpl[i], sizeof( line ) );
            if ( !line[0] ) {
                continue;
            }
        }
        if ( line[0] ) {
            lines_add( &lines, line );
        }
    }
    (void)is_cfg;
    root_path( path, name );
    if ( plat_get_attr( path, &attr ) == 0 ) {
        str_cpyn( old, path, sizeof( old ) );
        str_cpyn( str_rchr( old, '.' ), ".OLD", 5 );
        plat_set_attr( old, 0 );
        plat_delete( old );
        plat_set_attr( path, 0 );
        plat_rename( path, old );
    }
    rc = lines_write( &lines, path );
    lines_free( &lines );
    return rc;
}

int cfg_write_new( void )
{
    return write_template( inf.config, inf.nconfig, "CONFIG.SYS", 1 ) |
           write_template( inf.autoexec, inf.nautoexec, "AUTOEXEC.BAT", 0 );
}

/* ------------------------------------------------------------------ */
/* an upgrade                                                          */
/* ------------------------------------------------------------------ */

/* A DEVICE= or INSTALL= line for a program PM-DOS has a version of
   (SETVER.EXE) names it in the PM-DOS directory, which need not be the
   one the old DOS was in.  "val" points at the file name in the line. */
static void device_path( LINES *lines, int at, const char *val, const char *name )
{
    INFFILE *file = inf_file( name );
    const char *line = lines->line[at], *end = val;
    char *made;
    int size;

    if ( file == NULL || file->where != WH_DOS ) {
        return;
    }
    while ( *end && *end != ' ' && *end != '\t' && *end != '/' ) {
        end++;
    }
    size = (int)(val - line) + str_len( opt.dosdir ) + 1 + str_len( name ) + str_len( end ) + 1;
    made = (char *)xalloc( (u32)size );
    str_cpyn( made, line, (int)(val - line) + 1 );
    str_catn( made, opt.dosdir, size );
    str_catn( made, "\\", size );
    str_catn( made, name, size );
    str_catn( made, end, size );
    lines_set( lines, at, made );
    xfree( made );
}

static int upgrade_config( int full, int *emm_ems )
{
    LINES lines;
    char path[PATHLEN], word[24], name[16], line[LINELEN];
    const char *val;
    int i, j, top = 0, have_country = 0, rc;

    root_path( path, "CONFIG.SYS" );
    if ( lines_read( &lines, path ) ) {
        return 1;
    }
    for ( i = 0; i < lines.n; i++ ) {
        cfg_keyword( lines.line[i], word, sizeof( word ) );
        val = cfg_value( lines.line[i] );
        if ( !str_icmp( word, "COUNTRY" ) ) {
            rem_out( &lines, i );
            have_country = 1;
        } else if ( !full ) {
            continue;
        } else if ( !str_icmp( word, "SHELL" ) ) {
            rem_out( &lines, i );
        } else if ( !str_icmp( word, "DEVICE" ) || !str_icmp( word, "DEVICEHIGH" ) ||
                    !str_icmp( word, "INSTALL" ) || !str_icmp( word, "INSTALLHIGH" ) ) {
            program_of( val, name, sizeof( name ), 1 );
            if ( named_in( inf.cfgdrop, inf.ncfgdrop, name ) ) {
                if ( !str_icmp( name, "EMM386.EXE" ) ) {
                    char upper[LINELEN];
                    str_cpyn( upper, val, sizeof( upper ) );
                    str_upper( upper );
                    *emm_ems = 1;
                    for ( j = 0; upper[j]; j++ ) {
                        if ( !str_nicmp( upper + j, "NOEMS", 5 ) ) {
                            *emm_ems = 0;
                        }
                    }
                }
                rem_out( &lines, i );
            } else {
                device_path( &lines, i, val, name );
            }
        }
    }
    /* the new lines, at the top */
    if ( full ) {
        sfmt( line, sizeof( line ), "SHELL=%s\\CMD.EXE /P", opt.dosdir );
        lines_insert( &lines, top++, line );
    }
    country_line( line, sizeof( line ) );
    if ( line[0] && (full || have_country || opt.country) ) {
        lines_insert( &lines, top++, line );
    }
    if ( full ) {
        for ( j = 0; j < inf.ncfgadd; j++ ) {
            int found = 0;
            cfg_keyword( inf.cfgadd[j], word, sizeof( word ) );
            for ( i = top; i < lines.n; i++ ) {
                char other[24];
                cfg_keyword( lines.line[i], other, sizeof( other ) );
                if ( !str_icmp( other, word ) ) {
                    found = 1;
                }
            }
            if ( !found ) {
                lines_insert( &lines, top++, inf.cfgadd[j] );
            }
        }
        if ( *emm_ems ) {
            int found = 0;
            for ( i = 0; i < lines.n; i++ ) {
                cfg_keyword( lines.line[i], word, sizeof( word ) );
                if ( !str_icmp( word, "EMS" ) ) {
                    found = 1;
                }
            }
            if ( !found ) {
                lines_insert( &lines, top++, "EMS=AUTO" );
            }
        }
    }
    rc = lines_write( &lines, path );
    lines_free( &lines );
    return rc;
}

static int path_has( const char *list, const char *dir )
{
    int len = str_len( dir );

    while ( *list ) {
        list = skip_blanks( list );
        if ( !str_nicmp( list, dir, len ) &&
             (list[len] == ';' || list[len] == 0 || list[len] == ' ' ||
              (list[len] == '\\' && (list[len + 1] == ';' || !list[len + 1]))) ) {
            return 1;
        }
        while ( *list && *list != ';' ) {
            list++;
        }
        if ( *list ) {
            list++;
        }
    }
    return 0;
}

/* Line "at" is PATH (or SET PATH=) without the DOS directory.  PM-DOS's
   CMD.EXE runs the new AUTOEXEC.BAT and keeps 159 characters of a batch
   line, %PATH% and all once it is expanded - so no second line that adds
   to %PATH% can help a long one.  The directory goes in front when that
   fits; else it takes the old DOS directory's place in the list (an
   install in a directory of its own, the old one named on the PATH);
   and only when neither fits does the line run long. */
#define BATCH_LINE      127

static void path_first( LINES *lines, int at, const char *how, const char *list )
{
    char *line, *old;
    int len = str_len( how ) + str_len( opt.dosdir ) + 1 + str_len( list ) + 1;
    int olen = str_len( tgt.olddir ), i;

    line = (char *)xalloc( (u32)len + 1 );
    sfmt( line, len, "%s%s%s%s", how, opt.dosdir, *list ? ";" : "", list );
    if ( len - 1 > BATCH_LINE && olen && str_icmp( tgt.olddir, opt.dosdir ) ) {
        /* the old directory, as a whole entry of the list */
        for ( i = 0; list[i]; i++ ) {
            if ( (i == 0 || list[i - 1] == ';') && !str_nicmp( list + i, tgt.olddir, olen ) &&
                 (list[i + olen] == ';' || list[i + olen] == 0 ||
                  (list[i + olen] == '\\' && (list[i + olen + 1] == ';' || !list[i + olen + 1]))) ) {
                int rest = str_len( list + i + olen ), size;
                size = str_len( how ) + i + str_len( opt.dosdir ) + rest + 1;
                if ( size - 1 <= BATCH_LINE ) {
                    old = (char *)xalloc( (u32)size );
                    str_cpyn( old, how, size );
                    str_catn( old, list, str_len( how ) + i + 1 );
                    str_catn( old, opt.dosdir, size );
                    str_catn( old, list + i + olen, size );
                    xfree( line );
                    line = old;
                }
                break;
            }
        }
    }
    lines_set( lines, at, line );
    xfree( line );
}

static int upgrade_autoexec( int full )
{
    LINES lines;
    char path[PATHLEN], name[16], line[LINELEN], keyb[LINELEN];
    const char *cmd;
    int i, have_path = 0, keyb_at = -1, rc, at;

    root_path( path, "AUTOEXEC.BAT" );
    if ( lines_read( &lines, path ) ) {
        return 1;
    }
    for ( i = 0; i < lines.n; i++ ) {
        cmd = bat_command( lines.line[i] );
        program_of( cmd, name, sizeof( name ), 0 );
        if ( !str_icmp( name, "REM" ) || !name[0] || name[0] == ':' ) {
            continue;
        }
        if ( !str_icmp( name, "KEYB" ) ) {
            if ( keyb_at < 0 ) {
                keyb_at = i;
            }
            rem_out( &lines, i );
            continue;
        }
        if ( !full ) {
            continue;
        }
        if ( !str_icmp( name, "MODE" ) ) {
            /* MODE CON CODEPAGE ... / MODE CON CP ... */
            int j;
            for ( j = 0; cmd[j]; j++ ) {
                if ( !str_nicmp( cmd + j, "CODEPAGE", 8 ) || !str_nicmp( cmd + j, " CP ", 4 ) ) {
                    rem_out( &lines, i );
                    break;
                }
            }
        } else if ( !str_icmp( name, "SET" ) ) {
            const char *var = skip_blanks( cmd + 3 );
            if ( !str_nicmp( var, "COMSPEC", 7 ) ) {
                rem_out( &lines, i );
            } else if ( !str_nicmp( var, "PATH", 4 ) && str_chr( var, '=' ) ) {
                have_path = 1;
                if ( !path_has( str_chr( var, '=' ) + 1, opt.dosdir ) ) {
                    path_first( &lines, i, "SET PATH=", skip_blanks( str_chr( var, '=' ) + 1 ) );
                }
            }
        } else if ( !str_icmp( name, "PATH" ) ) {
            const char *list = cmd + 4;
            have_path = 1;
            list = skip_blanks( list );
            if ( *list == '=' ) {
                list = skip_blanks( list + 1 );
            }
            if ( *list != ';' && !path_has( list, opt.dosdir ) ) {
                path_first( &lines, i, "PATH ", list );
            }
        } else if ( named_in( inf.autodrop, inf.nautodrop, name ) ) {
            rem_out( &lines, i );
        }
    }
    keyb_line( keyb, sizeof( keyb ) );
    if ( keyb[0] ) {
        at = keyb_at >= 0 ? keyb_at + 1 : lines.n;
        lines_insert( &lines, at, keyb );
    }
    if ( full && !have_path ) {
        sfmt( line, sizeof( line ), "PATH %s", opt.dosdir );
        at = 0;
        if ( lines.n && !str_nicmp( skip_blanks( lines.line[0] ), "@ECHO", 5 ) ) {
            at = 1;
        }
        lines_insert( &lines, at, line );
    }
    rc = lines_write( &lines, path );
    lines_free( &lines );
    return rc;
}

/* full = an upgrade from another DOS; 0 = a reinstall of PM-DOS, which
   changes only the country and keyboard lines */
int cfg_upgrade( int full )
{
    int emm_ems = 0;

    return upgrade_config( full, &emm_ems ) | upgrade_autoexec( full );
}

/* a startup floppy's two files */
int cfg_floppy( int drive )
{
    LINES lines;
    char path[PATHLEN], line[LINELEN];
    int i, rc;

    lines_init( &lines, 16 );
    for ( i = 0; i < inf.nbootcfg; i++ ) {
        if ( !str_icmp( inf.bootcfg[i], "%COUNTRY%" ) ) {
            country_line( line, sizeof( line ) );
        } else {
            str_cpyn( line, inf.bootcfg[i], sizeof( line ) );
        }
        if ( line[0] ) {
            lines_add( &lines, line );
        }
    }
    sfmt( path, sizeof( path ), "%c:\\CONFIG.SYS", 'A' + drive );
    rc = lines_write( &lines, path );
    lines_free( &lines );

    lines_init( &lines, 16 );
    lines_add( &lines, "@ECHO OFF" );
    lines_add( &lines, "PROMPT $P$G" );
    sfmt( line, sizeof( line ), "PATH %c:\\", 'A' + drive );
    lines_add( &lines, line );
    keyb_line( line, sizeof( line ) );
    if ( line[0] ) {
        lines_add( &lines, line );
    }
    sfmt( path, sizeof( path ), "%c:\\AUTOEXEC.BAT", 'A' + drive );
    rc |= lines_write( &lines, path );
    lines_free( &lines );
    return rc;
}
