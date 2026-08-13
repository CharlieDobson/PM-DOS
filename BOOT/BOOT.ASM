;=====================================================================
; BOOT.ASM - PM-DOS FAT12 boot sector (1.44M / 720K floppy)
;
; Loaded by the BIOS at 0000:7C00 with DL = BIOS drive number.
; Locates PMIO.SYS in the root directory, follows its FAT12 cluster
; chain into 2000:0000, and transfers control there with DL intact.
;
; Linked with:  wlink form raw bin option offset=0
; All data references go through DS = 07C0h, so OFFSET values are
; segment-relative and need no load-address fixup at all.
;
; Limitation: uses CHS INT 13h and 16-bit LBA arithmetic, so this
; sector is for floppies only.  A FAT16/LBA sector for hard disks is
; a separate file (not yet written).
;=====================================================================
        .8086

FATSEG          EQU     07E0h           ; FAT image      -> 0000:7E00
ROOTSEG         EQU     0900h           ; root directory -> 0000:9000
LOADSEG         EQU     2000h           ; PMIO.SYS      -> 2000:0000

_BOOT   SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_BOOT, DS:_BOOT, ES:_BOOT
        ORG     0

;---------------------------------------------------------------------
; Jump over the BPB, then the BPB itself.  PUTBOOT preserves bytes
; 0Bh..3Dh from the target disk so a real format's geometry wins.
;---------------------------------------------------------------------
start:
        jmp     short begin
        nop

bsOEM           db      'PM-DOS  '
bpbBytesPerSec  dw      512
bpbSecPerClus   db      1
bpbRsvdSec      dw      1
bpbNumFATs      db      2
bpbRootEnts     dw      224
bpbTotalSec     dw      2880
bpbMedia        db      0F0h
bpbSecPerFAT    dw      9
bpbSecPerTrk    dw      18
bpbHeads        dw      2
bpbHidden       dd      0
bpbTotalSecBig  dd      0
bsDriveNum      db      0
bsReserved      db      0
bsBootSig       db      29h
bsVolID         dd      19260806h
bsVolLabel      db      'PM-DOS     '
bsFileSys       db      'FAT12   '

;---------------------------------------------------------------------
begin:
        cli
        mov     ax,07C0h                ; all our data is DS-relative
        mov     ds,ax
        xor     ax,ax
        mov     es,ax
        mov     ss,ax
        mov     sp,7C00h
        sti

        mov     [bootdrv],dl

        ; Nothing is printed on the way through.  A boot sector that
        ; succeeds has nothing to say - MS-DOS's said nothing either -
        ; and the first message a working machine shows is PMIO's, on
        ; the screen it has just cleared.  The error paths below still
        ; speak, because those are the only ones anybody needs to read.

;--- read the first FAT into 0000:7E00 -------------------------------
        mov     ax,[bpbRsvdSec]
        mov     cx,[bpbSecPerFAT]
        mov     bx,FATSEG
        mov     es,bx
        xor     bx,bx
        call    readmany

;--- read the root directory into 0000:9000 --------------------------
        ; first root sector = RsvdSec + NumFATs * SecPerFAT
        mov     al,[bpbNumFATs]
        xor     ah,ah
        mul     word ptr [bpbSecPerFAT]
        add     ax,[bpbRsvdSec]
        push    ax

        ; root sectors = RootEnts * 32 / BytesPerSec  (assumes 512)
        mov     ax,[bpbRootEnts]
        mov     cl,4
        shr     ax,cl
        mov     cx,ax

        pop     ax
        push    ax                      ; keep first root sector
        push    cx                      ; keep root sector count
        mov     bx,ROOTSEG
        mov     es,bx
        xor     bx,bx
        call    readmany

        ; data area starts right after the root directory
        pop     cx
        pop     ax
        add     ax,cx
        mov     [datastart],ax

;--- search the root directory for PMIO.SYS -------------------------
        mov     ax,ROOTSEG
        mov     es,ax
        xor     di,di
        mov     cx,[bpbRootEnts]
srchlp:
        push    cx
        push    di
        mov     si,OFFSET fname
        mov     cx,11
        repe    cmpsb
        pop     di
        pop     cx
        je      found
        add     di,32
        cmp     byte ptr es:[di],0      ; end of used entries?
        je      nofile
        loop    srchlp
nofile:
        mov     si,OFFSET msgNoFile
        jmp     fatal

found:
        mov     ax,es:[di+1Ah]          ; starting cluster
        mov     [cluster],ax

;--- walk the cluster chain into 2000:0000 ---------------------------
        mov     ax,LOADSEG
        mov     es,ax
        xor     bx,bx
loadlp:
        mov     ax,[cluster]
        cmp     ax,0FF8h
        jae     loaded

        ; LBA = datastart + (cluster - 2) * SecPerClus
        sub     ax,2
        mov     cl,[bpbSecPerClus]
        xor     ch,ch
        push    cx
        mul     cx
        add     ax,[datastart]
        pop     cx
        call    readmany                ; advances BX by CX*512

        ; next cluster: FAT12 entry at (cluster + cluster/2)
        mov     ax,[cluster]
        mov     si,ax
        shr     si,1
        add     si,ax                   ; SI = byte offset into the FAT
        push    es
        mov     dx,FATSEG
        mov     es,dx
        mov     dx,es:[si]
        pop     es
        test    ax,1
        jz      even_ent
        mov     cl,4
        shr     dx,cl                   ; odd cluster: high 12 bits
        jmp     short store_ent
even_ent:
        and     dx,0FFFh                ; even cluster: low 12 bits
store_ent:
        mov     [cluster],dx
        jmp     short loadlp

loaded:
        mov     dl,[bootdrv]
        db      0EAh                    ; JMP FAR LOADSEG:0000
        dw      0
        dw      LOADSEG

;---------------------------------------------------------------------
; readmany - read CX sectors starting at LBA AX into ES:BX.
;            BX is advanced past the data.  All else preserved.
;---------------------------------------------------------------------
readmany:
        push    ax
        push    cx
rmlp:
        call    readsec
        add     bh,2                    ; = BX + 512, one byte shorter, and
        jnc     rmnc                    ; carries out of BX just the same
        ; BX wrapped - step ES forward a full 64K so >64K loads work
        push    ax
        mov     ax,es
        add     ax,1000h
        mov     es,ax
        pop     ax
rmnc:
        inc     ax
        loop    rmlp
        pop     cx
        pop     ax
        ret

;---------------------------------------------------------------------
; readsec - read one sector, LBA in AX, into ES:BX.  Retries 3 times.
;---------------------------------------------------------------------
readsec:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     di,3
rslp:
        push    di
        push    ax

        xor     dx,dx
        div     word ptr [bpbSecPerTrk] ; AX = track, DX = sector-1
        inc     dl
        mov     cl,dl                   ; CL = sector
        xor     dx,dx
        div     word ptr [bpbHeads]     ; AX = cylinder, DX = head
        mov     dh,dl                   ; DH = head
        mov     ch,al                   ; CH = cylinder (< 256 on floppy)
        mov     dl,[bootdrv]
        mov     ax,0201h
        int     13h
        pop     ax
        pop     di
        jnc     rsok

        push    ax
        xor     ax,ax                   ; reset the controller and retry
        mov     dl,[bootdrv]
        int     13h
        pop     ax
        dec     di
        jnz     rslp

        mov     si,OFFSET msgDisk
        jmp     short fatal
rsok:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
fatal:
        call    puts
        mov     si,OFFSET msgHalt
        call    puts
        xor     ax,ax
        int     16h                     ; wait for a key
        int     19h                     ; and try booting again

;---------------------------------------------------------------------
; puts - write the ASCIIZ string at DS:SI via the BIOS teletype call.
;---------------------------------------------------------------------
puts:
        push    ax
        push    bx
        mov     bx,0007h
putlp:
        lodsb
        or      al,al
        jz      putdone
        mov     ah,0Eh
        int     10h
        jmp     short putlp
putdone:
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
fname       db  'PMIO    SYS'   ; 8.3 padded to exactly 11 bytes
msgNoFile   db  13,10,'PMIO.SYS not found',0
msgDisk     db  13,10,'Disk error',0
msgHalt     db  13,10,'Press a key to reboot',13,10,0

bootdrv     db  0
cluster     dw  0
datastart   dw  0

;---------------------------------------------------------------------
; Size guard.  The limit is 200h, not 1FEh, because the last two bytes
; of the sector ARE datastart: the 0AA55h signature below overwrites its
; zero initialiser.  That is harmless - datastart is written (from the
; root directory size) before it is ever read - but one more byte would
; push a live variable past the end of the loaded sector and into the
; FAT buffer at 0000:7E00, where the chain walk would corrupt it.
boot_end:
IF (boot_end - start) GT 200h
.ERR    ; the sector has overflowed - see the note above
ENDIF

        ORG     1FEh
        dw      0AA55h
_BOOT   ENDS
        END     start
