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
    unsigned int at;            /* where in the code the address goes */
    int          text;          /* where the reference was, for the error */
    unsigned int line;
    int          col, len;
} FIXUP;

typedef struct SYM {
    struct SYM  *next;
    char         name[NAME_MAX + 1];  /* upper case, no suffix */
    uint8_t      tkey;                /* the type kind in the key: TY_*, 0 for AS names */
    uint8_t      kind;                /* SK_* */
    uint8_t      scope;               /* SC_* */
    uint8_t      flags;               /* SF_* */
    QTYPE        type;
    unsigned int offset;
    uint8_t      ndims;
    int          proc;                /* SK_PROC: its number */
    CELL         cval;                /* SK_CONST */
    unsigned int cstr;                /* SK_CONST string: its constant number */
    unsigned int addr;                /* SK_LABEL: where, or NOADDR */
    unsigned int data_at;             /* SK_LABEL: the DATA item RESTORE goes to */
    FIXUP       *fixups;
    int          rec;                 /* SK_TYPE: its number */
} SYM;

#define NOADDR      0xFFFFFFFFUL
#define NBUCKET     64

typedef struct SCOPE {
    struct SCOPE *chain;        /* every scope a compile made, for freeing */
    SYM          *bucket[NBUCKET];
    uint8_t       deftype[26];  /* the DEFtype letters */
    unsigned int  size;         /* the bytes its variables take */
    unsigned int  ssize;        /* ...and its static ones */
    int           proc;         /* -1 for the module */
    int           is_deffn;     /* a DEF FN: unknown names are the module's */
    unsigned int *rel_str;      /* string slots to let go of at the end */
    unsigned int  nrel_str, cap_str;
    unsigned int *rel_arr;
    unsigned int  nrel_arr, cap_arr;
    unsigned int *srel_str;     /* the same in the static block */
    unsigned int  nsrel_str, cap_sstr;
    unsigned int *srel_arr;
    unsigned int  nsrel_arr, cap_sarr;
    unsigned int *imp_arr;
    QTYPE        *imp_type;
    uint8_t      *imp_dims;
    unsigned int  nimp_arr, cap_imp;
} SCOPE;

/* an open block: IF, FOR, DO... waiting for its end */
enum { B_IF, B_DO, B_WHILE, B_FOR, B_SELECT, B_DEFFN, B_PROC };

typedef struct {
    int           kind;
    unsigned int  top;          /* where a loop goes back to */
    unsigned int  patch;        /* IF: the jump to the next part; SELECT: to the next CASE */
    unsigned int *exits;        /* jumps to the end */
    int           nexits, cap_exits;
    int           seen_else;
    int           ncases;
    /* FOR */
    uint8_t       vscope, lscope, sscope;
    unsigned int  voff, loff, soff;
    QTYPE         vtype;
    SYM          *vsym;
    /* SELECT */
    QTYPE         seltype;
    unsigned int  loc;          /* its STMTLOC: RESUME NEXT skips the whole SELECT */
    uint8_t       selscope;
    unsigned int  seloff;
    /* where it opened, for "without" errors */
    int           text;
    unsigned int  line;
    int           col, len;
} BLOCK;

/* what an Immediate line may not do: declare, or make a label */
#define IMM_REFUSED "Illegal in direct mode"

#define MAX_BLOCKS  64
#define MAX_TOKENS  1024
#define MAX_EBACK   64              /* element copies waiting on calls */

/* the state of a compile */
typedef struct {
    PROGTEXT     *pt;
    POOL         *pool;
    JMPBUF        fail;
    CERR         *err;
    int           pass;
    int           checking;     /* only a line's syntax, for the editor */

    /* the line being read */
    int           text;
    unsigned int  line;
    const char   *src;
    unsigned int  srclen;
    TOKEN         toks[MAX_TOKENS];
    int           ntoks;
    int           tp;           /* the next token */

    /* the code */
    uint8_t      *code;
    unsigned int  code_len, code_cap;
    STR         **consts;
    unsigned int  nconsts, consts_cap;
    STMTLOC      *locs;
    unsigned int  nlocs, locs_cap;
    DATAITEM     *data;
    unsigned int  ndata, data_cap;
    ARRINIT      *arrinit;
    unsigned int  narrinit, arrinit_cap;
    COMMONVAR    *common;
    unsigned int  ncommon, common_cap;

    /* the names */
    SCOPE         module;
    SCOPE        *scopes;       /* the procedures' and DEF FNs', chained */
    SCOPE        *scope;        /* where names are made now */
    SCOPE        *procscope;    /* the procedure being compiled, or NULL */
    PROC         *procs;
    int           nprocs, procs_cap;
    SYM         **procsym;      /* each procedure's name */
    RECDEF       *recs;
    int           nrecs, recs_cap;
    SYM          *types[NBUCKET];

    BLOCK         blocks[MAX_BLOCKS];
    int           nblocks;
    int           option_base;
    int           option_base_set;
    int           dynamic;      /* '$DYNAMIC in force */
    int           cur_proc;     /* -1 module */
    unsigned int  cur_stmt;     /* the STMTLOC being compiled */
    int32_t       lineno;       /* the last line number, for ERL */
    int           uses_events;
    int           in_deffn;     /* the DEF FN being compiled, or -1 */
    int           sl_then;      /* one-line IFs whose THEN part is being read */
    SCOPE        *deffn_scope;
    unsigned int  pending_data_label;
    int           immediate;    /* an Immediate line in a stopped program */
    unsigned int  imm_limit;    /* ...and the module data it may fill */
    SCOPE        *immscope;     /* the names Immediate lines made */

    /* array elements passed as copies, to write back after the call */
    unsigned int  last_aidx;    /* where the last AIDX/AIDX1 was emitted */
    struct {
        unsigned int copy, note;    /* the hidden copy and its slot/offset note */
        uint8_t      scope;
        uint8_t      is_str;
        unsigned int size;
    }             eback[MAX_EBACK];
    int           neback;
} COMP;

extern COMP *cc;

/* CSYM.C */
void        *c_pool( unsigned int size );       /* zeroed; "Out of memory" fails */
char        *c_pool_strn( const char *text, unsigned int len );
void         c_fail( const char *msg );         /* at the current token */
void         c_fail_at( int tp, const char *msg );
void         c_fail_tok( const char *msg );
void         c_expect_msg( const char *what );  /* "Expected: what" */
void         c_emit( int byte );
void         c_emit16( uint32_t val );
void         c_emit32( uint32_t val );
void         c_emitf( float val );
void         c_emitd( double val );
void         c_patch32( unsigned int at, uint32_t val );
unsigned int c_here( void );
unsigned int c_const_str( const char *text, unsigned int len );
void         c_op( int op );
void         c_op32( int op, uint32_t val );
void         c_rt( int routine );
void         c_push_l( int32_t val );
unsigned int c_jump( int op );                  /* -> where to patch */

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

void         scope_init( SCOPE *scope, int proc );
SCOPE       *scope_new( int proc );             /* from the pool, chained for freeing */
SYM         *sym_find( SCOPE *scope, const char *upper, int tkey, int kind );
SYM         *sym_add( SCOPE *scope, const char *upper, int tkey, int kind );
unsigned int scope_alloc( SCOPE *scope, unsigned int size, int is_static );
void         scope_note_str( SCOPE *scope, unsigned int off );
void         scope_note_arr( SCOPE *scope, unsigned int off );
void         scope_note_static( SCOPE *scope, unsigned int off, int is_array );
unsigned int type_size( QTYPE type );
int          type_default( SCOPE *scope, const char *upper );  /* TY_* by DEFtype */
SYM         *type_find( const char *upper );
int          rec_field( int rec, const char *upper, QTYPE *type, unsigned int *offset );
QTYPE        parse_as_type( int allow_any, int allow_fixed );  /* after AS */
const char  *type_name( QTYPE type );
unsigned int c_temp( QTYPE type, uint8_t *scope );  /* a hidden variable */

/* the variable named by the tokens at tp: found or made */
typedef struct {
    QTYPE        type;
    int          simple;        /* a scalar at scope:offset; nothing emitted */
    uint8_t      scope;
    unsigned int offset;
    SYM         *sym;
    int          is_array_whole; /* a() */
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
