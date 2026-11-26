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

#include <stdint.h>

typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi;
    uint32_t flags;             /* in: bit 0 = CF before the INT */
} REGS;

void __cdecl int21( REGS *regs );
void __cdecl int33( REGS *regs );
void __cdecl break_handler( void );
void __cdecl crit_handler( void );

#define CF( regs )      ((regs).flags & 1)
#define AX( regs )      ((regs).eax & 0xFFFF)

/* DOSINT.ASM's notes on the last critical errors (UI.C asks about them) */
extern uint32_t crit_hits;
extern uint16_t crit_code;      /* DI: 00h write-protected, 02h not ready... */
extern uint8_t  crit_drive;     /* AL: 0 = A: */
extern uint8_t  crit_flags;     /* AH: bit 0 a write, bit 7 not a disk */
extern char     break_hit;

/* INT 21h with EAX = "func" and the other registers as "regs" has them;
   0, or the error code the kernel gave (31 when it gave none) */
int   xa_dos( REGS *regs, uint32_t func );
void  xa_clear( REGS *regs );

/* the program, from LIBIO.C */
extern uint8_t *xa_psp;
extern int      xa_lfn;         /* the 71h long-name calls are there */

/* little-endian fields of on-disk and kernel records */
#define RD16( p )       ((uint16_t)(((const uint8_t *)(p))[0] | ((uint16_t)((const uint8_t *)(p))[1] << 8)))
#define RD32( p )       ((uint32_t)RD16( p ) | ((uint32_t)RD16( (const uint8_t *)(p) + 2 ) << 16))

/* 64-bit arithmetic without Watcom's run-time helpers, which XARC does
   not link: a 32x32 product, and a 64-bit number divided in place by a
   32-bit one, giving back the remainder */
uint64_t xa_mul32( uint32_t lhs, uint32_t rhs );
uint32_t xa_div64( uint64_t *num, uint32_t divisor );
uint32_t xa_lo32( uint64_t val );
uint32_t xa_hi32( uint64_t val );
uint64_t xa_make64( uint32_t hi, uint32_t lo );

/* run before exit: put the screen back, close what the program has open */
void  xa_on_exit( void (*fn)( void ) );

/* the program's command tail, after xa_crt_init has read the PSP */
void  xa_crt_init( void );
const char *xa_cmdtail( void );

/* the calendar (LIBIO.C): days since 1 January 1970 and back */
long  xa_days_from_civil( long year, int month, int day );
void  xa_civil_from_days( long days, int *year, int *month, int *day );

#endif
