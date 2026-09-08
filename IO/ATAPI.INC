;=====================================================================
; ATAPI.INC - the packet interface, on top of ATA.INC's transport
;
; An ATAPI device is an ATA device that takes SCSI command blocks
; instead of ATA commands: you write a twelve-byte packet to the data
; port and then move the data exactly the way a PIO transfer moves it.
; Everything underneath - port layout, select, the status waits, the
; 400ns settle - is ATA.INC's and is used unchanged.  What is new is
; the command layer above it and one thing that has no ATA analogue:
; THE DEVICE DECIDES HOW MUCH IT SENDS.  A disk read moves exactly the
; sectors you asked for; a packet command comes back in bursts whose
; size the drive announces in LBA1/LBA2 as each one starts, and the
; host has to keep taking them until DRQ stays low.  So the data loop
; here is driven by the drive rather than by a count.
;
; WHY A CD-ROM IS NOT A BIOS DRIVE HERE.  ata_slot hands out 80h, 81h
; and so on in probe order to everything carrying ATDF_PRESENT, so
; giving a CD-ROM that flag would renumber the hard disks behind it -
; a machine with the CD on the primary slave would find its second
; disk had moved.  An ATAPI unit gets ATDF_ATAPI and NOT ATDF_PRESENT:
; ata_slot, ata_count and the whole INT 13h path carry on seeing
; exactly the disks they saw before, and atapi_slot indexes the packet
; devices separately.
;
; Included by IOPM.ASM, after ATA.INC.
;=====================================================================

ATAPI_MAXDEV    EQU     ATA_MAXDEV      ; the same four slots

ATA_CMD_PACKET  EQU     0A0h            ; "here comes a command block"
ATA_CMD_IDENTPKT EQU    0A1h            ; IDENTIFY PACKET DEVICE

; The signature a packet device leaves in LBA1/LBA2 when a plain
; IDENTIFY DEVICE is aborted: LBA1 = 14h, LBA2 = EBh.
ATAPI_SIG       EQU     014EBh          ; as AH:AL from ata_identify

ATAPI_PKTLEN    EQU     12              ; command block, in bytes
ATAPI_BLKSIZE   EQU     2048            ; a CD-ROM logical block

; How many bytes the host will take in one burst.  Announced to the
; drive in LBA1/LBA2 before the command; it must be even, and a whole
; logical block is the natural figure.
ATAPI_BCL       EQU     2048

; The SCSI operations used here.  A packet is a SCSI command block,
; and its multi-byte fields are BIG-endian, which is why every one of
; them is assembled a byte at a time below.
SCSI_TESTREADY  EQU     000h
SCSI_REQSENSE   EQU     003h
SCSI_STARTSTOP  EQU     01Bh
SCSI_READCAP    EQU     025h
SCSI_READ10     EQU     028h

; A drive with a disc newly inserted answers "not ready, becoming
; ready" for a while, and the honest way to deal with that is to ask
; again rather than to report no disc.
ATAPI_READYTRY  EQU     16

;=====================================================================
; atapi_ident - ata_identify found the packet signature on ESI.
;
; DX = base port.  Runs IDENTIFY PACKET DEVICE and, if it answers,
; claims the slot as an ATAPI unit.  Returns nothing: the caller
; treats every packet device as "not a hard disk" either way, so
; failing here simply means the slot stays empty.
;=====================================================================
atapi_ident:
        pushad
        movzx   edx,word ptr [esi+ATD_BASE]
        call    ata_select
        jc      apid_no

        push    edx
        add     dx,ATA_COMMAND
        mov     al,ATA_CMD_IDENTPKT
        out     dx,al
        pop     edx
        call    ata_delay
        call    ata_waitdrq
        jc      apid_no

        push    edx
        add     dx,ATA_DATA
        mov     edi,OFFSET ata_idbuf
        mov     ecx,256
        cld
        rep     insw
        pop     edx

        or      byte ptr [esi+ATD_FLAGS],ATDF_ATAPI
        inc     dword ptr [atapi_count]
apid_no:
        popad
        ret

;=====================================================================
; atapi_slot - AL = which packet device (0 = the first found) -> ESI =
;              its device table entry.  CF set if there is no such
;              unit.  Preserves everything else.
;=====================================================================
atapi_slot:
        push    eax
        push    ebx
        push    ecx
        movzx   ebx,al
        xor     ecx,ecx
        mov     esi,OFFSET ata_devs
aps_scan:
        cmp     ecx,ATAPI_MAXDEV
        jae     aps_bad
        test    byte ptr [esi+ATD_FLAGS],ATDF_ATAPI
        jz      aps_next
        or      ebx,ebx
        jz      aps_found
        dec     ebx
aps_next:
        add     esi,ATD_LEN
        inc     ecx
        jmp     short aps_scan
aps_found:
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
aps_bad:
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; atapi_packet - run the command block in atapi_cdb against ESI.
;
;   ESI = device entry
;   EDI = where the data goes (ignored when ECX is zero)
;   ECX = how many bytes the caller will accept
;
; Returns EAX = bytes actually delivered, CF set if the drive reported
; an error or stopped answering.
;
; A BURST LONGER THAN THE CALLER'S BUFFER IS DRAINED, NOT ABANDONED.
; Leaving data sitting in the drive wedges the next command on that
; channel, and the next command is usually REQUEST SENSE asking what
; went wrong - so the one path that most wants to work would be the
; one guaranteed to fail.
;=====================================================================
atapi_packet:
        pushad
        mov     [atapi_dev],esi
        mov     [atapi_buf],edi
        mov     [atapi_left],ecx
        mov     dword ptr [atapi_got],0

        movzx   edx,word ptr [esi+ATD_BASE]
        call    ata_select
        jc      apk_fail

        ; Features 0: PIO, no overlap, no DMA.
        push    edx
        add     dx,ATA_FEATURES
        xor     al,al
        out     dx,al
        pop     edx
        push    edx
        add     dx,ATA_SECCNT
        xor     al,al
        out     dx,al
        pop     edx

        ; How much this host will take at a time.
        push    edx
        add     dx,ATA_LBA1
        ; wasm 1.9 will not take LOW/HIGH on a constant here, so the
        ; two bytes are cut out with AND and SHR - parenthesised,
        ; because this assembler's expression parser has been caught
        ; associating a bare chain the wrong way before now.
        mov     al,(ATAPI_BCL AND 0FFh)
        out     dx,al
        inc     dx
        mov     al,((ATAPI_BCL SHR 8) AND 0FFh)
        out     dx,al
        pop     edx

        push    edx
        add     dx,ATA_COMMAND
        mov     al,ATA_CMD_PACKET
        out     dx,al
        pop     edx
        call    ata_delay

        ; The drive asks for the command block.
        call    ata_waitdrq
        jc      apk_fail
        push    edx
        add     dx,ATA_DATA
        mov     esi,OFFSET atapi_cdb
        mov     ecx,ATAPI_PKTLEN / 2
        cld
        rep     outsw
        pop     edx
        mov     esi,[atapi_dev]         ; ata_delay reads ESI

;--- the data phase, for as long as the drive keeps asking -----------
apk_loop:
        call    ata_delay
        call    ata_waitbsy
        jc      apk_fail
        push    edx
        add     dx,ATA_STATUS
        in      al,dx
        pop     edx
        test    al,ATA_ST_ERR OR ATA_ST_DF
        jnz     apk_fail
        test    al,ATA_ST_DRQ
        jz      apk_done                ; nothing more: the command is over

        ; how many bytes this burst carries
        push    edx
        add     dx,ATA_LBA1
        in      al,dx
        mov     bl,al
        inc     dx
        in      al,dx
        mov     bh,al
        pop     edx
        movzx   ebx,bx
        or      ebx,ebx
        jz      apk_done                ; a zero-length burst ends it

        ; A burst is always a whole number of words.
        mov     ecx,ebx
        inc     ecx
        shr     ecx,1                   ; ECX = words in the burst

        ; split it into what fits and what has to be thrown away
        mov     eax,[atapi_left]
        shr     eax,1
        cmp     ecx,eax
        jbe     apk_allfit
        mov     [atapi_drain],ecx
        sub     [atapi_drain],eax
        mov     ecx,eax
        jmp     short apk_in
apk_allfit:
        mov     dword ptr [atapi_drain],0
apk_in:
        push    edx
        add     dx,ATA_DATA
        mov     edi,[atapi_buf]
        mov     eax,ecx                 ; remember the words kept
        cld
        rep     insw
        mov     [atapi_buf],edi
        shl     eax,1                   ; -> bytes kept
        add     [atapi_got],eax
        sub     [atapi_left],eax

        mov     ecx,[atapi_drain]
        jecxz   apk_nodrain
apk_dump:
        in      ax,dx
        loop    apk_dump
apk_nodrain:
        pop     edx
        jmp     short apk_loop

apk_done:
        ; The status that ended the transfer has already been read and
        ; had ERR clear, so the command stands.
        mov     eax,[atapi_got]
        mov     [atapi_result],eax
        popad
        mov     eax,[atapi_result]
        clc
        ret
apk_fail:
        mov     eax,[atapi_got]
        mov     [atapi_result],eax
        popad
        mov     eax,[atapi_result]
        stc
        ret

;=====================================================================
; atapi_cdbzero - blank the command block.  Every builder below starts
;                 here, because a stale byte in a reserved field is a
;                 command the drive is entitled to refuse.
;=====================================================================
atapi_cdbzero:
        push    eax
        push    ecx
        push    edi
        mov     edi,OFFSET atapi_cdb
        xor     eax,eax
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     stosb
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; atapi_ready - TEST UNIT READY on ESI, asked until the drive either
;               says yes or stops saying "becoming ready".
;               CF set if there is no usable disc.
;=====================================================================
atapi_ready:
        pushad
        mov     ecx,ATAPI_READYTRY
ardy_try:
        push    ecx
        call    atapi_cdbzero
        mov     byte ptr [atapi_cdb+0],SCSI_TESTREADY
        xor     ecx,ecx
        xor     edi,edi
        call    atapi_packet
        pop     ecx
        jnc     ardy_ok
        dec     ecx
        jnz     ardy_try
        popad
        stc
        ret
ardy_ok:
        popad
        clc
        ret

;=====================================================================
; atapi_capacity - READ CAPACITY on ESI.
;                  -> EAX = last logical block, EBX = block size.
;                  CF set if the drive would not say.
;
; Both fields come back BIG-endian, which is why they are reassembled
; a byte at a time rather than read as dwords.
;=====================================================================
atapi_capacity:
        push    ecx
        push    edx
        push    edi
        push    esi
        call    atapi_cdbzero
        mov     byte ptr [atapi_cdb+0],SCSI_READCAP
        mov     edi,OFFSET atapi_data
        mov     ecx,8
        call    atapi_packet
        jc      acap_no
        cmp     eax,8
        jb      acap_no

        movzx   eax,byte ptr [atapi_data+0]
        shl     eax,8
        mov     al,byte ptr [atapi_data+1]
        shl     eax,8
        mov     al,byte ptr [atapi_data+2]
        shl     eax,8
        mov     al,byte ptr [atapi_data+3]

        movzx   ebx,byte ptr [atapi_data+4]
        shl     ebx,8
        mov     bl,byte ptr [atapi_data+5]
        shl     ebx,8
        mov     bl,byte ptr [atapi_data+6]
        shl     ebx,8
        mov     bl,byte ptr [atapi_data+7]

        pop     esi
        pop     edi
        pop     edx
        pop     ecx
        clc
        ret
acap_no:
        pop     esi
        pop     edi
        pop     edx
        pop     ecx
        stc
        ret

;=====================================================================
; atapi_read - READ(10) from the device in ESI.
;
;   EBX = first logical block   ECX = how many   EDI = buffer
;
; CF set on failure.  The caller's buffer must hold ECX * 2048 bytes.
;=====================================================================
atapi_read:
        pushad
        mov     [atapi_rdlba],ebx
        mov     [atapi_rdcnt],ecx
        mov     [atapi_rdbuf],edi

        call    atapi_cdbzero
        mov     byte ptr [atapi_cdb+0],SCSI_READ10

        ; the block, big-endian, in bytes 2..5
        mov     eax,[atapi_rdlba]
        mov     byte ptr [atapi_cdb+5],al
        shr     eax,8
        mov     byte ptr [atapi_cdb+4],al
        shr     eax,8
        mov     byte ptr [atapi_cdb+3],al
        shr     eax,8
        mov     byte ptr [atapi_cdb+2],al

        ; how many blocks, big-endian, in bytes 7..8
        mov     eax,[atapi_rdcnt]
        mov     byte ptr [atapi_cdb+8],al
        shr     eax,8
        mov     byte ptr [atapi_cdb+7],al

        mov     eax,[atapi_rdcnt]
        imul    eax,ATAPI_BLKSIZE
        mov     ecx,eax
        mov     edi,[atapi_rdbuf]
        call    atapi_packet
        jc      ard_bad

        ; short is wrong here, unlike a status read: a caller asking
        ; for four blocks and given three has no way to notice
        mov     ecx,[atapi_rdcnt]
        imul    ecx,ATAPI_BLKSIZE
        cmp     eax,ecx
        jb      ard_bad
        popad
        clc
        ret
ard_bad:
        popad
        stc
        ret

IFDEF ATA_DEBUG
;=====================================================================
; atapi_selftest - the probe's own proof, printed at boot under
;                  BUILD ata.
;
; It asks the three questions in the order a driver has to ask them,
; and each one can only be reached if the one before it worked:
; READ CAPACITY says the packet transport carries a reply, and
; READ(10) of block 16 says the data path carries a whole block - 16
; being where ISO9660 puts its primary volume descriptor, whose second
; through sixth bytes are the letters CD001.  Getting those five
; characters out of a CD is the smallest complete end-to-end result
; this layer can produce.
;=====================================================================
atapi_selftest:
        pushad
        mov     esi,OFFSET msgPkt
        call    con_str
        mov     eax,[atapi_count]
        call    ata_hex16
        cmp     dword ptr [atapi_count],0
        je      apst_done

        xor     al,al
        call    atapi_slot              ; -> ESI
        jc      apst_done
        mov     [apst_dev],esi

        call    atapi_ready
        jc      apst_notready

        mov     esi,[apst_dev]
        call    atapi_capacity          ; -> EAX last block, EBX size
        jc      apst_nocap
        push    ebx
        mov     esi,OFFSET msgCap
        call    con_str
        push    eax
        shr     eax,16
        call    ata_hex16
        pop     eax
        call    ata_hex16
        mov     al,'/'
        call    con_out
        pop     eax                     ; the block size
        call    ata_hex16

        ; and now a real block, through the real read path
        mov     esi,OFFSET msgPvd
        call    con_str
        mov     edi,OFFSET atapi_blk
        mov     eax,0DEADBEEFh          ; poison, so a no-op is visible
        mov     ecx,ATAPI_BLKSIZE / 4
        cld
        rep     stosd
        mov     esi,[apst_dev]
        mov     ebx,16
        mov     ecx,1
        mov     edi,OFFSET atapi_blk
        call    atapi_read
        jc      apst_noread
        mov     ecx,5
        mov     esi,OFFSET atapi_blk + 1
apst_sig:
        lodsb
        call    con_out
        loop    apst_sig
        jmp     short apst_done
apst_noread:
        mov     esi,OFFSET msgNoRead
        call    con_str
        jmp     short apst_done
apst_nocap:
        mov     esi,OFFSET msgNoCap
        call    con_str
        jmp     short apst_done
apst_notready:
        mov     esi,OFFSET msgNoDisc
        call    con_str
apst_done:
        mov     al,13
        call    con_out
        mov     al,10
        call    con_out
        popad
        ret

apst_dev    dd 0
msgPkt      db  ' ATAPI units=',0
msgCap      db  ' cap=',0
msgPvd      db  ' pvd=',0
msgNoDisc   db  ' no disc',0
msgNoCap    db  ' READ CAPACITY refused',0
msgNoRead   db  ' READ(10) refused',0
ENDIF

;=====================================================================
; Driver state
;=====================================================================
atapi_count  dd 0                       ; packet devices found
atapi_dev    dd 0                       ; the one in play
atapi_buf    dd 0                       ; where the next burst lands
atapi_left   dd 0                       ; room still in the caller's buffer
atapi_got    dd 0                       ; bytes delivered so far
atapi_drain  dd 0                       ; words to read and throw away
atapi_result dd 0                       ; EAX across the popad
atapi_rdlba  dd 0
atapi_rdcnt  dd 0
atapi_rdbuf  dd 0

atapi_cdb    db ATAPI_PKTLEN dup (0)    ; the command block
             db 4 dup (0)               ; pad, so a builder overrunning
                                        ; by a field lands here
atapi_data   db 64 dup (0)              ; small status replies
atapi_blk    db ATAPI_BLKSIZE dup (0)   ; one logical block
