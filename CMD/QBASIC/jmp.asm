;
; JMP.ASM - rt_setjmp and rt_longjmp for QBASIC, which links no C
; library.  Open Watcom WASM and MASM 6.1x.  ACROREAD's, unchanged.
;
; Two things leave from wherever they are, however deep: the compiler
; at the first error it finds (QBasic reports one at a time), and a
; runtime error in the middle of a statement, which goes back to the
; VM's loop to be handled or reported.
;
;       int  __cdecl rt_setjmp( JMPBUF *buf );     0, or what longjmp gave
;       void __cdecl rt_longjmp( JMPBUF *buf, int val );
;
; JMPBUF is six doublewords: EBX ESI EDI EBP ESP EIP.  The stack pointer
; kept is the one the caller has when rt_setjmp has returned, before it
; drops the argument, so the second return finds the stack exactly as
; the first did.
;
        .386
        .model  flat

        .code

        public  _rt_setjmp
_rt_setjmp proc near
        mov     eax,[esp+4]
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
_rt_setjmp endp

        public  _rt_longjmp
_rt_longjmp proc near
        mov     edx,[esp+4]
        mov     eax,[esp+8]
        or      eax,eax
        jnz     lj_go
        inc     eax                     ; never 0: that is the first return
lj_go:
        mov     ebx,[edx]
        mov     esi,[edx+4]
        mov     edi,[edx+8]
        mov     ebp,[edx+12]
        mov     esp,[edx+16]
        cld
        jmp     dword ptr [edx+20]
_rt_longjmp endp

        end
