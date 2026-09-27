;=====================================================================
; IRQ.INC - whose interrupt is it
;
; Until now a hardware interrupt had one owner or none.  The timer and
; the keyboard were the kernel's, the mouse's line became the mouse
; driver's when one was found, and everything else arrived at a RET.
; That is an ISA answer, and it is wrong for PCI twice over:
;
;   A PCI LINE IS SHARED.  Four interrupt pins are routed to whatever
;   IRQs the BIOS had free, so two cards on one line is the ordinary
;   case and not a conflict.  A line therefore has a CHAIN of handlers
;   (irq_hook), every one of them is called every time, and each says
;   whether the interrupt was its own device's.  All of them, not the
;   first to claim it: two devices on a line can both be asking.
;
;   A PCI LINE IS LEVEL-TRIGGERED.  It stays asserted until the device
;   has been dealt with, so an interrupt nobody deals with does not go
;   away - it comes back the instant it is acknowledged, for ever.
;   irq_shared counts the ones nobody took and shuts the line when
;   there is no other way to get the machine back.
;
; AND A DOS BOX IS THE LAST THING ON EVERY CHAIN.  An interrupt no
; handler here claims is offered to the box (vpc_raise, VPIC.INC),
; which is how a 16-bit driver gets the interrupt of the card it found.
;
; ONE PLACE WRITES THE MASK REGISTERS, and it is irq_sync.  What the
; 8259s are told is worked out from what the kernel wants, what a box
; has unmasked and what a box still owes an answer on, every time any
; of them changes - and it is written to GV_PICMASK as well, because
; every trip into the BIOS ends with pic_to_pm putting that back.
;
; Included by IOPM.ASM, after EXCEPT.INC and before VPIC.INC.
;=====================================================================

ELCR_MASTER     EQU     4D0h                    ; edge or level, a bit a line
ELCR_SLAVE      EQU     4D1h
ELCR_NEVER      EQU     2107h                   ; 0, 1, 2, 8 and 13 are never
                                                ; level: a register that says
                                                ; so is not that register
IRQ_STRAYMAX    EQU     256                     ; unclaimed, inside a second

;=====================================================================
; irq_init - once, from pm_entry, after pci_init.
;
; The edge/level register is read and not written: which lines are
; level is the BIOS's decision, made when it routed the PCI pins, and
; nothing here has a reason to change it.  It is believed only on a
; machine with a PCI bus - an ISA machine has nothing at 4D0h, and what
; comes back from a port nothing decodes is not a setting.
;=====================================================================
irq_init:
        pushad
        mov     dword ptr [irq_elcr],0
        cmp     byte ptr [pci_ok],0
        je      iqi_none
        mov     dx,ELCR_SLAVE
        in      al,dx
        mov     ah,al
        mov     dx,ELCR_MASTER
        in      al,dx
        movzx   eax,ax
        test    eax,ELCR_NEVER
        jnz     iqi_none
        mov     dword ptr [irq_elcr],eax
iqi_none:
        call    vpc_init
        call    irq_sync
IFDEF IRQ_SELFTEST
        call    iqx_init
ENDIF
        popad
        ret

;=====================================================================
; irq_sync - tell the 8259s what is wanted.  Everything preserved.
;
; A line is open if the kernel wants it or a box has unmasked it -
; unless a box has been handed that interrupt and has not yet said it
; is finished with it.  That last clause is the whole of level-
; triggered support: the line is still asserted while the box's
; handler is getting round to the device, and opening it again before
; the handler's EOI would deliver the same interrupt a second time.
;=====================================================================
irq_sync:
        pushfd
        cli
        push    eax
        push    ebx
        mov     eax,dword ptr [vpc_imr]
        not     eax
        and     eax,VP_GUEST
        or      eax,dword ptr [irq_kwant]
        mov     ebx,dword ptr [vpc_pend]
        or      ebx,dword ptr [vpc_isr]
        and     ebx,VP_GUEST
        or      ebx,dword ptr [irq_dead]
        not     ebx
        and     eax,ebx
        test    eax,0FF00h              ; anything behind the cascade needs
        jz      iqs_nocas               ; the cascade
        or      eax,4
iqs_nocas:
        mov     dword ptr [irq_open],eax
        not     eax
        mov     word ptr ds:[GV_PICMASK],ax
        out     PIC1_DATA,al
        mov     al,ah
        out     PIC2_DATA,al
        pop     ebx
        pop     eax
        popfd
        ret

;---------------------------------------------------------------------
; irq_want / irq_unwant - ECX = a line the kernel itself does or does
;                         not need open.  Everything preserved.
;
; Asking for a line again is a second chance for one that was shut for
; arriving unclaimed: whoever is asking has presumably brought the
; handler it was missing.
;---------------------------------------------------------------------
irq_want:
        pushfd
        cli
        push    eax
        mov     eax,1
        shl     eax,cl
        or      dword ptr [irq_kwant],eax
        not     eax
        and     dword ptr [irq_dead],eax
        mov     dword ptr [irq_strays + ecx*4],0
        pop     eax
        call    irq_sync
        popfd
        ret

irq_unwant:
        pushfd
        cli
        push    eax
        mov     eax,1
        shl     eax,cl
        not     eax
        and     dword ptr [irq_kwant],eax
        pop     eax
        call    irq_sync
        popfd
        ret

;=====================================================================
; irq_hook - IOS_IRQHOOK.  AL = the line, EDI = the handler.  CF if the
;            line is one the kernel keeps to itself, or is full.
;
; The handler is called from irq_shared with EBX = the line, DS and ES
; the kernel's and interrupts off, and may change any register.  It
; returns CF CLEAR if its device was asking and CF SET if it was not -
; which it finds out by asking the device, never by assuming: on a
; shared line most calls are for somebody else.  It does not send the
; EOI.  irq_common does that.
;=====================================================================
irq_hook:
        pushfd
        cli
        push    ebx
        push    ecx
        call    iq_line
        jc      iqh_no
        push    ecx
        mov     ecx,IRQ_CHAIN
iqh_slot:
        cmp     dword ptr [ebx],0
        je      iqh_free
        add     ebx,4
        loop    iqh_slot
        pop     ecx
        jmp     short iqh_no
iqh_free:
        pop     ecx
        mov     dword ptr [ebx],edi
        call    irq_want
        pop     ecx
        pop     ebx
        popfd
        clc
        ret
iqh_no:
        pop     ecx
        pop     ebx
        popfd
        stc
        ret

;=====================================================================
; irq_unhook - IOS_IRQUNHOOK.  The same registers.  CF if that handler
;              was not on that line.  The line is shut again when the
;              last one leaves it, unless a box has it open.
;=====================================================================
irq_unhook:
        pushfd
        cli
        push    eax
        push    ebx
        push    ecx
        push    edx
        call    iq_line
        jc      iqu_no
        xor     edx,edx                 ; handlers still there, and
        xor     eax,eax                 ; whether ours was one
        push    ecx
        mov     ecx,IRQ_CHAIN
iqu_slot:
        cmp     dword ptr [ebx],0
        je      iqu_next
        cmp     dword ptr [ebx],edi
        jne     iqu_other
        or      eax,eax
        jnz     iqu_other               ; hooked twice: one at a time
        mov     dword ptr [ebx],0
        inc     eax
        jmp     short iqu_next
iqu_other:
        inc     edx
iqu_next:
        add     ebx,4
        loop    iqu_slot
        pop     ecx
        or      eax,eax
        jz      iqu_no
        or      edx,edx
        jnz     iqu_ok
        call    irq_unwant
iqu_ok:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        clc
        ret
iqu_no:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        popfd
        stc
        ret

;---------------------------------------------------------------------
; iq_line - AL = a line -> ECX = the line, EBX -> its handlers.  CF if
;           it has an owner that does not share: the timer, the
;           keyboard, the cascade, and the mouse's once a mouse is
;           found.
;---------------------------------------------------------------------
iq_line:
        movzx   ecx,al
        cmp     ecx,16
        jae     iql_no
        cmp     dword ptr [irq_vectors + ecx*4],OFFSET irq_shared
        jne     iql_no
        imul    ebx,ecx,IRQ_CHAIN * 4
        add     ebx,OFFSET irq_handlers
        clc
        ret
iql_no:
        stc
        ret

;=====================================================================
; irq_shared - every line without an owner of its own.  EBX = the line,
;              from irq_common, which sends the EOI when this returns.
;=====================================================================
irq_shared:
        ; AN INTERRUPT THAT WAS NOT ONE.  A line that drops before the
        ; processor has acknowledged it leaves the 8259 with nothing to
        ; report, and it reports the lowest priority it has: 7 on the
        ; first controller, 15 on the second.  The in-service register
        ; is what tells them from the real thing, and they are not
        ; handed to anybody - a driver's handler is one thing, a DOS
        ; program's printer interrupt arriving from nowhere is another.
        cmp     ebx,7
        je      iqr_sp1
        cmp     ebx,15
        je      iqr_sp2
iqr_real:
        inc     dword ptr [irq_count + ebx*4]
        imul    esi,ebx,IRQ_CHAIN * 4
        add     esi,OFFSET irq_handlers
        mov     ecx,IRQ_CHAIN
        xor     edi,edi                 ; how many said it was theirs
iqr_one:
        mov     eax,dword ptr [esi]
        or      eax,eax
        jz      iqr_next
        push    ebx
        push    ecx
        push    esi
        push    edi
        call    eax
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        jc      iqr_next
        inc     edi
iqr_next:
        add     esi,4
        loop    iqr_one
        or      edi,edi
        jz      iqr_nobody
        mov     dword ptr [irq_strays + ebx*4],0
        ret

iqr_nobody:
        call    vpc_raise               ; a box's, if a box has it open
        jc      iqr_stray
        mov     dword ptr [irq_strays + ebx*4],0
        ret

        ; NOBODY'S.  On an edge-triggered line that is the end of it.
        ; On a level-triggered one the device is still asking and will
        ; be asking again before the IRETD has finished, so a run of
        ; them inside one second is a line that has to be shut: the
        ; alternative is a machine that does nothing else.
iqr_stray:
        inc     dword ptr [irq_lost + ebx*4]
        mov     eax,dword ptr ds:[BDA_TICKS]
        sub     eax,dword ptr [irq_straywhen + ebx*4]
        cmp     eax,18
        jbe     iqr_run
        mov     eax,dword ptr ds:[BDA_TICKS]
        mov     dword ptr [irq_straywhen + ebx*4],eax
        mov     dword ptr [irq_strays + ebx*4],0
iqr_run:
        inc     dword ptr [irq_strays + ebx*4]
        cmp     dword ptr [irq_strays + ebx*4],IRQ_STRAYMAX
        jb      iqr_out
        bts     dword ptr [irq_dead],ebx
        call    irq_sync
iqr_out:
        ret

iqr_sp1:
        mov     dx,PIC1_CMD
        jmp     short iqr_sp
iqr_sp2:
        mov     dx,PIC2_CMD
iqr_sp:
        mov     al,0Bh                  ; OCW3: the next read is in-service
        out     dx,al
        in      al,dx
        mov     ah,al
        mov     al,0Ah                  ; ...and the one after is requests
        out     dx,al                   ; again, as everything else assumes
        test    ah,80h
        jnz     iqr_real
        inc     dword ptr [irq_lost + ebx*4]
        ret

;=====================================================================
; irq_stat - BUS_IRQSTAT.  DL = a line -> EAX = IRQF_* flags, EBX =
;            interrupts taken on it, ECX = how many went to a DOS box,
;            EDX = how many nobody claimed.  CF if there is no such
;            line.
;=====================================================================
irq_stat:
        push    esi
        movzx   esi,dl
        cmp     esi,16
        jae     iqt_no
        pushfd
        cli
        xor     eax,eax
        cmp     dword ptr [irq_vectors + esi*4],OFFSET irq_shared
        je      iqt_shared
        or      eax,IRQF_KERNEL
iqt_shared:
        imul    ebx,esi,IRQ_CHAIN * 4
        add     ebx,OFFSET irq_handlers
        mov     ecx,IRQ_CHAIN
iqt_slot:
        cmp     dword ptr [ebx],0
        je      iqt_empty
        or      eax,IRQF_HOOKED
iqt_empty:
        add     ebx,4
        loop    iqt_slot
        mov     ebx,dword ptr [vpc_imr]
        not     ebx
        and     ebx,VP_GUEST
        bt      ebx,esi
        jnc     iqt_nowant
        or      eax,IRQF_BOXWANT
iqt_nowant:
        bt      dword ptr [vpc_pend],esi
        jnc     iqt_nopend
        or      eax,IRQF_PENDING
iqt_nopend:
        bt      dword ptr [vpc_isr],esi
        jnc     iqt_noserv
        or      eax,IRQF_INSERV
iqt_noserv:
        bt      dword ptr [irq_elcr],esi
        jnc     iqt_edge
        or      eax,IRQF_LEVEL
iqt_edge:
        bt      dword ptr [irq_open],esi
        jnc     iqt_shut
        or      eax,IRQF_OPEN
iqt_shut:
        bt      dword ptr [irq_dead],esi
        jnc     iqt_alive
        or      eax,IRQF_DEAD
iqt_alive:
        mov     ebx,dword ptr [irq_count + esi*4]
        mov     ecx,dword ptr [vpc_seen + esi*4]
        mov     edx,dword ptr [irq_lost + esi*4]
        popfd
        pop     esi
        clc
        ret
iqt_no:
        pop     esi
        stc
        ret

;=====================================================================
; State
;=====================================================================
irq_kwant       dd      0003h                   ; what the kernel wants open:
                                                ; the timer and the keyboard,
                                                ; until somebody asks for more
irq_open        dd      0003h                   ; what the 8259s were last told
irq_dead        dd      0                       ; shut for arriving unclaimed
irq_elcr        dd      0                       ; level-triggered, as the BIOS
                                                ; routed them
irq_handlers    dd      16 * IRQ_CHAIN dup(0)
irq_count       dd      16 dup(0)               ; taken, a line
irq_lost        dd      16 dup(0)               ; ...and claimed by nobody
irq_strays      dd      16 dup(0)               ; the run of those under way
irq_straywhen   dd      16 dup(0)               ; and the tick it began on

IFDEF IRQ_SELFTEST
;=====================================================================
; THE CHAIN, TESTED.  Nothing in the kernel hooks a shared line yet -
; the mouse has a line to itself - so a build made with -dIRQ_SELFTEST
; (BUILD irqtest) brings two handlers of its own and puts them both on
; line 8, the real-time clock's, which it sets interrupting sixty-four
; times a second.  One of them never claims anything; the other asks
; the clock whether it interrupted and claims what it did.  That is a
; shared line with two devices on it, one of them silent, and
; IRQTEST.EXE reads the counts back through BUS_SELFTEST.
;
; NOT FOR A KERNEL ANYBODY USES: with this in, the clock's interrupt
; is the kernel's and no DOS box will ever see it.
;=====================================================================
IQX_RTCA        EQU     0Ah
IQX_RTCB        EQU     0Bh
IQX_RTCC        EQU     0Ch

iqx_init:
        pushad
        mov     dword ptr [iqx_flags],0

        xor     al,al                   ; the timer's line is not shared
        mov     edi,OFFSET iqx_quiet
        call    irq_hook
        jnc     iqx_i1
        or      dword ptr [iqx_flags],1
iqx_i1:
        mov     ecx,IRQ_CHAIN           ; a line holds IRQ_CHAIN and no more
iqx_fill:
        mov     al,9
        mov     edi,OFFSET iqx_quiet
        call    irq_hook
        jc      iqx_out
        loop    iqx_fill
        mov     al,9
        mov     edi,OFFSET iqx_quiet
        call    irq_hook
        jnc     iqx_i2
        or      dword ptr [iqx_flags],2
iqx_i2:
        mov     ecx,IRQ_CHAIN           ; and gives them all back
iqx_empty:
        mov     al,9
        mov     edi,OFFSET iqx_quiet
        call    irq_unhook
        jc      iqx_i3
        loop    iqx_empty
        mov     al,9                    ; ...after which there is nothing
        mov     edi,OFFSET iqx_quiet    ; there to take off
        call    irq_unhook
        jnc     iqx_i3
        bt      dword ptr [irq_kwant],9 ; and the line is shut again
        jc      iqx_i3
        or      dword ptr [iqx_flags],4
iqx_i3:
        mov     al,8
        mov     edi,OFFSET iqx_quiet
        call    irq_hook
        jc      iqx_out
        mov     al,8
        mov     edi,OFFSET iqx_rtc
        call    irq_hook
        jc      iqx_out
        or      dword ptr [iqx_flags],8

        pushfd
        cli
        mov     al,IQX_RTCA
        out     70h,al
        in      al,71h
        and     al,0F0h
        or      al,0Ah                  ; 64 a second
        mov     ah,al
        mov     al,IQX_RTCA
        out     70h,al
        mov     al,ah
        out     71h,al
        mov     al,IQX_RTCB
        out     70h,al
        in      al,71h
        or      al,40h                  ; and interrupt at that rate
        mov     ah,al
        mov     al,IQX_RTCB
        out     70h,al
        mov     al,ah
        out     71h,al
        mov     al,IQX_RTCC
        out     70h,al
        in      al,71h
        popfd
iqx_out:
        popad
        ret

;--- a device that shares the line and never has anything to say
iqx_quiet:
        inc     dword ptr [iqx_nquiet]
        stc
        ret

;--- and the clock.  Register C says whether it interrupted, and
;--- reading it is what makes it let go of the line.
iqx_rtc:
        mov     al,IQX_RTCC
        out     70h,al
        in      al,71h
        test    al,80h
        jz      iqx_notme
        inc     dword ptr [iqx_nrtc]
        clc
        ret
iqx_notme:
        stc
        ret

;--- BUS_SELFTEST - EAX = 'IRQT', EBX = calls to the silent handler,
;--- ECX = interrupts the clock's handler claimed, EDX = what iqx_init
;--- found: bit 0 the timer's line refused a handler, bit 1 a full line
;--- refused one more, bit 2 an empty one had nothing to take off and
;--- was shut, bit 3 both handlers are on line 8.
iqx_report:
        mov     eax,54515249h
        mov     ebx,dword ptr [iqx_nquiet]
        mov     ecx,dword ptr [iqx_nrtc]
        mov     edx,dword ptr [iqx_flags]
        clc
        ret

iqx_nquiet      dd      0
iqx_nrtc        dd      0
iqx_flags       dd      0
ENDIF
