;
; C0D32.ASM - the start of every Open Watcom program the SDK builds:
; the D32 header, the entry point and the interrupt thunks.  WASM or
; MASM 6.1x.
;
; It is in the library, not on the link line.  wcc386 makes the module
; with main() in it refer to _cstart_, and that reference is what brings
; this module in.  D32.LNK orders class D32HEAD first, so the header is
; the first 32 bytes of the image wherever this module sits among the
; others.
;
; The BSS goes into the file as zeroes: D32END is the last class, and
; wlink writes everything before it.  The header's BSS field stays 0
; because the kernel starts the 48h heap at the end of the file, which
; is then past the end of the BSS too.  _edata and _end exist only when
; DGROUP has data in it, so this module always has some.
;
; AND THE WHOLE IMAGE IS ONE GROUP: wlink's raw writer keeps the gaps
; between segments inside a group but writes one group straight after
; another, so a segment outside it would sit a few bytes short of the
; addresses the code uses for it (CMD\LABEL\DOSINT.ASM found this).
;
; The registers block for pm_int21 and pm_int33 (PMDOS.H):
;
;       +0 EAX  +4 EBX  +8 ECX  +12 EDX  +16 ESI  +20 EDI
;       +24 FLAGS (bit 0 on input: the carry flag before the INT)
;
; ESP is kept in memory across the INT, which copes with handlers that
; leave flags on the stack and with ones that do not.
;
        .386p
        .model  flat

D32_VER         equ     1
D32T_PROG       equ     0
SLOT_IMAGE      equ     2000h
STACK_SIZE      equ     10000h          ; 64K unless D32FIX says otherwise

        extrn   ___pm_start:near        ; __pm_start, __cdecl
        extrn   _edata:byte
        extrn   _end:byte

D32HEAD segment dword public use32 'D32HEAD'
        db      'C', 'D'                ; the signature
        db      0CDh, 20h               ; INT 20h, for real DOS
        db      D32_VER, D32T_PROG
        dw      0
        dd      SLOT_IMAGE
        dd      offset start
        dd      0                       ; the BSS is in the file
        dd      STACK_SIZE
        dd      0, 0
D32HEAD ends

        .code

        public  _cstart_
_cstart_:
start:
        push    ds
        pop     es
        cld
        mov     edi,offset _edata
        mov     ecx,offset _end
        sub     ecx,edi
        xor     eax,eax
        rep     stosb
        call    ___pm_start             ; does not come back
        mov     eax,4CFFh
        int     21h

INTCALL macro   procname, intnum
        public  procname
procname proc   near
        push    ebp
        push    ebx
        push    esi
        push    edi
        mov     eax,[esp+20]            ; pm_regs *
        push    eax
        mov     saved_esp,esp
        mov     ebx,[eax+4]
        mov     ecx,[eax+8]
        mov     edx,[eax+12]
        mov     esi,[eax+16]
        mov     edi,[eax+20]
        bt      dword ptr [eax+24],0    ; CF = the carry asked for
        mov     eax,[eax]
        int     intnum
        mov     esp,saved_esp
        xchg    eax,[esp]               ; EAX = pm_regs *, [ESP] = EAX out
        mov     [eax+4],ebx
        mov     [eax+8],ecx
        mov     [eax+12],edx
        mov     [eax+16],esi
        mov     [eax+20],edi
        pushfd
        pop     ebx
        mov     [eax+24],ebx
        pop     ecx
        mov     [eax],ecx
        cld
        mov     eax,ebx
        and     eax,1                   ; the carry, as the result
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret
procname endp
        endm

        INTCALL _pm_int21, 21h
        INTCALL _pm_int33, 33h

        .data
saved_esp       dd      0

; THE LAST BYTES OF THE IMAGE, so that wlink writes the BSS out as
; zeroes to reach them.
D32END  segment dword public use32 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   D32HEAD, _TEXT, _DATA, D32END

        end     start
