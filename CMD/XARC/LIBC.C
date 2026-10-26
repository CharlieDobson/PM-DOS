/*
 * LIBC.C - the C library XARC carries instead of Open Watcom's: memory
 * and strings, character classes, qsort, the printf family's engine,
 * the heap and a few conversions.  Files, folders and the clock are in
 * LIBIO.C.
 *
 * WHY A LIBRARY OF ITS OWN.  Every other C program on PM-DOS gets by on a
 * small RT.C of its own functions, but XARC is two programs' worth of
 * code that was written against the standard library - the XArchive
 * backend, shared byte for byte with the Win32s, DOS and OS/2 builds, and
 * a front end ported from a DOS one - so the standard names are what it
 * calls.  Watcom's DOS library assumes an extender (DPMI for its memory,
 * real-mode buffers for its file calls) and none is here, so these are
 * written over INT 21h as a native program has it.
 *
 * THE PRINTF ENGINE does %d %i %u %x %X %o %c %s %p and %%, with the
 * flags - + space 0 #, a width and a precision (either may be *), and
 * h, l, ll and I64 before the conversion; ll and I64 take a 64-bit
 * number.  There is no floating point at all: XARC never asks for it,
 * and a 386 without a coprocessor would fault on the first instruction.
 */
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "xsys.h"

/* ------------------------------------------------------------------ */
/* memory and strings                                                  */
/* ------------------------------------------------------------------ */

void *memcpy( void *dst, const void *src, size_t len )
{
    u8 *to = (u8 *)dst;
    const u8 *from = (const u8 *)src;

    while ( len >= 4 ) {
        *(u32 *)to = *(const u32 *)from;
        to += 4;
        from += 4;
        len -= 4;
    }
    while ( len-- ) {
        *to++ = *from++;
    }
    return dst;
}

void *memmove( void *dst, const void *src, size_t len )
{
    u8 *to = (u8 *)dst;
    const u8 *from = (const u8 *)src;

    if ( to <= from || to >= from + len ) {
        return memcpy( dst, src, len );
    }
    to += len;
    from += len;
    while ( len-- ) {
        *--to = *--from;
    }
    return dst;
}

void *memset( void *dst, int val, size_t len )
{
    u8 *to = (u8 *)dst;
    u32 four = (u8)val * 0x01010101UL;

    while ( len && ((u32)to & 3) ) {
        *to++ = (u8)val;
        len--;
    }
    while ( len >= 4 ) {
        *(u32 *)to = four;
        to += 4;
        len -= 4;
    }
    while ( len-- ) {
        *to++ = (u8)val;
    }
    return dst;
}

int memcmp( const void *lhs, const void *rhs, size_t len )
{
    const u8 *one = (const u8 *)lhs, *two = (const u8 *)rhs;

    for ( ; len; len--, one++, two++ ) {
        if ( *one != *two ) {
            return (int)*one - (int)*two;
        }
    }
    return 0;
}

void *memchr( const void *buf, int ch, size_t len )
{
    const u8 *at = (const u8 *)buf;

    for ( ; len; len--, at++ ) {
        if ( *at == (u8)ch ) {
            return (void *)at;
        }
    }
    return NULL;
}

size_t strlen( const char *text )
{
    const char *end = text;

    while ( *end ) {
        end++;
    }
    return (size_t)(end - text);
}

char *strcpy( char *dst, const char *src )
{
    char *to = dst;

    while ( (*to++ = *src++) != 0 ) {
    }
    return dst;
}

/* the standard's: no NUL when src fills max, and the rest padded */
char *strncpy( char *dst, const char *src, size_t max )
{
    size_t at = 0;

    for ( ; at < max && src[at]; at++ ) {
        dst[at] = src[at];
    }
    for ( ; at < max; at++ ) {
        dst[at] = 0;
    }
    return dst;
}

char *strcat( char *dst, const char *src )
{
    strcpy( dst + strlen( dst ), src );
    return dst;
}

char *strncat( char *dst, const char *src, size_t max )
{
    char *to = dst + strlen( dst );

    while ( max-- && *src ) {
        *to++ = *src++;
    }
    *to = 0;
    return dst;
}

int strcmp( const char *lhs, const char *rhs )
{
    while ( *lhs && *lhs == *rhs ) {
        lhs++;
        rhs++;
    }
    return (int)(u8)*lhs - (int)(u8)*rhs;
}

int strncmp( const char *lhs, const char *rhs, size_t max )
{
    for ( ; max; max--, lhs++, rhs++ ) {
        if ( *lhs != *rhs || *lhs == 0 ) {
            return (int)(u8)*lhs - (int)(u8)*rhs;
        }
    }
    return 0;
}

int stricmp( const char *lhs, const char *rhs )
{
    int one, two;

    do {
        one = toupper( (u8)*lhs++ );
        two = toupper( (u8)*rhs++ );
    } while ( one && one == two );
    return one - two;
}

int strnicmp( const char *lhs, const char *rhs, size_t max )
{
    int one, two;

    for ( ; max; max-- ) {
        one = toupper( (u8)*lhs++ );
        two = toupper( (u8)*rhs++ );
        if ( one != two || one == 0 ) {
            return one - two;
        }
    }
    return 0;
}

char *strchr( const char *text, int ch )
{
    for ( ;; text++ ) {
        if ( *text == (char)ch ) {
            return (char *)text;
        }
        if ( *text == 0 ) {
            return NULL;
        }
    }
}

char *strrchr( const char *text, int ch )
{
    const char *last = NULL;

    for ( ;; text++ ) {
        if ( *text == (char)ch ) {
            last = text;
        }
        if ( *text == 0 ) {
            return (char *)last;
        }
    }
}

char *strstr( const char *hay, const char *needle )
{
    size_t len = strlen( needle );

    for ( ; *hay; hay++ ) {
        if ( strncmp( hay, needle, len ) == 0 ) {
            return (char *)hay;
        }
    }
    return len == 0 ? (char *)hay : NULL;
}

char *strdup( const char *text )
{
    size_t len = strlen( text ) + 1;
    char *copy = (char *)malloc( len );

    if ( copy ) {
        memcpy( copy, text, len );
    }
    return copy;
}

char *strupr( char *text )
{
    char *at;

    for ( at = text; *at; at++ ) {
        *at = (char)toupper( (u8)*at );
    }
    return text;
}

char *strlwr( char *text )
{
    char *at;

    for ( at = text; *at; at++ ) {
        *at = (char)tolower( (u8)*at );
    }
    return text;
}

/* ------------------------------------------------------------------ */
/* character classes: ASCII only - see CTYPE.H                         */
/* ------------------------------------------------------------------ */

int isupper( int ch )  { return ch >= 'A' && ch <= 'Z'; }
int islower( int ch )  { return ch >= 'a' && ch <= 'z'; }
int isalpha( int ch )  { return isupper( ch ) || islower( ch ); }
int isdigit( int ch )  { return ch >= '0' && ch <= '9'; }
int isalnum( int ch )  { return isalpha( ch ) || isdigit( ch ); }
int isspace( int ch )  { return ch == ' ' || (ch >= 9 && ch <= 13); }
int isprint( int ch )  { return ch >= 32 && ch < 127; }
int iscntrl( int ch )  { return (ch >= 0 && ch < 32) || ch == 127; }
int ispunct( int ch )  { return isprint( ch ) && !isalnum( ch ) && ch != ' '; }
int toupper( int ch )  { return islower( ch ) ? ch - 'a' + 'A' : ch; }
int tolower( int ch )  { return isupper( ch ) ? ch - 'A' + 'a' : ch; }

int isxdigit( int ch )
{
    return isdigit( ch ) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

/* ------------------------------------------------------------------ */
/* numbers from text                                                   */
/* ------------------------------------------------------------------ */

unsigned long strtoul( const char *text, char **end, int base )
{
    const char *at = text;
    unsigned long val = 0;
    int digit, neg = 0, any = 0;

    while ( isspace( (u8)*at ) ) {
        at++;
    }
    if ( *at == '+' || *at == '-' ) {
        neg = *at++ == '-';
    }
    if ( (base == 0 || base == 16) && at[0] == '0' && (at[1] == 'x' || at[1] == 'X') ) {
        at += 2;
        base = 16;
    } else if ( base == 0 ) {
        base = *at == '0' ? 8 : 10;
    }
    for ( ;; at++ ) {
        if ( isdigit( (u8)*at ) ) {
            digit = *at - '0';
        } else if ( isalpha( (u8)*at ) ) {
            digit = toupper( (u8)*at ) - 'A' + 10;
        } else {
            break;
        }
        if ( digit >= base ) {
            break;
        }
        val = val * (unsigned long)base + (unsigned long)digit;
        any = 1;
    }
    if ( end ) {
        *end = (char *)(any ? at : text);
    }
    return neg ? (unsigned long)-(long)val : val;
}

long strtol( const char *text, char **end, int base )
{
    return (long)strtoul( text, end, base );
}

int  atoi( const char *text )   { return (int)strtoul( text, NULL, 10 ); }
long atol( const char *text )   { return (long)strtoul( text, NULL, 10 ); }
int  abs( int value )           { return value < 0 ? -value : value; }
long labs( long value )         { return value < 0 ? -value : value; }

/* ------------------------------------------------------------------ */
/* 64-bit arithmetic - see XSYS.H                                      */
/* ------------------------------------------------------------------ */

typedef union {
    u64 whole;
    struct { u32 lo, hi; } part;
} U64PARTS;

static u64 mul_asm( u32 lhs, u32 rhs );
#pragma aux mul_asm = \
    "mul edx" \
    parm [eax] [edx] value [edx eax] modify exact [eax edx];

/* EDX:EAX / ECX, EDX below ECX: the quotient into *quot, the remainder back */
static u32 div_asm( u32 hi, u32 lo, u32 divisor, u32 *quot );
#pragma aux div_asm = \
    "div ecx" \
    "mov [ebx],eax" \
    parm [edx] [eax] [ecx] [ebx] value [edx] modify exact [eax edx];

u64 xa_mul32( u32 lhs, u32 rhs )
{
    return mul_asm( lhs, rhs );
}

u32 xa_div64( u64 *num, u32 divisor )
{
    U64PARTS val;
    u32 qhi, qlo, rem;

    val.whole = *num;
    qhi = val.part.hi / divisor;
    rem = div_asm( val.part.hi % divisor, val.part.lo, divisor, &qlo );
    val.part.hi = qhi;
    val.part.lo = qlo;
    *num = val.whole;
    return rem;
}

u32 xa_lo32( u64 val )
{
    U64PARTS parts;

    parts.whole = val;
    return parts.part.lo;
}

u32 xa_hi32( u64 val )
{
    U64PARTS parts;

    parts.whole = val;
    return parts.part.hi;
}

u64 xa_make64( u32 hi, u32 lo )
{
    U64PARTS parts;

    parts.part.hi = hi;
    parts.part.lo = lo;
    return parts.whole;
}

/* ------------------------------------------------------------------ */
/* the heap - see STDLIB.H                                             */
/* ------------------------------------------------------------------ */

/* Each block is the kernel's, with sixteen bytes in front of it for its
   size: realloc has to know how much to copy, and sixteen keeps the
   block on the sixteen-byte boundary the kernel's heap gives. */
#define HEAD    16

void *malloc( size_t size )
{
    REGS regs;
    u8 *block;

    if ( size > 0xFFFFFFFFUL - HEAD - 16 ) {
        return NULL;
    }
    xa_clear( &regs );
    regs.ebx = (u32)size + HEAD;
    if ( xa_dos( &regs, 0x4800 ) ) {
        return NULL;
    }
    block = (u8 *)regs.eax;
    *(u32 *)block = (u32)size;
    return block + HEAD;
}

void free( void *ptr )
{
    REGS regs;

    if ( ptr == NULL ) {
        return;
    }
    xa_clear( &regs );
    regs.edx = (u32)ptr - HEAD;
    xa_dos( &regs, 0x4900 );
}

void *calloc( size_t count, size_t size )
{
    void *ptr;
    size_t total;

    if ( size && count > 0xFFFFFFFFUL / size ) {
        return NULL;
    }
    total = count * size;
    ptr = malloc( total );
    if ( ptr ) {
        memset( ptr, 0, total );
    }
    return ptr;
}

/* In place first (4Ah grows a block into a free one above it); else a
   new block, the copy, and the old one freed - and the old one left
   alone if there is no new one, as the standard says. */
void *realloc( void *ptr, size_t size )
{
    REGS regs;
    u32 old;
    void *fresh;

    if ( ptr == NULL ) {
        return malloc( size );
    }
    if ( size > 0xFFFFFFFFUL - HEAD - 16 ) {
        return NULL;
    }
    old = *(u32 *)((u8 *)ptr - HEAD);
    xa_clear( &regs );
    regs.edx = (u32)ptr - HEAD;
    regs.ebx = (u32)size + HEAD;
    if ( xa_dos( &regs, 0x4A00 ) == 0 ) {
        *(u32 *)((u8 *)ptr - HEAD) = (u32)size;
        return ptr;
    }
    fresh = malloc( size );
    if ( fresh == NULL ) {
        return NULL;
    }
    memcpy( fresh, ptr, old < size ? old : size );
    free( ptr );
    return fresh;
}

/* ------------------------------------------------------------------ */
/* qsort                                                               */
/* ------------------------------------------------------------------ */

static void swap_bytes( u8 *one, u8 *two, size_t size )
{
    u8 keep;

    while ( size-- ) {
        keep = *one;
        *one++ = *two;
        *two++ = keep;
    }
}

/* Quicksort on the median of three, the smaller part first and the
   larger looped on - so the stack is never deeper than log2 of the
   count - and insertion sort below eight elements. */
void qsort( void *base, size_t count, size_t size,
            int (*compare)( const void *, const void * ) )
{
    u8 *lo, *hi, *mid, *left, *right, *at, *back;
    size_t lcount, rcount;

    lo = (u8 *)base;
    while ( count > 7 ) {
        hi = lo + (count - 1) * size;
        mid = lo + (count / 2) * size;
        if ( compare( mid, lo ) < 0 ) {
            swap_bytes( mid, lo, size );
        }
        if ( compare( hi, mid ) < 0 ) {
            swap_bytes( hi, mid, size );
            if ( compare( mid, lo ) < 0 ) {
                swap_bytes( mid, lo, size );
            }
        }
        swap_bytes( mid, lo, size );            /* the pivot at the front */
        left = lo + size;
        right = hi;
        for ( ;; ) {
            while ( left <= right && compare( left, lo ) < 0 ) {
                left += size;
            }
            while ( left <= right && compare( right, lo ) > 0 ) {
                right -= size;
            }
            if ( left >= right ) {
                break;
            }
            swap_bytes( left, right, size );
            left += size;
            right -= size;
        }
        swap_bytes( lo, right, size );          /* the pivot where it goes */
        lcount = (size_t)(right - lo) / size;
        rcount = count - lcount - 1;
        if ( lcount < rcount ) {
            qsort( lo, lcount, size, compare );
            lo = right + size;
            count = rcount;
        } else {
            qsort( right + size, rcount, size, compare );
            count = lcount;
        }
    }
    for ( at = lo + size; at < lo + count * size; at += size ) {
        for ( back = at; back > lo && compare( back - size, back ) > 0; back -= size ) {
            swap_bytes( back - size, back, size );
        }
    }
}

/* ------------------------------------------------------------------ */
/* the printf engine                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char   *buf;                /* a string: where it goes... */
    size_t  room;               /* ...and what it has room for, NUL included */
    FILE   *fp;                 /* a file, when buf is NULL */
    size_t  count;              /* characters produced, whether or not kept */
} SINK;

static void sink_ch( SINK *sink, int ch )
{
    if ( sink->buf ) {
        if ( sink->count + 1 < sink->room ) {
            sink->buf[sink->count] = (char)ch;
        }
    } else if ( sink->fp ) {
        fputc( ch, sink->fp );
    }
    sink->count++;
}

static void sink_rep( SINK *sink, int ch, int times )
{
    while ( times-- > 0 ) {
        sink_ch( sink, ch );
    }
}

/* digits of "val" in "base", most significant first, into "out" */
static int to_digits( u64 val, int base, int upper, char *out )
{
    char rev[24];
    int len = 0, at;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        rev[len++] = set[xa_div64( &val, (u32)base )];
    } while ( val != 0 );
    for ( at = 0; at < len; at++ ) {
        out[at] = rev[len - 1 - at];
    }
    return len;
}

static int xa_vformat( SINK *sink, const char *fmt, va_list args )
{
    int left, plus, space, zero, alt, width, prec, longs, len, pad, base;
    char digits[24], sign;
    const char *str;
    u64 val;
    s32 sval;

    for ( ; *fmt; fmt++ ) {
        if ( *fmt != '%' ) {
            sink_ch( sink, (u8)*fmt );
            continue;
        }
        left = plus = space = zero = alt = 0;
        for ( ;; ) {
            fmt++;
            if ( *fmt == '-' ) {
                left = 1;
            } else if ( *fmt == '+' ) {
                plus = 1;
            } else if ( *fmt == ' ' ) {
                space = 1;
            } else if ( *fmt == '0' ) {
                zero = 1;
            } else if ( *fmt == '#' ) {
                alt = 1;
            } else {
                break;
            }
        }
        width = 0;
        if ( *fmt == '*' ) {
            width = va_arg( args, int );
            if ( width < 0 ) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while ( isdigit( (u8)*fmt ) ) {
                width = width * 10 + (*fmt++ - '0');
            }
        }
        prec = -1;
        if ( *fmt == '.' ) {
            fmt++;
            prec = 0;
            if ( *fmt == '*' ) {
                prec = va_arg( args, int );
                fmt++;
            } else {
                while ( isdigit( (u8)*fmt ) ) {
                    prec = prec * 10 + (*fmt++ - '0');
                }
            }
        }
        longs = 0;
        for ( ;; ) {
            if ( *fmt == 'h' || *fmt == 'L' || *fmt == 'z' ) {
                fmt++;
            } else if ( *fmt == 'l' ) {
                longs++;
                fmt++;
            } else if ( fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4' ) {
                longs = 2;
                fmt += 3;
            } else {
                break;
            }
        }
        if ( *fmt == 0 ) {
            break;
        }
        sign = 0;
        switch ( *fmt ) {
        case '%':
            sink_ch( sink, '%' );
            continue;
        case 'c':
            digits[0] = (char)va_arg( args, int );
            str = digits;
            len = 1;
            zero = 0;
            break;
        case 's':
            str = va_arg( args, const char * );
            if ( str == NULL ) {
                str = "(null)";
            }
            for ( len = 0; str[len] && (prec < 0 || len < prec); len++ ) {
            }
            zero = 0;
            break;
        case 'd':
        case 'i':
            if ( longs >= 2 ) {
                val = va_arg( args, u64 );
                if ( (s32)xa_hi32( val ) < 0 ) {
                    val = 0 - val;
                    sign = '-';
                }
            } else {
                sval = va_arg( args, s32 );
                if ( sval < 0 ) {
                    sign = '-';
                    val = (u32)0 - (u32)sval;
                } else {
                    val = (u32)sval;
                }
            }
            if ( sign == 0 ) {
                sign = plus ? '+' : space ? ' ' : 0;
            }
            base = 10;
            goto number;
        case 'u':
            base = 10;
            goto unsigned_number;
        case 'o':
            base = 8;
            goto unsigned_number;
        case 'p':
            prec = 8;
            /* fall through */
        case 'x':
        case 'X':
            base = 16;
        unsigned_number:
            val = longs >= 2 ? va_arg( args, u64 ) : (u64)va_arg( args, u32 );
        number:
            if ( prec == 0 && val == 0 ) {
                len = 0;
            } else {
                len = to_digits( val, base, *fmt != 'x', digits );
            }
            if ( prec >= 0 ) {
                zero = 0;
            } else {
                prec = 0;
            }
            /* a number: the sign or the prefix, then zeroes to the
               precision or the width, then the digits */
            {
                int prefix = (sign ? 1 : 0) + (alt && base == 16 && val ? 2 : 0);
                int body = len > prec ? len : prec;

                pad = width - body - prefix;
                if ( !left && !zero ) {
                    sink_rep( sink, ' ', pad );
                }
                if ( sign ) {
                    sink_ch( sink, sign );
                }
                if ( alt && base == 16 && val ) {
                    sink_ch( sink, '0' );
                    sink_ch( sink, *fmt == 'X' ? 'X' : 'x' );
                }
                if ( !left && zero ) {
                    sink_rep( sink, '0', pad );
                }
                sink_rep( sink, '0', prec - len );
                for ( prefix = 0; prefix < len; prefix++ ) {
                    sink_ch( sink, digits[prefix] );
                }
                if ( left ) {
                    sink_rep( sink, ' ', pad );
                }
            }
            continue;
        default:
            /* not a conversion this library has: shown as it was */
            sink_ch( sink, '%' );
            sink_ch( sink, (u8)*fmt );
            continue;
        }
        pad = width - len;
        if ( !left ) {
            sink_rep( sink, ' ', pad );
        }
        while ( len-- > 0 ) {
            sink_ch( sink, (u8)*str++ );
        }
        if ( left ) {
            sink_rep( sink, ' ', pad );
        }
    }
    return (int)sink->count;
}

int _vsnprintf( char *buf, size_t size, const char *fmt, va_list args )
{
    SINK sink;

    sink.buf = buf;
    sink.room = size;
    sink.fp = NULL;
    sink.count = 0;
    xa_vformat( &sink, fmt, args );
    if ( size ) {
        buf[sink.count < size ? sink.count : size - 1] = 0;
    }
    return (int)sink.count;
}

int vsprintf( char *buf, const char *fmt, va_list args )
{
    return _vsnprintf( buf, 0x7FFFFFFFUL, fmt, args );
}

int sprintf( char *buf, const char *fmt, ... )
{
    va_list args;
    int len;

    va_start( args, fmt );
    len = _vsnprintf( buf, 0x7FFFFFFFUL, fmt, args );
    va_end( args );
    return len;
}

int _snprintf( char *buf, size_t size, const char *fmt, ... )
{
    va_list args;
    int len;

    va_start( args, fmt );
    len = _vsnprintf( buf, size, fmt, args );
    va_end( args );
    return len;
}

int vfprintf( FILE *fp, const char *fmt, va_list args )
{
    SINK sink;

    sink.buf = NULL;
    sink.room = 0;
    sink.fp = fp;
    sink.count = 0;
    return xa_vformat( &sink, fmt, args );
}

int vprintf( const char *fmt, va_list args )
{
    return vfprintf( stdout, fmt, args );
}

int fprintf( FILE *fp, const char *fmt, ... )
{
    va_list args;
    int len;

    va_start( args, fmt );
    len = vfprintf( fp, fmt, args );
    va_end( args );
    return len;
}

int printf( const char *fmt, ... )
{
    va_list args;
    int len;

    va_start( args, fmt );
    len = vfprintf( stdout, fmt, args );
    va_end( args );
    return len;
}
