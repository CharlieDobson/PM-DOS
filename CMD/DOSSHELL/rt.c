/*
 * RT.C - minimal runtime for DOSSHELL (no C library is linked).  The
 * heap is EDIT's: first fit over blocks bought from 48h.
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

    if ( len + 1 > max ) {
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

/* a copy that takes what there is room for and drops the rest */
void str_fit( char *dst, const char *src, u32 max )
{
    u32 len = str_len( src );

    if ( max == 0 ) {
        return;
    }
    if ( len > max - 1 ) {
        len = max - 1;
    }
    mem_cpy( dst, src, len );
    dst[len] = 0;
}

int ch_upper( int ch )
{
    ch &= 0xFF;
    if ( ch >= 'a' && ch <= 'z' ) {
        ch -= 'a' - 'A';
    }
    return ch;
}

void str_upper( char *str )
{
    for ( ; *str; str++ ) {
        *str = (char)ch_upper( *str );
    }
}

int str_cmp( const char *lhs, const char *rhs )
{
    while ( *lhs && *lhs == *rhs ) {
        lhs++;
        rhs++;
    }
    return (int)(u8)*lhs - (int)(u8)*rhs;
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

char *str_rchr( const char *str, int ch )
{
    const char *found = NULL;

    for ( ; *str; str++ ) {
        if ( *str == (char)ch ) {
            found = str;
        }
    }
    return (char *)found;
}

char *str_dup( const char *str )
{
    u32 len = str_len( str ) + 1;
    char *copy = (char *)xalloc( len );

    mem_cpy( copy, str, len );
    return copy;
}

/* ------------------------------------------------------------------ */
/* numbers                                                             */
/* ------------------------------------------------------------------ */

static char group_sep = ',';

void fmt_group_set( int sep )
{
    group_sep = (char)sep;
}

static char *fmt_any( u32 val, int width, char *buf, int grouped )
{
    char digits[16];
    int count = 0, pos = 0, done = 0;

    do {
        if ( grouped && done && done % 3 == 0 ) {
            digits[count++] = group_sep;
        }
        digits[count++] = (char)('0' + val % 10);
        val /= 10;
        done++;
    } while ( val );
    while ( width-- > count ) {
        buf[pos++] = ' ';
    }
    while ( count ) {
        buf[pos++] = digits[--count];
    }
    buf[pos] = 0;
    return buf;
}

char *fmt_dec( u32 val, int width, char *buf )
{
    return fmt_any( val, width, buf, 0 );
}

char *fmt_num( u32 val, int width, char *buf )
{
    return fmt_any( val, width, buf, 1 );
}

char *fmt_dec2( u32 val, char *buf )
{
    buf[0] = (char)('0' + (val / 10) % 10);
    buf[1] = (char)('0' + val % 10);
    buf[2] = 0;
    return buf;
}

/* ------------------------------------------------------------------ */
/* file names                                                          */
/* ------------------------------------------------------------------ */

const char *path_name( const char *path )
{
    const char *name = path;

    for ( ; *path; path++ ) {
        if ( *path == '\\' || *path == '/' || *path == ':' ) {
            name = path + 1;
        }
    }
    return name;
}

const char *path_ext( const char *path )
{
    const char *name = path_name( path );
    const char *dot = str_rchr( name, '.' );

    return dot ? dot + 1 : name + str_len( name );
}

/* "dir" and "name" with one backslash between them: 0 if too long */
int path_join( char *dst, const char *dir, const char *name, u32 max )
{
    u32 len;

    if ( !str_cpyn( dst, dir, max ) ) {
        return 0;
    }
    len = str_len( dst );
    if ( len && dst[len - 1] != '\\' && dst[len - 1] != ':' ) {
        if ( !str_catn( dst, "\\", max ) ) {
            return 0;
        }
    }
    return str_catn( dst, name, max );
}

/* DOS's wildcards, on the name as it is spelled: '?' is any one
   character, '*' any run of them, and "*.*" matches a name with no
   dot in it too, as it does for DIR */
static int wild_part( const char *pattern, const char *name )
{
    for ( ;; ) {
        if ( *pattern == '*' ) {
            pattern++;
            if ( *pattern == 0 ) {
                return 1;
            }
            for ( ; *name; name++ ) {
                if ( wild_part( pattern, name ) ) {
                    return 1;
                }
            }
            return wild_part( pattern, name );
        }
        if ( *pattern == 0 ) {
            return *name == 0;
        }
        if ( *name == 0 ) {
            return 0;
        }
        if ( *pattern != '?' && ch_upper( *pattern ) != ch_upper( *name ) ) {
            return 0;
        }
        pattern++;
        name++;
    }
}

int wild_match( const char *pattern, const char *name )
{
    u32 len = str_len( pattern );
    char base[80];

    if ( wild_part( pattern, name ) ) {
        return 1;
    }
    /* "X.*" and "*.*" take a name with no extension */
    if ( len >= 2 && len < sizeof( base ) && pattern[len - 2] == '.' && pattern[len - 1] == '*'
         && str_chr( name, '.' ) == NULL ) {
        mem_cpy( base, pattern, len - 2 );
        base[len - 2] = 0;
        return wild_part( base, name );
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* heap: first-fit free list on top of blocks from sys_mem_alloc()     */
/* ------------------------------------------------------------------ */

typedef struct HBLK {
    u32          size;          /* whole block including this header */
    struct HBLK *next;          /* free list link (free blocks only) */
} HBLK;

#define HDR         8u
#define MIN_SPLIT   32u
#define CHUNK       (16u * 1024u)

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

/* Only for what the program cannot go on without. */
void *xalloc( u32 size )
{
    void *ptr = try_alloc( size );

    if ( ptr == NULL ) {
        out_of_memory();
        sys_exit( 8 );
    }
    return ptr;
}

void xfree( void *ptr )
{
    if ( ptr != NULL ) {
        heap_insert( (HBLK *)((u8 *)ptr - HDR) );
    }
}
