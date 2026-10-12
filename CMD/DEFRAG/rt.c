/*
 * RT.C - minimal runtime for DEFRAG (no C library is linked).  PMD's
 * strings, heap and sfmt; CHKDSK's cluster lists and 64-bit helpers.
 */
#include "rt.h"
#include "sys.h"

/* ------------------------------------------------------------------ */
/* memory and strings                                                  */
/* ------------------------------------------------------------------ */

void mem_set( void *dst, int val, u32 len )
{
    u8 *cur = (u8 *)dst;

    while ( len-- ) {
        *cur++ = (u8)val;
    }
}

void mem_cpy( void *dst, const void *src, u32 len )
{
    u8 *dcur = (u8 *)dst;
    const u8 *scur = (const u8 *)src;

    if ( dcur < scur ) {
        while ( len-- ) {
            *dcur++ = *scur++;
        }
    } else if ( dcur > scur ) {
        dcur += len;
        scur += len;
        while ( len-- ) {
            *--dcur = *--scur;
        }
    }
}

int mem_cmp( const void *lhs, const void *rhs, u32 len )
{
    const u8 *lcur = (const u8 *)lhs;
    const u8 *rcur = (const u8 *)rhs;

    for ( ; len; len--, lcur++, rcur++ ) {
        if ( *lcur != *rcur ) {
            return (int)*lcur - (int)*rcur;
        }
    }
    return 0;
}

u32 str_len( const char *str )
{
    u32 len = 0;

    while ( str[len] ) {
        len++;
    }
    return len;
}

char *str_cpy( char *dst, const char *src )
{
    char *start = dst;

    while ( (*dst++ = *src++) != 0 ) {
    }
    return start;
}

/* a copy that stops short of "max" bytes including the NUL */
int str_cpyn( char *dst, const char *src, u32 max )
{
    u32 len = str_len( src );

    if ( max == 0 ) {
        return 0;
    }
    if ( len + 1 > max ) {
        mem_cpy( dst, src, max - 1 );
        dst[max - 1] = 0;
        return 0;
    }
    mem_cpy( dst, src, len + 1 );
    return 1;
}

int str_catn( char *dst, const char *src, u32 max )
{
    u32 have = str_len( dst );

    if ( have >= max ) {
        return 0;
    }
    return str_cpyn( dst + have, src, max - have );
}

int ch_upper( int ch )
{
    ch &= 0xFF;
    if ( ch >= 'a' && ch <= 'z' ) {
        ch -= 'a' - 'A';
    }
    return ch;
}

int str_icmp( const char *lhs, const char *rhs )
{
    int lch, rch;

    for ( ;; ) {
        lch = ch_upper( *lhs++ );
        rch = ch_upper( *rhs++ );
        if ( lch != rch ) {
            return lch - rch;
        }
        if ( lch == 0 ) {
            return 0;
        }
    }
}

int str_nicmp( const char *lhs, const char *rhs, u32 len )
{
    int lch, rch;

    for ( ; len; len-- ) {
        lch = ch_upper( *lhs++ );
        rch = ch_upper( *rhs++ );
        if ( lch != rch ) {
            return lch - rch;
        }
        if ( lch == 0 ) {
            return 0;
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

char *str_dup( const char *str )
{
    return str_cpy( (char *)xalloc( str_len( str ) + 1 ), str );
}

/* "C:\" + "X" -> "C:\X",  "C:\A" + "X" -> "C:\A\X" */
char *path_join( const char *dir, const char *name )
{
    u32 len = str_len( dir );
    char *path = (char *)xalloc( len + str_len( name ) + 2 );

    str_cpy( path, dir );
    if ( len == 0 || path[len - 1] != '\\' ) {
        path[len++] = '\\';
    }
    str_cpy( path + len, name );
    return path;
}

/* ------------------------------------------------------------------ */
/* heap: first-fit free list on top of blocks from sys_mem_alloc()     */
/* ------------------------------------------------------------------ */

typedef struct HBLK {
    u32          size;          /* whole block including this header */
    struct HBLK *next;          /* free list link (free blocks only) */
} HBLK;

#define HDR         8u
#define MIN_SPLIT   64u
#define CHUNK       (64u * 1024u)

static HBLK *free_list;

static void heap_insert( HBLK *blk )
{
    HBLK **link = &free_list;
    HBLK  *prev = NULL;

    while ( *link && *link < blk ) {
        prev = *link;
        link = &(*link)->next;
    }
    blk->next = *link;
    *link = blk;
    if ( blk->next && (u8 *)blk + blk->size == (u8 *)blk->next ) {
        blk->size += blk->next->size;
        blk->next = blk->next->next;
    }
    if ( prev && (u8 *)prev + prev->size == (u8 *)blk ) {
        prev->size += blk->size;
        prev->next = blk->next;
    }
}

void *try_alloc( u32 size )
{
    HBLK **link, *blk, *rest;
    u32 need, chunk;
    void *mem;
    int pass;

    if ( size > 0xFFFFFF00UL - HDR ) {
        return NULL;
    }
    need = (size + HDR + 7u) & ~7u;
    for ( pass = 0; pass < 2; pass++ ) {
        for ( link = &free_list; (blk = *link) != NULL; link = &blk->next ) {
            if ( blk->size < need ) {
                continue;
            }
            if ( blk->size - need >= MIN_SPLIT ) {
                rest = (HBLK *)((u8 *)blk + need);
                rest->size = blk->size - need;
                rest->next = blk->next;
                *link = rest;
                blk->size = need;
            } else {
                *link = blk->next;
            }
            return (u8 *)blk + HDR;
        }
        if ( pass ) {
            break;
        }
        chunk = need > CHUNK ? need : CHUNK;
        mem = sys_mem_alloc( chunk );
        if ( mem == NULL && chunk > need ) {
            chunk = need;
            mem = sys_mem_alloc( chunk );
        }
        if ( mem == NULL ) {
            return NULL;
        }
        blk = (HBLK *)mem;
        blk->size = chunk & ~7u;
        heap_insert( blk );
    }
    return NULL;
}

void *xalloc( u32 size )
{
    void *ptr = try_alloc( size );

    if ( ptr == NULL ) {
        out_of_memory();
    }
    return ptr;
}

void *xzalloc( u32 size )
{
    void *ptr = xalloc( size );

    mem_set( ptr, 0, size );
    return ptr;
}

void xfree( void *ptr )
{
    if ( ptr != NULL ) {
        heap_insert( (HBLK *)((u8 *)ptr - HDR) );
    }
}

void list_add( U32LIST *list, u32 val )
{
    u32 *grown;

    if ( list->count == list->cap ) {
        list->cap = list->cap ? list->cap * 2 : 16;
        grown = (u32 *)xalloc( list->cap * sizeof( u32 ) );
        if ( list->count ) {
            mem_cpy( grown, list->items, list->count * sizeof( u32 ) );
        }
        xfree( list->items );
        list->items = grown;
    }
    list->items[list->count++] = val;
}

void list_free( U32LIST *list )
{
    xfree( list->items );
    list->items = NULL;
    list->count = list->cap = 0;
}

/* ------------------------------------------------------------------ */
/* lines of text                                                       */
/* ------------------------------------------------------------------ */

void tx_init( TEXT *text )
{
    text->line = NULL;
    text->count = text->cap = 0;
}

void tx_free( TEXT *text )
{
    int index;

    for ( index = 0; index < text->count; index++ ) {
        xfree( text->line[index] );
    }
    xfree( text->line );
    tx_init( text );
}

void tx_add( TEXT *text, const char *line )
{
    char **grown;

    if ( text->count == text->cap ) {
        text->cap = text->cap ? text->cap * 2 : 32;
        grown = (char **)xalloc( (u32)text->cap * sizeof( char * ) );
        if ( text->count ) {
            mem_cpy( grown, text->line, (u32)text->count * sizeof( char * ) );
        }
        xfree( text->line );
        text->line = grown;
    }
    text->line[text->count++] = str_dup( line );
}

void __cdecl tx_addf( TEXT *text, const char *fmt, ... )
{
    char line[400];

    vfmt( line, sizeof( line ), fmt, VA_START( fmt ) );
    tx_add( text, line );
}

void tx_wrap( TEXT *text, const char *para, int width )
{
    char line[200];
    int len, cut;

    if ( width > (int)sizeof( line ) - 1 ) {
        width = sizeof( line ) - 1;
    }
    for ( ;; ) {
        for ( len = 0; para[len] && para[len] != '\n'; len++ ) {
        }
        if ( len > width ) {
            for ( cut = width; cut > 0 && para[cut] != ' '; cut-- ) {
            }
            if ( cut == 0 ) {
                cut = width;                /* one word wider than the line */
            }
            mem_cpy( line, para, (u32)cut );
            line[cut] = 0;
            tx_add( text, line );
            para += cut;
            while ( *para == ' ' ) {
                para++;
            }
            continue;
        }
        mem_cpy( line, para, (u32)len );
        line[len] = 0;
        tx_add( text, line );
        para += len;
        if ( *para == 0 ) {
            break;
        }
        para++;
    }
}

/* ------------------------------------------------------------------ */
/* 64-bit helpers                                                      */
/* ------------------------------------------------------------------ */

u64 mul32( u32 lhs, u32 rhs )
{
    U64 prod;
    u32 al = lhs & 0xFFFFu, ah = lhs >> 16;
    u32 bl = rhs & 0xFFFFu, bh = rhs >> 16;
    u32 p0 = al * bl, p1 = al * bh, p2 = ah * bl, p3 = ah * bh;
    u32 mid = p1 + p2;
    u32 cmid = (mid < p1) ? 0x10000u : 0;
    u32 lo = p0 + (mid << 16);
    u32 clo = (lo < p0) ? 1u : 0;

    prod.parts.lo = lo;
    prod.parts.hi = p3 + (mid >> 16) + cmid + clo;
    return prod.whole;
}

u32 div64( u64 *num, u32 divisor )
{
    U64 split;
    u32 rem, part, qh, ql;

    split.whole = *num;
    qh = split.parts.hi / divisor;
    rem = split.parts.hi % divisor;
    part = (rem << 16) | (split.parts.lo >> 16);
    ql = (part / divisor) << 16;
    rem = part % divisor;
    part = (rem << 16) | (split.parts.lo & 0xFFFFu);
    ql |= part / divisor;
    rem = part % divisor;
    split.parts.hi = qh;
    split.parts.lo = ql;
    *num = split.whole;
    return rem;
}

u32 lo32( u64 val )
{
    U64 split;

    split.whole = val;
    return split.parts.lo;
}

u32 hi32( u64 val )
{
    U64 split;

    split.whole = val;
    return split.parts.hi;
}

/* ------------------------------------------------------------------ */
/* sfmt                                                                */
/* ------------------------------------------------------------------ */

static int group_ch = ',';

void fmt_group_set( int ch )
{
    group_ch = ch;
}

char *fmt_bytes( u64 val, char *buf )
{
    char tmp[40];
    int len = 0, digits = 0;
    char *cur = buf;

    do {
        if ( digits && digits % 3 == 0 && group_ch ) {
            tmp[len++] = (char)group_ch;
        }
        tmp[len++] = (char)('0' + div64( &val, 10 ));
        digits++;
    } while ( val != 0 );
    while ( len ) {
        *cur++ = tmp[--len];
    }
    *cur = 0;
    return buf;
}

typedef struct {
    char *buf;
    u32   max, len;
} FOUT;

static void out_ch( FOUT *out, int ch )
{
    if ( out->len + 1 < out->max ) {
        out->buf[out->len++] = (char)ch;
    }
}

static void out_pad( FOUT *out, int count, int ch )
{
    while ( count-- > 0 ) {
        out_ch( out, ch );
    }
}

static void out_str( FOUT *out, const char *text, int len, int width, int left, int fill )
{
    int index;

    if ( !left ) {
        out_pad( out, width - len, fill );
    }
    for ( index = 0; index < len; index++ ) {
        out_ch( out, text[index] );
    }
    if ( left ) {
        out_pad( out, width - len, ' ' );
    }
}

/* a number into "digits", backwards; returns how many characters */
static int num_digits( u32 val, u32 base, int upper, int group, char *digits )
{
    static const char lower_set[] = "0123456789abcdef";
    static const char upper_set[] = "0123456789ABCDEF";
    int count = 0, run = 0;

    do {
        if ( group && run == 3 ) {
            digits[count++] = (char)group_ch;
            run = 0;
        }
        digits[count++] = upper ? upper_set[val % base] : lower_set[val % base];
        val /= base;
        run++;
    } while ( val );
    return count;
}

int vfmt( char *buf, u32 max, const char *fmt, char *args )
{
    FOUT out;
    char digits[20], text[20];
    const char *str;
    int left, zero, group, width, count, index, neg;
    u32 val;

    out.buf = buf;
    out.max = max;
    out.len = 0;
    if ( max == 0 ) {
        return 0;
    }
    for ( ; *fmt; fmt++ ) {
        if ( *fmt != '%' ) {
            out_ch( &out, *fmt );
            continue;
        }
        fmt++;
        left = zero = group = width = 0;
        for ( ;; fmt++ ) {
            if ( *fmt == '-' ) {
                left = 1;
            } else if ( *fmt == '0' ) {
                zero = 1;
            } else if ( *fmt == ',' ) {
                group = 1;
            } else {
                break;
            }
        }
        while ( *fmt >= '0' && *fmt <= '9' ) {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if ( *fmt == 'l' ) {
            fmt++;
        }
        switch ( *fmt ) {
        case 's':
            str = VA_NEXT( args, const char * );
            if ( str == NULL ) {
                str = "";
            }
            out_str( &out, str, (int)str_len( str ), width, left, ' ' );
            break;
        case 'c':
            text[0] = (char)VA_NEXT( args, int );
            out_str( &out, text, 1, width, left, ' ' );
            break;
        case 'd':
        case 'u':
        case 'x':
        case 'X':
            val = VA_NEXT( args, u32 );
            neg = 0;
            if ( *fmt == 'd' && (s32)val < 0 ) {
                neg = 1;
                val = (u32)(-(s32)val);
            }
            count = num_digits( val, *fmt == 'x' || *fmt == 'X' ? 16 : 10, *fmt == 'X',
                                group && *fmt != 'x' && *fmt != 'X', digits );
            index = 0;
            if ( neg ) {
                text[index++] = '-';
            }
            if ( zero && !left ) {
                while ( index + count < width && index < 12 ) {
                    text[index++] = '0';
                }
            }
            while ( count && index < (int)sizeof( text ) ) {
                text[index++] = digits[--count];
            }
            out_str( &out, text, index, width, left, ' ' );
            break;
        case 0:
            fmt--;
            break;
        default:
            out_ch( &out, *fmt );
            break;
        }
    }
    out.buf[out.len] = 0;
    return (int)out.len;
}

int __cdecl sfmt( char *buf, u32 max, const char *fmt, ... )
{
    return vfmt( buf, max, fmt, VA_START( fmt ) );
}
