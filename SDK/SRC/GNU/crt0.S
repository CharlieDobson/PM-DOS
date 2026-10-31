/*
 * CRT0.S - the start of every GCC, Clang and DJGPP program the SDK
 * builds: the D32 header, the entry point and the interrupt thunks.
 * GNU as syntax, through the C preprocessor.
 *
 * It is in the library.  The linker script names _d32_start with
 * EXTERN(), which brings this module in, and puts the .d32head section
 * first, so the header is the first 32 bytes of the image.
 *
 * The BSS is not in the file: the script works out its size, the
 * header gives it to the kernel, and the kernel zeroes it.  The stack
 * size is __d32_stack_size, 64K unless the link says otherwise with
 * --defsym=__d32_stack_size=<bytes>.
 *
 * DJGPP's compiler puts an underscore in front of every C name (COFF)
 * and the ELF compilers do not; CSYM() gives the name as the C side
 * spells it.
 *
 * The registers block for pm_int21 and pm_int33 (PMDOS.H):
 *
 *       +0 EAX  +4 EBX  +8 ECX  +12 EDX  +16 ESI  +20 EDI
 *       +24 FLAGS (bit 0 on input: the carry flag before the INT)
 */
#define CAT2( a, b )    a ## b
#define CAT( a, b )     CAT2( a, b )
#define CSYM( name )    CAT( __USER_LABEL_PREFIX__, name )

        .section .d32head, "ax"
        .byte   'C', 'D'                /* the signature */
        .byte   0xCD, 0x20              /* INT 20h, for real DOS */
        .byte   1, 0                    /* D32_VER, D32T_PROG */
        .word   0
        .long   0x2000                  /* SLOT_IMAGE */
        .long   CSYM( _d32_start )
        .long   CSYM( __d32_bss_size )
        .long   CSYM( __d32_stack_size )
        .long   0, 0

        .text
        .globl  CSYM( _d32_start )
CSYM( _d32_start ):
        push    %ds
        pop     %es
        cld
        and     $-16, %esp              /* what GCC's code assumes */
        call    CSYM( __pm_start )      /* does not come back */
        mov     $0x4CFF, %eax
        int     $0x21

#define INTCALL( name, num ) \
        .globl  name ; \
name: ; \
        push    %ebp ; \
        push    %ebx ; \
        push    %esi ; \
        push    %edi ; \
        mov     20(%esp), %eax ; \
        push    %eax ; \
        mov     %esp, saved_esp ; \
        mov     4(%eax), %ebx ; \
        mov     8(%eax), %ecx ; \
        mov     12(%eax), %edx ; \
        mov     16(%eax), %esi ; \
        mov     20(%eax), %edi ; \
        btl     $0, 24(%eax) ; \
        mov     (%eax), %eax ; \
        int     $num ; \
        mov     saved_esp, %esp ; \
        xchg    %eax, (%esp) ; \
        mov     %ebx, 4(%eax) ; \
        mov     %ecx, 8(%eax) ; \
        mov     %edx, 12(%eax) ; \
        mov     %esi, 16(%eax) ; \
        mov     %edi, 20(%eax) ; \
        pushf ; \
        pop     %ebx ; \
        mov     %ebx, 24(%eax) ; \
        pop     %ecx ; \
        mov     %ecx, (%eax) ; \
        cld ; \
        mov     %ebx, %eax ; \
        and     $1, %eax ; \
        pop     %edi ; \
        pop     %esi ; \
        pop     %ebx ; \
        pop     %ebp ; \
        ret

        INTCALL( CSYM( pm_int21 ), 0x21 )
        INTCALL( CSYM( pm_int33 ), 0x33 )

        .data
        .p2align 2
saved_esp:
        .long   0
