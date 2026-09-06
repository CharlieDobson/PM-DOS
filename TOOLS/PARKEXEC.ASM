;=====================================================================
; PARKEXEC.ASM - PARKEXEC.COM, EXEC on the other side of the handover
;
; RMDOS's 4Bh was written in v0.45 and had never been called by
; anything until Windows called it in v0.50.  Two bugs fell out of it
; in the first hour; the third is still there, and the only instrument
; pointed at it is a bar painted into a 640x480 framebuffer by a DOS
; that has no console left, read off a screenshot, one boot at a time.
;
; SO TAKE WINDOWS OUT OF IT.  This program parks the kernel exactly as
; Windows does - INT 2Fh AX=1605h with DX bit 0 set - and then does
; what Windows does next, which is to EXEC a program.  The machine is
; in TEXT MODE, RMDOS's own console is answering AH=09h, and every
; answer is a printed line instead of a bar.
;
; It is EXECTST.COM's test run against the other DOS.  That is the
; whole idea: the two are meant to agree, so a check that passes here
; and fails there names the difference, and a check that fails in both
; was never about the handover at all.
;
;   ARGS.COM     a .COM child - the branch Windows takes second
;   HELLOX.EXE   a real MZ .EXE with relocations - the branch it
;                takes first, and the one with the arithmetic in it
;   NOSUCH.COM   a child that is not there, which must fail cleanly
;                rather than take the loader somewhere interesting
;
; and after each of them a 4Dh, because "came back" and "came back
; from the child that was asked for" are different claims.
;
; WHAT IT CANNOT DO is tell you why a child that starts never calls
; DOS again - it can only tell you that this one did, or did not.
; When it disagrees with Windows the next question is what Windows
; passes that this does not, and the answer will be in the parameter
; block: an environment, a tail, two FCBs.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_PE     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_PE, DS:_PE, ES:_PE, SS:_PE
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts

;--- give the rest of the machine back -------------------------------
; Twice necessary here.  The park needs somewhere to put RMDOS if it
; is running low, and the CHILD needs somewhere to go in either case:
; a .COM owns every free paragraph until it says otherwise, and a
; parent with nothing left to give is a 4Bh that fails for a reason
; that has nothing to do with the loader.  64K, and this file wants 2K.
        mov     bx,1000h
        mov     ah,4Ah
        int     21h                     ; ES is still the PSP
        jnc     pe_shrunk
        mov     dx,OFFSET msg_noshrink
        call    puts
        jmp     pe_end
pe_shrunk:

;--- who is answering now? -------------------------------------------
        mov     dx,OFFSET msg_pre
        call    puts
        call    sayoem

;=====================================================================
; The broadcast.  PM-DOS parks inside this INT and the next
; instruction runs in real mode on RMDOS - see PARKWIN.COM, which
; proves that much and nothing else.  Here it is only the setup.
;=====================================================================
        mov     dx,OFFSET msg_call
        call    puts

        push    ds
        push    es
        xor     ax,ax
        mov     es,ax
        xor     bx,bx
        xor     si,si
        mov     ds,ax
        xor     cx,cx
        mov     di,0A03h
        mov     dx,1                    ; bit 0: standard mode
        mov     ax,1605h
        int     2Fh
        mov     word ptr cs:[got_cx],cx
        pop     es
        pop     ds

        mov     dx,OFFSET msg_post
        call    puts
        call    sayoem

        smsw    ax
        test    al,1
        jz      pe_parked
        mov     dx,OFFSET msg_nopark
        call    puts
        jmp     pe_end
pe_parked:
        mov     ax,word ptr cs:[got_cx]
        or      ax,ax
        jz      pe_go
        mov     dx,OFFSET msg_vetoed
        call    puts
        jmp     pe_end

;=====================================================================
; From here down, every INT 21h is RMDOS's.
;=====================================================================
pe_go:
        push    cs
        pop     ds
        push    cs
        pop     es

        mov     dx,OFFSET msg_ready
        call    puts

;--- the parameter block, exactly as EXECTST builds it ---------------
        mov     word ptr [pb_env],0             ; 0 = a copy of ours
        mov     word ptr [pb_tail+2],cs
        mov     word ptr [pb_fcb1],5Ch
        mov     word ptr [pb_fcb1+2],cs
        mov     word ptr [pb_fcb2],6Ch
        mov     word ptr [pb_fcb2+2],cs

;=====================================================================
; 1 - a .COM child
;=====================================================================
        mov     dx,OFFSET msg_com
        call    puts
        mov     word ptr [pb_tail],OFFSET tail5
        mov     dx,OFFSET child_com
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        call    reload
        jc      pe_comfail
        mov     dx,OFFSET msg_back
        call    puts
        mov     ah,4Dh
        int     21h
        call    reload
        mov     dx,OFFSET msg_code
        call    puts
        call    put4
        call    crlf
        cmp     ax,0005h
        je      pe_comok
        mov     dx,OFFSET msg_codebad
        call    say_fail
        jmp     short pe_exe
pe_comok:
        mov     dx,OFFSET msg_comok
        call    say_ok
        jmp     short pe_exe
pe_comfail:
        mov     word ptr [err_ax],ax
        mov     dx,OFFSET msg_comfail
        call    say_fail
        mov     dx,OFFSET msg_err
        call    puts
        mov     ax,word ptr [err_ax]
        call    put4
        call    crlf

;=====================================================================
; 2 - an .EXE child.  A real MZ header, a relocation table, and an
;     initial SS:SP of its own, which is where the loader's arithmetic
;     lives and where a wrong answer is a child that runs on somebody
;     else's stack.
;=====================================================================
pe_exe:
        mov     dx,OFFSET msg_exe
        call    puts
        mov     word ptr [pb_tail],OFFSET tailnone
        mov     dx,OFFSET child_exe
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        call    reload
        jc      pe_exefail
        mov     dx,OFFSET msg_back
        call    puts
        mov     ah,4Dh
        int     21h
        call    reload
        mov     dx,OFFSET msg_code
        call    puts
        call    put4
        call    crlf
        or      ax,ax
        jnz     pe_exebadcode
        mov     dx,OFFSET msg_exeok
        call    say_ok
        jmp     short pe_miss
pe_exebadcode:
        mov     dx,OFFSET msg_codebad
        call    say_fail
        jmp     short pe_miss
pe_exefail:
        mov     word ptr [err_ax],ax
        mov     dx,OFFSET msg_exefail
        call    say_fail
        mov     dx,OFFSET msg_err
        call    puts
        mov     ax,word ptr [err_ax]
        call    put4
        call    crlf

;=====================================================================
; 3 - a child that is not there
;=====================================================================
pe_miss:
        mov     dx,OFFSET msg_miss
        call    puts
        mov     word ptr [pb_tail],OFFSET tailnone
        mov     dx,OFFSET child_no
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        call    reload
        jnc     pe_missbad
        cmp     ax,2
        jne     pe_missbad
        mov     dx,OFFSET msg_missok
        call    say_ok
        jmp     short pe_done
pe_missbad:
        mov     dx,OFFSET msg_missbad
        call    say_fail

pe_done:
        mov     dx,OFFSET msg_tally
        call    puts
        mov     al,byte ptr [fails]
        call    put2
        call    crlf

pe_end:
        mov     dx,OFFSET msg_bye
        call    puts
        mov     ax,4C00h
        int     21h                     ; and this is what unparks

;---------------------------------------------------------------------
; reload - DS and ES back to CS.
;
; NOT HOUSEKEEPING.  4Bh returns with every register but CS:IP and
; SS:SP undefined - that is the specification, and RMDOS is entitled
; to take it literally - so a message printed before this runs is a
; message from whatever segment the loader happened to leave behind.
; The flags and AX are the answer and have to survive it.
;---------------------------------------------------------------------
reload:
        pushf
        push    cs
        pop     ds
        push    cs
        pop     es
        popf
        ret

say_ok:
        call    puts
        ret
say_fail:
        call    puts
        inc     byte ptr cs:[fails]
        ret

;---------------------------------------------------------------------
; sayoem - AH=30h's OEM byte, which is the one thing that says WHICH
;          DOS answered: PM-DOS 03, RMDOS FF.
;---------------------------------------------------------------------
sayoem:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     ah,30h
        int     21h
        mov     ax,bx
        call    put4
        call    crlf
        pop     dx
        pop     cx
        pop     bx
        pop     ax
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
crlf:
        push    ax
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
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
put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:
        push    ax
        shr     al,4
        call    put1
        pop     ax
put1:
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1_go
        add     al,7
p1_go:
        mov     dl,al
        call    putc
        pop     ax
        ret

got_cx      dw  0
err_ax      dw  0
fails       db  0

child_com   db  'PEKID.COM',0
child_exe   db  'HELLOX.EXE',0
child_no    db  'NOSUCH.COM',0
tail5       db  2,' 5',0Dh
tailnone    db  0,0Dh

pb_env      dw  0
pb_tail     dd  0
pb_fcb1     dd  0
pb_fcb2     dd  0

msg_hi      db  13,10,'PARKEXEC - EXEC on the far side of the handover',13,10,13,10,'$'
msg_noshrink db 'FAILED: 4Ah would not shrink us - nothing to give a child.',13,10,'$'
msg_pre     db  'before the broadcast, the DOS answering says BX = $'
msg_call    db  13,10,'broadcasting 1605h, standard mode...',13,10,13,10,'$'
msg_post    db  'after  the broadcast, the DOS answering says BX = $'
msg_nopark  db  'FAILED: still in protected mode - no handover happened.',13,10,'$'
msg_vetoed  db  'FAILED: vetoed, CX came back non-zero.',13,10,'$'
msg_ready   db  13,10,'RMDOS has the machine.  Now EXEC, which is the point.',13,10,13,10,'$'
msg_com     db  '1  PEKID.COM  (.COM - marks the screen before it asks DOS)',13,10,'$'
msg_exe     db  13,10,'2  HELLOX.EXE  (.EXE, relocations, own stack)',13,10,'$'
msg_miss    db  13,10,'3  NOSUCH.COM  (must fail, and fail cleanly)',13,10,'$'
msg_back    db  '   the child ended and we are running again',13,10,'$'
msg_code    db  '   4Dh says $'
msg_err     db  '   AX = $'
msg_comok   db  '   ok - exit code 5, which is what its tail asked for',13,10,'$'
msg_exeok   db  '   ok - exit code 0',13,10,'$'
msg_missok  db  '   ok - AX=2, file not found',13,10,'$'
msg_comfail db  '   FAILED: 4Bh returned carry',13,10,'$'
msg_exefail db  '   FAILED: 4Bh returned carry',13,10,'$'
msg_codebad db  '   FAILED: wrong exit code',13,10,'$'
msg_missbad db  '   FAILED: a missing child did not report "not found"',13,10,'$'
msg_tally   db  13,10,'failures: $'
msg_crlf    db  13,10,'$'
msg_bye     db  13,10,'exiting - the DOS that answers this 4Ch unparks.',13,10,'$'

_PE     ENDS
        END     start
