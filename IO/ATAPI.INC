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

        ; HOW THIS DEVICE WILL MOVE ITS DATA, decided here for the same
        ; reason a disk's is decided in ata_identify: every input is in
        ; ata_idbuf and the next IDENTIFY overwrites it.  A packet
        ; device answers words 49, 63 and 88 exactly as a disk does, so
        ; the policy IDEDMA= sets applies to a CD-ROM unchanged - which
        ; is the point.  ata_pick32 is NOT asked: the 32-bit data port
        ; is a PIO question and the PIO path here is the fallback.
        call    ata_pickbm
        call    ata_pickdma

        ; polled driver: this device is to keep its interrupt to itself.
        ; ata_identify does this for a disk and never reaches it for a
        ; packet device, and a bus-master transfer is exactly where an
        ; unhandled interrupt would start to matter.
        push    edx
        movzx   edx,word ptr [esi+ATD_CTRL]
        mov     al,ATA_DC_NIEN
        out     dx,al
        pop     edx

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
        mov     eax,ecx                 ; remember the words moved
        cld
        cmp     byte ptr [atapi_dir],0
        jne     apk_out
        mov     edi,[atapi_buf]
        rep     insw
        mov     [atapi_buf],edi
        jmp     short apk_moved
apk_out:
        ; THE SAME LOOP THE OTHER WAY ROUND, for the handful of
        ; commands that SEND a parameter list - setting the audio
        ; volume is the one that made this necessary.  ESI is the
        ; device entry and ata_delay reads it, so it is borrowed and
        ; put back rather than simply loaded.
        push    esi
        mov     esi,[atapi_buf]
        rep     outsw
        mov     [atapi_buf],esi
        pop     esi
apk_moved:
        shl     eax,1                   ; -> bytes moved
        add     [atapi_got],eax
        sub     [atapi_left],eax

        mov     ecx,[atapi_drain]
        jecxz   apk_nodrain
        cmp     byte ptr [atapi_dir],0
        jne     apk_pad
apk_dump:
        in      ax,dx
        loop    apk_dump
        jmp     short apk_nodrain
apk_pad:
        ; The drive wants more than the caller has.  Padding it is the
        ; same lesson as draining: a burst left half finished wedges
        ; the channel, and the next command would be the one asking
        ; what went wrong.
        xor     eax,eax
apk_padlp:
        out     dx,ax
        loop    apk_padlp
apk_nodrain:
        pop     edx
        jmp     apk_loop

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
; atapi_dmapkt - run the command block in atapi_cdb with the DATA
;                PHASE DONE BY THE BUS MASTER.  ESI = device; the
;                buffer is already described in the descriptor table.
;                CF set on failure.
;
; THE ORDER IS A DISK'S ORDER WITH ONE EXTRA STEP IN THE MIDDLE: arm
; the engine, issue the command, hand over the packet, and only then
; start the engine.  A packet command has a PIO phase whatever its
; data phase is - the twelve bytes of the command block always go out
; through the data port, one word at a time - and the engine must not
; be running while they do.
;
; The byte count limit in LBA1/LBA2 is MEANINGLESS here and is written
; as zero.  There are no bursts to announce, because the drive is not
; talking to the processor at all: it asserts DMARQ and the engine
; answers, and the first the processor hears of it is the ACTIVE bit
; going down.
;
; What this does NOT do is decide anything.  Whether a transfer may go
; this way is atapi_xfer's question and ata_dmaok's answer.
;=====================================================================
atapi_dmapkt:
        pushad
        mov     [atapi_dev],esi
        movzx   edx,word ptr [esi+ATD_BASE]
        call    ata_select
        jc      apdm_fail

        call    ata_dmaprep             ; the table, and the direction

        push    edx
        add     dx,ATA_FEATURES
        mov     al,1                    ; bit 0: this transfer is by DMA
        out     dx,al
        pop     edx
        push    edx
        add     dx,ATA_SECCNT
        xor     al,al
        out     dx,al
        pop     edx
        push    edx
        add     dx,ATA_LBA1
        xor     al,al
        out     dx,al
        inc     dx
        out     dx,al
        pop     edx

        push    edx
        add     dx,ATA_COMMAND
        mov     al,ATA_CMD_PACKET
        out     dx,al
        pop     edx
        call    ata_delay

        call    ata_waitdrq             ; the drive asks for the block
        jc      apdm_fail
        push    edx
        add     dx,ATA_DATA
        mov     esi,OFFSET atapi_cdb
        mov     ecx,ATAPI_PKTLEN / 2
        cld
        rep     outsw
        pop     edx
        mov     esi,[atapi_dev]         ; ata_dmarun reads the device

        call    ata_dmarun
        jc      apdm_fail
        popad
        clc
        ret
apdm_fail:
        popad
        stc
        ret

;=====================================================================
; atapi_xfer - run the command block, moving ECX bytes into EDI by
;              whichever path this device and this buffer allow.
;              ESI = device.  EAX = bytes delivered, CF on failure.
;
; THE QUESTION IS ASKED PER TRANSFER AND NOT PER DEVICE, exactly as
; ata_rw asks it, because two of the three answers depend on WHERE the
; buffer is rather than on what the drive can do - and that changes
; call by call.  A buffer in the band the A20 gate aliases, or one
; that upper memory has moved somewhere else, is not an address a bus
; master may be handed, and PIO needs no address at all.
;
; A WHOLE NUMBER OF 512-BYTE SECTORS is the fourth condition, and it
; is ours rather than the hardware's: ata_dmaok and ata_prdbuild count
; in sectors because everything else that calls them is a disk.  A CD
; block is four of them, so every read that gets here satisfies it,
; and a status reply of eight bytes does not - which is the right
; answer for a different reason, that arming an engine to move eight
; bytes costs more than moving them.
;=====================================================================
atapi_xfer:
        mov     byte ptr [atapi_bydma],0
        test    byte ptr [esi+ATD_FLAGS],ATDF_DMA
        jz      axf_pio
        test    ecx,511
        jnz     axf_pio

        mov     [ata_buf],edi
        push    ecx
        shr     ecx,9
        mov     [ata_chunk],ecx
        pop     ecx
        mov     byte ptr [ata_dir],0    ; a CD is read-only here
        call    ata_dmaok
        jc      axf_pio
        call    ata_prdbuild
        jc      axf_pio

        call    atapi_dmapkt
        jnc     axf_moved

        ; THE ENGINE'S FAULT OR THE DRIVE'S?  ata_dmarun has already
        ; decided, and the answer is acted on exactly as ata_rw acts on
        ; it: a drive that failed has really failed, and a bus master
        ; that has got one transfer wrong is not trusted with the
        ; retry - this device drops to PIO for the rest of the session
        ; and the same read is done again the slow way.
        cmp     byte ptr [ata_bmbad],0
        je      axf_fail
        and     byte ptr [esi+ATD_FLAGS],ATDF_NODMA
        mov     byte ptr [esi+ATD_DMA],0

axf_pio:
        call    atapi_packet
        ret
axf_moved:
        mov     byte ptr [atapi_bydma],1
        mov     eax,ecx                 ; the engine moves all of it or the
        clc                             ; descriptor table never runs out
        ret
axf_fail:
        xor     eax,eax
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
        ; ...AND THE DIRECTION WITH IT.  Every builder starts here, so
        ; clearing it here is what stops a command that SENT data
        ; leaving the next ordinary read expecting to send some.
        mov     byte ptr [atapi_dir],0
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
; atapi_idx - ESI = a device entry -> EAX = its slot, 0..3.  Every
;             other register preserved.
;
; The bookkeeping below is indexed by SLOT rather than by unit number,
; because a slot is a fact about the hardware and a unit number is a
; position in a list the probe is entitled to rebuild.
;=====================================================================
atapi_idx:
        push    ecx
        push    edx
        mov     eax,esi
        sub     eax,OFFSET ata_devs
        xor     edx,edx
        mov     ecx,ATD_LEN
        div     ecx
        cmp     eax,ATAPI_MAXDEV
        jb      aidx_ok
        xor     eax,eax                 ; out of range: slot zero is a safer
aidx_ok:                                ; answer than an index off the end
        pop     edx
        pop     ecx
        ret

;=====================================================================
; atapi_pendall - the medium in ESI has moved, and EVERY listener has
;                 to be told.  All registers preserved.
;
; THE DRIVE SAYS IT ONCE.  A disc change arrives as a sense condition
; on the next command given to the drive, and being told CLEARS it -
; so whichever part of the kernel asks first destroys the answer for
; everybody else.  While the volume layer was the only thing that ever
; spoke to a CD that was safe by luck.  It is not safe now: a program
; polling the Q channel through the MSCDEX API can easily be the
; command the change is reported on, and the volume layer would carry
; on using the extents of a disc that is no longer in the drive.
;
; So the report is caught HERE, where every failed command passes, and
; kept as one bit per listener until that listener asks.  PMIO does
; not know or care who the listeners are: it keeps the bits and hands
; each one back once.
;=====================================================================
atapi_pendall:
        push    eax
        call    atapi_idx
        or      byte ptr [atapi_pend+eax],0FFh
        pop     eax
        ret

;=====================================================================
; atapi_sense - REQUEST SENSE on ESI, into atapi_skey/asc/ascq.
;               All registers preserved.
;
; THE CALLER'S COMMAND BLOCK IS SAVED AND PUT BACK, because asking why
; a command failed must not destroy the command that failed: a retry
; is the usual next move and it needs the block intact.  The direction
; goes with it, for the same reason and because atapi_cdbzero clears
; it on the way past.
;=====================================================================
atapi_sense:
        pushad
        mov     [asen_dev],esi
        mov     al,byte ptr [atapi_dir]
        mov     byte ptr [asen_dir],al
        mov     byte ptr [atapi_skey],0
        mov     byte ptr [atapi_asc],0
        mov     byte ptr [atapi_ascq],0

        mov     esi,OFFSET atapi_cdb
        mov     edi,OFFSET atapi_cdbsave
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb

        call    atapi_cdbzero
        mov     byte ptr [atapi_cdb+0],SCSI_REQSENSE
        mov     byte ptr [atapi_cdb+4],18       ; all a fixed-format reply
        mov     esi,[asen_dev]                  ; can be
        mov     edi,OFFSET atapi_data
        mov     ecx,18
        call    atapi_packet
        jc      asen_put                        ; no answer: the zeros stand
        cmp     eax,14                          ; ASC and ASCQ live at 12
        jb      asen_put                        ; and 13
        mov     al,byte ptr [atapi_data+2]
        and     al,0Fh
        mov     byte ptr [atapi_skey],al
        mov     al,byte ptr [atapi_data+12]
        mov     byte ptr [atapi_asc],al
        mov     al,byte ptr [atapi_data+13]
        mov     byte ptr [atapi_ascq],al
asen_put:
        mov     esi,OFFSET atapi_cdbsave
        mov     edi,OFFSET atapi_cdb
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb
        mov     al,byte ptr [asen_dir]
        mov     byte ptr [atapi_dir],al
        popad
        ret
asen_dev    dd 0
asen_dir    db 0

;=====================================================================
; atapi_note - a command on ESI has just failed.  Find out whether it
;              failed because the disc is not the disc any more.
;              All registers preserved.
;
; Sense key 6 is UNIT ATTENTION - the drive saying "something about me
; changed since you last spoke to me": a new disc, a tray closed, a
; reset.  Key 2 is NOT READY - a tray standing open, or nothing in the
; drive.  Both mean whatever was mounted is no longer trustworthy.
;
; NO SENSE AT ALL counts as a change too, and deliberately: the drive
; did not answer the question, so nothing has been ruled out, and the
; only cost of assuming the worst is one extra mount.
;=====================================================================
atapi_note:
        pushad
        call    atapi_sense
        mov     al,byte ptr [atapi_skey]
        cmp     al,6
        je      anote_changed
        cmp     al,2
        je      anote_changed
        or      al,al
        jnz     anote_out               ; a real failure of some other kind
anote_changed:
        call    atapi_pendall
anote_out:
        popad
        ret

;=====================================================================
; atapi_run - run the command block in atapi_cdb against ESI, and if
;             it fails, ASK THE DRIVE WHY before reporting it.
;
;   EDI = buffer, ECX = its size (zero for a command with no data)
;   -> EAX = bytes moved, CF on failure
;
; Every command this driver issues goes through here now, and that is
; the point: the one thing a failure has to do besides failing is
; leave a record that the medium moved, and there is no reliable place
; to do that except where the failures are.
;=====================================================================
atapi_run:
        or      ecx,ecx
        jz      arun_nodata
        call    atapi_xfer              ; bus master if the transfer allows
        jmp     short arun_test
arun_nodata:
        call    atapi_packet            ; a command with no data phase is
                                        ; not a transfer to decide about
arun_test:
        jnc     arun_ok
        push    eax
        call    atapi_note
        pop     eax
        stc
        ret
arun_ok:
        clc
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
        call    atapi_run
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
; atapi_chkchg - has the disc in ESI been changed?  CF set if whatever
;                was mounted on it has to be mounted again.
;
; TEST UNIT READY, ONCE, AND THE FAILURE IS THE ANSWER.  There is no
; disk-change line on an ATAPI drive the way there is on a floppy
; controller: the drive reports "not ready to ready transition, medium
; may have changed" as a sense condition on the next command it is
; given, and that command fails.  So the cheapest command in the set
; is issued and the whole question is whether it worked.
;
; THE RETRY IS DELIBERATELY NOT HERE, and that is the trap this
; routine exists to avoid.  atapi_ready asks up to sixteen times,
; because a drive with a disc newly pushed in spends a second or two
; answering "becoming ready" and giving up on that would report an
; empty drive.  But asking again also CLEARS the condition - the drive
; reports it exactly once - so a retry is a second question whose
; answer the first question has already destroyed.  This asks once and
; leaves the waiting to the re-mount that follows.
;=====================================================================
atapi_chkchg:
        pushad
        call    atapi_cdbzero
        mov     byte ptr [atapi_cdb+0],SCSI_TESTREADY
        xor     ecx,ecx
        xor     edi,edi
        call    atapi_run
        jc      apc_changed
        popad
        clc
        ret
apc_changed:
        ; ANY failure of this command is a change as far as the volume
        ; layer is concerned - an empty drive and an open tray want the
        ; same answer as a swapped disc - so the listeners are marked
        ; here even when the sense atapi_run collected did not say so.
        call    atapi_pendall
        popad
        stc
        ret

;=====================================================================
; atapi_svcchg - IOS_CDCHG.  AL = unit; CF set if the volume on it
;                must be mounted again.
;
; A unit that is not there answers "changed" rather than an error of
; its own, because the caller's next move either way is to mount the
; drive and find out - and a mount that fails says so properly.
;=====================================================================
atapi_svcchg:
        push    eax
        push    ebx
        push    esi
        movzx   ebx,ah                  ; the listener's bit, kept
        or      bl,bl
        jnz     asc_have
        mov     bl,01h                  ; an AH nobody filled in is the
                                        ; volume layer, which was the only
                                        ; asker there ever was
asc_have:
        call    atapi_slot              ; AL -> ESI
        jc      asc_yes
        call    atapi_chkchg            ; one TEST UNIT READY; any failure
                                        ; has already been recorded for
                                        ; every listener
        call    atapi_idx               ; EAX = the slot
        test    byte ptr [atapi_pend+eax],bl
        jz      asc_no
        not     bl                      ; asked and answered: this listener
        and     byte ptr [atapi_pend+eax],bl    ; is up to date now
        jmp     short asc_yes
asc_no:
        pop     esi
        pop     ebx
        pop     eax
        clc
        ret
asc_yes:
        pop     esi
        pop     ebx
        pop     eax
        stc
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
        call    atapi_run
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
        call    atapi_run               ; bus master if it can, PIO if not,
        jc      ard_bad                 ; and the drive asked why on failure

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

;=====================================================================
; atapi_svcread - IOS_CDREAD.  AL = unit, EBX = block, ECX = count,
;                 EDI = buffer.  CF if there is no such unit or the
;                 read failed.
;
; The unit is looked up on every call rather than cached, because a
; caller holds a unit NUMBER and the table it indexes is the probe's,
; which nothing here is entitled to assume has not been rebuilt.
;=====================================================================
atapi_svcread:
        push    esi
        call    atapi_slot              ; AL -> ESI
        jc      asr_bad
        call    atapi_read
        jc      asr_bad
        pop     esi
        clc
        ret
asr_bad:
        pop     esi
        stc
        ret

;=====================================================================
; atapi_svcinfo - IOS_CDINFO.  AL = unit -> EAX = last block, EBX =
;                 block size, ECX = units present.
;
; ECX IS FILLED IN EVEN WHEN THE CALL FAILS, so that asking about unit
; zero on a machine with no CD-ROM answers "none" rather than leaving
; the caller to tell "no such unit" apart from "no drives at all".
;=====================================================================
atapi_svcinfo:
        push    esi
        push    edx
        call    atapi_slot              ; AL -> ESI
        jc      asi_bad
        call    atapi_ready
        jc      asi_bad
        call    atapi_capacity          ; -> EAX, EBX
        jc      asi_bad
        mov     ecx,[atapi_count]
        pop     edx
        pop     esi
        clc
        ret
asi_bad:
        mov     ecx,[atapi_count]
        xor     eax,eax
        xor     ebx,ebx
        pop     edx
        pop     esi
        stc
        ret

;=====================================================================
; atapi_svcpkt - IOS_CDPKT.  Run one packet command the CALLER built.
;
;   AL  = ATAPI unit      AH = 1 if the data phase sends
;   ESI = the twelve-byte command block
;   EDI = the buffer      ECX = how many bytes it holds
;   -> EAX = bytes moved, EBX = key OR ASC shl 8 OR ASCQ shl 16,
;      CF on failure
;
; WHY A PASSTHROUGH RATHER THAN A CALL PER COMMAND.  Everything the
; kernel needs from a CD beyond reading blocks - the table of
; contents, playing audio, where the head is, locking the tray - is a
; SCSI command block and nothing else, and the knowledge of WHICH
; block to build belongs with the code that knows what it wants.  That
; code is the MSCD001 device over in PMDOS, because the MSCDEX request
; it is answering is where the question came from.  PMIO's business is
; getting twelve bytes to the drive and the answer back, which is what
; it was already for.
;
; IT IS ALWAYS PIO.  Every reply that comes this way is tens of bytes
; - a table of contents, a mode page, a Q channel position - and
; arming a bus master to move them costs more than moving them.  The
; block reads worth a DMA transfer go through IOS_CDREAD, which
; decides properly, per transfer.
;
; A UNIT ATTENTION IS RETRIED ONCE, which is the one place this
; differs from IOS_CDREAD: the command never ran, so running it again
; IS the fix, and a program that asked where the head was while
; somebody shut the tray gets an answer instead of an error.  The
; change is still remembered for the volume layer either way - that
; is atapi_note's doing and it happens before the retry.
;=====================================================================
atapi_svcpkt:
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        mov     ebp,ecx                 ; the length, for the retry
        mov     [apkt_buf],edi
        mov     [apkt_dir],ah
        mov     [apkt_unit],al
        mov     byte ptr [atapi_skey],0
        mov     byte ptr [atapi_asc],0
        mov     byte ptr [atapi_ascq],0

        call    atapi_cdbzero           ; and the direction with it
        mov     edi,OFFSET atapi_cdb
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb                   ; ESI is the caller's block
        mov     al,[apkt_dir]
        mov     byte ptr [atapi_dir],al

        mov     al,[apkt_unit]
        call    atapi_slot              ; AL -> ESI
        jc      apkt_nounit
        mov     [apkt_dev],esi

        mov     ecx,ebp
        mov     edi,[apkt_buf]
        call    atapi_packet
        jnc     apkt_ok

        mov     esi,[apkt_dev]
        call    atapi_note              ; remembers a change, and puts the
                                        ; command block back for the retry
        cmp     byte ptr [atapi_skey],6 ; UNIT ATTENTION: it never ran
        jne     apkt_fail
        mov     esi,[apkt_dev]
        mov     ecx,ebp
        mov     edi,[apkt_buf]
        call    atapi_packet
        jc      apkt_fail
apkt_ok:
        mov     [apkt_got],eax
        xor     ebx,ebx
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        mov     eax,[apkt_got]
        clc
        ret
apkt_nounit:
        xor     eax,eax
apkt_fail:
        mov     [apkt_got],eax
        movzx   ebx,byte ptr [atapi_ascq]
        shl     ebx,8
        mov     bl,byte ptr [atapi_asc]
        shl     ebx,8
        mov     bl,byte ptr [atapi_skey]
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        mov     eax,[apkt_got]
        stc
        ret

apkt_buf    dd 0
apkt_dev    dd 0
apkt_got    dd 0
apkt_dir    db 0
apkt_unit   db 0

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

        ; WHICH PATH THAT READ ACTUALLY TOOK.  IDETEST can say that a
        ; packet device is in a DMA mode, and that is not the same
        ; claim: atapi_xfer asks four more questions before it uses
        ; one, and every "no" is silent by design.  This is the only
        ; place that says which of them answered.
        mov     esi,OFFSET msgPio
        cmp     byte ptr [atapi_bydma],0
        je      apst_saidhow
        mov     esi,OFFSET msgDma
apst_saidhow:
        call    con_str
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
msgDma      db  ' by DMA',0
msgPio      db  ' by PIO',0
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
atapi_bydma  db 0                       ; the last transfer went by bus
                                        ; master.  Written on every read and
                                        ; only ever read by the selftest,
                                        ; which is the one thing that can
                                        ; tell "in a DMA mode" from "used it"

atapi_dir    db 0                       ; 0 the data phase receives, 1 it
                                        ; sends.  Cleared by atapi_cdbzero,
                                        ; so a builder that does not think
                                        ; about it cannot inherit one
atapi_skey   db 0                       ; the last REQUEST SENSE: key,
atapi_asc    db 0                       ; additional sense code,
atapi_ascq   db 0                       ; and its qualifier
atapi_pend   db ATAPI_MAXDEV dup (0)    ; per SLOT, one bit per listener
                                        ; that has not yet been told the
                                        ; medium moved - see atapi_pendall
atapi_cdbsave db ATAPI_PKTLEN dup (0)   ; the command being asked about,
                                        ; while REQUEST SENSE asks

atapi_cdb    db ATAPI_PKTLEN dup (0)    ; the command block
             db 4 dup (0)               ; pad, so a builder overrunning
                                        ; by a field lands here
atapi_data   db 64 dup (0)              ; small status replies
atapi_blk    db ATAPI_BLKSIZE dup (0)   ; one logical block
