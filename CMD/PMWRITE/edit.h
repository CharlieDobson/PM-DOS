/*
 * EDIT.H - a document being edited: the caret, the selection, the
 * window on the text, and what each key and command does to them.
 *
 * Nothing here draws or reads a key.  The screen code (VIEW.C) asks an
 * EDITOR where things are and tells it what the user did, in twips,
 * so the same editor serves the text screen and every graphics one.
 * Changing the kind of screen is ed_set_device: the document, the
 * caret and the selection stay as they are and the text is laid out
 * again for the new one.
 */
#ifndef EDIT_H
#define EDIT_H

#include "wdoc.h"
#include "wedit.h"
#include "lay.h"
#include "sys.h"

enum {
    MV_LEFT, MV_RIGHT, MV_UP, MV_DOWN, MV_WORD_LEFT, MV_WORD_RIGHT,
    MV_HOME, MV_END,            /* of the line */
    MV_PAGE_UP, MV_PAGE_DOWN,   /* a windowful */
    MV_TOP, MV_BOTTOM,          /* of the document */
    MV_PARA_UP, MV_PARA_DOWN
};

typedef struct {
    WDOC  *doc;
    STORY *story;               /* the story the caret is in */
    LAY   *lay;
    int    grid;                /* laid out for the text screen */
    WPOS   caret, anchor;       /* equal when nothing is selected */
    s32    want_x;              /* the x Up and Down keep to, or -1 */
    CHP    type_chp;            /* the look of the next character typed */
    int    type_set;            /* ...which the user chose, with nothing selected */
    s32    scroll_x, scroll_y;  /* the window's top left, in the story */
    WPOS   body_caret;          /* where the document's own text was left for a */
    s32    body_scroll;         /* header, a footer or a note: gone back to */
    s32    view_w, view_h;      /* the window */
    int    overtype;
    int    follow;              /* ed_update is to bring the caret into the window */
    int    no_memory;           /* the last command could not be done */
    u32    saved_serial;        /* the document's, when it was last saved */
    char   path[PATH_LEN];      /* "" for one never saved */
    int    format, variant;     /* how it is saved: FMT_* */
    int    untitled;            /* its number as "Document n" */
    int    read_only;
} EDITOR;

EDITOR *ed_new( WDOC *doc, int grid );          /* NULL: no memory */
void  ed_free( EDITOR *ed );                    /* and the document */
void  ed_set_device( EDITOR *ed, int grid );    /* 0 if there was no memory: unchanged */
void  ed_set_view( EDITOR *ed, s32 width, s32 height );
void  ed_set_story( EDITOR *ed, STORY *story ); /* the body, the header or the footer */
void  ed_update( EDITOR *ed );                  /* laid out, and the caret in the window */
void  ed_scroll_to( EDITOR *ed, s32 y );        /* the window's top, kept inside the text */
void  ed_scroll_by( EDITOR *ed, s32 lines );    /* the scroll bar's arrows */
int   ed_modified( const EDITOR *ed );
void  ed_mark_saved( EDITOR *ed );

/* the caret */
void  ed_move( EDITOR *ed, int how, int select );
void  ed_click( EDITOR *ed, s32 x, s32 y, int select );    /* a point of the story */
void  ed_goto( EDITOR *ed, WPOS pos, int select );
void  ed_select_word( EDITOR *ed );
void  ed_select_para( EDITOR *ed );
void  ed_select_all( EDITOR *ed );
int   ed_has_sel( const EDITOR *ed );
void  ed_sel( const EDITOR *ed, WPOS *from, WPOS *to );     /* in order */

/* typing */
void  ed_type( EDITOR *ed, u32 code );          /* a character, a tab */
void  ed_enter( EDITOR *ed );
void  ed_break( EDITOR *ed, u32 code );         /* WCH_LINE or WCH_PAGE */
void  ed_backspace( EDITOR *ed );
void  ed_delete( EDITOR *ed );
void  ed_delete_word( EDITOR *ed, int back );

/* the clipboard, which every document shares */
int   ed_can_paste( void );
void  ed_copy( EDITOR *ed );
void  ed_cut( EDITOR *ed );
void  ed_paste( EDITOR *ed );
void  ed_clip_free( void );

void  ed_undo( EDITOR *ed );
void  ed_redo( EDITOR *ed );

/* looks: of the selection, or of what is typed next */
void  ed_toggle( EDITOR *ed, u16 flag );                    /* a CF_* bit */
void  ed_set_chp( EDITOR *ed, u32 mask, const CHP *value, u16 flag_mask );
void  ed_set_pap( EDITOR *ed, u32 mask, const PAP *value, u16 flag_mask );
void  ed_set_style( EDITOR *ed, int style );
void  ed_indent( EDITOR *ed, int more );                    /* half an inch in or out */
/* what the toolbar shows: the look at the caret, and the caret's
   paragraph's own */
void  ed_cur_chp( EDITOR *ed, CHP *chp );
const PAP *ed_cur_pap( EDITOR *ed );

/* 1 = found, and selected.  Goes round the end of the document once. */
int   ed_find( EDITOR *ed, const u16 *what, u32 len, int flags );
/* the selection, if it is a match of "what", replaced: 1 if it was */
int   ed_replace( EDITOR *ed, const u16 *what, u32 len, const u16 *with, u32 with_len, int flags );
u32   ed_replace_all( EDITOR *ed, const u16 *what, u32 len, const u16 *with, u32 with_len, int flags );

/* a picture file's bytes put in at the caret: 0 if it could not be */
int   ed_insert_image( EDITOR *ed, const u8 *data, u32 len, int fmt, s32 width, s32 height );
/* another document's text put in at the caret */
void  ed_insert_doc( EDITOR *ed, WDOC *other );
/* an empty table at the caret, as wide as the text is */
void  ed_insert_table( EDITOR *ed, int rows, int cols );
int   ed_in_table( const EDITOR *ed );          /* the caret is in a table's cell */
/* Tab in a table: the caret to the next cell, or the one before.  0
   when the caret is not in a table, and then nothing was done. */
int   ed_cell_step( EDITOR *ed, int back );
int   ed_insert_row( EDITOR *ed );                /* an empty row below the caret's: 0 = not in a table */
int   ed_insert_row_above( EDITOR *ed );
int   ed_delete_rows( EDITOR *ed );               /* the rows the caret or the selection is in */
int   ed_insert_col( EDITOR *ed, int after );     /* 0: not in a table, or a row is full */
int   ed_delete_col( EDITOR *ed );
s32   ed_col_width( const EDITOR *ed );           /* of the caret's column, in twips: 0 = not in a table */
int   ed_set_col_width( EDITOR *ed, s32 width );
STORY *ed_insert_note( EDITOR *ed, int kind );    /* NOTE_FOOT or NOTE_END at the caret: its story, or NULL */
STORY *ed_note_near( EDITOR *ed );                /* the note called from nearest the caret, or NULL */

/* for the status bar: counted from 1 */
void  ed_where( EDITOR *ed, u32 *page, u32 *pages, u32 *line, u32 *col );

#endif
