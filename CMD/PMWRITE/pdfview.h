/*
 * PDFVIEW.H - a PDF file's pages as pictures and as lines of text, for
 * the part of PMWRITE that shows them.
 *
 * The PDF code in PDF\ is ACROREAD's, and asks a good deal of whoever
 * calls it: a place to jump to when a file turns out to be damaged, a
 * pool of memory for each page, a canvas laid out its way.  These few
 * calls are all of that done once.  None of them ends the program,
 * whatever is in the file and however little memory there is: each
 * answers with a reason, PV_*.
 *
 * ONE PDF IS OPEN AT A TIME.  Opening another closes the one that was.
 * So does reading a PDF as a document to be edited (F_PDF.C): after
 * fmt_pdf's load, nothing is open here.
 *
 * MEMORY.  The PDF code takes what it needs with xalloc and its pools
 * (RT.H), which call the program's mem_reclaim and, when that has no
 * more to give, its out_of_memory.  Both must do their part:
 *
 *     int mem_reclaim( void )
 *     {
 *         return pdfview_reclaim() || ...the program's own caches...;
 *     }
 *
 *     void out_of_memory( void )
 *     {
 *         pdfview_out_of_memory();     - does not come back if a call
 *                                        here was what ran out
 *         ...the program's own way out...
 *     }
 *
 * Then a page too big for the machine is PV_NOMEM and nothing worse.
 */
#ifndef PDFVIEW_H
#define PDFVIEW_H

#include "types.h"

#define PV_OK           0
#define PV_NOMEM        1       /* not enough memory for it */
#define PV_DAMAGED      2       /* the file is damaged past reading */
#define PV_NOTPDF       3       /* it is not a PDF file */
#define PV_PASSWORD     4       /* it wants a password: pdfview_password */
#define PV_CRYPT        5       /* protected in a way that is not read */
#define PV_IO           6       /* it could not be opened or read */
#define PV_ABORT        7       /* the poll said to stop */
#define PV_NOPAGE       8       /* there is no such page, or nothing is open */
#define PV_PARAM        9       /* a block that cannot be drawn into */

/* pixels to a point, 16.16, for a screen of "dpi" dots to the inch
   showing a page at "percent" of its size: PV_ZOOM( 96, 100 ) */
#define PV_ZOOM( dpi, percent ) \
    ((s32)((u32)(dpi) * 65536UL / 72UL * (u32)(percent) / 100UL))

/* A file by name, or one that is in memory already (and stays there,
   unmoved, until it is closed).  PV_PASSWORD leaves the file waiting
   for pdfview_password; any other failure leaves nothing open.  A file
   whose password is the empty one opens without asking. */
int   pdfview_open( const char *path );
int   pdfview_open_mem( const u8 *data, u32 len );
/* PV_OK: it was the user's or the owner's password, and the file is
   open.  PV_PASSWORD: it was neither; the file still waits. */
int   pdfview_password( const char *text, u32 len );
void  pdfview_close( void );            /* harmless when nothing is open */
int   pdfview_is_open( void );
int   pdfview_pages( void );            /* 0 when nothing is open */

/* A page's width and height in points, 16.16, as it is meant to be
   looked at - turned as the file says it is.  Pages count from 0. */
int   pdfview_page_size( int page, s32 *width, s32 *height );
/* ...and in pixels at a zoom, turned a further 0, 90, 180 or 270
   degrees clockwise: the size of the block that holds all of it */
int   pdfview_page_pixels( int page, s32 zoom, int turn, int *width, int *height );

/*
 * Where a page is drawn: the caller's memory.  A row is "width" pixels
 * of one byte each (bpp 1: gray, 0 black and 255 white) or of three
 * (bpp 3: blue, green, red), and rows are "stride" bytes apart, the
 * top one first.  The block need not be the whole page: org_x and
 * org_y say which pixel of the page is its top left corner, so a page
 * can be drawn a tile at a time, or only as much of it as is on show.
 */
typedef struct {
    u8  *pix;
    int  width, height;
    int  bpp;
    u32  stride;
    int  org_x, org_y;
} PVBLOCK;

/* The block is made white and the page drawn into it.  When the answer
   is not PV_OK the block has what was drawn before the page was given
   up, which is worth showing. */
int   pdfview_render( int page, s32 zoom, int turn, const PVBLOCK *block );

/* "poll" is called every hundred or so of a page's operators while it
   is drawn; a 1 from it gives the page up (PV_ABORT).  NULL for none.
   A single large image is not interrupted. */
void  pdfview_poll( int (*poll)( void ) );

/* A page's text as lines, top to bottom, in code page 437, with spaces
   where the page had columns.  One block from the heap - the lines'
   addresses and then the lines, each ending in a NUL - which is the
   caller's to xfree.  A page with no text is PV_OK and no lines. */
int   pdfview_text( int page, char ***lines, int *count );

/* the program's part: see MEMORY above */
int   pdfview_reclaim( void );          /* 1 if something was given back */
void  pdfview_out_of_memory( void );

/* The password fmt_pdf's load tries on a file that wants one: the text
   is not copied, so it must still be there when load is called.  NULL
   takes it away again.  (F_PDF.C) */
void  fmt_pdf_password( const char *text );

/*
 * A JPEG picture that is not in a PDF: a whole .JPG file in memory,
 * decoded.  The answer is a block from the heap, the caller's to
 * xfree: width * height pixels of three bytes - blue, green, red -
 * the top row first.  NULL if it is not a JPEG of the kind that is
 * read (baseline, eight bits; not progressive, not arithmetic-coded),
 * if it is damaged, if it is more than 4096 pixels either way or more
 * than 16 megabytes decoded, or if there is no memory.  A gray picture
 * comes back with its three bytes equal; one in four inks is turned
 * into light as a PDF's is.  No PDF need be open, and one that is open
 * is not disturbed.
 */
u8   *pdfview_jpeg( const u8 *data, u32 len, u32 *width, u32 *height );

#endif
