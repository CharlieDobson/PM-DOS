;=====================================================================
; BOOTHD.ASM - PM-DOS hard-disk volume boot record, stage 1
;
;   wasm boothd.asm              -> FAT16 VBR, code starts at 3Eh
;   wasm -dFAT32BOOT boothd.asm  -> FAT32 VBR, code starts at 5Ah
;
; Loaded at 0000:7C00 by the MBR with DL = BIOS drive number.  All it
; does is read stage 2 out of the partition's RESERVED sectors and
; jump to it; stage 2 (BOOT2.ASM) has the room to walk a FAT.
;
; Why: a 512-byte sector cannot hold a FAT-walking loader once the BPB
; (3Eh bytes, 5Ah on FAT32) and the error path are paid for - measured
; at 690 bytes for FAT32 against 420 available.  Microsoft hit the
; same wall and put their FAT32 boot record's continuation in sector
; 2 of the reserved area.  This does the same, and gains a stage 2
; that is identical for both filesystems.
;
; Stage 2 lives in reserved sectors rather than in a file so that
; nothing has to be found before the FAT walker exists.
;
; Linked with:  wlink form raw bin option offset=0
; All data references go through DS = 07C0h.
;=====================================================================
        .386

        INCLUDE PMDOS.INC

IFDEF FAT32BOOT
S2_START        EQU     S2_START32
BPB_SIZE        EQU     5Ah
ELSE
S2_START        EQU     S2_START16
BPB_SIZE        EQU     3Eh
ENDIF

_BOOT   SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_BOOT, DS:_BOOT, ES:NOTHING
        ORG     0

start:
        jmp     short begin
        nop

;---------------------------------------------------------------------
; BPB template.  MKDISK overwrites all of it; only its length matters
; here, because that is where the code begins.
;---------------------------------------------------------------------
bsOEM           db      'PM-DOS  '
bpbBytesPerSec  dw      512
bpbSecPerClus   db      2
bpbRsvdSec      dw      9
bpbNumFATs      db      2
bpbRootEnts     dw      512
bpbTotalSec     dw      0
bpbMedia        db      0F8h
bpbSecPerFAT    dw      128
bpbSecPerTrk    dw      63
bpbHeads        dw      16
bpbHidden       dd      63
bpbTotalSecBig  dd      0
IFDEF FAT32BOOT
bpbSecPerFAT32  dd      0
bpbExtFlags     dw      0
bpbFSVersion    dw      0
bpbRootClus     dd      2
bpbFSInfo       dw      1
bpbBkBoot       dw      6
bpbReserved     db      12 dup(0)
ENDIF
bsDriveNum      db      80h
bsReserved      db      0
bsBootSig       db      29h
bsVolID         dd      19260806h
bsVolLabel      db      'PM-DOS     '
IFDEF FAT32BOOT
bsFileSys       db      'FAT32   '
ELSE
bsFileSys       db      'FAT16   '
ENDIF

; MKDISK splices its own BPB in over exactly this span, so a drift
; here would have it cutting into code instead.
IF ($ - start) - BPB_SIZE
.ERR    ; BPB is not BPB_SIZE bytes
ENDIF

;=====================================================================
begin:
        cli
        mov     ax,07C0h                ; all our data is DS-relative
        mov     ds,ax
        xor     ax,ax
        mov     es,ax
        mov     ss,ax
        mov     sp,7C00h
        sti
        cld

        ; This sector only ever lives on a hard disk, so a DL below
        ; 80h came from a loader that never set it (DOSBox 0.74 boots
        ; hard-disk images with DL=0, which would send every read to
        ; drive A: and look exactly like a corrupt volume).
        cmp     dl,80h
        jae     drv_ok
        mov     dl,80h
drv_ok:
        mov     [bootdrv],dl

;--- read stage 2 out of the reserved area ---------------------------
        mov     eax,[bpbHidden]
        add     eax,S2_START
        mov     bx,OFFSET S2_BASE
        mov     cx,S2_SECTORS
s1_lp:
        push    cx
        push    eax
        call    read1
        pop     eax
        pop     cx
        inc     eax
        add     bx,512
        loop    s1_lp

        mov     dl,[bootdrv]
        db      0EAh                    ; JMP FAR 0000:S2_BASE
        dw      S2_BASE
        dw      0

;---------------------------------------------------------------------
; read1 - read one sector, LBA in EAX, into ES:BX.  Preserves all.
;         Retries three times, resetting the controller between tries.
;---------------------------------------------------------------------
read1:
        pushad
        mov     di,3
r1_try:
        push    di
        push    eax                     ; INT 13h returns status in AX,
                                        ; which would otherwise eat the LBA
        movzx   ecx,word ptr [bpbSecPerTrk]
        xor     edx,edx
        div     ecx                     ; EAX = track, EDX = sector-1
        mov     cl,dl
        inc     cl                      ; CL = sector, 1-based
        movzx   esi,word ptr [bpbHeads]
        xor     edx,edx
        div     esi                     ; EAX = cylinder, EDX = head
        mov     dh,dl
        mov     dl,[bootdrv]
        mov     ch,al                   ; cylinder bits 0-7
        shr     eax,8
        and     al,3
        shl     al,6                    ; cylinder bits 8-9 -> CL bits 7-6
        or      cl,al
        mov     ax,0201h
        int     13h
        pop     eax
        pop     di
        jnc     r1_ok

        push    ax                      ; the reset call eats AX as well
        xor     ax,ax
        mov     dl,[bootdrv]
        int     13h
        pop     ax
        dec     di
        jnz     r1_try

;--- unrecoverable ---------------------------------------------------
        mov     si,OFFSET msgErr
err_lp:
        lodsb
        or      al,al
        jz      err_wait
        mov     ah,0Eh
        mov     bx,0007h
        int     10h
        jmp     short err_lp
err_wait:
        xor     ax,ax
        int     16h                     ; wait for a key ...
        int     19h                     ; ... and try booting again
r1_ok:
        popad
        ret

;---------------------------------------------------------------------
msgErr      db  13,10,'PM-DOS: disk error',13,10,0
bootdrv     db  0

;---------------------------------------------------------------------
boot_end:
IF (boot_end - start) GT 1FEh
.ERR    ; stage 1 has overflowed past the signature
ENDIF

        ORG     1FEh
        dw      0AA55h
_BOOT   ENDS
        END     start
