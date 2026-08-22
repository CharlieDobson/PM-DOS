;=====================================================================
; ALLOCTST.ASM - ALLOCTST.COM, does memory a program asked for come
;                back when the program does not?
;
; A PROGRAM THAT EXITS WITHOUT FREEING ITS OWN MEMORY IS THE NORMAL
; CASE, NOT THE EXCEPTION - almost nothing calls 49h on the way out,
; because DOS has always swept up afterwards.  PM-DOS did not, for
; blocks got through 21h/48h: lm_alloc leaves a block owning ITSELF,
; which is right for a program's own block (its PSP is the first
; paragraph, so "owner = block + 1" IS the PSP) and wrong for memory a
; program merely asks for.  The exit sweep frees by PSP, a self-owned
; block never matched one, and every 48h a DOS box ever made stood
; there until the machine was switched off.
;
; NOTHING COMPLAINED, and that is the point of writing a test for it.
; A leak is not an error message.  It is a largest-free-block that
; keeps getting smaller, and it is invisible until some later program
; needs a big one and is told no.  MSD takes two of these; MEM after
; it reported 604K free with the biggest run 309K, which is the shape
; a leak makes and reads like a memory-reporting bug.
;
; So this asks the question in ONE run, by being both halves of it:
;
;   the parent measures the largest free block, EXECs ITSELF with a
;   tail that says "be the child", and measures again.  The child
;   allocates two blocks and exits without freeing either.  The two
;   measurements must match.
;
; Measuring is 48h with BX = FFFFh, which cannot succeed and answers
; with the largest run there is - the one call in DOS whose FAILURE is
; the answer.
;
; The second check is the plain one it would be wrong to leave out: a
; block asked for and freed by the SAME program comes back.  That
; separates "the sweep is broken" from "free is broken", which look
; identical from outside.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_AL     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_AL, DS:_AL, ES:_AL, SS:_AL
        ORG     0

STACK_BYTES     EQU     512
CHILD_BLK1      EQU     100h            ; 4K, and 8K - two of them, so a
CHILD_BLK2      EQU     200h            ; single missing free cannot pass
                                        ; by cancelling out against a
                                        ; single spurious one

start:
        ; DS is the PSP on a .COM, so the tail is at 81h.  One space
        ; and a C means "you are the child".
        mov     si,81h
        mov     cl,[si-1]               ; the tail length at 80h
        xor     ch,ch
        jcxz    be_parent
ch_scan:
        lodsb
        cmp     al,' '
        je      ch_next
        cmp     al,'C'
        je      be_child
        cmp     al,'c'
        je      be_child
        jmp     short be_parent
ch_next:
        loop    ch_scan

;=====================================================================
; THE CHILD.  Allocate, say so, and exit WITHOUT freeing - which is
; the whole of its job and the thing every real program does.
;=====================================================================
be_child:
        ; SHRINK FIRST OR THERE IS NOTHING TO ASK FOR.  A .COM is given
        ; every free paragraph DOS has, so a .COM that calls 48h
        ; without giving its unused memory back is asking for what it
        ; is already holding, and is refused.  That is DOS behaving
        ; correctly and it is the first thing every program that
        ; allocates has to do - the parent below does the same, for
        ; room to load this child into.
        mov     bx,OFFSET prog_end
        add     bx,STACK_BYTES
        add     bx,15
        shr     bx,4
        add     bx,10h
        mov     ah,4Ah
        int     21h
        jc      child_nomem
        cli
        mov     sp,OFFSET prog_end
        add     sp,STACK_BYTES
        sti

        mov     bx,CHILD_BLK1
        mov     ah,48h
        int     21h
        jc      child_nomem
        mov     bx,CHILD_BLK2
        mov     ah,48h
        int     21h
        jc      child_nomem
        mov     ax,4C00h                ; and leave them both behind
        int     21h
child_nomem:
        mov     ax,4C01h
        int     21h

;=====================================================================
; THE PARENT.
;=====================================================================
be_parent:
        mov     dx,OFFSET msg_hi
        call    puts

;--- give the child somewhere to be loaded ---------------------------
; A .COM owns every free paragraph DOS had, so there is nothing left
; to EXEC into until this hands the rest back.  EXECTST makes the same
; call for the same reason.
        mov     bx,OFFSET prog_end
        add     bx,STACK_BYTES
        add     bx,15
        shr     bx,4
        add     bx,10h                  ; and the PSP in front of us
        mov     ah,4Ah
        int     21h
        jc      no_shrink
        cli
        mov     sp,OFFSET prog_end
        add     sp,STACK_BYTES
        sti

;=====================================================================
; 1 - a block asked for and freed by the same program comes back.
;=====================================================================
        mov     dx,OFFSET t_free
        call    puts
        call    largest
        mov     [before],ax

        mov     bx,CHILD_BLK1
        mov     ah,48h
        int     21h
        jc      al_f_bad
        mov     es,ax
        mov     ah,49h
        int     21h
        jc      al_f_bad

        call    largest
        cmp     ax,[before]
        jne     al_f_bad
        call    ok
        jmp     short al_exec
al_f_bad:
        call    show2
        call    bad

;=====================================================================
; 2 - and the child's blocks come back when the CHILD does not free
;     them.  This is the one that was failing.
;=====================================================================
al_exec:
        mov     dx,OFFSET t_sweep
        call    puts
        call    largest
        mov     [before],ax

        push    cs
        pop     ds
        push    cs
        pop     es
        mov     word ptr [pb_env],0             ; inherit ours
        mov     word ptr [pb_tail],OFFSET tail
        mov     word ptr [pb_tail+2],cs
        mov     word ptr [pb_fcb1],5Ch
        mov     word ptr [pb_fcb1+2],cs
        mov     word ptr [pb_fcb2],6Ch
        mov     word ptr [pb_fcb2+2],cs

        mov     dx,OFFSET childpath
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        ; 4Bh returns with every register but CS:IP and SS:SP
        ; undefined, and a program that assumes otherwise works until
        ; it runs somewhere that takes the specification literally.
        push    cs
        pop     ds
        push    cs
        pop     es
        jc      al_s_noexec

        mov     ah,4Dh                  ; and did it get that far?
        int     21h
        cmp     al,0
        jne     al_s_nochild

        call    largest
        cmp     ax,[before]
        jne     al_s_bad
        call    ok
        jmp     short al_done
al_s_bad:
        call    show2
        call    bad
        jmp     short al_done
al_s_noexec:
        mov     dx,OFFSET msg_noexec
        call    puts
        call    bad
        jmp     short al_done
al_s_nochild:
        mov     dx,OFFSET msg_nochild
        call    puts
        call    bad

al_done:
        mov     dx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        jne     al_failed
        mov     dx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h
al_failed:
        mov     dx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h

no_shrink:
        mov     dx,OFFSET msg_noshrink
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; helpers
;=====================================================================
; largest - AX = the largest free block, in paragraphs.  48h asking for
;           64K-worth of paragraphs cannot be satisfied, and DOS
;           answers a refusal by putting the biggest run it HAS in BX.
largest:
        push    bx
        mov     bx,0FFFFh
        mov     ah,48h
        int     21h
        jnc     lg_got                  ; it succeeded?  then take it back
        mov     ax,bx                   ; and BX is the answer
        pop     bx
        ret
lg_got:
        push    ax
        mov     es,ax
        mov     ah,49h
        int     21h
        pop     ax
        pop     bx
        ret

; show2 - the two measurements, so a failure says by how much
show2:
        push    ax
        push    dx
        mov     dx,OFFSET msg_was
        call    puts
        mov     ax,[before]
        call    put4
        mov     dx,OFFSET msg_now
        call    puts
        pop     dx
        pop     ax
        push    ax
        call    put4
        pop     ax
        ret

ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     dx
        ret

puts:
        push    ax
        push    ds
        push    cs
        pop     ds
        mov     ah,09h
        int     21h
        pop     ds
        pop     ax
        ret
putc:
        push    ax
        push    dx
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret
put4:                                   ; AX in hex
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:
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
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        call    putc
        ret

;=====================================================================
; data
;=====================================================================
before      dw  0
fails       db  0
pb_env      dw  0
pb_tail     dd  0
pb_fcb1     dd  0
pb_fcb2     dd  0
tail        db  2,' C',0Dh
childpath   db  'C:\ALLOCTST.COM',0

msg_hi      db  13,10,'ALLOCTST - does a program''s memory come back?',13,10
            db  '(the leak is silent: it is a largest-free-block that shrinks)'
            db  13,10,13,10,'$'
t_free      db  '  1 asked for and freed by the same program  $'
t_sweep     db  '  2 asked for by a child that never freed it $'
msg_was     db  ' was $'
msg_now     db  ' now $'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_noexec  db  ' could not run the child$'
msg_nochild db  ' the child reported a problem$'
msg_noshrink db 13,10,'ALLOCTST: 4Ah would not shrink us; no room to EXEC',13,10,'$'
msg_pass    db  'ALLOCTST: all checks passed',13,10,'$'
msg_fail    db  'ALLOCTST: FAILURES - see above',13,10,'$'

prog_end    equ $

_AL     ENDS
        END     start
