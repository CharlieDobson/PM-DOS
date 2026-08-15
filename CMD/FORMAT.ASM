;=====================================================================
; FORMAT.ASM - FORMAT.COM, initialise a FAT volume afresh
;
;   FORMAT d:       full format: fresh structures, then verify every
;                   data sector, marking bad clusters in the FAT
;   FORMAT d: /Q    quick format: fresh structures only
;
; PM-DOS cannot low-level format - there are no tracks to lay down on
; an IDE disk anyway - so FORMAT is the structural rebuild it has
; really been since the AT: the volume's existing BPB supplies the
; geometry and the layout is kept, the FATs are written afresh (entry
; 0 carries the media byte, entry 1 the end marker, and on FAT32 the
; root cluster its EOC), the root directory is emptied, and on FAT32
; the FSInfo sector and its backup are rebuilt.  The boot sector is
; NOT touched: a SYS'd disk comes out of FORMAT still bootable, and a
; data disk keeps whatever stub it had.
;
; A volume with no readable BPB cannot be mounted, so the kernel's
; INT 25h/26h - which speak volume-relative sectors - cannot reach
; it; raw media is MKDISK's business, on the host.  The kernel is
; told afterwards through 21h/F1h AL=2 (remount): every cached notion
; of the old filesystem is discarded unflushed and the BPB is read
; back fresh, with the drive's directory back at the root.
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

CHUNK_SECS      EQU     32              ; 16K of sectors at a time

_FMT    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_FMT, DS:_FMT, ES:_FMT, SS:_FMT
        ORG     0

;=====================================================================
; D32 header - the sector and chunk buffers live in the BSS
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET fm_entry>,<512 + 16384 + 16384>,4096

;=====================================================================
fm_entry:
        mov     eax,OFFSET img_end      ; carve up the BSS
        mov     [boot_sec],eax          ; 512: the boot sector
        add     eax,512
        mov     [fat_work],eax          ; 16K: FAT read-modify-write
        add     eax,16384
        mov     [chunk_buf],eax         ; 16K: zeroes out, verify in

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

;--- the command tail: FORMAT d: [/Q] --------------------------------
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
        cmp     ecx,2
        jb      fm_usage
        mov     al,[esi]
        cmp     byte ptr [esi+1],':'
        jne     fm_usage
        cmp     al,'a'
        jb      fm_up
        cmp     al,'z'
        ja      fm_up
        sub     al,20h
fm_up:
        sub     al,'A'
        cmp     al,4                    ; A: B: C: D:
        jae     fm_baddrv
        mov     [tgt_log],al
        add     al,'A'
        mov     [warn_drv],al           ; the warning names the victim
        add     esi,2
        sub     ecx,2

;--- switches: /Q is the only one ------------------------------------
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
        cmp     al,'q'
        jne     fm_swq
        mov     al,'Q'
fm_swq:
        cmp     al,'Q'
        jne     fm_badsw
        mov     byte ptr [quick],1
        add     esi,2
        sub     ecx,2
        jmp     short fm_sw
fm_swstep:
        inc     esi
        dec     ecx
        jmp     short fm_sw
fm_argsdone:

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
        mov     ah,09h
        int     21h
        mov     ah,01h
        int     21h
        call    fm_upcase
        push    eax
        call    fm_crlf
        pop     eax
        cmp     al,'Y'
        jne     fm_quit

;--- geometry, from the volume's own BPB -----------------------------
        mov     al,[tgt_log]
        mov     ebx,[boot_sec]
        mov     ecx,1
        xor     edx,edx
        int     25h
        jc      fm_ioerr
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
        jnz     fm_havespf
        mov     eax,dword ptr [esi+24h] ; ...FAT32 keeps it here
fm_havespf:
        mov     [g_spf],eax
        movzx   eax,word ptr [esi+13h]  ; total sectors...
        or      eax,eax
        jnz     fm_havetot
        mov     eax,dword ptr [esi+20h] ; ...or the 32-bit count
fm_havetot:
        mov     [g_totsec],eax
        mov     eax,dword ptr [esi+2Ch] ; FAT32 root cluster
        mov     [g_rootclus],eax
        movzx   eax,word ptr [esi+30h]  ; FAT32 FSInfo sector
        mov     [g_fsinfo],eax
        movzx   eax,word ptr [esi+32h]  ; FAT32 backup boot sector
        mov     [g_bkboot],eax

        ; derived layout, volume-relative like INT 25h/26h
        mov     eax,[g_nfats]
        imul    eax,[g_spf]
        add     eax,[g_rsvd]
        mov     [g_rootstart],eax       ; root area (FAT12/16)
        mov     ebx,[g_rootent]
        shr     ebx,4                   ; * 32 / 512
        mov     [g_rootsecs],ebx
        add     eax,ebx
        mov     [g_datastart],eax
        mov     ebx,[g_totsec]
        sub     ebx,eax
        mov     eax,ebx
        xor     edx,edx
        div     dword ptr [g_spc]
        mov     [g_totclus],eax

;=====================================================================
; Fresh FATs.  The first sector of each copy carries the reserved
; entries; everything after it is zeroes.
;=====================================================================
        mov     edi,[fat_work]          ; build the first sector
        push    edi
        mov     ecx,128
        xor     eax,eax
        rep     stosd
        pop     edi
        movzx   eax,byte ptr [g_media]
        cmp     byte ptr [fat_kind],32
        je      fmf_32
        cmp     byte ptr [fat_kind],16
        je      fmf_16
        or      eax,0F00h               ; FAT12: media | FF, FF
        mov     [edi],ax
        mov     byte ptr [edi+2],0FFh
        jmp     short fmf_built
fmf_16:
        or      eax,0FF00h              ; FAT16: media FF / FF FF
        mov     [edi],ax
        mov     word ptr [edi+2],0FFFFh
        jmp     short fmf_built
fmf_32:
        or      eax,0FFFFF00h           ; FAT32: two reserved dwords
        mov     [edi],eax
        mov     dword ptr [edi+4],0FFFFFFFFh
fmf_built:

        xor     ebp,ebp                 ; EBP = FAT copy number
fmf_copy:
        mov     eax,ebp
        imul    eax,[g_spf]
        add     eax,[g_rsvd]            ; this copy's first sector
        push    eax
        mov     edx,eax
        mov     al,[tgt_log]
        mov     ebx,[fat_work]
        mov     ecx,1
        int     26h
        pop     eax
        jc      fm_ioerr
        inc     eax                     ; and SPF-1 sectors of zeroes
        mov     ecx,[g_spf]
        dec     ecx
        call    zero_range
        jc      fm_ioerr
        inc     ebp
        cmp     ebp,[g_nfats]
        jb      fmf_copy

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
        mov     [edi+1E8h],eax                  ; free clusters
        mov     eax,[g_rootclus]
        inc     eax
        mov     [edi+1ECh],eax                  ; next-free hint
        mov     dword ptr [edi+1FCh],0AA550000h
        mov     edx,[g_fsinfo]
        mov     al,[tgt_log]
        mov     ebx,[fat_work]
        mov     ecx,1
        int     26h
        jc      fm_ioerr
        mov     edx,[g_bkboot]
        or      edx,edx
        jz      fmr_done
        add     edx,[g_fsinfo]                  ; the mirrored copy
        mov     al,[tgt_log]
        mov     ebx,[fat_work]
        mov     ecx,1
        int     26h
        jc      fm_ioerr
fmr_done:

;=====================================================================
; The verify pass - a full format reads every data sector and marks
; the bad ones' clusters in the fresh FAT.  /Q skips it.
;=====================================================================
        cmp     byte ptr [quick],0
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
        push    ecx
        push    edx
        mov     al,[tgt_log]
        mov     ebx,[chunk_buf]
        int     25h
        pop     edx
        pop     ecx
        jnc     fmv_good
        call    verify_hard             ; sector by sector, marking
        jc      fm_ioerr
fmv_good:
        add     edx,ecx
        add     [v_done],ecx

        ; NN percent completed, only when the number moves
        mov     eax,[v_done]
        imul    eax,100
        push    edx
        xor     edx,edx
        div     dword ptr [v_total]
        pop     edx
        cmp     al,[v_pct]
        je      fmv_lp
        mov     [v_pct],al
        push    edx
        mov     dl,13
        mov     ah,02h
        int     21h
        movzx   eax,byte ptr [v_pct]
        call    pdec
        mov     edx,OFFSET msg_pct
        mov     ah,09h
        int     21h
        pop     edx
        jmp     fmv_lp
fmv_end:
        mov     dl,13                   ; wipe the percent line
        mov     ah,02h
        int     21h
        mov     edx,OFFSET msg_wipe
        mov     ah,09h
        int     21h
        mov     dl,13
        mov     ah,02h
        int     21h

;=====================================================================
; Volume label
;=====================================================================
fm_label:
        mov     edx,OFFSET msg_label
        mov     ah,09h
        int     21h
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
        mov     al,[tgt_log]
        mov     ebx,[fat_work]
        mov     ecx,1
        int     25h
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
        mov     al,[tgt_log]
        mov     ebx,[fat_work]
        mov     ecx,1
        int     26h
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
        mov     ah,09h
        int     21h

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
        mov     ah,09h
        int     21h

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
        mov     ah,09h
        int     21h
fmx_free:
        mov     eax,[v_free]
        imul    eax,[v_cbytes]
        call    pdec
        mov     edx,OFFSET msg_avail
        mov     ah,09h
        int     21h
fm_exit:
        mov     ax,4C00h
        int     21h
fm_quit:
        mov     ax,4C01h
        int     21h

;--- errors ----------------------------------------------------------
fm_usage:
        mov     edx,OFFSET msg_usage
        jmp     short fm_die
fm_baddrv:
        mov     edx,OFFSET msg_baddrv
        jmp     short fm_die
fm_badsw:
        mov     edx,OFFSET msg_badsw
        jmp     short fm_die
fm_iscur:
        mov     edx,OFFSET msg_iscur
        jmp     short fm_die
fm_notmnt:
        mov     edx,OFFSET msg_notmnt
        jmp     short fm_die
fm_ioerr:
        mov     edx,OFFSET msg_ioerr
fm_die:
        mov     ah,09h
        int     21h
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; zero_range - ECX sectors of zeroes starting at volume sector EAX.
;              CF on I/O error.
;---------------------------------------------------------------------
zero_range:
        pushad
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
        mov     al,[tgt_log]
        mov     ebx,[chunk_buf]
        mov     ecx,ebp
        int     26h
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
;            CF on I/O error.  FAT12 rewrites the whole (small) FAT;
;            FAT16/32 read-modify-write one sector.
;---------------------------------------------------------------------
mark_fat:
        pushad
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
        mov     al,[tgt_log]            ; copy 0 supplies the sector
        mov     ebx,[fat_work]
        mov     ecx,1
        int     25h
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
        mov     [esi],ax
        jmp     short mf_wr
mf_32:
        mov     [esi],edi
mf_wr:
        xor     esi,esi                 ; ESI = copy number now
mf_wlp:
        mov     eax,esi
        imul    eax,[g_spf]
        add     eax,[g_rsvd]
        add     eax,ebp
        mov     edx,eax
        mov     al,[tgt_log]
        mov     ebx,[fat_work]
        mov     ecx,1
        int     26h
        jc      mf_fail
        inc     esi
        cmp     esi,[g_nfats]
        jb      mf_wlp
        popad
        clc
        ret

mf_12:
        ; the whole FAT12 FAT fits the work buffer with room to spare
        mov     esi,eax
        mov     edx,[g_rsvd]
        mov     al,[tgt_log]
        mov     ebx,[fat_work]
        mov     ecx,[g_spf]
        int     25h
        jc      mf_fail
        mov     eax,esi                 ; entry offset = clus * 3 / 2
        shr     eax,1
        add     eax,esi
        add     eax,[fat_work]
        test    esi,1
        jnz     mf_odd
        mov     cx,[eax]                ; even: low 12 bits
        and     cx,0F000h
        or      cx,bx
        mov     [eax],cx
        jmp     short mf_12wr
mf_odd:
        mov     cx,[eax]                ; odd: high 12 bits
        and     cx,000Fh
        shl     bx,4
        or      cx,bx
        mov     [eax],cx
mf_12wr:
        xor     esi,esi
mf_12lp:
        mov     eax,esi
        imul    eax,[g_spf]
        add     eax,[g_rsvd]
        mov     edx,eax
        mov     al,[tgt_log]
        mov     ebx,[fat_work]
        mov     ecx,[g_spf]
        int     26h
        jc      mf_fail
        inc     esi
        cmp     esi,[g_nfats]
        jb      mf_12lp
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
        mov     al,[tgt_log]
        mov     ebx,[chunk_buf]
        mov     ecx,1
        int     25h
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
        mov     ah,09h
        int     21h
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
        push    eax
        mov     edx,OFFSET msg_crlf
        mov     ah,09h
        int     21h
        pop     eax
        pop     edx
        ret

;=====================================================================
; Data
;=====================================================================
boot_sec    dd  0                       ; BSS carve-outs
fat_work    dd  0
chunk_buf   dd  0

tgt_log     db  0                       ; 0-based logical drive
fat_kind    db  0                       ; 12, 16 or 32
g_media     db  0
quick       db  0                       ; /Q

g_rsvd      dd  0                       ; the BPB, pulled apart
g_nfats     dd  0
g_rootent   dd  0
g_spc       dd  0
g_spf       dd  0
g_totsec    dd  0
g_rootclus  dd  0
g_fsinfo    dd  0
g_bkboot    dd  0
g_rootstart dd  0
g_rootsecs  dd  0
g_datastart dd  0
g_totclus   dd  0

v_total     dd  0                       ; the verify pass
v_done      dd  0
v_bad       dd  0                       ; bad CLUSTERS found
v_lastbad   dd  0
v_cbytes    dd  0                       ; bytes per cluster (report)
v_free      dd  0                       ; usable clusters (report)
v_pct       db  0
lbl_lba     dd  0

lbl_in      db  12, 0
            db  14 dup(0)
dec_buf     db  12 dup(0)

msg_warn    db  13,10
            db  'WARNING: ALL DATA ON DRIVE '
warn_drv    db  'A'
            db  ': WILL BE LOST!',13,10
            db  'Proceed with Format (Y/N)? ','$'
msg_pct     db  ' percent completed','$'
msg_wipe    db  '                     ','$'
msg_label   db  13,10,'Volume label (11 characters, ENTER for none)? ','$'
msg_done    db  13,10,'Format complete',13,10,13,10,'$'
msg_total   db  ' bytes total disk space',13,10,'$'
msg_bad     db  ' bytes in bad sectors',13,10,'$'
msg_avail   db  ' bytes available on disk',13,10,'$'
msg_usage   db  'Usage: FORMAT d: [/Q]',13,10,'$'
msg_baddrv  db  'Invalid drive specification',13,10,'$'
msg_badsw   db  'Invalid switch',13,10,'$'
msg_iscur   db  'Cannot format the current drive',13,10,'$'
msg_notmnt  db  'Invalid media or Track 0 bad - disk unusable',13,10
            db  '(No readable BPB: raw media is MKDISK',27h,'s business)',13,10,'$'
msg_ioerr   db  13,10,'Format failure',13,10,'$'
msg_crlf    db  13,10,'$'

img_end:                                ; the BSS begins here

_FMT    ENDS
        END
