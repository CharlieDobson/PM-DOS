;=====================================================================
; BOOT2.ASM - PM-DOS hard-disk boot loader, stage 2
;
; Stage 1 (the volume boot record) reads this out of the partition's
; reserved sectors to 0000:8000 and jumps here with DL = BIOS drive
; and the VBR still resident at 0000:7C00, where FATLOAD finds the BPB.
;
; The job: find PMIO.SYS in the root directory and load it at
; 2000:0000 by FOLLOWING ITS CLUSTER CHAIN, then chain to it exactly
; as the floppy boot sector does.  Nothing on the volume needs to be
; contiguous.  One binary serves FAT16 and FAT32 - and FAT12, though
; nothing currently boots that way - because FATLOAD works the type
; out from the cluster count at run time.
;
; Linked with:  wlink form raw bin option offset=0x8000
; (a 4K multiple, so wlink's silent round-up of a non-4K offset does
; not bite), and run with DS = ES = SS = 0 so those absolute offsets
; address correctly.
;=====================================================================
        .386

        INCLUDE PMDOS.INC

LOADSEG         EQU     2000h           ; PMIO.SYS -> 2000:0000

_S2     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_S2, DS:_S2, ES:NOTHING
        ORG     0

;=====================================================================
s2_entry:
        cli
        xor     ax,ax                   ; we are linked absolute from 0
        mov     ds,ax
        mov     es,ax
        mov     ss,ax
        mov     sp,RM_STACK_TOP
        sti
        cld

        mov     [fl_drive],dl
        call    fl_init

        mov     si,OFFSET ioname
        call    fl_find
        jc      s2_nofile

;--- follow the chain into 2000:0000 ---------------------------------
        mov     word ptr [s2_seg],LOADSEG
        mov     word ptr [s2_off],0
        mov     eax,[fl_clus]
s2_lp:
        cmp     eax,2
        jb      s2_done
        cmp     eax,[fl_eoc]
        jae     s2_done

        push    eax
        call    fl_c2l                  ; EAX = the cluster's first sector
        mov     ecx,[fl_spc]
        mov     es,[s2_seg]
        mov     bx,[s2_off]
        call    fl_read                 ; ECX sectors, BX advanced
        jc      s2_diskerr

        xor     dx,dx                   ; back to segment 0 for the
        mov     es,dx                   ; loader's own buffers
        mov     [s2_off],bx
        pop     eax
        call    fl_next
        jmp     short s2_lp

s2_done:
        mov     dl,[fl_drive]
        db      0EAh                    ; JMP FAR LOADSEG:0000
        dw      0
        dw      LOADSEG

s2_nofile:
        mov     si,OFFSET msgNoFile
        jmp     short s2_fatal
s2_diskerr:
        pop     eax
        mov     si,OFFSET msgDisk
s2_fatal:
        call    s2_puts
        mov     si,OFFSET msgHalt
        call    s2_puts
        xor     ax,ax
        int     16h                     ; wait for a key ...
        int     19h                     ; ... and try booting again

;---------------------------------------------------------------------
s2_puts:
        push    ax
        push    bx
        mov     bx,0007h
sp_lp:
        lodsb
        or      al,al
        jz      sp_done
        mov     ah,0Eh
        int     10h
        jmp     short sp_lp
sp_done:
        pop     bx
        pop     ax
        ret

;=====================================================================
        INCLUDE FATLOAD.INC

;=====================================================================
ioname      db  'PMIO    SYS'   ; 8.3 padded to exactly 11 bytes
msgNoFile   db  13,10,'PM-DOS: PMIO.SYS not found',0
msgDisk     db  13,10,'PM-DOS: disk error',0
msgHalt     db  13,10,'Press a key to reboot',13,10,0

s2_seg      dw  0
s2_off      dw  0

;---------------------------------------------------------------------
; Stage 1 reads a fixed number of sectors, so overrunning them would
; load a truncated stage 2 and execute whatever followed it.
;---------------------------------------------------------------------
s2_end:
IF (s2_end - s2_entry) GT (S2_SECTORS * 512)
.ERR    ; stage 2 no longer fits in S2_SECTORS sectors
ENDIF

_S2     ENDS
        END     s2_entry
