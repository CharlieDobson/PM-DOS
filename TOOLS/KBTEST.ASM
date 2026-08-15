;=====================================================================
; KBTEST.ASM - KBTEST.COM, can a 16-bit program get a keystroke?
;
; An ordinary 16-bit DOS program, and the question it asks is the one
; that turned out to have the answer "no".
;
; PM-DOS owns IRQ 1.  The ROM's INT 09h therefore never runs, and until
; v0.34 nothing else wrote the BIOS keyboard buffer at 40:1E either -
; so INT 16h, which is the ROM looking at exactly those bytes, found
; head equal to tail for ever.  Every full-screen DOS program stopped
; at its first keystroke and looked, from outside, like a machine that
; had locked up.  This is the v0.32 timer lesson a second time: OWNING
; AN INTERRUPT MEANS OWNING WHAT THE BIOS'S HANDLER DID.
;
; The three ways a program can ask, all of them here because a kernel
; that answers one and not the others has not fixed anything:
;
;   INT 16h AH=01h   is there a key?  (the poll every event loop runs)
;   INT 16h AH=00h   wait for one     (and it must not wait for ever)
;   40:1A / 40:1C    the buffer itself, read by hand - which plenty of
;                    programs do rather than call anybody
;
; ...and 40:17, the shift-state byte, which is how "is Shift held down"
; is asked and which nothing was writing either.
;
; INTERACTIVE ON PURPOSE.  A keystroke test cannot be run without a
; keystroke, and the failure it exists to catch is a HANG - so every
; wait here is bounded by the tick count at 40:6C and gives up rather
; than becoming the thing it is testing.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_KB     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_KB, DS:_KB, ES:_KB, SS:_KB
        ORG     0

WAIT_TICKS      EQU     182             ; ten seconds, and then we give up

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; 1 - the buffer bounds.  If 40:80 and 40:82 do not describe a ring,
;     nothing below can work and the reason is here rather than four
;     checks further on.
;=====================================================================
        mov     dx,OFFSET t_bounds
        call    puts
        mov     ax,40h
        mov     es,ax
        mov     ax,es:[80h]
        call    put4
        mov     dl,'-'
        call    putc
        mov     ax,es:[82h]
        call    put4
        mov     ax,es:[80h]
        cmp     ax,1Eh
        jne     kb_b_no
        mov     ax,es:[82h]
        cmp     ax,3Eh
        jne     kb_b_no
        call    ok
        jmp     short kb_poll
kb_b_no:
        call    bad

;=====================================================================
; 2 - AH=01h with nothing typed says "no key", and says it by NOT
;     hanging.  A kernel that got this wrong the other way would look
;     perfect until something asked.
;=====================================================================
kb_poll:
        mov     dx,OFFSET t_empty
        call    puts
        call    drain
        mov     ah,01h
        int     16h
        jz      kb_p_ok                 ; ZF = the buffer is empty
        call    bad
        jmp     short kb_wait
kb_p_ok:
        call    ok

;=====================================================================
; 3 - AH=00h returns the key that was typed, and returns AT ALL.
;=====================================================================
kb_wait:
        mov     dx,OFFSET t_press
        call    puts
        call    ticknow
        mov     [t0],ax
kb_w_lp:
        mov     ah,01h
        int     16h
        jnz     kb_w_got
        call    tickgone
        cmp     ax,WAIT_TICKS
        jb      kb_w_lp
        mov     dx,OFFSET msg_gaveup    ; the hang, caught and named
        call    puts
        call    bad
        jmp     short kb_bda
kb_w_got:
        mov     ah,00h                  ; and now the blocking read, which
        int     16h                     ; has a key waiting for it
        push    ax
        mov     dx,OFFSET msg_got
        call    puts
        pop     ax
        push    ax
        call    put4
        pop     ax
        cmp     al,20h                  ; show it if it is printable
        jb      kb_w_np
        cmp     al,7Eh
        ja      kb_w_np
        push    ax
        mov     dl,' '
        call    putc
        mov     dl,27h
        call    putc
        pop     ax
        mov     dl,al
        call    putc
        mov     dl,27h
        call    putc
kb_w_np:
        call    ok

;=====================================================================
; 4 - and the buffer really is the machine's: after that read, head
;     and tail agree again.  A kernel keeping a private ring and
;     mirroring into this one would pass check 3 and fail this.
;=====================================================================
kb_bda:
        mov     dx,OFFSET t_bda
        call    puts
        mov     ax,40h
        mov     es,ax
        mov     ax,es:[1Ah]
        call    put4
        mov     dl,'/'
        call    putc
        mov     ax,es:[1Ch]
        call    put4
        mov     ax,es:[1Ah]
        cmp     ax,es:[1Ch]
        jne     kb_bda_no
        call    ok
        jmp     short kb_shift
kb_bda_no:
        call    bad

;=====================================================================
; 5 - 40:17, which is how a program asks whether Shift is down.  It
;     cannot be checked without being held, so this reports rather
;     than judging: hold either Shift and press a key.
;
;     THE BYTE IS SAMPLED WHILE WE WAIT, not after the key arrives,
;     and the bits seen are latched.  40:17 says what is down NOW, so
;     reading it once the keystroke is already in the buffer is a race
;     against the break code - and one that a human always wins and an
;     automated keyboard always loses, which is the worst way round
;     for a test.  A program that genuinely cares reads it during its
;     own wait, exactly as this does.
;=====================================================================
kb_shift:
        mov     dx,OFFSET t_shift
        call    puts
        call    drain                   ; a FRESH key.  Check 3 leaves the
                                        ; Enter that ended the command line
                                        ; sitting there, and without this the
                                        ; wait below ends before it starts
        mov     byte ptr [shseen],0
        call    ticknow
        mov     [t0],ax
kb_s_lp:
        mov     ax,40h                  ; latch whatever is held, every
        mov     es,ax                   ; time round
        mov     al,es:[17h]
        or      [shseen],al
        mov     ah,01h
        int     16h
        jnz     kb_s_got
        call    tickgone
        cmp     ax,WAIT_TICKS
        jb      kb_s_lp
        mov     dx,OFFSET msg_gaveup
        call    puts
        call    bad
        jmp     short kb_done
kb_s_got:
        mov     ah,00h                  ; take the key, or the shell runs it
        int     16h
        mov     al,[shseen]
        push    ax
        call    put2
        pop     ax
        test    al,03h                  ; either shift bit
        jz      kb_s_no
        mov     dx,OFFSET msg_shifton
        call    puts
        call    drain
        call    ok
        jmp     short kb_done
kb_s_no:
        mov     dx,OFFSET msg_shiftoff
        call    puts
        call    drain
        call    bad

kb_done:
        mov     dx,OFFSET msg_bye
        call    puts
        movzx   ax,byte ptr [fails]
        or      al,al
        jz      kb_exit
        push    ax
        mov     dx,OFFSET msg_fails
        call    puts
        pop     ax
        call    put2
        mov     dx,OFFSET msg_crlf
        call    puts
kb_exit:
        mov     al,[fails]
        mov     ah,4Ch
        int     21h

;=====================================================================
; helpers
;=====================================================================
; drain - empty the buffer, so a check that wants it empty gets it
drain:
        push    ax
dr_lp:
        mov     ah,01h
        int     16h
        jz      dr_out
        mov     ah,00h
        int     16h
        jmp     short dr_lp
dr_out:
        pop     ax
        ret

; ticknow - AX = the low word of the BIOS tick count.  Read from 40:6C
; rather than through INT 1Ah deliberately: it is the same place a
; delay loop in real software reads, and v0.32 found out the hard way
; what happens when nothing is writing it.
ticknow:
        push    es
        push    bx
        mov     bx,40h
        mov     es,bx
        mov     ax,es:[6Ch]
        pop     bx
        pop     es
        ret

; tickgone - AX = ticks since [t0], allowing for the word wrapping
tickgone:
        push    dx
        call    tickcur
        sub     ax,[t0]
        pop     dx
        ret
tickcur:
        call    ticknow
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
t0          dw  0
shseen      db  0                       ; every modifier bit seen while waiting
fails       db  0

msg_hi      db  13,10,'KBTEST - can a 16-bit program get a keystroke?',13,10,13,10,'$'
msg_bye     db  13,10,'KBTEST done.',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_fails   db  'checks failed: $'
msg_got     db  ' got $'
msg_gaveup  db  ' nothing arrived in ten seconds$'
msg_shifton db  ' - shift is down$'
msg_shiftoff db ' - shift is NOT down$'

t_bounds    db  '  1 the ring at 40:80 and 40:82 says   $'
t_empty     db  '  2 AH=01h on an empty buffer says no  $'
t_press     db  '  3 AH=00h - PRESS ANY KEY             $'
t_bda       db  '  4 40:1A and 40:1C moved together     $'
t_shift     db  '  5 HOLD SHIFT and press a key: 40:17  $'

_KB     ENDS
        END     start
