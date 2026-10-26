/*
 * SYS_D32.C - DELTREE's system calls, PM-DOS native.
 *
 * DELTREE.EXE is a D32 image (DOSINT.ASM with -dD32), loaded at offset
 * 2000h of a program slot and run at ring 3 with every pointer a 32-bit
 * offset within the slot.  INT 21h is the real-mode interface widened
 * to 32 bits: pointers in EDX, ESI, EDI and EBX, counts in ECX, CF and
 * AX for errors.  48h takes BYTES in EBX and returns the block's offset
 * in EAX; PSP+2Ch holds the environment as a 32-bit offset in the slot.
 *
 * THE LONG-NAME CALLS FIRST, as in XCOPY.  The first "7100h, not
 * supported" answer - LFN=OFF in CONFIG.SYS - switches the whole
 * program to the classic calls, which on PM-DOS take long names anyway.
 *
 * THE DELETE IS THE ORDINARY ONE.  XCOPY and MOVE delete through F1h
 * AL=19h, which keeps nothing, because what they delete has just been
 * copied somewhere else.  What DELTREE deletes has not, so it goes the
 * way DEL's does: with UNDELETE=ON the kernel keeps it, and UNDELETE /S
 * brings a whole tree back, directories and all.
 */
#include "rt.h"
#include "sys.h"

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void deltree_main( void );

#define CF( regs )  ((regs).flags & 1)
#define AX( regs )  ((regs).eax & 0xFFFF)

#define TAIL_LEN    300
#define FIND_LEN    320

static u8   *psp;
static char *tail;              /* TAIL_LEN; from 48h, as MAIN.C's buffers */
static int   lfn_ok = 1;
static u8   *find_rec;          /* FIND_LEN: 714Eh's record, or 4Eh's DTA */

void __cdecl dos_entry( void )
{
    deltree_main();
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

    tail = (char *)sys_mem_alloc( TAIL_LEN + FIND_LEN );
    if ( tail == NULL ) {
        return 1;
    }
    find_rec = (u8 *)tail + TAIL_LEN;

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
       first.  Skip the name and take the rest. */
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
        str_cpyn( tail, full, TAIL_LEN );
    }

    /* does this DOS have the long-name calls?  Ask for the current
       directory the long way and see. */
    {
        char probe[PATH_MAX];

        clear( &regs );
        regs.esi = (u32)probe;
        call21( &regs, 0x7147 );
        lfn_missing( &regs );
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
   stands, one answer after another. */
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

/* one byte of redirected input; -1 at the end */
static int read_byte( void )
{
    REGS regs;
    u8 ch;

    clear( &regs );
    regs.ebx = H_IN;
    regs.ecx = 1;
    regs.edx = (u32)&ch;
    call21( &regs, 0x3F00 );
    if ( CF( regs ) || regs.eax == 0 ) {
        return -1;
    }
    return ch;
}

/* A keystroke for a question.  From the keyboard it is 08h - no echo,
   and ^C ends the program through the kernel's INT 23h - with an
   extended key read whole and answered as 0.  From a file it is the
   next byte, Enter included. */
int sys_read_key( void )
{
    REGS regs;
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
    return read_byte();
}

void sys_exit( int code )
{
    REGS regs;

    clear( &regs );
    call21( &regs, 0x4C00 | ((u32)code & 0xFF) );
}

/* ------------------------------------------------------------------ */
/* names, attributes, deleting                                         */
/* ------------------------------------------------------------------ */

int sys_truename( const char *path, char *out, int form )
{
    REGS regs;

    out[0] = 0;
    if ( lfn_ok ) {
        clear( &regs );
        regs.ecx = (u32)form;
        regs.esi = (u32)path;
        regs.edi = (u32)out;
        call21( &regs, 0x7160 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.esi = (u32)path;
    regs.edi = (u32)out;
    call21( &regs, 0x6000 );
    return result( &regs );
}

int sys_set_attr( const char *path, u16 attr )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.ebx = 1;
        regs.ecx = attr;
        regs.edx = (u32)path;
        call21( &regs, 0x7143 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.ecx = attr;
    regs.edx = (u32)path;
    call21( &regs, 0x4301 );
    return result( &regs );
}

int sys_delete( const char *path )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (u32)path;
        regs.esi = 0;                       /* the name, not a pattern */
        call21( &regs, 0x7141 );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0x4100 );
    return result( &regs );
}

int sys_rmdir( const char *path )
{
    REGS regs;

    if ( lfn_ok ) {
        clear( &regs );
        regs.edx = (u32)path;
        call21( &regs, 0x713A );
        if ( !lfn_missing( &regs ) ) {
            return result( &regs );
        }
    }
    clear( &regs );
    regs.edx = (u32)path;
    call21( &regs, 0x3A00 );
    return result( &regs );
}

/* ------------------------------------------------------------------ */
/* searching                                                           */
/* ------------------------------------------------------------------ */

static void rec_from_lfn( FINDREC *rec )
{
    rec->attr = find_rec[0];
    find_rec[0x2C + PATH_MAX - 1] = 0;      /* the kernel's record, but */
    find_rec[0x130 + 13] = 0;               /* never trust a terminator */
    str_cpyn( rec->name, (const char *)find_rec + 0x2C, sizeof( rec->name ) );
    str_cpyn( rec->alias, (const char *)find_rec + 0x130, sizeof( rec->alias ) );
    if ( rec->alias[0] == 0 ) {             /* Windows: the name IS 8.3 */
        str_cpyn( rec->alias, rec->name, sizeof( rec->alias ) );
    }
}

static void rec_from_dta( FINDREC *rec )
{
    rec->attr = find_rec[0x15];
    find_rec[0x1E + 12] = 0;
    str_cpyn( rec->name, (const char *)find_rec + 0x1E, 13 );
    str_cpyn( rec->alias, rec->name, sizeof( rec->alias ) );
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
/* memory                                                              */
/* ------------------------------------------------------------------ */

void *sys_mem_alloc( u32 size )
{
    REGS regs;

    clear( &regs );
    regs.ebx = size;                        /* bytes, on PM-DOS */
    call21( &regs, 0x4800 );
    return CF( regs ) ? NULL : (void *)regs.eax;
}
