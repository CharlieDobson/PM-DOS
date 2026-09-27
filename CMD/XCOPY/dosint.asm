;
; DOSINT.ASM - entry point, the INT 21h thunk and the ^C handler for
; XCOPY.EXE, the PM-DOS native XCOPY.  Open Watcom WASM, -dD32.
;
; The program links no C runtime, so this is its whole startup: make
; ES equal DS (flat model code assumes it), clear the BSS and call
; dos_entry().  The thunk loads a REGS block into the registers, issues
; the interrupt and stores the registers and flags back:
;
;       REGS  +0 EAX  +4 EBX  +8 ECX  +12 EDX  +16 ESI  +20 EDI
;             +24 FLAGS (bit 0 on input: carry flag set before the INT)
;
; The same D32 layout as CHKDSK's and LABEL's (see LABEL\DOSINT.ASM for
; why the image is one group and why it ends with D32END): the kernel
; loads the file whole at slot offset 2000h, so this module is FIRST on
; the link line and the header is the first thing in it.
;
; THE ^C HANDLER only raises a flag.  An IRET from INT 23h tells DOS to
; carry on with the call it was in, so the copy is never cut off in the
; middle of a write: the program notices the flag between files - or
; between the blocks of one - deletes the half-written copy, and ends
; with errorlevel 2, which is what DOS's XCOPY says about a break.
;
        .386p
        .model  flat

        include pmdos.inc
D32STACK        equ     10000h          ; recursion, one frame a level

        extrn   _dos_entry:near
        extrn   _edata:byte
        extrn   _end:byte

        .code

        D32_HEAD SLOT_IMAGE,<OFFSET start>,0,D32STACK

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

        public  _int21
_int21  proc    near
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
        int     21h
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
_int21  endp

; INT 23h: note the break and let DOS carry on - unless the program is
; waiting on a question, where nothing is half-done and the only
; sensible answer to ^C is to stop now (break_exit does not return).
        extrn   _break_exit:near
        public  _break_handler
_break_handler proc near
        mov     byte ptr ds:[_break_hit],1
        cmp     byte ptr ds:[_in_prompt],0
        je      bh_ret
        cld
        call    _break_exit
bh_ret:
        iretd
_break_handler endp

        .data
saved_esp       dd      0
        public  _break_hit
        public  _in_prompt
_break_hit      db      0
_in_prompt      db      0,0,0

; THE LAST BYTES OF THE IMAGE, so that wlink writes the BSS out as
; zeroes to reach them (the link orders class D32END after BSS).
D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end     start
