;=====================================================================
; MSCDEX.INC - the INT 2Fh AH=15h API, answered by the kernel.
;
; THERE IS NO MSCDEX.EXE HERE AND THERE SHOULD NOT BE.  MSCDEX is a
; REDIRECTOR CLIENT: it exists because DOS had no way to put a
; non-FAT filesystem behind a drive letter, so the letter had to be
; faked by the network layer.  This kernel's founding idea is that a
; letter is a VOLUME and vol_fs says which filesystem is behind it, so
; the CD already has a real drive letter with no redirector anywhere.
; Writing an MSCDEX.EXE would mean building the redirector interface
; we deliberately do not have, in order to route around machinery that
; already works.
;
; WHAT PROGRAMS NEED IS THE API, NOT THE PROGRAM.  A DOS-box game or
; installer does not care how the letter came to exist; it asks INT
; 2Fh AX=1500h whether a CD driver is loaded and which letters it
; owns, and if nobody answers it concludes the machine has no CD-ROM
; and refuses to install.  So the calls are answered here, beside the
; XMS and DPMI multiplex handlers, out of the volume table the scan
; already built.
;
; AND IT STOPS SOMEBODY LOADING A SECOND ONE.  MSCDEX's own start-up
; does the 1500h check and refuses to install twice.  Until this file
; existed, MSCDEX.EXE in an AUTOEXEC.BAT would run, look for a block
; driver by the name in its /D: switch, not find one - there is no CD
; driver in the device chain, the letter comes from drv_addcds - and
; print "device driver not found" at somebody who had every reason to
; think that was the way to do it.
;
; WHERE THE LINE IS DRAWN, and it is drawn on purpose.
;
;   ANSWERED: everything that is a QUESTION ABOUT THE DRIVES or a
;   READ OF THE DISC - how many, which letters, is this one a CD,
;   what version, the volume descriptors, and absolute sector reads.
;   All of it comes out of state this kernel already keeps.
;
;   HANDED TO THE DEVICE: 1501h (the device driver header list) and
;   1510h (send a device request).  Both of those hand the caller a
;   DEVICE DRIVER to talk to, and for one release there was not one -
;   so both were chained, on the argument that a synthetic header is
;   worse than silence: it invites a program to CALL a strategy and
;   interrupt routine that do not exist, and a request packet that is
;   never marked done hangs the caller in a wait loop rather than
;   failing.  That argument was right, and the answer it pointed at
;   was a REAL character device named MSCD001 served by the kernel.
;   MSCD001.INC is that device, and CD AUDIO is what made it
;   necessary rather than merely tidy: every audio call a program
;   makes - play, stop, resume, where is the head, what is on this
;   disc - arrives as a 1510h request aimed at the driver, so the
;   audio API and the device interface are the same thing.
;
;   ANSWERED THROUGH THE READER: 150Fh, get directory entry.  It was
;   chained for a release, and the reason given then still holds - it
;   is a FILESYSTEM question, and a second reader of its own is how
;   two answers about one disc start to disagree.  What changed is
;   the implementation rather than the argument: the path is resolved
;   by the same resolve_parent and fat_search that 3Dh and 4Eh use,
;   on the same mounted volume, and the record it hands back is the
;   one THAT walk kept.  There is still one reader.  This call is a
;   second SHAPE for what it found.
;
; IF THERE ARE NO CD LETTERS WE ARE NOT MSCDEX, and every call chains.
; That is one test at the top and it is the honest answer: a machine
; with no CD-ROM has no CD-ROM extensions on it either.
;
; Included by DOSMAIN.ASM after ISO9660.INC.
;=====================================================================

;--- the errors these calls return, which are DOS's own -------------
CDX_ERR_DRIVE   EQU     15              ; invalid drive
CDX_ERR_NOTRDY  EQU     21              ; drive not ready
CDX_ERR_FUNC    EQU     1               ; invalid function
CDX_ERR_NOFILE  EQU     2               ; no such file
CDX_ERR_NOPATH  EQU     3               ; ...or nothing to hold one

; 150Fh's directory entry, which is MSCDEX's own shape and not the
; disc's - see the comment at cdx_f0F, and the offsets there.
CDX_DE_NAMELEN  EQU     17h
CDX_DE_NAME     EQU     18h
CDX_DE_NAMEMAX  EQU     32              ; what fits before the version
CDX_DE_VER      EQU     38h
CDX_DE_SYSLEN   EQU     3Ah
CDX_DE_SYS      EQU     3Bh             ; ...and the fixed part ends here
CDX_DE_MAX      EQU     255             ; the buffer the call promises

CDX_PATHMAX     EQU     128             ; the longest path this answers
                                        ; about, which is MSCDEX's own
                                        ; limit and more than ISO9660's
                                        ; eight levels can spell

CDX_MAXBLK      EQU     32              ; 64K of blocks, which is all
                                        ; an ES:BX buffer can hold
                                        ; before BX wraps - so a
                                        ; bigger count is a caller
                                        ; mistake and not a request

CDX_VER_MAJOR   EQU     2               ; the version reported by 150Ch.
CDX_VER_MINOR   EQU     25              ; 2.25 is the last MSCDEX and the
                                        ; one every program was written
                                        ; against

;=====================================================================
; cdx_int2F - CF clear if the call was ours, CF set to chain it on.
;
; Called from vdm_i2F.  Everything here reads and writes vdm_ctx: the
; caller is a 16-bit guest and its registers are the halves.
;=====================================================================
cdx_int2F:
        mov     ax,word ptr [vdm_ctx+V86C_EAX]
        cmp     ah,15h
        jne     cdx_notus

        ; No CD letters, no CD extensions.  Asked before the
        ; subfunction is even looked at, because the answer is the
        ; same for all of them.
        call    cdx_count                       ; -> ECX, EBX
        or      ecx,ecx
        jz      cdx_notus

        ; THE GUEST'S CARRY IS CLEARED HERE, not in each answer.
        ; This family reports failure the way a DOS call does, in
        ; CF, and a subfunction that succeeds without clearing it
        ; hands back whatever flag the caller arrived carrying.
        ; Doing it once, before the dispatch, is the only place a
        ; subfunction added later cannot forget it.
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF

        cmp     al,00h
        je      cdx_f00
        cmp     al,02h
        je      cdx_f02
        cmp     al,03h
        je      cdx_f03
        cmp     al,04h
        je      cdx_f04
        cmp     al,05h
        je      cdx_f05
        cmp     al,06h
        je      cdx_fnul
        cmp     al,07h
        je      cdx_fnul
        cmp     al,08h
        je      cdx_f08
        cmp     al,09h
        je      cdx_f09
        cmp     al,0Bh
        je      cdx_f0B
        cmp     al,0Ch
        je      cdx_f0C
        cmp     al,0Dh
        je      cdx_f0D
        cmp     al,0Fh
        je      cdx_f0F
        cmp     al,01h                  ; the device driver list, and
        je      cdd_f01                 ; sending that device a request.
        cmp     al,10h                  ; Both are answered by MSCD001.INC
        je      cdd_f10                 ; now that there is a device
cdx_notus:
        stc
        ret

;=====================================================================
; 00h - how many CD-ROM drive letters, and where they start.
;
;   -> BX = how many, CX = the first (0 = A)
;
; THE INSTALLATION CHECK IS THIS CALL, and the convention is worth
; stating because it is what made the machine answer correctly by
; accident before today: a program sets BX to zero, calls, and reads
; "installed" from BX having CHANGED.  The IRET stub on 2Fh left it
; zero, so every guest correctly concluded there was no CD driver.
; The answer was right and the reason was luck.
;=====================================================================
cdx_f00:
        mov     word ptr [vdm_ctx+V86C_EBX],cx
        mov     word ptr [vdm_ctx+V86C_ECX],bx
        clc
        ret

;=====================================================================
; 02h, 03h, 04h - the copyright, abstract and bibliographic file
;                 names out of the primary volume descriptor.
;
;   CX = drive, ES:BX -> 38 bytes
;
; Three calls, one routine, because they are the same 37-byte field
; three times over at three offsets - and the volume descriptor is one
; read whichever is wanted.
;=====================================================================
cdx_f02:
        mov     edx,ISO_PVD_COPYR
        jmp     short cdx_fname
cdx_f03:
        mov     edx,ISO_PVD_ABSTR
        jmp     short cdx_fname
cdx_f04:
        mov     edx,ISO_PVD_BIBLIO
cdx_fname:
        push    edx
        call    cdx_unit                        ; CX -> AL, CF if not a CD
        pop     edx
        jc      cdx_baddrv
        push    edx
        call    cdx_readpvd                     ; the descriptor into cdx_buf
        pop     edx
        jc      cdx_notready

        call    cdx_esbx                        ; -> EDI, flat
        mov     eax,edi
        mov     ecx,38
        call    buf_ok
        jc      cdx_badptr

        mov     esi,OFFSET cdx_buf
        add     esi,edx
        mov     ecx,ISO_PVD_IDLEN
        rep     movsb
        mov     byte ptr [edi],0                ; the 38th byte is the
        clc                                     ; terminator, and the field
        ret                                     ; itself is space-padded

;=====================================================================
; 05h - read the volume table of contents.
;
;   CX = drive, DX = which descriptor (0 = the one at block 16),
;   ES:BX -> 2048 bytes
;   -> AX = the descriptor's type byte
;
; It is a raw block read of the descriptor set and nothing more, which
; is what MSCDEX's own answer was: the caller is expected to know the
; layout of what it asked for.
;=====================================================================
cdx_f05:
        call    cdx_unit
        jc      cdx_baddrv
        mov     [cdx_dev],al

        call    cdx_esbx
        mov     eax,edi
        mov     ecx,ISO_BLKSIZE
        call    buf_ok
        jc      cdx_badptr

        movzx   ebx,word ptr [vdm_ctx+V86C_EDX]
        add     ebx,ISO_PVDBLK
        push    ebx
        mov     al,[cdx_dev]
        mov     edi,OFFSET cdx_buf              ; scratch: the guest's
        call    iso_findbase                    ; buffer is wanted intact
        pop     ebx
        add     ebx,eax                         ; the descriptor set of the
                                                ; session that is current
        call    cdx_esbx                        ; EDI again, which the
                                                ; scratch above took
        mov     al,[cdx_dev]
        mov     ecx,1
        call    dword ptr ds:[IOS_CDREAD]
        jc      cdx_notready

        call    cdx_esbx
        movzx   eax,byte ptr [edi+ISO_VD_TYPE]
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        clc
        ret

;=====================================================================
; 08h - absolute sector read.
;
;   CX = drive, SI:DI = the first block (SI is the high half),
;   DX = how many, ES:BX -> DX * 2048 bytes
;
; THE COUNT IS CHECKED AGAINST THE BUFFER AND NOT AGAINST THE DISC,
; which is buf_ok's whole lesson from another direction: the guest is
; naming a length, and a guest that names a big one while pointing
; somewhere legal is the case that gets through a check on the
; pointer alone.
;=====================================================================
cdx_f08:
        call    cdx_unit
        jc      cdx_baddrv
        mov     [cdx_dev],al

        movzx   ecx,word ptr [vdm_ctx+V86C_EDX] ; how many blocks
        or      ecx,ecx
        jz      cdx_ok                          ; nothing asked for
        cmp     ecx,CDX_MAXBLK
        ja      cdx_badptr
        mov     [cdx_cnt],ecx

        call    cdx_esbx
        mov     eax,edi
        mov     ecx,[cdx_cnt]
        shl     ecx,11                          ; blocks -> bytes, and it
        call    buf_ok                          ; cannot overflow: CDX_MAXBLK
        jc      cdx_badptr                      ; bounded it two lines up

        movzx   ebx,word ptr [vdm_ctx+V86C_ESI]
        shl     ebx,16
        movzx   eax,word ptr [vdm_ctx+V86C_EDI]
        or      ebx,eax                         ; SI:DI = the block number
        mov     al,[cdx_dev]
        mov     ecx,[cdx_cnt]
        call    dword ptr ds:[IOS_CDREAD]
        jc      cdx_notready
cdx_ok:
        clc
        ret

;=====================================================================
; 09h - absolute sector WRITE, and the answer is no.
;
; Real MSCDEX answered this way too on every drive anybody ever had:
; the call exists for writable media the specification anticipated and
; the hardware of the day did not have.  Refusing it is not a
; limitation of this implementation.
;=====================================================================
cdx_f09:
        mov     ax,CDX_ERR_FUNC
        jmp     cdx_fail

;=====================================================================
; 06h and 07h - turn the driver's debugging on and off.
;
; Deliberately a no-op that reports success.  There is no driver and
; therefore no debugging in it, and a program that switches tracing on
; is not asking a question whose answer changes anything it does.
;=====================================================================
cdx_fnul:
        clc
        ret

;=====================================================================
; 0Bh - is this drive one of ours?
;
;   CX = drive
;   -> BX = ADADh always, AX non-zero if it is a CD-ROM
;
; BX IS THE INSTALLATION CHECK AGAIN, in the form the later versions
; used: ADADh is the signature that says something answered at all,
; and AX is the answer to the question that was asked.  Both have to
; be set, including on the "no" - the signature is about US, not about
; the drive.
;=====================================================================
cdx_f0B:
        mov     word ptr [vdm_ctx+V86C_EBX],0ADADh
        call    cdx_unit
        jc      cdx_f0Bno
        mov     word ptr [vdm_ctx+V86C_EAX],1
        clc
        ret
cdx_f0Bno:
        mov     word ptr [vdm_ctx+V86C_EAX],0
        clc
        ret

;=====================================================================
; 0Ch - which version of MSCDEX this claims to be.
;
;   -> BH = major, BL = minor
;
; 2.25 is claimed because it is the last one and the one every program
; was written against, and because everything a program tests the
; version for - the 0Dh drive-letter list, the ADADh signature on 0Bh
; - is here.  Claiming something older would send programs down paths
; that exist for MSCDEX 1.x and are worse.
;=====================================================================
cdx_f0C:
        mov     byte ptr [vdm_ctx+V86C_EBX+1],CDX_VER_MAJOR
        mov     byte ptr [vdm_ctx+V86C_EBX],CDX_VER_MINOR
        clc
        ret

;=====================================================================
; 0Dh - the drive letters, one byte each.
;
;   ES:BX -> a byte per drive, 0 = A
;
; The letters are NOT necessarily consecutive - CDROM= in CONFIG.SYS
; can put the first one anywhere, and a second unit follows it rather
; than following the disks - so this call exists precisely because
; 00h's "count and first letter" cannot describe every arrangement.
; Answering it from the volume table costs nothing and answering it
; from 00h's two numbers would be a guess.
;=====================================================================
cdx_f0D:
        call    cdx_count                       ; ECX = how many
        call    cdx_esbx
        mov     eax,edi
        call    buf_ok                          ; ECX bytes, one a drive
        jc      cdx_badptr

        xor     esi,esi                         ; the letter being examined
cdx_0Dscan:
        cmp     esi,MAX_DRIVES
        jae     cdx_ok
        mov     eax,esi
        call    drv_slot                        ; AL in, EBX = the slot out
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      cdx_0Dnext
        test    byte ptr [ebx+DRV_FLAGS],DRVF_CDROM
        jz      cdx_0Dnext
        mov     eax,esi
        mov     [edi],al
        inc     edi
cdx_0Dnext:
        inc     esi
        jmp     short cdx_0Dscan

;=====================================================================
; 0Fh - the directory entry for a path.
;
;   CL = drive, CH bit 0 = the caller would take a High Sierra entry,
;   ES:BX -> the path, ASCIIZ and without a drive letter,
;   SI:DI -> the buffer it is written into (SI is the SEGMENT)
;   -> AX = 0, the entry is ISO9660's.  1 would mean High Sierra's
;
; THE SHAPE WAS READ OUT OF MSCDEX.EXE rather than remembered, because
; a structure a program parses is not a place to be approximately
; right.  It is NOT the disc's record copied out: the both-endian
; pairs are collapsed to their little-endian halves, the fields are
; re-ordered, the version after the ';' is lifted out into a word of
; its own, and the system use area follows fifty-nine bytes of fixed
; fields.
;
;   00h  byte   length of the extended attribute record, in blocks
;   01h  dword  the extent
;   05h  word   a word MSCDEX takes from its own per-drive state.  What
;               it means could not be established from the binary and
;               is not guessed at here: this is written as zero, which
;               is at least not a wrong number
;   07h  dword  the size in bytes
;   0Bh  7 bytes the recording time, exactly as the disc records it
;   12h  byte   the file flags
;   13h  byte   interleave size, 14h interleave skip
;   15h  word   volume set sequence number
;   17h  byte   the length of the name, 18h the name itself
;   38h  word   the version number, the digits after the ';'
;   3Ah  byte   the length of the system use area, 3Bh the area
;
; THE NAME IS THE IDENTIFIER AS RECORDED IN THE TREE BEING READ, which
; on a Joliet disc means UCS-2 - the same bytes 1505h hands back out
; of the supplementary descriptor.  MSCDEX only ever read the primary
; tree, so the question did not arise for it; answering with anything
; else would mean this call describing a different record from the one
; it was asked about.  The extent and the size are the same in both
; trees, which is what a caller reads this for.
;
; CH's flag is read and ignored on purpose: it offers to take a High
; Sierra entry, this reader mounts nothing but ISO9660, and AX says so.
;=====================================================================
cdx_f0F:
        movzx   eax,byte ptr [vdm_ctx+V86C_ECX] ; CL alone - CH is flags
        mov     [cdx_letter],al                 ; on this call and nowhere
        call    cdx_unitof                      ; else in the family
        jc      cdx_baddrv

        call    cdx_getpath                     ; -> cdx_path, CF if the
        jc      cdx_badptr                      ; pointer is not the
                                                ; guest's to read
        mov     al,[cdx_letter]
        call    vol_select                      ; mounted the way every
        jc      cdx_notready                    ; other call on it mounts it

        mov     esi,OFFSET cdx_path
        call    resolve_parent
        jc      cdx_nopath
        cmp     byte ptr [rp_noname],0
        jne     cdx_f0Fdir                      ; the path names a directory

        mov     byte ptr [srch_attr],ATTR_DIR   ; find one of those too
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      cdx_nofile
        jmp     short cdx_f0Fput                ; iso_lastrec is the record
                                                ; that walk was built from
cdx_f0Fdir:
        ; A PATH THAT NAMES A DIRECTORY has no entry to search for - it
        ; IS the directory - so the record is the one the mount kept.
        ; For "\" that is the root's own record out of the volume
        ; descriptor, which is where ISO9660 puts it and the only
        ; directory record on a disc that lives outside a directory.
        push    esi
        push    edi
        mov     esi,OFFSET iso_rootrec
        mov     edi,OFFSET iso_lastrec
        mov     ecx,34
        cld
        rep     movsb
        pop     edi
        pop     esi

cdx_f0Fput:
        call    cdx_f0Fwrite
        jc      cdx_badptr
        mov     word ptr [vdm_ctx+V86C_EAX],0   ; ISO9660, not High Sierra
        clc
        ret

;--- cdx_f0Fwrite - iso_lastrec, in the shape above, into the guest's
;--- SI:DI.  CF when that buffer is not somewhere it may write.
cdx_f0Fwrite:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        ; where the system use area is, and how much of it there is.
        ; A PAD BYTE FOLLOWS AN IDENTIFIER OF EVEN LENGTH - the same
        ; rule iso_susp walks by, and the same one this file's reader
        ; got wrong once by applying it unconditionally.
        ;
        ; The two numbers go in memory rather than in registers because
        ; everything below them here is a string move and ECX is what a
        ; string move counts with.
        mov     esi,OFFSET iso_lastrec
        movzx   ebx,byte ptr [esi+ISO_DR_IDLEN]
        mov     ecx,ebx
        test    bl,1
        jnz     cdxw_odd
        inc     ecx
cdxw_odd:
        add     ecx,ISO_DR_ID                   ; -> where it starts
        mov     [cdx_sysoff],ecx
        movzx   edx,byte ptr [esi+ISO_DR_LEN]
        sub     edx,ecx                         ; -> how long it is
        jnc     cdxw_sys
        xor     edx,edx                         ; a record too short to
cdxw_sys:                                       ; have one
        cmp     edx,CDX_DE_MAX - CDX_DE_SYS
        jbe     cdxw_syslen
        mov     edx,CDX_DE_MAX - CDX_DE_SYS
cdxw_syslen:
        mov     [cdx_syslen],edx

        ; the guest's buffer: SI is the segment and DI the offset, which
        ; is the other way round from every other call in this family
        movzx   edi,word ptr [vdm_ctx+V86C_ESI]
        shl     edi,4
        movzx   eax,word ptr [vdm_ctx+V86C_EDI]
        add     edi,eax
        mov     eax,edi
        mov     ecx,CDX_DE_SYS
        add     ecx,[cdx_syslen]
        call    buf_ok
        jc      cdxw_bad

        push    edi                             ; the fixed fields first
        mov     ecx,CDX_DE_SYS
        xor     al,al
        cld
        rep     stosb                           ; a clean buffer: fields
        pop     edi                             ; this record does not set
                                                ; must not read as junk
        mov     al,[esi+ISO_DR_XALEN]
        mov     [edi+0],al
        mov     eax,dword ptr [esi+ISO_DR_EXTENT]
        mov     dword ptr [edi+1],eax
        mov     eax,dword ptr [esi+ISO_DR_SIZE]
        mov     dword ptr [edi+7],eax

        push    esi                             ; the seven date bytes,
        push    edi                             ; as recorded
        add     esi,ISO_DR_DATE
        add     edi,0Bh
        mov     ecx,7
        rep     movsb
        pop     edi
        pop     esi

        mov     al,[esi+ISO_DR_FLAGS]
        mov     [edi+12h],al
        mov     al,[esi+ISO_DR_UNIT]
        mov     [edi+13h],al
        mov     al,[esi+ISO_DR_GAP]
        mov     [edi+14h],al
        mov     ax,word ptr [esi+ISO_DR_VOLSEQ]
        mov     word ptr [edi+15h],ax

        ; the name, and the version that was hanging off the end of it
        call    cdx_f0Fname

        ; and the system use area behind the identifier
        mov     edx,[cdx_syslen]
        mov     [edi+CDX_DE_SYSLEN],dl
        or      edx,edx
        jz      cdxw_done
        add     edi,CDX_DE_SYS
        add     esi,[cdx_sysoff]
        mov     ecx,edx
        rep     movsb
cdxw_done:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
cdxw_bad:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;--- cdx_f0Fname - ESI = the record, EDI = the answer.  The identifier
;--- into the name field, its length in front of it, and the digits
;--- after the ';' into the version word.  ECX and EDX preserved.
;
; THE FIELD IS THIRTY-TWO BYTES AND A NAME CAN BE LONGER, on a Joliet
; disc certainly and at ISO9660 level 3 as well.  MSCDEX never met one
; - it read the primary tree of discs written to level 1 - and its
; structure has no room for one, so a longer identifier is written as
; much as fits and the length says how much that was.  The alternative
; is to overrun the version and system-use fields behind it, which is
; what the original does.
cdx_f0Fname:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        movzx   ecx,byte ptr [esi+ISO_DR_IDLEN]
        add     esi,ISO_DR_ID
        xor     edx,edx                         ; the version, once found
        xor     ebx,ebx                         ; how many bytes are name
cdxn_scan:
        cmp     ebx,ecx
        jae     cdxn_scanned
        cmp     byte ptr [esi+ebx],';'
        je      cdxn_semi
        inc     ebx
        jmp     short cdxn_scan
cdxn_semi:
        ; the digits after it, which are decimal and at most five - a
        ; version is a word on the disc as well as in the answer
        mov     eax,ebx
        inc     eax
cdxn_dig:
        cmp     eax,ecx
        jae     cdxn_scanned
        movzx   edi,byte ptr [esi+eax]
        cmp     edi,'0'
        jb      cdxn_scanned
        cmp     edi,'9'
        ja      cdxn_scanned
        sub     edi,'0'
        imul    edx,edx,10
        add     edx,edi
        inc     eax
        cmp     edx,0FFFFh
        jbe     cdxn_dig
        mov     edx,0FFFFh
cdxn_scanned:
        pop     edi
        push    edi
        mov     word ptr [edi+CDX_DE_VER],dx
        mov     ecx,ebx                         ; the name without it
        cmp     ecx,CDX_DE_NAMEMAX
        jbe     cdxn_fits
        mov     ecx,CDX_DE_NAMEMAX
cdxn_fits:
        mov     [edi+CDX_DE_NAMELEN],cl
        add     edi,CDX_DE_NAME
        cld
        rep     movsb

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;--- cdx_getpath - the guest's ES:BX path into cdx_path, with the
;--- drive letter on the front.  CF when the guest's pointer is not
;--- somewhere it may read from.
;
; THE LETTER HAS TO BE ADDED because MSCDEX's paths carry none - the
; drive is in CL - while resolve_parent works on the letter in the
; path or on the current drive, and this call means neither of those.
cdx_getpath:
        push    eax
        push    ecx
        push    esi
        push    edi

        call    cdx_esbx                        ; -> EDI = the guest's
        mov     eax,edi                         ; string, flat
        mov     ecx,CDX_PATHMAX
        call    buf_ok
        jc      cdxp_bad

        mov     esi,edi
        mov     edi,OFFSET cdx_path
        mov     al,[cdx_letter]
        add     al,'A'
        mov     [edi],al
        mov     byte ptr [edi+1],':'
        add     edi,2
        cmp     byte ptr [esi],'\'
        je      cdxp_copy
        mov     byte ptr [edi],'\'              ; MSCDEX's paths are from
        inc     edi                             ; the root whether or not
cdxp_copy:                                      ; they say so
        mov     ecx,CDX_PATHMAX
cdxp_lp:
        mov     al,[esi]
        mov     [edi],al
        or      al,al
        jz      cdxp_done
        inc     esi
        inc     edi
        dec     ecx
        jnz     cdxp_lp
        mov     byte ptr [edi],0                ; a string with no
cdxp_done:                                      ; terminator in range
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        clc
        ret
cdxp_bad:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; The shared failures.  CF is set IN THE GUEST'S FLAGS as well as
; ours: this family reports an error the way a DOS call does, and a
; caller that only looks at AX has been given a number that means
; something.
;=====================================================================
cdx_nopath:
        mov     ax,CDX_ERR_NOPATH
        jmp     short cdx_fail
cdx_nofile:
        mov     ax,CDX_ERR_NOFILE
        jmp     short cdx_fail
cdx_baddrv:
        mov     ax,CDX_ERR_DRIVE
        jmp     short cdx_fail
cdx_notready:
        mov     ax,CDX_ERR_NOTRDY
        jmp     short cdx_fail
cdx_badptr:
        mov     ax,CDX_ERR_FUNC
cdx_fail:
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        clc                                     ; handled - the CALL succeeded
        ret                                     ; even though the answer is no

;=====================================================================
; cdx_count - how many CD-ROM drive letters there are, and the first.
;             ECX = the count, EBX = the first letter (0 = A).
;             Every other register is preserved.
;=====================================================================
cdx_count:
        push    eax
        push    edx
        push    esi
        xor     ecx,ecx
        xor     ebx,ebx
        mov     dl,0FFh                         ; the lowest letter seen
        xor     esi,esi
cdc_scan:
        cmp     esi,MAX_DRIVES
        jae     cdc_done
        mov     eax,esi
        push    ebx                             ; drv_slot answers in EBX and
        call    drv_slot                        ; EBX is the answer being built
        mov     eax,ebx
        pop     ebx
        test    byte ptr [eax+DRV_FLAGS],DRVF_PRESENT
        jz      cdc_next
        test    byte ptr [eax+DRV_FLAGS],DRVF_CDROM
        jz      cdc_next
        inc     ecx
        cmp     dl,0FFh
        jne     cdc_next
        mov     ebx,esi                         ; the first one
        mov     dl,0
cdc_next:
        inc     esi
        jmp     short cdc_scan
cdc_done:
        pop     esi
        pop     edx
        pop     eax
        ret

;=====================================================================
; cdx_unit - the drive letter in the guest's CX, as an ATAPI unit.
;            -> AL = the unit.  CF set if that letter is not a CD.
;
; DRV_BIOS HOLDS THE ATAPI UNIT AND NOT A BIOS DRIVE on a DRVF_CDROM
; row, which is the trap this kernel has already been bitten by once:
; unit 0 is a perfectly valid BIOS drive number and the failure is not
; an error but a WRONG DEVICE.  Nothing here may reach the block layer
; with this number in it.
;=====================================================================
cdx_unit:
        movzx   eax,word ptr [vdm_ctx+V86C_ECX]
        ; and then the same question, asked about a letter that is in a
        ; register rather than in the guest's CX.  150Fh puts flags in
        ; CH, so it is the one call in the family whose drive is CL
        ; alone - and a whole-CX read would refuse every letter on it.
cdx_unitof:
        push    ebx
        push    ecx
        mov     ecx,eax
        cmp     ecx,MAX_DRIVES
        jae     cdu_no
        mov     eax,ecx
        call    drv_slot                        ; -> EBX
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      cdu_no
        test    byte ptr [ebx+DRV_FLAGS],DRVF_CDROM
        jz      cdu_no
        movzx   eax,byte ptr [ebx+DRV_BIOS]     ; the ATAPI unit
        pop     ecx
        pop     ebx
        clc
        ret
cdu_no:
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; cdx_readpvd - the primary volume descriptor of the unit in AL, into
;               cdx_buf.  CF set if it will not read or is not one.
;
; ITS OWN BUFFER AND NOT iso_buf.  The ISO reader's cache belongs to
; the volume that is MOUNTED, and these calls name a drive letter that
; may not be it - reading through the cache would answer about the
; right disc and leave the cache describing a block from the wrong
; one, which is the kind of fault that shows up somewhere else
; entirely and much later.
;=====================================================================
cdx_readpvd:
        push    ebx
        push    ecx
        push    edi
        push    edx
        mov     dl,al                           ; the unit, kept
        ; WHICH SESSION'S DESCRIPTORS.  This call names a drive letter
        ; that need not be the mounted one, so the base cannot be taken
        ; from iso_base: it is found for this unit, the same way a
        ; mount finds it.
        mov     edi,OFFSET cdx_buf
        call    iso_findbase                    ; AL = unit -> EAX = base
        mov     ebx,eax
        add     ebx,ISO_PVDBLK
        mov     al,dl
        mov     ecx,ISO_MAXDESC
cdp_look:
        push    ecx
        mov     al,dl
        mov     ecx,1
        mov     edi,OFFSET cdx_buf
        call    dword ptr ds:[IOS_CDREAD]
        pop     ecx
        jc      cdp_bad
        cmp     byte ptr [cdx_buf+ISO_VD_TYPE],ISO_VDT_END
        je      cdp_bad
        cmp     byte ptr [cdx_buf+ISO_VD_TYPE],ISO_VDT_PRIMARY
        je      cdp_got
        inc     ebx
        loop    cdp_look
cdp_bad:
        pop     edx
        pop     edi
        pop     ecx
        pop     ebx
        stc
        ret
cdp_got:
        pop     edx
        pop     edi
        pop     ecx
        pop     ebx
        clc
        ret

;--- cdx_esbx - the guest's ES:BX as a flat address in EDI ----------
cdx_esbx:
        push    eax
        mov     edi,dword ptr [vdm_ctx+V86C_ES]
        shl     edi,4
        movzx   eax,word ptr [vdm_ctx+V86C_EBX]
        add     edi,eax
        pop     eax
        ret

cdx_dev     db  0                       ; the unit a call is about
cdx_letter  db  0                       ; ...and which letter that was
            db  0, 0
cdx_cnt     dd  0                       ; and how many blocks it wants
cdx_sysoff  dd  0                       ; 150Fh: where the system use area
cdx_syslen  dd  0                       ; sits in the record, and its size
cdx_path    db  CDX_PATHMAX + 8 dup (0) ; a path with a drive letter put
                                        ; on the front of it
cdx_buf     db  ISO_BLKSIZE dup (0)     ; one descriptor, read for us alone
