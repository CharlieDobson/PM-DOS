/*
 * XSYS.H - what XARC's C library (LIBC.C, LIBIO.C), its platform file
 * (PMDOSPLT.C) and the program share about PM-DOS: the integer types,
 * the INT 21h and INT 33h thunks in DOSINT.ASM, and what the critical
 * error handler there wrote down.
 *
 * XARC.EXE is a D32 image loaded at offset 2000h of a program slot and
 * run at ring 3.  INT 21h is the real-mode interface widened to 32 bits:
 * a pointer is a flat offset in a register (DS:EDX for DS:DX), a count is
 * a whole register, and 42h takes the offset in ECX and answers in EAX.
 */
#ifndef XSYS_H
#define XSYS_H

/* u32 is the backend's own (ARC\SHA.H), which an "unsigned long" here
   would collide with: both are 32 bits, but they are not one type */
typedef unsigned char    u8;
typedef unsigned short   u16;
typedef unsigned int     u32;
typedef int              s32;
typedef unsigned __int64 u64;
#define NST_U32_DEFINED

typedef struct {
    u32 eax, ebx, ecx, edx, esi, edi;
    u32 flags;                  /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl int33( REGS *regs );
void __cdecl break_handler( void );
void __cdecl crit_handler( void );

#define CF( regs )      ((regs).flags & 1)
#define AX( regs )      ((regs).eax & 0xFFFF)

/* DOSINT.ASM's notes on the last critical errors (UI.C asks about them) */
extern u32  crit_hits;
extern u16  crit_code;          /* DI: 00h write-protected, 02h not ready... */
extern u8   crit_drive;         /* AL: 0 = A: */
extern u8   crit_flags;         /* AH: bit 0 a write, bit 7 not a disk */
extern char break_hit;

/* INT 21h with EAX = "func" and the other registers as "regs" has them;
   0, or the error code the kernel gave (31 when it gave none) */
int   xa_dos( REGS *regs, u32 func );
void  xa_clear( REGS *regs );

/* the program, from LIBIO.C */
extern u8  *xa_psp;
extern int  xa_lfn;             /* the 71h long-name calls are there */

/* little-endian fields of on-disk and kernel records */
#define RD16( p )       ((u16)(((const u8 *)(p))[0] | ((u16)((const u8 *)(p))[1] << 8)))
#define RD32( p )       ((u32)RD16( p ) | ((u32)RD16( (const u8 *)(p) + 2 ) << 16))

/* 64-bit arithmetic without Watcom's run-time helpers, which XARC does
   not link: a 32x32 product, and a 64-bit number divided in place by a
   32-bit one, giving back the remainder */
u64   xa_mul32( u32 lhs, u32 rhs );
u32   xa_div64( u64 *num, u32 divisor );
u32   xa_lo32( u64 val );
u32   xa_hi32( u64 val );
u64   xa_make64( u32 hi, u32 lo );

/* run before exit: put the screen back, close what the program has open */
void  xa_on_exit( void (*fn)( void ) );

/* the program's command tail, after xa_crt_init has read the PSP */
void  xa_crt_init( void );
const char *xa_cmdtail( void );

/* the calendar (LIBIO.C): days since 1 January 1970 and back */
long  xa_days_from_civil( long year, int month, int day );
void  xa_civil_from_days( long days, int *year, int *month, int *day );

#endif
