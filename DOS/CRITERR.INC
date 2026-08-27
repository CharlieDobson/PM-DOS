;=====================================================================
; CRITERR.INC - INT 24h, critical errors, and the sector I/O that
;               raises them
;
; Every sector the filesystem moves goes through disk_read or
; disk_write, and a failure there is no longer just CF on the way back
; up: the kernel builds the classic INT 24h register set, calls the
; vector, and does what the answer says - transfer again, pretend it
; worked, fail the DOS call that asked for it, or end the program.
; Before this a dead floppy came back as "File not found", which is
; a true statement about a disk nobody could read and a useless one.
;
; The register set is described in PMDOS.INC, next to the CE_* and
; CERR_* equates the kernel and the handler both use.
;
; A handler may call INT 21h - COMMAND's writes the message and reads
; the key that way - but it must keep off the DISK, because what
; disk_read remembers about the transfer it is retrying is a handful
; of variables and not a stack.  Real DOS drew the same line and drew
; it tighter (nothing above function 0Ch).
;
; INT 25h and 26h are deliberately NOT routed through here.  Absolute
; disk I/O has always reported its own errors to its caller - SYS and
; FORMAT would rather hear "that sector would not write" than have
; the shell ask the user about it - and DOS never raised a critical
; error out of them either.
;
; Included by DOSMAIN.ASM, after FAT.INC.
;=====================================================================

;=====================================================================
; disk_read / disk_write - the filesystem's only route to a sector.
;
;   AL = BIOS drive   EBX = LBA   CX = sector count   EDI = buffer
;
; CF set means the transfer did not happen AND the handler was content
; to let the call fail.  All registers are preserved: the callers all
; sit inside a pushad, and a retry needs the four inputs back anyway.
;=====================================================================
disk_read:
        push    eax
        mov     eax,IOS_RDBLK
        mov     byte ptr [dio_wr],0
        jmp     short disk_io
disk_write:
        push    eax
        mov     eax,IOS_WRBLK
        mov     byte ptr [dio_wr],CE_WRITE
disk_io:
        mov     [dio_svc],eax
        pop     eax
        push    ebx
        push    ecx
        push    edx
        push    edi
        mov     [dio_drv],al
        mov     [dio_lba],ebx
        mov     [dio_cnt],cx
        mov     [dio_buf],edi

dio_try:
        mov     al,[dio_drv]            ; the driver is free to clobber
        mov     ebx,[dio_lba]           ; every one of these, and a
        mov     cx,[dio_cnt]            ; retry has to start clean
        mov     edi,[dio_buf]
        mov     edx,[dio_svc]
        call    dio_raw
dio_done:
        jc      dio_bad

        ; AND IF VERIFY IS ON, THE WRITE IS NOT OVER YET.  A read-back
        ; that disagrees is a failed write and is reported as one -
        ; same status byte, same critical error, same retry - because
        ; the whole point of the flag is that the caller finds out.
        cmp     byte ptr [dio_wr],0
        je      dio_ok
        cmp     byte ptr [verify_on],0
        je      dio_ok

        ; ESI IS SAVED HERE AND THE ABSENCE OF THIS PUSH COST AN HOUR.
        ; disk_write preserves every register its callers use, and it
        ; preserves ESI by NEVER TOUCHING IT - there is no push for it
        ; below, because until this release nothing in here needed a
        ; fifth register.  Handing the buffer to dio_verify in ESI is
        ; four instructions that read like argument setup and are
        ; really a promise being broken: fat_flush walks the FAT copies
        ; with ESI as its counter, so a verified write turned its
        ; two-copy loop into one that never stopped and wrote the FAT
        ; across the whole disk.  POP does not touch the flags, so the
        ; answer still arrives in CF.
        push    esi
        mov     al,[dio_drv]
        mov     ebx,[dio_lba]
        movzx   ecx,word ptr [dio_cnt]
        mov     esi,[dio_buf]
        call    dio_verify
        pop     esi
        jnc     dio_ok
        mov     ah,10h                  ; "CRC error", which is what a
                                        ; sector that reads back different
                                        ; is, whatever made it one
dio_bad:
        call    crit_disk               ; -> AL = the answer
        cmp     al,CERR_RETRY
        je      dio_try
        cmp     al,CERR_IGNORE          ; "it worked" - and the buffer
        je      dio_ok                  ; holds whatever it holds
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret
dio_ok:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret

;=====================================================================
; dio_raw - one sector transfer, to whichever kind of drive AL names.
;
;   AL = BIOS drive   EBX = LBA   CX = count   EDI = buffer
;   EDX = IOS_RDBLK or IOS_WRBLK
;
; A LOADED BLOCK DRIVER IS A DRIVE HERE TOO, and this is the only
; place that has to know it.  Units get pseudo BIOS drive numbers from
; DL_UNIT_BASE up - a range no BIOS uses - so the number flows through
; fat_mount and bios2log and the volume table untouched, and one
; comparison sends the transfer to a request packet instead of to the
; block layer.
;
; IT IS A ROUTINE RATHER THAN FOUR LINES INSIDE dio_try because three
; callers need it now and only one of them knew: disk_read and
; disk_write, VERIFY's read-back, and INT 25h/26h - and the third was
; missing it.  Absolute disk I/O on a driver's drive went to the block
; layer with a drive number no BIOS has ever heard of and failed, and
; nothing noticed because nothing has called 25h or 26h since v0.17.
;=====================================================================
dio_raw:
        cmp     al,DL_UNIT_BASE
        jb      dir_hw
        jmp     dl_blkio                ; its CF and AH are the answer
dir_hw:
        jmp     dword ptr ds:[edx]

;=====================================================================
; dio_verify - a write came back clean.  Read it again and compare.
;
;   AL = BIOS drive   EBX = LBA   ECX = sector count   ESI = the data
;
;              CF set if any sector differs, or will not read at all.
;              Every register is preserved; the answer is the flag.
;
; THIS IS THE OTHER HALF OF 2Eh, and where it belongs.  The flag has
; been remembered honestly since v0.31 and acted on by nobody, which
; is the difference between "writes are not verified" and "VERIFY is
; broken" - a program that turns it on and copies a file has been
; asking a question nothing answered.
;
; ONE SECTOR AT A TIME, THROUGH A BUFFER OF ITS OWN.  The caller's
; buffer cannot be scratch - it is the data being checked - and the
; transfer may be a whole cluster, which is larger than anything the
; kernel would spend statically on a feature that is off by default.
; So a verified write costs one extra transfer per sector, and that
; is exactly the price the roadmap said it would be: it is why the
; flag exists at all instead of the behaviour being permanent.
;
; A read that FAILS counts as a mismatch rather than reporting its own
; status, because the answer the caller needs is about the write.  The
; sector cannot be shown to hold what it was given, and that is one
; fact however the reading went.
;=====================================================================
dio_verify:
        pushad
        mov     [dv_drv],al
        mov     [dv_left],ecx
dv_sec:
        cmp     dword ptr [dv_left],0
        je      dv_same
        mov     al,[dv_drv]
        mov     cx,1
        mov     edi,OFFSET vfy_buf
        mov     edx,IOS_RDBLK
        call    dio_raw
        jc      dv_diff
        mov     edi,OFFSET vfy_buf      ; the driver may have moved it
        mov     ecx,512 / 4
        repe    cmpsd                   ; ESI walks the caller's copy and
        jne     dv_diff                 ; is left on the next sector
        inc     ebx
        dec     dword ptr [dv_left]
        jmp     short dv_sec
dv_same:
        popad
        clc
        ret
dv_diff:
IFDEF VFY_DEBUG
        push    esi
        mov     esi,OFFSET msgVfy
        call    dword ptr ds:[IOS_CONSTR]
        pop     esi
        mov     eax,ebx                 ; the sector that disagreed
        call    vdm_hex32
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
        mov     eax,[dv_left]
        call    vdm_hex32
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
        movzx   eax,byte ptr [esi-4]    ; what was sent...
        call    vdm_hex32
        mov     al,'/'
        call    dword ptr ds:[IOS_CONOUT]
        movzx   eax,byte ptr [edi-4]    ; ...and what came back
        call    vdm_hex32
        mov     al,']'
        call    dword ptr ds:[IOS_CONOUT]
ENDIF
        popad
        stc
        ret

;=====================================================================
; crit_media - the volume on BIOS drive AL read perfectly well and is
;              still not a filesystem.  DOS called that a critical
;              error too ("Invalid media type reading drive A") and it
;              is the honest answer for an unformatted disk, which
;              until now looked exactly like an empty one.
;
;              Returns AL = the handler's answer; the mount fails
;              either way, so only retry is acted on.
;=====================================================================
crit_media:
        mov     [dio_drv],al
        mov     byte ptr [dio_wr],0
        mov     dword ptr [dio_lba],0
        push    edi
        mov     edi,CERR_MEDIA
        mov     ah,CE_FAIL_OK OR CE_RETRY_OK
        call    bios2log
        call    crit_raise
        pop     edi
        ret

;=====================================================================
; crit_disk - a sector transfer failed.  AH = the driver's status
;             byte; the dio_* variables say which transfer it was.
;             Returns AL = the answer, one of CERR_IGNORE, CERR_RETRY
;             or CERR_FAIL (abort never comes back).
;=====================================================================
crit_disk:
        ; A hard disk that will not answer INT 13h/08h is not a drive
        ; that is not ready - it is not a drive, and the caller should
        ; go on failing with "invalid drive" as it always has.  A
        ; FLOPPY unit is the opposite case: A: and B: exist on a PC
        ; whether or not anything is in them, and "Not ready reading
        ; drive A" with a chance to put a disk in and press R is the
        ; oldest reason INT 24h exists.
        cmp     ah,0FFh
        jne     cd_raise
        cmp     byte ptr [dio_drv],80h
        jb      cd_raise
        mov     al,CERR_FAIL
        ret

cd_raise:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        movzx   edi,ah
        call    crit_map                ; status byte -> EDI = DOS code

        call    crit_area               ; -> AH = the area bits
        or      ah,CE_FAIL_OK OR CE_RETRY_OK
        or      ah,[dio_wr]

        mov     al,[dio_drv]
        call    bios2log                ; the handler speaks A: = 0
        call    crit_raise

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; crit_map - EDI = the driver's status byte -> EDI = the code the
;            handler is shown.  Status bytes are INT 13h's, which the
;            IDE path speaks too: BLOCK.INC hands back 0Ch for an ATA
;            failure - which is "general failure" on both sides of the
;            table, a coincidence worth keeping - and FFh for a unit
;            that is not there.
;---------------------------------------------------------------------
crit_map:
        push    eax
        push    ecx
        push    esi
        mov     eax,edi
        mov     esi,OFFSET crit_tbl
        mov     ecx,CRIT_TBL_LEN
cm_lp:
        cmp     al,[esi]
        je      cm_hit
        add     esi,2
        loop    cm_lp
        mov     edi,CERR_GENERAL        ; nothing recognised it
        jmp     short cm_out
cm_hit:
        movzx   edi,byte ptr [esi+1]
cm_out:
        pop     esi
        pop     ecx
        pop     eax
        ret

crit_tbl:
        db      03h, CERR_WRPROT        ; write protected
        db      80h, CERR_NOTREADY      ; timeout - nothing answered
        db      0FFh,CERR_NOTREADY      ; no such unit (a floppy: empty)
        db      31h, CERR_NOTREADY      ; no media in the drive
        db      06h, CERR_NOTREADY      ; media changed
        db      10h, CERR_DATA          ; CRC error
        db      40h, CERR_SEEK          ; seek failed
        db      02h, CERR_SECTOR        ; address mark missing
        db      04h, CERR_SECTOR        ; sector not found
        db      01h, CERR_CMD           ; the controller refused it
CRIT_TBL_LEN    EQU     10

IF ($ - crit_tbl) - CRIT_TBL_LEN * 2
.ERR    ; CRIT_TBL_LEN no longer matches the table - update the EQU
ENDIF

;---------------------------------------------------------------------
; crit_area - AH = the area bits for the transfer in dio_lba.
;
; Nothing is known about a volume before its BPB has been read, so a
; failure during the mount is reported as the DOS area - which is
; where the boot sector it was reading genuinely lives.  On FAT32 the
; root is a cluster chain in the data area and gets reported as data,
; which is what it is; the directory area means the FIXED root, and
; only FAT12 and FAT16 have one.
;---------------------------------------------------------------------
crit_area:
        push    ebx
        xor     ah,ah
        cmp     byte ptr [fat_ok],0
        je      ca_out
        mov     bl,[dio_drv]            ; the layout in vol_state is
        cmp     bl,[fat_drive]          ; the LOADED volume's only
        jne     ca_out
        mov     ebx,[dio_lba]
        cmp     ebx,[fat_datalba]
        jb      ca_notdata
        mov     ah,CE_AREA_DATA
        jmp     short ca_out
ca_notdata:
        cmp     ebx,[fat_rootlba]
        jb      ca_notdir
        mov     ah,CE_AREA_DIR
        jmp     short ca_out
ca_notdir:
        cmp     ebx,[fat_fatlba]
        jb      ca_out
        mov     ah,CE_AREA_FAT
ca_out:
        pop     ebx
        ret

;=====================================================================
; crit_dev - A CHARACTER DEVICE DRIVER SAID NO.  AL = the low byte of
;            the status word it put in the request packet, ESI = the
;            flat address of its device header, EBX = the same header
;            as a real-mode far pointer.  Returns AL = the handler's
;            answer.
;
; TWO POINTERS TO ONE HEADER because there are two kinds of handler
; and they cannot hold the same thing.  A native one is shown the flat
; address; a DOS box's is shown BP:SI, which has to be a segment and
; an offset.  A loaded driver is the only device in this kernel where
; both exist - it lives below 1MB, which is where drivers live.
;
; This is the other half of INT 24h and it has been on the roadmap
; since v0.20 waiting for something to raise it.  The wait was real:
; the only character devices were CON, NUL, PRN and AUX, and the first
; two cannot fail while the other two are reached through the BIOS,
; which answers by not answering.  A DEVICE= driver is the first
; character device in this kernel that can report a reason - and until
; now dl_out threw the reason away, so a driver saying "out of paper"
; and a driver saying "done" were the same event.
;
; NO TRANSLATION TABLE, unlike crit_disk.  A block driver's status
; byte is INT 13h's and has to be mapped; a character driver's IS a
; DOS error code, because the request packet was designed to carry one
; - the numbering in the packet and the numbering INT 24h publishes
; are the same numbering, and have been since DOS 2.0.
;
; FAIL IS NOT OFFERED, and that is a statement about the call rather
; than about the device.  dev_out has no error return and never had
; one: 02h, 09h and the rest of the DOS character-output calls cannot
; report that a byte did not go anywhere.  So a handler answering Fail
; would get exactly what Ignore gives it - the byte is lost, the call
; says nothing - and offering a choice with no consequence is worse
; than not offering it.  Retry, Ignore and Abort all mean different
; things here, and those three are what AH allows.
;=====================================================================
crit_dev:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        movzx   edi,al
        cmp     edi,CERR_MAXCODE
        jbe     cdv_code
        mov     edi,CERR_GENERAL        ; a driver inventing its own
cdv_code:
        mov     edx,[crit_rmhdr]        ; the handler is shown the driver,
        push    edx                     ; whichever kind of handler it is
        mov     [crit_hdr],esi
        mov     [crit_rmhdr],ebx
        mov     ah,CE_CHARDEV OR CE_RETRY_OK OR CE_IGNORE_OK
        xor     al,al                   ; there is no drive; DOS says AL is
                                        ; undefined when bit 7 is set, and
                                        ; zero is a better undefined than
                                        ; whatever was in the register
        call    crit_raise
        mov     dword ptr [crit_hdr],OFFSET crit_devhdr
        pop     edx
        mov     [crit_rmhdr],edx        ; ...and the low copy of it back
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; crit_init - a copy of crit_devhdr somewhere a 16-bit program can
;             point at.  Once, at boot.
;
; The header the handler is shown travels in BP:SI, and for a DOS box
; that has to be a REAL-MODE far pointer.  A loaded character driver
; has one already - it lives below 1MB because that is where drivers
; live - but crit_devhdr is inside PMDOS.SYS at 110000h and no
; segment:offset can name it.  So eighteen bytes are copied into
; conventional memory and marked owner 8, the way the XMS callback
; paragraph and the DPMI landing pads are: it belongs to the machine
; and no exit sweep takes it.
;
; If there is no low memory for it the pointer stays zero, and
; crit_guest hands the handler a null BP:SI rather than a wrong one.
;=====================================================================
crit_init:
        pushad
        mov     ebx,(DEVH_LEN + 15) / 16
        call    lm_sysalloc
        jc      ci_none
        movzx   edi,ax
        shl     edi,16
        mov     [crit_rmhdr],edi                ; offset zero within it
        shr     edi,16
        shl     edi,4
        mov     esi,OFFSET crit_devhdr
        mov     ecx,DEVH_LEN
        rep     movsb
ci_none:
        popad
        ret

;=====================================================================
; crit_guest - hand the critical error to a DOS BOX'S OWN INT 24h
;              handler, if there is a box and it has one.
;
;   AH = flags, AL = drive, DI = code, exactly as crit_raise has them.
;   CF clear: the guest answered, and AL is the answer.
;   CF set:   not delivered; the caller falls back to the IDT.
;
; THIS IS THE HALF THAT WAS MISSING, and it was missing invisibly.  A
; guest's 21h/25h writes the REAL-MODE IVT at 0:0, which is where the
; monitor reflects an untrapped interrupt; crit_raise executes INT 24h
; from 32-bit kernel code, which goes through the IDT.  Two tables, and
; only the second was ever consulted - so a DOS program that hooked
; 24h to handle its own disk errors quietly got COMMAND's "Abort,
; Retry, Fail?" prompt instead, and a program that meant to fail
; silently asked the user a question.  R3TEST's fourteenth check has
; covered the ring-3 half since the ring-3 release, which is exactly
; why this went unnoticed: the mechanism works perfectly for the
; caller it was written for.
;
; NOTHING NEW HAD TO BE BUILT.  dpmi_simint parks the current V86
; context, points the guest at an INT instruction with the vector byte
; patched, and borrows the box's own loop; the monitor reflects 24h
; through the guest's table; the handler's IRET lands on a HLT the
; host planted, and dpmi_rmdone hands the registers back.  That is the
; whole delivery, and dos_int28 has been using it for INT 28h since
; v0.30.  EXECUTING the instruction rather than reading the vector is
; also what makes this correct rather than nearly correct - see the
; note in dpmi_simint.
;
; THE FOUR REFUSALS, and each of them is a different way of there
; being nobody to ask:
;
;   No box.  A native program's handler is an IDT gate and the caller
;   already knows how to reach it.
;
;   A simulated interrupt already in flight, or a DEVICE= driver's
;   INIT excursion running.  There is one vdm_ctx and both of those
;   have borrowed it; a third borrower would return the wrong context
;   to the wrong owner.  dl_depth is the driver half and it matters:
;   a driver's INIT makes DOS calls, and one of those can hit a disk.
;
;   No callback paragraph.  Without one there is no INT instruction to
;   execute and no landing pad to come back to.
;
;   AND THE GUEST HAS NOT HOOKED 24h AT ALL, which is the one that
;   cannot be skipped.  The vector always points at SOMETHING - v0.41
;   plants an IRET there for every DOS vector a box reflects - so a
;   box that never hooked it would run that IRET and come back with
;   whatever was in AL, which is the DRIVE NUMBER, which reads as
;   Ignore.  Comparing the vector against the stub we planted is what
;   tells "nobody is listening" from "somebody is".
;=====================================================================
crit_guest:
        mov     [cg_ax],ax                      ; the register set, before
        mov     [cg_di],di                      ; anything below needs them

        cmp     dword ptr ds:[vdm_busy],0
        je      cg_no
        cmp     dword ptr ds:[dpmi_rmact],0
        jne     cg_no
        cmp     dword ptr [dl_depth],0
        jne     cg_no
        cmp     word ptr ds:[xms_cbseg],0
        je      cg_no
        call    crit_hooked
        jc      cg_no

        mov     byte ptr [cg_ok],0
        pushad

        ; The register set, from the GUEST'S OWN parked context, for
        ; the reason dos_int28 gives about INT 28h: a critical error is
        ; taken at a moment when the program was in the middle of
        ; something, so the handler sees whatever was live - and in
        ; particular a plausible DS.  Zeroes would be a machine state
        ; no real DOS ever produces.
        mov     edi,OFFSET crit_rmcs
        xor     eax,eax
        mov     ecx,(RMCS_LEN + 3) / 4
        rep     stosd

        mov     esi,OFFSET vdm_ctx
        mov     edi,OFFSET crit_rmcs
        mov     eax,[esi+V86C_EBX]
        mov     [edi+RMCS_EBX],eax
        mov     eax,[esi+V86C_EDX]
        mov     [edi+RMCS_EDX],eax
        mov     eax,[esi+V86C_ECX]
        mov     [edi+RMCS_ECX],eax
        mov     eax,[esi+V86C_ES]
        mov     [edi+RMCS_ES],ax
        mov     eax,[esi+V86C_DS]
        mov     [edi+RMCS_DS],ax
        mov     eax,[esi+V86C_FS]
        mov     [edi+RMCS_FS],ax
        mov     eax,[esi+V86C_GS]
        mov     [edi+RMCS_GS],ax

        ; THE GUEST'S OWN STACK, and not the scratch one dpmi_simint
        ; would otherwise lend.  A handler is the program's code and
        ; the program sized its own stack; the 256 bytes the DPMI host
        ; keeps for clients is tight for one that prints a message and
        ; reads a key, and this is a place where running out would be
        ; very hard to recognise afterwards.  It is also what a real
        ; INT 24h looks like from inside: an interrupt taken on the
        ; stack that was current.
        mov     eax,[esi+V86C_SS]
        mov     [edi+RMCS_SS],ax
        mov     eax,[esi+V86C_ESP]
        mov     [edi+RMCS_SP],ax

        ; ...and the four registers that ARE the critical error.
        movzx   eax,word ptr [cg_ax]
        mov     [edi+RMCS_EAX],eax
        movzx   eax,word ptr [cg_di]
        mov     [edi+RMCS_EDI],eax
        mov     eax,[crit_rmhdr]                ; BP:SI, the device header
        movzx   edx,ax                          ; as a guest can hold it
        mov     [edi+RMCS_ESI],edx
        shr     eax,16
        mov     [edi+RMCS_EBP],eax

        mov     dword ptr ds:[dpmi_rmcs],OFFSET crit_rmcs
        mov     byte ptr ds:[dpmi_rmint],24h
        mov     word ptr ds:[dpmi_rmwords],0
        call    dpmi_simint
        jc      cg_done
        mov     byte ptr [cg_ok],1
cg_done:
        popad
        cmp     byte ptr [cg_ok],0
        je      cg_no
        mov     al,byte ptr [crit_rmcs+RMCS_EAX]
        clc
        ret
cg_no:
        stc
        ret

;---------------------------------------------------------------------
; crit_hooked - CF clear if the guest's INT 24h vector points anywhere
;               other than the IRET vdm_stubs planted.
;---------------------------------------------------------------------
crit_hooked:
        push    eax
        push    edx
        movzx   edx,word ptr ds:[xms_cbseg]
        shl     edx,16
        mov     dx,VDMCB_IRET
        mov     eax,dword ptr ds:[24h * 4]
        cmp     eax,edx
        je      ch_no                           ; still our stub
        or      eax,eax
        jz      ch_no                           ; 0000:0000 is nobody's
        pop     edx
        pop     eax
        clc
        ret
ch_no:
        pop     edx
        pop     eax
        stc
        ret

;=====================================================================
; crit_raise - AH = flags, AL = drive, DI = error code.  Calls the
;              INT 24h vector and returns its answer in AL.
;
; A critical error raised while a handler is already running is not
; asked about - it is failed on the spot, which is exactly what DOS's
; error-mode flag in the SDA has always done.  The lock comes off
; here on the way out, in 4Ch when an abort unwinds through it, and
; through 21h/F1h AL=5 when a handler leaves by a route of its own:
; COMMAND longjmps home to its prompt rather than IRETing back, the
; same escape it takes for ^C, and it says so before it goes.
;
; Registers are saved and restored around the call because a handler
; is a program's code and only AL is promised back.  If it never
; returns, the saved copies go the way of the rest of the stack.
;=====================================================================
crit_raise:
        cmp     byte ptr [crit_busy],0
        je      cr_go
        mov     al,CERR_FAIL
        ret
cr_go:
        mov     byte ptr [crit_busy],1
        ; The DOS-visible half of the same fact.  It sits one byte
        ; below the InDOS flag because that is where a great deal of
        ; TSR code looks for it - having asked 34h for InDOS and then
        ; subtracted one, rather than calling 5D06h for it.
        mov     byte ptr ds:[DOS_ERRFLG],1
        mov     byte ptr [crit_resp],CERR_FAIL

        ; A DOS BOX'S OWN HANDLER FIRST, when there is one.  The error
        ; belongs to the program that caused it, and a program that
        ; hooked 24h did so to be asked.  crit_guest refuses unless
        ; there is genuinely somebody in there listening, and the IDT
        ; below is what answers for everyone else - including a box
        ; that never hooked the vector, whose errors go to COMMAND's
        ; prompt exactly as they always have.
        call    crit_guest
        jc      cr_ourown
        and     eax,3
        mov     [crit_resp],al
        jmp     short cr_answered

cr_ourown:
        pushad
        movzx   edi,di                  ; the code, and nothing above it
        mov     esi,[crit_hdr]          ; whose driver it was - the block
        xor     ebp,ebp                 ; stand-in unless crit_dev said
        int     24h
        and     eax,3                   ; an answer of 4 is an answer
        mov     [crit_resp],al          ; of 3
        popad
cr_answered:
        mov     byte ptr [crit_busy],0
        mov     byte ptr ds:[DOS_ERRFLG],0
        mov     al,[crit_resp]
        cmp     al,CERR_ABORT
        je      cr_abort
        ret

cr_abort:
        ; The program goes, exactly as it does on INT 23h: 4Ch owns
        ; every unwind here, and the frames this call left behind on
        ; the kernel stack go with everything else it abandons.  The
        ; lock goes back ON first - closing the dying program's files
        ; means flushing them, on the disk that just failed, and the
        ; answer to any error there is "fail quietly", not another
        ; question.  4Ch clears it as it leaves.
        mov     byte ptr [crit_busy],1
        mov     byte ptr ds:[DOS_ERRFLG],1
        mov     ax,4C00h
        int     21h

;=====================================================================
; int24_default - what a program with no handler of its own gets.
;
; It answers FAIL, which is the pre-v0.20 behaviour of the whole
; kernel preserved as a default: the DOS call returns CF and an error
; code and nothing appears on the screen.  The vector is saved into
; the PSP and restored by 4Ch so that a program which hooks it and
; then forgets cannot leave its handler pointing into a slot that no
; longer holds it.
;=====================================================================
int24_default:
        mov     al,CERR_FAIL
        iretd

;=====================================================================
; The device header the handler is handed in ESI.  DOS pointed at the
; driver that failed; PM-DOS has one block driver, so this is it - a
; real header shape with nothing behind it, because the one field a
; program ever reads out of it is the attribute word saying "block".
;=====================================================================
crit_devhdr dd  -1                      ; no next driver
            dw  0000h                   ; attributes: a block device
            dw  0, 0                    ; strategy, interrupt: none
            db  MAX_DRIVES              ; units, where a name would be
            db  7 dup(0)

; ...and which header the handler is actually shown.  A character
; device error points this at the REAL driver's header, because there
; is one and because the attribute word is the field a handler reads:
; showing it the block stand-in would be telling it the printer was a
; disk.  crit_dev puts it back on the way out.
crit_hdr    dd  OFFSET crit_devhdr

; ...and the same thing again as a REAL-MODE far pointer, for a guest's
; handler, which cannot hold the one above.  crit_init fills in the low
; copy of the block stand-in; crit_dev points it at the real driver.
crit_rmhdr  dd  0

crit_rmcs   db  ((RMCS_LEN + 3) / 4) * 4 dup (0)
cg_ax       dw  0                       ; the register set crit_raise was
cg_di       dw  0                       ; called with, parked across the
cg_ok       db  0                       ; pushad that builds the guest's
            db  0

crit_busy   db  0                       ; a handler is running
crit_resp   db  CERR_FAIL

dio_drv     db  0                       ; the transfer in progress, kept
dio_wr      db  0                       ; whole so a retry can repeat it
dio_cnt     dw  0
dio_lba     dd  0
dio_buf     dd  0
dio_svc     dd  0

IFDEF VFY_DEBUG
msgVfy      db  13,10,'[verify: lba/left/sent/got ',0
ENDIF

dv_drv      db  0                       ; dio_verify's own arguments, out of
            db  0,0,0                   ; the registers it has to preserve
dv_left     dd  0                       ; sectors still unverified
vfy_buf     db  512 dup(0)              ; ...and the one being read back
