/*
 * NUM.C - numbers to text and text to numbers, the way QBasic does both.
 *
 * DIGITS ARE EXACT.  A SINGLE is shown with 7 significant digits and a
 * DOUBLE with 16, rounded from the exact binary value (BIGDEC.C works
 * it out in whole numbers).  Reading a number goes the other way: up to
 * 18 digits are gathered as a whole number and made the nearest DOUBLE
 * to it times its power of ten, again exactly.
 *
 * THE SHAPE: a whole number that fits in the precision is written out
 * (1000000); anything else is written with a decimal point and no
 * leading zero before it (.5) while that takes no more digits than the
 * precision, and in E form beyond (1E+07, 1.5E-05; D for a DOUBLE).
 * fixed_ok() is that rule, in one place.
 */
#include "qb.h"

/* a value rounded to a SINGLE.  It goes through memory: the FPU keeps
   more bits in a register, and a cast alone may leave them there.  One
   too big for a SINGLE is left as it is, for the caller's Overflow. */
double num_sng( double v )
{
    volatile float f;

    if ( v > SNG_MAX || v < -SNG_MAX ) {
        return v;
    }
    f = (float)v;
    return f;
}

/* the digits of |v| rounded to "prec" significant ones: digits[] holds
   them as characters, trailing zeros taken off; *exp10 is the power of
   ten of the first.  -> how many digits */
int num_sig_digits( double v, int prec, char *digits, int *exp10 )
{
    if ( v < 0 ) {
        v = -v;
    }
    return dec_digits( v, prec, digits, exp10 );
}

/* may a number with these digits be written without an exponent? */
static int fixed_ok( int exp10, int count, int prec )
{
    if ( exp10 >= 0 ) {
        return exp10 < prec;            /* every whole digit fits */
    }
    /* below 1: the zeros after the point and the digits together */
    return (-exp10 - 1) + count <= prec;
}

static int fmt_float( double v, int prec, int letter, char *buf )
{
    char digits[20];
    int count, exp10, pos = 0, i, e;

    if ( v == 0 ) {
        buf[0] = '0';
        buf[1] = 0;
        return 1;
    }
    if ( v < 0 ) {
        buf[pos++] = '-';
    }
    count = num_sig_digits( v, prec, digits, &exp10 );
    if ( fixed_ok( exp10, count, prec ) ) {
        if ( exp10 >= 0 ) {
            for ( i = 0; i <= exp10; i++ ) {
                buf[pos++] = i < count ? digits[i] : '0';
            }
            if ( count > exp10 + 1 ) {
                buf[pos++] = '.';
                for ( i = exp10 + 1; i < count; i++ ) {
                    buf[pos++] = digits[i];
                }
            }
        } else {
            buf[pos++] = '.';
            for ( i = 0; i < -exp10 - 1; i++ ) {
                buf[pos++] = '0';
            }
            for ( i = 0; i < count; i++ ) {
                buf[pos++] = digits[i];
            }
        }
    } else {
        buf[pos++] = digits[0];
        if ( count > 1 ) {
            buf[pos++] = '.';
            for ( i = 1; i < count; i++ ) {
                buf[pos++] = digits[i];
            }
        }
        buf[pos++] = (char)letter;
        e = exp10;
        if ( e < 0 ) {
            buf[pos++] = '-';
            e = -e;
        } else {
            buf[pos++] = '+';
        }
        if ( e >= 100 ) {
            buf[pos++] = (char)('0' + e / 100);
            e %= 100;
        }
        buf[pos++] = (char)('0' + e / 10);
        buf[pos++] = (char)('0' + e % 10);
    }
    buf[pos] = 0;
    return pos;
}

int num_fmt_l( s32 v, char *buf )
{
    char digits[12];
    u32 mag;
    int count = 0, pos = 0;

    if ( v < 0 ) {
        buf[pos++] = '-';
        mag = (u32)0 - (u32)v;
    } else {
        mag = (u32)v;
    }
    do {
        digits[count++] = (char)('0' + mag % 10);
        mag /= 10;
    } while ( mag );
    while ( count ) {
        buf[pos++] = digits[--count];
    }
    buf[pos] = 0;
    return pos;
}

int num_fmt_s( double v, char *buf )
{
    return fmt_float( num_sng( v ), 7, 'E', buf );  /* the SINGLE it is, not the wider value */
}

int num_fmt_d( double v, char *buf )
{
    return fmt_float( v, 16, 'D', buf );
}

/* A constant as the listing shows it: the shortest form of its value,
   and the suffix where the form alone would read as another type. */
int num_fmt_lit( double v, int type, char *buf )
{
    int len, i, has_point = 0, has_exp = 0;

    switch ( type ) {
    case TY_INT:
        return num_fmt_l( (s32)v, buf );
    case TY_LONG:
        len = num_fmt_l( (s32)v, buf );
        if ( v >= -32768.0 && v <= 32767.0 ) {
            buf[len++] = '&';
            buf[len] = 0;
        }
        return len;
    case TY_SINGLE:
        len = num_fmt_s( (float)v, buf );
        break;
    default:
        len = num_fmt_d( v, buf );
        break;
    }
    for ( i = 0; i < len; i++ ) {
        if ( buf[i] == '.' ) {
            has_point = 1;
        } else if ( buf[i] == 'E' || buf[i] == 'D' ) {
            has_exp = buf[i];
        }
    }
    if ( type == TY_SINGLE ) {
        if ( !has_point && !has_exp ) {
            buf[len++] = '!';
        }
    } else if ( has_exp != 'D' ) {
        buf[len++] = '#';
    }
    buf[len] = 0;
    return len;
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

static int is_blank( int ch )
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static int hex_digit( int ch )
{
    if ( ch >= '0' && ch <= '9' ) {
        return ch - '0';
    }
    ch = ch_upper( ch );
    if ( ch >= 'A' && ch <= 'F' ) {
        return ch - 'A' + 10;
    }
    return -1;
}

/* The number at the front of text[], as VAL reads it: blanks anywhere in
   it are skipped, and it ends at the first character that cannot go on
   with it.  *type comes back as the type the number is written as (a
   suffix, the digits, &H...), *used as the characters taken. */
static double scan_number( const char *text, u32 len, u32 *used, int *type, int *any )
{
    u32 pos = 0, lo = 0, hi = 0;
    int neg = 0, digits = 0, sig = 0, scale = 0, point = 0, expn = 0, eneg = 0;
    int ch, radix = 0, dval, exp_letter = 0, exp_digits = 0;
    u32 val32 = 0;
    double result;

    *any = 0;
    *type = TY_INT;
    while ( pos < len && is_blank( text[pos] ) ) {
        pos++;
    }
    if ( pos < len && (text[pos] == '-' || text[pos] == '+') ) {
        neg = text[pos] == '-';
        pos++;
    }
    while ( pos < len && is_blank( text[pos] ) ) {
        pos++;
    }
    if ( pos < len && text[pos] == '&' ) {
        pos++;
        if ( pos < len && ch_upper( text[pos] ) == 'H' ) {
            radix = 16;
            pos++;
        } else {
            radix = 8;
            if ( pos < len && ch_upper( text[pos] ) == 'O' ) {
                pos++;
            }
        }
        while ( pos < len ) {
            ch = text[pos];
            if ( is_blank( ch ) ) {
                pos++;
                continue;
            }
            dval = hex_digit( ch );
            if ( dval < 0 || dval >= radix ) {
                break;
            }
            if ( (radix == 16 && val32 > 0x0FFFFFFFUL) || (radix == 8 && val32 > 0x1FFFFFFFUL) ) {
                *type = -1;                     /* too big: Overflow */
            }
            val32 = val32 * (u32)radix + (u32)dval;
            digits++;
            pos++;
        }
        *any = digits > 0;
        if ( pos < len && text[pos] == '&' ) {
            *type = *type < 0 ? -1 : TY_LONG;
            pos++;
        } else if ( pos < len && text[pos] == '%' ) {
            pos++;
            if ( val32 > 0xFFFFUL ) {
                *type = -1;
            }
        } else if ( *type >= 0 && val32 > 0xFFFFUL ) {
            *type = TY_LONG;
        }
        *used = pos;
        if ( *type == TY_LONG ) {
            result = (double)(s32)val32;
        } else {
            result = (double)(s16)(u16)val32;
        }
        return neg ? -result : result;
    }
    for ( ; pos < len; pos++ ) {
        ch = text[pos];
        if ( is_blank( ch ) ) {
            continue;
        }
        if ( ch == '.' && !point ) {
            point = 1;
            continue;
        }
        if ( ch < '0' || ch > '9' ) {
            break;
        }
        digits++;
        if ( sig == 0 && ch == '0' ) {
            if ( point ) {
                scale--;
            }
            continue;
        }
        if ( sig < 18 ) {
            /* hi:lo = hi:lo * 10 + digit, in 16-bit pieces */
            u32 l0 = (lo & 0xFFFF) * 10 + (u32)(ch - '0');
            u32 l1 = (lo >> 16) * 10 + (l0 >> 16);
            u32 h0 = (hi & 0xFFFF) * 10 + (l1 >> 16);
            u32 h1 = (hi >> 16) * 10 + (h0 >> 16);
            lo = (l0 & 0xFFFF) | (l1 << 16);
            hi = (h0 & 0xFFFF) | (h1 << 16);
            sig++;
            if ( point ) {
                scale--;
            }
        } else if ( !point ) {
            scale++;
        }
    }
    *any = digits > 0;
    if ( digits > 0 && pos < len ) {
        ch = ch_upper( text[pos] );
        if ( ch == 'E' || ch == 'D' ) {
            exp_letter = ch;
            pos++;
            while ( pos < len && is_blank( text[pos] ) ) {
                pos++;
            }
            if ( pos < len && (text[pos] == '-' || text[pos] == '+') ) {
                eneg = text[pos] == '-';
                pos++;
            }
            for ( ; pos < len; pos++ ) {
                ch = text[pos];
                if ( is_blank( ch ) ) {
                    continue;
                }
                if ( ch < '0' || ch > '9' ) {
                    break;
                }
                exp_digits++;
                if ( expn < 10000 ) {
                    expn = expn * 10 + (ch - '0');
                }
            }
        }
    }
    result = dec_build( lo, hi, scale + (eneg ? -expn : expn) );
    if ( exp_letter && exp_digits == 0 ) {
        result = 0;                     /* VAL("1e") is 0: an exponent with no digits */
    }
    if ( neg ) {
        result = -result;
    }
    /* the type it is written as */
    if ( exp_letter == 'D' ) {
        *type = TY_DOUBLE;
    } else if ( exp_letter == 'E' ) {
        /* more digits than a SINGLE holds, or too big for one: DOUBLE */
        *type = sig > 7 || result > SNG_MAX || result < -SNG_MAX ? TY_DOUBLE : TY_SINGLE;
    } else if ( point ) {
        *type = sig > 7 ? TY_DOUBLE : TY_SINGLE;
    } else if ( result >= -32768.0 && result <= 32767.0 ) {
        *type = TY_INT;
    } else if ( result >= -2147483648.0 && result <= 2147483647.0 ) {
        *type = TY_LONG;
    } else {
        *type = sig > 7 ? TY_DOUBLE : TY_SINGLE;
    }
    if ( pos < len ) {
        switch ( text[pos] ) {
        case '%':   *type = TY_INT;     pos++;  break;
        case '&':   *type = TY_LONG;    pos++;  break;
        case '!':   *type = TY_SINGLE;  pos++;  break;
        case '#':   *type = TY_DOUBLE;  pos++;  break;
        }
    }
    *used = pos;
    return result;
}

double num_parse( const char *text, u32 len, u32 *used )
{
    int type, any;

    return scan_number( text, len, used, &type, &any );
}

/* A whole item as INPUT and READ want it: a number and nothing after it
   but blanks.  -> 0 if it is not one. */
int num_scan( const char *text, u32 len, double *val, int *type )
{
    u32 used;
    int any;

    *val = scan_number( text, len, &used, type, &any );
    if ( !any || *type < 0 ) {
        return 0;
    }
    while ( used < len && is_blank( text[used] ) ) {
        used++;
    }
    return used == len;
}
