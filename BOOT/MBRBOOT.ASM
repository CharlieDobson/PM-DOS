;=====================================================================
; MBRBOOT.ASM - PM-DOS master boot record
;
; The BIOS loads this at 0000:7C00 with DL = drive.  It copies itself
; down to 0060:0000 (the classic dodge, so the volume boot record can
; be loaded at 7C00 where it expects to run), scans the partition
; table for the active entry, reads that partition's first sector
; using the CHS fields MKDISK filled in, and jumps to it with DL
; intact.
;
; MKDISK -mbr takes the first 1BEh bytes of this file (the code) and
; supplies its own partition table and signature, so the table here
; is just a placeholder.
;
; Linked with:  wlink form raw bin option offset=0
; All data references go through DS = 0060h after relocation.
;=====================================================================
        .8086

RELSEG          EQU     0060h           ; where the copy runs

_MBR    SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_MBR, DS:_MBR
        ORG     0

start:
        cli
        xor     ax,ax
        mov     ds,ax
        mov     es,ax
        mov     ss,ax
        mov     sp,7C00h
        sti
        cld

        mov     si,7C00h                ; move ourselves out of the way
        mov     di,RELSEG * 16
        mov     cx,256
        rep     movsw

        db      0EAh                    ; JMP FAR RELSEG:relocated
        dw      OFFSET relocated
        dw      RELSEG

relocated:
        mov     ax,cs                   ; OFFSETs are RELSEG-relative now
        mov     ds,ax

        ; Only a hard disk has a partition table, so a DL below 80h is
        ; a lying loader, not a floppy.  (DOSBox 0.74's "boot -l c"
        ; hands us DL=0, which would send every read to drive A:.)
        cmp     dl,80h
        jae     drv_ok
        mov     dl,80h
drv_ok:
        mov     [bootdrv],dl
IFDEF MBR_DEBUG
        mov     al,'['
        call    putc
        mov     al,dl
        call    puthex
        mov     al,']'
        call    putc
ENDIF

;--- find the active partition ---------------------------------------
        mov     si,OFFSET parttab
        mov     cx,4
scan:
        cmp     byte ptr [si],80h
        je      have
        add     si,16
        loop    scan
        mov     si,OFFSET msgNoAct
        jmp     short fatal

;--- load its boot sector at 0000:7C00 -------------------------------
have:
        mov     di,3                    ; retries
rdlp:
        push    di
        mov     dh,[si+1]               ; CHS start straight from the
        mov     cl,[si+2]               ; entry: head, sector+cyl hi,
        mov     ch,[si+3]               ; cyl low
        mov     dl,[bootdrv]
        xor     bx,bx
        mov     es,bx
        mov     bx,7C00h
        mov     ax,0201h
        int     13h
        pop     di
        jnc     rdok
        xor     ax,ax                   ; reset the disk and retry
        mov     dl,[bootdrv]
        int     13h
        dec     di
        jnz     rdlp
        mov     si,OFFSET msgDisk
        jmp     short fatal

rdok:
IFDEF MBR_DEBUG
        mov     al,'<'
        call    putc
        mov     al,es:[7DFFh]
        call    puthex
        mov     al,es:[7DFEh]
        call    puthex
        mov     al,'>'
        call    putc
ENDIF
        cmp     word ptr es:[7DFEh],0AA55h
        jne     notsys
        mov     dl,[bootdrv]
        db      0EAh                    ; JMP FAR 0000:7C00
        dw      7C00h
        dw      0

notsys:
        mov     si,OFFSET msgNoSys
fatal:
        call    puts
        xor     ax,ax
        int     16h                     ; any key ...
        int     19h                     ; ... and try the next device

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

IFDEF MBR_DEBUG
putc:
        push    ax
        push    bx
        mov     ah,0Eh
        mov     bx,0007h
        int     10h
        pop     bx
        pop     ax
        ret
puthex:
        push    ax
        push    cx
        mov     ah,al
        mov     cl,4
        shr     al,cl
        call    puthex1
        mov     al,ah
        and     al,0Fh
        call    puthex1
        pop     cx
        pop     ax
        ret
puthex1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph1
        add     al,7
ph1:
        jmp     short putc
ENDIF

;---------------------------------------------------------------------
msgNoAct    db  13,10,'No active partition',0
msgDisk     db  13,10,'Disk read error',0
msgNoSys    db  13,10,'Missing operating system',0
bootdrv     db  0

;---------------------------------------------------------------------
mbr_code_end:                           ; MKDISK copies 0..1BDh verbatim
IF (mbr_code_end - start) GT 1BEh
.ERR    ; MBR code has grown into the partition table
ENDIF

        ORG     1BEh                    ; placeholder partition table -
parttab db      64 dup(0)               ; MKDISK writes the real one
        dw      0AA55h
_MBR    ENDS
        END     start
