/*===========================================================================
 * CRC32.C  -  CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320)
 * Target: MSVC 2.2  Win32s
 *
 * Matches the CRC used by the 7z format for header and file verification.
 *===========================================================================*/

#include "crc32.h"

static uint32_t g_table[256];
static int      g_inited = 0;

static void BuildTable( void )
{
    uint32_t c;
    int      n, k;

    for ( n = 0; n < 256; n++ )
    {
        c = (uint32_t)n;
        for ( k = 0; k < 8; k++ )
            c = ( c & 1 ) ? ( 0xEDB88320UL ^ ( c >> 1 ) ) : ( c >> 1 );
        g_table[n] = c;
    }
    g_inited = 1;
}

uint32_t Crc32Init( void )
{
    if ( !g_inited )
        BuildTable();
    return 0xFFFFFFFFUL;
}

uint32_t Crc32Update( uint32_t crc, const void *data, uint32_t len )
{
    const uint8_t *p = (const uint8_t *)data;

    if ( !g_inited )
        BuildTable();

    while ( len-- )
        crc = g_table[ (uint8_t)( crc ^ *p++ ) ] ^ ( crc >> 8 );

    return crc;
}

uint32_t Crc32Done( uint32_t crc )
{
    return crc ^ 0xFFFFFFFFUL;
}

uint32_t Crc32Calc( const void *data, uint32_t len )
{
    return Crc32Done( Crc32Update( Crc32Init(), data, len ) );
}
