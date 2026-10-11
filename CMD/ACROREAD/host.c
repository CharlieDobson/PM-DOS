/*
 * HOST.C - ACROHOST.EXE: ACROREAD's PDF code as a Windows console
 * program, for testing it against files without booting PM-DOS.
 *
 *   ACROHOST selftest               the ciphers and hashes
 *   ACROHOST info file.pdf          version, pages, each page's size
 *   ACROHOST obj file.pdf n         object n, as it was read
 *   ACROHOST stream file.pdf n      object n's stream, decoded, to stdout
 *   ACROHOST content file.pdf p     page p's content stream, decoded
 *   ACROHOST text file.pdf [p]      the text of page p, or of every page
 *   ACROHOST render file.pdf p out.bmp [zoom%] [gray]
 *                                   page p as a picture
 *   ACROHOST all file.pdf [zoom%]   every page drawn and thrown away: does
 *                                   any of them fail, and how long do they take
 *
 * Pages count from 1.  A password, if the file needs one, is the
 * environment variable ACROPW.
 *
 * Not built by BUILD.CMD; MKHOST.CMD builds it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include "pdf.h"
#include "gfx.h"

void out_of_memory( void )
{
    if ( pdf_trap ) {
        pdf_fail( PE_NOMEM );
    }
    fprintf( stderr, "out of memory\n" );
    exit( 8 );
}

static const char *why_text( int why )
{
    switch ( why ) {
    case PE_NOMEM:    return "out of memory";
    case PE_DAMAGED:  return "the file is damaged";
    case PE_NOTPDF:   return "not a PDF file";
    case PE_PASSWORD: return "a password is needed";
    case PE_CRYPT:    return "encrypted in a way this cannot read";
    case PE_IO:       return "the file cannot be read";
    }
    return "?";
}

static void dump( OBJ *obj, int depth )
{
    u32 index;

    switch ( obj->type ) {
    case T_NULL:
        printf( "null" );
        break;
    case T_BOOL:
        printf( obj->u.ival ? "true" : "false" );
        break;
    case T_INT:
        printf( "%ld", obj->u.ival );
        break;
    case T_REAL:
        printf( "%.5f", (double)real_to_fx( obj->u.real ) / 65536.0 );
        break;
    case T_STR:
        printf( "(" );
        for ( index = 0; index < obj->u.str.len && index < 60; index++ ) {
            if ( obj->u.str.ptr[index] >= 32 && obj->u.str.ptr[index] < 127 ) {
                putchar( obj->u.str.ptr[index] );
            } else {
                printf( "\\%03o", obj->u.str.ptr[index] );
            }
        }
        printf( index < obj->u.str.len ? "...)" : ")" );
        break;
    case T_NAME:
        printf( "/%s", obj->u.str.ptr );
        break;
    case T_REF:
        printf( "%ld %u R", obj->u.ival, obj->gen );
        break;
    case T_ARR:
        printf( "[" );
        for ( index = 0; index < obj->u.arr->count; index++ ) {
            if ( index ) {
                printf( " " );
            }
            if ( index == 40 ) {
                printf( "... (%lu)", obj->u.arr->count );
                break;
            }
            dump( &obj->u.arr->items[index], depth + 1 );
        }
        printf( "]" );
        break;
    case T_DICT:
    case T_STREAM:
        printf( "<<" );
        for ( index = 0; index < obj->u.dict->count; index++ ) {
            printf( "\n%*s/%s ", depth * 2 + 2, "", obj->u.dict->ents[index].key );
            dump( &obj->u.dict->ents[index].val, depth + 1 );
        }
        printf( "\n%*s>>", depth * 2, "" );
        if ( obj->type == T_STREAM ) {
            printf( " stream@%lu", obj->u.dict->stm_pos );
        }
        break;
    default:
        printf( "?%d", obj->type );
        break;
    }
}

static void copy_out( IN *in )
{
    u8 buf[4096];
    u32 got;

    setmode( fileno( stdout ), O_BINARY );
    while ( (got = in_read( in, buf, sizeof( buf ) )) != 0 ) {
        fwrite( buf, 1, got, stdout );
    }
}

static int write_bmp( const char *path, CANVAS *cv )
{
    FILE *out = fopen( path, "wb" );
    u8 head[54], pal[4];
    u32 stride = ((u32)cv->width * (u32)cv->bpp + 3) & ~3UL;
    u32 size = stride * (u32)cv->height, offset = cv->bpp == 1 ? 54 + 1024 : 54;
    u8 *row;
    int line, index;

    if ( out == NULL ) {
        return 0;
    }
    memset( head, 0, sizeof( head ) );
    head[0] = 'B';
    head[1] = 'M';
    *(u32 *)(head + 2) = offset + size;
    *(u32 *)(head + 10) = offset;
    *(u32 *)(head + 14) = 40;
    *(s32 *)(head + 18) = cv->width;
    *(s32 *)(head + 22) = cv->height;
    *(u16 *)(head + 26) = 1;
    *(u16 *)(head + 28) = (u16)(cv->bpp * 8);
    *(u32 *)(head + 34) = size;
    fwrite( head, 1, 54, out );
    if ( cv->bpp == 1 ) {
        for ( index = 0; index < 256; index++ ) {
            pal[0] = pal[1] = pal[2] = (u8)index;
            pal[3] = 0;
            fwrite( pal, 1, 4, out );
        }
    }
    row = (u8 *)calloc( 1, stride );
    for ( line = cv->height - 1; line >= 0; line-- ) {
        memcpy( row, cv->pix + (u32)line * cv->stride, (u32)cv->width * (u32)cv->bpp );
        fwrite( row, 1, stride, out );
    }
    free( row );
    fclose( out );
    return 1;
}

static void text_page( int index )
{
    static JMPBUF trap;
    TEXTPAGE *tp;
    int why, line;

    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        tp = text_extract( index );
        if ( tp ) {
            for ( line = 0; line < tp->count; line++ ) {
                printf( "%s\n", tp->lines[line] );
            }
        } else {
            printf( "[no such page]\n" );
        }
    } else {
        printf( "[page %d: %s]\n", index + 1, why_text( why ) );
    }
    pdf_page_end();
    pdf_trap = NULL;
}

static int render_page( int index, const char *path, int zoom, int gray )
{
    static JMPBUF trap;
    static CANVAS cv;
    PAGE page;
    VIEW view;
    int why;

    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        if ( !pdf_page( index, &page ) ) {
            printf( "no such page\n" );
            pdf_page_end();
            return 1;
        }
        view_setup( &view, &page, I2FX( zoom ) / 100, 0 );
        cv.width = view.width;
        cv.height = view.height;
        cv.bpp = gray ? 1 : 3;
        cv.stride = (u32)cv.width * (u32)cv.bpp;
        cv.pix = (u8 *)malloc( cv.stride * (u32)cv.height );
        cv.org_x = cv.org_y = 0;
        canvas_clear( &cv, 0xFFFFFFUL );
        page_render( &page, &view, &cv );
        if ( path ) {
            printf( "%d x %d\n", cv.width, cv.height );
            write_bmp( path, &cv );
        }
    } else {
        printf( "[page %d: %s]\n", index + 1, why_text( why ) );
        if ( cv.pix && path ) {
            write_bmp( path, &cv );
        }
    }
    free( cv.pix );
    cv.pix = NULL;
    pdf_page_end();
    pdf_trap = NULL;
    return why;
}

int main( int argc, char **argv )
{
    static JMPBUF trap;
    PAGE page;
    OBJ *obj, *contents;
    IN *in;
    const char *pw;
    int why, index, first, last;

    if ( argc >= 2 && strcmp( argv[1], "selftest" ) == 0 ) {
        why = crypt_selftest();
        printf( "crypt selftest: %s (%X)\n", why ? "FAILED" : "ok", why );
        return why != 0;
    }
    if ( argc < 3 ) {
        printf( "ACROHOST info|obj|stream|content|text|render file.pdf ...\n" );
        return 1;
    }
    why = pdf_open( argv[2] );
    if ( why == PE_PASSWORD ) {
        pw = getenv( "ACROPW" );
        if ( pw && pdf_password( pw, strlen( pw ) ) ) {
            why = pdf_open_finish();
        }
    }
    if ( why != PE_NONE ) {
        printf( "%s: %s\n", argv[2], why_text( why ) );
        return 2;
    }

    if ( strcmp( argv[1], "text" ) == 0 ) {
        first = argc > 3 ? atoi( argv[3] ) - 1 : 0;
        last = argc > 3 ? first : doc.page_count - 1;
        for ( index = first; index <= last; index++ ) {
            if ( argc <= 3 ) {
                printf( "---- page %d ----\n", index + 1 );
            }
            text_page( index );
        }
        return 0;
    }
    if ( strcmp( argv[1], "all" ) == 0 ) {
        for ( index = 0; index < doc.page_count; index++ ) {
            render_page( index, NULL, argc > 3 ? atoi( argv[3] ) : 100, 0 );
        }
        printf( "%d pages, %lu bytes of heap in use at the end\n", doc.page_count, heap_in_use() );
        return 0;
    }
    if ( strcmp( argv[1], "render" ) == 0 && argc >= 5 ) {
        return render_page( atoi( argv[3] ) - 1, argv[4], argc > 5 ? atoi( argv[5] ) : 100,
                            argc > 6 );
    }

    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why ) {
        printf( "\n[%s]\n", why_text( why ) );
        return 3;
    }
    pdf_page_begin();
    if ( strcmp( argv[1], "info" ) == 0 ) {
        printf( "PDF %d.%d, %d page%s, %lu objects%s%s\n", doc.ver_major, doc.ver_minor,
                doc.page_count, doc.page_count == 1 ? "" : "s", doc.xref_count,
                doc.encrypted ? ", encrypted" : "", doc.repaired ? ", table rebuilt" : "" );
        for ( index = 0; index < doc.page_count && index < (argc > 3 ? atoi( argv[3] ) : 5); index++ ) {
            if ( pdf_page( index, &page ) ) {
                printf( "  page %d: %ld x %ld pt, rotate %d\n", index + 1,
                        (long)FX_ROUND( real_to_fx( page.box[2] ) - real_to_fx( page.box[0] ) ),
                        (long)FX_ROUND( real_to_fx( page.box[3] ) - real_to_fx( page.box[1] ) ),
                        page.rotate );
            } else {
                printf( "  page %d: not found\n", index + 1 );
            }
        }
    } else if ( strcmp( argv[1], "obj" ) == 0 && argc >= 4 ) {
        obj = pdf_load( (u32)atol( argv[3] ) );
        dump( obj, 0 );
        printf( "\n" );
    } else if ( strcmp( argv[1], "stream" ) == 0 && argc >= 4 ) {
        in = pdf_stream( pdf_load( (u32)atol( argv[3] ) ) );
        if ( in == NULL ) {
            fprintf( stderr, "no stream, or a filter this cannot undo\n" );
            return 3;
        }
        copy_out( in );
    } else if ( strcmp( argv[1], "content" ) == 0 && argc >= 4 ) {
        if ( !pdf_page( atoi( argv[3] ) - 1, &page ) ) {
            fprintf( stderr, "no such page\n" );
            return 3;
        }
        contents = dict_get( page.dict, "Contents" );
        if ( contents->type == T_ARR ) {
            for ( index = 0; index < (int)contents->u.arr->count; index++ ) {
                in = pdf_stream( arr_get( contents->u.arr, (u32)index ) );
                if ( in ) {
                    copy_out( in );
                    in_close( in );
                    printf( "\n" );
                }
            }
        } else if ( (in = pdf_stream( contents )) != NULL ) {
            copy_out( in );
        }
    }
    pdf_page_end();
    pdf_close();
    return 0;
}
