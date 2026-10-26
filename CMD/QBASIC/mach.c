/*
 * MACH.C - the machine a QBasic program thinks it is on: DEF SEG, PEEK,
 * POKE, VARPTR, VARSEG, SADD, VARPTR$, BLOAD, BSAVE, INP, OUT, WAIT, and
 * CALL ABSOLUTE.
 *
 * THERE IS NO REAL MODE HERE, so the first megabyte a program PEEKs and
 * POKEs is put together for it:
 *
 *   its own variables   VARSEG and VARPTR hand out segments for the
 *                       module's data (one "DGROUP", the default DEF
 *                       SEG), the frames, the statics, each array and
 *                       each string asked about; PEEK and POKE there
 *                       reach the real bytes, and no further
 *   B800 and A000       the output screen: text pages, SCREEN 13's
 *                       picture (a byte a pixel), SCREEN 1 and 2's CGA
 *                       layout
 *   0040 and the ROMs   read from the machine (21h/F1h AL=20h); a POKE
 *                       to the keyboard buffer's head or tail empties
 *                       the program's key queue, which is what programs
 *                       that do it mean
 *
 * Anything else reads as 0 and takes nothing.
 *
 * PORTS go to the hardware - the kernel emulates a native program's IN
 * and OUT - except the keyboard controller's and A20's (60h, 64h, 92h),
 * which INP answers from the key queue and OUT ignores.
 *
 * CALL ABSOLUTE would run real-mode machine code: "Advanced feature
 * unavailable".
 */
#include "qb.h"

#define DGROUP      0x1000
#define SEG_FIRST   0x2000
#define SEG_LAST    0x9000

typedef struct {
    u32  lin;                   /* where it starts in the first megabyte */
    u8  *base;
    u32  size;
    STR **slot;                 /* SADD's: the variable, which must still hold
                                   the string at base (else it maps nothing) */
} REGION;

#define MAX_REGIONS 128

static REGION regions[MAX_REGIONS];
static int    nregions;
static u16    next_seg = SEG_FIRST;
static u32    next_sadd;          /* SADD's strings go in DGROUP's top half */
static u16    def_seg = DGROUP;

/* the BIOS keyboard ring as a program sees it: 40:1A head, 40:1C tail,
   40:1E-3D the keys, 40:80 and 40:82 its start and end - a program may
   type keys into it, and con_poll_keys takes them */
static u8     kb_area[0x100];

/* VARPTR$'s handles: the variables named so far */
#define MAX_VPS     256
static struct {
    void *addr;
    QTYPE type;
} vps[MAX_VPS];
static int nvps;

/* string variables VARPTR has been asked about: PEEK there reads a
   descriptor as QBasic lays it out, the length then SADD's offset */
#define MAX_DESCS   32
static struct {
    STR **slot;
    u32   lin;
} descs[MAX_DESCS];
static int ndescs;

int  gfx_width_px( void );
u8  *gfx_page( int page );
int  gfx_active_page( void );
u16 *con_page_cells( int page );
void con_page_changed( int page, u32 first, u32 count );
int  con_cols( void );
int  con_rows( void );
int  con_last_scan( void );
void con_flush_keys( void );
int  con_bda_byte( u32 off );

void mem_reset_segs( void )
{
    mem_set( kb_area, 0, sizeof( kb_area ) );
    kb_area[0x1A] = kb_area[0x1C] = kb_area[0x80] = 0x1E;
    kb_area[0x82] = 0x3E;
    nregions = 0;
    next_seg = SEG_FIRST;
    next_sadd = 0;
    def_seg = DGROUP;
    nvps = 0;
    ndescs = 0;
}

static void region_drop( int i )
{
    mem_cpy( &regions[i], &regions[i + 1], (u32)(nregions - i - 1) * sizeof( REGION ) );
    nregions--;
}

/* the region (registering it if new) for a block of memory: at "lin", or
   (0) in segments of its own; SADD's strings give their variable too */
static REGION *region_for( u8 *base, u32 size, u32 lin, STR **slot )
{
    REGION *r;
    int i;
    u32 paras;

    for ( i = 0; i < nregions; i++ ) {
        if ( regions[i].base == base && regions[i].slot == slot ) {
            if ( size > regions[i].size ) {
                regions[i].size = size;
            }
            return &regions[i];
        }
    }
    if ( lin == 0 ) {
        paras = (size + 15) / 16;
        if ( (u32)next_seg + paras > SEG_LAST ) {
            next_seg = SEG_FIRST;
        }
        lin = (u32)next_seg * 16;
        next_seg = (u16)(next_seg + paras + 1);
    }
    /* an older block given these addresses before goes: the newest is
       what they mean now (DGROUP, the module's data, stays) */
    for ( i = 0; i < nregions; ) {
        REGION *old = &regions[i];
        if ( old->lin < lin + size && lin < old->lin + old->size
          && !(old->lin == (u32)DGROUP * 16 && old->slot == NULL) ) {
            region_drop( i );
        } else {
            i++;
        }
    }
    if ( nregions == MAX_REGIONS ) {
        region_drop( 0 );                       /* the oldest goes */
    }
    r = &regions[nregions++];
    r->base = base;
    r->size = size;
    r->lin = lin;
    r->slot = slot;
    return r;
}

/* the region a byte of the first megabyte is in, the newest first; a
   SADD string's only while its variable still holds it */
static REGION *region_at( u32 lin )
{
    int i;

    for ( i = nregions - 1; i >= 0; i-- ) {
        REGION *r = &regions[i];
        if ( lin < r->lin || lin >= r->lin + r->size ) {
            continue;
        }
        if ( r->slot != NULL && (*r->slot == NULL || (u8 *)(*r->slot)->text != r->base
                                 || lin - r->lin >= (*r->slot)->len) ) {
            continue;
        }
        return r;
    }
    return NULL;
}

/* memory that is going (a frame, an array, the variables at CLEAR): what
   VARPTR, SADD and VARPTR$ said about it is forgotten */
void mem_forget( const u8 *lo, const u8 *hi )
{
    int i;

    if ( nregions == 0 && ndescs == 0 && nvps == 0 ) {
        return;                                 /* every procedure return comes here */
    }
    for ( i = 0; i < nregions; ) {
        if ( (regions[i].base >= lo && regions[i].base < hi)
          || ((const u8 *)regions[i].slot >= lo && (const u8 *)regions[i].slot < hi) ) {
            region_drop( i );
        } else {
            i++;
        }
    }
    for ( i = 0; i < ndescs; ) {
        if ( (const u8 *)descs[i].slot >= lo && (const u8 *)descs[i].slot < hi ) {
            descs[i] = descs[--ndescs];
        } else {
            i++;
        }
    }
    for ( i = 0; i < nvps; i++ ) {
        if ( (const u8 *)vps[i].addr >= lo && (const u8 *)vps[i].addr < hi ) {
            vps[i].addr = NULL;                 /* its handle stays taken, and is refused */
        }
    }
}

/* where a variable is, as segment and offset */
static void address_of( u8 *addr, u16 *seg, u32 *off )
{
    PROGRAM *prog = vm.prog;
    REGION *r = NULL;
    int i;

    if ( addr >= vm.gdata && addr < vm.gdata + prog->gsize && prog->gsize <= 0xFFF0UL ) {
        r = region_for( vm.gdata, prog->gsize, (u32)DGROUP * 16, NULL );
    } else if ( addr >= vm.farena && addr < vm.farena + vm.fcap ) {
        r = region_for( vm.farena, vm.fcap, 0, NULL );
    } else {
        for ( i = 0; i < prog->nprocs && r == NULL; i++ ) {
            PROC *proc = &prog->procs[i];
            if ( proc->static_data && addr >= proc->static_data && addr < proc->static_data + proc->static_size ) {
                r = region_for( proc->static_data, proc->static_size, 0, NULL );
            }
        }
    }
    if ( r == NULL ) {
        /* an array's element, or anything else: a block of the heap */
        for ( i = nregions - 1; i >= 0; i-- ) {
            if ( regions[i].slot == NULL && addr >= regions[i].base && addr < regions[i].base + regions[i].size ) {
                r = &regions[i];
                break;
            }
        }
        if ( r == NULL ) {
            r = region_for( addr, 16, 0, NULL );
        }
    }
    *seg = (u16)(r->lin >> 4);
    *off = (u32)(addr - r->base) + (r->lin & 15);
    if ( *off > 0xFFFFUL ) {
        *seg = (u16)(*seg + (*off >> 4));
        *off &= 15;
    }
}

/* VARPTR, VARSEG and SADD take the variable's address; an array's
   element is registered with its whole array */
static int register_in( u8 *addr, u8 *data, const u32 *list, u32 count )
{
    u32 i;
    ARR *arr;

    for ( i = 0; i < count; i++ ) {
        arr = *(ARR **)(data + list[i]);
        if ( arr && addr >= arr->data && addr < arr->data + arr->count * arr->esize ) {
            region_for( arr->data, arr->count * arr->esize, 0, NULL );
            return 1;
        }
    }
    return 0;
}

static void register_array_of( u8 *addr )
{
    PROGRAM *prog = vm.prog;
    u32 k;
    int i;

    /* the module's arrays, the procedures' STATIC ones, and the arrays of
       the calls under way */
    if ( register_in( addr, vm.gdata, prog->grel_arr, prog->ngrel_arr ) ) {
        return;
    }
    for ( i = 0; i < prog->nprocs; i++ ) {
        PROC *proc = &prog->procs[i];
        if ( proc->static_data && register_in( addr, proc->static_data, proc->srel_arr, proc->nsrel_arr ) ) {
            return;
        }
    }
    for ( k = 0; k < vm.nframes; k++ ) {
        PROC *proc = vm.frames[k].proc;
        if ( register_in( addr, vm.frames[k].mine, proc->rel_arr, proc->nrel_arr )
          || register_in( addr, vm.frames[k].mine, proc->imp_arr, proc->nimp_arr ) ) {
            return;
        }
    }
}

void rtf_VARPTR( void )
{
    u8 *addr = (u8 *)POP_A();
    u16 seg;
    u32 off;

    register_array_of( addr );
    address_of( addr, &seg, &off );
    PUSH_L( (s32)(s16)(u16)off );
}

void rtf_VARSEG( void )
{
    u8 *addr = (u8 *)POP_A();
    u16 seg;
    u32 off;

    register_array_of( addr );
    address_of( addr, &seg, &off );
    PUSH_L( (s32)(s16)seg );
}

/* SADD: the string's own bytes, made its own first so a POKE into them
   changes this string and no other */
/* where SADD's strings go in DGROUP: above the module's data and the
   room the Immediate window's variables have */
static u32 sadd_low( void )
{
    u32 low = (vm.gcap + 15) & ~15UL;

    if ( low < 0x8000UL ) {
        low = 0x8000UL;
    }
    if ( low > 0xF000UL ) {
        low = 0xF000UL;                         /* a module of 60K: at least 4K of room */
    }
    return low;
}

static u32 sadd_of( STR **slot )
{
    REGION *r;
    u32 low = sadd_low(), len;
    int i;

    *slot = qs_unique( *slot );
    if ( *slot == NULL ) {
        return 0;
    }
    for ( i = nregions - 1; i >= 0; i-- ) {
        if ( regions[i].slot == slot && regions[i].base == (u8 *)(*slot)->text ) {
            return regions[i].lin - (u32)DGROUP * 16;   /* asked before, the same string */
        }
    }
    len = (*slot)->len;
    if ( len > 0x10000UL - low ) {
        len = 0x10000UL - low;                  /* only what fits is reached */
    }
    if ( next_sadd < low || next_sadd + len > 0x10000UL ) {
        next_sadd = low;
    }
    r = region_for( (u8 *)(*slot)->text, len, (u32)DGROUP * 16 + next_sadd, slot );
    next_sadd += (len + 15) & ~15UL;
    return r->lin - (u32)DGROUP * 16;
}

void rtf_SADD( void )
{
    STR **slot = (STR **)POP_A();

    PUSH_L( (s32)(s16)(u16)sadd_of( slot ) );
}

/* the module's variables and the procedures' STATIC ones stay put for
   the run; a frame or an array can go, and its slot with it */
static int lasting_slot( u8 *addr )
{
    PROGRAM *prog = vm.prog;
    u32 i;

    if ( addr >= vm.gdata && addr + sizeof( STR * ) <= vm.gdata + prog->gsize ) {
        return 1;
    }
    for ( i = 0; i < (u32)prog->nprocs; i++ ) {
        PROC *proc = &prog->procs[i];
        if ( proc->static_data && addr >= proc->static_data
          && addr + sizeof( STR * ) <= proc->static_data + proc->static_size ) {
            return 1;
        }
    }
    return 0;
}

/* VARPTR of a string variable: the slot's address, kept so that PEEK
   there finds the length and SADD */
void rtf_VARPTRSTR( void )
{
    STR **slot = (STR **)POP_A();
    u16 seg;
    u32 off;
    int i;

    register_array_of( (u8 *)slot );
    address_of( (u8 *)slot, &seg, &off );
    if ( !lasting_slot( (u8 *)slot ) ) {
        PUSH_L( (s32)(s16)(u16)off );     /* a frame's or an array's may go */
        return;
    }
    for ( i = 0; i < ndescs && descs[i].slot != slot; i++ ) {
    }
    if ( i == ndescs ) {
        if ( ndescs == MAX_DESCS ) {
            ndescs = 0;
        }
        i = ndescs++;
    }
    descs[i].slot = slot;
    descs[i].lin = (u32)seg * 16 + off;
    PUSH_L( (s32)(s16)(u16)off );
}

/* VARPTR$: a type byte and a handle, which DRAW and PLAY take back */
void rtf_VARPTRS( void )
{
    QTYPE type = (QTYPE)POP_L();
    void *addr = POP_A();
    char bytes[3];
    int i, code;

    for ( i = 0; i < nvps; i++ ) {
        if ( vps[i].addr == addr ) {
            break;
        }
    }
    if ( i == nvps ) {
        if ( nvps == MAX_VPS ) {
            nvps = 0;
        }
        i = nvps++;
        vps[i].addr = addr;
    }
    vps[i].type = type;                         /* a frame's address is another variable's later */
    switch ( TKIND( type ) ) {
    case TY_INT:    code = 2; break;
    case TY_LONG:   code = 20; break;
    case TY_SINGLE: code = 4; break;
    case TY_DOUBLE: code = 8; break;
    default:        code = 3; break;
    }
    bytes[0] = (char)code;
    bytes[1] = (char)(i & 0xFF);
    bytes[2] = (char)(i >> 8);
    PUSH_STR( qs_make( bytes, 3 ) );
}

STR **mem_varptr_string( const u8 *bytes, QTYPE *type, void **addr )
{
    int i = bytes[1] | (bytes[2] << 8);

    if ( i >= nvps || vps[i].addr == NULL ) {
        vm_error( E_ILLEGAL_CALL );             /* never given, or its variable has gone */
    }
    *type = vps[i].type;
    *addr = vps[i].addr;
    return TKIND( *type ) == TY_STR ? (STR **)vps[i].addr : NULL;
}

/* ------------------------------------------------------------------ */
/* PEEK and POKE                                                       */
/* ------------------------------------------------------------------ */

/* a byte of the program's first megabyte: -1 if nothing is there */
static int mem_read( u32 lin )
{
    int i, mode = gfx_mode();
    u8 byte;
    REGION *r;

    for ( i = 0; i < ndescs; i++ ) {
        if ( lin >= descs[i].lin && lin < descs[i].lin + 4 ) {
            u32 part = lin - descs[i].lin;
            u32 word = part < 2 ? QS_LEN( *descs[i].slot ) : sadd_of( descs[i].slot );
            return (part & 1) ? (int)((word >> 8) & 0xFF) : (int)(word & 0xFF);
        }
    }
    r = region_at( lin );
    if ( r != NULL ) {
        return r->base[lin - r->lin];
    }
    if ( lin >= 0xB8000UL && lin < 0xC0000UL && mode == 0 ) {
        u32 at = lin - 0xB8000UL, page_size = (u32)con_rows() * (u32)con_cols() * 2;
        int page = (int)(at / (page_size <= 2048 ? 2048 : page_size <= 4096 ? 4096 : 8192));
        u32 in = at % (page_size <= 2048 ? 2048 : page_size <= 4096 ? 4096 : 8192);
        u16 *cells = con_page_cells( page );
        if ( cells && in < page_size ) {
            return (in & 1) ? cells[in / 2] >> 8 : cells[in / 2] & 0xFF;
        }
        return 0;
    }
    if ( lin >= 0xA0000UL && lin < 0xAFA00UL && mode == 13 ) {
        return gfx_page( gfx_active_page() )[lin - 0xA0000UL];
    }
    if ( lin >= 0xB8000UL && lin < 0xBC000UL && (mode == 1 || mode == 2) ) {
        u32 at = lin - 0xB8000UL, row, col;
        u8 *pic = gfx_page( 0 );
        int bpp = mode == 1 ? 2 : 1, ppb = 8 / bpp, x, out = 0;
        row = (at & 0x1FFF) / 80 * 2 + (at >= 0x2000 ? 1 : 0);
        col = (at & 0x1FFF) % 80;
        if ( row >= 200 ) {
            return 0;
        }
        for ( x = 0; x < ppb; x++ ) {
            out = (out << bpp) | (pic[row * (u32)gfx_width_px() + col * (u32)ppb + (u32)x] & ((1 << bpp) - 1));
        }
        return out;
    }
    if ( (lin >= 0x41AUL && lin < 0x43EUL) || (lin >= 0x480UL && lin < 0x484UL) ) {
        return kb_area[lin - 0x400UL];
    }
    if ( (lin >= 0x449UL && lin < 0x467UL) || (lin >= 0x484UL && lin < 0x487UL) ) {
        int byte = con_bda_byte( lin - 0x400UL );
        if ( byte >= 0 ) {
            return byte;                        /* the program's screen, not the IDE's */
        }
    }
    if ( lin < 0x500UL || lin >= 0xC0000UL ) {
        if ( sys_peek( lin, &byte, 1 ) ) {
            return byte;
        }
    }
    return 0;
}

static void mem_write( u32 lin, int val )
{
    int mode = gfx_mode();
    REGION *r;

    r = region_at( lin );
    if ( r != NULL ) {
        r->base[lin - r->lin] = (u8)val;
        return;
    }
    if ( lin >= 0xB8000UL && lin < 0xC0000UL && mode == 0 ) {
        u32 at = lin - 0xB8000UL, page_size = (u32)con_rows() * (u32)con_cols() * 2;
        u32 stride = page_size <= 2048 ? 2048 : page_size <= 4096 ? 4096 : 8192;
        int page = (int)(at / stride);
        u32 in = at % stride;
        u16 *cells = con_page_cells( page );
        if ( cells && in < page_size ) {
            u16 *cell = &cells[in / 2];
            *cell = (in & 1) ? (u16)((*cell & 0x00FF) | (val << 8)) : (u16)((*cell & 0xFF00) | (u8)val);
            con_page_changed( page, in / 2, 1 );
        }
        return;
    }
    if ( lin >= 0xA0000UL && lin < 0xAFA00UL && mode == 13 ) {
        u32 off = lin - 0xA0000UL;
        void gfx_plot( int x, int y, int c );
        gfx_plot( (int)(off % 320), (int)(off / 320), val );
        return;
    }
    if ( (lin >= 0x41AUL && lin < 0x43EUL) || (lin >= 0x480UL && lin < 0x484UL) ) {
        kb_area[lin - 0x400UL] = (u8)val;
        if ( lin == 0x41AUL && kb_area[0x1A] == kb_area[0x1C] ) {
            con_flush_keys();                   /* POKE &H41A, PEEK(&H41C): the keys go */
        }
    }
}

/* keys a program typed into the ring, for con_poll_keys: -> a key or -1 */
int mach_ring_key( void )
{
    int head = kb_area[0x1A], tail = kb_area[0x1C], start = kb_area[0x80], end = kb_area[0x82], key;

    if ( head == tail || head < 0x1E || head > 0x3C ) {
        return -1;
    }
    key = kb_area[head] | (kb_area[head + 1] << 8);
    head += 2;
    if ( head >= end || head < start ) {
        head = start;
    }
    kb_area[0x1A] = (u8)head;
    return key;
}

static u32 linear( s32 off )
{
    if ( off < -32768L || off > 65535L ) {
        vm_error( E_OVERFLOW );      /* an offset is a word */
    }
    return (u32)def_seg * 16 + ((u32)off & 0xFFFFUL);
}

void rtf_PEEK( void )
{
    int v = mem_read( linear( POP_L() ) );

    PUSH_L( v < 0 ? 0 : v );
}

void rtf_POKE( void )
{
    s32 val = POP_L();
    u32 lin = linear( POP_L() );

    if ( val < -32768L || val > 65535L ) {
        vm_error( E_OVERFLOW );
    }
    mem_write( lin, (int)(val & 0xFF) );      /* 256 is no error in QBasic: the low byte */
}

void rtf_DEFSEG( void )
{
    s32 seg = POP_L();
    int given = (int)POP_L();

    if ( !given ) {
        def_seg = DGROUP;
        return;
    }
    if ( seg < -32768L || seg > 65535L ) {
        vm_error( E_OVERFLOW );
    }
    def_seg = (u16)seg;
}

/* ------------------------------------------------------------------ */
/* BSAVE and BLOAD                                                     */
/* ------------------------------------------------------------------ */

/* BSAVE file$, offset, length: FDh, the segment, the offset, the
   length, then the bytes */
void rtf_BSAVE( void )
{
    s32 len = POP_L(), off = POP_L();
    STR *name = POP_STR();
    char path[PATH_MAX];
    u8 head[7], *buf;
    u32 handle, done, i, lin;
    int rc;

    if ( QS_LEN( name ) == 0 || QS_LEN( name ) >= PATH_MAX ) {
        qs_release( name );
        vm_error( E_BAD_FILENAME );
    }
    mem_cpy( path, name->text, name->len );
    path[name->len] = 0;
    qs_release( name );
    if ( len < 0 || len > 65535L || off < -32768L || off > 65535L ) {
        vm_error( E_ILLEGAL_CALL );
    }
    buf = (u8 *)try_alloc( (u32)len + 7 );
    if ( buf == NULL ) {
        vm_error( E_OUT_OF_MEMORY );
    }
    head[0] = 0xFD;
    head[1] = (u8)def_seg;
    head[2] = (u8)(def_seg >> 8);
    head[3] = (u8)off;
    head[4] = (u8)((u32)off >> 8);
    head[5] = (u8)len;
    head[6] = (u8)((u32)len >> 8);
    mem_cpy( buf, head, 7 );
    lin = (u32)def_seg * 16 + ((u32)off & 0xFFFF);
    for ( i = 0; i < (u32)len; i++ ) {
        int v = mem_read( lin + i );
        buf[7 + i] = (u8)(v < 0 ? 0 : v);
    }
    rc = sys_create( path, &handle );
    if ( rc == 0 ) {
        rc = sys_write( handle, buf, (u32)len + 7, &done );
        sys_close( handle );
    }
    xfree( buf );
    if ( rc != 0 ) {
        vm_error( rc == ERR_NOPATH ? E_PATH_NOT_FOUND : rc == ERR_DISKFULL ? E_DISK_FULL : E_PATH_FILE );
    }
}

/* BLOAD file$ [, offset]: where BSAVE took it from, or the offset in
   the DEF SEG segment */
void rtf_BLOAD( void )
{
    s32 off = POP_L();
    int given = (int)POP_L();
    STR *name = POP_STR();
    char path[PATH_MAX];
    u8 head[7], *buf;
    u32 handle, done, len, i, lin;
    int rc;

    if ( QS_LEN( name ) == 0 || QS_LEN( name ) >= PATH_MAX ) {
        qs_release( name );
        vm_error( E_BAD_FILENAME );
    }
    mem_cpy( path, name->text, name->len );
    path[name->len] = 0;
    qs_release( name );
    rc = sys_open( path, OPEN_READ, &handle );
    if ( rc != 0 ) {
        vm_error( rc == ERR_NOFILE ? E_FILE_NOT_FOUND : rc == ERR_NOPATH ? E_PATH_NOT_FOUND : E_PATH_FILE );
    }
    if ( sys_read( handle, head, 7, &done ) != 0 || done != 7 || head[0] != 0xFD ) {
        sys_close( handle );
        vm_error( E_BAD_FILENAME );
    }
    len = head[5] | (head[6] << 8);
    buf = (u8 *)try_alloc( len + 1 );
    if ( buf == NULL ) {
        sys_close( handle );
        vm_error( E_OUT_OF_MEMORY );
    }
    sys_read( handle, buf, len, &done );
    sys_close( handle );
    if ( given ) {
        lin = (u32)def_seg * 16 + ((u32)off & 0xFFFF);
    } else {
        lin = (u32)(head[1] | (head[2] << 8)) * 16 + (u32)(head[3] | (head[4] << 8));
    }
    for ( i = 0; i < done; i++ ) {
        mem_write( lin + i, buf[i] );
    }
    xfree( buf );
}

void rtf_CALLABS( void )
{
    (void)POP_L();
    vm_error( E_ADVANCED );
}

/* ------------------------------------------------------------------ */
/* ports                                                               */
/* ------------------------------------------------------------------ */

static int dac_read_index, dac_write_index, dac_part;
static u8  dac_value[3];

void gfx_dac_write( int index, const u8 *rgb );
void gfx_dac_read( int index, u8 *rgb );

static int port_read( u32 port )
{
    u8 rgb[3];
    int v;

    switch ( port ) {
    case 0x60:
        con_poll_keys();
        return con_last_scan();
    case 0x64:
        con_poll_keys();
        return con_key_waiting() ? 0x1D : 0x1C;
    case 0x92:
        return 0x02;
    case 0x3C9:
        gfx_dac_read( dac_read_index, rgb );
        v = rgb[dac_part];
        if ( ++dac_part == 3 ) {
            dac_part = 0;
            dac_read_index = (dac_read_index + 1) & 255;
        }
        return v;
    case 0x3DA:
        gfx_flush();                            /* a wait for the retrace: show the picture */
        break;
    }
    return (int)(port_in( port ) & 0xFF);
}

void rtf_INP( void )
{
    s32 port = POP_L();

    if ( port < 0 || port > 65535L ) {
        vm_error( E_ILLEGAL_CALL );
    }
    PUSH_L( port_read( (u32)port ) );
}

void rtf_OUT( void )
{
    s32 val = POP_L(), port = POP_L();

    if ( port < 0 || port > 65535L || val < 0 || val > 255 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    switch ( port ) {
    case 0x60:
    case 0x64:
    case 0x92:
        return;                                 /* the keyboard controller and A20 are not the program's */
    case 0x3C7:
        dac_read_index = (int)val;
        dac_part = 0;
        return;
    case 0x3C8:
        dac_write_index = (int)val;
        dac_part = 0;
        return;
    case 0x3C9:
        dac_value[dac_part++] = (u8)(val & 63);
        if ( dac_part == 3 ) {
            gfx_dac_write( dac_write_index, dac_value );
            dac_part = 0;
            dac_write_index = (dac_write_index + 1) & 255;
        }
        return;
    }
    port_out( (u32)port, (u32)val );
}

/* WAIT port, and [, xor]: until (INP(port) XOR xor) AND and is not 0 */
void rtf_WAIT( void )
{
    s32 xorv = POP_L(), andv = POP_L(), port = POP_L();
    u32 count = 0;

    if ( port < 0 || port > 65535L || andv < 0 || andv > 255 || xorv < 0 || xorv > 255 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    for ( ;; ) {
        if ( ((port_read( (u32)port ) ^ (int)xorv) & (int)andv) != 0 ) {
            return;
        }
        if ( (++count & 255) == 0 ) {
            con_poll_keys();
            snd_service();
        }
    }
}
