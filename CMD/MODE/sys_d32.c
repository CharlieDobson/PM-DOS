/*
 * SYS_D32.C - MODE's system calls, PM-DOS native.
 *
 * MODE.EXE is a D32 image (DOSINT.ASM with -dD32), loaded at offset
 * 2000h of a program slot and run at ring 3 with every pointer a
 * 32-bit offset within the slot.  INT 21h is the real-mode interface
 * widened to 32 bits: pointers in EDX, ESI and EDI, counts in ECX, CF
 * and AX for errors.  48h takes bytes and returns the block's offset.
 *
 * WHAT MS-DOS'S MODE DID WITH THE BIOS, THIS ONE ASKS THE KERNEL FOR:
 *
 *   - a text mode, its scan lines and its font, and the equipment
 *     word's display bits: 21h/F0h AL=0Ch (fF0_mode, DOS\INT21.INC)
 *   - the keyboard's repeat: 21h/F0h AL=0Dh, the call KEYRATE= uses
 *   - a printer sent to a serial port, and RETRY=: 21h/F1h AL=28h.
 *     MS-DOS's MODE left a resident part on INT 17h and INT 14h for
 *     these; on PM-DOS the kernel's printer and serial devices keep
 *     the table and follow it, for a DOS box's INT 17h and INT 14h too
 *   - a byte to a printer, for COLS= and LINES=: the same call
 *   - the BIOS data area, the adapter and the RAM at B000h and B800h:
 *     21h/F1h AL=20h and AL=21h, the calls PMD reads the machine with
 *
 * A serial port's speed and format are written to its UART directly,
 * through IN and OUT, which the kernel carries out for a native
 * program.  MS-DOS's MODE used INT 14h, which cannot go past 9600 on
 * an AT's BIOS and has no mark or space parity; the chip can.
 */
#include "rt.h"
#include "sys.h"
#include "mode.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)

#define TAIL_MAX    130

#define F0( al )            (0xF000UL | (al))
#define F1( al, fn )        (0xF100UL | (al) | ((u32)(fn) << 16))

#define LMF_READ    0
#define LMF_RAM     1

#define MDF_GET     0
#define MDF_ROUTE   1
#define MDF_RETRY   2
#define MDF_LPTOUT  3

static u8  *psp;
static char tail[TAIL_MAX];

void __cdecl dos_entry( void )
{
    mode_main();
}

static void call21( REGS *regs, u32 eax )
{
    regs->eax = eax;
    regs->flags = 0;
    int21( regs );
}

static void clear( REGS *regs )
{
    mem_set( regs, 0, sizeof( *regs ) );
}

static int result( REGS *regs )
{
    return CF( *regs ) ? (int)AX( *regs ) : 0;
}

/* ------------------------------------------------------------------ */
/* the program and its command line                                    */
/* ------------------------------------------------------------------ */

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

void sys_exit( int code )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x4C00 | ((u32)code & 0xFF) );
}

/* ------------------------------------------------------------------ */
/* low memory and the BIOS                                             */
/* ------------------------------------------------------------------ */

static int low_read( u32 addr, void *buf, u32 len )
{
    REGS regs;

    clear( &regs );
    regs.ebx = addr;
    regs.ecx = len;
    regs.edi = (u32)buf;
    call21( &regs, F1( 0x20, LMF_READ ) );
    return result( &regs );
}

u16 sys_low_word( u32 addr )
{
    u16 word = 0;

    low_read( addr, &word, 2 );
    return word;
}

u8 sys_low_byte( u32 addr )
{
    u8 byte = 0;

    low_read( addr, &byte, 1 );
    return byte;
}

int sys_low_ram( u32 addr )
{
    REGS regs;

    clear( &regs );
    regs.ebx = addr;
    call21( &regs, F1( 0x20, LMF_RAM ) );
    return !CF( regs ) && regs.eax == 1;
}

int sys_bios( int intno, BQ *bq )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)intno;
    regs.esi = (u32)bq;
    call21( &regs, F1( 0x21, 0 ) );
    return result( &regs );
}

/* ------------------------------------------------------------------ */
/* the screen and the keyboard                                         */
/* ------------------------------------------------------------------ */

void sys_scr_size( int *rows, int *cols )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F0( 0x03 ) );
    *rows = (int)((regs.edx >> 8) & 0xFF);
    *cols = (int)(regs.edx & 0xFF);
}

/* mode 0-3 or 7; scan 0 to leave the scan lines, 1-3 for 200, 350 or
   400; font8 for the 8x8 font afterwards; equip 10h, 20h or 30h for
   the equipment word's display bits first, 0 to leave them */
int sys_text_mode( int mode, int scan, int font8, int equip )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)(mode & 0xFF) | ((u32)scan << 8);
    regs.ecx = (u32)(font8 ? 1 : 0);
    if ( equip ) {
        regs.ecx |= (u32)(0x80 | equip) << 8;
    }
    call21( &regs, F0( 0x0C ) );
    return result( &regs );
}

int sys_equip( int bits )
{
    REGS regs;

    clear( &regs );
    regs.ebx = 0xFF;
    regs.ecx = (u32)(0x80 | bits) << 8;
    call21( &regs, F0( 0x0C ) );
    return result( &regs );
}

int sys_kb_rate( int byte )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)byte;
    call21( &regs, F0( 0x0D ) );
    return result( &regs );
}

/* ------------------------------------------------------------------ */
/* the printers and serial ports                                       */
/* ------------------------------------------------------------------ */

int sys_port_get( PORTSET *set )
{
    REGS regs;

    clear( &regs );
    call21( &regs, F1( 0x28, MDF_GET ) );
    mem_cpy( set->route, &regs.ebx, 4 );
    mem_cpy( set->lretry, &regs.ecx, 4 );
    mem_cpy( set->cretry, &regs.edx, 4 );
    if ( CF( regs ) ) {
        mem_set( set, 0, sizeof( *set ) );
    }
    return result( &regs );
}

int sys_port_route( int lpt, int com )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)lpt | ((u32)com << 8);
    call21( &regs, F1( 0x28, MDF_ROUTE ) );
    return result( &regs );
}

int sys_port_retry( int dev, int retry )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)dev | ((u32)retry << 8);
    call21( &regs, F1( 0x28, MDF_RETRY ) );
    return result( &regs );
}

int sys_lpt_out( int lpt, int byte, int *status )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)lpt | ((u32)(byte & 0xFF) << 8);
    call21( &regs, F1( 0x28, MDF_LPTOUT ) );
    *status = (int)(regs.eax & 0xFF);
    return result( &regs );
}

/* ------------------------------------------------------------------ */
/* files and devices                                                   */
/* ------------------------------------------------------------------ */

int sys_open( const char *name, int mode, int *handle )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)name;
    call21( &regs, 0x3D00 | (u32)(mode & 0xFF) );
    *handle = (int)AX( regs );
    return result( &regs );
}

void sys_close( int handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    call21( &regs, 0x3E00 );
}

int sys_read( int handle, void *buf, u32 len, u32 *got )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x3F00 );
    *got = CF( regs ) ? 0 : regs.eax;
    return result( &regs );
}

int sys_devinfo( int handle, u16 *info )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    call21( &regs, 0x4400 );
    *info = (u16)regs.edx;
    return result( &regs );
}

int sys_setinfo( int handle, u16 info )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    regs.edx = info & 0xFF;
    call21( &regs, 0x4401 );
    return result( &regs );
}

int sys_ioctl_write( int handle, const void *buf, u32 len )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = len;
    regs.edx = (u32)buf;
    call21( &regs, 0x4403 );
    return result( &regs );
}

int sys_genioctl( int handle, int cat, int func, void *block )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)handle;
    regs.ecx = ((u32)cat << 8) | (u32)func;
    regs.edx = (u32)block;
    call21( &regs, 0x440C );
    return result( &regs );
}

int sys_break( int set, int on )
{
    REGS regs;
    int was;

    clear( &regs );
    call21( &regs, 0x3300 );
    was = (int)(regs.edx & 0xFF);
    if ( set ) {
        clear( &regs );
        regs.edx = (u32)(on ? 1 : 0);
        call21( &regs, 0x3301 );
    }
    return was;
}

void *sys_mem_alloc( u32 size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = size;                        /* bytes, on PM-DOS */
    call21( &regs, 0x4800 );
    return CF( regs ) ? NULL : (void *)regs.eax;
}
