;
; DOSINT.ASM - entry point, the INT 21h thunk and the ^C handler for
; EDIT.EXE, the PM-DOS native editor.  Open Watcom WASM, -dD32.
;
; The same D32 layout as XCOPY's and LABEL's: the kernel loads the file
; whole at slot offset 2000h, so this module is FIRST on the link line
; and the header is the first thing in it; the image is one group and
; ends with D32END so the BSS is written out as zeroes.
;
; ^C IS A KEY IN AN EDITOR, not a way out of it: a program that died on
; Ctrl+C would take an unsaved file with it.  The handler IRETs, which
; tells DOS to carry on with whatever it was doing, and the keystroke
; is simply not special.
;
        .386p
        .model  flat

        include pmdos.inc
D32STACK        equ     4000h           ; 16K: a dialog over a dialog is the deepest

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

; INT 23h: nothing to do - the editor reads ^C as an ordinary key
        public  _break_handler
_break_handler proc near
        iretd
_break_handler endp

        .data
saved_esp       dd      0

D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end     start
