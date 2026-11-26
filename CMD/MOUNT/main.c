/*
 * MAIN.C - MOUNT.EXE, a file given a drive letter.
 *
 *   MOUNT                   what is mounted now
 *   MOUNT E: DISK.IMG       give that file the letter E:
 *   MOUNT DISK.IMG          ...or the next free letter
 *   MOUNT E: CD.ISO /R      read-only, which an .ISO is anyway
 *   MOUNT /U E:             and give the letter back
 *
 * WHAT IS IN THE FILE DECIDES HOW IT IS READ, not what it is called:
 * the kernel looks at block 16 and sector 0 (DOS\IMAGE.INC img_sniff).
 * The one exception is .IMZ, a zipped .IMA, which has to be unpacked
 * before there is anything to look at.
 *
 * THREE WAYS TO MOUNT, and the kernel picks two of them by itself
 * (21h/F1h AL=0Fh).  A file on a local FAT drive is MAPPED: its cluster
 * chain becomes a list of sectors once, and the letter reads and writes
 * the file in place.  A file on a NETWORK DRIVE is read and written
 * where it is, through the redirector - any size, .ISO or hard disk.
 * A file on neither - a CD, exFAT, a RAM disk, another image - comes
 * back as error 50, and a diskette image (.IMG, .IMA or .DSK, 2.88M at
 * most) is then LOADED: read here and handed to the kernel whole
 * (AL=2Bh IMEM_LOAD), which keeps a copy.  A .IMZ is always loaded,
 * wherever it is.  Anything else is refused there (Charlie,
 * 2026-11-25).
 *
 * A LOADED IMAGE IS SAVED BY MOUNT /U.  The kernel hands the image back
 * (IMEM_COPY) with its write count, this program writes it over the
 * file - packed again for a .IMZ - and only then unmounts, giving the
 * count back (AL=10h BH=2) so that a write made after the copy is
 * refused rather than lost.  When the file cannot be written the drive
 * stays mounted, and the user is asked whether to unmount it anyway.
 */
#include "rt.h"
#include "sys.h"
#include "imz.h"

#define MEM_MAX         2949120UL       /* IMG_MEM_MAX: a 2.88M diskette */
#define IMZ_SLACK       65536UL         /* a stored .IMZ is a little over */
#define TAIL_READ       70000UL         /* the end of a .IMZ: its directory */
#define PATH_KEEP       79              /* IM_PATH's 80 bytes, NUL included */

#define KIND_OTHER      0
#define KIND_DISK       1               /* .IMG .IMA .DSK */
#define KIND_IMZ        2

typedef struct {
    int  letter;                        /* 0 = A:, -1 = none given */
    int  have_path;
    int  ro, umount, help;
    char path[PATH_MAX];
} ARGS;

static int thou;                        /* the country's thousands separator */

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

static void say( const char *text )
{
    sys_write( H_OUT, text, str_len( text ) );
}

static void eol( void )
{
    say( "\r\n" );
}

static void say_drive( int letter )
{
    char text[3];

    text[0] = (char)('A' + letter);
    text[1] = ':';
    text[2] = 0;
    say( text );
}

static void say_num( unsigned int val, int width )
{
    char text[24];

    say( fmt_grp( val, width, thou, text ) );
}

static void usage( void )
{
    say( "\r\nMOUNT - Mount a file as a drive\r\n\r\n"
         "  MOUNT                 List currently mounted drives.\r\n"
         "  MOUNT d: file         Mount file as drive d:.\r\n"
         "  MOUNT file            Mount file as next available drive.\r\n"
         "  MOUNT d: file /R      Mount file as read-only.\r\n"
         "  MOUNT /U d:           Unmount drive d:\r\n\r\n"
         "An .ISO is read as ISO9660 or UDF and an .IMG, .IMA,\r\n"
         ".IMZ or .DSK as FAT.  A file on a network drive is read\r\n"
         "and written where it is.  A diskette image on a CD or\r\n"
         "exFAT drive, and any .IMZ, is loaded into memory, and\r\n"
         "MOUNT /U saves changes back to the file.\r\n\r\n" );
}

/* ------------------------------------------------------------------ */
/* what went wrong                                                     */
/* ------------------------------------------------------------------ */

#define WHY_DISKONLY    "only a diskette image (.IMG, .IMA, .IMZ or .DSK) can be " \
                        "mounted from this drive - copy the file to a hard disk " \
                        "or a network drive first"
#define WHY_TOOBIG      "the file is larger than a 2.88 MB diskette - copy the " \
                        "file to a hard disk or a network drive first"
#define WHY_LONGPATH    "the full path is longer than 79 characters"
#define WHY_LOADMEM     "not enough memory to load the image"
#define WHY_NOREAD      "the file cannot be read"

/* A mount's errors, which are not a file operation's: "access denied"
   alone would send the reader looking for a permission. */
static const char *why_mount( int err, int loaded )
{
    switch ( err ) {
    case ERR_NOFILE:   return "no such file";
    case ERR_NOPATH:   return "no such directory";
    case ERR_ACCESS:   return "the file is in use, is a directory, or is empty";
    case ERR_NOMEM:    return loaded ? WHY_LOADMEM : "not enough memory for the extent map";
    case ERR_BADDATA:  return "nothing in it that this kernel can read - not a FAT "
                              "volume, a partitioned disk, or an ISO9660 or UDF disc";
    case ERR_BADDRIVE: return "no such drive letter, or it is already in use";
    case ERR_GENERAL:  return "the cluster chain does not cover the file - run CHKDSK";
    case ERR_NOTSUP:   return WHY_DISKONLY;
    case ERR_DISKFULL: return "the file is in too many pieces - defragment the volume";
    case ERR_EXISTS:   return "that file already has a drive letter";
    }
    return NULL;
}

/* ...and a file's, for reading and saving one */
static const char *why_file( int err )
{
    switch ( err ) {
    case ERR_NOFILE:   return "no such file";
    case ERR_NOPATH:   return "no such directory";
    case ERR_ACCESS:   return "access denied";
    case ERR_NOMEM:    return "not enough memory";
    case ERR_WRFAULT:  return "the drive could not be written to";
    case ERR_DISKFULL: return "the drive is full";
    }
    return NULL;
}

static const char *why_imz( int why )
{
    switch ( why ) {
    case IMZ_NOMEM:  return WHY_LOADMEM;
    case IMZ_UNSUPP: return "the .IMZ file is encrypted, or packed in a way MOUNT cannot read";
    case IMZ_EMPTY:  return "the .IMZ file has no image in it";
    case IMZ_TOOBIG: return "the image in the .IMZ file is larger than a 2.88 MB diskette";
    case IMZ_BADCRC: return "the image in the .IMZ file is damaged (CRC error)";
    }
    return "the file is not a ZIP archive, or is damaged";
}

static void say_why( const char *why, int err )
{
    char num[16];

    if ( why != NULL ) {
        say( why );
    } else {
        say( "error " );
        say( fmt_dec( (unsigned int)err, 0, num ) );
    }
}

/* THE PATH IS PRINTED BACK, and it is not decoration: "no such file"
   about a name the user typed and a name this program parsed are two
   different statements, and only the second is what the kernel was
   asked. */
static int mount_failed( const char *path, const char *why, int err )
{
    say( "Cannot mount " );
    say( path );
    say( ": " );
    say_why( why, err );
    eol();
    return 1;
}

static int unmount_failed( int letter, int err )
{
    say( "Cannot unmount " );
    say_drive( letter );
    say( " - " );
    if ( err == ERR_BADDRIVE ) {
        say_drive( letter );
        say( " is not a mounted file" );
    } else if ( err == ERR_ACCESS ) {
        say( "a file on the drive is open" );
    } else {
        say_why( why_file( err ), err );
    }
    eol();
    return 1;
}

/* ------------------------------------------------------------------ */
/* names                                                               */
/* ------------------------------------------------------------------ */

/* where the last component of a path starts */
static const char *base_name( const char *path )
{
    const char *base = path;

    for ( ; *path; path++ ) {
        if ( *path == '\\' || *path == '/' || *path == ':' ) {
            base = path + 1;
        }
    }
    return base;
}

static int file_kind( const char *path )
{
    const char *dot = NULL, *cur;

    for ( cur = base_name( path ); *cur; cur++ ) {
        if ( *cur == '.' ) {
            dot = cur;
        }
    }
    if ( dot == NULL ) {
        return KIND_OTHER;
    }
    if ( str_nicmp( dot, ".IMZ", 5 ) == 0 ) {
        return KIND_IMZ;
    }
    if ( str_nicmp( dot, ".IMG", 5 ) == 0 || str_nicmp( dot, ".IMA", 5 ) == 0 ||
         str_nicmp( dot, ".DSK", 5 ) == 0 ) {
        return KIND_DISK;
    }
    return KIND_OTHER;
}

/* the mount a letter has: its slot, or -1 */
static int find_mount( int letter, REPORT *rep )
{
    int slot;

    for ( slot = 0; slot < IMG_MAX; slot++ ) {
        if ( sys_img_report( slot, rep ) == 0 && rep->letter == letter ) {
            return slot;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* MOUNT: the table                                                    */
/* ------------------------------------------------------------------ */

static void list( void )
{
    REPORT rep;
    int slot, any = 0;

    for ( slot = 0; slot < IMG_MAX; slot++ ) {
        if ( sys_img_report( slot, &rep ) != 0 ) {
            continue;
        }
        if ( !any ) {
            say( "\r\nDrive  Type   Mode  Extents      Size     File\r\n"
                 "-----  -----  ----  -------  -----------  "
                 "----------------------------------\r\n" );
            any = 1;
        }
        say_drive( rep.letter );
        say( "     " );
        /* exFAT is not FAT, whatever the file is called: the kernel
           says which it found when it mounted the file */
        if ( rep.flags & IMF_ISO ) {
            say( "ISO    " );
        } else if ( sys_drive_type( rep.letter ) == F1T_EXFAT ) {
            say( "exFAT  " );
        } else {
            say( "FAT    " );
        }
        say( (rep.flags & IMF_RO) ? "ro    " : "rw    " );
        /* the pieces the file is in, which is the one number that
           predicts trouble - or where a file with no map is read from */
        if ( rep.flags & IMF_MEM ) {
            say( " memory" );
        } else if ( rep.flags & IMF_NET ) {
            say( "network" );
        } else {
            say_num( rep.extents, 7 );
        }
        say( "  " );
        /* the size of the VOLUME, which a partitioned image has smaller
           than the file */
        say_num( rep.secs / 2, 8 );
        say( " KB  " );
        say( rep.path );
        eol();
    }
    if ( !any ) {
        say( "No image files are mounted.\r\n" );
    }
}

/* ------------------------------------------------------------------ */
/* MOUNT d: file                                                       */
/* ------------------------------------------------------------------ */

static void mounted( int letter, int loaded, int ro, const char *why_ro )
{
    say( "Mounted as " );
    say_drive( letter );
    if ( loaded ) {
        if ( ro ) {
            say( " in memory, read-only" );
            if ( why_ro != NULL ) {
                say( ": " );
                say( why_ro );
            }
            say( "." );
        } else {
            say( " in memory.  MOUNT /U " );
            say_drive( letter );
            say( " saves changes to the file." );
        }
    }
    eol();
}

/* A file the kernel could not map, read here whole and handed over. */
static int mount_loaded( int letter, const char *arg, int ro, int imz )
{
    char canon[PATH_MAX], name[IMZ_NAME_MAX];
    const char *why_ro = NULL;
    REPORT rep;
    uint32_t handle, size, where, got, img_len;
    unsigned int others;
    uint8_t *file, *img;
    uint16_t attr;
    int err, took, why;

    /* the name it will be saved back to, whole: a cut-off name in the
       kernel's 80 bytes would be somebody else's file */
    err = sys_truename( arg, canon, 1 );
    if ( err ) {
        return mount_failed( arg, why_file( err ), err );
    }
    if ( str_len( canon ) > PATH_KEEP ) {
        return mount_failed( canon, WHY_LONGPATH, 0 );
    }

    err = sys_open( canon, 0, &handle );
    if ( err ) {
        return mount_failed( canon, why_file( err ), err );
    }
    err = sys_seek( handle, 0, 2, &size );
    if ( !err ) {
        err = sys_seek( handle, 0, 0, &where );
    }
    if ( err ) {
        sys_close( handle );
        return mount_failed( canon, WHY_NOREAD, 0 );
    }
    if ( size < 512 ) {
        sys_close( handle );
        return mount_failed( canon, why_mount( ERR_ACCESS, 1 ), 0 );
    }
    if ( size > (imz ? MEM_MAX + IMZ_SLACK : MEM_MAX) ) {
        sys_close( handle );
        return mount_failed( canon, imz ? why_imz( IMZ_TOOBIG ) : WHY_TOOBIG, 0 );
    }
    file = (uint8_t *)try_alloc( size );
    if ( file == NULL ) {
        sys_close( handle );
        return mount_failed( canon, WHY_LOADMEM, 0 );
    }
    err = sys_read( handle, file, size, &got );
    sys_close( handle );
    if ( err || got != size ) {
        xfree( file );
        return mount_failed( canon, WHY_NOREAD, 0 );
    }

    /* A FILE THAT CANNOT BE WRITTEN IS MOUNTED READ-ONLY, so the changes
       nobody could save are never made.  Read-only on the host, on a
       CD, on a share that gives only read access: the open says. */
    if ( !ro ) {
        if ( sys_get_attr( canon, &attr ) == 0 && (attr & ATTR_READONLY) ) {
            why_ro = "the file is read-only";
        } else if ( sys_open( canon, 2, &handle ) != 0 ) {
            why_ro = "the file cannot be written to";
        } else {
            sys_close( handle );
        }
    }

    if ( imz ) {
        img = imz_unpack( file, size, MEM_MAX, &img_len, name, &others, &why );
        xfree( file );
        if ( img == NULL ) {
            return mount_failed( canon, why_imz( why ), 0 );
        }
        if ( others && why_ro == NULL ) {
            why_ro = "the .IMZ file holds more than one file";
        }
    } else {
        img = file;
        img_len = size;
    }

    err = sys_img_load( letter, ro || why_ro != NULL, img, img_len, canon, &took );
    xfree( img );
    if ( err ) {
        return mount_failed( canon, why_mount( err, 1 ), err );
    }
    /* the kernel makes an optical image read-only by itself */
    if ( find_mount( took, &rep ) >= 0 && (rep.flags & IMF_RO) ) {
        ro = 1;
    }
    mounted( took, 1, ro || why_ro != NULL, why_ro );
    return 0;
}

static int mount_file( int letter, const char *path, int ro )
{
    int kind = file_kind( path ), err, took;

    if ( kind != KIND_IMZ ) {
        err = sys_img_map( letter, ro, path, &took );
        if ( err == 0 ) {
            mounted( took, 0, ro, NULL );
            return 0;
        }
        if ( err != ERR_NOTSUP || kind != KIND_DISK ) {
            return mount_failed( path, why_mount( err, 0 ), err );
        }
    }
    return mount_loaded( letter, path, ro, kind == KIND_IMZ );
}

/* ------------------------------------------------------------------ */
/* MOUNT /U d:                                                         */
/* ------------------------------------------------------------------ */

/* the name a .IMZ's image has inside it: read from the end of the file
   as it is now, or the file's own name with .IMA when that fails */
static void imz_member( const char *path, char *name )
{
    uint32_t handle, size, base, want, where, got;
    uint8_t *tail;
    const char *base_at;
    unsigned int len;

    name[0] = 0;
    if ( sys_open( path, 0, &handle ) == 0 ) {
        if ( sys_seek( handle, 0, 2, &size ) == 0 ) {
            want = size < TAIL_READ ? size : TAIL_READ;
            base = size - want;
            tail = (uint8_t *)try_alloc( want ? want : 1 );
            if ( tail != NULL ) {
                if ( sys_seek( handle, base, 0, &where ) == 0 &&
                     sys_read( handle, tail, want, &got ) == 0 && got == want ) {
                    imz_name( tail, want, base, name );
                }
                xfree( tail );
            }
        }
        sys_close( handle );
    }
    if ( name[0] == 0 ) {
        base_at = base_name( path );
        for ( len = 0; base_at[len] && base_at[len] != '.' && len < 8; len++ ) {
            name[len] = base_at[len];
        }
        name[len] = 0;
        str_catn( name, ".IMA", IMZ_NAME_MAX );
    }
}

/* The whole file rewritten.  A raw image is the size it was, so it is
   written in place.  A .IMZ packs to a new size, and a write of no
   bytes - DOS's way to end a file - does nothing on a share (SMBFS
   does not truncate), so a smaller archive would keep the old one's
   tail, and its old end record with it: a .IMZ is CREATED over, which
   empties the file on a share as on a disk.  "recreate" says which. */
static int write_whole( const char *path, const uint8_t *data, uint32_t len, int recreate )
{
    uint32_t handle, where, put;
    int err, cerr;

    if ( recreate ) {
        err = sys_create( path, &handle );
    } else {
        err = sys_open( path, 2, &handle );
        if ( err == ERR_NOFILE ) {
            err = sys_create( path, &handle );  /* it was there when mounted */
        }
    }
    if ( err ) {
        return err;
    }
    err = sys_seek( handle, 0, 0, &where );
    if ( !err ) {
        err = sys_write_file( handle, data, len, &put );
    }
    if ( !err ) {
        err = sys_write_file( handle, data, 0, &put );
    }
    cerr = sys_close( handle );
    return err ? err : cerr;
}

/* The image back from the kernel and onto its file.  *copied is set
   once the kernel has given it up, so the caller can tell "the drive
   is busy" from "the file would not take it". */
static int save_image( int letter, const REPORT *rep, uint32_t *gen, int *copied )
{
    char name[IMZ_NAME_MAX];
    uint8_t *img, *zip;
    uint32_t len, zip_len;
    int err;

    *copied = 0;
    img = (uint8_t *)try_alloc( rep->size ? rep->size : 1 );
    if ( img == NULL ) {
        return ERR_NOMEM;
    }
    err = sys_img_copy( letter, img, rep->size, &len, gen );
    if ( err ) {
        xfree( img );
        return err;
    }
    *copied = 1;
    if ( file_kind( rep->path ) == KIND_IMZ ) {
        imz_member( rep->path, name );
        zip = imz_pack( img, len, name, &zip_len );
        xfree( img );
        if ( zip == NULL ) {
            return ERR_NOMEM;
        }
        err = write_whole( rep->path, zip, zip_len, 1 );
        xfree( zip );
    } else {
        err = write_whole( rep->path, img, len, 0 );
        xfree( img );
    }
    return err;
}

static int ask_yes( const char *question )
{
    char echo[2];
    int key;

    say( question );
    sys_key_flush();
    for ( ;; ) {
        key = sys_read_key();
        if ( key < 0 ) {                    /* the end of a file of answers */
            eol();
            return 0;
        }
        key = ch_upper( key );
        if ( key == 'Y' || key == 'N' ) {
            echo[0] = (char)key;
            echo[1] = 0;
            say( echo );
            eol();
            return key == 'Y';
        }
    }
}

static int unmount_drive( int letter )
{
    REPORT rep;
    uint32_t gen;
    int err = ERR_BADDRIVE, tries, mode, copied;

    for ( tries = 0; tries < 3; tries++ ) {
        if ( find_mount( letter, &rep ) < 0 ) {
            err = ERR_BADDRIVE;
            break;
        }
        mode = UM_KEEP;
        gen = 0;
        if ( (rep.flags & IMF_MEM) && (rep.flags & IMF_CHANGED) ) {
            say( "Saving " );
            say_drive( letter );
            say( " to " );
            say( rep.path );
            eol();
            err = save_image( letter, &rep, &gen, &copied );
            if ( err && !copied ) {
                break;                      /* busy, or no memory to copy */
            }
            if ( err ) {
                say( "Cannot save " );
                say_drive( letter );
                say( " to " );
                say( rep.path );
                say( ": " );
                say_why( why_file( err ), err );
                eol();
                say( "Unmount " );
                say_drive( letter );
                if ( !ask_yes( " anyway and lose the changes (Y/N)? " ) ) {
                    say_drive( letter );
                    say( " is still mounted.\r\n" );
                    return 1;
                }
                mode = UM_DISCARD;
            } else {
                mode = UM_SAVED;
            }
        }
        err = sys_img_unmount( letter, mode, gen );
        if ( err != ERR_ACCESS || mode == UM_DISCARD ) {
            break;
        }
        /* Refused: a file on it is open, or it was written after the
           copy was taken - only the second is worth another go. */
        if ( find_mount( letter, &rep ) < 0 ||
             !((rep.flags & IMF_MEM) && (rep.flags & IMF_CHANGED)) ) {
            break;
        }
    }
    if ( err ) {
        return unmount_failed( letter, err );
    }
    say( "Unmounted " );
    say_drive( letter );
    eol();
    return 0;
}

/* ------------------------------------------------------------------ */
/* the command line                                                    */
/* ------------------------------------------------------------------ */

/*
 * A TOKEN AT A TIME, and which token it is decided by what it looks
 * like rather than by where it sits: "MOUNT E: X.IMG" and "MOUNT X.IMG
 * E:" mean the same thing, because a two-character word ending in a
 * colon can only be a drive.  Switches may come anywhere.  A path may
 * be in double quotes, for a long name with a space in it.
 */
static int parse( const char *line, ARGS *args )
{
    char word[PATH_MAX];
    unsigned int len;
    int quoted, ch;

    mem_set( args, 0, sizeof( *args ) );
    args->letter = -1;
    for ( ;; ) {
        while ( *line == ' ' || *line == '\t' ) {
            line++;
        }
        if ( *line == 0 || *line == '\r' ) {
            break;
        }
        /* "-U" as well as "/U", as the first MOUNT took them - but only
           a dash with one letter after it, so a name may start with one */
        if ( *line == '/' || (*line == '-' && line[1] != 0 &&
             (line[2] == 0 || line[2] == ' ' || line[2] == '\t' || line[2] == '\r')) ) {
            ch = ch_upper( line[1] );
            if ( ch == 'U' ) {
                args->umount = 1;
            } else if ( ch == 'R' ) {
                args->ro = 1;
            } else if ( ch == '?' ) {
                args->help = 1;
            } else {
                return 0;
            }
            line += 2;
            continue;
        }
        len = 0;
        quoted = 0;
        while ( *line && *line != '\r' && (quoted || (*line != ' ' && *line != '\t')) ) {
            if ( *line == '"' ) {
                quoted = !quoted;
                line++;
                continue;
            }
            if ( len + 1 >= sizeof( word ) ) {
                return 0;
            }
            word[len++] = *line++;
        }
        word[len] = 0;
        if ( len == 2 && word[1] == ':' ) {
            ch = ch_upper( word[0] );
            if ( ch < 'A' || ch > 'Z' || args->letter >= 0 ) {
                return 0;
            }
            args->letter = ch - 'A';
            continue;
        }
        if ( len == 0 || args->have_path ) {
            return 0;                       /* two files is not a question */
        }
        str_cpyn( args->path, word, sizeof( args->path ) );
        args->have_path = 1;
    }
    /* /U and a file together are two different commands at once */
    return !(args->umount && args->have_path);
}

void mount_main( void )
{
    ARGS args;

    if ( sys_init() ) {
        sys_exit( 1 );
    }
    thou = sys_thousands();
    if ( !parse( sys_cmdline(), &args ) ) {
        usage();
        sys_exit( 1 );
    }
    if ( args.help ) {
        usage();
        sys_exit( 0 );
    }
    if ( args.umount ) {
        if ( args.letter < 0 ) {
            usage();
            sys_exit( 1 );
        }
        sys_exit( unmount_drive( args.letter ) );
    }
    if ( !args.have_path ) {
        list();
        sys_exit( 0 );
    }
    sys_exit( mount_file( args.letter, args.path, args.ro ) );
}
