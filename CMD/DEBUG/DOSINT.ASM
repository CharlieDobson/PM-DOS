;
; DOSINT.ASM - entry point, the interrupt thunks, port I/O and the ^C
; handler for DEBUG.EXE, the PM-DOS native DEBUG.  Open Watcom WASM,
; -dD32.
;
; The program links no C runtime, so this is its whole startup: make
; ES equal DS (flat model code assumes it), clear the BSS and call
; dos_entry().  The thunks load a REGS block into the registers, issue
; the interrupt and store the registers and flags back:
;
;       REGS  +0 EAX  +4 EBX  +8 ECX  +12 EDX  +16 ESI  +20 EDI
;             +24 FLAGS (bit 0 on input: carry flag set before the INT)
;
; INT 25h and 26h are PM-DOS's flat reading of the raw-sector API: AL
; the drive, EDX the first sector, ECX the count, EBX the buffer, and a
; plain IRET with no flags word left on the stack.
;
; I AND O are DEBUG's own IN and OUT.  At ring 3 they fault into the
; kernel's monitor and are done there, as they are for a DOS box.
;
; THE ^C HANDLER is 6.22's: a break while DEBUG is working stops the
; command and gives the prompt back.  It leaves the way CMD.EXE's does
; - 21h/F1h AL=5 to say the handler is not coming back, the stack put
; back where loop_run left it, and the command loop called again - and
; never while a file or a disk is being written, when break_quiet asks
; it only to make a note (break_hit) and let the call finish.
;
        .386p
        .model  flat

        include pmdos.inc
D32STACK        equ     10000h          ; the assembler parses by recursion

        extrn   _dos_entry:near
        extrn   _break_reset:near
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
        mov     eax,4C00h               ; not reached: sys_exit() ends us
        int     21h

; the three thunks share one body: EAX = the vector's own short call
        public  _int21
_int21  proc    near
        push    offset do21
        jmp     short thunk
_int21  endp

        public  _int25
_int25  proc    near
        push    offset do25
        jmp     short thunk
_int25  endp

        public  _int26
_int26  proc    near
        push    offset do26
        jmp     short thunk
_int26  endp

; [esp] = the routine that issues the interrupt, [esp+4] = the return
; address, [esp+8] = REGS *
thunk:
        push    ebp
        push    ebx
        push    esi
        push    edi
        mov     eax,[esp+16]            ; the issuing routine
        mov     thunk_call,eax
        mov     eax,[esp+24]            ; REGS *
        push    eax
        mov     saved_esp,esp
        mov     ebx,[eax+4]
        mov     ecx,[eax+8]
        mov     edx,[eax+12]
        mov     esi,[eax+16]
        mov     edi,[eax+20]
        bt      dword ptr [eax+24],0    ; CF = requested carry
        mov     eax,[eax]
        call    dword ptr [thunk_call]
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
        add     esp,4                   ; the issuing routine
        ret

do21:   int     21h
        ret
do25:   int     25h
        ret
do26:   int     26h
        ret

; u32 port_in( u32 port ) / void port_out( u32 port, u32 value )
        public  _port_in
_port_in proc   near
        mov     edx,[esp+4]
        xor     eax,eax
        in      al,dx
        ret
_port_in endp

        public  _port_out
_port_out proc  near
        mov     edx,[esp+4]
        mov     eax,[esp+8]
        out     dx,al
        ret
_port_out endp

; void loop_run( void (*body)( void ) ) - call BODY for ever; a ^C
; comes back here, with this stack, and calls it again
        public  _loop_run
_loop_run proc  near
        mov     eax,[esp+4]
        mov     loop_body,eax
        mov     loop_esp,esp
lr_again:
        call    dword ptr [loop_body]
        jmp     short lr_again
_loop_run endp

; void cmd_abort( void ) - the command cannot go on: drop everything it
; was in the middle of and read the next one
        public  _cmd_abort
_cmd_abort proc near
        mov     esp,loop_esp
        jmp     lr_again
_cmd_abort endp

; A's commands read their own lines, so a bad one re-prompts rather than
; ending the command.  These are setjmp/longjmp over three dwords (ESP,
; EBP, the return address): int dbg_setjmp( buf ) returns 0 the first
; time and the value dbg_longjmp( buf, value ) passes thereafter.
        public  _dbg_setjmp
_dbg_setjmp proc near
        mov     eax,[esp+4]
        mov     edx,[esp]
        mov     [eax],esp
        mov     [eax+4],ebp
        mov     [eax+8],edx
        xor     eax,eax
        ret
_dbg_setjmp endp

        public  _dbg_longjmp
_dbg_longjmp proc near
        mov     eax,[esp+4]
        mov     ecx,[esp+8]
        mov     esp,[eax]
        mov     ebp,[eax+4]
        mov     edx,[eax+8]
        or      ecx,ecx
        jnz     dlj_val
        inc     ecx
dlj_val:
        mov     eax,ecx
        mov     [esp],edx
        ret
_dbg_longjmp endp

; INT 23h
        public  _break_handler
_break_handler proc near
        cmp     byte ptr ds:[_break_quiet],0
        jne     bh_note
        cmp     dword ptr ds:[loop_esp],0
        je      bh_note                 ; no loop yet: nothing to go back to
        mov     ax,0F105h               ; the handler is not coming back
        int     21h
        mov     esp,loop_esp
        sti                             ; the gate cleared IF, and nothing
                                        ; below IRETs to set it again
        call    _break_reset
        jmp     short lr_again
bh_note:
        mov     byte ptr ds:[_break_hit],1
        iretd
_break_handler endp

        .data
saved_esp       dd      0
thunk_call      dd      0
loop_body       dd      0
loop_esp        dd      0
        public  _break_hit, _break_quiet
_break_hit      db      0
_break_quiet    db      0
                db      0,0

; THE LAST BYTES OF THE IMAGE, so that wlink writes the BSS out as
; zeroes to reach them (the link orders class D32END after BSS).
D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end     start
