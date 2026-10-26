/*
 * CSYM.C - the compiler's bookkeeping: errors, the code buffer, the
 * token stream, names and scopes, types.
 */
#include "comp.h"

COMP *cc;

/* ------------------------------------------------------------------ */
/* errors: the first one ends the compile                              */
/* ------------------------------------------------------------------ */

void c_fail_at( int tp, const char *msg )
{
    CERR *err = cc->err;
    TOKEN *tok;

    if ( tp < 0 ) {
        tp = 0;
    }
    if ( tp >= cc->ntoks ) {
        tp = cc->ntoks - 1;
    }
    err->text = cc->text;
    err->line = cc->line;
    if ( tp >= 0 && cc->ntoks > 0 ) {
        tok = &cc->toks[tp];
        err->col = tok->col;
        err->len = tok->len ? tok->len : 1;
    } else {
        err->col = 0;
        err->len = 1;
    }
    str_cpyn( err->msg, msg, sizeof( err->msg ) );
    rt_longjmp( &cc->fail, 1 );
}

void c_fail( const char *msg )
{
    c_fail_at( cc->tp, msg );
}

void c_fail_tok( const char *msg )
{
    c_fail_at( cc->tp > 0 ? cc->tp - 1 : 0, msg );
}

void c_expect_msg( const char *what )
{
    char msg[80];

    str_cpy( msg, "Expected: " );
    str_catn( msg, what, sizeof( msg ) );
    c_fail( msg );
}

/* ------------------------------------------------------------------ */
/* the code                                                            */
/* ------------------------------------------------------------------ */

static void code_room( u32 more )
{
    u8 *grown;
    u32 cap;

    if ( cc->code_len + more <= cc->code_cap ) {
        return;
    }
    if ( cc->immediate ) {
        /* a running program's code may not move: its frames point into it */
        c_fail( "Out of memory" );
    }
    cap = cc->code_cap ? cc->code_cap * 2 : 16384;
    while ( cap < cc->code_len + more ) {
        cap *= 2;
    }
    grown = (u8 *)try_realloc( cc->code, cap );
    if ( grown == NULL ) {
        c_fail( "Out of memory" );
    }
    cc->code = grown;
    cc->code_cap = cap;
}

void c_emit( int byte )
{
    code_room( 1 );
    cc->code[cc->code_len++] = (u8)byte;
}

/* the compile's own memory, freed with it */
void *c_pool( u32 size )
{
    void *mem = pool_alloc( cc->pool, size );

    if ( mem == NULL ) {
        c_fail( "Out of memory" );
    }
    return mem;
}

char *c_pool_strn( const char *text, u32 len )
{
    char *copy = pool_strn( cc->pool, text, len );

    if ( copy == NULL ) {
        c_fail( "Out of memory" );
    }
    return copy;
}

void c_emit16( u32 val )
{
    code_room( 2 );
    *(u16 *)(cc->code + cc->code_len) = (u16)val;
    cc->code_len += 2;
}

void c_emit32( u32 val )
{
    code_room( 4 );
    *(u32 *)(cc->code + cc->code_len) = val;
    cc->code_len += 4;
}

void c_emitf( float val )
{
    code_room( 4 );
    *(float *)(cc->code + cc->code_len) = val;
    cc->code_len += 4;
}

void c_emitd( double val )
{
    code_room( 8 );
    *(double *)(cc->code + cc->code_len) = val;
    cc->code_len += 8;
}

void c_patch32( u32 at, u32 val )
{
    *(u32 *)(cc->code + at) = val;
}

u32 c_here( void )
{
    return cc->code_len;
}

void c_op( int op )
{
    c_emit( op );
}

void c_op32( int op, u32 val )
{
    c_emit( op );
    c_emit32( val );
}

void c_rt( int routine )
{
    c_emit( OP_RT );
    c_emit16( (u32)routine );
}

void c_push_l( s32 val )
{
    c_op32( OP_CI, (u32)val );
}

u32 c_jump( int op )
{
    u32 at;

    c_emit( op );
    at = cc->code_len;
    c_emit32( 0 );
    return at;
}

u32 c_const_str( const char *text, u32 len )
{
    STR **grown;
    u32 cap;

    if ( cc->nconsts == cc->consts_cap ) {
        cap = cc->consts_cap ? cc->consts_cap * 2 : 256;
        grown = (STR **)try_realloc( cc->consts, cap * sizeof( STR * ) );
        if ( grown == NULL ) {
            c_fail( "Out of memory" );
        }
        cc->consts = grown;
        cc->consts_cap = cap;
    }
    cc->consts[cc->nconsts] = qs_perm( text, len );
    if ( len > 0 && cc->consts[cc->nconsts] == NULL ) {
        c_fail( "Out of memory" );
    }
    return cc->nconsts++;
}

/* ------------------------------------------------------------------ */
/* tokens                                                              */
/* ------------------------------------------------------------------ */

TOKEN *c_tok( void )
{
    return &cc->toks[cc->tp];
}

TOKEN *c_peek( int ahead )
{
    int at = cc->tp + ahead;

    if ( at >= cc->ntoks ) {
        at = cc->ntoks - 1;
    }
    return &cc->toks[at];
}

void c_next( void )
{
    if ( cc->toks[cc->tp].kind != TK_EOL ) {
        cc->tp++;
    }
}

int c_is( int kind )
{
    return cc->toks[cc->tp].kind == kind;
}

int c_is_kw( int kw )
{
    return cc->toks[cc->tp].kind == TK_KW && cc->toks[cc->tp].kw == kw;
}

int c_accept( int kind )
{
    if ( c_is( kind ) ) {
        c_next();
        return 1;
    }
    return 0;
}

int c_accept_kw( int kw )
{
    if ( c_is_kw( kw ) ) {
        c_next();
        return 1;
    }
    return 0;
}

static const char *tk_spelling( int kind )
{
    switch ( kind ) {
    case TK_LPAREN: return "(";
    case TK_RPAREN: return ")";
    case TK_COMMA:  return ",";
    case TK_SEMI:   return ";";
    case TK_EQ:     return "=";
    case TK_MINUS:  return "-";
    case TK_HASH:   return "#";
    case TK_STAR:   return "*";
    }
    return "?";
}

void c_need( int kind, const char *what )
{
    if ( !c_accept( kind ) ) {
        c_expect_msg( what ? what : tk_spelling( kind ) );
    }
}

void c_need_kw( int kw )
{
    if ( !c_accept_kw( kw ) ) {
        c_expect_msg( kw_name( kw ) );
    }
}

/* the end of a statement: the line's end, a colon, a remark, or the
   ELSE of a one-line IF */
int c_at_end( void )
{
    TOKEN *tok = c_tok();

    return tok->kind == TK_EOL || tok->kind == TK_COLON || tok->kind == TK_REM
        || (tok->kind == TK_KW && tok->kw == KW_ELSE);
}

void c_name_of( TOKEN *tok, char *upper )
{
    int len = tok->len, i;

    if ( tok->kind == TK_ID && tok->kw ) {
        len--;                                  /* the suffix */
    }
    if ( len > NAME_MAX ) {
        len = NAME_MAX;
    }
    for ( i = 0; i < len; i++ ) {
        upper[i] = (char)ch_upper( cc->src[tok->col + i] );
    }
    upper[len] = 0;
}

/* ------------------------------------------------------------------ */
/* scopes and names                                                    */
/* ------------------------------------------------------------------ */

static u32 name_hash( const char *upper )
{
    u32 hash = 0;

    while ( *upper ) {
        hash = hash * 31 + (u8)*upper++;
    }
    return hash % NBUCKET;
}

void scope_init( SCOPE *scope, int proc )
{
    int i;

    mem_set( scope, 0, sizeof( *scope ) );
    scope->proc = proc;
    for ( i = 0; i < 26; i++ ) {
        scope->deftype[i] = TY_SINGLE;
    }
}

SCOPE *scope_new( int proc )
{
    SCOPE *scope = (SCOPE *)c_pool( sizeof( SCOPE ) );

    scope_init( scope, proc );
    scope->chain = cc->scopes;
    cc->scopes = scope;
    return scope;
}

/* every kind of name is looked up as that kind only: a CONST, a label
   and a procedure may share a name's letters without being found for
   one another */
static int kind_matches( int have, int want )
{
    return have == want;
}

SYM *sym_find( SCOPE *scope, const char *upper, int tkey, int kind )
{
    SYM *sym;

    for ( sym = scope->bucket[name_hash( upper )]; sym != NULL; sym = sym->next ) {
        if ( sym->tkey == tkey && kind_matches( sym->kind, kind ) && str_cmp( sym->name, upper ) == 0 ) {
            return sym;
        }
    }
    return NULL;
}

SYM *sym_add( SCOPE *scope, const char *upper, int tkey, int kind )
{
    SYM *sym = (SYM *)c_pool( sizeof( SYM ) );
    u32 hash = name_hash( upper );

    str_cpyn( sym->name, upper, sizeof( sym->name ) );
    sym->tkey = (u8)tkey;
    sym->kind = (u8)kind;
    sym->addr = NOADDR;
    sym->next = scope->bucket[hash];
    scope->bucket[hash] = sym;
    return sym;
}

u32 scope_alloc( SCOPE *scope, u32 size, int is_static )
{
    u32 *where = is_static ? &scope->ssize : &scope->size;
    u32 off;

    if ( size >= 4 ) {
        *where = (*where + 3u) & ~3u;
    } else if ( size == 2 ) {
        *where = (*where + 1u) & ~1u;
    }
    off = *where;
    *where += size;
    if ( *where > 0x7FFFFFFFUL ) {
        c_fail( "Out of memory" );
    }
    if ( cc->immediate ) {
        /* a stopped program's frames and statics are made: only the
           module's spare room can take more */
        if ( scope != &cc->module || is_static ) {
            c_fail( IMM_REFUSED );
        }
        if ( *where > cc->imm_limit ) {
            c_fail( "Out of memory" );
        }
    }
    return off;
}

static void list_add( u32 **list, u32 *count, u32 *cap, u32 val )
{
    u32 *grown;

    if ( *count == *cap ) {
        u32 ncap = *cap ? *cap * 2 : 16;
        grown = (u32 *)try_realloc( *list, ncap * sizeof( u32 ) );
        if ( grown == NULL ) {
            c_fail( "Out of memory" );          /* the list and its size as they were */
        }
        *list = grown;
        *cap = ncap;
    }
    (*list)[(*count)++] = val;
}

void scope_note_str( SCOPE *scope, u32 off )
{
    list_add( &scope->rel_str, &scope->nrel_str, &scope->cap_str, off );
}

void scope_note_arr( SCOPE *scope, u32 off )
{
    list_add( &scope->rel_arr, &scope->nrel_arr, &scope->cap_arr, off );
}

/* a string or an array in a procedure's static block: let go of when
   the run ends, not when the call does */
void scope_note_static( SCOPE *scope, u32 off, int is_array )
{
    if ( is_array ) {
        list_add( &scope->srel_arr, &scope->nsrel_arr, &scope->cap_sarr, off );
    } else {
        list_add( &scope->srel_str, &scope->nsrel_str, &scope->cap_sstr, off );
    }
}

int type_default( SCOPE *scope, const char *upper )
{
    int letter = upper[0] - 'A';

    if ( letter < 0 || letter >= 26 ) {
        return TY_SINGLE;
    }
    return scope->deftype[letter];
}

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */

u32 type_size( QTYPE type )
{
    switch ( TKIND( type ) ) {
    case TY_INT:    return 2;
    case TY_LONG:   return 4;
    case TY_SINGLE: return 4;
    case TY_DOUBLE: return 8;
    case TY_STR:    return 4;
    case TY_FSTR:   return TARG( type );
    case TY_REC:    return cc->recs[TARG( type )].size;
    }
    return 4;
}

const char *type_name( QTYPE type )
{
    switch ( TKIND( type ) ) {
    case TY_INT:    return "INTEGER";
    case TY_LONG:   return "LONG";
    case TY_SINGLE: return "SINGLE";
    case TY_DOUBLE: return "DOUBLE";
    case TY_STR:
    case TY_FSTR:   return "STRING";
    case TY_REC:    return cc->recs[TARG( type )].name;
    }
    return "ANY";
}

SYM *type_find( const char *upper )
{
    SYM *sym;

    for ( sym = cc->types[name_hash( upper )]; sym != NULL; sym = sym->next ) {
        if ( str_cmp( sym->name, upper ) == 0 ) {
            return sym;
        }
    }
    return NULL;
}

int rec_field( int rec, const char *upper, QTYPE *type, u32 *offset )
{
    RECDEF *def = &cc->recs[rec];
    int i;

    if ( def->fields == NULL ) {
        return 0;                               /* a TYPE not finished (a line check) */
    }
    for ( i = 0; i < def->nfields; i++ ) {
        if ( str_icmp( def->fields[i].name, upper ) == 0 ) {
            *type = def->fields[i].type;
            *offset = def->fields[i].offset;
            return 1;
        }
    }
    return 0;
}

/* what follows AS: INTEGER, LONG, SINGLE, DOUBLE, STRING, STRING * n,
   ANY (DECLARE only), or a TYPE's name */
QTYPE parse_as_type( int allow_any, int allow_fixed )
{
    TOKEN *tok = c_tok();
    char upper[NAME_MAX + 1];
    SYM *sym;
    CELL val;
    QTYPE vtype;
    STR *sval;
    s32 len;

    if ( tok->kind == TK_KW ) {
        switch ( tok->kw ) {
        case KW_INTEGER:    c_next(); return TY_INT;
        case KW_LONG:       c_next(); return TY_LONG;
        case KW_SINGLE:     c_next(); return TY_SINGLE;
        case KW_DOUBLE:     c_next(); return TY_DOUBLE;
        case KW_ANY:
            if ( !allow_any ) {
                break;
            }
            c_next();
            return TY_ANY;
        case KW_STRING:
            c_next();
            if ( !c_accept( TK_STAR ) ) {
                return TY_STR;
            }
            if ( !allow_fixed ) {
                c_fail_tok( "Fixed-length string not allowed here" );
            }
            if ( !const_expr( &val, &vtype, &sval ) || !IS_NUMT( vtype ) ) {
                c_fail( "Invalid constant" );
            }
            len = vtype == TY_SINGLE ? (s32)fp_round( val.s ) :
                  vtype == TY_DOUBLE ? (s32)fp_round( val.d ) : val.l;
            if ( len < 1 || len > STR_MAX ) {
                c_fail_tok( "Overflow" );
            }
            return MKTYPE( TY_FSTR, len );
        }
    }
    if ( tok->kind == TK_ID && tok->kw == 0 ) {
        c_name_of( tok, upper );
        sym = type_find( upper );
        if ( sym != NULL ) {
            c_next();
            return MKTYPE( TY_REC, sym->rec );
        }
        c_fail( "Type not defined" );
    }
    c_expect_msg( "INTEGER, LONG, SINGLE, DOUBLE, STRING, or user type" );
    return TY_NONE;
}

/* A hidden variable: a FOR's limit and step, SELECT's value, an
   argument passed by value.  In the module it is the module's; in a
   procedure it is the call's own, so that a recursive call gets fresh
   ones. */
u32 c_temp( QTYPE type, u8 *scope_out )
{
    SCOPE *scope;
    u32 off;

    if ( cc->immediate ) {
        scope = &cc->module;                /* a frame's size is fixed by now */
        *scope_out = SC_GLOBAL;
    } else if ( cc->in_deffn >= 0 ) {
        scope = cc->deffn_scope;
        *scope_out = SC_FRAME;
    } else if ( cc->procscope != NULL ) {
        scope = cc->procscope;
        *scope_out = SC_FRAME;
    } else {
        scope = &cc->module;
        *scope_out = SC_GLOBAL;
    }
    off = scope_alloc( scope, type_size( type ), 0 );
    if ( TKIND( type ) == TY_STR ) {
        scope_note_str( scope, off );
    }
    return off;
}
