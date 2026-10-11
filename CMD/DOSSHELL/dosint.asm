;
; DOSINT.ASM - entry point, the interrupt thunks and the few things C
; cannot say, for DOSSHELL.EXE, the PM-DOS Shell.  Open Watcom WASM
; and MASM 6.1x.
;
; EDIT's startup (see EDIT\DOSINT.ASM): the D32 header first - so this
; module is FIRST on the link line - then make ES equal DS, clear the
; BSS and call dos_entry().  int21 and int33 load a REGS block into the
; registers, issue the interrupt and store the registers and flags
; back:
;
;       REGS  +0 EAX  +4 EBX  +8 ECX  +12 EDX  +16 ESI  +20 EDI
;             +24 FLAGS (bit 0 on input: carry flag set before the INT)
;
; THE FRAME BUFFER IS WRITTEN HERE.  A graphics mode's video memory is
; not in the program's segment: the kernel answers a mode set with a
; selector for it (21h/F1h AL=1Fh), and a selector is something C's
; flat pointers cannot name.  vid_put and vid_fill load it into ES for
; the length of one string instruction and put ES back, because the
; compiler's code takes ES to be DS.
;
; THE PORTS ARE WRITTEN HERE TOO.  IN and OUT fault at ring 3 and the
; kernel carries them out, a byte at a time; the planar modes need the
; sequencer's map mask once for each plane of a picture, and that is
; all the shell asks of them.
;
        .386p
        .model  flat

        include pmdos.inc
D32STACK        equ     8000h           ; 32K: a directory tree is read by
                                        ; a routine that calls itself

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

; INT 23h: ^C is a key here, as it is in EDIT - the menus are how the
; shell is left
        public  _break_handler
_break_handler proc near
        iretd
_break_handler endp

; INT 24h: a drive that will not answer is failed, and the shell says
; so in a box of its own.  A diskette drive with nothing in the drive
; is the usual one.
        public  _crit_handler
_crit_handler proc near
        mov     byte ptr crit_seen,1
        mov     al,CERR_FAIL
        iretd
_crit_handler endp

; int crit_take( void ) - 1 if a critical error was failed since the
; last call
        public  _crit_take
_crit_take proc near
        xor     eax,eax
        xchg    al,byte ptr crit_seen
        ret
_crit_take endp

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

; void vid_fill( unsigned long selector, unsigned long offset,
;                unsigned long value, unsigned long len )
; "len" bytes of the frame buffer set to one value.
        public  _vid_fill
_vid_fill proc  near
        push    edi
        push    es
        mov     eax,[esp+12]
        mov     edi,[esp+16]
        mov     edx,[esp+20]
        mov     ecx,[esp+24]
        mov     es,ax
        mov     al,dl
        cld
        rep     stosb
        pop     es
        pop     edi
        ret
_vid_fill endp

; void port_out( unsigned port, unsigned value ) - one byte
        public  _port_out
_port_out proc  near
        mov     edx,[esp+4]
        mov     eax,[esp+8]
        out     dx,al
        ret
_port_out endp

        .data
saved_esp       dd      0
crit_seen       db      0

; THE LAST BYTES OF THE IMAGE, so that wlink writes the BSS out as
; zeroes to reach them (the link orders class D32END after BSS) and the
; kernel's 48h heap starts past it.
D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end     start
