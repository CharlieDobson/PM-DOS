/*
 * WEDIT.H - changing a document, in ways that can be taken back.
 *
 * Everything the user does to a document goes through here.  A command
 * is an ACTION: we_begin, some changes, we_end.  Each change leaves
 * behind a record of how to undo it, the records of one action are
 * undone together, and undoing writes the records that redo it - so
 * Undo and Redo are the same code going opposite ways.
 *
 * WHEN MEMORY RUNS OUT in the middle of an action, we_end takes back
 * what the action had done so far, forgets the undo history (which
 * could no longer be trusted) and answers 0.  The document is whole
 * and as it was before the command; the caller says so to the user.
 *
 * A reader of a file does NOT come through here: WDOC.H's calls are
 * the ones that leave no records.
 */
#ifndef WEDIT_H
#define WEDIT_H

#include "wdoc.h"

void  we_begin( WDOC *doc, STORY *story, WPOS caret );
int   we_end( WDOC *doc, WPOS caret );          /* 0 = no memory: taken back */
/* an action that is more of the one before it - the next letter of a
   word being typed - and is undone with it: when the one before had
   the same "kind" (any number but 0) and ended where this starts.
   Otherwise it is we_begin. */
void  we_begin_more( WDOC *doc, STORY *story, WPOS caret, int kind );

/* text at a place, which moves past it.  No paragraph ends in "text". */
void  we_insert( WDOC *doc, STORY *story, WPOS *pos, const uint16_t *text, unsigned int len, uint16_t chp );
/* an object of the document's, as one character */
void  we_insert_obj( WDOC *doc, STORY *story, WPOS *pos, int obj, uint16_t chp );
/* the paragraph ended at a place, which becomes the start of the new one */
void  we_split( WDOC *doc, STORY *story, WPOS *pos );
/* everything from one place up to another, paragraph ends and all */
void  we_delete( WDOC *doc, STORY *story, WPOS from, WPOS to );
/* an empty table of "rows" by "cols" cells, "width" wide in all, put in
   at a place - which becomes its first cell.  Text after the place
   goes below the table.  Nothing happens inside another table. */
void  we_insert_table( WDOC *doc, STORY *story, WPOS *pos, int rows, int cols, int width );
/* an empty row after the row that paragraph "row_end" ends; *pos is put in its first cell */
void  we_add_row( WDOC *doc, STORY *story, unsigned int row_end, WPOS *pos );
/* ...and one above the row paragraph "pi" is in */
void  we_add_row_above( WDOC *doc, STORY *story, unsigned int pi, WPOS *pos );

/* the row paragraph "pi" is in: its first paragraph and its last, which
   has the row's TROW.  0 if "pi" is not in a table's row. */
int   we_row_of( const STORY *story, unsigned int pi, unsigned int *first, unsigned int *end );
/* which of its row's cells paragraph "pi" is in, from 0: -1 if in none */
int   we_cell_of( const STORY *story, unsigned int pi );

/* the rows paragraphs "from" to "to" are in, taken out with what is in
   them; *pos is put where they were.  Both must be in one table. */
void    we_delete_rows( WDOC *doc, STORY *story, unsigned int from, unsigned int to, WPOS *pos );
/* an empty column beside the one paragraph "pi" is in - to its right if
   "after" - in every row of the table, as wide as that column; if the
   table would then pass "limit", every column is narrowed to fit.  *pos
   is put in the new cell of the row "pi" is in.  Nothing happens when a
   row already has MAX_CELLS cells. */
void    we_insert_col( WDOC *doc, STORY *story, unsigned int pi, int after, int limit, WPOS *pos );
/* the column paragraph "pi" is in, taken out of every row that has it;
   a row whose only column it is goes whole */
void    we_delete_col( WDOC *doc, STORY *story, unsigned int pi, WPOS *pos );
/* how wide that column is in the row "pi" is in (0: not in a table),
   and the column made "width" wide in every row */
int     we_col_width( const STORY *story, unsigned int pi );
void    we_set_col_width( WDOC *doc, STORY *story, unsigned int pi, int width );

/* which parts of a CHP a change sets */
#define CM_FONT         0x0001
#define CM_SIZE         0x0002
#define CM_COLOR        0x0004
#define CM_HILITE       0x0008
#define CM_FLAGS        0x0010  /* the CF_ bits in "flag_mask", and no others */
void  we_set_chp( WDOC *doc, STORY *story, WPOS from, WPOS to, uint32_t mask, const CHP *value,
                  uint16_t flag_mask );
/* one CHP with the same change made to it: what typing will use */
void  chp_apply( CHP *chp, uint32_t mask, const CHP *value, uint16_t flag_mask );
/* 1 if every character from one place to another has all of "flags" */
int   we_all_have( WDOC *doc, STORY *story, WPOS from, WPOS to, uint16_t flags );

/* which parts of a PAP a change sets */
#define PM_ALIGN        0x0001
#define PM_LEFT         0x0002
#define PM_RIGHT        0x0004
#define PM_FIRST        0x0008
#define PM_BEFORE       0x0010
#define PM_AFTER        0x0020
#define PM_LINE         0x0040  /* line and line_rule */
#define PM_LIST         0x0080  /* list, level and list_start */
#define PM_STYLE        0x0100
#define PM_TABS         0x0200
#define PM_FLAGS        0x0400  /* the PF_ bits in "flag_mask" */
void  we_set_pap( WDOC *doc, STORY *story, unsigned int first, unsigned int last, uint32_t mask, const PAP *value,
                  uint16_t flag_mask );

int   we_can_undo( const WDOC *doc );
int   we_can_redo( const WDOC *doc );
/* 1 = something was undone: where the caret was before it, and in
   which story */
int   we_undo( WDOC *doc, STORY **story, WPOS *caret );
int   we_redo( WDOC *doc, STORY **story, WPOS *caret );
void  we_forget( WDOC *doc );                   /* the history, not the document */

/* a number that is different after every change, undo and redo: how
   the screen knows to lay the document out again, and File > Save
   that there is something to save */
unsigned int we_serial( const WDOC *doc );

/*
 * A piece of a document as a document of its own, which is what the
 * clipboard holds: its body's paragraphs are the piece's, and the last
 * one is the text after the last paragraph end taken (empty, when the
 * piece ended with one).  NULL if there was no memory.
 */
WDOC *we_copy( WDOC *doc, STORY *story, WPOS from, WPOS to );
/* ...put into a document at a place, which moves past it.  The piece
   may come from another document: its fonts and looks are found, or
   made, in this one. */
void  we_paste( WDOC *doc, STORY *story, WPOS *pos, WDOC *piece );

/* looking for text */
#define WF_CASE         0x01    /* capitals matter */
#define WF_WORD         0x02    /* whole words only */
#define WF_BACK         0x04    /* towards the start */
/* from a place on (or back): 1 and where it starts, in one paragraph.
   The search stops at the story's end; the caller starts again from
   the other end if it wants to go round. */
int   we_find( WDOC *doc, STORY *story, WPOS from, const uint16_t *what, unsigned int len, int flags, WPOS *at );

/* places */
int   wpos_cmp( WPOS lhs, WPOS rhs );           /* <0, 0, >0 */
WPOS  wpos_clamp( const STORY *story, WPOS pos );
WPOS  wpos_end( const STORY *story );

#endif
