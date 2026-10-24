;=====================================================================
; GPT.INC - the GUID partition table, read; and the first sector of a
;           volume, asked what is on it
;
; THREE THINGS READ PARTITION TABLES IN THIS KERNEL - the scan that
; gives the fixed disks their letters (FAT.INC), the USB letters each
; time a stick arrives (USBDRV.INC), and the mount of an image file
; (IMAGE.INC) - and they read their sectors three different ways: the
; block layer quietly, the block layer after a first read that has
; been through INT 24h, and a file on another volume.  So this file
; reads through gpt_rdfn, which each of them points at its own way of
; getting a sector, and none of them needs a GPT reader of its own.
;
; WHICH COPY TO BELIEVE.  A GPT is kept twice, the primary at the front
; of the disk and the backup at the end, and each header carries a
; CRC-32 of itself and one of its array.  The primary is tried first;
; one that does not check is not an error but a reason to try the
; backup, which is what the backup is for - a boot manager that wrote
; sector 1 over, or a disk image cut short at the front, still has a
; table.  If neither checks there is no GPT: the protective MBR in
; front of it names no volume, and the disk has no letters.
;
; WHICH ENTRIES GET A LETTER.  Basic Data, which is the type Windows
; puts FAT and exFAT volumes in, and only when the volume's own first
; sector says FAT, exFAT, or nothing at all - a partition FDISK has
; just made has no format yet, and FORMAT needs a letter to give it
; one.  Two of Basic Data's attribute bits are requests in the way the
; hidden MBR types are, and are honoured the same way: 62 hidden, 63
; no drive letter.  The EFI system partition is FAT too and gets no
; letter, as Windows gives it none: the firmware's files are not a
; place for DIR and DEL.
;
; Included by DOSMAIN.ASM, after FAT.INC.
;=====================================================================

; what vbr_probe found
VBR_OTHER       EQU     0               ; something this system does not read
VBR_EXFAT       EQU     1
VBR_FAT         EQU     2
VBR_BLANK       EQU     3               ; nothing at all: a sector of zeros

;---------------------------------------------------------------------
; gpt_bios - gpt_rdfn for a disk the block layer reaches: sector EBX of
;            drive AL into EDI.  CF on failure; everything preserved.
;
; QUIET, by design: the scan runs before anything is mounted, and a
; disk that does not answer is a disk that has no letters, not a
; question for the user about a drive they never named.
;---------------------------------------------------------------------
gpt_bios:
        push    eax
        push    ecx
        mov     cx,1
        call    dword ptr ds:[IOS_RDBLK]
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; gpt_rd - sector EBX of [gpt_drive] into EDI, through [gpt_rdfn].
;---------------------------------------------------------------------
gpt_rd:
        push    eax
        mov     al,[gpt_drive]
        call    dword ptr [gpt_rdfn]
        pop     eax
        ret

;---------------------------------------------------------------------
; gpt_crc - EAX = a running CRC-32, ESI -> ECX bytes.  -> EAX with them
;           folded in, ESI past them.  The caller starts at FFFFFFFFh
;           and inverts the answer, which is the IEEE CRC GPT names.
;
; A BIT AT A TIME and no table: the most this ever sees is a header
; and a 16K array, once a disk, and a kilobyte of table would be in
; the image for the sake of a few milliseconds at boot.
;---------------------------------------------------------------------
gpt_crc:
        or      ecx,ecx
        jz      gcr_out
        push    ecx
        push    edx
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
        pop     edx
        pop     ecx
gcr_out:
        ret

;=====================================================================
; gpt_open - is there a GUID partition table on drive AL?
;
;   AL = the drive, ECX = its sectors (0 if nobody knows), [gpt_rdfn]
;   says how to read it.  CF clear when a copy of the table checks, and
;   gpt_alba / gpt_nent / gpt_esz / gpt_first / gpt_last describe it.
;
; The caller has already seen the PT_GPT entry; this decides whether
; there is anything behind it.
;=====================================================================
gpt_open:
        pushad
        mov     [gpt_drive],al
        mov     [gpt_total],ecx
        mov     dword ptr [gpt_buflba],-1
        mov     byte ptr [gpt_ok],0

        mov     ebx,1                   ; the primary
        call    gpt_try
        jnc     gop_have

        ; THE BACKUP IS WHERE THE PRIMARY SAID, if the primary's header
        ; was sound and only its array was not - and otherwise in the
        ; last sector, which is where it always is when nobody has been
        ; clever.  A disk whose size nobody knows has no last sector.
        mov     ebx,[gpt_alt]
        or      ebx,ebx
        jz      gop_last
        call    gpt_try
        jnc     gop_have
gop_last:
        mov     ebx,[gpt_total]
        cmp     ebx,3
        jb      gop_none
        dec     ebx
        cmp     ebx,[gpt_alt]           ; tried already
        je      gop_none
        call    gpt_try
        jc      gop_none
gop_have:
        mov     byte ptr [gpt_ok],1
        popad
        clc
        ret
gop_none:
        popad
        stc
        ret

;---------------------------------------------------------------------
; gpt_try - the header copy in sector EBX, and its array.  CF clear if
;           both check, with the gpt_* description filled in.  A header
;           that checks leaves [gpt_alt] = where it says the other copy
;           is, whether or not its array then does.
;---------------------------------------------------------------------
gpt_try:
        pushad
        mov     edi,OFFSET gpt_buf
        call    gpt_rd
        jc      gtr_bad
        mov     dword ptr [gpt_buflba],ebx
        mov     esi,edi

        cmp     dword ptr [esi+GH_SIG],GPT_SIG1
        jne     gtr_bad
        cmp     dword ptr [esi+GH_SIG+4],GPT_SIG2
        jne     gtr_bad
        mov     ecx,[esi+GH_HSIZE]
        cmp     ecx,GH_LEN
        jb      gtr_bad
        cmp     ecx,512
        ja      gtr_bad

        ; the header's CRC is taken with its own field as zero
        mov     edx,[esi+GH_HCRC]
        mov     dword ptr [esi+GH_HCRC],0
        mov     eax,0FFFFFFFFh
        call    gpt_crc                 ; ECX bytes from the front
        not     eax
        mov     esi,OFFSET gpt_buf
        mov     [esi+GH_HCRC],edx
        cmp     eax,edx
        jne     gtr_bad

        ; and it must be the copy it says it is - a header found in the
        ; last sector that calls itself sector 1 has been copied there
        ; whole by something that does not know what a backup is
        cmp     dword ptr [esi+GH_MYLBA+4],0
        jne     gtr_bad
        cmp     [esi+GH_MYLBA],ebx
        jne     gtr_bad
        cmp     dword ptr [esi+GH_ALTLBA+4],0
        jne     gtr_noalt
        mov     eax,[esi+GH_ALTLBA]
        mov     [gpt_alt],eax
gtr_noalt:

        ; THE ARRAY'S SHAPE BEFORE ITS CONTENTS.  128 bytes an entry, or
        ; that doubled; a count with a ceiling, so that a header with a
        ; nonsense count cannot send the CRC walking a gigabyte; and an
        ; array that is somewhere this system can address.
        mov     eax,[esi+GH_ESIZE]
        cmp     eax,GE_LEN
        je      gtr_esz
        cmp     eax,GE_LEN * 2
        je      gtr_esz
        cmp     eax,GE_LEN * 4
        jne     gtr_bad
gtr_esz:
        mov     [gpt_esz],eax
        mov     eax,[esi+GH_NENT]
        or      eax,eax
        jz      gtr_bad
        cmp     eax,GPT_MAXENT
        ja      gtr_bad
        mov     [gpt_nent],eax
        cmp     dword ptr [esi+GH_ALBA+4],0
        jne     gtr_bad
        mov     eax,[esi+GH_ALBA]
        cmp     eax,2                   ; the array is never in front of
        jb      gtr_bad                 ; the header it belongs to
        mov     [gpt_alba],eax
        mov     eax,[esi+GH_ACRC]
        mov     [gpt_acrc],eax

        ; the usable range, as far as 32 bits reach: a last sector past
        ; them is the last one this system can name
        mov     eax,[esi+GH_FIRST]
        cmp     dword ptr [esi+GH_FIRST+4],0
        jne     gtr_bad
        mov     [gpt_first],eax
        mov     eax,[esi+GH_LAST]
        cmp     dword ptr [esi+GH_LAST+4],0
        je      gtr_last
        mov     eax,0FFFFFFFFh
gtr_last:
        mov     [gpt_last],eax

        ; NOW THE ARRAY, sector by sector through the CRC.  Its length is
        ; a whole number of entries, which need not be a whole number of
        ; sectors - the last one is taken in part.
        mov     eax,[gpt_nent]
        mul     dword ptr [gpt_esz]     ; EDX:EAX = bytes; the ceilings
        mov     ebp,eax                 ; above keep it to 512K
        mov     ebx,[gpt_alba]
        mov     eax,0FFFFFFFFh
gtr_arr:
        or      ebp,ebp
        jz      gtr_arrdone
        mov     edi,OFFSET gpt_buf
        call    gpt_rd
        jc      gtr_bad
        mov     dword ptr [gpt_buflba],ebx
        mov     ecx,512
        cmp     ebp,ecx
        jae     gtr_whole
        mov     ecx,ebp
gtr_whole:
        sub     ebp,ecx
        mov     esi,edi
        call    gpt_crc
        inc     ebx
        jmp     short gtr_arr
gtr_arrdone:
        not     eax
        cmp     eax,[gpt_acrc]
        jne     gtr_bad
        popad
        clc
        ret
gtr_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; gpt_ent - EDX = a slot in the array.  -> ESI -> its entry in gpt_buf,
;           CF if there is no such slot or its sector will not read.
;           Everything else preserved.
;---------------------------------------------------------------------
gpt_ent:
        cmp     edx,[gpt_nent]
        jae     gen_bad
        push    eax
        push    ebx
        push    edx
        push    edi
        mov     eax,edx
        mul     dword ptr [gpt_esz]     ; EAX = byte offset in the array
        mov     ebx,eax
        shr     ebx,9
        add     ebx,[gpt_alba]          ; the sector it is in
        and     eax,511                 ; and where in it: an entry never
        lea     esi,[gpt_buf+eax]       ; straddles two, being 128 << n
        cmp     ebx,[gpt_buflba]
        je      gen_have
        mov     edi,OFFSET gpt_buf
        call    gpt_rd
        jc      gen_fail
        mov     [gpt_buflba],ebx
gen_have:
        pop     edi
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret
gen_fail:
        mov     dword ptr [gpt_buflba],-1
        pop     edi
        pop     edx
        pop     ebx
        pop     eax
gen_bad:
        stc
        ret

;---------------------------------------------------------------------
; gpt_vol - ESI -> an entry.  CF clear if it is a volume that gets a
;           letter: AH = the type to give the drive table row (PT_*),
;           EDX = its first sector, ECX = how many.  The entry in
;           gpt_buf survives - the volume's sector is read elsewhere.
;---------------------------------------------------------------------
gpt_vol:
        push    eax
        push    ebx
        cmp     dword ptr [esi+GE_TYPE],GPT_BDP0
        jne     gvl_no
        cmp     dword ptr [esi+GE_TYPE+4],GPT_BDP1
        jne     gvl_no
        cmp     dword ptr [esi+GE_TYPE+8],GPT_BDP2
        jne     gvl_no
        cmp     dword ptr [esi+GE_TYPE+12],GPT_BDP3
        jne     gvl_no
        test    dword ptr [esi+GE_ATTR+4],GEA_HIDDEN OR GEA_NOLETTER
        jnz     gvl_no

        ; within 32 bits, in order, and inside the usable range - an
        ; entry that is not is one a bad tool wrote, and following it
        ; would put a letter on the table itself
        cmp     dword ptr [esi+GE_START+4],0
        jne     gvl_no
        cmp     dword ptr [esi+GE_END+4],0
        jne     gvl_no
        mov     edx,[esi+GE_START]
        mov     ecx,[esi+GE_END]
        cmp     edx,[gpt_first]
        jb      gvl_no
        cmp     ecx,[gpt_last]
        ja      gvl_no
        sub     ecx,edx
        jb      gvl_no
        inc     ecx                     ; the end is inclusive

        mov     al,[gpt_drive]
        call    vbr_probe               ; -> AL = VBR_*, AH = a type
        jc      gvl_no
        cmp     al,VBR_OTHER
        je      gvl_no
        cmp     al,VBR_BLANK
        jne     gvl_typed
        mov     ah,PT_FAT32L            ; nothing on it yet: a label for a
gvl_typed:                              ; volume that has no format at all
        mov     [gvl_type],ah
        pop     ebx
        pop     eax
        mov     ah,[gvl_type]
        clc
        ret
gvl_no:
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; gpt_next - the next lettered volume at or after slot EBP.  -> EBP =
;            its slot, AH / EDX / ECX as gpt_vol says.  CF when there
;            are no more.
;---------------------------------------------------------------------
gpt_next:
        push    esi
gnx_lp:
        cmp     ebp,[gpt_nent]
        jae     gnx_none
        push    edx
        mov     edx,ebp
        call    gpt_ent
        pop     edx
        jc      gnx_none                ; a sector that will not read now
        call    gpt_vol                 ; read when the CRC was taken: give up
        jnc     gnx_have
        inc     ebp
        jmp     short gnx_lp
gnx_have:
        pop     esi
        clc
        ret
gnx_none:
        pop     esi
        stc
        ret

;---------------------------------------------------------------------
; mbr_hasgpt - CF clear if the table in sec_buf has a PT_GPT entry.
;
; ONE IS ENOUGH, and the others are not looked at: a "hybrid" MBR, with
; real entries beside the protective one, is a boot-time trick for an
; operating system that cannot read GPT.  Where both tables can be read
; the GPT is the one that is true, as it is for Windows.
;---------------------------------------------------------------------
mbr_hasgpt:
        push    ecx
        push    esi
        mov     esi,OFFSET sec_buf + PART_OFF
        mov     ecx,PART_MAX
mhg_lp:
        cmp     byte ptr [esi+PE_TYPE],PT_GPT
        je      mhg_yes
        add     esi,PE_LEN
        loop    mhg_lp
        pop     esi
        pop     ecx
        stc
        ret
mhg_yes:
        pop     esi
        pop     ecx
        clc
        ret

;=====================================================================
; vbr_probe - AL = a drive, EDX = a volume's first sector.  Read it
;             through [gpt_rdfn] into probe_buf, and say what it is:
;
;   AL = VBR_EXFAT     "EXFAT   " at 3            AH = PT_EXFAT
;   AL = VBR_FAT       a boot sector with a BPB   AH = PT_FAT32L if it
;                      is shaped like FAT32's (no 16-bit FAT length),
;                      else PT_FAT16L
;   AL = VBR_BLANK     every byte zero, and no Linux superblock behind it
;   AL = VBR_OTHER     anything else
;
; CF only if the sector would not read.  Everything else preserved.
;
; A SECTOR OF ZEROS IS LOOKED BEHIND because a Linux filesystem leaves
; its first kilobyte alone and keeps its superblock at 1024, so ext2,
; 3 and 4 all start with a sector that looks unformatted - and older
; Linux tools put them in Basic Data partitions.  A letter there would
; be a letter FORMAT could be pointed at.
;=====================================================================
vbr_probe:
        push    ebx
        push    ecx
        push    esi
        push    edi
        mov     ebx,edx
        mov     edi,OFFSET probe_buf
        call    dword ptr [gpt_rdfn]
        jc      vbp_fail

        cmp     dword ptr [probe_buf+3],'AFXE'
        jne     vbp_notex
        cmp     dword ptr [probe_buf+7],'   T'
        jne     vbp_notex
        mov     ax,(PT_EXFAT SHL 8) OR VBR_EXFAT
        jmp     vbp_out
vbp_notex:
        ; THE TESTS bpb_looks makes, on this buffer: a jump, a sector
        ; size that is a power of two from 128 to 4096, a cluster size
        mov     cl,[probe_buf+BPB_JMP]
        cmp     cl,0EBh
        je      vbp_jmp
        cmp     cl,0E9h
        jne     vbp_blank
vbp_jmp:
        movzx   ecx,word ptr [probe_buf+BPB_BPS]
        cmp     ecx,128
        jb      vbp_other
        cmp     ecx,4096
        ja      vbp_other
        lea     esi,[ecx-1]
        test    esi,ecx
        jnz     vbp_other
        cmp     byte ptr [probe_buf+BPB_SPC],0
        je      vbp_other
        ; ...AND AT LEAST ONE FAT, which is the test NTFS fails: its
        ; boot sector carries the same BPB down to the cluster size, so
        ; that a DOS of 1993 would leave it alone, with no FATs and no
        ; reserved sectors.  bpb_looks can do without this - it is only
        ; ever asked about types that already said FAT
        cmp     byte ptr [probe_buf+BPB_NFATS],0
        je      vbp_other
        mov     ax,(PT_FAT16L SHL 8) OR VBR_FAT
        cmp     word ptr [probe_buf+BPB_SPF16],0
        jne     vbp_out
        mov     ah,PT_FAT32L
        jmp     vbp_out

vbp_blank:
        mov     esi,OFFSET probe_buf
        mov     ecx,512 / 4
vbp_zero:
        cmp     dword ptr [esi],0
        jne     vbp_other
        add     esi,4
        loop    vbp_zero
        add     ebx,2                   ; byte 1024 + 56: ext's magic
        mov     edi,OFFSET probe_buf
        call    dword ptr [gpt_rdfn]
        jc      vbp_isblank             ; nothing to say there is one
        cmp     word ptr [probe_buf+38h],0EF53h
        je      vbp_other
vbp_isblank:
        mov     ax,(PT_FAT32L SHL 8) OR VBR_BLANK
        jmp     vbp_out
vbp_other:
        mov     ax,VBR_OTHER
vbp_out:
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        clc
        ret
vbp_fail:
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; part_exfat - AL = drive, EDX = the first sector of an MBR partition
;              whose type is PT_EXFAT.  CF clear if it IS exFAT, and not
;              the NTFS or HPFS volume 07h means at least as often.
;              Reads through [gpt_rdfn]; everything preserved.
;---------------------------------------------------------------------
part_exfat:
        push    eax
        call    vbr_probe
        jc      pex_no
        cmp     al,VBR_EXFAT
        jne     pex_no
        pop     eax
        clc
        ret
pex_no:
        pop     eax
        stc
        ret

;=====================================================================
; GPT state - one table at a time, which is all any reader needs: the
; scan finishes with a disk before it starts the next, a USB arrival
; and an image mount are each one call
;=====================================================================
gpt_rdfn    dd  OFFSET gpt_bios         ; how a sector is read
gpt_total   dd  0                       ; the disk's sectors, 0 = unknown
gpt_alt     dd  0                       ; where the header just read says
                                        ; the other copy is
gpt_alba    dd  0                       ; the array's first sector
gpt_nent    dd  0                       ; its entries
gpt_esz     dd  0                       ; ...and their size
gpt_acrc    dd  0                       ; the array's CRC, as the header has it
gpt_first   dd  0                       ; the usable range
gpt_last    dd  0
gpt_buflba  dd  -1                      ; which sector gpt_buf holds
gpt_drive   db  0
gpt_ok      db  0                       ; gpt_open found one
gvl_type    db  0                       ; gpt_vol's answer, while EAX is put back
            db  0

_DOS    ENDS
_KBSS   SEGMENT
gpt_buf     db  512 dup(?)              ; a header, or a sector of entries
probe_buf   db  512 dup(?)              ; a volume's first sector, looked at
_KBSS   ENDS
_DOS    SEGMENT
