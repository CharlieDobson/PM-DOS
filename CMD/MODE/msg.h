/*
 * MSG.H - MODE's text and output.
 *
 * All user-visible text lives in MSG.C.  Templates take %1 and %2 as
 * already-formatted string arguments, and "\n" becomes CR LF on output.
 * A template that starts with "\n" has the blank line in front of it
 * that MS-DOS's MODE printed before most of what it said.  Everything
 * goes to standard output, the errors as well, as 6.22's did.
 */
#ifndef MSG_H
#define MSG_H

#include "types.h"

extern const char M_HELP[];

extern const char M_BADPARAM[];
extern const char M_BADSWITCH[];
extern const char M_BADFORMAT[];
extern const char M_BADCOUNT[];
extern const char M_BAUDREQ[];
extern const char M_RATEDELAY[];
extern const char M_ILLDEV[];
extern const char M_NOTSUPP[];
extern const char M_NOTSUPP_BS[];
extern const char M_NOFONT[];
extern const char M_PRNERR[];
extern const char M_SETFOR[];
extern const char M_LPISET[];
extern const char M_COMSET[];
extern const char M_ROUTED[];
extern const char M_NOTROUTED[];
extern const char M_RETRYMSG[];
extern const char M_NO[];
extern const char M_INFINITE[];
extern const char M_NOSHIFT[];
extern const char M_LEFT[];
extern const char M_RIGHT[];
extern const char M_STATFOR[];
extern const char M_DASH5[];
extern const char M_DASH4[];
extern const char M_COLUMNS[];
extern const char M_LINES[];
extern const char M_RETRY[];
extern const char M_NONE[];

extern const char M_CPFILE[];
extern const char M_CPDEV[];
extern const char M_CPMISSING[];
extern const char M_CPBADFILE[];
extern const char M_CPREPLACED[];
extern const char M_CPACTIVE[];
extern const char M_CPDEVNOTPREP[];
extern const char M_CPLIST[];
extern const char M_HARDWARE[];
extern const char M_PREPARED[];
extern const char M_CPENTRY[];
extern const char M_CPUNPREP[];
extern const char M_CPDONE[];
extern const char M_CPNOTSUPP[];
extern const char M_CPNOSEL[];
extern const char M_CPDEVERR[];
extern const char M_CPNOTPREP[];
extern const char M_CPKEYB[];
extern const char M_CPREAD[];
extern const char M_CPNOREFRESH[];
extern const char M_W_STATUS[];
extern const char M_W_SELECT[];
extern const char M_W_REFRESH[];
extern const char M_W_PREPARE[];
extern const char M_W_WRITE[];
extern const char M_NOMEM[];

void  out_raw( int handle, const char *buf, u32 len );
void  out_text( const char *str );
void  out_msg( const char *tmpl, const char *a1, const char *a2 );   /* no newline */
void  out_line( const char *tmpl, const char *a1, const char *a2 );  /* + newline */

#endif
