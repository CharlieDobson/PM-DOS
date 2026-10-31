;
; SETJMP.ASM - setjmp(), longjmp() and the Ctrl+C handler, for Open
; Watcom.  WASM or MASM 6.1x.  GNU\SETJMP.S is the same for the others.
;
; A jmp_buf holds EBX ESI EDI EBP, ESP as it is after setjmp returns,
; and the address it returns to.  Both are __cdecl.
;
; __pm_int23 is what INT 23h runs once signal() has been given a
; handler for SIGINT: it keeps every register, calls the C side
; (SIGNAL.C), and IRETs, which tells DOS to carry on with the call the
; Ctrl+C interrupted.
;
        .386p
        .model  flat

        extrn   ___pm_sigint:near       ; __pm_sigint, __cdecl

        .code

        public  _setjmp
_setjmp proc    near
        mov     eax,[esp+4]             ; the jmp_buf
        mov     [eax],ebx
        mov     [eax+4],esi
        mov     [eax+8],edi
        mov     [eax+12],ebp
        lea     ecx,[esp+4]
        mov     [eax+16],ecx
        mov     ecx,[esp]
        mov     [eax+20],ecx
        xor     eax,eax
        ret
_setjmp endp

        public  _longjmp
_longjmp proc   near
        mov     edx,[esp+4]             ; the jmp_buf
        mov     eax,[esp+8]             ; the value, never 0
        or      eax,eax
        jnz     lj_value
        inc     eax
lj_value:
        mov     ebx,[edx]
        mov     esi,[edx+4]
        mov     edi,[edx+8]
        mov     ebp,[edx+12]
        mov     esp,[edx+16]
        jmp     dword ptr [edx+20]
_longjmp endp

        public  ___pm_int23
___pm_int23 proc near
        pushad
        cld
        call    ___pm_sigint
        popad
        iretd
___pm_int23 endp

        end
