;=====================================================================
; EXFMT.INC - an exFAT volume laid down from nothing
;
; Shared by FORMAT.ASM and FDISK.ASM, which each reach a volume's
; sectors their own way - and in INC, the one directory both take
; includes from.  The program including this supplies:
;
;   exf_wr      EDX = a sector of the volume, ECX = how many, EBX -> the
;               data.  CF on failure; every register kept.
;   exf_rd      the same, reading: only exf_markbad and exf_setlabel
;               call it
;   exf_buf     dd - a work buffer of EXF_BUFLEN bytes
;
; and fills in, before exf_layout:
;
;   exf_len     the volume's sectors (512 bytes each, as the kernel's
;               disks have them)
;   exf_start   where it starts on its disk: the boot sector says so
;   exf_cbytes  the cluster size wanted, 0 for Microsoft's choice
;
; exf_layout works the rest out, CF if no exFAT volume fits; exf_write
; puts it on the disk.  exf_markbad and exf_setlabel come after, for a
; surface check and a label.
;
; WHAT GOES DOWN, in order: the FAT region, the cluster heap's three
; system files - the allocation bitmap, the up-case table, the root
; directory - and LAST the two boot regions, so that a volume cut short
; by a failure is not one: without its boot sector it is not exFAT at
; all.  The bitmap first in the heap and the up-case table and root
; after it is the order Windows writes them in.
;
; THE UP-CASE TABLE IS A SHORT ONE: a-z, and the Latin-1 letters, which
; is every character a kernel name can hold.  A volume carries its own
; table and every implementation reads the volume's, so a short one is
; as correct as Windows's long one - names hash by it, and compare by
; it - it only folds fewer of the characters nobody here can type.
;=====================================================================

EXF_BUFLEN      EQU     16384           ; what exf_buf must hold
EXF_FATAT      EQU     128             ; the FAT: past both boot regions,
EXF_FATAT0     EQU     24              ; or right after them if small
EXF_MINSECS     EQU     2048            ; 1MB: smaller is not worth it
EXF_JMPLEN      EQU     11              ; the jump and the name, at the front
EXF_STUBLEN     EQU     92              ; the code at 78h and its message
EXF_UPLEN       EQU     134             ; the up-case table, compressed

;---------------------------------------------------------------------
; exf_layout - the layout for exf_len sectors.  CF if none fits.
;---------------------------------------------------------------------
exf_layout:
        pushad
        mov     eax,[exf_len]
        cmp     eax,EXF_MINSECS
        jb      exl_no
        ; THE CLUSTER: as asked, or as Windows chooses it - 4K up to
        ; 256MB, 32K up to 32GB, 128K above
        mov     ecx,[exf_cbytes]
        or      ecx,ecx
        jnz     exl_csz
        mov     ecx,4096
        cmp     eax,256 * 2048
        jbe     exl_csz
        mov     ecx,32768
        cmp     eax,32 * 1024 * 2048
        jbe     exl_csz
        mov     ecx,131072
exl_csz:
        ; a power of two from 512 bytes to 32MB
        cmp     ecx,512
        jb      exl_no
        cmp     ecx,32 * 1024 * 1024
        ja      exl_no
        lea     edx,[ecx-1]
        test    edx,ecx
        jnz     exl_no
        mov     [exf_cbytes],ecx
        shr     ecx,9
        mov     [exf_spc],ecx
        bsf     edx,ecx
        mov     [exf_spcsh],dl

        ; THE FAT AND THE HEAP, settled: the FAT's length depends on the
        ; clusters, and the clusters on where the heap starts after it
        mov     ebx,EXF_FATAT
        cmp     eax,4096
        jae     exl_off
        mov     ebx,EXF_FATAT0
exl_off:
        mov     [exf_fatoff],ebx
        mov     dword ptr [exf_fatlen],1
        mov     ebp,8                   ; a few rounds settle it
exl_settle:
        mov     eax,[exf_fatoff]
        add     eax,[exf_fatlen]
        mov     ecx,[exf_spc]
        dec     ecx
        add     eax,ecx
        not     ecx
        and     eax,ecx                 ; the heap, on a cluster boundary
        mov     [exf_heap],eax
        mov     edx,[exf_len]
        sub     edx,eax
        jbe     exl_no
        mov     eax,edx
        xor     edx,edx
        div     dword ptr [exf_spc]
        mov     [exf_nclus],eax
        add     eax,2                   ; entries, four bytes, 128 a sector
        add     eax,127
        shr     eax,7
        cmp     eax,[exf_fatlen]
        jbe     exl_settled
        mov     [exf_fatlen],eax
        dec     ebp
        jnz     exl_settle
        jmp     exl_no
exl_settled:
        cmp     dword ptr [exf_nclus],16        ; a volume of fewer clusters
        jb      exl_no                          ; than that is not worth one

        ; the system files' clusters: the bitmap, the up-case table, the
        ; root, one after the other from cluster 2
        mov     eax,[exf_nclus]
        add     eax,7
        shr     eax,3
        mov     [exf_bmlen],eax
        call    exl_clusof              ; -> EAX clusters for EAX bytes
        mov     [exf_bmclus],eax
        add     eax,2
        mov     [exf_upfirst],eax
        mov     eax,EXF_UPLEN
        call    exl_clusof
        add     eax,[exf_upfirst]
        mov     [exf_rootclus],eax
        inc     eax
        sub     eax,2
        mov     [exf_used],eax          ; clusters the three take
        cmp     eax,[exf_nclus]
        jae     exl_no
        popad
        clc
        ret
exl_no:
        popad
        stc
        ret

; exl_clusof - EAX bytes -> EAX clusters, rounded up
exl_clusof:
        push    ecx
        push    edx
        mov     ecx,[exf_cbytes]
        add     eax,ecx
        dec     eax
        xor     edx,edx
        div     ecx
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; exf_write - the volume exf_layout laid out, onto the disk through
;             exf_wr: FAT, bitmap, up-case table, root, boot regions.
;             exf_serial is the serial number.  CF on failure.
;---------------------------------------------------------------------
exf_write:
        pushad
        ; the FAT: every sector zero but the first, which has the two
        ; reserved entries and the system files' chains
        mov     eax,[exf_fatoff]
        mov     ecx,[exf_fatlen]
        call    exf_zero
        jc      exw_fail
        call    exf_fatchains
        jc      exw_fail

        ; THE BITMAP: the system files' clusters in use, nothing else -
        ; and on a big enough volume the bitmap alone takes more clusters
        ; than one of its sectors has bits for, so a sector at a time
        mov     eax,2
        call    exf_clus2sec
        mov     ecx,[exf_bmclus]
        imul    ecx,[exf_spc]
        call    exf_zero
        jc      exw_fail
        mov     eax,2
        call    exf_clus2sec
        mov     edx,eax                 ; EDX = the bitmap sector
        xor     ebp,ebp                 ; EBP = its first bit's cluster - 2
exw_bmsec:
        cmp     ebp,[exf_used]
        jae     exw_bmdone
        mov     edi,[exf_buf]
        push    edi
        xor     eax,eax
        mov     ecx,128
        rep     stosd
        pop     edi
        xor     ecx,ecx
exw_bits:
        lea     eax,[ebp+ecx]
        cmp     eax,[exf_used]
        jae     exw_bitsdone
        bts     dword ptr [edi],ecx
        inc     ecx
        cmp     ecx,512 * 8
        jb      exw_bits
exw_bitsdone:
        mov     ebx,[exf_buf]
        mov     ecx,1
        call    exf_wr
        jc      exw_fail
        inc     edx
        add     ebp,512 * 8
        jmp     exw_bmsec
exw_bmdone:

        ; the up-case table, in the first sector of its cluster run
        mov     eax,EXF_UPLEN
        call    exl_clusof
        imul    eax,[exf_spc]
        mov     ecx,eax                 ; its whole clusters zeroed
        mov     eax,[exf_upfirst]
        call    exf_clus2sec
        call    exf_zero
        jc      exw_fail
        mov     edi,[exf_buf]
        push    edi
        xor     eax,eax
        mov     ecx,128
        rep     stosd
        pop     edi
        mov     esi,OFFSET exf_uptab
        mov     ecx,EXF_UPLEN
        rep     movsb
        mov     eax,[exf_upfirst]
        call    exf_clus2sec
        mov     edx,eax
        mov     ebx,[exf_buf]
        mov     ecx,1
        call    exf_wr
        jc      exw_fail

        ; the root: a cluster of zeros, and the three entries in front -
        ; the label (none yet), the bitmap's, the up-case table's
        mov     eax,[exf_rootclus]
        call    exf_clus2sec
        mov     ecx,[exf_spc]
        call    exf_zero
        jc      exw_fail
        call    exf_rootsec             ; -> the first sector, in exf_buf
        mov     eax,[exf_rootclus]
        call    exf_clus2sec
        mov     edx,eax
        mov     ebx,[exf_buf]
        mov     ecx,1
        call    exf_wr
        jc      exw_fail

        ; LAST, the boot regions: the backup, and then the main one
        call    exf_bootreg
        mov     edx,XFB_REGION
        mov     ebx,[exf_buf]
        mov     ecx,XFB_REGION
        call    exf_wr
        jc      exw_fail
        xor     edx,edx
        mov     ecx,XFB_REGION
        call    exf_wr
        jc      exw_fail
        popad
        clc
        ret
exw_fail:
        popad
        stc
        ret

; exl_secsof - ECX bytes -> ECX sectors, rounded up
exl_secsof:
        add     ecx,511
        shr     ecx,9
        ret

; exf_clus2sec - EAX = a cluster -> EAX = its first sector in the volume
exf_clus2sec:
        sub     eax,2
        imul    eax,[exf_spc]
        add     eax,[exf_heap]
        ret

;---------------------------------------------------------------------
; exf_zero - ECX sectors of zeros from sector EAX.  CF on failure.
;---------------------------------------------------------------------
exf_zero:
        pushad
        mov     edi,[exf_buf]
        push    eax
        push    ecx
        xor     eax,eax
        mov     ecx,EXF_BUFLEN / 4
        rep     stosd
        pop     ecx
        pop     eax
        mov     edx,eax
exz_lp:
        or      ecx,ecx
        jz      exz_ok
        mov     ebp,EXF_BUFLEN / 512
        cmp     ecx,ebp
        jae     exz_have
        mov     ebp,ecx
exz_have:
        push    ecx
        mov     ebx,[exf_buf]
        mov     ecx,ebp
        call    exf_wr
        pop     ecx
        jc      exz_fail
        add     edx,ebp
        sub     ecx,ebp
        jmp     exz_lp
exz_ok:
        popad
        clc
        ret
exz_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; exf_fatchains - the FAT's first sectors: entries 0 and 1, and each
;                 system file a chain of its clusters.  The three are
;                 all within the first sectors, which are written as
;                 far as the last of them reaches.  CF on failure.
;---------------------------------------------------------------------
exf_fatchains:
        pushad
        xor     ebp,ebp                 ; EBP = the FAT sector being built
exc_sec:
        mov     edi,[exf_buf]
        push    edi
        xor     eax,eax
        mov     ecx,128
        rep     stosd
        pop     edi
        mov     ebx,ebp
        shl     ebx,7                   ; the first entry in this sector
        xor     ecx,ecx                 ; ECX = entry in the sector
exc_ent:
        lea     eax,[ebx+ecx]           ; EAX = the cluster
        call    exc_value               ; -> EDX
        mov     [edi+ecx*4],edx
        inc     ecx
        cmp     ecx,128
        jb      exc_ent
        mov     edx,[exf_fatoff]
        add     edx,ebp
        push    ebx
        mov     ebx,[exf_buf]
        mov     ecx,1
        call    exf_wr
        pop     ebx
        jc      exc_fail
        inc     ebp
        mov     eax,[exf_used]
        add     eax,2                   ; the entries the chains reach
        shr     eax,7
        cmp     ebp,eax
        jbe     exc_sec
        popad
        clc
        ret
exc_fail:
        popad
        stc
        ret

; exc_value - EAX = a cluster -> EDX = its first FAT entry
exc_value:
        mov     edx,0FFFFFFF8h          ; entry 0: the media
        or      eax,eax
        jz      exv_out
        mov     edx,XF_EOC              ; entry 1
        cmp     eax,1
        je      exv_out
        xor     edx,edx                 ; past the system files: free
        mov     esi,[exf_used]
        add     esi,2
        cmp     eax,esi
        jae     exv_out
        ; the last cluster of each of the three ends its chain
        lea     edx,[eax+1]
        mov     esi,[exf_upfirst]
        dec     esi
        cmp     eax,esi                 ; the bitmap's last
        je      exv_eoc
        mov     esi,[exf_rootclus]
        dec     esi
        cmp     eax,esi                 ; the up-case table's last
        je      exv_eoc
        cmp     eax,[exf_rootclus]      ; the root's one
        jne     exv_out
exv_eoc:
        mov     edx,XF_EOC
exv_out:
        ret

;---------------------------------------------------------------------
; exf_rootsec - the root directory's first sector, in exf_buf: the
;               label (exf_lcount characters of exf_lchars), the bitmap,
;               the up-case table
;---------------------------------------------------------------------
exf_rootsec:
        pushad
        mov     edi,[exf_buf]
        push    edi
        xor     eax,eax
        mov     ecx,128
        rep     stosd
        pop     edi
        mov     byte ptr [edi],XFT_LABEL
        movzx   ecx,byte ptr [exf_lcount]
        mov     [edi+XFE_LCOUNT],cl
        lea     ebx,[edi+XFE_CHARS]
        mov     esi,OFFSET exf_lchars
exr_lch:
        jecxz   exr_ldone
        movzx   eax,byte ptr [esi]
        mov     [ebx],ax
        inc     esi
        add     ebx,2
        dec     ecx
        jmp     exr_lch
exr_ldone:
        add     edi,32
        mov     byte ptr [edi],XFT_BITMAP
        mov     dword ptr [edi+XFE_FIRST],2
        mov     eax,[exf_bmlen]
        mov     [edi+XFE_DLEN],eax
        add     edi,32
        mov     byte ptr [edi],XFT_UPCASE
        call    exf_upsum
        mov     [edi+XFE_UPSUM],eax
        mov     eax,[exf_upfirst]
        mov     [edi+XFE_FIRST],eax
        mov     dword ptr [edi+XFE_DLEN],EXF_UPLEN
        popad
        ret

; exf_upsum - EAX = the up-case table's checksum
exf_upsum:
        push    ebx
        push    ecx
        push    esi
        mov     esi,OFFSET exf_uptab
        mov     ecx,EXF_UPLEN
        xor     eax,eax
exs_lp:
        movzx   ebx,byte ptr [esi]
        ror     eax,1
        add     eax,ebx
        inc     esi
        loop    exs_lp
        pop     esi
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; exf_bootreg - a boot region in exf_buf: the boot sector, eight
;               extended boot sectors, the OEM parameters and a reserved
;               sector, and the checksum sector.  12 sectors.
;---------------------------------------------------------------------
exf_bootreg:
        pushad
        mov     edi,[exf_buf]
        push    edi
        xor     eax,eax
        mov     ecx,(XFB_REGION * 512) / 4
        rep     stosd
        pop     edi
        mov     esi,OFFSET exf_bootcode         ; the jump, the name, and
        push    edi                             ; the code that says it is
        mov     ecx,EXF_JMPLEN                  ; not a system disk
        rep     movsb
        pop     edi
        push    edi
        add     edi,78h
        mov     esi,OFFSET exf_stub
        mov     ecx,EXF_STUBLEN
        rep     movsb
        pop     edi
        mov     eax,[exf_start]
        mov     [edi+XFB_PARTOFF],eax
        mov     eax,[exf_len]
        mov     [edi+XFB_VOLLEN],eax
        mov     eax,[exf_fatoff]
        mov     [edi+XFB_FATOFF],eax
        mov     eax,[exf_fatlen]
        mov     [edi+XFB_FATLEN],eax
        mov     eax,[exf_heap]
        mov     [edi+XFB_HEAP],eax
        mov     eax,[exf_nclus]
        mov     [edi+XFB_NCLUS],eax
        mov     eax,[exf_rootclus]
        mov     [edi+XFB_ROOT],eax
        mov     eax,[exf_serial]
        mov     [edi+XFB_SERIAL],eax
        mov     word ptr [edi+XFB_REV],0100h
        mov     byte ptr [edi+XFB_BPSSH],9
        mov     al,[exf_spcsh]
        mov     [edi+XFB_SPCSH],al
        mov     byte ptr [edi+XFB_NFATS],1
        mov     byte ptr [edi+XFB_DRIVE],80h
        mov     word ptr [edi+1FEh],0AA55h
        ; the extended boot sectors carry a signature each
        mov     ecx,8
        lea     ebx,[edi+512]
exb_ext:
        mov     dword ptr [ebx+508],0AA550000h
        add     ebx,512
        loop    exb_ext
        ; the checksum, over the first eleven: the flags and the
        ; percentage in use left out, as they may change
        xor     eax,eax
        xor     ecx,ecx
exb_sum:
        cmp     ecx,XFB_FLAGS
        je      exb_skip
        cmp     ecx,XFB_FLAGS + 1
        je      exb_skip
        cmp     ecx,XFB_PCT
        je      exb_skip
        movzx   edx,byte ptr [edi+ecx]
        ror     eax,1
        add     eax,edx
exb_skip:
        inc     ecx
        cmp     ecx,11 * 512
        jb      exb_sum
        lea     ebx,[edi+11 * 512]
        mov     ecx,128
exb_fill:
        mov     [ebx],eax
        add     ebx,4
        loop    exb_fill
        popad
        ret

;---------------------------------------------------------------------
; exf_markbad - cluster EAX does not read: FFFFFFF7h in the FAT, its bit
;               set in the bitmap, so it is never given to a file.  Both
;               read and written back through exf_rd / exf_wr.  CF on
;               failure.
;---------------------------------------------------------------------
exf_markbad:
        pushad
        mov     ebp,eax
        ; the FAT entry
        mov     edx,eax
        shr     edx,7
        add     edx,[exf_fatoff]
        mov     ebx,[exf_buf]
        mov     ecx,1
        call    exf_rd
        jc      exm_fail
        mov     esi,eax
        and     esi,127
        mov     edi,[exf_buf]
        mov     dword ptr [edi+esi*4],XF_BAD
        call    exf_wr
        jc      exm_fail
        ; the bit
        mov     eax,ebp
        sub     eax,2
        mov     esi,eax
        shr     eax,12                  ; 4096 bits a sector
        mov     edx,eax
        push    edx
        mov     eax,2
        call    exf_clus2sec
        pop     edx
        add     edx,eax
        call    exf_rd
        jc      exm_fail
        and     esi,4095
        bts     dword ptr [edi],esi
        call    exf_wr
        jc      exm_fail
        popad
        clc
        ret
exm_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; exf_setlabel - the root's label entry made exf_lcount characters of
;                exf_lchars, in place.  CF on failure.
;---------------------------------------------------------------------
exf_setlabel:
        pushad
        mov     eax,[exf_rootclus]
        call    exf_clus2sec
        mov     edx,eax
        mov     ebx,[exf_buf]
        mov     ecx,1
        call    exf_rd
        jc      exsl_fail
        mov     edi,[exf_buf]
        cmp     byte ptr [edi],XFT_LABEL
        jne     exsl_fail
        push    edi
        add     edi,XFE_CHARS
        xor     eax,eax
        mov     ecx,11
        rep     stosw
        pop     edi
        movzx   ecx,byte ptr [exf_lcount]
        mov     [edi+XFE_LCOUNT],cl
        lea     ebx,[edi+XFE_CHARS]
        mov     esi,OFFSET exf_lchars
exsl_ch:
        jecxz   exsl_put
        movzx   eax,byte ptr [esi]
        mov     [ebx],ax
        inc     esi
        add     ebx,2
        dec     ecx
        jmp     exsl_ch
exsl_put:
        mov     ebx,[exf_buf]
        mov     ecx,1
        call    exf_wr
        jc      exsl_fail
        popad
        clc
        ret
exsl_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; Data
;---------------------------------------------------------------------
exf_len     dd  0                       ; the volume's sectors
exf_start   dd  0                       ; where it starts on its disk
exf_cbytes  dd  0                       ; bytes a cluster, 0 = the default
exf_serial  dd  0
exf_spc     dd  0                       ; sectors a cluster
exf_fatoff  dd  0
exf_fatlen  dd  0
exf_heap    dd  0
exf_nclus   dd  0
exf_bmlen   dd  0                       ; the bitmap's bytes
exf_bmclus  dd  0                       ; ...and clusters
exf_upfirst dd  0                       ; the up-case table's first cluster
exf_rootclus dd 0
exf_used    dd  0                       ; clusters the three take
exf_spcsh   db  0
exf_lcount  db  0                       ; the label's characters
exf_lchars  db  11 dup(0)               ; ...one byte each
            db  0

; the boot sector's first eleven bytes, and its code at 78h: the jump
; over the fields, which say nothing to a processor; the same stub that
; FORMAT's diskettes carry, set up a stack, print, wait for a key, boot
; again
exf_bootcode db 0EBh, 76h, 90h, 'EXFAT   '
IF ($ - exf_bootcode) NE EXF_JMPLEN
        .ERR    EXF_JMPLEN is not the length of exf_bootcode
ENDIF
exf_stub    db      0FAh                    ; 78h cli
            db      31h, 0C0h               ;     xor ax,ax
            db      8Eh, 0D0h               ;     mov ss,ax
            db      0BCh, 00h, 7Ch          ;     mov sp,7C00h
            db      0FBh                    ;     sti
            db      8Eh, 0D8h               ;     mov ds,ax
            db      0BEh, 9Bh, 7Ch          ;     mov si,7C9Bh
            db      0FCh                    ;     cld
            db      0ACh                    ; 87h lodsb
            db      08h, 0C0h               ;     or al,al
            db      74h, 09h                ;     jz 95h
            db      0B4h, 0Eh               ;     mov ah,0Eh
            db      0BBh, 07h, 00h          ;     mov bx,7
            db      0CDh, 10h               ;     int 10h
            db      0EBh, 0F2h              ;     jmp 87h
            db      30h, 0E4h               ; 95h xor ah,ah
            db      0CDh, 16h               ;     int 16h
            db      0CDh, 19h               ;     int 19h
            db      'This is not a system disk.',13,10
            db      'Change it and press a key.',13,10,0
IF ($ - exf_stub) NE EXF_STUBLEN
        .ERR    EXF_STUBLEN is not the length of exf_stub
ENDIF

; the up-case table, compressed: FFFFh and a count is that many
; characters that are their own capital
exf_uptab   dw      0FFFFh, 61h                         ; 00-60
            dw      41h, 42h, 43h, 44h, 45h, 46h, 47h   ; a-z
            dw      48h, 49h, 4Ah, 4Bh, 4Ch, 4Dh, 4Eh
            dw      4Fh, 50h, 51h, 52h, 53h, 54h, 55h
            dw      56h, 57h, 58h, 59h, 5Ah
            dw      0FFFFh, 3Ah                         ; 7B-B4
            dw      39Ch                                ; B5: micro
            dw      0FFFFh, 2Ah                         ; B6-DF
            dw      0C0h, 0C1h, 0C2h, 0C3h, 0C4h, 0C5h  ; E0-F6
            dw      0C6h, 0C7h, 0C8h, 0C9h, 0CAh, 0CBh
            dw      0CCh, 0CDh, 0CEh, 0CFh, 0D0h, 0D1h
            dw      0D2h, 0D3h, 0D4h, 0D5h, 0D6h
            dw      0F7h                                ; F7: division
            dw      0D8h, 0D9h, 0DAh, 0DBh, 0DCh, 0DDh  ; F8-FE
            dw      0DEh
            dw      178h                                ; FF: y diaeresis
            dw      0FFFFh, 0FF00h                      ; 100-FFFF
IF ($ - exf_uptab) NE EXF_UPLEN
        .ERR    EXF_UPLEN is not the length of exf_uptab
ENDIF
