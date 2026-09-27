/*
 * MSG.C - every word XCOPY says.
 *
 * The messages are Windows 98's XCOPY32's, because batch files and the
 * people who read them know them by those words: "File not found - X",
 * "N File(s) copied" with the count right-aligned in nine columns, the
 * three questions.  The help screen is this program's own description
 * of the same switches.
 */
#include "rt.h"
#include "sys.h"
#include "msg.h"

const char M_HELP[] =
    "Copies files and whole directory trees, long file names included.\n"
    "\n"
    "XCOPY source [destination] [/A | /M] [/D[:date]] [/P] [/S [/E]] [/V] [/W]\n"
    "      [/C] [/I] [/Q] [/F] [/L] [/H] [/R] [/T] [/U] [/K] [/N] [/Y | /-Y]\n"
    "\n"
    "  source       What to copy: a file, a wildcard pattern or a directory.\n"
    "  destination  Where the copies go.  The current directory if left out.\n"
    "  /A           Only files with the archive attribute; the source keeps it.\n"
    "  /M           Only files with the archive attribute; the source loses it.\n"
    "  /D:date      Only files changed on or after the date given.\n"
    "  /D           With no date: only files newer than the copy already there.\n"
    "  /P           Asks before each destination file is made.\n"
    "  /S           Takes subdirectories too, leaving out empty ones.\n"
    "  /E           Takes subdirectories too, empty ones included.  Also goes\n"
    "               with /T.\n"
    "  /V           Verifies what is written.\n"
    "  /W           Waits for a key before it starts.\n"
    "  /C           Carries on copying after an error.\n"
    "  /I           If the destination does not exist and more than one file\n"
    "               is copied, takes the destination to be a directory.\n"
    "  /Q           Shows no file names while copying.\n"
    "  /F           Shows the full source and destination name of each file.\n"
    "  /L           Lists what would be copied, and copies nothing.\n"
    "  /H           Includes hidden and system files.\n"
    "  /R           Replaces read-only files.\n"
    "  /T           Builds the directory tree and copies no files: only the\n"
    "               directories that would hold a file, or all of them with /E.\n"
    "  /U           Copies only files that are already in the destination.\n"
    "  /K           Keeps the read-only attribute, which copies otherwise lose.\n"
    "  /N           Names the copies by their short (8.3) names.\n"
    "  /Y           Replaces existing files without asking.\n"
    "  /-Y          Asks before replacing an existing file.\n"
    "\n"
    "The COPYCMD environment variable may hold /Y or /-Y; the same switch on\n"
    "the command line overrides it.\n";

const char M_NOMEM[]     = "Insufficient memory";
const char M_BADDATE[]   = "Invalid date";
const char M_BADPARM[]   = "Invalid parameter - %1";
const char M_NPARMS[]    = "Invalid number of parameters";
const char M_NOTFOUND[]  = "File not found - %1";
const char M_BADDRIVE[]  = "Invalid drive specification";
const char M_BADPATH[]   = "Invalid path - %1";
const char M_COPYERR[]   = "Error copying file %1 to %2";
const char M_MKDIR[]     = "Unable to create directory %1";
const char M_FILEDIR[]   = "Does %1 specify a file name\n"
                           "or directory name on the target\n"
                           "(F = file, D = directory)? ";
const char M_YESNO[]     = "%1 (Y/N)? ";
const char M_PRESSKEY[]  = "Press any key to begin copying file(s)";
const char M_COPIED[]    = "%1 File(s) copied";
const char M_FILES[]     = "%1 File(s)";
const char M_LONGWARN[]  = "Warning: Not all files were found/copied because the resulting\n"
                           "path and/or filename would have been too long";
const char M_CYCLIC[]    = "Cannot perform a cyclic copy";
const char M_SELF[]      = "File cannot be copied onto itself";
const char M_OVERWRITE[] = "Overwrite %1 (Yes/No/All)? ";
const char M_ARROW[]     = "%1 -> %2";
const char M_FROMDEV[]   = "Cannot XCOPY from a reserved device";
const char M_TODEV[]     = "Cannot XCOPY to a reserved device";

const char *msg_error( int code )
{
    switch ( code ) {
    case ERR_NOFILE:    return "File not found";
    case ERR_NOPATH:    return "Path not found";
    case ERR_NOHANDLES: return "Too many open files";
    case ERR_ACCESS:    return "Access denied";
    case ERR_NOMEM:     return "Insufficient memory";
    case ERR_BADDRIVE:  return "Invalid drive specification";
    case 19:            return "Write protect error";
    case 21:            return "Not ready";
    case 23:            return "Data error";
    case 29:            return "Write fault";
    case 30:            return "Read fault";
    case ERR_SHARE:     return "Sharing violation";
    case ERR_LOCK:      return "Lock violation";
    case ERR_DISKFULL:  return "Insufficient disk space";
    }
    return "General failure";
}

static void raw( const char *buf, u32 len )
{
    sys_write( H_OUT, buf, len );
}

/* "\n" as CR LF, in runs, so a line is one or two writes */
static void put_run( const char *str, u32 len )
{
    u32 start = 0, pos;

    for ( pos = 0; pos < len; pos++ ) {
        if ( str[pos] == '\n' ) {
            if ( pos > start ) {
                raw( str + start, pos - start );
            }
            raw( "\r\n", 2 );
            start = pos + 1;
        }
    }
    if ( pos > start ) {
        raw( str + start, pos - start );
    }
}

void out_text( const char *str )
{
    put_run( str, str_len( str ) );
}

void out_line( const char *str )
{
    out_text( str );
    raw( "\r\n", 2 );
}

static void expand( const char *tmpl, const char *a1, const char *a2 )
{
    const char *arg;
    const char *run = tmpl;

    for ( ; *tmpl; tmpl++ ) {
        if ( tmpl[0] == '%' && (tmpl[1] == '1' || tmpl[1] == '2') ) {
            put_run( run, (u32)(tmpl - run) );
            arg = tmpl[1] == '1' ? a1 : a2;
            if ( arg ) {
                raw( arg, str_len( arg ) );         /* names are shown as they are */
            }
            tmpl++;
            run = tmpl + 1;
        }
    }
    put_run( run, (u32)(tmpl - run) );
}

void out_msg( const char *tmpl, const char *a1, const char *a2 )
{
    expand( tmpl, a1, a2 );
    raw( "\r\n", 2 );
}

void out_prompt( const char *tmpl, const char *a1 )
{
    expand( tmpl, a1, NULL );
}
