/*=====================================================================
 * D32DRV.C - make a 32-bit PM-DOS driver file out of two links of
 *            the same driver, for BUILD.CMD (INC\NDRV.INC)
 *
 *     D32DRV <out.sys> <stub.bin> <image0> <image1>
 *
 * <image0> and <image1> are the same D32 driver image, type D32T_DRV,
 * as the linker wrote it at two offsets: 0 and 10000000h.  Every
 * dword that holds an address differs between the two by exactly
 * 10000000h, and nothing else differs at all - which is the check:
 * a byte that differs in any other way stops the build, because it
 * would be a relocation this cannot describe (a 16-bit address, say)
 * and a driver that would run at the wrong place.
 *
 * 10000000h and not something smaller, because a difference that is
 * all in one byte, with no carry into the next, is what makes the
 * scan exact: the three low bytes of an address are the same in both
 * links, so a window that starts one, two or three bytes before an
 * address sees a difference of 10000000h shifted, never 10000000h,
 * and the only window that matches is the address itself.
 *
 * The file written is <stub.bin> (128 bytes, TOOLS\DRVSTUB.ASM), then
 * <image0> with D32H_RELOC and D32H_NRELOC filled in, then the table:
 * one dword per relocation, the offset of the dword from the start of
 * the image.  The kernel adds (where it landed - D32H_BASE) to each
 * (DOS\NDLOAD.INC).
 *
 * Says nothing when it works.  Exits 1 when a file is not what it has
 * to be and 2 when one cannot be opened or written.
 *
 * Built with:  wcl386 -q -bt=nt -fe=d32drv.exe d32drv.c
 *=====================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D32_HDRLEN  32              /* D32H_SIZE in PMDOS.INC */
#define D32H_VER    4
#define D32H_TYPE   5
#define D32H_BASE   8
#define D32H_ENTRY  12
#define D32H_RELOC  24
#define D32H_NRELOC 28
#define D32T_DRV    1
#define D32_VERSION 1

#define STUB_LEN    128             /* D32_DRVSTUB in PMDOS.INC */
#define STUB_NAME   10              /* DEVH_NAME: where the mark is */
#define DELTA       0x10000000UL    /* the second link's offset */

static unsigned long get32( const unsigned char *at )
{
    return (unsigned long)at[0] | ( (unsigned long)at[1] << 8 ) |
           ( (unsigned long)at[2] << 16 ) | ( (unsigned long)at[3] << 24 );
}

static void put32( unsigned char *at, unsigned long value )
{
    at[0] = (unsigned char)value;
    at[1] = (unsigned char)( value >> 8 );
    at[2] = (unsigned char)( value >> 16 );
    at[3] = (unsigned char)( value >> 24 );
}

/* the whole of a file, or exit 2 */
static unsigned char *load( const char *path, long *size )
{
    FILE          *file;
    unsigned char *data;

    file = fopen( path, "rb" );
    if ( file == NULL ) {
        printf( "D32DRV: %s could not be opened\n", path );
        exit( 2 );
    }
    fseek( file, 0L, SEEK_END );
    *size = ftell( file );
    fseek( file, 0L, SEEK_SET );
    data = (unsigned char *)malloc( (size_t)*size + 4 );
    if ( data == NULL || fread( data, 1, (size_t)*size, file ) != (size_t)*size ) {
        printf( "D32DRV: %s could not be read\n", path );
        exit( 2 );
    }
    fclose( file );
    return data;
}

static int is_driver( const unsigned char *data, long size, const char *path )
{
    if ( size < D32_HDRLEN || data[0] != 'C' || data[1] != 'D' ) {
        printf( "D32DRV: %s is not a D32 image\n", path );
        return 0;
    }
    if ( data[D32H_VER] != D32_VERSION || data[D32H_TYPE] != D32T_DRV ) {
        printf( "D32DRV: %s is not a D32 driver image\n", path );
        return 0;
    }
    if ( get32( data + D32H_RELOC ) != 0 || get32( data + D32H_NRELOC ) != 0 ) {
        printf( "D32DRV: %s already has a relocation table\n", path );
        return 0;
    }
    return 1;
}

int main( int argc, char **argv )
{
    unsigned char *stub, *image0, *image1, *table;
    long           stub_size, size0, size1, at, count;
    FILE          *out;

    if ( argc != 5 ) {
        printf( "usage: D32DRV <out.sys> <stub.bin> <image0> <image1>\n" );
        return 2;
    }

    stub = load( argv[2], &stub_size );
    if ( stub_size != STUB_LEN || memcmp( stub + STUB_NAME, "PMD32DRV", 8 ) != 0 ) {
        printf( "D32DRV: %s is not the driver stub\n", argv[2] );
        return 1;
    }

    image0 = load( argv[3], &size0 );
    image1 = load( argv[4], &size1 );
    if ( !is_driver( image0, size0, argv[3] ) || !is_driver( image1, size1, argv[4] ) ) {
        return 1;
    }
    if ( size0 != size1 ) {
        printf( "D32DRV: %s and %s are not the same length\n", argv[3], argv[4] );
        return 1;
    }
    if ( get32( image1 + D32H_BASE ) - get32( image0 + D32H_BASE ) != DELTA ) {
        printf( "D32DRV: %s and %s are not linked 10000000h apart\n", argv[3], argv[4] );
        return 1;
    }

    /* THE SCAN.  A dword that moved by the delta is a relocation; a
     * byte that differs any other way is a file this cannot describe. */
    table = (unsigned char *)malloc( (size_t)size0 + 4 );
    if ( table == NULL ) {
        printf( "D32DRV: out of memory\n" );
        return 2;
    }
    count = 0;
    at = 0;
    while ( at < size0 ) {
        if ( at + 4 <= size0 && get32( image1 + at ) - get32( image0 + at ) == DELTA ) {
            if ( at < D32H_BASE || ( at > D32H_ENTRY && at < D32_HDRLEN ) ) {
                printf( "D32DRV: %s has an address in its header at %ld\n", argv[3], at );
                return 1;
            }
            put32( table + count * 4, (unsigned long)at );
            count++;
            at += 4;
        } else if ( image0[at] != image1[at] ) {
            printf( "D32DRV: %s and %s differ at byte %ld, and not by a relocation\n",
                    argv[3], argv[4], at );
            return 1;
        } else {
            at++;
        }
    }
    if ( count < 2 ) {
        printf( "D32DRV: %s has no relocations: not a D32_DRVHEAD image\n", argv[3] );
        return 1;
    }

    /* the header says where the table is: right after the image */
    put32( image0 + D32H_RELOC, (unsigned long)size0 );
    put32( image0 + D32H_NRELOC, (unsigned long)count );

    out = fopen( argv[1], "wb" );
    if ( out == NULL ) {
        printf( "D32DRV: %s could not be written\n", argv[1] );
        return 2;
    }
    if ( fwrite( stub, 1, STUB_LEN, out ) != STUB_LEN ||
         fwrite( image0, 1, (size_t)size0, out ) != (size_t)size0 ||
         fwrite( table, 1, (size_t)( count * 4 ), out ) != (size_t)( count * 4 ) ||
         fclose( out ) != 0 ) {
        printf( "D32DRV: %s could not be written\n", argv[1] );
        return 2;
    }
    return 0;
}
