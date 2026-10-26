/*
 * MAIN.C - TREE: the command line, the heading and where the tree
 * starts.
 *
 *   TREE [drive:][path] [/F] [/A]
 *
 * MS-DOS 6.22'S TREE IS THE MODEL, measured by running it (DOSBox-X's
 * own DOS, VER=6.22) and read in its source:
 *
 *   - the heading is the label line ("... for Volume" and the label in
 *     eleven columns, or just "Directory PATH listing" with no label),
 *     the serial number when the disk has one, and the drive with the
 *     path AS TYPED, in capitals: "C:." when no path is given, "C:A2"
 *     for a relative one.  A backslash on the end is dropped, unless
 *     it is all there is
 *   - a path that is not a directory is "Invalid path - path" (without
 *     its drive) after the heading, then "No sub-directories exist"
 *   - the switches are /F, /A and /?, in either case, each once: a
 *     second /F is "Invalid switch - /F", because 6.22's parser struck
 *     a switch off its list when it was used.  The command line is read
 *     in order and the first thing wrong with it is the one reported;
 *     /? shows the help, reads no further and leaves errorlevel 1
 *   - errors go to standard error; errorlevel 1 for any of them, 3 for
 *     ^C, 0 when the tree was shown
 *
 * WHAT IS PM-DOS'S AND NOT 6.22'S: the names are the long ones when the
 * kernel has them (as XCOPY's are); a path may be in double quotes, so
 * that it can have spaces in it; and a path may be as long as the long-
 * name API allows, not 64 characters.
 */
#include "tree.h"

OPTIONS opt;

static int  drive = -1;                 /* the one typed, or -1 */
static int  have_path;
static char *typed;                     /* PATH_MAX: the path, without its drive */

void quit( int code )
{
    sys_exit( code );
}

void out_of_memory( void )
{
    out_msg( H_ERR, M_NOMEM, NULL, NULL );
    quit( EXIT_ERROR );
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

/* what 6.22's parser took to separate one thing from the next */
static int is_delim( int ch )
{
    return ch == ' ' || ch == '\t' || ch == ',' || ch == ';' || ch == '=';
}

static int at_end( int ch )
{
    return ch == 0 || ch == '\r';
}

/* An error about something on the command line, shown as it was typed -
   except a path, which 6.22 showed from its parser's copy, in capitals. */
static void bad_text( const char *tmpl, const char *start, u32 len )
{
    char text[PATH_MAX];

    if ( len > sizeof( text ) - 1 ) {
        len = sizeof( text ) - 1;
    }
    mem_cpy( text, start, len );
    text[len] = 0;
    if ( tmpl == M_BADPATH ) {
        sys_upper( text, len );
    }
    out_msg( H_ERR, tmpl, text, NULL );
    quit( EXIT_ERROR );
}

static void one_switch( const char *start, u32 len, int *used_f, int *used_a )
{
    int letter = len == 2 ? ch_upper( start[1] ) : 0;

    if ( letter == '?' ) {
        out_text( H_OUT, M_HELP );
        quit( EXIT_ERROR );
    }
    if ( letter == 'F' && !*used_f ) {
        *used_f = 1;
        opt.files = 1;
        return;
    }
    if ( letter == 'A' && !*used_a ) {
        *used_a = 1;
        opt.ascii = 1;
        return;
    }
    bad_text( M_BADSWITCH, start, len );
}

/* The path: its drive into "drive", the rest into "typed".  A path too
   long to be one is refused here, before the heading, as 6.22 refused
   one longer than its 64 characters. */
static void one_path( const char *text, u32 len )
{
    const char *rest = text;
    int letter;

    if ( len >= 2 && text[1] == ':' ) {
        letter = ch_upper( text[0] );
        if ( letter < 'A' || letter > 'Z' ) {
            out_msg( H_ERR, M_BADDRIVE, NULL, NULL );
            quit( EXIT_ERROR );
        }
        drive = letter - 'A';
        rest += 2;
        len -= 2;
    }
    if ( len >= PATH_MAX - 2 ) {            /* "X:" in front of it must fit */
        bad_text( M_BADPATH, rest, len );
    }
    mem_cpy( typed, rest, len );
    typed[len] = 0;
    /* "\A\" is "\A", but "\" stays and so does "\\" */
    if ( len > 1 && typed[len - 1] == '\\' && typed[len - 2] != '\\' ) {
        typed[len - 1] = 0;
    }
}

static void parse( const char *line )
{
    char text[PATH_MAX];
    const char *pos = line, *start;
    int used_f = 0, used_a = 0, quoted;
    u32 len;

    for ( ;; ) {
        while ( is_delim( *pos ) ) {
            pos++;
        }
        if ( at_end( *pos ) ) {
            break;
        }
        start = pos;
        if ( *pos == '/' ) {
            pos++;
            while ( !at_end( *pos ) && !is_delim( *pos ) && *pos != '/' ) {
                pos++;
            }
            one_switch( start, (u32)(pos - start), &used_f, &used_a );
            continue;
        }

        /* A path runs to a separator or a switch; inside double quotes
           a blank is part of it, and the quotes themselves are not. */
        quoted = 0;
        len = 0;
        while ( !at_end( *pos ) && (quoted || (!is_delim( *pos ) && *pos != '/')) ) {
            if ( *pos == '"' ) {
                quoted = !quoted;
            } else {
                if ( len < sizeof( text ) ) {
                    text[len] = *pos;
                }
                len++;
            }
            pos++;
        }
        if ( have_path ) {
            bad_text( M_TOOMANY, start, (u32)(pos - start) );
        }
        have_path = 1;
        if ( len >= sizeof( text ) ) {         /* shown without its drive */
            if ( pos - start >= 2 && start[1] == ':' ) {
                start += 2;
            }
            bad_text( M_BADPATH, start, (u32)(pos - start) );
        }
        one_path( text, len );
    }
}

/* ------------------------------------------------------------------ */
/* where the tree starts                                               */
/* ------------------------------------------------------------------ */

/* "typed" made absolute on drive "drv", with "." and ".." taken out,
   into "out" (PATH_MAX bytes): "X:\" or "X:\DIR\SUB".  0, or an error
   when it is not a directory - or would climb above the root, which
   6.22's CHDIR refused too. */
static int start_dir( int drv, char *out )
{
    char abs[PATH_MAX * 2];
    char *comp[LEVEL_MAX];
    char *cur, *name;
    u32 count = 0, i;
    u16 attr;
    int rc;

    abs[0] = (char)('A' + drv);
    abs[1] = ':';
    abs[2] = '\\';
    abs[3] = 0;
    if ( typed[0] != '\\' ) {
        rc = sys_get_cwd( drv, abs + 3 );   /* PATH_MAX bytes, and there are more */
        if ( rc != 0 ) {
            return rc;
        }
        if ( abs[3] && !str_catn( abs, "\\", sizeof( abs ) ) ) {
            return ERR_NOPATH;
        }
    }
    if ( !str_catn( abs, typed, sizeof( abs ) ) ) {
        return ERR_NOPATH;
    }

    cur = abs + 3;
    while ( *cur ) {
        while ( *cur == '\\' ) {
            *cur++ = 0;
        }
        if ( *cur == 0 ) {
            break;
        }
        name = cur;
        while ( *cur && *cur != '\\' ) {
            cur++;
        }
        if ( *cur ) {
            *cur++ = 0;
        }
        if ( str_icmp( name, "." ) == 0 ) {
            continue;
        }
        if ( str_icmp( name, ".." ) == 0 ) {
            if ( count == 0 ) {
                return ERR_NOPATH;
            }
            count--;
            continue;
        }
        if ( count == LEVEL_MAX ) {
            return ERR_NOPATH;
        }
        comp[count++] = name;
    }

    out[0] = abs[0];
    out[1] = ':';
    out[2] = '\\';
    out[3] = 0;
    for ( i = 0; i < count; i++ ) {
        if ( (i > 0 && !str_catn( out, "\\", PATH_MAX )) || !str_catn( out, comp[i], PATH_MAX ) ) {
            return ERR_NOPATH;
        }
    }
    if ( count == 0 ) {
        return 0;                           /* the root is always a directory */
    }
    rc = sys_get_attr( out, &attr );        /* a wildcard is refused here */
    if ( rc != 0 ) {
        return rc;
    }
    return (attr & ATTR_DIR) ? 0 : ERR_NOPATH;
}

static void hex4( u32 val, char *buf )
{
    static const char digits[] = "0123456789ABCDEF";
    int pos;

    for ( pos = 3; pos >= 0; pos-- ) {
        buf[pos] = digits[val & 15];
        val >>= 4;
    }
    buf[4] = 0;
}

/* ------------------------------------------------------------------ */
/* the program                                                         */
/* ------------------------------------------------------------------ */

void tree_main( void )
{
    char full[PATH_MAX];
    char label[LABEL_LEN + 1];
    char high[5], low[5];
    char head[2];
    u32 serial;
    int drv;

    sys_init();
    sys_break_install();
    typed = (char *)xalloc( PATH_MAX );     /* out of the file, as WALK.C's */
    typed[0] = 0;
    parse( sys_cmdline() );

    drv = drive >= 0 ? drive : sys_get_drive();
    if ( drive >= 0 && !sys_drive_valid( drv ) ) {
        out_msg( H_ERR, M_BADDRIVE, NULL, NULL );
        quit( EXIT_ERROR );
    }
    if ( typed[0] == 0 ) {
        typed[0] = '.';                     /* the drive's current directory */
        typed[1] = 0;
    }
    sys_upper( typed, str_len( typed ) );

    if ( sys_volume_label( drv, label ) == 0 ) {
        out_msg( H_OUT, M_LABEL, label, NULL );
    } else {
        out_msg( H_OUT, M_NOLABEL, NULL, NULL );
    }
    if ( sys_volume_serial( drv, &serial ) == 0 ) {
        hex4( serial >> 16, high );
        hex4( serial & 0xFFFFu, low );
        out_msg( H_OUT, M_SERIAL, high, low );
    }
    head[0] = (char)('A' + drv);
    head[1] = ':';
    out_raw( H_OUT, head, 2 );
    out_msg( H_OUT, "%1", typed, NULL );

    if ( start_dir( drv, full ) != 0 ) {
        out_msg( H_ERR, M_BADPATH, typed, NULL );
        out_raw( H_OUT, M_NOSUBS, str_len( M_NOSUBS ) );
        quit( EXIT_ERROR );
    }
    quit( walk_tree( full ) );
}
