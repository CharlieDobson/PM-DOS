;
; DOSINT.ASM - entry point, the interrupt thunks and the few things C
; cannot say, for PMD.EXE, the PM-DOS diagnostics.  Open Watcom WASM
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
; THE PROCESSOR IS ASKED HERE.  Whether the flags register will hold
; the AC and ID bits, what CPUID says, whether a divide leaves the
; flags alone (a Cyrix chip's signature) and how a coprocessor treats
; infinity are all single instructions with no spelling in C.
;
; THE PORTS ARE READ HERE TOO.  IN and OUT fault at ring 3 and the
; kernel carries them out, a byte at a time - so port_in and port_out
; are bytes, and a word is two of them.
;
; kernel_facts is what PMDOS.INC says about the kernel this program
; was built with, for the pages that describe it: where a DOS box's
; list of lists is, and the versions of the services it gives a box.
;
        .386p
        .387
        .model  flat

        include pmdos.inc
D32STACK        equ     6000h           ; 24K: a page being built under a
                                        ; dialog over a dialog

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
; program is left
        public  _break_handler
_break_handler proc near
        iretd
_break_handler endp

; INT 24h: a drive that will not answer is failed, and the page says
; so.  A diskette drive with nothing in it is the usual one.
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

; int cpu_flag_holds( unsigned long mask ) - 1 if those bits of EFLAGS
; can be changed: AC (40000h) on a 486 and later, ID (200000h) where
; there is a CPUID instruction.  The flags are left as they were.
        public  _cpu_flag_holds
_cpu_flag_holds proc near
        mov     ecx,[esp+4]
        pushfd
        pop     eax
        mov     edx,eax                 ; EDX = the flags as they are
        xor     eax,ecx
        push    eax
        popfd
        pushfd
        pop     eax
        push    edx
        popfd
        xor     eax,edx
        and     eax,ecx
        cmp     eax,ecx
        mov     eax,0
        jne     cfh_out
        inc     eax
cfh_out:
        ret
_cpu_flag_holds endp

; void cpu_id( unsigned long leaf, unsigned long *regs ) - CPUID, the
; four registers into regs[0..3] as EAX, EBX, ECX, EDX.  Only where
; cpu_flag_holds( 200000h ) said there is one.
        public  _cpu_id
_cpu_id proc    near
        push    ebx
        push    edi
        mov     eax,[esp+12]
        mov     edi,[esp+16]
        xor     ecx,ecx
        db      0Fh, 0A2h               ; CPUID
        mov     [edi],eax
        mov     [edi+4],ebx
        mov     [edi+8],ecx
        mov     [edi+12],edx
        pop     edi
        pop     ebx
        ret
_cpu_id endp

; int cpu_div_keeps_flags( void ) - 1 if a divide leaves the flags as
; they were, which is a Cyrix (or a NexGen) processor: Intel's and
; AMD's change them.  5 / 2 with every arithmetic flag cleared first.
        public  _cpu_div_keeps_flags
_cpu_div_keeps_flags proc near
        push    ebx
        xor     eax,eax
        sahf                            ; SF, ZF, AF, PF, CF = 0
        mov     eax,5
        mov     ebx,2
        div     bl
        lahf
        xor     ecx,ecx
        cmp     ah,2                    ; bit 1 alone: nothing moved
        jne     cdk_out
        inc     ecx
cdk_out:
        mov     eax,ecx
        pop     ebx
        ret
_cpu_div_keeps_flags endp

; int fpu_is_287( void ) - 1 if the coprocessor takes plus and minus
; infinity for the same number, as an 80287 does and an 80387 does
; not.  Only where the kernel said there is a coprocessor.
        public  _fpu_is_287
_fpu_is_287 proc near
        fninit
        fld1
        fldz
        fdivp   st(1),st                ; 1 / 0 = +infinity
        fld     st(0)
        fchs                            ; and -infinity
        fcompp
        fstsw   word ptr fpu_word
        fwait
        fninit
        xor     eax,eax
        test    byte ptr fpu_word+1,40h ; C3: they compared equal
        jz      f287_out
        inc     eax
f287_out:
        ret
_fpu_is_287 endp

        .data
saved_esp       dd      0
fpu_word        dw      0
crit_seen       db      0

; What the kernel this was built with says of itself - see KFACTS in
; SYS.H, which is this laid out for C.
        public  _kernel_facts
_kernel_facts   label   dword
        dd      DOSDATA + GLOL_PTR      ; the list of lists, as a box has it
        dd      DOSDATA                 ; and the segment it is in, flat
        dd      V86_TPA                 ; the first memory control block
        dd      EMS_VERSION             ; LIM, BCD
        dd      DPMI_VER_MAJOR
        dd      DPMI_VER_MINOR
        dd      LASTDRIVE

; THE LAST BYTES OF THE IMAGE, so that wlink writes the BSS out as
; zeroes to reach them (the link orders class D32END after BSS) and the
; kernel's 48h heap starts past it.
D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end     start
