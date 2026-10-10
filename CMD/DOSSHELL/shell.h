/*
 * SHELL.H - what the parts of DOSSHELL share.
 *
 *   MAIN.C   the command line, the lists on the screen, the loop
 *   SCR.C    the screen in memory, colours, the keyboard, the mouse
 *   GFX.C    a graphics screen: modes, fonts, the picture, the pointer
 *   ICONS.C  the pictures a graphics screen puts beside names
 *   UI.C     the menu bar, menus, dialogs and message boxes
 *   TREE.C   a drive's directories
 *   FILES.C  the files of a directory, or of a whole drive
 *   OPS.C    copy, move, delete, rename, attributes, new directories
 *   VIEW.C   a file's contents, as text or in hexadecimal
 *   PROGS.C  program groups, DOSSHELL.INI, and running a program
 *   DLGS.C   the Options dialogs, Search, Run, Associate, About
 *   HELP.C   the help viewer and its text
 */
#ifndef SHELL_H
#define SHELL_H

#include "types.h"
#include "rt.h"
#include "sys.h"

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
#define K_F7            K_EXT( 0x41 )
#define K_F8            K_EXT( 0x42 )
#define K_F9            K_EXT( 0x43 )
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
#define K_SHF5          K_EXT( 0x58 )
#define K_SHF8          K_EXT( 0x5B )
#define K_SHF9          K_EXT( 0x5C )
#define K_ALTF4         K_EXT( 0x6B )
#define K_CEND          K_EXT( 0x75 )
#define K_CHOME         K_EXT( 0x77 )
#define K_CSLASH        K_EXT( 0x95 )   /* Ctrl+/ on the number pad */
#define K_CSTAR         K_EXT( 0x96 )   /* Ctrl+* on the number pad */
#define K_CBSLASH       0x1C            /* Ctrl+\ */
#define K_CTRL( letter ) (0x400 | (letter))     /* Ctrl+A is K_CTRL( 'A' ) */

#define K_ALTTAP        0x2000  /* Alt pressed and let go on its own */
#define K_DUMP          0x2001  /* test mode: write the screen to the log */
#define K_EOF           0x2002  /* test mode: the key file ran out */
#define K_MOUSE         0x2003  /* the mouse: "mouse" says what it did */
#define K_TICK          0x2004  /* key_get_timed: nothing, in the time */
#define K_SHOT          0x2005  /* test mode: the picture to a .BMP file */

int   key_get( void );
int   key_get_timed( int hundredths );  /* K_TICK when nothing came */
int   key_shift( void );
int   key_alt_letter( int key );    /* 'A'..'Z', '0'..'9', or 0 */
void  key_init( void );
int   key_testing( void );
int   key_service( int key );       /* 1 = it was the test harness's, and done */

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

void  mouse_init( void );       /* after a mode is set: the screen's size */
void  mouse_done( void );

/* ------------------------------------------------------------------ */
/* the screen                                                          */
/*                                                                     */
/* A cell is a character and its colours, as on a text screen, and on  */
/* a graphics screen it may be more: a slice of an icon in place of    */
/* the character, and lines along its edges.                           */
/* ------------------------------------------------------------------ */
typedef u32 CELL;

#define CF_TOP          0x04000000UL    /* a line along the cell's top... */
#define CF_BOTTOM       0x08000000UL
#define CF_LEFT         0x10000000UL
#define CF_RIGHT        0x20000000UL
#define CF_UNDER        0x40000000UL    /* the character underlined */
#define CF_GRAD         0x80000000UL    /* 256 colours: a shaded background */
#define CELL_ICON( id, slice )  (((u32)(id) << 16) | ((u32)(slice) << 24))

typedef struct {
    u8 desk;            /* the lines with the path and the drives */
    u8 title;           /* the shell's own title bar */
    u8 menu;            /* the menu bar and menus */
    u8 menu_hot;        /* an access key in them */
    u8 menu_sel;        /* the highlighted item */
    u8 menu_sel_hot;
    u8 menu_off;        /* an item that cannot be chosen now */
    u8 pane;            /* a list's text */
    u8 pane_title;      /* a list's title bar */
    u8 pane_focus;      /* ...when the keys go to that list */
    u8 select;          /* a selected entry */
    u8 select_off;      /* ...in a list the keys are not in */
    u8 drive_sel;       /* the drive in use */
    u8 scroll;          /* scroll bars */
    u8 thumb;           /* the scroll box in them */
    u8 status;          /* the status bar */
    u8 status_hot;
    u8 dlg;             /* a dialog box */
    u8 dlg_hot;
    u8 dlg_title;
    u8 field;           /* an entry field in it */
    u8 field_sel;
    u8 button;
    u8 button_focus;
    u8 shadow;
} PALETTE;

/* the screens there are: text ones, and graphics ones on three kinds
   of hardware */
enum { VK_TEXT, VK_EGA, VK_VGA, VK_VBE };

typedef struct {
    u8  kind;                   /* VK_* */
    u8  rows;
    u8  font;                   /* a character's height in a graphics mode */
    const char *tag;            /* its name in DOSSHELL.INI */
    const char *kind_name;      /* the Display dialog's three columns */
    const char *res_name;
} DMODE;

#define NDMODES         10
extern const DMODE dmodes[NDMODES];

extern PALETTE pal;
extern int scr_rows, scr_cols;
extern int scr_mode;            /* the dmodes[] entry on the screen */
extern int gfx_on;              /* it is a graphics one */

int   scr_mode_ok( int index );         /* this machine can show it */
int   scr_init( int index );            /* the mode it settled on */
int   scr_set_mode( int index );        /* 1 = set */
void  scr_done( void );
void  scr_suspend( void );              /* the DOS screen, for a program */
void  scr_resume( void );
void  scr_repaint( void );              /* everything sent again */
void  scr_palette( int scheme );
void  scr_ch( int row, int col, int ch, u8 attr );
void  scr_putn( int row, int col, const char *text, int len, u8 attr );
void  scr_put( int row, int col, const char *text, u8 attr );
void  scr_putw( int row, int col, const char *text, int width, u8 attr );  /* padded or cut to "width" */
void  scr_hot( int row, int col, const char *text, u8 attr, u8 hot_attr );
int   scr_hotlen( const char *text );
int   scr_hotkey( const char *text );
void  scr_fill( int row, int col, int len, int ch, u8 attr );
void  scr_recolor( int row, int col, int len, u8 attr );
void  scr_flag( int row, int col, int len, u32 flags );     /* CF_* onto cells */
u32   scr_bar_edges( void );            /* CF_TOP | CF_BOTTOM, where there is room */
int   scr_icon( int row, int col, int icon, u8 attr );      /* -> cells it took */
void  scr_box( int top, int left, int bottom, int right, u8 attr );
void  scr_shadow( int top, int left, int bottom, int right );
void  scr_cursor( int row, int col, int kind );
void  scr_flush( void );
void  scr_dump( void );                 /* test mode */
void  scr_shot( void );                 /* test mode */

/* a scroll bar down a column: drawn, and what a press on it means */
enum { SB_NONE, SB_UP, SB_DOWN, SB_PGUP, SB_PGDN, SB_THUMB };
void  sbar_draw( int top, int bottom, int col, int first, int shown, int total );
int   sbar_hit( int top, int bottom, int col, int first, int shown, int total, int row );
int   sbar_drag( int top, int bottom, int shown, int total, int row );  /* -> first */

/* ------------------------------------------------------------------ */
/* GFX.C - a graphics screen                                           */
/* ------------------------------------------------------------------ */
enum { IC_NONE, IC_DIR, IC_DIR_PLUS, IC_DIR_MINUS, IC_FILE, IC_PROG,
       IC_GROUP, IC_ITEM, IC_FLOPPY, IC_FIXED, IC_CDROM, IC_OTHER,
       IC_ARROW_UP, IC_ARROW_DOWN, IC_CHECK_OFF, IC_CHECK_ON, IC_RADIO_OFF,
       IC_RADIO_ON, IC_COUNT };

typedef struct {
    u8 width;                   /* pixels: 8, 16 or 32 */
    const char *const *rows;    /* sixteen strings, a character a pixel */
} ICON;

extern const ICON icons[IC_COUNT];
extern const char *const pointer_shape[16];

extern int gfx_adapter;         /* VK_TEXT (no graphics), VK_EGA or VK_VGA */
extern int gfx_vbe;             /* the VBE mode for 640x480 in 256 colours, or 0 */
extern int gfx_colors;          /* 16 or 256, on a graphics screen */

void  gfx_probe( void );
int   gfx_enter( const DMODE *mode );   /* 1 = the mode is on */
void  gfx_leave( void );
void  gfx_cell( int row, int col, CELL cell );
void  gfx_flush( void );
void  gfx_caret( int row, int col, int kind, int color );
void  gfx_pointer( int px, int py, int show );
void  gfx_scheme( int scheme );         /* the colours a new scheme or mode needs */
int   gfx_snapshot( const char *path ); /* the picture as a .BMP file */

/* ------------------------------------------------------------------ */
/* a drive's directories                                               */
/* ------------------------------------------------------------------ */
#define TN_KIDS         0x01    /* it has directories of its own */
#define TN_OPEN         0x02    /* ...and they are showing */
#define TN_LAST         0x04    /* the last of its parent's */

typedef struct {
    char *name;                 /* the root's is "" */
    int   parent;               /* -1 for the root */
    u8    depth;
    u8    flags;
} TNODE;

typedef struct {
    int    drive;               /* 0 = A: */
    int    valid;               /* it was read */
    TNODE *node;                /* in the order they are listed */
    int    count, cap;
    int   *vis;                 /* the nodes that are showing */
    int    nvis;
    char   label[12];
    u32    free_kb, total_kb;
} DTREE;

DTREE *tree_get( int drive, int reread );       /* NULL: the drive cannot be read */
void   tree_path( DTREE *tree, int node, char *buf );   /* "C:\A\B"; the root is "C:\" */
int    tree_find( DTREE *tree, const char *path );      /* the nearest node there is */
int    tree_vis_of( DTREE *tree, int node );    /* its line, opening what hides it */
void   tree_open( DTREE *tree, int node, int whole_branch );
void   tree_open_all( DTREE *tree );
void   tree_close( DTREE *tree, int node );
int    tree_add( DTREE *tree, int parent, const char *name );
void   tree_remove( DTREE *tree, int node );
void   tree_space( DTREE *tree );               /* free and total, asked again */

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    char *name;
    u32   size;
    u16   date, time;
    u8    attr;
    u8    sel;
    int   dir;                  /* the tree node the file is in */
    u32   order;                /* where the disk had it */
} FENT;

/* a sum of sizes, which can pass four gigabytes: K, and the bytes over */
typedef struct {
    u32 kb, rest;
} BIGSIZE;

void  big_add( BIGSIZE *sum, u32 bytes );
char *big_fmt( const BIGSIZE *sum, char *buf );     /* 20 bytes */

typedef struct {
    FENT *ent;
    int   count, cap;
    BIGSIZE total;              /* all of them together */
    int   nsel;
    BIGSIZE selected;           /* the selected ones: flist_count() */
} FLIST;

enum { SORT_NAME, SORT_EXT, SORT_DATE, SORT_SIZE, SORT_DISK };

void  flist_free( FLIST *list );
int   flist_read_dir( FLIST *list, DTREE *tree, int node, const char *filter );
int   flist_read_all( FLIST *list, DTREE *tree, const char *filter, int from_node );
void  flist_sort( FLIST *list );
void  flist_count( FLIST *list );               /* nsel and sel_bytes */
void  flist_path( DTREE *tree, const FENT *ent, char *buf );
void  fmt_date( u16 date, char *buf );          /* eight characters */
void  fmt_time( u16 time, char *buf );          /* "10:42a" or "22:42" */

/* ------------------------------------------------------------------ */
/* the application                                                     */
/* ------------------------------------------------------------------ */
enum { VIEW_SINGLE, VIEW_DUAL, VIEW_ALL, VIEW_PROGFILE, VIEW_PROGS };
enum { AREA_DRIVES, AREA_TREE, AREA_FILES, AREA_PROGS };

typedef struct {
    int  display;               /* a dmodes[] index */
    int  scheme;
    int  view;                  /* VIEW_* */
    int  sort_key, sort_desc;
    char filter[64];
    int  show_hidden;
    int  confirm_delete, confirm_replace, confirm_mouse;
    int  select_across;
} OPTIONS;

extern OPTIONS opt;

typedef struct {
    int    drive;
    int    drive_cur;           /* the drive the keys are on, in the drive line */
    DTREE *tree;
    int    node;                /* the directory the files are of */
    int    tree_top;            /* the first line of the tree showing */
    FLIST  files;
    int    cur, top;            /* the file the keys are on; the first showing */
    int    anchor;              /* where a Shift selection began */
    int    add_mode;            /* Shift+F8: moving does not select */
    int    stale;               /* the files are to be read again */
    int    search;              /* the list is a search's answer: 1 for one
                                   directory's, 2 for the whole drive's */
    char   search_for[64];
    /* where it is on the screen, from app_layout */
    int    row_path, row_drives, row_head, row_top, row_bot;
    int    tree_left, tree_right, file_left, file_right;
} FWIN;

extern FWIN wins[2];
extern int  nwins;
extern int  focus_win, focus_area;

enum {
    CMD_NONE,
    CMD_OPEN, CMD_RUN, CMD_PRINT, CMD_ASSOCIATE, CMD_SEARCH, CMD_VIEWFILE,
    CMD_MOVE, CMD_COPY, CMD_DELETE, CMD_RENAME, CMD_ATTRIB, CMD_MKDIR,
    CMD_SELALL, CMD_DESELALL, CMD_EXIT,
    CMD_PNEW, CMD_POPEN, CMD_PCOPY, CMD_PDELETE, CMD_PPROPS, CMD_PREORDER,
    CMD_CONFIRM, CMD_FILEOPTS, CMD_ACROSS, CMD_INFO, CMD_DISPLAY, CMD_COLORS,
    CMD_VSINGLE, CMD_VDUAL, CMD_VALL, CMD_VPROGFILE, CMD_VPROGS,
    CMD_REPAINT, CMD_REFRESH,
    CMD_EXPAND1, CMD_EXPANDBR, CMD_EXPANDALL, CMD_COLLAPSE,
    CMD_HINDEX, CMD_HKEYS, CMD_HBASICS, CMD_HCOMMANDS, CMD_HPROCS, CMD_HUSING,
    CMD_ABOUT, CMD_PROMPT
};

void  shell_main( void );
const char *err_text( int rc );     /* a DOS error, as a sentence */
void  app_layout( void );
void  app_draw( void );             /* everything under the dialogs */
void  app_status( const char *text );   /* a hint in the status bar */
void  app_command( int cmd );
int   app_can( int cmd );           /* can it be chosen right now? */
int   app_checked( int cmd );       /* does its menu line carry a mark? */
int   app_files_menu( void );       /* the keys are on files, not programs */
void  app_abandon( void );          /* test mode's end */
void  app_set_view( int view );
void  app_mode_changed( void );     /* the screen has another size now */
FWIN *app_win( void );              /* the file window the keys are in */
void  win_set_drive( FWIN *win, int drive );
void  win_set_node( FWIN *win, int node );
void  win_reload( FWIN *win );      /* its files, read again now */
void  wins_stale_all( void );       /* every window's files, to be read again */
void  wins_reread( int drive );     /* ...and the drive's directories too */
void  wins_mark( int drive );       /* round a tree_add or tree_remove: the */
void  wins_restore( int drive );    /* windows' directories, found again */
void  app_chdir( void );            /* DOS's current directory = the window's */
int   win_ok( const FWIN *win );    /* it has a drive that was read */

/* Select Across Directories: files that stay selected while another
   directory is the one showing */
int   kept_count( void );
const char *kept_path( int index );
void  kept_clear( void );

/* ------------------------------------------------------------------ */
/* menus, dialogs, message boxes                                       */
/* ------------------------------------------------------------------ */
#define MB_OK           1
#define MB_YESNO        2
#define MB_YESNOCANCEL  3
#define MB_OKCANCEL     4
#define ID_OK           1
#define ID_CANCEL       2
#define ID_YES          3
#define ID_NO           4
#define ID_HELP         5
#define ID_USER         10      /* a dialog's own buttons count from here */

#define MENU_ROW        1       /* the menu bar's line; the title is above it */

int   menu_run( int key );          /* F10, Alt, Alt+letter -> a command or 0 */
void  menu_draw_bar( int selected, int show_hot );
int   msg_box( const char *text, int buttons );
int   msg_box2( const char *line1, const char *line2, int buttons );
int   msg_text( const char *title, const char *text, int buttons, int help );   /* lines, \n between */
void  msg_error( const char *what, int rc );    /* "what" and err_text( rc ) */

/* the dialog machinery */
enum { CT_LABEL, CT_EDIT, CT_CHECK, CT_RADIO, CT_LIST, CT_BUTTON, CT_TEXT };

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
    int   hidden;           /* CT_EDIT: a password, shown as stars */
} CTL;

typedef struct DLG {
    const char *title;
    int   rows, cols;
    int   top, left;
    CTL  *ctl;
    int   nctl;
    int   focus;
    int   help;             /* the help topic F1 and the Help button show */
    void *data;
    /* changed: a control's value moved; activate: Enter on a list, or a
       button - return an id to close the dialog, 0 to stay */
    void (*changed)( struct DLG *dlg, int ctl );
    int  (*activate)( struct DLG *dlg, int ctl );
} DLG;

int   dlg_run( DLG *dlg );
void  dlg_draw( DLG *dlg );
void  dlg_frame( int top, int left, int rows, int cols, const char *title );
void  ui_push( DLG *dlg );          /* dialogs drawn over the lists */
void  ui_pop( void );
void  ui_redraw( void );            /* the whole screen, dialogs and all */

/* a box that says what is going on while it goes on: no buttons, drawn
   when asked.  busy_show( NULL, NULL ) takes it away. */
void  busy_show( const char *line1, const char *line2 );

/* controls, filled in a line at a time */
void  ctl_label( CTL *ctl, int row, int col, const char *text );
void  ctl_text( CTL *ctl, int row, int col, int width, const char *text );
void  ctl_edit( CTL *ctl, int row, int col, int width, char *buf, int max );
void  ctl_check( CTL *ctl, int row, int col, const char *text, int *value );
void  ctl_radio( CTL *ctl, int row, int col, const char *text, int *value, int id );
void  ctl_button( CTL *ctl, int row, int col, const char *text, int id, int is_default );

/* ------------------------------------------------------------------ */
/* OPS.C - what is done to files                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    char *path;                 /* whole */
    u32   size;
    u16   date, time;
    u8    attr;
    u8    is_dir;
} PICK;

typedef struct {
    PICK *item;
    int   count;
    u32   bytes;
} PICKS;

int   picks_take( PICKS *picks );   /* what the command applies to: 0 = nothing */
void  picks_free( PICKS *picks );

void  op_copy( int move );
void  op_delete( void );
void  op_rename( void );
void  op_attrib( void );
void  op_mkdir( void );
void  op_print( void );
void  op_drop( PICKS *picks, const char *dest_dir, int move );  /* the mouse let go there */
int   copy_file( const char *from, const char *to, u16 attr );

/* VIEW.C */
void  view_file( const char *path );

/* ------------------------------------------------------------------ */
/* PROGS.C - program groups, DOSSHELL.INI, running a program           */
/* ------------------------------------------------------------------ */
#define MAX_ITEMS       160
#define TITLE_MAX       24
#define CMDS_MAX        256
#define DIR_MAX         68
#define HELP_MAX        256
#define PASS_MAX        20
#define MAX_PROMPTS     3

typedef struct {
    char title[30];             /* the box's title */
    char info[78];              /* a line of advice in it */
    char label[30];             /* what stands before the field */
    char value[64];             /* what the field starts with */
} PPROMPT;

typedef struct {
    int   is_group;
    int   parent;               /* an items[] index; -1 for the top group */
    char  title[TITLE_MAX];
    char  help[HELP_MAX];
    char  password[PASS_MAX];
    /* a program's */
    char  cmds[CMDS_MAX];       /* commands, a ; between them */
    char  dir[DIR_MAX];         /* where to start, or "" */
    int   pause;                /* wait for a key before coming back */
    PPROMPT prompt[MAX_PROMPTS];    /* the boxes %1 to %3 are asked with */
} PITEM;

extern PITEM *items;            /* items[0] is the top group, "Main" */
extern int    nitems;
extern int    prog_group;       /* the group that is showing */
extern int    prog_cur, prog_top;

int   prog_count( void );                   /* lines the list has now */
int   prog_at( int line );                  /* -> an items[] index; -1 is "back" */
void  prog_open( void );
int   prog_help( void );                    /* 1 = the entry's own help was shown */
void  prog_new( void );
void  prog_copy( void );
void  prog_delete( void );
void  prog_props( void );
void  prog_reorder( void );
void  prog_back( void );                    /* out of a group, to the one above */

void  ini_load( void );
void  ini_save( void );

/* a command line handed to the interpreter, the screen given up for it */
int   run_command( const char *cmds, const char *dir, int pause );
void  run_prompt( void );                   /* Shift+F9 */
void  run_file( const char *path );         /* a program, or a file an extension is tied to */

const char *assoc_program( const char *ext );       /* NULL for none */
void  assoc_set( const char *ext, const char *program );
int   assoc_count( void );
const char *assoc_ext( int index );
const char *assoc_prog_at( int index );

/* ------------------------------------------------------------------ */
/* DLGS.C                                                              */
/* ------------------------------------------------------------------ */
void  dlg_confirmation( void );
void  dlg_file_options( void );
void  dlg_display( void );
void  dlg_colors( void );
void  dlg_show_info( void );
int   info_line( FWIN *win, int line, char *buf, u32 max );     /* 0 past the last */
void  dlg_search( void );
void  dlg_run_line( void );
void  dlg_associate( void );
void  dlg_about( void );
int   input_box( const char *title, const char *info, const char *label,
                 char *buf, int max, int width, int help, int hidden );
const char *scheme_name( int index );
int   scheme_count( void );
void  scheme_fill( int index, int graphics, PALETTE *out );

/* HELP.C */
enum { HELP_INDEX, HELP_KEYS, HELP_BASICS, HELP_COMMANDS, HELP_PROCS, HELP_USING,
       HELP_FILES, HELP_PROGRAMS, HELP_DISPLAY, HELP_DIALOGS, HELP_VIEWER,
       HELP_TOPICS };
void  help_show( int topic );

#endif
