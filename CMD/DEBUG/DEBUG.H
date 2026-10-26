/*
 * DEBUG.H - what DEBUG's modules share.
 *
 * TWO KINDS OF THING CAN BE IN FRONT OF DEBUG, and they are told apart
 * by what was loaded:
 *
 *   MODE_16   the work area: nothing, a data file, or a 16-bit .COM or
 *             .EXE.  A real-mode megabyte as 6.22's DEBUG saw it, with
 *             the work area at work_seg and the rest the machine's own
 *             memory, which can be read and not written.  Registers are
 *             16-bit.  Nothing here can be run.
 *   MODE_NAT  a native PM-DOS program, held by the kernel one level
 *             below DEBUG (21h/F1h AL=29h).  Its slot is one 32-bit
 *             segment, named by its two selectors; a segment that is
 *             neither is the first megabyte again, read only.  G, T and
 *             P run it.
 */
#ifndef DEBUG_H
#define DEBUG_H

#include "rt.h"
#include "sys.h"

#define MODE_16         0
#define MODE_NAT        1

typedef struct {
    u16 seg;
    u32 off;
} ADDR;

/* MAIN.C - the command loop and the parser */
extern int   mode;
extern DRX   cpu;               /* the registers, in either mode */
extern u16   work_seg;          /* MODE_16: where the work area starts */
extern u32   work_len;          /* ...and how long it is */
extern u8   *work_buf;
extern int   prog_held;         /* MODE_NAT: the kernel holds a program */
extern char  file_name[128];    /* N's name, for L and W */
extern int   file_kind;         /* KIND_* of file_name */
extern ADDR  next_dump;         /* where a bare D, U and A carry on */
extern ADDR  next_unasm;
extern ADDR  next_asm;

#define KIND_DATA       0
#define KIND_COM        1
#define KIND_EXE        2
#define KIND_HEX        3

extern const char *line;        /* the command being read */
extern const char *lp;          /* where in it */
extern int   prompt_len;        /* columns in front of it on the screen */

void  __cdecl cmd_abort( void );        /* back to the prompt (DOSINT.ASM) */
int   __cdecl dbg_setjmp( u32 *buf );   /* buf: three dwords */
void  __cdecl dbg_longjmp( u32 *buf, int value );
extern u32 *error_jmp;          /* non-NULL: syntax_error longjmps here */
void  syntax_error( void );     /* "^ Error" under lp, and back */
void  coded_error( const char *code );  /* "bf Error" and the like */
void  skip_blanks( void );
int   at_end( void );
void  need_end( void );
int   peek_upper( void );
u32   get_hex( int max_digits );
int   try_hex( u32 *val, int max_digits );
int   off_digits( void );       /* 4 in MODE_16, 8 in MODE_NAT */
void  get_addr( ADDR *addr, u16 def_seg );
int   try_addr( ADDR *addr, u16 def_seg );
void  get_range( ADDR *addr, u32 *len, u16 def_seg, u32 def_len, int need );
u32   get_list( u8 *buf, u32 max );
int   reg_seg_value( const char *name, u16 *val );
void  new_work( void );
void  positions_at_ip( void );
void  set_file_name( const char *text );

/* MEM.C - one address space or the other */
int   mem_get( ADDR addr, u8 *buf, u32 len, u8 *ok );
int   mem_read( ADDR addr, u8 *buf, u32 len );
int   mem_write( ADDR addr, const u8 *buf, u32 len );
int   mem_is_low( ADDR addr );
u32   mem_seg_end( ADDR addr );     /* the first offset past the segment */
void  addr_add( ADDR *addr, u32 n );  /* wraps at 64K in the first megabyte */
void  out_addr( ADDR addr );
int   addr_width( ADDR addr );
void  work_reset( void );

/* CMDS.C */
void  cmd_compare( void );
void  cmd_dump( void );
void  cmd_enter( void );
void  cmd_fill( void );
void  cmd_hex( void );
void  cmd_input( void );
void  cmd_load( void );
void  cmd_move( void );
void  cmd_name( void );
void  cmd_output( void );
void  cmd_search( void );
void  cmd_write( void );
void  cmd_ems( void );
void  cmd_help( void );
void  load_file( const ADDR *where );
void  quit( int code );

/* REGS.C */
void  cmd_register( void );
void  show_regs( void );
void  regs_reset( void );

/* RUN.C */
void  cmd_go( void );
void  cmd_trace( int proceed );
void  prog_release( void );

/* DIS.C - the disassembler */
#define DIS_MAX         16          /* the longest instruction there is */
typedef struct {
    u32   len;              /* bytes it takes */
    char  text[80];         /* mnemonic, padded, and operands */
    int   mem_seg;          /* a memory operand: its segment register, -1 none */
    u32   mem_off;          /* ...its effective offset */
    int   mem_size;         /* ...and its size in bytes */
    int   kind;             /* DK_* for T and P */
    u32   target;           /* DK_CALL, DK_JUMP: where it goes, if known */
} DISASM;

#define DK_OTHER        0
#define DK_CALL         1   /* CALL: P steps over it */
#define DK_INT          2   /* INT n, INTO: stepped over always */
#define DK_INT3         3
#define DK_LOOP         4   /* LOOPxx: P steps over it */
#define DK_REP          5   /* a REP prefix on a string instruction */
#define DK_EMUL         6   /* CLI, STI, IN, OUT: the kernel does these */
#define DK_JUMP         7
#define DK_PREFIX       8   /* 16-bit: a prefix on a line of its own */

void  disasm( ADDR addr, const u8 *code, int use32, DISASM *dis );
void  cmd_unassemble( void );
void  show_insn( ADDR addr, int with_mem );

/* ASM.C - the assembler */
void  cmd_assemble( void );

/* MSG.C */
extern const char msg_error[];
extern const char msg_ended[];
extern const char msg_nofile[];
extern const char msg_writing[];
extern const char msg_bytes[];
extern const char msg_noexe[];
extern const char msg_noname[];
extern const char msg_access[];
extern const char msg_nocreate[];
extern const char msg_diskfull[];
extern const char msg_rderr[];
extern const char msg_wrerr[];
extern const char msg_noems[];
extern const char msg_nomem[];
extern const char msg_norun[];
extern const char msg_noprog[];
extern const char msg_toobig[];
extern const char * const help_lines[];

#endif
