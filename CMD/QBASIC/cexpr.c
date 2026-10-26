/*
 * CEXPR.C - expressions, variables and calls.
 *
 * PRECEDENCE, loosest first: IMP, EQV, XOR, OR, AND, NOT, the
 * comparisons, + and -, MOD, \, * and /, a sign, ^.  A sign binds more
 * loosely than ^, so -2^2 is -4; after ^ a sign is allowed (2^-1).
 *
 * TYPES: arithmetic is done in the wider of the two types, INTEGER <
 * LONG < SINGLE < DOUBLE; / and ^ give a SINGLE unless a DOUBLE is
 * involved; \, MOD and the logical operators round their operands to
 * INTEGER (both were INTEGER) or LONG.  The left operand is on the stack
 * before the right one's type is known, so it is widened in place under
 * the top (OP_UNDER_*).  A comparison is an INTEGER, -1 or 0.
 *
 * A VARIABLE comes back from c_var() as a VREF: a plain scalar in the
 * module's data or the frame is left "simple" - nothing emitted - so
 * that loading or storing it is one instruction; anything else (an
 * element, a field reached through a parameter, a static) has its
 * address emitted at once and is used through it.
 */
#include "comp.h"

static QTYPE parse_imp( void );
static QTYPE parse_unary( void );

QTYPE value_type( QTYPE vtype )
{
    return TKIND( vtype ) == TY_FSTR ? TY_STR : vtype;
}

static int num_rank( QTYPE type )
{
    return TKIND( type );                       /* INT < LONG < SINGLE < DOUBLE */
}

static void need_num( QTYPE type )
{
    if ( !IS_NUMT( type ) ) {
        c_fail_tok( "Type mismatch" );
    }
}

static void need_str( QTYPE type )
{
    if ( TKIND( type ) != TY_STR ) {
        c_fail_tok( "Type mismatch" );
    }
}

void c_convert( QTYPE from, QTYPE to )
{
    int f = TKIND( from ), t = TKIND( to );

    if ( t == TY_FSTR ) {
        t = TY_STR;
    }
    if ( f == TY_FSTR ) {
        f = TY_STR;
    }
    if ( f == t ) {
        return;
    }
    if ( f == TY_STR || t == TY_STR || f == TY_REC || t == TY_REC ) {
        c_fail_tok( "Type mismatch" );
    }
    switch ( f ) {
    case TY_INT:
        if ( t == TY_SINGLE ) {
            c_op( OP_L2S );
        } else if ( t == TY_DOUBLE ) {
            c_op( OP_L2D );
        }
        break;
    case TY_LONG:
        if ( t == TY_INT ) {
            c_op( OP_L2I );
        } else if ( t == TY_SINGLE ) {
            c_op( OP_L2S );
        } else if ( t == TY_DOUBLE ) {
            c_op( OP_L2D );
        }
        break;
    case TY_SINGLE:
        c_op( t == TY_INT ? OP_S2I : t == TY_LONG ? OP_S2L : OP_S2D );
        break;
    case TY_DOUBLE:
        c_op( t == TY_INT ? OP_D2I : t == TY_LONG ? OP_D2L : OP_D2S );
        break;
    }
}

/* the operand under the top widened to "to" */
static void convert_under( QTYPE from, QTYPE to )
{
    int f = TKIND( from ), t = TKIND( to );

    if ( f == t || (f == TY_INT && t == TY_LONG) ) {
        return;
    }
    if ( t == TY_SINGLE ) {
        c_op( OP_UNDER_L2S );
    } else if ( t == TY_DOUBLE ) {
        c_op( f == TY_SINGLE ? OP_UNDER_S2D : OP_UNDER_L2D );
    } else if ( t == TY_LONG || t == TY_INT ) {
        if ( f == TY_SINGLE ) {
            c_op( OP_UNDER_S2L );
        } else if ( f == TY_DOUBLE ) {
            c_op( OP_UNDER_D2L );
        }
    }
}

/* Both operands to one numeric type, returned.  The wider of the two,
   except that a LONG with a SINGLE is a DOUBLE (a SINGLE cannot hold
   every LONG), and for / and ^ (at_least SINGLE) a LONG makes the
   answer a DOUBLE too: 100000& / 3 is 33333.33333333334. */
static QTYPE unify( QTYPE left, QTYPE right, int at_least )
{
    int lk = TKIND( left ), rk = TKIND( right );
    int want = num_rank( left ) > num_rank( right ) ? lk : rk;

    if ( (lk == TY_LONG && rk == TY_SINGLE) || (lk == TY_SINGLE && rk == TY_LONG) ) {
        want = TY_DOUBLE;
    }
    if ( at_least == TY_SINGLE && (lk == TY_LONG || rk == TY_LONG) ) {
        want = TY_DOUBLE;
    }
    if ( want < at_least ) {
        want = at_least;
    }
    convert_under( left, want );
    c_convert( right, want );
    return want;
}

/* \, MOD and the logical operators: INTEGER if both are, else LONG */
static QTYPE unify_whole( QTYPE left, QTYPE right )
{
    int want = (TKIND( left ) == TY_INT && TKIND( right ) == TY_INT) ? TY_INT : TY_LONG;

    if ( TKIND( left ) == TY_SINGLE || TKIND( left ) == TY_DOUBLE ) {
        convert_under( left, TY_LONG );
    }
    if ( TKIND( right ) != TY_INT ) {
        c_convert( right, TY_LONG );
    }
    return want;
}

static QTYPE logic_op( QTYPE left, QTYPE right, int op )
{
    QTYPE type;

    need_num( left );
    need_num( right );
    type = unify_whole( left, right );
    c_op( op );
    return type;
}

static QTYPE parse_imp( void );

static QTYPE parse_not( void )
{
    QTYPE type;

    if ( c_accept_kw( KW_NOT ) ) {
        type = parse_not();
        need_num( type );
        if ( TKIND( type ) == TY_SINGLE || TKIND( type ) == TY_DOUBLE ) {
            c_convert( type, TY_LONG );
            type = TY_LONG;
        }
        c_op( OP_NOT );
        return type;
    }
    {
        /* the comparisons */
        QTYPE left, right;
        int cc_code, kind;

        extern QTYPE parse_add( void );
        left = parse_add();
        for ( ;; ) {
            kind = c_tok()->kind;
            switch ( kind ) {
            case TK_EQ: cc_code = CC_EQ; break;
            case TK_NE: cc_code = CC_NE; break;
            case TK_LT: cc_code = CC_LT; break;
            case TK_GT: cc_code = CC_GT; break;
            case TK_LE: cc_code = CC_LE; break;
            case TK_GE: cc_code = CC_GE; break;
            default:
                return left;
            }
            c_next();
            right = parse_add();
            if ( TKIND( left ) == TY_STR || TKIND( right ) == TY_STR ) {
                need_str( left );
                need_str( right );
                c_op( OP_CMPSTR );
            } else {
                QTYPE type = unify( left, right, TY_INT );
                c_op( TKIND( type ) == TY_DOUBLE ? OP_CMPD : TKIND( type ) == TY_SINGLE ? OP_CMPS : OP_CMPL );
            }
            c_emit( cc_code );
            left = TY_INT;
        }
    }
}

static QTYPE parse_and( void )
{
    QTYPE left = parse_not();

    while ( c_accept_kw( KW_AND ) ) {
        left = logic_op( left, parse_not(), OP_AND );
    }
    return left;
}

static QTYPE parse_or( void )
{
    QTYPE left = parse_and();

    while ( c_accept_kw( KW_OR ) ) {
        left = logic_op( left, parse_and(), OP_OR );
    }
    return left;
}

static QTYPE parse_xor( void )
{
    QTYPE left = parse_or();

    while ( c_accept_kw( KW_XOR ) ) {
        left = logic_op( left, parse_or(), OP_XOR );
    }
    return left;
}

static QTYPE parse_eqv( void )
{
    QTYPE left = parse_xor();

    while ( c_accept_kw( KW_EQV ) ) {
        left = logic_op( left, parse_xor(), OP_EQV );
    }
    return left;
}

static QTYPE parse_imp( void )
{
    QTYPE left = parse_eqv();

    while ( c_accept_kw( KW_IMP ) ) {
        left = logic_op( left, parse_eqv(), OP_IMP );
    }
    return left;
}

static QTYPE parse_pow( void );

static QTYPE parse_mul( void )
{
    QTYPE left = parse_unary(), right, type;
    int kind;

    for ( ;; ) {
        kind = c_tok()->kind;
        if ( kind != TK_STAR && kind != TK_SLASH ) {
            return left;
        }
        c_next();
        right = parse_unary();
        need_num( left );
        need_num( right );
        if ( kind == TK_STAR ) {
            type = unify( left, right, TY_INT );
            switch ( TKIND( type ) ) {
            case TY_INT:    c_op( OP_MULI ); break;
            case TY_LONG:   c_op( OP_MULL ); break;
            case TY_SINGLE: c_op( OP_MULS ); break;
            default:        c_op( OP_MULD ); break;
            }
        } else {
            type = unify( left, right, TY_SINGLE );
            c_op( TKIND( type ) == TY_DOUBLE ? OP_DIVD : OP_DIVS );
        }
        left = type;
    }
}

static QTYPE parse_idiv( void )
{
    QTYPE left = parse_mul(), right, type;

    while ( c_accept( TK_BSLASH ) ) {
        right = parse_mul();
        need_num( left );
        need_num( right );
        type = unify_whole( left, right );
        c_op( TKIND( type ) == TY_INT ? OP_IDIVI : OP_IDIVL );
        left = type;
    }
    return left;
}

static QTYPE parse_mod( void )
{
    QTYPE left = parse_idiv(), right, type;

    while ( c_accept_kw( KW_MOD ) ) {
        right = parse_idiv();
        need_num( left );
        need_num( right );
        type = unify_whole( left, right );
        c_op( TKIND( type ) == TY_INT ? OP_MODI : OP_MODL );
        left = type;
    }
    return left;
}

QTYPE parse_add( void )
{
    QTYPE left = parse_mod(), right, type;
    int kind;

    for ( ;; ) {
        kind = c_tok()->kind;
        if ( kind != TK_PLUS && kind != TK_MINUS ) {
            return left;
        }
        c_next();
        right = parse_mod();
        if ( kind == TK_PLUS && (TKIND( left ) == TY_STR || TKIND( right ) == TY_STR) ) {
            need_str( left );
            need_str( right );
            c_op( OP_CAT );
            left = TY_STR;
            continue;
        }
        need_num( left );
        need_num( right );
        type = unify( left, right, TY_INT );
        switch ( TKIND( type ) ) {
        case TY_INT:    c_op( kind == TK_PLUS ? OP_ADDI : OP_SUBI ); break;
        case TY_LONG:   c_op( kind == TK_PLUS ? OP_ADDL : OP_SUBL ); break;
        case TY_SINGLE: c_op( kind == TK_PLUS ? OP_ADDS : OP_SUBS ); break;
        default:        c_op( kind == TK_PLUS ? OP_ADDD : OP_SUBD ); break;
        }
        left = type;
    }
}

static void negate( QTYPE type )
{
    switch ( TKIND( type ) ) {
    case TY_INT:    c_op( OP_NEGI ); break;
    case TY_LONG:   c_op( OP_NEGL ); break;
    case TY_SINGLE: c_op( OP_NEGS ); break;
    case TY_DOUBLE: c_op( OP_NEGD ); break;
    default:        c_fail_tok( "Type mismatch" );
    }
}

/* a sign: it takes in a whole ^ chain, so -2^2 is -(2^2) */
static QTYPE parse_unary( void )
{
    QTYPE type;

    if ( c_is( TK_MINUS ) ) {
        c_tok()->flags |= TF_UNARY;
        c_next();
        type = parse_unary();
        negate( type );
        return type;
    }
    if ( c_is( TK_PLUS ) ) {
        c_tok()->flags |= TF_UNARY;
        c_next();
        type = parse_unary();
        need_num( type );
        return type;
    }
    return parse_pow();
}

static QTYPE parse_atom( void );

static QTYPE parse_pow( void )
{
    QTYPE left = parse_atom(), right, type;
    int neg;

    while ( c_accept( TK_CARET ) ) {
        neg = 0;
        while ( c_is( TK_MINUS ) || c_is( TK_PLUS ) ) {
            c_tok()->flags |= TF_UNARY;
            if ( c_is( TK_MINUS ) ) {
                neg = !neg;
            }
            c_next();
        }
        right = parse_atom();
        need_num( left );
        need_num( right );
        if ( neg ) {
            negate( right );
        }
        type = unify( left, right, TY_SINGLE );
        c_op( TKIND( type ) == TY_DOUBLE ? OP_POWD : OP_POWS );
        left = type;
    }
    return left;
}

/* the type of the expression ahead, with nothing taken and nothing
   left emitted: PAINT's second argument may be a colour or a tile */
QTYPE c_peek_type( void )
{
    int save = cc->tp;
    u32 at = c_here();
    QTYPE type = expr();

    cc->tp = save;
    cc->code_len = at;
    return type;
}

QTYPE expr( void )
{
    TOKEN *tok = c_tok();

    if ( tok->kind == TK_EOL || tok->kind == TK_COLON || tok->kind == TK_COMMA
      || tok->kind == TK_RPAREN || tok->kind == TK_SEMI || tok->kind == TK_REM ) {
        c_expect_msg( "expression" );
    }
    return parse_imp();
}

void expr_num( QTYPE want )
{
    QTYPE type = expr();

    need_num( type );
    c_convert( type, want );
}

void expr_long( void )
{
    expr_num( TY_LONG );
}

/* an INTEGER argument: 32768 is an Overflow, not an Illegal function call */
static void expr_int( void )
{
    expr_num( TY_INT );
}

void expr_dbl( void )
{
    expr_num( TY_DOUBLE );
}

void expr_str( void )
{
    need_str( expr() );
}

/* ------------------------------------------------------------------ */
/* variables                                                           */
/* ------------------------------------------------------------------ */

/* scope's own entry for the name: with a suffix, an AS name of the same
   type answers for it too */
static SYM *find_in( SCOPE *scope, const char *upper, int sfxtype, int kind, int deftype )
{
    SYM *sym, *as;

    as = sym_find( scope, upper, 0, kind );
    if ( sfxtype ) {
        sym = sym_find( scope, upper, sfxtype, kind );
        if ( sym != NULL ) {
            return sym;
        }
        if ( as != NULL ) {
            int have = TKIND( as->type ) == TY_FSTR ? TY_STR : TKIND( as->type );
            if ( have != sfxtype ) {
                c_fail_tok( "Duplicate definition" );
            }
            return as;
        }
        return NULL;
    }
    if ( as != NULL ) {
        return as;
    }
    return sym_find( scope, upper, deftype, kind );
}

/* the procedure's own variable of that name, if it has one */
SYM *c_find_local( const char *upper, int sfx, int kind )
{
    return find_in( cc->procscope, upper, sfx, kind, type_default( cc->scope, upper ) );
}

/* is there any entry for this name of this kind, whatever its type? */
static int any_in( SCOPE *scope, const char *upper, int kind )
{
    int tk;

    for ( tk = 0; tk <= TY_STR; tk++ ) {
        if ( sym_find( scope, upper, tk, kind ) != NULL ) {
            return 1;
        }
    }
    return 0;
}

static void make_storage( SYM *sym, SCOPE *scope, int force_static )
{
    u32 size = sym->kind == SK_ARRAY ? 4 : type_size( sym->type );
    int is_static = force_static;

    if ( scope == &cc->module || scope == cc->immscope ) {
        scope = &cc->module;                /* the Immediate window's names too */
        sym->scope = SC_GLOBAL;
        sym->offset = scope_alloc( scope, size, 0 );
        if ( sym->kind == SK_ARRAY ) {
            scope_note_arr( scope, sym->offset );
        } else if ( TKIND( sym->type ) == TY_STR ) {
            scope_note_str( scope, sym->offset );
        }
        return;
    }
    if ( scope->proc >= 0 && cc->procs[scope->proc].is_static ) {
        is_static = 1;
    }
    if ( is_static ) {
        sym->scope = SC_STATIC;
        sym->offset = scope_alloc( scope, size, 1 );
        if ( sym->kind == SK_ARRAY ) {
            scope_note_static( scope, sym->offset, 1 );
        } else if ( TKIND( sym->type ) == TY_STR ) {
            scope_note_static( scope, sym->offset, 0 );
        }
        return;
    }
    sym->scope = SC_FRAME;
    sym->offset = scope_alloc( scope, size, 0 );
    if ( sym->kind == SK_ARRAY ) {
        scope_note_arr( scope, sym->offset );
    } else if ( TKIND( sym->type ) == TY_STR ) {
        scope_note_str( scope, sym->offset );
    }
}

/* Find a variable or an array by name as the scoping rules have it, and
   make it if "create" and it is not there.  sfxtype is the suffix's
   type, 0 for none; has_as: the name is being declared with AS, in
   which case *type_out is its type on the way in. */
SYM *var_lookup( const char *upper, int sfxtype, int kind, int create, QTYPE *type_out,
                 int sfxtype_unused, int has_as )
{
    SCOPE *home;
    SYM *sym = NULL;
    int deftype = type_default( cc->scope, upper );
    int tkey;

    (void)sfxtype_unused;
    if ( cc->in_deffn >= 0 ) {
        sym = find_in( cc->deffn_scope, upper, sfxtype, kind, deftype );
        if ( sym == NULL ) {
            sym = find_in( &cc->module, upper, sfxtype, kind, deftype );
        }
        home = &cc->module;
    } else if ( cc->procscope != NULL ) {
        sym = find_in( cc->procscope, upper, sfxtype, kind, deftype );
        if ( sym == NULL ) {
            SYM *shared = find_in( &cc->module, upper, sfxtype, kind, deftype );
            if ( shared != NULL && (shared->flags & SF_SHARED) ) {
                sym = shared;
            }
        }
        home = cc->procscope;
    } else {
        sym = find_in( &cc->module, upper, sfxtype, kind, deftype );
        home = &cc->module;
    }
    if ( cc->immediate ) {
        /* names the program does not have: the Immediate window's own */
        if ( sym == NULL ) {
            sym = find_in( cc->immscope, upper, sfxtype, kind, deftype );
        }
        home = cc->immscope;
    }
    if ( has_as ) {
        if ( sym != NULL && sym->scope != SC_GLOBAL && home != &cc->module ) {
            c_fail_tok( "Duplicate definition" );
        }
        if ( sym != NULL || any_in( home, upper, kind ) ) {
            c_fail_tok( "Duplicate definition" );
        }
        if ( sfxtype ) {
            int want = TKIND( *type_out ) == TY_FSTR ? TY_STR : TKIND( *type_out );
            if ( want != sfxtype ) {
                c_fail_tok( "Duplicate definition" );
            }
        }
        sym = sym_add( home, upper, 0, kind );
        sym->type = *type_out;
        sym->flags |= SF_AS;
        make_storage( sym, home, 0 );
        return sym;
    }
    if ( sym != NULL || !create ) {
        if ( sym != NULL ) {
            *type_out = sym->type;
        }
        return sym;
    }
    tkey = sfxtype ? sfxtype : deftype;
    if ( sym_find( home, upper, 0, kind ) != NULL ) {
        c_fail_tok( "Duplicate definition" );
    }
    sym = sym_add( home, upper, tkey, kind );
    sym->type = (QTYPE)tkey;
    make_storage( sym, home, 0 );
    *type_out = sym->type;
    return sym;
}

int c_is_proc_name( TOKEN *tok, int kind )
{
    char upper[NAME_MAX + 1];
    SYM *sym;

    if ( tok->kind != TK_ID ) {
        return 0;
    }
    c_name_of( tok, upper );
    sym = sym_find( &cc->module, upper, 0, SK_PROC );
    if ( sym == NULL ) {
        return 0;
    }
    return kind < 0 || cc->procs[sym->proc].kind == kind;
}

static SYM *proc_sym( TOKEN *tok )
{
    char upper[NAME_MAX + 1];

    if ( tok->kind != TK_ID ) {
        return NULL;
    }
    c_name_of( tok, upper );
    return sym_find( &cc->module, upper, 0, SK_PROC );
}

static SYM *const_sym( TOKEN *tok )
{
    char upper[NAME_MAX + 1];
    SYM *sym = NULL;
    int sfx;

    if ( tok->kind != TK_ID ) {
        return NULL;
    }
    c_name_of( tok, upper );
    sfx = tok->ntype;
    if ( cc->procscope != NULL ) {
        sym = sym_find( cc->procscope, upper, 0, SK_CONST );
    }
    if ( sym == NULL ) {
        sym = sym_find( &cc->module, upper, 0, SK_CONST );
    }
    if ( sym != NULL && sfx && TKIND( sym->type ) != sfx ) {
        return NULL;
    }
    return sym;
}

void c_addr( VREF *ref )
{
    if ( !ref->simple ) {
        return;
    }
    c_op32( ref->scope == SC_GLOBAL ? OP_AG : OP_AF, ref->offset );
    ref->simple = 0;
}

/* the address of what a symbol names (an array's slot, or a scalar) */
static void sym_addr( SYM *sym, u32 extra );

void c_sym_addr( SYM *sym )
{
    sym_addr( sym, 0 );
}

static void sym_addr( SYM *sym, u32 extra )
{
    switch ( sym->scope ) {
    case SC_GLOBAL: c_op32( OP_AG, sym->offset + extra ); break;
    case SC_FRAME:  c_op32( OP_AF, sym->offset + extra ); break;
    case SC_STATIC: c_op32( OP_AS, sym->offset + extra ); break;
    case SC_REF:
        c_op32( OP_AR, sym->offset );
        if ( extra ) {
            c_op32( OP_AOFS, extra );
        }
        break;
    }
}

/* A field chain after a record value: .name.name... */
static QTYPE fields( VREF *ref, QTYPE type, const char *chain )
{
    char part[NAME_MAX + 1];
    QTYPE ftype;
    u32 foff;
    int len;

    while ( *chain ) {
        if ( TKIND( type ) != TY_REC ) {
            c_fail_tok( "Element not defined" );
        }
        len = 0;
        while ( *chain && *chain != '.' && len < NAME_MAX ) {
            part[len++] = *chain++;
        }
        part[len] = 0;
        if ( *chain == '.' ) {
            chain++;
        }
        if ( !rec_field( (int)TARG( type ), part, &ftype, &foff ) ) {
            c_fail_tok( "Element not defined" );
        }
        if ( ref->simple ) {
            ref->offset += foff;
        } else if ( foff ) {
            c_op32( OP_AOFS, foff );
        }
        type = ftype;
    }
    return type;
}

/* subscripts in parentheses after an array's name; the slot's address
   is emitted first */
static void subscripts( SYM *sym )
{
    int count = 0;
    QTYPE type;

    sym_addr( sym, 0 );
    c_need( TK_LPAREN, "(" );
    for ( ;; ) {
        type = expr();
        need_num( type );
        c_convert( type, TY_LONG );
        count++;
        if ( count > MAX_DIMS ) {
            c_fail_tok( "Too many dimensions" );
        }
        if ( !c_accept( TK_COMMA ) ) {
            break;
        }
    }
    c_need( TK_RPAREN, ")" );
    if ( sym->ndims == 0 ) {
        sym->ndims = (u8)count;
    } else if ( sym->ndims != count ) {
        c_fail_tok( "Wrong number of dimensions" );
    }
    cc->last_aidx = c_here();
    if ( count == 1 ) {
        c_op( OP_AIDX1 );
    } else {
        c_op( OP_AIDX );
        c_emit( count );
    }
}

/* An array used without a DIM: 0 (or the OPTION BASE) to 10 in each of
   its dimensions, made before the first statement (the module) or when
   the procedure is entered. */
static void implicit_array( SYM *sym, int ndims )
{
    SCOPE *scope;
    int i;

    sym->ndims = (u8)ndims;
    sym->flags |= SF_IMPLICIT;
    if ( cc->checking ) {
        return;
    }
    if ( sym->scope == SC_GLOBAL || sym->scope == SC_STATIC ) {
        ARRINIT *init;
        if ( cc->narrinit == cc->arrinit_cap ) {
            u32 cap = cc->arrinit_cap ? cc->arrinit_cap * 2 : 32;
            ARRINIT *grown = (ARRINIT *)try_realloc( cc->arrinit, cap * sizeof( ARRINIT ) );
            if ( grown == NULL ) {
                c_fail( "Out of memory" );
            }
            cc->arrinit = grown;
            cc->arrinit_cap = cap;
        }
        init = &cc->arrinit[cc->narrinit++];
        mem_set( init, 0, sizeof( *init ) );
        init->scope = sym->scope;
        init->proc = (u16)(cc->procscope ? cc->procscope->proc : 0);
        init->offset = sym->offset;
        init->type = sym->type;
        init->esize = type_size( sym->type );
        init->ndims = (u8)ndims;
        init->flags = AF_STATIC | AF_IMPLICIT;
        init->bounds = (s32 *)c_pool( ndims * 2 * sizeof( s32 ) );
        for ( i = 0; i < ndims; i++ ) {
            init->bounds[i * 2] = cc->option_base;
            init->bounds[i * 2 + 1] = 10;
        }
        return;
    }
    scope = cc->procscope;
    if ( scope->nimp_arr == scope->cap_imp ) {
        u32 cap = scope->cap_imp ? scope->cap_imp * 2 : 8;
        scope->imp_arr = (u32 *)try_realloc( scope->imp_arr, cap * sizeof( u32 ) );
        scope->imp_type = (QTYPE *)try_realloc( scope->imp_type, cap * sizeof( QTYPE ) );
        scope->imp_dims = (u8 *)try_realloc( scope->imp_dims, cap );
        if ( scope->imp_arr == NULL || scope->imp_type == NULL || scope->imp_dims == NULL ) {
            c_fail( "Out of memory" );
        }
        scope->cap_imp = cap;
    }
    scope->imp_arr[scope->nimp_arr] = sym->offset;
    scope->imp_type[scope->nimp_arr] = sym->type;
    scope->imp_dims[scope->nimp_arr] = (u8)ndims;
    scope->nimp_arr++;
}

/* how many subscripts follow, without reading them */
static int count_subscripts( void )
{
    int depth = 0, count = 1, at = cc->tp;

    if ( cc->toks[at].kind != TK_LPAREN ) {
        return 0;
    }
    for ( ; at < cc->ntoks; at++ ) {
        switch ( cc->toks[at].kind ) {
        case TK_LPAREN:
            depth++;
            break;
        case TK_RPAREN:
            if ( --depth == 0 ) {
                return count;
            }
            break;
        case TK_COMMA:
            if ( depth == 1 ) {
                count++;
            }
            break;
        case TK_EOL:
            return count;
        }
    }
    return count;
}

/* Read a variable reference at the current token.  0 (nothing taken) if
   the name is a FUNCTION, a DEF FN or a constant - or not a name. */
int c_var( VREF *ref, int for_store, int allow_whole )
{
    TOKEN *tok = c_tok();
    char upper[NAME_MAX + 1], base[NAME_MAX + 1];
    const char *chain = "";
    SYM *sym, *psym;
    QTYPE type;
    int kind, sfx, dot, i;

    mem_set( ref, 0, sizeof( *ref ) );
    if ( tok->kind != TK_ID ) {
        return 0;
    }
    c_name_of( tok, upper );
    sfx = tok->ntype;

    /* a FUNCTION's own name inside it is where its value goes */
    psym = proc_sym( tok );
    if ( psym != NULL ) {
        PROC *proc = &cc->procs[psym->proc];
        if ( for_store && (psym->proc == cc->cur_proc || psym->proc == cc->in_deffn)
          && proc->kind != PK_SUB && c_peek( 1 )->kind != TK_LPAREN ) {
            if ( sfx && sfx != TKIND( proc->rettype ) ) {
                c_fail( "Duplicate definition" );
            }
            c_next();
            ref->type = proc->rettype;
            ref->simple = 1;
            ref->scope = SC_FRAME;
            ref->offset = proc->ret_off;
            return 1;
        }
        if ( proc->kind != PK_SUB ) {
            return 0;
        }
    }
    if ( tok->flags & TF_FN ) {
        return 0;                               /* a DEF FN, called */
    }
    if ( const_sym( tok ) != NULL ) {
        if ( for_store ) {
            c_fail( "Duplicate definition" );
        }
        return 0;
    }

    /* a.b: a record and its field, if "a" is a record - else one name */
    str_cpy( base, upper );
    dot = -1;
    for ( i = 0; upper[i]; i++ ) {
        if ( upper[i] == '.' ) {
            dot = i;
            break;
        }
    }
    kind = c_peek( 1 )->kind == TK_LPAREN ? SK_ARRAY : SK_VAR;
    if ( dot > 0 ) {
        base[dot] = 0;
        sym = var_lookup( base, 0, kind, 0, &type, 0, 0 );
        if ( sym != NULL && TKIND( sym->type ) == TY_REC && sfx == 0 ) {
            chain = upper + dot + 1;
        } else {
            str_cpy( base, upper );
        }
    }
    c_next();
    if ( kind == SK_ARRAY ) {
        if ( allow_whole && c_peek( 1 )->kind == TK_RPAREN ) {
            /* name(): the whole array */
            sym = var_lookup( base, sfx, SK_ARRAY, 0, &type, 0, 0 );
            if ( sym == NULL ) {
                c_fail_tok( "Array not defined" );
            }
            c_next();
            c_next();
            sym_addr( sym, 0 );
            ref->sym = sym;
            ref->type = sym->type;
            ref->is_array_whole = 1;
            return 1;
        }
        sym = var_lookup( base, sfx, SK_ARRAY, 0, &type, 0, 0 );
        if ( sym == NULL ) {
            int ndims = count_subscripts();
            sym = var_lookup( base, sfx, SK_ARRAY, 1, &type, 0, 0 );
            if ( sym->scope == SC_GLOBAL || sym->scope == SC_STATIC || sym->scope == SC_FRAME ) {
                implicit_array( sym, ndims );
            }
        }
        subscripts( sym );
        ref->sym = sym;
        ref->simple = 0;
        type = sym->type;
        if ( c_is( TK_DOT ) ) {
            /* a(1).field: the rest of the chain is the next name */
            c_next();
            if ( !c_is( TK_ID ) ) {
                c_expect_msg( "identifier" );
            }
            c_name_of( c_tok(), upper );
            c_next();
            type = fields( ref, type, upper );
        }
        ref->type = type;
        return 1;
    }
    sym = var_lookup( base, chain[0] ? 0 : sfx, SK_VAR, 1, &type, 0, 0 );
    ref->sym = sym;
    type = sym->type;
    if ( sym->scope == SC_GLOBAL || sym->scope == SC_FRAME ) {
        ref->simple = 1;
        ref->scope = sym->scope;
        ref->offset = sym->offset;
    } else {
        sym_addr( sym, 0 );
    }
    if ( chain[0] ) {
        type = fields( ref, type, chain );
    }
    if ( c_is( TK_DOT ) && TKIND( type ) == TY_REC ) {
        c_next();
        if ( !c_is( TK_ID ) ) {
            c_expect_msg( "identifier" );
        }
        c_name_of( c_tok(), upper );
        c_next();
        type = fields( ref, type, upper );
    }
    ref->type = type;
    return 1;
}

void c_load( VREF *ref )
{
    int kind = TKIND( ref->type );

    if ( ref->simple ) {
        int g = ref->scope == SC_GLOBAL;
        switch ( kind ) {
        case TY_INT:    c_op32( g ? OP_GLDI : OP_FLDI, ref->offset ); return;
        case TY_LONG:   c_op32( g ? OP_GLDL : OP_FLDL, ref->offset ); return;
        case TY_SINGLE: c_op32( g ? OP_GLDS : OP_FLDS, ref->offset ); return;
        case TY_DOUBLE: c_op32( g ? OP_GLDD : OP_FLDD, ref->offset ); return;
        case TY_STR:    c_op32( g ? OP_GLDSTR : OP_FLDSTR, ref->offset ); return;
        }
        c_addr( ref );
    }
    switch ( kind ) {
    case TY_INT:    c_op( OP_LDI ); break;
    case TY_LONG:   c_op( OP_LDL ); break;
    case TY_SINGLE: c_op( OP_LDS ); break;
    case TY_DOUBLE: c_op( OP_LDD ); break;
    case TY_STR:    c_op( OP_LDSTR ); break;
    case TY_FSTR:
        c_op( OP_LDFS );
        c_emit16( TARG( ref->type ) );
        break;
    default:
        c_fail_tok( "Type mismatch" );
    }
}

/* the value on top of the stack into the variable; its address, if it
   has one emitted, is under the value */
void c_store( VREF *ref, QTYPE from )
{
    int kind = TKIND( ref->type );

    if ( kind == TY_REC ) {
        c_fail_tok( "Type mismatch" );
    }
    if ( IS_STRT( ref->type ) ) {
        if ( TKIND( from ) != TY_STR ) {
            c_fail_tok( "Type mismatch" );
        }
    } else {
        need_num( from );
        c_convert( from, ref->type );
    }
    if ( ref->simple ) {
        int g = ref->scope == SC_GLOBAL;
        switch ( kind ) {
        case TY_INT:    c_op32( g ? OP_GSTI : OP_FSTI, ref->offset ); return;
        case TY_LONG:   c_op32( g ? OP_GSTL : OP_FSTL, ref->offset ); return;
        case TY_SINGLE: c_op32( g ? OP_GSTS : OP_FSTS, ref->offset ); return;
        case TY_DOUBLE: c_op32( g ? OP_GSTD : OP_FSTD, ref->offset ); return;
        case TY_STR:    c_op32( g ? OP_GSTSTR : OP_FSTSTR, ref->offset ); return;
        }
        c_fail_tok( "Internal error" );     /* a simple FSTR is made non-simple first */
    }
    switch ( kind ) {
    case TY_INT:    c_op( OP_STI ); break;
    case TY_LONG:   c_op( OP_STL ); break;
    case TY_SINGLE: c_op( OP_STS ); break;
    case TY_DOUBLE: c_op( OP_STD ); break;
    case TY_STR:    c_op( OP_STSTR ); break;
    case TY_FSTR:
        c_op( OP_STFS );
        c_emit16( TARG( ref->type ) );
        break;
    }
}

/* name(): an array's slot named for LBOUND, ERASE, GET and the like -
   its address is emitted; need_dims: 1 = "name()" or "name" both do */
SYM *c_array_slot( int need_dims )
{
    TOKEN *tok = c_tok();
    char upper[NAME_MAX + 1];
    QTYPE type;
    SYM *sym;

    (void)need_dims;
    if ( tok->kind != TK_ID ) {
        c_expect_msg( "identifier" );
    }
    c_name_of( tok, upper );
    sym = var_lookup( upper, tok->ntype, SK_ARRAY, 0, &type, 0, 0 );
    if ( sym == NULL ) {
        c_fail( "Array not defined" );
    }
    c_next();
    if ( c_is( TK_LPAREN ) && c_peek( 1 )->kind == TK_RPAREN ) {
        c_next();
        c_next();
    }
    sym_addr( sym, 0 );
    return sym;
}

/* ------------------------------------------------------------------ */
/* calls                                                               */
/* ------------------------------------------------------------------ */

/* Is the argument at the current token nothing but a variable - a name,
   subscripts, fields - ending at a comma or the closing parenthesis?
   Then it goes by reference. */
static int arg_is_variable( int parens )
{
    int at = cc->tp, depth = 0;
    TOKEN *tok = &cc->toks[at];

    if ( tok->kind != TK_ID || (tok->flags & TF_FN) ) {
        return 0;
    }
    if ( const_sym( tok ) != NULL ) {
        return 0;
    }
    {
        SYM *psym = proc_sym( tok );
        if ( psym != NULL && cc->procs[psym->proc].kind != PK_SUB ) {
            return 0;
        }
    }
    at++;
    for ( ;; ) {
        tok = &cc->toks[at];
        if ( tok->kind == TK_LPAREN ) {
            depth = 1;
            at++;
            while ( depth > 0 && cc->toks[at].kind != TK_EOL ) {
                if ( cc->toks[at].kind == TK_LPAREN ) {
                    depth++;
                } else if ( cc->toks[at].kind == TK_RPAREN ) {
                    depth--;
                }
                at++;
            }
            continue;
        }
        if ( tok->kind == TK_DOT && cc->toks[at + 1].kind == TK_ID ) {
            at += 2;
            continue;
        }
        break;
    }
    tok = &cc->toks[at];
    if ( tok->kind == TK_COMMA ) {
        return 1;
    }
    if ( parens ) {
        return tok->kind == TK_RPAREN;
    }
    return tok->kind == TK_EOL || tok->kind == TK_COLON || tok->kind == TK_REM
        || (tok->kind == TK_KW && tok->kw == KW_ELSE);
}

static int types_match( QTYPE param, QTYPE arg )
{
    if ( TKIND( param ) == TY_ANY ) {
        return 1;
    }
    if ( TKIND( param ) == TY_STR ) {
        return TKIND( arg ) == TY_STR;
    }
    return param == arg;
}

/* One argument for a parameter: a variable's address, or a value put in
   a hidden variable whose address goes instead. */
/* An array element as an argument, its address just emitted: QBasic
   passes a copy, and puts the copy back when the call returns - into
   the array the slot holds by then (a REDIM in the procedure), or
   nowhere (an ERASE).  The last AIDX becomes AIDXR, which leaves the
   slot and the element's byte offset (a field's offset is added to it
   as to an address); EREF notes them and copies the element in. */
static void element_copy( QTYPE type )
{
    u32 copy, note, size = type_size( type );
    u8 scope, note_scope;
    int is_str = TKIND( type ) == TY_STR;

    if ( cc->neback >= MAX_EBACK ) {
        c_fail( "Expression too complex" );
    }
    cc->code[cc->last_aidx] = (u8)(cc->code[cc->last_aidx] == OP_AIDX1 ? OP_AIDXR1 : OP_AIDXR);
    copy = c_temp( type, &scope );
    note = c_temp( TY_DOUBLE, &note_scope );        /* 8 bytes: the slot, the offset */
    c_op32( scope == SC_GLOBAL ? OP_AG : OP_AF, copy );
    c_op32( note_scope == SC_GLOBAL ? OP_AG : OP_AF, note );
    c_op( OP_EREF );
    c_emit32( size );
    c_emit( is_str );
    cc->eback[cc->neback].copy = copy;
    cc->eback[cc->neback].note = note;
    cc->eback[cc->neback].scope = scope;
    cc->eback[cc->neback].is_str = (u8)is_str;
    cc->eback[cc->neback].size = size;
    cc->neback++;
}

/* after the call: the copies back, the last argument's first (as QBasic:
   the same element passed twice ends with the first one's value) */
void c_call_back( int base )
{
    while ( cc->neback > base ) {
        int n = --cc->neback;
        c_op32( cc->eback[n].scope == SC_GLOBAL ? OP_AG : OP_AF, cc->eback[n].copy );
        c_op32( cc->eback[n].scope == SC_GLOBAL ? OP_AG : OP_AF, cc->eback[n].note );
        c_op( OP_EBACK );
        c_emit32( cc->eback[n].size );
        c_emit( cc->eback[n].is_str );
    }
}

static void one_arg( PARAM *param, int parens, int by_value_only )
{
    VREF ref;
    QTYPE type;
    u8 scope;
    u32 off;
    int save = cc->tp;

    if ( param->array ) {
        TOKEN *tok = c_tok();
        char upper[NAME_MAX + 1];
        SYM *sym;
        QTYPE atype;
        if ( tok->kind != TK_ID || c_peek( 1 )->kind != TK_LPAREN || c_peek( 2 )->kind != TK_RPAREN ) {
            c_fail( "Parameter type mismatch" );
        }
        c_name_of( tok, upper );
        sym = var_lookup( upper, tok->ntype, SK_ARRAY, 0, &atype, 0, 0 );
        if ( sym == NULL ) {
            c_fail( "Array not defined" );
        }
        if ( !types_match( param->type, sym->type ) ) {
            c_fail( "Parameter type mismatch" );
        }
        c_next();
        c_next();
        c_next();
        sym_addr( sym, 0 );
        return;
    }
    if ( !by_value_only && arg_is_variable( parens ) ) {
        u32 at = c_here();
        cc->last_aidx = NOADDR;
        if ( c_var( &ref, 0, 0 ) ) {
            if ( types_match( param->type, ref.type ) ) {
                if ( !ref.simple && cc->last_aidx != NOADDR && cc->last_aidx >= at ) {
                    element_copy( param->type );
                    return;
                }
                c_addr( &ref );
                return;
            }
            if ( !(TKIND( param->type ) == TY_STR && TKIND( ref.type ) == TY_FSTR) ) {
                c_fail_at( save, "Parameter type mismatch" );
            }
        }
        /* a fixed-length string for a STRING parameter is passed as a
           copy: back out of the code c_var emitted (an element's
           address) and read the argument again as an expression */
        cc->tp = save;
        cc->code_len = at;
    }
    if ( TKIND( param->type ) == TY_REC || TKIND( param->type ) == TY_ANY ) {
        c_fail( "Parameter type mismatch" );
    }
    off = c_temp( param->type, &scope );
    c_op32( scope == SC_GLOBAL ? OP_AG : OP_AF, off );
    type = expr();
    if ( IS_STRT( param->type ) ) {
        need_str( type );
        c_op( OP_STSTR );
    } else {
        need_num( type );
        c_convert( type, param->type );
        switch ( TKIND( param->type ) ) {
        case TY_INT:    c_op( OP_STI ); break;
        case TY_LONG:   c_op( OP_STL ); break;
        case TY_SINGLE: c_op( OP_STS ); break;
        default:        c_op( OP_STD ); break;
        }
    }
    c_op32( scope == SC_GLOBAL ? OP_AG : OP_AF, off );
}

/* the arguments of a call to "proc": in parentheses, or (a CALL-less
   SUB) up to the end of the statement */
void c_call_args( PROC *proc, int parens )
{
    int count = 0;

    if ( parens ) {
        if ( !c_accept( TK_LPAREN ) ) {
            if ( proc->nparams != 0 ) {
                c_fail( "Argument-count mismatch" );
            }
            return;
        }
        if ( c_accept( TK_RPAREN ) ) {
            if ( proc->nparams != 0 ) {
                c_fail_tok( "Argument-count mismatch" );
            }
            return;
        }
    } else if ( c_at_end() ) {
        if ( proc->nparams != 0 ) {
            c_fail( "Argument-count mismatch" );
        }
        return;
    }
    for ( ;; ) {
        if ( count >= proc->nparams ) {
            c_fail( "Argument-count mismatch" );
        }
        one_arg( &proc->params[count], parens, proc->kind == PK_DEFFN );
        count++;
        if ( !c_accept( TK_COMMA ) ) {
            break;
        }
    }
    if ( parens ) {
        c_need( TK_RPAREN, ")" );
    }
    if ( count != proc->nparams ) {
        c_fail_tok( "Argument-count mismatch" );
    }
}

/* a call of a FUNCTION or DEF FN inside an expression */
static QTYPE call_function( SYM *psym )
{
    PROC *proc = &cc->procs[psym->proc];
    TOKEN *tok = c_tok();
    int base;

    if ( tok->ntype && tok->ntype != TKIND( proc->rettype ) ) {
        c_fail( "Duplicate definition" );
    }
    if ( !proc->defined && !cc->checking ) {
        c_fail( proc->kind == PK_DEFFN ? "Function not defined" : "Subprogram not defined" );
    }
    c_next();
    base = cc->neback;
    c_call_args( proc, 1 );
    c_op( OP_CALL );
    c_emit16( (u32)psym->proc );
    c_call_back( base );
    return proc->rettype;
}

/* ------------------------------------------------------------------ */
/* the built-in functions                                              */
/* ------------------------------------------------------------------ */

static void open_paren( void )
{
    c_need( TK_LPAREN, "(" );
}

static void close_paren( void )
{
    c_need( TK_RPAREN, ")" );
}

static void comma( void )
{
    c_need( TK_COMMA, "," );
}

/* f(string) -> string */
static QTYPE str_fn( int routine )
{
    open_paren();
    expr_str();
    close_paren();
    c_rt( routine );
    return TY_STR;
}

/* f(number) by the argument's precision: SINGLE or DOUBLE */
static QTYPE math_fn( int rt_single, int rt_double )
{
    QTYPE type;

    open_paren();
    type = expr();
    need_num( type );
    close_paren();
    if ( TKIND( type ) == TY_DOUBLE ) {
        c_rt( rt_double );
        return TY_DOUBLE;
    }
    c_convert( type, TY_SINGLE );
    c_rt( rt_single );
    return TY_SINGLE;
}

/* a file number, with or without # */
static void file_number( void )
{
    c_accept( TK_HASH );
    expr_long();
}

static QTYPE paren_long_fn( int routine )
{
    open_paren();
    expr_long();
    close_paren();
    c_rt( routine );
    return TY_LONG;
}

/* LEN(x): a string's length, or the bytes a variable of another type
   takes - that is known now */
static QTYPE len_fn( void )
{
    VREF ref;
    int save;
    QTYPE type;

    open_paren();
    save = cc->tp;
    if ( arg_is_variable( 1 ) ) {
        u32 code_at = c_here();
        if ( c_var( &ref, 0, 0 ) && TKIND( ref.type ) != TY_STR ) {
            cc->code_len = code_at;             /* its address is not wanted */
            close_paren();
            c_push_l( (s32)type_size( ref.type ) );
            return TY_INT;
        }
        cc->code_len = code_at;
        cc->tp = save;
    }
    type = expr();
    need_str( type );
    close_paren();
    c_rt( RT_LENSTR );
    return TY_LONG;
}

/* VARPTR, VARSEG, SADD, VARPTR$: a variable's address */
static QTYPE addr_fn( int routine )
{
    VREF ref;

    open_paren();
    if ( !c_var( &ref, 0, 0 ) ) {
        c_fail( "Variable required" );
    } else {
        c_addr( &ref );
        if ( routine == RT_SADD && TKIND( ref.type ) != TY_STR ) {
            c_fail_tok( "Type mismatch" );
        }
        if ( routine == RT_VARPTRS ) {
            c_push_l( (s32)ref.type );
        }
        if ( routine == RT_VARPTR && TKIND( ref.type ) == TY_STR ) {
            routine = RT_VARPTRSTR;
        }
    }
    close_paren();
    c_rt( routine );
    return routine == RT_VARPTRS ? TY_STR : TY_LONG;
}

static QTYPE builtin( int kw )
{
    QTYPE type;

    switch ( kw ) {
    case KW_LEFT_S:
    case KW_RIGHT_S:
        open_paren();
        expr_str();
        comma();
        expr_int();
        close_paren();
        c_rt( kw == KW_LEFT_S ? RT_LEFT : RT_RIGHT );
        return TY_STR;
    case KW_MID_S:
        open_paren();
        expr_str();
        comma();
        expr_int();
        if ( c_accept( TK_COMMA ) ) {
            expr_int();
            close_paren();
            c_rt( RT_MID3 );
        } else {
            close_paren();
            c_rt( RT_MID2 );
        }
        return TY_STR;
    case KW_LEN:
        return len_fn();
    case KW_INSTR:
        open_paren();
        type = expr();
        comma();
        if ( IS_NUMT( type ) ) {
            c_convert( type, TY_INT );
            expr_str();
            comma();
            expr_str();
            close_paren();
            c_rt( RT_INSTR3 );
        } else {
            need_str( type );
            expr_str();
            close_paren();
            c_rt( RT_INSTR2 );
        }
        return TY_INT;
    case KW_LCASE_S:    return str_fn( RT_LCASE );
    case KW_UCASE_S:    return str_fn( RT_UCASE );
    case KW_LTRIM_S:    return str_fn( RT_LTRIM );
    case KW_RTRIM_S:    return str_fn( RT_RTRIM );
    case KW_SPACE_S:
        open_paren();
        expr_int();
        close_paren();
        c_rt( RT_SPACE );
        return TY_STR;
    case KW_STRING_S:
        open_paren();
        expr_int();
        comma();
        type = expr();
        close_paren();
        if ( TKIND( type ) == TY_STR ) {
            c_rt( RT_STRINGS );
        } else {
            need_num( type );
            c_convert( type, TY_INT );
            c_rt( RT_STRINGN );
        }
        return TY_STR;
    case KW_CHR_S:
        open_paren();
        expr_int();
        close_paren();
        c_rt( RT_CHR );
        return TY_STR;
    case KW_ASC:
        open_paren();
        expr_str();
        close_paren();
        c_rt( RT_ASC );
        return TY_INT;
    case KW_STR_S:
        open_paren();
        type = expr();
        need_num( type );
        close_paren();
        c_rt( TKIND( type ) == TY_DOUBLE ? RT_STRD : TKIND( type ) == TY_SINGLE ? RT_STRS : RT_STRL );
        return TY_STR;
    case KW_VAL:
        open_paren();
        expr_str();
        close_paren();
        c_rt( RT_VAL );
        return TY_DOUBLE;
    case KW_HEX_S:
    case KW_OCT_S:
        /* the width of the answer depends on the type: pushed after it */
        open_paren();
        type = expr();
        need_num( type );
        close_paren();
        c_convert( type, TY_LONG );
        c_push_l( TKIND( type ) );
        c_rt( kw == KW_HEX_S ? RT_HEX : RT_OCT );
        return TY_STR;
    case KW_MKI_S:
        open_paren(); expr_num( TY_INT ); close_paren(); c_rt( RT_MKI ); return TY_STR;
    case KW_MKL_S:
        open_paren(); expr_num( TY_LONG ); close_paren(); c_rt( RT_MKL ); return TY_STR;
    case KW_MKS_S:
        open_paren(); expr_num( TY_SINGLE ); close_paren(); c_rt( RT_MKS ); return TY_STR;
    case KW_MKD_S:
        open_paren(); expr_num( TY_DOUBLE ); close_paren(); c_rt( RT_MKD ); return TY_STR;
    case KW_MKSMBF_S:
        open_paren(); expr_num( TY_SINGLE ); close_paren(); c_rt( RT_MKSMBF ); return TY_STR;
    case KW_MKDMBF_S:
        open_paren(); expr_num( TY_DOUBLE ); close_paren(); c_rt( RT_MKDMBF ); return TY_STR;
    case KW_CVI:    str_fn( RT_CVI );    return TY_INT;
    case KW_CVL:    str_fn( RT_CVL );    return TY_LONG;
    case KW_CVS:    str_fn( RT_CVS );    return TY_SINGLE;
    case KW_CVD:    str_fn( RT_CVD );    return TY_DOUBLE;
    case KW_CVSMBF: str_fn( RT_CVSMBF ); return TY_SINGLE;
    case KW_CVDMBF: str_fn( RT_CVDMBF ); return TY_DOUBLE;
    case KW_INKEY_S:
        c_rt( RT_INKEY );
        return TY_STR;
    case KW_INPUT_S:
        open_paren();
        expr_long();
        if ( c_accept( TK_COMMA ) ) {
            file_number();
            close_paren();
            c_rt( RT_INPUTSF );
        } else {
            close_paren();
            c_rt( RT_INPUTS );
        }
        return TY_STR;
    case KW_DATE_S:
        c_rt( RT_DATE );
        return TY_STR;
    case KW_TIME_S:
        c_rt( RT_TIME );
        return TY_STR;
    case KW_ENVIRON_S:
        open_paren();
        type = expr();
        close_paren();
        if ( TKIND( type ) == TY_STR ) {
            c_rt( RT_ENVIRONS );
        } else {
            need_num( type );
            c_convert( type, TY_LONG );
            c_rt( RT_ENVIRONN );
        }
        return TY_STR;
    case KW_ERDEV_S:
        c_rt( RT_ERDEVS );
        return TY_STR;
    case KW_IOCTL_S:
        open_paren();
        file_number();
        close_paren();
        c_rt( RT_IOCTLS );
        return TY_STR;
    case KW_VARPTR_S:   return addr_fn( RT_VARPTRS );
    case KW_VARPTR:     addr_fn( RT_VARPTR ); return TY_INT;
    case KW_VARSEG:     addr_fn( RT_VARSEG ); return TY_INT;
    case KW_SADD:       addr_fn( RT_SADD ); return TY_INT;

    case KW_ABS:
        open_paren();
        type = expr();
        need_num( type );
        close_paren();
        switch ( TKIND( type ) ) {
        case TY_INT:    c_rt( RT_ABSL ); c_op( OP_L2I ); break;
        case TY_LONG:   c_rt( RT_ABSL ); break;
        case TY_SINGLE: c_rt( RT_ABSS ); break;
        default:        c_rt( RT_ABSD ); break;
        }
        return type;
    case KW_SGN:
        open_paren();
        type = expr();
        need_num( type );
        close_paren();
        c_rt( TKIND( type ) == TY_DOUBLE ? RT_SGND : TKIND( type ) == TY_SINGLE ? RT_SGNS : RT_SGNL );
        return TY_INT;
    case KW_INT:
    case KW_FIX:
        open_paren();
        type = expr();
        need_num( type );
        close_paren();
        if ( TKIND( type ) == TY_SINGLE ) {
            c_rt( kw == KW_INT ? RT_INTS : RT_FIXS );
        } else if ( TKIND( type ) == TY_DOUBLE ) {
            c_rt( kw == KW_INT ? RT_INTD : RT_FIXD );
        }
        return type;
    case KW_CINT:
        open_paren(); expr_num( TY_INT ); close_paren(); return TY_INT;
    case KW_CLNG:
        open_paren(); expr_num( TY_LONG ); close_paren(); return TY_LONG;
    case KW_CSNG:
        open_paren(); expr_num( TY_SINGLE ); close_paren(); return TY_SINGLE;
    case KW_CDBL:
        open_paren(); expr_num( TY_DOUBLE ); close_paren(); return TY_DOUBLE;
    case KW_SQR:    return math_fn( RT_SQRS, RT_SQRD );
    case KW_SIN:    return math_fn( RT_SINS, RT_SIND );
    case KW_COS:    return math_fn( RT_COSS, RT_COSD );
    case KW_TAN:    return math_fn( RT_TANS, RT_TAND );
    case KW_ATN:    return math_fn( RT_ATNS, RT_ATND );
    case KW_EXP:    return math_fn( RT_EXPS, RT_EXPD );
    case KW_LOG:    return math_fn( RT_LOGS, RT_LOGD );
    case KW_RND:
        if ( c_accept( TK_LPAREN ) ) {
            expr_num( TY_SINGLE );
            close_paren();
            c_rt( RT_RND1 );
        } else {
            c_rt( RT_RND0 );
        }
        return TY_SINGLE;
    case KW_TIMER:
        c_rt( RT_TIMER );
        return TY_SINGLE;
    case KW_CSRLIN:
        c_rt( RT_CSRLIN );
        return TY_INT;
    case KW_POS:
        paren_long_fn( RT_POS );
        return TY_INT;
    case KW_LPOS:
        paren_long_fn( RT_LPOS );
        return TY_INT;
    case KW_EOF:
        open_paren(); file_number(); close_paren(); c_rt( RT_EOF ); return TY_INT;
    case KW_LOF:
        open_paren(); file_number(); close_paren(); c_rt( RT_LOF ); return TY_LONG;
    case KW_LOC:
        open_paren(); file_number(); close_paren(); c_rt( RT_LOC ); return TY_LONG;
    case KW_SEEK:
        open_paren(); file_number(); close_paren(); c_rt( RT_SEEKF ); return TY_LONG;
    case KW_FREEFILE:
        c_rt( RT_FREEFILE );
        return TY_INT;
    case KW_FILEATTR:
        open_paren();
        file_number();
        comma();
        expr_long();
        close_paren();
        c_rt( RT_FILEATTR );
        return TY_LONG;
    case KW_ERR:
        c_rt( RT_ERR );
        return TY_INT;
    case KW_ERL:
        c_rt( RT_ERL );
        return TY_LONG;
    case KW_ERDEV:
        c_rt( RT_ERDEV );
        return TY_INT;
    case KW_FRE:
        open_paren();
        type = expr();
        close_paren();
        if ( TKIND( type ) == TY_STR ) {
            c_rt( RT_FRESTR );
        } else {
            need_num( type );
            c_convert( type, TY_LONG );
            c_rt( RT_FRE );
        }
        return TY_LONG;
    case KW_PEEK:
        paren_long_fn( RT_PEEK );
        return TY_INT;
    case KW_INP:
        paren_long_fn( RT_INP );
        return TY_INT;
    case KW_LBOUND:
    case KW_UBOUND:
        open_paren();
        c_array_slot( 0 );
        if ( c_accept( TK_COMMA ) ) {
            expr_long();
        } else {
            c_push_l( 1 );
        }
        close_paren();
        c_rt( kw == KW_LBOUND ? RT_LBOUND : RT_UBOUND );
        return TY_INT;
    case KW_SCREEN:
        open_paren();
        expr_long();
        comma();
        expr_long();
        if ( c_accept( TK_COMMA ) ) {
            expr_long();
            c_push_l( 1 );
        } else {
            c_push_l( 0 );
            c_push_l( 0 );
        }
        close_paren();
        c_rt( RT_SCREENF );
        return TY_INT;
    case KW_POINT:
        open_paren();
        type = expr();
        need_num( type );
        if ( c_accept( TK_COMMA ) ) {
            c_convert( type, TY_DOUBLE );
            expr_dbl();
            close_paren();
            c_rt( RT_POINT2 );
            return TY_INT;
        }
        c_convert( type, TY_LONG );
        close_paren();
        c_rt( RT_POINT1 );
        return TY_SINGLE;
    case KW_PMAP:
        open_paren();
        expr_dbl();
        comma();
        expr_long();
        close_paren();
        c_rt( RT_PMAP );
        return TY_SINGLE;
    case KW_STICK:
        paren_long_fn( RT_STICK );
        return TY_INT;
    case KW_STRIG:
        paren_long_fn( RT_STRIG );
        return TY_INT;
    case KW_PEN:
        paren_long_fn( RT_PEN );
        return TY_INT;
    case KW_PLAY:
        paren_long_fn( RT_PLAYF );
        return TY_INT;
    case KW_TAB:
    case KW_SPC:
        c_fail_tok( "Expected: expression" );
    }

    c_expect_msg( "expression" );
    return TY_NONE;
}

static QTYPE push_const( SYM *sym )
{
    switch ( TKIND( sym->type ) ) {
    case TY_INT:
    case TY_LONG:
        c_push_l( sym->cval.l );
        break;
    case TY_SINGLE:
        c_op( OP_CS );
        c_emitf( sym->cval.s );
        break;
    case TY_DOUBLE:
        c_op( OP_CD );
        c_emitd( sym->cval.d );
        break;
    default:
        c_op32( OP_CSTR, sym->cstr );
        return TY_STR;
    }
    return sym->type;
}

static QTYPE parse_atom( void )
{
    TOKEN *tok = c_tok();
    QTYPE type;
    VREF ref;
    SYM *sym;

    switch ( tok->kind ) {
    case TK_NUM:
        c_next();
        switch ( tok->ntype ) {
        case TY_INT:
        case TY_LONG:
            c_push_l( (s32)tok->num );
            return tok->ntype;
        case TY_SINGLE:
            c_op( OP_CS );
            c_emitf( (float)tok->num );
            return TY_SINGLE;
        default:
            c_op( OP_CD );
            c_emitd( tok->num );
            return TY_DOUBLE;
        }
    case TK_STR: {
        u32 len = tok->len - 1;
        if ( !(tok->flags & TF_UNTERM) ) {
            len--;
        }
        c_next();
        c_op32( OP_CSTR, c_const_str( cc->src + tok->col + 1, len ) );
        return TY_STR;
    }
    case TK_LPAREN:
        c_next();
        type = expr();
        close_paren();
        return type;
    case TK_KW:
        c_next();
        return builtin( tok->kw );
    case TK_ID:
        sym = const_sym( tok );
        if ( sym != NULL ) {
            c_next();
            return push_const( sym );
        }
        sym = proc_sym( tok );
        if ( sym != NULL && cc->procs[sym->proc].kind != PK_SUB ) {
            /* a FUNCTION's name with no ( inside itself, being assigned,
               is not this; c_var took care of that case */
            return call_function( sym );
        }
        if ( sym != NULL ) {
            c_fail( "Type mismatch" );          /* a SUB used as a value */
        }
        if ( tok->flags & TF_FN ) {
            c_fail( "Function not defined" );
        }
        if ( !c_var( &ref, 0, 0 ) ) {
            c_expect_msg( "expression" );
        }
        if ( TKIND( ref.type ) == TY_REC ) {
            c_fail_tok( "Type mismatch" );
        }
        c_load( &ref );
        return value_type( ref.type );
    }
    c_expect_msg( "expression" );
    return TY_NONE;
}

/* ------------------------------------------------------------------ */
/* labels                                                              */
/* ------------------------------------------------------------------ */

/* The line number or label at the current token, in the procedure's
   scope (or the module's), made if it is not there yet. */
SYM *c_label_ref( int module_level )
{
    TOKEN *tok = c_tok();
    char upper[NAME_MAX + 1];
    SCOPE *scope;
    SYM *sym;

    if ( tok->kind == TK_NUM ) {
        if ( tok->num < 0 || tok->num > 65529.0 || tok->num != fp_trunc( tok->num ) ) {
            c_fail( "Invalid line number" );
        }
        fmt_dec( (u32)tok->num, 0, upper );
    } else if ( tok->kind == TK_ID && tok->kw == 0 ) {
        c_name_of( tok, upper );
    } else {
        c_expect_msg( "label or line number" );
        return NULL;
    }
    scope = (module_level || cc->procscope == NULL) ? &cc->module : cc->procscope;
    sym = sym_find( scope, upper, 0, SK_LABEL );
    if ( cc->immediate && (sym == NULL || sym->addr == NOADDR) ) {
        c_fail( "Label not defined" );      /* nothing is left to define it */
    }
    if ( sym == NULL ) {
        sym = sym_add( scope, upper, 0, SK_LABEL );
    }
    c_next();
    return sym;
}

/* the label's address in the code, now or when it is defined */
void c_emit_label( SYM *label )
{
    FIXUP *fix;
    TOKEN *tok = &cc->toks[cc->tp > 0 ? cc->tp - 1 : 0];

    if ( label->addr != NOADDR ) {
        c_emit32( label->addr );
        return;
    }
    fix = (FIXUP *)c_pool( sizeof( FIXUP ) );
    fix->at = c_here();
    fix->text = cc->text;
    fix->line = cc->line;
    fix->col = tok->col;
    fix->len = tok->len;
    fix->next = label->fixups;
    label->fixups = fix;
    c_emit32( 0 );
}

/* ------------------------------------------------------------------ */
/* constant expressions: CONST, STRING * n, array bounds                */
/* ------------------------------------------------------------------ */

typedef struct {
    QTYPE type;                 /* TY_INT..TY_DOUBLE, TY_STR */
    double num;
    STR   *str;
} KVAL;

static int k_ok;

static void k_fail( void )
{
    k_ok = 0;
}

static void k_imp( KVAL *val );

static void k_atom( KVAL *val )
{
    TOKEN *tok = c_tok();
    SYM *sym;

    val->str = NULL;
    switch ( tok->kind ) {
    case TK_NUM:
        val->type = tok->ntype;
        val->num = tok->num;
        c_next();
        return;
    case TK_STR: {
        u32 len = tok->len - 1;
        int ok;
        if ( !(tok->flags & TF_UNTERM) ) {
            len--;
        }
        val->type = TY_STR;
        val->str = qs_try_make( cc->src + tok->col + 1, len, &ok );
        if ( !ok ) {
            c_fail( "Out of memory" );
        }
        c_next();
        return;
    }
    case TK_LPAREN:
        c_next();
        k_imp( val );
        if ( !c_accept( TK_RPAREN ) ) {
            k_fail();
        }
        return;
    case TK_ID:
        sym = const_sym( tok );
        if ( sym == NULL ) {
            break;
        }
        c_next();
        val->type = sym->type;
        switch ( TKIND( sym->type ) ) {
        case TY_INT:
        case TY_LONG:   val->num = sym->cval.l; break;
        case TY_SINGLE: val->num = sym->cval.s; break;
        case TY_DOUBLE: val->num = sym->cval.d; break;
        default:
            val->type = TY_STR;
            val->str = qs_retain( cc->consts[sym->cstr] );
            break;
        }
        return;
    }
    k_fail();
    val->type = TY_INT;
    val->num = 0;
    c_next();
}

/* a constant's value forced into its type's range, or a failure */
static void k_fit( KVAL *val )
{
    if ( !k_ok ) {
        return;                                 /* a part is not a constant: no value */
    }
    switch ( TKIND( val->type ) ) {
    case TY_INT:
        if ( val->num < -32768.0 || val->num > 32767.0 ) {
            c_fail_tok( "Overflow" );
        }
        break;
    case TY_LONG:
        if ( val->num < -2147483648.0 || val->num > 2147483647.0 ) {
            c_fail_tok( "Overflow" );
        }
        break;
    case TY_SINGLE:
        if ( val->num > SNG_MAX || val->num < -SNG_MAX ) {
            c_fail_tok( "Overflow" );
        }
        val->num = num_sng( val->num );
        break;
    case TY_DOUBLE:
        if ( DBL_BAD( val->num ) ) {
            c_fail_tok( "Overflow" );
        }
        break;
    }
}

static int k_rank_max( QTYPE a, QTYPE b, int at_least )
{
    int want = TKIND( a ) > TKIND( b ) ? TKIND( a ) : TKIND( b );

    return want < at_least ? at_least : want;
}

static void k_need_num( KVAL *val )
{
    if ( val->type == TY_STR ) {
        c_fail_tok( "Type mismatch" );
    }
}

static void k_pow( KVAL *val )
{
    KVAL rhs;

    k_atom( val );
    while ( c_accept( TK_CARET ) ) {
        int neg = 0;
        while ( c_is( TK_MINUS ) || c_is( TK_PLUS ) ) {
            c_tok()->flags |= TF_UNARY;
            if ( c_is( TK_MINUS ) ) {
                neg = !neg;
            }
            c_next();
        }
        k_atom( &rhs );
        k_need_num( val );
        k_need_num( &rhs );
        if ( neg ) {
            rhs.num = -rhs.num;
        }
        val->type = k_rank_max( val->type, rhs.type, TY_SINGLE );
        if ( !k_ok ) {
            continue;
        }
        if ( val->num == 0 && rhs.num < 0 ) {
            c_fail_tok( "Division by zero" );
        }
        if ( val->num < 0 && rhs.num != fp_trunc( rhs.num ) ) {
            c_fail_tok( "Illegal function call" );
        }
        if ( val->num == 0 ) {
            val->num = rhs.num == 0 ? 1 : 0;
        } else if ( val->num > 0 ) {
            val->num = fp_pow( val->num, rhs.num );
        } else {
            double mag = fp_pow( -val->num, rhs.num );
            double half = rhs.num / 2;
            val->num = half == fp_trunc( half ) ? mag : -mag;
        }
        k_fit( val );
    }
}

static void k_unary( KVAL *val )
{
    if ( c_is( TK_MINUS ) ) {
        c_tok()->flags |= TF_UNARY;
        c_next();
        k_unary( val );
        k_need_num( val );
        val->num = val->num == 0 ? 0 : -val->num;
        k_fit( val );
        return;
    }
    if ( c_is( TK_PLUS ) ) {
        c_tok()->flags |= TF_UNARY;
        c_next();
        k_unary( val );
        k_need_num( val );
        return;
    }
    k_pow( val );
}

static s32 k_whole( KVAL *val )
{
    double rounded = fp_round( val->num );

    if ( !k_ok ) {
        return 0;
    }
    if ( rounded < -2147483648.0 || rounded > 2147483647.0 ) {
        c_fail_tok( "Overflow" );
    }
    return (s32)rounded;
}

static void k_mul( KVAL *val )
{
    KVAL rhs;
    int kind;

    k_unary( val );
    for ( ;; ) {
        kind = c_tok()->kind;
        if ( kind == TK_STAR || kind == TK_SLASH ) {
            c_next();
            k_unary( &rhs );
            k_need_num( val );
            k_need_num( &rhs );
            if ( kind == TK_STAR ) {
                val->type = k_rank_max( val->type, rhs.type, TY_INT );
                val->num *= rhs.num;
            } else {
                val->type = k_rank_max( val->type, rhs.type, TY_SINGLE );
                if ( !k_ok ) {
                    continue;
                }
                if ( rhs.num == 0 ) {
                    c_fail_tok( "Division by zero" );
                }
                val->num /= rhs.num;
            }
            k_fit( val );
        } else if ( kind == TK_BSLASH || (kind == TK_KW && c_tok()->kw == KW_MOD) ) {
            s32 lhs_w, rhs_w;
            c_next();
            k_unary( &rhs );
            k_need_num( val );
            k_need_num( &rhs );
            lhs_w = k_whole( val );
            rhs_w = k_whole( &rhs );
            val->type = (TKIND( val->type ) == TY_INT && TKIND( rhs.type ) == TY_INT) ? TY_INT : TY_LONG;
            if ( !k_ok ) {
                continue;
            }
            if ( rhs_w == 0 ) {
                c_fail_tok( "Division by zero" );
            }
            if ( lhs_w == (s32)0x80000000UL && rhs_w == -1 ) {
                /* the one quotient a LONG cannot hold (the CPU faults on it) */
                if ( kind == TK_BSLASH ) {
                    c_fail_tok( "Overflow" );
                }
                val->num = 0;
                continue;
            }
            val->num = kind == TK_BSLASH ? (double)(lhs_w / rhs_w) : (double)(lhs_w % rhs_w);
            k_fit( val );
        } else {
            return;
        }
    }
}

static void k_add( KVAL *val )
{
    KVAL rhs;
    int kind;

    k_mul( val );
    for ( ;; ) {
        kind = c_tok()->kind;
        if ( kind != TK_PLUS && kind != TK_MINUS ) {
            return;
        }
        c_next();
        k_mul( &rhs );
        if ( kind == TK_PLUS && val->type == TY_STR && rhs.type == TY_STR ) {
            /* not qs_cat: its errors are a run's */
            u32 llen = QS_LEN( val->str ), rlen = QS_LEN( rhs.str );
            STR *both;
            int ok;
            if ( llen + rlen > STR_MAX ) {
                qs_release( val->str );
                qs_release( rhs.str );
                val->str = NULL;
                c_fail_tok( "Out of string space" );
            }
            both = qs_try_make( NULL, llen + rlen, &ok );
            if ( !ok ) {
                qs_release( val->str );
                qs_release( rhs.str );
                val->str = NULL;
                c_fail( "Out of memory" );
            }
            if ( both != NULL ) {
                mem_cpy( both->text, QS_TEXT( val->str ), llen );
                mem_cpy( both->text + llen, QS_TEXT( rhs.str ), rlen );
            }
            qs_release( val->str );
            qs_release( rhs.str );
            val->str = both;
            continue;
        }
        k_need_num( val );
        k_need_num( &rhs );
        val->type = k_rank_max( val->type, rhs.type, TY_INT );
        val->num = kind == TK_PLUS ? val->num + rhs.num : val->num - rhs.num;
        k_fit( val );
    }
}

static void k_rel( KVAL *val )
{
    KVAL rhs;
    int kind, result;

    k_add( val );
    for ( ;; ) {
        kind = c_tok()->kind;
        if ( kind < TK_EQ || kind > TK_NE ) {
            return;
        }
        c_next();
        k_add( &rhs );
        if ( (val->type == TY_STR) != (rhs.type == TY_STR) ) {
            c_fail_tok( "Type mismatch" );
        }
        if ( val->type == TY_STR ) {
            int cmp = qs_cmp( val->str, rhs.str );
            qs_release( val->str );
            qs_release( rhs.str );
            val->str = NULL;
            result = cmp;
        } else {
            result = val->num < rhs.num ? -1 : val->num > rhs.num ? 1 : 0;
        }
        switch ( kind ) {
        case TK_EQ: result = result == 0; break;
        case TK_NE: result = result != 0; break;
        case TK_LT: result = result < 0; break;
        case TK_GT: result = result > 0; break;
        case TK_LE: result = result <= 0; break;
        default:    result = result >= 0; break;
        }
        val->type = TY_INT;
        val->num = result ? -1 : 0;
    }
}

static void k_not( KVAL *val )
{
    if ( c_accept_kw( KW_NOT ) ) {
        k_not( val );
        k_need_num( val );
        val->num = (double)~k_whole( val );
        if ( TKIND( val->type ) != TY_INT ) {
            val->type = TY_LONG;
        }
        return;
    }
    k_rel( val );
}

static void k_logic( KVAL *val, int level )
{
    static const int ops[] = { KW_IMP, KW_EQV, KW_XOR, KW_OR, KW_AND };
    KVAL rhs;
    s32 lhs_w, rhs_w;

    if ( level == 5 ) {
        k_not( val );
        return;
    }
    k_logic( val, level + 1 );
    while ( c_accept_kw( ops[level] ) ) {
        k_logic( &rhs, level + 1 );
        k_need_num( val );
        k_need_num( &rhs );
        lhs_w = k_whole( val );
        rhs_w = k_whole( &rhs );
        switch ( ops[level] ) {
        case KW_AND:    lhs_w = lhs_w & rhs_w; break;
        case KW_OR:     lhs_w = lhs_w | rhs_w; break;
        case KW_XOR:    lhs_w = lhs_w ^ rhs_w; break;
        case KW_EQV:    lhs_w = ~(lhs_w ^ rhs_w); break;
        default:        lhs_w = ~lhs_w | rhs_w; break;
        }
        val->type = (TKIND( val->type ) == TY_INT && TKIND( rhs.type ) == TY_INT) ? TY_INT : TY_LONG;
        val->num = lhs_w;
    }
}

static void k_imp( KVAL *val )
{
    k_logic( val, 0 );
}

/* -> 1 with the value and its type; 0 if it is not a constant */
int const_expr( CELL *cval, QTYPE *type, STR **sval )
{
    KVAL val;

    k_ok = 1;
    mem_set( &val, 0, sizeof( val ) );
    k_imp( &val );
    *sval = NULL;
    if ( !k_ok ) {
        qs_release( val.str );
        return 0;
    }
    *type = val.type;
    switch ( TKIND( val.type ) ) {
    case TY_INT:
    case TY_LONG:   cval->l = (s32)val.num; break;
    case TY_SINGLE: cval->s = (float)val.num; break;
    case TY_DOUBLE: cval->d = val.num; break;
    default:        *sval = val.str; break;
    }
    return 1;
}
