/*
 * FMT.H - the printf engine, for the modules that use it
 */
#ifndef FMT_H
#define FMT_H

#include <stddef.h>
#include <stdarg.h>

/* where the engine's output goes: n bytes at s.  Returns 0, or -1 to
   stop with an error. */
typedef int (*__pm_out)( void *ctx, const char *s, size_t n );

/* format fmt with args through out; the number of characters the
   whole result has (whether or not out kept them), or -1 when out
   failed */
int __pm_vformat( __pm_out out, void *ctx, const char *fmt, va_list args );

#define FF_MINUS    0x01                /* - : left-justify */
#define FF_PLUS     0x02                /* + : a sign always */
#define FF_SPACE    0x04                /* space : a space for no sign */
#define FF_HASH     0x08                /* # : the alternate form */
#define FF_ZERO     0x10                /* 0 : pad with zeroes */
#define FF_PREC     0x20                /* a precision was given */

/* write count copies of c */
int __pm_pad( __pm_out out, void *ctx, char c, int count );

/* FMTFLT.C: one of %f %F %e %E %g %G %a %A.  Returns the characters
   written, or -1. */
int __pm_fmt_float( __pm_out out, void *ctx, long double value,
                    int flags, int width, int prec, int conv );

#endif
