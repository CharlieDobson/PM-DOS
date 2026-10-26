/*
 * STDDEF.H - XARC's C library (LIBC.C): the basic types.
 *
 * XARC.EXE links no Open Watcom library.  These headers stand in for
 * Watcom's and declare only what LIBC.C, LIBIO.C and PMDOSPLT.C supply,
 * so a call to anything else is an unresolved symbol at link time and
 * not a silent pull from the DOS/4GW library.
 */
#ifndef _XA_STDDEF_H
#define _XA_STDDEF_H

#ifndef _XA_SIZE_T
#define _XA_SIZE_T
typedef unsigned size_t;
#endif

typedef int ptrdiff_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof( type, field ) ((size_t)&(((type *)0)->field))

#endif
