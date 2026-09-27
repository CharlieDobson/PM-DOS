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
; (ideh_udma, ideh_noudma).
;
; The cable is the host's to report because the detect pin is wired
; to the board, not to the controller: the BIOS reads it through
; whatever general-purpose input the board's designer chose and leaves
; the answer in a register for the driver.  That is an Intel bit in
; one place and a VIA bit in another, and on an older VIA bridge it is
; nowhere at all - so "the host cannot say" is an answer too, and
; ata_cables goes on to ask the drives.
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

; VIA.  50h is the secondary slave and 53h the primary master.
IHV_UDMA        EQU     50h
IHV_CABLE       EQU     10h             ; in each byte, on the 100 and 133
                                        ; bridges: the BIOS saw 80 conductors
IHV_CLK66       EQU     00080008h       ; on the 66 bridges: run the fast clock

;=====================================================================
; ideh_probe - which host is this?  Called by ata_bminit once it has
;              the controller's address and ID.
;=====================================================================
ideh_probe:
        pushad
        mov     byte ptr [ideh_kind],IH_NONE
        mov     byte ptr [ideh_top],IH_NOUDMA
        mov     byte ptr [ideh_row],0
IFDEF IDEH_BLIND
        ; BUILD nohost: every host is a stranger.  The rules for one
        ; can then be tested on a board whose BIOS selects a fast mode,
        ; and the boards that do are the ones this file knows
        jmp     short ihp_stranger
ENDIF

        mov     eax,[ata_pciid]
        cmp     ax,8086h
        je      ihp_intel
        cmp     eax,05711106h
        je      ihp_via
ihp_stranger:
        popad
        ret

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
        popad
        ret

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
        jmp     short ihp_out
ihp_vnext:
        add     esi,6
        jmp     short ihp_vlp
ihp_out:
        popad
        ret

;=====================================================================
; ideh_index - ESI = device -> ECX = which of the host's four drives:
;              0 and 1 on the primary channel, 2 and 3 on the other.
;=====================================================================
ideh_index:
        xor     ecx,ecx
        cmp     word ptr [esi+ATD_BASE],1F0h
        je      ihx_unit
        mov     ecx,2
ihx_unit:
        test    byte ptr [esi+ATD_SEL],10h
        jz      ihx_out
        inc     ecx
ihx_out:
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
        mov     ebx,[ata_pcidev]
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
        movzx   edi,al                  ; EDI = the mode
        call    ideh_index              ; ECX = the drive
        mov     ebx,[ata_pcidev]
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
;
; The multiword timing itself is left alone.  On both families it is
; the PIO timing under another name, the BIOS set that for the drive
; at POST, and nothing here has made it wrong.
;=====================================================================
ideh_noudma:
        pushad
        cmp     byte ptr [ideh_top],IH_NOUDMA
        je      ihn_out                 ; it has none to stop
        call    ideh_index
        mov     ebx,[ata_pcidev]
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
        mov     ah,al
        and     ah,0F8h
        cmp     ah,40h
        jne     iha_no
        movzx   edi,al
        and     edi,07h                 ; EDI = the mode
        call    ideh_index              ; ECX = the drive
        mov     ebx,[ata_pcidev]
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
        jmp     short iha_yes

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

;=====================================================================
; State
;=====================================================================
ideh_kind   db  0                       ; IH_*
ideh_top    db  IH_NOUDMA               ; its fastest mode, if it is known
ideh_row    db  0                       ; VIA: the row of ih_vcycle
            db  0
