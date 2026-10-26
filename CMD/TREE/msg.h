/*
 * MSG.H - TREE's text and console output.
 *
 * All user-visible text lives in MSG.C.  Templates take %1 and %2 as
 * already-formatted string arguments, and "\n" becomes CR LF on output.
 * The tree and the heading go to standard output; errors go to standard
 * error, as 6.22's did, so "TREE > FILE" still shows them on the screen.
 */
#ifndef MSG_H
#define MSG_H

#include "types.h"

extern const char M_HELP[];
extern const char M_NOMEM[];
extern const char M_BADSWITCH[];
extern const char M_TOOMANY[];
extern const char M_BADDRIVE[];
extern const char M_BADPATH[];
extern const char M_LABEL[];
extern const char M_NOLABEL[];
extern const char M_SERIAL[];
extern const char M_NOSUBS[];
extern const char M_TOODEEP[];

void  out_raw( int handle, const char *buf, u32 len );
void  out_text( int handle, const char *str );
void  out_msg( int handle, const char *tmpl, const char *a1, const char *a2 );   /* + newline */

#endif
