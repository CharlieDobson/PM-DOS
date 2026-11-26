;=====================================================================
; RMBRG.INC - the way into the parked kernel from REAL mode
;
; THE KERNEL'S NET USE LETTERS UNDER WINDOWS IN STANDARD MODE.  The
; kernel is parked while Windows runs, and its network - the card, the
; TCP/IP stack, the SMB redirector - is 32-bit code above the megabyte
; that nothing runs.  Under 386 enhanced mode PMDOS.386 runs it: the
; VxD switches CR3 to the kernel's own page directory and calls
; brg_entry (NDLOAD.INC), and RMNET2.INC reaches the VxD through INT
; 2Fh AX=1684h.  Standard mode has no VxDs.  DOSX runs Windows in
; protected mode and drops to REAL mode for every DOS call, and real
; mode is ours to leave - the block move does it (RMXMS2.INC,
; rx_moveflat).  So this does what the VxD's trampoline does, from
; here: protected mode, the kernel's page tables, brg_entry, and back.
;
; Windows 3.1's Setup and Windows for Workgroups 3.11's both run their
; Windows half in standard mode, and read the rest of the disks from
; where SETUP was started.  Started from a NET USE letter, they asked
; for "Disk #3 in drive O:" because nothing here could read it
; (2026-11-26, the real-hardware report).
;
; WHAT THE CALL HAS, as the VxD's has it (pmdos386.asm, pd_tramp_src):
; the kernel's CR3 and brg_entry, out of its globals block at 1F00h,
; which the park leaves where it is; paging on with CR0.WP clear, as
; the kernel runs; flat DS, ES and SS; interrupts off; and a stack in
; low memory - a free one of RMDOS's own slots (stk_take), a kilobyte,
; which is what the VxD's page gives it.  The kernel's map is the
; identity everywhere this touches: this segment, the HMA (the park
; puts the A20 alias back to the identity), and the globals page.
;
; THE DESCRIPTORS ARE rx_gdt's (RMXMS2.INC): 08h flat data, the block
; move's; 10h flat 32-bit code; 18h 16-bit code at this segment, for
; the way back; 20h 16-bit data with a 64K limit, so that SS, DS and
; ES come back to real mode with real mode's limit.  FS and GS are not
; touched: the kernel's code loads no segment register but DS and ES,
; which is also why the VxD's way works.  The IDT is not changed:
; interrupts are off, and the kernel's code takes no fault it does not
; take under the VxD, where a fault is the same reset.  GDTR is not
; put back, as rx_moveflat does not put it back: DOSX loads its own
; every time it goes back to protected mode.  CR0 comes back with WP
; clear, which nothing reads with paging off; the un-park loads the
; kernel's own.
;
; THE 32-BIT PART IS SPELLED IN BYTES, because this segment is USE16
; and the assembler would encode every instruction in it for 16-bit
; code.  It finds its own data through EBP, the segment's linear base.
;
; Included by RMDOS.ASM after RMCACHE.INC, behind the stack slots.
;=====================================================================

;--- the kernel's globals block (INC\PMDOS.INC: GV_BASE = 1F00h) -----
RB_GVPGDIR      EQU     1F14h           ; GV_PGDIR - its CR3
RB_GVSIG        EQU     1FE0h           ; GV_BRGSIG - BRG_SIGVAL when brg_entry is there
RB_GVENTRY      EQU     1FE4h           ; GV_BRGENTRY - brg_entry's address
RB_SIGVAL       EQU     42475250h       ; BRG_SIGVAL, 'PRGB'

;--- what is called (INC\NDRV.INC, NIC.INC, SOCKET.H) -----------------
RB_NDTF_NIC     EQU     1               ; NDTF_NIC
RB_NDTF_STACK   EQU     2               ; NDTF_STACK
RB_NDTF_REDIR   EQU     4               ; NDTF_REDIR
RB_NIC_WAKE     EQU     5               ; NIC_WAKE - the parked card, up and polled
RB_SOCK_PUMP    EQU     11              ; SOCK_PUMP - the stack, one step

;--- rx_gdt's selectors -----------------------------------------------
RB_SELDATA      EQU     08h             ; flat data
RB_SELC32       EQU     10h             ; flat 32-bit code
RB_SELC16       EQU     18h             ; 16-bit code at this segment
RB_SELD16       EQU     20h             ; 16-bit data, 64K

RB_WAITMAX      EQU     2184            ; ticks (two minutes) before an op that
                                        ; never finishes is given up - SMBFS
                                        ; gives up on a server long before

;--- data, ahead of the code that names it ----------------------------
rb_state    db  0                       ; 0 not tried, 1 up, 2 no way in
rb_a20      db  0                       ; the gate as rb_call found it
rb_cf       dw  0                       ; low byte 0FFh: brg_entry's carry
rb_slot     dw  0                       ; the stack slot's top
rb_eax      dd  0                       ; the request's EAX, then the answer
rb_rmsp     dd  0                       ; the caller's SP, then SS
rb_pm32     dd  0                       ; rb_code32's linear address (rb_up)
            dw  RB_SELC32
rb_pm16     dd  (rb_code16 - rd_base) + RM_ORG
            dw  RB_SELC16

;=====================================================================
; rb_call - one call into the parked kernel, from real mode.
;   EAX = an NDTF_* flag, EDX = which of those drivers (0 = the first),
;   EBX = the driver's function, ECX = the buffer's length, EDI = the
;   buffer, a flat linear address in the identity map.
;   -> EAX = what brg_entry answered, CF its carry; CF with EAX
;   meaningless when no stack slot is free.  Every other register is
;   kept, and the caller's interrupt flag.  DS = ours; rb_up has run.
;=====================================================================
rb_call:
        pushf
        pushad
        push    es
        cli
        mov     [rb_eax],eax
        call    rx_a20test              ; the kernel is above the megabyte
        mov     [rb_a20],al
        call    rx_a20raise
        call    stk_take                ; AX = a free slot's top
        jc      rbk_noslot
        mov     [rb_slot],ax
        xor     ebp,ebp
        mov     bp,cs
        shl     ebp,4                   ; EBP = this segment's linear base
        movzx   esi,ax
        add     esi,ebp                 ; ESI = the slot, as a flat stack
        mov     word ptr [rb_rmsp],sp
        mov     word ptr [rb_rmsp+2],ss
        db      66h                     ; all thirty-two bits of the base
        lgdt    fword ptr [rx_gdtr]
        mov     eax,cr0
        or      al,1
        mov     cr0,eax
        db      66h,0FFh,2Eh            ; jmp fword ptr [rb_pm32]
        dw      OFFSET rb_pm32

rb_code32:                              ; CS = flat 32-bit, EIP = linear
        db      66h,0B8h                ; mov ax,RB_SELDATA
        dw      RB_SELDATA
        db      8Eh,0D8h                ; mov ds,ax
        db      8Eh,0C0h                ; mov es,ax
        db      8Eh,0D0h                ; mov ss,ax
        db      8Bh,0E6h                ; mov esp,esi
        db      0A1h                    ; mov eax,[RB_GVPGDIR]
        dd      RB_GVPGDIR
        db      0Fh,22h,0D8h            ; mov cr3,eax
        db      0Fh,20h,0C0h            ; mov eax,cr0
        db      0Dh                     ; or eax,80000000h - paging on
        dd      80000000h
        db      25h                     ; and eax,0FFFEFFFFh - WP off, as
        dd      0FFFEFFFFh              ; the kernel runs (PAGING.INC)
        db      0Fh,22h,0C0h            ; mov cr0,eax
        db      0EBh,00h                ; jmp $+2
        db      55h                     ; push ebp
        db      8Bh,85h                 ; mov eax,[ebp+rb_eax]
        dd      (rb_eax - rd_base) + RM_ORG
        db      0FFh,15h                ; call dword ptr [RB_GVENTRY]
        dd      RB_GVENTRY
        db      5Dh                     ; pop ebp
        db      1Bh,0F6h                ; sbb esi,esi - its carry
        db      8Bh,0D8h                ; mov ebx,eax - its answer
        db      0Fh,20h,0C0h            ; mov eax,cr0
        db      25h                     ; and eax,7FFFFFFFh - paging off
        dd      7FFFFFFFh
        db      0Fh,22h,0C0h            ; mov cr0,eax
        db      0FFh,0ADh               ; jmp fword ptr [ebp+rb_pm16]
        dd      (rb_pm16 - rd_base) + RM_ORG

rb_code16:                              ; CS = 16-bit code at this segment
        mov     ax,RB_SELD16
        mov     ds,ax
        mov     es,ax
        mov     ss,ax
        mov     eax,cr0
        and     al,0FEh
        mov     cr0,eax
        db      0EAh                    ; jmp far rb_rm, in real mode
        dw      OFFSET rb_rm
rb_rmcs     dw  0                       ; this segment (rb_up)

rb_rm:
        mov     ax,cs
        mov     ds,ax
        lss     sp,dword ptr [rb_rmsp]
        mov     [rb_eax],ebx            ; the answer
        mov     [rb_cf],si              ; ...and its carry
        mov     bx,[rb_slot]
        dec     bx                      ; inside the slot
        call    stk_freeat
rbk_out:
        cmp     byte ptr [rb_a20],0
        jne     rbk_gate
        call    rx_a20lower
rbk_gate:
        pop     es
        popad
        popf                            ; the caller's interrupt flag
        mov     eax,[rb_eax]
        shl     byte ptr [rb_cf],1      ; CF = brg_entry's
        ret
rbk_noslot:
        mov     byte ptr [rb_cf],0FFh
        jmp     short rbk_out

;=====================================================================
; rb_up - CF if there is no way in: a kernel too old to publish
;         brg_entry, or one not paging.  The first call fills in what
;         needs this segment's address and wakes the card, polled - its
;         interrupt masked at the card, so nothing is left unserviced
;         under Windows; rb_redir's pump drains it.  The un-park puts
;         the card back as it was (NETCORE.C, nd_park).  Registers kept.
;=====================================================================
rb_up:
        cmp     byte ptr [rb_state],1
        je      rbu_out                 ; up (and CF clear)
        ja      rbu_none
        mov     byte ptr [rb_state],2
        pushad
        push    es
        xor     ax,ax
        mov     es,ax
        cmp     dword ptr es:[RB_GVSIG],RB_SIGVAL
        jne     rbu_pop
        cmp     dword ptr es:[RB_GVPGDIR],0
        je      rbu_pop
        xor     eax,eax
        mov     ax,cs
        mov     [rb_rmcs],ax
        shl     eax,4
        mov     word ptr [rx_gdt + RB_SELC16 + 2],ax    ; base 15-0
        ror     eax,16
        mov     byte ptr [rx_gdt + RB_SELC16 + 4],al    ; base 23-16
        rol     eax,16
        add     eax,OFFSET rb_code32
        mov     [rb_pm32],eax
        mov     byte ptr [rb_state],1
        mov     eax,RB_NDTF_NIC
        xor     edx,edx
        mov     ebx,RB_NIC_WAKE
        xor     ecx,ecx
        xor     edi,edi
        call    rb_call
rbu_pop:
        pop     es
        popad
        cmp     byte ptr [rb_state],1
        je      rbu_out
rbu_none:
        stc
rbu_out:
        ret

;=====================================================================
; rb_redir - n2_pdapi in real mode: run [n2_req] to the end through
;            rb_call.  Each round is one step of the stack (SOCK_PUMP:
;            the frames off the card, the protocol, the timers) and one
;            slice of the op (REDIR_RESUME - SMBFS runs it on its fibre
;            until a socket wait has nothing, and answers REDIR_PENDING),
;            with the caller's interrupts let in between, so Windows'
;            clock and keyboard run while the server answers.  SOCK_PUMP
;            is handed the request's registers too, and reads none of
;            them.  -> CF clear and NR_STATUS, or CF when there is no
;            way in.  Registers kept.
;=====================================================================
rb_redir:
        pushad
        push    es
        call    rb_up
        jc      rbr_none
        xor     ax,ax
        mov     es,ax
        movzx   edi,word ptr [rb_rmcs]
        shl     edi,4
        add     edi,OFFSET n2_req       ; EDI = the request, flat
        mov     ecx,NR_SIZE
        xor     edx,edx
        mov     esi,dword ptr [n2_req+NR_OP]
        or      esi,REDIR_RESUME
        mov     ebp,dword ptr es:[46Ch] ; when it started (the BIOS clock,
rbr_poll:                               ; which runs under Windows)
        mov     eax,RB_NDTF_STACK
        mov     ebx,RB_SOCK_PUMP
        call    rb_call
        mov     eax,RB_NDTF_REDIR
        mov     ebx,esi
        call    rb_call
        jc      rbr_none                ; no redirector to run it
        cmp     dword ptr [n2_req+NR_STATUS],REDIR_PENDING
        jne     rbr_done
        mov     eax,dword ptr es:[46Ch]
        cmp     eax,ebp
        jae     rbr_since
        mov     ebp,eax                 ; midnight: count from here
rbr_since:
        sub     eax,ebp
        cmp     eax,RB_WAITMAX
        jb      rbr_poll                ; still waiting: NR_STATUS says so
rbr_done:
        pop     es
        popad
        clc
        ret
rbr_none:
        pop     es
        popad
        stc
        ret
