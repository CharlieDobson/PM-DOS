;=====================================================================
; TICKS.ASM - TICKS.COM: the BIOS tick count, as a number, and nothing
;             else.  For timing a batch file from the outside: two of
;             these around a command, and the difference is in 55ms
;             ticks.  40:6C is read directly, so it is the count the
;             machine has, whichever DOS is answering.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_TK     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_TK, DS:_TK, ES:_TK, SS:_TK
        ORG     0

start:
        mov     dx,OFFSET msg_ticks
        mov     ah,09h
        int     21h
        push    ds
        mov     ax,40h
        mov     ds,ax
        mov     eax,dword ptr ds:[6Ch]
        pop     ds
        mov     di,OFFSET decbuf + 11
        mov     byte ptr [di],'$'
        mov     ecx,10
tk_loop:
        xor     edx,edx
        div     ecx
        add     dl,'0'
        dec     di
        mov     [di],dl
        or      eax,eax
        jnz     tk_loop
        mov     dx,di
        mov     ah,09h
        int     21h
        mov     dx,OFFSET msg_crlf
        mov     ah,09h
        int     21h
        mov     ax,4C00h
        int     21h

msg_ticks   db  'ticks $'
msg_crlf    db  13,10,'$'
decbuf      db  12 dup(0)

_TK     ENDS
        END
