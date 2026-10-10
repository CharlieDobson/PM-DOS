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
;   (0306h, the raw mode switch, IS here now - see dpmi_rawrm.  It
;   was left out on the grounds that a client changing mode itself
;   wants ring 0, which is wrong: it wants the HOST to change mode for
;   it, at an address the host chose.  PKZIP and PKUNZIP 2.04g jump
;   through that address without checking 0306h's carry, and died on
;   the garbage it left in BX:CX and SI:DI.)
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
                                                ; which has no state to save
        mov     byte ptr [edi+DPMICB_RAWRM],0F4h        ; HLT - and 0306h's two
        mov     byte ptr [edi+DPMICB_RAWPM],0F4h        ; mode switches
        mov     word ptr [edi+DPMICB_EXCRET],31CDh      ; INT 31h - an exception
        mov     word ptr [edi+DPMICB_EXCDEF],31CDh      ; handler's way back, and
                                                        ; the default handler
        mov     word ptr [edi+DPMICB_MSDOS],31CDh       ; INT 31h, RETF - the
        mov     byte ptr [edi+DPMICB_MSDOS+2],0CBh      ; "MS-DOS" API
di_nocb:

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

        ; THE CODE SEGMENT IS A 16-BIT ONE WHATEVER THE CLIENT IS.  The
        ; switch returns to the instruction after a far call made in
        ; real mode, which is 16-bit code and does not stop being it:
        ; the specification gives every client a 16-bit CS over its
        ; real-mode one, and a 32-bit client builds its 32-bit segments
        ; itself from there.  This one followed the client's size, so
        ; the first instruction a 32-bit client executed in protected
        ; mode was decoded as something else.
        mov     ebx,dword ptr ds:[dpmi_cseg]
        shl     ebx,4
        mov     ax,(DPMI_IDX_CS SHL 3) + DPMI_SELBITS
        mov     ecx,0FFFFh
        mov     dl,DACC_CODE
        mov     dh,DGRAN_16
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

        ; THE ENVIRONMENT WORD IN THE PSP BECOMES A SELECTOR, as the
        ; specification says it does, and goes back to the segment it
        ; was when the client ends (dpmi_drop) - DOS frees the block by
        ; it.  DOS/4GW and DJGPP's start-up both read the environment
        ; through PSP:2Ch, loaded 0556h into a segment register, and
        ; died there: "General Protection Fault in DOOM.EXE", and
        ; Quake's SIGSEGV.  The limit is the block's own size, from the
        ; arena header in front of it, or 64K where there is none.
        movzx   esi,word ptr ds:[vdm_pspseg]
        shl     esi,4
        mov     dword ptr ds:[dpmi_envpsp],0
        movzx   ebx,word ptr [esi+2Ch]
        or      ebx,ebx
        jz      dsw_noenv
        mov     dword ptr ds:[dpmi_envseg],ebx
        mov     dword ptr ds:[dpmi_envpsp],esi
        lea     edi,[ebx-1]
        shl     edi,4
        mov     ecx,0FFFFh
        cmp     byte ptr [edi],'M'
        je      dsw_envmcb
        cmp     byte ptr [edi],'Z'
        jne     dsw_envlim
dsw_envmcb:
        movzx   eax,word ptr [edi+3]
        shl     eax,4
        jz      dsw_envlim
        cmp     eax,10000h
        ja      dsw_envlim
        lea     ecx,[eax-1]
dsw_envlim:
        shl     ebx,4
        mov     ax,(DPMI_IDX_ENV SHL 3) + DPMI_SELBITS
        mov     dl,DACC_DATA
        mov     dh,DGRAN_16
        call    dpmi_setdesc
        mov     word ptr [esi+2Ch],(DPMI_IDX_ENV SHL 3) + DPMI_SELBITS
dsw_noenv:

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

        ; And the one every vector starts in - see DPMI_IDX_DEFVEC.  It
        ; is over the host's own image, which costs no memory below
        ; 1MB, and follows the client's size for what it has to pop:
        ; a handler that jumps here has left its caller's frame on
        ; the stack in the width it was given it.
        mov     ebx,OFFSET dpmi_defstubs
        mov     ax,DPMI_SEL_DEFVEC
        mov     ecx,DST_END - 1
        mov     dl,DACC_CODE
        mov     dh,byte ptr ds:[dpmi_gran]
        call    dpmi_setdesc

        call    dpmi_cbclr                      ; a new client holds none

        ; ...and no handlers.  The last client's were selectors in a
        ; table that has just been wiped.
        mov     edi,OFFSET dpmi_pmvec
        xor     eax,eax
        mov     ecx,256 * 2
        cld
        rep     stosd
        mov     edi,OFFSET dpmi_exc             ; exception handlers too
        mov     ecx,DPMI_MAXEXC * 2
        rep     stosd
        mov     word ptr ds:[dpmi_dtasel],0     ; and no DTA of its own yet

        ; AND 0002h REMEMBERS NOTHING, for the same reason.  It keeps
        ; the selector it made for a segment, so that asking twice gets
        ; the same one - and kept it for the next client, which asked
        ; for the BIOS data area, was given the selector the last one
        ; had, and faulted loading a descriptor that was eight zeros.
        ; The second DPMI program run since boot, if both used 0002h.
        mov     dword ptr ds:[dpmi_segcnt],0

        ; SELECTOR 0040h IS THE BIOS DATA AREA, for as long as the client
        ; runs.  Windows' host gives every client that, extenders count
        ; on it, and DOS/4GW Professional reads the clock as PUSH 40h /
        ; POP ES / ES:[6Eh] - which here is GDT entry 8, SEL_PROG_DS, a
        ; native program's slot: Rise of the Triad page-faulted reading
        ; one.  No native program runs while a client does (the shell is
        ; waiting in its 4Bh), so the entry is lent out and dpmi_drop
        ; gives it back (r3_setdesc).
        mov     edi,GDT_BASE + GDT_IDX_PROGDS * 8
        mov     ebx,400h
        mov     ecx,0FFFFh
        mov     dl,DACC_DATA
        xor     dh,dh
        call    r3_wrdesc

;--- and go ----------------------------------------------------------
        mov     dword ptr ds:[dpmi_active],1
        mov     eax,dword ptr ds:[vdm_depth]    ; (vdm_rmleave: whose 4Ch ends it)
        mov     dword ptr ds:[dpmi_depth],eax
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
        mov     es,ax                           ; ES -> the PSP, as promised
        xor     ax,ax
        mov     fs,ax
        mov     gs,ax

        ; AND EVERY OTHER REGISTER IS THE CLIENT'S, which the
        ; specification says in as many words: the switch changes the
        ; segment registers and nothing else.  The client FAR CALLed
        ; us from inside a routine of its own, with BP its frame
        ; pointer; these used to arrive holding whatever the kernel had
        ; in them last.  PKUNZIP 2.04g reads its argument list off
        ; [bp+6] after the switch, found nothing there it could use,
        ; and showed its usage screen for any command line at all.
        mov     eax,dword ptr ss:[vdm_ctx+V86C_EAX]
        mov     ebx,dword ptr ss:[vdm_ctx+V86C_EBX]
        mov     ecx,dword ptr ss:[vdm_ctx+V86C_ECX]
        mov     edx,dword ptr ss:[vdm_ctx+V86C_EDX]
        mov     esi,dword ptr ss:[vdm_ctx+V86C_ESI]
        mov     edi,dword ptr ss:[vdm_ctx+V86C_EDI]
        mov     ebp,dword ptr ss:[vdm_ctx+V86C_EBP]
        push    (DPMI_IDX_DS SHL 3) + DPMI_SELBITS
        pop     ds
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
        ; The same segment holds the two stubs an EXCEPTION handler
        ; comes back through: its RETF, and the default handler it may
        ; have chained to.  Told apart by where the INT 31h was.
        cmp     word ptr [ebp+F_CS],(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        jne     d31_notcb
        cmp     dword ptr [ebp+F_EIP],DPMICB_EXCRET + 2
        je      dpmi_excret
        cmp     dword ptr [ebp+F_EIP],DPMICB_EXCDEF + 2
        je      dpmi_excdef
        cmp     dword ptr [ebp+F_EIP],DPMICB_MSDOS + 2
        je      dpmi_msapi
        jmp     dpmi_cbret
d31_notcb:

        ; A VECTOR'S DEFAULT HANDLER has been reached, by the same
        ; test: every one of them is an INT 31h in a segment nothing
        ; else is in.
        cmp     word ptr [ebp+F_CS],DPMI_SEL_DEFVEC
        je      dpmi_defvec

        ; And a client may have a handler of its own on INT 31h, which
        ; an extender that adds functions to it does.  Its handler
        ; comes first and the host is where that one sends the rest.
        cmp     word ptr ds:[dpmi_pmvec + 31h * 8 + 4],0
        je      d31_own
        lea     esi,[ebp+F_EIP]
        mov     ebx,OFFSET dpmi_pmvec + 31h * 8
        call    dpmi_enter
        jnc     d31_return
d31_own:
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
        cmp     ax,0202h
        je      d31_getexc
        cmp     ax,0203h
        je      d31_setexc
        cmp     ax,0204h
        je      d31_getpmvec
        cmp     ax,0205h
        je      d31_setpmvec
        cmp     ax,0300h
        je      d31_simint
        cmp     ax,0301h
        je      d31_rmcall
        cmp     ax,0302h
        je      d31_rmcall
        cmp     ax,0303h
        je      d31_cballoc
        cmp     ax,0304h
        je      d31_cbfree
        cmp     ax,0305h
        je      d31_savest
        cmp     ax,0306h
        je      d31_rawaddr
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
        cmp     ax,0800h
        je      d31_physmap
        cmp     ax,0A00h
        je      d31_vendor
        cmp     ax,0801h
        je      d31_ok                          ; nothing is ever unmapped

        mov     ax,DPE_UNSUPPORTED
;--- the two ways out ------------------------------------------------
d31_fail:
        mov     word ptr [ebp+F_EAX],ax
        or      dword ptr [ebp+F_FLAGS],FL_CF
        jmp     short d31_return
d31_ok:
        and     dword ptr [ebp+F_FLAGS],NOT FL_CF
d31_return:
        ; DS AND ES ARE RELOADED HERE, AT RING 0, from what the client
        ; had when it called - and the call may have been the one that
        ; took the descriptor away.  DJGPP's exit frees the selector its
        ; own DS holds (0001h), and the POP DS below faulted the KERNEL:
        ; Quake ended with "CPU exception 0D".  A register naming
        ; something that is no longer a loadable segment comes back
        ; null, which faults the client if it is ever used, and only
        ; then.  The host's own selectors (TI clear) are left alone.
        mov     ax,word ptr [ebp+F_DS]
        test    al,4
        jz      d31_dsok
        mov     bl,2
        call    dpmi_rawsel
        jnc     d31_dsok
        mov     word ptr [ebp+F_DS],0
d31_dsok:
        mov     ax,word ptr [ebp+F_ES]
        test    al,4
        jz      d31_esok
        mov     bl,2
        call    dpmi_rawsel
        jnc     d31_esok
        mov     word ptr [ebp+F_ES],0
d31_esok:
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
        call    dpmi_seg2sel
        jc      d31_nodesc
        mov     word ptr [ebp+F_EAX],ax
        jmp     d31_ok

;--- dpmi_seg2sel - EBX = a real-mode segment -> EAX = a selector over
;--- it, 64K of data, and the same one every time it is asked for.  CF
;--- if no descriptor is to be had.  (0002h, and dpmi_post21's answers.)
dpmi_seg2sel:
        push    ebx
        push    ecx
        push    edx
        push    esi
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
        jae     d2s_full
        push    ebx
        mov     ecx,1
        call    dpmi_getfree
        pop     ebx
        jc      d2s_full
        shl     eax,3
        or      eax,DPMI_SELBITS
        push    eax                             ; the selector
        push    ebx                             ; and the segment
        mov     ecx,0FFFFh
        shl     ebx,4
        mov     dl,DACC_DATA
        mov     dh,DGRAN_16
        call    dpmi_setdesc
        pop     ebx
        pop     eax
        mov     ecx,dword ptr ds:[dpmi_segcnt]
        mov     esi,OFFSET dpmi_segmap
        lea     esi,[esi+ecx*4]
        mov     [esi],bx
        mov     [esi+2],ax
        inc     dword ptr ds:[dpmi_segcnt]
        movzx   eax,ax
        jmp     d2s_out
d2s_found:
        movzx   eax,word ptr [esi+2]
d2s_out:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
d2s_full:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

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
        ; THE DESCRIPTOR'S BASE, PRESENT OR NOT.  dpmi_base will not
        ; follow one that is not present - right for a pointer about to
        ; be used, wrong for a question about the descriptor.  A not-
        ; present one keeps whatever 0007h put in it, and Borland's RTM
        ; keeps a mark there ("TT" in the high word) that its #NP handler
        ; reads back with this call: refused, it decided every fault was
        ; somebody else's and passed it on.
        mov     ax,word ptr [ebp+F_EBX]
        call    dpmi_slot
        jc      d31_badsel
        movzx   ebx,byte ptr [edi+7]
        shl     ebx,8
        mov     bl,[edi+4]
        shl     ebx,16
        mov     bx,[edi+2]
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
        ; BUT PRESENT ONLY IF IT SAYS SO.  A descriptor that is not
        ; present is the client's own business and no danger to anyone:
        ; loading it is a #NP, which goes to the client's handler
        ; (0203h).  It is how Borland's RTM loads a segment the first
        ; time it is touched, keeping its own notes in the base field
        ; meanwhile - this used to make it present, and TPX wrote
        ; through a "base" of 545404A8h.
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
        mov     ecx,8
        call    dpmi_esdiw
        jc      d31_badsel
        mov     ecx,2
        rep     movsd
        jmp     d31_ok

d31_setdesc:
        mov     ax,word ptr [ebp+F_EBX]
        call    dpmi_slot
        jc      d31_badsel
        ; THE CLIENT'S BUFFER IS WHAT dpmi_esdi RETURNS, and this used
        ; to push EDI around the call and pop it straight back over the
        ; answer - so both pointers were our own descriptor and the
        ; copy below wrote it onto itself.  000Ch has never set
        ; anything.  DOS/4GW builds its data segment with it, loaded
        ; the zero-length descriptor 0000h had made, and faulted on
        ; its first write.
        mov     esi,edi                         ; ESI = ours
        call    dpmi_esdi                       ; EDI = theirs
        jc      d31_badsel
        xchg    esi,edi                         ; theirs onto ours
        ; A CODE OR DATA SEGMENT, AS 0009h ALREADY INSISTS.  Forcing the
        ; DPL to 3 is no protection for a GATE: a call gate at DPL 3 is
        ; one any ring-3 code may use, and it goes wherever its own
        ; selector says - SEL_CODE32, at whatever offset the client
        ; wrote.  One far call through it was ring 0.
        test    byte ptr [esi+5],10h
        jz      d31_badval
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
;
; WHAT 0204h ANSWERS FOR ONE OF THOSE IS AN ADDRESS THAT WORKS.  It
; answered the client's own code segment and offset zero, which is the
; first instruction of the client: a handler that passed on what it did
; not want - and that is what a handler does - restarted the program
; that had installed it.  It is the vector's place in the table of
; defaults now (DPMI_IDX_DEFVEC), and 0205h given that address back
; means what the client meant by it: no handler.
;---------------------------------------------------------------------
d31_getpmvec:
        movzx   eax,byte ptr [ebp+F_EBX]
        lea     ebx,[dpmi_pmvec + eax*8]
        cmp     word ptr [ebx+4],0
        jne     dgp_have
        shl     eax,1                           ; DST_LEN
        mov     dword ptr [ebp+F_EDX],eax
        mov     word ptr [ebp+F_ECX],DPMI_SEL_DEFVEC
        jmp     d31_ok
dgp_have:
        mov     eax,dword ptr [ebx]
        mov     dword ptr [ebp+F_EDX],eax
        mov     ax,word ptr [ebx+4]
        mov     word ptr [ebp+F_ECX],ax
        jmp     d31_ok

d31_setpmvec:
        movzx   ebx,byte ptr [ebp+F_EBX]
        shl     ebx,3
        add     ebx,OFFSET dpmi_pmvec
        mov     ax,word ptr [ebp+F_ECX]
        cmp     ax,DPMI_SEL_DEFVEC
        je      dsp_none
        call    dpmi_codeok                     ; somewhere an interrupt can go
        jc      d31_badsel
        mov     eax,dword ptr [ebp+F_EDX]
        call    dpmi_off
        mov     dword ptr [ebx],eax
        mov     ax,word ptr [ebp+F_ECX]
        mov     word ptr [ebx+4],ax
        jmp     d31_ok
dsp_none:
        mov     dword ptr [ebx],0
        mov     word ptr [ebx+4],0
        jmp     d31_ok

;---------------------------------------------------------------------
; 0300h - run a real-mode interrupt and come back with the registers
;---------------------------------------------------------------------
d31_simint:
        cmp     byte ptr [ebp+F_EBX+1],0        ; BH must be zero
        jne     d31_badval
        mov     ecx,RMCS_LEN                    ; the answer goes back into it
        call    dpmi_esdiw
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
; 0301h / 0302h - call a real-mode procedure, at the CS:IP in the
; structure at ES:(E)DI.  0301h's returns with a RETF and 0302h's with
; an IRET, so the frame each is given to return through differs by the
; flags; both end on the same HLT a simulated interrupt's IRET lands
; on, and everything else is 0300h's (dpmi_simint, dpmi_rmcall says
; which).  DOS/4GW reaches its real-mode half through 0302h, and gave
; up with exit code 8 when the host had neither.
;---------------------------------------------------------------------
d31_rmcall:
        cmp     byte ptr [ebp+F_EBX+1],0        ; BH must be zero
        jne     d31_badval
        mov     ecx,RMCS_LEN
        call    dpmi_esdiw
        jc      d31_badsel
        mov     dword ptr ds:[dpmi_rmcs],edi
        mov     al,DRC_RETF
        cmp     byte ptr [ebp+F_EAX],01h
        je      drc_kind
        mov     al,DRC_IRET
drc_kind:
        mov     byte ptr ds:[dpmi_rmcall],al
        mov     ax,word ptr [ebp+F_ECX]
        mov     word ptr ds:[dpmi_rmwords],ax
        call    dpmi_simint
        mov     byte ptr ds:[dpmi_rmcall],DRC_INT       ; (MOV keeps CF)
        jc      d31_badval
        jmp     d31_ok

;---------------------------------------------------------------------
; 0202h / 0203h - a processor exception's handler, BL = 00h-1Fh, in
; CX:(E)DX.  Until the client sets one, the answer is the default -
; the INT 31h at DPMICB_EXCDEF, which a handler that chains there
; reaches with the exception's frame still on its stack (dpmi_excdef).
;---------------------------------------------------------------------
d31_getexc:
        movzx   ebx,byte ptr [ebp+F_EBX]
        cmp     ebx,DPMI_MAXEXC
        jae     d31_badval
        cmp     word ptr ds:[dpmi_cbseg],0
        je      d31_nocb
        mov     eax,dword ptr [dpmi_exc + ebx * 8]
        mov     edx,dword ptr [dpmi_exc + ebx * 8 + 4]
        or      edx,edx
        jnz     dge_have
        mov     eax,DPMICB_EXCDEF
        mov     edx,(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
dge_have:
        mov     word ptr [ebp+F_ECX],dx
        mov     word ptr [ebp+F_EDX],ax
        cmp     byte ptr ds:[dpmi_is32],0
        je      d31_ok
        mov     dword ptr [ebp+F_EDX],eax
        jmp     d31_ok

d31_setexc:
        movzx   ebx,byte ptr [ebp+F_EBX]
        cmp     ebx,DPMI_MAXEXC
        jae     d31_badval
        mov     ax,word ptr [ebp+F_ECX]
        movzx   edx,word ptr [ebp+F_EDX]
        cmp     byte ptr ds:[dpmi_is32],0
        je      dse_16
        mov     edx,dword ptr [ebp+F_EDX]
dse_16:
        ; The default handed back is a selector of the host's: putting
        ; it back means "no handler", and is stored that way.
        cmp     ax,(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        jne     dse_own
        xor     eax,eax
        xor     edx,edx
        jmp     dse_put
dse_own:
        push    edi
        call    dpmi_codeok                     ; one of its own code segments,
        pop     edi                             ; or no handler at all
        jc      d31_badsel
        movzx   eax,ax
dse_put:
        mov     dword ptr [dpmi_exc + ebx * 8],edx
        mov     dword ptr [dpmi_exc + ebx * 8 + 4],eax
        jmp     d31_ok

;---------------------------------------------------------------------
; 0A00h - a vendor's API entry point, named by DS:(E)SI.  See
;         dpmi_vendor: the one name this host answers to is "MS-DOS".
;---------------------------------------------------------------------
d31_vendor:
        call    dpmi_vendor
        jnc     d31_ok
        mov     ax,DPE_UNSUPPORTED
        jmp     d31_fail

;---------------------------------------------------------------------
; dpmi_vendor - DS:(E)SI in the frame at EBP is a vendor's name.  For
;               "MS-DOS", ES:(E)DI in the frame becomes the extension's
;               entry point and CF is clear; for anything else CF.
;
; THE "MS-DOS" EXTENSIONS ARE A PROMISE THAT INT 21h WORKS IN
; PROTECTED MODE, pointers and all, which on this host it always has:
; DOS here is protected-mode code (see the top of this file).  Windows
; and the hosts after it answer for it on INT 2Fh AX=168Ah and 0A00h,
; and Borland's RTM will not start without the answer - Turbo Pascal
; 7's TPX said "no DOS extensions in the DPMI server" and stopped.
; Its entry point has one function, the LDT's selector (dpmi_msapi).
;---------------------------------------------------------------------
dpmi_vendor:
        pushad
        ; NOT YET ANSWERED (dpmi_msdos is 0), and on purpose.  With the
        ; answer, RTM gets as far as loading TPX and then unloads itself
        ; the DOS way - 50h to its own resident PSP, then 4Ch, expecting
        ; DOS to resume the program that loaded it at the address in
        ; that PSP - and a DOS box keeps neither the terminate address
        ; nor the parent's stack in a PSP (RSP_TERM is INT 22h's
        ; vector, PSP:2Eh is never written).  It ended the wrong program
        ; and left RTM's real-mode hooks pointing at nothing, which broke
        ; every program after it in the session.  Without the answer RTM
        ; stops at once with "no DOS extensions in the DPMI server", as
        ; it always did.  Turn this on with the EXEC work that keeps
        ; those two fields.
        cmp     byte ptr ds:[dpmi_msdos],0
        je      dvn_no
        cmp     word ptr ds:[dpmi_cbseg],0
        je      dvn_no
        mov     ax,word ptr [ebp+F_DS]
        call    dpmi_base
        jc      dvn_no
        mov     esi,dword ptr [ebp+F_ESI]
        cmp     byte ptr ds:[dpmi_is32],0
        jne     dvn_32
        movzx   esi,si
dvn_32:
        add     esi,ebx
        mov     eax,esi                         ; seven bytes the client
        mov     ecx,7                           ; could have written: a
        call    dword ptr ds:[IOS_USRWOK]       ; name in a page that is not
        jc      dvn_no                          ; there faults the kernel
        mov     edi,OFFSET dvn_name
        mov     ecx,7
        repe    cmpsb
        jne     dvn_no
        mov     word ptr [ebp+F_ES],(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        mov     word ptr [ebp+F_EDI],DPMICB_MSDOS
        cmp     byte ptr ds:[dpmi_is32],0
        je      dvn_yes
        mov     word ptr [ebp+F_EDI+2],0
dvn_yes:
        popad
        clc
        ret
dvn_no:
        popad
        stc
        ret
dvn_name        db  'MS-DOS',0

;---------------------------------------------------------------------
; dpmi_msapi - the client has FAR CALLed the "MS-DOS" entry point, whose
;              INT 31h brought it here; the RETF behind it takes it
;              back.  AX = 0100h asks for a selector over the LDT,
;              which comes back read-only (DPMI_IDX_LDTA).  Nothing
;              else is defined.
;---------------------------------------------------------------------
dpmi_msapi:
        cmp     word ptr [ebp+F_EAX],0100h
        jne     dma_no
        mov     ebx,DPMI_LDT
        mov     ax,(DPMI_IDX_LDTA SHL 3) + DPMI_SELBITS
        mov     ecx,DPMI_LDT_LIMIT
        mov     dl,DACC_RODATA
        mov     dh,DGRAN_16
        call    dpmi_setdesc
        mov     word ptr [ebp+F_EAX],ax
        jmp     d31_ok
dma_no:
        mov     ax,DPE_UNSUPPORTED
        jmp     d31_fail

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
        mov     ecx,RMCS_LEN                    ; EDI = the structure, flat,
        call    dpmi_esdiw                      ; which every call fills in
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
; 0306h - the raw mode switch addresses.  BX:CX is where real-mode code
; jumps to get into protected mode, SI:(E)DI where protected-mode code
; jumps to get out.  Both are HLTs in the callback paragraph - the
; real-mode one reached as a segment, the protected-mode one through
; the descriptor over the same paragraph - and dpmi_rawrm/dpmi_rawpm
; below are what happens when somebody arrives.
;---------------------------------------------------------------------
d31_rawaddr:
        cmp     word ptr ds:[dpmi_cbseg],0
        je      d31_nocb
        mov     ax,word ptr ds:[dpmi_cbseg]
        mov     word ptr [ebp+F_EBX],ax                 ; BX:CX - real mode
        mov     word ptr [ebp+F_ECX],DPMICB_RAWPM
        mov     word ptr [ebp+F_ESI],(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        mov     word ptr [ebp+F_EDI],DPMICB_RAWRM       ; SI:(E)DI - protected
        cmp     byte ptr ds:[dpmi_is32],0
        je      d31_ok
        mov     word ptr [ebp+F_EDI+2],0
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
        ; DH and DL: where the first and the second interrupt
        ; controller's vectors start - the VIRTUAL ones, which is what
        ; the specification asks for and what a client hooks.  They
        ; are where a DOS program has always found them; IRQ_BASE is
        ; where the kernel keeps the real ones, and a client told that
        ; would hook vectors nothing is ever delivered to.
        mov     word ptr [ebp+F_EDX],0870h
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
        mov     ecx,30h
        call    dpmi_esdiw
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
        jnc     dmr_put

        ; AND WHERE IT CANNOT, IT MOVES: the specification gives the
        ; host that right and hands the client the new address in BX:CX.
        ; This refused instead, and DJGPP's sbrk grows its one block
        ; with 0503h - every size failed, down to nothing, and Quake
        ; said "Only -0.1 megs of memory reported".  A new block, the
        ; contents carried over (as much as both hold), the old one
        ; freed.  A client with a selector over the old address moves
        ; it itself; that is the bargain 0503h has always been.
        push    esi
        push    ebx
        call    arena_alloc                     ; EBX bytes -> EAX
        pop     ebx
        pop     esi
        jc      d31_nomem
        push    esi                             ; the entry
        push    eax                             ; the new block
        mov     ecx,dword ptr [esi+DM_SIZE]
        cmp     ecx,ebx
        jbe     dmr_len
        mov     ecx,ebx
dmr_len:
        mov     edx,dword ptr [esi+DM_ADDR]     ; the old block
        mov     esi,edx
        mov     edi,eax
        cld
        push    ecx
        shr     ecx,2
        rep     movsd
        pop     ecx
        and     ecx,3
        rep     movsb
        call    arena_free                      ; EDX: the old one goes
        pop     edx                             ; EDX: the new one
        pop     esi
        mov     dword ptr [esi+DM_ADDR],edx
dmr_put:
        mov     dword ptr [esi+DM_SIZE],ebx
        mov     eax,edx                         ; where it is now
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

;---------------------------------------------------------------------
; 0800h - physical address mapping: BX:CX = where a device's memory
; is, SI:DI = how much of it.  BX:CX comes back as the linear address
; to build a descriptor over, and it is the same number - PMIO maps
; device memory where it is (pg_mapphys).  The first megabyte is
; refused, as the specification says it must be: that is what 0002h
; is for.
;---------------------------------------------------------------------
d31_physmap:
        movzx   ebx,word ptr [ebp+F_EBX]
        shl     ebx,16
        mov     bx,word ptr [ebp+F_ECX]
        movzx   ecx,word ptr [ebp+F_ESI]
        shl     ecx,16
        mov     cx,word ptr [ebp+F_EDI]
        cmp     ebx,HMA_BASE
        jb      d31_badval
        or      ecx,ecx
        jz      d31_badval
        call    dword ptr ds:[IOS_MAPPHYS]
        jc      d31_nomem
        mov     word ptr [ebp+F_ECX],ax         ; BX:CX, high then low
        shr     eax,16
        mov     word ptr [ebp+F_EBX],ax
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
; dpmi_esdiw - dpmi_esdi, for a buffer the host is about to WRITE: ECX
;              = how many bytes.  CF as well unless the client could
;              have written every one of them itself.
;
; The client names the buffer and the host fills it - as ring 0, which
; a read-only page does not stop.  A descriptor over PMDOS.SYS and an
; 0500h would be forty-eight bytes of -1 written into the kernel.
;---------------------------------------------------------------------
dpmi_esdiw:
        call    dpmi_esdi
        jc      dew_out
        push    eax
        mov     eax,edi
        call    dword ptr ds:[IOS_USRWOK]
        pop     eax
dew_out:
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
        ; THE FLAGS IT GOES IN WITH are nothing, for a call the client
        ; made through 0300h, and the caller's own for one that is being
        ; passed on (dpmi_rmreflect): what comes back is then what the
        ; handler did to them, which for a card's interrupt is nothing
        ; at all and for a service is its answer.
        mov     eax,dword ptr ds:[dpmi_rmflags]
        and     eax,DPMI_ARITH
        or      eax,EFL_IF
        mov     dword ptr [vdm_ctx+V86C_FLAGS],eax
        mov     dword ptr ds:[dpmi_rmflags],0

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
        ; WHERE THE CLIENT SAID, and it said so in the structure: an
        ; SS:SP of 0100:0C40 and twenty words off its own stack would be
        ; a GDT of its making, written by us as ring 0.  Into memory it
        ; could write itself, or not at all.
        push    ecx
        add     ecx,ecx
        call    dword ptr ds:[IOS_USRWOK]
        pop     ecx
        jc      dsi_undo
        rep     movsw
dsi_nocopy:

;--- and point the guest at an INT instruction of the right number ---
; NOT at whatever the vector table holds.  When the monitor trapped
; INT 21h the table had no entry for it - reading one gave 0000:0000,
; and the guest went off to execute the vector table as code - and
; there are still vectors the monitor claims outright (13h, 67h).
; Executing the instruction sends the interrupt wherever it would go
; for any DOS program: to the kernel if the monitor claims it, and
; through the guest's own table, hooks and all, if it does not - which
; is where INT 21h goes now (vdm_stubs).
        cmp     byte ptr ds:[dpmi_rmcall],DRC_INT
        jne     dsi_proc
        movzx   edi,word ptr ds:[xms_cbseg]
        shl     edi,4
        mov     al,byte ptr ds:[dpmi_rmint]
        mov     byte ptr [edi+DPMICB_SIM+1],al

        movzx   eax,word ptr ds:[xms_cbseg]
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        mov     dword ptr [vdm_ctx+V86C_EIP],DPMICB_SIM
        jmp     dsi_go

;--- ...or at a procedure: 0301h and 0302h -----------------------------
; The frame it returns through goes on the stack under the words the
; client asked to have copied, and points at DPMICB_RET - the HLT the
; INT xx above is followed by - so the way back is the same one.  An
; IRET frame carries the flags in the structure, as 0300h's does.
dsi_proc:
        mov     eax,dword ptr [vdm_ctx+V86C_SS]
        shl     eax,4
        mov     edx,dword ptr [vdm_ctx+V86C_ESP]
        mov     ecx,4                           ; CS:IP
        cmp     byte ptr ds:[dpmi_rmcall],DRC_IRET
        jne     dsi_cs
        mov     ecx,6                           ; ...and the flags above them
dsi_cs:
        sub     dx,cx
        mov     dword ptr [vdm_ctx+V86C_ESP],edx
        movzx   edi,dx
        add     edi,eax
        mov     eax,edi
        call    dword ptr ds:[IOS_USRWOK]       ; the client's stack, or ours
        jc      dsi_undo
        mov     word ptr [edi],DPMICB_RET
        mov     ax,word ptr ds:[xms_cbseg]
        mov     word ptr [edi+2],ax
        mov     esi,dword ptr ds:[dpmi_rmcs]
        cmp     byte ptr ds:[dpmi_rmcall],DRC_IRET
        jne     dsi_target
        mov     ax,word ptr [esi+RMCS_FLAGS]
        mov     word ptr [edi+4],ax
dsi_target:
        movzx   eax,word ptr [esi+RMCS_CS]
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        movzx   eax,word ptr [esi+RMCS_IP]
        mov     dword ptr [vdm_ctx+V86C_EIP],eax
dsi_go:

;--- and let the DOS box's own loop run it ---------------------------
        mov     dword ptr ds:[dpmi_rmact],1
        mov     dword ptr ds:[dpmi_rmesp],esp
        ; AND WHERE RING 3 COMES IN, which the DOS box is about to move:
        ; v86_run points it at its own frame, below here, and nothing
        ; pointed it back.  The client's next call in began there, its
        ; next simulated interrupt put it lower still, and about a
        ; hundred of them walked the ring-0 stack down into the TSS:
        ; DPMIVEC on the kernel as it was gets through 80 calls to the
        ; BIOS and not through 120.
        mov     eax,dword ptr ds:[TSS_BASE + TSS_ESP0]
        mov     dword ptr ds:[dpmi_rmesp0],eax
        mov     eax,dword ptr ds:[vdm_depth]    ; and the level whose code it
        mov     dword ptr ds:[dpmi_rmdepth],eax ; is: a program deeper than this
        jmp     vdm_loop                        ; started inside it - vdm_anchor

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
        mov     eax,dword ptr ds:[dpmi_rmesp0]
        mov     dword ptr ds:[TSS_BASE + TSS_ESP0],eax

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
; THE RAW MODE SWITCH (0306h)
;
; The client jumps to the protected-mode address 0306h gave it with the
; real-mode registers it wants in AX (DS), CX (ES), DX:BX (SS:SP) and
; SI:DI (CS:IP), and the DOS box's own loop runs that - INT 21h and all,
; for as long as the client likes - until real-mode code jumps to the
; real-mode address with the protected-mode registers in the same
; places, and ring 3 is entered there afresh.
;
; NOTHING IS PARKED, and that is the design.  Real mode runs at the
; box loop's own level (vdm_esp), exactly where it would be had the
; client never left it, and protected mode is entered from there the
; way the first switch entered it (dpmi_toring3).  So neither half owes
; the other a stack frame, and the real-mode half may do anything a
; DOS program does - including going resident.  Borland's RTM does: it
; is the DPMI client, it ends with 31h from real mode, and the program
; that loaded it calls it later through INT 2Fh, whose handler switches
; back into the client.  (A switch that parked the protected-mode side
; and unwound to it had nowhere to come back to once 31h had gone.)
;
; Not from inside a simulated interrupt or a callback procedure: both
; of those ARE parked, on the stack the client's entries arrive on.
;
; The specification calls AX, BX, CX, DX, SI and DI undefined after a
; switch and keeps EBP; FS and GS come out zero.  The arithmetic flags
; cross as they are, and so does the interrupt flag the client
; believes in.
;=====================================================================
DWR_EDI         EQU     HK_REGS + 0             ; the client's registers, as
DWR_ESI         EQU     HK_REGS + 4             ; pm3_fault's PUSHAD left them
DWR_EBP         EQU     HK_REGS + 8
DWR_EBX         EQU     HK_REGS + 16
DWR_EDX         EQU     HK_REGS + 20
DWR_ECX         EQU     HK_REGS + 24
DWR_EAX         EQU     HK_REGS + 28

;---------------------------------------------------------------------
; dpmi_rawrm - protected to real.  From dpmi_fault, which recognised
;              the HLT at ring 3, with EBP on pm3_fault's frame.  It
;              does not return: the fault frame is left where it is,
;              and the box's loop carries on at its own level.
;---------------------------------------------------------------------
dpmi_rawrm:
        cmp     dword ptr ds:[dpmi_rmact],0     ; not from inside a simulated
        jne     dfa_real                        ; interrupt, or a callback
        cmp     dword ptr ds:[dpmi_cbact],0     ; procedure: those are parked
        jne     dfa_real                        ; on this very stack
        cld                                     ; (the client's DF came in)

        movzx   eax,word ptr [ebp+DWR_EAX]
        mov     dword ptr [vdm_ctx+V86C_DS],eax
        movzx   eax,word ptr [ebp+DWR_ECX]
        mov     dword ptr [vdm_ctx+V86C_ES],eax
        movzx   eax,word ptr [ebp+DWR_EDX]
        mov     dword ptr [vdm_ctx+V86C_SS],eax
        movzx   eax,word ptr [ebp+DWR_EBX]
        mov     dword ptr [vdm_ctx+V86C_ESP],eax
        movzx   eax,word ptr [ebp+DWR_ESI]
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        movzx   eax,word ptr [ebp+DWR_EDI]
        mov     dword ptr [vdm_ctx+V86C_EIP],eax
        xor     eax,eax
        mov     dword ptr [vdm_ctx+V86C_FS],eax
        mov     dword ptr [vdm_ctx+V86C_GS],eax
        mov     eax,[ebp+DWR_EAX]
        mov     dword ptr [vdm_ctx+V86C_EAX],eax
        mov     eax,[ebp+DWR_EBX]
        mov     dword ptr [vdm_ctx+V86C_EBX],eax
        mov     eax,[ebp+DWR_ECX]
        mov     dword ptr [vdm_ctx+V86C_ECX],eax
        mov     eax,[ebp+DWR_EDX]
        mov     dword ptr [vdm_ctx+V86C_EDX],eax
        mov     eax,[ebp+DWR_ESI]
        mov     dword ptr [vdm_ctx+V86C_ESI],eax
        mov     eax,[ebp+DWR_EDI]
        mov     dword ptr [vdm_ctx+V86C_EDI],eax
        mov     eax,[ebp+DWR_EBP]
        mov     dword ptr [vdm_ctx+V86C_EBP],eax

        mov     eax,dword ptr [ebp+HK_IRET+8]
        and     eax,DPMI_ARITH
        mov     edx,dword ptr ds:[dpmi_pvif]
        or      edx,edx
        jz      dwr_if
        cmp     byte ptr [edx],0
        je      dwr_flags
dwr_if:
        or      eax,EFL_IF
dwr_flags:
        mov     dword ptr [vdm_ctx+V86C_FLAGS],eax

        cli
        mov     esp,dword ptr ds:[vdm_esp]
        sti                                     ; we came in through a gate
        jmp     vdm_loop

;---------------------------------------------------------------------
; dpmi_israwpm - is this HLT 0306h's real-mode address?  CF if not.
;---------------------------------------------------------------------
dpmi_israwpm:
        push    eax
        movzx   eax,word ptr ds:[dpmi_cbseg]
        or      eax,eax
        jz      dwi_no
        cmp     dword ptr [vdm_ctx+V86C_CS],eax
        jne     dwi_no
        cmp     dword ptr [vdm_ctx+V86C_EIP],DPMICB_RAWPM
        jne     dwi_no
        pop     eax
        clc
        ret
dwi_no:
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; dpmi_rawpm - real to protected.  From vdm_halt, at the box loop's
;              level; ring 3 is entered from here, as dpmi_toring3 does
;              it, and nothing comes back through this routine.
;
; THE SELECTORS ARE LOOKED AT BEFORE ANYTHING MOVES.  The IRETD, and
; the segment loads in front of it, run in the kernel; a selector there
; that does not describe what it is loaded as faults ring 0.  A client
; naming one is ended here instead, where somebody can be told.
;---------------------------------------------------------------------
dpmi_rawpm:
        cmp     dword ptr ds:[dpmi_active],0
        je      dwp_none                        ; a jump nobody was told of
        cmp     dword ptr ds:[dpmi_rmact],0     ; and not out of a simulated
        jne     dwp_bad                         ; interrupt: its frame is on
        cmp     dword ptr ds:[dpmi_cbact],0     ; DPMI_KSTACK, which ring 3
        jne     dwp_bad                         ; is about to be given

        mov     ax,word ptr [vdm_ctx+V86C_ESI]  ; CS
        mov     bl,0
        call    dpmi_rawsel
        jc      dwp_bad
        mov     ax,word ptr [vdm_ctx+V86C_EDX]  ; SS
        mov     bl,1
        call    dpmi_rawsel
        jc      dwp_bad
        mov     ax,word ptr [vdm_ctx+V86C_EAX]  ; DS
        mov     bl,2
        call    dpmi_rawsel
        jc      dwp_bad
        mov     ax,word ptr [vdm_ctx+V86C_ECX]  ; ES
        call    dpmi_rawsel
        jc      dwp_bad

        ; Where it carries on in real mode next time, if it ever comes
        ; back here without a switch of its own: just past the HLT.
        inc     dword ptr [vdm_ctx+V86C_EIP]

        ; The interrupt flag it believes in, from real mode's.
        mov     edx,dword ptr ds:[dpmi_pvif]
        or      edx,edx
        jz      dwp_frame
        test    dword ptr [vdm_ctx+V86C_FLAGS],EFL_IF
        setnz   byte ptr [edx]
dwp_frame:
        cli
        mov     dword ptr ds:[TSS_BASE + TSS_ESP0],DPMI_KSTACK
        movzx   eax,word ptr [vdm_ctx+V86C_EDX]         ; SS
        or      al,3
        push    eax
        movzx   eax,word ptr [vdm_ctx+V86C_EBX]         ; (E)SP
        cmp     byte ptr ds:[dpmi_is32],0
        je      dwp_sp16
        mov     eax,dword ptr [vdm_ctx+V86C_EBX]
dwp_sp16:
        push    eax
        mov     eax,dword ptr [vdm_ctx+V86C_FLAGS]      ; the arithmetic
        and     eax,DPMI_ARITH                          ; flags, and IOPL 0
        or      eax,EFL_IF OR 2                         ; with IF, as for
        push    eax                                     ; any client
        movzx   eax,word ptr [vdm_ctx+V86C_ESI]         ; CS
        or      al,3
        push    eax
        movzx   eax,word ptr [vdm_ctx+V86C_EDI]         ; (E)IP
        cmp     byte ptr ds:[dpmi_is32],0
        je      dwp_ip16
        mov     eax,dword ptr [vdm_ctx+V86C_EDI]
dwp_ip16:
        push    eax

        ; DS and ES now, at ring 0 - an inter-privilege IRETD would
        ; null them (dpmi_toring3 says why) - and every general
        ; register as real mode left it.  DS last of all: after it there
        ; is no kernel data to read, only SS-relative.
        mov     ax,word ptr [vdm_ctx+V86C_ECX]
        or      al,3
        test    ax,0FFFCh
        jnz     dwp_es
        xor     eax,eax
dwp_es:
        mov     es,ax
        movzx   eax,word ptr [vdm_ctx+V86C_EAX]
        or      al,3
        test    ax,0FFFCh
        jnz     dwp_ds
        xor     eax,eax
dwp_ds:
        push    eax                                     ; for the POP DS
        xor     eax,eax
        mov     fs,ax
        mov     gs,ax
        mov     eax,dword ptr ss:[vdm_ctx+V86C_EAX]
        mov     ebx,dword ptr ss:[vdm_ctx+V86C_EBX]
        mov     ecx,dword ptr ss:[vdm_ctx+V86C_ECX]
        mov     edx,dword ptr ss:[vdm_ctx+V86C_EDX]
        mov     esi,dword ptr ss:[vdm_ctx+V86C_ESI]
        mov     edi,dword ptr ss:[vdm_ctx+V86C_EDI]
        mov     ebp,dword ptr ss:[vdm_ctx+V86C_EBP]
        pop     ds
        iretd

dwp_bad:
        mov     esi,OFFSET msgDpmiRaw
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,VDX_CRITERR
        jmp     vdm_killprog
dwp_none:
        mov     esi,OFFSET msgDpmiRawNone
        call    dword ptr ds:[IOS_CONSTR]
        jmp     vdm_abend

;---------------------------------------------------------------------
; dpmi_rawsel - AX = a selector a raw switch is about to load, BL =
;               what for: 0 CS, 1 SS, 2 DS or ES (which may be null).
;               CF if it is not a present, ring-3 descriptor of the
;               client's that can be loaded that way.
;---------------------------------------------------------------------
dpmi_rawsel:
        push    eax
        push    edi
        cmp     bl,2
        jne     dws_look
        test    ax,0FFFCh
        jz      dws_ok                          ; null loads, and faults
dws_look:                                       ; only when it is used
        call    dpmi_slot
        jc      dws_bad
        mov     al,byte ptr [edi+5]
        and     al,0F0h                         ; present, DPL 3, and code
        cmp     al,0F0h                         ; or data
        jne     dws_bad
        mov     al,byte ptr [edi+5]
        and     al,0Ah                          ; executable, and writable
        cmp     bl,1                            ; (data) or readable (code)
        je      dws_ss
        jb      dws_cs
        cmp     al,08h                          ; DS and ES: anything but code
        je      dws_bad                         ; that cannot be read
        jmp     short dws_ok
dws_cs:
        test    al,08h
        jz      dws_bad
        jmp     short dws_ok
dws_ss:
        cmp     al,02h                          ; writable data
        jne     dws_bad
dws_ok:
        pop     edi
        pop     eax
        clc
        ret
dws_bad:
        pop     edi
        pop     eax
        stc
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

        cmp     dword ptr ds:[dpmi_active],0
        je      dre_nobody

        ; INT 2Fh AX=168Ah is a question for the HOST, and a protected-
        ; mode one: real mode has never heard of it (dpmi_vendor).  A
        ; client with a handler of its own on 2Fh is asked first.
        cmp     dword ptr [ebp+RF_VEC],2Fh
        jne     dre_pass
        cmp     word ptr [ebp+F_EAX],168Ah
        jne     dre_pass
        cmp     word ptr [dpmi_pmvec + 2Fh * 8 + 4],0
        jne     dre_pass
        call    dpmi_vendor
        jc      dre_pass                        ; AL stays 8Ah: no such thing
        mov     byte ptr [ebp+F_EAX],0
        jmp     dre_out
dre_pass:
        mov     eax,dword ptr [ebp+RF_VEC]
        lea     esi,[ebp+RF_EIP]
        call    dpmi_pmint

dre_out:
        popad
        pop     ebp
        pop     es
        pop     ds
        add     esp,4                           ; the vector the stub pushed
        iretd

;--- nothing is running: this was a real exception, not a service ----
dre_nobody:
        mov     esi,OFFSET msgDpmiStray
        call    dword ptr ds:[IOS_CONSTR]
        movzx   eax,byte ptr [ebp+RF_VEC]
        call    vdm_hex8
        mov     esi,OFFSET msgDpmiCR
        call    dword ptr ds:[IOS_CONSTR]
        jmp     dre_out

;=====================================================================
; A CLIENT'S OWN HANDLERS, AND WHAT IS THERE WHEN IT HAS NONE.
;
; A client executes INT n.  If it has put a handler on n (0205h) that
; is where it goes, and if it has not the interrupt is run in real
; mode.  There are three doors that arrives through and one routine
; behind all of them:
;
;   the gates dpmi_reflect is behind, for the BIOS's vectors;
;   the gates for INT 21h and INT 31h, which are the host's own calls
;     and ask dpmi_enter whether the client wants them first;
;   and A GENERAL PROTECTION FAULT, for every other vector there is.
;     The table has no gate for them that ring 3 may use, so the
;     instruction faults, PMIO finds nothing to emulate and the DOS
;     side is told a client has died (dpmi_fault).  It had not: it had
;     called INT 60h.
;
; THE HANDLER IS GIVEN A FRAME, which is the part that was missing.
; dre_client changed the address the host was about to return to and
; nothing else, so the handler began on a stack holding whatever the
; client had last pushed and its IRET took three words of that for an
; address and a set of flags.  Nothing here tested a handler on a
; software interrupt, and nothing was run that installs one.
;
; The frame is on the client's stack because there is no other: the
; specification promises a handler one of the host's, and this host
; has never had one to give.  It is as wide as the client is, which is
; the specification's rule and the one the other end keeps: a 32-bit
; client's handler ends with an IRETD whatever segment it is in.
;
; AND A HANDLER PASSES ON WHAT IT DOES NOT WANT, to the address 0204h
; gave it before it installed itself.  That is dpmi_defvec, further
; down, and it ends by returning through the frame the handler was
; given - so the flags the service answered in are put in that frame,
; and the client's interrupt flag is put back to what the frame says
; it was.  An IRET of the client's own cannot do the second: at ring 3
; it leaves the real flag alone, and the one the client believes in
; is a byte of PMIO's.
;=====================================================================

;---------------------------------------------------------------------
; dpmi_pmint - AL = the vector a client has called, EBP -> its
;              registers (F_EDI to F_EAX), ESI -> the frame it will be
;              resumed through: EIP, CS, EFLAGS, ESP, SS, with EIP
;              already past the instruction.  Everything preserved.
;---------------------------------------------------------------------
dpmi_pmint:
        pushad
        movzx   eax,al
        lea     ebx,[dpmi_pmvec + eax*8]
        cmp     word ptr [ebx+4],0
        je      dpi_real
        call    dpmi_enter
        jnc     dpi_out
dpi_real:
        mov     edx,dword ptr [esi+8]
        call    dpmi_rmreflect
        jc      dpi_out
        and     eax,DPMI_ARITH
        and     dword ptr [esi+8],NOT DPMI_ARITH
        or      dword ptr [esi+8],eax
dpi_out:
        popad
        ret

;---------------------------------------------------------------------
; dpmi_swint - from dpmi_fault, which has been told a client faulted.
;              EBP -> what PMIO's hook is given (HK_*).  CF clear if it
;              was an INT n, which has been dealt with, and PMIO is to
;              resume the client; everything preserved either way.
;
; The error code of a fault on a gate names the vector and says it was
; the interrupt table that refused, and the instruction has to agree:
; an INT n is two bytes, CD and the number.
;---------------------------------------------------------------------
dpmi_swint:
        pushad
        mov     ax,word ptr [ebp+HK_IRET+4]
        call    dpmi_base
        jc      dsw_no
        mov     eax,dword ptr [ebp+HK_IRET]
        cmp     byte ptr [ebx+eax],0CDh
        jne     dsw_no
        movzx   eax,byte ptr [ebx+eax+1]
        lea     edx,[eax*8+2]
        cmp     edx,dword ptr [ebp+HK_ERR]
        jne     dsw_no
        add     dword ptr [ebp+HK_IRET],2
        lea     esi,[ebp+HK_IRET]
        push    ebp
        lea     ebp,[ebp+HK_REGS]
        call    dpmi_pmint
        pop     ebp
        popad
        clc
        ret
dsw_no:
        popad
        stc
        ret

;---------------------------------------------------------------------
; dpmi_codeok - AX = a selector.  CF clear and EDI -> its descriptor if
;               it is one of the client's, present, and code.
;---------------------------------------------------------------------
dpmi_codeok:
        call    dpmi_slot
        jc      dco_no
        push    eax
        mov     al,byte ptr [edi+5]
        and     al,98h                          ; present, not a system
        cmp     al,98h                          ; segment, executable
        pop     eax
        jne     dco_no
        clc
        ret
dco_no:
        stc
        ret

;---------------------------------------------------------------------
; dpmi_stack - ESI -> a frame the client will be resumed through.
;              -> EDX = the flat base of its stack, EDI = its stack
;              pointer, CH bit 6 set if that is ESP and not SP.  CF if
;              the selector is not a stack of the client's.
;---------------------------------------------------------------------
dpmi_stack:
        push    eax
        push    ebx
        mov     ax,word ptr [esi+16]
        call    dpmi_slot
        jc      dsk_no
        mov     ch,byte ptr [edi+6]
        call    dpmi_base
        jc      dsk_no
        mov     edx,ebx
        mov     edi,dword ptr [esi+12]
        test    ch,40h
        jnz     dsk_big
        movzx   edi,di
dsk_big:
        pop     ebx
        pop     eax
        clc
        ret
dsk_no:
        pop     ebx
        pop     eax
        stc
        ret

;--- dpmi_setsp - EDI = the stack pointer, back into the frame at ESI,
;--- as wide as CH says it is
dpmi_setsp:
        test    ch,40h
        jnz     dss_big
        mov     word ptr [esi+12],di
        ret
dss_big:
        mov     dword ptr [esi+12],edi
        ret

;---------------------------------------------------------------------
; dpmi_enter - EBX -> a vector's entry in dpmi_pmvec, ESI -> the frame
;              the client will be resumed through.  The frame is
;              pointed at the handler, and what it held goes on the
;              client's stack for the handler's IRET.  CF if that
;              cannot be done, with nothing changed; everything
;              preserved.
;
; The flags that go on the stack carry the client's OWN interrupt flag
; and not the machine's, which is always set: they are what the
; default handler puts back, and what a handler that wants to know
; whether its caller had interrupts off will look at.  The flag itself
; is left as it is.  This is a software interrupt and the client chose
; the moment.
;---------------------------------------------------------------------
dpmi_enter:
        pushad
        mov     ax,word ptr [ebx+4]
        call    dpmi_codeok
        jc      den_no
        call    dpmi_stack
        jc      den_no

        mov     eax,dword ptr [esi+8]
        and     eax,NOT EFL_IF
        mov     ebp,dword ptr ds:[dpmi_pvif]
        or      ebp,ebp
        jz      den_if
        cmp     byte ptr [ebp],0
        je      den_flags
den_if:
        or      eax,EFL_IF
den_flags:
        cmp     byte ptr ds:[dpmi_is32],0
        je      den_16
        cmp     edi,12
        jb      den_no
        sub     edi,12
        push    ecx                             ; CH: dpmi_setsp's width
        mov     ecx,12
        call    den_room
        pop     ecx
        jc      den_no
        mov     dword ptr [edx+edi+8],eax
        movzx   eax,word ptr [esi+4]
        mov     dword ptr [edx+edi+4],eax
        mov     eax,dword ptr [esi]
        mov     dword ptr [edx+edi],eax
        mov     eax,dword ptr [ebx]
        jmp     short den_go
den_16:
        cmp     edi,6
        jb      den_no
        sub     edi,6
        push    ecx
        mov     ecx,6
        call    den_room
        pop     ecx
        jc      den_no
        mov     word ptr [edx+edi+4],ax
        mov     ax,word ptr [esi+4]
        mov     word ptr [edx+edi+2],ax
        mov     eax,dword ptr [esi]
        mov     word ptr [edx+edi],ax
        movzx   eax,word ptr [ebx]
den_go:
        mov     dword ptr [esi],eax
        movzx   eax,word ptr [ebx+4]
        mov     dword ptr [esi+4],eax
        call    dpmi_setsp
        popad
        clc
        ret
den_no:
        popad
        stc
        ret

;--- den_room - EDX + EDI = where the frame goes, ECX = its size.  CF
;--- unless the client could have written it there itself: its SS can
;--- be a flat descriptor and its ESP anything, and the frame is a
;--- ring-0 write (IOS_USRWOK).  Everything preserved.
den_room:
        push    eax
        lea     eax,[edx + edi]
        call    dword ptr ds:[IOS_USRWOK]
        pop     eax
        ret

;---------------------------------------------------------------------
; dpmi_image - ESI -> the frame the client will be resumed through.
;              -> EDX = the flags in the frame on the client's stack,
;              which is left where it is.  CF if there is no stack to
;              look on.  Nothing else changed.
;---------------------------------------------------------------------
dpmi_image:
        push    ecx
        push    edi
        call    dpmi_stack
        jc      dim_out
        cmp     byte ptr ds:[dpmi_is32],0
        je      dim_16
        mov     edx,dword ptr [edx+edi+8]
        jmp     short dim_ok
dim_16:
        movzx   edx,word ptr [edx+edi+4]
dim_ok:
        clc
dim_out:
        pop     edi
        pop     ecx
        ret

;---------------------------------------------------------------------
; dpmi_leave - the other end: an IRET, done for a handler that ended
;              in the host.  EAX = the flags the service answered in,
;              ESI -> the frame the client will be resumed through.
;              The frame on the client's stack is taken off it and the
;              client goes back to where that says.  CF if what is
;              there is not a frame, with nothing changed; everything
;              preserved.
;
; The width is the client's, as it was when the frame was made.
;---------------------------------------------------------------------
dpmi_leave:
        pushad
        mov     ebp,eax
        and     ebp,DPMI_ARITH
        call    dpmi_stack
        jc      dlv_no
        cmp     byte ptr ds:[dpmi_is32],0
        je      dlv_16
        mov     ebx,dword ptr [edx+edi]         ; EIP
        mov     eax,dword ptr [edx+edi+4]       ; CS
        mov     edx,dword ptr [edx+edi+8]       ; EFLAGS
        add     edi,12
        jmp     short dlv_have
dlv_16:
        movzx   ebx,word ptr [edx+edi]
        movzx   eax,word ptr [edx+edi+2]
        movzx   edx,word ptr [edx+edi+4]
        add     edi,6
dlv_have:
        push    edi
        call    dpmi_codeok
        pop     edi
        jc      dlv_no
        mov     dword ptr [esi],ebx
        movzx   eax,ax
        mov     dword ptr [esi+4],eax
        call    dpmi_setsp

        mov     eax,dword ptr [esi+8]
        and     eax,NOT (DPMI_ARITH OR DPMI_DF)
        or      eax,ebp
        mov     ecx,edx
        and     ecx,DPMI_DF
        or      eax,ecx
        mov     dword ptr [esi+8],eax

        mov     ebp,dword ptr ds:[dpmi_pvif]
        or      ebp,ebp
        jz      dlv_out
        mov     byte ptr [ebp],0
        test    edx,EFL_IF
        jz      dlv_out
        mov     byte ptr [ebp],1
dlv_out:
        popad
        clc
        ret
dlv_no:
        popad
        stc
        ret

;---------------------------------------------------------------------
; dpmi_rmreflect - AL = a vector, EDX = the caller's flags, EBP -> the
;                  client's registers (F_EDI to F_EAX).  The interrupt
;                  is run in real mode with all of them and they are
;                  what it left; EAX = the flags it left.  CF if it
;                  could not be run, and EAX is then the flags it was
;                  given.  Nothing else changed.
;
; THE FLAGS GO IN AS WELL AS COMING OUT, and that is not for a service:
; it is for an interrupt nobody called.  A handler on a card's vector
; that passes the interrupt on ends here, the code it interrupted was
; between a compare and a jump, and the flags it gets back have to be
; the ones it had.  They are, if real mode is handed them and hands
; them back; zero going in, which is what this did on the first day,
; sent a loop waiting for the clock out of the loop.
;
; INT 2Fh AX=1686h is answered HERE and not reflected.  It asks "am I
; in protected mode?", the answer is yes, and the real-mode handler
; this would otherwise go to is in no position to say so - it would
; report the mode it is itself running in, which is the opposite one.
;---------------------------------------------------------------------
dpmi_rmreflect:
        push    edi
        mov     byte ptr ds:[dpmi_rmint],al
        cmp     al,2Fh
        jne     drr_run
        cmp     word ptr [ebp+F_EAX],1686h
        jne     drr_run
        mov     word ptr [ebp+F_EAX],0          ; zero: yes, you are
        mov     eax,edx
        pop     edi
        clc
        ret

drr_run:
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
        mov     dword ptr ds:[dpmi_rmflags],edx
        call    dpmi_simint
        jnc     drr_ran
        mov     dword ptr ds:[dpmi_rmflags],0
        mov     eax,edx
        stc
        jmp     short drr_out
drr_ran:
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
        clc
drr_out:
        pop     edi
        ret

;---------------------------------------------------------------------
; dpmi_defvec - from dpmi_int31: the INT 31h was one of the defaults'.
;               EBP -> the frame, which is int21_entry's.  Where it
;               was called from says which vector, or which of the
;               three other things behind the table (DST_*).
;---------------------------------------------------------------------
dpmi_defvec:
        mov     eax,dword ptr [ebp+F_EIP]
        cmp     eax,DST_PASS31
        je      d31_own                         ; the client's INT 31h, which
                                                ; its handler has passed on
        cmp     eax,DST_TAIL21
        je      ddv_tail
        cmp     eax,DST_TAIL31
        je      ddv_tail
        cmp     eax,DST_VECMAX
        ja      ddv_bad
        sub     eax,DST_LEN
        jb      ddv_bad
        test    al,1
        jnz     ddv_bad
        shr     eax,1

        ; What the caller's flags were is in the frame the handler was
        ; given, and whatever is done next is done with those.
        lea     esi,[ebp+F_EIP]
        call    dpmi_image                      ; -> EDX
        jc      ddv_bad

        ; The host's own two go round again, from an address that is
        ; not a vector's: the call is made from the table, by the
        ; instruction itself, and comes back to the INT 31h after it.
        cmp     al,21h
        jne     ddv_not21
        mov     dword ptr [ebp+F_EIP],DST_DO21
        jmp     short ddv_again
ddv_not21:
        cmp     al,31h
        jne     ddv_real
        mov     dword ptr [ebp+F_EIP],DST_DO31
ddv_again:
        and     edx,DPMI_ARITH
        and     dword ptr [ebp+F_FLAGS],NOT DPMI_ARITH
        or      dword ptr [ebp+F_FLAGS],edx
        jmp     d31_return

ddv_real:
        call    dpmi_rmreflect                  ; EAX = the flags, either way
        jmp     short ddv_pop
ddv_tail:
        mov     eax,dword ptr [ebp+F_FLAGS]     ; what the call answered in
ddv_pop:
        lea     esi,[ebp+F_EIP]
        call    dpmi_leave
        jnc     d31_return

        ; What is on the client's stack is not a frame, so there is
        ; nowhere to go back to.  It is reported as the fault it would
        ; have been a moment later.
ddv_bad:
        mov     eax,13
        xor     edx,edx
        mov     ecx,dword ptr [ebp+F_EIP]
        movzx   ebx,word ptr [ebp+F_CS]
        jmp     dfa_real

;---------------------------------------------------------------------
; dpmi_hook21 - from int21_entry, for a call that came out of a
;               client's descriptor table.  EBP -> the frame.  CF clear
;               if the client has a handler of its own on INT 21h: the
;               frame is pointed at it, and the call has not been made.
;               Everything preserved.
;
; Not for a call made from the table of defaults, which is that
; handler passing the call on.
;---------------------------------------------------------------------
dpmi_hook21:
        cmp     word ptr [ebp+F_CS],DPMI_SEL_DEFVEC
        je      dhk_no
        cmp     word ptr ds:[dpmi_pmvec + 21h * 8 + 4],0
        je      dhk_no
        push    ebx
        push    esi
        lea     esi,[ebp+F_EIP]
        mov     ebx,OFFSET dpmi_pmvec + 21h * 8
        call    dpmi_enter
        pop     esi
        pop     ebx
        ret
dhk_no:
        stc
        ret

;---------------------------------------------------------------------
; The defaults themselves.  Ring-3 code, in the host's image, reached
; through DPMI_IDX_DEFVEC and never from here.
;---------------------------------------------------------------------
dpmi_defstubs:
        dw      256 dup (31CDh)                 ; INT 31h, for each vector
        db      0CDh,21h,0CDh,31h               ; DST_DO21
        db      0CDh,31h,0CDh,31h               ; DST_DO31

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
; 33h is not in the table below: the gate belongs to PMIO's mouse driver
; (IO\MOUSE.INC), which serves native programs itself and jumps here
; for a client - so a client's INT 33h still reaches the box.
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
        test    byte ptr ds:[dpmi_pf],P_BXD
        jz      dxl_bxes
        mov     ax,word ptr [ebp+F_DS]  ; 7305h's packet, against DS
dxl_bxes:
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
; dpmi_pre21 - a client's INT 21h that is the HOST's to answer and not
;              DOS's.  CF if it has been answered (the frame holds it).
;
; 25h AND 35h ARE THE PROTECTED-MODE VECTORS, as 0205h and 0204h are:
; that is what the "MS-DOS" extensions promise (dpmi_vendor), and it is
; the only answer that is not dangerous.  DOS's own 25h sets a gate in
; the KERNEL'S table - to the client's code, translated to a flat
; address and entered at ring 0.  DOS/4GW never let a 25h through to
; the host (it hooks INT 21h itself); Borland's RTM does.
;=====================================================================
dpmi_pre21:
        push    eax
        mov     ax,word ptr [ebp+F_EAX]
        mov     word ptr ds:[dpmi_fn],ax        ; for dpmi_post21: AH may be an
        cmp     ah,25h                          ; answer by then
        je      dp2_set
        cmp     ah,35h
        je      dp2_get
        cmp     ah,50h
        je      dp2_setpsp
        pop     eax
        clc
        ret

        ; 50h TAKES THE SELECTOR 51h AND 62h HAND OUT (dpmi_post21), and
        ; sets the PSP a DOS box counts by, vdm_curpsp, as the box's own
        ; 50h does.  The kernel's took 0027h for a segment: RTM set its
        ; PSP that way, and its real-mode half read the "PSP" at 0027:0 -
        ; the interrupt vector table - and built a selector out of it.
        ; A value that is not a selector is a segment, as it always was.
dp2_setpsp:
        pushad
        mov     ax,word ptr [ebp+F_EBX]
        test    al,4
        jz      dp2_pspseg
        call    dpmi_base                       ; EBX = where it is
        jc      dp2_done
        test    bl,0Fh
        jnz     dp2_done                        ; not a paragraph: not a PSP
        cmp     ebx,110000h
        jae     dp2_done
        shr     ebx,4
        mov     ax,bx
dp2_pspseg:
        mov     word ptr ds:[vdm_curpsp],ax
        jmp     dp2_done
dp2_set:
        pushad
        movzx   ebx,byte ptr [ebp+F_EAX]
        lea     ebx,[dpmi_pmvec + ebx*8]
        mov     ax,word ptr [ebp+F_DS]
        cmp     ax,DPMI_SEL_DEFVEC
        je      dp2_none
        call    dpmi_codeok                     ; somewhere an interrupt can
        jc      dp2_done                        ; go, or nothing changes
        mov     eax,dword ptr [ebp+F_EDX]
        call    dpmi_off
        mov     dword ptr [ebx],eax
        mov     ax,word ptr [ebp+F_DS]
        mov     word ptr [ebx+4],ax
        jmp     dp2_done
dp2_none:
        mov     dword ptr [ebx],0
        mov     word ptr [ebx+4],0
        jmp     dp2_done
dp2_get:
        pushad
        movzx   eax,byte ptr [ebp+F_EAX]
        lea     ebx,[dpmi_pmvec + eax*8]
        cmp     word ptr [ebx+4],0
        jne     dp2_have
        shl     eax,1                           ; DST_LEN: its default
        mov     word ptr [ebp+F_ES],DPMI_SEL_DEFVEC
        jmp     dp2_bx
dp2_have:
        mov     cx,word ptr [ebx+4]
        mov     word ptr [ebp+F_ES],cx
        mov     eax,dword ptr [ebx]
dp2_bx:
        mov     word ptr [ebp+F_EBX],ax
        cmp     byte ptr ds:[dpmi_is32],0
        je      dp2_done
        mov     dword ptr [ebp+F_EBX],eax
dp2_done:
        popad
        pop     eax
        stc
        ret

;=====================================================================
; dpmi_post21 - and the calls whose ANSWER is an address.  DOS works out
;               segment:offset, or the kernel's own flat address, and a
;               program in protected mode can only follow a selector:
;               each comes back as one (dpmi_seg2sel, which answers the
;               same segment with the same selector every time).
;
;   1Ah        remembered, so that 2Fh can give back what was set
;   2Fh        ES:BX - the DTA, as the client named it
;   34h        ES:BX - the InDOS byte
;   52h        ES:BX - the list of lists a DOS box is shown
;   51h, 62h   BX - the PSP: its selector, if it is the client's own
;   1Bh, 1Ch   DS:BX - the media byte
;   63h, 5D06h DS:SI - the lead-byte table, the swappable data area
;
; Borland's RTM asks 34h and 5D06h before anything else, and was handed
; the kernel's numbers in the client's registers.
;=====================================================================
dpmi_post21:
        pushad
        mov     al,byte ptr ds:[dpmi_fn+1]
        cmp     al,1Ah
        je      dpo_1A
        test    dword ptr [ebp+F_FLAGS],FL_CF
        jnz     dpo_out                         ; a call that failed answered
        cmp     al,2Fh                          ; nothing
        je      dpo_2F
        cmp     al,34h
        je      dpo_34
        cmp     al,52h
        je      dpo_52
        cmp     al,51h
        je      dpo_psp
        cmp     al,62h
        je      dpo_psp
        cmp     al,1Bh
        je      dpo_dsbx
        cmp     al,1Ch
        je      dpo_dsbx
        cmp     al,63h
        je      dpo_dssi
        cmp     al,5Dh
        jne     dpo_out
        cmp     byte ptr ds:[dpmi_fn],06h
        je      dpo_dssi
dpo_out:
        popad
        ret

dpo_1A:
        mov     ax,word ptr [ebp+F_DS]
        mov     word ptr ds:[dpmi_dtasel],ax
        mov     eax,dword ptr [ebp+F_EDX]
        call    dpmi_off
        mov     dword ptr ds:[dpmi_dtaoff],eax
        mov     eax,dword ptr [dta_ptr]
        mov     dword ptr ds:[dpmi_dtaflat],eax
        jmp     dpo_out

dpo_2F:
        mov     eax,dword ptr [dta_ptr]
        cmp     word ptr ds:[dpmi_dtasel],0
        je      dpo_esbx
        cmp     eax,dword ptr ds:[dpmi_dtaflat]
        jne     dpo_esbx                        ; moved since, by someone else
        mov     ax,word ptr ds:[dpmi_dtasel]
        mov     word ptr [ebp+F_ES],ax
        mov     eax,dword ptr ds:[dpmi_dtaoff]
        jmp     dpo_bx

dpo_34:
        mov     eax,DOS_INDOS
dpo_esbx:                                       ; EAX = flat, below 1MB+64K
        call    dpo_sel                         ; -> EAX selector, EDX offset
        jc      dpo_out
        mov     word ptr [ebp+F_ES],ax
        mov     eax,edx
dpo_bx:
        mov     word ptr [ebp+F_EBX],ax
        cmp     byte ptr ds:[dpmi_is32],0
        je      dpo_out
        mov     dword ptr [ebp+F_EBX],eax
        jmp     dpo_out

dpo_52:
        call    vdm_lolfresh                    ; EAX = the copy's segment
        jc      dpo_out
        mov     ebx,eax
        call    dpmi_seg2sel
        jc      dpo_out
        mov     word ptr [ebp+F_ES],ax
        mov     eax,GLOL_PTR
        jmp     dpo_bx

dpo_psp:
        movzx   ebx,word ptr ds:[vdm_curpsp]
        mov     eax,(DPMI_IDX_PSP SHL 3) + DPMI_SELBITS
        cmp     bx,word ptr ds:[vdm_pspseg]
        je      dpo_pspsel
        call    dpmi_seg2sel
        jc      dpo_out
dpo_pspsel:
        mov     word ptr [ebp+F_EBX],ax
        jmp     dpo_out

dpo_dsbx:
        mov     eax,dword ptr [ebp+F_EBX]
        call    dpo_sel
        jc      dpo_out
        mov     word ptr [ebp+F_DS],ax
        mov     eax,edx
        jmp     dpo_bx
dpo_dssi:
        mov     eax,dword ptr [ebp+F_ESI]
        call    dpo_sel
        jc      dpo_out
        mov     word ptr [ebp+F_DS],ax
        mov     word ptr [ebp+F_ESI],dx
        cmp     byte ptr ds:[dpmi_is32],0
        je      dpo_out
        mov     dword ptr [ebp+F_ESI],edx
        jmp     dpo_out

;--- dpo_sel - EAX = a flat address in the first megabyte (and the 64K
;--- above it) -> EAX = a selector, EDX = the offset in it.  CF if it is
;--- not one, or no descriptor is to be had.
dpo_sel:
        cmp     eax,10FFF0h
        jae     dps_no
        push    ebx
        mov     edx,eax
        and     edx,0Fh
        mov     ebx,eax
        shr     ebx,4
        call    dpmi_seg2sel
        pop     ebx
        ret
dps_no:
        stc
        ret

;=====================================================================
; PROCESSOR EXCEPTIONS (0202h / 0203h)
;
; A fault the client has a handler for is not the end of it: the
; handler is entered on a locked stack of the host's, with a frame the
; specification lays out - where to RETF to, the error code, and the
; faulting CS:(E)IP, (E)FLAGS and SS:(E)SP, words for a 16-bit client
; and dwords for a 32-bit one.  It may change any of those, and RETFs;
; the RETF lands on an INT 31h (DPMICB_EXCRET) and the client carries
; on from what the frame says now.  A handler that does not want the
; fault jumps to the one 0202h gave it, which for a client that set
; none is DPMICB_EXCDEF: the frame is still on its stack, and the
; client is ended there with the fault reported as it always was.
;
; DOS/4GW sets handlers for every exception before it does anything
; else, and did not look at the carry when 0202h/0203h were missing:
; the error code they left in AX was the function number of its next
; thirty calls.
;=====================================================================

;---------------------------------------------------------------------
; dpmi_excgo - from dpmi_fault: EAX = the vector, EBP on pm3_fault's
;              frame.  CF if the client has no handler for it (or the
;              handler cannot be entered), with nothing changed;
;              otherwise the frame is pointed at the handler and
;              pm3_fault resumes ring 3 there.  Everything preserved.
;---------------------------------------------------------------------
dpmi_excgo:
        cmp     eax,DPMI_MAXEXC
        jae     deg_no
        cmp     dword ptr [dpmi_exc + eax * 8 + 4],0
        je      deg_no
        pushad
        mov     dword ptr ds:[dpmi_excvec],eax
        mov     ax,word ptr [dpmi_exc + eax * 8 + 4]
        mov     bl,0                            ; still a code segment of its
        call    dpmi_rawsel                     ; own: it may have changed it
        jc      deg_fail                        ; since 0203h
        call    dpmi_excstack
        jc      deg_fail

        ; Its top, or - for a fault taken on it, which is a handler
        ; faulting - under that handler's frame.
        mov     edi,DPMI_EXCSTK - 16
        mov     ax,word ptr [ebp+HK_IRET+16]
        or      al,3
        cmp     ax,(DPMI_IDX_EXCSS SHL 3) + DPMI_SELBITS
        jne     deg_top
        mov     edi,dword ptr [ebp+HK_IRET+12]
        cmp     edi,DPMI_EXCSTK
        ja      deg_fail
        cmp     edi,64
        jb      deg_fail                        ; out of room: no handler
deg_top:
        mov     esi,dword ptr ds:[dpmi_excbase]
        cmp     byte ptr ds:[dpmi_is32],0
        je      deg_16
        sub     edi,32
        add     esi,edi
        mov     dword ptr [esi],DPMICB_EXCRET
        mov     dword ptr [esi+4],(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        mov     eax,dword ptr [ebp+HK_ERR]
        mov     dword ptr [esi+8],eax
        mov     eax,dword ptr [ebp+HK_IRET]
        mov     dword ptr [esi+12],eax
        mov     eax,dword ptr [ebp+HK_IRET+4]
        mov     dword ptr [esi+16],eax
        mov     eax,dword ptr [ebp+HK_IRET+8]
        mov     dword ptr [esi+20],eax
        mov     eax,dword ptr [ebp+HK_IRET+12]
        mov     dword ptr [esi+24],eax
        mov     eax,dword ptr [ebp+HK_IRET+16]
        mov     dword ptr [esi+28],eax
        jmp     deg_enter
deg_16:
        sub     edi,16
        add     esi,edi
        mov     word ptr [esi],DPMICB_EXCRET
        mov     word ptr [esi+2],(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        mov     ax,word ptr [ebp+HK_ERR]
        mov     word ptr [esi+4],ax
        mov     ax,word ptr [ebp+HK_IRET]
        mov     word ptr [esi+6],ax
        mov     ax,word ptr [ebp+HK_IRET+4]
        mov     word ptr [esi+8],ax
        mov     ax,word ptr [ebp+HK_IRET+8]
        mov     word ptr [esi+10],ax
        mov     ax,word ptr [ebp+HK_IRET+12]
        mov     word ptr [esi+12],ax
        mov     ax,word ptr [ebp+HK_IRET+16]
        mov     word ptr [esi+14],ax
deg_enter:
        mov     eax,dword ptr ds:[dpmi_excvec]
        mov     edx,dword ptr [dpmi_exc + eax * 8]
        mov     dword ptr [ebp+HK_IRET],edx
        mov     edx,dword ptr [dpmi_exc + eax * 8 + 4]
        or      dl,3
        mov     dword ptr [ebp+HK_IRET+4],edx
        mov     dword ptr [ebp+HK_IRET+12],edi
        mov     dword ptr [ebp+HK_IRET+16],(DPMI_IDX_EXCSS SHL 3) + DPMI_SELBITS
        and     dword ptr [ebp+HK_IRET+8],NOT (EFL_TF OR EFL_NT)
        popad
        clc
        ret
deg_fail:
        popad
deg_no:
        stc
        ret

;---------------------------------------------------------------------
; dpmi_excstack - the locked stack and its selector, made the first time
;               and described again for each client (the table it is
;               in is wiped by every mode switch).  CF if no memory.
;---------------------------------------------------------------------
dpmi_excstack:
        pushad
        cmp     dword ptr ds:[dpmi_excbase],0
        jne     des_have
        mov     ebx,DPMI_EXCSTK
        call    arena_alloc
        jc      des_none
        mov     dword ptr ds:[dpmi_excbase],eax
des_have:
        mov     ebx,dword ptr ds:[dpmi_excbase]
        mov     ax,(DPMI_IDX_EXCSS SHL 3) + DPMI_SELBITS
        mov     ecx,DPMI_EXCSTK - 1
        mov     dl,DACC_DATA
        mov     dh,byte ptr ds:[dpmi_gran]
        call    dpmi_setdesc
        popad
        clc
        ret
des_none:
        popad
        stc
        ret

;---------------------------------------------------------------------
; dpmi_excret - the handler has RETFed onto DPMICB_EXCRET.  From
;               dpmi_int31: the rest of the frame is at its SS:(E)SP.
;
; The selectors in it are looked at before they are used, for
; dpmi_rawpm's reason: the IRETD that goes back to the client runs in
; the kernel, and a bad CS or SS there would be the kernel's fault.
;---------------------------------------------------------------------
dpmi_excret:
        mov     ax,word ptr [ebp+F_SS3]
        call    dpmi_base
        jc      dxr_bad
        mov     esi,dword ptr [ebp+F_ESP3]
        cmp     byte ptr ds:[dpmi_is32],0
        jne     dxr_32
        movzx   esi,si
        add     esi,ebx
        movzx   eax,word ptr [esi+2]            ; (E)IP
        mov     dword ptr ds:[dpmi_xeip],eax
        movzx   eax,word ptr [esi+4]            ; CS
        mov     dword ptr ds:[dpmi_xcs],eax
        movzx   eax,word ptr [esi+6]            ; (E)FLAGS
        mov     dword ptr ds:[dpmi_xfl],eax
        movzx   eax,word ptr [esi+8]            ; (E)SP
        mov     dword ptr ds:[dpmi_xesp],eax
        movzx   eax,word ptr [esi+10]           ; SS
        mov     dword ptr ds:[dpmi_xss],eax
        jmp     dxr_check
dxr_32:
        add     esi,ebx
        mov     eax,dword ptr [esi+4]
        mov     dword ptr ds:[dpmi_xeip],eax
        movzx   eax,word ptr [esi+8]
        mov     dword ptr ds:[dpmi_xcs],eax
        mov     eax,dword ptr [esi+12]
        mov     dword ptr ds:[dpmi_xfl],eax
        mov     eax,dword ptr [esi+16]
        mov     dword ptr ds:[dpmi_xesp],eax
        movzx   eax,word ptr [esi+20]
        mov     dword ptr ds:[dpmi_xss],eax
dxr_check:
        mov     ax,word ptr ds:[dpmi_xcs]
        mov     bl,0
        call    dpmi_rawsel
        jc      dxr_bad
        mov     ax,word ptr ds:[dpmi_xss]
        mov     bl,1
        call    dpmi_rawsel
        jc      dxr_bad

        mov     eax,dword ptr ds:[dpmi_xeip]
        mov     dword ptr [ebp+F_EIP],eax
        mov     eax,dword ptr ds:[dpmi_xcs]
        or      al,3
        mov     dword ptr [ebp+F_CS],eax
        mov     eax,dword ptr ds:[dpmi_xesp]
        mov     dword ptr [ebp+F_ESP3],eax
        mov     eax,dword ptr ds:[dpmi_xss]
        or      al,3
        mov     dword ptr [ebp+F_SS3],eax
        ; The flags a program may have: the arithmetic ones and DF,
        ; from the frame.  IF and IOPL stay the host's (pm3_vif is the
        ; client's interrupt flag, and nothing here touches it).
        mov     eax,dword ptr ds:[dpmi_xfl]
        and     eax,DPMI_ARITH OR EFL_DF
        mov     edx,dword ptr [ebp+F_FLAGS]
        and     edx,NOT (DPMI_ARITH OR EFL_DF OR EFL_TF OR EFL_NT)
        or      eax,edx
        mov     dword ptr [ebp+F_FLAGS],eax
        jmp     d31_return

        ; A frame that names something the client cannot be resumed on:
        ; reported as the exception it came from, and the client ends.
dxr_bad:
        mov     eax,dword ptr ds:[dpmi_excvec]
        xor     edx,edx
        mov     ecx,dword ptr [ebp+F_EIP]
        movzx   ebx,word ptr [ebp+F_CS]
        jmp     dfa_real

;---------------------------------------------------------------------
; dpmi_excdef - a handler chained to 0202h's default.  The whole frame
;               is still on its stack, return address and all; the
;               fault it describes is reported and the client ends,
;               which is what the fault would have done with no handler.
;---------------------------------------------------------------------
dpmi_excdef:
        mov     ax,word ptr [ebp+F_SS3]
        call    dpmi_base
        jc      dxr_bad
        mov     esi,dword ptr [ebp+F_ESP3]
        cmp     byte ptr ds:[dpmi_is32],0
        jne     dxd_32
        movzx   esi,si
        add     esi,ebx
        movzx   edx,word ptr [esi+4]            ; the error code
        movzx   ecx,word ptr [esi+6]            ; (E)IP
        movzx   ebx,word ptr [esi+8]            ; CS
        jmp     dxd_say
dxd_32:
        add     esi,ebx
        mov     edx,dword ptr [esi+8]
        mov     ecx,dword ptr [esi+12]
        movzx   ebx,word ptr [esi+16]
dxd_say:
        mov     eax,dword ptr ds:[dpmi_excvec]
        jmp     dfa_real

;=====================================================================
; dpmi_exit - the client has called 21h/4Ch, or something has killed
;             it.  AL = the exit code.
;
; A DPMI client IS a program in the DOS box, so its ending is that
; program's ending - and ONLY that program's.  This used to go straight
; to vdm_done, which ends the box: right for a client CMD.EXE started,
; and wrong for one a 16-bit COMMAND.COM started, whose 4Bh then never
; returned and whose box was left one level deeper than it thought.
; We are somewhere inside an IDT handler on the ring-0 stack; the way
; back to the box's own loop is the anchor vdm_run left.
;=====================================================================
dpmi_exit:
        cli
        movzx   eax,al
        mov     dword ptr ds:[dpmi_exitcode],eax
        call    dpmi_drop
        mov     esp,dword ptr ds:[vdm_esp]
        mov     byte ptr ds:[DOS_INDOS],0       ; the client's own 4Ch frame
        sti                                     ; went with the stack; at the
                                                ; top, vdm_done puts the
                                                ; shell's depth back
        mov     eax,dword ptr ds:[dpmi_exitcode]
        mov     dword ptr ds:[vdm_exit],eax
        mov     dword ptr ds:[vdm_extype],VDX_NORMAL
        jmp     vdm_exitprog

;---------------------------------------------------------------------
; dpmi_drop - the client is going: its callbacks, its descriptor table
;             and the host's idea that there is one.  EAX trashed.
;---------------------------------------------------------------------
dpmi_drop:
        call    dpmi_cbclr              ; its callbacks go with it: the
                                        ; addresses stay, and answer nobody
        ; The environment word back to the segment it was, if it still
        ; holds our selector - the sweep that frees the program's memory
        ; reads it next.
        mov     eax,dword ptr ds:[dpmi_envpsp]
        or      eax,eax
        jz      ddr_noenv
        cmp     word ptr [eax+2Ch],(DPMI_IDX_ENV SHL 3) + DPMI_SELBITS
        jne     ddr_noenv
        push    edx
        mov     edx,dword ptr ds:[dpmi_envseg]
        mov     word ptr [eax+2Ch],dx
        pop     edx
ddr_noenv:
        mov     dword ptr ds:[dpmi_envpsp],0
        call    r3_setdesc              ; GDT entry 8 back to the native slot
        xor     eax,eax
        lldt    ax                              ; no client, no table
        mov     dword ptr ds:[dpmi_active],0
        mov     dword ptr ds:[dpmi_rmact],0
        ret

;=====================================================================
; dpmi_fault - a ring-3 client faulted on something PMIO could not
;              emulate.  EAX = vector, EDX = error code, ECX = CS:EIP's
;              offset, EBX = its selector.
;
; Called from pm3_fault with the fault frame still intact.  It returns
; for one thing, which is not a fault at all: a client's INT n on a
; vector with no gate it may use (dpmi_swint).  For anything else the
; client is finished.
;=====================================================================
dpmi_fault:
        cmp     eax,16
        je      dfa_fperr
        cmp     dword ptr ds:[dpmi_active],0
        je      dfa_real
        cmp     eax,13
        jne     dfa_exc
        ; 0306h's protected-mode address, which is a HLT and so a #GP
        ; at ring 3.  Before anything is pushed: dpmi_rawpm returns
        ; from this stack.
        cmp     ebx,(DPMI_IDX_CBRET SHL 3) + DPMI_SELBITS
        jne     dfa_notraw
        cmp     ecx,DPMICB_RAWRM
        je      dpmi_rawrm
dfa_notraw:
        call    dpmi_swint
        jnc     dfa_back
        ; A REAL FAULT, and the client may have a handler for it
        ; (0203h).  Only when it has not is the client finished here.
dfa_exc:
        call    dpmi_excgo
        jc      dfa_real
dfa_back:
        ret

        ; THE COPROCESSOR'S ERROR IS THE CLIENT'S INTERRUPT 13, as it
        ; is a DOS box's (IO\FPU.INC): its handler on INT 75h if it has
        ; put one there, and the real-mode one if it has not.  It comes
        ; back to the instruction it was stopped at, which runs once
        ; the error has been cleared - and if nobody clears it, is
        ; stopped at again.  That is a client that has stopped, and the
        ; monitor's rule for a box is this one's for a client
        ; (vfe_fptime, in PMIO): nothing but this error at this address
        ; for two seconds, and it is ended while somebody can be told.
dfa_fperr:
        cmp     dword ptr ds:[dpmi_active],0
        je      dfa_real
        push    eax
        call    dword ptr ds:[IOS_TICKS]
        cmp     ecx,dword ptr ds:[dpmi_fpeip]
        jne     dfp_moved
        cmp     ebx,dword ptr ds:[dpmi_fpcs]
        jne     dfp_moved
        sub     eax,dword ptr ds:[dpmi_fpwhen]
        js      dfp_moved               ; midnight: the count began again
        cmp     eax,DPMI_FPTICKS
        pop     eax
        jae     dfa_real
        jmp     short dfp_send
dfp_moved:
        call    dword ptr ds:[IOS_TICKS]
        mov     dword ptr ds:[dpmi_fpwhen],eax
        mov     dword ptr ds:[dpmi_fpeip],ecx
        mov     dword ptr ds:[dpmi_fpcs],ebx
        pop     eax
dfp_send:
        pushad
        lea     esi,[ebp+HK_IRET]
        lea     ebx,[dpmi_pmvec + FPU_VEC * 8]
        cmp     word ptr [ebx+4],0
        je      dfp_rm
        call    dpmi_enter
        jc      dfp_rm
        mov     eax,dword ptr ds:[dpmi_pvif]    ; an interrupt's handler
        or      eax,eax                         ; starts with them off
        jz      dfp_out
        mov     byte ptr [eax],0
        jmp     short dfp_out
dfp_rm:
        ; What an interrupt found in the registers it leaves there, so
        ; the flags it came back with are not the client's business.
        mov     edx,dword ptr [esi+8]
        lea     ebp,[ebp+HK_REGS]
        mov     al,FPU_VEC
        call    dpmi_rmreflect
dfp_out:
        popad
        ret
dfa_real:
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
        ; A page fault says where it was reaching, as a box's report
        ; does (vdm_fault): "0E at x err 06" names the instruction and
        ; not the address, which is the half that finds the bug.
        cmp     dword ptr ds:[dpmi_fvec],14
        jne     dfa_nocr2
        mov     esi,OFFSET msgVdmCR2
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,cr2
        call    vdm_hex32
dfa_nocr2:
        mov     esi,OFFSET msgDpmiCR
        call    dword ptr ds:[IOS_CONSTR]

        cmp     dword ptr ds:[dpmi_active],0
        je      dfa_stray

        ; The client is finished, and so is the box it was.  This is
        ; the same unwind a ^C does, for the same reason: we are deep
        ; inside a fault handler on a stack that is about to stop
        ; meaning anything.
        cli
        call    dpmi_drop
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
dpmi_pvif       dd  0                   ; -> its interrupt flag, in PMIO
dpmi_rmact      dd  0                   ; ...and inside a simulated interrupt
dpmi_rmesp      dd  0                   ; where dpmi_simint must come back to
dpmi_rmesp0     dd  0                   ; and where ring 3 was coming in
dpmi_rmflags    dd  0                   ; the flags a reflected one goes in with
dpmi_rmdepth    dd  0                   ; the box level it was made from
dpmi_depth      dd  0                   ; the box level the client's program is on
dpmi_rmcall     db  DRC_INT             ; how dpmi_simint enters real mode
dpmi_msdos      db  0                   ; answer "MS-DOS" (dpmi_vendor): not yet
                db  0,0
dpmi_envpsp     dd  0                   ; the PSP whose environment word holds
dpmi_envseg     dd  0                   ; DPMI_IDX_ENV, and the segment it held
dpmi_fn         dw  0                   ; the INT 21h in hand, as AX came in
dpmi_dtasel     dw  0                   ; the DTA as the client set it (1Ah),
dpmi_dtaoff     dd  0                   ; for 2Fh to give back, and where that
dpmi_dtaflat    dd  0                   ; was in the kernel's terms
dpmi_excvec     dd  0                   ; the exception last handed to a handler
dpmi_excbase    dd  0                   ; its locked stack, 0 until the first
dpmi_xeip       dd  0                   ; the frame a handler came back with
dpmi_xcs        dd  0
dpmi_xfl        dd  0
dpmi_xesp       dd  0
dpmi_xss        dd  0
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
dpmi_fpeip      dd  -1                  ; the last coprocessor error a client
dpmi_fpcs       dd  -1                  ; was sent, and the tick the run of
dpmi_fpwhen     dd  0                   ; them began on - see dfa_fperr
dpmi_pf         db  0                  ; the pointer flags of the call in hand
                db  0,0,0
dpmi_odx        dd  0                   ; and what the client had in those
dpmi_osi        dd  0                   ; registers before we converted them
dpmi_odi        dd  0
dpmi_obx        dd  0
dpmi_segmap     dw  (DPMI_SEGMAX * 2) dup(0)    ; segment, selector

dpmi_cbtab      db  (DPMI_MAXCB * DCB_LEN) dup(0)
dpmi_pmvec      dd  (256 * 2) dup(0)    ; offset, then selector, per vector
dpmi_exc        dd  (DPMI_MAXEXC * 2) dup(0)    ; ...and per exception, 0203h
dpmi_memtab     db  (DPMI_MAXMEM * DM_LEN) dup(0)
dpmi_refcs      db  RMCS_LEN dup(0)
dpmi_savectx    db  V86C_SIZE dup(0)

msgDpmiFault    db  13,10,'DPMI client fault ',0
msgDpmiAt       db  ' at ',0
msgDpmiErr      db  ' err ',0
msgDpmiCR       db  13,10,0
msgDpmiStray    db  13,10,'PM-DOS: protected-mode interrupt with no client: ',0
msgDpmiNoClient db  'a ring-3 fault with no DPMI client running',0
msgDpmiRaw      db  13,10,'DPMI client switched to protected mode with a bad selector',13,10,0
msgDpmiRawNone  db  13,10,'Real-mode code switched to protected mode with no DPMI client',13,10,0
