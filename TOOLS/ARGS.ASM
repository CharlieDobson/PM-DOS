;=====================================================================
; ARGS.ASM - ARGS.COM, a 16-bit DOS program that reports what it was
;            given: the command tail, the two parsed FCBs, the AX the
;            loader left it, and its environment block.
;
; HELLO.COM proved a DOS box could RUN.  This one proves it was
; STARTED PROPERLY - which is a different question, and the one that
; matters once COMMAND.COM is the thing doing the starting.  Everything
; here comes out of the PSP the kernel built:
;
;   PSP+80h  the command tail, as typed after the program's name
;   PSP+5Ch  the first argument, parsed into an FCB
;   PSP+6Ch  the second, the same
;   AX       the two FCB parse codes, which a program must read before
;            it does anything else - the first INT 21h destroys them
;   PSP+2Ch  the environment segment, and at the end of that block the
;            program's own fully-qualified path
;
; It exits with ERRORLEVEL taken from the first digit of its tail, so
; that a batch file can prove the exit code comes back through EXEC.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_ARGS   SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_ARGS, DS:_ARGS, ES:_ARGS, SS:_ARGS
        ORG     0

start:
        ; FIRST, before any DOS call: AX holds the two FCB codes and
        ; nothing preserves them.  This is the oldest convention in
        ; DOS and the easiest one to lose.
        mov     word ptr cs:[fcb_ax],ax

        mov     dx,OFFSET msg_hi
        mov     ah,09h
        int     21h

;--- the command tail -------------------------------------------------
        mov     dx,OFFSET msg_tail
        mov     ah,09h
        int     21h
        mov     dl,'['
        mov     ah,02h
        int     21h
        mov     si,80h
        movzx   cx,byte ptr [si]
        inc     si
        jcxz    tail_done
tail_ch:
        mov     al,[si]
        inc     si
        push    cx
        push    si
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     si
        pop     cx
        loop    tail_ch
tail_done:
        mov     dl,']'
        mov     ah,02h
        int     21h
        call    crlf

;--- and what the loader made of it -----------------------------------
        mov     dx,OFFSET msg_ax
        mov     ah,09h
        int     21h
        mov     ax,word ptr [fcb_ax]
        call    put4
        call    crlf

        mov     dx,OFFSET msg_fcb1
        mov     ah,09h
        int     21h
        mov     si,5Ch
        call    put_fcb

        mov     dx,OFFSET msg_fcb2
        mov     ah,09h
        int     21h
        mov     si,6Ch
        call    put_fcb

;--- the environment --------------------------------------------------
        mov     dx,OFFSET msg_env
        mov     ah,09h
        int     21h
        ; ds: IS LOAD-BEARING.  A bare [2Ch] is an immediate expression
        ; to wasm, not a memory reference, and this printed 002C - the
        ; offset itself - until the override went in.
        mov     ax,word ptr ds:[2Ch]
        push    ax
        call    put4
        call    crlf
        pop     ax
        or      ax,ax
        jnz     env_walk
        mov     dx,OFFSET msg_noenv
        mov     ah,09h
        int     21h
        jmp     short env_done

env_walk:
        mov     es,ax
        xor     si,si
env_str:
        mov     al,es:[si]
        or      al,al
        jz      env_end                 ; the empty string ends them
        mov     dx,OFFSET msg_ind
        mov     ah,09h
        int     21h
env_ch:
        mov     al,es:[si]
        inc     si
        or      al,al
        jz      env_eol
        mov     dl,al
        mov     ah,02h
        int     21h
        jmp     short env_ch
env_eol:
        call    crlf
        jmp     short env_str
env_end:
        inc     si                      ; past the empty string
        mov     ax,es:[si]              ; the count of what follows
        add     si,2
        cmp     ax,1
        jne     env_done                ; DOS 2 had no path here
        mov     dx,OFFSET msg_path
        mov     ah,09h
        int     21h
path_ch:
        mov     al,es:[si]
        inc     si
        or      al,al
        jz      path_end
        mov     dl,al
        mov     ah,02h
        int     21h
        jmp     short path_ch
path_end:
        call    crlf
env_done:

;--- /B: touch a drive that is not there ------------------------------
; The point is not the error - it is what happens when the KERNEL
; raises INT 24h on behalf of a guest.  The shell's own handler asks
; the question, and Abort has to kill the DOS box and nothing else.
        mov     al,'B'
        call    has_switch
        jc      no_bad
        mov     dx,OFFSET msg_bad
        mov     ah,09h
        int     21h
        mov     dx,OFFSET badname
        mov     ax,3D00h
        int     21h
        mov     dx,OFFSET msg_badno
        jc      bad_said
        mov     bx,ax
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET msg_badyes
bad_said:
        mov     ah,09h
        int     21h
no_bad:

;--- /W: wait for a key, so there is something to interrupt -----------
        mov     al,'W'
        call    has_switch
        jc      no_wait
        mov     dx,OFFSET msg_wait
        mov     ah,09h
        int     21h
        mov     ah,08h                  ; blocking, no echo - and where a
        int     21h                     ; ^C reaches the kernel's check
        call    crlf
no_wait:

;--- exit with whatever the tail asked for ----------------------------
        mov     si,81h                  ; past the length byte
        mov     cx,0Ah
find_dig:
        jcxz    exit_zero
        dec     cx
        mov     al,[si]
        inc     si
        cmp     al,0Dh
        je      exit_zero
        cmp     al,'0'
        jb      find_dig
        cmp     al,'9'
        ja      find_dig
        sub     al,'0'
        mov     ah,4Ch
        int     21h
exit_zero:
        mov     ax,4C00h
        int     21h

;--- helpers ----------------------------------------------------------
; has_switch - CF CLEAR if "/AL" appears in the command tail.  Case is
;              folded, so /w and /W are the same switch.
has_switch:
        push    si
        push    cx
        push    dx
        mov     dl,al
        or      dl,20h
        mov     si,81h
        mov     cx,80h
hs_ch:
        mov     al,[si]
        inc     si
        cmp     al,0Dh
        je      hs_no
        cmp     al,'/'
        jne     hs_next
        mov     al,[si]
        or      al,20h
        cmp     al,dl
        je      hs_yes
hs_next:
        loop    hs_ch
hs_no:
        stc
        jmp     short hs_out
hs_yes:
        clc
hs_out:
        pop     dx
        pop     cx
        pop     si
        ret

; put_fcb - the FCB at DS:SI: its drive number, then its eleven name
;           bytes exactly as they are, blanks and all.
put_fcb:
        mov     al,[si]
        call    put2
        mov     dl,' '
        mov     ah,02h
        int     21h
        mov     dl,'['
        mov     ah,02h
        int     21h
        inc     si
        mov     cx,11
fcb_ch:
        mov     al,[si]
        inc     si
        push    cx
        push    si
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     si
        pop     cx
        loop    fcb_ch
        mov     dl,']'
        mov     ah,02h
        int     21h
        call    crlf
        ret

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

fcb_ax      dw  0
msg_hi      db  'ARGS.COM - what a 16-bit program was handed:',13,10,'$'
msg_tail    db  '  tail  $'
msg_ax      db  '  AX    $'
msg_fcb1    db  '  FCB1  $'
msg_fcb2    db  '  FCB2  $'
msg_env     db  '  env   $'
msg_noenv   db  '  (no environment block)',13,10,'$'
msg_ind     db  '    $'
msg_path    db  '  path  $'
msg_bad     db  '  /B: opening B:\NOSUCH.TXT',13,10,'$'
msg_badno   db  '  the open failed, and the guest is still running',13,10,'$'
msg_badyes  db  '  the open SUCCEEDED',13,10,'$'
msg_wait    db  '  /W: waiting for a key (^C should kill just me)',13,10,'$'
badname     db  'B:\NOSUCH.TXT',0

_ARGS   ENDS
        END
