;
; DOSINT.ASM - entry point and software interrupt thunks for the DOS
; builds of LABEL/32 (LABEL.EXE and LABEL32A.EXE).  Open Watcom WASM.
;
; The program links no C runtime, so this is its whole startup: make
; ES equal DS (flat model code assumes it), clear the BSS and call
; dos_entry().  The thunks load a REGS block into the registers, issue
; the interrupt and store the registers and flags back:
;
;       REGS  +0 EAX  +4 EBX  +8 ECX  +12 EDX  +16 ESI  +20 EDI
;             +24 FLAGS (bit 0 on input: carry flag set before the INT)
;
; ESP is saved in memory around the INT, which copes both with handlers
; that leave the flags on the stack (INT 25h/26h) and with ones that
; do not.
;
; Assembled with -dD32 it is the start of LABEL.EXE for PM-DOS, which
; is a D32 image and not an LE one: the kernel loads the file whole at
; slot offset 2000h and enters at the offset the header names, so this
; module has to be FIRST on the link line and the header has to be the
; first thing in it.  There is no STACK segment in that build - PM-DOS
; puts the stack at the top of the slot and sizes it from the header -
; and the header's BSS field is zero because the BSS is IN the file, as
; zeroes (see D32END at the bottom): the kernel starts the 48h heap at
; the end of the file, which is then past the end of the BSS too.
;
        .386p
        .model  flat

ifdef D32
        include pmdos.inc
D32STACK        equ     8000h           ; no recursion, 130-byte buffers
endif

        extrn   _dos_entry:near
        extrn   _edata:byte
        extrn   _end:byte

        .code

ifdef D32
        D32_HEAD SLOT_IMAGE,<OFFSET start>,0,D32STACK
endif

start:
        push    ds
        pop     es
        cld
        mov     edi,offset _edata
        mov     ecx,offset _end
        sub     ecx,edi
        xor     eax,eax
        rep     stosb
        call    _dos_entry
        mov     eax,4CFFh               ; not reached: sys_exit() ends us
        int     21h

INTCALL macro   procname, intnum
        public  procname
procname proc   near
        push    ebp
        push    ebx
        push    esi
        push    edi
        mov     eax,[esp+20]            ; REGS *
        push    eax
        mov     saved_esp,esp
        mov     ebx,[eax+4]
        mov     ecx,[eax+8]
        mov     edx,[eax+12]
        mov     esi,[eax+16]
        mov     edi,[eax+20]
        bt      dword ptr [eax+24],0    ; CF = requested carry
        mov     eax,[eax]
        int     intnum
        mov     esp,saved_esp
        xchg    eax,[esp]               ; EAX = REGS *, [ESP] = result EAX
        mov     [eax+4],ebx
        mov     [eax+8],ecx
        mov     [eax+12],edx
        mov     [eax+16],esi
        mov     [eax+20],edi
        pushfd
        pop     ebx
        mov     [eax+24],ebx
        pop     ebx
        mov     [eax],ebx
        cld
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret
procname endp
        endm

        INTCALL _int21, 21h
        INTCALL _int25, 25h
        INTCALL _int26, 26h
        INTCALL _int31, 31h

        .data
saved_esp       dd      0

ifndef D32
STACK   segment dword stack 'STACK'
        db      10000h dup(?)
STACK   ends
else
; THE LAST BYTES OF THE IMAGE, and they are here to be last: wlink's raw
; output ends at the last byte anything INITIALISED, so without them the
; BSS would not be in the file at all and the kernel would build its
; 48h heap on top of it.  The link orders class D32END after BSS, which
; makes wlink write the BSS out as zeroes to reach this.
D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

; AND THE WHOLE IMAGE IS ONE GROUP, which is not a formality.  wlink's
; raw writer keeps the gaps between segments INSIDE a group but writes
; one group straight after another - so with the code in a group of its
; own, the alignment padding between the end of _TEXT and the start of
; the data was simply left out, and every string and table in the file
; sat a few bytes short of the address the code used for it.
DGROUP  group   _TEXT, D32END
endif

        end     start
