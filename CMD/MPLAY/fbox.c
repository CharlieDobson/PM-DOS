/*
 * FBOX.C - the file box: Open and Save As.
 *
 * EDIT'S FILE DIALOG BY WAY OF QBASIC'S, which fixed three things in
 * it (Enter in the directory list no longer presses OK, a pattern at a
 * root lists the root, a name too long for a path is refused), with
 * what a word processor's needs added: a list of the kinds of file,
 * a list of the drives, long names, and its own care for the two
 * things the old ones left to the caller - Cancel puts the drive and
 * directory back as they were, and Save As asks before a file is
 * replaced.
 *
 * THE DIRECTORY SHOWN IS THE CURRENT ONE.  Going into a directory, or
 * to another drive, is done with DOS's own current drive and
 * directory, so a name typed without a path means what it would mean
 * at the prompt.  That is why Cancel has something to put back.
 *
 * THE FILES ARE CHOSEN BY PATTERN, HERE AND NOT BY DOS.  One search of
 * "*.*" reads the directory, and each name is tried against every
 * pattern of the kind listed ("*.DOC;*.DOT") - its long name and its
 * 8.3 one, since "*.DOC" should find LETTER~1.DOC however long its
 * name is.  A pattern typed into the name field replaces the list's.
 *
 * A DRIVE THAT DOES NOT ANSWER is found out by looking: SYS_D32.C
 * fails every critical error, so a drive with no diskette in the drive
 * is a search that comes back with an error and sys_crit_seen() set.
 * The box says so and stays on the drive it was on.
 */
#include "rt.h"
#include "ui.h"

#define MAX_TYPES       24
#define MAX_NAMES       4000    /* entries a list is let grow to */

enum { C_NAME_LABEL, C_NAME, C_FILES, C_DIRS_LABEL, C_DIRS, C_DIRTEXT, C_TYPE_LABEL, C_TYPE,
       C_DRIVE_LABEL, C_DRIVE, C_OK, C_CANCEL, C_COUNT };

typedef struct {
    char **item;
    int    count, cap;
} STRLIST;

typedef struct {
    STRLIST files, dirs;
    char    name[PATH_LEN];             /* the field */
    char    pattern[PATH_LEN];          /* the patterns the files are chosen by */
    char    dirtext[40];                /* the directory, cut to fit its line */
    char    result[PATH_LEN];
    const FBTYPE *types;
    int     ntypes;
    const char *type_label[MAX_TYPES];
    char    drive_text[26][16];
    const char *drive_label[26];
    int     drive_of[26];               /* the drive each line of the list is */
    int     ndrives;
    int     save;
} FDATA;

static const FBTYPE all_files = { "All files (*.*)", "*.*" };

/* ------------------------------------------------------------------ */
/* lists of names                                                      */
/* ------------------------------------------------------------------ */

static void sl_free( STRLIST *list )
{
    int index;

    for ( index = 0; index < list->count; index++ ) {
        xfree( list->item[index] );
    }
    xfree( list->item );
    list->item = NULL;
    list->count = list->cap = 0;
}

/* 0 when there is no memory for it, or the list is as long as it may
   be: the name is left out, and the rest of the directory still shows */
static int sl_add( STRLIST *list, const char *text )
{
    char **grown, *copy;
    int cap;

    if ( list->count >= MAX_NAMES ) {
        return 0;
    }
    if ( list->count == list->cap ) {
        cap = list->cap ? list->cap * 2 : 64;
        grown = (char **)try_realloc( list->item, (unsigned int)cap * sizeof( char * ) );
        if ( grown == NULL ) {
            return 0;
        }
        list->item = grown;
        list->cap = cap;
    }
    copy = (char *)try_alloc( str_len( text ) + 1 );
    if ( copy == NULL ) {
        return 0;
    }
    str_cpy( copy, text );
    list->item[list->count++] = copy;
    return 1;
}

/* ".." before everything, then by the alphabet */
static int sl_before( const char *lhs, const char *rhs )
{
    int ldots = lhs[0] == '.' && lhs[1] == '.' && lhs[2] == 0;
    int rdots = rhs[0] == '.' && rhs[1] == '.' && rhs[2] == 0;

    if ( ldots != rdots ) {
        return ldots;
    }
    return str_icmp( lhs, rhs ) < 0;
}

/* Shell's sort: a directory of a few thousand names is too many for
   the plain insertion sort the old dialogs used */
static void sl_sort( STRLIST *list )
{
    int gap, index, back;
    char *hold;

    for ( gap = list->count / 2; gap > 0; gap /= 2 ) {
        for ( index = gap; index < list->count; index++ ) {
            hold = list->item[index];
            for ( back = index; back >= gap && sl_before( hold, list->item[back - gap] ); back -= gap ) {
                list->item[back] = list->item[back - gap];
            }
            list->item[back] = hold;
        }
    }
}

/* ------------------------------------------------------------------ */
/* names and patterns                                                  */
/* ------------------------------------------------------------------ */

static int has_wild( const char *name )
{
    return str_chr( name, '*' ) != NULL || str_chr( name, '?' ) != NULL;
}

/* what follows the last \, / or : */
static const char *name_part( const char *path )
{
    const char *name = path;

    for ( ; *path; path++ ) {
        if ( *path == '\\' || *path == '/' || *path == ':' ) {
            name = path + 1;
        }
    }
    return name;
}

/* '*' is any run of characters and '?' any one; letters in either case */
static int glob( const char *pat, const char *name )
{
    const char *star = NULL, *back = NULL;

    while ( *name ) {
        if ( *pat == '*' ) {
            star = pat++;
            back = name;
        } else if ( *pat == '?' || ch_upper( *pat ) == ch_upper( *name ) ) {
            pat++;
            name++;
        } else if ( star != NULL ) {
            pat = star + 1;
            name = ++back;
        } else {
            return 0;
        }
    }
    while ( *pat == '*' ) {
        pat++;
    }
    return *pat == 0;
}

/* one pattern, as DOS means it: "*.*" is every name, and a name with
   no dot has an empty extension for "*." and "X.*" to match */
static int wild_match( const char *pat, const char *name )
{
    char dotted[PATH_LEN + 2];

    if ( str_cmp( pat, "*.*" ) == 0 || glob( pat, name ) ) {
        return 1;
    }
    if ( str_chr( name, '.' ) == NULL && str_len( name ) < PATH_LEN ) {
        sfmt( dotted, sizeof( dotted ), "%s.", name );
        return glob( pat, dotted );
    }
    return 0;
}

/* a name against patterns with ; between them */
static int patterns_match( const char *patterns, const char *name )
{
    char one[80];
    unsigned int len;

    while ( *patterns ) {
        for ( len = 0; patterns[len] && patterns[len] != ';'; len++ ) {
        }
        if ( len && len < sizeof( one ) ) {
            mem_cpy( one, patterns, len );
            one[len] = 0;
            if ( wild_match( one, name ) ) {
                return 1;
            }
        }
        patterns += len;
        if ( *patterns == ';' ) {
            patterns++;
        }
    }
    return 0;
}

/* The extension a name with none is given: the first pattern's, when
   that pattern is "*." and plain letters - "*.DOC;*.DOT" gives ".DOC",
   "*.*" gives nothing.  0 when there is none. */
static int default_ext( const char *patterns, char *ext, unsigned int max )
{
    unsigned int len;

    if ( patterns[0] != '*' || patterns[1] != '.' ) {
        return 0;
    }
    patterns++;
    for ( len = 0; patterns[len] && patterns[len] != ';'; len++ ) {
        if ( patterns[len] == '*' || patterns[len] == '?' ) {
            return 0;
        }
    }
    if ( len < 2 || len >= max ) {
        return 0;
    }
    mem_cpy( ext, patterns, len );
    ext[len] = 0;
    return 1;
}

/* the full name of what was typed: -> 0 if it will not fit a path */
static int make_path( const char *name, char *out )
{
    int drv;
    const char *rest = name;
    char dir[PATH_LEN];

    if ( name[0] && name[1] == ':' ) {
        drv = ch_upper( name[0] ) - 'A';
        rest = name + 2;
    } else {
        drv = sys_get_drive();
    }
    out[0] = (char)('A' + drv);
    out[1] = ':';
    out[2] = 0;
    if ( rest[0] == '\\' || rest[0] == '/' ) {
        return str_catn( out, rest, PATH_LEN );
    }
    out[2] = '\\';
    out[3] = 0;
    if ( sys_get_cwd( drv, dir ) == 0 && dir[0] ) {
        if ( !str_catn( out, dir, PATH_LEN ) || !str_catn( out, "\\", PATH_LEN ) ) {
            return 0;
        }
    }
    return str_catn( out, rest, PATH_LEN );
}

/* ------------------------------------------------------------------ */
/* drives and directories                                              */
/* ------------------------------------------------------------------ */

/* Is the current drive there to be read?  A search of its directory
   says: one that comes back with a critical error failed, or with an
   error that is not "no files", has no disk to read. */
static int drive_ready( void )
{
    FINDREC *rec;
    uint32_t handle;
    int rc, ready;

    rec = (FINDREC *)try_alloc( sizeof( FINDREC ) );
    if ( rec == NULL ) {
        return 1;
    }
    sys_crit_seen();
    rc = sys_find_first( "*.*", ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_DIR | ATTR_ARCHIVE,
                         rec, &handle );
    if ( rc == 0 ) {
        sys_find_close( handle );
    }
    ready = !sys_crit_seen() && (rc == 0 || rc == ERR_NOFILE || rc == ERR_NOMORE);
    xfree( rec );
    return ready;
}

static void not_ready( int drv )
{
    char text[60];

    sfmt( text, sizeof( text ), "Drive %c: is not ready.", 'A' + drv );
    msg_box( text, MB_OK );
}

/* To a drive ("C:"), a directory, or both ("C:\DOCS"): 1 = there.  A
   drive that is not ready, or a directory that is not there, is said
   and the drive stays the one it was. */
static int go_to( const char *where )
{
    int old = sys_get_drive(), drv = old, rc = 0;

    if ( where[0] && where[1] == ':' ) {
        drv = ch_upper( where[0] ) - 'A';
        if ( drv < 0 || drv > 25 || !sys_drive_valid( drv ) ) {
            msg_error( where, ERR_BADDRIVE );
            return 0;
        }
    }
    sys_crit_seen();
    if ( drv != old ) {
        sys_set_drive( drv );
    }
    if ( !(where[0] && where[1] == ':' && where[2] == 0) && where[0] ) {
        rc = sys_chdir( where );
    }
    if ( sys_crit_seen() || !drive_ready() ) {
        sys_set_drive( old );
        not_ready( drv );
        return 0;
    }
    if ( rc != 0 ) {
        sys_set_drive( old );
        msg_error( where, rc == ERR_NOFILE ? ERR_NOPATH : rc );
        return 0;
    }
    return 1;
}

/* the current directory, whole: "C:\DOCS\LETTERS" */
static void cur_dir( char *out )
{
    int drv = sys_get_drive();

    out[0] = (char)('A' + drv);
    out[1] = ':';
    out[2] = '\\';
    if ( sys_get_cwd( drv, out + 3 ) != 0 ) {
        out[3] = 0;
    }
}

static void fill_lists( FDATA *fd )
{
    FINDREC *rec;
    uint32_t handle;
    char dir[PATH_LEN + 4];
    unsigned int len;

    sl_free( &fd->files );
    sl_free( &fd->dirs );
    /* the directory's name, its end kept when it is too long to show */
    cur_dir( dir );
    len = str_len( dir );
    if ( len < sizeof( fd->dirtext ) ) {
        str_cpy( fd->dirtext, dir );
    } else {
        sfmt( fd->dirtext, sizeof( fd->dirtext ), "...%s", dir + len - (sizeof( fd->dirtext ) - 4) );
    }
    rec = (FINDREC *)try_alloc( sizeof( FINDREC ) );
    if ( rec == NULL ) {
        return;
    }
    sys_crit_seen();
    if ( sys_find_first( "*.*", ATTR_READONLY | ATTR_ARCHIVE | ATTR_DIR, rec, &handle ) == 0 ) {
        do {
            if ( rec->attr & ATTR_DIR ) {
                if ( str_cmp( rec->name, "." ) != 0 ) {
                    sl_add( &fd->dirs, rec->name );
                }
            } else if ( patterns_match( fd->pattern, rec->name )
                        || patterns_match( fd->pattern, rec->alias ) ) {
                sl_add( &fd->files, rec->name );
            }
        } while ( sys_find_next( handle, rec ) == 0 );
        sys_find_close( handle );
    }
    sys_crit_seen();
    xfree( rec );
    sl_sort( &fd->files );
    sl_sort( &fd->dirs );
}

static const char *files_item( DLG *dlg, int index )
{
    FDATA *fd = (FDATA *)dlg->data;

    return index < fd->files.count ? fd->files.item[index] : "";
}

static const char *dirs_item( DLG *dlg, int index )
{
    FDATA *fd = (FDATA *)dlg->data;

    return index < fd->dirs.count ? fd->dirs.item[index] : "";
}

/* the lists read again, and the drive list on the drive that is current */
static void refresh( DLG *dlg )
{
    FDATA *fd = (FDATA *)dlg->data;
    int index, drv = sys_get_drive();

    fill_lists( fd );
    dlg->ctl[C_FILES].count = fd->files.count;
    dlg->ctl[C_FILES].sel = 0;
    dlg->ctl[C_FILES].top = 0;
    dlg->ctl[C_DIRS].count = fd->dirs.count;
    dlg->ctl[C_DIRS].sel = 0;
    dlg->ctl[C_DIRS].top = 0;
    for ( index = 0; index < fd->ndrives; index++ ) {
        if ( fd->drive_of[index] == drv ) {
            dlg->ctl[C_DRIVE].sel = index;
        }
    }
}

static void name_set( DLG *dlg, const char *text )
{
    FDATA *fd = (FDATA *)dlg->data;

    sfmt( fd->name, sizeof( fd->name ), "%s", text );
    dlg->ctl[C_NAME].caret = (int)str_len( fd->name );
    dlg->ctl[C_NAME].scroll = 0;
    dlg->ctl[C_NAME].fresh = 1;
}

static void fb_changed( DLG *dlg, int ctl )
{
    FDATA *fd = (FDATA *)dlg->data;

    switch ( ctl ) {
    case C_FILES:                       /* a file pointed at: its name in the field */
        if ( dlg->ctl[C_FILES].sel < fd->files.count ) {
            name_set( dlg, fd->files.item[dlg->ctl[C_FILES].sel] );
        }
        break;
    case C_TYPE:                        /* another kind of file listed */
        sfmt( fd->pattern, sizeof( fd->pattern ), "%s", fd->types[dlg->ctl[C_TYPE].sel].patterns );
        if ( !fd->save || has_wild( fd->name ) ) {
            name_set( dlg, fd->pattern );
        }
        refresh( dlg );
        break;
    case C_DRIVE:                       /* another drive - if it answers */
        if ( dlg->ctl[C_DRIVE].sel < fd->ndrives ) {
            char where[4];
            where[0] = (char)('A' + fd->drive_of[dlg->ctl[C_DRIVE].sel]);
            where[1] = ':';
            where[2] = 0;
            go_to( where );
            refresh( dlg );             /* which puts the list back on the drive it is */
        }
        break;
    }
}

/* the name in the field, acted on: 1 when it names the file to give back */
static int take_name( DLG *dlg )
{
    FDATA *fd = (FDATA *)dlg->data;
    char ext[16], text[PATH_LEN + 40];
    const char *last;
    char *slash;
    uint16_t attr;

    if ( fd->name[0] == 0 ) {
        return 0;
    }
    if ( has_wild( fd->name ) ) {
        /* a pattern, with a directory in front of it or not */
        slash = str_rchr( fd->name, '\\' );
        if ( slash != NULL ) {
            sfmt( fd->pattern, sizeof( fd->pattern ), "%s", slash + 1 );
            if ( slash == fd->name || (slash == fd->name + 2 && fd->name[1] == ':') ) {
                slash[1] = 0;           /* C:\*.DOC: the root, not C:'s directory */
            } else {
                *slash = 0;
            }
            go_to( fd->name );
        } else if ( fd->name[1] == ':' ) {
            sfmt( fd->pattern, sizeof( fd->pattern ), "%s", fd->name + 2 );
            fd->name[2] = 0;
            go_to( fd->name );
        } else {
            sfmt( fd->pattern, sizeof( fd->pattern ), "%s", fd->name );
        }
        if ( fd->pattern[0] == 0 ) {
            str_cpy( fd->pattern, "*.*" );
        }
        name_set( dlg, fd->pattern );
        refresh( dlg );
        return 0;
    }
    if ( (fd->name[1] == ':' && fd->name[2] == 0)
         || (sys_get_attr( fd->name, &attr ) == 0 && (attr & ATTR_DIR)) ) {
        /* a drive or a directory: go there */
        if ( go_to( fd->name ) ) {
            name_set( dlg, fd->save ? "" : fd->pattern );
        }
        refresh( dlg );
        return 0;
    }
    sys_crit_seen();
    if ( !make_path( fd->name, fd->result ) ) {
        msg_box( "That name is too long.", MB_OK );
        return 0;
    }
    /* a name with no extension gets the listed kind's */
    last = name_part( fd->result );
    if ( str_chr( last, '.' ) == NULL && default_ext( fd->pattern, ext, sizeof( ext ) ) ) {
        if ( fd->save || sys_get_attr( fd->result, &attr ) != 0 ) {
            if ( !str_catn( fd->result, ext, PATH_LEN ) ) {
                msg_box( "That name is too long.", MB_OK );
                return 0;
            }
        }
    }
    last = name_part( fd->result );
    if ( *last == 0 ) {
        return 0;
    }
    if ( fd->save ) {
        if ( sys_get_attr( fd->result, &attr ) == 0 ) {
            sfmt( text, sizeof( text ), "%s is already there.\nReplace it?", last );
            if ( msg_box( text, MB_YESNO ) != ID_YES ) {
                return 0;
            }
        }
    } else if ( sys_get_attr( fd->result, &attr ) != 0 ) {
        if ( sys_crit_seen() ) {
            not_ready( ch_upper( fd->result[0] ) - 'A' );
        } else {
            sfmt( text, sizeof( text ), "%s was not found.", last );
            msg_box( text, MB_OK );
        }
        return 0;
    }
    return 1;
}

static int fb_activate( DLG *dlg, int ctl )
{
    FDATA *fd = (FDATA *)dlg->data;

    dlg->stay = 1;                      /* Enter in a list is that list's, never OK's as well */
    switch ( ctl ) {
    case C_CANCEL:
        return ID_CANCEL;
    case C_DIRS:
        if ( dlg->ctl[C_DIRS].sel < fd->dirs.count && go_to( fd->dirs.item[dlg->ctl[C_DIRS].sel] ) ) {
            if ( !fd->save ) {
                name_set( dlg, fd->pattern );
            }
        }
        refresh( dlg );
        return 0;
    case C_FILES:
        fb_changed( dlg, C_FILES );
        break;
    }
    return take_name( dlg ) ? ID_OK : 0;
}

/* ------------------------------------------------------------------ */
/* the box                                                             */
/* ------------------------------------------------------------------ */

static void list_drives( FDATA *fd )
{
    static const char *const kinds[5] = { "", "Diskette", "Hard disk", "CD-ROM", "Drive" };
    int drv, kind;

    fd->ndrives = 0;
    for ( drv = 0; drv < 26; drv++ ) {
        if ( !sys_drive_valid( drv ) ) {
            continue;
        }
        kind = sys_drive_kind( drv );
        if ( kind < 0 || kind > 4 ) {
            kind = 0;
        }
        sfmt( fd->drive_text[fd->ndrives], sizeof( fd->drive_text[0] ), "%c:  %s", 'A' + drv, kinds[kind] );
        fd->drive_label[fd->ndrives] = fd->drive_text[fd->ndrives];
        fd->drive_of[fd->ndrives] = drv;
        fd->ndrives++;
    }
}

static int fb_run( const char *title, char *path, const FBTYPE *types, int ntypes, int *type,
                     int save )
{
    FDATA *fd;
    CTL ctl[C_COUNT];
    DLG dlg;
    char start_dir[PATH_LEN + 4], begin[PATH_LEN];
    const char *name;
    int start_drive, answer, index, first = type ? *type : 0;
    uint16_t attr;

    fd = (FDATA *)try_alloc( sizeof( FDATA ) );
    if ( fd == NULL ) {
        msg_error( title, ERR_NOMEM );
        return 0;
    }
    mem_set( fd, 0, sizeof( FDATA ) );
    mem_set( &dlg, 0, sizeof( dlg ) );
    if ( types == NULL || ntypes <= 0 ) {
        types = &all_files;
        ntypes = 1;
    }
    if ( ntypes > MAX_TYPES ) {
        ntypes = MAX_TYPES;
    }
    if ( first < 0 || first >= ntypes ) {
        first = 0;
    }
    fd->types = types;
    fd->ntypes = ntypes;
    fd->save = save;
    for ( index = 0; index < ntypes; index++ ) {
        fd->type_label[index] = types[index].label;
    }
    sfmt( fd->pattern, sizeof( fd->pattern ), "%s", types[first].patterns );
    list_drives( fd );

    /* where things stand, for Cancel to put back */
    start_drive = sys_get_drive();
    cur_dir( start_dir );

    /* where to start: the directory of the path that came in, and its
       name in the field if it has one */
    sfmt( begin, sizeof( begin ), "%s", path );
    name = "";
    if ( begin[0] ) {
        sys_crit_seen();
        if ( (begin[1] == ':' && begin[2] == 0)
             || (sys_get_attr( begin, &attr ) == 0 && (attr & ATTR_DIR)) ) {
            go_to( begin );
        } else {
            name = name_part( begin );
            if ( name > begin ) {
                /* cut the name off, and keep the backslash of a root */
                index = (int)(name - begin);
                sfmt( fd->name, sizeof( fd->name ), "%s", name );
                if ( index > 1 && begin[index - 1] == '\\' && begin[index - 2] != ':' ) {
                    index--;
                }
                begin[index] = 0;
                go_to( begin );
                name = fd->name;
            }
        }
    }
    sfmt( begin, sizeof( begin ), "%s", name );         /* fd->name is about to be the field */
    sfmt( fd->name, sizeof( fd->name ), "%s", save || has_wild( begin ) ? begin : fd->pattern );
    if ( !save && begin[0] && !has_wild( begin ) ) {
        sfmt( fd->name, sizeof( fd->name ), "%s", begin );
    }

    ctl_label( &ctl[C_NAME_LABEL], 2, 3, "File &Name:" );
    ctl_edit( &ctl[C_NAME], 3, 3, 31, fd->name, PATH_LEN );
    ctl_list( &ctl[C_FILES], 5, 2, 34, 8, "&Files", NULL, 0 );
    ctl[C_FILES].item = files_item;
    ctl_label( &ctl[C_DIRS_LABEL], 2, 38, "&Directories:" );
    ctl_list( &ctl[C_DIRS], 5, 38, 20, 8, NULL, NULL, 0 );
    ctl[C_DIRS].item = dirs_item;
    ctl_text( &ctl[C_DIRTEXT], 3, 38, 0, fd->dirtext );
    ctl_label( &ctl[C_TYPE_LABEL], 14, 3, "List Files of &Type:" );
    ctl_drop( &ctl[C_TYPE], 15, 3, 32, fd->type_label, ntypes );
    ctl[C_TYPE].sel = first;
    ctl_label( &ctl[C_DRIVE_LABEL], 14, 39, "Dri&ves:" );
    ctl_drop( &ctl[C_DRIVE], 15, 39, 19, fd->drive_label, fd->ndrives );
    ctl[C_DRIVE].drop_rows = 6;
    ctl_button( &ctl[C_OK], 5, 60, "  OK  ", ID_OK, 1 );
    ctl_button( &ctl[C_CANCEL], 7, 60, "Cancel", ID_CANCEL, 0 );
    dlg.title = title;
    dlg.rows = 18;
    dlg.cols = 72;
    dlg.ctl = ctl;
    dlg.nctl = C_COUNT;
    dlg.focus = C_NAME;
    dlg.data = fd;
    dlg.changed = fb_changed;
    dlg.activate = fb_activate;
    refresh( &dlg );
    answer = dlg_run( &dlg );
    if ( answer == ID_OK ) {
        str_cpy( path, fd->result );
        if ( type != NULL ) {
            *type = ctl[C_TYPE].sel;
        }
    } else {
        /* as it was: the drive, and the directory it was in */
        sys_set_drive( start_drive );
        sys_chdir( start_dir );
        sys_crit_seen();
    }
    sl_free( &fd->files );
    sl_free( &fd->dirs );
    xfree( fd );
    return answer == ID_OK;
}

int fbox_open( const char *title, char *path, const FBTYPE *types, int ntypes, int *type )
{
    return fb_run( title ? title : "Open", path, types, ntypes, type, 0 );
}

int fbox_save( const char *title, char *path, const FBTYPE *types, int ntypes, int *type )
{
    return fb_run( title ? title : "Save As", path, types, ntypes, type, 1 );
}
