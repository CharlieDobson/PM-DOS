;=====================================================================
; TIMERTST.ASM - TIMERTST.COM, does a DOS box get the timer?
;
; VPICTEST asks it of a card's line.  This asks it of line 0, which the
; kernel keeps for its own clock and now hands on as well (IO\VTIMER.INC):
; a program's INT 08h and INT 1Ch were never called before.
;
;   1  INT 1CH arrives, eighteen times a second.
;   2  INT 08H arrives, and a hook that passes it on still gets INT 1Ch
;      called behind it.
;   3  THE LINE IS IN SERVICE while INT 08h runs, and not between.
;   4  MASKED, NO INT 08H - and the clock at 40:6C still runs: it is the
;      kernel's, and not on the box's mask.
;   5  A TIMER FOUR TIMES AS FAST gives INT 08h four times as often, and
;      40:6C still counts eighteen a second - against the real-time
;      clock's seconds, not against itself.
;
; Every wait is bounded by the tick count at 40:6C, and the tick count
; by a number of turns of a loop, so a failure is a line that says so.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_TT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_TT, DS:_TT, ES:_TT, SS:_TT
        ORG     0

RTC_INDEX       EQU     70h
RTC_DATA        EQU     71h
RTC_SEC         EQU     00h
RTC_A           EQU     0Ah             ; bit 7: an update is under way

start:
        mov     dx,OFFSET msg_hi
        call    puts

        mov     ax,351Ch
        int     21h
        mov     word ptr [old1c],bx
        mov     word ptr [old1c+2],es
        mov     ax,3508h
        int     21h
        mov     word ptr [old08],bx
        mov     word ptr [old08+2],es
        push    cs
        pop     es

;=====================================================================
; 1 - INT 1Ch, with INT 08h left as it was
;=====================================================================
        mov     dx,OFFSET i1c
        mov     ax,251Ch
        int     21h
        mov     dx,OFFSET t_1c
        call    puts
        mov     ax,1                    ; from the start of a tick
        call    waitt
        mov     word ptr [n1c],0
        mov     ax,36
        call    waitt
        mov     ax,word ptr [n1c]
        call    put4
        cmp     ax,30
        jb      c1_bad
        cmp     ax,42
        ja      c1_bad
        call    ok
        jmp     short c2
c1_bad:
        call    bad

;=====================================================================
; 2 - INT 08h, passed on to whatever was there
;=====================================================================
c2:
        mov     dx,OFFSET i08
        mov     ax,2508h
        int     21h
        mov     dx,OFFSET t_08
        call    puts
        mov     ax,1
        call    waitt
        mov     word ptr [n08],0
        mov     word ptr [n1c],0
        mov     ax,36
        call    waitt
        mov     ax,word ptr [n08]
        call    put4
        cmp     ax,30
        jb      c2_bad
        cmp     ax,42
        ja      c2_bad
        call    ok
        jmp     short c2b
c2_bad:
        call    bad
c2b:
        mov     dx,OFFSET t_chain
        call    puts
        mov     ax,word ptr [n1c]
        call    put4
        cmp     ax,30
        jb      c2b_bad
        call    ok
        jmp     short c3
c2b_bad:
        call    bad

;=====================================================================
; 3 - in service inside INT 08h, and not outside it
;=====================================================================
c3:
        mov     dx,OFFSET t_inserv
        call    puts
        mov     al,[isr08]
        call    put2
        test    byte ptr [isr08],01h
        jz      c3_bad
        call    ok
        jmp     short c3b
c3_bad:
        call    bad
c3b:
        mov     dx,OFFSET t_outserv
        call    puts
        cli
        mov     al,0Bh
        out     20h,al
        in      al,20h
        mov     ah,al
        mov     al,0Ah
        out     20h,al
        sti
        mov     al,ah
        call    put2
        test    ah,01h
        jnz     c3b_bad
        call    ok
        jmp     short c4
c3b_bad:
        call    bad

;=====================================================================
; 4 - masked, nothing: and the clock goes on
;=====================================================================
c4:
        mov     dx,OFFSET t_masked
        call    puts
        in      al,21h
        mov     [mask1],al
        or      al,01h
        out     21h,al
        mov     ax,2                    ; one on its way may land
        call    waitt
        jc      c4_bad
        mov     word ptr [n08],0
        mov     ax,18
        call    waitt
        pushf
        mov     al,[mask1]
        out     21h,al
        popf
        jc      c4_bad                  ; the clock stopped
        mov     ax,word ptr [n08]
        call    put4
        or      ax,ax
        jnz     c4_bad
        call    ok
        jmp     short c4b
c4_bad:
        call    bad
c4b:
        mov     dx,OFFSET t_unmask
        call    puts
        mov     word ptr [n08],0
        mov     ax,18
        call    waitt
        mov     ax,word ptr [n08]
        call    put4
        cmp     ax,12
        jb      c4b_bad
        call    ok
        jmp     short c5
c4b_bad:
        call    bad

;=====================================================================
; 5 - the timer at four times the rate
;=====================================================================
c5:
        mov     byte ptr [fast],1
        cli
        mov     al,36h                  ; channel 0, low then high, mode 3
        out     43h,al
        mov     al,00h
        out     40h,al
        mov     al,40h                  ; 4000h: a quarter of 65536
        out     40h,al
        sti
        mov     dx,OFFSET t_fast
        call    puts
        mov     ax,1
        call    waitt
        mov     word ptr [n08],0
        mov     word ptr [n1c],0
        mov     ax,18
        call    waitt
        mov     ax,word ptr [n08]
        call    put4
        cmp     ax,60
        jb      c5_bad
        cmp     ax,84
        ja      c5_bad
        call    ok
        jmp     short c5b
c5_bad:
        call    bad
c5b:
        mov     dx,OFFSET t_fast1c
        call    puts
        mov     ax,word ptr [n1c]
        call    put4
        cmp     ax,14
        jb      c5b_bad
        cmp     ax,22
        ja      c5b_bad
        call    ok
        jmp     short c5c
c5b_bad:
        call    bad

c5c:
        ; THE CLOCK AGAINST THE REAL-TIME CLOCK.  From one change of its
        ; seconds to the change two seconds on, 40:6C should move by
        ; thirty-six; at the timer's rate it would move by 146.
        mov     dx,OFFSET t_clock
        call    puts
        call    rtcsec                  ; to the edge of a second
        call    rtcsec
        push    es
        mov     bx,40h
        mov     es,bx
        mov     bx,es:[6Ch]
        pop     es
        mov     [before],bx
        call    rtcsec
        call    rtcsec
        push    es
        mov     bx,40h
        mov     es,bx
        mov     ax,es:[6Ch]
        pop     es
        sub     ax,[before]
        call    put4
        cmp     ax,30
        jb      c5c_bad
        cmp     ax,42
        ja      c5c_bad
        call    ok
        jmp     short c5d
c5c_bad:
        call    bad
c5d:
        cli
        mov     al,36h                  ; back to the BIOS's 65536
        out     43h,al
        xor     al,al
        out     40h,al
        out     40h,al
        sti
        mov     byte ptr [fast],0

;=====================================================================
; Both vectors back, before anything else is printed
;=====================================================================
        push    ds
        lds     dx,dword ptr [old08]
        mov     ax,2508h
        int     21h
        pop     ds
        push    ds
        lds     dx,dword ptr [old1c]
        mov     ax,251Ch
        int     21h
        pop     ds

        mov     dx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        jne     failed
        mov     dx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h
failed:
        mov     dx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; i1c - counts
;=====================================================================
i1c:
        inc     word ptr cs:[n1c]
        iret

;=====================================================================
; i08 - counts, looks at what is in service the first time, and passes
;       the interrupt on.  At four times the rate it passes on one in
;       four and ends the other three itself, as a game does.
;=====================================================================
i08:
        push    ax
        inc     word ptr cs:[n08]
        cmp     byte ptr cs:[sampled],0
        jne     i08_on
        mov     byte ptr cs:[sampled],1
        mov     al,0Bh
        out     20h,al
        in      al,20h
        mov     byte ptr cs:[isr08],al
        mov     al,0Ah
        out     20h,al
i08_on:
        cmp     byte ptr cs:[fast],0
        je      i08_chain
        inc     byte ptr cs:[quarter]
        test    byte ptr cs:[quarter],3
        jz      i08_chain
        mov     al,20h
        out     20h,al
        pop     ax
        iret
i08_chain:
        pop     ax
        jmp     dword ptr cs:[old08]

;=====================================================================
; rtcsec - wait for the real-time clock's seconds to change.  Bounded
;          by a turn count, like waitt.
;=====================================================================
rtcsec:
        push    ax
        push    bx
        push    ecx
        call    rtcread
        mov     bl,al
        mov     ecx,40000000h
rs_lp:
        call    rtcread
        cmp     al,bl
        jne     rs_out
        dec     ecx
        jnz     rs_lp
rs_out:
        pop     ecx
        pop     bx
        pop     ax
        ret

rtcread:
        cli
rr_wait:
        mov     al,RTC_A
        out     RTC_INDEX,al
        in      al,RTC_DATA
        test    al,80h
        jnz     rr_wait
        mov     al,RTC_SEC
        out     RTC_INDEX,al
        in      al,RTC_DATA
        sti
        ret

;=====================================================================
; waitt - AX = ticks to wait.  CF if the clock was not moving.
;=====================================================================
waitt:
        push    bx
        push    dx
        push    es
        push    ecx
        mov     bx,40h
        mov     es,bx
        mov     dx,ax
wt_tick:
        or      dx,dx
        jz      wt_done
        mov     bx,es:[6Ch]
        mov     ecx,08000000h
wt_spin:
        cmp     bx,es:[6Ch]
        jne     wt_moved
        dec     ecx
        jnz     wt_spin
        pop     ecx
        pop     es
        pop     dx
        pop     bx
        stc
        ret
wt_moved:
        dec     dx
        jmp     short wt_tick
wt_done:
        pop     ecx
        pop     es
        pop     dx
        pop     bx
        clc
        ret

;=====================================================================
; helpers
;=====================================================================
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
        push    dx
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        call    putc
        pop     dx
        ret

;=====================================================================
; data
;=====================================================================
old1c       dd  0
old08       dd  0
n1c         dw  0
n08         dw  0
before      dw  0
mask1       db  0
isr08       db  0
sampled     db  0
fast        db  0
quarter     db  0
fails       db  0

msg_hi      db  13,10,'TIMERTST - does a DOS box get the timer?'
            db  13,10,13,10,'$'
t_1c        db  'INT 1Ch, 2s ............. $'
t_08        db  'INT 08h, 2s ............. $'
t_chain     db  'and INT 1Ch behind it ... $'
t_inserv    db  'in service, in handler .. $'
t_outserv   db  'in service, outside it .. $'
t_masked    db  'INT 08h while masked, 1s  $'
t_unmask    db  'and after unmasking ..... $'
t_fast      db  'timer x4: INT 08h, 1s ... $'
t_fast1c    db  'and INT 1Ch, passed on .. $'
t_clock     db  'clock ticks in 2 RTC s .. $'
msg_ok      db  ' OK',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_pass    db  'TIMERTST: all checks passed',13,10,'$'
msg_fail    db  'TIMERTST: FAILURES - see above',13,10,'$'

_TT     ENDS
        END     start
