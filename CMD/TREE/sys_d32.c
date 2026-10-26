/*
 * SYS_D32.C - TREE's system calls, PM-DOS native.
 *
 * TREE.EXE is a D32 image (DOSINT.ASM with -dD32), loaded at offset
 * 2000h of a program slot (512K to start with; 48h grows it) and run
 * at ring 3 with every pointer a 32-bit offset within the slot.  INT
 * 21h is the real-mode interface widened to 32 bits: pointers in EDX,
 * ESI, EDI and EBX, counts in ECX, CF and AX for errors.  Where PM-DOS
 * differs from DOS:
 *
 *   - 48h takes BYTES in EBX and returns the block's offset in EAX.
 *   - PSP+2Ch holds the environment as a 32-bit offset in the slot.
 *   - 4Eh gives a volume label back whole and right-trimmed, with no
 *     dot after the eighth character; DOS splits it like a file name.
 *     Both are read here.
 *
 * THE LONG-NAME CALLS FIRST, as in XCOPY: every call that takes a name
 * tries its 71xxh form, and the first "7100h, not supported" answer -
 * which is what the kernel says with LFN=OFF in CONFIG.SYS - switches
 * the whole program to the classic calls.
 */
#include "rt.h"
#include "sys.h"
#include "tree.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl break_handler( void );
extern u8 break_hit;

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)

#define TAIL_MAX    300
#define FREC_MAX    320         /* 714Eh's record is 318 bytes */
#define LDTA_MAX    128

/* The buffers are one block from the heap, bought by sys_init(), so
   that they are not zeroes in the file (TREE.EXE goes on the full boot
   diskette). */
static u8   *psp;
static char *tail;              /* TAIL_MAX: the command line */
static int   lfn_ok = 1;
static u8   *find_rec;          /* FREC_MAX: 714Eh's record, or the DTA of 4Eh */
static u8   *label_dta;         /* LDTA_MAX: the volume label's search */

void __cdecl dos_entry( void )
{
    tree_main();
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
/* the program, its command line and its environment                   */
/* ------------------------------------------------------------------ */

int sys_init( void )
{
    REGS regs;
    const char *full;
    u32 len;
    int quoted;

    tail = (char *)xalloc( TAIL_MAX + FREC_MAX + LDTA_MAX );
    find_rec = (u8 *)tail + TAIL_MAX;
    label_dta = find_rec + FREC_MAX;

    clear( &regs );
    call21( &regs, 0x6200 );
    psp = (u8 *)regs.ebx;
    len = psp[0x80];
    if ( len > 126 ) {
        len = 126;
    }
    mem_cpy( tail, psp + 0x81, len );
    tail[len] = 0;

    /* A command line longer than the PSP holds: Windows 95's convention
       is a length of 7Fh and the whole line in CMDLINE, program name
       first.  Skip the name and take the rest - as much of it as fits,
       which is more than any path TREE can be given. */
    if ( psp[0x80] == 0x7F && (full = sys_getenv( "CMDLINE" )) != NULL ) {
        quoted = 0;
        while ( *full == ' ' || *full == '\t' ) {
            full++;
        }
        while ( *full && (quoted || (*full != ' ' && *full != '\t' && *full != '/')) ) {
            if ( *full == '"' ) {
                quoted = !quoted;
            }
            full++;
        }
        str_field( tail, full, TAIL_MAX, TAIL_MAX );
    }

    /* does this DOS have the long-name calls?  Ask for the current
       directory the long way and see. */
    {
        char probe[PATH_MAX];
        sys_get_cwd( sys_get_drive(), probe );
    }
    return 0;
}

const char *sys_cmdline( void )
{
    return tail;
}

const char *sys_getenv( const char *name )
{
    const char *env;
    u32 len = str_len( name );

    env = (const char *)*(u32 *)(psp + 0x2C);
    if ( env == NULL ) {
        return NULL;
    }
    while ( *env ) {
        if ( str_nicmp( env, name, len ) == 0 && env[len] == '=' ) {
            return env + len + 1;
        }
        env += str_len( env ) + 1;
    }
    return NULL;
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

void sys_break_install( void )
{
    REGS regs;

    clear( &regs );
    regs.edx = (u32)break_handler;
    call21( &regs, 0x2523 );
}

int sys_break_hit( void )
{
    return break_hit != 0;
}

/* ------------------------------------------------------------------ */
/* drives, directories and names                                       */
/* ------------------------------------------------------------------ */

int sys_get_drive( void )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x1900 );
    return (int)(regs.eax & 0xFF);
}

int sys_drive_valid( int drv )
{
    REGS regs;

    clear( &regs );
    regs.ebx = (u32)drv + 1;
    call21( &regs, 0x4409 );
    return !(CF( regs ) && AX( regs ) == ERR_BADDRIVE);
}

/* "buf" is PATH_MAX bytes: 7147h writes up to that, 4700h up to 64 */
int sys_get_cwd( int drv, char *buf )
{
    REGS regs;

    buf[0] = 0;
    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (u32)drv + 1;
        regs.esi = (u32)buf;
        call21( &regs, 0x7147 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)drv + 1;
    regs.esi = (u32)buf;
    call21( &regs, 0x4700 );
    return result( &regs );
}

int sys_get_attr( const char *path, u16 *attr )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = 0;
        regs.edx = (u32)path;
        call21( &regs, 0x7143 );
        if ( !lfn_missing( &regs ) ) {
            *attr = (u16)regs.ecx;
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0x4300 );
    *attr = (u16)regs.ecx;
    return result( &regs );
}

/* The label as the directory holds it: eleven characters, padded with
   blanks, which is how 6.22's TREE printed it (it read the label with
   an FCB search).  The search is the classic one with attribute 08h,
   which finds the label and nothing else. */
int sys_volume_label( int drv, char *label )
{
    REGS regs;
    char spec[8];
    const char *name;
    u32 pos, got = 0;

    spec[0] = (char)('A' + drv);
    spec[1] = ':';
    spec[2] = '\\';
    spec[3] = '*';
    spec[4] = '.';
    spec[5] = '*';
    spec[6] = 0;
    clear( &regs );
    regs.edx = (u32)label_dta;
    call21( &regs, 0x1A00 );
    clear( &regs );
    regs.ecx = ATTR_VOLUME;
    regs.edx = (u32)spec;
    call21( &regs, 0x4E00 );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    if ( !(label_dta[0x15] & ATTR_VOLUME) ) {
        return ERR_NOFILE;
    }
    name = (const char *)label_dta + 0x1E;  /* 13 bytes, 8.3 and a NUL */
    for ( pos = 0; pos < 13 && name[pos] && got < LABEL_LEN; pos++ ) {
        if ( name[pos] == '.' && got == 8 ) {
            continue;                       /* DOS's split, not the label's */
        }
        label[got++] = name[pos];
    }
    while ( got < LABEL_LEN ) {
        label[got++] = ' ';
    }
    label[LABEL_LEN] = 0;
    return 0;
}

/* 69h: the serial number in the boot sector's extended BPB.  A disk
   formatted before DOS 4, a CD and a network drive have none. */
int sys_volume_serial( int drv, u32 *serial )
{
    REGS regs;
    u8 info[32];                /* info level, serial, label, FS name: 25 */

    mem_set( info, 0, sizeof( info ) );
    clear( &regs );
    regs.ebx = (u32)drv + 1;
    regs.edx = (u32)info;
    call21( &regs, 0x6900 );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *serial = *(u32 *)(info + 2);
    return 0;
}

/* 6521h: "len" bytes in capitals, the country's way; a-z if refused */
void sys_upper( char *str, u32 len )
{
    REGS regs;
    u32 pos;

    if ( len == 0 ) {
        return;
    }
    clear( &regs );
    regs.ecx = len;
    regs.edx = (u32)str;
    call21( &regs, 0x6521 );
    if ( CF( regs ) ) {
        for ( pos = 0; pos < len; pos++ ) {
            str[pos] = (char)ch_upper( str[pos] );
        }
    }
}

/* ------------------------------------------------------------------ */
/* searching                                                           */
/* ------------------------------------------------------------------ */

static void rec_from_lfn( FINDREC *rec )
{
    rec->attr = find_rec[0];
    str_field( rec->name, (const char *)find_rec + 0x2C, 260, sizeof( rec->name ) );
}

static void rec_from_dta( FINDREC *rec )
{
    rec->attr = find_rec[0x15];
    str_field( rec->name, (const char *)find_rec + 0x1E, 13, sizeof( rec->name ) );
}

/* The classic search cannot ask for a REQUIRED attribute, so the
   directory-only searches filter here, as 714Eh would have. */
static u8 dta_required;

static int dta_skip( void )
{
    return (find_rec[0x15] & dta_required) != dta_required;
}

int sys_find_first( const char *pattern, u16 attrs, FINDREC *rec, u32 *handle )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ecx = attrs;                   /* CL allowed, CH required */
        regs.edx = (u32)pattern;
        regs.edi = (u32)find_rec;
        regs.esi = 1;                       /* times in DOS form */
        call21( &regs, 0x714E );
        if ( !lfn_missing( &regs ) ) {
            if ( CF( regs ) ) {
                return (int)AX( regs );
            }
            *handle = AX( regs );
            rec_from_lfn( rec );
            return 0;
        }
    }
    clear( &regs );
    regs.edx = (u32)find_rec;
    call21( &regs, 0x1A00 );
    dta_required = (u8)(attrs >> 8);
    clear( &regs );
    regs.ecx = attrs & 0xFF;
    regs.edx = (u32)pattern;
    call21( &regs, 0x4E00 );
    if ( CF( regs ) ) {
        return (int)AX( regs );
    }
    *handle = 0;
    if ( dta_skip() ) {
        return sys_find_next( 0, rec );
    }
    rec_from_dta( rec );
    return 0;
}

int sys_find_next( u32 handle, FINDREC *rec )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = handle;
        regs.edi = (u32)find_rec;
        regs.esi = 1;
        call21( &regs, 0x714F );
        if ( CF( regs ) ) {
            return (int)AX( regs );
        }
        rec_from_lfn( rec );
        return 0;
    }
    do {
        clear( &regs );
        call21( &regs, 0x4F00 );
        if ( CF( regs ) ) {
            return (int)AX( regs );
        }
    } while ( dta_skip() );
    rec_from_dta( rec );
    return 0;
}

void sys_find_close( u32 handle )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = handle;
        call21( &regs, 0x71A1 );
    }
}

/* ------------------------------------------------------------------ */
/* the rest                                                            */
/* ------------------------------------------------------------------ */

void *sys_mem_alloc( u32 size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = size;                        /* bytes, on PM-DOS */
    call21( &regs, 0x4800 );
    return CF( regs ) ? NULL : (void *)regs.eax;
}
