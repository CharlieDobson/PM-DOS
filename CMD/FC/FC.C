/*
 * FC.C - FC.EXE for PM-DOS: compares two files, or two sets of files,
 * and shows where they differ.
 *
 * MS-DOS 6.22's FC, behaviour for behaviour - its output is what people
 * read and what batch files capture, so the same lines come out in the
 * same places:
 *
 *   - A TEXT COMPARISON reads up to /LBn lines (100) of each file and
 *     looks for the first line that differs.  It then searches for the
 *     place the files agree again: /nnnn lines in a row (2) that match,
 *     trying offsets in the order 6.22 tried them.  A block is shown as
 *     the last line that matched, the differing lines, and the first
 *     line that matches again, under "***** NAME" for each file.  If
 *     the buffers fill with no resync it says "Resync failed.  Files are
 *     too different" and shows what it holds.
 *   - Lines are read 127 characters at a time - a longer line is taken
 *     as several - with every CR dropped and tabs expanded to every
 *     eighth column.  /T keeps the tabs and the CR before the LF.
 *     /W compares runs of white space as one blank, ignores them at the
 *     ends, and skips lines that are nothing else.  /C ignores case.
 *   - A BINARY COMPARISON (/B, or a first file called .EXE .COM .SYS
 *     .OBJ .LIB .BIN) lists every byte that differs as offset and the
 *     two values, then says which file is longer.
 *   - WILDCARDS: the first name is searched for; each file it finds is
 *     compared with the second name, whose * and ? are filled in from
 *     the file found.
 *   - The errorlevel is 0 whatever the files hold; 1 only when the
 *     command line is unusable (too few or too many names, /B with /L
 *     or /N).  An unknown switch is reported and the comparison goes on.
 *
 * Two of 6.22's slips are kept because the output depends on them: a
 * search for a resync whose second file runs out first carries on with
 * the FIRST file's length (yc = l1 below), and with /W a blank line
 * still uses up a place in the buffer.  One is not: under /T 6.22
 * chopped the last character off a line that had no line feed to end
 * it - the final line of a file, or a 127-character piece of a longer
 * one.  Here only a line feed is ever taken off.
 *
 * No C library.  DOSINT.ASM is the startup and the INT 21h thunk; the
 * few routines a library would have supplied are at the bottom.
 */

typedef unsigned char  u8;
typedef unsigned long  u32;

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );

#define CF( regs )   ((regs).flags & 1)
#define TRUE         1
#define FALSE        0
#define NULL         ((void *)0)

#define MAXARG       128        /* a line, NUL included */
#define MAXFNAME     80
#define LINE_TAB     8
#define RD_BUF       8192
#define OUT_BUF      4096

typedef struct {
    int line;                   /* its number in the file */
    u8  text[MAXARG];
} LINE;

typedef struct {
    int handle;
    u32 pos, len;
    int eof;
    u8 *buf;                    /* RD_BUF bytes, off the heap */
} INFILE;

/* switches */
static int ctSync  = -1;        /* lines that must match to resync */
static int cLine   = -1;        /* lines each buffer holds */
static int fAbbrev = FALSE;     /* /A */
static int fBinary = FALSE;     /* /B */
static int fLine   = FALSE;     /* /L */
static int fNumb   = FALSE;     /* /N */
static int fCase   = TRUE;      /* not /C */
static int fIgnore = FALSE;     /* /W */
static int fTabs   = FALSE;     /* /T */

static LINE   *buffer1, *buffer2;
static INFILE  in1, in2;
static char    tail[160];
static char   *argv[64];
static int     argc;
static u8      out_buf[OUT_BUF];
static u32     out_len;
static u8      dta[64];

static const char *ext_bin[] = {
    ".EXE", ".OBJ", ".LIB", ".COM", ".BIN", ".SYS", NULL
};

static const char msg_help[] =
    "Compares two files, or two sets of files, and shows how they differ.\r\n"
    "\r\n"
    "FC [/A] [/C] [/L] [/LBn] [/N] [/T] [/W] [/nnnn] [drive1:][path1]filename1\r\n"
    "  [drive2:][path2]filename2\r\n"
    "FC /B [drive1:][path1]filename1 [drive2:][path2]filename2\r\n"
    "\r\n"
    "  /A     Shows only the first and last line of each set of differences.\r\n"
    "  /B     Compares byte by byte and lists each byte that differs.\r\n"
    "  /C     Treats capitals and small letters as the same.\r\n"
    "  /L     Compares the files as lines of text.\r\n"
    "  /LBn   Gives up after n differing lines in a row (100 if not given).\r\n"
    "  /N     Shows line numbers in a text comparison.\r\n"
    "  /T     Leaves tabs alone instead of expanding them to every 8th column.\r\n"
    "  /W     Treats runs of spaces and tabs as one space, and ignores them at\r\n"
    "         either end of a line and on lines with nothing else on them.\r\n"
    "  /nnnn  How many lines must match before a difference counts as ended\r\n"
    "         (2 if not given).\r\n"
    "\r\n"
    "A file named .EXE, .COM, .SYS, .OBJ, .LIB or .BIN is compared byte by\r\n"
    "byte unless /L is given.\r\n";

/* ---------------------------------------------------------------- */
/* the few library routines                                          */
/* ---------------------------------------------------------------- */

static u32 str_len( const char *str )
{
    u32 len = 0;

    while ( str[len] ) len++;
    return len;
}

static void str_cpy( char *dst, const char *src )
{
    while ( (*dst++ = *src++) != 0 ) ;
}

static void str_cat( char *dst, const char *src )
{
    str_cpy( dst + str_len( dst ), src );
}

static char *str_chr( const char *str, int ch )
{
    for ( ; *str; str++ ) {
        if ( *str == (char)ch ) return (char *)str;
    }
    return NULL;
}

static char *str_rchr( const char *str, int ch )
{
    char *hit = NULL;

    for ( ; *str; str++ ) {
        if ( *str == (char)ch ) hit = (char *)str;
    }
    return hit;
}

static int up( int ch )
{
    if ( ch >= 'a' && ch <= 'z' ) return ch - 'a' + 'A';
    return ch;
}

static void str_upr( char *str )
{
    for ( ; *str; str++ ) *str = (char)up( (u8)*str );
}

static int str_icmp( const char *lhs, const char *rhs )
{
    int c1, c2;

    do {
        c1 = up( (u8)*lhs++ );
        c2 = up( (u8)*rhs++ );
    } while ( c1 == c2 && c1 );
    return c1 - c2;
}

static void mem_move( void *dst, const void *src, u32 len )
{
    u8 *d = dst;
    const u8 *s = src;

    if ( d < s ) {
        while ( len-- ) *d++ = *s++;
    } else {
        d += len;
        s += len;
        while ( len-- ) *--d = *--s;
    }
}

static int is_space( int ch )
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'
        || ch == '\v' || ch == '\f';
}

/* ---------------------------------------------------------------- */
/* system calls                                                      */
/* ---------------------------------------------------------------- */

static void clear( REGS *regs )
{
    u8 *p = (u8 *)regs;
    u32 i;

    for ( i = 0; i < sizeof( *regs ); i++ ) p[i] = 0;
}

static void out_flush( void )
{
    REGS regs;

    if ( out_len ) {
        clear( &regs );
        regs.eax = 0x4000;
        regs.ebx = 1;
        regs.ecx = out_len;
        regs.edx = (u32)out_buf;
        int21( &regs );
        out_len = 0;
    }
}

static void fc_exit( int code )
{
    REGS regs;

    out_flush();
    clear( &regs );
    regs.eax = 0x4C00 | (code & 0xFF);
    int21( &regs );
}

static void out_bytes( const char *str, u32 len )
{
    while ( len-- ) {
        if ( out_len == OUT_BUF ) out_flush();
        out_buf[out_len++] = (u8)*str++;
    }
}

static void out_str( const char *str )
{
    out_bytes( str, str_len( str ) );
}

/* a line feed, as C's text-mode stdout wrote one */
static void out_nl( void )
{
    out_bytes( "\r\n", 2 );
}

static void out_line( const char *str )
{
    out_str( str );
    out_nl();
}

static void out_hex( u32 val, int digits )
{
    char buf[8];
    int i;

    for ( i = digits - 1; i >= 0; i-- ) {
        buf[i] = "0123456789ABCDEF"[val & 15];
        val >>= 4;
    }
    out_bytes( buf, digits );
}

/* "%5d" */
static void out_dec5( int val )
{
    char buf[12];
    int i = 11, neg = val < 0;
    u32 u = neg ? (u32)-val : (u32)val;

    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while ( u );
    if ( neg ) buf[--i] = '-';
    while ( 11 - i < 5 ) buf[--i] = ' ';
    out_str( buf + i );
}

static void *mem_alloc( u32 size )
{
    REGS regs;

    clear( &regs );
    regs.eax = 0x4800;
    regs.ebx = size;
    int21( &regs );
    return CF( regs ) ? NULL : (void *)regs.eax;
}

static void mem_free( void *ptr )
{
    REGS regs;

    clear( &regs );
    regs.eax = 0x4900;
    regs.edx = (u32)ptr;
    int21( &regs );
}

/* the DOS error code, or 0 */
static int f_open( INFILE *f, const char *name )
{
    REGS regs;

    clear( &regs );
    regs.eax = 0x3D00;
    regs.edx = (u32)name;
    int21( &regs );
    if ( CF( regs ) ) return (int)(regs.eax & 0xFFFF);
    f->handle = (int)(regs.eax & 0xFFFF);
    f->pos = f->len = 0;
    f->eof = FALSE;
    return 0;
}

static void f_close( INFILE *f )
{
    REGS regs;

    clear( &regs );
    regs.eax = 0x3E00;
    regs.ebx = (u32)f->handle;
    int21( &regs );
}

/* the next byte, or -1 at the end */
static int f_getc( INFILE *f )
{
    REGS regs;

    if ( f->pos == f->len ) {
        if ( f->eof ) return -1;
        clear( &regs );
        regs.eax = 0x3F00;
        regs.ebx = (u32)f->handle;
        regs.ecx = RD_BUF;
        regs.edx = (u32)f->buf;
        int21( &regs );
        if ( CF( regs ) || regs.eax == 0 ) {
            f->eof = TRUE;
            return -1;
        }
        f->len = regs.eax;
        f->pos = 0;
    }
    return f->buf[f->pos++];
}

/* the C library's words for what went wrong, as 6.22's FC printed them */
static const char *err_text( int err )
{
    switch ( err ) {
    case 2: case 3: case 15: case 18:
        return "No such file or directory";
    case 4:
        return "Too many open files";
    case 5: case 19: case 32: case 33:
        return "Permission denied";
    case 6:
        return "Bad file number";
    case 8:
        return "Not enough core";
    }
    return "Invalid argument";
}

static int find_first( const char *spec )
{
    REGS regs;

    clear( &regs );
    regs.eax = 0x1A00;
    regs.edx = (u32)dta;
    int21( &regs );
    clear( &regs );
    regs.eax = 0x4E00;
    regs.ecx = 1;                       /* files and read-only ones */
    regs.edx = (u32)spec;
    int21( &regs );
    return !CF( regs );
}

static int find_next( void )
{
    REGS regs;

    clear( &regs );
    regs.eax = 0x4F00;
    int21( &regs );
    return !CF( regs );
}

/* ---------------------------------------------------------------- */
/* messages                                                          */
/* ---------------------------------------------------------------- */

static void fc_say( const char *a, const char *b, const char *c )
{
    out_str( "FC: " );
    out_str( a );
    if ( b ) out_str( b );
    if ( c ) out_str( c );
    out_nl();
}

/* ---------------------------------------------------------------- */
/* the binary comparison                                             */
/* ---------------------------------------------------------------- */

/* in1 and in2 are open; both are closed by the caller */
static void binary_compare( const char *f1, const char *f2 )
{
    int c1, c2, same = TRUE;
    u32 pos = 0;

    for ( ;; ) {
        c1 = f_getc( &in1 );
        c2 = f_getc( &in2 );
        if ( c1 < 0 || c2 < 0 ) break;
        if ( c1 != c2 ) {
            same = FALSE;
            out_hex( pos, 8 );
            out_str( ": " );
            out_hex( (u32)c1, 2 );
            out_str( " " );
            out_hex( (u32)c2, 2 );
            out_nl();
        }
        pos++;
    }
    if ( c1 >= 0 ) {
        fc_say( f1, " longer than ", f2 );
    } else if ( c2 >= 0 ) {
        fc_say( f2, " longer than ", f1 );
    } else if ( same ) {
        fc_say( "no differences encountered", NULL, NULL );
    }
}

/* ---------------------------------------------------------------- */
/* the text comparison                                               */
/* ---------------------------------------------------------------- */

/* One line into text, at most MAXARG-1 characters.  FALSE at the end of
   the file with nothing read. */
static int read_line( INFILE *f, u8 *text )
{
    int c = 0, left = MAXARG - 1, n = 0, fill;

    if ( fTabs ) {
        /* the CR stays, the LF goes, and nothing is expanded */
        while ( left ) {
            c = f_getc( f );
            if ( c < 0 ) break;
            if ( c == '\n' ) break;
            text[n++] = (u8)c;
            left--;
        }
        text[n] = 0;
        return !( c < 0 && n == 0 );
    }
    while ( left ) {
        c = f_getc( f );
        if ( c < 0 || c == '\n' ) break;
        if ( c == '\r' ) continue;
        if ( c != '\t' ) {
            text[n++] = (u8)c;
            left--;
        } else {
            fill = LINE_TAB - (n & (LINE_TAB - 1));
            if ( fill > left ) fill = left;
            left -= fill;
            while ( fill-- ) text[n++] = ' ';
        }
    }
    text[n] = 0;
    return !( c < 0 && n == 0 );
}

static const u8 *skip_white( const u8 *p )
{
    while ( is_space( *p ) ) p++;
    return p;
}

/* white space compressed: zero if the two lines are the same */
static int cmp_white( const u8 *p1, const u8 *p2, int fold )
{
    const u8 *q;
    int c1, c2;

    p1 = skip_white( p1 );
    p2 = skip_white( p2 );
    for ( ;; ) {
        c1 = *p1;
        c2 = *p2;
        if ( fold ) {
            c1 = up( c1 );
            c2 = up( c2 );
        }
        if ( c1 != c2 ) return *p1 - *p2;
        if ( *p1++ == 0 ) return 0;
        p2++;
        if ( is_space( *p1 ) ) {
            q = skip_white( p1 );
            p1 = *q == 0 ? q : q - 1;
        }
        if ( is_space( *p2 ) ) {
            q = skip_white( p2 );
            p2 = *q == 0 ? q : q - 1;
        }
    }
}

static int lines_differ( const u8 *a, const u8 *b )
{
    int c1, c2;

    if ( fIgnore ) return cmp_white( a, b, !fCase );
    for ( ;; ) {
        c1 = *a++;
        c2 = *b++;
        if ( !fCase ) {
            c1 = up( c1 );
            c2 = up( c2 );
        }
        if ( c1 != c2 ) return 1;
        if ( c1 == 0 ) return 0;
    }
}

/* Read up to ct lines in at pl.  Returns how many were stored. */
static int fill( LINE *pl, INFILE *f, int ct, int *plnum )
{
    int i = 0;

    while ( ct-- > 0 && read_line( f, pl->text ) ) {
        if ( fIgnore && cmp_white( pl->text, (const u8 *)"", 0 ) == 0 ) {
            pl->text[0] = 0;
            ++*plnum;
        }
        if ( pl->text[0] != 0 || !fIgnore ) {
            pl->line = ++*plnum;
            pl++;
            i++;
        }
    }
    return i;
}

/* Drop the first lt of the ml lines; how many are left */
static int adjust( LINE *pl, int ml, int lt )
{
    if ( ml <= lt ) return 0;
    mem_move( &pl[0], &pl[lt], sizeof( LINE ) * (u32)(ml - lt) );
    return ml - lt;
}

/* ct lines from s1 and s2 all the same? */
static int compare( int l1, int s1, int l2, int s2, int ct )
{
    if ( ct <= 0 || s1 + ct > l1 || s2 + ct > l2 ) return FALSE;
    while ( ct-- ) {
        if ( lines_differ( buffer1[s1++].text, buffer2[s2++].text ) ) {
            return FALSE;
        }
    }
    return TRUE;
}

static void pline( const LINE *pl )
{
    if ( fNumb ) {
        out_dec5( pl->line );
        out_str( ":  " );
    }
    out_line( (const char *)pl->text );
}

static void dump( const LINE *pl, int start, int end )
{
    if ( fAbbrev && end - start > 2 ) {
        pline( pl + start );
        out_line( "..." );
        pline( pl + end );
    } else {
        while ( start <= end ) pline( pl + start++ );
    }
}

static void show( const char *f1, int end1, const char *f2, int end2 )
{
    out_str( "***** " );
    out_line( f1 );
    dump( buffer1, 0, end1 );
    out_str( "***** " );
    out_line( f2 );
    dump( buffer2, 0, end2 );
    out_line( "*****" );
    out_nl();
}

static int min_i( int a, int b )
{
    return a < b ? a : b;
}

/* in1 and in2 are open; both are closed by the caller */
static void line_compare( const char *f1, const char *f2 )
{
    int l1, l2, i, xp, yp, xc, yc, xd, yd, same = TRUE;
    int line1, line2;

    buffer1 = mem_alloc( (u32)cLine * sizeof( LINE ) + 1 );
    buffer2 = buffer1 ? mem_alloc( (u32)cLine * sizeof( LINE ) + 1 ) : NULL;
    if ( buffer2 == NULL ) {
        if ( buffer1 ) mem_free( buffer1 );
        fc_say( "out of memory", NULL, NULL );
        return;
    }

    l1 = l2 = 0;
    line1 = line2 = 0;
    for ( ;; ) {
        l1 += fill( buffer1 + l1, &in1, cLine - l1, &line1 );
        l2 += fill( buffer2 + l2, &in2, cLine - l2, &line2 );
        if ( l1 == 0 && l2 == 0 ) {
            if ( same ) fc_say( "no differences encountered", NULL, NULL );
            break;
        }

        /* past what is the same, keeping one line of it to show */
        xc = min_i( l1, l2 );
        for ( i = 0; i < xc; i++ ) {
            if ( !compare( l1, i, l2, i, 1 ) ) break;
        }
        if ( i != xc ) i = i - 1 > 0 ? i - 1 : 0;
        l1 = adjust( buffer1, l1, i );
        l2 = adjust( buffer2, l2, i );
        if ( l1 == 0 && l2 == 0 ) continue;
        l1 += fill( buffer1 + l1, &in1, cLine - l1, &line1 );
        l2 += fill( buffer2 + l2, &in2, cLine - l2, &line2 );

        /* where do they agree again?  xc lines into the first file
           against yp into the second, then xp against yc, widening */
        xd = yd = FALSE;
        xc = yc = 1;
        xp = yp = 1;
        for ( ;; ) {
            i = min_i( min_i( l1 - xc, l2 - yp ), ctSync );
            if ( compare( l1, xc, l2, yp, i ) ) {
                same = FALSE;
                show( f1, xc, f2, yp );
                l1 = adjust( buffer1, l1, xc );
                l2 = adjust( buffer2, l2, yp );
                break;
            }
            i = min_i( min_i( l1 - xp, l2 - yc ), ctSync );
            if ( compare( l1, xp, l2, yc, i ) ) {
                same = FALSE;
                show( f1, xp, f2, yc );
                l1 = adjust( buffer1, l1, xp );
                l2 = adjust( buffer2, l2, yc );
                break;
            }
            if ( ++xp > xc ) {
                xp = 1;
                if ( ++xc >= l1 ) {
                    xc = l1;
                    xd = TRUE;
                }
            }
            if ( ++yp > yc ) {
                yp = 1;
                if ( ++yc >= l2 ) {
                    yc = l1;            /* sic - see the top of the file */
                    yd = TRUE;
                }
            }
            if ( xd && yd ) break;
        }
        if ( !(xd && yd) ) continue;     /* resynced: on to the next */

        /* nothing agrees again inside the buffers */
        same = FALSE;
        if ( l1 >= cLine || l2 >= cLine ) {
            out_line( "Resync failed.  Files are too different" );
        }
        show( f1, l1 - 1, f2, l2 - 1 );
        break;
    }
    mem_free( buffer2 );
    mem_free( buffer1 );
}

/* ---------------------------------------------------------------- */
/* names                                                             */
/* ---------------------------------------------------------------- */

static void cannot_open( const char *name, int err )
{
    out_str( "FC: cannot open " );
    out_str( name );
    out_str( " - " );
    out_line( err_text( err ) );
}

static void comp( const char *f1, const char *f2 )
{
    int err;

    out_str( "Comparing files " );
    out_str( f1 );
    out_str( " and " );
    out_line( f2 );

    if ( (err = f_open( &in1, f1 )) != 0 ) {
        cannot_open( f1, err );
    } else if ( (err = f_open( &in2, f2 )) != 0 ) {
        cannot_open( f2, err );
        f_close( &in1 );
    } else {
        if ( fBinary ) {
            binary_compare( f1, f2 );
        } else {
            line_compare( f1, f2 );
        }
        f_close( &in1 );
        f_close( &in2 );
    }
    out_nl();
}

/* where the file's own name starts in a path */
static char *find_file_name( char *path )
{
    char *p = path + str_len( path );

    while ( p > path && p[-1] != '\\' && p[-1] != ':' && p[-1] != '/' ) p--;
    return p;
}

static int has_wildcard( const char *name )
{
    return str_chr( name, '?' ) != NULL || str_chr( name, '*' ) != NULL;
}

/* "*" with no dot after it means "*.*" to the user and "*." to 4Eh */
static void check_wildcard( char *path, char *file )
{
    char *star;

    if ( str_chr( file, '.' ) == NULL && (star = str_chr( file, '*' )) != NULL ) {
        if ( str_len( path ) <= MAXFNAME - 3 ) {
            star[1] = '.';
            star[2] = '*';
            star[3] = 0;
        }
    }
}

/* The second name's * and ? from the first file's name.  FALSE if a ?
   has nothing in the first name to stand for. */
static int expand_file2( const char *file1, char *file2 )
{
    const char *dot1, *end1, *src;
    char *dot2, *end2, *dst;
    char ext[8];

    end1 = file1 + str_len( file1 );
    end2 = file2 + str_len( file2 );
    if ( (dot1 = str_rchr( file1, '.' )) == NULL ) dot1 = end1;
    if ( (dot2 = str_rchr( file2, '.' )) == NULL ) dot2 = end2;

    if ( (dst = str_chr( file2, '*' )) != NULL ) {
        if ( dst < dot2 ) {             /* in the name */
            str_cpy( ext, dot2 );
            src = file1 + (dst - file2);
            for ( ; src < dot1; src++, dst++ ) *dst = *src;
            *dst = 0;
            end2 += dst - dot2;
            dot2 = dst;
            str_cat( file2, ext );
        }
        if ( (dst = str_chr( file2, '*' )) != NULL ) {
            src = dot1 + (dst - dot2);  /* in the extension */
            for ( ; src < end1; src++, dst++ ) *dst = *src;
            *dst = 0;
            end2 = dst;
        }
    }

    dst = file2;
    while ( (dst = str_chr( dst, '?' )) != NULL ) {
        if ( dst < dot2 ) {
            src = file1 + (dst - file2);
            if ( src >= dot1 ) return FALSE;
        } else {
            src = dot1 + (dst - dot2);
            if ( src >= end1 ) return FALSE;
        }
        *dst = *src;
        dst++;
    }

    if ( (dst = str_rchr( file2, '.' )) != NULL && dst == end2 - 1 ) {
        *dst = 0;
    }
    return TRUE;
}

static void parse_file_names( char *file1, char *file2 )
{
    char final1[MAXFNAME + 4], final2[MAXFNAME + 4], inter2[MAXFNAME + 4];
    char *fname1, *fname2;
    int wild2;

    str_upr( file1 );
    str_upr( file2 );
    str_cpy( final1, file1 );
    str_cpy( final2, file2 );
    fname1 = find_file_name( final1 );
    fname2 = find_file_name( final2 );
    check_wildcard( final1, fname1 );
    check_wildcard( final2, fname2 );
    str_cpy( inter2, final2 );

    if ( !find_first( final1 ) ) {
        out_str( "File(s) not found : " );
        out_line( file1 );
        return;
    }
    wild2 = has_wildcard( inter2 );
    do {
        str_cpy( fname1, (const char *)dta + 0x1E );
        if ( wild2 ) {
            if ( !expand_file2( fname1, fname2 ) ) {
                out_str( final1 );
                out_str( "     " );
                out_line( file2 );
                out_line( "Could not expand second filename so as to match first" );
                out_nl();
                out_nl();
            } else {
                comp( final1, final2 );
            }
            str_cpy( final2, inter2 );
        } else {
            comp( final1, final2 );
        }
    } while ( find_next() );
}

/* ---------------------------------------------------------------- */
/* the command line                                                  */
/* ---------------------------------------------------------------- */

/* The tail into words, as the C library's startup split it: blanks
   separate, double quotes group and are removed. */
static void split_args( void )
{
    REGS regs;
    u8 *psp;
    u32 len, i;
    char *in, *out;
    int quoted;

    clear( &regs );
    regs.eax = 0x6200;
    int21( &regs );
    psp = (u8 *)regs.ebx;
    len = psp[0x80];
    if ( len > 127 ) len = 127;
    for ( i = 0; i < len; i++ ) tail[i] = (char)psp[0x81 + i];
    tail[len] = 0;

    in = out = tail;
    argc = 0;
    for ( ;; ) {
        while ( *in == ' ' || *in == '\t' ) in++;
        if ( *in == 0 || *in == '\r' ) break;
        if ( argc == 63 ) break;
        argv[argc++] = out;
        quoted = FALSE;
        while ( *in && *in != '\r' ) {
            if ( *in == '"' ) {
                quoted = !quoted;
                in++;
                continue;
            }
            if ( !quoted && (*in == ' ' || *in == '\t') ) break;
            *out++ = *in++;
        }
        if ( *in ) in++;
        *out++ = 0;
    }
}

/* the digits at p, as a number; 0 if there are none */
static int to_int( const char *p )
{
    int val = 0;

    while ( *p >= '0' && *p <= '9' ) val = val * 10 + (*p++ - '0');
    return val;
}

static int all_digits( const char *p )
{
    while ( *p >= '0' && *p <= '9' ) p++;
    return *p == 0;
}

static void extension( const char *name, char *ext )
{
    const char *dot = str_rchr( name, '.' );
    const char *slash = find_file_name( (char *)name );

    if ( dot == NULL || dot < slash ) {
        ext[0] = 0;
    } else {
        str_cpy( ext, dot );
    }
}

void __cdecl dos_entry( void )
{
    char n[2][MAXFNAME + 4];
    char ext[MAXFNAME + 4];
    int i, fileargs = 0;
    u32 j, len;
    char *arg, *slash;

    in1.buf = mem_alloc( RD_BUF );      /* off the heap, so that the */
    in2.buf = mem_alloc( RD_BUF );      /* file is not 16K of zeroes */
    if ( in1.buf == NULL || in2.buf == NULL ) {
        fc_say( "out of memory", NULL, NULL );
        fc_exit( 1 );
    }

    split_args();
    for ( i = 0; i < argc; i++ ) {
        arg = argv[i];
        if ( arg[0] != '/' ) {
            if ( fileargs == 2 ) {
                fc_say( "Too many filenames", NULL, NULL );
                out_nl();
                fc_exit( 1 );
            }
            slash = str_chr( arg, '/' );
            if ( slash ) *slash = 0;
            if ( str_len( arg ) > MAXFNAME ) arg[MAXFNAME] = 0;
            str_cpy( n[fileargs++], arg );
            if ( slash ) *slash = '/';
        }
        len = str_len( arg );
        for ( j = 0; j < len; j++ ) {
            if ( arg[j] != '/' ) continue;
            switch ( up( (u8)arg[j + 1] ) ) {
            case '?':
                out_str( msg_help );
                fc_exit( 0 );
                break;
            case 'A':
                fAbbrev = TRUE;
                break;
            case 'B':
                fBinary = TRUE;
                break;
            case 'C':
                fCase = FALSE;
                break;
            case 'W':
                fIgnore = TRUE;
                break;
            case 'L':
                if ( up( (u8)arg[j + 2] ) == 'B' ) {
                    cLine = to_int( arg + j + 3 );
                } else {
                    fLine = TRUE;
                }
                break;
            case 'N':
                fNumb = TRUE;
                break;
            case 'T':
                fTabs = TRUE;
                break;
            default:
                if ( all_digits( arg + j + 1 ) ) {
                    ctSync = to_int( arg + j + 1 );
                } else {
                    fc_say( "Invalid switch", NULL, NULL );
                    out_nl();
                }
            }
        }
    }
    if ( fileargs != 2 ) {
        fc_say( "Insufficient number of filespecs", NULL, NULL );
        fc_exit( 1 );
    }
    if ( ctSync != -1 ) {
        fLine = TRUE;
    } else {
        ctSync = 2;
    }
    if ( cLine == -1 ) cLine = 100;

    if ( !fBinary && !fLine ) {
        extension( n[0], ext );
        for ( i = 0; ext_bin[i]; i++ ) {
            if ( str_icmp( ext_bin[i], ext ) == 0 ) fBinary = TRUE;
        }
        if ( !fBinary ) fLine = TRUE;
    }
    if ( fBinary && (fLine || fNumb) ) {
        fc_say( "Incompatible switches", NULL, NULL );
        fc_exit( 1 );
    }

    parse_file_names( n[0], n[1] );
    fc_exit( 0 );
}
