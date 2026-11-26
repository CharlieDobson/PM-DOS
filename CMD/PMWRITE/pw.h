/*
 * PW.H - what the parts of PMWRITE's own program share.
 *
 *   MAIN.C   the command line, the documents that are open, the screen's
 *            layout, the loop, PMWRITE.INI
 *   VIEW.C   the document on the screen: as cells on a text screen, as
 *            type on a graphics one; a PDF's pages; the mouse in it
 *   BARS.C   the toolbar, the formatting line, the ruler, the status bar
 *   CMDS.C   the menus, the keys, and what each command does
 *   DLGS.C   the dialog boxes
 *   FILES.C  opening and saving, in whatever format
 *
 * Under them: DISP.H and UI.H (the screen and its menus and dialogs),
 * EDIT.H (a document being edited), LAY.H (its lines), FMT.H (files).
 */
#ifndef PW_H
#define PW_H

#include "fmt.h"
#include "edit.h"
#include "ui.h"
#include "tf.h"

#define PW_VERSION      "1.0"

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */
enum {
    CMD_NONE,
    CMD_NEW, CMD_OPEN, CMD_CLOSE, CMD_SAVE, CMD_SAVEAS, CMD_TOTEXT, CMD_PAGESETUP, CMD_PRINT,
    CMD_INFO, CMD_EXIT,
    CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_CLEAR, CMD_SELALL, CMD_FIND,
    CMD_FINDNEXT, CMD_REPLACE, CMD_GOTO,
    CMD_TOOLBAR, CMD_RULER, CMD_STATUSBAR, CMD_MARKS, CMD_ZOOM, CMD_DISPLAY, CMD_HEADER,
    CMD_FOOTER, CMD_BODY,
    CMD_BREAK, CMD_PAGENUM, CMD_NUMPAGES, CMD_DATE, CMD_TIME, CMD_INSFILE, CMD_PICTURE,
    CMD_FONT, CMD_PARAGRAPH, CMD_TABS, CMD_STYLE, CMD_BULLETS, CMD_NUMBERING,
    CMD_BOLD, CMD_ITALIC, CMD_UNDERLINE, CMD_SUPER, CMD_SUB,
    CMD_LEFT, CMD_CENTER, CMD_RIGHT, CMD_JUSTIFY, CMD_INDENT, CMD_OUTDENT,
    CMD_SPACE1, CMD_SPACE15, CMD_SPACE2,
    CMD_COUNT, CMD_TABLE, CMD_TABLEROW, CMD_ROWABOVE, CMD_ROWDEL, CMD_COLLEFT, CMD_COLRIGHT, CMD_COLDEL,
    CMD_COLWIDTH, CMD_FOOTNOTE, CMD_ENDNOTE, CMD_VIEWNOTE, CMD_NEXTWIN,
    CMD_PICKSTYLE, CMD_PICKFONT, CMD_PICKSIZE,
    CMD_PAGEPREV, CMD_PAGENEXT,
    CMD_KEYS, CMD_ABOUT,
    CMD_WINDOW                  /* CMD_WINDOW + n: the nth open document */
};

/* ------------------------------------------------------------------ */
/* the documents that are open                                         */
/* ------------------------------------------------------------------ */
#define MAX_DOCS        9

/* a PDF being looked at, which is pages and not text */
typedef struct {
    int      pages;
    int      page;                /* the one showing, from 0 */
    int      zoom;                /* percent; 0: as wide as the window */
    int      scroll_x, scroll_y;  /* pixels of the page at the window's top left */
    uint8_t *pix;                 /* the page as pixel numbers, when it has been drawn */
    int      pix_w, pix_h;
    int      pix_page, pix_zoom;  /* ...which page, and how big */
    int      pix_failed;          /* that page at that size could not be drawn */
    char   **lines;               /* a text screen: the page's words */
    int      nlines, lines_page;
    int      top;                 /* ...and the first line showing */
    char     password[48];        /* what opened it, for when it is opened again */
} PDFDOCVIEW;

typedef struct {
    EDITOR *ed;                 /* NULL for a PDF's pages */
    PDFDOCVIEW *pdf;            /* NULL for a document */
    char   path[PATH_LEN];      /* "" for one never saved */
    char   name[40];            /* what the title bar and Window menu call it */
} PWDOC;

extern PWDOC docs[MAX_DOCS];
extern int   ndocs;
extern int   cur_doc;           /* which one is on the screen */

#define ED              (docs[cur_doc].ed)
#define PDFV            (docs[cur_doc].pdf)

/* ------------------------------------------------------------------ */
/* settings: PMWRITE.INI                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    int show_toolbar, show_ruler, show_status, show_marks;
    int mode_w, mode_h;         /* the screen: 0 by lines for text, pixels for graphics */
    int zoom;                   /* percent */
    int inches;                 /* 1: lengths in inches; 0: centimeters */
    int find_case, find_word;
} SETTINGS;

extern SETTINGS set;

/* ------------------------------------------------------------------ */
/* MAIN.C                                                              */
/* ------------------------------------------------------------------ */
enum { PANE_TOOLBAR, PANE_RULER, PANE_PAGE };

/* where the parts of the screen are, in cells: app_layout works it out
   from the screen's size and what is switched on */
typedef struct {
    int title_row;              /* -1 when there is none */
    int menu_row;
    int tool_row, tool_rows;    /* the button bar: graphics only */
    int ribbon_row;             /* -1 when it is off */
    int ruler_row;
    int doc_top, doc_rows;      /* the document: columns 0 to doc_cols - 1 */
    int doc_cols;               /* ...and the scroll bar is in the column after */
    int status_row;             /* -1 when it is off */
} SCREENPLAN;

extern SCREENPLAN plan;

void  app_layout( void );
void  app_quit( void );                         /* File > Exit: asks about what is unsaved */
void  app_message( const char *text );          /* the status bar's, until the next key */
int   doc_add( EDITOR *ed, PDFDOCVIEW *pdf, const char *path );   /* -> its number, -1: no room */
void  doc_close( int index );
void  doc_show( int index );
void  doc_name( int index );                    /* its title, from its path */
int   doc_find( const char *path );             /* the one that is that file, or -1 */
void  settings_save( void );
void  screen_apply( int mode_index );           /* another mode: everything laid out again */
void  no_memory( void );                        /* said to the user */

/* ------------------------------------------------------------------ */
/* VIEW.C                                                              */
/* ------------------------------------------------------------------ */
void  view_init( void );
void  view_draw( void );                        /* the document's part of the screen */
int   view_after_draw( void );                  /* once the screen is out: 1 = draw it again */
void  view_reset( void );                       /* another document, or another screen */
void  view_mouse( void );                       /* a K_MOUSE in the document */
void  view_wheel( int notches );
int   view_px( int twips );                     /* a length as pixels at the zoom now */
void  view_free_pdf( PDFDOCVIEW *pdf );
void  view_pdf_key( int key );                  /* a key, when a PDF is showing */
int   view_reclaim( void );                     /* caches given back: 1 if any */

/* ------------------------------------------------------------------ */
/* BARS.C                                                              */
/* ------------------------------------------------------------------ */
/* a toolbar button (ICONS.C has them): sixteen rows of sixteen pixels,
   the command it gives, and whether a gap comes before it */
typedef struct {
    const char *const *rows;
    int   cmd;
    int   gap;
} TOOLBTN;

extern const TOOLBTN tool_buttons[];
extern const int tool_count;

void  bars_init( void );
void  bars_draw( void );                        /* toolbar, formatting line, ruler, status bar */
int   bars_mouse( void );                       /* a K_MOUSE: -> a command, 0, or -1 "not mine" */
void  bars_changed( void );                     /* what they show may have changed */

/* ------------------------------------------------------------------ */
/* CMDS.C                                                              */
/* ------------------------------------------------------------------ */
void  cmds_init( void );                        /* the menus */
int   cmd_of_key( int key );                    /* a shortcut's command, or 0 */
void  cmd_do( int cmd );
void  cmd_key( int key );                       /* a key that is no shortcut: typing, the arrows */

/* ------------------------------------------------------------------ */
/* DLGS.C                                                              */
/* ------------------------------------------------------------------ */
void  dlg_font( void );
void  dlg_paragraph( void );
void  dlg_tabs( void );
void  dlg_page_setup( void );
void  dlg_find( int replace );
void  find_again( void );
void  dlg_goto( void );
void  dlg_zoom( void );
void  dlg_display( void );
void  dlg_break( void );
void  dlg_table( void );
void  dlg_col_width( void );
void  dlg_info( void );
void  dlg_count( void );
void  dlg_keys( void );
void  dlg_about( void );
int   dlg_pick( const char *title, const char *const *names, int count, int start );  /* -> index or -1 */
/* a length as the user reads it ("1.25" or "3.18 cm"), and back: twips */
void  len_text( int twips, char *buf );
int   len_parse( const char *text, int *twips );

/* ------------------------------------------------------------------ */
/* FILES.C                                                             */
/* ------------------------------------------------------------------ */
int      file_open( const char *path, int as_text );  /* 1 = it is open and showing */
void     file_open_ask( void );
int      file_save( int index, int ask );             /* 1 = saved */
int      file_close_ask( int index );                 /* 1 = it may go (saved, or not wanted) */
void     file_new( void );
void     file_insert( void );
void     file_picture( void );
void     file_print( void );
void     file_to_text( void );                        /* a PDF's words as a document */
uint8_t *file_read( const char *path, uint32_t *len, int quiet ); /* the whole file, or NULL */

#endif
