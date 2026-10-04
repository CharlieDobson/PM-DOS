/*
 * SYS_D32.C - PM-DOS host (the production CHKDSK.EXE).
 *
 * No DOS extender and no C library: DOSINT.ASM holds the entry point
 * and the INT thunks, this file is every system call CHKDSK makes.
 *
 * THE PM-DOS NATIVE ABI
 * ---------------------
 * CHKDSK.EXE is a D32 image (DOSINT.ASM, assembled with -dD32, puts the
 * header in front).  PM-DOS loads it at offset 2000h of a program slot
 * (512K to start with; 48h grows it) and runs it at ring 3 with CS, DS,
 * ES and SS all based at the
 * slot, so every pointer is a plain 32-bit offset within it.  INT 21h
 * is the real-mode DOS register interface widened to 32 bits:
 *
 *   - Wherever real-mode DOS takes seg:reg for a pointer, the offset
 *     goes in the full 32-bit register (DS:DX -> EDX, DS:SI -> ESI,
 *     ES:DI -> EDI, DS:BX -> EBX).  Segment registers are not used.
 *   - Byte counts in CX become ECX.
 *   - CF set on return means failure, error code in AX, as in DOS.
 *   - The 21h/7305h disk packet keeps the DOS layout, but its buffer
 *     field (offset 6) is a 32-bit offset.  ECX = 0FFFFFFFFh.
 *   - 21h/62h returns the PSP's offset in EBX.  The command tail at 80h
 *     is as in DOS; the top of memory is a DWORD at PSP+4 (the first
 *     offset past the slot), not DOS's paragraph word at PSP+2.
 *   - 21h/48h takes BYTES in EBX, not paragraphs, and returns the
 *     block's offset in EAX; the block comes out of the slot, between
 *     the end of the image and the stack.
 *   - 21h/7302h is DOS 7.1's (DL = drive, DOS 7.1's extended DPB) since
 *     PM-DOS v0.60, which named the drive with a path until then.
 *   - 21h/4409h never reports a drive as remote.  21h/F101h (PM-DOS's
 *     own) names the filesystem instead, and a CD-ROM is the drive whose
 *     answer is not a FAT width.  See sys_drive_type().
 *   - INT 25h/26h take the sector, count and buffer in EDX, ECX and EBX
 *     and have no packet form, so they are no fallback for 7305h - which
 *     PM-DOS always has.
 *
 * Functions used: 0Ah 19h 2Ah 2Ch 3800h 3Fh 40h 4400h 4409h 440Dh
 * (084Ah/086Ah, 484Ah/486Ah) 47h 48h 4Ch 60h 62h 710Dh 7147h 7302h
 * 7305h F101h F11Dh.  PM-DOS answers 440Dh and 710Dh with "invalid
 * function": the first is taken as "no lock needed", which is what it
 * means there (the kernel serves one program at a time and flushes
 * round every 7305h), and the second is followed by 0Dh anyway.  Each
 * call is isolated in one small function below, so adapting to a
 * different kernel convention means changing only that function.
 */
#include "chkdsk.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)

static char  tail[260];
static u8   *psp;

void __cdecl dos_entry( void )
{
    chkdsk_main();
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

int sys_read_line( char *buf, int max )
{
    REGS regs;
    u8 kb[258];
    int len = 0;
    char ch;

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
        clear( &regs );
        regs.ebx = 0;
        regs.ecx = 1;
        regs.edx = (u32)&ch;
        call21( &regs, 0x3F00 );
        if ( CF( regs ) || regs.eax == 0 ) {
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
            buf[len++] = ch;
        }
    }
    buf[len] = 0;
    sys_write( H_OUT, buf, (u32)len );
    sys_write( H_OUT, "\r\n", 2 );
    return len;
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

int sys_get_cwd( int drv, char *buf, int max )
{
    REGS regs;
    char tmp[270];

    tmp[0] = 0;
    clear( &regs );
    regs.edx = (u32)drv + 1;
    regs.esi = (u32)tmp;
    regs.flags = 1;
    call21( &regs, 0x7147 );                    /* long path (DOS 7) */
    if ( CF( regs ) ) {
        clear( &regs );
        regs.edx = (u32)drv + 1;
        regs.esi = (u32)tmp;
        call21( &regs, 0x4700 );
        if ( CF( regs ) ) {
            return -1;
        }
    }
    if ( (int)str_len( tmp ) >= max ) {
        return -1;
    }
    str_cpy( buf, tmp );
    return 0;
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
    return CF( regs ) ? 1 : 0;
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

/* 21h/F1h AL=1Dh, VLF_CHECKED in bits 16-23: the volume may have its
   clean-shutdown bit back.  A drive without the bit ignores it. */
void sys_mark_checked( int drv )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)drv + 1;
    call21( &regs, 0x0001F11Dul );
}

void *sys_mem_alloc( u32 size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = size;                            /* bytes, on PM-DOS */
    call21( &regs, 0x4800 );
    return CF( regs ) ? NULL : (void *)regs.eax;
}

/* DOS CHKDSK arithmetic - the top of memory less where the PSP is - on
   PM-DOS's terms: the top is the dword at PSP+4, and it is the end of
   this program's slot, which is all the memory a program has there */
void sys_mem_info( u32 *total, u32 *avail )
{
    u32 top = RD32( psp + 4 );

    *total = top;
    *avail = top > (u32)psp ? top - (u32)psp : 0;
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

void sys_get_country( COUNTRY *ctry )
{
    REGS regs;
    u8 buf[48];

    mem_set( buf, 0, sizeof( buf ) );
    clear( &regs );
    regs.edx = (u32)buf;
    call21( &regs, 0x3800 );
    if ( CF( regs ) ) {
        return;
    }
    ctry->date_fmt = RD16( buf ) <= 2 ? RD16( buf ) : 0;
    if ( buf[7] ) {
        ctry->thou_sep = (char)buf[7];
    }
    if ( buf[11] ) {
        ctry->date_sep = (char)buf[11];
    }
    if ( buf[13] ) {
        ctry->time_sep = (char)buf[13];
    }
    ctry->time_24 = buf[17] & 1;
}
