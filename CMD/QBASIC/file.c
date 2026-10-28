/*
 * FILE.C - OPEN and everything done with what it opens, the printer, the
 * directory statements, SHELL, the environment, CHAIN and RUN.
 *
 * A SEQUENTIAL FILE HAS A BUFFER OF ITS OWN, so that INPUT # and PRINT #
 * cost a DOS call per few thousand bytes rather than per character.  As
 * QBasic's: Ctrl+Z ends a file being read, a bare CR ends a LINE INPUT
 * line (a bare LF does not), LOC counts 128-byte blocks, and a number
 * read by INPUT # ends at the first character that cannot go on with it
 * and takes the rest of its item with it.
 *
 * A RANDOM FILE HAS A RECORD BUFFER, which FIELD names parts of and GET
 * and PUT move; a variable-length string PUT to one takes two bytes for
 * its length first.  A BINARY file is bytes at a position.
 *
 * Devices are opened by name - SCRN:, KYBD:, CONS:, LPTn:, COMn: - and
 * go to the screen, the keyboard, or the DOS device of that name.
 */
#include "qb.h"

#define MAX_FILES   255
#define SEQBUF      4096

enum { FM_NONE, FM_INPUT, FM_OUTPUT, FM_APPEND, FM_RANDOM, FM_BINARY };
enum { DV_FILE, DV_SCRN, DV_KYBD, DV_LPT, DV_COM, DV_CONS };

typedef struct {
    STR **var;
    u32   off, width;
} FIELDVAR;

typedef struct {
    int    mode;
    int    dev;
    u32    handle;
    u32    reclen;
    u8    *buf;             /* sequential: what is read ahead, or waiting to go */
    u32    buf_len;         /* bytes in it */
    u32    buf_pos;         /* reading: the next one to take */
    u32    file_pos;        /* the file's position of buf[0] (reading) or of the end (writing) */
    int    at_eof;          /* reading: the end, or Ctrl+Z, has been reached */
    int    past_end;        /* BINARY: a read went past the end */
    int    col;
    int    width;
    u8    *record;
    u32    recno;           /* the last record moved */
    u32    bpos;            /* BINARY/RANDOM: the next byte or record, 1-based */
    FIELDVAR *fields;
    int    nfields;
    char   name[PATH_MAX];
} QFILE;

static QFILE *files[MAX_FILES + 1];
static u32    lpt_handle;
static int    lpt_open, lpt_column = 1, lpt_wid = 80;

/* a DOS error as QBasic's */
static int dos_to_qb( int rc )
{
    switch ( rc ) {
    case ERR_NOFILE:    return E_FILE_NOT_FOUND;
    case ERR_NOPATH:    return E_PATH_NOT_FOUND;
    case ERR_NOHANDLES: return E_TOO_MANY_FILES;
    case ERR_ACCESS:    return E_PATH_FILE;
    case ERR_BADDRIVE:  return E_PATH_NOT_FOUND;
    case ERR_CURDIR:    return E_PATH_FILE;
    case ERR_NOTSAME:   return E_RENAME_DISKS;
    case ERR_WRPROT:    return E_PERMISSION;
    case ERR_NOTREADY:  return E_DISK_NOT_READY;
    case ERR_SHARE:     return E_PERMISSION;
    case ERR_EXISTS:    return E_FILE_EXISTS;
    case ERR_DISKFULL:  return E_DISK_FULL;
    case ERR_NOMEM:     return E_OUT_OF_MEMORY;
    }
    return E_DEV_IO;
}

static void dos_fail( int rc )
{
    vm_error( dos_to_qb( rc ) );
}

/* a string as a C path: cut at 259, the error if it is empty */
static void path_of( STR *s, char *path )
{
    u32 len = QS_LEN( s );

    if ( len == 0 ) {
        qs_release( s );
        vm_error( E_BAD_FILENAME );
    }
    if ( len >= PATH_MAX ) {
        len = PATH_MAX - 1;
    }
    mem_cpy( path, QS_TEXT( s ), len );
    path[len] = 0;
    qs_release( s );
}

/* OPEN's file name: an empty one is error 52, not 64 */
static void open_path( STR *s, char *path )
{
    if ( QS_LEN( s ) == 0 ) {
        qs_release( s );
        vm_error( E_BAD_FILENUM );
    }
    path_of( s, path );
}

static QFILE *get_file( int num )
{
    if ( num < 1 || num > MAX_FILES ) {
        vm_error( E_BAD_FILENUM );
    }
    if ( files[num] == NULL ) {
        vm_error( E_BAD_FILENUM );
    }
    return files[num];
}

/* ------------------------------------------------------------------ */
/* buffers                                                             */
/* ------------------------------------------------------------------ */

/* the buffered output written: -> 0, or DOS's error (raised by the caller:
   outside a statement, after a run, there is nothing to raise it in) */
static int flush_out( QFILE *f )
{
    u32 done;
    int rc;

    if ( f->buf_len == 0 || f->dev == DV_SCRN || f->dev == DV_CONS ) {
        f->buf_len = 0;
        return 0;
    }
    rc = sys_write( f->handle, f->buf, f->buf_len, &done );
    f->file_pos += done;
    f->buf_len = 0;
    return rc;
}

/* a DOS error from outside file.c's statements (CLEAR's closing) */
void file_raise( int rc )
{
    if ( rc != 0 ) {
        dos_fail( rc );
    }
}

static void flush_or_fail( QFILE *f )
{
    int rc = flush_out( f );

    if ( rc != 0 ) {
        dos_fail( rc );
    }
}

/* the next byte of a file being read, or -1 at its end (Ctrl+Z is one) */
static int read_byte( QFILE *f, int peek )
{
    u32 done;
    int rc, ch;

    if ( f->dev == DV_KYBD ) {
        ch = con_key( 1 ) & 0xFF;
        return ch;
    }
    if ( f->at_eof ) {
        return -1;
    }
    if ( f->buf_pos >= f->buf_len ) {
        f->file_pos += f->buf_len;
        f->buf_pos = f->buf_len = 0;
        rc = sys_read( f->handle, f->buf, SEQBUF, &done );
        if ( rc != 0 ) {
            dos_fail( rc );
        }
        f->buf_len = done;
        if ( done == 0 ) {
            f->at_eof = 1;
            return -1;
        }
    }
    ch = f->buf[f->buf_pos];
    if ( ch == 26 ) {
        f->at_eof = 1;
        return -1;
    }
    if ( !peek ) {
        f->buf_pos++;
    }
    return ch;
}

/* the position the program sees, 1-based */
static u32 seq_position( QFILE *f )
{
    if ( f->mode == FM_INPUT ) {
        return f->file_pos + f->buf_pos;
    }
    return f->file_pos + f->buf_len;
}

/* ------------------------------------------------------------------ */
/* opening and closing                                                 */
/* ------------------------------------------------------------------ */

static int device_of( const char *path )
{
    char upper[8];
    int i, len = (int)str_len( path );

    if ( len < 4 || len > 6 ) {
        if ( len > 5 && str_nicmp( path, "COM", 3 ) == 0 && path[4] == ':' ) {
            return DV_COM;
        }
        return DV_FILE;
    }
    for ( i = 0; i < len && i < 7; i++ ) {
        upper[i] = (char)ch_upper( path[i] );
    }
    upper[i] = 0;
    if ( str_cmp( upper, "SCRN:" ) == 0 ) {
        return DV_SCRN;
    }
    if ( str_cmp( upper, "KYBD:" ) == 0 ) {
        return DV_KYBD;
    }
    if ( str_cmp( upper, "CONS:" ) == 0 ) {
        return DV_CONS;
    }
    if ( len == 5 && str_nicmp( upper, "LPT", 3 ) == 0 && upper[3] >= '1' && upper[3] <= '3' && upper[4] == ':' ) {
        return DV_LPT;
    }
    if ( len == 5 && str_nicmp( upper, "COM", 3 ) == 0 && upper[3] >= '1' && upper[3] <= '4' && upper[4] == ':' ) {
        return DV_COM;
    }
    return DV_FILE;
}

static int already_open_for_write( const char *path, int except )
{
    char full[PATH_MAX], other[PATH_MAX];
    int i;

    if ( sys_truename( path, full, 0 ) != 0 ) {
        str_cpyn( full, path, sizeof( full ) );
    }
    for ( i = 1; i <= MAX_FILES; i++ ) {
        QFILE *f = files[i];
        if ( f == NULL || i == except || f->dev != DV_FILE ) {
            continue;
        }
        if ( sys_truename( f->name, other, 0 ) != 0 ) {
            str_cpyn( other, f->name, sizeof( other ) );
        }
        if ( str_icmp( full, other ) == 0 ) {
            return f->mode != FM_INPUT ? 2 : 1;
        }
    }
    return 0;
}

static void do_open( const char *path, int mode, int num, s32 reclen )
{
    QFILE *f;
    int dev = device_of( path ), rc, other;
    u32 handle = 0, size;
    char devname[8];

    /* the name is looked at before the number */
    if ( dev == DV_FILE && (str_chr( path, '?' ) != NULL || str_chr( path, '*' ) != NULL) ) {
        vm_error( E_BAD_FILENAME );         /* no wildcards in a name to open */
    }
    if ( num < 1 || num > MAX_FILES ) {
        vm_error( E_BAD_FILENUM );
    }
    if ( files[num] != NULL ) {
        vm_error( E_FILE_OPEN );
    }
    if ( reclen == 0 || reclen < -1 || reclen > 32767 ) {
        vm_error( E_BAD_RECLEN );
    }
    if ( dev == DV_KYBD && mode != FM_INPUT && mode != FM_RANDOM ) {
        vm_error( E_BAD_MODE );
    }
    if ( (dev == DV_SCRN || dev == DV_LPT) && mode == FM_INPUT ) {
        vm_error( E_BAD_MODE );
    }
    if ( dev == DV_FILE ) {
        /* QBasic: a file open already may be opened again for anything but
           OUTPUT or APPEND, whatever it was opened for first */
        other = already_open_for_write( path, num );
        if ( other && (mode == FM_OUTPUT || mode == FM_APPEND) ) {
            vm_error( E_FILE_OPEN );
        }
    }
    switch ( dev ) {
    case DV_FILE:
        switch ( mode ) {
        case FM_INPUT:
            rc = sys_open( path, OPEN_READ, &handle );
            break;
        case FM_OUTPUT:
            rc = sys_create( path, &handle );
            break;
        case FM_APPEND:
            rc = sys_open( path, OPEN_RW, &handle );
            if ( rc == ERR_NOFILE ) {
                rc = sys_create( path, &handle );
            }
            break;
        default:
            rc = sys_open( path, OPEN_RW, &handle );
            if ( rc == ERR_NOFILE ) {
                rc = sys_create( path, &handle );
            } else if ( rc == ERR_ACCESS ) {
                rc = sys_open( path, OPEN_READ, &handle );
            }
            break;
        }
        if ( rc != 0 ) {
            dos_fail( rc );
        }
        break;
    case DV_LPT:
    case DV_COM:
        mem_cpy( devname, path, 4 );
        devname[4] = 0;
        rc = sys_open_write( devname, &handle );
        if ( rc != 0 ) {
            vm_error( E_DEV_UNAVAIL );
        }
        break;
    }
    f = (QFILE *)try_alloc( sizeof( QFILE ) );
    if ( f == NULL ) {
        if ( dev == DV_FILE || dev == DV_LPT || dev == DV_COM ) {
            sys_close( handle );
        }
        vm_error( E_OUT_OF_MEMORY );
    }
    mem_set( f, 0, sizeof( *f ) );
    f->mode = mode;
    f->dev = dev;
    f->handle = handle;
    f->col = 1;
    f->width = dev == DV_LPT ? 80 : dev == DV_SCRN || dev == DV_CONS ? con_width() : 0;
    f->reclen = reclen > 0 ? (u32)reclen : 128;
    f->bpos = 1;
    str_cpyn( f->name, path, sizeof( f->name ) );
    if ( mode == FM_INPUT || mode == FM_OUTPUT || mode == FM_APPEND ) {
        f->buf = (u8 *)try_alloc( SEQBUF );
        if ( f->buf == NULL ) {
            if ( dev == DV_FILE || dev == DV_LPT || dev == DV_COM ) {
                sys_close( handle );            /* the handle goes with it */
            }
            xfree( f->buf );
            xfree( f );
            vm_error( E_OUT_OF_MEMORY );
        }
    }
    if ( mode == FM_RANDOM ) {
        f->record = (u8 *)try_alloc( f->reclen );
        if ( f->record == NULL ) {
            if ( dev == DV_FILE || dev == DV_LPT || dev == DV_COM ) {
                sys_close( handle );            /* the handle goes with it */
            }
            xfree( f->buf );
            xfree( f );
            vm_error( E_OUT_OF_MEMORY );
        }
        mem_set( f->record, 0, f->reclen );
    }
    if ( mode == FM_APPEND && dev == DV_FILE ) {
        /* to the end, less a Ctrl+Z if the file ends with one */
        sys_size_keep( handle, &size );
        if ( size > 0 ) {
            u8 last;
            u32 done;
            sys_seek( handle, size - 1 );
            if ( sys_read( handle, &last, 1, &done ) == 0 && done == 1 && last == 26 ) {
                size--;
            }
        }
        sys_seek( handle, size );
        f->file_pos = size;
    }
    files[num] = f;
}

/* OPEN file$ FOR mode ACCESS a lock AS #n LEN = r */
void rtf_OPEN( void )
{
    s32 reclen = POP_L(), num = POP_L();
    int lock = (int)POP_L(), access = (int)POP_L(), mode = (int)POP_L();
    char path[PATH_MAX];

    (void)lock;
    (void)access;
    open_path( POP_STR(), path );
    do_open( path, mode, (int)num, reclen );
}

/* OPEN "O", #n, file$ [, length] */
void rtf_OPENOLD( void )
{
    s32 reclen = POP_L();
    char path[PATH_MAX];
    STR *ms;
    s32 num;
    int mode;

    open_path( POP_STR(), path );
    num = POP_L();
    ms = POP_STR();
    switch ( QS_LEN( ms ) ? ch_upper( ms->text[0] ) : 0 ) {
    case 'I':   mode = FM_INPUT; break;
    case 'O':   mode = FM_OUTPUT; break;
    case 'A':   mode = FM_APPEND; break;
    case 'R':   mode = FM_RANDOM; break;
    case 'B':   mode = FM_BINARY; break;
    default:
        qs_release( ms );
        vm_error( E_BAD_MODE );
        return;
    }
    qs_release( ms );
    do_open( path, mode, (int)num, reclen );
}

/* -> 0, or the error writing the last of its output (it is closed anyway) */
static int close_file( int num )
{
    QFILE *f = files[num];
    int rc = 0;

    if ( f == NULL ) {
        return 0;
    }
    files[num] = NULL;
    if ( (f->mode == FM_OUTPUT || f->mode == FM_APPEND) && f->dev != DV_KYBD ) {
        rc = flush_out( f );
    }
    if ( f->dev == DV_FILE || f->dev == DV_LPT || f->dev == DV_COM ) {
        sys_close( f->handle );
    }
    xfree( f->buf );
    xfree( f->record );
    xfree( f->fields );
    xfree( f );
    return rc;
}

void rtf_CLOSE( void )
{
    s32 num = POP_L();
    int rc;

    if ( num < 1 || num > MAX_FILES ) {
        vm_error( E_BAD_FILENUM );
    }
    rc = close_file( (int)num );
    if ( rc != 0 ) {
        dos_fail( rc );
    }
}

void rtf_CLOSEALL( void )
{
    int rc = file_close_all();

    if ( rc != 0 ) {
        dos_fail( rc );
    }
}

/* -> 0, or the first error writing out (every file is closed anyway) */
int file_close_all( void )
{
    int i, rc, first = 0;

    for ( i = 1; i <= MAX_FILES; i++ ) {
        if ( files[i] != NULL ) {
            rc = close_file( i );
            if ( first == 0 ) {
                first = rc;
            }
        }
    }
    if ( lpt_open ) {
        sys_close( lpt_handle );
        lpt_open = 0;
    }
    return first;
}

void file_reset_state( void )
{
    file_close_all();
    lpt_column = 1;
    lpt_wid = 80;
}

/* ------------------------------------------------------------------ */
/* PRINT # and WRITE #                                                 */
/* ------------------------------------------------------------------ */

void file_check_output( int num )
{
    QFILE *f = get_file( num );

    if ( f->mode == FM_INPUT || f->dev == DV_KYBD || f->mode == FM_BINARY ) {
        vm_error( E_BAD_MODE );
    }
}

int file_col( int num )
{
    return get_file( num )->col;
}

int file_width( int num )
{
    return get_file( num )->width;
}

static void put_bytes( QFILE *f, const char *text, u32 len )
{
    u32 n, done;
    int rc;

    if ( f->dev == DV_SCRN || f->dev == DV_CONS ) {
        con_write( text, len );
        return;
    }
    if ( f->mode == FM_RANDOM || f->mode == FM_BINARY ) {
        /* PRINT # to a BINARY or RANDOM file: at the position, straight */
        rc = sys_write( f->handle, text, len, &done );
        if ( rc != 0 ) {
            dos_fail( rc );
        }
        return;
    }
    while ( len > 0 ) {
        n = SEQBUF - f->buf_len;
        if ( n > len ) {
            n = len;
        }
        mem_cpy( f->buf + f->buf_len, text, n );
        f->buf_len += n;
        text += n;
        len -= n;
        if ( f->buf_len == SEQBUF ) {
            flush_or_fail( f );
        }
    }
}

void file_text( int num, const char *text, u32 len )
{
    QFILE *f = get_file( num );
    u32 i;

    put_bytes( f, text, len );
    for ( i = 0; i < len; i++ ) {
        if ( text[i] == 13 || text[i] == 10 ) {
            f->col = 1;
        } else {
            f->col++;
        }
    }
}

void file_newline( int num )
{
    QFILE *f = get_file( num );

    if ( f->dev == DV_SCRN || f->dev == DV_CONS ) {
        con_newline();
    } else {
        put_bytes( f, "\r\n", 2 );
    }
    f->col = 1;
}

/* WIDTH #n, w */
void rtf_WIDTHF( void )
{
    s32 width = POP_L();
    QFILE *f = get_file( (int)POP_L() );

    if ( width < 0 || width > 255 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    f->width = width == 255 ? 0 : (int)width;
}

/* WIDTH "LPT1:", w and the like */
void rtf_WIDTHDEV( void )
{
    s32 width = POP_L();
    char path[PATH_MAX];

    path_of( POP_STR(), path );
    if ( width < 0 || width > 255 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    if ( device_of( path ) == DV_LPT ) {
        lpt_wid = width == 255 ? 0 : (int)width;
    } else if ( device_of( path ) == DV_SCRN ) {
        PUSH_L( width );
        PUSH_L( 0 );
        PUSH_L( 1 );
        rtf_WIDTH();
    }
}

/* the printer, for LPRINT */
static void lpt_need( void )
{
    if ( !lpt_open ) {
        if ( sys_open_write( "PRN", &lpt_handle ) != 0 ) {
            vm_error( E_DEV_UNAVAIL );
        }
        lpt_open = 1;
    }
}

int lpt_col( void )
{
    return lpt_column;
}

int lpt_width( void )
{
    return lpt_wid;
}

void lpt_text( const char *text, u32 len )
{
    u32 done, i;

    lpt_need();
    if ( sys_write( lpt_handle, text, len, &done ) != 0 ) {
        vm_error( E_DEV_FAULT );
    }
    for ( i = 0; i < len; i++ ) {
        lpt_column = (text[i] == 13 || text[i] == 10) ? 1 : lpt_column + 1;
    }
}

void lpt_newline( void )
{
    lpt_text( "\r\n", 2 );
    lpt_column = 1;
}

/* ------------------------------------------------------------------ */
/* INPUT # and LINE INPUT #                                            */
/* ------------------------------------------------------------------ */

static QFILE *input_file( int num )
{
    QFILE *f = get_file( num );

    if ( f->mode == FM_OUTPUT || f->mode == FM_APPEND ) {
        vm_error( E_INPUT_PAST_END );           /* QBasic says 62 here, not 54 */
    }
    if ( f->mode == FM_BINARY ) {
        vm_error( E_BAD_MODE );                /* INPUT # and LINE INPUT # are for text */
    }
    if ( f->dev == DV_SCRN || f->dev == DV_LPT ) {
        vm_error( E_BAD_MODE );
    }
    return f;
}

/* one item for INPUT #: a quoted string, or text up to a comma or the
   end of the line; a number ends at a blank too and takes the rest of
   its item with it */
int file_input_item( int num, char *buf, int max, int *quoted, int want_string )
{
    QFILE *f = input_file( num );
    int ch, len = 0;

    *quoted = 0;
    /* blanks and line feeds before it; an empty line is an empty item */
    for ( ;; ) {
        ch = read_byte( f, 1 );
        if ( ch < 0 ) {
            vm_error( E_INPUT_PAST_END );
        }
        if ( ch == ' ' || ch == '\t' || ch == 10 ) {
            read_byte( f, 0 );
            continue;
        }
        break;
    }
    if ( ch == '"' ) {
        read_byte( f, 0 );
        *quoted = 1;
        for ( ;; ) {
            ch = read_byte( f, 0 );
            if ( ch < 0 || ch == '"' ) {
                break;
            }
            if ( len < max ) {
                buf[len++] = (char)ch;
            }
        }
        /* past it, to the comma or the end of the line */
        for ( ;; ) {
            ch = read_byte( f, 0 );
            if ( ch < 0 || ch == ',' ) {
                break;
            }
            if ( ch == 13 ) {
                if ( read_byte( f, 1 ) == 10 ) {
                    read_byte( f, 0 );
                }
                break;
            }
        }
        return len;
    }
    for ( ;; ) {
        ch = read_byte( f, 0 );
        if ( ch < 0 || ch == ',' ) {
            break;
        }
        if ( ch == 13 ) {
            if ( read_byte( f, 1 ) == 10 ) {
                read_byte( f, 0 );
            }
            break;
        }
        if ( ch == 10 && !want_string ) {
            break;
        }
        if ( !want_string && (ch == ' ' || ch == '\t') ) {
            /* a number ends at a blank: "1 2 3" is three of them; a comma
               or a line end after the blanks belongs to it */
            for ( ;; ) {
                ch = read_byte( f, 1 );
                if ( ch == ' ' || ch == '\t' ) {
                    read_byte( f, 0 );
                    continue;
                }
                if ( ch == ',' ) {
                    read_byte( f, 0 );
                } else if ( ch == 13 ) {
                    read_byte( f, 0 );
                    if ( read_byte( f, 1 ) == 10 ) {
                        read_byte( f, 0 );
                    }
                } else if ( ch == 10 ) {
                    read_byte( f, 0 );
                }
                break;
            }
            break;
        }
        if ( len < max ) {
            buf[len++] = (char)ch;
        }
    }
    while ( len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t') ) {
        len--;
    }
    return len;
}

/* a line: up to a CR (and the LF after it); a bare LF stays in it */
int file_line_input( int num, char *buf, int max )
{
    QFILE *f = input_file( num );
    int ch, len = 0;

    if ( read_byte( f, 1 ) < 0 ) {
        vm_error( E_INPUT_PAST_END );
    }
    for ( ;; ) {
        ch = read_byte( f, 0 );
        if ( ch < 0 ) {
            break;
        }
        if ( ch == 13 ) {
            if ( read_byte( f, 1 ) == 10 ) {
                read_byte( f, 0 );
            }
            break;
        }
        if ( len < max ) {
            buf[len++] = (char)ch;
        }
    }
    return len;
}

/* INPUT$(n, #f) */
void rtf_INPUTSF( void )
{
    int num = (int)POP_L();
    s32 n = POP_L(), i;
    QFILE *f;
    STR *s;
    u32 done;
    int ch, rc;

    if ( n < 1 || n > STR_MAX ) {
        vm_error( E_ILLEGAL_CALL );
    }
    f = get_file( num );
    if ( f->mode == FM_OUTPUT || f->mode == FM_APPEND ) {
        vm_error( E_INPUT_PAST_END );
    }
    s = qs_new( (u32)n );
    if ( f->mode == FM_BINARY || f->mode == FM_RANDOM ) {
        rc = sys_read( f->handle, s->text, (u32)n, &done );
        if ( rc != 0 || done < (u32)n ) {
            qs_release( s );
            vm_error( rc ? dos_to_qb( rc ) : E_INPUT_PAST_END );
        }
        PUSH_STR( s );
        return;
    }
    for ( i = 0; i < n; i++ ) {
        if ( f->dev != DV_KYBD && f->buf_pos >= f->buf_len && f->at_eof ) {
            ch = -1;
        } else {
            ch = read_byte( f, 0 );
        }
        if ( ch < 0 ) {
            /* a Ctrl+Z is a byte here, if there is more */
            qs_release( s );
            vm_error( E_INPUT_PAST_END );
        }
        s->text[i] = (char)ch;
    }
    PUSH_STR( s );
}

/* ------------------------------------------------------------------ */
/* positions                                                           */
/* ------------------------------------------------------------------ */

void rtf_EOF( void )
{
    QFILE *f = get_file( (int)POP_L() );

    switch ( f->mode ) {
    case FM_INPUT:
        if ( f->dev == DV_KYBD ) {
            PUSH_L( 0 );
            return;
        }
        PUSH_L( read_byte( f, 1 ) < 0 ? -1 : 0 );
        return;
    case FM_BINARY:
        PUSH_L( f->past_end ? -1 : 0 );
        return;
    case FM_RANDOM:
        if ( f->dev != DV_FILE ) {
            PUSH_L( 0 );
            return;
        }
        PUSH_L( f->past_end ? -1 : 0 );        /* only once a GET has gone past the end */
        return;
    }
    PUSH_L( -1 );                               /* a file being written is at its end */
}

void rtf_LOF( void )
{
    QFILE *f = get_file( (int)POP_L() );
    u32 size = 0;

    if ( f->dev != DV_FILE ) {
        PUSH_L( 0 );
        return;
    }
    if ( f->mode == FM_OUTPUT || f->mode == FM_APPEND ) {
        flush_or_fail( f );
    }
    sys_size_keep( f->handle, &size );
    PUSH_L( (s32)size );
}

void rtf_LOC( void )
{
    QFILE *f = get_file( (int)POP_L() );
    u32 pos;

    switch ( f->mode ) {
    case FM_INPUT:
        PUSH_L( (s32)(seq_position( f ) / 128 + 1) );
        return;
    case FM_OUTPUT:
    case FM_APPEND:
        PUSH_L( (s32)(seq_position( f ) / 128) );
        return;
    case FM_RANDOM:
        PUSH_L( (s32)f->recno );
        return;
    }
    sys_tell( f->handle, &pos );
    PUSH_L( (s32)pos );
}

void rtf_SEEKF( void )
{
    QFILE *f = get_file( (int)POP_L() );
    u32 pos;

    switch ( f->mode ) {
    case FM_RANDOM:
        PUSH_L( (s32)f->bpos );
        return;
    case FM_BINARY:
        sys_tell( f->handle, &pos );
        PUSH_L( (s32)pos + 1 );
        return;
    }
    PUSH_L( (s32)seq_position( f ) + 1 );
}

void rtf_SEEKS( void )
{
    s32 pos = POP_L();
    QFILE *f = get_file( (int)POP_L() );

    if ( pos < 1 ) {
        vm_error( E_BAD_RECNUM );
    }
    if ( f->dev != DV_FILE ) {
        return;
    }
    switch ( f->mode ) {
    case FM_RANDOM:
        if ( (u32)pos - 1 > 0xFFFFFFFFUL / f->reclen ) {
            vm_error( E_BAD_RECNUM );                   /* past what a DOS file can hold */
        }
        f->bpos = (u32)pos;
        sys_seek( f->handle, ((u32)pos - 1) * f->reclen );
        return;
    case FM_BINARY:
        f->past_end = 0;
        sys_seek( f->handle, (u32)pos - 1 );
        return;
    case FM_INPUT:
        f->buf_len = f->buf_pos = 0;
        f->at_eof = 0;
        f->file_pos = (u32)pos - 1;
        sys_seek( f->handle, (u32)pos - 1 );
        return;
    }
    flush_or_fail( f );
    f->file_pos = (u32)pos - 1;
    sys_seek( f->handle, (u32)pos - 1 );
}

void rtf_FREEFILE( void )
{
    int i;

    for ( i = 1; i <= MAX_FILES; i++ ) {
        if ( files[i] == NULL ) {
            PUSH_L( i );
            return;
        }
    }
    vm_error( E_TOO_MANY_FILES );
}

void rtf_FILEATTR( void )
{
    s32 which = POP_L();
    QFILE *f = get_file( (int)POP_L() );
    static const int codes[] = { 0, 1, 2, 8, 4, 32 };

    if ( which == 1 ) {
        PUSH_L( codes[f->mode] );
    } else if ( which == 2 ) {
        PUSH_L( (s32)f->handle );
    } else {
        vm_error( E_ILLEGAL_CALL );
    }
}

/* ------------------------------------------------------------------ */
/* GET, PUT, FIELD                                                     */
/* ------------------------------------------------------------------ */

/* the record buffer into the FIELD's variables */
/* The FIELD variables are copies of the record buffer, kept in step: made
   again from it after a GET, a FIELD, or a change to another of them.
   "except" is one not to make again (-1 none). */
static void fields_from_record( QFILE *f, int except )
{
    int i;

    for ( i = 0; i < f->nfields; i++ ) {
        STR *old;
        if ( i == except || f->fields[i].var == NULL ) {
            continue;
        }
        old = *f->fields[i].var;
        *f->fields[i].var = qs_make( (const char *)f->record + f->fields[i].off, f->fields[i].width );
        qs_release( old );
    }
}

/* a variable's memory is gone (a procedure returned, an array or CLEAR
   freed it): the FIELDs that named a variable there forget it */
static int any_fields;                  /* a FIELD has been made: forgetting has work */

void file_forget_vars( const u8 *lo, const u8 *hi )
{
    int num, i;

    if ( !any_fields ) {
        return;                                 /* every procedure return comes here */
    }
    for ( num = 1; num <= MAX_FILES; num++ ) {
        if ( files[num] == NULL ) {
            continue;
        }
        for ( i = 0; i < files[num]->nfields; i++ ) {
            const u8 *at = (const u8 *)files[num]->fields[i].var;
            if ( at >= lo && at < hi ) {
                files[num]->fields[i].var = NULL;
            }
        }
    }
}

/* LSET or RSET into a variable: if a FIELD names it, into the buffer */
void file_field_changed( STR **var )
{
    int num, i;
    QFILE *f;

    for ( num = 1; num <= MAX_FILES; num++ ) {
        f = files[num];
        if ( f == NULL ) {
            continue;
        }
        for ( i = 0; i < f->nfields; i++ ) {
            if ( f->fields[i].var == var ) {
                u32 n = QS_LEN( *var ) < f->fields[i].width ? QS_LEN( *var ) : f->fields[i].width;
                mem_cpy( f->record + f->fields[i].off, QS_TEXT( *var ), n );
                fields_from_record( f, i );     /* fields over the same bytes see it */
                return;
            }
        }
    }
}

/* FIELD #f, (width, variable)..., count */
void rtf_FIELD( void )
{
    int count = (int)POP_L(), i;
    FIELDVAR *list;
    QFILE *f;
    u32 off = 0;

    list = (FIELDVAR *)try_alloc( (u32)(count ? count : 1) * sizeof( FIELDVAR ) );
    if ( list == NULL ) {
        vm_error( E_OUT_OF_MEMORY );
    }
    for ( i = count - 1; i >= 0; i-- ) {
        list[i].var = (STR **)POP_A();
        list[i].width = (u32)POP_L();
    }
    f = get_file( (int)POP_L() );
    if ( f->mode != FM_RANDOM ) {
        xfree( list );
        vm_error( E_BAD_MODE );
    }
    for ( i = 0; i < count; i++ ) {
        list[i].off = off;
        off += list[i].width;
        if ( (s32)list[i].width < 0 || off > f->reclen ) {
            xfree( list );
            vm_error( E_FIELD_OVERFLOW );
        }
    }
    /* a second FIELD adds to the first (QBasic): the earlier variables
       stay, but for any this one places again */
    {
        FIELDVAR *all = (FIELDVAR *)try_alloc( (u32)(f->nfields + count + 1) * sizeof( FIELDVAR ) );
        int n = 0, j;
        if ( all == NULL ) {
            xfree( list );
            vm_error( E_OUT_OF_MEMORY );
        }
        for ( i = 0; i < f->nfields; i++ ) {
            for ( j = 0; j < count && list[j].var != f->fields[i].var; j++ ) {
            }
            if ( j == count && f->fields[i].var != NULL ) {
                all[n++] = f->fields[i];
            }
        }
        for ( i = 0; i < count; i++ ) {
            all[n++] = list[i];
        }
        xfree( list );
        xfree( f->fields );
        f->fields = all;
        f->nfields = n;
    }
    any_fields = 1;
    fields_from_record( f, -1 );
}

/* the bytes of a variable of a type, for GET and PUT: a STRING's are its
   text, any other's are its memory */
static void get_put( int is_put )
{
    int type = (int)POP_L();
    u32 width = (u32)POP_L(), done, size;
    void *var = POP_A();
    s32 rec = POP_L();
    QFILE *f = get_file( (int)POP_L() );
    int rc;

    if ( f->mode != FM_RANDOM && f->mode != FM_BINARY ) {
        vm_error( E_BAD_MODE );
    }
    if ( f->mode == FM_RANDOM ) {
        u32 recno = rec != NO_RECORD ? (u32)rec : f->bpos;
        if ( rec != NO_RECORD && rec < 1 ) {
            vm_error( E_BAD_RECNUM );
        }
        if ( var != NULL ) {
            /* a variable instead of the FIELD buffer: it must fit */
            if ( type == TY_STR ) {
                STR *s = *(STR **)var;
                if ( f->reclen < 2 || (is_put && QS_LEN( s ) + 2 > f->reclen) ) {
                    vm_error( E_BAD_RECLEN );           /* no room for even its length */
                }
            } else if ( width > f->reclen ) {
                vm_error( E_BAD_RECLEN );
            }
        }
        if ( recno - 1 > 0xFFFFFFFFUL / f->reclen ) {
            vm_error( E_BAD_RECNUM );               /* past what a DOS file can hold */
        }
        if ( f->dev == DV_FILE ) {
            sys_seek( f->handle, (recno - 1) * f->reclen );
        }
        if ( is_put ) {
            /* a variable's own bytes only, as QBasic writes them: the rest
               of the record is left as it was */
            if ( var == NULL ) {
                rc = sys_write( f->handle, f->record, f->reclen, &done );
            } else if ( type == TY_STR ) {
                /* the length in two bytes, then the text */
                STR *s = *(STR **)var;
                u16 n = (u16)QS_LEN( s );
                rc = sys_write( f->handle, &n, 2, &done );
                if ( rc == 0 && n > 0 ) {
                    rc = sys_write( f->handle, QS_TEXT( s ), n, &done );
                }
            } else {
                rc = sys_write( f->handle, var, width, &done );
            }
            if ( rc != 0 ) {
                dos_fail( rc );
            }
        } else {
            mem_set( f->record, 0, f->reclen );
            rc = sys_read( f->handle, f->record, f->reclen, &done );
            if ( rc != 0 ) {
                dos_fail( rc );
            }
            f->past_end = done < f->reclen;
            if ( var != NULL ) {
                if ( type == TY_STR ) {
                    u16 n;
                    STR *old = *(STR **)var;
                    mem_cpy( &n, f->record, 2 );
                    if ( n > f->reclen - 2 ) {
                        n = (u16)(f->reclen - 2);
                    }
                    *(STR **)var = qs_make( (const char *)f->record + 2, n );
                    qs_release( old );
                } else {
                    mem_cpy( var, f->record, width );
                }
            } else {
                fields_from_record( f, -1 );
            }
        }
        f->recno = recno;
        f->bpos = recno + 1;
        return;
    }
    /* BINARY */
    if ( rec != NO_RECORD && rec < 1 ) {
        vm_error( E_BAD_RECNUM );
    }
    if ( rec != NO_RECORD ) {
        sys_seek( f->handle, (u32)rec - 1 );
    }
    if ( var == NULL ) {
        vm_error( E_BAD_RECLEN );
    }
    if ( type == TY_STR ) {
        STR **slot = (STR **)var;
        size = QS_LEN( *slot );
        if ( is_put ) {
            rc = sys_write( f->handle, QS_TEXT( *slot ), size, &done );
        } else {
            STR *s = qs_unique( *slot );
            *slot = s;
            rc = size ? sys_read( f->handle, s->text, size, &done ) : 0;
            if ( rc == 0 && size && done < size ) {
                mem_set( s->text + done, 0, size - done );
                f->past_end = 1;
            }
        }
    } else if ( is_put ) {
        rc = sys_write( f->handle, var, width, &done );
    } else {
        rc = sys_read( f->handle, var, width, &done );
        if ( rc == 0 && done < width ) {
            mem_set( (u8 *)var + done, 0, width - done );
            f->past_end = 1;
        }
    }
    if ( rc != 0 ) {
        dos_fail( rc );
    }
}

void rtf_FGET( void )
{
    get_put( 0 );
}

void rtf_FPUT( void )
{
    get_put( 1 );
}

/* LOCK and UNLOCK: one program, nothing to share it with */
void rtf_LOCK( void )
{
    (void)POP_L();
    (void)POP_L();
    (void)POP_L();
    get_file( (int)POP_L() );
}

/* ------------------------------------------------------------------ */
/* the directory                                                       */
/* ------------------------------------------------------------------ */

static int is_open_file( const char *path )
{
    return already_open_for_write( path, 0 ) != 0;
}

void rtf_KILL( void )
{
    char pattern[PATH_MAX], path[PATH_MAX], *slash;
    FINDREC rec;
    u32 handle;
    int rc, any = 0, len;

    path_of( POP_STR(), pattern );
    rc = sys_find_first( pattern, ATTR_ARCHIVE | ATTR_READONLY, &rec, &handle );
    if ( rc != 0 ) {
        dos_fail( rc == ERR_NOMORE ? ERR_NOFILE : rc );
    }
    /* the directory part of the pattern, for each name found */
    str_cpy( path, pattern );
    slash = str_rchr( path, '\\' );
    if ( slash == NULL ) {
        slash = str_rchr( path, ':' );
    }
    len = slash ? (int)(slash - path) + 1 : 0;
    do {
        if ( rec.attr & ATTR_DIR ) {
            continue;
        }
        path[len] = 0;
        if ( !str_catn( path, rec.alias[0] ? rec.alias : rec.name, sizeof( path ) ) ) {
            sys_find_close( handle );
            vm_error( E_BAD_FILENAME );
        }
        if ( is_open_file( path ) ) {
            sys_find_close( handle );
            vm_error( E_FILE_OPEN );
        }
        rc = sys_delete( path );
        if ( rc != 0 ) {
            sys_find_close( handle );
            dos_fail( rc );
        }
        any = 1;
    } while ( sys_find_next( handle, &rec ) == 0 );
    sys_find_close( handle );
    if ( !any ) {
        vm_error( E_FILE_NOT_FOUND );
    }
}

void rtf_NAME( void )
{
    char to[PATH_MAX], from[PATH_MAX];
    u16 attr;
    int rc;

    path_of( POP_STR(), to );
    path_of( POP_STR(), from );
    if ( is_open_file( from ) ) {
        vm_error( E_FILE_OPEN );
    }
    if ( sys_get_attr( to, &attr ) == 0 ) {
        vm_error( E_FILE_EXISTS );
    }
    rc = sys_rename( from, to );
    if ( rc != 0 ) {
        dos_fail( rc );
    }
}

static void dir_op( int (*op)( const char * ) )
{
    char path[PATH_MAX];
    int rc;

    path_of( POP_STR(), path );
    rc = op( path );
    if ( rc != 0 ) {
        vm_error( rc == ERR_NOPATH || rc == ERR_NOFILE ? E_PATH_NOT_FOUND : E_PATH_FILE );
    }
}

void rtf_MKDIR( void )
{
    dir_op( sys_mkdir );
}

void rtf_RMDIR( void )
{
    dir_op( sys_rmdir );
}

void rtf_CHDIR( void )
{
    dir_op( sys_chdir );
}

/* FILES [pattern$]: the directory, then the names, then the bytes free */
void rtf_FILES( void )
{
    int given = (int)POP_L(), rc, drive;
    STR *s = POP_STR();
    char pattern[PATH_MAX], cwd[PATH_MAX + 3], entry[24], num[16];   /* "X:\" and the 7147h path */
    FINDREC rec;
    u32 handle, freeb;
    int i, dot;

    if ( given ) {
        path_of( s, pattern );
    } else {
        qs_release( s );
        str_cpy( pattern, "*.*" );
    }
    drive = sys_get_drive();
    if ( pattern[0] && pattern[1] == ':' ) {
        drive = ch_upper( pattern[0] ) - 'A';
    }
    cwd[0] = (char)('A' + drive);
    cwd[1] = ':';
    cwd[2] = '\\';
    sys_get_cwd( drive, cwd + 3 );
    con_write( cwd, str_len( cwd ) );
    con_newline();
    rc = sys_find_first( pattern, ATTR_DIR | ATTR_ARCHIVE | ATTR_READONLY, &rec, &handle );
    if ( rc != 0 ) {
        vm_error( E_FILE_NOT_FOUND );
    }
    do {
        const char *name = rec.alias[0] ? rec.alias : rec.name;
        mem_set( entry, ' ', 18 );
        entry[18] = 0;
        dot = -1;
        for ( i = 0; name[i]; i++ ) {
            if ( name[i] == '.' && i > 0 ) {
                dot = i;
            }
        }
        if ( name[0] == '.' ) {
            dot = -1;
        }
        for ( i = 0; name[i] && (dot < 0 || i < dot) && i < 8; i++ ) {
            entry[i] = name[i];
        }
        if ( dot >= 0 ) {
            entry[8] = '.';
            for ( i = 0; name[dot + 1 + i] && i < 3; i++ ) {
                entry[9 + i] = name[dot + 1 + i];
            }
        }
        if ( rec.attr & ATTR_DIR ) {
            mem_cpy( entry + 12, "<DIR>", 5 );
        }
        if ( con_col() + 18 > con_width() + 1 ) {
            con_newline();
        }
        con_write( entry, 18 );
    } while ( sys_find_next( handle, &rec ) == 0 );
    sys_find_close( handle );
    if ( con_col() != 1 ) {
        con_newline();
    }
    if ( sys_disk_free( drive, &freeb ) != 0 ) {
        freeb = 0;
    }
    num[0] = ' ';
    num_fmt_l( (s32)(freeb > 0x7FFFFFFFUL ? 0x7FFFFFFFUL : freeb), num + 1 );
    con_write( num, str_len( num ) );
    con_write( " Bytes free", 11 );
    con_newline();
}

/* ------------------------------------------------------------------ */
/* the environment                                                     */
/* ------------------------------------------------------------------ */

#define MAX_ENV     256

static STR *env[MAX_ENV];
static int  nenv = -1;

static void env_load( void )
{
    const char *block = sys_environment();
    u32 len;

    if ( nenv >= 0 ) {
        return;
    }
    nenv = 0;
    if ( block == NULL ) {
        return;
    }
    while ( *block && nenv < MAX_ENV ) {
        len = str_len( block );
        env[nenv++] = qs_make( block, len );
        block += len + 1;
    }
}

/* the entry for "name", by its length up to the '=' */
static int env_find( const char *name, u32 len )
{
    int i;

    for ( i = 0; i < nenv; i++ ) {
        if ( QS_LEN( env[i] ) > len && env[i]->text[len] == '=' && mem_cmp( env[i]->text, name, len ) == 0 ) {
            return i;
        }
    }
    return -1;
}

static int has_byte( const char *text, u32 len, int ch )
{
    u32 i;

    for ( i = 0; i < len; i++ ) {
        if ( text[i] == (char)ch ) {
            return 1;
        }
    }
    return 0;
}

void rtf_ENVIRONS( void )
{
    STR *s = POP_STR();
    u32 len = QS_LEN( s );
    int i;

    env_load();
    i = len ? env_find( s->text, len ) : -1;
    /* a name with a blank or an = in it cannot be one */
    if ( len > 0 && (has_byte( s->text, len, ' ' ) || has_byte( s->text, len, '=' )) ) {
        len = 0;
    }
    qs_release( s );
    if ( len == 0 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    if ( i < 0 ) {
        PUSH_STR( NULL );
        return;
    }
    PUSH_STR( qs_make( env[i]->text + len + 1, env[i]->len - len - 1 ) );
}

void rtf_ENVIRONN( void )
{
    s32 n = POP_L();

    env_load();
    if ( n < 1 || n > 255 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    PUSH_STR( n <= nenv ? qs_retain( env[n - 1] ) : NULL );
}

/* ENVIRON "NAME=value" (or "NAME value"): set, replaced, or with nothing
   after the = taken out */
void rtf_ENVSET( void )
{
    STR *s = POP_STR();
    u32 len = QS_LEN( s ), eq, name_len;
    int i;
    char *text;

    env_load();
    text = (char *)QS_TEXT( s );
    for ( eq = 0; eq < len && text[eq] != '=' && text[eq] != ' '; eq++ ) {
    }
    if ( eq == 0 || eq == len ) {
        qs_release( s );
        vm_error( E_ILLEGAL_CALL );
    }
    name_len = eq;
    while ( eq < len && text[eq] == ' ' ) {
        eq++;
    }
    if ( eq < len && text[eq] == '=' ) {
        eq++;
    }
    i = env_find( text, name_len );
    if ( eq >= len ) {
        if ( i >= 0 ) {
            qs_release( env[i] );
            mem_cpy( &env[i], &env[i + 1], (u32)(nenv - i - 1) * sizeof( STR * ) );
            nenv--;
        }
        qs_release( s );
        return;
    }
    {
        STR *entry = qs_new( name_len + 1 + (len - eq) );
        mem_cpy( entry->text, text, name_len );
        entry->text[name_len] = '=';
        mem_cpy( entry->text + name_len + 1, text + eq, len - eq );
        qs_release( s );
        if ( i >= 0 ) {
            qs_release( env[i] );
            env[i] = entry;
        } else if ( nenv < MAX_ENV ) {
            env[nenv++] = entry;
        } else {
            qs_release( entry );
            vm_error( E_OUT_OF_MEMORY );
        }
    }
}

/* SHELL [command$]: COMSPEC (or the kernel's interpreter) /C it, or on its own */
void rtf_SHELL( void )
{
    int given = (int)POP_L(), rc, i, at;
    STR *s = POP_STR();
    const char *comspec = sys_getenv( "COMSPEC" );
    char tail[260], shell[PATH_MAX];
    char *block = NULL;
    u32 size = 1;
    void con_after_shell( void );

    /* the child gets the environment as ENVIRON left it, and its COMSPEC */
    if ( nenv >= 0 ) {
        for ( i = 0; i < nenv; i++ ) {
            size += QS_LEN( env[i] ) + 1;
        }
        block = (char *)try_alloc( size );
        if ( block == NULL ) {
            qs_release( s );
            vm_error( E_OUT_OF_MEMORY );
        }
        size = 0;
        for ( i = 0; i < nenv; i++ ) {
            mem_cpy( block + size, QS_TEXT( env[i] ), QS_LEN( env[i] ) );
            size += QS_LEN( env[i] );
            block[size++] = 0;
        }
        block[size] = 0;
        at = env_find( "COMSPEC", 7 );
        comspec = NULL;
        if ( at >= 0 && QS_LEN( env[at] ) - 8 < sizeof( shell ) ) {
            mem_cpy( shell, env[at]->text + 8, QS_LEN( env[at] ) - 8 );
            shell[QS_LEN( env[at] ) - 8] = 0;
            comspec = shell;
        }
    }
    if ( comspec == NULL ) {
        comspec = sys_shell();
    }
    tail[0] = 0;
    if ( given && QS_LEN( s ) ) {
        str_cpy( tail, "/C " );
        mem_cpy( tail + 3, QS_TEXT( s ), QS_LEN( s ) > 250 ? 250 : QS_LEN( s ) );
        tail[3 + (QS_LEN( s ) > 250 ? 250 : QS_LEN( s ))] = 0;
    }
    qs_release( s );
    file_flush_all();
    con_flush();
    rc = sys_exec( comspec, tail, block );
    xfree( block );
    con_after_shell();
    if ( rc != 0 ) {
        vm_error( rc == ERR_NOFILE ? E_FILE_NOT_FOUND : E_ILLEGAL_CALL );
    }
}

/* -> 0, or the first error writing out */
int file_flush_all( void )
{
    int i, rc, first = 0;

    for ( i = 1; i <= MAX_FILES; i++ ) {
        if ( files[i] != NULL && (files[i]->mode == FM_OUTPUT || files[i]->mode == FM_APPEND) ) {
            rc = flush_out( files[i] );
            if ( first == 0 ) {
                first = rc;
            }
        }
    }
    return first;
}

/* ------------------------------------------------------------------ */
/* CHAIN, RUN, and the rest                                            */
/* ------------------------------------------------------------------ */

/* CHAIN's and RUN's file: .BAS when the name has none, and there to be
   read - a file that is not there is the statement's error */
static void chain_path( STR *s, char *path )
{
    const char *name = path;
    u32 at, handle;
    int rc;

    path_of( s, path );
    for ( at = 0; path[at] != 0; at++ ) {
        if ( path[at] == '\\' || path[at] == ':' ) {
            name = path + at + 1;
        }
    }
    if ( str_chr( name, '.' ) == NULL && at + 4 < PATH_MAX ) {
        str_cpy( path + at, ".BAS" );
    }
    rc = sys_open_read( path, &handle );
    if ( rc != 0 ) {
        dos_fail( rc );
    }
    sys_close( handle );
}

void rtf_CHAIN( void )
{
    char path[PATH_MAX];

    chain_path( POP_STR(), path );
    str_cpyn( vm.chain_to, path, sizeof( vm.chain_to ) );
    vm.chain_at = 0;
    vm.chain_keep = 1;                          /* the files and the COMMON go on */
    vm.stopped = RUN_CHAIN;
}

void rtf_RUNFILE( void )
{
    STR *s = POP_STR();

    if ( QS_LEN( s ) == 0 ) {
        vm.chain_to[0] = 0;                     /* RUN: this program, again */
        vm.chain_at = 0;
        vm.chain_keep = 0;
        qs_release( s );
    } else {
        chain_path( s, vm.chain_to );
        vm.chain_at = 0;
        vm.chain_keep = 0;
    }
    vm.stopped = RUN_CHAIN;
}

/* RUN n: this program again, started at the line's code */
void rtf_RUNLINE( void )
{
    vm.chain_at = (u32)POP_L() + 1;
    vm.chain_to[0] = 0;
    vm.chain_keep = 0;
    vm.stopped = RUN_CHAIN;
}

void rtf_IOCTL( void )
{
    qs_release( POP_STR() );
    get_file( (int)POP_L() );
    vm_error( E_ILLEGAL_CALL );
}

void rtf_IOCTLS( void )
{
    get_file( (int)POP_L() );
    vm_error( E_ILLEGAL_CALL );
}

void rtf_ERDEV( void )
{
    PUSH_L( 0 );
}

void rtf_ERDEVS( void )
{
    PUSH_STR( NULL );
}
