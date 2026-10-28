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
GPT_BUF         EQU     0200h           ; where a GPT sector is read: just past
                                        ; the copy, and behind it
GPT_ALBA        EQU     0400h           ; dq - the array's sector being read
GPT_LEFT        EQU     0408h           ; dw - entries not looked at yet
GPT_ESIZE       EQU     040Ch           ; dw - bytes an entry

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
        jmp     gpt_look

;--- load its boot sector at 0000:7C00 -------------------------------
; unless it is the protective entry of a GPT disk, marked active for
; the BIOSes that will not start a disk with no active entry
have:
        cmp     byte ptr [si+4],0EEh
        jne     have_mbr
        jmp     gpt_disk
have_mbr:
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

;--- no active entry: a GPT disk? ------------------------------------
; A protective entry (EEh) says the partitions are in the GPT, and the
; one to start is the first entry with its legacy BIOS bootable bit set
; (attribute bit 2), which is what FDISK /A sets on a GPT disk.  The
; GPT is read through the INT 13h extensions, by LBA; a BIOS without
; them cannot start a GPT disk from here.  The volume boot record that
; is started finds itself by its own BPB, as it does on an MBR disk.
gpt_look:
        cmp     byte ptr [parttab+4],0EEh       ; where the specification
        mov     si,OFFSET msgNoAct              ; puts the protective entry
        jne     fatal
gpt_disk:
        mov     ah,41h
        mov     bx,55AAh
        mov     dl,[bootdrv]
        int     13h
        jc      gpt_err
        cmp     bx,0AA55h               ; and said so
        jne     gpt_err
        push    ds
        pop     es
        inc     word ptr [dap_lba]      ; the header, LBA 1 (0 until now)
        call    gpt_read
        jc      gpt_err
        cmp     word ptr ds:[GPT_BUF],'FE'      ; "EFI PART"
        jne     gpt_none
        mov     si,GPT_BUF + 48h        ; where the array is, how many
        mov     di,GPT_ALBA             ; entries, and how big each is
        mov     cx,7
        rep     movsw
        mov     ax,ds:[GPT_ESIZE]       ; 128, 256 or 512 bytes, so the
        cmp     ax,80h                  ; entries tile each sector
        je      gpt_sector
        cmp     ax,100h
        je      gpt_sector
        cmp     ax,200h
        je      gpt_sector
; the two ways out, in the middle where every jump to them reaches
gpt_none:
        mov     si,OFFSET msgNoAct
        jmp     fatal
gpt_err:
        mov     si,OFFSET msgDisk
        jmp     fatal
gpt_sector:
        mov     si,GPT_ALBA
        mov     di,OFFSET dap_lba
        mov     cx,4
        rep     movsw
        call    gpt_read
        jc      gpt_err
        mov     si,GPT_BUF
gpt_entry:
        cmp     word ptr ds:[GPT_LEFT],0
        je      gpt_none
        test    byte ptr [si+30h],4     ; legacy BIOS bootable
        jnz     gpt_found
        dec     word ptr ds:[GPT_LEFT]
        add     si,ds:[GPT_ESIZE]
        cmp     si,GPT_BUF + 512
        jb      gpt_entry
        add     word ptr ds:[GPT_ALBA],1        ; the array's next sector
        adc     word ptr ds:[GPT_ALBA+2],0
        jmp     short gpt_sector
gpt_found:
        add     si,20h                  ; its first sector, to 0000:7C00 -
        mov     di,OFFSET dap_lba       ; which is RELSEG:7600h
        mov     cx,4
        rep     movsw
        mov     word ptr [dap_buf],7C00h - RELSEG * 16
        call    gpt_read
        jc      gpt_err
        xor     ax,ax
        mov     es,ax
        jmp     rdok

;--- gpt_read - the sector at dap_lba into dap_buf.  CF on failure.
gpt_read:
        push    si
        mov     si,OFFSET dap
        mov     ah,42h
        mov     dl,[bootdrv]
        int     13h
        pop     si
        ret

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

; the INT 13h extensions' packet: one sector, to GPT_BUF while the GPT
; is read, and to 0000:7C00 at the end.  Its LBA's high words stay zero.
dap         db  10h, 0
            dw  1
dap_buf     dw  GPT_BUF, RELSEG
dap_lba     dw  0, 0, 0, 0

;---------------------------------------------------------------------
mbr_code_end:                           ; MKDISK copies 0..1BDh verbatim,
                                        ; but FDISK /MBR writes 0..1B7h: 1B8h
                                        ; on is the disk's identity
.ERRE   (mbr_code_end - start) LE 1B8h, <MBR code has grown into the disk signature>

        ORG     1BEh                    ; placeholder partition table -
parttab db      64 dup(0)               ; MKDISK writes the real one
        dw      0AA55h
_MBR    ENDS
        END     start
