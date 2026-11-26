/*
 * IMZ.C - a diskette image in a ZIP archive (IMZ.H).
 *
 * ONLY WHAT A .IMZ NEEDS.  PMWRITE's ZIP.C reads and writes whole
 * packages, but it writes through PMWRITE's OUT layer and pulls its
 * document headers in behind it; this is the one-member subset, with
 * the packing left to the FLATE.C they share.
 *
 * EVERY OFFSET IS CHECKED AGAINST THE BYTES THERE ARE before it is
 * used, in the form "off <= len && n <= len - off" so that no sum can
 * wrap: the archive is a file from a network drive and its numbers are
 * whatever is in it.  The central directory is believed over the local
 * headers (it is what an archiver writes last), but the local header's
 * own name and extra lengths place the data, as every reader does.
 */
#include "rt.h"
#include "sys.h"
#include "crc.h"
#include "flate.h"
#include "imz.h"

#define RD16( ptr )     ((uint32_t)(ptr)[0] | ((uint32_t)(ptr)[1] << 8))
#define RD32( ptr )     (RD16( ptr ) | (RD16( (ptr) + 2 ) << 16))

#define SIG_LOCAL       0x04034B50UL
#define SIG_CENTRAL     0x02014B50UL
#define SIG_END         0x06054B50UL

#define LOCAL_LEN       30
#define CENTRAL_LEN     46
#define END_LEN         22

typedef struct {
    uint32_t flags, method, crc, csize, usize, local;
    uint32_t name_at, name_len;     /* in the buffer */
} ENTRY;

/* the end-of-central-directory record, searched for from the end over
   the most a comment can be */
static int find_end( const uint8_t *buf, uint32_t len, uint32_t *at )
{
    uint32_t pos, low;

    if ( len < END_LEN ) {
        return 0;
    }
    pos = len - END_LEN;
    low = pos > 65535UL ? pos - 65535UL : 0;
    for ( ;; ) {
        if ( RD32( buf + pos ) == SIG_END && RD16( buf + pos + 20 ) <= len - END_LEN - pos ) {
            *at = pos;
            return 1;
        }
        if ( pos == low ) {
            return 0;
        }
        pos--;
    }
}

/* one central directory entry at *pos, which must end by "end" */
static int central_entry( const uint8_t *buf, uint32_t end, uint32_t *pos, ENTRY *ent )
{
    const uint8_t *rec;
    uint32_t at = *pos, nlen, xlen, clen;

    if ( at > end || end - at < CENTRAL_LEN ) {
        return 0;
    }
    rec = buf + at;
    if ( RD32( rec ) != SIG_CENTRAL ) {
        return 0;
    }
    ent->flags = RD16( rec + 8 );
    ent->method = RD16( rec + 10 );
    ent->crc = RD32( rec + 16 );
    ent->csize = RD32( rec + 20 );
    ent->usize = RD32( rec + 24 );
    nlen = RD16( rec + 28 );
    xlen = RD16( rec + 30 );
    clen = RD16( rec + 32 );
    ent->local = RD32( rec + 42 );
    if ( nlen + xlen + clen > end - at - CENTRAL_LEN ) {   /* three 16-bit sums */
        return 0;
    }
    ent->name_at = at + CENTRAL_LEN;
    ent->name_len = nlen;
    *pos = at + CENTRAL_LEN + nlen + xlen + clen;
    return 1;
}

/* a directory's entry ends in a slash and is skipped */
static int is_dir( const uint8_t *buf, const ENTRY *ent )
{
    return ent->name_len == 0 || buf[ent->name_at + ent->name_len - 1] == '/';
}

static void copy_name( char *name, const uint8_t *buf, const ENTRY *ent )
{
    uint32_t len = ent->name_len;

    if ( len > IMZ_NAME_MAX - 1 ) {
        len = IMZ_NAME_MAX - 1;
    }
    mem_cpy( name, buf + ent->name_at, len );
    name[len] = 0;
}

/*
 * The directory as "end_at" (an offset in the buffer) describes it, the
 * buffer having been read from offset "base" of the file: the first
 * file's entry, and how many more there are.  0 if it does not add up,
 * with *why set.
 */
static int first_file( const uint8_t *buf, uint32_t end_at, uint32_t base, ENTRY *pick, unsigned int *others, int *why )
{
    const uint8_t *rec = buf + end_at;
    uint32_t count, cd_size, cd_off, pos, stop;
    unsigned int i;
    ENTRY ent;
    int have = 0;

    *others = 0;
    *why = IMZ_NOTZIP;
    if ( RD16( rec + 4 ) != 0 || RD16( rec + 6 ) != 0 ) {
        *why = IMZ_UNSUPP;                  /* an archive split over disks */
        return 0;
    }
    count = RD16( rec + 10 );
    cd_size = RD32( rec + 12 );
    cd_off = RD32( rec + 16 );
    if ( count == 0xFFFFUL || cd_off == 0xFFFFFFFFUL ) {
        *why = IMZ_UNSUPP;                  /* zip64 */
        return 0;
    }
    if ( cd_off < base ) {
        return 0;
    }
    cd_off -= base;
    if ( cd_off > end_at || cd_size > end_at - cd_off ) {
        return 0;
    }
    pos = cd_off;
    stop = cd_off + cd_size;
    for ( i = 0; i < count; i++ ) {
        if ( !central_entry( buf, stop, &pos, &ent ) ) {
            return 0;
        }
        if ( is_dir( buf, &ent ) ) {
            continue;
        }
        if ( have ) {
            (*others)++;
        } else {
            *pick = ent;
            have = 1;
        }
    }
    if ( !have ) {
        *why = IMZ_EMPTY;
        return 0;
    }
    *why = IMZ_OK;
    return 1;
}

uint8_t *imz_unpack( const uint8_t *zip, uint32_t len, uint32_t max, uint32_t *img_len,
                     char *name, unsigned int *others, int *why )
{
    ENTRY pick;
    uint32_t end_at, nlen, xlen, data, got;
    uint8_t *img;

    *img_len = 0;
    *others = 0;
    name[0] = 0;
    *why = IMZ_NOTZIP;
    if ( !find_end( zip, len, &end_at ) ) {
        return NULL;
    }
    if ( !first_file( zip, end_at, 0, &pick, others, why ) ) {
        return NULL;
    }
    copy_name( name, zip, &pick );

    if ( (pick.flags & 1) || (pick.method != 0 && pick.method != 8) ||
         pick.csize == 0xFFFFFFFFUL || pick.usize == 0xFFFFFFFFUL ) {
        *why = IMZ_UNSUPP;                  /* encrypted, packed otherwise, zip64 */
        return NULL;
    }
    if ( pick.usize > max ) {
        *why = IMZ_TOOBIG;
        return NULL;
    }

    /* the local header places the data */
    *why = IMZ_NOTZIP;
    if ( pick.local > len || len - pick.local < LOCAL_LEN ||
         RD32( zip + pick.local ) != SIG_LOCAL ) {
        return NULL;
    }
    nlen = RD16( zip + pick.local + 26 );
    xlen = RD16( zip + pick.local + 28 );
    if ( nlen + xlen > len - pick.local - LOCAL_LEN ) {
        return NULL;
    }
    data = pick.local + LOCAL_LEN + nlen + xlen;
    if ( pick.csize > len - data ) {
        return NULL;
    }

    img = (uint8_t *)try_alloc( pick.usize ? pick.usize : 1 );
    if ( img == NULL ) {
        *why = IMZ_NOMEM;
        return NULL;
    }
    if ( pick.method == 0 ) {
        if ( pick.csize != pick.usize ) {
            xfree( img );
            return NULL;
        }
        mem_cpy( img, zip + data, pick.usize );
    } else if ( !inflate_mem( zip + data, pick.csize, img, pick.usize, &got ) ||
                got != pick.usize ) {
        xfree( img );
        return NULL;
    }
    if ( crc32_update( 0, img, pick.usize ) != pick.crc ) {
        xfree( img );
        *why = IMZ_BADCRC;
        return NULL;
    }
    *img_len = pick.usize;
    *why = IMZ_OK;
    return img;
}

int imz_name( const uint8_t *tail, uint32_t len, uint32_t base, char *name )
{
    ENTRY pick;
    uint32_t end_at;
    unsigned int others;
    int why;

    name[0] = 0;
    if ( !find_end( tail, len, &end_at ) ) {
        return 0;
    }
    if ( !first_file( tail, end_at, base, &pick, &others, &why ) ) {
        return 0;
    }
    copy_name( name, tail, &pick );
    return 1;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

static uint8_t *put16( uint8_t *out, uint32_t val )
{
    out[0] = (uint8_t)val;
    out[1] = (uint8_t)(val >> 8);
    return out + 2;
}

static uint8_t *put32( uint8_t *out, uint32_t val )
{
    return put16( put16( out, val ), val >> 16 );
}

/*
 * Local header, data, one central directory entry, the end record.
 * Sizes and CRC are in the local header itself - no data descriptor -
 * and "version made by" is 2.0 on MS-DOS, as PKZIP 2's own were.
 */
uint8_t *imz_pack( const uint8_t *img, uint32_t len, const char *name, uint32_t *zip_len )
{
    uint8_t *packed, *zip, *out;
    const uint8_t *data;
    uint32_t plen, nlen, crc, total, method;
    uint16_t date, time;

    *zip_len = 0;
    nlen = str_len( name );
    if ( nlen > IMZ_NAME_MAX - 1 ) {
        nlen = IMZ_NAME_MAX - 1;
    }
    crc = crc32_update( 0, img, len );

    method = 8;
    packed = deflate_alloc( img, len, &plen, 0 );
    if ( packed == NULL || plen >= len ) {
        xfree( packed );                    /* no smaller, or no memory: */
        packed = NULL;                      /* stored is still a .IMZ */
        method = 0;
        plen = len;
    }
    data = packed != NULL ? packed : img;

    total = LOCAL_LEN + nlen + plen + CENTRAL_LEN + nlen + END_LEN;
    zip = (uint8_t *)try_alloc( total );
    if ( zip == NULL ) {
        xfree( packed );
        return NULL;
    }
    sys_now( &date, &time );

    out = put32( zip, SIG_LOCAL );
    out = put16( out, 20 );                 /* version needed: 2.0 */
    out = put16( out, 0 );                  /* flags */
    out = put16( out, method );
    out = put16( out, time );
    out = put16( out, date );
    out = put32( out, crc );
    out = put32( out, plen );
    out = put32( out, len );
    out = put16( out, nlen );
    out = put16( out, 0 );                  /* no extra field */
    mem_cpy( out, name, nlen );
    out += nlen;
    mem_cpy( out, data, plen );
    out += plen;

    out = put32( out, SIG_CENTRAL );
    out = put16( out, 20 );                 /* made by: 2.0, MS-DOS */
    out = put16( out, 20 );
    out = put16( out, 0 );
    out = put16( out, method );
    out = put16( out, time );
    out = put16( out, date );
    out = put32( out, crc );
    out = put32( out, plen );
    out = put32( out, len );
    out = put16( out, nlen );
    out = put16( out, 0 );                  /* extra */
    out = put16( out, 0 );                  /* comment */
    out = put16( out, 0 );                  /* disk */
    out = put16( out, 0 );                  /* internal attributes */
    out = put32( out, 0x20 );               /* archive */
    out = put32( out, 0 );                  /* the local header's offset */
    mem_cpy( out, name, nlen );
    out += nlen;

    out = put32( out, SIG_END );
    out = put16( out, 0 );
    out = put16( out, 0 );
    out = put16( out, 1 );
    out = put16( out, 1 );
    out = put32( out, CENTRAL_LEN + nlen );
    out = put32( out, LOCAL_LEN + nlen + plen );
    put16( out, 0 );                        /* no comment */

    xfree( packed );
    *zip_len = total;
    return zip;
}
