/*
 * MAIN.C - DELTREE.EXE, the PM-DOS native DELTREE: deletes directories
 * with everything in them, and files.
 *
 *      DELTREE [/Y] [drive:]path [[drive:]path ...]
 *
 * WHAT IT DOES IS MS-DOS 6'S DELTREE, measured from the real one (6.22
 * under DOSBox-X's DOS, and Windows 98's for long names):
 *
 *   - each path is a PATTERN, matched against files and directories,
 *     hidden and system ones included.  "X" deletes X itself; "X\*.*"
 *     and "X\" delete what is in X and leave X; "." and ".." mean
 *     everything in that directory.  Nothing matching is not an error
 *     and says nothing.
 *   - without /Y each match is asked about: 'Delete directory "X" and
 *     all its subdirectories? [yn] ' or 'Delete file "X"? [yn] '.  The
 *     answer is Y or N and then Enter; the letter is echoed, the last
 *     one typed counts, Enter before either does nothing, and Ctrl-Z is
 *     a no.  Ctrl-C ends the program.
 *   - "Deleting X..." goes out before each match is deleted, whether it
 *     was asked about or not.  Nothing is said about what is inside.
 *   - the name shown is the directory part as typed and the name as the
 *     file system has it: Windows 98's form, long names and case kept.
 *     MS-DOS 6.22 showed found names in small letters; that was its
 *     library, not a decision, and it would hide long names' case.
 *   - "Required parameter missing" and "Invalid switch - X" with
 *     errorlevel 1, before anything is deleted.
 *
 * WHERE IT DIFFERS, on purpose:
 *   - switches are taken anywhere on the line.  MS-DOS 6's looked for
 *     them only in front of the first path.
 *   - the echoed letter has the cursor after it, not on it (confirm()).
 *   - what cannot be deleted is named (MS-DOS 6's said nothing) and the
 *     errorlevel is then 1 (it was always 0).  A directory with such a
 *     thing in it is left, with everything else in it gone.
 *   - a read-only file that will not go has its read-only bit back.
 *
 * MATCHES ARE LISTED BEFORE ANY IS DELETED.  With UNDELETE=ON the first
 * delete in a root can make \RECYCLED, and a search still going through
 * that root would then find it and delete the files just kept in it.
 *
 * INSIDE A DIRECTORY ONLY ONE SEARCH IS EVER OPEN.  The files go in one
 * pass, the search carried on past each delete as DEL's is.  Then each
 * subdirectory is found by a search started afresh and closed before
 * the walk goes down into it - so the depth of a tree costs no find
 * handles, and a subdirectory that would not go is counted and passed
 * over the next time.
 *
 * THE PATH WORKED ON IS BUILT FROM 8.3 NAMES, as DEL builds it: an alias
 * is unambiguous where a long name may not be, and short enough that a
 * tree made through the long-name calls can always be walked.  Messages
 * show the long form, from 7160h.
 */
#include "rt.h"
#include "sys.h"

/* ------------------------------------------------------------------ */
/* what it says                                                        */
/* ------------------------------------------------------------------ */

static const char M_HELP[] =
    "Deletes a directory with all of its files and subdirectories.\r\n"
    "\r\n"
    "DELTREE [/Y] [drive:]path [[drive:]path[...]]\r\n"
    "\r\n"
    "  [drive:]path  The directory or file to delete.  Wildcards allowed.\r\n"
    "  /Y            Deletes without asking first.\r\n"
    "\r\n"
    "Hidden, system and read-only files are deleted as well.\r\n";

static const char M_NOPARM[]   = "Required parameter missing\r\n";
static const char M_BADSW[]    = "Invalid switch - %1\r\n";
static const char M_ASKDIR[]   = "Delete directory \"%1\" and all its subdirectories? [yn] ";
static const char M_ASKFILE[]  = "Delete file \"%1\"? [yn] ";
static const char M_DELETING[] = "Deleting %1...\r\n";
static const char M_NOMEM[]    = "Insufficient memory\r\n";
static const char M_DENIED[]   = "Access denied - %1\r\n";
static const char M_CURDIR[]   = "Attempt to remove current directory - %1\r\n";
static const char M_TOOLONG[]  = "Path too long - %1\r\n";
static const char M_TOOLONG2[] = "Path too long - %1\\%2\r\n";
static const char M_ERROR[]    = "Error %2 deleting %1\r\n";

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

#define FIND_ANY    (ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE)
#define FIND_DIRS   ((ATTR_DIR << 8) | FIND_ANY | ATTR_DIR)

typedef struct HIT {            /* one match of a path typed */
    struct HIT *next;
    u8          attr;
    char       *alias;          /* points past name's NUL */
    char        name[1];
} HIT;

/* The buffers come from the heap, not the BSS: the BSS is written into
   the file as zeroes (DOSINT.ASM, D32END), and the boot floppy is short
   of room. */
#define ARGBUF_LEN  600

static char    *work;           /* PATH_MAX: the path worked on, 8.3 names */
static char    *shown;          /* PATH_MAX * 2: a match as the user sees it */
static FINDREC *rec;            /* one search at a time: one record */
static char    *argbuf;         /* ARGBUF_LEN: the command line, a NUL after
                                   each word */
static int     ask = 1;
static int     failed;          /* something could not be deleted */
static int     stopped;         /* "Fail" at a disk error: nothing more */

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

static void out( const char *str )
{
    sys_write( H_OUT, str, str_len( str ) );
}

/* a message with %1 and %2 in it */
static void say( const char *msg, const char *one, const char *two )
{
    const char *run = msg;

    while ( *msg ) {
        if ( msg[0] == '%' && (msg[1] == '1' || msg[1] == '2') ) {
            sys_write( H_OUT, run, (u32)(msg - run) );
            out( msg[1] == '1' ? one : two );
            msg += 2;
            run = msg;
        } else {
            msg++;
        }
    }
    sys_write( H_OUT, run, (u32)(msg - run) );
}

/* Why work[] could not be deleted, by the long form of its name.  A
   "Fail" to a disk error has had its say on the screen already, and
   stops the run: every file after it would only ask again. */
static void say_failure( int err )
{
    char full[PATH_MAX];
    char num[12];
    const char *name = work;

    failed = 1;
    if ( err == ERR_FAIL ) {
        stopped = 1;
        return;
    }
    if ( sys_truename( work, full, 2 ) == 0 && full[0] != 0 ) {
        name = full;
    }
    switch ( err ) {
    case ERR_ACCESS:
    case ERR_SHARE:
    case ERR_LOCK:
        say( M_DENIED, name, NULL );
        break;
    case ERR_CURDIR:
        say( M_CURDIR, name, NULL );
        break;
    default:
        say( M_ERROR, name, fmt_dec( (u32)err, 0, num ) );
        break;
    }
}

/* ------------------------------------------------------------------ */
/* paths                                                               */
/* ------------------------------------------------------------------ */

static int is_sep( char ch )
{
    return ch == '\\' || ch == '/' || ch == ':';
}

static int is_dots( const char *name )
{
    return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

/* work[] = work[0..len) + a backslash + name.  The new length, or 0
   when it would not fit; work[] is unchanged then. */
static u32 path_add( u32 len, const char *name )
{
    u32 nlen = str_len( name );
    u32 sep = (len > 0 && !is_sep( work[len - 1] )) ? 1 : 0;

    if ( len + sep + nlen + 1 > PATH_MAX ) {
        return 0;
    }
    if ( sep ) {
        work[len++] = '\\';
    }
    mem_cpy( work + len, name, nlen + 1 );
    return len + nlen;
}

/* ------------------------------------------------------------------ */
/* deleting                                                            */
/* ------------------------------------------------------------------ */

/* The file whose path is in work[], with attributes attr.  41h will
   not delete a read-only file, so the bit is taken off first - and put
   back if the file stays. */
static int del_file( u8 attr )
{
    int err;

    attr &= FIND_ANY;
    if ( attr & ATTR_READONLY ) {
        err = sys_set_attr( work, (u16)(attr & ~ATTR_READONLY) );
        if ( err ) {
            say_failure( err );
            return err;
        }
    }
    err = sys_delete( work );
    if ( err ) {
        if ( attr & ATTR_READONLY ) {
            sys_set_attr( work, attr );
        }
        say_failure( err );
    }
    return err;
}

/* A name that will not fit after the directory work[] holds: said with
   the directory's long form, and counted. */
static void say_toolong( const char *name )
{
    char full[PATH_MAX];
    const char *dir = work;

    failed = 1;
    if ( sys_truename( work, full, 2 ) == 0 && full[0] != 0 ) {
        dir = full;
    }
    say( M_TOOLONG2, dir, name );
}

/* Empties the directory whose path is work[0..len) and removes it.
   0 when it has gone; otherwise what stayed has been named. */
static int del_dir( u32 len )
{
    u32 handle, sub;
    int err, kept = 0, skip = 0, seen;

    /* the files, in one pass */
    if ( path_add( len, "*.*" ) == 0 ) {
        say_toolong( "*.*" );
        return 1;
    }
    err = sys_find_first( work, FIND_ANY, rec, &handle );
    work[len] = 0;
    if ( err == 0 ) {
        do {
            sub = path_add( len, rec->alias );
            if ( sub == 0 ) {
                say_toolong( rec->name );
                kept++;
            } else {
                if ( del_file( rec->attr ) ) {
                    kept++;
                }
                work[len] = 0;
            }
        } while ( !stopped && (err = sys_find_next( handle, rec )) == 0 );
        sys_find_close( handle );
    }
    /* "no more" ends a search; anything else - a Fail at a disk error
       among them - is said, and the directory is left */
    if ( err != 0 && err != ERR_NOFILE && err != ERR_NOMORE ) {
        say_failure( err );
        return 1;
    }

    /* the subdirectories: a fresh search for each, closed before the
       walk goes down, past the ones that have already refused */
    while ( !stopped ) {
        path_add( len, "*.*" );         /* fitted above */
        err = sys_find_first( work, FIND_DIRS, rec, &handle );
        work[len] = 0;
        if ( err == 0 ) {
            seen = 0;
            while ( err == 0 && (is_dots( rec->name ) || seen++ < skip) ) {
                err = sys_find_next( handle, rec );
            }
            sys_find_close( handle );
        }
        if ( err == ERR_NOFILE || err == ERR_NOMORE ) {
            break;
        }
        if ( err ) {
            say_failure( err );
            return 1;
        }
        sub = path_add( len, rec->alias );
        if ( sub == 0 ) {
            say_toolong( rec->name );
            skip++;
            continue;
        }
        if ( del_dir( sub ) ) {
            skip++;
        }
        work[len] = 0;
    }
    if ( stopped || kept || skip ) {
        return 1;
    }

    err = sys_rmdir( work );
    if ( err ) {
        say_failure( err );
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* asking                                                              */
/* ------------------------------------------------------------------ */

/* Y or N, then Enter.  The letter is echoed and the last one typed
   counts; nothing else does, except Ctrl-Z and the end of a file of
   answers, which are a no.
   MS-DOS's DELTREE wrote the letter and a backspace, to leave the
   cursor on it.  PM-DOS's console backspace ERASES (IO\CONSOLE.INC,
   co_bs), so here a backspace goes BEFORE a letter that replaces one:
   the same letter shows on either kind of console, with the cursor
   after it. */
static int confirm( const char *msg )
{
    char echo[2];
    int key, last = 0;

    sys_key_flush();
    say( msg, shown, NULL );
    for ( ;; ) {
        key = sys_read_key();
        if ( key < 0 || key == 0x1A || key == 0x03 ) {
            out( "\r\n" );
            return 0;
        }
        if ( key == '\r' ) {
            if ( last ) {
                out( "\r\n" );
                return last == 'y';
            }
            continue;
        }
        if ( key == 'Y' || key == 'N' ) {
            key += 'a' - 'A';
        }
        if ( key == 'y' || key == 'n' ) {
            echo[0] = '\b';
            echo[1] = (char)key;
            if ( last ) {
                sys_write( H_OUT, echo, 2 );
            } else {
                sys_write( H_OUT, echo + 1, 1 );
            }
            last = key;
        }
    }
}

/* ------------------------------------------------------------------ */
/* one path from the command line                                      */
/* ------------------------------------------------------------------ */

static void free_hits( HIT *list )
{
    HIT *next;

    for ( ; list != NULL; list = next ) {
        next = list->next;
        xfree( list );
    }
}

static void do_path( const char *arg )
{
    char pattern[PATH_MAX], prefix[PATH_MAX];
    HIT *list = NULL, **tail = &list, *hit;
    u32 plen = 0, nlen, alen, len, handle, i;
    const char *name;
    int err;

    for ( i = 0; arg[i]; i++ ) {
        if ( is_sep( arg[i] ) ) {
            plen = i + 1;
        }
    }
    name = arg + plen;

    /* "X\" and "X:" mean what is in X; so do "." and "..", as names */
    if ( name[0] == 0 ) {
        if ( !str_cpyn( prefix, arg, sizeof( prefix ) ) ) {
            goto toolong;
        }
    } else if ( is_dots( name ) ) {
        if ( !str_cpyn( prefix, arg, sizeof( prefix ) ) ||
             !str_catn( prefix, "\\", sizeof( prefix ) ) ) {
            goto toolong;
        }
    } else {
        if ( plen >= sizeof( prefix ) ) {
            goto toolong;
        }
        mem_cpy( prefix, arg, plen );
        prefix[plen] = 0;
    }
    if ( name[0] == 0 || is_dots( name ) ) {
        if ( !str_cpyn( pattern, prefix, sizeof( pattern ) ) ||
             !str_catn( pattern, "*.*", sizeof( pattern ) ) ) {
            goto toolong;
        }
    } else if ( !str_cpyn( pattern, arg, sizeof( pattern ) ) ) {
        goto toolong;
    }

    /* every match first, then the deleting */
    err = sys_find_first( pattern, FIND_ANY | ATTR_DIR, rec, &handle );
    if ( err ) {
        if ( err == ERR_FAIL ) {
            failed = 1;
            stopped = 1;
        }
        return;                         /* nothing there: nothing said */
    }
    do {
        if ( is_dots( rec->name ) ) {
            continue;
        }
        nlen = str_len( rec->name );
        alen = str_len( rec->alias );
        hit = (HIT *)try_alloc( sizeof( HIT ) + nlen + alen + 1 );
        if ( hit == NULL ) {
            sys_find_close( handle );
            free_hits( list );
            out( M_NOMEM );
            failed = 1;
            stopped = 1;
            return;
        }
        hit->next = NULL;
        hit->attr = rec->attr;
        mem_cpy( hit->name, rec->name, nlen + 1 );
        hit->alias = hit->name + nlen + 1;
        mem_cpy( hit->alias, rec->alias, alen + 1 );
        *tail = hit;
        tail = &hit->next;
    } while ( sys_find_next( handle, rec ) == 0 );
    sys_find_close( handle );

    for ( hit = list; hit != NULL && !stopped; hit = hit->next ) {
        str_cpyn( shown, prefix, PATH_MAX * 2 );     /* both fit: */
        str_catn( shown, hit->name, PATH_MAX * 2 );  /* 259 + 259 */
        if ( ask && !confirm( (hit->attr & ATTR_DIR) ? M_ASKDIR : M_ASKFILE ) ) {
            continue;
        }
        say( M_DELETING, shown, NULL );

        str_cpyn( work, prefix, PATH_MAX );
        len = path_add( str_len( work ), hit->alias );
        if ( len == 0 ) {
            failed = 1;
            say( M_TOOLONG, shown, NULL );
            continue;
        }
        if ( hit->attr & ATTR_DIR ) {
            del_dir( len );
        } else {
            del_file( hit->attr );
        }
    }
    free_hits( list );
    return;

toolong:
    failed = 1;
    say( M_TOOLONG, arg, NULL );
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

/* argbuf = the words of the line, a NUL after each and one more at the
   end.  A word is a run without blanks, or anything in double quotes,
   which go; a switch is a / and what follows up to a blank or the next
   /.  The buffer holds twice the line, more than the NULs can add, and
   "end" leaves room for a word's NUL and the last one. */
static void split_args( const char *line )
{
    char *put = argbuf;
    char *end = argbuf + ARGBUF_LEN - 2;
    char *word;
    int quoted;

    while ( *line && put < end ) {
        if ( *line == ' ' || *line == '\t' ) {
            line++;
            continue;
        }
        word = put;
        if ( *line == '/' ) {
            *put++ = *line++;
            while ( *line && *line != ' ' && *line != '\t' && *line != '/' && put < end ) {
                *put++ = *line++;
            }
        } else {
            quoted = 0;
            while ( *line && (quoted || (*line != ' ' && *line != '\t' && *line != '/')) && put < end ) {
                if ( *line == '"' ) {
                    quoted = !quoted;
                    line++;
                    continue;
                }
                *put++ = *line++;
            }
        }
        if ( put > word ) {             /* "" is no word at all: an empty */
            *put++ = 0;                 /* one would read as the end      */
        }
    }
    *put = 0;
}

/* the next word after "word", or NULL after the last */
static const char *next_arg( const char *word )
{
    word += str_len( word ) + 1;
    return *word ? word : NULL;
}

void deltree_main( void )
{
    const char *arg;
    int paths = 0;

    work = (char *)try_alloc( PATH_MAX * 3 + ARGBUF_LEN + sizeof( FINDREC ) );
    if ( sys_init() != 0 || work == NULL ) {
        out( M_NOMEM );
        sys_exit( 1 );
    }
    shown = work + PATH_MAX;
    argbuf = shown + PATH_MAX * 2;
    rec = (FINDREC *)(argbuf + ARGBUF_LEN);
    split_args( sys_cmdline() );

    for ( arg = argbuf[0] ? argbuf : NULL; arg != NULL; arg = next_arg( arg ) ) {
        if ( arg[0] != '/' ) {
            paths++;
        } else if ( arg[1] == '?' && arg[2] == 0 ) {
            out( M_HELP );
            sys_exit( 0 );
        } else if ( (arg[1] == 'Y' || arg[1] == 'y') && arg[2] == 0 ) {
            ask = 0;
        } else {
            say( M_BADSW, arg + 1, NULL );
            sys_exit( 1 );
        }
    }
    if ( paths == 0 ) {
        out( M_NOPARM );
        sys_exit( 1 );
    }

    for ( arg = argbuf; arg != NULL && !stopped; arg = next_arg( arg ) ) {
        if ( arg[0] != '/' ) {
            do_path( arg );
        }
    }
    sys_exit( failed ? 1 : 0 );
}
