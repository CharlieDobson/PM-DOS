/*=====================================================================
 * MOVEPROB.C - MOVEPROB.EXE: how much does a move to extended memory
 *              cost from a DOS box?
 *
 * A 16-bit DOS program for a Windows DOS box (and for a plain DOS
 * prompt, where the same numbers say what the machine itself does).
 * Two ways exist to move bytes between conventional and extended
 * memory from real or V86 mode, and RMCACHE.INC has to pick one for
 * the cache under WIN386: INT 15h AH=87h, the BIOS block move, which a
 * V86 monitor emulates; and XMS function 0Bh through the driver's
 * entry, which under WIN386 is V86MMGR's.  This times both, 2K at a
 * time, both directions, and prints microseconds a call from the
 * BIOS tick count - so 1000 calls or more, or the number is noise.
 *
 * Writes C:\MOVEPROB.TXT, because a DOS box's screen is gone when the
 * box closes.
 *
 * Built:  wcl -zq -bt=dos -ms (BUILD.CMD)
 *===================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <i86.h>

#define CHUNK   2048
#define ROUNDS  1000

static unsigned char far *tickp = (unsigned char far *)MK_FP( 0x40, 0x6C );
static unsigned char      buf[CHUNK + 16];
static unsigned char      buf2[CHUNK + 16];
static unsigned long      far_;
static void ( far *xmsEntry )( void );
static unsigned           xmsHandle;
static unsigned long      xmsPhys;

/* the descriptor table INT 15h AH=87h wants: six 8-byte descriptors */
static unsigned char gdt[48];

struct xmove {
    unsigned long len;
    unsigned      srcH;
    unsigned long srcO;
    unsigned      dstH;
    unsigned long dstO;
};
static struct xmove mv;

static FILE *out;

static void say( const char *text )
{
    fputs( text, out );
    fputs( "\r\n", out );
    fputs( text, stdout );
    fputs( "\n", stdout );
}

static unsigned long ticks( void )
{
    return *(unsigned long far *)tickp;
}

static int xmsPresent( void )
{
    union REGS r;
    struct SREGS s;
    r.x.ax = 0x4300;
    int86( 0x2F, &r, &r );
    if ( r.h.al != 0x80 ) return 0;
    r.x.ax = 0x4310;
    segread( &s );
    int86x( 0x2F, &r, &r, &s );
    xmsEntry = (void ( far * )( void ))MK_FP( s.es, r.x.bx );
    return 1;
}

/* AH = function, DX in, returns AX; BX and DX come back in *bx, *dx */
static unsigned xmsCall( unsigned char fn, unsigned dxIn, unsigned *bx, unsigned *dx )
{
    unsigned rax, rbx, rdx;
    _asm {
        mov     ah, fn
        mov     dx, dxIn
        call    dword ptr [xmsEntry]
        mov     rax, ax
        mov     rbx, bx
        mov     rdx, dx
    }
    *bx = rbx;
    *dx = rdx;
    return rax;
}

static unsigned xmsMove( void )
{
    unsigned rax;
    _asm {
        push    ds
        push    si
        mov     ah, 0Bh
        mov     si, offset mv
        call    dword ptr [xmsEntry]
        pop     si
        pop     ds
        mov     rax, ax
    }
    return rax;
}

static void setDesc( int which, unsigned long base, unsigned len )
{
    unsigned char *d = gdt + which * 8;
    d[0] = (unsigned char)( ( len - 1 ) & 0xFF );
    d[1] = (unsigned char)( ( len - 1 ) >> 8 );
    d[2] = (unsigned char)( base & 0xFF );
    d[3] = (unsigned char)( ( base >> 8 ) & 0xFF );
    d[4] = (unsigned char)( ( base >> 16 ) & 0xFF );
    d[5] = 0x93;
    d[6] = 0;
    d[7] = (unsigned char)( base >> 24 );
}

/* INT 15h AH=87h: CX words from the source descriptor to the destination */
static int biosMove( unsigned long src, unsigned long dst, unsigned bytes )
{
    union REGS r;
    struct SREGS s;
    memset( gdt, 0, sizeof( gdt ) );
    setDesc( 2, src, bytes );
    setDesc( 3, dst, bytes );
    r.h.ah = 0x87;
    r.x.cx = bytes / 2;
    r.x.si = FP_OFF( gdt );
    segread( &s );
    s.es = FP_SEG( gdt );
    int86x( 0x15, &r, &r, &s );
    return r.x.cflag ? -1 : r.h.ah;
}

static void report( const char *what, unsigned long t0, unsigned long t1, unsigned rounds, int bad )
{
    char line[120];
    unsigned long dt = t1 - t0;
    /* 18.2 ticks a second: a tick is 54945 us */
    unsigned long us = ( dt * 54945UL ) / rounds;
    sprintf( line, "%-34s %5lu ticks for %u calls = %6lu us a call%s",
             what, dt, rounds, us, bad ? "  (ERRORS)" : "" );
    say( line );
}

int main( void )
{
    unsigned bx, dx, ax;
    unsigned long bufFlat, t0, t1;
    unsigned i;
    int bad;
    char line[120];

    out = fopen( "C:\\MOVEPROB.TXT", "a" );
    if ( out == NULL ) out = stdout;
    say( "MOVEPROB - the cost of a 2K move to extended memory and back" );

    bufFlat = ( (unsigned long)FP_SEG( buf ) << 4 ) + FP_OFF( buf );
    sprintf( line, "buffer at %05lX, %u rounds of %u bytes", bufFlat, ROUNDS, CHUNK );
    say( line );

    if ( !xmsPresent() ) {
        say( "no XMS driver answers 2Fh/4300h" );
        return 1;
    }
    ax = xmsCall( 0x00, 0, &bx, &dx );
    sprintf( line, "XMS version %04X, driver %04X, entry at %04X:%04X", ax, bx,
             FP_SEG( xmsEntry ), FP_OFF( xmsEntry ) );
    say( line );

    ax = xmsCall( 0x09, 64, &bx, &dx );
    if ( ax != 1 ) {
        sprintf( line, "cannot get 64K of XMS: error %02X", bx & 0xFF );
        say( line );
        return 1;
    }
    xmsHandle = dx;
    ax = xmsCall( 0x0C, xmsHandle, &bx, &dx );
    if ( ax != 1 ) {
        /* V86MMGR refuses to lock for a V86 program - a physical
           address means nothing in there - so INT 15h is timed between
           two conventional buffers instead, which costs the same trip */
        sprintf( line, "cannot lock the block: error %02X (a monitor); 87h goes low to low", bx & 0xFF );
        say( line );
        xmsPhys = 0;
    } else {
        xmsPhys = ( (unsigned long)dx << 16 ) | bx;
    }
    sprintf( line, "64K block: handle %u at %08lX", xmsHandle, xmsPhys );
    say( line );

    for ( i = 0; i < CHUNK; i++ ) buf[i] = (unsigned char)( i * 7 + 3 );

    /* XMS 0Bh, out and back */
    mv.len = CHUNK;
    mv.srcH = 0;
    mv.srcO = ( (unsigned long)FP_SEG( buf ) << 16 ) | FP_OFF( buf );
    mv.dstH = xmsHandle;
    mv.dstO = 0;
    bad = 0;
    t0 = ticks();
    for ( i = 0; i < ROUNDS; i++ ) {
        if ( xmsMove() != 1 ) bad++;
    }
    t1 = ticks();
    report( "XMS 0Bh conventional -> extended", t0, t1, ROUNDS, bad );

    mv.srcH = xmsHandle;
    mv.srcO = 0;
    mv.dstH = 0;
    mv.dstO = ( (unsigned long)FP_SEG( buf ) << 16 ) | FP_OFF( buf );
    memset( buf, 0, CHUNK );
    bad = 0;
    t0 = ticks();
    for ( i = 0; i < ROUNDS; i++ ) {
        if ( xmsMove() != 1 ) bad++;
    }
    t1 = ticks();
    report( "XMS 0Bh extended -> conventional", t0, t1, ROUNDS, bad );
    for ( i = 0; i < CHUNK; i++ ) {
        if ( buf[i] != (unsigned char)( i * 7 + 3 ) ) break;
    }
    say( i == CHUNK ? "  and the bytes came back right" : "  BUT THE BYTES CAME BACK WRONG" );

    /* INT 15h 87h, out and back - or, with no physical address to go
       to, between the two conventional buffers */
    for ( i = 0; i < CHUNK; i++ ) buf[i] = (unsigned char)( i * 5 + 1 );
    far_ = xmsPhys ? xmsPhys + 4096
                   : ( (unsigned long)FP_SEG( buf2 ) << 4 ) + FP_OFF( buf2 );
    bad = 0;
    t0 = ticks();
    for ( i = 0; i < ROUNDS; i++ ) {
        if ( biosMove( bufFlat, far_, CHUNK ) != 0 ) bad++;
    }
    t1 = ticks();
    report( xmsPhys ? "INT 15h 87h conventional -> extended" : "INT 15h 87h low -> low", t0, t1, ROUNDS, bad );
    memset( buf, 0, CHUNK );
    bad = 0;
    t0 = ticks();
    for ( i = 0; i < ROUNDS; i++ ) {
        if ( biosMove( far_, bufFlat, CHUNK ) != 0 ) bad++;
    }
    t1 = ticks();
    report( xmsPhys ? "INT 15h 87h extended -> conventional" : "INT 15h 87h low -> low, back", t0, t1, ROUNDS, bad );
    for ( i = 0; i < CHUNK; i++ ) {
        if ( buf[i] != (unsigned char)( i * 5 + 1 ) ) break;
    }
    say( i == CHUNK ? "  and the bytes came back right" : "  BUT THE BYTES CAME BACK WRONG" );

    /* and a plain copy, for scale */
    t0 = ticks();
    for ( i = 0; i < ROUNDS; i++ ) {
        memcpy( buf, gdt, 48 );
        memmove( buf + 16, buf, CHUNK - 16 );
    }
    t1 = ticks();
    report( "a 2K memmove in conventional memory", t0, t1, ROUNDS, 0 );

    xmsCall( 0x0D, xmsHandle, &bx, &dx );
    xmsCall( 0x0A, xmsHandle, &bx, &dx );
    say( "MOVEPROB done." );
    if ( out != stdout ) fclose( out );
    return 0;
}
