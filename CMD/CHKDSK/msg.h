/*
 * MSG.H - message text and console output for CHKDSK/32.
 *
 * All user-visible text lives in MSG.C.  Templates use DOS message
 * retriever style substitution: %1, %2 and %3 are replaced by the
 * (already formatted) string arguments.  "\n" becomes CR LF on output.
 */
#ifndef MSG_H
#define MSG_H

#include "types.h"
#include "sys.h"

extern const char M_HELP[];
extern const char M_BADSW[];
extern const char M_TOOMANY[];
extern const char M_BADPARM[];
extern const char M_BADDRV[];
extern const char M_NONET[];
extern const char M_SUBST[];
extern const char M_NOLOCK[];
extern const char M_NOMEM[];
extern const char M_VOLID[];
extern const char M_SERIAL[];
extern const char M_BADR[];
extern const char M_BADW[];
extern const char M_FATBAD[];
extern const char M_FATAL[];
extern const char M_BADIDBYT[];
extern const char M_BADCD[];
extern const char M_FIXMES[];
extern const char M_DIREC[];
extern const char M_NOISY[];
extern const char M_BADCHAIN[];
extern const char M_BADSUBDIR[];
extern const char M_NDOT[];
extern const char M_NULNZ[];
extern const char M_BADCLUS[];
extern const char M_NORECDOT[];
extern const char M_NULDIR[];
extern const char M_NORECDDOT[];
extern const char M_BADLINK[];
extern const char M_BADATTR[];
extern const char M_BADSIZE[];
extern const char M_CROSS[];
extern const char M_BADTARG[];
extern const char M_BADTARG2[];
extern const char M_PTRANDIR[];
extern const char M_PTRANDIR2[];
extern const char M_ORPH[];
extern const char M_FREEMES[];
extern const char M_FREED[];
extern const char M_WOULDFREE[];
extern const char M_CREAT[];
extern const char M_BADLFN[];
extern const char M_WRFAULT[];
extern const char M_DSKSPC[];
extern const char M_HIDMES[];
extern const char M_DIRMES[];
extern const char M_FILEMES[];
extern const char M_ORPHMES2[];
extern const char M_ORPHMES3[];
extern const char M_BADSPC[];
extern const char M_FRESPC[];
extern const char M_IDMES2[];
extern const char M_IDMES1[];
extern const char M_IDMES3[];
extern const char M_TOTMEM[];
extern const char M_FREMEM[];
extern const char M_EXTENT[];
extern const char M_NOEXT[];
extern const char M_INVPATH[];
extern const char M_OPNERR[];

extern COUNTRY country;

void  msg_init( void );
void  out_text( int handle, const char *str );
void  out_line( int handle, const char *str );
void  out_msg( int handle, const char *tmpl, const char *a1, const char *a2,
               const char *a3 );
void  out_blank( void );
void  out_flush( void );
void  out_input_done( void );

char *fmt_num( u64 val, char *buf );        /* 1,234,567 (country separator) */
char *fmt_dec( u32 val, char *buf );        /* 1234567                       */
char *fmt_hex4( u32 val, char *buf );       /* 0000-FFFF                     */
char *fmt_right( const char *str, int width, char *buf );
char *fmt_date( u16 dos_date, char *buf );
char *fmt_time( u16 dos_time, char *buf );

#endif
