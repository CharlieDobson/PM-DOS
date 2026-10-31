/*
 * FMT.C - the printf engine
 *
 * Everything C99 asks of printf except the floating-point conversions,
 * which are FMTFLT.C's: flags - + space # 0, a width and a precision
 * (either may be *), the length modifiers hh h l ll j z t L, and the
 * conversions d i u o x X c s p n %.  %p is eight hex digits, as the
 * other 32-bit DOS compilers print it.
 */
#include "pmlib.h"
#include "fmt.h"
#include <string.h>
#include <limits.h>

enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_J, LEN_Z, LEN_T, LEN_BIG_L };

int __pm_pad( __pm_out out, void *ctx, char c, int count )
{
    char run[32];
    int n;

    if ( count <= 0 ) {
        return 0;
    }
    memset( run, c, sizeof( run ) );
    while ( count > 0 ) {
        n = count < (int)sizeof( run ) ? count : (int)sizeof( run );
        if ( out( ctx, run, (size_t)n ) < 0 ) {
            return -1;
        }
        count -= n;
    }
    return 0;
}

/* one integer conversion, already fetched: magnitude, sign */
static int put_int( __pm_out out, void *ctx, unsigned long long value, int neg,
                    int flags, int width, int prec, int conv )
{
    char digits[24];                    /* 22 octal digits for 64 bits */
    const char *set = (conv == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned base = (conv == 'o') ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10;
    const char *prefix = "";
    int ndig = 0;
    int zeros;
    int len;

    while ( value ) {
        digits[sizeof( digits ) - 1 - ndig++] = set[value % base];
        value /= base;
    }
    if ( !(flags & FF_PREC) ) {
        prec = 1;
    }
    zeros = prec > ndig ? prec - ndig : 0;
    if ( conv == 'o' && (flags & FF_HASH) && zeros == 0
         && (ndig == 0 || digits[sizeof( digits ) - ndig] != '0') ) {
        zeros = 1;                      /* # makes the first digit a 0 */
    }
    if ( neg ) {
        prefix = "-";
    } else if ( conv == 'd' || conv == 'i' ) {
        prefix = (flags & FF_PLUS) ? "+" : (flags & FF_SPACE) ? " " : "";
    } else if ( (flags & FF_HASH) && ndig > 0 && (conv == 'x' || conv == 'X') ) {
        prefix = (conv == 'X') ? "0X" : "0x";
    }
    len = (int)strlen( prefix ) + zeros + ndig;
    if ( (flags & FF_ZERO) && !(flags & (FF_MINUS | FF_PREC)) && width > len ) {
        zeros += width - len;           /* 0: the padding is zeroes, after the sign */
        len = width;
    }
    if ( !(flags & FF_MINUS) && __pm_pad( out, ctx, ' ', width - len ) < 0 ) {
        return -1;
    }
    if ( *prefix && out( ctx, prefix, strlen( prefix ) ) < 0 ) {
        return -1;
    }
    if ( __pm_pad( out, ctx, '0', zeros ) < 0 ) {
        return -1;
    }
    if ( ndig && out( ctx, digits + sizeof( digits ) - ndig, (size_t)ndig ) < 0 ) {
        return -1;
    }
    if ( (flags & FF_MINUS) && __pm_pad( out, ctx, ' ', width - len ) < 0 ) {
        return -1;
    }
    return width > len ? width : len;
}

/* a string, at most prec bytes of it when a precision was given */
static int put_str( __pm_out out, void *ctx, const char *s, int flags, int width, int prec )
{
    int len;

    if ( s == NULL ) {
        s = "(null)";
    }
    len = (flags & FF_PREC) ? (int)strnlen( s, (size_t)prec ) : (int)strlen( s );
    if ( !(flags & FF_MINUS) && __pm_pad( out, ctx, ' ', width - len ) < 0 ) {
        return -1;
    }
    if ( len && out( ctx, s, (size_t)len ) < 0 ) {
        return -1;
    }
    if ( (flags & FF_MINUS) && __pm_pad( out, ctx, ' ', width - len ) < 0 ) {
        return -1;
    }
    return width > len ? width : len;
}

int __pm_vformat( __pm_out out, void *ctx, const char *fmt, va_list args )
{
    const char *run;
    long long total = 0;
    long long sval;
    unsigned long long uval;
    long double fval;
    int flags, width, prec, len, conv, n;
    char ch;
    void *where;

    while ( *fmt ) {
        if ( *fmt != '%' ) {
            run = fmt;
            while ( *fmt && *fmt != '%' ) {
                fmt++;
            }
            if ( out( ctx, run, (size_t)(fmt - run) ) < 0 ) {
                return -1;
            }
            total += fmt - run;
            continue;
        }
        fmt++;

        flags = 0;
        for ( ;; fmt++ ) {
            if ( *fmt == '-' ) {
                flags |= FF_MINUS;
            } else if ( *fmt == '+' ) {
                flags |= FF_PLUS;
            } else if ( *fmt == ' ' ) {
                flags |= FF_SPACE;
            } else if ( *fmt == '#' ) {
                flags |= FF_HASH;
            } else if ( *fmt == '0' ) {
                flags |= FF_ZERO;
            } else {
                break;
            }
        }

        width = 0;
        if ( *fmt == '*' ) {
            width = va_arg( args, int );
            if ( width < 0 ) {
                flags |= FF_MINUS;
                width = width == INT_MIN ? INT_MAX : -width;
            }
            fmt++;
        } else {
            while ( *fmt >= '0' && *fmt <= '9' ) {
                width = width > (INT_MAX - 9) / 10 ? INT_MAX : width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        prec = 0;
        if ( *fmt == '.' ) {
            fmt++;
            flags |= FF_PREC;
            if ( *fmt == '*' ) {
                prec = va_arg( args, int );
                if ( prec < 0 ) {
                    flags &= ~FF_PREC;  /* a negative one is none at all */
                    prec = 0;
                }
                fmt++;
            } else {
                while ( *fmt >= '0' && *fmt <= '9' ) {
                    prec = prec > (INT_MAX - 9) / 10 ? INT_MAX : prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        len = LEN_NONE;
        switch ( *fmt ) {
        case 'h':
            len = (fmt[1] == 'h') ? LEN_HH : LEN_H;
            fmt += (len == LEN_HH) ? 2 : 1;
            break;
        case 'l':
            len = (fmt[1] == 'l') ? LEN_LL : LEN_L;
            fmt += (len == LEN_LL) ? 2 : 1;
            break;
        case 'j': len = LEN_J; fmt++; break;
        case 'z': len = LEN_Z; fmt++; break;
        case 't': len = LEN_T; fmt++; break;
        case 'L': len = LEN_BIG_L; fmt++; break;
        }

        conv = (unsigned char)*fmt;
        if ( conv == '\0' ) {
            break;                      /* a lone % at the end */
        }
        fmt++;
        n = 0;
        switch ( conv ) {
        case 'd':
        case 'i':
            switch ( len ) {
            case LEN_HH: sval = (signed char)va_arg( args, int ); break;
            case LEN_H:  sval = (short)va_arg( args, int ); break;
            case LEN_L:  sval = va_arg( args, long ); break;
            case LEN_LL:
            case LEN_J:  sval = va_arg( args, long long ); break;
            case LEN_Z:
            case LEN_T:  sval = va_arg( args, ptrdiff_t ); break;
            default:     sval = va_arg( args, int ); break;
            }
            uval = sval < 0 ? 0 - (unsigned long long)sval : (unsigned long long)sval;
            n = put_int( out, ctx, uval, sval < 0, flags, width, prec, conv );
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            switch ( len ) {
            case LEN_HH: uval = (unsigned char)va_arg( args, unsigned ); break;
            case LEN_H:  uval = (unsigned short)va_arg( args, unsigned ); break;
            case LEN_L:  uval = va_arg( args, unsigned long ); break;
            case LEN_LL:
            case LEN_J:  uval = va_arg( args, unsigned long long ); break;
            case LEN_Z:
            case LEN_T:  uval = va_arg( args, size_t ); break;
            default:     uval = va_arg( args, unsigned ); break;
            }
            n = put_int( out, ctx, uval, 0, flags & ~(FF_PLUS | FF_SPACE), width, prec, conv );
            break;
        case 'p':
            uval = (uintptr_t)va_arg( args, void * );
            n = put_int( out, ctx, uval, 0, (flags & FF_MINUS) | FF_PREC, width, 8, 'X' );
            break;
        case 'c':
            ch = (char)va_arg( args, int );
            if ( !(flags & FF_MINUS) && __pm_pad( out, ctx, ' ', width - 1 ) < 0 ) {
                return -1;
            }
            if ( out( ctx, &ch, 1 ) < 0 ) {
                return -1;
            }
            if ( (flags & FF_MINUS) && __pm_pad( out, ctx, ' ', width - 1 ) < 0 ) {
                return -1;
            }
            n = width > 1 ? width : 1;
            break;
        case 's':
            n = put_str( out, ctx, va_arg( args, const char * ), flags, width, prec );
            break;
        case 'f': case 'F':
        case 'e': case 'E':
        case 'g': case 'G':
        case 'a': case 'A':
            if ( len == LEN_BIG_L ) {
                fval = va_arg( args, long double );
            } else {
                fval = va_arg( args, double );
            }
            n = __pm_fmt_float( out, ctx, fval, flags, width, (flags & FF_PREC) ? prec : -1, conv );
            break;
        case 'n':
            where = va_arg( args, void * );
            switch ( len ) {
            case LEN_HH: *(signed char *)where = (signed char)total; break;
            case LEN_H:  *(short *)where = (short)total; break;
            case LEN_L:  *(long *)where = (long)total; break;
            case LEN_LL:
            case LEN_J:  *(long long *)where = total; break;
            default:     *(int *)where = (int)total; break;
            }
            break;
        case '%':
            if ( out( ctx, "%", 1 ) < 0 ) {
                return -1;
            }
            n = 1;
            break;
        default:                        /* not a conversion: print it as it was */
            ch = (char)conv;
            if ( out( ctx, "%", 1 ) < 0 || out( ctx, &ch, 1 ) < 0 ) {
                return -1;
            }
            n = 2;
            break;
        }
        if ( n < 0 ) {
            return -1;
        }
        total += n;
    }
    return total > INT_MAX ? INT_MAX : (int)total;
}
