/*
 * INTTYPES.H - printf and scanf formats for the STDINT.H types, and
 * the intmax_t functions
 *
 * The four compilers do not agree on which C type int32_t and intptr_t
 * are (int or long, all 32 bits), so the formats here follow each
 * one, and printf's format checks pass with all of them.
 */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

#if defined( __clang__ )
#define __PM_P32        ""
#define __PM_PPTR       ""
#elif defined( __DJGPP__ )
#define __PM_P32        ""
#define __PM_PPTR       "l"
#elif defined( __GNUC__ )
#define __PM_P32        "l"
#define __PM_PPTR       "l"
#else
#define __PM_P32        "l"
#define __PM_PPTR       ""
#endif

#define PRId8           "d"
#define PRIi8           "i"
#define PRIo8           "o"
#define PRIu8           "u"
#define PRIx8           "x"
#define PRIX8           "X"
#define PRId16          "d"
#define PRIi16          "i"
#define PRIo16          "o"
#define PRIu16          "u"
#define PRIx16          "x"
#define PRIX16          "X"
#define PRId32          __PM_P32 "d"
#define PRIi32          __PM_P32 "i"
#define PRIo32          __PM_P32 "o"
#define PRIu32          __PM_P32 "u"
#define PRIx32          __PM_P32 "x"
#define PRIX32          __PM_P32 "X"
#define PRId64          "lld"
#define PRIi64          "lli"
#define PRIo64          "llo"
#define PRIu64          "llu"
#define PRIx64          "llx"
#define PRIX64          "llX"
#define PRIdMAX         "lld"
#define PRIiMAX         "lli"
#define PRIoMAX         "llo"
#define PRIuMAX         "llu"
#define PRIxMAX         "llx"
#define PRIXMAX         "llX"
#define PRIdPTR         __PM_PPTR "d"
#define PRIiPTR         __PM_PPTR "i"
#define PRIoPTR         __PM_PPTR "o"
#define PRIuPTR         __PM_PPTR "u"
#define PRIxPTR         __PM_PPTR "x"
#define PRIXPTR         __PM_PPTR "X"
#define PRIdLEAST8      PRId8
#define PRIdLEAST16     PRId16
#define PRIdLEAST32     PRId32
#define PRIdLEAST64     PRId64
#define PRIuLEAST8      PRIu8
#define PRIuLEAST16     PRIu16
#define PRIuLEAST32     PRIu32
#define PRIuLEAST64     PRIu64
#define PRIxLEAST32     PRIx32
#define PRIxLEAST64     PRIx64
#define PRIdFAST8       PRId8
#define PRIdFAST16      PRId32
#define PRIdFAST32      PRId32
#define PRIdFAST64      PRId64
#define PRIuFAST8       PRIu8
#define PRIuFAST16      PRIu32
#define PRIuFAST32      PRIu32
#define PRIuFAST64      PRIu64
#define PRIxFAST32      PRIx32
#define PRIxFAST64      PRIx64

#define SCNd8           "hhd"
#define SCNi8           "hhi"
#define SCNo8           "hho"
#define SCNu8           "hhu"
#define SCNx8           "hhx"
#define SCNd16          "hd"
#define SCNi16          "hi"
#define SCNo16          "ho"
#define SCNu16          "hu"
#define SCNx16          "hx"
#define SCNd32          __PM_P32 "d"
#define SCNi32          __PM_P32 "i"
#define SCNo32          __PM_P32 "o"
#define SCNu32          __PM_P32 "u"
#define SCNx32          __PM_P32 "x"
#define SCNd64          "lld"
#define SCNi64          "lli"
#define SCNo64          "llo"
#define SCNu64          "llu"
#define SCNx64          "llx"
#define SCNdMAX         "lld"
#define SCNiMAX         "lli"
#define SCNuMAX         "llu"
#define SCNxMAX         "llx"
#define SCNdPTR         __PM_PPTR "d"
#define SCNuPTR         __PM_PPTR "u"
#define SCNxPTR         __PM_PPTR "x"

typedef struct {
    intmax_t quot, rem;
} imaxdiv_t;

_PM_BEGIN_C

intmax_t imaxabs( intmax_t n );
imaxdiv_t imaxdiv( intmax_t num, intmax_t den );
intmax_t strtoimax( const char *s, char **end, int base );
uintmax_t strtoumax( const char *s, char **end, int base );

_PM_END_C

#endif
