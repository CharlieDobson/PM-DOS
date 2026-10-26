/*
 * LEX.C - a line of a program into tokens.
 *
 * A token keeps where it came from (column and length), not a copy of
 * the text: the listing and the error messages both point back into the
 * line.  A name keeps its dots - "a.b" may be a record and its field or
 * one name, and only the compiler knows which - and its suffix is noted
 * apart from it.  What the lexer cannot know (a sign or a subtraction,
 * LINE's "-(") COMP.C marks on the tokens while it reads them, and
 * LIST.C goes by the marks.
 */
#include "qb.h"

static const char *const kw_text[KW_COUNT] = {
#define KW( id, text ) text,
#include "kw.h"
#undef KW
};

const char *kw_name( int kw )
{
    return kw >= 0 && kw < KW_COUNT ? kw_text[kw] : "";
}

/* the reserved word spelled name[0..len), in either case, or -1 */
int kw_lookup( const char *name, u32 len )
{
    char upper[NAME_MAX + 2];
    int lo = 0, hi = KW_COUNT - 1, mid, cmp;
    u32 i;

    if ( len == 0 || len > NAME_MAX + 1 ) {
        return -1;
    }
    for ( i = 0; i < len; i++ ) {
        upper[i] = (char)ch_upper( name[i] );
    }
    upper[len] = 0;
    while ( lo <= hi ) {
        mid = (lo + hi) / 2;
        cmp = str_cmp( upper, kw_text[mid] );
        if ( cmp == 0 ) {
            return mid;
        }
        if ( cmp < 0 ) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return -1;
}

int lex_suffix_type( int ch )
{
    switch ( ch ) {
    case '%':   return TY_INT;
    case '&':   return TY_LONG;
    case '!':   return TY_SINGLE;
    case '#':   return TY_DOUBLE;
    case '$':   return TY_STR;
    }
    return TY_NONE;
}

static int is_alpha( int ch )
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static int is_digit( int ch )
{
    return ch >= '0' && ch <= '9';
}

static int is_namech( int ch )
{
    return is_alpha( ch ) || is_digit( ch ) || ch == '.';
}

static int is_space( int ch )
{
    return ch == ' ' || ch == '\t';
}

/* the end of a number written in decimal, from "pos": digits, a point,
   digits, an exponent if the E or D has digits (or a sign) after it,
   and a suffix */
static u32 number_end( const char *text, u32 len, u32 pos )
{
    u32 save;

    while ( pos < len && is_digit( text[pos] ) ) {
        pos++;
    }
    if ( pos < len && text[pos] == '.' ) {
        pos++;
        while ( pos < len && is_digit( text[pos] ) ) {
            pos++;
        }
    }
    if ( pos < len && (ch_upper( text[pos] ) == 'E' || ch_upper( text[pos] ) == 'D') ) {
        save = pos;
        pos++;
        if ( pos < len && (text[pos] == '+' || text[pos] == '-') ) {
            pos++;
        }
        if ( pos < len && is_digit( text[pos] ) ) {
            while ( pos < len && is_digit( text[pos] ) ) {
                pos++;
            }
        } else {
            pos = save;             /* an E with no digits is not an exponent */
        }
    }
    if ( pos < len && (text[pos] == '%' || text[pos] == '&' || text[pos] == '!' || text[pos] == '#') ) {
        pos++;
    }
    return pos;
}

static u32 radix_end( const char *text, u32 len, u32 pos )
{
    int hex = 0;

    pos++;                                      /* the & */
    if ( pos < len && ch_upper( text[pos] ) == 'H' ) {
        hex = 1;
        pos++;
    } else if ( pos < len && ch_upper( text[pos] ) == 'O' ) {
        pos++;
    }
    while ( pos < len ) {
        int ch = ch_upper( text[pos] );
        if ( is_digit( ch ) && (hex || ch <= '7') ) {
            pos++;
        } else if ( hex && ch >= 'A' && ch <= 'F' ) {
            pos++;
        } else {
            break;
        }
    }
    if ( pos < len && (text[pos] == '%' || text[pos] == '&') ) {
        pos++;
    }
    return pos;
}

int lex_line( const char *text, u32 len, TOKEN *toks, int max, int *err_col,
              const char **err_msg )
{
    u32 pos = 0, start, end, used;
    int count = 0, kw, type, first = 1, label_ok = 1, may_label, ch;
    double val;
    TOKEN *tok;

#define FAIL( col, msg )    do { *err_col = (int)(col); *err_msg = (msg); return -1; } while ( 0 )

    for ( ;; ) {
        while ( pos < len && is_space( text[pos] ) ) {
            pos++;
        }
        if ( count >= max - 1 ) {
            FAIL( pos, "Line too long" );
        }
        tok = &toks[count];
        mem_set( tok, 0, sizeof( *tok ) );
        tok->col = (u16)pos;
        if ( pos >= len ) {
            tok->kind = TK_EOL;
            return count + 1;
        }
        start = pos;
        ch = (u8)text[pos];
        may_label = label_ok;                   /* only the first token, or one after a line number */
        label_ok = 0;

        /* a line number, first thing on the line */
        if ( first && is_digit( ch ) ) {
            while ( pos < len && is_digit( text[pos] ) ) {
                pos++;
            }
            tok->kind = TK_LINENUM;
            tok->len = (u16)(pos - start);
            tok->num = num_parse( text + start, pos - start, &used );
            if ( tok->num > 65529.0 ) {
                FAIL( start, "Invalid line number" );
            }
            first = 0;
            label_ok = 1;                       /* "20 lbl:" - a label may follow */
            count++;
            continue;
        }

        if ( is_alpha( ch ) ) {
            while ( pos < len && is_namech( text[pos] ) ) {
                pos++;
            }
            end = pos;
            /* a reserved word with a $ in its spelling */
            if ( pos < len && text[pos] == '$' && (kw = kw_lookup( text + start, pos + 1 - start )) >= 0 ) {
                pos++;
                tok->kind = TK_KW;
                tok->kw = (u16)kw;
                tok->len = (u16)(pos - start);
            } else if ( !(pos < len && text[pos] == '$') && (kw = kw_lookup( text + start, end - start )) >= 0 ) {
                /* (a reserved word with a $ that its spelling has not - LEN$,
                   PRINT$ - is a string variable, as QBasic takes it) */
                tok->kind = TK_KW;
                tok->kw = (u16)kw;
                tok->len = (u16)(end - start);
                if ( kw == KW_REM ) {
                    /* the rest is the remark, as it is */
                    tok->kind = TK_REM;
                    tok->len = (u16)(len - start);
                    pos = len;
                } else if ( kw == KW_DATA ) {
                    count++;
                    if ( count >= max - 1 ) {
                        FAIL( pos, "Line too long" );
                    }
                    while ( pos < len && is_space( text[pos] ) ) {
                        pos++;
                    }
                    tok = &toks[count];
                    mem_set( tok, 0, sizeof( *tok ) );
                    tok->kind = TK_DATA;
                    tok->col = (u16)pos;
                    start = pos;
                    {
                        int quoted = 0;
                        while ( pos < len && (quoted || text[pos] != ':') ) {
                            if ( text[pos] == '"' ) {
                                quoted = !quoted;
                            }
                            pos++;
                        }
                    }
                    end = pos;
                    while ( end > start && is_space( text[end - 1] ) ) {
                        end--;
                    }
                    tok->len = (u16)(end - start);
                }
            } else {
                if ( end - start > NAME_MAX ) {
                    FAIL( start, "Identifier too long" );
                }
                tok->kind = TK_ID;
                if ( pos < len && lex_suffix_type( text[pos] ) != TY_NONE ) {
                    tok->kw = (u16)(u8)text[pos];
                    tok->ntype = (u8)lex_suffix_type( text[pos] );
                    pos++;
                }
                tok->len = (u16)(pos - start);
                if ( (end - start) >= 2 && ch_upper( text[start] ) == 'F' && ch_upper( text[start + 1] ) == 'N' ) {
                    tok->flags |= TF_FN;
                }
                /* a label: a name and a colon, first on the line */
                if ( may_label && tok->kw == 0 ) {
                    u32 look = pos;
                    while ( look < len && is_space( text[look] ) ) {
                        look++;
                    }
                    if ( look < len && text[look] == ':' ) {
                        tok->kind = TK_LABEL;
                        tok->len = (u16)(end - start);
                        pos = look + 1;
                    }
                }
            }
            first = 0;
            count++;
            continue;
        }

        if ( is_digit( ch ) || (ch == '.' && pos + 1 < len && is_digit( text[pos + 1] )) ) {
            pos = number_end( text, len, pos );
            tok->kind = TK_NUM;
            tok->len = (u16)(pos - start);
            if ( !num_scan( text + start, pos - start, &val, &type ) || DBL_BAD( val ) ) {
                FAIL( start, "Overflow" );
            }
            if ( (type == TY_INT && (val < -32768.0 || val > 32767.0))
              || (type == TY_LONG && (val < -2147483648.0 || val > 2147483647.0))
              || (type == TY_SINGLE && (val > SNG_MAX || val < -SNG_MAX)) ) {
                FAIL( start, "Overflow" );
            }
            tok->num = val;
            tok->ntype = (u8)type;
            first = 0;
            count++;
            continue;
        }

        if ( ch == '&' ) {
            pos = radix_end( text, len, pos );
            tok->kind = TK_NUM;
            tok->len = (u16)(pos - start);
            if ( !num_scan( text + start, pos - start, &val, &type ) ) {
                FAIL( start, type < 0 ? "Overflow" : "Syntax error" );
            }
            tok->num = val;
            tok->ntype = (u8)type;
            tok->flags |= (start + 1 < len && ch_upper( text[start + 1] ) == 'H') ? TF_HEX : TF_OCT;
            first = 0;
            count++;
            continue;
        }

        first = 0;
        pos++;
        tok->len = 1;
        switch ( ch ) {
        case '"':
            while ( pos < len && text[pos] != '"' ) {
                pos++;
            }
            tok->kind = TK_STR;
            if ( pos < len ) {
                pos++;
            } else {
                tok->flags |= TF_UNTERM;
            }
            tok->len = (u16)(pos - start);
            break;
        case '\'':
            tok->kind = TK_REM;
            tok->flags |= TF_APOS;
            tok->len = (u16)(len - start);
            pos = len;
            break;
        case '?':
            tok->kind = TK_KW;
            tok->kw = KW_PRINT;
            tok->flags |= TF_QMARK;
            break;
        case '+':   tok->kind = TK_PLUS;    break;
        case '-':   tok->kind = TK_MINUS;   break;
        case '*':   tok->kind = TK_STAR;    break;
        case '/':   tok->kind = TK_SLASH;   break;
        case '\\':  tok->kind = TK_BSLASH;  break;
        case '^':   tok->kind = TK_CARET;   break;
        case '(':   tok->kind = TK_LPAREN;  break;
        case ')':   tok->kind = TK_RPAREN;  break;
        case ',':   tok->kind = TK_COMMA;   break;
        case ';':   tok->kind = TK_SEMI;    break;
        case ':':   tok->kind = TK_COLON;   break;
        case '#':   tok->kind = TK_HASH;    break;
        case '.':   tok->kind = TK_DOT;     break;
        case '=':
            tok->kind = TK_EQ;
            if ( pos < len && text[pos] == '<' ) {
                tok->kind = TK_LE;
                pos++;
            } else if ( pos < len && text[pos] == '>' ) {
                tok->kind = TK_GE;
                pos++;
            }
            break;
        case '<':
            tok->kind = TK_LT;
            if ( pos < len && text[pos] == '=' ) {
                tok->kind = TK_LE;
                pos++;
            } else if ( pos < len && text[pos] == '>' ) {
                tok->kind = TK_NE;
                pos++;
            }
            break;
        case '>':
            tok->kind = TK_GT;
            if ( pos < len && text[pos] == '=' ) {
                tok->kind = TK_GE;
                pos++;
            } else if ( pos < len && text[pos] == '<' ) {
                tok->kind = TK_NE;
                pos++;
            }
            break;
        default:
            tok->kind = TK_BAD;
            break;
        }
        tok->len = (u16)(pos - start);
        count++;
    }
#undef FAIL
}
