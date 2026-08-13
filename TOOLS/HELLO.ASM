;=====================================================================
; HELLO.ASM - HELLO.COM, an ordinary 16-bit DOS program
;
; Nothing about this file knows it is being run by a 32-bit kernel in
; virtual-8086 mode, and that is the entire point.  It is assembled as
; a plain real-mode .COM and it does what a DOS program does: prints
; with AH=09h, asks DOS for its version and its PSP, allocates and
; frees memory, opens a file and reads it, and exits with AH=4Ch.
;
; Every one of those is a different path through the VDM bridge:
;   09h  a DS:DX pointer translated to flat
;   30h  a register-only call passed straight through
;   62h  intercepted, because a PSP is a segment here
;   48h/49h  intercepted, because the guest's memory is the DOS arena
;   3Dh/3Fh/3Eh  the kernel's real file handlers, reached from 16-bit
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
; (ORDER, not "option offset=" - wlink rounds that up to 4K.)
;=====================================================================
        .386
_HELLO  SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_HELLO, DS:_HELLO, ES:_HELLO, SS:_HELLO
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        mov     ah,09h
        int     21h

;--- 30h: the DOS version --------------------------------------------
        mov     ah,30h
        int     21h
        push    ax
        mov     dx,OFFSET msg_ver
        mov     ah,09h
        int     21h
        pop     ax
        push    ax
        and     al,0Fh                  ; crude, but the majors here are
        add     al,'0'                  ; single digits
        mov     dl,al
        mov     ah,02h
        int     21h
        mov     dl,'.'
        mov     ah,02h
        int     21h
        pop     ax
        mov     al,ah
        call    put2
        call    crlf

;--- 62h: our own PSP ------------------------------------------------
        mov     ah,62h
        int     21h
        push    bx
        mov     dx,OFFSET msg_psp
        mov     ah,09h
        int     21h
        pop     ax
        call    put4
        call    crlf

;--- 4Ah: give back what we do not need ------------------------------
; A .COM is handed every free paragraph DOS has, so until it shrinks
; its own block there is nothing left to allocate.  Every real .COM
; that wants memory does this first, and so does this one.
        mov     bx,1000h                ; keep 64K, hand back the rest
        mov     ah,4Ah
        int     21h
        mov     dx,OFFSET msg_shrink
        jc      shrink_bad
        mov     dx,OFFSET msg_shrunk
shrink_bad:
        mov     ah,09h
        int     21h

;--- 48h / 49h: a block of memory ------------------------------------
        mov     bx,64                   ; 1K
        mov     ah,48h
        int     21h
        jc      no_mem
        push    ax
        mov     dx,OFFSET msg_mem
        mov     ah,09h
        int     21h
        pop     ax
        push    ax
        call    put4
        call    crlf
        pop     ax
        mov     es,ax
        mov     ah,49h
        int     21h
        jmp     short do_file
no_mem:
        mov     dx,OFFSET msg_nomem
        mov     ah,09h
        int     21h

;--- 3Dh / 3Fh / 3Eh: read a file the kernel owns --------------------
do_file:
        mov     dx,OFFSET fname
        mov     ax,3D00h
        int     21h
        jc      no_file
        mov     bx,ax
        mov     dx,OFFSET buf
        mov     cx,64
        mov     ah,3Fh
        int     21h
        jc      no_file
        mov     si,OFFSET buf           ; print what came back, stopping
        mov     cx,ax                   ; at the first control character
        jcxz    shut
show:
        lodsb
        cmp     al,' '
        jb      shut
        mov     dl,al
        push    cx
        push    si
        mov     ah,02h
        int     21h
        pop     si
        pop     cx
        loop    show
shut:
        call    crlf
        mov     ah,3Eh
        int     21h
        jmp     short done
no_file:
        mov     dx,OFFSET msg_nofile
        mov     ah,09h
        int     21h

done:
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

msg_hi      db  'HELLO.COM here - a 16-bit .COM, running on PM-DOS.',13,10,'$'
msg_ver     db  '  INT 21h/30h says DOS version $'
msg_psp     db  '  INT 21h/62h says my PSP is $'
msg_shrunk  db  '  INT 21h/4Ah shrank me to 64K',13,10,'$'
msg_shrink  db  '  INT 21h/4Ah refused to shrink me',13,10,'$'
msg_mem     db  '  INT 21h/48h gave me a block at $'
msg_nomem   db  '  INT 21h/48h refused',13,10,'$'
msg_nofile  db  '  could not read README.TXT',13,10,'$'
msg_bye     db  'HELLO.COM exiting through AH=4Ch.',13,10,'$'
fname       db  '\README.TXT',0     ; no drive letter - whichever we booted
buf         db  64 dup(0)

_HELLO  ENDS
        END
