;=====================================================================
; EXFAT.INC - exFAT, read and written, in FAT32's shape
;
; THE SHAPE IS THE DESIGN, as it is for ISO9660 (iso_entry).  Above the
; directory layer this kernel reads thirty-two byte FAT entries and
; walks cluster chains, and an exFAT volume is made to answer in those
; terms: dir_seek hands back a FAT entry built out of an exFAT entry
; set, fat_next answers the next cluster, fat_dirupdate takes a changed
; FAT entry and writes the set it came from.  Everything that opens,
; reads, writes, creates, renames and deletes is the code FAT has always
; used; what is exFAT's is here.
;
; WHAT DOES NOT FIT THE SHAPE, AND HOW IT IS MADE TO
;
;   A file is a SET of entries - a file entry, a stream extension, a
;   name entry for each fifteen characters - in that order, the name
;   AFTER the entry where FAT puts it before.  Directory index N is the
;   N'th 32-byte slot: a file entry's slot answers with the whole file,
;   its other entries with something no walker takes for a file (an
;   LFN-shaped fragment, which every walker steps over), a deleted
;   entry as E5h and the end as 0.  A subdirectory has no "." and ".."
;   on the disk, and gets them made up as indexes 0 and 1, because DOS
;   programs count on them; the slots start at 2 there.
;
;   There are no 8.3 names.  A name that fits 8.3 is its own alias; any
;   other gets STEM~N.EXT with N from its slot, which is unique in the
;   directory as ISO's record ordinal is.  The full name is in lfn_buf
;   with iso_lname set, the flag lfnw_seal reads for a CD.
;
;   A FILE'S CLUSTERS NEED NOT BE IN THE FAT.  NoFatChain says they are
;   one run, and then the FAT entries under them are whatever they were
;   - another file's old chain, as often as not - and following them
;   reads somebody else's data.  So fat_next asks, before the FAT, whether
;   the cluster is in a RUN it has been told about: an open file's (the
;   SFT carries it, SFT_XRUN, and each walk of the file sets it here),
;   a directory's (a table of every directory a walk has gone through,
;   kept for the life of the mount, which is what lets a directory be
;   found again from nothing but its first cluster), or the entry
;   de_getclus last read (which covers deleting a file).  A run that is
;   then changed in any way but grown at its end, or cut, is written into
;   the FAT and becomes a chain; the entry's flag says which at the next
;   update.
;
;   Allocation is a BITMAP, not FAT entries of zero, and the FAT entry of
;   a free cluster means nothing.  A cluster bigger than the kernel's 32K
;   buffer is presented as several kernel clusters (xv_kshift), numbered
;   in order so that clus2lba's arithmetic needs no change.
;
;   ValidDataLength: bytes past it read as zeroes, and the first write
;   to such a file zeroes them on the disk, after which it is the size.
;
; WHAT IT DOES NOT DO: TexFAT (two FATs) is read and never written; a
; file of 4GB or more is shown as FFFFFFFFh bytes, read-only; names are
; the kernel's characters, a byte each, as long names are on FAT.  The
; boot code is not written - nothing here makes an exFAT volume boot.
;
; Included by DOSMAIN.ASM after FAT.INC and GPT.INC.
;=====================================================================

; a directory the registry knows (xv_reg*)
XFR_FIRST       EQU     0               ; dd - its first kernel cluster
XFR_RUN         EQU     4               ; dd - its run, 0 = a FAT chain
XFR_PDIR        EQU     8               ; dd - the directory it is in
XFR_PSLOT       EQU     12              ; dd - the slot of its file entry there
XFR_LEN         EQU     16
XFR_GROW        EQU     64              ; records the table starts with

; what the entry dir_seek last made was made from: XVK_* in FAT.INC

XV_SETMAX       EQU     19              ; entries in the biggest set
XV_DIRMAX       EQU     800000h         ; entries in the biggest directory

;=====================================================================
; THE MOUNT
;=====================================================================

;---------------------------------------------------------------------
; xv_mount - fat_mount found "EXFAT   " in the boot sector, which is in
;            sec_buf.  Lay the volume out in FAT32's shape.  CF on
;            failure, [xv_why] = 1 if a sector would not read (INT 24h
;            has been raised already), 0 if it is not a volume to mount.
;---------------------------------------------------------------------
xv_mount:
        pushad
        mov     byte ptr [xv_why],0

        ; THE SHIFTS FIRST: everything after them is in their units.  A
        ; sector of 512 to 4096 bytes, a cluster of up to 32MB, as the
        ; specification bounds them; the kernel counts in 512-byte units
        ; and turns the volume's own into those once, here, as fm_scale
        ; does for a FAT volume with big sectors.
        movzx   ecx,byte ptr [sec_buf+XFB_BPSSH]
        cmp     ecx,9
        jb      xm_bad
        cmp     ecx,12
        ja      xm_bad
        movzx   edx,byte ptr [sec_buf+XFB_SPCSH]
        lea     eax,[ecx+edx]
        cmp     eax,25
        ja      xm_bad
        sub     ecx,9
        mov     [fat_lshift],cl
        sub     eax,9
        mov     [xv_pshift],al

        ; BOTH BOOT REGIONS ARE CHECKSUMMED, and the volume is mounted
        ; from one that checks: the backup is there for a main region
        ; somebody wrote over.  xv_bootck leaves the good boot sector in
        ; sec_buf.
        call    xv_bootck
        jc      xm_fail
        cmp     byte ptr [sec_buf+XFB_REV+1],1  ; revision 1.xx
        jne     xm_bad
        movzx   ecx,byte ptr [fat_lshift]

        ; the volume's length, in units, as far as 32 bits go - and no
        ; further than the partition it is in
        mov     eax,dword ptr [sec_buf+XFB_VOLLEN]
        mov     edx,dword ptr [sec_buf+XFB_VOLLEN+4]
        shld    edx,eax,cl
        shl     eax,cl
        or      edx,edx
        jz      xm_lenok
        mov     eax,0FFFFFFFFh
xm_lenok:
        mov     edx,[fat_partsecs]
        or      edx,edx
        jz      xm_lenpart
        cmp     eax,edx
        jbe     xm_lenpart
        mov     eax,edx
xm_lenpart:
        mov     [fat_totsec],eax
        mov     [xm_vlen],eax

        ; the FAT: past the boot regions, inside the volume
        mov     eax,dword ptr [sec_buf+XFB_FATOFF]
        cmp     eax,2 * XFB_REGION
        jb      xm_bad
        call    xm_units
        jc      xm_bad
        mov     [fat_rsvd],eax
        mov     ebx,eax
        add     eax,[fat_partlba]
        mov     [fat_fatlba],eax
        mov     eax,dword ptr [sec_buf+XFB_FATLEN]
        or      eax,eax
        jz      xm_bad
        call    xm_units
        jc      xm_bad
        mov     [fat_spf],eax
        movzx   edx,byte ptr [sec_buf+XFB_NFATS]
        cmp     edx,1
        je      xm_nfat
        cmp     edx,2
        jne     xm_bad
xm_nfat:
        mov     [fat_nfats],edx
        imul    eax,edx
        add     ebx,eax                 ; EBX = units the FATs end at
        jc      xm_bad

        ; the heap, after the FATs
        mov     eax,dword ptr [sec_buf+XFB_HEAP]
        call    xm_units
        jc      xm_bad
        cmp     eax,ebx
        jb      xm_bad
        mov     ebx,eax
        add     eax,[fat_partlba]
        mov     [fat_datalba],eax

        ; its clusters: at least one, all in the volume, all in the FAT
        mov     eax,dword ptr [sec_buf+XFB_NCLUS]
        or      eax,eax
        jz      xm_bad
        cmp     eax,0FFFFFFF5h          ; the most the format allows
        ja      xm_bad
        mov     [xv_nclus],eax
        xor     edx,edx
        mov     cl,[xv_pshift]
        shld    edx,eax,cl
        shl     eax,cl                  ; EDX:EAX = the heap's units
        add     eax,ebx
        adc     edx,0
        jnz     xm_bad
        cmp     eax,[xm_vlen]
        ja      xm_bad
        mov     eax,[xv_nclus]
        add     eax,2                   ; entries the FAT needs...
        mov     edx,[fat_spf]
        shl     edx,7                   ; ...and has, 128 a unit
        jc      xm_spfok
        cmp     eax,edx
        ja      xm_bad
xm_spfok:

        ; THE KERNEL'S CLUSTER: the volume's, up to the 32K clus_buf
        ; holds, and a fixed fraction of it above that
        mov     cl,[xv_pshift]
        xor     edx,edx
        cmp     cl,6
        jbe     xm_kfit
        mov     dl,cl
        sub     dl,6
        mov     cl,6
xm_kfit:
        mov     [xv_kshift],dl
        mov     eax,1
        shl     eax,cl
        mov     [fat_spc],eax
        mov     eax,[xv_nclus]
        mov     cl,dl
        mov     edx,eax
        shl     eax,cl
        shr     eax,cl                  ; it must come back, or the kernel's
        cmp     eax,edx                 ; cluster numbers will not hold it
        jne     xm_bad
        shl     eax,cl
        cmp     eax,0FFFFFFF0h
        jae     xm_bad
        mov     [fat_totclus],eax

        mov     eax,dword ptr [sec_buf+XFB_ROOT]
        cmp     eax,2
        jb      xm_bad
        mov     edx,[xv_nclus]
        inc     edx
        cmp     eax,edx
        ja      xm_bad
        call    xv_virt
        mov     [fat_rootclus],eax

        mov     byte ptr [fat_type],32
        mov     dword ptr [fat_eoc],0FFFFFFF8h
        mov     dword ptr [fat_eocv],0FFFFFFFFh
        mov     byte ptr [fat_media],0F8h
        mov     dword ptr [fat_rootent],0
        mov     dword ptr [fat_hint],2
        mov     word ptr [fat_fsinfo],0FFFFh
        mov     word ptr [fat_bkboot],0FFFFh
        mov     eax,dword ptr [sec_buf+XFB_SERIAL]
        mov     [xv_serial],eax

        ; TEXFAT IS READ AND NOT WRITTEN: its second FAT and bitmap are
        ; a transaction this does not take part in.  Which copy is live
        ; is the volume's to say, and fat_active is how fat_rdcopy hears.
        mov     dword ptr [fat_active],0
        mov     byte ptr [xv_ro],0
        cmp     dword ptr [fat_nfats],1
        je      xm_onefat
        mov     byte ptr [xv_ro],1
        mov     dword ptr [fat_active],1
        test    byte ptr [sec_buf+XFB_FLAGS],XFBF_ACTIVEFAT
        jz      xm_onefat
        mov     dword ptr [fat_active],2
xm_onefat:

        mov     byte ptr [vol_fs],FS_EXFAT
        mov     dword ptr [xv_bmlba],-1
        mov     byte ptr [xv_bmdirty],0
        mov     dword ptr [xv_free],-1
        mov     dword ptr [xv_ahint],2
        mov     dword ptr [xv_regn],0
        mov     dword ptr [xv_bmci],0
        mov     byte ptr [xv_lcount],0
        call    xv_tclear

        ; the boot sector is about to leave sec_buf; the flag in it is
        ; what the clean-shutdown bit is read from
        mov     esi,OFFSET sec_buf
        mov     edi,OFFSET xv_bsave
        mov     ecx,128
        rep     movsd

        call    xv_rootscan
        jc      xm_fail
        call    xv_vfmount
        popad
        clc
        ret

xm_bad:
        mov     byte ptr [xv_why],0
xm_fail:
        popad
        stc
        ret

; xm_units - EAX = a count of the volume's sectors -> 512-byte units,
;            CF if it does not fit in 32 bits.  ECX's low byte is not
;            touched; the shift is read afresh.
xm_units:
        push    ecx
        push    edx
        movzx   ecx,byte ptr [fat_lshift]
        mov     edx,eax
        shl     eax,cl
        push    eax
        shr     eax,cl
        cmp     eax,edx
        pop     eax
        pop     edx
        pop     ecx
        je      xmu_ok
        stc
        ret
xmu_ok:
        clc
        ret

;---------------------------------------------------------------------
; xv_bootck - checksum the main boot region, and the backup if the main
;             one does not check.  CF if neither does ([xv_why] says
;             whether that was a read failing).  On success sec_buf holds
;             the first 512 bytes of the boot sector of the region that
;             checked.
;
; THE SUM IS OVER ELEVEN SECTORS OF THE VOLUME'S OWN SIZE, leaving out
; three bytes of the first: the flags at 6Ah and the percentage at 70h,
; which change while the volume is in use.  The twelfth sector is the
; sum, over and over; its first dword is enough.
;---------------------------------------------------------------------
xv_bootck:
        pushad
        xor     ebp,ebp                 ; EBP = the region, 0 main, 1 backup
xbc_region:
        mov     eax,ebp
        imul    eax,XFB_REGION
        movzx   ecx,byte ptr [fat_lshift]
        shl     eax,cl
        mov     [xbc_base],eax
        mov     eax,11
        shl     eax,cl
        mov     [xbc_left],eax
        mov     dword ptr [xbc_unit],0
        xor     edx,edx                 ; EDX = the sum
xbc_chunk:
        mov     ecx,[xbc_left]
        or      ecx,ecx
        jz      xbc_summed
        cmp     ecx,CLUSBUF_SIZE / 512
        jbe     xbc_csz
        mov     ecx,CLUSBUF_SIZE / 512
xbc_csz:
        mov     ebx,[xbc_base]
        add     ebx,[xbc_unit]
        add     ebx,[fat_partlba]
        mov     al,[fat_drive]
        mov     edi,OFFSET clus_buf
        call    disk_read
        jc      xbc_io
        mov     dword ptr [clusbuf_clus],-1
        sub     [xbc_left],ecx
        mov     eax,[xbc_unit]
        add     [xbc_unit],ecx
        shl     eax,9                   ; EAX = the byte this chunk starts at
        shl     ecx,9
        mov     esi,OFFSET clus_buf
xbc_byte:
        cmp     eax,106
        je      xbc_skip
        cmp     eax,107
        je      xbc_skip
        cmp     eax,112
        je      xbc_skip
        movzx   ebx,byte ptr [esi]
        ror     edx,1
        add     edx,ebx
xbc_skip:
        inc     esi
        inc     eax
        dec     ecx
        jnz     xbc_byte
        jmp     xbc_chunk

xbc_summed:
        mov     ebx,[xbc_base]
        movzx   ecx,byte ptr [fat_lshift]
        mov     eax,11
        shl     eax,cl
        add     ebx,eax
        add     ebx,[fat_partlba]
        mov     al,[fat_drive]
        mov     cx,1
        mov     edi,OFFSET clus_buf
        call    disk_read
        jc      xbc_io
        cmp     edx,dword ptr [clus_buf]
        jne     xbc_next
        ; this region checks: its boot sector is the one to use
        mov     ebx,[xbc_base]
        add     ebx,[fat_partlba]
        mov     al,[fat_drive]
        mov     cx,1
        mov     edi,OFFSET sec_buf
        call    disk_read
        jc      xbc_io
        cmp     dword ptr [sec_buf+XFB_NAME],'AFXE'
        jne     xbc_next
        popad
        clc
        ret
xbc_next:
        inc     ebp
        cmp     ebp,2
        jb      xbc_region
        mov     byte ptr [xv_why],0
        popad
        stc
        ret
xbc_io:
        mov     byte ptr [xv_why],1
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_rootscan - the root's own entries: the allocation bitmap (the one
;               for the live FAT, when there are two), the up-case table,
;               the label.  CF if there is no bitmap or no up-case table.
;---------------------------------------------------------------------
xv_rootscan:
        pushad
        call    ctx_root
        mov     dword ptr [xv_bmfirst],0
        mov     dword ptr [xrs_upfirst],0
        xor     ebx,ebx
xrs_lp:
        call    dir_rawseek
        jc      xrs_done
        mov     al,[esi]
        or      al,al
        jz      xrs_done
        cmp     al,XFT_BITMAP
        je      xrs_bitmap
        cmp     al,XFT_UPCASE
        je      xrs_upcase
        cmp     al,XFT_LABEL
        je      xrs_label
xrs_next:
        inc     ebx
        cmp     ebx,10000h              ; the system entries are always near
        jb      xrs_lp                  ; the front; a root bigger than this
        jmp     xrs_done          ; has had its chance
xrs_bitmap:
        ; with two, the one whose flag names the live FAT
        mov     al,[esi+1]
        and     al,1
        mov     ah,byte ptr [fat_active]
        or      ah,ah
        jz      xrs_bmtake              ; one FAT: the first bitmap
        dec     ah
        cmp     al,ah
        jne     xrs_next
xrs_bmtake:
        cmp     dword ptr [xv_bmfirst],0
        jne     xrs_next
        cmp     dword ptr [esi+XFE_DLEN+4],0
        jne     xrs_next
        mov     eax,[esi+XFE_FIRST]
        mov     [xv_bmfirst],eax
        mov     eax,[esi+XFE_DLEN]
        mov     [xv_bmlen],eax
        jmp     xrs_next
xrs_upcase:
        mov     eax,[esi+XFE_FIRST]
        mov     [xrs_upfirst],eax
        mov     eax,[esi+XFE_DLEN]
        cmp     dword ptr [esi+XFE_DLEN+4],0
        je      xrs_upl2
        xor     eax,eax
xrs_upl2:
        mov     [xrs_uplen],eax
        mov     eax,[esi+XFE_UPSUM]
        mov     [xrs_upsum],eax
        jmp     xrs_next
xrs_label:
        call    xv_lbltake
        jmp     xrs_next

xrs_done:
        ; the bitmap: there, inside the heap, and long enough
        mov     eax,[xv_bmfirst]
        call    xv_cluschk
        jc      xrs_bad
        mov     eax,[xv_nclus]
        add     eax,7
        shr     eax,3
        cmp     [xv_bmlen],eax
        jb      xrs_bad
        call    xv_bmrun
        mov     eax,[xrs_upfirst]
        call    xv_cluschk
        jc      xrs_bad
        call    xv_upload
        popad
        clc
        ret
xrs_bad:
        mov     byte ptr [xv_why],0
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_lbltake - ESI = a label entry -> xv_label and xv_lcount.  Preserves
;              all.
;---------------------------------------------------------------------
xv_lbltake:
        pushad
        movzx   ecx,byte ptr [esi+XFE_LCOUNT]
        cmp     ecx,11
        jbe     xlt_cnt
        mov     ecx,11
xlt_cnt:
        mov     [xv_lcount],cl
        add     esi,XFE_CHARS
        mov     edi,OFFSET xv_label
        mov     ecx,11
        rep     movsw
        popad
        ret

;---------------------------------------------------------------------
; xv_lblscan - the label read afresh from the root.  LABEL writes the
;              root's sectors itself, and an absolute write only drops
;              the caches, so the copy made at mount can be out of date.
;              No label entry, or a root that cannot be read: no label.
;---------------------------------------------------------------------
xv_lblscan:
        pushad
        mov     byte ptr [xv_lcount],0
        call    ctx_root
        xor     ebx,ebx
xlsc_lp:
        call    dir_rawseek
        jc      xlsc_done
        mov     al,[esi]
        or      al,al
        jz      xlsc_done
        cmp     al,XFT_LABEL
        jne     xlsc_next
        call    xv_lbltake
        jmp     xlsc_done
xlsc_next:
        inc     ebx
        cmp     ebx,10000h
        jb      xlsc_lp
xlsc_done:
        popad
        ret

;---------------------------------------------------------------------
; xv_cluschk - CF unless EAX is a cluster in the heap.  Preserves all.
;---------------------------------------------------------------------
xv_cluschk:
        cmp     eax,2
        jb      xcc_no
        push    eax
        dec     eax
        cmp     eax,[xv_nclus]
        pop     eax
        ja      xcc_no
        clc
        ret
xcc_no:
        stc
        ret

;---------------------------------------------------------------------
; xv_bmrun - is the bitmap one run of clusters?  A formatter always
;            makes it one, and then a bit's sector is arithmetic; if not
;            the FAT is walked for it (xv_bmunit).  Sets xv_bmcont.
;---------------------------------------------------------------------
xv_bmrun:
        pushad
        mov     byte ptr [xv_bmcont],0
        mov     eax,[xv_bmlen]          ; clusters it takes
        mov     cl,[xv_pshift]
        add     cl,9
        mov     edx,1
        shl     edx,cl
        dec     edx
        add     eax,edx
        shr     eax,cl
        mov     ecx,eax
        mov     eax,[xv_bmfirst]
xbr_lp:
        dec     ecx
        jz      xbr_one
        mov     ebx,eax
        call    xv_fatget
        jc      xbr_out
        call    xv_cluschk
        jc      xbr_out
        inc     ebx
        cmp     eax,ebx
        jne     xbr_out
        jmp     xbr_lp
xbr_one:
        mov     byte ptr [xv_bmcont],1
xbr_out:
        popad
        ret

;---------------------------------------------------------------------
; xv_upload - the first 256 characters of the volume's up-case table,
;             which is every character a kernel name can hold.  The
;             table's checksum is taken over all of it; a table that
;             does not check is not used, and a-z alone are folded.
;
; THE TABLE IS COMPRESSED: FFFFh followed by a count means that many
; characters map to themselves.  [xrs_upfirst] / [xrs_uplen] / [xrs_upsum]
; are what the root's entry said.
;---------------------------------------------------------------------
xv_upload:
        pushad
        call    xv_updefault
        mov     ecx,[xrs_uplen]
        or      ecx,ecx
        jz      xul_out
        cmp     ecx,1 SHL 20            ; 64K characters, uncompressed, is
        ja      xul_out                 ; 128K: a megabyte is nonsense
        mov     [xul_left],ecx
        mov     eax,[xrs_upfirst]
        call    xv_virt
        mov     [xul_clus],eax
        xor     edx,edx                 ; EDX = the checksum
        mov     dword ptr [xul_ch],0    ; the character the next word maps
        mov     byte ptr [xul_cnt],0    ; a count is due next
        mov     byte ptr [xul_odd],0    ; half a word is in hand
        mov     edi,OFFSET xul_tab
        call    xv_updefault2           ; built in xul_tab, kept if it checks
xul_clus_lp:
        mov     eax,[xul_clus]
        cmp     eax,2
        jb      xul_out
        cmp     eax,[fat_eoc]
        jae     xul_out
        mov     edi,OFFSET clus_buf
        call    fat_readclus
        jc      xul_out
        mov     dword ptr [clusbuf_clus],-1
        mov     ecx,[fat_spc]
        shl     ecx,9
        cmp     ecx,[xul_left]
        jbe     xul_take
        mov     ecx,[xul_left]
xul_take:
        sub     [xul_left],ecx
        mov     esi,OFFSET clus_buf
xul_byte:
        movzx   ebx,byte ptr [esi]
        ror     edx,1
        add     edx,ebx
        cmp     byte ptr [xul_odd],0
        jne     xul_hi
        mov     [xul_lo],bl
        mov     byte ptr [xul_odd],1
        jmp     xul_bnext
xul_hi:
        mov     byte ptr [xul_odd],0
        mov     bh,bl
        mov     bl,[xul_lo]             ; BX = the word
        call    xul_word
xul_bnext:
        inc     esi
        dec     ecx
        jnz     xul_byte
        cmp     dword ptr [xul_left],0
        je      xul_done
        mov     eax,[xul_clus]
        call    fat_next
        mov     [xul_clus],eax
        jmp     xul_clus_lp
xul_done:
        cmp     edx,[xrs_upsum]
        jne     xul_out
        mov     esi,OFFSET xul_tab      ; it checks: it is the table
        mov     edi,OFFSET xv_upcase
        mov     ecx,256 / 2
        rep     movsd
xul_out:
        popad
        ret

; xul_word - BX = the next word of the table
xul_word:
        push    eax
        cmp     byte ptr [xul_cnt],0
        je      xuw_notcnt
        mov     byte ptr [xul_cnt],0
        movzx   eax,bx
        add     [xul_ch],eax            ; that many map to themselves, which
        jmp     xuw_out           ; xul_tab already says
xuw_notcnt:
        cmp     bx,0FFFFh
        jne     xuw_map
        mov     byte ptr [xul_cnt],1
        jmp     xuw_out
xuw_map:
        mov     eax,[xul_ch]
        cmp     eax,256
        jae     xuw_past
        mov     word ptr [xul_tab+eax*2],bx
xuw_past:
        inc     dword ptr [xul_ch]
xuw_out:
        pop     eax
        ret

; xv_updefault - a-z to A-Z and every other character itself, in
;                xv_upcase; xv_updefault2 the same into xul_tab
xv_updefault:
        push    edi
        mov     edi,OFFSET xv_upcase
        call    xv_updefault2
        pop     edi
        ret
xv_updefault2:
        push    eax
        push    edi
        xor     eax,eax
xud_lp:
        stosw
        inc     eax
        cmp     eax,256
        jb      xud_lp
        pop     edi
        push    edi
        mov     eax,'A'
        add     edi,'a' * 2
xud_az:
        stosw
        inc     eax
        cmp     eax,'Z'
        jbe     xud_az
        pop     edi
        pop     eax
        ret

;---------------------------------------------------------------------
; xv_vfmount - the clean-shutdown flag (VOLFLAG.INC) is VolumeDirty in
;              the boot sector, for a volume this kernel reads itself
;              from a partition and may write.  Preserves all.
;---------------------------------------------------------------------
xv_vfmount:
        pushad
        mov     byte ptr [vol_vf],0
        cmp     byte ptr [xv_ro],0
        jne     xvm_out
        mov     al,[cur_vol]
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_FLOPPY OR DRVF_CDROM OR DRVF_DRIVER OR DRVF_IMAGE OR DRVF_USB
        jnz     xvm_out
        mov     byte ptr [vol_vf],1
        mov     edi,OFFSET xv_bsave
        call    vfl_found
xvm_out:
        popad
        ret

;---------------------------------------------------------------------
; xv_release - fat_mount, before it mounts anything: a volume that was
;              exFAT gives its directory table back.  Preserves all.
;---------------------------------------------------------------------
xv_release:
        cmp     byte ptr [vol_fs],FS_EXFAT
        jne     xrl_ret
        pushad
        mov     edx,[xv_regp]
        or      edx,edx
        jz      xrl_out
        call    arena_free
xrl_out:
        mov     dword ptr [xv_regp],0
        mov     dword ptr [xv_regn],0
        mov     dword ptr [xv_regcap],0
        popad
xrl_ret:
        ret

;---------------------------------------------------------------------
; xv_forget - vol_forget on an exFAT volume: the bitmap sector, the free
;             count and the directory table go, unwritten.  Preserves
;             all.
;---------------------------------------------------------------------
xv_forget:
        mov     dword ptr [xv_bmlba],-1
        mov     byte ptr [xv_bmdirty],0
        mov     dword ptr [xv_free],-1
        mov     dword ptr [xv_bmci],0
        mov     dword ptr [xv_regn],0
        jmp     xv_tclear

;=====================================================================
; CLUSTERS
;
; A KERNEL cluster is what everything outside this file counts: fat_spc
; sectors, at most 64.  A PHYSICAL cluster is the volume's.  With
; xv_kshift = 0 they are the same; otherwise one physical cluster is
; 1 SHL xv_kshift kernel clusters, numbered in a row.
;=====================================================================

; xv_virt - EAX = a physical cluster -> its first kernel cluster
xv_virt:
        push    ecx
        mov     cl,[xv_kshift]
        sub     eax,2
        shl     eax,cl
        add     eax,2
        pop     ecx
        ret

; xv_phys - EAX = a kernel cluster -> the physical cluster it is in
xv_phys:
        push    ecx
        mov     cl,[xv_kshift]
        sub     eax,2
        shr     eax,cl
        add     eax,2
        pop     ecx
        ret

; xv_islast - CF clear if kernel cluster EAX is the last of its physical
;             one.  Preserves all.
xv_islast:
        push    eax
        push    ecx
        mov     cl,[xv_kshift]
        sub     eax,1                   ; (EAX - 2) + 1
        push    edx
        mov     edx,1
        shl     edx,cl
        dec     edx
        test    eax,edx
        pop     edx
        pop     ecx
        pop     eax
        jnz     xil_no
        clc
        ret
xil_no:
        stc
        ret

; xv_lastof - EAX = a kernel cluster -> the last kernel cluster of the
;             physical cluster it is in
xv_lastof:
        push    ecx
        push    edx
        mov     cl,[xv_kshift]
        mov     edx,1
        shl     edx,cl
        dec     edx
        sub     eax,2
        or      eax,edx
        add     eax,2
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; xv_fatget - EAX = a physical cluster -> EAX = its FAT entry, as the
;             disk has it.  CF on a read that failed.  Preserves all
;             else.
; xv_fatput - EAX = a physical cluster, EDX = the entry to give it.
;---------------------------------------------------------------------
xv_fatget:
        push    ebx
        mov     ebx,eax
        shl     eax,2
        call    fat_secload
        jc      xfg_out
        and     ebx,127
        mov     eax,dword ptr [fatsec_buf+ebx*4]
xfg_out:
        pop     ebx
        ret

xv_fatput:
        push    eax
        push    ebx
        mov     ebx,eax
        shl     eax,2
        call    fat_secload
        jc      xfp_out
        and     ebx,127
        mov     dword ptr [fatsec_buf+ebx*4],edx
        mov     byte ptr [fatsec_dirty],1
xfp_out:
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; xv_next - fat_next on exFAT.  EAX = a kernel cluster -> the next one,
;           or [fat_eocv] where the chain ends.
;---------------------------------------------------------------------
xv_next:
        push    ecx
        push    edx
        push    esi
        cmp     eax,2
        jb      xnx_end
        mov     edx,[fat_totclus]
        add     edx,2
        cmp     eax,edx
        jae     xnx_end
        call    xv_runof                ; one of the runs it has been told of?
        jc      xnx_chain
        inc     eax
        cmp     eax,edx
        jb      xnx_out
        jmp     xnx_end
xnx_chain:
        call    xv_islast               ; the middle of a physical cluster:
        jnc     xnx_fat                 ; the next one is in it too
        inc     eax
        jmp     xnx_out
xnx_fat:
        call    xv_phys
        call    xv_fatget
        jc      xnx_end
        call    xv_cluschk              ; FFFFFFFFh, FFFFFFF7h, nonsense
        jc      xnx_end
        call    xv_virt
        jmp     xnx_out
xnx_end:
        mov     eax,[fat_eocv]
xnx_out:
        pop     esi
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; xv_setnext - fat_setnext on exFAT.  EAX = a kernel cluster, EDX = the
;              one after it, or [fat_eocv] to end the chain there.
;
; IN A RUN, three things are allowed to stay a run: the chain ending
; (the run is cut after this physical cluster), the next one being the
; one after the end (a fresh cluster the run grows into), and the next
; one being the next one anyway.  Anything else and the run becomes a
; FAT chain first, every entry of it, and is forgotten.
;---------------------------------------------------------------------
xv_setnext:
        pushad
        mov     ebp,edx                 ; EBP = what follows
        cmp     eax,2
        jb      xsn_out
        mov     ecx,[fat_totclus]
        add     ecx,2
        cmp     eax,ecx
        jae     xsn_out
        call    xv_runof                ; ESI -> FIRST/RUN, EDX = the end
        jc      xsn_fat
        cmp     ebp,[fat_eocv]
        jne     xsn_link
        push    eax                     ; the chain ends here: so does the
        call    xv_lastof               ; run, at this physical cluster's
        inc     eax                     ; end
        cmp     eax,edx
        jae     xsn_cutnone
        mov     ecx,eax
        mov     eax,[esi+XFR_FIRST]
        sub     ecx,eax
        call    xv_runset
xsn_cutnone:
        pop     eax
        jmp     xsn_out
xsn_link:
        lea     ecx,[eax+1]
        cmp     ebp,ecx
        jne     xsn_conv
        cmp     ecx,edx
        jb      xsn_out                 ; the next one anyway
        ; the cluster after the end: the run takes a physical cluster more
        mov     ecx,1
        push    eax
        mov     cl,[xv_kshift]
        mov     eax,1
        shl     eax,cl
        mov     ecx,eax
        mov     eax,[esi+XFR_FIRST]
        add     ecx,edx
        sub     ecx,eax
        call    xv_runset
        pop     eax
        jmp     xsn_out
xsn_conv:
        push    eax
        mov     eax,[esi+XFR_FIRST]
        call    xv_runconv
        pop     eax
xsn_fat:
        ; a FAT entry, then - unless both are in one physical cluster
        call    xv_islast
        jnc     xsn_put
        lea     ecx,[eax+1]
        cmp     ebp,ecx
        je      xsn_out
xsn_put:
        call    xv_phys
        mov     edx,XF_EOC
        cmp     ebp,[fat_eocv]
        je      xsn_val
        push    eax
        mov     eax,ebp
        call    xv_phys
        mov     edx,eax
        pop     eax
xsn_val:
        call    xv_fatput
xsn_out:
        popad
        ret

;---------------------------------------------------------------------
; xv_runconv - the run starting at kernel cluster EAX (ESI -> its
;              FIRST/RUN) is written into the FAT as the chain it always
;              was, and is a run no longer.  Preserves all.
;---------------------------------------------------------------------
xv_runconv:
        pushad
        mov     ecx,[esi+XFR_RUN]
        or      ecx,ecx
        jz      xrc_out
        lea     ebx,[eax+ecx-1]         ; its last kernel cluster
        push    eax
        mov     eax,ebx
        call    xv_phys
        mov     ebx,eax                 ; EBX = its last physical cluster
        pop     eax
        call    xv_phys                 ; EAX = its first
xrc_lp:
        lea     edx,[eax+1]
        cmp     eax,ebx
        jb      xrc_put
        mov     edx,XF_EOC
xrc_put:
        call    xv_fatput
        inc     eax
        cmp     eax,ebx
        jbe     xrc_lp
        popad
        push    ecx
        xor     ecx,ecx
        call    xv_runset
        pop     ecx
        ret
xrc_out:
        popad
        ret

;---------------------------------------------------------------------
; xv_alloc - fat_alloc on exFAT: a free physical cluster out of the
;            bitmap, marked used, its FAT entry the end of a chain.
;            EAX = its first kernel cluster.  CF if the volume is full or
;            is not to be written.
;---------------------------------------------------------------------
xv_alloc:
        push    ebx
        push    ecx
        push    edx
        push    esi
        cmp     byte ptr [xv_ro],0
        jne     xal_full
        mov     ebx,[xv_nclus]
        inc     ebx                     ; EBX = the last cluster there is
        ; A RUN IN HAND GROWS BEST INTO THE CLUSTER AFTER IT, and then it
        ; is still a run and nothing goes into the FAT; anywhere else
        ; and the whole run has to be written out as a chain
        mov     eax,[xv_ahint]
        cmp     dword ptr [xv_trun],0
        je      xal_hint
        mov     eax,[xv_tfirst]
        add     eax,[xv_trun]
        call    xv_phys
xal_hint:
        cmp     eax,2
        jb      xal_clamp
        cmp     eax,ebx
        jbe     xal_go
xal_clamp:
        mov     eax,2
xal_go:
        mov     edx,eax                 ; EDX = where the look started
xal_lp:
        call    xv_bmptr                ; ESI -> its byte, CL = its bit
        jc      xal_full
        mov     ch,1
        shl     ch,cl
        test    [esi],ch
        jz      xal_have
        ; A WHOLE BYTE IN USE is eight clusters stepped over at once -
        ; unless the place the look started is among them, which the
        ; test for having come all the way round must not miss
        or      cl,cl
        jnz     xal_one
        cmp     byte ptr [esi],0FFh
        jne     xal_one
        lea     ecx,[eax+8]
        cmp     ecx,ebx
        ja      xal_one
        cmp     eax,edx
        jae     xal_eight
        cmp     ecx,edx
        ja      xal_one
xal_eight:
        mov     eax,ecx
        jmp     xal_wrap
xal_one:
        inc     eax
xal_wrap:
        cmp     eax,ebx
        jbe     xal_more
        mov     eax,2
xal_more:
        cmp     eax,edx
        jne     xal_lp
xal_full:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret
xal_have:
        or      [esi],ch
        mov     byte ptr [xv_bmdirty],1
        cmp     dword ptr [xv_free],-1
        je      xal_nocount
        dec     dword ptr [xv_free]
xal_nocount:
        lea     ecx,[eax+1]
        mov     [xv_ahint],ecx
        mov     edx,XF_EOC
        call    xv_fatput
        call    xv_virt
        ; A RUN THAT CLAIMS A FREE CLUSTER IS ONE THAT IS OUT OF DATE -
        ; a directory deleted behind the table's back - and the cluster
        ; is about to be something else's
        push    eax
        call    xv_runof
        jc      xal_norun
        mov     eax,[esi+XFR_FIRST]
        call    xv_rundrop
xal_norun:
        pop     eax
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret

;---------------------------------------------------------------------
; xv_freechain - fat_freechain on exFAT: the chain or run from kernel
;                cluster EAX on goes back to the bitmap.  A start in the
;                middle of a physical cluster keeps that cluster, which
;                is still the file's.  Preserves all.
;---------------------------------------------------------------------
xv_freechain:
        pushad
        cmp     eax,2
        jb      xfc_out
        cmp     eax,[fat_eoc]
        jae     xfc_out
        mov     ebx,eax
        call    xv_phys
        call    xv_virt                 ; the start of its physical cluster
        cmp     eax,ebx
        je      xfc_go
        mov     eax,ebx                 ; not it: the physical cluster after
        call    xv_lastof
        call    xv_next
        cmp     eax,[fat_eoc]
        jae     xfc_out
xfc_go:
        mov     ebp,[xv_nclus]          ; a loop guard: no chain is longer
xfc_lp:
        call    xv_runof
        jc      xfc_chain
        ; A RUN: everything from here to its end, and the run is cut
        ; here or gone
        mov     ebx,eax
        mov     ecx,edx
        dec     ecx                     ; the last kernel cluster in it
        push    eax
        mov     eax,ecx
        call    xv_phys
        mov     ecx,eax                 ; ECX = the last physical one
        mov     eax,ebx
        call    xv_phys
xfc_rfree:
        call    xv_bmfree
        inc     eax
        cmp     eax,ecx
        jbe     xfc_rfree
        pop     eax
        mov     ecx,eax
        mov     eax,[esi+XFR_FIRST]
        cmp     ecx,eax
        jne     xfc_cut
        call    xv_rundrop
        jmp     xfc_out
xfc_cut:
        sub     ecx,eax
        call    xv_runset
        jmp     xfc_out
xfc_chain:
        call    xv_phys
        mov     ebx,eax
        call    xv_fatget
        jc      xfc_last
        xchg    eax,ebx
        call    xv_bmfree
        mov     eax,ebx
        call    xv_cluschk
        jc      xfc_out
        call    xv_virt
        dec     ebp
        jnz     xfc_lp
        jmp     xfc_out
xfc_last:
        mov     eax,ebx
        call    xv_bmfree
xfc_out:
        popad
        ret

;---------------------------------------------------------------------
; xv_runconvat - EAX = the first kernel cluster of a run the caller
;                knows of: it becomes a FAT chain.  Preserves all.
;---------------------------------------------------------------------
xv_runconvat:
        push    edx
        push    esi
        call    xv_runof
        jc      xrca_out
        cmp     eax,[esi+XFR_FIRST]
        jne     xrca_out
        call    xv_runconv
xrca_out:
        pop     esi
        pop     edx
        ret

;=====================================================================
; THE BITMAP - a bit a cluster, cluster 2 at bit 0, set when in use,
; read and written a 512-byte piece at a time through xv_bmbuf
;=====================================================================

;---------------------------------------------------------------------
; xv_bmptr - EAX = a physical cluster -> ESI -> the byte its bit is in,
;            ECX = the bit (0-7).  CF on a read that failed.  Preserves
;            all else.
;---------------------------------------------------------------------
xv_bmptr:
        push    eax
        push    ebx
        push    edx
        sub     eax,2
        mov     ecx,eax
        and     ecx,7
        shr     eax,3                   ; the byte
        mov     edx,eax
        and     edx,511
        shr     eax,9                   ; the 512-byte piece
        call    xv_bmunit               ; -> EBX = its LBA
        jc      xbp_fail
        cmp     ebx,[xv_bmlba]
        je      xbp_have
        call    xv_bmflush
        jc      xbp_fail
        push    ecx
        push    edi
        mov     al,[fat_drive]
        mov     cx,1
        mov     edi,OFFSET xv_bmbuf
        call    disk_read
        pop     edi
        pop     ecx
        jc      xbp_fail
        mov     [xv_bmlba],ebx
xbp_have:
        lea     esi,[xv_bmbuf+edx]
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret
xbp_fail:
        mov     dword ptr [xv_bmlba],-1
        pop     edx
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; xv_bmunit - EAX = a 512-byte piece of the bitmap -> EBX = the sector
;             it is in.  CF if the bitmap's chain does not reach it.
;             Preserves all else.
;---------------------------------------------------------------------
xv_bmunit:
        push    eax
        push    ecx
        push    edx
        mov     cl,[xv_pshift]
        mov     edx,1
        shl     edx,cl
        dec     edx
        and     edx,eax                 ; EDX = the piece in its cluster
        shr     eax,cl                  ; EAX = which cluster of the bitmap
        cmp     byte ptr [xv_bmcont],0
        je      xbu_walk
        add     eax,[xv_bmfirst]
        jmp     xbu_have
xbu_walk:
        cmp     eax,[xv_bmci]
        jae     xbu_fwd
        mov     dword ptr [xv_bmci],0
xbu_fwd:
        cmp     dword ptr [xv_bmci],0
        jne     xbu_step
        mov     ebx,[xv_bmfirst]
        mov     [xv_bmcp],ebx
xbu_step:
        cmp     eax,[xv_bmci]
        je      xbu_found
        push    eax
        mov     eax,[xv_bmcp]
        call    xv_fatget
        jc      xbu_bad
        call    xv_cluschk
        jc      xbu_bad
        mov     [xv_bmcp],eax
        pop     eax
        inc     dword ptr [xv_bmci]
        jmp     xbu_step
xbu_bad:
        pop     eax
        mov     dword ptr [xv_bmci],0
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret
xbu_found:
        mov     eax,[xv_bmcp]
xbu_have:
        sub     eax,2
        shl     eax,cl
        add     eax,edx
        add     eax,[fat_datalba]
        mov     ebx,eax
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret

;---------------------------------------------------------------------
; xv_bmflush - the bitmap sector in hand, written if it was changed.
;              CF on failure.  Preserves all.
;---------------------------------------------------------------------
xv_bmflush:
        cmp     byte ptr [xv_bmdirty],0
        je      xbf_ok
        cmp     dword ptr [xv_bmlba],-1
        je      xbf_ok
        push    eax
        push    ebx
        push    ecx
        push    edi
        mov     al,[fat_drive]
        mov     ebx,[xv_bmlba]
        mov     cx,1
        mov     edi,OFFSET xv_bmbuf
        call    disk_write
        pop     edi
        pop     ecx
        pop     ebx
        pop     eax
        jc      xbf_fail
        mov     byte ptr [xv_bmdirty],0
xbf_ok:
        clc
xbf_fail:
        ret

;---------------------------------------------------------------------
; xv_bmfree - physical cluster EAX is free.  Preserves all.
;---------------------------------------------------------------------
xv_bmfree:
        pushad
        call    xv_cluschk
        jc      xbm_out
        call    xv_bmptr
        jc      xbm_out
        mov     ch,1
        shl     ch,cl
        test    [esi],ch
        jz      xbm_out                 ; it was free already
        not     ch
        and     [esi],ch
        mov     byte ptr [xv_bmdirty],1
        cmp     dword ptr [xv_free],-1
        je      xbm_out
        inc     dword ptr [xv_free]
xbm_out:
        popad
        ret

;---------------------------------------------------------------------
; xv_freecount - fat_freecount on exFAT: EAX = the free kernel clusters.
;
; COUNTED ONCE, out of the bitmap, and then kept: an allocation and a
; free change it by one, and DIR asks after every listing.
;---------------------------------------------------------------------
xv_freecount:
        cmp     dword ptr [xv_free],-1
        jne     xfn_have
        pushad
        xor     ebp,ebp                 ; EBP = free so far
        mov     ebx,2                   ; EBX = the cluster
xfn_lp:
        mov     eax,[xv_nclus]
        add     eax,2
        cmp     ebx,eax
        jae     xfn_done
        mov     eax,ebx
        call    xv_bmptr
        jc      xfn_done
        or      cl,cl
        jnz     xfn_bit
        mov     ecx,[xv_nclus]
        add     ecx,2
        sub     ecx,ebx
        cmp     ecx,8
        jb      xfn_bit2
        ; a whole byte at once
        movzx   eax,byte ptr [esi]
        not     al
        mov     ecx,eax
        and     ecx,0Fh
        movzx   ecx,byte ptr [xfn_nib+ecx]
        add     ebp,ecx
        shr     eax,4
        movzx   ecx,byte ptr [xfn_nib+eax]
        add     ebp,ecx
        add     ebx,8
        jmp     xfn_lp
xfn_bit2:
        xor     ecx,ecx
xfn_bit:
        mov     ch,1
        shl     ch,cl
        test    [esi],ch
        jnz     xfn_used
        inc     ebp
xfn_used:
        inc     ebx
        jmp     xfn_lp
xfn_done:
        mov     [xv_free],ebp
        popad
xfn_have:
        push    ecx
        mov     eax,[xv_free]
        mov     cl,[xv_kshift]
        shl     eax,cl
        pop     ecx
        ret

xfn_nib     db  0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4

;=====================================================================
; THE RUNS
;
; The table of directories (xv_reg*, an arena block per volume, sorted
; by first cluster) and the one run in hand, xv_tfirst/xv_trun, laid out
; as a record's first two fields so that both are reached the same way.
;=====================================================================

; xv_tclear - nothing in hand
xv_tclear:
        mov     dword ptr [xv_tfirst],0
        mov     dword ptr [xv_trun],0
        ret

;---------------------------------------------------------------------
; xv_runof - EAX = a kernel cluster.  CF clear if it is in a run the
;            kernel knows of: ESI -> that run's FIRST/RUN, EDX = the
;            first cluster past it.  Preserves all else.
;---------------------------------------------------------------------
xv_runof:
        mov     edx,[xv_trun]
        or      edx,edx
        jz      xro_table
        add     edx,[xv_tfirst]
        cmp     eax,[xv_tfirst]
        jb      xro_table
        cmp     eax,edx
        jae     xro_table
        mov     esi,OFFSET xv_tfirst
        clc
        ret
xro_table:
        push    ecx
        call    xv_regbelow             ; ECX = records with FIRST <= EAX
        jecxz   xro_none
        dec     ecx
        shl     ecx,4
        mov     esi,[xv_regp]
        add     esi,ecx
        mov     edx,[esi+XFR_RUN]
        or      edx,edx
        jz      xro_none
        add     edx,[esi+XFR_FIRST]
        cmp     eax,edx
        jae     xro_none
        pop     ecx
        clc
        ret
xro_none:
        pop     ecx
        stc
        ret

;---------------------------------------------------------------------
; xv_regbelow - ECX = how many records have FIRST <= EAX (an upper
;               bound, by halves).  Preserves all else.
; xv_regfind  - EAX = a first cluster -> ESI -> its record, ECX = its
;               index; CF if there is none, with ECX = where it would go.
;---------------------------------------------------------------------
xv_regbelow:
        push    ebx
        push    edx
        push    esi
        xor     ebx,ebx                 ; lo
        mov     edx,[xv_regn]           ; hi
        mov     esi,[xv_regp]
xrb_lp:
        cmp     ebx,edx
        jae     xrb_done
        lea     ecx,[ebx+edx]
        shr     ecx,1
        push    ecx
        shl     ecx,4
        cmp     [esi+ecx+XFR_FIRST],eax
        pop     ecx
        ja      xrb_hi
        lea     ebx,[ecx+1]
        jmp     xrb_lp
xrb_hi:
        mov     edx,ecx
        jmp     xrb_lp
xrb_done:
        mov     ecx,ebx
        pop     esi
        pop     edx
        pop     ebx
        ret

xv_regfind:
        push    eax
        dec     eax                     ; FIRST <= EAX - 1: the ones below
        call    xv_regbelow
        pop     eax
        cmp     ecx,[xv_regn]
        jae     xrf_no
        mov     esi,ecx
        shl     esi,4
        add     esi,[xv_regp]
        cmp     [esi+XFR_FIRST],eax
        jne     xrf_no
        clc
        ret
xrf_no:
        stc
        ret

;---------------------------------------------------------------------
; xv_regput - a directory seen: EAX = its first kernel cluster, ECX =
;             its run (0 = a chain), EDX = the directory it is in, EBX =
;             the slot of its file entry there.  CF if there was no room
;             and no memory for more.  Preserves all.
;---------------------------------------------------------------------
xv_regput:
        pushad
        mov     [xrp_rec+XFR_FIRST],eax
        mov     [xrp_rec+XFR_RUN],ecx
        mov     [xrp_rec+XFR_PDIR],edx
        mov     [xrp_rec+XFR_PSLOT],ebx
        call    xv_regfind
        jnc     xrp_set
        ; a new one: room first
        mov     eax,[xv_regn]
        cmp     eax,[xv_regcap]
        jb      xrp_room
        push    ecx
        call    xv_reggrow
        pop     ecx
        jc      xrp_fail
xrp_room:
        ; open a gap at ECX, from the top down
        mov     esi,[xv_regp]
        mov     eax,[xv_regn]
        sub     eax,ecx                 ; records to move up
        shl     eax,4
        shl     ecx,4
        lea     esi,[esi+ecx]           ; ESI = where it goes
        push    esi
        lea     edi,[esi+eax+XFR_LEN-1]
        lea     esi,[esi+eax-1]
        mov     ecx,eax
        std
        rep     movsb
        cld
        pop     esi
        inc     dword ptr [xv_regn]
xrp_set:
        mov     edi,esi
        mov     esi,OFFSET xrp_rec
        mov     ecx,XFR_LEN / 4
        rep     movsd
        popad
        clc
        ret
xrp_fail:
        popad
        stc
        ret

; xv_reggrow - the table, twice the size (or XFR_GROW records), out of
;              the arena and the kernel's own.  CF if there is no memory.
xv_reggrow:
        pushad
        mov     ebx,[xv_regcap]
        shl     ebx,1
        jnz     xrg_size
        mov     ebx,XFR_GROW
xrg_size:
        mov     [xrg_cap],ebx
        shl     ebx,4
        call    arena_alloc
        jc      xrg_fail
        mov     dword ptr [eax-MCB_LEN+MCB_OWNER],0     ; the kernel's, so no
        mov     edi,eax                                 ; program's end frees it
        mov     esi,[xv_regp]
        mov     ecx,[xv_regn]
        shl     ecx,2
        jecxz   xrg_copied
        rep     movsd
xrg_copied:
        mov     edx,[xv_regp]
        mov     [xv_regp],eax
        mov     eax,[xrg_cap]
        mov     [xv_regcap],eax
        or      edx,edx
        jz      xrg_ok
        call    arena_free
xrg_ok:
        popad
        clc
        ret
xrg_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_runset - the run starting at EAX is now ECX long (0 = a chain now),
;             in the hand and in the table, wherever it is.  Preserves
;             all.
; xv_rundrop - the run starting at EAX is gone: out of the hand, and its
;              record out of the table.  Preserves all.
;---------------------------------------------------------------------
xv_runset:
        push    esi
        cmp     eax,[xv_tfirst]
        jne     xrs2_table
        mov     [xv_trun],ecx
xrs2_table:
        push    ecx
        call    xv_regfind
        pop     ecx
        jc      xrs2_out
        mov     [esi+XFR_RUN],ecx
xrs2_out:
        pop     esi
        ret

xv_rundrop:
        pushad
        cmp     eax,[xv_tfirst]
        jne     xrd_table
        call    xv_tclear
xrd_table:
        call    xv_regfind
        jc      xrd_out
        mov     edi,esi                 ; close the gap
        lea     esi,[esi+XFR_LEN]
        mov     edx,[xv_regn]
        dec     edx
        mov     [xv_regn],edx
        sub     edx,ecx                 ; records after it
        shl     edx,2
        mov     ecx,edx
        rep     movsd
xrd_out:
        popad
        ret

;=====================================================================
; THE SFT AND THE RUN IN HAND
;=====================================================================

;---------------------------------------------------------------------
; xv_sftrun - ESI = an open file about to be walked: its run is the one
;             in hand.  xv_sftback - and afterwards, what the walk made
;             of it goes back.  Both preserve all, and do nothing on any
;             other filesystem.
;---------------------------------------------------------------------
xv_sftrun:
        cmp     byte ptr [vol_fs],FS_EXFAT
        jne     xsr_ret
        push    eax
        mov     eax,[esi+SFT_CLUSTER]
        mov     [xv_tfirst],eax
        mov     eax,[esi+SFT_XRUN]
        mov     [xv_trun],eax
        pop     eax
xsr_ret:
        ret

xv_sftback:
        cmp     byte ptr [vol_fs],FS_EXFAT
        jne     xsb_ret
        push    eax
        mov     eax,[esi+SFT_CLUSTER]
        cmp     eax,[xv_tfirst]
        jne     xsb_none                ; a file given its first cluster just
        mov     eax,[xv_trun]           ; now has no run
        mov     [esi+SFT_XRUN],eax
        jmp     xsb_out
xsb_none:
        mov     dword ptr [esi+SFT_XRUN],0
xsb_out:
        pop     eax
xsb_ret:
        ret

;---------------------------------------------------------------------
; xv_sftbind - open_core has a table entry at ESI for the file fat_search
;              just found: its run and its ValidDataLength.  CF if it
;              may not be opened as asked - a file of 4GB or more for
;              writing, which a 32-bit size cannot describe back.
;---------------------------------------------------------------------
xv_sftbind:
        cmp     byte ptr [vol_fs],FS_EXFAT
        jne     xsbd_ok
        cmp     byte ptr [xv_ekind],XVK_SET
        jne     xsbd_ok
        push    eax
        mov     eax,[xv_erun]
        mov     [esi+SFT_XRUN],eax
        mov     eax,[xv_evdl]
        mov     [esi+SFT_XVDL],eax
        pop     eax
        cmp     byte ptr [xv_ebig],0
        je      xsbd_ok
        cmp     dword ptr [esi+SFT_MODE],0
        je      xsbd_ok
        stc
        ret
xsbd_ok:
        clc
        ret

;---------------------------------------------------------------------
; xv_wprep - ESI = an open exFAT file about to be written or cut: the
;            bytes between its ValidDataLength and its end are made
;            zeroes on the disk, which they read as already, and then it
;            is all valid.  CF on a failed write.  Preserves all.
;---------------------------------------------------------------------
xv_wprep:
        cmp     byte ptr [vol_fs],FS_EXFAT
        jne     xwp_ok
        push    eax
        mov     eax,[esi+SFT_XVDL]
        cmp     eax,[esi+SFT_SIZE]
        pop     eax
        jae     xwp_ok
        pushad
        call    xv_sftrun
        mov     ebx,[fat_spc]
        shl     ebx,9                   ; EBX = a cluster's bytes
        mov     eax,[esi+SFT_XVDL]
        xor     edx,edx
        div     ebx                     ; EAX = its cluster, EDX = where in it
        mov     ecx,eax
        mov     eax,[esi+SFT_CLUSTER]
xwp_walk:
        jecxz   xwp_there
        call    fat_next
        dec     ecx
        jmp     xwp_walk
xwp_there:
        mov     ecx,[esi+SFT_SIZE]
        sub     ecx,[esi+SFT_XVDL]      ; ECX = bytes to zero
xwp_clus:
        cmp     eax,2
        jb      xwp_done
        cmp     eax,[fat_eoc]
        jae     xwp_done
        push    ecx
        push    eax
        mov     edi,OFFSET clus_buf
        or      edx,edx
        jz      xwp_whole
        call    fat_readclus            ; a cluster partly valid
        jc      xwp_fail2
        jmp     xwp_zero
xwp_whole:
        push    eax
        xor     eax,eax
        mov     ecx,CLUSBUF_SIZE / 4
        rep     stosd
        pop     eax
xwp_zero:
        mov     dword ptr [clusbuf_clus],-1
        mov     edi,OFFSET clus_buf
        add     edi,edx
        mov     ecx,ebx
        sub     ecx,edx
        push    eax
        xor     eax,eax
        rep     stosb
        pop     eax
        mov     edi,OFFSET clus_buf
        call    fat_writeclus
        jc      xwp_fail2
        pop     eax
        pop     ecx
        mov     edi,ebx
        sub     edi,edx                 ; bytes this cluster zeroed
        xor     edx,edx
        sub     ecx,edi
        jbe     xwp_done
        call    fat_next
        jmp     xwp_clus
xwp_done:
        mov     eax,[esi+SFT_SIZE]
        mov     [esi+SFT_XVDL],eax
        or      dword ptr [esi+SFT_FLAGS],SFTF_DIRTY
        popad
xwp_ok:
        clc
        ret
xwp_fail2:
        pop     eax
        pop     ecx
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_wpost - after a write or a cut: all of the file is valid.
;            Preserves all.
;---------------------------------------------------------------------
xv_wpost:
        cmp     byte ptr [vol_fs],FS_EXFAT
        jne     xwq_ret
        push    eax
        mov     eax,[esi+SFT_SIZE]
        mov     [esi+SFT_XVDL],eax
        pop     eax
xwq_ret:
        ret

;---------------------------------------------------------------------
; xv_vdlclip - file_chunk has just copied EAX bytes from file offset
;              ECX to EDI: what lies past the file's ValidDataLength is
;              made zeroes.  ESI = the file.  Preserves all.
;---------------------------------------------------------------------
xv_vdlclip:
        cmp     byte ptr [vol_fs],FS_EXFAT
        jne     xvc_ret
        pushad
        mov     edx,[esi+SFT_XVDL]
        lea     ebx,[ecx+eax]           ; EBX = the end of what was copied
        cmp     ebx,edx
        jbe     xvc_out                 ; all of it valid
        sub     edx,ecx                 ; EDX = valid bytes at the front
        ja      xvc_some
        xor     edx,edx
xvc_some:
        add     edi,edx
        mov     ecx,eax
        sub     ecx,edx
        xor     eax,eax
        rep     stosb
xvc_out:
        popad
xvc_ret:
        ret

;---------------------------------------------------------------------
; xv_sftclose - sft_close is about to write the entry of the file at
;               ESI back: what the entry cannot carry goes with it.
;               Preserves all.
;---------------------------------------------------------------------
xv_sftclose:
        cmp     byte ptr [vol_fs],FS_EXFAT
        jne     xsc_ret
        push    eax
        mov     eax,[esi+SFT_XVDL]
        mov     [xv_psvdl],eax
        mov     eax,[esi+SFT_XRUN]
        mov     [xv_psrun],eax
        mov     byte ptr [xv_pson],1
        pop     eax
xsc_ret:
        ret

;---------------------------------------------------------------------
; xv_newop - an INT 21h call is starting: nothing a previous one left
;            pending carries over into it.  Preserves all.
;---------------------------------------------------------------------
xv_newop:
        mov     byte ptr [xv_nwon],0
        mov     byte ptr [xv_pnon],0
        mov     byte ptr [xv_pson],0
        mov     byte ptr [xv_hon],0
        ret

;---------------------------------------------------------------------
; xv_hold - 56h, as it reads the entry it is renaming: what the new
;           entry will need that the 32 bytes it copies cannot carry.
;           Preserves all.
;---------------------------------------------------------------------
xv_hold:
        cmp     byte ptr [vol_fs],FS_EXFAT
        jne     xho_ret
        cmp     byte ptr [xv_ekind],XVK_SET
        jne     xho_ret
        pushad
        mov     esi,OFFSET xv_efirst
        mov     edi,OFFSET xv_hfirst
        mov     ecx,XV_HOLDLEN
        rep     movsb
        mov     al,[xv_eflags]
        mov     [xv_hflags],al
        mov     byte ptr [xv_hon],1
        popad
xho_ret:
        ret

;=====================================================================
; DIRECTORIES
;=====================================================================

;---------------------------------------------------------------------
; xv_seek - dir_seek on exFAT.  EBX = the index -> ESI -> a 32-byte FAT
;           entry describing it, in xv_ent.  CF past the end, or on a
;           read that failed.  EBX and everything else preserved.
;---------------------------------------------------------------------
xv_seek:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    edi
        mov     eax,[dc_start]
        mov     [xv_edir],eax
        mov     byte ptr [iso_lname],0
        cmp     eax,[fat_rootclus]
        je      xsk_slot
        cmp     ebx,2                   ; a subdirectory: "." and ".." first
        jb      xsk_dot
        sub     ebx,2
xsk_slot:
        mov     [xv_eslot],ebx
        ; slots reserved for a set not yet written answer as free ones,
        ; for the caller to fill in
        cmp     byte ptr [xv_nwon],0
        je      xsk_raw
        cmp     ebx,[xv_nwslot]
        jne     xsk_raw
        cmp     eax,[xv_nwdir]
        jne     xsk_raw
        call    xv_blank
        mov     byte ptr [xv_ent],0E5h
        mov     byte ptr [xv_ekind],XVK_NEW
        jmp     xsk_out
xsk_raw:
        cmp     ebx,XV_DIRMAX           ; as big as a directory can be
        jae     xsk_fail
        mov     esi,OFFSET sec_buf      ; (the raw seek leaves it in there)
        call    dir_rawseek
        jc      xsk_fail
        call    xv_blank
        mov     al,[esi]
        or      al,al
        jz      xsk_end
        test    al,XFT_INUSE
        jz      xsk_free
        cmp     al,XFT_FILE
        je      xsk_file
        cmp     al,XFT_LABEL
        je      xsk_label
xsk_other:
        ; NOT A FILE, AND NOT FREE: a fragment of a long name to every
        ; walker, which steps over it - so a set's own entries, the
        ; bitmap and the up-case table are neither listed nor reused
        mov     byte ptr [xv_ent],1
        mov     byte ptr [xv_ent+DE_ATTR],ATTR_LFN
        mov     byte ptr [xv_ekind],XVK_OTHER
        jmp     xsk_out
xsk_end:
        mov     byte ptr [xv_ekind],XVK_END
        jmp     xsk_out
xsk_free:
        mov     byte ptr [xv_ent],0E5h
        mov     byte ptr [xv_ekind],XVK_FREE
        jmp     xsk_out
xsk_file:
        call    xv_getset               ; the whole set into xv_set
        jc      xsk_other
        call    xv_synth
        jc      xsk_other
        jmp     xsk_out
xsk_label:
        mov     eax,[dc_start]
        cmp     eax,[fat_rootclus]
        jne     xsk_other
        movzx   ecx,byte ptr [esi+XFE_LCOUNT]
        jecxz   xsk_other               ; an empty label is no label
        call    xv_lblsynth
        jmp     xsk_out
xsk_dot:
        call    xv_blank
        mov     edi,OFFSET xv_ent
        mov     ecx,11
        mov     al,' '
        rep     stosb
        mov     byte ptr [xv_ent],'.'
        mov     byte ptr [xv_ent+DE_ATTR],ATTR_DIR
        mov     byte ptr [xv_ekind],XVK_DOT
        mov     eax,[dc_start]          ; "." is this directory
        or      ebx,ebx
        jz      xsk_dotclus
        mov     byte ptr [xv_ent+1],'.' ; ".." is the one it is in, which
        call    xv_regfind              ; the table says; the root is 0
        mov     eax,0
        jc      xsk_dotclus
        mov     eax,[esi+XFR_PDIR]
        cmp     eax,[fat_rootclus]
        jne     xsk_dotclus
        xor     eax,eax
xsk_dotclus:
        mov     esi,OFFSET xv_ent
        call    de_setclus
xsk_out:
        mov     esi,OFFSET xv_ent
        mov     edi,OFFSET xv_eorig     ; as made, for xv_update to compare
        mov     ecx,8
        push    esi
        rep     movsd
        pop     esi
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
xsk_fail:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

; xv_blank - xv_ent all zero, and nothing known about it.  Preserves all.
xv_blank:
        push    eax
        push    ecx
        push    edi
        mov     edi,OFFSET xv_ent
        xor     eax,eax
        mov     ecx,8
        rep     stosd
        mov     edi,OFFSET xv_efirst
        mov     ecx,XV_HOLDLEN
        rep     stosb
        mov     byte ptr [xv_ebig],0
        mov     byte ptr [xv_eslots],1
        pop     edi
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; xv_getset - the file entry at slot EBX: the whole set into xv_set, and
;             checked - its order, its name entries, its checksum.  CF if
;             it is not a set to read.  Preserves all.
;---------------------------------------------------------------------
xv_getset:
        pushad
        mov     edi,OFFSET xv_set
        movsd                           ; the file entry, from ESI
        movsd
        movsd
        movsd
        movsd
        movsd
        movsd
        movsd
        movzx   ecx,byte ptr [xv_set+XFE_SECCNT]
        cmp     ecx,2
        jb      xgs_bad
        cmp     ecx,XV_SETMAX - 1
        ja      xgs_bad
        mov     [xgs_count],ecx
        mov     edx,ebx
xgs_lp:
        inc     edx
        push    ebx
        mov     ebx,edx
        call    dir_rawseek
        pop     ebx
        jc      xgs_bad
        mov     al,[esi]
        test    al,XFT_INUSE
        jz      xgs_bad
        test    al,XFT_SECONDARY
        jz      xgs_bad
        mov     ecx,8
        rep     movsd
        dec     dword ptr [xgs_count]
        jnz     xgs_lp

        ; a stream first, then the name's entries, every one of them
        cmp     byte ptr [xv_set+32],XFT_STREAM
        jne     xgs_bad
        movzx   eax,byte ptr [xv_set+32+XFE_NLEN]
        or      eax,eax
        jz      xgs_bad
        add     eax,14
        mov     ecx,15
        xor     edx,edx
        div     ecx                     ; EAX = name entries
        movzx   ecx,byte ptr [xv_set+XFE_SECCNT]
        dec     ecx
        cmp     eax,ecx
        ja      xgs_bad
        mov     esi,OFFSET xv_set + 64
        mov     ecx,eax
xgs_names:
        cmp     byte ptr [esi],XFT_NAME
        jne     xgs_bad
        add     esi,32
        loop    xgs_names
        ; ...and no critical secondary this does not know
        movzx   ecx,byte ptr [xv_set+XFE_SECCNT]
        sub     ecx,eax
        dec     ecx
        jz      xgs_sum
xgs_extra:
        test    byte ptr [esi],XFT_BENIGN
        jz      xgs_bad
        add     esi,32
        loop    xgs_extra
xgs_sum:
        call    xv_setsum
        cmp     ax,word ptr [xv_set+XFE_SETSUM]
        jne     xgs_bad
        popad
        clc
        ret
xgs_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_setsum - AX = the checksum of the set in xv_set, as long as its
;             file entry says.  Bytes 2 and 3 - the sum itself - are left
;             out.  Preserves all but EAX.
;---------------------------------------------------------------------
xv_setsum:
        push    ebx
        push    ecx
        push    esi
        movzx   ecx,byte ptr [xv_set+XFE_SECCNT]
        inc     ecx
        shl     ecx,5
        mov     esi,OFFSET xv_set
        xor     eax,eax
        xor     ebx,ebx
xss_lp:
        cmp     ebx,2
        je      xss_skip
        cmp     ebx,3
        je      xss_skip
        ror     ax,1
        add     al,[esi+ebx]
        adc     ah,0
xss_skip:
        inc     ebx
        cmp     ebx,ecx
        jb      xss_lp
        pop     esi
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; xv_synth - xv_set holds a good set from slot [xv_eslot]: make the FAT
;            entry of it in xv_ent, the long name in lfn_buf, and keep
;            what the entry cannot carry.  A directory goes in the table.
;            CF if its cluster is nonsense.  Preserves all.
;---------------------------------------------------------------------
xv_synth:
        pushad
        mov     byte ptr [xv_ekind],XVK_SET
        movzx   eax,byte ptr [xv_set+XFE_SECCNT]
        inc     eax
        mov     [xv_eslots],al
        mov     al,[xv_set+32+XFE_SFLAGS]
        mov     [xv_eflags],al

        ; the name, a byte a character, and its 8.3 alias
        call    xv_name2lfn             ; -> ECX = its length
        call    xv_alias

        ; attributes: FAT's own bits, in the same places
        mov     al,byte ptr [xv_set+XFE_ATTR]
        and     al,ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_DIR OR ATTR_ARCHIVE
        mov     [xv_ent+DE_ATTR],al

        ; the stamps: exFAT's are FAT's, time low and date high
        mov     eax,dword ptr [xv_set+XFE_MOD]
        mov     dword ptr [xv_ent+DE_TIME],eax
        mov     al,[xv_set+XFE_CRT10]
        mov     [xv_ent+13],al
        mov     eax,dword ptr [xv_set+XFE_CRT]
        mov     dword ptr [xv_ent+14],eax
        mov     ax,word ptr [xv_set+XFE_ACC+2]
        mov     word ptr [xv_ent+18],ax
        mov     eax,dword ptr [xv_set+XFE_CRT]
        mov     [xv_ecrt],eax
        mov     al,[xv_set+XFE_CRT10]
        mov     [xv_ecrt10],al

        ; the data
        mov     eax,dword ptr [xv_set+32+XFE_DLEN]
        mov     edx,dword ptr [xv_set+32+XFE_DLEN+4]
        mov     [xv_edlen],eax
        mov     [xv_edlen+4],edx
        mov     ecx,dword ptr [xv_set+32+XFE_VDL]
        cmp     dword ptr [xv_set+32+XFE_VDL+4],0
        je      xsy_vdl
        mov     ecx,0FFFFFFFFh
xsy_vdl:
        cmp     ecx,eax                 ; no further than the length, and a
        jbe     xsy_vdl2                ; length past 4G is all valid as far
        or      edx,edx                 ; as the kernel can see
        jnz     xsy_vdl2
        mov     ecx,eax
xsy_vdl2:
        mov     [xv_evdl],ecx
        or      edx,edx
        jz      xsy_small
        mov     byte ptr [xv_ebig],1    ; 4G and more: as much as 32 bits say,
        mov     eax,0FFFFFFFFh          ; and not to be written
        or      byte ptr [xv_ent+DE_ATTR],ATTR_READONLY
xsy_small:
        test    byte ptr [xv_ent+DE_ATTR],ATTR_DIR
        jnz     xsy_nosize
        mov     dword ptr [xv_ent+DE_SIZE],eax
xsy_nosize:

        ; the first cluster, as the kernel counts them
        mov     eax,dword ptr [xv_set+32+XFE_FIRST]
        or      eax,eax
        jz      xsy_noclus
        call    xv_cluschk
        jc      xsy_bad
        call    xv_virt
xsy_noclus:
        mov     [xv_efirst],eax
        mov     esi,OFFSET xv_ent
        call    de_setclus

        ; A RUN, if NoFatChain says so: as many physical clusters as the
        ; length needs, counted in kernel clusters
        mov     dword ptr [xv_erun],0
        or      eax,eax
        jz      xsy_norun
        test    byte ptr [xv_eflags],XFS_NOFAT
        jz      xsy_norun
        mov     eax,[xv_edlen]
        mov     edx,[xv_edlen+4]
        mov     cl,[xv_pshift]
        add     cl,9
        mov     ebx,1
        shl     ebx,cl
        dec     ebx
        add     eax,ebx
        adc     edx,0
        shrd    eax,edx,cl              ; physical clusters
        shr     edx,cl
        or      edx,edx
        jnz     xsy_bad
        or      eax,eax
        jz      xsy_norun
        mov     cl,[xv_kshift]
        shl     eax,cl
        mov     [xv_erun],eax
        add     eax,[xv_efirst]         ; it must be inside the heap
        jc      xsy_bad
        mov     edx,[fat_totclus]
        add     edx,2
        cmp     eax,edx
        ja      xsy_bad
xsy_norun:

        ; A DIRECTORY GOES IN THE TABLE: its run, and where its entry is,
        ; for whatever comes back to it with only its first cluster
        test    byte ptr [xv_ent+DE_ATTR],ATTR_DIR
        jz      xsy_out
        mov     eax,[xv_efirst]
        or      eax,eax
        jz      xsy_out
        mov     ecx,[xv_erun]
        mov     edx,[xv_edir]
        mov     ebx,[xv_eslot]
        call    xv_regput               ; no room: the walk in hand still
xsy_out:                                ; has the run (de_getclus)
        popad
        clc
        ret
xsy_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_name2lfn - the set's name into lfn_buf, a byte a character: a
;               character the kernel's names cannot hold becomes '_', as
;               a long name's does on FAT.  ECX = its length.  Preserves
;               all else.
;---------------------------------------------------------------------
xv_name2lfn:
        push    eax
        push    edx
        push    esi
        push    edi
        movzx   ecx,byte ptr [xv_set+32+XFE_NLEN]
        mov     edi,OFFSET lfn_buf
        mov     esi,OFFSET xv_set + 64 + XFE_CHARS
        xor     edx,edx                 ; EDX = characters taken from this entry
        mov     byte ptr [xnl_lossy],0
        push    ecx
xnl_lp:
        jecxz   xnl_done
        mov     ax,[esi]
        or      ah,ah
        jnz     xnl_wide
        or      al,al
        jnz     xnl_put
xnl_wide:
        mov     al,'_'
        mov     byte ptr [xnl_lossy],1
xnl_put:
        stosb
        add     esi,2
        inc     edx
        cmp     edx,15
        jb      xnl_same
        add     esi,2                   ; past the next entry's two bytes of
        xor     edx,edx                 ; type and flags
xnl_same:
        dec     ecx
        jmp     xnl_lp
xnl_done:
        mov     byte ptr [edi],0
        pop     ecx
        pop     edi
        pop     esi
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; xv_alias - lfn_buf holds a name of ECX characters: the 8.3 field of
;            xv_ent for it, and [iso_lname] set when the name is not
;            just that field spelt out.  Preserves all.
;
; A NAME THAT FITS 8.3 IS ITS OWN ALIAS, upper-cased - and still "long"
; when it was not upper case to begin with, so that a long-name listing
; shows it as it was written.  Any other gets the Windows basis, '~', and
; a number that is the slot's: no two sets share a slot, so no two
; aliases in a directory are the same, without a search for each.
;---------------------------------------------------------------------
xv_alias:
        pushad
        mov     edi,OFFSET xv_ent
        push    ecx
        mov     ecx,11
        mov     al,' '
        rep     stosb
        pop     ecx
        mov     byte ptr [iso_lname],0
        cmp     byte ptr [xnl_lossy],0  ; a character it could not spell:
        jne     xal2_long               ; long, whatever its shape

        ; does it fit?  1-8 then optionally a dot and 1-3; no second dot,
        ; no character 8.3 cannot hold
        mov     esi,OFFSET lfn_buf
        xor     ebx,ebx                 ; EBX = characters in this part
        mov     edx,8                   ; EDX = how many it may have
        mov     edi,OFFSET xv_ent
        xor     ebp,ebp                 ; EBP = 1 once past the dot
xal2_lp:
        jecxz   xal2_end
        mov     al,[esi]
        inc     esi
        dec     ecx
        cmp     al,'.'
        je      xal2_dot
        call    xv_83ok
        jc      xal2_long
        cmp     ebx,edx
        jae     xal2_long
        call    fsr_fold
        cmp     al,[esi-1]
        je      xal2_case
        mov     byte ptr [iso_lname],1  ; lower case in it: long as well
xal2_case:
        mov     [edi+ebx],al
        inc     ebx
        jmp     xal2_lp
xal2_dot:
        or      ebp,ebp
        jnz     xal2_long
        or      ebx,ebx
        jz      xal2_long               ; ".x", or "." itself
        jecxz   xal2_long               ; "x." - DOS would drop the dot
        inc     ebp
        mov     edi,OFFSET xv_ent + 8
        xor     ebx,ebx
        mov     edx,3
        jmp     xal2_lp
xal2_end:
        or      ebx,ebx
        jz      xal2_long
        popad
        ret

xal2_long:
        ; THE BASIS: letters and digits and what 8.3 can hold, up-cased,
        ; spaces and dots dropped, at most eight; the extension from after
        ; the last dot
        mov     byte ptr [iso_lname],1
        mov     edi,OFFSET xv_ent
        mov     ecx,11
        mov     al,' '
        push    edi
        rep     stosb
        pop     edi
        popad
        pushad
        mov     esi,OFFSET lfn_buf
        ; the last dot, if any (not a leading one)
        xor     edx,edx                 ; EDX -> after the last dot, 0 = none
        mov     ebx,esi
xal2_find:
        mov     al,[ebx]
        or      al,al
        jz      xal2_found
        cmp     al,'.'
        jne     xal2_fnext
        cmp     ebx,esi
        je      xal2_fnext
        lea     edx,[ebx+1]
xal2_fnext:
        inc     ebx
        jmp     xal2_find
xal2_found:
        mov     edi,OFFSET xal2_stem
        xor     ecx,ecx                 ; ECX = stem characters
xal2_stem1:
        mov     al,[esi]
        or      al,al
        jz      xal2_stemd
        cmp     esi,edx
        jne     xal2_notext
        jmp     xal2_stemd
xal2_notext:
        inc     esi
        cmp     al,' '
        jbe     xal2_stem1
        cmp     al,'.'
        je      xal2_stem1
        cmp     ecx,8
        jae     xal2_stem1
        call    xv_83map
        mov     [edi+ecx],al
        inc     ecx
        jmp     xal2_stem1
xal2_stemd:
        xor     esi,esi                 ; ESI -> the last dot, 0 = there is
        or      edx,edx                 ; no extension
        jz      xal2_noext
        lea     esi,[edx-1]
xal2_noext:
        cmp     ecx,1
        jae     xal2_hasstem
        mov     byte ptr [xal2_stem],'_'
        inc     ecx
xal2_hasstem:
        ; the number, and the room it leaves the stem
        mov     eax,[xv_eslot]
        inc     eax
        mov     ebx,10
        push    ecx
        mov     edi,OFFSET xal2_num + 8
        xor     ecx,ecx
xal2_digit:
        xor     edx,edx
        div     ebx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        inc     ecx
        or      eax,eax
        jnz     xal2_digit
        mov     ebx,ecx                 ; EBX = digits, at EDI
        pop     ecx
        mov     eax,7
        sub     eax,ebx                 ; the stem it can keep
        cmp     ecx,eax
        jbe     xal2_kept
        mov     ecx,eax
xal2_kept:
        push    edi
        push    esi
        mov     esi,OFFSET xal2_stem
        mov     edi,OFFSET xv_ent
        rep     movsb
        mov     byte ptr [edi],'~'
        inc     edi
        pop     esi
        pop     eax                     ; the digits
        push    esi
        mov     esi,eax
        mov     ecx,ebx
        rep     movsb
        pop     esi
        ; the extension: up to three after the last dot
        or      esi,esi
        jz      xal2_done
        cmp     byte ptr [esi],'.'
        jne     xal2_done
        inc     esi
        mov     edi,OFFSET xv_ent + 8
        xor     ecx,ecx
xal2_ext:
        mov     al,[esi]
        or      al,al
        jz      xal2_done
        inc     esi
        cmp     al,' '
        jbe     xal2_ext
        cmp     ecx,3
        jae     xal2_done
        call    xv_83map
        mov     [edi+ecx],al
        inc     ecx
        jmp     xal2_ext
xal2_done:
        popad
        ret

; xv_83ok - CF if AL cannot be in an 8.3 name as it stands
xv_83ok:
        cmp     al,' '
        jbe     x83_no
        push    edi
        push    ecx
        mov     edi,OFFSET x83_bad
        mov     ecx,X83_NBAD
        repne   scasb
        pop     ecx
        pop     edi
        je      x83_no
        clc
        ret
x83_no:
        stc
        ret
; xv_83map - AL as it goes into an alias: up-cased, '_' for what 8.3
;            cannot hold
xv_83map:
        call    xv_83ok
        jnc     x83m_ok
        mov     al,'_'
        ret
x83m_ok:
        jmp     fsr_fold

x83_bad     db  '"*+,/:;<=>?[\]|'
X83_NBAD    EQU     $ - x83_bad

;---------------------------------------------------------------------
; xv_lblsynth - ESI -> the root's label entry: xv_ent is the volume
;               label's FAT entry.  Preserves all.
;---------------------------------------------------------------------
xv_lblsynth:
        pushad
        movzx   ecx,byte ptr [esi+XFE_LCOUNT]
        cmp     ecx,11
        jbe     xls_cnt
        mov     ecx,11
xls_cnt:
        add     esi,XFE_CHARS
        mov     edi,OFFSET xv_ent
        push    edi
        push    ecx
        mov     ecx,11
        mov     al,' '
        rep     stosb
        pop     ecx
        pop     edi
xls_lp:
        lodsw
        or      ah,ah
        jz      xls_narrow
        mov     al,'_'
xls_narrow:
        call    fsr_fold
        stosb
        loop    xls_lp
        mov     byte ptr [xv_ent+DE_ATTR],ATTR_VOLUME
        mov     byte ptr [xv_ekind],XVK_LABEL
        popad
        ret

;=====================================================================
; WRITING A DIRECTORY
;=====================================================================

;---------------------------------------------------------------------
; xv_update - fat_dirupdate on exFAT: the FAT entry in xv_ent has been
;             changed, and the set it was made from is changed to match.
;             CF on failure.  Preserves all.
;
;   E5h in its first byte       the set is deleted
;   a set reserved, not written  it is written, with the name the
;                                reservation was given or the 8.3 one
;                                in the entry
;   the label                    the root's label entry
;   a set                        its attributes, stamps, cluster, size;
;                                and its name, if that changed
;
; THE CONTEXT IT WAS MADE IN IS THE ONE WRITTEN: a caller that has moved
; the directory context since its dir_seek is still writing the entry it
; was given.
;---------------------------------------------------------------------
xv_update:
        pushad
        cmp     byte ptr [xv_ro],0
        jne     xup_fail
        ; the directory it came from
        mov     al,[dc_isroot]
        mov     [xup_isroot],al
        mov     eax,[dc_start]
        mov     [xup_start],eax
        mov     eax,[xv_edir]
        cmp     eax,[dc_start]
        je      xup_ctx
        call    ctx_chain
xup_ctx:
        mov     al,[xv_ekind]
        cmp     al,XVK_NEW
        je      xup_new
        cmp     al,XVK_SET
        je      xup_set
        cmp     al,XVK_LABEL
        je      xup_label
        cmp     al,XVK_DOT              ; a made-up entry, or one that is not
        je      xup_ok                  ; a file: nothing on the disk to change
        cmp     al,XVK_OTHER
        je      xup_ok
        ; a free slot written without a reservation: the label, made
        test    byte ptr [xv_ent+DE_ATTR],ATTR_VOLUME
        jz      xup_fail
        cmp     byte ptr [xv_ent],0E5h
        je      xup_ok
        cmp     byte ptr [xv_ent],0
        je      xup_ok
        call    xv_lblwrite
        jc      xup_fail
        jmp     xup_ok
xup_label:
        call    xv_lblwrite
        jc      xup_fail
        jmp     xup_ok
xup_new:
        cmp     byte ptr [xv_ent],0E5h  ; reserved and then abandoned
        je      xup_ok
        cmp     byte ptr [xv_ent],0
        je      xup_ok
        test    byte ptr [xv_ent+DE_ATTR],ATTR_VOLUME
        jz      xup_newset
        call    xv_lblwrite
        jc      xup_fail
        mov     byte ptr [xv_nwon],0
        jmp     xup_ok
xup_newset:
        call    xv_create
        jc      xup_fail
        jmp     xup_ok
xup_set:
        cmp     byte ptr [xv_ent],0E5h
        jne     xup_mod
        call    xv_delset
        jc      xup_fail
        jmp     xup_ok
xup_mod:
        call    xv_modset
        jc      xup_fail
xup_ok:
        call    xup_back
        mov     byte ptr [xv_pson],0
        popad
        clc
        ret
xup_fail:
        call    xup_back
        mov     byte ptr [xv_pson],0
        popad
        stc
        ret
; ...and the caller's context back
xup_back:
        mov     eax,[xup_start]
        cmp     eax,[dc_start]
        je      xub_out
        mov     al,[xup_isroot]
        mov     edx,[xup_start]
        call    ctx_restore
xub_out:
        ret

;---------------------------------------------------------------------
; xv_delset - the set at [xv_eslot], [xv_eslots] entries, deleted: every
;             entry's in-use bit off.  Its clusters are the caller's to
;             free; a directory leaves the table.  CF on failure.
;---------------------------------------------------------------------
xv_delset:
        pushad
        mov     ebx,[xv_eslot]
        movzx   ecx,byte ptr [xv_eslots]
        call    xv_markfree
        jc      xds_fail
        test    byte ptr [xv_eorig+DE_ATTR],ATTR_DIR
        jz      xds_ok
        mov     eax,[xv_efirst]
        or      eax,eax
        jz      xds_ok
        push    esi
        call    xv_regfind
        pop     esi
        jc      xds_ok
        mov     edi,eax                 ; out of the table, its run kept in
        call    xv_rundrop              ; hand for the free that follows
        mov     [xv_tfirst],edi
        mov     eax,[xv_erun]
        mov     [xv_trun],eax
xds_ok:
        mov     byte ptr [xv_ekind],XVK_FREE
        popad
        clc
        ret
xds_fail:
        popad
        stc
        ret

; xv_markfree - ECX entries from slot EBX: in use no longer
xv_markfree:
        pushad
xmf_lp:
        call    dir_rawseek
        jc      xmf_fail
        and     byte ptr [esi],0FFh XOR XFT_INUSE
        call    dir_rawupdate
        jc      xmf_fail
        inc     ebx
        loop    xmf_lp
        popad
        clc
        ret
xmf_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_modset - the set at [xv_eslot] takes what changed in xv_ent.  CF on
;             failure.
;---------------------------------------------------------------------
xv_modset:
        pushad
        mov     ebx,[xv_eslot]
        call    dir_rawseek
        jc      xms_fail
        cmp     byte ptr [esi],XFT_FILE
        jne     xms_fail
        call    xv_getset               ; as it is on the disk now
        jc      xms_fail

        ; attributes, keeping the bits FAT's byte does not have
        mov     al,[xv_ent+DE_ATTR]
        and     al,ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_DIR OR ATTR_ARCHIVE
        cmp     byte ptr [xv_ebig],0    ; the read-only a big file was shown
        je      xms_attr                ; with is not the file's own
        mov     ah,byte ptr [xv_set+XFE_ATTR]
        and     ah,ATTR_READONLY
        and     al,0FFh XOR ATTR_READONLY
        or      al,ah
xms_attr:
        and     byte ptr [xv_set+XFE_ATTR],0FFh XOR (ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_DIR OR ATTR_ARCHIVE)
        or      byte ptr [xv_set+XFE_ATTR],al

        ; the stamps that changed
        mov     eax,dword ptr [xv_ent+DE_TIME]
        cmp     eax,dword ptr [xv_eorig+DE_TIME]
        je      xms_tsame
        mov     dword ptr [xv_set+XFE_MOD],eax
        mov     byte ptr [xv_set+XFE_MOD10],0
        mov     byte ptr [xv_set+XFE_MODUTC],0
xms_tsame:
        mov     eax,dword ptr [xv_ent+14]
        cmp     eax,dword ptr [xv_eorig+14]
        jne     xms_crt
        mov     al,[xv_ent+13]
        cmp     al,[xv_eorig+13]
        je      xms_csame
xms_crt:
        mov     eax,dword ptr [xv_ent+14]
        mov     dword ptr [xv_set+XFE_CRT],eax
        mov     al,[xv_ent+13]
        mov     [xv_set+XFE_CRT10],al
        mov     byte ptr [xv_set+XFE_CRTUTC],0
xms_csame:
        mov     ax,word ptr [xv_ent+18]
        cmp     ax,word ptr [xv_eorig+18]
        je      xms_asame
        mov     word ptr [xv_set+XFE_ACC+2],ax
        mov     word ptr [xv_set+XFE_ACC],0
        mov     byte ptr [xv_set+XFE_ACCUTC],0
xms_asame:

        ; THE DATA, for a file: a directory's is the directory's own,
        ; and dir_extend keeps it; a file of 4G and more is never
        ; written through here
        test    byte ptr [xv_set+XFE_ATTR],ATTR_DIR
        jnz     xms_dataok
        cmp     byte ptr [xv_ebig],0
        jne     xms_dataok
        mov     esi,OFFSET xv_ent
        call    de_getclus0             ; EAX = the kernel cluster in it
        mov     edx,dword ptr [xv_ent+DE_SIZE]
        cmp     eax,[xv_efirst]
        jne     xms_data
        cmp     edx,dword ptr [xv_eorig+DE_SIZE]
        jne     xms_data
        cmp     byte ptr [xv_pson],0
        je      xms_dataok
xms_data:
        call    xv_stream               ; EAX = cluster, EDX = size
xms_dataok:

        ; THE NAME, if it changed: rewritten where it is if it fits in
        ; the entries it has, moved within the directory if not
        call    xv_newname              ; -> xv_nbuf/xv_nlen, CF if unchanged
        jc      xms_write
        call    xv_putname
        jc      xms_fail
        movzx   eax,byte ptr [xv_set+XFE_SECCNT]
        inc     eax                     ; entries it needs now
        movzx   ecx,byte ptr [xv_eslots]
        cmp     eax,ecx
        jbe     xms_fits
        ; MOVED: a run of free entries, the set written there, the old
        ; one deleted
        mov     ecx,eax
        call    xv_findrun              ; -> EBX = the first slot
        jc      xms_fail
        push    ebx
        call    xv_putset
        pop     ebx
        jc      xms_fail
        push    ebx
        mov     ebx,[xv_eslot]
        movzx   ecx,byte ptr [xv_eslots]
        call    xv_markfree
        pop     ebx
        jc      xms_fail
        mov     [xv_eslot],ebx
        call    xv_dirmoved
        jmp     xms_done
xms_fits:
        ; written over the old one; any entries it no longer needs go
        mov     ebx,[xv_eslot]
        call    xv_putset
        jc      xms_fail
        movzx   ecx,byte ptr [xv_eslots]
        sub     ecx,eax
        jbe     xms_done
        mov     ebx,[xv_eslot]
        add     ebx,eax
        call    xv_markfree
        jc      xms_fail
        jmp     xms_done
xms_write:
        mov     ebx,[xv_eslot]
        call    xv_putset
        jc      xms_fail
xms_done:
        movzx   eax,byte ptr [xv_set+XFE_SECCNT]
        inc     eax
        mov     [xv_eslots],al
        mov     esi,OFFSET xv_ent       ; it is what was written now
        mov     edi,OFFSET xv_eorig
        mov     ecx,8
        rep     movsd
        popad
        clc
        ret
xms_fail:
        popad
        stc
        ret

; de_getclus0 - de_getclus without the run it sets: EAX = the cluster in
;               the FAT entry at ESI
de_getclus0:
        movzx   eax,word ptr [esi+DE_CLUSHI]
        shl     eax,16
        mov     ax,word ptr [esi+DE_CLUSTER]
        ret

;---------------------------------------------------------------------
; xv_stream - the set in xv_set is a file of EDX bytes at kernel cluster
;             EAX (0 for none): its stream extension says so.
;
; THE FLAG AND THE VALID LENGTH come from the open file when a close is
; writing (xv_pson), from the run in hand otherwise; a file given new
; clusters without either is a FAT chain, all of it valid.
;---------------------------------------------------------------------
xv_stream:
        pushad
        mov     dword ptr [xv_set+32+XFE_DLEN],edx
        mov     dword ptr [xv_set+32+XFE_DLEN+4],0
        mov     byte ptr [xv_set+32+XFE_SFLAGS],XFS_ALLOC
        or      eax,eax
        jnz     xst_clus
        mov     dword ptr [xv_set+32+XFE_FIRST],0
        mov     dword ptr [xv_set+32+XFE_VDL],0
        mov     dword ptr [xv_set+32+XFE_VDL+4],0
        jmp     xst_out
xst_clus:
        mov     ebx,eax
        call    xv_phys
        mov     dword ptr [xv_set+32+XFE_FIRST],eax
        mov     dword ptr [xv_set+32+XFE_VDL+4],0
        mov     ecx,edx                 ; the valid length: all of it...
        xor     esi,esi                 ; ...and no run
        cmp     byte ptr [xv_pson],0
        je      xst_hand
        mov     esi,[xv_psrun]          ; a close says
        mov     ecx,[xv_psvdl]
        jmp     xst_have
xst_hand:
        cmp     ebx,[xv_tfirst]
        jne     xst_have
        mov     esi,[xv_trun]
xst_have:
        cmp     ecx,edx
        jbe     xst_vdl
        mov     ecx,edx
xst_vdl:
        mov     dword ptr [xv_set+32+XFE_VDL],ecx
        or      esi,esi
        jz      xst_out
        or      byte ptr [xv_set+32+XFE_SFLAGS],XFS_NOFAT
xst_out:
        call    xv_fixsum
        popad
        ret

; xv_fixsum - the set's checksum, put right.  Preserves all.
xv_fixsum:
        push    eax
        call    xv_setsum
        mov     word ptr [xv_set+XFE_SETSUM],ax
        pop     eax
        ret

;---------------------------------------------------------------------
; xv_newname - the name the set is to have, if it is not the one it has:
;              a long one given (xv_pnon / a reservation's), or the 8.3
;              field of xv_ent when that is not the alias it was made
;              with.  -> xv_nbuf / xv_nlen; CF if the name stays.
;---------------------------------------------------------------------
xv_newname:
        pushad
        cmp     byte ptr [xv_pnon],0
        jne     xnn_long
        mov     esi,OFFSET xv_ent
        mov     edi,OFFSET xv_eorig
        mov     ecx,11
        repe    cmpsb
        je      xnn_same
        call    xv_83name
        jmp     xnn_new
xnn_long:
        mov     byte ptr [xv_pnon],0
xnn_new:
        popad
        clc
        ret
xnn_same:
        popad
        stc
        ret

; xv_83name - xv_ent's 8.3 field spelt out as a name, "NAME.EXT", into
;             xv_nbuf / xv_nlen
xv_83name:
        pushad
        mov     esi,OFFSET xv_ent
        mov     edi,OFFSET xv_nbuf
        xor     ecx,ecx
x8n_base:
        cmp     ecx,8
        jae     x8n_ext
        mov     al,[esi+ecx]
        cmp     al,' '
        je      x8n_ext
        or      ecx,ecx
        jnz     x8n_bput
        cmp     al,05h                  ; FAT's stand-in for a leading E5h
        jne     x8n_bput
        mov     al,0E5h
x8n_bput:
        stosb
        inc     ecx
        jmp     x8n_base
x8n_ext:
        cmp     byte ptr [esi+8],' '
        je      x8n_done
        mov     al,'.'
        stosb
        mov     ecx,8
x8n_e:
        cmp     ecx,11
        jae     x8n_done
        mov     al,[esi+ecx]
        cmp     al,' '
        je      x8n_done
        stosb
        inc     ecx
        jmp     x8n_e
x8n_done:
        sub     edi,OFFSET xv_nbuf
        mov     [xv_nlen],edi
        popad
        ret

;---------------------------------------------------------------------
; xv_putname - the name in xv_nbuf / xv_nlen into the set in xv_set: its
;              length and hash in the stream, its name entries rebuilt,
;              SecondaryCount to match, the checksum.  Benign entries
;              after the name are kept.  CF if the name will not fit.
;---------------------------------------------------------------------
xv_putname:
        pushad
        mov     ecx,[xv_nlen]
        or      ecx,ecx
        jz      xpn_fail
        cmp     ecx,XF_NAMEMAX
        ja      xpn_fail
        ; the benign entries the old set had after its name, aside
        movzx   eax,byte ptr [xv_set+32+XFE_NLEN]
        add     eax,14
        xor     edx,edx
        mov     ebx,15
        div     ebx
        mov     ebx,eax                 ; EBX = old name entries
        movzx   eax,byte ptr [xv_set+XFE_SECCNT]
        dec     eax
        sub     eax,ebx                 ; EAX = entries after them
        mov     [xpn_extra],eax
        lea     esi,[xv_set+64+ebx*8]
        lea     esi,[esi+ebx*8]
        lea     esi,[esi+ebx*8]
        lea     esi,[esi+ebx*8]         ; + 32 * old name entries
        mov     edi,OFFSET xpn_keep
        mov     ecx,eax
        shl     ecx,3
        rep     movsd
        ; the new name entries
        mov     ecx,[xv_nlen]
        lea     eax,[ecx+14]
        xor     edx,edx
        mov     ebx,15
        div     ebx                     ; EAX = new name entries
        mov     ebx,eax
        add     eax,[xpn_extra]
        add     eax,1
        cmp     eax,XV_SETMAX - 1
        ja      xpn_fail
        mov     byte ptr [xv_set+XFE_SECCNT],al
        mov     byte ptr [xv_set+32+XFE_NLEN],cl
        mov     edi,OFFSET xv_set + 64
        mov     esi,OFFSET xv_nbuf
xpn_entry:
        mov     word ptr [edi],XFT_NAME ; type, and no flags
        add     edi,2
        mov     edx,15
xpn_char:
        xor     eax,eax
        jecxz   xpn_pad
        lodsb
        dec     ecx
xpn_pad:
        stosw
        dec     edx
        jnz     xpn_char
        dec     ebx
        jnz     xpn_entry
        mov     esi,OFFSET xpn_keep
        mov     ecx,[xpn_extra]
        shl     ecx,3
        rep     movsd
        call    xv_hashname
        call    xv_fixsum
        popad
        clc
        ret
xpn_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_hashname - the stream's NameHash, over the name in xv_nbuf /
;               xv_nlen up-cased by the volume's own table.  Preserves
;               all.
;---------------------------------------------------------------------
xv_hashname:
        pushad
        mov     esi,OFFSET xv_nbuf
        mov     ecx,[xv_nlen]
        xor     eax,eax                 ; AX = the hash
xhn_lp:
        jecxz   xhn_done
        movzx   ebx,byte ptr [esi]
        inc     esi
        mov     bx,[xv_upcase+ebx*2]
        ror     ax,1
        add     al,bl
        adc     ah,0
        ror     ax,1
        add     al,bh
        adc     ah,0
        dec     ecx
        jmp     xhn_lp
xhn_done:
        mov     word ptr [xv_set+32+XFE_HASH],ax
        popad
        ret

;---------------------------------------------------------------------
; xv_putset - the set in xv_set written at slot EBX.  Its last entry
;             first and its file entry last: a set cut short by a crash
;             has a file entry whose checksum is wrong, or none, and is
;             not a file.  CF on failure.  Preserves all.
;---------------------------------------------------------------------
xv_putset:
        pushad
        call    xv_fixsum               ; whatever changed, the sum is made
        movzx   ecx,byte ptr [xv_set+XFE_SECCNT]        ; here, last
xpu_lp:
        push    ebx
        add     ebx,ecx
        call    dir_rawseek
        pop     ebx
        jc      xpu_fail
        mov     edi,esi
        mov     esi,ecx
        shl     esi,5
        add     esi,OFFSET xv_set
        push    ecx
        mov     ecx,8
        rep     movsd
        pop     ecx
        call    dir_rawupdate
        jc      xpu_fail
        dec     ecx
        jns     xpu_lp
        popad
        clc
        ret
xpu_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_findrun - ECX free entries in a row in the context directory, which
;              grows if it has to.  -> EBX = the first slot.  CF if it
;              cannot be had.
;---------------------------------------------------------------------
xv_findrun:
        push    eax
        push    edx
        push    esi
        xor     ebx,ebx
        xor     edx,edx                 ; EDX = the run so far
xfr_lp:
        lea     eax,[ebx+edx]
        cmp     eax,XV_DIRMAX           ; as big as a directory can be
        jae     xfr_fail
        push    ebx
        add     ebx,edx
        call    dir_rawseek
        pop     ebx
        jnc     xfr_look
        call    xv_dirext               ; the end of the directory: more
        jc      xfr_fail
        jmp     xfr_lp
xfr_look:
        mov     al,[esi]
        or      al,al
        jz      xfr_free
        test    al,XFT_INUSE
        jz      xfr_free
        ; in use: the run starts again after it
        lea     ebx,[ebx+edx+1]
        xor     edx,edx
        jmp     xfr_lp
xfr_free:
        inc     edx
        cmp     edx,ecx
        jb      xfr_lp
        pop     esi
        pop     edx
        pop     eax
        clc
        ret
xfr_fail:
        pop     esi
        pop     edx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; xv_reserve - fat_findfree and lfn_store on exFAT.  ECX = the entries
;              the set will take: a run of them is found and reserved,
;              and EBX = the index of its first, ESI -> xv_ent standing
;              for it, to be filled in and given to fat_dirupdate.
;              [xv_nwlong] says whether xv_nbuf already holds its name.
;              CF if there is no room.
;---------------------------------------------------------------------
xv_reserve:
        push    eax
        cmp     byte ptr [xv_ro],0
        jne     xrv_fail
        call    xv_findrun
        jc      xrv_fail
        mov     [xv_nwslot],ebx
        mov     [xv_nwcnt],ecx
        mov     eax,[dc_start]
        mov     [xv_nwdir],eax
        mov     byte ptr [xv_nwon],1
        cmp     eax,[fat_rootclus]
        je      xrv_root
        add     ebx,2                   ; past the made-up "." and ".."
xrv_root:
        call    xv_seek                 ; -> the blank one
        pop     eax
        ret
xrv_fail:
        pop     eax
        stc
        ret

; xv_findfree - fat_findfree: a set for an 8.3 name, three entries
xv_findfree:
        push    ecx
        mov     byte ptr [xv_nwlong],0
        mov     ecx,3
        call    xv_reserve
        pop     ecx
        ret

; xv_lfnstore - lfn_store: ESI -> the name, ECX = its length.  Kept, and
;               a set of the size it needs reserved.  A name holding a
;               character exFAT forbids is refused here.
xv_lfnstore:
        push    eax
        push    ecx
        push    edx
        call    xv_takename
        jc      xlf_fail
        mov     eax,[xv_nlen]
        add     eax,14
        xor     edx,edx
        mov     ecx,15
        div     ecx
        lea     ecx,[eax+2]             ; the file entry, the stream, the names
        mov     byte ptr [xv_nwlong],1
        call    xv_reserve
        pop     edx
        pop     ecx
        pop     eax
        ret
xlf_fail:
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

; xv_pendname - 56h: ESI -> the new long name, ECX = its length, for the
;               set its next fat_dirupdate writes.  CF if it cannot be
;               an exFAT name.  Preserves all.
xv_pendname:
        call    xv_takename
        jc      xpd_out
        mov     byte ptr [xv_pnon],1
xpd_out:
        ret

; xv_takename - ESI -> a name, ECX = its length: into xv_nbuf/xv_nlen,
;               or CF if it is empty, too long, or holds a character
;               exFAT names may not.  Preserves all.
xv_takename:
        pushad
        or      ecx,ecx
        jz      xtn_fail
        cmp     ecx,XF_NAMEMAX
        ja      xtn_fail
        mov     [xv_nlen],ecx
        mov     edi,OFFSET xv_nbuf
xtn_copy:
        lodsb
        cmp     al,' '
        jb      xtn_fail
        push    ecx
        push    edi
        mov     edi,OFFSET xtn_bad
        mov     ecx,XTN_NBAD
        repne   scasb
        pop     edi
        pop     ecx
        je      xtn_fail
        stosb
        loop    xtn_copy
        popad
        clc
        ret
xtn_fail:
        popad
        stc
        ret
xtn_bad     db  '"*/:<>?\|'
XTN_NBAD    EQU     $ - xtn_bad

;---------------------------------------------------------------------
; xv_create - the reserved run is written as a set: the name given, or
;             xv_ent's 8.3 one; xv_ent's attributes, stamp, cluster and
;             size.  What a rename is carrying (xv_hold) for the same
;             first cluster - the valid length, the run, the creation
;             stamp, a length past 4G - goes with it.  CF on failure.
;---------------------------------------------------------------------
xv_create:
        pushad
        cmp     byte ptr [xv_nwlong],0
        jne     xcr_named
        call    xv_83name
xcr_named:
        mov     edi,OFFSET xv_set       ; a file entry and a stream, clean
        xor     eax,eax
        mov     ecx,(XV_SETMAX * 32) / 4
        rep     stosd
        mov     byte ptr [xv_set],XFT_FILE
        mov     byte ptr [xv_set+XFE_SECCNT],1  ; just the stream, so far
        mov     byte ptr [xv_set+32],XFT_STREAM
        mov     al,[xv_ent+DE_ATTR]
        and     al,ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_DIR OR ATTR_ARCHIVE
        mov     byte ptr [xv_set+XFE_ATTR],al
        mov     eax,dword ptr [xv_ent+DE_TIME]
        mov     dword ptr [xv_set+XFE_MOD],eax
        mov     dword ptr [xv_set+XFE_ACC],eax
        mov     dword ptr [xv_set+XFE_CRT],eax

        mov     esi,OFFSET xv_ent
        call    de_getclus0
        mov     edx,dword ptr [xv_ent+DE_SIZE]
        cmp     byte ptr [xv_hon],0
        je      xcr_fresh
        or      eax,eax
        jz      xcr_fresh
        cmp     eax,[xv_hfirst]
        jne     xcr_fresh
        ; A RENAME: the set it was is the set it is, but its name
        mov     ecx,[xv_hcrt]
        mov     dword ptr [xv_set+XFE_CRT],ecx
        mov     cl,[xv_hcrt10]
        mov     [xv_set+XFE_CRT10],cl
        mov     cl,[xv_hflags]
        mov     [xv_set+32+XFE_SFLAGS],cl
        push    eax
        call    xv_phys
        mov     dword ptr [xv_set+32+XFE_FIRST],eax
        pop     eax
        mov     ecx,[xv_hdlen]
        mov     dword ptr [xv_set+32+XFE_DLEN],ecx
        mov     ecx,[xv_hdlen+4]
        mov     dword ptr [xv_set+32+XFE_DLEN+4],ecx
        mov     ecx,[xv_hvdl]
        mov     dword ptr [xv_set+32+XFE_VDL],ecx
        cmp     dword ptr [xv_hdlen+4],0
        je      xcr_named2
        mov     ecx,[xv_hdlen]          ; past 4G: all of it valid, as far as
        mov     dword ptr [xv_set+32+XFE_VDL],ecx       ; anything was ever
        mov     ecx,[xv_hdlen+4]                        ; told
        mov     dword ptr [xv_set+32+XFE_VDL+4],ecx
        jmp     xcr_named2
xcr_fresh:
        test    byte ptr [xv_ent+DE_ATTR],ATTR_DIR
        jz      xcr_file
        ; A NEW DIRECTORY: its one cluster, which mkdir has just had
        call    xcr_dirlen
        jmp     xcr_named2
xcr_file:
        call    xv_stream
xcr_named2:
        call    xv_putname
        jc      xcr_fail
        mov     ebx,[xv_nwslot]
        call    xv_putset
        jc      xcr_fail
        mov     byte ptr [xv_nwon],0
        mov     byte ptr [xv_ekind],XVK_SET
        mov     [xv_eslot],ebx
        movzx   eax,byte ptr [xv_set+XFE_SECCNT]
        inc     eax
        mov     [xv_eslots],al
        mov     esi,OFFSET xv_ent
        call    de_getclus0
        mov     [xv_efirst],eax
        mov     esi,OFFSET xv_ent
        mov     edi,OFFSET xv_eorig
        mov     ecx,8
        rep     movsd
        ; a directory, new or renamed, is where it is now in the table
        test    byte ptr [xv_ent+DE_ATTR],ATTR_DIR
        jz      xcr_ok
        mov     eax,[xv_efirst]
        or      eax,eax
        jz      xcr_ok
        xor     ecx,ecx
        cmp     byte ptr [xv_hon],0
        je      xcr_reg
        cmp     eax,[xv_hfirst]
        jne     xcr_reg
        mov     ecx,[xv_hrun]
xcr_reg:
        mov     edx,[xv_nwdir]
        call    xv_regput
xcr_ok:
        popad
        clc
        ret
xcr_fail:
        popad
        stc
        ret

; xcr_dirlen - a new directory's stream: its first cluster, a whole
;              physical cluster long, all valid, a FAT chain
xcr_dirlen:
        push    eax
        push    ecx
        mov     byte ptr [xv_set+32+XFE_SFLAGS],XFS_ALLOC
        or      eax,eax
        jz      xcd_none
        call    xv_phys
        mov     dword ptr [xv_set+32+XFE_FIRST],eax
        mov     cl,[xv_pshift]
        add     cl,9
        mov     eax,1
        shl     eax,cl
        mov     dword ptr [xv_set+32+XFE_DLEN],eax
        mov     dword ptr [xv_set+32+XFE_VDL],eax
xcd_none:
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; xv_dirmoved - a directory's set moved to slot [xv_eslot]: the table
;               says where it is.  Preserves all.
;---------------------------------------------------------------------
xv_dirmoved:
        pushad
        test    byte ptr [xv_set+XFE_ATTR],ATTR_DIR
        jz      xdm_out
        mov     eax,[xv_efirst]
        or      eax,eax
        jz      xdm_out
        call    xv_regfind
        jc      xdm_out
        mov     eax,[xv_eslot]
        mov     [esi+XFR_PSLOT],eax
xdm_out:
        popad
        ret

;---------------------------------------------------------------------
; xv_lblwrite - the volume label as xv_ent's 8.3 field has it, or none
;               if its first byte is E5h, into the root's label entry -
;               the one there is, or a new one in the slot reserved.  CF
;               on failure.
;---------------------------------------------------------------------
xv_lblwrite:
        pushad
        mov     eax,[dc_start]
        cmp     eax,[fat_rootclus]
        jne     xlw_fail
        ; the label entry that is there, if any
        xor     ebx,ebx
xlw_find:
        cmp     ebx,XV_DIRMAX
        jae     xlw_fail
        call    dir_rawseek
        jc      xlw_new
        mov     al,[esi]
        or      al,al
        jz      xlw_new
        cmp     al,XFT_LABEL
        je      xlw_have
        inc     ebx
        jmp     xlw_find
xlw_new:
        cmp     byte ptr [xv_ent],0E5h
        je      xlw_ok                  ; no label, and none to remove
        mov     ecx,1
        call    xv_findrun
        jc      xlw_fail
        call    dir_rawseek
        jc      xlw_fail
xlw_have:
        mov     edi,esi                 ; a clean label entry
        push    edi
        xor     eax,eax
        mov     ecx,8
        rep     stosd
        pop     edi
        mov     byte ptr [edi],XFT_LABEL
        ; the eleven bytes, trailing blanks dropped; none at all for a
        ; label deleted
        xor     ecx,ecx
        cmp     byte ptr [xv_ent],0E5h
        je      xlw_count
        mov     ecx,11
xlw_trim:
        cmp     byte ptr [xv_ent+ecx-1],' '
        jne     xlw_count
        loop    xlw_trim
xlw_count:
        mov     [edi+XFE_LCOUNT],cl
        mov     [xv_lcount],cl
        push    edi
        push    ecx
        mov     edi,OFFSET xv_label
        xor     eax,eax
        mov     ecx,11
        rep     stosw
        pop     ecx
        pop     edi
        xor     ebx,ebx
xlw_char:
        cmp     ebx,ecx
        jae     xlw_put
        movzx   eax,byte ptr [xv_ent+ebx]
        mov     [edi+XFE_CHARS+ebx*2],ax
        mov     word ptr [xv_label+ebx*2],ax
        inc     ebx
        jmp     xlw_char
xlw_put:
        call    dir_rawupdate
        jc      xlw_fail
xlw_ok:
        popad
        clc
        ret
xlw_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_dirext - dir_extend on exFAT: a physical cluster more for the
;             context directory, zeroed, joined on, and - for anything
;             but the root - the directory's own entry says it is that
;             much longer.  CF if the disk is full, or the directory's
;             entry cannot be found to say so.
;---------------------------------------------------------------------
xv_dirext:
        pushad
        cmp     byte ptr [xv_ro],0
        jne     xde_fail
        ; a subdirectory's entry first: without it the cluster could
        ; not be accounted for
        mov     eax,[dc_start]
        cmp     eax,[fat_rootclus]
        je      xde_alloc
        call    xv_regfind
        jc      xde_fail
xde_alloc:
        call    fat_alloc
        jc      xde_fail
        mov     ebx,eax                 ; EBX = its first kernel cluster
        mov     cl,[xv_kshift]
        mov     edx,1
        shl     edx,cl
        mov     ecx,edx                 ; ECX = kernel clusters in it
xde_zero:
        call    fat_zeroclus
        jc      xde_fail
        inc     eax
        loop    xde_zero
        ; the tail, and the new cluster after it
        mov     eax,[dc_start]
        mov     edi,[fat_totclus]       ; EDI = the most a chain can have
xde_tail:
        mov     ecx,eax
        call    fat_next
        cmp     eax,[fat_eoc]
        jae     xde_end
        dec     edi
        jnz     xde_tail
        mov     eax,ebx                 ; a chain that goes round: the new
        call    fat_freechain           ; cluster goes back
        jmp     xde_fail
xde_end:
        mov     eax,ecx
        mov     edx,ebx
        call    fat_setnext
        mov     dword ptr [dc_curord],-1
        mov     eax,[dc_start]
        cmp     eax,[fat_rootclus]
        je      xde_ok
        call    xv_dirgrew
        jc      xde_fail
xde_ok:
        popad
        clc
        ret
xde_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; xv_dirgrew - the directory starting at kernel cluster EAX has grown by
;              a physical cluster: its stream in its parent says how long
;              it is now, and whether it is still a run.  CF on failure.
;---------------------------------------------------------------------
xv_dirgrew:
        pushad
        ; A SET MAY BE HALF BUILT IN xv_set - a rename moving one has
        ; asked for room, which is how the directory came to grow - and
        ; the parent's set is read into the same buffer
        mov     esi,OFFSET xv_set
        mov     edi,OFFSET xv_setsave
        mov     ecx,(XV_SETMAX * 32) / 4
        rep     movsd
        call    xv_regfind
        jc      xdg_fail
        mov     ecx,[esi+XFR_RUN]
        mov     [xdg_run],ecx
        mov     ebx,[esi+XFR_PSLOT]
        mov     edx,[esi+XFR_PDIR]
        mov     [xdg_first],eax
        ; into the parent, the caller's directory saved round it
        mov     al,[dc_isroot]
        mov     [xdg_isroot],al
        mov     eax,[dc_start]
        mov     [xdg_start],eax
        mov     eax,edx
        cmp     eax,[fat_rootclus]
        jne     xdg_sub
        call    ctx_root
        jmp     xdg_in
xdg_sub:
        call    ctx_chain
xdg_in:
        call    dir_rawseek
        jc      xdg_back
        cmp     byte ptr [esi],XFT_FILE
        jne     xdg_bad
        call    xv_getset
        jc      xdg_bad
        mov     eax,[xdg_first]
        call    xv_phys
        cmp     eax,dword ptr [xv_set+32+XFE_FIRST]
        jne     xdg_bad                 ; not the entry the table thought
        mov     cl,[xv_pshift]
        add     cl,9
        mov     eax,1
        shl     eax,cl
        add     dword ptr [xv_set+32+XFE_DLEN],eax
        adc     dword ptr [xv_set+32+XFE_DLEN+4],0
        mov     eax,dword ptr [xv_set+32+XFE_DLEN]
        mov     dword ptr [xv_set+32+XFE_VDL],eax
        mov     eax,dword ptr [xv_set+32+XFE_DLEN+4]
        mov     dword ptr [xv_set+32+XFE_VDL+4],eax
        and     byte ptr [xv_set+32+XFE_SFLAGS],0FFh XOR XFS_NOFAT
        cmp     dword ptr [xdg_run],0
        je      xdg_chain
        or      byte ptr [xv_set+32+XFE_SFLAGS],XFS_NOFAT
xdg_chain:
        call    xv_fixsum
        call    xv_putset
        jc      xdg_back
        call    xdg_restore
        call    xdg_setback
        popad
        clc
        ret
xdg_bad:
        stc
xdg_back:
        call    xdg_restore
xdg_fail:
        call    xdg_setback
        popad
        stc
        ret
xdg_restore:
        pushfd
        mov     al,[xdg_isroot]
        mov     edx,[xdg_start]
        call    ctx_restore
        popfd
        ret
xdg_setback:
        mov     esi,OFFSET xv_setsave
        mov     edi,OFFSET xv_set
        mov     ecx,(XV_SETMAX * 32) / 4
        rep     movsd
        ret

;---------------------------------------------------------------------
; xv_mkclus - 39h's new directory, at kernel cluster EAX: all of its
;             physical cluster zeroes, which is an empty directory.  CF
;             on failure.  Preserves all.
;---------------------------------------------------------------------
xv_mkclus:
        pushad
        mov     cl,[xv_kshift]
        mov     edx,1
        shl     edx,cl
        mov     ecx,edx
xmk_lp:
        call    fat_zeroclus
        jc      xmk_fail
        inc     eax
        loop    xmk_lp
        popad
        clc
        ret
xmk_fail:
        popad
        stc
        ret

;=====================================================================
; THE REST OF THE KERNEL'S QUESTIONS
;=====================================================================

;---------------------------------------------------------------------
; xv_info69 - 21h/69h's record for the loaded exFAT volume at EDI: the
;             serial, the label, "EXFAT   ".  Preserves all.
;---------------------------------------------------------------------
xv_info69:
        pushad
        call    xv_lblscan
        mov     word ptr [edi],0
        mov     eax,[xv_serial]
        mov     [edi+2],eax
        add     edi,6
        push    edi
        mov     ecx,11
        mov     al,' '
        rep     stosb
        pop     edi
        movzx   ecx,byte ptr [xv_lcount]
        jecxz   xi69_none
        mov     esi,OFFSET xv_label
xi69_lp:
        lodsw
        or      ah,ah
        jz      xi69_put
        mov     al,'_'
xi69_put:
        call    fsr_fold
        stosb
        loop    xi69_lp
        jmp     xi69_fs
xi69_none:
        mov     esi,OFFSET xi69_noname
        mov     ecx,11
        rep     movsb
xi69_fs:
        popad
        pushad
        add     edi,6 + 11
        mov     dword ptr [edi],'FAXE'
        mov     dword ptr [edi+4],'   T'
        popad
        ret
xi69_noname db  'NO NAME    '

;=====================================================================
; State.  None of it is a volume's: it is what one call is doing, and a
; volume swap or the next call starts it afresh.  The volume's own is
; in vol_state, over FAT12's cache (FAT.INC).
;=====================================================================
xv_why      db  0                       ; why a mount failed: 1 = a read
xv_ekind    db  0                       ; XVK_*: what xv_ent is
xv_eslots   db  0                       ; ...entries in its set
xv_eflags   db  0                       ; ...its stream's flags
xv_ebig     db  0                       ; ...4G or more
            db  0,0,0
xv_eslot    dd  0                       ; ...its slot
xv_edir     dd  0                       ; ...the directory it is in
xv_ent      db  32 dup(0)               ; the FAT entry made
xv_eorig    db  32 dup(0)               ; ...as it was when made

; what the entry cannot carry, in one block xv_hold copies whole
xv_efirst   dd  0                       ; its first kernel cluster
xv_erun     dd  0                       ; its run, 0 = a chain
xv_evdl     dd  0                       ; ValidDataLength, as 32 bits go
xv_edlen    dd  0,0                     ; DataLength
xv_ecrt     dd  0                       ; created
xv_ecrt10   db  0
            db  0,0,0
XV_HOLDLEN  EQU     $ - xv_efirst
xv_hfirst   dd  0                       ; ...the same, for a rename
xv_hrun     dd  0
xv_hvdl     dd  0
xv_hdlen    dd  0,0
xv_hcrt     dd  0
xv_hcrt10   db  0
            db  0,0,0
xv_hon      db  0                       ; xv_hold holds something
xv_hflags   db  0                       ; ...and its stream's flags

; the run in hand: FIRST and RUN, as a table record lays them out
xv_tfirst   dd  0
xv_trun     dd  0

; a set reserved and not yet written
xv_nwon     db  0
xv_nwlong   db  0                       ; its name is in xv_nbuf
xv_pnon     db  0                       ; a new name for an existing set
xv_pson     db  0                       ; a close is writing (xv_psvdl/run)
xv_nwdir    dd  0
xv_nwslot   dd  0
xv_nwcnt    dd  0
xv_psvdl    dd  0
xv_psrun    dd  0
xv_nlen     dd  0
xv_nbuf     db  XF_NAMEMAX + 1 dup(0)
            db  0,0,0

; scratch
xm_vlen     dd  0
xbc_base    dd  0
xbc_left    dd  0
xbc_unit    dd  0
xrs_upfirst dd  0
xrs_uplen   dd  0
xrs_upsum   dd  0
xul_left    dd  0
xul_clus    dd  0
xul_ch      dd  0
xul_cnt     db  0
xul_odd     db  0
xul_lo      db  0
            db  0
xgs_count   dd  0
xrp_rec     dd  0,0,0,0
xrg_cap     dd  0
xpn_extra   dd  0
xdg_run     dd  0
xdg_first   dd  0
xdg_start   dd  0
xdg_isroot  db  0
xup_isroot  db  0
            db  0,0
xup_start   dd  0
xal2_stem   db  8 dup(0)
xal2_num    db  8 dup(0)
xnl_lossy   db  0                       ; the name had a character the
            db  0,0,0                   ; kernel's names cannot hold

_DOS    ENDS
_KBSS   SEGMENT
xv_set      db  XV_SETMAX * 32 dup(?)   ; a set, as read or to be written
xpn_keep    db  XV_SETMAX * 32 dup(?)   ; its benign entries, while its name
                                        ; is rebuilt
xv_setsave  db  XV_SETMAX * 32 dup(?)   ; xv_set, while a directory's own
                                        ; entry is rewritten (xv_dirgrew)
xul_tab     dw  256 dup(?)              ; an up-case table being read
xv_bsave    db  512 dup(?)              ; the boot sector, from the mount
_KBSS   ENDS
_DOS    SEGMENT
