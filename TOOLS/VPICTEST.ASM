;=====================================================================
; VPICTEST.ASM - VPICTEST.COM, does a DOS box get a CARD'S interrupt?
;
; INT9TEST asked it of the keyboard, which the kernel delivers by hand.
; This asks it of everything else: a 16-bit driver hooks a vector,
; clears its line's bit in the mask register, and waits - and until
; the monitor stood in for the two interrupt controllers (IO\VPIC.INC)
; it waited for ever, because the interrupt went to a vector the
; kernel owns and was thrown away.
;
; THE REAL-TIME CLOCK IS THE CARD.  Every machine has one, every
; emulator has one, it interrupts as often as it is told to, and it is
; on the SECOND controller - so the cascade is tested too, and so is
; the pair of EOIs a handler behind it has to send.  Its interrupt
; stays asserted until register C is read, which is as near as ISA
; gets to a PCI line that stays asserted until the card is dealt with.
;
; What is asked, in order:
;
;   1  THE MASK IS THE BOX'S OWN.  Written, it reads back; and a box
;      that masks every line has not stopped the machine's clock.
;   2  THE INTERRUPT ARRIVES, at the vector a DOS program expects it
;      at, and keeps arriving.
;   3  IT IS IN SERVICE WHILE THE HANDLER RUNS - the line and the
;      cascade - and not after.
;   4  NO EOI, NO SECOND INTERRUPT.  A handler that does not end the
;      interrupt gets exactly one, and the rest when the EOI is sent.
;   5  MASKED, IT WAITS; unmasked, it carries on.
;   6  THE FIRST CONTROLLER TOO.  COM1, if there is one and the mouse
;      is not on it: a UART interrupts the moment it is allowed to,
;      to say it has nothing left to send.
;
; Every wait is bounded by the tick count at 40:6C, and the tick count
; by a number of turns of a loop, so a failure is a line that says so.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_VP     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_VP, DS:_VP, ES:_VP, SS:_VP
        ORG     0

RTC_INDEX       EQU     70h
RTC_DATA        EQU     71h
RTC_A           EQU     0Ah             ; the rate, in the low four bits
RTC_B           EQU     0Bh             ; bit 6: interrupt at that rate
RTC_C           EQU     0Ch             ; why it interrupted; reading it
                                        ; is what ends the request
RTC_64HZ        EQU     0Ah
RTC_PIE         EQU     40h

UART_IER        EQU     1               ; which events interrupt
UART_IIR        EQU     2               ; which one did; reading it ends
                                        ; a "nothing to send"
UART_MCR        EQU     4               ; bit 3 lets the line out at all
UART_THRE       EQU     02h
UART_OUT2       EQU     08h
COM_IRQ         EQU     4
COM_VECTOR      EQU     0Ch

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; 1 - the mask registers
;=====================================================================
        in      al,21h
        mov     [mask1],al
        in      al,0A1h
        mov     [mask2],al

        mov     dx,OFFSET t_mask1
        call    puts
        mov     al,0A5h
        out     21h,al
        in      al,21h
        call    put2
        cmp     al,0A5h
        jne     m1_bad
        call    ok
        jmp     short m2
m1_bad:
        call    bad
m2:
        mov     dx,OFFSET t_mask2
        call    puts
        mov     al,5Ah
        out     0A1h,al
        in      al,0A1h
        call    put2
        cmp     al,5Ah
        jne     m2_bad
        call    ok
        jmp     short m3
m2_bad:
        call    bad
m3:
        mov     dx,OFFSET t_clock
        call    puts
        mov     al,0FFh                 ; every line, the timer's included
        out     21h,al
        out     0A1h,al
        mov     ax,3
        call    waitt
        pushf
        mov     al,[mask1]
        out     21h,al
        mov     al,[mask2]
        out     0A1h,al
        popf
        jc      m3_bad
        call    ok
        jmp     short rtc
m3_bad:
        call    bad

;=====================================================================
; 2 - the real-time clock, sixty-four times a second
;=====================================================================
rtc:
        mov     ax,3570h
        int     21h
        mov     word ptr [old70],bx
        mov     word ptr [old70+2],es
        push    cs
        pop     es
        mov     dx,OFFSET i70
        mov     ax,2570h
        int     21h

        cli
        mov     al,RTC_A
        call    rtcget
        mov     [save_a],al
        and     al,0F0h
        or      al,RTC_64HZ
        mov     ah,al
        mov     al,RTC_A
        call    rtcput
        mov     al,RTC_B
        call    rtcget
        mov     [save_b],al
        or      al,RTC_PIE
        mov     ah,al
        mov     al,RTC_B
        call    rtcput
        mov     al,RTC_C                ; and nothing left over from before
        call    rtcget
        sti

        in      al,21h                  ; the cascade, and then the line
        and     al,0FBh
        out     21h,al
        in      al,0A1h
        and     al,0FEh
        out     0A1h,al

        mov     dx,OFFSET t_arrive
        call    puts
        mov     ax,18                   ; a second: sixty-four of them
        call    waitt
        mov     ax,word ptr [nint]
        call    put4
        cmp     ax,16
        jb      r2_bad
        call    ok
        jmp     short r3
r2_bad:
        call    bad
        jmp     rtc_off                 ; nothing below can mean anything

;=====================================================================
; 3 - in service inside the handler, and not outside it
;=====================================================================
r3:
        mov     dx,OFFSET t_inserv
        call    puts
        mov     al,[isr2]
        call    put2
        mov     dl,'/'
        call    putc
        mov     al,[isr1]
        call    put2
        test    byte ptr [isr2],01h     ; line 8
        jz      r3_bad
        test    byte ptr [isr1],04h     ; and the cascade
        jz      r3_bad
        call    ok
        jmp     short r3b
r3_bad:
        call    bad
r3b:
        mov     dx,OFFSET t_outserv
        call    puts
        cli                             ; between two interrupts, not in one
        mov     al,0Bh
        out     0A0h,al
        in      al,0A0h
        mov     ah,al
        mov     al,0Ah
        out     0A0h,al
        mov     al,0Bh
        out     20h,al
        in      al,20h
        mov     bl,al
        mov     al,0Ah
        out     20h,al
        sti
        mov     al,ah
        call    put2
        mov     dl,'/'
        call    putc
        mov     al,bl
        call    put2
        test    ah,01h
        jnz     r3b_bad
        test    bl,04h
        jnz     r3b_bad
        call    ok
        jmp     short r4
r3b_bad:
        call    bad

;=====================================================================
; 4 - a handler that does not send the EOI gets one interrupt
;=====================================================================
r4:
        mov     dx,OFFSET t_noeoi
        call    puts
        cli
        mov     word ptr [nheld],0
        mov     byte ptr [noeoi],1
        sti
        mov     ax,6
        call    waitt
        mov     ax,word ptr [nheld]
        call    put4
        cmp     ax,1
        jne     r4_bad
        call    ok
        jmp     short r4b
r4_bad:
        call    bad
r4b:
        mov     dx,OFFSET t_eoi
        call    puts
        cli
        mov     byte ptr [noeoi],0
        mov     ax,word ptr [nint]
        mov     [before],ax
        mov     al,20h                  ; the EOI it was waiting for, to
        out     0A0h,al                 ; the second controller and then
        out     20h,al                  ; the first
        sti
        mov     ax,6
        call    waitt
        mov     ax,word ptr [nint]
        sub     ax,[before]
        call    put4
        cmp     ax,4
        jb      r4b_bad
        call    ok
        jmp     short r5
r4b_bad:
        call    bad

;=====================================================================
; 5 - masked it waits, unmasked it carries on
;=====================================================================
r5:
        mov     dx,OFFSET t_masked
        call    puts
        in      al,0A1h
        or      al,01h
        out     0A1h,al
        mov     ax,2                    ; one already on its way may land
        call    waitt
        mov     ax,word ptr [nint]
        mov     [before],ax
        mov     ax,6
        call    waitt
        mov     ax,word ptr [nint]
        sub     ax,[before]
        call    put4
        or      ax,ax
        jnz     r5_bad
        call    ok
        jmp     short r5b
r5_bad:
        call    bad
r5b:
        mov     dx,OFFSET t_unmasked
        call    puts
        mov     ax,word ptr [nint]
        mov     [before],ax
        in      al,0A1h
        and     al,0FEh
        out     0A1h,al
        mov     ax,6
        call    waitt
        mov     ax,word ptr [nint]
        sub     ax,[before]
        call    put4
        cmp     ax,4
        jb      r5b_bad
        call    ok
        jmp     short rtc_off
r5b_bad:
        call    bad

;=====================================================================
; Everything back as it was found, and the handler out of the way
; before anything else is printed.
;=====================================================================
rtc_off:
        cli
        mov     ah,[save_b]
        mov     al,RTC_B
        call    rtcput
        mov     ah,[save_a]
        mov     al,RTC_A
        call    rtcput
        mov     al,RTC_C
        call    rtcget
        mov     al,[mask2]
        out     0A1h,al
        mov     al,[mask1]
        out     21h,al
        sti
        push    ds
        mov     dx,word ptr [old70]
        mov     ax,word ptr [old70+2]
        mov     ds,ax
        mov     ax,2570h
        int     21h
        pop     ds

;=====================================================================
; 6 - COM1, which is line 4 on the first controller
;=====================================================================
        mov     dx,OFFSET t_com
        call    puts
        push    es
        mov     ax,40h
        mov     es,ax
        mov     ax,es:[0]
        pop     es
        mov     [combase],ax
        or      ax,ax
        jz      com_none
        mov     ax,0024h                ; the kernel's mouse driver says
        xor     cx,cx                   ; what it is driving: CH = 3 is a
        int     33h                     ; serial one, and CL is its line
        cmp     ch,3
        jne     com_there
        cmp     cl,COM_IRQ
        jne     com_there
com_none:
        mov     dx,OFFSET msg_nocom
        call    puts
        jmp     report
com_there:
        mov     ax,3500h + COM_VECTOR
        int     21h
        mov     word ptr [oldcom],bx
        mov     word ptr [oldcom+2],es
        push    cs
        pop     es
        mov     dx,OFFSET icom
        mov     ax,2500h + COM_VECTOR
        int     21h

        cli
        mov     dx,[combase]
        add     dx,UART_IER
        in      al,dx
        mov     [save_ier],al
        xor     al,al
        out     dx,al
        mov     dx,[combase]
        add     dx,UART_MCR
        in      al,dx
        mov     [save_mcr],al
        or      al,UART_OUT2
        out     dx,al
        mov     dx,[combase]
        add     dx,UART_IIR
        in      al,dx
        in      al,21h
        mov     [mask1],al
        and     al,0EFh                 ; line 4
        out     21h,al
        mov     dx,[combase]
        add     dx,UART_IER
        mov     al,UART_THRE
        out     dx,al
        sti
        mov     ax,4
        call    waitt

        cli
        mov     dx,[combase]
        add     dx,UART_IER
        mov     al,[save_ier]
        out     dx,al
        mov     dx,[combase]
        add     dx,UART_MCR
        mov     al,[save_mcr]
        out     dx,al
        mov     al,[mask1]
        out     21h,al
        sti
        push    ds
        mov     dx,word ptr [oldcom]
        mov     ax,word ptr [oldcom+2]
        mov     ds,ax
        mov     ax,2500h + COM_VECTOR
        int     21h
        pop     ds

        mov     ax,word ptr [ncom]
        call    put4
        or      ax,ax
        jz      com_bad
        call    ok
        jmp     short report
com_bad:
        call    bad

;=====================================================================
report:
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
; i70 - the handler under test.  Everything through CS: an interrupt
;       arrives with whatever DS the interrupted code was using.
;=====================================================================
i70:
        push    ax
        mov     al,RTC_C                ; why, which is also what makes the
        out     RTC_INDEX,al            ; clock let go of the line
        in      al,RTC_DATA

        cmp     byte ptr cs:[sampled],0
        jne     i70_count
        mov     byte ptr cs:[sampled],1
        mov     al,0Bh                  ; what is in service, on both
        out     0A0h,al
        in      al,0A0h
        mov     byte ptr cs:[isr2],al
        mov     al,0Ah
        out     0A0h,al
        mov     al,0Bh
        out     20h,al
        in      al,20h
        mov     byte ptr cs:[isr1],al
        mov     al,0Ah
        out     20h,al

i70_count:
        cmp     byte ptr cs:[noeoi],0
        jne     i70_held
        inc     word ptr cs:[nint]
        mov     al,20h
        out     0A0h,al
        out     20h,al
        pop     ax
        iret
i70_held:
        inc     word ptr cs:[nheld]
        pop     ax
        iret

;=====================================================================
; icom - the UART's
;=====================================================================
icom:
        push    ax
        push    dx
        mov     dx,cs:[combase]
        add     dx,UART_IIR
        in      al,dx
        inc     word ptr cs:[ncom]
        mov     al,20h
        out     20h,al
        pop     dx
        pop     ax
        iret

;=====================================================================
; rtcget - AL = a register -> AL.   rtcput - AL = a register, AH = the
; byte for it.
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
old70       dd  0
oldcom      dd  0
nint        dw  0                       ; interrupts that were ended
nheld       dw  0                       ; ...and that were not
ncom        dw  0
combase     dw  0
before      dw  0
mask1       db  0
mask2       db  0
save_a      db  0
save_b      db  0
isr1        db  0                       ; in service, first controller
isr2        db  0                       ; ...and second
sampled     db  0
noeoi       db  0
save_ier    db  0
save_mcr    db  0
fails       db  0

msg_hi      db  13,10,'VPICTEST - does a DOS box get a card''s interrupt?'
            db  13,10,13,10,'$'
t_mask1     db  'mask, first controller .. $'
t_mask2     db  'mask, second controller . $'
t_clock     db  'clock runs, all masked .. $'
t_arrive    db  'IRQ 8 at INT 70h, 1s .... $'
t_inserv    db  'in service, in handler .. $'
t_outserv   db  'in service, outside it .. $'
t_noeoi     db  'interrupts with no EOI .. $'
t_eoi       db  'and after the EOI ....... $'
t_masked    db  'interrupts while masked . $'
t_unmasked  db  'and after unmasking ..... $'
t_com       db  'IRQ 4 at INT 0Ch, COM1 .. $'
msg_nocom   db  'no port, or the mouse has it (skipped)',13,10,'$'
msg_ok      db  ' OK',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_pass    db  'VPICTEST: all checks passed',13,10,'$'
msg_fail    db  'VPICTEST: FAILURES - see above',13,10,'$'

_VP     ENDS
        END     start
