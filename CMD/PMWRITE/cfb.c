/*
 * CFB.C - the OLE2 compound file, read and written (CFB.H).
 *
 * THE FILE is a header and then sectors of 512 bytes (4096 in version
 * 4).  A stream is a chain of sectors, and the FAT says which sector
 * follows which; the FAT is itself in sectors, which the header's
 * DIFAT lists (the first 109 in the header, the rest in a chain of
 * their own).  The directory is a stream of 128-byte entries: entry 0
 * is the root, and the entries of one storage are a binary tree hung
 * from the storage's "child".  A stream shorter than 4096 bytes is not
 * in sectors of its own: it is in 64-byte pieces of the "mini stream",
 * which is the root entry's stream, chained by a mini-FAT.
 *
 * READING.  Every link is checked as it is followed, and no walk goes
 * on for more steps than the file has sectors, so a chain that loops
 * or runs off the end stops the read.  The FAT is not copied: only the
 * list of where its sectors are.  The directory's tree is walked once,
 * when the file is opened, with a stack on the heap and each entry
 * taken once; what is kept is each entry's parent, and a name is then
 * found by looking along the entries - so a tree that is damaged into
 * a loop costs nothing.
 *
 * WRITING.  Version 3, 512-byte sectors, in the order FAT, DIFAT,
 * directory, mini-FAT, mini stream, the long streams.  The names are
 * put in the order the format wants - shorter first, then by their
 * capitals - and the tree is made by splitting that list at its middle
 * again and again.  Such a tree has every level full but the last, so
 * colouring the last level red and the rest black makes it the
 * red-black tree the format asks for.
 */
#include "cfb.h"

#define FREESECT        0xFFFFFFFFUL
#define ENDOFCHAIN      0xFFFFFFFEUL
#define FATSECT         0xFFFFFFFDUL
#define DIFSECT         0xFFFFFFFCUL
#define NOSTREAM        0xFFFFFFFFUL
#define NOT_KNOWN       0xFFFFFFFFUL    /* chain_read: as long as the chain is */

#define HEADER_LEN      512
#define DIR_ENTRY       128
#define MINI_SIZE       64
#define MINI_CUTOFF     4096
#define HEADER_DIFAT    109

#define TYPE_STORAGE    1
#define TYPE_STREAM     2
#define TYPE_ROOT       5

static const u8 cfb_sig[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };

struct CFB {
    const u8 *data;
    u32   len;
    u32   shift;                /* 9 or 12 */
    u32   ssz;                  /* a sector's bytes */
    u32   nsect;                /* sectors after the header; the last may be short */
    u32  *fatsect;              /* where each sector of the FAT is */
    u32   nfat;
    u8   *dir;                  /* the directory */
    u32   ndir;                 /* ...and how many entries */
    s32  *parent;               /* the storage each entry is in, -1 for none */
    u8   *minifat;
    u32   nminifat;             /* entries */
    u8   *mini;                 /* the mini stream */
    u32   mini_len;
    u32   cutoff;
};

int cfb_is( const u8 *data, u32 len )
{
    return len >= HEADER_LEN && mem_cmp( data, cfb_sig, sizeof( cfb_sig ) ) == 0;
}

/* the sector that follows "sect", or ENDOFCHAIN when the FAT has
   nothing to say about it */
static u32 fat_next( const CFB *cfb, u32 sect )
{
    u32 per = cfb->ssz / 4, which = sect / per, where, pos;

    if ( which >= cfb->nfat ) {
        return ENDOFCHAIN;
    }
    where = cfb->fatsect[which];
    if ( where >= cfb->nsect ) {
        return ENDOFCHAIN;
    }
    pos = ((where + 1) << cfb->shift) + (sect % per) * 4;
    if ( !IN_RANGE( pos, 4, cfb->len ) ) {
        return ENDOFCHAIN;
    }
    return LE32( cfb->data + pos );
}

/* a sector's bytes into "dst": "want" of them, zeroes for what a file
   cut short has not got */
static void sect_copy( const CFB *cfb, u32 sect, u8 *dst, u32 want )
{
    u32 pos = (sect + 1) << cfb->shift, have;

    have = pos < cfb->len ? cfb->len - pos : 0;
    if ( have > want ) {
        have = want;
    }
    mem_cpy( dst, cfb->data + pos, have );
    mem_set( dst + have, 0, want - have );
}

/* a chain of sectors as one heap block: "size" bytes of it, or all of
   it when "size" is NOT_KNOWN.  NULL on damage or no memory. */
static u8 *chain_read( const CFB *cfb, u32 start, u32 size, u32 *got )
{
    u32 sect, count, index, left, part;
    u8 *block;

    if ( size == NOT_KNOWN ) {
        count = 0;
        for ( sect = start; sect < cfb->nsect; sect = fat_next( cfb, sect ) ) {
            if ( ++count > cfb->nsect ) {
                return NULL;            /* it goes round */
            }
        }
        size = count << cfb->shift;
    } else {
        if ( size > (cfb->nsect << cfb->shift) ) {
            return NULL;
        }
        count = (size + cfb->ssz - 1) >> cfb->shift;
    }
    block = (u8 *)try_alloc( size ? size : 1 );
    if ( block == NULL ) {
        return NULL;
    }
    sect = start;
    left = size;
    for ( index = 0; index < count; index++ ) {
        if ( sect >= cfb->nsect ) {
            xfree( block );
            return NULL;
        }
        part = left < cfb->ssz ? left : cfb->ssz;
        sect_copy( cfb, sect, block + (size - left), part );
        left -= part;
        sect = fat_next( cfb, sect );
    }
    *got = size;
    return block;
}

/* where the FAT's sectors are: 109 in the header, the rest in the
   DIFAT's own chain, each of whose sectors ends with the next one */
static int fat_list( CFB *cfb, u32 nfat, u32 difat_first )
{
    u32 per = cfb->ssz / 4 - 1, have = 0, sect, steps = 0, index, pos;

    if ( nfat > cfb->nsect ) {
        return 0;
    }
    cfb->fatsect = (u32 *)try_alloc( (nfat ? nfat : 1) * sizeof( u32 ) );
    if ( cfb->fatsect == NULL ) {
        return 0;
    }
    while ( have < nfat && have < HEADER_DIFAT ) {
        cfb->fatsect[have] = LE32( cfb->data + 0x4C + have * 4 );
        have++;
    }
    sect = difat_first;
    while ( have < nfat && sect < cfb->nsect && steps++ < cfb->nsect ) {
        pos = (sect + 1) << cfb->shift;
        if ( !IN_RANGE( pos, cfb->ssz, cfb->len ) ) {
            break;
        }
        for ( index = 0; index < per && have < nfat; index++ ) {
            cfb->fatsect[have++] = LE32( cfb->data + pos + index * 4 );
        }
        sect = LE32( cfb->data + pos + per * 4 );
    }
    /* what a damaged DIFAT did not give is "nowhere" */
    while ( have < nfat ) {
        cfb->fatsect[have++] = FREESECT;
    }
    cfb->nfat = nfat;
    return 1;
}

static const u8 *dir_entry( const CFB *cfb, u32 entry )
{
    return cfb->dir + entry * DIR_ENTRY;
}

static int dir_type( const CFB *cfb, u32 entry )
{
    return dir_entry( cfb, entry )[0x42];
}

/* the storage each entry is in.  An entry is taken once however many
   others point at it, and each one taken puts three more at most on
   the stack, so the stack cannot run over. */
static int dir_parents( CFB *cfb )
{
    u32 *stack;
    u32 top = 0, entry, from, index, link;

    cfb->parent = (s32 *)try_alloc( cfb->ndir * sizeof( s32 ) );
    stack = (u32 *)try_alloc( (cfb->ndir * 3 + 1) * 2 * sizeof( u32 ) );
    if ( cfb->parent == NULL || stack == NULL ) {
        xfree( stack );
        return 0;
    }
    for ( index = 0; index < cfb->ndir; index++ ) {
        cfb->parent[index] = -1;
    }
    cfb->parent[0] = 0;
    link = LE32( dir_entry( cfb, 0 ) + 0x4C );
    if ( link < cfb->ndir ) {
        stack[top * 2] = link;
        stack[top * 2 + 1] = 0;
        top++;
    }
    while ( top ) {
        top--;
        entry = stack[top * 2];
        from = stack[top * 2 + 1];
        if ( entry == 0 || cfb->parent[entry] >= 0 ) {
            continue;
        }
        cfb->parent[entry] = (s32)from;
        for ( index = 0; index < 3; index++ ) {
            /* its two neighbours in the same storage, and for a storage
               the first of what is in it */
            link = LE32( dir_entry( cfb, entry ) + 0x44 + index * 4 );
            if ( link >= cfb->ndir || (index == 2 && dir_type( cfb, entry ) != TYPE_STORAGE) ) {
                continue;
            }
            stack[top * 2] = link;
            stack[top * 2 + 1] = index == 2 ? entry : from;
            top++;
        }
    }
    xfree( stack );
    return 1;
}

CFB *cfb_open( const u8 *data, u32 len )
{
    CFB *cfb;
    u32 shift, got, size;

    if ( !cfb_is( data, len ) || len > 0x7FFFFFFFUL ) {
        return NULL;
    }
    shift = LE16( data + 0x1E );
    if ( (shift != 9 && shift != 12) || LE16( data + 0x20 ) != 6 ) {
        return NULL;
    }
    cfb = (CFB *)try_alloc( sizeof( CFB ) );
    if ( cfb == NULL ) {
        return NULL;
    }
    mem_set( cfb, 0, sizeof( CFB ) );
    cfb->data = data;
    cfb->len = len;
    cfb->shift = shift;
    cfb->ssz = 1UL << shift;
    cfb->nsect = len > cfb->ssz ? (len - 1) >> shift : 0;
    cfb->cutoff = LE32( data + 0x38 );
    if ( cfb->cutoff > MINI_CUTOFF ) {
        cfb->cutoff = MINI_CUTOFF;
    }
    if ( !fat_list( cfb, LE32( data + 0x2C ), LE32( data + 0x44 ) ) ) {
        cfb_close( cfb );
        return NULL;
    }
    cfb->dir = chain_read( cfb, LE32( data + 0x30 ), NOT_KNOWN, &got );
    if ( cfb->dir == NULL || got < DIR_ENTRY || dir_type( cfb, 0 ) != TYPE_ROOT ) {
        cfb_close( cfb );
        return NULL;
    }
    cfb->ndir = got / DIR_ENTRY;
    if ( !dir_parents( cfb ) ) {
        cfb_close( cfb );
        return NULL;
    }
    /* the mini stream and its FAT: a file that has lost them still has
       its long streams */
    cfb->minifat = chain_read( cfb, LE32( data + 0x3C ), NOT_KNOWN, &got );
    if ( cfb->minifat != NULL ) {
        cfb->nminifat = got / 4;
    }
    size = LE32( dir_entry( cfb, 0 ) + 0x78 );
    if ( size != 0 && cfb->nminifat != 0 ) {
        cfb->mini = chain_read( cfb, LE32( dir_entry( cfb, 0 ) + 0x74 ), size, &got );
        if ( cfb->mini != NULL ) {
            cfb->mini_len = got;
        }
    }
    return cfb;
}

void cfb_close( CFB *cfb )
{
    if ( cfb == NULL ) {
        return;
    }
    xfree( cfb->fatsect );
    xfree( cfb->dir );
    xfree( cfb->parent );
    xfree( cfb->minifat );
    xfree( cfb->mini );
    xfree( cfb );
}

/* 1 if an entry's name is the "len" characters at "name" */
static int name_is( const CFB *cfb, u32 entry, const char *name, u32 len )
{
    const u8 *raw = dir_entry( cfb, entry );
    u32 bytes = LE16( raw + 0x40 ), index;

    if ( bytes < 2 || bytes > 64 || bytes / 2 - 1 != len ) {
        return 0;
    }
    for ( index = 0; index < len; index++ ) {
        if ( raw[index * 2 + 1] != 0 || ch_upper( raw[index * 2] ) != ch_upper( (u8)name[index] ) ) {
            return 0;
        }
    }
    return 1;
}

int cfb_find( CFB *cfb, const char *name )
{
    u32 within = 0, entry, len;

    if ( cfb == NULL || name == NULL ) {
        return -1;
    }
    while ( *name ) {
        for ( len = 0; name[len] && name[len] != '/'; len++ ) {
        }
        for ( entry = 1; entry < cfb->ndir; entry++ ) {
            if ( cfb->parent[entry] == (s32)within && name_is( cfb, entry, name, len ) ) {
                break;
            }
        }
        if ( entry >= cfb->ndir ) {
            return -1;
        }
        within = entry;
        name += len;
        if ( *name == '/' ) {
            name++;
        }
    }
    return within ? (int)within : -1;
}

u32 cfb_size( CFB *cfb, int entry )
{
    if ( cfb == NULL || entry <= 0 || (u32)entry >= cfb->ndir ||
         dir_type( cfb, (u32)entry ) != TYPE_STREAM ) {
        return 0;
    }
    /* a version 3 file's high half is not to be believed, and a stream
       of more than 4G is not one this reads */
    return LE32( dir_entry( cfb, (u32)entry ) + 0x78 );
}

u8 *cfb_read( CFB *cfb, int entry, u32 *len )
{
    const u8 *raw;
    u8 *block;
    u32 size, start, sect, left, part, total;

    if ( cfb == NULL || entry <= 0 || (u32)entry >= cfb->ndir ||
         dir_type( cfb, (u32)entry ) != TYPE_STREAM ) {
        return NULL;
    }
    raw = dir_entry( cfb, (u32)entry );
    start = LE32( raw + 0x74 );
    size = LE32( raw + 0x78 );
    if ( size >= cfb->cutoff ) {
        return chain_read( cfb, start, size, len );
    }
    /* a short one: 64-byte pieces of the mini stream */
    total = cfb->mini_len / MINI_SIZE;
    if ( size > cfb->mini_len ) {
        return NULL;
    }
    block = (u8 *)try_alloc( size ? size : 1 );
    if ( block == NULL ) {
        return NULL;
    }
    sect = start;
    for ( left = size; left; left -= part ) {
        if ( sect >= total || sect >= cfb->nminifat ) {
            xfree( block );
            return NULL;
        }
        part = left < MINI_SIZE ? left : MINI_SIZE;
        mem_cpy( block + (size - left), cfb->mini + sect * MINI_SIZE, part );
        sect = LE32( cfb->minifat + sect * 4 );
    }
    *len = size;
    return block;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

#define CFBW_MAX        16
#define CFBW_NAME       32

typedef struct {
    char  name[CFBW_NAME];
    const u8 *data;
    u32   len;
    u32   start;                /* its first sector, or mini sector */
    u32   left, right;          /* directory entries, or NOSTREAM */
    int   depth;
} CFBW_ITEM;

struct CFBW {
    CFBW_ITEM item[CFBW_MAX];
    int   count;
    int   deepest;
    u8    clsid[16];
};

CFBW *cfbw_new( void )
{
    CFBW *w = (CFBW *)try_alloc( sizeof( CFBW ) );

    if ( w != NULL ) {
        mem_set( w, 0, sizeof( CFBW ) );
    }
    return w;
}

void cfbw_free( CFBW *w )
{
    xfree( w );
}

void cfbw_clsid( CFBW *w, const u8 *clsid )
{
    if ( w != NULL ) {
        mem_cpy( w->clsid, clsid, 16 );
    }
}

/* the order the directory's tree is in: the shorter name first, and
   of two as long the one whose capitals come first */
static int name_order( const char *lhs, const char *rhs )
{
    u32 lhs_len = str_len( lhs ), rhs_len = str_len( rhs ), index;

    if ( lhs_len != rhs_len ) {
        return lhs_len < rhs_len ? -1 : 1;
    }
    for ( index = 0; index < lhs_len; index++ ) {
        int one = ch_upper( (u8)lhs[index] ), two = ch_upper( (u8)rhs[index] );

        if ( one != two ) {
            return one < two ? -1 : 1;
        }
    }
    return 0;
}

int cfbw_add( CFBW *w, const char *name, const u8 *data, u32 len )
{
    CFBW_ITEM hold;
    int index;

    if ( w == NULL || w->count >= CFBW_MAX || str_len( name ) >= CFBW_NAME || name[0] == 0 ) {
        return 0;
    }
    for ( index = 0; index < w->count; index++ ) {
        if ( name_order( w->item[index].name, name ) == 0 ) {
            return 0;                   /* a storage has one of each name */
        }
    }
    mem_set( &hold, 0, sizeof( hold ) );
    str_cpy( hold.name, name );
    hold.data = data;
    hold.len = len;
    /* kept in order as they come */
    for ( index = w->count; index > 0 && name_order( w->item[index - 1].name, name ) > 0; index-- ) {
        w->item[index] = w->item[index - 1];
    }
    w->item[index] = hold;
    w->count++;
    return 1;
}

/* the tree over items low .. high-1: its top's directory entry.  No
   deeper than the list is long, which is 16. */
static u32 tree_build( CFBW *w, int low, int high, int depth )
{
    int mid;

    if ( low >= high || depth > CFBW_MAX ) {
        return NOSTREAM;
    }
    mid = low + (high - low) / 2;
    w->item[mid].depth = depth;
    if ( depth > w->deepest ) {
        w->deepest = depth;
    }
    w->item[mid].left = tree_build( w, low, mid, depth + 1 );
    w->item[mid].right = tree_build( w, mid + 1, high, depth + 1 );
    return (u32)mid + 1;
}

/* count links of a chain that starts at "first" */
static void put_chain( OUT *out, u32 first, u32 count )
{
    u32 index;

    for ( index = 0; index < count; index++ ) {
        out_le32( out, index + 1 < count ? first + index + 1 : ENDOFCHAIN );
    }
}

static void pad_to( OUT *out, u32 unit, int value )
{
    u32 over = out_tell( out ) % unit;

    if ( over ) {
        out_fill( out, value, unit - over );
    }
}

static void put_dir_entry( OUT *out, const char *name, int type, int red, u32 left, u32 right,
                           u32 child, const u8 *clsid, u32 start, u32 size )
{
    u32 len = str_len( name ), index;

    for ( index = 0; index < 32; index++ ) {
        out_le16( out, index < len ? (u8)name[index] : 0 );
    }
    out_le16( out, (len + 1) * 2 );
    out_byte( out, type );
    out_byte( out, red ? 0 : 1 );
    out_le32( out, left );
    out_le32( out, right );
    out_le32( out, child );
    if ( clsid != NULL ) {
        out_put( out, clsid, 16 );
    } else {
        out_fill( out, 0, 16 );
    }
    out_fill( out, 0, 4 + 8 + 8 );      /* state bits and the two times */
    out_le32( out, start );
    out_le32( out, size );
    out_le32( out, 0 );
}

int cfbw_write( CFBW *w, OUT *out )
{
    u32 ndir, nmini = 0, nminifat, nms, nbig = 0, ndata, nfat = 0, ndifat = 0, next_fat;
    u32 total = 0, sect, mini_at = 0, index, top, filled, in_tree = 0;
    u32 dir_at, minifat_at, ms_at, big_at;
    int item, perfect;

    if ( w == NULL || out == NULL ) {
        return 0;
    }
    for ( item = 0; item < w->count; item++ ) {
        if ( w->item[item].len > 0x7FFF0000UL - total ) {
            return 0;                   /* a version 3 file stops at 2G */
        }
        total += w->item[item].len;
        if ( w->item[item].len < MINI_CUTOFF ) {
            nmini += (w->item[item].len + MINI_SIZE - 1) / MINI_SIZE;
        } else {
            nbig += (w->item[item].len + HEADER_LEN - 1) / HEADER_LEN;
        }
    }
    ndir = ((u32)w->count + 1 + 3) / 4;
    nminifat = (nmini + 127) / 128;
    nms = (nmini + 7) / 8;
    ndata = ndir + nminifat + nms + nbig;
    /* the FAT has to have room for its own sectors too */
    for ( ;; ) {
        next_fat = (ndata + nfat + ndifat + 127) / 128;
        if ( next_fat == nfat ) {
            break;
        }
        nfat = next_fat;
        ndifat = nfat > HEADER_DIFAT ? (nfat - HEADER_DIFAT + 126) / 127 : 0;
    }
    dir_at = nfat + ndifat;
    minifat_at = dir_at + ndir;
    ms_at = minifat_at + nminifat;
    big_at = ms_at + nms;

    /* where each stream starts */
    sect = big_at;
    for ( item = 0; item < w->count; item++ ) {
        if ( w->item[item].len == 0 ) {
            w->item[item].start = ENDOFCHAIN;
        } else if ( w->item[item].len < MINI_CUTOFF ) {
            w->item[item].start = mini_at;
            mini_at += (w->item[item].len + MINI_SIZE - 1) / MINI_SIZE;
        } else {
            w->item[item].start = sect;
            sect += (w->item[item].len + HEADER_LEN - 1) / HEADER_LEN;
        }
    }
    w->deepest = 0;
    top = tree_build( w, 0, w->count, 0 );
    for ( index = 1; index <= (u32)w->deepest + 1; index++ ) {
        in_tree = in_tree * 2 + 1;
    }
    perfect = in_tree == (u32)w->count;

    /* the header */
    out_put( out, cfb_sig, sizeof( cfb_sig ) );
    out_fill( out, 0, 16 );
    out_le16( out, 0x003E );
    out_le16( out, 3 );
    out_le16( out, 0xFFFE );
    out_le16( out, 9 );
    out_le16( out, 6 );
    out_fill( out, 0, 6 );
    out_le32( out, 0 );
    out_le32( out, nfat );
    out_le32( out, dir_at );
    out_le32( out, 0 );
    out_le32( out, MINI_CUTOFF );
    out_le32( out, nminifat ? minifat_at : ENDOFCHAIN );
    out_le32( out, nminifat );
    out_le32( out, ndifat ? nfat : ENDOFCHAIN );
    out_le32( out, ndifat );
    for ( index = 0; index < HEADER_DIFAT; index++ ) {
        out_le32( out, index < nfat ? index : FREESECT );
    }

    /* the FAT */
    for ( index = 0; index < nfat; index++ ) {
        out_le32( out, FATSECT );
    }
    for ( index = 0; index < ndifat; index++ ) {
        out_le32( out, DIFSECT );
    }
    put_chain( out, dir_at, ndir );
    put_chain( out, minifat_at, nminifat );
    put_chain( out, ms_at, nms );
    sect = big_at;
    for ( item = 0; item < w->count; item++ ) {
        if ( w->item[item].len >= MINI_CUTOFF ) {
            index = (w->item[item].len + HEADER_LEN - 1) / HEADER_LEN;
            put_chain( out, sect, index );
            sect += index;
        }
    }
    pad_to( out, HEADER_LEN, 0xFF );

    /* the DIFAT's own sectors: 127 FAT sectors each, and the next one */
    filled = HEADER_DIFAT;
    for ( index = 0; index < ndifat; index++ ) {
        u32 slot;

        for ( slot = 0; slot < 127; slot++ ) {
            out_le32( out, filled < nfat ? filled : FREESECT );
            filled++;
        }
        out_le32( out, index + 1 < ndifat ? nfat + index + 1 : ENDOFCHAIN );
    }

    /* the directory */
    put_dir_entry( out, "Root Entry", TYPE_ROOT, 0, NOSTREAM, NOSTREAM, top, w->clsid,
                   nms ? ms_at : ENDOFCHAIN, nmini * MINI_SIZE );
    for ( item = 0; item < w->count; item++ ) {
        put_dir_entry( out, w->item[item].name, TYPE_STREAM,
                       !perfect && w->item[item].depth == w->deepest && w->deepest > 0,
                       w->item[item].left, w->item[item].right, NOSTREAM, NULL,
                       w->item[item].start, w->item[item].len );
    }
    for ( index = (u32)w->count + 1; index < ndir * 4; index++ ) {
        out_fill( out, 0, 0x44 );
        out_le32( out, NOSTREAM );
        out_le32( out, NOSTREAM );
        out_le32( out, NOSTREAM );
        out_fill( out, 0, DIR_ENTRY - 0x50 );
    }

    /* the mini-FAT, and the short streams it chains */
    for ( item = 0; item < w->count; item++ ) {
        if ( w->item[item].len != 0 && w->item[item].len < MINI_CUTOFF ) {
            put_chain( out, w->item[item].start, (w->item[item].len + MINI_SIZE - 1) / MINI_SIZE );
        }
    }
    pad_to( out, HEADER_LEN, 0xFF );
    for ( item = 0; item < w->count; item++ ) {
        if ( w->item[item].len != 0 && w->item[item].len < MINI_CUTOFF ) {
            out_put( out, w->item[item].data, w->item[item].len );
            pad_to( out, MINI_SIZE, 0 );
        }
    }
    pad_to( out, HEADER_LEN, 0 );

    /* the long ones */
    for ( item = 0; item < w->count; item++ ) {
        if ( w->item[item].len >= MINI_CUTOFF ) {
            out_put( out, w->item[item].data, w->item[item].len );
            pad_to( out, HEADER_LEN, 0 );
        }
    }
    return out_error( out ) ? 0 : 1;
}
