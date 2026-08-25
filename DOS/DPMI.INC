;=====================================================================
; DPMI.INC - a DPMI 0.9 host
;
; The DOS Protected Mode Interface is how a DOS program gets at
; protected mode without owning the machine: it asks the host to put
; it there, and from then on it runs with selectors instead of
; segments, calls DOS through the host, and lets the host deal with
; the fact that DOS itself is a real-mode operating system.
;
; PM-DOS is an unusual host, because DOS here is ALREADY 32-bit
; protected-mode code.  Everything a conventional host does by
; switching to real mode, calling INT 21h and switching back, this one
; does by calling its own handler - the same k21 the DOS box bridge
; uses.  The client's pointers are selector:offset and the kernel's
; are flat, and that translation is the whole of the DOS bridge below.
;
; WHERE THE PIECES ARE:
;
;   The client is a program in a DOS box.  It starts in V86 like any
;   other, calls INT 2Fh AX=1687h to find us, and FAR CALLs the entry
;   point we hand back.  That entry is a HLT in the callback paragraph
;   XMS already uses - the monitor stops on it, the DOS side sees the
;   address, and instead of resuming the guest it builds descriptors
;   and IRETDs the client into ring 3.  The DOS box is still there;
;   its real-mode context is parked, and ending the client ends it.
;
;   The client runs at CPL 3 out of an LDT of its own.  IOPL stays 0,
;   which means CLI, STI, IN and OUT fault - and that is deliberate:
;   IOPL 3 would hand the client the I/O ports back, and among them
;   are the three the A20 gate is reached through, which v0.24 spent a
;   release taking away.  pm3_fault in VM86.INC emulates those six
;   instructions for a ring-3 client exactly as the monitor emulates
;   them for a V86 one, and the interrupt flag is virtual in the same
;   way and for the same reason.
;
;   INT 31h and INT 21h come straight back to the kernel through the
;   IDT, because the gates are DPL 3 and the handlers are already
;   protected-mode code.  Only real-mode services need an excursion:
;   21h/0300h, and the BIOS vectors, both of which go through
;   dpmi_simint, which parks the client, borrows the DOS box's own
;   V86 loop, and comes back when the simulated interrupt IRETs onto
;   a second HLT.
;
;   Real-mode callbacks go the other way, and are the other half of
;   the same trick: 0303h hands back a HLT in a paragraph of low
;   memory, real-mode code FAR CALLs it, the monitor stops, and the
;   client's procedure is entered at ring 3 with the guest's registers
;   in the structure it named.  It returns with an IRET, as the
;   specification requires, into a stub that is an INT 31h - a trap in
;   each direction, and no privileged instruction in the client.
;
; WHAT IS NOT HERE, and is not pretended to be:
;
;   Nested callbacks.  There is one callback stack and one saved
;   monitor frame, so a callback taken while a callback is running is
;   refused - the RETF behind the landing pad returns to the caller as
;   though nothing had claimed it.  Eight may be allocated at once;
;   one may be in flight.
;
;   0306h, the raw mode switch addresses.  A client that wants to
;   change mode itself wants ring 0, and that is the one thing on
;   offer nowhere here.
;
;   Virtual memory.  There is no swapping, so 0600h/0601h - lock and
;   unlock - succeed by doing nothing, which is the correct answer
;   for a host that never pages anything out.
;
;   Nested clients, and more than one at a time.  There is one LDT.
;
;   Protection between the client and the kernel.  Every page is
;   marked user (V86 needs that, see PMDOS.INC), so a client that
;   builds itself a descriptor over the kernel can reach it.  Real
;   hosts without paging protection are in the same position; it is
;   recorded here rather than implied.
;
; Included by DOSMAIN.ASM, after VDM.INC - it uses vdm_ctx, vdm_loop
; and the box's own unwind anchor.
;=====================================================================

;=====================================================================
; dpmi_init - once, at boot.
;
; The scratch real-mode stack is the only thing here that costs
; conventional memory, and it is one block of 256 bytes marked owner 8
; the way the XMS callback is: it belongs to the machine, not to any
; program, so no exit sweep takes it away.
;=====================================================================
dpmi_init:
        pushad
        mov     ebx,(DPMI_RMSTK + 15) / 16
        call    lm_sysalloc
        jc      di_nostk
        mov     word ptr ds:[dpmi_stkseg],ax
di_nostk:

        ; And the landing pads real-mode callbacks are made of, which
        ; are owner 8 for the same reason: a client comes and goes and
        ; these addresses do not move.
        mov     ebx,DPMICB_PARAS
        call    lm_sysalloc
        jc      di_nocb
        mov     word ptr ds:[dpmi_cbseg],ax

        movzx   edi,word ptr ds:[dpmi_cbseg]
        shl     edi,4
        xor     ebx,ebx
        mov     ecx,DPMI_MAXCB
di_cbone:
        mov     byte ptr [edi+ebx],0F4h         ; HLT - the monitor stops here
        mov     byte ptr [edi+ebx+1],0CBh       ; RETF - and if no client owns
        add     ebx,DPMICB_SLOT                 ; the slot, the call just
        loop    di_cbone                        ; returns whence it came

        mov     byte ptr [edi+DPMICB_R3RET],0CDh        ; INT 31h - where a
        mov     byte ptr [edi+DPMICB_R3RET+1],31h       ; procedure's IRET goes
        mov     byte ptr [edi+DPMICB_RETF],0CBh ; RETF - 0305h's state save,
di_nocb:                                        ; which has no state to save

        ; The vectors a protected-mode client expects to reach the
        ; BIOS through.  They are installed for good rather than per
        ; client: an IDT gate is machine state, and dpmi_reflect
        ; refuses politely when no client is running, which is what
        ; these vectors did before anyway.
        mov     esi,OFFSET dpmi_veclist
di_vec:
        movzx   eax,byte ptr [esi]
        or      al,al
        jz      di_vdone
        mov     edi,dword ptr [esi+1]
        call    dword ptr ds:[IOS_SETVEC]
        add     esi,5
        jmp     short di_vec
di_vdone:

        mov     al,31h                          ; and the interface itself
        mov     edi,OFFSET dpmi_int31
        call    dword ptr ds:[IOS_SETVEC]

        ; (What PMIO is to call when ring-3 code faults on something it
        ; cannot emulate is registered by DOSMAIN, not here: there are
        ; two kinds of ring-3 code now and the DPMI host is only one of
        ; them.  See pm3_dispatch.)
        popad
        ret

;=====================================================================
; dpmi_int2F - the two multiplex calls a client uses to find a host.
;              CF clear = answered.
;
; Called from the DOS box's INT 2Fh path, after XMS has had its look.
;=====================================================================
dpmi_int2F:
        mov     ax,word ptr [vdm_ctx+V86C_EAX]
        cmp     ax,1686h
        je      d2f_inpm
        cmp     ax,1687h
        je      d2f_entry
        stc
        ret

;--- 1686h: "am I in protected mode?"  A guest asking is not, and the
;--- answer for that is a NON-zero AX.  The inverted sense is the
;--- specification's, not a slip.
d2f_inpm:
        mov     word ptr [vdm_ctx+V86C_EAX],1686h
        clc
        ret

;--- 1687h: the mode-switch entry point, and everything a client needs
;--- to decide whether it wants it
d2f_entry:
        cmp     byte ptr ds:[dpmi_on],0
        je      d2f_none
        cmp     word ptr ds:[xms_cbseg],0
        je      d2f_none                        ; no callback paragraph, no
                                                ; entry point to give

        mov     word ptr [vdm_ctx+V86C_EAX],0   ; AX = 0: a host is here
        mov     word ptr [vdm_ctx+V86C_EBX],1   ; BX bit 0: 32-bit clients too
        mov     byte ptr [vdm_ctx+V86C_ECX],3   ; CL: a 386
        mov     word ptr [vdm_ctx+V86C_EDX],(DPMI_VER_MAJOR SHL 8) + DPMI_VER_MINOR
        mov     word ptr [vdm_ctx+V86C_ESI],DPMI_PRIVATE
        movzx   eax,word ptr ds:[xms_cbseg]
        mov     dword ptr [vdm_ctx+V86C_ES],eax
        mov     word ptr [vdm_ctx+V86C_EDI],DPMICB_ENTRY
        clc
        ret

d2f_none:
        mov     word ptr [vdm_ctx+V86C_EAX],8000h       ; AX <> 0: no host
        clc
        ret

;=====================================================================
; dpmi_iscb / dpmi_isret - whose HLT is this?
;
; The monitor stops on every HLT and the DOS side decides what it was.
; Two addresses matter here: the mode-switch entry, which a client
; FAR CALLs, and the landing pad a simulated real-mode interrupt IRETs
; onto.  Anything else at that address is a program that really did
; halt.
;=====================================================================
dpmi_iscb:
        mov     al,DPMICB_ENTRY
        jmp     short dpmi_atcb
dpmi_isret:
        mov     al,DPMICB_RET
dpmi_atcb:
        push    ebx
        push    edx
        movzx   ebx,al
        movzx   eax,word ptr ds:[xms_cbseg]
        or      eax,eax
        jz      dab_no
        cmp     dword ptr [vdm_ctx+V86C_CS],eax
        jne     dab_no
        mov     edx,dword ptr [vdm_ctx+V86C_EIP]
        cmp     edx,ebx
        jne     dab_no
        pop     edx
        pop     ebx
        clc
        ret
dab_no:
        pop     edx
        pop     ebx
        stc
        ret

;=====================================================================
; dpmi_switch - a guest has FAR CALLed the mode-switch entry.
;
; Reached from vdm_halt, so we are on the DOS box's own stack with
; vdm_ctx holding the client's real-mode registers.  This routine does
; not return: it either refuses (and resumes the guest) or leaves for
; ring 3 and never comes back through here.
;
; The client's stack holds the far return address of its own CALL, and
; THAT is what becomes CS:EIP in protected mode - the specification
; says the switch returns to the instruction after the call, with CS a
; selector for the segment the call was made from.
;=====================================================================
dpmi_switch:
        cmp     byte ptr ds:[dpmi_on],0
        je      dsw_refuse
        cmp     dword ptr ds:[dpmi_active],0
        jne     dsw_refuse                      ; one client, one LDT

;--- the far return address, off the guest's stack -------------------
        mov     eax,dword ptr [vdm_ctx+V86C_SS]
        shl     eax,4
        movzx   edx,word ptr [vdm_ctx+V86C_ESP]
        add     eax,edx
        movzx   ebx,word ptr [eax]              ; the offset to return to
        mov     dword ptr ds:[dpmi_ceip],ebx
        movzx   ebx,word ptr [eax+2]            ; and the segment it is in
        mov     dword ptr ds:[dpmi_cseg],ebx
        add     edx,4                           ; the CALL is finished with
        mov     word ptr [vdm_ctx+V86C_ESP],dx

;--- 16-bit or 32-bit? -----------------------------------------------
        mov     byte ptr ds:[dpmi_is32],0
        mov     byte ptr ds:[dpmi_gran],DGRAN_16
        test    byte ptr [vdm_ctx+V86C_EAX],1
        jz      dsw_16
        mov     byte ptr ds:[dpmi_is32],1
        mov     byte ptr ds:[dpmi_gran],DGRAN_32
dsw_16:

;--- the descriptor table, and the four the client starts with -------
        call    dpmi_ldtinit

        mov     ebx,dword ptr ds:[dpmi_cseg]
        shl     ebx,4
        mov     ax,(DPMI_IDX_CS SHL 3) + DPMI_SELBITS
        mov     ecx,0FFFFh
        mov     dl,DACC_CODE
        mov     dh,byte ptr ds:[dpmi_gran]
        call    dpmi_setdesc

        mov     ebx,dword ptr [vdm_ctx+V86C_DS]
        shl     ebx,4
        mov     ax,(DPMI_IDX_DS SHL 3) + DPMI_SELBITS
        mov     ecx,0FFFFh
        mov     dl,DACC_DATA
        mov     dh,byte ptr ds:[dpmi_gran]
        call    dpmi_setdesc

        mov     ebx,dword ptr [vdm_ctx+V86C_SS]
        shl     ebx,4
        mov     ax,(DPMI_IDX_SS SHL 3) + DPMI_SELBITS
        mov     ecx,0FFFFh
        mov     dl,DACC_DATA
        mov     dh,byte ptr ds:[dpmi_gran]
        call    dpmi_setdesc

        ; The PSP selector, and its limit is 100h bytes and not 64K.
        ; A client walking off the end of its own PSP gets a fault
        ; rather than the memory of whatever follows it, which is the
        ; one place the specification asks for a short limit and the
        ; one place it is worth having.
        movzx   ebx,word ptr ds:[vdm_pspseg]
        shl     ebx,4
        mov     ax,(DPMI_IDX_PSP SHL 3) + DPMI_SELBITS
        mov     ecx,0FFh
        mov     dl,DACC_DATA
        mov     dh,DGRAN_16
        call    dpmi_setdesc

        ; The code segment a real-mode callback returns THROUGH.  It is
        ; the callback paragraph, and the two bytes at DPMICB_R3RET in
        ; it are an INT 31h - which is how the host takes control back
        ; from a procedure that, by the specification, ends in an IRET
        ; and cannot be asked to end in anything else.  Its D bit
        ; follows the client's, because the RETF at DPMICB_RETF is the
        ; other thing this selector is for and a far return has to pop
        ; what the client's far call pushed.
        movzx   ebx,word ptr ds:[dpmi_cbseg]
        shl     ebx,4
        mov     ax,(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        mov     ecx,0FFFFh
        mov     dl,DACC_CODE
        mov     dh,byte ptr ds:[dpmi_gran]
        call    dpmi_setdesc

        call    dpmi_cbclr                      ; a new client holds none

;--- and go ----------------------------------------------------------
        mov     dword ptr ds:[dpmi_active],1
        movzx   eax,word ptr [vdm_ctx+V86C_ESP]
        mov     dword ptr ds:[dpmi_cesp],eax

        mov     ax,SEL_LDT
        lldt    ax

        mov     eax,dword ptr ds:[dpmi_ceip]
        mov     dword ptr ds:[dpmi_r3eip],eax
        mov     eax,dword ptr ds:[dpmi_cesp]
        mov     dword ptr ds:[dpmi_r3esp],eax
        jmp     dpmi_toring3

;--- DPMI is off, or a client is already running.  CF says no, and the
;--- RETF behind the HLT takes the caller back to real-mode life.
dsw_refuse:
        inc     dword ptr [vdm_ctx+V86C_EIP]    ; step over the HLT
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

;=====================================================================
; dpmi_toring3 - into the client, at dpmi_r3eip with dpmi_r3esp.
;
; The ring-0 stack pointer in the TSS is set FIRST and every time.
; Anything that interrupts ring-3 code switches stacks through it -
; a DPMI call, a DOS call, and the timer and keyboard IRQs, which are
; not asked for and arrive anyway - and v86_run leaves it pointing at
; its own frame, which by the time a client is running is a stack that
; belongs to nobody.
;=====================================================================
dpmi_toring3:
        cli
        mov     dword ptr ds:[TSS_BASE + TSS_ESP0],DPMI_KSTACK

        push    (DPMI_IDX_SS SHL 3) + DPMI_SELBITS
        push    dword ptr ds:[dpmi_r3esp]
        ; IOPL 0, so that CLI, STI, IN and OUT come back to us.  IF is
        ; set because the machine's own timer must keep running
        ; whatever the client thinks it has done - the flag it believes
        ; in is pm3_vif, exactly as a V86 guest's is v86_vif.
        push    EFL_IF OR 2
        push    (DPMI_IDX_CS SHL 3) + DPMI_SELBITS
        push    dword ptr ds:[dpmi_r3eip]

        ; DS AND ES HAVE TO BE LOADED HERE AND NOT LEFT TO THE IRETD.
        ; An inter-privilege IRET does not carry the caller's data
        ; segments across - it NULLS any of DS, ES, FS and GS whose
        ; DPL is below the level being returned to, and SEL_DATA32 is
        ; a ring-0 selector, so all four would arrive at the client as
        ; zero.  DPMI promises it DS and ES, so they are loaded now,
        ; while CPL is still 0 and a DPL-3 selector loads freely.
        ;
        ; Nothing below this may touch memory through DS.  The pushes
        ; are done, IRETD reads its frame through SS, and SS is still
        ; the kernel's.
        mov     ax,(DPMI_IDX_PSP SHL 3) + DPMI_SELBITS
        mov     bx,(DPMI_IDX_DS SHL 3) + DPMI_SELBITS
        mov     es,ax                           ; ES -> the PSP, as promised
        mov     ds,bx
        xor     ax,ax
        mov     fs,ax
        mov     gs,ax
        iretd

;=====================================================================
; The descriptor table
;=====================================================================

;---------------------------------------------------------------------
; dpmi_ldtinit - a table of zeros.  Descriptor 0 is left null so that
;                a stray selector of 0007h faults rather than working.
;---------------------------------------------------------------------
dpmi_ldtinit:
        pushad
        mov     edi,DPMI_LDT
        xor     eax,eax
        mov     ecx,DPMI_LDT_ENT * 2
        rep     stosd
        popad
        ret

;---------------------------------------------------------------------
; dpmi_slot - AX = selector -> EDI = its entry in the LDT.
;             CF set if it is not an LDT selector or is off the end.
;---------------------------------------------------------------------
dpmi_slot:
        push    eax
        test    al,4                            ; TI: must be the LDT
        jz      dsl_bad
        movzx   edi,ax
        shr     edi,3
        cmp     edi,DPMI_LDT_ENT
        jae     dsl_bad
        shl     edi,3
        add     edi,DPMI_LDT
        pop     eax
        clc
        ret
dsl_bad:
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; dpmi_setdesc - AX = selector, EBX = base, ECX = limit, DL = access,
;                DH = the granularity and size bits.  Preserves all.
;---------------------------------------------------------------------
dpmi_setdesc:
        pushad
        call    dpmi_slot
        jc      dsd_out
        mov     [edi],cx                        ; limit 15:0
        mov     [edi+2],bx                      ; base 15:0
        shr     ebx,16
        mov     [edi+4],bl                      ; base 23:16
        mov     [edi+5],dl                      ; the access byte
        shr     ecx,16
        and     cl,0Fh                          ; limit 19:16 ...
        or      cl,dh                           ; ... and G and D beside it
        mov     [edi+6],cl
        mov     [edi+7],bh                      ; base 31:24
dsd_out:
        popad
        ret

;---------------------------------------------------------------------
; dpmi_base - AX = selector -> EBX = the base it describes.  CF if the
;             selector is not one of ours or is not present.
;---------------------------------------------------------------------
dpmi_base:
        push    edi
        call    dpmi_slot
        jc      dbs_bad
        test    byte ptr [edi+5],80h            ; present?
        jz      dbs_bad
        movzx   ebx,byte ptr [edi+7]
        shl     ebx,8
        mov     bl,[edi+4]
        shl     ebx,16
        mov     bx,[edi+2]
        pop     edi
        clc
        ret
dbs_bad:
        pop     edi
        stc
        ret

;---------------------------------------------------------------------
; dpmi_getfree - EAX = the first of N free descriptors, CF if there is
;                no run of N.  N in ECX.
;---------------------------------------------------------------------
dpmi_getfree:
        push    ebx
        push    edx
        push    edi
        mov     eax,DPMI_IDX_FIRST
dgf_try:
        mov     ebx,eax
        add     ebx,ecx
        cmp     ebx,DPMI_LDT_ENT
        ja      dgf_none
        mov     edx,ecx                         ; are all N of them free?
        mov     edi,eax
dgf_one:
        push    eax
        mov     eax,edi
        shl     eax,3
        add     eax,DPMI_LDT
        cmp     byte ptr [eax+5],0              ; the access byte is 0 when
        pop     eax                             ; nothing has been put here
        jne     dgf_next
        inc     edi
        dec     edx
        jnz     dgf_one
        pop     edi
        pop     edx
        pop     ebx
        clc
        ret
dgf_next:
        inc     eax
        jmp     short dgf_try
dgf_none:
        pop     edi
        pop     edx
        pop     ebx
        stc
        ret

;=====================================================================
; dpmi_int31 - the interface.  An IDT gate at DPL 3, so the client
;              reaches it with a plain INT 31h.
;
; The frame is int21_entry's, field for field, because the two are
; entered the same way and the same F_* offsets address both.  DS and
; ES arrive holding the CLIENT'S selectors - an interrupt gate does
; not reload them - so they are saved and replaced before anything
; here touches memory, and put back on the way out.
;=====================================================================
dpmi_int31:
        push    ds
        push    es
        push    ebp
        pushad
        mov     ebp,esp
        mov     ax,ds
        mov     bx,es
        mov     cx,SEL_DATA32
        mov     ds,cx
        mov     es,cx
        mov     word ptr ds:[dpmi_cds],ax       ; the client's own, for the
        mov     word ptr ds:[dpmi_ces],bx       ; calls that take ES:(E)DI
        cld

        ; A CALLBACK PROCEDURE HAS FINISHED.  It arrives here because
        ; the IRET it is required to end with returns into the stub the
        ; host pushed for it, and that stub is an INT 31h.  The test is
        ; on where the call came FROM and not on what is in AX, because
        ; by then AX belongs to the client and means nothing to us.
        cmp     word ptr [ebp+F_CS],(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        je      dpmi_cbret

        mov     ax,word ptr [ebp+F_EAX]
        cmp     ax,0000h
        je      d31_alloc
        cmp     ax,0001h
        je      d31_freesel
        cmp     ax,0002h
        je      d31_seg2sel
        cmp     ax,0003h
        je      d31_selinc
        cmp     ax,0006h
        je      d31_getbase
        cmp     ax,0007h
        je      d31_setbase
        cmp     ax,0008h
        je      d31_setlimit
        cmp     ax,0009h
        je      d31_setacc
        cmp     ax,000Ah
        je      d31_alias
        cmp     ax,000Bh
        je      d31_getdesc
        cmp     ax,000Ch
        je      d31_setdesc
        cmp     ax,0100h
        je      d31_dosalloc
        cmp     ax,0101h
        je      d31_dosfree
        cmp     ax,0102h
        je      d31_dosresize
        cmp     ax,0200h
        je      d31_getrmvec
        cmp     ax,0201h
        je      d31_setrmvec
        cmp     ax,0204h
        je      d31_getpmvec
        cmp     ax,0205h
        je      d31_setpmvec
        cmp     ax,0300h
        je      d31_simint
        cmp     ax,0303h
        je      d31_cballoc
        cmp     ax,0304h
        je      d31_cbfree
        cmp     ax,0305h
        je      d31_savest
        cmp     ax,0400h
        je      d31_version
        cmp     ax,0401h
        je      d31_capab
        cmp     ax,0500h
        je      d31_freeinfo
        cmp     ax,0501h
        je      d31_memalloc
        cmp     ax,0502h
        je      d31_memfree
        cmp     ax,0503h
        je      d31_memresize
        cmp     ax,0600h
        je      d31_ok                          ; lock: nothing is pageable
        cmp     ax,0601h
        je      d31_ok                          ; unlock: likewise
        cmp     ax,0602h
        je      d31_ok
        cmp     ax,0603h
        je      d31_ok
        cmp     ax,0604h
        je      d31_pagesize

        mov     ax,DPE_UNSUPPORTED
;--- the two ways out ------------------------------------------------
d31_fail:
        mov     word ptr [ebp+F_EAX],ax
        or      dword ptr [ebp+F_FLAGS],FL_CF
        jmp     short d31_return
d31_ok:
        and     dword ptr [ebp+F_FLAGS],NOT FL_CF
d31_return:
        popad
        pop     ebp
        pop     es
        pop     ds
        iretd

;---------------------------------------------------------------------
; 0000h - allocate CX descriptors, contiguous.  AX = the first.
;---------------------------------------------------------------------
d31_alloc:
        movzx   ecx,word ptr [ebp+F_ECX]
        or      ecx,ecx
        jz      d31_badval
        cmp     ecx,DPMI_LDT_ENT
        ja      d31_nodesc
        call    dpmi_getfree
        jc      d31_nodesc
        mov     dword ptr ds:[dpmi_first],eax
        mov     edi,eax
da_one:
        ; A fresh descriptor is a PRESENT, zero-length data segment,
        ; which is what the specification asks for: usable as a
        ; selector, and faulting on any access until the client has
        ; said what it is for.
        push    ecx
        mov     eax,edi
        shl     eax,3
        or      eax,DPMI_SELBITS
        xor     ebx,ebx
        xor     ecx,ecx
        mov     dl,DACC_DATA
        mov     dh,DGRAN_16
        call    dpmi_setdesc
        pop     ecx
        inc     edi
        dec     ecx
        jnz     da_one

        mov     eax,dword ptr ds:[dpmi_first]
        shl     eax,3
        or      eax,DPMI_SELBITS
        mov     word ptr [ebp+F_EAX],ax
        jmp     d31_ok

;---------------------------------------------------------------------
; 0001h - free the descriptor in BX.
;---------------------------------------------------------------------
d31_freesel:
        mov     ax,word ptr [ebp+F_EBX]
        call    dpmi_slot
        jc      d31_badsel
        xor     eax,eax
        mov     [edi],eax
        mov     [edi+4],eax
        jmp     d31_ok

;---------------------------------------------------------------------
; 0002h - a descriptor for real-mode segment BX.
;
; The specification is explicit that the SAME selector comes back for
; the same segment and that it can never be freed, so they are handed
; out once and remembered.
;---------------------------------------------------------------------
d31_seg2sel:
        movzx   ebx,word ptr [ebp+F_EBX]
        mov     esi,OFFSET dpmi_segmap
        mov     ecx,dword ptr ds:[dpmi_segcnt]
        or      ecx,ecx
        jz      d2s_new
d2s_look:
        cmp     word ptr [esi],bx
        je      d2s_found
        add     esi,4
        loop    d2s_look
d2s_new:
        cmp     dword ptr ds:[dpmi_segcnt],DPMI_SEGMAX
        jae     d31_nodesc
        push    ebx
        mov     ecx,1
        call    dpmi_getfree
        pop     ebx
        jc      d31_nodesc
        push    eax
        shl     eax,3
        or      eax,DPMI_SELBITS
        push    eax
        mov     ecx,0FFFFh
        shl     ebx,4
        mov     dl,DACC_DATA
        mov     dh,DGRAN_16
        call    dpmi_setdesc
        pop     eax                             ; the selector
        pop     ecx                             ; (index, no longer wanted)
        mov     ecx,dword ptr ds:[dpmi_segcnt]
        mov     esi,OFFSET dpmi_segmap
        shl     ecx,2
        add     esi,ecx
        mov     bx,word ptr [ebp+F_EBX]
        mov     [esi],bx
        mov     [esi+2],ax
        inc     dword ptr ds:[dpmi_segcnt]
        mov     word ptr [ebp+F_EAX],ax
        jmp     d31_ok
d2s_found:
        mov     ax,[esi+2]
        mov     word ptr [ebp+F_EAX],ax
        jmp     d31_ok

;---------------------------------------------------------------------
; 0003h - how far apart two selectors are
;---------------------------------------------------------------------
d31_selinc:
        mov     word ptr [ebp+F_EAX],DPMI_SELINC
        jmp     d31_ok

;---------------------------------------------------------------------
; 0006h / 0007h - the base of the descriptor in BX
;---------------------------------------------------------------------
d31_getbase:
        mov     ax,word ptr [ebp+F_EBX]
        call    dpmi_base
        jc      d31_badsel
        mov     word ptr [ebp+F_EDX],bx
        shr     ebx,16
        mov     word ptr [ebp+F_ECX],bx
        jmp     d31_ok

d31_setbase:
        mov     ax,word ptr [ebp+F_EBX]
        call    dpmi_slot
        jc      d31_badsel
        movzx   ebx,word ptr [ebp+F_ECX]
        shl     ebx,16
        mov     bx,word ptr [ebp+F_EDX]
        mov     [edi+2],bx
        shr     ebx,16
        mov     [edi+4],bl
        mov     [edi+7],bh
        jmp     d31_ok

;---------------------------------------------------------------------
; 0008h - the limit.  A limit over a megabyte has to be 4K granular,
;         and the specification requires the low twelve bits to be all
;         ones when it is: it is the client's job to ask for the right
;         thing, and a refusal here is more use than a rounded answer.
;---------------------------------------------------------------------
d31_setlimit:
        mov     ax,word ptr [ebp+F_EBX]
        call    dpmi_slot
        jc      d31_badsel
        movzx   ecx,word ptr [ebp+F_ECX]
        shl     ecx,16
        mov     cx,word ptr [ebp+F_EDX]
        cmp     ecx,0FFFFFh
        jbe     dsl_byte
        mov     eax,ecx
        and     eax,0FFFh
        cmp     eax,0FFFh
        jne     d31_badval
        shr     ecx,12                          ; 4K granular from here
        mov     [edi],cx
        shr     ecx,16
        and     cl,0Fh
        mov     al,[edi+6]
        and     al,0F0h
        or      al,DGRAN_G
        or      al,cl
        mov     [edi+6],al
        jmp     d31_ok
dsl_byte:
        mov     [edi],cx
        shr     ecx,16
        and     cl,0Fh
        mov     al,[edi+6]
        and     al,070h                         ; keep D, drop G and the top
        or      al,cl                           ; of the old limit
        mov     [edi+6],al
        jmp     d31_ok

;---------------------------------------------------------------------
; 0009h - the access rights.  CL is the access byte and CH carries the
;         extended bits.  DPL is FORCED to 3: a client asking for ring
;         0 is asking for the kernel, and the answer is no.
;---------------------------------------------------------------------
d31_setacc:
        mov     ax,word ptr [ebp+F_EBX]
        call    dpmi_slot
        jc      d31_badsel
        mov     al,byte ptr [ebp+F_ECX]
        test    al,10h                          ; must be a code or data
        jz      d31_badval                      ; segment, never a gate
        or      al,60h                          ; DPL 3, always
        or      al,80h                          ; and present
        mov     [edi+5],al
        mov     al,byte ptr [ebp+F_ECX+1]
        and     al,0D0h                         ; G, D/B and the AVL bit
        mov     ah,[edi+6]
        and     ah,0Fh                          ; the limit stays as it was
        or      al,ah
        mov     [edi+6],al
        jmp     d31_ok

;---------------------------------------------------------------------
; 000Ah - a data descriptor over the same memory as the code one in BX
;---------------------------------------------------------------------
d31_alias:
        mov     ax,word ptr [ebp+F_EBX]
        call    dpmi_slot
        jc      d31_badsel
        mov     esi,edi
        push    esi
        mov     ecx,1
        call    dpmi_getfree
        pop     esi
        jc      d31_nodesc
        mov     edi,eax
        shl     edi,3
        add     edi,DPMI_LDT
        push    edi
        mov     ecx,2
        rep     movsd
        pop     edi
        mov     byte ptr [edi+5],DACC_DATA      ; the same memory, writable
        shl     eax,3
        or      eax,DPMI_SELBITS
        mov     word ptr [ebp+F_EAX],ax
        jmp     d31_ok

;---------------------------------------------------------------------
; 000Bh / 000Ch - the raw eight bytes, to and from ES:(E)DI
;---------------------------------------------------------------------
d31_getdesc:
        mov     ax,word ptr [ebp+F_EBX]
        call    dpmi_slot
        jc      d31_badsel
        mov     esi,edi
        call    dpmi_esdi
        jc      d31_badsel
        mov     ecx,2
        rep     movsd
        jmp     d31_ok

d31_setdesc:
        mov     ax,word ptr [ebp+F_EBX]
        call    dpmi_slot
        jc      d31_badsel
        push    edi
        call    dpmi_esdi
        pop     edi
        jc      d31_badsel
        mov     esi,edi                         ; ESI = ours, EDI = theirs
        xchg    esi,edi
        push    edi
        mov     ecx,2
        rep     movsd
        pop     edi
        or      byte ptr [edi+5],60h            ; DPL 3, whatever they said
        jmp     d31_ok

;---------------------------------------------------------------------
; 0100h / 0101h / 0102h - memory below a megabyte, for talking to
; real-mode code with.  AX comes back as the SEGMENT and DX as a
; selector for it, which is what makes the block usable from both.
;---------------------------------------------------------------------
d31_dosalloc:
        movzx   ebx,word ptr [ebp+F_EBX]        ; paragraphs
        call    lm_alloc
        jnc     dda_got
        mov     word ptr [ebp+F_EBX],bx         ; the largest there is
        mov     ax,8012h
        jmp     d31_fail
dda_got:
        ; The same correction 21h/48h needs, for the same reason: a
        ; block lm_alloc handed back owns ITSELF, and a client that
        ; exits without calling 0101h would otherwise leave it behind
        ; for good.  A DPMI client is a program in a DOS box, so the
        ; box's own exit sweep takes it once it is stamped.
        mov     dx,word ptr ds:[vdm_curpsp]
        call    lm_setown

        movzx   eax,ax
        mov     dword ptr ds:[dpmi_dseg],eax
        mov     ecx,1
        call    dpmi_getfree
        jc      dda_nodesc
        shl     eax,3
        or      eax,DPMI_SELBITS
        mov     dword ptr ds:[dpmi_dsel],eax

        mov     ebx,dword ptr ds:[dpmi_dseg]
        shl     ebx,4
        mov     ecx,0FFFFh
        mov     dl,DACC_DATA
        mov     dh,DGRAN_16
        call    dpmi_setdesc

        mov     eax,dword ptr ds:[dpmi_dseg]
        mov     word ptr [ebp+F_EAX],ax         ; the SEGMENT, for real-mode
        mov     eax,dword ptr ds:[dpmi_dsel]    ; code to be given
        mov     word ptr [ebp+F_EDX],ax         ; and a selector, for the client
        jmp     d31_ok
dda_nodesc:
        mov     eax,dword ptr ds:[dpmi_dseg]
        call    lm_free
        jmp     d31_nodesc

d31_dosfree:
        mov     ax,word ptr [ebp+F_EDX]         ; the selector we gave back
        call    dpmi_base
        jc      d31_badsel
        shr     ebx,4
        mov     eax,ebx
        call    lm_free
        jc      d31_badsel
        mov     ax,word ptr [ebp+F_EDX]
        call    dpmi_slot
        jc      d31_ok
        xor     eax,eax
        mov     [edi],eax
        mov     [edi+4],eax
        jmp     d31_ok

d31_dosresize:
        mov     ax,word ptr [ebp+F_EDX]
        call    dpmi_base
        jc      d31_badsel
        shr     ebx,4
        mov     eax,ebx
        movzx   ebx,word ptr [ebp+F_EBX]
        call    lm_resize
        jc      ddr_no
        ; The block did not move, so neither does the descriptor - only
        ; its limit could, and DPMI does not ask us to change it here.
        jmp     d31_ok
ddr_no:
        mov     word ptr [ebp+F_EBX],bx
        mov     ax,8012h
        jmp     d31_fail

;---------------------------------------------------------------------
; 0200h / 0201h - the REAL-MODE vector table, which is still at 0:0
; and still the machine's.  A client hooking one here is hooking it
; for the DOS box as well, which is exactly what it means to.
;---------------------------------------------------------------------
d31_getrmvec:
        movzx   ebx,byte ptr [ebp+F_EBX]
        shl     ebx,2
        mov     eax,dword ptr ds:[ebx]
        mov     word ptr [ebp+F_EDX],ax
        shr     eax,16
        mov     word ptr [ebp+F_ECX],ax
        jmp     d31_ok

d31_setrmvec:
        ; Through ivt_set, not a bare store: vectors 0-3 are the bytes
        ; the null guard watches, and a client hooking one of them is
        ; doing something legitimate.  See ivt_set in VDM.INC.
        movzx   eax,byte ptr [ebp+F_EBX]
        movzx   edx,word ptr [ebp+F_ECX]
        shl     edx,16
        mov     dx,word ptr [ebp+F_EDX]
        call    ivt_set
        jmp     d31_ok

;---------------------------------------------------------------------
; 0204h / 0205h - the PROTECTED-MODE vectors, which are ours.  A zero
; entry means "reflect to real mode", which is what every one of them
; starts as.
;---------------------------------------------------------------------
d31_getpmvec:
        movzx   ebx,byte ptr [ebp+F_EBX]
        shl     ebx,3
        add     ebx,OFFSET dpmi_pmvec
        mov     eax,dword ptr [ebx]
        mov     dword ptr [ebp+F_EDX],eax
        movzx   eax,word ptr [ebx+4]
        or      eax,eax
        jnz     dgp_have
        mov     eax,(DPMI_IDX_CS SHL 3) + DPMI_SELBITS
dgp_have:
        mov     word ptr [ebp+F_ECX],ax
        jmp     d31_ok

d31_setpmvec:
        movzx   ebx,byte ptr [ebp+F_EBX]
        shl     ebx,3
        add     ebx,OFFSET dpmi_pmvec
        mov     eax,dword ptr [ebp+F_EDX]
        mov     dword ptr [ebx],eax
        mov     ax,word ptr [ebp+F_ECX]
        mov     word ptr [ebx+4],ax
        jmp     d31_ok

;---------------------------------------------------------------------
; 0300h - run a real-mode interrupt and come back with the registers
;---------------------------------------------------------------------
d31_simint:
        cmp     byte ptr [ebp+F_EBX+1],0        ; BH must be zero
        jne     d31_badval
        call    dpmi_esdi
        jc      d31_badsel
        mov     dword ptr ds:[dpmi_rmcs],edi
        mov     al,byte ptr [ebp+F_EBX]
        mov     byte ptr ds:[dpmi_rmint],al
        mov     ax,word ptr [ebp+F_ECX]
        mov     word ptr ds:[dpmi_rmwords],ax
        call    dpmi_simint
        jc      d31_badval
        jmp     d31_ok

;---------------------------------------------------------------------
; 0303h - allocate a real-mode callback.
;
;   DS:ESI = the procedure to call, ES:EDI = the register structure it
;   wants filled in.  CX:DX comes back as an address real-mode code can
;   FAR CALL, and calling it arrives in that procedure, in protected
;   mode, at ring 3.
;
; BOTH HALVES OF ES:EDI ARE KEPT.  The flat address, because the host
; is the one who writes the structure; and the selector and offset as
; the client wrote them, because the procedure is promised the same
; ES:EDI back and a flat address would mean nothing to it.
;---------------------------------------------------------------------
d31_cballoc:
        cmp     word ptr ds:[dpmi_cbseg],0
        je      d31_nocb                        ; no low memory, no addresses
        call    dpmi_cbstack                    ; the stack it will run on,
        jc      d31_nomem                       ; made once and then kept

        mov     ax,word ptr ds:[dpmi_cds]       ; is the procedure's selector
        call    dpmi_base                       ; one of ours at all?
        jc      d31_badsel
        call    dpmi_esdi                       ; EDI = the structure, flat
        jc      d31_badsel

        push    edi
        call    dpmi_cbslot                     ; ESI = a free entry, EAX = its
        pop     edi                             ; index
        jc      d31_nocb

        mov     dword ptr [esi+DCB_RMCS],edi
        movzx   ebx,word ptr ds:[dpmi_ces]
        mov     dword ptr [esi+DCB_RSEL],ebx
        movzx   ebx,word ptr ds:[dpmi_cds]
        mov     dword ptr [esi+DCB_PSEL],ebx
        push    eax
        mov     eax,dword ptr [ebp+F_EDI]
        call    dpmi_off
        mov     dword ptr [esi+DCB_ROFF],eax
        mov     eax,dword ptr [ebp+F_ESI]
        call    dpmi_off
        mov     dword ptr [esi+DCB_PROC],eax
        pop     eax
        mov     dword ptr [esi+DCB_FLAGS],1

        imul    eax,eax,DPMICB_SLOT             ; the slot's landing pad
        mov     word ptr [ebp+F_EDX],ax
        mov     ax,word ptr ds:[dpmi_cbseg]
        mov     word ptr [ebp+F_ECX],ax
        jmp     d31_ok

;---------------------------------------------------------------------
; 0304h - free the callback whose address is in CX:DX
;---------------------------------------------------------------------
d31_cbfree:
        mov     ax,word ptr ds:[dpmi_cbseg]
        or      ax,ax
        jz      d31_badval
        cmp     word ptr [ebp+F_ECX],ax
        jne     d31_badval
        movzx   eax,word ptr [ebp+F_EDX]
        call    dpmi_cbfind
        jc      d31_badval
        mov     dword ptr [esi+DCB_FLAGS],0
        jmp     d31_ok

;---------------------------------------------------------------------
; 0305h - where to save and restore the state a mode switch loses.
;
; NOTHING, and that is a real answer rather than a shrug.  A
; conventional host has to offer this because putting a real CPU back
; into real mode drops state a client cannot reach to save; here the
; two modes are the same CPU with a different CPL and the switch is an
; IRETD, so there is nothing to lose.  The size is zero and both
; procedures are the RETF at DPMICB_RETF - the real-mode one reached
; as a segment, the protected-mode one through the descriptor over the
; same paragraph.
;---------------------------------------------------------------------
d31_savest:
        cmp     word ptr ds:[dpmi_cbseg],0
        je      d31_nocb
        mov     word ptr [ebp+F_EAX],0          ; no buffer wanted
        mov     ax,word ptr ds:[dpmi_cbseg]
        mov     word ptr [ebp+F_EBX],ax                 ; BX:CX - real mode
        mov     word ptr [ebp+F_ECX],DPMICB_RETF
        mov     word ptr [ebp+F_ESI],(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        mov     word ptr [ebp+F_EDI],DPMICB_RETF        ; SI:DI - protected
        jmp     d31_ok

;---------------------------------------------------------------------
; 0400h / 0401h - who is the host
;---------------------------------------------------------------------
d31_version:
        mov     word ptr [ebp+F_EAX],(DPMI_VER_MAJOR SHL 8) + DPMI_VER_MINOR
        ; BX bit 0: no 386 paging games we expose; bit 2: we are the
        ; whole machine, not a window in a multitasker.
        mov     word ptr [ebp+F_EBX],0004h
        mov     byte ptr [ebp+F_ECX],3          ; CL = a 386
        mov     word ptr [ebp+F_EDX],0FF00h     ; slave PIC base | master
        mov     byte ptr [ebp+F_EDX],IRQ_BASE
        mov     byte ptr [ebp+F_EDX+1],IRQ_BASE2
        jmp     d31_ok

d31_capab:
        mov     word ptr [ebp+F_EAX],0          ; no optional capability
        mov     word ptr [ebp+F_ECX],0
        mov     word ptr [ebp+F_EDX],0
        jmp     d31_ok

;---------------------------------------------------------------------
; 0500h - the free memory report, into the 30h-byte block at ES:(E)DI.
; Only the first field is a real number; the specification says the
; rest may be -1 and that a client must not believe them.
;---------------------------------------------------------------------
d31_freeinfo:
        call    dpmi_esdi
        jc      d31_badsel
        push    edi
        mov     ecx,12
        mov     eax,0FFFFFFFFh
        rep     stosd                           ; -1 everywhere first: the
        pop     edi                             ; specification allows it and
                                                ; a client must not believe a
                                                ; field we did not fill in
        call    arena_avail                     ; EAX = the biggest run there is
        mov     dword ptr [edi],eax
        shr     eax,12
        mov     dword ptr [edi+4],eax           ; the same number as pages
        mov     dword ptr [edi+8],eax           ; and none of it is locked
        jmp     d31_ok

;---------------------------------------------------------------------
; 0501h / 0502h / 0503h - extended memory blocks, out of the kernel's
; own arena.  The client gets a LINEAR address and a handle; making a
; descriptor over it is a separate call and its own business.
;---------------------------------------------------------------------
d31_memalloc:
        movzx   ebx,word ptr [ebp+F_EBX]
        shl     ebx,16
        mov     bx,word ptr [ebp+F_ECX]
        or      ebx,ebx
        jz      d31_badval
        call    dpmi_memslot                    ; ESI = a free entry, EAX = the
        jc      d31_nomem                       ; handle that names it
        mov     dword ptr ds:[dpmi_hand],eax
        mov     dword ptr ds:[dpmi_msize],ebx
        call    arena_alloc                     ; EBX bytes -> EAX
        jc      d31_nomem
        mov     dword ptr [esi+DM_FLAGS],1
        mov     dword ptr [esi+DM_ADDR],eax
        mov     ebx,dword ptr ds:[dpmi_msize]
        mov     dword ptr [esi+DM_SIZE],ebx
        mov     ebx,dword ptr ds:[cur_own]
        mov     dword ptr [esi+DM_OWN],ebx

        ; BX:CX and SI:DI are HIGH:LOW, both of them - the same order
        ; the size came in as.  Getting this the other way round hands
        ; the client a base with its halves swapped, and the first
        ; thing it does with the selector is a page fault a long way
        ; from here.
        mov     word ptr [ebp+F_ECX],ax
        shr     eax,16
        mov     word ptr [ebp+F_EBX],ax
        mov     eax,dword ptr ds:[dpmi_hand]
        mov     word ptr [ebp+F_EDI],ax
        shr     eax,16
        mov     word ptr [ebp+F_ESI],ax
        jmp     d31_ok

d31_memfree:
        call    dpmi_memfind                    ; ESI, from the handle in SI:DI
        jc      d31_badhandle
        mov     edx,dword ptr [esi+DM_ADDR]
        call    arena_free
        mov     dword ptr [esi+DM_FLAGS],0
        jmp     d31_ok

d31_memresize:
        call    dpmi_memfind
        jc      d31_badhandle
        movzx   ebx,word ptr [ebp+F_EBX]
        shl     ebx,16
        mov     bx,word ptr [ebp+F_ECX]
        or      ebx,ebx
        jz      d31_badval
        mov     edx,dword ptr [esi+DM_ADDR]
        push    esi
        call    arena_resize                    ; in place where it can be
        pop     esi
        jc      d31_nomem
        mov     dword ptr [esi+DM_SIZE],ebx
        mov     eax,edx                         ; the address does not move
        mov     word ptr [ebp+F_ECX],ax         ; BX:CX, high then low
        shr     eax,16
        mov     word ptr [ebp+F_EBX],ax
        jmp     d31_ok

;---------------------------------------------------------------------
; 0604h - the page size, which is the one thing about paging a client
;         is allowed to assume
;---------------------------------------------------------------------
d31_pagesize:
        mov     word ptr [ebp+F_EBX],0
        mov     word ptr [ebp+F_ECX],1000h
        jmp     d31_ok

;--- the failure exits, named so the code above reads --------------
d31_badsel:
        mov     ax,DPE_BADSEL
        jmp     d31_fail
d31_badval:
        mov     ax,DPE_BADVALUE
        jmp     d31_fail
d31_nodesc:
        mov     ax,DPE_NODESC
        jmp     d31_fail
d31_nomem:
        mov     ax,DPE_NOMEM
        jmp     d31_fail
d31_badhandle:
        mov     ax,DPE_BADHANDLE
        jmp     d31_fail
d31_nocb:
        mov     ax,DPE_NOCB
        jmp     d31_fail

;---------------------------------------------------------------------
; dpmi_esdi - the client's ES:(E)DI as one flat address in EDI.
;             CF if ES is not a descriptor we know.
;---------------------------------------------------------------------
dpmi_esdi:
        push    eax
        push    ebx
        mov     ax,word ptr ds:[dpmi_ces]       ; captured on the way in: the
        call    dpmi_base                       ; live ES is the kernel's now
        jc      des_bad
        mov     edi,dword ptr [ebp+F_EDI]
        cmp     byte ptr ds:[dpmi_is32],0
        jne     des_32
        movzx   edi,di
des_32:
        add     edi,ebx
        pop     ebx
        pop     eax
        clc
        ret
des_bad:
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; dpmi_memslot / dpmi_memfind - the memory block table
;---------------------------------------------------------------------
dpmi_memslot:
        push    ecx
        mov     esi,OFFSET dpmi_memtab
        mov     ecx,DPMI_MAXMEM
        xor     eax,eax
dms_one:
        inc     eax                             ; handles are one-based
        cmp     dword ptr [esi+DM_FLAGS],0
        je      dms_got
        add     esi,DM_LEN
        loop    dms_one
        pop     ecx
        stc
        ret
dms_got:
        pop     ecx
        clc
        ret

dpmi_memfind:
        movzx   eax,word ptr [ebp+F_ESI]
        shl     eax,16
        mov     ax,word ptr [ebp+F_EDI]
        or      eax,eax
        jz      dmf_bad
        cmp     eax,DPMI_MAXMEM
        ja      dmf_bad
        dec     eax
        imul    eax,DM_LEN
        mov     esi,OFFSET dpmi_memtab
        add     esi,eax
        cmp     dword ptr [esi+DM_FLAGS],0
        je      dmf_bad
        clc
        ret
dmf_bad:
        stc
        ret

;=====================================================================
; dpmi_simint - run one real-mode interrupt on the client's behalf.
;
; dpmi_rmcs -> the client's register structure, dpmi_rmint = the
; vector, dpmi_rmwords = words to copy from the client's stack.
;
; THE TRICK, AND IT IS THE WHOLE ROUTINE.  The DOS box already has a
; V86 loop that knows how to answer everything a real-mode program
; can ask - INT 21h, INT 13h, XMS, the lot - so rather than write a
; second one, this borrows it.  The client's parked context is saved,
; vdm_ctx is filled in from the register structure, an IRET frame is
; pushed that returns to a HLT we planted, and control jumps into
; vdm_loop.  When the real-mode handler finishes, its IRET lands on
; that HLT, the monitor stops, and vdm_halt recognises the address and
; longjmps back here.
;
; So a simulated INT 21h from a protected-mode client is served by the
; same code that serves it from a DOS program, which is the point.
;=====================================================================
dpmi_simint:
        pushad
        cmp     word ptr ds:[xms_cbseg],0
        je      dsi_no
        cmp     dword ptr ds:[dpmi_rmact],0
        jne     dsi_no                          ; no nesting

;--- park the client's real-mode context -----------------------------
        mov     esi,OFFSET vdm_ctx
        mov     edi,OFFSET dpmi_savectx
        mov     ecx,V86C_SIZE / 4
        rep     movsd

;--- and build the one the interrupt runs in -------------------------
        mov     esi,dword ptr ds:[dpmi_rmcs]
        mov     eax,[esi+RMCS_EDI]
        mov     dword ptr [vdm_ctx+V86C_EDI],eax
        mov     eax,[esi+RMCS_ESI]
        mov     dword ptr [vdm_ctx+V86C_ESI],eax
        mov     eax,[esi+RMCS_EBP]
        mov     dword ptr [vdm_ctx+V86C_EBP],eax
        mov     eax,[esi+RMCS_EBX]
        mov     dword ptr [vdm_ctx+V86C_EBX],eax
        mov     eax,[esi+RMCS_EDX]
        mov     dword ptr [vdm_ctx+V86C_EDX],eax
        mov     eax,[esi+RMCS_ECX]
        mov     dword ptr [vdm_ctx+V86C_ECX],eax
        mov     eax,[esi+RMCS_EAX]
        mov     dword ptr [vdm_ctx+V86C_EAX],eax
        movzx   eax,word ptr [esi+RMCS_ES]
        mov     dword ptr [vdm_ctx+V86C_ES],eax
        movzx   eax,word ptr [esi+RMCS_DS]
        mov     dword ptr [vdm_ctx+V86C_DS],eax
        movzx   eax,word ptr [esi+RMCS_FS]
        mov     dword ptr [vdm_ctx+V86C_FS],eax
        movzx   eax,word ptr [esi+RMCS_GS]
        mov     dword ptr [vdm_ctx+V86C_GS],eax
        mov     dword ptr [vdm_ctx+V86C_FLAGS],EFL_IF

        ; A stack of the caller's, or one of ours if it left the field
        ; zero - which is what the specification tells a client to do
        ; and what every one of them does.
        movzx   eax,word ptr [esi+RMCS_SS]
        movzx   edx,word ptr [esi+RMCS_SP]
        or      eax,eax
        jnz     dsi_stack
        or      edx,edx
        jnz     dsi_stack
        movzx   eax,word ptr ds:[dpmi_stkseg]
        or      eax,eax
        jz      dsi_undo
        mov     edx,DPMI_RMSTK
dsi_stack:
        mov     dword ptr [vdm_ctx+V86C_SS],eax
        mov     dword ptr [vdm_ctx+V86C_ESP],edx

;--- the words the client wants copied onto that stack ---------------
; Only 21h/0300h ever asks for this, and only 21h/0300h has the frame
; the offsets below name.  The reflector sets the count to zero, and
; the branch is skipped.
        movzx   ecx,word ptr ds:[dpmi_rmwords]
        or      ecx,ecx
        jz      dsi_nocopy
        mov     esi,dword ptr [ebp+F_ESP3]      ; the client's own stack
        push    ecx
        mov     ax,word ptr [ebp+F_SS3]
        call    dpmi_base
        pop     ecx
        jc      dsi_nocopy
        add     esi,ebx
        mov     eax,dword ptr [vdm_ctx+V86C_SS]
        shl     eax,4
        mov     edx,dword ptr [vdm_ctx+V86C_ESP]
        sub     edx,ecx
        sub     edx,ecx                         ; words, so twice
        mov     dword ptr [vdm_ctx+V86C_ESP],edx
        add     eax,edx
        mov     edi,eax
        rep     movsw
dsi_nocopy:

;--- and point the guest at an INT instruction of the right number ---
; NOT at whatever the vector table holds.  The monitor traps INT 21h
; and never reflects it, so PM-DOS has no reason to put an entry in
; the table for it and does not - reading one gave 0000:0000, and the
; guest went off to execute the vector table as code.  Executing the
; instruction sends the interrupt wherever it would go for any DOS
; program: to the kernel if the monitor claims it, through the guest's
; own table if it does not.
        movzx   edi,word ptr ds:[xms_cbseg]
        shl     edi,4
        mov     al,byte ptr ds:[dpmi_rmint]
        mov     byte ptr [edi+DPMICB_SIM+1],al

        movzx   eax,word ptr ds:[xms_cbseg]
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        mov     dword ptr [vdm_ctx+V86C_EIP],DPMICB_SIM

;--- and let the DOS box's own loop run it ---------------------------
        mov     dword ptr ds:[dpmi_rmact],1
        mov     dword ptr ds:[dpmi_rmesp],esp
        jmp     vdm_loop

dsi_undo:
        mov     esi,OFFSET dpmi_savectx
        mov     edi,OFFSET vdm_ctx
        mov     ecx,V86C_SIZE / 4
        rep     movsd
dsi_no:
        popad
        stc
        ret

;=====================================================================
; dpmi_rmdone - the simulated interrupt has IRETed onto our landing
;               pad.  Reached from vdm_halt, on the monitor's stack;
;               it unwinds to dpmi_simint's caller.
;=====================================================================
dpmi_rmdone:
        cmp     dword ptr ds:[dpmi_rmact],0
        je      vdm_abend                       ; a HLT we never planted

        mov     esp,dword ptr ds:[dpmi_rmesp]
        mov     dword ptr ds:[dpmi_rmact],0

;--- the registers, back into the client's structure -----------------
        mov     edi,dword ptr ds:[dpmi_rmcs]
        mov     eax,dword ptr [vdm_ctx+V86C_EDI]
        mov     [edi+RMCS_EDI],eax
        mov     eax,dword ptr [vdm_ctx+V86C_ESI]
        mov     [edi+RMCS_ESI],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EBP]
        mov     [edi+RMCS_EBP],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EBX]
        mov     [edi+RMCS_EBX],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EDX]
        mov     [edi+RMCS_EDX],eax
        mov     eax,dword ptr [vdm_ctx+V86C_ECX]
        mov     [edi+RMCS_ECX],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EAX]
        mov     [edi+RMCS_EAX],eax
        mov     eax,dword ptr [vdm_ctx+V86C_FLAGS]
        mov     [edi+RMCS_FLAGS],ax
        mov     eax,dword ptr [vdm_ctx+V86C_ES]
        mov     [edi+RMCS_ES],ax
        mov     eax,dword ptr [vdm_ctx+V86C_DS]
        mov     [edi+RMCS_DS],ax
        mov     eax,dword ptr [vdm_ctx+V86C_FS]
        mov     [edi+RMCS_FS],ax
        mov     eax,dword ptr [vdm_ctx+V86C_GS]
        mov     [edi+RMCS_GS],ax

;--- and the client's own context, back where it was -----------------
        mov     esi,OFFSET dpmi_savectx
        mov     edi,OFFSET vdm_ctx
        mov     ecx,V86C_SIZE / 4
        rep     movsd

        popad
        clc
        ret

;=====================================================================
; REAL-MODE CALLBACKS
;
; The other direction, and the harder one.  Everything above is a
; client asking for something; a callback is real-mode code calling
; the client, which means the host has to manufacture an address that
; is executable in real mode and arrives in protected mode - and then
; has to get control back from a procedure the specification says ends
; with an IRET and nothing else.
;
; The address is a HLT in the callback paragraph, which is the same
; idiom as everything else the monitor is asked to notice.  The way
; back is the interesting half: the procedure's IRET is pointed at a
; stub which is an INT 31h, executed at ring 3 through a code
; descriptor over that same paragraph.  So a callback goes out through
; a trap in real mode and comes back through a trap in protected mode,
; and neither end needs a privileged instruction in the client.
;=====================================================================

;---------------------------------------------------------------------
; dpmi_iscall - is this HLT one of the callback landing pads?  CF
;               clear and EAX = which one, if it is.
;---------------------------------------------------------------------
dpmi_iscall:
        movzx   eax,word ptr ds:[dpmi_cbseg]
        or      eax,eax
        jz      dic_no
        cmp     dword ptr [vdm_ctx+V86C_CS],eax
        jne     dic_no
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        cmp     eax,DPMI_MAXCB * DPMICB_SLOT
        jae     dic_no
        test    al,DPMICB_SLOT - 1              ; and on a slot boundary: the
        jnz     dic_no                          ; RETF is not an entry point
        shr     eax,1                           ; DPMICB_SLOT is 2
        clc
        ret
dic_no:
        stc
        ret

;---------------------------------------------------------------------
; dpmi_cbslot - ESI = a free callback entry, EAX = its index.  CF if
;               the client is holding all of them already.
; dpmi_cbfind - EAX = an offset in the paragraph -> ESI = the entry it
;               names, CF if it names nothing.
; dpmi_cbclr  - all of them, free.  A new client holds none, and a
;               dead one holds none either.
;---------------------------------------------------------------------
dpmi_cbslot:
        push    ecx
        mov     esi,OFFSET dpmi_cbtab
        xor     eax,eax
        mov     ecx,DPMI_MAXCB
dcs_one:
        cmp     dword ptr [esi+DCB_FLAGS],0
        je      dcs_got
        add     esi,DCB_LEN
        inc     eax
        loop    dcs_one
        pop     ecx
        stc
        ret
dcs_got:
        pop     ecx
        clc
        ret

dpmi_cbfind:
        cmp     eax,DPMI_MAXCB * DPMICB_SLOT
        jae     dcf_bad
        test    al,DPMICB_SLOT - 1
        jnz     dcf_bad
        shr     eax,1
        imul    esi,eax,DCB_LEN
        add     esi,OFFSET dpmi_cbtab
        cmp     dword ptr [esi+DCB_FLAGS],0
        je      dcf_bad
        clc
        ret
dcf_bad:
        stc
        ret

dpmi_cbclr:
        pushad
        mov     esi,OFFSET dpmi_cbtab
        mov     ecx,DPMI_MAXCB
dcc_one:
        mov     dword ptr [esi+DCB_FLAGS],0
        add     esi,DCB_LEN
        loop    dcc_one
        mov     dword ptr ds:[dpmi_cbact],0
        popad
        ret

;---------------------------------------------------------------------
; dpmi_cbstack - the protected-mode stack a callback procedure runs on,
;                and a descriptor over it.  The memory is taken once
;                and never given back; the descriptor is rebuilt every
;                time, because each client gets a wiped LDT.
;
; DPMI calls this "a locked API stack".  Nothing here is pageable, so
; locking it amounts to never freeing it, which is what happens.
;---------------------------------------------------------------------
dpmi_cbstack:
        pushad
        cmp     dword ptr ds:[dpmi_cbbase],0
        jne     dck_have
        mov     ebx,DPMI_CBSTK
        call    arena_alloc
        jc      dck_no
        mov     dword ptr ds:[dpmi_cbbase],eax
dck_have:
        mov     ebx,dword ptr ds:[dpmi_cbbase]
        mov     ax,(DPMI_IDX_CBSS SHL 3) + DPMI_SELBITS
        mov     ecx,DPMI_CBSTK - 1
        mov     dl,DACC_DATA
        mov     dh,byte ptr ds:[dpmi_gran]
        call    dpmi_setdesc
        popad
        clc
        ret
dck_no:
        popad
        stc
        ret

;=====================================================================
; dpmi_docall - real-mode code has FAR CALLed a callback address.
;               EAX = which slot.  Reached from vdm_halt, on the
;               monitor's stack, and it does not return.
;
; WHAT THE PROCEDURE IS GIVEN is exactly what the specification asks
; for: DS:ESI on the real-mode stack and ES:EDI on the register
; structure the callback was allocated with.  The host does NOT take
; the far return address off that stack - the example handler in the
; specification does it itself, which is the entire reason DS:ESI is a
; stack pointer and not something more convenient.  So CS:IP in the
; structure is where the call went TO, and the procedure decides where
; real mode carries on by writing where it came FROM.
;=====================================================================
dpmi_docall:
        imul    esi,eax,DCB_LEN
        add     esi,OFFSET dpmi_cbtab
        cmp     dword ptr [esi+DCB_FLAGS],0
        je      dcl_refuse                      ; a slot nobody owns
        cmp     dword ptr ds:[dpmi_active],0
        je      dcl_refuse                      ; ...or no client to own it
        cmp     dword ptr ds:[dpmi_cbact],0
        jne     dcl_refuse                      ; one at a time: one stack
        cmp     dword ptr ds:[dpmi_cbbase],0
        je      dcl_refuse

;--- the real-mode registers, into the structure the client named ----
        mov     edi,dword ptr [esi+DCB_RMCS]
        mov     eax,dword ptr [vdm_ctx+V86C_EDI]
        mov     [edi+RMCS_EDI],eax
        mov     eax,dword ptr [vdm_ctx+V86C_ESI]
        mov     [edi+RMCS_ESI],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EBP]
        mov     [edi+RMCS_EBP],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EBX]
        mov     [edi+RMCS_EBX],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EDX]
        mov     [edi+RMCS_EDX],eax
        mov     eax,dword ptr [vdm_ctx+V86C_ECX]
        mov     [edi+RMCS_ECX],eax
        mov     eax,dword ptr [vdm_ctx+V86C_EAX]
        mov     [edi+RMCS_EAX],eax
        mov     eax,dword ptr [vdm_ctx+V86C_FLAGS]
        mov     [edi+RMCS_FLAGS],ax
        mov     eax,dword ptr [vdm_ctx+V86C_ES]
        mov     [edi+RMCS_ES],ax
        mov     eax,dword ptr [vdm_ctx+V86C_DS]
        mov     [edi+RMCS_DS],ax
        mov     eax,dword ptr [vdm_ctx+V86C_FS]
        mov     [edi+RMCS_FS],ax
        mov     eax,dword ptr [vdm_ctx+V86C_GS]
        mov     [edi+RMCS_GS],ax
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        mov     [edi+RMCS_IP],ax
        mov     eax,dword ptr [vdm_ctx+V86C_CS]
        mov     [edi+RMCS_CS],ax
        mov     eax,dword ptr [vdm_ctx+V86C_ESP]
        mov     [edi+RMCS_SP],ax
        mov     eax,dword ptr [vdm_ctx+V86C_SS]
        mov     [edi+RMCS_SS],ax

;--- a window on the real-mode stack, for DS:ESI ---------------------
        mov     ebx,dword ptr [vdm_ctx+V86C_SS]
        shl     ebx,4
        mov     ax,(DPMI_IDX_RMS SHL 3) + DPMI_SELBITS
        mov     ecx,0FFFFh
        mov     dl,DACC_DATA
        mov     dh,byte ptr ds:[dpmi_gran]
        call    dpmi_setdesc

;--- the IRET frame the procedure will return through ----------------
; Words for a 16-bit client and dwords for a 32-bit one: what pops
; this is the client's own IRET, and its width is the width of the
; segment the procedure lives in.
        mov     edi,dword ptr ds:[dpmi_cbbase]
        mov     eax,DPMI_CBSTK - 16
        cmp     byte ptr ds:[dpmi_is32],0
        jne     dcl_32
        sub     eax,6
        mov     word ptr [edi+eax],DPMICB_R3RET
        mov     word ptr [edi+eax+2],(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        mov     word ptr [edi+eax+4],2
        jmp     short dcl_go
dcl_32:
        sub     eax,12
        mov     dword ptr [edi+eax],DPMICB_R3RET
        mov     dword ptr [edi+eax+4],(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        mov     dword ptr [edi+eax+8],2
dcl_go:
        mov     dword ptr ds:[dpmi_cbesp3],eax
        mov     eax,dword ptr [esi+DCB_RMCS]
        mov     dword ptr ds:[dpmi_cbrmcs],eax
        mov     dword ptr ds:[dpmi_cbact],1

        ; THE RING-0 STACK HAS TO BE WHERE WE ARE STANDING, and not
        ; DPMI_KSTACK.  The client's own INT 31h frame and the parked
        ; simulated interrupt are on that stack ABOVE this point, and
        ; DPMI_KSTACK is its top - so anything that interrupts the
        ; procedure would start again up there and write over both.
        mov     dword ptr ds:[dpmi_cbesp],esp
        mov     dword ptr ds:[TSS_BASE + TSS_ESP0],esp

        ; Everything the IRETD needs goes into registers first: the
        ; last two instructions before it load DS and ES, and after
        ; that there is no kernel data left to read.
        mov     ecx,dword ptr [esi+DCB_PROC]
        mov     edx,dword ptr [esi+DCB_PSEL]
        mov     ebx,dword ptr [esi+DCB_RSEL]
        mov     edi,dword ptr [esi+DCB_ROFF]
        movzx   esi,word ptr [vdm_ctx+V86C_ESP]

        cli
        push    (DPMI_IDX_CBSS SHL 3) + DPMI_SELBITS
        push    dword ptr ds:[dpmi_cbesp3]
        push    EFL_IF OR 2
        push    edx
        push    ecx
        mov     ax,(DPMI_IDX_RMS SHL 3) + DPMI_SELBITS
        mov     ds,ax                   ; DS:ESI - the real-mode stack
        mov     es,bx                   ; ES:EDI - the register structure
        xor     ax,ax
        mov     fs,ax
        mov     gs,ax
        iretd

;--- nobody to call.  Step over the HLT and let the RETF behind it
;--- take the caller back, which is what the pad is shaped like for.
dcl_refuse:
        inc     dword ptr [vdm_ctx+V86C_EIP]
        jmp     vdm_loop

;=====================================================================
; dpmi_cbret - the callback procedure has IRETed onto the stub.
;
; Reached from dpmi_int31, which recognised the CS the INT came from.
; It leaves the same way dpmi_rmdone does: what the procedure put in
; the register structure becomes the guest's, ESP goes back to where
; the monitor was standing when the call arrived, and the V86 loop
; carries on - at whatever CS:IP the procedure named, on whatever
; stack it named.  That is what makes an IRET out of a callback a
; RETURN and not a resumption.
;=====================================================================
dpmi_cbret:
        cmp     dword ptr ds:[dpmi_cbact],0
        je      d31_badval              ; nobody was called: the client got
                                        ; here on its own account
        mov     dword ptr ds:[dpmi_cbact],0
        mov     esi,dword ptr ds:[dpmi_cbrmcs]

        mov     eax,[esi+RMCS_EDI]
        mov     dword ptr [vdm_ctx+V86C_EDI],eax
        mov     eax,[esi+RMCS_ESI]
        mov     dword ptr [vdm_ctx+V86C_ESI],eax
        mov     eax,[esi+RMCS_EBP]
        mov     dword ptr [vdm_ctx+V86C_EBP],eax
        mov     eax,[esi+RMCS_EBX]
        mov     dword ptr [vdm_ctx+V86C_EBX],eax
        mov     eax,[esi+RMCS_EDX]
        mov     dword ptr [vdm_ctx+V86C_EDX],eax
        mov     eax,[esi+RMCS_ECX]
        mov     dword ptr [vdm_ctx+V86C_ECX],eax
        mov     eax,[esi+RMCS_EAX]
        mov     dword ptr [vdm_ctx+V86C_EAX],eax
        movzx   eax,word ptr [esi+RMCS_FLAGS]
        mov     dword ptr [vdm_ctx+V86C_FLAGS],eax
        movzx   eax,word ptr [esi+RMCS_ES]
        mov     dword ptr [vdm_ctx+V86C_ES],eax
        movzx   eax,word ptr [esi+RMCS_DS]
        mov     dword ptr [vdm_ctx+V86C_DS],eax
        movzx   eax,word ptr [esi+RMCS_FS]
        mov     dword ptr [vdm_ctx+V86C_FS],eax
        movzx   eax,word ptr [esi+RMCS_GS]
        mov     dword ptr [vdm_ctx+V86C_GS],eax
        movzx   eax,word ptr [esi+RMCS_IP]
        mov     dword ptr [vdm_ctx+V86C_EIP],eax
        movzx   eax,word ptr [esi+RMCS_CS]
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        movzx   eax,word ptr [esi+RMCS_SP]
        mov     dword ptr [vdm_ctx+V86C_ESP],eax
        movzx   eax,word ptr [esi+RMCS_SS]
        mov     dword ptr [vdm_ctx+V86C_SS],eax

        mov     esp,dword ptr ds:[dpmi_cbesp]
        sti                             ; we came in through a gate, and the
        jmp     vdm_loop                ; monitor is not an interrupt handler

;=====================================================================
; dpmi_reflect - a protected-mode interrupt with no handler of its own.
;
; The stubs below push the vector and come here.  If the client has
; installed a protected-mode handler through 0205h it is called; if
; not, the interrupt is run in real mode with the client's registers,
; which is what the specification says an unhandled vector does.
;=====================================================================
; The stub's PUSH happens AFTER the CPU has built the interrupt frame,
; so the vector ends up BELOW it - between the saved EBP and the
; return address.  Everything above the general registers is therefore
; one dword further out than in int21_entry's frame, and gets its own
; names rather than a silent +4 on the shared ones.
RF_VEC          EQU     44
RF_EIP          EQU     48
RF_CS           EQU     52
RF_FLAGS        EQU     56
RF_ESP3         EQU     60
RF_SS3          EQU     64

dpmi_reflect:
        push    ds
        push    es
        push    ebp
        pushad
        mov     ebp,esp
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     es,ax
        cld

        mov     eax,dword ptr [ebp+RF_VEC]
        mov     byte ptr ds:[dpmi_rmint],al

        cmp     dword ptr ds:[dpmi_active],0
        je      dre_nobody

        ; INT 2Fh AX=1686h has to be answered HERE and not reflected.
        ; It asks "am I in protected mode?", the answer is yes, and
        ; the real-mode handler this would otherwise go to is in no
        ; position to say so - it would report the mode it is itself
        ; running in, which is the opposite one.
        cmp     al,2Fh
        jne     dre_pmvec
        cmp     word ptr [ebp+F_EAX],1686h
        jne     dre_pmvec
        mov     word ptr [ebp+F_EAX],0          ; zero: yes, you are
        jmp     dre_out

dre_pmvec:
        ; a handler of the client's own?
        movzx   ebx,al
        shl     ebx,3
        add     ebx,OFFSET dpmi_pmvec
        cmp     word ptr [ebx+4],0
        jne     dre_client

;--- build a register structure out of the client's registers --------
        mov     edi,OFFSET dpmi_refcs
        mov     eax,dword ptr [ebp+F_EDI]
        mov     [edi+RMCS_EDI],eax
        mov     eax,dword ptr [ebp+F_ESI]
        mov     [edi+RMCS_ESI],eax
        mov     eax,dword ptr [ebp+F_EBP]
        mov     [edi+RMCS_EBP],eax
        mov     eax,dword ptr [ebp+F_EBX]
        mov     [edi+RMCS_EBX],eax
        mov     eax,dword ptr [ebp+F_EDX]
        mov     [edi+RMCS_EDX],eax
        mov     eax,dword ptr [ebp+F_ECX]
        mov     [edi+RMCS_ECX],eax
        mov     eax,dword ptr [ebp+F_EAX]
        mov     [edi+RMCS_EAX],eax
        xor     eax,eax                         ; segments and stack: ours
        mov     [edi+RMCS_ES],ax
        mov     [edi+RMCS_DS],ax
        mov     [edi+RMCS_FS],ax
        mov     [edi+RMCS_GS],ax
        mov     [edi+RMCS_SS],ax
        mov     [edi+RMCS_SP],ax
        mov     [edi+RMCS_FLAGS],ax

        mov     dword ptr ds:[dpmi_rmcs],edi
        mov     word ptr ds:[dpmi_rmwords],0
        call    dpmi_simint
        jc      dre_out

        mov     edi,OFFSET dpmi_refcs
        mov     eax,[edi+RMCS_EDI]
        mov     dword ptr [ebp+F_EDI],eax
        mov     eax,[edi+RMCS_ESI]
        mov     dword ptr [ebp+F_ESI],eax
        mov     eax,[edi+RMCS_EBX]
        mov     dword ptr [ebp+F_EBX],eax
        mov     eax,[edi+RMCS_EDX]
        mov     dword ptr [ebp+F_EDX],eax
        mov     eax,[edi+RMCS_ECX]
        mov     dword ptr [ebp+F_ECX],eax
        mov     eax,[edi+RMCS_EAX]
        mov     dword ptr [ebp+F_EAX],eax
        movzx   eax,word ptr [edi+RMCS_FLAGS]
        and     eax,FL_CF
        and     dword ptr [ebp+RF_FLAGS],NOT FL_CF
        or      dword ptr [ebp+RF_FLAGS],eax

dre_out:
        popad
        pop     ebp
        pop     es
        pop     ds
        add     esp,4                           ; the vector the stub pushed
        iretd

;--- the client installed its own handler: go there ------------------
; Its handler returns with an IRET, so it needs an IRET frame of its
; own on the client's stack.  We are still on the ring-0 stack, so the
; way to arrange that is to rewrite the frame we will IRETD through.
dre_client:
        mov     eax,dword ptr [ebx]
        mov     dword ptr [ebp+RF_EIP],eax
        movzx   eax,word ptr [ebx+4]
        mov     dword ptr [ebp+RF_CS],eax
        jmp     short dre_out

;--- nothing is running: this was a real exception, not a service ----
dre_nobody:
        mov     esi,OFFSET msgDpmiStray
        call    dword ptr ds:[IOS_CONSTR]
        movzx   eax,byte ptr ds:[dpmi_rmint]
        call    vdm_hex8
        mov     esi,OFFSET msgDpmiCR
        call    dword ptr ds:[IOS_CONSTR]
        jmp     short dre_out

;---------------------------------------------------------------------
; The stubs.  One per vector, because the CPU does not tell a handler
; which gate it came through and the alternative is 256 of them.
;---------------------------------------------------------------------
dst_10: push    10h
        jmp     dpmi_reflect
dst_11: push    11h
        jmp     dpmi_reflect
dst_12: push    12h
        jmp     dpmi_reflect
dst_13: push    13h
        jmp     dpmi_reflect
dst_14: push    14h
        jmp     dpmi_reflect
dst_15: push    15h
        jmp     dpmi_reflect
dst_16: push    16h
        jmp     dpmi_reflect
dst_17: push    17h
        jmp     dpmi_reflect
dst_1A: push    1Ah
        jmp     dpmi_reflect
dst_2F: push    2Fh
        jmp     dpmi_reflect
dst_33: push    33h
        jmp     dpmi_reflect

dpmi_veclist:
        db      10h
        dd      OFFSET dst_10
        db      11h
        dd      OFFSET dst_11
        db      12h
        dd      OFFSET dst_12
        db      13h
        dd      OFFSET dst_13
        db      14h
        dd      OFFSET dst_14
        db      15h
        dd      OFFSET dst_15
        db      16h
        dd      OFFSET dst_16
        db      17h
        dd      OFFSET dst_17
        db      1Ah
        dd      OFFSET dst_1A
        db      2Fh
        dd      OFFSET dst_2F
        db      33h
        dd      OFFSET dst_33
        db      0

;=====================================================================
; dpmi_xlat - a client's INT 21h, made to look like a kernel one.
;
; Called from int21_entry when the caller's CS is not the kernel's.
; The kernel's handlers want flat pointers; a client passes
; selector:offset, and which registers are which is the same question
; the DOS box bridge answers - so it is the same table.
;
; The originals are put back on the way out (dpmi_unxlat), because a
; client that passed DS:DX expects DX to still be DX afterwards.
;=====================================================================
dpmi_xlat:
        pushad
        mov     al,byte ptr [ebp+F_EAX+1]       ; AH: the function
        mov     ah,byte ptr [ebp+F_EAX]         ; AL: the subfunction
        call    vdm_flags
        mov     byte ptr ds:[dpmi_pf],bl
        or      bl,bl
        jz      dxl_done

        mov     eax,dword ptr [ebp+F_EDX]
        mov     dword ptr ds:[dpmi_odx],eax
        mov     eax,dword ptr [ebp+F_ESI]
        mov     dword ptr ds:[dpmi_osi],eax
        mov     eax,dword ptr [ebp+F_EDI]
        mov     dword ptr ds:[dpmi_odi],eax
        mov     eax,dword ptr [ebp+F_EBX]
        mov     dword ptr ds:[dpmi_obx],eax

        test    byte ptr ds:[dpmi_pf],P_DX
        jz      dxl_nodx
        ; The client's own DS and ES, which int21_entry saved in
        ; the frame before it replaced them.
        mov     ax,word ptr [ebp+F_DS]
        call    dpmi_base
        jc      dxl_bad
        mov     eax,dword ptr [ebp+F_EDX]
        call    dpmi_off
        add     eax,ebx
        mov     dword ptr [ebp+F_EDX],eax
dxl_nodx:
        test    byte ptr ds:[dpmi_pf],P_SI
        jz      dxl_nosi
        mov     ax,word ptr [ebp+F_DS]
        call    dpmi_base
        jc      dxl_bad
        mov     eax,dword ptr [ebp+F_ESI]
        call    dpmi_off
        add     eax,ebx
        mov     dword ptr [ebp+F_ESI],eax
dxl_nosi:
        test    byte ptr ds:[dpmi_pf],P_DI
        jz      dxl_nodi
        mov     ax,word ptr [ebp+F_ES]          ; and the client's ES
        call    dpmi_base
        jc      dxl_bad
        mov     eax,dword ptr [ebp+F_EDI]
        call    dpmi_off
        add     eax,ebx
        mov     dword ptr [ebp+F_EDI],eax
dxl_nodi:
        test    byte ptr ds:[dpmi_pf],P_BX
        jz      dxl_done
        mov     ax,word ptr [ebp+F_ES]
        call    dpmi_base
        jc      dxl_bad
        mov     eax,dword ptr [ebp+F_EBX]
        call    dpmi_off
        add     eax,ebx
        mov     dword ptr [ebp+F_EBX],eax
dxl_done:
        ; AND THE SAME MEASUREMENT THE DOS BOX GETS.  A client's
        ; pointer arrives as selector:offset and comes out of
        ; dpmi_base as whatever base that descriptor carries, so
        ; unlike a guest's segment:offset there is no arithmetic
        ; confining it to the first megabyte - it can name the kernel
        ; directly.  One byte again; the length-carrying calls are
        ; measured inside the handlers, where the length is.
        call    dpmi_ptrchk
        jc      dxl_bad
        popad
        clc
        ret
;--- Whatever had already been converted goes back before the error
;--- does.  A refusal must leave the client's registers as it wrote
;--- them: the call did not happen, and a flat kernel address in DX is
;--- both a wrong answer and one the client had no way to ask for.
dxl_bad:
        test    byte ptr ds:[dpmi_pf],P_DX
        jz      dxb_nodx
        mov     eax,dword ptr ds:[dpmi_odx]
        mov     dword ptr [ebp+F_EDX],eax
dxb_nodx:
        test    byte ptr ds:[dpmi_pf],P_SI
        jz      dxb_nosi
        mov     eax,dword ptr ds:[dpmi_osi]
        mov     dword ptr [ebp+F_ESI],eax
dxb_nosi:
        test    byte ptr ds:[dpmi_pf],P_DI
        jz      dxb_nodi
        mov     eax,dword ptr ds:[dpmi_odi]
        mov     dword ptr [ebp+F_EDI],eax
dxb_nodi:
        test    byte ptr ds:[dpmi_pf],P_BX
        jz      dxb_nobx
        mov     eax,dword ptr ds:[dpmi_obx]
        mov     dword ptr [ebp+F_EBX],eax
dxb_nobx:
        popad
        stc
        ret

;---------------------------------------------------------------------
; dpmi_ptrchk - the converted pointers, back out of the frame and
;               measured against the kernel's own memory.  CF if any
;               of them is in it.  EBP still addresses the frame here,
;               which is why this runs before the popad.
;---------------------------------------------------------------------
dpmi_ptrchk:
        push    eax
        push    ecx
        mov     ecx,1
        test    byte ptr ds:[dpmi_pf],P_DX
        jz      dpc_nodx
        mov     eax,dword ptr [ebp+F_EDX]
        call    buf_ok
        jc      dpc_bad
dpc_nodx:
        test    byte ptr ds:[dpmi_pf],P_SI
        jz      dpc_nosi
        mov     eax,dword ptr [ebp+F_ESI]
        call    buf_ok
        jc      dpc_bad
dpc_nosi:
        test    byte ptr ds:[dpmi_pf],P_DI
        jz      dpc_nodi
        mov     eax,dword ptr [ebp+F_EDI]
        call    buf_ok
        jc      dpc_bad
dpc_nodi:
        test    byte ptr ds:[dpmi_pf],P_BX
        jz      dpc_good
        mov     eax,dword ptr [ebp+F_EBX]
        call    buf_ok
        jc      dpc_bad
dpc_good:
        pop     ecx
        pop     eax
        clc
        ret
dpc_bad:
        pop     ecx
        pop     eax
        stc
        ret

;--- dpmi_off - EAX = an offset, narrowed to what the client can mean
dpmi_off:
        cmp     byte ptr ds:[dpmi_is32],0
        jne     dof_out
        movzx   eax,ax
dof_out:
        ret

;--- dpmi_unxlat - the registers the client passed, back as it left
;--- them.  Only the ones that were converted, and only if the handler
;--- did not put an answer in them.
dpmi_unxlat:
        pushad
        mov     bl,byte ptr ds:[dpmi_pf]
        test    bl,P_DX
        jz      dux_nodx
        mov     eax,dword ptr ds:[dpmi_odx]
        mov     dword ptr [ebp+F_EDX],eax
dux_nodx:
        test    bl,P_SI
        jz      dux_nosi
        mov     eax,dword ptr ds:[dpmi_osi]
        ; ...except 29h, whose whole answer is how far SI got.  AH is
        ; still the function number: the parse writes AL and nothing
        ; else, which is what makes this recognisable at all.
        cmp     byte ptr [ebp+F_EAX+1],29h
        jne     dux_si
        add     eax,dword ptr ds:[p29_adv]
dux_si:
        mov     dword ptr [ebp+F_ESI],eax
dux_nosi:
        test    bl,P_DI
        jz      dux_nodi
        mov     eax,dword ptr ds:[dpmi_odi]
        mov     dword ptr [ebp+F_EDI],eax
dux_nodi:
        test    bl,P_BX
        jz      dux_done
        mov     eax,dword ptr ds:[dpmi_obx]
        mov     dword ptr [ebp+F_EBX],eax
dux_done:
        popad
        ret

;=====================================================================
; dpmi_exit - the client has called 21h/4Ch, or something has killed
;             it.  AL = the exit code.
;
; A DPMI client IS the program in the DOS box, so its ending is the
; box's ending.  We are somewhere inside an IDT handler on the ring-0
; stack; the way back to the box's own loop is the anchor vdm_run
; left, which is exactly what a ^C uses.
;=====================================================================
dpmi_exit:
        cli
        movzx   eax,al
        mov     dword ptr ds:[dpmi_exitcode],eax
        call    dpmi_cbclr              ; its callbacks go with it: the
                                        ; addresses stay, and answer nobody
        xor     eax,eax
        lldt    ax                              ; no client, no table
        mov     dword ptr ds:[dpmi_active],0
        mov     dword ptr ds:[dpmi_rmact],0
        mov     esp,dword ptr ds:[vdm_esp]
        mov     eax,dword ptr ds:[vdm_indos]    ; the client's own 4Ch frame
        mov     byte ptr ds:[DOS_INDOS],al      ; went with the stack
        sti
        mov     eax,dword ptr ds:[dpmi_exitcode]
        mov     dword ptr ds:[vdm_exit],eax
        mov     dword ptr ds:[vdm_extype],VDX_NORMAL
        jmp     vdm_done

;=====================================================================
; dpmi_fault - a ring-3 client faulted on something PMIO could not
;              emulate.  EAX = vector, EDX = error code, ECX = CS:EIP's
;              offset, EBX = its selector.
;
; Called from pm3_fault with the fault frame still intact, so this
; never returns: the client is finished either way.
;=====================================================================
dpmi_fault:
        mov     dword ptr ds:[dpmi_fvec],eax
        mov     dword ptr ds:[dpmi_ferr],edx
        mov     dword ptr ds:[dpmi_feip],ecx
        mov     dword ptr ds:[dpmi_fcs],ebx

        mov     esi,OFFSET msgDpmiFault
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,dword ptr ds:[dpmi_fvec]
        call    vdm_hex8
        mov     esi,OFFSET msgDpmiAt
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,dword ptr ds:[dpmi_fcs]
        call    vdm_hex4
        mov     al,':'
        call    dword ptr ds:[IOS_CONOUT]
        mov     eax,dword ptr ds:[dpmi_feip]
        call    vdm_hex32
        mov     esi,OFFSET msgDpmiErr
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,dword ptr ds:[dpmi_ferr]
        call    vdm_hex8
        mov     esi,OFFSET msgDpmiCR
        call    dword ptr ds:[IOS_CONSTR]

        cmp     dword ptr ds:[dpmi_active],0
        je      dfa_stray

        ; The client is finished, and so is the box it was.  This is
        ; the same unwind a ^C does, for the same reason: we are deep
        ; inside a fault handler on a stack that is about to stop
        ; meaning anything.
        call    dpmi_cbclr
        cli
        xor     eax,eax
        lldt    ax
        mov     dword ptr ds:[dpmi_active],0
        mov     dword ptr ds:[dpmi_rmact],0
        mov     esp,dword ptr ds:[vdm_esp]
        mov     eax,dword ptr ds:[vdm_indos]
        mov     byte ptr ds:[DOS_INDOS],al
        sti
        mov     dword ptr ds:[vdm_exit],0
        mov     dword ptr ds:[vdm_extype],VDX_CRITERR
        jmp     vdm_done
dfa_stray:
        mov     esi,OFFSET msgDpmiNoClient
        call    dword ptr ds:[IOS_PANIC]

;=====================================================================
; Data
;=====================================================================
dpmi_on         db  1                   ; CONFIG.SYS DPMI=ON|OFF
dpmi_is32       db  0                   ; the running client's word size
dpmi_gran       db  DGRAN_16
dpmi_rmint      db  0
                db  0
dpmi_cds        dw  0                   ; the client's own DS and ES, taken
dpmi_ces        dw  0                   ; at the door of every INT 31h
dpmi_active     dd  0                   ; a client is in protected mode
dpmi_rmact      dd  0                   ; ...and inside a simulated interrupt
dpmi_rmesp      dd  0                   ; where dpmi_simint must come back to
dpmi_rmcs       dd  0                   ; the register structure it is using
dpmi_rmwords    dw  0
dpmi_stkseg     dw  0                   ; the scratch real-mode stack
dpmi_cbseg      dw  0                   ; and the callback landing pads
dpmi_cbact      dd  0                   ; a callback procedure is running
dpmi_cbesp      dd  0                   ; where the monitor was standing
dpmi_cbesp3     dd  0                   ; and where the procedure's stack is
dpmi_cbrmcs     dd  0                   ; the structure it was handed
dpmi_cbbase     dd  0                   ; the flat base of that stack, 0 until
                                        ; the first callback asks for one
dpmi_cseg       dd  0                   ; the client's real-mode CS ...
dpmi_ceip       dd  0                   ; ... and where the switch returns to
dpmi_cesp       dd  0
dpmi_r3eip      dd  0                   ; where dpmi_toring3 is going
dpmi_r3esp      dd  0
dpmi_exitcode   dd  0
dpmi_segcnt     dd  0
dpmi_first      dd  0                   ; scratch for the allocators below
dpmi_hand       dd  0
dpmi_msize      dd  0
dpmi_dseg       dd  0
dpmi_dsel       dd  0
dpmi_fvec       dd  0                   ; what the fault reporter was told
dpmi_ferr       dd  0
dpmi_feip       dd  0
dpmi_fcs        dd  0
dpmi_pf         db  0                   ; the pointer flags of the call in hand
                db  0,0,0
dpmi_odx        dd  0                   ; and what the client had in those
dpmi_osi        dd  0                   ; registers before we converted them
dpmi_odi        dd  0
dpmi_obx        dd  0
dpmi_segmap     dw  (DPMI_SEGMAX * 2) dup(0)    ; segment, selector

dpmi_cbtab      db  (DPMI_MAXCB * DCB_LEN) dup(0)
dpmi_pmvec      dd  (256 * 2) dup(0)    ; offset, then selector, per vector
dpmi_memtab     db  (DPMI_MAXMEM * DM_LEN) dup(0)
dpmi_refcs      db  RMCS_LEN dup(0)
dpmi_savectx    db  V86C_SIZE dup(0)

msgDpmiFault    db  13,10,'DPMI client fault ',0
msgDpmiAt       db  ' at ',0
msgDpmiErr      db  ' err ',0
msgDpmiCR       db  13,10,0
msgDpmiStray    db  13,10,'PM-DOS: protected-mode interrupt with no client: ',0
msgDpmiNoClient db  'a ring-3 fault with no DPMI client running',0
