/*
 * SETUP.H - everything the parts of SETUP.EXE share.
 *
 * SETUP.EXE IS TWO PROGRAMS IN ONE FILE.  The same C is compiled twice:
 *
 *   PLAT16.C   a 16-bit real-mode MS-DOS program (Open Watcom, large
 *              model).  This is the half an MS-DOS user starts: it
 *              upgrades the DOS on drive C: to PM-DOS.
 *   PLAT32.C   a PM-DOS native (D32) program.  This is the half that
 *              runs when the machine was started from the Setup disk,
 *              CD or USB stick: it can partition and format a hard
 *              disk, install on an empty one or upgrade the DOS on it.
 *
 * TOOLS\D32BIND puts the 16-bit .EXE in front of the D32 image, so
 * MS-DOS runs the first and PM-DOS the second (see D32BIND.C).  What
 * the halves are allowed to do differs only through the platform layer
 * (PLAT.H) and the plat_caps bits it sets; every screen, every file
 * rule and the copying itself are this one source.
 *
 * WRITTEN FOR A 16-BIT INT.  An "int" here is only ever a small count,
 * a row, a key or a flag.  Sizes, sector numbers and offsets are u32,
 * and a product of two of them is cast first.  No u64 anywhere: the
 * 16-bit half has no library helpers for it and a sector count fits in
 * 32 bits.  No object is larger than 32K.
 *
 * THE SCREENS ARE MS-DOS 6.22 SETUP'S: where everything is, the colours,
 * what each key does.  The wording is PM-DOS's own (TEXT.C).
 */
#ifndef SETUP_H
#define SETUP_H

#include "types.h"
#include "plat.h"

/* ------------------------------------------------------------------ */
/* limits                                                              */
/* ------------------------------------------------------------------ */

#define PATHLEN         80      /* a DOS path, as 6.22's own field holds */
#define DIRFIELD        44      /* the directory field's width on screen */
#define MAX_FILES       160     /* lines in SETUP.INF's [files]          */
#define MAX_DISKS       9       /* Setup disks                           */
#define MAX_COUNTRIES   64
#define MAX_KEYBOARDS   48
#define MAX_PAGES       4       /* code pages a country or layout names  */
#define MAX_LINES       200     /* lines of a CONFIG.SYS or AUTOEXEC.BAT */
#define LINELEN         160     /* and the longest of them               */
#define COPYBUF         16384u

/* ------------------------------------------------------------------ */
/* keys: ASCII below 100h, 100h + scan code for the rest               */
/* ------------------------------------------------------------------ */

#define K_ENTER         0x0D
#define K_ESC           0x1B
#define K_BKSP          0x08
#define K_TAB           0x09
#define K_EXT( scan )   (0x100 | (scan))
#define K_F1            K_EXT( 0x3B )
#define K_F3            K_EXT( 0x3D )
#define K_F5            K_EXT( 0x3F )
#define K_F7            K_EXT( 0x41 )
#define K_UP            K_EXT( 0x48 )
#define K_DOWN          K_EXT( 0x50 )
#define K_LEFT          K_EXT( 0x4B )
#define K_RIGHT         K_EXT( 0x4D )
#define K_HOME          K_EXT( 0x47 )
#define K_END           K_EXT( 0x4F )
#define K_PGUP          K_EXT( 0x49 )
#define K_PGDN          K_EXT( 0x51 )
#define K_DEL           K_EXT( 0x53 )
#define K_INS           K_EXT( 0x52 )

/* ------------------------------------------------------------------ */
/* SETUP.INF                                                           */
/* ------------------------------------------------------------------ */

/* where an installed file goes */
#define WH_ROOT         0       /* the root of the target drive          */
#define WH_DOS          1       /* the DOS directory                     */
#define WH_SETUP        2       /* nowhere: Setup's own (the boot code)  */

/* file flags */
#define FF_SYSTEM       0x01    /* read-only, hidden and system          */
#define FF_PACKED       0x02    /* SZDD-compressed on the Setup disk     */
#define FF_FLOPPY       0x04    /* part of the system a floppy gets (F7) */
#define FF_UNINST       0x08    /* goes on the Uninstall disk too        */

typedef struct {
    u8   disk;                  /* 1..                                   */
    u8   where;                 /* WH_*                                  */
    u8   flags;                 /* FF_*                                  */
    char src[13];               /* the name on the Setup disk            */
    char dst[13];               /* the name it is installed under        */
    u32  size;                  /* its size once expanded                */
} INFFILE;

typedef struct {
    char code[4];               /* "001"                                 */
    char name[24];
    u16  page[MAX_PAGES];       /* the first is what COUNTRY=code gets   */
    u8   npages;
    u8   date_order;            /* 0 = M D Y, 1 = D M Y, 2 = Y M D       */
    char date_sep, time_sep;
    u8   clock24;
} COUNTRYDEF;

typedef struct {
    char code[3];               /* "GR"                                  */
    char name[28];
    u16  id;                    /* /ID: when the code has more than one  */
    u16  page[MAX_PAGES];
    u8   npages;
} KEYBDEF;

typedef struct {
    char product[16];           /* "PM-DOS"                              */
    char version[8];            /* "0.65"                                */
    char dosdir[PATHLEN];       /* the default, "C:\DOS"                 */
    char label[12];             /* the volume label of a new C:          */
    int  ndisks;
    char disktag[MAX_DISKS + 1][13];    /* a file only that disk carries */
    char diskname[MAX_DISKS + 1][40];   /* what to ask the user for      */
    int  nfiles;
    INFFILE *file;
    int  ncountries;
    COUNTRYDEF *country;
    int  nkeyboards;
    KEYBDEF *keyboard;
    /* sections taken whole: lines of text */
    char **config;      int nconfig;    /* CONFIG.SYS of a new system    */
    char **autoexec;    int nautoexec;  /* AUTOEXEC.BAT of a new system  */
    char **cfgadd;      int ncfgadd;    /* lines an upgrade adds         */
    char **cfgdrop;     int ncfgdrop;   /* programs an upgrade REMs out: */
    char **autodrop;    int nautodrop;  /* CONFIG.SYS and AUTOEXEC.BAT   */
    char **bootcfg;     int nbootcfg;   /* the F7 floppy's CONFIG.SYS    */
} INF;

extern INF inf;

int  inf_load( const char *path );      /* 0 = read; else a TX_ message */
const COUNTRYDEF *inf_country( const char *code );
const KEYBDEF    *inf_keyboard( const char *code, u16 id );
INFFILE          *inf_file( const char *dst );

/* ------------------------------------------------------------------ */
/* what Setup found and what it is going to do                         */
/* ------------------------------------------------------------------ */

/* how drive C: looks (disk_examine) */
#define TG_NODISK       0       /* no hard disk at all                   */
#define TG_EMPTY        1       /* a disk with no partition table        */
#define TG_FREE         2       /* partitions, none for DOS, room for one */
#define TG_FULL         3       /* partitions, none for DOS, no room     */
#define TG_RAW          4       /* a DOS partition, not formatted        */
#define TG_BLANK        5       /* formatted, nothing installed          */
#define TG_MSDOS        6       /* another DOS: an upgrade               */
#define TG_PMDOS        7       /* PM-DOS already: a reinstall           */
#define TG_WIN9X        8       /* Windows 95/98's DOS: refused          */
#define TG_PACKED       9       /* DoubleSpace/DriveSpace: refused       */
#define TG_FOREIGN      10      /* C: is a network drive, a CD...        */

typedef struct {
    /* the disk */
    int  bios;                  /* its BIOS number, 80h or 81h           */
    int  state;                 /* TG_*                                  */
    u32  disk_secs;             /* its size                              */
    u16  spt, heads;            /* its geometry                          */
    u32  gap_lba, gap_secs;     /* the free area Setup would use         */
    int  slot;                  /* the partition slot for it, 0..3       */
    /* the volume */
    int  drive;                 /* its letter NOW, 0 = A: (-1 = none)    */
    char final;                 /* its letter when PM-DOS starts: 'C'    */
    u32  part_lba, part_secs;   /* where the partition is                */
    u32  free_kb;               /* free space, in KB                   */
    u16  reserved;              /* reserved sectors in its BPB           */
    int  fat;                   /* 12, 16, 32                            */
    /* the system on it */
    char sysname[24];           /* "MS-DOS 6.22", for the screens        */
    char sysfile[2][13];        /* IO.SYS / MSDOS.SYS, or IBM's names    */
    char olddir[PATHLEN];       /* where its commands are                */
} TARGET;

extern TARGET tgt;

typedef struct {
    int  year, month, day, hour, minute;
    const COUNTRYDEF *country;
    const KEYBDEF    *keyboard;
    char dosdir[PATHLEN];       /* as it will be: "C:\DOS"               */
    int  floppy;                /* F7: a system on a diskette            */
    int  floppy_drive;          /* 0 = A:                                */
    int  no_uninstall;          /* /G                                    */
    int  upgrade;               /* tgt.state is TG_MSDOS or TG_PMDOS     */
    int  nls_changed;           /* the country or keyboard was changed   */
} CHOICES;

extern CHOICES opt;

/* where the Setup files are, "A:\" or "D:\PMDOS\" (with the '\') */
extern char srcdir[PATHLEN];
extern int  src_drive;          /* its letter, 0 = A:                    */
extern int  src_removable;      /* a diskette: ask for the other disks   */

/* ------------------------------------------------------------------ */
/* the screen (SCR.C)                                                  */
/* ------------------------------------------------------------------ */

/* MS-DOS 6.22 Setup's colours */
#define A_DESK          0x17    /* light grey on blue                    */
#define A_BAR           0x70    /* black on light grey: the status line, */
                                /* the highlight, the warning boxes      */
#define A_NOTE          0x30    /* black on cyan: notices and prompts    */
#define A_HELPHEAD      0x71    /* blue on grey: "Setup Help"            */
#define A_GAUGE         0x0E    /* yellow on black: the gauge's blocks   */

#define SCR_ROWS        25
#define SCR_COLS        80
#define STATUS_ROW      24
#define STATUS_SPLIT    57      /* the bar dividing the status line      */

void scr_open( void );
void scr_close( void );
void scr_flush( void );
void scr_put( int row, int col, int attr, const char *text );
void scr_putn( int row, int col, int attr, const char *text, int len );
void scr_fill( int row, int col, int rows, int cols, int attr, int ch );
void scr_attr( int row, int col, int cols, int attr );
void scr_cursor( int row, int col );    /* an underline there            */
void scr_nocursor( void );
void scr_save( u16 *cells );            /* SCR_ROWS * SCR_COLS of them   */
void scr_restore( const u16 *cells );

/* the parts every screen is made of */
void scr_desk( const char *title );     /* blue, the title, the rule     */
void scr_status( const char *keys, const char *right );
int  scr_text( int row, int col, int width, int attr, const char *para );
int  scr_text_rows( int width, const char *para );
int  scr_para( int row, const char *para );     /* body text at col 10   */
int  scr_bullet( int row, const char *para );
int  scr_note( int row, const char *label, const char *para );
void scr_center( int row, int left, int right, int attr, const char *text );
void scr_box( int top, int left, int bottom, int right, int attr,
              const char *title );
void scr_gauge( u32 done, u32 total );

/* keys */
int  key_get( void );                   /* waits                         */
int  key_hit( void );                   /* a key now, or 0               */
void test_dump( void );                 /* the screen into the test log  */

const char *status_now( void );         /* the status line's keys        */

/* boxes laid out as 6.22 laid them (UI.C) */
#define DLG_LINES       22
typedef struct {
    const char *line[DLG_LINES];        /* [0] is the title              */
    int  n, width, used;
    char buf[1400];
} DLG;
void dlg_start( DLG *dlg, const char *title, int width );
void dlg_blank( DLG *dlg );
void dlg_center( DLG *dlg, const char *text );
void dlg_para( DLG *dlg, const char *text );
void dlg_bullet( DLG *dlg, const char *text );
void dlg_draw( DLG *dlg, int attr );
int  dlg_run( DLG *dlg, int attr, const char *status, const int *keys, int f3 );

/* the in-page list */
#define LS_NOESC        0x01            /* ESC does nothing here         */
typedef struct {
    int  top, rows;                     /* the box's top line, the window */
    const char **items;                 /* "" = a gap the bar steps over */
    int  count, sel, first;             /* first = -1: place it for me   */
    int  flags;
    const char *help_head, *help_text;
    int  (*other)( int key );           /* nonzero = this key ends it too */
} LIST;
int  ui_list( LIST *list );             /* the item, -1 = ESC, -2-key    */

typedef struct {
    int  row, col, width;
    char *buf;
    int  max, fixed, pos;
    const char *accept;                 /* the characters it takes, NULL = all */
} FIELD;
int  ui_field( FIELD *fld );            /* -> the key that ended it      */

int  ui_notice( const char *title, const char *text, int attr,
                const char *keys );    /* waits for ENTER               */
int  ui_exit_box( void );               /* F3                            */
void ui_help( const char *heading, const char *text );
int  scr_page( int row, int attr, const char *text );
void ui_working( const char *what );    /* "Setup is ..." in a box       */
void leave( int code );                 /* the screen put back, then out */

/* ------------------------------------------------------------------ */
/* the parts of the work                                               */
/* ------------------------------------------------------------------ */

/* DISK.C */
int  disk_load_code( void );            /* the boot code, from the source */
void disk_examine( void );              /* fills tgt                     */
int  disk_marked( void );               /* Setup made this partition     */
int  disk_partition( void );            /* 0 = the table is written      */
int  disk_format( void (*progress)( u32 done, u32 total ) );
int  disk_bootable( void );             /* the boot sector + stage 2     */
int  disk_activate( int made );
int  disk_space_ok( u32 need );
extern u8 *code_floppy;

/* a file held in memory, in COPYBUF pieces (COPY.C) */
#define HELD_PIECES     48
typedef struct {
    u8  *piece[HELD_PIECES];
    u16  len[HELD_PIECES];
    int  n;
    u16  date, time;
} HELD;
int  held_read( HELD *held, const char *path );
int  held_copy( HELD *held, const INFFILE *file );
int  held_write( HELD *held, const char *path, u16 attr );
void held_free( HELD *held );
void gauge_start( u32 total );
void gauge_finish( void );

/* COPY.C */
int  copy_all( void );                  /* 0 = done, else stopped        */
int  copy_one( const INFFILE *file, const char *dest, int quiet );
int  expand_file( u16 in, u16 out, u32 *written );
int  source_find( void );               /* 0 = SETUP.INF was found       */
void source_better( void );             /* a source with every disk      */
int  disk_wanted( int disk );           /* 0 = the disk is in the drive  */
u32  copy_total( int floppy );
void copy_dest( const INFFILE *file, char *dest, int max );
int  copy_error_box( const char *fmt, const char *name );
void disk_forget( void );

/* CFG.C */
int  cfg_write_new( void );
int  cfg_upgrade( int full );
int  cfg_floppy( int drive );
void cfg_read_current( void );          /* country, keyboard and DOS dir
                                           of the system being upgraded */

/* UNDO.C */
int  undo_prepare( void );              /* OLD_DOS.n and what is in it   */
int  undo_save( const char *path );     /* move an old file out of the way */
int  undo_finish( void );
const char *undo_dir( void );          /* OLD_DOS.n as PM-DOS sees it   */
int  undo_disk( void );                 /* the Uninstall disk            */
void label_page( const char *intro, const char *name, const char *note,
                 const char *help_head, const char *help_text );
int  uninstall_main( void );

/* FLOPPY.C */
int  floppy_format( int drive, const char *label,
                    void (*progress)( u32 done, u32 total ) );
int  floppy_has_tag( int drive, const char *tag );
int  floppy_install( void );
int  floppy_is_startup( int drive );
int  startup_again( void );
void floppy_target( int drive );
int  floppy_drive( void );

/* MAIN.C */
#define MADE_DIRS       8               /* levels of the DOS directory made */
extern char made_dirs[MADE_DIRS][PATHLEN];
extern int  nmade_dirs;
void setup_main( void );
void fatal( const char *text );         /* a box, then out               */
const char *setup_title( void );
extern int test_mode;

/* RT.C - the runtime both halves carry */
void  mem_set( void *dst, int val, u16 len );
void  mem_cpy( void *dst, const void *src, u16 len );
int   mem_cmp( const void *lhs, const void *rhs, u16 len );
int   str_len( const char *str );
void  str_cpyn( char *dst, const char *src, int max );
void  str_catn( char *dst, const char *src, int max );
int   str_icmp( const char *lhs, const char *rhs );
int   str_nicmp( const char *lhs, const char *rhs, int len );
int   ch_upper( int ch );
char *str_chr( const char *str, int ch );
char *str_rchr( const char *str, int ch );
char *str_dup( const char *str );
void  str_upper( char *str );
char *skip_blanks( const char *str );
void  path_join( char *dst, const char *dir, const char *name, int max );
void  path_name( char *dst, const char *path, int max ); /* the last part */
int   path_valid( const char *path );
void *xalloc( u32 size );               /* never NULL: exits instead     */
void  xfree( void *ptr );
int   __cdecl sfmt( char *buf, int max, const char *fmt, ... );
u32   parse_u32( const char *str, const char **end );

#endif
