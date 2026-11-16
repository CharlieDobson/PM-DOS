/*
 * UI.H - menus, dialog boxes, message boxes and the file box, over the
 * cells of DISP.H: UI.C and FBOX.C.
 *
 * EVERYTHING HERE IS DRAWN IN CELLS, so it is the same on a text screen
 * and a graphics one, give or take the lines round a button.  Nothing
 * remembers what it covered.  A menu or a dialog that is open runs a
 * loop of its own, and on every turn of it the application is asked to
 * draw the whole screen again (app_draw), the open dialogs are drawn
 * over that, and the screen layer sends what changed - and tells any
 * pane that was uncovered to paint itself.
 *
 * THE APPLICATION SUPPLIES five functions, which the loops here call:
 *
 *   app_draw      draw the whole screen into the cells: menu bar
 *                 (menu_draw_bar), panes (pane_place), status bar, and
 *                 set the cursor and the caret.  No scr_flush.
 *   app_can       can this command be chosen now?  0 greys its menu
 *                 line and makes it deaf
 *   app_checked   does its menu line carry a check mark?
 *   app_status    a hint for the status bar while a menu line is lit,
 *                 or a dialog is open; NULL when that is over.  The
 *                 application keeps the pointer and shows the text
 *                 (or its own status) the next time it draws
 *   app_abandon   test mode: the key file has run out.  Leave the
 *                 program without saving or asking anything.  It must
 *                 not return.
 *
 * COLOURS are all in one PALETTE, "pal".  There are two looks and
 * ui_look picks the one for the screen: on a text screen Word for
 * DOS's - a grey menu bar and grey dialogs over a blue document, black
 * for what is lit, a cyan line for messages; on a graphics screen
 * Word 6's - light grey bars and dialogs with edges that stand out,
 * navy for titles and for what is selected, a white page on a darker
 * grey desk.  The application takes the colours of its own parts from
 * the same place: the attributes for its cells, and the pixel numbers
 * (px_...) for what it draws in its panes.
 */
#ifndef UI_H
#define UI_H

#include "disp.h"

/* ------------------------------------------------------------------ */
/* the application's side                                              */
/* ------------------------------------------------------------------ */
void  app_draw( void );
int   app_can( int cmd );
int   app_checked( int cmd );
void  app_status( const char *hint );
void  app_abandon( void );

/* the program's name, for the title of a message box.  "PMWRITE" until
   the application says otherwise. */
extern const char *ui_name;

/* F1 in a dialog, and a button whose answer is ID_HELP, call this with
   the dialog's "help" string when it is not NULL.  It starts NULL. */
extern void (*ui_help)( const char *topic );

/* ------------------------------------------------------------------ */
/* colours                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    /* cells: attributes (text colour in the low half, background in
       the high half) */
    u8 desk;            /* the screen where nothing else is */
    u8 title;           /* a title bar: the program's, a dialog's on a graphics screen */
    u8 menu;            /* the menu bar and menus */
    u8 menu_hot;        /* an access key in them */
    u8 menu_sel;        /* the lit title or line */
    u8 menu_sel_hot;
    u8 menu_off;        /* a line that cannot be chosen now */
    u8 status;          /* the status bar */
    u8 status_hot;      /* something that stands out in it */
    u8 dlg;             /* a dialog box and the words in it */
    u8 dlg_hot;         /* an access key */
    u8 dlg_off;         /* a control that is switched off */
    u8 field;           /* an entry field, a drop-down list closed */
    u8 field_sel;       /* the text offered in a field, which typing replaces */
    u8 list;            /* a list's entries */
    u8 select;          /* the chosen entry */
    u8 button;
    u8 button_hot;      /* the brackets of the button Enter presses, on a text screen */
    u8 scroll;          /* a scroll bar */
    u8 thumb;           /* the box in it */
    u8 shadow;          /* what a box's shadow makes of what is under it */
    u8 text;            /* a text screen's document */
    u8 text_sel;        /* ...selected */
    u8 ribbon;          /* a text screen's line of formatting, under the menu bar */
    u8 ribbon_hot;      /* what is switched on in it */
    /* how things are drawn */
    u8 shadows;         /* 1: menus and dialogs cast a shadow */
    u8 brackets;        /* 1: a field is [in brackets], a button <in angle ones> */
    u8 fill;            /* the character in the unused part of a field */
    /* pixels: pixel numbers for what the application paints in panes */
    u8 px_face;         /* a toolbar's and a ruler's surface */
    u8 px_light;        /* the lit edge of something raised */
    u8 px_shade;        /* its shaded edge */
    u8 px_frame;        /* an outline */
    u8 px_desk;         /* behind the page */
    u8 px_page;         /* the page */
    u8 px_ink;          /* writing on it */
    u8 px_select;       /* behind selected writing */
    u8 px_select_ink;   /* selected writing */
} PALETTE;

extern PALETTE pal;

/* LOOK_AUTO is the look that goes with the screen (LOOK_TEXT or
   LOOK_GRAPHICS by gfx_on) and is what to call after disp_init and
   after every mode change; LOOK_MONO is a text screen in black, white
   and bright white for a display with no colour.  A look that was
   asked for by name stays until another is. */
enum { LOOK_AUTO, LOOK_TEXT, LOOK_GRAPHICS, LOOK_MONO };
void  ui_look( int look );

/* disp_set_mode, the look that goes with the new screen, and - when the
   mode could not be had - a message box saying why and what the screen
   is instead.  Returns what disp_set_mode returned.  The application
   lays its screen out again afterwards (or does so in app_draw, from
   scr_rows and scr_cols, every time). */
int   ui_set_mode( int index );

/* What every loop that reads keys does with the test harness's own:
   K_EOF (app_abandon), K_DUMP (scr_dump) and K_SHOT (scr_shot).
   Returns 1 when the key was one of them and is dealt with. */
int   key_service( int key );

/* ------------------------------------------------------------------ */
/* menus                                                               */
/*                                                                     */
/* The application's tables:                                           */
/*                                                                     */
/*   static const MENUITEM file_items[] = {                            */
/*       { "&New",        "Ctrl+N", CMD_NEW,  "Starts a new document" },*/
/*       { "&Open...",    "Ctrl+O", CMD_OPEN, "Opens a document" },    */
/*       { NULL, NULL, 0, NULL },             (a line across the menu) */
/*       { "E&xit",       "Alt+F4", CMD_EXIT, "Leaves the program" }   */
/*   };                                                                */
/*   static const MENU menus[] = {                                     */
/*       { "&File", file_items, 4 }, { "&Edit", edit_items, 7 } ...    */
/*   };                                                                */
/*   menu_set( menus, 9, 0 );            (the bar is on row 0)         */
/*                                                                     */
/* "keys" is only shown: the application acts on the shortcut itself   */
/* when key_get returns it.  A command number is anything but 0.       */
/*                                                                     */
/* IN THE LOOP:  key = key_get();  if ( menu_wants( key ) ) {          */
/* cmd = menu_run( key ); ... }.  The keys and the mouse are EDIT's    */
/* and the MS-DOS Shell's: F10 or a tap of Alt lights the first title, */
/* Alt and a letter opens that menu, the arrows go round, Enter        */
/* chooses, a letter chooses the line with that access key, Esc        */
/* leaves; a press on a title opens its menu, which follows the        */
/* pointer while the button is held - let go on a line and that is     */
/* the command, let go on the title and the menu stays open for a      */
/* click or a key.                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *text;           /* "&Save As..."; NULL for a line across the menu */
    const char *keys;           /* "F12", or "" */
    int         cmd;
    const char *hint;           /* the status bar's words while the line is lit */
} MENUITEM;

typedef struct {
    const char     *title;      /* "&File" */
    const MENUITEM *items;
    int             count;
} MENU;

void  menu_set( const MENU *menus, int count, int row );

/* The bar, drawn into the cells: for app_draw, with selected -1.
   show_hot 1 shows the access keys (a graphics screen underlines
   them); Word for DOS shows them only while the menus are in use. */
void  menu_draw_bar( int selected, int show_hot );

int   menu_wants( int key );    /* F10, a tap of Alt, Alt and a title's letter, a press on a title */
int   menu_run( int key );      /* -> the command chosen, or 0 */

/* A menu on its own at a cell, for the right button: the same lines,
   keys and mouse.  It is moved to fit the screen.  -> the command, or
   0. */
int   menu_popup( const MENUITEM *items, int count, int row, int col );

/* ------------------------------------------------------------------ */
/* dialogs                                                             */
/*                                                                     */
/* A dialog is a box and a table of controls, each at a row and column */
/* inside the box (the box's top left is 0, 0, and row 0 is its title  */
/* and border).  Fill a CTL with one of the ctl_ calls, then set what  */
/* else it needs.  dlg_run draws, reads keys and the mouse, and        */
/* returns the id of the button that closed it - ID_CANCEL for Esc.    */
/*                                                                     */
/*   Tab, Shift+Tab   the next control, the one before                 */
/*   Alt and a letter the control whose label has that access key; a   */
/*                    plain letter does the same where letters mean    */
/*                    nothing else                                     */
/*   Enter            the default button (or the button the keys are   */
/*                    on); in a list, the dialog's "activate" first    */
/*   Esc              Cancel                                           */
/*   F1               ui_help                                          */
/*                                                                     */
/* A dialog must fit 80 by 25: at most 78 columns and 23 rows.         */
/* ------------------------------------------------------------------ */
#define ID_OK           1
#define ID_CANCEL       2
#define ID_YES          3
#define ID_NO           4
#define ID_HELP         5
#define ID_USER         10      /* a dialog's own buttons count from here */

enum { CT_LABEL,        /* words with an access key, naming the control after it */
       CT_TEXT,         /* words: lines with \n between, centred in "width" if that is not 0 */
       CT_FRAME,        /* a box round a group, "width" by "height", with a title */
       CT_EDIT,         /* an entry field of one line */
       CT_CHECK,        /* [X] a check box */
       CT_RADIO,        /* ( ) an option button */
       CT_LIST,         /* a list box with a scroll bar */
       CT_DROP,         /* a drop-down list: one line that opens into a list */
       CT_BUTTON };

struct DLG;
typedef struct CTL {
    int   type;                 /* CT_* */
    int   row, col;             /* inside the dialog */
    int   width, height;        /* cells: CT_EDIT, CT_DROP (width); CT_LIST, CT_FRAME (both,
                                   borders included); CT_TEXT (width, to centre in) */
    const char *text;           /* with '&' before the access key; a CT_LIST's is its title */
    int   id;                   /* CT_BUTTON: its answer.  CT_RADIO: its value */
    int   disabled;             /* 1: greyed, and passed over */

    /* CT_EDIT.  The text offered when the field is entered is replaced
       by the first character typed; Home, End, the arrows, Backspace
       and Del edit it. */
    char *buf;                  /* the text, NUL-ended */
    int   max;                  /* buf's size with the NUL */
    int   hidden;               /* 1: shown as stars */
    int   caret, scroll, fresh; /* the engine's */

    /* CT_CHECK: *value is 0 or 1.  CT_RADIO: *value is the id of the
       one that is set; controls next to each other with the same
       "value" are one group. */
    int  *value;

    /* CT_LIST and CT_DROP: "count" entries, each a string - from
       "strings" when that is not NULL, else from "item".  "sel" is the
       chosen one (the application reads it afterwards).  A letter
       jumps to the next entry that starts with it. */
    int   count;
    const char *const *strings;
    const char *(*item)( struct DLG *dlg, int index );
    int   sel;
    int   top;                  /* the engine's: the first entry showing */
    int   drop_rows;            /* CT_DROP: the most entries its list shows at once (0: 8) */

    int   is_default;           /* CT_BUTTON: Enter presses it */
} CTL;

typedef struct DLG {
    const char *title;
    int   rows, cols;           /* the box, borders included */
    int   placed;               /* 0: centred on the screen.  1: at top, left (it is
                                   still moved to be wholly on the screen) */
    int   top, left;
    CTL  *ctl;
    int   nctl;
    int   focus;                /* the control the keys start on; and are on */
    int   sep;                  /* not 0: a line across the box on this row (above the buttons) */
    const char *help;           /* for ui_help */
    void *data;                 /* the caller's */

    /* changed: a control's value moved (a key typed in a field, a box
       ticked, another entry chosen).  activate: Enter or a double
       click in a list, or a button pressed - return an id to close the
       dialog with that answer, 0 to stay open.  A dialog with no
       "activate" closes on any button.  A list's "activate" that
       wants Enter to stop there and not go on to the default button
       sets "stay". */
    void (*changed)( struct DLG *dlg, int ctl );
    int  (*activate)( struct DLG *dlg, int ctl );
    int   stay;

    int   drop;                 /* the engine's: the open drop-down's control + 1 */
    int   drop_pick, drop_top;
} DLG;

int   dlg_run( DLG *dlg );
void  dlg_draw( DLG *dlg );     /* into the cells, as it stands */

/* the box alone: border, title, shadow */
void  dlg_frame( int top, int left, int rows, int cols, const char *title );

/* dialogs drawn over the application's screen: dlg_run pushes its
   dialog for as long as it runs */
void  ui_push( DLG *dlg );
void  ui_pop( void );
void  ui_redraw( void );        /* app_draw and every dialog that is open; no flush */

/* controls, filled in a line at a time: each clears the CTL first */
void  ctl_label( CTL *ctl, int row, int col, const char *text );
void  ctl_text( CTL *ctl, int row, int col, int width, const char *text );
void  ctl_frame( CTL *ctl, int row, int col, int width, int height, const char *text );
void  ctl_edit( CTL *ctl, int row, int col, int width, char *buf, int max );
void  ctl_check( CTL *ctl, int row, int col, const char *text, int *value );
void  ctl_radio( CTL *ctl, int row, int col, const char *text, int *value, int id );
void  ctl_list( CTL *ctl, int row, int col, int width, int height, const char *title,
                const char *const *strings, int count );
void  ctl_drop( CTL *ctl, int row, int col, int width, const char *const *strings, int count );
void  ctl_button( CTL *ctl, int row, int col, const char *text, int id, int is_default );

/* a button's width in cells: its text without the '&', and four */
int   ctl_button_width( const char *text );

/* ------------------------------------------------------------------ */
/* message boxes                                                       */
/* ------------------------------------------------------------------ */
#define MB_OK           1
#define MB_OKCANCEL     2
#define MB_YESNO        3
#define MB_YESNOCANCEL  4

/* lines of text (\n between them) over a row of buttons -> ID_OK,
   ID_CANCEL, ID_YES or ID_NO; Esc is ID_CANCEL (ID_NO in a Yes/No box) */
int   msg_text( const char *title, const char *text, int buttons );
int   msg_box( const char *text, int buttons );                 /* titled with ui_name */
int   msg_box2( const char *line1, const char *line2, int buttons );

/* "what" and, under it, a DOS error code as a sentence */
void  msg_error( const char *what, int rc );
const char *err_text( int rc );

/* One line asked for: a title, a label before the field, and the text,
   which starts as "buf" holds it.  "width" is the field's, in cells.
   -> 1 for OK, 0 for Cancel (and buf as it was). */
int   input_box( const char *title, const char *label, char *buf, int max, int width );

/* A box that says what is going on while it goes on: no buttons, drawn
   and sent at once.  The next redraw of the screen leaves it out. */
void  busy_show( const char *line1, const char *line2 );

/* ------------------------------------------------------------------ */
/* a scroll bar down a column of cells                                 */
/*                                                                     */
/* An arrow at each end and a track between, with a box in the track   */
/* when there is more than fits: "first" is the first line showing,    */
/* "shown" how many show, "total" how many there are.                  */
/* ------------------------------------------------------------------ */
enum { SB_NONE, SB_UP, SB_DOWN, SB_PGUP, SB_PGDN, SB_THUMB };
void  sbar_draw( int top, int bottom, int col, int first, int shown, int total );
int   sbar_hit( int top, int bottom, int first, int shown, int total, int row );    /* -> SB_* */
int   sbar_drag( int top, int bottom, int shown, int total, int row );      /* the box dragged to
                                                                               "row" -> first */

/* ------------------------------------------------------------------ */
/* FBOX.C - the file box                                               */
/*                                                                     */
/* Open and Save As.  A field for the name, the files of the directory */
/* (long names where the kernel has them), its directories, a          */
/* drop-down list of the drives and one of the kinds of file the       */
/* caller names.  Typing a pattern (*.TXT) lists those files; typing a */
/* directory or a drive (C:, \DOCS, ..) goes there.  A drive that is   */
/* not ready is said to be not ready, and the box stays where it was.  */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *label;          /* "Word documents (*.DOC)" */
    const char *patterns;       /* "*.DOC;*.DOT" - one or more, a ; between them */
} FBTYPE;

/* "path" is PATH_LEN bytes.  Coming in it is where to start: "" for
   the current directory, a directory, or a whole file name (the box
   starts in that file's directory with its name in the field - what
   Save As wants).  Returning 1 it is the file chosen, whole:
   "C:\DOCS\LETTER.DOC".  "*type" is the kind of file listed first and,
   afterwards, the one that was listed when OK was pressed (NULL when
   the caller does not care; "types" may be NULL for "*.*" alone).
   A name typed with no extension is given the first pattern's, when
   that pattern has a plain one.
   Returning 0 (Cancel) the drive and directory that were current are
   current again; after 1 they are those of the file.
   fbox_save asks before it gives back the name of a file that exists. */
int   fbox_open( const char *title, char *path, const FBTYPE *types, int ntypes, int *type );
int   fbox_save( const char *title, char *path, const FBTYPE *types, int ntypes, int *type );

#endif
