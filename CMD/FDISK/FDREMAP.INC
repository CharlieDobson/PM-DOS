;=====================================================================
; FDREMAP.INC - CLUSTER ADJUSTMENT.  Included by FDISK.ASM.
;
; A resize whose new size the volume's own cluster size cannot express
; - or not at the volume's own FAT width - used to stop at RL_REMAP or
; RL_FATTYPE.  These are the three things done instead, and rm_choose
; takes the nearest that works:
;
;   RM_WIDTH   the same cluster size, FAT12 <-> FAT16.  Cluster numbers
;              survive, so nothing but the FAT changes: it is written
;              again at the other width.  A FAT12 volume growing past
;              4,084 clusters becomes FAT16 and a FAT16 one shrinking
;              below 4,085 becomes FAT12, which is what FORMAT would
;              have made of either size.
;   RM_SPLIT   smaller clusters.  Each old cluster becomes k new ones in
;              the same sectors, so NO DATA MOVES: what changes is every
;              number - every chain in the FAT, and the first cluster in
;              every directory entry.  FAT32 meets this shrinking below
;              65,525 clusters.
;   RM_MERGE   larger clusters.  k old clusters become one new one, and
;              one new cluster cannot hold two files - so the files are
;              PACKED: each chain is laid down again from the front of
;              the data area, its pieces in order, on whole new clusters.
;              FAT16 meets this growing past 65,524 clusters.
;
; THE PLAN IS MADE IN MEMORY, AND THE VOLUME IS CHECKED BEFORE ANYTHING
; IS WRITTEN.  The whole FAT is read into a table of 32-bit entries with
; the width forgotten (rm_decode), and the volume has to hold together:
; every chain ending, no cluster in two chains, no chain that loops,
; every directory entry naming the first cluster of a chain no other
; entry names.  A volume that does not is refused as damaged, with
; CHKDSK as the answer - renumbering chains that are already wrong
; would make them wrong in a new way, and nobody could then tell which.
;
; THE MEMORY IS 48h's.  The tables cost four bytes a cluster and more:
; 256K for the largest FAT16 volume and megabytes for FAT32, past the
; 512K a program's slot starts with - and 48h grows the slot (RING3.INC,
; slot_grow).  One block, kept between plans and replaced when a bigger
; volume needs more, because the screen plans every change twice.
;
; NOTHING THAT RENUMBERS A VOLUME IN PLACE SURVIVES A LOST POWER SUPPLY
; half way through: the FAT would describe one layout and the data area
; hold parts of two.  The partition table is still written last, and the
; screen's question says not to turn the machine off.
;=====================================================================

;---------------------------------------------------------------------
; rm_choose - a cluster size and width that make pl_nlen a volume
;             holding what is stored, nearest the volume's own.  EAX =
;             the refusal keeping the cluster size earned.  CF clear:
;             fg_exec, rm_mode and rm_k are set.  CF set: EAX = the
;             refusal - the one handed in, unless bad clusters were all
;             that stopped a larger cluster.
;
; NEAREST, because the rule is that the clusters change only when they
; have to: the same cluster size at the other width first, then one
; halving or doubling, then two - in the direction the resize goes
; first, since a volume growing out of its clusters needs bigger ones.
; FAT12 and FAT16 trade places freely; FAT32 stays FAT32.  Converting
; between FAT16 and FAT32 moves the root directory into the data area
; or out of it, and that is a conversion, not a resize.
;---------------------------------------------------------------------
rm_choose:
        pushad
        mov     [rm_why],eax
        mov     byte ptr [rm_badhit],0
        mov     byte ptr [rm_fithit],0
        cmp     dword ptr [pl_op],OP_GROW
        je      rmc_go
        cmp     dword ptr [pl_op],OP_SHRINK
        jne     rmc_none                ; a move keeps its length, and so
rmc_go:                                 ; its layout
        xor     ebp,ebp                 ; EBP = how many doublings away
rmc_dist:
        cmp     ebp,7
        jae     rmc_none
        xor     esi,esi                 ; ESI = 0: the resize's own way first
rmc_way:
        cmp     esi,2
        jae     rmc_far
        mov     eax,esi                 ; up on pass 0 when growing, and on
        cmp     dword ptr [pl_op],OP_GROW ; pass 1 when shrinking
        je      rmc_wayset
        xor     eax,1
rmc_wayset:
        mov     ecx,ebp
        or      eax,eax
        jnz     rmc_smaller
        mov     eax,[fv_spc]
        shl     eax,cl
        cmp     eax,RM_SPCMAX
        ja      rmc_skip
        jmp     short rmc_try
rmc_smaller:
        or      ebp,ebp
        jz      rmc_skip                ; distance nought is looked at once
        mov     eax,[fv_spc]
        shr     eax,cl
        jz      rmc_skip
        mov     edx,eax
        shl     edx,cl
        cmp     edx,[fv_spc]
        jne     rmc_skip
rmc_try:
        call    rmc_one
        jnc     rmc_found
rmc_skip:
        inc     esi
        jmp     short rmc_way
rmc_far:
        inc     ebp
        jmp     short rmc_dist
rmc_found:
        popad
        clc
        ret
rmc_none:
        ; WHY NOTHING DID, in the order that helps: a larger cluster that
        ; would have done but for bad ones; then a size some cluster can
        ; make but what is stored does not fit - which is TOOSMALL, and
        ; not the width the cluster count alone would suggest
        cmp     byte ptr [rm_badhit],0
        je      rmc_fit
        mov     dword ptr [rm_why],RL_BADCLUS
        jmp     short rmc_say
rmc_fit:
        cmp     byte ptr [rm_fithit],0
        je      rmc_say
        mov     dword ptr [rm_why],RL_TOOSMALL
rmc_say:
        popad
        mov     eax,[rm_why]
        stc
        ret

;--- rmc_one - EAX = a cluster size.  CF clear, with fg_exec, rm_mode
;--- and rm_k set, if the new size takes it.
rmc_one:
        pushad
        mov     [rm_spc],eax
        mov     ecx,eax
        mov     eax,[pl_nlen]
        mov     edi,OFFSET rm_try
        call    fg_forspc
        jc      roe_no
        cmp     dword ptr [rm_try+FG_VALID],0
        je      roe_no
        mov     eax,dword ptr [rm_try+FG_BITS]
        cmp     dword ptr [fv_bits],32
        jne     roe_small
        cmp     eax,32                  ; FAT32 stays FAT32...
        jne     roe_no
        jmp     short roe_family
roe_small:
        cmp     eax,32                  ; ...and FAT12 and FAT16 stay out
        je      roe_no                  ; of it
roe_family:
        mov     edx,[rm_spc]
        cmp     edx,[fv_spc]
        jne     roe_fits
        cmp     eax,[fv_bits]
        je      roe_no                  ; the volume's own, which failed
roe_fits:
        ; WHAT IS STORED HAS TO FIT.  At the same cluster size or a
        ; smaller one it stays where it is, so the clusters have to reach
        ; its last sector.  At a larger one it is packed - rm_links
        ; measures that - and here only the first step has to fit: the
        ; data area moved whole, as far as its last sector in use
        mov     ecx,[fv_usedsec]
        cmp     edx,[fv_spc]
        ja      roe_merge
        cmp     ecx,dword ptr [rm_try+FG_CAP]
        ja      roe_nofit
        jmp     short roe_take
roe_merge:
        cmp     ecx,dword ptr [rm_try+FG_DATA]
        ja      roe_nofit
        cmp     dword ptr [fv_nbad],0
        je      roe_take
        ; A BAD CLUSTER CANNOT BE PART OF A LARGER ONE that holds a file,
        ; and packing puts files everywhere.  It would have done but for
        ; those, which is what the refusal says if nothing else does
        mov     byte ptr [rm_badhit],1
        jmp     short roe_no
roe_take:
        mov     esi,OFFSET rm_try
        mov     edi,OFFSET fg_exec
        mov     ecx,FG_LEN / 4
        rep     movsd
        mov     eax,[rm_spc]
        mov     byte ptr [rm_mode],RM_WIDTH
        mov     dword ptr [rm_k],1
        cmp     eax,[fv_spc]
        je      roe_out
        jb      roe_split
        xor     edx,edx
        div     dword ptr [fv_spc]
        mov     [rm_k],eax
        mov     byte ptr [rm_mode],RM_MERGE
        jmp     short roe_out
roe_split:
        mov     ecx,eax
        mov     eax,[fv_spc]
        xor     edx,edx
        div     ecx
        mov     [rm_k],eax
        mov     byte ptr [rm_mode],RM_SPLIT
roe_out:
        popad
        clc
        ret
roe_nofit:
        mov     byte ptr [rm_fithit],1  ; a volume of that size, but too small
roe_no:                                 ; for what is stored
        popad
        stc
        ret

;---------------------------------------------------------------------
; rm_prepare - the tables, read and checked, and for RM_MERGE the
;              packing worked out.  CF set with EAX = the refusal.
;
; rm_w      a dword an old cluster: its FAT entry, as rm_decode left it
; rm_x      merging, a dword an old cluster: the slot of the new data
;           area it goes to, a slot being one old cluster's worth;
;           splitting, a byte an old cluster: how many of its new ones
;           a file's last cluster keeps, nought meaning all of them
; rm_bref   a bit an old cluster: another cluster's entry leads here
; rm_bdir   ...it is the first cluster of a directory
; rm_bdone  ...that directory has been read; at the move, that cluster
;           has been lifted
; rm_bclm   ...a directory entry names it
; rm_bend   merging, a bit a NEW cluster: the last of its chain
;---------------------------------------------------------------------
rm_prepare:
        pushad
        mov     eax,[fv_clusters]
        add     eax,2
        mov     [rm_nold],eax
        mov     eax,dword ptr [fg_exec+FG_CLUS]
        add     eax,2
        mov     [rm_nnew],eax
        call    rr_layout               ; where the regions are, both ways

        mov     eax,[rm_nold]
        shl     eax,2
        mov     [rm_szw],eax
        xor     ecx,ecx
        cmp     byte ptr [rm_mode],RM_WIDTH
        je      rpr_x
        mov     ecx,[rm_nold]
        add     ecx,3
        and     ecx,NOT 3
        cmp     byte ptr [rm_mode],RM_MERGE
        jne     rpr_x
        mov     ecx,[rm_nold]
        shl     ecx,2
rpr_x:
        mov     [rm_szx],ecx
        mov     eax,[rm_nold]
        add     eax,31
        shr     eax,5
        shl     eax,2
        mov     [rm_szb],eax
        xor     edx,edx
        cmp     byte ptr [rm_mode],RM_MERGE
        jne     rpr_e
        mov     edx,[rm_nnew]
        add     edx,31
        shr     edx,5
        shl     edx,2
rpr_e:
        mov     [rm_sze],edx

        ; the sum, which a FAT32 volume of a size nobody has could make
        ; wrap - refused rather than allocated short
        mov     eax,[rm_szw]
        add     eax,[rm_szx]
        jc      rpr_nomem
        mov     ecx,[rm_szb]
        shl     ecx,2
        add     eax,ecx
        jc      rpr_nomem
        add     eax,[rm_sze]
        jc      rpr_nomem
        call    rm_need                 ; -> EDI
        jc      rpr_nomem

        mov     [rm_w],edi
        add     edi,[rm_szw]
        mov     [rm_x],edi
        add     edi,[rm_szx]
        mov     [rm_bref],edi
        add     edi,[rm_szb]
        mov     [rm_bdir],edi
        add     edi,[rm_szb]
        mov     [rm_bdone],edi
        add     edi,[rm_szb]
        mov     [rm_bclm],edi
        add     edi,[rm_szb]
        mov     [rm_bend],edi
        ; everything past the FAT starts at nought
        mov     edi,[rm_x]
        mov     ecx,[rm_szb]
        shl     ecx,2
        add     ecx,[rm_szx]
        add     ecx,[rm_sze]
        shr     ecx,2
        xor     eax,eax
        rep     stosd

        call    rm_decode
        jc      rpr_refused
        cmp     byte ptr [rm_mode],RM_WIDTH
        je      rpr_ok                  ; the numbers survive: nothing to walk
        call    rm_links
        jc      rpr_refused
        call    rm_dirscan
        jc      rpr_refused
rpr_ok:
        popad
        clc
        ret
rpr_nomem:
        mov     eax,RL_NOMEM
rpr_refused:
        mov     [rm_why],eax
        popad
        mov     eax,[rm_why]
        stc
        ret

;--- rm_need - EAX = bytes -> EDI = a block that big, from 48h.  CF if
;--- there is not that much.  The block is kept for the next plan, and
;--- given back for a bigger one.
rm_need:
        push    eax
        push    ebx
        push    edx
        cmp     eax,[rm_memsz]
        jbe     rnd_have
        mov     ebx,eax
        mov     edx,[rm_mem]
        or      edx,edx
        jz      rnd_new
        push    ebx
        mov     ah,49h
        int     21h
        pop     ebx
        mov     dword ptr [rm_mem],0
        mov     dword ptr [rm_memsz],0
rnd_new:
        push    ebx
        mov     ah,48h
        int     21h
        pop     ebx
        jc      rnd_fail
        mov     [rm_mem],eax
        mov     [rm_memsz],ebx
rnd_have:
        mov     edi,[rm_mem]
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret
rnd_fail:
        pop     edx
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; rm_decode - the FAT the volume is using, into rm_w: a dword an entry,
;             nought for free, RM_EOC, RM_BADCL, or the next cluster.
;             Entries 0 and 1 are kept as they were.  CF set with EAX =
;             the refusal: a value no width allows is damage.
;---------------------------------------------------------------------
rm_decode:
        pushad
        mov     dword ptr [rm_nbad],0
        mov     eax,0FF8h               ; the first end-of-chain value of
        cmp     dword ptr [fv_bits],12  ; this width
        je      rmd_eoc
        mov     eax,0FFF8h
        cmp     dword ptr [fv_bits],16
        je      rmd_eoc
        mov     eax,0FFFFFF8h
rmd_eoc:
        mov     [rm_oeoc],eax
        ; FAT32 WITH MIRRORING OFF names the one FAT it keeps up to date
        mov     ebx,[rr_ofat]
        cmp     dword ptr [fv_bits],32
        jne     rmd_where
        mov     eax,[fv_extflags]
        test    eax,80h
        jz      rmd_where
        and     eax,0Fh
        cmp     eax,[fv_nfats]
        jae     rmd_where
        imul    eax,[fv_spf]
        add     ebx,eax
rmd_where:
        mov     [rmd_lba],ebx
        mov     edi,[rm_w]
        xor     ebp,ebp                 ; EBP = the entry
        cmp     dword ptr [fv_bits],12
        je      rmd_12

        ; sixteen or thirty-two bits: a sector at a time - and a FAT too
        ; short for the clusters the BPB claims is damage, not something
        ; to read past the end of
        mov     ecx,256
        cmp     dword ptr [fv_bits],16
        je      rmd_epb
        mov     ecx,128
rmd_epb:
        mov     [rmd_per],ecx
        mov     eax,[fv_spf]
        mul     ecx
        or      edx,edx
        jnz     rmd_sec
        cmp     eax,[rm_nold]
        jb      rmd_damaged
rmd_sec:
        cmp     ebp,[rm_nold]
        jae     rmd_ok
        mov     eax,ebp
        xor     edx,edx
        div     dword ptr [rmd_per]     ; EAX = the sector, EDX = the entry
        push    edx
        mov     ebx,[rmd_lba]
        add     ebx,eax
        mov     al,[pl_drive]
        mov     ecx,1
        push    edi
        mov     edi,[sec_buf]
        call    dk_read
        pop     edi
        pop     edx
        jc      rmd_rderr
        mov     esi,[sec_buf]
rmd_ent:
        cmp     ebp,[rm_nold]
        jae     rmd_ok
        cmp     edx,[rmd_per]
        jae     rmd_sec
        cmp     dword ptr [fv_bits],16
        jne     rmd_32
        movzx   eax,word ptr [esi+edx*2]
        jmp     short rmd_one
rmd_32:
        mov     eax,[esi+edx*4]
        and     eax,0FFFFFFFh           ; the top four bits are reserved
rmd_one:
        call    rmd_norm
        jc      rmd_damaged
        mov     [edi+ebp*4],eax
        inc     ebp
        inc     edx
        jmp     short rmd_ent

        ; twelve bits straddle sectors, so the FAT is read whole - into
        ; the copy buffer, as fv_scanused reads it
rmd_12:
        mov     ecx,[fv_spf]
        cmp     ecx,RC_CHUNK
        ja      rmd_damaged
        mov     eax,ecx
        shl     eax,9
        mov     edx,[rm_nold]
        lea     edx,[edx*2+edx]
        inc     edx
        shr     edx,1                   ; the bytes the entries need
        cmp     eax,edx
        jb      rmd_damaged
        mov     al,[pl_drive]
        mov     ebx,[rmd_lba]
        push    edi
        mov     edi,[cpy_buf]
        call    dk_read
        pop     edi
        jc      rmd_rderr
rmd_12one:
        cmp     ebp,[rm_nold]
        jae     rmd_ok
        mov     eax,ebp
        call    fat12_get
        call    rmd_norm
        jc      rmd_damaged
        mov     [edi+ebp*4],eax
        inc     ebp
        jmp     short rmd_12one

rmd_ok:
        mov     edi,[rm_w]
        mov     eax,[edi]
        mov     [rm_e0],eax
        mov     eax,[edi+4]
        mov     [rm_e1],eax
        popad
        clc
        ret
rmd_rderr:
        mov     eax,RL_READERR
        jmp     short rmd_fail
rmd_damaged:
        mov     eax,RL_DAMAGED
rmd_fail:
        mov     [rm_why],eax
        popad
        mov     eax,[rm_why]
        stc
        ret

;--- rmd_norm - EAX = an entry as stored, EBP = its cluster -> EAX as
;--- rm_w holds it.  CF for a value that names no cluster.
rmd_norm:
        cmp     ebp,2
        jb      rmn_ok                  ; 0 and 1 are not clusters: kept
        or      eax,eax
        jz      rmn_ok
        cmp     eax,[rm_oeoc]
        jae     rmn_eoc
        inc     eax
        cmp     eax,[rm_oeoc]
        je      rmn_bad                 ; one below the end marks is bad
        dec     eax
        cmp     eax,2
        jb      rmn_no
        cmp     eax,[rm_nold]
        jae     rmn_no                  ; past the last cluster, or one of
rmn_ok:                                 ; the reserved values
        clc
        ret
rmn_eoc:
        mov     eax,RM_EOC
        clc
        ret
rmn_bad:
        inc     dword ptr [rm_nbad]
        mov     eax,RM_BADCL
        clc
        ret
rmn_no:
        stc
        ret

;---------------------------------------------------------------------
; rm_links - the chains, checked: every link leads to a cluster in use,
;            and no cluster is where two links lead.  Then every chain
;            is walked from its head - the cluster nothing leads to -
;            and the walks have to account for every cluster in use: a
;            chain that loops has no head, and is left over.
;
; MERGING, THE WALK IS ALSO THE PACKING.  Each chain, in the order of
; its head, gets the next whole new clusters, its pieces in order: rm_x
; says which slot each old cluster goes to, and rm_bend marks the last
; new cluster of each chain.  In head order, a volume written front to
; back mostly packs into the place it is already in.
;---------------------------------------------------------------------
rm_links:
        pushad
        mov     esi,[rm_w]
        mov     edi,[rm_bref]
        xor     ebx,ebx                 ; EBX = clusters in use
        mov     ebp,2
rml_ref:
        cmp     ebp,[rm_nold]
        jae     rml_walk
        mov     eax,[esi+ebp*4]
        or      eax,eax
        jz      rml_rnext
        cmp     eax,RM_BADCL
        je      rml_rnext
        inc     ebx
        cmp     eax,RM_EOC
        je      rml_rnext
        mov     edx,[esi+eax*4]         ; where it leads is in use...
        or      edx,edx
        jz      rml_damaged
        cmp     edx,RM_BADCL
        je      rml_damaged
        bts     dword ptr [edi],eax     ; ...and nowhere else leads there
        jc      rml_damaged
rml_rnext:
        inc     ebp
        jmp     short rml_ref

rml_walk:
        mov     [rm_nused],ebx
        mov     dword ptr [rm_walked],0
        mov     dword ptr [rm_alloc],2
        mov     ebp,2                   ; EBP = a head, if nothing leads here
rml_head:
        cmp     ebp,[rm_nold]
        jae     rml_count
        mov     eax,[esi+ebp*4]
        or      eax,eax
        jz      rml_hnext
        cmp     eax,RM_BADCL
        je      rml_hnext
        bt      dword ptr [edi],ebp
        jc      rml_hnext
        call    rml_chain
        jc      rml_fail
rml_hnext:
        inc     ebp
        jmp     short rml_head
rml_count:
        mov     eax,[rm_walked]
        cmp     eax,[rm_nused]
        jne     rml_damaged             ; a loop, which no head leads into
        popad
        clc
        ret
rml_damaged:
        mov     eax,RL_DAMAGED
rml_fail:
        mov     [rm_why],eax
        popad
        mov     eax,[rm_why]
        stc
        ret

;--- rml_chain - the chain whose head is EBP, counted into rm_walked
;--- and, merging, packed.  CF: EAX = the refusal.
rml_chain:
        pushad
        mov     esi,[rm_w]
        mov     edi,[rm_x]
        mov     ecx,ebp                 ; ECX = the cluster
        xor     ebx,ebx                 ; EBX = its place in the chain
        mov     eax,[rm_alloc]
        sub     eax,2
        mul     dword ptr [rm_k]
        mov     [rml_slot0],eax         ; the first slot this chain gets
rmh_one:
        inc     dword ptr [rm_walked]
        mov     eax,[rm_walked]
        cmp     eax,[rm_nused]
        ja      rmh_damaged             ; more than there are: it loops
        cmp     byte ptr [rm_mode],RM_MERGE
        jne     rmh_next
        mov     eax,[rml_slot0]
        add     eax,ebx
        mov     [edi+ecx*4],eax
rmh_next:
        inc     ebx
        mov     eax,[esi+ecx*4]
        cmp     eax,RM_EOC
        je      rmh_end
        mov     ecx,eax
        jmp     short rmh_one
rmh_end:
        cmp     byte ptr [rm_mode],RM_MERGE
        jne     rmh_ok
        mov     eax,ebx                 ; the chain in whole new clusters
        add     eax,[rm_k]
        dec     eax
        xor     edx,edx
        div     dword ptr [rm_k]
        add     eax,[rm_alloc]
        cmp     eax,[rm_nnew]
        ja      rmh_toosmall            ; packed, it still does not fit
        mov     [rm_alloc],eax
        dec     eax
        mov     edx,[rm_bend]
        bts     dword ptr [edx],eax
rmh_ok:
        popad
        clc
        ret
rmh_toosmall:
        mov     eax,RL_TOOSMALL
        jmp     short rmh_fail
rmh_damaged:
        mov     eax,RL_DAMAGED
rmh_fail:
        mov     [rm_why],eax
        popad
        mov     eax,[rm_why]
        stc
        ret

;---------------------------------------------------------------------
; rm_dirscan - every directory, read where it is now, and every entry
;              in it checked: a file or directory names the head of a
;              chain, and no two name the same one.  CF: EAX = refusal.
;
; NOTHING RECURSES.  A directory is marked when an entry names it
; (rm_bdir), and the marked ones are read in passes until a pass finds
; none it has not read (rm_bdone) - so no depth is too deep, and a
; directory that names its own ancestor is caught as a cluster named
; twice rather than followed for ever.
;
; SPLITTING, each file's last cluster learns how many of its new
; clusters the file's length needs.  The rest are let go, because a
; chain longer than its file is what CHKDSK calls an allocation error.
;---------------------------------------------------------------------
rm_dirscan:
        pushad
        mov     byte ptr [rm_fix],0
        cmp     dword ptr [fv_bits],32
        jne     rds_region
        ; FAT32: the root is a chain like any directory, named by the BPB
        mov     eax,[fv_rootclus]
        mov     dl,ATTR_DIR
        call    rm_named
        jc      rds_damaged
        jmp     short rds_passes
rds_region:
        mov     ebx,[rr_oroot]
        mov     ecx,[fv_rootsec]
        call    rm_region
        jc      rds_fail
rds_passes:
        mov     byte ptr [rds_found],0
        mov     ebp,2
rds_scan:
        cmp     ebp,[rm_nold]
        jae     rds_passend
        mov     edi,[rm_bdir]
        bt      dword ptr [edi],ebp
        jnc     rds_snext
        mov     edi,[rm_bdone]
        bts     dword ptr [edi],ebp
        jc      rds_snext
        mov     byte ptr [rds_found],1
        mov     eax,ebp
        call    rm_dirchain
        jc      rds_fail
rds_snext:
        inc     ebp
        jmp     short rds_scan
rds_passend:
        cmp     byte ptr [rds_found],0
        jne     rds_passes
        popad
        clc
        ret
rds_damaged:
        mov     eax,RL_DAMAGED
rds_fail:
        mov     [rm_why],eax
        popad
        mov     eax,[rm_why]
        stc
        ret

;--- rm_named - EAX = a cluster an entry names, DL = the entry's
;--- attributes.  It has to be the head of a chain in use that nothing
;--- else names; it is claimed, and a directory's is marked to be read.
;--- CF when it is not.
rm_named:
        push    edi
        cmp     eax,2
        jb      rnm_no
        cmp     eax,[rm_nold]
        jae     rnm_no
        mov     edi,[rm_w]
        mov     edi,[edi+eax*4]
        or      edi,edi
        jz      rnm_no                  ; free
        cmp     edi,RM_BADCL
        je      rnm_no
        mov     edi,[rm_bref]
        bt      dword ptr [edi],eax
        jc      rnm_no                  ; the middle of somebody's chain
        mov     edi,[rm_bclm]
        bts     dword ptr [edi],eax
        jc      rnm_no                  ; named twice
        test    dl,ATTR_DIR
        jz      rnm_yes
        mov     edi,[rm_bdir]
        bts     dword ptr [edi],eax
rnm_yes:
        pop     edi
        clc
        ret
rnm_no:
        pop     edi
        stc
        ret

;--- rm_region - the root directory of a FAT12 or FAT16 volume: ECX
;--- sectors at EBX, in pieces the copy buffer holds, checked or (with
;--- rm_fix) rewritten.  CF: EAX = refusal.
rm_region:
        pushad
        mov     byte ptr [rm_dirend],0
        mov     [rrg_left],ecx
rrg_piece:
        mov     ecx,[rrg_left]
        or      ecx,ecx
        jz      rrg_ok
        cmp     byte ptr [rm_dirend],0
        jne     rrg_ok                  ; the end mark: nothing after it
        cmp     ecx,RC_CHUNK
        jbe     rrg_have
        mov     ecx,RC_CHUNK
rrg_have:
        sub     [rrg_left],ecx
        mov     al,[pl_drive]
        mov     edi,[cpy_buf]
        call    dk_read
        jc      rrg_rderr
        push    ecx
        mov     esi,[cpy_buf]
        shl     ecx,4                   ; sixteen entries a sector
        call    rm_entries
        pop     ecx
        jc      rrg_fail
        cmp     byte ptr [rm_fix],0
        je      rrg_next
        mov     al,[pl_drive]
        mov     edi,[cpy_buf]
        call    dk_write
        jc      rrg_wrerr
rrg_next:
        add     ebx,ecx
        jmp     short rrg_piece
rrg_ok:
        popad
        clc
        ret
rrg_rderr:
        mov     eax,RL_READERR
        jmp     short rrg_fail
rrg_wrerr:
        mov     eax,RL_WRITEERR
rrg_fail:
        mov     [rm_why],eax
        popad
        mov     eax,[rm_why]
        stc
        ret

;--- rm_dirchain - the directory whose first cluster is EAX, a cluster
;--- at a time: read, its entries checked or rewritten, and written back
;--- when rewritten.  After the end mark the chain is still followed,
;--- unread, to its last cluster.  CF: EAX = refusal.
rm_dirchain:
        pushad
        mov     byte ptr [rm_dirend],0
        mov     ebp,eax                 ; EBP = the cluster
        mov     dword ptr [rdc_steps],0
rdc_one:
        inc     dword ptr [rdc_steps]
        mov     eax,[rdc_steps]
        cmp     eax,[rm_nold]
        ja      rdc_damaged
        cmp     byte ptr [rm_dirend],0
        jne     rdc_follow
        call    rm_dirlba               ; -> EBX
        mov     al,[pl_drive]
        mov     ecx,[fv_spc]
        mov     edi,[cpy_buf]
        call    dk_read
        jc      rdc_rderr
        mov     esi,[cpy_buf]
        shl     ecx,4
        call    rm_entries
        jc      rdc_fail
        cmp     byte ptr [rm_fix],0
        je      rdc_follow
        mov     al,[pl_drive]
        mov     ecx,[fv_spc]
        mov     edi,[cpy_buf]
        call    dk_write
        jc      rdc_wrerr
rdc_follow:
        mov     [rdc_last],ebp
        mov     esi,[rm_w]
        mov     eax,[esi+ebp*4]
        cmp     eax,RM_EOC
        je      rdc_end
        mov     ebp,eax
        jmp     short rdc_one
rdc_end:
        cmp     byte ptr [rm_fix],0
        je      rdc_ok
        cmp     byte ptr [rm_mode],RM_MERGE
        jne     rdc_ok
        call    rm_dirtail
        jc      rdc_wrerr
rdc_ok:
        popad
        clc
        ret
rdc_damaged:
        mov     eax,RL_DAMAGED
        jmp     short rdc_fail
rdc_rderr:
        mov     eax,RL_READERR
        jmp     short rdc_fail
rdc_wrerr:
        mov     eax,RL_WRITEERR
rdc_fail:
        mov     [rm_why],eax
        popad
        mov     eax,[rm_why]
        stc
        ret

;--- rm_dirlba - EBP = an old cluster -> EBX = its first sector: where
;--- it was while planning, and where the move has put it once rm_fix
;--- is set - the same place in the new data area, or merging, its slot
rm_dirlba:
        push    eax
        push    esi
        mov     eax,ebp
        sub     eax,2
        cmp     byte ptr [rm_fix],0
        je      rdl_old
        cmp     byte ptr [rm_mode],RM_MERGE
        jne     rdl_new
        mov     esi,[rm_x]
        mov     eax,[esi+ebp*4]
rdl_new:
        imul    eax,[fv_spc]
        add     eax,[rr_ndata]
        jmp     short rdl_out
rdl_old:
        imul    eax,[fv_spc]
        add     eax,[rr_odata]
rdl_out:
        mov     ebx,eax
        pop     esi
        pop     eax
        ret

;--- rm_entries - ESI -> ECX directory entries, each through rm_entry,
;--- as far as the end mark.  CF: EAX = refusal.
rm_entries:
        pushad
ree_one:
        jecxz   ree_ok
        cmp     byte ptr [rm_dirend],0
        jne     ree_ok
        call    rm_entry
        jc      ree_fail
        add     esi,32
        dec     ecx
        jmp     short ree_one
ree_ok:
        popad
        clc
        ret
ree_fail:
        mov     [rm_why],eax
        popad
        mov     eax,[rm_why]
        stc
        ret

;--- rm_entry - ESI -> one directory entry: checked, or with rm_fix,
;--- its first cluster renumbered.  CF: EAX = refusal.  An entry that
;--- starts with nought ends the directory, and sets rm_dirend.
;---
;--- "." AND ".." ARE NOT CLAIMED: they name directories some other
;--- entry names already.  They are renumbered like everything else.
rm_entry:
        pushad
        mov     al,[esi]
        or      al,al
        jz      ren_end
        cmp     al,0E5h
        je      ren_ok                  ; deleted
        mov     dl,[esi+0Bh]            ; DL = the attributes
        cmp     dl,ATTR_LFN
        je      ren_ok                  ; part of a long name
        test    dl,ATTR_VOLUME
        jnz     ren_ok                  ; the label
        movzx   ebx,word ptr [esi+1Ah]
        cmp     dword ptr [fv_bits],32
        jne     ren_have
        movzx   eax,word ptr [esi+14h]  ; FAT32 keeps the top half here
        shl     eax,16
        or      ebx,eax
ren_have:                               ; EBX = the first cluster
        cmp     byte ptr [rm_fix],0
        jne     ren_fix
        cmp     byte ptr [esi],'.'
        je      ren_ok
        or      ebx,ebx
        jnz     ren_named
        test    dl,ATTR_DIR
        jnz     ren_damaged             ; a directory has a cluster
        jmp     short ren_ok            ; an empty file has none
ren_named:
        mov     eax,ebx
        call    rm_named
        jc      ren_damaged
        test    dl,ATTR_DIR
        jnz     ren_ok
        cmp     byte ptr [rm_mode],RM_SPLIT
        jne     ren_ok
        mov     eax,ebx
        mov     ecx,[esi+1Ch]
        call    rm_trim
        jmp     short ren_ok
ren_fix:
        cmp     ebx,2
        jb      ren_ok
        cmp     ebx,[rm_nold]
        jae     ren_ok                  ; only "." and ".." were not checked,
        mov     eax,ebx                 ; and a wrong one is left wrong
        call    rm_map
        jc      ren_ok
        mov     [esi+1Ah],ax
        cmp     dword ptr [fv_bits],32
        jne     ren_ok
        shr     eax,16
        mov     [esi+14h],ax
ren_ok:
        popad
        clc
        ret
ren_end:
        mov     byte ptr [rm_dirend],1
        jmp     short ren_ok
ren_damaged:
        popad
        mov     eax,RL_DAMAGED
        stc
        ret

;--- rm_trim - a file of ECX bytes whose chain starts at EAX: how many
;--- of its last cluster's new clusters the length needs, into rm_x.
;--- Left at nought - all of them - when the chain and the length do
;--- not agree: that is for CHKDSK to settle, not a resize.
rm_trim:
        pushad
        mov     esi,[rm_w]
        xor     ebx,ebx                 ; EBX = clusters in the chain
        mov     edi,eax
rtr_walk:
        inc     ebx
        cmp     ebx,[rm_nused]
        ja      rtr_out
        mov     eax,[esi+edi*4]
        cmp     eax,RM_EOC
        je      rtr_last
        mov     edi,eax
        jmp     short rtr_walk
rtr_last:                               ; EDI = the last cluster
        mov     eax,1                   ; an empty file holding a cluster
        jecxz   rtr_need                ; keeps one
        lea     eax,[ecx-1]
        mov     ecx,dword ptr [fg_exec+FG_SPC]
        shl     ecx,9
        xor     edx,edx
        div     ecx
        inc     eax                     ; new clusters the length needs
rtr_need:
        mov     ecx,ebx                 ; less what the others give
        dec     ecx
        imul    ecx,[rm_k]
        sub     eax,ecx
        jbe     rtr_out                 ; the chain is longer than the file
        cmp     eax,[rm_k]
        ja      rtr_out                 ; or shorter
        mov     ecx,[rm_x]
        mov     [ecx+edi],al
rtr_out:
        popad
        ret

;--- rm_map - EAX = the first cluster of a chain, as numbered now ->
;--- EAX = its number afterwards.  CF when it is no chain's.
rm_map:
        cmp     byte ptr [rm_mode],RM_SPLIT
        je      rmp_split
        cmp     byte ptr [rm_mode],RM_MERGE
        je      rmp_merge
        clc
        ret
rmp_split:
        push    edx
        sub     eax,2
        mul     dword ptr [rm_k]
        add     eax,2
        pop     edx
        clc
        ret
rmp_merge:
        push    edx
        push    esi
        mov     esi,[rm_w]
        mov     edx,[esi+eax*4]
        or      edx,edx
        jz      rmp_no
        cmp     edx,RM_BADCL
        je      rmp_no
        mov     esi,[rm_x]
        mov     eax,[esi+eax*4]         ; its slot, the first of a new cluster
        xor     edx,edx
        div     dword ptr [rm_k]
        add     eax,2
        pop     esi
        pop     edx
        clc
        ret
rmp_no:
        pop     esi
        pop     edx
        stc
        ret

;---------------------------------------------------------------------
; rm_exec - a planned cluster adjustment, carried out.  CF on a read
;           or write that failed.
;
;   1  the root directory and the data, moved whole to where the new FAT
;      size puts them - rl_doresize's move, as far as the last sector in
;      use - so that a cluster's place in the data area is unchanged
;   2  merging, the files packed onto whole new clusters
;   3  splitting or merging, every directory entry renumbered, where the
;      first two steps have put its directory
;   4  the FATs, written new from the plan
;   5  the boot sector
;
; and then rx_table writes the partition table, last as always.
;---------------------------------------------------------------------
rm_exec:
        pushad
        call    rr_layout
        call    rm_shift
        jc      rmx_bad
        cmp     byte ptr [rm_mode],RM_MERGE
        jne     rmx_dirs
        call    rm_permute
        jc      rmx_bad
rmx_dirs:
        cmp     byte ptr [rm_mode],RM_WIDTH
        je      rmx_fats
        call    rm_fixdirs
        jc      rmx_bad
rmx_fats:
        call    rm_wrfats
        jc      rmx_bad
        call    rl_wrboot
        jc      rmx_bad
        popad
        clc
        ret
rmx_bad:
        popad
        stc
        ret

;--- rm_shift - the root and the data to where the new FAT puts them.
;--- The order is rl_doresize's: moving right, the data first, so the
;--- root does not land on data still to be read; moving left, the root
;--- first, for the same reason the other way.
rm_shift:
        pushad
        mov     eax,[rr_ndata]
        cmp     eax,[rr_odata]
        je      rsh_ok
        jb      rsh_left
        call    rsh_data
        jc      rsh_bad
        call    rz_root
        jc      rsh_bad
        jmp     short rsh_ok
rsh_left:
        call    rz_root
        jc      rsh_bad
        call    rsh_data
        jc      rsh_bad
rsh_ok:
        popad
        clc
        ret
rsh_bad:
        popad
        stc
        ret
rsh_data:
        mov     edx,OFFSET msg_movedata
        call    puts
        mov     eax,[rr_odata]
        mov     [rc_src],eax
        mov     eax,[rr_ndata]
        mov     [rc_dst],eax
        mov     eax,[fv_usedsec]
        mov     [rc_cnt],eax
        jmp     rl_copy

;---------------------------------------------------------------------
; rm_permute - merging: every old cluster to its slot, in place.
;
; THE MOVES ARE A PERMUTATION, and it is done by following its cycles.
; A slot is lifted into one buffer; the slot it goes to is lifted into
; the other before it is written over; and so on along the cycle until
; it reaches a slot that holds nothing, or the one it started from -
; which was lifted first, and can be written.  Every slot is read once
; and written once, and nothing is overwritten that has not been read:
; rm_bdone says which have been.
;---------------------------------------------------------------------
rm_permute:
        pushad
        mov     edx,OFFSET msg_rmpack
        call    puts
        mov     edi,[rm_bdone]
        mov     ecx,[rm_szb]
        shr     ecx,2
        xor     eax,eax
        rep     stosd
        mov     eax,[cpy_buf]
        mov     [rmp_a],eax
        add     eax,32768
        mov     [rmp_b],eax
        mov     dword ptr [rmp_tick],0
        mov     eax,[rm_nold]
        sub     eax,2
        mov     [rmp_n],eax             ; slots that can hold anything
        xor     ebp,ebp                 ; EBP = a slot: old cluster EBP+2
rpm_slot:
        cmp     ebp,[rmp_n]
        jae     rpm_ok
        lea     ecx,[ebp+2]
        call    rpm_live
        jc      rpm_next
        mov     esi,[rm_x]
        mov     ebx,[esi+ecx*4]         ; EBX = where it goes
        cmp     ebx,ebp
        jne     rpm_cycle
        mov     edi,[rm_bdone]
        bts     dword ptr [edi],ecx     ; there already
        jmp     short rpm_next
rpm_cycle:
        mov     eax,ebp
        mov     edi,[rmp_a]
        call    rpm_read
        jc      rpm_bad
        mov     edi,[rm_bdone]
        bts     dword ptr [edi],ecx
rpm_follow:                             ; EBX = where rmp_a's data goes
        lea     ecx,[ebx+2]
        cmp     ebx,[rmp_n]
        jae     rpm_land                ; past the old clusters: empty
        call    rpm_live
        jc      rpm_land
        mov     eax,ebx
        mov     edi,[rmp_b]
        call    rpm_read
        jc      rpm_bad
        mov     edi,[rm_bdone]
        bts     dword ptr [edi],ecx
        mov     eax,ebx
        mov     edi,[rmp_a]
        call    rpm_write
        jc      rpm_bad
        mov     esi,[rm_x]
        mov     ebx,[esi+ecx*4]
        mov     eax,[rmp_a]             ; and what was lifted moves next
        xchg    eax,[rmp_b]
        mov     [rmp_a],eax
        jmp     short rpm_follow
rpm_land:
        mov     eax,ebx
        mov     edi,[rmp_a]
        call    rpm_write
        jc      rpm_bad
rpm_next:
        inc     ebp
        jmp     short rpm_slot
rpm_ok:
        popad
        clc
        ret
rpm_bad:
        popad
        stc
        ret

;--- rpm_live - ECX = an old cluster.  CF clear if it holds data not
;--- lifted yet.
rpm_live:
        push    eax
        push    esi
        mov     esi,[rm_w]
        mov     eax,[esi+ecx*4]
        or      eax,eax
        jz      rpl_no
        cmp     eax,RM_BADCL
        je      rpl_no
        mov     esi,[rm_bdone]
        bt      dword ptr [esi],ecx
        jc      rpl_no
        pop     esi
        pop     eax
        clc
        ret
rpl_no:
        pop     esi
        pop     eax
        stc
        ret

;--- rpm_read / rpm_write - slot EAX of the new data area, EDI -> the
;--- buffer.  A slot is one OLD cluster.  A dot for every 64K written.
rpm_read:
        push    eax
        push    ebx
        push    ecx
        mov     ecx,[fv_spc]
        imul    eax,ecx
        add     eax,[rr_ndata]
        mov     ebx,eax
        mov     al,[pl_drive]
        call    dk_read
        pop     ecx
        pop     ebx
        pop     eax
        ret
rpm_write:
        push    eax
        push    ebx
        push    ecx
        mov     ecx,[fv_spc]
        imul    eax,ecx
        add     eax,[rr_ndata]
        mov     ebx,eax
        mov     al,[pl_drive]
        call    dk_write
        jc      rpw_out
        add     [rmp_tick],ecx
        cmp     dword ptr [rmp_tick],RC_CHUNK
        jb      rpw_ok
        mov     dword ptr [rmp_tick],0
        call    rc_dot
rpw_ok:
        clc
rpw_out:
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; rm_fixdirs - every directory entry renumbered, where steps 1 and 2
;              have put its directory: the root region, then every
;              directory the plan found.  CF on a failed read or write.
;---------------------------------------------------------------------
rm_fixdirs:
        pushad
        mov     edx,OFFSET msg_rmdirs
        call    puts
        mov     byte ptr [rm_fix],1
        cmp     dword ptr [fv_bits],32
        je      rfd_chains
        mov     ebx,[rr_nroot]
        mov     ecx,[fv_rootsec]
        call    rm_region
        jc      rfd_bad
rfd_chains:
        ; zeros, for what merging leaves after a directory's end - made
        ; after the root, which can fill the whole buffer, and kept clear
        ; of the half a directory's cluster is read into
        mov     edi,[cpy_buf]
        add     edi,32768
        mov     ecx,32768 / 4
        xor     eax,eax
        rep     stosd
        mov     ebp,2
rfd_one:
        cmp     ebp,[rm_nold]
        jae     rfd_ok
        mov     edi,[rm_bdir]
        bt      dword ptr [edi],ebp
        jnc     rfd_next
        mov     eax,ebp
        call    rm_dirchain
        jc      rfd_bad
rfd_next:
        inc     ebp
        jmp     short rfd_one
rfd_ok:
        mov     byte ptr [rm_fix],0
        popad
        clc
        ret
rfd_bad:
        mov     byte ptr [rm_fix],0
        popad
        stc
        ret

;--- rm_dirtail - merging: the slots after a directory's last old
;--- cluster, in its last new one, hold whatever the packing left there
;--- - and a directory is read to the end of its last cluster.  Zeroed.
rm_dirtail:
        pushad
        mov     esi,[rm_x]
        mov     ecx,[rdc_last]
        mov     ebx,[esi+ecx*4]         ; EBX = the last slot it fills
        mov     eax,ebx
        xor     edx,edx
        div     dword ptr [rm_k]
        inc     eax
        mul     dword ptr [rm_k]
        mov     ebp,eax                 ; EBP = the next new cluster's first
rdt_one:
        inc     ebx
        cmp     ebx,ebp
        jae     rdt_ok
        mov     eax,ebx
        mov     edi,[cpy_buf]
        add     edi,32768
        call    rpm_write
        jc      rdt_bad
        jmp     short rdt_one
rdt_ok:
        popad
        clc
        ret
rdt_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; rm_wrfats - every copy of the new FAT, from the plan: a sector at a
;             time for sixteen and thirty-two bits, and whole for
;             twelve, whose entries straddle sectors.
;---------------------------------------------------------------------
rm_wrfats:
        pushad
        mov     edx,OFFSET msg_movefat
        call    puts
        cmp     dword ptr [fg_exec+FG_BITS],12
        je      rmw_12
        mov     ecx,256
        cmp     dword ptr [fg_exec+FG_BITS],16
        je      rmw_epb
        mov     ecx,128
rmw_epb:
        mov     [rmw_per],ecx
        xor     ebp,ebp                 ; EBP = the sector of the FAT
rmw_sec:
        cmp     ebp,dword ptr [fg_exec+FG_SPF]
        jae     rmw_ok
        mov     edi,[sec_buf]
        mov     eax,ebp
        mul     dword ptr [rmw_per]
        mov     ebx,eax                 ; EBX = its first entry
        xor     ecx,ecx
rmw_ent:
        cmp     ecx,[rmw_per]
        jae     rmw_put
        lea     eax,[ebx+ecx]
        call    rm_newent
        cmp     dword ptr [fg_exec+FG_BITS],16
        jne     rmw_e32
        mov     [edi+ecx*2],ax
        jmp     short rmw_enext
rmw_e32:
        mov     [edi+ecx*4],eax
rmw_enext:
        inc     ecx
        jmp     short rmw_ent
rmw_put:
        xor     edx,edx                 ; the same sector into every copy
rmw_copy:
        cmp     edx,[fv_nfats]
        jae     rmw_snext
        mov     eax,edx
        imul    eax,dword ptr [fg_exec+FG_SPF]
        add     eax,[rr_nfat]
        add     eax,ebp
        mov     ebx,eax
        mov     al,[pl_drive]
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_write
        jc      rmw_bad
        inc     edx
        jmp     short rmw_copy
rmw_snext:
        inc     ebp
        jmp     short rmw_sec

rmw_12:
        mov     ecx,dword ptr [fg_exec+FG_SPF]
        cmp     ecx,RC_CHUNK
        ja      rmw_bad                 ; no FAT12 FAT is that long
        shl     ecx,7                   ; dwords
        mov     edi,[cpy_buf]
        xor     eax,eax
        rep     stosd
        xor     ebp,ebp
rmw_12one:
        cmp     ebp,[rm_nnew]
        jae     rmw_12put
        mov     eax,ebp
        call    rm_newent
        mov     edx,eax
        mov     eax,ebp
        call    fat12_put
        inc     ebp
        jmp     short rmw_12one
rmw_12put:
        xor     edx,edx
rmw_12copy:
        cmp     edx,[fv_nfats]
        jae     rmw_ok
        mov     eax,edx
        imul    eax,dword ptr [fg_exec+FG_SPF]
        add     eax,[rr_nfat]
        mov     ebx,eax
        mov     al,[pl_drive]
        mov     ecx,dword ptr [fg_exec+FG_SPF]
        mov     edi,[cpy_buf]
        call    dk_write
        jc      rmw_bad
        inc     edx
        jmp     short rmw_12copy
rmw_ok:
        popad
        clc
        ret
rmw_bad:
        popad
        stc
        ret

;--- rm_newent - EAX = a cluster of the new volume -> EAX = its entry in
;--- the new FAT, at the new width.
rm_newent:
        push    ebx
        push    ecx
        push    edx
        push    esi
        cmp     eax,2
        jae     rne_clus
        ; ENTRIES 0 AND 1: as they were at the same width.  At the other,
        ; made: the media byte with every bit above it set, and the
        ; end-of-chain value
        mov     ebx,dword ptr [fg_exec+FG_BITS]
        cmp     ebx,[fv_bits]
        jne     rne_made
        mov     ecx,eax
        mov     eax,[rm_e0]
        or      ecx,ecx
        jz      rne_out
        mov     eax,[rm_e1]
        jmp     rne_out
rne_made:
        mov     ecx,eax
        call    rne_mask                ; -> EDX
        mov     eax,edx
        or      ecx,ecx
        jz      rne_media
        jmp     rne_out
rne_media:
        and     eax,NOT 0FFh
        or      eax,[fv_media]
        jmp     rne_out
rne_clus:
        cmp     eax,[rm_nnew]
        jae     rne_free
        cmp     byte ptr [rm_mode],RM_SPLIT
        je      rne_split
        cmp     byte ptr [rm_mode],RM_MERGE
        je      rne_merge
        cmp     eax,[rm_nold]           ; the same numbers: the old entry
        jae     rne_free
        mov     esi,[rm_w]
        mov     eax,[esi+eax*4]
        jmp     rne_enc
rne_merge:
        cmp     eax,[rm_alloc]
        jae     rne_free
        mov     esi,[rm_bend]
        bt      dword ptr [esi],eax
        jc      rne_eoc
        inc     eax
        jmp     rne_enc
rne_split:
        mov     ebx,eax                 ; EBX = the new cluster
        sub     eax,2
        xor     edx,edx
        div     dword ptr [rm_k]        ; EAX = old - 2, EDX = which of its k
        add     eax,2
        cmp     eax,[rm_nold]
        jae     rne_free
        mov     esi,[rm_w]
        mov     ecx,[esi+eax*4]         ; ECX = the old cluster's entry
        or      ecx,ecx
        jz      rne_free
        cmp     ecx,RM_BADCL
        je      rne_bad
        mov     esi,[rm_x]
        movzx   esi,byte ptr [esi+eax]  ; how many it keeps; nought for all
        or      esi,esi
        jnz     rne_kept
        mov     esi,[rm_k]
rne_kept:
        inc     edx                     ; EDX = which, counted from one
        cmp     edx,esi
        ja      rne_free                ; past what the file needs
        jb      rne_inner
        cmp     ecx,RM_EOC              ; the last it keeps: where the old
        je      rne_eoc                 ; cluster led
        lea     eax,[ecx-2]
        mul     dword ptr [rm_k]
        add     eax,2
        jmp     rne_enc
rne_inner:
        lea     eax,[ebx+1]
        jmp     rne_enc
rne_free:
        xor     eax,eax
        jmp     rne_out
rne_bad:
        mov     eax,RM_BADCL
        jmp     rne_enc
rne_eoc:
        mov     eax,RM_EOC
rne_enc:
        cmp     eax,RM_BADCL
        jb      rne_out                 ; a cluster, or free
        mov     ecx,eax
        call    rne_mask                ; EOC is every bit of the width,
        mov     eax,edx                 ; and BAD is eight below it
        cmp     ecx,RM_EOC
        je      rne_out
        sub     eax,8
rne_out:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- rne_mask - EDX = every bit of the new width
rne_mask:
        mov     edx,0FFFh
        cmp     dword ptr [fg_exec+FG_BITS],12
        je      rnk_out
        mov     edx,0FFFFh
        cmp     dword ptr [fg_exec+FG_BITS],16
        je      rnk_out
        mov     edx,0FFFFFFFh
rnk_out:
        ret

;--- fat12_put - EAX = a cluster, EDX = its entry, into the twelve-bit
;--- FAT being built whole in the copy buffer: fat12_get's other half.
fat12_put:
        push    eax
        push    ebx
        push    edx
        mov     ebx,eax
        shr     ebx,1
        add     ebx,eax
        add     ebx,[cpy_buf]           ; EBX -> the pair
        and     edx,0FFFh
        test    eax,1
        jnz     f12p_odd
        mov     ax,[ebx]
        and     ax,0F000h
        or      ax,dx
        mov     [ebx],ax
        jmp     short f12p_out
f12p_odd:
        shl     edx,4
        mov     ax,[ebx]
        and     ax,000Fh
        or      ax,dx
        mov     [ebx],ax
f12p_out:
        pop     edx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; rm_say - what a cluster adjustment changes, for the report of a
;          resize that is about to do one.  Nothing when it is not.
;---------------------------------------------------------------------
rm_say:
        pushad
        cmp     byte ptr [rm_mode],RM_NONE
        je      rsy_out
        cmp     byte ptr [rm_mode],RM_WIDTH
        jne     rsy_clus
        mov     edx,OFFSET msg_rmwidth
        call    puts
        mov     eax,[fv_bits]
        call    put2d
        mov     edx,OFFSET msg_rmto
        call    puts
        mov     eax,dword ptr [fg_exec+FG_BITS]
        call    put2d
        mov     edx,OFFSET msg_rmwdone
        call    puts
        jmp     short rsy_out
rsy_clus:
        mov     edx,OFFSET msg_rmclus
        call    puts
        mov     eax,[fv_spc]
        call    put1
        mov     edx,OFFSET msg_rmsecto
        call    puts
        mov     eax,dword ptr [fg_exec+FG_SPC]
        call    put1
        mov     edx,OFFSET msg_rmsplit
        cmp     byte ptr [rm_mode],RM_SPLIT
        je      rsy_say
        mov     edx,OFFSET msg_rmmerge
rsy_say:
        call    puts
        mov     edx,OFFSET msg_rmwarn
        call    puts
rsy_out:
        popad
        ret

;--- the plan, and the tables it lives in -----------------------------
rm_mode     db  0                       ; RM_*
rm_badhit   db  0                       ; a larger cluster was stopped by a
                                        ; bad one
rm_fithit   db  0                       ; a size was stopped by what is stored
rm_fix      db  0                       ; 0 reading the directories, 1
                                        ; rewriting them
rm_dirend      db  0                       ; a directory's end mark was met
rds_found   db  0
            db  2 dup(0)
rm_k        dd  1                       ; the ratio, whichever way
rm_spc      dd  0
rm_why      dd  0
rm_nold     dd  0                       ; entries in the old FAT: clusters + 2
rm_nnew     dd  0                       ; ...and in the new one
rm_nused    dd  0                       ; clusters in use
rm_walked   dd  0
rm_alloc    dd  0                       ; merging: the next new cluster free
rm_nbad     dd  0
rm_oeoc     dd  0                       ; the old width's first end mark
rm_e0       dd  0                       ; entries 0 and 1, as they were
rm_e1       dd  0
rm_mem      dd  0                       ; the 48h block, and how big
rm_memsz    dd  0
rm_szw      dd  0
rm_szx      dd  0
rm_szb      dd  0
rm_sze      dd  0
rm_w        dd  0
rm_x        dd  0
rm_bref     dd  0
rm_bdir     dd  0
rm_bdone    dd  0
rm_bclm     dd  0
rm_bend     dd  0
rmd_lba     dd  0
rmd_per     dd  0
rml_slot0   dd  0
rrg_left    dd  0
rdc_steps   dd  0
rdc_last    dd  0
rmp_a       dd  0
rmp_b       dd  0
rmp_n       dd  0
rmp_tick    dd  0
rmw_per     dd  0
rm_try      db  FG_LEN dup(0)

msg_rmwidth db  '  The volume changes from FAT$'
msg_rmto    db  ' to FAT$'
msg_rmwdone db  '.  No file moves.',13,10,'$'
msg_rmclus  db  '  The clusters change from $'
msg_rmsecto db  ' sector(s) to $'
msg_rmsplit db  '.  No file moves, but every',13,10
            db  '  cluster is renumbered.',13,10,'$'
msg_rmmerge db  ', so every file moves and',13,10
            db  '  every cluster is renumbered.',13,10,'$'
msg_rmwarn  db  '  Do not turn the machine off until FDISK has finished.',13,10,'$'
msg_rmpack  db  13,10,'  packing the files into the larger clusters$'
msg_rmdirs  db  13,10,'  renumbering the directories$'
