;=====================================================================
; DPMIIRQ.ASM - DPMIIRQ.COM, does a DPMI client get a card's interrupt,
;               and can it reach a card's memory?
;
; VPICTEST asks the first question of a program in real mode.  A
; program that has gone into protected mode - which is every DOS game
; with an extender under it - hooks the interrupt with INT 31h 0205h
; instead of writing the vector table, and is in protected mode when
; the interrupt arrives.  The host has to take it there: onto the
; client's stack, into the client's handler, with the client's idea of
; the interrupt flag cleared until the handler sets it again.
;
; The second is INT 31h 0800h.  A card's registers and a frame buffer
; are at physical addresses far above any memory, and a client reaches
; them by asking for the address to be mapped and building a
; descriptor over what comes back.
;
; The real-time clock is the card again, for VPICTEST's reasons.
;
; Eight checks, all of them made from protected mode:
;
;   1  the switch
;   2  0205h takes a handler for INT 70h, and 0204h gives it back
;   3  the interrupt arrives there, sixty-four times a second
;   4  with the client's interrupts off it does not; on again, it does
;   5  0800h will not map the first megabyte
;   6  0800h maps the top of the address space, where the ROM is
;   7  ...and reading it does not fault
;   8  ...and it IS the ROM: what a processor starts at, which is a
;      jump
;
; Assembled twice: as it stands it is DPMIIRQ.COM, a 16-bit client, and
; with -dCLIENT32 it is DPMII32.COM, the same program as a 32-bit one -
; whose handler is given a frame of dwords (see DPMIVEC.ASM).
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_DI     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_DI, DS:_DI, ES:_DI, SS:_DI
        ORG     0

RTC_INDEX       EQU     70h
RTC_DATA        EQU     71h
RTC_A           EQU     0Ah
RTC_B           EQU     0Bh
RTC_C           EQU     0Ch
RTC_64HZ        EQU     0Ah
RTC_PIE         EQU     40h
ROM_HIGH        EQU     0FFFFh          ; the high word of FFFF0000h
ROM_BYTES       EQU     16

; A 32-bit client's handler is given a frame of dwords and ends with an
; IRETD, although the segment it is in is a 16-bit one.
IFDEF CLIENT32
PMIRET          MACRO
        db      66h
        iret
                ENDM
ELSE
PMIRET          MACRO
        iret
                ENDM
ENDIF

start:
        mov     ah,4Ah                  ; a .COM is given all the memory
        mov     bx,1000h                ; there is, and the host wants some
        int     21h

        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results

        ; What the ROM holds in its last sixteen bytes, read here where
        ; F000:FFF0 is an address.
        push    ds
        mov     ax,0F000h
        mov     ds,ax
        mov     si,0FFF0h
        mov     di,OFFSET romlow
        mov     cx,ROM_BYTES
        cld
        rep     movsb
        pop     ds

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
; 2 - a handler of our own on INT 70h
;=====================================================================
t2:
        mov     dx,OFFSET msg_t2
        call    puts
        mov     ax,0002h                ; the BIOS data area, for the clock
        mov     bx,0040h
        int     31h
        jc      t2_bad
        mov     [bdasel],ax

        mov     ax,0204h
        mov     bl,70h
        int     31h
        jc      t2_bad
        mov     [old_sel],cx
        mov     [old_off],dx

        mov     ax,0205h
        mov     bl,70h
        mov     cx,[pm_cs]
        mov     dx,OFFSET i70
        movzx   edx,dx
        int     31h
        jc      t2_bad
        mov     ax,0204h
        mov     bl,70h
        int     31h
        jc      t2_bad
        cmp     cx,[pm_cs]
        jne     t2_bad
        cmp     dx,OFFSET i70
        jne     t2_bad
        call    good
        jmp     short t3
t2_bad:
        call    bad
        jmp     finish

;=====================================================================
; 3 - the clock, interrupting
;=====================================================================
t3:
        mov     dx,OFFSET msg_t3
        call    puts
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
        mov     al,RTC_C
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
        mov     ax,[nint]
        call    put4
        cmp     ax,16
        jb      t3_bad
        call    good
        jmp     short t4
t3_bad:
        call    bad
        jmp     rtc_off

;=====================================================================
; 4 - the client's own interrupt flag
;=====================================================================
t4:
        mov     dx,OFFSET msg_t4
        call    puts
        cli
        mov     ax,[nint]
        mov     [before],ax
        mov     ax,6
        call    waitt
        mov     ax,[nint]
        sub     ax,[before]
        mov     [during],ax
        mov     ax,[nint]
        mov     [before],ax
        sti
        mov     ax,6
        call    waitt
        mov     ax,[during]
        call    put4
        mov     dl,'/'
        call    putc
        mov     ax,[nint]
        sub     ax,[before]
        call    put4
        cmp     word ptr [during],0
        jne     t4_bad
        cmp     ax,4
        jb      t4_bad
        call    good
        jmp     short rtc_off
t4_bad:
        call    bad

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
        mov     ax,0205h
        mov     bl,70h
        mov     cx,[old_sel]
        mov     dx,[old_off]
        movzx   edx,dx
        int     31h

;=====================================================================
; 5 - the first megabyte is not device memory
;=====================================================================
        mov     dx,OFFSET msg_t5
        call    puts
        mov     ax,0800h
        mov     bx,000Ah                ; A0000h
        xor     cx,cx
        xor     si,si
        mov     di,1000h
        int     31h
        jnc     t5_bad
        call    good
        jmp     short t6
t5_bad:
        call    bad

;=====================================================================
; 6 - the last 64K of the address space
;=====================================================================
t6:
        mov     dx,OFFSET msg_t6
        call    puts
        mov     ax,0800h
        mov     bx,ROM_HIGH
        xor     cx,cx
        mov     si,1                    ; 64K
        xor     di,di
        int     31h
        jc      t6_bad
        cmp     bx,ROM_HIGH             ; mapped where it is
        jne     t6_bad
        or      cx,cx
        jnz     t6_bad
        mov     ax,0000h
        mov     cx,1
        int     31h
        jc      t6_bad
        mov     [romsel],ax
        mov     bx,ax
        mov     cx,ROM_HIGH
        xor     dx,dx
        mov     ax,0007h
        int     31h
        jc      t6_bad
        mov     bx,[romsel]
        xor     cx,cx
        mov     dx,0FFFFh
        mov     ax,0008h
        int     31h
        jc      t6_bad
        call    good
        jmp     short t7
t6_bad:
        call    bad
        jmp     finish

;=====================================================================
; 7 - reading it.  Before it was mapped this was a page fault, and a
;     page fault in a client is the end of the client: the line below
;     this one being printed at all is the result.
;=====================================================================
t7:
        mov     dx,OFFSET msg_t7
        call    puts
        push    ds
        push    ds
        pop     es
        mov     ds,[romsel]
        mov     si,0FFF0h
        mov     di,OFFSET romhigh
        mov     cx,ROM_BYTES
        cld
        rep     movsb
        pop     ds
        call    good

;=====================================================================
; 8 - is it the ROM?  A machine decodes its BIOS at the top of the
;     address space as well as below 1MB, because that is where a
;     processor starts - and what it starts at is a jump.  The copy
;     below 1MB is NOT compared byte for byte unless that fails: on a
;     real board it is shadow RAM, which the BIOS unpacked itself into
;     and is entitled to have changed.  An emulator may decode nothing
;     up there at all, and then what is read is sixteen bytes of
;     nothing.
;=====================================================================
        mov     dx,OFFSET msg_t8
        call    puts
        mov     si,OFFSET romhigh
        mov     cx,4
t8_show:
        mov     al,[si]
        call    put2
        inc     si
        loop    t8_show
        mov     si,OFFSET romhigh
        mov     cx,ROM_BYTES
        mov     al,0FFh
t8_any:
        and     al,[si]
        inc     si
        loop    t8_any
        cmp     al,0FFh
        je      t8_none
        mov     al,[romhigh]
        cmp     al,0EAh                 ; JMP FAR
        je      t8_is
        cmp     al,0E9h                 ; JMP NEAR
        je      t8_is
        cmp     al,0EBh                 ; JMP SHORT
        je      t8_is
        mov     si,OFFSET romhigh
        mov     di,OFFSET romlow
        mov     cx,ROM_BYTES
        repe    cmpsb
        jne     t8_bad
t8_is:
        call    good
        jmp     short finish
t8_none:
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_norom
        call    puts
        jmp     short finish
t8_bad:
        call    bad

finish:
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
; i70 - the handler, in protected mode.  A code segment cannot be
;       written through, so the first thing it needs is the data
;       selector - which it finds in the code segment, where it can be
;       read.  It ends with STI and then IRET, as the specification
;       says a handler must: an IRET at ring 3 does not put the
;       interrupt flag back.
;=====================================================================
i70:
        push    ax
        push    ds
        mov     ax,cs:[pm_ds]
        mov     ds,ax
        mov     al,RTC_C
        out     RTC_INDEX,al
        in      al,RTC_DATA
        inc     word ptr [nint]
        mov     al,20h
        out     0A0h,al
        out     20h,al
        pop     ds
        pop     ax
        sti
        PMIRET

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
put2:                                   ; AL in hex
        push    ax
        push    cx
        push    dx
        mov     ah,al
        mov     cx,2
        jmp     short p4_lp
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
        pop     dx
        pop     cx
        pop     ax
        ret

;=====================================================================
entry_off   dw  0
entry_seg   dw  0
pm_ds       dw  0
pm_cs       dw  0
bdasel      dw  0
romsel      dw  0
old_sel     dw  0
old_off     dw  0
nint        dw  0
before      dw  0
during      dw  0
resptr      dw  0
mask1       db  0
mask2       db  0
save_a      db  0
save_b      db  0
fails       db  0
romlow      db  ROM_BYTES dup(0)
romhigh     db  ROM_BYTES dup(0)
results     db  16 dup('$')

IFDEF CLIENT32
msg_hi      db  13,10,'DPMII32 - a card''s interrupt, and its memory, '
            db  'in a 32-bit client',13,10,13,10,'$'
ELSE
msg_hi      db  13,10,'DPMIIRQ - a card''s interrupt, and its memory, '
            db  'in protected mode',13,10,13,10,'$'
ENDIF
msg_t1      db  ' 1 into protected mode                   $'
msg_t2      db  ' 2 0205h: a handler on INT 70h           $'
msg_t3      db  ' 3 IRQ 8 arrives there, 1s          $'
msg_t4      db  ' 4 interrupts off/on           $'
msg_t5      db  ' 5 0800h refuses the first megabyte      $'
msg_t6      db  ' 6 0800h maps FFFF0000h                  $'
msg_t7      db  ' 7 ...and it can be read                 $'
msg_t8      db  ' 8 ...and it is the ROM         $'
msg_nodpmi  db  'no DPMI host (skipped)',13,10,'$'
msg_norom   db  ' nothing decoded there (skipped)',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
IFDEF CLIENT32
msg_pass    db  'DPMII32: all checks passed',13,10,'$'
msg_fail    db  'DPMII32: FAILURES',13,10,'$'
ELSE
msg_pass    db  'DPMIIRQ: all checks passed',13,10,'$'
msg_fail    db  'DPMIIRQ: FAILURES',13,10,'$'
ENDIF

_DI     ENDS
        END     start
