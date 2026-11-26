/*
 * HOST.C - PWHOST.EXE: PMWRITE's documents and formats as a Windows
 * console program, for testing a format without booting PM-DOS.
 *
 *   PWHOST formats                       the formats and what each writes
 *   PWHOST detect FILE                   which format a file is
 *   PWHOST dump FILE [OUT]               the document, line by line
 *   PWHOST conv IN OUT FORMAT [VARIANT]  read one file, write another
 *   PWHOST rt IN FORMAT [VARIANT]        read IN, write it as FORMAT in
 *                                        memory, read that back, and
 *                                        say where the two documents
 *                                        differ; exit code 1 if they do
 *
 * FORMAT is a tag "formats" lists (DOCX, WORDSTAR ...); VARIANT is the
 * start of, or a piece of, one of the kinds it lists ("5.1").  "IN" may
 * be of any format: it is detected.
 *
 * Not built by BUILD.CMD; MKHOST.CMD builds it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmt.h"
#include "sys.h"

int mem_reclaim( void )
{
    return 0;
}

void out_of_memory( void )
{
    fprintf( stderr, "PWHOST: out of memory\n" );
    exit( 8 );
}

static const char *why( int rc )
{
    switch ( rc ) {
    case WE_OK:          return "ok";
    case WE_NOMEM:       return "no memory";
    case WE_FORMAT:      return "not that format, or damaged";
    case WE_UNSUPPORTED: return "a version or feature that is not read";
    case WE_PASSWORD:    return "protected with a password";
    case WE_IO:          return "disk error";
    }
    return "unknown error";
}

static uint8_t *read_file( const char *path, uint32_t *len )
{
    FILE *file = fopen( path, "rb" );
    uint8_t *data;
    long size;

    if ( file == NULL ) {
        fprintf( stderr, "PWHOST: cannot open %s\n", path );
        return NULL;
    }
    fseek( file, 0, SEEK_END );
    size = ftell( file );
    fseek( file, 0, SEEK_SET );
    data = (uint8_t *)malloc( (size_t)size + 1 );
    if ( data == NULL || fread( data, 1, (size_t)size, file ) != (size_t)size ) {
        fprintf( stderr, "PWHOST: cannot read %s\n", path );
        fclose( file );
        free( data );
        return NULL;
    }
    fclose( file );
    *len = (uint32_t)size;
    return data;
}

/* a file's bytes as a document: NULL, with the reason said */
static WDOC *load_bytes( const uint8_t *data, uint32_t len, const char *name, int quiet )
{
    WDOC *doc;
    int id, rc;

    id = fmt_detect( data, len, name );
    if ( id == FMT_NONE ) {
        fprintf( stderr, "PWHOST: %s: no format knows it\n", name );
        return NULL;
    }
    doc = wd_new();
    if ( doc == NULL ) {
        out_of_memory();
    }
    rc = formats[id]->load( data, len, doc );
    if ( rc == WE_OK && wd_failed( doc ) ) {
        rc = WE_NOMEM;
    }
    if ( rc != WE_OK ) {
        fprintf( stderr, "PWHOST: %s: read as %s: %s\n", name, fmt_tag( id ), why( rc ) );
        wd_free( doc );
        return NULL;
    }
    doc->format = id;
    wd_finish( doc );
    if ( !quiet ) {
        fprintf( stderr, "%s: %s\n", name, formats[id]->name );
    }
    return doc;
}

static WDOC *load_file( const char *path, int quiet )
{
    WDOC *doc;
    uint8_t *data;
    uint32_t len;

    data = read_file( path, &len );
    if ( data == NULL ) {
        return NULL;
    }
    doc = load_bytes( data, len, path, quiet );
    free( data );
    return doc;
}

static int pick_format( const char *tag, const char *variant_text, int *variant )
{
    int id = fmt_by_tag( tag );

    if ( id < 0 ) {
        fprintf( stderr, "PWHOST: no format is called %s\n", tag );
        return -1;
    }
    if ( formats[id]->save == NULL ) {
        fprintf( stderr, "PWHOST: %s is not written\n", tag );
        return -1;
    }
    *variant = formats[id]->def_variant;
    if ( variant_text != NULL ) {
        *variant = fmt_variant( formats[id], variant_text );
        if ( *variant < 0 ) {
            fprintf( stderr, "PWHOST: %s has no kind called %s\n", tag, variant_text );
            return -1;
        }
    }
    return id;
}

static int cmd_formats( void )
{
    int id, index;

    for ( id = FMT_NONE + 1; id < FMT_COUNT; id++ ) {
        printf( "%-12s %-28s %-16s %s\n", fmt_tag( id ), formats[id]->name, formats[id]->exts,
                formats[id]->save != NULL ? "read, write" : "read" );
        if ( formats[id]->variants != NULL ) {
            for ( index = 0; formats[id]->variants[index] != NULL; index++ ) {
                printf( "             %c %s\n", index == formats[id]->def_variant ? '*' : ' ',
                        formats[id]->variants[index] );
            }
        }
    }
    return 0;
}

static int cmd_detect( const char *path )
{
    uint8_t *data;
    uint32_t len;
    int id;

    data = read_file( path, &len );
    if ( data == NULL ) {
        return 2;
    }
    id = fmt_detect( data, len, path );
    printf( "%s\n", id == FMT_NONE ? "NONE" : fmt_tag( id ) );
    free( data );
    return 0;
}

static int cmd_dump( const char *path, const char *out_path )
{
    WDOC *doc = load_file( path, 1 );
    OUT *out;
    uint8_t *text;
    uint32_t len;

    if ( doc == NULL ) {
        return 2;
    }
    out = out_mem();
    wd_dump( doc, out );
    text = out_data( out, &len );
    if ( out_path != NULL ) {
        FILE *file = fopen( out_path, "wb" );

        if ( file == NULL ) {
            fprintf( stderr, "PWHOST: cannot make %s\n", out_path );
            return 2;
        }
        fwrite( text, 1, len, file );
        fclose( file );
    } else {
        fwrite( text, 1, len, stdout );
    }
    out_close( out );
    wd_free( doc );
    return 0;
}

static int cmd_conv( const char *in_path, const char *out_path, const char *tag, const char *variant_text )
{
    WDOC *doc;
    OUT *out;
    int id, variant, rc;

    id = pick_format( tag, variant_text, &variant );
    if ( id < 0 ) {
        return 2;
    }
    doc = load_file( in_path, 0 );
    if ( doc == NULL ) {
        return 2;
    }
    out = out_file( out_path );
    if ( out == NULL ) {
        fprintf( stderr, "PWHOST: cannot make %s\n", out_path );
        return 2;
    }
    rc = formats[id]->save( doc, out, variant );
    if ( out_close( out ) != 0 && rc == WE_OK ) {
        rc = WE_IO;
    }
    wd_free( doc );
    if ( rc != WE_OK ) {
        fprintf( stderr, "PWHOST: writing %s: %s\n", out_path, why( rc ) );
        return 2;
    }
    return 0;
}

/* where two dumps part: the line, and both versions of it */
static int compare( const uint8_t *one, uint32_t one_len, const uint8_t *two, uint32_t two_len )
{
    unsigned int pos = 0, line = 1, start = 0, end;

    while ( pos < one_len && pos < two_len && one[pos] == two[pos] ) {
        if ( one[pos] == '\n' ) {
            line++;
            start = pos + 1;
        }
        pos++;
    }
    if ( pos == one_len && pos == two_len ) {
        return 0;
    }
    printf( "DIFFERENT at line %lu\n", (unsigned long)line );
    for ( end = start; end < one_len && one[end] != '\r' && one[end] != '\n'; end++ ) {
    }
    printf( "  before: %.*s\n", (int)(end - start), one + start );
    for ( end = start; end < two_len && two[end] != '\r' && two[end] != '\n'; end++ ) {
    }
    printf( "  after:  %.*s\n", (int)(end - start), two + start );
    return 1;
}

static int cmd_rt( const char *in_path, const char *tag, const char *variant_text, const char *keep )
{
    WDOC *doc, *back;
    OUT *file_out, *dump1, *dump2;
    uint8_t *bytes, *text1, *text2;
    uint32_t len, len1, len2;
    char name[32];
    int id, variant, rc, differ;

    id = pick_format( tag, variant_text, &variant );
    if ( id < 0 ) {
        return 2;
    }
    doc = load_file( in_path, 1 );
    if ( doc == NULL ) {
        return 2;
    }
    file_out = out_mem();
    rc = formats[id]->save( doc, file_out, variant );
    if ( rc == WE_OK && out_error( file_out ) ) {
        rc = WE_IO;
    }
    if ( rc != WE_OK ) {
        fprintf( stderr, "PWHOST: writing as %s: %s\n", tag, why( rc ) );
        return 2;
    }
    bytes = out_data( file_out, &len );
    if ( keep != NULL ) {
        FILE *file = fopen( keep, "wb" );

        if ( file != NULL ) {
            fwrite( bytes, 1, len, file );
            fclose( file );
        }
    }
    /* a name with the format's own extension, for formats that go by it */
    sprintf( name, "MEMORY.%.7s", formats[id]->exts );
    if ( strchr( name, ';' ) != NULL ) {
        *strchr( name, ';' ) = 0;
    }
    back = load_bytes( bytes, len, name, 1 );
    if ( back == NULL ) {
        printf( "UNREADABLE: what was written as %s could not be read back\n", tag );
        return 1;
    }
    if ( back->format != id ) {
        printf( "MISTAKEN: what was written as %s was read back as %s\n", tag, fmt_tag( back->format ) );
        return 1;
    }
    dump1 = out_mem();
    dump2 = out_mem();
    wd_dump( doc, dump1 );
    wd_dump( back, dump2 );
    text1 = out_data( dump1, &len1 );
    text2 = out_data( dump2, &len2 );
    differ = compare( text1, len1, text2, len2 );
    if ( !differ ) {
        printf( "SAME: %lu bytes as %s\n", (unsigned long)len, tag );
    }
    out_close( dump1 );
    out_close( dump2 );
    out_close( file_out );
    wd_free( doc );
    wd_free( back );
    return differ;
}

int main( int argc, char **argv )
{
    if ( argc >= 2 && strcmp( argv[1], "formats" ) == 0 ) {
        return cmd_formats();
    }
    if ( argc >= 3 && strcmp( argv[1], "detect" ) == 0 ) {
        return cmd_detect( argv[2] );
    }
    if ( argc >= 3 && strcmp( argv[1], "dump" ) == 0 ) {
        return cmd_dump( argv[2], argc > 3 ? argv[3] : NULL );
    }
    if ( argc >= 5 && strcmp( argv[1], "conv" ) == 0 ) {
        return cmd_conv( argv[2], argv[3], argv[4], argc > 5 ? argv[5] : NULL );
    }
    if ( argc >= 4 && strcmp( argv[1], "rt" ) == 0 ) {
        return cmd_rt( argv[2], argv[3], argc > 4 ? argv[4] : NULL, getenv( "PWKEEP" ) );
    }
    fprintf( stderr, "PWHOST formats | detect FILE | dump FILE [OUT] | conv IN OUT FORMAT [VARIANT]\n"
                     "       | rt IN FORMAT [VARIANT]   (SET PWKEEP=file keeps what rt wrote)\n" );
    return 2;
}
