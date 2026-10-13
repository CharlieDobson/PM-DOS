;=====================================================================
; USBDRV.INC - the DOS side of USB disks: a letter for each, where the
;              volume on one is, and what happens when the disk changes
;
; DOS never had hot-plugging of DEVICES, only of MEDIA, and this treats
; a USB disk the way DOS treated the one removable thing it had.  The
; LETTER is fixed: USB= in CONFIG.SYS says how many, they are given out
; after every other letter (drv_addusb), and they are there whether or
; not anything is plugged in - as A: is.  What comes and goes is the
; medium behind the letter, and the kernel already knows how to live
; with that: a volume is mounted when the letter is touched, forgotten
; when the medium goes, and an empty slot answers "Not ready", as an
; empty diskette drive does.
;
; THREE THINGS ARE NEW, and they are all here.  A letter with no fixed
; partition behind it: where the volume is on the disk that is there
; now is worked out from that disk's sector 0 when it is mounted
; (usb_locate) - a partition table and its first FAT partition, or a
; boot sector and the whole disk, which is how sticks come.  A way to
; tell that the disk is not the one that was mounted: PMIO counts
; every attach on a unit, and a letter whose count has moved on is
; unmounted and its caches dropped before anything reads them
; (usb_chkmedia, from vol_select, where iso_chkmedia does the same for
; a changed disc).  And files left open on a disk that has gone: their
; handles are closed from under the program (sft_dropdrv), which is
; what DOS 4's "Invalid media change" came down to, so that a stick
; put in after another was pulled is never written through the first
; one's file table.
;
; The controllers are taken over from the BIOS when PMDOS asks, after
; CONFIG.SYS (usb_start): until then the BIOS's own USB keyboard and
; disk emulation is what the machine runs on, and USB=OFF leaves it
; that way.  Safe mode leaves it that way too, so that a keyboard the
; kernel's driver cannot handle is still a keyboard.
;
; Included by DOSMAIN.ASM after CACHE.INC.
;=====================================================================

;=====================================================================
; usb_start - after CONFIG.SYS: the controllers taken, the letters
;             given out, the devices present announced.
;=====================================================================
usb_start:
        pushad
IFDEF HANG_E9
        mov     al,'U'
        out     0E9h,al
ENDIF
        cmp     byte ptr [usb_on],0
        je      usx_out
        mov     al,USF_COUNT
        call    dword ptr ds:[IOS_USB]
        jecxz   usx_out                 ; no controller to drive
        ; the memory the schedules live in, 4K aligned, out of the arena
        ; and never given back
        mov     ebx,USB_MEMNEED + 1000h
        call    arena_alloc
        jc      usx_out
        add     eax,0FFFh
        and     eax,NOT 0FFFh
        mov     ebx,eax
        mov     al,USF_START
        call    dword ptr ds:[IOS_USB]
        jc      usx_out
        call    drv_addusb
        call    usb_announce
usx_out:
IFDEF HANG_E9
        mov     al,'u'
        out     0E9h,al
ENDIF
        popad
        ret

;---------------------------------------------------------------------
; drv_addusb - usb_letters letters for the USB units, after everything
;              the scan and CDROM= placed.  A letter the table has no
;              room for is not given out.
;---------------------------------------------------------------------
drv_addusb:
        pushad
        movzx   ecx,byte ptr [usb_letters]
        cmp     ecx,USB_MAXUNITS
        jbe     dau_count
        mov     ecx,USB_MAXUNITS
dau_count:
        xor     ebp,ebp                 ; EBP = the unit
dau_lp:
        cmp     ebp,ecx
        jae     dau_out
        movzx   eax,byte ptr [drv_count]
        cmp     eax,MAX_DRIVES
        jae     dau_out
        call    drv_slot                ; -> EBX
        mov     byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT OR DRVF_USB
        lea     eax,[ebp+USB_DRV0]
        mov     [ebx+DRV_BIOS],al
        mov     byte ptr [ebx+DRV_TYPE],0
        mov     byte ptr [ebx+DRV_PIDX],0FFh
        mov     dword ptr [ebx+DRV_START],0
        mov     dword ptr [ebx+DRV_SECS],0
        mov     byte ptr [usb_located+ebp],0
IFDEF HANG_E9
        mov     al,'L'
        out     0E9h,al
        mov     al,byte ptr [drv_count]
        call    uhx_hex8
        mov     al,[ebx+DRV_BIOS]
        call    uhx_hex8
        mov     al,[ebx+DRV_FLAGS]
        call    uhx_hex8
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
ENDIF
        inc     byte ptr [drv_count]
        inc     ebp
        jmp     dau_lp
dau_out:
        popad
        ret

;---------------------------------------------------------------------
; usb_announce - a line for each USB keyboard and mouse that was there
;                at boot, and one for each disk with a letter: the
;                boot's only word about USB, as cd_announce is about
;                CD-ROMs.  A machine with nothing plugged in says
;                nothing.
;---------------------------------------------------------------------
usb_announce:
        pushad
        xor     edx,edx                 ; DL = the slot
uan_dev:
        mov     edi,OFFSET usb_devrec
        mov     al,USF_DEVICE
        call    dword ptr ds:[IOS_USB]
        jc      uan_devnext
        cmp     byte ptr [usb_devrec+USBD_STATE],USBS_READY
        jne     uan_devnext
        mov     esi,OFFSET msgUsbKbd
        cmp     byte ptr [usb_devrec+USBD_CLASS],USBC_KBD
        je      uan_say
        mov     esi,OFFSET msgUsbMouse
        cmp     byte ptr [usb_devrec+USBD_CLASS],USBC_MOUSE
        jne     uan_devnext
uan_say:
        call    dword ptr ds:[IOS_CONSTR]
uan_devnext:
        inc     dl
        cmp     dl,USB_MAXDEV
        jb      uan_dev

        xor     edx,edx                 ; DL = the unit
uan_unit:
        mov     edi,OFFSET usb_model
        mov     al,USF_UNIT
        call    dword ptr ds:[IOS_USB]
        jc      uan_out
        test    al,USBUF_ATTACHED
        jz      uan_unitnext
        or      ch,ch                   ; a BIOS drive's disk is said under
        jz      uan_own                 ; that drive's letter, if it has one
        mov     al,ch
        call    usb_letterof_bios
        jnc     uan_letter
uan_own:
        lea     eax,[edx+USB_DRV0]
        call    usb_letterof            ; -> AL = the letter, or CF
        jc      uan_noletter
uan_letter:
        add     al,'A'
        mov     [msgUsbLetter],al
        mov     esi,OFFSET msgUsbDisk
        call    dword ptr ds:[IOS_CONSTR]
        jmp     uan_unitnext
uan_noletter:
        mov     esi,OFFSET msgUsbNoLtr
        call    dword ptr ds:[IOS_CONSTR]
uan_unitnext:
        inc     dl
        cmp     dl,USB_MAXUNITS
        jb      uan_unit
uan_out:
        popad
        ret

;--- usb_letterof - AL = a USB unit's drive number -> AL = the letter
;--- whose row names it (0 = A), or CF.  usb_letterof_bios - the same
;--- for any drive number: the first letter on BIOS drive AL
usb_letterof_bios:
        mov     byte ptr [ulo_anyrow],1
        jmp     ulo_go
usb_letterof:
        mov     byte ptr [ulo_anyrow],0
ulo_go:
        push    ebx
        push    ecx
        mov     ebx,OFFSET drv_tbl
        xor     ecx,ecx
ulo_lp:
        cmp     ecx,MAX_DRIVES
        jae     ulo_no
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      ulo_next
        cmp     byte ptr [ulo_anyrow],0
        jne     ulo_cmp
        test    byte ptr [ebx+DRV_FLAGS],DRVF_USB
        jz      ulo_next
ulo_cmp:
        cmp     [ebx+DRV_BIOS],al
        je      ulo_hit
ulo_next:
        add     ebx,DRV_LEN
        inc     ecx
        jmp     ulo_lp
ulo_hit:
        mov     al,cl
        pop     ecx
        pop     ebx
        clc
        ret
ulo_no:
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; usb_locate - EBX -> a DRVF_USB row about to be mounted: DRV_START,
;              DRV_SECS and DRV_TYPE from the disk that is on the unit
;              now, unless they were worked out for this very disk
;              already.  CF if sector 0 could not be read - the
;              critical-error handler has had its say by then.
;
; Sector 0 is a partition table with a FAT partition in it, and the
; first one is the volume; or it is a boot sector, and the whole disk
; is; or it is neither, and the row says the whole disk so that the
; mount goes on to find no BPB and say "Invalid media type", which is
; the truth about a stick nobody has formatted.
;=====================================================================
usb_locate:
        pushad
        movzx   edx,byte ptr [ebx+DRV_BIOS]
        sub     dl,USB_DRV0
        mov     edi,OFFSET usb_model
        mov     al,USF_UNIT
        push    ebx                     ; the call answers EBX = sectors,
        call    dword ptr ds:[IOS_USB]  ; -> AL flags, AH generation
        pop     ebx                     ; and EBX is the row here
        jc      ulc_fail
        test    ch,ch                   ; the disk a BIOS drive brought up
        jz      ulc_go                  ; has that drive's letters, if the
        push    eax                     ; boot found a volume on it, and
        mov     al,ch                   ; then this letter stays empty.  A
        call    usb_letterof_bios       ; blank stick has none, and is
        pop     eax                     ; this letter's to format
        jnc     ulc_fail
ulc_go:
        movzx   ecx,dl                  ; ECX = the unit
        cmp     byte ptr [usb_located+ecx],0
        je      ulc_read
        cmp     ah,byte ptr [usb_locgen+ecx]
        je      ulc_ok                  ; the same disk, already placed
ulc_read:
        mov     byte ptr [usb_locgen+ecx],ah
        mov     byte ptr [usb_located+ecx],0
        push    ebx
        mov     al,[ebx+DRV_BIOS]
        xor     ebx,ebx
        mov     cx,1
        mov     edi,OFFSET sec_buf
        call    disk_read               ; the first access: INT 24h if the
        pop     ebx                     ; slot is empty
        jc      ulc_fail
        ; the disk's size, now that it has been asked
        movzx   edx,byte ptr [ebx+DRV_BIOS]
        sub     dl,USB_DRV0
        mov     edi,OFFSET usb_model
        mov     al,USF_UNIT
        push    ebx
        call    dword ptr ds:[IOS_USB]  ; -> EBX = sectors
        mov     ebp,ebx
        pop     ebx
        movzx   ecx,dl
        mov     byte ptr [usb_locgen+ecx],ah
        mov     byte ptr [usb_located+ecx],1
        ; the whole disk, until the table says otherwise
        mov     byte ptr [ebx+DRV_TYPE],0
        mov     dword ptr [ebx+DRV_START],0
        mov     dword ptr [ebx+DRV_SECS],ebp
        cmp     word ptr [sec_buf+1FEh],MBR_SIG
        jne     ulc_nosig
        mov     esi,OFFSET sec_buf + PART_OFF
        mov     ecx,PART_MAX
ulc_entry:
        mov     al,[esi+PE_TYPE]
        call    part_isfat
        jnc     ulc_part
        add     esi,PE_LEN
        loop    ulc_entry
ulc_nosig:
        ; no partition for us: a boot sector makes the disk the volume
        call    bpb_looks
        jc      ulc_ok                  ; neither: the mount will say so
        mov     byte ptr [ebx+DRV_TYPE],PT_FAT16B
        jmp     ulc_ok
ulc_part:
        mov     al,[esi+PE_TYPE]
        mov     [ebx+DRV_TYPE],al
        mov     eax,dword ptr [esi+PE_START]
        mov     dword ptr [ebx+DRV_START],eax
        mov     eax,dword ptr [esi+PE_SECS]
        mov     dword ptr [ebx+DRV_SECS],eax
ulc_ok:
        popad
        clc
        ret
ulc_fail:
        popad
        stc
        ret

;=====================================================================
; usb_chkmedia - from vol_select: the loaded volume, if it is a USB
;                letter with a volume mounted, is checked against the
;                unit.  A disk that has gone, or a different one, or a
;                table rewritten under it (usb_relayout), unmounts the
;                volume, drops every cache that described it, and
;                closes the files that were open on it.  All registers
;                preserved.
;=====================================================================
usb_chkmedia:
        cmp     byte ptr [fat_ok],0
        je      ucm_out
        pushad
        mov     al,[cur_vol]
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_USB
        jz      ucm_same
        movzx   edx,byte ptr [ebx+DRV_BIOS]
        sub     dl,USB_DRV0
        movzx   ecx,dl
        cmp     byte ptr [usb_located+ecx],0
        je      ucm_changed
        mov     edi,OFFSET usb_model
        mov     al,USF_UNIT
        push    ecx                     ; the call answers ECX = the slot,
        call    dword ptr ds:[IOS_USB]  ; and ECX is the unit here
        pop     ecx
        jc      ucm_changed
        test    al,USBUF_ATTACHED
        jz      ucm_changed
        cmp     ah,byte ptr [usb_locgen+ecx]
        je      ucm_same
ucm_changed:
IFDEF HANG_E9
        push    eax
        mov     al,'M'
        out     0E9h,al
        mov     al,byte ptr [usb_located+ecx]
        call    uhx_hex8
        pop     eax
        call    uhx_hex8
        push    eax
        mov     al,ah
        call    uhx_hex8
        mov     al,byte ptr [usb_locgen+ecx]
        call    uhx_hex8
        mov     al,byte ptr [cur_vol]
        call    uhx_hex8
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
        pop     eax
ENDIF
        mov     al,[cur_vol]
        call    drv_slot
        mov     al,[ebx+DRV_BIOS]
        call    cache_dropdrv           ; its sectors describe a disk gone
        mov     al,[cur_vol]
        call    sft_dropdrv             ; and so do its open files
        mov     byte ptr [usb_located+ecx],0
        mov     byte ptr [fat_ok],0
        mov     byte ptr [fatsec_dirty],0
        mov     byte ptr [fat12_dirty],0
        mov     dword ptr [fatsec_lba],-1
        mov     dword ptr [sec_lba],-1
        mov     dword ptr [clusbuf_clus],-1
        mov     dword ptr [dc_curord],-1
ucm_same:
        popad
ucm_out:
        ret

;=====================================================================
; usb_relayout - AL = a drive number that has just had its sector 0
;                written (FDISK, through 21h/F1h AL=0Eh): if it is a
;                USB unit, the letter on it is placed again on its next
;                use.  All registers preserved.
;=====================================================================
usb_relayout:
        push    eax
        cmp     al,USB_DRV0
        jb      url_bios
        cmp     al,USB_DRV0 + USB_MAXUNITS
        jae     url_out
        sub     al,USB_DRV0
        movzx   eax,al
        mov     byte ptr [usb_located+eax],0
url_out:
        pop     eax
        ret
url_bios:
        ; a BIOS drive a unit stands in for: that unit's letter, placed
        ; afresh - FDISK on the BIOS number of a blank stick that the
        ; USB letter is serving
        push    edx
        call    usb_aliasunit           ; -> DL = the unit, or CF
        jc      url_nobios
        movzx   eax,dl
        mov     byte ptr [usb_located+eax],0
url_nobios:
        pop     edx
        jmp     url_out

;=====================================================================
; usb_aliasunit - AL = a BIOS drive number -> DL = the USB unit that
;                 stands in for it (usb_adopt), or CF.  Everything
;                 else preserved.
;=====================================================================
usb_aliasunit:
        push    eax
        push    ebx
        push    ecx
        push    edi
        mov     [uau_want],al           ; not in AH: the call answers AH
        xor     dl,dl
uau_lp:
        mov     edi,OFFSET usb_model
        mov     al,USF_UNIT
        push    edx
        call    dword ptr ds:[IOS_USB]  ; -> CH = the drive it stands in for
        pop     edx
        jc      uau_next
        or      ch,ch
        jz      uau_next
        cmp     ch,[uau_want]
        je      uau_hit
uau_next:
        inc     dl
        cmp     dl,USB_MAXUNITS
        jb      uau_lp
        pop     edi
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret
uau_hit:
        pop     edi
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret

;=====================================================================
; sft_dropdrv - AL = a letter (0 = A): every file open on it is closed
;               from under its program, which hears "invalid handle"
;               the next time it asks.  Nothing is written: what the
;               entry remembers describes a disk that is not there.
;=====================================================================
sft_dropdrv:
        pushad
        movzx   edx,al
        mov     esi,OFFSET sft_tbl
        mov     ecx,MAX_HANDLES
sdd_lp:
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      sdd_next
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jnz     sdd_next
        cmp     edx,[esi+SFT_DRIVE]
        jne     sdd_next
        and     dword ptr [esi+SFT_FLAGS],NOT (SFTF_INUSE OR SFTF_DIRTY)
sdd_next:
        add     esi,SFT_LEN
        loop    sdd_lp
        popad
        ret

;=====================================================================
; Data
;=====================================================================
IFDEF HANG_E9
;--- uhx_hex8 - AL as two hex digits on port E9h, everything preserved
uhx_hex8:
        push    eax
        push    ecx
        mov     ah,al
        mov     cl,2
uhx_lp:
        rol     ah,4
        mov     al,ah
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     uhx_dig
        add     al,7
uhx_dig:
        out     0E9h,al
        dec     cl
        jnz     uhx_lp
        pop     ecx
        pop     eax
        ret
ENDIF
ulo_anyrow  db  0                       ; usb_letterof: any row, not USB rows
uau_want    db  0                       ; usb_aliasunit's drive number
usb_on      db  1                       ; USB=ON|OFF: take the controllers
usb_letters db  1                       ; ...and how many letters, USB=ON,n
            db  0,0
usb_locgen  db  USB_MAXUNITS dup(0)     ; the generation each letter was
                                        ; placed for
usb_located db  USB_MAXUNITS dup(0)     ; ...and whether it has been
usb_model   db  24 dup(0)               ; USF_UNIT's model, unused here
usb_devrec  db  USBD_LEN dup(0)         ; USF_DEVICE's row, for the announce

msgUsbKbd   db  'USB keyboard detected',13,10,0
msgUsbMouse db  'Mouse detected on USB',13,10,0
msgUsbDisk  db  'USB disk detected as '
msgUsbLetter db 'D:',13,10,0
msgUsbNoLtr db  'USB disk detected, and no drive letter left for it',13,10,0
