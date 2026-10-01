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
    "Copies files (except hidden and system files) and directory trees.\n"
    "\n"
    "XCOPY source [destination] [/A | /M] [/D[:date]] [/P] [/S [/E]] [/V] [/W]\n"
    "      [/C] [/I] [/Q] [/F] [/L] [/H] [/R] [/T] [/U] [/K] [/N] [/Y | /-Y]\n"
    "\n"
    "  source       Specifies the file(s) to copy.\n"
    "  destination  Specifies the location and/or name of new files.\n"
    "  /A           Copies files with the archive attribute set,\n"
    "               doesn't change the attribute.\n"
    "  /M           Copies files with the archive attribute set,\n"
    "               turns off the archive attribute.\n"
    "  /D:date      Copies files changed on or after the specified date.\n"
    "               If no date is given, copies only those files whose\n"
    "               source time is newer than the destination time.\n"
    "  /P           Prompts you before creating each destination file.\n"
    "  /S           Copies directories and subdirectories except empty ones.\n"
    "  /E           Copies any subdirectories, even if empty.\n"
    "  /V           Verifies each new file.\n"
    "  /W           Prompts you to press a key before copying.\n"
    "  /C           Continues copying even if errors occur.\n"
    "  /I           If destination does not exist and copying more than one file,\n"
    "               assumes that destination must be a directory.\n"
    "  /Q           Does not display file names while copying.\n"
    "  /F           Displays full source and destination file names while copying.\n"
    "  /L           Displays files that would be copied.\n"
    "  /H           Copies hidden and system files also.\n"
    "  /R           Overwrites read-only files.\n"
    "  /T           Creates directory structure, but does not copy files. Does not\n"
    "               include empty directories or subdirectories. /T /E includes\n"
    "               empty directories and subdirectories.\n"
    "  /U           Copies only files that already exist in the destination.\n"
    "  /K           Copies attributes. Normal Xcopy will reset read-only attributes.\n"
    "  /N           Copies using the generated short names.\n"
    "  /Y           Suppresses prompting to confirm you want to overwrite an\n"
    "               existing destination file.\n"
    "  /-Y          Causes prompting to confirm you want to overwrite an\n"
    "               existing destination file.\n"
    "\n"
    "The switch /Y may be preset in the COPYCMD environment variable.\n"
    "This may be overridden with /-Y on the command line.\n\n";

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
