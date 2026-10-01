;=====================================================================
; RECYCLE.INC - delete protection: UNDELETE=ON|OFF
;
; With UNDELETE=ON (the default) a deleted file is KEPT.  Its clusters
; are not freed; its directory entry is moved into X:\RECYCLED under a
; name of its own, and a record in X:\RECYCLED\UNDELETE.DAT says where
; it was and what it was called.  UNDELETE.EXE reads the records and
; renames files back, or empties the directory for good.  The format
; is in PMDOS.INC, under DELETE PROTECTION.
;
; ONE ROUTINE DOES EVERY DELETE THE KERNEL MAKES - rcy_delete - and the
; three ways in are 41h (and 7141h, which IS 41h here), the FCB API's
; 13h, and F1h AL=19h.  A 16-bit program in a DOS box calls the same
; handlers a native one does, so there is nothing more to hook for it.
;
; MOVING THE ENTRY IS THE WHOLE TRICK, and it is why this costs about
; what a rename costs.  A delete already knows the entry and its
; directory; the file goes into \RECYCLED the way 56h moves a file
; between directories, and the long name - which the move would lose -
; goes into the record instead.
;
; WHAT IS NOT KEPT, and is deleted exactly as before:
;   - anything with UNDELETE=OFF, or deleted through F1h AL=19h
;   - *.TMP and *.$$$.  The shells make and delete %PIPEn.$$$ for every
;     pipe and CMD.EXE's FOR /F makes %FORF.$$$; keeping those would
;     fill the allowance with plumbing
;   - anything deleted from inside \RECYCLED, which is how UNDELETE
;     /PURGE gets rid of things
;   - anything not on a FAT volume
;   - a file bigger than the whole allowance, and a file whose
;     directory path is too long to write into the record
;   - anything, when \RECYCLED or its index cannot be made or read:
;     the program asked for a delete, and a delete it gets.  An
;     UNDELETE.DAT that is not ours (wrong signature) is left alone
;
; THE ALLOWANCE is UNDEL_PCT percent of what would be free with nothing
; kept: kept <= PCT% of (free + kept), which after the file being
; deleted has been added to "kept" is
;
;       (100 - PCT) * kept  <=  PCT * free
;
; in clusters.  Counting the free clusters is a walk of the whole FAT,
; so the first test stops counting as soon as it has seen enough, and
; the full count is only made when the answer is no.  Files go oldest
; first until the new one fits; the arithmetic after each is done on
; the numbers in hand rather than by counting again.
;
; Included by DOSMAIN.ASM after INT21.INC.
;=====================================================================

;---------------------------------------------------------------------
; rcy_delete - delete, or keep, the file fat_search just found.
;
;   The directory context is the file's directory, EBX = the entry's
;   index in it, ESI -> the entry, and [lfn_ok]/[lfn_first]/lfn_buf
;   still describe its long name.  [rcy_pnames]/[rcy_pcnt] = that
;   directory's path, as 11-byte canonical components from the root.
;   [rcy_perm] = 1 means do not keep it.  The caller has already made
;   every refusal (directory, read-only, mounted image).
;
;   -> CF with AX = the error.  Anything is trashed; a caller that
;      wants its context, its EBX or [srch_attr] back saves them.
;---------------------------------------------------------------------
rcy_delete:
        mov     eax,[crit_nfail]        ; see rcy_giveup
        mov     [rcy_nfail],eax

        ; everything the plain delete needs, before anything moves
        mov     [rcy_slot],ebx
        push    esi
        mov     edi,OFFSET rcy_ent
        mov     ecx,8
        rep     movsd
        pop     esi
        call    ctx_save
        mov     [rcy_disroot],al
        mov     [rcy_dstart],edx
        mov     al,[lfn_ok]
        mov     [rcy_haslfn],al
        mov     eax,[lfn_first]
        mov     [rcy_lfirst],eax

        cmp     byte ptr [rcy_perm],0
        jne     rcy_plain
        cmp     byte ptr [undel_on],0
        je      rcy_plain
        cmp     byte ptr [vol_fs],FS_FAT
        jne     rcy_plain

        ; a temporary file?
        mov     edi,OFFSET rcy_temps
ydl_tmp:
        cmp     byte ptr [edi],0
        je      ydl_nottmp
        mov     al,[rcy_ent+8]
        cmp     al,[edi]
        jne     ydl_tnext
        mov     al,[rcy_ent+9]
        cmp     al,[edi+1]
        jne     ydl_tnext
        mov     al,[rcy_ent+10]
        cmp     al,[edi+2]
        je      rcy_plain
ydl_tnext:
        add     edi,3
        jmp     short ydl_tmp
ydl_nottmp:

        ; from inside \RECYCLED?
        cmp     dword ptr [rcy_pcnt],0
        je      ydl_notbin
        mov     esi,[rcy_pnames]
        mov     edi,OFFSET rcy_dname
        mov     ecx,11
        repe    cmpsb
        je      rcy_plain
ydl_notbin:

        ; THE RECORD, built now while the path components are still
        ; the caller's - everything below resolves paths of its own.
        mov     edi,OFFSET rcy_rec
        xor     eax,eax
        mov     ecx,UDR_LEN / 4
        rep     stosd
        mov     byte ptr [rcy_rec+UDR_FLAGS],UDRF_USED
        mov     al,[rcy_ent+DE_ATTR]
        mov     [rcy_rec+UDR_ATTR],al
        mov     eax,dword ptr [rcy_ent+DE_SIZE]
        mov     dword ptr [rcy_rec+UDR_SIZE],eax
        mov     esi,OFFSET rcy_ent
        call    de_getclus
        mov     dword ptr [rcy_rec+UDR_CLUS],eax
        call    get_dostime
        mov     word ptr [rcy_rec+UDR_DTIME],ax
        call    get_dosdate
        mov     word ptr [rcy_rec+UDR_DDATE],ax

        mov     esi,OFFSET rcy_ent      ; "NAME.EXT"
        mov     ecx,1
        mov     edi,OFFSET rcy_rec+UDR_SHORT
        call    names_string

        mov     esi,[rcy_pnames]        ; "\DIR\SUB", if it fits
        mov     ecx,[rcy_pcnt]
        call    names_len
        cmp     eax,UDR_DIRMAX - 2      ; the leading '\' and the NUL
        jae     rcy_plain
        mov     byte ptr [rcy_rec+UDR_DIR],'\'
        mov     edi,OFFSET rcy_rec+UDR_DIR+1
        call    names_string

        cmp     byte ptr [rcy_haslfn],0
        je      ydl_nolong
        mov     esi,OFFSET lfn_buf
        mov     edi,OFFSET rcy_rec+UDR_LONG
        mov     ecx,UDR_LONGMAX - 1
ydl_lcopy:
        mov     al,[esi]
        or      al,al
        jz      ydl_nolong
        mov     [edi],al
        inc     esi
        inc     edi
        loop    ydl_lcopy
ydl_nolong:

        ; how many clusters it holds, by its size
        mov     eax,[fat_spc]
        shl     eax,9
        mov     [rcy_clbytes],eax
        mov     eax,dword ptr [rcy_ent+DE_SIZE]
        call    rcy_clus
        mov     [rcy_need],eax

        call    rcy_open                ; \RECYCLED and its index
        jc      rcy_giveup
        call    rcy_scan
        jc      rcy_giveup

        ; THE ALLOWANCE.  First the cheap question: are there at least
        ; as many free clusters as the kept files, this one included,
        ; would need?
        mov     eax,[rcy_bin]
        add     eax,[rcy_need]
        call    rcy_want
        call    fat_freeatleast
        jnc     ydl_fits
        mov     [rcy_free],eax          ; no - and that is the whole count

        ; Would it fit with nothing else kept?  If not, it is bigger
        ; than the allowance and is not kept either.
        mov     eax,[rcy_need]
        call    rcy_want
        mov     edx,[rcy_free]
        add     edx,[rcy_bin]
        cmp     eax,edx
        ja      rcy_giveup

        ; then the oldest go, one at a time, until it does
ydl_purge:
        mov     eax,[rcy_old]
        or      eax,eax
        jz      rcy_giveup              ; nothing left to let go of
        call    rcy_purge               ; -> EAX = clusters freed
        jc      rcy_giveup
        add     [rcy_free],eax
        call    rcy_scan
        jc      rcy_giveup
        mov     eax,[rcy_bin]
        add     eax,[rcy_need]
        call    rcy_want
        cmp     eax,[rcy_free]
        ja      ydl_purge

ydl_fits:
        ; ITS NAME IN \RECYCLED: 'D', the sequence number in seven hex
        ; digits, and its own extension.  A file of that name may be
        ; there already - put in by hand, or left by a record that was
        ; since overwritten - so each candidate is looked for first.
        mov     dword ptr [rcy_tries],64
ydl_name:
        dec     dword ptr [rcy_tries]
        jz      rcy_giveup
        mov     eax,[rcy_seq]
        mov     dword ptr [rcy_rec+UDR_SEQ],eax
        inc     dword ptr [rcy_seq]
        call    rcy_mkname
        mov     eax,[rcy_rdir]
        call    ctx_chain
        mov     byte ptr [srch_attr],ATTR_DIR
        mov     dword ptr [srch_long],0
        mov     byte ptr [srch_lonly],0
        xor     ebx,ebx
        mov     edi,OFFSET rcy_rec+UDR_STORED
        call    fat_search
        jnc     ydl_name

        ; the record, and the header's next number, before the move -
        ; a record whose file never arrived is harmless, and UNDELETE
        ; says "missing"; a file with no record is space nobody can
        ; see
        mov     eax,[rcy_seq]
        mov     dword ptr [rcy_hdr+UDH_NEXTSEQ],eax
        xor     eax,eax
        mov     edi,OFFSET rcy_hdr
        mov     ecx,UDH_NEXTSEQ + 4
        call    rcy_write
        jc      rcy_giveup
        mov     eax,[rcy_slotrec]
        mov     edi,OFFSET rcy_rec
        mov     ecx,UDR_LEN
        call    rcy_write
        jc      rcy_giveup
        call    rcy_close

        ; THE MOVE.  The entry goes into \RECYCLED whole - clusters,
        ; size, stamp, attributes - with the new name over the old.
        mov     eax,[rcy_rdir]
        call    ctx_chain
        call    fat_findfree
        jc      ydl_unrecord
        mov     edi,esi
        push    esi
        mov     esi,OFFSET rcy_ent
        mov     ecx,8
        rep     movsd
        pop     edi
        mov     esi,OFFSET rcy_rec+UDR_STORED
        mov     ecx,11
        rep     movsb
        call    fat_dirupdate
        jc      ydl_ioerr

        ; ...and leaves where it was: long name first, then the entry
        mov     al,[rcy_disroot]
        mov     edx,[rcy_dstart]
        call    ctx_restore
        call    rcy_tomb
        jc      ydl_ioerr
        mov     byte ptr [esi],0E5h
        call    fat_dirupdate
        jc      ydl_ioerr
        call    fat_flush
        jc      ydl_ioerr
        clc
        ret

; \RECYCLED would not take the entry.  The record goes again and the
; file is deleted as it would have been without any of this.
ydl_unrecord:
        call    rcy_open
        jc      rcy_plain
        mov     byte ptr [rcy_rec+UDR_FLAGS],0
        mov     eax,[rcy_slotrec]
        mov     edi,OFFSET rcy_rec
        mov     ecx,1
        call    rcy_write
        jmp     short rcy_giveup
ydl_ioerr:
        mov     ax,ERR_ACCESS
        stc
        ret

; rcy_giveup - close the index if it is open, and delete the file -
;              unless keeping it failed because the disk refused, the
;              user was asked, and the answer was Fail.  The delete is
;              writes to the same disk, so trying it would only ask the
;              same question again; the caller gets "access denied",
;              which is what a plain delete answered Fail returns too.
rcy_giveup:
        call    rcy_close
        mov     eax,[crit_nfail]
        cmp     eax,[rcy_nfail]
        je      rcy_plain
        mov     ax,ERR_ACCESS
        stc
        ret

;---------------------------------------------------------------------
; rcy_plain - the delete there always was: the long name, the chain,
;             the entry.  Works from what rcy_delete saved, so it may
;             be reached after anything.
;---------------------------------------------------------------------
rcy_plain:
        mov     al,[rcy_disroot]
        mov     edx,[rcy_dstart]
        call    ctx_restore
        call    rcy_tomb
        jc      ypl_nofile
        call    de_getclus
        call    fat_freechain
        mov     byte ptr [esi],0E5h
        call    fat_dirupdate
        jc      ypl_deny
        call    fat_flush
        jc      ypl_deny
        mov     dword ptr [clusbuf_clus],-1     ; cached data now stale
        clc
        ret
ypl_nofile:
        mov     ax,ERR_NOFILE
        stc
        ret
ypl_deny:
        mov     ax,ERR_ACCESS
        stc
        ret

;---------------------------------------------------------------------
; rcy_tomb - the saved entry's long-name chain goes, and ESI -> the
;            entry itself.  The context must be its directory.  CF if
;            the entry cannot be reached.
;
; lfn_tomb works from what the last fat_search left, and a great deal
; has searched since, so the chain is put back from what was saved -
; the same repair f56_tomb makes for a rename.
;---------------------------------------------------------------------
rcy_tomb:
        mov     ebx,[rcy_slot]
        cmp     byte ptr [rcy_haslfn],0
        je      ytb_seek
        mov     eax,[rcy_lfirst]
        mov     [lfn_first],eax
        mov     byte ptr [lfn_ok],1
        call    lfn_tomb
ytb_seek:
        mov     ebx,[rcy_slot]
        call    dir_seek
        ret

;---------------------------------------------------------------------
; rcy_open - find \RECYCLED on the loaded volume, making it if need
;            be, then UNDELETE.DAT in it, making that too.  The index
;            is opened on rcy_sft, a handle that belongs to no program,
;            and its header is read into rcy_hdr.  CF if any of it
;            cannot be had.
;---------------------------------------------------------------------
rcy_open:
        mov     dword ptr [rcy_sft+SFT_FLAGS],0
        call    rcy_finddir
        jnc     yop_have
        mov     al,[cur_vol]            ; "X:\RECYCLED", hidden and system
        add     al,'A'
        mov     [rcy_mkpath],al
        mov     esi,OFFSET rcy_mkpath
        mov     byte ptr [mk_attr],ATTR_DIR OR ATTR_HIDDEN OR ATTR_SYSTEM
        call    mkdir_core
        jc      yop_fail
        call    rcy_finddir
        jc      yop_fail
yop_have:
        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jz      yop_fail                ; a FILE called RECYCLED
        call    de_getclus
        or      eax,eax
        jz      yop_fail
        mov     [rcy_rdir],eax
        call    ctx_chain

        mov     byte ptr [srch_attr],0
        mov     dword ptr [srch_long],0
        mov     byte ptr [srch_lonly],0
        xor     ebx,ebx
        mov     edi,OFFSET rcy_iname
        call    fat_search
        jnc     yop_open

        ; no index yet: an empty file, hidden and system like its
        ; directory
        call    fat_findfree
        jc      yop_fail
        push    esi
        mov     edi,esi
        xor     eax,eax
        mov     ecx,8
        rep     stosd
        pop     esi
        push    esi
        mov     edi,esi
        mov     esi,OFFSET rcy_iname
        mov     ecx,11
        rep     movsb
        pop     esi
        mov     byte ptr [esi+DE_ATTR],ATTR_ARCHIVE OR ATTR_HIDDEN OR ATTR_SYSTEM
        call    get_dostime
        mov     [esi+DE_TIME],ax
        call    get_dosdate
        mov     [esi+DE_DATE],ax
        call    fat_dirupdate
        jc      yop_fail

yop_open:
        ; the handle, filled in as open_core fills one
        mov     edi,OFFSET rcy_sft
        push    esi
        push    edi
        xor     eax,eax
        mov     ecx,SFT_LEN / 4
        rep     stosd
        pop     edi
        pop     esi
        mov     dword ptr [edi+SFT_FLAGS],SFTF_INUSE
        mov     dword ptr [edi+SFT_MODE],2
        call    de_getclus
        mov     [edi+SFT_CLUSTER],eax
        mov     [edi+SFT_CURCLUS],eax
        mov     eax,[esi+DE_SIZE]
        mov     [edi+SFT_SIZE],eax
        mov     eax,[esi+DE_TIME]
        mov     [edi+SFT_TIME],eax
        mov     [edi+SFT_DIRENT],ebx
        mov     esi,edi
        call    sft_setdir              ; the context is \RECYCLED

        ; A NEW INDEX GETS ITS HEADER; an old one must have ours
        cmp     dword ptr [rcy_sft+SFT_SIZE],UDR_LEN
        jae     yop_readhdr
        mov     edi,OFFSET rcy_hdr
        xor     eax,eax
        mov     ecx,UDR_LEN / 4
        rep     stosd
        mov     esi,OFFSET rcy_magic
        mov     edi,OFFSET rcy_hdr
        mov     ecx,8
        rep     movsb
        mov     word ptr [rcy_hdr+UDH_VERSION],1
        mov     word ptr [rcy_hdr+UDH_RECLEN],UDR_LEN
        mov     dword ptr [rcy_hdr+UDH_NEXTSEQ],1
        xor     eax,eax
        mov     edi,OFFSET rcy_hdr
        mov     ecx,UDR_LEN
        call    rcy_write
        jc      yop_fail
yop_readhdr:
        xor     eax,eax
        mov     edi,OFFSET rcy_hdr
        mov     ecx,UDH_NEXTSEQ + 4
        call    rcy_read
        jc      yop_fail
        mov     esi,OFFSET rcy_magic
        mov     edi,OFFSET rcy_hdr
        mov     ecx,8
        repe    cmpsb
        jne     yop_fail
        cmp     word ptr [rcy_hdr+UDH_RECLEN],UDR_LEN
        jne     yop_fail
        mov     eax,dword ptr [rcy_hdr+UDH_NEXTSEQ]
        mov     [rcy_seq],eax
        clc
        ret
yop_fail:
        call    rcy_close
        stc
        ret

; rcy_finddir - \RECYCLED in the root: ESI -> its entry, CF if absent
rcy_finddir:
        call    ctx_root
        mov     byte ptr [srch_attr],ATTR_DIR
        mov     dword ptr [srch_long],0
        mov     byte ptr [srch_lonly],0
        xor     ebx,ebx
        mov     edi,OFFSET rcy_dname
        call    fat_search
        ret

;---------------------------------------------------------------------
; rcy_close - put the index's size and chain back in its entry, if it
;             is open.  Registers preserved.
;---------------------------------------------------------------------
rcy_close:
        test    dword ptr [rcy_sft+SFT_FLAGS],SFTF_INUSE
        jz      ycl_out
        push    esi
        mov     esi,OFFSET rcy_sft
        call    sft_close
        pop     esi
ycl_out:
        ret

;---------------------------------------------------------------------
; rcy_read / rcy_write - ECX bytes at record EAX of the index, into or
;                        out of EDI.  CF if they could not all be moved.
;                        Registers preserved.
;---------------------------------------------------------------------
rcy_read:
        pushad
        imul    eax,UDR_LEN
        mov     esi,OFFSET rcy_sft
        mov     [esi+SFT_POS],eax
        push    ecx
        call    file_read
        pop     ecx
        jmp     short yrw_check
rcy_write:
        pushad
        imul    eax,UDR_LEN
        mov     esi,OFFSET rcy_sft
        mov     [esi+SFT_POS],eax
        push    ecx
        call    file_write
        pop     ecx
yrw_check:
        jc      yrw_fail
        cmp     edx,ecx
        jne     yrw_fail                ; end of file, or a full disk
        popad
        clc
        ret
yrw_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; rcy_scan - read the head of every record.
;   rcy_bin     = the clusters the kept files hold
;   rcy_old     = the record of the one deleted first, 0 for none
;   rcy_slotrec = where a new record goes: the first free one, or the
;                 end of the file
;   CF on a read error.
;---------------------------------------------------------------------
rcy_scan:
        mov     eax,dword ptr [rcy_sft+SFT_SIZE]
        xor     edx,edx
        mov     ecx,UDR_LEN
        div     ecx
        mov     [rcy_nrec],eax
        xor     eax,eax
        mov     [rcy_bin],eax
        mov     [rcy_old],eax
        mov     [rcy_slotrec],eax       ; 0 = none free yet (0 is the header)
        mov     dword ptr [rcy_oldseq],-1
        mov     ebx,1
ysc_lp:
        cmp     ebx,[rcy_nrec]
        jae     ysc_done
        mov     eax,ebx
        mov     edi,OFFSET rcy_head
        mov     ecx,UDR_SIZE + 4
        call    rcy_read
        jc      ysc_fail
        test    byte ptr [rcy_head+UDR_FLAGS],UDRF_USED
        jnz     ysc_used
        cmp     dword ptr [rcy_slotrec],0
        jne     ysc_next
        mov     [rcy_slotrec],ebx
        jmp     short ysc_next
ysc_used:
        mov     eax,dword ptr [rcy_head+UDR_SIZE]
        call    rcy_clus
        add     [rcy_bin],eax
        mov     eax,dword ptr [rcy_head+UDR_SEQ]
        cmp     eax,[rcy_oldseq]
        jae     ysc_next
        mov     [rcy_oldseq],eax
        mov     [rcy_old],ebx
ysc_next:
        inc     ebx
        jmp     short ysc_lp
ysc_done:
        cmp     dword ptr [rcy_slotrec],0
        jne     ysc_ok
        mov     eax,[rcy_nrec]
        mov     [rcy_slotrec],eax
ysc_ok:
        clc
        ret
ysc_fail:
        stc
        ret

;---------------------------------------------------------------------
; rcy_purge - the kept file of record EAX goes for good, and so does
;             its record.  -> EAX = the clusters that freed (0 if the
;             file was not there to free), CF on an I/O error.
;---------------------------------------------------------------------
rcy_purge:
        mov     [rcy_pidx],eax
        mov     dword ptr [rcy_pfreed],0
        mov     edi,OFFSET rcy_head
        mov     ecx,UDR_STORED + 11
        call    rcy_read
        jc      ypg_fail

        mov     eax,[rcy_rdir]
        call    ctx_chain
        mov     byte ptr [srch_attr],0
        mov     dword ptr [srch_long],0
        mov     byte ptr [srch_lonly],0
        xor     ebx,ebx
        mov     edi,OFFSET rcy_head+UDR_STORED
        call    fat_search
        jc      ypg_record              ; gone already: only the record
        call    de_getclus
        call    fat_freechain
        mov     byte ptr [esi],0E5h
        call    fat_dirupdate
        jc      ypg_fail
        mov     dword ptr [clusbuf_clus],-1
        mov     eax,dword ptr [rcy_head+UDR_SIZE]
        call    rcy_clus
        mov     [rcy_pfreed],eax
ypg_record:
        mov     byte ptr [rcy_head+UDR_FLAGS],0
        mov     eax,[rcy_pidx]
        mov     edi,OFFSET rcy_head
        mov     ecx,1
        call    rcy_write
        jc      ypg_fail
        mov     eax,[rcy_pfreed]
        clc
        ret
ypg_fail:
        stc
        ret

;---------------------------------------------------------------------
; rcy_mkname - rcy_rec's UDR_STORED from the number in EAX and the
;              extension of rcy_ent: "D" + seven hex digits + the ext.
;---------------------------------------------------------------------
rcy_mkname:
        pushad
        mov     edi,OFFSET rcy_rec+UDR_STORED
        mov     byte ptr [edi],'D'
        mov     edx,eax
        shl     edx,4                   ; seven digits: the top nibble goes
        mov     ecx,7
ymn_lp:
        rol     edx,4
        mov     al,dl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ymn_dig
        add     al,'A' - '9' - 1
ymn_dig:
        inc     edi
        mov     [edi],al
        loop    ymn_lp
        mov     ax,word ptr [rcy_ent+8]
        mov     word ptr [rcy_rec+UDR_STORED+8],ax
        mov     al,[rcy_ent+10]
        mov     [rcy_rec+UDR_STORED+10],al
        popad
        ret

;---------------------------------------------------------------------
; rcy_clus - EAX = a size in bytes -> EAX = the clusters it takes on
;            this volume.  [rcy_clbytes] must be set.
;---------------------------------------------------------------------
rcy_clus:
        push    ecx
        push    edx
        xor     edx,edx
        mov     ecx,[rcy_clbytes]
        div     ecx
        or      edx,edx
        jz      ycs_out
        inc     eax
ycs_out:
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; rcy_want - EAX = clusters kept -> EAX = the free clusters there must
;            be for that to be within the allowance:
;            ceil( kept * (100 - PCT) / PCT ).  Cannot overflow - a
;            FAT32 volume has under 2^28 clusters.
;---------------------------------------------------------------------
rcy_want:
        push    ecx
        push    edx
        mov     ecx,100 - UNDEL_PCT
        mul     ecx
        mov     ecx,UNDEL_PCT
        div     ecx
        or      edx,edx
        jz      ywt_out
        inc     eax
ywt_out:
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; fat_freeatleast - EAX = a number of clusters.  CF clear if the
;                   loaded volume has at least that many free; the
;                   count stops the moment it knows.  CF set with EAX
;                   = the whole free count if it has not.
;---------------------------------------------------------------------
fat_freeatleast:
        push    ebx
        push    ecx
        push    edx
        push    esi
        mov     esi,eax                 ; wanted
        xor     ebx,ebx                 ; found
        or      esi,esi
        jz      yfa_yes
        mov     ecx,2
yfa_lp:
        mov     eax,ecx
        call    fat_next
        or      eax,eax
        jnz     yfa_used
        inc     ebx
        cmp     ebx,esi
        jae     yfa_yes
yfa_used:
        inc     ecx
        mov     eax,[fat_totclus]
        add     eax,2
        cmp     ecx,eax
        jb      yfa_lp
        mov     eax,ebx
        stc
        jmp     short yfa_out
yfa_yes:
        clc
yfa_out:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; State
;=====================================================================
undel_on    db  1                       ; UNDELETE=ON|OFF
rcy_perm    db  0                       ; this delete is not to be kept
rcy_disroot db  0                       ; the file's directory, as
rcy_haslfn  db  0                       ; ctx_save gives it, and whether
rcy_dstart  dd  0                       ; it had a long name
rcy_lfirst  dd  0                       ; ...whose chain starts here
rcy_slot    dd  0                       ; the entry's index
rcy_pnames  dd  0                       ; the directory's path, as the
rcy_pcnt    dd  0                       ; caller has it
rcy_rdir    dd  0                       ; \RECYCLED's first cluster
rcy_clbytes dd  0
rcy_need    dd  0                       ; clusters: this file,
rcy_bin     dd  0                       ; ...the kept ones,
rcy_free    dd  0                       ; ...and the free ones
rcy_nrec    dd  0                       ; records, the header included
rcy_old     dd  0                       ; the oldest kept file's record
rcy_oldseq  dd  0
rcy_slotrec dd  0                       ; where the new record goes
rcy_seq     dd  0                       ; the next sequence number
rcy_tries   dd  0
rcy_pidx    dd  0
rcy_pfreed  dd  0
rcy_nfail   dd  0                       ; crit_nfail when this delete began

rcy_temps   db  'TMP', '$$$', 0         ; extensions that are never kept
rcy_dname   db  'RECYCLED   '
rcy_iname   db  'UNDELETEDAT'
rcy_magic   db  'PMUNDEL', 1Ah
rcy_mkpath  db  'X:\RECYCLED', 0
            db  0,0,0
rcy_ent     db  32 dup(0)               ; the entry, as it was
rcy_head    db  32 dup(0)               ; one record's head, scanning
rcy_sft     db  SFT_LEN dup(0)          ; the index's handle
rcy_rec     db  UDR_LEN dup(0)          ; the record being written
rcy_hdr     db  UDR_LEN dup(0)          ; the index's header
