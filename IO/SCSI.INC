;=====================================================================
; SCSI.INC - host adapters, the devices on their buses, and the SCSI
;            transport behind CDUNIT.INC
;
; A SCSI CD-ROM takes exactly the command blocks an ATAPI one does -
; ATAPI is SCSI's command set carried over an IDE cable - so what this
; file adds is the carrying: which adapter, which target, and how a
; request gets to it and back.  Three kinds of adapter, each a driver
; of its own behind one entry point (HBA_EXEC):
;
;   AHA154X.INC   Adaptec AHA-154x, and the BusLogic cards that speak
;                 its mailbox interface, ISA and PCI
;   NCR5380.INC   the SCSI port on a Pro Audio Spectrum sound card -
;                 an NCR 53C80 the processor drives itself
;   ASPI.INC      (in PMDOS) anything a loaded ASPI manager drives,
;                 which is how an AHA-2940 or a Sound Blaster's
;                 SCSI-2 port is reached: the vendor's own driver
;
; THE TABLES.  scs_hbas holds the adapters and scs_devs every device
; that answered INQUIRY on any of their buses, of every type - a disk
; on the bus is listed too, because "why is my drive not there" is
; answered by showing what IS there.  CDUNIT.INC takes the CD-ROMs out
; of scs_devs when it builds the unit table (scs_iscd).
;
; ONE REQUEST AT A TIME, POLLED.  The kernel never has two commands in
; flight and never waits for an interrupt from a disk; neither does
; this.  Every driver spins on its card's registers with a bound, the
; way ATA.INC does, so nothing here depends on the interrupt flag or
; on a line being free.
;
; WHEN THE PROBE RUNS.  Not at boot with the IDE probe: PMDOS asks for
; it (SSF_PROBE) after CONFIG.SYS, because two things in CONFIG.SYS
; decide it.  SCSI=OFF says not to look at all, and a DEVICE= line may
; have loaded an ASPI manager that already owns an adapter - which the
; probe must find out BEFORE it resets the card, since the reset is
; what would take the card away from the manager (SPF_ASPI).
;
; Included by IOPM.ASM, after ATAPI.INC and before the drivers.
;=====================================================================

SCSI_INQUIRY    EQU     012h
SCSI_MODESEL6   EQU     015h
SCSI_MODESNS6   EQU     01Ah
SCSI_MODESEL10  EQU     055h
SCSI_MODESNS10  EQU     05Ah
SCSI_READ12     EQU     0A8h
SCSI_READCD     EQU     0BEh

SCSI_INQLEN     EQU     36              ; the standard INQUIRY reply
SCSI_ZEROMAX    EQU     4096            ; a reply this short or shorter is
                                        ; zero-filled before it is asked for
SCSI_MODEMAX    EQU     255             ; most a 6-byte MODE SENSE can carry
SCSI_INQTRY     EQU     3               ; INQUIRY asked this often: a device
                                        ; just out of reset may refuse once

;=====================================================================
; scs_svc - IOS_SCSI.  AH = SSF_*; the registers are that function's,
;           as PMDOS.INC lists them.
;=====================================================================
scs_svc:
        cmp     ah,SSF_PROBE
        je      scs_probe
        cmp     ah,SSF_ADDHBA
        je      scs_addhba
        cmp     ah,SSF_ADDDEV
        je      scs_adddev
        cmp     ah,SSF_BUILD
        je      scs_build
        cmp     ah,SSF_ADAPTER
        je      scs_getadapter
        cmp     ah,SSF_DEVICE
        je      scs_getdevice
        cmp     ah,SSF_CDDESC
        je      cun_desc
        stc
        ret

;=====================================================================
; scs_probe - SSF_PROBE.  AL = SPF_* -> ECX = adapters PMIO drives
;             itself.  Everything else kept.
;
; ONCE.  A second call only rebuilds the unit table: looking again
; would reset cards whose devices already have drive letters.
;=====================================================================
scs_probe:
        push    eax
        push    ebx
        push    edx
        push    esi
        push    edi
        push    ebp
        cmp     byte ptr [scs_probed],0
        jne     spr_done
        mov     byte ptr [scs_probed],1
        mov     byte ptr [scs_pflags],al
        test    al,SPF_NATIVE
        jz      spr_done

        call    aha_probe               ; Adaptec and BusLogic
        call    pas_probe               ; the Pro Audio Spectrum
        mov     eax,[scs_nhba]
        mov     [scs_nnative],eax

        ; every bus just found, target by target
        xor     ebx,ebx
spr_scan:
        cmp     ebx,[scs_nhba]
        jae     spr_done
        call    scs_scanbus
        inc     ebx
        jmp     spr_scan
spr_done:
        call    cun_build
        mov     ecx,[scs_nnative]
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret

;=====================================================================
; scs_build - SSF_BUILD.  -> ECX = CD units, the table rebuilt.
;=====================================================================
scs_build:
        call    cun_build
        mov     ecx,[cun_count]
        clc
        ret

;=====================================================================
; scs_addhba - SSF_ADDHBA, and how the drivers here register too.
;              EDI -> HBA_LEN bytes -> EAX = the adapter's number.
;              CF if the table is full.  Everything else kept.
;=====================================================================
scs_addhba:
        push    ecx
        push    esi
        push    edi
        mov     eax,[scs_nhba]
        cmp     eax,SCSI_MAXHBA
        jae     sah_full
        push    eax
        mov     esi,edi
        imul    eax,eax,HBA_LEN
        lea     edi,[scs_hbas+eax]
        mov     ecx,HBA_LEN
        cld
        rep     movsb
        pop     eax
        inc     dword ptr [scs_nhba]
        pop     edi
        pop     esi
        pop     ecx
        clc
        ret
sah_full:
        pop     edi
        pop     esi
        pop     ecx
        stc
        ret

;=====================================================================
; scs_adddev - SSF_ADDDEV, and the bus scan's own way in.
;              AL = adapter, DL = target, DH = LUN, ESI -> SCSI_INQLEN
;              bytes of INQUIRY data.  CF if the table is full.  All
;              registers kept.
;
; THE SAME DRIVE SEEN TWICE.  An ASPI manager lists every adapter it
; drives, and one of them may be a card PMIO found and is driving too -
; the Pro Audio Spectrum's port, which has no state to show who else
; is using it.  A row from an ASPI adapter that matches one PMIO
; already has, target for target and byte for byte, is kept for the
; listing but marked SDF_DUP, and never becomes a second drive letter
; for the same disc.  Two identical drives at the same ID on two
; different buses would be taken for one; that is the price, and it
; is a small one.
;=====================================================================
scs_adddev:
        pushad
        mov     ebx,[scs_ndev]
        cmp     ebx,SCSI_MAXDEV
        jae     sad_full
        imul    ebx,ebx,SDV_LEN
        lea     edi,[scs_devs+ebx]
        push    eax
        push    edi
        xor     eax,eax
        mov     ecx,SDV_LEN
        cld
        rep     stosb
        pop     edi
        pop     eax
        mov     [edi+SDV_HBA],al
        mov     [edi+SDV_TARGET],dl
        mov     [edi+SDV_LUN],dh
        mov     al,[esi]
        mov     [edi+SDV_TYPE],al
        mov     al,[esi+1]
        mov     [edi+SDV_RMB],al
        mov     al,[esi+2]
        and     al,07h                  ; the ANSI version
        mov     [edi+SDV_ANSI],al

        ; vendor, product and revision, with anything unprintable made
        ; a blank: a report prints these, and a drive that pads with
        ; zeros should not end the string halfway through
        push    esi
        push    edi
        add     esi,8
        add     edi,SDV_INQ
        mov     ecx,28
sad_char:
        lodsb
        cmp     al,' '
        jb      sad_blank
        cmp     al,7Eh
        jbe     sad_put
sad_blank:
        mov     al,' '
sad_put:
        stosb
        loop    sad_char
        pop     edi
        pop     esi

        call    scs_isdup
        jc      sad_new
        or      byte ptr [edi+SDV_FLAGS],SDF_DUP
sad_new:
IFDEF SCSI_MODE6
        ; BUILD scsi6: every drive is taken for one with only the 6-byte
        ; MODE commands, so that the translation in sce_msense and
        ; sce_msel runs on a machine whose drives would never need it
        or      byte ptr [edi+SDV_FLAGS],SDF_MODE6
ENDIF
        inc     dword ptr [scs_ndev]
        popad
        clc
        ret
sad_full:
        popad
        stc
        ret

;---------------------------------------------------------------------
; scs_isdup - EDI -> a new row.  CF CLEAR if it is a device PMIO
;             already drives on another adapter (see scs_adddev);
;             CF set if it is new.  All registers kept.
;---------------------------------------------------------------------
scs_isdup:
        pushad
        movzx   eax,byte ptr [edi+SDV_HBA]
        imul    eax,eax,HBA_LEN
        cmp     byte ptr [scs_hbas+eax+HBA_KIND],SHK_ASPI
        jne     sdp_new                 ; only a manager's view can repeat
        mov     esi,OFFSET scs_devs
        mov     ecx,[scs_ndev]
sdp_row:
        jecxz   sdp_new
        movzx   eax,byte ptr [esi+SDV_HBA]
        imul    eax,eax,HBA_LEN
        cmp     byte ptr [scs_hbas+eax+HBA_KIND],SHK_ASPI
        je      sdp_next
        test    byte ptr [scs_hbas+eax+HBA_FLAGS],SHF_GONE
        jnz     sdp_next
        mov     al,[esi+SDV_TARGET]
        cmp     al,[edi+SDV_TARGET]
        jne     sdp_next
        mov     al,[esi+SDV_LUN]
        cmp     al,[edi+SDV_LUN]
        jne     sdp_next
        mov     al,[esi+SDV_TYPE]
        cmp     al,[edi+SDV_TYPE]
        jne     sdp_next
        push    ecx
        push    esi
        push    edi
        add     esi,SDV_INQ
        add     edi,SDV_INQ
        mov     ecx,28
        cld
        repe    cmpsb
        pop     edi
        pop     esi
        pop     ecx
        je      sdp_dup
sdp_next:
        add     esi,SDV_LEN
        dec     ecx
        jmp     sdp_row
sdp_dup:
        popad
        clc
        ret
sdp_new:
        popad
        stc
        ret

;=====================================================================
; scs_iscd - ESI -> a device row.  CF clear if it is a CD unit: a
;            CD-ROM or a write-once drive, really there (qualifier 0),
;            not a second sighting, on an adapter still in use.  All
;            registers kept.
;=====================================================================
scs_iscd:
        push    eax
        mov     al,[esi+SDV_TYPE]
        test    al,0E0h                 ; the qualifier: connected
        jnz     sic_no
        cmp     al,SCSI_TYPE_CD
        je      sic_type
        cmp     al,SCSI_TYPE_WORM
        jne     sic_no
sic_type:
        test    byte ptr [esi+SDV_FLAGS],SDF_DUP
        jnz     sic_no
        movzx   eax,byte ptr [esi+SDV_HBA]
        imul    eax,eax,HBA_LEN
        test    byte ptr [scs_hbas+eax+HBA_FLAGS],SHF_GONE
        jnz     sic_no
        pop     eax
        clc
        ret
sic_no:
        pop     eax
        stc
        ret

;=====================================================================
; scs_getadapter / scs_getdevice - SSF_ADAPTER, SSF_DEVICE.  AL = which,
;               EDI -> room for the row -> ECX = how many there are.
;               CF if there is no such one; ECX is valid either way.
;=====================================================================
scs_getadapter:
        push    eax
        push    esi
        push    edi
        movzx   eax,al
        mov     ecx,[scs_nhba]
        cmp     eax,ecx
        jae     sga_no
        imul    eax,eax,HBA_LEN
        lea     esi,[scs_hbas+eax]
        push    ecx
        mov     ecx,HBA_LEN
        cld
        rep     movsb
        pop     ecx
        pop     edi
        pop     esi
        pop     eax
        clc
        ret
sga_no:
        pop     edi
        pop     esi
        pop     eax
        stc
        ret

scs_getdevice:
        push    eax
        push    esi
        push    edi
        movzx   eax,al
        mov     ecx,[scs_ndev]
        cmp     eax,ecx
        jae     sgd_no
        imul    eax,eax,SDV_LEN
        lea     esi,[scs_devs+eax]
        push    ecx
        mov     ecx,SDV_LEN
        cld
        rep     movsb
        pop     ecx
        pop     edi
        pop     esi
        pop     eax
        clc
        ret
sgd_no:
        pop     edi
        pop     esi
        pop     eax
        stc
        ret

;=====================================================================
; scs_scanbus - EBX = an adapter PMIO drives.  INQUIRY each target on
;               its bus, LUN 0, and keep what answers.  All kept.
;
; LUN 0 ONLY.  A CD changer puts its discs at LUNs 1 and up, but so do
; plenty of single drives that answer every LUN as if it were the
; first - SCSI-1 has no way to say "no such unit" - and telling the two
; apart takes a table of model names.  One disc per target is what a
; DOS machine with a CD-ROM on a SCSI card has.
;=====================================================================
scs_scanbus:
        pushad
        mov     eax,ebx
        imul    eax,eax,HBA_LEN
        lea     ebp,[scs_hbas+eax]      ; EBP -> the adapter
        xor     edx,edx                 ; DL = target, DH = LUN 0
ssb_target:
        cmp     dl,8
        jae     ssb_done
        cmp     dl,[ebp+HBA_ID]
        je      ssb_next
        call    scs_inquiry             ; EBX = adapter, DL/DH
        jc      ssb_next
        mov     al,byte ptr [scs_inqbuf]
        shr     al,5
        cmp     al,3                    ; "there is no device at this LUN"
        je      ssb_next
        mov     eax,ebx
        mov     esi,OFFSET scs_inqbuf
        call    scs_adddev
        jc      ssb_done                ; the table is full
ssb_next:
        inc     dl
        jmp     ssb_target
ssb_done:
        popad
        ret

;---------------------------------------------------------------------
; scs_inquiry - EBX = adapter, DL = target, DH = LUN.  INQUIRY into
;               scs_inqbuf; CF if nothing answered or it would not
;               say.  All registers kept.
;
; ASKED MORE THAN ONCE IF IT FAILS WITH A CHECK CONDITION.  INQUIRY is
; supposed to answer even with a unit attention pending, and SCSI-2
; drives do; older ones report the attention instead, once.  Nothing
; answering selection is final at the first try.
;---------------------------------------------------------------------
scs_inquiry:
        pushad
        mov     ecx,SCSI_INQTRY
sinq_try:
        push    ecx
        mov     edi,OFFSET scs_isrq
        call    scs_srqzero
        mov     [edi+SRQ_TARGET],dl
        mov     [edi+SRQ_LUN],dh
        mov     byte ptr [edi+SRQ_CDBLEN],6
        mov     byte ptr [edi+SRQ_DIR],SRD_IN
        mov     dword ptr [edi+SRQ_BUF],OFFSET scs_inqbuf
        mov     dword ptr [edi+SRQ_COUNT],SCSI_INQLEN
        mov     byte ptr [edi+SRQ_CDB+0],SCSI_INQUIRY
        mov     al,dh
        shl     al,5                    ; the LUN, where SCSI-1 looks
        mov     [edi+SRQ_CDB+1],al
        mov     byte ptr [edi+SRQ_CDB+4],SCSI_INQLEN
        call    scs_run
        pop     ecx
        jnc     sinq_ok
        cmp     byte ptr [edi+SRQ_HSTAT],SRH_OK
        jne     sinq_bad                ; nobody there, or a dead bus
        dec     ecx
        jnz     sinq_try
sinq_bad:
        popad
        stc
        ret
sinq_ok:
        popad
        clc
        ret

;---------------------------------------------------------------------
; scs_srqzero - blank the request at EDI.  All registers kept.
;---------------------------------------------------------------------
scs_srqzero:
        push    eax
        push    ecx
        push    edi
        xor     eax,eax
        mov     ecx,SRQ_SIZE
        cld
        rep     stosb
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; scs_run - EBX = adapter number, EDI -> a request.  Run it.  CF
;           unless the target said GOOD; the request says what
;           happened (SRQ_HSTAT, SRQ_TSTAT, SRQ_SENSE).  All kept.
;
; WHAT EVERY ADAPTER GETS FOR NOTHING, so that no driver has to do it:
;
;   THE BOUNCE.  A card that masters the ISA bus is handed SCSI_BUF
;   instead of the caller's buffer, and the data is copied across.
;
;   ZEROS FOR WHAT WAS NOT SENT.  A short reply - a table of contents
;   for a disc with three tracks, asked for with room for a hundred -
;   is normal, and most adapters cannot say how short.  So a small
;   reply's buffer is cleared first, and what the target did not fill
;   reads as zeros rather than as whatever was there before.
;
;   THE SENSE.  A CHECK CONDITION with no sense data attached - an
;   adapter that does not fetch it by itself - is followed by REQUEST
;   SENSE here, through the same adapter.
;=====================================================================
scs_run:
        pushad
        call    scs_run1
        jnc     srn_out                 ; GOOD: nothing to ask
        cmp     byte ptr [edi+SRQ_HSTAT],SRH_OK
        jne     srn_fail
        cmp     byte ptr [edi+SRQ_TSTAT],SCSI_ST_CHECK
        jne     srn_fail
        cmp     byte ptr [edi+SRQ_SLEN],0
        jne     srn_fail                ; the adapter fetched it already

        mov     esi,edi                 ; the failed request
        mov     edi,OFFSET scs_ssrq
        call    scs_srqzero
        mov     al,[esi+SRQ_TARGET]
        mov     [edi+SRQ_TARGET],al
        mov     al,[esi+SRQ_LUN]
        mov     [edi+SRQ_LUN],al
        shl     al,5
        mov     [edi+SRQ_CDB+1],al
        mov     byte ptr [edi+SRQ_CDBLEN],6
        mov     byte ptr [edi+SRQ_DIR],SRD_IN
        mov     dword ptr [edi+SRQ_BUF],OFFSET scs_sbuf
        mov     dword ptr [edi+SRQ_COUNT],18
        mov     byte ptr [edi+SRQ_CDB+0],SCSI_REQSENSE
        mov     byte ptr [edi+SRQ_CDB+4],18
        call    scs_run1
        mov     edi,esi
        jc      srn_fail                ; no sense: the zeros stand
        push    esi
        push    edi
        mov     esi,OFFSET scs_sbuf
        add     edi,SRQ_SENSE
        mov     ecx,18
        cld
        rep     movsb
        pop     edi
        pop     esi
        mov     byte ptr [edi+SRQ_SLEN],18
srn_fail:
        popad
        stc
        ret
srn_out:
        popad
        clc
        ret

;---------------------------------------------------------------------
; scs_run1 - scs_run without the sense: one trip to the adapter, with
;            the bounce and the zeros.  EBX = adapter, EDI -> request.
;            CF unless GOOD.  All kept.
;---------------------------------------------------------------------
scs_run1:
        pushad
        mov     byte ptr [edi+SRQ_HSTAT],SRH_ERROR
        mov     byte ptr [edi+SRQ_TSTAT],0
        mov     byte ptr [edi+SRQ_SLEN],0
        mov     dword ptr [edi+SRQ_GOT],0
        cmp     ebx,[scs_nhba]
        jae     sr1_fail
        imul    ebx,ebx,HBA_LEN
        add     ebx,OFFSET scs_hbas     ; EBX -> the adapter from here on
        mov     eax,[edi+SRQ_COUNT]
        cmp     eax,[ebx+HBA_MAXX]
        ja      sr1_fail                ; too big to go in one request

        mov     ebp,[edi+SRQ_BUF]       ; EBP = the caller's buffer
        mov     ecx,[edi+SRQ_COUNT]
        or      ecx,ecx
        jnz     sr1_hasdata
        mov     byte ptr [edi+SRQ_DIR],SRD_NONE
sr1_hasdata:
        test    byte ptr [ebx+HBA_FLAGS],SHF_BOUNCE
        jz      sr1_nobounce
        or      ecx,ecx
        jz      sr1_nobounce
        mov     dword ptr [edi+SRQ_BUF],SCSI_BUF
        cmp     byte ptr [edi+SRQ_DIR],SRD_OUT
        jne     sr1_nobounce
        push    edi
        mov     esi,ebp
        mov     edi,SCSI_BUF
        cld
        rep     movsb                   ; what the target is to receive
        pop     edi
sr1_nobounce:
        ; a small reply's room is cleared, wherever it now is
        cmp     byte ptr [edi+SRQ_DIR],SRD_IN
        jne     sr1_go
        mov     ecx,[edi+SRQ_COUNT]
        cmp     ecx,SCSI_ZEROMAX
        ja      sr1_go
        push    edi
        mov     edi,[edi+SRQ_BUF]
        xor     eax,eax
        cld
        rep     stosb
        pop     edi
sr1_go:
        call    dword ptr [ebx+HBA_EXEC] ; EBX -> adapter, EDI -> request
        pushfd
        mov     [edi+SRQ_BUF],ebp       ; the caller's address back

        ; ...and the caller's data, if it went round
        test    byte ptr [ebx+HBA_FLAGS],SHF_BOUNCE
        jz      sr1_back
        cmp     byte ptr [edi+SRQ_DIR],SRD_IN
        jne     sr1_back
        mov     ecx,[edi+SRQ_GOT]
        cmp     ecx,[edi+SRQ_COUNT]
        jbe     sr1_copy
        mov     ecx,[edi+SRQ_COUNT]
sr1_copy:
        push    edi
        mov     esi,SCSI_BUF
        mov     edi,ebp
        cld
        rep     movsb
        pop     edi
sr1_back:
        popfd
        jc      sr1_fail
        popad
        clc
        ret
sr1_fail:
        popad
        stc
        ret

;=====================================================================
; scs_cdexec - the SCSI transport for CDUNIT.INC: run the command in
;              cun_cdb against the unit in ESI.
;
;   EDI = buffer, ECX = its size (zero for a command with no data)
;   -> EAX = bytes moved, CF on failure with cun_skey/asc/ascq set
;
; THREE THINGS AN ATAPI DRIVE NEVER NEEDED, and all of them are here
; rather than in the callers, so that MSCD001 and the volume layer
; build one block for both kinds of drive:
;
;   THE LENGTH OF THE BLOCK.  ATAPI always takes twelve bytes; a SCSI
;   target takes the length its opcode's group says (scs_cdblen).
;
;   A READ BIGGER THAN THE ADAPTER TAKES AT ONCE is cut into pieces:
;   READ(10), READ(12) and READ CD carry a block address and a count,
;   so the pieces are plain to build (sce_read).
;
;   MODE SENSE(10) AND MODE SELECT(10) ON A DRIVE THAT HAS ONLY THE
;   6-BYTE FORMS - SCSI-1 drives, and plenty of SCSI-2 ones - are
;   asked in the short form and the header turned round, after the
;   first refusal and for good (SDF_MODE6, sce_msense).
;
; The caller's block is put back as it was whatever happens here,
; because cun_svcpkt retries a unit attention with it.
;=====================================================================
scs_cdexec:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        mov     esi,[esi+CUE_DEV]       ; the SDV row
        mov     [sce_dev],esi
        mov     [sce_buf],edi
        mov     [sce_len],ecx
        mov     byte ptr [cun_skey],0
        mov     byte ptr [cun_asc],0
        mov     byte ptr [cun_ascq],0
        push    esi
        push    edi
        push    ecx
        mov     esi,OFFSET cun_cdb
        mov     edi,OFFSET sce_save
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb
        mov     al,byte ptr [cun_dir]
        mov     byte ptr [sce_dsave],al
        pop     ecx
        pop     edi
        pop     esi

        mov     al,byte ptr [cun_cdb]
        cmp     al,SCSI_MODESNS10
        je      sce_msense
        cmp     al,SCSI_MODESEL10
        je      sce_msel
        cmp     al,SCSI_READ10
        je      sce_read
        cmp     al,SCSI_READ12
        je      sce_read
        cmp     al,SCSI_READCD
        je      sce_read
sce_plain:
        mov     edi,[sce_buf]
        mov     ecx,[sce_len]
        call    sce_one
sce_out:
        pushfd
        push    eax
        mov     esi,OFFSET sce_save     ; the caller's block, as it was
        mov     edi,OFFSET cun_cdb
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb
        mov     al,byte ptr [sce_dsave]
        mov     byte ptr [cun_dir],al
        pop     eax
        popfd
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- A READ: in one piece if the adapter takes that much, else cut ----
sce_read:
        mov     esi,[sce_dev]
        movzx   ebx,byte ptr [esi+SDV_HBA]
        imul    ebx,ebx,HBA_LEN
        mov     ebx,dword ptr [scs_hbas+ebx+HBA_MAXX]     ; EBX = most per request
        mov     ecx,[sce_len]
        cmp     ecx,ebx
        jbe     sce_plain

        ; how many blocks, where the count sits in this opcode
        movzx   eax,byte ptr [sce_save+7]       ; READ(10): bytes 7-8
        shl     eax,8
        mov     al,byte ptr [sce_save+8]
        cmp     byte ptr [sce_save],SCSI_READ10
        je      sre_count
        movzx   eax,byte ptr [sce_save+6]       ; READ CD: bytes 6-8
        shl     eax,8
        mov     al,byte ptr [sce_save+7]
        shl     eax,8
        mov     al,byte ptr [sce_save+8]
        cmp     byte ptr [sce_save],SCSI_READCD
        je      sre_count
        shl     eax,8                           ; READ(12): bytes 6-9
        mov     al,byte ptr [sce_save+9]
sre_count:
        or      eax,eax
        jz      sce_plain
        mov     [sre_left],eax
        ; ...and so how big each one is, which has to come out even
        mov     ebp,eax
        mov     eax,[sce_len]
        xor     edx,edx
        div     ebp
        or      edx,edx
        jnz     sce_plain               ; it does not: one request, refused
        or      eax,eax
        jz      sce_plain
        mov     [sre_size],eax
        mov     ecx,eax
        mov     eax,ebx
        xor     edx,edx
        div     ecx                     ; EAX = blocks per request
        or      eax,eax
        jz      sce_plain
        mov     [sre_per],eax

        movzx   eax,byte ptr [sce_save+2]       ; the first block, 2-5
        shl     eax,8
        mov     al,byte ptr [sce_save+3]
        shl     eax,8
        mov     al,byte ptr [sce_save+4]
        shl     eax,8
        mov     al,byte ptr [sce_save+5]
        mov     [sre_lba],eax
        mov     eax,[sce_buf]
        mov     [sre_at],eax
        mov     dword ptr [sre_total],0

sre_piece:
        mov     ecx,[sre_per]
        cmp     ecx,[sre_left]
        jbe     sre_have
        mov     ecx,[sre_left]
sre_have:
        mov     [sre_n],ecx
        mov     eax,[sre_lba]
        mov     byte ptr [cun_cdb+5],al
        shr     eax,8
        mov     byte ptr [cun_cdb+4],al
        shr     eax,8
        mov     byte ptr [cun_cdb+3],al
        shr     eax,8
        mov     byte ptr [cun_cdb+2],al
        mov     eax,ecx
        cmp     byte ptr [sce_save],SCSI_READ10
        jne     sre_long
        mov     byte ptr [cun_cdb+8],al
        mov     byte ptr [cun_cdb+7],ah
        jmp     short sre_go
sre_long:
        cmp     byte ptr [sce_save],SCSI_READCD
        jne     sre_r12
        mov     byte ptr [cun_cdb+8],al
        mov     byte ptr [cun_cdb+7],ah
        shr     eax,16
        mov     byte ptr [cun_cdb+6],al
        jmp     short sre_go
sre_r12:
        mov     byte ptr [cun_cdb+9],al
        mov     byte ptr [cun_cdb+8],ah
        shr     eax,16
        mov     byte ptr [cun_cdb+7],al
        mov     byte ptr [cun_cdb+6],ah
sre_go:
        mov     ecx,[sre_n]
        imul    ecx,[sre_size]
        mov     edi,[sre_at]
        call    sce_one
        jc      sre_fail
        mov     eax,[sre_n]
        add     [sre_lba],eax
        sub     [sre_left],eax
        imul    eax,[sre_size]
        add     [sre_at],eax
        add     [sre_total],eax
        cmp     dword ptr [sre_left],0
        jne     sre_piece
        mov     eax,[sre_total]
        clc
        jmp     sce_out
sre_fail:
        mov     eax,[sre_total]
        stc
        jmp     sce_out

;--- MODE SENSE(10) -------------------------------------------------
sce_msense:
        mov     esi,[sce_dev]
        test    byte ptr [esi+SDV_FLAGS],SDF_MODE6
        jnz     sms_short
        mov     edi,[sce_buf]
        mov     ecx,[sce_len]
        call    sce_one
        jnc     sce_out
        cmp     byte ptr [cun_skey],5   ; ILLEGAL REQUEST: not this form
        jne     sms_fail
        call    sms_six
        jc      sce_out
        mov     esi,[sce_dev]
        or      byte ptr [esi+SDV_FLAGS],SDF_MODE6
        clc
        jmp     sce_out
sms_short:
        call    sms_six
        jmp     sce_out
sms_fail:
        stc
        jmp     sce_out

;---------------------------------------------------------------------
; sms_six - the MODE SENSE in sce_save, asked as MODE SENSE(6), the
;           reply given the 10-byte form's header.  -> EAX = bytes in
;           the caller's buffer, CF.
;
;   6-byte header:  length (1), medium type, device specific, block
;                   descriptor length (1)
;   10-byte header: length (2), medium type, device specific, two
;                   reserved, block descriptor length (2)
;
; The length field counts the bytes after itself, so one header's is
; the other's plus three.
;---------------------------------------------------------------------
sms_six:
        movzx   ecx,byte ptr [sce_save+7]       ; what the caller has room
        shl     ecx,8                           ; for, less the four bytes
        mov     cl,byte ptr [sce_save+8]        ; the long header adds
        sub     ecx,4
        jbe     sxs_bad
        cmp     ecx,SCSI_MODEMAX
        jbe     sxs_alloc
        mov     ecx,SCSI_MODEMAX
sxs_alloc:
        call    cun_cdbzero
        mov     byte ptr [cun_cdb+0],SCSI_MODESNS6
        mov     al,byte ptr [sce_save+1]
        mov     byte ptr [cun_cdb+1],al         ; DBD, as asked
        mov     al,byte ptr [sce_save+2]
        mov     byte ptr [cun_cdb+2],al         ; which page, which values
        mov     byte ptr [cun_cdb+4],cl
        mov     edi,OFFSET scs_mbuf
        call    sce_one
        jc      sxs_out
        cmp     eax,4
        jb      sxs_bad
        movzx   ecx,byte ptr [scs_mbuf]         ; the bytes after byte 0
        inc     ecx                             ; -> the whole reply
        cmp     ecx,eax
        jbe     sxs_whole
        mov     ecx,eax                         ; ...as far as it came
sxs_whole:
        ; build the long form in scs_mout.  ECX is the short reply's
        ; whole size; the long one is four bytes more, and its length
        ; field leaves out its own two
        lea     eax,[ecx+2]
        mov     byte ptr [scs_mout+0],ah
        mov     byte ptr [scs_mout+1],al
        mov     al,byte ptr [scs_mbuf+1]
        mov     byte ptr [scs_mout+2],al
        mov     al,byte ptr [scs_mbuf+2]
        mov     byte ptr [scs_mout+3],al
        mov     byte ptr [scs_mout+4],0
        mov     byte ptr [scs_mout+5],0
        mov     byte ptr [scs_mout+6],0
        mov     al,byte ptr [scs_mbuf+3]
        mov     byte ptr [scs_mout+7],al
        push    ecx
        sub     ecx,4
        mov     esi,OFFSET scs_mbuf + 4
        mov     edi,OFFSET scs_mout + 8
        cld
        rep     movsb
        pop     ecx
        add     ecx,4                           ; the long reply's size
        cmp     ecx,[sce_len]
        jbe     sxs_fits
        mov     ecx,[sce_len]
sxs_fits:
        mov     eax,ecx
        mov     esi,OFFSET scs_mout
        mov     edi,[sce_buf]
        cld
        rep     movsb
        clc
sxs_out:
        ret
sxs_bad:
        stc
        ret

;--- MODE SELECT(10) ------------------------------------------------
sce_msel:
        mov     esi,[sce_dev]
        test    byte ptr [esi+SDV_FLAGS],SDF_MODE6
        jnz     sml_short
        mov     edi,[sce_buf]
        mov     ecx,[sce_len]
        call    sce_one
        jnc     sce_out
        cmp     byte ptr [cun_skey],5
        jne     sms_fail
        call    sml_six
        jc      sce_out
        mov     esi,[sce_dev]
        or      byte ptr [esi+SDV_FLAGS],SDF_MODE6
        clc
        jmp     sce_out
sml_short:
        call    sml_six
        jmp     sce_out

;---------------------------------------------------------------------
; sml_six - the MODE SELECT in sce_save, sent as MODE SELECT(6) with
;           the parameter list's header cut from eight bytes to four.
;           -> EAX = the bytes the caller handed over, CF.
;---------------------------------------------------------------------
sml_six:
        mov     ecx,[sce_len]
        cmp     ecx,8
        jb      sxs_bad
        sub     ecx,4
        cmp     ecx,SCSI_MODEMAX
        ja      sxs_bad
        mov     esi,[sce_buf]
        mov     byte ptr [scs_mbuf+0],0         ; reserved on the way out
        mov     al,[esi+2]
        mov     byte ptr [scs_mbuf+1],al        ; medium type
        mov     al,[esi+3]
        mov     byte ptr [scs_mbuf+2],al        ; device specific
        mov     al,[esi+7]
        mov     byte ptr [scs_mbuf+3],al        ; block descriptor length
        push    ecx
        sub     ecx,4
        add     esi,8
        mov     edi,OFFSET scs_mbuf + 4
        cld
        rep     movsb
        pop     ecx
        call    cun_cdbzero
        mov     byte ptr [cun_cdb+0],SCSI_MODESEL6
        mov     al,byte ptr [sce_save+1]
        mov     byte ptr [cun_cdb+1],al         ; PF and SP, as asked
        mov     byte ptr [cun_cdb+4],cl
        mov     byte ptr [cun_dir],1
        mov     edi,OFFSET scs_mbuf
        call    sce_one
        jc      sxs_out
        mov     eax,[sce_len]
        clc
        ret

;---------------------------------------------------------------------
; sce_one - one request: cun_cdb and cun_dir as they stand, EDI and
;           ECX the data, to the device in [sce_dev].  -> EAX = bytes
;           moved, CF with cun_skey/asc/ascq.
;---------------------------------------------------------------------
sce_one:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     esi,[sce_dev]
        mov     edx,edi                 ; the data's address
        mov     edi,OFFSET scs_csrq
        call    scs_srqzero
        mov     al,[esi+SDV_TARGET]
        mov     [edi+SRQ_TARGET],al
        mov     al,[esi+SDV_LUN]
        mov     [edi+SRQ_LUN],al
        mov     [edi+SRQ_BUF],edx
        mov     [edi+SRQ_COUNT],ecx
        mov     al,byte ptr [cun_dir]   ; 0 receives, 1 sends: SRD_IN and
        mov     [edi+SRQ_DIR],al        ; SRD_OUT have the same values
        push    esi
        push    edi
        mov     esi,OFFSET cun_cdb
        add     edi,SRQ_CDB
        mov     ecx,ATAPI_PKTLEN
        cld
        rep     movsb
        pop     edi
        pop     esi
        mov     al,byte ptr [cun_cdb]
        call    scs_cdblen
        mov     [edi+SRQ_CDBLEN],al
        movzx   ebx,byte ptr [esi+SDV_HBA]
        call    scs_run
        pushfd
        ; what the drive said, in the form the unit layer keeps it
        cmp     byte ptr [edi+SRQ_SLEN],14
        jb      s1_nosense
        mov     al,byte ptr [edi+SRQ_SENSE+2]
        and     al,0Fh
        mov     byte ptr [cun_skey],al
        mov     al,byte ptr [edi+SRQ_SENSE+12]
        mov     byte ptr [cun_asc],al
        mov     al,byte ptr [edi+SRQ_SENSE+13]
        mov     byte ptr [cun_ascq],al
s1_nosense:
        mov     eax,[edi+SRQ_GOT]
        popfd
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; scs_cdblen - AL = an opcode -> AL = how long its command block is.
;
; The group, in the top three bits, says: 0 is six bytes, 1 and 2 are
; ten, 5 is twelve.  4 is sixteen, which nothing here sends and the
; block cannot hold; 3, 6 and 7 are reserved or the vendor's, and ten
; is what the drives that use them mostly take.
;---------------------------------------------------------------------
scs_cdblen:
        shr     al,5
        or      al,al
        jz      scl_six
        cmp     al,5
        je      scl_twelve
        cmp     al,4
        je      scl_twelve
        mov     al,10
        ret
scl_six:
        mov     al,6
        ret
scl_twelve:
        mov     al,12
        ret

;=====================================================================
; Data
;=====================================================================
scs_nhba    dd 0                        ; rows in scs_hbas
scs_ndev    dd 0                        ; rows in scs_devs
scs_nnative dd 0                        ; how many of the adapters are
                                        ; PMIO's own (they come first)
sce_dev     dd 0                        ; the CD request in hand
sce_buf     dd 0
sce_len     dd 0
sre_left    dd 0                        ; a read being cut up: blocks to go,
sre_size    dd 0                        ; the size of one,
sre_per     dd 0                        ; how many fit in a request,
sre_lba     dd 0                        ; where the next piece starts,
sre_at      dd 0                        ; and where it goes
sre_n       dd 0
sre_total   dd 0
scs_probed  db 0                        ; SSF_PROBE has run
scs_pflags  db 0                        ; ...with these SPF_* bits
sce_dsave   db 0                        ; the caller's cun_dir
sce_save    db ATAPI_PKTLEN dup (0)     ; ...and its command block

scs_hbas    db SCSI_MAXHBA * HBA_LEN dup (0)
scs_devs    db SCSI_MAXDEV * SDV_LEN dup (0)
scs_csrq    db SRQ_SIZE dup (0)         ; a CD unit's request
scs_isrq    db SRQ_SIZE dup (0)         ; the bus scan's
scs_ssrq    db SRQ_SIZE dup (0)         ; REQUEST SENSE's, inside scs_run
scs_inqbuf  db SCSI_INQLEN dup (0)
scs_sbuf    db 20 dup (0)
scs_mbuf    db SCSI_MODEMAX + 1 dup (0) ; a 6-byte mode reply, or list
scs_mout    db SCSI_MODEMAX + 8 dup (0) ; ...turned into the 10-byte one
