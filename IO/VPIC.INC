;=====================================================================
; VPIC.INC - the interrupt controller a DOS box believes in
;
; A 16-bit driver does four things to get its card's interrupt, and
; all four used to go to the machine's own 8259s: it writes its handler
; into the vector table, clears its line's bit in the mask register,
; waits, and ends every interrupt by writing an EOI to the command
; port.  The kernel had the real vectors somewhere else (IRQ_BASE), so
; the interrupt arrived at irq_none and was thrown away - and the mask
; the driver had written was put back to the kernel's at the next trip
; into the BIOS.  The driver found its card and never heard from it.
;
; So the four ports are the monitor's now, like the three of the A20
; gate, and what is behind them is a pair of 8259s that exist only
; here:
;
;   THE MASK REGISTER is the box's own.  Clearing a bit for one of the
;   lines a box may have (VP_GUEST) opens the real line; the rest of it
;   is remembered and handed back when asked for, and changes nothing -
;   a program that masks the timer has masked it for itself.
;
;   AN INTERRUPT ON AN OPEN LINE IS OWED TO THE BOX (vpc_raise) and the
;   real line is SHUT until the box has taken it and sent the EOI.
;   That is what makes a level-triggered line work: a PCI device goes
;   on asserting its line until its driver has dealt with it, the
;   kernel's own EOI is sent long before the driver has run, and an
;   open line would deliver the same interrupt again and again without
;   the driver ever reaching its first instruction.  Edge-triggered
;   lines are shut the same way and lose nothing by it - an 8259 goes
;   on latching edges behind a mask.
;
;   IT IS DELIVERED WHERE A KEYSTROKE IS - the IRQ that interrupted the
;   guest, a resume out of the monitor, an entry to v86_run - through
;   the guest's own table at the vector the box expects, 08h or 70h and
;   up, and only when the guest's interrupt flag is set and nothing of
;   a higher priority is still being served.
;
;   THE EOI ENDS IT, opens the real line again, and lets the next one
;   through.
;
; A DPMI CLIENT IS A BOX THAT IS SOMEWHERE ELSE.  While it is running
; its protected-mode code, an interrupt it has a protected-mode handler
; for (INT 31h 0205h) goes to that handler, on the client's own stack
; (vpc_pmchk).  One it has only a real-mode handler for waits until the
; client is next in real mode.
;
; WHAT IS NOT HERE.  The timer: IRQ 0 is not delivered to a box, as it
; never was.  An interrupt for a resident driver while NO box is
; running - the native shell at its prompt - waits for the next program
; that runs in one.  And a box's reprogramming of the controller is
; followed only as far as the vector bases; the modes nobody uses on a
; PC (polling, special mask, rotation) are accepted and ignored.
;
; Included by IOPM.ASM, after IRQ.INC and before VM86.INC.
;=====================================================================

VPC_IMRBOOT     EQU     3EB8h                   ; what a BIOS leaves, and what
                                                ; pic_to_rm gives it: master
                                                ; B8h, slave 3Eh

;=====================================================================
; vpc_init - once, from irq_init.  The sixteen vectors as the BIOS left
;            them, which is how "the box has hooked it" is told from
;            "that is still the ROM's dummy".
;=====================================================================
vpc_init:
        pushad
        cld
        mov     esi,08h * 4
        mov     edi,OFFSET vpc_boot
        mov     ecx,8
        rep     movsd
        mov     esi,70h * 4
        mov     ecx,8
        rep     movsd
        popad
        ret

;=====================================================================
; vpc_setup - the DPMI host's three addresses, from IOS_BUSINIT.
;             EBX -> its table of protected-mode vectors (offset, then
;             selector, eight bytes a vector), ECX -> the dword that is
;             non-zero while a client is in protected mode, EDX -> the
;             byte that is non-zero if that client is a 32-bit one.
;=====================================================================
vpc_setup:
        mov     dword ptr [vpc_pmvec],ebx
        mov     dword ptr [vpc_pactive],ecx
        mov     dword ptr [vpc_pis32],edx
        ret

;=====================================================================
; vpc_boxreset - a program is starting in a box.  Nothing is in service
;                - whatever was, its handler is not coming back - and
;                the keyboard is unmasked, because a program that died
;                with it masked must not cost the next one its keys.
;                The rest of the mask stays: a resident driver's line
;                is still that driver's.
;=====================================================================
vpc_boxreset:
        and     dword ptr [vpc_imr],NOT 2
        mov     dword ptr [vpc_isr],0
        call    irq_sync
        ret

;=====================================================================
; vpc_raise - EBX = a line nobody in the kernel claimed.  CF clear if a
;             box has it open: it is owed to the box, and the line is
;             shut until the box has answered.
;=====================================================================
vpc_raise:
        push    eax
        mov     eax,dword ptr [vpc_imr]
        not     eax
        and     eax,VP_GUEST
        bt      eax,ebx
        jnc     vpr_no
        bts     dword ptr [vpc_pend],ebx
        call    irq_sync
        pop     eax
        clc
        ret
vpr_no:
        pop     eax
        stc
        ret

;=====================================================================
; vpc_boxwant - is a hardware interrupt owed to the guest, and can it
;               take one now?  CF clear and EAX = the vector to deliver
;               through its table; it is in service from here on, so a
;               caller that gets CF clear must deliver.  Interrupts
;               off; the caller has looked at the guest's own flag.
;               Only EAX is changed.
;=====================================================================
vpc_boxwant:
        cmp     dword ptr [vpc_pend],0
        je      vbw_no
        push    ebx
        push    edx
        xor     al,al
        call    vpc_pick
        jc      vbw_none
        mov     ebx,eax
        call    vpc_take
        mov     eax,edx
        pop     edx
        pop     ebx
        clc
        ret
vbw_none:
        pop     edx
        pop     ebx
vbw_no:
        stc
        ret

;---------------------------------------------------------------------
; vpc_pick - AL = 0 for a guest in real mode, 1 for a client in
;            protected mode.  -> EAX = the line to deliver, EDX = its
;            vector.  CF if there is nothing that can be.
;
; THE ORDER IS THE 8259's: 0, 1, then everything behind the cascade,
; then 3 to 7.  A line being served stops the search where it stands,
; because nothing at its priority or below can interrupt it - and for
; the second controller that line is the cascade itself, which stays
; in service until the handler has sent the FIRST controller its EOI.
;---------------------------------------------------------------------
vpc_pick:
        push    ebx
        push    ecx
        push    esi
        mov     byte ptr [vpc_mode],al
        mov     ecx,dword ptr [vpc_imr]
        not     ecx
        and     ecx,dword ptr [vpc_pend]
        and     ecx,VP_GUEST
        jz      vpk_no
        mov     esi,OFFSET vpc_order
vpk_one:
        movzx   ebx,byte ptr [esi]
        inc     esi
        cmp     bl,0FFh
        je      vpk_no
        bt      dword ptr [vpc_isr],ebx
        jc      vpk_no
        bt      ecx,ebx
        jnc     vpk_one

        mov     eax,ebx                 ; the vector the box knows it by
        movzx   edx,byte ptr [vpc_base]
        cmp     eax,8
        jb      vpk_vec
        sub     eax,8
        movzx   edx,byte ptr [vpc_base + 1]
vpk_vec:
        add     edx,eax

        call    vpc_pmhas
        jc      vpk_real
        ; A client with a protected-mode handler for it.  In protected
        ; mode that is where it goes.  In real mode it waits for the
        ; client to come back - unless the real-mode vector has been
        ; hooked as well, which is a handler written for both and is
        ; entitled to the half it is standing in.
        cmp     byte ptr [vpc_mode],0
        jne     vpk_got
        mov     eax,dword ptr ds:[edx*4]
        cmp     eax,dword ptr [vpc_boot + ebx*4]
        je      vpk_one
        jmp     short vpk_ivt
vpk_real:
        cmp     byte ptr [vpc_mode],0
        jne     vpk_one                 ; protected mode, and nothing there
vpk_ivt:
        cmp     dword ptr ds:[edx*4],0  ; a vector that points nowhere
        je      vpk_one
vpk_got:
        mov     eax,ebx
        pop     esi
        pop     ecx
        pop     ebx
        clc
        ret
vpk_no:
        pop     esi
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; vpc_pmhas - EDX = a vector.  CF clear and EAX -> its entry in the
;             host's table if a client is running and has a handler of
;             its own there.
;---------------------------------------------------------------------
vpc_pmhas:
        mov     eax,dword ptr [vpc_pactive]
        or      eax,eax
        jz      vph_no
        cmp     dword ptr [eax],0
        je      vph_no
        mov     eax,dword ptr [vpc_pmvec]
        or      eax,eax
        jz      vph_no
        lea     eax,[eax + edx*8]
        cmp     word ptr [eax+4],0
        je      vph_no
        clc
        ret
vph_no:
        stc
        ret

;---------------------------------------------------------------------
; vpc_take - EBX = a line being handed over: owed no longer, in service
;            from now, and the cascade with it if it is behind one.
;---------------------------------------------------------------------
vpc_take:
        btr     dword ptr [vpc_pend],ebx
        bts     dword ptr [vpc_isr],ebx
        cmp     ebx,8
        jb      vtk_master
        or      dword ptr [vpc_isr],4
vtk_master:
        inc     dword ptr [vpc_seen + ebx*4]
        ret

;=====================================================================
; vpc_pmchk - irq_common and pm3_done, on the way back to ring 3.
;             EBP -> the IRETD frame (EIP, CS, EFLAGS, ESP, SS) the
;             program will be resumed through.  If it is a DPMI client
;             that can take an interrupt and is owed one it has a
;             handler for, it is resumed in the handler instead.
;             Everything preserved.
;
; The handler gets what an interrupt gate would have given it: the
; flags and the address it interrupted, on its own stack, in the width
; the CLIENT is - which is what the specification says, and is not the
; same thing as the width of the handler's code segment.  An extender
; is a 32-bit client with 16-bit segments of its own, and its handlers
; in them end with an IRETD.  The default handler takes a frame off
; again by the same rule (dpmi_leave), and the two have to agree.  The specification promises
; a handler a stack of the host's; this host has the client's and no
; other, and a client that expects more is told so here rather than
; by a fault.
;=====================================================================
vpc_pmchk:
        cmp     dword ptr [vpc_pend],0
        je      vpm_ret
        test    dword ptr [ebp+8],EFL_VM
        jnz     vpm_ret
        test    byte ptr [ebp+4],4      ; its selectors are in the LDT
        jz      vpm_ret
        cmp     byte ptr [pm3_vif],0
        jne     vpm_try
vpm_ret:
        ret
vpm_try:
        pushad
        mov     al,1
        call    vpc_pick
        jc      vpm_out
        mov     dword ptr [vpc_dline],eax
        call    vpc_pmhas               ; it was there a moment ago
        jc      vpm_out
        mov     esi,eax                 ; ESI -> offset, selector

        mov     ax,word ptr [esi+4]
        call    vpc_desc                ; the handler's code segment
        jc      vpm_out
        xor     cl,cl
        mov     edi,dword ptr [vpc_pis32]
        cmp     byte ptr [edi],0
        je      vpm_wide
        mov     cl,40h                  ; bit 6: a 32-bit client
vpm_wide:
        mov     ax,word ptr [ebp+16]
        call    vpc_desc                ; and the stack it goes on
        jc      vpm_out
        mov     ch,byte ptr [edi+6]     ; bit 6: ESP, not SP
        mov     ax,word ptr [ebp+16]
        call    pm3_base                ; EBX = where that stack is
        jc      vpm_out

        mov     edx,dword ptr [ebp+12]
        test    ch,40h
        jnz     vpm_esp
        movzx   edx,dx
vpm_esp:
        test    cl,40h
        jz      vpm_16
        cmp     edx,12
        jb      vpm_out
        sub     edx,12
        mov     eax,dword ptr [ebp]
        mov     dword ptr [ebx+edx],eax
        movzx   eax,word ptr [ebp+4]
        mov     dword ptr [ebx+edx+4],eax
        mov     eax,dword ptr [ebp+8]
        mov     dword ptr [ebx+edx+8],eax
        mov     eax,dword ptr [esi]
        jmp     short vpm_go
vpm_16:
        cmp     edx,6
        jb      vpm_out
        sub     edx,6
        mov     eax,dword ptr [ebp]
        mov     word ptr [ebx+edx],ax
        mov     ax,word ptr [ebp+4]
        mov     word ptr [ebx+edx+2],ax
        mov     eax,dword ptr [ebp+8]
        mov     word ptr [ebx+edx+4],ax
        movzx   eax,word ptr [esi]
vpm_go:
        mov     dword ptr [ebp],eax
        movzx   eax,word ptr [esi+4]
        mov     dword ptr [ebp+4],eax
        test    ch,40h
        jnz     vpm_wesp
        mov     word ptr [ebp+12],dx
        jmp     short vpm_taken
vpm_wesp:
        mov     dword ptr [ebp+12],edx
vpm_taken:
        mov     byte ptr [pm3_vif],0    ; a handler starts with them off
        mov     ebx,dword ptr [vpc_dline]
        call    vpc_take
vpm_out:
        popad
        ret

;---------------------------------------------------------------------
; vpc_desc - AX = one of a client's selectors -> EDI -> its descriptor.
;            CF if it is not in the LDT, or is not present.
;---------------------------------------------------------------------
vpc_desc:
        test    al,4
        jz      vpd_no
        movzx   edi,ax
        shr     edi,3
        cmp     edi,DPMI_LDT_ENT
        jae     vpd_no
        shl     edi,3
        add     edi,DPMI_LDT
        test    byte ptr [edi+5],80h
        jz      vpd_no
        clc
        ret
vpd_no:
        stc
        ret

;=====================================================================
; vpc_in - DX = one of the four ports -> AL.  From vio_in.
;
; A data port is the mask.  A command port is whichever of the two
; registers the last OCW3 asked for, and the request register is the
; one place the real controller is consulted: what a box is owed is
; here, but a line that is asserted and masked is only there, and a
; program finding out which interrupt its card uses reads exactly
; that.
;=====================================================================
vpc_in:
        push    ebx
        push    ecx
        call    vpc_which               ; EBX = 0 or 1, CF set for data
        jnc     vpi_cmd
        mov     al,byte ptr [vpc_imr + ebx]
        jmp     short vpi_out
vpi_cmd:
        cmp     byte ptr [vpc_rsel + ebx],0
        jne     vpi_isr
        in      al,dx                   ; the real one's requests...
        mov     cl,al
        mov     eax,VP_GUEST
        or      ebx,ebx
        jz      vpi_lines
        mov     al,ah
vpi_lines:
        and     cl,al                   ; ...for the lines a box may have
        or      cl,byte ptr [vpc_pend + ebx]
        or      ebx,ebx
        jnz     vpi_req
        cmp     byte ptr [vpc_pend + 1],0
        je      vpi_req
        or      cl,4                    ; the second controller is asking
vpi_req:
        mov     al,cl
        jmp     short vpi_out
vpi_isr:
        mov     al,byte ptr [vpc_isr + ebx]
vpi_out:
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; vpc_out - DX = one of the four ports, AL = the byte.  From vio_out.
;=====================================================================
vpc_out:
        push    eax
        push    ebx
        push    ecx
        call    vpc_which
        jnc     vpo_cmd

;--- a data port: the rest of an initialisation, or the mask ---------
        cmp     byte ptr [vpc_icw + ebx],0
        je      vpo_mask
        cmp     byte ptr [vpc_icw + ebx],3
        jne     vpo_icwn
        and     al,0F8h                 ; ICW2: where its vectors start
        mov     byte ptr [vpc_base + ebx],al
vpo_icwn:
        dec     byte ptr [vpc_icw + ebx]
        cmp     byte ptr [vpc_icw + ebx],1
        jne     vpo_done
        cmp     byte ptr [vpc_icw4 + ebx],0
        jne     vpo_done                ; one more to come
        mov     byte ptr [vpc_icw + ebx],0
        jmp     vpo_done
vpo_mask:
        mov     byte ptr [vpc_imr + ebx],al
        call    irq_sync
        jmp     vpo_done

;--- a command port --------------------------------------------------
vpo_cmd:
        test    al,10h
        jnz     vpo_icw1
        test    al,08h
        jnz     vpo_ocw3
        mov     cl,al                   ; OCW2, and only the EOIs of it
        shr     cl,5
        cmp     cl,1                    ; non-specific
        je      vpo_eoi
        cmp     cl,5                    ; ...and rotate, which is not done
        je      vpo_eoi
        cmp     cl,3                    ; specific
        je      vpo_seoi
        cmp     cl,7
        je      vpo_seoi
        jmp     vpo_done

vpo_icw1:
        ; ICW2, ICW3 and perhaps ICW4 follow on the data port.  The
        ; mask is cleared and reads go back to the request register,
        ; which is what the part does.
        mov     byte ptr [vpc_icw + ebx],3
        and     al,1
        mov     byte ptr [vpc_icw4 + ebx],al
        mov     byte ptr [vpc_imr + ebx],0
        mov     byte ptr [vpc_rsel + ebx],0
        call    irq_sync
        jmp     vpo_done

vpo_ocw3:
        test    al,2
        jz      vpo_done
        and     al,1
        mov     byte ptr [vpc_rsel + ebx],al
        jmp     vpo_done

vpo_seoi:
        movzx   ecx,al
        and     ecx,7
        jmp     short vpo_end
vpo_eoi:
        movzx   eax,byte ptr [vpc_isr + ebx]
        bsf     ecx,eax                 ; the most urgent one in service
        jz      vpo_done                ; none: an EOI for nothing
vpo_end:
        mov     al,1
        shl     al,cl
        not     al
        and     byte ptr [vpc_isr + ebx],al
        call    irq_sync
vpo_done:
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; vpc_which - DX = one of the four -> EBX = 0 for the first controller
;             and 1 for the second, CF set if it is the data port.
;---------------------------------------------------------------------
vpc_which:
        xor     ebx,ebx
        cmp     dx,PIC2_CMD
        jb      vpw_first
        inc     ebx
vpw_first:
        test    dl,1
        jnz     vpw_data
        clc
        ret
vpw_data:
        stc
        ret

;=====================================================================
; State.  The three registers are a bit a line, the first controller
; in the low byte and the second in the next, so the byte a port reads
; is the byte at [register + controller].
;=====================================================================
vpc_imr         dd      VPC_IMRBOOT             ; masked, as the box has it
vpc_pend        dd      0                       ; owed to the box
vpc_isr         dd      0                       ; in the box's handler; bit 2
                                                ; is the cascade's own
vpc_base        db      08h, 70h                ; where the box's vectors are
vpc_rsel        db      0, 0                    ; a command port reads 0 =
                                                ; requests, 1 = in service
vpc_icw         db      0, 0                    ; initialisation words to come
vpc_icw4        db      0, 0                    ; ...and whether ICW4 is one
vpc_mode        db      0
                db      3 dup(0)
vpc_dline       dd      0
vpc_pmvec       dd      0                       ; the DPMI host's vectors
vpc_pactive     dd      0                       ; ...and its "a client is in
                                                ; protected mode"
vpc_pis32       dd      0                       ; ...and its "and it is 32-bit"
vpc_seen        dd      16 dup(0)               ; delivered, a line
vpc_boot        dd      16 dup(0)               ; 08h-0Fh and 70h-77h as the
                                                ; BIOS left them
vpc_order       db      0, 1, 2, 8, 9, 10, 11, 12, 13, 14, 15
                db      3, 4, 5, 6, 7, 0FFh
