;=====================================================================
; CDEX.ASM - CDEX.EXE, the CD-ROM drives and their letters
;
;   CDEX            one line per drive: the letter, the unit number
;                   MSCD001 and INT 2Fh AH=15h count it by, where it
;                   is on the IDE bus, what is in it, and its name
;
; THERE IS NO MSCDEX.EXE ON THIS MACHINE AND THIS IS NOT ONE.  The
; extensions are in the kernel (MSCDEX.INC answers the API, MSCD001.INC
; is the device) and a CD-ROM has a real drive letter the moment the
; scan finds the drive.  What MSCDEX printed when it loaded - "Drive
; D: = Driver MSCD001 unit 0" - is the one thing a user still wants
; from it: which letter is which drive.  This prints that, and the
; things MSCDEX could not know because it only saw a device driver:
; where the drive is, what IDENTIFY called it, and whether there is a
; disc in it and what the disc is called.
;
; What it asks:
;   21h/F1h AL=0     the kernel's version
;   21h/F1h AL=6     the volume table, letter by letter: a DRVF_CDROM
;                    entry's DRV_BIOS is the ATAPI unit number
;   21h/F1h AL=17h   that unit - its place on the bus and its name,
;                    answered with the tray open
;   21h/F1h AL=1     mounts the letter, which is how "is there a
;                    disc" is asked; with none the kernel raises a
;                    critical error, which crit_fail answers FAIL
;   21h/4Eh          the volume label, which the ISO9660 reader
;                    synthesises from the volume id
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

LETTERS         EQU     26
MODEL_W         EQU     33              ; what fits on the line
DISC_W          EQU     12
LOC_W           EQU     17

_CDX    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_CDX, DS:_CDX, ES:_CDX, SS:_CDX
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET cdx_entry>,<0>,4096

cdx_entry:
;--- /? is the only argument there is ---------------------------------
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
ca_lp:
        or      ecx,ecx
        jz      ca_done
        mov     al,[esi]
        cmp     al,0Dh
        je      ca_done
        cmp     al,' '
        je      ca_next
        cmp     al,9
        je      ca_next
        cmp     al,'/'
        jne     ca_bad
        cmp     byte ptr [esi+1],'?'
        je      ca_help
ca_bad:
        mov     edx,OFFSET msg_badparm
        call    print
        mov     ax,4C01h
        int     21h
ca_help:
        mov     edx,OFFSET msg_usage
        call    print
        mov     ax,4C00h
        int     21h
ca_next:
        inc     esi
        dec     ecx
        jmp     short ca_lp
ca_done:

;--- the critical-error handler, before a drive is touched -----------
; Mounting a CD-ROM with nothing in it is a critical error, and
; without this it is "Not ready reading drive D / Abort, Retry, Fail?"
; in the middle of the table.  FAIL is the report.
        mov     edx,OFFSET crit_fail
        mov     eax,2524h
        int     21h
        mov     edx,OFFSET dta
        mov     ah,1Ah
        int     21h

        call    banner

;--- how many units are there at all? --------------------------------
        xor     edx,edx
        mov     edi,OFFSET cdu
        mov     eax,0F117h
        int     21h                             ; ECX is valid either way
        mov     [n_units],ecx
        or      ecx,ecx
        jnz     cx_some
        mov     edx,OFFSET msg_none
        call    print
        mov     ax,4C01h
        int     21h
cx_some:
        mov     edx,OFFSET msg_head
        call    print

;--- the letters, in order ---------------------------------------------
        xor     ebx,ebx                         ; EBX = the letter
cx_letter:
        cmp     ebx,LETTERS
        jae     cx_letters_done
        mov     edx,ebx
        mov     edi,OFFSET drv
        mov     eax,0F106h
        int     21h
        jc      cx_letters_done                 ; past the table
        mov     al,[drv+DRV_FLAGS]
        test    al,DRVF_PRESENT
        jz      cx_lnext
        test    al,DRVF_CDROM
        jz      cx_lnext
        test    al,DRVF_IMAGE                   ; an .ISO somebody mounted is
        jnz     cx_lnext                        ; a file, not a drive
        movzx   eax,byte ptr [drv+DRV_BIOS]     ; the unit
        cmp     eax,LETTERS
        jae     cx_lnext
        mov     byte ptr [unit_seen+eax],1
        mov     [row_letter],bl
        mov     [row_unit],al
        call    row
cx_lnext:
        inc     ebx
        jmp     short cx_letter
cx_letters_done:

;--- and any unit no letter reached, which is LASTDRIVE too small ------
        xor     ebx,ebx
cx_unit:
        cmp     ebx,[n_units]
        jae     cx_done
        cmp     ebx,LETTERS
        jae     cx_done
        cmp     byte ptr [unit_seen+ebx],0
        jne     cx_unext
        mov     byte ptr [row_letter],0FFh
        mov     [row_unit],bl
        call    row
cx_unext:
        inc     ebx
        jmp     short cx_unit
cx_done:
        mov     ax,4C00h
        int     21h

;=====================================================================
; row - one drive: row_letter (FFh for none) and row_unit are set.
;       Preserves EBX.
;=====================================================================
row:
        push    ebx

        ; the letter
        mov     edx,OFFSET msg_noletter
        cmp     byte ptr [row_letter],0FFh
        je      rw_letter
        mov     al,[row_letter]
        add     al,'A'
        mov     [msg_letter+2],al
        mov     edx,OFFSET msg_letter
rw_letter:
        call    print

        ; the unit
        movzx   eax,byte ptr [row_unit]
        call    pdec
        mov     edx,OFFSET msg_4sp
        call    print

        ; the drive itself
        movzx   edx,byte ptr [row_unit]
        mov     edi,OFFSET cdu
        mov     eax,0F117h
        int     21h
        jc      rw_nounit

        ; where it is: the channel by its base port, then the device
        movzx   eax,word ptr [cdu+CDU_BASE]
        mov     edx,OFFSET msg_primary
        cmp     ax,1F0h
        je      rw_chan
        mov     edx,OFFSET msg_secondary
        cmp     ax,170h
        je      rw_chan
        mov     edx,OFFSET msg_tertiary
        cmp     ax,1E8h
        je      rw_chan
        mov     edx,OFFSET msg_quaternary
        cmp     ax,168h
        je      rw_chan
        call    puthex16                        ; some other port
        mov     edx,OFFSET msg_hport
rw_chan:
        mov     dword ptr [loc_len],0
        call    printc
        mov     edx,OFFSET msg_master
        test    byte ptr [cdu+CDU_DEV],1
        jz      rw_sel
        mov     edx,OFFSET msg_slave
rw_sel:
        call    printc
        ; pad the column: the two words and the blank between them
        mov     ecx,LOC_W + 2
        sub     ecx,[loc_len]
        jle     rw_disc
rw_lpad:
        mov     al,' '
        call    putc
        loop    rw_lpad
        jmp     short rw_disc
rw_nounit:
        mov     edx,OFFSET msg_nounit
        call    print

        ; what is in it
rw_disc:
        cmp     byte ptr [row_letter],0FFh
        je      rw_noletter
        movzx   edx,byte ptr [row_letter]
        inc     edx                             ; AL=1 counts from 1
        mov     eax,0F101h
        int     21h                             ; mounts it, or cannot
        jc      rw_nodisc
        call    label_of
        jmp     short rw_model
rw_noletter:
        mov     edx,OFFSET msg_nolabel
        call    print
        jmp     short rw_model
rw_nodisc:
        mov     edx,OFFSET msg_nodisc
        call    print

        ; its name
rw_model:
        mov     esi,OFFSET cdu + CDU_MODEL
        mov     ecx,ATA_MODEL_LEN
        call    trim                            ; -> ECX = the length
        cmp     ecx,MODEL_W
        jbe     rw_mlen
        mov     ecx,MODEL_W
rw_mlen:
        or      ecx,ecx
        jnz     rw_mprint
        mov     edx,OFFSET msg_noname
        call    print
        jmp     short rw_end
rw_mprint:
        call    putn
rw_end:
        call    crlf
        pop     ebx
        ret

;---------------------------------------------------------------------
; label_of - the volume label of the letter in row_letter, printed in
;            a DISC_W column.  4Eh with the volume-label attribute on
;            X:\*.*: the ISO9660 reader synthesises entry zero of the
;            root from the volume id, so this is the same call VOL
;            makes on a FAT disk.
;---------------------------------------------------------------------
label_of:
        mov     al,[row_letter]
        add     al,'A'
        mov     [lbl_path],al
        mov     edx,OFFSET lbl_path
        mov     ecx,8                           ; the label attribute
        mov     ah,4Eh
        int     21h
        jc      lo_none
        mov     esi,OFFSET dta + 1Eh
        mov     ecx,13
        call    trimz                           ; -> ECX = the length
        or      ecx,ecx
        jz      lo_none
        push    ecx
        call    putn
        pop     ecx
        mov     eax,DISC_W + 2
        sub     eax,ecx
        jle     lo_out
lo_pad:
        push    eax
        mov     al,' '
        call    putc
        pop     eax
        dec     eax
        jnz     lo_pad
lo_out:
        ret
lo_none:
        mov     edx,OFFSET msg_nolabel
        call    print
        ret

;---------------------------------------------------------------------
; banner - whose extensions, and the kernel's version
;---------------------------------------------------------------------
banner:
        mov     edx,OFFSET msg_banner
        call    print
        mov     ax,0F100h
        int     21h
        movzx   eax,bh
        push    ebx
        call    pdec
        mov     al,'.'
        call    putc
        pop     ebx
        movzx   eax,bl
        cmp     eax,10
        jae     bn_minor
        push    eax
        mov     al,'0'
        call    putc
        pop     eax
bn_minor:
        call    pdec
        call    crlf
        call    crlf
        ret

;=====================================================================
; The critical-error handler: fail everything, silently.
;=====================================================================
crit_fail:
        mov     al,CERR_FAIL
        iretd

;=====================================================================
; Output helpers
;=====================================================================
print:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; printc - print, counting the characters into loc_len
printc:
        push    eax
        push    edx
prc_lp:
        cmp     byte ptr [edx],'$'
        je      prc_out
        inc     dword ptr [loc_len]
        inc     edx
        jmp     short prc_lp
prc_out:
        pop     edx
        pop     eax
        jmp     print

putc:
        push    eax
        push    edx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     edx
        pop     eax
        ret

; putn - ECX characters at ESI
putn:
        push    ecx
        push    esi
pn_lp:
        jecxz   pn_out
        mov     al,[esi]
        call    putc
        inc     esi
        dec     ecx
        jmp     short pn_lp
pn_out:
        pop     esi
        pop     ecx
        ret

crlf:
        push    edx
        mov     edx,OFFSET msg_crlf
        call    print
        pop     edx
        ret

; trim  - ESI -> ECX bytes, space padded -> ECX = the length without
;         the trailing blanks
; trimz - the same for an ASCIIZ string of at most ECX bytes
trimz:
        push    esi
        push    edx
        xor     edx,edx
tz_lp:
        cmp     edx,ecx
        jae     tz_end
        cmp     byte ptr [esi+edx],0
        je      tz_end
        inc     edx
        jmp     short tz_lp
tz_end:
        mov     ecx,edx
        pop     edx
        pop     esi
trim:
        jecxz   tr_out
        cmp     byte ptr [esi+ecx-1],' '
        ja      tr_out
        dec     ecx
        jmp     short trim
tr_out:
        ret

; pdec - EAX in decimal, no padding
pdec:
        pushad
        mov     edi,OFFSET numbuf + 11
        mov     byte ptr [edi],'$'
        mov     ecx,10
pd_lp:
        xor     edx,edx
        div     ecx
        dec     edi
        add     dl,'0'
        mov     [edi],dl
        or      eax,eax
        jnz     pd_lp
        mov     edx,edi
        call    print
        popad
        ret

; puthex16 - AX as four hex digits into msg_hport
puthex16:
        push    ecx
        push    edx
        mov     edx,OFFSET msg_hport
        mov     ecx,4
ph_lp:
        rol     ax,4
        push    eax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph_dig
        add     al,7
ph_dig:
        mov     [edx],al
        pop     eax
        inc     edx
        loop    ph_lp
        pop     edx
        pop     ecx
        ret

;=====================================================================
; Data
;=====================================================================
row_letter  db  0
row_unit    db  0
            db  0,0
loc_len     dd  0                       ; what printc has put in the column
n_units     dd  0
unit_seen   db  LETTERS dup(0)
            db  0,0
drv         db  DRV_LEN dup(0)
cdu         db  CDU_LEN dup(0)
dta         db  64 dup(0)
numbuf      db  12 dup(0)
lbl_path    db  'D:\*.*',0

msg_banner  db  'PM-DOS CD-ROM Extensions version ','$'
msg_none    db  'No CD-ROM drive was found.',13,10,'$'
msg_head    db  'Drive  Unit  Location',(LOC_W - 8) dup(' '),'  Disc',(DISC_W - 4) dup(' ')
            db  '  Model',13,10
            db  5 dup('-'),'  ',4 dup('-'),'  ',LOC_W dup('-'),'  ',DISC_W dup('-')
            db  '  ',MODEL_W dup('-'),13,10,'$'
msg_letter  db  '  D:    ','$'
msg_noletter db '  --    ','$'
msg_4sp     db  '    ','$'
msg_primary db  'primary ','$'
msg_secondary db 'secondary ','$'
msg_tertiary db 'tertiary ','$'
msg_quaternary db 'quaternary ','$'
msg_hport   db  '0000h ','$'
msg_master  db  'master','$'
msg_slave   db  'slave','$'
msg_nounit  db  '?',(LOC_W + 1) dup(' '),'$'
msg_nodisc  db  'no disc',(DISC_W + 2 - 7) dup(' '),'$'
msg_nolabel db  '-',(DISC_W + 2 - 1) dup(' '),'$'
msg_noname  db  '(unnamed)','$'
msg_crlf    db  13,10,'$'

msg_badparm db  'Invalid parameter',13,10,'$'
msg_usage   db  'Displays the CD-ROM drives and their drive letters.',13,10,13,10
            db  'CDEX',13,10,13,10
            db  'Each line is one drive: its letter, the unit number by which',13,10
            db  'MSCD001 and INT 2Fh AH=15h count it, where it is on the IDE bus,',13,10
            db  'the label of the disc in it, and the name the drive reports.',13,10,13,10
            db  'CDROM= in CONFIG.SYS chooses the first letter.',13,10,'$'

_CDX    ENDS
        END
