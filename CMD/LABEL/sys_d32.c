/*
 * SYS_D32.C - PM-DOS host (the production LABEL.EXE).
 *
 * No DOS extender and no C library: DOSINT.ASM holds the entry point
 * and the INT thunks, this file is every system call LABEL makes.
 *
 * THE PM-DOS NATIVE ABI
 * ---------------------
 * The same as CHKDSK's; its SYS_D32.C says it at more length.  LABEL.EXE
 * is a D32 image (DOSINT.ASM with -dD32), loaded at offset 2000h of a
 * 512K program slot and run at ring 3 with every pointer a 32-bit offset
 * within the slot.  INT 21h is the real-mode interface widened to 32
 * bits - pointers in EDX, ESI, EDI and EBX, counts in ECX, CF and AX
 * for errors - and where PM-DOS differs from DOS:
 *
 *   - 21h/48h takes BYTES in EBX, not paragraphs, and returns the
 *     block's offset in EAX.
 *   - 21h/7302h is DOS 7.1's (DL = drive, DOS 7.1's extended DPB) since
 *     PM-DOS v0.60, which named the drive with a path until then.
 *   - 21h/4409h never reports a drive as remote; 21h/F101h names the
 *     filesystem instead, and anything but FAT is a CD-ROM here.
 *   - 21h/7305h fails with DOS's extended errors, 13h-1Fh, since PM-DOS
 *     v0.60 (0Bh or 08h before, so write-protect read as a general
 *     failure).  PM-DOS always has 7305h, and its INT 25h/26h have no
 *     packet form, so there is no fallback to them.
 *
 * Functions used: 0Ah 0C08h 19h 2Ah 2Ch 3Fh 40h 4400h 4409h 440Dh
 * (084Ah/086Ah, 484Ah/486Ah) 48h 4Ch 60h 62h 6520h 710Dh 7302h 7305h
 * F101h.  PM-DOS answers 440Dh and 710Dh with "invalid function", which
 * is taken as "no lock needed" and followed by 0Dh respectively.  Each
 * call is isolated in one small function below, so adapting to a
 * different kernel convention means changing only that function.
 */
#include "label.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)

static char  tail[260];

void __cdecl dos_entry( void )
{
    label_main();
}

static void call21( REGS *regs, u32 eax )
{
    regs->eax = eax;
    int21( regs );
}

static void clear( REGS *regs )
{
    mem_set( regs, 0, sizeof( *regs ) );
}

int sys_init( void )
{
    REGS regs;
    u8 *psp;
    u32 len;

    clear( &regs );
    call21( &regs, 0x6200 );
    psp = (u8 *)regs.ebx;
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
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x4000 );
}

static int stdin_is_device( void )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 0;
    call21( &regs, 0x4400 );
    return !CF( regs ) && (regs.edx & 0x80);
}

/* one byte of redirected input; -1 at the end */
static int read_byte( void )
{
    REGS regs;
    u8 ch;

    clear( &regs );
    regs.ebx = 0;
    regs.ecx = 1;
    regs.edx = (u32)&ch;
    call21( &regs, 0x3F00 );
    if ( CF( regs ) || regs.eax == 0 ) {
        return -1;
    }
    return ch;
}

int sys_read_line( char *buf, int max )
{
    REGS regs;
    u8 kb[258];
    int len = 0, ch;

    if ( stdin_is_device() ) {                  /* DOS echoes the line */
        kb[0] = (u8)(max > 255 ? 255 : max);
        kb[1] = 0;
        clear( &regs );
        regs.edx = (u32)kb;
        call21( &regs, 0x0A00 );
        len = kb[1];
        if ( len > max - 1 ) {
            len = max - 1;
        }
        mem_cpy( buf, kb + 2, (u32)len );
        buf[len] = 0;
        sys_write( H_OUT, "\n", 1 );            /* DOS echoed only CR */
        return len;
    }
    for ( ;; ) {                                /* redirected input */
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

/* a keystroke, as the DOS message services read a Y/N answer (0C08h);
   redirected input skips line ends */
int sys_read_key( void )
{
    REGS regs;
    char ch;
    int key;

    if ( stdin_is_device() ) {
        clear( &regs );
        call21( &regs, 0x0C08 );        /* flush type-ahead, no echo */
        key = (int)(regs.eax & 0xFF);
        if ( key == 0 ) {               /* extended key: drop the scan code */
            clear( &regs );
            call21( &regs, 0x0800 );
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

    clear( &regs );
    call21( &regs, 0x4C00 | ((u32)code & 0xFF) );
}

int sys_get_drive( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x1900 );
    return (int)(regs.eax & 0xFF);
}

int sys_drive_type( int drv )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)drv + 1;
    call21( &regs, 0x4409 );
    if ( CF( regs ) ) {
        return AX( regs ) == 1 ? DRV_LOCAL : DRV_INVALID;
    }
    if ( regs.edx & 0x8000 ) {
        return DRV_SUBST;
    }
    if ( regs.edx & 0x1000 ) {
        return DRV_REMOTE;
    }
    /* PM-DOS reads a CD-ROM itself instead of through a redirector, so
       4409h calls it local.  F101h answers 12, 16 or 32 for a FAT volume
       and 0 for ISO9660 or UDF - a CD-ROM, or an .ISO given a letter by
       MOUNT - and that gets DOS's answer for a CD-ROM.  A drive F101h
       cannot mount at all is left for the boot sector read to refuse. */
    clear( &regs );
    regs.edx = (u32)drv + 1;
    call21( &regs, 0xF101 );
    if ( !CF( regs ) && (regs.eax & 0xFF) == 0 ) {
        return DRV_REMOTE;
    }
    return DRV_LOCAL;
}

int sys_truename_drive( int drv )
{
    REGS regs;
    char in[4], out[130];

    in[0] = (char)('A' + drv);
    in[1] = ':';
    in[2] = '\\';
    in[3] = 0;
    out[0] = 0;
    clear( &regs );
    regs.esi = (u32)in;
    regs.edi = (u32)out;
    call21( &regs, 0x6000 );
    if ( CF( regs ) || out[1] != ':' ) {
        return -1;
    }
    return ch_upper( out[0] ) - 'A';
}

/*
 * 21h/7302h, as DOS 7.1 has it: the drive in DL (1 = A:), the buffer at
 * EDI and its size in ECX, and back comes DOS 7.1's extended DPB behind
 * a length word - which bpb_from_dpb() in FAT.C reads for the DOS/32A
 * build too.  Until PM-DOS v0.60 this call took a path and returned a
 * layout of PM-DOS's own, and this file carried a decoder for it.
 */
int sys_get_bpb( int drv, u8 *sector )
{
    REGS regs;
    u8 buf[0x40];

    mem_set( buf, 0, sizeof( buf ) );
    WR16( buf, 0x3F );
    clear( &regs );
    regs.edx = (u32)drv + 1;
    regs.ecx = 0x3F;
    regs.edi = (u32)buf;
    call21( &regs, 0x7302 );
    if ( CF( regs ) ) {
        return -1;
    }
    bpb_from_dpb( buf + 2, sector );
    return 0;
}

static int lock_call( int drv, u32 cx )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)drv + 1;                    /* BH = lock level 0 */
    regs.ecx = cx;
    regs.edx = 0;
    call21( &regs, 0x440D );
    if ( !CF( regs ) ) {
        return 0;
    }
    return AX( regs ) == 1 ? 1 : -1;            /* 1 = not supported */
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

static int disk_io( int drv, u32 lba, u32 count, void *buf, u32 mode )
{
    REGS regs;
    u8 pkt[10];

    WR32( pkt, lba );
    WR16( pkt + 4, count );
    WR32( pkt + 6, (u32)buf );
    clear( &regs );
    regs.ebx = (u32)pkt;
    regs.ecx = 0xFFFFFFFFul;
    regs.edx = (u32)drv + 1;
    regs.esi = mode;
    regs.flags = 1;
    call21( &regs, 0x7305 );
    return CF( regs ) ? ext_error( AX( regs ) ) : 0;
}

int sys_read_sec( int drv, u32 lba, u32 count, u32 bps, void *buf )
{
    (void)bps;
    return disk_io( drv, lba, count, buf, 0 );
}

int sys_write_sec( int drv, u32 lba, u32 count, u32 bps, const void *buf,
                   int kind )
{
    (void)bps;
    return disk_io( drv, lba, count, (void *)buf, 1u | ((u32)kind << 13) );
}

void sys_reset_drive( int drv )
{
    REGS regs;

    clear( &regs );
    regs.ecx = 1;                               /* flush and invalidate */
    regs.edx = (u32)drv + 1;
    call21( &regs, 0x710D );
    clear( &regs );
    call21( &regs, 0x0D00 );
}

void *sys_mem_alloc( u32 size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = size;                            /* bytes, on PM-DOS */
    call21( &regs, 0x4800 );
    return CF( regs ) ? NULL : (void *)regs.eax;
}

void sys_get_datetime( u16 *dos_date, u16 *dos_time )
{
    REGS regs;
    u32 year, mon, day;

    clear( &regs );
    call21( &regs, 0x2A00 );
    year = regs.ecx & 0xFFFF;
    mon = (regs.edx >> 8) & 0xFF;
    day = regs.edx & 0xFF;
    *dos_date = (u16)(((year - 1980) << 9) | (mon << 5) | day);
    clear( &regs );
    call21( &regs, 0x2C00 );
    *dos_time = (u16)((((regs.ecx >> 8) & 0xFF) << 11) |
                      ((regs.ecx & 0xFF) << 5) |
                      (((regs.edx >> 8) & 0xFF) >> 1));
}

/* 6520h: capitalize one character by the country table */
int sys_upcase( int ch )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)ch & 0xFF;
    call21( &regs, 0x6520 );
    return CF( regs ) ? ch : (int)(regs.edx & 0xFF);
}
