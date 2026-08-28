;=====================================================================
; ATA.INC - IDE/ATA hard-disk driver, polled PIO
;
; Talks to the drive controller directly from protected mode instead
; of going out through the INT 13h thunk.  That buys three things:
;
;   - no mode switch per request.  The BIOS path costs a switch to
;     real mode, a PIC reprogram to 08h/70h, the call, a reprogram
;     back to 50h/58h and a switch home, for every transfer.
;   - no bounce buffer.  PIO reads land straight in the caller's flat
;     32-bit buffer, so XFER_BUF and its 16-sector chunking go away.
;   - no CHS horizon.  LBA28 addresses 128G, LBA48 effectively all of
;     it, and neither needs a geometry translation to be right.
;
; Detection is by IDENTIFY DEVICE; anything that does not answer is
; left to the BIOS path in BLOCK.INC, which is what keeps DOSBox 0.74
; (no IDE controller at all) working.
;
; Interrupts are suppressed at the drive (nIEN) and transfers are
; polled, so no IRQ 14/15 handlers are needed.
;
; SINCE v0.43 THERE ARE THREE WAYS TO MOVE THE DATA, and the driver
; picks one per transfer:
;
;   - 16-bit PIO, REP INSW.  What it has always done, what every
;     machine can do, and the only path an ISA controller gets.
;   - 32-bit PIO, REP INSD.  Half the bus cycles for the same bytes.
;     The data register is sixteen bits wide at the drive; what makes
;     a doubleword access legal is the HOST ADAPTER latching two
;     words, which is a VLB and PCI thing and is not an ISA thing.
;   - bus-master DMA, which does not use the processor at all.  The
;     engine is on the controller and only configuration space knows
;     where its registers are, so this one needs PCI.INC.
;
; THE TWO DETECTIONS ARE COMPLEMENTARY AND THAT IS NOT A COINCIDENCE.
; IDENTIFY word 48 was defined in ATA-1 as "can do doubleword I/O"
; and the VLB-era drives set it; ATA-3 made the word obsolete, so a
; later drive reports zero there whatever it is plugged into.  A PCI
; IDE controller, on the other hand, handles a 32-bit access to the
; data port by construction.  Word 48 covers the drives too old to be
; on a PCI bus and the PCI controller covers the drives too new to
; set word 48, and between them they cover the machines this kernel
; runs on.
;
; NEITHER IS TURNED ON BY GUESSWORK ON A WRITE PATH.  See ata_pickdma
; for why an unselected transfer mode is left unselected, and
; ata_dmarun for what happens when a bus master gets it wrong: the
; device is demoted to PIO for the rest of the session and the
; transfer is done again.  A machine whose chipset lies gets a slow
; disk rather than a wrong one.
;
; Included by IOPM.ASM, after PCI.INC.
;=====================================================================

ATA_MAXDEV      EQU     4               ; two channels, master + slave

; Command block registers, as offsets from the channel's base port
ATA_DATA        EQU     0
ATA_ERROR       EQU     1               ; read
ATA_FEATURES    EQU     1               ; write
ATA_SECCNT      EQU     2
ATA_LBA0        EQU     3               ; LBA 7:0    (or sector number)
ATA_LBA1        EQU     4               ; LBA 15:8   (or cylinder low)
ATA_LBA2        EQU     5               ; LBA 23:16  (or cylinder high)
ATA_DEVSEL      EQU     6
ATA_STATUS      EQU     7               ; read
ATA_COMMAND     EQU     7               ; write

; Status register bits
ATA_ST_ERR      EQU     01h
ATA_ST_DRQ      EQU     08h
ATA_ST_DF       EQU     20h
ATA_ST_DRDY     EQU     40h
ATA_ST_BSY      EQU     80h

; Device control register (the separate control port)
ATA_DC_NIEN     EQU     02h             ; stop the drive asserting IRQ
ATA_DC_SRST     EQU     04h

; Commands
ATA_CMD_READ28  EQU     20h
ATA_CMD_WRITE28 EQU     30h
ATA_CMD_READ48  EQU     24h
ATA_CMD_WRITE48 EQU     34h
ATA_CMD_FLUSH   EQU     0E7h
ATA_CMD_FLUSH48 EQU     0EAh
ATA_CMD_SETGEO  EQU     091h            ; INITIALIZE DEVICE PARAMETERS
ATA_CMD_SETFEAT EQU     0EFh            ; SET FEATURES
ATA_CMD_IDENT   EQU     0ECh
ATA_CMD_RDDMA   EQU     0C8h            ; READ DMA
ATA_CMD_WRDMA   EQU     0CAh            ; WRITE DMA
ATA_CMD_RDDMA48 EQU     025h            ; READ DMA EXT
ATA_CMD_WRDMA48 EQU     035h            ; WRITE DMA EXT

ATA_FEAT_XFER   EQU     03h             ; SET FEATURES: set transfer mode

; Words of interest in the 256-word IDENTIFY block
ATA_ID_CYLS     EQU     1 * 2           ; word - logical cylinders
ATA_ID_HEADS    EQU     3 * 2           ; word - logical heads
ATA_ID_SPT      EQU     6 * 2           ; word - logical sectors per track
ATA_ID_DWIO     EQU     48 * 2          ; bit 0 = doubleword I/O (ATA-1/2)
ATA_ID_CAPS     EQU     49 * 2          ; bit 9 = LBA supported
                                        ; bit 8 = DMA supported
ATA_ID_VALID    EQU     53 * 2          ; bit 1 = word 63 means something,
                                        ; bit 2 = word 88 does
ATA_ID_LBA28    EQU     60 * 2          ; dword - LBA28 sector count
ATA_ID_MWDMA    EQU     63 * 2          ; low byte supported, high selected
ATA_ID_CMDSET   EQU     83 * 2          ; bit 10 = LBA48 supported
ATA_ID_UDMA     EQU     88 * 2          ; low byte supported, high selected
ATA_ID_LBA48    EQU     100 * 2         ; qword - LBA48 sector count

; Per-device table entry
ATD_BASE        EQU     0               ; word - command block base port
ATD_CTRL        EQU     2               ; word - control port
ATD_SEL         EQU     4               ; byte - 00h master, 10h slave
ATD_FLAGS       EQU     5               ; byte - see below
ATD_HEADS       EQU     6               ; word - CHS heads
ATD_SPT         EQU     8               ; word - CHS sectors per track
ATD_BM          EQU     10              ; word - bus-master base for this
                                        ; device's channel, 0 if it has none
ATD_TOTAL       EQU     12              ; dword - total sectors
ATD_DMA         EQU     16              ; byte - the transfer mode in force,
                                        ; as SET FEATURES spells it
ATD_LEN         EQU     20

; ATDF_* live in PMDOS.INC, because F1h AL=0Ah publishes them: a
; program asking how its disk is being talked to is told in these
; bits, so they are an interface rather than a private note.
ATDF_NODMA      EQU     0F7h            ; ATDF_DMA taken away again
ATDF_NO32       EQU     0EFh            ; and ATDF_PIO32

;---------------------------------------------------------------------
; Bus-master IDE registers, as offsets from the channel's engine.  The
; primary channel's are at BAR4 and the secondary's at BAR4 + 8, which
; is the one piece of the layout that is fixed rather than discovered.
;---------------------------------------------------------------------
BM_CMD          EQU     0
BM_STATUS       EQU     2
BM_PRDT         EQU     4               ; dword - physical address of the
                                        ; descriptor table

BM_CM_START     EQU     01h             ; run
BM_CM_READ      EQU     08h             ; direction: 1 = write to memory,
                                        ; which is a disk READ
BM_ST_ACTIVE    EQU     01h             ; still going
BM_ST_ERR       EQU     02h             ; write 1 to clear
BM_ST_IRQ       EQU     04h             ; write 1 to clear

; A descriptor table entry: a physical address and a byte count, with
; 0 meaning 65536 and bit 31 of the second dword ending the table.
PRD_ADDR        EQU     0
PRD_COUNT       EQU     4
PRD_LEN         EQU     8
PRD_EOT         EQU     8000h           ; in the high word of PRD_COUNT

ATA_PRDMAX      EQU     8               ; entries in the table
ATA_DMAMAX      EQU     256             ; sectors in one DMA command: 128K,
                                        ; which is also the LBA28 ceiling

; Spin limit for the status waits.  Deliberately generous: it only has
; to be an upper bound, and a wedged controller must not hang the
; kernel.
ATA_SPIN        EQU     01000000h

;=====================================================================
; ata_init - probe both channels and fill in the device table.
;            [ata_count] ends up holding the number of usable disks.
;=====================================================================
ata_init:
        pushad
        mov     dword ptr [ata_count],0

        mov     edi,OFFSET ata_devs             ; blank the table
        xor     eax,eax
        mov     ecx,(ATA_MAXDEV * ATD_LEN) / 4
        rep     stosd

        call    ata_prdinit
        call    ata_bminit                      ; before the probe, because
                                                ; a device decides how it
                                                ; will transfer at the moment
                                                ; IDENTIFY answers

        xor     ebx,ebx                         ; EBX = device index
ai_loop:
        cmp     ebx,ATA_MAXDEV
        jae     ai_done

        mov     esi,ebx
        imul    esi,ATD_LEN
        add     esi,OFFSET ata_devs

        ; channel 0 -> 1F0h/3F6h, channel 1 -> 170h/376h
        mov     eax,ebx
        shr     eax,1                           ; EAX = channel
        mov     dx,1F0h
        mov     cx,3F6h
        or      eax,eax
        jz      ai_chan
        mov     dx,170h
        mov     cx,376h
ai_chan:
        mov     [esi+ATD_BASE],dx
        mov     [esi+ATD_CTRL],cx
        mov     al,bl                           ; odd index = slave
        and     al,1
        shl     al,4
        mov     [esi+ATD_SEL],al

        call    ata_identify
IFDEF ATA_DEBUG
        pushfd                          ; con_out eats the carry
        push    eax
        mov     al,[ata_stage]
        call    con_out
        mov     ax,word ptr [ata_idbuf+ATA_ID_CAPS]
        call    ata_hex16
        mov     ax,word ptr [ata_idbuf+ATA_ID_CYLS]
        call    ata_hex16
        mov     ax,word ptr [ata_idbuf+ATA_ID_HEADS]
        call    ata_hex16
        mov     ax,word ptr [ata_idbuf+ATA_ID_SPT]
        call    ata_hex16
        mov     al,' '
        call    con_out
        pop     eax
        popfd
ENDIF
        jc      ai_next
        or      byte ptr [esi+ATD_FLAGS],ATDF_PRESENT
        inc     dword ptr [ata_count]
ai_next:
        inc     ebx
        jmp     short ai_loop
ai_done:
IFDEF ATA_DEBUG
        ; self-test: read LBA 0 back and show its signature word
        cmp     dword ptr [ata_count],0
        je      ai_notest
        mov     esi,OFFSET msgSelf
        call    con_str
        mov     edi,OFFSET ata_idbuf            ; poison it first, so a
        mov     eax,0DEADBEEFh                  ; silent no-op is obvious
        mov     ecx,128
        rep     stosd
        mov     byte ptr [ata_dbgon],1
        mov     al,80h
        xor     ebx,ebx
        mov     ecx,1
        mov     edi,OFFSET ata_idbuf
        call    ata_read
        mov     byte ptr [ata_dbgon],0
        jc      ai_testfail
        mov     ax,word ptr [ata_idbuf]
        call    ata_hex16
        mov     ax,word ptr [ata_idbuf+2]
        call    ata_hex16
        mov     ax,word ptr [ata_idbuf+1FEh]
        call    ata_hex16
        jmp     short ai_notest
ai_testfail:
        mov     al,'!'
        call    con_out
ai_notest:
        mov     al,13
        call    con_out
        mov     al,10
        call    con_out
ENDIF
        popad
        ret

IFDEF ATA_DEBUG
msgSelf     db  ' LBA0 sig=',0
ENDIF

IFDEF ATA_DEBUG
;---------------------------------------------------------------------
; ata_hex16 - print AX as four hex digits (probe diagnostics only).
;---------------------------------------------------------------------
ata_hex16:
        push    eax
        push    ecx
        mov     ecx,4
ah16_lp:
        rol     ax,4
        push    eax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ah16_d
        add     al,7
ah16_d:
        call    con_out
        pop     eax
        loop    ah16_lp
        pop     ecx
        pop     eax
        ret
ENDIF

;=====================================================================
; ata_identify - IDENTIFY DEVICE on the entry at ESI.  CF clear and
;                ATD_TOTAL / ATD_FLAGS filled in if a plain ATA disk
;                answered.  Preserves EBX/ESI.
;=====================================================================
ata_identify:
        push    eax
        push    ecx
        push    edx
        push    edi

        movzx   edx,word ptr [esi+ATD_BASE]

        ; nothing there at all reads back as a floating bus
        mov     byte ptr [ata_stage],'F'
        add     dx,ATA_STATUS
        in      al,dx
        cmp     al,0FFh
        je      aid_none
        sub     dx,ATA_STATUS

        mov     byte ptr [ata_stage],'S'
        call    ata_select
        jc      aid_none

        ; zero the address registers, then IDENTIFY
        push    edx
        add     dx,ATA_SECCNT
        xor     al,al
        out     dx,al
        inc     dx                              ; LBA0
        out     dx,al
        inc     dx                              ; LBA1
        out     dx,al
        inc     dx                              ; LBA2
        out     dx,al
        pop     edx

        push    edx
        add     dx,ATA_COMMAND
        mov     al,ATA_CMD_IDENT
        out     dx,al
        pop     edx
        call    ata_delay

        ; a status of zero means "no device"
        mov     byte ptr [ata_stage],'Z'
        push    edx
        add     dx,ATA_STATUS
        in      al,dx
        pop     edx
        or      al,al
        jz      aid_none

        mov     byte ptr [ata_stage],'B'
        call    ata_waitbsy
        jc      aid_none

        ; a non-zero LBA1/LBA2 here is a packet device (ATAPI) or a
        ; SATA bridge answering - not something this driver handles
        mov     byte ptr [ata_stage],'P'
        push    edx
        add     dx,ATA_LBA1
        in      al,dx
        mov     ah,al
        inc     dx
        in      al,dx
        pop     edx
        or      al,ah
        jnz     aid_none

        mov     byte ptr [ata_stage],'Q'
        call    ata_waitdrq
        jc      aid_none

        ; pull the 256-word block into the scratch buffer
        push    edx
        add     dx,ATA_DATA
        mov     edi,OFFSET ata_idbuf
        mov     ecx,256
        rep     insw
        pop     edx

;--- LBA if the drive offers it, otherwise plain CHS ------------------
        mov     byte ptr [ata_stage],'L'
        test    word ptr [ata_idbuf+ATA_ID_CAPS],0200h
        jz      aid_chs
        mov     eax,dword ptr [ata_idbuf+ATA_ID_LBA28]
        or      eax,eax
        jz      aid_chs
        or      byte ptr [esi+ATD_FLAGS],ATDF_LBA

        ; LBA48 only matters past the 28-bit ceiling
        test    word ptr [ata_idbuf+ATA_ID_CMDSET],0400h
        jz      aid_store
        mov     ecx,dword ptr [ata_idbuf+ATA_ID_LBA48+4]
        or      ecx,ecx
        jnz     aid_huge                        ; over 2^32 sectors: cap
        mov     ecx,dword ptr [ata_idbuf+ATA_ID_LBA48]
        or      ecx,ecx
        jz      aid_store
        mov     eax,ecx
        or      byte ptr [esi+ATD_FLAGS],ATDF_LBA48
        jmp     short aid_store
aid_huge:
        mov     eax,0FFFFFFFFh
        or      byte ptr [esi+ATD_FLAGS],ATDF_LBA48
        jmp     short aid_store

;--- CHS: pre-LBA drives, and some emulated configurations ------------
aid_chs:
        mov     byte ptr [ata_stage],'C'
        movzx   eax,word ptr [ata_idbuf+ATA_ID_HEADS]
        or      eax,eax
        jz      aid_none
        mov     [esi+ATD_HEADS],ax
        movzx   ecx,word ptr [ata_idbuf+ATA_ID_SPT]
        or      ecx,ecx
        jz      aid_none
        mov     [esi+ATD_SPT],cx
        mul     ecx                             ; heads * spt
        movzx   ecx,word ptr [ata_idbuf+ATA_ID_CYLS]
        or      ecx,ecx
        jz      aid_none
        mul     ecx                             ; * cylinders
        push    eax
        call    ata_setgeo              ; make the drive agree with us
        pop     eax

aid_store:
        mov     byte ptr [ata_stage],'+'
        mov     [esi+ATD_TOTAL],eax

        ; how this device's data will move.  It has to be decided here
        ; and not in a later pass, because every input is in ata_idbuf
        ; and the next IDENTIFY overwrites it
        call    ata_pickbm
        call    ata_pickdma
        call    ata_pick32

        ; polled driver: tell the drive to keep its interrupt to itself
        movzx   edx,word ptr [esi+ATD_CTRL]
        mov     al,ATA_DC_NIEN
        out     dx,al

        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret

aid_none:
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; ata_setgeo - INITIALIZE DEVICE PARAMETERS.  ESI = device, DX = base.
;
; A drive's CHS translation is whatever was last programmed into it,
; normally by the BIOS at POST, and that need not match the geometry
; IDENTIFY reports as its default.  Reading C/H/S computed from the
; wrong translation lands on the wrong sector and reports no error at
; all - it just returns the wrong data.  So state the geometry we
; intend to use rather than assuming.
;=====================================================================
ata_setgeo:
        push    eax
        push    edx

        movzx   eax,word ptr [esi+ATD_HEADS]
        dec     al                      ; the register takes heads - 1
        or      al,[esi+ATD_SEL]
        or      al,0A0h
        push    edx
        add     dx,ATA_DEVSEL
        out     dx,al
        pop     edx
        call    ata_delay

        push    edx
        add     dx,ATA_SECCNT
        mov     ax,[esi+ATD_SPT]
        out     dx,al
        pop     edx

        push    edx
        add     dx,ATA_COMMAND
        mov     al,ATA_CMD_SETGEO
        out     dx,al
        pop     edx
        call    ata_delay
        call    ata_waitbsy

        pop     edx
        pop     eax
        ret

;=====================================================================
; ata_select - select the device described by ESI.  DX = base port.
;              CF set if the drive never becomes ready.
;=====================================================================
ata_select:
        push    eax
        push    ecx
        push    edx

        mov     al,[esi+ATD_SEL]
        or      al,0A0h
        add     dx,ATA_DEVSEL
        out     dx,al
        sub     dx,ATA_DEVSEL
        call    ata_delay               ; the select needs settling time

        ; wait for BSY to drop; DRDY may stay low on an empty slot, so
        ; only BSY is decisive here
        mov     ecx,ATA_SPIN
        add     dx,ATA_STATUS
asel_wait:
        in      al,dx
        cmp     al,0FFh
        je      asel_fail
        test    al,ATA_ST_BSY
        jz      asel_ok
        dec     ecx
        jnz     asel_wait
asel_fail:
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret
asel_ok:
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret

;=====================================================================
; ata_delay - read the alternate status four times, the classic way to
;             burn the 400ns a drive needs after a select or command.
;             DX = base port, preserved.
;=====================================================================
ata_delay:
        push    eax
        push    edx
        movzx   edx,word ptr [esi+ATD_CTRL]
        in      al,dx
        in      al,dx
        in      al,dx
        in      al,dx
        pop     edx
        pop     eax
        ret

;=====================================================================
; ata_waitbsy - spin until BSY clears.  DX = base port.
;               CF set on timeout or a floating bus.
;=====================================================================
ata_waitbsy:
        push    eax
        push    ecx
        push    edx
        add     dx,ATA_STATUS
        mov     ecx,ATA_SPIN
awb_lp:
        in      al,dx
        cmp     al,0FFh
        je      awb_fail
        test    al,ATA_ST_BSY
        jz      awb_ok
        dec     ecx
        jnz     awb_lp
awb_fail:
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret
awb_ok:
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret

;=====================================================================
; ata_waitdrq - spin until the drive asks for data (DRQ) with BSY
;               clear.  DX = base port.  CF set on timeout, ERR or DF.
;=====================================================================
ata_waitdrq:
        push    eax
        push    ecx
        push    edx
        add     dx,ATA_STATUS
        mov     ecx,ATA_SPIN
awd_lp:
        in      al,dx
        cmp     al,0FFh
        je      awd_fail
        test    al,ATA_ST_BSY
        jnz     awd_next
        test    al,ATA_ST_ERR OR ATA_ST_DF
        jnz     awd_fail
        test    al,ATA_ST_DRQ
        jnz     awd_ok
awd_next:
        dec     ecx
        jnz     awd_lp
awd_fail:
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret
awd_ok:
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret

;=====================================================================
; ata_slot - AL = BIOS drive number -> ESI = its device table entry.
;            CF set unless that drive is a hard disk this driver owns.
;
; Hard disks are handed out in probe order, so BIOS 80h is the first
; ATA disk found (normally the primary master), 81h the next.
;=====================================================================
ata_slot:
        push    eax
        push    ebx
        push    ecx

        cmp     al,80h
        jb      as_bad
        movzx   ebx,al
        sub     ebx,80h                 ; EBX = which hard disk is wanted

        xor     ecx,ecx                 ; ECX = devices scanned
        mov     esi,OFFSET ata_devs
as_scan:
        cmp     ecx,ATA_MAXDEV
        jae     as_bad
        test    byte ptr [esi+ATD_FLAGS],ATDF_PRESENT
        jz      as_next
        or      ebx,ebx
        jz      as_found
        dec     ebx
as_next:
        add     esi,ATD_LEN
        inc     ecx
        jmp     short as_scan

as_found:
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
as_bad:
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; ata_prdinit - decide where the descriptor table lives.
;
; TWO ALIGNMENT RULES AND ONE OF THEM IS ODD.  The table has to start
; on a dword boundary, which is ordinary, and it must not CROSS a 64K
; boundary, which is not: the engine carries the table's address in a
; register whose low sixteen bits it increments by itself, so a table
; that straddles the line wraps back to its own beginning.  There is
; no way to ask an assembler for that, so twice the room is reserved
; and the half that does not straddle is used.
;
; The address handed to the controller is a PHYSICAL one and this is
; a linear pointer - which is the same number here, because PAGING.INC
; maps every page to itself.  See ata_dmaok, which is where that stops
; being true.
;=====================================================================
ata_prdinit:
        push    eax
        push    edx
        mov     eax,OFFSET ata_prdraw
        add     eax,7
        and     eax,0FFFFFFF8h
        mov     edx,eax
        and     edx,0FFFFh
        add     edx,ATA_PRDMAX * PRD_LEN
        cmp     edx,10000h
        jbe     api_fits
        add     eax,10000h              ; start again on the far side
        and     eax,0FFFF0000h
api_fits:
        mov     [ata_prdaddr],eax
        pop     edx
        pop     eax
        ret

;=====================================================================
; ata_bminit - find the bus-master engine, if there is one.
;
; Class 01h subclass 01h is "mass storage, IDE" and BAR4 is where the
; specification puts the engine's registers - eight bytes for the
; primary channel and eight more for the secondary, in one I/O block.
;
; THE PROG-IF BYTE IS THE PART THAT MATTERS.  Bit 0 says the primary
; channel is in native mode and bit 2 says the secondary is, and a
; native-mode channel does not answer at 1F0h or 170h at all.  This
; driver probes the two legacy addresses and nothing else, so a
; channel in native mode is a channel whose drives were never found -
; and attaching its engine to a device found at a legacy address
; would be pointing the bus master at the wrong disk.
;=====================================================================
ata_bminit:
        pushad
        mov     byte ptr [ata_bmok],0
        mov     word ptr [ata_bmbase],0
        mov     dword ptr [ata_pciid],0

        mov     cx,0101h                        ; mass storage, IDE
        call    pci_find
        jc      abi_none
        mov     [ata_pcidev],ebx

        mov     al,PCIR_VENDOR
        call    pci_read
        mov     [ata_pciid],eax

        mov     al,PCIR_BAR4
        call    pci_read
        test    al,1                            ; it has to be an I/O BAR
        jz      abi_none
        and     eax,0FFFCh
        jz      abi_none                        ; ...and a real address
        mov     [ata_bmbase],ax

        mov     al,PCIR_CLASS
        call    pci_read
        shr     eax,8
        mov     [ata_progif],al

        ; a device that has not been told it may master the bus will
        ; sit there with its Start bit set and transfer nothing
        mov     al,PCIR_COMMAND
        call    pci_read
        mov     ecx,eax
        or      ecx,PCIC_BUSMASTER OR PCIC_IOEN
        mov     al,PCIR_COMMAND
        call    pci_write

        mov     byte ptr [ata_bmok],1
abi_none:
        popad
        ret

;=====================================================================
; ata_pickbm - ESI = device.  Fill in ATD_BM, or leave it zero.
;=====================================================================
ata_pickbm:
        push    eax
        push    edx
        mov     word ptr [esi+ATD_BM],0
        cmp     byte ptr [ata_bmok],0
        je      pbm_out
        movzx   eax,word ptr [ata_bmbase]
        or      eax,eax
        jz      pbm_out
        mov     dl,[ata_progif]
        cmp     word ptr [esi+ATD_BASE],1F0h
        jne     pbm_second
        test    dl,1                    ; primary in native mode
        jnz     pbm_out
        jmp     short pbm_have
pbm_second:
        test    dl,4                    ; secondary in native mode
        jnz     pbm_out
        add     eax,8
pbm_have:
        mov     [esi+ATD_BM],ax
pbm_out:
        pop     edx
        pop     eax
        ret

;=====================================================================
; ata_pickdma - ESI = device, ata_idbuf = its IDENTIFY block.  Decide
;               whether this device transfers by bus master, and say
;               so in ATD_FLAGS and ATD_DMA.
;
; AUTO DOES NOT SELECT A MODE, AND THAT IS THE WHOLE ARGUMENT.  A
; transfer mode is TWO settings that have to agree: the drive's, which
; SET FEATURES chooses and which is standardised, and the host
; adapter's timing, which is a chipset register and is not.  Every
; controller spells its timing differently - PIIX one way, VIA
; another, ALi a third - and PM-DOS knows none of them.  Telling a
; drive to run UDMA against timing the BIOS programmed for something
; slower is how a disk returns data that is almost right.
;
; So AUTO uses the mode that is ALREADY SELECTED - which the BIOS
; selected at POST, having programmed its own end to match - and if
; nothing is selected, it does nothing at all and the transfers stay
; PIO.  That is not timidity: it is the only version of "automatic"
; that cannot be wrong.
;
; IDEDMA=ON and IDEDMA=<mode> do issue SET FEATURES, because a user
; who writes one of those has said something deliberate about their
; own machine.  ON stops at UDMA mode 2 - 33 MB/s - which is the last
; mode a 40-conductor cable carries, and so the last one that can be
; chosen without asking a cable-detect question that hardware of this
; era answers unreliably.
;=====================================================================
ata_pickdma:
        push    eax
        push    ebx
        mov     byte ptr [esi+ATD_DMA],0
        and     byte ptr [esi+ATD_FLAGS],ATDF_NODMA

        cmp     byte ptr [ata_dmapol],0
        je      apd_out                 ; IDEDMA=OFF
        cmp     word ptr [esi+ATD_BM],0
        je      apd_out                 ; no engine on this channel
        test    word ptr [ata_idbuf+ATA_ID_CAPS],0100h
        jz      apd_out                 ; the drive does not do DMA

        cmp     byte ptr [ata_dmapol],1
        je      apd_auto
        cmp     byte ptr [ata_dmapol],3
        je      apd_forced

        call    ata_bestmode            ; IDEDMA=ON
        jc      apd_out
        jmp     short apd_select
apd_forced:
        mov     al,[ata_dmaforce]       ; IDEDMA=<mode>
        call    ata_hasmode
        jc      apd_out
apd_select:
        mov     bl,al
        call    ata_setmode
        jc      apd_out
        mov     al,bl
        jmp     short apd_take
apd_auto:
        call    ata_curmode
        or      al,al
        jz      apd_out
apd_take:
        mov     [esi+ATD_DMA],al
        or      byte ptr [esi+ATD_FLAGS],ATDF_DMA
apd_out:
        pop     ebx
        pop     eax
        ret

;=====================================================================
; ata_curmode - AL = the DMA mode already selected, or 0 for none.
;               UDMA first: a drive with both selected is answering
;               about two different things and the faster one wins.
;=====================================================================
ata_curmode:
        test    word ptr [ata_idbuf+ATA_ID_VALID],4
        jz      acm_mw
        movzx   eax,byte ptr [ata_idbuf+ATA_ID_UDMA+1]
        or      al,al
        jz      acm_mw
        call    ata_topbit
        or      al,40h
        ret
acm_mw:
        test    word ptr [ata_idbuf+ATA_ID_VALID],2
        jz      acm_none
        movzx   eax,byte ptr [ata_idbuf+ATA_ID_MWDMA+1]
        or      al,al
        jz      acm_none
        call    ata_topbit
        or      al,20h
        ret
acm_none:
        xor     eax,eax
        ret

;=====================================================================
; ata_bestmode - AL = the fastest mode the drive OFFERS, capped at
;                UDMA 2.  CF set if it offers none.
;=====================================================================
ata_bestmode:
        test    word ptr [ata_idbuf+ATA_ID_VALID],4
        jz      abm_mw
        movzx   eax,byte ptr [ata_idbuf+ATA_ID_UDMA]
        and     al,07h                  ; modes 0, 1 and 2 only
        jz      abm_mw
        call    ata_topbit
        or      al,40h
        clc
        ret
abm_mw:
        test    word ptr [ata_idbuf+ATA_ID_VALID],2
        jz      abm_none
        movzx   eax,byte ptr [ata_idbuf+ATA_ID_MWDMA]
        and     al,07h
        jz      abm_none
        call    ata_topbit
        or      al,20h
        clc
        ret
abm_none:
        stc
        ret

;=====================================================================
; ata_hasmode - AL = a mode byte.  CF clear if the drive says it can
;               do that one.  AL is preserved.
;=====================================================================
ata_hasmode:
        push    ecx
        push    edx
        mov     ah,al
        and     ah,0F8h                 ; 40h = UDMA, 20h = multiword
        mov     cl,al
        and     cl,07h
        mov     dl,1
        shl     dl,cl                   ; DL = the bit that would say so
        cmp     ah,40h
        jne     ahm_mw
        test    word ptr [ata_idbuf+ATA_ID_VALID],4
        jz      ahm_no
        test    byte ptr [ata_idbuf+ATA_ID_UDMA],dl
        jz      ahm_no
        jmp     short ahm_yes
ahm_mw:
        cmp     ah,20h
        jne     ahm_no
        test    word ptr [ata_idbuf+ATA_ID_VALID],2
        jz      ahm_no
        test    byte ptr [ata_idbuf+ATA_ID_MWDMA],dl
        jz      ahm_no
ahm_yes:
        pop     edx
        pop     ecx
        clc
        ret
ahm_no:
        pop     edx
        pop     ecx
        stc
        ret

;=====================================================================
; ata_topbit - AL = a byte with at least one bit set.
;              -> EAX = the index of the highest one.
;=====================================================================
ata_topbit:
        push    ecx
        movzx   ecx,al
        mov     eax,7
atb_lp:
        bt      ecx,eax
        jc      atb_done
        dec     eax
        jns     atb_lp
        xor     eax,eax
atb_done:
        pop     ecx
        ret

;=====================================================================
; ata_setmode - ESI = device, AL = mode byte.  SET FEATURES it.
;               CF set if the drive refused.
;=====================================================================
ata_setmode:
        push    eax
        push    ebx
        push    edx
        mov     bl,al
        movzx   edx,word ptr [esi+ATD_BASE]
        call    ata_select
        jc      asm_bad

        push    edx
        add     dx,ATA_FEATURES
        mov     al,ATA_FEAT_XFER
        out     dx,al
        inc     dx                      ; the mode goes in the sector count
        mov     al,bl
        out     dx,al
        pop     edx

        push    edx
        add     dx,ATA_COMMAND
        mov     al,ATA_CMD_SETFEAT
        out     dx,al
        pop     edx
        call    ata_delay
        call    ata_waitbsy
        jc      asm_bad

        push    edx
        add     dx,ATA_STATUS
        in      al,dx
        pop     edx
        test    al,ATA_ST_ERR OR ATA_ST_DF
        jnz     asm_bad

        pop     edx
        pop     ebx
        pop     eax
        clc
        ret
asm_bad:
        pop     edx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; ata_pick32 - ESI = device.  Is the data port read a dword at a time?
;
; Two independent reasons to say yes and each covers an era the other
; does not; the file header argues it at length.  Neither is a
; measurement, because there is no way to measure this: a host adapter
; that cannot latch two words does not report an error, it returns
; half the data.
;=====================================================================
ata_pick32:
        and     byte ptr [esi+ATD_FLAGS],ATDF_NO32
        cmp     byte ptr [ata_32pol],0
        je      p32_out                 ; IDE32=OFF
        cmp     byte ptr [ata_32pol],2
        je      p32_yes                 ; IDE32=ON
        test    word ptr [ata_idbuf+ATA_ID_DWIO],1
        jnz     p32_yes                 ; the drive says so (ATA-1/2)
        cmp     byte ptr [ata_bmok],0
        je      p32_out
p32_yes:                                ; or it is on a PCI controller
        or      byte ptr [esi+ATD_FLAGS],ATDF_PIO32
p32_out:
        ret

;=====================================================================
; ata_dmaok - CF clear if [ata_buf] for [ata_chunk] sectors is memory
;             a bus master may be pointed at.
;
; THE IDENTITY MAP IS WHY THIS IS FOUR COMPARISONS.  PAGING.INC maps
; every page to itself, so a linear address IS a physical one and the
; kernel's own pointers can go on the bus - with two exceptions, both
; below 1MB and both deliberate.  The sixteen pages from 100000h are
; what the A20 gate aliases down to zero, so an address in there means
; one of two different physical places depending on a switch.  And
; whatever UMB.INC and EMS.INC have pointed at upper memory means, by
; definition, somewhere other than itself.
;
; Everything below A0000h is itself and always has been; everything
; from DOS_BASE up is itself and always will be.  The band between
; them is the one place in the machine where our own pointer is not
; the device's, and a transfer aimed there goes by PIO - which needs
; no physical address at all, because the processor is the one doing
; the moving.
;
; The word alignment is the descriptor format's rule rather than
; ours: a PRD address with bit 0 set is not representable.
;=====================================================================
ata_dmaok:
        push    eax
        push    edx
        mov     eax,[ata_buf]
        test    al,1
        jnz     adk_no
        mov     edx,[ata_chunk]
        shl     edx,9
        add     edx,eax                 ; EDX = one past the last byte
        jc      adk_no
        cmp     edx,0A0000h
        jbe     adk_yes
        cmp     eax,DOS_BASE
        jb      adk_no
adk_yes:
        pop     edx
        pop     eax
        clc
        ret
adk_no:
        pop     edx
        pop     eax
        stc
        ret

;=====================================================================
; ata_prdbuild - describe [ata_buf] for [ata_chunk] sectors as a list
;                of physical regions.  CF set if it will not fit.
;
; A region may not cross a 64K boundary - the same wrap that decides
; where the table itself lives - so the buffer is cut at every one it
; spans.  A length of zero in an entry means 65536, which is why the
; count is stored as a word and the full 64K case needs no special
; handling: it stores as zero by arithmetic.
;=====================================================================
ata_prdbuild:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    edi
        mov     edi,[ata_prdaddr]
        mov     ebx,[ata_buf]           ; EBX = where we have got to
        mov     ecx,[ata_chunk]
        shl     ecx,9                   ; ECX = bytes still to describe
        xor     edx,edx                 ; EDX = entries used
apr_lp:
        cmp     edx,ATA_PRDMAX
        jae     apr_full
        mov     eax,ebx
        and     eax,0FFFFh
        neg     eax
        add     eax,10000h              ; EAX = bytes to the next boundary
        cmp     eax,ecx
        jbe     apr_take
        mov     eax,ecx
apr_take:
        mov     [edi+PRD_ADDR],ebx
        mov     [edi+PRD_COUNT],ax
        mov     word ptr [edi+PRD_COUNT+2],0
        add     ebx,eax
        sub     ecx,eax
        add     edi,PRD_LEN
        inc     edx
        or      ecx,ecx
        jnz     apr_lp
        or      word ptr [edi-PRD_LEN+PRD_COUNT+2],PRD_EOT
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
apr_full:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; ata_dmaprep - ESI = device.  Point the engine at the table and set
;               the direction, before the command goes to the drive.
;
; The order the specification gives is: table, direction, clear the
; latched bits, issue the ATA command, and only then Start.  It reads
; like ceremony and is not - the engine begins moving data the moment
; the drive asserts DMARQ, so Start must not be set until there is a
; command for the drive to be executing.
;=====================================================================
ata_dmaprep:
        push    eax
        push    edx
        movzx   edx,word ptr [esi+ATD_BM]

        xor     al,al                   ; stop whatever it thinks it is
        out     dx,al                   ; doing

        add     dx,BM_STATUS
        in      al,dx
        or      al,BM_ST_IRQ OR BM_ST_ERR
        out     dx,al                   ; both are write-one-to-clear

        movzx   edx,word ptr [esi+ATD_BM]
        add     dx,BM_PRDT
        mov     eax,[ata_prdaddr]
        out     dx,eax

        movzx   edx,word ptr [esi+ATD_BM]
        mov     al,BM_CM_READ           ; the engine WRITES to memory
        cmp     byte ptr [ata_dir],0
        je      adp_dir
        xor     al,al
adp_dir:
        out     dx,al
        pop     edx
        pop     eax
        ret

;=====================================================================
; ata_dmarun - ESI = device.  Start the engine and wait for it.
;              CF set on failure, and [ata_bmbad] says whose fault it
;              was.
;
; WITH nIEN SET THE INTERRUPT BIT NEVER ARRIVES, so the thing waited
; on is the ACTIVE bit going down, which happens when the last
; descriptor is exhausted.  That is the same event and needs no
; interrupt controller behind it - which is the point, because this
; driver has no IRQ 14 handler and the slave PIC is masked.
;
; A drive that fails part way through never exhausts the table, so
; the wait ends in a timeout rather than a clean stop; that is why
; the drive's own status is read afterwards either way.  An error the
; DRIVE reports is an ordinary bad sector and is passed up.  An error
; the ENGINE reports, or a stop that never came with the drive
; perfectly happy, is a bus-master fault - and a bus master that has
; got a transfer wrong once is not trusted with the retry.
;=====================================================================
ata_dmarun:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     byte ptr [ata_bmbad],0
        movzx   ebx,word ptr [esi+ATD_BM]

        mov     edx,ebx
        in      al,dx
        or      al,BM_CM_START
        out     dx,al

        mov     ecx,ATA_SPIN
        lea     edx,[ebx+BM_STATUS]
adr_wait:
        in      al,dx
        test    al,BM_ST_ERR
        jnz     adr_stop
        test    al,BM_ST_ACTIVE
        jz      adr_stop
        dec     ecx
        jnz     adr_wait
adr_stop:
        mov     edx,ebx                 ; stop the engine whatever happened
        xor     al,al
        out     dx,al
        lea     edx,[ebx+BM_STATUS]
        in      al,dx
        mov     [ata_bmst],al           ; the engine's account of it, kept
                                        ; somewhere that is not half of the
                                        ; port number we are still using
        or      al,BM_ST_IRQ OR BM_ST_ERR
        out     dx,al

        movzx   edx,word ptr [esi+ATD_BASE]
        call    ata_waitbsy
        jc      adr_bmfault             ; a drive that never came back is
                                        ; not evidence against itself, but
                                        ; there is nothing else to blame
        push    edx
        add     dx,ATA_STATUS
        in      al,dx
        pop     edx
        test    al,ATA_ST_ERR OR ATA_ST_DF
        jnz     adr_drvfault

        test    byte ptr [ata_bmst],BM_ST_ERR
        jnz     adr_bmfault
        test    byte ptr [ata_bmst],BM_ST_ACTIVE    ; still going, drive idle
        jnz     adr_bmfault

        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
adr_bmfault:
        mov     byte ptr [ata_bmbad],1
adr_drvfault:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; ata_config - IOS_ATACFG.  AL = 0 sets the DMA policy from AH (with
;              the mode byte in BL when AH = 3), AL = 1 sets the
;              32-bit PIO policy from AH.
;
; IT RE-RUNS THE WHOLE PROBE, and that is deliberate rather than
; lazy.  A policy does not change any fact about a device; it changes
; what the facts MEAN, and every one of those facts is in an IDENTIFY
; block that was overwritten three devices ago.  Asking again is the
; only honest way to apply it, the probe is idempotent, and the one
; caller is a CONFIG.SYS line read before any program exists.
;=====================================================================
ata_config:
        push    eax
        or      al,al
        jnz     acf_p32
        mov     [ata_dmapol],ah
        mov     [ata_dmaforce],bl
        jmp     short acf_redo
acf_p32:
        cmp     al,1
        jne     acf_out
        mov     [ata_32pol],ah
acf_redo:
        call    ata_init
acf_out:
        pop     eax
        ret

;=====================================================================
; ata_info - IOS_ATAINFO.  AL = device index, or FFh for the
;            controller.
;
;   a device:     EAX = ATDF_* flags, EBX = its engine's base port,
;                 ECX = the transfer mode in force, EDX = sectors
;   the whole:    EAX = disks found, EBX = PCI vendor:device,
;                 ECX = the engine's base, EDX = the prog-if byte,
;                 bit 16 PCI answered, bit 17 an engine was found,
;                 bits 24-27 the DMA policy, 28-31 the 32-bit one
;
; It exists because the report has to come from inside the kernel:
; MEM and a test program run at ring 3 out of a slot with no I/O
; privilege, so a native program cannot look at a controller even to
; describe it.  Same argument as F1h AL=9 makes for expanded memory.
;=====================================================================
ata_info:
        cmp     al,0FFh
        je      ain_whole
        movzx   ebx,al
        cmp     ebx,ATA_MAXDEV
        jae     ain_bad
        imul    ebx,ATD_LEN
        add     ebx,OFFSET ata_devs
        movzx   eax,byte ptr [ebx+ATD_FLAGS]
        movzx   ecx,byte ptr [ebx+ATD_DMA]
        mov     edx,[ebx+ATD_TOTAL]
        movzx   ebx,word ptr [ebx+ATD_BM]
        ret
ain_bad:
        xor     eax,eax
        xor     ebx,ebx
        xor     ecx,ecx
        xor     edx,edx
        ret
ain_whole:
        mov     eax,[ata_count]
        mov     ebx,[ata_pciid]
        movzx   ecx,word ptr [ata_bmbase]
        movzx   edx,byte ptr [ata_progif]
        cmp     byte ptr [pci_ok],0
        je      ain_nopci
        or      edx,00010000h
ain_nopci:
        cmp     byte ptr [ata_bmok],0
        je      ain_nobm
        or      edx,00020000h
ain_nobm:
        movzx   eax,byte ptr [ata_dmapol]
        shl     eax,24
        or      edx,eax
        movzx   eax,byte ptr [ata_32pol]
        shl     eax,28
        or      edx,eax
        mov     eax,[ata_count]
        ret

;=====================================================================
; ata_cmd28 / ata_cmd48 - AL = the command byte for this transfer.
;
; Four commands where there were two, and the choice is made in one
; place because it is made from the same two flags each time: which
; way the data is going, and who is going to move it.
;=====================================================================
ata_cmd28:
        cmp     byte ptr [ata_usedma],0
        jne     ac28_dma
        mov     al,ATA_CMD_READ28
        cmp     byte ptr [ata_dir],0
        je      ac28_out
        mov     al,ATA_CMD_WRITE28
        ret
ac28_dma:
        mov     al,ATA_CMD_RDDMA
        cmp     byte ptr [ata_dir],0
        je      ac28_out
        mov     al,ATA_CMD_WRDMA
ac28_out:
        ret

ata_cmd48:
        cmp     byte ptr [ata_usedma],0
        jne     ac48_dma
        mov     al,ATA_CMD_READ48
        cmp     byte ptr [ata_dir],0
        je      ac48_out
        mov     al,ATA_CMD_WRITE48
        ret
ac48_dma:
        mov     al,ATA_CMD_RDDMA48
        cmp     byte ptr [ata_dir],0
        je      ac48_out
        mov     al,ATA_CMD_WRDMA48
ac48_out:
        ret

;=====================================================================
; ata_rw - the transfer itself.
;
;   ESI = device entry   [ata_lba] = starting LBA
;   [ata_count] sectors  [ata_buf] = flat buffer
;   [ata_dir] selects the direction
;
; CF set on error.  One command per run of up to 256 sectors (LBA28)
; or 65536 (LBA48); the caller's request is split to fit.
;=====================================================================
ata_rw:
        pushad

arw_more:
        mov     ecx,[ata_nsec]
        or      ecx,ecx
        jz      arw_done

        ; how many sectors can one command carry?
        mov     eax,256
        test    byte ptr [esi+ATD_FLAGS],ATDF_LBA48
        jz      arw_cap
        mov     eax,65536
arw_cap:
        cmp     ecx,eax
        jbe     arw_have
        mov     ecx,eax
arw_have:
        mov     [ata_chunk],ecx

        ; can this chunk go by bus master?  The question is asked per
        ; chunk rather than per device because the answer depends on
        ; WHERE the buffer is, and that changes call by call
        mov     byte ptr [ata_usedma],0
        test    byte ptr [esi+ATD_FLAGS],ATDF_DMA
        jz      arw_pio32
        cmp     ecx,ATA_DMAMAX
        jbe     arw_dmacap
        mov     ecx,ATA_DMAMAX
        mov     [ata_chunk],ecx
arw_dmacap:
        call    ata_dmaok
        jc      arw_pio32
        call    ata_prdbuild
        jc      arw_pio32
        mov     byte ptr [ata_usedma],1
arw_pio32:
        mov     byte ptr [ata_p32],0
        test    byte ptr [esi+ATD_FLAGS],ATDF_PIO32
        jz      arw_wide
        mov     byte ptr [ata_p32],1
arw_wide:

        movzx   edx,word ptr [esi+ATD_BASE]
        call    ata_select
        jc      arw_fail

        test    byte ptr [esi+ATD_FLAGS],ATDF_LBA
        jz      arw_chs

        ; LBA48 whenever the drive supports it and the address needs it
        mov     eax,[ata_lba]
        test    byte ptr [esi+ATD_FLAGS],ATDF_LBA48
        jz      arw_lba28
        cmp     eax,0FFFFFFFh
        ja      arw_lba48
        cmp     dword ptr [ata_chunk],256
        ja      arw_lba48
        jmp     short arw_lba28

;--- CHS: LBA / spt -> track and sector, track / heads -> cyl and head
arw_chs:
        push    edx
        mov     eax,[ata_lba]
        xor     edx,edx
        movzx   ecx,word ptr [esi+ATD_SPT]
        div     ecx                     ; EAX = track, EDX = sector - 1
        inc     dl
        mov     bl,dl                   ; BL = sector number
        xor     edx,edx
        movzx   ecx,word ptr [esi+ATD_HEADS]
        div     ecx                     ; EAX = cylinder, EDX = head
        mov     bh,dl                   ; BH = head
        mov     ecx,eax                 ; CX = cylinder, out of AL's way
        pop     edx

        push    edx
        mov     al,[esi+ATD_SEL]
        or      al,0A0h                 ; CHS mode, device select
        or      al,bh                   ; head in the low nibble
        add     dx,ATA_DEVSEL
        out     dx,al
        pop     edx

        push    edx
        add     dx,ATA_SECCNT
        mov     eax,[ata_chunk]
        out     dx,al                   ; 256 is encoded as 0
        inc     dx
        mov     al,bl
        out     dx,al                   ; sector number
        inc     dx
        mov     al,cl
        out     dx,al                   ; cylinder 7:0
        inc     dx
        mov     al,ch
        out     dx,al                   ; cylinder 15:8
        pop     edx

        call    ata_cmd28
        jmp     arw_issue

;--- LBA28 -----------------------------------------------------------
arw_lba28:
        push    edx
        mov     eax,[ata_lba]
        shr     eax,24
        and     al,0Fh
        or      al,[esi+ATD_SEL]
        or      al,0E0h                 ; LBA mode, device select
        add     dx,ATA_DEVSEL
        out     dx,al
        pop     edx

        push    edx
        add     dx,ATA_SECCNT
        mov     eax,[ata_chunk]         ; 256 is encoded as 0
        out     dx,al
        inc     dx
        mov     eax,[ata_lba]
        out     dx,al                   ; LBA 7:0
        inc     dx
        shr     eax,8
        out     dx,al                   ; LBA 15:8
        inc     dx
        shr     eax,8
        out     dx,al                   ; LBA 23:16
        pop     edx

        call    ata_cmd28
        jmp     arw_issue

;--- LBA48 -----------------------------------------------------------
arw_lba48:
        push    edx
        mov     al,[esi+ATD_SEL]
        or      al,40h                  ; LBA mode, device select
        add     dx,ATA_DEVSEL
        out     dx,al
        pop     edx

        ; the high bytes go first, then the low ones land on top of them
        push    edx
        add     dx,ATA_SECCNT
        mov     eax,[ata_chunk]
        shr     eax,8
        out     dx,al                   ; count 15:8
        inc     dx
        mov     eax,[ata_lba]
        shr     eax,24
        out     dx,al                   ; LBA 31:24
        inc     dx
        xor     al,al                   ; LBAs are held in 32 bits here,
        out     dx,al                   ; so 47:32 are always zero
        inc     dx
        out     dx,al
        pop     edx

        push    edx
        add     dx,ATA_SECCNT
        mov     eax,[ata_chunk]
        out     dx,al                   ; count 7:0
        inc     dx
        mov     eax,[ata_lba]
        out     dx,al                   ; LBA 7:0
        inc     dx
        shr     eax,8
        out     dx,al                   ; LBA 15:8
        inc     dx
        shr     eax,8
        out     dx,al                   ; LBA 23:16
        pop     edx

        call    ata_cmd48

;--- issue it, and then move the data --------------------------------
;
; The engine is armed BEFORE the command and started AFTER it: see
; ata_dmaprep, which is where the order is argued.
arw_issue:
        cmp     byte ptr [ata_usedma],0
        je      arw_nodmaprep
        call    ata_dmaprep             ; AL is the command byte and this
arw_nodmaprep:                          ; routine keeps it
        push    edx
        add     dx,ATA_COMMAND
        out     dx,al
        pop     edx
        call    ata_delay
IFDEF ATA_DEBUG
        cmp     byte ptr [ata_dbgon],0
        je      arw_nodbg
        push    eax
        push    edx
        add     dx,ATA_STATUS           ; what did the drive make of it?
        in      al,dx
        mov     ah,al
        sub     dx,ATA_STATUS - ATA_ERROR
        in      al,dx
        call    ata_hex16
        pop     edx
        pop     eax
arw_nodbg:
ENDIF

        cmp     byte ptr [ata_usedma],0
        je      arw_polled

;--- the controller moves it, and we wait -----------------------------
        call    ata_dmarun
        jc      arw_dmabad
        mov     eax,[ata_chunk]         ; nothing advanced the pointer a
        shl     eax,9                   ; sector at a time this way
        add     [ata_buf],eax
        jmp     arw_written             ; over the whole polled path

arw_dmabad:
        cmp     byte ptr [ata_bmbad],0
        je      arw_fail                ; the DRIVE failed: a real error
        ; the ENGINE failed.  Take this device off DMA for the rest of
        ; the session and do the same chunk again the slow way: nothing
        ; has been advanced, so arw_more starts it over
        and     byte ptr [esi+ATD_FLAGS],ATDF_NODMA
        mov     byte ptr [esi+ATD_DMA],0
        jmp     arw_more

;--- or the processor does, a sector at a time ------------------------
arw_polled:
        mov     ecx,[ata_chunk]
arw_sector:
        push    ecx
        call    ata_waitdrq
        jc      arw_fail_pop

        push    edx
        add     dx,ATA_DATA
        mov     edi,[ata_buf]
        mov     ecx,256                 ; 512 bytes as words...
        cmp     byte ptr [ata_p32],0
        je      arw_narrow
        mov     ecx,128                 ; ...or as doublewords
arw_narrow:
        cmp     byte ptr [ata_dir],0
        jne     arw_out
        cmp     byte ptr [ata_p32],0
        je      arw_in16
        rep     insd
        jmp     short arw_moved
arw_in16:
        rep     insw
        jmp     short arw_moved
arw_out:
        mov     esi,edi                 ; OUTSW sources from DS:ESI
        cmp     byte ptr [ata_p32],0
        je      arw_out16
        rep     outsd
        jmp     short arw_moved
arw_out16:
        rep     outsw
arw_moved:
        pop     edx
        mov     esi,[ata_dev]           ; OUTSW ate ESI
        add     dword ptr [ata_buf],512
        call    ata_delay               ; settle before polling again
        pop     ecx
        dec     ecx
        jz      arw_written             ; the loop body outgrew a short jump
        jmp     arw_sector              ; when the wide path went into it

arw_written:

        ; writes have to be flushed or the drive may lie about being done
        cmp     byte ptr [ata_dir],0
        je      arw_advance
        call    ata_waitbsy
        jc      arw_fail
        push    edx
        add     dx,ATA_COMMAND
        mov     al,ATA_CMD_FLUSH
        test    byte ptr [esi+ATD_FLAGS],ATDF_LBA48
        jz      arw_fcmd
        mov     al,ATA_CMD_FLUSH48
arw_fcmd:
        out     dx,al
        pop     edx
        call    ata_delay
        call    ata_waitbsy
        jc      arw_fail

arw_advance:
        mov     eax,[ata_chunk]
        add     [ata_lba],eax
        sub     [ata_nsec],eax
        jmp     arw_more

arw_done:
        popad
        clc
        ret

arw_fail_pop:
        pop     ecx
arw_fail:
        popad
        stc
        ret

;=====================================================================
; ata_read / ata_write - the entry points BLOCK.INC dispatches to.
;
;   AL = BIOS drive   EBX = LBA   ECX = sector count   EDI = buffer
;
; CF set if this drive is not ATA, or the transfer failed.
;=====================================================================
ata_read:
        mov     byte ptr [ata_dir],0
        jmp     short ata_go
ata_write:
        mov     byte ptr [ata_dir],1
ata_go:
        push    esi
        call    ata_slot                        ; AL -> ESI, or CF
        jc      ago_bad
        mov     [ata_dev],esi
        mov     [ata_lba],ebx
        mov     [ata_nsec],ecx
        mov     [ata_buf],edi
        call    ata_rw
        jc      ago_bad
        pop     esi
        clc
        ret
ago_bad:
        pop     esi
        stc
        ret

;=====================================================================
; ata_owns - CF clear if BIOS drive AL is a disk this driver handles.
;            Preserves everything; BLOCK.INC uses it to pick a path
;            before committing, so that a genuine ATA failure is not
;            mistaken for "ask the BIOS instead".
;=====================================================================
ata_owns:
        push    esi
        call    ata_slot
        pop     esi
        ret

;=====================================================================
; ata_capacity - AL = BIOS drive -> EBX = total sectors.  CF set if
;                the drive is not one of ours.
;=====================================================================
ata_capacity:
        push    esi
        call    ata_slot
        jc      acap_bad
        mov     ebx,[esi+ATD_TOTAL]
        pop     esi
        clc
        ret
acap_bad:
        pop     esi
        stc
        ret

;=====================================================================
; Driver state
;=====================================================================
ata_count   dd  0                       ; usable ATA disks found
ata_dir     db  0                       ; 0 = read, 1 = write
ata_stage   db  0                       ; how far probing got (debug)
ata_dbgon   db  0                       ; trace this transfer (debug)
            db  0
ata_dev     dd  0                       ; device entry in play
ata_lba     dd  0
ata_nsec    dd  0
ata_chunk   dd  0
ata_buf     dd  0

;--- how the data moves ----------------------------------------------
ata_dmapol   db 1                       ; 0 off, 1 auto, 2 best, 3 forced
ata_dmaforce db 0                       ; the mode IDEDMA=<mode> named
ata_32pol    db 1                       ; 0 off, 1 auto, 2 on
ata_usedma   db 0                       ; this chunk, not this device
ata_p32      db 0                       ; ditto, for the data port width
ata_bmok     db 0                       ; a bus-master controller was found
ata_progif   db 0                       ; and which of its channels are
            db  0                       ; in compatibility mode
ata_bmbad   db  0                       ; the last failure was the engine's
ata_bmst    db  0                       ; and its status when it ended
            db  0
            db  0
ata_bmbase  dw  0                       ; BAR4: the engine's register block
            dw  0
ata_pcidev  dd  0                       ; where in configuration space
ata_pciid   dd  0                       ; and what it says it is
ata_prdaddr dd  0                       ; the descriptor table, placed by
                                        ; ata_prdinit rather than declared

ata_devs    db  ATA_MAXDEV * ATD_LEN dup(0)
ata_idbuf   db  512 dup(0)

; Twice the room the table needs, so that a start which does not cross
; a 64K boundary can always be found inside it.
ata_prdraw  db  (ATA_PRDMAX * PRD_LEN * 2) + 8 dup(0)
