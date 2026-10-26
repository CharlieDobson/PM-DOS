/*
 * MEM.C - the memory DEBUG's commands read and write.
 *
 * MODE_16: a real-mode megabyte.  seg:off is seg*16+off; the work area
 * is DEBUG's own buffer standing at work_seg, and everything else is
 * the machine's first megabyte, read through 21h/F1h AL=20h.  Nothing
 * outside the work area is written: the kernel offers no way to, and a
 * native program has no business changing the vector table or a DOS
 * box's memory behind the kernel's back.
 *
 * MODE_NAT: the held program's slot, when the segment is one of its two
 * selectors - read and written through 21h/F1h AL=29h, which refuses
 * the guard page at offsets 0 to FFFh and anything past the slot's end
 * - and the first megabyte, read only, for any other segment.
 */
#include "debug.h"

#define LOW_TOP     0x100000UL

static int is_slot( ADDR addr )
{
    return mode == MODE_NAT && ( addr.seg == SEL_PROG_CS || addr.seg == SEL_PROG_DS );
}

int mem_is_low( ADDR addr )
{
    return !is_slot( addr );
}

u32 mem_seg_end( ADDR addr )
{
    if ( is_slot( addr ) ) {
        return cpu.limit;
    }
    return 0x10000UL;
}

/* ADDR moved on by N.  In the first megabyte the offset goes round at
   64K as an 8086's does, which is what an L 0 range and C's and M's
   second addresses do in 6.22; a slot has no such wrap. */
void addr_add( ADDR *addr, u32 n )
{
    addr->off += n;
    if ( !is_slot( *addr ) ) {
        addr->off &= 0xFFFF;
    }
}

/* the work area's offset of LEN bytes at real-mode linear LIN, or -1
   when they are not all inside it */
static long work_at( u32 lin, u32 len )
{
    u32 base = (u32)work_seg * 16;

    if ( mode != MODE_16 || lin < base || lin - base > work_len || len > work_len - ( lin - base ) ) {
        return -1;
    }
    return (long)( lin - base );
}

int addr_width( ADDR addr )
{
    return is_slot( addr ) ? 13 : 9;
}

void out_addr( ADDR addr )
{
    out_hex( addr.seg, 4 );
    out_ch( ':' );
    out_hex( addr.off, is_slot( addr ) ? 8 : 4 );
}

/* LEN bytes at ADDR into BUF, and OK[i] = whether byte i could be read
   (OK may be NULL).  0 if every byte could. */
int mem_get( ADDR addr, u8 *buf, u32 len, u8 *ok )
{
    u32 i, lin, n, from, to;
    int bad = 0;

    if ( is_slot( addr ) ) {
        /* the part of it inside the slot, past the guard page */
        from = addr.off;
        to = addr.off + len;
        if ( from < SLOT_PSP ) from = SLOT_PSP;
        if ( to > cpu.limit ) to = cpu.limit;
        for ( i = 0; i < len; i++ ) {
            buf[i] = 0;
            if ( ok ) ok[i] = 0;
        }
        if ( from < to && sys_dbg_read( from, buf + ( from - addr.off ), to - from ) == 0 ) {
            if ( ok ) {
                for ( i = from - addr.off; i < to - addr.off; i++ ) ok[i] = 1;
            }
            return ( from == addr.off && to == addr.off + len ) ? 0 : 1;
        }
        return 1;
    }

    /* the megabyte: the work area out of the buffer, the rest from the
       machine, in as few pieces as the two allow */
    i = 0;
    while ( i < len ) {
        lin = (u32)addr.seg * 16 + ( ( addr.off + i ) & 0xFFFF );
        if ( mode == MODE_16 && lin >= (u32)work_seg * 16 && lin < (u32)work_seg * 16 + work_len ) {
            n = (u32)work_seg * 16 + work_len - lin;
            if ( n > len - i ) n = len - i;
            mem_cpy( buf + i, work_buf + ( lin - (u32)work_seg * 16 ), n );
            if ( ok ) mem_set( ok + i, 1, n );
        } else if ( lin < LOW_TOP ) {
            n = LOW_TOP - lin;
            if ( n > len - i ) n = len - i;
            if ( mode == MODE_16 && lin < (u32)work_seg * 16 && lin + n > (u32)work_seg * 16 ) {
                n = (u32)work_seg * 16 - lin;
            }
            if ( sys_low_read( lin, buf + i, n ) == 0 ) {
                if ( ok ) mem_set( ok + i, 1, n );
            } else {
                mem_set( buf + i, 0, n );
                if ( ok ) mem_set( ok + i, 0, n );
                bad = 1;
            }
        } else {
            n = 1;                      /* past the megabyte: the HMA */
            buf[i] = 0;
            if ( ok ) ok[i] = 0;
            bad = 1;
        }
        i += n;
    }
    return bad;
}

int mem_read( ADDR addr, u8 *buf, u32 len )
{
    return mem_get( addr, buf, len, NULL );
}

/* 0, or an error after nothing was written.  In the first megabyte LEN
   is 64K at most, and what runs past the segment's end goes on at its
   offset 0, as mem_get reads it. */
int mem_write( ADDR addr, const u8 *buf, u32 len )
{
    u32 off, first;
    long at1, at2;

    if ( len == 0 ) {
        return 0;
    }
    if ( is_slot( addr ) ) {
        if ( addr.off < SLOT_PSP || addr.off + len > cpu.limit || addr.off + len < addr.off ) {
            return ERR_ACCESS;
        }
        return sys_dbg_write( addr.off, buf, len );
    }
    if ( len > 0x10000UL ) {
        return ERR_ACCESS;
    }
    off = addr.off & 0xFFFF;
    first = 0x10000UL - off;
    if ( first > len ) {
        first = len;
    }
    at1 = work_at( (u32)addr.seg * 16 + off, first );
    at2 = first < len ? work_at( (u32)addr.seg * 16, len - first ) : 0;
    if ( at1 < 0 || at2 < 0 ) {
        return ERR_ACCESS;
    }
    mem_cpy( work_buf + at1, buf, first );
    if ( first < len ) {
        mem_cpy( work_buf + at2, buf + first, len - first );
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the work area's PSP                                                 */
/* ------------------------------------------------------------------ */

/* DOS's PSP for a program loaded at work_seg, so that a .COM written
   here finds what it would find if it were run: INT 20h at 0, the top
   of its memory at 2, the vectors of 22h-24h, a handle table, the far
   call to DOS at 50h, two blank FCBs and an empty tail. */
void work_reset( void )
{
    u8 *psp = work_buf;
    u32 top;
    int i;

    mem_set( work_buf, 0, work_len );
    top = (u32)work_seg + work_len / 16;
    if ( top > 0xA000UL ) top = 0xA000UL;
    psp[0x00] = 0xCD;
    psp[0x01] = 0x20;
    psp[0x02] = (u8)top;
    psp[0x03] = (u8)( top >> 8 );
    psp[0x05] = 0x9A;                   /* CALL F01D:FEF0, as DOS writes it */
    psp[0x06] = 0xF0;
    psp[0x07] = 0xFE;
    psp[0x08] = 0x1D;
    psp[0x09] = 0xF0;
    sys_low_read( 0x22 * 4, psp + 0x0A, 12 );       /* 22h, 23h, 24h */
    psp[0x16] = (u8)work_seg;           /* its own parent, as the shell's */
    psp[0x17] = (u8)( work_seg >> 8 );
    psp[0x18] = 0x01;                   /* the handle table: CON, CON, */
    psp[0x19] = 0x01;                   /* CON, AUX, PRN, and free */
    psp[0x1A] = 0x01;
    psp[0x1B] = 0x00;
    psp[0x1C] = 0x02;
    for ( i = 0x1D; i < 0x2C; i++ ) {
        psp[i] = 0xFF;
    }
    psp[0x32] = 0x14;                   /* 20 handles, at PSP:0018 */
    psp[0x34] = 0x18;
    psp[0x36] = (u8)work_seg;
    psp[0x37] = (u8)( work_seg >> 8 );
    psp[0x38] = 0xFF;
    psp[0x39] = 0xFF;
    psp[0x3A] = 0xFF;
    psp[0x3B] = 0xFF;
    psp[0x50] = 0xCD;                   /* INT 21h, RETF */
    psp[0x51] = 0x21;
    psp[0x52] = 0xCB;
    mem_set( psp + 0x5D, ' ', 11 );
    mem_set( psp + 0x6D, ' ', 11 );
    psp[0x81] = 0x0D;
}
