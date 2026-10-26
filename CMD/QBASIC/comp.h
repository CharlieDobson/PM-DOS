/*
 * COMP.H - what COMP.C, CEXPR.C and CSYM.C share: the compiler's names,
 * the code being written, the statement being read.
 *
 * THE COMPILER READS THE WHOLE PROGRAM EACH TIME IT IS RUN, in passes:
 *
 *   0  the module's TYPEs, CONSTs and DEF FN headings - what the rest
 *      may refer to before reaching where they are written;
 *   1  every procedure's SUB or FUNCTION line: names, parameters, types;
 *   2  the module's statements, into code;
 *   3  each procedure's statements.
 *
 * A name's type is settled where it is first used, from its suffix, an
 * AS clause, or the DEFtype letters in force on that line; after that
 * the code for it is typed (OP.H) and nothing is looked up at run time.
 */
#ifndef COMP_H
#define COMP_H

#include "qb.h"

/* what a name is */
enum {
    SK_VAR, SK_ARRAY, SK_CONST, SK_LABEL, SK_PROC, SK_TYPE
};

#define SF_AS       0x01        /* declared with an AS clause */
#define SF_SHARED   0x02        /* DIM SHARED or COMMON SHARED: every procedure sees it */
#define SF_DYNAMIC  0x04        /* an array made at run time */
#define SF_PARAM    0x08
#define SF_DIMMED   0x10        /* a DIM has been seen for this array */
#define SF_RETVAL   0x20        /* a FUNCTION's own name: its value */
#define SF_IMPLICIT 0x40        /* an array made by being used */

typedef struct FIXUP {
    struct FIXUP *next;
    u32   at;                   /* where in the code the address goes */
    int   text;                 /* where the reference was, for the error */
    u32   line;
    int   col, len;
} FIXUP;

typedef struct SYM {
    struct SYM *next;
    char   name[NAME_MAX + 1];  /* upper case, no suffix */
    u8     tkey;                /* the type kind in the key: TY_*, 0 for AS names */
    u8     kind;                /* SK_* */
    u8     scope;               /* SC_* */
    u8     flags;               /* SF_* */
    QTYPE  type;
    u32    offset;
    u8     ndims;
    int    proc;                /* SK_PROC: its number */
    CELL   cval;                /* SK_CONST */
    u32    cstr;                /* SK_CONST string: its constant number */
    u32    addr;                /* SK_LABEL: where, or NOADDR */
    u32    data_at;             /* SK_LABEL: the DATA item RESTORE goes to */
    FIXUP *fixups;
    int    rec;                 /* SK_TYPE: its number */
} SYM;

#define NOADDR      0xFFFFFFFFUL
#define NBUCKET     64

typedef struct SCOPE {
    struct SCOPE *chain;        /* every scope a compile made, for freeing */
    SYM  *bucket[NBUCKET];
    u8    deftype[26];          /* the DEFtype letters */
    u32   size;                 /* the bytes its variables take */
    u32   ssize;                /* ...and its static ones */
    int   proc;                 /* -1 for the module */
    int   is_deffn;             /* a DEF FN: unknown names are the module's */
    u32  *rel_str;              /* string slots to let go of at the end */
    u32   nrel_str, cap_str;
    u32  *rel_arr;
    u32   nrel_arr, cap_arr;
    u32  *srel_str;             /* the same in the static block */
    u32   nsrel_str, cap_sstr;
    u32  *srel_arr;
    u32   nsrel_arr, cap_sarr;
    u32  *imp_arr;
    QTYPE *imp_type;
    u8   *imp_dims;
    u32   nimp_arr, cap_imp;
} SCOPE;

/* an open block: IF, FOR, DO... waiting for its end */
enum { B_IF, B_DO, B_WHILE, B_FOR, B_SELECT, B_DEFFN, B_PROC };

typedef struct {
    int   kind;
    u32   top;                  /* where a loop goes back to */
    u32   patch;                /* IF: the jump to the next part; SELECT: to the next CASE */
    u32  *exits;                /* jumps to the end */
    int   nexits, cap_exits;
    int   seen_else;
    int   ncases;
    /* FOR */
    u8    vscope, lscope, sscope;
    u32   voff, loff, soff;
    QTYPE vtype;
    SYM  *vsym;
    /* SELECT */
    QTYPE seltype;
    u32   loc;                  /* its STMTLOC: RESUME NEXT skips the whole SELECT */
    u8    selscope;
    u32   seloff;
    /* where it opened, for "without" errors */
    int   text;
    u32   line;
    int   col, len;
} BLOCK;

/* what an Immediate line may not do: declare, or make a label */
#define IMM_REFUSED "Illegal in direct mode"

#define MAX_BLOCKS  64
#define MAX_TOKENS  1024
#define MAX_EBACK   64              /* element copies waiting on calls */

/* the state of a compile */
typedef struct {
    PROGTEXT *pt;
    POOL     *pool;
    JMPBUF    fail;
    CERR     *err;
    int       pass;
    int       checking;         /* only a line's syntax, for the editor */

    /* the line being read */
    int       text;
    u32       line;
    const char *src;
    u32       srclen;
    TOKEN     toks[MAX_TOKENS];
    int       ntoks;
    int       tp;               /* the next token */

    /* the code */
    u8       *code;
    u32       code_len, code_cap;
    STR     **consts;
    u32       nconsts, consts_cap;
    STMTLOC  *locs;
    u32       nlocs, locs_cap;
    DATAITEM *data;
    u32       ndata, data_cap;
    ARRINIT  *arrinit;
    u32       narrinit, arrinit_cap;
    COMMONVAR *common;
    u32       ncommon, common_cap;

    /* the names */
    SCOPE     module;
    SCOPE    *scopes;           /* the procedures' and DEF FNs', chained */
    SCOPE    *scope;            /* where names are made now */
    SCOPE    *procscope;        /* the procedure being compiled, or NULL */
    PROC     *procs;
    int       nprocs, procs_cap;
    SYM     **procsym;          /* each procedure's name */
    RECDEF   *recs;
    int       nrecs, recs_cap;
    SYM      *types[NBUCKET];

    BLOCK     blocks[MAX_BLOCKS];
    int       nblocks;
    int       option_base;
    int       option_base_set;
    int       dynamic;          /* '$DYNAMIC in force */
    int       cur_proc;         /* -1 module */
    u32       cur_stmt;         /* the STMTLOC being compiled */
    s32       lineno;           /* the last line number, for ERL */
    int       uses_events;
    int       in_deffn;         /* the DEF FN being compiled, or -1 */
    int       sl_then;          /* one-line IFs whose THEN part is being read */
    SCOPE    *deffn_scope;
    u32       pending_data_label;
    int       immediate;        /* an Immediate line in a stopped program */
    u32       imm_limit;        /* ...and the module data it may fill */
    SCOPE    *immscope;         /* the names Immediate lines made */

    /* array elements passed as copies, to write back after the call */
    u32       last_aidx;        /* where the last AIDX/AIDX1 was emitted */
    struct {
        u32   copy, note;       /* the hidden copy and its slot/offset note */
        u8    scope;
        u8    is_str;
        u32   size;
    }         eback[MAX_EBACK];
    int       neback;
} COMP;

extern COMP *cc;

/* CSYM.C */
void  *c_pool( u32 size );                      /* zeroed; "Out of memory" fails */
char  *c_pool_strn( const char *text, u32 len );
void   c_fail( const char *msg );               /* at the current token */
void   c_fail_at( int tp, const char *msg );
void   c_fail_tok( const char *msg );
void   c_expect_msg( const char *what );        /* "Expected: what" */
void   c_emit( int byte );
void   c_emit16( u32 val );
void   c_emit32( u32 val );
void   c_emitf( float val );
void   c_emitd( double val );
void   c_patch32( u32 at, u32 val );
u32    c_here( void );
u32    c_const_str( const char *text, u32 len );
void   c_op( int op );
void   c_op32( int op, u32 val );
void   c_rt( int routine );
void   c_push_l( s32 val );
u32    c_jump( int op );                        /* -> where to patch */

TOKEN *c_tok( void );                           /* the next token */
TOKEN *c_peek( int ahead );
void   c_next( void );
int    c_is( int kind );
int    c_is_kw( int kw );
int    c_accept( int kind );
int    c_accept_kw( int kw );
void   c_need( int kind, const char *what );
void   c_need_kw( int kw );
int    c_at_end( void );                        /* end of statement: EOL, :, REM, ELSE */
void   c_name_of( TOKEN *tok, char *upper );    /* a name's text, upper case, no suffix */

void   scope_init( SCOPE *scope, int proc );
SCOPE *scope_new( int proc );                   /* from the pool, chained for freeing */
SYM   *sym_find( SCOPE *scope, const char *upper, int tkey, int kind );
SYM   *sym_add( SCOPE *scope, const char *upper, int tkey, int kind );
u32    scope_alloc( SCOPE *scope, u32 size, int is_static );
void   scope_note_str( SCOPE *scope, u32 off );
void   scope_note_arr( SCOPE *scope, u32 off );
void   scope_note_static( SCOPE *scope, u32 off, int is_array );
u32    type_size( QTYPE type );
int    type_default( SCOPE *scope, const char *upper );    /* TY_* by DEFtype */
SYM   *type_find( const char *upper );
int    rec_field( int rec, const char *upper, QTYPE *type, u32 *offset );
QTYPE  parse_as_type( int allow_any, int allow_fixed );    /* after AS */
const char *type_name( QTYPE type );
u32    c_temp( QTYPE type, u8 *scope );         /* a hidden variable */

/* the variable named by the tokens at tp: found or made */
typedef struct {
    QTYPE type;
    int   simple;               /* a scalar at scope:offset; nothing emitted */
    u8    scope;
    u32   offset;
    SYM  *sym;
    int   is_array_whole;       /* a() */
} VREF;

/* CEXPR.C */
QTYPE  expr( void );                            /* -> TY_INT..TY_STR */
void   expr_num( QTYPE want );                  /* a number, converted */
void   expr_long( void );
void   expr_str( void );
void   expr_dbl( void );
void   c_convert( QTYPE from, QTYPE to );
int    c_var( VREF *ref, int for_store, int allow_whole );     /* 0 = not a variable */
void   c_addr( VREF *ref );                     /* emit its address */
void   c_sym_addr( SYM *sym );                  /* a scalar's or an array slot's */
SYM   *c_find_local( const char *upper, int sfx, int kind );
QTYPE  c_peek_type( void );                     /* the next expression's type; nothing taken */
void   c_load( VREF *ref );                     /* emit its value */
void   c_store( VREF *ref, QTYPE from );        /* the value is on the stack */
QTYPE  value_type( QTYPE vtype );               /* a fixed string's value is a string */
SYM   *c_array_slot( int need_dims );           /* name() for LBOUND, ERASE... */
int    const_expr( CELL *val, QTYPE *type, STR **sval );
void   c_call_args( PROC *proc, int parens );
void   c_call_back( int base );                 /* after the call: element copies back */
int    c_is_proc_name( TOKEN *tok, int kind );
SYM   *c_label_ref( int module_level );         /* a line number or label; fixup */
void   c_emit_label( SYM *label );
SYM   *var_lookup( const char *upper, int tkey, int kind, int create, QTYPE *type_out,
                   int sfxtype, int has_as );

/* COMP.C */
void   c_statement( void );
void   c_statements_until_eol( int stop_at_else );

#endif
