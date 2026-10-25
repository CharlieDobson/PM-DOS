/*
 * RT.C - the little runtime both halves of SETUP carry.
 *
 * The 32-bit half links no C library at all (like every native PM-DOS
 * program); the 16-bit half has Open Watcom's, but uses these as well
 * so that the two behave alike.  Only <stdarg.h> is borrowed, and it is
 * nothing but macros.
 */
#include <stdarg.h>
#include "setup.h"
#include "text.h"

void mem_set( void *dst, int val, u16 len )
{
    u8 *ptr = (u8 *)dst;

    while ( len-- ) {
        *ptr++ = (u8)val;
    }
}

void mem_cpy( void *dst, const void *src, u16 len )
{
    u8 *out = (u8 *)dst;
    const u8 *in = (const u8 *)src;

    while ( len-- ) {
        *out++ = *in++;
    }
}

int mem_cmp( const void *lhs, const void *rhs, u16 len )
{
    const u8 *one = (const u8 *)lhs;
    const u8 *two = (const u8 *)rhs;

    while ( len-- ) {
        if ( *one != *two ) {
            return *one < *two ? -1 : 1;
        }
        one++;
        two++;
    }
    return 0;
}

int str_len( const char *str )
{
    int len = 0;

    while ( str[len] ) {
        len++;
    }
    return len;
}

/* always ends the copy, cutting it to max - 1 characters */
void str_cpyn( char *dst, const char *src, int max )
{
    if ( max <= 0 ) {
        return;
    }
    while ( *src && max > 1 ) {
        *dst++ = *src++;
        max--;
    }
    *dst = 0;
}

void str_catn( char *dst, const char *src, int max )
{
    int len = str_len( dst );

    if ( len < max ) {
        str_cpyn( dst + len, src, max - len );
    }
}

int ch_upper( int ch )
{
    return ch >= 'a' && ch <= 'z' ? ch - 32 : ch;
}

int str_icmp( const char *lhs, const char *rhs )
{
    int one, two;

    do {
        one = ch_upper( (u8)*lhs++ );
        two = ch_upper( (u8)*rhs++ );
        if ( one != two ) {
            return one < two ? -1 : 1;
        }
    } while ( one );
    return 0;
}

int str_nicmp( const char *lhs, const char *rhs, int len )
{
    int one, two;

    while ( len-- > 0 ) {
        one = ch_upper( (u8)*lhs++ );
        two = ch_upper( (u8)*rhs++ );
        if ( one != two ) {
            return one < two ? -1 : 1;
        }
        if ( !one ) {
            break;
        }
    }
    return 0;
}

char *str_chr( const char *str, int ch )
{
    for ( ; *str; str++ ) {
        if ( *str == (char)ch ) {
            return (char *)str;
        }
    }
    return NULL;
}

char *str_rchr( const char *str, int ch )
{
    const char *last = NULL;

    for ( ; *str; str++ ) {
        if ( *str == (char)ch ) {
            last = str;
        }
    }
    return (char *)last;
}

char *str_dup( const char *str )
{
    int len = str_len( str );
    char *copy = (char *)xalloc( (u32)len + 1 );

    mem_cpy( copy, str, (u16)(len + 1) );
    return copy;
}

void str_upper( char *str )
{
    for ( ; *str; str++ ) {
        *str = (char)ch_upper( (u8)*str );
    }
}

char *skip_blanks( const char *str )
{
    while ( *str == ' ' || *str == '\t' ) {
        str++;
    }
    return (char *)str;
}

/* dir + '\' + name; dir may end in '\' already, or be empty */
void path_join( char *dst, const char *dir, const char *name, int max )
{
    int len;

    str_cpyn( dst, dir, max );
    len = str_len( dst );
    if ( len && dst[len - 1] != '\\' && dst[len - 1] != ':' ) {
        str_catn( dst, "\\", max );
    }
    str_catn( dst, name, max );
}

void path_name( char *dst, const char *path, int max )
{
    const char *last = str_rchr( path, '\\' );

    str_cpyn( dst, last ? last + 1 : path, max );
}

static int name_char( int ch )
{
    if ( ch <= ' ' || ch >= 0x7F ) {
        return 0;
    }
    return str_chr( "\"*+,./:;<=>?[\\]|", ch ) == NULL;
}

/* A directory Setup may install into: a drive letter, a colon, a '\',
   then 8.3 names joined by '\', not ending in one, nothing reserved. */
int path_valid( const char *path )
{
    int part, dot, ext;

    if ( !path[0] || path[1] != ':' || path[2] != '\\' ) {
        return 0;
    }
    path += 3;
    if ( !*path ) {
        return 0;                       /* the root: never */
    }
    while ( *path ) {
        part = dot = ext = 0;
        while ( *path && *path != '\\' ) {
            if ( *path == '.' ) {
                if ( dot || part == 0 ) {
                    return 0;
                }
                dot = 1;
            } else if ( !name_char( (u8)*path ) ) {
                return 0;
            } else if ( dot ) {
                if ( ++ext > 3 ) {
                    return 0;
                }
            } else if ( ++part > 8 ) {
                return 0;
            }
            path++;
        }
        if ( part == 0 || (dot && ext == 0) ) {
            return 0;
        }
        if ( *path == '\\' ) {
            path++;
            if ( !*path ) {
                return 0;
            }
        }
    }
    return 1;
}

void *xalloc( u32 size )
{
    void *ptr = plat_alloc( size ? size : 1 );

    if ( ptr == NULL ) {
        fatal( tx_nomem );
    }
    return ptr;
}

void xfree( void *ptr )
{
    if ( ptr ) {
        plat_free( ptr );
    }
}

u32 parse_u32( const char *str, const char **end )
{
    u32 val = 0;

    while ( *str >= '0' && *str <= '9' ) {
        val = val * 10 + (u32)(*str - '0');
        str++;
    }
    if ( end ) {
        *end = str;
    }
    return val;
}

/* ------------------------------------------------------------------ */
/* sfmt - a printf for the few shapes Setup prints:                    */
/*   %s %c %d %u %x %X, %ld %lu %lx, a width and '0', '-' to the left  */
/*   %, after the width puts the country's thousands separator in      */
/* ------------------------------------------------------------------ */

char fmt_thousands = ',';

static int put( char *buf, int pos, int max, int ch )
{
    if ( pos < max - 1 ) {
        buf[pos] = (char)ch;
    }
    return pos + 1;
}

int __cdecl sfmt( char *buf, int max, const char *fmt, ... )
{
    va_list args;
    char num[24], *str;
    int pos = 0, width, left, zero, islong, group, len, i, neg, digits;
    u32 val;
    unsigned base;

    va_start( args, fmt );
    for ( ; *fmt; fmt++ ) {
        if ( *fmt != '%' ) {
            pos = put( buf, pos, max, *fmt );
            continue;
        }
        fmt++;
        left = zero = islong = group = 0;
        if ( *fmt == '-' ) {
            left = 1;
            fmt++;
        }
        if ( *fmt == '0' ) {
            zero = 1;
            fmt++;
        }
        width = 0;
        while ( *fmt >= '0' && *fmt <= '9' ) {
            width = width * 10 + (*fmt++ - '0');
        }
        if ( *fmt == ',' ) {
            group = 1;
            fmt++;
        }
        if ( *fmt == 'l' ) {
            islong = 1;
            fmt++;
        }
        str = num;
        neg = 0;
        switch ( *fmt ) {
        case 's':
            str = va_arg( args, char * );
            if ( str == NULL ) {
                str = "";
            }
            break;
        case 'c':
            num[0] = (char)va_arg( args, int );
            num[1] = 0;
            break;
        case 'd': case 'u': case 'x': case 'X':
            if ( islong ) {
                val = va_arg( args, u32 );
            } else if ( *fmt == 'd' ) {
                val = (u32)(s32)va_arg( args, int );
            } else {
                val = (u32)va_arg( args, unsigned );
            }
            if ( *fmt == 'd' && (s32)val < 0 ) {
                neg = 1;
                val = (u32)(-(s32)val);
            }
            base = (*fmt == 'x' || *fmt == 'X') ? 16 : 10;
            len = 0;
            digits = 0;
            do {
                if ( group && digits && digits % 3 == 0 ) {
                    num[len++] = fmt_thousands;
                }
                i = (int)(val % base);
                num[len++] = (char)(i < 10 ? '0' + i
                                    : (*fmt == 'x' ? 'a' : 'A') + i - 10);
                val /= base;
                digits++;
            } while ( val );
            if ( neg ) {
                num[len++] = '-';
            }
            for ( i = 0; i < len / 2; i++ ) {       /* they came backwards */
                char tmp = num[i];
                num[i] = num[len - 1 - i];
                num[len - 1 - i] = tmp;
            }
            num[len] = 0;
            break;
        case '%':
            num[0] = '%';
            num[1] = 0;
            break;
        default:
            num[0] = 0;
            break;
        }
        len = str_len( str );
        if ( !left ) {
            for ( i = len; i < width; i++ ) {
                pos = put( buf, pos, max, zero ? '0' : ' ' );
            }
        }
        for ( i = 0; i < len; i++ ) {
            pos = put( buf, pos, max, str[i] );
        }
        if ( left ) {
            for ( i = len; i < width; i++ ) {
                pos = put( buf, pos, max, ' ' );
            }
        }
    }
    va_end( args );
    if ( max > 0 ) {
        buf[pos < max ? pos : max - 1] = 0;
    }
    return pos;
}
