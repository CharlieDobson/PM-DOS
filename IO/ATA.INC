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
; Included by IOPM.ASM.
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
ATA_CMD_IDENT   EQU     0ECh

; Words of interest in the 256-word IDENTIFY block
ATA_ID_CYLS     EQU     1 * 2           ; word - logical cylinders
ATA_ID_HEADS    EQU     3 * 2           ; word - logical heads
ATA_ID_SPT      EQU     6 * 2           ; word - logical sectors per track
ATA_ID_CAPS     EQU     49 * 2          ; bit 9 = LBA supported
ATA_ID_LBA28    EQU     60 * 2          ; dword - LBA28 sector count
ATA_ID_CMDSET   EQU     83 * 2          ; bit 10 = LBA48 supported
ATA_ID_LBA48    EQU     100 * 2         ; qword - LBA48 sector count

; Per-device table entry
ATD_BASE        EQU     0               ; word - command block base port
ATD_CTRL        EQU     2               ; word - control port
ATD_SEL         EQU     4               ; byte - 00h master, 10h slave
ATD_FLAGS       EQU     5               ; byte - see below
ATD_HEADS       EQU     6               ; word - CHS heads
ATD_SPT         EQU     8               ; word - CHS sectors per track
ATD_TOTAL       EQU     12              ; dword - total sectors
ATD_LEN         EQU     16

ATDF_PRESENT    EQU     01h
ATDF_LBA48      EQU     02h
ATDF_LBA        EQU     04h             ; clear = address it the CHS way

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

        mov     al,ATA_CMD_READ28
        cmp     byte ptr [ata_dir],0
        je      arw_go2
        mov     al,ATA_CMD_WRITE28
arw_go2:
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

        mov     al,ATA_CMD_READ28
        cmp     byte ptr [ata_dir],0
        je      arw_chs_go
        mov     al,ATA_CMD_WRITE28
arw_chs_go:
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

        mov     al,ATA_CMD_READ48
        cmp     byte ptr [ata_dir],0
        je      arw_issue
        mov     al,ATA_CMD_WRITE48

;--- issue it and move the data one sector at a time -----------------
arw_issue:
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

        mov     ecx,[ata_chunk]
arw_sector:
        push    ecx
        call    ata_waitdrq
        jc      arw_fail_pop

        push    edx
        add     dx,ATA_DATA
        mov     edi,[ata_buf]
        mov     ecx,256                 ; 512 bytes as words
        cmp     byte ptr [ata_dir],0
        jne     arw_out
        rep     insw
        jmp     short arw_moved
arw_out:
        mov     esi,edi                 ; OUTSW sources from DS:ESI
        rep     outsw
arw_moved:
        pop     edx
        mov     esi,[ata_dev]           ; OUTSW ate ESI
        add     dword ptr [ata_buf],512
        call    ata_delay               ; settle before polling again
        pop     ecx
        dec     ecx
        jnz     arw_sector

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

ata_devs    db  ATA_MAXDEV * ATD_LEN dup(0)
ata_idbuf   db  512 dup(0)
