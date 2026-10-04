;=====================================================================
; DISKIMG.INC - what DISKCOPY and DISKCOMP have in common: the command
;               line, the drives, what shape a disk is, a track in and
;               out, the two kinds of question, and an image of a whole
;               diskette kept where the program cannot address it.
;
; Included by CMD\DISKCOPY\DISKCOPY.ASM and CMD\DISKCOMP\DISKCOMP.ASM,
; inside their segments: it is code and data of theirs, not a module.
; Each program supplies
;
;   p_swtab     its switches: db letter, dd OFFSET flag byte, ... db 0
;   p_usage     what to say under a bad command line
;   p_baddrv    what to say about a drive that is not a diskette drive
;   p_help      where /? goes (it does not come back)
;   p_slotonly  a byte: 1 = keep the image in the slot (DISKCOPY /M)
;
; ONE PASS.  A diskette is up to 2.88M and a native program's slot is
; 512K, so the image goes into blocks of extended memory the program
; owns and the kernel copies in and out of (21h/F1h AL=1Ah, XMF_*).
; Whatever does not fit is done the way DOS did it, in passes, and a
; machine without the memory - or a kernel without the call - still
; gets a copy, with more disk swaps.
;
; THE SECTORS GO BY 21h/F1h AL=0Dh AND 0Eh, which name the drive and
; not a letter: a disk being copied onto may have no filesystem for a
; letter to stand for, and one being compared has two.  Those calls
; reach the BIOS with the geometry the block layer was last told, so
; FDF_SETGEO is called with the source's shape before every pass.  At
; the end the letters are remounted (F1h AL=2), because the kernel
; has not been told that the disk under them changed.
;=====================================================================

DI_TRKMAX       EQU     36 * 512        ; a 2.88M track, the biggest
DI_MAXTRK       EQU     2 * 86          ; and the most tracks a disk has
DI_BSS          EQU     2 * DI_TRKMAX   ; what each program reserves

; the errorlevels both programs use
DI_RC_OK        EQU     0
DI_RC_SOFT      EQU     1               ; a read or write error; for
                                        ; DISKCOMP, disks that differ
DI_RC_BREAK     EQU     2               ; ^C
DI_RC_HARD      EQU     3               ; a disk that could not be used
DI_RC_INIT      EQU     4               ; the command line or the drives

; a row of di_sizes
DIS_SPT         EQU     0               ; db
DIS_HEADS       EQU     1               ; db
DIS_CYLS        EQU     2               ; db
DIS_MEDIUM      EQU     3               ; db - FDM_*, for FDF_BEGIN
DIS_LEN         EQU     4
DIS_COUNT       EQU     9

;=====================================================================
; di_init - buffers, ^C and critical errors, and nothing owed to a
;           diskette before its sectors are read behind the cache.
;=====================================================================
di_init:
        mov     eax,OFFSET img_end
        mov     [di_trk],eax
        add     eax,DI_TRKMAX
        mov     [di_trk2],eax

        mov     edx,OFFSET di_ctlc
        mov     ax,2523h
        int     21h
        ; A remount of an empty drive at the end must not stop to ask
        ; Abort, Retry, Fail; every disk error that matters is met by
        ; the raw calls, which never raise INT 24h, and handled here
        mov     edx,OFFSET di_crit
        mov     ax,2524h
        int     21h

        mov     ah,0Dh
        int     21h
        ret

; INT 24h: fail it, quietly
di_crit:
        mov     al,CERR_FAIL
        iretd

; INT 23h: at a question, stop now - nothing is half done.  Otherwise
; note it, and the next track in or out ends the run.
di_ctlc:
        mov     byte ptr [di_brk],1
        cmp     byte ptr [di_inq],0
        je      dcc_back
        mov     ax,0F105h               ; this handler is not going back
        int     21h
        mov     al,DI_RC_BREAK
        jmp     di_exit
dcc_back:
        iretd

;=====================================================================
; di_exit - AL = the errorlevel.  The letters are told their disks
;           changed, and the program ends.
;=====================================================================
di_exit:
        mov     [di_rcout],al
        movzx   edx,byte ptr [di_fend]  ; a format left open puts the
        cmp     edx,0FFh                ; BIOS's diskette table back
        je      dx_nofmt
        mov     eax,(FDF_END SHL 16) OR 0F114h
        int     21h
dx_nofmt:
        cmp     byte ptr [di_touched],0
        je      dx_out
        movzx   edx,byte ptr [di_drv1]
        inc     edx
        mov     ax,0F102h
        int     21h
        movzx   edx,byte ptr [di_drv2]
        inc     edx
        mov     ax,0F102h
        int     21h
dx_out:
        mov     al,[di_rcout]
        mov     ah,4Ch
        int     21h

;=====================================================================
; di_parse - the tail: no more than two "d:" and the switches in
;            p_swtab, which may stand anywhere and be glued (/1/V).
;            A bad one is named and the program ends, errorlevel 4.
;=====================================================================
di_parse:
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
        mov     edi,OFFSET di_tail
        rep     movsb
        mov     byte ptr [edi],0
        mov     esi,OFFSET di_tail
dp_lp:
        mov     ah,[esi]
        call    di_isdelim
        jne     dp_tok
        or      ah,ah
        jz      dp_done
        inc     esi
        jmp     short dp_lp
dp_tok:
        mov     [di_tok],esi
        cmp     ah,'/'
        je      dp_sw
        mov     al,ah                   ; a drive: a letter and a colon
        call    di_upper
        cmp     al,'A'
        jb      dp_format
        cmp     al,'Z'
        ja      dp_format
        cmp     byte ptr [esi+1],':'
        jne     dp_format
        mov     ah,[esi+2]
        cmp     ah,'/'
        je      dp_drv
        call    di_isdelim
        jne     dp_format               ; A:FOO - a file, not a drive
dp_drv:
        movzx   edx,byte ptr [di_ndrv]
        cmp     edx,2
        jae     dp_many
        sub     al,'A'
        mov     [di_drv1+edx],al
        inc     byte ptr [di_ndrv]
        add     esi,2
        jmp     short dp_lp
dp_sw:
        mov     al,[esi+1]
        call    di_upper
        mov     ah,[esi+2]
        cmp     ah,'/'
        je      dp_swone
        call    di_isdelim
        jne     dp_badsw                ; /VM is not two switches
dp_swone:
        cmp     al,'?'
        je      p_help
        mov     ebx,OFFSET p_swtab
dp_swfind:
        mov     dl,[ebx]
        or      dl,dl
        jz      dp_badsw
        cmp     dl,al
        je      dp_swset
        add     ebx,5
        jmp     short dp_swfind
dp_swset:
        mov     edx,[ebx+1]
        mov     byte ptr [edx],1
        add     esi,2
        jmp     dp_lp
dp_done:
        ret

dp_badsw:
        mov     edx,OFFSET di_m_badsw
        jmp     short dp_die0
dp_many:
        mov     edx,OFFSET di_m_many
        jmp     short dp_die0
dp_format:
        mov     edx,OFFSET di_m_format
dp_die0:
        xor     ecx,ecx                 ; the whole token
dp_die:
        call    di_puts
        mov     edx,OFFSET di_m_dash
        call    di_puts
        mov     edx,[di_tok]
        or      ecx,ecx
        jnz     dp_tokn
dp_toklen:                              ; to the next separator
        mov     ah,[edx+ecx]
        call    di_isdelim
        je      dp_tokn
        inc     ecx
        jmp     short dp_toklen
dp_tokn:
        call    di_write
        call    di_crlf
        mov     edx,OFFSET p_usage
        call    di_puts
        mov     al,DI_RC_INIT
        jmp     di_exit

;=====================================================================
; di_drives - the letters given, or the current one for what was not,
;             each a diskette drive the BIOS has.  p_baddrv and
;             errorlevel 4 if not.
;=====================================================================
di_drives:
        mov     ah,19h
        int     21h
        cmp     byte ptr [di_ndrv],0
        jne     dd_one
        mov     [di_drv1],al            ; nothing named: the current
dd_one:                                 ; drive, both ends
        cmp     byte ptr [di_ndrv],2
        jae     dd_have
        mov     [di_drv2],al            ; one named: onto the current
dd_have:
        mov     al,[di_drv1]
        call    di_floppy
        jc      dd_bad
        mov     [di_bios1],al
        mov     [di_type1],ah
        mov     al,[di_drv2]
        call    di_floppy
        jc      dd_bad
        mov     [di_bios2],al
        mov     [di_type2],ah
        mov     byte ptr [di_one],0
        mov     al,[di_bios1]
        cmp     al,[di_bios2]
        jne     dd_two
        mov     byte ptr [di_one],1     ; one drive: swap disks in it
dd_two:
        mov     al,[di_drv1]            ; the letters' answers come from
        add     al,'A'                  ; here: drive1 and drive2 as
        mov     [di_l1],al              ; typed
        mov     al,[di_drv2]
        add     al,'A'
        mov     [di_l2],al
        mov     byte ptr [di_touched],1
        ret
dd_bad:
        mov     edx,OFFSET p_baddrv
        call    di_puts
        mov     al,DI_RC_INIT
        jmp     di_exit

;---------------------------------------------------------------------
; di_floppy - AL = a letter (0 = A:).  CF clear: AL = its BIOS unit,
;             AH = the drive's FDT_*.
;---------------------------------------------------------------------
di_floppy:
        movzx   edx,al
        mov     edi,OFFSET di_drec
        mov     eax,0F106h
        int     21h
        jc      dfl_bad
        mov     al,[di_drec+DRV_FLAGS]
        test    al,DRVF_PRESENT
        jz      dfl_bad
        test    al,DRVF_FLOPPY
        jz      dfl_bad
        test    al,DRVF_DRIVER OR DRVF_IMAGE OR DRVF_CDROM
        jnz     dfl_bad
        movzx   edx,byte ptr [di_drec+DRV_BIOS]
        cmp     edx,2
        jae     dfl_bad
        push    edx
        mov     eax,(FDF_TYPE SHL 16) OR 0F114h
        int     21h
        pop     edx
        jc      dfl_bad                 ; no such drive in this machine
        mov     al,dl
        mov     ah,bl
        clc
        ret
dfl_bad:
        stc
        ret

;=====================================================================
; Questions.  Every one reads from the keyboard, or a byte at a time
; from a file when standard input is one - where the end of the file
; is taken as ^C, so that a script that runs out stops the program
; rather than leaving it asking for ever.
;=====================================================================

; di_anykey - "Press any key to continue . . ." and the key
di_anykey:
        mov     edx,OFFSET di_m_press
        call    di_puts
        ; fall into

; di_getkey - AL = a key, not echoed.  ^C ends the program.
di_getkey:
        mov     byte ptr [di_inq],1
        cmp     byte ptr [di_brk],0
        jne     gk_brk                  ; one came in before the question
        mov     ax,4400h
        xor     ebx,ebx
        int     21h
        jc      gk_con
        test    dl,80h
        jnz     gk_con
        mov     ah,3Fh                  ; a file: one byte of it
        xor     ebx,ebx
        mov     ecx,1
        mov     edx,OFFSET di_kbyte
        int     21h
        jc      gk_brk
        or      eax,eax
        jz      gk_brk
        mov     al,[di_kbyte]
        cmp     al,3
        je      gk_brk
        jmp     short gk_out
gk_con:
        mov     ax,0C08h                ; what was typed ahead is not
        int     21h                     ; an answer to this
        or      al,al
        jnz     gk_out
        mov     ah,08h                  ; the rest of an extended key
        int     21h
        xor     al,al
gk_out:
        mov     byte ptr [di_inq],0
        ret
gk_brk:
        mov     byte ptr [di_inq],0
        call    di_crlf
        mov     al,DI_RC_BREAK
        jmp     di_exit

; di_yesno - EDX -> the question.  AL = 'Y' or 'N', and the key echoed.
di_yesno:
        mov     [di_q],edx
dyn_ask:
        mov     edx,[di_q]
        call    di_puts
        call    di_getkey
        mov     [di_kbyte],al
        cmp     al,' '
        jb      dyn_noecho
        mov     edx,OFFSET di_kbyte
        mov     ecx,1
        call    di_write
dyn_noecho:
        call    di_crlf
        mov     al,[di_kbyte]
        call    di_upper
        cmp     al,'Y'
        je      dyn_out
        cmp     al,'N'
        je      dyn_out
        call    di_crlf                 ; neither: ask again
        jmp     short dyn_ask
dyn_out:
        ret

; di_chkbrk - a ^C that came in while the disks were busy ends it here
di_chkbrk:
        cmp     byte ptr [di_brk],0
        jne     gk_brk
        ret

;=====================================================================
; di_io - AL = BIOS unit, AH = 0 read / 1 write, EBX = the first LBA,
;         ECX = sectors, EDI = the buffer.  A drive that is not ready,
;         and a write to a protected disk, are asked about and tried
;         again; CF and AX = the DOS error for anything else.
;=====================================================================
di_io:
        mov     [dio_unit],al
        mov     [dio_wr],ah
        mov     [dio_lba],ebx
        mov     [dio_cnt],ecx
        mov     [dio_buf],edi
dio_try:
        call    di_chkbrk
        movzx   edx,byte ptr [dio_unit]
        mov     ebx,[dio_lba]
        mov     ecx,[dio_cnt]
        mov     edi,[dio_buf]
        mov     eax,0F10Dh
        cmp     byte ptr [dio_wr],0
        je      dio_go
        mov     eax,0F10Eh
dio_go:
        int     21h
        jnc     dio_ok
        movzx   eax,ax
        cmp     eax,15h                 ; not ready
        je      dio_notrdy
        cmp     eax,13h                 ; write protected
        jne     dio_fail
        cmp     byte ptr [dio_wr],0
        je      dio_fail
        mov     edx,OFFSET di_m_wprot
        jmp     short dio_ask
dio_notrdy:
        mov     edx,OFFSET di_m_notrdy
dio_ask:
        call    di_puts
        mov     edx,OFFSET di_m_correct
        call    di_puts
        call    di_getkey
        jmp     short dio_try
dio_fail:
        stc
        ret
dio_ok:
        clc
        ret

;---------------------------------------------------------------------
; di_trkpos - EAX = a track as the programs count them (each cylinder's
;             sides in turn, /1 counting side 0 only).  -> EBX = its
;             first LBA, ECX = cylinder, EDX = side.
;---------------------------------------------------------------------
di_trkpos:
        xor     edx,edx
        movzx   ecx,byte ptr [di_sides]
        div     ecx                     ; EAX = cylinder, EDX = side
        mov     ecx,eax
        imul    eax,[g_heads]
        add     eax,edx
        imul    eax,[g_spt]
        mov     ebx,eax
        ret

;---------------------------------------------------------------------
; di_track - AL = unit, AH = 0 read / 1 write, EDX = the track,
;            EDI = the buffer.  [di_nsec] sectors.  CF, AX as di_io.
;---------------------------------------------------------------------
di_track:
        push    eax
        mov     eax,edx
        call    di_trkpos
        pop     eax
        movzx   ecx,byte ptr [di_nsec]
        jmp     di_io

;=====================================================================
; di_shape - AL = a BIOS unit.  The disk in it, as its boot sector
;            says: g_spt, g_heads, g_cyls, g_total and g_row (FFh for
;            a shape that is not one of di_sizes).  CF if it cannot be
;            read (AX = the DOS error) or says nothing usable (AX = 0).
;
; A disk older than DOS 2 has no BPB; its FAT's first byte says which
; of the four 5.25-inch formats it is, and DOS believed that, so this
; does too.  The boot sector is left in [di_trk2].
;=====================================================================
di_shape:
        mov     [dsh_unit],al
        movzx   edx,al
        mov     eax,(FDF_FORGET SHL 16) OR 0F114h
        int     21h                     ; the BIOS's view, not the last
                                        ; disk's
        mov     al,[dsh_unit]
        xor     ah,ah
        xor     ebx,ebx
        mov     ecx,1
        mov     edi,[di_trk2]
        call    di_io
        jc      dsh_out
        mov     esi,[di_trk2]
        call    di_bpbof
        jnc     dsh_have

dsh_old:
        mov     al,[dsh_unit]
        xor     ah,ah
        mov     ebx,1                   ; the first FAT sector
        mov     ecx,1
        mov     edi,[di_trk]
        call    di_io
        jc      dsh_out
        mov     esi,[di_trk]
        cmp     word ptr [esi+1],0FFFFh
        jne     dsh_none
        mov     al,[esi]
        mov     esi,OFFSET di_media
dsh_mlp:
        cmp     byte ptr [esi],0
        je      dsh_none
        cmp     al,[esi]
        je      dsh_mhit
        add     esi,4
        jmp     short dsh_mlp
dsh_mhit:
        movzx   eax,byte ptr [esi+1]    ; sectors a track
        movzx   ebx,byte ptr [esi+2]    ; heads
        mov     ecx,40                  ; forty cylinders, always
        imul    ecx,eax
        imul    ecx,ebx
        mov     edx,40

dsh_have:
        mov     [g_spt],eax
        mov     [g_heads],ebx
        mov     [g_total],ecx
        mov     [g_cyls],edx
        call    di_rowof
        mov     [g_row],al
        clc
        ret
dsh_none:
        xor     eax,eax
        stc
dsh_out:
        ret

;---------------------------------------------------------------------
; di_bpbof - ESI -> a boot sector.  CF unless its BPB describes a
;            diskette: EAX = sectors a track, EBX = heads, ECX =
;            sectors in all, EDX = cylinders.
;---------------------------------------------------------------------
di_bpbof:
        mov     al,[esi]
        cmp     al,0E9h
        je      dbp_jmp
        cmp     al,0EBh
        jne     dbp_no
dbp_jmp:
        cmp     word ptr [esi+0Bh],512
        jne     dbp_no
        movzx   eax,word ptr [esi+18h]  ; sectors a track
        movzx   ebx,word ptr [esi+1Ah]  ; heads
        movzx   ecx,word ptr [esi+13h]  ; sectors in all
        or      ecx,ecx
        jnz     dbp_check
        mov     ecx,[esi+20h]
dbp_check:
        or      eax,eax
        jz      dbp_no
        cmp     eax,36
        ja      dbp_no
        or      ebx,ebx
        jz      dbp_no
        cmp     ebx,2
        ja      dbp_no
        push    eax
        push    ebx
        imul    ebx,eax                 ; sectors a cylinder
        mov     eax,ecx
        xor     edx,edx
        div     ebx
        mov     ebx,edx                 ; what is over
        mov     edx,eax                 ; EDX = cylinders
        or      ebx,ebx
        pop     ebx
        pop     eax
        jnz     dbp_no                  ; not whole cylinders
        or      edx,edx
        jz      dbp_no
        cmp     edx,86
        ja      dbp_no
        clc
        ret
dbp_no:
        stc
        ret

;---------------------------------------------------------------------
; di_rowof - EAX = sectors a track, EBX = heads, EDX = cylinders.
;            AL = its di_sizes row, FFh if it is none of them.
;---------------------------------------------------------------------
di_rowof:
        push    ecx
        push    esi
        push    edi
        mov     edi,eax
        mov     esi,OFFSET di_sizes
        xor     ecx,ecx
dro_row:
        movzx   eax,byte ptr [esi+DIS_SPT]
        cmp     eax,edi
        jne     dro_next
        movzx   eax,byte ptr [esi+DIS_HEADS]
        cmp     eax,ebx
        jne     dro_next
        movzx   eax,byte ptr [esi+DIS_CYLS]
        cmp     eax,edx
        jne     dro_next
        mov     eax,ecx
        jmp     short dro_out
dro_next:
        add     esi,DIS_LEN
        inc     ecx
        cmp     ecx,DIS_COUNT
        jb      dro_row
        mov     eax,0FFh
dro_out:
        pop     edi
        pop     esi
        pop     ecx
        ret

;---------------------------------------------------------------------
; di_fits - AH = a drive's FDT_*.  CF unless that drive can take a
;           disk of the shape in g_*.  A shape that is not a standard
;           one is left to the BIOS: if the drive cannot read it, the
;           reads will say so.
;---------------------------------------------------------------------
di_fits:
        movzx   ecx,byte ptr [g_row]
        cmp     ecx,0FFh
        je      dfi_ok
        movzx   eax,ah
        cmp     eax,FDT_2880
        ja      dfi_no
        movzx   eax,word ptr [di_can+eax*2]
        bt      eax,ecx
        jnc     dfi_no
dfi_ok:
        clc
        ret
dfi_no:
        stc
        ret

; di_settle - AL = unit: the block layer is told the shape in g_*
di_settle:
        pushad
        movzx   edx,al
        mov     eax,[g_heads]
        mov     dh,al                   ; the drive in DL, the heads in
        mov     ecx,[g_spt]             ; the byte above it
        mov     ebx,[g_total]
        mov     eax,(FDF_SETGEO SHL 16) OR 0F114h
        int     21h
        popad
        ret

; di_layout - the "how much of each track" figures once the switches
; and the shape are known: di_sides, di_nsec (set by the caller first
; to 0 for "all of them"), g_tracks, di_tbytes
di_layout:
        mov     eax,[g_heads]
        cmp     byte ptr [di_sides],0
        je      dl_sides
        cmp     byte ptr [di_sides],al
        jbe     dl_shave
dl_sides:
        mov     [di_sides],al
dl_shave:
        mov     eax,[g_spt]
        cmp     byte ptr [di_nsec],0
        je      dl_nsec
        cmp     byte ptr [di_nsec],al
        jbe     dl_have
dl_nsec:
        mov     [di_nsec],al
dl_have:
        movzx   eax,byte ptr [di_sides]
        imul    eax,[g_cyls]
        mov     [g_tracks],eax
        movzx   eax,byte ptr [di_nsec]
        shl     eax,9
        mov     [di_tbytes],eax
        ret

;=====================================================================
; The image: as many tracks as there is memory for, up to the whole
; disk.  Extended memory first; the slot's own heap if there is none
; of that (or no kernel call for it, or DISKCOPY /M); [di_cap] says
; how many tracks it holds.
;=====================================================================
di_imgopen:
        mov     dword ptr [di_xhdl],0
        mov     dword ptr [di_sblk],0
        cmp     byte ptr [p_slotonly],0
        jne     dim_slot
        mov     ecx,[g_tracks]
        imul    ecx,[di_tbytes]
        mov     eax,(XMF_ALLOC SHL 16) OR 0F11Ah
        int     21h
        jnc     dim_xall
        cmp     ax,ERR_NOMEM
        jne     dim_slot                ; no such call: an older kernel
        mov     eax,ecx                 ; the most there is, in tracks
        xor     edx,edx
        div     dword ptr [di_tbytes]
        cmp     eax,DI_MAXTRK / 4       ; less than a quarter of a disk
        jb      dim_slot                ; is not worth the trouble
        mov     [di_cap],eax
        mov     ecx,eax
        imul    ecx,[di_tbytes]
        mov     eax,(XMF_ALLOC SHL 16) OR 0F11Ah
        int     21h
        jc      dim_slot
        mov     [di_xhdl],edx
        clc
        ret
dim_xall:
        mov     [di_xhdl],edx
        mov     eax,[g_tracks]
        mov     [di_cap],eax
        clc
        ret
dim_slot:
        mov     ebx,[g_tracks]
        imul    ebx,[di_tbytes]
        mov     ah,48h
        int     21h
        jnc     dim_sall
        mov     eax,ebx                 ; the most the heap has
        xor     edx,edx
        div     dword ptr [di_tbytes]
        or      eax,eax
        jz      dim_none
        mov     [di_cap],eax
        mov     ebx,eax
        imul    ebx,[di_tbytes]
        mov     ah,48h
        int     21h
        jc      dim_none
        mov     [di_sblk],eax
        clc
        ret
dim_sall:
        mov     [di_sblk],eax
        mov     eax,[g_tracks]
        mov     [di_cap],eax
        clc
        ret
dim_none:
        stc
        ret

di_imgclose:
        pushad
        mov     edx,[di_xhdl]
        or      edx,edx
        jz      dic_slot
        mov     eax,(XMF_FREE SHL 16) OR 0F11Ah
        int     21h
        mov     dword ptr [di_xhdl],0
dic_slot:
        mov     edx,[di_sblk]
        or      edx,edx
        jz      dic_out
        mov     ah,49h
        int     21h
        mov     dword ptr [di_sblk],0
dic_out:
        popad
        ret

; di_imgput - EAX = which of the image's tracks, ESI -> the track
di_imgput:
        pushad
        imul    eax,[di_tbytes]
        mov     ecx,[di_tbytes]
        cmp     dword ptr [di_xhdl],0
        je      dip_slot
        mov     ebx,eax
        mov     edx,[di_xhdl]
        mov     eax,(XMF_PUT SHL 16) OR 0F11Ah
        int     21h
        popad
        ret
dip_slot:
        mov     edi,[di_sblk]
        add     edi,eax
        shr     ecx,2
        rep     movsd
        popad
        ret

; di_imgget - EAX = which of the image's tracks, EDI -> where it goes
di_imgget:
        pushad
        imul    eax,[di_tbytes]
        mov     ecx,[di_tbytes]
        cmp     dword ptr [di_xhdl],0
        je      dig_slot
        mov     ebx,eax
        mov     edx,[di_xhdl]
        mov     eax,(XMF_GET SHL 16) OR 0F11Ah
        int     21h
        popad
        ret
dig_slot:
        mov     esi,[di_sblk]
        add     esi,eax
        shr     ecx,2
        rep     movsd
        popad
        ret

;=====================================================================
; Output - everything to standard output, as 6.22's two did
;=====================================================================
; di_puts - the zero-terminated string at EDX
di_puts:
        push    eax
        push    ecx
        push    edi
        mov     edi,edx
        xor     al,al
        mov     ecx,-1
        repne   scasb
        not     ecx
        dec     ecx
        call    di_write
        pop     edi
        pop     ecx
        pop     eax
        ret

; di_write - ECX bytes at EDX
di_write:
        pushad
        jecxz   dw_out
        mov     ebx,1
        mov     ah,40h
        int     21h
dw_out:
        popad
        ret

di_crlf:
        push    edx
        mov     edx,OFFSET di_m_crlf
        call    di_puts
        pop     edx
        ret

; di_pchar - the character in AL
di_pchar:
        pushad
        mov     [di_onech],al
        mov     edx,OFFSET di_onech
        mov     ecx,1
        call    di_write
        popad
        ret

; di_pdec - EAX in decimal
di_pdec:
        pushad
        mov     edi,OFFSET di_decbuf + 11
        mov     byte ptr [edi],0
        mov     ebx,10
dpd_div:
        xor     edx,edx
        div     ebx
        dec     edi
        add     dl,'0'
        mov     [edi],dl
        or      eax,eax
        jnz     dpd_div
        mov     edx,edi
        call    di_puts
        popad
        ret

; di_phex4 - AX as four hex digits
di_phex4:
        pushad
        mov     ecx,4
        shl     eax,16
dph_lp:
        rol     eax,4
        push    eax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     dph_put
        add     al,7
dph_put:
        call    di_pchar
        pop     eax
        dec     ecx
        jnz     dph_lp
        popad
        ret

; di_upper - AL lower -> upper, a-z
di_upper:
        cmp     al,'a'
        jb      dup_out
        cmp     al,'z'
        ja      dup_out
        sub     al,20h
dup_out:
        ret

; di_isdelim - ZF if AH separates parameters, or ends the line
di_isdelim:
        cmp     ah,' '
        je      did_out
        cmp     ah,9
        je      did_out
        cmp     ah,','
        je      did_out
        cmp     ah,';'
        je      did_out
        cmp     ah,'='
        je      did_out
        cmp     ah,0Dh
        je      did_out
        or      ah,ah
did_out:
        ret

;=====================================================================
; Data
;=====================================================================
; The standard formats, FORMAT's fd_sizes in the same order, and which
; of them each kind of drive can take (a bit per row, by FDT_*).
;
; DMF IS THE NINTH ROW AND FORMAT HAS NO NINTH: it is a disk to copy,
; not one to make.  Its medium is FDM_1440's, so DISKCOPY formats a
; 1.44M target as DMF and refuses a 720K one as it would for 1.44M,
; and FDF_BEGIN knows it by the 21 sectors.
di_sizes    db  8, 1, 40, FDM_360       ; 160K
            db  9, 1, 40, FDM_360       ; 180K
            db  8, 2, 40, FDM_360       ; 320K
            db  9, 2, 40, FDM_360       ; 360K
            db  9, 2, 80, FDM_720       ; 720K
            db  15, 2, 80, FDM_1200     ; 1.2M
            db  18, 2, 80, FDM_1440     ; 1.44M
            db  36, 2, 80, FDM_2880     ; 2.88M
            db  21, 2, 80, FDM_1440     ; DMF, 1.68M
di_can      dw  0, 0Fh, 2Fh, 10h, 150h, 1D0h

; a disk with no BPB: its FAT's media byte -> sectors, heads, and one
; spare byte
di_media    db  0FEh, 8, 1, 0
            db  0FCh, 9, 1, 0
            db  0FFh, 8, 2, 0
            db  0FDh, 9, 2, 0
            db  0

di_trk      dd  0                       ; the BSS: two tracks
di_trk2     dd  0
di_xhdl     dd  0                       ; the image: a block of extended
di_sblk     dd  0                       ; memory, or of the slot
di_cap      dd  0                       ; tracks it holds
di_tbytes   dd  0                       ; bytes in one track as handled
di_tok      dd  0
di_q        dd  0
g_spt       dd  0                       ; the disk's shape
g_heads     dd  0
g_cyls      dd  0
g_total     dd  0
g_tracks    dd  0                       ; the tracks handled
dio_lba     dd  0
dio_cnt     dd  0
dio_buf     dd  0
g_row       db  0                       ; its di_sizes row, FFh if none
di_drv1     db  0                       ; letters, 0 = A:
di_drv2     db  0
di_bios1    db  0                       ; and the drives behind them
di_bios2    db  0
di_type1    db  0                       ; FDT_*
di_type2    db  0
di_l1       db  'A'                     ; the letters, for messages
di_l2       db  'A'
di_ndrv     db  0
di_one      db  0                       ; one drive, two disks in turn
di_sides    db  0                       ; sides handled
di_nsec     db  0                       ; sectors of each track handled
di_touched  db  0                       ; the drives are known
di_inq      db  0                       ; waiting on a question
di_brk      db  0                       ; ^C seen
di_rcout    db  0
di_kbyte    db  0
di_onech    db  0
dio_unit    db  0
dio_wr      db  0
dsh_unit    db  0
di_fend     db  0FFh                    ; a unit with a format open
di_drec     db  DRV_LEN dup(0)
di_decbuf   db  12 dup(0)
di_tail     db  130 dup(0)

di_m_crlf   db  13,10,0
di_m_dash   db  ' - ',0
di_m_badsw  db  'Invalid switch',0
di_m_many   db  'Too many parameters',0
di_m_format db  'Parameter format not correct',0
di_m_press  db  13,10,'Press any key to continue . . .',13,10,0
di_m_notrdy db  13,10,'Not ready',13,10
            db  'Make sure a diskette is inserted into',13,10
            db  'the drive and the door is closed',13,10,0
di_m_wprot  db  13,10,'Write protect error',13,10,0
di_m_correct db 13,10,'Press CTRL+C to abort,',13,10
            db  'or correct this problem and press any other key to continue . . .',13,10,0
