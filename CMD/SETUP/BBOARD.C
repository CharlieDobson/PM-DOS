/*
 * BBOARD.C - the billboards: a few lines about PM-DOS that Setup shows
 * above the gauge while it copies the files of each Setup disk, as
 * MS-DOS 6 Setup did.
 *
 * THE WORDS ARE IN SETUP.MSG, a text file on Setup disk 1 beside
 * SETUP.INF (DISK\SETUP.MSG in the tree; DISK\SETUP.LST carries it as
 * one of Setup's own files, unpacked).  Nothing is built from it: change
 * the file, on the disk or in the tree, and the next Setup shows the
 * change.
 *
 *   [disk1]    is shown while Setup copies the files of Setup disk 1,
 *   [disk2]    while it copies disk 2's, and so on to [disk9]
 *
 * A billboard is written as C writes a string: in double quotes, and
 * strings one after another are joined into one, so each line of the
 * file can hold a line of the screen.  Inside the quotes:
 *
 *   \n  \r\n       a new line; \r alone goes back to the start of the line
 *   \t             spaces to the next multiple of 8
 *   \\  \"  \'     a backslash, a double quote, a single quote
 *   \a \b \f \v    the control characters 07h, 08h, 0Ch, 0Bh
 *   \xHH           the character HH: one or two hex digits (\x07 is the
 *                  bullet Setup's own screens use)
 *   \ooo           the same in octal, one to three digits
 *
 * Any other character, the extended ones among them, is shown as it is
 * (code page 437; the screen is written cell by cell, so a control
 * character shows its glyph).  \0 and \x00 are shown as a space.  A
 * string still open when its line ends is closed there.  Outside the
 * quotes ';' starts a comment that runs to the end of the line, and
 * anything else is passed over.  A section named twice: the last one
 * counts.  A section with no text is no billboard.
 *
 * THE LAYOUT IS THE FILE'S.  Lines are not filled or broken again; only
 * one wider than BB_COLS is broken, at the last space that fits.  The
 * lines, as one block, are centered across the screen and between the
 * title's rule and "NN% complete" (rows 3 to 15); lines past the 13th
 * are not shown.  A disk with no billboard gets "Setup is copying the
 * PM-DOS files." as before, and so does every disk when the source has
 * no SETUP.MSG: a billboard is something to read while the gauge moves,
 * and nothing in one can stop Setup.
 */
#include "setup.h"

#define BB_TOP          3       /* the row below the title's rule        */
#define BB_ROWS         13      /* ...to the row above "NN% complete"    */
#define BB_LEFT         2
#define BB_COLS         76      /* the widest a line is shown            */
#define BB_LINE         160     /* a line as the text builds it          */
#define BB_FILE_MAX     16000u  /* the largest SETUP.MSG that is read    */

static char *board[MAX_DISKS + 1];      /* [disk]; NULL = none           */

/* ------------------------------------------------------------------ */
/* SETUP.MSG                                                           */
/* ------------------------------------------------------------------ */

static char    *text_buf;               /* the section being read        */
static uint16_t text_len, text_max;

static void text_add( int ch )
{
    if ( ch == 0 ) {
        ch = ' ';                       /* a NUL would end the string    */
    }
    if ( text_len < text_max ) {
        text_buf[text_len++] = (char)ch;
    }
}

/* What the section read so far holds becomes the disk's billboard */
static void text_keep( int disk )
{
    char *copy;

    if ( disk == 0 ) {
        return;
    }
    xfree( board[disk] );
    board[disk] = NULL;
    if ( text_len == 0 ) {
        return;
    }
    copy = (char *)plat_alloc( (uint32_t)text_len + 1 );
    if ( copy == NULL ) {
        return;
    }
    mem_cpy( copy, text_buf, text_len );
    copy[text_len] = 0;
    board[disk] = copy;
}

static int hex_digit( int ch )
{
    if ( ch >= '0' && ch <= '9' ) {
        return ch - '0';
    }
    ch = ch_upper( ch );
    if ( ch >= 'A' && ch <= 'F' ) {
        return ch - 'A' + 10;
    }
    return -1;
}

/* The character an escape stands for.  *at is just past the backslash
   and is not at the end of the line; it is moved past the escape. */
static int unescape( const char **at )
{
    const char *src = *at;
    int val, digit, n;

    switch ( *src ) {
    case 'n':   val = '\n'; src++; break;
    case 'r':   val = '\r'; src++; break;
    case 't':   val = '\t'; src++; break;
    case 'a':   val = 0x07; src++; break;
    case 'b':   val = 0x08; src++; break;
    case 'f':   val = 0x0C; src++; break;
    case 'v':   val = 0x0B; src++; break;
    case 'x':
        src++;
        val = 0;
        for ( n = 0; n < 2 && (digit = hex_digit( (uint8_t)*src )) >= 0; n++ ) {
            val = val * 16 + digit;
            src++;
        }
        if ( n == 0 ) {
            val = 'x';                  /* "\x" and no digits: the x     */
        }
        break;
    default:
        if ( *src >= '0' && *src <= '7' ) {
            val = 0;
            for ( n = 0; n < 3 && *src >= '0' && *src <= '7'; n++ ) {
                val = val * 8 + (*src - '0');
                src++;
            }
            val &= 0xFF;                /* \777 is one byte, as in C     */
        } else {
            val = (uint8_t)*src;        /* \\ \" \' \? and the rest      */
            src++;
        }
        break;
    }
    *at = src;
    return val;
}

/* The strings on one line of the file, added to the section */
static void read_line( const char *src )
{
    int ch;

    while ( *src ) {
        if ( *src == ';' ) {
            return;                     /* a comment, to the line's end  */
        }
        if ( *src++ != '"' ) {
            continue;
        }
        while ( *src && *src != '"' ) {
            if ( *src == '\\' && src[1] ) {
                src++;
                ch = unescape( &src );
            } else {
                ch = (uint8_t)*src++;
            }
            text_add( ch );
        }
        if ( *src == '"' ) {
            src++;
        }
    }
}

/* "[disk3]" -> 3; any other section -> 0.  The line is cut at the ']' */
static int section_disk( char *head )
{
    char *close = str_chr( head, ']' ), *name;
    const char *end;
    uint32_t num;
    int len;

    if ( close == NULL ) {
        return 0;
    }
    *close = 0;
    name = skip_blanks( head + 1 );
    len = str_len( name );
    while ( len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t') ) {
        name[--len] = 0;
    }
    if ( str_nicmp( name, "disk", 4 ) || name[4] < '0' || name[4] > '9' ) {
        return 0;
    }
    num = parse_u32( name + 4, &end );
    if ( *end || num < 1 || num > MAX_DISKS ) {
        return 0;
    }
    return (int)num;
}

/* SETUP.MSG from the Setup source, while disk 1 is where Setup can read
   it.  No file, or one Setup cannot read, is no billboards. */
void billboard_load( void )
{
    char path[PATHLEN], *raw = NULL, *line, *next, *end;
    uint16_t handle, done;
    uint32_t size;
    int disk = 0, len;

    path_join( path, srcdir, "SETUP.MSG", sizeof( path ) );
    if ( plat_open( path, 0, &handle ) ) {
        return;
    }
    text_buf = NULL;
    if ( plat_size( handle, &size ) == 0 && size > 0 && size <= BB_FILE_MAX ) {
        raw = (char *)plat_alloc( size + 1 );
        text_buf = (char *)plat_alloc( size );
    }
    if ( raw == NULL || text_buf == NULL ||
         plat_read( handle, raw, (uint16_t)size, &done ) || done != (uint16_t)size ) {
        plat_close( handle );
        xfree( raw );
        xfree( text_buf );
        text_buf = NULL;
        return;
    }
    plat_close( handle );
    raw[(uint16_t)size] = 0;
    text_max = (uint16_t)size;          /* a section decodes to no more  */
    text_len = 0;                       /* bytes than the file holds     */

    for ( line = raw; *line; line = next ) {
        end = line;
        while ( *end && *end != '\n' ) {
            end++;
        }
        next = *end ? end + 1 : end;
        *end = 0;
        len = str_len( line );
        while ( len > 0 && line[len - 1] == '\r' ) {
            line[--len] = 0;
        }
        line = skip_blanks( line );
        if ( *line == '[' ) {
            text_keep( disk );
            text_len = 0;
            disk = section_disk( line );
            continue;
        }
        if ( disk ) {
            read_line( line );
        }
    }
    text_keep( disk );
    xfree( raw );
    xfree( text_buf );
    text_buf = NULL;
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

static char row_text[BB_ROWS][BB_COLS + 1];
static int  row_len[BB_ROWS];
static int  nrows, width;

static int put_char( char *line, int col, int *used, int ch )
{
    if ( col < BB_LINE ) {
        line[col++] = (char)ch;
        if ( col > *used ) {
            *used = col;
        }
    }
    return col;
}

/* One line of the billboard, "used" characters, into the rows: broken
   at a space where it is wider than BB_COLS, or at BB_COLS if it has
   no space to break at */
static void add_rows( const char *line, int used )
{
    int start = 0, take, skip, len;

    do {
        take = used - start;
        skip = 0;
        if ( take > BB_COLS ) {
            for ( take = BB_COLS; take > 0 && line[start + take] != ' '; take-- ) {
            }
            if ( take == 0 ) {
                take = BB_COLS;
            } else {
                skip = 1;               /* the space the line broke at   */
            }
        }
        if ( nrows < BB_ROWS ) {
            len = take;
            while ( len > 0 && line[start + len - 1] == ' ' ) {
                len--;
            }
            mem_cpy( row_text[nrows], line + start, (uint16_t)len );
            row_text[nrows][len] = 0;
            row_len[nrows] = len;
            if ( len > width ) {
                width = len;
            }
            nrows++;
        }
        start += take + skip;
    } while ( start < used );
}

/* The disk's billboard in the rows above the gauge.  0 = drawn; 1 = it
   has none and the caller writes its own line. */
int billboard_draw( int disk )
{
    char line[BB_LINE];
    const char *src;
    int col = 0, used = 0, ch, row, top, left;

    if ( disk < 1 || disk > MAX_DISKS || board[disk] == NULL ) {
        return 1;
    }
    nrows = width = 0;
    src = board[disk];
    for ( ;; ) {
        ch = (uint8_t)*src;
        if ( ch == 0 || ch == '\n' ) {
            add_rows( line, used );
            col = used = 0;
            if ( ch == 0 || *++src == 0 ) {
                break;                  /* a last new line starts no line */
            }
            continue;
        }
        src++;
        if ( ch == '\r' ) {
            col = 0;
        } else if ( ch == '\t' ) {
            do {
                col = put_char( line, col, &used, ' ' );
            } while ( (col & 7) && col < BB_LINE );
        } else {
            col = put_char( line, col, &used, ch );
        }
    }
    top = BB_TOP + (BB_ROWS - nrows) / 2;
    left = BB_LEFT + (BB_COLS - width) / 2;
    for ( row = 0; row < nrows; row++ ) {
        scr_putn( top + row, left, A_DESK, row_text[row], row_len[row] );
    }
    return 0;
}
