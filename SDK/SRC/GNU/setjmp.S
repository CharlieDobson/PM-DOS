/*
 * SETJMP.S - setjmp(), longjmp() and the Ctrl+C handler, for GCC,
 * Clang and DJGPP.  WATCOM\SETJMP.ASM is the same for Open Watcom and
 * says what a jmp_buf holds.  The Ctrl+C handler lines the stack up
 * on 16 bytes before it calls C, which the GNU compilers' code expects.
 */
#define CAT2( a, b )    a ## b
#define CAT( a, b )     CAT2( a, b )
#define CSYM( name )    CAT( __USER_LABEL_PREFIX__, name )
#define FUNC( name )    .globl CSYM( name ) ; CSYM( name ):

        .text

FUNC( setjmp )
        mov     4(%esp), %eax           /* the jmp_buf */
        mov     %ebx, (%eax)
        mov     %esi, 4(%eax)
        mov     %edi, 8(%eax)
        mov     %ebp, 12(%eax)
        lea     4(%esp), %ecx
        mov     %ecx, 16(%eax)
        mov     (%esp), %ecx
        mov     %ecx, 20(%eax)
        xor     %eax, %eax
        ret

FUNC( longjmp )
        mov     4(%esp), %edx           /* the jmp_buf */
        mov     8(%esp), %eax           /* the value, never 0 */
        or      %eax, %eax
        jnz     1f
        inc     %eax
1:      mov     (%edx), %ebx
        mov     4(%edx), %esi
        mov     8(%edx), %edi
        mov     12(%edx), %ebp
        mov     16(%edx), %esp
        jmp     *20(%edx)

FUNC( __pm_int23 )
        pushal
        cld
        mov     %esp, %ebx              /* kept by the call: callee-saved */
        and     $-16, %esp
        call    CSYM( __pm_sigint )
        mov     %ebx, %esp
        popal
        iret
