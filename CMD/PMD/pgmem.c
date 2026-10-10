/*
 * PGMEM.C - the pages Memory, TSR Programs and Device Drivers.
 *
 * THREE ARENAS, and the kernel walks each of them for us (21h/F1h
 * AL=8): its own, above 2MB, which is what native programs are given;
 * the conventional one a DOS box lives in; and that box's upper
 * memory.  A block comes back as a signature, a size, an owner and the
 * flat address of its header.  Below 1MB the header is DOS's - a name
 * in its last eight bytes - and is read with sys_peek, as the owner's
 * PSP and environment are for the name of the program and what was
 * typed after it.
 *
 * THE MAP of the adapter area is MSD's, a K to a cell from A000h to
 * FFFFh: a ROM that declares itself (55 AA, a length, and bytes that
 * sum to zero) is ROM for its length; a K whose first byte keeps what
 * is written to it is RAM - the kernel makes that test, LMF_RAM,
 * because it is a store; a K of FFh is nothing at all; and anything
 * else reads like a ROM without having said so, "possibly available".
 * Over that go the EMS page frame and the upper memory blocks.
 *
 * THE DEVICE DRIVERS are the chain a DOS box is given, from the NUL
 * header in the list of lists (DOSINT.ASM's kernel_facts says where).
 */
#include "pmd.h"

#define CONV_TOP        0xA0000UL
#define MAX_BLOCKS      400

/* ------------------------------------------------------------------ */
/* the blocks below 1MB                                                */
/* ------------------------------------------------------------------ */

static int name_char( int ch )
{
    return ch > ' ' && ch < 127;
}

/* the eight bytes at the end of a block's header, if they read as a
   name: DOS 4 and later put a program's there */
static void header_name( u32 header, char *name )
{
    u8 raw[8];
    int index;

    name[0] = 0;
    sys_peek( header + 8, raw, 8 );
    for ( index = 0; index < 8 && raw[index]; index++ ) {
        if ( !name_char( raw[index] ) && raw[index] != 0x20 ) {
            name[0] = 0;
            return;
        }
        name[index] = (char)raw[index];
    }
    while ( index > 0 && name[index - 1] == 0x20 ) {   /* "SC      " */
        index--;
    }
    name[index] = 0;
}

/* The file a PSP was loaded from: after its environment's last string
   there is a word of 1 and then the path.  Just the name is kept. */
static void psp_file_name( u32 psp_seg, char *name, u32 max )
{
    static u8 env[1024];
    u32 env_seg = peek_w( (psp_seg << 4) + 0x2C ), pos, start;

    name[0] = 0;
    if ( env_seg == 0 || env_seg >= 0xFFF0 ) {
        return;
    }
    sys_peek( env_seg << 4, env, sizeof( env ) );
    for ( pos = 0; pos + 4 < sizeof( env ); pos++ ) {
        if ( env[pos] == 0 && env[pos + 1] == 0 ) {
            break;
        }
    }
    if ( pos + 4 >= sizeof( env ) || env[pos + 2] != 1 || env[pos + 3] != 0 ) {
        return;
    }
    pos += 4;
    for ( start = pos; pos < sizeof( env ) - 1 && env[pos]; pos++ ) {
        if ( !name_char( env[pos] ) && env[pos] != ' ' ) {
            return;
        }
        if ( env[pos] == 0x5C || env[pos] == ':' || env[pos] == '/' ) {
            start = pos + 1;
        }
    }
    if ( pos - start + 1 <= max ) {
        mem_cpy( name, env + start, pos - start );
        name[pos - start] = 0;
    }
}

/* what was typed after a program's name: its PSP, offset 80h */
static void psp_params( u32 psp_seg, char *params, u32 max )
{
    u8 tail[128];
    u32 len, pos = 1, out = 0;

    sys_peek( (psp_seg << 4) + 0x80, tail, sizeof( tail ) );
    len = tail[0];
    if ( len > 126 ) {
        len = 0;
    }
    while ( pos <= len && tail[pos] == ' ' ) {
        pos++;
    }
    for ( ; pos <= len && out + 1 < max && tail[pos] >= ' ' && tail[pos] < 127; pos++ ) {
        params[out++] = (char)tail[pos];
    }
    params[out] = 0;
}

static void item_name( MEMITEM *item, const MEMBLK *blk )
{
    char name[20];
    u32 owner = blk->owner & 0xFFFF;

    item->params[0] = 0;
    if ( blk->sig == 'Z' ) {
        item->free = 1;
        str_cpy( item->name, "Free Memory" );
        return;
    }
    header_name( blk->addr, name );
    if ( owner == 8 ) {
        /* DOS's own: "SC" is its code - and, above 640K, the stretch of
           adapter space the chain steps over - and "SD" its data */
        if ( str_cmp( name, "SC" ) == 0 ) {
            str_cpy( item->name, blk->addr + 16 + blk->size > CONV_TOP ? "Excluded UMB Area"
                                 : "System Code" );
        } else if ( str_cmp( name, "SD" ) == 0 ) {
            str_cpy( item->name, "System Data" );
        } else {
            str_cpy( item->name, "System Area" );
        }
        return;
    }
    /* a program's block: the file it came from, or the name in the
       header of the block its PSP is in */
    psp_file_name( owner, item->name, sizeof( item->name ) );
    if ( item->name[0] == 0 ) {
        header_name( (owner - 1) << 4, item->name );
    }
    if ( item->name[0] == 0 ) {
        str_cpy( item->name, "???" );
    }
    if ( str_cmp( item->name, "COMMAND" ) == 0 ) {   /* the shell has no path to read */
        str_cpy( item->name, "COMMAND.COM" );
    }
    if ( peek_w( owner << 4 ) == 0x20CD ) {         /* it IS a PSP */
        psp_params( owner, item->params, sizeof( item->params ) );
    }
}

/* every block of the conventional arena and then of upper memory */
int mem_items( MEMITEM **items )
{
    MEMITEM *list = (MEMITEM *)try_alloc( MAX_BLOCKS * sizeof( MEMITEM ) );
    MEMBLK blk;
    int count = 0, arena;
    u32 index;

    *items = list;
    if ( list == NULL ) {
        return 0;
    }
    for ( arena = ARENA_CONV; arena <= ARENA_UPPER; arena++ ) {
        for ( index = 0; count < MAX_BLOCKS && sys_mem_block( arena, index, &blk ); index++ ) {
            mem_set( &list[count], 0, sizeof( MEMITEM ) );
            list[count].seg = blk.addr >> 4;
            list[count].size = blk.size;
            item_name( &list[count], &blk );
            count++;
        }
    }
    return count;
}

/* who has the memory an address below 1MB is in: a block's name, or "" */
const char *mem_owner_name( u32 addr )
{
    static char name[20];
    MEMITEM item;
    MEMBLK blk;
    int arena;
    u32 index;

    name[0] = 0;
    for ( arena = ARENA_CONV; arena <= ARENA_UPPER; arena++ ) {
        for ( index = 0; sys_mem_block( arena, index, &blk ); index++ ) {
            if ( addr >= blk.addr && addr < blk.addr + 16 + blk.size ) {
                mem_set( &item, 0, sizeof( item ) );
                item_name( &item, &blk );
                str_cpy( name, item.name );
                return name;
            }
        }
    }
    return name;
}

void pg_tsr( TEXT *text )
{
    MEMITEM *items;
    int count = mem_items( &items ), index;

    tx_add( text, "Program Name        Address   Size   Command Line Parameters" );
    tx_add( text, "------------------  -------  ------  --------------------------------" );
    for ( index = 0; index < count; index++ ) {
        tx_addf( text, "%-18s    %04X  %7u   %s", items[index].name, items[index].seg,
                 items[index].size, items[index].params );
    }
    xfree( items );
}

/* ------------------------------------------------------------------ */
/* the device drivers                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    u32  header;                /* seg:off, as a far pointer in a dword */
    u16  attr;
    int  block;                 /* a block device: units, not a name */
    int  units;
    char name[9];
} DEVICE;

/* the chain's next driver; 0 when "at" was the last.  Start with 0. */
static u32 dev_next( u32 at, DEVICE *dev )
{
    u8 head[18];
    u32 flat, next;
    int index;

    if ( at == 0 ) {
        /* the NUL driver's header is in the list of lists itself */
        next = ((kernel_facts.dosdata >> 4) << 16) | (kernel_facts.lol + 0x22 - kernel_facts.dosdata);
    } else {
        next = peek_d( FAR_FLAT( at >> 16, at & 0xFFFF ) );
    }
    if ( (next & 0xFFFF) == 0xFFFF ) {
        return 0;
    }
    flat = FAR_FLAT( next >> 16, next & 0xFFFF );
    if ( !sys_peek( flat, head, sizeof( head ) ) ) {
        return 0;
    }
    dev->header = next;
    dev->attr = *(u16 *)(head + 4);
    dev->block = (dev->attr & 0x8000) == 0;
    dev->units = head[10];
    for ( index = 0; index < 8; index++ ) {
        dev->name[index] = head[10 + index] >= ' ' && head[10 + index] < 127 ? (char)head[10 + index]
                           : ' ';
    }
    dev->name[8] = 0;
    for ( index = 8; index > 0 && dev->name[index - 1] == ' '; index-- ) {
        dev->name[index - 1] = 0;
    }
    return next;
}

int dev_present( const char *name )
{
    DEVICE dev;
    u32 at = 0;
    int guard;

    for ( guard = 0; guard < 100 && (at = dev_next( at, &dev )) != 0; guard++ ) {
        if ( !dev.block && str_icmp( dev.name, name ) == 0 ) {
            return 1;
        }
    }
    return 0;
}

void pg_device( TEXT *text )
{
    DEVICE dev;
    u32 at = 0;
    int guard, bit;
    char attrs[17], units[8];
    const char *file;

    tx_add( text, "Device        Filename  Units    Header      Attributes" );
    tx_add( text, "------------  --------  -----  ---------  ----------------" );
    for ( guard = 0; guard < 100 && (at = dev_next( at, &dev )) != 0; guard++ ) {
        for ( bit = 0; bit < 16; bit++ ) {
            attrs[bit] = (dev.attr & (0x8000 >> bit)) ? '1' : '.';
        }
        attrs[16] = 0;
        units[0] = 0;
        if ( dev.block ) {
            sfmt( units, sizeof( units ), "%u", dev.units );
        }
        /* a driver loaded from a file is in a block that has its name;
           the ones in the kernel's own data are in nobody's */
        file = mem_owner_name( FAR_FLAT( dev.header >> 16, dev.header & 0xFFFF ) );
        if ( str_cmp( file, "System Area" ) == 0 || str_cmp( file, "System Data" ) == 0
             || str_cmp( file, "System Code" ) == 0 || str_cmp( file, "Free Memory" ) == 0
             || str_cmp( file, "Excluded UMB Area" ) == 0 || str_cmp( file, "???" ) == 0 ) {
            file = "";
        }
        tx_addf( text, "%-12s  %-8s  %5s  %04X:%04X  %s", dev.block ? "Block Device" : dev.name,
                 file, units, dev.header >> 16, dev.header & 0xFFFF, attrs );
    }
}

/* ------------------------------------------------------------------ */
/* the map                                                             */
/* ------------------------------------------------------------------ */

/* An option ROM at "addr": how many K it says it is, or 0.  55 AA, its
   length in 512s, and every byte of it summing to zero - which a 55 AA
   that is only somebody's data will not. */
static int option_rom( u32 addr )
{
    static u8 part[512];
    u8 head[3], sum = 0;
    u32 blocks, index, pos;

    sys_peek( addr, head, 3 );
    if ( head[0] != 0x55 || head[1] != 0xAA || head[2] == 0 ) {
        return 0;
    }
    blocks = head[2];
    if ( addr + blocks * 512 > 0x100000UL ) {
        return 0;
    }
    for ( index = 0; index < blocks; index++ ) {
        sys_peek( addr + index * 512, part, 512 );
        for ( pos = 0; pos < 512; pos++ ) {
            sum = (u8)(sum + part[pos]);
        }
    }
    return sum == 0 ? (int)((blocks + 1) / 2) : 0;
}

static void map_fill( u8 map[MAP_ROWS][MAP_COLS], u32 addr, u32 bytes, int kind )
{
    u32 cell, cells = (bytes + 1023) / 1024;

    if ( addr < CONV_TOP ) {
        return;
    }
    for ( cell = (addr - CONV_TOP) / 1024; cells && cell < MAP_ROWS * MAP_COLS; cell++, cells-- ) {
        map[cell / MAP_COLS][cell % MAP_COLS] = (u8)kind;
    }
}

void mem_map( u8 map[MAP_ROWS][MAP_COLS] )
{
    static u8 block[1024];
    u32 cell, addr, frame, total, avail, index, pos;
    int rom_left = 0, arena;
    MEMBLK blk;
    /* E000h and up is the system's ROM on a Micro Channel machine;
       everywhere else it starts at F000h */
    u32 rom_from = bus_is_mca() ? 0xE0000UL : 0xF0000UL;

    for ( cell = 0; cell < MAP_ROWS * MAP_COLS; cell++ ) {
        addr = CONV_TOP + cell * 1024;
        if ( addr >= rom_from ) {
            map[cell / MAP_COLS][cell % MAP_COLS] = MC_ROM;
            continue;
        }
        if ( rom_left == 0 ) {
            rom_left = option_rom( addr );
        }
        if ( rom_left ) {
            map[cell / MAP_COLS][cell % MAP_COLS] = MC_ROM;
            rom_left--;
            continue;
        }
        if ( sys_ram_test( addr ) ) {
            map[cell / MAP_COLS][cell % MAP_COLS] = MC_RAM;
            continue;
        }
        sys_peek( addr, block, sizeof( block ) );
        for ( pos = 0; pos < sizeof( block ) && block[pos] == 0xFF; pos++ ) {
        }
        map[cell / MAP_COLS][cell % MAP_COLS] = pos == sizeof( block ) ? MC_EMPTY : MC_MAYBE;
    }

    /* and over it, what the memory managers have made of it */
    if ( sys_ems( &frame, &total, &avail ) && frame ) {
        map_fill( map, frame << 4, 0x10000UL, MC_FRAME );
    }
    for ( arena = ARENA_CONV; arena <= ARENA_UPPER; arena++ ) {
        for ( index = 0; sys_mem_block( arena, index, &blk ); index++ ) {
            if ( blk.addr < CONV_TOP || ((blk.owner & 0xFFFF) == 8 && blk.sig != 'Z'
                 && blk.addr + 16 + blk.size > CONV_TOP && peek_w( blk.addr + 8 ) == 0x4353) ) {
                continue;               /* below 640K, or "SC": a stretch stepped over */
            }
            map_fill( map, blk.addr, blk.size + 16, blk.sig == 'Z' ? MC_UMB_FREE : MC_UMB_USED );
        }
    }
}

int mem_map_char( int kind, int report )
{
    switch ( kind ) {
    case MC_RAM:      return report ? '#' : 0xB1;
    case MC_ROM:      return report ? 'R' : 0xDB;
    case MC_MAYBE:    return report ? '.' : 0xB0;
    case MC_FRAME:    return 'P';
    case MC_UMB_USED: return 'U';
    case MC_UMB_FREE: return 'F';
    }
    return ' ';
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    u32 total, avail, largest;      /* bytes */
} USAGE;

static void arena_usage( int arena, USAGE *use )
{
    MEMBLK blk;
    u32 index;

    mem_set( use, 0, sizeof( *use ) );
    for ( index = 0; sys_mem_block( arena, index, &blk ); index++ ) {
        if ( blk.sig == 'E' ) {
            break;
        }
        if ( arena != ARENA_KERNEL && (blk.owner & 0xFFFF) == 8 && blk.sig != 'Z'
             && blk.addr + 16 + blk.size > CONV_TOP && peek_w( blk.addr + 8 ) == 0x4353 ) {
            continue;                   /* adapter space the chain steps over */
        }
        use->total += blk.size;
        if ( blk.sig == 'Z' ) {
            use->avail += blk.size;
            if ( blk.size > use->largest ) {
                use->largest = blk.size;
            }
        }
    }
}

/* the right-hand side of the Memory page: what there is of each kind */
static void memory_figures( TEXT *side )
{
    USAGE conv, upper, kernel;
    u32 conv_k = peek_w( 0x413 ), ext_kb, top, frame, total, avail, xms;
    int width = 20;

    arena_usage( ARENA_CONV, &conv );
    arena_usage( ARENA_UPPER, &upper );
    arena_usage( ARENA_KERNEL, &kernel );
    sys_ext_memory( &ext_kb, &top );

    tx_add( side, "Conventional Memory" );
    tx_fieldf( side, width, "Total: ", "%,uK", conv_k );
    tx_fieldf( side, width, "Available: ", "%,uK", conv.avail / 1024 );
    tx_fieldf( side, width, "", "%,u bytes", conv.avail );
    tx_add( side, "" );
    tx_add( side, "Extended Memory" );
    tx_fieldf( side, width, "Total: ", "%,uK", ext_kb );
    tx_add( side, "" );
    tx_add( side, "PM-DOS Program Memory" );
    tx_fieldf( side, width, "Total: ", "%,uK", kernel.total / 1024 );
    tx_fieldf( side, width, "Available: ", "%,uK", kernel.avail / 1024 );
    tx_fieldf( side, width, "Largest Free Block: ", "%,uK", kernel.largest / 1024 );
    if ( upper.total ) {
        tx_add( side, "" );
        tx_add( side, "MS-DOS Upper Memory Blocks" );
        tx_fieldf( side, width, "Total UMBs: ", "%,uK", upper.total / 1024 );
        tx_fieldf( side, width, "Total Free UMBs: ", "%,uK", upper.avail / 1024 );
        tx_fieldf( side, width, "Largest Free Block: ", "%,uK", upper.largest / 1024 );
    }
    if ( sys_ems( &frame, &total, &avail ) ) {
        tx_add( side, "" );
        tx_add( side, "Expanded Memory (EMS)" );
        tx_fieldf( side, width, "LIM Version: ", "%u.%02u", kernel_facts.ems_version >> 4,
                   (kernel_facts.ems_version & 0x0F) * 10 );
        if ( frame ) {
            tx_fieldf( side, width, "Page Frame Address: ", "%04XH", frame );
        } else {
            tx_field( side, width, "Page Frame Address: ", "No Page Frame" );
        }
        tx_fieldf( side, width, "Total: ", "%,uK", total * 16 );
        tx_fieldf( side, width, "Available: ", "%,uK", avail * 16 );
    }
    /* XMS is one pool with the program memory above: what a DOS box
       may still have of it is the largest piece there is */
    xms = sys_xms_largest();
    tx_add( side, "" );
    tx_add( side, "XMS Information" );
    tx_field( side, width, "XMS Version: ", "3.00" );
    tx_fieldf( side, width, "Largest Free Block: ", "%,uK", xms / 1024 );
    tx_add( side, "" );
    tx_add( side, "DPMI Information" );
    tx_field( side, width, "DPMI Detected: ", "Yes" );
    tx_fieldf( side, width, "Version: ", "%u.%02u", kernel_facts.dpmi_major, kernel_facts.dpmi_minor );
}

void pg_memory( TEXT *text, int report )
{
    u8 map[MAP_ROWS][MAP_COLS];
    TEXT side;
    char row[40], label[8];
    int line, lines, cell, map_row;
    u32 seg;

    mem_map( map );
    tx_init( &side );
    memory_figures( &side );

    tx_addf( text, "Legend:  Available \"%c%c\"  RAM \"%c%c\"  ROM \"%c%c\"  Possibly Available \"%c%c\"",
             ' ', ' ', mem_map_char( MC_RAM, report ), mem_map_char( MC_RAM, report ),
             mem_map_char( MC_ROM, report ), mem_map_char( MC_ROM, report ),
             mem_map_char( MC_MAYBE, report ), mem_map_char( MC_MAYBE, report ) );
    tx_add( text, "  EMS Page Frame \"PP\"  Used UMBs \"UU\"  Free UMBs \"FF\"" );
    lines = side.count > MAP_ROWS ? side.count : MAP_ROWS;
    for ( line = 0; line < lines; line++ ) {
        if ( line < MAP_ROWS ) {
            /* the top of memory first: FC00h, and down to A000h */
            map_row = MAP_ROWS - 1 - line;
            seg = 0xA000 + (u32)map_row * 0x400;
            label[0] = 0;
            if ( line == 0 ) {
                str_cpy( label, "1024K" );
            } else if ( (seg & 0x0FFF) == 0 ) {
                sfmt( label, sizeof( label ), "%4uK", seg / 64 );
            }
            sfmt( row, sizeof( row ), "%5s %04X ", label, seg );
            for ( cell = 0; cell < MAP_COLS; cell++ ) {
                row[11 + cell] = (char)mem_map_char( map[map_row][cell], report );
            }
            sfmt( row + 11 + MAP_COLS, 8, " %04X", seg + 0x3FF );
        } else {
            row[0] = 0;
        }
        tx_addf( text, "%-32s  %s", row, line < side.count ? side.line[line] : "" );
    }
    tx_free( &side );
}

void sum_memory( char *line1, char *line2 )
{
    u32 ext_kb, top, frame, total, avail;
    char line[40];

    sys_ext_memory( &ext_kb, &top );
    sfmt( line, sizeof( line ), "%uK, %uK Ext", peek_w( 0x413 ), ext_kb );
    str_cpyn( line1, line, SUM_WIDTH + 1 );
    line2[0] = 0;
    if ( sys_ems( &frame, &total, &avail ) ) {
        sfmt( line, sizeof( line ), "%uK EMS", total * 16 );
        str_cpyn( line2, line, SUM_WIDTH + 1 );
    }
}
