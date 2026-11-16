/*
 * ZIPHOST.C - ZIPHOST.EXE: the checks of CRC.C, FLATE.C, ZIP.C and
 * XML.C, and the timing of the formats built on them, as a Windows
 * console program.
 *
 *   ZIPHOST crc                 the two checksums of known text
 *   ZIPHOST flate               deflate and inflate there and back:
 *                               nothing, one byte, text, noise, 5 MB
 *   ZIPHOST list FILE.ZIP       every member read, with its CRC checked
 *   ZIPHOST get FILE.ZIP NAME OUT   one member written to a file
 *   ZIPHOST make OUT.ZIP FILE...    an archive of files (the first is
 *                               stored when it is called "mimetype";
 *                               every other one goes in in pieces)
 *   ZIPHOST xml FILE            a .zip's XML members, or one XML file,
 *                               read to the end: pieces counted
 *   ZIPHOST zlib FILE OUT       a file as a zlib stream
 *   ZIPHOST unzlib FILE OUT     ...and back
 *   ZIPHOST load FILE           a document read as PMWRITE reads it:
 *                               the time, and the most heap it took
 *   ZIPHOST save FILE OUT FMT   ...and written: the time, and the heap
 *   ZIPHOST text FILE OUT       a document's text and counts, in the
 *                               form the Word comparison reads
 *
 * Not built by BUILD.CMD; MKZIPH.CMD builds it.  This file may use the
 * C library; nothing it tests does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fmt.h"
#include "sys.h"
#include "crc.h"
#include "flate.h"
#include "zip.h"
#include "xml.h"

extern u32 pw_peak_heap;        /* F_DOCX.C and F_ODT.C, built with PW_PEAK */

int mem_reclaim( void )
{
    return 0;
}

void out_of_memory( void )
{
    fprintf( stderr, "ZIPHOST: out of memory\n" );
    exit( 8 );
}

static int failures;

static void check( int ok, const char *what )
{
    printf( "%-6s %s\n", ok ? "ok" : "FAILED", what );
    if ( !ok ) {
        failures++;
    }
}

static u8 *read_file( const char *path, u32 *len )
{
    FILE *file = fopen( path, "rb" );
    u8 *data;
    long size;

    if ( file == NULL ) {
        fprintf( stderr, "ZIPHOST: cannot open %s\n", path );
        return NULL;
    }
    fseek( file, 0, SEEK_END );
    size = ftell( file );
    fseek( file, 0, SEEK_SET );
    data = (u8 *)malloc( (size_t)size + 1 );
    if ( data == NULL || fread( data, 1, (size_t)size, file ) != (size_t)size ) {
        fprintf( stderr, "ZIPHOST: cannot read %s\n", path );
        fclose( file );
        free( data );
        return NULL;
    }
    fclose( file );
    *len = (u32)size;
    return data;
}

static int write_file( const char *path, const u8 *data, u32 len )
{
    FILE *file = fopen( path, "wb" );

    if ( file == NULL ) {
        fprintf( stderr, "ZIPHOST: cannot make %s\n", path );
        return 0;
    }
    fwrite( data, 1, len, file );
    fclose( file );
    return 1;
}

static int cmd_crc( void )
{
    static const char nine[] = "123456789";
    static const char fox[] = "The quick brown fox jumps over the lazy dog";
    u32 sum;

    check( crc32_update( 0, (const u8 *)"", 0 ) == 0, "CRC-32 of nothing is 0" );
    check( crc32_update( 0, (const u8 *)nine, 9 ) == 0xCBF43926UL, "CRC-32 of 123456789 is CBF43926" );
    check( crc32_update( 0, (const u8 *)fox, 43 ) == 0x414FA339UL, "CRC-32 of the fox is 414FA339" );
    sum = crc32_update( 0, (const u8 *)fox, 10 );
    sum = crc32_update( sum, (const u8 *)fox + 10, 33 );
    check( sum == 0x414FA339UL, "CRC-32 in two pieces is the same" );
    check( adler32_update( 1, (const u8 *)"", 0 ) == 1, "Adler-32 of nothing is 1" );
    check( adler32_update( 1, (const u8 *)"Wikipedia", 9 ) == 0x11E60398UL, "Adler-32 of Wikipedia is 11E60398" );
    check( adler32_update( 1, (const u8 *)nine, 9 ) == 0x091E01DEUL, "Adler-32 of 123456789 is 091E01DE" );
    sum = adler32_update( 1, (const u8 *)fox, 20 );
    sum = adler32_update( sum, (const u8 *)fox + 20, 23 );
    check( sum == 0x5BDC0FDAUL, "Adler-32 of the fox, in two pieces, is 5BDC0FDA" );
    return failures != 0;
}

/* there and back, every way the calls allow */
static void round_trip( const u8 *data, u32 len, const char *what )
{
    char line[160];
    u8 *packed, *back, *fixed;
    u32 packed_len, back_len, piece_len, first;
    clock_t start;
    long pack_ms, unpack_ms;
    int ok;

    start = clock();
    packed = deflate_alloc( data, len, &packed_len, 0 );
    pack_ms = (long)(clock() - start) * 1000 / CLOCKS_PER_SEC;
    ok = packed != NULL;
    start = clock();
    back = ok ? inflate_alloc( packed, packed_len, &back_len, len + 16 ) : NULL;
    unpack_ms = (long)(clock() - start) * 1000 / CLOCKS_PER_SEC;
    ok = ok && back != NULL && back_len == len && memcmp( back, data, len ) == 0;
    sprintf( line, "%s: %lu -> %lu bytes (%ld ms, back in %ld ms)", what, (unsigned long)len,
             (unsigned long)packed_len, pack_ms, unpack_ms );
    check( ok, line );
    xfree( back );

    /* into a block of exactly the right size, and one a byte short */
    fixed = (u8 *)malloc( len + 1 );
    ok = packed != NULL && inflate_mem( packed, packed_len, fixed, len, &back_len ) &&
         back_len == len && memcmp( fixed, data, len ) == 0;
    if ( len > 0 ) {
        ok = ok && !inflate_mem( packed, packed_len, fixed, len - 1, &back_len );
        ok = ok && inflate_alloc( packed, packed_len, &back_len, len - 1 ) == NULL;
    }
    sprintf( line, "%s: fits its size and not a byte less", what );
    check( ok, line );
    xfree( packed );

    /* the zlib wrapping */
    packed = deflate_alloc( data, len, &packed_len, 1 );
    back = packed != NULL ? zlib_inflate_alloc( packed, packed_len, &back_len, len + 16 ) : NULL;
    ok = back != NULL && back_len == len && memcmp( back, data, len ) == 0 &&
         zlib_inflate_mem( packed, packed_len, fixed, len + 1, &back_len ) && back_len == len;
    if ( ok && packed_len > 6 ) {
        packed[packed_len - 1] ^= 1;    /* the sum is checked */
        ok = !zlib_inflate_mem( packed, packed_len, fixed, len + 1, &back_len );
    }
    sprintf( line, "%s: as a zlib stream, sum checked", what );
    check( ok, line );
    xfree( back );
    xfree( packed );

    /* in two pieces */
    first = len / 3;
    packed = deflate_piece( data, first, &packed_len, 0 );
    back = deflate_piece( data + first, len - first, &piece_len, 1 );
    ok = packed != NULL && back != NULL;
    if ( ok ) {
        u8 *both = (u8 *)malloc( packed_len + piece_len + 1 );

        memcpy( both, packed, packed_len );
        memcpy( both + packed_len, back, piece_len );
        ok = inflate_mem( both, packed_len + piece_len, fixed, len, &back_len ) && back_len == len &&
             memcmp( fixed, data, len ) == 0;
        free( both );
    }
    sprintf( line, "%s: in two pieces", what );
    check( ok, line );
    xfree( back );
    xfree( packed );
    free( fixed );
}

static int cmd_flate( void )
{
    static const char words[] = "the quick brown fox jumps over the lazy dog and runs away ";
    u8 *big;
    u32 size = 5UL * 1024 * 1024, index, seed = 12345;

    round_trip( (const u8 *)"", 0, "nothing" );
    round_trip( (const u8 *)"A", 1, "one byte" );
    round_trip( (const u8 *)words, sizeof( words ) - 1, "a sentence" );

    big = (u8 *)malloc( size );
    for ( index = 0; index < 200000; index++ ) {
        seed = seed * 1103515245UL + 12345;
        big[index] = (u8)words[(index + (seed >> 28)) % (sizeof( words ) - 1)];
    }
    round_trip( big, 200000, "200K of text" );
    for ( index = 0; index < 300000; index++ ) {
        seed = seed * 1103515245UL + 12345;
        big[index] = (u8)(seed >> 16);
    }
    round_trip( big, 300000, "300K of noise" );
    memset( big, 0, 100000 );
    round_trip( big, 100000, "100K of zeroes" );
    /* 5 MB: text, noise, long runs and repeats from far back, mixed */
    for ( index = 0; index < size; index++ ) {
        seed = seed * 1103515245UL + 12345;
        if ( (index >> 16) % 4 == 0 ) {
            big[index] = (u8)(seed >> 16);
        } else if ( (index >> 16) % 4 == 1 ) {
            big[index] = (u8)words[(index + (seed >> 29)) % (sizeof( words ) - 1)];
        } else if ( (index >> 16) % 4 == 2 ) {
            big[index] = (u8)(index >> 12);
        } else {
            big[index] = big[index - 40000];
        }
    }
    round_trip( big, size, "5 MB mixed" );
    free( big );
    return failures != 0;
}

static int cmd_list( const char *path )
{
    ZIP *zip;
    u8 *data, *member;
    u32 len, index, got, total = 0;
    int why, bad = 0;

    data = read_file( path, &len );
    if ( data == NULL ) {
        return 2;
    }
    zip = zip_open_why( data, len, &why );
    if ( zip == NULL ) {
        printf( "%s: not opened (%d)\n", path, why );
        free( data );
        return 1;
    }
    for ( index = 0; index < zip_count( zip ); index++ ) {
        member = zip_read( zip, index, &got );
        if ( member == NULL ) {
            printf( "  %-40s NOT READ (%d)\n", zip_name( zip, index ), zip_error( zip ) );
            bad++;
        } else {
            printf( "  %-40s %8lu  %08lX\n", zip_name( zip, index ), (unsigned long)got,
                    (unsigned long)crc32_update( 0, member, got ) );
            total += got;
            xfree( member );
        }
    }
    printf( "%s: %lu members, %lu bytes, %d not read\n", path, (unsigned long)zip_count( zip ),
            (unsigned long)total, bad );
    zip_close( zip );
    free( data );
    return bad != 0;
}

static int cmd_get( const char *path, const char *name, const char *out_path )
{
    ZIP *zip;
    u8 *data, *member;
    u32 len, got;
    int index, rc = 1;

    data = read_file( path, &len );
    if ( data == NULL ) {
        return 2;
    }
    zip = zip_open( data, len );
    index = zip_find( zip, name );
    if ( index < 0 ) {
        index = zip_ifind( zip, name );
    }
    if ( index >= 0 ) {
        member = zip_read( zip, (u32)index, &got );
        if ( member != NULL ) {
            rc = write_file( out_path, member, got ) ? 0 : 2;
            xfree( member );
        }
    }
    if ( rc ) {
        fprintf( stderr, "ZIPHOST: %s: no %s\n", path, name );
    }
    zip_close( zip );
    free( data );
    return rc;
}

static int cmd_make( int count, char **paths )
{
    OUT *out = out_file( paths[0] );
    ZIPW *zipw;
    u8 *data;
    const char *name;
    u32 len, done, part;
    int index, ok = 1;

    if ( out == NULL ) {
        fprintf( stderr, "ZIPHOST: cannot make %s\n", paths[0] );
        return 2;
    }
    zipw = zipw_new( out );
    for ( index = 1; index < count && ok; index++ ) {
        data = read_file( paths[index], &len );
        if ( data == NULL ) {
            ok = 0;
            break;
        }
        name = strrchr( paths[index], '\\' );
        name = name != NULL ? name + 1 : paths[index];
        if ( index == 1 && strcmp( name, "mimetype" ) == 0 ) {
            ok = zipw_add( zipw, name, data, len, 1 );
        } else if ( index % 2 ) {
            ok = zipw_add( zipw, name, data, len, 0 );
        } else {
            ok = zipw_begin( zipw, name );
            for ( done = 0; ok && done < len; done += part ) {
                part = len - done > 50000 ? 50000 : len - done;
                ok = zipw_write( zipw, data + done, part );
            }
            ok = ok && zipw_finish( zipw );
        }
        free( data );
    }
    ok = zipw_end( zipw ) && ok;
    if ( out_close( out ) != 0 ) {
        ok = 0;
    }
    printf( "%s: %s\n", paths[0], ok ? "written" : "FAILED" );
    return ok ? 0 : 1;
}

/* one document read to its end: 1 if it ended well */
static int xml_walk( const char *name, const u8 *data, u32 len )
{
    XML *xml = xml_open( data, len );
    u32 starts = 0, ends = 0, texts = 0, attrs = 0, known = 0, text_bytes = 0;
    int token, deepest = 0;

    xml_ns( xml, "http://schemas.openxmlformats.org/wordprocessingml/2006/main", 1 );
    xml_ns( xml, "http://purl.oclc.org/ooxml/wordprocessingml/main", 1 );
    xml_ns( xml, "urn:oasis:names:tc:opendocument:xmlns:text:1.0", 2 );
    xml_ns( xml, "urn:oasis:names:tc:opendocument:xmlns:office:1.0", 3 );
    xml_ns( xml, "urn:oasis:names:tc:opendocument:xmlns:style:1.0", 4 );
    for ( ;; ) {
        token = xml_next( xml );
        if ( token == XML_EOF || token == XML_ERROR ) {
            break;
        }
        if ( token == XML_START ) {
            starts++;
            attrs += xml->nattr;
            if ( xml->ns != 0 ) {
                known++;
            }
            if ( xml->depth > deepest ) {
                deepest = xml->depth;
            }
        } else if ( token == XML_END ) {
            ends++;
        } else {
            texts++;
            text_bytes += xml->text_len;
        }
    }
    printf( "  %-36s %s %lu elements (%lu known), %lu attributes, %lu texts (%lu bytes), depth %d\n",
            name, token == XML_EOF && starts == ends ? "ok    " : "FAILED",
            (unsigned long)starts, (unsigned long)known, (unsigned long)attrs, (unsigned long)texts,
            (unsigned long)text_bytes, deepest );
    xml_close( xml );
    return token == XML_EOF && starts == ends;
}

static int cmd_xml( const char *path )
{
    ZIP *zip;
    u8 *data, *member;
    const char *name, *dot;
    u32 len, index, got;
    int bad = 0;

    data = read_file( path, &len );
    if ( data == NULL ) {
        return 2;
    }
    zip = zip_open( data, len );
    if ( zip == NULL ) {
        bad = !xml_walk( path, data, len );
        free( data );
        return bad;
    }
    for ( index = 0; index < zip_count( zip ); index++ ) {
        name = zip_name( zip, index );
        dot = strrchr( name, '.' );
        if ( dot == NULL || (strcmp( dot, ".xml" ) != 0 && strcmp( dot, ".rels" ) != 0) ) {
            continue;
        }
        member = zip_read( zip, index, &got );
        if ( member == NULL ) {
            if ( zip_size( zip, index ) != 0 ) {
                printf( "  %-36s NOT READ (%d)\n", name, zip_error( zip ) );
            }
            continue;
        }
        if ( got > 0 && !xml_walk( name, member, got ) ) {
            bad++;
        }
        xfree( member );
    }
    zip_close( zip );
    free( data );
    return bad != 0;
}

static int cmd_zlib( const char *path, const char *out_path, int back )
{
    u8 *data, *made;
    u32 len, made_len;

    data = read_file( path, &len );
    if ( data == NULL ) {
        return 2;
    }
    made = back ? zlib_inflate_alloc( data, len, &made_len, 0x10000000UL )
                : deflate_alloc( data, len, &made_len, 1 );
    free( data );
    if ( made == NULL ) {
        fprintf( stderr, "ZIPHOST: %s: could not be %s\n", path, back ? "unpacked" : "packed" );
        return 1;
    }
    write_file( out_path, made, made_len );
    xfree( made );
    return 0;
}

#ifdef PW_FORMATS
static int cmd_load( const char *path, const char *out_path, const char *tag )
{
    WDOC *doc;
    OUT *out;
    u8 *data;
    u32 len, before = heap_in_use(), chars = 0, index;
    clock_t start;
    long load_ms, save_ms;
    int id, rc;

    data = read_file( path, &len );
    if ( data == NULL ) {
        return 2;
    }
    id = fmt_detect( data, len, path );
    doc = wd_new();
    pw_peak_heap = 0;
    start = clock();
    rc = id == FMT_NONE ? WE_FORMAT : formats[id]->load( data, len, doc );
    load_ms = (long)(clock() - start) * 1000 / CLOCKS_PER_SEC;
    free( data );
    if ( rc != WE_OK ) {
        printf( "%s: not read (%d)\n", path, rc );
        return 1;
    }
    wd_finish( doc );
    for ( index = 0; index < doc->body.count; index++ ) {
        chars += doc->body.para[index]->len;
    }
    printf( "%s: %lu bytes as %s\n", path, (unsigned long)len, fmt_tag( id ) );
    printf( "  read in %ld ms: %lu paragraphs, %lu characters\n", load_ms,
            (unsigned long)doc->body.count, (unsigned long)chars );
    printf( "  heap: %lu bytes at the most while reading, %lu for the document when done\n",
            (unsigned long)(pw_peak_heap - before), (unsigned long)(heap_in_use() - before) );
    if ( out_path != NULL ) {
        id = fmt_by_tag( tag );
        out = out_file( out_path );
        if ( id < 0 || out == NULL ) {
            return 2;
        }
        pw_peak_heap = 0;
        start = clock();
        rc = formats[id]->save( doc, out, formats[id]->def_variant );
        save_ms = (long)(clock() - start) * 1000 / CLOCKS_PER_SEC;
        printf( "  written as %s in %ld ms (%lu bytes): %lu bytes of heap at the most, document and all\n",
                tag, save_ms, (unsigned long)out_size( out ), (unsigned long)(pw_peak_heap - before) );
        if ( out_close( out ) != 0 || rc != WE_OK ) {
            printf( "  WRITE FAILED (%d)\n", rc );
            return 1;
        }
    }
    wd_free( doc );
    return 0;
}
static void put_text( FILE *file, const WDOC *doc, const WPARA *para )
{
    u8 utf[4];
    u32 pos = 0, code;

    (void)doc;
    while ( pos < para->len ) {
        code = utf16_get( para->text, para->len, &pos );
        if ( code == WCH_OBJ || WCH_IS_FIELD( code ) ) {
            continue;
        }
        fwrite( utf, 1, (size_t)utf8_put( utf, code < 0x20 ? ' ' : code ), file );
    }
}

static void put_story( FILE *file, const WDOC *doc, const STORY *story, const char *name )
{
    u32 index;

    fprintf( file, "%s ", name );
    for ( index = 0; index < story->count; index++ ) {
        put_text( file, doc, story->para[index] );
        fputc( '|', file );
    }
    fputc( '\n', file );
}

/* what Word is asked for too (WORDINFO.PS1), so the two can be compared */
static int cmd_text( const char *path, const char *out_path )
{
    WDOC *doc;
    FILE *file;
    u8 *data;
    u32 len, index, rows = 0, foot = 0, end = 0, images = 0;
    int id, rc;

    data = read_file( path, &len );
    if ( data == NULL ) {
        return 2;
    }
    id = fmt_detect( data, len, path );
    doc = wd_new();
    rc = id == FMT_NONE ? WE_FORMAT : formats[id]->load( data, len, doc );
    free( data );
    file = fopen( out_path, "wb" );
    if ( file == NULL ) {
        return 2;
    }
    if ( rc != WE_OK ) {
        fprintf( file, "NOT READ %d\n", rc );
        fclose( file );
        return 1;
    }
    wd_finish( doc );
    for ( index = 0; index < doc->body.count; index++ ) {
        if ( doc->body.para[index]->pap.flags & PF_ROWEND ) {
            rows++;
        }
    }
    for ( index = 0; index < doc->nobj; index++ ) {
        if ( doc->obj[index].kind == OBJ_IMAGE ) {
            images++;
        } else if ( doc->obj[index].fmt == NOTE_END ) {
            end++;
        } else {
            foot++;
        }
    }
    fprintf( file, "PARAS %lu\nROWS %lu\nFOOT %lu\nEND %lu\nIMAGES %lu\nLOST %04lX\n", (unsigned long)doc->body.count,
             (unsigned long)rows, (unsigned long)foot, (unsigned long)end, (unsigned long)images,
             (unsigned long)doc->lost );
    fprintf( file, "PAGE %ld %ld %ld %ld %ld %ld %ld %ld %u %u\n", (long)doc->page.width, (long)doc->page.height,
             (long)doc->page.left, (long)doc->page.right, (long)doc->page.top, (long)doc->page.bottom,
             (long)doc->page.header, (long)doc->page.footer, (unsigned)doc->page.first_page,
             (unsigned)doc->page.landscape );
    put_story( file, doc, &doc->header, "HEADER" );
    put_story( file, doc, &doc->footer, "FOOTER" );
    fprintf( file, "TEXT\n" );
    for ( index = 0; index < doc->body.count; index++ ) {
        put_text( file, doc, doc->body.para[index] );
        fputc( '\n', file );
    }
    fclose( file );
    wd_free( doc );
    return 0;
}
#endif

int main( int argc, char **argv )
{
    if ( argc >= 2 && strcmp( argv[1], "crc" ) == 0 ) {
        return cmd_crc();
    }
    if ( argc >= 2 && strcmp( argv[1], "flate" ) == 0 ) {
        return cmd_flate();
    }
    if ( argc >= 3 && strcmp( argv[1], "list" ) == 0 ) {
        return cmd_list( argv[2] );
    }
    if ( argc >= 5 && strcmp( argv[1], "get" ) == 0 ) {
        return cmd_get( argv[2], argv[3], argv[4] );
    }
    if ( argc >= 4 && strcmp( argv[1], "make" ) == 0 ) {
        return cmd_make( argc - 2, argv + 2 );
    }
    if ( argc >= 3 && strcmp( argv[1], "xml" ) == 0 ) {
        return cmd_xml( argv[2] );
    }
    if ( argc >= 4 && strcmp( argv[1], "zlib" ) == 0 ) {
        return cmd_zlib( argv[2], argv[3], 0 );
    }
    if ( argc >= 4 && strcmp( argv[1], "unzlib" ) == 0 ) {
        return cmd_zlib( argv[2], argv[3], 1 );
    }
#ifdef PW_FORMATS
    if ( argc >= 3 && strcmp( argv[1], "load" ) == 0 ) {
        return cmd_load( argv[2], NULL, NULL );
    }
    if ( argc >= 5 && strcmp( argv[1], "save" ) == 0 ) {
        return cmd_load( argv[2], argv[3], argv[4] );
    }
    if ( argc >= 4 && strcmp( argv[1], "text" ) == 0 ) {
        return cmd_text( argv[2], argv[3] );
    }
#endif
    fprintf( stderr, "ZIPHOST crc | flate | list ZIP | get ZIP NAME OUT | make ZIP FILE... | xml FILE\n"
                     "        | zlib FILE OUT | unzlib FILE OUT | load FILE | save FILE OUT FORMAT\n" );
    return 2;
}
