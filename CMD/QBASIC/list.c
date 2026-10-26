/*
 * LIST.C - a line as QBasic shows it once the cursor has left it.
 *
 * Reserved words in capitals, every name in the case it was last typed
 * in (one spelling for the whole program), numbers in their shortest
 * form with the suffix their type needs (1.0 -> 1!, 1E3 -> 1000!,
 * &h1f -> &H1F), one blank round an operator and after a comma, a
 * semicolon or a colon.  A remark, DATA and a string stay exactly as
 * typed, and so does the indentation.
 *
 * What the lexer cannot tell - a minus that is a sign, LINE's "-(",
 * DEFINT's "A-Z" - the compiler has marked on the tokens while it read
 * the line (TF_UNARY, TF_TIGHT); this file goes by the marks.
 */
#include "qb.h"

/* ------------------------------------------------------------------ */
/* the spellings                                                       */
/* ------------------------------------------------------------------ */

typedef struct NAME {
    struct NAME *next;
    u8   len;
    char text[NAME_MAX + 1];    /* as last typed */
} NAME;

#define NBUCKETS    256

static NAME *names[NBUCKETS];

static u32 hash_name( const char *name, u32 len )
{
    u32 hash = 0, i;

    for ( i = 0; i < len; i++ ) {
        hash = hash * 31 + (u32)ch_upper( name[i] );
    }
    return hash % NBUCKETS;
}

void names_reset( void )
{
    NAME *cur, *next;
    int i;

    for ( i = 0; i < NBUCKETS; i++ ) {
        for ( cur = names[i]; cur != NULL; cur = next ) {
            next = cur->next;
            xfree( cur );
        }
        names[i] = NULL;
    }
}

static NAME *find_name( const char *name, u32 len )
{
    NAME *cur;

    for ( cur = names[hash_name( name, len )]; cur != NULL; cur = cur->next ) {
        if ( cur->len == len && str_nicmp( cur->text, name, len ) == 0 ) {
            return cur;
        }
    }
    return NULL;
}

/* -> 1 if the spelling changed (or the name is new) */
int names_learn( const char *name, u32 len )
{
    NAME *cur;
    u32 hash;

    if ( len == 0 || len > NAME_MAX ) {
        return 0;
    }
    cur = find_name( name, len );
    if ( cur != NULL ) {
        if ( mem_cmp( cur->text, name, len ) == 0 ) {
            return 0;
        }
        mem_cpy( cur->text, name, len );
        return 1;
    }
    cur = (NAME *)try_alloc( sizeof( NAME ) );
    if ( cur == NULL ) {
        return 0;
    }
    hash = hash_name( name, len );
    cur->len = (u8)len;
    mem_cpy( cur->text, name, len );
    cur->text[len] = 0;
    cur->next = names[hash];
    names[hash] = cur;
    return 0;
}

const char *names_spelling( const char *name, u32 len )
{
    NAME *cur = find_name( name, len );

    return cur ? cur->text : NULL;
}

/* ------------------------------------------------------------------ */
/* the line                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char *out;
    u32   len, max;
    int   cut;                  /* something did not fit */
} OUTBUF;

static void put( OUTBUF *buf, const char *text, u32 len )
{
    while ( len > 0 && buf->len + 1 < buf->max ) {
        buf->out[buf->len++] = *text++;
        len--;
    }
    if ( len > 0 ) {
        buf->cut = 1;
    }
    buf->out[buf->len] = 0;
}

static void put_ch( OUTBUF *buf, int ch )
{
    char one = (char)ch;

    put( buf, &one, 1 );
}

static void put_cstr( OUTBUF *buf, const char *text )
{
    put( buf, text, str_len( text ) );
}

/* a name, a part at a time between its dots, each in its own spelling */
static void put_name( OUTBUF *buf, const char *text, u32 len )
{
    u32 start = 0, end;
    const char *spell;

    while ( start < len ) {
        end = start;
        while ( end < len && text[end] != '.' ) {
            end++;
        }
        spell = names_spelling( text + start, end - start );
        put( buf, spell ? spell : text + start, end - start );
        if ( end < len ) {
            put_ch( buf, '.' );
        }
        start = end + 1;
    }
}

static const char hexdig[] = "0123456789ABCDEF";

static void put_radix( OUTBUF *buf, TOKEN *tok )
{
    char digits[16];
    u32 val, mask;
    int count = 0, hex = (tok->flags & TF_HEX) != 0;

    if ( tok->ntype == TY_LONG ) {
        val = (u32)(s32)tok->num;
        mask = 0xFFFFFFFFUL;
    } else {
        val = (u32)(u16)(s16)(s32)tok->num;
        mask = 0xFFFFUL;
    }
    val &= mask;
    do {
        if ( hex ) {
            digits[count++] = hexdig[val & 15];
            val >>= 4;
        } else {
            digits[count++] = hexdig[val & 7];
            val >>= 3;
        }
    } while ( val );
    put_cstr( buf, hex ? "&H" : "&O" );
    while ( count ) {
        put_ch( buf, digits[--count] );
    }
    if ( tok->ntype == TY_LONG && (u32)(s32)tok->num <= 0xFFFFUL ) {
        put_ch( buf, '&' );
    }
}

/* the keywords that take ( straight after them: functions, and the
   statements whose number goes in parentheses (KEY(n) ON) */
static int tight_paren( int kw )
{
    switch ( kw ) {
    case KW_ABS: case KW_ASC: case KW_ATN: case KW_CDBL: case KW_CHR_S:
    case KW_CINT: case KW_CLNG: case KW_COS: case KW_CSNG: case KW_CVD:
    case KW_CVDMBF: case KW_CVI: case KW_CVL: case KW_CVS: case KW_CVSMBF:
    case KW_ENVIRON_S: case KW_EOF: case KW_EXP: case KW_FILEATTR:
    case KW_FIX: case KW_FRE: case KW_HEX_S: case KW_INP: case KW_INPUT_S:
    case KW_INSTR: case KW_INT: case KW_IOCTL_S: case KW_LBOUND:
    case KW_LCASE_S: case KW_LEFT_S: case KW_LEN: case KW_LOC: case KW_LOF:
    case KW_LOG: case KW_LPOS: case KW_LTRIM_S: case KW_MID_S: case KW_MKD_S:
    case KW_MKDMBF_S: case KW_MKI_S: case KW_MKL_S: case KW_MKS_S:
    case KW_MKSMBF_S: case KW_OCT_S: case KW_PEEK: case KW_PEN: case KW_PLAY:
    case KW_PMAP: case KW_POINT: case KW_POS: case KW_RIGHT_S: case KW_RND:
    case KW_RTRIM_S: case KW_SADD: case KW_SCREEN: case KW_SEEK: case KW_SGN:
    case KW_SIN: case KW_SPACE_S: case KW_SPC: case KW_SQR: case KW_STICK:
    case KW_STR_S: case KW_STRIG: case KW_STRING_S: case KW_TAB: case KW_TAN:
    case KW_TIMER: case KW_UBOUND: case KW_UCASE_S: case KW_VAL: case KW_VARPTR:
    case KW_VARPTR_S: case KW_VARSEG: case KW_KEY: case KW_COM: case KW_STEP:
    case KW_ERDEV:
        return 1;
    }
    return 0;
}

/* a blank between these two?  "first" is the line's first token */
static int space_between( TOKEN *first, TOKEN *prev, TOKEN *cur )
{
    int pk = prev->kind, ck = cur->kind;

    if ( ck == TK_EOL ) {
        return 0;
    }
    if ( ck == TK_COMMA || ck == TK_SEMI || ck == TK_COLON || ck == TK_RPAREN || ck == TK_DOT ) {
        return 0;
    }
    if ( pk == TK_LPAREN || pk == TK_DOT || pk == TK_HASH ) {
        return 0;
    }
    if ( pk == TK_LINENUM || pk == TK_LABEL ) {
        return 1;
    }
    if ( (pk == TK_MINUS || pk == TK_PLUS) && (prev->flags & TF_UNARY) ) {
        return 0;
    }
    if ( pk == TK_MINUS && (prev->flags & TF_TIGHT) ) {
        return 0;
    }
    if ( ck == TK_MINUS && (cur->flags & TF_TIGHT) ) {
        return pk == TK_KW;                     /* LINE -(x, y), but A-Z and )-( */
    }
    if ( ck == TK_LPAREN ) {
        if ( pk == TK_ID ) {
            /* SUB name (params) and FUNCTION's: a blank; a call: none */
            return prev > first && prev[-1].kind == TK_KW
                   && (prev[-1].kw == KW_SUB || prev[-1].kw == KW_FUNCTION);
        }
        if ( pk == TK_KW && tight_paren( prev->kw ) ) {
            return 0;
        }
        return 1;
    }
    return 1;
}

int list_line( const char *src, u32 len, TOKEN *toks, char *out, u32 max )
{
    OUTBUF buf;
    TOKEN *tok, *prev = NULL;
    char num[40];
    u32 lead = 0;

    buf.out = out;
    buf.len = 0;
    buf.max = max;
    buf.cut = 0;
    out[0] = 0;
    /* the indentation, as typed */
    while ( lead < len && (src[lead] == ' ' || src[lead] == '\t') ) {
        lead++;
    }
    put( &buf, src, lead );
    for ( tok = toks; ; tok++ ) {
        if ( tok->kind == TK_EOL ) {
            break;
        }
        if ( prev != NULL ) {
            if ( tok->kind == TK_REM && (tok->flags & TF_APOS) ) {
                /* a remark keeps the gap before it, as typed */
                u32 from = prev->col + prev->len, gap = tok->col > from ? tok->col - from : 1;
                while ( gap-- ) {
                    put_ch( &buf, ' ' );
                }
            } else if ( space_between( toks, prev, tok ) ) {
                put_ch( &buf, ' ' );
            }
        }
        switch ( tok->kind ) {
        case TK_KW:
            put_cstr( &buf, kw_name( tok->kw ) );
            break;
        case TK_ID: {
            u32 nlen = tok->len - (tok->kw ? 1 : 0);
            put_name( &buf, src + tok->col, nlen );
            if ( tok->kw ) {
                put_ch( &buf, tok->kw );
            }
            break;
        }
        case TK_LABEL:
            put_name( &buf, src + tok->col, tok->len );
            put_ch( &buf, ':' );
            break;
        case TK_LINENUM:
            num_fmt_l( (s32)tok->num, num );
            put_cstr( &buf, num );
            break;
        case TK_NUM:
            if ( tok->flags & (TF_HEX | TF_OCT) ) {
                put_radix( &buf, tok );
            } else {
                num_fmt_lit( tok->num, tok->ntype, num );
                put_cstr( &buf, num );
            }
            break;
        case TK_REM:
            if ( tok->flags & TF_APOS ) {
                put( &buf, src + tok->col, tok->len );
            } else {
                put_cstr( &buf, "REM" );
                put( &buf, src + tok->col + 3, tok->len - 3 );
            }
            break;
        case TK_STR:
        case TK_DATA:
        case TK_BAD:
            put( &buf, src + tok->col, tok->len );
            break;
        case TK_PLUS:   put_ch( &buf, '+' ); break;
        case TK_MINUS:  put_ch( &buf, '-' ); break;
        case TK_STAR:   put_ch( &buf, '*' ); break;
        case TK_SLASH:  put_ch( &buf, '/' ); break;
        case TK_BSLASH: put_ch( &buf, '\\' ); break;
        case TK_CARET:  put_ch( &buf, '^' ); break;
        case TK_EQ:     put_ch( &buf, '=' ); break;
        case TK_LT:     put_ch( &buf, '<' ); break;
        case TK_GT:     put_ch( &buf, '>' ); break;
        case TK_LE:     put_cstr( &buf, "<=" ); break;
        case TK_GE:     put_cstr( &buf, ">=" ); break;
        case TK_NE:     put_cstr( &buf, "<>" ); break;
        case TK_LPAREN: put_ch( &buf, '(' ); break;
        case TK_RPAREN: put_ch( &buf, ')' ); break;
        case TK_COMMA:  put_ch( &buf, ',' ); break;
        case TK_SEMI:   put_ch( &buf, ';' ); break;
        case TK_COLON:  put_ch( &buf, ':' ); break;
        case TK_HASH:   put_ch( &buf, '#' ); break;
        case TK_DOT:    put_ch( &buf, '.' ); break;
        }
        prev = tok;
    }
    /* no blanks at the end */
    while ( buf.len > lead && out[buf.len - 1] == ' ' ) {
        out[--buf.len] = 0;
    }
    if ( buf.cut || buf.len > MAX_LINE ) {
        return -1;                              /* too long once listed: the caller keeps the line */
    }
    return (int)buf.len;
}
