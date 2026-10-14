;=====================================================================
; DRVSTUB.ASM - DRVSTUB.BIN, the MS-DOS driver in front of every
;               32-bit PM-DOS driver (INC\NDRV.INC)
;
; What real MS-DOS runs when its CONFIG.SYS names a PM-DOS driver.
; DOS loads a .SYS as a device driver and calls the header at byte
; zero, so the first 128 bytes of every 32-bit driver file are this:
; a BLOCK device driver whose INIT prints one sentence and reports no
; units - the one answer that makes every DOS from 3.0 on drop a
; driver quietly and free its memory (SYSINIT's erase_dev_do,
; S:\DOS6SRC\bios\sysconf.asm).  A character driver cannot refuse to
; install; a block driver with nothing to offer can, and nothing is
; said but the sentence here.
;
; PM-DOS knows the file by the name in this header, which DOS never
; reads: a block driver's name field is where DOS WRITES the unit
; count, and only when there is one.  The D32 image starts at byte
; 128 (D32_DRVSTUB in PMDOS.INC); TOOLS\D32DRV puts it there.
;
; Linked with:  wlink form raw bin option offset=0x0
;=====================================================================
        .386
_DS     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_DS, DS:_DS, ES:_DS, SS:_DS
        ORG     0

;--- the device header: a block device, nothing special ---------------
header:
        dd      0FFFFFFFFh              ; the only driver in the file
        dw      0000h                   ; block device
        dw      strat
        dw      intr
        db      'PMD32DRV'              ; DRVSTUB_NAME: what PM-DOS looks for

reqptr  dd      0                       ; what strategy remembers

strat:
        mov     word ptr cs:[reqptr],bx
        mov     word ptr cs:[reqptr+2],es
        retf

intr:
        push    ax
        push    bx
        push    dx
        push    ds
        push    es
        les     bx,cs:[reqptr]
        mov     ax,8103h                ; error, unknown command
        cmp     byte ptr es:[bx+2],0    ; INIT?
        jne     done
        push    cs
        pop     ds
        mov     dx,OFFSET msg
        mov     ah,09h
        int     21h
        mov     byte ptr es:[bx+0Dh],0  ; units: none
        mov     word ptr es:[bx+0Eh],0  ; break address: keep nothing
        mov     word ptr es:[bx+10h],cs
        mov     ax,0100h                ; done
done:
        mov     word ptr es:[bx+3],ax
        pop     es
        pop     ds
        pop     dx
        pop     bx
        pop     ax
        retf

msg     db      'This driver needs PM-DOS.',13,10,'$'

;--- exactly 128 bytes: the image follows ------------------------------
        ORG     7Fh
        db      0
_DS     ENDS
        END
