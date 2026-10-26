/*
 * SCAN.C - MODE's command line, split the way MS-DOS's parser split it.
 *
 * MS-DOS's MODE ran its command line through the system parser
 * (PARSE.ASM), and what a batch file may write follows from that
 * parser's rules more than from MODE's documentation:
 *
 *   - Blanks next to an '=' do not count: "BAUD = 96" is "BAUD=96".
 *   - A blank, a tab, a comma or a semicolon ends a word.  Blanks run
 *     together, but a comma is HARD: two of them, with or without
 *     blanks between, have an empty field between them, which is how
 *     "COM1 96,,,,P" says "the retry, and defaults for the rest".  A
 *     comma just before the end makes no empty field.
 *   - A colon is a delimiter only straight after the device name: the
 *     first word is read again with it once the plain reading has not
 *     found a form (MAIN.C), which is why "COM1:96" works and "MODE
 *     /STA COM1:" does not.
 *   - '/' begins a switch, and ends the word in front of it.
 *   - A word with an '=' in it is a keyword, named up to the '='.
 *   - Words are compared whole, in either case.
 *
 * The piece of the line an error names is the item from its first
 * character to the one before where the parser would look next, and
 * to the end of the line when nothing follows - which is how 6.22 came
 * to show "Invalid parameter - XYZ " with the blank a batch file's
 * redirection left behind it.
 */
#include "mode.h"

#define LINE_MAX    160

static char line[LINE_MAX];

static int is_blank( int ch )
{
    return ch == ' ' || ch == '\t';
}

/* a copy of the tail, with the blanks round each '=' taken out */
void line_init( const char *tail )
{
    u32 out = 0;
    const char *src = tail;

    while ( *src && *src != '\r' && *src != '\n' && out + 1 < LINE_MAX ) {
        if ( *src == '=' ) {
            while ( out && is_blank( line[out - 1] ) ) {
                out--;
            }
            line[out++] = '=';
            src++;
            while ( is_blank( *src ) ) {
                src++;
            }
            continue;
        }
        line[out++] = *src++;
    }
    line[out] = 0;
}

const char *line_text( void )
{
    return line;
}

void scan_start( SCAN *sc )
{
    sc->pos = line;
}

static int is_hard( int ch, int colon )
{
    return ch == ',' || ch == ';' || (colon && ch == ':');
}

/* where a word ends: a blank, a hard delimiter, a switch or the end */
static int word_end( int ch, int colon )
{
    return ch == 0 || is_blank( ch ) || is_hard( ch, colon ) || ch == '/';
}

int scan_next( SCAN *sc, ITEM *it, int colon )
{
    const char *pos = sc->pos;
    const char *eq;

    while ( is_blank( *pos ) ) {
        pos++;
    }
    if ( *pos == 0 ) {
        sc->pos = pos;
        return 0;
    }
    mem_set( it, 0, sizeof( *it ) );
    it->text = pos;

    if ( is_hard( *pos, colon ) ) {
        it->kind = IT_EMPTY;
        it->resume = pos + 1;
        sc->pos = it->resume;
        return 1;
    }

    if ( *pos == '/' ) {
        it->kind = IT_SWITCH;
        pos++;
    } else {
        it->kind = IT_POS;
    }
    while ( !word_end( *pos, colon ) ) {
        pos++;
    }
    it->len = (u32)(pos - it->text);

    if ( it->kind == IT_POS ) {
        for ( eq = it->text; eq < pos; eq++ ) {
            if ( *eq == '=' ) {
                it->kind = IT_KEY;
                it->value = eq + 1;
                it->vlen = (u32)(pos - it->value);
                break;
            }
        }
    }

    it->comma = (*pos == ',');
    while ( is_blank( *pos ) ) {
        pos++;
    }
    if ( is_hard( *pos, colon ) ) {
        pos++;
    }
    it->resume = pos;
    sc->pos = pos;
    return 1;
}

int scan_more( const SCAN *sc )
{
    const char *pos = sc->pos;

    while ( is_blank( *pos ) ) {
        pos++;
    }
    return *pos != 0;
}

/* ------------------------------------------------------------------ */
/* comparing                                                           */
/* ------------------------------------------------------------------ */

static int same( const char *text, u32 len, const char *word )
{
    return str_len( word ) == len && str_nicmp( text, word, len ) == 0;
}

int is_word( const ITEM *it, const char *word )
{
    return it->kind == IT_POS && same( it->text, it->len, word );
}

int is_key( const ITEM *it, const char *name )
{
    return it->kind == IT_KEY && same( it->text, (u32)(it->value - it->text), name );
}

int val_is( const ITEM *it, const char *word )
{
    return same( it->value, it->vlen, word );
}

int val_num( const ITEM *it, u32 *num )
{
    u32 pos, val = 0;

    if ( it->vlen == 0 ) {
        return 1;
    }
    for ( pos = 0; pos < it->vlen; pos++ ) {
        if ( it->value[pos] < '0' || it->value[pos] > '9' || val > 99999999UL ) {
            return 1;
        }
        val = val * 10 + (u32)(it->value[pos] - '0');
    }
    *num = val;
    return 0;
}

int is_status( const ITEM *it )
{
    return it->kind == IT_SWITCH &&
           (same( it->text, it->len, "/STA" ) || same( it->text, it->len, "/STAT" ) ||
            same( it->text, it->len, "/STATUS" ));
}

int is_help( const ITEM *it )
{
    return it->kind == IT_SWITCH && same( it->text, it->len, "/?" );
}

/* ------------------------------------------------------------------ */
/* the errors that name a piece of the line                            */
/* ------------------------------------------------------------------ */

#define SHOW_MAX    80

/* the item as the error shows it, into SHOW_MAX bytes */
static void show_text( const ITEM *it, char *buf )
{
    const char *end;
    u32 len;

    if ( it->kind == IT_EMPTY ) {
        buf[0] = 0;
        return;
    }
    end = it->resume;
    if ( *end != 0 ) {
        end--;
    }
    if ( end < it->text + it->len ) {
        end = it->text + it->len;
    }
    len = (u32)(end - it->text);
    if ( len > SHOW_MAX - 1 ) {
        len = SHOW_MAX - 1;
    }
    mem_cpy( buf, it->text, len );
    buf[len] = 0;
}

/* "Invalid parameter - " and the text, a blank when there is none */
static int bad_with( const char *tmpl, const char *text )
{
    out_text( "\n" );
    out_line( tmpl, text[0] ? text : " ", NULL );
    failed = 1;
    return EXIT_ERROR;
}

int err_param( const ITEM *it )
{
    char buf[SHOW_MAX];

    show_text( it, buf );
    return bad_with( M_BADPARAM, buf );
}

int err_param_cut( const ITEM *it )
{
    char buf[SHOW_MAX];
    u32 pos;

    show_text( it, buf );
    for ( pos = 0; buf[pos]; pos++ ) {
        if ( buf[pos] == '.' || buf[pos] == '"' || buf[pos] == '\\' || buf[pos] == '[' ||
             buf[pos] == ']' || buf[pos] == '+' ) {
            buf[pos] = 0;
            break;
        }
    }
    return bad_with( M_BADPARAM, buf );
}

int err_param_none( void )
{
    return bad_with( M_BADPARAM, "" );
}

int err_param_str( const char *text )
{
    return bad_with( M_BADPARAM, text );
}

int err_switch( const ITEM *it )
{
    char buf[SHOW_MAX];

    show_text( it, buf );
    return bad_with( M_BADSWITCH, buf );
}

int err_format( const ITEM *it )
{
    char buf[SHOW_MAX];

    show_text( it, buf );
    return bad_with( M_BADFORMAT, buf );
}

int err_count( void )
{
    return err_line( M_BADCOUNT );
}

int err_line( const char *msg )
{
    out_line( msg, NULL, NULL );
    failed = 1;
    return EXIT_ERROR;
}
