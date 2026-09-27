/*
 * MSG.H - XCOPY's text and console output.
 *
 * All user-visible text lives in MSG.C.  Templates take %1 and %2 as
 * already-formatted string arguments, and "\n" becomes CR LF on output.
 * Everything goes to standard output, errors included: neither shell
 * here can redirect standard error, and a batch file that logs an XCOPY
 * wants its complaints in the log.
 */
#ifndef MSG_H
#define MSG_H

#include "types.h"

extern const char M_HELP[];
extern const char M_NOMEM[];
extern const char M_BADDATE[];
extern const char M_BADPARM[];
extern const char M_NPARMS[];
extern const char M_NOTFOUND[];
extern const char M_BADDRIVE[];
extern const char M_BADPATH[];
extern const char M_COPYERR[];
extern const char M_MKDIR[];
extern const char M_FILEDIR[];
extern const char M_YESNO[];
extern const char M_PRESSKEY[];
extern const char M_COPIED[];
extern const char M_FILES[];
extern const char M_LONGWARN[];
extern const char M_CYCLIC[];
extern const char M_SELF[];
extern const char M_OVERWRITE[];
extern const char M_ARROW[];
extern const char M_FROMDEV[];
extern const char M_TODEV[];

const char *msg_error( int code );          /* a DOS error code as text */

void  out_text( const char *str );
void  out_line( const char *str );
void  out_msg( const char *tmpl, const char *a1, const char *a2 );   /* + newline */
void  out_prompt( const char *tmpl, const char *a1 );               /* no newline */

#endif
