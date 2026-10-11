/*
 * INI.C - SCANDISK.INI.
 *
 * The file is MS-DOS ScanDisk's: an [ENVIRONMENT] section for how the
 * program behaves, and a [CUSTOM] section that says what a run started
 * with /CUSTOM does about each kind of problem.  A file written for
 * MS-DOS 6.22 is read as it stands: the settings that are about
 * DoubleSpace and DriveSpace volumes (CheckHost, DS_Header, Boot_Sector,
 * Invalid_MDFAT, DS_Crosslinks, DS_LostClust, DS_Signatures) are taken
 * and ignored, because PM-DOS has no compressed volumes.
 *
 * One word is this program's own: Display = Graphics, which is /G.
 * MS-DOS's Display = Mono is read and changes nothing: there is no
 * black and white screen here, and no /MONO.
 *
 * It is looked for where SCANDISK.EXE would be found: the current
 * directory, each directory of PATH, then the root of the boot drive.
 * A native program is not told where it was loaded from.
 *
 * Without a file, the settings are the ones the file ships with.
 */
#include "scandisk.h"

OPTIONS opt;

void ini_defaults( void )
{
    int cat;

    opt.mouse = 1;
    opt.timeout = 0;
    opt.passes = 1;
    opt.labelcheck = 0;
    opt.lfncheck = 1;
    opt.drive_summary = TRI_AUTO;
    opt.all_summary = TRI_AUTO;
    opt.cust_surface = SURF_NEVER;
    opt.save_log = LOG_OFF;
    opt.undo_prompt = 1;
    for ( cat = 0; cat < CAT_COUNT; cat++ ) {
        opt.action[cat] = IA_PROMPT;
    }
}

static int word_is( const char *value, const char *word )
{
    return str_icmp( value, word ) == 0;
}

static int on_off( const char *value, int now )
{
    if ( word_is( value, "On" ) ) {
        return 1;
    }
    if ( word_is( value, "Off" ) ) {
        return 0;
    }
    return now;
}

static int tri( const char *value, int now )
{
    if ( word_is( value, "Auto" ) ) {
        return TRI_AUTO;
    }
    if ( word_is( value, "On" ) ) {
        return TRI_ON;
    }
    if ( word_is( value, "Off" ) ) {
        return TRI_OFF;
    }
    return now;
}

/* what to do about a kind of problem; a word the setting does not take
   leaves it as it was */
static int action( const char *value, int now, const char *allowed )
{
    static const struct {
        const char *word;
        char letter;
        int  ia;
    } words[] = {
        { "Prompt", 'P', IA_PROMPT }, { "Fix", 'F', IA_FIX }, { "Quit", 'Q', IA_QUIT },
        { "Skip", 'S', IA_SKIP }, { "Delete", 'D', IA_DELETE }, { "Save", 'V', IA_SAVE }
    };
    int index;

    for ( index = 0; index < 6; index++ ) {
        if ( word_is( value, words[index].word ) && str_chr( allowed, words[index].letter ) ) {
            return words[index].ia;
        }
    }
    return now;
}

static void set_environment( const char *name, const char *value )
{
    u32 num = 0;
    const char *cur;

    if ( word_is( name, "Display" ) ) {
        /* MS-DOS's words are Auto, Mono, Color and Off.  There is one
           text screen here, a colour one, and all four mean that one. */
        opt.graphics = word_is( value, "Graphics" );
    } else if ( word_is( name, "Mouse" ) ) {
        opt.mouse = on_off( value, opt.mouse );
    } else if ( word_is( name, "ScanTimeOut" ) ) {
        opt.timeout = on_off( value, opt.timeout );
    } else if ( word_is( name, "NumPasses" ) ) {
        for ( cur = value; *cur >= '0' && *cur <= '9' && num < 65536ul; cur++ ) {
            num = num * 10 + (u32)(*cur - '0');
        }
        if ( *cur == 0 && num >= 1 && num <= 65535ul ) {
            opt.passes = num;
        }
    } else if ( word_is( name, "LabelCheck" ) ) {
        opt.labelcheck = on_off( value, opt.labelcheck );
    } else if ( word_is( name, "LfnCheck" ) ) {
        opt.lfncheck = on_off( value, opt.lfncheck );
    }
}

static void set_custom( const char *name, const char *value )
{
    if ( word_is( name, "DriveSummary" ) ) {
        opt.drive_summary = tri( value, opt.drive_summary );
    } else if ( word_is( name, "AllSummary" ) ) {
        opt.all_summary = tri( value, opt.all_summary );
    } else if ( word_is( name, "Surface" ) ) {
        if ( word_is( value, "Never" ) ) {
            opt.cust_surface = SURF_NEVER;
        } else if ( word_is( value, "Always" ) ) {
            opt.cust_surface = SURF_ALWAYS;
        } else if ( word_is( value, "Prompt" ) ) {
            opt.cust_surface = SURF_PROMPT;
        }
    } else if ( word_is( name, "SaveLog" ) ) {
        if ( word_is( value, "Off" ) ) {
            opt.save_log = LOG_OFF;
        } else if ( word_is( value, "Append" ) ) {
            opt.save_log = LOG_APPEND;
        } else if ( word_is( value, "Overwrite" ) ) {
            opt.save_log = LOG_OVERWRITE;
        }
    } else if ( word_is( name, "Undo" ) ) {
        if ( word_is( value, "Prompt" ) ) {
            opt.undo_prompt = 1;
        } else if ( word_is( value, "Never" ) ) {
            opt.undo_prompt = 0;
        }
    } else if ( word_is( name, "FAT_Media" ) ) {
        opt.action[CAT_MEDIA] = action( value, opt.action[CAT_MEDIA], "PFQ" );
    } else if ( word_is( name, "Okay_Entries" ) ) {
        opt.action[CAT_OKAY] = action( value, opt.action[CAT_OKAY], "PFQ" );
    } else if ( word_is( name, "Bad_Chain" ) ) {
        opt.action[CAT_CHAIN] = action( value, opt.action[CAT_CHAIN], "PFQ" );
    } else if ( word_is( name, "Crosslinks" ) ) {
        opt.action[CAT_CROSS] = action( value, opt.action[CAT_CROSS], "PFQ" );
    } else if ( word_is( name, "Mismatch_FAT" ) ) {
        opt.action[CAT_MISMATCH] = action( value, opt.action[CAT_MISMATCH], "PFQS" );
    } else if ( word_is( name, "Bad_Clusters" ) ) {
        opt.action[CAT_BADCLUS] = action( value, opt.action[CAT_BADCLUS], "PFQS" );
    } else if ( word_is( name, "Bad_Entries" ) ) {
        opt.action[CAT_BADENT] = action( value, opt.action[CAT_BADENT], "PDQ" );
    } else if ( word_is( name, "LostClust" ) ) {
        opt.action[CAT_LOST] = action( value, opt.action[CAT_LOST], "PVDQS" );
    }
}

/* one line: "[SECTION]", or "Name = Value ; comment" */
static void ini_line( char *line, int *section )
{
    char *cur = line, *name, *value, *end;

    while ( *cur == ' ' || *cur == '\t' ) {
        cur++;
    }
    if ( *cur == '[' ) {
        *section = str_nicmp( cur, "[ENVIRONMENT]", 13 ) == 0 ? 1
                   : str_nicmp( cur, "[CUSTOM]", 8 ) == 0 ? 2 : 0;
        return;
    }
    end = str_chr( cur, ';' );
    if ( end ) {
        *end = 0;
    }
    name = cur;
    value = str_chr( cur, '=' );
    if ( value == NULL || *section == 0 ) {
        return;
    }
    for ( end = value; end > name && (end[-1] == ' ' || end[-1] == '\t'); end-- ) {
    }
    *end = 0;
    value++;
    while ( *value == ' ' || *value == '\t' ) {
        value++;
    }
    for ( end = value + str_len( value ); end > value && (end[-1] == ' ' || end[-1] == '\t'); end-- ) {
    }
    *end = 0;
    if ( *section == 1 ) {
        set_environment( name, value );
    } else {
        set_custom( name, value );
    }
}

static int ini_read( const char *path )
{
    u32 handle, size, done, pos, start;
    char *data, line[200];
    int section = 0;

    if ( sys_open_read( path, &handle ) != 0 ) {
        return 0;
    }
    if ( sys_file_size( handle, &size ) != 0 || size > 65536ul ) {
        sys_close( handle );
        return 0;
    }
    data = (char *)xalloc( size + 1 );
    if ( sys_read( handle, data, size, &done ) != 0 ) {
        done = 0;
    }
    sys_close( handle );
    data[done] = 0;
    for ( pos = 0; pos < done; pos++ ) {
        start = pos;
        while ( pos < done && data[pos] != '\r' && data[pos] != '\n' && data[pos] != 0x1A ) {
            pos++;
        }
        if ( pos - start < sizeof( line ) ) {
            mem_cpy( line, data + start, pos - start );
            line[pos - start] = 0;
            ini_line( line, &section );
        }
        if ( pos < done && data[pos] == 0x1A ) {
            break;
        }
    }
    xfree( data );
    return 1;
}

int ini_load( void )
{
    const char *path = sys_getenv( "PATH" );
    char trial[PATH_MAX];
    u32 len, end;

    if ( ini_read( "SCANDISK.INI" ) ) {
        return 1;
    }
    while ( path && *path ) {
        for ( len = 0; path[len] && path[len] != ';'; len++ ) {
        }
        if ( len && len < PATH_MAX - 14 ) {
            mem_cpy( trial, path, len );
            end = len;
            if ( trial[end - 1] != '\\' && trial[end - 1] != ':' ) {
                trial[end++] = '\\';
            }
            str_cpy( trial + end, "SCANDISK.INI" );
            if ( ini_read( trial ) ) {
                return 1;
            }
        }
        path += len;
        if ( *path == ';' ) {
            path++;
        }
    }
    sfmt( trial, sizeof( trial ), "%c:\\SCANDISK.INI", 'A' + sys_boot_drive() );
    return ini_read( trial );
}
