/*
 * WALK.C - the walk through the directories, and the drawing.
 *
 * ONE SEARCH OPEN AT A TIME.  A directory's subdirectories are listed
 * into memory and that search closed before anything is drawn; its
 * files (/F) are drawn straight from a second search, which ends before
 * the first subdirectory is entered.  A search held open at every level
 * of a deep tree would run out of the kernel's find handles (thirty-
 * two) long before the tree ran out of levels - XCOPY's reason too.
 * Nothing changes directory: every search is by its full path.
 *
 * THE LINES ARE 6.22'S.  Each level of the tree is four columns: under
 * a directory with more after it, a bar and three blanks; under the
 * last one, four blanks.  A subdirectory's own line is a tee - an elbow
 * for the last one - three dashes and the name.  A file's line is the
 * columns so far, a bar if the directory has subdirectories to come and
 * a blank if not, three blanks and the name; the files end with that
 * same line and no name, its blanks kept.  Files and subdirectories
 * come in the order the directory holds them, and hidden and system
 * ones are left out (6.22 searched with attribute 10h, and 00h for
 * files), as are "." and "..".
 *
 * A DIRECTORY THAT CANNOT BE READ ENDS THE WALK with errorlevel 1 and
 * no word of its own - 6.22's way, which left the saying to the
 * critical-error handler.  One too deep to have a path is drawn and
 * not entered, and TREE says so at the end.
 */
#include "tree.h"

#define G_ELBOW     0
#define G_DASH      1
#define G_TEE       2
#define G_BAR       3

/* a level's columns, a graphic, three dashes, a name, CR LF */
#define LINE_MAX    (LEVEL_MAX * 4 + 4 + PATH_MAX + 2)

/* names, each ended by its NUL, back to back */
typedef struct {
    char *buf;
    u32   used, cap, count;
} NLIST;

static const u8 graf_line[4]  = { 0xC0, 0xC4, 0xC3, 0xB3 };
static const u8 graf_ascii[4] = { '\\', '-', '+', '|' };

/* The three buffers come from the heap, so that they are not zeroes in
   the file: TREE.EXE goes on the boot diskette, which is full. */
static const u8 *graf;
static char    *path;               /* PATH_MAX: the directory being read, '\'-ended */
static char    *line;               /* LINE_MAX: the columns so far, then the rest */
static FINDREC *rec;                /* one search at a time, so one record */
static int     drawn;               /* a directory has been drawn */
static int     failed;              /* a search went wrong: stop */
static int     too_deep;            /* a directory was not entered */

/* line[0 .. len) and CR LF, in one write; ^C ends the program here */
static void emit( u32 len )
{
    line[len] = '\r';
    line[len + 1] = '\n';
    out_raw( H_OUT, line, len + 2 );
    if ( sys_break_hit() ) {
        quit( EXIT_BREAK );
    }
}

static int is_dots( const char *name )
{
    return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

static void list_add( NLIST *list, const char *name )
{
    u32 need = str_len( name ) + 1;
    char *grown;

    if ( list->used + need > list->cap ) {
        list->cap = list->cap ? list->cap * 2 : 1024;
        while ( list->used + need > list->cap ) {
            list->cap *= 2;
        }
        grown = (char *)xalloc( list->cap );
        if ( list->used ) {
            mem_cpy( grown, list->buf, list->used );
        }
        xfree( list->buf );
        list->buf = grown;
    }
    mem_cpy( list->buf + list->used, name, need );
    list->used += need;
    list->count++;
}

/* the end of a search: "no more" is the ordinary one */
static int search_end( int rc )
{
    return rc == ERR_NOMORE || rc == ERR_NOFILE ? 0 : rc;
}

/* "path" + "*.*" for a search; the caller made sure it fits */
static void set_pattern( u32 plen )
{
    mem_cpy( path + plen, "*.*", 4 );
}

/* the subdirectories of "path", in the directory's order */
static int list_dirs( u32 plen, NLIST *list )
{
    u32 handle;
    int rc;

    set_pattern( plen );
    rc = sys_find_first( path, ATTR_DIR | (ATTR_DIR << 8), rec, &handle );
    path[plen] = 0;
    if ( rc != 0 ) {
        return search_end( rc );
    }
    do {
        if ( (rec->attr & ATTR_DIR) && !is_dots( rec->name ) ) {
            list_add( list, rec->name );
        }
        rc = sys_find_next( handle, rec );
    } while ( rc == 0 );
    sys_find_close( handle );
    return search_end( rc );
}

/* /F: the files of "path" at column "col", and the line after them */
static int draw_files( u32 plen, u32 col, int subdirs )
{
    u32 handle, nlen;
    int rc, any = 0;

    set_pattern( plen );
    rc = sys_find_first( path, 0, rec, &handle );
    path[plen] = 0;
    if ( rc != 0 ) {
        return search_end( rc );
    }
    line[col] = (char)(subdirs ? graf[G_BAR] : ' ');
    line[col + 1] = ' ';
    line[col + 2] = ' ';
    line[col + 3] = ' ';
    do {
        if ( !(rec->attr & (ATTR_DIR | ATTR_VOLUME)) ) {
            nlen = str_len( rec->name );     /* less than PATH_MAX */
            mem_cpy( line + col + 4, rec->name, nlen );
            emit( col + 4 + nlen );
            any = 1;
        }
        rc = sys_find_next( handle, rec );
    } while ( rc == 0 );
    sys_find_close( handle );
    if ( any ) {
        emit( col + 4 );
    }
    return search_end( rc );
}

/* The directory in "path" (plen characters, '\'-ended), drawn from
   column "col"; "level" directories below the start. */
static void walk_dir( u32 plen, u32 col, u32 level )
{
    NLIST list;
    const char *name;
    u32 pos = 0, nlen, i;
    int last;

    mem_set( &list, 0, sizeof( list ) );
    if ( list_dirs( plen, &list ) != 0 ) {
        failed = 1;
    } else if ( opt.files && draw_files( plen, col, list.count != 0 ) != 0 ) {
        failed = 1;
    }
    for ( i = 0; i < list.count && !failed; i++ ) {
        name = list.buf + pos;
        nlen = str_len( name );
        pos += nlen + 1;
        last = i + 1 == list.count;

        line[col] = (char)graf[last ? G_ELBOW : G_TEE];
        line[col + 1] = (char)graf[G_DASH];
        line[col + 2] = (char)graf[G_DASH];
        line[col + 3] = (char)graf[G_DASH];
        mem_cpy( line + col + 4, name, nlen );
        emit( col + 4 + nlen );
        drawn = 1;

        /* below it: its path, '\' and "*.*" must fit */
        if ( level + 1 >= LEVEL_MAX || plen + nlen + 1 + 4 > PATH_MAX ) {
            too_deep = 1;
            continue;
        }
        mem_cpy( path + plen, name, nlen );
        path[plen + nlen] = '\\';
        path[plen + nlen + 1] = 0;
        line[col] = (char)(last ? ' ' : graf[G_BAR]);
        line[col + 1] = ' ';
        line[col + 2] = ' ';
        line[col + 3] = ' ';
        walk_dir( plen + nlen + 1, col + 4, level + 1 );
        path[plen] = 0;
    }
    xfree( list.buf );
}

int walk_tree( const char *dir )
{
    u32 plen;

    graf = opt.ascii ? graf_ascii : graf_line;
    path = (char *)xalloc( PATH_MAX );
    line = (char *)xalloc( LINE_MAX );
    rec = (FINDREC *)xalloc( sizeof( FINDREC ) );
    if ( !str_cpyn( path, dir, PATH_MAX ) ) {
        too_deep = 1;
    } else {
        plen = str_len( path );
        if ( plen && path[plen - 1] != '\\' ) {
            if ( plen + 1 + 4 > PATH_MAX ) {
                too_deep = 1;
            } else {
                path[plen++] = '\\';
                path[plen] = 0;
            }
        }
        if ( !too_deep ) {
            walk_dir( plen, 0, 0 );
        }
    }
    if ( too_deep ) {
        out_msg( H_ERR, M_TOODEEP, NULL, NULL );
    }
    if ( !drawn ) {
        out_raw( H_OUT, M_NOSUBS, str_len( M_NOSUBS ) );
    }
    return failed || too_deep ? EXIT_ERROR : EXIT_OK;
}
