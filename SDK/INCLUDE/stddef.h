/*
 * STDDEF.H - common definitions
 */
#ifndef _STDDEF_H
#define _STDDEF_H

#include <_pmdefs.h>

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __pm_size_t size_t;
#endif
typedef __pm_ptrdiff_t ptrdiff_t;
#if !defined( __cplusplus ) && !defined( _WCHAR_T_DEFINED )
#define _WCHAR_T_DEFINED
typedef __pm_wchar_t wchar_t;
#endif

/* the strictest alignment any type here needs */
typedef struct {
    long long   __ll;
    long double __ld;
} max_align_t;

#ifdef __GNUC__
#define offsetof( type, member )    __builtin_offsetof( type, member )
#else
#define offsetof( type, member )    ((size_t)&(((type *)0)->member))
#endif

#endif
