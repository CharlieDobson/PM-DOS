/*
 * CTYPE.H - character classes
 *
 * The "C" locale: ASCII.  Characters 128-255 belong to no class.
 *
 * The table runs from -128, so a plain char above 127 that a signed
 * char makes negative still lands inside it (in no class), and EOF
 * (-1) does too.
 */
#ifndef _CTYPE_H
#define _CTYPE_H

#include <_pmdefs.h>

_PM_BEGIN_C

int isalnum( int c );
int isalpha( int c );
int isblank( int c );
int iscntrl( int c );
int isdigit( int c );
int isgraph( int c );
int islower( int c );
int isprint( int c );
int ispunct( int c );
int isspace( int c );
int isupper( int c );
int isxdigit( int c );
int tolower( int c );
int toupper( int c );

/* what the functions above look up: -128 to 255 */
extern const unsigned short __pm_ctype[384];

_PM_END_C

#define __PM_UPPER      0x01
#define __PM_LOWER      0x02
#define __PM_DIGIT      0x04
#define __PM_SPACE      0x08    /* space, \t \n \v \f \r */
#define __PM_PUNCT      0x10
#define __PM_CNTRL      0x20
#define __PM_XDIGIT     0x40
#define __PM_BLANK      0x80    /* space and \t */
#define __PM_PRINT      0x100   /* graphic, or space */

#define __PM_IS( c, bits )  (__pm_ctype[(int)(c) + 128] & (bits))

#define isalnum( c )    __PM_IS( c, __PM_UPPER | __PM_LOWER | __PM_DIGIT )
#define isalpha( c )    __PM_IS( c, __PM_UPPER | __PM_LOWER )
#define isblank( c )    __PM_IS( c, __PM_BLANK )
#define iscntrl( c )    __PM_IS( c, __PM_CNTRL )
#define isdigit( c )    __PM_IS( c, __PM_DIGIT )
#define isgraph( c )    __PM_IS( c, __PM_UPPER | __PM_LOWER | __PM_DIGIT | __PM_PUNCT )
#define islower( c )    __PM_IS( c, __PM_LOWER )
#define isprint( c )    __PM_IS( c, __PM_PRINT )
#define ispunct( c )    __PM_IS( c, __PM_PUNCT )
#define isspace( c )    __PM_IS( c, __PM_SPACE )
#define isupper( c )    __PM_IS( c, __PM_UPPER )
#define isxdigit( c )   __PM_IS( c, __PM_XDIGIT )

#endif
