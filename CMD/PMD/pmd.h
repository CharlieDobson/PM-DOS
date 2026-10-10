/*
 * PMD.H - what the parts of PMD share.
 *
 *   MAIN.C     the command line, the main screen, the commands
 *   SCR.C      the screen in memory, colours, the keyboard, the mouse
 *   UI.C       the menu bar, dialogs, message boxes, the text window
 *   TEXT.C     a page as lines of text; reading the ROMs
 *   CPU.C      the processor: CPUID where there is one
 *   PGSYS.C    the pages Computer, Video, OS Version, Network, Mouse,
 *              Other Adapters
 *   PGMEM.C    Memory, TSR Programs, Device Drivers
 *   PGPORT.C   Disk Drives, LPT Ports, COM Ports, IRQ Status
 *   REPORT.C   the report: /F /P /S and File, Print Report
 *   TOOLS.C    Find File, the file viewer, Memory Block Display, Memory
 *              Browser, Insert Command, Test Printer, About
 *   RT.C       the runtime instead of a C library
 *   SYS_D32.C  every call to the kernel
 *   DOSINT.ASM startup, the interrupt thunks, ports, the processor
 */
#ifndef PMD_H
#define PMD_H

#include "types.h"
#include "rt.h"
#include "sys.h"

#define PMD_VERSION     "1.00"

/* ------------------------------------------------------------------ */
/* keys: ASCII as itself, extended keys as 100h + scan code            */
/* ------------------------------------------------------------------ */
#define K_EXT( scan )   (0x100 | (scan))
#define K_SHIFT         0x1000  /* added to a movement key with Shift down */

#define K_BS            0x08
#define K_TAB           0x09
#define K_ENTER         0x0D
#define K_ESC           0x1B
#define K_SHTAB         K_EXT( 0x0F )
#define K_F1            K_EXT( 0x3B )
#define K_F3            K_EXT( 0x3D )
#define K_F5            K_EXT( 0x3F )
#define K_F10           K_EXT( 0x44 )
#define K_HOME          K_EXT( 0x47 )
#define K_UP            K_EXT( 0x48 )
#define K_PGUP          K_EXT( 0x49 )
#define K_LEFT          K_EXT( 0x4B )
#define K_RIGHT         K_EXT( 0x4D )
#define K_END           K_EXT( 0x4F )
#define K_DOWN          K_EXT( 0x50 )
#define K_PGDN          K_EXT( 0x51 )
#define K_INS           K_EXT( 0x52 )
#define K_DEL           K_EXT( 0x53 )
#define K_CLEFT         K_EXT( 0x73 )
#define K_CRIGHT        K_EXT( 0x74 )
#define K_CEND          K_EXT( 0x75 )
#define K_CPGDN         K_EXT( 0x76 )
#define K_CHOME         K_EXT( 0x77 )
#define K_CPGUP         K_EXT( 0x84 )

#define K_ALTTAP        0x2000  /* Alt pressed and let go on its own */
#define K_DUMP          0x2001  /* test mode: write the screen to the log */
#define K_EOF           0x2002  /* test mode: the key file ran out */
#define K_MOUSE         0x2003  /* the mouse: "mouse" says what it did */
#define K_TICK          0x2004  /* key_get_timed: nothing, in the time */

int   key_get( void );
int   key_get_timed( int hundredths );  /* K_TICK when nothing came */
int   key_shift( void );
int   key_alt_letter( int key );    /* 'A'..'Z', '0'..'9', or 0 */
void  key_init( void );
int   key_testing( void );

/* ------------------------------------------------------------------ */
/* the mouse: the left button, as events on screen cells               */
/* ------------------------------------------------------------------ */
enum { ME_DOWN, ME_UP, ME_DRAG, ME_DOUBLE };

typedef struct {
    int kind;                   /* ME_* */
    int row, col;               /* the cell it happened on */
    int shift;                  /* the shift keys at the time */
} MEVENT;

extern MEVENT mouse;            /* what the last K_MOUSE was */

void  mouse_init( void );       /* after scr_init: the screen's size */
void  mouse_done( void );
int   mouse_present( void );
int   mouse_held( void );       /* the left button is down now */

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    u8 desk;            /* the main screen behind the buttons */
    u8 summary;         /* the text beside a button */
    u8 push;            /* a button on the main screen */
    u8 push_hot;        /* its access key */
    u8 push_focus;      /* the one Enter would press */
    u8 push_shadow;
    u8 menu;            /* the menu bar and menus */
    u8 menu_hot;        /* an access key in them */
    u8 menu_sel;        /* the highlighted item */
    u8 menu_sel_hot;
    u8 menu_off;        /* an item that cannot be chosen now */
    u8 status;          /* the status bar */
    u8 dlg;             /* a dialog box or a page */
    u8 dlg_hot;
    u8 field;           /* an entry field in it */
    u8 field_sel;
    u8 button;
    u8 button_focus;
    u8 shadow;
    u8 map;             /* the memory map's cells */
    u8 map_sel;         /* the block picked out in it */
} PALETTE;

extern PALETTE pal;
extern int scr_rows, scr_cols;

void  scr_init( void );
void  scr_done( void );
void  scr_ch( int row, int col, int ch, u8 attr );
void  scr_putn( int row, int col, const char *text, int len, u8 attr );
void  scr_put( int row, int col, const char *text, u8 attr );
void  scr_hot( int row, int col, const char *text, u8 attr, u8 hot_attr );
int   scr_hotlen( const char *text );
int   scr_hotkey( const char *text );
void  scr_fill( int row, int col, int len, int ch, u8 attr );
void  scr_recolor( int row, int col, int len, u8 attr );
void  scr_box( int top, int left, int bottom, int right, u8 attr );
void  scr_shadow( int top, int left, int bottom, int right );
void  scr_cursor( int row, int col, int kind );
void  scr_flush( void );
void  scr_dump( void );             /* test mode */

/* ------------------------------------------------------------------ */
/* a page: lines of text                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    char **line;
    int    count, cap;
    int    failed;                  /* a line was lost for want of memory */
} TEXT;

void  tx_init( TEXT *text );
void  tx_free( TEXT *text );
void  tx_add( TEXT *text, const char *line );
void  __cdecl tx_addf( TEXT *text, const char *fmt, ... );
int   tx_width( const TEXT *text );
/* "Label: value", the labels of a page ending in one column */
void  tx_field( TEXT *text, int label_width, const char *label, const char *value );
void  __cdecl tx_fieldf( TEXT *text, int label_width, const char *label, const char *fmt, ... );

/* the ROMs, through sys_peek */
u8   *rom_load( u32 addr, u32 len );            /* xfree it; NULL = no memory */
int   rom_find_name( const u8 *rom, u32 len, const char *const *names );  /* index, or -1 */
int   rom_versions( const u8 *rom, u32 len, char found[3][64] );
int   rom_date( const u8 *rom, u32 len, char *date );       /* "mm/dd/yy" */
/* the readable text round rom[at]: its start and length, lines and all */
u32   rom_string( const u8 *rom, u32 len, u32 at, u32 *length );

/* ------------------------------------------------------------------ */
/* the pages                                                           */
/* ------------------------------------------------------------------ */
enum {
    PG_COMPUTER, PG_MEMORY, PG_VIDEO, PG_NETWORK, PG_OS, PG_MOUSE, PG_OTHER,
    PG_DISK, PG_LPT, PG_COM, PG_IRQ, PG_TSR, PG_DEVICE, PG_COUNT
};

#define SUM_WIDTH       18      /* a summary line beside a button */

extern const char *const page_names[PG_COUNT];

/* "report": for paper - the memory map in letters, not block characters */
void  page_build( int page, TEXT *text, int report );
void  page_summary( int page, char *line1, char *line2 );

/* CPU.C */
typedef struct {
    char name[64];              /* "Intel Pentium II", "486DX", "80386" */
    char vendor[16];            /* CPUID's twelve letters, or "" */
    char brand[52];             /* CPUID's own name for it, or "" */
    char features[80];
    int  cpuid;                 /* there is a CPUID instruction */
    int  family, model, stepping;
    int  level;                 /* 3 for a 386, 4, 5, 6... */
    int  fpu_internal;          /* the coprocessor is part of it */
} CPUINFO;
void  cpu_identify( CPUINFO *cpu );

/* PGSYS.C */
void  pg_computer( TEXT *text );
void  pg_video( TEXT *text );
void  pg_os( TEXT *text );
void  pg_network( TEXT *text );
void  pg_mouse( TEXT *text );
void  pg_other( TEXT *text );
void  sum_computer( char *line1, char *line2 );
void  sum_video( char *line1, char *line2 );
void  sum_os( char *line1, char *line2 );
void  sum_network( char *line1, char *line2 );
void  sum_mouse( char *line1, char *line2 );
void  sum_other( char *line1, char *line2 );
int   bus_is_mca( void );
int   mouse_com_port( void );       /* 1..4 when the mouse is a serial one, or 0 */
int   game_port( int stick[4], int *switches );     /* 1 = there is one */

/* PGMEM.C */
#define MAP_ROWS        24      /* A000h to FC00h, 16K a row */
#define MAP_COLS        16      /* 1K a cell */
enum { MC_EMPTY, MC_RAM, MC_ROM, MC_MAYBE, MC_FRAME, MC_UMB_USED, MC_UMB_FREE };

typedef struct {
    char name[20];
    u32  seg;                   /* the block's header */
    u32  size;                  /* bytes */
    char params[34];
    int  sub;                   /* a part of the block above it */
    int  free;
} MEMITEM;

void  pg_memory( TEXT *text, int report );
void  pg_tsr( TEXT *text );
void  pg_device( TEXT *text );
void  sum_memory( char *line1, char *line2 );
void  mem_map( u8 map[MAP_ROWS][MAP_COLS] );    /* what is at each K above 640K */
int   mem_map_char( int kind, int report );
int   mem_items( MEMITEM **items );             /* the blocks below 1MB; xfree it */
const char *mem_owner_name( u32 seg );          /* who owns an address below 1MB */
int   dev_present( const char *name );           /* a character device of that name is loaded */

/* PGPORT.C */
void  pg_disk( TEXT *text );
void  pg_lpt( TEXT *text );
void  pg_com( TEXT *text );
void  pg_irq( TEXT *text );
void  sum_disk( char *line1, char *line2 );
void  sum_lpt( char *line1, char *line2 );
void  sum_com( char *line1, char *line2 );
int   lpt_port( int index );                    /* 0..2 -> its address, or 0 */
int   com_port( int index );                    /* 0..3 -> its address, or 0 */

/* ------------------------------------------------------------------ */
/* the application                                                     */
/* ------------------------------------------------------------------ */
enum {
    CMD_NONE,
    CMD_FIND, CMD_REPORT, CMD_EXIT,
    CMD_BLOCKS, CMD_BROWSE, CMD_INSERT, CMD_PRINTER,
    CMD_ABOUT,
    CMD_VIEW1,              /* ...to CMD_VIEW1 + MAX_VIEWS - 1: show that file */
    CMD_PAGE = 100          /* + PG_*: show that page */
};

#define MAX_VIEWS       6

extern int  view_count;
extern char view_names[MAX_VIEWS][14];
extern char view_paths[MAX_VIEWS][PATH_MAX];

const char *err_text( int rc );     /* a DOS error, as a sentence */
void  app_draw( void );             /* everything under the dialogs */
void  app_status( const char *text );   /* a hint in the status bar */
int   app_can( int cmd );
void  app_abandon( void );          /* test mode's end */
void  pmd_main( void );
void  out_of_memory( void );

/* ------------------------------------------------------------------ */
/* menus, dialogs, message boxes                                       */
/* ------------------------------------------------------------------ */
#define MB_OK           1
#define MB_YESNO        2
#define MB_OKCANCEL     4
#define ID_OK           1
#define ID_CANCEL       2
#define ID_YES          3
#define ID_NO           4

int   menu_run( int key );          /* F10, Alt, Alt+letter -> a command or 0 */
void  menu_draw_bar( int selected, int show_hot );
int   msg_box( const char *text, int buttons );
int   msg_box2( const char *line1, const char *line2, int buttons );

/* the dialog machinery - EDIT's */
enum { CT_LABEL, CT_EDIT, CT_CHECK, CT_RADIO, CT_LIST, CT_BUTTON, CT_TEXT, CT_CUSTOM };

struct DLG;
typedef struct CTL {
    int   type;
    int   row, col, width, height;
    const char *text;       /* with '&' before the access key */
    int   id;               /* a button's answer, or a control's name */
    char *buf;              /* CT_EDIT */
    int   max, caret, scroll, fresh;
    int  *value;            /* CT_CHECK and CT_RADIO: *value, CT_RADIO: == id */
    int   count;            /* CT_LIST */
    const char *(*item)( struct DLG *dlg, int index );
    int   sel, top;
    int   is_default;       /* CT_BUTTON: Enter presses it */
    void (*draw)( struct DLG *dlg, struct CTL *ctl );   /* CT_CUSTOM */
} CTL;

typedef struct DLG {
    const char *title;
    int   rows, cols;
    int   top, left;
    CTL  *ctl;
    int   nctl;
    int   focus;
    void *data;
    /* changed: a control's value moved; activate: Enter on a list, or a
       button - return an id to close the dialog, 0 to stay */
    void (*changed)( struct DLG *dlg, int ctl );
    int  (*activate)( struct DLG *dlg, int ctl );
} DLG;

int   dlg_run( DLG *dlg );
void  dlg_draw( DLG *dlg );
void  ui_redraw( void );            /* the whole screen, dialogs and all */

/* a window of text with a title and an OK button: a page, or a file.
   "mark" picks out a block of it in another colour - the memory map -
   or is NULL */
typedef struct {
    int first, lines;               /* which lines of the text */
    int col, width;                 /* and which columns of them */
    u8  attr;
} TEXTMARK;

void  text_window( const char *title, const TEXT *text, const TEXTMARK *mark );

/* REPORT.C */
#define RPT_CUSTOMER    0
#define RPT_SUMMARY     1
#define RPT_PAGE        2       /* + PG_*: thirteen of them */
#define RPT_BROWSER     (RPT_PAGE + PG_COUNT)
#define RPT_FILE        (RPT_BROWSER + 1)   /* + a view_names index */
#define RPT_ITEMS       (RPT_FILE + MAX_VIEWS)

typedef struct {
    char field[8][52];          /* name, company, two address lines, city,
                                   country, telephone, comments */
} CUSTOMER;

extern const char *const customer_labels[8];

int   report_write( const char *path, const u8 *items, const CUSTOMER *who );  /* DOS error */
void  report_summary_text( TEXT *text );
void  cmd_report( void );           /* File, Print Report... */

/* TOOLS.C */
void  views_find( void );           /* which of the files to view exist, and where */
void  cmd_view( int index );
void  cmd_find_file( void );
void  cmd_blocks( void );
void  cmd_browse( void );
void  cmd_insert( void );
void  cmd_printer( void );
void  cmd_about( void );
int   file_to_text( const char *path, TEXT *text );         /* DOS error */
void  browse_text( TEXT *text );    /* the strings of every ROM, for the report */

#endif
