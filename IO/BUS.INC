;=====================================================================
; BUS.INC - the way in to IRQ.INC, PCI.INC, PAGING.INC and VDS.INC
;
; One service with the function in AH (IOS_BUS), because the DOS side
; has one use for all of it: 21h/F1h AL=12h hands a program's
; registers over as they came and hands back what returns.  A native
; program runs at ring 3 and a report about the machine has to be
; asked for from inside the kernel - the argument F1h has been making
; since AL=9 - and PMDOS.SYS has no room for a function it would only
; be passing on.
;
; The three the DOS side calls for itself have entries of their own in
; the service table.
;
; Included by IOPM.ASM, last of the five.
;=====================================================================

;=====================================================================
; bsv_call - IOS_BUS.  AH = a BUS_* function; the other registers are
;            that function's, and are listed with the equates in
;            PMDOS.INC.  CF and AX = a DOS error if it was refused.
;=====================================================================
bsv_call:
        cmp     ah,BUS_PCIHERE
        je      bsv_here
        cmp     ah,BUS_PCIENUM
        je      bsv_enum
        cmp     ah,BUS_PCICLASS
        je      bsv_class
        cmp     ah,BUS_PCIID
        je      bsv_id
        cmp     ah,BUS_PCIREAD
        je      bsv_read
        cmp     ah,BUS_PCIWRITE
        je      bsv_write
        cmp     ah,BUS_PCIBAR
        je      bsv_bar
        cmp     ah,BUS_PCIIRQ
        je      bsv_irq
        cmp     ah,BUS_MAPPHYS
        je      bsv_map
        cmp     ah,BUS_PHYSOF
        je      bsv_physof
        cmp     ah,BUS_IRQSTAT
        je      bsv_stat
IFDEF IRQ_SELFTEST
        cmp     ah,BUS_SELFTEST
        je      iqx_report
ENDIF
        mov     eax,ERR_FUNC
        stc
        ret

bsv_here:
        movzx   eax,byte ptr [pci_ok]
        mov     ebx,dword ptr [pci_lastbus]
        mov     ecx,dword ptr [pci_count]
        mov     edx,dword ptr [irq_elcr]
        clc
        ret

bsv_enum:
        push    esi
        call    pcx_nth
        jc      bsv_nomore
bsv_entry:
        mov     ebx,dword ptr [esi+PCT_ADDR]
        mov     eax,dword ptr [esi+PCT_ID]
        mov     ecx,dword ptr [esi+PCT_CLASS]
        pop     esi
        clc
        ret

bsv_class:
        push    esi
        push    edx
        push    edi
        and     ecx,00FFFFFFh
        shl     ecx,8                   ; the revision is not part of it
        mov     edx,0FFFFFF00h
        mov     edi,PCT_CLASS
        call    pcx_match
        pop     edi
        pop     edx
        jc      bsv_nomore
        jmp     short bsv_entry

bsv_id:
        push    esi
        push    edx
        push    edi
        shl     edx,16
        mov     dx,cx
        mov     ecx,edx
        mov     edx,0FFFFFFFFh
        mov     edi,PCT_ID
        call    pcx_match
        pop     edi
        pop     edx
        jc      bsv_nomore
        jmp     short bsv_entry

bsv_nomore:
        pop     esi
        mov     eax,ERR_NOFILE
        stc
        ret

bsv_read:
        call    bsv_isdev
        jc      bsv_nodev
        call    pcx_rd
        jc      bsv_bad
        ret
bsv_write:
        call    bsv_isdev
        jc      bsv_nodev
        call    pcx_wr
        jc      bsv_bad
        ret
bsv_bar:
        call    bsv_isdev
        jc      bsv_nodev
        call    pcx_bar
        jc      bsv_bad
        ret

;--- the line and the pin, and whether the line is a level one.  FFh
;--- is what a BIOS writes for "this pin was not routed", and it is
;--- handed back as it is: there is no such line to look up.
bsv_irq:
        call    bsv_isdev
        jc      bsv_nodev
        mov     al,PCIR_INTR
        call    pci_read
        movzx   eax,ax
        xor     edx,edx
        cmp     al,16
        jae     bsv_irqout
        push    ecx
        movzx   ecx,al
        bt      dword ptr [irq_elcr],ecx
        pop     ecx
        jnc     bsv_irqout
        inc     edx
bsv_irqout:
        clc
        ret

bsv_nodev:
        mov     eax,ERR_BADDRIVE
        stc
        ret
bsv_bad:
        mov     eax,ERR_BADDATA
        stc
        ret

bsv_map:
        call    pg_mapphys
        jnc     bsv_ret
        mov     eax,ERR_NOMEM
        stc
bsv_ret:
        ret

bsv_physof:
        call    vd_run
        jnc     bsv_ret
        mov     eax,ERR_BADBLOCK
        stc
        ret

bsv_stat:
        call    irq_stat
        jnc     bsv_ret
        mov     eax,ERR_FUNC
        stc
        ret

;---------------------------------------------------------------------
; bsv_isdev - EBX = what a caller says is a device.  CF unless there is
;             a PCI bus, the number is an address and nothing else, and
;             something answers at it.
;---------------------------------------------------------------------
bsv_isdev:
        cmp     byte ptr [pci_ok],0
        je      bsd_no
        test    ebx,0FF0000FFh
        jnz     bsd_no
        push    eax
        mov     al,PCIR_VENDOR
        call    pci_read
        cmp     ax,0FFFFh
        pop     eax
        je      bsd_no
        clc
        ret
bsd_no:
        stc
        ret

;=====================================================================
; bsv_init - IOS_BUSINIT.  AX = BUS_PARAS paragraphs below 1MB for the
;            INT 4Bh stub, or 0; EBX -> the DPMI host's protected-mode
;            vectors; ECX -> its "a client is running"; EDX -> its "and
;            it is a 32-bit one".  Answers EAX ->
;            the client's interrupt flag, which is PMIO's to keep and
;            the host's to put back when a handler returns through it.
;=====================================================================
bsv_init:
        call    vpc_setup
        call    vd_setup
        mov     eax,OFFSET pm3_vif
        ret

;=====================================================================
; box_hlt - IOS_MSHLT.  ESI -> a box's context, stopped at a HLT; EAX =
;           whose program it is.  CF clear if the HLT was one of
;           PMIO's, and served.
;
; The service is still named for the mouse, which was the first thing
; here to be reached this way and is still asked first.
;=====================================================================
box_hlt:
        call    ms_hlt
        jnc     bxh_out
        call    vd_hlt
bxh_out:
        ret

;=====================================================================
; box_park - IOS_MSPARK.  AL = 0 before the machine is handed to real
;            mode and 1 after: everything of PMIO's that stands behind
;            a vector a real-mode program can call.
;=====================================================================
box_park:
        push    eax
        call    ms_park
        pop     eax
        call    vd_park
        ret
