;=====================================================================
; HELLOX.ASM - HELLOX.EXE, an ordinary 16-bit DOS .EXE
;
; HELLO.COM proved the VDM could run a flat 64K image.  This proves it
; can run a RELOCATED one, which is a different claim: an .EXE arrives
; with its segment references unresolved and the loader has to patch
; every one of them before the first instruction executes.
;
; So this file is deliberately built out of four separate segments -
; two code, one data, one stack - and reaches across all of them:
;
;   mov ax,SEG _DATA        a segment fixup in the load module
;   SS:SP from the header,  itself relocated
;   CS:IP from the header,  itself relocated
;   a far call to a SECOND code segment, which is a 9Ah with a real
;     segment fixup - a far call inside one segment is not, because
;     the assembler quietly makes it near and nothing gets tested
;   a table of far pointers walked at run time, so that several
;     fixups have to be right rather than one
;
; If the fixup table were skipped, or applied relative to the PSP
; instead of PSP+10h, this would print nothing and wander off.  A
; program that runs at all has been relocated correctly.
;
; Linked with:  wlink system dos file hellox.obj name hellox.exe
;=====================================================================
        .386

STACK   SEGMENT USE16 STACK 'STACK'
        db      512 dup(0)
STACK   ENDS

_DATA   SEGMENT USE16 PUBLIC 'DATA'
msg_hi      db  'HELLOX.EXE here - a relocated 16-bit .EXE.',13,10,'$'
msg_seg     db  '  my code segment relocated to $'
msg_data    db  '  my data segment relocated to $'
msg_stack   db  '  my stack segment relocated to $'
msg_far     db  '  a far call inside the image returned',13,10,'$'
msg_psp     db  '  INT 21h/62h says my PSP is $'
msg_free    db  '  INT 21h/48h with BX=FFFF says the biggest block is $'
msg_bye     db  'HELLOX.EXE exiting through AH=4Ch.',13,10,'$'
msg_walk    db  '  four far pointers followed: $'
msg_p1      db  'one $'
msg_p2      db  'two $'
msg_p3      db  'three $'
msg_p4      db  'four',13,10,'$'

; Four far pointers into our own data.  Every one of these is a
; segment fixup the loader has to patch, so following the table proves
; the fixup loop, not just the header relocation.
farptrs     dd  msg_p1, msg_p2, msg_p3, msg_p4
_DATA   ENDS

;--- a second code segment, so the far call has to be a real one -------
_TEXT2  SEGMENT USE16 PUBLIC 'CODE2'
        ASSUME  CS:_TEXT2
faraway:
        nop
        retf
_TEXT2  ENDS

_TEXT   SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_TEXT, DS:_DATA, SS:STACK

;--- entry ------------------------------------------------------------
; DS and ES address the PSP on entry, exactly as DOS leaves them, so
; the first job of every .EXE is to go and find its own data.  That
; "mov ax,SEG _DATA" is the relocation this whole program exists to
; test: the linker cannot know the number, so it writes a placeholder
; and lists the address in the fixup table.
start:
        mov     ax,SEG _DATA
        mov     ds,ax

        mov     dx,OFFSET msg_hi
        mov     ah,09h
        int     21h

        mov     dx,OFFSET msg_seg               ; where did CS land?
        mov     ah,09h
        int     21h
        mov     ax,cs
        call    put4
        call    crlf

        mov     dx,OFFSET msg_data              ; and DS?
        mov     ah,09h
        int     21h
        mov     ax,ds
        call    put4
        call    crlf

        mov     dx,OFFSET msg_stack             ; and SS?
        mov     ah,09h
        int     21h
        mov     ax,ss
        call    put4
        call    crlf

;--- a far call, so a wrong CS shows up immediately --------------------
        call    FAR PTR faraway
        mov     dx,OFFSET msg_far
        mov     ah,09h
        int     21h

;--- 62h: our PSP, which should be one paragraph below the load segment
        mov     ah,62h
        int     21h
        push    bx
        mov     dx,OFFSET msg_psp
        mov     ah,09h
        int     21h
        pop     ax
        call    put4
        call    crlf

;--- 48h with BX=FFFF: the standard way to ask "how much is left?" -----
; It always fails, and the answer is in BX.  An .EXE that asked for
; MINALLOC and got a whole block should find something left over.
        mov     bx,0FFFFh
        mov     ah,48h
        int     21h
        push    bx
        mov     dx,OFFSET msg_free
        mov     ah,09h
        int     21h
        pop     ax
        call    put4
        call    crlf

;--- follow the far pointer table --------------------------------------
; Each entry was written by the linker as offset:placeholder and had
; its segment half patched by the loader.  Printing all four through
; a far pointer means four separate fixups landed correctly.
        mov     dx,OFFSET msg_walk
        mov     ah,09h
        int     21h
        mov     si,OFFSET farptrs
        mov     cx,4
walk:
        push    cx
        push    si
        lds     dx,[si]                 ; DS:DX = the string, both halves
        mov     ah,09h                  ; from the table
        int     21h
        mov     ax,SEG _DATA            ; lds trampled DS; put it back
        mov     ds,ax
        pop     si
        pop     cx
        add     si,4
        loop    walk

        mov     dx,OFFSET msg_bye
        mov     ah,09h
        int     21h
        mov     ax,4C00h
        int     21h

;--- helpers ----------------------------------------------------------
put4:                                   ; AX in hex
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:                                   ; AL in hex
        push    ax
        shr     al,4
        call    put1
        pop     ax
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1:
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        push    ax
        mov     ah,02h
        int     21h
        pop     ax
        ret
crlf:
        mov     dl,13
        mov     ah,02h
        int     21h
        mov     dl,10
        mov     ah,02h
        int     21h
        ret

_TEXT   ENDS
        END     start
