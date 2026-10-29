;=====================================================================
; RAMDISK.ASM - RAMDISK.SYS, a RAM disk: memory above the megabyte
;               made into a drive letter
;
;   DEVICE=RAMDISK.SYS [size [sector-size [entries]]] [/E | /A] [/L:x]
;
;   size          kilobytes, 4 to 2096896; 64 if it is not given.  A
;                 bigger number is cut to the largest FAT16 volume
;   sector-size   128, 256, 512 or 1024, and every one of them makes
;                 512-byte sectors: the kernel's sectors and RMDOS's
;                 are 512 bytes, and a smaller one was only ever a way
;                 to waste less of a 64K disk
;   entries       how many files the root directory holds, 2 to 1024;
;                 64 if not given, rounded up to a whole sector (16)
;   /E, /A        extended or expanded memory, which on PM-DOS are the
;                 same memory.  Taken and ignored, so that a line
;                 written for MS-DOS's RAMDRIVE.SYS works as it stands
;   /L:x          the drive letter x, if no other drive has it; the
;                 next free one if it is taken or past the kernel's
;                 last letter, as without /L:.  It is how a CONFIG.SYS
;                 that has to work on any machine - a start-up CD's -
;                 can say TEMP=N:\ and know N: is it
;
; THE DISK IS ONE BLOCK OF THE KERNEL'S ARENA, taken at INIT through
; NDS_ALLOC and never given back, so neither a program's exit nor the
; park ever hands it to anybody else.  It is formatted there and then:
; a boot sector, ONE FAT - a second copy of a table that is in memory
; guards against nothing - and a root directory whose first entry is
; the label RAMDISK.  The clusters are the smallest that keep the
; volume legal: FAT12 while the count fits, then FAT16, then larger
; clusters, which is the least slack a disk of that size can have.
; The data area is not cleared.  Nothing reads a cluster the FAT has
; not given out, and clearing 16MB of it would be the slowest thing
; CONFIG.SYS did.
;
; A TRANSFER IS ONE COPY.  The kernel calls rd_io (NDRV.INC, NDU_IO)
; with its own buffer and this driver moves the sectors with REP MOVSD:
; no request packet, no staging buffer, no excursion into V86 mode,
; and no cache in front of it either, because the unit says it is
; memory (NDUF_MEMORY) and the kernel leaves such a unit uncached.
;
; AND THE LETTER LIVES THROUGH WINDOWS.  While the kernel is parked no
; driver can be called, so the unit also says where its sectors are,
; and RMDOS moves them itself out of the same block (RMDISK.INC,
; rd_memraw).  There is nothing to do at a park, nor at a restart: a
; block of memory has no hardware behind it to stop.
;
; It prints one line, which letter the disk got and how big it is,
; because a RAM disk's letter is the thing a person has to know to use
; it.  The size is in K as the DEVICE= line wrote it, ungrouped, since
; a driver cannot ask the country for a separator at CONFIG.SYS time.
;
; Linked with:  wlink form raw bin option offset=0x0, and again with
;               offset=0x10000000; D32DRV makes RAMDISK.SYS of the two
;=====================================================================
        .386P
        INCLUDE PMDOS.INC
        INCLUDE NDRV.INC

RD_KBDEF        EQU     64              ; what RAMDRIVE.SYS makes unasked
RD_KBMIN        EQU     4
RD_KBMAX        EQU     2096896         ; the largest that FAT16 with 32K
                                        ; clusters covers whatever the root:
                                        ; 65,522 clusters at 1024 entries
RD_ENTDEF       EQU     64
RD_ENTMIN       EQU     2
RD_ENTMAX       EQU     1024
RD_FAT12MAX     EQU     4084            ; the MS rule, as FAT.INC reads it
RD_FAT16MIN     EQU     4085
RD_FAT16MAX     EQU     65524
RD_SPCMAX       EQU     64              ; 32K, CLUSBUF_SIZE in FAT.INC
RD_TOKMAX       EQU     20              ; of a bad word, what is repeated

; the extended BPB, after the fields PMDOS.INC names
RDB_DRIVE       EQU     24h             ; db
RDB_EXTSIG      EQU     26h             ; db 29h
RDB_SERIAL      EQU     27h             ; dd
RDB_LABEL       EQU     2Bh             ; 11 bytes
RDB_FSTYPE      EQU     36h             ; 8 bytes
RDB_SIG         EQU     1FEh            ; dw 0AA55h
RDD_STAMP       EQU     16h             ; a directory entry's time, and its
                                        ; date behind it: NDS_STAMP's dword

_RD     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_RD, DS:_RD, ES:_RD, SS:_RD
        ORG     0

image:
        D32_DRVHEAD <OFFSET drvtab>, 0

;--- the driver table, a disk driver's: version 2 and the unit fields --
drvtab:
        dd      NDT_SIGVAL
        dw      NDT_DISKVER
        dw      NDTF_DISK
        db      'RAMDISK '
        dd      OFFSET rd_init
        dd      0                       ; PARK: memory keeps
        dd      0                       ; HALT: nothing is running
        dd      0                       ; CALL: nothing to ask
        dd      0                       ; NDT_NUNITS, which INIT sets
        dd      OFFSET unit             ; NDT_UNITS
IF ($ - drvtab) NE NDT_DLEN
        .ERR    <RAMDISK: the driver table is not NDT_DLEN bytes>
ENDIF

;--- the one unit (NDU_*) ----------------------------------------------
unit:
        dd      OFFSET rd_io            ; NDU_IO
        dd      0                       ; NDU_SECS
        dd      0                       ; NDU_MEM
        db      MEDIA_F8                ; NDU_MEDIA
        db      NDUF_MEMORY             ; NDU_FLAGS
        db      0FFh                    ; NDU_LETTER, the kernel's to write
        db      0
IF ($ - unit) NE NDU_LEN
        .ERR    <RAMDISK: the unit is not NDU_LEN bytes>
ENDIF

;=====================================================================
; rd_io - NDU_IO.  AL = 0 read, 1 write; EDX -> the unit; EBX = the
;         first sector; ECX = how many; EDI -> the kernel's buffer.
;
; The kernel has held the sectors to NDU_SECS already.  They are held
; to it again here, because it costs four instructions and a wrong
; number in this routine writes somewhere that is not the disk.
;=====================================================================
rd_io:
        mov     esi,ebx
        add     esi,ecx
        jc      rio_nosec
        cmp     esi,[edx+NDU_SECS]
        ja      rio_nosec
        mov     esi,ebx
        shl     esi,9
        add     esi,[edx+NDU_MEM]       ; ESI -> the first sector
        shl     ecx,7                   ; sectors -> dwords
        or      al,al
        jz      rio_move
        xchg    esi,edi                 ; a write: from the buffer to it
rio_move:
        rep     movsd
        clc
        ret
rio_nosec:
        mov     ah,04h                  ; sector not found
        stc
        ret

;=====================================================================
; INIT - ESI -> the services, EDI -> the row, EDX -> the line, ECX =
;        the letters left for units, EBX = the first of them.
;=====================================================================
rd_init:
        mov     [services],esi
        mov     [room],ecx
        mov     [letter],bl
        mov     [freemask],ebp          ; before rd_parse, which counts in EBP
        mov     esi,edx
        call    rd_parse
        jc      rii_refuse              ; rd_parse said why
        call    rd_want                 ; /L:, if it was there
        cmp     dword ptr [room],0
        je      rii_noletter
        call    rd_geometry

        mov     ebx,[secs]
        shl     ebx,9
        mov     esi,[services]
        call    dword ptr [esi+NDS_ALLOC]
        jc      rii_nomem
        mov     [base],eax
        call    rd_format

        mov     eax,[secs]
        mov     dword ptr [unit+NDU_SECS],eax
        mov     eax,[base]
        mov     dword ptr [unit+NDU_MEM],eax
        mov     dword ptr [drvtab+NDT_NUNITS],1

        ; RAM disk E: is 64K
        mov     edi,OFFSET textbuf
        mov     esi,OFFSET msg_made
        call    rd_cat
        mov     al,[letter]
        add     al,'A'
        stosb
        mov     esi,OFFSET msg_made2
        call    rd_cat
        mov     eax,[secs]
        shr     eax,1
        call    rd_dec
        mov     esi,OFFSET msg_kcrlf
        call    rd_cat
        mov     esi,OFFSET textbuf
        call    rd_say
        clc
        ret

rii_noletter:
        mov     esi,OFFSET msg_noletter
        call    rd_say
        jmp     short rii_refuse
rii_nomem:
        mov     edi,OFFSET textbuf
        mov     esi,OFFSET msg_nomem
        call    rd_cat
        mov     eax,[kb]
        call    rd_dec
        mov     esi,OFFSET msg_kcrlf
        call    rd_cat
        mov     esi,OFFSET textbuf
        call    rd_say
rii_refuse:
        stc
        ret

;=====================================================================
; rd_want - /L:x was on the line ([want], 1 = A:).  The kernel said at
;           INIT which letters are free (EBP, kept in [freemask]): one
;           of those is asked for in the unit's NDU_WANT and becomes
;           the letter the message names.  One that is taken, or past
;           the kernel's last letter (never in the mask), is reported
;           and the disk takes the next free letter, as without /L:.
;=====================================================================
rd_want:
        movzx   eax,byte ptr [want]
        or      eax,eax
        jz      rw_out
        dec     eax
        bt      dword ptr [freemask],eax
        jnc     rw_taken
        mov     [letter],al
        inc     eax
        mov     byte ptr [unit+NDU_WANT],al
        mov     dword ptr [room],1      ; a free letter is room, even past
rw_out:                                 ; the last one in use
        ret
rw_taken:
        push    eax
        mov     edi,OFFSET textbuf
        mov     esi,OFFSET msg_inuse
        call    rd_cat                  ; uses AL
        pop     eax
        add     al,'A'
        stosb
        mov     esi,OFFSET msg_inuse2
        call    rd_cat
        mov     esi,OFFSET textbuf
        call    rd_say
        ret

;=====================================================================
; rd_parse - ESI -> the DEVICE= line: the path and what follows it,
;            ended by CR (or LF, or a zero).  Sets [kb] and [entries].
;            CF, with the word it did not like printed, if the line is
;            wrong.
;
; Up to three numbers in RAMDRIVE.SYS's order, and the two switches
; anywhere among them.  A number is all digits and ends at a blank or
; the end of the line, so "64K" and "1,024" are refused rather than
; read as the 64 or the 1 in front of them.
;=====================================================================
rd_parse:
        mov     dword ptr [kb],RD_KBDEF
        mov     dword ptr [entries],RD_ENTDEF
        call    rd_skipbl
        call    rd_skipword             ; the driver's own path
        xor     ebp,ebp                 ; numbers so far
rp_next:
        call    rd_skipbl
        call    rd_atend
        jz      rp_done
        mov     [tokstart],esi
        cmp     byte ptr [esi],'/'
        je      rp_switch
        call    rd_number               ; -> EAX
        jc      rp_bad
        cmp     ebp,0
        je      rp_size
        cmp     ebp,1
        je      rp_secsize
        cmp     ebp,2
        je      rp_entries
        jmp     rp_bad                  ; a fourth number

rp_size:
        cmp     eax,RD_KBMIN
        jb      rp_bad
        cmp     eax,RD_KBMAX
        jbe     rp_sizeok
        mov     eax,RD_KBMAX
rp_sizeok:
        mov     [kb],eax
        jmp     short rp_took
rp_secsize:
        cmp     eax,128
        je      rp_took
        cmp     eax,256
        je      rp_took
        cmp     eax,512
        je      rp_took
        cmp     eax,1024
        je      rp_took
        jmp     rp_bad
rp_entries:
        cmp     eax,RD_ENTMIN
        jb      rp_bad
        cmp     eax,RD_ENTMAX
        ja      rp_bad
        mov     [entries],eax
rp_took:
        inc     ebp
        jmp     rp_next

rp_switch:
        mov     al,[esi+1]
        or      al,20h
        cmp     al,'l'
        je      rp_letter
        cmp     al,'e'
        je      rp_swok
        cmp     al,'a'
        jne     rp_bad
rp_swok:
        add     esi,2
        call    rd_atblank
        jnz     rp_bad
        jmp     rp_next

rp_letter:                              ; /L:x, a letter A to Z
        cmp     byte ptr [esi+2],':'
        jne     rp_bad
        mov     al,[esi+3]
        and     al,0DFh                 ; a to z as A to Z; anything else
        sub     al,'A'                  ; lands outside them
        cmp     al,26
        jae     rp_bad
        inc     al
        mov     [want],al               ; 1 = A:, as NDU_WANT counts
        add     esi,4
        call    rd_atblank
        jnz     rp_bad
        jmp     rp_next

rp_done:
        clc
        ret

rp_bad:
        mov     edi,OFFSET textbuf
        mov     esi,OFFSET msg_badparm
        call    rd_cat
        mov     esi,[tokstart]
        mov     ecx,RD_TOKMAX
rpb_copy:
        call    rd_atblank
        jz      rpb_end
        movsb
        loop    rpb_copy
rpb_end:
        mov     esi,OFFSET msg_crlf
        call    rd_cat
        mov     esi,OFFSET textbuf
        call    rd_say
        stc
        ret

;--- rd_number - ESI -> digits -> EAX, ESI past them; CF if there are
;--- none, or if something other than a blank or the end follows them.
;--- A number too big for 32 bits comes back as FFFFFFFFh, which every
;--- caller then treats as too big for what it is
rd_number:
        xor     eax,eax
        xor     ecx,ecx                 ; digits seen
rn_digit:
        movzx   edx,byte ptr [esi]
        sub     edx,'0'
        cmp     edx,9
        ja      rn_end
        cmp     eax,429496728           ; ten times more, and a digit,
        ja      rn_huge                 ; would not fit in 32 bits
        imul    eax,10
        add     eax,edx
        jmp     short rn_more
rn_huge:
        or      eax,-1
rn_more:
        inc     esi
        inc     ecx
        jmp     short rn_digit
rn_end:
        jecxz   rn_bad
        push    eax                     ; rd_atblank reads into AL
        call    rd_atblank
        pop     eax                     ; and POP leaves its flags alone
        jnz     rn_bad
        clc
        ret
rn_bad:
        stc
        ret

;--- ESI at the end of the line: ZF
rd_atend:
        mov     al,[esi]
        or      al,al
        jz      rae_out
        cmp     al,13
        je      rae_out
        cmp     al,10
rae_out:
        ret

;--- ESI at a blank or at the end: ZF
rd_atblank:
        call    rd_atend
        jz      rab_out
        cmp     al,' '
        je      rab_out
        cmp     al,9
rab_out:
        ret

rd_skipbl:
        cmp     byte ptr [esi],' '
        je      rsb_one
        cmp     byte ptr [esi],9
        jne     rsb_out
rsb_one:
        inc     esi
        jmp     short rd_skipbl
rsb_out:
        ret

rd_skipword:
        call    rd_atblank
        jz      rsw_out
        inc     esi
        jmp     short rd_skipword
rsw_out:
        ret

;=====================================================================
; rd_geometry - [kb] and [entries] -> [secs], [rootsecs], [spc], [spf],
;               [fat16]: the smallest clusters that make a legal
;               volume, FAT12 before FAT16 at each size.
;
; THE ROOT IS AT MOST HALF THE DISK.  1024 entries are 64 sectors, and
; a 16K disk has 32: the root is cut down to what leaves room for
; files, which only a line asking for both extremes ever meets.
;
; With RD_KBMAX as the ceiling a legal size is always found by 64
; sectors a cluster; the loop stops there regardless.
;=====================================================================
rd_geometry:
        mov     eax,[kb]
        shl     eax,1
        mov     [secs],eax
        mov     eax,[entries]
        add     eax,15
        shr     eax,4                   ; 16 entries a sector
        mov     ecx,[secs]
        sub     ecx,2
        shr     ecx,1
        cmp     eax,ecx
        jbe     rg_rootok
        mov     eax,ecx
rg_rootok:
        mov     [rootsecs],eax
        mov     dword ptr [spc],1
rg_spc:
        mov     byte ptr [fat16],0
        call    rg_count
        jc      rg_try16
        cmp     eax,RD_FAT12MAX
        jbe     rg_done
rg_try16:
        mov     byte ptr [fat16],1
        call    rg_count
        jc      rg_more
        cmp     eax,RD_FAT16MIN
        jb      rg_more
        cmp     eax,RD_FAT16MAX
        jbe     rg_done
rg_more:
        cmp     dword ptr [spc],RD_SPCMAX
        jae     rg_done
        shl     dword ptr [spc],1
        jmp     short rg_spc
rg_done:
        ret

;--- rg_count - at [spc] and the FAT type in [fat16]: EAX = the
;--- clusters there are room for, with [spf] the FAT that maps them.
;--- CF if there is no room for any.  The FAT grows until it is big
;--- enough for the clusters left beside it, which can only shrink.
rg_count:
        mov     dword ptr [spf],1
rgc_again:
        mov     eax,[secs]
        dec     eax                     ; the boot sector
        sub     eax,[spf]               ; the one FAT
        jbe     rgc_none
        sub     eax,[rootsecs]
        jbe     rgc_none
        xor     edx,edx
        div     dword ptr [spc]
        or      eax,eax
        jz      rgc_none
        lea     ecx,[eax+2]             ; and the two entries nobody gets
        cmp     byte ptr [fat16],0
        jne     rgc_16
        lea     ecx,[ecx+ecx*2]
        inc     ecx
        shr     ecx,1                   ; a cluster and a half, rounded up
        jmp     short rgc_secs
rgc_16:
        shl     ecx,1
rgc_secs:
        add     ecx,511
        shr     ecx,9
        cmp     ecx,[spf]
        jbe     rgc_fits
        mov     [spf],ecx
        jmp     short rgc_again
rgc_fits:
        clc
        ret
rgc_none:
        stc
        ret

;=====================================================================
; rd_format - the boot sector, the FAT and the root, at [base].
;=====================================================================
rd_format:
        ; the system area to zeros, the data area left alone
        mov     edi,[base]
        mov     ecx,[spf]
        add     ecx,[rootsecs]
        inc     ecx
        shl     ecx,7
        xor     eax,eax
        cld
        rep     stosd

        ; the boot sector
        mov     edi,[base]
        mov     esi,OFFSET boot_tpl
        mov     ecx,BOOT_TPLLEN
        rep     movsb
        mov     edi,[base]
        mov     word ptr [edi+BPB_BPS],512
        mov     al,byte ptr [spc]
        mov     [edi+BPB_SPC],al
        mov     word ptr [edi+BPB_RSVD],1
        mov     byte ptr [edi+BPB_NFATS],1
        mov     eax,[rootsecs]
        shl     eax,4
        mov     [edi+BPB_ROOTENT],ax
        mov     byte ptr [edi+BPB_MEDIA],MEDIA_F8
        mov     eax,[spf]
        mov     [edi+BPB_SPF16],ax
        mov     word ptr [edi+BPB_SECPERTRK],32
        mov     word ptr [edi+BPB_NUMHEADS],2
        mov     eax,[secs]
        cmp     eax,10000h
        jae     rf_big
        mov     [edi+BPB_TOTSEC16],ax
        jmp     short rf_total
rf_big:
        mov     [edi+BPB_TOTSEC32],eax
rf_total:
        mov     esi,[services]          ; a serial number, which changes
        call    dword ptr [esi+NDS_STAMP]       ; with the date and the time
        mov     edx,eax                 ; as FORMAT's does, and with the
        call    dword ptr [esi+NDS_TICKS]       ; ticks, since two disks
        imul    eax,9E3779B1h           ; are made in the same second
        xor     eax,edx
        xor     eax,[base]
        mov     edi,[base]
        mov     [edi+RDB_SERIAL],eax
        mov     esi,OFFSET fs_fat12
        cmp     byte ptr [fat16],0
        je      rf_type
        mov     esi,OFFSET fs_fat16
rf_type:
        push    edi
        add     edi,RDB_FSTYPE
        mov     ecx,8
        rep     movsb
        pop     edi
        mov     word ptr [edi+RDB_SIG],0AA55h

        ; the FAT: the media byte and the two entries nobody gets
        add     edi,512
        mov     word ptr [edi],0FFF8h
        mov     byte ptr [edi+2],0FFh
        cmp     byte ptr [fat16],0
        je      rf_root
        mov     byte ptr [edi+3],0FFh

        ; and the label, first in the root
rf_root:
        mov     eax,[spf]
        shl     eax,9
        add     edi,eax
        mov     esi,OFFSET vol_label
        mov     ecx,11
        rep     movsb
        mov     byte ptr [edi],ATTR_VOLUME
        mov     esi,[services]          ; and when it was made, which is
        call    dword ptr [esi+NDS_STAMP]       ; what CHKDSK says of it
        mov     [edi+RDD_STAMP-11],eax
        ret

;=====================================================================
; Text
;=====================================================================
;--- rd_say - ESI -> ASCIIZ, to the console
rd_say:
        mov     edx,[services]
        call    dword ptr [edx+NDS_PRINT]
        ret

;--- rd_cat - ESI -> ASCIIZ onto EDI, which is left on its zero
rd_cat:
        lodsb
        stosb
        or      al,al
        jnz     rd_cat
        dec     edi
        ret

;--- rd_dec - EAX in decimal onto EDI, ended by a zero it is left on
rd_dec:
        mov     ecx,10
        xor     edx,edx
        push    edx                     ; the end of the digits
rdd_div:
        xor     edx,edx
        div     ecx
        add     dl,'0'
        push    edx
        or      eax,eax
        jnz     rdd_div
rdd_out:
        pop     eax
        stosb
        or      al,al
        jnz     rdd_out
        dec     edi
        ret

;=====================================================================
; Data
;=====================================================================
; The boot sector's first 3Eh bytes and two after them.  Nobody boots
; a RAM disk, but a jump at byte 0 is what says a BPB follows, and the
; INT 18h at 3Eh is the honest answer if something tries.
boot_tpl:
        db      0EBh, 3Ch, 90h
        db      'RAMDISK '              ; 03h
        db      25 dup(0)               ; 0Bh-23h: the BPB, filled in
        db      0, 0, 29h               ; 24h drive, 25h, 26h signature
        dd      0                       ; 27h serial number
vol_label:
        db      'RAMDISK    '           ; 2Bh label
        db      'FAT12   '              ; 36h, made FAT16 when it is
        db      0CDh, 18h               ; 3Eh int 18h
BOOT_TPLLEN     EQU     $ - boot_tpl
IF (vol_label - boot_tpl) NE RDB_LABEL
        .ERR    <RAMDISK: the label is not at 2Bh in the boot sector>
ENDIF
fs_fat12    db  'FAT12   '
fs_fat16    db  'FAT16   '

msg_made    db  'RAM disk ',0
msg_made2   db  ': is ',0
msg_kcrlf   db  'K'
msg_crlf    db  13,10,0
msg_badparm db  'RAMDISK.SYS: invalid parameter - ',0
msg_noletter db 'RAMDISK.SYS: no drive letter is left',13,10,0
msg_nomem   db  'RAMDISK.SYS: not enough memory for ',0
msg_inuse   db  'RAMDISK.SYS: drive ',0
msg_inuse2  db  ': cannot be used',13,10,0

services    dd  0
room        dd  0
kb          dd  0
entries     dd  0
secs        dd  0                       ; the volume's 512-byte sectors
rootsecs    dd  0
spc         dd  0
spf         dd  0
base        dd  0                       ; where they are
tokstart    dd  0
freemask    dd  0                       ; the letters free at INIT, bit 0 = A:
letter      db  0
fat16       db  0
want        db  0                       ; /L:, 1 = A:; 0 = not given
            db  0
textbuf     db  80 dup(0)

_RD     ENDS
        END
