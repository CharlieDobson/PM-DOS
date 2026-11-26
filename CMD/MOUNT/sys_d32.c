/*
 * SYS_D32.C - MOUNT's system calls, PM-DOS native.
 *
 * MOUNT.EXE is a D32 image (DOSINT.ASM with -dD32), loaded at offset
 * 2000h of a program slot and run at ring 3 with every pointer a 32-bit
 * offset within the slot.  INT 21h is the real-mode interface widened
 * to 32 bits: pointers in EDX, ESI, EDI and EBX, counts in ECX, CF and
 * AX for errors.  48h takes BYTES in EBX and returns the block's offset
 * in EAX; 42h takes the offset in ECX and returns the position in EAX.
 *
 * The image calls are 21h/F1h AL=0Fh..11h and 2Bh (DOS\IMAGE.INC).
 * They are what a native program has instead of a pointer into the
 * kernel: it runs at ring 3 and cannot read the volume table.
 */
#include "rt.h"
#include "sys.h"

typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi;
    uint32_t flags;             /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void mount_main( void );

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)

#define TAIL_LEN    300
#define IO_PIECE    32768u      /* the most one 3Fh or 40h is asked for */

#define IMEM_LOAD   0           /* F1h AL=2Bh, bits 16-23 of EAX */
#define IMEM_COPY   1

static uint8_t *psp;
static char    *tail;           /* TAIL_LEN, from 48h */
static int      lfn_ok = 1;

void __cdecl dos_entry( void )
{
    mount_main();
}

static void call21( REGS *regs, uint32_t eax )
{
    regs->eax = eax;
    regs->flags = 0;
    int21( regs );
}

static void clear( REGS *regs )
{
    mem_set( regs, 0, sizeof( *regs ) );
}

/* CF with AX = 7100h: the long-name call is not there */
static int lfn_missing( REGS *regs )
{
    if ( CF( *regs ) && AX( *regs ) == 0x7100 ) {
        lfn_ok = 0;
        return 1;
    }
    return 0;
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
    unsigned int len;

    tail = (char *)sys_mem_alloc( TAIL_LEN );
    if ( tail == NULL ) {
        return 1;
    }
    clear( &regs );
    call21( &regs, 0x6200 );
    psp = (uint8_t *)regs.ebx;
    len = psp[0x80];
    if ( len > 126 ) {
        len = 126;
    }
    mem_cpy( tail, psp + 0x81, len );
    tail[len] = 0;

    /* does this DOS have the long-name calls?  Ask for the current
       directory the long way and see. */
    {
        char probe[PATH_MAX];

        clear( &regs );
        regs.esi = (uint32_t)probe;
        call21( &regs, 0x7147 );
        lfn_missing( &regs );
    }
    return 0;
}

const char *sys_cmdline( void )
{
    return tail;
}

void sys_write( int handle, const char *buf, unsigned int len )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (uint32_t)handle;
    regs.ecx = len;
    regs.edx = (uint32_t)buf;
    call21( &regs, 0x4000 );
}

int sys_stdin_is_console( void )
{
    REGS regs;

    clear( &regs );
    regs.ebx = H_IN;
    call21( &regs, 0x4400 );
    return !CF( regs ) && (regs.edx & 0x81) == 0x81;
}

/* Keys typed before the question was on the screen are not answers to
   it.  Only the keyboard is emptied: a file of answers is read as it
   stands. */
void sys_key_flush( void )
{
    REGS regs;

    if ( !sys_stdin_is_console() ) {
        return;
    }
    for ( ;; ) {
        clear( &regs );
        call21( &regs, 0x0B00 );
        if ( (regs.eax & 0xFF) == 0 ) {
            break;
        }
        clear( &regs );
        call21( &regs, 0x0700 );
    }
}

/* A keystroke for a question.  From the keyboard it is 08h - no echo,
   and ^C ends the program through the kernel's INT 23h - with an
   extended key read whole and answered as 0.  From a file it is the
   next byte. */
int sys_read_key( void )
{
    REGS regs;
    uint8_t ch;
    int key;

    if ( sys_stdin_is_console() ) {
        clear( &regs );
        call21( &regs, 0x0800 );
        key = (int)(regs.eax & 0xFF);
        if ( key == 0 ) {
            clear( &regs );
            call21( &regs, 0x0800 );
        }
        return key;
    }
    clear( &regs );
    regs.ebx = H_IN;
    regs.ecx = 1;
    regs.edx = (uint32_t)&ch;
    call21( &regs, 0x3F00 );
    if ( CF( regs ) || regs.eax == 0 ) {
        return -1;
    }
    return ch;
}

void sys_exit( int code )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x4C00 | ((uint32_t)code & 0xFF) );
}

/* ------------------------------------------------------------------ */
/* names and files                                                     */
/* ------------------------------------------------------------------ */

/* "out" has PATH_MAX bytes */
int sys_truename( const char *path, char *out, int form )
{
    REGS regs;

    out[0] = 0;
    if ( lfn_ok ) {
        clear( &regs );
        regs.ecx = (uint32_t)form;
        regs.esi = (uint32_t)path;
        regs.edi = (uint32_t)out;
        call21( &regs, 0x7160 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.esi = (uint32_t)path;
    regs.edi = (uint32_t)out;
    call21( &regs, 0x6000 );
    return result( &regs );
}

int sys_get_attr( const char *path, uint16_t *attr )
{
    REGS regs;

    *attr = 0;
    clear( &regs );
    regs.edx = (uint32_t)path;
    call21( &regs, 0x4300 );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *attr = (uint16_t)regs.ecx;
    return 0;
}

int sys_open( const char *path, int mode, uint32_t *handle )
{
    REGS regs;

    *handle = 0;
    clear( &regs );
    regs.edx = (uint32_t)path;
    call21( &regs, 0x3D00 | ((uint32_t)mode & 0xFF) );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *handle = AX( regs );
    return 0;
}

int sys_create( const char *path, uint32_t *handle )
{
    REGS regs;

    *handle = 0;
    clear( &regs );
    regs.edx = (uint32_t)path;
    call21( &regs, 0x3C00 );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *handle = AX( regs );
    return 0;
}

/* All of "len", a piece at a time.  *got short of "len" is the end of
   the file. */
int sys_read( uint32_t handle, void *buf, uint32_t len, uint32_t *got )
{
    REGS regs;
    uint32_t piece;

    *got = 0;
    while ( *got < len ) {
        piece = len - *got;
        if ( piece > IO_PIECE ) {
            piece = IO_PIECE;
        }
        clear( &regs );
        regs.ebx = handle;
        regs.ecx = piece;
        regs.edx = (uint32_t)((uint8_t *)buf + *got);
        call21( &regs, 0x3F00 );
        if ( CF( regs ) ) {
            return (int)AX( regs );
        }
        if ( regs.eax > piece ) {           /* never more than asked */
            return ERR_GENERAL;
        }
        *got += regs.eax;
        if ( regs.eax < piece ) {
            break;
        }
    }
    return 0;
}

/* All of "len", a piece at a time.  A piece that goes short is a full
   disk, as DOS reports one: fewer bytes and no error.  A length of 0
   cuts the file off where it is. */
int sys_write_file( uint32_t handle, const void *buf, uint32_t len, uint32_t *put )
{
    REGS regs;
    uint32_t piece;

    *put = 0;
    if ( len == 0 ) {
        clear( &regs );
        regs.ebx = handle;
        regs.edx = (uint32_t)buf;           /* a pointer even for no bytes: */
        call21( &regs, 0x4000 );            /* the kernel checks where EDX */
        return result( &regs );             /* points, and 0 is not in the slot */
    }
    while ( *put < len ) {
        piece = len - *put;
        if ( piece > IO_PIECE ) {
            piece = IO_PIECE;
        }
        clear( &regs );
        regs.ebx = handle;
        regs.ecx = piece;
        regs.edx = (uint32_t)((const uint8_t *)buf + *put);
        call21( &regs, 0x4000 );
        if ( CF( regs ) ) {
            return (int)AX( regs );
        }
        if ( regs.eax > piece ) {
            return ERR_GENERAL;
        }
        *put += regs.eax;
        if ( regs.eax < piece ) {
            return ERR_DISKFULL;
        }
    }
    return 0;
}

int sys_seek( uint32_t handle, uint32_t pos, int how, uint32_t *where )
{
    REGS regs;

    *where = 0;
    clear( &regs );
    regs.ebx = handle;
    regs.ecx = pos;
    call21( &regs, 0x4200 | ((uint32_t)how & 0xFF) );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *where = regs.eax;
    return 0;
}

int sys_close( uint32_t handle )
{
    REGS regs;

    clear( &regs );
    regs.ebx = handle;
    call21( &regs, 0x3E00 );
    return result( &regs );
}

/* The country's thousands separator (38h, offset 7), so a size reads
   the way DIR's do.  A comma when the call fails. */
int sys_thousands( void )
{
    REGS regs;
    uint8_t info[48];                       /* CHKDSK's room for it */

    mem_set( info, 0, sizeof( info ) );
    clear( &regs );
    regs.edx = (uint32_t)info;
    call21( &regs, 0x3800 );
    if ( CF( regs ) || info[7] == 0 ) {
        return ',';
    }
    return info[7];
}

/* now, as a ZIP header wants it */
void sys_now( uint16_t *dos_date, uint16_t *dos_time )
{
    REGS regs;
    uint32_t year, month, day;

    clear( &regs );
    call21( &regs, 0x2A00 );
    year = regs.ecx & 0xFFFF;
    month = (regs.edx >> 8) & 0xFF;
    day = regs.edx & 0xFF;
    if ( year < 1980 ) {
        year = 1980;
    }
    *dos_date = (uint16_t)(((year - 1980) << 9) | (month << 5) | day);
    clear( &regs );
    call21( &regs, 0x2C00 );
    *dos_time = (uint16_t)((((regs.ecx >> 8) & 0xFF) << 11) | ((regs.ecx & 0xFF) << 5) |
                           (((regs.edx >> 8) & 0xFF) / 2));
}

/* ------------------------------------------------------------------ */
/* the kernel's image calls                                            */
/* ------------------------------------------------------------------ */

/* AL=0Fh: map a file on a FAT drive.  "letter" 0 = A:, -1 = the next
   free one.  *took = the letter it got. */
int sys_img_map( int letter, int ro, const char *path, int *took )
{
    REGS regs;

    *took = -1;
    clear( &regs );
    regs.ebx = (letter < 0 ? 0xFFu : (uint32_t)letter) | (ro ? 0x100u : 0u);
    regs.edx = (uint32_t)path;
    call21( &regs, 0xF10F );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *took = (int)(regs.eax & 0xFF);
    return 0;
}

/* AL=2Bh IMEM_LOAD: the kernel copies "len" bytes and keeps "path" to
   save them back to */
int sys_img_load( int letter, int ro, const void *data, uint32_t len,
                  const char *path, int *took )
{
    REGS regs;

    *took = -1;
    clear( &regs );
    regs.ebx = (letter < 0 ? 0xFFu : (uint32_t)letter) | (ro ? 0x100u : 0u);
    regs.esi = (uint32_t)data;
    regs.ecx = len;
    regs.edx = (uint32_t)path;
    call21( &regs, 0xF12B | ((uint32_t)IMEM_LOAD << 16) );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *took = (int)(regs.eax & 0xFF);
    return 0;
}

/* AL=2Bh IMEM_COPY: a loaded image back out, and the write count it
   was taken at */
int sys_img_copy( int letter, void *buf, uint32_t room, uint32_t *len, uint32_t *gen )
{
    REGS regs;

    *len = 0;
    *gen = 0;
    clear( &regs );
    regs.ebx = (uint32_t)letter;
    regs.edi = (uint32_t)buf;
    regs.ecx = room;
    call21( &regs, 0xF12B | ((uint32_t)IMEM_COPY << 16) );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *len = regs.ecx;
    *gen = regs.edx;
    return 0;
}

/* AL=10h, with BH = UM_* */
int sys_img_unmount( int letter, int mode, uint32_t gen )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (uint32_t)letter | ((uint32_t)mode << 8);
    regs.edx = gen;
    call21( &regs, 0xF110 );
    return result( &regs );
}

/* AL=11h: slot 0..IMG_MAX-1 */
int sys_img_report( int slot, REPORT *rep )
{
    REGS regs;

    mem_set( rep, 0, sizeof( *rep ) );
    clear( &regs );
    regs.ebx = (uint32_t)slot;
    regs.edi = (uint32_t)rep;
    call21( &regs, 0xF111 );
    rep->path[sizeof( rep->path ) - 1] = 0;
    return result( &regs );
}

/* AL=1, which speaks 1-based drives */
int sys_drive_type( int letter )
{
    REGS regs;

    clear( &regs );
    regs.edx = (uint32_t)letter + 1;
    call21( &regs, 0xF101 );
    if ( CF( regs ) ) {
        return -1;
    }
    return (int)(regs.eax & 0xFF);
}

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

void *sys_mem_alloc( unsigned int size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = size;                        /* bytes, on PM-DOS */
    call21( &regs, 0x4800 );
    return CF( regs ) ? NULL : (void *)regs.eax;
}
