;
; DOSINT.ASM - entry point, the INT 21h thunk and the way to the screen
; for ACROREAD.EXE, the PM-DOS PDF reader.  Open Watcom WASM.
;
; The same D32 layout as EDIT's and PMD's: the kernel loads the file
; whole at slot offset 2000h, so this module is FIRST on the link line
; and the header is the first thing in it; the image is one group and
; ends with D32END so the BSS is written out as zeroes.
;
; THE FRAME BUFFER IS A SELECTOR.  A native program's segment is its
; slot, and video memory is outside it; 21h/F1h AL=1Fh sets a graphics
; mode and describes its frame buffer with SEL_VBE (DOS\VBE.INC), and
; gfx_put and gfx_fill are how C writes through it: ES is loaded for
; one string instruction and put back.  ES, because DS and ES are the
; two segment registers every DOS call gives back.
;
; THE PORTS are for the sixteen-colour VGA screen, whose four planes
; are chosen through the sequencer and whose colours are the DAC's.
; IN and OUT fault at ring 3 and the kernel carries them out, a byte at
; a time, so they are bytes here and a screen is written with as few of
; them as will do.
;
        .386p
        .model  flat

        include pmdos.inc
D32STACK        equ     10000h          ; 64K: a form in a form in a form, each
                                        ; with its own operands

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

; INT 23h: ^C is a key here.  A program that died on it would leave
; the shell a graphics screen to write its prompt on.
        public  _break_handler
_break_handler proc near
        iretd
_break_handler endp

; INT 24h: a drive that will not answer is failed, and the read that
; asked says so
        public  _crit_handler
_crit_handler proc near
        mov     al,CERR_FAIL
        iretd
_crit_handler endp

; void gfx_put( unsigned long offset, const void *src, unsigned long bytes )
; "bytes" from the program's memory to the frame buffer at "offset"
        public  _gfx_put
_gfx_put proc   near
        push    esi
        push    edi
        push    es
        mov     edi,[esp+16]
        mov     esi,[esp+20]
        mov     ecx,[esp+24]
        mov     eax,SEL_VBE
        mov     es,ax
        mov     eax,ecx
        shr     ecx,2
        cld
        rep     movsd
        mov     ecx,eax
        and     ecx,3
        rep     movsb
        pop     es
        pop     edi
        pop     esi
        ret
_gfx_put endp

; void gfx_fill( unsigned long offset, unsigned long value, unsigned long bytes )
; "bytes" of the byte "value" into the frame buffer at "offset"
        public  _gfx_fill
_gfx_fill proc  near
        push    edi
        push    es
        mov     edi,[esp+12]
        mov     eax,[esp+16]
        mov     ecx,[esp+20]
        mov     edx,SEL_VBE
        mov     es,dx
        cld
        rep     stosb
        pop     es
        pop     edi
        ret
_gfx_fill endp

; unsigned port_in( unsigned port ) - one byte
        public  _port_in
_port_in proc   near
        mov     edx,[esp+4]
        xor     eax,eax
        in      al,dx
        ret
_port_in endp

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

; THE LAST BYTES OF THE IMAGE, so that wlink writes the BSS out as
; zeroes to reach them (the link orders class D32END after BSS) and the
; kernel's 48h heap starts past it.
D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end     start
