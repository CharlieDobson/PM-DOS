/*
 * MSG.C - message text and console output for CHKDSK/32.
 *
 * Wording follows the MS-DOS 7.x CHKDSK.  The table layout and the
 * leading three-space indent on per-file messages come from the MS-DOS
 * 4.0 message skeleton (CHKDSK.SKL); DOS 5 and later say "allocation
 * unit" where DOS 4 said "cluster".
 */
#include "msg.h"
#include "rt.h"

const char M_HELP[] =
    "Checks a disk and displays a status report.\n"
    "\n"
    "CHKDSK [drive:][[path]filename] [/F] [/V]\n"
    "\n"
    "  [drive:][path]  Specifies the drive and directory to check.\n"
    "  filename        Specifies the file(s) to check for fragmentation.\n"
    "  /F              Fixes errors on the disk.\n"
    "  /V              Displays the full path and name of every file on the disk.\n"
    "\n"
    "Type CHKDSK without parameters to check the current drive.\n";

/* command line and drive validation */
const char M_BADSW[]      = "Invalid switch - %1";
const char M_TOOMANY[]    = "Too many parameters - %1";
const char M_BADPARM[]    = "Invalid parameter - %1";
const char M_BADDRV[]     = "Invalid drive specification";
const char M_NONET[]      = "Cannot CHKDSK a network drive";
const char M_SUBST[]      = "Cannot CHKDSK a SUBSTed or ASSIGNed drive";
const char M_NOLOCK[]     = "Cannot lock drive %1";
const char M_NOMEM[]      = "Insufficient memory";

/* volume identification */
const char M_VOLID[]      = "Volume %1 created %2 %3";
const char M_SERIAL[]     = "Volume Serial Number is %1-%2";

/* FAT access */
const char M_BADR[]       = "Disk error reading FAT %1";
const char M_BADW[]       = "   Disk error writing FAT %1";
const char M_FATBAD[]     = "   File allocation table bad, drive %1";
const char M_FATAL[]      = "   Processing cannot continue";
const char M_BADIDBYT[]   = "Probable non-DOS disk\nContinue (Y/N)?";
const char M_BADCD[]      = "   Cannot CHDIR to root";
const char M_WRFAULT[]    = "Write fault error writing drive %1";

/* tree walk */
const char M_FIXMES[]     = "Errors found, F parameter not specified\n"
                            "Corrections will not be written to disk";
const char M_DIREC[]      = "Directory %1";
const char M_NOISY[]      = "        %1";
const char M_BADCHAIN[]   = "   Has invalid allocation unit, file truncated";
const char M_BADSUBDIR[]  = "   Invalid sub-directory entry";
const char M_NDOT[]       = "   Does not exist";
const char M_NULNZ[]      = "   First allocation unit is invalid, entry truncated";
const char M_BADCLUS[]    = "   Allocation error, size adjusted";
const char M_NORECDOT[]   = "   Cannot recover . entry, processing continued";
const char M_NULDIR[]     = "   Directory is totally empty, no . or ..";
const char M_NORECDDOT[]  = "   Cannot recover .. entry";
const char M_BADLINK[]    = "   Entry has a bad link";
const char M_BADATTR[]    = "   Entry has a bad attribute";
const char M_BADSIZE[]    = "   Entry has a bad size";
const char M_CROSS[]      = "   Is cross linked on allocation unit %1";
const char M_BADTARG[]    = "   Cannot CHDIR to %1,\n"
                            "tree past this point not processed";
const char M_BADTARG2[]   = "   tree past this point not processed";
const char M_PTRANDIR[]   = "Unrecoverable error in directory";
const char M_PTRANDIR2[]  = "Convert directory to file (Y/N)?";
const char M_BADLFN[]     = "   Invalid long filename entry, entry removed";

/* lost chains */
const char M_ORPH[]       = "   %1 lost allocation units found in %2 chains.";
const char M_FREEMES[]    = "Convert lost chains to files (Y/N)?";
const char M_FREED[]      = "%1 bytes disk space freed";
const char M_WOULDFREE[]  = "%1 bytes disk space would be freed";
const char M_CREAT[]      = "   Insufficient room in root directory\n"
                            "   Move files from root directory and repeat CHKDSK";

/* status report */
const char M_DSKSPC[]     = "%1 bytes total disk space";
const char M_HIDMES[]     = "%1 bytes in %2 hidden files";
const char M_DIRMES[]     = "%1 bytes in %2 directories";
const char M_FILEMES[]    = "%1 bytes in %2 user files";
const char M_ORPHMES2[]   = "%1 bytes in %2 recovered files";
const char M_ORPHMES3[]   = "%1 bytes would be in %2 recovered files";
const char M_BADSPC[]     = "%1 bytes in bad sectors";
const char M_FRESPC[]     = "%1 bytes available on disk";
const char M_IDMES2[]     = "%1 bytes in each allocation unit";
const char M_IDMES1[]     = "%1 total allocation units on disk";
const char M_IDMES3[]     = "%1 available allocation units on disk";
const char M_TOTMEM[]     = "%1 total bytes memory";
const char M_FREMEM[]     = "%1 bytes free";

/* fragmentation report */
const char M_EXTENT[]     = "%1 Contains %2 non-contiguous blocks";
const char M_NOEXT[]      = "All specified file(s) are contiguous";
const char M_INVPATH[]    = "Path not found";
const char M_OPNERR[]     = "File not found";

COUNTRY country;

/* ------------------------------------------------------------------ */
/* buffered console output                                             */
/* ------------------------------------------------------------------ */

static char obuf[2048];
static u32  olen;
static int  ohandle = H_OUT;
static int  col;                /* characters on the current line          */
static int  blank_run;          /* empty lines written since the last text */

void msg_init( void )
{
    country.date_fmt = 0;
    country.date_sep = '-';
    country.time_sep = ':';
    country.thou_sep = ',';
    country.time_24  = 0;
    sys_get_country( &country );
}

void out_flush( void )
{
    if ( olen ) {
        sys_write( ohandle, obuf, olen );
        olen = 0;
    }
}

static void raw( int handle, const char *src, u32 len )
{
    if ( handle != ohandle ) {
        out_flush();
        ohandle = handle;
    }
    while ( len-- ) {
        if ( olen == sizeof( obuf ) ) {
            out_flush();
        }
        obuf[olen++] = *src++;
    }
}

void out_text( int handle, const char *str )
{
    for ( ; *str; str++ ) {
        if ( *str == '\n' ) {
            raw( handle, "\r\n", 2 );
            blank_run = col ? 0 : blank_run + 1;
            col = 0;
        } else {
            raw( handle, str, 1 );
            col++;
        }
    }
}

void out_line( int handle, const char *str )
{
    out_text( handle, str );
    out_text( handle, "\n" );
}

void out_msg( int handle, const char *tmpl, const char *a1, const char *a2,
              const char *a3 )
{
    char one[2];
    const char *arg;

    one[1] = 0;
    for ( ; *tmpl; tmpl++ ) {
        if ( tmpl[0] == '%' && tmpl[1] >= '1' && tmpl[1] <= '3' ) {
            arg = tmpl[1] == '1' ? a1 : tmpl[1] == '2' ? a2 : a3;
            if ( arg ) {
                out_text( handle, arg );
            }
            tmpl++;
            continue;
        }
        one[0] = *tmpl;
        out_text( handle, one );
    }
    out_text( handle, "\n" );
}

/* make sure the previous line on screen is an empty one */
void out_blank( void )
{
    if ( col ) {
        out_text( ohandle, "\n" );
    }
    if ( blank_run == 0 ) {
        out_text( ohandle, "\n" );
    }
}

/* keyboard input echo has moved the cursor to a fresh line */
void out_input_done( void )
{
    col = 0;
    blank_run = 0;
}

/* ------------------------------------------------------------------ */
/* number and date formatting                                          */
/* ------------------------------------------------------------------ */

char *fmt_num( u64 val, char *buf )
{
    char tmp[40];
    int len = 0, digits = 0;
    char *cur = buf;

    do {
        if ( digits && digits % 3 == 0 && country.thou_sep ) {
            tmp[len++] = country.thou_sep;
        }
        tmp[len++] = (char)('0' + div64( &val, 10 ));
        digits++;
    } while ( val != 0 );
    while ( len ) {
        *cur++ = tmp[--len];
    }
    *cur = 0;
    return buf;
}

char *fmt_dec( u32 val, char *buf )
{
    char tmp[12];
    int len = 0;
    char *cur = buf;

    do {
        tmp[len++] = (char)('0' + val % 10);
        val /= 10;
    } while ( val );
    while ( len ) {
        *cur++ = tmp[--len];
    }
    *cur = 0;
    return buf;
}

char *fmt_hex4( u32 val, char *buf )
{
    static const char hex[] = "0123456789ABCDEF";
    int i;

    for ( i = 0; i < 4; i++ ) {
        buf[i] = hex[(val >> ((3 - i) * 4)) & 15];
    }
    buf[4] = 0;
    return buf;
}

char *fmt_right( const char *str, int width, char *buf )
{
    int len = (int)str_len( str ), pad = 0;

    while ( len + pad < width ) {
        buf[pad++] = ' ';
    }
    str_cpy( buf + pad, str );
    return buf;
}

static char *two( u32 val, char *cur )
{
    cur[0] = (char)('0' + (val / 10) % 10);
    cur[1] = (char)('0' + val % 10);
    return cur + 2;
}

char *fmt_date( u16 dos_date, char *buf )
{
    u32 day = dos_date & 31, mon = (dos_date >> 5) & 15,
        year = 1980 + (dos_date >> 9);
    char *cur = buf;
    char year_buf[8];

    fmt_dec( year, year_buf );
    switch ( country.date_fmt ) {
    case 1:                                     /* D M Y */
        cur = two( day, cur );  *cur++ = country.date_sep;
        cur = two( mon, cur );  *cur++ = country.date_sep;
        str_cpy( cur, year_buf );
        break;
    case 2:                                     /* Y M D */
        str_cpy( cur, year_buf );  cur += str_len( year_buf );
        *cur++ = country.date_sep;  cur = two( mon, cur );
        *cur++ = country.date_sep;  cur = two( day, cur );
        *cur = 0;
        break;
    default:                                    /* M D Y */
        cur = two( mon, cur );  *cur++ = country.date_sep;
        cur = two( day, cur );  *cur++ = country.date_sep;
        str_cpy( cur, year_buf );
        break;
    }
    return buf;
}

char *fmt_time( u16 dos_time, char *buf )
{
    u32 hour = dos_time >> 11, min = (dos_time >> 5) & 63;
    char *cur = buf;
    char ampm = 'a';

    if ( !country.time_24 ) {
        if ( hour >= 12 ) {
            ampm = 'p';
        }
        hour %= 12;
        if ( hour == 0 ) {
            hour = 12;
        }
    }
    if ( hour >= 10 ) {
        *cur++ = (char)('0' + hour / 10);
    }
    *cur++ = (char)('0' + hour % 10);
    *cur++ = country.time_sep;
    cur = two( min, cur );
    if ( !country.time_24 ) {
        *cur++ = ampm;
    }
    *cur = 0;
    return buf;
}
