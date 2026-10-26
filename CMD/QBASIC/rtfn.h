/*
 * RTFN.H - the runtime's routines that OP_RT calls, once, included with
 * RT() defined as wanted: QB.H makes the RT_* numbers, VM.C the table
 * of entry points.  Each routine takes its arguments off the stack (the
 * first pushed is the deepest) and leaves its result there.
 *
 * In the comments: L is an INTEGER or LONG cell, S a SINGLE, D a
 * DOUBLE, $ a string, A an address, and "->" what is left behind.
 */

/* strings */
RT( LEFT )          /* $ L -> $ */
RT( RIGHT )         /* $ L -> $ */
RT( MID2 )          /* $ L -> $ */
RT( MID3 )          /* $ L L -> $ */
RT( LENSTR )        /* $ -> L */
RT( INSTR2 )        /* $ $ -> L */
RT( INSTR3 )        /* L $ $ -> L */
RT( LCASE )         /* $ -> $ */
RT( UCASE )
RT( LTRIM )
RT( RTRIM )
RT( SPACE )         /* L -> $ */
RT( STRINGN )       /* L L -> $ */
RT( STRINGS )       /* L $ -> $ */
RT( CHR )           /* L -> $ */
RT( ASC )           /* $ -> L */
RT( STRL )          /* L -> $ */
RT( STRS )          /* S -> $ */
RT( STRD )          /* D -> $ */
RT( VAL )           /* $ -> D */
RT( HEX )           /* L -> $ */
RT( OCT )
RT( MKI )           /* L -> $ */
RT( MKL )
RT( MKS )           /* S -> $ */
RT( MKD )           /* D -> $ */
RT( MKSMBF )        /* S -> $ */
RT( MKDMBF )        /* D -> $ */
RT( CVI )           /* $ -> L */
RT( CVL )
RT( CVS )           /* $ -> S */
RT( CVD )           /* $ -> D */
RT( CVSMBF )        /* $ -> S */
RT( CVDMBF )        /* $ -> D */
RT( MIDSETV )       /* A L L $ ->   (length -1: as long as the source) */
RT( MIDSETF )       /* A L(width) L L $ -> */
RT( LSETV )         /* A $ -> */
RT( LSETF )         /* A L(width) $ -> */
RT( RSETV )
RT( RSETF )
RT( LSETREC )       /* A L A L -> : a record into a record, cut or padded */

/* numbers */
RT( ABSL )          /* L -> L (INTEGER's range checked by the caller) */
RT( ABSS )
RT( ABSD )
RT( SGNL )          /* L -> L */
RT( SGNS )          /* S -> L */
RT( SGND )          /* D -> L */
RT( INTS )          /* S -> S: the floor */
RT( INTD )
RT( FIXS )          /* S -> S: towards zero */
RT( FIXD )
RT( SQRS )
RT( SQRD )
RT( SINS )
RT( SIND )
RT( COSS )
RT( COSD )
RT( TANS )
RT( TAND )
RT( ATNS )
RT( ATND )
RT( EXPS )
RT( EXPD )
RT( LOGS )
RT( LOGD )
RT( RND0 )          /* -> S */
RT( RND1 )          /* S -> S */
RT( RANDOMIZE )     /* D -> */
RT( RANDASK )       /* -> : asks for the seed */
RT( TIMER )         /* -> S */

/* the screen and the keyboard */
RT( PRCHAN )        /* L -> : 0 the screen, -1 the printer, n a file */
RT( PRNUML )        /* L -> */
RT( PRNUMS )
RT( PRNUMD )
RT( PRSTR )         /* $ -> */
RT( PRCOMMA )
RT( PRTAB )         /* L -> */
RT( PRSPC )         /* L -> */
RT( PREOL )         /* -> : the end of the line */
RT( PRUSING )       /* $ -> : the format for the items after it */
RT( PRUNUMS )       /* S -> */
RT( PRUNUMD )       /* D -> */
RT( PRUSTR )        /* $ -> */
RT( PRUEND )        /* L -> : 1 = and end the line */
RT( WRNUML )        /* L -> : WRITE's forms */
RT( WRNUMS )
RT( WRNUMD )
RT( WRSTR )
RT( WRSEP )
RT( INKEY )         /* -> $ */
RT( INPUTS )        /* L -> $ : INPUT$(n) */
RT( INPUTSF )       /* L L -> $ : INPUT$(n, #f) */
RT( INBEGIN )       /* $ L $ -> : prompt, flags, the variables' types */
RT( INFILE )        /* L -> : INPUT #f */
RT( INITEMI )       /* A -> */
RT( INITEML )
RT( INITEMS )
RT( INITEMD )
RT( INITEMSTR )
RT( INITEMFS )      /* A L -> */
RT( LINEIN )        /* $ L A L -> : prompt, flags, the variable, its width (0 = var) */
RT( LINEINF )       /* L A L -> : #f, the variable, its width */
RT( CLS )           /* L -> : -1 for none given */
RT( LOCATE )        /* L L L L L L -> : the mask of those given first */
RT( COLOR )         /* L L L L -> */
RT( WIDTH )         /* L L L -> */
RT( WIDTHDEV )      /* $ L -> */
RT( WIDTHF )        /* L L -> */
RT( VIEWPRINT )     /* L L L -> : given?, top, bottom */
RT( SCREENS )       /* L L L L L -> : mask, mode, colour switch, apage, vpage */
RT( SCREENF )       /* L L L L -> L : row, column, colour flag, given? */
RT( POS )           /* L -> L */
RT( CSRLIN )        /* -> L */
RT( LPOS )          /* L -> L */
RT( KEYONOFF )      /* L -> : 0 off, 1 on, 2 list */
RT( KEYSET )        /* L $ -> */
RT( BEEP )
RT( SOUND )         /* D L -> */
RT( PLAY )          /* $ -> */
RT( PLAYF )         /* L -> L */

/* graphics */
RT( PSET )          /* D D L L -> : x y colour flags (1 STEP, 2 colour given, 4 PRESET) */
RT( LINE )          /* D D D D L L L -> : x1 y1 x2 y2 colour style flags */
RT( CIRCLE )        /* D D D L D D D L -> : x y r colour start end aspect flags */
RT( PAINT )         /* D D L $ L $ L -> : x y paint tile? border background flags */
RT( DRAW )          /* $ -> */
RT( GGET )          /* D D D D A -> : the corners and the array's element */
RT( GPUT )          /* D D A L L -> : x y element action flags */
RT( VIEW )          /* D D D D L L L -> */
RT( WINDOW )        /* D D D D L -> */
RT( PALETTE )       /* L L L -> : mask, attribute, colour */
RT( PALUSING )      /* A L -> : the element and its type */
RT( PCOPY )         /* L L -> */
RT( POINT1 )        /* L -> S */
RT( POINT2 )        /* D D -> L */
RT( PMAP )          /* D L -> S */

/* files and devices */
RT( OPEN )          /* $ L L L L L -> : name mode access lock number length */
RT( OPENOLD )       /* $ L $ L -> : mode letter, number, name, length */
RT( CLOSE )         /* L -> */
RT( CLOSEALL )
RT( SEEKS )         /* L L -> */
RT( SEEKF )         /* L -> L */
RT( FGET )          /* L L A L L -> : #f record address width type */
RT( FPUT )
RT( FIELD )         /* L ... -> : #f, count, then (width, address) pairs */
RT( LOCK )          /* L L L L -> */
RT( EOF )           /* L -> L */
RT( LOF )
RT( LOC )
RT( FREEFILE )      /* -> L */
RT( FILEATTR )      /* L L -> L */
RT( KILL )          /* $ -> */
RT( NAME )          /* $ $ -> */
RT( MKDIR )
RT( RMDIR )
RT( CHDIR )
RT( FILES )         /* $ L -> : the pattern, given? */
RT( SHELL )         /* $ L -> */
RT( ENVSET )        /* $ -> */
RT( ENVIRONS )      /* $ -> $ */
RT( ENVIRONN )      /* L -> $ */
RT( DATESET )       /* $ -> */
RT( TIMESET )
RT( DATE )          /* -> $ */
RT( TIME )
RT( IOCTL )         /* L $ -> */
RT( IOCTLS )        /* L -> $ */
RT( ERDEV )         /* -> L */
RT( ERDEVS )        /* -> $ */
RT( CHAIN )         /* $ -> */
RT( RUNFILE )       /* $ -> */
RT( RUNLINE )       /* L -> */

/* data */
RT( READI )         /* A -> */
RT( READL )
RT( READS )
RT( READD )
RT( READSTR )
RT( READFS )        /* A L -> */
RT( RESTORE )       /* L -> : the DATA item to read next */

/* arrays */
RT( DIM )           /* A L L L L (lo hi)... -> : slot type width dims flags, bounds */
RT( ERASE )         /* A -> */
RT( LBOUND )        /* A L -> L */
RT( UBOUND )

/* the machine */
RT( PEEK )          /* L -> L */
RT( POKE )          /* L L -> */
RT( DEFSEG )        /* L L -> : given?, segment */
RT( INP )           /* L -> L */
RT( OUT )           /* L L -> */
RT( WAIT )          /* L L L -> */
RT( VARPTR )        /* A -> L */
RT( VARPTRSTR )     /* A -> L: a string variable, whose descriptor PEEK reads */
RT( VARSEG )        /* A -> L */
RT( SADD )          /* A -> L */
RT( VARPTRS )       /* A L -> $ */
RT( BLOAD )         /* $ L L -> : name, given?, offset */
RT( BSAVE )         /* $ L L -> */
RT( CALLABS )       /* L -> : the arguments are not supported */
RT( FRE )           /* L -> L */
RT( FRESTR )        /* $ -> L */
RT( SLEEP )         /* L L -> : given?, seconds */
RT( STICK )         /* L -> L */
RT( STRIG )         /* L -> L */
RT( PEN )           /* L -> L */

/* events and errors */
RT( EVSET )         /* L L L -> : kind, which, handler address (0 = none) */
RT( EVCTL )         /* L L L -> : kind, which, ON/OFF/STOP */
RT( ERROR )         /* L -> */
RT( ERR )           /* -> L */
RT( ERL )           /* -> L */
RT( CLEAR )         /* -> */
RT( TRON )          /* L -> */
