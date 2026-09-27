;=====================================================================
; NICIRQ.ASM - NICIRQ.COM, a PCI card's interrupt, from the card to a
;              DOS program's handler.
;
; VPICTEST proves a box is handed an interrupt with the real-time
; clock, which is as near as ISA gets to PCI.  This is the thing
; itself: a PCI network card, found the way a DOS driver finds one,
; on whatever line the BIOS routed it to, and that line is LEVEL-
; triggered - it stays asserted until the card has been dealt with.
;
; The card is an NE2000, because there is nothing simpler that
; interrupts on request: given a packet to send it says "sent", and
; saying so is an interrupt.  The Realtek 8029 is the PCI one, and the
; one 86Box has.
;
; Seven checks:
;
;   1  the PCI BIOS answers INT 1Ah from a DOS box
;   2  it finds a network card, and the card is an 8029
;   3  the card has ports and a line, and the ports answer
;   4  the line is level-triggered, as a PCI line should be - printed,
;      and not a failure if the machine says otherwise
;   5  a packet sent is an interrupt at the handler
;   6  AN INTERRUPT THAT IS NOT DEALT WITH COMES BACK.  The handler
;      sends its EOI without telling the card it has been heard; on a
;      level-triggered line that is a second interrupt, at once, and
;      the handler deals with it then.  Two, and no more than two.
;   7  and the machine is still running: the clock moves
;
; A machine with no such card says so and tests nothing.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_NI     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_NI, DS:_NI, ES:_NI, SS:_NI
        ORG     0

; The 8390's registers, as offsets from the card's first port
NE_CR           EQU     00h             ; command
NE_PSTART       EQU     01h
NE_PSTOP        EQU     02h
NE_BNRY         EQU     03h
NE_TPSR         EQU     04h             ; the page a packet to send is in
NE_TBCR0        EQU     05h             ; ...and how long it is
NE_TBCR1        EQU     06h
NE_ISR          EQU     07h             ; why it interrupted; writing a bit
                                        ; back is what dismisses it
NE_RSAR0        EQU     08h
NE_RSAR1        EQU     09h
NE_RBCR0        EQU     0Ah
NE_RBCR1        EQU     0Bh
NE_RCR          EQU     0Ch
NE_TCR          EQU     0Dh
NE_DCR          EQU     0Eh
NE_IMR          EQU     0Fh             ; which reasons interrupt
NE_DATA         EQU     10h
NE_RESET        EQU     1Fh

ISR_PTX         EQU     02h             ; a packet was sent
ISR_TXE         EQU     08h             ; ...or could not be
ISR_RDC         EQU     40h             ; the card's memory was written
ISR_RST         EQU     80h
TXPAGE          EQU     40h
PKTLEN          EQU     60

start:
        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results

;=====================================================================
; 1 - the PCI BIOS
;=====================================================================
        mov     dx,OFFSET msg_t1
        call    puts
        mov     ax,0B101h
        xor     edx,edx
        int     1Ah
        jc      t1_none
        or      ah,ah
        jnz     t1_none
        cmp     edx,20494350h           ; 'PCI '
        jne     t1_none
        call    good
        jmp     short t2
t1_none:
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_nobios
        call    puts
        jmp     finish

;=====================================================================
; 2 - a network card
;=====================================================================
t2:
        mov     dx,OFFSET msg_t2
        call    puts
        mov     ax,0B103h
        mov     ecx,020000h             ; class 02, subclass 00: Ethernet
        xor     si,si
        int     1Ah
        jc      t2_none
        or      ah,ah
        jnz     t2_none
        mov     [pcidev],bx
        xor     di,di
        call    cfgdword
        mov     [pciid],ecx
        mov     eax,ecx
        call    put8
        cmp     ecx,802910ECh
        jne     t2_none
        call    good
        jmp     short t3
t2_none:
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_nocard
        call    puts
        jmp     finish

;=====================================================================
; 3 - where it is
;=====================================================================
t3:
        mov     dx,OFFSET msg_t3
        call    puts
        mov     di,10h
        call    cfgdword
        test    cl,1
        jz      t3_bad                  ; an 8029's first register is ports
        and     cx,0FFFCh
        mov     [base],cx
        mov     di,3Ch
        call    cfgdword
        mov     [irq],cl
        cmp     cl,15
        ja      t3_bad
        cmp     cl,2
        jbe     t3_bad

        mov     ax,[base]
        call    put4
        mov     dl,'/'
        call    putc
        mov     al,[irq]
        call    put2

        mov     di,04h                  ; and it answers to them, which the
        call    cfgdword                ; BIOS will have seen to
        test    cl,1
        jz      t3_bad

        call    nic_reset
        jc      t3_bad
        call    good
        jmp     short t4
t3_bad:
        call    bad
        jmp     finish

;=====================================================================
; 4 - edge or level
;=====================================================================
t4:
        mov     dx,OFFSET msg_t4
        call    puts
        mov     dx,4D1h
        in      al,dx
        mov     ah,al
        mov     dx,4D0h
        in      al,dx
        mov     [elcr],ax
        call    put4
        mov     cl,[irq]
        mov     bx,1
        shl     bx,cl
        test    ax,bx
        jz      t4_edge
        mov     byte ptr [level],1
        mov     dx,OFFSET msg_level
        call    puts
        jmp     short t5
t4_edge:
        mov     dx,OFFSET msg_edge
        call    puts

;=====================================================================
; 5 - one packet, one interrupt
;=====================================================================
t5:
        mov     al,[irq]                ; the vector a DOS program knows
        add     al,08h                  ; the line by
        cmp     byte ptr [irq],8
        jb      t5_vec
        add     al,70h - 10h
t5_vec:
        mov     [vector],al
        mov     ah,35h
        int     21h
        mov     word ptr [oldvec],bx
        mov     word ptr [oldvec+2],es
        push    cs
        pop     es
        mov     dx,OFFSET inic
        mov     al,[vector]
        mov     ah,25h
        int     21h

        in      al,21h
        mov     [mask1],al
        in      al,0A1h
        mov     [mask2],al
        call    unmask

        mov     dx,OFFSET msg_t5
        call    puts
        call    nic_start
        mov     byte ptr [dismiss],1
        mov     word ptr [nint],0
        call    nic_send
        mov     ax,9
        call    waitt
        mov     ax,[nint]
        call    put4
        cmp     ax,1
        jne     t5_bad
        call    good
        jmp     short t6
t5_bad:
        call    bad

;=====================================================================
; 6 - not dealt with, it comes back
;=====================================================================
t6:
        mov     dx,OFFSET msg_t6
        call    puts
        cli
        mov     byte ptr [dismiss],0    ; the first time: EOI and nothing
        mov     word ptr [nint],0       ; else
        sti
        call    nic_send
        mov     ax,9
        call    waitt
        mov     ax,[nint]
        call    put4
        mov     bx,1                    ; an edge is seen once
        cmp     byte ptr [level],0
        je      t6_want
        mov     bx,2                    ; a level until it is dismissed
t6_want:
        cmp     ax,bx
        jne     t6_bad
        call    good
        jmp     short t7
t6_bad:
        call    bad

;=====================================================================
; 7 - and the machine is still there
;=====================================================================
t7:
        cli
        mov     dx,[base]
        add     dx,NE_IMR
        xor     al,al
        out     dx,al
        mov     dx,[base]
        add     dx,NE_ISR
        mov     al,0FFh
        out     dx,al
        mov     dx,[base]
        mov     al,21h                  ; stopped
        out     dx,al
        mov     al,[mask2]
        out     0A1h,al
        mov     al,[mask1]
        out     21h,al
        sti
        push    ds
        mov     dx,word ptr [oldvec]
        mov     ax,word ptr [oldvec+2]
        mov     ds,ax
        mov     al,cs:[vector]
        mov     ah,25h
        int     21h
        pop     ds

        mov     dx,OFFSET msg_t7
        call    puts
        mov     ax,3
        call    waitt
        jc      t7_bad
        call    good
        jmp     short finish
t7_bad:
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
; inic - the handler.  It asks the card why, which is how a handler on
;        a shared line knows the interrupt is its own; it tells the
;        card it has been heard, unless this is the interrupt check 6
;        is leaving unanswered; and it sends the EOI either way.
;=====================================================================
inic:
        push    ax
        push    dx
        mov     dx,cs:[base]
        add     dx,NE_ISR
        in      al,dx
        mov     byte ptr cs:[lastisr],al
        inc     word ptr cs:[nint]
        cmp     byte ptr cs:[dismiss],0
        jne     inic_clear
        mov     byte ptr cs:[dismiss],1 ; next time
        jmp     short inic_eoi
inic_clear:
        out     dx,al                   ; every bit that was set, dismissed
inic_eoi:
        mov     al,20h
        cmp     byte ptr cs:[irq],8
        jb      inic_first
        out     0A0h,al
inic_first:
        out     20h,al
        pop     dx
        pop     ax
        iret

;=====================================================================
; unmask - the card's line, and the cascade if it is behind it
;=====================================================================
unmask:
        mov     cl,[irq]
        cmp     cl,8
        jb      um_first
        sub     cl,8
        mov     ah,1
        shl     ah,cl
        not     ah
        in      al,0A1h
        and     al,ah
        out     0A1h,al
        mov     cl,2
um_first:
        mov     ah,1
        shl     ah,cl
        not     ah
        in      al,21h
        and     al,ah
        out     21h,al
        ret

;=====================================================================
; The card.  nic_reset - CF if nothing like an 8390 is at [base].
;=====================================================================
nic_reset:
        mov     dx,[base]
        add     dx,NE_RESET
        in      al,dx
        out     dx,al
        mov     cx,8000h
ner_wait:
        mov     dx,[base]
        add     dx,NE_ISR
        in      al,dx
        test    al,ISR_RST
        jnz     ner_up
        loop    ner_wait
        stc
        ret
ner_up:
        mov     al,0FFh
        out     dx,al
        clc
        ret

;--- nic_start - out of reset and running, interrupting when a packet
;--- has gone or has failed to
nic_start:
        mov     al,21h                  ; stopped, page 0
        mov     bl,NE_CR
        call    nic_out
        mov     al,48h                  ; bytes, not words
        mov     bl,NE_DCR
        call    nic_out
        xor     al,al
        mov     bl,NE_RBCR0
        call    nic_out
        mov     bl,NE_RBCR1
        call    nic_out
        mov     al,20h                  ; receive nothing
        mov     bl,NE_RCR
        call    nic_out
        mov     al,02h                  ; and send to nobody, yet
        mov     bl,NE_TCR
        call    nic_out
        mov     al,46h
        mov     bl,NE_PSTART
        call    nic_out
        mov     bl,NE_BNRY
        call    nic_out
        mov     al,80h
        mov     bl,NE_PSTOP
        call    nic_out
        mov     al,0FFh
        mov     bl,NE_ISR
        call    nic_out
        mov     al,ISR_PTX OR ISR_TXE
        mov     bl,NE_IMR
        call    nic_out
        mov     al,22h                  ; started
        mov     bl,NE_CR
        call    nic_out
        xor     al,al                   ; a transmitter that transmits
        mov     bl,NE_TCR
        call    nic_out
        ret

;--- nic_send - sixty bytes into the card's memory, and the order to send
;--- them.  A broadcast from nobody, of a type nothing answers.
nic_send:
        mov     al,PKTLEN
        mov     bl,NE_RBCR0
        call    nic_out
        xor     al,al
        mov     bl,NE_RBCR1
        call    nic_out
        mov     bl,NE_RSAR0
        call    nic_out
        mov     al,TXPAGE
        mov     bl,NE_RSAR1
        call    nic_out
        mov     al,12h                  ; write, started
        mov     bl,NE_CR
        call    nic_out
        mov     dx,[base]
        add     dx,NE_DATA
        mov     si,OFFSET packet
        mov     cx,PKTLEN
        cld
nes_byte:
        lodsb
        out     dx,al
        loop    nes_byte
        mov     cx,8000h
nes_wait:
        mov     dx,[base]
        add     dx,NE_ISR
        in      al,dx
        test    al,ISR_RDC
        jnz     nes_done
        loop    nes_wait
nes_done:
        mov     al,ISR_RDC
        out     dx,al

        mov     al,TXPAGE
        mov     bl,NE_TPSR
        call    nic_out
        mov     al,PKTLEN
        mov     bl,NE_TBCR0
        call    nic_out
        xor     al,al
        mov     bl,NE_TBCR1
        call    nic_out
        mov     al,26h                  ; send, started
        mov     bl,NE_CR
        call    nic_out
        ret

nic_out:                                 ; AL to register BL
        push    dx
        mov     dx,[base]
        add     dl,bl
        adc     dh,0
        out     dx,al
        pop     dx
        ret

;=====================================================================
; cfgdword - DI = a register of [pcidev] -> ECX, by the PCI BIOS
;=====================================================================
cfgdword:
        push    ax
        push    bx
        mov     bx,[pcidev]
        mov     ax,0B10Ah
        int     1Ah
        pop     bx
        pop     ax
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
put8:                                   ; EAX in hex
        push    eax
        shr     eax,16
        call    put4
        pop     eax
put4:                                   ; AX
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:                                   ; AL
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
oldvec      dd  0
pciid       dd  0
pcidev      dw  0
base        dw  0
elcr        dw  0
nint        dw  0
resptr      dw  0
irq         db  0
vector      db  0
level       db  0
dismiss     db  1
lastisr     db  0
mask1       db  0
mask2       db  0
fails       db  0
results     db  16 dup('$')

packet      db  0FFh,0FFh,0FFh,0FFh,0FFh,0FFh          ; to everybody
            db  02h,00h,00h,50h,4Dh,44h                 ; from nobody
            db  88h,0B5h                                ; an experiment
            db  'PM-DOS NICIRQ '
            db  (PKTLEN - 28) dup(0)

msg_hi      db  13,10,'NICIRQ - a PCI card''s interrupt, in a DOS box'
            db  13,10,13,10,'$'
msg_t1      db  ' 1 the PCI BIOS answers INT 1Ah          $'
msg_t2      db  ' 2 a network card, an 8029     $'
msg_t3      db  ' 3 ports and a line             $'
msg_t4      db  ' 4 edge/level register              $'
msg_t5      db  ' 5 a packet sent, interrupts        $'
msg_t6      db  ' 6 not dismissed, interrupts        $'
msg_t7      db  ' 7 and the clock still runs              $'
msg_level   db  ': level',13,10,'$'
msg_edge    db  ': edge',13,10,'$'
msg_nobios  db  'no PCI BIOS (skipped)',13,10,'$'
msg_nocard  db  ' no 8029 (skipped)',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_pass    db  'NICIRQ: all checks passed',13,10,'$'
msg_fail    db  'NICIRQ: FAILURES',13,10,'$'

_NI     ENDS
        END     start
