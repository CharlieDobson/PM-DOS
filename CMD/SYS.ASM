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
;   3. PMIO.SYS, PMDOS.SYS and COMMAND.COM, copied as ordinary
;      files, the first two re-marked read-only+hidden+system.
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
        D32_HEAD PROG_BASE,<OFFSET sy_entry>,<1024 + (S2_SECTORS * 512) + COPYBUF_SIZE>,4096

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
        mov     esi,PROG_PSP + PSP_TAIL
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
        call    sy_copy
        jc      sy_nofile
        mov     esi,OFFSET name_dos
        call    sy_copy
        jc      sy_nofile
        mov     esi,OFFSET name_cmd
        mov     byte ptr [sys_attr],ATTR_ARCHIVE
        call    sy_copy
        jc      sy_nofile

        mov     edx,OFFSET msg_done
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
; sy_copy - ESI -> the bare 8.3 name; copy X:\name to Y:\name and
;           set [sys_attr] on the result.  CF on any failure.
;---------------------------------------------------------------------
sy_copy:
        push    esi                     ; the name into both templates
        mov     edi,OFFSET src_path + 3
        call    sy_strcpy
        pop     esi
        mov     edi,OFFSET tgt_path + 3
        call    sy_strcpy

        mov     edx,OFFSET src_path
        mov     ax,3D00h
        int     21h
        jc      sc_fail
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

name_io     db  'PMIO.SYS',0
name_dos    db  'PMDOS.SYS',0
name_cmd    db  'COMMAND.COM',0
src_path    db  'A:\'
            db  16 dup(0)
tgt_path    db  'A:\'
            db  16 dup(0)

msg_usage   db  'Usage: SYS d:',13,10,'$'
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
