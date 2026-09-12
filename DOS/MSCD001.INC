;=====================================================================
; MSCD001.INC - the CD-ROM device driver MSCDEX would have talked to
;
; MSCDEX.INC answers the questions a program asks ABOUT the drives -
; how many, which letters, which version - out of the volume table.
; Three calls in that family it deliberately refused, and two of them
; refused for the same reason: 1501h hands back a DEVICE DRIVER and
; 1510h sends a REQUEST to one.  (The third, 150Fh, asks one for a
; directory entry, and it turned out to belong on the filesystem side
; after all - it is answered in MSCDEX.INC now, through the reader the
; drive letter uses.)
; There was no driver, and the note left at the time said a synthetic
; header would be worse than silence - it invites a program to CALL a
; strategy routine that does not exist, and a request packet that is
; never marked done hangs the caller in a wait loop rather than
; failing.  That note also said what the right answer was: a REAL
; character device named MSCD001, served by the kernel.
;
; This is that device, and CD AUDIO is why it had to exist now.  Every
; audio call a DOS program makes - play, stop, resume, where is the
; head, what tracks are on this disc - is an MSCDEX 1510h request
; aimed at the driver.  There is no other route: the audio API IS the
; device interface.  So the two gaps were always one job.
;
; HOW A GUEST REACHES 32-BIT CODE, which is the only genuinely new
; machinery here.  The header carries two far entry points, and both
; are two bytes of guest memory holding HLT and RETF.  The HLT faults
; out of V86, the monitor recognises the address as one the kernel
; planted, the kernel does the work and steps IP past the HLT, and the
; RETF takes the program home.  That is exactly the trick the XMS
; entry point and the DPMI mode switch already use, and it beats
; claiming an interrupt vector because every vector belongs to
; somebody.
;
; SO THE DEVICE IS REAL FROM BOTH SIDES.  It is linked into the device
; chain behind NUL, where a program walking the chain finds it and
; reads its name; 1501h hands out its address; a program that calls
; the strategy and interrupt entries directly gets served; and 1510h
; is answered by running the same request handler without the round
; trip, which is what MSCDEX itself did.
;
; WHAT IS HERE NOW that was not at first: RAW 2352-BYTE READS, built
; as a READ CD packet because no block read can express one, and the
; DISC'S CATALOGUE NUMBER out of the Q sub-channel.  150Fh went the
; other way and is answered in MSCDEX.INC, through the same reader the
; drive letter uses rather than through a second one of its own.
;
; WHAT IS STILL NOT HERE, and it is an honest "unknown command" rather
; than a wrong answer: the R-W SUB-CHANNELS, IOCTL input 13.  Two
; reasons, and the second is the one that decides it: no drive in
; reach answers a request for de-interleaved R-W data, and the layout
; of that control block is the one part of this interface that could
; not be established from anything on this machine - MSCDEX passes the
; block through to the driver without reading it, so its own binary
; does not describe it either.  A guessed structure that a program
; writes its graphics into is worse than a refusal.
;
; Included by DOSMAIN.ASM after MSCDEX.INC.
;=====================================================================

; The guest-visible block: a device header, the CD-ROM extension that
; follows it, and the two entry stubs.  Two paragraphs in all.
CDDB_ATTR       EQU     0C800h          ; character device, IOCTL, open and
                                        ; close - what every CD driver set
CDDB_RESV       EQU     12h             ; word - reserved, zero
CDDB_LETTER     EQU     14h             ; byte - the first drive letter it
                                        ; owns, 1 = A.  MSCDEX filled this
                                        ; in; here the kernel does
CDDB_UNITS      EQU     15h             ; byte - how many letters
CDDB_STRAT      EQU     16h             ; HLT, RETF
CDDB_INTR       EQU     18h             ; HLT, RETF
CDDB_SIZE       EQU     1Ah

; The request header, as every DOS driver has seen it since 2.0.
CDRQ_LEN        EQU     00h             ; byte
CDRQ_SUB        EQU     01h             ; byte - which unit of this driver
CDRQ_CMD        EQU     02h             ; byte
CDRQ_STAT       EQU     03h             ; word
CDRQ_MEDIA      EQU     0Dh             ; byte - media descriptor / mode
CDRQ_ADDRM      EQU     0Dh             ; ...which on the CD commands is the
                                        ; ADDRESSING MODE instead
CDRQ_XFER       EQU     0Eh             ; dword - far pointer to the data
CDRQ_COUNT      EQU     12h             ; word - bytes, for an IOCTL
CDRQ_NSEC       EQU     12h             ; word - sectors, for a read
CDRQ_SECTOR     EQU     14h             ; dword - the first one
CDRQ_RDMODE     EQU     18h             ; byte - 0 cooked, 1 raw
CDRQ_PLAYST     EQU     0Eh             ; dword - play: the first sector
CDRQ_PLAYCNT    EQU     12h             ; dword - play: how many

; The commands.  The gap between 14 and 128 is where a block device's
; commands live, and a CD-ROM driver is a character device.
CDC_INIT        EQU     0
CDC_IOCTLIN     EQU     3
CDC_FLUSHIN     EQU     7
CDC_FLUSHOUT    EQU     11
CDC_IOCTLOUT    EQU     12
CDC_OPEN        EQU     13
CDC_CLOSE       EQU     14
CDC_READLONG    EQU     128
CDC_PREFETCH    EQU     130
CDC_SEEK        EQU     131
CDC_PLAY        EQU     132
CDC_STOP        EQU     133
CDC_RESUME      EQU     136

CDST_DONE       EQU     0100h
CDST_BUSY       EQU     0200h           ; audio is still running
CDST_ERR        EQU     8000h

; The error codes a driver returns in the low byte of its status, and
; they are DOS's own numbers rather than this kernel's.
CDE_UNKUNIT     EQU     1
CDE_NOTREADY    EQU     2
CDE_UNKCMD      EQU     3
CDE_BADLEN      EQU     5
CDE_SEEKERR     EQU     6
CDE_NOTFOUND    EQU     8
CDE_READFAULT   EQU     11
CDE_GENERAL     EQU     12
CDE_DISKCHG     EQU     15

; How many packet devices there can be, which is PMIO's ATA_MAXDEV
; seen from this side.  It is written out again rather than shared,
; because ATAPI.INC's equates belong to PMIO and this file is in
; PMDOS: the two are assembled separately and the only things they
; agree on are in PMDOS.INC, deliberately.
CDD_MAXUNIT     EQU     4

; The per-unit flags this driver keeps for itself.
CDF_PLAYED      EQU     01h             ; a play was started on this unit,
                                        ; so the busy bit is worth asking
                                        ; about
CDF_PAUSED      EQU     02h             ; ...and stopped while it ran
CDF_LOCKED      EQU     04h             ; the door is locked

;=====================================================================
; cdd_init - plant the device and link it into the chain.
;
; ONLY IF THERE IS A CD-ROM.  A machine with no packet device gets no
; MSCD001, which is the same answer MSCDEX.INC gives to every call it
; is asked on such a machine: a machine with no CD-ROM has no CD-ROM
; extensions on it either.
;
; The memory comes out of the DOS arena marked owner 8, exactly as the
; XMS callback paragraph does - no PSP is ever 8, so the exit sweeps
; walk past it and it lives as long as the machine.
;=====================================================================
cdd_init:
        pushad
        xor     al,al
        xor     ecx,ecx
        call    dword ptr ds:[IOS_CDINFO]       ; ECX = units, valid on CF
        or      ecx,ecx
        jz      cdi_none

        mov     ebx,(CDDB_SIZE + 15) / 16
        call    lm_sysalloc
        jc      cdi_none
        movzx   eax,ax
        mov     [cdd_seg],eax
        mov     edi,eax
        shl     edi,4

        ; the header itself
        mov     dword ptr [edi+DEVH_NEXT],0FFFFFFFFh
        mov     word ptr [edi+DEVH_ATTR],CDDB_ATTR
        mov     word ptr [edi+DEVH_STRAT],CDDB_STRAT
        mov     word ptr [edi+DEVH_INTR],CDDB_INTR
        push    edi
        add     edi,DEVH_NAME
        mov     esi,OFFSET cdd_name
        mov     ecx,8
        cld
        rep     movsb
        pop     edi
        mov     word ptr [edi+CDDB_RESV],0
        mov     byte ptr [edi+CDDB_LETTER],0
        mov     byte ptr [edi+CDDB_UNITS],0

        ; and the two entry points a program may call
        mov     byte ptr [edi+CDDB_STRAT],0F4h          ; HLT
        mov     byte ptr [edi+CDDB_STRAT+1],0CBh        ; RETF
        mov     byte ptr [edi+CDDB_INTR],0F4h
        mov     byte ptr [edi+CDDB_INTR+1],0CBh

        ; BEHIND NUL, where DOS has always put a loaded driver: the
        ; list of lists anchors on NUL and every device walker starts
        ; there, so going in immediately after it leaves the chain
        ; whole from either end.  This is dl_chain's link, done once
        ; for a device that was never loaded from a file.
        movzx   edi,word ptr ds:[vdm_lolseg]
        shl     edi,4
        add     edi,GLOL_HNUL
        mov     eax,[edi+DEVH_NEXT]
        mov     esi,[cdd_seg]
        shl     esi,4
        mov     [esi+DEVH_NEXT],eax
        mov     eax,[cdd_seg]
        shl     eax,16                                  ; seg:0000
        mov     [edi+DEVH_NEXT],eax
cdi_none:
        popad
        ret

;=====================================================================
; cdd_sync - write the drive letter and unit count into the header.
;
; Asked before every call that hands the header out, because CDROM= in
; CONFIG.SYS can move the letters after the device was planted and a
; number written once at boot would be a lie by the time anything read
; it.
;=====================================================================
cdd_sync:
        pushad
        cmp     dword ptr [cdd_seg],0
        je      cds_out
        call    cdx_count                       ; ECX = how many, EBX = first
        mov     edi,[cdd_seg]
        shl     edi,4
        mov     eax,ebx
        inc     eax                             ; the header counts A as 1
        mov     [edi+CDDB_LETTER],al
        mov     eax,ecx
        mov     [edi+CDDB_UNITS],al
cds_out:
        popad
        ret

;=====================================================================
; cdd_iscb - CF clear if the context's CS:IP is sitting on one of this
;            device's entry stubs, and EAX says which.
;=====================================================================
cdd_iscb:
        push    edx
        mov     eax,[cdd_seg]
        or      eax,eax
        jz      cdcb_no
        cmp     dword ptr [vdm_ctx+V86C_CS],eax
        jne     cdcb_no
        mov     edx,dword ptr [vdm_ctx+V86C_EIP]
        cmp     edx,CDDB_STRAT
        je      cdcb_yes
        cmp     edx,CDDB_INTR
        jne     cdcb_no
cdcb_yes:
        mov     eax,edx
        pop     edx
        clc
        ret
cdcb_no:
        pop     edx
        stc
        ret

;=====================================================================
; cdd_call - a guest has called one of the entry points.  EAX = which.
;
; STRATEGY SAVES THE PACKET AND INTERRUPT DOES THE WORK, which is an
; artefact of a 1981 plan for asynchronous drivers that never arrived
; and is what every driver ever shipped does.  A DOS that called only
; the second would break all of them, so both exist here too - and
; the first genuinely only remembers where the packet is.
;=====================================================================
cdd_call:
        push    eax
        push    edx
        inc     dword ptr [vdm_ctx+V86C_EIP]    ; over the HLT, so the RETF
                                                ; takes the caller home
        cmp     eax,CDDB_STRAT
        jne     cdc_intr

        mov     eax,dword ptr [vdm_ctx+V86C_ES]
        shl     eax,4
        movzx   edx,word ptr [vdm_ctx+V86C_EBX]
        add     eax,edx
        mov     [cdd_reqp],eax
        pop     edx
        pop     eax
        ret

cdc_intr:
        cmp     dword ptr [cdd_reqp],0
        je      cdc_out                         ; interrupt without strategy
        call    cdd_bysub                       ; the subunit names the unit
        jc      cdc_out
        call    cdd_request
cdc_out:
        pop     edx
        pop     eax
        ret

;=====================================================================
; cdd_f01 - INT 2Fh AX=1501h: the driver's own address, per drive.
;
;   ES:BX -> five bytes per CD letter: the subunit, then a far
;            pointer to the device header
;=====================================================================
cdd_f01:
        cmp     dword ptr [cdd_seg],0
        je      cdd_f01no
        call    cdd_sync
        call    cdx_count                       ; ECX = how many
        call    cdx_esbx                        ; -> EDI
        mov     eax,edi
        imul    ecx,5
        call    buf_ok
        jc      cdd_f01bad

        xor     esi,esi                         ; the subunit being written
        mov     edx,[cdd_seg]
        shl     edx,16                          ; seg:0000, once
cdd_f01lp:
        call    cdx_count
        cmp     esi,ecx
        jae     cdd_f01ok
        mov     eax,esi
        mov     [edi],al
        mov     [edi+1],edx
        add     edi,5
        inc     esi
        jmp     short cdd_f01lp
cdd_f01ok:
        clc
        ret
cdd_f01no:
        stc                                     ; no device: chain it, which
        ret                                     ; is what this call did
cdd_f01bad:                                     ; before there was one
        mov     ax,CDX_ERR_FUNC
        jmp     cdx_fail

;=====================================================================
; cdd_f10 - INT 2Fh AX=1510h: send a device request.
;
;   CX = drive letter, ES:BX -> a request header
;
; MSCDEX filled in the subunit from the drive letter and passed the
; packet to the driver; this does the same thing without the round
; trip, because the driver is us.
;=====================================================================
cdd_f10:
        cmp     dword ptr [cdd_seg],0
        je      cdd_f10no
        call    cdx_unit                        ; CX -> AL, CF if not a CD
        jc      cdd_f10drv
        mov     [cdd_unit],al

        call    cdx_esbx                        ; -> EDI
        mov     eax,edi
        mov     ecx,1Bh                         ; the longest header a
        call    buf_ok                          ; command here reads
        jc      cdd_f10bad
        mov     [cdd_reqp],edi

        ; THE SUBUNIT IS A POSITION AMONG THE CD LETTERS, so what goes
        ; into cdd_subof is the LETTER the guest named - not the ATAPI
        ; unit cdx_unit has just turned it into.  Handing it the unit
        ; is how the first version of this failed, and it failed
        ; SILENTLY in the worst way available: the call was refused
        ; before the request header was touched, so the status word
        ; stayed exactly as the caller had left it, and a program that
        ; reads only the status - which is every program - saw a
        ; request that had apparently succeeded and a buffer that had
        ; not been written.  Same family as DRV_BIOS holding an ATAPI
        ; unit: every one of these numbers is a small integer, and the
        ; wrong one reaches the wrong thing rather than failing.
        movzx   eax,word ptr [vdm_ctx+V86C_ECX]
        call    cdd_subof                       ; -> AL = the subunit
        jc      cdd_f10drv
        mov     edi,[cdd_reqp]
        mov     [edi+CDRQ_SUB],al

        call    cdd_request
        clc
        ret
cdd_f10no:
        stc
        ret
cdd_f10drv:
        mov     ax,CDX_ERR_DRIVE
        jmp     cdx_fail
cdd_f10bad:
        mov     ax,CDX_ERR_FUNC
        jmp     cdx_fail

;=====================================================================
; cdd_bysub / cdd_subof - the two directions of one mapping.
;
; A SUBUNIT IS A POSITION AMONG THE CD LETTERS and not a drive number
; and not an ATAPI unit, and keeping the three apart is the same trap
; DRV_BIOS already taught this kernel once: every one of them is a
; small integer and using the wrong one reaches the wrong device
; rather than failing.
;
;   cdd_bysub  [cdd_reqp]'s subunit -> [cdd_unit], CF if there is none
;   cdd_subof  AL = a drive letter  -> AL = its subunit, CF if not a CD
;=====================================================================
cdd_bysub:
        pushad
        mov     edi,[cdd_reqp]
        movzx   edx,byte ptr [edi+CDRQ_SUB]
        xor     esi,esi                         ; the letter being examined
        xor     ebp,ebp                         ; ...and its subunit
cdb_scan:
        cmp     esi,MAX_DRIVES
        jae     cdb_no
        mov     eax,esi
        call    drv_slot                        ; -> EBX
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      cdb_next
        test    byte ptr [ebx+DRV_FLAGS],DRVF_CDROM
        jz      cdb_next
        cmp     ebp,edx
        je      cdb_got
        inc     ebp
cdb_next:
        inc     esi
        jmp     short cdb_scan
cdb_got:
        mov     al,byte ptr [ebx+DRV_BIOS]      ; the ATAPI unit
        mov     [cdd_unit],al
        popad
        clc
        ret
cdb_no:
        popad
        stc
        ret

cdd_subof:
        push    ebx
        push    ecx
        push    edx
        push    esi
        movzx   edx,al                          ; the letter wanted
        xor     esi,esi
        xor     ecx,ecx                         ; the subunit counter
cdsu_scan:
        cmp     esi,MAX_DRIVES
        jae     cdsu_no
        mov     eax,esi
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      cdsu_next
        test    byte ptr [ebx+DRV_FLAGS],DRVF_CDROM
        jz      cdsu_next
        cmp     esi,edx
        je      cdsu_got
        inc     ecx
cdsu_next:
        inc     esi
        jmp     short cdsu_scan
cdsu_got:
        mov     eax,ecx
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
cdsu_no:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; cdd_request - run the request at [cdd_reqp] against [cdd_unit].
;
; Every exit goes through cdr_done, which writes the status word -
; because a request packet that comes back without the DONE bit set
; leaves the caller spinning on it forever, and that is the failure
; mode this whole file was written to avoid.
;=====================================================================
cdd_request:
        pushad
        mov     word ptr [cdd_stat],CDST_DONE
        mov     byte ptr [cdd_ecode],0
        mov     edi,[cdd_reqp]
        movzx   eax,byte ptr [edi+CDRQ_CMD]

        cmp     eax,CDC_IOCTLIN
        je      cdr_in
        cmp     eax,CDC_IOCTLOUT
        je      cdr_out
        cmp     eax,CDC_READLONG
        je      cdr_read
        cmp     eax,CDC_SEEK
        je      cdr_seek
        cmp     eax,CDC_PLAY
        je      cdr_play
        cmp     eax,CDC_STOP
        je      cdr_stop
        cmp     eax,CDC_RESUME
        je      cdr_resume
        cmp     eax,CDC_INIT
        je      cdr_ok
        cmp     eax,CDC_OPEN
        je      cdr_ok
        cmp     eax,CDC_CLOSE
        je      cdr_ok
        cmp     eax,CDC_PREFETCH
        je      cdr_ok                  ; a hint, and ignoring a hint is a
        cmp     eax,CDC_FLUSHIN         ; complete implementation of it
        je      cdr_ok
        cmp     eax,CDC_FLUSHOUT
        je      cdr_ok
        mov     al,CDE_UNKCMD
        jmp     cdr_fail

cdr_in:
        call    cdd_ioctlin
        jmp     short cdr_test
cdr_out:
        call    cdd_ioctlout
        jmp     short cdr_test
cdr_read:
        call    cdd_readlong
        jmp     short cdr_test
cdr_seek:
        call    cdd_doseek
        jmp     short cdr_test
cdr_play:
        call    cdd_doplay
        jmp     short cdr_test
cdr_stop:
        call    cdd_dostop
        jmp     short cdr_test
cdr_resume:
        call    cdd_doresume
cdr_test:
        jc      cdr_failed
cdr_ok:
        jmp     short cdr_done
cdr_failed:
        mov     al,[cdd_ecode]
        or      al,al
        jnz     cdr_fail
        mov     al,CDE_GENERAL
cdr_fail:
        movzx   ax,al
        or      ax,CDST_ERR OR CDST_DONE
        mov     [cdd_stat],ax
cdr_done:
        ; THE BUSY BIT, on every reply.  A program that started a play
        ; polls the driver to find out when it has finished, and this
        ; is the bit it polls.
        call    cdd_playing
        jnc     cdr_notbusy
        or      word ptr [cdd_stat],CDST_BUSY
cdr_notbusy:
        mov     edi,[cdd_reqp]
        mov     ax,[cdd_stat]
        mov     word ptr [edi+CDRQ_STAT],ax
        popad
        ret

;=====================================================================
; IOCTL INPUT - the questions.  The control block's first byte says
;               which one, and its length varies with the answer.
;=====================================================================
cdd_ioctlin:
        mov     edi,[cdd_reqp]
        call    cdd_xferptr             ; -> EAX = the control block
        mov     [cdd_cb],eax
        movzx   ecx,word ptr [edi+CDRQ_COUNT]
        or      ecx,ecx
        jz      cdi_badlen
        call    buf_ok
        jc      cdi_badlen

        mov     edi,[cdd_cb]
        movzx   eax,byte ptr [edi]
        cmp     eax,0                   ; the device header's address
        je      cdii_hdr
        cmp     eax,1                   ; where the head is
        je      cdii_head
        cmp     eax,4                   ; the audio channels
        je      cdii_chan
        cmp     eax,6                   ; what this device can do
        je      cdii_stat
        cmp     eax,7                   ; the sector size
        je      cdii_secsz
        cmp     eax,8                   ; how many sectors
        je      cdii_volsz
        cmp     eax,9                   ; has the disc changed
        je      cdii_chg
        cmp     eax,10                  ; the tracks
        je      cdii_disc
        cmp     eax,11                  ; ...one of them
        je      cdii_trk
        cmp     eax,12                  ; the Q channel
        je      cdii_q
        cmp     eax,14                  ; the disc's catalogue number
        je      cdii_upc
        cmp     eax,15                  ; is it paused, and where did it
        je      cdii_astat              ; last play from
        mov     byte ptr [cdd_ecode],CDE_UNKCMD
        stc
        ret
cdi_badlen:
        mov     byte ptr [cdd_ecode],CDE_BADLEN
        stc
        ret

;--- 0: the address of this device's header -------------------------
cdii_hdr:
        call    cdd_sync
        mov     eax,[cdd_seg]
        shl     eax,16
        mov     dword ptr [edi+1],eax
        clc
        ret

;--- 1: where the head is, in whichever notation was asked for ------
cdii_head:
        call    cdd_subq
        jc      cdih_bad
        call    cdd_qlba                ; -> EAX = the absolute position
        movzx   ecx,byte ptr [edi+1]    ; the addressing mode
        call    cdd_toaddr              ; -> EAX in that notation
        mov     dword ptr [edi+2],eax
        clc
        ret
cdih_bad:
        stc
        ret

;--- 4: which input channel feeds which output, and how loudly ------
; MODE SENSE page 0Eh is where a CD-ROM keeps this, and the four
; output channels are four pairs of bytes in it.
cdii_chan:
        call    cdd_modesns
        jc      cdic_bad
        mov     esi,OFFSET cdd_mbuf + 8 + 8     ; header, then the page
        mov     al,[esi]                        ; port 0 channel selection
        mov     [edi+1],al
        mov     al,[esi+1]                      ; ...and its volume
        mov     [edi+2],al
        mov     al,[esi+2]
        mov     [edi+3],al
        mov     al,[esi+3]
        mov     [edi+4],al
        mov     al,[esi+4]
        mov     [edi+5],al
        mov     al,[esi+5]
        mov     [edi+6],al
        mov     al,[esi+6]
        mov     [edi+7],al
        mov     al,[esi+7]
        mov     [edi+8],al
        clc
        ret
cdic_bad:
        stc
        ret

;--- 6: what this device can do, and what is in it ------------------
; The bits are claims, and a wrong claim is worse than a missing
; feature: a program told this drive can read raw sectors will ask for
; some and get cooked ones.  So only what is true is set.
cdii_stat:
        xor     ebx,ebx
        or      ebx,10h                 ; reads data AND plays audio
        or      ebx,100h                ; the audio channels can be set
        or      ebx,200h                ; both HSG and Red Book addressing
        movzx   eax,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,eax
        test    byte ptr [esi],CDF_LOCKED
        jnz     cdis_locked
        or      ebx,2                   ; the door is unlocked
cdis_locked:
        call    cdd_ready               ; -> CF if there is no disc, and
        jnc     cdis_disc               ; [cdd_sense] says why
        or      ebx,800h                ; nothing in the drive
        mov     eax,[cdd_sense]
        shr     eax,8
        and     eax,0FFh
        cmp     al,3Ah                  ; "medium not present" - and the
        jne     cdis_disc               ; qualifier says whether the tray
        mov     eax,[cdd_sense]         ; is standing open
        shr     eax,16
        and     eax,0FFh
        cmp     al,2
        jne     cdis_disc
        or      ebx,1                   ; the door is open
cdis_disc:
        mov     dword ptr [edi+1],ebx
        clc
        ret

;--- 7: how big a sector is ------------------------------------------
cdii_secsz:
        mov     word ptr [edi+2],ISO_BLKSIZE
        clc
        ret

;--- 8: how many of them there are -----------------------------------
cdii_volsz:
        movzx   eax,byte ptr [cdd_unit]
        call    dword ptr ds:[IOS_CDINFO]       ; EAX = the last block
        jc      cdiv_bad
        inc     eax
        mov     dword ptr [edi+1],eax
        clc
        ret
cdiv_bad:
        mov     byte ptr [cdd_ecode],CDE_NOTREADY
        stc
        ret

;--- 9: has the disc been changed ------------------------------------
; 1 means no, FFh means yes, and 0 means the driver cannot tell.  The
; question is asked of PMIO, which keeps a bit for this listener
; precisely because the drive reports a change once and the asking
; destroys it - see atapi_pendall.
cdii_chg:
        movzx   eax,byte ptr [cdd_unit]
        mov     ah,CDL_CDX
        call    dword ptr ds:[IOS_CDCHG]
        jc      cdich_moved
        mov     byte ptr [edi+1],1
        clc
        ret
cdich_moved:
        ; A drive with nothing in it answers "changed" every time it is
        ; asked, which is true and useless.  "Cannot tell" is the
        ; better answer there, and it is the one MSCDEX gave.
        call    cdd_ready
        jc      cdich_nodisc
        mov     byte ptr [edi+1],0FFh
        clc
        ret
cdich_nodisc:
        mov     byte ptr [edi+1],0
        clc
        ret

;--- 10: the first and last tracks, and where the music runs out ----
cdii_disc:
        movzx   eax,byte ptr [cdd_unit]
        call    cd_readtoc
        jc      cdid_bad
        mov     al,byte ptr [iso_toc+ISO_TOC_FIRST]
        mov     [edi+1],al
        mov     al,byte ptr [iso_toc+ISO_TOC_LAST]
        mov     [edi+2],al
        mov     al,ISO_TRK_LEADOUT
        call    cdd_findtrk             ; -> ESI
        jc      cdid_bad
        call    cdd_trklba              ; -> EAX
        call    cdd_lba2rb              ; -> EAX, Red Book
        mov     dword ptr [edi+3],eax
        clc
        ret
cdid_bad:
        mov     byte ptr [cdd_ecode],CDE_NOTREADY
        stc
        ret

;--- 11: where one track starts, and what kind of track it is -------
cdii_trk:
        movzx   eax,byte ptr [cdd_unit]
        call    cd_readtoc
        jc      cdit_bad
        mov     al,byte ptr [edi+1]     ; the track asked about
        call    cdd_findtrk
        jc      cdit_bad
        call    cdd_trklba
        call    cdd_lba2rb
        mov     dword ptr [edi+2],eax
        mov     al,byte ptr [esi+ISO_TD_ADRCTL]
        rol     al,4                    ; MSCDEX wants CONTROL in the high
        mov     [edi+6],al              ; nibble; SCSI puts ADR there
        clc
        ret
cdit_bad:
        mov     byte ptr [cdd_ecode],CDE_NOTFOUND
        stc
        ret

;--- 12: the Q channel, which is what a CD player displays ----------
cdii_q:
        call    cdd_subq
        jc      cdiq_bad
        mov     al,byte ptr [cdd_sbuf+5]
        rol     al,4                    ; CONTROL into the high nibble
        mov     [edi+1],al
        mov     al,byte ptr [cdd_sbuf+6]        ; the track...
        call    cdd_bcd                         ; ...as it is on the disc,
        mov     [edi+2],al                      ; which is BCD
        mov     al,byte ptr [cdd_sbuf+7]        ; the index, the same way
        call    cdd_bcd
        mov     [edi+3],al
        mov     al,byte ptr [cdd_sbuf+13]       ; running time in the track
        mov     [edi+4],al
        mov     al,byte ptr [cdd_sbuf+14]
        mov     [edi+5],al
        mov     al,byte ptr [cdd_sbuf+15]
        mov     [edi+6],al
        mov     byte ptr [edi+7],0
        mov     al,byte ptr [cdd_sbuf+9]        ; and on the disc
        mov     [edi+8],al
        mov     al,byte ptr [cdd_sbuf+10]
        mov     [edi+9],al
        mov     al,byte ptr [cdd_sbuf+11]
        mov     [edi+10],al
        clc
        ret
cdiq_bad:
        stc
        ret

;--- 15: paused or not, and the range the last play named -----------
cdii_astat:
        movzx   ebx,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,ebx
        xor     eax,eax
        test    byte ptr [esi],CDF_PAUSED
        jz      cdia_notp
        or      eax,1
cdia_notp:
        mov     word ptr [edi+1],ax
        shl     ebx,2
        mov     esi,OFFSET cdd_pst
        add     esi,ebx
        mov     eax,[esi]
        mov     dword ptr [edi+3],eax
        mov     esi,OFFSET cdd_pen
        add     esi,ebx
        mov     eax,[esi]
        mov     dword ptr [edi+7],eax
        clc
        ret

;--- 14: the disc's catalogue number --------------------------------
; THE UPC IS IN THE SUB-CHANNEL AND NOT IN ANY FILESYSTEM.  It is the
; barcode printed on the back of a pressed disc, recorded in the Q
; channel so that a program can tell one disc from another without
; reading a byte of what is on it - which is what a jukebox does, and
; what a CD player does before it looks a disc up in a database.
;
; The drive answers READ SUB-CHANNEL format 2 with the number as
; THIRTEEN ASCII DIGITS.  The control block wants it as PACKED BCD,
; two digits to a byte, seven bytes with the last nibble empty - so
; this is a conversion and not a copy, and getting it wrong would show
; up as a disc that identifies as a different disc.
;
; MOST DISCS DO NOT HAVE ONE.  The drive says so in MCVal, and the
; field is then left zero, which is what "there is not one" looks like
; in a field that is always fourteen digits long.  The control and ADR
; byte is left zero as well: a format 2 answer does not carry one, and
; the current track's would be an answer to a different question.
cdii_upc:
        push    edi
        mov     ecx,10                  ; the whole block, zeroed first:
        xor     al,al                   ; every part of the answer that is
        cld                             ; not filled in below MEANS zero
        rep     stosb
        pop     edi
        mov     byte ptr [edi],0Eh
        call    cdd_mcn
        jc      cdiu_bad
        test    byte ptr [cdd_sbuf+8],80h
        jz      cdiu_none               ; no catalogue number on this disc
        push    esi
        push    edi
        mov     esi,OFFSET cdd_sbuf + 9 ; the digits
        add     edi,2                   ; where they pack down to
        xor     ebx,ebx                 ; which digit
cdiu_pack:
        cmp     ebx,14
        jae     cdiu_packed
        call    cdiu_digit              ; -> AL = the high half
        shl     al,4
        mov     ah,al
        inc     ebx
        call    cdiu_digit
        or      al,ah
        mov     [edi],al
        inc     edi
        inc     ebx
        jmp     short cdiu_pack
cdiu_packed:
        pop     edi
        pop     esi
cdiu_none:
        clc
        ret
cdiu_bad:
        stc
        ret

;--- cdiu_digit - EBX = which of the thirteen, ESI = them.  AL = its
;--- value, and 0 for the fourteenth nibble and for anything that is
;--- not a digit: a drive that pads the field with spaces must not
;--- pack a space into the answer as though it were a number.
cdiu_digit:
        mov     al,[esi+ebx]
        cmp     al,'0'
        jb      cdiu_zero
        cmp     al,'9'
        ja      cdiu_zero
        sub     al,'0'
        ret
cdiu_zero:
        xor     al,al
        ret

;--- cdd_mcn - READ SUB-CHANNEL, the media catalogue number, into
;--- cdd_sbuf.  The same command as cdd_subq with a different format
;--- byte, and deliberately a separate routine: the two answers have
;--- different shapes and sharing the buffer is as far as the
;--- similarity goes.
cdd_mcn:
        push    eax
        push    ecx
        push    edi
        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_SUBCHAN
        mov     byte ptr [cdd_dcb+2],40h        ; the Q sub-channel
        mov     byte ptr [cdd_dcb+3],02h        ; ...its catalogue number
        mov     byte ptr [cdd_dcb+8],24
        xor     ah,ah
        mov     ecx,24
        mov     edi,OFFSET cdd_sbuf
        call    cdd_pkt
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; IOCTL OUTPUT - the instructions.
;=====================================================================
cdd_ioctlout:
        mov     edi,[cdd_reqp]
        call    cdd_xferptr
        mov     [cdd_cb],eax
        movzx   ecx,word ptr [edi+CDRQ_COUNT]
        or      ecx,ecx
        jz      cdo_badlen
        call    buf_ok
        jc      cdo_badlen

        mov     edi,[cdd_cb]
        movzx   eax,byte ptr [edi]
        cmp     eax,0                   ; eject
        je      cdio_eject
        cmp     eax,1                   ; lock the door, or unlock it
        je      cdio_lock
        cmp     eax,2                   ; reset the drive
        je      cdio_reset
        cmp     eax,3                   ; the audio channels
        je      cdio_chan
        cmp     eax,5                   ; close the tray
        je      cdio_close
        mov     byte ptr [cdd_ecode],CDE_UNKCMD
        stc
        ret
cdo_badlen:
        mov     byte ptr [cdd_ecode],CDE_BADLEN
        stc
        ret

cdio_eject:
        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_STARTSTOP
        mov     byte ptr [cdd_dcb+1],1          ; return before it finishes
        mov     byte ptr [cdd_dcb+4],02h        ; load/eject, and stop
        xor     ah,ah
        xor     ecx,ecx
        xor     edi,edi
        call    cdd_pkt
        ret
cdio_close:
        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_STARTSTOP
        mov     byte ptr [cdd_dcb+1],1
        mov     byte ptr [cdd_dcb+4],03h        ; load/eject, and start
        xor     ah,ah
        xor     ecx,ecx
        xor     edi,edi
        call    cdd_pkt
        ret
cdio_lock:
        movzx   eax,byte ptr [edi+1]            ; 0 unlock, 1 lock
        push    eax
        call    cdd_cdbzero
        pop     eax
        mov     byte ptr [cdd_dcb+0],CDOP_PREVENT
        mov     byte ptr [cdd_dcb+4],al
        push    eax
        xor     ah,ah
        xor     ecx,ecx
        xor     edi,edi
        call    cdd_pkt
        pop     eax
        jc      cdio_lockbad
        movzx   ebx,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,ebx
        or      al,al
        jz      cdio_unlocked
        or      byte ptr [esi],CDF_LOCKED
        clc
        ret
cdio_unlocked:
        and     byte ptr [esi],NOT CDF_LOCKED
        clc
        ret
cdio_lockbad:
        stc
        ret
cdio_reset:
        ; There is nothing to reset that the next command does not
        ; already put right, and reporting success for doing nothing is
        ; the truthful answer rather than the lazy one.
        clc
        ret
cdio_chan:
        ; MODE SELECT of the same page MODE SENSE reads.  The page is
        ; read first rather than invented, so that the fields this call
        ; does not set keep the values the drive had.
        call    cdd_modesns
        jc      cdio_chanbad
        mov     esi,edi                         ; the caller's block
        mov     edi,OFFSET cdd_mbuf + 8 + 8
        mov     al,[esi+1]
        mov     [edi],al
        mov     al,[esi+2]
        mov     [edi+1],al
        mov     al,[esi+3]
        mov     [edi+2],al
        mov     al,[esi+4]
        mov     [edi+3],al
        mov     al,[esi+5]
        mov     [edi+4],al
        mov     al,[esi+6]
        mov     [edi+5],al
        mov     al,[esi+7]
        mov     [edi+6],al
        mov     al,[esi+8]
        mov     [edi+7],al
        call    cdd_modesel
        ret
cdio_chanbad:
        stc
        ret

;=====================================================================
; READ LONG - the driver's own way to read sectors, which a program
;             may use instead of the drive letter.
;
; TWO MODES, and they are two different commands to the drive.  Cooked
; is 2048 bytes of user data per sector and goes through IOS_CDREAD,
; which is what every other read in this kernel uses.  Raw is the
; 2352-byte FRAME as it is recorded - sync pattern, header, user data,
; error correction - and there is no way to get that out of a block
; read, so it is built here as a packet of its own.
;=====================================================================
cdd_readlong:
        mov     edi,[cdd_reqp]
        cmp     byte ptr [edi+CDRQ_RDMODE],1
        je      cdd_readraw
        cmp     byte ptr [edi+CDRQ_RDMODE],0
        jne     cdrl_unk
        movzx   ecx,word ptr [edi+CDRQ_NSEC]
        or      ecx,ecx
        jz      cdrl_none
        cmp     ecx,CDX_MAXBLK
        ja      cdrl_badlen             ; more than an ES:BX buffer holds
        mov     [cdd_cnt],ecx

        call    cdd_xferptr
        mov     [cdd_cb],eax
        mov     ecx,[cdd_cnt]
        shl     ecx,11
        call    buf_ok
        jc      cdrl_badlen

        mov     eax,dword ptr [edi+CDRQ_SECTOR]
        movzx   ecx,byte ptr [edi+CDRQ_ADDRM]
        call    cdd_tolba               ; -> EAX
        mov     ebx,eax
        mov     ecx,[cdd_cnt]
        mov     edi,[cdd_cb]
        mov     al,byte ptr [cdd_unit]
        call    dword ptr ds:[IOS_CDREAD]
        jc      cdrl_bad
cdrl_none:
        clc
        ret
cdrl_unk:
        mov     byte ptr [cdd_ecode],CDE_UNKCMD
        stc
        ret
cdrl_badlen:
        mov     byte ptr [cdd_ecode],CDE_BADLEN
        stc
        ret
cdrl_bad:
        mov     byte ptr [cdd_ecode],CDE_READFAULT
        stc
        ret

;=====================================================================
; READ LONG, RAW - the sector as it is recorded and not as it is meant.
;
; A raw read hands back the whole 2352-byte frame: twelve bytes of
; sync, a four-byte header saying which sector this is, the user data,
; and the error correction behind it.  A program asks for one to do
; something the filesystem cannot - copy an audio track's samples,
; check a disc byte for byte, read a sector whose mode nothing here
; understands - and for that the answer has to be the frame or
; nothing.  Handing back 2048 cooked bytes and leaving the caller to
; wonder is the failure this driver refused to have until it could do
; the real thing.
;
; READ CD (BEh) is the command and byte nine is the whole of it: 0F8h
; asks for sync, all three header fields, the user data and the
; EDC/ECC, which is every byte of the frame.  The sector TYPE field
; stays zero - "any" - because a program reading raw is usually
; reading something this driver has no business having an opinion
; about, an audio frame most of all.
;
; NOT EVERY DRIVE HAS IT.  One that does not answers ILLEGAL REQUEST,
; cdd_map turns that into "unknown command", and the caller is told
; the same thing it was told before by a driver that could not do this
; at all.  That is the truth on such a drive rather than a limitation
; of this code, and it is why the claim in the device-status bits is
; still not made: 6's bit 0 is "reads raw", and whether this drive
; does cannot be known without asking it to.
;=====================================================================
CDD_RAWSEC      EQU     2352            ; a frame, all of it
CDD_RAWMAX      EQU     27              ; 64K / 2352: what an ES:BX buffer
                                        ; can hold before BX wraps, which
                                        ; is the same rule CDX_MAXBLK is
cdd_readraw:
        movzx   ecx,word ptr [edi+CDRQ_NSEC]
        or      ecx,ecx
        jz      cdrr_none               ; nothing asked for is not an error
        cmp     ecx,CDD_RAWMAX
        ja      cdrl_badlen
        mov     [cdd_cnt],ecx

        call    cdd_xferptr
        mov     [cdd_cb],eax
        mov     ecx,[cdd_cnt]
        imul    ecx,ecx,CDD_RAWSEC
        call    buf_ok
        jc      cdrl_badlen

        mov     edi,[cdd_reqp]
        mov     eax,dword ptr [edi+CDRQ_SECTOR]
        movzx   ecx,byte ptr [edi+CDRQ_ADDRM]
        call    cdd_tolba
        push    eax
        call    cdd_cdbzero
        pop     eax
        mov     byte ptr [cdd_dcb+0],CDOP_READCD
        mov     byte ptr [cdd_dcb+5],al ; the first sector, big-endian as
        shr     eax,8                   ; every packet command counts
        mov     byte ptr [cdd_dcb+4],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+3],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+2],al
        mov     eax,[cdd_cnt]           ; how many, in three bytes
        mov     byte ptr [cdd_dcb+8],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+7],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+6],al
        mov     byte ptr [cdd_dcb+9],0F8h
        mov     ecx,[cdd_cnt]
        imul    ecx,ecx,CDD_RAWSEC
        mov     edi,[cdd_cb]
        xor     ah,ah
        call    cdd_pkt                 ; its sense becomes the error code
        jc      cdrr_bad
cdrr_none:
        clc
        ret
cdrr_bad:
        stc
        ret

;=====================================================================
; SEEK - move the head and say nothing else.
;=====================================================================
cdd_doseek:
        mov     edi,[cdd_reqp]
        mov     eax,dword ptr [edi+CDRQ_SECTOR]
        movzx   ecx,byte ptr [edi+CDRQ_ADDRM]
        call    cdd_tolba
        push    eax
        call    cdd_cdbzero
        pop     eax
        mov     byte ptr [cdd_dcb+0],CDOP_SEEK
        mov     byte ptr [cdd_dcb+5],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+4],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+3],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+2],al
        xor     ah,ah
        xor     ecx,ecx
        xor     edi,edi
        call    cdd_pkt
        ret

;=====================================================================
; PLAY AUDIO - the reason the rest of this file exists.
;
;   the first sector, and how many to play
;
; PLAY AUDIO MSF is the command used rather than PLAY AUDIO(10),
; because the ten-byte form counts its length in a WORD - about
; fourteen minutes - and a program that asks to play a whole disc from
; the first track is asking for more than that.  The MSF form names
; the end instead of the length and has no such limit.
;=====================================================================
cdd_doplay:
        mov     edi,[cdd_reqp]
        mov     eax,dword ptr [edi+CDRQ_PLAYST]
        movzx   ecx,byte ptr [edi+CDRQ_ADDRM]
        call    cdd_tolba
        mov     [cdd_from],eax
        mov     eax,dword ptr [edi+CDRQ_PLAYCNT]
        or      eax,eax
        jz      cdpy_none               ; nothing to play is not an error
        add     eax,[cdd_from]
        mov     [cdd_to],eax

        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_PLAYMSF
        mov     eax,[cdd_from]
        call    cdd_lba2msf             ; -> EAX = 00 MM SS FF
        mov     byte ptr [cdd_dcb+5],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+4],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+3],al
        mov     eax,[cdd_to]
        call    cdd_lba2msf
        mov     byte ptr [cdd_dcb+8],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+7],al
        shr     eax,8
        mov     byte ptr [cdd_dcb+6],al
        xor     ah,ah
        xor     ecx,ecx
        xor     edi,edi
        call    cdd_pkt
        jc      cdpy_bad

        ; remember what was asked for: 15h reports it back, and the
        ; flags are what make the busy bit worth asking about
        movzx   ebx,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,ebx
        or      byte ptr [esi],CDF_PLAYED
        and     byte ptr [esi],NOT CDF_PAUSED
        shl     ebx,2
        mov     eax,[cdd_from]
        call    cdd_lba2rb
        mov     esi,OFFSET cdd_pst
        add     esi,ebx
        mov     [esi],eax
        mov     eax,[cdd_to]
        call    cdd_lba2rb
        mov     esi,OFFSET cdd_pen
        add     esi,ebx
        mov     [esi],eax
cdpy_none:
        clc
        ret
cdpy_bad:
        stc
        ret

;=====================================================================
; STOP AUDIO - which pauses, and only stops if it was already paused.
;
; That two-step is MSCDEX's own definition and not an invention: a
; STOP while playing leaves the position so that RESUME can carry on
; from it, and a second STOP throws the position away.  A program that
; wants silence sends one; a program that wants to start again later
; sends one and then RESUME.
;=====================================================================
cdd_dostop:
        movzx   ebx,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,ebx
        test    byte ptr [esi],CDF_PAUSED
        jnz     cdst_already

        call    cdd_playing
        jnc     cdst_notplaying
        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_PAUSE         ; bit 0 of byte 8
        mov     byte ptr [cdd_dcb+8],0                  ; clear: pause
        xor     ah,ah
        xor     ecx,ecx
        xor     edi,edi
        call    cdd_pkt
        jc      cdst_bad
        movzx   ebx,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,ebx
        or      byte ptr [esi],CDF_PAUSED
        clc
        ret
cdst_already:
        ; paused already: this one really stops, and the range the
        ; last play named goes with it
        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_STOPPLAY
        xor     ah,ah
        xor     ecx,ecx
        xor     edi,edi
        call    cdd_pkt
        movzx   ebx,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,ebx
        and     byte ptr [esi],NOT (CDF_PAUSED OR CDF_PLAYED)
        shl     ebx,2
        mov     esi,OFFSET cdd_pst
        add     esi,ebx
        mov     dword ptr [esi],0
        mov     esi,OFFSET cdd_pen
        add     esi,ebx
        mov     dword ptr [esi],0
        clc
        ret
cdst_notplaying:
        clc
        ret
cdst_bad:
        stc
        ret

;=====================================================================
; RESUME AUDIO - carry on from where STOP left off.
;=====================================================================
cdd_doresume:
        movzx   ebx,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,ebx
        test    byte ptr [esi],CDF_PAUSED
        jz      cdrs_notpaused
        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_PAUSE
        mov     byte ptr [cdd_dcb+8],1          ; set: resume
        xor     ah,ah
        xor     ecx,ecx
        xor     edi,edi
        call    cdd_pkt
        jc      cdrs_bad
        movzx   ebx,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,ebx
        and     byte ptr [esi],NOT CDF_PAUSED
        clc
        ret
cdrs_notpaused:
        ; Nothing to resume.  "General failure" is what MSCDEX said,
        ; and a program that asks anyway is asking a question whose
        ; answer it can see in 15h.
        mov     byte ptr [cdd_ecode],CDE_GENERAL
        stc
        ret
cdrs_bad:
        stc
        ret

;=====================================================================
; The packet plumbing.  Everything above builds a command block and
; gets here; nothing above knows which port it went out of.
;=====================================================================
cdd_cdbzero:
        push    eax
        push    ecx
        push    edi
        mov     edi,OFFSET cdd_dcb
        xor     eax,eax
        mov     ecx,12
        cld
        rep     stosb
        pop     edi
        pop     ecx
        pop     eax
        ret

;--- cdd_pkt - run cdd_dcb.  AH = 1 if it sends, EDI = buffer,
;--- ECX = its size.  -> EAX = bytes moved, CF with [cdd_ecode] set.
cdd_pkt:
        push    ebx
        push    esi
        mov     esi,OFFSET cdd_dcb
        mov     al,byte ptr [cdd_unit]
        call    dword ptr ds:[IOS_CDPKT]
        jc      cdpk_bad
        mov     dword ptr [cdd_sense],0
        pop     esi
        pop     ebx
        clc
        ret
cdpk_bad:
        mov     [cdd_sense],ebx
        call    cdd_map
        pop     esi
        pop     ebx
        stc
        ret

;--- cdd_map - the sense key, as the error number a DOS driver
;--- returns.  The mapping is short because the sense key is a coarse
;--- classification and these numbers are a coarse classification, and
;--- they were both designed for the same conversation.
cdd_map:
        push    eax
        mov     eax,[cdd_sense]
        and     eax,0Fh
        cmp     al,2
        je      cdm_notrdy
        cmp     al,3
        je      cdm_read
        cmp     al,5
        je      cdm_illegal
        cmp     al,6
        je      cdm_changed
        mov     al,CDE_GENERAL
        jmp     short cdm_set
cdm_notrdy:
        mov     al,CDE_NOTREADY
        jmp     short cdm_set
cdm_read:
        mov     al,CDE_READFAULT
        jmp     short cdm_set
cdm_illegal:
        ; ILLEGAL REQUEST covers both "this drive has no such command"
        ; and "that command cannot apply to this track", and the second
        ; is the one that happens: asking for audio from a data track.
        mov     al,CDE_UNKCMD
        jmp     short cdm_set
cdm_changed:
        mov     al,CDE_DISKCHG
cdm_set:
        mov     byte ptr [cdd_ecode],al
        pop     eax
        ret

;--- cdd_ready - TEST UNIT READY, once.  CF if there is no usable
;--- disc, with [cdd_sense] saying why.
cdd_ready:
        push    eax
        push    ecx
        push    edi
        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_TESTRDY
        xor     ah,ah
        xor     ecx,ecx
        xor     edi,edi
        call    cdd_pkt
        pop     edi
        pop     ecx
        pop     eax
        ret

;--- cdd_subq - READ SUB-CHANNEL, current position, into cdd_sbuf.
;--- The addresses come back as MSF, which is what the Q channel
;--- itself carries and what 0Ch hands to the program.
cdd_subq:
        push    eax
        push    ecx
        push    edi
        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_SUBCHAN
        mov     byte ptr [cdd_dcb+1],02h        ; answer in MSF
        mov     byte ptr [cdd_dcb+2],40h        ; the Q sub-channel
        mov     byte ptr [cdd_dcb+3],01h        ; ...its current position
        mov     byte ptr [cdd_dcb+8],16
        xor     ah,ah
        mov     ecx,16
        mov     edi,OFFSET cdd_sbuf
        call    cdd_pkt
        pop     edi
        pop     ecx
        pop     eax
        ret

;--- cdd_modesns / cdd_modesel - the audio control page, in and out.
cdd_modesns:
        push    eax
        push    ecx
        push    edi
        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_MODESNS
        mov     byte ptr [cdd_dcb+2],0Eh        ; the audio control page
        mov     byte ptr [cdd_dcb+8],24         ; header, block descriptor
        xor     ah,ah                           ; and the page itself
        mov     ecx,24
        mov     edi,OFFSET cdd_mbuf
        call    cdd_pkt
        pop     edi
        pop     ecx
        pop     eax
        ret

cdd_modesel:
        push    eax
        push    ecx
        push    edi
        call    cdd_cdbzero
        mov     byte ptr [cdd_dcb+0],CDOP_MODESEL
        mov     byte ptr [cdd_dcb+1],10h        ; the page format, not the
                                                ; vendor's own
        mov     byte ptr [cdd_dcb+8],24
        mov     word ptr [cdd_mbuf],0           ; the data length field is
                                                ; reserved on the way out
        mov     ah,1                            ; THIS ONE SENDS
        mov     ecx,24
        mov     edi,OFFSET cdd_mbuf
        call    cdd_pkt
        pop     edi
        pop     ecx
        pop     eax
        ret

;--- cdd_playing - CF set if audio is running on this unit.
;
; ONLY ASKED WHEN THIS DRIVER STARTED A PLAY.  The busy bit means
; "audio is still running", and a drive that was never told to play is
; not playing - so the ordinary case, a program reading files through
; this device, costs no extra command at all.
cdd_playing:
        push    eax
        push    ebx
        push    esi
        movzx   ebx,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,ebx
        test    byte ptr [esi],CDF_PLAYED
        jz      cdpl_no
        call    cdd_subq
        jc      cdpl_no
        cmp     byte ptr [cdd_sbuf+1],11h       ; audio play in progress
        jne     cdpl_stopped
        pop     esi
        pop     ebx
        pop     eax
        stc
        ret
cdpl_stopped:
        ; it has run to the end: stop asking until the next play
        movzx   ebx,byte ptr [cdd_unit]
        mov     esi,OFFSET cdd_fl
        add     esi,ebx
        test    byte ptr [esi],CDF_PAUSED
        jnz     cdpl_no                         ; paused is not finished
        and     byte ptr [esi],NOT CDF_PLAYED
cdpl_no:
        pop     esi
        pop     ebx
        pop     eax
        clc
        ret

;=====================================================================
; Addresses.  DOS programs name a sector in one of two notations and
; say which in the request, and the drive wants a third.
;
;   HSG       the logical block number, counting from zero
;   RED BOOK  minutes, seconds and frames as they are printed on the
;             sleeve, packed into a dword with the FRAME in the lowest
;             byte - which is the reverse of how anybody says it
;
; There are 75 frames to a second, and the two notations are offset by
; two seconds - 150 frames - because a CD's programme area starts
; where the lead-in stops.
;=====================================================================
CDD_FPS         EQU     75
CDD_FPM         EQU     4500            ; 75 * 60
CDD_PREGAP      EQU     150             ; the two seconds

;--- cdd_tolba - EAX in the notation ECX names -> EAX as a block
cdd_tolba:
        or      ecx,ecx
        jz      cdtl_out                ; HSG already is one
        push    ebx
        push    edx
        movzx   ebx,al                  ; frames
        mov     edx,eax
        shr     edx,8
        and     edx,0FFh                ; seconds
        imul    edx,CDD_FPS
        add     ebx,edx
        mov     edx,eax
        shr     edx,16
        and     edx,0FFh                ; minutes
        imul    edx,CDD_FPM
        add     ebx,edx
        sub     ebx,CDD_PREGAP
        mov     eax,ebx
        pop     edx
        pop     ebx
cdtl_out:
        ret

;--- cdd_toaddr - EAX = a block -> EAX in the notation ECX names
cdd_toaddr:
        or      ecx,ecx
        jz      cdta_out
        call    cdd_lba2rb
cdta_out:
        ret

;--- cdd_lba2rb - EAX = a block -> EAX = 00 MM SS FF
cdd_lba2rb:
        push    ebx
        push    edx
        add     eax,CDD_PREGAP
        xor     edx,edx
        mov     ebx,CDD_FPM
        div     ebx                     ; EAX = minutes, EDX = the rest
        shl     eax,16
        mov     ebx,eax                 ; EBX = MM in place
        mov     eax,edx
        xor     edx,edx
        mov     ecx,CDD_FPS
        push    ecx
        div     ecx                     ; EAX = seconds, EDX = frames
        pop     ecx
        shl     eax,8
        or      ebx,eax
        or      ebx,edx
        mov     eax,ebx
        pop     edx
        pop     ebx
        ret

;--- cdd_lba2msf - the same thing, and the same layout: the command
;--- block wants the three bytes separately and takes them from here.
cdd_lba2msf:
        call    cdd_lba2rb
        ret

;--- cdd_bcd - AL in place, binary to the packed decimal the Q channel
;--- carries.  Numbers above 99 cannot be spelled and are left alone,
;--- which is better than spelling them wrongly.
cdd_bcd:
        push    ebx
        push    edx
        cmp     al,99
        ja      cdbc_out
        movzx   eax,al
        xor     edx,edx
        mov     ebx,10
        div     ebx
        shl     al,4
        or      al,dl
cdbc_out:
        pop     edx
        pop     ebx
        ret

;--- cdd_findtrk - AL = a track number (or AAh for the lead-out) ->
;--- ESI = its descriptor in iso_toc.  CF if the disc has no such
;--- track.  cd_readtoc must have been called first.
cdd_findtrk:
        push    ecx
        push    edx
        movzx   edx,al
        movzx   ecx,byte ptr [iso_toc+ISO_TOC_LEN]
        shl     ecx,8
        mov     cl,byte ptr [iso_toc+ISO_TOC_LEN+1]
        cmp     ecx,2
        jbe     cdft_no
        sub     ecx,2
        shr     ecx,3
        jz      cdft_no
        mov     esi,OFFSET iso_toc + ISO_TOC_DESC
cdft_scan:
        movzx   eax,byte ptr [esi+ISO_TD_TRACK]
        cmp     eax,edx
        je      cdft_got
        add     esi,ISO_TD_LEN
        dec     ecx
        jnz     cdft_scan
cdft_no:
        pop     edx
        pop     ecx
        stc
        ret
cdft_got:
        pop     edx
        pop     ecx
        clc
        ret

;--- cdd_trklba - ESI = a track descriptor -> EAX = where it starts.
;--- Big-endian on the disc, as everything in a packet reply is.
cdd_trklba:
        movzx   eax,byte ptr [esi+ISO_TD_LBA+0]
        shl     eax,8
        mov     al,byte ptr [esi+ISO_TD_LBA+1]
        shl     eax,8
        mov     al,byte ptr [esi+ISO_TD_LBA+2]
        shl     eax,8
        mov     al,byte ptr [esi+ISO_TD_LBA+3]
        ret

;--- cdd_qlba - the absolute position out of the last cdd_subq, as a
;--- block.  The reply is in MSF because that is what was asked for.
cdd_qlba:
        push    ecx
        movzx   eax,byte ptr [cdd_sbuf+9]       ; minutes
        shl     eax,8
        mov     al,byte ptr [cdd_sbuf+10]       ; seconds
        shl     eax,8
        mov     al,byte ptr [cdd_sbuf+11]       ; frames
        mov     ecx,1                           ; ...which is Red Book
        call    cdd_tolba
        pop     ecx
        ret

;--- cdd_xferptr - EDI = a request -> EAX = the flat address of the
;--- buffer its transfer-address field names.
cdd_xferptr:
        push    edx
        mov     eax,dword ptr [edi+CDRQ_XFER]
        mov     edx,eax
        shr     edx,16                          ; the segment
        shl     edx,4
        movzx   eax,ax                          ; and the offset
        add     eax,edx
        pop     edx
        ret

;=====================================================================
; State
;=====================================================================
cdd_name    db 'MSCD001 '                ; eight bytes, blank padded, and
                                         ; the name every CD program looks
                                         ; for because MSCDEX's /D: switch
                                         ; defaulted to it
cdd_seg     dd 0                         ; the guest-visible block
cdd_reqp    dd 0                         ; the request being served
cdd_cb      dd 0                         ; ...and the buffer inside it
cdd_cnt     dd 0
cdd_from    dd 0                         ; a play, in blocks
cdd_to      dd 0
cdd_sense   dd 0                         ; the last packet's sense
cdd_stat    dw 0                         ; the status word being built
cdd_unit    db 0                         ; which ATAPI unit this request is
cdd_ecode   db 0                         ; ...and what went wrong with it
cdd_fl      db CDD_MAXUNIT dup (0)       ; per unit: played, paused, locked
cdd_pst     dd CDD_MAXUNIT dup (0)       ; where the last play started
cdd_pen     dd CDD_MAXUNIT dup (0)       ; ...and where it was to end
cdd_dcb     db 12 dup (0)                ; the command block
cdd_sbuf    db 24 dup (0)                ; a sub-channel reply
cdd_mbuf    db 32 dup (0)                ; a mode page, in and out
