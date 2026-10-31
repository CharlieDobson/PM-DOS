/*
 * CTYPE.C - the character class table and the functions behind the
 * CTYPE.H macros
 */
#include <ctype.h>

#define C   __PM_CNTRL
#define S   __PM_SPACE
#define B   __PM_BLANK
#define P   (__PM_PUNCT | __PM_PRINT)
#define D   (__PM_DIGIT | __PM_XDIGIT | __PM_PRINT)
#define UX  (__PM_UPPER | __PM_XDIGIT | __PM_PRINT)
#define U   (__PM_UPPER | __PM_PRINT)
#define LX  (__PM_LOWER | __PM_XDIGIT | __PM_PRINT)
#define L   (__PM_LOWER | __PM_PRINT)
#define SP  (__PM_SPACE | __PM_BLANK | __PM_PRINT)

#define Z8  0, 0, 0, 0, 0, 0, 0, 0
#define Z64 Z8, Z8, Z8, Z8, Z8, Z8, Z8, Z8

const unsigned short __pm_ctype[384] = {
    Z64, Z64,                                   /* -128 to -1, EOF too */
    C, C, C, C, C, C, C, C,                     /* 00h */
    C, C | S | B, C | S, C | S, C | S, C | S, C, C,
    C, C, C, C, C, C, C, C,                     /* 10h */
    C, C, C, C, C, C, C, C,
    SP, P, P, P, P, P, P, P,                    /* 20h */
    P, P, P, P, P, P, P, P,
    D, D, D, D, D, D, D, D,                     /* 30h */
    D, D, P, P, P, P, P, P,
    P, UX, UX, UX, UX, UX, UX, U,               /* 40h */
    U, U, U, U, U, U, U, U,
    U, U, U, U, U, U, U, U,                     /* 50h */
    U, U, U, P, P, P, P, P,
    P, LX, LX, LX, LX, LX, LX, L,               /* 60h */
    L, L, L, L, L, L, L, L,
    L, L, L, L, L, L, L, L,                     /* 70h */
    L, L, L, P, P, P, P, C,
    Z64, Z64                                    /* 80h to FFh */
};

#undef isalnum
#undef isalpha
#undef isblank
#undef iscntrl
#undef isdigit
#undef isgraph
#undef islower
#undef isprint
#undef ispunct
#undef isspace
#undef isupper
#undef isxdigit

int isalnum( int c )  { return __PM_IS( c, __PM_UPPER | __PM_LOWER | __PM_DIGIT ); }
int isalpha( int c )  { return __PM_IS( c, __PM_UPPER | __PM_LOWER ); }
int isblank( int c )  { return __PM_IS( c, __PM_BLANK ); }
int iscntrl( int c )  { return __PM_IS( c, __PM_CNTRL ); }
int isdigit( int c )  { return __PM_IS( c, __PM_DIGIT ); }
int isgraph( int c )  { return __PM_IS( c, __PM_UPPER | __PM_LOWER | __PM_DIGIT | __PM_PUNCT ); }
int islower( int c )  { return __PM_IS( c, __PM_LOWER ); }
int isprint( int c )  { return __PM_IS( c, __PM_PRINT ); }
int ispunct( int c )  { return __PM_IS( c, __PM_PUNCT ); }
int isspace( int c )  { return __PM_IS( c, __PM_SPACE ); }
int isupper( int c )  { return __PM_IS( c, __PM_UPPER ); }
int isxdigit( int c ) { return __PM_IS( c, __PM_XDIGIT ); }

int tolower( int c )
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int toupper( int c )
{
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}
