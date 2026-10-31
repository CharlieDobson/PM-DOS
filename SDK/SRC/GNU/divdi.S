/*
 * DIVDI.S - 64-bit division for GCC, Clang and DJGPP
 *
 * i386 code divides a long long by calling these; GCC's libgcc has
 * them, but Clang has nothing for a bare i386 target, so the SDK
 * carries its own for all three.  cdecl:
 *
 *   __udivdi3( a, b )     a / b, unsigned
 *   __umoddi3( a, b )     a % b, unsigned
 *   __divdi3( a, b )      a / b, signed (toward zero)
 *   __moddi3( a, b )      a % b, signed (the sign of a)
 *   __udivmoddi4( a, b, *rem )   a / b, and a % b stored at rem if
 *                                 rem is not null
 *   __divmoddi4( a, b, *rem )    the same, signed
 *
 * The core is udivmod: EDX:EAX / ECX:EBX -> quotient EDX:EAX,
 * remainder ECX:EBX, every other register kept.  A zero divisor
 * faults, as DIV does.
 */
#define CAT2( a, b )    a ## b
#define CAT( a, b )     CAT2( a, b )
#define CSYM( name )    CAT( __USER_LABEL_PREFIX__, name )

        .text

udivmod:
        or      %ecx, %ecx
        jnz     1f
        push    %esi                    /* a divisor under 2^32: two DIVs */
        mov     %eax, %esi
        mov     %edx, %eax
        xor     %edx, %edx
        div     %ebx
        mov     %eax, %ecx
        mov     %esi, %eax
        div     %ebx
        mov     %edx, %ebx
        mov     %ecx, %edx
        xor     %ecx, %ecx
        pop     %esi
        ret
1:      push    %esi                    /* one bit at a time, 64 of them */
        push    %edi
        push    %ebp
        xor     %esi, %esi              /* ESI:EDI = the remainder */
        xor     %edi, %edi
        mov     $64, %ebp
2:      shl     $1, %eax
        rcl     $1, %edx
        rcl     $1, %edi
        rcl     $1, %esi
        jc      4f                      /* past 64 bits: certainly >= */
        cmp     %ecx, %esi
        jb      5f
        ja      4f
        cmp     %ebx, %edi
        jb      5f
4:      sub     %ebx, %edi
        sbb     %ecx, %esi
        inc     %eax
5:      dec     %ebp
        jnz     2b
        mov     %edi, %ebx
        mov     %esi, %ecx
        pop     %ebp
        pop     %edi
        pop     %esi
        ret

/* make the operands positive: ESI bit 0 = negate the quotient, bit 1 =
   negate the remainder */
sdivprep:
        xor     %esi, %esi
        or      %edx, %edx
        jns     1f
        neg     %edx
        neg     %eax
        sbb     $0, %edx
        mov     $3, %esi
1:      or      %ecx, %ecx
        jns     2f
        neg     %ecx
        neg     %ebx
        sbb     $0, %ecx
        xor     $1, %esi
2:      ret

sdivfix:
        test    $1, %esi
        jz      1f
        neg     %edx
        neg     %eax
        sbb     $0, %edx
1:      test    $2, %esi
        jz      2f
        neg     %ecx
        neg     %ebx
        sbb     $0, %ecx
2:      ret

/* the stack after the three pushes below: 16(%esp) = a, 24(%esp) = b,
   32(%esp) = rem */
#define ENTER \
        push    %ebx ; \
        push    %esi ; \
        push    %edi ; \
        mov     16(%esp), %eax ; \
        mov     20(%esp), %edx ; \
        mov     24(%esp), %ebx ; \
        mov     28(%esp), %ecx
#define LEAVE \
        pop     %edi ; \
        pop     %esi ; \
        pop     %ebx ; \
        ret

        .globl  CSYM( __udivdi3 )
CSYM( __udivdi3 ):
        ENTER
        call    udivmod
        LEAVE

        .globl  CSYM( __umoddi3 )
CSYM( __umoddi3 ):
        ENTER
        call    udivmod
        mov     %ebx, %eax
        mov     %ecx, %edx
        LEAVE

        .globl  CSYM( __divdi3 )
CSYM( __divdi3 ):
        ENTER
        call    sdivprep
        call    udivmod
        call    sdivfix
        LEAVE

        .globl  CSYM( __moddi3 )
CSYM( __moddi3 ):
        ENTER
        call    sdivprep
        call    udivmod
        call    sdivfix
        mov     %ebx, %eax
        mov     %ecx, %edx
        LEAVE

        .globl  CSYM( __udivmoddi4 )
CSYM( __udivmoddi4 ):
        ENTER
        call    udivmod
        jmp     1f

        .globl  CSYM( __divmoddi4 )
CSYM( __divmoddi4 ):
        ENTER
        call    sdivprep
        call    udivmod
        call    sdivfix
1:      mov     32(%esp), %edi
        or      %edi, %edi
        jz      2f
        mov     %ebx, (%edi)
        mov     %ecx, 4(%edi)
2:      LEAVE
