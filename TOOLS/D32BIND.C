/*=====================================================================
 * D32BIND.C - put an MS-DOS program in front of a D32 image, for
 *             BUILD.CMD
 *
 *     D32BIND <program> <stub>
 *
 * <program> is a D32 image as the linker wrote it: 'CD' at byte zero.
 * It is rewritten in place as <stub>, a little padding, and the
 * image, with the image's offset in the dword at 3Ch of the stub's MZ
 * header.  Real MS-DOS then runs the stub; PM-DOS follows 3Ch, finds
 * 'CD' and loads what is there (d32_find and exec_load, INT21.INC).
 *
 * <stub> is TOOLS\D32STUB.EXE for every command that ships - it prints
 * one sentence and ends - or a 16-bit .EXE of the program's own, for
 * one that is to run on both systems.
 *
 * A <program> that already has a stub gets the new one in its place,
 * so binding twice is harmless.
 *
 * TWO THINGS ARE MADE TRUE HERE, because the kernel depends on both:
 *
 *   - The stub's fixup table starts at 40h or later.  That is what
 *     says an MZ header HAS a 3Ch; a linker that puts the table at
 *     1Ch or 1Eh has fixups, or nothing in particular, where the
 *     dword would go.  Such a table is moved to 40h - inside the
 *     header it has when there is room, so that nothing behind it
 *     shifts, and into a longer header when there is not.
 *
 *   - The image starts on a paragraph and its 32-byte header does not
 *     cross a 512-byte boundary, so the header is whole in one
 *     cluster whatever the disk's cluster size.
 *
 * Says nothing when it works.  Exits 1 when a file is not what it has
 * to be and 2 when one cannot be opened or written.
 *
 * Built with:  wcl386 -q -bt=nt -fe=d32bind.exe d32bind.c
 *=====================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MZ_CBLP     0x02        /* bytes used in the last page */
#define MZ_CP       0x04        /* pages of 512, last included */
#define MZ_CRLC     0x06        /* fixups */
#define MZ_CPARHDR  0x08        /* header, in paragraphs */
#define MZ_LFARLC   0x18        /* where the fixup table is */
#define MZ_FIXED    0x1C        /* the fields every MZ header has */
#define MZ_LFANEW   0x3C        /* where the newer header is */
#define MZ_NEWHDR   0x40        /* a header that has that field */

#define D32_HDRLEN  32          /* D32H_SIZE in PMDOS.INC */

static unsigned get16( const unsigned char *at )
{
    return (unsigned)at[0] | ( (unsigned)at[1] << 8 );
}

static unsigned long get32( const unsigned char *at )
{
    return (unsigned long)get16( at ) | ( (unsigned long)get16( at + 2 ) << 16 );
}

static void put16( unsigned char *at, unsigned value )
{
    at[0] = (unsigned char)value;
    at[1] = (unsigned char)( value >> 8 );
}

static void put32( unsigned char *at, unsigned long value )
{
    put16( at, (unsigned)( value & 0xFFFFUL ) );
    put16( at + 2, (unsigned)( value >> 16 ) );
}

/* the whole of a file, or exit 2 */
static unsigned char *load( const char *path, long *size )
{
    FILE          *file;
    unsigned char *data;

    file = fopen( path, "rb" );
    if ( file == NULL ) {
        printf( "D32BIND: %s could not be opened\n", path );
        exit( 2 );
    }
    fseek( file, 0L, SEEK_END );
    *size = ftell( file );
    fseek( file, 0L, SEEK_SET );
    data = (unsigned char *)malloc( (size_t)*size + 1 );
    if ( data == NULL || fread( data, 1, (size_t)*size, file ) != (size_t)*size ) {
        printf( "D32BIND: %s could not be read\n", path );
        exit( 2 );
    }
    fclose( file );
    return data;
}

static int is_mz( const unsigned char *data, long size )
{
    return size >= MZ_FIXED &&
           ( ( data[0] == 'M' && data[1] == 'Z' ) || ( data[0] == 'Z' && data[1] == 'M' ) );
}

/* where a file's D32 image starts: 0, what 3Ch says, or -1 for none */
static long image_at( const unsigned char *data, long size )
{
    unsigned long offset;

    if ( size >= D32_HDRLEN && data[0] == 'C' && data[1] == 'D' ) {
        return 0;
    }
    if ( is_mz( data, size ) && size >= MZ_NEWHDR + D32_HDRLEN && get16( data + MZ_LFARLC ) >= MZ_NEWHDR ) {
        offset = get32( data + MZ_LFANEW );
        if ( offset >= MZ_NEWHDR && offset <= (unsigned long)( size - D32_HDRLEN ) &&
             data[offset] == 'C' && data[offset + 1] == 'D' ) {
            return (long)offset;
        }
    }
    return -1;
}

int main( int argc, char **argv )
{
    unsigned char *program, *stub, *front;
    long           program_size, stub_size, image_start, front_size, image_offset;
    unsigned long  header, new_header, table, fixups, declared, pointed;
    FILE          *out;

    if ( argc != 3 ) {
        printf( "usage: D32BIND <program> <stub>\n" );
        return 2;
    }

    program = load( argv[1], &program_size );
    image_start = image_at( program, program_size );
    if ( image_start < 0 ) {
        printf( "D32BIND: %s is not a D32 image\n", argv[1] );
        return 1;
    }

    stub = load( argv[2], &stub_size );
    if ( !is_mz( stub, stub_size ) ) {
        printf( "D32BIND: %s is not an MS-DOS .EXE\n", argv[2] );
        return 1;
    }
    header = (unsigned long)get16( stub + MZ_CPARHDR ) * 16;
    table  = get16( stub + MZ_LFARLC );
    fixups = get16( stub + MZ_CRLC );
    if ( header < MZ_FIXED || header > (unsigned long)stub_size || table + fixups * 4 > (unsigned long)stub_size ) {
        printf( "D32BIND: the MZ header of %s does not describe the file\n", argv[2] );
        return 1;
    }
    if ( table >= MZ_NEWHDR && stub_size >= MZ_NEWHDR ) {
        pointed = get32( stub + MZ_LFANEW );
        if ( pointed >= MZ_NEWHDR && pointed < (unsigned long)stub_size ) {
            printf( "D32BIND: %s already has a header at the offset in 3Ch\n", argv[2] );
            return 1;
        }
    }

    /* the stub, with a header that has a 3Ch */
    new_header = header;
    if ( table < MZ_NEWHDR || header < MZ_NEWHDR ) {
        if ( header < MZ_NEWHDR + fixups * 4 ) {
            new_header = ( MZ_NEWHDR + fixups * 4 + 15 ) & ~15UL;
        }
    }
    front_size = (long)( new_header + ( (unsigned long)stub_size - header ) );
    front = (unsigned char *)calloc( (size_t)front_size + 1, 1 );
    if ( front == NULL ) {
        printf( "D32BIND: out of memory\n" );
        return 2;
    }
    if ( table >= MZ_NEWHDR && header >= MZ_NEWHDR ) {
        memcpy( front, stub, (size_t)stub_size );
    } else {
        memcpy( front, stub, MZ_FIXED );
        memcpy( front + MZ_NEWHDR, stub + table, (size_t)( fixups * 4 ) );
        memcpy( front + new_header, stub + header, (size_t)( (unsigned long)stub_size - header ) );
        put16( front + MZ_LFARLC, MZ_NEWHDR );
        if ( new_header != header ) {
            declared = (unsigned long)get16( stub + MZ_CP ) * 512;
            if ( get16( stub + MZ_CBLP ) != 0 ) {
                declared -= 512 - get16( stub + MZ_CBLP );
            }
            if ( declared < header ) {
                printf( "D32BIND: the MZ header of %s does not describe the file\n", argv[2] );
                return 1;
            }
            declared += new_header - header;
            put16( front + MZ_CPARHDR, (unsigned)( new_header / 16 ) );
            put16( front + MZ_CBLP, (unsigned)( declared % 512 ) );
            put16( front + MZ_CP, (unsigned)( ( declared + 511 ) / 512 ) );
        }
    }

    /* where the image goes: a paragraph, and its header in one sector */
    image_offset = ( front_size + 15 ) & ~15L;
    if ( image_offset % 512 > 512 - D32_HDRLEN ) {
        image_offset = ( image_offset + 511 ) & ~511L;
    }
    put32( front + MZ_LFANEW, (unsigned long)image_offset );

    out = fopen( argv[1], "wb" );
    if ( out == NULL ) {
        printf( "D32BIND: %s could not be written\n", argv[1] );
        return 2;
    }
    if ( fwrite( front, 1, (size_t)front_size, out ) != (size_t)front_size ) {
        printf( "D32BIND: %s could not be written\n", argv[1] );
        return 2;
    }
    while ( front_size < image_offset ) {
        fputc( 0, out );
        front_size++;
    }
    if ( fwrite( program + image_start, 1, (size_t)( program_size - image_start ), out )
             != (size_t)( program_size - image_start ) || fclose( out ) != 0 ) {
        printf( "D32BIND: %s could not be written\n", argv[1] );
        return 2;
    }
    return 0;
}
