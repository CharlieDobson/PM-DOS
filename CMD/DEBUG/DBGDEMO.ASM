;
; DBGDEMO.ASM - a tiny native program to debug by hand under DEBUG, for
; the native-mode tests (not shipped).  Open Watcom WASM, -dD32.
;
; It prints a letter, adds a few numbers, calls a subroutine and ends
; with errorlevel 7, so that T walks it, P steps over the CALL, G runs
; to a breakpoint, and R shows 32-bit registers and the instruction.
;
        .386p
        .model  flat

        include pmdos.inc

        .code

        D32_HEAD SLOT_IMAGE,<OFFSET start>,0,1000h

start:
        mov     eax,11111111h
        mov     ebx,22222222h
        add     eax,ebx
        mov     dl,'D'
        mov     ah,2
        int     21h
        call    sub1
        mov     eax,4C07h
        int     21h
sub1:
        inc     ecx
        inc     ecx
        ret

D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end     start
