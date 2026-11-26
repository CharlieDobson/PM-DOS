/*
 * PDFHOST.C - PDFHOST.EXE: PMWRITE's PDF code, its type faces and its
 * PDF viewer calls as a Windows console program, for trying them
 * against files without booting PM-DOS.  It is ACROREAD's ACROHOST
 * carried over - the first six commands print what that printed, so
 * the two can be compared byte for byte - with commands for what
 * PMWRITE added.
 *
 *   PDFHOST selftest                 the ciphers and hashes
 *   PDFHOST info file.pdf [n]        version, pages, the first n pages' sizes
 *   PDFHOST text file.pdf [p]        the text of page p, or of every page
 *   PDFHOST render file.pdf p out.bmp [zoom%] [gray]
 *                                    page p as a picture
 *   PDFHOST mem file.pdf p out.bmp [zoom%] [gray]
 *                                    the same, of the file read into memory
 *   PDFHOST all file.pdf [zoom%]     every page drawn and thrown away
 *
 *   PDFHOST leak rounds file.pdf...  each file opened, every page drawn and
 *                                    read, closed - "rounds" times over - and
 *                                    the heap in use after each round
 *   PDFHOST look file.pdf p          page p's characters with their fonts,
 *                                    as the second text sink is handed them
 *   PDFHOST view file.pdf p out.bmp zoom% turn [gray] [x y w h]
 *                                    through PDFVIEW.H: the page, or the
 *                                    tile of it at x, y
 *   PDFHOST lines file.pdf p         pdfview_text's lines
 *   PDFHOST jpeg in.jpg out.bmp [file.pdf page.bmp]
 *                                    pdfview_jpeg; with a PDF, that is open
 *                                    meanwhile and its first page drawn after
 *
 *   PDFHOST sheet out.bmp base points...
 *                                    a specimen of a face (base: times,
 *                                    helvetica, courier, symbol, wingdings)
 *                                    upright, bold and slanted at each size,
 *                                    for a screen of 96 dots to the inch
 *   PDFHOST type out.bmp base style points text
 *                                    one line (style: r, b, i or bi)
 *   PDFHOST widths base style        tf_advance of 20h to FFh
 *   PDFHOST fonts                    the TrueType families that were found,
 *                                    and what each is in the four styles
 *
 * The environment variable PWFONTS names directories of TrueType files
 * (a semicolon between them).  With it, the faces are those files' where
 * a family's name matches, and "base" may be any family that was found.
 *
 * Pages count from 1.  A password, if the file needs one, is the
 * environment variable ACROPW.
 *
 * Not built by BUILD.CMD; MKPDFH.CMD builds it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include "pdf.h"
#include "gfx.h"
#include "tf.h"
#include "pdfview.h"

int mem_reclaim( void )
{
    return pdfview_reclaim() || tf_reclaim();
}

void out_of_memory( void )
{
    pdfview_out_of_memory();
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
    case PV_ABORT:    return "stopped";
    case PV_NOPAGE:   return "no such page";
    case PV_PARAM:    return "a block that cannot be drawn into";
    }
    return "?";
}

static uint8_t *read_file( const char *path, uint32_t *len )
{
    FILE *file = fopen( path, "rb" );
    uint8_t *data;
    long size;

    if ( file == NULL ) {
        return NULL;
    }
    fseek( file, 0, SEEK_END );
    size = ftell( file );
    fseek( file, 0, SEEK_SET );
    data = (uint8_t *)malloc( (size_t)size + 1 );
    if ( data == NULL || fread( data, 1, (size_t)size, file ) != (size_t)size ) {
        fclose( file );
        free( data );
        return NULL;
    }
    fclose( file );
    *len = (uint32_t)size;
    return data;
}

/* rows of one byte (gray, with a palette) or three, the top one first */
static int write_bmp( const char *path, const uint8_t *pix, int width, int height, int bpp, unsigned int pitch )
{
    FILE *out = fopen( path, "wb" );
    uint8_t head[54], pal[4];
    uint32_t stride = ((uint32_t)width * (uint32_t)bpp + 3) & ~3UL;
    uint32_t size = stride * (uint32_t)height, offset = bpp == 1 ? 54 + 1024 : 54;
    uint8_t *row;
    int line, index;

    if ( out == NULL ) {
        return 0;
    }
    memset( head, 0, sizeof( head ) );
    head[0] = 'B';
    head[1] = 'M';
    *(uint32_t *)(head + 2) = offset + size;
    *(uint32_t *)(head + 10) = offset;
    *(uint32_t *)(head + 14) = 40;
    *(int32_t *)(head + 18) = width;
    *(int32_t *)(head + 22) = height;
    *(uint16_t *)(head + 26) = 1;
    *(uint16_t *)(head + 28) = (uint16_t)(bpp * 8);
    *(uint32_t *)(head + 34) = size;
    fwrite( head, 1, 54, out );
    if ( bpp == 1 ) {
        for ( index = 0; index < 256; index++ ) {
            pal[0] = pal[1] = pal[2] = (uint8_t)index;
            pal[3] = 0;
            fwrite( pal, 1, 4, out );
        }
    }
    row = (uint8_t *)calloc( 1, stride );
    for ( line = height - 1; line >= 0; line-- ) {
        memcpy( row, pix + (unsigned int)line * pitch, (unsigned int)width * (unsigned int)bpp );
        fwrite( row, 1, stride, out );
    }
    free( row );
    fclose( out );
    return 1;
}

/* ------------------------------------------------------------------ */
/* what ACROHOST did                                                   */
/* ------------------------------------------------------------------ */

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
        cv.stride = (unsigned int)cv.width * (unsigned int)cv.bpp;
        cv.pix = (uint8_t *)malloc( cv.stride * (unsigned int)cv.height );
        cv.org_x = cv.org_y = 0;
        canvas_clear( &cv, 0xFFFFFFUL );
        page_render( &page, &view, &cv );
        if ( path ) {
            printf( "%d x %d\n", cv.width, cv.height );
            write_bmp( path, cv.pix, cv.width, cv.height, cv.bpp, cv.stride );
        }
    } else {
        printf( "[page %d: %s]\n", index + 1, why_text( why ) );
        if ( cv.pix && path ) {
            write_bmp( path, cv.pix, cv.width, cv.height, cv.bpp, cv.stride );
        }
    }
    free( cv.pix );
    cv.pix = NULL;
    pdf_page_end();
    pdf_trap = NULL;
    return why;
}

/* ------------------------------------------------------------------ */
/* the second text sink                                                */
/* ------------------------------------------------------------------ */

static void look_char( const TEXTCH *ch )
{
    printf( "U+%04X %c%c%c%c r%d x %7.2f y %7.2f adv %6.2f size %5.2f dir %5.2f %5.2f rgb %06lX [%s]",
            ch->uni, (ch->flags & FF_BOLD) ? 'B' : '-', (ch->flags & FF_ITALIC) ? 'I' : '-',
            (ch->flags & FF_FIXED) ? 'F' : '-', (ch->flags & FF_SERIF) ? 'S' : '-', ch->render,
            ch->xpos / 65536.0, ch->ypos / 65536.0, ch->advance / 65536.0, ch->size / 65536.0,
            ch->dir_x / 65536.0, ch->dir_y / 65536.0, (unsigned long)ch->rgb, ch->name );
    if ( ch->uni >= 0x20 && ch->uni < 0x7F ) {
        printf( " %c", ch->uni );
    }
    printf( "\n" );
}

static int look_page( int index )
{
    static JMPBUF trap;
    PAGE page;
    VIEW view;
    int why;

    pdf_trap = &trap;
    why = rt_setjmp( &trap );
    if ( why == 0 ) {
        pdf_page_begin();
        if ( !pdf_page( index, &page ) ) {
            printf( "no such page\n" );
        } else {
            view_setup( &view, &page, FX_ONE, 0 );
            text_sink2 = look_char;
            page_text( &page, &view );
            printf( "%d image%s\n", text_images, text_images == 1 ? "" : "s" );
        }
    } else {
        printf( "[page %d: %s]\n", index + 1, why_text( why ) );
    }
    text_sink2 = NULL;
    pdf_page_end();
    pdf_trap = NULL;
    return why;
}

/* ------------------------------------------------------------------ */
/* opening, closing and what is left                                   */
/* ------------------------------------------------------------------ */

static int open_file( const char *path )
{
    const char *pw;
    int why = pdfview_open( path );

    if ( why == PV_PASSWORD ) {
        pw = getenv( "ACROPW" );
        if ( pw ) {
            why = pdfview_password( pw, strlen( pw ) );
        }
    }
    return why;
}

static void look_none( const TEXTCH *ch )
{
    (void)ch;
}

static int cmd_leak( int rounds, int count, char **paths )
{
    static JMPBUF trap;
    static uint16_t words[] = { 'P', 'M', 'W', 'R', 'I', 'T', 'E', ' ', 0xA9, ' ', 0x20AC };
    WFONT swiss;
    PVBLOCK block;
    COVER cover;
    PAGE page;
    VIEW view;
    char **lines;
    unsigned int start = heap_in_use(), after;
    int round, file, index, pages, wide, high, got, why, failed = 0, drawn = 0;

    memset( &swiss, 0, sizeof( swiss ) );
    strcpy( swiss.name, "Arial" );
    swiss.family = FAM_SWISS;
    cover.width = 400;
    cover.height = 40;
    cover.stride = 400;
    cover.pix = (uint8_t *)calloc( 400, 40 );
    printf( "heap in use at the start: %lu\n", (unsigned long)start );
    for ( round = 1; round <= rounds; round++ ) {
        for ( file = 0; file < count; file++ ) {
            /* text of the program's own, before and while a PDF is open */
            tf_draw( tf_face( &swiss, file & 3 ), I2FX( 16 ), I2FX( 4 ), I2FX( 24 ), words, 11, &cover );
            why = open_file( paths[file] );
            if ( why != PV_OK ) {
                if ( round == 1 ) {
                    printf( "  %s: %s\n", paths[file], why_text( why ) );
                }
                failed++;
                continue;
            }
            pages = pdfview_pages();
            for ( index = 0; index < pages && index < 40; index++ ) {
                if ( pdfview_page_pixels( index, PV_ZOOM( 72, 50 ), 0, &wide, &high ) == PV_OK &&
                     wide <= 4000 && high <= 4000 ) {
                    block.width = wide;
                    block.height = high;
                    block.bpp = (index & 1) ? 1 : 3;
                    block.stride = (unsigned int)wide * (unsigned int)block.bpp;
                    block.org_x = block.org_y = 0;
                    block.pix = (uint8_t *)malloc( block.stride * (unsigned int)high );
                    pdfview_render( index, PV_ZOOM( 72, 50 ), 0, &block );
                    free( block.pix );
                    drawn++;
                }
                if ( pdfview_text( index, &lines, &got ) == PV_OK ) {
                    xfree( lines );
                }
                pdf_trap = &trap;
                if ( rt_setjmp( &trap ) == 0 ) {
                    pdf_page_begin();
                    if ( pdf_page( index, &page ) ) {
                        view_setup( &view, &page, FX_ONE, 0 );
                        text_sink2 = look_none;
                        page_text( &page, &view );
                    }
                }
                text_sink2 = NULL;
                pdf_page_end();
                pdf_trap = NULL;
                tf_draw( tf_face( NULL, 0 ), I2FX( 12 ), I2FX( 4 ), I2FX( 12 ), words, 11, &cover );
            }
            pdfview_close();
        }
        after = heap_in_use();
        printf( "after round %d: %lu bytes in use (%d files, %d could not be opened, %d pages drawn so far)\n",
                round, (unsigned long)after, count, failed, drawn );
        failed = 0;
    }
    tf_done();
    printf( "after tf_done: %lu\n", (unsigned long)heap_in_use() );
    pdf_done();
    after = heap_in_use();
    printf( "after pdf_done: %lu  (%s)\n", (unsigned long)after,
            after == start ? "where it started" : after == 0 ? "nothing left" : "NOT WHERE IT STARTED" );
    free( cover.pix );
    /* with PWFONTS set, the start had tf_font_dirs' copy of it in use,
       and tf_done gave that back: less than the start is no leak */
    return after != start && after != 0;
}

/* ------------------------------------------------------------------ */
/* PDFVIEW.H                                                           */
/* ------------------------------------------------------------------ */

static int cmd_view( int argc, char **argv )
{
    PVBLOCK block;
    int32_t zoom = PV_ZOOM( 72, atoi( argv[5] ) ), wide_pt, high_pt;
    int page = atoi( argv[3] ) - 1, turn = atoi( argv[6] ), gray = 0, at = 7, wide, high, why;

    why = open_file( argv[2] );
    if ( why != PV_OK ) {
        printf( "%s: %s\n", argv[2], why_text( why ) );
        return 2;
    }
    if ( argc > at && strcmp( argv[at], "gray" ) == 0 ) {
        gray = 1;
        at++;
    }
    why = pdfview_page_size( page, &wide_pt, &high_pt );
    if ( why == PV_OK ) {
        why = pdfview_page_pixels( page, zoom, turn, &wide, &high );
    }
    if ( why != PV_OK ) {
        printf( "page %d: %s\n", page + 1, why_text( why ) );
        return 2;
    }
    printf( "%d pages; page %d is %.2f x %.2f points, %d x %d pixels\n", pdfview_pages(), page + 1,
            wide_pt / 65536.0, high_pt / 65536.0, wide, high );
    block.org_x = block.org_y = 0;
    block.width = wide;
    block.height = high;
    if ( argc >= at + 4 ) {
        block.org_x = atoi( argv[at] );
        block.org_y = atoi( argv[at + 1] );
        block.width = atoi( argv[at + 2] );
        block.height = atoi( argv[at + 3] );
    }
    block.bpp = gray ? 1 : 3;
    /* rows further apart than they are wide, as a screen's may be */
    block.stride = (unsigned int)block.width * (unsigned int)block.bpp + 5;
    block.pix = (uint8_t *)malloc( block.stride * (unsigned int)block.height );
    why = pdfview_render( page, zoom, turn, &block );
    if ( why != PV_OK ) {
        printf( "page %d: %s\n", page + 1, why_text( why ) );
    }
    write_bmp( argv[4], block.pix, block.width, block.height, block.bpp, block.stride );
    free( block.pix );
    pdfview_close();
    return why;
}

static int cmd_lines( const char *path, int page )
{
    char **lines;
    int count, index, why = open_file( path );

    if ( why == PV_OK ) {
        why = pdfview_text( page, &lines, &count );
    }
    if ( why != PV_OK ) {
        printf( "%s: %s\n", path, why_text( why ) );
        return 2;
    }
    for ( index = 0; index < count; index++ ) {
        printf( "%s\n", lines[index] );
    }
    xfree( lines );
    pdfview_close();
    return 0;
}

/* with a PDF named as well, that is opened first and its first page
   drawn afterwards: the picture must not have disturbed it */
static int cmd_jpeg( const char *in_path, const char *out_path, const char *pdf_path, const char *page_path )
{
    uint8_t *data, *pix;
    uint32_t len, wide, high;
    unsigned int before;

    if ( pdf_path != NULL && open_file( pdf_path ) != PV_OK ) {
        printf( "%s: cannot be opened\n", pdf_path );
        return 2;
    }
    before = heap_in_use();
    data = read_file( in_path, &len );
    if ( data == NULL ) {
        printf( "%s: cannot be read\n", in_path );
        return 2;
    }
    pix = pdfview_jpeg( data, len, &wide, &high );
    free( data );
    if ( pix == NULL ) {
        printf( "%s: NULL (heap %lu before, %lu after)\n", in_path, (unsigned long)before,
                (unsigned long)heap_in_use() );
        return 1;
    }
    printf( "%s: %lu x %lu\n", in_path, (unsigned long)wide, (unsigned long)high );
    write_bmp( out_path, pix, (int)wide, (int)high, 3, wide * 3 );
    xfree( pix );
    printf( "heap %lu before, %lu after\n", (unsigned long)before, (unsigned long)heap_in_use() );
    if ( pdf_path != NULL && page_path != NULL ) {
        render_page( 0, page_path, 100, 0 );
        pdfview_close();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* type faces                                                          */
/* ------------------------------------------------------------------ */

static int font_named( const char *base, WFONT *font )
{
    memset( font, 0, sizeof( *font ) );
    if ( stricmp( base, "times" ) == 0 ) {
        strcpy( font->name, "Times New Roman" );
        font->family = FAM_ROMAN;
    } else if ( stricmp( base, "helvetica" ) == 0 ) {
        strcpy( font->name, "Arial" );
        font->family = FAM_SWISS;
    } else if ( stricmp( base, "courier" ) == 0 ) {
        strcpy( font->name, "Courier New" );
        font->family = FAM_MODERN;
        font->fixed = 1;
    } else if ( stricmp( base, "symbol" ) == 0 ) {
        strcpy( font->name, "Symbol" );
        font->family = FAM_SYMBOL;
    } else if ( stricmp( base, "wingdings" ) == 0 ) {
        strcpy( font->name, "Wingdings" );
        font->family = FAM_SYMBOL;
    } else if ( tf_family( 0 ) != NULL ) {
        /* with font files to hand (PWFONTS), any family's name */
        strncpy( font->name, base, FONT_NAME_LEN - 1 );
        font->family = FAM_ANY;
    } else {
        return 0;
    }
    return 1;
}

/* the families that were found in PWFONTS' directories, and what each
   is in the four styles */
static int cmd_fonts( void )
{
    static const char *const styles[4] = { "regular", "bold", "italic", "bold italic" };
    WFONT font;
    TFINFO info;
    TFACE *face;
    const char *name;
    unsigned int index, before = heap_in_use();
    int style, ascent, descent, gap, made, count = 0;

    for ( index = 0; (name = tf_family( index )) != NULL; index++ ) {
        printf( "%s\n", name );
        memset( &font, 0, sizeof( font ) );
        strncpy( font.name, name, FONT_NAME_LEN - 1 );
        for ( style = 0; style < 4; style++ ) {
            face = tf_face( &font, style );
            tf_metrics( face, &ascent, &descent, &gap );
            made = tf_made( face );
            if ( tf_is_builtin( face ) ) {
                printf( "  %-11s the built-in face\n", styles[style] );
                continue;
            }
            tf_file_info( face, &info );
            printf( "  %-11s %s, %lu glyphs, lines %d+%d+%d, A is %lu wide%s%s%s\n", styles[style], info.name,
                    (unsigned long)info.glyphs, ascent, descent, gap, (unsigned long)tf_advance( face, 'A' ),
                    (made & CF_BOLD) ? ", bold made" : "", (made & CF_ITALIC) ? ", slope made" : "",
                    tf_embed( face ) ? "" : ", not for a PDF" );
            count++;
        }
    }
    printf( "%lu families, %d faces from files; heap in use %lu", (unsigned long)index, count,
            (unsigned long)(heap_in_use() - before) );
    printf( ", after tf_reclaim twice %lu", (unsigned long)((tf_reclaim(), tf_reclaim(), heap_in_use()) - before) );
    tf_done();
    printf( ", after tf_done %lu\n", (unsigned long)heap_in_use() );
    return 0;
}

static int style_named( const char *text )
{
    return (strchr( text, 'b' ) ? CF_BOLD : 0) | (strchr( text, 'i' ) ? CF_ITALIC : 0);
}

/* UTF-8 from the command line, or \uXXXX, as UTF-16 */
static unsigned int to_utf16( const char *text, uint16_t *out, unsigned int max )
{
    const uint8_t *cur = (const uint8_t *)text;
    unsigned int len = 0;
    uint32_t code;

    while ( *cur && len + 2 < max ) {
        if ( cur[0] == '\\' && cur[1] == 'u' && strlen( (const char *)cur ) >= 6 ) {
            char hex[5];

            memcpy( hex, cur + 2, 4 );
            hex[4] = 0;
            code = strtoul( hex, NULL, 16 );
            cur += 6;
        } else if ( cur[0] < 0x80 ) {
            code = *cur++;
        } else if ( (cur[0] & 0xE0) == 0xC0 && cur[1] ) {
            code = ((uint32_t)(cur[0] & 0x1F) << 6) | (cur[1] & 0x3F);
            cur += 2;
        } else if ( (cur[0] & 0xF0) == 0xE0 && cur[1] && cur[2] ) {
            code = ((uint32_t)(cur[0] & 0x0F) << 12) | ((uint32_t)(cur[1] & 0x3F) << 6) | (cur[2] & 0x3F);
            cur += 3;
        } else {
            code = *cur++;
        }
        out[len++] = (uint16_t)code;
    }
    return len;
}

/* black on white, from a block of coverage */
static int write_cover( const char *path, COVER *cover )
{
    unsigned int index, count = (unsigned int)cover->width * (unsigned int)cover->height;

    for ( index = 0; index < count; index++ ) {
        cover->pix[index] = (uint8_t)(255 - cover->pix[index]);
    }
    return write_bmp( path, cover->pix, cover->width, cover->height, 1, cover->stride );
}

static const uint16_t pangram[] = {
    'T','h','e',' ','q','u','i','c','k',' ','b','r','o','w','n',' ','f','o','x',' ','j','u','m','p','s',' ',
    'o','v','e','r',' ','t','h','e',' ','l','a','z','y',' ','d','o','g','.',' ','0','1','2','3','4','5','6',
    '7','8','9'
};
static const uint16_t capitals[] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    ' ','(','a',')',' ','[','b',']',' ','{','c','}',' ','&',' ','@',' ','#',' ','$','1','2','.','5','0',' ',
    '9','9','%'
};
/* the drawings SFONT.C was given for PMWRITE, in Latin-1's order and then the rest */
static const uint16_t later[] = {
    0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2,
    0xB3, 0xB4, 0xB5, 0xB6, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xD0, 0xD7, 0xD8, 0xDE, 0xDF,
    0xF0, 0xF7, 0xF8, 0xFE, 0x20AC, 0x201E, 0x2020, 0x2021, 0x2030, 0x2039, 0x201C, 0x201D, 0x2122, 0x203A
};
static const uint16_t mixed[] = {
    0x201C, 'Q', 'u', 'o', 't', 'e', 'd', 0x201D, ' ', 0x2018, 'o', 'n', 'e', 0x2019, ' ', 0x2013, ' ',
    0x2014, ' ', 0x2026, ' ', 0x2022, ' ', 0xFB01, ' ', 0xFB02, ' ', 0xC6, 0xE6, 0x152, 0x153, ' ', 0xC0,
    0xC9, 0xCE, 0xD5, 0xDC, 0xE0, 0xE9, 0xEE, 0xF5, 0xFC, 0xE7, 0xF1, 0x160, 0x17E, ' ', 0xA3, '5', ' ',
    0x20AC, '9', ' ', '2', 0xBD, ' ', '2', '0', 0xB0, ' ', 0xA9, ' ', '1', '9', '9', '5', ' ', 0x3B1,
    0x2192, 0x2713
};

#define SHEET_LINES 4

static int cmd_sheet( const char *path, const char *base, int sizes, char **points )
{
    static const struct {
        const uint16_t *text;
        unsigned int len;
    } lines[SHEET_LINES] = {
        { pangram, sizeof( pangram ) / 2 }, { capitals, sizeof( capitals ) / 2 },
        { later, sizeof( later ) / 2 }, { mixed, sizeof( mixed ) / 2 }
    };
    WFONT font;
    COVER cover;
    TFACE *face;
    int32_t em, end, widest = 0, ypos = 0;
    int ascent, descent, gap, size, style, line, pass, row, height = 0;

    if ( !font_named( base, &font ) ) {
        printf( "no base is called %s\n", base );
        return 2;
    }
    tf_init();
    memset( &cover, 0, sizeof( cover ) );
    /* once to measure, once to draw */
    for ( pass = 0; pass < 2; pass++ ) {
        ypos = I2FX( 4 );
        for ( size = 0; size < sizes; size++ ) {
            em = (int32_t)(atof( points[size] ) * 96.0 / 72.0 * 65536.0);
            for ( style = 0; style < 3; style++ ) {
                face = tf_face( &font, style == 1 ? CF_BOLD : (style == 2 ? CF_ITALIC : 0) );
                tf_metrics( face, &ascent, &descent, &gap );
                for ( line = 0; line < SHEET_LINES; line++ ) {
                    ypos += mul_div( em, ascent, 1000 );
                    end = tf_draw( face, em, I2FX( 6 ), ypos, lines[line].text, lines[line].len,
                                   pass ? &cover : NULL );
                    if ( end > widest ) {
                        widest = end;
                    }
                    ypos += mul_div( em, descent + gap, 1000 );
                }
                ypos += em / 4;
            }
        }
        if ( pass == 0 ) {
            cover.width = (int)FX_CEIL( widest ) + 8;
            cover.height = height = (int)FX_CEIL( ypos ) + 4;
            cover.stride = (unsigned int)cover.width;
            cover.pix = (uint8_t *)calloc( cover.stride, (size_t)cover.height );
        }
    }
    row = write_cover( path, &cover );
    printf( "%s: %d x %d\n", path, cover.width, height );
    free( cover.pix );
    tf_done();
    return row ? 0 : 2;
}

static int cmd_type( const char *path, const char *base, const char *style, const char *points,
                     const char *text )
{
    static uint16_t wide[512];
    WFONT font;
    COVER cover;
    TFACE *face;
    int32_t em = (int32_t)(atof( points ) * 96.0 / 72.0 * 65536.0), end;
    unsigned int len = to_utf16( text, wide, 512 );
    int ascent, descent, gap;

    if ( !font_named( base, &font ) ) {
        printf( "no base is called %s\n", base );
        return 2;
    }
    face = tf_face( &font, style_named( style ) );
    tf_metrics( face, &ascent, &descent, &gap );
    end = tf_draw( face, em, I2FX( 4 ), 0, wide, len, NULL );
    cover.width = (int)FX_CEIL( end ) + 8;
    cover.height = (int)FX_CEIL( mul_div( em, ascent + descent + gap, 1000 ) ) + 8;
    cover.stride = (unsigned int)cover.width;
    cover.pix = (uint8_t *)calloc( cover.stride, (size_t)cover.height );
    tf_draw( face, em, I2FX( 4 ), I2FX( 4 ) + mul_div( em, ascent, 1000 ), wide, len, &cover );
    if ( getenv( "PWRECLAIM" ) != NULL ) {
        /* SET PWRECLAIM=1: the shapes and then the font files given
           back, as when memory is short, and the line drawn again: it
           must come out the same */
        COVER again = cover;
        int gave = tf_reclaim();

        gave += tf_reclaim();
        again.pix = (uint8_t *)calloc( cover.stride, (size_t)cover.height );
        tf_draw( face, em, I2FX( 4 ), I2FX( 4 ) + mul_div( em, ascent, 1000 ), wide, len, &again );
        printf( "tf_reclaim gave back %d times; drawn again: %s\n", gave,
                memcmp( again.pix, cover.pix, cover.stride * (unsigned int)cover.height ) == 0 ? "the same" : "DIFFERENT" );
        free( again.pix );
    }
    write_cover( path, &cover );
    printf( "%s: %d x %d, the pen ends at %.2f; ascent %d descent %d gap %d\n", path, cover.width,
            cover.height, end / 65536.0, ascent, descent, gap );
    free( cover.pix );
    return 0;
}

static int cmd_widths( const char *base, const char *style )
{
    WFONT font;
    TFACE *face;
    uint32_t code;

    if ( !font_named( base, &font ) ) {
        printf( "no base is called %s\n", base );
        return 2;
    }
    face = tf_face( &font, style_named( style ) );
    for ( code = 0x20; code < 0x100; code++ ) {
        printf( "%4lu%s", (unsigned long)tf_advance( face, code ), (code & 15) == 15 ? "\n" : " " );
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static void dump( OBJ *obj, int depth )
{
    unsigned int index;

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

int main( int argc, char **argv )
{
    static JMPBUF trap;
    PAGE page;
    OBJ *obj;
    IN *in;
    uint8_t *data = NULL, buf[4096];
    const char *pw;
    uint32_t len, got;
    int why, index, first, last;

    /* SET PDFSFONT=RIGHT: a PDF's built-in font drawn as it should be,
       not as ACROREAD drew it (PDF\SFONT.C) */
    if ( getenv( "PDFSFONT" ) != NULL && stricmp( getenv( "PDFSFONT" ), "right" ) == 0 ) {
        sfont_as_acroread = 0;
    }
    /* SET PWFONTS=dir;dir: TrueType files for the type faces.  Only
       those directories are searched: a test's result is not to depend
       on what fonts the machine it is run on happens to have */
    if ( getenv( "PWFONTS" ) != NULL ) {
        tf_font_dirs( getenv( "PWFONTS" ), 0 );
    }
    /* SET PWINK=96: coverage as a 16-color screen is given it, where
       96 and above is ink (tf_screen) */
    if ( getenv( "PWINK" ) != NULL ) {
        tf_screen( atoi( getenv( "PWINK" ) ) );
    }
    if ( argc >= 2 && strcmp( argv[1], "fonts" ) == 0 ) {
        return cmd_fonts();
    }
    if ( argc >= 2 && strcmp( argv[1], "selftest" ) == 0 ) {
        why = crypt_selftest();
        printf( "crypt selftest: %s (%X)\n", why ? "FAILED" : "ok", why );
        return why != 0;
    }
    if ( argc >= 4 && strcmp( argv[1], "leak" ) == 0 ) {
        return cmd_leak( atoi( argv[2] ), argc - 3, argv + 3 );
    }
    if ( argc >= 7 && strcmp( argv[1], "view" ) == 0 ) {
        return cmd_view( argc, argv );
    }
    if ( argc >= 4 && strcmp( argv[1], "lines" ) == 0 ) {
        return cmd_lines( argv[2], atoi( argv[3] ) - 1 );
    }
    if ( argc >= 4 && strcmp( argv[1], "jpeg" ) == 0 ) {
        return cmd_jpeg( argv[2], argv[3], argc > 5 ? argv[4] : NULL, argc > 5 ? argv[5] : NULL );
    }
    if ( argc >= 5 && strcmp( argv[1], "sheet" ) == 0 ) {
        return cmd_sheet( argv[2], argv[3], argc - 4, argv + 4 );
    }
    if ( argc >= 7 && strcmp( argv[1], "type" ) == 0 ) {
        return cmd_type( argv[2], argv[3], argv[4], argv[5], argv[6] );
    }
    if ( argc >= 4 && strcmp( argv[1], "widths" ) == 0 ) {
        return cmd_widths( argv[2], argv[3] );
    }
    if ( argc < 3 ) {
        printf( "PDFHOST selftest | info|text|render|mem|all|look|lines|view file.pdf ... | leak n file.pdf...\n"
                "        | jpeg in.jpg out.bmp | sheet out.bmp base points... | type out.bmp base style points text\n"
                "        | widths base style\n" );
        return 1;
    }
    if ( strcmp( argv[1], "mem" ) == 0 ) {
        data = read_file( argv[2], &len );
        why = data ? pdf_open_mem( data, len ) : PE_IO;
    } else {
        why = pdf_open( argv[2] );
    }
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
    if ( (strcmp( argv[1], "render" ) == 0 || strcmp( argv[1], "mem" ) == 0) && argc >= 5 ) {
        why = render_page( atoi( argv[3] ) - 1, argv[4], argc > 5 ? atoi( argv[5] ) : 100, argc > 6 );
        pdf_close();
        free( data );
        return why;
    }
    if ( strcmp( argv[1], "look" ) == 0 && argc >= 4 ) {
        return look_page( atoi( argv[3] ) - 1 );
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
        obj = pdf_load( (uint32_t)atol( argv[3] ) );
        dump( obj, 0 );
        printf( "\n" );
    } else if ( strcmp( argv[1], "stream" ) == 0 && argc >= 4 ) {
        in = pdf_stream( pdf_load( (uint32_t)atol( argv[3] ) ) );
        if ( in == NULL ) {
            fprintf( stderr, "no stream, or a filter this cannot undo\n" );
            return 3;
        }
        setmode( fileno( stdout ), O_BINARY );
        while ( (got = in_read( in, buf, sizeof( buf ) )) != 0 ) {
            fwrite( buf, 1, got, stdout );
        }
    }
    pdf_page_end();
    pdf_close();
    return 0;
}
