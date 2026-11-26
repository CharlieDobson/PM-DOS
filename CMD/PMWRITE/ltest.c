/*
 * LTEST.C - a test of LAY.C on Windows: a file laid out and printed.
 * Built by MKLTEST.CMD; nothing here ships.
 *
 *   LTEST FILE [COLUMNS]         as the text screen would show it,
 *                                broken to COLUMNS (0 or none: the
 *                                page's own width), with a rule where
 *                                each page starts
 *   LTEST FILE type              measured as type: each line's place
 *                                and size in twips, then its pieces
 *
 * It also checks, for every line, that a click on each character's
 * left edge finds that character and that the caret for that place is
 * back at the same x: "hit test" lines are printed for any that fail.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmt.h"
#include "wedit.h"
#include "lay.h"

int mem_reclaim( void )
{
    return 0;
}

void out_of_memory( void )
{
    fprintf( stderr, "LTEST: out of memory\n" );
    exit( 8 );
}

static char row[512];
static int  as_type;

static void put_unit( int col, uint32_t code )
{
    char near_buf[8];

    if ( col < 0 || col >= (int)sizeof( row ) - 1 ) {
        return;
    }
    cp_put( CP_1252, code, near_buf );
    row[col] = near_buf[0];
}

static void seg_print( void *ctx, const LSEG *seg )
{
    unsigned int index;
    int col = (int)(seg->x / GRID_W);

    (void)ctx;
    if ( as_type ) {
        printf( "      %s x=%ld w=%ld size=%ld", seg->kind == SEG_TEXT ? "text" : seg->kind == SEG_TAB ? "tab " :
                seg->kind == SEG_OBJECT ? "obj " : "made", (long)seg->x, (long)seg->width, (long)seg->size );
        if ( seg->kind == SEG_TEXT || seg->kind == SEG_MADE ) {
            printf( " \"" );
            for ( index = 0; index < seg->count; index++ ) {
                putchar( seg->text[index] < 0x7F && seg->text[index] >= 0x20 ? (int)seg->text[index] : '?' );
            }
            printf( "\"" );
        }
        printf( "\n" );
        return;
    }
    switch ( seg->kind ) {
    case SEG_TEXT:
    case SEG_MADE:
        for ( index = 0; index < seg->count; index++ ) {
            put_unit( col + (int)index, seg->upper && seg->text[index] >= 'a' && seg->text[index] <= 'z' ?
                      (uint32_t)seg->text[index] - 0x20 : seg->text[index] );
        }
        break;
    case SEG_TAB:
        for ( index = 0; index < (unsigned int)(seg->width / GRID_W); index++ ) {
            put_unit( col + (int)index, seg->lead == LEAD_DOTS ? '.' : seg->lead ? '_' : ' ' );
        }
        break;
    case SEG_OBJECT:
        put_unit( col, '#' );
        break;
    }
}

/* every place on a line: the caret's x, clicked, must give the place */
static void check_line( LAY *lay, unsigned int pi, unsigned int li, const LINFO *info )
{
    WPOS pos, back;
    int x, y, height;
    unsigned int off;

    for ( off = info->start; off < info->start + info->len; off++ ) {
        pos.para = pi;
        pos.off = off;
        if ( lay_line_of( lay, pos ) != li ) {
            printf( "hit test: para %lu off %lu is not on line %lu\n", (unsigned long)pi,
                    (unsigned long)off, (unsigned long)li );
            continue;
        }
        lay_caret( lay, pos, &x, &y, &height );
        back = lay_hit_line( lay, pi, li, x );
        lay_caret( lay, back, &y, &height, &height );
        /* places that share an x (nothing between them shows) are one place to a click */
        if ( y != x ) {
            printf( "hit test: para %lu off %lu at x %ld came back as off %lu\n", (unsigned long)pi,
                    (unsigned long)off, (long)x, (unsigned long)back.off );
        }
    }
}

int main( int argc, char **argv )
{
    FILE *file;
    WDOC *doc;
    LAY *lay;
    LINFO info;
    uint8_t *data;
    long size;
    unsigned int pi, li, page = 0, lines;
    int id, cols = 0, last;

    if ( argc < 2 ) {
        fprintf( stderr, "LTEST FILE [COLUMNS | type]\n" );
        return 2;
    }
    if ( argc > 2 && strcmp( argv[2], "type" ) == 0 ) {
        as_type = 1;
    } else if ( argc > 2 ) {
        cols = atoi( argv[2] );
    }
    file = fopen( argv[1], "rb" );
    if ( file == NULL ) {
        fprintf( stderr, "LTEST: cannot open %s\n", argv[1] );
        return 2;
    }
    fseek( file, 0, SEEK_END );
    size = ftell( file );
    fseek( file, 0, SEEK_SET );
    data = (uint8_t *)malloc( (size_t)size + 1 );
    fread( data, 1, (size_t)size, file );
    fclose( file );
    id = fmt_detect( data, (uint32_t)size, argv[1] );
    doc = wd_new();
    if ( id == FMT_NONE || formats[id]->load( data, (uint32_t)size, doc ) != WE_OK || wd_failed( doc ) ) {
        fprintf( stderr, "LTEST: cannot read %s\n", argv[1] );
        return 2;
    }
    wd_finish( doc );
    lay = lay_new( doc, &doc->body, !as_type );
    lay_set_width( lay, cols * GRID_W );
    if ( !lay_update( lay ) ) {
        fprintf( stderr, "LTEST: no memory to lay it out\n" );
        return 2;
    }
    printf( "width %ld, height %ld, %lu page%s of %ld\n", (long)lay_width( lay ), (long)lay_height( lay ),
            (unsigned long)lay_pages( lay ), lay_pages( lay ) == 1 ? "" : "s", (long)lay_page_height( lay ) );
    for ( pi = 0; pi < doc->body.count; pi++ ) {
        lines = lay_lines( lay, pi );
        for ( li = 0; li < lines; li++ ) {
            lay_line( lay, pi, li, &info );
            while ( page + 1 < lay_pages( lay ) && lay_page( lay, page + 1 )->para == pi &&
                    lay_page( lay, page + 1 )->line == li ) {
                page++;
                printf( "%s page %lu\n", as_type ? "====" : "- - - - - - - - - - - - - - - -", (unsigned long)page + 1 );
            }
            if ( as_type ) {
                printf( "%4lu.%-2lu y=%-6ld x=%-5ld w=%-5ld h=%-4ld base=%-4ld%s\n", (unsigned long)pi,
                        (unsigned long)li, (long)info.y, (long)info.x, (long)info.width, (long)info.height,
                        (long)info.base, info.flags & LL_PAGE ? " PAGE" : "" );
            } else {
                memset( row, ' ', sizeof( row ) );
            }
            lay_segs( lay, pi, li, seg_print, NULL );
            if ( !as_type ) {
                for ( last = (int)sizeof( row ) - 1; last > 0 && row[last - 1] == ' '; last-- ) {
                }
                row[last] = 0;
                /* a line of double spacing is two rows */
                printf( "%s\n", row );
                for ( last = GRID_H; last < info.height; last += GRID_H ) {
                    printf( "\n" );
                }
            }
            check_line( lay, pi, li, &info );
        }
    }
    lay_free( lay );
    wd_free( doc );
    printf( "heap in use at the end: %lu\n", (unsigned long)heap_in_use() );
    return 0;
}
