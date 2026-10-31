;
; I8.ASM - the arithmetic Open Watcom calls instead of inlining.  WASM
; or MASM 6.1x.
;
; wcc386 does 64-bit multiply, divide and shift, and the conversions
; between unsigned 64-bit integers and double, by calling these.  The
; register contracts are the compiler's:
;
;   __U8D  __I8D   EDX:EAX / ECX:EBX -> quotient EDX:EAX,
;                  remainder ECX:EBX.  Unsigned and signed.  A zero
;                  divisor faults, as DIV does.
;   __U8M  __I8M   EDX:EAX * ECX:EBX -> EDX:EAX (the low 64 bits are
;                  the same for both)
;   __U8LS __I8LS  EDX:EAX << EBX
;   __U8RS __I8RS  EDX:EAX >> EBX, logical and arithmetic
;   __CHP          ST(0) rounded toward zero, in place
;   __FDU87        EDX:EAX = a double's bits -> the unsigned 64-bit
;                  integer, in EDX:EAX
;   __U8FD7        EDX:EAX = an unsigned 64-bit integer -> the double's
;                  bits, in EDX:EAX
;
; Every other register comes back as it went in.  __8087 is the byte
; the compiler's floating-point code refers to; a native PM-DOS program
; has a coprocessor or no floating point at all, so it says 387.
;
        .386p
        .model  flat

        .code

        public  __U8D
__U8D   proc    near
        or      ecx,ecx
        jnz     u8d_wide
        push    esi                     ; a divisor under 2^32: two DIVs
        mov     esi,eax
        mov     eax,edx
        xor     edx,edx
        div     ebx                     ; EAX = the high quotient
        mov     ecx,eax
        mov     eax,esi
        div     ebx                     ; EAX = the low quotient
        mov     ebx,edx                 ; the remainder fits in 32 bits
        mov     edx,ecx
        xor     ecx,ecx
        pop     esi
        ret
u8d_wide:
        push    esi                     ; one bit at a time, 64 of them
        push    edi
        push    ebp
        xor     esi,esi                 ; ESI:EDI = the remainder
        xor     edi,edi
        mov     ebp,64
u8d_bit:
        shl     eax,1                   ; dividend out, quotient in
        rcl     edx,1
        rcl     edi,1
        rcl     esi,1
        jc      u8d_sub                 ; past 64 bits: certainly >= divisor
        cmp     esi,ecx
        jb      u8d_next
        ja      u8d_sub
        cmp     edi,ebx
        jb      u8d_next
u8d_sub:
        sub     edi,ebx
        sbb     esi,ecx
        inc     eax                     ; the quotient bit SHL left clear
u8d_next:
        dec     ebp
        jnz     u8d_bit
        mov     ebx,edi
        mov     ecx,esi
        pop     ebp
        pop     edi
        pop     esi
        ret
__U8D   endp

        public  __I8D
__I8D   proc    near
        push    esi
        xor     esi,esi                 ; bit 0: negate the quotient,
        or      edx,edx                 ; bit 1: negate the remainder
        jns     i8d_apos
        neg     edx
        neg     eax
        sbb     edx,0
        mov     esi,3
i8d_apos:
        or      ecx,ecx
        jns     i8d_bpos
        neg     ecx
        neg     ebx
        sbb     ecx,0
        xor     esi,1
i8d_bpos:
        call    __U8D
        test    esi,1
        jz      i8d_quot
        neg     edx
        neg     eax
        sbb     edx,0
i8d_quot:
        test    esi,2
        jz      i8d_rem
        neg     ecx
        neg     ebx
        sbb     ecx,0
i8d_rem:
        pop     esi
        ret
__I8D   endp

        public  __U8M, __I8M
__U8M   proc    near
__I8M:
        push    ecx
        push    esi
        mov     esi,edx
        imul    esi,ebx                 ; high(a) * low(b)
        imul    ecx,eax                 ; high(b) * low(a)
        add     esi,ecx
        mul     ebx                     ; low(a) * low(b), 64 bits
        add     edx,esi
        pop     esi
        pop     ecx
        ret
__U8M   endp

        public  __U8LS, __I8LS
__U8LS  proc    near
__I8LS:
        push    ecx
        mov     ecx,ebx
        and     ecx,63
        cmp     cl,32
        jae     u8ls_wide
        shld    edx,eax,cl
        shl     eax,cl
        pop     ecx
        ret
u8ls_wide:
        mov     edx,eax
        xor     eax,eax
        sub     cl,32
        shl     edx,cl
        pop     ecx
        ret
__U8LS  endp

        public  __U8RS
__U8RS  proc    near
        push    ecx
        mov     ecx,ebx
        and     ecx,63
        cmp     cl,32
        jae     u8rs_wide
        shrd    eax,edx,cl
        shr     edx,cl
        pop     ecx
        ret
u8rs_wide:
        mov     eax,edx
        xor     edx,edx
        sub     cl,32
        shr     eax,cl
        pop     ecx
        ret
__U8RS  endp

        public  __I8RS
__I8RS  proc    near
        push    ecx
        mov     ecx,ebx
        and     ecx,63
        cmp     cl,32
        jae     i8rs_wide
        shrd    eax,edx,cl
        sar     edx,cl
        pop     ecx
        ret
i8rs_wide:
        mov     eax,edx
        sar     edx,31
        sub     cl,32
        sar     eax,cl
        pop     ecx
        ret
__I8RS  endp

        public  __CHP
__CHP   proc    near
        push    eax
        sub     esp,4                   ; [ESP] the old control word,
        fstcw   word ptr [esp]          ; [ESP+2] the chopping one
        fwait
        mov     ax,word ptr [esp]
        or      ah,0Ch                  ; round toward zero
        mov     word ptr [esp+2],ax
        fldcw   word ptr [esp+2]
        frndint
        fldcw   word ptr [esp]
        add     esp,4
        pop     eax
        ret
__CHP   endp

        public  __FDU87
__FDU87 proc    near
        push    edx
        push    eax
        fld     qword ptr [esp]
        fcom    qword ptr two63
        fnstsw  ax
        sahf
        jae     fdu_wide                ; 2^63 or more
        call    __CHP
        fistp   qword ptr [esp]
        pop     eax
        pop     edx
        ret
fdu_wide:
        fsub    qword ptr two63         ; FISTP takes signed values only
        call    __CHP
        fistp   qword ptr [esp]
        pop     eax
        pop     edx
        xor     edx,80000000h
        ret
__FDU87 endp

        public  __U8FD7
__U8FD7 proc    near
        push    edx
        push    eax
        fild    qword ptr [esp]         ; read as signed
        or      edx,edx
        jns     u8fd_done
        fadd    qword ptr two64         ; it was 2^64 more than that
u8fd_done:
        fstp    qword ptr [esp]
        pop     eax
        pop     edx
        ret
__U8FD7 endp

        .const
two63   dd      0, 43E00000h            ; 2^63 as a double
two64   dd      0, 43F00000h            ; 2^64

        .data
        public  __8087
__8087  db      3                       ; a 387 or better
        public  _fltused_               ; what a module using floating
_fltused_ db    0                       ; point refers to

        end
