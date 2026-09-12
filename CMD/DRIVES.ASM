;=====================================================================
; DRIVES.ASM - DRIVES.EXE, what the partition scan found.
;
; One line per drive letter: which BIOS disk it lives on, what the
; partition type byte says it is, where it starts, how big it is, and
; - by mounting it - what FAT the contents actually turn out to be.
;
; THE LAST TWO COLUMNS ARE THE POINT.  A partition's type byte is a
; label somebody wrote and the FAT flavour is what the volume really
; is, and they disagree more often than they should: a 32MB FAT12
; volume inside an 06h partition is legal and exists, and so is a
; FAT16 one inside an 04h that outgrew it.  Anything that decides how
; to read a disk from the type byte will eventually read one wrong.
;
; IT IS A DIAGNOSTIC AND IT LIVES IN \TESTS, not in the root with the
; commands.  PM-DOS ships what DOS shipped and DOS never had a DRIVES;
; the DISK-side view of the same information is FDISK /L, which prints
; the partitions and the letter each one became.  What this adds is the
; LETTER-side view, and the part of it nothing else can give: the
; letters NO PARTITION TABLE EXPLAINS - a CD-ROM, a RAM disk a block
; driver created, a floppy - plus what the volume really turned out to
; be when it was mounted.  Kept for that, and kept out of the
; distribution.
;
; What it asks:
;   21h/F1h AL=6    the volume table entry for drive DL
;   21h/F1h AL=1    the FAT type of drive DL, which mounts it
;   21h/36h         free space, for volumes that mount
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_DRV    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_DRV, DS:_DRV, ES:_DRV, SS:_DRV
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET drv_entry>,<0>,4096

drv_entry:
;--- our own critical-error handler, before anything touches a disk --
; A tool that enumerates drives has to try to mount them, and an empty
; floppy drive answers that with a critical error - which without this
; puts "Not ready reading drive B / Abort, Retry, Fail?" in the middle
; of the table and waits for a human.  FAIL is the right answer here:
; "will not mount" IS the report.
        mov     edx,OFFSET crit_fail
        mov     eax,2524h
        int     21h

        mov     edx,OFFSET msg_head
        call    puts

        xor     ebx,ebx                 ; EBX = the letter being reported
dv_one:
        mov     edx,ebx
        mov     edi,OFFSET drv_rec
        mov     eax,0F106h
        int     21h
        jc      dv_done                 ; past the end of the table
        mov     [drv_total],ecx

        test    byte ptr [drv_rec+DRV_FLAGS],DRVF_PRESENT
        jz      dv_next                 ; a letter nothing was assigned to

;--- the letter -------------------------------------------------------
        mov     al,bl
        add     al,'A'
        call    putc
        mov     al,':'
        call    putc
        call    sp2

;--- which disk -------------------------------------------------------
        movzx   eax,byte ptr [drv_rec+DRV_BIOS]
        test    byte ptr [drv_rec+DRV_FLAGS],DRVF_DRIVER
        jz      dv_notdrv
        mov     edx,OFFSET msg_driver   ; a loaded block driver: DRV_BIOS is
        call    puts                    ; a unit number and not a disk, and
        jmp     short dv_type           ; printing it as one prints a control
                                        ; character
dv_notdrv:
        test    byte ptr [drv_rec+DRV_FLAGS],DRVF_CDROM
        jnz     dv_cd
        test    byte ptr [drv_rec+DRV_FLAGS],DRVF_FLOPPY
        jz      dv_hd
        mov     edx,OFFSET msg_floppy
        call    puts
        jmp     short dv_type
dv_hd:
        mov     edx,OFFSET msg_disk
        call    puts
        movzx   eax,byte ptr [drv_rec+DRV_BIOS]
        sub     eax,80h
        add     al,'0'
        call    putc
        call    sp2

;--- the type byte, and what it is called -----------------------------
dv_type:
        movzx   eax,byte ptr [drv_rec+DRV_TYPE]
        call    put2
        call    sp1
        movzx   eax,byte ptr [drv_rec+DRV_TYPE]
        call    typename

;--- where it is ------------------------------------------------------
        mov     eax,dword ptr [drv_rec+DRV_START]
        call    put10
        call    sp1
        mov     eax,dword ptr [drv_rec+DRV_SECS]
        shr     eax,11                  ; sectors -> MB
        call    put6
        mov     edx,OFFSET msg_mb
        call    puts

;--- and what the volume actually is ----------------------------------
; F1h AL=1 mounts the drive, so this is also the test of whether the
; scan found something readable rather than something plausible.
        mov     edx,ebx
        inc     edx                     ; this call is 1-based
        mov     eax,0F101h
        int     21h
        jc      dv_nomount
        movzx   eax,al
        mov     edx,OFFSET msg_fat12
        cmp     eax,12
        je      dv_say
        mov     edx,OFFSET msg_fat16
        cmp     eax,16
        je      dv_say
        mov     edx,OFFSET msg_fat32
dv_say:
        call    puts
        jmp     short dv_eol
dv_nomount:
        mov     edx,OFFSET msg_nomount
        call    puts
dv_eol:
        call    crlf
        jmp     dv_next

;--- a CD-ROM, which fits none of the columns above -------------------
; DRV_BIOS holds an ATAPI UNIT and not a BIOS drive, so it is printed
; as itself.  There is no partition type byte and no start LBA because
; a CD-ROM is not a partition on anything.  The one figure it does
; have is how big the disc in it is, and 36h answering at all is what
; says there is one.
dv_cd:
        mov     edx,OFFSET msg_cd
        call    puts
        movzx   eax,byte ptr [drv_rec+DRV_BIOS]
        add     al,'0'
        call    putc
        call    sp2
        mov     edx,OFFSET msg_cdtype
        call    puts
        mov     edx,OFFSET msg_cdstart
        call    puts

        push    ebx                     ; 36h answers in BX, and BX is
        mov     edx,ebx                 ; the letter this loop is on
        inc     edx                     ; ...and counts A: as 1, as F1h does
        mov     eax,3600h
        int     21h
        cmp     ax,0FFFFh
        je      dv_nodisc
        movzx   eax,ax                  ; sectors in a cluster
        movzx   ecx,cx                  ; bytes in a sector
        imul    eax,ecx                 ; -> bytes in a cluster
        movzx   ecx,dx                  ; clusters on the volume
        imul    eax,ecx
        shr     eax,20                  ; -> MB
        pop     ebx
        call    put6
        mov     edx,OFFSET msg_mb
        call    puts
        mov     edx,OFFSET msg_isofs
        call    puts
        jmp     short dv_next
dv_nodisc:
        pop     ebx
        mov     edx,OFFSET msg_nosize
        call    puts
        mov     edx,OFFSET msg_nodisc
        call    puts

dv_next:
        inc     ebx
        cmp     ebx,[drv_total]
        jb      dv_one
dv_done:
        call    crlf
        mov     eax,[drv_total]
        call    put6
        mov     edx,OFFSET msg_total
        call    puts

        mov     eax,4C00h
        int     21h

;=====================================================================
; The critical-error handler: fail everything, silently.
;=====================================================================
crit_fail:
        mov     al,CERR_FAIL
        iretd

;=====================================================================
; typename - EAX = a partition type byte, print what it is called,
;            padded to a column.
;=====================================================================
typename:
        push    ebx
        mov     ebx,OFFSET tn_tbl
tn_look:
        cmp     byte ptr [ebx],0FFh     ; FFh ends the table, NOT zero:
        je      tn_unknown              ; zero is a real entry here
        cmp     al,[ebx]
        je      tn_found
        add     ebx,11                  ; type byte + nine characters + '$'
        jmp     short tn_look
tn_found:
        lea     edx,[ebx+1]
        call    puts
        pop     ebx
        ret
tn_unknown:
        mov     edx,OFFSET msg_unknown
        call    puts
        pop     ebx
        ret

;--- type byte, nine characters of name, and the '$' that 21h/09h
;--- stops at.  NOT a NUL: this table is printed with the string call,
;--- and a zero terminator sends it off down memory looking for a
;--- dollar sign it will eventually find in somebody else's data.
tn_tbl:
        db  PT_EMPTY,     'no table $'          ; the whole medium
        db  PT_FAT12,     'FAT12    $'
        db  PT_FAT16S,    'FAT16    $'
        db  PT_FAT16B,    'FAT16B   $'
        db  PT_FAT16L,    'FAT16-LBA$'
        db  PT_FAT32,     'FAT32    $'
        db  PT_FAT32L,    'FAT32-LBA$'
        db  0FFh

;=====================================================================
; Output helpers.  All through 21h/02h and 21h/09h: this is a program
; and not part of the kernel, and it has no more access than any
; other program does.
;=====================================================================
puts:
        push    eax
        mov     eax,0900h
        int     21h
        pop     eax
        ret
putc:
        push    eax
        push    edx
        movzx   edx,al
        mov     eax,0200h
        int     21h
        pop     edx
        pop     eax
        ret
sp1:
        push    eax
        mov     al,' '
        call    putc
        pop     eax
        ret
sp2:
        call    sp1
        jmp     short sp1
crlf:
        push    eax
        mov     al,13
        call    putc
        mov     al,10
        call    putc
        pop     eax
        ret

;--- EAX in hex, two digits ------------------------------------------
put2:
        push    eax
        shr     eax,4
        and     al,0Fh
        call    puthex1
        pop     eax
        push    eax
        and     al,0Fh
        call    puthex1
        pop     eax
        ret
puthex1:
        add     al,'0'
        cmp     al,'9'
        jbe     ph_out
        add     al,7
ph_out:
        call    putc
        ret

;--- EAX in decimal, right aligned in six / ten columns --------------
put6:
        push    ebx
        mov     ebx,6
        jmp     short put_n
put10:
        push    ebx
        mov     ebx,10
put_n:
        push    eax
        push    ecx
        push    edx
        push    edi
        mov     edi,OFFSET num_buf + 15
        mov     byte ptr [edi],'$'
        xor     ecx,ecx
pn_div:
        xor     edx,edx
        push    ebx
        mov     ebx,10
        div     ebx
        pop     ebx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        inc     ecx
        or      eax,eax
        jnz     pn_div
pn_pad:
        cmp     ecx,ebx
        jae     pn_out
        dec     edi
        mov     byte ptr [edi],' '
        inc     ecx
        jmp     short pn_pad
pn_out:
        mov     edx,edi
        call    puts
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        pop     ebx
        ret

;=====================================================================
; Data
;=====================================================================
drv_rec     db  DRV_LEN dup(0)
drv_total   dd  0
num_buf     db  16 dup(0)

msg_head    db  13,10,'Drive  Disk    Type            Start     Size  '
            db  'Filesystem',13,10
            db  '-----  ------  ---------  ----------  -------  '
            db  '----------',13,10,'$'
msg_floppy  db  'floppy  $'
msg_driver  db  'driver  $'
msg_disk    db  'hd$'
msg_cd      db  'cd$'
msg_cdtype  db  '-- CD-ROM   $'         ; the two columns a CD has no
msg_cdstart db  '         -  $'         ; answer for, kept in their places
msg_isofs   db  'ISO9660',13,10,'$'
msg_nodisc  db  '(no disc)',13,10,'$'
msg_nosize  db  '     -  $'
msg_mb      db  ' MB  $'
msg_fat12   db  'FAT12',13,10,'$'
msg_fat16   db  'FAT16',13,10,'$'
msg_fat32   db  'FAT32',13,10,'$'
msg_nomount db  '(will not mount)',13,10,'$'
msg_unknown db  'unknown  $'
msg_total   db  ' drive letters assigned',13,10,'$'

_DRV    ENDS
        END
