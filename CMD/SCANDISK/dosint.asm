;
; DOSINT.ASM - entry point and the interrupt thunks for SCANDISK.EXE.
; Open Watcom WASM and MASM 6.1x.
;
; PMD's startup (see EDIT\DOSINT.ASM): the D32 header first - so this
; module is FIRST on the link line - then make ES equal DS, clear the
; BSS and call dos_entry().  int21 and int33 load a REGS block into the
; registers, issue the interrupt and store the registers and flags
; back:
;
;       REGS  +0 EAX  +4 EBX  +8 ECX  +12 EDX  +16 ESI  +20 EDI
;             +24 FLAGS (bit 0 on input: carry flag set before the INT)
;
; ^C IS A REQUEST TO STOP, NOT A STOP.  A check that is halfway through
; a repair has to finish the repair, so the handler raises a flag that
; the checks look at between one piece of work and the next.
;
; A CRITICAL ERROR IS FAILED.  The sector calls report their own errors
; and never raise one; what does is a file - the Undo file on a diskette
; that has been taken out - and the call that met it gets the failure.
;
; THE GRAPHICS SCREEN IS WRITTEN HERE.  A graphics mode's video memory
; is not in the program's segment: the kernel answers a mode set with a
; selector for it (21h/F1h AL=1Fh), which C's flat pointers cannot name.
; vid_put loads it into ES for the length of one string instruction and
; puts ES back, because the compiler's code takes ES to be DS.  The
; ports are here too: IN and OUT fault at ring 3 and the kernel carries
; them out, a byte at a time.
;
        .386p
        .model  flat

        include pmdos.inc
D32STACK        equ     10000h          ; directories nest: see CHECK.C

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

; The same for INT 33h, the mouse: a native program reaches the driver
; with the registers Microsoft's takes, whole, and a pointer in EDX
        public  _int33
_int33  proc    near
        push    ebp
        push    ebx
        push    esi
        push    edi
        mov     eax,[esp+20]            ; REGS *
        push    eax
        mov     ebx,[eax+4]
        mov     ecx,[eax+8]
        mov     edx,[eax+12]
        mov     esi,[eax+16]
        mov     edi,[eax+20]
        mov     eax,[eax]
        int     33h
        xchg    eax,[esp]               ; EAX = REGS *, [ESP] = result EAX
        mov     [eax+4],ebx
        mov     [eax+8],ecx
        mov     [eax+12],edx
        mov     [eax+16],esi
        mov     [eax+20],edi
        pop     ebx
        mov     [eax],ebx
        cld
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret
_int33  endp

; INT 23h: note the break and let DOS carry on
        public  _break_handler
_break_handler proc near
        mov     byte ptr ds:[_break_hit],1
        iretd
_break_handler endp

; INT 24h: fail the call that met the error, and remember there was one
        public  _crit_handler
_crit_handler proc near
        mov     byte ptr ds:[_crit_hit],1
        mov     al,CERR_FAIL
        iretd
_crit_handler endp

; void vid_put( unsigned long selector, unsigned long offset,
;               const void *src, unsigned long len )
; "len" bytes from the program's memory to the frame buffer.
        public  _vid_put
_vid_put proc   near
        push    esi
        push    edi
        push    es
        mov     eax,[esp+16]
        mov     edi,[esp+20]
        mov     esi,[esp+24]
        mov     ecx,[esp+28]
        mov     es,ax
        cld
        mov     edx,ecx
        shr     ecx,2
        rep     movsd
        mov     ecx,edx
        and     ecx,3
        rep     movsb
        pop     es
        pop     edi
        pop     esi
        ret
_vid_put endp

; void port_out( unsigned port, unsigned value ) - one byte
        public  _port_out
_port_out proc  near
        mov     edx,[esp+4]
        mov     eax,[esp+8]
        out     dx,al
        ret
_port_out endp

; unsigned port_in( unsigned port ) - one byte
        public  _port_in
_port_in proc   near
        mov     edx,[esp+4]
        xor     eax,eax
        in      al,dx
        ret
_port_in endp

        .data
saved_esp       dd      0
        public  _break_hit
        public  _crit_hit
_break_hit      db      0
_crit_hit       db      0,0,0

; THE LAST BYTES OF THE IMAGE, so that wlink writes the BSS out as
; zeroes to reach them (the link orders class D32END after BSS) and the
; kernel's 48h heap starts past it.
D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end     start
