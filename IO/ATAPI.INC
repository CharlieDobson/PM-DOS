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
; WHAT IS NOT HERE ANY MORE is every question asked OF a CD - is there
; a disc, how big, read these blocks, has it changed.  Those are the
; same for a SCSI drive and live in CDUNIT.INC, which calls atapi_exec
; for a unit on an IDE channel.  This file gets a command block to the
; drive and the answer back, and asks REQUEST SENSE when it fails.
;
; WHY A CD-ROM IS NOT A BIOS DRIVE HERE.  ata_slot hands out 80h, 81h
; and so on in probe order to everything carrying ATDF_PRESENT, so
; giving a CD-ROM that flag would renumber the hard disks behind it -
; a machine with the CD on the primary slave would find its second
; disk had moved.  An ATAPI unit gets ATDF_ATAPI and NOT ATDF_PRESENT:
; ata_slot, ata_count and the whole INT 13h path carry on seeing
; exactly the disks they saw before, and CDUNIT.INC numbers the packet
; devices separately.
;
; Included by IOPM.ASM, after ATA.INC.
;=====================================================================

ATAPI_MAXDEV    EQU     ATA_MAXDEV      ; the same slots

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
        call    ata_wake                ; AL = the status, once it has one
        mov     ah,10h
        mov     edi,PLI_ST3
        call    ata_plogb

        ; ata_probedrq and not ata_waitdrq: since a device that signed
        ; nothing is asked this too, the question can be put to one
        ; that will never answer it
        call    ata_probedrq
        jc      apid_no

        push    edx
        add     dx,ATA_DATA
        mov     edi,OFFSET ata_idbuf
        mov     ecx,256
        cld
        rep     insw
        pop     edx

        ; A PACKET DEVICE SAYS SO IN WORD 0: bits 15 and 14 are 10.  A
        ; drive that signed itself is not asked - it has said what it
        ; is already.  One that did not is, because a disk that failed
        ; IDENTIFY DEVICE gets here as well now, and 512 bytes of
        ; whatever it had to say are not a CD-ROM
        cmp     byte ptr [ata_stage],'K'
        je      apid_is
        mov     ax,word ptr [ata_idbuf]
        and     ax,0C000h
        cmp     ax,8000h
        jne     apid_no
apid_is:

        or      byte ptr [esi+ATD_FLAGS],ATDF_ATAPI
        mov     byte ptr [ata_stage],'A'

        ; WHAT CHOOSING THIS DEVICE'S TRANSFER MODE WILL NEED, written
        ; down here for the same reason a disk's is in ata_identify:
        ; every input is in ata_idbuf and the next IDENTIFY overwrites
        ; it.  A packet device answers words 49, 63, 88 and 93 exactly
        ; as a disk does, so the policy IDEDMA= sets - and the cable
        ; it is limited by - applies to a CD-ROM unchanged, which is
        ; the point.  ata_init does the choosing, once the whole
        ; channel has answered.  ata_pick32 is NOT asked: the 32-bit
        ; data port is a PIO question and the PIO path here is the
        ; fallback.
        call    ata_keepid
        call    ata_pickbm

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
; atapi_packet - run the command block in cun_cdb against ESI.
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
        mov     esi,OFFSET cun_cdb
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
        cmp     byte ptr [cun_dir],0
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
        cmp     byte ptr [cun_dir],0
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
; atapi_dmapkt - run the command block in cun_cdb with the DATA
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
        mov     esi,OFFSET cun_cdb
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
; atapi_exec - the ATAPI transport for CDUNIT.INC: run the command
;              block in cun_cdb against the unit in ESI.
;
;   EDI = buffer, ECX = its size (zero for a command with no data)
;   -> EAX = bytes moved, CF on failure, and then cun_skey/asc/ascq
;      say why, as REQUEST SENSE did - zeros if it would not
;
; A transfer goes by bus master if atapi_xfer finds that it may, and
; by PIO otherwise; a command with no data phase is not a transfer to
; decide about, and neither is one the caller asked to keep to PIO
; (cun_pio, which IOS_CDPKT sets: see cun_svcpkt).
;=====================================================================
atapi_exec:
        push    esi
        mov     esi,[esi+CUE_DEV]       ; the ATA device entry
        or      ecx,ecx
        jz      aex_pio
        cmp     byte ptr [cun_pio],0
        jne     aex_pio
        call    atapi_xfer
        jmp     short aex_test
aex_pio:
        call    atapi_packet
aex_test:
        jnc     aex_ok
        call    atapi_sense             ; all registers kept
        pop     esi
        stc
        ret
aex_ok:
        pop     esi
        clc
        ret

;=====================================================================
; atapi_sense - REQUEST SENSE on the ATA device ESI, into cun_skey/
;               cun_asc/cun_ascq.  All registers preserved.
;
; THE CALLER'S COMMAND BLOCK IS SAVED AND PUT BACK, because asking why
; a command failed must not destroy the command that failed: a retry
; is the usual next move and it needs the block intact.  The direction
; goes with it, for the same reason and because cun_cdbzero clears
; it on the way past.
;=====================================================================
atapi_sense:
        pushad
        mov     [asen_dev],esi
        mov     al,byte ptr [cun_dir]
        mov     byte ptr [asen_dir],al
        mov     byte ptr [cun_skey],0
        mov     byte ptr [cun_asc],0
        mov     byte ptr [cun_ascq],0

        mov     esi,OFFSET cun_cdb
        mov     edi,OFFSET atapi_cdbsave
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb

        call    cun_cdbzero
        mov     byte ptr [cun_cdb+0],SCSI_REQSENSE
        mov     byte ptr [cun_cdb+4],18         ; all a fixed-format reply
        mov     esi,[asen_dev]                  ; can be
        mov     edi,OFFSET atapi_sbuf
        mov     ecx,18
        call    atapi_packet
        jc      asen_put                        ; no answer: the zeros stand
        cmp     eax,14                          ; ASC and ASCQ live at 12
        jb      asen_put                        ; and 13
        mov     al,byte ptr [atapi_sbuf+2]
        and     al,0Fh
        mov     byte ptr [cun_skey],al
        mov     al,byte ptr [atapi_sbuf+12]
        mov     byte ptr [cun_asc],al
        mov     al,byte ptr [atapi_sbuf+13]
        mov     byte ptr [cun_ascq],al
asen_put:
        mov     esi,OFFSET atapi_cdbsave
        mov     edi,OFFSET cun_cdb
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb
        mov     al,byte ptr [asen_dir]
        mov     byte ptr [cun_dir],al
        popad
        ret
asen_dev    dd 0
asen_dir    db 0

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
atapi_bydma  db 0                       ; the last transfer went by bus
                                        ; master.  Written on every read and
                                        ; only ever read by the selftest,
                                        ; which is the one thing that can
                                        ; tell "in a DMA mode" from "used it"

atapi_pend   db ATAPI_MAXDEV dup (0)    ; per SLOT, one bit per listener
                                        ; that has not yet been told the
                                        ; medium moved - see cun_pendall
atapi_cdbsave db ATAPI_PKTLEN dup (0)   ; the command being asked about,
                                        ; while REQUEST SENSE asks
atapi_sbuf   db 20 dup (0)              ; REQUEST SENSE's reply
