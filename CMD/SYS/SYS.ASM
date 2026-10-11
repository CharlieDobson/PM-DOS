;=====================================================================
; SYS.ASM - SYS.COM, transfer the system to another disk
;
;   SYS d:          make drive d: bootable from the current drive
;
; Three jobs, all of them plain I/O now that nothing on a PM-DOS
; disk has to be contiguous:
;
;   1. The boot sector.  The SOURCE disk is itself bootable, so its
;      boot code is sitting right there in logical sector 0: read
;      it, splice the TARGET's own BPB over the middle, write it to
;      the target (and to the FAT32 backup at sector 6).  No boot
;      image is carried inside SYS at all.
;   2. Stage 2, on hard disks: the FAT-walking loader lives in the
;      partition's reserved sectors (1..8 on FAT16, 9..16 on FAT32
;      - the S2_* constants).  Copied sector for sector.
;   3. PMIO.SYS, PMDOS.SYS, RMDOS.SYS, CMD.EXE and COMMAND.COM,
;      copied as ordinary files into the target's root, the three
;      .SYS files re-marked read-only+hidden+system.  RMDOS.SYS and
;      COMMAND.COM are only copied if the source has them: a disk
;      without RMDOS.SYS boots and runs everything except Windows,
;      and without COMMAND.COM - the 16-bit interpreter DOS programs
;      get - a DOS program cannot shell out.
;
; WHERE THE SOURCE HAS THEM.  PMIO.SYS and PMDOS.SYS are in its root,
; because the boot code reads nothing else.  The other three are
; looked for there first, and then in the directory the interpreter
; was loaded from when that is on the source drive: an installed
; system keeps its files in a directory of their own, with
; SHELL=C:\DOS\CMD.EXE /P to say which, and the kernel looks for its
; files by the same rule (sys_open in DOS\DOSMAIN.ASM).  On the target
; everything goes in the root, where a disk with no CONFIG.SYS boots.
;
; RMCD.SYS, the CD-ROM's letter under Windows, went across with these
; until it stopped being a system file.  It is an ordinary one now and
; COPY carries it.  A copy an earlier SYS left on the target, read-only
; hidden and system, is made ordinary here so that COPY can replace it.
;
; Because the boot code is filesystem-specific, source and target
; must carry the SAME FAT type - 21h/F1h AL=1 asks the kernel, which
; already knows the cluster-count rule.  The sectors move through
; INT 25h/26h, the DOS absolute disk API, new in this kernel and
; existing for exactly this program.
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

COPYBUF_SIZE    EQU     4000h           ; 16K file-copy buffer (BSS)

_SYS    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_SYS, DS:_SYS, ES:_SYS, SS:_SYS
        ORG     0

;=====================================================================
; D32 header - the sector and copy buffers live in the BSS
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET sy_entry>,<1024 + (S2_SECTORS * 512) + COPYBUF_SIZE>,4096

;=====================================================================
sy_entry:
        mov     eax,OFFSET img_end      ; carve up the BSS
        mov     [src_sec],eax           ; 512: source boot sector
        add     eax,512
        mov     [tgt_sec],eax           ; 512: target boot sector
        add     eax,512
        mov     [s2_buf],eax            ; 4K: stage 2
        add     eax,S2_SECTORS * 512
        mov     [copy_buf],eax          ; 16K: file copies

;--- the target drive from the tail ----------------------------------
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
sy_skipb:
        or      ecx,ecx
        jz      sy_usage
        cmp     byte ptr [esi],' '
        je      sy_bstep
        cmp     byte ptr [esi],9
        jne     sy_havearg
sy_bstep:
        inc     esi
        dec     ecx
        jmp     short sy_skipb
sy_havearg:
        ; /? BEFORE THE DRIVE IS PARSED, because it stands where the
        ; drive would and there is nothing else it could mean.
        cmp     byte ptr [esi],'/'
        jne     sy_notq
        cmp     byte ptr [esi+1],'?'
        je      sy_help
sy_notq:
        cmp     ecx,2
        jb      sy_usage
        mov     al,[esi]
        cmp     byte ptr [esi+1],':'
        jne     sy_usage
        cmp     al,'a'
        jb      sy_up
        cmp     al,'z'
        ja      sy_up
        sub     al,20h
sy_up:
        sub     al,'A'
        cmp     al,4                    ; A: B: C: D:
        jae     sy_baddrv
        mov     [tgt_log],al
        add     al,'A'
        mov     [tgt_letter],al         ; patch the path templates
        mov     [tgt_path],al

;--- the source is the current drive ---------------------------------
        mov     ah,19h
        int     21h
        mov     [src_log],al
        add     al,'A'
        mov     [src_path],al

        mov     al,[src_log]
        cmp     al,[tgt_log]
        je      sy_same

;--- both volumes must carry the same FAT type -----------------------
        mov     dl,[src_log]
        inc     dl                      ; F1h speaks 1-based drives
        mov     ax,0F101h
        int     21h
        jc      sy_baddrv
        mov     [src_type],al
        mov     dl,[tgt_log]
        inc     dl
        mov     ax,0F101h
        int     21h
        jc      sy_baddrv
        cmp     al,[src_type]
        jne     sy_mismatch

;--- the boot sector: source code, target BPB ------------------------
        mov     al,[src_log]            ; read both boot sectors
        mov     ebx,[src_sec]
        mov     ecx,1
        xor     edx,edx
        int     25h
        jc      sy_ioerr
        mov     al,[tgt_log]
        mov     ebx,[tgt_sec]
        mov     ecx,1
        xor     edx,edx
        int     25h
        jc      sy_ioerr

        ; the target must have room for stage 2 in its reserved area
        ; BEFORE anything is written - a one-reserved-sector disk
        ; would have its FATs overwritten (the classic SYS message
        ; exists for exactly this reason)
        cmp     byte ptr [src_type],12  ; floppies boot in one sector
        je      sy_roomok
        mov     edx,[tgt_sec]
        movzx   eax,word ptr [edx+0Eh]  ; the target's reserved count
        mov     edx,S2_START16 + S2_SECTORS
        cmp     byte ptr [src_type],32
        jne     sy_room
        mov     edx,S2_START32 + S2_SECTORS
sy_room:
        cmp     eax,edx
        jb      sy_noroom
sy_roomok:

        mov     ecx,3Eh - 0Bh           ; the BPB span to preserve:
        cmp     byte ptr [src_type],32  ; through the FAT16 EBPB, or
        jne     sy_span                 ; the longer FAT32 one
        mov     ecx,5Ah - 0Bh
sy_span:
        mov     esi,[tgt_sec]           ; target's own numbers over
        add     esi,0Bh                 ; the source's boot code
        mov     edi,[src_sec]
        add     edi,0Bh
        rep     movsb

        mov     al,[tgt_log]            ; and down it goes
        mov     ebx,[src_sec]
        mov     ecx,1
        xor     edx,edx
        int     26h
        jc      sy_ioerr
        cmp     byte ptr [src_type],32  ; FAT32 keeps a backup at 6
        jne     sy_nobackup
        mov     al,[tgt_log]
        mov     ebx,[src_sec]
        mov     ecx,1
        mov     edx,6
        int     26h
        jc      sy_ioerr
sy_nobackup:

;--- stage 2, hard disks only ----------------------------------------
        cmp     byte ptr [src_type],12
        je      sy_files                ; floppies boot in one sector
        mov     edx,S2_START16
        cmp     byte ptr [src_type],32
        jne     sy_s2have
        mov     edx,S2_START32
sy_s2have:
        mov     [s2_lba],edx
        mov     al,[src_log]
        mov     ebx,[s2_buf]
        mov     ecx,S2_SECTORS
        int     25h
        jc      sy_ioerr
        mov     al,[tgt_log]
        mov     ebx,[s2_buf]
        mov     ecx,S2_SECTORS
        mov     edx,[s2_lba]
        int     26h
        jc      sy_ioerr

;--- the system files ------------------------------------------------
sy_files:
        mov     esi,OFFSET name_io
        mov     byte ptr [sys_attr],ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM
        mov     byte ptr [src_beside],0 ; the root, and nowhere else
        call    sy_copy
        jc      sy_nofile
        mov     esi,OFFSET name_dos
        call    sy_copy
        jc      sy_nofile
        ; RMDOS.SYS is the kernel's as well - the DOS it leaves in
        ; charge under Windows - and the kernel looks for it in the
        ; root of the boot drive before anywhere else, so it goes
        ; across with the kernel and wears the same attributes.  Hidden
        ; files are also the ones a COPY *.* leaves behind, which makes
        ; this the only thing that carries it.
        mov     byte ptr [src_beside],1 ; from here on: see the header
        mov     esi,OFFSET name_rm      ; optional - see the header
        call    sy_copyopt
        jc      sy_nofile
        mov     esi,OFFSET name_cmd
        mov     byte ptr [sys_attr],ATTR_ARCHIVE
        call    sy_copy
        jc      sy_nofile
        mov     esi,OFFSET name_c16     ; optional
        call    sy_copyopt
        jc      sy_nofile

        ; an RMCD.SYS from when SYS carried it: ordinary again
        mov     esi,OFFSET name_cd
        mov     edi,OFFSET tgt_path + 3
        call    sy_strcpy
        mov     edx,OFFSET tgt_path
        mov     ax,4300h
        int     21h
        jc      sy_cddone               ; the target has none
        test    cl,ATTR_SYSTEM
        jz      sy_cddone
        mov     edx,OFFSET tgt_path
        mov     ecx,ATTR_ARCHIVE
        mov     ax,4301h
        int     21h
sy_cddone:

        mov     edx,OFFSET msg_done
        mov     ah,09h
        int     21h
        mov     ax,4C00h
        int     21h

;--- help, which is not an error -------------------------------------
; Same text as the usage line an argument mistake prints, and NOT the
; same exit code: a batch file that asked for help got what it asked
; for, and 1 would tell it otherwise.
sy_help:
        mov     edx,OFFSET msg_usage
        mov     ah,09h
        int     21h
        mov     ax,4C00h
        int     21h

;--- errors ----------------------------------------------------------
sy_usage:
        mov     edx,OFFSET msg_usage
        jmp     short sy_die
sy_baddrv:
        mov     edx,OFFSET msg_baddrv
        jmp     short sy_die
sy_same:
        mov     edx,OFFSET msg_same
        jmp     short sy_die
sy_mismatch:
        mov     edx,OFFSET msg_mismatch
        jmp     short sy_die
sy_noroom:
        mov     edx,OFFSET msg_noroom
        jmp     short sy_die
sy_ioerr:
        mov     edx,OFFSET msg_ioerr
        jmp     short sy_die
sy_nofile:
        mov     edx,OFFSET msg_nofile
sy_die:
        mov     ah,09h
        int     21h
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; sy_copyopt - sy_copy for a file the source disk may not have.  A file
;              that is not there is not a failure, and the target's own
;              copy, if it has one, is left alone; a file that is there
;              and will not copy is a failure like any other.  It used
;              to be sy_copy with the carry ignored, and SYS then said
;              "System transferred" over a COMMAND.COM cut short by a
;              full disk.
;---------------------------------------------------------------------
sy_copyopt:
        call    sy_copy
        jnc     so_out
        cmp     byte ptr [src_absent],0
        je      so_bad                  ; it was there: CF stays set
        clc
        ret
so_bad:
        stc
so_out:
        ret

;---------------------------------------------------------------------
; sy_copy - ESI -> the bare 8.3 name; copy X:\name to Y:\name and
;           set [sys_attr] on the result.  With [src_beside] set, a
;           name the source's root has not got is looked for beside
;           the interpreter as well (sy_beside).  CF on any failure,
;           with [src_absent] saying whether the failure was only that
;           the source has no such file.
;---------------------------------------------------------------------
sy_copy:
        push    esi                     ; the name into both templates
        mov     edi,OFFSET src_path + 3
        call    sy_strcpy
        pop     esi
        push    esi
        mov     edi,OFFSET tgt_path + 3
        call    sy_strcpy
        pop     esi

        mov     byte ptr [src_absent],1
        mov     edx,OFFSET src_path
        mov     ax,3D00h
        int     21h
        jnc     sc_opened
        cmp     byte ptr [src_beside],0
        je      sc_fail
        call    sy_beside
        jc      sc_fail
        mov     edx,OFFSET alt_path
        mov     ax,3D00h
        int     21h
        jc      sc_fail
sc_opened:
        mov     byte ptr [src_absent],0
        movzx   eax,ax
        mov     [src_hdl],eax

        ; a previous system's file may sit there read-only
        mov     edx,OFFSET tgt_path
        mov     ecx,ATTR_ARCHIVE
        mov     ax,4301h
        int     21h                     ; failure just means "new"

        mov     edx,OFFSET tgt_path
        xor     ecx,ecx
        mov     ah,3Ch
        int     21h
        jc      sc_fail1
        movzx   eax,ax
        mov     [tgt_hdl],eax

sc_pump:
        mov     ebx,[src_hdl]
        mov     ecx,COPYBUF_SIZE
        mov     edx,[copy_buf]
        mov     ah,3Fh
        int     21h
        jc      sc_fail2
        or      eax,eax
        jz      sc_done
        mov     ecx,eax
        mov     ebx,[tgt_hdl]
        mov     edx,[copy_buf]
        mov     ah,40h
        int     21h
        jc      sc_fail2
        cmp     eax,ecx
        jb      sc_fail2                ; the target filled up
        jmp     short sc_pump

sc_done:
        mov     ebx,[tgt_hdl]
        mov     ah,3Eh
        int     21h
        mov     ebx,[src_hdl]
        mov     ah,3Eh
        int     21h
        mov     edx,OFFSET tgt_path     ; the promised attributes
        movzx   ecx,byte ptr [sys_attr]
        mov     ax,4301h
        int     21h
        clc
        ret

sc_fail2:
        mov     ebx,[tgt_hdl]
        mov     ah,3Eh
        int     21h
sc_fail1:
        mov     ebx,[src_hdl]
        mov     ah,3Eh
        int     21h
sc_fail:
        stc
        ret

;---------------------------------------------------------------------
; sy_beside - ESI -> a name: alt_path = that name in the directory the
;             interpreter was loaded from.  CF if the kernel will not
;             say where that is, or it is not on the source drive.
;
; ONLY ON THE SOURCE DRIVE.  The source is the disk whose system is
; being copied, and an interpreter loaded from another disk says
; nothing about where this one keeps its files.  It also means the
; file found can never be the one about to be written: the target is
; a different drive, which sy_entry has checked.
;---------------------------------------------------------------------
sy_beside:
        push    esi
        mov     edx,OFFSET alt_path
        mov     ax,0F103h               ; the interpreter's own path
        int     21h
        pop     esi
        jc      sb_no
        cmp     byte ptr [alt_path+1],':'
        jne     sb_no
        mov     al,[alt_path]
        cmp     al,'a'
        jb      sb_up
        cmp     al,'z'
        ja      sb_up
        sub     al,20h
sb_up:
        cmp     al,[src_path]
        jne     sb_no
        mov     edi,OFFSET alt_path     ; the name goes after the last '\'
        mov     edx,edi                 ; or ':'
sb_dir:
        mov     al,[edi]
        or      al,al
        jz      sb_name
        inc     edi
        cmp     al,'\'
        je      sb_mark
        cmp     al,':'
        jne     sb_dir
sb_mark:
        mov     edx,edi
        jmp     short sb_dir
sb_name:
        mov     edi,edx
        call    sy_strcpy
        clc
        ret
sb_no:
        stc
        ret

; sy_strcpy - ASCIIZ at ESI to EDI
sy_strcpy:
        push    eax
sy_sc1:
        mov     al,[esi]
        mov     [edi],al
        inc     esi
        inc     edi
        or      al,al
        jnz     sy_sc1
        pop     eax
        ret

;=====================================================================
; Data
;=====================================================================
src_sec     dd  0                       ; BSS carve-outs
tgt_sec     dd  0
s2_buf      dd  0
copy_buf    dd  0
s2_lba      dd  0
src_hdl     dd  0
tgt_hdl     dd  0
src_log     db  0                       ; 0-based logical drives
tgt_log     db  0
tgt_letter  db  0
src_type    db  0
sys_attr    db  0
src_absent  db  0                       ; sy_copy: the source had no such file
src_beside  db  0                       ; sy_copy: look beside the interpreter
                                        ; too

name_io     db  'PMIO.SYS',0
name_dos    db  'PMDOS.SYS',0
name_rm     db  'RMDOS.SYS',0
name_cd     db  'RMCD.SYS',0
name_cmd    db  'CMD.EXE',0
name_c16    db  'COMMAND.COM',0
src_path    db  'A:\'
            db  16 dup(0)
tgt_path    db  'A:\'
            db  16 dup(0)
alt_path    db  SHELLNAME_MAX + 16 dup(0)       ; sy_beside's answer

; ONE ARGUMENT, and it is the target: the source is the current drive,
; which has to be a system disk itself.  A help screen that showed
; MS-DOS's "SYS [drive1:][path] drive2:" would have a user typing
; SYS C: A: and making C: the target.
msg_usage   db  'Copies PM-DOS system files and command interpreter to a disk you specify.',13,10,13,10
            db  'SYS drive:',13,10,13,10
            db  '  drive:  Specifies the drive the files are to be copied to.',13,10
            db  '          The current drive is the source, and must be a system disk.',13,10,'$'
msg_baddrv  db  'Invalid drive specification',13,10,'$'
msg_same    db  'Source and target are the same drive',13,10,'$'
msg_mismatch db 'Source and target filesystems differ',13,10,'$'
msg_noroom  db  'No room for system on destination disk',13,10,'$'
msg_ioerr   db  'Disk error writing the system areas',13,10,'$'
msg_nofile  db  'Cannot copy the system files',13,10,'$'
msg_done    db  'System transferred',13,10,'$'

img_end:                                ; the BSS begins here

_SYS    ENDS
        END
