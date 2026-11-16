/*
 * PTEST.C - a test of PDFW.C on Windows: a file of any format read and
 * written as a PDF.  Built by MKPTEST.CMD; nothing here ships.
 *
 *   PTEST IN OUT.PDF [VARIANT]      VARIANT is 0 (PDF 1.0) to 8 (2.0);
 *                                   4, PDF 1.4, when it is not given
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmt.h"
#include "tf.h"

extern int pdfw_save( WDOC *doc, OUT *out, int variant );

int mem_reclaim( void )
{
    return 0;
}

void out_of_memory( void )
{
    fprintf( stderr, "PTEST: out of memory\n" );
    exit( 8 );
}

int main( int argc, char **argv )
{
    FILE *file;
    WDOC *doc;
    OUT *out;
    u8 *data;
    long size;
    int id, rc;

    if ( argc < 3 ) {
        fprintf( stderr, "PTEST IN OUT.PDF [VARIANT]\n" );
        return 2;
    }
    /* SET PWFONTS=dir;dir: TrueType files for the type faces, and for
       the PDF to hold */
    if ( getenv( "PWFONTS" ) != NULL ) {
        tf_font_dirs( getenv( "PWFONTS" ), 0 );
    }
    file = fopen( argv[1], "rb" );
    if ( file == NULL ) {
        fprintf( stderr, "PTEST: cannot open %s\n", argv[1] );
        return 2;
    }
    fseek( file, 0, SEEK_END );
    size = ftell( file );
    fseek( file, 0, SEEK_SET );
    data = (u8 *)malloc( (size_t)size + 1 );
    fread( data, 1, (size_t)size, file );
    fclose( file );
    id = fmt_detect( data, (u32)size, argv[1] );
    doc = wd_new();
    if ( id == FMT_NONE || formats[id]->load( data, (u32)size, doc ) != WE_OK || wd_failed( doc ) ) {
        fprintf( stderr, "PTEST: cannot read %s\n", argv[1] );
        return 2;
    }
    wd_finish( doc );
    out = out_file( argv[2] );
    if ( out == NULL ) {
        fprintf( stderr, "PTEST: cannot make %s\n", argv[2] );
        return 2;
    }
    rc = pdfw_save( doc, out, argc > 3 ? atoi( argv[3] ) : 4 );
    if ( out_close( out ) != 0 && rc == WE_OK ) {
        rc = WE_IO;
    }
    wd_free( doc );
    tf_done();                          /* the faces it measured with: what is left after is a leak */
    printf( "%s: %d, heap in use at the end: %lu\n", argv[2], rc, (unsigned long)heap_in_use() );
    return rc != WE_OK;
}
