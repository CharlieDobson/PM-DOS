/*
 * QSTR.C - string values, and the runtime's string routines.
 *
 * A STRING IS COUNTED AND SHARED.  Copying one - into a variable, onto
 * the stack, into an array - takes a reference, not the bytes; a string
 * is only copied when one of its holders changes it in place (MID$ =,
 * LSET, a FIELD), and only if someone else holds it too (qs_unique).
 * NULL is the empty string, so most strings a program makes and drops
 * cost nothing at all.
 *
 * The routines follow QBasic on the edges: a length below 0 or above
 * 32,767 is "Illegal function call", a string longer than 32,767 bytes
 * is "Out of string space".
 */
#include "qb.h"

static u32 live_bytes;           /* the text of the strings a program holds */

/* for FRE: the bytes of text in strings now */
u32 qs_bytes( void )
{
    return live_bytes;
}

STR *qs_new( u32 len )
{
    STR *s;

    if ( len == 0 ) {
        return NULL;
    }
    if ( len > STR_MAX ) {
        vm_error( E_OUT_OF_STRSPACE );
    }
    s = (STR *)try_alloc( sizeof( STR ) - 4 + len );
    if ( s == NULL ) {
        vm_error( E_OUT_OF_STRSPACE );
    }
    s->ref = 1;
    s->len = len;
    live_bytes += len;
    return s;
}

STR *qs_make( const char *text, u32 len )
{
    STR *s = qs_new( len );

    if ( s != NULL ) {
        mem_cpy( s->text, text, len );
    }
    return s;
}

/* for the compiler, which has no run to raise an error in: *ok = 0 when
   there is no memory; "text" NULL leaves the bytes to the caller */
STR *qs_try_make( const char *text, u32 len, int *ok )
{
    STR *s;

    *ok = 1;
    if ( len == 0 ) {
        return NULL;
    }
    s = (STR *)try_alloc( sizeof( STR ) - 4 + len );
    if ( s == NULL ) {
        *ok = 0;
        return NULL;
    }
    s->ref = 1;
    s->len = len;
    live_bytes += len;
    if ( text != NULL ) {
        mem_cpy( s->text, text, len );
    }
    return s;
}

STR *qs_cstr( const char *text )
{
    return qs_make( text, str_len( text ) );
}

/* a constant of the program: never freed while the program is */
STR *qs_perm( const char *text, u32 len )
{
    STR *s;

    if ( len == 0 ) {
        return NULL;
    }
    s = (STR *)try_alloc( sizeof( STR ) - 4 + len );
    if ( s == NULL ) {
        return NULL;
    }
    s->ref = QS_PERM;
    s->len = len;
    mem_cpy( s->text, text, len );
    return s;
}

void qs_release( STR *s )
{
    if ( s != NULL && --s->ref == 0 ) {
        live_bytes -= s->len;
        xfree( s );
    }
}

STR *qs_retain( STR *s )
{
    if ( s != NULL ) {
        s->ref++;
    }
    return s;
}

/* a string this holder may change: the same one if nobody else has it */
STR *qs_unique( STR *s )
{
    STR *copy;

    if ( s == NULL || s->ref == 1 ) {
        return s;
    }
    copy = qs_make( s->text, s->len );
    qs_release( s );
    return copy;
}

STR *qs_cat( STR *lhs, STR *rhs )
{
    STR *s;
    u32 llen = QS_LEN( lhs ), rlen = QS_LEN( rhs );

    if ( rlen == 0 ) {
        return lhs;
    }
    if ( llen == 0 ) {
        return rhs;
    }
    if ( llen + rlen > STR_MAX ) {
        qs_release( lhs );
        qs_release( rhs );
        vm_error( E_OUT_OF_STRSPACE );
    }
    if ( lhs->ref == 1 && mem_size( lhs ) >= sizeof( STR ) - 4 + llen + rlen ) {
        /* a + b where nobody else has a, and there is room behind it */
        mem_cpy( lhs->text + llen, rhs->text, rlen );
        lhs->len += rlen;
        live_bytes += rlen;
        qs_release( rhs );
        return lhs;
    }
    s = (STR *)try_alloc( sizeof( STR ) - 4 + llen + rlen + (llen + rlen) / 2 );
    if ( s == NULL ) {
        s = (STR *)try_alloc( sizeof( STR ) - 4 + llen + rlen );
    }
    if ( s == NULL ) {
        qs_release( lhs );
        qs_release( rhs );
        vm_error( E_OUT_OF_STRSPACE );
    }
    s->ref = 1;
    s->len = llen + rlen;
    live_bytes += llen + rlen;
    mem_cpy( s->text, lhs->text, llen );
    mem_cpy( s->text + llen, rhs->text, rlen );
    qs_release( lhs );
    qs_release( rhs );
    return s;
}

int qs_cmp( STR *lhs, STR *rhs )
{
    u32 llen = QS_LEN( lhs ), rlen = QS_LEN( rhs ), i, n = llen < rlen ? llen : rlen;
    const u8 *ltext = (const u8 *)QS_TEXT( lhs ), *rtext = (const u8 *)QS_TEXT( rhs );

    for ( i = 0; i < n; i++ ) {
        if ( ltext[i] != rtext[i] ) {
            return (int)ltext[i] - (int)rtext[i];
        }
    }
    return llen < rlen ? -1 : llen > rlen ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* the routines                                                        */
/* ------------------------------------------------------------------ */

static void bad_call( void )
{
    vm_error( E_ILLEGAL_CALL );
}

/* a count argument: 0 to 32,767 */
static u32 count_arg( s32 val )
{
    if ( val < 0 || val > STR_MAX ) {
        bad_call();
    }
    return (u32)val;
}

void rtf_LEFT( void )
{
    u32 n = count_arg( POP_L() );
    STR *s = POP_STR();

    if ( n >= QS_LEN( s ) ) {
        PUSH_STR( s );
        return;
    }
    PUSH_STR( qs_make( QS_TEXT( s ), n ) );
    qs_release( s );
}

void rtf_RIGHT( void )
{
    u32 n = count_arg( POP_L() );
    STR *s = POP_STR();
    u32 len = QS_LEN( s );

    if ( n >= len ) {
        PUSH_STR( s );
        return;
    }
    PUSH_STR( qs_make( QS_TEXT( s ) + len - n, n ) );
    qs_release( s );
}

static void mid( STR *s, s32 start, s32 count )
{
    u32 len = QS_LEN( s );

    if ( start < 1 || start > STR_MAX ) {
        qs_release( s );
        bad_call();
    }
    if ( count < 0 || count > STR_MAX ) {
        qs_release( s );
        bad_call();
    }
    if ( (u32)start > len ) {
        PUSH_STR( NULL );
        qs_release( s );
        return;
    }
    if ( (u32)count > len - (u32)start + 1 ) {
        count = (s32)(len - (u32)start + 1);
    }
    if ( start == 1 && (u32)count == len ) {
        PUSH_STR( s );
        return;
    }
    PUSH_STR( qs_make( s->text + start - 1, (u32)count ) );
    qs_release( s );
}

void rtf_MID2( void )
{
    s32 start = POP_L();
    STR *s = POP_STR();

    mid( s, start, STR_MAX );
}

void rtf_MID3( void )
{
    s32 count = POP_L();
    s32 start = POP_L();
    STR *s = POP_STR();

    mid( s, start, count );
}

void rtf_LENSTR( void )
{
    STR *s = POP_STR();

    PUSH_L( (s32)QS_LEN( s ) );
    qs_release( s );
}

static s32 instr( s32 start, STR *hay, STR *needle )
{
    u32 hlen = QS_LEN( hay ), nlen = QS_LEN( needle ), pos;

    if ( start < 1 || start > STR_MAX ) {
        qs_release( hay );
        qs_release( needle );
        bad_call();
    }
    if ( (u32)start > hlen ) {
        return 0;
    }
    if ( nlen == 0 ) {
        return start;
    }
    for ( pos = (u32)start - 1; pos + nlen <= hlen; pos++ ) {
        if ( hay->text[pos] == needle->text[0] && mem_cmp( hay->text + pos, needle->text, nlen ) == 0 ) {
            return (s32)pos + 1;
        }
    }
    return 0;
}

void rtf_INSTR2( void )
{
    STR *needle = POP_STR();
    STR *hay = POP_STR();
    s32 at = instr( 1, hay, needle );

    qs_release( hay );
    qs_release( needle );
    PUSH_L( at );
}

void rtf_INSTR3( void )
{
    STR *needle = POP_STR();
    STR *hay = POP_STR();
    s32 start = POP_L();
    s32 at = instr( start, hay, needle );

    qs_release( hay );
    qs_release( needle );
    PUSH_L( at );
}

/* each byte through a mapping: upper or lower case */
static void map_case( int upper )
{
    STR *s = POP_STR(), *out;
    u32 i, len = QS_LEN( s );
    int changed = 0;

    for ( i = 0; i < len; i++ ) {
        int ch = (u8)s->text[i];
        if ( upper ? (ch >= 'a' && ch <= 'z') : (ch >= 'A' && ch <= 'Z') ) {
            changed = 1;
            break;
        }
    }
    if ( !changed ) {
        PUSH_STR( s );
        return;
    }
    out = qs_make( s->text, len );
    qs_release( s );
    for ( i = 0; i < len; i++ ) {
        out->text[i] = (char)(upper ? ch_upper( out->text[i] ) : ch_lower( out->text[i] ));
    }
    PUSH_STR( out );
}

void rtf_LCASE( void )
{
    map_case( 0 );
}

void rtf_UCASE( void )
{
    map_case( 1 );
}

void rtf_LTRIM( void )
{
    STR *s = POP_STR();
    u32 len = QS_LEN( s ), skip = 0;

    while ( skip < len && s->text[skip] == ' ' ) {
        skip++;
    }
    if ( skip == 0 ) {
        PUSH_STR( s );
        return;
    }
    PUSH_STR( qs_make( s->text + skip, len - skip ) );
    qs_release( s );
}

void rtf_RTRIM( void )
{
    STR *s = POP_STR();
    u32 len = QS_LEN( s ), keep = len;

    while ( keep > 0 && s->text[keep - 1] == ' ' ) {
        keep--;
    }
    if ( keep == len ) {
        PUSH_STR( s );
        return;
    }
    PUSH_STR( qs_make( s->text, keep ) );
    qs_release( s );
}

static STR *repeat( u32 count, int ch )
{
    STR *s = qs_new( count );

    if ( s != NULL ) {
        mem_set( s->text, ch, count );
    }
    return s;
}

void rtf_SPACE( void )
{
    PUSH_STR( repeat( count_arg( POP_L() ), ' ' ) );
}

void rtf_STRINGN( void )
{
    s32 code = POP_L();
    u32 count = count_arg( POP_L() );

    PUSH_STR( repeat( count, (int)(code & 255) ) );   /* 256 is 0, -1 is 255 */
}

void rtf_STRINGS( void )
{
    STR *s = POP_STR();
    s32 count = POP_L();
    int ch;

    if ( QS_LEN( s ) == 0 ) {
        bad_call();
    }
    ch = (u8)s->text[0];
    qs_release( s );
    PUSH_STR( repeat( count_arg( count ), ch ) );
}

void rtf_CHR( void )
{
    s32 code = POP_L();
    char ch;

    if ( code < 0 || code > 255 ) {
        bad_call();
    }
    ch = (char)code;
    PUSH_STR( qs_make( &ch, 1 ) );
}

void rtf_ASC( void )
{
    STR *s = POP_STR();
    int ch;

    if ( QS_LEN( s ) == 0 ) {
        bad_call();
    }
    ch = (u8)s->text[0];
    qs_release( s );
    PUSH_L( ch );
}

/* STR$: a number as PRINT writes it, with the blank for its sign but
   not the one PRINT puts after it */
static void push_str_num( const char *digits )
{
    char buf[48];

    if ( digits[0] == '-' ) {
        str_cpy( buf, digits );
    } else {
        buf[0] = ' ';
        str_cpy( buf + 1, digits );
    }
    PUSH_STR( qs_cstr( buf ) );
}

void rtf_STRL( void )
{
    char digits[40];

    num_fmt_l( POP_L(), digits );
    push_str_num( digits );
}

void rtf_STRS( void )
{
    char digits[40];

    num_fmt_s( pop_single(), digits );
    push_str_num( digits );
}

void rtf_STRD( void )
{
    char digits[40];

    num_fmt_d( POP_D(), digits );
    push_str_num( digits );
}

void rtf_VAL( void )
{
    STR *s = POP_STR();
    u32 used;
    double v = num_parse( QS_TEXT( s ), QS_LEN( s ), &used );

    qs_release( s );
    if ( DBL_BAD( v ) ) {
        vm_error( E_OVERFLOW );
    }
    PUSH_D( v );
}

/* HEX$ and OCT$: an INTEGER's bits are 16, anything else's 32; the type
   of the argument is pushed after it */
static void radix( int shift )
{
    int kind = (int)POP_L();
    s32 v = POP_L();
    char buf[16];
    u32 bits;
    int count = 0, mask = (1 << shift) - 1;
    char digits[16];

    if ( kind == TY_INT ) {
        bits = (u32)v & 0xFFFFUL;
    } else {
        bits = (u32)v;
    }
    do {
        digits[count++] = "0123456789ABCDEF"[bits & (u32)mask];
        bits >>= shift;
    } while ( bits );
    for ( mask = 0; count; ) {
        buf[mask++] = digits[--count];
    }
    PUSH_STR( qs_make( buf, (u32)mask ) );
}

void rtf_HEX( void )
{
    radix( 4 );
}

void rtf_OCT( void )
{
    radix( 3 );
}

static void push_bytes( const void *bytes, u32 len )
{
    PUSH_STR( qs_make( (const char *)bytes, len ) );
}

void rtf_MKI( void )
{
    s16 v = (s16)POP_L();

    push_bytes( &v, 2 );
}

void rtf_MKL( void )
{
    s32 v = POP_L();

    push_bytes( &v, 4 );
}

void rtf_MKS( void )
{
    float v;

    if ( opt_mbf ) {
        rtf_MKSMBF();                           /* QBASIC /MBF */
        return;
    }
    v = POP_S();
    push_bytes( &v, 4 );
}

void rtf_MKD( void )
{
    double v;

    if ( opt_mbf ) {
        rtf_MKDMBF();
        return;
    }
    v = POP_D();
    push_bytes( &v, 8 );
}

/* MICROSOFT BINARY FORMAT, the floating point of GW-BASIC and QuickBASIC
   before 4.0: an exponent byte biased by 129 at the top (0 = zero), then
   the sign, then the mantissa without its leading 1 - for a single the
   same 23 bits IEEE has, for a double 55 bits. */
void rtf_MKSMBF( void )
{
    float v = POP_S();
    u32 ieee = *(u32 *)&v, mbf;
    u32 exp = (ieee >> 23) & 0xFF;

    if ( exp == 0 ) {
        mbf = 0;                                /* zero, and the denormals with it */
    } else {
        exp += 2;                               /* 127 bias -> 129, one less for the point */
        if ( exp > 255 ) {
            vm_error( E_OVERFLOW );
        }
        mbf = (exp << 24) | ((ieee >> 8) & 0x800000UL) | (ieee & 0x7FFFFFUL);
    }
    push_bytes( &mbf, 4 );
}

void rtf_MKDMBF( void )
{
    double v = POP_D();
    u32 lo = ((u32 *)&v)[0], hi = ((u32 *)&v)[1];
    u32 exp = (hi >> 20) & 0x7FF;
    u32 out[2];

    if ( exp == 0 ) {
        out[0] = out[1] = 0;
    } else {
        s32 mexp = (s32)exp - 1023 + 129;
        if ( mexp > 255 ) {
            vm_error( E_OVERFLOW );
        }
        if ( mexp <= 0 ) {
            out[0] = out[1] = 0;
        } else {
            /* 52 bits of IEEE mantissa into 55: three zero bits below */
            u32 mhi = hi & 0xFFFFFUL;               /* 20 bits */
            out[0] = lo << 3;
            out[1] = ((u32)mexp << 24) | ((hi >> 8) & 0x800000UL) | (mhi << 3) | (lo >> 29);
        }
    }
    push_bytes( out, 8 );
}

/* CVI and the rest: the string must have the bytes */
static const char *bytes_of( STR *s, u32 need )
{
    if ( QS_LEN( s ) < need ) {
        qs_release( s );
        bad_call();
    }
    return s->text;
}

void rtf_CVI( void )
{
    STR *s = POP_STR();
    s16 v = *(const s16 *)bytes_of( s, 2 );

    qs_release( s );
    PUSH_L( v );
}

void rtf_CVL( void )
{
    STR *s = POP_STR();
    s32 v = *(const s32 *)bytes_of( s, 4 );

    qs_release( s );
    PUSH_L( v );
}

void rtf_CVS( void )
{
    STR *s;
    float v;

    if ( opt_mbf ) {
        rtf_CVSMBF();
        return;
    }
    s = POP_STR();
    v = *(const float *)bytes_of( s, 4 );
    qs_release( s );
    if ( (*(u32 *)&v & 0x7F800000UL) == 0x7F800000UL ) {
        v = 0;                                  /* not a number: QBasic's has none */
    }
    PUSH_S( v );
}

void rtf_CVD( void )
{
    STR *s;
    double v;

    if ( opt_mbf ) {
        rtf_CVDMBF();
        return;
    }
    s = POP_STR();
    v = *(const double *)bytes_of( s, 8 );
    qs_release( s );
    if ( DBL_BAD( v ) ) {
        v = 0;
    }
    PUSH_D( v );
}

void rtf_CVSMBF( void )
{
    STR *s = POP_STR();
    u32 mbf = *(const u32 *)bytes_of( s, 4 ), ieee, exp = mbf >> 24;
    float v;

    qs_release( s );
    if ( exp <= 2 ) {
        v = 0;                                  /* zero, or too small for IEEE */
    } else {
        ieee = ((mbf << 8) & 0x80000000UL) | ((exp - 2) << 23) | (mbf & 0x7FFFFFUL);
        v = *(float *)&ieee;
    }
    PUSH_S( v );
}

void rtf_CVDMBF( void )
{
    STR *s = POP_STR();
    const u32 *in = (const u32 *)bytes_of( s, 8 );
    u32 lo = in[0], hi = in[1], exp = hi >> 24;
    u32 out[2];

    qs_release( s );
    if ( exp == 0 ) {
        PUSH_D( 0.0 );
        return;
    }
    {
        s32 iexp = (s32)exp - 129 + 1023;
        out[0] = (lo >> 3) | ((hi & 7) << 29);
        out[1] = ((hi << 8) & 0x80000000UL) | ((u32)iexp << 20) | ((hi >> 3) & 0xFFFFFUL);
    }
    PUSH_D( *(double *)out );
}

/* MID$(v$, start [, n]) = x$: bytes of the variable replaced in place;
   its length never changes */
static void mid_into( char *text, u32 len, s32 start, s32 count, STR *src )
{
    u32 n;

    if ( start < 1 || (u32)start > len || count < -1 ) {
        qs_release( src );
        bad_call();
    }
    n = QS_LEN( src );
    if ( count >= 0 && (u32)count < n ) {
        n = (u32)count;
    }
    if ( n > len - (u32)start + 1 ) {
        n = len - (u32)start + 1;
    }
    mem_cpy( text + start - 1, QS_TEXT( src ), n );
    qs_release( src );
}

void rtf_MIDSETV( void )
{
    STR *src = POP_STR();
    s32 count = POP_L();
    s32 start = POP_L();
    STR **var = (STR **)POP_A();
    STR *s;

    s = qs_unique( *var );
    *var = s;
    mid_into( s ? s->text : NULL, QS_LEN( s ), start, count, src );
    file_field_changed( var );                  /* a FIELD's variable: its buffer too */
}

void rtf_MIDSETF( void )
{
    STR *src = POP_STR();
    s32 count = POP_L();
    s32 start = POP_L();
    u32 width = (u32)POP_L();
    char *field = (char *)POP_A();

    mid_into( field, width, start, count, src );
}

/* LSET and RSET into a field of "width" bytes */
static void set_justified( char *field, u32 width, STR *src, int right )
{
    u32 n = QS_LEN( src );

    if ( n > width ) {
        n = width;
    }
    if ( right ) {
        mem_set( field, ' ', width - n );
        mem_cpy( field + width - n, QS_TEXT( src ), n );
    } else {
        mem_cpy( field, QS_TEXT( src ), n );
        mem_set( field + n, ' ', width - n );
    }
    qs_release( src );
}

void file_field_changed( STR **var );

static void set_var( int right )
{
    STR *src = POP_STR();
    STR **var = (STR **)POP_A();
    STR *s = qs_unique( *var );

    *var = s;
    if ( s == NULL ) {
        qs_release( src );
        return;
    }
    set_justified( s->text, s->len, src, right );
    file_field_changed( var );
}

void rtf_LSETV( void )
{
    set_var( 0 );
}

void rtf_RSETV( void )
{
    set_var( 1 );
}

void rtf_LSETF( void )
{
    STR *src = POP_STR();
    u32 width = (u32)POP_L();
    char *field = (char *)POP_A();

    set_justified( field, width, src, 0 );
}

void rtf_RSETF( void )
{
    STR *src = POP_STR();
    u32 width = (u32)POP_L();
    char *field = (char *)POP_A();

    set_justified( field, width, src, 1 );
}

/* LSET rec1 = rec2: the bytes, cut to the shorter, the rest blanks */
void rtf_LSETREC( void )
{
    u32 slen = (u32)POP_L();
    u8 *src = (u8 *)POP_A();
    u32 dlen = (u32)POP_L();
    u8 *dst = (u8 *)POP_A();

    if ( slen >= dlen ) {
        mem_cpy( dst, src, dlen );
    } else {
        mem_cpy( dst, src, slen );
        mem_set( dst + slen, ' ', dlen - slen );
    }
}
