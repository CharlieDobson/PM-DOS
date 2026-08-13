;=====================================================================
; TSRTEST.ASM - TSRTEST.COM, a 16-bit DOS program that stays resident.
;
; It hooks INT 2Fh, the DOS multiplex, and answers one identifier with
; a signature.  Then it terminates with 21h/31h and does not give its
; memory back.
;
; Run it twice.  The first run installs; the second finds the first
; still there and says so - AND THE SECOND RUN IS A DIFFERENT DOS BOX.
; That is the whole test.  A PM-DOS box is created to run a program
; and destroyed when it exits, so "resident" could easily have meant
; "resident for the eight milliseconds until the box goes away".  It
; does not, because the conventional memory arena and the real-mode
; interrupt vector table belong to the machine and not to the box: the
; block stays allocated, the vector stays hooked, and the monitor
; reflects the next box's INT 2Fh straight into code that was loaded
; by a box that no longer exists.
;
; INT 2Fh IS THE RIGHT VECTOR TO TEST WITH, not a convenience.  The
; kernel claims 2Fh for XMS and hands back everything it does not
; recognise, so an answer arriving here also proves that decline-and-
; reflect path works with a guest's own handler on the other end.
; Identifier C0h is in the range DOS left for applications.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_TSR    SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_TSR, DS:_TSR, ES:_TSR, SS:_TSR
        ORG     0

MPX_ID          EQU     0C0h            ; our multiplex identifier
SIG_LO          EQU     'MT'            ; what an installed copy answers
SIG_HI          EQU     'RS'
STACK_BYTES     EQU     256
BUFFER_BYTES    EQU     8192            ; kept, so that MEM has something
                                        ; to show

start:
        jmp     setup                   ; the resident part comes first,
                                        ; so that what stays is at the
                                        ; front of the block and the
                                        ; setup code is what gets cut off

;=====================================================================
; THE RESIDENT PART.  Everything from here to resident_end survives.
;=====================================================================
old_2F      dd  0                       ; whoever had the vector before us
hit_count   dw  0                       ; how many times we have answered

int2f:
        cmp     ah,MPX_ID
        je      i2f_ours
        jmp     dword ptr cs:[old_2F]   ; not ours - and passing it on is
                                        ; the whole etiquette of 2Fh
i2f_ours:
        cmp     al,00h                  ; 00h - are you there?
        jne     i2f_next
        inc     word ptr cs:[hit_count]
        mov     al,0FFh                 ; the documented "installed"
        mov     bx,SIG_LO               ; and a signature, so that two
        mov     cx,SIG_HI               ; different programs using the
                                        ; same identifier can tell
        ; WHERE we are, which is the part that makes the test mean
        ; anything.  If the resident block had been handed back, the
        ; next copy of this program would be loaded at the same address
        ; - it is the first free run - and would find its OWN code at
        ; the hooked vector, answer itself, and pass.  Two copies at
        ; two different segments is the only proof that the first one
        ; is still there.
        mov     dx,cs
        iret
i2f_next:
        cmp     al,01h                  ; 01h - how many times asked?
        jne     i2f_no
        mov     bx,word ptr cs:[hit_count]
        mov     al,0FFh
        iret
i2f_no:
        xor     al,al                   ; a subfunction we do not do
        iret

resident_end    label byte

;=====================================================================
; THE TRANSIENT PART.  Runs once and is thrown away.
;=====================================================================
setup:
        mov     dx,OFFSET msg_hi
        mov     ah,09h
        int     21h

;--- is a copy already resident? -------------------------------------
        mov     ah,MPX_ID
        mov     al,00h
        xor     bx,bx
        xor     cx,cx
        xor     dx,dx
        int     2Fh
        mov     word ptr [tmp],dx               ; where it says it lives
        cmp     al,0FFh
        jne     not_there
        cmp     bx,SIG_LO
        jne     not_ours
        cmp     cx,SIG_HI
        jne     not_ours

;--- found one, and it answered --------------------------------------
        mov     dx,OFFSET msg_found
        call    say_ok

;--- but is it a DIFFERENT copy, or did we answer ourselves? ---------
        mov     word ptr [tmp2],cs      ; CS compares against memory, not
        mov     ax,word ptr [tmp]       ; against a register - a segment
        cmp     ax,word ptr [tmp2]      ; register is not a CMP operand
        jne     other_copy
        mov     dx,OFFSET msg_selfans
        call    say_fail
        jmp     short did_where
other_copy:
        mov     dx,OFFSET msg_where
        mov     ah,09h
        int     21h
        mov     ax,word ptr [tmp]
        call    put4
        mov     dx,OFFSET msg_weare
        mov     ah,09h
        int     21h
        mov     ax,word ptr [tmp2]
        call    put4
        call    crlf
        mov     dx,OFFSET msg_twocopies
        call    say_ok
did_where:

        mov     ah,MPX_ID               ; ask it something only a live
        mov     al,01h                  ; copy could answer
        int     2Fh
        cmp     al,0FFh
        jne     count_bad
        mov     word ptr [tmp],bx
        mov     dx,OFFSET msg_count
        mov     ah,09h
        int     21h
        mov     ax,word ptr [tmp]
        call    put4
        call    crlf
        mov     dx,OFFSET msg_alive
        call    say_ok
        jmp     short done_exit
count_bad:
        mov     dx,OFFSET msg_countbad
        call    say_fail
done_exit:
        mov     dx,OFFSET msg_bye
        mov     ah,09h
        int     21h
        mov     al,byte ptr [failed]
        mov     ah,4Ch                  ; an ordinary exit: one copy is
        int     21h                     ; enough

not_ours:
        mov     dx,OFFSET msg_someone
        call    say_fail
        jmp     short done_exit

;--- nothing there: install ------------------------------------------
not_there:
        mov     ah,35h                  ; the old vector, to chain to
        mov     al,2Fh
        int     21h
        mov     word ptr [old_2F],bx
        mov     word ptr [old_2F+2],es

        push    cs
        pop     ds                      ; 35h returned in ES; DS is ours
        mov     dx,OFFSET int2f
        mov     ah,25h
        mov     al,2Fh
        int     21h

        push    cs
        pop     ds
        mov     dx,OFFSET msg_hooked
        call    say_ok

;--- and go resident -------------------------------------------------
; DX is paragraphs to keep, measured from the PSP.  What is kept is
; the PSP, the vector, the counter and the handler - not the setup
; code below it and not the messages, which is why the resident part
; was put first.
;
; Plus BUFFER_BYTES that are not in the file and never written to.  A
; real TSR keeps a buffer, and this one keeps eight kilobytes for the
; same reason a test needs a signal: half a kilobyte of resident code
; disappears into MEM's rounding, and "the free memory did not change"
; is indistinguishable from "the memory was given back".
        mov     dx,OFFSET resident_end
        add     dx,BUFFER_BYTES + 15
        shr     dx,4
        mov     ax,3100h                ; AL = 0: we succeeded
        int     21h

;--- helpers (transient) ---------------------------------------------
say_ok:
        push    dx
        mov     dx,OFFSET msg_ok
        mov     ah,09h
        int     21h
        pop     dx
        mov     ah,09h
        int     21h
        ret
say_fail:
        mov     byte ptr [failed],1
        push    dx
        mov     dx,OFFSET msg_fail
        mov     ah,09h
        int     21h
        pop     dx
        mov     ah,09h
        int     21h
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
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1_out
        add     al,7
p1_out:
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

tmp         dw  0
tmp2        dw  0
failed      db  0
            db  0

msg_hi      db  'TSRTEST.COM - terminate and stay resident',13,10,'$'
msg_hooked  db  'hooked INT 2Fh and going resident',13,10,'$'
msg_found   db  'a copy is already resident and it answered',13,10,'$'
msg_where   db  '  it is at segment $'
msg_weare   db  ', we are at $'
msg_twocopies db 'two copies at two segments: the first one is still there',13,10,'$'
msg_selfans db  'the answer came from our own code - it was not resident',13,10,'$'
msg_count   db  '  it has been asked $'
msg_alive   db  'the resident copy survived its own DOS box',13,10,'$'
msg_countbad db 'the resident copy would not answer 01h',13,10,'$'
msg_someone db  'something else owns multiplex C0h',13,10,'$'
msg_bye     db  'TSRTEST done',13,10,'$'
msg_ok      db  '    OK   $'
msg_fail    db  '    FAIL $'

_TSR    ENDS
        END
