/*
 * MSG.H - message text and console output for LABEL/32.
 *
 * All user-visible text lives in MSG.C.  Templates use DOS message
 * retriever style substitution: %1 and %2 are replaced by the (already
 * formatted) string arguments.  "\n" becomes CR LF on output.
 */
#ifndef MSG_H
#define MSG_H

#include "types.h"
#include "sys.h"

extern const char M_HELP[];
extern const char M_BADDRV[];
extern const char M_NONET[];
extern const char M_SUBST[];
extern const char M_NOLOCK[];
extern const char M_NOMEM[];
extern const char M_HASLABEL[];
extern const char M_NOLABEL[];
extern const char M_SERIAL[];
extern const char M_NEWLABEL[];
extern const char M_DELLABEL[];
extern const char M_BADCHR[];
extern const char M_NOROOM[];
extern const char M_READING[];
extern const char M_WRITING[];
extern const char M_CRLF[];

const char *msg_disk_error( int code );     /* text of a DE_* code */

void  out_text( int handle, const char *str );
void  out_line( int handle, const char *str );
void  out_msg( int handle, const char *tmpl, const char *a1, const char *a2 );
void  out_flush( void );

char *fmt_hex4( u32 val, char *buf );       /* 0000-FFFF */

#endif
