/*
 * MSG.C - every word TREE says.
 *
 * The heading, the errors and "No sub-directories exist" are 6.22's,
 * because a batch file that saves a tree and a person who reads one
 * know them by those words.  The help screen is this program's own
 * description of the same two switches.  M_TOODEEP is PM-DOS's: 6.22
 * could not make a tree deeper than its 64-character paths, and a
 * long-name disk can.
 */
#include "rt.h"
#include "sys.h"
#include "msg.h"

const char M_HELP[] =
    "Shows the directories of a drive or path as a tree.\n"
    "\n"
    "TREE [drive:][path] [/F] [/A]\n"
    "\n"
    "  /F   Lists the files in each directory as well.\n"
    "  /A   Draws the lines with ASCII characters (+ - \\ |).\n";

const char M_NOMEM[]     = "Insufficient memory";
const char M_BADSWITCH[] = "Invalid switch - %1";
const char M_TOOMANY[]   = "Too many parameters - %1";
const char M_BADDRIVE[]  = "Invalid drive specification";
const char M_BADPATH[]   = "Invalid path - %1";
const char M_LABEL[]     = "Directory PATH listing for Volume %1";
const char M_NOLABEL[]   = "Directory PATH listing";
const char M_SERIAL[]    = "Volume Serial Number is %1-%2";
const char M_TOODEEP[]   = "Some directories are not shown: their path would be too long";

/* 6.22's message ends CR LF LF - the second line end has no carriage
   return - and a saved tree is the same file on either system */
const char M_NOSUBS[]    = "No sub-directories exist\r\n\n";

void out_raw( int handle, const char *buf, u32 len )
{
    sys_write( handle, buf, len );
}

/* "\n" as CR LF, in runs, so a line is one or two writes */
static void put_run( int handle, const char *str, u32 len )
{
    u32 start = 0, pos;

    for ( pos = 0; pos < len; pos++ ) {
        if ( str[pos] == '\n' ) {
            if ( pos > start ) {
                out_raw( handle, str + start, pos - start );
            }
            out_raw( handle, "\r\n", 2 );
            start = pos + 1;
        }
    }
    if ( pos > start ) {
        out_raw( handle, str + start, pos - start );
    }
}

void out_text( int handle, const char *str )
{
    put_run( handle, str, str_len( str ) );
}

void out_msg( int handle, const char *tmpl, const char *a1, const char *a2 )
{
    const char *arg;
    const char *run = tmpl;

    for ( ; *tmpl; tmpl++ ) {
        if ( tmpl[0] == '%' && (tmpl[1] == '1' || tmpl[1] == '2') ) {
            put_run( handle, run, (u32)(tmpl - run) );
            arg = tmpl[1] == '1' ? a1 : a2;
            if ( arg ) {
                out_raw( handle, arg, str_len( arg ) );     /* names are shown as they are */
            }
            tmpl++;
            run = tmpl + 1;
        }
    }
    put_run( handle, run, (u32)(tmpl - run) );
    out_raw( handle, "\r\n", 2 );
}
