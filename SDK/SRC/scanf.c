/*
 * SCANF.C - scanf() and its family
 *
 * One engine reads from a string or a stream.  A number is gathered
 * into a buffer first - the characters that can belong to one, up to
 * the field width - and then converted by strtoll(), strtoull() or
 * strtod(), so scanf rounds exactly as strtod does.  Conversions: d i
 * u o x X p n c s [ ] and f e g a (any case), with *, a width, and the
 * length modifiers hh h l ll j z t L.
 *
 * A stream gives back one character with ungetc(), which is what C
 * promises: input like "1e+" followed by something else is a matching
 * failure with the "1e+" gone, as it is in the other libraries.
 */
#include "pmlib.h"
#include "stream.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#define FIELD_MAX   512                 /* the longest number read */

typedef struct {
    FILE       *fp;                     /* or NULL for a string */
    const char *s;
    int         consumed;               /* for %n */
} SOURCE;

static int get( SOURCE *src )
{
    int c;

    if ( src->fp != NULL ) {
        c = fgetc( src->fp );
    } else {
        c = *src->s ? (unsigned char)*src->s++ : EOF;
    }
    if ( c != EOF ) {
        src->consumed++;
    }
    return c;
}

static void unget( SOURCE *src, int c )
{
    if ( c == EOF ) {
        return;
    }
    src->consumed--;
    if ( src->fp != NULL ) {
        ungetc( c, src->fp );
    } else {
        src->s--;
    }
}

enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_J, LEN_Z, LEN_T, LEN_BIG_L };

static void store_int( void *where, int len, unsigned long long v )
{
    switch ( len ) {
    case LEN_HH: *(unsigned char *)where = (unsigned char)v; break;
    case LEN_H:  *(unsigned short *)where = (unsigned short)v; break;
    case LEN_L:  *(unsigned long *)where = (unsigned long)v; break;
    case LEN_LL:
    case LEN_J:  *(unsigned long long *)where = v; break;
    case LEN_Z:
    case LEN_T:  *(size_t *)where = (size_t)v; break;
    default:     *(unsigned *)where = (unsigned)v; break;
    }
}

/* the scanset of %[: 256 flags from the format; returns the format
   after the closing ] */
static const char *scanset( const char *fmt, unsigned char *set )
{
    int invert = 0;
    int c, last;

    memset( set, 0, 256 );
    if ( *fmt == '^' ) {
        invert = 1;
        fmt++;
    }
    if ( *fmt == ']' ) {
        set[']'] = 1;                   /* ] first is a member */
        fmt++;
    }
    last = -1;
    while ( *fmt && *fmt != ']' ) {
        c = (unsigned char)*fmt++;
        if ( c == '-' && last >= 0 && *fmt && *fmt != ']' ) {
            c = (unsigned char)*fmt++;
            while ( last <= c ) {
                set[last++] = 1;
            }
            last = -1;
            continue;
        }
        set[c] = 1;
        last = c;
    }
    if ( *fmt == ']' ) {
        fmt++;
    }
    if ( invert ) {
        for ( c = 0; c < 256; c++ ) {
            set[c] = (unsigned char)!set[c];
        }
    }
    return fmt;
}

/* Gather an integer of the given base (0: as C writes it) into buf.
   Returns its length, 0 if there was none. */
static int gather_int( SOURCE *src, char *buf, int width, int base )
{
    int n = 0;
    int c = get( src );
    int digits = 0;

    if ( (c == '-' || c == '+') && n < width ) {
        buf[n++] = (char)c;
        c = get( src );
    }
    if ( (base == 0 || base == 16) && c == '0' && n < width ) {
        buf[n++] = (char)c;
        digits = 1;
        c = get( src );
        if ( (c == 'x' || c == 'X') && n < width ) {
            buf[n++] = (char)c;
            base = 16;
            digits = 0;
            c = get( src );
        } else if ( base == 0 ) {
            base = 8;
        }
    }
    if ( base == 0 ) {
        base = 10;
    }
    while ( n < width && c != EOF ) {
        if ( base == 16 ? !isxdigit( c ) : (c < '0' || c >= '0' + (base < 10 ? base : 10)) ) {
            break;
        }
        buf[n++] = (char)c;
        digits = 1;
        c = get( src );
    }
    unget( src, c );
    buf[n] = '\0';
    return digits ? n : 0;
}

/* Gather a floating-point number: what strtod reads, roughly; strtod
   then decides.  Returns its length. */
static int gather_float( SOURCE *src, char *buf, int width )
{
    int n = 0;
    int c = get( src );
    int prev = 0;
    int hex = 0;

    while ( n < width && c != EOF ) {
        if ( c == '+' || c == '-' ) {
            if ( n != 0 && !(hex ? (prev == 'p' || prev == 'P') : (prev == 'e' || prev == 'E')) ) {
                break;
            }
        } else if ( (c == 'x' || c == 'X') && n > 0 && buf[n - 1] == '0' && (n == 1 || n == 2) ) {
            hex = 1;
        } else if ( !(isalnum( c ) || c == '.' || c == '(' || c == ')') ) {
            break;
        } else if ( !hex && isalpha( c ) && !strchr( "eEiInNfFtTyYaA", c ) ) {
            break;
        } else if ( hex && isalpha( c ) && !isxdigit( c ) && c != 'p' && c != 'P' ) {
            break;
        }
        buf[n++] = (char)c;
        prev = c;
        c = get( src );
    }
    unget( src, c );
    buf[n] = '\0';
    return n;
}

static int engine( SOURCE *src, const char *fmt, va_list args )
{
    char buf[FIELD_MAX + 1];
    unsigned char set[256];
    int assigned = 0;
    int converted = 0;                  /* any conversion tried yet */
    int suppress, width, len, conv, c, n, base;
    char *end;
    char *out;
    void *where;
    long double fv;

    while ( *fmt ) {
        if ( isspace( (unsigned char)*fmt ) ) {
            while ( isspace( (unsigned char)*fmt ) ) {
                fmt++;
            }
            while ( isspace( c = get( src ) ) ) {
            }
            unget( src, c );
            continue;
        }
        if ( *fmt != '%' || fmt[1] == '%' ) {
            if ( *fmt == '%' ) {
                fmt++;                  /* %% matches one %, after space */
                while ( isspace( c = get( src ) ) ) {
                }
            } else {
                c = get( src );
            }
            if ( c != (unsigned char)*fmt ) {
                unget( src, c );
                return (c == EOF && !converted) ? EOF : assigned;
            }
            fmt++;
            continue;
        }
        fmt++;
        suppress = 0;
        if ( *fmt == '*' ) {
            suppress = 1;
            fmt++;
        }
        width = 0;
        while ( isdigit( (unsigned char)*fmt ) ) {
            width = width * 10 + (*fmt++ - '0');
            if ( width > FIELD_MAX ) {
                width = FIELD_MAX;
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
        conv = (unsigned char)*fmt++;
        if ( conv == '\0' ) {
            break;
        }
        if ( conv == 'n' ) {
            if ( !suppress ) {
                store_int( va_arg( args, void * ), len, (unsigned long long)src->consumed );
            }
            continue;
        }
        /* everything but %c and %[ skips white space first */
        if ( conv != 'c' && conv != '[' ) {
            while ( isspace( c = get( src ) ) ) {
            }
            if ( c == EOF ) {
                return converted ? assigned : EOF;
            }
            unget( src, c );
        }
        converted = 1;
        switch ( conv ) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p':
            base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16
                 : conv == 'i' ? 0 : 10;
            n = gather_int( src, buf, width ? width : FIELD_MAX, base );
            if ( n == 0 ) {
                return assigned;
            }
            if ( !suppress ) {
                unsigned long long v = (conv == 'd' || conv == 'i')
                    ? (unsigned long long)strtoll( buf, &end, base )
                    : strtoull( buf, &end, base );

                if ( conv == 'p' ) {
                    *va_arg( args, void ** ) = (void *)(uintptr_t)v;
                } else {
                    store_int( va_arg( args, void * ), len, v );
                }
                assigned++;
            }
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            n = gather_float( src, buf, width ? width : FIELD_MAX );
            if ( n == 0 ) {
                return assigned;
            }
            if ( len == LEN_BIG_L ) {
                fv = strtold( buf, &end );
            } else if ( len == LEN_L ) {
                fv = strtod( buf, &end );
            } else {
                fv = strtof( buf, &end );
            }
            if ( end != buf + n ) {
                return assigned;        /* not a number after all */
            }
            if ( !suppress ) {
                where = va_arg( args, void * );
                if ( len == LEN_BIG_L ) {
                    *(long double *)where = fv;
                } else if ( len == LEN_L ) {
                    *(double *)where = (double)fv;
                } else {
                    *(float *)where = (float)fv;
                }
                assigned++;
            }
            break;
        case 'c':
            if ( width == 0 ) {
                width = 1;
            }
            out = suppress ? NULL : va_arg( args, char * );
            for ( n = 0; n < width; n++ ) {
                c = get( src );
                if ( c == EOF ) {
                    break;
                }
                if ( out != NULL ) {
                    out[n] = (char)c;
                }
            }
            if ( n == 0 ) {
                return assigned ? assigned : EOF;
            }
            if ( !suppress ) {
                assigned++;
            }
            break;
        case 's':
        case '[':
            if ( conv == '[' ) {
                fmt = scanset( fmt, set );
            } else {
                for ( c = 0; c < 256; c++ ) {
                    set[c] = (unsigned char)!isspace( c );
                }
            }
            out = suppress ? NULL : va_arg( args, char * );
            n = 0;
            c = 0;
            for ( ;; ) {
                if ( width && n >= width ) {
                    break;
                }
                c = get( src );
                if ( c == EOF || !set[c] ) {
                    unget( src, c );
                    break;
                }
                if ( out != NULL ) {
                    out[n] = (char)c;
                }
                n++;
            }
            if ( n == 0 ) {
                return (c == EOF && assigned == 0) ? EOF : assigned;
            }
            if ( out != NULL ) {
                out[n] = '\0';
                assigned++;
            }
            break;
        default:
            return assigned;            /* not a conversion C has */
        }
    }
    return assigned;
}

int vfscanf( FILE *fp, const char *fmt, va_list args )
{
    SOURCE src;

    src.fp = fp;
    src.s = NULL;
    src.consumed = 0;
    return engine( &src, fmt, args );
}

int vscanf( const char *fmt, va_list args )
{
    return vfscanf( stdin, fmt, args );
}

int vsscanf( const char *s, const char *fmt, va_list args )
{
    SOURCE src;

    src.fp = NULL;
    src.s = s;
    src.consumed = 0;
    return engine( &src, fmt, args );
}

int scanf( const char *fmt, ... )
{
    va_list args;
    int n;

    va_start( args, fmt );
    n = vfscanf( stdin, fmt, args );
    va_end( args );
    return n;
}

int fscanf( FILE *fp, const char *fmt, ... )
{
    va_list args;
    int n;

    va_start( args, fmt );
    n = vfscanf( fp, fmt, args );
    va_end( args );
    return n;
}

int sscanf( const char *s, const char *fmt, ... )
{
    va_list args;
    int n;

    va_start( args, fmt );
    n = vsscanf( s, fmt, args );
    va_end( args );
    return n;
}
