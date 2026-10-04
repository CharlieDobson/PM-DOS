;=====================================================================
; VOLFLAG.INC - the clean-shutdown flag, and CHKDSK at boot
;
; A FAT16 or FAT32 volume carries one bit that says whether it was
; left in a state the filesystem agrees with: bit 15 of FAT entry 1 on
; FAT16, bit 27 on FAT32.  Set means clean.  Windows 95 put it there
; and turns it off while a volume is in use, and its ScanDisk runs at
; the next start when it finds the bit off.  Using the same bit means
; a disk Windows 98 left dirty is checked here, and one this kernel
; left dirty is checked there.  FAT12 has no spare bit, so a diskette
; never has the flag; neither does a volume behind a loaded block
; driver, a CD-ROM or a mounted image, because those are not this
; kernel's disks to vouch for.  vol_vf in vol_state says whether the
; loaded volume has it.
;
;---------------------------------------------------------------------
; WHEN THE BIT GOES OFF AND WHEN IT COMES BACK
;
; CACHE.INC makes a promise this rests on: at every commit point -
; the prompt, a program starting, a program ending, INT 2Fh's commit -
; everything the filesystem wrote is on the medium.  So a volume is in
; a state CHKDSK would agree with at a commit point, unless a file is
; open on it with writes that have not reached its directory entry,
; and is not otherwise.  The bit follows that:
;
;   OFF  before the first write to the volume since it was last
;        marked clean.  disk_write calls vfl_touch, which writes the
;        bit off and pushes the sector out of the cache at once, so
;        that it is on the disk ahead of anything that follows it.
;        After that, writes cost nothing extra.
;
;   ON   at a commit point (dos_commit -> vfl_cleanall), if the cache
;        owes nothing and no open file on the drive has SFTF_DIRTY.
;        Written last, after the flush, for the same reason.
;
; So turning the machine off at the prompt leaves every volume clean,
; and so does turning it off in the middle of a game that is not
; writing anything; turning it off while a program has a file half
; written leaves that volume dirty, which is the truth.  The cost is
; one FAT sector written to each FAT copy when a volume becomes dirty,
; and again when it becomes clean - once per command that writes, not
; once per write.
;
; RAW WRITES TURN IT OFF TOO.  INT 26h, 21h/7305h and a DOS box's
; INT 13h are sectors written behind the filesystem's back, and a disk
; utility stopped halfway through is exactly the volume that needs
; checking.  Each of those asks vol_syncdrv for mode 2 before it
; writes, which is mode 0 plus vfl_touch on every volume the range
; touches.  And a raw write may land ON the flag: mode 1 afterwards
; calls vfl_after, which reads the bit back when the range covered
; sector 0 of a FAT copy.  CHKDSK /F writes back the FAT it read,
; which can put the bit on underneath a volume the kernel has just
; marked; vfl_after puts it off again.
;
; WINDOWS 3.1 IN ENHANCED MODE writes through RMDOS, which knows
; nothing of any of this.  So park_go marks every volume dirty on the
; way down (vfl_parkmark) and no commit marks anything clean until
; park_recover has mounted them all again.  A crash inside Windows
; therefore means a check at the next start, as it did on Windows 95.
;
;---------------------------------------------------------------------
; FOUND OFF, AND HELD
;
; A volume whose bit is off when it is mounted, and not because this
; kernel turned it off (DRVF_DIRTY was clear), was left that way by a
; crash, a power cut, or another system.  DRVF_HOLD goes on, and while
; it is on no commit turns the bit back on: the kernel has no idea
; whether the volume is sound, and only a check can say.  CHKDSK says
; so through 21h/F1h AL=1Dh VLF_CHECKED once it has found nothing
; wrong, or has finished a /F; the hold goes, and the next commit
; turns the bit on.  The two flags live in the volume table rather
; than in vol_state, because a remount re-reads the bit and has to
; know whether the kernel put it there.
;
; AT BOOT, after CONFIG.SYS, vfl_bootscan mounts every volume that has
; the flag and lists the held ones; shell_restart then runs CHKDSK X:
; /F on each before the interpreter, as the depth-0 program.  It ends
; the way the shell does - 4Ch at depth 0 comes back to shell_restart
; - so the next drive, and then the shell, follow on their own, and a
; ^C or a fault in CHKDSK skips only that drive.  AUTOCHECK= in
; CONFIG.SYS chooses ON, ASK or OFF; a safe-mode start turns ON into
; ASK (cfg_keys), because the check is one more thing that could be
; why the machine needed a safe-mode start.  CHKDSK.EXE is looked for
; beside the shell, then in the root of the boot drive.
;
; Included by DOSMAIN.ASM after RECYCLE.INC: it reads the SFT, the
; disk cache's count of what is owed, and FAT.INC's volume state, and
; FAT.INC, CRITERR.INC and CACHE.INC call forwards into it.
;=====================================================================

;---------------------------------------------------------------------
; vfl_touch - the loaded volume is about to be written.  If its bit is
;             still on, turn it off first.  Preserves all.
;
; DRVF_DIRTY goes on BEFORE the write, because the write comes back
; through disk_write - which calls this - and has to find it on.
;---------------------------------------------------------------------
vfl_touch:
        cmp     byte ptr [vol_vf],0
        je      vto_ret
        pushad
        mov     al,[cur_vol]
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_DIRTY
        jnz     vto_out
        or      byte ptr [ebx+DRV_FLAGS],DRVF_DIRTY
        mov     al,0
        call    vfl_setflag             ; a failure leaves DRVF_DIRTY on,
                                        ; and the commit tries the other way
vto_out:
        popad
vto_ret:
        ret

;---------------------------------------------------------------------
; vfl_setflag - AL = 0: the loaded volume's bit off.  AL = 1: on.
;               Every FAT copy the volume keeps in step, then out of
;               the cache and onto the disk.  CF on failure.  Preserves
;               all.
;
; QUIETLY: a sector that will not write here is housekeeping going
; wrong, not something the user asked for, and crit_busy makes the
; critical error answer Fail without a prompt - as park_remount does.
;
; FAT SECTOR 0 MAY BE THE ONE THE FILESYSTEM HOLDS, dirty, in
; fatsec_buf.  Then that copy is the newest and it gets the bit too:
; otherwise its next flush would write the old bit back over this.
;---------------------------------------------------------------------
vfl_setflag:
        pushad
        mov     [vfl_want],al
        movzx   eax,byte ptr [crit_busy]
        push    eax
        mov     byte ptr [crit_busy],1

        mov     ebx,[fat_fatlba]                ; a copy-0 LBA, which is
        cmp     ebx,[fatsec_lba]                ; how fatsec_lba is kept
        jne     vsf_read
        mov     edi,OFFSET fatsec_buf
        call    vfl_patch
        mov     esi,OFFSET fatsec_buf
        mov     edi,OFFSET vfl_buf
        mov     ecx,512 / 4
        rep     movsd
        jmp     vsf_write
vsf_read:
        call    fat_rdcopy                      ; the live copy
        mov     al,[fat_drive]
        mov     cx,1
        mov     edi,OFFSET vfl_buf
        call    disk_read
        jc      vsf_fail
        mov     edi,OFFSET vfl_buf
        call    vfl_patch

vsf_write:
        mov     ebx,[fat_fatlba]
        call    fat_wrcopies                    ; EBX = first, ESI = how many
vsf_copy:
        mov     al,[fat_drive]
        mov     cx,1
        mov     edi,OFFSET vfl_buf
        call    disk_wrsure                     ; not disk_write: no vfl_touch
        jc      vsf_fail
        mov     al,[fat_drive]
        mov     ecx,1
        call    cache_flushrange                ; on the disk now, ahead of
        add     ebx,[fat_spf]                   ; whatever is written next
        dec     esi
        jnz     vsf_copy

        pop     eax
        mov     byte ptr [crit_busy],al
        popad
        clc
        ret
vsf_fail:
        pop     eax
        mov     byte ptr [crit_busy],al
        popad
        stc
        ret

;---------------------------------------------------------------------
; vfl_patch - EDI -> a copy of FAT sector 0.  The bit as vfl_want says.
; vfl_isdirty - EDI -> a copy of FAT sector 0.  CF if the bit is off.
;
; Both for the loaded volume's FAT width, 16 or 32.  The FAT16 mask is
; written as an XOR because NOT 8000h is a 32-bit number, and MASM
; will not put it in a word.
;---------------------------------------------------------------------
vfl_patch:
        cmp     byte ptr [fat_type],16
        jne     vpa_32
        and     word ptr [edi+2],0FFFFh XOR FAT16_CLEAN
        cmp     byte ptr [vfl_want],0
        je      vpa_out
        or      word ptr [edi+2],FAT16_CLEAN
        ret
vpa_32:
        and     dword ptr [edi+4],NOT FAT32_CLEAN
        cmp     byte ptr [vfl_want],0
        je      vpa_out
        or      dword ptr [edi+4],FAT32_CLEAN
vpa_out:
        ret

vfl_isdirty:
        cmp     byte ptr [fat_type],16
        jne     vid_32
        test    word ptr [edi+2],FAT16_CLEAN
        jmp     vid_test
vid_32:
        test    dword ptr [edi+4],FAT32_CLEAN
vid_test:
        jnz     vid_clean
        stc
        ret
vid_clean:
        clc
        ret

;---------------------------------------------------------------------
; vfl_found - EDI -> FAT sector 0 of the loaded volume, as just read
;             off the disk.  The volume table is brought into line
;             with it.  Preserves all.
;
;   bit off, and the kernel did not turn it off   DRVF_DIRTY + HOLD
;   bit off, and the kernel did                   DRVF_DIRTY
;   bit on                                        neither
;
; The last line drops a hold too: whoever put the bit back on - a
; FORMAT, or another system's checker - has said the volume is sound.
;---------------------------------------------------------------------
vfl_found:
        pushad
        mov     al,[cur_vol]
        call    drv_slot
        call    vfl_isdirty
        jnc     vfd_clean
        test    byte ptr [ebx+DRV_FLAGS],DRVF_DIRTY
        jnz     vfd_ours
        or      byte ptr [ebx+DRV_FLAGS],DRVF_HOLD
vfd_ours:
        or      byte ptr [ebx+DRV_FLAGS],DRVF_DIRTY
        popad
        ret
vfd_clean:
        and     byte ptr [ebx+DRV_FLAGS],NOT (DRVF_DIRTY OR DRVF_HOLD)
        popad
        ret

;---------------------------------------------------------------------
; vfl_mounted - fat_mount has just checked FAT[0], which left the live
;               copy's FAT sector 0 in clus_buf.  Does this volume have
;               the flag, and what does it say?  Preserves all.
;---------------------------------------------------------------------
vfl_mounted:
        pushad
        mov     byte ptr [vol_vf],0
        cmp     byte ptr [fat_type],12
        je      vmo_out
        mov     al,[cur_vol]
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_FLOPPY OR DRVF_CDROM OR DRVF_DRIVER OR DRVF_IMAGE
        jnz     vmo_out
        mov     byte ptr [vol_vf],1
        mov     edi,OFFSET clus_buf
        call    vfl_found
vmo_out:
        popad
        ret

;---------------------------------------------------------------------
; vfl_after - vol_syncdrv mode 1: somebody else has written sectors
;             [vsd_lba, vsd_end) of the loaded volume's disk, and the
;             caches are already gone.  If sector 0 of a FAT copy was
;             among them the bit may have changed, so it is read back.
;             Preserves all.
;
; A WRITE THAT TURNED THE BIT BACK ON UNDER THE KERNEL is the case
; this is for: CHKDSK /F writing back the FAT it read before its first
; write turned the bit off.  The volume is still being written, so the
; bit goes off again.  The hold goes either way - vfl_found's rule.
;---------------------------------------------------------------------
vfl_after:
        cmp     byte ptr [vol_vf],0
        je      vaf_ret
        pushad
        mov     ebx,[fat_fatlba]
        mov     ecx,[fat_nfats]
vaf_copy:
        cmp     ebx,[vsd_lba]
        jb      vaf_next
        cmp     ebx,[vsd_end]
        jb      vaf_hit
vaf_next:
        add     ebx,[fat_spf]
        loop    vaf_copy
        popad
vaf_ret:
        ret

vaf_hit:
        movzx   eax,byte ptr [crit_busy]
        push    eax
        mov     byte ptr [crit_busy],1
        mov     ebx,[fat_fatlba]
        call    fat_rdcopy
        mov     al,[fat_drive]
        mov     cx,1
        mov     edi,OFFSET vfl_buf
        call    disk_read
        jc      vaf_done                        ; leave the table as it was
        mov     al,[cur_vol]
        call    drv_slot
        mov     dl,[ebx+DRV_FLAGS]              ; what the kernel thought
        mov     edi,OFFSET vfl_buf
        call    vfl_found                       ; what the disk says
        test    dl,DRVF_DIRTY
        jz      vaf_done
        test    byte ptr [ebx+DRV_FLAGS],DRVF_DIRTY
        jnz     vaf_done
        or      byte ptr [ebx+DRV_FLAGS],DRVF_DIRTY
        mov     al,0
        call    vfl_setflag
vaf_done:
        pop     eax
        mov     byte ptr [crit_busy],al
        popad
        ret

;---------------------------------------------------------------------
; vfl_written - EAX = a drive letter, 0 = A.  CF if a file open on it
;               has been written to and not yet closed or committed -
;               its directory entry does not yet say what its clusters
;               do.  Preserves all.
;---------------------------------------------------------------------
vfl_written:
        push    ecx
        push    esi
        mov     esi,OFFSET sft_tbl
        mov     ecx,MAX_HANDLES
vwr_lp:
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      vwr_next
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jnz     vwr_next
        test    dword ptr [esi+SFT_FLAGS],SFTF_DIRTY
        jz      vwr_next
        cmp     [esi+SFT_DRIVE],eax
        je      vwr_yes
vwr_next:
        add     esi,SFT_LEN
        loop    vwr_lp
        pop     esi
        pop     ecx
        clc
        ret
vwr_yes:
        pop     esi
        pop     ecx
        stc
        ret

;---------------------------------------------------------------------
; vfl_ours - EAX = a drive letter, 0 = A.  CF clear if it is one this
;            kernel reads itself, from a partition on a disk: the only
;            kind that can have the flag.  Whether it does depends on
;            the FAT width, which takes a mount to know.  Preserves all.
;---------------------------------------------------------------------
vfl_ours:
        push    ebx
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      vou_no
        test    byte ptr [ebx+DRV_FLAGS],DRVF_FLOPPY OR DRVF_CDROM OR DRVF_DRIVER OR DRVF_IMAGE
        jnz     vou_no
        pop     ebx
        clc
        ret
vou_no:
        pop     ebx
        stc
        ret

;=====================================================================
; vfl_cleanall - the end of dos_commit.  Every volume the kernel made
;                dirty, that is not held and has no written file open,
;                gets its bit back on.  Preserves all.
;
; NOTHING IF THE CACHE STILL OWES A WRITE.  cache_commit has just run;
; a block left dirty means a sector would not write, the disk is not
; what the filesystem says, and "clean" would be a lie on top of it.
;
; A volume a raw write unmounted is mounted again to be marked - and
; the mount reads the bit itself, so a FORMAT that has just written a
; clean FAT needs nothing more.
;=====================================================================
vfl_cleanall:
        cmp     byte ptr [vfl_parked],0         ; RMDOS has the disks: see
        jne     vca_ret                         ; vfl_parkmark
        cmp     dword ptr [cache_dirty],0
        jne     vca_ret
        pushad
        mov     al,[cur_vol]
        mov     [vfl_back],al
        movzx   eax,byte ptr [crit_busy]
        push    eax
        mov     byte ptr [crit_busy],1

        xor     ebp,ebp                         ; EBP = drive letter
vca_one:
        movzx   eax,byte ptr [drv_count]
        cmp     ebp,eax
        jae     vca_done
        mov     eax,ebp
        call    drv_slot
        mov     al,[ebx+DRV_FLAGS]
        and     al,DRVF_DIRTY OR DRVF_HOLD
        cmp     al,DRVF_DIRTY                   ; dirty, and not held
        jne     vca_next
        mov     eax,ebp
        call    vfl_written
        jc      vca_next
        mov     eax,ebp
        call    vol_select
        jc      vca_next
        cmp     byte ptr [vol_vf],0
        je      vca_next
        mov     al,[ebx+DRV_FLAGS]              ; the mount may have read
        and     al,DRVF_DIRTY OR DRVF_HOLD      ; it afresh
        cmp     al,DRVF_DIRTY
        jne     vca_next
        mov     al,1
        call    vfl_setflag
        jc      vca_next
        and     byte ptr [ebx+DRV_FLAGS],NOT DRVF_DIRTY
vca_next:
        inc     ebp
        jmp     vca_one

vca_done:
        mov     al,[vfl_back]
        call    vol_swap
        pop     eax
        mov     byte ptr [crit_busy],al
        popad
vca_ret:
        ret

;---------------------------------------------------------------------
; vfl_parkmark - park_go, after its commit: every volume with the flag
;                is marked dirty before RMDOS takes the disks, and none
;                is marked clean until park_recover clears vfl_parked.
;                Preserves all.
;---------------------------------------------------------------------
vfl_parkmark:
        pushad
        mov     byte ptr [vfl_parked],1
        mov     al,[cur_vol]
        mov     [vfl_back],al
        movzx   eax,byte ptr [crit_busy]
        push    eax
        mov     byte ptr [crit_busy],1
        xor     ebp,ebp
vpm_one:
        movzx   eax,byte ptr [drv_count]
        cmp     ebp,eax
        jae     vpm_done
        mov     eax,ebp
        call    vfl_ours
        jc      vpm_next
        call    vol_select
        jc      vpm_next
        call    vfl_touch
vpm_next:
        inc     ebp
        jmp     vpm_one
vpm_done:
        mov     al,[vfl_back]
        call    vol_swap
        pop     eax
        mov     byte ptr [crit_busy],al
        popad
        ret

;=====================================================================
; vfl_bootscan - after CONFIG.SYS: mount every volume that can have
;                the flag, and list the ones found held in ack_list for
;                shell_restart.  With AUTOCHECK=OFF each is named on
;                the screen instead and the list is dropped.
;
; Mounting them all here is new - they used to wait for their first
; use - and costs a few sector reads each.  A volume that will not
; mount says nothing (crit_busy) and is left to raise its error when
; somebody uses it, as it always did.
;=====================================================================
vfl_bootscan:
        pushad
        mov     dword ptr [ack_list],0
        mov     al,[cur_vol]
        mov     [vfl_back],al
        movzx   eax,byte ptr [crit_busy]
        push    eax
        mov     byte ptr [crit_busy],1
        xor     ebp,ebp
vbs_one:
        movzx   eax,byte ptr [drv_count]
        cmp     ebp,eax
        jae     vbs_done
        mov     eax,ebp
        call    vfl_ours
        jc      vbs_next
        call    vol_select
        jc      vbs_next
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_HOLD
        jz      vbs_next
        bts     dword ptr [ack_list],ebp
vbs_next:
        inc     ebp
        jmp     vbs_one
vbs_done:
        mov     al,[vfl_back]
        call    vol_swap
        pop     eax
        mov     byte ptr [crit_busy],al

        cmp     byte ptr [autoc_mode],AUTOC_OFF
        jne     vbs_out
vbs_say:
        mov     eax,[ack_list]
        or      eax,eax
        jz      vbs_out
        bsf     eax,eax
        btr     dword ptr [ack_list],eax
        mov     esi,OFFSET msgAckOff
        call    ack_say
        jmp     vbs_say
vbs_out:
        popad
        ret

;---------------------------------------------------------------------
; ack_next - shell_restart: the next drive to check at boot.  CF clear
;            with CHKDSK loaded into the slot and ESI -> its tail; CF
;            set when there is none left and the shell is next.
;---------------------------------------------------------------------
ack_next:
        mov     eax,[ack_list]
        or      eax,eax
        jz      ane_none
        bsf     eax,eax
        btr     dword ptr [ack_list],eax
        mov     [ack_drv],al
        add     al,'A'
        mov     byte ptr [ack_tail+2],al        ; " X: /F"

        cmp     byte ptr [ack_more],0           ; a blank line after the
        je      ane_first                       ; last drive's CHKDSK report
        mov     esi,OFFSET msgCrLf
        call    dword ptr ds:[IOS_CONSTR]
ane_first:
        mov     byte ptr [ack_more],1
        mov     al,[ack_drv]
        cmp     byte ptr [autoc_mode],AUTOC_ASK
        je      ane_ask
        mov     esi,OFFSET msgAckRun
        call    ack_say
        jmp     ane_find
ane_ask:
        mov     esi,OFFSET msgAckAsk
        call    ack_say
ane_key:
        call    dword ptr ds:[IOS_CONIN]
        and     al,0DFh                         ; y and n count too
        cmp     al,'Y'
        je      ane_echo
        cmp     al,'N'
        jne     ane_key
ane_echo:
        push    eax
        call    dword ptr ds:[IOS_CONOUT]
        mov     esi,OFFSET msgCrLf
        call    dword ptr ds:[IOS_CONSTR]
        pop     eax
        cmp     al,'Y'
        jne     ack_next

        ; CHKDSK.EXE beside the shell: shell_name up to and including
        ; its last '\' (or its ':'), and the name after that.
ane_find:
        mov     esi,OFFSET shell_name
        mov     edi,OFFSET ack_path
        mov     edx,edi
ane_cp:
        lodsb
        stosb
        cmp     al,'\'
        je      ane_sep
        cmp     al,':'
        jne     ane_cpnext
ane_sep:
        mov     edx,edi                         ; just past it
ane_cpnext:
        or      al,al
        jnz     ane_cp
        mov     edi,edx
        call    ack_name
        mov     edx,OFFSET ack_path
        call    exec_load
        jnc     ane_loaded

        ; ...or in the root of the boot drive, which is where every
        ; image this tree builds puts it
        mov     edi,OFFSET ack_path
        mov     al,[shell_dflt]                 ; the boot letter, patched
        stosb                                   ; in at startup
        mov     al,':'
        stosb
        mov     al,'\'
        stosb
        call    ack_name
        mov     edx,OFFSET ack_path
        call    exec_load
        jnc     ane_loaded

        mov     al,[ack_drv]
        mov     esi,OFFSET msgAckNone
        call    ack_say
        jmp     ack_next

ane_loaded:
        mov     esi,OFFSET ack_tail
        clc
        ret
ane_none:
        stc
        ret

; ack_name - "CHKDSK.EXE",0 at EDI
ack_name:
        mov     esi,OFFSET ack_exe
an_cp:
        lodsb
        stosb
        or      al,al
        jnz     an_cp
        ret

;---------------------------------------------------------------------
; ack_say - ESI -> ASCIIZ with '#' where drive AL's letter goes, on
;           the screen.  AL and ESI are spent.
;---------------------------------------------------------------------
ack_say:
        add     al,'A'
        mov     ah,al
as_lp:
        mov     al,[esi]
        inc     esi
        or      al,al
        jz      as_out
        cmp     al,'#'
        jne     as_put
        mov     al,ah
as_put:
        push    eax
        push    esi
        call    dword ptr ds:[IOS_CONOUT]
        pop     esi
        pop     eax
        jmp     as_lp
as_out:
        ret

;=====================================================================
; F1h AL=1Dh - DL = drive (0 = default, 1 = A:), bits 16-23 of EAX =
;              VLF_QUERY or VLF_CHECKED.  -> EAX = VLS_* bits.  A drive
;              without the flag answers 0 to both.
;=====================================================================
fF1_vflag:
        cmp     byte ptr [ebp+F_EAX+2],VLF_CHECKED
        ja      i21_unsup
        mov     al,[ebp+F_EDX]
        or      al,al
        jnz     fvf_expl
        mov     al,[cur_drive]
        jmp     fvf_sel
fvf_expl:
        dec     al
        cmp     al,MAX_DRIVES
        jae     fF1_baddrv
fvf_sel:
        call    vol_select
        jc      fF1_baddrv
        call    drv_slot                        ; AL survived: vol_select
        xor     edx,edx                         ; keeps every register
        cmp     byte ptr [vol_vf],0
        je      fvf_answer
        cmp     byte ptr [ebp+F_EAX+2],VLF_CHECKED
        jne     fvf_state
        and     byte ptr [ebx+DRV_FLAGS],NOT DRVF_HOLD
fvf_state:
        mov     dl,VLS_KEPT
        test    byte ptr [ebx+DRV_FLAGS],DRVF_DIRTY
        jz      fvf_hold
        or      dl,VLS_DIRTY
fvf_hold:
        test    byte ptr [ebx+DRV_FLAGS],DRVF_HOLD
        jz      fvf_answer
        or      dl,VLS_HOLD
fvf_answer:
        mov     [ebp+F_EAX],edx
        jmp     i21_ok

;---------------------------------------------------------------------
autoc_mode  db  AUTOC_ON                ; AUTOCHECK=, or ASK in safe mode
vfl_parked  db  0                       ; RMDOS has the disks
vfl_want    db  0                       ; vfl_setflag: 0 = off, 1 = on
vfl_back    db  0                       ; the loaded volume, to go back to
ack_drv     db  0                       ; the drive being checked
ack_more    db  0                       ; a drive has been asked about already
            db  0,0
ack_list    dd  0                       ; bit n: drive n waits for CHKDSK
ack_tail    db  6,' #: /F',0Dh          ; CHKDSK's tail, the letter put in
            db  0
ack_exe     db  'CHKDSK.EXE',0
ack_path    db  SHELLNAME_MAX + 16 dup(0)

msgAckRun   db  'Drive #: was not shut down properly. Running CHKDSK #: /F.',13,10,0
msgAckAsk   db  'Drive #: was not shut down properly. Run CHKDSK #: /F now (Y/N)? ',0
msgAckOff   db  'Drive #: was not shut down properly. Run CHKDSK #: /F to check the drive.',13,10,0
msgAckNone  db  'Cannot find CHKDSK.EXE to check drive #:.',13,10,0

; Below the arena and not in the file - see KBSS_BASE in PMDOS.INC.
_DOS    ENDS
_KBSS   SEGMENT
vfl_buf     db  512 dup(?)              ; FAT sector 0, for the bit
_KBSS   ENDS
_DOS    SEGMENT
