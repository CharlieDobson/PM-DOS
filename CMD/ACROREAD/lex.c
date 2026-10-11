/*
 * LEX.C - the words of a PDF file, and the objects made of them.
 *
 * One reader for the file's own objects and for a page's content
 * stream, because they are the same language.  What differs is that
 * the file may say "12 0 R" - which takes reading two numbers ahead to
 * tell from three numbers - and a content stream has operators, which
 * come back as T_OP.
 *
 * An array or a dictionary is collected on a stack of objects (it may
 * hold any number, and they nest) and copied into the pool when its
 * closing bracket is found.
 */
#include "pdf.h"

#define MAX_DEPTH   48

static OBJ *stack;              /* arrays and dictionaries being collected */
static u32  stack_len, stack_cap;
static u8  *text;               /* a string or a name being collected */
static u32  text_len, text_cap;

static int lex_value( LEX *lex, OBJ *out, int depth );

int is_delim( int ch )
{
    return ch == '(' || ch == ')' || ch == '<' || ch == '>' || ch == '[' || ch == ']' ||
           ch == '{' || ch == '}' || ch == '/' || ch == '%';
}

void lex_init( LEX *lex, IN *in, POOL *pool, int content )
{
    mem_set( lex, 0, sizeof( *lex ) );
    lex->in = in;
    lex->pool = pool;
    lex->content = content;
}

int lex_skip_ws( IN *in )
{
    int ch;

    for ( ;; ) {
        ch = in_peek( in );
        if ( ch < 0 ) {
            return -1;
        }
        if ( IS_WS( ch ) ) {
            in->cur++;
        } else if ( ch == '%' ) {
            do {
                in->cur++;
                ch = in_peek( in );
            } while ( ch >= 0 && ch != '\n' && ch != '\r' );
        } else {
            return ch;
        }
    }
}

static void stack_push( const OBJ *obj )
{
    OBJ *grown;
    u32 cap;

    if ( stack_len == stack_cap ) {
        cap = stack_cap ? stack_cap * 2 : 256;
        grown = (OBJ *)xalloc( cap * sizeof( OBJ ) );
        if ( stack ) {
            mem_cpy( grown, stack, stack_len * sizeof( OBJ ) );
            xfree( stack );
        }
        stack = grown;
        stack_cap = cap;
    }
    stack[stack_len++] = *obj;
}

static void text_put( int ch )
{
    u8 *grown;
    u32 cap;

    if ( text_len == text_cap ) {
        cap = text_cap ? text_cap * 2 : 1024;
        grown = (u8 *)xalloc( cap );
        if ( text ) {
            mem_cpy( grown, text, text_len );
            xfree( text );
        }
        text = grown;
        text_cap = cap;
    }
    text[text_len++] = (u8)ch;
}

/* what was collected, as a string of the pool's, with a NUL after it */
static void text_keep( LEX *lex, OBJ *out, int type )
{
    u8 *copy = (u8 *)pool_alloc( lex->pool, text_len + 1 );

    mem_cpy( copy, text, text_len );
    copy[text_len] = 0;
    out->type = (u8)type;
    out->u.str.ptr = copy;
    out->u.str.len = text_len;
    if ( type == T_STR && lex->crypt ) {
        crypt_string( lex->num, lex->gen, copy, &out->u.str.len );
    }
}

static int hex_val( int ch )
{
    if ( ch >= '0' && ch <= '9' ) {
        return ch - '0';
    }
    if ( ch >= 'a' && ch <= 'f' ) {
        return ch - 'a' + 10;
    }
    if ( ch >= 'A' && ch <= 'F' ) {
        return ch - 'A' + 10;
    }
    return -1;
}

static void lex_name( LEX *lex, OBJ *out )
{
    IN *in = lex->in;
    int ch, hi, lo;

    text_len = 0;
    for ( ;; ) {
        ch = in_peek( in );
        if ( ch < 0 || IS_WS( ch ) || is_delim( ch ) ) {
            break;
        }
        in->cur++;
        if ( ch == '#' ) {
            hi = hex_val( in_peek( in ) );
            if ( hi >= 0 ) {
                in->cur++;
                lo = hex_val( in_peek( in ) );
                if ( lo >= 0 ) {
                    in->cur++;
                    ch = hi * 16 + lo;
                } else {
                    ch = hi;
                }
            }
        }
        text_put( ch );
    }
    text_keep( lex, out, T_NAME );
}

static void lex_string( LEX *lex, OBJ *out )
{
    IN *in = lex->in;
    int ch, depth = 1, val, count;

    text_len = 0;
    for ( ;; ) {
        ch = IN_GETC( in );
        if ( ch < 0 ) {
            break;
        }
        if ( ch == '(' ) {
            depth++;
        } else if ( ch == ')' ) {
            if ( --depth == 0 ) {
                break;
            }
        } else if ( ch == '\r' ) {              /* an end of line is a line feed */
            if ( in_peek( in ) == '\n' ) {
                in->cur++;
            }
            ch = '\n';
        } else if ( ch == '\\' ) {
            ch = IN_GETC( in );
            switch ( ch ) {
            case 'n':  ch = '\n'; break;
            case 'r':  ch = '\r'; break;
            case 't':  ch = '\t'; break;
            case 'b':  ch = '\b'; break;
            case 'f':  ch = '\f'; break;
            case '\r':
                if ( in_peek( in ) == '\n' ) {
                    in->cur++;
                }
                continue;
            case '\n':
                continue;
            default:
                if ( ch >= '0' && ch <= '7' ) {
                    val = ch - '0';
                    for ( count = 1; count < 3; count++ ) {
                        ch = in_peek( in );
                        if ( ch < '0' || ch > '7' ) {
                            break;
                        }
                        in->cur++;
                        val = val * 8 + (ch - '0');
                    }
                    ch = val & 0xFF;
                } else if ( ch < 0 ) {
                    continue;
                }
                break;
            }
        }
        text_put( ch );
    }
    text_keep( lex, out, T_STR );
}

static void lex_hex( LEX *lex, OBJ *out )
{
    IN *in = lex->in;
    int ch, val, have = -1;

    text_len = 0;
    for ( ;; ) {
        ch = IN_GETC( in );
        if ( ch < 0 || ch == '>' ) {
            break;
        }
        val = hex_val( ch );
        if ( val < 0 ) {
            continue;
        }
        if ( have < 0 ) {
            have = val;
        } else {
            text_put( have * 16 + val );
            have = -1;
        }
    }
    if ( have >= 0 ) {
        text_put( have * 16 );
    }
    text_keep( lex, out, T_STR );
}

static void lex_array( LEX *lex, OBJ *out, int depth )
{
    OBJ item;
    ARR *arr;
    u32 base = stack_len, count;
    int type;

    for ( ;; ) {
        type = lex_value( lex, &item, depth + 1 );
        if ( type == T_EOF ) {
            break;
        }
        if ( type == T_OP ) {
            if ( item.u.str.ptr[0] == ']' ) {
                break;
            }
            if ( !lex->content && (str_cmp( (char *)item.u.str.ptr, "endobj" ) == 0 ||
                                   str_cmp( (char *)item.u.str.ptr, ">>" ) == 0) ) {
                break;                  /* it was never closed */
            }
            continue;
        }
        stack_push( &item );
    }
    count = stack_len - base;
    arr = (ARR *)pool_alloc( lex->pool, sizeof( ARR ) + count * sizeof( OBJ ) );
    arr->count = count;
    arr->items = (OBJ *)(arr + 1);
    mem_cpy( arr->items, stack + base, count * sizeof( OBJ ) );
    stack_len = base;
    out->type = T_ARR;
    out->u.arr = arr;
}

static void lex_dict( LEX *lex, OBJ *out, int depth )
{
    OBJ key, val;
    DICT *dict;
    u32 base = stack_len, count, index;
    int type;

    for ( ;; ) {
        type = lex_value( lex, &key, depth + 1 );
        if ( type == T_EOF ) {
            break;
        }
        if ( type == T_OP ) {
            if ( key.u.str.ptr[0] == '>' ) {
                break;
            }
            if ( !lex->content && str_cmp( (char *)key.u.str.ptr, "endobj" ) == 0 ) {
                break;
            }
            continue;
        }
        if ( type != T_NAME ) {
            continue;
        }
        type = lex_value( lex, &val, depth + 1 );
        if ( type == T_EOF || (type == T_OP && val.u.str.ptr[0] == '>') ) {
            break;
        }
        if ( type == T_OP ) {
            val.type = T_NULL;
        }
        stack_push( &key );
        stack_push( &val );
    }
    count = (stack_len - base) / 2;
    dict = (DICT *)pool_alloc( lex->pool, sizeof( DICT ) + count * sizeof( DENT ) );
    dict->count = count;
    dict->ents = (DENT *)(dict + 1);
    for ( index = 0; index < count; index++ ) {
        dict->ents[index].key = (const char *)stack[base + index * 2].u.str.ptr;
        dict->ents[index].val = stack[base + index * 2 + 1];
    }
    stack_len = base;
    out->type = T_DICT;
    out->u.dict = dict;
}

/* A number out of "word", if it is one: 0 if it is not.  Nine digits
   are kept; the rest only move the point. */
static int lex_number( const char *word, OBJ *out )
{
    u32 digits = 0, whole = 0;
    int neg = 0, kept = 0, scale = 0, dot = 0, any = 0, fits = 1, dig;

    if ( *word == '+' ) {
        word++;
    } else if ( *word == '-' ) {
        neg = 1;
        word++;
    }
    for ( ; *word; word++ ) {
        if ( *word >= '0' && *word <= '9' ) {
            dig = *word - '0';
            any = 1;
            if ( !dot ) {
                if ( whole > (0x7FFFFFFFUL - (u32)dig) / 10 ) {
                    fits = 0;
                } else {
                    whole = whole * 10 + (u32)dig;
                }
            }
            if ( kept < 9 ) {
                digits = digits * 10 + (u32)dig;
                if ( digits ) {
                    kept++;
                }
                if ( dot ) {
                    scale++;
                }
            } else if ( !dot ) {
                scale--;
            }
        } else if ( *word == '.' && !dot ) {
            dot = 1;
        } else if ( *word == '-' ) {
            continue;                   /* "1-2" and "--3" are read as Acrobat reads them */
        } else {
            return 0;
        }
    }
    if ( !any ) {
        return 0;
    }
    if ( !dot && fits ) {
        out->type = T_INT;
        out->u.ival = neg ? -(s32)whole : (s32)whole;
    } else {
        out->type = T_REAL;
        out->u.real = real_dec( digits, scale );
        if ( neg ) {
            out->u.real.man = -out->u.real.man;
        }
    }
    return 1;
}

/* the word that starts with "first", into lex->word: up to white space
   or a delimiter, which is left unread */
static void lex_word( LEX *lex, int first )
{
    IN *in = lex->in;
    u32 len = 0;
    int ch = first;

    for ( ;; ) {
        if ( len < sizeof( lex->word ) - 1 ) {
            lex->word[len++] = (char)ch;
        }
        ch = in_peek( in );
        if ( ch < 0 || IS_WS( ch ) || is_delim( ch ) ) {
            break;
        }
        in->cur++;
    }
    lex->word[len] = 0;
}

static void lex_op( LEX *lex, OBJ *out )
{
    out->type = T_OP;
    out->u.str.ptr = (u8 *)lex->word;
    out->u.str.len = str_len( lex->word );
}

static int lex_value( LEX *lex, OBJ *out, int depth )
{
    IN *in = lex->in;
    OBJ second;
    int ch;

    out->type = T_NULL;
    out->spare = 0;
    out->gen = 0;
    if ( lex->has_pend ) {
        lex->has_pend = 0;
        *out = lex->pend;
        if ( out->type == T_INT ) {
            goto maybe_ref;
        }
        return out->type;
    }
    for ( ;; ) {
        ch = lex_skip_ws( in );
        if ( ch < 0 ) {
            return out->type = T_EOF;
        }
        in->cur++;
        if ( ch != ')' ) {              /* a stray one: nothing opens it */
            break;
        }
    }
    switch ( ch ) {
    case '/':
        lex_name( lex, out );
        return T_NAME;
    case '(':
        lex_string( lex, out );
        return T_STR;
    case '<':
        if ( in_peek( in ) == '<' ) {
            in->cur++;
            if ( depth >= MAX_DEPTH ) {
                pdf_fail( PE_DAMAGED );
            }
            lex_dict( lex, out, depth );
            return T_DICT;
        }
        lex_hex( lex, out );
        return T_STR;
    case '[':
        if ( depth >= MAX_DEPTH ) {
            pdf_fail( PE_DAMAGED );
        }
        lex_array( lex, out, depth );
        return T_ARR;
    case '>':
        if ( in_peek( in ) == '>' ) {
            in->cur++;
        }
        str_cpy( lex->word, ">>" );
        lex_op( lex, out );
        return T_OP;
    case ']':
    case '{':
    case '}':
        lex->word[0] = (char)ch;
        lex->word[1] = 0;
        lex_op( lex, out );
        return T_OP;
    }
    lex_word( lex, ch );
    if ( (ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' ) {
        if ( lex_number( lex->word, out ) ) {
            if ( out->type == T_INT ) {
                goto maybe_ref;
            }
            return out->type;
        }
    }
    if ( str_cmp( lex->word, "true" ) == 0 || str_cmp( lex->word, "false" ) == 0 ) {
        out->type = T_BOOL;
        out->u.ival = lex->word[0] == 't';
        return T_BOOL;
    }
    if ( str_cmp( lex->word, "null" ) == 0 ) {
        return out->type = T_NULL;
    }
    lex_op( lex, out );
    return T_OP;

maybe_ref:
    /* "12 0 R" - only in the file, and only if the next two words are a
       whole number and an R.  A number read ahead that turns out not to
       be a generation is handed over by the next call. */
    if ( lex->content || out->u.ival < 0 ) {
        return T_INT;
    }
    ch = lex_skip_ws( in );
    if ( ch < '0' || ch > '9' ) {
        return T_INT;
    }
    in->cur++;
    lex_word( lex, ch );
    if ( !lex_number( lex->word, &second ) ) {
        return T_INT;                   /* not a number at all: the word is lost */
    }
    if ( second.type == T_INT ) {
        ch = lex_skip_ws( in );
        if ( ch == 'R' ) {
            in->cur++;
            out->type = T_REF;
            out->gen = (u16)second.u.ival;
            return T_REF;
        }
    }
    second.spare = 0;
    second.gen = 0;
    lex->has_pend = 1;
    lex->pend = second;
    return T_INT;
}

int lex_obj( LEX *lex, OBJ *out )
{
    return lex_value( lex, out, 0 );
}
