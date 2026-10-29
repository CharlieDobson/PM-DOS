;=====================================================================
; AHCI.INC - SATA hard disks on an AHCI host adapter
;
; A board from 2004 on can run its SATA ports in one of three ways,
; and the BIOS setup says which.  "IDE" or "compatible" makes them
; look like the two channels at 1F0h and 170h, and ATA.INC drives
; them already.  "Enhanced" or "native" moves those channels to PCI
; addresses ATA.INC does not probe.  "AHCI" has no task file at all:
; the disk is reached through command lists in memory that the
; adapter fetches by bus mastering, and this file is what does that.
; Until it existed an AHCI disk went by INT 13h, through the BIOS's
; own AHCI driver and the 8K staging buffer - correct, and slow.
;
; THE BIOS IS USING THE SAME PORTS, and that decides most of what is
; here.  Its INT 13h code keeps a command list of its own and has
; pointed each port at it; the port has one list pointer, and when the
; kernel points it at ours the BIOS's next command is fetched from our
; list - whatever we last put there, which may have been a write.  So:
;
;   - nothing is changed at boot.  ahci_init reads the registers and
;     writes nothing but the adapter's PCI command bits, and the ports
;     stay the BIOS's until PMDOS asks, after CONFIG.SYS (AHF_START).
;     AHCI=OFF and a safe-mode start never ask, and the disks go on
;     being the BIOS's exactly as before this file;
;
;   - a port is only taken for a disk that IS a BIOS drive, found by
;     reading sector 0 through the BIOS first and then through the
;     port.  That also gives each disk the number the BIOS gave it -
;     which is the boot order its setup screen chose, and need not be
;     port order;
;
;   - everything the BIOS had in a port's registers is kept, and put
;     back whenever the machine is handed to real mode (a park) or
;     restarted.  The geometry INT 13h/08h gives is asked once, before
;     the take, and never again (BLOCK.INC caches it); a DOS box's INT
;     13h is never reflected to the ROM for one of these disks
;     (DOS\INT13.INC asks AHF_OWNS);
;
;   - a port that stops answering is given back to the BIOS.  The
;     block layer then sends that drive's sectors by INT 13h again, so
;     a driver fault costs speed, not the disk.
;
; ONE COMMAND AT A TIME, IN SLOT 0, POLLED.  The port's interrupt is
; left off and completion is the slot's bit in PxCI going down.  There
; is no queueing to gain from: the block layer asks for one transfer
; and waits for it, as it does of ATA.INC.
;
; THE MEMORY IS PMIO'S OWN TAIL.  The stub blits IO_PM_MAX bytes up to
; IO_PM_BASE whatever the file's length, so between the end of the
; image and IO_LIMIT there is room nobody uses - linear, and identity
; mapped, so its addresses are what goes on the bus.  The tables and
; the report are carved from there (AHM_*) rather than declared, which
; keeps 18K of zeros out of PMIO.SYS and off the boot floppy.  IOPM.ASM
; checks at assembly time that the image leaves room for it.
;
; ONE ADAPTER, the first one the PCI scan found, and up to AHCI_MAXREC
; of its ports.  A second one on the same board (a JMicron beside the
; south bridge) is for eSATA as a rule, and the block layer has only
; two hard-disk numbers to give out anyway.
;
; A CD-ROM DRIVE ON A PORT is a CD-ROM unit (CDUNIT.INC, CUK_AHCI):
; the command block goes in the command table's ATAPI area and the
; adapter moves the data, by the drive's DMA for whole blocks and by
; its PIO otherwise (ahci_cdexec).  The drives are taken with the disks
; after CONFIG.SYS, except in two cases:
;
;   - the BIOS is reading a disk image from a CD - 80h (GV_HDEMU) or
;     A: (asked here, INT 13h AX=4B01h) - through one of these ports.
;     Then no CD-ROM port is taken, and the drives stay the BIOS's;
;
;   - the machine started from a CD with no emulation (GV_CDBOOT).
;     The kernel has to read that disc for CONFIG.SYS, so the drives
;     are taken before the boot drive is looked for (AHF_CDSTART), and
;     AHCI= and a safe-mode start cannot keep them from the kernel.
;
; Under Windows an AHCI CD-ROM drive is not served: RMCD.SYS knows only
; IDE channels, and the park gives the port back to the BIOS.
;
; Included by IOPM.ASM after BLOCK.INC, whose geometry cache and BIOS
; path it uses while the BIOS still owns the disks.
;=====================================================================

;--- the adapter's registers, from ABAR (BAR5) ------------------------
AHC_CAP         EQU     00h
AHC_GHC         EQU     04h
AHC_IS          EQU     08h             ; a port's interrupt pending, a bit
                                        ; each, write 1 to clear
AHC_PI          EQU     0Ch             ; which ports are there, a bit each
AHC_VS          EQU     10h
AHC_CAP2        EQU     24h             ; from version 1.2
AHC_BOHC        EQU     28h             ; the BIOS/OS hand-off, if CAP2 says
AHC_PORT0       EQU     100h            ; port 0's registers
AHC_MAPLEN      EQU     100h + 32 * 80h ; all the ports there can be

AHC_CAP_SCLO    EQU     01000000h       ; it can clear a stuck BSY (CLO)
AHC_GHC_AE      EQU     80000000h       ; AHCI mode on
AHC_CAP2_BOH    EQU     00000001h       ; there is a BOHC
AHC_BOHC_BOS    EQU     00000001h       ; the BIOS owns it
AHC_BOHC_OOS    EQU     00000002h       ; the OS asks to

;--- a port's, from AHC_PORT0 + 80h * its number ----------------------
AHX_CLB         EQU     00h             ; the command list, 1K aligned
AHX_CLBU        EQU     04h
AHX_FB          EQU     08h             ; where received FISes go, 256 aligned
AHX_FBU         EQU     0Ch
AHX_IS          EQU     10h             ; interrupt status, write 1 to clear
AHX_IE          EQU     14h
AHX_CMD         EQU     18h
AHX_TFD         EQU     20h             ; the device's status and error
AHX_SIG         EQU     24h             ; what the device said it is
AHX_SSTS        EQU     28h             ; the link
AHX_SERR        EQU     30h             ; write 1 to clear
AHX_CI          EQU     38h             ; commands issued, a bit a slot

AHX_CMD_ST      EQU     00000001h       ; the engine runs
AHX_CMD_CLO     EQU     00000008h       ; command list override
AHX_CMD_FRE     EQU     00000010h       ; FISes are received
AHX_CMD_FR      EQU     00004000h       ; ...and are
AHX_CMD_CR      EQU     00008000h       ; the engine is running
AHX_CMD_KEEP    EQU     0FFFFFFFh       ; all but ICC, which acts when written

AHX_IS_ERR      EQU     79000000h       ; task file, host bus data and
                                        ; fatal, interface fatal, overflow
AHX_IS_HOST     EQU     39000000h       ; ...all of those but the task file:
                                        ; the adapter's faults, not the
                                        ; device's answer
AHX_TFD_ERR     EQU     21h             ; ERR, DF
AHX_TFD_BUSY    EQU     88h             ; BSY, DRQ
AHX_DET_LIVE    EQU     3               ; SSTS 3:0 - a device, link up
AHX_SIG_DISK    EQU     00000101h
AHX_SIG_ATAPI   EQU     0EB140101h

;--- the command FIS -------------------------------------------------
AHD_H2D         EQU     27h             ; register FIS, host to device
AHD_CBIT        EQU     80h             ; ...carrying a command
AHD_IDENT       EQU     0ECh
AHD_FLUSH       EQU     0E7h            ; FLUSH CACHE: see ATA.INC for why
                                        ; not the EXT form
AHD_RD28        EQU     0C8h            ; READ DMA
AHD_WR28        EQU     0CAh            ; WRITE DMA
AHD_RD48        EQU     025h            ; READ DMA EXT
AHD_WR48        EQU     035h            ; WRITE DMA EXT
AHD_PACKET      EQU     0A0h            ; PACKET: a command block follows
AHD_IDPKT       EQU     0A1h            ; IDENTIFY PACKET DEVICE
AHD_DEVOBS      EQU     0A0h            ; the device byte of a packet command:
                                        ; the two bits that are always set

; what sa_build is asked to put in it (AH)
AHB_WRITE       EQU     01h             ; the data goes to the device
AHB_LBA         EQU     02h             ; an address and a count
AHB_48          EQU     04h             ; ...in the 48-bit registers
AHB_DATA        EQU     08h             ; one region, at [ahci_dbuf]

;--- the tail of PMIO, carved -----------------------------------------
; A port's 2K: its command list (only slot 0's 32 bytes are ever used,
; but the adapter is owed the whole 1K), the FIS area, the command
; table with its one region, and a sector's room for the probe.
AHM_CLB         EQU     000h
AHM_FIS         EQU     400h
AHM_CT          EQU     500h            ; the FIS, then the region at +80h
AHM_SEC         EQU     600h
AHM_PORT        EQU     800h

AHCI_MAXREC     EQU     8               ; ports looked at

; a port's private half, beside its AHP_ record in the report
AHQ_REGS        EQU     0               ; dd - its registers, mapped
AHQ_MEM         EQU     4               ; dd - its 2K
AHQ_CLBU        EQU     8               ; dd - the BIOS's, as found: CLB and
AHQ_FB          EQU     12              ;      CMD are in the report
AHQ_FBU         EQU     16
AHQ_IE          EQU     20
AHQ_LEN         EQU     32

.ERRE   AHCI_MAXREC EQ AHR_MAXPORT, <the AHCI report and the driver disagree>

AHM_BIOS        EQU     AHCI_MAXREC * AHM_PORT  ; sector 0 of 80h and 81h,
                                                ; as the BIOS read them
AHM_REP         EQU     AHM_BIOS + 400h         ; the report, AHR_LEN
AHM_PRV         EQU     AHM_REP + 300h          ; and what is not in it
AHM_NEED        EQU     AHM_PRV + AHCI_MAXREC * AHQ_LEN

.ERRE   AHR_LEN LE 300h, <the AHCI report outgrew its room>

AHCI_CHUNK      EQU     2048            ; sectors in one command: 1MB, one
                                        ; region (a region holds 4MB)
AHC_LOOPMS      EQU     1000            ; spins of a wait in a millisecond,
                                        ; each an I/O read of port 80h: a
                                        ; microsecond on the ISA side of a
                                        ; real board, so these are floors
AHCI_CMDMS      EQU     10000           ; a command, with a spin-up in it
AHCI_PKTMS      EQU     20000           ; a packet command: a disc spinning
                                        ; up, or the drive working out what
                                        ; was just put in it
AHCI_PKTMAX     EQU     400000h         ; bytes one region carries

; a command header's first dword, beyond its FIS length
AHH_ATAPI       EQU     20h             ; a command block is in the table
AHH_WRITE       EQU     40h             ; the data goes to the device

;=====================================================================
; ahci_init - from blk_init.  Find the adapter, map it, and write down
;             what is on each port.  Changes nothing on the ports.
;=====================================================================
ahci_init:
        pushad
        mov     byte ptr [ahci_units],0
        mov     eax,OFFSET pm_end + 3FFh        ; the tail, 1K aligned for
        and     eax,NOT 3FFh                    ; the command lists
        mov     dword ptr [ahci_mem],eax
        lea     edx,[eax+AHM_REP]
        mov     dword ptr [ahci_rep],edx
        lea     edx,[eax+AHM_PRV]
        mov     dword ptr [ahci_prv],edx
        mov     edi,eax                         ; whatever the blit brought
        mov     ecx,AHM_NEED / 4                ; up with it, gone
        xor     eax,eax
        cld
        rep     stosd
        mov     ebp,dword ptr [ahci_rep]

        ; mass storage, SATA, AHCI 1.0
        xor     esi,esi
        mov     edi,PCT_CLASS
        mov     edx,0FFFFFF00h
        mov     ecx,01060100h
        call    pcx_match                       ; -> EBX
        jc      san_out
        mov     dword ptr [ebp+AHR_PCI],ebx
        mov     al,PCIR_VENDOR
        call    pci_read
        mov     dword ptr [ebp+AHR_ID],eax
        mov     byte ptr [ebp+AHR_STATE],AHS_NOREGS

        mov     dl,5                            ; ABAR
        call    pcx_bar
        jc      san_out
        test    edx,BARF_IO OR BARF_NONE
        jnz     san_out
        or      edi,edi                         ; above 4GB, where a 32-bit
        jnz     san_out                         ; kernel cannot follow
        mov     dword ptr [ebp+AHR_ABAR],eax
        mov     ebx,eax
        mov     ecx,AHC_MAPLEN
        call    pg_mapphys
        jc      san_out
        mov     dword ptr [ahci_abar],eax

        ; memory decoding and bus mastering, which a BIOS that uses the
        ; adapter has on already
        mov     ebx,dword ptr [ebp+AHR_PCI]
        mov     al,PCIR_COMMAND
        call    pci_read
        mov     ecx,eax
        and     ecx,0000FFFFh
        or      ecx,PCIC_MEMEN OR PCIC_BUSMASTER
        mov     al,PCIR_COMMAND
        call    pci_write

        mov     edi,dword ptr [ahci_abar]
        mov     eax,[edi+AHC_CAP]
        mov     dword ptr [ebp+AHR_CAP],eax
        mov     dword ptr [ahci_cap],eax
        mov     eax,[edi+AHC_GHC]
        mov     dword ptr [ebp+AHR_GHC],eax
        mov     eax,[edi+AHC_PI]
        mov     dword ptr [ebp+AHR_PI],eax
        mov     eax,[edi+AHC_VS]
        mov     dword ptr [ebp+AHR_VS],eax
        cmp     eax,00010200h                   ; CAP2 and BOHC are 1.2's
        jb      san_ver
        mov     eax,[edi+AHC_CAP2]
        mov     dword ptr [ebp+AHR_CAP2],eax
        test    eax,AHC_CAP2_BOH
        jz      san_ver
        mov     eax,[edi+AHC_BOHC]
        mov     dword ptr [ebp+AHR_BOHC],eax
san_ver:
        mov     byte ptr [ebp+AHR_STATE],AHS_FOUND

        ; the ports, in number order
        xor     ecx,ecx                         ; ECX = the port number
san_port:
        cmp     ecx,32
        jae     san_out
        bt      dword ptr [ebp+AHR_PI],ecx
        jnc     san_next
        movzx   eax,byte ptr [ebp+AHR_NREC]
        cmp     eax,AHCI_MAXREC
        jae     san_out
        push    ecx
        mov     ecx,eax
        call    sa_recat                        ; -> EBX = its record,
        pop     ecx                             ;    ESI = its private half
        mov     eax,ecx
        shl     eax,7
        lea     edx,[edi+eax+AHC_PORT0]         ; EDX = its registers
        mov     dword ptr [esi+AHQ_REGS],edx
        movzx   eax,byte ptr [ebp+AHR_NREC]
        imul    eax,eax,AHM_PORT
        add     eax,dword ptr [ahci_mem]
        mov     dword ptr [esi+AHQ_MEM],eax
        mov     byte ptr [ebx+AHP_NUM],cl
        mov     eax,[edx+AHX_SSTS]
        mov     dword ptr [ebx+AHP_SSTS],eax
        mov     eax,[edx+AHX_SIG]
        mov     dword ptr [ebx+AHP_SIG],eax
        mov     eax,[edx+AHX_TFD]
        mov     dword ptr [ebx+AHP_TFD],eax
        mov     eax,[edx+AHX_CLB]
        mov     dword ptr [ebx+AHP_BCLB],eax
        mov     eax,[edx+AHX_CMD]
        mov     dword ptr [ebx+AHP_BCMD],eax

        mov     byte ptr [ebx+AHP_STAGE],'n'
        mov     al,byte ptr [ebx+AHP_SSTS]
        and     al,0Fh
        cmp     al,AHX_DET_LIVE
        jne     san_kept
        mov     byte ptr [ebx+AHP_STAGE],'u'
        mov     eax,dword ptr [ebx+AHP_SIG]
        cmp     eax,AHX_SIG_DISK
        jne     san_atapi
        or      byte ptr [ebx+AHP_FLAGS],AHPF_DISK
        mov     byte ptr [ebx+AHP_STAGE],'f'
        jmp     short san_kept
san_atapi:
        cmp     eax,AHX_SIG_ATAPI
        jne     san_kept
        or      byte ptr [ebx+AHP_FLAGS],AHPF_ATAPI
        mov     byte ptr [ebx+AHP_STAGE],'p'
san_kept:
        inc     byte ptr [ebp+AHR_NREC]
san_next:
        inc     ecx
        jmp     san_port
san_out:
        popad
        ret

;=====================================================================
; ahci_svc - IOS_AHCI.  AL = an AHF_* function.
;=====================================================================
ahci_svc:
        cmp     al,AHF_REPORT
        je      ahci_report
        cmp     al,AHF_START
        je      ahci_start
        cmp     al,AHF_OWNS
        je      sa_fowns
        cmp     al,AHF_CDSTART
        je      ahci_cdstart
        stc
        ret

;--- AHF_OWNS: DL = a BIOS drive -> CF clear if it is one of ours -----
; ...or, while a CD-ROM port is ours, any number past the BIOS's hard
; disks.  A BIOS can give a CD-ROM drive an INT 13h number - the one the
; machine started from always, and SeaBIOS every one, as E0h and up -
; and reaches it through its port, which may be one whose list pointer
; is now the kernel's.  Which number is which port nothing says, so
; none of them goes to the ROM.
sa_fowns:
        push    eax
        push    edx
        mov     al,dl
        call    ahci_owns
        jnc     safo_out
        cmp     byte ptr [ahci_cds],0
        je      safo_no
        movzx   edx,dl
        cmp     edx,80h                         ; a diskette
        jb      safo_no
        movzx   eax,byte ptr ds:[BDA_HDCOUNT]
        add     eax,80h
        cmp     edx,eax
        jae     safo_out                        ; CF clear: past the disks
safo_no:
        stc
safo_out:
        pop     edx
        pop     eax
        ret

;--- AHF_REPORT: EDI -> AHR_LEN bytes ---------------------------------
ahci_report:
        pushad
        mov     esi,dword ptr [ahci_rep]
        mov     ecx,AHR_LEN / 4
        cld
        rep     movsd
        popad
        clc
        ret

;=====================================================================
; ahci_start - AHF_START, after CONFIG.SYS.  The disks the BIOS knows
;              by a hard-disk number this kernel has no driver for yet
;              are taken from it, each under the number it had.
;
; IN THIS ORDER, because of the shared list pointer: every BIOS read
; first, while every port is still the BIOS's; then the ports are taken
; and read through; then whatever matched nothing is given back.  No
; BIOS disk call is made while any port is ours.  The CD-ROM drives
; come last, and the BIOS is asked first whether one of them is serving
; a disk image (sa_cdask).
;
; After a start from a CD the CD-ROM ports are ours already
; (AHF_CDSTART), and the disks' reads by INT 13h are made with them
; taken: the BIOS reaches a disk through the disk's own port.
;=====================================================================
ahci_start:
        pushad
        cmp     byte ptr [ahci_dkgo],0          ; once
        jne     sax_out
        mov     byte ptr [ahci_dkgo],1
        mov     ebp,dword ptr [ahci_rep]
        mov     al,byte ptr [ebp+AHR_STATE]
        cmp     al,AHS_FOUND
        je      sax_go
        cmp     al,AHS_STARTED                  ; the CD-ROM drives went first
        jne     sax_out
sax_go:
        mov     byte ptr [ebp+AHR_STATE],AHS_STARTED

;--- 1: what the BIOS reads as sector 0 of 80h and 81h ---------------
        xor     edx,edx                         ; EDX = 0 for 80h, 1 for 81h
sax_bios:
        lea     eax,[edx+80h]                   ; AL = the drive
        cmp     al,USB_DRV0
        jae     sax_biosdone
        cmp     dl,byte ptr ds:[BDA_HDCOUNT]
        jae     sax_biosdone
        or      edx,edx
        jnz     sax_noemu
        cmp     byte ptr ds:[GV_HDEMU],0        ; 80h is a CD's disk image
        jne     sax_bnext
sax_noemu:
        call    ata_owns                        ; an IDE disk's number
        jnc     sax_bnext
        call    blk_getgeo                      ; the geometry, cached for
        jc      sax_bnext                       ; good while it can be asked
        mov     ecx,[ebx+GEO_LBATOT]
        mov     dword ptr [ebp+AHR_BTOT0+edx*4],ecx
        mov     edi,edx
        shl     edi,9
        add     edi,dword ptr [ahci_mem]
        add     edi,AHM_BIOS
        xor     ebx,ebx
        mov     ecx,1
        push    edx
        call    blk_read                        ; by INT 13h, still
        pop     edx
        jc      sax_bnext
        bts     dword ptr [ebp+AHR_BIOSOK],edx
sax_bnext:
        inc     edx
        cmp     edx,2
        jb      sax_bios
sax_biosdone:
        call    sa_cdask                        ; a BIOS question as well
        cmp     byte ptr [ebp+AHR_BIOSOK],0
        jne     sax_hba
        cmp     byte ptr [ahci_cdok],0
        je      sax_out                         ; no number left to give, and
                                                ; no CD-ROM drive to take

;--- 2: the adapter, from the BIOS ------------------------------------
sax_hba:
        call    sa_hba
        jc      sax_out
        cmp     byte ptr [ebp+AHR_BIOSOK],0
        je      sax_cds

;--- 3: each disk port taken, and its sector 0 read through it --------
        xor     ecx,ecx
sax_take:
        mov     eax,dword ptr [ahci_rep]
        cmp     cl,byte ptr [eax+AHR_NREC]
        jae     sax_match
        call    sa_recat                        ; -> EBX, ESI
        mov     ebp,ebx
        test    byte ptr [ebp+AHP_FLAGS],AHPF_DISK
        jz      sax_tnext
        mov     byte ptr [ebp+AHP_STAGE],'t'
        call    sa_take
        jc      sax_tnext
        mov     byte ptr [ebp+AHP_STAGE],'i'
        call    sa_ident
        jc      sax_tgive
        mov     byte ptr [ebp+AHP_STAGE],'r'
        mov     eax,dword ptr [esi+AHQ_MEM]
        add     eax,AHM_SEC
        mov     dword ptr [ahci_dbuf],eax
        mov     dword ptr [ahci_lba],0
        mov     dword ptr [ahci_count],1
        mov     byte ptr [ahci_dir],0
        call    sa_rwcmd
        call    sa_build
        call    sa_issue
        jc      sax_tgive
        mov     byte ptr [ebp+AHP_STAGE],'m'

        ; which of the BIOS's sectors is this one
        push    ecx
        push    esi
        xor     edx,edx
sax_cmp:
        mov     eax,dword ptr [ahci_rep]
        bt      dword ptr [eax+AHR_BIOSOK],edx
        jnc     sax_cnext
        mov     esi,dword ptr [ahci_dbuf]
        mov     edi,edx
        shl     edi,9
        add     edi,dword ptr [ahci_mem]
        add     edi,AHM_BIOS
        mov     ecx,512 / 4
        cld
        repe    cmpsd
        jne     sax_cnext
        bts     dword ptr [ebp+AHP_MATCH],edx
sax_cnext:
        inc     edx
        cmp     edx,2
        jb      sax_cmp
        pop     esi
        pop     ecx
        jmp     short sax_tnext
sax_tgive:
        test    byte ptr [ebp+AHP_FLAGS],AHPF_TAKEN     ; unless the failure
        jz      sax_tnext                               ; gave it back itself
        call    sa_give
sax_tnext:
        inc     ecx
        jmp     sax_take

;--- 4: a number for each port that is exactly one BIOS drive ---------
sax_match:
        xor     edx,edx
sax_mlp:
        mov     eax,dword ptr [ahci_rep]
        bt      dword ptr [eax+AHR_BIOSOK],edx
        jnc     sax_mnext
        call    sa_pick                         ; -> ECX, or CF
        jc      sax_mnext
        call    sa_recat
        lea     eax,[edx+80h]
        mov     byte ptr [ebx+AHP_DRIVE],al
        or      byte ptr [ebx+AHP_FLAGS],AHPF_OWNED
        mov     byte ptr [ebx+AHP_STAGE],'o'
        inc     byte ptr [ahci_units]
        mov     eax,dword ptr [ahci_rep]
        inc     byte ptr [eax+AHR_UNITS]
sax_mnext:
        inc     edx
        cmp     edx,2
        jb      sax_mlp

;--- 5: and the rest back to the BIOS ---------------------------------
        xor     ecx,ecx
sax_back:
        mov     eax,dword ptr [ahci_rep]
        cmp     cl,byte ptr [eax+AHR_NREC]
        jae     sax_cds
        call    sa_recat
        mov     ebp,ebx
        test    byte ptr [ebp+AHP_FLAGS],AHPF_TAKEN
        jz      sax_bknext
        test    byte ptr [ebp+AHP_FLAGS],AHPF_OWNED OR AHPF_CD
        jnz     sax_bknext                      ; a disk of ours, or a CD-ROM
        call    sa_give                         ; drive AHF_CDSTART took

sax_bknext:
        inc     ecx
        jmp     sax_back

;--- 6: the CD-ROM drives, if sa_cdask said they may be taken ---------
sax_cds:
        call    sa_cdtake
sax_out:
        popad
        clc
        ret

;=====================================================================
; ahci_cdstart - AHF_CDSTART, from DOSMAIN before disk_scan, after a
;                start from a CD with no emulation: the CD-ROM drives
;                only, so that the disc the machine started from can
;                be found and read for CONFIG.SYS.  The disks wait for
;                AHF_START as always.
;=====================================================================
ahci_cdstart:
        pushad
        mov     ebp,dword ptr [ahci_rep]
        cmp     byte ptr [ebp+AHR_STATE],AHS_FOUND
        jne     sxc_out
        call    sa_anycd
        jc      sxc_out
        mov     byte ptr [ahci_cdok],1          ; no image to leave alone:
        call    sa_hba                          ; BOOTCD started with none
        jc      sxc_out
        mov     byte ptr [ebp+AHR_STATE],AHS_STARTED
        call    sa_cdtake
sxc_out:
        popad
        clc
        ret

;---------------------------------------------------------------------
; sa_hba - the adapter, from the BIOS: in AHCI mode, and handed over
;          if it has a hand-off register.  Once; CF if it cannot be
;          had, and then AHR_STATE says why.
;---------------------------------------------------------------------
sa_hba:
        pushad
        cmp     byte ptr [ahci_hbaok],0
        jne     sxh_ok
        mov     ebp,dword ptr [ahci_rep]
        mov     edi,dword ptr [ahci_abar]
        test    dword ptr [edi+AHC_GHC],AHC_GHC_AE
        jnz     sxh_ae                          ; not in AHCI mode: whatever the
        mov     byte ptr [ebp+AHR_STATE],AHS_NOAE       ; BIOS does with it,
        jmp     short sxh_no                    ; it is not through lists
sxh_ae:
        test    dword ptr [ebp+AHR_CAP2],AHC_CAP2_BOH
        jz      sxh_mine
        or      dword ptr [edi+AHC_BOHC],AHC_BOHC_OOS
        mov     ebx,AHC_BOHC
        mov     edx,AHC_BOHC_BOS
        mov     ecx,2000                        ; 25ms, and two seconds more
        call    sa_wait                         ; if it is busy
        jnc     sxh_mine
        mov     byte ptr [ebp+AHR_STATE],AHS_NOHAND
        jmp     short sxh_no
sxh_mine:
        mov     byte ptr [ahci_hbaok],1
sxh_ok:
        popad
        clc
        ret
sxh_no:
        popad
        stc
        ret

;---------------------------------------------------------------------
; sa_anycd - CF if no port has a packet device's signature.
;---------------------------------------------------------------------
sa_anycd:
        push    eax
        push    ebx
        push    ecx
        push    esi
        xor     ecx,ecx
sxy_lp:
        mov     eax,dword ptr [ahci_rep]
        cmp     cl,byte ptr [eax+AHR_NREC]
        jae     sxy_none
        call    sa_recat
        test    byte ptr [ebx+AHP_FLAGS],AHPF_ATAPI
        jnz     sxy_one
        inc     ecx
        jmp     short sxy_lp
sxy_none:
        stc
sxy_one:                                        ; CF clear from the TEST
        pop     esi
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; sa_cdask - may the CD-ROM drives be taken?  [ahci_cdok] = 1 if so.
;            While every port is still the BIOS's: it asks the BIOS.
;
; NOT WHILE THE BIOS IS READING A DISK IMAGE FROM A CD.  It does that
; through the drive's port, and every INT 13h for the image would run
; the kernel's last command instead.  80h as an image is GV_HDEMU,
; which IORM asked about.  A: as one is asked here: a start from A:
; is a start from a diskette or from a CD's diskette image, and the El
; Torito status call (INT 13h AX=4B01h) says which.  The ports with a
; packet signature then stay the BIOS's, marked 'e'.
;---------------------------------------------------------------------
sa_cdask:
        pushad
        mov     byte ptr [ahci_cdok],0
        call    sa_anycd
        jc      sxk_out
        cmp     byte ptr ds:[GV_HDEMU],0
        jne     sxk_emu
        cmp     byte ptr ds:[GV_BOOTDRV],80h
        jae     sxk_yes
        mov     edi,GV_EDP
        xor     eax,eax
        mov     ecx,32 / 4
        cld
        rep     stosd
        mov     byte ptr ds:[GV_EDP],13h        ; the packet's size, which some
                                                ; BIOSes look at before filling it
        call    rm_clear
        mov     word ptr ds:[RMR_EAX],4B01h
        mov     al,byte ptr ds:[GV_BOOTDRV]
        mov     byte ptr ds:[RMR_EDX],al
        mov     word ptr ds:[RMR_DS],IO_SEG
        mov     dword ptr ds:[RMR_ESI],GVO_EDP
        mov     al,13h
        call    rm_int
        call    rm_cf
        jnz     sxk_yes                         ; CF: nothing is emulated
        test    byte ptr ds:[GV_EDP+ET_MEDIA],0Fh
        jz      sxk_yes                         ; 0: no emulation
sxk_emu:
        xor     ecx,ecx
sxk_mark:
        mov     eax,dword ptr [ahci_rep]
        cmp     cl,byte ptr [eax+AHR_NREC]
        jae     sxk_out
        call    sa_recat
        test    byte ptr [ebx+AHP_FLAGS],AHPF_ATAPI
        jz      sxk_next
        mov     byte ptr [ebx+AHP_STAGE],'e'
sxk_next:
        inc     ecx
        jmp     short sxk_mark
sxk_yes:
        mov     byte ptr [ahci_cdok],1
sxk_out:
        popad
        ret

;---------------------------------------------------------------------
; sa_cdtake - every packet device's port not yet tried, as a CD-ROM
;             unit, if [ahci_cdok] says it may be.  The unit table is
;             built again when any was taken.  A port is tried once:
;             after AHF_CDSTART, AHF_START finds none still at 'p'.
;---------------------------------------------------------------------
sa_cdtake:
        pushad
        cmp     byte ptr [ahci_cdok],0
        je      sxt_out
        mov     byte ptr [ahci_cdok],0
        xor     ecx,ecx
        xor     edx,edx                         ; EDX = how many were taken
sxt_lp:
        mov     eax,dword ptr [ahci_rep]
        cmp     cl,byte ptr [eax+AHR_NREC]
        jae     sxt_done
        call    sa_recat                        ; -> EBX, ESI
        mov     ebp,ebx
        cmp     byte ptr [ebp+AHP_STAGE],'p'    ; a packet device, untried
        jne     sxt_next
        call    sa_cdone
        jc      sxt_next
        inc     edx
sxt_next:
        inc     ecx
        jmp     short sxt_lp
sxt_done:
        or      edx,edx
        jz      sxt_out
        call    cun_build
sxt_out:
        popad
        ret

;---------------------------------------------------------------------
; sa_cdone - ESI, EBP: one packet device's port taken, and kept if
;            IDENTIFY PACKET DEVICE says a CD-ROM drive taking twelve-
;            byte command blocks is on it.  CF if not, and then the
;            port is the BIOS's again.
;---------------------------------------------------------------------
sa_cdone:
        pushad
        mov     byte ptr [ebp+AHP_STAGE],'w'
        call    sa_take
        jc      sxo_fail                        ; given back already
        mov     byte ptr [ebp+AHP_STAGE],'k'
        mov     edi,dword ptr [esi+AHQ_MEM]
        add     edi,AHM_SEC
        mov     dword ptr [ahci_dbuf],edi
        mov     dword ptr [ahci_count],1
        mov     al,AHD_IDPKT
        mov     ah,AHB_DATA
        call    sa_build
        call    sa_issue
        jc      sxo_give
        mov     edi,dword ptr [ahci_dbuf]
        mov     ax,word ptr [edi]               ; word 0
        mov     dx,ax
        and     dx,0C000h
        cmp     dx,08000h                       ; a packet device
        jne     sxo_other
        mov     dl,ah
        and     dl,1Fh
        cmp     dl,5                            ; ...that is a CD-ROM drive
        jne     sxo_other
        test    al,3                            ; ...taking twelve bytes
        jnz     sxo_other
        test    word ptr [edi+49*2],0100h       ; DMA, which it may still be
        jz      sxo_pio                         ; given PIO instead of
        or      byte ptr [ebp+AHP_FLAGS],AHPF_PDMA
sxo_pio:
        call    sa_model
        or      byte ptr [ebp+AHP_FLAGS],AHPF_CD
        mov     byte ptr [ebp+AHP_STAGE],'c'
        inc     byte ptr [ahci_cds]
        popad
        clc
        ret
sxo_other:
        mov     byte ptr [ebp+AHP_STAGE],'q'
sxo_give:
        test    byte ptr [ebp+AHP_FLAGS],AHPF_TAKEN     ; unless the failure
        jz      sxo_fail                                ; gave it back itself
        call    sa_give
sxo_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; sa_pick - EDX = 0 or 1: the one taken, unnumbered port whose sector 0
;           is what the BIOS read from 80h + EDX.  -> ECX = its record.
;           CF if there is none, or more than one and their sizes do
;           not settle it - a disk and its clone, say, which the kernel
;           has no way to tell apart and so leaves to the BIOS.
;---------------------------------------------------------------------
sa_pick:
        push    eax
        push    ebx
        push    esi
        push    edi
        push    ebp
        xor     edi,edi                         ; EDI = pass: 1 counts size
sap_pass:
        xor     ebp,ebp                         ; EBP = how many fit
        xor     ecx,ecx
sap_lp:
        mov     eax,dword ptr [ahci_rep]
        cmp     cl,byte ptr [eax+AHR_NREC]
        jae     sap_counted
        call    sa_recat
        test    byte ptr [ebx+AHP_FLAGS],AHPF_TAKEN
        jz      sap_next
        test    byte ptr [ebx+AHP_FLAGS],AHPF_OWNED
        jnz     sap_next
        bt      dword ptr [ebx+AHP_MATCH],edx
        jnc     sap_next
        or      edi,edi
        jz      sap_fits
        mov     eax,dword ptr [eax+AHR_BTOT0+edx*4]
        or      eax,eax                         ; the BIOS gave no size
        jz      sap_next
        cmp     eax,dword ptr [ebx+AHP_TOTAL]
        jne     sap_next
sap_fits:
        inc     ebp
        mov     dword ptr [ahci_found],ecx
sap_next:
        inc     ecx
        jmp     sap_lp
sap_counted:
        cmp     ebp,1
        je      sap_one
        jb      sap_none
        or      edi,edi
        jnz     sap_none
        inc     edi
        jmp     sap_pass
sap_one:
        mov     ecx,dword ptr [ahci_found]
        pop     ebp
        pop     edi
        pop     esi
        pop     ebx
        pop     eax
        clc
        ret
sap_none:
        pop     ebp
        pop     edi
        pop     esi
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; sa_recat - ECX = a record's index -> EBX = the record, ESI = its
;            private half.
;---------------------------------------------------------------------
sa_recat:
        mov     ebx,ecx
        imul    ebx,ebx,AHP_LEN
        add     ebx,dword ptr [ahci_rep]
        add     ebx,AHR_PORTS
        mov     esi,ecx
        imul    esi,esi,AHQ_LEN
        add     esi,dword ptr [ahci_prv]
        ret

;=====================================================================
; ahci_park - AL = 0 before the machine is handed to real mode, 1 after
;             it comes back (box_park, BUS.INC).  Going, every port the
;             kernel has is given back as the BIOS left it; coming back
;             it is taken again, and one that cannot be is the BIOS's
;             from then on.  Everything kept.
; ahci_halt - the ports given back for a restart through the ROM.
;=====================================================================
ahci_halt:
        push    eax
        xor     al,al
        call    ahci_park
        pop     eax
        ret

ahci_park:
        pushad
        mov     dl,al
        mov     al,byte ptr [ahci_units]
        or      al,byte ptr [ahci_cds]
        jz      sak_out
        xor     ecx,ecx
sak_lp:
        mov     eax,dword ptr [ahci_rep]
        cmp     cl,byte ptr [eax+AHR_NREC]
        jae     sak_out
        call    sa_recat
        mov     ebp,ebx
        test    byte ptr [ebp+AHP_FLAGS],AHPF_OWNED OR AHPF_CD
        jz      sak_next
        or      dl,dl
        jnz     sak_back
        test    byte ptr [ebp+AHP_FLAGS],AHPF_TAKEN
        jz      sak_next                        ; given back already
        call    sa_give
        jmp     short sak_next
sak_back:
        test    byte ptr [ebp+AHP_FLAGS],AHPF_TAKEN
        jnz     sak_next
        call    sa_take
        jnc     sak_next
        mov     al,'j'
        test    byte ptr [ebp+AHP_FLAGS],AHPF_CD
        jz      sak_lost
        mov     al,'l'
sak_lost:
        call    sa_lose
        mov     byte ptr [ebp+AHP_STAGE],al
sak_next:
        inc     ecx
        jmp     sak_lp
sak_out:
        popad
        ret

;---------------------------------------------------------------------
; sa_lose - EBP = an owned record whose port has gone back to the BIOS
;           for good: the number is the BIOS's again.  A CD-ROM unit
;           keeps its place in the unit table, and every command on it
;           fails from then on as a drive with nothing in it.
;---------------------------------------------------------------------
sa_lose:
        push    eax
        test    byte ptr [ebp+AHP_FLAGS],AHPF_OWNED
        jz      sal_cd
        and     byte ptr [ebp+AHP_FLAGS],NOT AHPF_OWNED
        dec     byte ptr [ahci_units]
        mov     eax,dword ptr [ahci_rep]
        dec     byte ptr [eax+AHR_UNITS]
sal_cd:
        test    byte ptr [ebp+AHP_FLAGS],AHPF_CD
        jz      sal_out
        and     byte ptr [ebp+AHP_FLAGS],NOT AHPF_CD
        dec     byte ptr [ahci_cds]
sal_out:
        pop     eax
        ret

;=====================================================================
; ahci_owns - CF clear if BIOS drive AL is a disk this driver handles.
;             Everything kept.
; ahci_capacity - AL = BIOS drive -> EBX = total sectors, or CF.
;=====================================================================
ahci_owns:
        push    esi
        push    ebp
        call    sa_slot
        pop     ebp
        pop     esi
        ret

ahci_capacity:
        push    esi
        push    ebp
        call    sa_slot
        jc      sac_out
        mov     ebx,dword ptr [ebp+AHP_TOTAL]
sac_out:
        pop     ebp
        pop     esi
        ret

;---------------------------------------------------------------------
; sa_slot - AL = BIOS drive -> EBP = its record, ESI = its private
;           half.  CF if no port of ours is that drive.
;---------------------------------------------------------------------
sa_slot:
        cmp     byte ptr [ahci_units],0         ; the machine without one
        je      sas_no                          ; pays one compare
        push    ecx
        mov     ebp,dword ptr [ahci_rep]
        movzx   ecx,byte ptr [ebp+AHR_NREC]
        add     ebp,AHR_PORTS
        mov     esi,dword ptr [ahci_prv]
sas_lp:
        jecxz   sas_none
        test    byte ptr [ebp+AHP_FLAGS],AHPF_OWNED
        jz      sas_next
        cmp     al,byte ptr [ebp+AHP_DRIVE]
        je      sas_hit
sas_next:
        add     ebp,AHP_LEN
        add     esi,AHQ_LEN
        dec     ecx
        jmp     short sas_lp
sas_hit:
        pop     ecx
        clc
        ret
sas_none:
        pop     ecx
sas_no:
        stc
        ret

;---------------------------------------------------------------------
; ahci_cdat - ECX = a port record's index -> EBX = the record.  CF past
;             the last, or before ahci_init has run.  For cun_build.
;---------------------------------------------------------------------
ahci_cdat:
        mov     ebx,dword ptr [ahci_rep]
        or      ebx,ebx
        jz      sxa_none
        cmp     cl,byte ptr [ebx+AHR_NREC]
        jae     sxa_none
        push    esi
        call    sa_recat
        pop     esi
        clc
        ret
sxa_none:
        stc
        ret

;=====================================================================
; ahci_cdexec - the AHCI transport for CDUNIT.INC: run the command
;               block in cun_cdb against the unit in ESI.
;
;   EDI = buffer, ECX = its size (zero for a command with no data)
;   -> EAX = bytes moved, CF on failure with cun_skey/asc/ascq set
;
; THE ADAPTER IS HANDED THE CALLER'S BUFFER when it can be: a word
; aligned address outside the band ata_dmaok describes, an even
; length, and no more than one region's 4MB.  Otherwise it is handed
; XFER_BUF, 8K at a time.  A read longer than one command carries -
; READ(10), READ(12) or READ CD, which have a block address and a
; count - is cut into pieces as scs_cdexec cuts one.  Anything else
; that long is refused as an illegal request; nothing here sends one.
;
; THE CALLER'S BLOCK IS PUT BACK as it was, because cun_svcpkt retries
; a unit attention with it.  Every other register is kept.
;=====================================================================
ahci_cdexec:
        pushad
        mov     [axe_buf],edi
        mov     [axe_len],ecx
        mov     dword ptr [axe_got],0
        mov     byte ptr [cun_skey],0
        mov     byte ptr [cun_asc],0
        mov     byte ptr [cun_ascq],0
        push    esi
        mov     esi,OFFSET cun_cdb
        mov     edi,OFFSET axe_save
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb
        pop     esi
        mov     al,byte ptr [cun_dir]
        mov     byte ptr [axe_dsave],al

        movzx   ecx,byte ptr [esi+CUE_DEVIDX]
        call    sa_recat                        ; -> EBX = the record, ESI
        mov     ebp,ebx
        mov     al,byte ptr [ebp+AHP_FLAGS]     ; a unit of ours, and the port
        and     al,AHPF_CD OR AHPF_TAKEN        ; on our lists: never a command
        cmp     al,AHPF_CD OR AHPF_TAKEN        ; to a port whose list pointer
        jne     axe_gone                        ; is the BIOS's

        ; how much one command carries, and through which buffer
        mov     byte ptr [ahci_bounce],0
        mov     eax,AHCI_PKTMAX
        mov     ecx,[axe_len]
        or      ecx,ecx
        jz      axe_lim                         ; nothing to move
        test    cl,1
        jnz     axe_bnc
        mov     edx,[axe_buf]
        push    eax
        mov     eax,ecx
        call    sa_dmaspan
        pop     eax
        jnc     axe_lim
axe_bnc:
        mov     byte ptr [ahci_bounce],1
        mov     eax,XFER_SIZE
axe_lim:
        mov     [axe_max],eax
        cmp     ecx,eax
        ja      axe_cut
        mov     edi,[axe_buf]
        call    sa_pone                         ; ECX = the length
        mov     [axe_got],eax
        jc      axe_said
        jmp     axe_ok

;--- longer than one command: a read, in pieces -----------------------
axe_cut:
        mov     al,byte ptr [axe_save]
        movzx   ebx,byte ptr [axe_save+7]       ; READ(10): the count in 7-8
        shl     ebx,8
        mov     bl,byte ptr [axe_save+8]
        cmp     al,SCSI_READ10
        je      axc_count
        movzx   ebx,byte ptr [axe_save+6]       ; READ CD: 6-8
        shl     ebx,8
        mov     bl,byte ptr [axe_save+7]
        shl     ebx,8
        mov     bl,byte ptr [axe_save+8]
        cmp     al,SCSI_READCD
        je      axc_count
        cmp     al,SCSI_READ12
        jne     axe_refuse
        shl     ebx,8                           ; READ(12): 6-9
        mov     bl,byte ptr [axe_save+9]
axc_count:
        or      ebx,ebx
        jz      axe_refuse
        mov     [axc_left],ebx
        mov     eax,[axe_len]                   ; each block's size, which has
        xor     edx,edx                         ; to come out even
        div     ebx
        or      edx,edx
        jnz     axe_refuse
        or      eax,eax
        jz      axe_refuse
        mov     [axc_size],eax
        mov     ecx,eax
        mov     eax,[axe_max]
        xor     edx,edx
        div     ecx                             ; EAX = blocks a command
        or      eax,eax
        jz      axe_refuse
        mov     [axc_per],eax
        movzx   eax,byte ptr [axe_save+2]       ; the first block, 2-5
        shl     eax,8
        mov     al,byte ptr [axe_save+3]
        shl     eax,8
        mov     al,byte ptr [axe_save+4]
        shl     eax,8
        mov     al,byte ptr [axe_save+5]
        mov     [axc_lba],eax
        mov     eax,[axe_buf]
        mov     [axc_at],eax
axc_piece:
        mov     ecx,[axc_per]
        cmp     ecx,[axc_left]
        jbe     axc_have
        mov     ecx,[axc_left]
axc_have:
        mov     [axc_n],ecx
        mov     eax,[axc_lba]
        mov     byte ptr [cun_cdb+5],al
        shr     eax,8
        mov     byte ptr [cun_cdb+4],al
        shr     eax,8
        mov     byte ptr [cun_cdb+3],al
        shr     eax,8
        mov     byte ptr [cun_cdb+2],al
        mov     eax,ecx
        cmp     byte ptr [axe_save],SCSI_READ10
        je      axc_r10
        cmp     byte ptr [axe_save],SCSI_READCD
        je      axc_rcd
        mov     byte ptr [cun_cdb+9],al         ; READ(12)
        mov     byte ptr [cun_cdb+8],ah
        shr     eax,16
        mov     byte ptr [cun_cdb+7],al
        mov     byte ptr [cun_cdb+6],ah
        jmp     short axc_go
axc_rcd:
        mov     byte ptr [cun_cdb+8],al
        mov     byte ptr [cun_cdb+7],ah
        shr     eax,16
        mov     byte ptr [cun_cdb+6],al
        jmp     short axc_go
axc_r10:
        mov     byte ptr [cun_cdb+8],al
        mov     byte ptr [cun_cdb+7],ah
axc_go:
        imul    ecx,dword ptr [axc_size]        ; ECX = this piece's bytes
        mov     [axc_want],ecx
        mov     edi,[axc_at]
        call    sa_pone
        jc      axc_bad
        add     [axe_got],eax
        cmp     eax,[axc_want]
        jb      axe_ok                          ; short: the drive stopped there
        mov     eax,[axc_n]
        add     [axc_lba],eax
        sub     [axc_left],eax
        mov     eax,[axc_want]
        add     [axc_at],eax
        cmp     dword ptr [axc_left],0
        jne     axc_piece
        jmp     short axe_ok
axc_bad:
        add     [axe_got],eax
        jmp     short axe_said

axe_refuse:
        mov     byte ptr [cun_skey],5           ; ILLEGAL REQUEST, never sent
        jmp     short axe_fail
axe_gone:
        mov     byte ptr [cun_skey],2           ; NOT READY: the port has gone
        jmp     short axe_fail                  ; back to the BIOS
axe_said:
        call    sa_psense                       ; why, while the port is ours
axe_fail:
        call    axe_put
        popad
        mov     eax,[axe_got]
        stc
        ret
axe_ok:
        call    axe_put
        popad
        mov     eax,[axe_got]
        clc
        ret

;--- the caller's block and direction, back as they came --------------
axe_put:
        push    eax
        push    ecx
        push    esi
        push    edi
        mov     esi,OFFSET axe_save
        mov     edi,OFFSET cun_cdb
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb
        mov     al,byte ptr [axe_dsave]
        mov     byte ptr [cun_dir],al
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; sa_pone - ESI, EBP: one packet command, the block in cun_cdb.
;
;   EDI = the caller's buffer, ECX = its length, [ahci_bounce] = 1 to
;   go through XFER_BUF (ECX is then at most XFER_SIZE)
;   -> EAX = bytes moved, CF if it failed
;
; AN ODD LENGTH IS MADE EVEN: a region's count must be.  Only through
; XFER_BUF, where the extra byte is the buffer's and not the caller's;
; ahci_cdexec sends every odd length that way.
;---------------------------------------------------------------------
sa_pone:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     [axp_cbuf],edi
        mov     [axp_len],ecx
        mov     edx,edi
        cmp     byte ptr [ahci_bounce],0
        je      axp_go
        mov     edx,XFER_BUF
        cmp     byte ptr [cun_dir],0
        je      axp_go
        push    esi                             ; data to send: down into the
        mov     esi,edi                         ; buffer first
        mov     edi,XFER_BUF
        cld
        rep     movsb
        test    byte ptr [axp_len],1
        jz      axp_even
        mov     byte ptr [edi],0                ; and the pad byte a zero
axp_even:
        pop     esi
axp_go:
        mov     dword ptr [ahci_dbuf],edx
        mov     ecx,[axp_len]
        inc     ecx
        and     ecx,NOT 1
        mov     dword ptr [ahci_plen],ecx
        call    sa_pbuild
        mov     ecx,AHCI_PKTMS
        call    sa_issuems
        jc      axp_bad
        mov     edi,dword ptr [esi+AHQ_MEM]
        mov     eax,dword ptr [edi+AHM_CLB+4]   ; what the adapter moved
        or      eax,eax
        jnz     axp_got
        cmp     byte ptr [ahci_pdma],0          ; a bus master that left the
        je      axp_got                         ; count alone moved it all, as
        mov     eax,[axp_len]                   ; atapi_xfer takes it to
axp_got:
        cmp     eax,[axp_len]
        jbe     axp_fits
        mov     eax,[axp_len]                   ; the pad byte is not the caller's
axp_fits:
        cmp     byte ptr [ahci_bounce],0
        je      axp_ok
        cmp     byte ptr [cun_dir],0
        jne     axp_ok
        push    eax                             ; a reply: up out of the buffer
        mov     ecx,eax
        mov     esi,XFER_BUF
        mov     edi,[axp_cbuf]
        cld
        rep     movsb
        pop     eax
axp_ok:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
axp_bad:
        xor     eax,eax
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; sa_pbuild - ESI, EBP: slot 0's header and the command table for a
;             PACKET command with the block in cun_cdb, [ahci_plen]
;             bytes at [ahci_dbuf], the direction in cun_dir.
;             -> [ahci_pdma] = 1 if the drive is to move it by DMA.
;
; BY DMA ONLY FOR WHOLE BLOCKS COMING IN, from a drive that has it, and
; not when the caller asked for PIO (cun_pio): atapi_xfer's rule.  By
; PIO the adapter does the drive's bursts itself, and the byte count
; limit only says how big each may be.
;---------------------------------------------------------------------
sa_pbuild:
        pushad
        mov     ebx,dword ptr [esi+AHQ_MEM]
        mov     edi,ebx
        xor     eax,eax
        mov     ecx,32 / 4                      ; the header
        cld
        rep     stosd
        lea     edx,[ebx+AHM_CT]                ; EDX -> the command table
        mov     edi,edx
        mov     ecx,(80h + 10h) / 4             ; the FIS, the block, a region
        rep     stosd

        mov     byte ptr [edx+0],AHD_H2D
        mov     byte ptr [edx+1],AHD_CBIT
        mov     byte ptr [edx+2],AHD_PACKET
        mov     byte ptr [edx+7],AHD_DEVOBS
        lea     edi,[edx+40h]                   ; the table's ATAPI area
        mov     esi,OFFSET cun_cdb
        mov     ecx,ATAPI_PKTLEN
        rep     movsb

        mov     byte ptr [ahci_pdma],0
        mov     eax,5 OR AHH_ATAPI              ; the FIS is five dwords
        mov     ecx,dword ptr [ahci_plen]
        or      ecx,ecx
        jz      sxp_put
        or      eax,00010000h                   ; one region
        cmp     byte ptr [cun_dir],0
        je      sxp_in
        or      al,AHH_WRITE
        jmp     short sxp_pio
sxp_in:
        cmp     byte ptr [cun_pio],0
        jne     sxp_pio
        test    byte ptr [ebp+AHP_FLAGS],AHPF_PDMA
        jz      sxp_pio
        test    ecx,ATAPI_BLKSIZE - 1
        jnz     sxp_pio
        mov     byte ptr [edx+3],01h            ; features: DMA
        mov     byte ptr [ahci_pdma],1
        jmp     short sxp_region
sxp_pio:
        push    ecx                             ; the byte count limit: even,
        cmp     ecx,0FFFEh                      ; and at most FFFEh
        jbe     sxp_bcl
        mov     ecx,0FFFEh
sxp_bcl:
        mov     byte ptr [edx+5],cl
        mov     byte ptr [edx+6],ch
        pop     ecx
sxp_region:
        push    eax
        mov     eax,dword ptr [ahci_dbuf]
        mov     dword ptr [edx+80h],eax
        pop     eax
        dec     ecx                             ; a byte count less one
        mov     dword ptr [edx+8Ch],ecx
sxp_put:
        mov     dword ptr [ebx+AHM_CLB+0],eax
        mov     dword ptr [ebx+AHM_CLB+8],edx   ; the table, identity mapped
        popad
        ret

;---------------------------------------------------------------------
; sa_psense - ESI, EBP: a packet command has just failed.  REQUEST
;             SENSE, into cun_skey/cun_asc/cun_ascq.  All kept.
;
; The sense key is also the top half of the drive's error register,
; and that stands if REQUEST SENSE gets no answer.  The block it was
; asked about is ahci_cdexec's to put back.
;---------------------------------------------------------------------
sa_psense:
        pushad
        test    byte ptr [ebp+AHP_FLAGS],AHPF_CD
        jz      sxq_out                         ; the failure lost the port
        mov     al,byte ptr [ebp+AHP_TFD+1]
        shr     al,4
        mov     byte ptr [cun_skey],al
        mov     al,byte ptr [cun_pio]
        mov     byte ptr [axs_pio],al
        mov     al,byte ptr [ahci_bounce]
        mov     byte ptr [axs_bnc],al
        call    cun_cdbzero                     ; and the direction with it
        mov     byte ptr [cun_cdb+0],SCSI_REQSENSE
        mov     byte ptr [cun_cdb+4],18         ; a fixed-format reply
        mov     byte ptr [cun_pio],1
        mov     byte ptr [ahci_bounce],0
        mov     edi,dword ptr [esi+AHQ_MEM]
        add     edi,AHM_SEC
        mov     ecx,18
        call    sa_pone
        jc      sxq_back
        cmp     eax,14                          ; ASC and ASCQ are at 12, 13
        jb      sxq_back
        mov     al,byte ptr [edi+2]
        and     al,0Fh
        mov     byte ptr [cun_skey],al
        mov     al,byte ptr [edi+12]
        mov     byte ptr [cun_asc],al
        mov     al,byte ptr [edi+13]
        mov     byte ptr [cun_ascq],al
sxq_back:
        mov     al,byte ptr [axs_pio]
        mov     byte ptr [cun_pio],al
        mov     al,byte ptr [axs_bnc]
        mov     byte ptr [ahci_bounce],al
sxq_out:
        popad
        ret

;=====================================================================
; ahci_read / ahci_write - the entry points BLOCK.INC dispatches to.
;
;   AL = BIOS drive   EBX = LBA   ECX = sector count   EDI = buffer
;
; CF set if this drive is not ours, or the transfer failed.
;
; THE ADAPTER HAS NO PIO, so a buffer it cannot be handed - the same
; band ATA.INC's ata_dmaok describes, or an odd address - is bounced
; through XFER_BUF, sixteen sectors at a time.  Nothing else uses that
; buffer while a disk of ours is being read: it is the BIOS path's,
; and the BIOS is not asked about our disks.
;=====================================================================
ahci_read:
        mov     byte ptr [ahci_dir],0
        jmp     short sa_go
ahci_write:
        mov     byte ptr [ahci_dir],1
sa_go:
        pushad
        call    sa_slot                         ; -> EBP, ESI
        jc      sao_fail
        mov     dword ptr [ahci_lba],ebx
        mov     dword ptr [ahci_left],ecx
        mov     dword ptr [ahci_cbuf],edi
sao_more:
        mov     ecx,dword ptr [ahci_left]
        or      ecx,ecx
        jz      sao_done

        ; how much one command carries
        mov     eax,AHCI_CHUNK
        test    byte ptr [ebp+AHP_FLAGS],AHPF_LBA48
        jnz     sao_cap
        mov     eax,256
sao_cap:
        cmp     ecx,eax
        jbe     sao_have
        mov     ecx,eax
sao_have:
        test    byte ptr [ebp+AHP_FLAGS],AHPF_LBA48
        jnz     sao_reach
        mov     eax,dword ptr [ahci_lba]        ; a 28-bit drive cannot be
        add     eax,ecx                         ; asked past 2^28, and the
        jc      sao_fail                        ; command would say a
        cmp     eax,10000000h                   ; different sector if it were
        ja      sao_fail
sao_reach:
        mov     byte ptr [ahci_bounce],0
        mov     edx,dword ptr [ahci_cbuf]
        call    sa_dmaok
        jnc     sao_direct
        mov     byte ptr [ahci_bounce],1
        cmp     ecx,XFER_SIZE / 512
        jbe     sao_bsize
        mov     ecx,XFER_SIZE / 512
sao_bsize:
        mov     edx,XFER_BUF
        cmp     byte ptr [ahci_dir],0
        je      sao_direct
        push    ecx                             ; a write: the sectors down
        push    esi                             ; into the buffer first
        mov     esi,dword ptr [ahci_cbuf]
        mov     edi,XFER_BUF
        shl     ecx,7
        cld
        rep     movsd
        pop     esi
        pop     ecx
sao_direct:
        mov     dword ptr [ahci_count],ecx
        mov     dword ptr [ahci_dbuf],edx
        call    sa_rwcmd
        call    sa_build
        call    sa_issue
        jc      sao_fail

        cmp     byte ptr [ahci_dir],0
        jne     sao_flush
        cmp     byte ptr [ahci_bounce],0
        je      sao_next
        push    esi                             ; a read: up out of it
        mov     esi,XFER_BUF
        mov     edi,dword ptr [ahci_cbuf]
        mov     ecx,dword ptr [ahci_count]
        shl     ecx,7
        cld
        rep     movsd
        pop     esi
        jmp     short sao_next
sao_flush:
        mov     al,AHD_FLUSH                    ; or the drive's cache may
        xor     ah,ah                           ; be all that holds it
        call    sa_build
        call    sa_issue
        jc      sao_fail
sao_next:
        mov     eax,dword ptr [ahci_count]
        add     dword ptr [ahci_lba],eax
        sub     dword ptr [ahci_left],eax
        shl     eax,9
        add     dword ptr [ahci_cbuf],eax
        jmp     sao_more
sao_done:
        popad
        clc
        ret
sao_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; sa_dmaok - can the adapter be handed EDX for ECX sectors?  CF if not.
;            ata_dmaok's rule, for the same reasons.
; sa_dmaspan - ...EDX for EAX bytes.
;---------------------------------------------------------------------
sa_dmaok:
        push    eax
        mov     eax,ecx
        shl     eax,9
        call    sa_dmaspan
        pop     eax
        ret

sa_dmaspan:
        push    eax
        test    dl,1                            ; a region starts on a word
        jnz     saq_no
        add     eax,edx                         ; EAX = one past the end
        jc      saq_no
        cmp     eax,0A0000h
        jbe     saq_yes
        cmp     edx,DOS_BASE
        jb      saq_no
saq_yes:
        pop     eax
        clc
        ret
saq_no:
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; sa_rwcmd - EBP = the record -> AL = the read or write command for
;            [ahci_dir], AH = what sa_build is to put with it.
;---------------------------------------------------------------------
sa_rwcmd:
        test    byte ptr [ebp+AHP_FLAGS],AHPF_LBA48
        jnz     sam_48
        mov     ax,((AHB_LBA OR AHB_DATA) SHL 8) OR AHD_RD28
        cmp     byte ptr [ahci_dir],0
        je      sam_out
        mov     ax,((AHB_LBA OR AHB_DATA OR AHB_WRITE) SHL 8) OR AHD_WR28
        ret
sam_48:
        mov     ax,((AHB_LBA OR AHB_DATA OR AHB_48) SHL 8) OR AHD_RD48
        cmp     byte ptr [ahci_dir],0
        je      sam_out
        mov     ax,((AHB_LBA OR AHB_DATA OR AHB_48 OR AHB_WRITE) SHL 8) OR AHD_WR48
sam_out:
        ret

;---------------------------------------------------------------------
; sa_build - ESI = the port's private half, AL = the command, AH = AHB_*.
;            [ahci_lba] and [ahci_count] are the address and the count,
;            [ahci_dbuf] where the data is.  Slot 0's header and the
;            command table, made from nothing.
;---------------------------------------------------------------------
sa_build:
        pushad
        mov     ebx,eax                         ; BL = command, BH = AHB_*
        mov     edi,dword ptr [esi+AHQ_MEM]
        push    edi
        xor     eax,eax
        mov     ecx,32 / 4                      ; the header
        cld
        rep     stosd
        pop     edi
        lea     edx,[edi+AHM_CT]                ; EDX -> the command table
        push    edi
        mov     edi,edx
        mov     ecx,(80h + 10h) / 4             ; the FIS room and one region
        rep     stosd
        pop     edi

        mov     byte ptr [edx+0],AHD_H2D
        mov     byte ptr [edx+1],AHD_CBIT
        mov     byte ptr [edx+2],bl
        test    bh,AHB_LBA
        jz      sab_hdr
        mov     eax,dword ptr [ahci_lba]
        mov     byte ptr [edx+4],al             ; LBA 7:0
        mov     byte ptr [edx+5],ah             ; 15:8
        shr     eax,16
        mov     byte ptr [edx+6],al             ; 23:16
        mov     byte ptr [edx+7],40h            ; by LBA
        test    bh,AHB_48
        jnz     sab_48
        and     ah,0Fh                          ; 27:24 in the device byte
        or      byte ptr [edx+7],ah
        jmp     short sab_count
sab_48:
        mov     byte ptr [edx+8],ah             ; 31:24, and 47:32 are zero:
                                                ; a sector number is 32 bits
                                                ; everywhere above this
sab_count:
        mov     eax,dword ptr [ahci_count]
        mov     byte ptr [edx+12],al            ; 256 on a 28-bit command is
        test    bh,AHB_48                       ; 0, by arithmetic
        jz      sab_hdr
        mov     byte ptr [edx+13],ah
sab_hdr:
        mov     eax,5                           ; the FIS is five dwords
        test    bh,AHB_WRITE
        jz      sab_dir
        or      al,40h
sab_dir:
        test    bh,AHB_DATA
        jz      sab_put
        or      eax,00010000h                   ; one region
        mov     ecx,dword ptr [ahci_dbuf]
        mov     dword ptr [edx+80h],ecx
        mov     ecx,dword ptr [ahci_count]
        shl     ecx,9
        dec     ecx                             ; a byte count less one
        mov     dword ptr [edx+8Ch],ecx
sab_put:
        mov     dword ptr [edi+AHM_CLB+0],eax
        mov     dword ptr [edi+AHM_CLB+8],edx   ; the table, identity mapped
        popad
        ret

;---------------------------------------------------------------------
; sa_issue - ESI = the port's private half, EBP = its record: run what
;            sa_build made, and wait for it.  CF if it failed, and then
;            the port has been put right (sa_recover) or given back.
; sa_issuems - the same, waiting ECX milliseconds.
;
; A PACKET DEVICE'S "NO" IS NOT COUNTED.  A CD-ROM drive answers a
; command it will not carry out - a disc changed, the tray open - with
; ERR in its status, which the adapter reports as a task file error
; like any other.  That is an answer, and REQUEST SENSE says what it
; was; AHP_ERRS counts the adapter's faults and the timeouts.
;---------------------------------------------------------------------
sa_issue:
        push    ecx
        mov     ecx,AHCI_CMDMS
        call    sa_issuems
        pop     ecx                             ; CF kept
        ret

sa_issuems:
        pushad
        mov     edi,dword ptr [esi+AHQ_REGS]
        mov     dword ptr [edi+AHX_IS],0FFFFFFFFh
        mov     dword ptr [edi+AHX_CI],1
        imul    ecx,ecx,AHC_LOOPMS
sai_lp:
        test    dword ptr [edi+AHX_IS],AHX_IS_ERR
        jnz     sai_bad
        test    dword ptr [edi+AHX_CI],1
        jz      sai_done
        in      al,80h
        dec     ecx
        jnz     sai_lp
        jmp     short sai_bad
sai_done:
        mov     eax,[edi+AHX_TFD]
        mov     dword ptr [ebp+AHP_TFD],eax
        test    al,AHX_TFD_ERR
        jnz     sai_bad
        test    dword ptr [edi+AHX_IS],AHX_IS_ERR
        jnz     sai_bad
        mov     dword ptr [edi+AHX_IS],0FFFFFFFFh
        popad
        clc
        ret
sai_bad:
        mov     eax,[edi+AHX_TFD]
        mov     dword ptr [ebp+AHP_TFD],eax
        mov     eax,[edi+AHX_IS]
        mov     dword ptr [ebp+AHP_IS],eax
        mov     eax,[edi+AHX_SERR]
        mov     dword ptr [ebp+AHP_SERR],eax
        test    byte ptr [ebp+AHP_FLAGS],AHPF_ATAPI
        jz      sai_count
        test    dword ptr [ebp+AHP_IS],AHX_IS_HOST
        jnz     sai_count
        test    byte ptr [ebp+AHP_TFD],80h      ; still busy: a timeout, and
        jnz     sai_count                       ; ERR means nothing yet
        test    byte ptr [ebp+AHP_TFD],01h      ; ERR: the drive said no
        jnz     sai_counted
sai_count:
        cmp     byte ptr [ebp+AHP_ERRS],0FFh
        je      sai_counted
        inc     byte ptr [ebp+AHP_ERRS]
sai_counted:
        call    sa_recover
        popad
        stc
        ret

;---------------------------------------------------------------------
; sa_recover - ESI, EBP: a command failed.  The engine stopped, the
;              errors cleared and the engine started again; a port
;              that will not do that goes back to the BIOS for good.
;---------------------------------------------------------------------
sa_recover:
        push    edi
        mov     edi,dword ptr [esi+AHQ_REGS]
        call    sa_stop
        jc      sav_lost
        call    sa_run
        jnc     sav_out
sav_lost:
        call    sa_give
        push    eax
        mov     al,'x'
        test    byte ptr [ebp+AHP_FLAGS],AHPF_CD
        jz      sav_disk
        mov     al,'y'
sav_disk:
        test    byte ptr [ebp+AHP_FLAGS],AHPF_OWNED OR AHPF_CD
        jz      sav_kept
        call    sa_lose
        mov     byte ptr [ebp+AHP_STAGE],al
sav_kept:
        pop     eax
sav_out:
        pop     edi
        ret

;---------------------------------------------------------------------
; sa_ident - ESI, EBP: IDENTIFY DEVICE into the port's sector room, and
;            what it says into the record.  CF if it failed, or the
;            disk's sectors are not 512 bytes.
;---------------------------------------------------------------------
sa_ident:
        pushad
        mov     edi,dword ptr [esi+AHQ_MEM]
        add     edi,AHM_SEC
        mov     dword ptr [ahci_dbuf],edi
        mov     dword ptr [ahci_count],1
        mov     al,AHD_IDENT
        mov     ah,AHB_DATA
        call    sa_build
        call    sa_issue
        jc      sad_fail
        mov     edi,dword ptr [ahci_dbuf]

        ; word 106 in use (bits 15-14 = 01) with bit 12: a logical
        ; sector longer than 256 words, which the block layer cannot move
        mov     ax,word ptr [edi+106*2]
        and     ax,0D000h
        cmp     ax,05000h
        jne     sad_512
        or      byte ptr [ebp+AHP_FLAGS],AHPF_NOT512
        mov     byte ptr [ebp+AHP_STAGE],'z'
        jmp     short sad_fail
sad_512:
        test    word ptr [edi+83*2],0400h
        jz      sad_28
        or      byte ptr [ebp+AHP_FLAGS],AHPF_LBA48
        mov     eax,dword ptr [edi+100*2]
        cmp     dword ptr [edi+102*2],0
        je      sad_total
        mov     eax,0FFFFFFFFh                  ; over 2^32 sectors: cap, as
        jmp     short sad_total                 ; ATA.INC does
sad_28:
        mov     eax,dword ptr [edi+60*2]
sad_total:
        or      eax,eax
        jz      sad_fail
        mov     dword ptr [ebp+AHP_TOTAL],eax
        call    sa_model
        popad
        clc
        ret
sad_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; sa_model - EDI = what IDENTIFY gave, EBP = the record: the model into
;            it, each word's two bytes swapped into reading order.
;            Everything kept.
;---------------------------------------------------------------------
sa_model:
        pushad
        lea     esi,[edi+27*2]
        lea     edi,[ebp+AHP_MODEL]
        mov     ecx,ATA_MODEL_LEN / 2
        cld
sad_mdl:
        lodsw
        xchg    al,ah
        stosw
        loop    sad_mdl
        popad
        ret

;=====================================================================
; THE PORT, BETWEEN THE BIOS AND US.
;
; sa_take - ESI, EBP: the BIOS's registers kept, the engine stopped and
;           started again on our lists.  CF if it would not stop or
;           start, and then everything has been put back.
; sa_give - the reverse: stopped, the BIOS's lists and interrupt
;           enables back, and its engine running again if it was.
;=====================================================================
sa_take:
        push    eax
        push    edi
        mov     edi,dword ptr [esi+AHQ_REGS]
        mov     eax,[edi+AHX_CLB]
        mov     dword ptr [ebp+AHP_BCLB],eax
        mov     eax,[edi+AHX_CMD]
        mov     dword ptr [ebp+AHP_BCMD],eax
        mov     eax,[edi+AHX_CLBU]
        mov     dword ptr [esi+AHQ_CLBU],eax
        mov     eax,[edi+AHX_FB]
        mov     dword ptr [esi+AHQ_FB],eax
        mov     eax,[edi+AHX_FBU]
        mov     dword ptr [esi+AHQ_FBU],eax
        mov     eax,[edi+AHX_IE]
        mov     dword ptr [esi+AHQ_IE],eax
        call    sa_stop
        jc      sat_back
        call    sa_run
        jc      sat_back
        or      byte ptr [ebp+AHP_FLAGS],AHPF_TAKEN
        pop     edi
        pop     eax
        clc
        ret
sat_back:
        or      byte ptr [ebp+AHP_FLAGS],AHPF_TAKEN     ; for sa_give
        call    sa_give
        pop     edi
        pop     eax
        stc
        ret

sa_give:
        push    eax
        push    edi
        mov     edi,dword ptr [esi+AHQ_REGS]
        call    sa_stop                         ; and if it would not, the
                                                ; lists go back regardless
        mov     eax,dword ptr [ebp+AHP_BCLB]
        mov     [edi+AHX_CLB],eax
        mov     eax,dword ptr [esi+AHQ_CLBU]
        mov     [edi+AHX_CLBU],eax
        mov     eax,dword ptr [esi+AHQ_FB]
        mov     [edi+AHX_FB],eax
        mov     eax,dword ptr [esi+AHQ_FBU]
        mov     [edi+AHX_FBU],eax
        mov     dword ptr [edi+AHX_SERR],0FFFFFFFFh
        mov     dword ptr [edi+AHX_IS],0FFFFFFFFh
        call    sa_hbais
        mov     eax,dword ptr [esi+AHQ_IE]
        mov     [edi+AHX_IE],eax
        test    dword ptr [ebp+AHP_BCMD],AHX_CMD_FRE
        jz      sag_out
        mov     eax,[edi+AHX_CMD]
        and     eax,AHX_CMD_KEEP
        or      eax,AHX_CMD_FRE
        mov     [edi+AHX_CMD],eax
        test    dword ptr [ebp+AHP_BCMD],AHX_CMD_ST
        jz      sag_out
        mov     eax,[edi+AHX_CMD]
        and     eax,AHX_CMD_KEEP
        or      eax,AHX_CMD_ST
        mov     [edi+AHX_CMD],eax
sag_out:
        and     byte ptr [ebp+AHP_FLAGS],NOT AHPF_TAKEN
        pop     edi
        pop     eax
        ret

;---------------------------------------------------------------------
; sa_stop - EDI = a port's registers: the engine and the FIS receiver
;           stopped, half a second each.  CF if one would not.
;---------------------------------------------------------------------
sa_stop:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ebx,AHX_CMD
        mov     eax,[edi+AHX_CMD]
        test    eax,AHX_CMD_ST
        jz      sst_st
        and     eax,(AHX_CMD_KEEP AND NOT AHX_CMD_ST)
        mov     [edi+AHX_CMD],eax
sst_st:
        mov     edx,AHX_CMD_CR
        mov     ecx,500
        call    sa_wait
        jc      sst_out
        mov     eax,[edi+AHX_CMD]
        test    eax,AHX_CMD_FRE
        jz      sst_fre
        and     eax,(AHX_CMD_KEEP AND NOT AHX_CMD_FRE)
        mov     [edi+AHX_CMD],eax
sst_fre:
        mov     edx,AHX_CMD_FR
        mov     ecx,500
        call    sa_wait
sst_out:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; sa_run - EDI = a stopped port's registers, ESI = its private half:
;          our lists in, the errors cleared, the receiver on, and once
;          the device is idle the engine.  CF if the device stayed busy.
;---------------------------------------------------------------------
sa_run:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     eax,dword ptr [esi+AHQ_MEM]
        mov     [edi+AHX_CLB],eax               ; AHM_CLB is at 0
        mov     dword ptr [edi+AHX_CLBU],0
        add     eax,AHM_FIS
        mov     [edi+AHX_FB],eax
        mov     dword ptr [edi+AHX_FBU],0
        mov     dword ptr [edi+AHX_IE],0        ; polled
        mov     dword ptr [edi+AHX_SERR],0FFFFFFFFh
        mov     dword ptr [edi+AHX_IS],0FFFFFFFFh
        call    sa_hbais
        mov     eax,[edi+AHX_CMD]
        and     eax,AHX_CMD_KEEP
        or      eax,AHX_CMD_FRE
        mov     [edi+AHX_CMD],eax

        mov     ebx,AHX_TFD
        mov     edx,AHX_TFD_BUSY
        mov     ecx,1000
        call    sa_wait
        jnc     sru_idle
        ; still busy from whatever failed: an adapter that can is told
        ; to take no notice, which is what CLO is for
        test    dword ptr [ahci_cap],AHC_CAP_SCLO
        jnz     sru_clo
        stc
        jmp     short sru_out
sru_clo:
        mov     eax,[edi+AHX_CMD]
        and     eax,AHX_CMD_KEEP
        or      eax,AHX_CMD_CLO
        mov     [edi+AHX_CMD],eax
        mov     ebx,AHX_CMD
        mov     edx,AHX_CMD_CLO
        mov     ecx,500
        call    sa_wait
        jc      sru_out
sru_idle:
        mov     eax,[edi+AHX_CMD]
        and     eax,AHX_CMD_KEEP
        or      eax,AHX_CMD_ST
        mov     [edi+AHX_CMD],eax
        clc
sru_out:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; sa_hbais - EBP = a port's record: its bit in the adapter's interrupt
;            status cleared.  Once PxIS is clear the bit is all that is
;            left of an interrupt the BIOS's last command raised, and
;            with GHC.IE on it holds the adapter's line - level
;            triggered on a PCI board - for as long as it stays set.
;---------------------------------------------------------------------
sa_hbais:
        push    eax
        push    ecx
        push    edx
        movzx   ecx,byte ptr [ebp+AHP_NUM]
        mov     eax,1
        shl     eax,cl
        mov     edx,dword ptr [ahci_abar]
        mov     [edx+AHC_IS],eax                ; write 1 to clear
        pop     edx
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; sa_wait - until the dword at [EDI+EBX] has none of the bits in EDX,
;           for ECX milliseconds at least.  CF if it never did.
;---------------------------------------------------------------------
sa_wait:
        push    eax
        push    ecx
        imul    ecx,ecx,AHC_LOOPMS
saw_lp:
        mov     eax,[edi+ebx]
        test    eax,edx
        jz      saw_ok
        in      al,80h
        dec     ecx
        jnz     saw_lp
        mov     eax,[edi+ebx]                   ; one last look
        test    eax,edx
        jz      saw_ok
        pop     ecx
        pop     eax
        stc
        ret
saw_ok:
        pop     ecx
        pop     eax
        clc
        ret

;=====================================================================
; State.  The tables and the report are in the tail (AHM_*); this is
; only what finds them.
;=====================================================================
ahci_mem    dd  0                       ; the tail, 1K aligned
ahci_rep    dd  0                       ; -> the report, AHR_LEN
ahci_prv    dd  0                       ; -> the private halves
ahci_abar   dd  0                       ; the registers, mapped
ahci_cap    dd  0                       ; CAP, kept for CLO
ahci_lba    dd  0                       ; the transfer in hand
ahci_left   dd  0
ahci_count  dd  0
ahci_cbuf   dd  0                       ; the caller's buffer
ahci_dbuf   dd  0                       ; what the adapter is handed
ahci_found  dd  0                       ; sa_pick's last fit
ahci_plen   dd  0                       ; a packet command's region, even
axe_buf     dd  0                       ; ahci_cdexec's caller's buffer,
axe_len     dd  0                       ; its length,
axe_got     dd  0                       ; what has been moved into it,
axe_max     dd  0                       ; and the most one command carries
axc_left    dd  0                       ; a read in pieces: blocks to go,
axc_size    dd  0                       ; bytes a block,
axc_per     dd  0                       ; blocks a piece,
axc_lba     dd  0                       ; the next one,
axc_at      dd  0                       ; where it goes,
axc_n       dd  0                       ; how many this piece,
axc_want    dd  0                       ; and its bytes
axp_cbuf    dd  0                       ; sa_pone's caller's buffer
axp_len     dd  0                       ; ...and its length
ahci_units  db  0                       ; ports with a number of ours
ahci_cds    db  0                       ; ports that are CD-ROM units of ours
ahci_dir    db  0                       ; 0 read, 1 write
ahci_bounce db  0                       ; this chunk goes by XFER_BUF
ahci_dkgo   db  0                       ; AHF_START has run
ahci_hbaok  db  0                       ; the adapter is ours (sa_hba)
ahci_cdok   db  0                       ; sa_cdtake may take the CD-ROM ports
ahci_pdma   db  0                       ; the packet command goes by DMA
axe_dsave   db  0                       ; the caller's direction, kept
axs_pio     db  0                       ; sa_psense keeps cun_pio
axs_bnc     db  0                       ; ...and ahci_bounce
axe_save    db  ATAPI_PKTLEN dup (0)    ; the caller's block, kept
ahci_pend   db  AHCI_MAXREC dup (0)     ; per port record, the listeners not
                                        ; yet told the disc moved (cun_pendall)
