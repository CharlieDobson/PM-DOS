/*
 * COMP.C - statements, blocks, procedures, and the program as a whole.
 *
 * EVERY STATEMENT THAT DOES SOMETHING STARTS WITH OP_STMT and its
 * STMTLOC: where it is in the text (the cursor goes there for an error,
 * a breakpoint, a step) and the range of its code (RESUME NEXT goes to
 * the end of it).  A statement that emits nothing more - DIM of a
 * static array, DECLARE, a DEFtype - has its OP_STMT taken back.
 *
 * A BLOCK (IF...END IF, FOR...NEXT, DO...LOOP, WHILE...WEND, SELECT,
 * DEF FN...END DEF) is a BLOCK record on a stack while it is open: the
 * jumps out of it wait there to be patched when its end is reached.
 * The part of ELSE, ELSEIF and CASE that ends the previous branch
 * belongs to that branch, so it is emitted before their OP_STMT.
 */
#include "comp.h"

static void define_label( int module_level, TOKEN *tok );

/* ------------------------------------------------------------------ */
/* lines and statements                                                */
/* ------------------------------------------------------------------ */

/* the text of a line in one of the program's texts */
static void get_line( int text, u32 line, const char **src, u32 *len )
{
    LINE *ln = doc_line( cc->pt->texts[text], line );

    *src = ln ? ln->text : "";
    *len = ln ? ln->len : 0;
}

static void lex_src( const char *src, u32 len )
{
    int col;
    const char *msg;

    cc->src = src;
    cc->srclen = len;
    cc->tp = 0;
    cc->ntoks = lex_line( src, len, cc->toks, MAX_TOKENS, &col, &msg );
    if ( cc->ntoks < 0 ) {
        cc->ntoks = 1;
        cc->toks[0].kind = TK_EOL;
        cc->toks[0].col = (u16)col;
        cc->toks[0].len = 1;
        c_fail_at( 0, msg );
    }
}

static void stmt_mark( void )
{
    STMTLOC *loc;
    TOKEN *tok = c_tok();

    if ( cc->nlocs == cc->locs_cap ) {
        u32 cap = cc->locs_cap ? cc->locs_cap * 2 : 1024;
        STMTLOC *grown = (STMTLOC *)try_realloc( cc->locs, cap * sizeof( STMTLOC ) );
        if ( grown == NULL ) {
            c_fail( "Out of memory" );
        }
        cc->locs = grown;
        cc->locs_cap = cap;
    }
    cc->cur_stmt = cc->nlocs;
    loc = &cc->locs[cc->nlocs++];
    mem_set( loc, 0, sizeof( *loc ) );
    loc->pc = c_here();
    loc->text = (u16)cc->text;
    loc->line = cc->line;
    loc->col = tok->col;
    loc->lineno = cc->lineno;
    loc->proc = (u16)(cc->cur_proc + 1);
    c_op32( OP_STMT, cc->cur_stmt );
}

/* the OP_STMT just made, taken back: nothing followed it */
static void stmt_unmark( void )
{
    if ( cc->nlocs > 0 && cc->locs[cc->nlocs - 1].pc + 5 == c_here() ) {
        cc->code_len -= 5;
        cc->nlocs--;
    }
}

static void stmt_finish( u32 first_loc )
{
    u32 i;
    TOKEN *tok = &cc->toks[cc->tp > 0 ? cc->tp - 1 : 0];
    u32 end_col = tok->col + tok->len;

    stmt_unmark();
    for ( i = first_loc; i < cc->nlocs; i++ ) {
        STMTLOC *loc = &cc->locs[i];
        if ( loc->end_pc == 0 ) {
            loc->end_pc = c_here();
            if ( loc->len == 0 ) {
                loc->len = (u16)(end_col > loc->col ? end_col - loc->col : 1);
            }
        }
    }
}

/* one statement, with its OP_STMT */
static void one_statement( void )
{
    u32 first = cc->nlocs;

    stmt_mark();
    c_statement();
    stmt_finish( first );
}

/* statements up to the end of the line - or, inside a one-line IF, up
   to an ELSE */
void c_statements_until_eol( int stop_at_else )
{
    for ( ;; ) {
        if ( c_is( TK_EOL ) || c_is( TK_REM ) ) {
            return;
        }
        if ( stop_at_else && c_is_kw( KW_ELSE ) ) {
            return;
        }
        if ( c_is( TK_COLON ) ) {
            c_next();
            continue;
        }
        one_statement();
        if ( c_is( TK_COLON ) ) {
            c_next();
            continue;
        }
        if ( c_is( TK_EOL ) || c_is( TK_REM ) ) {
            return;
        }
        if ( c_is_kw( KW_ELSE ) ) {
            if ( stop_at_else ) {
                return;
            }
            c_fail( "ELSE without IF" );
        }
        c_expect_msg( "end-of-statement" );
    }
}

/* '$DYNAMIC and '$STATIC in a remark */
static void metacommand( TOKEN *tok )
{
    const char *text = cc->src + tok->col;
    u32 len = tok->len, pos;

    pos = (tok->flags & TF_APOS) ? 1 : 3;
    while ( pos < len && (text[pos] == ' ' || text[pos] == '\t') ) {
        pos++;
    }
    if ( pos < len && text[pos] == '$' ) {
        if ( len - pos >= 8 && str_nicmp( text + pos, "$DYNAMIC", 8 ) == 0 ) {
            cc->dynamic = 1;
        } else if ( len - pos >= 7 && str_nicmp( text + pos, "$STATIC", 7 ) == 0 ) {
            cc->dynamic = 0;
        }
    }
}

/* a line of a text: its label or number, then its statements */
static void compile_line( void )
{
    TOKEN *tok = c_tok();
    int module_level = cc->procscope == NULL && cc->in_deffn < 0;

    cc->neback = 0;                     /* a failed line's are not waiting */
    if ( tok->kind == TK_LINENUM ) {
        cc->lineno = (s32)tok->num;
        define_label( module_level, tok );
        c_next();
        tok = c_tok();
    }
    if ( tok->kind == TK_LABEL ) {
        define_label( module_level, tok );
        c_next();
    }
    c_statements_until_eol( 0 );
    if ( c_is( TK_REM ) ) {
        metacommand( c_tok() );
    }
}

/* a TYPE's first line in pass 2: only its label, if it has one */
static void compile_line_label_only( void )
{
    TOKEN *tok = c_tok();

    if ( tok->kind == TK_LINENUM ) {
        cc->lineno = (s32)tok->num;
        define_label( 1, tok );
        tok++;
    }
    if ( tok->kind == TK_LABEL ) {
        define_label( 1, tok );
    }
}

/* ------------------------------------------------------------------ */
/* labels                                                              */
/* ------------------------------------------------------------------ */

static void define_label( int module_level, TOKEN *tok )
{
    char upper[NAME_MAX + 1];
    SCOPE *scope = (module_level || cc->procscope == NULL) ? &cc->module : cc->procscope;
    SYM *sym;
    FIXUP *fix;
    int len, i;

    if ( tok->kind == TK_LINENUM ) {
        fmt_dec( (u32)tok->num, 0, upper );
    } else {
        len = tok->len > NAME_MAX ? NAME_MAX : tok->len;
        for ( i = 0; i < len; i++ ) {
            upper[i] = (char)ch_upper( cc->src[tok->col + i] );
        }
        upper[len] = 0;
    }
    if ( cc->checking ) {
        return;
    }
    sym = sym_find( scope, upper, 0, SK_LABEL );
    if ( sym == NULL ) {
        sym = sym_add( scope, upper, 0, SK_LABEL );
    } else if ( sym->addr != NOADDR ) {
        c_fail_at( cc->tp, "Duplicate label" );
    }
    sym->addr = c_here();
    sym->data_at = cc->ndata;
    for ( fix = sym->fixups; fix != NULL; fix = fix->next ) {
        c_patch32( fix->at, fix->len < 0 ? sym->data_at : sym->addr );
    }
    sym->fixups = NULL;
}

static void check_labels( SCOPE *scope )
{
    SYM *sym;
    int i;

    for ( i = 0; i < NBUCKET; i++ ) {
        for ( sym = scope->bucket[i]; sym != NULL; sym = sym->next ) {
            if ( sym->kind == SK_LABEL && sym->addr == NOADDR && sym->fixups != NULL ) {
                FIXUP *fix = sym->fixups;
                cc->err->text = fix->text;
                cc->err->line = fix->line;
                cc->err->col = fix->col;
                cc->err->len = fix->len > 0 ? fix->len : 1;
                str_cpy( cc->err->msg, "Label not defined" );
                rt_longjmp( &cc->fail, 1 );
            }
        }
    }
}

/* RESTORE's label: the number of the DATA item after it */
static void emit_data_label( SYM *label )
{
    FIXUP *fix;
    TOKEN *tok = &cc->toks[cc->tp > 0 ? cc->tp - 1 : 0];

    c_emit( OP_CI );
    if ( label->addr != NOADDR ) {
        c_emit32( label->data_at );
        return;
    }
    fix = (FIXUP *)c_pool( sizeof( FIXUP ) );
    fix->at = c_here();
    fix->text = cc->text;
    fix->line = cc->line;
    fix->col = tok->col;
    fix->len = -(int)tok->len - 1;              /* negative: a DATA number */
    fix->next = label->fixups;
    label->fixups = fix;
    c_emit32( 0 );
}

/* ------------------------------------------------------------------ */
/* blocks                                                              */
/* ------------------------------------------------------------------ */

static BLOCK *block_open( int kind )
{
    BLOCK *blk;
    TOKEN *tok = c_tok();

    if ( cc->nblocks >= MAX_BLOCKS ) {
        c_fail( "Program too complex" );
    }
    blk = &cc->blocks[cc->nblocks++];
    mem_set( blk, 0, sizeof( *blk ) );
    blk->kind = kind;
    blk->text = cc->text;
    blk->line = cc->line;
    blk->col = tok->col;
    blk->len = tok->len;
    return blk;
}

static BLOCK *block_top( int kind )
{
    if ( cc->nblocks == 0 || cc->blocks[cc->nblocks - 1].kind != kind ) {
        return NULL;
    }
    return &cc->blocks[cc->nblocks - 1];
}

/* a code address to be patched with the block's end */
static void block_add_exit( BLOCK *blk, u32 at )
{
    if ( blk->nexits == blk->cap_exits ) {
        int cap = blk->cap_exits ? blk->cap_exits * 2 : 8;
        u32 *grown = (u32 *)c_pool( cap * sizeof( u32 ) );
        if ( blk->exits ) {
            mem_cpy( grown, blk->exits, blk->nexits * sizeof( u32 ) );
        }
        blk->exits = grown;
        blk->cap_exits = cap;
    }
    blk->exits[blk->nexits++] = at;
}

static void block_exit_jump( BLOCK *blk, int op )
{
    block_add_exit( blk, c_jump( op ) );
}

static void block_close( BLOCK *blk )
{
    int i;

    for ( i = 0; i < blk->nexits; i++ ) {
        c_patch32( blk->exits[i], c_here() );
    }
    cc->nblocks--;
}

/* the innermost open block of a kind, for EXIT */
static BLOCK *block_find( int kind )
{
    int i;

    for ( i = cc->nblocks - 1; i >= 0; i-- ) {
        if ( cc->blocks[i].kind == kind ) {
            return &cc->blocks[i];
        }
        if ( cc->blocks[i].kind == B_DEFFN || cc->blocks[i].kind == B_PROC ) {
            break;
        }
    }
    return NULL;
}

/* a block left open at the end of a text */
static void block_unclosed( BLOCK *blk )
{
    const char *msg;

    switch ( blk->kind ) {
    case B_IF:      msg = "Block IF without END IF"; break;
    case B_DO:      msg = "DO without LOOP"; break;
    case B_WHILE:   msg = "WHILE without WEND"; break;
    case B_FOR:     msg = "FOR without NEXT"; break;
    case B_SELECT:  msg = "SELECT without END SELECT"; break;
    case B_DEFFN:   msg = "DEF without END DEF"; break;
    default:        msg = "SUB without END SUB"; break;
    }
    cc->err->text = blk->text;
    cc->err->line = blk->line;
    cc->err->col = blk->col;
    cc->err->len = blk->len ? blk->len : 1;
    str_cpy( cc->err->msg, msg );
    rt_longjmp( &cc->fail, 1 );
}

/* ------------------------------------------------------------------ */
/* small parsing helpers                                               */
/* ------------------------------------------------------------------ */

static void opt_long( int *mask, int bit )
{
    if ( c_is( TK_COMMA ) || c_at_end() || c_is( TK_RPAREN ) ) {
        c_push_l( 0 );
    } else {
        expr_long();
        *mask |= bit;
    }
}

static void file_num( void )
{
    c_accept( TK_HASH );
    expr_long();
}

static int sig_char( QTYPE type )
{
    switch ( TKIND( type ) ) {
    case TY_INT:    return 'I';
    case TY_LONG:   return 'L';
    case TY_SINGLE: return 'S';
    case TY_DOUBLE: return 'D';
    case TY_STR:
    case TY_FSTR:   return '$';
    }
    return '?';
}

/* a variable to be written to, for READ, INPUT, GET...: its address
   on the stack, its type returned */
static QTYPE store_target( VREF *ref )
{
    if ( !c_var( ref, 1, 0 ) ) {
        c_fail( "Variable required" );
    }
    c_addr( ref );
    return ref->type;
}

static void item_store( QTYPE type, int rt_i, int rt_l, int rt_s, int rt_d, int rt_str, int rt_fs )
{
    switch ( TKIND( type ) ) {
    case TY_INT:    c_rt( rt_i ); break;
    case TY_LONG:   c_rt( rt_l ); break;
    case TY_SINGLE: c_rt( rt_s ); break;
    case TY_DOUBLE: c_rt( rt_d ); break;
    case TY_STR:    c_rt( rt_str ); break;
    case TY_FSTR:
        c_push_l( (s32)TARG( type ) );
        c_rt( rt_fs );
        break;
    default:
        c_fail_tok( "Type mismatch" );
    }
}

/* (x, y) with STEP before it maybe: two doubles pushed; -> 1 for STEP */
static int coord( void )
{
    int step = c_accept_kw( KW_STEP );

    c_need( TK_LPAREN, "(" );
    expr_dbl();
    c_need( TK_COMMA, "," );
    expr_dbl();
    c_need( TK_RPAREN, ")" );
    return step;
}

/* an array for GET, PUT and PALETTE USING: the slot's address, then the
   element's (or 0 for the first) */
static void array_and_element( void )
{
    TOKEN *tok = c_tok();
    VREF ref;
    int save = cc->tp;

    if ( tok->kind != TK_ID ) {
        c_expect_msg( "identifier" );
    }
    c_array_slot( 0 );
    if ( c_is( TK_LPAREN ) ) {
        cc->tp = save;
        if ( !c_var( &ref, 0, 0 ) ) {
            c_fail( "Array not defined" );
        }
        c_addr( &ref );
    } else {
        c_push_l( 0 );
    }
}

/* ------------------------------------------------------------------ */
/* assignment and calls                                                */
/* ------------------------------------------------------------------ */

static void st_assign( void )
{
    VREF ref, src;
    QTYPE type;
    u32 at = c_here(), len = 0, i;
    u8 *target = NULL;

    if ( !c_var( &ref, 1, 0 ) ) {
        TOKEN *tok = c_tok();
        if ( tok->kind == TK_ID && (tok->flags & TF_FN) ) {
            c_fail( "Function not defined" );
        }
        c_fail( "Duplicate definition" );
    }
    if ( ref.simple && (TKIND( ref.type ) == TY_FSTR || TKIND( ref.type ) == TY_REC) ) {
        c_addr( &ref );
    }
    c_need( TK_EQ, "=" );
    if ( TKIND( ref.type ) == TY_REC ) {
        if ( !c_var( &src, 0, 0 ) || src.type != ref.type ) {
            c_fail( "Type mismatch" );
        }
        c_addr( &src );
        c_op32( OP_STREC, type_size( ref.type ) );
        return;
    }
    if ( !ref.simple && c_here() > at ) {
        /* QBasic works out the value first, then the subscripts (a
           FUNCTION in the value may change them, or REDIM the array):
           the target's code moves behind the value's.  Expression code
           has no jumps or fixups in it, so it can move. */
        len = c_here() - at;
        target = (u8 *)c_pool( len );
        mem_cpy( target, cc->code + at, len );
        cc->code_len = at;
    }
    type = expr();
    if ( target != NULL ) {
        if ( IS_STRT( ref.type ) ) {
            if ( TKIND( type ) != TY_STR ) {
                c_fail_tok( "Type mismatch" );
            }
        } else {
            if ( !IS_NUMT( type ) ) {
                c_fail_tok( "Type mismatch" );
            }
            c_convert( type, ref.type );
            type = ref.type;
        }
        for ( i = 0; i < len; i++ ) {
            c_emit( target[i] );
        }
        c_op( OP_EXCH );
    }
    c_store( &ref, type );
}

/* Is the statement at the current name an assignment?  A name followed
   by =, or by (...) and then = or a field. */
static int is_assignment( void )
{
    int at = cc->tp + 1, depth;

    if ( cc->toks[at].kind == TK_EQ || cc->toks[at].kind == TK_DOT ) {
        return 1;
    }
    if ( cc->toks[at].kind != TK_LPAREN ) {
        return 0;
    }
    depth = 0;
    for ( ; cc->toks[at].kind != TK_EOL; at++ ) {
        if ( cc->toks[at].kind == TK_LPAREN ) {
            depth++;
        } else if ( cc->toks[at].kind == TK_RPAREN && --depth == 0 ) {
            at++;
            break;
        }
    }
    return cc->toks[at].kind == TK_EQ || cc->toks[at].kind == TK_DOT;
}

static void call_sub( int parens )
{
    TOKEN *tok = c_tok();
    char upper[NAME_MAX + 1];
    SYM *sym;
    PROC *proc;
    int base;

    c_name_of( tok, upper );
    sym = sym_find( &cc->module, upper, 0, SK_PROC );
    if ( sym == NULL || cc->procs[sym->proc].kind != PK_SUB ) {
        if ( cc->checking ) {
            /* not known yet: read the arguments as expressions */
            c_next();
            if ( parens && c_accept( TK_LPAREN ) ) {
                if ( !c_is( TK_RPAREN ) ) {
                    do {
                        expr();
                    } while ( c_accept( TK_COMMA ) );
                }
                c_need( TK_RPAREN, ")" );
            } else if ( !c_at_end() ) {
                do {
                    expr();
                } while ( c_accept( TK_COMMA ) );
            }
            return;
        }
        c_fail( "Subprogram not defined" );
    }
    if ( tok->ntype ) {
        c_fail( "Syntax error" );
    }
    proc = &cc->procs[sym->proc];
    if ( !proc->defined ) {
        c_fail( "Subprogram not defined" );
    }
    c_next();
    base = cc->neback;
    c_call_args( proc, parens );
    c_op( OP_CALL );
    c_emit16( (u32)sym->proc );
    c_call_back( base );
}

static void st_call( void )
{
    if ( c_accept_kw( KW_ABSOLUTE ) ) {
        /* machine code: there is no real mode here to run it in */
        c_need( TK_LPAREN, "(" );
        for ( ;; ) {
            VREF ref;
            int save = cc->tp;
            u32 at = c_here();
            if ( c_is( TK_ID ) && c_var( &ref, 0, 0 ) && (c_is( TK_COMMA ) || c_is( TK_RPAREN )) ) {
                cc->code_len = at;
            } else {
                cc->tp = save;
                cc->code_len = at;
                expr();
                c_op( OP_POPN );
            }
            if ( !c_accept( TK_COMMA ) ) {
                break;
            }
        }
        c_need( TK_RPAREN, ")" );
        c_push_l( 0 );
        c_rt( RT_CALLABS );
        return;
    }
    if ( !c_is( TK_ID ) ) {
        c_expect_msg( "identifier" );
    }
    call_sub( 1 );
}

/* ------------------------------------------------------------------ */
/* PRINT, WRITE, INPUT                                                 */
/* ------------------------------------------------------------------ */

static void print_value( QTYPE type, int using )
{
    if ( TKIND( type ) == TY_STR ) {
        c_rt( using ? RT_PRUSTR : RT_PRSTR );
        return;
    }
    if ( using ) {
        if ( TKIND( type ) == TY_SINGLE ) {
            c_rt( RT_PRUNUMS );
        } else {
            c_convert( type, TY_DOUBLE );
            c_rt( RT_PRUNUMD );
        }
        return;
    }
    switch ( TKIND( type ) ) {
    case TY_INT:
    case TY_LONG:   c_rt( RT_PRNUML ); break;
    case TY_SINGLE: c_rt( RT_PRNUMS ); break;
    default:        c_rt( RT_PRNUMD ); break;
    }
}

static void st_print( int lprint )
{
    int using = 0, last_sep = 0;

    if ( !lprint && c_accept( TK_HASH ) ) {
        expr_long();
        c_need( TK_COMMA, "," );
    } else {
        c_push_l( lprint ? -1 : 0 );
    }
    c_rt( RT_PRCHAN );
    if ( c_accept_kw( KW_USING ) ) {
        expr_str();
        c_need( TK_SEMI, ";" );
        c_rt( RT_PRUSING );
        using = 1;
    }
    while ( !c_at_end() ) {
        if ( c_accept( TK_SEMI ) ) {
            last_sep = 1;
            continue;
        }
        if ( !using && c_accept_kw( KW_USING ) ) {
            /* USING after some plain items: the rest go through it */
            expr_str();
            c_need( TK_SEMI, ";" );
            c_rt( RT_PRUSING );
            using = 1;
            last_sep = 1;
            continue;
        }
        if ( c_accept( TK_COMMA ) ) {
            if ( !using ) {
                c_rt( RT_PRCOMMA );
            }
            last_sep = 1;
            continue;
        }
        if ( !using && (c_is_kw( KW_TAB ) || c_is_kw( KW_SPC )) ) {
            int tab = c_is_kw( KW_TAB );
            c_next();
            c_need( TK_LPAREN, "(" );
            expr_long();
            c_need( TK_RPAREN, ")" );
            c_rt( tab ? RT_PRTAB : RT_PRSPC );
            last_sep = 1;
            if ( c_at_end() ) {
                last_sep = 0;
            }
            continue;
        }
        print_value( expr(), using );
        last_sep = 0;
    }
    if ( using ) {
        c_push_l( last_sep ? 0 : 1 );
        c_rt( RT_PRUEND );
    } else if ( !last_sep ) {
        c_rt( RT_PREOL );
    }
}

static void st_write( void )
{
    QTYPE type;

    if ( c_accept( TK_HASH ) ) {
        expr_long();
        c_need( TK_COMMA, "," );
    } else {
        c_push_l( 0 );
    }
    c_rt( RT_PRCHAN );
    while ( !c_at_end() ) {
        type = expr();
        switch ( TKIND( type ) ) {
        case TY_STR:    c_rt( RT_WRSTR ); break;
        case TY_INT:
        case TY_LONG:   c_rt( RT_WRNUML ); break;
        case TY_SINGLE: c_rt( RT_WRNUMS ); break;
        default:        c_rt( RT_WRNUMD ); break;
        }
        if ( !c_accept( TK_COMMA ) && !c_accept( TK_SEMI ) ) {
            break;
        }
        c_rt( RT_WRSEP );
    }
    c_rt( RT_PREOL );
}

/* the prompt and the flags of INPUT and LINE INPUT */
static int input_prompt( int line_input )
{
    int flags = 0;
    TOKEN *tok;

    if ( c_accept( TK_SEMI ) ) {
        flags |= 1;                     /* the cursor stays on the line */
    }
    tok = c_tok();
    if ( tok->kind == TK_STR ) {
        u32 len = tok->len - 1;
        if ( !(tok->flags & TF_UNTERM) ) {
            len--;
        }
        c_next();
        c_op32( OP_CSTR, c_const_str( cc->src + tok->col + 1, len ) );
        if ( c_accept( TK_SEMI ) ) {
            if ( !line_input ) {
                flags |= 2;             /* and a question mark after it */
            }
        } else if ( !c_accept( TK_COMMA ) ) {
            c_expect_msg( "; or ," );
        }
    } else {
        c_op32( OP_CSTR, c_const_str( "", 0 ) );
        if ( !line_input ) {
            flags |= 2;
        }
    }
    c_push_l( flags );
    return flags;
}

static void st_input( void )
{
    char sig[MAX_TOKENS];
    int count = 0, save_tp;
    u32 save_code;
    VREF ref;

    if ( c_accept( TK_HASH ) ) {
        expr_long();
        c_need( TK_COMMA, "," );
        c_rt( RT_INFILE );
        do {
            item_store( store_target( &ref ), RT_INITEMI, RT_INITEML, RT_INITEMS, RT_INITEMD,
                        RT_INITEMSTR, RT_INITEMFS );
        } while ( c_accept( TK_COMMA ) );
        return;
    }
    input_prompt( 0 );
    /* the types of the variables first, so the line can be checked
       before anything is stored */
    save_tp = cc->tp;
    save_code = c_here();
    do {
        if ( !c_var( &ref, 1, 0 ) ) {
            c_fail( "Variable required" );
        }
        sig[count++] = (char)sig_char( ref.type );
        if ( sig[count - 1] == '?' ) {
            c_fail_tok( "Type mismatch" );
        }
    } while ( c_accept( TK_COMMA ) );
    cc->code_len = save_code;
    cc->tp = save_tp;
    c_op32( OP_CSTR, c_const_str( sig, (u32)count ) );
    c_rt( RT_INBEGIN );
    do {
        item_store( store_target( &ref ), RT_INITEMI, RT_INITEML, RT_INITEMS, RT_INITEMD,
                    RT_INITEMSTR, RT_INITEMFS );
    } while ( c_accept( TK_COMMA ) );
}

static void st_line_input( void )
{
    VREF ref;
    QTYPE type;

    if ( c_accept( TK_HASH ) ) {
        expr_long();
        c_need( TK_COMMA, "," );
        type = store_target( &ref );
        if ( !IS_STRT( type ) ) {
            c_fail_tok( "Type mismatch" );
        }
        c_push_l( TKIND( type ) == TY_FSTR ? (s32)TARG( type ) : 0 );
        c_rt( RT_LINEINF );
        return;
    }
    input_prompt( 1 );
    type = store_target( &ref );
    if ( !IS_STRT( type ) ) {
        c_fail_tok( "Type mismatch" );
    }
    c_push_l( TKIND( type ) == TY_FSTR ? (s32)TARG( type ) : 0 );
    c_rt( RT_LINEIN );
}

/* ------------------------------------------------------------------ */
/* IF, loops, SELECT                                                   */
/* ------------------------------------------------------------------ */

static void cond( void )
{
    QTYPE type = expr();

    if ( !IS_NUMT( type ) ) {
        c_fail_tok( "Type mismatch" );
    }
    if ( TKIND( type ) == TY_SINGLE || TKIND( type ) == TY_DOUBLE ) {
        /* any value other than 0 is true: compare it with 0 */
        if ( TKIND( type ) == TY_SINGLE ) {
            c_op( OP_CS );
            c_emitf( 0.0f );
            c_op( OP_CMPS );
        } else {
            c_op( OP_CD );
            c_emitd( 0.0 );
            c_op( OP_CMPD );
        }
        c_emit( CC_NE );
    }
}

/* after THEN or ELSE on one line: a line number alone is a GOTO */
static void branch_part( int stop_at_else )
{
    if ( c_is( TK_NUM ) ) {
        SYM *label;
        u32 first = cc->nlocs;
        stmt_mark();
        label = c_label_ref( 0 );
        c_op( cc->immediate ? OP_IMMGOTO : OP_JMP );
        c_emit_label( label );
        stmt_finish( first );
        return;
    }
    c_statements_until_eol( stop_at_else );
}

static void st_if( void )
{
    BLOCK *blk;
    u32 to_else, to_end, myloc = cc->cur_stmt;

    cond();
    if ( c_accept_kw( KW_GOTO ) ) {
        /* IF x GOTO label [ELSE ...] */
        SYM *label = c_label_ref( 0 );
        if ( cc->immediate ) {
            u32 skip = c_jump( OP_JZ );         /* the program goes on from the label */
            c_op( OP_IMMGOTO );
            c_emit_label( label );
            c_patch32( skip, c_here() );
        } else {
            c_op( OP_JNZ );
            c_emit_label( label );
        }
        cc->locs[myloc].end_pc = c_here();
        if ( c_accept_kw( KW_ELSE ) ) {
            branch_part( cc->sl_then > 0 );
        }
        return;
    }
    c_need_kw( KW_THEN );
    if ( c_is( TK_EOL ) || c_is( TK_REM ) ) {
        /* a block IF */
        blk = block_open( B_IF );
        blk->patch = c_jump( OP_JZ );
        cc->locs[myloc].end_pc = c_here();       /* RESUME NEXT: into the THEN part */
        return;
    }
    to_else = c_jump( OP_JZ );
    cc->locs[myloc].end_pc = c_here();
    cc->sl_then++;
    branch_part( 1 );
    cc->sl_then--;
    if ( c_accept_kw( KW_ELSE ) ) {
        to_end = c_jump( OP_JMP );
        c_patch32( to_else, c_here() );
        /* an IF inside another one's THEN: the next ELSE is the outer one's */
        branch_part( cc->sl_then > 0 );
        c_patch32( to_end, c_here() );
    } else {
        c_patch32( to_else, c_here() );
    }
}

static void st_elseif( void )
{
    BLOCK *blk = block_top( B_IF );

    if ( blk == NULL || blk->seen_else ) {
        if ( cc->checking ) {
            cond();
            c_need_kw( KW_THEN );
            return;
        }
        c_fail_tok( "ELSEIF without IF" );
    }
    stmt_unmark();
    block_exit_jump( blk, OP_JMP );
    c_patch32( blk->patch, c_here() );
    stmt_mark();
    cond();
    c_need_kw( KW_THEN );
    blk->patch = c_jump( OP_JZ );
    cc->locs[cc->cur_stmt].end_pc = c_here();
    if ( !c_is( TK_EOL ) && !c_is( TK_REM ) ) {
        c_expect_msg( "end-of-statement" );
    }
}

static void st_else( void )
{
    BLOCK *blk = block_top( B_IF );

    if ( blk == NULL || blk->seen_else ) {
        if ( cc->checking ) {
            return;
        }
        c_fail_tok( "ELSE without IF" );
    }
    stmt_unmark();
    block_exit_jump( blk, OP_JMP );
    c_patch32( blk->patch, c_here() );
    blk->patch = 0;
    blk->seen_else = 1;
    /* statements may follow ELSE on its line */
}

static void st_end_if( void )
{
    BLOCK *blk = block_top( B_IF );

    if ( blk == NULL ) {
        if ( cc->checking ) {
            return;
        }
        c_fail_tok( "END IF without block IF" );
    }
    stmt_unmark();
    if ( blk->patch ) {
        c_patch32( blk->patch, c_here() );
    }
    block_close( blk );
}

static int for_op( QTYPE type, int first )
{
    switch ( TKIND( type ) ) {
    case TY_INT:    return first ? OP_FORI : OP_NEXTI;
    case TY_LONG:   return first ? OP_FORL : OP_NEXTL;
    case TY_SINGLE: return first ? OP_FORS : OP_NEXTS;
    }
    return first ? OP_FORD : OP_NEXTD;
}

static void emit_ref( u8 scope, u32 off )
{
    c_emit( scope );
    c_emit32( off );
}

/* a hidden variable of the FOR's type gets the value on the stack */
static void store_temp( u8 scope, u32 off, QTYPE type, QTYPE from )
{
    VREF tmp;

    mem_set( &tmp, 0, sizeof( tmp ) );
    tmp.simple = 1;
    tmp.scope = scope;
    tmp.offset = off;
    tmp.type = type;
    c_store( &tmp, from );
}

static QTYPE num_expr( void )
{
    QTYPE type = expr();

    if ( !IS_NUMT( type ) ) {
        c_fail_tok( "Type mismatch" );
    }
    return type;
}

/* FOR var = start TO limit [STEP step]: the limit and the step are
   worked out once and kept in hidden variables; OP_FORx makes the first
   test and OP_NEXTx each later one */
static void st_for( void )
{
    BLOCK *blk;
    VREF ref;

    if ( !c_is( TK_ID ) || !c_var( &ref, 1, 0 ) ) {
        c_expect_msg( "variable" );
    }
    if ( !IS_NUMT( ref.type ) || ref.sym == NULL || ref.sym->kind != SK_VAR
      || (!ref.simple && ref.sym->scope != SC_REF && ref.sym->scope != SC_STATIC)
      || (ref.simple && ref.offset != ref.sym->offset) ) {
        c_fail_tok( "Variable required" );
    }
    blk = block_open( B_FOR );
    blk->vtype = ref.type;
    blk->vsym = ref.sym;
    blk->vscope = ref.simple ? ref.scope : ref.sym->scope;
    blk->voff = ref.sym->offset;
    c_need( TK_EQ, "=" );
    c_store( &ref, num_expr() );                /* through its address, if it has one */
    c_need_kw( KW_TO );
    blk->loff = c_temp( ref.type, &blk->lscope );
    store_temp( blk->lscope, blk->loff, ref.type, num_expr() );
    blk->soff = c_temp( ref.type, &blk->sscope );
    if ( c_accept_kw( KW_STEP ) ) {
        store_temp( blk->sscope, blk->soff, ref.type, num_expr() );
    } else {
        c_push_l( 1 );
        store_temp( blk->sscope, blk->soff, ref.type, TY_INT );
    }
    c_op( for_op( ref.type, 1 ) );
    emit_ref( blk->vscope, blk->voff );
    emit_ref( blk->lscope, blk->loff );
    emit_ref( blk->sscope, blk->soff );
    block_add_exit( blk, c_here() );            /* where to go when it does not run */
    c_emit32( 0 );
    blk->top = c_here();
}

/* NEXT [var [, var]...] */
static void st_next( void )
{
    BLOCK *blk;
    char upper[NAME_MAX + 1];
    int first = 1;

    for ( ;; ) {
        blk = block_top( B_FOR );
        if ( blk == NULL ) {
            if ( cc->checking ) {
                while ( !c_at_end() ) {
                    c_next();
                }
                return;
            }
            c_fail_tok( "NEXT without FOR" );
        }
        if ( c_is( TK_ID ) ) {
            VREF ref;
            u32 at = c_here();
            int save = cc->tp;
            c_name_of( c_tok(), upper );
            if ( !c_var( &ref, 1, 0 ) || ref.sym != blk->vsym ) {
                cc->tp = save;
                c_fail( "NEXT without FOR" );
            }
            cc->code_len = at;
        } else if ( !first ) {
            c_expect_msg( "identifier" );
        }
        if ( !first ) {
            /* the second variable's NEXT is its own statement */
        }
        c_op( for_op( blk->vtype, 0 ) );
        emit_ref( blk->vscope, blk->voff );
        emit_ref( blk->lscope, blk->loff );
        emit_ref( blk->sscope, blk->soff );
        c_emit32( blk->top );
        block_close( blk );
        first = 0;
        if ( !c_accept( TK_COMMA ) ) {
            break;
        }
    }
}

static void st_do( void )
{
    BLOCK *blk = block_open( B_DO );

    blk->top = c_here();
    if ( c_accept_kw( KW_WHILE ) ) {
        cond();
        block_exit_jump( blk, OP_JZ );
    } else if ( c_accept_kw( KW_UNTIL ) ) {
        cond();
        block_exit_jump( blk, OP_JNZ );
    } else {
        stmt_unmark();
        blk->top = c_here();
    }
}

static void st_loop( void )
{
    BLOCK *blk = block_top( B_DO );

    if ( blk == NULL ) {
        if ( cc->checking ) {
            if ( c_accept_kw( KW_WHILE ) || c_accept_kw( KW_UNTIL ) ) {
                cond();
            }
            return;
        }
        c_fail_tok( "LOOP without DO" );
    }
    if ( c_accept_kw( KW_WHILE ) ) {
        cond();
        c_op32( OP_JNZ, blk->top );
    } else if ( c_accept_kw( KW_UNTIL ) ) {
        cond();
        c_op32( OP_JZ, blk->top );
    } else {
        c_op32( OP_JMP, blk->top );
    }
    block_close( blk );
}

static void st_while( void )
{
    BLOCK *blk = block_open( B_WHILE );

    blk->top = c_here() - 5;                    /* the OP_STMT, run again each time */
    cond();
    block_exit_jump( blk, OP_JZ );
}

static void st_wend( void )
{
    BLOCK *blk = block_top( B_WHILE );

    if ( blk == NULL ) {
        if ( cc->checking ) {
            return;
        }
        c_fail_tok( "WEND without WHILE" );
    }
    c_op32( OP_JMP, blk->top );
    block_close( blk );
}

static void st_exit( void )
{
    BLOCK *blk;

    if ( c_accept_kw( KW_FOR ) ) {
        blk = block_find( B_FOR );
        if ( blk == NULL ) {
            if ( cc->checking ) {
                return;
            }
            c_fail_tok( "EXIT FOR not within FOR...NEXT" );
        }
        block_exit_jump( blk, OP_JMP );
    } else if ( c_accept_kw( KW_DO ) ) {
        blk = block_find( B_DO );
        if ( blk == NULL ) {
            if ( cc->checking ) {
                return;
            }
            c_fail_tok( "EXIT DO not within DO...LOOP" );
        }
        block_exit_jump( blk, OP_JMP );
    } else if ( c_accept_kw( KW_SUB ) ) {
        if ( !cc->checking && (cc->cur_proc < 0 || cc->procs[cc->cur_proc].kind != PK_SUB || cc->in_deffn >= 0) ) {
            c_fail_tok( cc->cur_proc < 0 && cc->in_deffn < 0 ? "Illegal outside of SUB/FUNCTION"
                                                         : "EXIT SUB not within SUB" );
        }
        c_op( OP_RET );
    } else if ( c_accept_kw( KW_FUNCTION ) ) {
        if ( !cc->checking && (cc->cur_proc < 0 || cc->procs[cc->cur_proc].kind != PK_FUNC || cc->in_deffn >= 0) ) {
            c_fail_tok( cc->cur_proc < 0 && cc->in_deffn < 0 ? "Illegal outside of SUB/FUNCTION"
                                                         : "EXIT FUNCTION not within FUNCTION" );
        }
        c_op( OP_FRET );
    } else if ( c_accept_kw( KW_DEF ) ) {
        if ( !cc->checking && cc->in_deffn < 0 ) {
            c_fail_tok( "EXIT DEF not within DEF FN" );
        }
        c_op( OP_FRET );
    } else {
        c_expect_msg( "DEF, DO, FOR, FUNCTION, or SUB" );
    }
}

static void st_select( void )
{
    BLOCK *blk;
    QTYPE type;
    VREF tmp;

    c_need_kw( KW_CASE );
    type = expr();
    blk = block_open( B_SELECT );
    blk->loc = cc->cur_stmt;
    blk->seltype = value_type( type );
    blk->seloff = c_temp( blk->seltype, &blk->selscope );
    mem_set( &tmp, 0, sizeof( tmp ) );
    tmp.simple = 1;
    tmp.scope = blk->selscope;
    tmp.offset = blk->seloff;
    tmp.type = blk->seltype;
    c_store( &tmp, type );
    blk->patch = 0;
}

static void load_selector( BLOCK *blk )
{
    VREF tmp;

    mem_set( &tmp, 0, sizeof( tmp ) );
    tmp.simple = 1;
    tmp.scope = blk->selscope;
    tmp.offset = blk->seloff;
    tmp.type = blk->seltype;
    c_load( &tmp );
}

/* the selector compared with an expression: an INTEGER on the stack */
static void case_compare( BLOCK *blk, int cc_code )
{
    QTYPE type;

    load_selector( blk );
    type = expr();
    if ( TKIND( blk->seltype ) == TY_STR ) {
        if ( TKIND( type ) != TY_STR ) {
            c_fail_tok( "Type mismatch" );
        }
        c_op( OP_CMPSTR );
    } else {
        int want;
        if ( !IS_NUMT( type ) ) {
            c_fail_tok( "Type mismatch" );
        }
        want = TKIND( blk->seltype ) > TKIND( type ) ? TKIND( blk->seltype ) : TKIND( type );
        if ( TKIND( blk->seltype ) == TY_INT || TKIND( blk->seltype ) == TY_LONG ) {
            /* a whole-number selector: the CASE value is rounded to it
               (CASE 2.4 matches 2) */
            want = TY_LONG;
        }
        if ( want == TY_INT ) {
            want = TY_LONG;
        }
        if ( TKIND( blk->seltype ) != want ) {
            /* widen the selector, which is under the value */
            if ( want == TY_SINGLE ) {
                c_op( OP_UNDER_L2S );
            } else if ( want == TY_DOUBLE ) {
                c_op( TKIND( blk->seltype ) == TY_SINGLE ? OP_UNDER_S2D : OP_UNDER_L2D );
            }
        }
        c_convert( type, want );
        c_op( want == TY_DOUBLE ? OP_CMPD : want == TY_SINGLE ? OP_CMPS : OP_CMPL );
    }
    c_emit( cc_code );
}

static void st_case( void )
{
    BLOCK *blk = block_top( B_SELECT );
    u32 body_jumps[64];
    int nbody = 0, i, cc_code;

    if ( blk == NULL ) {
        if ( cc->checking ) {
            /* only its syntax */
            if ( c_accept_kw( KW_ELSE ) ) {
                return;
            }
            do {
                if ( c_accept_kw( KW_IS ) ) {
                    if ( c_tok()->kind < TK_EQ || c_tok()->kind > TK_NE ) {
                        c_expect_msg( "relational operator" );
                    }
                    c_next();
                    expr();
                } else {
                    expr();
                    if ( c_accept_kw( KW_TO ) ) {
                        expr();
                    }
                }
            } while ( c_accept( TK_COMMA ) );
            return;
        }
        c_fail_tok( "CASE without SELECT" );
    }
    stmt_unmark();
    if ( blk->ncases > 0 ) {
        block_exit_jump( blk, OP_JMP );         /* the end of the case before */
    }
    if ( blk->patch ) {
        c_patch32( blk->patch, c_here() );
        blk->patch = 0;
    }
    if ( blk->seen_else ) {
        c_fail_tok( "CASE ELSE must be the last CASE" );
    }
    blk->ncases++;
    if ( c_accept_kw( KW_ELSE ) ) {
        blk->seen_else = 1;
        return;
    }
    stmt_mark();
    for ( ;; ) {
        if ( c_accept_kw( KW_IS ) ) {
            switch ( c_tok()->kind ) {
            case TK_EQ: cc_code = CC_EQ; break;
            case TK_NE: cc_code = CC_NE; break;
            case TK_LT: cc_code = CC_LT; break;
            case TK_GT: cc_code = CC_GT; break;
            case TK_LE: cc_code = CC_LE; break;
            case TK_GE: cc_code = CC_GE; break;
            default:
                c_expect_msg( "relational operator" );
                cc_code = 0;
            }
            c_next();
            case_compare( blk, cc_code );
        } else {
            int save = cc->tp;
            u32 at = c_here();
            /* "a TO b" or a value: look for TO at this level */
            case_compare( blk, CC_EQ );
            if ( c_is_kw( KW_TO ) ) {
                cc->tp = save;
                cc->code_len = at;
                case_compare( blk, CC_GE );
                c_need_kw( KW_TO );
                {
                    u32 skip = c_jump( OP_JZ );
                    case_compare( blk, CC_LE );
                    if ( nbody >= 64 ) {
                        c_fail( "Program too complex" );
                    }
                    body_jumps[nbody++] = c_jump( OP_JNZ );
                    c_patch32( skip, c_here() );
                }
                if ( !c_accept( TK_COMMA ) ) {
                    break;
                }
                continue;
            }
        }
        if ( nbody >= 64 ) {
            c_fail( "Program too complex" );
        }
        body_jumps[nbody++] = c_jump( OP_JNZ );
        if ( !c_accept( TK_COMMA ) ) {
            break;
        }
    }
    blk->patch = c_jump( OP_JMP );              /* none matched: the next CASE */
    for ( i = 0; i < nbody; i++ ) {
        c_patch32( body_jumps[i], c_here() );
    }
}

static void st_end_select( void )
{
    BLOCK *blk = block_top( B_SELECT );

    if ( blk == NULL ) {
        if ( cc->checking ) {
            return;
        }
        c_fail_tok( "END SELECT without SELECT" );
    }
    stmt_unmark();
    cc->locs[blk->loc].end_pc = c_here();
    if ( blk->patch ) {
        c_patch32( blk->patch, c_here() );
    }
    block_close( blk );
}

/* ------------------------------------------------------------------ */
/* GOTO, GOSUB, ON, errors                                             */
/* ------------------------------------------------------------------ */

static void st_goto( int op )
{
    SYM *label = c_label_ref( 0 );

    if ( cc->immediate && op == OP_JMP ) {
        op = OP_IMMGOTO;                    /* the program goes on from the label */
    }
    c_op( op );
    c_emit_label( label );
}

static int event_kind( int kw )
{
    switch ( kw ) {
    case KW_TIMER:  return 1;
    case KW_KEY:    return 2;
    case KW_PLAY:   return 3;
    case KW_COM:    return 4;
    case KW_PEN:    return 5;
    case KW_STRIG:  return 6;
    }
    return 0;
}

static void st_on( void )
{
    TOKEN *tok = c_tok();
    SYM *labels[256];
    int count = 0, kind, gosub, i;

    if ( c_accept_kw( KW_ERROR ) ) {
        c_need_kw( KW_GOTO );
        if ( c_is( TK_NUM ) && c_tok()->num == 0 ) {
            c_next();
            c_op32( OP_ONERR, 0 );
            return;
        }
        {
            SYM *label = c_label_ref( 1 );
            c_op( OP_ONERR );
            c_emit_label( label );
        }
        return;
    }
    if ( tok->kind == TK_KW && (kind = event_kind( tok->kw )) != 0 ) {
        /* ON TIMER(n) GOSUB, ON KEY(n), ON PLAY(n), ON COM(n), ON STRIG(n),
           ON PEN: which, kind, the handler */
        c_next();
        if ( kind != 5 ) {
            c_need( TK_LPAREN, "(" );
            expr_long();
            c_need( TK_RPAREN, ")" );
        } else {
            c_push_l( 0 );
        }
        c_need_kw( KW_GOSUB );
        c_push_l( kind );
        if ( c_is( TK_NUM ) && c_tok()->num == 0 ) {
            c_next();
            c_push_l( 0 );
        } else {
            SYM *label = c_label_ref( 1 );
            c_emit( OP_CI );
            c_emit_label( label );
        }
        c_rt( RT_EVSET );
        cc->uses_events = 1;
        return;
    }
    expr_long();
    if ( c_accept_kw( KW_GOTO ) ) {
        gosub = 0;
        if ( cc->immediate ) {
            c_fail_tok( IMM_REFUSED );
        }
    } else if ( c_accept_kw( KW_GOSUB ) ) {
        gosub = 1;
    } else {
        c_expect_msg( "GOTO or GOSUB" );
        gosub = 0;
    }
    do {
        if ( count >= 255 ) {
            c_fail( "Program too complex" );
        }
        labels[count++] = c_label_ref( 0 );
    } while ( c_accept( TK_COMMA ) );
    c_op( OP_ONGO );
    c_emit16( (u32)count );
    c_emit( gosub );
    for ( i = 0; i < count; i++ ) {
        c_emit_label( labels[i] );
    }
}

static void st_resume( void )
{
    if ( c_at_end() ) {
        c_op( OP_RESUME );
    } else if ( c_accept_kw( KW_NEXT ) ) {
        c_op( OP_RESUMENEXT );
    } else if ( c_is( TK_NUM ) && c_tok()->num == 0 ) {
        c_next();
        c_op( OP_RESUME );
    } else {
        SYM *label = c_label_ref( 1 );
        c_op( OP_RESUMETO );
        c_emit_label( label );
    }
}

/* KEY(n) ON, TIMER OFF, PLAY STOP...: which, kind, state */
static void event_control( int kind )
{
    int state;

    if ( kind == 1 || kind == 5 || (kind == 3 && !c_is( TK_LPAREN )) ) {
        c_push_l( 0 );
    } else {
        c_need( TK_LPAREN, "(" );
        expr_long();
        c_need( TK_RPAREN, ")" );
    }
    if ( c_accept_kw( KW_ON ) ) {
        state = 1;
    } else if ( c_accept_kw( KW_OFF ) ) {
        state = 0;
    } else if ( c_accept_kw( KW_STOP ) ) {
        state = 2;
    } else {
        c_expect_msg( "ON, OFF, or STOP" );
        state = 0;
    }
    c_push_l( kind );
    c_push_l( state );
    c_rt( RT_EVCTL );
    cc->uses_events = 1;
}

/* ------------------------------------------------------------------ */
/* declarations                                                        */
/* ------------------------------------------------------------------ */

static int deftype_kw( int kw )
{
    switch ( kw ) {
    case KW_DEFINT: return TY_INT;
    case KW_DEFLNG: return TY_LONG;
    case KW_DEFSNG: return TY_SINGLE;
    case KW_DEFDBL: return TY_DOUBLE;
    case KW_DEFSTR: return TY_STR;
    }
    return 0;
}

/* DEFINT a-z, c...: into the deftype table given */
static void st_deftype( int type, u8 *table )
{
    TOKEN *tok;
    int from, to, i;

    do {
        tok = c_tok();
        if ( tok->kind != TK_ID || tok->len != 1 ) {
            c_expect_msg( "letter" );
        }
        from = ch_upper( cc->src[tok->col] ) - 'A';
        c_next();
        to = from;
        if ( c_accept( TK_MINUS ) ) {
            cc->toks[cc->tp - 1].flags |= TF_TIGHT;
            tok = c_tok();
            if ( tok->kind != TK_ID || tok->len != 1 ) {
                c_expect_msg( "letter" );
            }
            to = ch_upper( cc->src[tok->col] ) - 'A';
            c_next();
        }
        if ( from < 0 || to > 25 || from > to ) {
            c_fail_tok( "Syntax error" );
        }
        for ( i = from; i <= to; i++ ) {
            table[i] = (u8)type;
        }
    } while ( c_accept( TK_COMMA ) );
}

/* the bounds of one array: 1 if every one is a constant */
static int array_bounds( s32 *bounds, int *ndims )
{
    CELL val;
    QTYPE type;
    STR *sval;
    int count = 0, all_const = 1, save = cc->tp;
    double num;

    c_need( TK_LPAREN, "(" );
    for ( ;; ) {
        s32 lo = cc->option_base, hi;
        if ( count >= MAX_DIMS ) {
            c_fail( "Too many dimensions" );
        }
        if ( !const_expr( &val, &type, &sval ) || !IS_NUMT( type ) ) {
            all_const = 0;
            break;
        }
        num = TKIND( type ) == TY_SINGLE ? val.s : TKIND( type ) == TY_DOUBLE ? val.d : val.l;
        hi = (s32)fp_round( num );
        if ( c_accept_kw( KW_TO ) ) {
            lo = hi;
            if ( !const_expr( &val, &type, &sval ) || !IS_NUMT( type ) ) {
                all_const = 0;
                break;
            }
            num = TKIND( type ) == TY_SINGLE ? val.s : TKIND( type ) == TY_DOUBLE ? val.d : val.l;
            hi = (s32)fp_round( num );
        }
        bounds[count * 2] = lo;
        bounds[count * 2 + 1] = hi;
        count++;
        if ( !c_accept( TK_COMMA ) ) {
            break;
        }
    }
    if ( all_const ) {
        c_need( TK_RPAREN, ")" );
        *ndims = count;
        return 1;
    }
    /* not constants: count them again and leave the tokens to be read */
    cc->tp = save;
    c_next();
    count = 1;
    {
        int depth = 1, at = cc->tp;
        for ( ; cc->toks[at].kind != TK_EOL; at++ ) {
            if ( cc->toks[at].kind == TK_LPAREN ) {
                depth++;
            } else if ( cc->toks[at].kind == TK_RPAREN ) {
                if ( --depth == 0 ) {
                    break;
                }
            } else if ( cc->toks[at].kind == TK_COMMA && depth == 1 ) {
                count++;
            }
        }
    }
    if ( count > MAX_DIMS ) {
        c_fail( "Too many dimensions" );
    }
    cc->tp = save;
    *ndims = count;
    return 0;
}

/* the bounds pushed at run time, read again from the tokens */
static void emit_bounds( void )
{
    c_need( TK_LPAREN, "(" );
    for ( ;; ) {
        int save = cc->tp, depth = 0, has_to = 0, at;
        for ( at = save; cc->toks[at].kind != TK_EOL; at++ ) {
            int kind = cc->toks[at].kind;
            if ( kind == TK_LPAREN ) {
                depth++;
            } else if ( kind == TK_RPAREN ) {
                if ( depth-- == 0 ) {
                    break;
                }
            } else if ( kind == TK_COMMA && depth == 0 ) {
                break;
            } else if ( kind == TK_KW && cc->toks[at].kw == KW_TO && depth == 0 ) {
                has_to = 1;
                break;
            }
        }
        if ( has_to ) {
            expr_long();
            c_need_kw( KW_TO );
        } else {
            c_push_l( cc->option_base );
        }
        expr_long();
        if ( !c_accept( TK_COMMA ) ) {
            break;
        }
    }
    c_need( TK_RPAREN, ")" );
}

static void add_arrinit( SYM *sym, int ndims, s32 *bounds )
{
    ARRINIT *init;

    if ( cc->checking ) {
        return;
    }
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
    init->flags = AF_STATIC;
    init->bounds = (s32 *)c_pool( ndims * 2 * sizeof( s32 ) );
    mem_cpy( init->bounds, bounds, ndims * 2 * sizeof( s32 ) );
}

/* DIM, REDIM, and the declarations of COMMON, SHARED and STATIC */
enum { D_DIM, D_REDIM, D_COMMON, D_SHARED, D_STATIC };

/* COMMON's variable, in order, for CHAIN */
static void common_add( SYM *sym, int is_array )
{
    COMMONVAR *cv;

    if ( cc->ncommon == cc->common_cap ) {
        u32 cap = cc->common_cap ? cc->common_cap * 2 : 16;
        COMMONVAR *grown = (COMMONVAR *)try_realloc( cc->common, cap * sizeof( COMMONVAR ) );
        if ( grown == NULL ) {
            c_fail( "Out of memory" );
        }
        cc->common = grown;
        cc->common_cap = cap;
    }
    cv = &cc->common[cc->ncommon++];
    cv->offset = sym->offset;
    cv->type = sym->type;
    cv->is_array = (u8)is_array;
    cv->size = is_array ? 4 : type_size( sym->type );
}

static void st_dim( int how )
{
    int shared = 0, ndims, is_array, has_as, all_const, sfx, kind, whole, at_bounds, i;
    int named = 0;
    char upper[NAME_MAX + 1];
    s32 bounds[MAX_DIMS * 2];
    QTYPE type, vtype;
    TOKEN *tok;
    SYM *sym;

    if ( how == D_COMMON && c_accept( TK_SLASH ) ) {
        /* a named block: /name/ - kept apart from the blank COMMON, and
           not handed on by CHAIN (as in QBasic) */
        if ( c_is( TK_ID ) ) {
            c_next();
            named = 1;
        }
        c_need( TK_SLASH, "/" );
    }
    if ( how == D_DIM || how == D_REDIM || how == D_COMMON ) {
        shared = c_accept_kw( KW_SHARED );
        if ( shared && cc->procscope != NULL && !cc->checking ) {
            c_fail_tok( "Illegal in procedure" );
        }
    }
    if ( how == D_COMMON && c_accept( TK_SLASH ) ) {
        if ( c_is( TK_ID ) ) {
            c_next();
            named = 1;
        }
        c_need( TK_SLASH, "/" );
    }
    if ( how == D_SHARED && cc->procscope == NULL && !cc->checking ) {
        c_fail_tok( "Illegal outside of SUB/FUNCTION" );
    }
    for ( ;; ) {
        tok = c_tok();
        if ( tok->kind != TK_ID ) {
            c_expect_msg( "identifier" );
        }
        c_name_of( tok, upper );
        sfx = tok->ntype;
        c_next();
        is_array = c_is( TK_LPAREN );
        whole = 0;
        all_const = 1;
        ndims = 0;
        at_bounds = cc->tp;
        if ( is_array ) {
            if ( c_peek( 1 )->kind == TK_RPAREN ) {
                /* name(): an array named, with no bounds yet */
                c_next();
                c_next();
                whole = 1;
            } else {
                all_const = array_bounds( bounds, &ndims );
                if ( !all_const ) {
                    int depth = 0;          /* past the bounds; emit_bounds reads them */
                    for ( ;; ) {
                        if ( c_is( TK_LPAREN ) ) {
                            depth++;
                        } else if ( c_is( TK_RPAREN ) && --depth == 0 ) {
                            c_next();
                            break;
                        } else if ( c_is( TK_EOL ) ) {
                            c_expect_msg( ")" );
                        }
                        c_next();
                    }
                }
            }
        }
        has_as = 0;
        type = TY_NONE;
        if ( c_accept_kw( KW_AS ) ) {
            has_as = 1;
            type = parse_as_type( 0, 1 );
        }
        kind = is_array ? SK_ARRAY : SK_VAR;

        if ( how == D_SHARED ) {
            /* the module's variable, seen from this procedure */
            SCOPE *save = cc->procscope;
            SYM *link;
            QTYPE mtype = type;
            cc->procscope = NULL;
            cc->scope = &cc->module;
            sym = var_lookup( upper, sfx, kind, 0, &mtype, 0, 0 );
            if ( sym == NULL ) {
                mtype = type;
                sym = var_lookup( upper, sfx, kind, !has_as, &mtype, 0, has_as );
            } else if ( has_as && sym->type != type ) {
                cc->procscope = save;
                cc->scope = save;
                c_fail_tok( "Duplicate definition" );
            }
            cc->procscope = save;
            cc->scope = save;
            if ( save != NULL && !cc->checking ) {
                if ( sym_find( save, sym->name, sym->tkey, sym->kind ) != NULL ) {
                    c_fail_tok( "Duplicate definition" );
                }
                link = sym_add( save, sym->name, sym->tkey, sym->kind );
                link->type = sym->type;
                link->scope = SC_GLOBAL;
                link->offset = sym->offset;
                link->flags = sym->flags;
                link->ndims = sym->ndims;
            }
        } else {
            vtype = type;
            sym = NULL;
            if ( how == D_STATIC && cc->procscope != NULL ) {
                /* STATIC makes the procedure's own, even beside a shared one */
                sym = c_find_local( upper, sfx, kind );
                if ( sym == NULL ) {
                    int tkey = has_as ? 0 : (sfx ? sfx : type_default( cc->scope, upper ));
                    sym = sym_add( cc->procscope, upper, tkey, kind );
                    sym->type = has_as ? type : (QTYPE)tkey;
                    if ( has_as ) {
                        sym->flags |= SF_AS;
                    }
                    sym->scope = SC_STATIC;
                    sym->offset = scope_alloc( cc->procscope, kind == SK_ARRAY ? 4 : type_size( sym->type ), 1 );
                    if ( kind == SK_ARRAY || TKIND( sym->type ) == TY_STR ) {
                        scope_note_static( cc->procscope, sym->offset, kind == SK_ARRAY );
                    }
                }
            } else if ( has_as ) {
                sym = var_lookup( upper, sfx, kind, 0, &vtype, 0, 0 );
                if ( sym == NULL || !(sym->flags & SF_AS) || how != D_REDIM ) {
                    vtype = type;
                    sym = var_lookup( upper, sfx, kind, 1, &vtype, 0, 1 );
                } else if ( sym->type != type ) {
                    c_fail_tok( "Duplicate definition" );
                }
            } else {
                sym = var_lookup( upper, sfx, kind, 1, &vtype, 0, 0 );
            }
            if ( shared ) {
                sym->flags |= SF_SHARED;
            }
            if ( how == D_COMMON && !named && sym->scope == SC_GLOBAL && !cc->checking ) {
                common_add( sym, is_array );
            }
            if ( is_array && whole ) {
                sym->flags |= SF_DYNAMIC;
            } else if ( is_array ) {
                int dynamic = how == D_REDIM || cc->dynamic || !all_const
                              || sym->scope == SC_FRAME || sym->scope == SC_REF
                              || (sym->flags & SF_DYNAMIC);
                int flags;
                if ( sym->flags & SF_IMPLICIT ) {
                    c_fail_at( at_bounds - 1, "Array already dimensioned" );
                }
                if ( sym->ndims && sym->ndims != ndims && how != D_REDIM ) {
                    c_fail_at( at_bounds - 1, "Wrong number of dimensions" );
                }
                if ( how == D_REDIM && (sym->flags & SF_DIMMED) && !(sym->flags & SF_DYNAMIC) ) {
                    c_fail_at( at_bounds - 1, "Array already dimensioned" );
                }
                if ( !dynamic && (sym->flags & SF_DIMMED) ) {
                    c_fail_at( at_bounds - 1, "Array already dimensioned" );
                }
                sym->ndims = (u8)ndims;
                sym->flags |= SF_DIMMED;
                if ( !dynamic && sym->scope == SC_GLOBAL ) {
                    for ( i = 0; i < ndims; i++ ) {
                        if ( bounds[i * 2 + 1] < bounds[i * 2] ) {
                            c_fail_at( at_bounds, "Subscript out of range" );
                        }
                    }
                    add_arrinit( sym, ndims, bounds );     /* made before the first statement */
                } else {
                    /* made when the DIM is reached: a static once, a procedure's
                       at each call, a dynamic one each time it is asked for */
                    flags = 0;
                    if ( !dynamic ) {
                        flags |= 1;             /* static: a second DIM does nothing */
                    } else {
                        sym->flags |= SF_DYNAMIC;
                    }
                    if ( how == D_REDIM ) {
                        flags |= 2;             /* what is there goes first */
                    }
                    /* the slot, the bounds, then type, width, flags and
                       the count on top, where RT_DIM finds it first */
                    c_sym_addr( sym );
                    if ( all_const ) {
                        for ( i = 0; i < ndims * 2; i++ ) {
                            c_push_l( bounds[i] );
                        }
                    } else {
                        int save = cc->tp;
                        cc->tp = at_bounds;
                        emit_bounds();
                        cc->tp = save;
                    }
                    c_push_l( (s32)sym->type );
                    c_push_l( (s32)type_size( sym->type ) );
                    c_push_l( flags );
                    c_push_l( ndims );
                    c_rt( RT_DIM );
                }
            }
        }
        if ( !c_accept( TK_COMMA ) ) {
            break;
        }
    }
}

/* CONST name = value, ... */
static void st_const( int define )
{
    TOKEN *tok;
    char upper[NAME_MAX + 1];
    CELL val;
    QTYPE type;
    STR *sval;
    SYM *sym;
    SCOPE *scope = cc->procscope ? cc->procscope : &cc->module;
    int sfx;

    do {
        tok = c_tok();
        if ( tok->kind != TK_ID ) {
            c_expect_msg( "identifier" );
        }
        c_name_of( tok, upper );
        sfx = tok->ntype;
        c_next();
        c_need( TK_EQ, "=" );
        if ( !const_expr( &val, &type, &sval ) ) {
            c_fail_tok( "Invalid constant" );
        }
        if ( cc->checking && cc->pass != 0 ) {
            qs_release( sval );
            continue;
        }
        if ( sym_find( scope, upper, 0, SK_CONST ) != NULL ) {
            if ( !define || cc->checking ) {
                qs_release( sval );             /* pass 0 made it already */
                continue;
            }
            c_fail_tok( "Duplicate definition" );
        }
        if ( sfx ) {
            if ( (sfx == TY_STR) != (TKIND( type ) == TY_STR) ) {
                c_fail_tok( "Type mismatch" );
            }
            if ( sfx != TY_STR && sfx != TKIND( type ) ) {
                double num = TKIND( type ) == TY_SINGLE ? val.s : TKIND( type ) == TY_DOUBLE ? val.d : val.l;
                switch ( sfx ) {
                case TY_INT:
                case TY_LONG:
                    num = fp_round( num );
                    if ( (sfx == TY_INT && (num < -32768.0 || num > 32767.0))
                      || num < -2147483648.0 || num > 2147483647.0 ) {
                        c_fail_tok( "Overflow" );
                    }
                    val.l = (s32)num;
                    break;
                case TY_SINGLE:
                    if ( num > SNG_MAX || num < -SNG_MAX ) {
                        c_fail_tok( "Overflow" );
                    }
                    val.s = (float)num;
                    break;
                default:
                    val.d = num;
                    break;
                }
                type = sfx;
            }
        }
        sym = sym_add( scope, upper, 0, SK_CONST );
        sym->type = type;
        sym->cval = val;
        if ( TKIND( type ) == TY_STR ) {
            sym->cstr = c_const_str( QS_TEXT( sval ), QS_LEN( sval ) );
            qs_release( sval );
        }
    } while ( c_accept( TK_COMMA ) );
}

/* a parameter list, (a, b$, c() AS INTEGER...), into a procedure; with
   "scope" the parameters become its variables */
static void param_list( PROC *proc, SCOPE *scope, u8 *deftype, int is_declare )
{
    PARAM params[64];
    int count = 0;
    char upper[NAME_MAX + 1];
    TOKEN *tok;
    QTYPE type;
    int is_array, sfx, has_as;

    if ( c_accept( TK_LPAREN ) && !c_accept( TK_RPAREN ) ) {
        for ( ;; ) {
            if ( count >= 64 ) {
                c_fail( "Too many arguments" );
            }
            tok = c_tok();
            if ( tok->kind != TK_ID ) {
                c_expect_msg( "identifier" );
            }
            c_name_of( tok, upper );
            sfx = tok->ntype;
            c_next();
            is_array = 0;
            if ( c_accept( TK_LPAREN ) ) {
                c_need( TK_RPAREN, ")" );
                is_array = 1;
            }
            has_as = 0;
            if ( c_accept_kw( KW_AS ) ) {
                type = parse_as_type( is_declare, 0 );
                if ( sfx && TKIND( type ) != sfx ) {
                    c_fail_tok( "Duplicate definition" );
                }
                has_as = !sfx;
            } else {
                type = sfx ? (QTYPE)sfx : (QTYPE)deftype[upper[0] - 'A'];
            }
            params[count].type = type;
            params[count].array = (u8)is_array;
            params[count].ndims = 0;
            if ( scope != NULL ) {
                /* the parameter is a variable of the procedure: a name AS a
                   type is keyed as an AS name, any other by its type */
                SYM *sym;
                int kind = is_array ? SK_ARRAY : SK_VAR;
                int tkey = has_as ? 0 : TKIND( type );
                if ( sym_find( scope, upper, tkey, kind ) != NULL ) {
                    c_fail_tok( "Duplicate definition" );
                }
                sym = sym_add( scope, upper, tkey, kind );
                sym->type = type;
                sym->scope = SC_REF;
                sym->flags |= SF_PARAM;
                if ( has_as ) {
                    sym->flags |= SF_AS;
                }
                sym->offset = (u32)count * 4;
            }
            count++;
            if ( !c_accept( TK_COMMA ) ) {
                break;
            }
        }
        c_need( TK_RPAREN, ")" );
    }
    proc->nparams = count;
    proc->params = (PARAM *)c_pool( (count ? count : 1) * sizeof( PARAM ) );
    mem_cpy( proc->params, params, count * sizeof( PARAM ) );
}

/* a new procedure in the table, and its name in the module's scope */
static int proc_new( const char *upper, int kind )
{
    SYM *sym;
    PROC *proc;

    sym = sym_find( &cc->module, upper, 0, SK_PROC );
    if ( sym != NULL ) {
        return sym->proc;
    }
    if ( cc->nprocs == cc->procs_cap ) {
        int cap = cc->procs_cap ? cc->procs_cap * 2 : 32;
        PROC *grown;
        SYM **gsym;
        /* each block kept as soon as it has moved: state_free frees them */
        grown = (PROC *)try_realloc( cc->procs, cap * sizeof( PROC ) );
        if ( grown == NULL ) {
            c_fail( "Out of memory" );
        }
        cc->procs = grown;
        gsym = (SYM **)try_realloc( cc->procsym, cap * sizeof( SYM * ) );
        if ( gsym == NULL ) {
            c_fail( "Out of memory" );
        }
        cc->procsym = gsym;
        cc->procs_cap = cap;
    }
    proc = &cc->procs[cc->nprocs];
    mem_set( proc, 0, sizeof( *proc ) );
    proc->kind = (u8)kind;
    proc->text = -1;
    sym = sym_add( &cc->module, upper, 0, SK_PROC );
    sym->proc = cc->nprocs;
    cc->procsym[cc->nprocs] = sym;
    return cc->nprocs++;
}

/* the heading of a SUB, FUNCTION or DECLARE: -> the procedure */
static int proc_heading( int kind, SCOPE *scope, u8 *deftype, int is_declare )
{
    TOKEN *tok = c_tok();
    char upper[NAME_MAX + 1];
    int index, sfx, i;
    PROC *proc, tmp;
    u32 len;

    if ( tok->kind != TK_ID ) {
        c_expect_msg( "identifier" );
    }
    c_name_of( tok, upper );
    sfx = tok->ntype;
    if ( kind == PK_SUB && sfx ) {
        c_fail( "Syntax error" );
    }
    c_next();
    mem_set( &tmp, 0, sizeof( tmp ) );
    tmp.kind = (u8)kind;
    tmp.rettype = sfx ? (QTYPE)sfx : (QTYPE)deftype[upper[0] - 'A'];
    param_list( &tmp, scope, deftype, is_declare );
    if ( c_accept_kw( KW_STATIC ) ) {
        if ( is_declare ) {
            c_fail_tok( "Syntax error" );
        }
        tmp.is_static = 1;
    }
    if ( cc->checking ) {
        return -1;
    }
    index = proc_new( upper, kind );
    proc = &cc->procs[index];
    if ( proc->kind != kind ) {
        c_fail_at( 0, "Duplicate definition" );
    }
    if ( is_declare ) {
        if ( proc->defined || proc->declared ) {
            /* the DECLARE must agree with the procedure */
            if ( proc->nparams != tmp.nparams ) {
                c_fail_at( 1, "Argument-count mismatch" );
            }
            for ( i = 0; i < tmp.nparams; i++ ) {
                if ( TKIND( tmp.params[i].type ) != TY_ANY
                  && (tmp.params[i].type != proc->params[i].type || tmp.params[i].array != proc->params[i].array) ) {
                    c_fail_at( 1, "Parameter type mismatch" );
                }
            }
            if ( kind == PK_FUNC && proc->rettype != tmp.rettype ) {
                c_fail_at( 1, "Duplicate definition" );
            }
            return index;
        }
        proc->declared = 1;
    } else {
        if ( proc->defined ) {
            c_fail_at( 1, "Duplicate definition" );
        }
        proc->defined = 1;
        proc->is_static = tmp.is_static;
    }
    proc->nparams = tmp.nparams;
    proc->params = tmp.params;
    proc->rettype = tmp.rettype;
    len = str_len( upper );
    mem_cpy( proc->name, cc->src + tok->col, len );
    proc->name[len] = 0;
    if ( sfx ) {
        proc->name[len] = cc->src[tok->col + len];
        proc->name[len + 1] = 0;
    }
    return index;
}

/* ------------------------------------------------------------------ */
/* DEF FN                                                              */
/* ------------------------------------------------------------------ */

static u32 deffn_over;

static void st_def_fn( void )
{
    TOKEN *tok = c_tok();
    char upper[NAME_MAX + 1];
    SYM *sym;
    PROC *proc;
    SCOPE *scope;
    BLOCK *blk;
    QTYPE type;
    int index, i;

    if ( c_accept_kw( KW_SEG ) ) {
        /* DEF SEG [= segment] */
        if ( c_accept( TK_EQ ) ) {
            c_push_l( 1 );
            expr_long();
        } else {
            c_push_l( 0 );
            c_push_l( 0 );
        }
        c_rt( RT_DEFSEG );
        return;
    }
    if ( tok->kind != TK_ID || !(tok->flags & TF_FN) ) {
        c_expect_msg( "FN or SEG" );
    }
    if ( cc->procscope != NULL || cc->in_deffn >= 0 ) {
        if ( !cc->checking ) {
            c_fail( "Illegal in procedure" );
        }
    }
    c_name_of( tok, upper );
    if ( cc->checking ) {
        PROC tmp;
        c_next();
        param_list( &tmp, NULL, cc->scope->deftype, 0 );
        if ( c_accept( TK_EQ ) ) {
            expr();
        }
        return;
    }
    sym = sym_find( &cc->module, upper, 0, SK_PROC );
    if ( sym == NULL ) {
        c_fail( "Internal error" );
    }
    index = sym->proc;
    proc = &cc->procs[index];
    c_next();
    /* the parameters are the DEF FN's own; anything else is the module's */
    scope = scope_new( index );
    scope->is_deffn = 1;
    for ( i = 0; i < 26; i++ ) {
        scope->deftype[i] = cc->module.deftype[i];
    }
    {
        PROC tmp;
        param_list( &tmp, scope, cc->module.deftype, 0 );
    }
    scope->size = (u32)proc->nparams * 4;
    proc->ret_off = scope_alloc( scope, type_size( proc->rettype ), 0 );
    if ( TKIND( proc->rettype ) == TY_STR ) {
        scope_note_str( scope, proc->ret_off );
    }
    stmt_unmark();
    deffn_over = c_jump( OP_JMP );
    proc->entry = c_here();
    proc->defined = 1;
    cc->in_deffn = index;
    cc->deffn_scope = scope;
    if ( c_accept( TK_EQ ) ) {
        /* one line: the value is the expression, and the DEF itself is
           the statement a step stops on inside it */
        VREF ref;
        STMTLOC *loc;
        mem_set( &ref, 0, sizeof( ref ) );
        stmt_mark();
        loc = &cc->locs[cc->nlocs - 1];
        loc->col = cc->toks[0].col;
        type = expr();
        ref.simple = 1;
        ref.scope = SC_FRAME;
        ref.offset = proc->ret_off;
        ref.type = proc->rettype;
        c_store( &ref, type );
        c_op( OP_FRET );
        cc->locs[cc->nlocs - 1].end_pc = c_here();
        proc->frame_size = scope->size;
        proc->rel_str = scope->rel_str;
        proc->nrel_str = scope->nrel_str;
        cc->in_deffn = -1;
        cc->deffn_scope = NULL;
        c_patch32( deffn_over, c_here() );
        return;
    }
    blk = block_open( B_DEFFN );
    blk->patch = deffn_over;
    blk->top = (u32)index;
}

static void st_end_def( void )
{
    BLOCK *blk = block_top( B_DEFFN );
    PROC *proc;

    if ( blk == NULL ) {
        if ( cc->checking ) {
            return;
        }
        c_fail_tok( "END DEF without DEF" );
    }
    proc = &cc->procs[blk->top];
    c_op( OP_FRET );
    proc->frame_size = cc->deffn_scope->size;
    proc->rel_str = cc->deffn_scope->rel_str;
    proc->nrel_str = cc->deffn_scope->nrel_str;
    c_patch32( blk->patch, c_here() );
    block_close( blk );
    cc->in_deffn = -1;
    cc->deffn_scope = NULL;
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

static void st_open( void )
{
    int mode = 4, access = 0, lock = 0;

    expr_str();
    if ( c_accept( TK_COMMA ) ) {
        /* OPEN mode$, #n, file$ [, length] */
        file_num();
        c_need( TK_COMMA, "," );
        expr_str();
        if ( c_accept( TK_COMMA ) ) {
            expr_long();
        } else {
            c_push_l( -1 );
        }
        c_rt( RT_OPENOLD );
        return;
    }
    if ( c_accept_kw( KW_FOR ) ) {
        if ( c_accept_kw( KW_INPUT ) ) {
            mode = 1;
        } else if ( c_accept_kw( KW_OUTPUT ) ) {
            mode = 2;
        } else if ( c_accept_kw( KW_APPEND ) ) {
            mode = 3;
        } else if ( c_accept_kw( KW_RANDOM ) ) {
            mode = 4;
        } else if ( c_accept_kw( KW_BINARY ) ) {
            mode = 5;
        } else {
            c_expect_msg( "INPUT, OUTPUT, APPEND, RANDOM, or BINARY" );
        }
    }
    if ( c_accept_kw( KW_ACCESS ) ) {
        if ( c_accept_kw( KW_READ ) ) {
            access = 1;
            if ( c_accept_kw( KW_WRITE ) ) {
                access = 3;
            }
        } else if ( c_accept_kw( KW_WRITE ) ) {
            access = 2;
        } else {
            c_expect_msg( "READ or WRITE" );
        }
    }
    if ( c_accept_kw( KW_SHARED ) ) {
        lock = 1;
    } else if ( c_accept_kw( KW_LOCK ) ) {
        if ( c_accept_kw( KW_READ ) ) {
            lock = 2;
            if ( c_accept_kw( KW_WRITE ) ) {
                lock = 4;
            }
        } else if ( c_accept_kw( KW_WRITE ) ) {
            lock = 3;
        } else {
            c_expect_msg( "READ or WRITE" );
        }
    }
    c_need_kw( KW_AS );
    c_push_l( mode );
    c_push_l( access );
    c_push_l( lock );
    file_num();
    if ( c_accept_kw( KW_LEN ) ) {
        c_need( TK_EQ, "=" );
        expr_long();
    } else {
        c_push_l( -1 );
    }
    c_rt( RT_OPEN );
}

static void st_close( void )
{
    if ( c_at_end() ) {
        c_rt( RT_CLOSEALL );
        return;
    }
    do {
        file_num();
        c_rt( RT_CLOSE );
    } while ( c_accept( TK_COMMA ) );
}

/* GET and PUT: a file's, or the graphics screen's */
static void st_get_put( int is_put )
{
    VREF ref;
    QTYPE type;

    if ( c_is( TK_LPAREN ) || c_is_kw( KW_STEP ) ) {
        int flags;
        if ( is_put ) {
            flags = coord();
            c_need( TK_COMMA, "," );
            array_and_element();
            if ( c_accept( TK_COMMA ) ) {
                int action;
                if ( c_accept_kw( KW_PSET ) ) {
                    action = 0;
                } else if ( c_accept_kw( KW_PRESET ) ) {
                    action = 1;
                } else if ( c_accept_kw( KW_AND ) ) {
                    action = 2;
                } else if ( c_accept_kw( KW_OR ) ) {
                    action = 3;
                } else if ( c_accept_kw( KW_XOR ) ) {
                    action = 4;
                } else {
                    c_expect_msg( "PSET, PRESET, AND, OR, or XOR" );
                    action = 4;
                }
                c_push_l( action );
            } else {
                c_push_l( 4 );
            }
            c_push_l( flags );
            c_rt( RT_GPUT );
            return;
        }
        flags = coord();
        c_need( TK_MINUS, "-" );
        cc->toks[cc->tp - 1].flags |= TF_TIGHT;
        flags |= coord() << 1;
        c_need( TK_COMMA, "," );
        array_and_element();
        c_push_l( flags );
        c_rt( RT_GGET );
        return;
    }
    file_num();
    if ( c_accept( TK_COMMA ) ) {
        if ( c_is( TK_COMMA ) ) {
            c_push_l( NO_RECORD );
        } else {
            expr_long();
        }
        if ( c_accept( TK_COMMA ) ) {
            type = store_target( &ref );
            if ( TKIND( type ) == TY_STR ) {
                c_push_l( 0 );
            } else {
                c_push_l( (s32)type_size( type ) );
            }
            c_push_l( TKIND( type ) );
        } else {
            c_push_l( 0 );
            c_push_l( 0 );
            c_push_l( 0 );
        }
    } else {
        c_push_l( NO_RECORD );
        c_push_l( 0 );
        c_push_l( 0 );
        c_push_l( 0 );
    }
    c_rt( is_put ? RT_FPUT : RT_FGET );
}

static void st_field( void )
{
    VREF ref;
    int count = 0;
    QTYPE type;

    file_num();
    while ( c_accept( TK_COMMA ) ) {
        expr_long();
        c_need_kw( KW_AS );
        type = store_target( &ref );
        if ( TKIND( type ) != TY_STR ) {
            c_fail_tok( "Type mismatch" );
        }
        count++;
    }
    c_push_l( count );
    c_rt( RT_FIELD );
}

static void st_lock( int unlock )
{
    file_num();
    if ( c_accept( TK_COMMA ) ) {
        if ( c_accept_kw( KW_TO ) ) {
            c_push_l( 1 );
            expr_long();
        } else {
            expr_long();
            if ( c_accept_kw( KW_TO ) ) {
                expr_long();
            } else {
                c_op( OP_DUP );
            }
        }
    } else {
        c_push_l( 0 );
        c_push_l( 0 );
    }
    c_push_l( unlock );
    c_rt( RT_LOCK );
}

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */

static void st_screen( void )
{
    int mask = 0;

    opt_long( &mask, 1 );
    if ( c_accept( TK_COMMA ) ) {
        opt_long( &mask, 2 );
        if ( c_accept( TK_COMMA ) ) {
            opt_long( &mask, 4 );
            if ( c_accept( TK_COMMA ) ) {
                opt_long( &mask, 8 );
            } else {
                c_push_l( 0 );
            }
        } else {
            c_push_l( 0 );
            c_push_l( 0 );
        }
    } else {
        c_push_l( 0 );
        c_push_l( 0 );
        c_push_l( 0 );
    }
    c_push_l( mask );
    c_rt( RT_SCREENS );
}

/* up to "count" numbers, any of them left out: then the mask */
static void opt_list( int count, int routine )
{
    int mask = 0, i;

    for ( i = 0; i < count; i++ ) {
        opt_long( &mask, 1 << i );
        if ( !c_accept( TK_COMMA ) ) {
            for ( i++; i < count; i++ ) {
                c_push_l( 0 );
            }
            break;
        }
    }
    c_push_l( mask );
    c_rt( routine );
}

static void st_width( void )
{
    if ( c_accept( TK_HASH ) ) {
        expr_long();
        c_need( TK_COMMA, "," );
        expr_long();
        c_rt( RT_WIDTHF );
        return;
    }
    if ( c_accept_kw( KW_LPRINT ) ) {
        c_op32( OP_CSTR, c_const_str( "LPT1:", 5 ) );
        expr_long();
        c_rt( RT_WIDTHDEV );
        return;
    }
    /* WIDTH device$, n: any string expression names the device */
    if ( !c_at_end() && !c_is( TK_COMMA ) && TKIND( c_peek_type() ) == TY_STR ) {
        expr_str();
        c_need( TK_COMMA, "," );
        expr_long();
        c_rt( RT_WIDTHDEV );
        return;
    }
    opt_list( 2, RT_WIDTH );
}

static void st_view( void )
{
    int flags = 0;

    if ( c_accept_kw( KW_PRINT ) ) {
        if ( c_at_end() ) {
            c_push_l( 0 );
            c_push_l( 0 );
            c_push_l( 0 );
        } else {
            c_push_l( 1 );
            expr_long();
            c_need_kw( KW_TO );
            expr_long();
        }
        c_rt( RT_VIEWPRINT );
        return;
    }
    if ( c_at_end() ) {
        c_op( OP_CD ); c_emitd( 0 );
        c_op( OP_CD ); c_emitd( 0 );
        c_op( OP_CD ); c_emitd( 0 );
        c_op( OP_CD ); c_emitd( 0 );
        c_push_l( 0 );
        c_push_l( 0 );
        c_push_l( 0 );
        c_rt( RT_VIEW );
        return;
    }
    flags = 1;
    if ( c_accept_kw( KW_SCREEN ) ) {
        flags |= 2;
    }
    if ( coord() ) {
        c_fail_tok( "Syntax error" );
    }
    c_need( TK_MINUS, "-" );
    cc->toks[cc->tp - 1].flags |= TF_TIGHT;
    if ( coord() ) {
        c_fail_tok( "Syntax error" );
    }
    if ( c_accept( TK_COMMA ) ) {
        if ( !c_is( TK_COMMA ) && !c_at_end() ) {
            expr_long();
            flags |= 4;
        } else {
            c_push_l( 0 );
        }
        if ( c_accept( TK_COMMA ) ) {
            expr_long();
            flags |= 8;
        } else {
            c_push_l( 0 );
        }
    } else {
        c_push_l( 0 );
        c_push_l( 0 );
    }
    c_push_l( flags );
    c_rt( RT_VIEW );
}

static void st_window( void )
{
    int flags = 0;

    if ( c_at_end() ) {
        c_op( OP_CD ); c_emitd( 0 );
        c_op( OP_CD ); c_emitd( 0 );
        c_op( OP_CD ); c_emitd( 0 );
        c_op( OP_CD ); c_emitd( 0 );
        c_push_l( 0 );
        c_rt( RT_WINDOW );
        return;
    }
    flags = 1;
    if ( c_accept_kw( KW_SCREEN ) ) {
        flags |= 2;
    }
    if ( coord() ) {
        c_fail_tok( "Syntax error" );
    }
    c_need( TK_MINUS, "-" );
    cc->toks[cc->tp - 1].flags |= TF_TIGHT;
    if ( coord() ) {
        c_fail_tok( "Syntax error" );
    }
    c_push_l( flags );
    c_rt( RT_WINDOW );
}

static void st_pset( int preset )
{
    int flags = coord();

    if ( preset ) {
        flags |= 4;
    }
    if ( c_accept( TK_COMMA ) ) {
        expr_long();
        flags |= 2;
    } else {
        c_push_l( 0 );
    }
    c_push_l( flags );
    c_rt( RT_PSET );
}

static void st_line( void )
{
    int flags = 0;

    if ( c_is( TK_LPAREN ) || c_is_kw( KW_STEP ) ) {
        flags |= coord();
        flags |= 4;
    } else {
        c_op( OP_CD ); c_emitd( 0 );
        c_op( OP_CD ); c_emitd( 0 );
    }
    c_need( TK_MINUS, "-" );
    cc->toks[cc->tp - 1].flags |= TF_TIGHT;
    if ( coord() ) {
        flags |= 2;
    }
    if ( c_accept( TK_COMMA ) ) {
        if ( !c_is( TK_COMMA ) && !c_at_end() ) {
            expr_long();
            flags |= 8;
        } else {
            c_push_l( 0 );
        }
        if ( c_accept( TK_COMMA ) ) {
            TOKEN *tok = c_tok();
            if ( tok->kind == TK_ID && tok->len == 1 && ch_upper( cc->src[tok->col] ) == 'B' ) {
                flags |= 16;
                c_next();
            } else if ( tok->kind == TK_ID && tok->len == 2 && ch_upper( cc->src[tok->col] ) == 'B'
                     && ch_upper( cc->src[tok->col + 1] ) == 'F' ) {
                flags |= 32;
                c_next();
            } else if ( !c_is( TK_COMMA ) && !c_at_end() ) {
                c_expect_msg( "B or BF" );
            }
            if ( c_accept( TK_COMMA ) ) {
                expr_long();
                flags |= 64;
            } else {
                c_push_l( 0 );
            }
        } else {
            c_push_l( 0 );
        }
    } else {
        c_push_l( 0 );
        c_push_l( 0 );
    }
    c_push_l( flags );
    c_rt( RT_LINE );
}

static void opt_dbl( int *flags, int bit )
{
    if ( c_is( TK_COMMA ) || c_at_end() ) {
        c_op( OP_CD );
        c_emitd( 0 );
    } else {
        expr_dbl();
        *flags |= bit;
    }
}

static void st_circle( void )
{
    int flags = coord();

    c_need( TK_COMMA, "," );
    expr_dbl();
    if ( c_accept( TK_COMMA ) ) {
        opt_long( &flags, 2 );
        if ( c_accept( TK_COMMA ) ) {
            opt_dbl( &flags, 4 );
            if ( c_accept( TK_COMMA ) ) {
                opt_dbl( &flags, 8 );
                if ( c_accept( TK_COMMA ) ) {
                    opt_dbl( &flags, 16 );
                } else {
                    c_op( OP_CD ); c_emitd( 0 );
                }
            } else {
                c_op( OP_CD ); c_emitd( 0 );
                c_op( OP_CD ); c_emitd( 0 );
            }
        } else {
            c_op( OP_CD ); c_emitd( 0 );
            c_op( OP_CD ); c_emitd( 0 );
            c_op( OP_CD ); c_emitd( 0 );
        }
    } else {
        c_push_l( 0 );
        c_op( OP_CD ); c_emitd( 0 );
        c_op( OP_CD ); c_emitd( 0 );
        c_op( OP_CD ); c_emitd( 0 );
    }
    c_push_l( flags );
    c_rt( RT_CIRCLE );
}

/* PAINT (x, y) [, paint [, border [, background$]]]: the paint is a
   colour or a tile string - x y colour tile border background flags */
static void st_paint( void )
{
    int flags = coord();

    if ( c_accept( TK_COMMA ) ) {
        if ( !c_is( TK_COMMA ) && !c_at_end() ) {
            if ( TKIND( c_peek_type() ) == TY_STR ) {
                c_push_l( 0 );
                expr_str();
                flags |= 2 | 4;
            } else {
                expr_long();
                c_op32( OP_CSTR, c_const_str( "", 0 ) );
                flags |= 2;
            }
        } else {
            c_push_l( 0 );
            c_op32( OP_CSTR, c_const_str( "", 0 ) );
        }
        if ( c_accept( TK_COMMA ) ) {
            opt_long( &flags, 8 );
            if ( c_accept( TK_COMMA ) ) {
                expr_str();
                flags |= 16;
            } else {
                c_op32( OP_CSTR, c_const_str( "", 0 ) );
            }
        } else {
            c_push_l( 0 );
            c_op32( OP_CSTR, c_const_str( "", 0 ) );
        }
    } else {
        c_push_l( 0 );
        c_op32( OP_CSTR, c_const_str( "", 0 ) );
        c_push_l( 0 );
        c_op32( OP_CSTR, c_const_str( "", 0 ) );
    }
    c_push_l( flags );
    c_rt( RT_PAINT );
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static void st_end( void )
{
    if ( c_accept_kw( KW_IF ) ) {
        st_end_if();
    } else if ( c_accept_kw( KW_SELECT ) ) {
        st_end_select();
    } else if ( c_accept_kw( KW_SUB ) ) {
        if ( !cc->checking ) {
            if ( cc->cur_proc < 0 || cc->procs[cc->cur_proc].kind != PK_SUB ) {
                c_fail_tok( "END SUB without SUB" );
            }
            if ( cc->nblocks > 0 && cc->blocks[cc->nblocks - 1].kind != B_PROC ) {
                block_unclosed( &cc->blocks[cc->nblocks - 1] );
            }
        }
        c_op( OP_RET );
        if ( cc->nblocks > 0 && cc->blocks[cc->nblocks - 1].kind == B_PROC ) {
            cc->nblocks--;
        }
    } else if ( c_accept_kw( KW_FUNCTION ) ) {
        if ( !cc->checking ) {
            if ( cc->cur_proc < 0 || cc->procs[cc->cur_proc].kind != PK_FUNC ) {
                c_fail_tok( "END FUNCTION without FUNCTION" );
            }
            if ( cc->nblocks > 0 && cc->blocks[cc->nblocks - 1].kind != B_PROC ) {
                block_unclosed( &cc->blocks[cc->nblocks - 1] );
            }
        }
        c_op( OP_FRET );
        if ( cc->nblocks > 0 && cc->blocks[cc->nblocks - 1].kind == B_PROC ) {
            cc->nblocks--;
        }
    } else if ( c_accept_kw( KW_DEF ) ) {
        st_end_def();
    } else if ( c_accept_kw( KW_TYPE ) ) {
        if ( !cc->checking ) {
            c_fail_tok( "END TYPE without TYPE" );
        }
    } else {
        c_op( OP_END );
    }
}

static void st_mid( void )
{
    VREF ref;
    QTYPE type;

    c_need( TK_LPAREN, "(" );
    type = store_target( &ref );
    if ( !IS_STRT( type ) ) {
        c_fail_tok( "Type mismatch" );
    }
    if ( TKIND( type ) == TY_FSTR ) {
        c_push_l( (s32)TARG( type ) );
    }
    c_need( TK_COMMA, "," );
    expr_long();
    if ( c_accept( TK_COMMA ) ) {
        expr_long();
    } else {
        c_push_l( -1 );
    }
    c_need( TK_RPAREN, ")" );
    c_need( TK_EQ, "=" );
    expr_str();
    c_rt( TKIND( type ) == TY_FSTR ? RT_MIDSETF : RT_MIDSETV );
}

static void st_lset( int rset )
{
    VREF ref, src;
    QTYPE type;

    type = store_target( &ref );
    c_need( TK_EQ, "=" );
    if ( TKIND( type ) == TY_REC ) {
        if ( rset ) {
            c_fail_tok( "Type mismatch" );
        }
        c_push_l( (s32)type_size( type ) );
        if ( !c_var( &src, 0, 0 ) || TKIND( src.type ) != TY_REC ) {
            c_fail( "Type mismatch" );
        }
        c_addr( &src );
        c_push_l( (s32)type_size( src.type ) );
        c_rt( RT_LSETREC );
        return;
    }
    if ( TKIND( type ) == TY_FSTR ) {
        c_push_l( (s32)TARG( type ) );
        expr_str();
        c_rt( rset ? RT_RSETF : RT_LSETF );
    } else if ( TKIND( type ) == TY_STR ) {
        expr_str();
        c_rt( rset ? RT_RSETV : RT_LSETV );
    } else {
        c_fail_tok( "Type mismatch" );
    }
}

static void st_swap( void )
{
    VREF lhs, rhs;

    if ( !c_var( &lhs, 1, 0 ) ) {
        c_fail( "Variable required" );
    }
    c_addr( &lhs );
    c_need( TK_COMMA, "," );
    if ( !c_var( &rhs, 1, 0 ) ) {
        c_fail( "Variable required" );
    }
    c_addr( &rhs );
    if ( lhs.type != rhs.type ) {
        c_fail_tok( "Type mismatch" );
    }
    c_op32( OP_SWAP, type_size( lhs.type ) );
}

static void st_read( void )
{
    VREF ref;

    do {
        item_store( store_target( &ref ), RT_READI, RT_READL, RT_READS, RT_READD, RT_READSTR, RT_READFS );
    } while ( c_accept( TK_COMMA ) );
}

static void st_restore( void )
{
    if ( c_at_end() ) {
        c_push_l( 0 );
    } else {
        SYM *label = c_label_ref( 1 );
        emit_data_label( label );
    }
    c_rt( RT_RESTORE );
}

/* DATA's items, kept for READ: split at commas outside quotes */
static void st_data( void )
{
    TOKEN *tok = c_tok();
    const char *text;
    u32 len, pos = 0, start, end;
    int quoted;
    DATAITEM *item;

    if ( tok->kind != TK_DATA ) {
        return;
    }
    c_next();
    if ( cc->checking || cc->pass != 2 ) {
        return;
    }
    text = cc->src + tok->col;
    len = tok->len;
    for ( ;; ) {
        while ( pos < len && (text[pos] == ' ' || text[pos] == '\t') ) {
            pos++;
        }
        start = pos;
        quoted = 0;
        if ( pos < len && text[pos] == '"' ) {
            quoted = 1;
            pos++;
            start = pos;
            while ( pos < len && text[pos] != '"' ) {
                pos++;
            }
            end = pos;
            if ( pos < len ) {
                pos++;
            }
            while ( pos < len && text[pos] != ',' ) {
                pos++;
            }
        } else {
            while ( pos < len && text[pos] != ',' ) {
                pos++;
            }
            end = pos;
            while ( end > start && (text[end - 1] == ' ' || text[end - 1] == '\t') ) {
                end--;
            }
        }
        if ( cc->ndata == cc->data_cap ) {
            u32 cap = cc->data_cap ? cc->data_cap * 2 : 256;
            DATAITEM *grown = (DATAITEM *)try_realloc( cc->data, cap * sizeof( DATAITEM ) );
            if ( grown == NULL ) {
                c_fail( "Out of memory" );
            }
            cc->data = grown;
            cc->data_cap = cap;
        }
        item = &cc->data[cc->ndata++];
        item->text = c_pool_strn( text + start, end - start );
        item->len = (u16)(end - start);
        item->quoted = (u8)quoted;
        item->in_text = (u16)cc->text;
        item->line = cc->line;
        item->col = (u16)(tok->col + start);
        if ( pos >= len ) {
            break;
        }
        pos++;                                  /* the comma */
    }
}

static void st_key( void )
{
    if ( c_is( TK_LPAREN ) ) {
        event_control( 2 );
        return;
    }
    if ( c_accept_kw( KW_ON ) ) {
        c_push_l( 1 );
        c_rt( RT_KEYONOFF );
    } else if ( c_accept_kw( KW_OFF ) ) {
        c_push_l( 0 );
        c_rt( RT_KEYONOFF );
    } else if ( c_accept_kw( KW_LIST ) ) {
        c_push_l( 2 );
        c_rt( RT_KEYONOFF );
    } else {
        expr_long();
        c_need( TK_COMMA, "," );
        expr_str();
        c_rt( RT_KEYSET );
    }
}

static void simple_str( int routine )
{
    expr_str();
    c_rt( routine );
}

static void opt_str_given( int routine )
{
    if ( c_at_end() ) {
        c_op32( OP_CSTR, c_const_str( "", 0 ) );
        c_push_l( 0 );
    } else {
        expr_str();
        c_push_l( 1 );
    }
    c_rt( routine );
}

/* An Immediate line runs and is gone: it may not declare what the
   program was made without, as QBasic's may not.  EXIT SUB would leave
   a procedure the line did not call. */
static int immediate_allows( int kw )
{
    switch ( kw ) {
    case KW_DECLARE:
    case KW_SUB:
    case KW_FUNCTION:
    case KW_CONST:
    case KW_DIM:
    case KW_REDIM:
    case KW_COMMON:
    case KW_STATIC:
    case KW_SHARED:
    case KW_TYPE:
    case KW_DATA:
    case KW_OPTION:
        return 0;
    case KW_EXIT:
        /* in the module, st_exit() says "Illegal outside of SUB/FUNCTION" */
        return cc->procscope == NULL
               || !(c_peek( 1 )->kind == TK_KW && (c_peek( 1 )->kw == KW_SUB
                    || c_peek( 1 )->kw == KW_FUNCTION || c_peek( 1 )->kw == KW_DEF));
    case KW_DEF:
        return c_peek( 1 )->kind == TK_KW && c_peek( 1 )->kw == KW_SEG;
    case KW_END:
        /* END SUB would leave the stopped procedure from under the run */
        return !(c_peek( 1 )->kind == TK_KW && (c_peek( 1 )->kw == KW_SUB
                 || c_peek( 1 )->kw == KW_FUNCTION || c_peek( 1 )->kw == KW_DEF));
    }
    return 1;
}

void c_statement( void )
{
    TOKEN *tok = c_tok();
    int kw;

    if ( tok->kind == TK_ID ) {
        if ( is_assignment() ) {
            st_assign();
            return;
        }
        if ( tok->flags & TF_FN ) {
            c_fail( "Syntax error" );
        }
        call_sub( 0 );
        return;
    }
    if ( tok->kind != TK_KW ) {
        if ( tok->kind == TK_DATA ) {
            if ( cc->immediate ) {
                c_fail( IMM_REFUSED );
            }
            st_data();
            return;
        }
        c_expect_msg( "statement" );
    }
    kw = tok->kw;
    if ( cc->immediate && !immediate_allows( kw ) ) {
        c_fail( IMM_REFUSED );
    }
    c_next();
    switch ( kw ) {
    case KW_LET:
        if ( !c_is( TK_ID ) ) {
            c_expect_msg( "identifier" );
        }
        st_assign();
        return;
    case KW_PRINT:      st_print( 0 ); return;
    case KW_LPRINT:     st_print( 1 ); return;
    case KW_WRITE:      st_write(); return;
    case KW_INPUT:      st_input(); return;
    case KW_LINE:
        if ( c_accept_kw( KW_INPUT ) ) {
            st_line_input();
        } else {
            st_line();
        }
        return;
    case KW_IF:         st_if(); return;
    case KW_ELSEIF:     st_elseif(); return;
    case KW_ELSE:
        st_else();
        return;
    case KW_FOR:        st_for(); return;
    case KW_NEXT:       st_next(); return;
    case KW_DO:         st_do(); return;
    case KW_LOOP:       st_loop(); return;
    case KW_WHILE:      st_while(); return;
    case KW_WEND:       st_wend(); return;
    case KW_EXIT:       st_exit(); return;
    case KW_SELECT:     st_select(); return;
    case KW_CASE:       st_case(); return;
    case KW_GOTO:       st_goto( OP_JMP ); return;
    case KW_GOSUB:      st_goto( OP_GOSUB ); return;
    case KW_RETURN:
        if ( c_at_end() ) {
            c_op( cc->immediate ? OP_IMMRETURN : OP_RETURN );
        } else {
            SYM *label;
            if ( cc->immediate ) {
                c_fail_tok( IMM_REFUSED );
            }
            label = c_label_ref( 0 );
            c_op( OP_RETURNTO );
            c_emit_label( label );
        }
        return;
    case KW_ON:         st_on(); return;
    case KW_RESUME:     st_resume(); return;
    case KW_ERROR:
        expr_long();
        c_rt( RT_ERROR );
        return;
    case KW_END:        st_end(); return;
    case KW_STOP:
        c_op( OP_STOP );
        return;
    case KW_SYSTEM:     c_op( OP_SYSTEM ); return;
    case KW_CALL:       st_call(); return;
    case KW_DECLARE:
        if ( c_accept_kw( KW_SUB ) ) {
            proc_heading( PK_SUB, NULL, cc->scope->deftype, 1 );
        } else if ( c_accept_kw( KW_FUNCTION ) ) {
            proc_heading( PK_FUNC, NULL, cc->scope->deftype, 1 );
        } else {
            c_expect_msg( "SUB or FUNCTION" );
        }
        return;
    case KW_SUB:
    case KW_FUNCTION:
        if ( cc->checking ) {
            proc_heading( kw == KW_SUB ? PK_SUB : PK_FUNC, NULL, cc->scope->deftype, 0 );
            return;
        }
        c_fail_tok( "SUB or FUNCTION not allowed here" );
        return;
    case KW_DEF:        st_def_fn(); return;
    case KW_DIM:        st_dim( D_DIM ); return;
    case KW_REDIM:      st_dim( D_REDIM ); return;
    case KW_COMMON:     st_dim( D_COMMON ); return;
    case KW_SHARED:     st_dim( D_SHARED ); return;
    case KW_STATIC:
        if ( cc->procscope == NULL && !cc->checking ) {
            c_fail_tok( "Illegal outside of SUB, FUNCTION, or DEF FN" );
        }
        st_dim( D_STATIC );
        return;
    case KW_CONST:      st_const( cc->procscope != NULL ); return;
    case KW_TYPE:
        if ( !cc->checking ) {
            c_fail_tok( "Illegal in procedure" );
        }
        if ( !c_is( TK_ID ) ) {
            c_expect_msg( "identifier" );
        }
        c_next();
        return;
    case KW_DEFINT:
    case KW_DEFLNG:
    case KW_DEFSNG:
    case KW_DEFDBL:
    case KW_DEFSTR:
        st_deftype( deftype_kw( kw ), cc->scope->deftype );
        return;
    case KW_OPTION:
        c_need_kw( KW_BASE );
        if ( !c_is( TK_NUM ) || (c_tok()->num != 0 && c_tok()->num != 1) ) {
            c_expect_msg( "0 or 1" );
        }
        if ( !cc->checking && cc->option_base_set ) {
            c_fail( "Duplicate definition" );
        }
        cc->option_base = (int)c_tok()->num;
        cc->option_base_set = 1;
        c_next();
        return;
    case KW_DATA:       st_data(); return;
    case KW_READ:       st_read(); return;
    case KW_RESTORE:    st_restore(); return;
    case KW_SWAP:       st_swap(); return;
    case KW_MID_S:      st_mid(); return;
    case KW_LSET:       st_lset( 0 ); return;
    case KW_RSET:       st_lset( 1 ); return;
    case KW_ERASE:
        do {
            c_array_slot( 0 );
            c_rt( RT_ERASE );
        } while ( c_accept( TK_COMMA ) );
        return;
    case KW_RANDOMIZE:
        if ( c_at_end() ) {
            c_rt( RT_RANDASK );
        } else {
            expr_dbl();
            c_rt( RT_RANDOMIZE );
        }
        return;
    case KW_CLS:
        if ( c_at_end() ) {
            c_push_l( -1 );
        } else {
            expr_long();
        }
        c_rt( RT_CLS );
        return;
    case KW_COLOR:      opt_list( 3, RT_COLOR ); return;
    case KW_LOCATE:     opt_list( 5, RT_LOCATE ); return;
    case KW_WIDTH:      st_width(); return;
    case KW_VIEW:       st_view(); return;
    case KW_WINDOW:     st_window(); return;
    case KW_SCREEN:     st_screen(); return;
    case KW_PSET:       st_pset( 0 ); return;
    case KW_PRESET:     st_pset( 1 ); return;
    case KW_CIRCLE:     st_circle(); return;
    case KW_PAINT:      st_paint(); return;
    case KW_DRAW:       simple_str( RT_DRAW ); return;
    case KW_PALETTE:
        if ( c_accept_kw( KW_USING ) ) {
            array_and_element();
            c_rt( RT_PALUSING );
            return;
        }
        if ( c_at_end() ) {
            c_push_l( 0 );
            c_push_l( 0 );
            c_push_l( 0 );
        } else {
            expr_long();
            c_need( TK_COMMA, "," );
            expr_long();
            c_push_l( 1 );
        }
        c_rt( RT_PALETTE );
        return;
    case KW_PCOPY:
        expr_long();
        c_need( TK_COMMA, "," );
        expr_long();
        c_rt( RT_PCOPY );
        return;
    case KW_BEEP:       c_rt( RT_BEEP ); return;
    case KW_SOUND:
        expr_dbl();
        c_need( TK_COMMA, "," );
        expr_dbl();
        c_rt( RT_SOUND );
        return;
    case KW_PLAY:
        if ( c_is_kw( KW_ON ) || c_is_kw( KW_OFF ) || c_is_kw( KW_STOP ) ) {
            event_control( 3 );
        } else {
            simple_str( RT_PLAY );
        }
        return;
    case KW_TIMER:      event_control( 1 ); return;
    case KW_KEY:        st_key(); return;
    case KW_COM:        event_control( 4 ); return;
    case KW_PEN:        event_control( 5 ); return;
    case KW_STRIG:      event_control( 6 ); return;
    case KW_OPEN:       st_open(); return;
    case KW_CLOSE:      st_close(); return;
    case KW_RESET:      c_rt( RT_CLOSEALL ); return;
    case KW_GET:        st_get_put( 0 ); return;
    case KW_PUT:        st_get_put( 1 ); return;
    case KW_FIELD:      st_field(); return;
    case KW_SEEK:
        file_num();
        c_need( TK_COMMA, "," );
        expr_long();
        c_rt( RT_SEEKS );
        return;
    case KW_LOCK:       st_lock( 0 ); return;
    case KW_UNLOCK:     st_lock( 1 ); return;
    case KW_KILL:       simple_str( RT_KILL ); return;
    case KW_NAME:
        expr_str();
        c_need_kw( KW_AS );
        expr_str();
        c_rt( RT_NAME );
        return;
    case KW_MKDIR:      simple_str( RT_MKDIR ); return;
    case KW_RMDIR:      simple_str( RT_RMDIR ); return;
    case KW_CHDIR:      simple_str( RT_CHDIR ); return;
    case KW_FILES:      opt_str_given( RT_FILES ); return;
    case KW_SHELL:      opt_str_given( RT_SHELL ); return;
    case KW_ENVIRON:    simple_str( RT_ENVSET ); return;
    case KW_IOCTL:
        file_num();
        c_need( TK_COMMA, "," );
        expr_str();
        c_rt( RT_IOCTL );
        return;
    case KW_DATE_S:
        c_need( TK_EQ, "=" );
        simple_str( RT_DATESET );
        return;
    case KW_TIME_S:
        c_need( TK_EQ, "=" );
        simple_str( RT_TIMESET );
        return;
    case KW_SLEEP:
        if ( c_at_end() ) {
            c_push_l( 0 );
            c_push_l( 0 );
        } else {
            c_push_l( 1 );
            expr_long();
        }
        c_rt( RT_SLEEP );
        return;
    case KW_WAIT:
        expr_long();
        c_need( TK_COMMA, "," );
        expr_long();
        if ( c_accept( TK_COMMA ) ) {
            expr_long();
        } else {
            c_push_l( 0 );
        }
        c_rt( RT_WAIT );
        return;
    case KW_OUT:
        expr_long();
        c_need( TK_COMMA, "," );
        expr_long();
        c_rt( RT_OUT );
        return;
    case KW_POKE:
        expr_long();
        c_need( TK_COMMA, "," );
        expr_long();
        c_rt( RT_POKE );
        return;
    case KW_BLOAD:
        expr_str();
        if ( c_accept( TK_COMMA ) ) {
            c_push_l( 1 );
            expr_long();
        } else {
            c_push_l( 0 );
            c_push_l( 0 );
        }
        c_rt( RT_BLOAD );
        return;
    case KW_BSAVE:
        expr_str();
        c_need( TK_COMMA, "," );
        expr_long();
        c_need( TK_COMMA, "," );
        expr_long();
        c_rt( RT_BSAVE );
        return;
    case KW_CLEAR:
        /* CLEAR [, [, stack]]: the numbers mean nothing here */
        while ( !c_at_end() ) {
            if ( !c_accept( TK_COMMA ) ) {
                expr();
                c_op( OP_POPN );
            }
        }
        c_rt( RT_CLEAR );
        return;
    case KW_RUN:
        if ( c_at_end() ) {
            c_op32( OP_CSTR, c_const_str( "", 0 ) );
        } else if ( c_is( TK_NUM ) ) {
            /* RUN n: the program again, from line n of the module */
            c_op( OP_CI );
            c_emit_label( c_label_ref( 1 ) );
            c_rt( RT_RUNLINE );
            return;
        } else {
            expr_str();
        }
        c_rt( RT_RUNFILE );
        return;
    case KW_CHAIN:      simple_str( RT_CHAIN ); return;
    case KW_TRON:
        c_push_l( 1 );
        c_rt( RT_TRON );
        return;
    case KW_TROFF:
        c_push_l( 0 );
        c_rt( RT_TRON );
        return;
    case KW_UEVENT:
        c_fail_tok( "Advanced feature unavailable" );
        return;
    }
    c_fail_tok( "Expected: statement" );
}

/* ------------------------------------------------------------------ */
/* the passes                                                          */
/* ------------------------------------------------------------------ */

static void open_line( int text, u32 line )
{
    const char *src;
    u32 len;

    cc->text = text;
    cc->line = line;
    get_line( text, line, &src, &len );
    lex_src( src, len );
}

/* skip a line number or label at the front */
static void skip_label( void )
{
    if ( c_is( TK_LINENUM ) ) {
        c_next();
    }
    if ( c_is( TK_LABEL ) ) {
        c_next();
    }
}

/* PASS 0: the module's TYPEs, CONSTs, DEF FN headings and DECLAREs */
static void pass0( void )
{
    DOC *doc = cc->pt->texts[0];
    u32 line;
    int in_type = 0;
    RECDEF *rec = NULL;
    struct RFIELD fields[256];
    TOKEN *tok;

    cc->pass = 0;
    cc->scope = &cc->module;
    for ( line = 0; line < doc->count; line++ ) {
        open_line( 0, line );
        skip_label();
        if ( in_type ) {
            if ( c_is( TK_EOL ) || c_is( TK_REM ) ) {
                continue;
            }
            if ( c_is_kw( KW_END ) && c_peek( 1 )->kind == TK_KW && c_peek( 1 )->kw == KW_TYPE ) {
                rec->fields = (struct RFIELD *)c_pool( (rec->nfields ? rec->nfields : 1) * sizeof( struct RFIELD ) );
                mem_cpy( rec->fields, fields, rec->nfields * sizeof( struct RFIELD ) );
                in_type = 0;
                continue;
            }
            tok = c_tok();
            if ( tok->kind != TK_ID || tok->kw ) {
                c_expect_msg( "identifier" );
            }
            if ( rec->nfields >= 256 ) {
                c_fail( "Too many elements" );
            }
            {
                char upper[NAME_MAX + 1];
                int i;
                c_name_of( tok, upper );
                for ( i = 0; i < rec->nfields; i++ ) {
                    if ( str_icmp( fields[i].name, upper ) == 0 ) {
                        c_fail( "Duplicate definition" );
                    }
                }
                mem_cpy( fields[rec->nfields].name, cc->src + tok->col, tok->len );
                fields[rec->nfields].name[tok->len] = 0;
                c_next();
                c_need_kw( KW_AS );
                fields[rec->nfields].type = parse_as_type( 0, 1 );
                if ( TKIND( fields[rec->nfields].type ) == TY_STR ) {
                    c_fail_tok( "Fixed-length string expected" );
                }
                fields[rec->nfields].offset = rec->size;
                rec->size += type_size( fields[rec->nfields].type );
                rec->nfields++;
            }
            continue;
        }
        while ( !c_is( TK_EOL ) && !c_is( TK_REM ) ) {
            tok = c_tok();
            if ( tok->kind == TK_KW && tok->kw == KW_TYPE ) {
                char upper[NAME_MAX + 1];
                SYM *sym;
                c_next();
                if ( !c_is( TK_ID ) || c_tok()->kw ) {
                    c_expect_msg( "identifier" );
                }
                c_name_of( c_tok(), upper );
                if ( type_find( upper ) != NULL ) {
                    c_fail( "Duplicate definition" );
                }
                if ( cc->nrecs == cc->recs_cap ) {
                    int cap = cc->recs_cap ? cc->recs_cap * 2 : 16;
                    RECDEF *grown = (RECDEF *)try_realloc( cc->recs, cap * sizeof( RECDEF ) );
                    if ( grown == NULL ) {
                        c_fail( "Out of memory" );
                    }
                    cc->recs = grown;
                    cc->recs_cap = cap;
                }
                rec = &cc->recs[cc->nrecs];
                mem_set( rec, 0, sizeof( *rec ) );
                mem_cpy( rec->name, cc->src + c_tok()->col, c_tok()->len );
                rec->name[c_tok()->len] = 0;
                sym = (SYM *)c_pool( sizeof( SYM ) );
                str_cpy( sym->name, upper );
                sym->kind = SK_TYPE;
                sym->rec = cc->nrecs;
                {
                    u32 hash = 0;
                    const char *cur = upper;
                    while ( *cur ) {
                        hash = hash * 31 + (u8)*cur++;
                    }
                    sym->next = cc->types[hash % NBUCKET];
                    cc->types[hash % NBUCKET] = sym;
                }
                cc->nrecs++;
                c_next();
                in_type = 1;
                break;
            }
            if ( tok->kind == TK_KW && deftype_kw( tok->kw ) ) {
                c_next();
                st_deftype( deftype_kw( tok->kw ), cc->module.deftype );
            } else if ( tok->kind == TK_KW && tok->kw == KW_CONST ) {
                c_next();
                st_const( 1 );
            } else if ( tok->kind == TK_KW && tok->kw == KW_DEF && c_peek( 1 )->kind == TK_ID
                     && (c_peek( 1 )->flags & TF_FN) ) {
                char upper[NAME_MAX + 1];
                int index, sfx;
                PROC tmp;
                c_next();
                c_name_of( c_tok(), upper );
                sfx = c_tok()->ntype;
                if ( sym_find( &cc->module, upper, 0, SK_PROC ) != NULL ) {
                    c_fail( "Duplicate definition" );
                }
                index = proc_new( upper, PK_DEFFN );
                /* a DEF FN takes its DEFtype from the letter after FN */
                cc->procs[index].rettype = sfx ? (QTYPE)sfx
                    : (upper[2] >= 'A' && upper[2] <= 'Z') ? (QTYPE)cc->module.deftype[upper[2] - 'A'] : TY_SINGLE;
                mem_cpy( cc->procs[index].name, cc->src + c_tok()->col, c_tok()->len );
                cc->procs[index].name[c_tok()->len] = 0;
                cc->procs[index].text = 0;
                cc->procs[index].line = line;
                c_next();
                param_list( &tmp, NULL, cc->module.deftype, 0 );
                cc->procs[index].nparams = tmp.nparams;
                cc->procs[index].params = tmp.params;
                break;
            } else if ( tok->kind == TK_KW && tok->kw == KW_DECLARE ) {
                c_next();
                if ( c_accept_kw( KW_SUB ) ) {
                    proc_heading( PK_SUB, NULL, cc->module.deftype, 1 );
                } else if ( c_accept_kw( KW_FUNCTION ) ) {
                    proc_heading( PK_FUNC, NULL, cc->module.deftype, 1 );
                }
                break;
            } else {
                break;
            }
            if ( !c_accept( TK_COLON ) ) {
                break;
            }
        }
    }
    if ( in_type ) {
        c_fail( "TYPE without END TYPE" );
    }
}

/* PASS 1: each procedure's heading */
static void pass1( void )
{
    int text, index;
    u32 line;
    DOC *doc;
    u8 deftype[26];
    TOKEN *tok;

    cc->pass = 1;
    for ( text = 1; text < cc->pt->ntexts; text++ ) {
        doc = cc->pt->texts[text];
        mem_set( deftype, TY_SINGLE, sizeof( deftype ) );
        for ( line = 0; line < doc->count; line++ ) {
            open_line( text, line );
            skip_label();
            tok = c_tok();
            if ( tok->kind == TK_KW && deftype_kw( tok->kw ) ) {
                c_next();
                st_deftype( deftype_kw( tok->kw ), deftype );
                continue;
            }
            if ( tok->kind == TK_KW && (tok->kw == KW_SUB || tok->kw == KW_FUNCTION) ) {
                c_next();
                index = proc_heading( tok->kw == KW_SUB ? PK_SUB : PK_FUNC, NULL, deftype, 0 );
                cc->procs[index].text = text;
                cc->procs[index].line = line;
                break;
            }
        }
        if ( line >= doc->count ) {
            cc->text = text;
            cc->line = 0;
            c_fail_at( 0, "SUB or FUNCTION expected" );
        }
    }
}

/* PASS 2: the module */
static void pass2( void )
{
    DOC *doc = cc->pt->texts[0];
    u32 line;
    int in_type = 0, i;

    cc->pass = 2;
    cc->cur_proc = -1;
    cc->procscope = NULL;
    cc->scope = &cc->module;
    cc->lineno = 0;
    for ( i = 0; i < 26; i++ ) {
        cc->module.deftype[i] = TY_SINGLE;
    }
    for ( line = 0; line < doc->count; line++ ) {
        open_line( 0, line );
        if ( in_type ) {
            skip_label();
            if ( c_is_kw( KW_END ) && c_peek( 1 )->kind == TK_KW && c_peek( 1 )->kw == KW_TYPE ) {
                in_type = 0;
            }
            continue;
        }
        {
            int at = cc->tp;
            if ( cc->toks[at].kind == TK_LINENUM ) {
                at++;
            }
            if ( cc->toks[at].kind == TK_LABEL ) {
                at++;
            }
            if ( cc->toks[at].kind == TK_KW && cc->toks[at].kw == KW_TYPE ) {
                compile_line_label_only();
                in_type = 1;
                continue;
            }
        }
        compile_line();
    }
    if ( cc->in_deffn >= 0 ) {
        block_unclosed( &cc->blocks[cc->nblocks - 1] );
    }
    if ( cc->nblocks > 0 ) {
        block_unclosed( &cc->blocks[cc->nblocks - 1] );
    }
    c_op( OP_MODEND );
    check_labels( &cc->module );
}

/* PASS 3: one procedure */
static void pass3_proc( int text )
{
    DOC *doc = cc->pt->texts[text];
    SCOPE *scope;
    PROC *proc = NULL;
    u32 line;
    int index = -1, seen_head = 0, seen_end = 0;
    TOKEN *tok;

    cc->pass = 3;
    scope = scope_new( -1 );
    cc->procscope = scope;
    cc->scope = scope;
    cc->cur_proc = -1;
    cc->lineno = 0;
    cc->nblocks = 0;
    for ( line = 0; line < doc->count; line++ ) {
        open_line( text, line );
        if ( !seen_head ) {
            skip_label();
            tok = c_tok();
            if ( tok->kind == TK_KW && (tok->kw == KW_SUB || tok->kw == KW_FUNCTION) ) {
                char upper[NAME_MAX + 1];
                SYM *sym;
                int kind = tok->kw == KW_SUB ? PK_SUB : PK_FUNC;
                c_next();
                c_name_of( c_tok(), upper );
                sym = sym_find( &cc->module, upper, 0, SK_PROC );
                index = sym->proc;
                proc = &cc->procs[index];
                scope->proc = index;
                cc->cur_proc = index;
                c_next();
                {
                    PROC tmp;
                    param_list( &tmp, scope, scope->deftype, 0 );
                }
                c_accept_kw( KW_STATIC );
                scope->size = (u32)proc->nparams * 4;
                if ( kind == PK_FUNC ) {
                    proc->ret_off = scope_alloc( scope, type_size( proc->rettype ), 0 );
                    if ( TKIND( proc->rettype ) == TY_STR ) {
                        scope_note_str( scope, proc->ret_off );
                    }
                }
                proc->entry = c_here();
                proc->first_stmt = cc->nlocs;
                {
                    BLOCK *blk = block_open( B_PROC );
                    blk->text = text;
                    blk->line = line;
                    cc->tp = 0;
                    blk->col = c_tok()->col;
                    blk->len = c_tok()->len;
                }
                cc->tp = 0;
                stmt_mark();
                c_op( OP_NOP );                 /* a step into the procedure stops on its heading */
                cc->locs[cc->nlocs - 1].end_pc = c_here();
                cc->locs[cc->nlocs - 1].len = (u16)doc_line( doc, line )->len;
                seen_head = 1;
                continue;
            }
            /* before the heading: DEFtype, remarks and blank lines */
            if ( tok->kind == TK_KW && deftype_kw( tok->kw ) ) {
                c_next();
                st_deftype( deftype_kw( tok->kw ), scope->deftype );
                continue;
            }
            if ( tok->kind == TK_EOL || tok->kind == TK_REM ) {
                if ( tok->kind == TK_REM ) {
                    metacommand( tok );
                }
                continue;
            }
            c_fail( "Statement cannot precede SUB/FUNCTION definition" );
        }
        if ( seen_end ) {
            skip_label();
            if ( !c_is( TK_EOL ) && !c_is( TK_REM ) ) {
                c_fail( "Statements cannot follow END SUB/FUNCTION" );
            }
            continue;
        }
        compile_line();
        if ( cc->nblocks == 0 ) {
            seen_end = 1;
        }
    }
    if ( !seen_end ) {
        if ( cc->nblocks > 0 ) {
            block_unclosed( &cc->blocks[cc->nblocks - 1] );
        }
    }
    check_labels( scope );
    proc->frame_size = scope->size;
    proc->static_size = scope->ssize;
    proc->rel_str = scope->rel_str;
    proc->nrel_str = scope->nrel_str;
    proc->rel_arr = scope->rel_arr;
    proc->nrel_arr = scope->nrel_arr;
    proc->srel_str = scope->srel_str;
    proc->nsrel_str = scope->nsrel_str;
    proc->srel_arr = scope->srel_arr;
    proc->nsrel_arr = scope->nsrel_arr;
    proc->imp_arr = scope->imp_arr;
    proc->imp_type = scope->imp_type;
    proc->imp_dims = scope->imp_dims;
    proc->nimp_arr = scope->nimp_arr;
    cc->procscope = NULL;
    cc->scope = &cc->module;
    cc->cur_proc = -1;
}

/* ------------------------------------------------------------------ */
/* the program                                                         */
/* ------------------------------------------------------------------ */

static void scope_lists_free( SCOPE *scope )
{
    xfree( scope->rel_str );
    xfree( scope->rel_arr );
    xfree( scope->srel_str );
    xfree( scope->srel_arr );
    xfree( scope->imp_arr );
    xfree( scope->imp_type );
    xfree( scope->imp_dims );
}

/* everything a compile made, given back */
static void state_free( COMP *state )
{
    SCOPE *scope;
    u32 i;

    if ( state == NULL ) {
        return;
    }
    for ( scope = state->scopes; scope != NULL; scope = scope->chain ) {
        scope_lists_free( scope );
    }
    scope_lists_free( &state->module );
    /* a constant's count never reaches 0: free the ones nothing else
       holds (one kept by, say, a KEY's text stays, and is lost) */
    for ( i = 0; i < state->nconsts; i++ ) {
        if ( state->consts[i] != NULL && state->consts[i]->ref == QS_PERM ) {
            xfree( state->consts[i] );
        }
    }
    xfree( state->consts );
    xfree( state->code );
    xfree( state->locs );
    xfree( state->data );
    xfree( state->arrinit );
    xfree( state->common );
    xfree( state->procs );
    xfree( state->procsym );
    xfree( state->recs );
    pool_free( state->pool );
    xfree( state );
}

static COMP *state_new( PROGTEXT *pt, CERR *err )
{
    COMP *state = (COMP *)try_alloc( sizeof( COMP ) );

    if ( state == NULL ) {
        return NULL;
    }
    mem_set( state, 0, sizeof( *state ) );
    state->pt = pt;
    state->err = err;
    state->pool = pool_new();
    if ( state->pool == NULL ) {
        xfree( state );
        return NULL;
    }
    state->in_deffn = -1;
    state->cur_proc = -1;
    cc = state;
    scope_init( &state->module, -1 );
    state->scope = &state->module;
    return state;
}

PROGRAM *comp_program( PROGTEXT *pt, CERR *err )
{
    COMP *state;
    PROGRAM *prog;
    int text;

    mem_set( err, 0, sizeof( *err ) );
    state = state_new( pt, err );
    if ( state == NULL ) {
        str_cpy( err->msg, "Out of memory" );
        return NULL;
    }
    if ( rt_setjmp( &state->fail ) ) {
        state_free( state );
        cc = NULL;
        return NULL;
    }
    pass0();
    pass1();
    pass2();
    for ( text = 1; text < pt->ntexts; text++ ) {
        pass3_proc( text );
    }

    /* room after the code for the Immediate window's lines: without it
       they are refused, not the program */
    if ( state->code_cap < state->code_len + IMM_CODE_SPARE ) {
        u8 *grown = (u8 *)try_realloc( state->code, state->code_len + IMM_CODE_SPARE );
        if ( grown != NULL ) {
            state->code = grown;
            state->code_cap = state->code_len + IMM_CODE_SPARE;
        }
    }

    prog = (PROGRAM *)try_alloc( sizeof( PROGRAM ) );
    if ( prog == NULL ) {
        c_fail_at( 0, "Out of memory" );
    }
    mem_set( prog, 0, sizeof( *prog ) );
    prog->code = state->code;
    prog->code_len = state->code_len;
    prog->consts = state->consts;
    prog->nconsts = state->nconsts;
    prog->locs = state->locs;
    prog->nlocs = state->nlocs;
    prog->procs = state->procs;
    prog->nprocs = state->nprocs;
    prog->recs = state->recs;
    prog->nrecs = state->nrecs;
    prog->gsize = state->module.size;
    prog->grel_str = state->module.rel_str;
    prog->ngrel_str = state->module.nrel_str;
    prog->grel_arr = state->module.rel_arr;
    prog->ngrel_arr = state->module.nrel_arr;
    prog->arrinit = state->arrinit;
    prog->common = state->common;
    prog->ncommon = state->ncommon;
    prog->narrinit = state->narrinit;
    prog->data = state->data;
    prog->ndata = state->ndata;
    prog->main_entry = 0;
    prog->uses_events = (u8)state->uses_events;
    prog->option_base = state->option_base;
    prog->syms = (struct COMPSYMS *)state;      /* kept: the names, the pool */
    cc = NULL;
    return prog;
}

void comp_free( PROGRAM *prog )
{
    if ( prog == NULL ) {
        return;
    }
    state_free( (COMP *)prog->syms );
    xfree( prog );
}

/* ------------------------------------------------------------------ */
/* the Immediate window, in a stopped program                          */
/* ------------------------------------------------------------------ */

/* A line of the Immediate window, compiled into the stopped program for
   the procedure it is stopped in (-1 the module): -> where its code
   starts, or -1 with err set.  The code goes after the program's, in
   the room comp_program() left, as code that frames point into may not
   move.  Names the program does not have go in a scope of their own,
   their data in the module's spare room, up to gcap bytes.  A line that
   fails takes back every name it made. */
s32 comp_immediate( PROGRAM *prog, int proc, const char *src, u32 len, u32 gcap, CERR *err )
{
    COMP *state = (COMP *)prog->syms;
    SCOPE *scope = NULL;
    SYM *module[NBUCKET], *local[NBUCKET], *mine[NBUCKET];
    u32 size, nrel_str, nrel_arr, narrinit, nlocs, code_len, nconsts;
    s32 entry;

    mem_set( err, 0, sizeof( *err ) );
    if ( state == NULL ) {
        str_cpy( err->msg, "Out of memory" );
        return -1;
    }
    cc = state;
    state->err = err;
    if ( proc >= 0 ) {
        for ( scope = state->scopes; scope != NULL; scope = scope->chain ) {
            if ( scope->proc == proc && !scope->is_deffn ) {
                break;
            }
        }
    }
    if ( rt_setjmp( &state->fail ) ) {
        cc = NULL;
        return -1;
    }
    if ( state->immscope == NULL ) {
        state->immscope = scope_new( -1 );
    }

    mem_cpy( module, state->module.bucket, sizeof( module ) );
    mem_cpy( mine, state->immscope->bucket, sizeof( mine ) );
    if ( scope != NULL ) {
        mem_cpy( local, scope->bucket, sizeof( local ) );
    }
    size = state->module.size;
    nrel_str = state->module.nrel_str;
    nrel_arr = state->module.nrel_arr;
    narrinit = state->narrinit;
    nlocs = state->nlocs;
    code_len = state->code_len;
    nconsts = state->nconsts;
    if ( rt_setjmp( &state->fail ) ) {
        /* the line's strings go; the lists it grew may have moved, and
           the program must see where they are now */
        while ( state->nconsts > nconsts ) {
            STR *s = state->consts[--state->nconsts];
            if ( s != NULL ) {
                xfree( s );
            }
        }
        prog->consts = state->consts;
        prog->nconsts = state->nconsts;
        prog->locs = state->locs;
        prog->arrinit = state->arrinit;
    prog->common = state->common;
    prog->ncommon = state->ncommon;
        prog->grel_str = state->module.rel_str;
        prog->grel_arr = state->module.rel_arr;
        mem_cpy( state->module.bucket, module, sizeof( module ) );
        mem_cpy( state->immscope->bucket, mine, sizeof( mine ) );
        if ( scope != NULL ) {
            mem_cpy( scope->bucket, local, sizeof( local ) );
        }
        state->module.size = size;
        state->module.nrel_str = nrel_str;
        state->module.nrel_arr = nrel_arr;
        state->narrinit = narrinit;
        state->nlocs = nlocs;
        state->code_len = code_len;
        state->immediate = 0;
        cc = NULL;
        return -1;
    }

    state->pass = scope != NULL ? 3 : 2;
    state->checking = 0;
    state->text = -1;
    state->line = 0;
    state->lineno = 0;
    state->cur_proc = scope != NULL ? proc : -1;
    state->procscope = scope;
    state->scope = scope != NULL ? scope : &state->module;
    state->in_deffn = -1;
    state->sl_then = 0;
    state->nblocks = 0;
    state->immediate = 1;
    state->imm_limit = gcap;
    entry = (s32)state->code_len;
    lex_src( src, len );
    if ( c_is( TK_LINENUM ) || c_is( TK_LABEL ) ) {
        c_fail( IMM_REFUSED );
    }
    c_statements_until_eol( 0 );
    if ( state->nblocks > 0 ) {
        block_unclosed( &state->blocks[state->nblocks - 1] );
    }
    c_op( OP_IMMRET );
    state->immediate = 0;

    prog->code_len = state->code_len;
    prog->consts = state->consts;
    prog->nconsts = state->nconsts;
    prog->locs = state->locs;
    prog->nlocs = state->nlocs;
    prog->gsize = state->module.size;
    prog->grel_str = state->module.rel_str;
    prog->ngrel_str = state->module.nrel_str;
    prog->grel_arr = state->module.rel_arr;
    prog->ngrel_arr = state->module.nrel_arr;
    prog->arrinit = state->arrinit;
    prog->common = state->common;
    prog->ncommon = state->ncommon;
    prog->narrinit = state->narrinit;
    prog->uses_events = (u8)state->uses_events;
    cc = NULL;
    return entry;
}

/* the line has run: its code and statements are done with (its names
   and their data stay, for the lines after it) */
void comp_immediate_done( PROGRAM *prog, u32 code_len, u32 nlocs )
{
    COMP *state = (COMP *)prog->syms;

    state->code_len = code_len;
    state->nlocs = nlocs;
    prog->code_len = code_len;
    prog->nlocs = nlocs;
}

/* The editor's check of one line as it is left: its syntax, in the
   setting the rest of the program gives it (TYPEs, CONSTs, the DEFtype
   letters above it).  1 = it is good, and "out" is the line as QBasic
   would show it; 0 = "err" says what and where. */
int comp_check_line( PROGTEXT *pt, int text, u32 line, const char *src, u32 len,
                     char *out, u32 outmax, CERR *err )
{
    COMP *state;
    u32 above;
    int ok, in_type = 0;

    mem_set( err, 0, sizeof( *err ) );
    state = state_new( pt, err );
    if ( state == NULL ) {
        /* no memory: the line as it is (it has no 0 at its end) */
        if ( len >= outmax ) {
            len = outmax - 1;
        }
        mem_cpy( out, src, len );
        out[len] = 0;
        return 1;
    }
    state->checking = 1;

    /* what the rest of the program declares, as far as it can be read:
       a fault elsewhere is not this line's */
    if ( rt_setjmp( &state->fail ) == 0 ) {
        pass0();
    }
    cc->nblocks = 0;
    if ( text > 0 ) {
        cc->procscope = scope_new( -1 );
        cc->scope = cc->procscope;
    } else {
        cc->scope = &cc->module;
        mem_set( cc->module.deftype, TY_SINGLE, 26 );
    }
    if ( rt_setjmp( &state->fail ) == 0 ) {
        for ( above = 0; above < line; above++ ) {
            int at;
            open_line( text, above );
            at = c_is( TK_LINENUM ) ? 1 : 0;
            if ( cc->toks[at].kind == TK_LABEL ) {
                at++;
            }
            if ( cc->toks[at].kind == TK_KW && cc->toks[at].kw == KW_TYPE ) {
                in_type = 1;
            } else if ( cc->toks[at].kind == TK_KW && cc->toks[at].kw == KW_END
                     && cc->toks[at + 1].kind == TK_KW && cc->toks[at + 1].kw == KW_TYPE ) {
                in_type = 0;
            }
            while ( cc->toks[at].kind == TK_KW && deftype_kw( cc->toks[at].kw ) ) {
                cc->tp = at + 1;
                st_deftype( deftype_kw( cc->toks[at].kw ), cc->scope->deftype );
                if ( !c_is( TK_COLON ) ) {
                    break;
                }
                at = cc->tp + 1;
            }
        }
    }
    cc->nblocks = 0;
    cc->pass = 2;
    cc->text = text;
    cc->line = line;
    mem_set( err, 0, sizeof( *err ) );
    if ( rt_setjmp( &state->fail ) == 0 ) {
        lex_src( src, len );
        if ( in_type ) {
            /* a TYPE's element: name AS type, or the END TYPE */
            skip_label();
            if ( c_is_kw( KW_END ) ) {
                c_next();
                c_need_kw( KW_TYPE );
            } else if ( c_is( TK_ID ) ) {
                c_next();
                c_need_kw( KW_AS );
                parse_as_type( 0, 1 );
            }
            if ( !c_is( TK_EOL ) && !c_is( TK_REM ) ) {
                c_expect_msg( "end-of-statement" );
            }
        } else {
            compile_line();
        }
        if ( list_line( src, len, cc->toks, out, outmax ) < 0 ) {
            /* longer than a line once spaced out: as it was typed */
            if ( len >= outmax ) {
                len = outmax - 1;
            }
            mem_cpy( out, src, len );
            out[len] = 0;
        }
        ok = 1;
    } else {
        ok = 0;
    }
    state_free( state );
    cc = NULL;
    return ok;
}
