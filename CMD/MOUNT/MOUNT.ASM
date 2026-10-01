;=====================================================================
; MOUNT.ASM - MOUNT.EXE, a file given a drive letter
;
;   MOUNT                   what is mounted now
;   MOUNT E: DISK.IMG       give that file the letter E:
;   MOUNT DISK.IMG          ...or the next free letter
;   MOUNT E: CD.ISO /R      read-only, which an .ISO is anyway
;   MOUNT /U E:             and give the letter back
;
; WHAT IS IN THE FILE DECIDES HOW IT IS READ, not what it is called.
; An .ISO is read as ISO9660 or UDF because block 16 says so; an .IMG,
; .IMA or .DSK is read as FAT because sector zero does; and a file
; holding a whole disk - a master boot record and a partition table -
; becomes its first FAT partition.  A file named the wrong way round
; still works, which is the point of looking rather than guessing.
;
; THE KERNEL DOES ALL OF IT.  This program parses a command line and
; prints a table; the mount itself is 21h/F1h AL=0Fh, because a native
; program runs at ring 3 and cannot reach the volume table, and
; because turning a file into a list of sectors means walking a
; cluster chain, which is the filesystem's business and not a
; program's.  Same argument MEM makes for asking about memory.
;
; What it asks:
;   21h/F1h AL=0Fh   mount EDX as drive BL (FFh = next free), BH = r/o
;   21h/F1h AL=10h   unmount drive BL
;   21h/F1h AL=11h   describe mount BL into the buffer at EDI
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

; The report AL=11h fills in.  Its layout is documented at img_report
; in DOS\IMAGE.INC and repeated here because a program reading a
; kernel structure should say which one it thinks it is reading.
RP_LETTER       EQU     00h             ; db - 0 = A:
RP_FLAGS        EQU     01h             ; db - IMF_*
RP_HOST         EQU     02h             ; db - the volume the file is on
RP_SIZE         EQU     04h             ; dd - the file, in bytes
RP_EXTENTS      EQU     08h             ; dd - runs in its map
RP_START        EQU     0Ch             ; dd - the volume's first sector
RP_SECS         EQU     10h             ; dd - and how many it has
RP_PATH         EQU     14h             ; 80 bytes, ASCIIZ
RP_LEN          EQU     14h + 80

_MNT    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_MNT, DS:_MNT, ES:_MNT, SS:_MNT
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET mn_entry>,<0>,4096

mn_entry:
        call    mn_parse
        jc      mn_usage

        cmp     byte ptr [opt_help],0
        jne     mn_usage
        cmp     byte ptr [opt_umount],0
        jne     mn_do_umount
        cmp     byte ptr [have_path],0
        je      mn_list

;=====================================================================
; MOUNT d: file
;=====================================================================
        mov     bl,0FFh                 ; no letter named: the next free
        cmp     byte ptr [have_drive],0
        je      mn_noletter
        mov     bl,[arg_drive]
mn_noletter:
        mov     bh,[opt_ro]
        mov     edx,OFFSET arg_path
        mov     eax,0F10Fh
        int     21h
        jc      mn_failed

        ; AL came back as the letter it took, which is worth printing
        ; even when one was asked for: "MOUNT X.IMG" with no letter is
        ; the common case and the answer is the whole result.
        push    eax
        mov     edx,OFFSET msg_mounted
        call    puts
        pop     eax
        add     al,'A'
        call    putc
        mov     al,':'
        call    putc
        call    crlf
        jmp     mn_exit

;=====================================================================
; MOUNT /U d:
;=====================================================================
mn_do_umount:
        cmp     byte ptr [have_drive],0
        je      mn_usage
        mov     bl,[arg_drive]
        mov     eax,0F110h
        int     21h
        jc      mn_failed
        mov     edx,OFFSET msg_umounted
        call    puts
        movzx   eax,byte ptr [arg_drive]
        add     al,'A'
        call    putc
        mov     al,':'
        call    putc
        call    crlf
        jmp     mn_exit

;=====================================================================
; MOUNT, with nothing to do: say what is mounted
;=====================================================================
mn_list:
        mov     byte ptr [any_found],0
        xor     ebx,ebx
mn_lsone:
        cmp     ebx,IMG_MAX
        jae     mn_lsdone
        push    ebx
        mov     edi,OFFSET rep_buf
        mov     eax,0F111h
        int     21h
        pop     ebx
        jc      mn_lsnext

        cmp     byte ptr [any_found],0  ; the heading, once, and only if
        jne     mn_lshead               ; there is anything under it
        mov     byte ptr [any_found],1
        push    ebx
        mov     edx,OFFSET msg_head
        call    puts
        pop     ebx
mn_lshead:

;--- the letter ------------------------------------------------------
        movzx   eax,byte ptr [rep_buf+RP_LETTER]
        add     al,'A'
        call    putc
        mov     al,':'
        call    putc
        call    sp4                     ; "E:" plus five is the seven the
        call    sp1                     ; "Drive  " heading occupies

;--- FAT or ISO, and read-only or not --------------------------------
        mov     edx,OFFSET msg_fat
        test    byte ptr [rep_buf+RP_FLAGS],IMF_ISO
        jz      mn_lsfs
        mov     edx,OFFSET msg_iso
mn_lsfs:
        call    puts
        mov     edx,OFFSET msg_rw
        test    byte ptr [rep_buf+RP_FLAGS],IMF_RO
        jz      mn_lsmode
        mov     edx,OFFSET msg_ro
mn_lsmode:
        call    puts

;--- how many pieces the file is in ----------------------------------
; PRINTED BECAUSE IT IS THE ONE NUMBER THAT PREDICTS TROUBLE.  One
; extent is a contiguous file and the fastest this can be; a few
; hundred is a host volume that wants defragmenting; past four
; thousand the mount is refused outright.
        mov     eax,dword ptr [rep_buf+RP_EXTENTS]
        call    put6
        call    sp2

;--- the size of the VOLUME, which is not always the size of the file
; A partitioned image is bigger than the volume inside it, and saying
; which one this column means matters: the difference is exactly the
; space outside the partition, which FDISK can see and DIR cannot.
        mov     eax,dword ptr [rep_buf+RP_SECS]
        shr     eax,1                   ; 512-byte sectors -> KB
        call    put8
        mov     edx,OFFSET msg_kb
        call    puts

;--- and what it is -------------------------------------------------
        mov     edx,OFFSET rep_buf + RP_PATH
        call    putz
        call    crlf

mn_lsnext:
        inc     ebx
        jmp     mn_lsone
mn_lsdone:
        cmp     byte ptr [any_found],0
        jne     mn_exit
        mov     edx,OFFSET msg_none
        call    puts
        jmp     short mn_exit

;=====================================================================
; Endings
;=====================================================================
mn_failed:
        ; THE PATH IS PRINTED BACK, and it is not decoration: "no such
        ; file" about a name the user typed and a name this program
        ; PARSED are two different statements, and only the second one
        ; is what the kernel was asked.  The first time this message
        ; had no name in it, an hour went on the wrong half.
        push    eax
        mov     edx,OFFSET msg_cannot
        call    puts
        cmp     byte ptr [opt_umount],0
        jne     mn_fnopath
        mov     edx,OFFSET arg_path
        call    putz
mn_fnopath:
        mov     edx,OFFSET msg_colon
        call    puts
        pop     eax
        movzx   eax,ax
        call    mn_why
        call    crlf
        mov     eax,4C01h
        int     21h

mn_usage:
        mov     edx,OFFSET msg_usage
        call    puts
mn_exit:
        mov     eax,4C00h
        int     21h

;---------------------------------------------------------------------
; mn_why - EAX = the DOS error the kernel returned.  Say what it means
;          HERE, which is not always what it means elsewhere: a mount
;          can fail for reasons a file operation has no word for, and
;          "access denied" on its own would send the reader looking
;          for a permission that does not exist.
;---------------------------------------------------------------------
mn_why:
        cmp     eax,ERR_NOFILE
        jne     mn_w3
        mov     edx,OFFSET msg_e2
        jmp     short mn_wsay
mn_w3:
        cmp     eax,ERR_NOPATH
        jne     mn_w5
        mov     edx,OFFSET msg_e3
        jmp     short mn_wsay
mn_w5:
        cmp     eax,ERR_ACCESS
        jne     mn_w8
        mov     edx,OFFSET msg_e5
        jmp     short mn_wsay
mn_w8:
        cmp     eax,ERR_NOMEM
        jne     mn_w13
        mov     edx,OFFSET msg_e8
        jmp     short mn_wsay
mn_w13:
        cmp     eax,ERR_BADDATA
        jne     mn_w15
        mov     edx,OFFSET msg_e13
        jmp     short mn_wsay
mn_w15:
        cmp     eax,ERR_BADDRIVE
        jne     mn_w31
        mov     edx,OFFSET msg_e15
        jmp     short mn_wsay
mn_w31:
        cmp     eax,ERR_GENERAL
        jne     mn_w70
        mov     edx,OFFSET msg_e31
        jmp     short mn_wsay
mn_w70:
        cmp     eax,ERR_DISKFULL
        jne     mn_w80
        mov     edx,OFFSET msg_e70
        jmp     short mn_wsay
mn_w80:
        cmp     eax,ERR_EXISTS
        jne     mn_wnum
        mov     edx,OFFSET msg_e80
mn_wsay:
        call    puts
        ret
mn_wnum:
        push    eax
        mov     edx,OFFSET msg_enum
        call    puts
        pop     eax
        call    put6
        ret

;=====================================================================
; mn_parse - take the command tail apart.  CF set if it is nonsense.
;
; A TOKEN AT A TIME, and which token it is decided by what it looks
; like rather than by where it sits: "MOUNT E: X.IMG" and
; "MOUNT X.IMG E:" both mean the same thing, because a two-character
; word ending in a colon can only be a drive and anything else can
; only be a file.  Switches may come anywhere, which is what DOS
; commands have always allowed and what a user will type regardless.
;=====================================================================
mn_parse:
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
mp_token:
        ; skip whitespace
        or      ecx,ecx
        jz      mp_done
        mov     al,[esi]
        cmp     al,' '
        je      mp_eat
        cmp     al,9
        je      mp_eat
        cmp     al,13
        je      mp_done
        jmp     short mp_have
mp_eat:
        inc     esi
        dec     ecx
        jmp     short mp_token

mp_have:
        cmp     al,'/'
        je      mp_switch
        cmp     al,'-'
        je      mp_switch

        ; a word: measure it before deciding what it is
        mov     edi,esi
        xor     edx,edx                 ; EDX = its length
mp_meas:
        cmp     edx,ecx
        jae     mp_measured
        mov     al,[esi+edx]
        cmp     al,' '
        je      mp_measured
        cmp     al,9
        je      mp_measured
        cmp     al,13
        je      mp_measured
        inc     edx
        jmp     short mp_meas
mp_measured:
        cmp     edx,2                   ; "X:" and nothing else is a drive
        jne     mp_ispath
        cmp     byte ptr [esi+1],':'
        jne     mp_ispath
        mov     al,[esi]
        cmp     al,'a'
        jb      mp_up
        cmp     al,'z'
        ja      mp_up
        sub     al,20h
mp_up:
        sub     al,'A'
        cmp     al,LASTDRIVE
        jae     mp_bad
        mov     [arg_drive],al
        mov     byte ptr [have_drive],1
        jmp     short mp_next

mp_ispath:
        cmp     byte ptr [have_path],0  ; two files is not a question this
        jne     mp_bad                  ; command knows how to answer
        cmp     edx,79
        ja      mp_bad
        push    ecx
        push    esi
        mov     edi,OFFSET arg_path
        mov     ecx,edx
mp_cp:
        mov     al,[esi]
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     mp_cp
        mov     byte ptr [edi],0
        pop     esi
        pop     ecx
        mov     byte ptr [have_path],1
        jmp     short mp_next

mp_switch:
        inc     esi                     ; past the '/'
        dec     ecx
        jz      mp_bad
        mov     al,[esi]
        cmp     al,'a'
        jb      mp_sup
        cmp     al,'z'
        ja      mp_sup
        sub     al,20h
mp_sup:
        cmp     al,'U'
        jne     mp_s2
        mov     byte ptr [opt_umount],1
        jmp     short mp_sdone
mp_s2:
        cmp     al,'R'
        jne     mp_s3
        mov     byte ptr [opt_ro],1
        jmp     short mp_sdone
mp_s3:
        cmp     al,'?'
        jne     mp_bad
        mov     byte ptr [opt_help],1
mp_sdone:
        mov     edx,1                   ; one character consumed

;--- step over EDX characters and any that follow them ---------------
mp_next:
        add     esi,edx
        sub     ecx,edx
        jmp     mp_token

mp_done:
        ; /U and a file together is two different commands at once
        cmp     byte ptr [opt_umount],0
        je      mp_ok
        cmp     byte ptr [have_path],0
        jne     mp_bad
mp_ok:
        clc
        ret
mp_bad:
        stc
        ret

;=====================================================================
; Output helpers.  Through 21h/02h and 21h/09h, like any program.
;=====================================================================
puts:
        push    eax
        mov     eax,0900h
        int     21h
        pop     eax
        ret

;--- putz - the same for an ASCIIZ string, which is what the kernel
;--- hands back.  21h/09h stops at a '$' and a path may contain one.
putz:
        push    eax
        push    esi
        mov     esi,edx
pz_lp:
        mov     al,[esi]
        or      al,al
        jz      pz_out
        call    putc
        inc     esi
        jmp     short pz_lp
pz_out:
        pop     esi
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
sp4:
        call    sp2
        jmp     short sp2
crlf:
        push    eax
        mov     al,13
        call    putc
        mov     al,10
        call    putc
        pop     eax
        ret

;--- EAX in decimal, right aligned in six / eight columns ------------
put6:
        push    ebx
        mov     ebx,6
        jmp     short put_n
put8:
        push    ebx
        mov     ebx,8
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
        jae     pn_say
        dec     edi
        mov     byte ptr [edi],' '
        inc     ecx
        jmp     short pn_pad
pn_say:
        mov     edx,edi
        call    puts
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        pop     ebx
        ret

;=====================================================================
; State
;=====================================================================
arg_drive   db  0
have_drive  db  0
have_path   db  0
opt_ro      db  0
opt_umount  db  0
opt_help    db  0
any_found   db  0
            db  0
arg_path    db  80 dup(0)
rep_buf     db  RP_LEN dup(0)
num_buf     db  16 dup(0)

msg_head    db  13,10,'Drive  Type  Mode  Extents      Size  File',13,10
            db          '-----  ----  ----  -------  --------  '
            db  '----------------------------------',13,10,'$'
msg_fat     db  'FAT   $'
msg_iso     db  'ISO   $'
msg_rw      db  'rw   $'
msg_ro      db  'ro   $'
msg_kb      db  ' KB  $'
msg_none    db  'No image files are mounted.',13,10,'$'
msg_mounted db  'Mounted as $'
msg_umounted db 'Unmounted $'
msg_cannot  db  'Cannot mount $'
msg_colon   db  ': $'

msg_e2      db  'no such file$'
msg_e3      db  'no such directory, or the file is not on a FAT volume$'
msg_e5      db  'the file is in use, is a directory, or is empty$'
msg_e8      db  'not enough memory for the extent map$'
msg_e13     db  'nothing in it that this kernel can read - not a FAT '
            db  'volume,$            a partitioned disk, or an ISO9660 '
            db  'or UDF disc$'
msg_e15     db  'no such drive letter, or it is already in use$'
msg_e31     db  'the cluster chain does not cover the file - run CHKDSK$'
msg_e70     db  'the file is in too many pieces - defragment the volume$'
msg_e80     db  'that file already has a drive letter$'
msg_enum    db  'error $'

msg_usage   db  13,10,'MOUNT - Mount a file as a drive',13,10,13,10
            db  '  MOUNT                 List currently mounted drives.',13,10
            db  '  MOUNT d: file         Mount file as drive d:.',13,10
            db  '  MOUNT file            Mount file as next available drive.',13,10
            db  '  MOUNT d: file /R      Mount file as read-only.',13,10
            db  '  MOUNT /U d:           Unmount drive d:',13,10,13,10
            db  'An .ISO is read as ISO9660 or UDF and an .IMG, .IMA or',13,10
            db  '.DSK as FAT.',13,10,13,10,'$'

_MNT    ENDS
        END
