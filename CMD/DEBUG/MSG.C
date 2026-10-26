/*
 * MSG.C - the text DEBUG prints.  In my own words, not MS-DOS's.  Short
 * functional lines a batch file or a person might rely on - "Error",
 * the write count - are kept the way DEBUG has always said them.
 */
#include "debug.h"

const char msg_error[]    = "^ Error";
const char msg_ended[]    = "Program ended, exit code ";
const char msg_nofile[]   = "File not found";
const char msg_writing[]  = "Writing ";
const char msg_bytes[]    = " bytes";
const char msg_noexe[]    = "EXE and HEX files cannot be written";
const char msg_noname[]   = "No file to write: name one with N";
const char msg_access[]   = "Access denied";
const char msg_nocreate[] = "The file cannot be created";
const char msg_diskfull[] = "Not enough space on the disk for the file";
const char msg_rderr[]    = "Disk error reading drive ";
const char msg_wrerr[]    = "Disk error writing drive ";
const char msg_noems[]    = "No expanded memory";
const char msg_nomem[]    = "Not enough memory";
const char msg_norun[]    = "Only a PM-DOS program can be run";
const char msg_noprog[]   = "No program is loaded to run";
const char msg_toobig[]   = "Not enough room to load the file";

/* ? - the commands, a screen that fits 25 lines.  DEBUG's own words
   for these are Microsoft's; these are mine. */
const char * const help_lines[] = {
    "assemble      A [address]",
    "compare       C range address",
    "dump          D [range]",
    "enter         E address [list]",
    "fill          F range list",
    "go            G [=address] [address...]",
    "hex add/sub   H value value",
    "input a port  I port",
    "load          L [address [drive sector count]]",
    "move          M range address",
    "name          N [pathname] [arglist]",
    "output a port O port byte",
    "proceed       P [=address] [count]",
    "quit          Q",
    "register      R [register]",
    "search        S range list",
    "trace         T [=address] [count]",
    "unassemble    U [range]",
    "write         W [address [drive sector count]]",
    "expanded mem  XA/XD/XM/XS",
    NULL
};
