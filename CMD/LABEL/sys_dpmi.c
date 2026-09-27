/*
 * SYS_DPMI.C - DOS/32A host (the test build, LABEL32A.EXE).
 *
 * Runs the same program on real-mode MS-DOS 7.x (or any DOS) under the
 * DOS/32A extender bound in as the stub.  Every DOS call is made in
 * real mode through DPMI function 0300h with a 64 KB conventional
 * memory transfer buffer, so nothing depends on which INT 21h
 * functions the extender itself translates.  The flat address space is
 * zero-based, so conventional memory is addressed directly.
 */
#include "label.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;
} REGS;

typedef struct {                /* DPMI real mode call structure */
    u32 edi, esi, ebp, resv, ebx, edx, ecx, eax;
    u16 flags, es, ds, fs, gs, ip, cs, sp, ss;
} RMCS;

void __cdecl int21( REGS *regs );
void __cdecl int31( REGS *regs );

#define TB_SIZE     0x10000ul
#define TB_DATA     0x10u       /* sector data follows the packet */
#define RCF( rmcs ) ((rmcs).flags & 1)
#define RAX( rmcs ) ((rmcs).eax & 0xFFFF)

static u16  tb_seg;
static u8  *tb;
static char tail[260];

void __cdecl dos_entry( void )
{
    label_main();
}

static void rm_clear( RMCS *rmcs )
{
    mem_set( rmcs, 0, sizeof( *rmcs ) );
}

/* real-mode INT through DPMI 0300h; cf_in presets the carry flag */
static void rm_int( int no, RMCS *rmcs, int cf_in )
{
    REGS regs;

    rmcs->flags = (u16)(0x0202 | (cf_in ? 1 : 0));
    rmcs->sp = rmcs->ss = 0;
    rmcs->fs = rmcs->gs = 0;
    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0x0300;
    regs.ebx = (u32)no;
    regs.edi = (u32)rmcs;
    int31( &regs );
    if ( regs.flags & 1 ) {
        rmcs->flags |= 1;
    }
}

static void dos( RMCS *rmcs, u32 ax )
{
    rmcs->eax = ax;
    rm_int( 0x21, rmcs, 0 );
}

int sys_init( void )
{
    REGS regs;
    RMCS rmcs;
    u8 *psp;
    u32 len;

    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0x0100;                  /* allocate DOS memory */
    regs.ebx = TB_SIZE >> 4;
    int31( &regs );
    if ( regs.flags & 1 ) {
        return -1;
    }
    tb_seg = (u16)regs.eax;
    tb = (u8 *)((u32)tb_seg << 4);

    rm_clear( &rmcs );
    dos( &rmcs, 0x6200 );
    psp = (u8 *)((rmcs.ebx & 0xFFFF) << 4);
    len = psp[0x80];
    if ( len > 126 ) {
        len = 126;
    }
    mem_cpy( tail, psp + 0x81, len );
    tail[len] = 0;
    return 0;
}

const char *sys_cmdline( void )
{
    return tail;
}

void sys_write( int handle, const char *buf, u32 len )
{
    RMCS rmcs;
    u32 chunk;

    while ( len ) {
        chunk = len > 0xF000u ? 0xF000u : len;
        mem_cpy( tb, buf, chunk );
        rm_clear( &rmcs );
        rmcs.ebx = (u32)handle;
        rmcs.ecx = chunk;
        rmcs.ds = tb_seg;
        rmcs.edx = 0;
        dos( &rmcs, 0x4000 );
        buf += chunk;
        len -= chunk;
    }
}

static int stdin_is_device( void )
{
    RMCS rmcs;

    rm_clear( &rmcs );
    rmcs.ebx = 0;
    dos( &rmcs, 0x4400 );
    return !RCF( rmcs ) && (rmcs.edx & 0x80);
}

/* one byte of redirected input; -1 at the end */
static int read_byte( void )
{
    RMCS rmcs;

    rm_clear( &rmcs );
    rmcs.ebx = 0;
    rmcs.ecx = 1;
    rmcs.ds = tb_seg;
    rmcs.edx = 0;
    dos( &rmcs, 0x3F00 );
    if ( RCF( rmcs ) || RAX( rmcs ) == 0 ) {
        return -1;
    }
    return tb[0];
}

int sys_read_line( char *buf, int max )
{
    RMCS rmcs;
    int len = 0, ch;

    if ( stdin_is_device() ) {
        tb[0] = (u8)(max > 255 ? 255 : max);
        tb[1] = 0;
        rm_clear( &rmcs );
        rmcs.ds = tb_seg;
        rmcs.edx = 0;
        dos( &rmcs, 0x0A00 );
        len = tb[1];
        if ( len > max - 1 ) {
            len = max - 1;
        }
        mem_cpy( buf, tb + 2, (u32)len );
        buf[len] = 0;
        sys_write( H_OUT, "\n", 1 );
        return len;
    }
    for ( ;; ) {
        ch = read_byte();
        if ( ch < 0 ) {
            buf[len] = 0;
            sys_write( H_OUT, buf, (u32)len );
            sys_write( H_OUT, "\r\n", 2 );
            return len ? len : -1;
        }
        if ( ch == '\r' ) {
            continue;
        }
        if ( ch == '\n' ) {
            break;
        }
        if ( len < max - 1 ) {
            buf[len++] = (char)ch;
        }
    }
    buf[len] = 0;
    sys_write( H_OUT, buf, (u32)len );
    sys_write( H_OUT, "\r\n", 2 );
    return len;
}

int sys_read_key( void )
{
    RMCS rmcs;
    char ch;
    int key;

    if ( stdin_is_device() ) {
        rm_clear( &rmcs );
        dos( &rmcs, 0x0C08 );
        key = (int)(rmcs.eax & 0xFF);
        if ( key == 0 ) {
            rm_clear( &rmcs );
            dos( &rmcs, 0x0800 );
        }
    } else {
        do {
            key = read_byte();
        } while ( key == '\r' || key == '\n' );
        if ( key < 0 ) {
            return -1;
        }
    }
    if ( key >= ' ' ) {
        ch = (char)key;
        sys_write( H_OUT, &ch, 1 );
    }
    return key;
}

void sys_exit( int code )
{
    REGS regs;

    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0x4C00 | ((u32)code & 0xFF);
    int21( &regs );
}

int sys_get_drive( void )
{
    RMCS rmcs;

    rm_clear( &rmcs );
    dos( &rmcs, 0x1900 );
    return (int)(rmcs.eax & 0xFF);
}

int sys_drive_type( int drv )
{
    RMCS rmcs;

    rm_clear( &rmcs );
    rmcs.ebx = (u32)drv + 1;
    dos( &rmcs, 0x4409 );
    if ( RCF( rmcs ) ) {
        return RAX( rmcs ) == 1 ? DRV_LOCAL : DRV_INVALID;
    }
    if ( rmcs.edx & 0x8000 ) {
        return DRV_SUBST;
    }
    if ( rmcs.edx & 0x1000 ) {
        return DRV_REMOTE;
    }
    return DRV_LOCAL;
}

int sys_truename_drive( int drv )
{
    RMCS rmcs;

    tb[0] = (u8)('A' + drv);
    tb[1] = ':';
    tb[2] = '\\';
    tb[3] = 0;
    tb[0x100] = 0;
    rm_clear( &rmcs );
    rmcs.ds = rmcs.es = tb_seg;
    rmcs.esi = 0;
    rmcs.edi = 0x100;
    dos( &rmcs, 0x6000 );
    if ( RCF( rmcs ) || tb[0x101] != ':' ) {
        return -1;
    }
    return ch_upper( tb[0x100] ) - 'A';
}

int sys_get_bpb( int drv, u8 *sector )
{
    RMCS rmcs;

    mem_set( tb, 0, 0x40 );
    WR16( tb, 0x3F );
    rm_clear( &rmcs );
    rmcs.edx = (u32)drv + 1;
    rmcs.ecx = 0x3F;
    rmcs.es = tb_seg;
    rmcs.edi = 0;
    rmcs.eax = 0x7302;
    rm_int( 0x21, &rmcs, 1 );
    if ( RCF( rmcs ) ) {
        return -1;
    }
    bpb_from_dpb( tb + 2, sector );
    return 0;
}

static int lock_call( int drv, u32 cx )
{
    RMCS rmcs;

    rm_clear( &rmcs );
    rmcs.ebx = (u32)drv + 1;
    rmcs.ecx = cx;
    rmcs.edx = 0;
    dos( &rmcs, 0x440D );
    if ( !RCF( rmcs ) ) {
        return 0;
    }
    return RAX( rmcs ) == 1 ? 1 : -1;
}

int sys_lock( int drv, int fat32 )
{
    int rc = -1;

    if ( fat32 ) {
        rc = lock_call( drv, 0x484A );
    }
    if ( rc != 0 ) {
        rc = lock_call( drv, 0x084A );
    }
    return rc < 0 ? -1 : 0;
}

void sys_unlock( int drv, int fat32 )
{
    if ( !fat32 || lock_call( drv, 0x486A ) != 0 ) {
        lock_call( drv, 0x086A );
    }
}

static int ext_error( u32 ax )
{
    return ax >= DE_FIRST && ax <= DE_LAST ? (int)ax : DE_GENERAL;
}

static int crit_error( u32 al )
{
    return al <= DE_LAST - DE_FIRST ? (int)al + DE_FIRST : DE_GENERAL;
}

static int disk_io( int drv, u32 lba, u32 count, u32 mode )
{
    RMCS rmcs;

    WR32( tb, lba );
    WR16( tb + 4, count );
    WR16( tb + 6, TB_DATA );
    WR16( tb + 8, tb_seg );

    rm_clear( &rmcs );
    rmcs.ecx = 0xFFFF;
    rmcs.edx = (u32)drv + 1;
    rmcs.esi = mode;
    rmcs.ds = tb_seg;
    rmcs.ebx = 0;
    rmcs.eax = 0x7305;
    rm_int( 0x21, &rmcs, 1 );
    if ( !RCF( rmcs ) ) {
        return 0;
    }
    if ( RAX( rmcs ) != 0x7300 && RAX( rmcs ) != 0x0001 ) {
        return ext_error( RAX( rmcs ) );
    }

    /* DOS before 7.1: INT 25h/26h, packet form, then the old form.
       DOS/32A will not start on a DOS before 4.00, so the packet form
       (DOS 3.31 and later) is always understood; the old form is only
       a second try within the first 64K sectors */
    rm_clear( &rmcs );
    rmcs.eax = (u32)drv;
    rmcs.ecx = 0xFFFF;
    rmcs.ds = tb_seg;
    rmcs.ebx = 0;
    rm_int( (mode & 1) ? 0x26 : 0x25, &rmcs, 0 );
    if ( !RCF( rmcs ) ) {
        return 0;
    }
    if ( lba + count > 0xFFFFul ) {
        return crit_error( rmcs.eax & 0xFF );
    }
    rm_clear( &rmcs );
    rmcs.eax = (u32)drv;
    rmcs.ecx = count;
    rmcs.edx = lba;
    rmcs.ds = tb_seg;
    rmcs.ebx = TB_DATA;
    rm_int( (mode & 1) ? 0x26 : 0x25, &rmcs, 0 );
    return RCF( rmcs ) ? crit_error( rmcs.eax & 0xFF ) : 0;
}

int sys_read_sec( int drv, u32 lba, u32 count, u32 bps, void *buf )
{
    int err;

    if ( count * bps > TB_SIZE - TB_DATA - 4096 ) {
        return DE_GENERAL;
    }
    err = disk_io( drv, lba, count, 0 );
    if ( err == 0 ) {
        mem_cpy( buf, tb + TB_DATA, count * bps );
    }
    return err;
}

int sys_write_sec( int drv, u32 lba, u32 count, u32 bps, const void *buf,
                   int kind )
{
    if ( count * bps > TB_SIZE - TB_DATA - 4096 ) {
        return DE_GENERAL;
    }
    mem_cpy( tb + TB_DATA, buf, count * bps );
    return disk_io( drv, lba, count, 1u | ((u32)kind << 13) );
}

void sys_reset_drive( int drv )
{
    RMCS rmcs;

    rm_clear( &rmcs );
    rmcs.ecx = 1;
    rmcs.edx = (u32)drv + 1;
    dos( &rmcs, 0x710D );
    rm_clear( &rmcs );
    dos( &rmcs, 0x0D00 );
}

void *sys_mem_alloc( u32 size )
{
    REGS regs;

    mem_set( &regs, 0, sizeof( regs ) );
    regs.eax = 0x0501;
    regs.ebx = size >> 16;
    regs.ecx = size & 0xFFFF;
    int31( &regs );
    if ( regs.flags & 1 ) {
        return NULL;
    }
    return (void *)(((regs.ebx & 0xFFFF) << 16) | (regs.ecx & 0xFFFF));
}

void sys_get_datetime( u16 *dos_date, u16 *dos_time )
{
    RMCS rmcs;
    u32 year, mon, day;

    rm_clear( &rmcs );
    dos( &rmcs, 0x2A00 );
    year = rmcs.ecx & 0xFFFF;
    mon = (rmcs.edx >> 8) & 0xFF;
    day = rmcs.edx & 0xFF;
    *dos_date = (u16)(((year - 1980) << 9) | (mon << 5) | day);
    rm_clear( &rmcs );
    dos( &rmcs, 0x2C00 );
    *dos_time = (u16)((((rmcs.ecx >> 8) & 0xFF) << 11) |
                      ((rmcs.ecx & 0xFF) << 5) |
                      (((rmcs.edx >> 8) & 0xFF) >> 1));
}

int sys_upcase( int ch )
{
    RMCS rmcs;

    rm_clear( &rmcs );
    rmcs.edx = (u32)ch & 0xFF;
    dos( &rmcs, 0x6520 );
    return RCF( rmcs ) ? ch : (int)(rmcs.edx & 0xFF);
}
