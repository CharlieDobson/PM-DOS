;=====================================================================
; ASPI.INC - SCSI through a loaded ASPI manager, and the SCSI probe
;
; PMIO drives two kinds of SCSI card itself (AHA154X.INC, NCR5380.INC).
; Every other card - an Adaptec AHA-2940, the AIC-6360 on a Sound
; Blaster 16 SCSI-2, a parallel-port adapter - came with a DOS driver
; of its own, an ASPI MANAGER, loaded from CONFIG.SYS with DEVICE=.
; Each one presents the same interface, Adaptec's ASPI: a character
; device called SCSIMGR$ whose IOCTL read hands back a far entry
; point, and SCSI Request Blocks (SRBs) passed to that entry.  So one
; transport here reaches all of them, without a line of code for any
; particular card.
;
; THE CALL IS AN EXCURSION, like a driver call (DEVLOAD.INC): a stub
; below 1MB pushes the SRB's address and far-calls the manager, in V86
; mode, and comes home to the pad's HLT.  The manager starts the
; command and usually returns before it has finished; the SRB's status
; byte changes when its interrupt handler sees the card complete.
;
; AND THE WAITING IS AN EXCURSION TOO, which is the part that is not
; obvious.  The manager's interrupt handler is real-mode code in the
; vector table, and PMIO delivers a card's interrupt to it only while
; V86 code is running - an interrupt that arrives while the kernel sits
; in protected mode waits for the next program (VPIC.INC).  So the
; status byte is watched by a second stub, a loop in V86 with
; interrupts on, run again and again until the byte changes or the
; time is up.  The manager's interrupt is delivered into that loop
; exactly as it would be into a DOS program's.
;
; WHAT THE MANAGER NEEDS FROM THE MACHINE, and gets:
;
;   ITS INTERRUPT on one of the lines a box may have (VP_GUEST: 3-5, 7
;   and 9-11).  A card set to IRQ 12, 14 or 15 starts commands that
;   never finish, and every request here times out.
;
;   VIRTUAL DMA SERVICES, because a bus-master card has to be told the
;   physical address of the buffer.  dl_excur passes PMIO's INT 4Bh
;   stub through, as a box's run loop does.
;
;   ITS DATA BELOW 1MB, in memory a real-mode program can write: the
;   SRB and an ASPI_BUFSZ buffer, taken from conventional memory once,
;   only when a manager is loaded.  Requests are copied through it.
;
; THE PROBE, scsi_start, lives here too because it has to know about
; the manager before PMIO looks at any card: a card the manager already
; owns must not be reset (SPF_ASPI, see AHA154X.INC).
;
; Included by DOSMAIN.ASM, after DEVLOAD.INC.
;=====================================================================

ASPI_BUFSZ      EQU     8192            ; data per request, below 1MB
ASPI_TICKS      EQU     546             ; 30 seconds for one request
ASPI_SPIN       EQU     8000h           ; status reads per trip into V86

; the conventional block, by offset
ASB_CALL        EQU     0000h           ; stub: call the manager
ASB_WAIT        EQU     0010h           ; stub: watch the status byte
ASB_ENTRY       EQU     0020h           ; dd - the manager's entry point
ASB_SPIN        EQU     0024h           ; dw - how long the watch lasts
ASB_SRB         EQU     0040h           ; the SRB
ASB_SRBMAX      EQU     0080h
ASB_DATA        EQU     0100h           ; the data
ASB_SIZE        EQU     ASB_DATA + ASPI_BUFSZ
ASB_PARAS       EQU     (ASB_SIZE + 15) / 16

; an SRB, as ASPI for DOS lays it out
SRB_CMD         EQU     00h             ; db - SRBC_*
SRB_STATUS      EQU     01h             ; db - SRBS_*: 0 until finished
SRB_HA          EQU     02h             ; db - which host adapter
SRB_FLAGS       EQU     03h             ; db - SRBF_*
; ...host adapter inquiry
SRBI_COUNT      EQU     08h             ; db - how many adapters
SRBI_MYID       EQU     09h             ; db - its own SCSI ID
SRBI_IDENT      EQU     1Ah             ; 16 bytes - what it is
; ...get device type
SRBT_TARGET     EQU     08h
SRBT_LUN        EQU     09h
SRBT_TYPE       EQU     0Ah
; ...execute a SCSI request
SRBX_TARGET     EQU     08h
SRBX_LUN        EQU     09h
SRBX_BUFLEN     EQU     0Ah             ; dd
SRBX_SENSELEN   EQU     0Eh             ; db
SRBX_BUFPTR     EQU     0Fh             ; dd - segment:offset
SRBX_CDBLEN     EQU     17h             ; db
SRBX_HASTAT     EQU     18h             ; db - out
SRBX_TSTAT      EQU     19h             ; db - out
SRBX_CDB        EQU     40h             ; the command block, then the sense

SRBC_INQUIRY    EQU     0               ; host adapter inquiry
SRBC_DEVTYPE    EQU     1
SRBC_EXEC       EQU     2

SRBF_IN         EQU     08h             ; the target sends
SRBF_OUT        EQU     10h             ; the target receives
SRBF_NONE       EQU     18h

SRBS_DONE       EQU     01h
SRBS_ERROR      EQU     04h
SRBS_NODEV      EQU     82h

ASH_SELTO       EQU     11h             ; host status: nothing answered
ASH_UNDER       EQU     12h             ; ...a short reply

;=====================================================================
; scsi_start - after CONFIG.SYS: the manager, if one was loaded, then
;              PMIO's own probe, then the manager's adapters, and the
;              CD-ROM letters for whatever turned up.
;=====================================================================
scsi_start:
        pushad
        call    aspi_find               ; sets aspi_entry, or leaves it 0
        xor     al,al
        cmp     byte ptr [scsi_on],0
        je      sst_flags
        or      al,SPF_NATIVE
sst_flags:
        cmp     dword ptr [aspi_entry],0
        je      sst_probe
        or      al,SPF_ASPI
sst_probe:
        mov     ah,SSF_PROBE
        call    dword ptr ds:[IOS_SCSI]
        cmp     dword ptr [aspi_entry],0
        je      sst_units
        call    aspi_scan
sst_units:
        mov     ah,SSF_BUILD
        call    dword ptr ds:[IOS_SCSI]         ; -> ECX = units
        cmp     cl,[cd_units]
        je      sst_out                         ; nothing new to name
        call    drv_addcds
        cmp     dword ptr [cdd_seg],0
        jne     sst_out
        call    cdd_init                        ; the first CD-ROM there is
sst_out:
        popad
        ret

;=====================================================================
; aspi_find - is SCSIMGR$ loaded?  If it is, ask it for its entry
;             point and take the conventional block.  aspi_entry is
;             left 0 otherwise.  All registers kept.
;
; THE ENTRY POINT IS AN IOCTL READ of four bytes, which a DOS program
; does with 3D00h and 4402h; the kernel goes straight to the driver
; with the same request (DEVC_IOCTLIN), through the packet DEVLOAD
; keeps for its calls.
;=====================================================================
aspi_find:
        pushad
        mov     dword ptr [aspi_entry],0
        cmp     dword ptr [dl_seg],0
        je      afd_out                 ; no DEVICE= line ran at all
        mov     esi,OFFSET aspi_name
        mov     edi,OFFSET dev_name
        mov     ecx,8
        cld
        rep     movsb
        call    dl_lookup               ; -> EAX = device id
        jc      afd_out
        sub     eax,DEV_LOADED
        xor     edx,edx
        mov     ecx,DEV_UNIT
        div     ecx                     ; -> the row in dl_tab
        mov     ecx,DEVC_IOCTLIN
        call    dl_prep                 ; -> EDI = the packet
        jc      afd_out
        mov     eax,[dl_pktoff]
        add     eax,DL_PKTLEN - 8       ; the four bytes, inside the packet
        mov     word ptr [edi+REQC_BUFOFF],ax
        mov     eax,[dl_seg]
        mov     word ptr [edi+REQC_BUFSEG],ax
        mov     word ptr [edi+REQC_COUNT],4
        call    dl_fire
        jc      afd_out
        mov     edi,[dl_pktflat]
        mov     eax,[edi+DL_PKTLEN-8]
        or      eax,eax
        jz      afd_out

        mov     [aspi_entry],eax
        mov     ebx,ASB_PARAS
        call    lm_alloc
        jc      afd_none
        movzx   eax,ax
        mov     [aspi_seg],eax
        mov     dx,8                    ; owned by DOS itself, like dl_seg:
        call    lm_setown               ; the exit sweep never takes it
        mov     edi,[aspi_seg]
        shl     edi,4
        push    edi
        xor     eax,eax
        mov     ecx,ASB_DATA / 4
        rep     stosd
        pop     edi
        mov     esi,OFFSET aspi_stubs
        mov     ecx,ASB_ENTRY
        rep     movsb                   ; the two stubs, at 0000 and 0010
        mov     eax,[aspi_entry]
        mov     [edi+ASB_ENTRY-ASB_ENTRY],eax
        mov     word ptr [edi+ASB_SPIN-ASB_ENTRY],ASPI_SPIN
        jmp     short afd_out
afd_none:
        mov     dword ptr [aspi_entry],0        ; no room: as if no manager
afd_out:
        popad
        ret

;---------------------------------------------------------------------
; THE TWO STUBS, 16-bit code assembled by hand because this file is a
; 32-bit one.  DS is the block's segment when either runs (dl_excur
; makes CS and DS the same) and both end in RETF, back to DEVLOAD's pad.
;
;   0000  push ds                   1E
;         push ASB_SRB              68 40 00
;         call far [ASB_ENTRY]      FF 1E 20 00
;         add  sp,4                 83 C4 04
;         retf                      CB
;
;   0010  sti                       FB
;         mov  cx,[ASB_SPIN]        8B 0E 24 00
;   0015  cmp  byte ptr [ASB_SRB+SRB_STATUS],0     80 3E 41 00 00
;         jne  001E                 75 02
;         loop 0015                 E2 F7
;   001E  retf                      CB
;---------------------------------------------------------------------
aspi_stubs:
        db      1Eh, 68h, 40h, 00h, 0FFh, 1Eh, 20h, 00h
        db      83h, 0C4h, 04h, 0CBh, 90h, 90h, 90h, 90h
        db      0FBh, 8Bh, 0Eh, 24h, 00h, 80h, 3Eh, 41h
        db      00h, 00h, 75h, 02h, 0E2h, 0F7h, 0CBh, 90h
.ERRNZ  ($ - aspi_stubs) - ASB_ENTRY, <the stubs no longer fill the block's first 32 bytes>

;=====================================================================
; aspi_scan - the manager's adapters and their devices, given to PMIO.
;             All registers kept.
;=====================================================================
aspi_scan:
        pushad
        mov     edi,OFFSET aspi_tmpl
        mov     byte ptr [asc_ha],0
asc_ha_lp:
        ; what is this adapter, and is there one at all
        call    aspi_srbzero            ; -> ESI = the SRB
        mov     byte ptr [esi+SRB_CMD],SRBC_INQUIRY
        mov     al,byte ptr [asc_ha]
        mov     [esi+SRB_HA],al
        call    aspi_call
        jc      asc_out
        cmp     byte ptr [esi+SRB_STATUS],SRBS_DONE
        jne     asc_out
        cmp     byte ptr [asc_ha],0
        jne     asc_have
        movzx   eax,byte ptr [esi+SRBI_COUNT]
        mov     [asc_count],eax
asc_have:
        movzx   eax,byte ptr [asc_ha]
        cmp     eax,[asc_count]
        jae     asc_out

        push    edi
        xor     eax,eax
        mov     ecx,HBA_LEN
        cld
        rep     stosb
        pop     edi
        mov     dword ptr [edi+HBA_EXEC],OFFSET aspi_exec
        mov     byte ptr [edi+HBA_KIND],SHK_ASPI
        mov     al,[esi+SRBI_MYID]
        and     al,0Fh
        mov     [edi+HBA_ID],al
        mov     al,byte ptr [asc_ha]
        mov     [edi+HBA_NUM],al
        mov     byte ptr [edi+HBA_IRQ],0FFh
        mov     byte ptr [edi+HBA_DMA],0FFh
        mov     dword ptr [edi+HBA_MAXX],ASPI_BUFSZ
        push    esi
        push    edi
        add     esi,SRBI_IDENT
        add     edi,HBA_NAME
        mov     ecx,16
asc_name:
        lodsb
        cmp     al,' '
        jae     asc_nchar
        mov     al,' '
asc_nchar:
        stosb
        loop    asc_name
        pop     edi
        pop     esi
        mov     ah,SSF_ADDHBA
        call    dword ptr ds:[IOS_SCSI]         ; -> EAX = PMIO's number
        jc      asc_out
        mov     [asc_pmio],eax

        ; every target on it, LUN 0, but its own
        mov     byte ptr [asc_tgt],0
asc_tgt_lp:
        mov     al,byte ptr [asc_tgt]
        cmp     al,8
        jae     asc_next
        cmp     al,[edi+HBA_ID]
        je      asc_tgt_next
        call    aspi_srbzero
        mov     byte ptr [esi+SRB_CMD],SRBC_DEVTYPE
        mov     al,byte ptr [asc_ha]
        mov     [esi+SRB_HA],al
        mov     al,byte ptr [asc_tgt]
        mov     [esi+SRBT_TARGET],al
        call    aspi_call
        jc      asc_tgt_next
        cmp     byte ptr [esi+SRB_STATUS],SRBS_DONE
        jne     asc_tgt_next            ; 82h: nothing there
        call    aspi_inquiry            ; -> scs-style 36 bytes at asc_inq
        jc      asc_tgt_next
        mov     eax,[asc_pmio]
        mov     dl,byte ptr [asc_tgt]
        xor     dh,dh
        push    esi
        mov     esi,OFFSET asc_inq
        mov     ah,SSF_ADDDEV
        call    dword ptr ds:[IOS_SCSI]
        pop     esi
        jc      asc_out                 ; PMIO's table is full
asc_tgt_next:
        inc     byte ptr [asc_tgt]
        jmp     asc_tgt_lp
asc_next:
        inc     byte ptr [asc_ha]
        jmp     asc_ha_lp
asc_out:
        popad
        ret

;---------------------------------------------------------------------
; aspi_inquiry - INQUIRY to [asc_ha], target [asc_tgt], LUN 0, through
;                the manager, into asc_inq.  CF if it would not say.
;---------------------------------------------------------------------
aspi_inquiry:
        pushad
        mov     edi,OFFSET asc_srq
        xor     eax,eax
        mov     ecx,SRQ_SIZE
        cld
        push    edi
        rep     stosb
        pop     edi
        mov     al,byte ptr [asc_tgt]
        mov     [edi+SRQ_TARGET],al
        mov     byte ptr [edi+SRQ_CDBLEN],6
        mov     byte ptr [edi+SRQ_DIR],SRD_IN
        mov     dword ptr [edi+SRQ_BUF],OFFSET asc_inq
        mov     dword ptr [edi+SRQ_COUNT],36
        mov     byte ptr [edi+SRQ_CDB+0],12h    ; INQUIRY
        mov     byte ptr [edi+SRQ_CDB+4],36
        mov     ebx,OFFSET aspi_tmpl            ; HBA_NUM is asc_ha's
        call    aspi_exec
        popad
        ret

;---------------------------------------------------------------------
; aspi_srbzero - blank the SRB.  -> ESI = its flat address.  All else
;                kept.
;---------------------------------------------------------------------
aspi_srbzero:
        push    eax
        push    ecx
        push    edi
        mov     esi,[aspi_seg]
        shl     esi,4
        add     esi,ASB_SRB
        mov     edi,esi
        xor     eax,eax
        mov     ecx,ASB_SRBMAX / 4
        cld
        rep     stosd
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; aspi_exec - HBA_EXEC for an adapter the manager drives.  EBX -> the
;             adapter's row, EDI -> a request.  CF unless the target
;             said GOOD.  All registers kept.  PMIO calls this, from
;             scs_run, with the kernel's flat selectors loaded.
;=====================================================================
aspi_exec:
        pushad
        mov     byte ptr [edi+SRQ_HSTAT],SRH_ERROR
        mov     ecx,[edi+SRQ_COUNT]
        cmp     ecx,ASPI_BUFSZ
        ja      axe_fail
        call    aspi_srbzero            ; -> ESI = the SRB
        mov     byte ptr [esi+SRB_CMD],SRBC_EXEC
        mov     al,[ebx+HBA_NUM]
        mov     [esi+SRB_HA],al
        mov     al,SRBF_NONE
        or      ecx,ecx
        jz      axe_dir
        mov     al,SRBF_IN
        cmp     byte ptr [edi+SRQ_DIR],SRD_IN
        je      axe_dir
        mov     al,SRBF_OUT
axe_dir:
        mov     [esi+SRB_FLAGS],al
        mov     al,[edi+SRQ_TARGET]
        mov     [esi+SRBX_TARGET],al
        mov     al,[edi+SRQ_LUN]
        mov     [esi+SRBX_LUN],al
        mov     [esi+SRBX_BUFLEN],ecx
        mov     byte ptr [esi+SRBX_SENSELEN],18
        mov     eax,[aspi_seg]
        shl     eax,16
        mov     ax,ASB_DATA
        mov     [esi+SRBX_BUFPTR],eax
        movzx   ecx,byte ptr [edi+SRQ_CDBLEN]
        mov     [esi+SRBX_CDBLEN],cl
        push    esi
        push    edi
        lea     eax,[esi+SRBX_CDB]
        lea     esi,[edi+SRQ_CDB]
        mov     edi,eax
        cld
        rep     movsb
        pop     edi
        pop     esi

        ; the data, below 1MB: what is to be sent, or zeros for what
        ; the target may not fill
        mov     ecx,[edi+SRQ_COUNT]
        jecxz   axe_go
        push    esi
        push    edi
        mov     edx,[aspi_seg]
        shl     edx,4
        add     edx,ASB_DATA
        cmp     byte ptr [edi+SRQ_DIR],SRD_OUT
        je      axe_out
        mov     edi,edx
        xor     eax,eax
        rep     stosb
        jmp     short axe_ready
axe_out:
        mov     esi,[edi+SRQ_BUF]
        mov     edi,edx
        rep     movsb
axe_ready:
        pop     edi
        pop     esi
axe_go:
        call    aspi_call
        jc      axe_hung

        ; what came back
        mov     al,[esi+SRBX_TSTAT]
        mov     [edi+SRQ_TSTAT],al
        mov     al,[esi+SRB_STATUS]
        cmp     al,SRBS_NODEV
        je      axe_nodev
        cmp     al,SRBS_DONE            ; done, or done with an error the
        je      axe_stats               ; two status bytes explain; anything
        cmp     al,SRBS_ERROR           ; else - aborted, or a request the
        jne     axe_fail                ; manager would not take - failed
axe_stats:
        mov     al,[esi+SRBX_HASTAT]
        cmp     al,ASH_SELTO
        je      axe_nodev
        or      al,al
        jz      axe_hok
        cmp     al,ASH_UNDER
        jne     axe_fail
axe_hok:
        mov     byte ptr [edi+SRQ_HSTAT],SRH_OK
        cmp     byte ptr [edi+SRQ_TSTAT],SCSI_ST_CHECK
        jne     axe_nosense
        push    esi
        push    edi
        movzx   eax,byte ptr [esi+SRBX_CDBLEN]
        lea     esi,[esi+SRBX_CDB+eax]
        add     edi,SRQ_SENSE
        mov     ecx,18
        cld
        rep     movsb
        pop     edi
        pop     esi
        mov     byte ptr [edi+SRQ_SLEN],18
axe_nosense:
        cmp     byte ptr [edi+SRQ_TSTAT],SCSI_ST_GOOD
        jne     axe_fail
        ; the data back up to where it was wanted
        mov     ecx,[edi+SRQ_COUNT]
        mov     [edi+SRQ_GOT],ecx
        cmp     byte ptr [edi+SRQ_DIR],SRD_IN
        jne     axe_ok
        jecxz   axe_ok
        push    edi
        mov     esi,[aspi_seg]
        shl     esi,4
        add     esi,ASB_DATA
        mov     edi,[edi+SRQ_BUF]
        cld
        rep     movsb
        pop     edi
axe_ok:
        popad
        clc
        ret
axe_nodev:
        mov     byte ptr [edi+SRQ_HSTAT],SRH_NODEV
        jmp     short axe_fail
axe_hung:
        mov     byte ptr [edi+SRQ_HSTAT],SRH_TIMEOUT
axe_fail:
        popad
        stc
        ret

;=====================================================================
; aspi_call - hand the SRB to the manager and wait for it to finish.
;             CF if it never does, or the excursion went wrong.  All
;             registers kept.
;=====================================================================
aspi_call:
        pushad
        mov     esi,[aspi_seg]
        shl     esi,4
        add     esi,ASB_SRB
        mov     eax,ASB_CALL
        call    aspi_run
        jc      acl_bad
        call    dword ptr ds:[IOS_TICKS]
        mov     [acl_start],eax
acl_wait:
        cmp     byte ptr [esi+SRB_STATUS],0
        jne     acl_ok
        call    dword ptr ds:[IOS_TICKS]
        cmp     eax,[acl_start]
        jae     acl_since
        mov     [acl_start],eax         ; midnight: the count went back to 0
acl_since:
        sub     eax,[acl_start]
        cmp     eax,ASPI_TICKS
        ja      acl_bad
        mov     eax,ASB_WAIT
        call    aspi_run
        jc      acl_bad
        jmp     short acl_wait
acl_ok:
        popad
        clc
        ret
acl_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; aspi_run - EAX = an offset in the block: run the stub there, in V86,
;            on a stack of DEVLOAD's.  CF if the excursion went wrong.
;            All registers kept.
;
; DEVLOAD's call state is saved round it, because this can happen in
; the middle of a driver call: a loaded driver's own INT 21h reading a
; file on a disc behind the manager is exactly that.
;---------------------------------------------------------------------
aspi_run:
        pushad
        push    dword ptr [dl_drvseg]
        push    dword ptr [dl_entry]
        push    dword ptr [dl_pktoff]
        push    dword ptr [dl_stktop]
        push    dword ptr [dl_hdrflat]
        movzx   ecx,byte ptr [dl_lost]
        push    ecx
        mov     [dl_entry],eax
        call    dl_enter                ; -> EBX = the level's packet offset
        jc      arn_deep
        mov     eax,[aspi_seg]
        mov     [dl_drvseg],eax
        mov     [dl_pktoff],ebx
        lea     eax,[ebx+DL_STK]
        mov     [dl_stktop],eax
        mov     byte ptr [dl_lost],0
        call    dl_excur
        call    dl_leave
        mov     al,byte ptr [dl_lost]
        mov     byte ptr [arn_lost],al
        jmp     short arn_back
arn_deep:
        mov     byte ptr [arn_lost],1
arn_back:
        pop     ecx
        mov     byte ptr [dl_lost],cl
        pop     dword ptr [dl_hdrflat]
        pop     dword ptr [dl_stktop]
        pop     dword ptr [dl_pktoff]
        pop     dword ptr [dl_entry]
        pop     dword ptr [dl_drvseg]
        cmp     byte ptr [arn_lost],0
        jne     arn_bad
        popad
        clc
        ret
arn_bad:
        popad
        stc
        ret

;=====================================================================
; Data
;=====================================================================
aspi_name   db  'SCSIMGR$'
aspi_entry  dd  0                       ; the manager's entry, or 0
aspi_seg    dd  0                       ; the block below 1MB
acl_start   dd  0
asc_count   dd  0                       ; adapters the manager has
asc_pmio    dd  0                       ; ...and PMIO's number for this one
asc_ha      db  0
asc_tgt     db  0
arn_lost    db  0
scsi_on     db  1                       ; SCSI=ON|OFF: look for cards
asc_inq     db  36 dup (0)
asc_srq     db  SRQ_SIZE dup (0)
aspi_tmpl   db  HBA_LEN dup (0)
