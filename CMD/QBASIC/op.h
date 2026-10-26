/*
 * OP.H - the instructions COMP.C writes and VM.C carries out, once,
 * included with OP() defined as wanted (QB.H makes the OP_* numbers,
 * the host's disassembler a table of names and operand shapes).
 *
 * The operand shape says what follows the opcode byte in the code:
 *   b  a byte          w  a 16-bit word      i  a 32-bit integer
 *   o  a 32-bit offset into data              a  a 32-bit code address
 *   f  a single        d  a double            c  a constant's number (32)
 *   n  ON...GOTO: a word count, a byte kind, then that many addresses
 *   r  a variable: a scope byte (SC_*) and a 32-bit offset
 *
 * The stack holds 8-byte cells.  INTEGER and LONG are both 32-bit
 * signed there; an INTEGER is only ever 16 bits wide in memory, and
 * the arithmetic on it checks the 16-bit range.  A SINGLE is a float
 * in its cell and a DOUBLE a double.  A string is a counted STR with a
 * reference held by the cell.
 */

/* constants */
OP( CI,         "i" )           /* push an INTEGER or LONG */
OP( CS,         "f" )           /* push a SINGLE */
OP( CD,         "d" )           /* push a DOUBLE */
OP( CSTR,       "c" )           /* push a constant string */
OP( POPN,       "" )            /* drop a number */
OP( POPS,       "" )            /* drop a string */
OP( DUP,        "" )            /* a number or an address, again */

/* addresses: where a variable is, pushed for the loads and stores */
OP( AG,         "o" )           /* the module's data + offset */
OP( AF,         "o" )           /* this call's frame + offset */
OP( AS,         "o" )           /* this procedure's static data + offset */
OP( AR,         "o" )           /* the address a parameter in the frame holds */
OP( AOFS,       "o" )           /* the address on the stack + offset: a field */
OP( AIDX,       "b" )           /* array slot, then n subscripts -> the element */
OP( AIDX1,      "" )            /* the same with one subscript */

/* through an address on the stack */
OP( LDI,        "" )
OP( LDL,        "" )
OP( LDS,        "" )
OP( LDD,        "" )
OP( LDSTR,      "" )
OP( LDFS,       "w" )           /* a fixed-length string of that length */
OP( STI,        "" )            /* address, value -> */
OP( STL,        "" )
OP( STS,        "" )
OP( STD,        "" )
OP( STSTR,      "" )
OP( STFS,       "w" )
OP( STREC,      "o" )           /* address, address of the value, bytes */

/* the common cases in one step: the module's data and the frame */
OP( GLDI,       "o" )
OP( GLDL,       "o" )
OP( GLDS,       "o" )
OP( GLDD,       "o" )
OP( GLDSTR,     "o" )
OP( GSTI,       "o" )
OP( GSTL,       "o" )
OP( GSTS,       "o" )
OP( GSTD,       "o" )
OP( GSTSTR,     "o" )
OP( FLDI,       "o" )
OP( FLDL,       "o" )
OP( FLDS,       "o" )
OP( FLDD,       "o" )
OP( FLDSTR,     "o" )
OP( FSTI,       "o" )
OP( FSTL,       "o" )
OP( FSTS,       "o" )
OP( FSTD,       "o" )
OP( FSTSTR,     "o" )

/* conversions (I and L are the same cell; L2I checks the range) */
OP( L2I,        "" )
OP( L2S,        "" )
OP( L2D,        "" )
OP( S2D,        "" )
OP( D2S,        "" )
OP( S2I,        "" )
OP( S2L,        "" )
OP( D2I,        "" )
OP( D2L,        "" )
OP( UNDER_S2L,  "" )            /* the number under the top one, to LONG */
OP( UNDER_D2L,  "" )
OP( UNDER_L2S,  "" )
OP( UNDER_L2D,  "" )
OP( UNDER_S2D,  "" )

/* arithmetic */
OP( ADDI,       "" )
OP( SUBI,       "" )
OP( MULI,       "" )
OP( NEGI,       "" )
OP( IDIVI,      "" )
OP( MODI,       "" )
OP( ADDL,       "" )
OP( SUBL,       "" )
OP( MULL,       "" )
OP( NEGL,       "" )
OP( IDIVL,      "" )
OP( MODL,       "" )
OP( ADDS,       "" )
OP( SUBS,       "" )
OP( MULS,       "" )
OP( DIVS,       "" )
OP( NEGS,       "" )
OP( POWS,       "" )
OP( ADDD,       "" )
OP( SUBD,       "" )
OP( MULD,       "" )
OP( DIVD,       "" )
OP( NEGD,       "" )
OP( POWD,       "" )
OP( CAT,        "" )

/* comparisons -> INTEGER -1 or 0; the byte is CC_* */
OP( CMPL,       "b" )
OP( CMPS,       "b" )
OP( CMPD,       "b" )
OP( CMPSTR,     "b" )

/* logic, on 32-bit cells */
OP( NOT,        "" )
OP( AND,        "" )
OP( OR,         "" )
OP( XOR,        "" )
OP( EQV,        "" )
OP( IMP,        "" )

/* flow */
OP( STMT,       "i" )           /* a statement starts: its STMTLOC */
OP( JMP,        "a" )
OP( JZ,         "a" )
OP( JNZ,        "a" )
OP( GOSUB,      "a" )
OP( RETURN,     "" )
OP( RETURNTO,   "a" )
OP( ONGO,       "n" )
OP( CALL,       "w" )           /* a procedure's number; its arguments' addresses */
OP( RET,        "" )            /* the end of a SUB, or EXIT SUB */
OP( FRET,       "" )            /* the end of a FUNCTION or DEF FN: its value */
OP( FORI,       "rrra" )        /* var, limit, step, out: the first test */
OP( FORL,       "rrra" )
OP( FORS,       "rrra" )
OP( FORD,       "rrra" )
OP( NEXTI,      "rrra" )        /* var, limit, step, back to the body */
OP( NEXTL,      "rrra" )
OP( NEXTS,      "rrra" )
OP( NEXTD,      "rrra" )
OP( SWAP,       "o" )           /* two addresses, that many bytes */
OP( RT,         "w" )           /* a runtime routine: RT.H's number */
OP( END,        "" )
OP( STOP,       "" )
OP( SYSTEM,     "" )
OP( ONERR,      "a" )           /* 0 = ON ERROR GOTO 0 */
OP( RESUME,     "" )
OP( RESUMENEXT, "" )
OP( RESUMETO,   "a" )
OP( NOP,        "" )
OP( IMMRET,     "" )           /* the end of an Immediate line's code */
OP( IMMGOTO,    "a" )          /* GOTO in an Immediate line: the program goes on there */
OP( IMMRETURN,  "" )           /* RETURN in an Immediate line: the same, from the GOSUB */

/* an array element passed to a SUB or FUNCTION goes as a copy, written
   back when the call returns - into the array the slot holds then */
OP( AIDXR,      "b" )           /* as AIDX, leaving the slot and the element's byte offset */
OP( AIDXR1,     "" )            /* the same with one subscript */
OP( EREF,       "ob" )          /* slot, offset, copy's and note's addresses: copy in (bytes, is a string) */
OP( EBACK,      "ob" )          /* copy's and note's addresses: the copy back into the element */
OP( EXCH,       "" )            /* the top two cells change places */
OP( MODEND,     "" )            /* the module's last line run past: END, or "No RESUME" in a handler */
