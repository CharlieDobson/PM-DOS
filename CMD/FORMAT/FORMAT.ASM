;=====================================================================
; FORMAT.ASM - FORMAT.COM, initialise a FAT volume afresh
;
;   FORMAT d:           fresh FATs and root directory, then every data
;                       sector read back and the bad ones' clusters
;                       marked
;   FORMAT d: /Q        quick: the structures only
;   FORMAT d: /U        unconditional: nothing of the old format is
;                       trusted, and a diskette has every track laid
;                       down again
;   FORMAT d: /C        clusters the old FAT had marked bad are tested
;                       again instead of staying marked
;   FORMAT d: /F:size   a diskette of that capacity
;   FORMAT d: /S        ...and the system copied to it afterwards
;   FORMAT d: /B        accepted, and does nothing: see fm_swb
;
; TWO JOBS UNDER ONE NAME, and which one is done depends on what is in
; the drive.
;
; REFORMATTING A VOLUME THAT HAS A FORMAT.  The BPB that is already
; there supplies the geometry and the layout is kept: the FATs are
; written afresh (entry 0 carries the media byte, entry 1 the end
; marker, and on FAT32 the root cluster its EOC), the root directory
; is emptied, and on FAT32 the FSInfo sector and its backup are
; rebuilt.  The boot sector is NOT touched: a SYS'd disk comes out
; still bootable.  This is all a hard disk ever gets - there are no
; tracks to lay down on an IDE disk - and it is what a diskette gets
; when its format is the one wanted and /U did not ask for more.
; Everything goes through INT 25h and 26h.
;
; MAKING A DISKETTE FROM NOTHING.  A disk that has never been
; formatted, or has the wrong capacity, or /U: every track is
; formatted and read back through 21h/F1h AL=14h (the BIOS's INT 13h
; AH=05h, which a native program cannot call), and then a boot
; sector, two FATs and a root directory are written for the capacity
; in fd_sizes.  Such a disk has no letter INT 26h could be pointed
; at until it is finished, so its sectors go by 21h/F1h AL=0Dh and
; 0Eh, which name the DRIVE.  sec_read and sec_write are where the
; two jobs meet, and nothing above them knows which it is doing.
;
; A BAD CLUSTER MARK IS KEPT unless /C or /U says otherwise.  A mark
; is the result of a surface test somebody waited for, and a quick
; format that threw them away would hand the bad sectors back to the
; next file written.
;
; The kernel is told afterwards through 21h/F1h AL=2 (remount): every
; cached notion of the old filesystem is discarded unflushed and the
; BPB is read back fresh, with the drive's directory back at the root.
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

CHUNK_SECS      EQU     32              ; 16K of sectors at a time

; a row of fd_sizes
FS_KB           EQU     0               ; dw - what /F: calls it
FS_TOTAL        EQU     2               ; dw - sectors
FS_SPT          EQU     4               ; db - sectors a track
FS_HEADS        EQU     5               ; db
FS_CYLS         EQU     6               ; db
FS_SPC          EQU     7               ; db - sectors a cluster
FS_ROOTENT      EQU     8               ; dw
FS_SPF          EQU     10              ; db - sectors a FAT
FS_MEDIA        EQU     11              ; db
FS_MEDIUM       EQU     12              ; db - FDM_*
FS_NAME         EQU     13              ; 7 bytes, '$' and all
FS_LEN          EQU     20
FS_COUNT        EQU     8

NO_SIZE         EQU     0FFh

_FMT    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_FMT, DS:_FMT, ES:_FMT, SS:_FMT
        ORG     0

;=====================================================================
; D32 header - the sector and chunk buffers live in the BSS
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET fm_entry>,<512 + 16384 + 16384 + 512>,4096

;=====================================================================
fm_entry:
        mov     eax,OFFSET img_end      ; carve up the BSS
        mov     [boot_sec],eax          ; 512: the boot sector
        add     eax,512
        mov     [fat_work],eax          ; 16K: a FAT, or a run of one
        add     eax,16384
        mov     [chunk_buf],eax         ; 16K: zeroes out, verify in
        add     eax,16384
        mov     [scratch],eax           ; 512: one sector, looked at

;--- critical errors are OURS ----------------------------------------
; A disk being formatted is a disk that may well be unreadable - that
; is frequently the reason - and the shell's Abort, Retry, Fail?
; prompt in the middle of it would be asking the user to arbitrate a
; failure FORMAT already knows how to report, with an Abort that
; would take FORMAT down mid-write.  So: fail everything, quietly, and
; let the checks below do the talking.  Every disk utility of the era
; hooked 24h for the same reason.  4Ch puts the shell's handler back
; from the PSP on the way out.
        mov     edx,OFFSET fm_crit
        mov     ax,2524h
        int     21h

;--- the command tail: FORMAT d: [switches] --------------------------
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
fm_skipb:
        or      ecx,ecx
        jz      fm_usage
        cmp     byte ptr [esi],' '
        je      fm_bstep
        cmp     byte ptr [esi],9
        jne     fm_havearg
fm_bstep:
        inc     esi
        dec     ecx
        jmp     short fm_skipb
fm_havearg:
        ; /? BEFORE THE DRIVE IS PARSED, because it stands where the
        ; drive would and there is nothing else it could mean.  The
        ; switch loop below never sees it: it runs after a drive has
        ; been accepted, and "FORMAT /?" has no drive in it.
        cmp     byte ptr [esi],'/'
        jne     fm_notq
        cmp     byte ptr [esi+1],'?'
        je      fm_help
fm_notq:
        cmp     ecx,2
        jb      fm_usage
        mov     al,[esi]
        cmp     byte ptr [esi+1],':'
        jne     fm_usage
        call    fm_upcase
        sub     al,'A'
        cmp     al,LASTDRIVE
        jae     fm_baddrv
        mov     [tgt_log],al
        add     al,'A'
        mov     [warn_drv],al           ; the prompts name the drive
        mov     [ins_drv],al
        add     esi,2
        sub     ecx,2

;--- switches --------------------------------------------------------
fm_sw:
        or      ecx,ecx
        jz      fm_argsdone
        cmp     byte ptr [esi],' '
        je      fm_swstep
        cmp     byte ptr [esi],9
        je      fm_swstep
        cmp     byte ptr [esi],0Dh
        je      fm_argsdone
        cmp     byte ptr [esi],'/'
        jne     fm_badsw
        cmp     ecx,2
        jb      fm_badsw
        mov     al,[esi+1]
        call    fm_upcase
        add     esi,2
        sub     ecx,2
        cmp     al,'?'                  ; "FORMAT C: /?" - a question
        je      fm_help                 ; about FORMAT, not an order
        cmp     al,'Q'
        jne     fm_swnq
        mov     byte ptr [quick],1
        jmp     short fm_sw
fm_swnq:
        cmp     al,'S'
        jne     fm_swns
        mov     byte ptr [sysit],1
        jmp     short fm_sw
fm_swns:
        cmp     al,'U'
        jne     fm_swnu
        mov     byte ptr [uncond],1
        jmp     short fm_sw
fm_swnu:
        cmp     al,'C'
        jne     fm_swnc
        mov     byte ptr [retest],1
        jmp     short fm_sw
fm_swnc:
        cmp     al,'B'
        jne     fm_swnb
        ; /B KEPT ROOM FOR TWO SYSTEM FILES that had to be the first
        ; things on the disk and in one piece, so that a later SYS
        ; could put them there.  PM-DOS's loader follows the FAT, SYS
        ; copies its files wherever there is room, and so there is
        ; nothing to keep.  The switch is taken so that a batch file
        ; written for another DOS runs
fm_swb:
        mov     byte ptr [bflag],1
        jmp     short fm_sw
fm_swnb:
        cmp     al,'F'
        jne     fm_badsw
        call    fm_size
        jc      fm_badsize
        jmp     fm_sw
fm_swstep:
        inc     esi
        dec     ecx
        jmp     fm_sw
fm_argsdone:
        mov     al,[bflag]              ; room for a system, and the
        and     al,[sysit]              ; system: one or the other
        jnz     fm_clash

;--- what is this letter? --------------------------------------------
        movzx   edx,byte ptr [tgt_log]
        mov     edi,OFFSET drv_rec
        mov     eax,0F106h
        int     21h
        jc      fm_baddrv
        test    byte ptr [drv_rec+DRV_FLAGS],DRVF_PRESENT
        jz      fm_baddrv
        mov     al,[drv_rec+DRV_FLAGS]
        test    al,DRVF_FLOPPY
        jz      fm_fixed
        test    al,DRVF_DRIVER OR DRVF_IMAGE OR DRVF_CDROM
        jnz     fm_fixed
        mov     al,[drv_rec+DRV_BIOS]
        cmp     al,2
        jae     fm_fixed
        mov     [fd_bios],al
        mov     byte ptr [floppy],1
        jmp     fm_diskette

;=====================================================================
; Anything that is not a diskette drive
;=====================================================================
fm_fixed:
        cmp     byte ptr [fsize],NO_SIZE
        jne     fm_nosize               ; /F: is a diskette's

;--- the current drive cannot be pulled out from under itself --------
        mov     ah,19h
        int     21h
        cmp     al,[tgt_log]
        je      fm_iscur

;--- FAT type - which also proves the volume mounts ------------------
        mov     dl,[tgt_log]
        inc     dl                      ; F1h speaks 1-based drives
        mov     ax,0F101h
        int     21h
        jc      fm_notmnt
        mov     [fat_kind],al

;--- the warning -----------------------------------------------------
        mov     edx,OFFSET msg_warn
        call    puts
        mov     ah,01h
        int     21h
        call    fm_upcase
        push    eax
        call    fm_crlf
        pop     eax
        cmp     al,'Y'
        jne     fm_quit

        call    fm_readbpb
        jc      fm_ioerr
        jmp     fm_reformat

;=====================================================================
; A diskette
;=====================================================================
fm_diskette:
        mov     edx,OFFSET msg_insert
        call    puts
fmd_key:
        mov     ah,08h
        int     21h
        cmp     al,0Dh
        jne     fmd_key
        call    fm_crlf

        ; what the drive is
        movzx   edx,byte ptr [fd_bios]
        mov     eax,(FDF_TYPE SHL 16) OR 0F114h
        int     21h
        jc      fm_baddrv
        mov     [fd_type],bl

        ; what is in it.  The remount is for the disk that was just
        ; put in: the kernel has not been told that it is a different
        ; one, and the answer wanted is about this one
        mov     dl,[tgt_log]
        inc     dl
        mov     ax,0F102h
        int     21h
        jc      fmd_blank
        mov     dl,[tgt_log]
        inc     dl
        mov     ax,0F101h
        int     21h
        jc      fmd_blank
        mov     [fat_kind],al
        call    fm_readbpb
        jc      fmd_blank
        call    fd_which                ; AL = its row in fd_sizes
        jc      fmd_blank
        mov     [fd_have],al
        mov     byte ptr [has_fs],1
fmd_blank:

        ; what is wanted: /F:, or what is there, or what the drive is
        mov     al,[fsize]
        cmp     al,NO_SIZE
        jne     fmd_want
        mov     al,[fd_have]
        cmp     byte ptr [has_fs],0
        jne     fmd_want
        movzx   eax,byte ptr [fd_type]
        mov     al,[fd_default+eax]
fmd_want:
        mov     [fd_want],al

        ; ...and whether this drive can make one
        movzx   ecx,al
        movzx   eax,byte ptr [fd_type]
        movzx   eax,byte ptr [fd_can+eax]
        bt      eax,ecx
        jnc     fm_nosize

        movzx   esi,byte ptr [fd_want]
        imul    esi,FS_LEN
        add     esi,OFFSET fd_sizes
        mov     [fd_row],esi

        ; every track, if the format that is there will not do
        cmp     byte ptr [has_fs],0
        je      fmd_tracks
        mov     al,[fd_want]
        cmp     al,[fd_have]
        jne     fmd_tracks
        cmp     byte ptr [uncond],0
        je      fmd_reuse
        cmp     byte ptr [quick],0
        je      fmd_tracks
fmd_reuse:
        mov     edx,OFFSET msg_quicking
        cmp     byte ptr [quick],0
        jne     fmd_say
        mov     edx,OFFSET msg_checking
fmd_say:
        call    puts
        call    fd_sayname
        jmp     fm_reformat

fmd_tracks:
        cmp     byte ptr [quick],0
        je      fmd_notq
        mov     edx,OFFSET msg_noquick
        call    puts
        mov     byte ptr [quick],0
fmd_notq:
        mov     edx,OFFSET msg_formatting
        call    puts
        call    fd_sayname
        jmp     fd_lowlevel

;=====================================================================
; fd_lowlevel - every track of the diskette, and then a filesystem
;               from fd_row.
;=====================================================================
fd_lowlevel:
        mov     byte ptr [raw_io],1
        mov     byte ptr [fresh],1
        mov     esi,[fd_row]

        movzx   edx,byte ptr [fd_bios]
        mov     cl,[esi+FS_SPT]
        mov     ch,[esi+FS_CYLS]
        movzx   ebx,byte ptr [esi+FS_MEDIUM]
        mov     eax,(FDF_BEGIN SHL 16) OR 0F114h
        int     21h
        mov     bl,al                   ; the BIOS's status, if it failed
        jc      fdl_failed

        movzx   eax,byte ptr [esi+FS_CYLS]
        movzx   ebx,byte ptr [esi+FS_HEADS]
        imul    eax,ebx
        mov     [v_total],eax           ; in tracks, this time
        mov     dword ptr [v_done],0
        mov     byte ptr [v_pct],0FFh

        xor     ebp,ebp                 ; EBP = the track, counting both
fdl_track:                              ; sides
        cmp     ebp,[v_total]
        jae     fdl_done
        mov     esi,[fd_row]
        mov     eax,ebp
        xor     edx,edx
        movzx   ebx,byte ptr [esi+FS_HEADS]
        div     ebx                     ; EAX = cylinder, EDX = head
        mov     ch,al
        mov     dh,dl
        mov     dl,[fd_bios]
        mov     cl,[esi+FS_SPT]
        push    eax
        mov     eax,(FDF_TRACK SHL 16) OR 0F114h
        int     21h
        mov     ebx,eax                 ; BL = the BIOS's status
        pop     eax
        jnc     fdl_next

        cmp     bl,03h                  ; write protected
        je      fdl_stop
        cmp     bl,80h                  ; the drive is empty
        je      fdl_stop
        or      eax,eax
        jz      fdl_track0              ; the system area is on cylinder 0
        bts     dword ptr [bad_tracks],ebp
fdl_next:
        inc     ebp
        mov     [v_done],ebp
        call    fm_percent
        jmp     fdl_track

fdl_done:
        call    fm_wipepct
        call    fdl_end

        ; the block layer is told what the disk now is, because the
        ; sectors that follow are addressed by number
        mov     esi,[fd_row]
        movzx   edx,byte ptr [esi+FS_HEADS]
        shl     edx,8
        mov     dl,[fd_bios]            ; DL = drive.  The heads go in
        movzx   ecx,byte ptr [esi+FS_SPT]
        movzx   ebx,word ptr [esi+FS_TOTAL]
        call    fd_setgeo
        jc      fm_ioerr

        call    fd_newbpb
        call    fd_newboot
        xor     edx,edx
        mov     ecx,1
        mov     ebx,[boot_sec]
        call    sec_write
        jc      fm_track0
        jmp     fm_build

fdl_stop:
        push    ebx
        call    fm_wipepct
        call    fdl_end
        pop     ebx
fdl_failed:
        mov     edx,OFFSET msg_wprot
        cmp     bl,03h
        je      fm_die
        mov     edx,OFFSET msg_notrdy
        cmp     bl,80h
        je      fm_die
        mov     edx,OFFSET msg_nosize
        cmp     bl,0Ch
        je      fm_die
        jmp     fm_ioerr
fdl_track0:
        call    fm_wipepct
        call    fdl_end
fm_track0:
        mov     edx,OFFSET msg_track0
        jmp     fm_die

fdl_end:
        movzx   edx,byte ptr [fd_bios]
        mov     eax,(FDF_END SHL 16) OR 0F114h
        int     21h
        ret

;---------------------------------------------------------------------
; fd_setgeo - DL = drive, DH = heads, CL = sectors a track, EBX =
;             sectors in all.  The call wants the heads in DX and the
;             drive in DL, and cannot have both: the kernel takes the
;             drive from DL and the heads from the byte above it.
;---------------------------------------------------------------------
fd_setgeo:
        mov     eax,(FDF_SETGEO SHL 16) OR 0F114h
        int     21h
        ret

;=====================================================================
; fm_reformat - the volume has a format and keeps it.  g_* are set.
;=====================================================================
fm_reformat:
        mov     byte ptr [raw_io],0
        mov     byte ptr [fresh],0
        ; fall into

;=====================================================================
; fm_build - FATs, root directory, surface check, label.  The geometry
;            is in g_*, and sec_read and sec_write know the way.
;=====================================================================
fm_build:
        ; WHOSE MARKS ARE KEPT.  /C says test them, /U says trust
        ; nothing - except that a quick format tests nothing either,
        ; so under /Q the marks are all there is, and /U alone does
        ; not throw them away
        mov     byte ptr [keepmarks],1
        cmp     byte ptr [retest],0
        jne     fmb_nokeep
        cmp     byte ptr [uncond],0
        je      fmb_keepset
        cmp     byte ptr [quick],0
        jne     fmb_keepset
fmb_nokeep:
        mov     byte ptr [keepmarks],0
fmb_keepset:

        cmp     byte ptr [fresh],0
        jne     fmb_fresh
        call    fat_carry
        jc      fm_ioerr
        jmp     short fmb_fatsdone
fmb_fresh:
        call    fat_fresh
        jc      fm_ioerr
fmb_fatsdone:

        cmp     byte ptr [fat_kind],32  ; FAT32: the root cluster is a
        jne     fmf_done                ; one-cluster chain
        mov     eax,[g_rootclus]
        mov     ebx,0FFFFFFFh
        call    mark_fat
        jc      fm_ioerr
fmf_done:

;=====================================================================
; Fresh root directory
;=====================================================================
        cmp     byte ptr [fat_kind],32
        je      fmr_32
        mov     eax,[g_rootstart]
        mov     ecx,[g_rootsecs]
        call    zero_range
        jc      fm_ioerr
        jmp     fmr_done
fmr_32:
        call    root32_lba
        mov     ecx,[g_spc]
        call    zero_range
        jc      fm_ioerr

        ; and a fresh FSInfo, plus the backup copy the layout keeps
        mov     edi,[fat_work]
        push    edi
        mov     ecx,128
        xor     eax,eax
        rep     stosd
        pop     edi
        mov     dword ptr [edi],41615252h       ; "RRaA"
        mov     dword ptr [edi+1E4h],61417272h  ; "rrAa"
        mov     eax,[g_totclus]
        dec     eax                             ; the root took one
        sub     eax,[v_bad]                     ; and the marks the rest
        mov     [edi+1E8h],eax                  ; free clusters
        mov     eax,[g_rootclus]
        inc     eax
        mov     [edi+1ECh],eax                  ; next-free hint
        mov     dword ptr [edi+1FCh],0AA550000h
        mov     edx,[g_fsinfo]
        mov     ebx,[fat_work]
        mov     ecx,1
        call    sec_write
        jc      fm_ioerr
        mov     edx,[g_bkboot]
        or      edx,edx
        jz      fmr_done
        add     edx,[g_fsinfo]                  ; the mirrored copy
        mov     ebx,[fat_work]
        mov     ecx,1
        call    sec_write
        jc      fm_ioerr
fmr_done:

;=====================================================================
; The verify pass - every data sector is read and the bad ones'
; clusters marked in the fresh FAT.  /Q skips it, and so does a
; diskette whose tracks were each read back as they were formatted.
;=====================================================================
        cmp     byte ptr [quick],0
        jne     fm_label
        cmp     byte ptr [fresh],0
        jne     fm_label

        mov     eax,[g_totsec]
        sub     eax,[g_datastart]
        mov     [v_total],eax
        mov     dword ptr [v_done],0
        mov     byte ptr [v_pct],0FFh
        mov     edx,[g_datastart]
fmv_lp:
        mov     eax,[v_total]
        sub     eax,[v_done]
        jz      fmv_end
        mov     ecx,CHUNK_SECS
        cmp     eax,ecx
        jae     fmv_have
        mov     ecx,eax
fmv_have:
        mov     ebx,[chunk_buf]
        call    sec_read
        jnc     fmv_good
        call    verify_hard             ; sector by sector, marking
        jc      fm_ioerr
fmv_good:
        add     edx,ecx
        add     [v_done],ecx
        call    fm_percent
        jmp     fmv_lp
fmv_end:
        call    fm_wipepct

;=====================================================================
; Volume label
;=====================================================================
fm_label:
        mov     edx,OFFSET msg_label
        call    puts
        mov     edx,OFFSET lbl_in
        mov     byte ptr [lbl_in],12
        mov     ah,0Ah
        int     21h
        call    fm_crlf
        movzx   ecx,byte ptr [lbl_in+1]
        or      ecx,ecx
        jz      fm_finish

        ; the label entry goes into the empty root's first sector
        cmp     byte ptr [fat_kind],32
        jne     fml_16
        call    root32_lba
        jmp     short fml_have
fml_16:
        mov     eax,[g_rootstart]
fml_have:
        mov     [lbl_lba],eax
        mov     edx,eax
        mov     ebx,[fat_work]
        mov     ecx,1
        call    sec_read
        jc      fm_ioerr

        mov     edi,[fat_work]          ; name, upcased, space-padded
        mov     esi,OFFSET lbl_in + 2
        movzx   ecx,byte ptr [lbl_in+1]
        cmp     ecx,11
        jbe     fml_cp
        mov     ecx,11
fml_cp:
        push    ecx
        mov     ecx,11
fml_pad:
        mov     byte ptr [edi],' '
        inc     edi
        loop    fml_pad
        sub     edi,11
        pop     ecx
fml_name:
        or      ecx,ecx
        jz      fml_named
        lodsb
        call    fm_upcase
        mov     [edi],al
        inc     edi
        dec     ecx
        jmp     short fml_name
fml_named:
        mov     edi,[fat_work]
        mov     byte ptr [edi+0Bh],ATTR_VOLUME

        mov     ah,2Ch                  ; stamp it with now
        int     21h
        movzx   eax,ch                  ; hours
        shl     eax,6
        movzx   ebx,cl                  ; minutes
        or      eax,ebx
        shl     eax,5
        movzx   ebx,dh                  ; seconds
        shr     ebx,1
        or      eax,ebx
        mov     [edi+16h],ax
        mov     ah,2Ah
        int     21h
        movzx   eax,cx                  ; the year
        sub     eax,1980
        shl     eax,4
        movzx   ebx,dh                  ; month
        or      eax,ebx
        shl     eax,5
        movzx   ebx,dl                  ; day
        or      eax,ebx
        mov     [edi+18h],ax

        mov     edx,[lbl_lba]
        mov     ebx,[fat_work]
        mov     ecx,1
        call    sec_write
        jc      fm_ioerr

        ; a boot sector FORMAT wrote carries the label as well
        cmp     byte ptr [fresh],0
        je      fm_finish
        mov     esi,[fat_work]
        mov     edi,[boot_sec]
        add     edi,2Bh
        mov     ecx,11
        rep     movsb
        xor     edx,edx
        mov     ebx,[boot_sec]
        mov     ecx,1
        call    sec_write
        jc      fm_ioerr

;=====================================================================
; Tell the kernel, and report what the volume now holds
;=====================================================================
fm_finish:
        mov     dl,[tgt_log]
        inc     dl
        mov     ax,0F102h               ; remount: fresh BPB, caches
        int     21h                     ; discarded, cwd at the root
        jc      fm_ioerr

        mov     edx,OFFSET msg_done
        call    puts

        ; FORMAT's OWN numbers, not 21h/36h's: that call reports
        ; clusters in a 16-bit register, so it saturates at 65535 and
        ; would call this 40MB volume a 32MB one.  The program that
        ; just laid the volume out knows the truth exactly.
        mov     eax,[g_spc]
        shl     eax,9                   ; * 512 bytes per sector
        mov     [v_cbytes],eax
        mov     eax,[g_totclus]
        imul    eax,[v_cbytes]
        call    pdec
        mov     edx,OFFSET msg_total
        call    puts

        mov     eax,[g_totclus]
        sub     eax,[v_bad]
        cmp     byte ptr [fat_kind],32
        jne     fmx_used
        dec     eax                     ; FAT32's root took a cluster
fmx_used:
        mov     [v_free],eax

        cmp     dword ptr [v_bad],0
        je      fmx_free
        mov     eax,[v_bad]
        imul    eax,[v_cbytes]
        call    pdec
        mov     edx,OFFSET msg_bad
        call    puts
fmx_free:
        mov     eax,[v_free]
        imul    eax,[v_cbytes]
        call    pdec
        mov     edx,OFFSET msg_avail
        call    puts

        cmp     byte ptr [fresh],0      ; a serial number is the boot
        je      fmx_noser               ; sector's, and only a new one
        mov     edx,OFFSET msg_serial   ; has a new one
        call    puts
        mov     esi,[boot_sec]
        movzx   eax,word ptr [esi+29h]
        call    phex4
        mov     dl,'-'
        mov     ah,02h
        int     21h
        movzx   eax,word ptr [esi+27h]
        call    phex4
        call    fm_crlf
fmx_noser:
        cmp     byte ptr [sysit],0
        jne     fm_sys
fm_exit:
        mov     ax,4C00h
        int     21h

;=====================================================================
; /S - and it is SYS.COM that does it, run as a child.
;
; NOT A COPY OF SYS INSIDE FORMAT.  Making a disk bootable is three
; jobs - a boot sector with the target's own BPB spliced into it, the
; stage-2 loader in the reserved sectors, and three files copied and
; re-attributed - and SYS.ASM does all three already, correctly, on
; every FAT type.  A second implementation here would be a second
; thing to keep right, and the one that got it wrong would be the one
; nobody ran.  Real FORMAT carried the code because real DOS had no
; nested EXEC worth the name; this one has had one since v0.18.
;
; WHERE SYS.COM IS is the only question, and COMSPEC answers it: the
; interpreter is on the system disk by definition, so the directory it
; came from is the directory the system came from.  If the environment
; will not say, "SYS.COM" on its own is tried, which finds it in the
; current directory - and a plain "not found" from EXEC is a good
; enough message for a disk that has been formatted correctly and
; simply cannot be made bootable from here.
;=====================================================================
fm_sys:
        mov     edx,OFFSET msg_sysing
        call    puts

        call    fm_syspath              ; sys_path = where SYS.COM is
        mov     edi,OFFSET sys_tail     ; " d:" as a DOS command tail
        mov     byte ptr [edi],3
        mov     byte ptr [edi+1],' '
        mov     al,[tgt_log]
        add     al,'A'
        mov     [edi+2],al
        mov     byte ptr [edi+3],':'
        mov     byte ptr [edi+4],0Dh

        ; THE NATIVE PARAMETER BLOCK, WHICH IS NOT THE EP_* ONE.  A
        ; 16-bit program's block is a WORD environment segment and
        ; three far pointers; a D32 program's is two flat dwords, an
        ; environment and a tail, and nothing else - there are no FCBs
        ; to parse for a program that has no PSP fields to put them in.
        ; Building the sixteen-bit shape put the tail two bytes from
        ; where fn4B reads it, and SYS came up with no arguments and
        ; printed its usage.
        mov     dword ptr [sys_blk],0           ; no environment: SYS wants
                                                ; nothing out of one
        mov     dword ptr [sys_blk+4],OFFSET sys_tail
        mov     edx,OFFSET sys_path
        mov     ebx,OFFSET sys_blk
        mov     ax,4B00h
        int     21h
        jc      fm_sysfail
        jmp     short fm_exit
fm_sysfail:
        mov     edx,OFFSET msg_nosys
        call    puts
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; fm_syspath - sys_path = COMSPEC's directory + "SYS.COM", or just
;              "SYS.COM" when there is no COMSPEC to read.
;---------------------------------------------------------------------
fm_syspath:
        push    eax
        push    ecx
        push    esi
        push    edi
        mov     edi,OFFSET sys_path
        xor     edx,edx                 ; EDX = length of the directory part

        ; ds: IS LOAD-BEARING here for the reason it is everywhere else
        ; a PSP field is read - without the override wasm biases the
        ; address by the link offset and the environment silently is
        ; not there.  See inherit_env in COMMAND.ASM.
        mov     esi,ds:[SLOT_PSP + PSP_ENV]
        or      esi,esi
        jz      fsp_bare
fsp_var:
        cmp     byte ptr [esi],0
        je      fsp_bare                ; the end of the block
        push    esi
        mov     ecx,OFFSET fm_comspec
fsp_cmp:
        mov     al,[ecx]
        or      al,al
        jz      fsp_hit
        cmp     al,[esi]
        jne     fsp_next
        inc     esi
        inc     ecx
        jmp     short fsp_cmp
fsp_next:
        pop     esi
fsp_skip:
        mov     al,[esi]
        inc     esi
        or      al,al
        jnz     fsp_skip
        jmp     short fsp_var
fsp_hit:
        add     esp,4                   ; the pushed ESI is finished with
                                        ; ...and ESI is ALREADY past the '=':
                                        ; fm_comspec carries it, so the
                                        ; comparison consumed it.  Stepping
                                        ; over it again ate the drive letter
                                        ; and left ":\SYS.COM"
        xor     ecx,ecx
fsp_scan:
        mov     al,[esi+ecx]
        or      al,al
        jz      fsp_cut
        mov     [edi+ecx],al
        inc     ecx
        cmp     al,'\'
        je      fsp_mark
        cmp     al,':'
        jne     fsp_scan
fsp_mark:
        mov     edx,ecx                 ; keep everything up to here
        jmp     short fsp_scan
fsp_cut:
        mov     edi,OFFSET sys_path
        add     edi,edx
fsp_bare:
        mov     esi,OFFSET fm_sysname
fsp_name:
        lodsb
        stosb
        or      al,al
        jnz     fsp_name
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        ret
fm_quit:
        mov     ax,4C01h
        int     21h

;--- help, which is not an error -------------------------------------
fm_help:
        mov     edx,OFFSET msg_usage
        call    puts
        mov     ax,4C00h
        int     21h

;--- errors ----------------------------------------------------------
fm_usage:
        mov     edx,OFFSET msg_usage
        jmp     fm_die
fm_baddrv:
        mov     edx,OFFSET msg_baddrv
        jmp     fm_die
fm_badsw:
        mov     edx,OFFSET msg_badsw
        jmp     fm_die
fm_badsize:
        mov     edx,OFFSET msg_badsize
        jmp     fm_die
fm_nosize:
        mov     edx,OFFSET msg_nosize
        jmp     fm_die
fm_clash:
        mov     edx,OFFSET msg_clash
        jmp     fm_die
fm_iscur:
        mov     edx,OFFSET msg_iscur
        jmp     fm_die
fm_notmnt:
        mov     edx,OFFSET msg_notmnt
        jmp     fm_die
fm_ioerr:
        mov     edx,OFFSET msg_ioerr
fm_die:
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; fm_size - the rest of /F: at ESI, ECX bytes left.  fsize = its row
;           in fd_sizes.  CF if it names no capacity there is.
;
; 1440, 1440K, 1440KB, 1.44, 1.44M and 1.44MB are one size, so the
; number is read as thousandths when it has a point in it and the
; letters after it are stepped over without being read.
;=====================================================================
fm_size:
        push    ebx
        push    edx
        push    edi
        or      ecx,ecx
        jz      fsz_bad
        cmp     byte ptr [esi],':'
        jne     fsz_num
        inc     esi
        dec     ecx
fsz_num:
        xor     ebx,ebx                 ; EBX = the number
        xor     edi,edi                 ; EDI = digits read
fsz_int:
        call    fsz_digit
        jc      fsz_intdone
        imul    ebx,10
        add     ebx,eax
        inc     edi
        cmp     ebx,9999
        ja      fsz_bad
        jmp     short fsz_int
fsz_intdone:
        or      edi,edi
        jz      fsz_bad
        or      ecx,ecx
        jz      fsz_look
        cmp     byte ptr [esi],'.'
        jne     fsz_unit
        inc     esi
        dec     ecx
        imul    ebx,1000
        mov     edx,100                 ; what the next digit is worth
fsz_frac:
        call    fsz_digit
        jc      fsz_unit
        imul    eax,edx
        add     ebx,eax
        mov     eax,edx
        xor     edx,edx
        push    ecx
        mov     ecx,10
        div     ecx
        pop     ecx
        mov     edx,eax
        jmp     short fsz_frac
fsz_unit:
        or      ecx,ecx
        jz      fsz_look
        mov     al,[esi]
        call    fm_upcase
        cmp     al,'K'
        je      fsz_letter
        cmp     al,'M'
        je      fsz_letter
        cmp     al,'B'
        jne     fsz_look
fsz_letter:
        inc     esi
        dec     ecx
        jmp     short fsz_unit
fsz_look:
        xor     edx,edx
        mov     edi,OFFSET fd_sizes
fsz_row:
        movzx   eax,word ptr [edi+FS_KB]
        cmp     eax,ebx
        je      fsz_found
        add     edi,FS_LEN
        inc     edx
        cmp     edx,FS_COUNT
        jb      fsz_row
fsz_bad:
        pop     edi
        pop     edx
        pop     ebx
        stc
        ret
fsz_found:
        mov     [fsize],dl
        pop     edi
        pop     edx
        pop     ebx
        clc
        ret

; the digit at ESI -> EAX and stepped over, or CF and nothing moved
fsz_digit:
        or      ecx,ecx
        jz      fsd_no
        movzx   eax,byte ptr [esi]
        sub     eax,'0'
        cmp     eax,9
        ja      fsd_no
        inc     esi
        dec     ecx
        clc
        ret
fsd_no:
        stc
        ret

;=====================================================================
; fm_readbpb - sector 0 of the volume into boot_sec, and its BPB into
;              g_*.  CF if it could not be read.
;=====================================================================
fm_readbpb:
        xor     edx,edx
        mov     ecx,1
        mov     ebx,[boot_sec]
        call    sec_read
        jc      frb_out
        mov     esi,[boot_sec]

        movzx   eax,word ptr [esi+0Eh]  ; reserved sectors
        mov     [g_rsvd],eax
        movzx   eax,byte ptr [esi+10h]  ; FAT copies
        mov     [g_nfats],eax
        movzx   eax,word ptr [esi+11h]  ; root entries (0 on FAT32)
        mov     [g_rootent],eax
        movzx   eax,byte ptr [esi+0Dh]  ; sectors per cluster
        mov     [g_spc],eax
        mov     al,[esi+15h]            ; media descriptor
        mov     [g_media],al
        movzx   eax,word ptr [esi+16h]  ; sectors per FAT...
        or      eax,eax
        jnz     frb_havespf
        mov     eax,dword ptr [esi+24h] ; ...FAT32 keeps it here
frb_havespf:
        mov     [g_spf],eax
        movzx   eax,word ptr [esi+13h]  ; total sectors...
        or      eax,eax
        jnz     frb_havetot
        mov     eax,dword ptr [esi+20h] ; ...or the 32-bit count
frb_havetot:
        mov     [g_totsec],eax
        mov     eax,dword ptr [esi+2Ch] ; FAT32 root cluster
        mov     [g_rootclus],eax
        movzx   eax,word ptr [esi+30h]  ; FAT32 FSInfo sector
        mov     [g_fsinfo],eax
        movzx   eax,word ptr [esi+32h]  ; FAT32 backup boot sector
        mov     [g_bkboot],eax
        movzx   eax,word ptr [esi+18h]
        mov     [g_spt],eax
        movzx   eax,word ptr [esi+1Ah]
        mov     [g_heads],eax

        cmp     dword ptr [g_spc],0     ; a block nothing can be divided
        je      frb_bad                 ; by is not a BPB
        cmp     dword ptr [g_nfats],0
        je      frb_bad
        call    fm_derive
        clc
frb_out:
        ret
frb_bad:
        stc
        ret

;---------------------------------------------------------------------
; fm_derive - the layout that follows from g_*, volume-relative like
;             INT 25h/26h
;---------------------------------------------------------------------
fm_derive:
        mov     eax,[g_nfats]
        imul    eax,[g_spf]
        add     eax,[g_rsvd]
        mov     [g_rootstart],eax       ; root area (FAT12/16)
        mov     ebx,[g_rootent]
        add     ebx,15
        shr     ebx,4                   ; * 32 / 512, rounded up
        mov     [g_rootsecs],ebx
        add     eax,ebx
        mov     [g_datastart],eax
        mov     ebx,[g_totsec]
        sub     ebx,eax
        mov     eax,ebx
        xor     edx,edx
        div     dword ptr [g_spc]
        mov     [g_totclus],eax
        ret

;=====================================================================
; fd_which - which row of fd_sizes is the BPB in g_*?  -> AL, or CF if
;            it is not a format FORMAT makes.
;=====================================================================
fd_which:
        push    ecx
        push    edi
        xor     ecx,ecx
        mov     edi,OFFSET fd_sizes
fdw_row:
        movzx   eax,word ptr [edi+FS_TOTAL]
        cmp     eax,[g_totsec]
        jne     fdw_next
        movzx   eax,byte ptr [edi+FS_SPT]
        cmp     eax,[g_spt]
        jne     fdw_next
        movzx   eax,byte ptr [edi+FS_HEADS]
        cmp     eax,[g_heads]
        je      fdw_found
fdw_next:
        add     edi,FS_LEN
        inc     ecx
        cmp     ecx,FS_COUNT
        jb      fdw_row
        pop     edi
        pop     ecx
        stc
        ret
fdw_found:
        mov     al,cl
        pop     edi
        pop     ecx
        clc
        ret

;---------------------------------------------------------------------
; fd_sayname - the capacity in fd_row, and the end of its line
;---------------------------------------------------------------------
fd_sayname:
        push    edx
        mov     edx,[fd_row]
        add     edx,FS_NAME
        call    puts
        call    fm_crlf
        pop     edx
        ret

;=====================================================================
; fd_newbpb - g_* from fd_row.  A diskette is FAT12 whatever its size.
;=====================================================================
fd_newbpb:
        mov     esi,[fd_row]
        mov     byte ptr [fat_kind],12
        mov     dword ptr [g_rsvd],1
        mov     dword ptr [g_nfats],2
        movzx   eax,word ptr [esi+FS_ROOTENT]
        mov     [g_rootent],eax
        movzx   eax,byte ptr [esi+FS_SPC]
        mov     [g_spc],eax
        mov     al,[esi+FS_MEDIA]
        mov     [g_media],al
        movzx   eax,byte ptr [esi+FS_SPF]
        mov     [g_spf],eax
        movzx   eax,word ptr [esi+FS_TOTAL]
        mov     [g_totsec],eax
        movzx   eax,byte ptr [esi+FS_SPT]
        mov     [g_spt],eax
        movzx   eax,byte ptr [esi+FS_HEADS]
        mov     [g_heads],eax
        call    fm_derive
        ret

;=====================================================================
; fd_newboot - a boot sector in boot_sec for the volume in g_*.
;
; It boots nothing: what it runs is a message and a wait for a key.
; SYS replaces it with one that loads the system, and takes the BPB
; across when it does.
;=====================================================================
fd_newboot:
        pushad
        mov     edi,[boot_sec]
        push    edi
        xor     eax,eax
        mov     ecx,128
        rep     stosd
        pop     edi
        mov     esi,OFFSET boot_stub
        mov     ecx,BOOT_STUBLEN
        rep     movsb

        mov     edi,[boot_sec]
        mov     word ptr [edi+0Bh],512
        mov     eax,[g_spc]
        mov     [edi+0Dh],al
        mov     eax,[g_rsvd]
        mov     [edi+0Eh],ax
        mov     eax,[g_nfats]
        mov     [edi+10h],al
        mov     eax,[g_rootent]
        mov     [edi+11h],ax
        mov     eax,[g_totsec]
        mov     [edi+13h],ax
        mov     al,[g_media]
        mov     [edi+15h],al
        mov     eax,[g_spf]
        mov     [edi+16h],ax
        mov     eax,[g_spt]
        mov     [edi+18h],ax
        mov     eax,[g_heads]
        mov     [edi+1Ah],ax

        ; the serial number: the clock, which is what makes two disks
        ; formatted a minute apart different disks
        mov     ah,2Ch
        int     21h
        mov     ebx,ecx
        shl     ebx,16
        mov     bx,dx
        mov     ah,2Ah
        int     21h
        shl     ecx,16
        mov     cx,dx
        add     ebx,ecx
        mov     edi,[boot_sec]
        mov     [edi+27h],ebx
        mov     word ptr [edi+1FEh],0AA55h
        popad
        ret

;=====================================================================
; fat_fresh - FATs for a volume nothing is carried over from.  The
;             tracks in bad_tracks are marked.  CF on I/O error.
;
; Only a diskette comes this way, so the FAT is FAT12 and all of it
; fits the work buffer.
;=====================================================================
fat_fresh:
        pushad
        mov     edi,[fat_work]
        mov     ecx,[g_spf]
        shl     ecx,7
        xor     eax,eax
        rep     stosd
        mov     esi,[fat_work]
        mov     al,[g_media]
        mov     [esi],al
        mov     word ptr [esi+1],0FFFFh

        xor     ebp,ebp                 ; EBP = track
ffr_track:
        cmp     ebp,160
        jae     ffr_write
        bt      dword ptr [bad_tracks],ebp
        jnc     ffr_next
        mov     eax,ebp
        imul    eax,[g_spt]             ; its first sector
        mov     ecx,[g_spt]
ffr_sec:
        cmp     eax,[g_datastart]
        jb      ffr_secnext
        push    eax
        sub     eax,[g_datastart]
        xor     edx,edx
        div     dword ptr [g_spc]
        add     eax,2
        mov     edi,eax
        call    get12
        cmp     eax,0FF7h
        je      ffr_marked
        mov     eax,edi
        mov     ebx,0FF7h
        call    set12
        inc     dword ptr [v_bad]
ffr_marked:
        pop     eax
ffr_secnext:
        inc     eax
        dec     ecx
        jnz     ffr_sec
ffr_next:
        inc     ebp
        jmp     short ffr_track

ffr_write:
        mov     esi,[fat_work]
        call    fat_putall
        jc      ffr_fail
        popad
        clc
        ret
ffr_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; fat_putall - the whole FAT at ESI, to every copy.  CF on I/O error.
;---------------------------------------------------------------------
fat_putall:
        pushad
        xor     ebp,ebp
fpa_copy:
        mov     eax,ebp
        imul    eax,[g_spf]
        add     eax,[g_rsvd]
        mov     edx,eax
        mov     ebx,esi
        mov     ecx,[g_spf]
        call    sec_write
        jc      fpa_fail
        inc     ebp
        cmp     ebp,[g_nfats]
        jb      fpa_copy
        popad
        clc
        ret
fpa_fail:
        popad
        stc
        ret

;=====================================================================
; fat_carry - FATs for a volume that had a format: every entry freed
;             but the ones marked bad, which stay marked, or under /C
;             and /Q together are tested here and now.  CF on I/O
;             error.
;
; A FAT THAT CANNOT BE READ CARRIES NOTHING.  Its marks are lost with
; it, and the surface check that follows a full format finds them
; again; after a quick one they are simply gone, which is the price
; of quick.
;=====================================================================
fat_carry:
        pushad
        cmp     byte ptr [fat_kind],12
        je      fcy_12

        ; FAT16 and FAT32: a run of sectors at a time
        mov     dword ptr [fc_esize],2
        mov     dword ptr [fc_bad],0FFF7h
        mov     dword ptr [fc_mask],0FFFFh
        cmp     byte ptr [fat_kind],32
        jne     fcy_sized
        mov     dword ptr [fc_esize],4
        mov     dword ptr [fc_bad],0FFFFFF7h
        mov     dword ptr [fc_mask],0FFFFFFFh
fcy_sized:
        xor     ebp,ebp                 ; EBP = sector within the FAT
fcy_run:
        mov     ecx,[g_spf]
        sub     ecx,ebp
        jz      fcy_done
        cmp     ecx,CHUNK_SECS
        jbe     fcy_n
        mov     ecx,CHUNK_SECS
fcy_n:
        mov     [fc_n],ecx
        mov     edx,[g_rsvd]
        add     edx,ebp
        mov     ebx,[fat_work]
        call    sec_read
        jnc     fcy_got
        mov     edi,[fat_work]
        mov     ecx,[fc_n]
        shl     ecx,7
        xor     eax,eax
        rep     stosd
fcy_got:
        ; the entries in this run, and the cluster the first one is
        mov     eax,ebp
        shl     eax,9
        xor     edx,edx
        div     dword ptr [fc_esize]
        mov     edi,eax                 ; EDI = cluster
        mov     eax,[fc_n]
        shl     eax,9
        xor     edx,edx
        div     dword ptr [fc_esize]
        mov     ecx,eax                 ; ECX = entries
        mov     esi,[fat_work]
fcy_entry:
        mov     eax,[esi]
        and     eax,[fc_mask]
        xor     ebx,ebx                 ; EBX = what it becomes
        cmp     eax,[fc_bad]
        jne     fcy_put
        call    fc_keep
        jc      fcy_put
        mov     ebx,[fc_bad]
fcy_put:
        cmp     dword ptr [fc_esize],4
        je      fcy_put32
        mov     [esi],bx
        jmp     short fcy_step
fcy_put32:
        mov     [esi],ebx
fcy_step:
        add     esi,[fc_esize]
        inc     edi
        dec     ecx
        jnz     fcy_entry

        or      ebp,ebp                 ; the two entries that are not
        jnz     fcy_out                 ; clusters
        mov     esi,[fat_work]
        movzx   eax,byte ptr [g_media]
        cmp     dword ptr [fc_esize],4
        je      fcy_res32
        or      eax,0FF00h
        mov     [esi],ax
        mov     word ptr [esi+2],0FFFFh
        jmp     short fcy_out
fcy_res32:
        or      eax,0FFFFF00h
        mov     [esi],eax
        mov     dword ptr [esi+4],0FFFFFFFFh
fcy_out:
        xor     esi,esi                 ; ESI = copy
fcy_copy:
        mov     eax,esi
        imul    eax,[g_spf]
        add     eax,[g_rsvd]
        add     eax,ebp
        mov     edx,eax
        mov     ebx,[fat_work]
        mov     ecx,[fc_n]
        call    sec_write
        jc      fcy_fail
        inc     esi
        cmp     esi,[g_nfats]
        jb      fcy_copy
        add     ebp,[fc_n]
        jmp     fcy_run

        ; FAT12: all of it at once, and the new one built beside it
fcy_12:
        cmp     dword ptr [g_spf],CHUNK_SECS
        ja      fcy_fail
        mov     edx,[g_rsvd]
        mov     ebx,[fat_work]
        mov     ecx,[g_spf]
        call    sec_read
        jnc     fcy_12got
        mov     edi,[fat_work]
        mov     ecx,[g_spf]
        shl     ecx,7
        xor     eax,eax
        rep     stosd
fcy_12got:
        mov     edi,[chunk_buf]
        mov     ecx,[g_spf]
        shl     ecx,7
        xor     eax,eax
        rep     stosd
        mov     dword ptr [fc_bad],0FF7h
        mov     edi,2                   ; EDI = cluster
fcy_12lp:
        mov     eax,[g_totclus]
        add     eax,2
        cmp     edi,eax
        jae     fcy_12done
        mov     esi,[fat_work]
        mov     eax,edi
        call    get12
        cmp     eax,0FF7h
        jne     fcy_12next
        call    fc_keep
        jc      fcy_12next
        mov     esi,[chunk_buf]
        mov     eax,edi
        mov     ebx,0FF7h
        call    set12
fcy_12next:
        inc     edi
        jmp     short fcy_12lp
fcy_12done:
        mov     esi,[chunk_buf]
        mov     al,[g_media]
        mov     [esi],al
        mov     word ptr [esi+1],0FFFFh
        call    fat_putall
        jc      fcy_fail
fcy_done:
        popad
        clc
        ret
fcy_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; fc_keep - cluster EDI was marked bad.  CF clear if the mark stays,
;           and it has been counted.  Everything kept.
;---------------------------------------------------------------------
fc_keep:
        push    eax
        cmp     edi,2
        jb      fck_no
        mov     eax,[g_totclus]
        add     eax,2
        cmp     edi,eax
        jae     fck_no                  ; past the end: not a cluster
        cmp     byte ptr [keepmarks],0
        jne     fck_yes
        cmp     byte ptr [quick],0
        je      fck_no                  ; the surface check will say
        call    fc_test
        jnc     fck_no                  ; it reads: the mark was wrong
fck_yes:
        inc     dword ptr [v_bad]
        pop     eax
        clc
        ret
fck_no:
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; fc_test - read every sector of cluster EDI.  CF if one will not.
;---------------------------------------------------------------------
fc_test:
        pushad
        mov     eax,edi
        sub     eax,2
        imul    eax,[g_spc]
        add     eax,[g_datastart]
        mov     edx,eax
        mov     esi,[g_spc]
fct_lp:
        mov     ebx,[scratch]
        mov     ecx,1
        call    sec_read
        jc      fct_bad
        inc     edx
        dec     esi
        jnz     fct_lp
        popad
        clc
        ret
fct_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; get12 - ESI = a FAT12 FAT, EAX = cluster -> EAX = its entry
; set12 - ...and EBX = what to make it
;---------------------------------------------------------------------
get12:
        push    ebx
        mov     ebx,eax
        shr     ebx,1
        add     ebx,eax                 ; cluster * 3 / 2
        movzx   ebx,word ptr [esi+ebx]
        test    al,1
        jz      g12_even
        shr     ebx,4
g12_even:
        and     ebx,0FFFh
        mov     eax,ebx
        pop     ebx
        ret

set12:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     edx,eax
        shr     edx,1
        add     edx,eax
        mov     cx,[esi+edx]
        and     ebx,0FFFh
        test    al,1
        jnz     s12_odd
        and     cx,0F000h
        or      cx,bx
        jmp     short s12_put
s12_odd:
        and     cx,000Fh
        shl     ebx,4
        or      cx,bx
s12_put:
        mov     [esi+edx],cx
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; sec_read / sec_write - ECX sectors at volume sector EDX, to or from
;                        EBX.  CF on error.  All registers kept.
;
; A volume with a letter goes by INT 25h and 26h, which are the
; kernel's own path and keep its caches honest.  A diskette that is
; being made has no letter to go by, and is the one volume whose
; sectors and whose drive's sectors are the same numbers.
;=====================================================================
sec_read:
        pushad
        cmp     byte ptr [raw_io],0
        jne     srd_raw
        mov     al,[tgt_log]
        int     25h
        jmp     short sec_out
srd_raw:
        mov     eax,0F10Dh
        jmp     short sec_raw
sec_write:
        pushad
        cmp     byte ptr [raw_io],0
        jne     swr_raw
        mov     al,[tgt_log]
        int     26h
        jmp     short sec_out
swr_raw:
        mov     eax,0F10Eh
sec_raw:
        mov     edi,ebx
        mov     ebx,edx
        movzx   edx,byte ptr [fd_bios]
        int     21h
sec_out:
        popad
        ret

;---------------------------------------------------------------------
; zero_range - ECX sectors of zeroes starting at volume sector EAX.
;              CF on I/O error.
;---------------------------------------------------------------------
zero_range:
        pushad
        push    eax
        push    ecx
        mov     edi,[chunk_buf]         ; which has been read into since
        mov     ecx,CHUNK_SECS * 128
        xor     eax,eax
        rep     stosd
        pop     ecx
        pop     eax
        mov     edx,eax
zr_lp:
        or      ecx,ecx
        jz      zr_ok
        mov     ebp,CHUNK_SECS
        cmp     ecx,ebp
        jae     zr_have
        mov     ebp,ecx
zr_have:
        push    ecx
        mov     ebx,[chunk_buf]
        mov     ecx,ebp
        call    sec_write
        pop     ecx
        jc      zr_fail
        add     edx,ebp
        sub     ecx,ebp
        jmp     short zr_lp
zr_ok:
        popad
        clc
        ret
zr_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; mark_fat - write EBX into cluster EAX's FAT entry, in every copy.
;            CF on I/O error.  mf_same says the entry held that value
;            already.  FAT12 rewrites the whole (small) FAT; FAT16/32
;            read-modify-write one sector.
;---------------------------------------------------------------------
mark_fat:
        pushad
        mov     byte ptr [mf_same],0
        cmp     byte ptr [fat_kind],12
        je      mf_12
        mov     esi,eax                 ; ESI = cluster
        mov     edi,ebx                 ; EDI = value
        shl     eax,1                   ; byte offset: *2 or *4
        cmp     byte ptr [fat_kind],32
        jne     mf_ofs
        shl     eax,1
mf_ofs:
        mov     ebp,eax
        shr     ebp,9                   ; EBP = sector within a copy
        mov     edx,ebp
        add     edx,[g_rsvd]
        mov     ebx,[fat_work]          ; copy 0 supplies the sector
        mov     ecx,1
        call    sec_read
        jc      mf_fail
        shl     esi,1                   ; the offset again (EAX gone)
        cmp     byte ptr [fat_kind],32
        jne     mf_ofs2
        shl     esi,1
mf_ofs2:
        and     esi,1FFh
        add     esi,[fat_work]
        cmp     byte ptr [fat_kind],32
        je      mf_32
        mov     eax,edi
        cmp     [esi],ax
        je      mf_held
        mov     [esi],ax
        jmp     short mf_wr
mf_32:
        mov     eax,[esi]
        and     eax,0FFFFFFFh
        cmp     eax,edi
        je      mf_held
        mov     [esi],edi
mf_wr:
        xor     esi,esi                 ; ESI = copy number now
mf_wlp:
        mov     eax,esi
        imul    eax,[g_spf]
        add     eax,[g_rsvd]
        add     eax,ebp
        mov     edx,eax
        mov     ebx,[fat_work]
        mov     ecx,1
        call    sec_write
        jc      mf_fail
        inc     esi
        cmp     esi,[g_nfats]
        jb      mf_wlp
        popad
        clc
        ret

mf_12:
        ; the whole FAT12 FAT fits the work buffer with room to spare
        mov     edi,eax
        mov     edx,[g_rsvd]
        mov     ebx,[fat_work]
        mov     ecx,[g_spf]
        call    sec_read
        jc      mf_fail
        mov     esi,[fat_work]
        mov     eax,edi
        call    get12
        mov     ebx,[esp+16]            ; PUSHAD's EBX: the value
        and     ebx,0FFFh
        cmp     eax,ebx
        je      mf_held
        mov     eax,edi
        call    set12
        call    fat_putall
        jc      mf_fail
        popad
        clc
        ret
mf_held:
        mov     byte ptr [mf_same],1
        popad
        clc
        ret
mf_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; verify_hard - a chunk read failed: take its ECX sectors at EDX one
;               at a time and mark each failing sector's cluster bad.
;               CF only if the FAT itself cannot be written.
;---------------------------------------------------------------------
verify_hard:
        pushad
        mov     esi,edx                 ; ESI = sector, EDI = count
        mov     edi,ecx
        mov     dword ptr [v_lastbad],-1
vh_lp:
        or      edi,edi
        jz      vh_ok
        mov     edx,esi
        mov     ebx,[chunk_buf]
        mov     ecx,1
        call    sec_read
        jnc     vh_step
        mov     eax,esi                 ; the cluster it lives in
        sub     eax,[g_datastart]
        xor     edx,edx
        div     dword ptr [g_spc]
        add     eax,2
        cmp     eax,[v_lastbad]         ; one mark per cluster
        je      vh_step
        mov     [v_lastbad],eax
        mov     ebx,0FFF7h              ; BAD, sized per FAT below
        cmp     byte ptr [fat_kind],32
        jne     vh_val
        mov     ebx,0FFFFFF7h
vh_val:
        cmp     byte ptr [fat_kind],12
        jne     vh_mark
        mov     ebx,0FF7h
vh_mark:
        call    mark_fat
        jc      vh_fail
        cmp     byte ptr [mf_same],0    ; a mark that was carried over
        jne     vh_step                 ; has been counted
        inc     dword ptr [v_bad]
vh_step:
        inc     esi
        dec     edi
        jmp     short vh_lp
vh_ok:
        popad
        clc
        ret
vh_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; root32_lba - EAX = first sector of the FAT32 root cluster
;---------------------------------------------------------------------
root32_lba:
        mov     eax,[g_rootclus]
        sub     eax,2
        imul    eax,[g_spc]
        add     eax,[g_datastart]
        ret

;---------------------------------------------------------------------
; fm_percent - v_done of v_total, printed when the number moves
; fm_wipepct - and the line it was on, blank again
;---------------------------------------------------------------------
fm_percent:
        pushad
        mov     eax,[v_done]
        mov     ecx,100
        mul     ecx
        div     dword ptr [v_total]
        cmp     al,[v_pct]
        je      fpc_out
        mov     [v_pct],al
        mov     dl,13
        mov     ah,02h
        int     21h
        movzx   eax,byte ptr [v_pct]
        call    pdec
        mov     edx,OFFSET msg_pct
        call    puts
fpc_out:
        popad
        ret

fm_wipepct:
        pushad
        mov     dl,13
        mov     ah,02h
        int     21h
        mov     edx,OFFSET msg_wipe
        call    puts
        mov     dl,13
        mov     ah,02h
        int     21h
        popad
        ret

; puts - the '$' string at EDX
puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; pdec - EAX printed in unsigned decimal
pdec:
        pushad
        mov     edi,OFFSET dec_buf + 11
        mov     byte ptr [edi],'$'
        mov     ebx,10
pd_div:
        xor     edx,edx
        div     ebx
        dec     edi
        add     dl,'0'
        mov     [edi],dl
        or      eax,eax
        jnz     pd_div
        mov     edx,edi
        call    puts
        popad
        ret

; phex4 - AX printed as four hex digits
phex4:
        pushad
        mov     ecx,4
        shl     eax,16
ph4_lp:
        rol     eax,4
        mov     dl,al
        and     dl,0Fh
        add     dl,'0'
        cmp     dl,'9'
        jbe     ph4_put
        add     dl,7
ph4_put:
        push    eax
        mov     ah,02h
        int     21h
        pop     eax
        dec     ecx
        jnz     ph4_lp
        popad
        ret

; fm_crit - our INT 24h handler: everything fails, nothing is asked
fm_crit:
        mov     al,CERR_FAIL
        iretd

; fm_upcase - AL lower -> upper
fm_upcase:
        cmp     al,'a'
        jb      fu_out
        cmp     al,'z'
        ja      fu_out
        sub     al,20h
fu_out:
        ret

; fm_crlf
fm_crlf:
        push    edx
        mov     edx,OFFSET msg_crlf
        call    puts
        pop     edx
        ret

;=====================================================================
; Data
;=====================================================================
boot_sec    dd  0                       ; BSS carve-outs
fat_work    dd  0
chunk_buf   dd  0
scratch     dd  0

tgt_log     db  0                       ; 0-based logical drive
fat_kind    db  0                       ; 12, 16 or 32
g_media     db  0
quick       db  0                       ; /Q
sysit       db  0                       ; /S
uncond      db  0                       ; /U
retest      db  0                       ; /C
bflag       db  0                       ; /B
fsize       db  NO_SIZE                 ; /F: - a row of fd_sizes

floppy      db  0                       ; the target is a diskette drive
fd_bios     db  0                       ; ...this one, 0 or 1
fd_type     db  0                       ; FDT_*
has_fs      db  0                       ; the disk in it has a format
fd_have     db  0                       ; ...this row of fd_sizes
fd_want     db  0                       ; the row being made
raw_io      db  0                       ; sectors go by drive, not letter
fresh       db  0                       ; nothing is carried over
keepmarks   db  0                       ; bad cluster marks stay
mf_same     db  0                       ; mark_fat changed nothing
            db  0,0
fd_row      dd  0                       ; -> the row being made

g_rsvd      dd  0                       ; the BPB, pulled apart
g_nfats     dd  0
g_rootent   dd  0
g_spc       dd  0
g_spf       dd  0
g_totsec    dd  0
g_rootclus  dd  0
g_fsinfo    dd  0
g_bkboot    dd  0
g_spt       dd  0
g_heads     dd  0
g_rootstart dd  0
g_rootsecs  dd  0
g_datastart dd  0
g_totclus   dd  0

fc_esize    dd  0                       ; fat_carry: bytes an entry,
fc_bad      dd  0                       ; the bad mark at that width,
fc_mask     dd  0                       ; the bits that are the entry,
fc_n        dd  0                       ; and sectors in this run

v_total     dd  0                       ; the verify pass
v_done      dd  0
v_bad       dd  0                       ; bad CLUSTERS found
v_lastbad   dd  0
v_cbytes    dd  0                       ; bytes per cluster (report)
v_free      dd  0                       ; usable clusters (report)
v_pct       db  0
            db  0,0,0
lbl_lba     dd  0

bad_tracks  dd  0,0,0,0,0               ; a bit a track, 160 of them

lbl_in      db  12, 0
            db  14 dup(0)
dec_buf     db  12 dup(0)
drv_rec     db  DRV_LEN dup(0)

; What a diskette can be.  The order is /F:'s and fd_can's.
fd_sizes:
        dw      160, 320
        db      8, 1, 40, 1
        dw      64
        db      1, 0FEh, FDM_360
        db      '160K$  '
        dw      180, 360
        db      9, 1, 40, 1
        dw      64
        db      2, 0FCh, FDM_360
        db      '180K$  '
        dw      320, 640
        db      8, 2, 40, 2
        dw      112
        db      1, 0FFh, FDM_360
        db      '320K$  '
        dw      360, 720
        db      9, 2, 40, 2
        dw      112
        db      2, 0FDh, FDM_360
        db      '360K$  '
        dw      720, 1440
        db      9, 2, 80, 2
        dw      112
        db      3, 0F9h, FDM_720
        db      '720K$  '
        dw      1200, 2400
        db      15, 2, 80, 1
        dw      224
        db      7, 0F9h, FDM_1200
        db      '1.2M$  '
        dw      1440, 2880
        db      18, 2, 80, 1
        dw      224
        db      9, 0F0h, FDM_1440
        db      '1.44M$ '
        dw      2880, 5760
        db      36, 2, 80, 2
        dw      240
        db      9, 0F0h, FDM_2880
        db      '2.88M$ '

; by FDT_*: the rows a drive can make, a bit each, and the one it
; makes when nothing says otherwise
fd_can      db  0, 0Fh, 2Fh, 10h, 50h, 0D0h
fd_default  db  0, 3, 5, 4, 6, 7

; The boot sector of a disk with no system on it.  Sixteen-bit code,
; so it is bytes: EB 3C 90 jumps over the BPB to 3Eh, which sets up a
; stack, prints the message through INT 10h, waits for a key and asks
; the BIOS to boot again.
boot_stub:
        db      0EBh, 3Ch, 90h
        db      'PMDOS0.6'
        db      25 dup(0)               ; 0Bh-23h: the BPB, filled in
        db      0, 0, 29h               ; 24h drive, 25h, 26h signature
        dd      0                       ; 27h serial number
        db      'NO NAME    '           ; 2Bh label
        db      'FAT12   '              ; 36h
        db      0FAh                    ; 3Eh cli
        db      31h, 0C0h               ;     xor ax,ax
        db      8Eh, 0D0h               ;     mov ss,ax
        db      0BCh, 00h, 7Ch          ;     mov sp,7C00h
        db      0FBh                    ;     sti
        db      8Eh, 0D8h               ;     mov ds,ax
        db      0BEh, 61h, 7Ch          ;     mov si,7C61h
        db      0FCh                    ;     cld
        db      0ACh                    ; 4Dh lodsb
        db      08h, 0C0h               ;     or al,al
        db      74h, 09h                ;     jz 5Bh
        db      0B4h, 0Eh               ;     mov ah,0Eh
        db      0BBh, 07h, 00h          ;     mov bx,7
        db      0CDh, 10h               ;     int 10h
        db      0EBh, 0F2h              ;     jmp 4Dh
        db      30h, 0E4h               ; 5Bh xor ah,ah
        db      0CDh, 16h               ;     int 16h
        db      0CDh, 19h               ;     int 19h
        db      'This is not a system disk.',13,10
        db      'Change it and press a key.',13,10,0
BOOT_STUBLEN    EQU     $ - boot_stub

msg_warn    db  13,10
            db  'WARNING: ALL DATA ON DRIVE '
warn_drv    db  'A'
            db  ': WILL BE LOST!',13,10
            db  'Proceed with Format (Y/N)? ','$'
msg_insert  db  'Insert the diskette to format in drive '
ins_drv     db  'A'
            db  ':',13,10
            db  'and press ENTER when ready...','$'
msg_formatting db 'Formatting ','$'
msg_checking db 'Checking ','$'
msg_quicking db 'Quick format ','$'
msg_noquick db  'No format to reuse: formatting every track.',13,10,'$'
msg_pct     db  ' percent completed','$'
msg_wipe    db  '                     ','$'
msg_label   db  13,10,'Volume label (11 characters, ENTER for none)? ','$'
msg_done    db  13,10,'Format complete',13,10,13,10,'$'
msg_total   db  ' bytes total disk space',13,10,'$'
msg_bad     db  ' bytes in bad sectors',13,10,'$'
msg_avail   db  ' bytes available on disk',13,10,'$'
msg_serial  db  13,10,'Volume Serial Number is ','$'
msg_usage   db  'Formats a disk for use with PM-DOS.',13,10,13,10
            db  'FORMAT drive: [/V[:label]] [/Q] [/U] [/F:size] [/B | /S] [/C]',13,10,13,10
            db  '  /V[:label]  Specifies the volume label.',13,10
            db  '  /Q          Performs a quick format.',13,10
            db  '  /U          Performs an unconditional format.',13,10
            db  '  /F:size     Specifies the size of the floppy disk to format (such',13,10
            db  '              as 160, 180, 320, 360, 720, 1.2, 1.44, 2.88).',13,10
            db  '  /B          Allocates space on the formatted disk for system files.',13,10
            db  '  /S          Copies system files to the formatted disk.',13,10
            db  '  /C          Tests clusters that are currently marked "bad."',13,10,13,10,'$'
msg_sysing  db  'Transferring the system...',13,10,'$'
msg_nosys   db  'System not transferred (SYS.COM would not run)',13,10,'$'
fm_comspec  db  'COMSPEC=',0
fm_sysname  db  'SYS.COM',0
sys_path    db  144 dup(0)              ; where SYS.COM turned out to be
sys_tail    db  16 dup(0)               ; " d:" as EXEC wants it
sys_blk     db  16 dup(0)               ; and the parameter block
msg_baddrv  db  'Invalid drive specification',13,10,'$'
msg_badsw   db  'Invalid switch',13,10,'$'
msg_badsize db  'Invalid size for /F',13,10,'$'
msg_nosize  db  'Size not supported by drive',13,10,'$'
msg_clash   db  '/B and /S cannot be used together',13,10,'$'
msg_iscur   db  'Cannot format the current drive',13,10,'$'
msg_notmnt  db  'Invalid media or Track 0 bad - disk unusable',13,10
            db  '(No readable BPB on this volume)',13,10,'$'
msg_track0  db  13,10,'Track 0 bad - disk unusable',13,10,'$'
msg_wprot   db  13,10,'Disk is write protected',13,10,'$'
msg_notrdy  db  13,10,'Drive not ready',13,10,'$'
msg_ioerr   db  13,10,'Format failure',13,10,'$'
msg_crlf    db  13,10,'$'

img_end:                                ; the BSS begins here

_FMT    ENDS
        END
