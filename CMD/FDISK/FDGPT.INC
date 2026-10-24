;=====================================================================
; FDGPT.INC - GPT DISKS.  Included by FDISK.ASM after the verbs.
;
; A disk whose master boot record holds a protective entry (EEh) keeps
; its partitions in the GUID partition table instead: a header at
; sector 1 and an array of 128-byte entries behind it, both kept twice -
; the backup at the end of the disk - and each checked by a CRC-32.
;
; THE ENGINE SEES A GPT DISK THROUGH A SHADOW TABLE.  Each entry is
; made into a sixteen-byte record shaped like a master boot record's
; (gsh_tbl), and fd_slotptr and fd_entptr hand the verbs a pointer into
; that as they would into mbr_buf - so planning, moving, resizing,
; creating and deleting are the code that already does them for a
; primary partition.  Where a verb writes its table back, gpt_store
; makes the shadow into entries again and writes both copies, with
; their CRCs.  Partition p is entry p-1.
;
; What a record carries:
;   PE_BOOT    80h when the entry's legacy BIOS bootable bit is set
;   PE_TYPE    a type byte standing for the entry's type: 0Ch for basic
;              data - FAT, or nothing yet - so that the engine moves and
;              resizes it; 07h for exFAT and NTFS, EFh for the EFI system
;              partition, 83h and 82h for Linux, 27h for recovery, and
;              GPT_PTOTHER for the rest
;   PE_START   the first sector
;   PE_SECS    the length
; and gsh_kindp says what the entry is called (GK_*).
;
; THIRTY-TWO BITS, AS EVERYTHING IN THIS PROGRAM IS.  An entry that
; starts or ends past them is shown, takes its room, and is never
; written (GSF_FAR).
;
; WHAT IS EDITED is the array every tool makes: entries of 128 bytes,
; at most 128 of them.  A GPT of any other shape is refused rather than
; guessed at, and so is one where neither copy checks.
;=====================================================================

GTT_LEN         EQU     18              ; a row of gtt_tbl, and how many
GTT_N           EQU     7
GGU_SEEDLEN     EQU     7*4 + 16        ; ggu_seed and ggu_last, which follows it

;---------------------------------------------------------------------
; gpt_check - the table in mbr_buf, of the disk in pl_drive: is it a
;             GPT disk?  dsk_rec must be that disk's.  -> gpt_on, and
;             the shadow table when it is one.  CF set when the master
;             boot record says GPT and no copy of the GPT checks: a
;             disk nothing here may change.
;---------------------------------------------------------------------
gpt_check:
        pushad
        mov     byte ptr [gpt_on],0
        mov     byte ptr [gpt_bad],0
        mov     esi,[mbr_buf]
        add     esi,PART_OFF
        xor     ecx,ecx
gck_slot:
        cmp     byte ptr [esi+PE_TYPE],PT_GPT
        je      gck_gpt
        add     esi,PE_LEN
        inc     ecx
        cmp     ecx,PART_MAX
        jb      gck_slot
        popad
        clc
        ret
gck_gpt:
        call    gpt_load
        jc      gck_bad
        call    gsh_build
        mov     byte ptr [gpt_on],1
        popad
        clc
        ret
gck_bad:
        mov     byte ptr [gpt_bad],1
        popad
        stc
        ret

;---------------------------------------------------------------------
; fd_tblcheck - after a verb has read the table: the GPT, if the disk
;               has one, and the partition number checked against it.
;               On a GPT disk a number past four names an entry, not a
;               logical drive.  Jumps to the error labels itself, as
;               fd_loaddisk does.
;---------------------------------------------------------------------
fd_tblcheck:
        call    gpt_check
        jc      fdn_gptbad
        cmp     byte ptr [gpt_on],0
        je      ftc_mbr
        mov     eax,[pl_log]
        or      eax,eax
        jz      ftc_entry
        add     eax,PART_MAX - 1
        mov     [pl_idx],eax
        mov     dword ptr [pl_log],0
ftc_entry:
        mov     eax,[pl_idx]
        cmp     eax,[gpt_cnt]
        jae     fdn_gptnum
        ret
ftc_mbr:
        cmp     dword ptr [pl_log],EBR_MAX
        ja      fdn_nolog
        ret

;--- fd_gptfar - CF clear if the entry pl_idx names may be changed ----
fd_gptfar:
        push    eax
        cmp     byte ptr [gpt_on],0
        je      fgf_ok
        mov     eax,[gsh_flagp]
        add     eax,[pl_idx]
        test    byte ptr [eax],GSF_FAR
        jnz     fgf_no
fgf_ok:
        pop     eax
        clc
        ret
fgf_no:
        pop     eax
        stc
        ret

fdn_gptbad:
        mov     edx,OFFSET msg_gptbad
        call    puts
        jmp     fd_fail
fdn_gptnum:
        mov     edx,OFFSET msg_gptnum
        call    puts
        jmp     fd_fail
fdn_gptfar:
        mov     edx,OFFSET msg_gptfar
        call    puts
        jmp     fd_fail

;---------------------------------------------------------------------
; gpt_load - the GPT of pl_drive: the primary copy if it checks, else
;            the backup in the disk's last sector.  CF if neither does.
;---------------------------------------------------------------------
gpt_load:
        pushad
        mov     byte ptr [gpt_which],1
        mov     ebx,1
        call    gpt_try
        jnc     gld_ok
        mov     byte ptr [gpt_which],2
        mov     ebx,dword ptr [dsk_rec+DSK_TOTAL]
        dec     ebx
        call    gpt_try
        jc      gld_fail
gld_ok:
        popad
        clc
        ret
gld_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; gpt_try - EBX = where a header should be.  It is read and checked,
;           its array read and checked, and where both copies are is
;           worked out from it.  CF if any of that fails.
;
; THE HEADER SAYS WHERE ITS OWN ARRAY IS and where the other header is,
; but not where the other ARRAY is.  Every tool puts the backup array
; directly in front of the backup header, and the primary at sector 2,
; and so does this.
;---------------------------------------------------------------------
gpt_try:
        pushad
        mov     [gtr_lba],ebx
        mov     al,[pl_drive]
        mov     ecx,1
        mov     edi,[gpt_hdrp]
        call    dk_read
        jc      gtr_no
        mov     esi,[gpt_hdrp]
        cmp     dword ptr [esi+GH_SIG],GPT_SIG1
        jne     gtr_no
        cmp     dword ptr [esi+GH_SIG+4],GPT_SIG2
        jne     gtr_no
        mov     ecx,[esi+GH_HSIZE]
        cmp     ecx,GH_LEN
        jb      gtr_no
        cmp     ecx,SEC_BYTES
        ja      gtr_no
        ; the header's CRC is taken with its own field as zero
        mov     eax,[esi+GH_HCRC]
        mov     [gtr_crc],eax
        mov     dword ptr [esi+GH_HCRC],0
        call    gpt_crc
        mov     edx,[gtr_crc]
        mov     [esi+GH_HCRC],edx
        cmp     eax,edx
        jne     gtr_no
        ; where it says it is, and every sector number in 32 bits
        mov     eax,[gtr_lba]
        cmp     [esi+GH_MYLBA],eax
        jne     gtr_no
        mov     eax,[esi+GH_MYLBA+4]
        or      eax,[esi+GH_ALTLBA+4]
        or      eax,[esi+GH_FIRST+4]
        or      eax,[esi+GH_LAST+4]
        or      eax,[esi+GH_ALBA+4]
        jnz     gtr_no
        ; the array every tool writes, and nothing else
        cmp     dword ptr [esi+GH_ESIZE],GPT_ESIZE
        jne     gtr_no
        mov     ecx,[esi+GH_NENT]
        or      ecx,ecx
        jz      gtr_no
        cmp     ecx,GPT_MAXP
        ja      gtr_no
        mov     [gtr_n],ecx
        shl     ecx,7                   ; 128 bytes an entry
        add     ecx,SEC_BYTES - 1
        shr     ecx,9
        mov     [gtr_asec],ecx
        mov     eax,[esi+GH_FIRST]
        cmp     eax,[esi+GH_LAST]
        ja      gtr_no
        mov     eax,[esi+GH_LAST]
        cmp     eax,dword ptr [dsk_rec+DSK_TOTAL]
        jae     gtr_no

        ; the array, whole sectors of it, and the rest of the buffer zero
        mov     edi,[gpt_arrp]
        mov     ecx,GPT_MAXP * GPT_ESIZE / 4
        xor     eax,eax
        cld
        rep     stosd
        mov     al,[pl_drive]
        mov     ebx,[esi+GH_ALBA]
        mov     ecx,[gtr_asec]
        mov     edi,[gpt_arrp]
        call    dk_read
        jc      gtr_no
        mov     esi,[gpt_arrp]
        mov     ecx,[gtr_n]
        shl     ecx,7
        call    gpt_crc
        mov     esi,[gpt_hdrp]
        cmp     eax,[esi+GH_ACRC]
        jne     gtr_no

        ; and both copies placed from this one
        mov     eax,[esi+GH_FIRST]
        mov     [gtr_first],eax
        mov     eax,[esi+GH_LAST]
        mov     [gtr_last],eax
        cmp     dword ptr [gtr_lba],1
        jne     gtr_backup
        mov     eax,[esi+GH_ALBA]
        mov     [gtr_parr],eax
        mov     eax,[esi+GH_ALTLBA]
        mov     [gtr_alt],eax
        sub     eax,[gtr_asec]
        mov     [gtr_barr],eax
        jmp     short gtr_where
gtr_backup:
        mov     eax,[gtr_lba]
        mov     [gtr_alt],eax
        mov     eax,[esi+GH_ALBA]
        mov     [gtr_barr],eax
        mov     dword ptr [gtr_parr],2
gtr_where:
        ; NEITHER COPY IN THE USABLE SPACE, OR ON THE OTHER: the primary
        ; array before the first usable sector, the backup array and
        ; header after the last, in that order, and on the disk
        mov     eax,[gtr_parr]
        cmp     eax,2
        jb      gtr_no
        add     eax,[gtr_asec]
        cmp     eax,[gtr_first]
        ja      gtr_no
        mov     eax,[gtr_alt]
        cmp     eax,[gtr_last]
        jbe     gtr_no
        cmp     eax,dword ptr [dsk_rec+DSK_TOTAL]
        jae     gtr_no
        mov     eax,[gtr_barr]
        cmp     eax,[gtr_last]
        jbe     gtr_no
        add     eax,[gtr_asec]
        cmp     eax,[gtr_alt]
        ja      gtr_no

        mov     eax,[gtr_n]
        mov     [gpt_cnt],eax
        mov     eax,[gtr_asec]
        mov     [gpt_asec],eax
        mov     eax,[gtr_first]
        mov     [gpt_first],eax
        mov     eax,[gtr_last]
        mov     [gpt_last],eax
        mov     eax,[gtr_parr]
        mov     [gpt_parr],eax
        mov     eax,[gtr_barr]
        mov     [gpt_barr],eax
        mov     eax,[gtr_alt]
        mov     [gpt_alt],eax
        popad
        clc
        ret
gtr_no:
        popad
        stc
        ret

;---------------------------------------------------------------------
; gpt_crc - ESI -> ECX bytes -> EAX = their CRC-32, the one GPT uses
;           (reflected, polynomial EDB88320h).  A bit at a time: the
;           most it ever does is one 16K array.
;---------------------------------------------------------------------
gpt_crc:
        push    ecx
        push    edx
        push    esi
        mov     eax,0FFFFFFFFh
        or      ecx,ecx
        jz      gcr_done
gcr_byte:
        xor     al,[esi]
        inc     esi
        mov     edx,8
gcr_bit:
        shr     eax,1
        jnc     gcr_next
        xor     eax,0EDB88320h
gcr_next:
        dec     edx
        jnz     gcr_bit
        dec     ecx
        jnz     gcr_byte
gcr_done:
        not     eax
        pop     esi
        pop     edx
        pop     ecx
        ret

;--- ge_empty - ESI -> an entry.  ZF set if its type is all zero, which
;--- is what an unused entry is
ge_empty:
        push    eax
        mov     eax,[esi+GE_TYPE]
        or      eax,[esi+GE_TYPE+4]
        or      eax,[esi+GE_TYPE+8]
        or      eax,[esi+GE_TYPE+12]
        pop     eax
        ret

;---------------------------------------------------------------------
; gsh_build - the shadow table from the array: a record, a kind and
;             flags for every entry.  A basic data partition's first
;             sector is read, to say what is in it.
;---------------------------------------------------------------------
gsh_build:
        pushad
        mov     edi,[gsh_tblp]
        mov     ecx,GPT_MAXP * PE_LEN / 4
        xor     eax,eax
        cld
        rep     stosd
        mov     edi,[gsh_kindp]         ; the kinds, and the flags after them
        mov     ecx,GPT_MAXP * 2 / 4
        rep     stosd
        xor     ebp,ebp
gsb_one:
        cmp     ebp,[gpt_cnt]
        jae     gsb_done
        mov     esi,ebp
        shl     esi,7
        add     esi,[gpt_arrp]          ; ESI -> the entry
        call    ge_empty
        jz      gsb_next
        mov     edi,ebp
        shl     edi,4
        add     edi,[gsh_tblp]          ; EDI -> its record

        mov     eax,[esi+GE_START]
        mov     edx,[esi+GE_END]
        mov     ebx,[esi+GE_START+4]
        or      ebx,[esi+GE_END+4]
        jnz     gsb_far
        cmp     edx,eax
        jb      gsb_far
        cmp     edx,dword ptr [dsk_rec+DSK_TOTAL]
        jae     gsb_far
        mov     [edi+PE_START],eax
        sub     edx,eax
        inc     edx
        mov     [edi+PE_SECS],edx
        jmp     short gsb_boot
gsb_far:
        ; WHERE IT CAN BE SEEN IT TAKES ITS ROOM, so that nothing is made
        ; over it; what is past 32 bits takes none, being past the disk
        ; as this program can address it
        mov     ebx,[gsh_flagp]
        or      byte ptr [ebx+ebp],GSF_FAR
        cmp     dword ptr [esi+GE_START+4],0
        jne     gsb_boot
        mov     eax,[esi+GE_START]
        mov     edx,dword ptr [dsk_rec+DSK_TOTAL]
        cmp     eax,edx
        jae     gsb_boot
        mov     [edi+PE_START],eax
        sub     edx,eax
        mov     [edi+PE_SECS],edx
gsb_boot:
        test    byte ptr [esi+GE_ATTR],GEA_LEGACY
        jz      gsb_kind
        mov     byte ptr [edi+PE_BOOT],80h
gsb_kind:
        call    gsh_classify
gsb_next:
        inc     ebp
        jmp     gsb_one
gsb_done:
        popad
        ret

;---------------------------------------------------------------------
; gsh_classify - ESI -> an entry, EDI -> its record, EBP = its index:
;                the record's type byte and the entry's kind
;---------------------------------------------------------------------
gsh_classify:
        pushad
        mov     ebx,OFFSET gtt_tbl
        mov     ecx,GTT_N
gcl_try:
        mov     eax,[esi+GE_TYPE]
        cmp     eax,[ebx]
        jne     gcl_next
        mov     eax,[esi+GE_TYPE+4]
        cmp     eax,[ebx+4]
        jne     gcl_next
        mov     eax,[esi+GE_TYPE+8]
        cmp     eax,[ebx+8]
        jne     gcl_next
        mov     eax,[esi+GE_TYPE+12]
        cmp     eax,[ebx+12]
        jne     gcl_next
        mov     al,[ebx+16]
        mov     ah,[ebx+17]
        jmp     short gcl_found
gcl_next:
        add     ebx,GTT_LEN
        dec     ecx
        jnz     gcl_try
        mov     al,GK_OTHER
        mov     ah,GPT_PTOTHER
gcl_found:
        mov     [edi+PE_TYPE],ah
        mov     edx,[gsh_kindp]
        mov     [edx+ebp],al
        cmp     al,GK_BASIC
        jne     gcl_out
        ; BASIC DATA SAYS NOTHING ABOUT WHAT IS IN IT; the boot sector does
        mov     edx,[gsh_flagp]
        test    byte ptr [edx+ebp],GSF_FAR
        jnz     gcl_out
        mov     ebx,[edi+PE_START]
        call    gpt_fskind
        mov     [edi+PE_TYPE],ah
        mov     edx,[gsh_kindp]
        mov     [edx+ebp],al
gcl_out:
        popad
        ret

;---------------------------------------------------------------------
; gpt_fskind - EBX = the first sector of a volume on pl_drive -> AL =
;              its kind, AH = the type byte that stands for it: exFAT,
;              NTFS, FAT12, FAT16 or FAT32 by its boot sector, or basic
;              data when it is none of those.  Uses sec_buf.
;
; THE FAT WIDTH IS THE CLUSTER COUNT, as fv_derive has it: a BPB with no
; sixteen-bit FAT size is FAT32's, and otherwise the clusters decide.
;---------------------------------------------------------------------
gpt_fskind:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     al,[pl_drive]
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_read
        mov     ax,PT_FAT32L * 256 + GK_BASIC
        jc      gfk_out
        mov     esi,[sec_buf]
        cmp     dword ptr [esi+3],'AFXE'        ; "EXFAT   "
        jne     gfk_ntfs
        cmp     dword ptr [esi+7],'   T'
        jne     gfk_ntfs
        mov     ax,PT_EXFAT * 256 + GK_EXFAT
        jmp     gfk_out
gfk_ntfs:
        cmp     dword ptr [esi+3],'SFTN'        ; "NTFS    ", which 07h
        jne     gfk_fat                         ; stands for too
        mov     ax,PT_EXFAT * 256 + GK_NTFS
        jmp     gfk_out
gfk_fat:
        mov     ax,PT_FAT32L * 256 + GK_BASIC
        cmp     word ptr [esi+1FEh],MBR_SIG
        jne     gfk_out
        cmp     byte ptr [esi],0EBh
        je      gfk_jmp
        cmp     byte ptr [esi],0E9h
        jne     gfk_out
gfk_jmp:
        cmp     word ptr [esi+BPB_BPS],SEC_BYTES
        jne     gfk_out
        movzx   ecx,byte ptr [esi+BPB_SPC]
        or      ecx,ecx
        jz      gfk_out
        movzx   ebx,byte ptr [esi+BPB_NFATS]
        or      ebx,ebx
        jz      gfk_out
        movzx   eax,word ptr [esi+BPB_SPF16]
        or      eax,eax
        jnz     gfk_1216
        cmp     dword ptr [esi+BPB_SPF32],0
        mov     ax,PT_FAT32L * 256 + GK_BASIC
        je      gfk_out
        mov     ax,PT_FAT32L * 256 + GK_FAT32
        jmp     short gfk_out
gfk_1216:
        imul    eax,ebx                 ; the FATs
        movzx   edx,word ptr [esi+BPB_RSVD]
        add     eax,edx
        movzx   edx,word ptr [esi+BPB_ROOTENT]
        add     edx,15
        shr     edx,4                   ; the root's sectors
        add     eax,edx                 ; EAX = all that is not data
        movzx   edx,word ptr [esi+BPB_TOTSEC16]
        or      edx,edx
        jnz     gfk_tot
        mov     edx,[esi+BPB_TOTSEC32]
gfk_tot:
        sub     edx,eax
        mov     ax,PT_FAT32L * 256 + GK_BASIC
        jbe     gfk_out
        mov     eax,edx
        xor     edx,edx
        div     ecx
        cmp     eax,FAT12_MAXCLUS
        mov     ax,PT_FAT32L * 256 + GK_FAT12
        jbe     gfk_out
        mov     ax,PT_FAT32L * 256 + GK_FAT16
gfk_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; gpt_store - the shadow table back into the GPT, and both copies
;             written.  CF on a failed write.
;
; THE BACKUP FIRST, THEN THE PRIMARY, EACH ARRAY BEFORE ITS HEADER - for
; a write that stops part way.  A header is only believed if the array
; it names has the CRC it records, so until the primary header itself
; is written the primary still checks, old, against its old array; and
; once its new array is down and the header not, the primary fails and
; the backup, already new, is read in its place.  Either way the disk
; says one thing or the other, never half of each.
;---------------------------------------------------------------------
gpt_store:
        pushad
        xor     ebp,ebp
gst_one:
        cmp     ebp,[gpt_cnt]
        jae     gst_crc
        mov     edx,[gsh_flagp]
        test    byte ptr [edx+ebp],GSF_FAR
        jnz     gst_next
        mov     esi,ebp
        shl     esi,4
        add     esi,[gsh_tblp]          ; ESI -> the record
        mov     edi,ebp
        shl     edi,7
        add     edi,[gpt_arrp]          ; EDI -> the entry
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        jne     gst_used
        ; deleted, or never there: all of it zero
        mov     ecx,GPT_ESIZE / 4
        xor     eax,eax
        cld
        rep     stosd
        jmp     short gst_next
gst_used:
        xchg    esi,edi
        call    ge_empty
        xchg    esi,edi
        jnz     gst_place
        call    gpt_newent              ; a partition just made
gst_place:
        mov     eax,[esi+PE_START]
        mov     [edi+GE_START],eax
        mov     dword ptr [edi+GE_START+4],0
        add     eax,[esi+PE_SECS]
        dec     eax
        mov     [edi+GE_END],eax
        mov     dword ptr [edi+GE_END+4],0
        and     byte ptr [edi+GE_ATTR],0FFh XOR GEA_LEGACY
        cmp     byte ptr [esi+PE_BOOT],80h
        jne     gst_next
        or      byte ptr [edi+GE_ATTR],GEA_LEGACY
gst_next:
        inc     ebp
        jmp     gst_one

gst_crc:
        mov     esi,[gpt_arrp]
        mov     ecx,[gpt_cnt]
        shl     ecx,7
        call    gpt_crc
        mov     esi,[gpt_hdrp]
        mov     [esi+GH_ACRC],eax
        ; the backup
        mov     eax,[gpt_alt]
        mov     edx,1
        mov     ecx,[gpt_barr]
        call    gpt_sethdr
        mov     ebx,[gpt_barr]
        mov     ecx,[gpt_asec]
        mov     edi,[gpt_arrp]
        call    gst_write
        jc      gst_fail
        mov     ebx,[gpt_alt]
        mov     ecx,1
        mov     edi,[gpt_hdrp]
        call    gst_write
        jc      gst_fail
        ; and the primary
        mov     eax,1
        mov     edx,[gpt_alt]
        mov     ecx,[gpt_parr]
        call    gpt_sethdr
        mov     ebx,[gpt_parr]
        mov     ecx,[gpt_asec]
        mov     edi,[gpt_arrp]
        call    gst_write
        jc      gst_fail
        mov     ebx,1
        mov     ecx,1
        mov     edi,[gpt_hdrp]
        call    gst_write
        jc      gst_fail
        popad
        clc
        ret
gst_fail:
        popad
        stc
        ret

gst_write:                              ; EBX, ECX, EDI as dk_write takes them
        push    eax
        mov     al,[pl_drive]
        call    dk_write
        pop     eax
        ret

;--- gpt_sethdr - EAX = this copy's sector, EDX = the other's, ECX =
;--- this copy's array: the header in gpt_hdrp made this copy's, and
;--- its CRC taken
gpt_sethdr:
        pushad
        mov     esi,[gpt_hdrp]
        mov     [esi+GH_MYLBA],eax
        mov     dword ptr [esi+GH_MYLBA+4],0
        mov     [esi+GH_ALTLBA],edx
        mov     dword ptr [esi+GH_ALTLBA+4],0
        mov     [esi+GH_ALBA],ecx
        mov     dword ptr [esi+GH_ALBA+4],0
        mov     dword ptr [esi+GH_HCRC],0
        mov     ecx,[esi+GH_HSIZE]
        call    gpt_crc
        mov     [esi+GH_HCRC],eax
        popad
        ret

;---------------------------------------------------------------------
; gpt_newent - ESI -> a record, EDI -> its entry, unused until now: the
;              type its type byte stands for, a GUID of its own, and a
;              name.  The place is gpt_store's to fill in.
;---------------------------------------------------------------------
gpt_newent:
        pushad
        mov     al,[esi+PE_TYPE]
        mov     ebx,OFFSET gtt_tbl              ; basic data, unless...
        mov     edx,OFFSET msg_gnbasic
        cmp     al,0EFh
        jne     gne_notesp
        mov     ebx,OFFSET gtt_tbl + GTT_LEN    ; ...the EFI system partition
        mov     edx,OFFSET msg_gnesp
gne_notesp:
        cmp     al,83h
        jne     gne_notlin
        mov     ebx,OFFSET gtt_tbl + GTT_LEN*4
        mov     edx,OFFSET msg_gnlinux
gne_notlin:
        cmp     al,82h
        jne     gne_notswap
        mov     ebx,OFFSET gtt_tbl + GTT_LEN*5
        mov     edx,OFFSET msg_gnswap
gne_notswap:
        cmp     al,27h
        jne     gne_type
        mov     ebx,OFFSET gtt_tbl + GTT_LEN*3
        mov     edx,OFFSET msg_gnrecov
gne_type:
        ; A HIDDEN FAT TYPE, which only /GPT brings, is basic data with
        ; the attribute that says the same thing
        push    eax
        and     al,0EFh
        cmp     al,PT_FAT12             ; 11h, 14h, 16h, 1Bh, 1Ch, 1Eh: the
        je      gne_ishid               ; FAT types with 10h added
        cmp     al,PT_FAT16S
        je      gne_ishid
        cmp     al,PT_FAT16B
        je      gne_ishid
        cmp     al,PT_FAT32
        je      gne_ishid
        cmp     al,PT_FAT32L
        je      gne_ishid
        cmp     al,PT_FAT16L
        jne     gne_nothid
gne_ishid:
        pop     eax
        push    eax
        test    al,10h
        jz      gne_nothid
        or      dword ptr [edi+GE_ATTR+4],GEA_HIDDEN
gne_nothid:
        pop     eax
        mov     eax,[ebx]
        mov     [edi+GE_TYPE],eax
        mov     eax,[ebx+4]
        mov     [edi+GE_TYPE+4],eax
        mov     eax,[ebx+8]
        mov     [edi+GE_TYPE+8],eax
        mov     eax,[ebx+12]
        mov     [edi+GE_TYPE+12],eax
        push    edi
        add     edi,GE_UNIQ
        call    gpt_guid
        pop     edi
        ; the name, as UTF-16; the entry was zero, so it ends itself
        add     edi,GE_NAME
gne_name:
        movzx   eax,byte ptr [edx]
        cmp     al,'$'
        je      gne_out
        mov     [edi],ax
        add     edi,2
        inc     edx
        jmp     short gne_name
gne_out:
        popad
        ret

;---------------------------------------------------------------------
; gpt_guid - EDI -> sixteen bytes: a GUID nothing else has.
;
; There is no random number to be had here, so it is made from what
; differs between any two of them: the time and the date, a count that
; goes up with each one, the disk, and the GUID made before it - all
; through the CRC, a dword at a time.  It is marked as a random GUID
; (version 4), which is what every tool that makes these writes.
;---------------------------------------------------------------------
gpt_guid:
        pushad
        push    edi
        mov     ah,2Ch
        int     21h
        mov     [ggu_seed],ecx
        mov     [ggu_seed+4],edx
        mov     ah,2Ah
        int     21h
        mov     [ggu_seed+8],ecx
        mov     [ggu_seed+12],edx
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        mov     [ggu_seed+16],eax
        movzx   eax,byte ptr [pl_drive]
        mov     [ggu_seed+20],eax
        pop     edi
        xor     ebx,ebx
ggu_word:
        inc     dword ptr [ggu_count]
        mov     eax,[ggu_count]
        mov     [ggu_seed+24],eax
        mov     esi,OFFSET ggu_seed
        mov     ecx,GGU_SEEDLEN
        call    gpt_crc
        mov     [edi+ebx*4],eax
        mov     [ggu_last+ebx*4],eax
        inc     ebx
        cmp     ebx,4
        jb      ggu_word
        and     byte ptr [edi+7],0Fh
        or      byte ptr [edi+7],40h    ; version 4
        and     byte ptr [edi+8],3Fh
        or      byte ptr [edi+8],80h    ; the standard variant
        popad
        ret

;---------------------------------------------------------------------
; gpt_gap - fd_gap for a GPT disk: the largest free run of sectors -
;           or the one holding gp_want - between the first and last
;           usable sectors, out of the shadow table.  [gp_start] and
;           [gp_len]; CF set if there is none.
;---------------------------------------------------------------------
gpt_gap:
        pushad
        mov     dword ptr [gp_start],0
        mov     dword ptr [gp_len],0
        mov     ebx,[gpt_first]         ; EBX = the cursor
        mov     ebp,[gpt_last]
        inc     ebp                     ; EBP = one past the last usable
ggp_scan:
        cmp     ebx,ebp
        jae     ggp_done
        ; the lowest partition starting at or after EBX
        mov     edx,0FFFFFFFFh          ; EDX = its start, none yet
        xor     edi,edi                 ; EDI = its length
        mov     esi,[gsh_tblp]
        xor     eax,eax
ggp_one:
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      ggp_onenext
        cmp     dword ptr [esi+PE_SECS],0
        je      ggp_onenext
        mov     ecx,[esi+PE_START]
        cmp     ecx,ebx                 ; one that ENCLOSES the cursor
        jae     ggp_notbehind           ; pushes it past itself
        add     ecx,[esi+PE_SECS]
        cmp     ecx,ebx
        jbe     ggp_onenext
        mov     ebx,ecx
        jmp     short ggp_scan
ggp_notbehind:
        cmp     ecx,edx
        jae     ggp_onenext
        mov     edx,ecx
        mov     edi,[esi+PE_SECS]
ggp_onenext:
        add     esi,PE_LEN
        inc     eax
        cmp     eax,[gpt_cnt]
        jb      ggp_one

        mov     ecx,ebp                 ; nothing ahead: to the end
        cmp     edx,0FFFFFFFFh
        je      ggp_measure
        mov     ecx,edx
        cmp     ecx,ebp
        jbe     ggp_measure
        mov     ecx,ebp
ggp_measure:
        sub     ecx,ebx                 ; ECX = this gap's length
        jbe     ggp_step
        cmp     dword ptr [gp_want],0
        je      ggp_biggest
        mov     eax,[gp_want]
        sub     eax,ebx
        jb      ggp_step
        cmp     eax,ecx
        jae     ggp_step
        jmp     short ggp_take
ggp_biggest:
        cmp     ecx,[gp_len]
        jbe     ggp_step
ggp_take:
        mov     [gp_len],ecx
        mov     [gp_start],ebx
ggp_step:
        cmp     edx,0FFFFFFFFh
        je      ggp_done
        mov     ebx,edx
        add     ebx,edi
        jmp     ggp_scan
ggp_done:
        cmp     dword ptr [gp_len],0
        je      ggp_none
        popad
        clc
        ret
ggp_none:
        popad
        stc
        ret

;---------------------------------------------------------------------
; fd_nbgpt - the walls around a GPT partition: every other entry that
;            takes room.  A disk of more than NB_MAX of them walls in
;            the first NB_MAX, which is more than any disk has.
;---------------------------------------------------------------------
fd_nbgpt:
        pushad
        mov     dword ptr [nb_n],0
        mov     edi,OFFSET nb_tbl
        mov     esi,[gsh_tblp]
        xor     ebp,ebp
nbg_one:
        cmp     ebp,[gpt_cnt]
        jae     nbg_done
        cmp     ebp,[pl_idx]
        je      nbg_step
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      nbg_step
        cmp     dword ptr [esi+PE_SECS],0
        je      nbg_step
        cmp     dword ptr [nb_n],NB_MAX
        jae     nbg_done
        mov     eax,[esi+PE_START]
        mov     [edi+NB_START],eax
        mov     eax,[esi+PE_SECS]
        mov     [edi+NB_SECS],eax
        add     edi,NB_LEN
        inc     dword ptr [nb_n]
nbg_step:
        add     esi,PE_LEN
        inc     ebp
        jmp     nbg_one
nbg_done:
        popad
        ret

;---------------------------------------------------------------------
; gpt_kname - AL = a kind -> EDX -> its name, nine wide and '$' ended,
;             as tn_find's names are
;---------------------------------------------------------------------
gpt_kname:
        push    eax
        movzx   eax,al
        or      eax,eax
        jz      gkn_other
        cmp     eax,GK_OTHER
        jbe     gkn_have
gkn_other:
        mov     eax,GK_OTHER
gkn_have:
        imul    edx,eax,10
        add     edx,OFFSET gkn_tbl - 10
        pop     eax
        ret

;--- fd_kindof - the kind of the entry pl_idx names on a GPT disk, in
;--- AL; 0 on any other
fd_kindof:
        xor     eax,eax
        cmp     byte ptr [gpt_on],0
        je      fko_out
        mov     eax,[gsh_kindp]
        add     eax,[pl_idx]
        movzx   eax,byte ptr [eax]
fko_out:
        ret

;--- fd_ptname - the selected partition's type for the verbs' messages:
;--- AL = its type byte; on a GPT disk the entry's name and nothing else
fd_ptname:
        push    eax
        push    edx
        cmp     byte ptr [gpt_on],0
        jne     fpt_gpt
        movzx   eax,al
        call    put2
        call    sp1
        call    typename
        pop     edx
        pop     eax
        ret
fpt_gpt:
        call    fd_kindof
        call    gpt_kname
        call    puts
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; fr_gpt - the report's table for a GPT disk, EBX = its index: where
;          partitions may go, and a row for every entry in use, in the
;          columns fr_table prints a master boot record's slots in.  The
;          type column has the entry's kind where a type byte would be.
;---------------------------------------------------------------------
fr_gpt:
        pushad
        mov     edx,OFFSET msg_rgpt1
        call    puts
        mov     eax,[gpt_cnt]
        call    put1
        mov     edx,OFFSET msg_rgpt2
        call    puts
        mov     eax,[gpt_first]
        call    put1
        mov     edx,OFFSET msg_rgpt3
        call    puts
        mov     eax,[gpt_last]
        call    put1
        call    crlf
        cmp     byte ptr [gpt_which],1
        je      frg_head
        mov     edx,OFFSET msg_rgptbk
        call    puts
frg_head:
        mov     edx,OFFSET msg_thead
        call    puts
        xor     ebp,ebp                 ; EBP = the entry
        xor     edi,edi                 ; EDI = rows printed
frg_row:
        cmp     ebp,[gpt_cnt]
        jae     frg_done
        mov     esi,ebp
        shl     esi,4
        add     esi,[gsh_tblp]
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      frg_next
        inc     edi
        lea     eax,[ebp+1]
        call    put2d
        call    sp1
        mov     al,' '
        cmp     byte ptr [esi+PE_BOOT],80h
        jne     frg_act
        mov     al,'A'
frg_act:
        call    putc
        call    sp2
        mov     edx,OFFSET msg_sp2      ; where the type byte would be
        call    puts
        mov     eax,[gsh_kindp]
        mov     al,[eax+ebp]
        call    gpt_kname
        call    puts
        mov     eax,[esi+PE_START]
        call    put10
        mov     eax,[esi+PE_SECS]
        call    put12
        mov     eax,[esi+PE_SECS]
        call    sec2mb
        call    put7
        mov     edx,OFFSET msg_mb3
        call    puts
        ; on a megabyte, start and end?  No sector here is let off.
        push    ebx
        mov     eax,[esi+PE_START]
        mov     edx,eax
        add     edx,[esi+PE_SECS]
        mov     ebx,0FFFFFFFFh
        call    fd_aligned
        mov     al,' '
        jnc     frg_algok
        mov     al,'*'
        mov     byte ptr [fd_misal],1
frg_algok:
        call    putc
        pop     ebx
        mov     eax,[esi+PE_START]
        call    vol_letter
        jc      frg_noletter
        call    putc
        mov     al,':'
        call    putc
frg_noletter:
        mov     eax,[gsh_flagp]
        test    byte ptr [eax+ebp],GSF_FAR
        jz      frg_rowend
        mov     edx,OFFSET msg_rgptfar
        call    puts
frg_rowend:
        call    crlf
frg_next:
        inc     ebp
        jmp     frg_row
frg_done:
        or      edi,edi
        jnz     frg_out
        mov     edx,OFFSET msg_rgptnone
        call    puts
frg_out:
        popad
        ret

;=====================================================================
; /GPT d   - THE DISK'S TABLE MADE A GPT, partitions and all.
; /NOGPT d - and back, for a GPT a master boot record can describe:
;            four partitions at most, of types it has bytes for.
;
; NOTHING MOVES.  Every partition stays where it is and nothing inside
; one is written - a FAT or exFAT boot sector records where its volume
; starts counted from the disk's start, which neither table changes -
; so the change is the table and only the table.  Partition numbers are
; kept: slot 3 is entry 3, and back.  What has to be free is where a
; GPT keeps itself, sectors 1 to 33 and the last 33 on the disk; a DOS
; disk has nothing in the first, since its partitions begin a track in,
; and a partition in either is a refusal that says which.
;
; GPT FIRST, THEN THE MASTER BOOT RECORD.  Until sector 0 says EEh the
; disk is the MBR disk it was, with some sectors written that nothing
; reads; once it does, the GPT is already there whole.  Going back, the
; master boot record goes first and the two GPT headers are wiped after
; it, for the same reason the other way round.  The boot code in sector
; 0 is kept both ways.
;=====================================================================
GPT_TABSECS     EQU     GPT_MAXP * GPT_ESIZE / SEC_BYTES   ; 32
GPT_FIRSTU      EQU     2 + GPT_TABSECS                     ; 34

fd_togpt:
        cmp     dword ptr [fd_nargs],1
        jb      fdn_usage
        call    fd_getd
        jc      fdn_usage
        xor     eax,eax
        mov     [pl_idx],eax
        mov     [pl_log],eax
        call    fd_loaddisk
        cmp     byte ptr [gpt_on],0
        jne     ftg_isgpt
        cmp     dword ptr [phy_total],GPT_ALIGN * 2
        jb      ftg_small

        ; every slot one a GPT can carry, and out of the way of both copies
        mov     esi,[mbr_buf]
        add     esi,PART_OFF
        xor     ebp,ebp
ftg_slot:
        mov     al,[esi+PE_TYPE]
        or      al,al
        jz      ftg_next
        call    fd_isext
        jnc     ftg_ext
        call    gpt_mbrok
        jc      ftg_type
        mov     eax,[esi+PE_START]
        cmp     eax,GPT_FIRSTU
        jb      ftg_head
        add     eax,[esi+PE_SECS]
        jc      ftg_tail
        mov     edx,[phy_total]
        sub     edx,GPT_TABSECS + 1
        cmp     eax,edx
        ja      ftg_tail
ftg_next:
        add     esi,PE_LEN
        inc     ebp
        cmp     ebp,PART_MAX
        jb      ftg_slot

        call    crlf
        mov     edx,OFFSET msg_tgprop
        call    puts
        movzx   eax,byte ptr [pl_drive]
        sub     eax,80h
        call    put1
        mov     edx,OFFSET msg_tgprop2
        call    puts
        call    fd_bootdisk
        jc      ftg_notboot
        mov     edx,OFFSET msg_tgboot
        call    puts
ftg_notboot:
        cmp     byte ptr [dk_armed],0
        jne     ftg_go
        mov     edx,OFFSET msg_dryrun
        call    puts
        jmp     fd_done
ftg_go:
        call    gpt_fresh
        call    gpt_store
        jc      fdn_wrerr
        call    gpt_protect
        call    fd_wrsec0
        jc      fdn_wrerr
        mov     edx,OFFSET msg_tgdone
        call    puts
        call    fd_rebootnote
        jmp     fd_done

ftg_isgpt:
        mov     edx,OFFSET msg_tgisgpt
        jmp     short ftg_say
ftg_small:
        mov     edx,OFFSET msg_tgsmall
        jmp     short ftg_say
ftg_ext:
        mov     edx,OFFSET msg_tgext
        jmp     short ftg_say
ftg_type:
        mov     edx,OFFSET msg_tgtype
        jmp     short ftg_slotsay
ftg_head:
        mov     edx,OFFSET msg_tghead
        jmp     short ftg_slotsay
ftg_tail:
        mov     edx,OFFSET msg_tgtail
ftg_slotsay:
        push    edx
        mov     edx,OFFSET msg_tgslot
        call    puts
        lea     eax,[ebp+1]
        call    put1
        pop     edx
ftg_say:
        call    puts
        jmp     fd_fail

;--- gpt_mbrok - AL = a type byte in a master boot record.  CF set if a
;--- GPT has nothing to call it.
gpt_mbrok:
        push    eax
        push    edi
        mov     edi,OFFSET gmt_tbl
gmo_one:
        cmp     byte ptr [edi],0
        je      gmo_no
        cmp     al,[edi]
        je      gmo_yes
        inc     edi
        jmp     short gmo_one
gmo_yes:
        pop     edi
        pop     eax
        clc
        ret
gmo_no:
        pop     edi
        pop     eax
        stc
        ret

;--- gpt_fresh - a new, empty GPT for the disk in dsk_rec, with the four
;--- slots of mbr_buf in its shadow table as entries 1 to 4; gpt_on is
;--- set, so that gpt_store writes it
gpt_fresh:
        pushad
        mov     dword ptr [gpt_cnt],GPT_MAXP
        mov     dword ptr [gpt_asec],GPT_TABSECS
        mov     dword ptr [gpt_parr],2
        mov     dword ptr [gpt_first],GPT_FIRSTU
        mov     eax,[phy_total]
        dec     eax
        mov     [gpt_alt],eax
        sub     eax,GPT_TABSECS
        mov     [gpt_barr],eax
        dec     eax
        mov     [gpt_last],eax
        ; the header: what both copies have in common
        mov     edi,[gpt_hdrp]
        mov     ecx,SEC_BYTES / 4
        xor     eax,eax
        cld
        rep     stosd
        mov     edi,[gpt_hdrp]
        mov     dword ptr [edi+GH_SIG],GPT_SIG1
        mov     dword ptr [edi+GH_SIG+4],GPT_SIG2
        mov     dword ptr [edi+GH_REV],GPT_REV
        mov     dword ptr [edi+GH_HSIZE],GH_LEN
        mov     eax,[gpt_first]
        mov     [edi+GH_FIRST],eax
        mov     eax,[gpt_last]
        mov     [edi+GH_LAST],eax
        mov     dword ptr [edi+GH_NENT],GPT_MAXP
        mov     dword ptr [edi+GH_ESIZE],GPT_ESIZE
        add     edi,GH_GUID
        call    gpt_guid
        ; an empty array, and a shadow table holding the slots
        mov     edi,[gpt_arrp]
        mov     ecx,GPT_MAXP * GPT_ESIZE / 4
        xor     eax,eax
        rep     stosd
        mov     edi,[gsh_tblp]
        mov     ecx,GPT_MAXP * PE_LEN / 4
        rep     stosd
        mov     edi,[gsh_kindp]
        mov     ecx,GPT_MAXP * 2 / 4
        rep     stosd
        mov     esi,[mbr_buf]
        add     esi,PART_OFF
        mov     edi,[gsh_tblp]
        mov     ecx,PART_MAX * PE_LEN / 4
        rep     movsd
        mov     byte ptr [gpt_on],1
        popad
        ret

;--- gpt_protect - mbr_buf's table made the protective one: a single
;--- entry of type EEh from sector 1 to the end of the disk, with the
;--- CHS fields the specification gives it
gpt_protect:
        pushad
        mov     edi,[mbr_buf]
        add     edi,PART_OFF
        push    edi
        mov     ecx,PART_MAX * PE_LEN / 4
        xor     eax,eax
        cld
        rep     stosd
        pop     edi
        mov     byte ptr [edi+PE_CHSBEG+1],2    ; head 0, sector 2, cylinder 0
        mov     byte ptr [edi+PE_TYPE],PT_GPT
        mov     byte ptr [edi+PE_CHSEND],0FFh
        mov     byte ptr [edi+PE_CHSEND+1],0FFh
        mov     byte ptr [edi+PE_CHSEND+2],0FFh
        mov     dword ptr [edi+PE_START],1
        mov     eax,[phy_total]
        dec     eax
        mov     [edi+PE_SECS],eax
        popad
        ret

;---------------------------------------------------------------------
; /NOGPT d
;---------------------------------------------------------------------
fd_tombr:
        cmp     dword ptr [fd_nargs],1
        jb      fdn_usage
        call    fd_getd
        jc      fdn_usage
        xor     eax,eax
        mov     [pl_idx],eax
        mov     [pl_log],eax
        call    fd_loaddisk
        cmp     byte ptr [gpt_on],0
        je      ftm_notgpt

        ; four at most, each a type a master boot record has a byte for
        xor     ebp,ebp
        xor     edi,edi                 ; EDI = how many
ftm_one:
        cmp     ebp,[gpt_cnt]
        jae     ftm_counted
        mov     esi,ebp
        shl     esi,4
        add     esi,[gsh_tblp]
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      ftm_next
        mov     eax,[gsh_flagp]
        test    byte ptr [eax+ebp],GSF_FAR
        jnz     ftm_far
        call    gpt_mbrtype             ; -> AL, CF if there is none
        jc      ftm_type
        inc     edi
ftm_next:
        inc     ebp
        jmp     ftm_one
ftm_counted:
        cmp     edi,PART_MAX
        ja      ftm_many

        call    crlf
        mov     edx,OFFSET msg_tmprop
        call    puts
        movzx   eax,byte ptr [pl_drive]
        sub     eax,80h
        call    put1
        mov     edx,OFFSET msg_tmprop2
        call    puts
        cmp     byte ptr [dk_armed],0
        jne     ftm_go
        mov     edx,OFFSET msg_dryrun
        call    puts
        jmp     fd_done

ftm_go:
        ; the slots, in the entries' order; the first legacy bootable
        ; entry is the active one
        mov     edi,[mbr_buf]
        add     edi,PART_OFF
        push    edi
        mov     ecx,PART_MAX * PE_LEN / 4
        xor     eax,eax
        cld
        rep     stosd
        pop     edi
        mov     byte ptr [ftm_act],0
        xor     ebp,ebp
ftm_put:
        cmp     ebp,[gpt_cnt]
        jae     ftm_write
        mov     esi,ebp
        shl     esi,4
        add     esi,[gsh_tblp]
        cmp     byte ptr [esi+PE_TYPE],PT_EMPTY
        je      ftm_putnext
        call    gpt_mbrtype
        push    ebx
        push    ecx
        push    edx
        mov     ebx,[esi+PE_START]
        mov     ecx,[esi+PE_SECS]
        xor     edx,edx
        call    fd_putent
        pop     edx
        pop     ecx
        pop     ebx
        cmp     byte ptr [esi+PE_BOOT],80h
        jne     ftm_inact
        cmp     byte ptr [ftm_act],0
        jne     ftm_inact
        mov     byte ptr [edi+PE_BOOT],80h
        mov     byte ptr [ftm_act],1
ftm_inact:
        add     edi,PE_LEN
ftm_putnext:
        inc     ebp
        jmp     ftm_put
ftm_write:
        mov     byte ptr [gpt_on],0     ; sector 0 is an MBR's from here
        call    fd_wrsec0
        jc      fdn_wrerr
        ; and the two headers, which nothing should find now
        mov     edi,[sec_buf]
        mov     ecx,SEC_BYTES / 4
        xor     eax,eax
        rep     stosd
        mov     al,[pl_drive]
        mov     ebx,1
        mov     ecx,1
        mov     edi,[sec_buf]
        call    dk_write
        jc      fdn_wrerr
        mov     al,[pl_drive]
        mov     ebx,[gpt_alt]
        mov     ecx,1
        call    dk_write
        jc      fdn_wrerr
        mov     edx,OFFSET msg_tmdone
        call    puts
        call    fd_rebootnote
        jmp     fd_done

ftm_notgpt:
        mov     edx,OFFSET msg_tmnotgpt
        jmp     short ftm_say
ftm_many:
        mov     edx,OFFSET msg_tmmany
        jmp     short ftm_say
ftm_far:
        mov     edx,OFFSET msg_tmfar
        jmp     short ftm_numsay
ftm_type:
        mov     edx,OFFSET msg_tmtype
ftm_numsay:
        push    edx
        mov     edx,OFFSET msg_tgslot
        call    puts
        lea     eax,[ebp+1]
        call    put1
        pop     edx
ftm_say:
        call    puts
        jmp     fd_fail

;---------------------------------------------------------------------
; gpt_mbrtype - ESI -> a shadow record, EBP = its entry: the type byte a
;               master boot record gives it, in AL.  CF set when there
;               is none.  The FAT widths go by where the partition ends,
;               as fd_newtype's do: past CHS's reach the LBA spellings.
;---------------------------------------------------------------------
gpt_mbrtype:
        push    ebx
        push    edx
        mov     ebx,[esi+PE_START]
        add     ebx,[esi+PE_SECS]       ; EBX = one past its end
        mov     eax,[gsh_kindp]
        movzx   eax,byte ptr [eax+ebp]
        cmp     al,GK_BASIC
        je      gmt_blank
        cmp     al,GK_FAT12
        je      gmt_12
        cmp     al,GK_FAT16
        je      gmt_16
        cmp     al,GK_FAT32
        je      gmt_32
        mov     ah,PT_EXFAT
        cmp     al,GK_EXFAT
        je      gmt_have
        cmp     al,GK_NTFS
        je      gmt_have
        mov     ah,0EFh
        cmp     al,GK_ESP
        je      gmt_have
        mov     ah,83h
        cmp     al,GK_LINUX
        je      gmt_have
        mov     ah,82h
        cmp     al,GK_SWAP
        je      gmt_have
        mov     ah,27h
        cmp     al,GK_RECOV
        je      gmt_have
        pop     edx
        pop     ebx
        stc
        ret
gmt_blank:
        mov     eax,[esi+PE_START]
        mov     edx,[esi+PE_SECS]
        call    fd_newtype
        mov     ah,al
        jmp     short gmt_have
gmt_12:
        mov     ah,PT_FAT12
        jmp     short gmt_have
gmt_16:
        mov     ah,PT_FAT16L
        cmp     ebx,[phy_chs]
        ja      gmt_have
        mov     ah,PT_FAT16S
        cmp     dword ptr [esi+PE_SECS],65536
        jb      gmt_have
        mov     ah,PT_FAT16B
        jmp     short gmt_have
gmt_32:
        mov     ah,PT_FAT32L
        cmp     ebx,[phy_chs]
        ja      gmt_have
        mov     ah,PT_FAT32
gmt_have:
        mov     al,ah
        pop     edx
        pop     ebx
        clc
        ret

;--- fd_bootdisk - CF clear if pl_drive holds the drive the machine
;--- started from, as 21h/3305h names it
fd_bootdisk:
        pushad
        call    vol_load
        mov     eax,3305h
        int     21h
        movzx   ebx,dl
        or      ebx,ebx
        jz      fbd_no
        dec     ebx                     ; 1 = A:
        cmp     ebx,[vol_n]
        jae     fbd_no
        imul    esi,ebx,DRV_LEN
        add     esi,OFFSET vol_tbl
        mov     al,[pl_drive]
        cmp     [esi+DRV_BIOS],al
        jne     fbd_no
        test    byte ptr [esi+DRV_FLAGS],DRVF_CDROM OR DRVF_DRIVER OR DRVF_IMAGE
        jnz     fbd_no
        popad
        clc
        ret
fbd_no:
        popad
        stc
        ret

;--- fd_tname - typename for a partition that exists: AL = its type,
;--- EBX = its first sector on pl_drive.  07h is NTFS's and exFAT's,
;--- and the volume says which.
fd_tname:
        cmp     al,PT_EXFAT
        jne     typename
        push    eax
        push    edx
        call    gpt_fskind
        cmp     al,GK_EXFAT
        pop     edx
        pop     eax
        jne     typename
        push    edx
        mov     edx,OFFSET gkn_tbl + (GK_EXFAT - 1) * 10
        call    puts
        pop     edx
        ret

;--- fd_newname - AL = the type a partition is being made with: "06
;--- FAT16B", or on a GPT disk the kind of entry that type makes
fd_newname:
        push    eax
        push    edx
        cmp     byte ptr [gpt_on],0
        jne     fnn_gpt
        movzx   eax,al
        call    put2
        call    sp1
        call    typename
        jmp     short fnn_out
fnn_gpt:
        call    gpt_tkind
        call    gpt_kname
        call    puts
fnn_out:
        pop     edx
        pop     eax
        ret

;--- gpt_tkind - AL = a type byte -> AL = the kind of entry gpt_newent
;--- makes for it
gpt_tkind:
        cmp     al,0EFh
        je      gtk_esp
        cmp     al,83h
        je      gtk_linux
        cmp     al,82h
        je      gtk_swap
        mov     al,GK_BASIC
        ret
gtk_esp:
        mov     al,GK_ESP
        ret
gtk_linux:
        mov     al,GK_LINUX
        ret
gtk_swap:
        mov     al,GK_SWAP
        ret

;=====================================================================
; The data.
;=====================================================================
gpt_on      db  0                       ; the disk loaded is a GPT disk, and
                                        ; gsh_tblp is its table
gpt_bad     db  0                       ; it says GPT, and no copy checks
gpt_which   db  0                       ; 1: the primary was read; 2: the backup
gpt_cnt     dd  0                       ; entries in the array
gpt_asec    dd  0                       ; ...and the sectors they take
gpt_first   dd  0                       ; the usable sectors
gpt_last    dd  0
gpt_parr    dd  0                       ; where the primary array is,
gpt_barr    dd  0                       ; the backup array,
gpt_alt     dd  0                       ; and the backup header
gpt_hdrp    dd  0                       ; -> the header (BSS)
gpt_arrp    dd  0                       ; -> the array
gsh_tblp    dd  0                       ; -> the shadow table, PE_LEN a row
gsh_kindp   dd  0                       ; -> a GK_* an entry
gsh_flagp   dd  0                       ; -> a GSF_* an entry

gtr_lba     dd  0                       ; gpt_try's: where the header is,
gtr_crc     dd  0                       ; the CRC it carried,
gtr_n       dd  0                       ; and the array as it found it
gtr_asec    dd  0
gtr_first   dd  0
gtr_last    dd  0
gtr_parr    dd  0
gtr_barr    dd  0
gtr_alt     dd  0

ggu_seed    dd  7 dup(0)                ; what goes into a GUID: the seed,
ggu_last    dd  4 dup(0)                ; and the last GUID behind it
ggu_count   dd  0

; THE TYPES THIS KNOWS: the GUID as it is on the disk, the kind, and the
; type byte that stands for it.  Basic data first and the EFI system
; partition second, and Linux fifth and sixth: gpt_newent counts on it.
gtt_tbl     dd  GPT_BDP0, GPT_BDP1, GPT_BDP2, GPT_BDP3
            db  GK_BASIC, PT_FAT32L
            dd  GPT_ESP0, GPT_ESP1, GPT_ESP2, GPT_ESP3
            db  GK_ESP, 0EFh
            dd  0E3C9E316h, 4DB80B5Ch, 2DF97D81h, 0AE1502F0h   ; Microsoft
            db  GK_MSR, GPT_PTOTHER                             ; reserved
            dd  0DE94BBA4h, 4D4006D1h, 0D5BF6AA1h, 0ACD67901h   ; Windows
            db  GK_RECOV, 27h                                   ; recovery
            dd  0FC63DAFh, 47728483h, 693D798Eh, 0E47D47D8h     ; Linux
            db  GK_LINUX, 83h
            dd  0657FD6Dh, 43C4A4ABh, 3309E584h, 4F4F4BC8h      ; Linux swap
            db  GK_SWAP, 82h
            dd  21686148h, 6E6F6449h, 65654E74h, 49464564h      ; BIOS boot
            db  GK_BIOS, GPT_PTOTHER

; a kind's name, nine wide, from GK_BASIC on
gkn_tbl     db  'Basic    $'
            db  'FAT12    $'
            db  'FAT16    $'
            db  'FAT32    $'
            db  'exFAT    $'
            db  'NTFS     $'
            db  'EFI sys  $'
            db  'Reserved $'
            db  'Recovery $'
            db  'Linux    $'
            db  'Linux swp$'
            db  'BIOS boot$'
            db  'GPT other$'

; what a partition made here is called
msg_gnbasic db  'Basic data partition$'
msg_gnesp   db  'EFI system partition$'
msg_gnlinux db  'Linux filesystem$'
msg_gnswap  db  'Linux swap$'
msg_gnrecov db  'Windows recovery$'

; the types /GPT carries over: FAT, hidden FAT, exFAT and NTFS, recovery,
; Linux, and the EFI system partition
gmt_tbl     db  01h, 04h, 06h, 0Bh, 0Ch, 0Eh
            db  11h, 14h, 16h, 1Bh, 1Ch, 1Eh
            db  07h, 27h, 82h, 83h, 0EFh, 0
ftm_act     db  0                       ; /NOGPT: an active slot is written

msg_tgprop  db  '  proposed: give disk $'
msg_tgprop2 db  ' a GPT.  Its partitions keep their numbers, places and',13,10
            db  '  contents; sector 0 keeps its boot code, and holds one entry saying',13,10
            db  '  the disk is GPT.  Older systems see no partitions on it then.',13,10,'$'
msg_tgboot  db  13,10,'  The machine started from this disk.  To start from it as a GPT disk',13,10
            db  '  its boot code has to read a GPT, and FDISK /MBR writes boot code that',13,10
            db  '  does: it starts the partition marked active.',13,10,'$'
msg_tgdone  db  13,10,'  Converted.$'
msg_tgisgpt db  13,10,'  That disk has a GPT already.',13,10,'$'
msg_tgsmall db  13,10,'  That disk is too small for a GPT.',13,10,'$'
msg_tgext   db  13,10,'  That disk has an extended partition, which a GPT has no place for.',13,10
            db  '  Delete its logical drives and the extended partition first.',13,10,'$'
msg_tgslot  db  13,10,'  Partition $'
msg_tgtype  db  ' has a type that FDISK has no GPT type for.',13,10,'$'
msg_tghead  db  ' starts in the first 34 sectors, where the GPT goes.',13,10,'$'
msg_tgtail  db  ' reaches into the last 33 sectors, where the GPT keeps its',13,10
            db  '  backup.',13,10,'$'
msg_tmprop  db  '  proposed: give disk $'
msg_tmprop2 db  ' a master boot record table in place of its GPT.  The',13,10
            db  '  partitions keep their places and contents, and are numbered 1 to 4',13,10
            db  '  in the order they had.',13,10,'$'
msg_tmdone  db  13,10,'  Converted.$'
msg_tmnotgpt db 13,10,'  That disk has no GPT.',13,10,'$'
msg_tmmany  db  13,10,'  That disk has more than four partitions, and a master boot record',13,10
            db  '  holds four.',13,10,'$'
msg_tmfar   db  ' is past the first 2 TB, where a master boot record cannot',13,10
            db  '  reach.',13,10,'$'
msg_tmtype  db  ' is a type that a master boot record has no type byte for.',13,10,'$'

; the report
msg_rgpt1   db  '         GPT, $'
msg_rgpt2   db  ' entries; partitions go in sectors $'
msg_rgpt3   db  ' to $'
msg_rgptbk  db  '         the primary copy is damaged: this is the backup, and a',13,10
            db  '         change writes both',13,10,'$'
msg_rgptnone db '     (no partitions)',13,10,'$'
msg_rgptfar db  '  past 2 TB$'
msg_rgptbad db  '         GPT, and neither copy of it is whole',13,10,'$'
msg_galnote db  '         * begins or ends off a megabyte',13,10,'$'

msg_gptbad  db  13,10,'  This disk has a GPT, and neither copy of it is whole.  FDISK',13,10
            db  '  changes nothing on it.',13,10,'$'
msg_gptnum  db  13,10,'  There is no partition with that number on this disk.',13,10,'$'
msg_gptfar  db  13,10,'  That partition is past the first 2 TB, where FDISK cannot',13,10
            db  '  reach it.',13,10,'$'
