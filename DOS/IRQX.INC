;=====================================================================
; IRQX.INC - an interrupt for a resident driver, with no program in a
;            box to take it
;
; A DOS box is given a card's interrupt the moment it can take one:
; when the interrupt arrives while the box's code is running, and
; otherwise on the way back into it (IO\VPIC.INC).  That left the times
; when no box code is running at all - and they are most of the time:
; the native shell at its prompt, a native program, and a box waiting
; inside a DOS call for a key.  A resident driver - a packet driver, a
; serial port's, a TSR on the timer - was left through all of them
; with its line shut, waiting for the next program that ran in a box.
;
; So the DOS side makes an EXCURSION, at the moments it is idle enough
; to.  The handler runs in real mode as a simulated interrupt
; (dpmi_simint, which is how INT 28h already gets out at the same
; moments): on the stack of the box that is waiting if there is one,
; as the interrupt would have found it, and on the scratch real-mode
; stack if there is not.  IOS_BOXIRQ hands the interrupts out, most
; urgent first, each in service from the moment it is handed out, and
; the handler's own EOI ends it as it would in a box.
;
; THE MOMENTS ARE TWO: dos_idle, which every wait for a key goes
; through, and the end of every DOS call a native program makes.  A
; native program that makes no calls at all - computing, or spinning on
; a flag of its own - holds an interrupt off until it does.
;
; InDOS IS 1 AT BOTH, because a DOS call is open at both, so a handler
; that looks before it calls DOS will not.  One that calls anyway is
; served, except for the four calls that start or end a program, which
; fail: there is no program here to end, and a program started from
; inside an interrupt handler is not one anybody meant to start.  A
; handler that faults, or that a ^C or an Abort would end, ends the
; EXCURSION instead (irx_lost): what it was given is taken back out of
; service and the machine carries on.
;
; Included by DOSMAIN.ASM, after DPMI.INC.
;=====================================================================

IRX_MAX         EQU     8               ; interrupts one excursion delivers

;=====================================================================
; irx_try - deliver whatever is owed, if anything is and this is a
;           moment it can be.  CF clear if a handler ran.  Every
;           register preserved.
;
; Not inside another excursion of any kind: a simulated interrupt, a
; driver's call or this one each have vdm_ctx for the duration.
;=====================================================================
irx_try:
        cmp     byte ptr ds:[irx_busy],0
        jne     irx_no
        cmp     dword ptr ds:[dpmi_rmact],0
        jne     irx_no
        cmp     byte ptr ds:[vdm_inexc],0
        jne     irx_no
        cmp     dword ptr [dl_depth],0
        jne     irx_no
        cmp     word ptr ds:[xms_cbseg],0
        je      irx_no
        pushad
        mov     al,BXI_TAKE
        call    dword ptr ds:[IOS_BOXIRQ]
        jc      irx_none

        mov     byte ptr ds:[irx_busy],1
        mov     byte ptr ds:[irx_lostf],0
        mov     dword ptr ds:[irx_n],0
        movzx   edx,byte ptr ds:[DOS_INDOS]     ; what irx_lost puts back
        mov     dword ptr ds:[irx_indos],edx
        mov     dl,byte ptr [crit_busy]
        mov     byte ptr ds:[irx_crit],dl
        mov     dl,byte ptr ds:[DOS_ERRFLG]
        mov     byte ptr ds:[irx_errflg],dl

        ; NO BOX, AND vdm_ctx IS WHATEVER THE LAST ONE LEFT - possibly
        ; nothing at all - so the vectors a box has the kernel answer go
        ; into it first.  Without them an INT 13h from a handler would
        ; go to the BIOS behind the kernel's back.
        cmp     dword ptr ds:[vdm_busy],0
        jne     irx_one
        call    vdm_traps

irx_one:
        mov     byte ptr ds:[dpmi_rmint],al
        call    irx_regs
        mov     dword ptr ds:[dpmi_rmcs],OFFSET irx_rmcs
        mov     word ptr ds:[dpmi_rmwords],0
        mov     dword ptr ds:[dpmi_rmflags],0
        mov     dword ptr ds:[irx_esp],esp
        call    dpmi_simint
        jnc     irx_back
        mov     byte ptr ds:[irx_lostf],1       ; it never started, so the
                                                ; interrupt is not ended either
irx_back:
        inc     dword ptr ds:[irx_n]
        cmp     byte ptr ds:[irx_lostf],0
        jne     irx_end
        cmp     dword ptr ds:[irx_n],IRX_MAX
        jae     irx_end
        mov     al,BXI_TAKE
        call    dword ptr ds:[IOS_BOXIRQ]
        jnc     irx_one
irx_end:
        mov     dl,byte ptr ds:[irx_lostf]
        mov     al,BXI_END
        call    dword ptr ds:[IOS_BOXIRQ]
        mov     byte ptr ds:[irx_busy],0
        popad
        clc
        ret
irx_none:
        popad
irx_no:
        stc
        ret

;---------------------------------------------------------------------
; irx_regs - the registers the handler starts with, into irx_rmcs.
;
; A box that is waiting: every register it had when it made the call,
; and its stack, as dos_int28 hands an idle hook them - an interrupt on
; a real DOS finds whatever was live.  No box: nothing, and an SS:SP of
; zero, which dpmi_simint takes as "the scratch stack".
;---------------------------------------------------------------------
irx_regs:
        pushad
        mov     edi,OFFSET irx_rmcs
        xor     eax,eax
        mov     ecx,(RMCS_LEN + 3) / 4
        cld
        rep     stosd
        cmp     dword ptr ds:[vdm_busy],0
        je      irr_out
        mov     esi,OFFSET vdm_ctx
        mov     edi,OFFSET irx_rmcs
        mov     eax,[esi+V86C_EDI]
        mov     [edi+RMCS_EDI],eax
        mov     eax,[esi+V86C_ESI]
        mov     [edi+RMCS_ESI],eax
        mov     eax,[esi+V86C_EBP]
        mov     [edi+RMCS_EBP],eax
        mov     eax,[esi+V86C_EBX]
        mov     [edi+RMCS_EBX],eax
        mov     eax,[esi+V86C_EDX]
        mov     [edi+RMCS_EDX],eax
        mov     eax,[esi+V86C_ECX]
        mov     [edi+RMCS_ECX],eax
        mov     eax,[esi+V86C_EAX]
        mov     [edi+RMCS_EAX],eax
        mov     eax,[esi+V86C_ES]
        mov     [edi+RMCS_ES],ax
        mov     eax,[esi+V86C_DS]
        mov     [edi+RMCS_DS],ax
        mov     eax,[esi+V86C_FS]
        mov     [edi+RMCS_FS],ax
        mov     eax,[esi+V86C_GS]
        mov     [edi+RMCS_GS],ax
        mov     eax,[esi+V86C_SS]
        mov     [edi+RMCS_SS],ax
        mov     eax,[esi+V86C_ESP]
        mov     [edi+RMCS_SP],ax
irr_out:
        popad
        ret

;=====================================================================
; irx_dos - an INT 21h made inside an excursion, from vdm_dos with AL =
;           the function.  The four that start or end a program fail;
;           everything else goes on to be served.
;=====================================================================
irx_dos:
        cmp     al,4Ch
        je      irx_refuse
        cmp     al,00h
        je      irx_refuse
        cmp     al,31h
        je      irx_refuse
        cmp     al,4Bh
        je      irx_refuse
        jmp     vdm_dos_go
irx_refuse:
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_FUNC
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

;=====================================================================
; irx_lost - a handler run by an excursion has faulted, or something
;            would end it.  There is nothing to end: the kernel stack
;            goes back to irx_try, the context the simulated interrupt
;            parked comes back, and the excursion stops there.  Reached
;            by a jump from vdm_abend and vdm_killprog.
;=====================================================================
irx_lost:
        cli
        mov     esp,dword ptr ds:[irx_esp]
        mov     eax,dword ptr ds:[irx_indos]
        mov     byte ptr ds:[DOS_INDOS],al
        sti
        mov     dword ptr ds:[dpmi_rmact],0
        mov     eax,dword ptr ds:[dpmi_rmesp0]
        mov     dword ptr ds:[TSS_BASE + TSS_ESP0],eax
        mov     esi,OFFSET dpmi_savectx
        mov     edi,OFFSET vdm_ctx
        mov     ecx,V86C_SIZE / 4
        cld
        rep     movsd
        mov     al,byte ptr ds:[irx_crit]
        mov     byte ptr [crit_busy],al
        mov     al,byte ptr ds:[irx_errflg]
        mov     byte ptr ds:[DOS_ERRFLG],al
        mov     byte ptr ds:[irx_lostf],1
        inc     dword ptr ds:[irx_nlost]
        jmp     irx_back

;=====================================================================
; State
;=====================================================================
irx_esp         dd      0               ; irx_try's stack, for irx_lost
irx_n           dd      0               ; handlers run by this excursion
irx_nlost       dd      0               ; excursions abandoned, ever
irx_indos       dd      0
irx_busy        db      0               ; an excursion is running
irx_lostf       db      0               ; ...and has been abandoned
irx_crit        db      0
irx_errflg      db      0
irx_rmcs        db      (RMCS_LEN + 3) / 4 * 4 dup(0)
