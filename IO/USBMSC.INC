;=====================================================================
; USBMSC.INC - mass storage, bulk-only: USB sticks, card readers and
;              disks in enclosures, as the block layer's drives
;              USB_DRV0 upwards
;
; A USB disk is a SCSI disk at the far end of two bulk pipes.  The
; host sends a Command Block Wrapper - 31 bytes: a signature, a tag,
; how much data and which way, and the SCSI command block itself -
; down the OUT pipe, moves the data on whichever pipe the command
; needs, and reads a 13-byte Command Status Wrapper back off the IN
; pipe.  The commands are the ones every disk has: INQUIRY to learn
; what it is, READ CAPACITY for its size, READ(10) and WRITE(10) for
; the sectors, TEST UNIT READY and REQUEST SENSE for whether it is
; there and why not.  All of it is foreground work - the block layer
; asks for a sector, this asks the device, and polls the transfers to
; the end the way ATA.INC polls a drive.
;
; A UNIT is a slot in usb_units with a block-layer drive number of its
; own, USB_DRV0 and up, and a device attached to it or not.  A device
; takes the first free unit when it is configured (usbm_attach) and
; gives it back when it goes (usbm_detach); every attach counts the
; unit's GENERATION up, and that number is how PMDOS knows the disk
; behind a letter is not the one it mounted.  The medium itself is
; asked about on the first access and not before (usbm_probe): a stick
; plugged in and never touched costs nothing.
;
; THE STICK THE MACHINE BOOTED FROM is a special case.  The BIOS had
; it as drive 80h and PMDOS mounted C: on it through INT 13h; taking
; the controller ends that, so before the takeover usb_savebios keeps
; sector 0 of every BIOS hard disk the IDE driver does not own, and
; afterwards usb_adopt looks for the same sector on a USB unit.  A
; match means the unit answers for that BIOS number too (UN_ALIAS),
; and C: goes on without noticing.
;
; Status comes back as the BIOS INT 13h codes the block layer speaks,
; so that the critical-error handler says "Not ready" for an empty
; slot and "Write protect" for a stick with its switch on.
;
; Included by IOPM.ASM after USBHID.INC.
;=====================================================================

;--- the wrappers ----------------------------------------------------
CBW_SIG         EQU     43425355h       ; 'USBC'
CBW_TAG         EQU     04h             ; dd
CBW_LEN         EQU     08h             ; dd - bytes of data
CBW_FLAGS       EQU     0Ch             ; db - 80h: the device sends
CBW_LUN         EQU     0Dh             ; db
CBW_CDBLEN      EQU     0Eh             ; db
CBW_CDB         EQU     0Fh             ; 16 bytes
CBW_SIZE        EQU     31
CSW_SIG         EQU     53425355h       ; 'USBS'
CSW_TAG         EQU     04h             ; dd
CSW_RESIDUE     EQU     08h             ; dd
CSW_STATUS      EQU     0Ch             ; db - 0 good, 1 failed, 2 phase error
CSW_SIZE        EQU     13

;--- the commands ----------------------------------------------------
SC_TESTREADY    EQU     00h
SC_REQSENSE     EQU     03h
SC_INQUIRY      EQU     12h
SC_READCAP      EQU     25h
SC_READ10       EQU     28h
SC_WRITE10      EQU     2Ah

SK_NOTREADY     EQU     02h             ; sense keys
SK_MEDIUM       EQU     03h
SK_ILLEGAL      EQU     05h
SK_ATTENTION    EQU     06h
SK_PROTECT      EQU     07h

; what usbm_cmd answers in AL
MR_OK           EQU     0
MR_CHECK        EQU     1               ; the device failed it: UN_SENSE says why
MR_TRANSPORT    EQU     2               ; the pipes broke; reset and recovered
MR_GONE         EQU     3               ; nothing is there

MSC_READYTRIES  EQU     40              ; TEST UNIT READY, two seconds' worth
MSC_SECS_XFER   EQU     XFER_MAX / 512  ; sectors a READ(10) asks for at once
USBM_XSPIN      EQU     20000000        ; a bridged transfer polls with IF off,
                                        ; so the tick clock cannot time it out:
                                        ; bound the poll by this many turns (far
                                        ; more than any healthy transfer takes,
                                        ; a wedge gives up in well under a second)
MSC_BAND_LO     EQU     0A0000h         ; a buffer here is not where the bus
MSC_BAND_HI     EQU     DOS_BASE        ; thinks it is: see ATA.INC's ata_dmaok

;=====================================================================
; usbm_init - the units empty.  From usb_init.
;=====================================================================
usbm_init:
        push    ecx
        push    edi
        mov     edi,OFFSET usb_units
        mov     ecx,USB_MAXUNITS
umi_lp:
        mov     byte ptr [edi+UN_DEV],0FFh
        mov     byte ptr [edi+UN_FLAGS],0
        mov     byte ptr [edi+UN_GEN],0
        mov     byte ptr [edi+UN_ALIAS],0
        mov     dword ptr [edi+UN_SECS],0
        add     edi,UN_LEN
        loop    umi_lp
        mov     byte ptr [usbm_saved],0
        mov     byte ptr [usbm_adopted],0
        pop     edi
        pop     ecx
        ret

;--- unit_row - AL = a unit -> ESI -> its row
unit_row:
        movzx   esi,al
        imul    esi,UN_LEN
        add     esi,OFFSET usb_units
        ret

;--- unit_dev - ESI -> a unit -> EBX -> its device.  CF if none.
unit_dev:
        mov     al,[esi+UN_DEV]
        cmp     al,0FFh
        je      und_none
        call    dev_row
        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     und_none
        clc
        ret
und_none:
        stc
        ret

;=====================================================================
; usbm_attach - EBX -> a configured disk: the first free unit is its.
;               CF if none is.  Everything preserved.
; usbm_detach - EBX -> one that has gone: its unit is empty.
;=====================================================================
usbm_attach:
        pushad
        mov     esi,OFFSET usb_units
        xor     ecx,ecx
uma_lp:
        cmp     byte ptr [esi+UN_DEV],0FFh
        je      uma_got
        add     esi,UN_LEN
        inc     ecx
        cmp     ecx,USB_MAXUNITS
        jb      uma_lp
        popad
        stc
        ret
uma_got:
        call    dev_slot
        mov     [esi+UN_DEV],al
        mov     [ebx+USBD_UNIT],cl
        mov     byte ptr [esi+UN_FLAGS],USBUF_ATTACHED
        mov     byte ptr [esi+UN_LUN],0
        mov     byte ptr [esi+UN_ALIAS],0
        mov     byte ptr [esi+UN_SENSE],0
        mov     dword ptr [esi+UN_SECS],0
        mov     dword ptr [esi+UN_TAG],1
        inc     byte ptr [esi+UN_GEN]
        jnz     uma_gen
        inc     byte ptr [esi+UN_GEN]   ; never zero: zero is "never"
uma_gen:
        ; the model, blank until INQUIRY fills it
        lea     edi,[esi+UN_MODEL]
        mov     eax,20202020h
        mov     ecx,6
        rep     stosd
        popad
        clc
        ret

usbm_detach:
        pushad
        mov     al,[ebx+USBD_UNIT]
        cmp     al,USB_MAXUNITS
        jae     umd_out
        call    unit_row
        call    dev_slot
        cmp     [esi+UN_DEV],al
        jne     umd_out
        mov     byte ptr [esi+UN_DEV],0FFh
        mov     byte ptr [esi+UN_FLAGS],0
        mov     byte ptr [esi+UN_ALIAS],0
        mov     dword ptr [esi+UN_SECS],0
umd_out:
        popad
        ret

;=====================================================================
; usbm_funit - USF_UNIT.  DL = unit, EDI -> 24 bytes for the model ->
;              AL = USBUF_*, AH = generation, EBX = sectors, ECX =
;              device slot.  Nothing is read from the device.
; usbm_fprobe - USF_PROBE.  DL = unit -> the medium asked what it is:
;               AL = USBUF_*, EBX = sectors.  CF if it is not there or
;               would not say.
;=====================================================================
usbm_funit:
        push    esi
        cmp     dl,USB_MAXUNITS
        jae     umf_bad
        mov     al,dl
        call    unit_row
        cmp     byte ptr [esi+UN_ALIAS],0
        jne     umf_taken               ; the BIOS's drive, with letters of
                                        ; its own: the USB letter stays empty
        push    ecx
        push    esi
        add     esi,UN_MODEL
        mov     ecx,6
        rep     movsd
        pop     esi
        pop     ecx
        sub     edi,24
        mov     al,[esi+UN_FLAGS]
        mov     ah,[esi+UN_GEN]
        mov     ebx,dword ptr [esi+UN_SECS]
        movzx   ecx,byte ptr [esi+UN_DEV]
        pop     esi
        clc
        ret
umf_bad:
        pop     esi
        stc
        ret
umf_taken:
        push    ecx
        push    esi
        add     esi,UN_MODEL
        mov     ecx,6
        rep     movsd
        pop     esi
        pop     ecx
        sub     edi,24
        mov     al,[esi+UN_FLAGS]       ; as any unit; CH says which BIOS
        mov     ah,[esi+UN_GEN]         ; drive it stands in for, and the
        mov     ebx,dword ptr [esi+UN_SECS]     ; DOS side knows whether that
        movzx   ecx,byte ptr [esi+UN_DEV]       ; drive has letters
        mov     ch,[esi+UN_ALIAS]
        pop     esi
        clc
        ret

usbm_fprobe:
        push    esi
        cmp     dl,USB_MAXUNITS
        jae     umq_bad
        mov     al,dl
        call    unit_row
        call    usbm_probe
        jc      umq_bad
        mov     al,[esi+UN_FLAGS]
        mov     ebx,dword ptr [esi+UN_SECS]
        pop     esi
        clc
        ret
umq_bad:
        pop     esi
        stc
        ret

;=====================================================================
; usbm_probeall - every unit with a device on it and no answer yet is
;                 asked.  From usb_start.
;=====================================================================
usbm_probeall:
        pushad
        mov     esi,OFFSET usb_units
        mov     ecx,USB_MAXUNITS
ump_lp:
        test    byte ptr [esi+UN_FLAGS],USBUF_ATTACHED
        jz      ump_next
        test    byte ptr [esi+UN_FLAGS],USBUF_READY
        jnz     ump_next
        call    usbm_probe
ump_next:
        add     esi,UN_LEN
        loop    ump_lp
        popad
        ret

;=====================================================================
; usbm_probe - ESI -> a unit: ready, or made so.  TEST UNIT READY until
;              the medium answers, INQUIRY for what it is, READ
;              CAPACITY for how big.  CF if there is no device, or it
;              is not a disk this kernel can use, or it would not say.
;              Everything but flags preserved.
;=====================================================================
usbm_probe:
        pushad
        test    byte ptr [esi+UN_FLAGS],USBUF_ATTACHED
        jz      upr_fail
        test    byte ptr [esi+UN_FLAGS],USBUF_READY
        jnz     upr_ok
        test    byte ptr [esi+UN_FLAGS],USBUF_NOTDISK
        jnz     upr_fail
        call    unit_dev
        jc      upr_fail

        ; TEST UNIT READY, patiently: a stick just plugged in says NOT
        ; READY for a while, and UNIT ATTENTION once
        mov     ebp,MSC_READYTRIES
upr_ready:
        call    usbm_cdbclear
        mov     byte ptr [usbm_cdblen],6
        xor     ecx,ecx
        mov     dl,2
        call    usbm_cmd
        cmp     al,MR_OK
        je      upr_isready
        cmp     al,MR_GONE
        je      upr_fail
        cmp     al,MR_TRANSPORT
        je      upr_again
        ; a check condition: not ready yet is the expected one
        mov     al,[esi+UN_SENSE]
        cmp     al,SK_NOTREADY
        je      upr_again
        cmp     al,SK_ATTENTION
        je      upr_again
        jmp     upr_fail
upr_again:
        dec     ebp
        jz      upr_fail
        mov     ecx,1
        call    usb_waitticks
        jmp     upr_ready

upr_isready:
        ; INQUIRY: what it is, and its name
        call    usbm_cdbclear
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_CDB
        mov     byte ptr [edi],SC_INQUIRY
        mov     byte ptr [edi+4],36
        mov     byte ptr [usbm_cdblen],6
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_INQ
        mov     ecx,36
        mov     dl,1
        call    usbm_cmd
        cmp     al,MR_OK
        jne     upr_fail
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_INQ
        mov     al,[edi]
        and     al,1Fh                  ; the peripheral device type
        jz      upr_isdisk              ; direct access
        cmp     al,0Eh                  ; simplified direct access
        je      upr_isdisk
        or      byte ptr [esi+UN_FLAGS],USBUF_NOTDISK
        jmp     upr_fail
upr_isdisk:
        push    esi
        lea     edi,[esi+UN_MODEL]
        mov     esi,dword ptr [usb_scr]
        add     esi,SCR_INQ + 8         ; vendor, eight; product, sixteen
        mov     ecx,24
upr_model:
        lodsb
        cmp     al,' '
        jb      upr_ctl
        cmp     al,7Eh
        jbe     upr_mok
upr_ctl:
        mov     al,' '
upr_mok:
        stosb
        loop    upr_model
        pop     esi

        ; READ CAPACITY: the last block and the block size, big-endian
        call    usbm_cdbclear
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_CDB
        mov     byte ptr [edi],SC_READCAP
        mov     byte ptr [usbm_cdblen],10
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_INQ
        mov     ecx,8
        mov     dl,1
        call    usbm_cmd
        cmp     al,MR_OK
        jne     upr_fail
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_INQ
        mov     eax,dword ptr [edi+4]
        xchg    al,ah                   ; big-endian to little, on a 386
        rol     eax,16
        xchg    al,ah
        cmp     eax,512
        je      upr_512
        or      byte ptr [esi+UN_FLAGS],USBUF_NOTDISK
        jmp     upr_fail
upr_512:
        mov     byte ptr [esi+UN_BPSLOG],9
        mov     eax,dword ptr [edi]
        xchg    al,ah                   ; big-endian to little, on a 386
        rol     eax,16
        xchg    al,ah
        inc     eax
        jz      upr_fail                ; FFFFFFFFh is "bigger than I can
                                        ; say", and bigger than a FAT
        mov     dword ptr [esi+UN_SECS],eax
        or      byte ptr [esi+UN_FLAGS],USBUF_READY
upr_ok:
        popad
        clc
        ret
upr_fail:
        popad
        stc
        ret

;--- usbm_cdbclear - sixteen zeros at SCR_CDB
usbm_cdbclear:
        push    eax
        push    ecx
        push    edi
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_CDB
        xor     eax,eax
        mov     ecx,4
        rep     stosd
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; usbm_cmd - one SCSI command, bulk-only: ESI -> unit, the command at
;            SCR_CDB with usbm_cdblen bytes, EDI = the data buffer, ECX
;            = its length, DL = 0 host to device, 1 device to host, 2
;            no data.  -> AL = MR_*.  On MR_CHECK the sense key and
;            code are in UN_SENSE and UN_ASC.  usbm_moved = the data
;            bytes that moved; on MR_OK a short answer is made up to
;            ECX with zeros.  Everything else preserved.
;=====================================================================
usbm_cmd:
        pushad
        mov     dword ptr [usbm_buf],edi
        mov     dword ptr [usbm_len],ecx
        mov     byte ptr [usbm_dir],dl
        call    unit_dev                ; -> EBX
        jc      ucm_gone

        ; the CBW
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_CBW
        mov     dword ptr [edi],CBW_SIG
        mov     eax,dword ptr [esi+UN_TAG]
        inc     dword ptr [esi+UN_TAG]
        mov     dword ptr [edi+CBW_TAG],eax
        mov     dword ptr [usbm_tag],eax
        mov     eax,dword ptr [usbm_len]
        mov     dword ptr [edi+CBW_LEN],eax
        xor     al,al
        cmp     byte ptr [usbm_dir],1
        jne     ucm_flags
        mov     al,80h
ucm_flags:
        mov     [edi+CBW_FLAGS],al
        mov     al,[esi+UN_LUN]
        mov     [edi+CBW_LUN],al
        mov     al,byte ptr [usbm_cdblen]
        mov     [edi+CBW_CDBLEN],al
        push    esi
        push    edi
        add     edi,CBW_CDB
        mov     esi,dword ptr [usb_scr]
        add     esi,SCR_CDB
        mov     ecx,4
        rep     movsd
        pop     edi
        pop     esi
        ; out it goes
        xor     dl,dl
        mov     ecx,CBW_SIZE
        call    usbm_xfer
        cmp     al,XS_DONE
        jne     ucm_broken

        ; the data, in pieces the pool can carry, through the bounce
        ; buffer when the caller's buffer is not where the bus thinks
        mov     dword ptr [usbm_moved],0
        cmp     byte ptr [usbm_dir],2
        je      ucm_csw
        cmp     dword ptr [usbm_len],0
        je      ucm_csw
        mov     edi,dword ptr [usbm_buf]
        mov     ecx,dword ptr [usbm_len]
ucm_data:
        or      ecx,ecx
        jz      ucm_csw
        mov     ebp,ecx
        cmp     ebp,XFER_MAX
        jbe     ucm_piece
        mov     ebp,XFER_MAX
ucm_piece:
        ; across the band?  Then at most a bounce buffer's worth
        mov     eax,edi
        cmp     eax,MSC_BAND_LO
        jb      ucm_below
        cmp     eax,MSC_BAND_HI
        jae     ucm_direct
ucm_inband:
        cmp     ebp,1000h
        jbe     ucm_bounce
        mov     ebp,1000h
ucm_bounce:
        push    edi
        push    ecx
        mov     edx,dword ptr [usb_scr]
        add     edx,SCR_BOUNCE
        cmp     byte ptr [usbm_dir],0
        jne     ucm_bin
        push    esi
        mov     esi,edi
        mov     edi,edx
        mov     ecx,ebp
        rep     movsb                   ; out: the data into the bounce
        pop     esi
ucm_bin:
        mov     edi,edx
        mov     ecx,ebp
        mov     dl,byte ptr [usbm_dir]
        call    usbm_xfer
        mov     edx,ecx                 ; EDX = what moved
        pop     ecx
        pop     edi
        call    usbm_tally
        cmp     byte ptr [usbm_dir],1
        jne     ucm_bounced
        push    esi
        push    ecx
        mov     esi,dword ptr [usb_scr]
        add     esi,SCR_BOUNCE
        mov     ecx,edx
        rep     movsb                   ; in: what came, back out of it
        pop     ecx                     ; (EDI moves)
        pop     esi
        cmp     al,XS_DONE
        jne     ucm_dataerr
        cmp     edx,ebp
        jb      ucm_csw                 ; short: the device has no more
        sub     ecx,ebp
        jmp     ucm_data
ucm_bounced:
        cmp     al,XS_DONE
        jne     ucm_dataerr
        add     edi,ebp
        sub     ecx,ebp
        jmp     ucm_data
ucm_direct:
        push    ecx
        mov     ecx,ebp
        mov     dl,byte ptr [usbm_dir]
        call    usbm_xfer
        mov     edx,ecx                 ; EDX = what moved
        pop     ecx
        call    usbm_tally
        cmp     al,XS_DONE
        jne     ucm_dataerr
        cmp     edx,ebp
        jb      ucm_csw                 ; short: the device has no more
        add     edi,ebp
        sub     ecx,ebp
        jmp     ucm_data
ucm_below:
        ; below it but running into it: direct up to the last whole
        ; sector short of it, and the bounce buffer across it
        add     eax,ebp
        cmp     eax,MSC_BAND_LO
        jbe     ucm_direct
        mov     eax,MSC_BAND_LO
        sub     eax,edi
        and     eax,0FFFFFE00h
        jz      ucm_inband
        mov     ebp,eax
        jmp     ucm_direct

ucm_dataerr:
        ; a stall on the data pipe is the device's way of ending the
        ; data stage early; the status still comes, once the pipe is
        ; cleared.  Anything else is the transport broken
        cmp     ah,USBE_STALL
        jne     ucm_broken
        mov     dl,[ebx+USBD_EPOUT]
        cmp     byte ptr [usbm_dir],0
        je      ucm_clear
        mov     dl,[ebx+USBD_EPIN]
        or      dl,80h
ucm_clear:
        call    usbm_clearhalt
        jc      ucm_broken

ucm_csw:
        ; the CSW, and a stalled status pipe is cleared and asked once
        ; more before the transport is called broken
        mov     ebp,2
ucm_cswtry:
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_CSW
        mov     ecx,CSW_SIZE
        mov     dl,1
        call    usbm_xfer
        cmp     al,XS_DONE
        je      ucm_haveCSW
        cmp     ah,USBE_STALL
        jne     ucm_broken
        mov     dl,[ebx+USBD_EPIN]
        or      dl,80h
        call    usbm_clearhalt
        jc      ucm_broken
        dec     ebp
        jnz     ucm_cswtry
        jmp     ucm_broken
ucm_haveCSW:
        cmp     ecx,CSW_SIZE
        jb      ucm_broken
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_CSW
        cmp     dword ptr [edi],CSW_SIG
        jne     ucm_broken
        mov     eax,dword ptr [edi+CSW_TAG]
        cmp     eax,dword ptr [usbm_tag]
        jne     ucm_broken
        mov     al,[edi+CSW_STATUS]
        or      al,al
        jz      ucm_good
        cmp     al,1
        jne     ucm_broken
        ; the command failed: ask why, unless this was the asking
        cmp     byte ptr [usbm_sensing],0
        jne     ucm_check
        call    usbm_sense
ucm_check:
        popad
        mov     al,MR_CHECK
        ret
ucm_good:
        ; a short answer: the rest of the buffer zeros, not whatever
        ; was there before
        cmp     byte ptr [usbm_dir],1
        jne     ucm_ok
        mov     ecx,dword ptr [usbm_len]
        mov     edi,dword ptr [usbm_moved]
        sub     ecx,edi
        jbe     ucm_ok
        add     edi,dword ptr [usbm_buf]
        xor     al,al
        rep     stosb
ucm_ok:
        popad
        mov     al,MR_OK
        ret
ucm_broken:
        ; reset the transport and clear both pipes; the caller may try
        ; again
        call    usbm_reset
        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     ucm_gone
        popad
        mov     al,MR_TRANSPORT
        ret
ucm_gone:
        popad
        mov     al,MR_GONE
        ret

;--- usbm_tally - EDX = what a piece moved, EBP = what it asked for:
;--- EDX held to that, and added to usbm_moved
usbm_tally:
        cmp     edx,ebp
        jbe     umt_add
        mov     edx,ebp
umt_add:
        add     dword ptr [usbm_moved],edx
        ret

;---------------------------------------------------------------------
; usbm_xfer - one bulk transfer, start to finish: EBX -> device, DL =
;             0 out / 1 in, EDI = buffer, ECX = bytes -> AL = XS_*, AH,
;             ECX = bytes moved.
;---------------------------------------------------------------------
usbm_xfer:
        call    usb_blk_start
        jc      umx_nostart
        mov     dword ptr [usbm_spinleft],USBM_XSPIN
umx_wait:
        cmp     byte ptr [usb_brgwin],0
        jne     umx_bounded             ; a bridged transfer: leave interrupts
        sti                             ; off.  The bridge window runs under the
        jmp     short umx_poll          ; kernel's CR3 with WIN386's IDT out of
                                        ; reach, so an interrupt delivered mid
                                        ; poll triple-faults; blk_bridge both
                                        ; masks the PICs and sets usb_brgwin.
umx_bounded:
        ; interrupts stay off here, so usb_blk_poll's tick timeout cannot
        ; fire (the tick clock is frozen with IF off); bound the poll by a
        ; count instead, so a wedged device fails the transfer rather than
        ; spinning for ever and freezing Windows.
        dec     dword ptr [usbm_spinleft]
        jz      umx_spinout
umx_poll:
        call    usb_blk_poll
        cmp     al,XS_PENDING
        je      umx_wait
        ret
umx_spinout:
        call    usb_blk_abort           ; stop and free the stuck transfer
        mov     al,XS_ERROR
        mov     ah,USBE_TIMEOUT
        xor     ecx,ecx
        ret
umx_nostart:
        mov     al,XS_ERROR
        xor     ecx,ecx
        ret

;---------------------------------------------------------------------
; usbm_clearhalt - EBX -> device, DL = endpoint address (bit 7 for IN):
;                  CLEAR_FEATURE(ENDPOINT_HALT), and the toggle starts
;                  over.  CF if the request itself failed.
;---------------------------------------------------------------------
usbm_clearhalt:
        pushad
        mov     dh,dl
        movzx   edx,dl
        mov     al,02h                  ; standard, to an endpoint
        mov     ah,UREQ_CLRFEAT
        mov     cx,UFEAT_EPHALT
        xor     di,di
        push    edx
        call    usb_setup
        pop     edx
        xor     ecx,ecx
        call    usb_ctl_sync
        cmp     al,XS_DONE
        jne     uch_fail
        test    dl,80h
        jz      uch_out
        mov     byte ptr [ebx+USBD_TOGIN],0
        popad
        clc
        ret
uch_out:
        mov     byte ptr [ebx+USBD_TOGOUT],0
        popad
        clc
        ret
uch_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; usbm_reset - EBX -> device: Bulk-Only Mass Storage Reset, then both
;              pipes cleared.  What the specification says to do when
;              the wrappers stop making sense.
;---------------------------------------------------------------------
usbm_reset:
        pushad
        movzx   edx,byte ptr [ebx+USBD_IFACE]
        mov     al,21h
        mov     ah,UREQ_BOTRESET
        xor     cx,cx
        xor     di,di
        call    usb_setup
        xor     ecx,ecx
        call    usb_ctl_sync
        mov     dl,[ebx+USBD_EPIN]
        or      dl,80h
        call    usbm_clearhalt
        mov     dl,[ebx+USBD_EPOUT]
        call    usbm_clearhalt
        popad
        ret

;---------------------------------------------------------------------
; usbm_sense - ESI -> unit: REQUEST SENSE, into UN_SENSE and UN_ASC.
;              Zeros if the device would not say.
;---------------------------------------------------------------------
usbm_sense:
        pushad
        mov     byte ptr [esi+UN_SENSE],0
        mov     byte ptr [esi+UN_ASC],0
        mov     byte ptr [usbm_sensing],1
        call    usbm_cdbclear
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_CDB
        mov     byte ptr [edi],SC_REQSENSE
        mov     byte ptr [edi+4],18
        mov     byte ptr [usbm_cdblen],6
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_INQ
        mov     ecx,18
        mov     dl,1
        call    usbm_cmd
        mov     byte ptr [usbm_sensing],0
        cmp     al,MR_OK
        jne     ums_out
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_INQ
        mov     al,[edi+2]
        and     al,0Fh
        mov     [esi+UN_SENSE],al
        mov     al,[edi+12]
        mov     [esi+UN_ASC],al
ums_out:
        popad
        ret

;=====================================================================
; THE BLOCK LAYER'S SIDE.  AL = a drive number, as BLOCK.INC has them.
;
;   usbm_owns      CF clear if a unit answers for it
;   usbm_read      EBX = LBA, CX = sectors, EDI = buffer.  CF and AH =
;   usbm_write     an INT 13h status if not
;   usbm_capacity  -> EBX = sectors.  CF if it is not there
;   usbm_geo       -> CX = sectors a track, DX = heads, EBX = sectors
;                  those two reach, for the INT 13h view of it
;=====================================================================

;--- usbm_unitof - AL = a drive -> ESI -> its unit, or CF.  A unit
;--- answers for its own number and for the BIOS drive it took over.
usbm_unitof:
        push    eax
        push    ecx
        cmp     al,USB_DRV0
        jb      umu_alias
        cmp     al,USB_DRV0 + USB_MAXUNITS
        jae     umu_alias
        sub     al,USB_DRV0
        call    unit_row
        test    byte ptr [esi+UN_FLAGS],USBUF_ATTACHED
        jz      umu_no
        pop     ecx
        pop     eax
        clc
        ret
umu_alias:
        or      al,al
        jz      umu_no
        mov     esi,OFFSET usb_units
        mov     ecx,USB_MAXUNITS
umu_lp:
        cmp     [esi+UN_ALIAS],al
        jne     umu_next
        test    byte ptr [esi+UN_FLAGS],USBUF_ATTACHED
        jnz     umu_yes
umu_next:
        add     esi,UN_LEN
        loop    umu_lp
umu_no:
        pop     ecx
        pop     eax
        stc
        ret
umu_yes:
        pop     ecx
        pop     eax
        clc
        ret

usbm_owns:
        push    esi
        cmp     byte ptr [usb_started],0
        je      umo_no
        call    usbm_unitof
        jnc     umo_yes
        ; A USB NUMBER WITH NOTHING IN THE SLOT IS STILL OURS: the BIOS
        ; has no drive 82h, and the answer for an empty slot is "not
        ; ready" from usbm_rw, as it is for an empty A:, so that a
        ; letter kept for a stick says so instead of "invalid drive"
        cmp     al,USB_DRV0
        jb      umo_notusb
        cmp     al,USB_DRV0 + USB_MAXUNITS
        jb      umo_yes
umo_notusb:
        ; A BIOS DRIVE THAT WAS A USB DISK STAYS OURS WHEN THE DISK GOES.
        ; The BIOS cannot reach its controller any more, and a call into
        ; it for drive 81h would sit in the BIOS's own USB code for ever:
        ; the answer is "not ready", from usbm_read, until a disk with
        ; that sector 0 is plugged in again
        call    usbm_wasbios
        jc      umo_no
umo_yes:
        pop     esi
        clc
        ret
umo_no:
        pop     esi
        stc
        ret

;--- usbm_wasbios - CF clear if AL is a BIOS drive a unit once took over
usbm_wasbios:
        cmp     al,80h
        jne     uwb_81
        test    byte ptr [usbm_adopted],1
        jz      uwb_no
        clc
        ret
uwb_81:
        cmp     al,81h
        jne     uwb_no
        test    byte ptr [usbm_adopted],2
        jz      uwb_no
        clc
        ret
uwb_no:
        stc
        ret

usbm_capacity:
        push    esi
        cmp     byte ptr [usb_started],0
        je      umc_no
        call    usbm_unitof
        jc      umc_no
        call    usbm_probe
        jc      umc_no
        mov     ebx,dword ptr [esi+UN_SECS]
        pop     esi
        clc
        ret
umc_no:
        pop     esi
        stc
        ret

;--- 63 sectors a track; 16 heads until that runs out at 504MB, then
;--- 255, which is what every translating BIOS did; and the cylinders
;--- stop at 1024, as INT 13h's do
usbm_geo:
        push    esi
        push    eax
        call    usbm_capacity           ; -> EBX
        jc      umg_no
        mov     ecx,63
        mov     edx,16
        cmp     ebx,1024 * 16 * 63
        jbe     umg_heads
        mov     edx,255
umg_heads:
        mov     eax,ebx
        push    edx
        push    ecx
        imul    ecx,edx                 ; sectors a cylinder
        push    ecx
        xor     edx,edx
        div     ecx                     ; EAX = cylinders
        cmp     eax,1024
        jbe     umg_cyls
        mov     eax,1024
umg_cyls:
        pop     ecx
        imul    eax,ecx
        mov     ebx,eax
        pop     ecx
        pop     edx
        pop     eax
        pop     esi
        clc
        ret
umg_no:
        pop     eax
        pop     esi
        stc
        ret

usbm_read:
        mov     byte ptr [usbm_iswrite],0
        jmp     usbm_rw
usbm_write:
        mov     byte ptr [usbm_iswrite],1
usbm_rw:
        pushad
        cmp     byte ptr [usb_started],0
        je      urw_nodrive
        call    usbm_unitof
        jc      urw_nodrive
        call    usbm_probe
        jc      urw_notready
        movzx   ecx,cx
        mov     dword ptr [usbm_lba],ebx
        mov     dword ptr [usbm_count],ecx
        mov     dword ptr [usbm_rwbuf],edi
        ; inside the medium?
        mov     eax,ebx
        add     eax,ecx
        jc      urw_badsec
        cmp     eax,dword ptr [esi+UN_SECS]
        ja      urw_badsec
urw_lp:
        mov     ecx,dword ptr [usbm_count]
        or      ecx,ecx
        jz      urw_done
        cmp     ecx,MSC_SECS_XFER
        jbe     urw_have
        mov     ecx,MSC_SECS_XFER
urw_have:
        mov     ebp,ecx                 ; EBP = sectors this command
        ; READ(10) or WRITE(10): the LBA and the count big-endian
        call    usbm_cdbclear
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_CDB
        mov     al,SC_READ10
        cmp     byte ptr [usbm_iswrite],0
        je      urw_op
        mov     al,SC_WRITE10
urw_op:
        mov     [edi],al
        mov     eax,dword ptr [usbm_lba]
        xchg    al,ah                   ; big-endian to little, on a 386
        rol     eax,16
        xchg    al,ah
        mov     dword ptr [edi+2],eax
        mov     eax,ebp
        xchg    al,ah
        mov     word ptr [edi+7],ax
        mov     byte ptr [usbm_cdblen],10
        mov     edi,dword ptr [usbm_rwbuf]
        mov     ecx,ebp
        shl     ecx,9
        mov     dl,1
        cmp     byte ptr [usbm_iswrite],0
        je      urw_go
        xor     dl,dl
urw_go:
        mov     byte ptr [usbm_tries],3
urw_try:
        call    usbm_cmd
        cmp     al,MR_OK
        je      urw_next
        cmp     al,MR_GONE
        je      urw_gone
        cmp     al,MR_TRANSPORT
        jne     urw_sense
        dec     byte ptr [usbm_tries]
        jnz     urw_try
        jmp     urw_timeout
urw_sense:
        ; a check condition: what kind
        mov     al,[esi+UN_SENSE]
        cmp     al,SK_ATTENTION
        je      urw_changed
        cmp     al,SK_NOTREADY
        je      urw_nomedia
        cmp     al,SK_PROTECT
        je      urw_protected
        cmp     al,SK_MEDIUM
        je      urw_crc
        cmp     al,SK_ILLEGAL
        je      urw_illegal
        jmp     urw_general
urw_next:
        mov     eax,ebp
        shl     eax,9
        cmp     dword ptr [usbm_moved],eax
        jne     urw_general             ; not every sector came, or went
        add     dword ptr [usbm_lba],ebp
        sub     dword ptr [usbm_count],ebp
        add     dword ptr [usbm_rwbuf],eax
        jmp     urw_lp
urw_done:
        popad
        xor     ah,ah
        clc
        ret

; the failures, as INT 13h status codes: CRITERR.INC's table turns
; them into Not ready, Write protect, Data error and General failure
urw_nodrive:
        mov     ah,31h                  ; an empty USB slot: no medium
        cmp     al,USB_DRV0
        jb      urw_nd_none
        cmp     al,USB_DRV0 + USB_MAXUNITS
        jb      urw_fail
urw_nd_none:
        mov     ah,0FFh                 ; a BIOS drive whose disk went
        jmp     urw_fail
urw_gone:
        mov     ah,0FFh
        jmp     urw_fail
urw_notready:
        mov     ah,31h                  ; no medium, or not a disk
        jmp     urw_fail
urw_nomedia:
        and     byte ptr [esi+UN_FLAGS],NOT USBUF_READY  ; ask it again next
        mov     ah,31h                                    ; time
        jmp     urw_fail
urw_changed:
        and     byte ptr [esi+UN_FLAGS],NOT USBUF_READY
        mov     ah,06h                  ; media changed
        jmp     urw_fail
urw_protected:
        or      byte ptr [esi+UN_FLAGS],USBUF_WRPROT
        mov     ah,03h
        jmp     urw_fail
urw_crc:
        mov     ah,10h
        jmp     urw_fail
urw_illegal:
        mov     ah,01h
        jmp     urw_fail
urw_badsec:
        mov     ah,04h
        jmp     urw_fail
urw_timeout:
        mov     ah,80h
        jmp     urw_fail
urw_general:
        mov     ah,0Ch
urw_fail:
        mov     [esp+28],eax            ; EAX, through the pushad frame:
        popad                           ; AH is the status, AL is noise
        stc
        ret

;=====================================================================
; THE BOOT STICK.
;
; usb_savebios - before the takeover: sector 0 of the BIOS's hard disks
;                the IDE driver does not drive, through the BIOS, into
;                SCR_SAVE0 and SCR_SAVE1.  From usb_start.
; usb_adopt    - after it: a unit whose sector 0 is one of those takes
;                the BIOS drive's number as well (UN_ALIAS).
;=====================================================================
usb_savebios:
        pushad
        mov     byte ptr [usbm_saved],0
        mov     al,80h
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_SAVE0
        mov     dl,1
        call    usv_one
        mov     al,81h
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_SAVE1
        mov     dl,2
        call    usv_one
        popad
        ret

;--- usv_one - AL = the drive, EDI -> 512 bytes, DL = its bit
usv_one:
        push    eax
        push    ebx
        push    ecx
        call    ata_owns
        jnc     uso_out                 ; the IDE driver's: not the BIOS's
        call    ahci_owns
        jnc     uso_out                 ; nor the SATA one's, whose port
                                        ; the BIOS must not be sent to
        call    blk_getgeo              ; -> EBX = its geometry record
        jc      uso_out
        ; the capacity the BIOS gave it is a key as well: a stick that
        ; has never been formatted has an all-zero sector 0, which is
        ; nothing to recognise it by, and its size is all there is
        push    eax
        mov     eax,dword ptr [ebx+GEO_LBATOT]
        or      eax,eax
        jz      uso_nosize
        cmp     dl,1
        jne     uso_size81
        mov     dword ptr [usbm_biostot],eax
        jmp     uso_sized
uso_size81:
        mov     dword ptr [usbm_biostot+4],eax
uso_sized:
        mov     al,dl
        shl     al,2                    ; bits 2 and 3: the size is known
        or      byte ptr [usbm_saved],al
uso_nosize:
        pop     eax
        xor     ebx,ebx
        mov     cx,1
        call    blk_read
        jc      uso_out
        ; all zeros would match anything: not a sector to recognise by
        push    edi
        mov     ecx,128
        xor     eax,eax
        repe    scasd
        pop     edi
        je      uso_out
        or      byte ptr [usbm_saved],dl
uso_out:
        pop     ecx
        pop     ebx
        pop     eax
        ret

usb_adopt:
        pushad
        cmp     byte ptr [usbm_saved],0
        je      uad_out
        mov     esi,OFFSET usb_units
        xor     ebp,ebp                 ; EBP = the unit
uad_unit:
        test    byte ptr [esi+UN_FLAGS],USBUF_READY
        jz      uad_next
        lea     eax,[ebp+USB_DRV0]
        xor     ebx,ebx
        mov     cx,1
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_SECT
        call    usbm_read
        jc      uad_next
        ; 80h: the same sector 0, or failing that the same size
        test    byte ptr [usbm_saved],1
        jz      uad_size80
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_SAVE0
        call    uad_same
        je      uad_is80
uad_size80:
        test    byte ptr [usbm_saved],4
        jz      uad_try81
        mov     eax,dword ptr [esi+UN_SECS]
        cmp     eax,dword ptr [usbm_biostot]
        jne     uad_try81
uad_is80:
        mov     byte ptr [esi+UN_ALIAS],80h
        and     byte ptr [usbm_saved],NOT 5
        or      byte ptr [usbm_adopted],1
        jmp     uad_next
uad_try81:
        test    byte ptr [usbm_saved],2
        jz      uad_size81
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_SAVE1
        call    uad_same
        je      uad_is81
uad_size81:
        test    byte ptr [usbm_saved],8
        jz      uad_next
        mov     eax,dword ptr [esi+UN_SECS]
        cmp     eax,dword ptr [usbm_biostot+4]
        jne     uad_next
uad_is81:
        mov     byte ptr [esi+UN_ALIAS],81h
        and     byte ptr [usbm_saved],NOT 0Ah
        or      byte ptr [usbm_adopted],2
uad_next:
        add     esi,UN_LEN
        inc     ebp
        cmp     ebp,USB_MAXUNITS
        jb      uad_unit
uad_out:
        popad
        ret

;--- uad_same - ZF if the 512 bytes at EDI are those at SCR_SECT
uad_same:
        push    esi
        push    edi
        push    ecx
        mov     esi,dword ptr [usb_scr]
        add     esi,SCR_SECT
        mov     ecx,128
        repe    cmpsd
        pop     ecx
        pop     edi
        pop     esi
        ret

;=====================================================================
; State
;=====================================================================
usb_units   db  USB_MAXUNITS * UN_LEN dup(0)

usbm_cdblen db  0
usbm_dir    db  0
usbm_iswrite db 0
usbm_sensing db 0                       ; REQUEST SENSE is out: do not ask
                                        ; why REQUEST SENSE failed
usbm_tries  db  0
usbm_saved  db  0                       ; bit 0: SCR_SAVE0 holds 80h's
usbm_biostot dd  0,0                    ; ...and their capacities, for a
                                        ; stick with nothing in sector 0
                                        ; sector 0, bit 1: SCR_SAVE1 81h's
usbm_adopted db 0                       ; the same bits: that drive was a
                                        ; USB disk, and the BIOS is never
                                        ; asked for it again
            db  0
usbm_buf    dd  0
usbm_len    dd  0
usbm_moved  dd  0                       ; of usbm_len, what the device took
                                        ; or gave
usbm_tag    dd  0
usbm_lba    dd  0
usbm_count  dd  0
usbm_rwbuf  dd  0
usbm_spinleft dd 0                      ; bridged-transfer poll bound (no tick
                                        ; clock to time out against with IF off)
                                        ; - named apart from the USBM_XSPIN
                                        ; equate, which wasm folds case-blind
