/*
 * MSG.C - message text and console output for LABEL/32.
 *
 * The messages are those of the MS-DOS 4.0 LABEL (LABL.SKL and the
 * COMMON and EXTEND classes of USA-MS.MSG), with the MS-DOS 5+ help
 * text.  Which handle each one goes to follows LABEL.ASM: information
 * and prompts on standard output, errors on standard error.
 */
#include "msg.h"
#include "rt.h"

const char M_HELP[] =
    "Creates, changes, or deletes the volume label of a disk.\n"
    "\n"
    "LABEL [drive:][label]\n";

/* drive validation (%1 is "LABEL") */
const char M_BADDRV[]     = "Invalid drive specification";
const char M_NONET[]      = "Cannot %1 a network drive";
const char M_SUBST[]      = "Cannot %1 a SUBSTed or ASSIGNed drive";
const char M_NOLOCK[]     = "Cannot lock drive %1";
const char M_NOMEM[]      = "Insufficient memory";

/* the current label */
const char M_HASLABEL[]   = "Volume in drive %1 is %2";
const char M_NOLABEL[]    = "Volume in drive %1 has no label";
const char M_SERIAL[]     = "Volume Serial Number is %1-%2";

/* prompts; neither ends the line */
const char M_NEWLABEL[]   = "Volume label (11 characters, ENTER for none)? ";
const char M_DELLABEL[]   = "\nDelete current volume label (Y/N)?";
const char M_CRLF[]       = "\n";

/* errors */
const char M_BADCHR[]     = "Invalid characters in volume label";
const char M_NOROOM[]     = "Cannot make directory entry";

/* disk errors, worded as the DOS critical error message without the
   "Abort, Retry, Fail?" question: %1 is the error, %2 the drive */
const char M_READING[]    = "%1 reading drive %2";
const char M_WRITING[]    = "%1 writing drive %2";

static const char *const disk_err[DE_LAST - DE_FIRST + 1] = {
    "Write protect error",              /* 19 */
    "Invalid unit",
    "Not ready",
    "Invalid device request",
    "Data error",
    "Invalid device request parameters",
    "Seek error",
    "Invalid media type",
    "Sector not found",
    "Printer out of paper error",
    "Write fault error",
    "Read fault error",
    "General failure"                   /* 31 */
};

const char *msg_disk_error( int code )
{
    if ( code < DE_FIRST || code > DE_LAST ) {
        code = DE_GENERAL;
    }
    return disk_err[code - DE_FIRST];
}

/* ------------------------------------------------------------------ */
/* buffered console output                                             */
/* ------------------------------------------------------------------ */

static char obuf[1024];
static u32  olen;
static int  ohandle = H_OUT;

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
        } else {
            raw( handle, str, 1 );
        }
    }
}

void out_line( int handle, const char *str )
{
    out_text( handle, str );
    out_text( handle, "\n" );
}

/* arguments are written as they are: a label may hold any byte */
void out_msg( int handle, const char *tmpl, const char *a1, const char *a2 )
{
    const char *arg;

    for ( ; *tmpl; tmpl++ ) {
        if ( tmpl[0] == '%' && (tmpl[1] == '1' || tmpl[1] == '2') ) {
            arg = tmpl[1] == '1' ? a1 : a2;
            if ( arg ) {
                raw( handle, arg, str_len( arg ) );
            }
            tmpl++;
            continue;
        }
        if ( *tmpl == '\n' ) {
            raw( handle, "\r\n", 2 );
        } else {
            raw( handle, tmpl, 1 );
        }
    }
    out_text( handle, "\n" );
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
