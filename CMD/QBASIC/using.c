/*
 * USING.C - PRINT USING's format strings.
 *
 * A FORMAT IS LITERAL TEXT AND FIELDS.  Each value takes the literal
 * text up to the next field, then the field; when the format runs out
 * it starts again, and after the last value the literal text up to the
 * next field is written too ("## items" -> " 5 items").  A format with
 * no field at all is "Illegal function call" (after its text), a value
 * of the wrong kind for its field "Type mismatch".
 *
 *   numbers   # a digit   . the point   , thousands (before the point)
 *             + a sign at either end   - a minus at the end
 *             $$ a floating dollar   ** asterisk fill   **$ both
 *             ^^^^ or ^^^^^ an exponent (E for a SINGLE, D for a DOUBLE)
 *   strings   ! the first character   & all of it   \  \ that many
 *   _x        x itself
 *
 * ROUNDING is half away from zero on the value's 16-digit decimal form,
 * which is what QBasic does: .125 -> 0.13, but .145! -> 0.14, because a
 * SINGLE's .145 is 0.14499999582767487.  A number that does not fit is
 * written whole with % in front of it.
 */
#include "qb.h"

/* a piece of output: up to the longest string.  One buffer does for all
   of them (none is in use while another is), made when first needed */
#define OUTMAX  (STR_MAX + 1)

typedef struct {
    char *text;
    int   len;
} OUT;

static char *out_buf;

static void out_start( OUT *out )
{
    if ( out_buf == NULL ) {
        out_buf = (char *)try_alloc( OUTMAX );
        if ( out_buf == NULL ) {
            vm_error( E_OUT_OF_MEMORY );
        }
    }
    out->text = out_buf;
    out->len = 0;
}

static void out_ch( OUT *out, int ch )
{
    if ( out->len < OUTMAX - 1 ) {
        out->text[out->len++] = (char)ch;
    }
}

static void out_str( OUT *out, const char *text, int len )
{
    while ( len-- > 0 ) {
        out_ch( out, *text++ );
    }
}

/* the kind of field at p: 0 none, 'N' a number, '!', '&', '\\' */
static int field_at( const char *f, int len, int p )
{
    int ch = f[p], next = p + 1 < len ? f[p + 1] : 0, i;

    switch ( ch ) {
    case '#':
        return 'N';
    case '.':
        return next == '#' ? 'N' : 0;
    case '+':
        if ( next == '#' || (next == '.' && p + 2 < len && f[p + 2] == '#') ) {
            return 'N';
        }
        if ( (next == '$' && p + 2 < len && f[p + 2] == '$') || (next == '*' && p + 2 < len && f[p + 2] == '*') ) {
            return 'N';
        }
        return 0;
    case '$':
        return next == '$' ? 'N' : 0;
    case '*':
        return next == '*' ? 'N' : 0;
    case '!':
    case '&':
        return ch;
    case '\\':
        for ( i = p + 1; i < len && f[i] == ' '; i++ ) {
        }
        return i < len && f[i] == '\\' ? '\\' : 0;
    }
    return 0;
}

/* literal text from *pos to the next field, into out: -> the field's
   kind, 0 at the end of the format */
static int literal_to_field( const char *f, int len, int *pos, OUT *out )
{
    int kind;

    while ( *pos < len ) {
        if ( f[*pos] == '_' ) {
            if ( *pos + 1 < len ) {
                out_ch( out, f[*pos + 1] );
                *pos += 2;
            } else {
                out_ch( out, '_' );
                (*pos)++;
            }
            continue;
        }
        kind = field_at( f, len, *pos );
        if ( kind ) {
            return kind;
        }
        out_ch( out, f[(*pos)++] );
    }
    return 0;
}

/* does the format have a field anywhere? */
static int any_field( const char *f, int len )
{
    int p;

    for ( p = 0; p < len; p++ ) {
        if ( f[p] == '_' ) {
            p++;
            continue;
        }
        if ( field_at( f, len, p ) ) {
            return 1;
        }
    }
    return 0;
}

/* to the next field, starting the format again when it runs out */
static int next_field( STR *fmt, int *pos, OUT *out )
{
    const char *f = QS_TEXT( fmt );
    int len = (int)QS_LEN( fmt ), kind;

    kind = literal_to_field( f, len, pos, out );
    if ( kind == 0 ) {
        if ( !any_field( f, len ) ) {
            return 0;
        }
        *pos = 0;
        kind = literal_to_field( f, len, pos, out );
    }
    return kind;
}

static STR *out_string( OUT *out )
{
    return qs_make( out->text, (u32)out->len );
}

/* ------------------------------------------------------------------ */
/* numbers                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int lead_plus, trail_plus, trail_minus;
    int star, dollar, comma, point;
    int positions;              /* # and , before the point, and the $$ ** ones */
    int decimals;
    int carets;                 /* 0, 4 or 5 */
} NFIELD;

static int parse_number_field( const char *f, int len, int p, NFIELD *nf )
{
    mem_set( nf, 0, sizeof( *nf ) );
    if ( f[p] == '+' ) {
        nf->lead_plus = 1;
        p++;
    }
    if ( p + 2 < len && f[p] == '*' && f[p + 1] == '*' && f[p + 2] == '$' ) {
        nf->star = nf->dollar = 1;
        nf->positions = 3;
        p += 3;
    } else if ( p + 1 < len && f[p] == '*' && f[p + 1] == '*' ) {
        nf->star = 1;
        nf->positions = 2;
        p += 2;
    } else if ( p + 1 < len && f[p] == '$' && f[p + 1] == '$' ) {
        nf->dollar = 1;
        nf->positions = 2;
        p += 2;
    }
    while ( p < len && (f[p] == '#' || (f[p] == ',' && !nf->point)) ) {
        if ( f[p] == ',' ) {
            nf->comma = 1;
        }
        nf->positions++;
        p++;
    }
    if ( p < len && f[p] == '.' ) {
        nf->point = 1;
        p++;
        while ( p < len && f[p] == '#' ) {
            nf->decimals++;
            p++;
        }
    }
    if ( p + 3 < len && f[p] == '^' && f[p + 1] == '^' && f[p + 2] == '^' && f[p + 3] == '^' ) {
        nf->carets = (p + 4 < len && f[p + 4] == '^') ? 5 : 4;
        p += nf->carets;
    }
    if ( !nf->lead_plus && p < len ) {
        if ( f[p] == '+' ) {
            nf->trail_plus = 1;
            p++;
        } else if ( f[p] == '-' ) {
            nf->trail_minus = 1;
            p++;
        }
    }
    return p;
}

/* |v| as a string of "count" digits rounded half up at a place: digits
   holds the digits of the number from 10^top down to 10^-decimals.
   -> top (the power of ten of the first digit kept) */
static int place_digits( double v, int decimals, char *digits, int *count, int min_top )
{
    char sig[20];
    int n, e, top, i, last, carry;

    if ( v < 0 ) {
        v = -v;
    }
    if ( v == 0 ) {
        n = 0;
        e = 0;
    } else {
        n = num_sig_digits( v, 16, sig, &e );
    }
    top = e > min_top ? e : min_top;
    /* the digits from 10^top to 10^-decimals, then one more for rounding */
    *count = top + decimals + 1;
    if ( *count < 0 ) {
        *count = 0;
    }
    last = 0;
    for ( i = 0; i < *count + 1; i++ ) {
        int power = top - i, idx = e - power;
        int d = (v != 0 && idx >= 0 && idx < n) ? sig[idx] - '0' : 0;
        if ( i < *count ) {
            digits[i] = (char)('0' + d);
        } else {
            last = d;
        }
    }
    if ( last >= 5 ) {
        carry = 1;
        for ( i = *count - 1; i >= 0 && carry; i-- ) {
            if ( digits[i] == '9' ) {
                digits[i] = '0';
            } else {
                digits[i]++;
                carry = 0;
            }
        }
        if ( carry ) {
            /* 9.99 -> 10.00: one digit more at the front */
            mem_cpy( digits + 1, digits, (u32)*count );
            digits[0] = '1';
            (*count)++;
            top++;
        }
    }
    return top;
}

static void format_fixed( OUT *out, double v, NFIELD *nf, int pct_ok );
static void format_exp( OUT *out, double v, NFIELD *nf, int is_double, int overflowed );

static void format_number( OUT *out, double v, NFIELD *nf, int is_double )
{
    if ( nf->carets ) {
        format_exp( out, v, nf, is_double, 0 );
    } else {
        format_fixed( out, v, nf, is_double );
    }
}

static void format_fixed( OUT *out, double v, NFIELD *nf, int is_double )
{
    char digits[400], body[600];
    int count, top, i, blen = 0, int_digits, neg = v < 0, width, lead;

    top = place_digits( v, nf->decimals, digits, &count, 0 );
    int_digits = top + 1;                       /* digits before the point */
    /* the integer part, leading zeros off */
    lead = 0;
    while ( lead < int_digits - 1 && digits[lead] == '0' ) {
        lead++;
    }
    if ( neg && (nf->lead_plus || (!nf->trail_plus && !nf->trail_minus)) ) {
        /* a negative number that rounds to nothing is still shown with
           its minus, as QBasic does (-.001 -> -.00) */
    }
    if ( nf->lead_plus ) {
        body[blen++] = neg ? '-' : '+';
    } else if ( neg && !nf->trail_plus && !nf->trail_minus ) {
        body[blen++] = '-';
    }
    if ( nf->dollar ) {
        body[blen++] = '$';
    }
    if ( int_digits - lead == 1 && digits[lead] == '0' ) {
        /* a zero integer part: written only where there is room for it,
           or where there is no point to stand in for it */
        if ( !nf->point || blen < nf->positions + nf->lead_plus ) {
            body[blen++] = '0';
        }
    } else {
        for ( i = lead; i < int_digits; i++ ) {
            body[blen++] = digits[i];
            if ( nf->comma && i < int_digits - 1 && (int_digits - 1 - i) % 3 == 0 ) {
                body[blen++] = ',';
            }
        }
    }
    width = nf->positions + nf->lead_plus;
    if ( blen - width >= 5 ) {
        /* five or more characters too many: QBasic writes it as an
           exponent in the field instead */
        format_exp( out, v, nf, is_double, 1 );
        return;
    }
    if ( blen > width ) {
        out_ch( out, '%' );
    } else {
        for ( i = blen; i < width; i++ ) {
            out_ch( out, nf->star ? '*' : ' ' );
        }
    }
    out_str( out, body, blen );
    if ( nf->point ) {
        out_ch( out, '.' );
        out_str( out, digits + int_digits, nf->decimals );
    }
    if ( nf->trail_plus ) {
        out_ch( out, neg ? '-' : '+' );
    } else if ( nf->trail_minus ) {
        out_ch( out, neg ? '-' : ' ' );
    }
}

static void format_exp( OUT *out, double v, NFIELD *nf, int is_double, int overflowed )
{
    char sig[20], digits[64];
    int n = 0, e = 0, places, mant, reserve, explicit_sign, total, i, x, neg = v < 0;
    int maxsig = is_double ? 15 : 7, carry, last = 0, digit_slots, used = 0;
    double a = neg ? -v : v;

    explicit_sign = nf->lead_plus || nf->trail_plus || nf->trail_minus;
    digit_slots = nf->positions - (nf->dollar ? 1 : 0);
    reserve = (!explicit_sign && digit_slots > 1) ? 1 : 0;
    mant = digit_slots - reserve;
    if ( mant < 0 ) {
        mant = 0;
    }
    places = nf->decimals;
    if ( nf->point && mant + places > maxsig ) {
        /* with a point, the digits shown stop at the type's precision:
           the ones before the point go first (QBasic's own rule; a field
           of #s alone keeps them all) */
        mant = maxsig - places;
        if ( mant < 0 ) {
            mant = 0;
        }
    }
    total = mant + places;
    if ( a != 0 ) {
        n = num_sig_digits( a, 16, sig, &e );
    }
    /* the first digit kept is 10^x: the exponent is e - (mant - 1) */
    if ( total == 0 ) {
        total = 1;
    }
    for ( i = 0; i <= total; i++ ) {
        int d = (a != 0 && i < n) ? sig[i] - '0' : 0;
        if ( i < total ) {
            digits[i] = (char)('0' + d);
        } else {
            last = d;
        }
    }
    carry = a != 0 && last >= 5;
    for ( i = total - 1; i >= 0 && carry; i-- ) {
        if ( digits[i] == '9' ) {
            digits[i] = '0';
        } else {
            digits[i]++;
            carry = 0;
        }
    }
    if ( carry ) {
        digits[0] = '1';
        e++;
    }
    x = a == 0 ? 0 : (mant > 0 ? e - (mant - 1) : e + 1);
    if ( overflowed ) {
        out_ch( out, '%' );
    }
    if ( neg && !explicit_sign && !reserve ) {
        out_ch( out, '%' );                     /* no room for the minus */
    }
    if ( nf->lead_plus ) {
        out_ch( out, neg ? '-' : '+' );
    } else if ( !nf->trail_plus && !nf->trail_minus ) {
        if ( neg ) {
            out_ch( out, '-' );
        } else if ( reserve ) {
            out_ch( out, nf->star ? '*' : ' ' );
        }
    }
    if ( nf->dollar ) {
        out_ch( out, '$' );
    }
    if ( mant == 0 ) {
        if ( digit_slots > 0 && !reserve ) {
            out_ch( out, '0' );
        } else if ( digit_slots > reserve ) {
            out_ch( out, '0' );
        }
    }
    for ( i = 0; i < mant; i++ ) {
        out_ch( out, digits[used++] );
    }
    if ( nf->point ) {
        out_ch( out, '.' );
        for ( i = 0; i < places; i++ ) {
            out_ch( out, digits[used++] );
        }
    }
    out_ch( out, is_double ? 'D' : 'E' );
    out_ch( out, x < 0 ? '-' : '+' );
    if ( x < 0 ) {
        x = -x;
    }
    if ( nf->carets == 5 ) {
        out_ch( out, '0' + x / 100 % 10 );
        out_ch( out, '0' + x / 10 % 10 );
        out_ch( out, '0' + x % 10 );
    } else if ( x >= 100 ) {
        out_ch( out, '%' );
        out_ch( out, '0' + x % 10 );
    } else {
        out_ch( out, '0' + x / 10 );
        out_ch( out, '0' + x % 10 );
    }
    if ( nf->trail_plus ) {
        out_ch( out, neg ? '-' : '+' );
    } else if ( nf->trail_minus ) {
        out_ch( out, neg ? '-' : ' ' );
    }
}

STR *using_number( STR *fmt, int *pos, double v, int is_double )
{
    OUT out;
    NFIELD nf;
    int kind, save = *pos;

    /* find the field first: a value of the wrong kind writes nothing */
    out_start( &out );
    kind = next_field( fmt, pos, &out );
    if ( kind == 0 ) {
        STR *text = out_string( &out );
        *pos = save;
        con_text_then_error( text, E_ILLEGAL_CALL );
    }
    if ( kind != 'N' ) {
        *pos = save;
        vm_error( E_TYPE_MISMATCH );
    }
    *pos = parse_number_field( QS_TEXT( fmt ), (int)QS_LEN( fmt ), *pos, &nf );
    if ( nf.lead_plus + nf.positions + nf.point + nf.decimals > 24 ) {
        /* 24 characters at most, as QBasic allows */
        *pos = save;
        vm_error( E_ILLEGAL_CALL );
    }
    format_number( &out, v, &nf, is_double );
    return out_string( &out );
}

STR *using_string( STR *fmt, int *pos, STR *s )
{
    OUT out;
    int kind, save = *pos, width, i, len = (int)QS_LEN( s );
    const char *f = QS_TEXT( fmt );

    out_start( &out );
    kind = next_field( fmt, pos, &out );
    if ( kind == 0 ) {
        STR *text = out_string( &out );
        qs_release( s );
        *pos = save;
        con_text_then_error( text, E_ILLEGAL_CALL );
    }
    if ( kind == 'N' ) {
        qs_release( s );
        *pos = save;
        vm_error( E_TYPE_MISMATCH );
    }
    switch ( kind ) {
    case '!':
        out_ch( &out, len > 0 ? s->text[0] : ' ' );
        (*pos)++;
        break;
    case '&':
        out_str( &out, QS_TEXT( s ), len );
        (*pos)++;
        break;
    default:
        width = 2;
        for ( i = *pos + 1; f[i] == ' '; i++ ) {
            width++;
        }
        *pos = i + 1;
        for ( i = 0; i < width; i++ ) {
            out_ch( &out, i < len ? s->text[i] : ' ' );
        }
        break;
    }
    qs_release( s );
    return out_string( &out );
}

/* after the last value: the text up to the next field */
STR *using_tail( STR *fmt, int *pos )
{
    OUT out;

    out_start( &out );
    literal_to_field( QS_TEXT( fmt ), (int)QS_LEN( fmt ), pos, &out );
    return out_string( &out );
}
