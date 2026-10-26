/*
 * QB.H - what the parts of QBASIC share.
 *
 * THE INTERPRETER (none of it knows about the editor):
 *   LEX.C    a line into tokens
 *   LIST.C   tokens back into a line the way QBasic shows it; the
 *            spelling of every name
 *   COMP.C   the program's text into code for the VM: statements
 *   CEXPR.C  ...expressions, variables and their types
 *   VM.C     carrying the code out: the stack, calls, errors, events
 *   NUM.C    numbers to text and back, PRINT USING
 *   QSTR.C   string values and the string functions
 *   RTLIB.C  the other runtime routines: numbers, data, arrays, memory
 *   CON.C    the program's own screen in text: PRINT, INPUT, the keys
 *   GFX.C    the graphics modes: a picture in memory and its drawing
 *   DRAW.C   CIRCLE, PAINT, DRAW, GET and PUT
 *   SND.C    SOUND, PLAY and BEEP
 *   FILE.C   OPEN and everything done with what it opened
 *   MEM.C    the heap
 *   FPU.ASM  the coprocessor's own instructions: rounding, the
 *            functions, decimal digits
 *
 * THE PROGRAM TEXT: PROG.C (a file split into its module and its
 * procedures, and put back together) on DOC.C's lines.
 *
 * THE ENVIRONMENT: MAIN.C, VIEW.C, SCR.C, UI.C, DLGS.C, HELP.C - EDIT's
 * screen, windows, menus and dialogs, with QBasic's commands.
 */
#ifndef QB_H
#define QB_H

#include "types.h"
#include "rt.h"
#include "sys.h"

typedef short s16;

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */
enum {
    TY_NONE, TY_INT, TY_LONG, TY_SINGLE, TY_DOUBLE,
    TY_STR,                     /* a variable-length string */
    TY_FSTR,                    /* STRING * n: the length is the argument */
    TY_REC,                     /* a TYPE: its number is the argument */
    TY_ANY                      /* DECLARE's AS ANY */
};

typedef u32 QTYPE;
#define MKTYPE( kind, arg )     ((QTYPE)(kind) | ((QTYPE)(arg) << 8))
#define TKIND( t )              ((int)((t) & 0xFF))
#define TARG( t )               ((u32)(t) >> 8)
#define IS_NUMT( t )            (TKIND( t ) >= TY_INT && TKIND( t ) <= TY_DOUBLE)
#define IS_STRT( t )            (TKIND( t ) == TY_STR || TKIND( t ) == TY_FSTR)

#define STR_MAX         32767   /* the longest a string may be */
#define NAME_MAX        40      /* the longest a name may be */
#define MAX_DIMS        60

/* ------------------------------------------------------------------ */
/* errors: QBasic's numbers, which ERR gives and ERROR takes           */
/* ------------------------------------------------------------------ */
enum {
    E_NEXT_NO_FOR = 1,  E_SYNTAX = 2,       E_RETURN_NO_GOSUB = 3,
    E_OUT_OF_DATA = 4,  E_ILLEGAL_CALL = 5, E_OVERFLOW = 6,
    E_OUT_OF_MEMORY = 7, E_LABEL_UNDEF = 8, E_SUBSCRIPT = 9,
    E_DUPLICATE = 10,   E_DIV_ZERO = 11,    E_ILLEGAL_DIRECT = 12,
    E_TYPE_MISMATCH = 13, E_OUT_OF_STRSPACE = 14, E_STR_COMPLEX = 16,
    E_CANT_CONTINUE = 17, E_FUNC_UNDEF = 18, E_NO_RESUME = 19,
    E_RESUME_NO_ERR = 20, E_DEV_TIMEOUT = 24, E_DEV_FAULT = 25,
    E_FOR_NO_NEXT = 26, E_OUT_OF_PAPER = 27, E_OUT_OF_STACK = 28,
    E_WHILE_NO_WEND = 29,
    E_WEND_NO_WHILE = 30, E_DUP_LABEL = 33, E_SUB_UNDEF = 35,
    E_ARG_COUNT = 37,   E_ARRAY_UNDEF = 38, E_VAR_REQUIRED = 40,
    E_FIELD_OVERFLOW = 50, E_INTERNAL = 51, E_BAD_FILENUM = 52,
    E_FILE_NOT_FOUND = 53, E_BAD_MODE = 54, E_FILE_OPEN = 55,
    E_FIELD_ACTIVE = 56, E_DEV_IO = 57,     E_FILE_EXISTS = 58,
    E_BAD_RECLEN = 59,  E_DISK_FULL = 61,   E_INPUT_PAST_END = 62,
    E_BAD_RECNUM = 63,  E_BAD_FILENAME = 64, E_TOO_MANY_FILES = 67,
    E_DEV_UNAVAIL = 68, E_COMM_OVERFLOW = 69, E_PERMISSION = 70,
    E_DISK_NOT_READY = 71, E_DISK_MEDIA = 72, E_ADVANCED = 73,
    E_RENAME_DISKS = 74, E_PATH_FILE = 75,  E_PATH_NOT_FOUND = 76
};

const char *err_message( int code );        /* "Overflow"... */

/* ------------------------------------------------------------------ */
/* string values                                                       */
/* ------------------------------------------------------------------ */
/* A string is counted and shared: every variable and every stack cell
   that holds it holds a reference.  NULL is the empty string. */
typedef struct STR {
    u32  ref;
    u32  len;
    char text[4];               /* "len" bytes; no NUL */
} STR;

#define QS_PERM     0x40000000UL    /* a constant's count: never freed */

STR  *qs_new( u32 len );            /* ref 1, the text left to be filled */
STR  *qs_make( const char *text, u32 len );
STR  *qs_cstr( const char *text );
STR  *qs_perm( const char *text, u32 len );     /* a constant */
STR  *qs_try_make( const char *text, u32 len, int *ok );  /* no vm_error */
#define QS_LEN( s )     ((s) ? (s)->len : 0UL)
#define QS_TEXT( s )    ((s) ? (s)->text : "")
void  qs_release( STR *s );
u32   qs_bytes( void );
STR  *qs_retain( STR *s );
STR  *qs_cat( STR *lhs, STR *rhs );         /* takes both references */
int   qs_cmp( STR *lhs, STR *rhs );         /* <0, 0, >0 */
STR  *qs_unique( STR *s );                  /* a copy only this holder has */

/* ------------------------------------------------------------------ */
/* the machine the code runs on                                        */
/* ------------------------------------------------------------------ */
typedef union {
    s32    l;                   /* INTEGER and LONG */
    float  s;
    double d;
    STR   *str;
    void  *ptr;
    u32    u;
} CELL;

/* where a variable lives */
enum { SC_GLOBAL, SC_FRAME, SC_STATIC, SC_REF };

/* comparisons, in OP_CMPx's byte */
enum { CC_EQ, CC_NE, CC_LT, CC_GT, CC_LE, CC_GE };

/* an array: QBasic's are column-major - the first subscript is the one
   that moves fastest through memory - which GET and PUT, BSAVE and a
   program walking VARPTR all see */
typedef struct ARR {
    QTYPE type;                 /* of one element */
    u32   esize;                /* bytes in one */
    u32   count;                /* elements in all */
    u8    ndims;
    u8    flags;                /* AF_* */
    u16   vseg;                 /* VARSEG's answer, given out on asking */
    u8   *data;
    s32   lb[1];                /* ndims lower bounds, then ndims extents */
} ARR;

#define AF_STATIC   0x01        /* ERASE clears it rather than freeing it */
#define AF_IMPLICIT 0x02        /* made by using it: 0 (or BASE) to 10 */

#define ARR_UB( arr, dim )  ((arr)->lb[dim] + (arr)->lb[(arr)->ndims + (dim)] - 1)

/* a TYPE */
typedef struct RECDEF {
    char  name[NAME_MAX + 1];
    u32   size;
    int   nfields;
    struct RFIELD {
        char  name[NAME_MAX + 1];
        QTYPE type;
        u32   offset;
    } *fields;
} RECDEF;

/* a parameter */
typedef struct {
    QTYPE type;
    u8    array;                /* a() */
    u8    ndims;                /* 0 = not known */
} PARAM;

/* a procedure, compiled */
enum { PK_SUB, PK_FUNC, PK_DEFFN };

typedef struct PROC {
    char   name[NAME_MAX + 2];  /* as spelled, with a FUNCTION's suffix */
    u8     kind;
    u8     is_static;           /* SUB x STATIC */
    u8     defined;             /* it has a body */
    u8     declared;            /* a DECLARE names it */
    QTYPE  rettype;             /* FUNCTION and DEF FN */
    int    nparams;
    PARAM *params;
    u32    entry;               /* in the code */
    u32    frame_size;          /* parameters, locals, temporaries */
    u32    static_size;
    u8    *static_data;         /* the run's */
    u32    ret_off;             /* where the value is, in the frame */
    u32   *rel_str;             /* frame offsets to release at the end */
    u32    nrel_str;
    u32   *rel_arr;
    u32    nrel_arr;
    u32   *srel_str;            /* static offsets, let go of when the run ends */
    u32    nsrel_str;
    u32   *srel_arr;
    u32    nsrel_arr;
    u32   *imp_arr;             /* frame arrays made by being used */
    QTYPE *imp_type;
    u8    *imp_dims;
    u32    nimp_arr;
    int    text;                /* the program text it came from */
    u32    line;                /* ...and the line of its SUB or FUNCTION */
    u32    first_stmt;          /* its statements' locations */
} PROC;

/* one statement's place in the text: the IDE's cursor goes there */
typedef struct {
    u32  pc, end_pc;            /* its code */
    u32  line;                  /* in the text */
    u16  text;                  /* which procedure's text, 0 the module */
    u16  col, len;              /* where on the line */
    s32  lineno;                /* the line number before it, for ERL */
    u16  proc;                  /* 0 = the module, else 1 + the PROC */
    u16  flags;                 /* SL_* */
} STMTLOC;

#define SL_BREAK    0x01        /* a breakpoint's line */
#define SL_HERE     0x02        /* F7's line: a stop there, once */

/* a DATA item: its text as written, and where it is written, for the
   error a READ of it makes */
typedef struct {
    char *text;
    u16   len;
    u8    quoted;
    u16   in_text;              /* 0 the module, else the procedure's text */
    u16   col;
    u32   line;
} DATAITEM;

/* an array the run makes before the first statement */
typedef struct {
    u8    scope;                /* SC_GLOBAL or SC_STATIC */
    u16   proc;                 /* for SC_STATIC */
    u32   offset;
    QTYPE type;
    u32   esize;
    u8    ndims;
    u8    flags;
    s32  *bounds;               /* lo, hi pairs */
} ARRINIT;

/* a variable of the blank COMMON, in the order the COMMON statements name
   them: what CHAIN hands to the next program */
typedef struct {
    u32   offset;               /* in the module's data */
    QTYPE type;
    u32   size;                 /* its bytes there (4 for an array's slot) */
    u8    is_array;
} COMMONVAR;

/* the program, compiled */
typedef struct {
    u8      *code;
    u32      code_len;
    STR    **consts;
    u32      nconsts;
    STMTLOC *locs;
    u32      nlocs;
    PROC    *procs;
    int      nprocs;
    RECDEF  *recs;
    int      nrecs;
    u32      gsize;             /* the module's data */
    u32     *grel_str;          /* string slots in it */
    u32      ngrel_str;
    u32     *grel_arr;          /* array slots */
    u32      ngrel_arr;
    ARRINIT *arrinit;
    u32      narrinit;
    COMMONVAR *common;
    u32      ncommon;
    DATAITEM *data;
    u32      ndata;
    u32      main_entry;
    u8       uses_events;
    s32      option_base;
    struct COMPSYMS *syms;      /* the names, for the Immediate window */
} PROGRAM;

/* why a run stopped */
enum {
    RUN_END,                    /* END, SYSTEM, or the last statement */
    RUN_SYSTEM,                 /* SYSTEM: QBASIC /RUN ends with it */
    RUN_STOP,                   /* STOP */
    RUN_BREAK,                  /* Ctrl+Break */
    RUN_STEP,                   /* a statement done, as asked */
    RUN_BREAKPOINT,             /* a breakpoint's statement is next */
    RUN_ERROR,                  /* an error nothing handled */
    RUN_CHAIN,                  /* CHAIN or RUN "file": vm.chain_to */
    RUN_IMMEDIATE,              /* an Immediate line is done */
    RUN_IMMGOTO                 /* ...and said GOTO or RETURN: vm.pc is where the program goes on */
};

/* how a run goes */
enum { GO_RUN, GO_STEP, GO_PSTEP };

/* where PRINT is writing, and its PRINT USING format: a FUNCTION that
   PRINTs, called in the middle of a PRINT, must not take them over */
typedef struct {
    int   chan;
    STR  *fmt;
    int   pos, any;
} PRINTSTATE;

void  con_print_push( PRINTSTATE *ps );  /* kept, and PRINT's own made fresh */
void  con_print_pop( PRINTSTATE *ps );   /* back as they were */

typedef struct FRAME {
    PROC *proc;
    u8   *ret_pc;
    u8   *fdata, *sdata;        /* the caller's */
    u32   gosub_base;           /* the GOSUBs made before the call */
    u8   *mine;                 /* this call's frame */
    u32   mine_size;
    u32   stmt;                 /* the caller's statement */
    CELL *stmt_sp;              /* ...and where its stack began */
    PRINTSTATE print;           /* the caller's PRINT, in the middle of which it may be */
} FRAME;

typedef struct {
    u8   *pc;
    u32   frame;                /* the frame depth it was made at */
    u8    event;                /* 0, or the event whose handler it is */
    u8    which;
} GOSUBREC;

typedef struct {
    PROGRAM *prog;
    u8      *code;
    u8      *pc;
    CELL    *sp;                /* the next free cell */
    CELL    *stack, *stack_end;
    CELL    *stmt_sp;           /* where the running statement's cells begin */
    u8      *stag;              /* 1 where a cell holds a string */
    u8      *gdata;
    u8      *fdata;
    u8      *sdata;
    FRAME   *frames;
    u32      nframes, frames_cap;
    GOSUBREC *gosubs;
    u32      ngosub, gosub_cap;
    u8      *farena;            /* the frames' memory, a stack */
    u32      ftop, fcap;
    u32      stmt;              /* the statement running */
    u32      data_next;         /* the DATA item READ takes next */
    u8      *on_error;          /* ON ERROR GOTO's handler, or NULL */
    int      in_handler;        /* between the error and RESUME */
    int      err;               /* ERR */
    s32      erl;               /* ERL */
    u32      err_stmt;          /* the statement that failed */
    u32      err_nframes;
    int      tron;              /* Debug's Trace On */
    int      poll;              /* statements until the next look round */
    int      go;                /* GO_* */
    int      skip_stop;         /* the statement a run resumes at is not a stop */
    u32      step_frames;       /* PSTEP: the depth stepping began at */
    int      stopped;           /* RUN_* asked for by a routine */
    int      running;           /* a program is started and not ended */
    int      fatal;             /* the error of RUN_ERROR */
    u32      fatal_stmt;
    u32      gcap;              /* gdata's bytes, the spare included */
    int      immediate;         /* an Immediate line runs: no breakpoints */
    int      imm_onerr;         /* ...and set ON ERROR, for the program */
    u32      clears;            /* CLEARs run (an Immediate line's CLEAR
                                   leaves no program to go back to) */
    int      dead;              /* CLEAR could not remake the variables */
    const DATAITEM *fatal_data; /* a bad DATA item: where to show the error */
    int      in_event;          /* events stopped while a handler runs */
    char     chain_to[260];
    u32      chain_at;          /* RUN n: where in the code, + 1 (0: the top) */
    int      chain_keep;        /* CHAIN: the files and the COMMON go on */
} VM;

extern VM vm;
extern int opt_mbf;             /* QBASIC /MBF: MKS$, MKD$, CVS and CVD in the old format */
extern int ide_trace;           /* the IDE lights each statement after TRON */

/* the routines OP_RT calls */
enum {
#define RT( name ) RT_##name,
#include "rtfn.h"
#undef RT
    RT_COUNT
};

/* ...and the routines themselves */
#define RT( name ) void rtf_##name( void );
#include "rtfn.h"
#undef RT

void  file_field_changed( STR **var );  /* LSET into a FIELD's variable */
void  file_forget_vars( const u8 *lo, const u8 *hi );  /* that memory is gone */

/* the instructions */
enum {
#define OP( name, shape ) OP_##name,
#include "op.h"
#undef OP
    OP_COUNT
};

int   vm_begin( PROGRAM *prog );        /* a fresh run; 0 = no memory */
int   vm_go( int how );                 /* -> RUN_* */
int   vm_immediate( u32 entry, u32 new_arrays ); /* -> RUN_*, -1 no memory */
void  vm_finish( void );            /* END: files closed, variables kept */
void  vm_common_take( void );       /* CHAIN: the COMMON out, the files kept open */
void  vm_common_give( void );       /* ...into the next program's COMMON */
void  vm_common_drop( void );       /* ...or let go, when it never runs */
void  vm_end( void );                   /* files closed, memory back */
void  vm_set_next( u32 stmt );          /* Set Next Statement */
void  vm_error( int code );             /* a runtime error: never returns */
void  vm_poll_now( void );              /* look at the keys and events now */
int   event_key_armed( int n );        /* KEY(n) traps its key now */
u32   vm_current_stmt( void );

/* pushing and popping, for the runtime's routines */
#define POP_L()     ((--vm.sp)->l)
#define POP_S()     ((--vm.sp)->d)      /* a SINGLE is a double on the stack */
#define POP_D()     ((--vm.sp)->d)
#define POP_A()     ((--vm.sp)->ptr)
#define POP_STR()   ((--vm.sp)->str)
/* functions, so that a POP in the argument is done before the push:
   PUSH_STR( f( POP_L() ) ) puts the string in the cell the number left */
void  push_l( s32 val );
void  push_d( double val );
void  push_str( STR *val );
double pop_single( void );         /* a SINGLE to show: too big is Overflow */
#define PUSH_L( v ) push_l( (s32)(v) )
#define PUSH_S( v ) push_d( (double)(v) )
#define PUSH_D( v ) push_d( (v) )
#define PUSH_STR( v ) push_str( (v) )

/* ------------------------------------------------------------------ */
/* tokens                                                              */
/* ------------------------------------------------------------------ */
enum {
    TK_EOL, TK_KW, TK_ID, TK_NUM, TK_STR, TK_REM, TK_DATA,
    TK_LINENUM, TK_LABEL,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_BSLASH, TK_CARET,
    TK_EQ, TK_LT, TK_GT, TK_LE, TK_GE, TK_NE,
    TK_LPAREN, TK_RPAREN, TK_COMMA, TK_SEMI, TK_COLON, TK_HASH, TK_DOT,
    TK_BAD
};

#define TF_UNARY    0x01        /* a sign, not a subtraction */
#define TF_TIGHT    0x02        /* no space round it: LINE's -( */
#define TF_HEX      0x04        /* a number written &H */
#define TF_OCT      0x08        /* ...&O */
#define TF_UNTERM   0x10        /* a string with no closing quote */
#define TF_QMARK    0x20        /* PRINT written as ? */
#define TF_FN       0x40        /* a name that starts with FN */
#define TF_APOS     0x80        /* a remark written with ' */

typedef struct {
    u8     kind;                /* TK_* */
    u8     flags;               /* TF_* */
    u16    kw;                  /* TK_KW: the KW_*; TK_ID: the suffix */
    u16    col, len;            /* in the line */
    u8     ntype;               /* TK_NUM, TK_ID: the type the suffix says */
    double num;
} TOKEN;

enum {
#define KW( id, text ) KW_##id,
#include "kw.h"
#undef KW
    KW_COUNT
};

/* -> the tokens, ending with TK_EOL; -1 and *err_col for a bad line */
int   lex_line( const char *text, u32 len, TOKEN *toks, int max, int *err_col,
                const char **err_msg );
const char *kw_name( int kw );
int   kw_lookup( const char *name, u32 len );   /* KW_*, or -1 */
int   lex_suffix_type( int ch );                /* '%' -> TY_INT ... */

/* ------------------------------------------------------------------ */
/* the program's text                                                  */
/* ------------------------------------------------------------------ */
/* cap == 0 with text: the line lives in one of the document's pools,
   packed as it was loaded, until an edit needs it to grow */
typedef struct {
    char *text;
    u16   len, cap;
    u8    flags;                /* LF_* */
} LINE;

#define LF_BREAK    0x01        /* a breakpoint */

typedef struct {
    LINE *lines;
    u32   count, cap;
    char **pools;
    u32   npools, pool_cap;
    char  pname[NAME_MAX + 2];  /* a procedure's name, "" for the module */
    u8    pkind;                /* PK_SUB or PK_FUNC */
    int   modified;
    u32   at_line, at_col;      /* where its cursor was, when no window shows it */
    u32   at_top, at_left;
} DOC;

#define MAX_LINE        4095    /* bytes in one line */

typedef struct {
    u32 line, col;              /* a byte position */
} POS;

extern int tab_size;

DOC  *doc_new( void );
void  doc_free( DOC *doc );
LINE *doc_line( DOC *doc, u32 line );
int   doc_set_line( DOC *doc, u32 line, const char *text, u32 len );
int   doc_insert_line( DOC *doc, u32 at, const char *text, u32 len );
int   doc_delete_lines( DOC *doc, u32 at, u32 count );
int   doc_append( DOC *doc, const char *text, u32 len );
int   doc_insert( DOC *doc, POS *at, const char *text, u32 len );   /* moves "at" */
int   doc_delete( DOC *doc, POS from, POS to );     /* 0: no memory, or too long a join */
u32   doc_joined_len( DOC *doc, POS from, POS to );
char *doc_extract( DOC *doc, POS from, POS to, u32 *len );          /* xfree it */
u32   doc_col_of( DOC *doc, LINE *line, u32 index );
u32   doc_index_of( DOC *doc, LINE *line, u32 col );

/* the whole program: the module and its procedures */
#define MAX_PROCS   512

typedef struct {
    DOC  *texts[MAX_PROCS + 1];  /* [0] the module */
    int   ntexts;
    char  path[PATH_MAX];       /* "" for Untitled */
    char  name[PATH_MAX];       /* what the title bar shows */
    int   modified;
} PROGTEXT;

extern PROGTEXT ptext;

void  prog_clear( void );
int   prog_load( const char *path );        /* DOS error, -1 no memory */
int   prog_save( const char *path );
int   prog_find( const char *name );        /* the text of a procedure, or -1 */
int   prog_add_proc( const char *name, int kind );  /* -> its text, -1 */
void  prog_delete_proc( int text );
int   prog_line_proc( const char *text, u32 len, char *name, int *kind );

/* ------------------------------------------------------------------ */
/* compiling                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    int   text;                 /* where: the text, the line, the column */
    u32   line;
    int   col, len;
    char  msg[80];
} CERR;

PROGRAM *comp_program( PROGTEXT *pt, CERR *err );   /* NULL with err set */
s32   comp_immediate( PROGRAM *prog, int proc, const char *src, u32 len, u32 gcap, CERR *err );
void  comp_immediate_done( PROGRAM *prog, u32 code_len, u32 nlocs );
#define IMM_DATA_SPARE  16384   /* module bytes the Immediate window's names may add */
#define IMM_CODE_SPARE  32768   /* code bytes one Immediate line may take */
void  comp_free( PROGRAM *prog );
int   comp_check_line( PROGTEXT *pt, int text, u32 line, const char *src, u32 len,
                       char *out, u32 outmax, CERR *err );
                        /* 1 = fine and "out" is the line QBasic would show */

/* LIST.C */
int   list_line( const char *src, u32 len, TOKEN *toks, char *out, u32 max );  /* -1: too long */
void  names_reset( void );
int   names_learn( const char *name, u32 len );     /* 1 = its spelling changed */
const char *names_spelling( const char *name, u32 len );

/* ------------------------------------------------------------------ */
/* numbers                                                             */
/* ------------------------------------------------------------------ */
int   num_fmt_l( s32 v, char *buf );       /* "-5", "5": no leading space */
int   num_fmt_s( double v, char *buf );  /* a SINGLE: 7 digits */
int   num_fmt_d( double v, char *buf );
int   num_fmt_lit( double v, int type, char *buf );     /* for the listing */
/* |v| as "prec" significant digits (trailing zeros taken off) -> how many;
   *exp10 is the power of ten of the first */
int   num_sig_digits( double v, int prec, char *digits, int *exp10 );
int   dec_digits( double v, int prec, char *digits, int *exp10 );   /* BIGDEC.C */
double dec_build( u32 lo, u32 hi, int scale );
double num_sng( double v );           /* rounded to a SINGLE */
/* VAL's reading of a number: *used = characters taken */
double num_parse( const char *text, u32 len, u32 *used );
/* a number as INPUT and READ take it: 0 = not one */
int   num_scan( const char *text, u32 len, double *val, int *type );

int   fpu_present( void );
double __cdecl fp_floor( double v );
double __cdecl fp_trunc( double v );
double __cdecl fp_round( double v );               /* to the nearest, ties to even */
double __cdecl fp_sin( double v );
double __cdecl fp_cos( double v );
double __cdecl fp_tan( double v );
double __cdecl fp_atn( double v );
double __cdecl fp_exp( double v );
double __cdecl fp_log( double v );
double __cdecl fp_sqrt( double v );
double __cdecl fp_pow( double base, double power );
double __cdecl fp_powi( double base, int n );       /* a whole power */
double __cdecl fp_scale( double a, double b, double f );        /* (a - b) * f */
double __cdecl fp_scale_round( double a, double b, double f );  /* ...rounded */
double __cdecl fp_offset( double a, double b, double g );       /* a + b * g */
void  __cdecl fp_init( void );

#define NO_RECORD       ((s32)0x80000000UL)     /* GET and PUT with the record left out */
#define DBL_BAD( v )    ((((u32 *)&(v))[1] & 0x7FF00000UL) == 0x7FF00000UL)
#define SNG_MAX         3.402823466e38

/* ------------------------------------------------------------------ */
/* the program's screen                                                */
/* ------------------------------------------------------------------ */
void  con_reset( void );            /* SCREEN 0, WIDTH 80, as at the start */
void  con_putc( int ch );
void  con_write( const char *text, u32 len );
void  con_newline( void );
int   con_col( void );              /* 1-based, as POS(0) */
int   con_row( void );
int   con_width( void );
void  con_flush( void );            /* what changed, onto the screen */
void  con_show( void );             /* the whole output screen, again */
void  con_exit( void );             /* ...as DOS gets it back */
void  con_show_on_output( void );   /* an Immediate line: the editor stays till output */
int   con_output_shown( void );     /* ...and whether the output screen came up */
int   con_key( int wait );          /* the next key (scan << 8 | char), or -1 */
int   con_key_waiting( void );
void  con_flush_keys( void );
void  con_poll_keys( void );        /* the keyboard into the queue; Break */
int   con_last_scan( void );
void  con_beep( void );
int   con_read_line( char *buf, int max, int is_input );    /* INPUT's editing */
void  con_cursor_visible( int on );
void  con_save_ide( void );         /* the text screen the IDE drew */
void  con_capture_dos( void );      /* the screen QBASIC started on */
void  con_text_then_error( STR *text, int code );
void  con_end_prompt( const char *text );      /* the run over: a word at the bottom */

/* graphics: GFX.C */
int   gfx_mode( void );             /* the SCREEN mode, 0 for text */
int   gfx_set( int mode, int apage, int vpage, int sw, int mask );
void  gfx_text_cell( int row, int col, int ch, int attr );
void  gfx_scroll_text( int top, int bottom, int attr );
void  gfx_clear_text( int top, int bottom, int attr );
void  gfx_flush( void );
int   gfx_dirty( void );            /* drawn since the last flush */
void  gfx_restore( void );          /* back in text for the IDE */
void  gfx_reset( void );            /* a run starts: no graphics mode at all */
int   gfx_draw_on_visible( void );     /* -> the active page, for gfx_draw_on */
void  gfx_draw_on( int page );
void  draw_reset( void );           /* ...and DRAW's angle, turn and scale */
void  gfx_show( void );             /* the program's graphics again */
int   gfx_text_rows( void );
int   gfx_text_cols( void );
void  gfx_cls( int how );
int   gfx_view_on( void );          /* a graphics VIEW is set */

/* a DRAW or PLAY string being read */
typedef struct {
    const char *text;
    u32 len, pos;
} MACRO;

int   macro_number( MACRO *cur, double *val, int allow_sign );
STR **mem_varptr_string( const u8 *bytes, QTYPE *type, void **addr );

/* sound */
void  snd_service( void );          /* the music queue, played on */
void  snd_stop( void );
int   snd_queued( void );            /* notes waiting in the background */
void  snd_beep( void );

/* files */
int   file_close_all( void );         /* -> 0, or the first write error */
int   file_flush_all( void );
void  file_raise( int rc );           /* rc not 0: its error */
void  file_reset_state( void );

/* the machine the run sees: the first megabyte as QBasic's programs
   expect it */
void  mem_reset_segs( void );
void  mem_forget( const u8 *lo, const u8 *hi );   /* that memory is gone */
u32   type_bytes( QTYPE type );          /* an element's size, at run time */
void  vm_error_at( int code, u32 stmt );
void  vm_error_data( int code, const DATAITEM *item );
void  vm_break( void );                 /* Ctrl+Break: stop, never returns */

#endif
