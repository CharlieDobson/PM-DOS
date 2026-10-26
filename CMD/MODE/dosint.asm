;
; DOSINT.ASM - entry point, the INT 21h thunk and port I/O for MODE.EXE,
; the PM-DOS native MODE.  Open Watcom WASM, -dD32.
;
; The program links no C runtime, so this is its whole startup: make
; ES equal DS (flat model code assumes it), clear the BSS and call
; dos_entry().  The thunk loads a REGS block into the registers, issues
; the interrupt and stores the registers and flags back:
;
;       REGS  +0 EAX  +4 EBX  +8 ECX  +12 EDX  +16 ESI  +20 EDI
;             +24 FLAGS (bit 0 on input: carry flag set before the INT)
;
; The same D32 layout as TREE's and XCOPY's (see LABEL\DOSINT.ASM for
; why the image is one group and why it ends with D32END): the kernel
; loads the file whole at slot offset 2000h, so this module is FIRST on
; the link line and the header is the first thing in it.
;
; PORT I/O.  A native program runs at IOPL 0, so IN and OUT fault and
; the kernel does them for it (pm3_fault, IO\VM86.INC) - every port but
; the A20 gate's goes to the hardware.  MODE writes a serial port's
; registers and a CGA's CRT controller that way, as MS-DOS's MODE did
; with the same instructions.
;
; No ^C handler: MODE does one thing and ends, and the kernel's INT 23h
; ends it between two calls as well as anywhere.
;
        .386p
        .model  flat

        include pmdos.inc
D32STACK        equ     4000h

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

; u32 port_in( u32 port ) - one byte
        public  _port_in
_port_in proc   near
        mov     edx,[esp+4]
        xor     eax,eax
        in      al,dx
        ret
_port_in endp

; void port_out( u32 port, u32 value ) - one byte
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
; zeroes to reach them (the link orders class D32END after BSS).
D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end     start
