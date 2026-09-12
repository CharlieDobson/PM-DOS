;=====================================================================
; UDF.INC - reading a disc that has no ISO9660 on it at all.
;
; WHY THERE IS A THIRD FILESYSTEM HERE.  ISO9660 was designed for a
; disc that is written once, in one go, by a machine that knows the
; whole tree before it starts.  That is what a pressed CD is and it is
; not what anything after it was: a DVD, a rewritable disc, a disc
; written a packet at a time out of a drag-and-drop window.  UDF is
; what those carry, and most of them carry an ISO9660 BRIDGE as well -
; a second set of descriptors describing the same files, put there
; precisely so that readers like the one next door keep working.  This
; is for the ones that do not.
;
; SO THE RULE IS: ISO9660 FIRST, AND UDF WHEN THERE IS NO CD001.  Not
; the other way round.  Every disc this kernel has ever read is read
; by the same code as before, and the new path is reached only by a
; disc that the old one would have refused - which is the safest shape
; a new filesystem can be added in.
;
; WHAT IT LOOKS LIKE FROM ABOVE: NOTHING.  The whole of this file's
; output is the same thirty-two byte FAT directory entry ISO9660.INC
; synthesises, in the same buffer, through the same iso_entry that
; dir_seek calls.  vol_fs still says FS_ISO, because from the block
; layer's point of view that is still exactly what this is: a
; read-only volume of 2048-byte blocks reached through IOS_CDREAD.
; One flag - [udf_on] - says which of the two directory formats is
; underneath, and that flag is read in three places.
;
; THE FOUR STEPS OF A MOUNT, and each one is a pointer to the next:
;
;   the VOLUME RECOGNITION SEQUENCE at block 16 says NSR02 or NSR03,
;   which is how a disc says "there is UDF on me" in the same place
;   ISO9660 says CD001
;   the ANCHOR at block 256 says where the volume descriptors are
;   the descriptors say where the PARTITION starts and where the FILE
;   SET is - and every block number from here on is counted from the
;   start of the partition rather than from the start of the disc
;   the file set says which File Entry is the ROOT DIRECTORY
;
; AND THEN THE PART THAT IS GENUINELY DIFFERENT FROM ISO9660: a
; directory record does NOT describe the file.  It carries a name and
; a pointer to a FILE ENTRY somewhere else on the disc, and the size,
; the dates and the extents are all in that.  So listing a directory
; of thirty files reads thirty extra blocks.  That is not a shortcut
; this reader is missing; it is what the format is, and a reader that
; tried to avoid it would be inventing the answers.
;
; WHAT IS NOT HERE, listed because a refusal has to be honest:
;
;   VIRTUAL AND SPARABLE PARTITIONS - partition maps of type 2, which
;   packet-written CD-RW and DVD+RW use to remap blocks through a
;   table.  A disc with one is refused rather than read through the
;   wrong addresses.
;   METADATA PARTITIONS - UDF 2.50's way of keeping the metadata
;   together, and the same argument.
;   NAMED STREAMS, extended attributes, and the permissions: a CD is
;   read-only and DOS has nowhere to put any of it.
;   EMBEDDED DATA - a file small enough to live inside its own File
;   Entry.  It is LISTED, with a size of zero, because everything
;   above this layer reads a file by starting at a block: there is no
;   block to give, and a wrong one would read somebody else's bytes.
;
; Included by DOSMAIN.ASM after ISO9660.INC, whose block reader,
; name machinery and codepage table this uses rather than repeating.
;=====================================================================

;--- the descriptor tag every structure begins with ------------------
UDF_TAG_ID      EQU     0               ; word - which structure this is
UDF_TAG_VER     EQU     2               ; word
UDF_TAG_CKSUM   EQU     4               ; byte - of the other 15
UDF_TAG_SERIAL  EQU     6               ; word
UDF_TAG_CRC     EQU     8               ; word
UDF_TAG_CRCLEN  EQU     10              ; word
UDF_TAG_LOC     EQU     12              ; dword - where it says it is
UDF_TAG_LEN     EQU     16

UDF_T_AVDP      EQU     2               ; the anchor
UDF_T_PD        EQU     5               ; a partition
UDF_T_LVD       EQU     6               ; the logical volume
UDF_T_TD        EQU     8               ; end of a descriptor sequence
UDF_T_FSD       EQU     256             ; the file set
UDF_T_FID       EQU     257             ; a name in a directory
UDF_T_FE        EQU     261             ; a file, or a directory
UDF_T_EFE       EQU     266             ; ...the later, larger form

;--- anchor volume descriptor pointer --------------------------------
UDF_AVDP_MAIN   EQU     16              ; extent_ad: dword bytes, dword block
UDF_AVDP_RSRV   EQU     24

;--- partition descriptor --------------------------------------------
UDF_PD_NUMBER   EQU     22              ; word
UDF_PD_START    EQU     188             ; dword - the first block of it
UDF_PD_LENGTH   EQU     192             ; dword - and how many

;--- logical volume descriptor ---------------------------------------
UDF_LVD_ID      EQU     84              ; dstring(128) - the volume name
UDF_LVD_BSIZE   EQU     212             ; dword
UDF_LVD_FSD     EQU     248             ; long_ad -> the file set
UDF_LVD_MAPLEN  EQU     264             ; dword
UDF_LVD_NMAPS   EQU     268             ; dword
UDF_LVD_MAPS    EQU     440             ; the partition maps themselves

;--- file set descriptor ---------------------------------------------
UDF_FSD_ROOT    EQU     400             ; long_ad -> the root directory

;--- file entry, and the extended one --------------------------------
UDF_FE_ICB      EQU     16              ; the ICB tag
UDF_ICB_TYPE    EQU     11              ; byte, within it: 4 dir, 5 file
UDF_ICB_FLAGS   EQU     18              ; word: bits 0-2 name the form
                                        ; the allocation descriptors take
UDF_FE_INFOLEN  EQU     56              ; qword - the size, and the low
                                        ; half is all DOS can hold
UDF_FE_MTIME    EQU     84              ; timestamp(12)
UDF_FE_LEA      EQU     168             ; dword - extended attributes
UDF_FE_LAD      EQU     172             ; dword - allocation descriptors
UDF_FE_BASE     EQU     176             ; ...and where the two of them start

UDF_EFE_MTIME   EQU     92              ; the same fields, moved along by
UDF_EFE_LEA     EQU     208             ; the extra times and the stream
UDF_EFE_LAD     EQU     212             ; directory an extended entry has
UDF_EFE_BASE    EQU     216

UDF_FT_DIR      EQU     4
UDF_FT_FILE     EQU     5

UDF_AD_SHORT    EQU     0               ; the low three bits of ICB flags
UDF_AD_LONG     EQU     1
UDF_AD_EXT      EQU     2
UDF_AD_INLINE   EQU     3               ; the data is in the entry itself

;--- file identifier descriptor --------------------------------------
UDF_FID_VER     EQU     16              ; word
UDF_FID_CHAR    EQU     18              ; byte - what kind of name this is
UDF_FID_LFI     EQU     19              ; byte - how long it is
UDF_FID_ICB     EQU     20              ; long_ad -> the File Entry
UDF_FID_LIU     EQU     36              ; word - implementation use
UDF_FID_BASE    EQU     38              ; ...and then that, then the name

UDF_FC_HIDDEN   EQU     01h
UDF_FC_DIR      EQU     02h
UDF_FC_DELETED  EQU     04h
UDF_FC_PARENT   EQU     08h             ; this record IS the ".."

UDF_MAXVDS      EQU     64              ; how far a descriptor sequence is
                                        ; followed before it is a bad disc
UDF_MAXFID      EQU     1024            ; ...and how many records a
                                        ; directory may have.  Both are
                                        ; there so that a corrupt disc
                                        ; fails rather than hangs

;=====================================================================
; State
;=====================================================================
udf_on       db 0                       ; the mounted volume is UDF
udf_unit     db 0
udf_adtype   db 0                       ; the form this File Entry's
                                        ; allocation descriptors take
udf_base     dd 0                       ; the block this volume starts at
udf_part     dd 0                       ; the partition's first block
udf_partlen  dd 0                       ; ...and how many blocks it has
udf_fsdblk   dd 0                       ; the file set, in partition blocks
udf_fsdok    db 0                       ; ...and whether one was named at
             db 0, 0, 0                 ; all, because block 0 is a legal
                                        ; answer and 0 is also "not seen"
udf_rootfe   dd 0                       ; the root directory's File Entry
udf_fereq    dd 0                       ; the entry udf_fe is to read, which
                                        ; is a different thing on every call
udf_dirfe    dd 0                       ; the directory being walked
udf_parfe    dd 0                       ; ...and its parent, out of the
                                        ; one record that carries it
udf_fsize    dd 0                       ; what udf_fe found: the size,
udf_fext     dd 0                       ; where the data starts,
udf_ftype    db 0                       ; and whether it is a directory
             db 0, 0, 0

udf_dstart   dd 0                       ; the walk: the directory's data
udf_dlen     dd 0                       ; how many bytes of it there are
udf_doff     dd 0                       ; and how far in we have read
udf_wbase    dd 0                       ; the byte offset udf_dbuf holds
udf_wok      db 0
             db 0, 0, 0

; TWO BLOCKS, because a UDF directory is a plain byte stream and a
; record in it MAY CROSS A BLOCK BOUNDARY - which is the exact
; opposite of ISO9660's rule and the trap waiting for anyone who
; writes the second reader with the first one's habits.  The window is
; always loaded so that the record being read starts inside the first
; of the two blocks, and no record is longer than 38 + 255 + 255
; bytes, so the second block is always enough to finish it.
udf_dbuf     db 2 * ISO_BLKSIZE dup (0)
udf_fbuf     db ISO_BLKSIZE dup (0)     ; one File Entry, read for the
                                        ; record being described - and a
                                        ; buffer of its own, because the
                                        ; directory window is what the
                                        ; record is sitting in

;=====================================================================
; udf_rdblk - ECX blocks from absolute block EAX into EDI.  CF on
;             failure.  This does NOT go through iso_rdblk's cache:
;             the cache holds one block and this reader wants two of
;             them at once and a third somewhere else.
;=====================================================================
udf_rdblk:
        push    eax
        push    ebx
        push    ecx
        push    edi
        mov     ebx,eax
        mov     al,byte ptr [udf_unit]
        call    dword ptr ds:[IOS_CDREAD]
        pop     edi
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; udf_istag - EDI = a descriptor, AX = the tag it should carry.
;             CF set if it is not that.
;
; THE TAG IS CHECKED AND THE CRC IS NOT, and that is a decision rather
; than an omission.  The tag identifier and the location field together
; catch the thing that actually goes wrong here - reading the wrong
; block, because an offset or a partition base was computed wrongly -
; and they catch it immediately.  A CRC catches a disc going bad,
; which on a read-only volume this kernel cannot do anything about
; beyond what the drive's own error correction already did.
;=====================================================================
udf_istag:
        cmp     word ptr [edi+UDF_TAG_ID],ax
        jne     uit_no
        clc
        ret
uit_no:
        stc
        ret

;=====================================================================
; udf_mount - AL = the ATAPI unit, EBX = the block the volume starts
;             at.  CF clear if this disc carries UDF, with the root
;             directory found and iso_rootext / iso_rootlen / iso_volid
;             filled in exactly as an ISO9660 mount fills them.
;=====================================================================
udf_mount:
        pushad
        mov     byte ptr [udf_on],0
        mov     byte ptr [udf_unit],al
        mov     [udf_base],ebx

        ; ---- is there UDF on this disc at all? ----
        ; The volume recognition sequence sits where ISO9660's
        ; descriptors sit, and is read the same way: a list of
        ; 2048-byte structures from block 16, each with a five-byte
        ; identifier one byte in.  NSR02 is UDF 1.x and NSR03 is 2.x;
        ; BEA01 and TEA01 are the brackets round them.
        mov     ecx,8
        mov     eax,[udf_base]
        add     eax,ISO_PVDBLK
umo_vrs:
        push    ecx
        push    eax
        mov     edi,OFFSET udf_fbuf
        mov     ecx,1
        call    udf_rdblk
        pop     eax
        pop     ecx
        jc      umo_no
        cmp     byte ptr [udf_fbuf+1],'N'
        jne     umo_vrsnext
        cmp     byte ptr [udf_fbuf+2],'S'
        jne     umo_vrsnext
        cmp     byte ptr [udf_fbuf+3],'R'
        jne     umo_vrsnext
        cmp     byte ptr [udf_fbuf+4],'0'
        jne     umo_vrsnext
        cmp     byte ptr [udf_fbuf+5],'2'
        je      umo_isudf
        cmp     byte ptr [udf_fbuf+5],'3'
        je      umo_isudf
umo_vrsnext:
        inc     eax
        dec     ecx
        jnz     umo_vrs
        jmp     umo_no

umo_isudf:
        ; ---- the anchor ----
        ; It is at block 256, and also at the last block of the volume
        ; and 256 before that, so that a disc whose beginning is
        ; unreadable can still be mounted from its end.  All three are
        ; tried, in that order, because that is the order of how likely
        ; they are to be right.
        mov     eax,[udf_base]
        add     eax,256
        call    udf_anchor
        jnc     umo_haveanchor
        movzx   eax,byte ptr [udf_unit]
        call    dword ptr ds:[IOS_CDINFO]       ; -> EAX = the last block
        jc      umo_no
        push    eax
        call    udf_anchor
        pop     eax
        jnc     umo_haveanchor
        sub     eax,256
        call    udf_anchor
        jc      umo_no

umo_haveanchor:
        ; udf_fbuf holds the anchor: the main descriptor sequence is an
        ; extent of so many BYTES at such a block
        mov     ecx,dword ptr [udf_fbuf+UDF_AVDP_MAIN]
        shr     ecx,11                          ; bytes -> blocks
        or      ecx,ecx
        jz      umo_no
        cmp     ecx,UDF_MAXVDS
        jbe     umo_vdslen
        mov     ecx,UDF_MAXVDS
umo_vdslen:
        mov     eax,dword ptr [udf_fbuf+UDF_AVDP_MAIN+4]

        ; ---- the descriptor sequence ----
        mov     dword ptr [udf_part],0FFFFFFFFh ; "not seen yet", and 0 is
        mov     dword ptr [udf_rootfe],0        ; a legal partition start
        mov     dword ptr [udf_fsdblk],0        ; ...and so is block 0 for the
        mov     byte ptr [udf_fsdok],0          ; file set, hence the flag
umo_vds:
        push    ecx
        push    eax
        mov     edi,OFFSET udf_fbuf
        mov     ecx,1
        call    udf_rdblk
        pop     eax
        pop     ecx
        jc      umo_no

        movzx   ebx,word ptr [udf_fbuf+UDF_TAG_ID]
        cmp     ebx,UDF_T_TD
        je      umo_vdsdone
        cmp     ebx,UDF_T_PD
        je      umo_pd
        cmp     ebx,UDF_T_LVD
        je      umo_lvd
umo_vdsnext:
        inc     eax
        dec     ecx
        jnz     umo_vds
umo_vdsdone:
        cmp     dword ptr [udf_part],0FFFFFFFFh
        je      umo_no                          ; no partition, no volume
        cmp     byte ptr [udf_fsdok],0
        je      umo_no                          ; ...and nowhere to start

        ; ---- the file set, at a block inside the partition ----
        mov     eax,[udf_part]
        add     eax,[udf_fsdblk]
        mov     edi,OFFSET udf_fbuf
        mov     ecx,1
        call    udf_rdblk
        jc      umo_no
        mov     edi,OFFSET udf_fbuf
        mov     ax,UDF_T_FSD
        call    udf_istag
        jc      umo_no
        mov     eax,dword ptr [udf_fbuf+UDF_FSD_ROOT+4]
        add     eax,[udf_part]
        mov     [udf_rootfe],eax
        mov     [udf_fereq],eax

        ; ---- and the root directory itself ----
        call    udf_fe                          ; -> udf_ftype/fext/fsize
        jc      umo_no
        cmp     byte ptr [udf_ftype],UDF_FT_DIR
        jne     umo_no
        mov     eax,[udf_fext]
        mov     [iso_rootext],eax
        mov     eax,[udf_fsize]
        mov     [iso_rootlen],eax
        mov     eax,[udf_rootfe]
        mov     [udf_dirfe],eax

        ; THE ROOT'S OWN RECORD, in ISO9660's shape, for the same one
        ; caller udf_mkrec exists for: 150Fh asked about "\" hands back
        ; iso_rootrec, and on this filesystem nothing has ever written
        ; one.  Thirty-four bytes with a one-byte identifier of zero,
        ; which is how ISO9660 spells "this directory".
        push    edi
        mov     edi,OFFSET iso_rootrec
        xor     al,al
        mov     ecx,34
        cld
        rep     stosb
        mov     edi,OFFSET iso_rootrec
        mov     byte ptr [edi+ISO_DR_LEN],34
        mov     eax,[udf_fext]
        mov     dword ptr [edi+ISO_DR_EXTENT],eax
        call    udf_swap
        mov     dword ptr [edi+ISO_DR_EXTENT+4],eax
        mov     eax,[udf_fsize]
        mov     dword ptr [edi+ISO_DR_SIZE],eax
        call    udf_swap
        mov     dword ptr [edi+ISO_DR_SIZE+4],eax
        mov     byte ptr [edi+ISO_DR_FLAGS],ISO_FL_DIR
        mov     word ptr [edi+ISO_DR_VOLSEQ],1
        mov     word ptr [edi+ISO_DR_VOLSEQ+2],0100h
        mov     byte ptr [edi+ISO_DR_IDLEN],1
        call    udf_isodate
        pop     edi

        mov     eax,[udf_partlen]
        mov     [iso_volsize],eax
        mov     dword ptr [iso_lbsize],ISO_BLKSIZE
        mov     byte ptr [iso_rr],0             ; neither of ISO9660's two
        mov     byte ptr [iso_jol],0            ; ways of spelling a name
        mov     byte ptr [udf_on],1
        popad
        clc
        ret
umo_no:
        mov     byte ptr [udf_on],0
        popad
        stc
        ret

;--- the partition, whose start every other block number is relative to
umo_pd:
        push    eax
        mov     eax,dword ptr [udf_fbuf+UDF_PD_START]
        mov     [udf_part],eax
        mov     eax,dword ptr [udf_fbuf+UDF_PD_LENGTH]
        mov     [udf_partlen],eax
        pop     eax
        jmp     short umo_vdsnext

;--- the logical volume: the block size, the file set, and the name
umo_lvd:
        push    eax
        push    ebx

        ; THE BLOCK SIZE MUST BE 2048, and this is the same refusal
        ; iso_mount makes for the same reason: IOS_CDREAD moves 2048
        ; bytes and a volume with any other size would be read wrongly
        ; rather than not at all.
        cmp     dword ptr [udf_fbuf+UDF_LVD_BSIZE],ISO_BLKSIZE
        jne     umo_lvdbad

        ; A PARTITION MAP OF TYPE 1 IS A REAL PARTITION and anything
        ; else is a table that remaps blocks - virtual, sparable or
        ; metadata, which packet-written and rewritable media use.
        ; This reader has no such table, so a disc with one is refused
        ; rather than read through addresses that mean something else.
        cmp     dword ptr [udf_fbuf+UDF_LVD_NMAPS],1
        jne     umo_lvdbad
        cmp     byte ptr [udf_fbuf+UDF_LVD_MAPS],1
        jne     umo_lvdbad

        mov     eax,dword ptr [udf_fbuf+UDF_LVD_FSD+4]
        mov     [udf_fsdblk],eax                ; where the file set is
        mov     byte ptr [udf_fsdok],1
        mov     esi,OFFSET udf_fbuf + UDF_LVD_ID
        mov     ecx,128
        call    udf_dstring                     ; -> iso_volid
        pop     ebx
        pop     eax
        jmp     umo_vdsnext
umo_lvdbad:
        pop     ebx
        pop     eax
        jmp     umo_no

;--- udf_anchor - EAX = a block that may hold the anchor.  CF clear
;--- with it in udf_fbuf if it does.
udf_anchor:
        push    ecx
        push    edi
        mov     edi,OFFSET udf_fbuf
        mov     ecx,1
        call    udf_rdblk
        jc      ua_no
        mov     edi,OFFSET udf_fbuf
        mov     ax,UDF_T_AVDP
        call    udf_istag
        jc      ua_no
        pop     edi
        pop     ecx
        clc
        ret
ua_no:
        pop     edi
        pop     ecx
        stc
        ret

;=====================================================================
; udf_fe - read a File Entry and take it apart.
;
; In: [udf_fereq] is the File Entry's absolute block.
;     Out: [udf_ftype], [udf_fext] = the first data block,
;     [udf_fsize] = how many bytes, and udf_fbuf holds the entry so
;     that the caller can take the timestamp out of it.  CF on a read
;     failure or a block that is not a File Entry.
;
; THE EXTENTS ARE ADDED UP WHILE THEY ARE CONTIGUOUS and the total
; stops at the first gap - the same rule, for the same reason, as the
; multi-extent files next door in ISO9660.INC: everything above this
; layer reads a file by running forward from one block, so a size that
; counted a piece somewhere else would be a promise the read path
; cannot keep.
;=====================================================================
udf_fe:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp

        mov     byte ptr [udf_ftype],0
        mov     dword ptr [udf_fext],0
        mov     dword ptr [udf_fsize],0

        mov     eax,[udf_fereq]
        mov     edi,OFFSET udf_fbuf
        mov     ecx,1
        call    udf_rdblk
        jc      ufe_bad

        mov     edi,OFFSET udf_fbuf
        movzx   ebx,word ptr [edi+UDF_TAG_ID]
        cmp     ebx,UDF_T_FE
        je      ufe_plain
        cmp     ebx,UDF_T_EFE
        jne     ufe_bad
        mov     esi,UDF_EFE_BASE                ; the extended entry has
        mov     edx,UDF_EFE_LEA                 ; its fields further down
        jmp     short ufe_fields
ufe_plain:
        mov     esi,UDF_FE_BASE
        mov     edx,UDF_FE_LEA
ufe_fields:
        mov     al,[edi+UDF_FE_ICB+UDF_ICB_TYPE]
        mov     [udf_ftype],al
        mov     ax,word ptr [edi+UDF_FE_ICB+UDF_ICB_FLAGS]
        and     al,7
        mov     [udf_adtype],al

        ; the size.  It is a 64-bit number and DOS has a 32-bit field,
        ; so a file over 4GB is reported as the largest size there is
        ; rather than as its own size modulo four gigabytes.
        mov     eax,dword ptr [edi+UDF_FE_INFOLEN]
        cmp     dword ptr [edi+UDF_FE_INFOLEN+4],0
        je      ufe_size
        mov     eax,0FFFFFFFFh
ufe_size:
        mov     [udf_fsize],eax

        ; where the allocation descriptors begin: past the fixed part,
        ; and then past whatever extended attributes are in front of
        ; them
        mov     ebx,esi                         ; the fixed part's end
        add     ebx,dword ptr [edi+edx]         ; + the extended attributes
        mov     ecx,dword ptr [edi+edx+4]       ; and how many bytes of
                                                ; descriptors there are
        cmp     byte ptr [udf_adtype],UDF_AD_INLINE
        je      ufe_inline
        cmp     byte ptr [udf_adtype],UDF_AD_SHORT
        je      ufe_short
        cmp     byte ptr [udf_adtype],UDF_AD_LONG
        je      ufe_long
        jmp     ufe_bad                        ; extended descriptors, which
                                                ; nothing writes on a disc
                                                ; this reader will meet
ufe_inline:
        ; THE DATA IS INSIDE THE ENTRY.  There is no block to hand to a
        ; reader that starts at a block, so the file is listed with a
        ; size of nothing - see the note at the top of this file.
        mov     dword ptr [udf_fsize],0
        mov     dword ptr [udf_fext],0
        jmp     ufe_ok     

ufe_short:
        ; A SHORT DESCRIPTOR IS EIGHT BYTES and a long one sixteen,
        ; and the only difference that matters here is the step: both
        ; begin with a length whose top two bits say whether the extent
        ; is really recorded, and both carry the block number four
        ; bytes in.  So one loop reads both, with the stride in EBP.
        mov     ebp,8
        jmp     short ufe_first
ufe_long:
        mov     ebp,16
ufe_first:
        or      ecx,ecx
        jz      ufe_ok                          ; a file with no extents
        add     edi,ebx                         ; -> the first descriptor
        mov     eax,dword ptr [edi+4]
        add     eax,[udf_part]
        mov     [udf_fext],eax
        mov     edx,dword ptr [edi]
        and     edx,3FFFFFFFh                   ; its length, without the type
        mov     esi,eax                         ; where it starts...
        mov     eax,edx
        add     eax,ISO_BLKSIZE - 1
        shr     eax,11
        add     esi,eax                         ; ...and where the next piece
                                                ; must start to be one run
ufe_run:
        ; EDX = the bytes accounted for, ESI = the block the next
        ; extent must begin at, ECX = what is left of the descriptor
        ; area.  ONLY THE LATEST EXTENT'S LENGTH ADVANCES ESI, which
        ; is the arithmetic this got wrong on the way in: adding the
        ; running total each time walks off the end of the file after
        ; two extents and calls a contiguous file fragmented.
        sub     ecx,ebp
        jbe     ufe_total
        add     edi,ebp
        mov     eax,dword ptr [edi+4]
        add     eax,[udf_part]
        cmp     eax,esi
        jne     ufe_total                       ; a gap: the run ends here
        mov     eax,dword ptr [edi]
        and     eax,0C0000000h
        jnz     ufe_total                       ; allocated but not recorded
        mov     eax,dword ptr [edi]
        and     eax,3FFFFFFFh
        add     edx,eax
        jc      ufe_total                       ; over 4GB, which DOS cannot
        add     eax,ISO_BLKSIZE - 1             ; say at all
        shr     eax,11
        add     esi,eax
        jmp     short ufe_run
ufe_total:
        ; the size the extents actually account for, which is the
        ; smaller of what they cover and what the entry claims
        cmp     edx,[udf_fsize]
        jae     ufe_ok
        mov     [udf_fsize],edx
ufe_ok:
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
ufe_bad:
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; THE DIRECTORY WALK
;
; udf_diropen  EAX = the first data block, EDX = the length in bytes.
; udf_dirnext  -> ESI = the next File Identifier Descriptor, with the
;                 parent record and the deleted ones stepped over and
;                 the parent's File Entry remembered in [udf_parfe].
;=====================================================================
udf_diropen:
        mov     [udf_dstart],eax
        mov     [udf_dlen],edx
        mov     dword ptr [udf_doff],0
        mov     byte ptr [udf_wok],0
        mov     dword ptr [udf_parfe],0
        ret

udf_dirnext:
        push    eax
        push    ebx
        push    ecx
        push    edx
udn_lp:
        mov     eax,[udf_doff]
        add     eax,UDF_FID_BASE
        cmp     eax,[udf_dlen]
        jae     udn_end                 ; no room for another header

        call    udf_window              ; -> ESI = the record, CF on I/O
        jc      udn_end

        mov     ax,UDF_T_FID
        push    edi
        mov     edi,esi
        call    udf_istag
        pop     edi
        jc      udn_end                 ; padding, or the end of the list

        ; how long this record is: the fixed part, the implementation
        ; use, the name, and then padding to a four-byte boundary
        movzx   ecx,word ptr [esi+UDF_FID_LIU]
        movzx   ebx,byte ptr [esi+UDF_FID_LFI]
        add     ecx,ebx
        add     ecx,UDF_FID_BASE
        mov     eax,ecx
        and     eax,3
        jz      udn_aligned
        add     ecx,4
        sub     ecx,eax
udn_aligned:
        add     [udf_doff],ecx

        mov     al,[esi+UDF_FID_CHAR]
        test    al,UDF_FC_PARENT
        jz      udn_notparent
        ; THE ".." RECORD, which is where a UDF directory keeps the one
        ; thing an ISO9660 directory keeps in a record of its own: it
        ; is not listed, but its File Entry is what ".." has to point
        ; at, so it is remembered rather than merely skipped.
        mov     eax,dword ptr [esi+UDF_FID_ICB+4]
        add     eax,[udf_part]
        mov     [udf_parfe],eax
        jmp     short udn_lp
udn_notparent:
        test    al,UDF_FC_DELETED
        jnz     udn_lp                  ; a name that is no longer there
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
udn_end:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; udf_window - make sure udf_dbuf holds the two blocks that [udf_doff]
;              points into, and return ESI pointing at it.
;
; TWO BLOCKS AND NOT ONE, because a record may straddle the boundary:
; the window is always aligned so that the record STARTS in the first
; of the two, and the longest a record can be - 38 bytes of header,
; 255 of implementation use and 255 of name - is comfortably less than
; the block that follows it.
;=====================================================================
udf_window:
        push    eax
        push    ecx
        push    edi
        mov     eax,[udf_doff]
        and     eax,NOT (ISO_BLKSIZE - 1)       ; the block it is in
        cmp     byte ptr [udf_wok],0
        je      uw_load
        cmp     eax,[udf_wbase]
        je      uw_have
uw_load:
        mov     [udf_wbase],eax
        mov     byte ptr [udf_wok],0
        shr     eax,11                          ; -> which block of the file
        add     eax,[udf_dstart]
        mov     edi,OFFSET udf_dbuf
        mov     ecx,2
        call    udf_rdblk
        jnc     uw_loaded
        ; THE SECOND BLOCK MAY NOT EXIST - a directory that ends at the
        ; last block of the partition has nothing after it.  One block
        ; is enough whenever the record does not cross, so this falls
        ; back to one rather than failing the walk.
        mov     eax,[udf_wbase]
        shr     eax,11
        add     eax,[udf_dstart]
        mov     edi,OFFSET udf_dbuf
        mov     ecx,1
        call    udf_rdblk
        jc      uw_bad
uw_loaded:
        mov     byte ptr [udf_wok],1
uw_have:
        mov     esi,[udf_doff]
        sub     esi,[udf_wbase]
        add     esi,OFFSET udf_dbuf
        pop     edi
        pop     ecx
        pop     eax
        clc
        ret
uw_bad:
        pop     edi
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; udf_dirlen - ctx_chain has put a DIRECTORY'S FILE ENTRY block in
;              dc_isoext, because that is what udf_entry puts in a
;              directory's synthesised cluster field.  Turn it into
;              what the walk wants: the data extent and its length.
;
; A DIRECTORY'S SIZE IS NOT IN THE DIRECTORY, which is the difference
; from ISO9660 that this routine exists for.  ISO9660 keeps it in the
; "." record at the front of the directory itself; UDF keeps it in the
; File Entry, one read away, and there is no way to get it from the
; data.
;=====================================================================
udf_dirlen:
        pushad
        mov     eax,[dc_isoext]
        mov     [udf_fereq],eax
        mov     [udf_dirfe],eax         ; "." points back at this
        call    udf_fe
        jc      udl_out
        cmp     byte ptr [udf_ftype],UDF_FT_DIR
        jne     udl_out
        mov     eax,[udf_fext]
        mov     [dc_isoext],eax
        mov     eax,[udf_fsize]
        mov     [dc_isolen],eax
udl_out:
        popad
        ret

;=====================================================================
; udf_entry - the same job iso_entry does, for the other format.
;
;   EBX = which entry of the current directory, 0 upwards
;   -> ESI = a thirty-two byte FAT directory entry, CF past the end
;
; The shape of the listing is ISO9660's, deliberately, because it is
; the shape the rest of the kernel already reads: entry zero of the
; root is the VOLUME LABEL, and a subdirectory begins with "." and
; ".." - which UDF does not record at all and this synthesises, one
; from the directory itself and one from the parent record.
;=====================================================================
udf_entry:
        push    eax
        push    ecx
        push    edx
        push    edi
        push    ebp

        mov     ebp,ebx
        cmp     byte ptr [dc_isroot],0
        je      ude_sub
        or      ebp,ebp
        jz      ude_label
        dec     ebp
        jmp     short ude_walk
ude_sub:
        or      ebp,ebp
        jz      ude_dot
        cmp     ebp,1
        je      ude_dotdot
        sub     ebp,2

ude_walk:
        mov     eax,[dc_isoext]
        mov     edx,[dc_isolen]
        call    udf_diropen
        mov     ecx,ebp
ude_skip:
        call    udf_dirnext
        jc      ude_none
        or      ecx,ecx
        jz      ude_got
        dec     ecx
        jmp     short ude_skip

ude_got:
        ; ESI is the record.  Its File Entry has everything the record
        ; does not - which on this filesystem is everything except the
        ; name.
        mov     eax,dword ptr [esi+UDF_FID_ICB+4]
        add     eax,[udf_part]
        mov     [udf_fereq],eax
        push    esi
        call    udf_fe
        pop     esi
        jc      ude_none

        mov     edi,OFFSET iso_fatent
        push    esi
        push    edi
        xor     al,al
        mov     ecx,32
        cld
        rep     stosb
        pop     edi
        pop     esi

        push    ebx
        mov     ebx,ebp                 ; the ordinal, for the ~N alias
        call    udf_dosname
        pop     ebx

        mov     al,ATTR_READONLY        ; a CD, and it is not decoration:
        cmp     byte ptr [udf_ftype],UDF_FT_DIR ; it is what stops a program
        jne     ude_notdir                      ; opening a file to write
        or      al,ATTR_DIR
ude_notdir:
        test    byte ptr [esi+UDF_FID_CHAR],UDF_FC_HIDDEN
        jz      ude_nothid
        or      al,ATTR_HIDDEN
ude_nothid:
        mov     byte ptr [edi+DE_ATTR],al

        ; A DIRECTORY CARRIES ITS FILE ENTRY AND A FILE CARRIES ITS
        ; DATA, and the difference matters: walking into a directory
        ; goes through ctx_chain, which hands this number back to
        ; udf_dirlen to be turned into an extent, while reading a file
        ; goes to the block layer, which wants the data itself.
        cmp     byte ptr [udf_ftype],UDF_FT_DIR
        jne     ude_fileext
        mov     eax,[udf_fereq]
        jmp     short ude_putext
ude_fileext:
        mov     eax,[udf_fext]
ude_putext:
        mov     word ptr [edi+DE_CLUSTER],ax
        shr     eax,16
        mov     word ptr [edi+DE_CLUSHI],ax
        mov     eax,[udf_fsize]
        mov     dword ptr [edi+DE_SIZE],eax
        call    udf_stamp
        call    udf_mkrec               ; ...and one in ISO9660's shape, for
                                        ; the one caller that asks for that

        mov     esi,edi
        pop     ebp
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret

;--- the volume label, entry zero of the root -----------------------
ude_label:
        call    iso_label
        pop     ebp
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret

;--- "." and "..", which this filesystem does not write -------------
ude_dot:
        mov     eax,[udf_dirfe]
        mov     ecx,1
        jmp     short ude_dots
ude_dotdot:
        ; The parent's File Entry is in the record UDF puts at the
        ; front of every directory, so one step of the walk finds it.
        mov     eax,[dc_isoext]
        mov     edx,[dc_isolen]
        call    udf_diropen
        call    udf_dirnext             ; its answer does not matter: the
                                        ; parent record is stepped over
                                        ; and remembered on the way past
        mov     eax,[udf_parfe]
        or      eax,eax
        jnz     ude_dotdot2
        mov     eax,[udf_rootfe]        ; a directory with no parent record
ude_dotdot2:                            ; is malformed; the root is the
        mov     ecx,2                   ; safe answer
ude_dots:
        push    eax
        mov     edi,OFFSET iso_fatent
        push    edi
        xor     al,al
        push    ecx
        mov     ecx,32
        cld
        rep     stosb
        pop     ecx
        pop     edi
        mov     al,' '
        push    edi
        push    ecx
        mov     ecx,11
        rep     stosb
        pop     ecx
        pop     edi
        mov     byte ptr [edi],'.'
        cmp     ecx,2
        jne     ude_dot1
        mov     byte ptr [edi+1],'.'
ude_dot1:
        mov     byte ptr [edi+DE_ATTR],ATTR_DIR OR ATTR_READONLY
        pop     eax
        mov     word ptr [edi+DE_CLUSTER],ax
        shr     eax,16
        mov     word ptr [edi+DE_CLUSHI],ax
        mov     byte ptr [iso_lname],0  ; "." was never a long name
        mov     esi,edi
        pop     ebp
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret

ude_none:
        pop     ebp
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; udf_dosname - ESI = a file identifier descriptor, EDI = the eleven
;               bytes to fill, EBX = the record's ordinal.  The full
;               name into lfn_buf and the 8.3 field made FROM it, the
;               same way round as everywhere else in this kernel.
;=====================================================================
udf_dosname:
        pushad
        mov     byte ptr [iso_lname],0
        call    udf_rawname             ; -> lfn_buf, ECX = its length
        call    iso_mkboth
        popad
        ret

;=====================================================================
; udf_rawname - the name out of a file identifier descriptor, into
;               lfn_buf.  -> ECX = how long it is.
;
; A UDF NAME IS A d-STRING, which is a length-counted string whose
; FIRST BYTE SAYS HOW WIDE ITS CHARACTERS ARE: 8 means one byte each
; and 16 means two, big-endian, exactly as Joliet spells them.  The
; two forms are not a matter of what the name contains - Windows
; writes every name as 16 whatever is in it - so both have to be read.
;=====================================================================
udf_rawname:
        push    eax
        push    ebx
        push    edx
        push    esi
        push    edi
        mov     edi,OFFSET lfn_buf
        xor     edx,edx                         ; characters written

        movzx   ecx,byte ptr [esi+UDF_FID_LFI]
        or      ecx,ecx
        jz      urn_end
        movzx   eax,word ptr [esi+UDF_FID_LIU]
        add     esi,UDF_FID_BASE
        add     esi,eax                         ; -> the d-string
        mov     al,[esi]                        ; its compression byte
        inc     esi
        dec     ecx
        jz      urn_end
        cmp     al,8
        je      urn_narrow
        cmp     al,16
        jne     urn_end                         ; a form nothing writes and
                                                ; this cannot guess at
urn_wide:
        shr     ecx,1
        jz      urn_end
urn_wlp:
        mov     al,[esi+1]                      ; big-endian, so the low
        mov     ah,[esi]                        ; half is the second byte
        add     esi,2
        call    iso_lchar
        cmp     edx,LFN_MAX - 1
        jae     urn_wnext
        mov     [edi+edx],al
        inc     edx
urn_wnext:
        dec     ecx
        jnz     urn_wlp
        jmp     short urn_end
urn_narrow:
        movzx   eax,byte ptr [esi]
        inc     esi
        call    iso_lchar                       ; a byte here is a code
        cmp     edx,LFN_MAX - 1                 ; point, so 437 can spell
        jae     urn_nnext                       ; more of them than ASCII
        mov     [edi+edx],al
        inc     edx
urn_nnext:
        dec     ecx
        jnz     urn_narrow
urn_end:
        mov     byte ptr [edi+edx],0
        mov     ecx,edx
        pop     edi
        pop     esi
        pop     edx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; udf_dstring - ESI = a d-string field, ECX = the whole field's size.
;               Its text into iso_volid, NUL terminated.
;
; THE LAST BYTE OF THE FIELD IS THE LENGTH, which is the part of this
; format that catches a reader out: the string is not terminated and
; not padded with spaces, and the count at the far end includes the
; compression byte at the near one.
;=====================================================================
udf_dstring:
        pushad
        mov     edi,OFFSET iso_volid
        mov     byte ptr [edi],0
        movzx   ebx,byte ptr [esi+ecx-1]        ; the length, at the end
        or      ebx,ebx
        jz      uds_out
        cmp     ebx,ecx
        ja      uds_out
        dec     ebx                             ; ...without the compression
        mov     al,[esi]                        ; byte it counts
        inc     esi
        xor     edx,edx
        cmp     al,8
        je      uds_narrow
        cmp     al,16
        jne     uds_out
        shr     ebx,1
uds_wide:
        or      ebx,ebx
        jz      uds_done
        mov     al,[esi+1]
        mov     ah,[esi]
        add     esi,2
        call    iso_lchar
        cmp     edx,32
        jae     uds_wnext
        mov     [edi+edx],al
        inc     edx
uds_wnext:
        dec     ebx
        jmp     short uds_wide
uds_narrow:
        or      ebx,ebx
        jz      uds_done
        movzx   eax,byte ptr [esi]
        inc     esi
        call    iso_lchar
        cmp     edx,32
        jae     uds_nnext
        mov     [edi+edx],al
        inc     edx
uds_nnext:
        dec     ebx
        jmp     short uds_narrow
uds_done:
        mov     byte ptr [edi+edx],0
uds_out:
        popad
        ret

;=====================================================================
; udf_mkrec - the thing just described, written out as an ISO9660
;             DIRECTORY RECORD in iso_lastrec.
;
; WHY A UDF READER BUILDS AN ISO9660 STRUCTURE.  One caller wants one:
; MSCDEX's 150Fh hands a program "the directory entry for this path",
; and that call is older than UDF and describes an ISO9660 record.
; There is no such record on this disc - so the fields UDF HAS are
; filled in from what was just read, and the fields ISO9660 invented
; for its own bookkeeping - the extended attribute length, the
; interleave, the volume set sequence - are left at zero, because a
; made-up number in a structure a program parses is worse than an
; obvious blank.
;
; The identifier is the name AS THIS READER SPELLS IT, in the DOS
; codepage, rather than the d-string the disc holds: a caller that
; wanted UCS-2 with a compression byte on the front of it would not be
; asking through a DOS interface from 1990.
;
; In: EDI = the FAT entry just built, and udf_fbuf still holds the
; File Entry.  Everything preserved.
;=====================================================================
udf_mkrec:
        pushad
        push    edi
        mov     edi,OFFSET iso_lastrec
        xor     al,al
        mov     ecx,255
        cld
        rep     stosb
        pop     esi                     ; the FAT entry, as the source
        mov     edi,OFFSET iso_lastrec

        ; THE DATA, FOR A DIRECTORY AS WELL AS FOR A FILE.  The
        ; synthesised FAT entry gives a directory its File Entry block,
        ; because that is what walking into it needs; this record is a
        ; DESCRIPTION rather than a walk, and what it describes is
        ; where the directory's contents are.
        mov     eax,[udf_fext]
        mov     dword ptr [edi+ISO_DR_EXTENT],eax
        call    udf_swap
        mov     dword ptr [edi+ISO_DR_EXTENT+4],eax     ; both-endian, as the
                                                        ; format requires
        mov     eax,[udf_fsize]
        mov     dword ptr [edi+ISO_DR_SIZE],eax
        call    udf_swap
        mov     dword ptr [edi+ISO_DR_SIZE+4],eax

        xor     al,al                           ; ...in ISO9660's flags,
        test    byte ptr [esi+DE_ATTR],ATTR_DIR ; which are not DOS's
        jz      umr_notdir
        or      al,ISO_FL_DIR
umr_notdir:
        test    byte ptr [esi+DE_ATTR],ATTR_HIDDEN
        jz      umr_nothid
        or      al,ISO_FL_HIDDEN
umr_nothid:
        mov     byte ptr [edi+ISO_DR_FLAGS],al
        mov     word ptr [edi+ISO_DR_VOLSEQ],1
        mov     word ptr [edi+ISO_DR_VOLSEQ+2],0100h    ; the big half of 1

        ; the recording time, out of the same File Entry timestamp the
        ; DOS date came from, in ISO9660's seven bytes
        call    udf_isodate

        ; and the name, which is in lfn_buf because udf_dosname put it
        ; there a moment ago
        mov     esi,OFFSET lfn_buf
        xor     ecx,ecx
umr_len:
        cmp     byte ptr [esi+ecx],0
        je      umr_gotlen
        inc     ecx
        cmp     ecx,200
        jb      umr_len
umr_gotlen:
        mov     byte ptr [edi+ISO_DR_IDLEN],cl
        push    ecx
        push    edi
        add     edi,ISO_DR_ID
        rep     movsb
        pop     edi
        pop     ecx
        add     ecx,ISO_DR_ID
        test    cl,1
        jz      umr_even
        inc     ecx                             ; records are an even length
umr_even:
        mov     byte ptr [edi+ISO_DR_LEN],cl
        popad
        ret

;--- udf_swap - EAX with its bytes reversed, which is what the second
;--- half of an ISO9660 both-endian pair holds.  BSWAP does this in one
;--- instruction and arrived with the 486; this kernel is assembled for
;--- a 386, so it is three.
udf_swap:
        xchg    al,ah
        rol     eax,16
        xchg    al,ah
        ret

;--- udf_isodate - the File Entry's timestamp in udf_fbuf, as the
;--- seven bytes an ISO9660 record carries.  EDI = the record.
udf_isodate:
        push    eax
        push    esi
        mov     esi,OFFSET udf_fbuf
        mov     eax,UDF_FE_MTIME
        cmp     word ptr [esi+UDF_TAG_ID],UDF_T_EFE
        jne     uid_off
        mov     eax,UDF_EFE_MTIME
uid_off:
        add     esi,eax
        movzx   eax,word ptr [esi+2]            ; the year, from 1900 here
        sub     eax,1900
        cmp     eax,255
        jbe     uid_year
        xor     eax,eax
uid_year:
        mov     byte ptr [edi+ISO_DR_DATE+0],al
        mov     al,[esi+4]
        mov     byte ptr [edi+ISO_DR_DATE+1],al ; month
        mov     al,[esi+5]
        mov     byte ptr [edi+ISO_DR_DATE+2],al ; day
        mov     al,[esi+6]
        mov     byte ptr [edi+ISO_DR_DATE+3],al ; hour
        mov     al,[esi+7]
        mov     byte ptr [edi+ISO_DR_DATE+4],al ; minute
        mov     al,[esi+8]
        mov     byte ptr [edi+ISO_DR_DATE+5],al ; second
        mov     byte ptr [edi+ISO_DR_DATE+6],0  ; and no timezone, for the
        pop     esi                             ; reason iso_stamp gives
        pop     eax
        ret

;=====================================================================
; udf_stamp - the modification time out of the File Entry in udf_fbuf,
;             into the entry at EDI.
;
; A UDF TIMESTAMP IS TWELVE BYTES and the year is a real year rather
; than an offset from anything, which makes this shorter than the
; ISO9660 one: the only arithmetic is DOS's own packing, and the one
; check worth making is that the year is inside the range a DOS date
; can hold at all.
;=====================================================================
udf_stamp:
        pushad
        mov     esi,OFFSET udf_fbuf
        mov     eax,UDF_FE_MTIME
        cmp     word ptr [esi+UDF_TAG_ID],UDF_T_EFE
        jne     ust_off
        mov     eax,UDF_EFE_MTIME
ust_off:
        add     esi,eax

        movzx   eax,word ptr [esi+2]            ; the year
        cmp     eax,1980
        jb      ust_out                         ; older than DOS itself
        sub     eax,1980
        cmp     eax,127
        ja      ust_out
        shl     eax,9
        movzx   ebx,byte ptr [esi+4]            ; month
        and     ebx,0Fh
        shl     ebx,5
        or      eax,ebx
        movzx   ebx,byte ptr [esi+5]            ; day
        and     ebx,1Fh
        or      eax,ebx
        mov     word ptr [edi+DE_DATE],ax

        movzx   eax,byte ptr [esi+6]            ; hour
        and     eax,1Fh
        shl     eax,11
        movzx   ebx,byte ptr [esi+7]            ; minute
        and     ebx,3Fh
        shl     ebx,5
        or      eax,ebx
        movzx   ebx,byte ptr [esi+8]            ; second, in two-second
        shr     ebx,1                           ; steps, which is all a DOS
        and     ebx,1Fh                         ; time has room for
        or      eax,ebx
        mov     word ptr [edi+DE_TIME],ax
ust_out:
        popad
        ret
