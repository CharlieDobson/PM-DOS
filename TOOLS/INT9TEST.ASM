;=====================================================================
; INT9TEST.ASM - INT9TEST.COM, does a DOS box get the KEYBOARD, as
;                opposed to the keyboard buffer?
;
; KBTEST asks the other question and by v0.34 the answer was yes: a
; 16-bit program calling INT 16h, or reading 40:1A by hand, gets the
; key.  This one hooks INT 09h - the interrupt itself - which is what
; every full-screen program does when it wants key-up as well as
; key-down, or a hot key that works while it is busy, or the raw scan
; code the BIOS ring cannot carry.  Until v0.36 the answer to THAT was
; no, and silently: PM-DOS owns IRQ 1, its handler had already taken
; the byte off port 60h, and no INT 09h was ever delivered into the
; box at all.  MSD is the program that showed it - it comes up, reports
; the machine correctly, and will not take a keystroke.
;
; Four things have to be true and they are separable, so they are
; reported separately:
;
;   1  THE HANDLER RUNS.  A count of interrupts, and nothing else
;      matters if this is zero.
;   2  THE SCAN CODE IS RIGHT.  Port 60h, read inside the handler, has
;      to hold the key that was pressed and not the one before it.
;   3  READING IT TWICE GIVES THE SAME BYTE.  That is what the 8042
;      does, and it is what makes a chained handler possible: this one
;      reads the port and then passes the interrupt to the handler it
;      replaced, which reads it again.
;   4  THE CHAIN STILL WORKS.  After all that, INT 16h must still
;      produce the key - because the handler we chained to is the
;      ROM's, and it is the one filling the buffer now.
;
; INTERACTIVE, like KBTEST and for the same reason: the failure is a
; program that sits there, so every wait is bounded by the tick count
; at 40:6C and reports rather than becomes it.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_I9     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_I9, DS:_I9, ES:_I9, SS:_I9
        ORG     0

WAIT_TICKS      EQU     182             ; ten seconds, and then we give up
LOGMAX          EQU     32

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; Hook INT 09h, keeping what was there to chain to.  35h and 25h
; rather than writing 0000:0024 by hand, because the vector belongs to
; DOS and asking is the only way that stays true if DOS ever moves it.
;=====================================================================
        mov     ax,3509h
        int     21h                     ; ES:BX = the current handler
        mov     word ptr [old9],bx
        mov     word ptr [old9+2],es

        mov     dx,OFFSET i9
        mov     ax,2509h
        int     21h                     ; DS:DX - and DS is CS in a .COM

;=====================================================================
; 1 - press some keys.  The handler logs what it sees; this prints the
;     log as it grows, because printing from inside an interrupt
;     handler is how a DOS program corrupts itself.
;=====================================================================
        mov     dx,OFFSET t_press
        call    puts
        call    ticknow
        mov     [t0],ax
        mov     word ptr [shown],0

i9_wait:
        mov     ax,word ptr [nlog]
        cmp     ax,word ptr [shown]
        je      i9_nonew
        mov     bx,word ptr [shown]
        mov     al,[log + bx]
        call    put2
        mov     dl,' '
        call    putc
        inc     word ptr [shown]
        cmp     byte ptr [sawesc],0
        jne     i9_enough
        jmp     short i9_wait
i9_nonew:
        call    tickgone
        cmp     ax,WAIT_TICKS
        jb      i9_wait
i9_enough:

;=====================================================================
; Unhook FIRST.  Everything below prints, and a handler that is still
; live while DOS is printing is a second thing that can go wrong on
; top of whatever is being reported.
;=====================================================================
        push    ds
        mov     dx,word ptr [old9]
        mov     ax,word ptr [old9+2]
        mov     ds,ax
        mov     ax,2509h
        int     21h
        pop     ds

        mov     dx,OFFSET msg_crlf
        call    puts

;--- 1: did it run at all? -------------------------------------------
        mov     dx,OFFSET t_ran
        call    puts
        mov     ax,word ptr [nint]
        call    put4
        cmp     ax,0
        je      i9_noran
        call    ok
        jmp     short i9_code
i9_noran:
        call    bad
        jmp     i9_report               ; nothing below can mean anything

;--- 2: was the code a real one? -------------------------------------
; Not "was it the key I pressed" - this program has no way to know what
; was pressed - but "was it a scan code at all".  00h and FFh are the
; two things a dead port hands back, and a make code is under 80h.
i9_code:
        mov     dx,OFFSET t_code
        call    puts
        mov     al,[log]
        call    put2
        cmp     al,0
        je      i9_codebad
        cmp     al,0FFh
        je      i9_codebad
        call    ok
        jmp     short i9_same
i9_codebad:
        call    bad

;--- 3: does the port repeat itself? ---------------------------------
i9_same:
        mov     dx,OFFSET t_same
        call    puts
        mov     al,[first1]
        call    put2
        mov     dl,'/'
        call    putc
        mov     al,[first2]
        call    put2
        mov     al,[first1]
        cmp     al,[first2]
        jne     i9_samebad
        call    ok
        jmp     short i9_chain
i9_samebad:
        call    bad

;--- 4: did the chain still fill the BIOS buffer? --------------------
; The handler above passed every interrupt on to the ROM's, so INT 16h
; should have something to say.  This is the check that the split did
; not cost the box the ordinary keyboard while giving it the raw one.
i9_chain:
        mov     dx,OFFSET t_chain
        call    puts
        mov     ah,01h
        int     16h
        jz      i9_chainbad
        call    ok
        jmp     short i9_report
i9_chainbad:
        call    bad

;=====================================================================
i9_report:
        mov     dx,OFFSET msg_crlf
        call    puts
        call    drain
        cmp     byte ptr [fails],0
        jne     i9_failed
        mov     dx,OFFSET msg_pass
        call    puts
        jmp     short i9_exit
i9_failed:
        mov     dx,OFFSET msg_fail
        call    puts
i9_exit:
        mov     ax,4C00h
        int     21h

;=====================================================================
; i9 - the handler under test.
;
; It reads port 60h TWICE on the first interrupt, which is the point of
; check 3, and then chains.  It does not send an EOI: the handler it
; jumps to does, and two of them would be one too many.
;
; Everything it touches is addressed through CS, because an interrupt
; arrives with whatever DS the interrupted code was using.
;=====================================================================
i9:
        push    ax
        push    bx

        in      al,60h
        cmp     byte ptr cs:[gotfirst],0
        jne     i9h_log
        mov     byte ptr cs:[gotfirst],1
        mov     byte ptr cs:[first1],al
        in      al,60h                  ; ...and again: the 8042 holds it
        mov     byte ptr cs:[first2],al

i9h_log:
        cmp     al,1                    ; Esc, the way out
        jne     i9h_notesc
        mov     byte ptr cs:[sawesc],1
i9h_notesc:
        mov     bx,word ptr cs:[nlog]
        cmp     bx,LOGMAX
        jae     i9h_full
        mov     byte ptr cs:[log + bx],al
        inc     bx
        mov     word ptr cs:[nlog],bx
i9h_full:
        inc     word ptr cs:[nint]

        pop     bx
        pop     ax
        jmp     dword ptr cs:[old9]

;=====================================================================
; helpers - the same ones KBTEST uses, and for the same reason: a test
;           that needs a runtime library is a test of the library
;=====================================================================
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

ticknow:
        push    es
        push    bx
        mov     bx,40h
        mov     es,bx
        mov     ax,es:[6Ch]
        pop     bx
        pop     es
        ret

tickgone:                               ; AX = ticks since [t0]
        call    ticknow
        sub     ax,[t0]
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
old9        dd  0
nint        dw  0                       ; interrupts the handler took
nlog        dw  0                       ; scan codes it recorded
shown       dw  0                       ; ...and how many are on screen
gotfirst    db  0
first1      db  0                       ; port 60h, read twice on the
first2      db  0                       ; first interrupt
sawesc      db  0
t0          dw  0
fails       db  0
log         db  LOGMAX dup (0)

msg_hi      db  13,10,'INT9TEST - does a DOS box get the KEYBOARD?',13,10
            db  '(KBTEST asks about the buffer; this hooks the interrupt)'
            db  13,10,13,10,'$'
t_press     db  'Press some keys, Esc to finish (ten seconds): $'
t_ran       db  'INT 09h handler entered .. $'
t_code      db  'first scan code ........ $'
t_same      db  'port 60h read twice .... $'
t_chain     db  'chain still filled 40:1E $'
msg_ok      db  ' OK',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_pass    db  'INT9TEST: all checks passed',13,10,'$'
msg_fail    db  'INT9TEST: FAILURES - see above',13,10,'$'

_I9     ENDS
        END     start
