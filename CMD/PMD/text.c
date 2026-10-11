/*
 * TEXT.C - a page as lines of text, and reading what the ROMs say.
 *
 * EVERY PAGE IS BUILT AS TEXT FIRST, whoever it is for: the window on
 * the screen shows the lines and the report writes them, so the two
 * cannot disagree, and a page is one routine that knows nothing about
 * either.  A line that could not be stored for want of memory sets
 * "failed", and what there is of the page is still shown.
 *
 * THE ROMs are read into a buffer with 21h/F1h AL=20h and searched
 * there.  What is looked for is what MSD looks for: a maker's name, a
 * string with a version number in it, and the latest thing that reads
 * as a date.
 */
#include "pmd.h"

void tx_init( TEXT *text )
{
    mem_set( text, 0, sizeof( *text ) );
}

void tx_free( TEXT *text )
{
    int index;

    for ( index = 0; index < text->count; index++ ) {
        xfree( text->line[index] );
    }
    xfree( text->line );
    mem_set( text, 0, sizeof( *text ) );
}

void tx_add( TEXT *text, const char *line )
{
    char **grown, *copy;
    u32 len = str_len( line );
    int cap;

    while ( len && line[len - 1] == ' ' ) {     /* nothing ends in blanks */
        len--;
    }
    if ( text->count == text->cap ) {
        cap = text->cap ? text->cap * 2 : 32;
        grown = (char **)try_alloc( (u32)cap * sizeof( char * ) );
        if ( grown == NULL ) {
            text->failed = 1;
            return;
        }
        if ( text->count ) {
            mem_cpy( grown, text->line, (u32)text->count * sizeof( char * ) );
        }
        xfree( text->line );
        text->line = grown;
        text->cap = cap;
    }
    copy = (char *)try_alloc( len + 1 );
    if ( copy == NULL ) {
        text->failed = 1;
        return;
    }
    mem_cpy( copy, line, len );
    copy[len] = 0;
    text->line[text->count++] = copy;
}

void __cdecl tx_addf( TEXT *text, const char *fmt, ... )
{
    char line[200];

    vfmt( line, sizeof( line ), fmt, VA_START( fmt ) );
    tx_add( text, line );
}

int tx_width( const TEXT *text )
{
    int index, width = 0, len;

    for ( index = 0; index < text->count; index++ ) {
        len = (int)str_len( text->line[index] );
        if ( len > width ) {
            width = len;
        }
    }
    return width;
}

void tx_field( TEXT *text, int label_width, const char *label, const char *value )
{
    tx_addf( text, "%*s%s", label_width, label, value );
}

void __cdecl tx_fieldf( TEXT *text, int label_width, const char *label, const char *fmt, ... )
{
    char value[160];

    vfmt( value, sizeof( value ), fmt, VA_START( fmt ) );
    tx_field( text, label_width, label, value );
}

/* ------------------------------------------------------------------ */
/* the ROMs                                                            */
/* ------------------------------------------------------------------ */

u8 *rom_load( u32 addr, u32 len )
{
    u8 *rom = (u8 *)try_alloc( len + 1 );
    u32 done, part;

    if ( rom == NULL ) {
        return NULL;
    }
    for ( done = 0; done < len; done += part ) {
        part = len - done > 0x4000 ? 0x4000 : len - done;
        sys_peek( addr + done, rom + done, part );
    }
    rom[len] = 0;
    return rom;
}

static int is_letter( int ch )
{
    ch = ch_upper( ch );
    return ch >= 'A' && ch <= 'Z';
}

/* "name" in rom[from..to), standing as a word of its own - "ATI" is
   not in "compatible" - and not the first word of "IBM compatible",
   which every clone's BIOS says of itself */
static int name_in( const u8 *rom, u32 len, u32 from, u32 to, const char *name )
{
    u32 nlen = str_len( name ), pos = from;
    int at;

    while ( nlen && name[nlen - 1] == ' ' ) {
        nlen--;
    }
    while ( pos < to ) {
        at = mem_ifind( rom + pos, to - pos, name );
        if ( at < 0 ) {
            return 0;
        }
        pos += (u32)at;
        if ( (pos == 0 || !is_letter( rom[pos - 1] ))
             && (pos + nlen >= len || !is_letter( rom[pos + nlen] ))
             && (pos + nlen + 11 > len || mem_ifind( rom + pos + nlen, 11, " compatible" ) != 0) ) {
            return 1;
        }
        pos++;
    }
    return 0;
}

/* A short name - three letters or fewer, "AMI" - turns up by chance in
   64K of code, so it is believed only near the start of a 4K block,
   where a BIOS keeps its copyright lines.  A long one is believed
   anywhere. */
int rom_find_name( const u8 *rom, u32 len, const char *const *names )
{
    int index;
    u32 block, span;

    for ( index = 0; names[index]; index++ ) {
        if ( str_len( names[index] ) > 3 ) {
            if ( name_in( rom, len, 0, len, names[index] ) ) {
                return index;
            }
            continue;
        }
        for ( block = 0; block < len; block += 0x1000 ) {
            span = len - block > 0x200 ? 0x200 : len - block;
            if ( name_in( rom, len, block, block + span, names[index] ) ) {
                return index;
            }
        }
    }
    return -1;
}

static int printable( int ch )
{
    return ch >= ' ' && ch <= 126 && ch != '$';
}

static int is_digit( int ch )
{
    return ch >= '0' && ch <= '9';
}

/* does this text call itself a version: Ver, Rev, Rel, or a v and a
   digit */
static int names_version( const u8 *text, u32 len )
{
    u32 pos;

    if ( mem_ifind( text, len, "Ver" ) >= 0 || mem_ifind( text, len, "Rev" ) >= 0
         || mem_ifind( text, len, "Rel" ) >= 0 ) {
        return 1;
    }
    for ( pos = 0; pos + 1 < len; pos++ ) {
        if ( ch_upper( text[pos] ) != 'V' ) {
            continue;
        }
        if ( is_digit( text[pos + 1] )
             || (text[pos + 1] == ' ' && pos + 2 < len && is_digit( text[pos + 2] )) ) {
            return 1;
        }
    }
    return 0;
}

/* Up to three strings that hold a version number: a full stop with a
   digit on each side of it, in readable text that says "version" one
   way or another.  Returns how many. */
int rom_versions( const u8 *rom, u32 len, char found[3][64] )
{
    u32 pos, start, end, count;
    int have = 0, same;

    found[0][0] = found[1][0] = found[2][0] = 0;
    for ( pos = 1; pos + 1 < len && have < 3; pos++ ) {
        if ( rom[pos] != '.' || !is_digit( rom[pos - 1] ) || !is_digit( rom[pos + 1] ) ) {
            continue;
        }
        for ( start = pos; start > 0 && pos - start < 56 && printable( rom[start - 1] ); start-- ) {
        }
        for ( end = pos; end < len && printable( rom[end] ); end++ ) {
        }
        if ( names_version( rom + start, end - start ) ) {
            while ( start < end && rom[start] == ' ' ) {
                start++;
            }
            count = end - start > 63 ? 63 : end - start;
            mem_cpy( found[have], rom + start, count );
            found[have][count] = 0;
            /* a BIOS that says the same thing twice is quoted once */
            for ( same = 0; same < have && str_cmp( found[same], found[have] ) != 0; same++ ) {
            }
            if ( same == have ) {
                have++;
            } else {
                found[have][0] = 0;
            }
        }
        pos = end;                      /* one string, one answer */
    }
    return have;
}

/* The latest "nn/nn/nn" there is, as mm/dd/yy.  Two digits of year: 80
   and up are taken for the 1900s. */
int rom_date( const u8 *rom, u32 len, char *date )
{
    u32 pos, best = 0, stamp;
    int year;

    date[0] = 0;
    for ( pos = 2; pos + 6 <= len; pos++ ) {
        if ( rom[pos] != '/' || rom[pos + 3] != '/' ) {
            continue;
        }
        if ( !is_digit( rom[pos - 2] ) || !is_digit( rom[pos - 1] ) || !is_digit( rom[pos + 1] )
             || !is_digit( rom[pos + 2] ) || !is_digit( rom[pos + 4] ) || !is_digit( rom[pos + 5] ) ) {
            continue;
        }
        year = (rom[pos + 4] - '0') * 10 + (rom[pos + 5] - '0');
        year += year < 80 ? 2000 : 1900;
        stamp = (u32)year * 10000 + (u32)((rom[pos - 2] - '0') * 10 + (rom[pos - 1] - '0')) * 100
                + (u32)((rom[pos + 1] - '0') * 10 + (rom[pos + 2] - '0'));
        if ( stamp > best ) {
            best = stamp;
            mem_cpy( date, rom + pos - 2, 8 );
            date[8] = 0;
        }
    }
    return best != 0;
}

static int in_string( int ch )
{
    return (ch >= ' ' && ch <= 126 && ch != '$' && ch != '@') || ch == '\r' || ch == '\n'
           || ch == '\t';
}

/* The readable text round rom[at], for the browser: back to where it
   starts - 192 bytes at most - and on to where it ends, 240 at most,
   with the blank space at either end left off. */
u32 rom_string( const u8 *rom, u32 len, u32 at, u32 *length )
{
    u32 start = at, end = at;

    while ( start > 0 && at - start < 192 && in_string( rom[start - 1] ) ) {
        start--;
    }
    while ( start < at && (rom[start] <= ' ') ) {
        start++;
    }
    while ( end < len && end - start < 240 && in_string( rom[end] ) ) {
        end++;
    }
    while ( end > start && rom[end - 1] <= ' ' ) {
        end--;
    }
    *length = end - start;
    return start;
}
