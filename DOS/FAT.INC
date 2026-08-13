;=====================================================================
; FAT.INC - FAT12 / FAT16 / FAT32 filesystem driver with subdirectories
;
; v0.4.  One volume is LOADED at a time (the vol_state block), but
; every drive keeps its own copy of that block in vol_slots and
; vol_select swaps them, so all four drives are live at once - FAT
; caches, geometry and current directory survive a switch.  Cluster
; numbers are 32-bit everywhere.  The FAT itself is accessed two ways:
;   FAT12  - cached whole in fat_buf (max 16 sectors), because FAT12
;            entries straddle sector boundaries.
;   FAT16/32 - through a one-sector cache (fatsec_buf) with lazy
;            write-back to every FAT copy.
;
; Directories are enumerated through a context (dc_isroot/dc_start):
; either the fixed root area of FAT12/16 or a cluster chain (all
; subdirectories, and the FAT32 root).  dir_seek hides the difference.
;
; Hard-disk drives (BIOS 80h+) are expected to carry an MBR; the first
; used partition is the volume.
;
; Included by DOSMAIN.ASM.
;=====================================================================

FATBUF_SIZE     EQU     8192            ; whole-FAT cache (FAT12 only)

; ONE CLUSTER.  32K, because that is the largest cluster MS-DOS itself
; can produce and this buffer is the ONLY thing deciding which volumes
; will mount: fat_mount refuses a volume whose cluster does not fit,
; and it refuses it as "invalid media type", which reads as a damaged
; disk rather than as a limit of ours.
;
; It was 8192 until v0.32, i.e. 16 sectors, i.e. 8K clusters - and
; every test image in BIN is between 32M and 84M, where FORMAT picks
; 2K and 4K clusters and the ceiling is nowhere in sight.  The first
; real hard disk it ever met was an 850M one, where MS-DOS 6.22 lays
; down 16K clusters because 850M over the 65524-cluster FAT16 limit
; leaves no smaller choice, and PM-DOS called a perfectly good C:
; drive unformatted.  The rule this got wrong: a limit that no test
; disk can reach is not tested by having tests.
CLUSBUF_SIZE    EQU     32768           ; one cluster, up to 64 sectors
MAX_DEPTH       EQU     8               ; nested directories supported
MAX_DRIVES      EQU     LASTDRIVE       ; shared with COMMAND.COM
MAX_PHYS        EQU     2               ; hard disks the block layer has
                                        ; slots for: BIOS 80h and 81h

; Size of the swappable per-volume state block.  wasm cannot divide by
; a forward-referenced label difference, so this is stated by hand and
; asserted against the real vol_state..vol_state_end span below.
VOL_STATE_SIZE  EQU     8884

; 32-byte directory entry field offsets
DE_NAME         EQU     0
DE_ATTR         EQU     11
DE_CLUSHI       EQU     20              ; high word of cluster (FAT32)
DE_TIME         EQU     22
DE_DATE         EQU     24
DE_CLUSTER      EQU     26
DE_SIZE         EQU     28

; (ATTR_* attribute bits live in PMDOS.INC - shared with programs)

;=====================================================================
; fat_init - the parked per-drive state, out of the arena.
;            Called from dos_entry before anything mounts anything.
;=====================================================================
fat_init:
        pushad
        mov     ebx,MAX_DRIVES * VOL_STATE_SIZE
        call    arena_alloc
        jc      fi_none
        mov     [vol_slotp],eax
        mov     edi,eax
        xor     eax,eax
        mov     ecx,(MAX_DRIVES * VOL_STATE_SIZE) / 4
        rep     stosd
        popad
        clc
        ret
fi_none:
        ; Nothing else can run without it - every drive switch copies
        ; through here - so this is fatal rather than degraded.
        mov     esi,OFFSET msgNoVolMem
        call    dword ptr ds:[IOS_PANIC]

;=====================================================================
; THE PARTITION SCAN
;
; A drive letter is a VOLUME.  Until 0.26 it was a DISK: logical drive
; 2 meant "BIOS 80h, and whatever the first non-empty partition table
; entry points at".  That was wrong in four ways at once - a second
; primary partition was unreachable, an extended partition was
; invisible, a non-FAT partition was mounted anyway and then failed as
; "invalid media", and a disk whose first entry was the extended
; container mounted the container.
;
; So the table below is built once at boot and every letter after that
; is an index into it.
;
; THE ORDER LETTERS ARE HANDED OUT IS NOT ARBITRARY and it is not the
; order the partitions appear in.  DOS assigned the first primary of
; every disk first, THEN all the logical drives, THEN the remaining
; primaries.  A disk built on a real DOS machine has its volumes
; labelled and its AUTOEXEC written on that assumption, so getting the
; order right is the difference between reading somebody's disk and
; reading somebody's disk with the letters shuffled.
;=====================================================================

;---------------------------------------------------------------------
; part_isfat - AL = a partition type byte.  CF clear if it is a FAT
;              volume that should get a letter.
;
; The hidden spellings (11h, 14h, 16h, 1Bh, 1Ch, 1Eh) are deliberately
; absent.  A boot manager sets those to make a volume invisible to the
; DOS that is running, and honouring that is a one-line decision that
; keeps dual-boot disks working the way their owner set them up.
;---------------------------------------------------------------------
part_isfat:
        cmp     al,PT_FAT12
        je      pif_yes
        cmp     al,PT_FAT16S
        je      pif_yes
        cmp     al,PT_FAT16B
        je      pif_yes
        cmp     al,PT_FAT16L
        je      pif_yes
        cmp     al,PT_FAT32
        je      pif_yes
        cmp     al,PT_FAT32L
        je      pif_yes
        stc
        ret
pif_yes:
        clc
        ret

;---------------------------------------------------------------------
; part_isext - AL = type.  CF clear if it is an extended container.
;---------------------------------------------------------------------
part_isext:
        cmp     al,PT_EXTENDED
        je      pie_yes
        cmp     al,PT_EXTENDEDL
        je      pie_yes
        stc
        ret
pie_yes:
        clc
        ret

;---------------------------------------------------------------------
; drv_slot - EBX = the table entry for logical drive AL.  No bounds
;            check; every caller has already made one.
;---------------------------------------------------------------------
drv_slot:
        movzx   ebx,al
        imul    ebx,DRV_LEN
        add     ebx,OFFSET drv_tbl
        ret

;---------------------------------------------------------------------
; drv_add - claim the next free letter.  AL = BIOS drive, AH = type,
;           EDX = start LBA, ECX = sectors, BL = primary index or FFh.
;           CF set if there are no letters left.
;---------------------------------------------------------------------
drv_add:
        push    ebx
        push    edi
        movzx   edi,byte ptr [drv_count]
        cmp     edi,MAX_DRIVES
        jae     da_full
        imul    edi,DRV_LEN
        add     edi,OFFSET drv_tbl
        mov     [edi+DRV_BIOS],al
        mov     [edi+DRV_TYPE],ah
        mov     byte ptr [edi+DRV_FLAGS],DRVF_PRESENT
        mov     [edi+DRV_PIDX],bl
        mov     [edi+DRV_START],edx
        mov     [edi+DRV_SECS],ecx
        inc     byte ptr [drv_count]
        pop     edi
        pop     ebx
        clc
        ret
da_full:
        pop     edi
        pop     ebx
        stc
        ret

;=====================================================================
; disk_scan - build drv_tbl.  Called once, from dos_entry, before
;             anything tries to mount anything.
;=====================================================================
disk_scan:
        pushad
        mov     edi,OFFSET drv_tbl              ; a clean table
        xor     eax,eax
        mov     ecx,(MAX_DRIVES * DRV_LEN) / 4
        rep     stosd
        mov     byte ptr [drv_count],0

;--- A: and B: are slots whether or not a drive is behind them -------
; DOS did this too: a machine with one floppy still had a B:, and
; asking for it prompted for a disk rather than saying "invalid drive".
; The mount is deferred, so an empty drive costs nothing until it is
; touched.
        xor     eax,eax
        xor     edx,edx
        xor     ecx,ecx
        mov     bl,0FFh
        call    drv_add
        mov     edi,OFFSET drv_tbl
        or      byte ptr [edi+DRV_FLAGS],DRVF_FLOPPY

        mov     ax,0001h
        xor     edx,edx
        xor     ecx,ecx
        mov     bl,0FFh
        call    drv_add
        mov     edi,OFFSET drv_tbl + DRV_LEN
        or      byte ptr [edi+DRV_FLAGS],DRVF_FLOPPY

;--- pass 1: the first primary FAT partition on each disk ------------
        mov     byte ptr [ds_disk],80h
ds_p1:
        call    ds_primary
        inc     byte ptr [ds_disk]
        cmp     byte ptr [ds_disk],80h + MAX_PHYS
        jb      ds_p1

;--- pass 2: every logical drive, disk by disk -----------------------
        mov     byte ptr [ds_disk],80h
ds_p2:
        call    ds_logicals
        inc     byte ptr [ds_disk]
        cmp     byte ptr [ds_disk],80h + MAX_PHYS
        jb      ds_p2

;--- pass 3: the primaries that pass 1 did not take ------------------
        mov     byte ptr [ds_disk],80h
ds_p3:
        call    ds_restprim
        inc     byte ptr [ds_disk]
        cmp     byte ptr [ds_disk],80h + MAX_PHYS
        jb      ds_p3

        popad
        ret

;---------------------------------------------------------------------
; ds_mbr - read sector EBX of [ds_disk] into sec_buf.  CF set if it
;          could not be read or does not end in AA55h.
;
; NOT through disk_read: this runs before any volume is mounted and a
; disk that is not there must be a quiet "no", not a critical-error
; prompt about a drive the user never mentioned.
;---------------------------------------------------------------------
ds_mbr:
        push    eax
        push    ecx
        push    edi
        mov     al,[ds_disk]
        mov     cx,1
        mov     edi,OFFSET sec_buf
        call    dword ptr ds:[IOS_RDBLK]
        jc      dm_out
        cmp     word ptr [sec_buf+1FEh],MBR_SIG
        je      dm_ok
        stc
        jmp     short dm_out
dm_ok:
        clc
dm_out:
        pop     edi
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; ds_primary - give a letter to the first primary FAT partition on
;              [ds_disk], and remember which entry it was.
;
; A disk with no partition table at all is not an error: plenty of
; removable media and a few hard disks were formatted as one big
; volume with a BPB in sector 0 and nothing else.  If sector 0 looks
; like a boot sector, the whole disk is the volume.
;---------------------------------------------------------------------
ds_primary:
        pushad
        movzx   ebp,byte ptr [ds_disk]
        sub     ebp,80h                         ; the disk's index, kept
        mov     byte ptr [ds_taken+ebp],0FFh    ; in EBP throughout
        mov     dword ptr [ds_extlba+ebp*4],0

        xor     ebx,ebx
        call    ds_mbr
        jc      dp_super                        ; no table - superfloppy?

        mov     esi,OFFSET sec_buf + PART_OFF
        xor     ecx,ecx                         ; entry index
        xor     edi,edi                         ; the entry we will take
        mov     bl,0FFh                         ; ...and its number
dp_scan:
        mov     al,[esi+PE_TYPE]
        call    part_isext
        jc      dp_notext
        ; A container, not a volume.  Note where it starts - pass 2
        ; walks it - and the first one wins, because DOS only ever
        ; followed one chain per disk.
        cmp     dword ptr [ds_extlba+ebp*4],0
        jne     dp_step
        mov     eax,dword ptr [esi+PE_START]
        mov     [ds_extlba+ebp*4],eax
        jmp     short dp_step
dp_notext:
        or      edi,edi                         ; already have one
        jnz     dp_step
        call    part_isfat
        jc      dp_step
        mov     bl,cl
        mov     edi,esi
dp_step:
        add     esi,PE_LEN
        inc     ecx
        cmp     ecx,PART_MAX
        jb      dp_scan

        or      edi,edi
        jz      dp_bpbonly                      ; a table with nothing
        mov     [ds_taken+ebp],bl               ; DOS would mount in it
        mov     ah,[edi+PE_TYPE]
        mov     al,[ds_disk]
        mov     edx,dword ptr [edi+PE_START]
        mov     ecx,dword ptr [edi+PE_SECS]
        call    drv_add
        popad
        ret

;--- a signature and a table, but nothing in the table ---------------
; Some media carry both: a boot sector with a real BPB in it AND the
; AA55h that makes a partition table look plausible.  If nothing in
; the table is a volume, sector 0 itself might be one.
dp_bpbonly:
        cmp     dword ptr [ds_extlba+ebp*4],0   ; unless it is all chain
        jne     dp_none
        jmp     short dp_try0

;--- no signature at all: is sector 0 a boot sector? -----------------
; A hard disk formatted as one big volume with no partition table.
; Uncommon on a PC and universal on removable media, and sector 0 has
; already been read by ds_mbr - it just did not end in AA55h, which
; is not a requirement for a boot sector, only for a partition table.
dp_super:
        mov     al,[ds_disk]                    ; ds_mbr may have failed on
        xor     ebx,ebx                         ; the READ rather than the
        mov     cx,1                            ; signature, so ask again
        mov     edi,OFFSET sec_buf
        call    dword ptr ds:[IOS_RDBLK]
        jc      dp_none
dp_try0:
        call    bpb_looks
        jc      dp_none
        mov     al,[ds_disk]
        mov     ah,PT_FAT16B                    ; a plausible label for
        xor     edx,edx                         ; something that has no
        xor     ecx,ecx                         ; type byte anywhere
        mov     bl,0FFh
        call    drv_add
dp_none:
        popad
        ret

;---------------------------------------------------------------------
; ds_logicals - walk [ds_disk]'s extended partition chain.
;
; The chain is a linked list of one-entry partition tables.  Entry 0 of
; each extended boot record describes a logical volume, and entry 1, if
; it is another container, points at the next EBR.
;
; THE TWO START FIELDS ARE RELATIVE TO DIFFERENT THINGS, which is the
; classic way to get this wrong: entry 0's is relative to the EBR that
; holds it, and entry 1's is relative to the FIRST container in the
; chain.  Using one base for both walks off the disk on the second
; logical drive.
;---------------------------------------------------------------------
ds_logicals:
        pushad
        movzx   eax,byte ptr [ds_disk]
        sub     eax,80h
        mov     esi,[ds_extlba+eax*4]           ; the container's base
        or      esi,esi
        jz      dl_done
        mov     edi,esi                         ; EDI = base, ESI = this EBR
        xor     ebp,ebp                         ; a guard against a chain
                                                ; that points at itself
dl_next:
        inc     ebp
        cmp     ebp,MAX_DRIVES + 4              ; more links than we could
        ja      dl_done                         ; ever hand out letters for
        mov     ebx,esi
        call    ds_mbr
        jc      dl_done

        mov     al,[sec_buf+PART_OFF+PE_TYPE]
        call    part_isfat
        jc      dl_nextlink
        mov     ah,[sec_buf+PART_OFF+PE_TYPE]
        mov     al,[ds_disk]
        mov     edx,dword ptr [sec_buf+PART_OFF+PE_START]
        add     edx,esi                         ; relative to THIS record
        mov     ecx,dword ptr [sec_buf+PART_OFF+PE_SECS]
        mov     bl,0FFh                         ; not a primary
        call    drv_add
        jc      dl_done                         ; out of letters

dl_nextlink:
        mov     al,[sec_buf+PART_OFF+PE_LEN+PE_TYPE]
        call    part_isext
        jc      dl_done
        mov     eax,dword ptr [sec_buf+PART_OFF+PE_LEN+PE_START]
        or      eax,eax
        jz      dl_done
        mov     esi,edi                         ; relative to the FIRST
        add     esi,eax                         ; container, not to this one
        jmp     dl_next
dl_done:
        popad
        ret

;---------------------------------------------------------------------
; ds_restprim - the primary FAT partitions pass 1 left behind.
;---------------------------------------------------------------------
ds_restprim:
        pushad
        xor     ebx,ebx
        call    ds_mbr
        jc      dr_done

        movzx   ebp,byte ptr [ds_disk]
        sub     ebp,80h
        mov     bh,[ds_taken+ebp]               ; the one pass 1 claimed
        mov     esi,OFFSET sec_buf + PART_OFF
        xor     ecx,ecx
dr_scan:
        cmp     cl,bh
        je      dr_skip
        mov     al,[esi+PE_TYPE]
        call    part_isfat
        jc      dr_skip
        mov     ah,[esi+PE_TYPE]
        mov     al,[ds_disk]
        mov     bl,cl
        push    ecx
        mov     edx,dword ptr [esi+PE_START]
        mov     ecx,dword ptr [esi+PE_SECS]
        call    drv_add
        pop     ecx
        jc      dr_done
dr_skip:
        add     esi,PE_LEN
        inc     ecx
        cmp     ecx,PART_MAX
        jb      dr_scan
dr_done:
        popad
        ret

;---------------------------------------------------------------------
; bpb_looks - does sec_buf hold something that could be a boot sector?
;             CF clear if so.
;
; Three tests, and they are the ones DOS itself used.  A jump
; instruction at the front, because that is what makes it bootable and
; nothing else starts EBh or E9h by accident; a sector size that is a
; power of two in the legal range; and a non-zero cluster size.  The
; AA55h signature is NOT one of them - plenty of perfectly good
; non-bootable DOS floppies do not have it.
;---------------------------------------------------------------------
bpb_looks:
        push    eax
        mov     al,[sec_buf+BPB_JMP]
        cmp     al,0EBh
        je      bl_jmp
        cmp     al,0E9h
        jne     bl_no
bl_jmp:
        movzx   eax,word ptr [sec_buf+BPB_BPS]
        cmp     eax,128
        jb      bl_no
        cmp     eax,4096
        ja      bl_no
        push    edx                             ; a power of two, so that
        mov     edx,eax                         ; "512" is not the only
        dec     edx                             ; thing this accepts and a
        and     edx,eax                         ; corrupt field is still
        pop     edx                             ; rejected
        jnz     bl_no
        cmp     byte ptr [sec_buf+BPB_SPC],0
        je      bl_no
        pop     eax
        clc
        ret
bl_no:
        pop     eax
        stc
        ret

;=====================================================================
; fat_mount - mount logical drive [cur_vol] into the loaded state
;             block, using the volume table entry for it.  CF on
;             failure.
;=====================================================================
fat_mount:
        pushad
fm_start:
        mov     byte ptr [fat_ok],0
        mov     dword ptr [clusbuf_clus],-1
        mov     dword ptr [fatsec_lba],-1
        mov     byte ptr [fatsec_dirty],0
        mov     byte ptr [fat12_dirty],0
        mov     dword ptr [fat_hint],2
        mov     dword ptr [fat_active],0

;--- where this letter lives -----------------------------------------
        mov     al,[cur_vol]
        cmp     al,MAX_DRIVES
        jae     fm_nodrive
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      fm_nodrive
        mov     al,[ebx+DRV_BIOS]
        mov     [fat_drive],al
        mov     eax,[ebx+DRV_START]
        mov     [fat_partlba],eax
        mov     eax,[ebx+DRV_SECS]
        mov     [fat_partsecs],eax

;--- volume boot record ----------------------------------------------
        mov     al,[fat_drive]
        mov     ebx,[fat_partlba]
        mov     cx,1
        mov     edi,OFFSET sec_buf
        call    disk_read
        jc      fm_iofail

;--- pull the BPB apart ----------------------------------------------
; Sector size first, and it is the one field this kernel refuses to
; adapt to: every buffer here is a whole number of 512-byte sectors
; and the block layer counts in them.  Media with 1024 or 2048 exist -
; some 5.25" formats, a few magneto-opticals - and they are rejected
; honestly rather than mounted and misread.
        movzx   eax,word ptr [sec_buf+BPB_BPS]
        cmp     eax,512
        jne     fm_nobpb

        movzx   eax,byte ptr [sec_buf+BPB_SPC]  ; sectors per cluster
        or      eax,eax
        jz      fm_nobpb
        cmp     eax,CLUSBUF_SIZE / 512
        ja      fm_fail
        mov     [fat_spc],eax

        movzx   eax,word ptr [sec_buf+BPB_RSVD] ; reserved sectors
        or      eax,eax                         ; at least the boot sector
        jz      fm_nobpb
        mov     [fat_rsvd],eax

        movzx   ecx,byte ptr [sec_buf+BPB_NFATS]
        or      ecx,ecx
        jz      fm_nobpb
        cmp     ecx,4                           ; nobody ever shipped more
        ja      fm_nobpb
        mov     [fat_nfats],ecx

        movzx   eax,word ptr [sec_buf+BPB_ROOTENT]
        mov     [fat_rootent],eax

        mov     al,[sec_buf+BPB_MEDIA]
        mov     [fat_media],al

        movzx   eax,word ptr [sec_buf+BPB_SPF16]
        or      eax,eax
        jnz     fm_havespf
        mov     eax,dword ptr [sec_buf+BPB_SPF32]
        or      eax,eax
        jz      fm_nobpb
fm_havespf:
        mov     [fat_spf],eax

        movzx   ecx,word ptr [sec_buf+BPB_TOTSEC16]
        or      ecx,ecx
        jnz     fm_havetot
        mov     ecx,dword ptr [sec_buf+BPB_TOTSEC32]
        or      ecx,ecx
        jz      fm_nobpb
fm_havetot:
        ; A BPB may describe MORE sectors than the partition entry says
        ; it has, and one of the two is lying.  Believe the smaller: a
        ; volume that reads past its own partition reads the next one.
        mov     eax,[fat_partsecs]
        or      eax,eax
        jz      fm_havetot2
        cmp     ecx,eax
        jbe     fm_havetot2
        mov     ecx,eax
fm_havetot2:
        mov     [fat_totsec],ecx

fm_layout:
;--- derived layout (all LBAs absolute, partition folded in) ---------
        mov     eax,[fat_rsvd]
        add     eax,[fat_partlba]
        mov     [fat_fatlba],eax

        mov     eax,[fat_nfats]
        imul    eax,[fat_spf]
        add     eax,[fat_fatlba]
        mov     [fat_rootlba],eax       ; root area (FAT12/16)

        ; Root DIRECTORY SECTORS, rounded up, which is the specified
        ; arithmetic and not the same as rootent/16 when the count is
        ; not a multiple of sixteen.  A 1.2M floppy has 224 entries and
        ; divides exactly; a hand-formatted volume need not.
        mov     eax,[fat_rootent]
        add     eax,15
        shr     eax,4                   ; * 32, / 512, rounded up
        add     eax,[fat_rootlba]
        mov     [fat_datalba],eax

        ; total clusters = (volume sectors - overhead) / spc
        mov     eax,[fat_totsec]
        add     eax,[fat_partlba]
        sub     eax,[fat_datalba]
        jbe     fm_fail
        xor     edx,edx
        div     dword ptr [fat_spc]
        mov     [fat_totclus],eax

;--- pick the FAT flavour by cluster count (the MS rule) --------------
; BY COUNT, never by the type string at 36h or 52h.  Those are a
; comment: FORMAT wrote "FAT16   " into volumes that are FAT12 by this
; rule, and every implementation that trusted them has misread a disk
; sooner or later.  The partition type byte is no better - a 32MB
; FAT12 volume in an 06h partition is legal and exists.
        cmp     eax,4085
        jb      fm_is12
        cmp     eax,65525
        jb      fm_is16

        mov     byte ptr [fat_type],32
        mov     dword ptr [fat_eoc],0FFFFFF8h
        mov     dword ptr [fat_eocv],0FFFFFFFh
        mov     eax,dword ptr [sec_buf+BPB_ROOTCLUS]
        cmp     eax,2                   ; a root below cluster 2 is not a
        jb      fm_fail                 ; root, it is a corrupt field
        mov     [fat_rootclus],eax

        ; MIRRORING.  With bit 7 of ExtFlags set, only one FAT is live
        ; and the others are stale by the volume's own instruction -
        ; writing all of them would overwrite copies something else is
        ; keeping deliberately.
        movzx   eax,word ptr [sec_buf+BPB_EXTFLAGS]
        test    eax,EXTF_NOMIRROR
        jz      fm_typed
        and     eax,EXTF_ACTIVE
        cmp     eax,[fat_nfats]
        jae     fm_fail                 ; "the live one is number 5 of 2"
        inc     eax                     ; stored +1: see fat_rdcopy
        mov     [fat_active],eax
        jmp     short fm_typed

fm_is16:
        mov     byte ptr [fat_type],16
        mov     dword ptr [fat_eoc],0FFF8h
        mov     dword ptr [fat_eocv],0FFFFh
        jmp     short fm_typed

fm_is12:
        mov     byte ptr [fat_type],12
        mov     dword ptr [fat_eoc],0FF8h
        mov     dword ptr [fat_eocv],0FFFh
        mov     eax,[fat_spf]
        cmp     eax,FATBUF_SIZE / 512   ; whole-FAT cache must fit
        ja      fm_fail

        ; cache the first FAT copy whole
        mov     al,[fat_drive]
        mov     ebx,[fat_fatlba]
        mov     ecx,[fat_spf]
        mov     edi,OFFSET fat_buf
        call    disk_read
        jc      fm_iofail
fm_typed:
        call    fat_checkmedia
        jc      fm_fail

        mov     dword ptr [sec_lba],-1          ; dir cache is empty
        call    ctx_root
        mov     dword ptr [cwd_cnt],0           ; back to the root
        mov     dword ptr [cwd_clus],0
        mov     byte ptr [fat_ok],1
        popad
        clc
        ret
;=====================================================================
; No usable BPB.
;
; A DOS 1.x floppy has none at all - the boot sector is code and an OEM
; string and nothing else - and the media descriptor byte in FAT[0] is
; the ENTIRE description of the format.  Four formats were ever made
; that way and all four are in the table below.
;
; Only floppies get this treatment.  A hard disk with no readable BPB
; is not a 1982 diskette, it is a disk that should not be guessed at.
;=====================================================================
fm_nobpb:
        cmp     byte ptr [fat_drive],80h
        jae     fm_fail

        mov     al,[fat_drive]                  ; FAT[0] - every one of
        mov     ebx,[fat_partlba]               ; these formats puts the
        inc     ebx                             ; first FAT at sector 1
        mov     cx,1
        mov     edi,OFFSET sec_buf
        call    disk_read
        jc      fm_iofail

        mov     al,[sec_buf]
        mov     esi,OFFSET fm_media_tbl
        mov     ecx,FM_MEDIA_N
fm_mlook:
        cmp     al,[esi]
        je      fm_mfound
        add     esi,8
        loop    fm_mlook
        jmp     fm_fail

fm_mfound:
        mov     [fat_media],al
        movzx   eax,byte ptr [esi+1]
        mov     [fat_spc],eax
        movzx   eax,word ptr [esi+2]
        mov     [fat_rootent],eax
        movzx   eax,word ptr [esi+4]
        mov     [fat_spf],eax
        movzx   eax,word ptr [esi+6]
        mov     [fat_totsec],eax
        mov     dword ptr [fat_rsvd],1
        mov     dword ptr [fat_nfats],2
        jmp     fm_layout

;--- media byte, sectors/cluster, root entries, sectors/FAT, total ---
; The 8-sector formats are DOS 1.0 and 1.1; the 9-sector ones are DOS
; 2.0, which also introduced the BPB - so a 9-sector disk reaching
; here was formatted by something that wrote the media byte and not
; the block, which happened.
FM_MEDIA_N      EQU     4
fm_media_tbl:
        db  MEDIA_FE, 1
        dw  64, 1, 320                  ; 160K, one side, 8 sectors
        db  MEDIA_FF, 2
        dw  112, 1, 640                 ; 320K, two sides, 8 sectors
        db  MEDIA_FC, 1
        dw  64, 2, 360                  ; 180K, one side, 9 sectors
        db  MEDIA_FD, 2
        dw  112, 2, 720                 ; 360K, two sides, 9 sectors

;=====================================================================
; fat_checkmedia - is FAT[0] the shape a FAT[0] has to be?
;
; The low byte of the first FAT entry repeats the media descriptor and
; the rest of the entry is all ones.  This is the oldest integrity
; check in the system, and the point of doing it here is that the
; partition scan can hand us a volume whose TYPE BYTE says FAT and
; whose contents are anything at all.
;
; DELIBERATELY LOOSE: it checks the byte is in F0h-FFh, not that it
; equals the BPB's.  A strict comparison rejects real disks - media
; bytes get out of step when a volume is imaged, resized or copied
; sector-by-sector onto different geometry, and DOS itself read those
; disks quite happily.  The loose form still catches the case that
; matters, which is a partition that is not a FAT volume at all.
;=====================================================================
fat_checkmedia:
        push    eax
        push    ebx
        push    ecx
        push    edi
        cmp     byte ptr [fat_type],12
        jne     fcm_read
        mov     al,[fat_buf]                    ; FAT12 is cached whole
        jmp     short fcm_test
fcm_read:
        mov     al,[fat_drive]
        mov     ebx,[fat_fatlba]
        mov     cx,1
        mov     edi,OFFSET clus_buf             ; scratch: the cluster
        call    disk_read                       ; cache is invalid anyway
        jc      fcm_bad
        mov     al,[clus_buf]
fcm_test:
        cmp     al,0F0h
        jb      fcm_bad
        pop     edi
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
fcm_bad:
        pop     edi
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;--- a letter with no volume behind it.  Not a media error and not
;--- worth a prompt: "invalid drive specification" is the answer, and
;--- it comes from the caller seeing CF.
fm_nodrive:
        popad
        stc
        ret

;--- the sectors read perfectly and none of it is a filesystem.  DOS
;--- called that a critical error of its own - "Invalid media type
;--- reading drive A" - and it is the honest answer for a disk that
;--- has never been formatted, which until now was indistinguishable
;--- from one with nothing on it.  Retry means the user has changed
;--- disks, so the whole mount starts again.
fm_fail:
        mov     al,[fat_drive]
        call    crit_media
        cmp     al,CERR_RETRY
        je      fm_start

;--- ...whereas a transfer that FAILED has already been through
;--- INT 24h on its own account; asking twice about one disk is how
;--- you teach people to stop reading the screen.
fm_iofail:
        popad
        stc
        ret

;=====================================================================
; fat_freecount - EAX = the loaded volume's free clusters, all 32 bits
;                 of the answer.  36h then squeezes that into a 16-bit
;                 register and saturates; 7303h does not have to, and
;                 that is the whole reason this came out of fn36.
;=====================================================================
fat_freecount:
        push    ebx
        push    ecx
        push    edx
        xor     ebx,ebx
        mov     ecx,2                   ; the first real cluster number
fct_lp:
        mov     eax,ecx
        call    fat_next
        or      eax,eax
        jnz     fct_used
        inc     ebx
fct_used:
        inc     ecx
        mov     eax,[fat_totclus]
        add     eax,2
        cmp     ecx,eax
        jb      fct_lp
        mov     eax,ebx
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; bios2log - AL = BIOS drive number -> AL = the first logical drive
;            that lives on it.
;
; It exists for one thing: a critical error arrives naming the BIOS
; drive the transfer failed on, and the message has to say a letter.
; With several volumes per disk that letter is no longer unique, and
; the first one is the honest approximation - the alternative is
; telling the user a number they have never seen.
;=====================================================================
bios2log:
        push    ebx
        push    ecx
        push    edx
        mov     dl,al
        movzx   ecx,byte ptr [drv_count]
        or      ecx,ecx
        jz      b2l_out
        mov     ebx,OFFSET drv_tbl
        xor     eax,eax
b2l_scan:
        cmp     [ebx+DRV_BIOS],dl
        je      b2l_out
        inc     eax
        add     ebx,DRV_LEN
        loop    b2l_scan
        xor     eax,eax                 ; nothing on that disk: say A:
b2l_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; vol_select - make logical drive AL (0..3) the loaded volume,
; mounting it if it never was (or if the last attempt failed, so a
; freshly inserted floppy gets another chance).  The outgoing volume's
; state - dirty FAT caches included - is parked in its slot, not
; flushed, exactly as it would have stayed in memory before.
;
; Preserves all registers.  CF set if the drive has no mountable
; volume; the swap has still happened, so callers can simply fail.
;=====================================================================
; vol_swap - the mechanical half on its own: park the loaded volume,
;            pull in drive AL, and drop the caches that were keyed to
;            the one going out.  No mount, no questions.
;
; Separate from vol_select because vol_syncdrv needs exactly this and
; must NOT have the other half: it is called around raw sector I/O,
; and the volume it puts back may be one it has just unmounted on
; purpose.  Mounting it again there - off a disk that is in the middle
; of being rewritten - would undo the very thing that unmounted it.
vol_swap:
        pushad
        cmp     al,[cur_vol]
        je      vsw_same

        ; park the loaded volume in its slot...
        movzx   edi,byte ptr [cur_vol]
        imul    edi,VOL_STATE_SIZE
        add     edi,[vol_slotp]
        mov     esi,OFFSET vol_state
        mov     ecx,VOL_STATE_SIZE / 4
        rep     movsd

        ; ...and pull the requested one in
        mov     [cur_vol],al
        movzx   esi,al
        imul    esi,VOL_STATE_SIZE
        add     esi,[vol_slotp]
        mov     edi,OFFSET vol_state
        mov     ecx,VOL_STATE_SIZE / 4
        rep     movsd

        ; caches keyed by LBA / cluster now describe the wrong disk
        mov     dword ptr [sec_lba],-1
        mov     dword ptr [clusbuf_clus],-1
        mov     dword ptr [dc_curord],-1
vsw_same:
        popad
        ret

vol_select:
        pushad
        call    vol_swap
        cmp     byte ptr [fat_ok],0
        jne     vs_ok
        call    fat_mount               ; not mounted - try it now.  Which
        jc      vs_fail                 ; disk and which partition is the
                                        ; volume table's business now, not
                                        ; a sum on the drive number
vs_ok:
        popad
        clc
        ret
vs_fail:
        popad
        stc
        ret

;=====================================================================
; The FAT itself
;=====================================================================

;---------------------------------------------------------------------
; fat_secload - make sure the FAT sector holding byte offset EAX (from
;               the start of the FAT) is in fatsec_buf.  Preserves all.
;               CF on I/O error.
;---------------------------------------------------------------------
fat_secload:
        pushad
        shr     eax,9                   ; byte offset -> sector index
        add     eax,[fat_fatlba]        ; -> absolute LBA in copy 0
        cmp     eax,[fatsec_lba]
        je      fsl_ok

        call    fat_secflush            ; retire the old sector first
        jc      fsl_fail

        mov     ebx,eax
        push    ebx                     ; fatsec_lba stays a COPY-0 LBA:
        call    fat_rdcopy              ; it is the cache key and every
        mov     al,[fat_drive]          ; caller computes it that way.
        mov     cx,1                    ; Only the transfer moves.
        mov     edi,OFFSET fatsec_buf
        call    disk_read
        pop     ebx
        jc      fsl_fail
        mov     [fatsec_lba],ebx
fsl_ok:
        popad
        clc
        ret
fsl_fail:
        mov     dword ptr [fatsec_lba],-1
        popad
        stc
        ret

;---------------------------------------------------------------------
; Which copy of the FAT to read, and which ones to write.
;
; Normally there are two and they are mirrors: a read comes off the
; first and a write goes to all of them, because a FAT that is only
; right in one copy is a FAT that the next CHKDSK will "repair" from
; the wrong one.
;
; A FAT32 volume can say otherwise.  Bit 7 of BPB_EXTFLAGS means one
; copy is live and the rest are being kept deliberately stale by
; whoever set it - reading copy 0 then gets an out-of-date FAT, and
; writing all of them destroys what the volume asked to preserve.
;
; fat_active holds the live copy number PLUS ONE, so that zero can
; mean "mirror" without colliding with "copy 0 is the live one".
;---------------------------------------------------------------------
fat_rdcopy:                             ; EBX = a copy-0 LBA -> the live one
        push    eax
        mov     eax,[fat_active]
        or      eax,eax
        jz      frc_out
        dec     eax
        imul    eax,[fat_spf]
        add     ebx,eax
frc_out:
        pop     eax
        ret

fat_wrcopies:                           ; -> EBX first, ESI how many
        mov     esi,[fat_nfats]
        cmp     dword ptr [fat_active],0
        je      fwc_out
        call    fat_rdcopy
        mov     esi,1
fwc_out:
        ret

;---------------------------------------------------------------------
; fat_secflush - write the cached FAT sector to every FAT copy if it
;                is dirty.  CF on I/O error.
;---------------------------------------------------------------------
fat_secflush:
        pushad
        cmp     byte ptr [fatsec_dirty],0
        je      fsf_ok
        mov     ebx,[fatsec_lba]
        cmp     ebx,-1
        je      fsf_ok

        call    fat_wrcopies
fsf_copy:
        mov     al,[fat_drive]
        mov     cx,1
        mov     edi,OFFSET fatsec_buf
        call    disk_write
        jc      fsf_fail
        add     ebx,[fat_spf]
        dec     esi
        jnz     fsf_copy
        mov     byte ptr [fatsec_dirty],0
fsf_ok:
        popad
        clc
        ret
fsf_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; fat_next - EAX = cluster in, EAX = successor out (masked).
;            End of chain when EAX >= [fat_eoc].  On I/O trouble the
;            chain simply appears to end.
;---------------------------------------------------------------------
fat_next:
        push    ebx
        push    ecx

        cmp     byte ptr [fat_type],12
        jne     fn_16or32

        mov     ebx,eax                 ; FAT12: whole-FAT cache
        mov     ecx,ebx
        shr     ebx,1
        add     ebx,ecx                 ; EBX = cluster * 3 / 2
        movzx   ebx,word ptr [fat_buf+ebx]
        test    al,1
        jz      fn12_even
        shr     ebx,4
        jmp     short fn12_have
fn12_even:
        and     ebx,0FFFh
fn12_have:
        mov     eax,ebx
        jmp     short fn_out

fn_16or32:
        cmp     byte ptr [fat_type],16
        jne     fn_32

        mov     ebx,eax                 ; FAT16: word entries
        shl     ebx,1                   ; byte offset in the FAT
        mov     eax,ebx
        call    fat_secload
        jc      fn_ioerr
        and     ebx,511
        movzx   eax,word ptr [fatsec_buf+ebx]
        jmp     short fn_out

fn_32:
        mov     ebx,eax                 ; FAT32: dword entries
        shl     ebx,2
        mov     eax,ebx
        call    fat_secload
        jc      fn_ioerr
        and     ebx,511
        mov     eax,dword ptr [fatsec_buf+ebx]
        and     eax,0FFFFFFFh           ; top nibble is reserved
        jmp     short fn_out

fn_ioerr:
        mov     eax,[fat_eocv]          ; pretend the chain ended
fn_out:
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; fat_setnext - point cluster EAX's entry at EDX.  Cache-level only;
;               fat_flush makes it durable.
;---------------------------------------------------------------------
fat_setnext:
        pushad

        cmp     byte ptr [fat_type],12
        jne     fs_16or32

        mov     ebx,eax
        mov     ecx,ebx
        shr     ebx,1
        add     ebx,ecx
        and     edx,0FFFh
        test    al,1
        jz      fs12_even
        shl     dx,4
        mov     cl,[fat_buf+ebx]
        and     cl,0Fh
        or      dl,cl
        mov     word ptr [fat_buf+ebx],dx
        jmp     short fs12_done
fs12_even:
        mov     cx,word ptr [fat_buf+ebx]
        and     cx,0F000h
        or      dx,cx
        mov     word ptr [fat_buf+ebx],dx
fs12_done:
        mov     byte ptr [fat12_dirty],1
        jmp     short fs_out

fs_16or32:
        cmp     byte ptr [fat_type],16
        jne     fs_32

        mov     ebx,eax
        shl     ebx,1
        mov     eax,ebx
        call    fat_secload
        jc      fs_out                  ; flush will complain later
        and     ebx,511
        mov     word ptr [fatsec_buf+ebx],dx
        mov     byte ptr [fatsec_dirty],1
        jmp     short fs_out

fs_32:
        mov     ebx,eax
        shl     ebx,2
        mov     eax,ebx
        call    fat_secload
        jc      fs_out
        and     ebx,511
        mov     eax,dword ptr [fatsec_buf+ebx]
        and     eax,0F0000000h          ; keep the reserved nibble
        and     edx,0FFFFFFFh
        or      eax,edx
        mov     dword ptr [fatsec_buf+ebx],eax
        mov     byte ptr [fatsec_dirty],1
fs_out:
        popad
        ret

;---------------------------------------------------------------------
; fat_alloc - claim a free cluster.  EAX = cluster, CF if full.
;             Starts at the rotating hint so growth stays mostly
;             sequential.
;---------------------------------------------------------------------
fat_alloc:
        push    ebx
        push    ecx
        push    edx

        mov     ebx,[fat_totclus]
        add     ebx,2                   ; EBX = first invalid cluster
        mov     ecx,[fat_hint]
        cmp     ecx,2
        jb      fa_clamp
        cmp     ecx,ebx                 ; a hint past the end would make
        jb      fa_go                   ; the wrap test unreachable
fa_clamp:
        mov     ecx,2
fa_go:
        mov     edx,ecx                 ; EDX = where we started
fa_scan:
        mov     eax,ecx
        call    fat_next
        or      eax,eax
        jz      fa_have
        inc     ecx
        cmp     ecx,ebx
        jb      fa_wrapped
        mov     ecx,2                   ; wrap to the bottom
fa_wrapped:
        cmp     ecx,edx                 ; back at the start = disk full
        jne     fa_scan

        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret
fa_have:
        mov     eax,ecx
        mov     edx,[fat_eocv]
        call    fat_setnext             ; mark it end-of-chain
        lea     edx,[ecx+1]
        mov     [fat_hint],edx
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret

;---------------------------------------------------------------------
; fat_freechain - free the chain starting at cluster EAX (0 = no-op).
;---------------------------------------------------------------------
fat_freechain:
        pushad
ffc_lp:
        cmp     eax,2
        jb      ffc_done
        cmp     eax,[fat_eoc]
        jae     ffc_done
        mov     ebx,eax
        call    fat_next                ; EAX = successor
        push    eax
        mov     eax,ebx
        xor     edx,edx
        call    fat_setnext             ; current -> free
        pop     eax
        jmp     short ffc_lp
ffc_done:
        popad
        ret

;---------------------------------------------------------------------
; fat_flush - make every cached FAT change durable.  CF on I/O error.
;---------------------------------------------------------------------
fat_flush:
        pushad
        cmp     byte ptr [fat_type],12
        jne     ffl_sector

        cmp     byte ptr [fat12_dirty],0
        je      ffl_ok
        mov     ebx,[fat_fatlba]
        call    fat_wrcopies
        mov     edx,ebx
ffl_copy:
        mov     al,[fat_drive]
        mov     ebx,edx
        mov     ecx,[fat_spf]
        mov     edi,OFFSET fat_buf
        call    disk_write
        jc      ffl_fail
        add     edx,[fat_spf]
        dec     esi
        jnz     ffl_copy
        mov     byte ptr [fat12_dirty],0
        jmp     short ffl_ok

ffl_sector:
        call    fat_secflush
        jc      ffl_fail
ffl_ok:
        popad
        clc
        ret
ffl_fail:
        popad
        stc
        ret

;=====================================================================
; vol_syncdrv - the filesystem and somebody else's raw sector transfer,
;               made to agree about BIOS disk AL.
;
;   AL = BIOS drive        EBX = first absolute LBA
;   AH = mode              ECX = sector count
;
;   AH = 0  BEFORE the transfer.  Everything this kernel is holding
;           dirty goes out to the medium, so that whoever is about to
;           read those sectors reads what the filesystem believes is
;           in them rather than the version before the last write.
;
;   AH = 1  AFTER a transfer that WROTE.  Every cache keyed by an LBA
;           or a cluster is now describing something that is gone, so
;           they are dropped - and if the volume's own boot sector was
;           in the range, the volume is unmounted outright, because
;           the BPB this kernel is working from may no longer be the
;           one on the disk.
;
; Mode 1 DISCARDS rather than flushes, and that is the whole point:
; the sectors underneath have just been overwritten by somebody else,
; and writing our copy back over the top would undo their work with
; data we know to be stale.  A caller that writes must therefore call
; mode 0 first - both of the ones in this kernel do.
;
; It runs over EVERY volume on that disk, not just the current one.  A
; disk carries several letters now, and a program writing sectors
; through INT 26h or a DOS box's INT 13h is addressing the DISK: it
; has no idea which of our letters its sectors landed in, and neither
; do we until we look.  Volumes that were never mounted are skipped -
; they hold no cache to be wrong, and mounting one here would mean
; reading a disk somebody is in the middle of rewriting.
;
; Preserves all registers.  Failures are swallowed on purpose: this is
; housekeeping around somebody else's I/O, and the error that matters
; to the caller is the one their own transfer returns.
;=====================================================================
vol_syncdrv:
        pushad
        mov     [vsd_bios],al
        mov     [vsd_mode],ah
        mov     [vsd_lba],ebx
        add     ecx,ebx                         ; the range, as [first,last)
        mov     [vsd_end],ecx
        mov     al,[cur_vol]
        mov     [vsd_back],al

        movzx   ecx,byte ptr [drv_count]
        or      ecx,ecx
        jz      vsd_done
        xor     ebx,ebx                         ; EBX = drive letter
vsd_one:
        push    ecx
        push    ebx

        mov     eax,ebx
        imul    eax,DRV_LEN
        add     eax,OFFSET drv_tbl
        mov     dl,[vsd_bios]
        cmp     [eax+DRV_BIOS],dl               ; a different disk entirely
        jne     vsd_next
        test    byte ptr [eax+DRV_FLAGS],DRVF_PRESENT
        jz      vsd_next

        ; Mounted?  fat_ok is the first byte of vol_state, so it is
        ; also the first byte of a parked slot - which is how a volume
        ; can be asked without being swapped in.
        cmp     bl,[cur_vol]
        je      vsd_islive
        mov     eax,ebx
        imul    eax,VOL_STATE_SIZE
        add     eax,[vol_slotp]
        cmp     byte ptr [eax],0
        je      vsd_next
        jmp     short vsd_take
vsd_islive:
        cmp     byte ptr [fat_ok],0
        je      vsd_next
vsd_take:
        mov     eax,ebx
        call    vol_swap                        ; swap, never mount

        ; does the transfer touch this volume at all?
        mov     eax,[fat_partlba]
        cmp     eax,[vsd_end]
        jae     vsd_next                        ; volume starts after it ends
        add     eax,[fat_totsec]
        cmp     eax,[vsd_lba]
        jbe     vsd_next                        ; volume ends before it starts

        cmp     byte ptr [vsd_mode],0
        jne     vsd_forget
        call    fat_flush
        jmp     short vsd_next

;--- mode 1: the sectors are somebody else's now ---------------------
vsd_forget:
        ; The boot sector in the range means the BPB may have changed
        ; under us - a guest running FORMAT is exactly this case - and
        ; nothing short of a fresh mount can be trusted afterwards.
        mov     eax,[fat_partlba]
        cmp     eax,[vsd_lba]
        jb      vsd_caches
        cmp     eax,[vsd_end]
        jae     vsd_caches
        mov     byte ptr [fat_ok],0             ; remount on next use
        mov     byte ptr [fat12_dirty],0
        mov     byte ptr [fatsec_dirty],0
        jmp     short vsd_next

vsd_caches:
        call    vol_forget

vsd_next:
        pop     ebx
        pop     ecx
        inc     ebx
        dec     ecx
        jnz     vsd_one

vsd_done:
        mov     al,[vsd_back]                   ; back to the caller's volume,
        call    vol_swap                        ; as it stands and not remounted
        popad
        ret

;---------------------------------------------------------------------
; vol_forget - drop every cache the loaded volume keeps, WITHOUT
;              writing any of it back.  The mount itself survives.
;
; FAT12 is the awkward one: the whole FAT lives in memory and nothing
; else would ever reload it, so it is re-read here and now.  If that
; read fails the volume is marked unmounted rather than left with half
; a FAT, which would be a far quieter kind of wrong.
;---------------------------------------------------------------------
vol_forget:
        pushad
        mov     dword ptr [fatsec_lba],-1
        mov     byte ptr [fatsec_dirty],0
        mov     dword ptr [sec_lba],-1
        mov     dword ptr [clusbuf_clus],-1
        mov     dword ptr [dc_curord],-1
        mov     dword ptr [fat_hint],2          ; the free-space hint was a
                                                ; guess about a FAT that has
                                                ; since been rewritten
        cmp     byte ptr [fat_type],12
        jne     vfg_done
        mov     byte ptr [fat12_dirty],0
        mov     al,[fat_drive]
        mov     ebx,[fat_fatlba]
        mov     ecx,[fat_spf]
        mov     edi,OFFSET fat_buf
        call    disk_read
        jnc     vfg_done
        mov     byte ptr [fat_ok],0
vfg_done:
        popad
        ret

;=====================================================================
; Clusters as data
;=====================================================================

;---------------------------------------------------------------------
; clus2lba - EAX = cluster -> EAX = absolute LBA of its first sector.
;---------------------------------------------------------------------
clus2lba:
        push    edx
        sub     eax,2
        imul    eax,[fat_spc]
        add     eax,[fat_datalba]
        pop     edx
        ret

;---------------------------------------------------------------------
; fat_readclus / fat_writeclus - cluster EAX from/to the buffer at EDI.
;                                CF on I/O error.
;---------------------------------------------------------------------
fat_readclus:
        pushad
        call    clus2lba
        mov     ebx,eax
        mov     ecx,[fat_spc]
        mov     al,[fat_drive]
        call    disk_read
        jc      frc_fail
        popad
        clc
        ret
frc_fail:
        popad
        stc
        ret

fat_writeclus:
        pushad
        call    clus2lba
        mov     ebx,eax
        mov     ecx,[fat_spc]
        mov     al,[fat_drive]
        call    disk_write
        jc      fwc_fail
        popad
        clc
        ret
fwc_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; fat_zeroclus - write zeros over cluster EAX (fresh directory or
;                file hole).  Trashes clus_buf.  CF on I/O error.
;---------------------------------------------------------------------
fat_zeroclus:
        pushad
        mov     dword ptr [clusbuf_clus],-1
        push    eax
        mov     edi,OFFSET clus_buf
        xor     eax,eax
        mov     ecx,CLUSBUF_SIZE / 4
        rep     stosd
        pop     eax
        mov     edi,OFFSET clus_buf
        call    fat_writeclus
        jc      fzc_fail
        popad
        clc
        ret
fzc_fail:
        popad
        stc
        ret

;=====================================================================
; Directory entries: the cluster field, in one piece
;=====================================================================
de_getclus:                             ; ESI -> entry, EAX = cluster
        movzx   eax,word ptr [esi+DE_CLUSTER]
        cmp     byte ptr [fat_type],32
        jne     dgc_out
        push    ebx
        movzx   ebx,word ptr [esi+DE_CLUSHI]
        shl     ebx,16
        or      eax,ebx
        pop     ebx
dgc_out:
        ret

de_setclus:                             ; EAX = cluster -> entry at ESI
        mov     [esi+DE_CLUSTER],ax
        push    eax
        shr     eax,16
        mov     [esi+DE_CLUSHI],ax
        pop     eax
        ret

;=====================================================================
; Directory contexts and enumeration
;
; [dc_isroot] = 1: the FAT12/16 fixed root area starting at [dc_start]
;               (an LBA); indexes run to fat_rootent.
; [dc_isroot] = 0: a cluster chain starting at cluster [dc_start].
;=====================================================================

;---------------------------------------------------------------------
; ctx_root - point the context at the volume root directory.
;---------------------------------------------------------------------
ctx_root:
        push    eax
        cmp     byte ptr [fat_type],32
        je      cr_32
        mov     byte ptr [dc_isroot],1
        mov     eax,[fat_rootlba]
        mov     [dc_start],eax
        jmp     short cr_done
cr_32:
        mov     byte ptr [dc_isroot],0
        mov     eax,[fat_rootclus]
        mov     [dc_start],eax
cr_done:
        mov     dword ptr [dc_curord],-1        ; walk cache invalid
        pop     eax
        ret

;---------------------------------------------------------------------
; ctx_chain - point the context at the chain starting at cluster EAX.
;---------------------------------------------------------------------
ctx_chain:
        mov     byte ptr [dc_isroot],0
        mov     [dc_start],eax
        mov     dword ptr [dc_curord],-1
        ret

;---------------------------------------------------------------------
; ctx_save / ctx_restore - stash the context in EAX/EDX form so a
; caller can wander off and come back.
;   ctx_save    -> AL = isroot, EDX = start
;   ctx_restore <- AL = isroot, EDX = start
;---------------------------------------------------------------------
ctx_save:
        mov     al,[dc_isroot]
        mov     edx,[dc_start]
        ret
ctx_restore:
        mov     [dc_isroot],al
        mov     [dc_start],edx
        mov     dword ptr [dc_curord],-1
        ret

;---------------------------------------------------------------------
; dir_seek - EBX = entry index within the context directory.
;            Returns ESI -> the 32-byte entry inside sec_buf.
;            CF set past the end (or on I/O error).
;---------------------------------------------------------------------
dir_seek:
        push    eax
        push    ebx
        push    ecx
        push    edx

        cmp     byte ptr [dc_isroot],0
        je      dsk_chain

        ; fixed root area
        cmp     ebx,[fat_rootent]
        jae     dsk_fail
        mov     eax,ebx
        shr     eax,4                   ; 16 entries per sector
        add     eax,[dc_start]
        jmp     short dsk_load

dsk_chain:
        ; entries per cluster = spc * 16
        mov     ecx,[fat_spc]
        shl     ecx,4
        mov     eax,ebx
        xor     edx,edx
        div     ecx                     ; EAX = cluster ordinal, EDX = entry
        mov     ecx,edx                 ;   within the cluster

        ; walk to that ordinal, using the cached position when it helps
        cmp     eax,[dc_curord]
        jb      dsk_restart
        mov     edx,[dc_curclus]
        push    eax
        sub     eax,[dc_curord]
        xchg    eax,edx                 ; EAX = cluster, EDX = steps left
        jmp     short dsk_walk
dsk_restart:
        mov     edx,eax                 ; steps from the front
        push    eax
        mov     eax,[dc_start]
dsk_walk:
        or      edx,edx
        jz      dsk_arrived
        call    fat_next
        cmp     eax,[fat_eoc]
        jae     dsk_offend
        dec     edx
        jmp     short dsk_walk
dsk_arrived:
        pop     edx                     ; the ordinal we walked to
        mov     [dc_curord],edx
        mov     [dc_curclus],eax

        ; LBA = cluster base + (entry / 16)
        call    clus2lba
        mov     edx,ecx
        shr     edx,4
        add     eax,edx
        and     ecx,15                  ; entry within the sector

dsk_load:
        ; get that sector into sec_buf.  EDI is the caller's (it holds
        ; fat_search's pattern!) - it must survive the read.
        cmp     eax,[sec_lba]
        je      dsk_cached
        push    eax
        push    ecx
        push    edi
        mov     ebx,eax
        mov     al,[fat_drive]
        mov     cx,1
        mov     edi,OFFSET sec_buf
        call    disk_read
        pop     edi
        pop     ecx
        pop     eax
        jc      dsk_ioerr
        mov     [sec_lba],eax
dsk_cached:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax

        ; ESI = sec_buf + (index & 15) * 32... but for chains the entry
        ; offset came from the cluster math, so recompute generically:
        push    eax
        push    ecx
        cmp     byte ptr [dc_isroot],0
        je      dsk_esi_chain
        mov     eax,ebx
        and     eax,15
        jmp     short dsk_esi
dsk_esi_chain:
        mov     ecx,[fat_spc]
        shl     ecx,4
        mov     eax,ebx
        push    edx
        xor     edx,edx
        div     ecx
        mov     eax,edx
        pop     edx
        and     eax,15
dsk_esi:
        shl     eax,5
        lea     esi,[sec_buf+eax]
        pop     ecx
        pop     eax
        clc
        ret

dsk_offend:
        pop     eax                     ; the pushed ordinal
dsk_fail:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret
dsk_ioerr:
        mov     dword ptr [sec_lba],-1
        jmp     short dsk_fail

;---------------------------------------------------------------------
; fat_dirupdate - write the directory sector in sec_buf back to disk.
;---------------------------------------------------------------------
fat_dirupdate:
        pushad
        mov     ebx,[sec_lba]
        cmp     ebx,-1
        je      fdu_fail
        mov     al,[fat_drive]
        mov     cx,1
        mov     edi,OFFSET sec_buf
        call    disk_write
        jc      fdu_fail
        popad
        clc
        ret
fdu_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; dir_extend - hang one zeroed cluster on the end of a chain directory
;              so it gains fresh entries.  CF if this is the fixed
;              root, the disk is full, or I/O fails.
;---------------------------------------------------------------------
dir_extend:
        pushad
        cmp     byte ptr [dc_isroot],0
        jne     dx_fail                 ; the fixed root cannot grow

        call    fat_alloc
        jc      dx_fail
        mov     ebx,eax                 ; EBX = the new cluster
        call    fat_zeroclus
        jc      dx_fail

        ; find the current tail and link it on
        mov     eax,[dc_start]
dx_tail:
        mov     ecx,eax
        call    fat_next
        cmp     eax,[fat_eoc]
        jb      dx_tail
        mov     eax,ecx
        mov     edx,ebx
        call    fat_setnext
        popad
        clc
        ret
dx_fail:
        popad
        stc
        ret

;=====================================================================
; Long-file-name collection (VFAT).
;
; An LFN is a chain of attribute-0Fh entries DIRECTLY BEFORE the 8.3
; entry it names, stored last part first: sequence N OR 40h, then N-1
; down to 1, then the real entry.  Each fragment carries 13 UCS-2
; characters and the checksum of the 8.3 name it belongs to.  The
; helpers below accumulate fragments during a directory walk;
; lfnw_seal, called at every REGULAR entry, decides whether a
; complete, checksum-matched chain landed on it - [lfn_ok] = 1 with
; the ASCIIZ name in lfn_buf and the chain's first index in
; [lfn_first].  Anything out of order - deleted entries, orphaned
; fragments left behind by a plain-DOS delete - just invalidates the
; accumulation, which is how Windows treats it too.
;=====================================================================
lfnw_reset:
        mov     byte ptr [lfn_expect],0
        ret

;---------------------------------------------------------------------
; lfnw_frag - feed one attr-0Fh entry (ESI, index EBX) to the
;             accumulator.  All registers preserved.
;---------------------------------------------------------------------
lfnw_frag:
        push    eax
        push    ecx
        push    esi
        push    edi
        mov     al,[esi]                ; sequence byte
        test    al,40h
        jz      lw_cont
        and     al,3Fh                  ; chain starts: highest sequence
        jz      lw_bad
        cmp     al,20                   ; 20 fragments = 260 chars = max
        ja      lw_bad
        mov     [lfn_expect],al
        mov     ah,[esi+0Dh]
        mov     [lfn_ck],ah
        mov     [lfn_first],ebx
        movzx   edi,al                  ; terminate where the name ends
        imul    edi,13
        mov     byte ptr [lfn_buf+edi],0
        jmp     short lw_copy
lw_cont:
        and     al,3Fh
        jz      lw_bad
        mov     ah,[lfn_expect]
        sub     ah,1                    ; exactly the next one down,
        jz      lw_bad                  ; and never below 1
        cmp     al,ah
        jne     lw_bad
        mov     ah,[esi+0Dh]            ; same checksum all the way
        cmp     ah,[lfn_ck]
        jne     lw_bad
        mov     [lfn_expect],al
lw_copy:
        movzx   edi,byte ptr [lfn_expect]
        dec     edi
        imul    edi,13
        add     edi,OFFSET lfn_buf
        push    esi
        inc     esi                     ; chars 1-5 live at +1
        mov     ecx,5
        call    lw_ucs
        pop     esi
        push    esi
        add     esi,0Eh                 ; chars 6-11
        mov     ecx,6
        call    lw_ucs
        pop     esi
        push    esi
        add     esi,1Ch                 ; chars 12-13
        mov     ecx,2
        call    lw_ucs
        pop     esi
        jmp     short lw_out
lw_bad:
        mov     byte ptr [lfn_expect],0
lw_out:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        ret

; lw_ucs - ECX UCS-2 characters at ESI -> bytes at EDI.  Anything
;          beyond ASCII becomes '_'; the 0000 terminator crosses as 0
;          and the FFFF padding as '_', both landing past the real end.
lw_ucs:
        mov     al,[esi]
        cmp     byte ptr [esi+1],0
        je      lw_u1
        mov     al,'_'
lw_u1:
        mov     [edi],al
        add     esi,2
        inc     edi
        loop    lw_ucs
        ret

;---------------------------------------------------------------------
; lfnw_seal - ESI -> the regular entry that follows the fragments.
;             Sets [lfn_ok] when a complete chain with the right
;             checksum precedes it; the accumulator is consumed
;             either way.  All registers preserved.
;---------------------------------------------------------------------
lfnw_seal:
        push    eax
        mov     byte ptr [lfn_ok],0
        cmp     byte ptr [lfn_expect],1 ; did the chain run down to 1?
        jne     ls_out
        call    lfn_cksum
        cmp     al,[lfn_ck]
        jne     ls_out
        mov     byte ptr [lfn_ok],1
ls_out:
        mov     byte ptr [lfn_expect],0
        pop     eax
        ret

;---------------------------------------------------------------------
; lfn_cksum - AL = the VFAT checksum of the 11-byte 8.3 name at ESI:
;             sum = ror8(sum) + byte over all eleven characters.
;             Everything else preserved.
;---------------------------------------------------------------------
lfn_cksum:
        push    ecx
        push    esi
        xor     al,al
        mov     ecx,11
lck_lp:
        ror     al,1
        add     al,[esi]
        inc     esi
        loop    lck_lp
        pop     esi
        pop     ecx
        ret

;---------------------------------------------------------------------
; lfn_tomb - tombstone the LFN chain preceding the entry fat_search
;            just found (EBX = its index).  Call BEFORE editing the
;            entry, and dir_seek again AFTER - the walk here may page
;            a different sector into the cache.  No-op when the hit
;            carried no chain.  This is what keeps our deletes and
;            renames from strewing orphaned fragments around.
;---------------------------------------------------------------------
lfn_tomb:
        cmp     byte ptr [lfn_ok],0
        je      lt_out
        pushad
        mov     edx,[lfn_first]
lt_lp:
        cmp     edx,ebx
        jae     lt_done
        push    ebx
        push    edx
        mov     ebx,edx
        call    dir_seek
        jc      lt_pop
        mov     byte ptr [esi],0E5h
        call    fat_dirupdate
lt_pop:
        pop     edx
        pop     ebx
        inc     edx
        jmp     short lt_lp
lt_done:
        popad
        mov     byte ptr [lfn_ok],0
lt_out:
        ret

;=====================================================================
; The write side (v0.12): alias generation and chain creation.
;=====================================================================

;---------------------------------------------------------------------
; lfn_alias - make a unique ~N 8.3 alias for the long name at ESI
;             (ECX characters) in the CONTEXT directory, Win95 style:
;             an up-cased basis from the name part (spaces and dots
;             dropped, characters 8.3 cannot hold turned into '_'),
;             '~N' spliced in so the whole stays within 8, and the
;             extension taken from after the LAST dot.  N counts up
;             until the candidate is unclaimed; the winner lands in
;             name_buf.  Trashes [srch_attr].  CF when 9999
;             candidates are somehow all taken.
;---------------------------------------------------------------------
lfn_alias:
        pushad

        ; leading dots carry no basis material (".profile")
la_skipdot:
        or      ecx,ecx
        jz      la_split
        cmp     byte ptr [esi],'.'
        jne     la_split
        inc     esi
        dec     ecx
        jmp     short la_skipdot

la_split:
        ; EDX = offset of the LAST dot, or -1 when there is none
        mov     edx,-1
        xor     ebx,ebx
la_dot:
        cmp     ebx,ecx
        jae     la_basis
        cmp     byte ptr [esi+ebx],'.'
        jne     la_d1
        mov     edx,ebx
la_d1:
        inc     ebx
        jmp     short la_dot

la_basis:
        ; collect up to 8 basis characters from before that dot
        push    esi
        push    ecx
        cmp     edx,-1
        je      la_blim
        mov     ecx,edx                 ; only the name part
la_blim:
        mov     edi,OFFSET la_base
        xor     ebx,ebx
la_b1:
        or      ecx,ecx
        jz      la_bdone
        cmp     ebx,8
        jae     la_bdone
        mov     al,[esi]
        inc     esi
        dec     ecx
        cmp     al,' '
        jbe     la_b1                   ; spaces and control vanish
        cmp     al,'.'
        je      la_b1                   ; embedded dots vanish
        call    la_map
        mov     [edi+ebx],al
        inc     ebx
        jmp     short la_b1
la_bdone:
        mov     [la_blen],ebx
        pop     ecx
        pop     esi

        ; and up to 3 extension characters from after it
        xor     ebx,ebx
        cmp     edx,-1
        je      la_edone
        push    esi
        push    ecx
        lea     esi,[esi+edx+1]
        sub     ecx,edx
        dec     ecx                     ; chars after the dot
        mov     edi,OFFSET la_ext
la_e1:
        or      ecx,ecx
        jz      la_epop
        cmp     ebx,3
        jae     la_epop
        mov     al,[esi]
        inc     esi
        dec     ecx
        cmp     al,' '
        jbe     la_e1
        call    la_map
        mov     [edi+ebx],al
        inc     ebx
        jmp     short la_e1
la_epop:
        pop     ecx
        pop     esi
la_edone:
        mov     [la_elen],ebx

        mov     dword ptr [la_num],1
la_try:
        ; render N's digits (most significant first)
        mov     eax,[la_num]
        xor     ecx,ecx
        mov     ebx,10
la_div:
        xor     edx,edx
        div     ebx
        push    edx
        inc     ecx
        or      eax,eax
        jnz     la_div
        mov     [la_ndig],ecx
        mov     edi,OFFSET la_dig
la_dpop:
        pop     eax
        add     al,'0'
        mov     [edi],al
        inc     edi
        loop    la_dpop

        ; the candidate: basis chars, '~', digits, extension
        mov     edi,OFFSET name_buf
        mov     ecx,11
        mov     al,' '
la_blank:
        mov     [edi],al
        inc     edi
        loop    la_blank

        mov     edx,7                   ; 8 minus the '~' minus digits
        sub     edx,[la_ndig]
        cmp     edx,[la_blen]
        jbe     la_keep
        mov     edx,[la_blen]
la_keep:
        push    esi
        mov     edi,OFFSET name_buf
        mov     esi,OFFSET la_base
        mov     ecx,edx
        rep     movsb
        mov     byte ptr [edi],'~'
        inc     edi
        mov     esi,OFFSET la_dig
        mov     ecx,[la_ndig]
        rep     movsb
        mov     edi,OFFSET name_buf+8
        mov     esi,OFFSET la_ext
        mov     ecx,[la_elen]
        rep     movsb
        pop     esi

        ; unclaimed?  A lookup by name sees files, directories, the
        ; hidden and the system alike - anything wearing it counts.
        mov     byte ptr [srch_attr],ATTR_DIR
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      la_free
        inc     dword ptr [la_num]
        cmp     dword ptr [la_num],10000
        jb      la_try
        popad
        stc
        ret
la_free:
        popad
        clc
        ret

; la_map - AL to its 8.3 self: letters up-cased, characters that
;          cannot live in an 8.3 name turned into '_'.
la_map:
        cmp     al,'a'
        jb      la_m1
        cmp     al,'z'
        ja      la_m1
        sub     al,20h
        ret
la_m1:
        cmp     al,'+'
        je      la_mbad
        cmp     al,','
        je      la_mbad
        cmp     al,';'
        je      la_mbad
        cmp     al,'='
        je      la_mbad
        cmp     al,'['
        je      la_mbad
        cmp     al,']'
        je      la_mbad
        cmp     al,'"'
        je      la_mbad
        ret
la_mbad:
        mov     al,'_'
        ret

;---------------------------------------------------------------------
; lfn_store - write the LFN chain for the name at ESI (ECX chars, the
;             alias already in name_buf) into the context directory.
;             Claims ceil(len/13)+1 CONTIGUOUS free slots - VFAT
;             fragments must directly precede their 8.3 entry in
;             enumeration order - growing a chain directory as needed.
;             The fragments are written through; the LAST slot is left
;             for the caller's 8.3 entry: EBX = its index, ESI -> its
;             32 bytes, freshly dir_seek'd.  CF when the directory can
;             neither offer nor grow the run.
;---------------------------------------------------------------------
lfn_store:
        push    ecx
        push    edx
        push    edi
        mov     [lst_txt],esi
        mov     [lst_len],ecx

        mov     eax,ecx                 ; fragments = ceil(len / 13)
        add     eax,12
        xor     edx,edx
        mov     ecx,13
        div     ecx
        mov     [lst_frag],eax
        inc     eax
        mov     [lst_need],eax          ; plus the 8.3 slot itself

        ; hunt for a contiguous run of free entries
        xor     ebx,ebx
        xor     edx,edx                 ; EDX = run length so far
lst_scan:
        call    dir_seek
        jnc     lst_look
        call    dir_extend              ; chain directories can grow
        jc      lst_fail
        call    dir_seek                ; same index - now a fresh,
        jc      lst_fail                ; zeroed cluster
lst_look:
        mov     al,[esi]
        or      al,al
        jz      lst_free
        cmp     al,0E5h
        je      lst_free
        xor     edx,edx                 ; occupied: run broken
        jmp     short lst_next
lst_free:
        or      edx,edx
        jnz     lst_run
        mov     [lst_start],ebx
lst_run:
        inc     edx
        cmp     edx,[lst_need]
        je      lst_have
lst_next:
        inc     ebx
        jmp     short lst_scan

lst_have:
        push    esi                     ; checksum of the alias
        mov     esi,OFFSET name_buf
        call    lfn_cksum
        pop     esi
        mov     [lst_ck],al

        ; write the fragments, last part first: the highest sequence
        ; (wearing 40h) at the run start, down to 1 just before the
        ; 8.3 slot
        mov     edx,[lst_frag]          ; EDX = sequence this entry
        mov     ebx,[lst_start]
lst_wr:
        or      edx,edx
        jz      lst_done
        call    dir_seek
        jc      lst_fail
        push    ecx                     ; blank the 32 bytes
        push    edi
        mov     edi,esi
        xor     eax,eax
        mov     ecx,8
        rep     stosd
        pop     edi
        pop     ecx
        mov     eax,edx                 ; sequence byte, attr, checksum
        cmp     ebx,[lst_start]
        jne     lst_seq
        or      al,40h
lst_seq:
        mov     [esi],al
        mov     byte ptr [esi+11],ATTR_LFN
        mov     al,[lst_ck]
        mov     [esi+13],al

        push    edx                     ; the 13 character cells
        mov     eax,edx
        dec     eax
        imul    eax,13
        mov     [lst_pos],eax           ; first character this one holds
        push    edi
        lea     edi,[esi+1]             ; cells 1-5
        mov     ecx,5
        call    lsx_cells
        lea     edi,[esi+0Eh]           ; cells 6-11
        mov     ecx,6
        call    lsx_cells
        lea     edi,[esi+1Ch]           ; cells 12-13
        mov     ecx,2
        call    lsx_cells
        pop     edi
        pop     edx
        call    fat_dirupdate
        jc      lst_fail
        inc     ebx
        dec     edx
        jmp     short lst_wr

lst_done:
        call    dir_seek                ; EBX = the 8.3 slot, handed
        jc      lst_fail                ; back seek'd for the caller
        pop     edi
        pop     edx
        pop     ecx
        clc
        ret
lst_fail:
        pop     edi
        pop     edx
        pop     ecx
        stc
        ret

; lsx_cells - fill ECX UCS-2 cells at EDI: [lst_pos] indexes the name
;             ([lst_txt]/[lst_len]); a character crosses with a zero
;             high byte, the terminator as 0000, everything past it
;             as FFFF.  Advances EDI and [lst_pos].
lsx_cells:
        push    eax
        push    ebx
lsx_lp:
        mov     eax,[lst_pos]
        cmp     eax,[lst_len]
        ja      lsx_pad
        je      lsx_term
        mov     ebx,[lst_txt]
        mov     al,[ebx+eax]
        mov     [edi],al
        mov     byte ptr [edi+1],0
        jmp     short lsx_adv
lsx_term:
        mov     word ptr [edi],0
        jmp     short lsx_adv
lsx_pad:
        mov     word ptr [edi],0FFFFh
lsx_adv:
        inc     dword ptr [lst_pos]
        add     edi,2
        loop    lsx_lp
        pop     ebx
        pop     eax
        ret

;=====================================================================
; fat_search - find a directory entry in the context directory.
;
;   EBX = index to start at, EDI = 11-character pattern ('?' wild)
;   [srch_attr] = 08h: volume labels only
;                 else: files, plus directories when bit 4 is set
;   [srch_long] = ASCIIZ-fragment pointer ([srch_llen] chars): a long
;                 name to ALSO accept, case-insensitively, when the
;                 8.3 pattern says no.  CONSUMED - cleared on every
;                 exit, so each search must be armed afresh.
;                 resolve_parent arms it for the final component.
;
; CF clear: EBX = hit index, ESI -> the live entry (sec_buf).  When
; the hit carried a long name, [lfn_ok] = 1 with the name in lfn_buf
; and the chain's first index in [lfn_first] - lfn_tomb consumes them.
;=====================================================================
fat_search:
        call    lfnw_reset              ; no leftovers from last time
        mov     byte ptr [lfn_ok],0
        cmp     byte ptr [fat_ok],0
        je      fsr_fail
fsr_loop:
        call    dir_seek
        jc      fsr_fail

        mov     al,[esi]
        or      al,al                   ; 00 = nothing further
        jz      fsr_fail
        cmp     al,0E5h                 ; deleted: breaks any chain
        je      fsr_del

        mov     al,[esi+DE_ATTR]
        and     al,ATTR_LFN
        cmp     al,ATTR_LFN             ; a long-name fragment: collect
        jne     fsr_real                ; it and move along
        call    lfnw_frag
        jmp     fsr_next
fsr_del:
        call    lfnw_reset
        jmp     fsr_next

fsr_real:
        call    lfnw_seal               ; publish any finished chain

        test    byte ptr [srch_attr],ATTR_VOLUME
        jz      fsr_notvol
        mov     al,[esi+DE_ATTR]        ; volume-label search.  The 0Fh
        and     al,ATTR_VOLUME OR ATTR_DIR      ; LFN case was peeled
        cmp     al,ATTR_VOLUME          ; off above, so a fragment can
        jne     fsr_next                ; no longer pass as the label
        jmp     short fsr_match
fsr_notvol:
        test    byte ptr [esi+DE_ATTR],ATTR_VOLUME
        jnz     fsr_next                ; labels never match normal searches

        test    byte ptr [srch_attr],SRCH_FILTER
        jz      fsr_notdir              ; a lookup by name sees everything
        mov     al,[esi+DE_ATTR]        ; hidden/system stay out of a
        and     al,ATTR_HIDDEN OR ATTR_SYSTEM   ; directory search unless
        jz      fsr_notdir                      ; the caller asked
        mov     ah,[srch_attr]
        not     ah
        and     al,ah                   ; attributes not asked for
        jnz     fsr_next
fsr_notdir:
        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jz      fsr_match
        test    byte ptr [srch_attr],ATTR_DIR
        jz      fsr_next                ; directories only when asked for
fsr_match:
        push    ecx
        push    esi
        push    edi
        mov     ecx,11
fsr_cmp:
        mov     al,[edi]
        cmp     al,'?'
        je      fsr_wild
        cmp     al,[esi]
        jne     fsr_miss
fsr_wild:
        inc     esi
        inc     edi
        loop    fsr_cmp
        pop     edi
        pop     esi
        pop     ecx
fsr_hit:
        mov     dword ptr [srch_long],0 ; consumed
        clc
        ret

fsr_miss:
        pop     edi
        pop     esi
        pop     ecx
        ; the 8.3 side said no - does the long name say yes?
        cmp     byte ptr [lfn_on],0     ; not under LFN=OFF: strict
        je      fsr_next                ; 8.3, exactly as real DOS
        cmp     dword ptr [srch_long],0
        je      fsr_next
        cmp     byte ptr [lfn_ok],0
        je      fsr_next
        push    ecx
        push    esi
        push    edi
        mov     esi,[srch_long]
        mov     ecx,[srch_llen]
        mov     edi,OFFSET lfn_buf
fsr_lcmp:
        or      ecx,ecx
        jz      fsr_lend
        mov     al,[esi]
        call    fsr_fold
        mov     ah,al
        mov     al,[edi]
        call    fsr_fold
        cmp     al,ah
        jne     fsr_lmiss
        inc     esi
        inc     edi
        dec     ecx
        jmp     short fsr_lcmp
fsr_lend:
        cmp     byte ptr [edi],0        ; the whole name, not a prefix
        jne     fsr_lmiss
        pop     edi
        pop     esi
        pop     ecx
        jmp     short fsr_hit
fsr_lmiss:
        pop     edi
        pop     esi
        pop     ecx
fsr_next:
        inc     ebx
        jmp     fsr_loop
fsr_fail:
        mov     dword ptr [srch_long],0 ; consumed either way
        stc
        ret

; fsr_fold - AL to upper case; letters only, so '@' stays clear of '`'
fsr_fold:
        cmp     al,'a'
        jb      fsr_fret
        cmp     al,'z'
        ja      fsr_fret
        sub     al,20h
fsr_fret:
        ret

;=====================================================================
; fat_findfree - find (or make) a free slot in the context directory.
;                EBX = index, ESI -> entry.  CF if the directory can
;                neither offer nor grow one.
;=====================================================================
fat_findfree:
        xor     ebx,ebx
fff_lp:
        call    dir_seek
        jc      fff_grow
        mov     al,[esi]
        or      al,al
        jz      fff_have
        cmp     al,0E5h
        je      fff_have
        inc     ebx
        jmp     short fff_lp
fff_have:
        clc
        ret
fff_grow:
        call    dir_extend              ; chain dirs can gain a cluster
        jc      fff_full
        call    dir_seek                ; same index again - now valid
        jc      fff_full
        clc
        ret
fff_full:
        stc
        ret

;=====================================================================
; fn_conv - convert one path component to a padded 8.3 pattern.
;
;   ESI = text (stops at '\', '/' or NUL - left pointing AT the stop)
;   EDI = receives 11 bytes, uppercased, '*' expanded to '?'
;
; CF set if the component is empty.  [fc_long] is set when the 8.3
; rendering LOST something - a truncated part, a space, an extra dot,
; a leading dot - i.e. when only a long name could spell what was
; typed.  The resolver then matches the raw text against LFNs instead
; of trusting the mangled pattern, which used to open the WRONG file:
; "LONGFILENAME.TXT" truncated to LONGFILE.TXT and matched it.
;=====================================================================
fn_conv:
        push    ebx
        push    ecx
        push    edx
        push    edi

        push    edi                     ; blank the output to spaces
        mov     ecx,11
        mov     ah,' '
fc_blank:
        mov     [edi],ah
        inc     edi
        loop    fc_blank
        pop     edi

        xor     ecx,ecx                 ; chars taken this part
        mov     edx,8                   ; chars allowed
        xor     ebx,ebx                 ; total chars consumed
fc_ch:
        mov     ah,[esi]
        or      ah,ah
        jz      fc_end
        cmp     ah,'\'
        je      fc_end
        cmp     ah,'/'
        je      fc_end
        inc     ebx
        cmp     ah,'.'
        je      fc_dot
        cmp     ah,'*'
        je      fc_star
        cmp     ah,' '                  ; a space cannot live in 8.3
        jne     fc_case
        mov     byte ptr [fc_long],1
fc_case:
        cmp     ah,'a'
        jb      fc_store
        cmp     ah,'z'
        ja      fc_store
        sub     ah,20h
fc_store:
        cmp     ecx,edx
        jae     fc_trunc                ; truncation = long territory
        mov     [edi],ah
        inc     edi
        inc     ecx
fc_skip:
        inc     esi
        jmp     short fc_ch
fc_trunc:
        mov     byte ptr [fc_long],1
        jmp     short fc_skip

fc_star:
fc_star1:
        cmp     ecx,edx
        jae     fc_starx
        mov     byte ptr [edi],'?'
        inc     edi
        inc     ecx
        jmp     short fc_star1
fc_starx:
        inc     esi
        jmp     short fc_ch

fc_dot:
        ; "." and ".." are literal names, not name.ext splits
        cmp     ebx,1                   ; first char of the component?
        jne     fc_dotsplit
        mov     byte ptr [edi],'.'
        inc     edi
        inc     ecx
        inc     esi
        cmp     byte ptr [esi],'.'      ; ".." ?
        jne     fc_dot1
        mov     byte ptr [edi],'.'
        inc     edi
        inc     ecx
        inc     esi
        inc     ebx
fc_dot1:
        ; a name that merely STARTS with a dot (".profile") is not a
        ; legal 8.3 name at all - only an LFN can spell it
        mov     ah,[esi]
        or      ah,ah
        jz      fc_ch
        cmp     ah,'\'
        je      fc_ch
        cmp     ah,'/'
        je      fc_ch
        mov     byte ptr [fc_long],1
        jmp     short fc_ch
fc_dotsplit:
        cmp     edx,3                   ; a SECOND dot: "a.b.c" is
        jne     fc_dosplit2             ; long-name territory too
        mov     byte ptr [fc_long],1
fc_dosplit2:
        mov     eax,edx                 ; jump to the extension field
        sub     eax,ecx
        add     edi,eax
        xor     ecx,ecx
        mov     edx,3
        inc     esi
        jmp     short fc_ch

fc_end:
        pop     edi
        or      ebx,ebx                 ; empty component?
        jz      fc_fail
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
fc_fail:
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; chk_wild - CF set if the 11-byte pattern at EDI contains '?'.
;=====================================================================
chk_wild:
        push    ecx
        push    edi
        mov     ecx,11
cw_lp:
        cmp     byte ptr [edi],'?'
        je      cw_wild
        inc     edi
        loop    cw_lp
        pop     edi
        pop     ecx
        clc
        ret
cw_wild:
        pop     edi
        pop     ecx
        stc
        ret

;=====================================================================
; Filesystem state
;
; vol_state..vol_state_end is ONE volume: geometry, FAT caches (dirty
; flags included) and the current directory.  vol_select swaps it with
; the per-drive copies in vol_slots.  The directory context (dc_*) and
; the plain sector/cluster caches stay outside: they are per-operation
; scratch, invalidated on a swap rather than preserved.
;=====================================================================
srch_attr   db  0                       ; attribute filter for fat_search
cur_vol     db  0                       ; logical drive held in vol_state
fc_long     db  0                       ; fn_conv: 8.3 rendering was lossy
            db  0                       ; alignment

; long-name search and collection state (per-operation scratch)
srch_long   dd  0                       ; ASCIIZ fragment to also match
srch_llen   dd  0                       ; its length in characters
lfn_expect  db  0                       ; next sequence number due, 0 = idle
lfn_ok      db  0                       ; a chain sealed onto the last entry
lfn_ck      db  0                       ; the chain's 8.3 checksum
            db  0
lfn_first   dd  0                       ; index of the chain's first entry
lfn_buf     db  LFN_MAX + 4 dup(0)      ; the collected name, ASCIIZ

; write-side scratch: alias generation (la_*) and chain storing (lst_*)
la_base     db  8 dup(0)                ; the alias basis characters
la_ext      db  3 dup(0)                ; and extension
la_dig      db  5 dup(0)                ; N rendered as digits
la_blen     dd  0
la_elen     dd  0
la_num      dd  0                       ; the ~N being tried
la_ndig     dd  0
lst_txt     dd  0                       ; the long name being written
lst_len     dd  0
lst_frag    dd  0                       ; fragments it needs
lst_need    dd  0                       ; fragments + the 8.3 slot
lst_start   dd  0                       ; first index of the free run
lst_pos     dd  0                       ; character cursor for lsx_cells
lst_ck      db  0                       ; the alias checksum
            db  0,0,0

; A SYSTEM setting, not a volume's - deliberately outside vol_state,
; where a volume swap could never overwrite it.  LFN=OFF in
; CONFIG.SYS clears it: 71xxh answers 7100h and the classic calls
; match strict 8.3 only.  Deletes and renames still tombstone LFN
; fragments - invisible to an 8.3 world, and it keeps the volume
; clean for the next LFN=ON boot.
lfn_on      db  1
            db  0,0,0

vol_state:
fat_ok      db  0
fat_drive   db  0                       ; BIOS drive of this volume
fat_type    db  0                       ; 12, 16 or 32
fat12_dirty db  0
fatsec_dirty db 0
            db  0,0,0                   ; pad to a dword boundary

fat_media   db  0                       ; the BPB's media descriptor
            db  0,0,0

fat_spc     dd  0
fat_rsvd    dd  0
fat_nfats   dd  0
fat_rootent dd  0
fat_spf     dd  0
fat_totsec  dd  0
fat_partlba dd  0
fat_partsecs dd 0                       ; from the partition entry, 0 if
                                        ; the whole medium is the volume
fat_active  dd  0                       ; FAT32: the live FAT when the
                                        ; volume says not to mirror
fat_fatlba  dd  0
fat_rootlba dd  0
fat_rootclus dd 0
fat_datalba dd  0
fat_totclus dd  0
fat_eoc     dd  0                       ; values >= this end a chain
fat_eocv    dd  0                       ; the value we write for EOC
fat_hint    dd  2

fatsec_lba  dd  -1                      ; LBA held in fatsec_buf (copy 0)

cwd_cnt     dd  0                       ; current directory (per volume)
cwd_clus    dd  0                       ; its start cluster (0 = root)
cwd_names   db  MAX_DEPTH * 11 dup(0)   ; 88 bytes, a dword multiple

fatsec_buf  db  512 dup(0)
fat_buf     db  FATBUF_SIZE dup(0)
vol_state_end:

IF (vol_state_end - vol_state) - VOL_STATE_SIZE
.ERR    ; VOL_STATE_SIZE no longer matches the block - update the EQU
ENDIF

;---------------------------------------------------------------------
; The parked per-drive state, and it is a POINTER rather than a block.
;
; Eight drives at 8.7K each is seventy kilobytes, and as a "dup(0)" in
; a raw binary that is seventy kilobytes of zeros in PMDOS.SYS - read
; off the disk at every boot to arrive at the value they already had.
; It comes out of the 32-bit arena at startup instead, which costs one
; allocation and makes the drive count free to grow.
;---------------------------------------------------------------------
vol_slotp   dd  0

;---------------------------------------------------------------------
; The volume table: one entry per drive letter, built once by
; disk_scan.  Outside vol_state on purpose - it describes where every
; volume IS, so a volume swap must never be able to overwrite it.
;---------------------------------------------------------------------
drv_count   db  0
ds_disk     db  0                       ; the disk disk_scan is walking
ds_taken    db  MAX_PHYS dup(0)         ; which primary pass 1 claimed
            db  0,0,0,0
ds_extlba   dd  MAX_PHYS dup(0)         ; and where its extended chain
                                        ; starts, 0 = it has none
drv_tbl     db  (MAX_DRIVES * DRV_LEN) dup(0)

; vol_syncdrv's working copies.  Outside vol_state on purpose: the
; routine swaps volumes underneath itself, so anything it needs to
; remember has to live somewhere a swap cannot park.
vsd_bios    db  0
vsd_mode    db  0
vsd_back    db  0
            db  0
vsd_lba     dd  0
vsd_end     dd  0

dc_isroot   db  0
            db  0,0,0
dc_start    dd  0
dc_curord   dd  -1                      ; chain walk cache
dc_curclus  dd  0

sec_lba     dd  -1                      ; LBA held in sec_buf, -1 = none

sec_buf     db  512 dup(0)
clus_buf    db  CLUSBUF_SIZE dup(0)
