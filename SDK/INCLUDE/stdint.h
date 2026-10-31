/*
 * STDINT.H - integer types of known width
 *
 * int32_t is long under GCC and DJGPP and int under Clang, as each
 * compiler's own headers have it; both are 32 bits.  INTTYPES.H's
 * PRId32 follows the same choice so that printf checks agree.
 */
#ifndef _STDINT_H
#define _STDINT_H

#include <_pmdefs.h>

typedef signed char             int8_t;
typedef unsigned char           uint8_t;
typedef short                   int16_t;
typedef unsigned short          uint16_t;
#ifdef __INT32_TYPE__
typedef __INT32_TYPE__          int32_t;
typedef __UINT32_TYPE__         uint32_t;
#else
typedef long                    int32_t;
typedef unsigned long           uint32_t;
#endif
typedef long long               int64_t;
typedef unsigned long long      uint64_t;

typedef int8_t                  int_least8_t;
typedef uint8_t                 uint_least8_t;
typedef int16_t                 int_least16_t;
typedef uint16_t                uint_least16_t;
typedef int32_t                 int_least32_t;
typedef uint32_t                uint_least32_t;
typedef int64_t                 int_least64_t;
typedef uint64_t                uint_least64_t;

typedef int8_t                  int_fast8_t;
typedef uint8_t                 uint_fast8_t;
typedef int32_t                 int_fast16_t;
typedef uint32_t                uint_fast16_t;
typedef int32_t                 int_fast32_t;
typedef uint32_t                uint_fast32_t;
typedef int64_t                 int_fast64_t;
typedef uint64_t                uint_fast64_t;

#ifdef __INTPTR_TYPE__
typedef __INTPTR_TYPE__         intptr_t;
typedef __UINTPTR_TYPE__        uintptr_t;
#else
typedef int                     intptr_t;
typedef unsigned int            uintptr_t;
#endif
typedef int64_t                 intmax_t;
typedef uint64_t                uintmax_t;

#define INT8_MIN        (-128)
#define INT8_MAX        127
#define UINT8_MAX       255
#define INT16_MIN       (-32767 - 1)
#define INT16_MAX       32767
#define UINT16_MAX      65535
#define INT32_MIN       (-2147483647L - 1)
#define INT32_MAX       2147483647L
#define UINT32_MAX      4294967295UL
#define INT64_MIN       (-9223372036854775807LL - 1)
#define INT64_MAX       9223372036854775807LL
#define UINT64_MAX      18446744073709551615ULL

#define INT_LEAST8_MIN  INT8_MIN
#define INT_LEAST8_MAX  INT8_MAX
#define UINT_LEAST8_MAX UINT8_MAX
#define INT_LEAST16_MIN INT16_MIN
#define INT_LEAST16_MAX INT16_MAX
#define UINT_LEAST16_MAX UINT16_MAX
#define INT_LEAST32_MIN INT32_MIN
#define INT_LEAST32_MAX INT32_MAX
#define UINT_LEAST32_MAX UINT32_MAX
#define INT_LEAST64_MIN INT64_MIN
#define INT_LEAST64_MAX INT64_MAX
#define UINT_LEAST64_MAX UINT64_MAX

#define INT_FAST8_MIN   INT8_MIN
#define INT_FAST8_MAX   INT8_MAX
#define UINT_FAST8_MAX  UINT8_MAX
#define INT_FAST16_MIN  INT32_MIN
#define INT_FAST16_MAX  INT32_MAX
#define UINT_FAST16_MAX UINT32_MAX
#define INT_FAST32_MIN  INT32_MIN
#define INT_FAST32_MAX  INT32_MAX
#define UINT_FAST32_MAX UINT32_MAX
#define INT_FAST64_MIN  INT64_MIN
#define INT_FAST64_MAX  INT64_MAX
#define UINT_FAST64_MAX UINT64_MAX

#define INTPTR_MIN      INT32_MIN
#define INTPTR_MAX      INT32_MAX
#define UINTPTR_MAX     UINT32_MAX
#define INTMAX_MIN      INT64_MIN
#define INTMAX_MAX      INT64_MAX
#define UINTMAX_MAX     UINT64_MAX
#define PTRDIFF_MIN     INT32_MIN
#define PTRDIFF_MAX     INT32_MAX
#define SIZE_MAX        UINT32_MAX
#define SIG_ATOMIC_MIN  INT32_MIN
#define SIG_ATOMIC_MAX  INT32_MAX
#ifdef __WCHAR_MAX__                    /* GCC's and Clang's wchar_t is int */
#define WCHAR_MIN       __WCHAR_MIN__
#define WCHAR_MAX       __WCHAR_MAX__
#else
#define WCHAR_MIN       0
#define WCHAR_MAX       65535
#endif
#define WINT_MIN        0
#define WINT_MAX        UINT32_MAX

#define INT8_C( v )     (v)
#define UINT8_C( v )    (v)
#define INT16_C( v )    (v)
#define UINT16_C( v )   (v)
#define INT32_C( v )    (v ## L)
#define UINT32_C( v )   (v ## UL)
#define INT64_C( v )    (v ## LL)
#define UINT64_C( v )   (v ## ULL)
#define INTMAX_C( v )   (v ## LL)
#define UINTMAX_C( v )  (v ## ULL)

#endif
