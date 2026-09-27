;=====================================================================
; DPMIVEC.ASM - DPMIVEC.COM, can a DPMI client put a handler on a
;               software interrupt, and pass on what it does not want?
;
; DPMIIRQ asks whether a card's interrupt reaches a client's handler.
; This asks the same of an INT instruction, which is what an extender
; hooks INT 21h and INT 10h with, and of the half nothing had looked
; at: a handler is installed over whatever was there before, and what
; was there before is an address the host gave out (0204h) and the
; handler jumps to.  Both ends of that have to be real - the handler
; needs a frame to return through, and the address has to be
; somewhere.
;
; Fourteen checks, all but the first made from protected mode:
;
;    1  the switch
;    2  a thousand BIOS calls, one after another
;    3  0204h: every vector has a default, and they are not the same
;    4  INT 60h with no handler is run in real mode
;    5  ...a thousand times
;    6  0205h: a handler on INT 60h is called, and returns
;    7  ...and what it passes on gets to real mode, and back
;    8  INT 21h through a handler of the client's
;    9  INT 10h through one
;   10  INT 31h through one
;   11  0205h given the default back: no handler
;   12  0205h refuses a selector that is not code
;   13  IRQ 8, passed on by the handler to real mode
;   14  ...and what it interrupted has the flags it had
;
; INT 60h is a vector nothing owns.  The program puts a real-mode
; handler on it before it switches, which answers AX = 6060h, BX one
; more than it was given and the carry flag set - so that what comes
; back from real mode is registers AND flags.
;
; Assembled twice: as it stands it is DPMIVEC.COM, a 16-bit client, and
; with -dCLIENT32 it is DPMIV32.COM, the same program as a 32-bit one.
; That one is still 16-bit code in a 16-bit segment, which is what an
; extender's own code is; what changes is what the host does for it -
; offsets are 32 bits, and so is every frame a handler is given.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_DV     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_DV, DS:_DV, ES:_DV, SS:_DV
        ORG     0

CMOS_INDEX      EQU     70h
CMOS_DATA       EQU     71h
CMOS_A          EQU     0Ah
CMOS_B          EQU     0Bh
CMOS_C          EQU     0Ch
CMOS_64HZ       EQU     0Ah
CMOS_PIE        EQU     40h
MANY            EQU     1000

; A handler's frame is as wide as the client: IP, CS and FLAGS as words
; for a 16-bit one and as dwords for a 32-bit one, whose handlers end
; with an IRETD although the segment they are in is a 16-bit one.
IFDEF CLIENT32
PMFLAGS         EQU     2 + 8           ; past the BP that was pushed
PMIRET          MACRO
        db      66h
        iret
                ENDM
ELSE
PMFLAGS         EQU     2 + 4
PMIRET          MACRO
        iret
                ENDM
ENDIF
ANSWER60        EQU     6060h
ANSWERPM        EQU     1234h

start:
        mov     ah,4Ah                  ; a .COM is given all the memory
        mov     bx,1000h                ; there is, and the host wants some
        int     21h

        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results
        mov     [rm_seg],cs

;=====================================================================
; 1 - into protected mode
;=====================================================================
        mov     dx,OFFSET msg_t1
        call    puts
        mov     ax,1687h
        int     2Fh
        or      ax,ax
        jnz     t1_none
        mov     [entry_off],di
        mov     [entry_seg],es
        push    cs
        pop     es
        mov     bx,si
        or      bx,bx
        jz      t1_switch
        mov     ah,48h
        int     21h
        jc      t1_none
        mov     es,ax
t1_switch:
IFDEF CLIENT32
        mov     ax,1                    ; a 32-bit client
ELSE
        xor     ax,ax                   ; a 16-bit client
ENDIF
        call    dword ptr [entry_off]
        jc      t1_none
        mov     [pm_ds],ds
        mov     [pm_cs],cs
        push    ds
        pop     es
        call    good
        jmp     short t2
t1_none:
        push    cs
        pop     es
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_nodpmi
        call    puts
        jmp     finish

;=====================================================================
; 2 - a thousand calls to the BIOS.  Each is a DOS box started and
;     stopped on the host's own stack, and what the host leaves behind
;     on that stack is what this measures.
;=====================================================================
t2:
        mov     dx,OFFSET msg_t2
        call    puts
        mov     ax,0002h                ; the BIOS data area, for the clock
        mov     bx,0040h
        int     31h
        mov     [bdasel],ax

        call    ticks
        mov     [tickwas],ax
        mov     cx,MANY
t2_one:
        mov     ah,0Fh
        int     10h
        cmp     ah,80
        jne     t2_bad
        loop    t2_one
        call    ticks
        sub     ax,[tickwas]
        call    put4
        call    good
        jmp     short t3
t2_bad:
        call    bad

;=====================================================================
; 3 - where a vector goes before anybody has touched it
;=====================================================================
t3:
        mov     dx,OFFSET msg_t3
        call    puts
        mov     ax,0204h
        mov     bl,60h
        int     31h
        jc      t3_bad
        mov     [old60],dx
        mov     [old60+2],cx
        mov     ax,cx
        call    put4
        mov     ax,dx
        call    put4
        mov     ax,0204h
        mov     bl,61h
        int     31h
        jc      t3_bad
        cmp     dx,[old60]              ; two vectors, two addresses
        jne     t3_code
        cmp     cx,[old60+2]
        je      t3_bad
t3_code:
        lar     ax,word ptr [old60+2]
        jnz     t3_bad
        test    ah,08h                  ; and code is what is there
        jz      t3_bad
        call    good
        jmp     short t4
t3_bad:
        call    bad
        jmp     finish                  ; nothing below can be tried

;=====================================================================
; 4 - INT 60h, which nobody has a handler on
;=====================================================================
t4:
        mov     dx,OFFSET msg_t4
        call    puts
        mov     ax,0200h
        mov     bl,60h
        int     31h
        mov     [rmold60],dx
        mov     [rmold60+2],cx
        mov     ax,0201h
        mov     bl,60h
        mov     cx,[rm_seg]
        mov     dx,OFFSET rm60
        movzx   edx,dx
        int     31h

        mov     [spwas],sp
        mov     bx,4000h
        xor     ax,ax
        clc
        int     60h
        jnc     t4_bad
        call    put4
        cmp     ax,ANSWER60
        jne     t4_bad
        cmp     bx,4001h
        jne     t4_bad
        cmp     sp,[spwas]
        jne     t4_bad
        call    good
        jmp     short t5
t4_bad:
        mov     sp,[spwas]
        call    bad

;=====================================================================
; 5 - and again, a thousand times
;=====================================================================
t5:
        mov     dx,OFFSET msg_t5
        call    puts
        call    ticks
        mov     [tickwas],ax
        mov     cx,MANY
        xor     bx,bx
t5_one:
        xor     ax,ax
        int     60h
        cmp     ax,ANSWER60
        jne     t5_bad
        loop    t5_one
        cmp     bx,MANY
        jne     t5_bad
        call    ticks
        sub     ax,[tickwas]
        call    put4
        call    good
        jmp     short t6
t5_bad:
        call    bad

;=====================================================================
; 6 - a handler of our own, which answers and returns
;=====================================================================
t6:
        mov     dx,OFFSET msg_t6
        call    puts
        mov     ax,0205h
        mov     bl,60h
        mov     cx,[pm_cs]
        mov     dx,OFFSET pm60
        movzx   edx,dx
        int     31h
        jc      t6_bad
        mov     byte ptr [passon],0
        mov     [spwas],sp
        xor     ax,ax
        stc
        int     60h
        jc      t6_bad                  ; the handler cleared it
        call    put4
        cmp     ax,ANSWERPM
        jne     t6_bad
        cmp     sp,[spwas]
        jne     t6_bad
        cmp     word ptr [n60],1
        jne     t6_bad
        mov     ax,ds
        cmp     ax,[pm_ds]
        jne     t6_bad
        call    good
        jmp     short t7
t6_bad:
        mov     sp,[spwas]
        call    bad

;=====================================================================
; 7 - and one that passes the call on
;=====================================================================
t7:
        mov     dx,OFFSET msg_t7
        call    puts
        mov     byte ptr [passon],1
        mov     [spwas],sp
        mov     bx,5000h
        xor     ax,ax
        clc
        int     60h
        jnc     t7_bad
        call    put4
        cmp     ax,ANSWER60
        jne     t7_bad
        cmp     bx,5001h
        jne     t7_bad
        cmp     sp,[spwas]
        jne     t7_bad
        cmp     word ptr [n60],2
        jne     t7_bad
        call    good
        jmp     short t8
t7_bad:
        mov     sp,[spwas]
        call    bad

;=====================================================================
; 8 - INT 21h.  Every line printed from here to the end of the check
;     goes through the handler as well, with a pointer in DS:DX.
;=====================================================================
t8:
        mov     dx,OFFSET msg_t8
        call    puts
        mov     ax,0204h
        mov     bl,21h
        int     31h
        mov     [old21],dx
        mov     [old21+2],cx
        mov     ax,0205h
        mov     bl,21h
        mov     cx,[pm_cs]
        mov     dx,OFFSET pm21
        movzx   edx,dx
        int     31h
        jc      t8_bad

        mov     [spwas],sp
        mov     ah,30h
        int     21h
        or      al,al
        jz      t8_bad
        mov     ax,3D00h                ; a file there is not
        mov     dx,OFFSET nofile
        movzx   edx,dx
        clc
        int     21h
        jnc     t8_bad
        cmp     ax,2
        jne     t8_bad
        mov     ah,09h                  ; and a pointer
        mov     dx,OFFSET msg_via
        movzx   edx,dx
        int     21h
        cmp     sp,[spwas]
        jne     t8_bad
        mov     ax,[n21]
        call    put4
        cmp     ax,3
        jne     t8_bad
        call    t8_off
        call    good
        jmp     short t9
t8_bad:
        mov     sp,[spwas]
        call    t8_off
        call    bad
        jmp     short t9
t8_off:
        mov     ax,0205h
        mov     bl,21h
        mov     cx,[old21+2]
        mov     dx,[old21]
        movzx   edx,dx
        int     31h
        ret

;=====================================================================
; 9 - INT 10h, which has a gate of its own
;=====================================================================
t9:
        mov     dx,OFFSET msg_t9
        call    puts
        mov     ax,0204h
        mov     bl,10h
        int     31h
        mov     [old10],dx
        mov     [old10+2],cx
        mov     ax,0205h
        mov     bl,10h
        mov     cx,[pm_cs]
        mov     dx,OFFSET pm10
        movzx   edx,dx
        int     31h
        jc      t9_bad
        mov     [spwas],sp
        mov     ah,0Fh
        int     10h
        cmp     ah,80
        jne     t9_bad
        cmp     sp,[spwas]
        jne     t9_bad
        mov     ax,[n10]
        call    put4
        cmp     ax,1
        jne     t9_bad
        call    t9_off
        call    good
        jmp     short t10
t9_bad:
        mov     sp,[spwas]
        call    t9_off
        call    bad
        jmp     short t10
t9_off:
        mov     ax,0205h
        mov     bl,10h
        mov     cx,[old10+2]
        mov     dx,[old10]
        movzx   edx,dx
        int     31h
        ret

;=====================================================================
; 10 - INT 31h itself.  The call that takes the handler off again goes
;      through the handler.
;=====================================================================
t10:
        mov     dx,OFFSET msg_t10
        call    puts
        mov     ax,0204h
        mov     bl,31h
        int     31h
        mov     [old31],dx
        mov     [old31+2],cx
        mov     ax,0205h
        mov     bl,31h
        mov     cx,[pm_cs]
        mov     dx,OFFSET pm31
        movzx   edx,dx
        int     31h
        jc      t10_bad
        mov     [spwas],sp
        mov     ax,0400h
        stc
        int     31h
        jc      t10_bad
        cmp     ax,005Ah                ; 0.90
        jne     t10_bad
        mov     ax,0006h                ; and one that answers in CX:DX
        mov     bx,[pm_ds]
        int     31h
        jc      t10_bad
        mov     ax,[rm_seg]
        shr     ax,12
        cmp     ax,cx
        jne     t10_bad
        mov     ax,[rm_seg]
        shl     ax,4
        cmp     ax,dx
        jne     t10_bad
        cmp     sp,[spwas]
        jne     t10_bad
        call    t10_off
        mov     ax,[n31]
        call    put4
        cmp     ax,3
        jne     t10_bad2
        mov     ax,0400h                ; off: this one is not counted
        int     31h
        cmp     word ptr [n31],3
        jne     t10_bad2
        call    good
        jmp     short t11
t10_bad:
        mov     sp,[spwas]
        call    t10_off
t10_bad2:
        call    bad
        jmp     short t11
t10_off:
        mov     ax,0205h
        mov     bl,31h
        mov     cx,[old31+2]
        mov     dx,[old31]
        movzx   edx,dx
        int     31h
        ret

;=====================================================================
; 11 - the default, given back
;=====================================================================
t11:
        mov     dx,OFFSET msg_t11
        call    puts
        mov     ax,0205h
        mov     bl,60h
        mov     cx,[old60+2]
        mov     dx,[old60]
        movzx   edx,dx
        int     31h
        jc      t11_bad
        mov     ax,0204h
        mov     bl,60h
        int     31h
        jc      t11_bad
        cmp     cx,[old60+2]
        jne     t11_bad
        cmp     dx,[old60]
        jne     t11_bad
        mov     [spwas],sp
        xor     ax,ax
        int     60h
        cmp     ax,ANSWER60
        jne     t11_bad
        cmp     sp,[spwas]
        jne     t11_bad
        cmp     word ptr [n60],2        ; as it was
        jne     t11_bad
        call    good
        jmp     short t12
t11_bad:
        mov     sp,[spwas]
        call    bad

;=====================================================================
; 12 - a handler has to be somewhere an interrupt can go
;=====================================================================
t12:
        mov     dx,OFFSET msg_t12
        call    puts
        mov     ax,0205h
        mov     bl,61h
        mov     cx,[pm_ds]              ; data
        mov     dx,OFFSET pm60
        movzx   edx,dx
        int     31h
        jnc     t12_bad
        call    good
        jmp     short t13
t12_bad:
        mov     ax,0205h                ; it took it: take it off again
        mov     bl,61h
        mov     cx,[old60+2]
        mov     dx,[old60]
        movzx   edx,dx
        int     31h
        call    bad

;=====================================================================
; 13 - the clock's interrupt.  The handler in protected mode counts it
;      and passes it on; the one in real mode counts it, reads the
;      clock's status so that there is a next one, and says it is
;      finished.  Both counts, or the second handler was never reached.
;=====================================================================
t13:
        mov     dx,OFFSET msg_t13
        call    puts
        mov     ax,0200h
        mov     bl,70h
        int     31h
        mov     [rmold70],dx
        mov     [rmold70+2],cx
        mov     ax,0201h
        mov     bl,70h
        mov     cx,[rm_seg]
        mov     dx,OFFSET rm70
        movzx   edx,dx
        int     31h
        mov     ax,0204h
        mov     bl,70h
        int     31h
        mov     [old70],dx
        mov     [old70+2],cx
        mov     ax,0205h
        mov     bl,70h
        mov     cx,[pm_cs]
        mov     dx,OFFSET pm70
        movzx   edx,dx
        int     31h
        jc      t13_bad

        cli
        mov     al,CMOS_A
        call    rtcget
        mov     [save_a],al
        and     al,0F0h
        or      al,CMOS_64HZ
        mov     ah,al
        mov     al,CMOS_A
        call    rtcput
        mov     al,CMOS_B
        call    rtcget
        mov     [save_b],al
        or      al,CMOS_PIE
        mov     ah,al
        mov     al,CMOS_B
        call    rtcput
        mov     al,CMOS_C
        call    rtcget
        in      al,21h
        mov     [mask1],al
        and     al,0FBh
        out     21h,al
        in      al,0A1h
        mov     [mask2],al
        and     al,0FEh
        out     0A1h,al
        sti

        mov     ax,18
        call    waitt
        mov     ax,[n70]
        mov     [n70was],ax
        call    flagrun                 ; for check 14, while they arrive

        cli
        mov     ah,[save_b]
        mov     al,CMOS_B
        call    rtcput
        mov     ah,[save_a]
        mov     al,CMOS_A
        call    rtcput
        mov     al,CMOS_C
        call    rtcget
        mov     al,[mask2]
        out     0A1h,al
        mov     al,[mask1]
        out     21h,al
        sti

        mov     ax,[n70was]
        call    put4
        mov     ax,[nrm70]
        call    put4
        cmp     word ptr [n70was],48
        jb      t13_bad
        mov     ax,[n70]
        cmp     ax,[nrm70]
        jne     t13_bad
        call    good
        jmp     short t14
t13_bad:
        call    bad

;=====================================================================
; 14 - what was interrupted.  Nobody called this interrupt, so nothing
;      it does is an answer: the code it arrived in the middle of has
;      to find its flags as it left them, whichever handler returned
;      and in whichever mode.  flagrun sat between a compare and a
;      jump for half a second of them.
;=====================================================================
t14:
        mov     dx,OFFSET msg_t14
        call    puts
        mov     ax,[n70]
        sub     ax,[n70was]
        call    put4
        cmp     ax,16                   ; and they did arrive
        jb      t14_bad
        cmp     byte ptr [flagbad],0
        jne     t14_bad
        call    good
        jmp     short t15
t14_bad:
        call    bad
t15:
        mov     ax,0205h
        mov     bl,70h
        mov     cx,[old70+2]
        mov     dx,[old70]
        movzx   edx,dx
        int     31h
        mov     ax,0201h
        mov     bl,70h
        mov     cx,[rmold70+2]
        mov     dx,[rmold70]
        movzx   edx,dx
        int     31h

finish:
        cmp     word ptr [rmold60+2],0  ; never changed, if never read
        jne     fin_put
        cmp     word ptr [rmold60],0
        je      fin_say
fin_put:
        mov     ax,0201h
        mov     bl,60h
        mov     cx,[rmold60+2]
        mov     dx,[rmold60]
        movzx   edx,dx
        int     31h
fin_say:
        mov     dx,OFFSET msg_res
        call    puts
        mov     dx,OFFSET results
        call    puts
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
; The handlers, in protected mode.  A code segment cannot be written
; through, so each finds the data selector in the code segment, where
; it can be read.
;=====================================================================
pm60:
        push    ds
        push    ax
        mov     ax,cs:[pm_ds]
        mov     ds,ax
        pop     ax
        inc     word ptr [n60]
        cmp     byte ptr [passon],0
        jne     pm60_on
        pop     ds
        mov     ax,ANSWERPM
        push    bp
        mov     bp,sp
        and     word ptr [bp+PMFLAGS],0FFFEh    ; the caller's carry flag
        pop     bp
        PMIRET
pm60_on:
        pop     ds
        jmp     dword ptr cs:[old60]

pm21:
        push    ds
        push    ax
        mov     ax,cs:[pm_ds]
        mov     ds,ax
        pop     ax
        inc     word ptr [n21]
        pop     ds
        jmp     dword ptr cs:[old21]

pm10:
        push    ds
        push    ax
        mov     ax,cs:[pm_ds]
        mov     ds,ax
        pop     ax
        inc     word ptr [n10]
        pop     ds
        jmp     dword ptr cs:[old10]

pm31:
        push    ds
        push    ax
        mov     ax,cs:[pm_ds]
        mov     ds,ax
        pop     ax
        inc     word ptr [n31]
        pop     ds
        jmp     dword ptr cs:[old31]

pm70:
        push    ds
        push    ax
        mov     ax,cs:[pm_ds]
        mov     ds,ax
        pop     ax
        inc     word ptr [n70]
        pop     ds
        jmp     dword ptr cs:[old70]

;=====================================================================
; And the two in real mode.
;=====================================================================
rm60:
        mov     ax,ANSWER60
        inc     bx
        push    bp
        mov     bp,sp
        or      word ptr [bp+6],1       ; the caller's carry flag
        pop     bp
        iret

rm70:
        push    ax
        mov     al,CMOS_C
        out     CMOS_INDEX,al
        in      al,CMOS_DATA
        inc     word ptr cs:[nrm70]
        mov     al,20h
        out     0A0h,al
        out     20h,al
        pop     ax
        iret

;=====================================================================
rtcget:
        out     CMOS_INDEX,al
        jmp     short $+2
        in      al,CMOS_DATA
        ret
rtcput:
        out     CMOS_INDEX,al
        jmp     short $+2
        mov     al,ah
        out     CMOS_DATA,al
        ret

;=====================================================================
; waitt - AX = ticks to wait, by the clock in the BIOS data area,
;         through the selector 0002h made for it.  CF if the clock
;         was not moving.
;=====================================================================
waitt:
        push    bx
        push    dx
        push    es
        push    ecx
        mov     es,[bdasel]
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
; flagrun - nine ticks between a compare and a jump.  LOOP leaves the
;           flags alone, so each inner loop holds one set of them for
;           a thousand jumps; an interrupt that comes back with
;           anything else lands in flagbad.
;=====================================================================
flagrun:
        push    ax
        push    cx
        push    dx
        call    ticks
        mov     dx,ax
fr_again:
        cmp     al,al                   ; zero and no carry
        mov     cx,1000
fr_zero:
        jnz     fr_bad
        jc      fr_bad
        loop    fr_zero
        mov     al,1
        cmp     al,2                    ; carry and not zero
        mov     cx,1000
fr_carry:
        jnc     fr_bad
        jz      fr_bad
        loop    fr_carry
        call    ticks
        sub     ax,dx
        cmp     ax,9
        jb      fr_again
        jmp     short fr_out
fr_bad:
        mov     byte ptr [flagbad],1
fr_out:
        pop     dx
        pop     cx
        pop     ax
        ret

;=====================================================================
; ticks - AX = the clock in the BIOS data area, low word
;=====================================================================
ticks:
        push    es
        mov     es,[bdasel]
        mov     ax,es:[6Ch]
        pop     es
        ret

;=====================================================================
good:
        push    dx
        mov     dl,'.'
        call    tally
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dl,'X'
        call    tally
        mov     dx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     dx
        ret
tally:
        push    bx
        mov     bx,[resptr]
        mov     [bx],dl
        inc     bx
        mov     byte ptr [bx],'$'
        mov     [resptr],bx
        pop     bx
        ret

puts:
        push    ax
        movzx   edx,dx                  ; DS:EDX, to a 32-bit client
        mov     ah,09h
        int     21h
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
put4:                                   ; AX in hex, and a space
        push    ax
        push    cx
        push    dx
        mov     cx,4
p4_lp:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p4_d
        add     al,7
p4_d:
        mov     dl,al
        call    putc
        pop     ax
        loop    p4_lp
        mov     dl,' '
        call    putc
        pop     dx
        pop     cx
        pop     ax
        ret

;=====================================================================
entry_off   dw  0
entry_seg   dw  0
rm_seg      dw  0
pm_ds       dw  0
pm_cs       dw  0
bdasel      dw  0
old60       dw  0,0                     ; offset, then selector
old21       dw  0,0
old10       dw  0,0
old31       dw  0,0
old70       dw  0,0
rmold60     dw  0,0                     ; offset, then segment
rmold70     dw  0,0
n60         dw  0
n21         dw  0
n10         dw  0
n31         dw  0
n70         dw  0
nrm70       dw  0
spwas       dw  0
tickwas     dw  0
n70was      dw  0
resptr      dw  0
passon      db  0
flagbad     db  0
mask1       db  0
mask2       db  0
save_a      db  0
save_b      db  0
fails       db  0
nofile      db  'NOSUCH.$$$',0
results     db  20 dup('$')

IFDEF CLIENT32
msg_hi      db  13,10,'DPMIV32 - a 32-bit client''s own handlers, and '
            db  'what is there without one',13,10,13,10,'$'
ELSE
msg_hi      db  13,10,'DPMIVEC - a client''s own handlers, and what is '
            db  'there without one',13,10,13,10,'$'
ENDIF
msg_t1      db  ' 1 into protected mode                   $'
msg_t2      db  ' 2 a thousand calls to the BIOS     $'
msg_t3      db  ' 3 0204h: a default each      $'
msg_t4      db  ' 4 INT 60h goes to real mode        $'
msg_t5      db  ' 5 ...a thousand times, in ticks    $'
msg_t6      db  ' 6 0205h: our handler answers       $'
msg_t7      db  ' 7 ...and passes it on              $'
msg_t8      db  ' 8 INT 21h through a handler$'
msg_via     db  ' (so is this) $'
msg_t9      db  ' 9 INT 10h through a handler        $'
msg_t10     db  '10 INT 31h through a handler        $'
msg_t11     db  '11 0205h: the default, given back        $'
msg_t12     db  '12 0205h refuses a data segment          $'
msg_t13     db  '13 IRQ 8, passed on, 1s        $'
msg_t14     db  '14 ...and leaves the flags alone    $'
msg_nodpmi  db  'no DPMI host (skipped)',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
IFDEF CLIENT32
msg_pass    db  'DPMIV32: all checks passed',13,10,'$'
msg_fail    db  'DPMIV32: FAILURES',13,10,'$'
ELSE
msg_pass    db  'DPMIVEC: all checks passed',13,10,'$'
msg_fail    db  'DPMIVEC: FAILURES',13,10,'$'
ENDIF

_DV     ENDS
        END     start
