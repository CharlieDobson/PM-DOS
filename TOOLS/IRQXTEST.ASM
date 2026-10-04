;=====================================================================
; IRQXTEST.ASM - IRQXTEST.COM, does a resident driver get its
;                interrupts with no program running in a box?
;
; A DOS box is given an interrupt when its code can take one.  A
; RESIDENT driver's code is in memory all the time, but until
; DOS\IRQX.INC it was only ever run while some program was running in
; a box - so with the native shell at its prompt, a packet driver, a
; serial port's driver or a TSR on the timer heard nothing at all.
;
;   IRQXTEST /I   goes resident with three handlers: INT 1Ch, which
;                 counts and, every two seconds, puts a Z in the BIOS's
;                 keyboard buffer; the real-time clock's IRQ 8, at 64 a
;                 second, which counts; and INT 2Fh AH=CEh, which says
;                 where the counts are.
;   IRQXWAIT      (native) waits for those Zs.  Every one of them was
;                 typed by a handler that ran while no box did.
;   IRQXTEST /R   reads the counts, checks them against the time gone
;                 by, and switches it all off again.
;
; THE CHECKS, in /R:
;   1  INT 1Ch ran on (nearly) every tick since /I - not just on the
;      ticks some program happened to be running in a box for.
;   2  IRQ 8 likewise, three and a half times as often - counted where
;      the clock's register C says it interrupted.  The handler's other
;      calls are printed beside it, and none of them was made in real
;      mode, inside a BIOS call of the kernel's.
;   3  at least two Zs went into the buffer.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_IX     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_IX, DS:_IX, ES:_IX, SS:_IX
        ORG     0

RTC_INDEX       EQU     70h
RTC_DATA        EQU     71h
RTC_A           EQU     0Ah
RTC_B           EQU     0Bh
RTC_C           EQU     0Ch
RTC_64HZ        EQU     0Ah
RTC_PIE         EQU     40h

MPX_ID          EQU     0CEh            ; INT 2Fh AH: this program's
KEY_Z           EQU     2C5Ah           ; scan code and character
KEY_EVERY       EQU     36              ; ticks between them

start:
        jmp     main

;=====================================================================
; THE RESIDENT PART, which is everything up to res_end
;=====================================================================
blk:                                    ; what INT 2Fh AX=CE01h points at
t0          dw  0                       ; 40:6C when it went resident
n1c         dw  0                       ; INT 1Ch calls since
n70         dw  0                       ; IRQ 8 interrupts since
nkeys       dw  0                       ; Zs put in the buffer
n70pf       dw  0                       ; ...of the IRQ 8s, the clock's own
n70rm       dw  0                       ; ...and the ones in real mode
armed       db  0
countdown   db  0
old2f       dd  0
old1c       dd  0
old70       dd  0

i2f:
        cmp     ah,MPX_ID
        jne     i2f_on
        or      al,al
        jnz     i2f_where
        mov     al,0FFh                 ; installed
        iret
i2f_where:
        push    cs
        pop     es
        mov     bx,OFFSET blk
        iret
i2f_on:
        jmp     dword ptr cs:[old2f]

i1c:
        inc     word ptr cs:[n1c]
        cmp     byte ptr cs:[armed],0
        je      i1c_on
        dec     byte ptr cs:[countdown]
        jnz     i1c_on
        mov     byte ptr cs:[countdown],KEY_EVERY
        push    ax
        push    bx
        push    ds
        mov     ax,40h
        mov     ds,ax
        mov     bx,ds:[1Ch]             ; the tail, where the next key goes
        mov     ax,bx
        add     ax,2
        cmp     ax,ds:[82h]             ; the end of the ring
        jb      i1c_nowrap
        mov     ax,ds:[80h]             ; ...and its start
i1c_nowrap:
        cmp     ax,ds:[1Ah]             ; full: the head is next
        je      i1c_full
        mov     word ptr [bx],KEY_Z
        mov     ds:[1Ch],ax
        inc     word ptr cs:[nkeys]
i1c_full:
        pop     ds
        pop     bx
        pop     ax
i1c_on:
        jmp     dword ptr cs:[old1c]

; THE CLOCK IS THE BIOS'S AS WELL.  Its services use the clock's other
; interrupts - the update-ended one, its 1024-a-second waits - and
; while the kernel is calling the BIOS (rm_int, in real mode) the BIOS
; takes the ones that arrive, and the kernel hands this handler the
; interrupt afterwards with register C already read.  Only the periodic
; interrupt this asked for, which register C says is the reason, is
; checked; the rest are counted apart.  The calls made IN real mode are
; counted too, and there must be none: the vector table a BIOS call
; runs with is a copy taken before any program ran (IO\RMCALL.INC),
; and until this release it was this one.  Register C is read either
; way - that is what lets the clock go.
i70:
        push    ax
        inc     word ptr cs:[n70]
        smsw    ax
        test    al,1                    ; real mode: a kernel call into
        jnz     i70_v86                 ; the BIOS
        inc     word ptr cs:[n70rm]
i70_v86:
        mov     al,RTC_C
        out     RTC_INDEX,al
        in      al,RTC_DATA
        test    al,40h
        jz      i70_eoi
        inc     word ptr cs:[n70pf]
i70_eoi:
        mov     al,20h
        out     0A0h,al
        out     20h,al
        pop     ax
        iret

res_end:

;=====================================================================
; THE TRANSIENT PART
;=====================================================================
main:
        mov     si,81h                  ; /I or /R
m_tail:
        lodsb
        cmp     al,0Dh
        je      usage
        cmp     al,'/'
        jne     m_tail
        lodsb
        or      al,20h
        cmp     al,'i'
        je      install
        cmp     al,'r'
        je      report
usage:
        mov     dx,OFFSET msg_use
        call    puts
        mov     ax,4C02h
        int     21h

;=====================================================================
; /I
;=====================================================================
install:
        mov     ax,MPX_ID SHL 8
        int     2Fh
        cmp     al,0FFh
        jne     in_new
        mov     dx,OFFSET msg_already
        call    puts
        mov     ax,4C01h
        int     21h
in_new:
        mov     ax,352Fh
        int     21h
        mov     word ptr [old2f],bx
        mov     word ptr [old2f+2],es
        mov     ax,351Ch
        int     21h
        mov     word ptr [old1c],bx
        mov     word ptr [old1c+2],es
        mov     ax,3570h
        int     21h
        mov     word ptr [old70],bx
        mov     word ptr [old70+2],es
        push    cs
        pop     es

        cli
        push    es
        mov     ax,40h
        mov     es,ax
        mov     ax,es:[6Ch]
        pop     es
        mov     [t0],ax
        mov     word ptr [n1c],0
        mov     word ptr [n70],0
        mov     word ptr [nkeys],0
        mov     byte ptr [countdown],KEY_EVERY
        mov     byte ptr [armed],1
        mov     dx,OFFSET i2f
        mov     ax,252Fh
        int     21h
        mov     dx,OFFSET i1c
        mov     ax,251Ch
        int     21h
        mov     dx,OFFSET i70
        mov     ax,2570h
        int     21h

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
        and     al,0FBh
        out     21h,al
        in      al,0A1h
        and     al,0FEh
        out     0A1h,al
        sti

        mov     dx,OFFSET msg_inst
        call    puts
        mov     dx,OFFSET res_end       ; paragraphs from the PSP
        add     dx,15
        shr     dx,4
        mov     ax,3100h
        int     21h

;=====================================================================
; /R
;=====================================================================
report:
        mov     dx,OFFSET msg_hi
        call    puts
        mov     ax,MPX_ID SHL 8
        int     2Fh
        cmp     al,0FFh
        je      r_there
        mov     dx,OFFSET msg_none
        call    puts
        mov     ax,4C01h
        int     21h
r_there:
        mov     ax,(MPX_ID SHL 8) OR 1
        int     2Fh                     ; ES:BX -> the resident block
        mov     [rseg],es

        ; switch the source of it all off first: no more Zs, no more
        ; clock interrupts, the line masked again
        cli
        mov     byte ptr es:[bx + (OFFSET armed - OFFSET blk)],0
        mov     al,RTC_B
        call    rtcget
        and     al,NOT RTC_PIE
        mov     ah,al
        mov     al,RTC_B
        call    rtcput
        mov     al,RTC_C
        call    rtcget
        in      al,0A1h
        or      al,01h
        out     0A1h,al
        sti

        push    es
        mov     ax,40h
        mov     es,ax
        mov     ax,es:[6Ch]
        mov     dx,es:[80h]             ; and no Z left over for whatever
        mov     es:[1Ah],dx             ; reads the keyboard next
        mov     es:[1Ch],dx
        pop     es
        sub     ax,es:[bx + (OFFSET t0 - OFFSET blk)]
        mov     [elapsed],ax
        mov     ax,es:[bx + (OFFSET n1c - OFFSET blk)]
        mov     [got1c],ax
        mov     ax,es:[bx + (OFFSET n70 - OFFSET blk)]
        mov     [got70],ax
        mov     ax,es:[bx + (OFFSET n70pf - OFFSET blk)]
        mov     [got70pf],ax
        mov     ax,es:[bx + (OFFSET n70rm - OFFSET blk)]
        mov     [got70rm],ax
        mov     ax,es:[bx + (OFFSET nkeys - OFFSET blk)]
        mov     [gotkeys],ax
        push    cs
        pop     es

        mov     dx,OFFSET t_elapsed
        call    puts
        mov     ax,[elapsed]
        call    putdec
        mov     dx,OFFSET msg_crlf
        call    puts

;--- 1: INT 1Ch on (nearly) every tick --------------------------------
        mov     dx,OFFSET t_1c
        call    puts
        mov     ax,[got1c]
        call    putdec
        mov     ax,[elapsed]            ; nine tenths of them, less two
        mov     cx,9
        mul     cx
        mov     cx,10
        div     cx
        sub     ax,2
        jbe     c1_bad                  ; not long enough to mean anything
        cmp     [got1c],ax
        jb      c1_bad
        call    ok
        jmp     short c2
c1_bad:
        call    bad

;--- 2: IRQ 8, 64 a second: from three a tick to four ----------------
c2:
        mov     dx,OFFSET t_70
        call    puts
        mov     ax,[got70pf]
        call    putdec
        mov     ax,[elapsed]
        mov     cx,3
        mul     cx
        cmp     [got70pf],ax
        jb      c2_bad
        mov     ax,[elapsed]
        shl     ax,2
        add     ax,4
        cmp     [got70pf],ax
        ja      c2_bad
        call    ok
        jmp     short c2b
c2_bad:
        call    bad

        ; and the handler's calls that the clock did not ask for: an
        ; interrupt delivered that nothing raised
c2b:
        mov     dx,OFFSET t_70x
        call    puts
        mov     ax,[got70]
        sub     ax,[got70pf]
        call    putdec
        mov     dx,OFFSET msg_crlf
        call    puts
        mov     dx,OFFSET t_70rm
        call    puts
        mov     ax,[got70rm]
        call    putdec
        or      ax,ax
        jnz     c2c_bad
        call    ok
        jmp     short c3
c2c_bad:
        call    bad

;--- 3: the Zs --------------------------------------------------------
c3:
        mov     dx,OFFSET t_keys
        call    puts
        mov     ax,[gotkeys]
        call    putdec
        cmp     ax,2
        jb      c3_bad
        call    ok
        jmp     short r_unhook
c3_bad:
        call    bad

;--- and the three vectors back, if they are still this program's -----
r_unhook:
        mov     es,[rseg]
        mov     ax,351Ch
        int     21h
        mov     ax,es
        cmp     ax,[rseg]
        jne     r_no1c
        cmp     bx,OFFSET i1c
        jne     r_no1c
        mov     es,[rseg]
        push    ds
        lds     dx,es:[old1c]
        mov     ax,251Ch
        int     21h
        pop     ds
r_no1c:
        mov     ax,3570h
        int     21h
        mov     ax,es
        cmp     ax,[rseg]
        jne     r_no70
        cmp     bx,OFFSET i70
        jne     r_no70
        mov     es,[rseg]
        push    ds
        lds     dx,es:[old70]
        mov     ax,2570h
        int     21h
        pop     ds
r_no70:
        mov     ax,352Fh
        int     21h
        mov     ax,es
        cmp     ax,[rseg]
        jne     r_no2f
        cmp     bx,OFFSET i2f
        jne     r_no2f
        mov     es,[rseg]
        push    ds
        lds     dx,es:[old2f]
        mov     ax,252Fh
        int     21h
        pop     ds
r_no2f:
        push    cs
        pop     es

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
; helpers
;=====================================================================
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
        mov     ah,09h
        int     21h
        pop     ax
        ret

putdec:                                 ; AX in decimal
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx,10
        xor     cx,cx
pd_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     pd_div
pd_out:
        pop     dx
        add     dl,'0'
        mov     ah,02h
        int     21h
        loop    pd_out
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; data
;=====================================================================
rseg        dw  0
elapsed     dw  0
got1c       dw  0
got70       dw  0
got70pf     dw  0
got70rm     dw  0
gotkeys     dw  0
fails       db  0

msg_use     db  'IRQXTEST /I goes resident; IRQXTEST /R reports and stops.',13,10,'$'
msg_already db  'IRQXTEST: already resident.',13,10,'$'
msg_none    db  'IRQXTEST: not resident - run IRQXTEST /I first.',13,10,'$'
msg_inst    db  'IRQXTEST: resident; INT 1Ch, IRQ 8 and INT 2Fh hooked.',13,10,'$'
msg_hi      db  13,10,'IRQXTEST - a resident driver''s interrupts, with no box running'
            db  13,10,13,10,'$'
t_elapsed   db  'ticks since /I ........... $'
t_1c        db  'INT 1Ch calls, 9/10 ...... $'
t_70        db  'IRQ 8 interrupts, 3/tick . $'
t_70x       db  'and calls the clock did not ask for .. $'
t_70rm      db  'and calls made in real mode ......... $'
t_keys      db  'Zs typed by INT 1Ch ...... $'
msg_ok      db  ' OK',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_pass    db  'IRQXTEST: all checks passed',13,10,'$'
msg_fail    db  'IRQXTEST: FAILURES - see above',13,10,'$'

_IX     ENDS
        END     start
