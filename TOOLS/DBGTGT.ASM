;
; DBGTGT.ASM - the program DBGTEST.EXE debugs.  Open Watcom WASM.
;
; A D32 image with nothing to do but be stepped through, stopped and
; faulted, at places DBGTEST can find: the jump at its entry steps over
; a table of the offsets that matter, which DBGTEST reads out of the
; held program with DBF_READ.  In the order the test meets them:
;
;       +2   realstart  where the first single step lands
;       +6   bp1        a breakpoint DBGTEST writes over the code
;       +10  int3site   an INT 3 of the program's own
;       +14  nulsite    a read through a null pointer
;       +18  oksite     where DBGTEST sends it after the fault
;       +22  h23        its INT 23h "handler", which is never called
;
        .386p
        .model  flat

        include pmdos.inc

        .code

        D32_HEAD SLOT_IMAGE,<OFFSET start>,0,1000h

start:
        jmp     short realstart
        dd      OFFSET realstart
        dd      OFFSET bp1
        dd      OFFSET int3site
        dd      OFFSET nulsite
        dd      OFFSET oksite
        dd      OFFSET h23
realstart:
        mov     eax,12345678h           ; the second step stops after this
        mov     ebx,eax
        inc     ebx
bp1:    mov     ecx,ebx                 ; DBGTEST's breakpoint: EBX = 12345679h
        mov     dl,'T'
        mov     ah,2
        int     21h                     ; one 'T' on the screen
        mov     edx,OFFSET h23          ; a vector of its own, which its
        mov     eax,2523h               ; debugger must not be left with
        int     21h
int3site:
        int     3
        xor     esi,esi
nulsite:
        mov     eax,[esi]               ; page fault, address 0
oksite:
        mov     eax,4C07h               ; errorlevel 7
        int     21h
h23:    iretd

; the end of the image, as every D32 program has it
D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end     start
