/*
 * MAIN.C - XCOPY: the command line, what the source and the destination
 * are, and the end.
 *
 *   XCOPY source [destination] [/A | /M] [/D[:date]] [/P] [/S [/E]] [/V]
 *         [/W] [/C] [/I] [/Q] [/F] [/L] [/H] [/R] [/T] [/U] [/K] [/N]
 *         [/Y | /-Y]
 *
 * WINDOWS 98's XCOPY32 IS THE MODEL - the one that keeps long names -
 * and not the real-mode XCOPY in front of it, which shows 8.3 names
 * and says "Reading source file(s)..." first.  So: names as they are on
 * the disk, every switch it has, its messages and its questions, and
 * DOS's errorlevels (0 copied, 1 nothing found, 2 ^C, 4 could not
 * start or out of space, 5 a read or write failed).
 *
 * THE DESTINATION IS WORKED OUT BEFORE ANYTHING IS COPIED, and it is
 * one of four things.  A directory, if it is one, ends in a backslash,
 * or /I says so for a multi-file copy.  A file name, if a file of that
 * name exists.  A template, if its last element has wildcards - each
 * copy is renamed through it, as COPY renames.  Otherwise XCOPY asks
 * "file or directory?", which is DOS's question and DOS's answer to not
 * knowing.  A file name is a template with no wildcards in it.
 */
#include "xcopy.h"

OPTIONS opt;
JOB     job;

static char params[2][PATH_MAX];
static int  nparams;
static int  old_verify = -1;
static int  multi;              /* more than one file is being copied */

extern u8 in_prompt;            /* DOSINT.ASM: ^C ends the program now */

void quit( int code )
{
    if ( old_verify >= 0 ) {
        sys_set_verify( old_verify );
    }
    sys_exit( code );
}

void out_of_memory( void )
{
    out_line( M_NOMEM );
    quit( EXIT_INIT );
}

/* ^C while a question is waiting: nothing is half-done, so just go.
   Called from the INT 23h handler (DOSINT.ASM), which never returns,
   and from ask() when the ^C came in before the question did. */
void __cdecl break_exit( void )
{
    sys_break_abandon();
    in_prompt = 0;
    out_line( "" );
    quit( EXIT_BREAK );
}

/* A question and a one-key answer out of "keys", echoed.  -1 when the
   input runs out.  ^C ends the program from inside the read. */
int ask( const char *tmpl, const char *arg, const char *keys )
{
    char echo[2];
    int key, pos;

    out_prompt( tmpl, arg );
    in_prompt = 1;
    if ( sys_break_hit() ) {                /* a ^C already on its way */
        break_exit();
    }
    for ( ;; ) {
        key = sys_read_key();
        if ( key < 0 ) {
            in_prompt = 0;
            out_line( "" );
            return -1;
        }
        key = ch_upper( key );
        for ( pos = 0; keys[pos]; pos++ ) {
            if ( keys[pos] == key ) {
                in_prompt = 0;
                echo[0] = (char)key;
                echo[1] = 0;
                out_line( echo );
                return key;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

static int days_in( int month, int year )
{
    static const u8 days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if ( month == 2 && (year % 4) == 0 ) {
        return 29;                          /* 1980-2107: no century rule */
    }
    return days[month - 1];
}

/* /D:date in the country's order, any of - / . or its own separator
   between the numbers, a two-digit year in 1980-2079 */
static int parse_date( const char *text, u32 len, u16 *out )
{
    u32 nums[3], val;
    int digits[3], count = 0, order, month, day, year;
    char sep;
    u32 pos = 0;

    sys_date_format( &order, &sep );
    while ( count < 3 ) {
        val = 0;
        digits[count] = 0;
        while ( pos < len && text[pos] >= '0' && text[pos] <= '9' ) {
            if ( digits[count] == 4 ) {
                return -1;
            }
            val = val * 10 + (u32)(text[pos] - '0');
            digits[count]++;
            pos++;
        }
        if ( digits[count] == 0 ) {
            return -1;
        }
        nums[count++] = val;
        if ( pos == len ) {
            break;
        }
        if ( count == 3 || (text[pos] != '-' && text[pos] != '/' && text[pos] != '.'
                            && text[pos] != sep) ) {
            return -1;
        }
        pos++;
    }
    if ( count != 3 ) {
        return -1;
    }
    if ( order == 1 ) {
        day = (int)nums[0];
        month = (int)nums[1];
        year = (int)nums[2];
    } else if ( order == 2 ) {
        year = (int)nums[0];
        month = (int)nums[1];
        day = (int)nums[2];
        digits[2] = digits[0];
    } else {
        month = (int)nums[0];
        day = (int)nums[1];
        year = (int)nums[2];
    }
    if ( digits[2] <= 2 ) {
        year += year < 80 ? 2000 : 1900;
    }
    if ( year < 1980 || year > 2107 || month < 1 || month > 12 || day < 1
         || day > days_in( month, year ) ) {
        return -1;
    }
    *out = (u16)(((year - 1980) << 9) | (month << 5) | day);
    return 0;
}

static void bad_switch( const char *start, u32 len )
{
    char text[40];

    if ( len > sizeof( text ) - 2 ) {
        len = sizeof( text ) - 2;
    }
    text[0] = '/';
    mem_cpy( text + 1, start, len );
    text[len + 1] = 0;
    out_msg( M_BADPARM, text, NULL );
    quit( EXIT_INIT );
}

/* one switch, the text after its '/'.  From COPYCMD only /Y and /-Y
   count, and anything else there is ignored, as COPY ignores it. */
static void one_switch( const char *sw, u32 len, int from_env )
{
    int letter = ch_upper( sw[0] );

    if ( len == 2 && sw[0] == '-' && ch_upper( sw[1] ) == 'Y' ) {
        opt.overwrite = 0;
        return;
    }
    if ( len == 1 && letter == 'Y' ) {
        opt.overwrite = 1;
        return;
    }
    if ( from_env ) {
        return;
    }
    if ( letter == 'D' && len >= 2 && sw[1] == ':' ) {
        if ( parse_date( sw + 2, len - 2, &opt.date ) != 0 ) {
            out_line( M_BADDATE );
            quit( EXIT_INIT );
        }
        opt.date_mode = DATE_SINCE;
        return;
    }
    if ( len != 1 ) {
        bad_switch( sw, len );
    }
    switch ( letter ) {
    case 'A': opt.archive_only = 1;       break;
    case 'M': opt.archive_reset = 1;      break;
    case 'D': opt.date_mode = DATE_NEWER; break;
    case 'P': opt.prompt = 1;             break;
    case 'S': opt.subdirs = 1;            break;
    case 'E': opt.empty = 1;              break;
    case 'V': opt.verify = 1;             break;
    case 'W': opt.wait = 1;               break;
    case 'C': opt.cont = 1;               break;
    case 'I': opt.assume_dir = 1;         break;
    case 'Q': opt.quiet = 1;              break;
    case 'F': opt.full = 1;               break;
    case 'L': opt.list = 1;               break;
    case 'H': opt.hidden = 1;             break;
    case 'R': opt.readonly = 1;           break;
    case 'T': opt.tree = 1;               break;
    case 'U': opt.update = 1;             break;
    case 'K': opt.keep_attr = 1;          break;
    case 'N': opt.short_names = 1;        break;
    default:  bad_switch( sw, len );      break;
    }
}

static int blank( int ch )
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

/* Switches and names.  A switch runs to the next blank or '/', except
   /D:date, whose date may be written with slashes.  A name runs to the
   next blank or '/' outside quotes - no file name holds a '/' - and its
   quotes are dropped, so "C:\My Files"\*.TXT is one name. */
static void parse_line( const char *line, int from_env )
{
    const char *cur = line, *start;
    char *dst;
    u32 used;
    int quoted;

    for ( ;; ) {
        while ( blank( *cur ) ) {
            cur++;
        }
        if ( *cur == 0 ) {
            return;
        }
        if ( *cur == '/' ) {
            start = ++cur;
            if ( ch_upper( cur[0] ) == 'D' && cur[1] == ':' ) {
                while ( *cur && !blank( *cur ) ) {
                    cur++;
                }
            } else {
                if ( *cur == '-' ) {
                    cur++;
                }
                while ( *cur && !blank( *cur ) && *cur != '/' ) {
                    cur++;
                }
            }
            if ( cur == start ) {
                if ( !from_env ) {
                    bad_switch( start, 0 );
                }
                continue;
            }
            one_switch( start, (u32)(cur - start), from_env );
            continue;
        }

        /* a name */
        quoted = 0;
        dst = nparams < 2 ? params[nparams] : NULL;
        used = 0;
        while ( *cur && (quoted || (!blank( *cur ) && *cur != '/')) ) {
            if ( *cur == '"' ) {
                quoted = !quoted;
            } else if ( dst && used < PATH_MAX - 1 ) {
                dst[used++] = *cur;
            }
            cur++;
        }
        if ( from_env ) {
            continue;
        }
        if ( dst == NULL ) {
            out_line( M_NPARMS );
            quit( EXIT_INIT );
        }
        dst[used] = 0;
        nparams++;
    }
}

/* /? anywhere on the line is help, whatever else is there */
static int wants_help( const char *line )
{
    for ( ; *line; line++ ) {
        if ( line[0] == '/' && line[1] == '?' ) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the source and the destination                                      */
/* ------------------------------------------------------------------ */

static void check_drive( const char *path )
{
    if ( path[0] && path[1] == ':' ) {
        int drv = ch_upper( path[0] ) - 'A';
        if ( drv < 0 || drv > 25 || !sys_drive_valid( drv ) ) {
            out_line( M_BADDRIVE );
            quit( EXIT_INIT );
        }
    }
}

static int is_dot( const char *name )
{
    return str_icmp( name, "." ) == 0 || str_icmp( name, ".." ) == 0;
}

/* the directory part of "path" (up to its last element) into "out";
   "." when there is none, "X:." for a bare drive */
static void dir_of( const char *path, const char *last, char *out )
{
    u32 len = (u32)(last - path);

    mem_cpy( out, path, len );
    out[len] = 0;
    if ( len == 0 ) {
        str_cpy( out, "." );
    } else if ( len == 2 && out[1] == ':' ) {
        str_catn( out, ".", PATH_MAX );
    }
}

static void full_or_fail( const char *dir, char *out, const char *typed )
{
    int rc = make_full( dir, out );

    if ( rc == ERR_BADDRIVE ) {
        out_line( M_BADDRIVE );
        quit( EXIT_INIT );
    }
    if ( rc != 0 ) {
        out_msg( M_BADPATH, typed, NULL );
        quit( EXIT_INIT );
    }
}

static void resolve_source( const char *arg )
{
    char dir[PATH_MAX];
    const char *last;
    u16 attr;
    u32 len;
    int isdir = 0;

    check_drive( arg );
    last = name_part( arg );
    if ( *last == 0 || is_dot( last ) ) {
        isdir = 1;
    } else if ( !has_wild( last ) ) {
        if ( sys_is_device( arg ) ) {
            out_line( M_FROMDEV );
            quit( EXIT_INIT );
        }
        if ( sys_get_attr( arg, &attr ) == 0 && (attr & ATTR_DIR) ) {
            isdir = 1;
        }
    }

    if ( isdir ) {
        str_cpy( dir, *arg ? arg : "." );
        str_cpy( job.src_disp, arg );
        len = str_len( job.src_disp );
        if ( len && job.src_disp[len - 1] != '\\' && job.src_disp[len - 1] != ':' ) {
            str_catn( job.src_disp, "\\", PATH_MAX );
        }
        str_cpy( job.pattern, "*.*" );
    } else {
        str_cpy( job.pattern, last );
        len = (u32)(last - arg);
        mem_cpy( job.src_disp, arg, len );
        job.src_disp[len] = 0;
        dir_of( arg, last, dir );
    }
    full_or_fail( dir, job.src_full, arg );
    if ( str_len( job.src_full ) > 3
         && (sys_get_attr( job.src_full, &attr ) != 0 || !(attr & ATTR_DIR)) ) {
        out_msg( M_BADPATH, arg, NULL );
        quit( EXIT_INIT );
    }
    end_slash( job.src_full );
    multi = isdir || has_wild( job.pattern ) || opt.subdirs;
}

static void resolve_dest( const char *arg )
{
    char dir[PATH_MAX];
    const char *last;
    u16 attr;
    int isdir, key;

    job.tmpl[0] = 0;
    if ( arg == NULL || *arg == 0 ) {
        full_or_fail( ".", job.dst_full, "." );
        end_slash( job.dst_full );
        return;
    }
    check_drive( arg );
    last = name_part( arg );
    if ( *last == 0 || is_dot( last ) ) {
        isdir = 1;
    } else if ( sys_get_attr( arg, &attr ) == 0 ) {
        isdir = (attr & ATTR_DIR) != 0;
        if ( !isdir && sys_is_device( arg ) ) {
            out_line( M_TODEV );
            quit( EXIT_INIT );
        }
    } else if ( has_wild( last ) ) {
        isdir = 0;
    } else if ( sys_is_device( arg ) ) {
        out_line( M_TODEV );
        quit( EXIT_INIT );
    } else if ( opt.assume_dir && multi ) {
        isdir = 1;
    } else {
        key = ask( M_FILEDIR, arg, "FD" );
        if ( key < 0 ) {
            quit( EXIT_INIT );
        }
        isdir = key == 'D';
    }

    if ( isdir ) {
        full_or_fail( arg, job.dst_full, arg );
    } else {
        dir_of( arg, last, dir );
        full_or_fail( dir, job.dst_full, arg );
        str_cpy( job.tmpl, last );
    }
    end_slash( job.dst_full );
}

/* ------------------------------------------------------------------ */

void xcopy_main( void )
{
    const char *env;
    int code;

    sys_init();
    sys_break_install();
    if ( wants_help( sys_cmdline() ) ) {
        out_text( M_HELP );
        quit( EXIT_OK );
    }
    env = sys_getenv( "COPYCMD" );
    if ( env ) {
        parse_line( env, 1 );
    }
    parse_line( sys_cmdline(), 0 );
    if ( nparams == 0 ) {
        out_line( M_NPARMS );
        quit( EXIT_INIT );
    }
    if ( opt.empty || opt.tree ) {
        opt.subdirs = 1;
    }

    resolve_source( params[0] );
    resolve_dest( nparams > 1 ? params[1] : NULL );

    if ( opt.subdirs
         && str_nicmp( job.dst_full, job.src_full, str_len( job.src_full ) ) == 0 ) {
        out_line( M_CYCLIC );
        quit( EXIT_INIT );
    }
    job.same_dir = str_icmp( job.dst_full, job.src_full ) == 0;

    if ( opt.wait ) {
        in_prompt = 1;
        out_line( M_PRESSKEY );
        if ( sys_break_hit() ) {
            break_exit();
        }
        if ( sys_read_key() < 0 ) {
            quit( EXIT_INIT );
        }
        in_prompt = 0;
    }
    if ( opt.verify ) {
        old_verify = sys_get_verify();
        sys_set_verify( 1 );
    }
    code = copy_run();
    quit( code );
}
