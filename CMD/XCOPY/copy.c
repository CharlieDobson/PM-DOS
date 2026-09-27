/*
 * COPY.C - the walk through the source tree, and the copying.
 *
 * ONE DIRECTORY AT A TIME, READ WHOLE BEFORE ANYTHING IS DONE WITH IT.
 * Each directory's matching files are listed into memory and the search
 * closed before the first one is copied, and its subdirectories likewise
 * before the first one is entered.  Two reasons.  A search left open
 * while files are created can meet its own copies - "XCOPY * *.BAK" in
 * one directory would copy the copies - and a search held open at every
 * level of a deep tree would run out of the kernel's find handles
 * (thirty-two) long before the tree ran out of levels.
 *
 * Files are listed before subdirectories and a directory's files are
 * copied before its subdirectories are entered, in directory order,
 * which is the order DOS's XCOPY shows them in.
 *
 * A DESTINATION DIRECTORY IS MADE WHEN SOMETHING NEEDS IT: when the
 * first file is copied into it, or - with /E - when the walk reaches
 * it, empty or not.  So /S without /E leaves no empty directories
 * behind, including the top one when nothing at all matched.
 */
#include "xcopy.h"

/* a listed directory entry: this header, then the name and the 8.3 name */
typedef struct {
    u32 size;
    u16 time, date;
    u8  attr;
    u8  namelen;                /* strlen( name ), at most 255 */
} ENTRY;

typedef struct {
    u8  *buf;
    u32  used, cap;
} ELIST;

#define COPYBUF_MAX (64u * 1024u)

static char  src_rel[PATH_MAX];     /* below the source dir, '\'-ended */
static char  dst_rel[PATH_MAX];     /* the same below the destination */
static char  made[PATH_MAX];        /* the last directory known to exist */
static u8   *copybuf;
static u32   copylen;
static u32   copied;                /* files copied, or listed with /L */
static u32   matched;               /* files the pattern found at all */
static int   too_long;              /* a path would not fit */
static int   errors;
static int   error_exit;
static int   overwrite_all;
static int   broken;                /* ^C */

/* ------------------------------------------------------------------ */
/* the lists                                                           */
/* ------------------------------------------------------------------ */

static void list_add( ELIST *list, const FINDREC *rec )
{
    u32 nlen = str_len( rec->name ), alen = str_len( rec->alias );
    u32 need = sizeof( ENTRY ) + nlen + 1 + alen + 1;
    ENTRY *ent;
    u8 *grown;

    if ( nlen > 255 ) {
        too_long = 1;
        return;
    }
    need = (need + 3) & ~3u;
    if ( list->used + need > list->cap ) {
        list->cap = list->cap ? list->cap * 2 : 2048;
        while ( list->used + need > list->cap ) {
            list->cap *= 2;
        }
        grown = (u8 *)xalloc( list->cap );
        if ( list->used ) {
            mem_cpy( grown, list->buf, list->used );
        }
        xfree( list->buf );
        list->buf = grown;
    }
    ent = (ENTRY *)(list->buf + list->used);
    ent->size = rec->size;
    ent->time = rec->time;
    ent->date = rec->date;
    ent->attr = rec->attr;
    ent->namelen = (u8)nlen;
    mem_cpy( ent + 1, rec->name, nlen + 1 );
    mem_cpy( (u8 *)(ent + 1) + nlen + 1, rec->alias, alen + 1 );
    list->used += need;
}

static ENTRY *list_next( ELIST *list, u32 *pos )
{
    ENTRY *ent;
    u32 nlen, alen;

    if ( *pos >= list->used ) {
        return NULL;
    }
    ent = (ENTRY *)(list->buf + *pos);
    nlen = ent->namelen;
    alen = str_len( (char *)(ent + 1) + nlen + 1 );
    *pos += (sizeof( ENTRY ) + nlen + 1 + alen + 1 + 3) & ~3u;
    return ent;
}

static const char *ent_name( const ENTRY *ent )
{
    return (const char *)(ent + 1);
}

static const char *ent_alias( const ENTRY *ent )
{
    return (const char *)(ent + 1) + ent->namelen + 1;
}

/* every entry of "dir" matching "pattern": files, or directories */
static int list_dir( const char *dir, const char *pattern, int dirs, ELIST *list )
{
    char spec[PATH_MAX];
    FINDREC rec;
    u32 handle;
    u16 attrs;
    int rc;

    if ( !str_cpyn( spec, dir, sizeof( spec ) ) || !str_catn( spec, pattern, sizeof( spec ) ) ) {
        too_long = 1;
        return 0;
    }
    attrs = ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE;
    if ( dirs ) {
        attrs |= ATTR_DIR | (ATTR_DIR << 8);    /* allowed, and required */
    }
    rc = sys_find_first( spec, attrs, &rec, &handle );
    if ( rc != 0 ) {
        return rc == ERR_NOMORE ? ERR_NOFILE : rc;
    }
    do {
        if ( dirs ) {
            if ( !(rec.attr & ATTR_DIR) || str_icmp( rec.name, "." ) == 0
                 || str_icmp( rec.name, ".." ) == 0 ) {
                continue;
            }
        } else if ( rec.attr & (ATTR_DIR | ATTR_VOLUME) ) {
            continue;
        }
        list_add( list, &rec );
    } while ( sys_find_next( handle, &rec ) == 0 );
    sys_find_close( handle );
    return 0;
}

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

/* one file went wrong: say why, and stop unless /C */
static int file_error( int code, const char *src, const char *dst )
{
    if ( opt.quiet ) {
        out_msg( M_COPYERR, src, dst );
    }
    out_line( msg_error( code ) );
    errors++;
    if ( code == ERR_DISKFULL ) {
        error_exit = EXIT_INIT;
    } else if ( error_exit == 0 ) {
        error_exit = EXIT_WRITE;
    }
    return opt.cont ? 0 : -1;
}

static int check_break( void )
{
    if ( sys_break_hit() ) {
        broken = 1;
    }
    return broken;
}

/* ------------------------------------------------------------------ */
/* the destination tree                                                */
/* ------------------------------------------------------------------ */

/* job.dst_full + dst_rel exists, making whatever of it does not */
static int ensure_dir( void )
{
    char path[PATH_MAX];
    u16 attr;
    u32 len, pos;
    int rc;

    if ( !str_cpyn( path, job.dst_full, sizeof( path ) )
         || !str_catn( path, dst_rel, sizeof( path ) ) ) {
        too_long = 1;
        return -1;
    }
    if ( str_icmp( path, made ) == 0 || opt.list ) {
        return 0;
    }
    len = str_len( path );
    for ( pos = 3; pos < len; pos++ ) {         /* past "X:\" */
        if ( path[pos] != '\\' ) {
            continue;
        }
        path[pos] = 0;
        rc = sys_get_attr( path, &attr );
        if ( rc == 0 && !(attr & ATTR_DIR) ) {
            rc = ERR_ACCESS;                    /* a file of that name */
        } else if ( rc != 0 ) {
            rc = sys_mkdir( path );
        }
        if ( rc != 0 ) {
            out_msg( M_MKDIR, path, NULL );
            errors++;
            error_exit = error_exit ? error_exit : EXIT_WRITE;
            path[pos] = '\\';
            return opt.cont ? 1 : -1;
        }
        path[pos] = '\\';
    }
    str_cpy( made, path );
    return 0;
}

/* ------------------------------------------------------------------ */
/* one file                                                            */
/* ------------------------------------------------------------------ */

/* the bytes, the time and the attributes.  0, or a DOS error code */
static int copy_data( const char *src, const char *dst, const ENTRY *ent )
{
    u32 in, out, got, put;
    u16 attr;
    int rc;

    rc = sys_open_read( src, &in );
    if ( rc != 0 ) {
        return rc;
    }
    rc = sys_create( dst, &out );
    if ( rc != 0 ) {
        sys_close( in );
        return rc;
    }
    for ( ;; ) {
        if ( check_break() ) {
            rc = -1;
            break;
        }
        rc = sys_read( in, copybuf, copylen, &got );
        if ( rc != 0 || got == 0 ) {
            break;
        }
        rc = sys_write_file( out, copybuf, got, &put );
        if ( rc != 0 ) {
            break;
        }
    }
    if ( rc == 0 ) {
        sys_set_ftime( out, ent->date, ent->time );
    }
    sys_close( out );
    sys_close( in );
    if ( rc != 0 ) {
        sys_delete( dst );                      /* never leave half a file */
        return rc;
    }
    if ( opt.keep_attr ) {
        attr = ent->attr & (ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE);
    } else {
        attr = (ent->attr & (ATTR_HIDDEN | ATTR_SYSTEM)) | ATTR_ARCHIVE;
    }
    sys_set_attr( dst, attr );
    if ( opt.archive_reset ) {
        sys_set_attr( src, ent->attr & (ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM) );
    }
    return 0;
}

/* what is at "path" already: 0 and its entry, or not there */
static int probe( const char *path, FINDREC *rec )
{
    u32 handle;

    if ( has_wild( path ) ) {
        return -1;
    }
    if ( sys_find_first( path, ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE
                         | ATTR_DIR, rec, &handle ) != 0 ) {
        return -1;
    }
    sys_find_close( handle );
    return 0;
}

static int newer( const ENTRY *ent, const FINDREC *there )
{
    if ( ent->date != there->date ) {
        return ent->date > there->date;
    }
    return ent->time > there->time;
}

/* -1 stops the whole copy, 0 goes on */
static int do_file( const ENTRY *ent )
{
    char src[PATH_MAX], disp[PATH_MAX], dst[PATH_MAX], name[PATH_MAX];
    FINDREC there;
    int exists, rc, key;

    if ( (ent->attr & (ATTR_HIDDEN | ATTR_SYSTEM)) && !opt.hidden ) {
        return 0;
    }
    matched++;
    if ( (opt.archive_only || opt.archive_reset) && !(ent->attr & ATTR_ARCHIVE) ) {
        return 0;
    }
    if ( opt.date_mode == DATE_SINCE && ent->date < opt.date ) {
        return 0;
    }

    /* the names: where it is, how it is shown, where it goes */
    if ( job.tmpl[0] ) {
        map_name( opt.short_names ? ent_alias( ent ) : ent_name( ent ), job.tmpl, name );
    } else {
        str_cpy( name, opt.short_names ? ent_alias( ent ) : ent_name( ent ) );
    }
    if ( !str_cpyn( src, job.src_full, PATH_MAX ) || !str_catn( src, src_rel, PATH_MAX )
         || !str_catn( src, ent_name( ent ), PATH_MAX )
         || !str_cpyn( disp, job.src_disp, PATH_MAX ) || !str_catn( disp, src_rel, PATH_MAX )
         || !str_catn( disp, ent_name( ent ), PATH_MAX )
         || !str_cpyn( dst, job.dst_full, PATH_MAX ) || !str_catn( dst, dst_rel, PATH_MAX )
         || !str_catn( dst, name, PATH_MAX ) ) {
        too_long = 1;
        return 0;
    }

    exists = probe( dst, &there ) == 0;
    if ( opt.update && !exists ) {
        return 0;
    }
    if ( opt.date_mode == DATE_NEWER && exists && !newer( ent, &there ) ) {
        return 0;
    }
    if ( opt.tree ) {
        return ensure_dir() < 0 ? -1 : 0;       /* the directory, not the file */
    }

    if ( job.same_dir && src_rel[0] == 0 && (str_icmp( name, ent_name( ent ) ) == 0
                                             || str_icmp( name, ent_alias( ent ) ) == 0) ) {
        out_line( M_SELF );
        errors++;
        error_exit = EXIT_INIT;
        return opt.cont ? 0 : -1;
    }

    if ( opt.prompt ) {
        key = ask( M_YESNO, disp, "YN" );
        if ( key < 0 || broken ) {
            return -1;
        }
        if ( key == 'N' ) {
            return 0;
        }
    }
    if ( exists && !opt.list ) {
        if ( there.attr & ATTR_DIR ) {
            if ( !opt.quiet ) {
                out_line( disp );
            }
            return file_error( ERR_ACCESS, disp, dst );
        }
        if ( !opt.overwrite && !overwrite_all ) {
            key = ask( M_OVERWRITE, dst, "YNA" );
            if ( key < 0 || broken ) {
                return -1;
            }
            if ( key == 'N' ) {
                return 0;
            }
            if ( key == 'A' ) {
                overwrite_all = 1;
            }
        }
    }

    if ( opt.full ) {
        out_msg( M_ARROW, src, dst );
    } else if ( !opt.quiet ) {
        out_line( disp );
    }
    if ( opt.list ) {
        copied++;
        return 0;
    }

    rc = ensure_dir();
    if ( rc != 0 ) {
        return rc < 0 ? -1 : 0;
    }
    if ( exists && (there.attr & (ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM)) ) {
        if ( (there.attr & ATTR_READONLY) && !opt.readonly ) {
            return file_error( ERR_ACCESS, disp, dst );
        }
        sys_set_attr( dst, there.attr & ATTR_ARCHIVE );
    }
    rc = copy_data( src, dst, ent );
    if ( broken ) {
        return -1;
    }
    if ( rc != 0 ) {
        return file_error( rc, disp, dst );
    }
    copied++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* the walk                                                            */
/* ------------------------------------------------------------------ */

static int walk( void )
{
    char dir[PATH_MAX];
    ELIST files, subs;
    ENTRY *ent;
    u32 pos, slen, dlen;
    int rc = 0;

    mem_set( &files, 0, sizeof( files ) );
    mem_set( &subs, 0, sizeof( subs ) );
    if ( !str_cpyn( dir, job.src_full, sizeof( dir ) ) || !str_catn( dir, src_rel, sizeof( dir ) ) ) {
        too_long = 1;
        return 0;
    }

    if ( opt.empty ) {                          /* /E: every level, empty or not */
        if ( ensure_dir() < 0 ) {
            return -1;
        }
    }

    list_dir( dir, job.pattern, 0, &files );
    for ( pos = 0; rc == 0 && (ent = list_next( &files, &pos )) != NULL; ) {
        if ( check_break() ) {
            rc = -1;
            break;
        }
        rc = do_file( ent );
    }
    xfree( files.buf );

    if ( rc == 0 && opt.subdirs ) {
        list_dir( dir, "*.*", 1, &subs );
        slen = str_len( src_rel );
        dlen = str_len( dst_rel );
        for ( pos = 0; rc == 0 && (ent = list_next( &subs, &pos )) != NULL; ) {
            if ( check_break() ) {
                rc = -1;
                break;
            }
            if ( (ent->attr & (ATTR_HIDDEN | ATTR_SYSTEM)) && !opt.hidden ) {
                continue;
            }
            if ( !str_catn( src_rel, ent_name( ent ), PATH_MAX - 1 )
                 || !str_catn( src_rel, "\\", PATH_MAX - 1 )
                 || !str_catn( dst_rel, opt.short_names ? ent_alias( ent ) : ent_name( ent ),
                               PATH_MAX - 1 )
                 || !str_catn( dst_rel, "\\", PATH_MAX - 1 ) ) {
                too_long = 1;
            } else {
                rc = walk();
            }
            src_rel[slen] = 0;
            dst_rel[dlen] = 0;
        }
        xfree( subs.buf );
    }
    return rc;
}

int copy_run( void )
{
    char count[16];

    copylen = COPYBUF_MAX;
    while ( (copybuf = (u8 *)try_alloc( copylen )) == NULL ) {
        copylen /= 2;
        if ( copylen < 512 ) {
            out_of_memory();
        }
    }
    src_rel[0] = 0;
    dst_rel[0] = 0;
    made[0] = 0;

    walk();

    if ( broken ) {
        return EXIT_BREAK;
    }
    if ( matched == 0 && !opt.tree ) {
        out_msg( M_NOTFOUND, job.pattern, NULL );
    }
    if ( too_long ) {
        out_line( M_LONGWARN );
    }
    fmt_dec( copied, 9, count );
    out_msg( opt.list ? M_FILES : M_COPIED, count, NULL );
    if ( error_exit ) {
        return error_exit;
    }
    if ( opt.tree ) {
        return EXIT_OK;
    }
    return copied ? EXIT_OK : EXIT_NONE;
}
