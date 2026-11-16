/*
 * ETEST.C - a test of WEDIT.C on Windows: editing commands done to a
 * document, undone and redone, with the document's dump compared at
 * each stage.  Built by MKETEST.CMD; nothing here ships.
 *
 *   ETEST            runs every check: "ok" lines, and exit code 0
 *                    when none failed
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmt.h"
#include "wedit.h"

static int failures;

int mem_reclaim( void )
{
    return 0;
}

void out_of_memory( void )
{
    fprintf( stderr, "ETEST: out of memory\n" );
    exit( 8 );
}

/* the document's dump, as a C string the caller frees */
static char *dump_of( WDOC *doc )
{
    OUT *out = out_mem();
    u8 *data;
    u32 len;
    char *text;

    wd_dump( doc, out );
    data = out_data( out, &len );
    text = (char *)malloc( len + 1 );
    memcpy( text, data, len );
    text[len] = 0;
    out_close( out );
    return text;
}

static void check( const char *what, int good )
{
    printf( "%s  %s\n", good ? "ok  " : "FAIL", what );
    if ( !good ) {
        failures++;
    }
}

static void same( const char *what, const char *lhs, const char *rhs )
{
    check( what, strcmp( lhs, rhs ) == 0 );
    if ( strcmp( lhs, rhs ) != 0 ) {
        printf( "---- one\n%s---- other\n%s----\n", lhs, rhs );
    }
}

static u32 wide( const char *text, u16 *out )
{
    u32 len = 0;

    while ( text[len] ) {
        out[len] = (u16)(u8)text[len];
        len++;
    }
    return len;
}

static WPOS at( u32 para, u32 off )
{
    WPOS pos;

    pos.para = para;
    pos.off = off;
    return pos;
}

static void type( WDOC *doc, WPOS *pos, const char *text, u16 chp )
{
    u16 buf[256];
    u32 len = wide( text, buf );

    we_begin( doc, &doc->body, *pos );
    we_insert( doc, &doc->body, pos, buf, len, chp );
    check( "insert ends", we_end( doc, *pos ) );
}

/* the body's text, paragraphs divided by '|' */
static char *plain( WDOC *doc )
{
    static char buf[4096];
    u32 pi, index, used = 0;

    for ( pi = 0; pi < doc->body.count; pi++ ) {
        if ( pi ) {
            buf[used++] = '|';
        }
        for ( index = 0; index < doc->body.para[pi]->len && used < sizeof( buf ) - 2; index++ ) {
            buf[used++] = (char)(doc->body.para[pi]->text[index] == WCH_OBJ ? '#' :
                                 doc->body.para[pi]->text[index]);
        }
    }
    buf[used] = 0;
    return buf;
}

int main( void )
{
    WDOC *doc = wd_new(), *piece, *other, *table;
    STORY *story;
    WPOS pos, hit;
    CHP chp;
    PAP pap;
    WTAB tabs[2];
    char *empty, *stage[10];
    u16 buf[64];
    u16 bold;
    u32 len;
    int step, img;
    static const u8 fake_png[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };

    wd_finish( doc );
    empty = dump_of( doc );

    /* typing, a split, a join */
    pos = at( 0, 0 );
    type( doc, &pos, "Hello world", 0 );
    same( "typed", plain( doc ), "Hello world" );
    stage[0] = dump_of( doc );

    pos = at( 0, 5 );
    we_begin( doc, &doc->body, pos );
    we_split( doc, &doc->body, &pos );
    check( "split ends", we_end( doc, pos ) );
    same( "split", plain( doc ), "Hello| world" );
    check( "caret after split", pos.para == 1 && pos.off == 0 );
    stage[1] = dump_of( doc );

    /* bold on "world", by a change of looks */
    mem_set( &chp, 0, sizeof( chp ) );
    chp.flags = CF_BOLD;
    we_begin( doc, &doc->body, pos );
    we_set_chp( doc, &doc->body, at( 1, 1 ), at( 1, 6 ), CM_FLAGS, &chp, CF_BOLD );
    check( "bold ends", we_end( doc, pos ) );
    check( "all bold", we_all_have( doc, &doc->body, at( 1, 1 ), at( 1, 6 ), CF_BOLD ) );
    check( "not all bold", !we_all_have( doc, &doc->body, at( 1, 0 ), at( 1, 6 ), CF_BOLD ) );
    stage[2] = dump_of( doc );

    /* a paragraph property with tabs */
    pap_init( &pap );
    pap.align = AL_CENTER;
    tabs[0].pos = 2880;
    tabs[0].kind = TAB_RIGHT;
    tabs[0].lead = LEAD_DOTS;
    tabs[0].spare = 0;
    tabs[1].pos = 1440;
    tabs[1].kind = TAB_LEFT;
    tabs[1].lead = LEAD_NONE;
    tabs[1].spare = 0;
    pap.tabs = tabs;
    pap.ntabs = 2;
    we_begin( doc, &doc->body, pos );
    we_set_pap( doc, &doc->body, 0, 1, PM_ALIGN | PM_TABS, &pap, 0 );
    check( "pap ends", we_end( doc, pos ) );
    check( "tabs sorted", doc->body.para[0]->pap.ntabs == 2 && doc->body.para[0]->pap.tabs[0].pos == 1440 );
    stage[3] = dump_of( doc );

    /* a picture, then more paragraphs, then a delete across them */
    img = wd_image_new( doc, fake_png, sizeof( fake_png ), IMG_PNG, 1440, 720 );
    pos = at( 1, 3 );
    we_begin( doc, &doc->body, pos );
    we_insert_obj( doc, &doc->body, &pos, img, 0 );
    check( "picture ends", we_end( doc, pos ) );
    same( "picture", plain( doc ), "Hello| wo#rld" );
    check( "picture anchored", wd_obj_at( doc->body.para[1], 3 ) == img );

    pos = wpos_end( &doc->body );
    we_begin( doc, &doc->body, pos );
    we_split( doc, &doc->body, &pos );
    len = wide( "third", buf );
    we_insert( doc, &doc->body, &pos, buf, len, 0 );
    we_split( doc, &doc->body, &pos );
    len = wide( "fourth line", buf );
    we_insert( doc, &doc->body, &pos, buf, len, 0 );
    check( "two more end", we_end( doc, pos ) );
    same( "four paragraphs", plain( doc ), "Hello| wo#rld|third|fourth line" );
    stage[4] = dump_of( doc );

    we_begin( doc, &doc->body, pos );
    we_delete( doc, &doc->body, at( 0, 3 ), at( 3, 7 ) );
    check( "delete ends", we_end( doc, at( 0, 3 ) ) );
    same( "deleted across", plain( doc ), "Helline" );
    check( "one paragraph", doc->body.count == 1 );
    stage[5] = dump_of( doc );

    /* everything undone, a stage at a time, and done again */
    for ( step = 6; step > 0; step-- ) {
        check( "undo", we_undo( doc, &story, &pos ) );
        if ( step == 6 ) {
            same( "undo delete", dump_of( doc ), stage[4] );
            check( "picture back", wd_obj_at( doc->body.para[1], 3 ) == img );
        }
    }
    same( "undone to typed", dump_of( doc ), stage[0] );
    check( "undo typing", we_undo( doc, &story, &pos ) );
    same( "undone to empty", dump_of( doc ), empty );
    check( "nothing left to undo", !we_undo( doc, &story, &pos ) );
    for ( step = 0; step < 7; step++ ) {
        check( "redo", we_redo( doc, &story, &pos ) );
        if ( step == 1 ) {
            same( "redo split", dump_of( doc ), stage[1] );
        }
        if ( step == 3 ) {
            same( "redo pap", dump_of( doc ), stage[3] );
        }
    }
    same( "redone to the end", dump_of( doc ), stage[5] );
    check( "nothing left to redo", !we_redo( doc, &story, &pos ) );
    check( "undo again", we_undo( doc, &story, &pos ) );
    same( "back to four", dump_of( doc ), stage[4] );

    /* copy two and a half paragraphs, paste into another document */
    piece = we_copy( doc, &doc->body, at( 1, 1 ), at( 3, 6 ) );
    check( "copied", piece != NULL );
    same( "piece", plain( piece ), "wo#rld|third|fourth" );
    other = wd_new();
    wd_finish( other );
    pos = at( 0, 0 );
    type( other, &pos, "AB", 0 );
    pos = at( 0, 1 );
    we_begin( other, &other->body, pos );
    we_paste( other, &other->body, &pos, piece );
    check( "paste ends", we_end( other, pos ) );
    same( "pasted", plain( other ), "Awo#rld|third|fourthB" );
    check( "caret after paste", pos.para == 2 && pos.off == 6 );
    check( "pasted picture", other->nobj == 1 && other->obj[0].len == sizeof( fake_png ) &&
           wd_obj_at( other->body.para[0], 3 ) == 0 );
    bold = other->body.para[0]->attr[1];
    check( "pasted bold", (wd_chp_get( other, bold )->flags & CF_BOLD) != 0 );
    check( "pasted alignment", other->body.para[0]->pap.align == AL_CENTER &&
           other->body.para[2]->pap.align == AL_LEFT );
    check( "undo paste", we_undo( other, &story, &pos ) );
    same( "paste undone", plain( other ), "AB" );
    check( "redo paste", we_redo( other, &story, &pos ) );
    same( "paste redone", plain( other ), "Awo#rld|third|fourthB" );

    /* find */
    len = wide( "LINE", buf );
    check( "find, any case", we_find( doc, &doc->body, at( 0, 0 ), buf, len, 0, &hit ) &&
           hit.para == 3 && hit.off == 7 );
    check( "find, exact case", !we_find( doc, &doc->body, at( 0, 0 ), buf, len, WF_CASE, &hit ) );
    len = wide( "ir", buf );
    check( "find a piece", we_find( doc, &doc->body, at( 0, 0 ), buf, len, 0, &hit ) && hit.para == 2 );
    check( "not a whole word", !we_find( doc, &doc->body, at( 0, 0 ), buf, len, WF_WORD, &hit ) );
    len = wide( "l", buf );
    check( "find backwards", we_find( doc, &doc->body, wpos_end( &doc->body ), buf, len, WF_BACK, &hit ) &&
           hit.para == 3 && hit.off == 7 );

    /* a table of two rows of two cells; text deleted across a cell's
       end, which joins two cells into one; and that taken back, which
       has to put the cell's end where it was */
    table = wd_new();
    wd_finish( table );
    pos = at( 0, 0 );
    we_begin( table, &table->body, pos );
    we_insert_table( table, &table->body, &pos, 2, 2, 8640 );
    check( "table ends", we_end( table, pos ) );
    check( "table paragraphs", table->body.count == 5 );
    check( "row end", (table->body.para[1]->pap.flags & PF_ROWEND) != 0 && table->body.para[1]->pap.row != NULL &&
           table->body.para[1]->pap.row->ncells == 2 && table->body.para[1]->pap.row->edge[1] == 8640 );
    check( "after the table", (table->body.para[4]->pap.flags & PF_INTABLE) == 0 );
    pos = at( 0, 0 );
    type( table, &pos, "ab", 0 );
    pos = at( 1, 0 );
    type( table, &pos, "cd", 0 );
    stage[6] = dump_of( table );
    we_begin( table, &table->body, pos );
    we_delete( table, &table->body, at( 0, 1 ), at( 1, 1 ) );
    check( "cell delete ends", we_end( table, at( 0, 1 ) ) );
    same( "cells joined", plain( table ), "ad|||" );
    check( "undo cell delete", we_undo( table, &story, &pos ) );
    same( "the cell's end is back", dump_of( table ), stage[6] );
    check( "redo cell delete", we_redo( table, &story, &pos ) );
    same( "cells joined again", plain( table ), "ad|||" );
    check( "undo it again", we_undo( table, &story, &pos ) );
    same( "the cell's end is back again", dump_of( table ), stage[6] );
    /* a third row, added after the second and taken back */
    we_begin( table, &table->body, pos );
    we_add_row( table, &table->body, 3, &pos );
    check( "row ends", we_end( table, pos ) );
    check( "row added", table->body.count == 7 && pos.para == 4 && pos.off == 0 &&
           (table->body.para[3]->pap.flags & PF_ROWEND) != 0 && table->body.para[3]->pap.row != NULL &&
           (table->body.para[4]->pap.flags & (PF_INTABLE | PF_CELLEND | PF_ROWEND)) == (PF_INTABLE | PF_CELLEND) &&
           (table->body.para[5]->pap.flags & PF_ROWEND) != 0 && table->body.para[5]->pap.row != NULL &&
           table->body.para[5]->pap.row->ncells == 2 && (table->body.para[6]->pap.flags & PF_INTABLE) == 0 );
    check( "undo row", we_undo( table, &story, &pos ) );
    same( "row gone", dump_of( table ), stage[6] );

    /* a column put in to the right of the first: every row has three
       cells, held to the width the table had */
    pos = at( 0, 0 );
    we_begin( table, &table->body, pos );
    we_insert_col( table, &table->body, 0, 1, 8640, &pos );
    check( "column ends", we_end( table, pos ) );
    same( "column added", plain( table ), "ab||cd||||" );
    check( "column's place", pos.para == 1 && pos.off == 0 && we_cell_of( &table->body, 1 ) == 1 &&
           we_cell_of( &table->body, 2 ) == 2 && we_cell_of( &table->body, 6 ) == -1 );
    check( "column's edges", table->body.para[2]->pap.row != NULL && table->body.para[2]->pap.row->ncells == 3 &&
           table->body.para[2]->pap.row->edge[0] == 2880 && table->body.para[2]->pap.row->edge[1] == 5760 &&
           table->body.para[2]->pap.row->edge[2] == 8640 && table->body.para[5]->pap.row != NULL &&
           table->body.para[5]->pap.row->ncells == 3 && (table->body.para[1]->pap.flags & PF_ROWEND) == 0 &&
           (table->body.para[6]->pap.flags & PF_INTABLE) == 0 );
    check( "undo column", we_undo( table, &story, &pos ) );
    same( "column gone", dump_of( table ), stage[6] );
    check( "redo column", we_redo( table, &story, &pos ) );
    same( "column back", plain( table ), "ab||cd||||" );
    pos = at( 1, 0 );
    type( table, &pos, "x", 0 );
    stage[7] = dump_of( table );
    /* ...and taken out again with what was typed in it */
    we_begin( table, &table->body, pos );
    we_delete_col( table, &table->body, 1, &pos );
    check( "column delete ends", we_end( table, pos ) );
    same( "column deleted", plain( table ), "ab|cd|||" );
    check( "two cells a row", table->body.para[1]->pap.row != NULL && table->body.para[1]->pap.row->ncells == 2 &&
           table->body.para[1]->pap.row->edge[0] == 2880 && table->body.para[1]->pap.row->edge[1] == 5760 &&
           table->body.para[3]->pap.row != NULL && table->body.para[3]->pap.row->ncells == 2 && pos.para == 1 );
    check( "undo column delete", we_undo( table, &story, &pos ) );
    same( "the column is back", dump_of( table ), stage[7] );
    check( "redo column delete", we_redo( table, &story, &pos ) );
    same( "column deleted again", plain( table ), "ab|cd|||" );
    /* a column's width */
    check( "width read", we_col_width( &table->body, 0 ) == 2880 && we_col_width( &table->body, 4 ) == 0 );
    we_begin( table, &table->body, pos );
    we_set_col_width( table, &table->body, 0, 1440 );
    check( "width ends", we_end( table, pos ) );
    check( "width set", table->body.para[1]->pap.row->edge[0] == 1440 && table->body.para[1]->pap.row->edge[1] == 4320 &&
           table->body.para[3]->pap.row->edge[0] == 1440 && table->body.para[3]->pap.row->edge[1] == 4320 );
    stage[8] = dump_of( table );
    /* a row above the first, and the same row taken out */
    we_begin( table, &table->body, pos );
    we_add_row_above( table, &table->body, 1, &pos );
    check( "row above ends", we_end( table, pos ) );
    same( "row above", plain( table ), "||ab|cd|||" );
    check( "row above is a row", pos.para == 0 && table->body.para[1]->pap.row != NULL &&
           table->body.para[1]->pap.row->ncells == 2 && table->body.para[1]->pap.row->edge[1] == 4320 &&
           (table->body.para[0]->pap.flags & (PF_INTABLE | PF_CELLEND | PF_ROWEND)) == (PF_INTABLE | PF_CELLEND) );
    we_begin( table, &table->body, pos );
    we_delete_rows( table, &table->body, 0, 1, &pos );
    check( "row delete ends", we_end( table, pos ) );
    same( "the row is gone", dump_of( table ), stage[8] );
    check( "undo row delete", we_undo( table, &story, &pos ) );
    same( "the row is back", plain( table ), "||ab|cd|||" );
    check( "redo row delete", we_redo( table, &story, &pos ) );
    same( "the row is gone again", dump_of( table ), stage[8] );
    /* the last column, then the only one: the rows go with it, and the
       paragraph after the table is what is left */
    we_begin( table, &table->body, pos );
    we_delete_col( table, &table->body, 1, &pos );
    check( "last column ends", we_end( table, pos ) );
    same( "last column deleted", plain( table ), "ab||" );
    check( "one cell a row", table->body.count == 3 && table->body.para[0]->pap.row != NULL &&
           table->body.para[0]->pap.row->ncells == 1 && table->body.para[0]->pap.row->edge[0] == 1440 &&
           (table->body.para[0]->pap.flags & PF_ROWEND) != 0 && table->body.para[1]->pap.row != NULL &&
           (table->body.para[2]->pap.flags & PF_INTABLE) == 0 );
    stage[9] = dump_of( table );
    we_begin( table, &table->body, pos );
    we_delete_col( table, &table->body, 0, &pos );
    check( "only column ends", we_end( table, pos ) );
    check( "the table is gone", table->body.count == 1 && table->body.para[0]->len == 0 &&
           (table->body.para[0]->pap.flags & PF_INTABLE) == 0 && table->body.para[0]->pap.row == NULL );
    check( "undo only column", we_undo( table, &story, &pos ) );
    same( "one column again", dump_of( table ), stage[9] );
    /* every row of a table that starts the document */
    we_begin( table, &table->body, pos );
    we_delete_rows( table, &table->body, 0, 1, &pos );
    check( "all rows end", we_end( table, pos ) );
    check( "all rows gone", table->body.count == 1 && table->body.para[0]->len == 0 &&
           (table->body.para[0]->pap.flags & PF_INTABLE) == 0 && pos.para == 0 );
    check( "undo all rows", we_undo( table, &story, &pos ) );
    same( "the rows are back", dump_of( table ), stage[9] );
    while ( we_undo( table, &story, &pos ) ) {
    }
    check( "table undone", table->body.count == 1 && table->body.para[0]->len == 0 &&
           (table->body.para[0]->pap.flags & PF_INTABLE) == 0 );
    wd_free( table );

    /* two hundred and fifty actions: the oldest are forgotten, the
       rest still undo */
    pos = wpos_end( &doc->body );
    for ( step = 0; step < 250; step++ ) {
        type( doc, &pos, "x", 0 );
    }
    for ( step = 0; we_undo( doc, &story, &pos ); step++ ) {
    }
    check( "two hundred undone", step == 200 );

    wd_free( piece );
    wd_free( other );
    wd_free( doc );
    printf( "heap in use at the end: %lu\n", (unsigned long)heap_in_use() );
    printf( failures ? "%d FAILED\n" : "all passed\n", failures );
    return failures != 0;
}
