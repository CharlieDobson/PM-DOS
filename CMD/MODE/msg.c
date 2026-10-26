/*
 * MSG.C - every word MODE says.
 *
 * The messages are 6.22's, blank lines and all, because a batch file
 * that reads them and a person who knows them expect those words; the
 * help screen is this program's own description of the same forms.
 * "Resident portion of MODE loaded" is not here: nothing of MODE stays
 * in memory on PM-DOS, because the kernel keeps the settings itself.
 */
#include "rt.h"
#include "sys.h"
#include "msg.h"

const char M_HELP[] =
    "Sets up the printer and serial ports, the screen and the keyboard,\n"
    "and shows how they are set.\n"
    "\n"
    "Printer:          MODE LPTn[:] [COLS=c] [LINES=l] [RETRY=r]\n"
    "Serial port:      MODE COMm[:] [BAUD=b] [PARITY=p] [DATA=d] [STOP=s] [RETRY=r]\n"
    "Printer to port:  MODE LPTn[:]=COMm[:]\n"
    "Screen:           MODE CON[:] [COLS=c] [LINES=n]\n"
    "                  MODE display[,n]\n"
    "Keyboard repeat:  MODE CON[:] RATE=r DELAY=d\n"
    "Code pages:       MODE device CP [/STATUS | SELECT=yyy | REFRESH]\n"
    "                  MODE device CP PREPARE=((yyy[...]) [drive:][path]filename)\n"
    "Settings:         MODE [device] [/STATUS]\n";

const char M_BADPARAM[]    = "Invalid parameter - %1";
const char M_BADSWITCH[]   = "Invalid switch - %1";
const char M_BADFORMAT[]   = "Parameter format not correct - %1";
const char M_BADCOUNT[]    = "\nInvalid number of parameters";
const char M_BAUDREQ[]     = "\nBaud rate required";
const char M_RATEDELAY[]   = "\nRate and delay must be specified together";
const char M_ILLDEV[]      = "\nIllegal device name - %1";
const char M_NOTSUPP[]     = "\nFunction not supported on this computer - %1";

/* with nothing to name, 6.22 rubbed the dash out with three backspaces */
const char M_NOTSUPP_BS[]  = "\nFunction not supported on this computer - \b\b\b   ";
const char M_NOFONT[]      = "\nRequired font not loaded";
const char M_PRNERR[]      = "\nPrinter error";
const char M_SETFOR[]      = "\nLPT%1: set for %2";
const char M_LPISET[]      = "\nPrinter lines per inch set";
const char M_COMSET[]      = "\nCOM%1: %2";
const char M_ROUTED[]      = "LPT%1: rerouted to COM%2:";
const char M_NOTROUTED[]   = "LPT%1: not rerouted";
const char M_RETRYMSG[]    = "\n%1 retry on parallel printer time-out";
const char M_NO[]          = "No";
const char M_INFINITE[]    = "Infinite";
const char M_NOSHIFT[]     = "\n      Unable to shift screen %1";
const char M_LEFT[]        = "left";
const char M_RIGHT[]       = "right";
const char M_STATFOR[]     = "\nStatus for device %1:";
const char M_DASH5[]       = "-----------------------";
const char M_DASH4[]       = "----------------------";
const char M_COLUMNS[]     = "Columns=%1";
const char M_LINES[]       = "Lines=%1";
const char M_RETRY[]       = "Retry=%1";
const char M_NONE[]        = "NONE";

const char M_CPFILE[]      = "\nFailure to access code page font file";
const char M_CPDEV[]       = "\nFailure to access device: %1";
const char M_CPMISSING[]   = "\nDevice or code page missing from font file";
const char M_CPBADFILE[]   = "\nFont file contents invalid";
const char M_CPREPLACED[]  = "\nPreviously prepared code page replaced";
const char M_CPACTIVE[]    = "\nActive code page for device %1 is %2";
const char M_CPDEVNOTPREP[] = "\nDevice %1 not prepared";
const char M_CPLIST[]      = "%1 code pages:";
const char M_HARDWARE[]    = "Hardware";
const char M_PREPARED[]    = "Prepared";
const char M_CPENTRY[]     = "  code page %1";
const char M_CPUNPREP[]    = "  code page not prepared";
const char M_CPDONE[]      = "\nMODE %1 code page function completed";
const char M_CPNOTSUPP[]   = "\nCode page operation not supported on this device";
const char M_CPNOSEL[]     = "\nNo code page has been selected";
const char M_CPDEVERR[]    = "Device error during %1";
const char M_CPNOTPREP[]   = "Code page not prepared";
const char M_CPKEYB[]      = "\nCurrent keyboard does not support this code page";
const char M_CPREAD[]      = "\nError during read of font file";
const char M_CPNOREFRESH[] = "\nUnable to perform refresh operation";
const char M_W_STATUS[]    = "status";
const char M_W_SELECT[]    = "select";
const char M_W_REFRESH[]   = "refresh";
const char M_W_PREPARE[]   = "prepare";
const char M_W_WRITE[]     = "write of font file to device";
const char M_NOMEM[]       = "Insufficient memory";

void out_raw( int handle, const char *buf, u32 len )
{
    sys_write( handle, buf, len );
}

/* "\n" as CR LF, in runs, so a line is one or two writes */
static void put_run( const char *str, u32 len )
{
    u32 start = 0, pos;

    for ( pos = 0; pos < len; pos++ ) {
        if ( str[pos] == '\n' ) {
            if ( pos > start ) {
                out_raw( H_OUT, str + start, pos - start );
            }
            out_raw( H_OUT, "\r\n", 2 );
            start = pos + 1;
        }
    }
    if ( pos > start ) {
        out_raw( H_OUT, str + start, pos - start );
    }
}

void out_text( const char *str )
{
    put_run( str, str_len( str ) );
}

void out_msg( const char *tmpl, const char *a1, const char *a2 )
{
    const char *arg;
    const char *run = tmpl;

    for ( ; *tmpl; tmpl++ ) {
        if ( tmpl[0] == '%' && (tmpl[1] == '1' || tmpl[1] == '2') ) {
            put_run( run, (u32)(tmpl - run) );
            arg = tmpl[1] == '1' ? a1 : a2;
            if ( arg ) {
                out_raw( H_OUT, arg, str_len( arg ) );
            }
            tmpl++;
            run = tmpl + 1;
        }
    }
    put_run( run, (u32)(tmpl - run) );
}

void out_line( const char *tmpl, const char *a1, const char *a2 )
{
    out_msg( tmpl, a1, a2 );
    out_raw( H_OUT, "\r\n", 2 );
}
