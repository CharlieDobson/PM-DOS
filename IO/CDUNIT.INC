;=====================================================================
; CDUNIT.INC - the CD-ROM units, whatever they are plugged into
;
; Everything the kernel asks of a CD-ROM is a SCSI command block: READ
; CAPACITY and READ(10) for the filesystem, TEST UNIT READY for a disc
; change, and whatever MSCD001 builds for audio and the rest.  An ATAPI
; drive takes those blocks through an IDE channel's data port; a SCSI
; drive takes the same blocks through a host adapter.  So the questions
; - is there a disc, how big is it, read these blocks, has it changed -
; are asked once, here, of a UNIT, and only the last step, getting a
; block to the drive and the answer back, belongs to the transport:
;
;   CUK_ATAPI   atapi_exec, in ATAPI.INC
;   CUK_SCSI    scs_cdexec, in SCSI.INC, and through it whichever
;               host adapter the device is on
;
; Both take the command in cun_cdb and cun_dir, the buffer in EDI and
; ECX, and give back EAX = bytes moved and CF.  On a failure both have
; already found out why, into cun_skey/cun_asc/cun_ascq: ATAPI by
; asking REQUEST SENSE, SCSI from the sense the adapter fetched by
; itself.  Zeros mean nobody could say.
;
; A UNIT NUMBER IS A PLACE IN cun_tab, and the table is rebuilt from
; the hardware tables whenever they change (cun_build): ATAPI units
; first, in probe order, then SCSI devices in the order the buses were
; scanned.  The ATAPI half is therefore numbered exactly as it was
; before SCSI existed, and a machine without SCSI cannot tell.
;
; THE LISTENER BITS STAY WITH THE HARDWARE.  A disc change is reported
; once and destroyed by the asking (see cun_pendall), so the bits that
; remember it for each listener are kept per device - atapi_pend per
; ATA slot, SDV_PEND in each SCSI device's row - and a unit holds a
; pointer to its device's byte.  Rebuilding the table cannot lose a
; change that was owed.
;
; Included by IOPM.ASM, after ATAPI.INC and SCSI.INC.
;=====================================================================

; The rows of cun_tab (CUE_*) and their kinds (CUK_*) are in PMDOS.INC,
; because ATAPI.INC is assembled first and uses them, and wasm will not
; take an EQU it has not yet seen.

;=====================================================================
; cun_build - the unit table, afresh.  All registers kept.
;
; Called at the end of every ATA probe (ata_init) and every time the
; SCSI tables change.  The order is the whole of the numbering rule.
;=====================================================================
cun_build:
        pushad
        xor     ebp,ebp                 ; EBP = units placed
        mov     edi,OFFSET cun_tab

        xor     ecx,ecx
        mov     esi,OFFSET ata_devs
cnb_ata:
        cmp     ecx,ATA_MAXDEV
        jae     cnb_scsi
        test    byte ptr [esi+ATD_FLAGS],ATDF_ATAPI
        jz      cnb_anext
        cmp     ebp,CDU_MAX
        jae     cnb_done
        mov     byte ptr [edi+CUE_KIND],CUK_ATAPI
        mov     byte ptr [edi+CUE_DEVIDX],cl
        mov     [edi+CUE_DEV],esi
        mov     eax,OFFSET atapi_pend
        add     eax,ecx
        mov     [edi+CUE_PENDP],eax
        add     edi,CUE_LEN
        inc     ebp
cnb_anext:
        add     esi,ATD_LEN
        inc     ecx
        jmp     cnb_ata

cnb_scsi:
        xor     ecx,ecx
        mov     esi,OFFSET scs_devs
cnb_sdev:
        cmp     ecx,[scs_ndev]
        jae     cnb_done
        call    scs_iscd                ; a CD-ROM, on an adapter in use?
        jc      cnb_snext
        cmp     ebp,CDU_MAX
        jae     cnb_done
        mov     byte ptr [edi+CUE_KIND],CUK_SCSI
        mov     byte ptr [edi+CUE_DEVIDX],cl
        mov     [edi+CUE_DEV],esi
        lea     eax,[esi+SDV_PEND]
        mov     [edi+CUE_PENDP],eax
        add     edi,CUE_LEN
        inc     ebp
cnb_snext:
        add     esi,SDV_LEN
        inc     ecx
        jmp     cnb_sdev

cnb_done:
        mov     [cun_count],ebp
        popad
        ret

;=====================================================================
; cun_slot - AL = unit -> ESI = its row.  CF if there is no such unit.
;            Everything else kept.
;
; The unit is looked up on every call rather than cached, because a
; caller holds a unit NUMBER and the table it indexes is rebuilt
; whenever the hardware tables are.
;=====================================================================
cun_slot:
        push    eax
        movzx   eax,al
        cmp     eax,[cun_count]
        jae     cns_bad
        imul    eax,eax,CUE_LEN
        lea     esi,[cun_tab+eax]
        pop     eax
        clc
        ret
cns_bad:
        pop     eax
        stc
        ret

;=====================================================================
; cun_cdbzero - blank the command block.  Every builder starts here,
;               because a stale byte in a reserved field is a command
;               the drive is entitled to refuse.
;=====================================================================
cun_cdbzero:
        push    eax
        push    ecx
        push    edi
        ; ...AND THE DIRECTION WITH IT.  Every builder starts here, so
        ; clearing it here is what stops a command that SENT data
        ; leaving the next ordinary read expecting to send some.
        mov     byte ptr [cun_dir],0
        mov     edi,OFFSET cun_cdb
        xor     eax,eax
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     stosb
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; cun_exec - run the command block in cun_cdb against the unit in ESI.
;
;   EDI = the buffer, ECX = its size (zero for no data phase)
;   -> EAX = bytes moved, CF on failure with cun_skey/asc/ascq set
;
; Nothing but the choice of transport.
;=====================================================================
cun_exec:
        cmp     byte ptr [esi+CUE_KIND],CUK_SCSI
        je      scs_cdexec
        jmp     atapi_exec

;=====================================================================
; cun_pendall - the medium in unit ESI has moved, and EVERY listener
;               has to be told.  All registers kept.
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
cun_pendall:
        push    eax
        mov     eax,[esi+CUE_PENDP]
        or      byte ptr [eax],0FFh
        pop     eax
        ret

;=====================================================================
; cun_note - a command on unit ESI has just failed, and the transport
;            has put what the drive said about it in cun_skey and the
;            rest.  Was it the disc?  All registers kept.
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
cun_note:
        push    eax
        mov     al,byte ptr [cun_skey]
        cmp     al,6
        je      cnote_changed
        cmp     al,2
        je      cnote_changed
        or      al,al
        jnz     cnote_out               ; a real failure of some other kind
cnote_changed:
        call    cun_pendall
cnote_out:
        pop     eax
        ret

;=====================================================================
; cun_run - cun_exec, and if it fails, note whether the disc moved.
;
;   EDI = buffer, ECX = its size (zero for a command with no data)
;   -> EAX = bytes moved, CF on failure
;
; Every command this layer issues goes through here, and that is the
; point: the one thing a failure has to do besides failing is leave a
; record that the medium moved, and there is no reliable place to do
; that except where the failures are.
;=====================================================================
cun_run:
        call    cun_exec
        jnc     crun_ok
        call    cun_note
        stc
        ret
crun_ok:
        clc
        ret

;=====================================================================
; cun_ready - TEST UNIT READY on unit ESI, asked until the drive either
;             says yes or stops saying "becoming ready".
;             CF set if there is no usable disc.
;=====================================================================
cun_ready:
        pushad
        mov     ecx,ATAPI_READYTRY
crdy_try:
        push    ecx
        call    cun_cdbzero
        mov     byte ptr [cun_cdb+0],SCSI_TESTREADY
        xor     ecx,ecx
        xor     edi,edi
        call    cun_run
        pop     ecx
        jnc     crdy_ok
        dec     ecx
        jnz     crdy_try
        popad
        stc
        ret
crdy_ok:
        popad
        clc
        ret

;=====================================================================
; cun_chkchg - has the disc in unit ESI been changed?  CF set if
;              whatever was mounted on it has to be mounted again.
;
; TEST UNIT READY, ONCE, AND THE FAILURE IS THE ANSWER.  There is no
; disk-change line on these drives the way there is on a floppy
; controller: the drive reports "not ready to ready transition, medium
; may have changed" as a sense condition on the next command it is
; given, and that command fails.  So the cheapest command in the set
; is issued and the whole question is whether it worked.
;
; THE RETRY IS DELIBERATELY NOT HERE, and that is the trap this
; routine exists to avoid.  cun_ready asks up to sixteen times,
; because a drive with a disc newly pushed in spends a second or two
; answering "becoming ready" and giving up on that would report an
; empty drive.  But asking again also CLEARS the condition - the drive
; reports it exactly once - so a retry is a second question whose
; answer the first question has already destroyed.  This asks once and
; leaves the waiting to the re-mount that follows.
;=====================================================================
cun_chkchg:
        pushad
        call    cun_cdbzero
        mov     byte ptr [cun_cdb+0],SCSI_TESTREADY
        xor     ecx,ecx
        xor     edi,edi
        call    cun_run
        jc      cchg_changed
        popad
        clc
        ret
cchg_changed:
        ; ANY failure of this command is a change as far as the volume
        ; layer is concerned - an empty drive and an open tray want the
        ; same answer as a swapped disc - so the listeners are marked
        ; here even when the sense the transport collected did not say so.
        call    cun_pendall
        popad
        stc
        ret

;=====================================================================
; cun_capacity - READ CAPACITY on unit ESI.
;                -> EAX = last logical block, EBX = block size.
;                CF set if the drive would not say.
;
; Both fields come back BIG-endian, which is why they are reassembled
; a byte at a time rather than read as dwords.
;=====================================================================
cun_capacity:
        push    ecx
        push    edx
        push    edi
        push    esi
        call    cun_cdbzero
        mov     byte ptr [cun_cdb+0],SCSI_READCAP
        mov     edi,OFFSET cun_data
        mov     ecx,8
        call    cun_run
        jc      ccap_no
        cmp     eax,8
        jb      ccap_no

        movzx   eax,byte ptr [cun_data+0]
        shl     eax,8
        mov     al,byte ptr [cun_data+1]
        shl     eax,8
        mov     al,byte ptr [cun_data+2]
        shl     eax,8
        mov     al,byte ptr [cun_data+3]

        movzx   ebx,byte ptr [cun_data+4]
        shl     ebx,8
        mov     bl,byte ptr [cun_data+5]
        shl     ebx,8
        mov     bl,byte ptr [cun_data+6]
        shl     ebx,8
        mov     bl,byte ptr [cun_data+7]

        pop     esi
        pop     edi
        pop     edx
        pop     ecx
        clc
        ret
ccap_no:
        pop     esi
        pop     edi
        pop     edx
        pop     ecx
        stc
        ret

;=====================================================================
; cun_read - READ(10) from unit ESI.
;
;   EBX = first logical block   ECX = how many   EDI = buffer
;
; CF set on failure.  The caller's buffer must hold ECX * 2048 bytes.
; A SCSI adapter that cannot move that much at once has the request
; cut into pieces underneath (scs_cdexec); the caller never knows.
;=====================================================================
cun_read:
        pushad
        mov     [cun_rdlba],ebx
        mov     [cun_rdcnt],ecx
        mov     [cun_rdbuf],edi

        call    cun_cdbzero
        mov     byte ptr [cun_cdb+0],SCSI_READ10

        ; the block, big-endian, in bytes 2..5
        mov     eax,[cun_rdlba]
        mov     byte ptr [cun_cdb+5],al
        shr     eax,8
        mov     byte ptr [cun_cdb+4],al
        shr     eax,8
        mov     byte ptr [cun_cdb+3],al
        shr     eax,8
        mov     byte ptr [cun_cdb+2],al

        ; how many blocks, big-endian, in bytes 7..8
        mov     eax,[cun_rdcnt]
        mov     byte ptr [cun_cdb+8],al
        shr     eax,8
        mov     byte ptr [cun_cdb+7],al

        mov     eax,[cun_rdcnt]
        imul    eax,eax,ATAPI_BLKSIZE
        mov     ecx,eax
        mov     edi,[cun_rdbuf]
        call    cun_run                 ; and the drive asked why on failure
        jc      crd_bad

        ; short is wrong here, unlike a status read: a caller asking
        ; for four blocks and given three has no way to notice
        mov     ecx,[cun_rdcnt]
        imul    ecx,ecx,ATAPI_BLKSIZE
        cmp     eax,ecx
        jb      crd_bad
        popad
        clc
        ret
crd_bad:
        popad
        stc
        ret

;=====================================================================
; cun_svcread - IOS_CDREAD.  AL = unit, EBX = block, ECX = count,
;               EDI = buffer.  CF if there is no such unit or the
;               read failed.
;=====================================================================
cun_svcread:
        push    esi
        call    cun_slot                ; AL -> ESI
        jc      csr_bad
        call    cun_read
        jc      csr_bad
        pop     esi
        clc
        ret
csr_bad:
        pop     esi
        stc
        ret

;=====================================================================
; cun_svcinfo - IOS_CDINFO.  AL = unit -> EAX = last block, EBX =
;               block size, ECX = units present.
;
; ECX IS FILLED IN EVEN WHEN THE CALL FAILS, so that asking about unit
; zero on a machine with no CD-ROM answers "none" rather than leaving
; the caller to tell "no such unit" apart from "no drives at all".
;=====================================================================
cun_svcinfo:
        push    esi
        push    edx
        call    cun_slot                ; AL -> ESI
        jc      csi_bad
        call    cun_ready
        jc      csi_bad
        call    cun_capacity            ; -> EAX, EBX
        jc      csi_bad
        mov     ecx,[cun_count]
        pop     edx
        pop     esi
        clc
        ret
csi_bad:
        mov     ecx,[cun_count]
        xor     eax,eax
        xor     ebx,ebx
        pop     edx
        pop     esi
        stc
        ret

;=====================================================================
; cun_svcchg - IOS_CDCHG.  AL = unit, AH = the listener's CDL_ bit;
;              CF set if the volume on it must be mounted again.
;
; A unit that is not there answers "changed" rather than an error of
; its own, because the caller's next move either way is to mount the
; drive and find out - and a mount that fails says so properly.
;=====================================================================
cun_svcchg:
        push    eax
        push    ebx
        push    esi
        movzx   ebx,ah                  ; the listener's bit, kept
        or      bl,bl
        jnz     ccg_have
        mov     bl,01h                  ; an AH nobody filled in is the
                                        ; volume layer, which was the only
                                        ; asker there ever was
ccg_have:
        call    cun_slot                ; AL -> ESI
        jc      ccg_yes
        call    cun_chkchg              ; one TEST UNIT READY; any failure
                                        ; has already been recorded for
                                        ; every listener
        mov     eax,[esi+CUE_PENDP]
        test    byte ptr [eax],bl
        jz      ccg_no
        not     bl                      ; asked and answered: this listener
        and     byte ptr [eax],bl       ; is up to date now
        jmp     short ccg_yes
ccg_no:
        pop     esi
        pop     ebx
        pop     eax
        clc
        ret
ccg_yes:
        pop     esi
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; cun_svcunit - IOS_CDUNIT.  AL = unit -> EDX = its ATA device index,
;               EBX = its channel's base port, and in the high word its
;               control port, ECX = units present.  CF if there is no
;               such unit.
;
; THE DRIVE IS NOT SPOKEN TO.  IOS_CDINFO is TEST UNIT READY and a
; capacity read, and fails with the tray open - which is right for a
; caller about to read the disc and wrong for one that wants to say
; which drive letter D: is.  This is the table lookup alone, so that a
; report can name a drive with nothing in it.
;
; A SCSI unit has no ATA index: EDX is -1 for one, and EBX is its
; adapter's port.  SSF_CDDESC is the call that describes either kind.
;=====================================================================
cun_svcunit:
        push    eax
        push    esi
        call    cun_slot                ; AL -> ESI
        jc      csu_bad
        cmp     byte ptr [esi+CUE_KIND],CUK_SCSI
        je      csu_scsi
        movzx   edx,byte ptr [esi+CUE_DEVIDX]
        mov     esi,[esi+CUE_DEV]
        movzx   ebx,word ptr [esi+ATD_CTRL]     ; the control port above,
        shl     ebx,16                          ; for RMCD.SYS - the park
        mov     bx,word ptr [esi+ATD_BASE]      ; hands it the channel
        jmp     short csu_have
csu_scsi:
        mov     edx,-1
        mov     esi,[esi+CUE_DEV]
        movzx   eax,byte ptr [esi+SDV_HBA]
        imul    eax,eax,HBA_LEN
        movzx   ebx,word ptr [scs_hbas+eax+HBA_BASE]
csu_have:
        mov     ecx,[cun_count]
        pop     esi
        pop     eax
        clc
        ret
csu_bad:
        mov     ecx,[cun_count]
        mov     edx,-1
        xor     ebx,ebx
        pop     esi
        pop     eax
        stc
        ret

;=====================================================================
; cun_desc - SSF_CDDESC.  AL = unit, EDI -> CDU_LEN bytes: what the
;            unit is and where, every field filled.  ECX = units.  CF
;            if there is no such unit, and the record is left alone.
;
; Asks the drive nothing, for cun_svcunit's reason.
;=====================================================================
cun_desc:
        push    eax
        push    ebx
        push    edx
        push    esi
        push    edi
        call    cun_slot                ; AL -> ESI
        jc      cdsc_bad
        mov     [edi+CDU_UNIT],al
        cmp     byte ptr [esi+CUE_KIND],CUK_SCSI
        je      cdsc_scsi

        ; AN IDE CHANNEL: exactly the record 21h/F1h AL=17h built from
        ; IOS_CDUNIT, IOS_ATAMODEL and IOS_ATAINFO before there was any
        ; other kind
        mov     byte ptr [edi+CDU_KIND],CDK_ATAPI
        mov     byte ptr [edi+CDU_TARGET],0
        mov     al,byte ptr [esi+CUE_DEVIDX]
        mov     [edi+CDU_DEV],al
        mov     ebx,[esi+CUE_DEV]
        mov     bx,word ptr [ebx+ATD_BASE]
        mov     [edi+CDU_BASE],bx
        push    edi
        add     edi,CDU_MODEL
        call    ata_model               ; AL = the device; all kept
        pop     edi
        push    edi
        call    ata_info                ; -> AL = flags, CL = the mode
        pop     edi
        mov     [edi+CDU_FLAGS],al
        mov     [edi+CDU_MODE],cl
        jmp     short cdsc_ok

cdsc_scsi:
        mov     esi,[esi+CUE_DEV]       ; the SDV row
        mov     byte ptr [edi+CDU_KIND],CDK_SCSI
        mov     al,[esi+SDV_LUN]
        shl     al,4
        or      al,[esi+SDV_TARGET]
        mov     [edi+CDU_TARGET],al
        movzx   eax,byte ptr [esi+SDV_HBA]
        mov     [edi+CDU_DEV],al
        imul    eax,eax,HBA_LEN
        add     eax,OFFSET scs_hbas
        mov     bl,[eax+HBA_KIND]
        mov     [edi+CDU_FLAGS],bl
        mov     bl,[eax+HBA_ID]
        mov     [edi+CDU_MODE],bl
        mov     bx,[eax+HBA_BASE]
        mov     [edi+CDU_BASE],bx
        ; INQUIRY's three strings, with a blank between each, in the
        ; forty bytes IDENTIFY's model used to fill
        push    edi
        add     edi,CDU_MODEL
        mov     ecx,ATA_MODEL_LEN
        mov     al,' '
        cld
        push    edi
        rep     stosb
        pop     edi
        lea     esi,[esi+SDV_INQ]
        mov     ecx,8                   ; vendor
        rep     movsb
        inc     edi
        mov     ecx,16                  ; product
        rep     movsb
        inc     edi
        mov     ecx,4                   ; revision
        rep     movsb
        pop     edi
cdsc_ok:
        mov     ecx,[cun_count]
        pop     edi
        pop     esi
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret
cdsc_bad:
        mov     ecx,[cun_count]
        pop     edi
        pop     esi
        pop     edx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; cun_svcpkt - IOS_CDPKT.  Run one command block the CALLER built.
;
;   AL  = unit            AH = 1 if the data phase sends
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
; ON AN IDE CHANNEL IT IS ALWAYS PIO.  Every reply that comes this way
; is tens of bytes - a table of contents, a mode page, a Q channel
; position - and arming a bus master to move them costs more than
; moving them.  The block reads worth a DMA transfer go through
; IOS_CDREAD, which decides properly, per transfer.  (A SCSI adapter
; moves everything the one way it can.)
;
; A UNIT ATTENTION IS RETRIED ONCE, which is the one place this
; differs from IOS_CDREAD: the command never ran, so running it again
; IS the fix, and a program that asked where the head was while
; somebody shut the tray gets an answer instead of an error.  The
; change is still remembered for the volume layer either way - that
; is cun_note's doing and it happens before the retry.
;=====================================================================
cun_svcpkt:
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        mov     ebp,ecx                 ; the length, for the retry
        mov     [cpkt_buf],edi
        mov     [cpkt_dir],ah
        mov     [cpkt_unit],al
        mov     byte ptr [cun_skey],0
        mov     byte ptr [cun_asc],0
        mov     byte ptr [cun_ascq],0

        call    cun_cdbzero             ; and the direction with it
        mov     edi,OFFSET cun_cdb
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb                   ; ESI is the caller's block
        mov     al,[cpkt_dir]
        mov     byte ptr [cun_dir],al

        mov     al,[cpkt_unit]
        call    cun_slot                ; AL -> ESI
        jc      cpkt_nounit
        mov     [cpkt_dev],esi

        mov     byte ptr [cun_pio],1
        mov     ecx,ebp
        mov     edi,[cpkt_buf]
        call    cun_exec
        jnc     cpkt_ok

        mov     esi,[cpkt_dev]
        call    cun_note                ; remembers a change
        cmp     byte ptr [cun_skey],6   ; UNIT ATTENTION: it never ran
        jne     cpkt_fail
        mov     esi,[cpkt_dev]
        mov     ecx,ebp
        mov     edi,[cpkt_buf]
        call    cun_exec
        jnc     cpkt_ok
        call    cun_note
        jmp     short cpkt_fail
cpkt_ok:
        mov     byte ptr [cun_pio],0
        mov     [cpkt_got],eax
        xor     ebx,ebx
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        mov     eax,[cpkt_got]
        clc
        ret
cpkt_nounit:
        xor     eax,eax
cpkt_fail:
        mov     byte ptr [cun_pio],0
        mov     [cpkt_got],eax
        movzx   ebx,byte ptr [cun_ascq]
        shl     ebx,8
        mov     bl,byte ptr [cun_asc]
        shl     ebx,8
        mov     bl,byte ptr [cun_skey]
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        mov     eax,[cpkt_got]
        stc
        ret

cpkt_buf    dd 0
cpkt_dev    dd 0
cpkt_got    dd 0
cpkt_dir    db 0
cpkt_unit   db 0

IFDEF ATA_DEBUG
;=====================================================================
; cun_selftest - the probe's own proof, printed at boot under
;                BUILD ata.
;
; It asks the three questions in the order a driver has to ask them,
; and each one can only be reached if the one before it worked:
; READ CAPACITY says the transport carries a reply, and READ(10) of
; block 16 says the data path carries a whole block - 16 being where
; ISO9660 puts its primary volume descriptor, whose second through
; sixth bytes are the letters CD001.  Getting those five characters
; out of a CD is the smallest complete end-to-end result this layer
; can produce.
;
; It runs at the end of the IDE probe, before any SCSI adapter has
; been looked for, so unit 0 here is always an ATAPI drive.
;=====================================================================
cun_selftest:
        pushad
        mov     esi,OFFSET msgPkt
        call    con_str
        mov     eax,[cun_count]
        call    ata_hex16
        cmp     dword ptr [cun_count],0
        je      cst_done

        xor     al,al
        call    cun_slot                ; -> ESI
        jc      cst_done
        mov     [cst_dev],esi

        call    cun_ready
        jc      cst_notready

        mov     esi,[cst_dev]
        call    cun_capacity            ; -> EAX last block, EBX size
        jc      cst_nocap
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
        mov     edi,OFFSET cun_blk
        mov     eax,0DEADBEEFh          ; poison, so a no-op is visible
        mov     ecx,ATAPI_BLKSIZE / 4
        cld
        rep     stosd
        mov     esi,[cst_dev]
        mov     ebx,16
        mov     ecx,1
        mov     edi,OFFSET cun_blk
        call    cun_read
        jc      cst_noread
        mov     ecx,5
        mov     esi,OFFSET cun_blk + 1
cst_sig:
        lodsb
        call    con_out
        loop    cst_sig

        ; WHICH PATH THAT READ ACTUALLY TOOK.  IDETEST can say that a
        ; packet device is in a DMA mode, and that is not the same
        ; claim: atapi_xfer asks four more questions before it uses
        ; one, and every "no" is silent by design.  This is the only
        ; place that says which of them answered.
        mov     esi,OFFSET msgPio
        cmp     byte ptr [atapi_bydma],0
        je      cst_saidhow
        mov     esi,OFFSET msgDma
cst_saidhow:
        call    con_str
        jmp     short cst_done
cst_noread:
        mov     esi,OFFSET msgNoRead
        call    con_str
        jmp     short cst_done
cst_nocap:
        mov     esi,OFFSET msgNoCap
        call    con_str
        jmp     short cst_done
cst_notready:
        mov     esi,OFFSET msgNoDisc
        call    con_str
cst_done:
        mov     al,13
        call    con_out
        mov     al,10
        call    con_out
        popad
        ret

cst_dev     dd 0
msgPkt      db  ' ATAPI units=',0
msgCap      db  ' cap=',0
msgPvd      db  ' pvd=',0
msgNoDisc   db  ' no disc',0
msgNoCap    db  ' READ CAPACITY refused',0
msgNoRead   db  ' READ(10) refused',0
msgDma      db  ' by DMA',0
msgPio      db  ' by PIO',0
cun_blk     db ATAPI_BLKSIZE dup (0)    ; one logical block
ENDIF

;=====================================================================
; The table, and the command being built
;=====================================================================
cun_count   dd 0                        ; rows in cun_tab
cun_rdlba   dd 0
cun_rdcnt   dd 0
cun_rdbuf   dd 0
cun_tab     db CDU_MAX * CUE_LEN dup (0)

cun_dir     db 0                        ; 0 the data phase receives, 1 it
                                        ; sends.  Cleared by cun_cdbzero,
                                        ; so a builder that does not think
                                        ; about it cannot inherit one
cun_pio     db 0                        ; 1: on an IDE channel, by PIO
                                        ; whatever the transfer could do
cun_skey    db 0                        ; why the last command failed: the
cun_asc     db 0                        ; sense key, the additional sense
cun_ascq    db 0                        ; code and its qualifier
cun_cdb     db ATAPI_PKTLEN dup (0)     ; the command block
            db 4 dup (0)                ; pad, so a builder overrunning
                                        ; by a field lands here
cun_data    db 64 dup (0)               ; small replies
