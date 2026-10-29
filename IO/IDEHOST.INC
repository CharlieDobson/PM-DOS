;=====================================================================
; IDEHOST.INC - the host adapter's half of a transfer mode
;
; A transfer mode is two settings that have to agree.  SET FEATURES
; tells the DRIVE how fast to talk and is the same command on every
; drive made; how fast the CONTROLLER talks is a timing register in
; configuration space, and every chipset spells it differently.  Up
; to v0.60 PM-DOS knew none of them, which is why ATA.INC stopped at
; Ultra DMA 2 and left even that to the user's judgement.
;
; THIS FILE KNOWS TWO FAMILIES, and they are the two that account for
; most of the boards a parallel ATA disk was ever plugged into:
;
;   Intel   PIIX3, PIIX4 and the ICH line.  An enable bit and a two-bit
;           cycle time for each drive, and from the ICH on a choice of
;           33, 66 or 100MHz clock behind them.
;   VIA     VT82C586A to VT8237.  One byte for each drive, and what
;           the byte means depends on which south bridge it is in -
;           the IDE function has carried the same ID since 1996, so
;           the ISA bridge beside it is what has to be asked.
;
; ANYTHING ELSE IS A STRANGER, and a stranger's timing is never
; written.  ata_limit is where that turns into a rule: on a host this
; file does not know, the kernel selects nothing faster than the BIOS
; proved the host could do.
;
; THREE QUESTIONS ARE ANSWERED HERE and ATA.INC asks all of them:
; how fast can this host go (ideh_top), what cable does it say is on
; this channel (ideh_cable), and make your end match this mode
; (ideh_udma, ideh_mwdma).
;
; SINCE v0.68 THE MULTIWORD TIMING IS WRITTEN TOO, and that is what
; lets IDEDMA=AUTO choose a mode on these hosts instead of keeping the
; BIOS's.  Before, a drive put in a multiword mode ran on the PIO
; timing the BIOS left, which is only right when the BIOS left PIO 4.
; ideh_mw says whether this host's is one PM-DOS writes: every host
; here but the first PIIX, which has one timing for both drives on a
; channel and so cannot be given one drive's without the other's.
;
; The cable is the host's to report because the detect pin is wired
; to the board, not to the controller: the BIOS reads it through
; whatever general-purpose input the board's designer chose and leaves
; the answer in a register for the driver.  That is an Intel bit in
; one place and a VIA bit in another, and on an older VIA bridge it is
; nowhere at all - so "the host cannot say" is an answer too, and
; ata_cables goes on to ask the drives.
;
; SINCE v0.68 THERE MAY BE TWO CONTROLLERS, and each has its own record
; in ATA.INC's ata_hosts.  The state at the bottom of this file is the
; selected one's: see ideh_sel.
;
; Included by IOPM.ASM, after PCI.INC and before ATA.INC.
;=====================================================================

IH_NONE         EQU     0               ; a chipset this file has not met
IH_PIIX         EQU     1               ; Intel, before the ICH: no clock to pick
IH_ICH          EQU     2               ; Intel ICH: 33, 66 or 100MHz a drive
IH_VIA          EQU     3

IH_NOUDMA       EQU     0Fh             ; known, and it has no Ultra DMA at all

IHC_UNKNOWN     EQU     0               ; ideh_cable: the host cannot say
IHC_40          EQU     1
IHC_80          EQU     2

; Intel.  Read a dword at a time, so 4Ah is the high word of 48h.
IHI_UDMACTL     EQU     48h             ; bits 0-3: Ultra DMA on, a drive each
                                        ; bits 16-31: cycle time, two bits in
                                        ; every four
IHI_IOCFG       EQU     54h             ; bits 0-3: 66MHz clock, a drive each
                                        ; bits 4-7: 80 conductors, as the BIOS
                                        ; found them; bits 12-15: 100MHz clock

IHI_IDETIM      EQU     40h             ; a word a channel: the primary in
                                        ; the low word.  Bits 12-13 and 8-9
                                        ; the fast timing, 14 the slave has
                                        ; its own, 0-3 the master's and 4-7
                                        ; the slave's TIME, IE, PPE and DTE
IHI_SIDETIM     EQU     44h             ; the slaves' own timing, four bits
                                        ; each: the primary's in bits 0-3
IHI_SITRE       EQU     4000h
IHI_TIME        EQU     01h             ; the fast timing, not the slow one
IHI_IE          EQU     02h             ; wait on IORDY
IHI_DTE         EQU     08h             ; the fast timing for DMA only

; VIA.  50h is the secondary slave and 53h the primary master.
IHV_DATA        EQU     48h             ; the same order: one byte a drive,
                                        ; PIO and multiword alike
IHV_UDMA        EQU     50h
IHV_CABLE       EQU     10h             ; in each byte, on the 100 and 133
                                        ; bridges: the BIOS saw 80 conductors
IHV_CLK66       EQU     00080008h       ; on the 66 bridges: run the fast clock

;=====================================================================
; ideh_probe - EDI = a controller's entry in ata_hosts, with its
;              address and ID filled in.  Which host is this?  The
;              answer goes in the entry's AH_KIND, AH_TOP, AH_ROW and
;              AH_MW, and is the selected host's until the next
;              ideh_sel.  Called by ata_bminit.
;=====================================================================
ideh_probe:
        pushad
        mov     byte ptr [ideh_kind],IH_NONE
        mov     byte ptr [ideh_top],IH_NOUDMA
        mov     byte ptr [ideh_row],0
        mov     byte ptr [ideh_mw],0
IFDEF IDEH_BLIND
        ; BUILD nohost: every host is a stranger.  The rules for one
        ; can then be tested on a board whose BIOS selects a fast mode,
        ; and the boards that do are the ones this file knows
        jmp     short ihp_stranger
ENDIF

        mov     eax,[edi+AH_ID]
        cmp     ax,8086h
        je      ihp_intel
        cmp     eax,05711106h
        je      ihp_via
ihp_stranger:
        jmp     ihp_out

ihp_intel:
        shr     eax,16                  ; AX = the device ID
        mov     esi,OFFSET iht_intel
ihp_ilp:
        cmp     word ptr [esi],0
        je      ihp_stranger
        cmp     ax,[esi]
        je      ihp_ihit
        add     esi,4
        jmp     short ihp_ilp
ihp_ihit:
        mov     al,[esi+2]
        mov     [ideh_top],al
        mov     al,[esi+3]
        mov     [ideh_kind],al
        cmp     word ptr [esi],1230h    ; the first PIIX: no slave timing
        je      ihp_out
        mov     byte ptr [ideh_mw],1
        jmp     short ihp_out

        ; THE IDE FUNCTION SAYS 0571 WHATEVER IT IS IN.  What it can do
        ; is a property of the south bridge, and the south bridge's
        ; name is on its ISA bridge - with the revision, because 686A
        ; and 686B are one device ID and differ by a mode
ihp_via:
        mov     cx,0601h
        call    pci_find
        jc      ihp_out
        mov     al,PCIR_VENDOR
        call    pci_read
        cmp     ax,1106h
        jne     ihp_out
        shr     eax,16
        mov     edx,eax                 ; DX = the bridge's device ID
        mov     al,PCIR_CLASS
        call    pci_read                ; AL = its revision
        mov     esi,OFFSET iht_via
ihp_vlp:
        cmp     word ptr [esi],0
        je      ihp_out
        cmp     dx,[esi]
        jne     ihp_vnext
        cmp     al,[esi+2]
        jb      ihp_vnext
        cmp     al,[esi+3]
        ja      ihp_vnext
        mov     al,[esi+4]
        mov     [ideh_top],al
        mov     al,[esi+5]
        mov     [ideh_row],al
        mov     byte ptr [ideh_kind],IH_VIA
        mov     byte ptr [ideh_mw],1
        jmp     short ihp_out
ihp_vnext:
        add     esi,6
        jmp     short ihp_vlp
ihp_out:
        mov     al,[ideh_kind]          ; EDI survived: the VIA search
        mov     [edi+AH_KIND],al        ; above used ESI and EBX
        mov     al,[ideh_top]
        mov     [edi+AH_TOP],al
        mov     al,[ideh_row]
        mov     [edi+AH_ROW],al
        mov     al,[ideh_mw]
        mov     [edi+AH_MW],al
        mov     eax,[edi+AH_DEV]
        mov     [ideh_dev],eax
        popad
        ret

;=====================================================================
; ideh_sel  - ESI = device.  Its controller is the selected host.
; ideh_selh - EDI = an entry in ata_hosts, or 0 for none.  That one is.
;
; TWO CONTROLLERS, ONE SET OF ANSWERS.  Every routine in this file that
; touches a device selects the device's own controller first, so the
; kind, the fastest mode and the configuration address it goes on to
; use are never another controller's.  A device on no controller this
; file knows - an ISA channel, or one at 1E8h - selects "none", and
; every routine here does nothing for it.  All registers are kept.
;=====================================================================
ideh_sel:
        push    edi
        mov     edi,[esi+ATD_HOST]
        call    ideh_selh
        pop     edi
        ret

ideh_selh:
        push    eax
        or      edi,edi
        jz      ihs_none
        mov     eax,[edi+AH_DEV]
        mov     [ideh_dev],eax
        mov     al,[edi+AH_KIND]
        mov     [ideh_kind],al
        mov     al,[edi+AH_TOP]
        mov     [ideh_top],al
        mov     al,[edi+AH_ROW]
        mov     [ideh_row],al
        mov     al,[edi+AH_MW]
        mov     [ideh_mw],al
        pop     eax
        ret
ihs_none:
        mov     dword ptr [ideh_dev],0
        mov     byte ptr [ideh_kind],IH_NONE
        mov     byte ptr [ideh_top],IH_NOUDMA
        mov     byte ptr [ideh_row],0
        mov     byte ptr [ideh_mw],0
        pop     eax
        ret

;=====================================================================
; ideh_index - ESI = device -> ECX = which of its host's four drives:
;              0 and 1 on the primary channel, 2 and 3 on the other.
;              ata_pickbm worked it out from where the channel is.
;=====================================================================
ideh_index:
        movzx   ecx,byte ptr [esi+ATD_HDRV]
        ret

;=====================================================================
; ideh_cable - AL = channel -> AL = IHC_80, IHC_40, or IHC_UNKNOWN if
;              this host has nowhere to say.
;
; BOTH DRIVES' BITS ARE READ AND EITHER ONE WILL DO.  The cable is one
; cable, but the BIOS writes its finding against the drives it found,
; so a channel with only a slave on it has the master's bit clear.
;=====================================================================
ideh_cable:
        push    ebx
        push    ecx
        push    edx
        movzx   ecx,al
        xor     edx,edx                 ; DL = IHC_UNKNOWN
        mov     ebx,[ideh_dev]
        cmp     byte ptr [ideh_kind],IH_ICH
        je      ihc_ich
        cmp     byte ptr [ideh_kind],IH_VIA
        je      ihc_via
        jmp     short ihc_out

ihc_ich:
        cmp     byte ptr [ideh_top],3   ; ICH0 stops at 33 and has no use
        jb      ihc_out                 ; for the answer
        mov     al,IHI_IOCFG
        call    pci_read
        shl     ecx,1
        add     ecx,4                   ; bits 4-5, or 6-7
        shr     eax,cl
        mov     dl,IHC_40
        test    al,3
        jz      ihc_out
        mov     dl,IHC_80
        jmp     short ihc_out

ihc_via:
        cmp     byte ptr [ideh_top],5   ; the 66 bridges have no such bit:
        jb      ihc_out                 ; only the drive can say
        mov     al,IHV_UDMA
        call    pci_read
        shl     ecx,4
        shl     eax,cl                  ; the channel's two bytes, on top
        mov     dl,IHC_40
        test    eax,(IHV_CABLE SHL 24) OR (IHV_CABLE SHL 16)
        jz      ihc_out
        mov     dl,IHC_80
ihc_out:
        mov     al,dl
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; ideh_udma - ESI = device, AL = an Ultra DMA mode, 0 to 6.  Time the
;             host's end for it.  The caller has already kept the mode
;             under ideh_top; nothing here checks again.
;
; INTEL: the cycle time is two bits and means a different thing at
; each clock - 01 is mode 1 at 33MHz, mode 3 at 66 and mode 5 at 100 -
; so the clock goes in first and the cycle time and the enable after
; it, in one write.
;
; VIA: one byte, from the table at the bottom.  The cable bit in it is
; the BIOS's and is put back as it was found.
;=====================================================================
ideh_udma:
        pushad
        call    ideh_sel
        movzx   edi,al                  ; EDI = the mode
        call    ideh_index              ; ECX = the drive
        mov     ebx,[ideh_dev]
        cmp     byte ptr [ideh_kind],IH_VIA
        je      ihu_via
        cmp     byte ptr [ideh_kind],IH_NONE
        je      ihu_out

        cmp     byte ptr [ideh_kind],IH_ICH
        jne     ihu_cycle
        mov     al,IHI_IOCFG
        call    pci_read
        mov     edx,1001h
        shl     edx,cl
        not     edx
        and     eax,edx                 ; 33MHz, until something says more
        xor     edx,edx
        cmp     edi,3
        jb      ihu_clock
        mov     edx,0001h               ; 66
        cmp     edi,5
        jb      ihu_clock
        mov     edx,1000h               ; 100
ihu_clock:
        shl     edx,cl
        or      eax,edx
        push    ecx
        mov     ecx,eax
        mov     al,IHI_IOCFG
        call    pci_write
        pop     ecx

ihu_cycle:
        mov     al,IHI_UDMACTL
        call    pci_read
        mov     edx,1
        shl     edx,cl
        or      eax,edx                 ; Ultra DMA on for this drive
        shl     ecx,2
        add     ecx,16                  ; its two bits of 4Ah
        mov     edx,3
        shl     edx,cl
        not     edx
        and     eax,edx
        movzx   edx,byte ptr [ih_icycle+edi]
        shl     edx,cl
        or      eax,edx
        mov     ecx,eax
        mov     al,IHI_UDMACTL
        call    pci_write
        jmp     short ihu_out

ihu_via:
        movzx   edx,byte ptr [ideh_row]
        imul    edx,7
        add     edx,edi
        movzx   edx,byte ptr [ih_vcycle+edx]
        call    ideh_viaput
ihu_out:
        popad
        ret

;=====================================================================
; ideh_noudma - ESI = device.  The drive has been put in a multiword
;               mode: stop the host answering it in Ultra DMA.
;               ideh_mwdma calls it first; on a host whose multiword
;               timing PM-DOS does not write, that is all there is.
;=====================================================================
ideh_noudma:
        pushad
        call    ideh_sel
        cmp     byte ptr [ideh_top],IH_NOUDMA
        je      ihn_out                 ; it has none to stop
        call    ideh_index
        mov     ebx,[ideh_dev]
        cmp     byte ptr [ideh_kind],IH_VIA
        je      ihn_via
        cmp     byte ptr [ideh_kind],IH_NONE
        je      ihn_out
        mov     al,IHI_UDMACTL
        call    pci_read
        mov     edx,1
        shl     edx,cl
        not     edx
        and     eax,edx
        mov     ecx,eax
        mov     al,IHI_UDMACTL
        call    pci_write
        jmp     short ihn_out
ihn_via:
        movzx   edx,byte ptr [ideh_row]
        movzx   edx,byte ptr [ih_voff+edx]
        call    ideh_viaput
ihn_out:
        popad
        ret

;=====================================================================
; ideh_mwdma - ESI = device, AL = a multiword DMA mode, 0 to 2.  Time
;              the host's end for it.
;
; ON BOTH FAMILIES THE MULTIWORD TIMING IS THE PIO TIMING, one setting
; a drive that the data port is read with as well.  So it is never
; made faster than the drive's own fastest PIO mode (ATD_PIO), or a
; command that moves its data by PIO - IDENTIFY, or a transfer that
; has fallen back - would go faster than the drive can answer.
;
; INTEL: two fields, the IORDY sample point and the recovery time, in
; clocks.  Each multiword mode has the timing of a PIO mode (ih_mwpio)
; and that PIO mode's two fields go in.  Where the drive's PIO is the
; slower of the two, the DTE bit gives the fast timing to DMA alone
; and the data port keeps the slow, compatible one.  A slave's fields
; are in SIDETIM, and SITRE says to look there; when SITRE was off the
; slave was using the master's, so those are copied across first and
; a change to the master leaves the slave as it was.  PPE is left as
; the BIOS set it: it is a PIO matter.
;
; VIA: one byte a drive, the pulse and the recovery in clocks, from
; ih_vmw.  The address setup and the command timing are a channel's,
; not a drive's, and are left as the BIOS set them; leaving them long
; costs nothing that DMA uses.
;=====================================================================
ideh_mwdma:
        call    ideh_noudma
        pushad
        call    ideh_sel
        cmp     byte ptr [ideh_mw],0
        je      ihm_out
        movzx   edi,al                  ; EDI = the mode
        call    ideh_index              ; ECX = the drive
        mov     ebx,[ideh_dev]
        cmp     byte ptr [ideh_kind],IH_VIA
        je      ihm_via

        call    ideh_mwbits             ; DL = control, DH = the fields
        mov     ebp,ecx
        shr     ebp,1
        shl     ebp,4                   ; EBP = where the channel's word is
        test    ecx,1
        jnz     ihm_slave

        ; the master: a slave on the old shared timing gets its own copy
        ; of it, and then the master's fields are replaced
        mov     al,IHI_IDETIM
        call    pci_read
        push    eax                     ; [ESP] = both channels' words
        push    ecx
        mov     ecx,ebp
        shr     eax,cl                  ; AX = this channel's
        pop     ecx
        test    eax,IHI_SITRE
        jnz     ihm_mfields
        push    edx
        mov     edx,eax
        shr     edx,10
        and     edx,0Ch                 ; ISP, as SIDETIM spells it
        shr     eax,8
        and     eax,3                   ; and RCT
        or      eax,edx                 ; AL = the slave's fields
        pop     edx
        call    ideh_sidetim
        mov     eax,IHI_SITRE
        push    ecx
        mov     ecx,ebp
        shl     eax,cl
        pop     ecx
        or      [esp],eax
ihm_mfields:
        movzx   eax,dh
        and     al,0Ch
        shl     eax,10                  ; ISP, from bits 2-3 to 12-13
        and     dh,03h
        or      ah,dh                   ; RCT to bits 8-9
        movzx   edx,dl
        or      eax,edx                 ; and the master's control bits
        mov     edx,0FFFFh AND (NOT 0CCF4h)
        jmp     short ihm_word

ihm_slave:
        mov     al,dh
        call    ideh_sidetim
        mov     al,IHI_IDETIM
        call    pci_read
        push    eax                     ; [ESP] = both channels' words
        movzx   eax,dl
        shl     eax,4                   ; the slave's control bits
        or      eax,IHI_SITRE
        mov     edx,0FFFFh AND (NOT 0FF4Fh)

        ; [ESP] = the register, EAX = the bits, EDX = what they replace,
        ; both as one channel's word
ihm_word:
        push    ecx
        mov     ecx,ebp
        shl     eax,cl
        shl     edx,cl
        pop     ecx
        not     edx
        and     [esp],edx
        or      [esp],eax
        pop     ecx                     ; ECX = the register as it will be
        mov     al,IHI_IDETIM
        call    pci_write
        jmp     short ihm_out

ihm_via:
        movzx   eax,byte ptr [esi+ATD_PIO]
        cmp     eax,4
        jbe     ihm_vpio
        mov     eax,4
ihm_vpio:
        imul    edx,edi,5
        add     edx,eax
        mov     dl,[ih_vmw+edx]
        call    ideh_viadata
ihm_out:
        popad
        ret

;---------------------------------------------------------------------
; ideh_mwbits - ESI = device, EDI = a multiword mode.  -> DL = the
;               control bits for one drive (TIME, IE, and DTE if the
;               drive's PIO is slower than the mode), DH = the timing
;               fields as SIDETIM spells them, ISP over RCT.
;---------------------------------------------------------------------
ideh_mwbits:
        push    eax
        movzx   eax,byte ptr [ih_mwpio+edi]     ; the PIO mode it is timed as
        mov     dh,[ih_itim+eax]
        xor     dl,dl
        cmp     al,2
        jb      ihmb_out                ; the compatible timing: see ih_itim
        mov     dl,IHI_TIME OR IHI_IE
        cmp     [esi+ATD_PIO],al
        jae     ihmb_out
        or      dl,IHI_DTE
ihmb_out:
        pop     eax
        ret

;---------------------------------------------------------------------
; ideh_sidetim - EBX = the controller, ECX = the drive (a slave's, or
;                the master's whose slave it is), AL = four bits of
;                slave timing.  Put them in that channel's half of
;                SIDETIM.  Everything else in the dword is kept.
;---------------------------------------------------------------------
ideh_sidetim:
        pushad
        movzx   edx,al
        and     edx,0Fh
        shr     ecx,1
        shl     ecx,2                   ; 0 or 4
        shl     edx,cl
        mov     edi,0Fh
        shl     edi,cl
        not     edi
        mov     al,IHI_SIDETIM
        call    pci_read
        and     eax,edi
        or      eax,edx
        mov     ecx,eax
        mov     al,IHI_SIDETIM
        call    pci_write
        popad
        ret

;---------------------------------------------------------------------
; ideh_viadata - EBX = the controller, ECX = the drive, DL = its new
;                data timing byte.  The other three drives' are kept.
;---------------------------------------------------------------------
ideh_viadata:
        pushad
        movzx   edx,dl
        neg     ecx
        add     ecx,3
        shl     ecx,3                   ; where this drive's byte starts
        shl     edx,cl
        mov     edi,0FFh
        shl     edi,cl
        not     edi
        mov     al,IHV_DATA
        call    pci_read
        and     eax,edi
        or      eax,edx
        mov     ecx,eax
        mov     al,IHV_DATA
        call    pci_write
        popad
        ret

;---------------------------------------------------------------------
; ideh_viaput - EBX = the controller, ECX = the drive, DL = its new
;               byte.  Everything but the cable bit is replaced.
;---------------------------------------------------------------------
ideh_viaput:
        push    eax
        push    ecx
        push    edx
        push    edi
        movzx   edx,dl
        neg     ecx
        add     ecx,3
        shl     ecx,3                   ; where this drive's byte starts
        shl     edx,cl
        mov     edi,0FFh AND (NOT IHV_CABLE)
        shl     edi,cl
        not     edi
        mov     al,IHV_UDMA
        call    pci_read
        and     eax,edi
        or      eax,edx
        cmp     byte ptr [ideh_top],4
        jne     ihvp_write
        or      eax,IHV_CLK66
ihvp_write:
        mov     ecx,eax
        mov     al,IHV_UDMA
        call    pci_write
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        ret

;=====================================================================
; ideh_agrees - ESI = device, AL = the mode in force as SET FEATURES
;               spells it.  CF clear if the host's registers say the
;               same thing.
;
; FOR THE TEST PROGRAM, and only meaningful for a mode PM-DOS set: a
; BIOS is entitled to time a mode more loosely than the table here
; does, and a register that differs from ours for that reason is not
; a disagreement.
;=====================================================================
ideh_agrees:
        pushad
        call    ideh_sel
        mov     ah,al
        and     ah,0F8h
        cmp     ah,20h
        je      iha_mw
        cmp     ah,40h
        jne     iha_no
        movzx   edi,al
        and     edi,07h                 ; EDI = the mode
        call    ideh_index              ; ECX = the drive
        mov     ebx,[ideh_dev]
        cmp     byte ptr [ideh_kind],IH_VIA
        je      iha_via
        cmp     byte ptr [ideh_kind],IH_NONE
        je      iha_no

        mov     al,IHI_UDMACTL
        call    pci_read
        bt      eax,ecx
        jnc     iha_no                  ; not even on
        push    ecx
        shl     ecx,2
        add     ecx,16
        shr     eax,cl
        pop     ecx
        and     al,3
        cmp     al,[ih_icycle+edi]
        jne     iha_no
        cmp     byte ptr [ideh_kind],IH_ICH
        jne     iha_yes
        mov     al,IHI_IOCFG
        call    pci_read
        shr     eax,cl
        and     eax,1001h
        xor     edx,edx
        cmp     edi,3
        jb      iha_clock
        mov     edx,0001h
        cmp     edi,5
        jb      iha_clock
        mov     edx,1000h
iha_clock:
        cmp     eax,edx
        jne     iha_no
        jmp     iha_yes

iha_via:
        mov     al,IHV_UDMA
        call    pci_read
        neg     ecx
        add     ecx,3
        shl     ecx,3
        shr     eax,cl
        and     al,0FFh AND (NOT IHV_CABLE)
        movzx   edx,byte ptr [ideh_row]
        imul    edx,7
        add     edx,edi
        cmp     al,[ih_vcycle+edx]
        jne     iha_no
        jmp     iha_yes

        ; MULTIWORD: the drive's timing is what ideh_mwdma would write,
        ; and Ultra DMA is off for it
iha_mw:
        cmp     byte ptr [ideh_mw],0
        je      iha_no
        movzx   edi,al
        and     edi,07h                 ; EDI = the mode
        call    ideh_index              ; ECX = the drive
        mov     ebx,[ideh_dev]
        cmp     byte ptr [ideh_kind],IH_VIA
        je      iha_mwvia

        cmp     byte ptr [ideh_top],IH_NOUDMA
        je      iha_mwon
        mov     al,IHI_UDMACTL
        call    pci_read
        bt      eax,ecx
        jc      iha_no                  ; still in Ultra DMA
iha_mwon:
        call    ideh_mwbits             ; DL = control, DH = the fields
        mov     al,IHI_IDETIM
        call    pci_read
        mov     ebp,ecx
        shr     ebp,1
        shl     ebp,4
        push    ecx
        mov     ecx,ebp
        shr     eax,cl                  ; AX = this channel's word
        pop     ecx
        test    ecx,1
        jnz     iha_mwslave
        mov     esi,eax
        and     al,IHI_TIME OR IHI_IE OR IHI_DTE
        cmp     al,dl
        jne     iha_no
        mov     eax,esi
        shr     eax,10
        and     eax,0Ch                 ; ISP
        shr     esi,8
        and     esi,3                   ; RCT
        or      eax,esi
        cmp     al,dh
        jne     iha_no
        jmp     short iha_yes
iha_mwslave:
        test    eax,IHI_SITRE
        jz      iha_no
        shr     eax,4
        and     al,IHI_TIME OR IHI_IE OR IHI_DTE
        cmp     al,dl
        jne     iha_no
        mov     al,IHI_SIDETIM
        call    pci_read
        shr     ecx,1
        shl     ecx,2
        shr     eax,cl
        and     al,0Fh
        cmp     al,dh
        jne     iha_no
        jmp     short iha_yes

iha_mwvia:
        mov     al,IHV_UDMA
        call    pci_read
        push    ecx
        neg     ecx
        add     ecx,3
        shl     ecx,3
        shr     eax,cl
        pop     ecx
        and     al,0FFh AND (NOT IHV_CABLE)
        movzx   edx,byte ptr [ideh_row]
        cmp     al,[ih_voff+edx]
        jne     iha_no                  ; still in Ultra DMA
        movzx   eax,byte ptr [esi+ATD_PIO]
        cmp     eax,4
        jbe     iha_vpio
        mov     eax,4
iha_vpio:
        imul    edx,edi,5
        add     edx,eax
        mov     dl,[ih_vmw+edx]
        mov     al,IHV_DATA
        call    pci_read
        neg     ecx
        add     ecx,3
        shl     ecx,3
        shr     eax,cl
        cmp     al,dl
        jne     iha_no
iha_yes:
        popad
        clc
        ret
iha_no:
        popad
        stc
        ret

;=====================================================================
; The hosts
;=====================================================================

; Intel: device ID, the fastest Ultra DMA mode, and which kind.
iht_intel:
        dw      1230h
        db      IH_NOUDMA, IH_PIIX      ; 82371FB  PIIX
        dw      7010h
        db      IH_NOUDMA, IH_PIIX      ; 82371SB  PIIX3
        dw      7111h
        db      2, IH_PIIX              ; 82371AB/EB/MB  PIIX4, 4E, 4M
        dw      7199h
        db      2, IH_PIIX              ; 82443MX
        dw      2411h
        db      4, IH_ICH               ; 82801AA  ICH
        dw      2421h
        db      2, IH_ICH               ; 82801AB  ICH0
        dw      244Ah
        db      5, IH_ICH               ; 82801BAM ICH2-M
        dw      244Bh
        db      5, IH_ICH               ; 82801BA  ICH2
        dw      245Bh
        db      5, IH_ICH               ; 82801E   C-ICH
        dw      248Ah
        db      5, IH_ICH               ; 82801CAM ICH3-M
        dw      248Bh
        db      5, IH_ICH               ; 82801CA  ICH3
        dw      24C1h
        db      5, IH_ICH               ; 82801DBL ICH4-L
        dw      24CAh
        db      5, IH_ICH               ; 82801DBM ICH4-M
        dw      24CBh
        db      5, IH_ICH               ; 82801DB  ICH4
        dw      24DBh
        db      5, IH_ICH               ; 82801EB  ICH5
        dw      25A2h
        db      5, IH_ICH               ; 6300ESB
        dw      266Fh
        db      5, IH_ICH               ; 82801FB  ICH6
        dw      27DFh
        db      5, IH_ICH               ; 82801GB  ICH7
        dw      0

; The cycle time bits for each mode.  Odd modes are 01 and even ones
; 10, except mode 0; the clock is what tells 1 from 3 from 5.
ih_icycle   db  0, 1, 2, 1, 2, 1, 0

; VIA: the ISA bridge's device ID, the revisions that are this part,
; the fastest mode, and which row of ih_vcycle times it.
iht_via:
        dw      0586h
        db      00h, 0Fh, IH_NOUDMA, 0  ; VT82C586
        dw      0586h
        db      20h, 2Fh, 2, 0          ; VT82C586A
        dw      0586h
        db      30h, 4Fh, 2, 0          ; VT82C586B
        dw      0596h
        db      00h, 0Fh, 2, 0          ; VT82C596A
        dw      0596h
        db      10h, 2Fh, 4, 1          ; VT82C596B
        dw      0686h
        db      00h, 0Fh, 2, 0          ; VT82C686
        dw      0686h
        db      10h, 2Fh, 4, 1          ; VT82C686A
        dw      0686h
        db      40h, 4Fh, 5, 2          ; VT82C686B
        dw      8231h
        db      00h, 2Fh, 5, 2          ; VT8231
        dw      3074h
        db      00h, 2Fh, 5, 2          ; VT8233
        dw      3109h
        db      00h, 2Fh, 5, 2          ; VT8233C
        dw      3147h
        db      00h, 2Fh, 6, 3          ; VT8233A
        dw      3177h
        db      00h, 2Fh, 6, 3          ; VT8235
        dw      3227h
        db      00h, 2Fh, 6, 3          ; VT8237
        dw      3337h
        db      00h, 2Fh, 6, 3          ; VT8237A
        dw      0

; A drive's byte for each mode, seven to a row.  The low bits count
; clocks in a cycle, less two, and the clock is 33MHz in the first
; row, 66 in the second, 100 in the third and 133 in the last - so
; the same 60ns of mode 2 is two clocks, four, six and eight.  E0h is
; "Ultra DMA, on, and set from here"; the 66 bridges want bit 3 too.
; A mode past the end of its row is never asked for.
ih_vcycle   db  0E2h, 0E1h, 0E0h, 0E0h, 0E0h, 0E0h, 0E0h
            db  0EEh, 0ECh, 0EAh, 0E9h, 0E8h, 0E8h, 0E8h
            db  0E7h, 0E6h, 0E4h, 0E3h, 0E1h, 0E0h, 0E0h
            db  0E7h, 0E7h, 0E6h, 0E4h, 0E2h, 0E1h, 0E0h

; ...and the byte that turns it off, a row each.
ih_voff     db  03h, 0Fh, 07h, 07h

; A drive's data timing byte for multiword DMA, five to a row: a row
; for each multiword mode, a column for the drive's fastest PIO mode.
; The pulse less one above the recovery less one, in clocks of the PCI
; bus at 33MHz, which these registers count on every bridge in the
; table.  Each is the ATA standard's minimum times for the multiword
; mode and the PIO mode together, the longer of each, rounded up to
; whole clocks and then stretched to the longer of the two cycles -
; 120ns at mode 2 and PIO 4 is three clocks and one, 20h.
ih_vmw      db  99h, 77h, 77h, 77h, 77h ; multiword 0
            db  99h, 65h, 33h, 22h, 21h ; multiword 1
            db  99h, 65h, 33h, 22h, 20h ; multiword 2

; Intel.  The PIO mode each multiword mode is timed as...
ih_mwpio    db  0, 3, 4
; ...and that PIO mode's two fields, the IORDY sample point above the
; recovery time, as SIDETIM spells them: 3 and 1 clocks for PIO 4 is
; 10b over 11b.  PIO 0 and 1 are not in it: the slowest the fields can
; say is 5 and 4 clocks, 270ns, and multiword 0 wants 480.  A drive in
; that mode gets the TIME bit off, the compatible timing every PIIX
; has for the oldest drives, which is slower than either.
ih_itim     db  00h, 00h, 04h, 09h, 0Bh

;=====================================================================
; State: the selected host's, as ideh_sel last copied it out of
; ata_hosts.  Nothing here is a controller's own record.
;=====================================================================
ideh_dev    dd  0                       ; its configuration address
ideh_kind   db  0                       ; IH_*
ideh_top    db  IH_NOUDMA               ; its fastest mode, if it is known
ideh_row    db  0                       ; VIA: the row of ih_vcycle
ideh_mw     db  0                       ; its multiword timing is written
