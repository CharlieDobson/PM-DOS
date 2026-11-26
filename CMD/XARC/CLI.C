/*
 * CLI.C - XARC from the command line: the DOS build's command-line half.
 *
 *   XARC L <archive>                list
 *   XARC T <archive>                test
 *   XARC X <archive> [dir]          extract all
 *   XARC A <archive> <items...>     make a .ZIP or .7Z
 *   XARC D <archive> <specs...>     delete entries
 *   XARC MEM                        the memory report
 *   XARC /?                         this
 *
 * No screen is set up and nothing waits for a key but A's question about
 * replacing an archive and D's about deleting (both on stderr, both
 * answered by /y), so the output can be sent to a file.  The exit
 * codes are the DOS build's: 0 done, 1 a mistake on the command line,
 * 2 the archive could not be opened (or nothing matched), 3 the work
 * failed.
 */
#include "xarc.h"

const char *base_name( const char *path )
{
    const char *at, *base = path ? path : "";

    for ( at = base; *at; at++ ) {
        if ( *at == '\\' || *at == '/' || *at == ':' ) {
            base = at + 1;
        }
    }
    return base;
}

/* "Every CRC matched" is true of 7z, zip and RAR; for a cabinet or a
   COMPRESS.EXE file the backend says what WAS checked */
const char *test_passed_text( ArcFile *arc )
{
    static char text[320];
    const char *note = ArcIntegrityNote( arc );

    if ( note == NULL ) {
        return "Integrity OK.  Every entry decoded and every CRC matched.";
    }
    _snprintf( text, sizeof( text ), "Integrity OK.  %.290s", note );
    return text;
}

/* a machine below even a zip, said once at the start */
void mem_warn_text( char *buf, int size )
{
    _snprintf( buf, (size_t)size, "%s\n\nXARC MEM shows the figures.", ArcMemWarnText() );
}

/* a 64-bit total, grouped as the country has it */
void fmt_total( uint64_t val, char *buf )
{
    char digits[24], rev[24];
    int len = 0, at;

    do {
        rev[len++] = (char)('0' + xa_div64( &val, 10 ));
    } while ( val != 0 );
    for ( at = 0; at < len; at++ ) {
        digits[at] = rev[len - 1 - at];
    }
    digits[len] = 0;
    NumFmtDigits( digits, buf );
}

static int cli_progress( void *user, int idx, int total, const char *name )
{
    (void)user;
    printf( "  %5d/%-5d  %s\n", idx + 1, total, name ? name : "" );
    return 1;
}

void cli_usage( void )
{
    printf( "XArchive, a compression and extraction utility.\n\n" );
    printf( "XARC [<option> <archive> [dir|<filespec>] [switches]]\n" );
    printf( "XARC mem\n\n" );
    printf( "Options:\n" );
    printf( "  a <archive> <dir>    Add to archive.\n" );
    printf( "  d <archive> <files>  Delete files from archive.\n" );
    printf( "  l <archive>          List archive contents.\n" );
    printf( "  t <archive>          Test archive integrity.\n" );
    printf( "  x <archive> [dir]    Extract files from archive.\n" );
    printf( "  mem                  Show memory report.\n\n" );
    printf( "Switches:\n" );
    printf( "  /g                   Use VGA graphics mode.\n" );
    printf( "  /nopaths             Do not store or create folders from archive.\n" );
    printf( "  /p<password>         Password for an encrypted archive.\n" );
    printf( "                       NOTE: visible in the command line.\n" );
    printf( "  /r                   Recursive file search.\n" );
    printf( "  /x<filespec|@file>   Excludes named files or specifies list file.\n" );
    printf( "  /y                   Do not prompt to replace archive or delete files.\n" );
    printf( "  /ac                  Clear archive attribute after compression.\n" );
}

int cli_is_help( const char *arg )
{
    return !stricmp( arg, "/?" ) || !stricmp( arg, "-?" ) || !stricmp( arg, "/h" )
           || !stricmp( arg, "-h" ) || !stricmp( arg, "--help" ) || !stricmp( arg, "/help" )
           || !stricmp( arg, "help" );
}

int cli_is_mem( const char *arg )
{
    return !stricmp( arg, "mem" ) || !stricmp( arg, "/mem" ) || !stricmp( arg, "-mem" );
}

/* a lone l, t, x, a or d; anything else is an archive for the screen */
int cli_is_cmd( const char *arg )
{
    int ch;

    if ( arg[0] && arg[1] == 0 ) {
        ch = tolower( (unsigned char)arg[0] );
        return ch == 'l' || ch == 't' || ch == 'x' || ch == 'a' || ch == 'd';
    }
    return 0;
}

/* "/g" and "/t", either slash and either case: taken out of the
   arguments before anything else reads them, and never saved */
int cli_video_switch( const char *arg, int *graphics )
{
    if ( arg == NULL || (arg[0] != '/' && arg[0] != '-') || arg[1] == 0 || arg[2] != 0 ) {
        return 0;
    }
    if ( tolower( (unsigned char)arg[1] ) == 'g' ) {
        *graphics = 1;
        return 1;
    }
    if ( tolower( (unsigned char)arg[1] ) == 't' ) {
        *graphics = 0;
        return 1;
    }
    return 0;
}

/* The readings behind the budget, where a user can send them to a file.
   The DOS build's report began with the XMS driver's version; a native
   PM-DOS program has no XMS between it and the kernel, so that line is
   gone and the first is what 21h/48h says it could give in one block. */
int cli_memory( void )
{
    uint32_t sys_kb = 0, big_kb = 0, bud_kb = 0, have_kb = 0, need_kb = 0;
    char b1[NUM_FMT_MAX], b2[NUM_FMT_MAX], b3[NUM_FMT_MAX], b4[NUM_FMT_MAX];

    ArcMemReportKB( &sys_kb, &big_kb, &bud_kb );
    printf( "\nMemory available to XArchive on this machine\n\n" );
    if ( sys_kb ) {
        printf( "  Largest free block               : %s KB\n", NumFmt( sys_kb, b1 ) );
    } else {
        printf( "  Largest free block               : not reported\n" );
    }
    printf( "  Largest block actually obtained  : %s KB\n", NumFmt( big_kb, b2 ) );
    printf( "  Budget for one archive           : %s KB\n", NumFmt( bud_kb, b3 ) );
    ArcMemStartupOk( &have_kb, &need_kb );
    printf( "  Smallest archive that will fit   : %s KB (a zip)\n\n", NumFmt( need_kb, b4 ) );
    if ( have_kb >= need_kb ) {
        printf( "  This machine can extract archives.  How large an archive it\n"
                "  can manage depends on the archive, not on its format - use\n"
                "  Archive Info on a particular file to see what that one needs.\n\n" );
    } else {
        printf( "  THIS MACHINE CANNOT EXTRACT ANY ARCHIVE.  Archives can still\n"
                "  be listed and browsed.\n\n" );
    }
    printf( "  Preferences file: %s\n\n", ArcPrefFile() );
    return 0;
}

static int cli_list( ArcFile *arc )
{
    char date[24], attr[16], size[NUM_FMT_MAX];
    int at, count = ArcNumEntries( arc );

    printf( "%-44s %13s  %-16s %s\n", "Name", "Size", "Modified", "Attr" );
    printf( "-------------------------------------------------------------------------------\n" );
    for ( at = 0; at < count; at++ ) {
        ArcEntryDate( arc, at, date, sizeof( date ) );
        ArcEntryAttr( arc, at, attr, sizeof( attr ) );
        if ( ArcEntryIsDir( arc, at ) ) {
            printf( "%-44s %13s  %-16s %s\n", ArcEntryName( arc, at ), "<DIR>", date, attr );
        } else {
            NumFmt( ArcEntrySize( arc, at ), size );
            printf( "%-44s %13s  %-16s %s\n", ArcEntryName( arc, at ), size, date, attr );
        }
    }
    printf( "\n%d entr%s\n", count, count == 1 ? "y" : "ies" );
    return SZ_OK;
}

static void cli_report( ArcFile *arc, int rc, const char *ok_text )
{
    if ( rc == SZ_OK ) {
        printf( "%s\n", ok_text );
    } else if ( rc == SZ_ERR_UNSUPPORTED ) {
        printf( "Unsupported feature:\n%s\n", ArcUnsupportedHint( arc ) );
    } else if ( rc == SZ_ERR_CANCEL ) {
        printf( "Cancelled.\n" );
    } else if ( rc == SZ_ERR_NORAM || rc == SZ_ERR_MEMORY ) {
        printf( "FAILED: %s\n\n%s\n", ArcErrorText( rc ),
                rc == SZ_ERR_MEMORY ? ArcMemFailHint() : ArcNoRamHint() );
    } else if ( rc == SZ_ERR_PASSWORD ) {
        printf( "FAILED: this archive is encrypted.\nGive the password with /p, e.g.  /pSECRET\n" );
    } else if ( rc == SZ_ERR_BADPASS ) {
        printf( "FAILED: wrong password.\n" );
    } else {
        printf( "FAILED: %s\n", ArcErrorText( rc ) );
    }
}

static int is_no_paths( const char *arg )
{
    return (arg[0] == '/' || arg[0] == '-') && !stricmp( arg + 1, "nopaths" );
}

/* "/p<password>": NULL if this is not one; "" is a password too */
static const char *cli_password( const char *arg )
{
    if ( (arg[0] != '/' && arg[0] != '-') || tolower( (unsigned char)arg[1] ) != 'p' ) {
        return NULL;
    }
    return arg + 2;
}

static int last_file = -1;

static int cli_comp_progress( void *user, int idx, int total, const char *name,
                              uint32_t done_kb, uint32_t total_kb )
{
    (void)user;
    (void)done_kb;
    (void)total_kb;
    if ( total >= 0 && idx != last_file ) {
        last_file = idx;
        printf( "  %5d/%-5d  %s\n", idx + 1, total, name ? name : "" );
    }
    return 1;
}

/* a key from standard input, unechoed (21h/08h) */
static int read_key( void )
{
    REGS regs;

    xa_clear( &regs );
    xa_dos( &regs, 0x0800 );
    return (int)(regs.eax & 0xFF);
}

/* PKZIP's switches where XArc has the same idea, 7-Zip's where it does
   not: the extension picks the format, a folder goes in whole */
static int cli_add( int argc, char **argv )
{
    ArcCompJob *job = ArcCompCreate();
    const char *archive = NULL, *arg;
    char path[SZ_MAX_NAME + 8], b1[NUM_FMT_MAX], b2[NUM_FMT_MAX];
    int at, rc = SZ_OK, yes = 0, items = 0, fmt, key;
    uint32_t in, out, pct;
    FILE *there;

    if ( job == NULL ) {
        printf( "Out of memory.\n" );
        return 3;
    }
    for ( at = 2; at < argc; at++ ) {
        arg = argv[at];
        if ( arg[0] == '/' || arg[0] == '-' ) {
            if ( is_no_paths( arg ) ) {
                ArcCompSetPaths( job, 0 );
                continue;
            }
            if ( !stricmp( arg + 1, "r" ) ) {
                ArcCompSetRecurse( job, 1 );
                continue;
            }
            if ( !stricmp( arg + 1, "y" ) ) {
                yes = 1;
                continue;
            }
            if ( !stricmp( arg + 1, "ac" ) ) {
                ArcCompSetClearArchive( job, 1 );
                continue;
            }
            if ( !stricmp( arg + 1, "ex" ) ) {
                continue;
            }
            if ( tolower( (unsigned char)arg[1] ) == 'x' && arg[2] ) {
                rc = ArcCompExclude( job, arg + 2 );
                if ( rc != SZ_OK ) {
                    printf( "Cannot read the exclusion list '%s'.\n", ArcCompProblem( job ) );
                    ArcCompFree( job );
                    return 2;
                }
                continue;
            }
            printf( "Unknown switch '%s'.\n\n", arg );
            cli_usage();
            ArcCompFree( job );
            return 1;
        }
        if ( archive == NULL ) {
            archive = arg;
            continue;
        }
        if ( ArcCompAdd( job, arg ) != SZ_OK ) {
            printf( "Out of memory.\n" );   /* the list of what to add */
            ArcCompFree( job );
            return 3;
        }
        items++;
    }
    if ( archive == NULL || !items ) {
        printf( "Name the archive, then what to put in it.\n\n" );
        cli_usage();
        ArcCompFree( job );
        return 1;
    }
    /* no extension: a zip, the way PKZIP named them */
    strncpy( path, archive, SZ_MAX_NAME - 1 );
    path[SZ_MAX_NAME - 1] = 0;
    if ( !strchr( base_name( path ), '.' ) ) {
        strcat( path, ".ZIP" );
    }
    fmt = ArcCompFormatFromName( path );
    ArcCompSetFormat( job, fmt );
    there = fopen( path, "rb" );
    if ( there ) {
        fclose( there );
        if ( !yes ) {
            /* on stderr: with the listing in a file, a question on stdout
               is a program that seems to hang */
            fprintf( stderr, "%s already exists.  Replace it (Y/N)? ", path );
            key = read_key();
            fprintf( stderr, "%c\n", key >= 32 && key < 127 ? key : ' ' );
            if ( key != 'y' && key != 'Y' ) {
                printf( "Not replaced.\n" );
                ArcCompFree( job );
                return 2;
            }
        }
    }
    printf( "Creating: %s   (%s)\n", path, fmt == ARC_CF_7Z ? "7-Zip" : "Zip" );
    last_file = -1;
    rc = ArcCompScan( job, path, cli_comp_progress, NULL );
    if ( ArcCompMissing( job ) ) {
        printf( "Warning: %d name%s matched nothing (the first: %s).\n", ArcCompMissing( job ),
                ArcCompMissing( job ) == 1 ? "" : "s", ArcCompProblem( job ) );
    }
    if ( rc == SZ_OK ) {
        rc = ArcCompRun( job, path, cli_comp_progress, NULL );
    }
    if ( rc == SZ_OK ) {
        in = ArcCompInBytes( job );
        out = ArcCompOutBytes( job );
        pct = in > 0 ? (in > 0x01000000UL ? out / (in / 100) : out * 100 / in) : 100;
        printf( "\nDone.  %d file%s, %s bytes in, %s bytes out (%lu%%).\n", ArcCompFiles( job ),
                ArcCompFiles( job ) == 1 ? "" : "s", NumFmt( in, b1 ), NumFmt( out, b2 ),
                (unsigned long)pct );
        if ( ArcCompDictSize( job ) >= 1024UL * 1024 ) {
            printf( "Extracting it will need about %lu MB of memory.\n",
                    (unsigned long)(ArcCompDictSize( job ) / (1024UL * 1024)) );
        }
        if ( ArcCompSkipped( job ) ) {
            printf( "Left out: %d file%s", ArcCompSkipped( job ), ArcCompSkipped( job ) == 1 ? "" : "s" );
            if ( ArcCompDuplicates( job ) ) {
                printf( ", %d with a name already in it", ArcCompDuplicates( job ) );
            }
            if ( ArcCompUnreadable( job ) ) {
                printf( ", %d unreadable", ArcCompUnreadable( job ) );
            }
            if ( ArcCompTooLong( job ) ) {
                printf( ", %d with a name too long", ArcCompTooLong( job ) );
            }
            if ( ArcCompTooBig( job ) ) {
                printf( ", %d of 2 GB or more", ArcCompTooBig( job ) );
            }
            printf( ".\nThe first was %s\n", ArcCompProblem( job ) );
        }
        ArcCompFree( job );
        return 0;
    }
    switch ( rc ) {
    case SZ_ERR_NOFILES:
        printf( "Nothing to compress: %s\n",
                ArcCompMissing( job ) ? "no file matched." : "everything was excluded." );
        break;
    case SZ_ERR_READ:
        printf( "FAILED: could not read %s\nNothing was written.\n", ArcCompProblem( job ) );
        break;
    case SZ_ERR_WRITE:
        printf( "FAILED: cannot write %s (disk full or write-protected?)\n", path );
        break;
    case SZ_ERR_TOOBIG:
        printf( "FAILED: too much for one archive (a zip holds 65,535 entries; "
                "XArc writes up to 2 GB).\n" );
        break;
    case SZ_ERR_MEMORY:
    case SZ_ERR_NORAM:
        printf( "FAILED: not enough memory.  A zip needs about 300 KB, a 7-Zip\n"
                "archive at least 1.5 MB.\n" );
        break;
    default:
        printf( "FAILED: %s\n", ArcErrorText( rc ) );
        break;
    }
    ArcCompFree( job );
    return rc == SZ_ERR_NOFILES ? 2 : 3;
}

/* The archive a command names, or why it will not open: 0 with *out
   set, or the exit code 2.  The memory warning goes to stderr before
   the command, because L will work where X will not. */
static int cli_open( const char *path, const char *pw, ArcFile **out )
{
    char warn[400];
    int rc;

    *out = NULL;
    if ( !ArcMemStartupOk( NULL, NULL ) ) {
        mem_warn_text( warn, sizeof( warn ) );
        fprintf( stderr, "%s\n\n", warn );
    }
    rc = ArcOpenPw( path, pw, out );
    if ( rc == SZ_ERR_PASSWORD ) {
        printf( "Cannot open '%s': it is encrypted.\n"
                "Give the password with /p, e.g.  XARC L %s /pSECRET\n", path, path );
    } else if ( rc == SZ_ERR_BADPASS ) {
        printf( "Cannot open '%s': wrong password.\n", path );
    } else if ( rc != SZ_OK ) {
        printf( "Cannot open '%s': %s\n", path, ArcErrorText( rc ) );
    }
    if ( rc != SZ_OK ) {
        *out = NULL;
        return 2;
    }
    if ( pw ) {
        ArcSetPassword( *out, pw );
    }
    printf( "Archive: %s\nFormat : %s   (%d entries)\n\n", path, ArcFormatName( *out ),
            ArcNumEntries( *out ) );
    return 0;
}

/* ------------------------------------------------------------------ */
/* XARC D                                                              */
/* ------------------------------------------------------------------ */

/* A filespec is an entry name, either case, '/' and '\' alike, with *
   and ? that stop at a folder separator - "*.TXT" is the root's text
   files and "DOCS\*.TXT" the ones in DOCS.  One that names a folder
   takes everything under it, whether or not the archive stored an
   entry for the folder itself; "*.*" is "*", as DOS has it. */
static int is_sep( int ch )
{
    return ch == '\\' || ch == '/';
}

static int fold( int ch )
{
    return ch == '/' ? '\\' : toupper( (unsigned char)ch );
}

/* does "pat" match the "len" characters at "name"? */
static int wild( const char *pat, const char *name, int len )
{
    int at = 0;

    for ( ; *pat; pat++ ) {
        if ( *pat == '*' ) {
            while ( pat[1] == '*' ) {
                pat++;
            }
            for ( ;; ) {
                if ( wild( pat + 1, name + at, len - at ) ) {
                    return 1;
                }
                if ( at >= len || is_sep( (unsigned char)name[at] ) ) {
                    return 0;
                }
                at++;
            }
        }
        if ( at >= len ) {
            return 0;
        }
        if ( *pat == '?' ) {
            if ( is_sep( (unsigned char)name[at] ) ) {
                return 0;
            }
        } else if ( fold( *pat ) != fold( name[at] ) ) {
            return 0;
        }
        at++;
    }
    return at == len;
}

/* the filespec as wild() wants it, into "dst" (SZ_MAX_NAME): no
   separator at either end, and each "*.*" a "*" */
static void spec_prep( const char *spec, char *dst )
{
    int len = 0;

    while ( is_sep( (unsigned char)*spec ) ) {
        spec++;
    }
    while ( *spec && len < SZ_MAX_NAME - 1 ) {
        if ( spec[0] == '*' && spec[1] == '.' && spec[2] == '*'
             && (len == 0 || is_sep( (unsigned char)dst[len - 1] ))
             && (spec[3] == 0 || is_sep( (unsigned char)spec[3] )) ) {
            dst[len++] = '*';
            spec += 3;
            continue;
        }
        dst[len++] = *spec++;
    }
    while ( len > 0 && is_sep( (unsigned char)dst[len - 1] ) ) {
        len--;
    }
    dst[len] = 0;
}

/* the entry, or any folder its stored path runs through */
static int spec_matches( const char *spec, const char *name )
{
    int len, at;

    if ( name == NULL ) {
        return 0;
    }
    while ( is_sep( (unsigned char)*name ) ) {
        name++;
    }
    len = (int)strlen( name );
    while ( len > 0 && is_sep( (unsigned char)name[len - 1] ) ) {
        len--;
    }
    if ( len == 0 || !spec[0] ) {
        return 0;
    }
    if ( wild( spec, name, len ) ) {
        return 1;
    }
    for ( at = 1; at < len; at++ ) {
        if ( is_sep( (unsigned char)name[at] ) && wild( spec, name, at ) ) {
            return 1;
        }
    }
    return 0;
}

static int cli_delete( int argc, char **argv )
{
    ArcFile *arc = NULL;
    const char *archive = NULL, *pw = NULL, *found, *first_miss = NULL, *arg;
    const char **typed;
    char **specs, num[NUM_FMT_MAX];
    int *idx = NULL;
    int at, one, count = 0, nspecs = 0, misses = 0, hits = 0, yes = 0, rc = SZ_OK, code = 0, key;

    /* one per argument at most, so argc of them is enough; "typed" keeps
       each as written, for the warning */
    specs = (char **)malloc( (size_t)(argc > 0 ? argc : 1) * sizeof( char * ) );
    typed = (const char **)malloc( (size_t)(argc > 0 ? argc : 1) * sizeof( const char * ) );
    if ( specs == NULL || typed == NULL ) {
        free( specs );
        free( (void *)typed );
        printf( "Out of memory.\n" );
        return 3;
    }
    for ( at = 2; at < argc && code == 0; at++ ) {
        arg = argv[at];
        if ( arg[0] == '/' || arg[0] == '-' ) {
            if ( !stricmp( arg + 1, "y" ) ) {
                yes = 1;
            } else if ( (found = cli_password( arg )) != NULL ) {
                pw = found;
            } else {
                printf( "Unknown switch '%s'.\n\n", arg );
                cli_usage();
                code = 1;
            }
            continue;
        }
        if ( archive == NULL ) {
            archive = arg;
            continue;
        }
        specs[nspecs] = (char *)malloc( SZ_MAX_NAME );
        if ( specs[nspecs] == NULL ) {
            printf( "Out of memory.\n" );
            code = 3;
            continue;
        }
        spec_prep( arg, specs[nspecs] );
        typed[nspecs] = arg;
        nspecs++;
    }
    if ( code == 0 && (archive == NULL || !nspecs) ) {
        printf( "Name the archive, then what to delete from it.\n\n" );
        cli_usage();
        code = 1;
    }
    if ( code == 0 ) {
        code = cli_open( archive, pw, &arc );
    }
    if ( code == 0 ) {
        count = ArcNumEntries( arc );
        idx = (int *)malloc( (size_t)(count > 0 ? count : 1) * sizeof( int ) );
        if ( idx == NULL ) {
            printf( "Out of memory.\n" );
            code = 3;
        }
    }
    if ( code == 0 ) {
        for ( at = 0; at < count; at++ ) {
            for ( one = 0; one < nspecs && !spec_matches( specs[one], ArcEntryName( arc, at ) ); one++ ) {
            }
            if ( one < nspecs ) {
                idx[hits++] = at;
            }
        }
        for ( one = 0; one < nspecs; one++ ) {
            for ( at = 0; at < count && !spec_matches( specs[one], ArcEntryName( arc, at ) ); at++ ) {
            }
            if ( at == count ) {
                if ( !misses ) {
                    first_miss = typed[one];
                }
                misses++;
            }
        }
        if ( misses ) {
            printf( "Warning: %d name%s matched nothing (the first: %s).\n", misses,
                    misses == 1 ? "" : "s", first_miss );
        }
        if ( hits == 0 ) {
            printf( "Nothing to delete: no entry matched.\n" );
            code = 2;
        }
    }
    if ( code == 0 ) {
        rc = ArcDeleteCheck( arc, idx, hits );
        if ( rc == SZ_OK && !yes ) {
            /* on stderr: with the output in a file, a question on stdout
               is a program that seems to hang */
            fprintf( stderr, "Delete %s entr%s from %s (Y/N)? ", NumFmt( (uint32_t)hits, num ),
                     hits == 1 ? "y" : "ies", base_name( archive ) );
            key = read_key();
            fprintf( stderr, "%c\n", key >= 32 && key < 127 ? key : ' ' );
            if ( key != 'y' && key != 'Y' ) {
                printf( "Not deleted.\n" );
                code = 2;
            }
        }
    }
    if ( code == 0 ) {
        if ( rc == SZ_OK ) {
            rc = ArcDeleteItems( arc, idx, hits, NULL, NULL );
        }
        if ( rc == SZ_OK ) {
            printf( "Done.  %s entr%s deleted.\n", NumFmt( (uint32_t)hits, num ), hits == 1 ? "y" : "ies" );
        } else if ( rc == SZ_ERR_UNSUPPORTED ) {
            printf( "FAILED: %s\n", ArcDeleteHint() );
        } else if ( rc == SZ_ERR_WRITE ) {
            printf( "FAILED: The archive could not be written.  It may be read-only, open in "
                    "another program, or on a full disk.\n" );
        } else {
            cli_report( arc, rc, "" );
        }
        code = rc == SZ_OK ? 0 : 3;
    }
    if ( arc ) {
        ArcClose( arc );
    }
    free( idx );
    for ( one = 0; one < nspecs; one++ ) {
        free( specs[one] );
    }
    free( specs );
    free( (void *)typed );
    return code;
}

int cli_run( int argc, char **argv )
{
    ArcFile *arc;
    const char *cmd = argv[1], *path, *dest, *pos[4], *pw = NULL, *found;
    int npos = 0, at, rc;

    if ( tolower( (unsigned char)cmd[0] ) == 'a' ) {
        return cli_add( argc, argv );
    }
    if ( tolower( (unsigned char)cmd[0] ) == 'd' ) {
        return cli_delete( argc, argv );
    }
    /* the switches out of the way, wherever they were written */
    for ( at = 2; at < argc; at++ ) {
        if ( is_no_paths( argv[at] ) ) {
            ArcSetFlattenPaths( 1 );
            continue;
        }
        if ( (found = cli_password( argv[at] )) != NULL ) {
            pw = found;
            continue;
        }
        if ( argv[at][0] == '/' || argv[at][0] == '-' ) {
            printf( "Unknown switch '%s'.\n\n", argv[at] );
            cli_usage();
            return 1;
        }
        if ( npos < 4 ) {
            pos[npos++] = argv[at];
        }
    }
    if ( npos < 1 ) {
        printf( "Missing archive name.\n\n" );
        cli_usage();
        return 1;
    }
    path = pos[0];
    rc = cli_open( path, pw, &arc );
    if ( rc != 0 ) {
        return rc;
    }
    switch ( tolower( (unsigned char)cmd[0] ) ) {
    case 'l':
        rc = cli_list( arc );
        break;
    case 't':
        rc = ArcTestAll( arc, cli_progress, NULL );
        cli_report( arc, rc, test_passed_text( arc ) );
        break;
    case 'x':
        dest = npos >= 2 ? pos[1] : ".";
        printf( "Extracting to: %s%s\n", dest, ArcFlattenPaths() ? "   (without folder names)" : "" );
        rc = ArcExtractAll( arc, dest, cli_progress, NULL );
        cli_report( arc, rc, "Done.  All entries extracted." );
        break;
    default:
        cli_usage();
        rc = 1;
        break;
    }
    ArcClose( arc );
    return rc == SZ_OK ? 0 : 3;
}
