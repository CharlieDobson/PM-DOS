/*
 * MODE.H - what the parts of MODE share.
 *
 *   MAIN.C   the first word, the forms it leads to, and the status of
 *            everything
 *   SCAN.C   the command line, split the way MS-DOS's parser split it,
 *            and the error messages that name a piece of it
 *   COM.C    the serial ports            LPT.C   the printer ports
 *   CONS.C   the screen and keyboard     CP.C    code pages
 */
#ifndef MODE_H
#define MODE_H

#include "types.h"
#include "rt.h"
#include "sys.h"
#include "msg.h"

/* the errorlevels MS-DOS's MODE leaves */
#define EXIT_OK     0
#define EXIT_ERROR  1

/* ------------------------------------------------------------------ */
/* the command line (SCAN.C)                                           */
/* ------------------------------------------------------------------ */

#define IT_POS      0           /* a word or a number                  */
#define IT_KEY      1           /* NAME=value                          */
#define IT_SWITCH   2           /* /word                               */
#define IT_EMPTY    3           /* nothing between two commas          */

typedef struct {
    const char *text;           /* where it starts in the line         */
    u32         len;
    int         kind;           /* IT_*                                */
    const char *value;          /* IT_KEY: what follows the '='        */
    u32         vlen;
    const char *resume;         /* where the next item is looked for   */
    int         comma;          /* ended by a comma straight after it  */
} ITEM;

typedef struct {
    const char *pos;
} SCAN;

void  line_init( const char *tail );
const char *line_text( void );
void  scan_start( SCAN *sc );
int   scan_next( SCAN *sc, ITEM *it, int colon );   /* 0 at the end */
int   scan_more( const SCAN *sc );                  /* 1 if an item is left */

int   is_word( const ITEM *it, const char *word );
int   is_key( const ITEM *it, const char *name );   /* name ends in '=' */
int   val_is( const ITEM *it, const char *word );
int   val_num( const ITEM *it, u32 *num );          /* 0, or 1 = not a number */
int   is_status( const ITEM *it );                  /* /STA /STAT /STATUS */
int   is_help( const ITEM *it );                    /* /? */

/* each prints its message, marks the run failed and returns EXIT_ERROR */
int   err_param( const ITEM *it );
int   err_param_cut( const ITEM *it );      /* the word up to . " \ [ ] + */
int   err_param_none( void );               /* "Invalid parameter -  " */
int   err_param_str( const char *text );
int   err_switch( const ITEM *it );
int   err_format( const ITEM *it );
int   err_count( void );
int   err_line( const char *msg );

/* ------------------------------------------------------------------ */
/* what the command line asked for                                     */
/* ------------------------------------------------------------------ */

#define F_HELP      1
#define F_STATUS    2           /* kind + unit; DK_ALL for everything   */
#define F_COM       3
#define F_LPT       4
#define F_LPTRESET  5           /* MODE LPTn                            */
#define F_ROUTE     6           /* MODE LPTn=COMm                       */
#define F_CON       7
#define F_DISPLAY   8
#define F_CP        9           /* kind + unit                          */
#define F_PRNCP     10          /* MODE PRN: LPT1's code page status    */

#define DK_ALL      0
#define DK_COM      1
#define DK_LPT      2
#define DK_CON      3

#define CP_STATUS   0
#define CP_SELECT   1
#define CP_REFRESH  2
#define CP_PREPARE  3
#define CP_MAX      12          /* code pages in one PREPARE=           */
#define CPFILE_MAX  128

/* the display words, in BIOS mode order where there is one */
#define DM_NONE     (-1)
#define DM_BW40     0
#define DM_CO40     1
#define DM_BW80     2
#define DM_CO80     3
#define DM_MONO     7
#define DM_40       40
#define DM_80       80

typedef struct {
    int  form;
    int  kind;                  /* DK_*                                 */
    int  unit;                  /* the port, 1 for COM1 or LPT1         */

    /* COMn */
    u32  baud;
    int  parity;                /* 'n' 'e' 'o' 'm' 's', 0 = not given   */
    int  data;                  /* 0 = not given                        */
    int  stop;                  /* 1, 15 for 1.5, 2; 0 = not given      */
    int  retry;                 /* RETRY_*, -1 = not given              */
    int  keyform;               /* the keyword form, not positions      */

    /* LPTn, and the COM port of LPTn=COMm */
    int  cols;                  /* 80 or 132, 0 = not given             */
    int  lines;                 /* 6 or 8, 0 = not given                */
    int  com;

    /* CON and the display forms */
    int  ccols;                 /* 40 or 80, 0 = not given              */
    int  crows;                 /* 25, 43 or 50, 0 = not given          */
    int  rate, delay;           /* 0 = not given                        */
    int  dmode;                 /* DM_*                                 */
    int  shift;                 /* 'R', 'L' or 0                        */
    int  test;                  /* the T after R or L                   */

    /* CP */
    int  cpop;                  /* CP_*                                 */
    int  ncp;
    int  cplist[CP_MAX];        /* -1 for an empty place                */
    char cpfile[CPFILE_MAX];    /* "" for none                          */
} REQ;

extern int failed;              /* the run ends with errorlevel 1       */

/* MAIN.C */
void mode_main( void );
void say_dev( char *buf, const char *name, int unit );     /* "COM1" */

/* COM.C */
int  com_parse( SCAN *sc, REQ *rq, const ITEM *first );
void com_set( const REQ *rq );
void com_status( int unit );
int  com_count( void );                 /* COM ports the BIOS lists     */
int  com_present( int unit );           /* this one's slot is filled    */

/* LPT.C */
int  lpt_parse( SCAN *sc, REQ *rq, int slot );
int  route_parse( SCAN *sc, REQ *rq, const ITEM *it );
void lpt_set( const REQ *rq );
void lpt_reset( int unit );
void lpt_route( const REQ *rq );
void lpt_status( int unit );

/* CONS.C */
int  con_parse( SCAN *sc, REQ *rq );
int  display_parse( SCAN *sc, REQ *rq, int mode );
void con_set( const REQ *rq );
void display_set( const REQ *rq );
void con_status( void );

/* CP.C */
int  cp_parse( SCAN *sc, REQ *rq, int kind, int unit );
void cp_run( const REQ *rq );
void cp_status( int kind, int unit );

#endif
