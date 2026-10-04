;=====================================================================
; RMIVT.ASM - RMIVT.COM, does a BIOS call reach a DOS box's vectors?
;
; The kernel goes down to real mode to call the BIOS - for the time,
; a diskette, the printer.  Until this release the vector table down
; there was the one every DOS box writes, so a handler a program had
; hooked ran in real mode in the middle of the kernel's call, where no
; page is read-only.  A BIOS call now runs with a copy of the table
; taken when CONFIG.SYS was finished (IO\RMCALL.INC, rm_freeze).
;
;   1  INT 1AH HOOKED, AND 2CH ASKED A HUNDRED TIMES.  The kernel reads
;      the clock through INT 1Ah; the hook is not called in real mode.
;   2  THE BOX'S OWN INT 1AH still reaches the hook.
;   3  INT 08H AND INT 1CH HOOKED, and 2Ch asked for a second: neither
;      is called in real mode.
;   4  THE CLOCK'S PERIODIC INTERRUPT, 64 a second, goes on arriving
;      while 2Ch is asked for a second, and never in real mode.  The
;      BIOS's handler, given one of a program's, switches it off.
;   5  2CH ASKED ON EVERY TURN for two of the real-time clock's
;      seconds: 40:6C counts eighteen a second.  Moving the 8259s'
;      vector bases for every call threw away a tick that was waiting,
;      and on 86Box's 486 a quarter of them went.
;   6  AND WITH THE TIMER FOUR TIMES AS FAST, still eighteen.  The ROM's
;      handler counted every interrupt that landed inside a call.
;
; On an emulator whose BIOS answers at once, few interrupts land
; inside a call; on one whose BIOS waits for the clock, many do.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_RV     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_RV, DS:_RV, ES:_RV, SS:_RV
        ORG     0

RTC_INDEX       EQU     70h
RTC_DATA        EQU     71h
RTC_A           EQU     0Ah
RTC_B           EQU     0Bh
RTC_C           EQU     0Ch
RTC_64HZ        EQU     0Ah             ; register A's rate bits
RTC_PIE         EQU     40h             ; register B: periodic, on
RTC_PF          EQU     40h             ; register C: periodic, it was

start:
        mov     dx,OFFSET msg_hi
        call    puts

        mov     ax,351Ah
        int     21h
        mov     word ptr [old1a],bx
        mov     word ptr [old1a+2],es
        mov     ax,351Ch
        int     21h
        mov     word ptr [old1c],bx
        mov     word ptr [old1c+2],es
        mov     ax,3508h
        int     21h
        mov     word ptr [old08],bx
        mov     word ptr [old08+2],es
        mov     ax,3570h
        int     21h
        mov     word ptr [old70],bx
        mov     word ptr [old70+2],es
        push    cs
        pop     es

;=====================================================================
; 1 - INT 1Ah, from the kernel's 2Ch
;=====================================================================
        mov     dx,OFFSET h1a
        mov     ax,251Ah
        int     21h
        mov     dx,OFFSET t_1arm
        call    puts
        mov     di,100                  ; not CX, which 2Ch answers in
c1_lp:
        mov     ah,2Ch
        int     21h
        dec     di
        jnz     c1_lp
        mov     ax,word ptr [n1arm]
        call    put4
        or      ax,ax
        jnz     c1_bad
        call    ok
        jmp     short c2
c1_bad:
        call    bad

;=====================================================================
; 2 - INT 1Ah, from the box
;=====================================================================
c2:
        mov     dx,OFFSET t_1abox
        call    puts
        mov     word ptr [n1av86],0
        mov     ah,02h
        int     1Ah
        mov     ax,word ptr [n1av86]
        call    put4
        or      ax,ax
        jz      c2_bad
        call    ok
        jmp     short c2_unhook
c2_bad:
        call    bad
c2_unhook:
        push    ds
        lds     dx,dword ptr [old1a]
        mov     ax,251Ah
        int     21h
        pop     ds

;=====================================================================
; 3 - INT 08h and INT 1Ch, while 2Ch is asked for a second
;=====================================================================
        mov     dx,OFFSET h08
        mov     ax,2508h
        int     21h
        mov     dx,OFFSET h1c
        mov     ax,251Ch
        int     21h
        mov     dx,OFFSET t_tick
        call    puts
        mov     word ptr [ntickrm],0
        push    es
        mov     bx,40h
        mov     es,bx
        mov     si,es:[6Ch]
        mov     dword ptr [turns],00100000h     ; a clock that does not move
c3_lp:
        mov     ah,2Ch
        int     21h
        mov     ax,es:[6Ch]
        sub     ax,si
        cmp     ax,18
        jae     c3_done
        dec     dword ptr [turns]
        jnz     c3_lp
c3_done:
        pop     es
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
        mov     ax,word ptr [ntickrm]
        call    put4
        cmp     dword ptr [turns],0
        je      c3_bad
        or      ax,ax
        jnz     c3_bad
        call    ok
        jmp     short c4
c3_bad:
        call    bad

;=====================================================================
; 4 - the clock's own interrupt, 64 a second, while 2Ch is asked for a
;     second: it goes on arriving, and it is never called in real mode
;=====================================================================
c4:
        mov     dx,OFFSET h70
        mov     ax,2570h
        int     21h
        mov     word ptr [n70pf],0
        mov     word ptr [n70rm],0
        cli
        mov     al,RTC_A
        call    rtcget
        and     al,0F0h
        or      al,RTC_64HZ
        mov     ah,al
        mov     al,RTC_A
        call    rtcput
        mov     al,RTC_B
        call    rtcget
        or      al,RTC_PIE
        mov     ah,al
        mov     al,RTC_B
        call    rtcput
        mov     al,RTC_C
        call    rtcget
        in      al,21h                  ; the cascade, and line 8
        mov     [mask1],al
        and     al,0FBh
        out     21h,al
        in      al,0A1h
        mov     [mask2],al
        and     al,0FEh
        out     0A1h,al
        sti
        mov     dx,OFFSET t_rtc
        call    puts
        push    es
        mov     bx,40h
        mov     es,bx
        mov     si,es:[6Ch]
        mov     dword ptr [turns],00100000h
c4_lp:
        mov     ah,2Ch
        int     21h
        mov     ax,es:[6Ch]
        sub     ax,si
        cmp     ax,18
        jae     c4_done
        dec     dword ptr [turns]
        jnz     c4_lp
c4_done:
        pop     es
        cli
        mov     al,RTC_B
        call    rtcget
        and     al,NOT RTC_PIE
        mov     ah,al
        mov     al,RTC_B
        call    rtcput
        mov     al,RTC_C
        call    rtcget
        mov     al,[mask2]
        out     0A1h,al
        mov     al,[mask1]
        out     21h,al
        sti
        push    ds
        lds     dx,dword ptr [old70]
        mov     ax,2570h
        int     21h
        pop     ds
        mov     ax,word ptr [n70pf]
        call    put4
        cmp     dword ptr [turns],0
        je      c4_bad
        cmp     ax,54                   ; three a tick
        jb      c4_bad
        call    ok
        jmp     short c4b
c4_bad:
        call    bad
c4b:
        mov     dx,OFFSET t_rtcrm
        call    puts
        mov     ax,word ptr [n70rm]
        call    put4
        or      ax,ax
        jnz     c4b_bad
        call    ok
        jmp     short c5
c4b_bad:
        call    bad

;=====================================================================
; 5 - 2Ch on every turn, at the BIOS's own rate
;=====================================================================
c5:
        mov     dx,OFFSET t_clock
        call    puts
        call    clock2s
        call    put4
        cmp     ax,30
        jb      c5_bad
        cmp     ax,42
        ja      c5_bad
        call    ok
        jmp     short c6
c5_bad:
        call    bad

;=====================================================================
; 6 - and the timer at four times the rate
;=====================================================================
c6:
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
        call    clock2s
        push    ax
        cli
        mov     al,36h                  ; back to the BIOS's 65536
        out     43h,al
        xor     al,al
        out     40h,al
        out     40h,al
        sti
        pop     ax
        call    put4
        cmp     ax,30
        jb      c6_bad
        cmp     ax,42
        ja      c6_bad
        call    ok
        jmp     short done
c6_bad:
        call    bad

done:
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
; The hooks.  SMSW says where each was called from: in a box it is
; V86 mode, which is protected mode; inside a BIOS call of the
; kernel's it is real mode.
;=====================================================================
h1a:
        push    ax
        smsw    ax
        test    al,1
        jz      h1a_rm
        inc     word ptr cs:[n1av86]
        jmp     short h1a_on
h1a_rm:
        inc     word ptr cs:[n1arm]
h1a_on:
        pop     ax
        jmp     dword ptr cs:[old1a]

h08:
        push    ax
        smsw    ax
        test    al,1
        jnz     h08_on
        inc     word ptr cs:[ntickrm]
h08_on:
        pop     ax
        jmp     dword ptr cs:[old08]

h1c:
        push    ax
        smsw    ax
        test    al,1
        jnz     h1c_on
        inc     word ptr cs:[ntickrm]
h1c_on:
        pop     ax
        jmp     dword ptr cs:[old1c]

; The clock's: register C says whether it was the periodic interrupt
; this program asked for, and reading it is what lets the clock go.
h70:
        push    ax
        smsw    ax
        test    al,1
        jnz     h70_v86
        inc     word ptr cs:[n70rm]
h70_v86:
        mov     al,RTC_C
        out     RTC_INDEX,al
        jmp     short $+2
        in      al,RTC_DATA
        test    al,RTC_PF
        jz      h70_eoi
        inc     word ptr cs:[n70pf]
h70_eoi:
        mov     al,20h
        out     0A0h,al
        out     20h,al
        pop     ax
        iret

rtcget:
        out     RTC_INDEX,al
        jmp     short $+2
        in      al,RTC_DATA
        ret
rtcput:
        out     RTC_INDEX,al
        jmp     short $+2
        mov     al,ah
        out     RTC_DATA,al
        ret

;=====================================================================
; clock2s - AX = how far 40:6C moves in two of the real-time clock's
;           seconds, asking 2Ch on every turn.
;=====================================================================
clock2s:
        push    bx
        push    es
        mov     bx,40h
        mov     es,bx
        call    sec2c                   ; to the edge of a second
        mov     bx,es:[6Ch]
        call    sec2c
        call    sec2c
        mov     ax,es:[6Ch]
        sub     ax,bx
        pop     es
        pop     bx
        ret

;=====================================================================
; sec2c - 2Ch until its seconds change.  Bounded by a turn count.
;=====================================================================
sec2c:
        pusha
        mov     ah,2Ch
        int     21h
        mov     bl,dh
        mov     dword ptr [turns],00400000h
s2_lp:
        mov     ah,2Ch
        int     21h
        cmp     dh,bl
        jne     s2_out
        dec     dword ptr [turns]
        jnz     s2_lp
s2_out:
        popa
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
old1a       dd  0
old1c       dd  0
old08       dd  0
old70       dd  0
n1arm       dw  0
n1av86      dw  0
ntickrm     dw  0
n70pf       dw  0
n70rm       dw  0
turns       dd  0
mask1       db  0
mask2       db  0
fails       db  0

msg_hi      db  13,10,'RMIVT - does a BIOS call reach a DOS box''s vectors?'
            db  13,10,13,10,'$'
t_1arm      db  'INT 1Ah, real mode, 2Ch x100 .. $'
t_1abox     db  'INT 1Ah, from the box ......... $'
t_tick      db  'INT 08h/1Ch, in real mode ..... $'
t_rtc       db  'IRQ 8 at 64 a second, 1s ...... $'
t_rtcrm     db  'IRQ 8, in real mode ........... $'
t_clock     db  '2Ch: clock in 2 RTC s ......... $'
t_fast      db  'timer x4: clock in 2 RTC s .... $'
msg_ok      db  ' OK',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_pass    db  'RMIVT: all checks passed',13,10,'$'
msg_fail    db  'RMIVT: FAILURES - see above',13,10,'$'

_RV     ENDS
        END     start
