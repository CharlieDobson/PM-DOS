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
        call    dword ptr ds:[edx]
        jnc     dio_ok

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
        pushad
        movzx   edi,di                  ; the code, and nothing above it
        mov     esi,OFFSET crit_devhdr
        xor     ebp,ebp
        int     24h
        and     eax,3                   ; an answer of 4 is an answer
        mov     [crit_resp],al          ; of 3
        popad
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

crit_busy   db  0                       ; a handler is running
crit_resp   db  CERR_FAIL

dio_drv     db  0                       ; the transfer in progress, kept
dio_wr      db  0                       ; whole so a retry can repeat it
dio_cnt     dw  0
dio_lba     dd  0
dio_buf     dd  0
dio_svc     dd  0
