;=====================================================================
; IMAGE.INC - a FILE on one disk, given a drive letter of its own
;
; MOUNT E: C:\IMAGES\TEST.IMG makes E: a volume whose sectors live
; inside a file on C:.  An .ISO is read as ISO9660 or UDF, an .IMG,
; .IMA or .DSK as FAT - and whether it is one or the other is decided
; by looking at the contents, not at the extension, because the
; contents are the thing that has to be right.
;
; It is the piece that lets this machine test disk layouts without a
; host tool writing images: FORMAT, SYS, FDISK and the whole of
; DISKTEST can run against a file, and the file can then be booted in
; an emulator or written to a diskette.
;
;---------------------------------------------------------------------
; AN EXTENT MAP, NOT AN OPEN HANDLE, and this is the design decision
; the rest of the file follows from.
;
; The obvious shape is the one the roadmap wrote down: keep the file
; open and serve each sector with a seek and a read.  It is wrong here
; for three reasons, and the third is the one that decided it.
;
;   RE-ENTRANCY.  Serving a sector for E: by reading a file on C:
;   means calling vol_select in the middle of an operation that is
;   standing on E:'s volume state - and this kernel keeps ONE volume
;   loaded and swaps the others in and out of it.  The caller's
;   filesystem would be pulled out from under it while it was using
;   it.  Every alternative to that is a second copy of the FAT layer.
;
;   THE CRITICAL ERROR PATH.  disk_read may call INT 24h, whose
;   handler may call INT 21h.  A backing store that is itself an
;   INT 21h file operation puts a DOS call inside a DOS call inside a
;   DOS call, which is exactly the rule real DOS drew a line under.
;
;   THE CACHE, WHICH IS THE ONE THAT DECIDED IT.  The disk cache is
;   keyed by PHYSICAL DRIVE AND ABSOLUTE LBA.  An image's sectors ARE
;   real sectors, on a real disk, at real LBAs - so if the translation
;   happens ABOVE the cache, the image's blocks and the host file's
;   blocks are THE SAME BLOCKS.  Coherency between the two views comes
;   free, and it comes free because there is only one view.  A
;   handle-based reader would have needed a second cache and then a
;   rule for keeping the two honest, which is the bug nobody finds
;   until a file written through E: reads back stale through C:.
;
; So the file's cluster chain is turned into a list of runs ONCE, at
; mount time, and every transfer afterwards is a lookup in that list
; and an ordinary transfer to the host's drive.  Below dio_try nothing
; knows an image is involved at all.
;
; THE PRICE, STATED PLAINLY: THE FILE MAY NOT MOVE WHILE IT IS
; MOUNTED.  Deleting it, truncating it, or letting another operating
; system defragment the host volume leaves the map pointing at
; clusters that now belong to somebody else, and writes to the image
; then land on that somebody else.  Deleting, renaming and truncating
; are refused for a mounted file by img_locked, which the delete and
; create paths call; moving it from OUTSIDE this machine cannot be
; refused and is documented instead.  This is the same bargain every
; loopback mount has ever made.
;
;---------------------------------------------------------------------
; A FILE ON A NETWORK DRIVE IS READ THROUGH THE REDIRECTOR (IMF_NET).
; It has no sectors on this machine, so there is nothing to map - and
; none of the three reasons above applies to it.  A request to the
; redirector selects no volume and touches nothing of the FAT layer's:
; it is a block (img_nreq, its own, so net_req is never in the middle
; of somebody else's call) handed to the driver, with the driver's
; handle for the file and a 64-bit position.  It is not INT 21h, so no
; DOS call is nested.  And there is no cache to keep two views of: the
; file's bytes are not in the disk cache under any number.  So F1h
; AL=0Fh opens the file once through the driver, keeps the handle in
; IM_FILE, and a transfer on the letter is a READ or WRITE at sector
; times 512.  Any size, .ISO or hard disk, written where it is
; (Charlie, 2026-11-25).  The driver opens the file again by name after
; a reconnect; a share that is gone fails the transfer, which is a
; critical error like any disk's.  NET USE /DELETE is refused while an
; image is mounted from the drive (img_netbusy).
;
;---------------------------------------------------------------------
; A FILE WITH NO CLUSTERS TO MAP AND NO REDIRECTOR IS COPIED INSTEAD.
; One on a CD or an exFAT volume has extents this file does not walk,
; and a .IMZ is packed - so F1h AL=0Fh refuses them with ERR_NOTSUP,
; and MOUNT reads the file itself and hands the bytes to F1h AL=2Bh.  That copies them into
; a block of the kernel's (IMF_MEM), and a transfer on the letter is
; then a copy to or from that block: no host drive, no map, no cache.
; IM_GEN counts the writes.  The file is not touched until MOUNT /U,
; which takes the image back out (IMEM_COPY), writes it to the file and
; only then unmounts - and the unmount checks IM_GEN, so a write that
; came in between is not thrown away.  A diskette is the most it holds
; (IMG_MEM_MAX): a hard disk image belongs on a local FAT drive.
;
;---------------------------------------------------------------------
; ONE IMAGE IS ONE LETTER.  An image holding a bare volume is that
; volume; an image holding a whole disk - a master boot record and a
; partition table - is its FIRST FAT partition, and the rest are
; visible to FDISK and not to DIR.  The drive table already has a
; start LBA per letter, which is how a partition inside an image costs
; nothing: DRV_START is where the volume begins and it does not care
; whether the number came from a disk's partition table or a file's.
;
; Included by DOSMAIN.ASM after PATH.INC (it resolves a path) and
; before INT21.INC (which calls it for F1h AL=0Fh..11h).
;=====================================================================

; (IMG_MAX, IMG_UNIT_BASE and IMG_UNIT_TOP live in PMDOS.INC beside
;  DRVF_IMAGE.  They have to: ISO9660.INC and CRITERR.INC both test a
;  drive number against the range and both are assembled before this
;  file, and wasm cannot use an equate it has not read yet.)

; One mount.
IM_FLAGS        EQU     00h             ; db - IMF_*
IM_HOSTVOL      EQU     01h             ; db - the letter the FILE lives on
IM_HOSTDRV      EQU     02h             ; db - and that volume's BIOS drive
IM_LETTER       EQU     03h             ; db - the letter the IMAGE took
IM_CLUS         EQU     04h             ; dd - the file's first cluster
IM_SIZE         EQU     08h             ; dd - its length in bytes
IM_SECS         EQU     0Ch             ; dd - and in 512-byte sectors
IM_NEXT         EQU     10h             ; dd - extents in the map
IM_MAP          EQU     14h             ; dd - the map itself, from the arena;
                                        ; for IMF_MEM the image's bytes
IM_HINT         EQU     18h             ; dd - the extent the last lookup hit
IM_GEN          EQU     1Ch             ; dd - IMF_MEM: writes since it was
                                        ; loaded, so MOUNT /U can tell a
                                        ; copy it saved from a later change
IM_FILE         EQU     20h             ; dd - IMF_NET: the redirector's
                                        ; handle for the file (IM_HOSTVOL
                                        ; is its drive)
IM_PATH         EQU     24h             ; 80 bytes - what was mounted, so
                                        ; MOUNT can print it back - and for
                                        ; IMF_MEM, where it is saved to
IM_LEN          EQU     24h + 80

; (IMF_USED, IMF_RO and IMF_ISO are in PMDOS.INC too - they are part
;  of what AL=11h reports, so they belong where a program can see
;  them.)

; One run of consecutive sectors.  The image LBA is stored rather than
; derived so that a lookup is a comparison and not a running sum.
IX_ILBA         EQU     00h             ; dd - first image LBA in the run
IX_HLBA         EQU     04h             ; dd - the host LBA it lives at
IX_SECS         EQU     08h             ; dd - how far the run goes
IX_LEN          EQU     0Ch

; A map of this many runs is 48K and describes a file in 4096 pieces.
; A file in 4096 pieces is a file on a host volume that wants
; defragmenting, and saying so is more use than spending a megabyte of
; arena on it.
IMG_EXT_MAX     EQU     4096

IFDEF IMG_DEBUG
;=====================================================================
; The tracer.  BUILD img.
;
; It prints through IOS_CONSTR, which is the console and not a file -
; so a run that is being redirected shows these lines ON THE SCREEN
; and the redirected output stays clean.  That is the right way round
; for once: the trace is for a person watching, and the test's answer
; is for FATGET to read afterwards.
;=====================================================================
imgd_say:                               ; ESI -> ASCIIZ
        pushad
        call    dword ptr ds:[IOS_CONSTR]
        popad
        ret
imgd_crlf:
        pushad
        mov     esi,OFFSET msgImgEol
        call    dword ptr ds:[IOS_CONSTR]
        popad
        ret
imgd_hex:                               ; EAX, eight digits and a space
        pushad
        mov     edi,OFFSET imgd_buf
        mov     ecx,8
imgdh_lp:
        rol     eax,4
        mov     dl,al
        and     dl,0Fh
        add     dl,'0'
        cmp     dl,'9'
        jbe     imgdh_dig
        add     dl,7
imgdh_dig:
        mov     [edi],dl
        inc     edi
        dec     ecx
        jnz     imgdh_lp
        mov     byte ptr [edi],' '
        mov     byte ptr [edi+1],0
        mov     esi,OFFSET imgd_buf
        call    dword ptr ds:[IOS_CONSTR]
        popad
        ret
imgd_11:                                ; the eleven bytes at EDI
        pushad
        mov     esi,edi
        mov     edi,OFFSET imgd_buf
        mov     ecx,11
imgd11_lp:
        mov     al,[esi]
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     imgd11_lp
        mov     byte ptr [edi],' '
        mov     byte ptr [edi+1],0
        mov     esi,OFFSET imgd_buf
        call    dword ptr ds:[IOS_CONSTR]
        popad
        ret

imgd_buf    db  20 dup(0)
imgd_cf     db  0
            db  0,0,0
msgImgEol   db  13,10,0
msgImgPath  db  'IMG path: ',0
msgImgRes   db  'IMG resolve baddrv/cnt/noname/vol/ok/drv/fs: ',0
msgImgName  db  'IMG name_buf: ',0
msgImgSrch  db  'IMG search attr/size: ',0
msgImgNo    db  'NOT FOUND',0
ENDIF

;=====================================================================
; img_init - no images.  Called from dos_entry before anything can ask.
;=====================================================================
img_init:
        pushad
        mov     edi,OFFSET img_tbl
        xor     eax,eax
        mov     ecx,(IMG_MAX * IM_LEN) / 4
        rep     stosd
        popad
        ret

;=====================================================================
; img_slot - AL = a pseudo BIOS drive number.  CF clear with ESI ->
;            its mount; CF set if AL does not name one.  Everything
;            else preserved.
;=====================================================================
img_slot:
        cmp     al,IMG_UNIT_BASE
        jb      ims_no
        cmp     al,IMG_UNIT_TOP
        jae     ims_no
        push    eax
        movzx   esi,al
        sub     esi,IMG_UNIT_BASE
        imul    esi,IM_LEN
        add     esi,OFFSET img_tbl
        test    byte ptr [esi+IM_FLAGS],IMF_USED
        pop     eax
        jz      ims_no
        clc
        ret
ims_no:
        stc
        ret

;=====================================================================
; img_isiso - AL = a pseudo BIOS drive number.  CF clear if it names a
;             mounted image whose contents are read in 2048-byte
;             blocks.  fat_mount asks this to decide which of the two
;             filesystems to call.
;=====================================================================
img_isiso:
        push    esi
        call    img_slot
        jc      iis_no
        test    byte ptr [esi+IM_FLAGS],IMF_ISO
        jz      iis_no
        pop     esi
        clc
        ret
iis_no:
        pop     esi
        stc
        ret

;=====================================================================
; img_locked - is this file mounted as an image?
;
;   AL = volume (0 = A:), EAX high bytes ignored, EBX = first cluster
;   CF set if it is, and the caller must refuse whatever it was about
;   to do to it.  Everything preserved.
;
; A CLUSTER AND A VOLUME, NOT A NAME.  A name can be reached by more
; than one path - "..\IMAGES\X.IMG" and "\IMAGES\X.IMG" are the same
; file - and comparing text would have let the second spelling delete
; what the first one mounted.  The first cluster is the file's
; identity as far as the filesystem is concerned, which is the level
; the damage would happen at.
;=====================================================================
img_locked:
        push    ecx
        push    esi
        mov     esi,OFFSET img_tbl
        mov     ecx,IMG_MAX
iml_lp:
        test    byte ptr [esi+IM_FLAGS],IMF_USED
        jz      iml_next
        test    byte ptr [esi+IM_FLAGS],IMF_MEM OR IMF_NET
        jnz     iml_next                ; a copy, or a file on a share: no
                                        ; cluster here to protect, and an
                                        ; IM_CLUS of 0 would match every
                                        ; empty file on the volume
        cmp     al,[esi+IM_HOSTVOL]
        jne     iml_next
        cmp     ebx,[esi+IM_CLUS]
        je      iml_yes
iml_next:
        add     esi,IM_LEN
        dec     ecx
        jnz     iml_lp
        pop     esi
        pop     ecx
        clc
        ret
iml_yes:
        pop     esi
        pop     ecx
        stc
        ret

;=====================================================================
; img_find - ESI = a mount, EBX = an image LBA.
;            CF clear with EAX = the host LBA it lives at and
;            EBP = how many sectors run on from there before the map
;            says something else.  CF set past the end of the image.
;
; THE HINT IS THE WHOLE OF THE OPTIMISATION and it is worth one line:
; a filesystem reads forwards, so the run that answered last time
; answers this time far more often than not.  Starting the scan there
; and wrapping makes a fragmented image cost one comparison per
; transfer instead of a walk of the map.
;=====================================================================
img_find:
        push    ecx
        push    edx
        push    edi
        mov     ecx,[esi+IM_NEXT]
        or      ecx,ecx
        jz      imf_no
        mov     edx,[esi+IM_HINT]       ; where to start looking
        cmp     edx,ecx
        jb      imf_go
        xor     edx,edx
imf_go:
        mov     edi,ecx                 ; EDI = runs still to try
imf_lp:
        mov     eax,edx
        imul    eax,IX_LEN
        add     eax,[esi+IM_MAP]        ; EAX -> the run
        mov     ebp,[eax+IX_ILBA]
        cmp     ebx,ebp
        jb      imf_step
        add     ebp,[eax+IX_SECS]
        cmp     ebx,ebp
        jae     imf_step

        ; found it: EBX is this run's [IX_ILBA] plus some offset
        mov     [esi+IM_HINT],edx
        mov     ebp,ebx
        sub     ebp,[eax+IX_ILBA]       ; EBP = offset into the run
        mov     edx,[eax+IX_HLBA]
        add     edx,ebp
        mov     ecx,[eax+IX_SECS]
        sub     ecx,ebp                 ; ECX = sectors left in the run
        mov     eax,edx
        mov     ebp,ecx
        pop     edi
        pop     edx
        pop     ecx
        clc
        ret
imf_step:
        inc     edx
        cmp     edx,ecx
        jb      imf_nowrap
        xor     edx,edx
imf_nowrap:
        dec     edi
        jnz     imf_lp
imf_no:
        pop     edi
        pop     edx
        pop     ecx
        stc
        ret

;=====================================================================
; img_io  - what dio_try calls instead of cache_io
; img_raw - what dio_raw calls instead of the block layer
;
;   AL = drive   EBX = LBA   CX = sector count   EDI = buffer
;   EDX = IOS_RDBLK or IOS_WRBLK
;
;   CF set on failure with AH = a status byte, exactly as cache_io.
;   EVERY OTHER REGISTER COMES BACK, AH INCLUDED IN THE EXCEPTION -
;   see the header of cache_io, and see v0.59's ESI, which is the
;   reason this is a wrapper round a body rather than a routine.  A
;   thing that replaces dio_raw or cache_io inherits every promise
;   they made, including the ones nobody wrote down.
;
; A DRIVE THAT IS NOT AN IMAGE FALLS STRAIGHT THROUGH, which is what
; lets these simply take the other routine's place at the call site:
; one comparison on a path that already had several.
;
; THE TWO ENTRIES DIFFER ONLY IN WHAT THEY TRANSLATE *TO*.  img_io
; hands the host's drive and LBA to the CACHE, which is the whole
; point of translating above it.  img_raw hands them to the block
; layer, because its caller - VERIFY's read-back, and INT 25h/26h -
; asked to go round the cache and would learn nothing from being
; shown the copy of the sector it just wrote.
;=====================================================================
img_io:
        cmp     al,IMG_UNIT_BASE
        jb      cache_io
        cmp     al,IMG_UNIT_TOP
        jae     cache_io
        mov     byte ptr [ii_thru],0
        jmp     short img_enter
img_raw:
        cmp     al,IMG_UNIT_BASE
        jb      dio_raw
        cmp     al,IMG_UNIT_TOP
        jae     dio_raw
        mov     byte ptr [ii_thru],1
img_enter:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        call    img_iobody

        ; EAX COMES BACK WHOLE EXCEPT FOR AH, which is the contract
        ; cache_io states and this routine inherits by standing in its
        ; place.  Saving AH, restoring all of EAX and putting AH back
        ; is three instructions and it is the only way to honour "every
        ; register but AH" exactly - popping nothing and rebuilding AL
        ; by hand, which is what this did first, quietly threw away the
        ; top half of the caller's EAX.  None of MOV, POP or MOV
        ; touches the flags, so CF still carries the answer out.
        mov     [ii_ah],ah
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        mov     ah,[ii_ah]
        ret

img_iobody:
        call    img_slot                ; -> ESI = the mount
        jc      iib_bad
        movzx   ecx,cx
        or      ecx,ecx
        jz      iib_done                ; nothing asked for, nothing wrong

        cmp     edx,IOS_WRBLK
        jne     iib_rdok
        test    byte ptr [esi+IM_FLAGS],IMF_RO
        jnz     iib_wp                  ; refused BEFORE anything moves
iib_rdok:
        test    byte ptr [esi+IM_FLAGS],IMF_MEM
        jnz     iib_mem
        test    byte ptr [esi+IM_FLAGS],IMF_NET
        jnz     iib_net

iib_lp:
        or      ecx,ecx
        jz      iib_done
        push    ecx
        call    img_find                ; -> EAX = host LBA, EBP = run left
        pop     ecx
        jc      iib_bad                 ; off the end of the image

        ; this transfer takes the smaller of "what is left to move" and
        ; "what is left in this run", and comes back for the rest.
        ;
        ; THE CHUNK SIZE GOES IN MEMORY AND NOT IN A REGISTER ACROSS
        ; THE CALL.  cache_io and dio_raw both promise every register
        ; back, and both promises are true - but the whole of v0.59's
        ; worst bug was a routine relying on a promise like that one
        ; level further down, and three cursors have to be advanced by
        ; this number AFTER the call.  A variable costs one store.
        mov     [ii_lba],eax
        cmp     ebp,ecx
        jbe     iib_part
        mov     ebp,ecx
iib_part:
        mov     [ii_take],ebp
        push    ebx
        push    ecx
        push    esi
        push    edi
        mov     al,[esi+IM_HOSTDRV]
        mov     ebx,[ii_lba]
        mov     ecx,[ii_take]
        cmp     byte ptr [ii_thru],0
        jne     iib_thrraw
        call    cache_io
        jmp     short iib_moved
iib_thrraw:
        call    dio_raw
iib_moved:
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        jc      iib_status

        ; advance all three cursors by what moved
        mov     ebp,[ii_take]
        add     ebx,ebp
        sub     ecx,ebp
        mov     eax,ebp
        shl     eax,9
        add     edi,eax
        jmp     short iib_lp

iib_done:
        xor     ah,ah
        clc
        ret
iib_status:                             ; the drive's own status byte,
        stc                             ; carried up unaltered
        ret
iib_bad:
        mov     ah,0Ch                  ; "general failure", as the block
        stc                             ; layer spells it
        ret
iib_wp:
        mov     ah,03h                  ; "write protected", which is what
        stc                             ; a read-only mount is
        ret

;--- an image held in memory: the transfer is a copy, and the bounds
;--- are checked before anything moves.  ECX is not 0 (tested above).
iib_mem:
        mov     eax,[esi+IM_SECS]
        cmp     ebx,eax
        jae     iib_bad                 ; it starts past the end
        sub     eax,ebx
        cmp     ecx,eax
        ja      iib_bad                 ; ...or runs on past it
        shl     ebx,9                   ; IMG_MEM_MAX keeps this small
        add     ebx,[esi+IM_MAP]        ; EBX -> the first sector in memory
        shl     ecx,7                   ; sectors -> dwords
        cld
        cmp     edx,IOS_WRBLK
        je      iib_memwr
        push    esi
        mov     esi,ebx
        rep     movsd                   ; the image -> the caller
        pop     esi
        jmp     iib_done
iib_memwr:
        push    esi
        mov     esi,edi
        mov     edi,ebx
        rep     movsd                   ; the caller -> the image
        pop     esi
        inc     dword ptr [esi+IM_GEN]  ; MOUNT /U's reason to save
        jmp     iib_done

;--- a file on a network drive: one READ or WRITE through the
;--- redirector at sector times 512, which is 64 bits wide.  The bounds
;--- are checked first, as for memory; CX was 16 bits, so the count in
;--- bytes is 32MB at most.  A transfer that comes back short means the
;--- file is shorter than it was when it was mounted.
iib_net:
        mov     eax,[esi+IM_SECS]
        cmp     ebx,eax
        jae     iib_bad
        sub     eax,ebx
        cmp     ecx,eax
        ja      iib_bad
        call    img_nclear
        mov     dword ptr [img_nreq+NR_OP],REDIR_READ
        cmp     edx,IOS_WRBLK
        jne     iin_op
        mov     dword ptr [img_nreq+NR_OP],REDIR_WRITE
iin_op:
        movzx   eax,byte ptr [esi+IM_HOSTVOL]
        mov     [img_nreq+NR_DRIVE],eax
        mov     eax,[esi+IM_FILE]
        mov     [img_nreq+NR_HANDLE],eax
        mov     eax,ebx
        shl     eax,9
        mov     [img_nreq+NR_POSLO],eax
        mov     eax,ebx
        shr     eax,23
        mov     [img_nreq+NR_POSHI],eax
        mov     eax,ecx
        shl     eax,9
        mov     [img_nreq+NR_COUNT],eax
        mov     [img_nreq+NR_BUFPTR],edi
        call    img_nfire
        jc      iib_bad
        cmp     dword ptr [img_nreq+NR_STATUS],0
        jne     iib_bad
        mov     eax,[img_nreq+NR_RESULT]
        cmp     eax,[img_nreq+NR_COUNT]
        jne     iib_bad
        jmp     iib_done

;=====================================================================
; img_block - read ECX blocks of 2048 from block EBX of image unit AL
;             into EDI.  The optical door: an .ISO mounted from a file
;             is read in the filesystem's blocks, not in sectors.
;             CF on failure.  All registers preserved.
;=====================================================================
img_block:
        pushad
        shl     ebx,2                   ; a block is four of our sectors
        shl     ecx,2
        jz      imb_bad
        cmp     ecx,0FFFFh
        ja      imb_bad
        mov     edx,IOS_RDBLK
        call    img_io
        jc      imb_bad
        popad
        clc
        ret
imb_bad:
        popad
        stc
        ret

;=====================================================================
; img_mount - give a file a drive letter.
;
;   ESI -> ASCIIZ path of the image file
;   AL  = the letter to use (0 = A:), or FFh for the next free one
;   AH  = IMF_RO to mount it read-only, else 0
;
;   CF clear: AL = the letter it took.
;   CF set:   AX = a DOS error.  The ones it can return and what each
;             one means HERE, because a mount can fail in more ways
;             than a file operation can:
;
;               2  ERR_NOFILE    no such file
;               3  ERR_NOPATH    no such directory, or the host volume
;                                is not a FAT one (an image inside an
;                                image, or on a CD, has no cluster
;                                chain to map)
;               5  ERR_ACCESS    it is a directory, or it is empty, or
;                                its contents are not a filesystem
;                                this kernel can read
;               8  ERR_NOMEM     the arena would not give up a map
;              15  ERR_BADDRIVE  no such letter, or it is taken, or
;                                there are no image slots left
;              50  ERR_NOTSUP    the file is on a volume with no
;                                clusters to map and no redirector - a
;                                CD, exFAT, another image, a driver's
;                                unit - and MOUNT has to load it (F1h
;                                AL=2Bh).  A file on a network drive is
;                                mounted where it is (img_netopen).
;=====================================================================
img_mount:
        pushad
        mov     [imm_letter],al
        mov     [imm_flags],ah
        mov     [imm_path],esi
        call    img_claim               ; a slot and a letter
        jc      imm_fail

;--- find the file ---------------------------------------------------
; A NETWORK DRIVE GOES ITS OWN WAY BEFORE ITS PATH IS WALKED.  walk_to
; is FAT's directory walk, and on a share it read sectors from a drive
; that is not there, with the last volume's layout - a critical error
; for a file in a directory.  resolve_core only parses the path and
; selects the volume, which is enough to know what the volume is.
        mov     esi,[imm_path]
        call    resolve_core
        jc      imm_nopath
        cmp     byte ptr [vol_fs],FS_NET
        je      imm_net
        mov     esi,[imm_path]
IFDEF IMG_DEBUG
        push    esi
        mov     esi,OFFSET msgImgPath
        call    imgd_say
        mov     eax,[imm_path]          ; the POINTER, to compare with what
        call    imgd_hex                ; MOUNT thinks it passed
        pop     esi
        call    imgd_say                ; ...and the bytes it points at
        call    imgd_crlf
        push    esi
ENDIF
        call    resolve_parent
IFDEF IMG_DEBUG
        pushfd
        mov     esi,OFFSET msgImgRes
        call    imgd_say
        movzx   eax,byte ptr [rp_baddrv]
        call    imgd_hex
        mov     eax,[rp_cnt]
        call    imgd_hex
        movzx   eax,byte ptr [rp_noname]
        call    imgd_hex
        movzx   eax,byte ptr [cur_vol]
        call    imgd_hex
        movzx   eax,byte ptr [fat_ok]
        call    imgd_hex
        movzx   eax,byte ptr [fat_drive]
        call    imgd_hex
        movzx   eax,byte ptr [vol_fs]
        call    imgd_hex
        call    imgd_crlf
        popfd
        pop     esi
ENDIF
        jc      imm_nopath
        cmp     byte ptr [rp_noname],0
        jne     imm_isdir               ; "\" or "..": a directory, not a file
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      imm_wild                ; a wildcard names no one file

        ; THE HOST HAS TO BE A FAT VOLUME.  Everything below walks a
        ; cluster chain; an ISO9660 file has extents instead and a
        ; second image's file has a map, and while both could be made
        ; to work neither is what this call is for.  Refused with
        ; ERR_NOTSUP, which tells MOUNT the file is there and has to be
        ; loaded instead (F1h AL=2Bh) - ERR_NOPATH said "no such
        ; directory", which was not the trouble.
        cmp     byte ptr [vol_fs],FS_FAT
        jne     imm_notsup

        ; ...AND NOT A FAT VOLUME THAT IS ITSELF AN IMAGE.  Stacking
        ; them would work arithmetically - each level translates to
        ; the next one's numbers and the bottom is always a real disk
        ; - and it would corrupt itself immediately, because the
        ; working variables img_iobody uses are single and a nested
        ; transfer would overwrite the outer one's.  A stack of them
        ; is three more variables and no use anybody has asked for, so
        ; this is refused honestly instead of half-supported.  (The
        ; test takes in the driver and ATAPI units above the images
        ; too, so a file on RAMDISK.SYS is loaded rather than mapped -
        ; which on a RAM disk costs nothing.)
        cmp     byte ptr [fat_drive],IMG_UNIT_BASE
        jae     imm_notsup

        mov     byte ptr [srch_attr],0
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
IFDEF IMG_DEBUG
        mov     esi,OFFSET msgImgName
        call    imgd_say
        call    imgd_11                 ; the eleven-byte pattern itself
        call    imgd_crlf
        mov     edi,OFFSET name_buf
ENDIF
        call    fat_search
IFDEF IMG_DEBUG
        ; CF IS TESTED BEFORE ANYTHING ELSE IS CALLED.  pushfd does not
        ; disturb the flags, but the routines below certainly do, and a
        ; "jc" placed after the first of them reports the tracer's own
        ; arithmetic instead of fat_search's answer.
        pushfd
        mov     byte ptr [imgd_cf],0
        jnc     imgd_srch0
        mov     byte ptr [imgd_cf],1
imgd_srch0:
        push    esi
        mov     esi,OFFSET msgImgSrch
        call    imgd_say
        pop     esi
        cmp     byte ptr [imgd_cf],0
        jne     imgd_srchbad
        movzx   eax,byte ptr [esi+DE_ATTR]
        call    imgd_hex
        mov     eax,[esi+DE_SIZE]
        call    imgd_hex
        jmp     short imgd_srchdone
imgd_srchbad:
        mov     esi,OFFSET msgImgNo
        call    imgd_say
imgd_srchdone:
        call    imgd_crlf
        popfd
ENDIF
        jc      imm_nofile

        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jnz     imm_denied
        test    byte ptr [esi+DE_ATTR],ATTR_VOLUME
        jnz     imm_denied
        mov     eax,[esi+DE_SIZE]
        mov     [imm_size],eax
        cmp     eax,512
        jb      imm_denied              ; not even one sector of anything
        call    de_getclus
        or      eax,eax
        jz      imm_denied              ; a length with no clusters under it
        mov     [imm_clus],eax

        ; ...AND NOT A FILE THAT IS ALREADY A DRIVE LETTER.  Mounting
        ; one image on two letters gives the same sectors TWO volumes,
        ; each with its own FAT cache, its own free-cluster hint and
        ; its own idea of the directory - and the second one to write
        ; wins, silently, on a filesystem the first one still believes
        ; in.  It is the "one reader, many letters" bug of v0.57 with
        ; the writing enabled.
        ;
        ; img_locked exists for the delete path and answers exactly
        ; this question, so the refusal costs one call.
        push    ebx
        mov     ebx,eax
        mov     al,[cur_vol]
        call    img_locked
        pop     ebx
        jc      imm_already

        ; A FILE READ-ONLY ON THE HOST IS READ-ONLY AS A VOLUME.  The
        ; attribute is a statement about the bytes and the bytes are
        ; the same bytes; honouring it here costs one test and stops
        ; MOUNT being the way round R.
        test    byte ptr [esi+DE_ATTR],ATTR_READONLY
        jz      imm_notro
        or      byte ptr [imm_flags],IMF_RO
imm_notro:

;--- fill the slot, then map the chain -------------------------------
; The slot is filled BEFORE the map is built because img_io - which
; the sniff below goes through - finds the mount by its unit number.
; Nothing outside this file can see it yet: no letter names it until
; the very end.
        mov     esi,[imm_slot]
        mov     al,[cur_vol]
        mov     [esi+IM_HOSTVOL],al
        mov     al,[fat_drive]
        mov     [esi+IM_HOSTDRV],al
        mov     eax,[imm_clus]
        mov     [esi+IM_CLUS],eax
        mov     eax,[imm_size]
        mov     [esi+IM_SIZE],eax
        shr     eax,9                   ; whole sectors only: a tail of
        mov     [esi+IM_SECS],eax       ; less than one cannot be addressed
        mov     dword ptr [esi+IM_NEXT],0
        mov     dword ptr [esi+IM_MAP],0
        mov     dword ptr [esi+IM_HINT],0
        mov     dword ptr [esi+IM_GEN],0
        mov     al,[imm_flags]
        or      al,IMF_USED
        mov     [esi+IM_FLAGS],al
        mov     al,[imm_letter]
        mov     [esi+IM_LETTER],al

        ; the path, for MOUNT's listing.  Canonical rather than as
        ; typed, so that the listing names one file per line however
        ; many ways there are to spell it.
        call    imm_savepath

        call    img_map                 ; -> CF, AX = why not
        jc      imm_unwind

;--- what is in it? --------------------------------------------------
        call    img_sniff               ; sets DRV_* fields in imm_*
        jc      imm_unwind2

        call    img_assign              ; and now it has a letter
        popad
        mov     al,[imm_letter]
        clc
        ret

;--- a file on a network drive: opened through the redirector and read
;--- where it is (img_netopen).  The sniff and the letter are a mapped
;--- file's, and a sniff that finds nothing closes the file again.
imm_net:
        call    img_netopen             ; -> CF, AX; the slot filled
        jc      imm_fail
        call    img_sniff
        jc      imm_netbad
        call    img_assign
        popad
        mov     al,[imm_letter]
        clc
        ret
imm_netbad:
        push    eax
        mov     esi,[imm_slot]
        call    img_netclose
        pop     eax
        jmp     imm_unwind

;--- unwinds ---------------------------------------------------------
; imm_unwind2 has a map to give back; imm_unwind does not.  Neither
; has a letter to give back, which is the whole reason the letter is
; claimed last.
imm_unwind2:
        push    eax
        mov     esi,[imm_slot]
        call    img_unmap
        pop     eax
imm_unwind:
        mov     esi,[imm_slot]
        mov     byte ptr [esi+IM_FLAGS],0
        mov     [imm_err],ax
        popad
        mov     ax,[imm_err]
        stc
        ret
imm_nofile:
        mov     ax,ERR_NOFILE
        jmp     short imm_fail
imm_isdir:
imm_wild:
        mov     ax,ERR_ACCESS           ; a directory or a pattern: neither
        jmp     short imm_fail          ; is one file to mount
imm_already:
        mov     ax,ERR_EXISTS           ; it already has a letter, and the
        jmp     short imm_fail          ; answer is that letter, not a second
imm_nopath:
        mov     ax,ERR_NOPATH
        jmp     short imm_fail
imm_notsup:
        mov     ax,ERR_NOTSUP           ; there, but not mappable from here
        jmp     short imm_fail
imm_denied:
        mov     ax,ERR_ACCESS
imm_fail:
        mov     [imm_err],ax
        popad
        mov     ax,[imm_err]
        stc
        ret

;---------------------------------------------------------------------
; img_claim - a free image slot, and the letter [imm_letter] names
;             (FFh = the next free one).  Sets imm_slot, imm_unit and
;             imm_letter.  CF set with AX = ERR_BADDRIVE when there is
;             no slot or no such letter.  Nothing is taken yet: the
;             slot stays empty and the letter free until the caller
;             fills them, so a failure after this has nothing to undo.
;---------------------------------------------------------------------
img_claim:
        pushad
        mov     esi,OFFSET img_tbl
        xor     ecx,ecx
imcl_free:
        test    byte ptr [esi+IM_FLAGS],IMF_USED
        jz      imcl_gotslot
        add     esi,IM_LEN
        inc     ecx
        cmp     ecx,IMG_MAX
        jb      imcl_free
        jmp     short imcl_no
imcl_gotslot:
        mov     [imm_slot],esi
        mov     eax,ecx
        add     eax,IMG_UNIT_BASE
        mov     [imm_unit],al

; FFh means "the next one after everything the scan found", which is
; the same rule a CD-ROM follows and for the same reason: a letter
; handed out in the middle would renumber the disks.
        movzx   eax,byte ptr [imm_letter]
        cmp     al,0FFh
        jne     imcl_named
        movzx   eax,byte ptr [drv_count]
        cmp     eax,[drv_ceil]                  ; LASTDRIVE= (drv_fixletters)
        jae     imcl_no
        jmp     short imcl_have
imcl_named:
        cmp     eax,[drv_ceil]
        jae     imcl_no
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jnz     imcl_no                 ; taken, and this call does not
                                        ; get to decide that C: is free
imcl_have:
        mov     [imm_letter],al
        popad
        clc
        ret
imcl_no:
        popad
        mov     ax,ERR_BADDRIVE
        stc
        ret

;---------------------------------------------------------------------
; img_assign - the mount at [imm_slot] is filled in and its contents
;              known: give it the letter.  imm_letter, imm_unit,
;              imm_ptype, imm_start and imm_secs say how.  Everything
;              preserved.
;---------------------------------------------------------------------
img_assign:
        pushad
        movzx   eax,byte ptr [imm_letter]
        call    drv_slot
        mov     al,[imm_unit]
        mov     [ebx+DRV_BIOS],al
        mov     al,[imm_ptype]
        mov     [ebx+DRV_TYPE],al
        mov     byte ptr [ebx+DRV_PIDX],0FFh
        mov     eax,[imm_start]
        mov     [ebx+DRV_START],eax
        mov     eax,[imm_secs]
        mov     [ebx+DRV_SECS],eax
        mov     byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT OR DRVF_IMAGE

        ; drv_count is a high-water mark, and a named letter can be
        ; past it - the same rule CDROM= follows
        movzx   eax,byte ptr [imm_letter]
        inc     eax
        cmp     al,[drv_count]
        jbe     imm_counted
        mov     [drv_count],al
imm_counted:

        ; THE VOLUME IS LEFT UNMOUNTED ON PURPOSE.  A CD's letter
        ; exists before anything is in the drive and mounts when
        ; somebody looks; an image is the same shape, and doing it the
        ; same way means the mount goes through fat_mount - one path,
        ; already tested - instead of a second copy of it here.
        ;
        ; The letter's parked state is cleared rather than assumed
        ; clear: a letter that held an image before this one still has
        ; that image's root directory and cluster hint in its slot,
        ; and vol_select would believe them.
        mov     al,[imm_letter]
        call    img_dropvol
        popad
        ret

;---------------------------------------------------------------------
; imm_savepath - put the canonical name of the file just resolved into
;                the slot at [imm_slot], truncated to fit.
;
; rp_names holds the resolution and name_buf holds the last component,
; which between them are the whole path: resolve_parent leaves the
; DIRECTORY in rp_names and the file's name beside it.
;---------------------------------------------------------------------
imm_savepath:
        pushad
        mov     edi,[imm_slot]
        add     edi,IM_PATH
        mov     al,[cur_vol]
        add     al,'A'
        mov     [edi],al
        mov     byte ptr [edi+1],':'
        mov     byte ptr [edi+2],'\'
        add     edi,3

        ; the DIRECTORY, which is rp_cnt LESS ONE component: after
        ; resolve_parent rp_names still holds the WHOLE path and
        ; name_buf holds its last component, so rendering all of
        ; rp_cnt and then appending name_buf names the file twice -
        ; "C:\TESTS\TESTIMG.IMG\TESTIMG.IMG", which is what MOUNT's
        ; first listing printed.  resolve_parent's own comment says
        ; the context is the PARENT; the array it leaves behind is
        ; not.
        ;
        ; If it will not fit, the name alone is printed: a truncated
        ; path in a listing is worse than an honest partial one.
        ; THE BUDGET, COUNTED OUT RATHER THAN ESTIMATED, because the
        ; whole field is eighty bytes and every one of them is spoken
        ; for: three for "C:\", the directory, one backslash, up to
        ; twelve for the filename and one for the NUL.  Eighty less
        ; those seventeen is sixty-three, and the first version of
        ; this line said sixty-four - which fits in eighty-ONE.
        mov     esi,OFFSET rp_names
        mov     ecx,[rp_cnt]
        or      ecx,ecx
        jz      imp_nodir               ; the file is in the root
        dec     ecx                     ; ...the PARENT's components
        jz      imp_nodir
        call    names_len
        cmp     eax,80 - 3 - 1 - 12 - 1
        ja      imp_nodir
        call    names_string
imp_scan:
        cmp     byte ptr [edi],0
        je      imp_slash
        inc     edi
        jmp     short imp_scan
imp_slash:
        mov     byte ptr [edi],'\'
        inc     edi
imp_nodir:
        mov     esi,OFFSET name_buf
        call    name83_str
        popad
        ret

;=====================================================================
; img_map - turn the file's cluster chain into a list of runs.
;           ESI = the mount, whose IM_CLUS and IM_SECS are set and
;           whose host volume is the one currently selected.
;           CF set with AX = ERR_NOMEM or ERR_ACCESS.
;
; TWO PASSES, and the first one exists so the second can be exact.
; Counting first means the arena is asked for what the map actually
; needs rather than for a ceiling, which matters on a machine where
; the ceiling is 48K and the answer is usually one extent: an image
; written by MKDISK, or by FORMAT, or copied onto a fresh volume, is
; contiguous, and a contiguous image has a map of twelve bytes.
;
; The FAT is walked twice and costs nothing extra the second time -
; the whole chain is in fat_buf on FAT12, and in the cache otherwise.
;=====================================================================
img_map:
        pushad
        mov     byte ptr [imp_count],1  ; pass 1: count only
        call    img_walk
        jc      imm_mapbad
        mov     eax,[imp_runs]
        or      eax,eax
        jz      imm_mapshort
        cmp     eax,IMG_EXT_MAX
        ja      imm_mapfrag

        ; THE MAP BELONGS TO THE KERNEL, NOT TO WHOEVER TYPED MOUNT.
        ; arena_alloc stamps every block with [cur_own] and 4Ch sweeps
        ; the arena for the exiting program's blocks - so a map bought
        ; while MOUNT.EXE is running is freed the moment MOUNT.EXE
        ; ends, and the image goes on reading it.  This is v0.59's
        ; cache bug exactly, and it is written out here rather than
        ; cross-referenced because the next thing that allocates from
        ; inside a program will need to be told a third time.
        mov     [imp_cap],eax
        imul    ebx,eax,IX_LEN
        push    esi                     ; ESI is the mount and arena_alloc
        push    dword ptr [cur_own]     ; has never been asked to keep it
        mov     dword ptr [cur_own],0
        call    arena_alloc
        pop     dword ptr [cur_own]
        pop     esi
        jc      imm_mapmem
        mov     [esi+IM_MAP],eax

        mov     byte ptr [imp_count],0  ; pass 2: fill it in
        call    img_walk
        jc      imm_mapbad2
        mov     eax,[imp_runs]
        mov     [esi+IM_NEXT],eax
        mov     dword ptr [esi+IM_HINT],0
        popad
        clc
        ret
; FIVE WAYS TO FAIL AND FIVE ANSWERS, which is not tidiness: a mount
; goes wrong in ways a file operation has no word for, and collapsing
; them into ERR_ACCESS makes the one message cover "it is a directory",
; "the arena is full" and "the FAT is broken" at once.  The first
; person to read that message had to guess, and guessed wrong.
imm_mapmem:
        mov     ax,ERR_NOMEM            ; the arena would not give a map
        jmp     short imm_mapfail
imm_mapfrag:
        mov     ax,ERR_DISKFULL         ; the file is in too many pieces
        jmp     short imm_mapfail
imm_mapbad2:
        call    img_unmap               ; the map bought for pass 2
imm_mapshort:
imm_mapbad:
        mov     ax,ERR_GENERAL          ; the chain does not cover the file
imm_mapfail:
        mov     [imm_err],ax
        popad
        mov     ax,[imm_err]
        stc
        ret

;---------------------------------------------------------------------
; img_walk - walk the chain, coalescing consecutive clusters into
;            runs.  [imp_count] = 1 counts them, 0 writes them.
;            ESI = the mount.  [imp_runs] = how many there were.
;
; A run ENDS when the next cluster is not the one after this one, and
; that test is the whole of the coalescing: a file written in one go
; has one run however many clusters it holds.
;
; It also ends at IM_SECS, because the last cluster of a file is only
; partly the file's - and a map that ran to the end of it would offer
; the volume sectors the image does not own.
;---------------------------------------------------------------------
img_walk:
        pushad
        mov     dword ptr [imp_runs],0
        mov     dword ptr [imw_isec],0  ; image LBA of the run being built
        mov     dword ptr [imw_len],0   ; ...and how long it is so far
        mov     dword ptr [imw_host],0
        mov     ecx,[esi+IM_SECS]
        mov     [imw_left],ecx
        or      ecx,ecx
        jz      imw_done
        mov     eax,[esi+IM_CLUS]
        mov     [imw_clus],eax
        mov     dword ptr [imw_prev],0

imw_lp:
        mov     eax,[imw_clus]
        cmp     eax,2
        jb      imw_bad
        cmp     eax,[fat_eoc]
        jae     imw_end
        mov     edx,[fat_totclus]
        add     edx,2                   ; the first number that is not one
        cmp     eax,edx
        jae     imw_bad

        call    clus2lba                ; -> EAX = host LBA of the cluster
        mov     [imw_lba],eax

        ; how many of this cluster's sectors the image still wants
        mov     ecx,[fat_spc]
        cmp     ecx,[imw_left]
        jbe     imw_full
        mov     ecx,[imw_left]
imw_full:
        mov     [imw_take],ecx

        ; does it continue the run being built?
        cmp     dword ptr [imw_len],0
        je      imw_start
        mov     eax,[imw_host]
        add     eax,[imw_len]
        cmp     eax,[imw_lba]
        jne     imw_flush               ; a gap: close this run, open one
        mov     eax,[imw_take]
        add     [imw_len],eax
        jmp     short imw_step
imw_flush:
        call    imw_emit
        jc      imw_bad
imw_start:
        mov     eax,[imw_lba]
        mov     [imw_host],eax
        mov     eax,[imw_take]
        mov     [imw_len],eax

imw_step:
        mov     eax,[imw_take]
        sub     [imw_left],eax
        cmp     dword ptr [imw_left],0
        je      imw_end
        mov     eax,[imw_clus]
        mov     [imw_prev],eax
        call    fat_next
        mov     [imw_clus],eax
        jmp     imw_lp

imw_end:
        cmp     dword ptr [imw_len],0
        je      imw_done
        call    imw_emit
        jc      imw_bad

        ; THE CHAIN MUST COVER THE FILE.  A chain that ends early is a
        ; cross-linked or truncated file, and mounting it would give
        ; the volume a length its sectors do not back.  CHKDSK's
        ; question, asked here because here is where the answer
        ; matters.
        cmp     dword ptr [imw_left],0
        jne     imw_bad
imw_done:
        popad
        clc
        ret
imw_bad:
        popad
        stc
        ret

;--- imw_emit - one finished run into the map (or just counted) ------
imw_emit:
        push    eax
        push    edi
        mov     eax,[imp_runs]
        cmp     byte ptr [imp_count],0
        jne     ime_justcount
        cmp     eax,[imp_cap]           ; the runs pass 1 made room for
        jae     ime_full
        imul    edi,eax,IX_LEN
        add     edi,[esi+IM_MAP]
        mov     eax,[imw_isec]
        mov     [edi+IX_ILBA],eax
        mov     eax,[imw_host]
        mov     [edi+IX_HLBA],eax
        mov     eax,[imw_len]
        mov     [edi+IX_SECS],eax
        mov     eax,[imp_runs]
ime_justcount:
        inc     eax
        mov     [imp_runs],eax
        mov     eax,[imw_isec]          ; the next run starts where this
        add     eax,[imw_len]           ; one finished
        mov     [imw_isec],eax
        mov     dword ptr [imw_len],0
        pop     edi
        pop     eax
        clc
        ret
ime_full:
        pop     edi
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; img_unmap - give a mount's map back to the arena.  ESI = the mount.
;---------------------------------------------------------------------
img_unmap:
        pushad
        mov     edx,[esi+IM_MAP]
        or      edx,edx
        jz      imu_none
        call    arena_free
imu_none:
        mov     dword ptr [esi+IM_MAP],0
        mov     dword ptr [esi+IM_NEXT],0
        popad
        ret

;=====================================================================
; img_sniff - decide what is inside the image just mapped, and where
;             its volume begins.  Fills imm_start / imm_secs /
;             imm_ptype, and sets IMF_ISO if the contents are read in
;             2048-byte blocks.  CF set with AX if it is nothing this
;             kernel can read.
;
; THE CONTENTS DECIDE, NOT THE EXTENSION.  An .ISO is an ISO9660 or
; UDF image because block 16 says so, and an .IMG is a FAT volume
; because sector 0 says so - and a file named the wrong way round
; still works, which is the point.  An extension is a hint from
; whoever named the file; a volume descriptor is a statement by
; whoever wrote it.
;
; The order is ISO, then a bare FAT volume, then a partitioned disk,
; and it is the order of how sure each test is.  CD001 sixteen blocks
; in is a five-byte signature at a fixed place and means one thing.  A
; BPB is three plausibility tests on fields that could be anything.  A
; partition table is two bytes of signature and four entries that a
; FAT boot sector's tail can resemble - so it is asked last, of a
; sector that has already failed to be a BPB.
;=====================================================================
img_sniff:
        pushad
        mov     esi,[imm_slot]
        mov     dword ptr [imm_start],0
        mov     eax,[esi+IM_SECS]
        mov     [imm_secs],eax
        mov     byte ptr [imm_ptype],0

;--- block 16: a volume descriptor? ----------------------------------
        mov     al,[imm_unit]
        mov     ebx,ISO_PVDBLK
        mov     ecx,1
        mov     edi,OFFSET img_sbuf
        call    img_block
        jc      ims_notiso              ; too short to hold one

        mov     edi,OFFSET img_sbuf + 1 ; the identifier, past the type
        mov     esi,OFFSET img_sig_cd
        call    ims_match5
        je      ims_isiso
        mov     edi,OFFSET img_sbuf + 1
        mov     esi,OFFSET img_sig_bea
        call    ims_match5
        je      ims_isiso
        mov     edi,OFFSET img_sbuf + 1
        mov     esi,OFFSET img_sig_n2
        call    ims_match5
        je      ims_isiso
        mov     edi,OFFSET img_sbuf + 1
        mov     esi,OFFSET img_sig_n3
        call    ims_match5
        jne     ims_notiso
ims_isiso:
        mov     esi,[imm_slot]
        or      byte ptr [esi+IM_FLAGS],IMF_ISO OR IMF_RO
        popad
        clc
        ret

;--- sector 0: a boot sector with a BPB? -----------------------------
ims_notiso:
        mov     al,[imm_unit]
        xor     ebx,ebx
        mov     cx,1
        mov     edi,OFFSET sec_buf
        mov     edx,IOS_RDBLK
        call    img_io
        jc      ims_bad
        mov     dword ptr [sec_lba],-1  ; sec_buf now holds someone else's
                                        ; sector: say so before leaving
        cmp     dword ptr [sec_buf+3],'AFXE'    ; an exFAT volume has no BPB
        jne     ims_notex                       ; for bpb_looks to find, and
        cmp     dword ptr [sec_buf+7],'   T'    ; is a volume all the same
        jne     ims_notex
        mov     byte ptr [imm_ptype],PT_EXFAT
        popad
        clc
        ret
ims_notex:
        call    bpb_looks
        jc      ims_mbr
        popad
        clc
        ret

;--- or a master boot record with a FAT partition in it? -------------
ims_mbr:
        cmp     word ptr [sec_buf+510],0AA55h
        jne     ims_bad
        mov     dword ptr [gpt_rdfn],OFFSET img_gptrd
        call    ims_gpt                 ; a GPT disk in a file (GPT.INC)
        jnc     ims_placed
        mov     esi,OFFSET sec_buf + 01BEh
        xor     ecx,ecx
ims_pscan:
        mov     al,[esi+PE_TYPE]
        call    part_isfat
        jnc     ims_pfat
        cmp     al,PT_EXFAT             ; ...or exFAT in an 07h one
        jne     ims_pnext
        push    edx
        mov     al,[imm_unit]
        mov     edx,dword ptr [esi+PE_START]
        call    part_exfat
        pop     edx
        jc      ims_pnext
ims_pfat:
        mov     eax,dword ptr [esi+PE_START]
        or      eax,eax
        jz      ims_pnext               ; a partition starting at the MBR
                                        ; is not a partition
        cmp     eax,[imm_secs]
        jae     ims_pnext               ; ...nor is one that starts past
                                        ; the end of the file
        mov     [imm_start],eax
        mov     al,[esi+PE_TYPE]
        mov     [imm_ptype],al
        mov     eax,dword ptr [esi+PE_SECS]
        mov     ebx,[imm_secs]
        sub     ebx,[imm_start]
        cmp     eax,ebx                 ; a length the file cannot honour
        jbe     ims_plen                ; is clamped, not believed
        mov     eax,ebx
ims_plen:
        mov     [imm_secs],eax
ims_placed:
        popad
        clc
        ret
ims_pnext:
        add     esi,PE_LEN
        inc     ecx
        cmp     ecx,PART_MAX
        jb      ims_pscan
ims_bad:
        mov     word ptr [imm_err],ERR_BADDATA  ; nothing recognisable in it
        popad
        mov     ax,[imm_err]
        stc
        ret

;--- ims_gpt - the MBR in sec_buf is a GUID partition table's protective
;--- one: the first volume in the table is the one mounted, as the first
;--- FAT partition of an MBR is.  CF set if there is no table that checks
;--- behind it, or nothing in it this system reads.
ims_gpt:
        call    mbr_hasgpt
        jc      igp_out
        pushad
        mov     al,[imm_unit]
        mov     ecx,[imm_secs]          ; the file's size: where the backup is
        call    gpt_open
        jc      igp_no
        xor     ebp,ebp
        call    gpt_next                ; -> AH = type, EDX = start, ECX
        jc      igp_no
        cmp     edx,[imm_secs]
        jae     igp_no
        mov     [imm_start],edx
        mov     [imm_ptype],ah
        mov     eax,[imm_secs]
        sub     eax,edx
        cmp     ecx,eax                 ; clamped to the file, as above
        jbe     igp_len
        mov     ecx,eax
igp_len:
        mov     [imm_secs],ecx
        popad
        clc
        ret
igp_no:
        popad
        stc
igp_out:
        ret

;--- img_gptrd - gpt_rdfn for the file being mounted: sector EBX of
;--- unit AL into EDI.  img_io keeps every register but AH.
img_gptrd:
        push    ecx
        push    edx
        mov     cx,1
        mov     edx,IOS_RDBLK
        call    img_io
        pop     edx
        pop     ecx
        ret

;--- ims_match5 - five bytes at EDI against five at ESI.  ZF on equal.
ims_match5:
        push    ecx
        push    esi
        push    edi
        mov     ecx,5
        repe    cmpsb
        pop     edi
        pop     esi
        pop     ecx
        ret

;=====================================================================
; img_umount - AL = a drive letter.  CF set with AX if it is not a
;              mounted image, or if something still has it open.
;
;              AH matters only for an image held in memory (IMF_MEM),
;              and says what to do with changes made to it:
;                0  refuse (ERR_ACCESS) if it has been written at all
;                1  throw them away
;                2  MOUNT has saved the copy IMEM_COPY took when the
;                   count was EDX - so refuse only if IM_GEN has moved
;                   on since, which is a write the save did not carry
;
; EVERYTHING THE VOLUME OWES GOES OUT FIRST.  The cache holds the
; image's blocks under the HOST's drive number, so they are not
; special and vol_syncdrv retires them the way it retires anybody's -
; but they have to be retired BEFORE the map that says where they go
; is given back to the arena, and that ordering is the whole of what
; this routine has to get right.
;=====================================================================
img_umount:
        pushad
        mov     [imu_mode],ah
        mov     [imu_gen],edx
        movzx   eax,al
        cmp     eax,MAX_DRIVES
        jae     imu_nodrive
        mov     [imu_letter],al
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      imu_nodrive
        test    byte ptr [ebx+DRV_FLAGS],DRVF_IMAGE
        jz      imu_nodrive
        mov     al,[ebx+DRV_BIOS]
        call    img_slot                ; -> ESI = the mount
        jc      imu_nodrive
        mov     [imu_slot],esi

        ; a file open on the volume would be reading a map that is
        ; about to stop existing
        mov     al,[imu_letter]
        call    img_anyopen
        jc      imu_busy

        ; FLUSH, THEN FORGET, THEN UNHOOK, THEN FREE, and the order is
        ; the whole of what this routine has to get right.  dos_commit
        ; is the flush, and it is the right one rather than a flush of
        ; this letter alone: the image volume's own FAT writes land in
        ; the cache under the HOST's drive number, so flushing the
        ; image without then committing the cache would leave them in
        ; memory with the map that says where they go about to be
        ; freed.  dos_commit does both layers in that order already.
        call    dos_commit

        ; A MEMORY IMAGE'S CHANGES ARE NOT LOST BY ACCIDENT.  The count
        ; is read after the commit, because the commit's own writes go
        ; into the block and are changes like any other.
        mov     esi,[imu_slot]
        test    byte ptr [esi+IM_FLAGS],IMF_MEM
        jz      imu_go
        mov     eax,[esi+IM_GEN]
        cmp     byte ptr [imu_mode],1
        je      imu_go                  ; thrown away, as asked
        cmp     byte ptr [imu_mode],2
        jne     imu_plain
        cmp     eax,[imu_gen]
        jne     imu_busy                ; written since MOUNT took its copy
        jmp     short imu_go
imu_plain:
        or      eax,eax
        jnz     imu_busy                ; written, and nobody saved it
imu_go:
        mov     al,[imu_letter]
        call    img_dropvol

        movzx   eax,byte ptr [imu_letter]
        call    drv_slot
        mov     byte ptr [ebx+DRV_FLAGS],0
        mov     byte ptr [ebx+DRV_BIOS],0
        mov     byte ptr [ebx+DRV_TYPE],0
        mov     dword ptr [ebx+DRV_START],0
        mov     dword ptr [ebx+DRV_SECS],0

        mov     esi,[imu_slot]
        call    img_unmap
        test    byte ptr [esi+IM_FLAGS],IMF_NET
        jz      imu_nonet
        call    img_netclose            ; last: the volume is gone already
imu_nonet:
        mov     byte ptr [esi+IM_FLAGS],0

        ; A LETTER THE DEFAULT DRIVE WAS STANDING ON is not left
        ; pointing at nothing: the shell would print a prompt for a
        ; volume that has gone.  Falling back to the boot drive is
        ; what DOS does when a drive is removed under it, and the boot
        ; drive is the letter shell_name carries - the shell is
        ; reloaded from it however far the current directory has
        ; wandered, which makes it the one letter always known good.
        mov     al,[imu_letter]
        cmp     al,[cur_drive]
        jne     imu_notcur
        mov     al,[shell_name]
        sub     al,'A'
        cmp     al,MAX_DRIVES
        jae     imu_notcur
        mov     [cur_drive],al
imu_notcur:
        popad
        clc
        ret
imu_busy:
        mov     word ptr [imm_err],ERR_ACCESS
        jmp     short imu_fail
imu_nodrive:
        mov     word ptr [imm_err],ERR_BADDRIVE
imu_fail:
        popad
        mov     ax,[imm_err]
        stc
        ret

;---------------------------------------------------------------------
; img_anyopen - AL = a drive letter.  CF set if any SFT entry names a
;               file on it.  The SFT records the drive an open file
;               lives on, which is exactly the question.
;---------------------------------------------------------------------
img_anyopen:
        pushad
        movzx   edx,al                  ; SFT_DRIVE is a DWORD
        mov     esi,OFFSET sft_tbl
        mov     ecx,MAX_HANDLES
iao_lp:
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      iao_next
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jnz     iao_next
        cmp     edx,[esi+SFT_DRIVE]
        je      iao_yes
iao_next:
        add     esi,SFT_LEN
        dec     ecx
        jnz     iao_lp
        popad
        clc
        ret
iao_yes:
        popad
        stc
        ret

;---------------------------------------------------------------------
; img_dropvol - AL = a letter.  Whatever that letter's volume state
;               remembers, it stops remembering - WITHOUT writing any
;               of it back, and without disturbing which volume was
;               loaded when the call was made.
;
; It is fF1_rem's discard block, applied to a letter that is about to
; change identity rather than to one that has been reformatted.  Both
; callers have already made sure nothing is owed: img_mount is giving
; the letter to a volume that did not exist a moment ago, and
; img_umount has just run dos_commit.
;---------------------------------------------------------------------
img_dropvol:
        pushad
        mov     ah,[cur_vol]            ; where to put things back
        call    vol_swap
        mov     byte ptr [fat_ok],0
        mov     byte ptr [fatsec_dirty],0
        mov     byte ptr [fat12_dirty],0
        mov     dword ptr [fatsec_lba],-1
        mov     dword ptr [sec_lba],-1
        mov     dword ptr [clusbuf_clus],-1
        mov     dword ptr [dc_curord],-1
        mov     dword ptr [cwd_cnt],0   ; a new volume starts at its root
        mov     dword ptr [cwd_clus],0
        mov     al,ah
        call    vol_swap
        popad
        ret

;=====================================================================
; img_report - ECX = 0..IMG_MAX-1, EDI -> a 100-byte buffer.
;              CF clear if that slot is mounted, and the buffer holds:
;
;                +00  db  the drive letter, 0 = A:
;                +01  db  IMF_* flags, and IMF_CHANGED for a memory
;                         image that has been written to
;                +02  db  the host volume's letter (FFh: none known)
;                +03  db  (pad)
;                +04  dd  the image's size in bytes
;                +08  dd  extents in its map
;                +0C  dd  where its volume starts, in sectors
;                +10  dd  and how long it is
;                +14  80 bytes  the file, ASCIIZ
;
; MOUNT with no arguments prints this, and it is a call rather than a
; pointer into kernel memory because a native program runs at ring 3
; and cannot read kernel memory even to describe it - the same
; argument F1h AL=9 and AL=0Ah make.
;=====================================================================
img_report:
        pushad
        cmp     ecx,IMG_MAX
        jae     irp_no
        imul    esi,ecx,IM_LEN
        add     esi,OFFSET img_tbl
        test    byte ptr [esi+IM_FLAGS],IMF_USED
        jz      irp_no
        mov     al,[esi+IM_LETTER]
        mov     [edi+0],al
        mov     al,[esi+IM_FLAGS]
        test    al,IMF_MEM              ; IMF_CHANGED is worked out, not
        jz      irp_flags               ; kept: it is "IM_GEN is not 0"
        cmp     dword ptr [esi+IM_GEN],0
        je      irp_flags
        or      al,IMF_CHANGED
irp_flags:
        mov     [edi+1],al
        mov     al,[esi+IM_HOSTVOL]
        mov     [edi+2],al
        mov     byte ptr [edi+3],0
        mov     eax,[esi+IM_SIZE]
        mov     [edi+4],eax
        mov     eax,[esi+IM_NEXT]
        mov     [edi+8],eax

        ; where the volume starts and how long it is come from the
        ; DRIVE table rather than from here: those are facts about the
        ; letter, and a partitioned image has them different from the
        ; file's own length
        movzx   eax,byte ptr [esi+IM_LETTER]
        push    esi
        call    drv_slot
        mov     eax,[ebx+DRV_START]
        mov     [edi+0Ch],eax
        mov     eax,[ebx+DRV_SECS]
        mov     [edi+10h],eax
        pop     esi

        add     esi,IM_PATH
        add     edi,14h
        mov     ecx,79                  ; 79 and not 80: the NUL written
                                        ; when the count runs out has to
                                        ; land INSIDE the caller's hundred
                                        ; bytes, and eighty of them plus a
                                        ; terminator is eighty-one
irp_cp:
        mov     al,[esi]
        mov     [edi],al
        or      al,al
        jz      irp_done
        inc     esi
        inc     edi
        dec     ecx
        jnz     irp_cp
        mov     byte ptr [edi],0
irp_done:
        popad
        clc
        ret
irp_no:
        popad
        stc
        ret

;=====================================================================
; img_memload - give a letter to an image held in memory.  F1h AL=2Bh,
;               IMEM_LOAD.
;
;   ESI -> the image's bytes   ECX = how many (fF1_imem checked them)
;   EDX -> ASCIIZ, the file they came from: MOUNT prints it, a second
;          load of the same file is refused by it, and MOUNT /U saves
;          the image back to it
;   AL  = the letter (0 = A:), or FFh for the next free one
;   AH  = IMF_RO to mount it read-only, else 0
;
;   CF clear: AL = the letter it took.  CF set: AX =
;               3  ERR_NOPATH    no path, or one that will not fit in
;                                IM_PATH whole
;               5  ERR_ACCESS    less than a sector, or more than
;                                IMG_MEM_MAX
;               8  ERR_NOMEM     the arena has no block that size
;              13  ERR_BADDATA   nothing in it this kernel can read
;              15  ERR_BADDRIVE  no slot left, or the letter is taken
;              80  ERR_EXISTS    that file already has a letter
;
; THE BYTES ARE COPIED, NOT BORROWED.  They are in MOUNT's slot, which
; goes when MOUNT ends; the copy is a block of the kernel's (cur_own 0,
; img_map's reason) bought last fit, from the top of the arena and out
; of a growing slot's way (CACHE.INC's).  The sniff and the letter are
; img_mount's own, so a loaded image is told apart and mounted exactly
; as a mapped one is.
;=====================================================================
img_memload:
        pushad
        mov     [imm_letter],al
        mov     [imm_flags],ah
        mov     [imld_src],esi
        mov     [imld_len],ecx
        mov     [imm_path],edx

        cmp     ecx,512
        jb      imld_denied             ; not one sector of anything
        cmp     ecx,IMG_MEM_MAX
        ja      imld_denied             ; bigger than a diskette

        ; THE PATH HAS TO FIT WHOLE.  MOUNT /U writes the image back to
        ; it, and a name cut short is somebody else's file.
        xor     ecx,ecx
imld_meas:
        cmp     byte ptr [edx+ecx],0
        je      imld_measd
        inc     ecx
        cmp     ecx,80
        jb      imld_meas
        jmp     imld_nopath             ; 80 and no NUL yet: too long
imld_measd:
        or      ecx,ecx
        jz      imld_nopath
        mov     [imld_plen],ecx

        call    imld_dup                ; one file, one letter
        jc      imld_already
        call    img_claim
        jc      imm_fail

        push    dword ptr [cur_own]
        push    dword ptr [alloc_strat]
        mov     dword ptr [cur_own],0
        mov     dword ptr [alloc_strat],AF_LAST
        mov     ebx,[imld_len]
        call    arena_alloc
        pop     dword ptr [alloc_strat]
        pop     dword ptr [cur_own]
        jc      imld_nomem
        mov     edi,eax
        mov     esi,[imld_src]
        mov     ecx,[imld_len]
        cld
        rep     movsb

        ; THE SLOT.  IM_HOSTVOL is the letter the path starts with, for
        ; MOUNT's listing and DEFRAG's walk to the medium underneath;
        ; no sector is ever read from it.  IM_CLUS 0 and IMF_MEM keep
        ; img_locked away from the file.
        mov     esi,[imm_slot]
        mov     [esi+IM_MAP],eax
        mov     edx,[imm_path]
        mov     al,0FFh
        cmp     byte ptr [edx+1],':'
        jne     imld_host
        mov     al,[edx]
        or      al,20h
        sub     al,'a'
        cmp     al,MAX_DRIVES
        jb      imld_host
        mov     al,0FFh
imld_host:
        mov     [esi+IM_HOSTVOL],al
        mov     byte ptr [esi+IM_HOSTDRV],0FFh
        mov     dword ptr [esi+IM_CLUS],0
        mov     eax,[imld_len]
        mov     [esi+IM_SIZE],eax
        shr     eax,9                   ; whole sectors, as img_mount
        mov     [esi+IM_SECS],eax
        mov     dword ptr [esi+IM_NEXT],0
        mov     dword ptr [esi+IM_HINT],0
        mov     dword ptr [esi+IM_GEN],0
        mov     al,[imm_flags]
        or      al,IMF_USED OR IMF_MEM
        mov     [esi+IM_FLAGS],al
        mov     al,[imm_letter]
        mov     [esi+IM_LETTER],al
        lea     edi,[esi+IM_PATH]
        mov     esi,[imm_path]
        mov     ecx,[imld_plen]
        inc     ecx                     ; and its NUL: measured under 80
        rep     movsb

        call    img_sniff               ; what is in it, through iib_mem
        jc      imm_unwind2             ; gives the block back
        call    img_assign
        popad
        mov     al,[imm_letter]
        clc
        ret
imld_denied:
        mov     ax,ERR_ACCESS
        jmp     imm_fail
imld_nopath:
        mov     ax,ERR_NOPATH
        jmp     imm_fail
imld_already:
        mov     ax,ERR_EXISTS
        jmp     imm_fail
imld_nomem:
        mov     ax,ERR_NOMEM
        jmp     imm_fail

;--- imld_dup - CF if [imm_path] is already the file of a memory image
;--- or of one on a share - the two kinds a name is all there is to go
;--- by.  The case of letters does not count: TRUENAME gives upper
;--- case, a caller that did not ask it may not.
imld_dup:
        pushad
        mov     ebx,OFFSET img_tbl
        mov     ecx,IMG_MAX
imdu_slot:
        test    byte ptr [ebx+IM_FLAGS],IMF_USED
        jz      imdu_next
        test    byte ptr [ebx+IM_FLAGS],IMF_MEM OR IMF_NET
        jz      imdu_next
        lea     esi,[ebx+IM_PATH]
        mov     edi,[imm_path]
imdu_ch:
        mov     al,[esi]
        cmp     al,'a'
        jb      imdu_up1
        cmp     al,'z'
        ja      imdu_up1
        sub     al,20h
imdu_up1:
        mov     dl,[edi]
        cmp     dl,'a'
        jb      imdu_up2
        cmp     dl,'z'
        ja      imdu_up2
        sub     dl,20h
imdu_up2:
        cmp     al,dl
        jne     imdu_next
        or      al,al
        jz      imdu_yes
        inc     esi
        inc     edi
        jmp     short imdu_ch
imdu_next:
        add     ebx,IM_LEN
        dec     ecx
        jnz     imdu_slot
        popad
        clc
        ret
imdu_yes:
        popad
        stc
        ret

;=====================================================================
; img_memcopy - an image held in memory, copied back out for MOUNT /U
;               to save.  F1h AL=2Bh, IMEM_COPY.
;
;   AL = the letter   EDI -> the caller's buffer   ECX = its room
;        (fF1_imem checked EDI and ECX)
;
;   CF clear: ECX = the image's length, all of it copied, and EDX =
;             IM_GEN when the copy was taken - the number F1h AL=10h
;             is handed back with BH=2 to say the copy was saved.
;   CF set:   AX = ERR_BADDRIVE  not an image held in memory
;                  ERR_ACCESS    a file on it is open
;                  ERR_BADLEN    the room is too small; ECX = the
;                                length needed
;
; EVERYTHING OWED GOES IN FIRST.  The volume's FAT and directory
; sectors may still be in the FAT layer's buffers; dos_commit writes
; them, they land in the block like any other write, and the count
; handed back includes them.  An open file is refused rather than
; copied half-written: its directory entry is settled when it closes.
;=====================================================================
img_memcopy:
        pushad
        mov     [imcp_dst],edi
        mov     [imcp_room],ecx
        movzx   eax,al
        cmp     eax,MAX_DRIVES
        jae     imcp_nodrive
        mov     [imcp_letter],al
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      imcp_nodrive
        test    byte ptr [ebx+DRV_FLAGS],DRVF_IMAGE
        jz      imcp_nodrive
        mov     al,[ebx+DRV_BIOS]
        call    img_slot                ; -> ESI = the mount
        jc      imcp_nodrive
        test    byte ptr [esi+IM_FLAGS],IMF_MEM
        jz      imcp_nodrive
        mov     [imcp_slot],esi
        mov     eax,[esi+IM_SIZE]
        mov     [imcp_len],eax
        cmp     eax,[imcp_room]
        ja      imcp_short

        mov     al,[imcp_letter]
        call    img_anyopen
        jc      imcp_busy
        call    dos_commit

        mov     esi,[imcp_slot]
        mov     eax,[esi+IM_GEN]
        mov     [imcp_gen],eax
        mov     ecx,[esi+IM_SIZE]
        mov     esi,[esi+IM_MAP]
        mov     edi,[imcp_dst]
        cld
        rep     movsb
        popad
        mov     ecx,[imcp_len]
        mov     edx,[imcp_gen]
        clc
        ret
imcp_short:
        popad
        mov     ecx,[imcp_len]
        mov     ax,ERR_BADLEN
        stc
        ret
imcp_busy:
        popad
        mov     ax,ERR_ACCESS
        stc
        ret
imcp_nodrive:
        popad
        mov     ax,ERR_BADDRIVE
        stc
        ret

;=====================================================================
; img_netopen - the file at [imm_path] is on the network drive that
;               resolve_core just selected ([cur_vol]).  Open it through
;               the redirector and fill the slot at [imm_slot] for
;               IMF_NET.  CF with AX = why not, and nothing left open.
;               Runs inside img_mount, so [imm_flags] may say IMF_RO.
;
; READ AND WRITE IF THE SERVER WILL, READ-ONLY IF IT WILL NOT.  The
; driver says "no such file" for any open it is refused, so a refused
; read-and-write open is asked again for reading before it is believed:
; a read-only file or share mounts read-only, the way a read-only file
; on C: does, and a file that is not there fails both.
;
; net_req IS FREE HERE: this is F1h's own call, with no file operation
; of anybody's in the middle of it, so the mount uses NETFS's routines.
; The transfers later use img_nreq, which nothing else touches.
;=====================================================================
img_netopen:
        pushad
        mov     al,[cur_vol]
        mov     [imn_drive],al

        ; THE NAME AS THE SERVER SPELLS IT, for the listing and so that
        ; one file has one letter.  The 8.3 form, as 60h gives.
        mov     ebx,REDIR_CANON
        xor     edx,edx
        mov     esi,[imm_path]
        call    net_text                ; -> net_req's NR_PATH, or CF/AX
        jc      imn_fail
        call    imn_keepname            ; -> IM_PATH, [imn_whole]
        cmp     byte ptr [imn_whole],0
        je      imn_open1               ; a name cut short proves nothing
        push    dword ptr [imm_path]
        mov     eax,[imm_slot]
        add     eax,IM_PATH
        mov     [imm_path],eax          ; what imld_dup compares
        call    imld_dup
        pop     dword ptr [imm_path]
        jc      imn_already

imn_open1:
        mov     dword ptr [imn_access],2        ; read and write
        test    byte ptr [imm_flags],IMF_RO
        jz      imn_open
        mov     dword ptr [imn_access],0        ; /R: reading only
imn_open:
        mov     al,[imn_drive]
        mov     ebx,REDIR_OPEN
        call    net_req_begin
        mov     esi,[imm_path]
        call    net_setpath
        mov     eax,[imn_access]
        mov     [net_req+NR_COUNT],eax          ; NR_SPARE 0: an existing file
        call    net_fire
        jc      imn_general
        cmp     dword ptr [net_req+NR_STATUS],0
        je      imn_opened
        cmp     dword ptr [imn_access],0
        je      imn_status              ; refused even for reading
        mov     dword ptr [imn_access],0
        or      byte ptr [imm_flags],IMF_RO
        jmp     short imn_open

imn_opened:
        mov     esi,[imm_slot]
        mov     eax,[net_req+NR_HANDLE]
        mov     [esi+IM_FILE],eax
        mov     al,[imn_drive]
        mov     [esi+IM_HOSTVOL],al
        mov     byte ptr [esi+IM_HOSTDRV],0FFh
        mov     dword ptr [esi+IM_CLUS],0
        mov     dword ptr [esi+IM_NEXT],0
        mov     dword ptr [esi+IM_MAP],0
        mov     dword ptr [esi+IM_HINT],0
        mov     dword ptr [esi+IM_GEN],0
        test    byte ptr [net_req+NR_ATTR],ATTR_DIR
        jnz     imn_closedeny           ; a directory is not one file
        test    byte ptr [net_req+NR_ATTR],ATTR_READONLY
        jz      imn_size
        or      byte ptr [imm_flags],IMF_RO
imn_size:
        ; THE SIZE IS 64 BITS ON A SHARE.  IM_SIZE, which only the report
        ; reads, stops at 4GB; IM_SECS is the drive table's 32 bits, which
        ; is two terabytes.
        mov     eax,[net_req+NR_SIZELO]
        mov     edx,[net_req+NR_SIZEHI]
        or      edx,edx
        jnz     imn_big
        cmp     eax,512
        jb      imn_closedeny           ; not one sector of anything
        mov     [esi+IM_SIZE],eax
        shr     eax,9                   ; whole sectors, as img_mount
        jmp     short imn_secs
imn_big:
        mov     dword ptr [esi+IM_SIZE],0FFFFFFFFh
        shrd    eax,edx,9
        shr     edx,9
        jz      imn_secs
        mov     eax,0FFFFFFFFh          ; past 2TB: as far as a letter goes
imn_secs:
        mov     [esi+IM_SECS],eax
        mov     al,[imm_flags]
        or      al,IMF_USED OR IMF_NET
        mov     [esi+IM_FLAGS],al
        mov     al,[imm_letter]
        mov     [esi+IM_LETTER],al
        popad
        clc
        ret

imn_closedeny:
        call    img_netclose            ; ESI = the slot, IM_FILE set
        mov     ax,ERR_ACCESS
        jmp     short imn_fail
imn_general:
        mov     ax,ERR_GENERAL
        jmp     short imn_fail
imn_status:
        mov     ax,word ptr [net_req+NR_STATUS]
        jmp     short imn_fail
imn_already:
        mov     ax,ERR_EXISTS
imn_fail:
        mov     [imm_err],ax
        popad
        mov     ax,[imm_err]
        stc
        ret

;--- imn_keepname - IM_PATH = "X:\" and net_req's NR_PATH, the server's
;--- share-relative name with no leading backslash.  When that will not
;--- fit in IM_PATH's 80 bytes, "X:\" and its last component, and
;--- [imn_whole] = 0 so nothing is concluded from a partial name.
imn_keepname:
        pushad
        mov     edi,[imm_slot]
        add     edi,IM_PATH
        mov     al,[imn_drive]
        add     al,'A'
        mov     [edi],al
        mov     byte ptr [edi+1],':'
        mov     byte ptr [edi+2],'\'
        add     edi,3
        mov     esi,OFFSET net_req + NR_PATH
        xor     ecx,ecx
ikn_len:
        cmp     byte ptr [esi+ecx],0
        je      ikn_measd
        inc     ecx
        cmp     ecx,259                 ; NR_PATH is 260 with its NUL
        jb      ikn_len
ikn_measd:
        mov     byte ptr [imn_whole],1
        cmp     ecx,80 - 3 - 1
        jbe     ikn_copy
        mov     byte ptr [imn_whole],0
        lea     edx,[esi+ecx]           ; the end; back to the last '\'
ikn_back:
        cmp     edx,esi
        je      ikn_tail
        cmp     byte ptr [edx-1],'\'
        je      ikn_tail
        dec     edx
        jmp     short ikn_back
ikn_tail:
        add     ecx,esi
        sub     ecx,edx                 ; ECX = the component's length
        mov     esi,edx
        cmp     ecx,80 - 3 - 1
        jbe     ikn_copy
        mov     ecx,80 - 3 - 1          ; an 8.3 name never is
ikn_copy:
        cld
        rep     movsb
        mov     byte ptr [edi],0
        popad
        ret

;---------------------------------------------------------------------
; img_netclose - ESI = an IMF_NET mount (or the slot img_netopen was
;                filling): close its file on the server.  Best effort,
;                as net_close's is: a share that is gone has nothing
;                left to close.  Everything preserved.
;---------------------------------------------------------------------
img_netclose:
        pushad
        call    img_nclear
        mov     dword ptr [img_nreq+NR_OP],REDIR_CLOSE
        movzx   eax,byte ptr [esi+IM_HOSTVOL]
        mov     [img_nreq+NR_DRIVE],eax
        mov     eax,[esi+IM_FILE]
        mov     [img_nreq+NR_HANDLE],eax
        call    img_nfire
        popad
        ret

;---------------------------------------------------------------------
; img_netbusy - AL = a drive letter.  CF set if an image is mounted
;               from a file on it: NET USE /DELETE asks, because taking
;               the share away would leave the image reading through a
;               handle on a session that no longer exists.  Everything
;               preserved.
;---------------------------------------------------------------------
img_netbusy:
        push    ecx
        push    esi
        mov     esi,OFFSET img_tbl
        mov     ecx,IMG_MAX
inb_lp:
        test    byte ptr [esi+IM_FLAGS],IMF_USED
        jz      inb_next
        test    byte ptr [esi+IM_FLAGS],IMF_NET
        jz      inb_next
        cmp     al,[esi+IM_HOSTVOL]
        je      inb_yes
inb_next:
        add     esi,IM_LEN
        dec     ecx
        jnz     inb_lp
        pop     esi
        pop     ecx
        clc
        ret
inb_yes:
        pop     esi
        pop     ecx
        stc
        ret

;---------------------------------------------------------------------
; img_nclear - img_nreq all zero.  Everything preserved.
; img_nfire  - hand img_nreq to the redirector, as net_fire hands
;              net_req: interrupts on for the call (the driver waits on
;              the card and the clock), the caller's flags back after.
;              CF if there is no redirector or it would not take the
;              call; otherwise NR_STATUS says how it went.
;
; THE CACHE IS HELD BUSY FOR THE CALL.  A transfer comes here from
; inside dio_try, whose statics hold it, and the driver may allocate
; memory: an arena with none to spare asks cache_reclaim, which writes
; dirty blocks out through disk_write - dio_try again, over those
; statics, which CACHE.INC says must never happen.  cache_busy is the
; flag cache_reclaim already obeys ("already inside ourselves"), so the
; driver's allocation is refused instead of paid for with a flush.
;---------------------------------------------------------------------
img_nclear:
        push    eax
        push    ecx
        push    edi
        mov     edi,OFFSET img_nreq
        xor     eax,eax
        mov     ecx,NR_SIZE / 4
        cld
        rep     stosd
        pop     edi
        pop     ecx
        pop     eax
        ret

img_nfire:
        pushad
        pushfd
        mov     al,[cache_busy]
        mov     [inf_busy],al
        mov     byte ptr [cache_busy],1
        sti
        mov     eax,NDTF_REDIR
        xor     edx,edx
        call    nd_find_flag            ; ESI -> the redirector's row
        jc      inf_fail
        mov     ebx,[img_nreq+NR_OP]
        mov     edi,OFFSET img_nreq
        mov     ecx,NR_SIZE
        call    nd_call_row
        jc      inf_fail
        mov     al,[inf_busy]
        mov     [cache_busy],al
        popfd
        popad
        clc
        ret
inf_fail:
        mov     al,[inf_busy]
        mov     [cache_busy],al
        popfd
        popad
        stc
        ret

;=====================================================================
; State
;=====================================================================
img_tbl     db  (IMG_MAX * IM_LEN) dup(0)

; img_mount's working set.  Not registers because the mount walks a
; filesystem in the middle of itself and pushad is eight deep already.
imm_letter  db  0
imm_flags   db  0
imm_unit    db  0
imm_ptype   db  0
imm_path    dd  0
imm_slot    dd  0
imm_clus    dd  0
imm_size    dd  0
imm_start   dd  0
imm_secs    dd  0
imm_err     dw  0
            dw  0

imu_letter  db  0
imu_mode    db  0                       ; AH: 0 keep, 1 discard, 2 saved
            db  0,0
imu_slot    dd  0
imu_gen     dd  0                       ; EDX: the count MOUNT saved at

; img_memload's and img_memcopy's
imld_src    dd  0
imld_len    dd  0
imld_plen   dd  0
imcp_dst    dd  0
imcp_room   dd  0
imcp_slot   dd  0
imcp_len    dd  0
imcp_gen    dd  0
imcp_letter db  0
            db  0,0,0

; img_netopen's, and the redirector request every transfer of an IMF_NET
; mount is made with - its own, not net_req (see the file's header)
imn_drive   db  0
imn_whole   db  0
inf_busy    db  0                       ; cache_busy as img_nfire found it
            db  0
imn_access  dd  0
img_nreq    dd  NR_SIZE / 4 dup(0)      ; dwords: wasm types every
                                        ; [img_nreq+NR_x] by this line

; img_walk's, and the chain walk is a loop of its own inside a pushad
imp_count   db  0
            db  0,0,0
imp_runs    dd  0
imp_cap     dd  0                       ; runs the map has room for
imw_clus    dd  0
imw_prev    dd  0
imw_lba     dd  0
imw_take    dd  0
imw_left    dd  0
imw_isec    dd  0
imw_host    dd  0
imw_len     dd  0

; img_io's, for the two things a wrapper must carry across its body
ii_thru     db  0
ii_ah       db  0
            db  0,0
ii_lba      dd  0
ii_take     dd  0

img_sig_cd  db  'CD001'
img_sig_bea db  'BEA01'
img_sig_n2  db  'NSR02'
img_sig_n3  db  'NSR03'
            db  0,0,0

; The sniff's own buffer.  Its own, rather than sec_buf, because the
; sniff runs while the HOST volume is selected and in the middle of a
; directory search - sec_buf is that search's sector.  (Sector 0 does
; go through sec_buf, because bpb_looks reads it there and because by
; then the search is finished with; sec_lba is invalidated on the spot
; so nothing believes what is left behind.)
;
; Below the arena and not in the file - see KBSS_BASE in PMDOS.INC.
_DOS    ENDS
_KBSS   SEGMENT
img_sbuf    db  2048 dup(?)
IF ($ - kbss_start) AND 3
        .ERR    what follows img_sbuf in _KBSS is not on a dword
ENDIF
_KBSS   ENDS
_DOS    SEGMENT
