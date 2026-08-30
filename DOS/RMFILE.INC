;=====================================================================
; RMFILE.INC - the FAT, the directories, the handles, and the calls
;              that reach them
;
; FAT12, FAT16 and FAT32, read and write.  8.3 names only: Windows 3.x
; has no long names to ask for, and an LFN implementation here would
; be a second one to keep in step with the kernel's for no caller.
;
; THREE CACHES AND NO MORE.  One FAT window, one directory sector, one
; data sector.  DOS had BUFFERS= and a chain; this has three fixed
; buffers, because an excursion is short, the working set is one file
; at a time, and a cache with a replacement policy is a cache with a
; coherency bug waiting in it.  Everything dirty is written before
; anything else is read into the same buffer, which is the whole of
; the policy.
;=====================================================================

;--- the system file table -------------------------------------------
SFT_REF         EQU 00h                 ; dw  handles pointing here, 0 = free
SFT_MODE        EQU 02h                 ; dw  as 3Dh was given it
SFT_ATTR        EQU 04h                 ; db
SFT_FLAGS       EQU 05h                 ; db  SFF_*
SFT_DRIVE       EQU 06h                 ; db  0 = A, or a device index
SFT_NAME        EQU 07h                 ; 11 bytes, the 8.3 form
SFT_START       EQU 12h                 ; dd  first cluster
SFT_SIZE        EQU 16h                 ; dd
SFT_POS         EQU 1Ah                 ; dd
SFT_CURCLUS     EQU 1Eh                 ; dd  the cluster POS is in
SFT_CURIDX      EQU 22h                 ; dd  and which index that is
SFT_DIRLBA      EQU 26h                 ; dd  where its directory entry is
SFT_DIROFF      EQU 2Ah                 ; dw  and where in that sector
SFT_TIME        EQU 2Ch                 ; dw
SFT_DATE        EQU 2Eh                 ; dw
SFT_LEN         EQU 30h

SFF_DEVICE      EQU 01h                 ; a character device, not a file
SFF_WRITTEN     EQU 02h                 ; the directory entry needs updating
SFF_EOF         EQU 04h

SFT_COUNT       EQU 40
JFT_COUNT       EQU 20                  ; what fits in a PSP, and what 67h
                                        ; would have to move somewhere else

;--- devices, in the order the standard handles open them ------------
DEV_CON         EQU 0
DEV_AUX         EQU 1
DEV_PRN         EQU 2
DEV_NUL         EQU 3

;--- a directory entry, which has not moved since 1983 ---------------
DE_NAME         EQU 00h
DE_ATTR         EQU 0Bh
DE_TIME         EQU 16h
DE_DATE         EQU 18h
DE_CLUSHI       EQU 14h
DE_CLUSLO       EQU 1Ah
DE_SIZE         EQU 1Ch
DE_LEN          EQU 32

;--- the current directory of each drive -----------------------------
CDS_CLUS        EQU 00h                 ; dd  0 = the root
CDS_PATH        EQU 04h                 ; 64 bytes, ASCIZ, no drive, no
                                        ; leading backslash - what 47h wants
CDS_LEN         EQU 44h

;=====================================================================
; THE FAT WINDOW
;
; Two sectors, not one, and for one reason: a FAT12 entry may straddle
; a sector boundary, and with the following sector already in the
; buffer the word read at offset 511 simply spills into it and needs
; no special case anywhere.  The same trick is in BOOT\FATLOAD.INC and
; for the same reason.
;=====================================================================

;--- fat_win - SI = volume, EAX = the FAT sector wanted.  Loads the
;--- pair starting there if it is not already in.
fat_win:
        pushad
        push    es
        cmp     si,[rd_fatvol]
        jne     fw_load
        cmp     eax,dword ptr [rd_fatlba]
        je      fw_done
fw_load:
        call    fat_flush
        mov     dword ptr [rd_fatlba],eax
        mov     [rd_fatvol],si
        add     eax,dword ptr [si+VOL_FATLBA]
        mov     bx,cs
        mov     es,bx
        mov     bx,OFFSET rd_fatbuf
        mov     cx,2
        call    rd_rdsec
        jnc     fw_done
        mov     dword ptr [rd_fatlba],-1        ; do not believe what is in
        mov     word ptr [rd_fatvol],0          ; the buffer next time
fw_done:
        pop     es
        popad
        ret

;--- fat_flush - write the window back if anything changed, to EVERY
;--- copy of the FAT.  A second FAT that does not match the first is
;--- what CHKDSK exists to complain about.
fat_flush:
        pushad
        push    es
        cmp     byte ptr [rd_fatdirty],0
        je      ff_done
        mov     si,[rd_fatvol]
        or      si,si
        jz      ff_clean
        mov     bx,cs
        mov     es,bx
        movzx   cx,byte ptr [si+VOL_NFATS]
        or      cx,cx
        jz      ff_clean
        mov     dword ptr [rd_fatn],0
ff_next:
        mov     eax,dword ptr [rd_fatn]
        mul     dword ptr [si+VOL_SPF]
        add     eax,dword ptr [rd_fatlba]
        add     eax,dword ptr [si+VOL_FATLBA]
        push    cx
        mov     bx,OFFSET rd_fatbuf
        mov     cx,2
        call    rd_wrsec
        pop     cx
        inc     dword ptr [rd_fatn]
        loop    ff_next
ff_clean:
        mov     byte ptr [rd_fatdirty],0
ff_done:
        pop     es
        popad
        ret

;--- fat_get - SI = volume, EAX = cluster -> EAX = its entry ---------
fat_get:
        push    ebx
        push    ecx
        push    edx
        push    di
        mov     ebx,eax
        movzx   ecx,byte ptr [si+VOL_FATTYPE]
        cmp     cl,12
        je      fg_12
        cmp     cl,16
        je      fg_16

;--- FAT32: four bytes each, and the top four bits are not ours ------
        shl     eax,2
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        mov     eax,dword ptr [rd_fatbuf + di]
        and     eax,0FFFFFFFh
        jmp     short fg_done

fg_16:
        shl     eax,1
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        movzx   eax,word ptr [rd_fatbuf + di]
        jmp     short fg_done

;--- FAT12: twelve bits, so the byte offset is cluster + cluster/2 ---
fg_12:
        mov     edx,eax
        shr     edx,1
        add     eax,edx
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        movzx   eax,word ptr [rd_fatbuf + di]
        test    bl,1
        jz      fg_12even
        shr     eax,4
        jmp     short fg_12done
fg_12even:
        and     eax,0FFFh
fg_12done:
        ; A FAT12 end-of-chain is FF8h and up; widen it so every caller
        ; can test against one number whatever the volume is.
        cmp     eax,0FF8h
        jb      fg_done
        or      eax,0FFFF000h
fg_done:
        pop     di
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- fat_put - SI = volume, EAX = cluster, EDX = the value -----------
fat_put:
        pushad
        mov     ebx,eax
        mov     ebp,edx                 ; BP is not a frame pointer in here
        movzx   ecx,byte ptr [si+VOL_FATTYPE]
        cmp     cl,12
        je      fp_12
        cmp     cl,16
        je      fp_16

        shl     eax,2
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        mov     eax,dword ptr [rd_fatbuf + di]
        and     eax,0F0000000h          ; the reserved top nibble stays
        and     ebp,0FFFFFFFh
        or      eax,ebp
        mov     dword ptr [rd_fatbuf + di],eax
        jmp     short fp_done

fp_16:
        shl     eax,1
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        mov     word ptr [rd_fatbuf + di],bp
        jmp     short fp_done

fp_12:
        mov     edx,eax
        shr     edx,1
        add     eax,edx
        mov     edx,eax
        shr     eax,9
        call    fat_win
        and     edx,1FFh
        mov     di,dx
        mov     ax,word ptr [rd_fatbuf + di]
        test    bl,1
        jz      fp_12even
        and     ax,000Fh                ; keep the low nibble of the pair
        mov     cx,bp
        shl     cx,4
        or      ax,cx
        jmp     short fp_12store
fp_12even:
        and     ax,0F000h               ; keep the high nibble
        mov     cx,bp
        and     cx,0FFFh
        or      ax,cx
fp_12store:
        mov     word ptr [rd_fatbuf + di],ax
fp_done:
        mov     byte ptr [rd_fatdirty],1
        popad
        ret

;--- fat_eoc - EAX = a FAT entry.  CF set if it ends a chain ---------
fat_eoc:
        cmp     eax,0FFFFFF8h
        jb      fe_no
        stc
        ret
fe_no:
        clc
        ret

;--- fat_alloc - SI = volume -> EAX = a cluster, marked as the end of
;--- a chain, or zero if the volume is full.
;
; The search starts where the last one finished rather than at cluster
; two, which turns filling a disk from something quadratic into
; something linear.  It wraps once, so a volume with a hole near the
; start is still filled.
fat_alloc:
        push    ebx
        push    ecx
        push    edx
        mov     ebx,dword ptr [si+VOL_FREEHINT]
        cmp     ebx,2
        jae     fa_hinted
        mov     ebx,2
fa_hinted:
        mov     ecx,dword ptr [si+VOL_CLUSTERS]
        add     ecx,2                   ; the highest legal number, plus one
        mov     edx,ecx
        sub     edx,2
        inc     edx                     ; how many to look at before giving up
fa_scan:
        or      edx,edx
        jz      fa_full
        dec     edx
        cmp     ebx,ecx
        jb      fa_inrange
        mov     ebx,2
fa_inrange:
        mov     eax,ebx
        call    fat_get
        or      eax,eax
        jz      fa_got
        inc     ebx
        jmp     short fa_scan
fa_got:
        mov     eax,ebx
        push    eax
        mov     edx,0FFFFFFFh
        call    fat_put
        pop     eax
        mov     ebx,eax
        inc     ebx
        mov     dword ptr [si+VOL_FREEHINT],ebx
        pop     edx
        pop     ecx
        pop     ebx
        ret
fa_full:
        xor     eax,eax
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- fat_freechain - SI = volume, EAX = the first cluster ------------
fat_freechain:
        pushad
        mov     ebx,eax
fc_loop:
        cmp     ebx,2
        jb      fc_done
        mov     eax,ebx
        call    fat_get
        push    eax
        mov     eax,ebx
        xor     edx,edx
        call    fat_put
        pop     ebx
        call    fat_eoc
        jnc     fc_loop
fc_done:
        popad
        ret

;--- rd_countfree - SI = volume -> EAX = free clusters ---------------
rd_countfree:
        push    ebx
        push    ecx
        push    edx
        xor     edx,edx                 ; the running count
        mov     ebx,2
        mov     ecx,dword ptr [si+VOL_CLUSTERS]
        add     ecx,2
cf_loop:
        cmp     ebx,ecx
        jae     cf_done
        mov     eax,ebx
        call    fat_get
        or      eax,eax
        jnz     cf_used
        inc     edx
cf_used:
        inc     ebx
        jmp     short cf_loop
cf_done:
        mov     eax,edx
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- clus2lba - SI = volume, EAX = cluster -> EAX = its first sector -
clus2lba:
        push    ecx
        sub     eax,2
        movzx   ecx,byte ptr [si+VOL_SPC]
        mul     ecx
        add     eax,dword ptr [si+VOL_DATALBA]
        pop     ecx
        ret

;=====================================================================
; THE DIRECTORY SECTOR CACHE
;=====================================================================

;--- dir_win - SI = volume, EAX = an absolute LBA -> that sector is in
;--- rd_dirbuf.  CF on a read error.
dir_win:
        pushad
        push    es
        cmp     si,[rd_dirvol]
        jne     dw_load
        cmp     eax,dword ptr [rd_dirlba]
        je      dw_ok
dw_load:
        call    dir_flush
        mov     dword ptr [rd_dirlba],eax
        mov     [rd_dirvol],si
        mov     bx,cs
        mov     es,bx
        mov     bx,OFFSET rd_dirbuf
        mov     cx,1
        call    rd_rdsec
        jnc     dw_ok
        mov     dword ptr [rd_dirlba],-1
        mov     word ptr [rd_dirvol],0
        pop     es
        popad
        stc
        ret
dw_ok:
        pop     es
        popad
        clc
        ret

dir_flush:
        pushad
        push    es
        cmp     byte ptr [rd_dirdirty],0
        je      df_done
        mov     si,[rd_dirvol]
        or      si,si
        jz      df_clean
        mov     bx,cs
        mov     es,bx
        mov     bx,OFFSET rd_dirbuf
        mov     eax,dword ptr [rd_dirlba]
        mov     cx,1
        call    rd_wrsec
df_clean:
        mov     byte ptr [rd_dirdirty],0
df_done:
        pop     es
        popad
        ret

;--- rd_flush - everything, everywhere.  0Dh and every close call it.
rd_flush:
        call    fat_flush
        call    dir_flush
        ret

;=====================================================================
; NAMES
;
; rd_name83 - DS:SI -> one path component, ES:DI -> eleven bytes.
;             Turns "readme.txt" into "README  TXT".  Stops at a
;             separator or a NUL and leaves SI on it.
;
; A NAME THAT IS TOO LONG IS TRUNCATED, NOT REFUSED, which is what DOS
; did: "verylongname.text" opens "VERYLONG.TEX".  Refusing would be
; more honest and would also mean a program that has always worked
; stops working, so it truncates.
;=====================================================================
rd_name83:
        push    ax
        push    cx
        push    di
        mov     al,' '
        mov     cx,11
        cld
        rep     stosb
        sub     di,11

        mov     cx,8
n83_base:
        mov     al,[si]
        cmp     al,0
        je      n83_done
        cmp     al,'\'
        je      n83_done
        cmp     al,'/'
        je      n83_done
        cmp     al,'.'
        je      n83_dot
        inc     si
        or      cx,cx
        jz      n83_base                ; past eight: swallow it
        call    rd_upcase
        mov     [di],al
        inc     di
        dec     cx
        jmp     short n83_base
n83_dot:
        inc     si
        pop     di
        push    di
        add     di,8
        mov     cx,3
n83_ext:
        mov     al,[si]
        cmp     al,0
        je      n83_done
        cmp     al,'\'
        je      n83_done
        cmp     al,'/'
        je      n83_done
        inc     si
        or      cx,cx
        jz      n83_ext
        call    rd_upcase
        mov     [di],al
        inc     di
        dec     cx
        jmp     short n83_ext
n83_done:
        pop     di
        pop     cx
        pop     ax
        ret

;--- rd_upcase - AL, in place.  ASCII only, which is the whole of what
;--- codepage 437 upper-cases and what PM-DOS does too.
rd_upcase:
        cmp     al,'a'
        jb      uc_done
        cmp     al,'z'
        ja      uc_done
        sub     al,20h
uc_done:
        ret

;=====================================================================
; rd_path - resolve the caller's path.
;
;   ES:SI -> the path, in the CALLER's segment
;   Returns: AL = drive, EBX = the directory's start cluster (0 = the
;            root of a FAT12/16 volume), rd_fcb = the final component
;            as eleven bytes, SI = volume record.  CF on error.
;
; The last component is NOT looked up - that is the caller's business,
; because create and open want different things when it is missing.
;=====================================================================
rd_path:
        push    cx
        push    dx
        push    di
        push    bp
        push    es

        ; THE PATH IS COPIED IN FIRST, and that is not tidiness.  Every
        ; routine below this one - rd_getvol, rd_mount, dir_find,
        ; fat_get - reaches RMDOS's own data through DS.  The first
        ; version of this routine pointed DS at the CALLER's segment so
        ; it could read the string, and then called all of them: the
        ; volume table it read was whatever the caller happened to have
        ; at that offset.  It failed in exactly one place, on the one
        ; check that opens a file, and looked like a FAT bug.
        ;
        ; With the string in our own segment there is no second segment
        ; in the routine at all.
        push    ds
        push    es
        pop     ds                      ; DS:SI is the caller's string
        push    cs
        pop     es                      ; ES:DI is our own buffer
        mov     di,OFFSET rd_pathbuf
        mov     cx,RD_PATHMAX - 1
        cld
rp_copy:
        lodsb
        stosb
        or      al,al
        jz      rp_copied
        loop    rp_copy
        mov     byte ptr es:[di],0      ; the loop ran out before the string
rp_copied:
        pop     ds
        push    cs
        pop     ds
        push    cs
        pop     es
        mov     bp,OFFSET rd_pathbuf

        mov     al,[rd_drive]
        cmp     byte ptr [bp+1],':'
        jne     rp_nodrive
        mov     al,[bp]
        call    rd_upcase
        sub     al,'A'
        add     bp,2
rp_nodrive:
        mov     [rp_drv],al

        ; Mount it before walking it, so a bad drive letter fails here
        ; rather than three components in.
        call    rd_getvol
        jc      rp_bad
        mov     [rp_vol],si

        ; Where does the walk start?  A leading separator says the
        ; root; anything else says wherever this drive is standing.
        xor     ebx,ebx
        mov     al,[bp]
        cmp     al,'\'
        je      rp_fromroot
        cmp     al,'/'
        je      rp_fromroot
        mov     al,[rp_drv]
        call    rd_cdsclus              ; -> EBX
        jmp     short rp_comp
rp_fromroot:
        inc     bp

rp_comp:
        ; One component into rd_fcb.  rd_name83 reads DS:SI and writes
        ; ES:DI, and both are ours.
        mov     si,bp
        mov     di,OFFSET rd_fcb
        call    rd_name83
        mov     bp,si

        mov     al,[bp]
        cmp     al,'\'
        je      rp_descend
        cmp     al,'/'
        je      rp_descend
        jmp     short rp_last

rp_descend:
        inc     bp
        cmp     byte ptr [bp],0
        je      rp_last                 ; a trailing separator names the
                                        ; directory itself
        mov     si,[rp_vol]
        mov     eax,ebx
        mov     di,OFFSET rd_fcb
        call    dir_find
        jc      rp_bad
        mov     si,[rd_deoff]
        test    byte ptr [rd_dirbuf + si + DE_ATTR],ATTR_DIR
        jz      rp_bad                  ; a file cannot be walked through
        call    rd_declus               ; -> EBX
        jmp     short rp_comp

rp_last:
        mov     al,[rp_drv]
        mov     si,[rp_vol]
        pop     es
        pop     bp
        pop     di
        pop     dx
        pop     cx
        clc
        ret
rp_bad:
        pop     es
        pop     bp
        pop     di
        pop     dx
        pop     cx
        mov     ax,ERR_NOPATH
        stc
        ret
;--- rd_declus - the cluster of the entry sitting in rd_dirbuf at
;--- rd_deoff, into EBX.  Both halves, because FAT32 uses both.
rd_declus:
        push    si
        mov     si,word ptr cs:[rd_deoff]
        movzx   ebx,word ptr cs:[rd_dirbuf + si + DE_CLUSHI]
        shl     ebx,16
        mov     bx,word ptr cs:[rd_dirbuf + si + DE_CLUSLO]
        pop     si
        ret

;--- rd_cdsclus - AL = drive -> EBX = where it is standing -----------
rd_cdsclus:
        push    ax
        push    si
        movzx   si,al
        mov     ax,CDS_LEN
        mul     si
        mov     si,ax
        add     si,OFFSET rd_cds
        mov     ebx,dword ptr cs:[si+CDS_CLUS]
        pop     si
        pop     ax
        ret

;=====================================================================
; dir_find - SI = volume, EAX = the directory's start cluster (0 = the
;            root of a FAT12/16 volume), DI -> eleven bytes.
;
; Found: CF clear, the entry's sector is in rd_dirbuf and rd_deoff is
; its offset in it.  Not found: CF set.
;
; ONE ROUTINE WALKS BOTH KINDS OF DIRECTORY.  A FAT12/16 root is a
; fixed run of sectors and everything else is a cluster chain, and the
; only difference the search sees is how it gets the next sector - so
; that is the only thing the two paths do differently.
;=====================================================================
dir_find:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    di
        mov     word ptr [rd_dfname],di
        call    dir_first
        jc      dfi_no
dfi_sector:
        mov     eax,dword ptr [rd_dfsec]
        call    dir_win
        jc      dfi_no
        xor     bx,bx
dfi_entry:
        mov     al,byte ptr [rd_dirbuf + bx]
        or      al,al
        jz      dfi_no                  ; a zero name ends the directory
        cmp     al,0E5h
        je      dfi_next
        mov     al,byte ptr [rd_dirbuf + bx + DE_ATTR]
        and     al,0Fh
        cmp     al,0Fh
        je      dfi_next                ; a long-name fragment, skipped -
                                        ; the 8.3 entry it belongs to is
                                        ; along in a moment
        push    bx
        push    si
        push    di
        mov     di,[rd_dfname]
        lea     si,[rd_dirbuf + bx]
        mov     cx,11
        push    ds
        push    es
        push    cs
        pop     ds
        push    cs
        pop     es
        cld
        repe    cmpsb
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     bx
        je      dfi_found
dfi_next:
        add     bx,DE_LEN
        cmp     bx,SECSIZE
        jb      dfi_entry
        call    dir_next
        jnc     dfi_sector
dfi_no:
        pop     di
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret
dfi_found:
        mov     [rd_deoff],bx
        mov     eax,dword ptr [rd_dfsec]
        mov     dword ptr [rd_delba],eax
        pop     di
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret

;--- dir_first - SI = volume, EAX = start cluster (0 = root).  Sets up
;--- the walk state and leaves the first sector's LBA in rd_dfsec.
dir_first:
        mov     dword ptr [rd_dfclus],eax
        or      eax,eax
        jnz     dfr_sub

        ; The fixed root of a FAT12/16 volume.  A FAT32 volume has no
        ; such thing, and its root cluster stands in for it.
        cmp     byte ptr [si+VOL_FATTYPE],32
        jne     dfr_root
        mov     eax,dword ptr [si+VOL_ROOTCLUS]
        mov     dword ptr [rd_dfclus],eax
        jmp     short dfr_sub
dfr_root:
        mov     eax,dword ptr [si+VOL_ROOTLBA]
        mov     dword ptr [rd_dfsec],eax
        movzx   eax,word ptr [si+VOL_ROOTENT]
        shl     eax,5
        add     eax,SECSIZE - 1
        shr     eax,9
        mov     dword ptr [rd_dfleft],eax
        mov     byte ptr [rd_dfroot],1
        or      eax,eax
        jz      dfr_none
        clc
        ret
dfr_none:
        stc
        ret
dfr_sub:
        mov     byte ptr [rd_dfroot],0
        mov     eax,dword ptr [rd_dfclus]
        cmp     eax,2
        jb      dfr_none
        call    clus2lba
        mov     dword ptr [rd_dfsec],eax
        movzx   eax,byte ptr [si+VOL_SPC]
        mov     dword ptr [rd_dfleft],eax
        clc
        ret

;--- dir_next - the next sector of the directory being walked.  CF set
;--- when there are no more.
dir_next:
        push    eax
        dec     dword ptr [rd_dfleft]
        jz      dn_cluster
        inc     dword ptr [rd_dfsec]
        pop     eax
        clc
        ret
dn_cluster:
        cmp     byte ptr [rd_dfroot],0
        jne     dn_end                  ; a fixed root does not chain
        mov     eax,dword ptr [rd_dfclus]
        call    fat_get
        call    fat_eoc
        jc      dn_end
        cmp     eax,2
        jb      dn_end
        mov     dword ptr [rd_dfclus],eax
        call    clus2lba
        mov     dword ptr [rd_dfsec],eax
        movzx   eax,byte ptr [si+VOL_SPC]
        mov     dword ptr [rd_dfleft],eax
        pop     eax
        clc
        ret
dn_end:
        pop     eax
        stc
        ret

;=====================================================================
; dir_alloc - SI = volume, EAX = the directory's start cluster.  Find
;             a free entry, or grow the directory to make one.
;
; Leaves the sector in rd_dirbuf with rd_deoff pointing at 32 bytes
; that are the caller's to fill.  CF if the directory cannot grow.
;=====================================================================
dir_alloc:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     dword ptr [rd_dastart],eax
        call    dir_first
        jc      da_grow
da_sector:
        mov     eax,dword ptr [rd_dfsec]
        call    dir_win
        jc      da_fail
        xor     bx,bx
da_entry:
        mov     al,byte ptr [rd_dirbuf + bx]
        or      al,al
        je      da_take
        cmp     al,0E5h
        je      da_take
        add     bx,DE_LEN
        cmp     bx,SECSIZE
        jb      da_entry
        call    dir_next
        jnc     da_sector
da_grow:
        ; No room.  A subdirectory can be given another cluster; the
        ; fixed root of a FAT12/16 volume cannot, and that is the one
        ; hard limit DOS ever had on how many files a disk may hold.
        cmp     byte ptr [rd_dfroot],0
        jne     da_fail
        call    fat_alloc
        or      eax,eax
        jz      da_fail
        mov     ebx,eax                 ; the new cluster
        mov     eax,dword ptr [rd_dfclus]
        mov     edx,ebx
        call    fat_put                 ; chain it on
        mov     eax,ebx
        call    dir_zero
        jc      da_fail
        mov     eax,ebx
        call    clus2lba
        mov     dword ptr [rd_dfsec],eax
        call    dir_win
        jc      da_fail
        xor     bx,bx
da_take:
        mov     [rd_deoff],bx
        mov     eax,dword ptr [rd_dfsec]
        mov     dword ptr [rd_delba],eax
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
da_fail:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;--- dir_zero - SI = volume, EAX = cluster.  Fill it with zeroes, which
;--- is what makes a fresh directory cluster read as "no more entries".
dir_zero:
        pushad
        push    es
        call    clus2lba
        mov     ebx,eax
        movzx   ecx,byte ptr [si+VOL_SPC]
        push    cx
        mov     ax,cs
        mov     es,ax
        mov     di,OFFSET rd_datbuf
        mov     cx,SECSIZE
        xor     al,al
        cld
        rep     stosb
        pop     cx
dz_loop:
        mov     eax,ebx
        push    cx
        mov     bx,OFFSET rd_datbuf
        mov     cx,1
        call    rd_wrsec
        pop     cx
        jc      dz_bad
        inc     ebx
        loop    dz_loop
        pop     es
        popad
        clc
        ret
dz_bad:
        pop     es
        popad
        stc
        ret

;--- dir_commit - the entry at rd_deoff has been changed; mark the
;--- buffer so it goes out.
dir_commit:
        mov     byte ptr [rd_dirdirty],1
        ret

;=====================================================================
; Data
;=====================================================================
rd_fatbuf   db  (SECSIZE * 2) dup(0)
rd_fatlba   dd  -1
rd_fatvol   dw  0
rd_fatdirty db  0
rd_fatn     dd  0

rd_dirbuf   db  SECSIZE dup(0)
rd_dirlba   dd  -1
rd_dirvol   dw  0
rd_dirdirty db  0

rd_datbuf   db  SECSIZE dup(0)

RD_PATHMAX  EQU 128
rd_pathbuf  db  RD_PATHMAX dup(0)
rd_fcb      db  11 dup(0)               ; the component being looked for
rd_deoff    dw  0                       ; where the found entry is, in buf
rd_delba    dd  0                       ; and which sector that was

rd_dfname   dw  0                       ; dir_find's arguments and state
rd_dfsec    dd  0
rd_dfclus   dd  0
rd_dfleft   dd  0
rd_dfroot   db  0
rd_dastart  dd  0

rp_drv      db  0
rp_vol      dw  0
rp_indir    db  0

rd_cds      db  (LASTDRIVE * CDS_LEN) dup(0)
rd_sfts     db  (SFT_COUNT * SFT_LEN) dup(0)
