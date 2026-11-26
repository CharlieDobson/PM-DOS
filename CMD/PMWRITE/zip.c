/*
 * ZIP.C - a ZIP archive read from memory and written to an OUT
 * (ZIP.H).
 *
 * READING starts at the end record, which is looked for from the back
 * of the file through the longest comment an archive can have, and
 * taken only where the directory it points at is really there.  The
 * directory is then walked once to count and once to keep: a name, the
 * sizes, the CRC and where the member's own header is.  NOTHING IN THE
 * FILE IS TRUSTED: every offset and length is checked against the
 * bytes there are before it is followed, a member's packed bytes must
 * lie inside the file, and what it says it unpacks to must be a size
 * its packed bytes could make (DEFLATE cannot do better than 1032 to
 * 1) and under ZIP_MAX_MEMBER - so a file of a hundred bytes cannot
 * ask for 4 GB.  The sizes come from the directory, not the member's
 * header, so archives written with data descriptors read as well.
 *
 * An archive with bytes in front of it (a self-extractor) is read:
 * the directory's own place says how far everything is moved.
 *
 * WRITING keeps a record of each member for the directory.  A member
 * given whole is deflated first and its header written knowing every
 * number; one given a piece at a time has its header written with
 * blanks, which are filled in afterwards by going back.  The date on
 * every member is the moment the archive was begun.
 *
 * NOT DONE: zip64, spanned archives, encryption, and any method but
 * 0 and 8.
 */
#include "fmt.h"
#include "sys.h"
#include "crc.h"
#include "flate.h"
#include "zip.h"

#define SIG_LOCAL       0x04034B50UL
#define SIG_CENTRAL     0x02014B50UL
#define SIG_END         0x06054B50UL
#define LOCAL_LEN       30
#define CENTRAL_LEN     46
#define END_LEN         22
#define COMMENT_MAX     65535UL

#define FLAG_ENCRYPTED  0x0001
#define FLAG_UTF8       0x0800
#define FLAG_MASKED     0x2000  /* the directory itself is encrypted */

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned int name;          /* where in "names" */
    uint32_t     crc, packed, size;
    uint32_t     local;         /* its own header, from the file's start */
    uint16_t     method, flags;
} ZENT;

struct ZIP {
    const uint8_t *data;
    uint32_t     len;
    ZENT        *ent;
    unsigned int count;
    char        *names;
    int          error;
};

/* the end record: where it is, or 0 for none (an archive cannot start
   with one) */
static uint32_t zip_find_end( const uint8_t *data, uint32_t len, uint32_t *bias )
{
    uint32_t pos, low, dir_size, dir_at;

    if ( len < END_LEN ) {
        return 0;
    }
    low = len - END_LEN > COMMENT_MAX ? len - END_LEN - COMMENT_MAX : 0;
    for ( pos = len - END_LEN; ; pos-- ) {
        if ( data[pos] == 0x50 && LE32( data + pos ) == SIG_END &&
             LE16( data + pos + 20 ) <= len - END_LEN - pos ) {
            dir_size = LE32( data + pos + 12 );
            dir_at = LE32( data + pos + 16 );
            /* the directory ends where this record starts, or the
               archive has something in front of it */
            if ( dir_size <= pos && dir_at <= pos - dir_size ) {
                *bias = pos - dir_size - dir_at;
                return pos;
            }
        }
        if ( pos == low ) {
            break;
        }
    }
    return 0;
}

ZIP *zip_open_why( const uint8_t *data, uint32_t len, int *why )
{
    ZIP *zip;
    const uint8_t *rec;
    uint32_t end, bias = 0, dir_at, dir_size, total, pos, stop;
    uint32_t name_len, skip;
    unsigned int count = 0, names = 0, index, used = 0;
    int dummy;

    if ( why == NULL ) {
        why = &dummy;
    }
    *why = ZIP_DAMAGED;
    if ( data == NULL ) {
        return NULL;
    }
    end = zip_find_end( data, len, &bias );
    if ( end == 0 ) {
        return NULL;
    }
    total = LE16( data + end + 10 );
    dir_size = LE32( data + end + 12 );
    dir_at = LE32( data + end + 16 );
    if ( LE16( data + end + 4 ) != 0 || LE16( data + end + 6 ) != 0 ||
         LE16( data + end + 8 ) != total ) {
        *why = ZIP_UNSUPPORTED;         /* one of several disks */
        return NULL;
    }
    if ( total == 0xFFFF || dir_at == 0xFFFFFFFFUL || dir_size == 0xFFFFFFFFUL ) {
        *why = ZIP_UNSUPPORTED;         /* zip64 */
        return NULL;
    }
    /* once through to count: as many records as are whole, never more
       than the end record says */
    pos = dir_at + bias;
    stop = pos + dir_size;
    while ( count < total && stop - pos >= CENTRAL_LEN && LE32( data + pos ) == SIG_CENTRAL ) {
        name_len = LE16( data + pos + 28 );
        skip = name_len + LE16( data + pos + 30 ) + LE16( data + pos + 32 );
        if ( skip > stop - pos - CENTRAL_LEN ) {
            break;
        }
        names += name_len + 1;
        pos += CENTRAL_LEN + skip;
        count++;
    }
    if ( count != total ) {
        return NULL;
    }
    *why = ZIP_NOMEM;
    zip = (ZIP *)try_alloc( sizeof( ZIP ) );
    if ( zip == NULL ) {
        return NULL;
    }
    mem_set( zip, 0, sizeof( ZIP ) );
    zip->data = data;
    zip->len = len;
    zip->count = count;
    zip->ent = (ZENT *)try_alloc( (count + 1) * sizeof( ZENT ) );
    zip->names = (char *)try_alloc( names + 1 );
    if ( zip->ent == NULL || zip->names == NULL ) {
        zip_close( zip );
        return NULL;
    }
    pos = dir_at + bias;
    for ( index = 0; index < count; index++ ) {
        rec = data + pos;
        name_len = LE16( rec + 28 );
        zip->ent[index].flags = (uint16_t)LE16( rec + 8 );
        zip->ent[index].method = (uint16_t)LE16( rec + 10 );
        zip->ent[index].crc = LE32( rec + 16 );
        zip->ent[index].packed = LE32( rec + 20 );
        zip->ent[index].size = LE32( rec + 24 );
        zip->ent[index].local = LE32( rec + 42 );
        zip->ent[index].name = used;
        mem_cpy( zip->names + used, rec + CENTRAL_LEN, name_len );
        /* some writers on Windows put '\' between directories */
        for ( skip = 0; skip < name_len; skip++ ) {
            if ( zip->names[used + skip] == '\\' ) {
                zip->names[used + skip] = '/';
            }
        }
        zip->names[used + name_len] = 0;
        used += name_len + 1;
        pos += CENTRAL_LEN + name_len + LE16( rec + 30 ) + LE16( rec + 32 );
        /* moved along with everything else, if that does not wrap */
        if ( zip->ent[index].local <= 0xFFFFFFFFUL - bias ) {
            zip->ent[index].local += bias;
        } else {
            zip->ent[index].local = 0xFFFFFFFFUL;
        }
    }
    *why = ZIP_OK;
    return zip;
}

void zip_close( ZIP *zip )
{
    if ( zip != NULL ) {
        xfree( zip->ent );
        xfree( zip->names );
        xfree( zip );
    }
}

unsigned int zip_count( const ZIP *zip )
{
    return zip == NULL ? 0 : zip->count;
}

const char *zip_name( const ZIP *zip, unsigned int index )
{
    return zip == NULL || index >= zip->count ? "" : zip->names + zip->ent[index].name;
}

int zip_find( const ZIP *zip, const char *name )
{
    unsigned int index;

    for ( index = 0; zip != NULL && index < zip->count; index++ ) {
        if ( str_cmp( zip->names + zip->ent[index].name, name ) == 0 ) {
            return (int)index;
        }
    }
    return -1;
}

int zip_ifind( const ZIP *zip, const char *name )
{
    unsigned int index;

    for ( index = 0; zip != NULL && index < zip->count; index++ ) {
        if ( str_icmp( zip->names + zip->ent[index].name, name ) == 0 ) {
            return (int)index;
        }
    }
    return -1;
}

uint32_t zip_size( const ZIP *zip, unsigned int index )
{
    return zip == NULL || index >= zip->count ? 0 : zip->ent[index].size;
}

int zip_error( const ZIP *zip )
{
    return zip == NULL ? ZIP_NOMEM : zip->error;
}

uint8_t *zip_read( ZIP *zip, unsigned int index, uint32_t *len )
{
    const ZENT *ent;
    const uint8_t *head;
    uint8_t *block;
    uint32_t at, got = 0;

    *len = 0;
    if ( zip == NULL ) {
        return NULL;
    }
    zip->error = ZIP_DAMAGED;
    if ( index >= zip->count ) {
        return NULL;
    }
    ent = &zip->ent[index];
    if ( (ent->flags & (FLAG_ENCRYPTED | FLAG_MASKED)) || (ent->method != 0 && ent->method != 8) ||
         ent->packed == 0xFFFFFFFFUL || ent->size == 0xFFFFFFFFUL || ent->local == 0xFFFFFFFFUL ) {
        zip->error = ZIP_UNSUPPORTED;
        return NULL;
    }
    if ( !IN_RANGE( ent->local, LOCAL_LEN, zip->len ) ) {
        return NULL;
    }
    head = zip->data + ent->local;
    if ( LE32( head ) != SIG_LOCAL ) {
        return NULL;
    }
    /* the member's bytes follow its own header's name and extra field,
       whose lengths need not be the directory's */
    at = LOCAL_LEN + LE16( head + 26 ) + LE16( head + 28 );
    if ( at > zip->len - ent->local ) {
        return NULL;
    }
    at += ent->local;
    if ( !IN_RANGE( at, ent->packed, zip->len ) ) {
        return NULL;
    }
    if ( ent->size > ZIP_MAX_MEMBER || (ent->method == 0 && ent->size != ent->packed) ||
         (ent->method == 8 && ent->size / 1032 > ent->packed) ) {
        return NULL;
    }
    block = (uint8_t *)try_alloc( ent->size + 1 );
    if ( block == NULL ) {
        zip->error = ZIP_NOMEM;
        return NULL;
    }
    if ( ent->method == 0 ) {
        mem_cpy( block, zip->data + at, ent->size );
        got = ent->size;
    } else if ( ent->size == 0 ) {
        got = 0;                        /* nothing to unpack into */
    } else if ( !inflate_mem( zip->data + at, ent->packed, block, ent->size, &got ) ) {
        /* damaged, or the 4K the decoder needs was not to be had */
        xfree( block );
        return NULL;
    }
    if ( got != ent->size || crc32_update( 0, block, got ) != ent->crc ) {
        xfree( block );
        return NULL;
    }
    block[got] = 0;
    zip->error = ZIP_OK;
    *len = got;
    return block;
}

int zip_peek( ZIP *zip, unsigned int index, uint8_t *buf, unsigned int cap )
{
    const ZENT *ent;
    const uint8_t *head;
    uint32_t at, got;

    mem_set( buf, 0, cap );
    if ( zip == NULL || index >= zip->count || cap == 0 ) {
        return 0;
    }
    ent = &zip->ent[index];
    if ( (ent->flags & (FLAG_ENCRYPTED | FLAG_MASKED)) || (ent->method != 0 && ent->method != 8) ||
         !IN_RANGE( ent->local, LOCAL_LEN, zip->len ) ) {
        return 0;
    }
    head = zip->data + ent->local;
    at = LOCAL_LEN + LE16( head + 26 ) + LE16( head + 28 );
    if ( LE32( head ) != SIG_LOCAL || at > zip->len - ent->local ) {
        return 0;
    }
    at += ent->local;
    if ( !IN_RANGE( at, ent->packed, zip->len ) ) {
        return 0;
    }
    if ( ent->method == 0 ) {
        mem_cpy( buf, zip->data + at, ent->packed < cap ? ent->packed : cap );
    } else {
        /* a member longer than the block stops the decoder when the
           block is full, which is all that is wanted of it */
        inflate_mem( zip->data + at, ent->packed, buf, cap, &got );
    }
    return buf[0] != 0;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned int name;          /* where in "names" */
    uint32_t     name_len;
    uint32_t     crc, packed, size;
    uint32_t     local;         /* from the archive's start */
    uint16_t     method, flags;
} ZWENT;

struct ZIPW {
    OUT         *out;
    uint32_t     base;          /* where in the OUT the archive starts */
    ZWENT       *ent;
    unsigned int count, cap;
    char        *names;
    unsigned int names_len, names_cap;
    uint16_t     date, time;
    int          failed;
    int          open;          /* a member is being written in pieces */
};

ZIPW *zipw_new( OUT *out )
{
    ZIPW *zipw = (ZIPW *)try_alloc( sizeof( ZIPW ) );
    DATETIME now;

    if ( zipw == NULL ) {
        return NULL;
    }
    mem_set( zipw, 0, sizeof( ZIPW ) );
    zipw->out = out;
    zipw->base = out_tell( out );
    sys_now( &now );
    if ( now.year < 1980 || now.year > 2107 ) {
        now.year = 1980;
    }
    zipw->date = (uint16_t)(((now.year - 1980) << 9) | ((now.month & 15) << 5) | (now.day & 31));
    zipw->time = (uint16_t)(((now.hour & 31) << 11) | ((now.minute & 63) << 5) | ((now.second / 2) & 31));
    return zipw;
}

static void zipw_fail( ZIPW *zipw )
{
    zipw->failed = 1;
    out_fail( zipw->out, WE_NOMEM );
}

/* a record for the next member, with its name kept; NULL if it cannot be */
static ZWENT *zipw_entry( ZIPW *zipw, const char *name )
{
    ZWENT *ent;
    char *names;
    unsigned int len = str_len( name ), want, index;

    if ( zipw->failed || zipw->open || zipw->count >= 0xFFFE || len == 0 || len > 0xFFFF ||
         zipw->names_len > 0x3FFFFFFFUL ) {
        zipw_fail( zipw );
        return NULL;
    }
    if ( zipw->count == zipw->cap ) {
        want = zipw->cap ? zipw->cap * 2 : 16;
        ent = (ZWENT *)try_realloc( zipw->ent, want * sizeof( ZWENT ) );
        if ( ent == NULL ) {
            zipw_fail( zipw );
            return NULL;
        }
        zipw->ent = ent;
        zipw->cap = want;
    }
    if ( len > zipw->names_cap - zipw->names_len ) {
        want = zipw->names_cap ? zipw->names_cap * 2 : 512;
        while ( want - zipw->names_len < len ) {
            want *= 2;
        }
        names = (char *)try_realloc( zipw->names, want );
        if ( names == NULL ) {
            zipw_fail( zipw );
            return NULL;
        }
        zipw->names = names;
        zipw->names_cap = want;
    }
    ent = &zipw->ent[zipw->count];
    mem_set( ent, 0, sizeof( ZWENT ) );
    ent->name = zipw->names_len;
    ent->name_len = len;
    mem_cpy( zipw->names + zipw->names_len, name, len );
    zipw->names_len += len;
    for ( index = 0; index < len; index++ ) {
        if ( (uint8_t)name[index] >= 0x80 ) {
            ent->flags = FLAG_UTF8;
        }
    }
    ent->local = out_tell( zipw->out ) - zipw->base;
    return ent;
}

/* a member's own header: the directory's record without its tail */
static void zipw_local( ZIPW *zipw, const ZWENT *ent )
{
    OUT *out = zipw->out;

    out_le32( out, SIG_LOCAL );
    out_le16( out, ent->method ? 20 : 10 );     /* what a reader must know */
    out_le16( out, ent->flags );
    out_le16( out, ent->method );
    out_le16( out, zipw->time );
    out_le16( out, zipw->date );
    out_le32( out, ent->crc );
    out_le32( out, ent->packed );
    out_le32( out, ent->size );
    out_le16( out, ent->name_len );
    out_le16( out, 0 );                         /* no extra field, ever */
    out_put( out, zipw->names + ent->name, ent->name_len );
}

int zipw_add( ZIPW *zipw, const char *name, const uint8_t *data, uint32_t len, int store )
{
    ZWENT *ent;
    uint8_t *packed = NULL;
    uint32_t packed_len = 0;

    if ( zipw == NULL ) {
        return 0;
    }
    ent = zipw_entry( zipw, name );
    if ( ent == NULL ) {
        return 0;
    }
    ent->size = ent->packed = len;
    ent->crc = crc32_update( 0, data, len );
    if ( !store && len > 0 ) {
        /* no memory to deflate with is no reason to fail: it is stored */
        packed = deflate_alloc( data, len, &packed_len, 0 );
        if ( packed != NULL && packed_len < len ) {
            ent->method = 8;
            ent->packed = packed_len;
        }
    }
    zipw_local( zipw, ent );
    if ( ent->method == 8 ) {
        out_put( zipw->out, packed, packed_len );
    } else {
        out_put( zipw->out, data, len );
    }
    xfree( packed );
    zipw->count++;
    return out_error( zipw->out ) == 0;
}

int zipw_begin( ZIPW *zipw, const char *name )
{
    ZWENT *ent;

    if ( zipw == NULL ) {
        return 0;
    }
    ent = zipw_entry( zipw, name );
    if ( ent == NULL ) {
        return 0;
    }
    ent->method = 8;
    zipw_local( zipw, ent );
    zipw->open = 1;
    return out_error( zipw->out ) == 0;
}

static int zipw_piece( ZIPW *zipw, const uint8_t *data, uint32_t len, int last )
{
    ZWENT *ent;
    uint8_t *packed;
    uint32_t packed_len;

    if ( zipw == NULL || !zipw->open || zipw->failed ) {
        return 0;
    }
    ent = &zipw->ent[zipw->count];
    if ( len > 0xFFFFFFFFUL - ent->size ) {
        zipw_fail( zipw );
        return 0;
    }
    packed = deflate_piece( data, len, &packed_len, last );
    if ( packed == NULL || packed_len > 0xFFFFFFFFUL - ent->packed ) {
        xfree( packed );
        zipw_fail( zipw );
        return 0;
    }
    out_put( zipw->out, packed, packed_len );
    xfree( packed );
    ent->crc = crc32_update( ent->crc, data, len );
    ent->size += len;
    ent->packed += packed_len;
    return out_error( zipw->out ) == 0;
}

int zipw_write( ZIPW *zipw, const uint8_t *data, uint32_t len )
{
    return len == 0 ? 1 : zipw_piece( zipw, data, len, 0 );
}

int zipw_finish( ZIPW *zipw )
{
    ZWENT *ent;
    uint32_t end;

    if ( !zipw_piece( zipw, NULL, 0, 1 ) ) {
        return 0;
    }
    /* back to the header for the three numbers nobody knew */
    ent = &zipw->ent[zipw->count];
    end = out_tell( zipw->out );
    out_seek( zipw->out, zipw->base + ent->local + 14 );
    out_le32( zipw->out, ent->crc );
    out_le32( zipw->out, ent->packed );
    out_le32( zipw->out, ent->size );
    out_seek( zipw->out, end );
    zipw->open = 0;
    zipw->count++;
    return out_error( zipw->out ) == 0;
}

int zipw_end( ZIPW *zipw )
{
    OUT *out;
    const ZWENT *ent;
    uint32_t dir_at, dir_end;
    unsigned int index;
    int ok;

    if ( zipw == NULL ) {
        return 0;
    }
    out = zipw->out;
    if ( zipw->open ) {
        zipw_fail( zipw );              /* a member that was never finished */
    }
    dir_at = out_tell( out ) - zipw->base;
    for ( index = 0; index < zipw->count && !zipw->failed; index++ ) {
        ent = &zipw->ent[index];
        out_le32( out, SIG_CENTRAL );
        out_le16( out, 20 );            /* written by: 2.0, on DOS */
        out_le16( out, ent->method ? 20 : 10 );
        out_le16( out, ent->flags );
        out_le16( out, ent->method );
        out_le16( out, zipw->time );
        out_le16( out, zipw->date );
        out_le32( out, ent->crc );
        out_le32( out, ent->packed );
        out_le32( out, ent->size );
        out_le16( out, ent->name_len );
        out_le16( out, 0 );             /* extra field */
        out_le16( out, 0 );             /* comment */
        out_le16( out, 0 );             /* disk */
        out_le16( out, 0 );             /* attributes: its own, the file's */
        out_le32( out, 0 );
        out_le32( out, ent->local );
        out_put( out, zipw->names + ent->name, ent->name_len );
    }
    dir_end = out_tell( out ) - zipw->base;
    out_le32( out, SIG_END );
    out_le16( out, 0 );
    out_le16( out, 0 );
    out_le16( out, zipw->count );
    out_le16( out, zipw->count );
    out_le32( out, dir_end - dir_at );
    out_le32( out, dir_at );
    out_le16( out, 0 );
    ok = !zipw->failed && out_error( out ) == 0;
    xfree( zipw->ent );
    xfree( zipw->names );
    xfree( zipw );
    return ok;
}
