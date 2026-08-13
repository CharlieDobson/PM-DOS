;=====================================================================
; FATLOAD.INC - real-mode FAT12/16/32 file loader
;
; Included by BOOT\BOOT2.ASM (the hard-disk stage 2, which loads
; PMIO.SYS) and by IO\IORM.ASM (which loads PMDOS.SYS).  One copy of
; the chain-walking logic serves both, so the system files never have
; to be contiguous on any of the three filesystems.
;
; The FAT is windowed TWO SECTORS at a time.  Two rather than one
; because a FAT12 entry can straddle a sector boundary: with the
; following sector already in the buffer, the word read at offset 511
; simply spills into it and needs no special case at all.
;
; Contract:
;   - the volume's BPB is readable at FL_BPBSEG:0000 (the boot sector
;     the BIOS left at 0000:7C00)
;   - DS addresses this code's own data; ES is set to 0 internally
;   - [fl_drive] holds the BIOS drive number
;   - call fl_init once, then fl_find / fl_next / fl_c2l / fl_read
;
; All LBAs are 32-bit and absolute (the partition's hidden-sector
; count is folded in), so a volume may sit anywhere on the disk.
; 386 instructions are used throughout - PM-DOS requires one.
;=====================================================================

; BPB field offsets
FLB_BPS         EQU     0Bh
FLB_SPC         EQU     0Dh
FLB_RSVD        EQU     0Eh
FLB_NFATS       EQU     10h
FLB_ROOTENT     EQU     11h
FLB_TOTSEC16    EQU     13h
FLB_SPF16       EQU     16h
FLB_SPT         EQU     18h
FLB_HEADS       EQU     1Ah
FLB_HIDDEN      EQU     1Ch
FLB_TOTSEC32    EQU     20h
FLB_SPF32       EQU     24h
FLB_ROOTCLUS    EQU     2Ch

; directory entry offsets
FLD_CLUSHI      EQU     14h
FLD_CLUSLO      EQU     1Ah
FLD_SIZE        EQU     1Ch

;=====================================================================
; fl_init - read the BPB and work out the volume's geometry.
;           Clobbers nothing the caller cares about.
;=====================================================================
fl_init:
        pushad
        push    es
        mov     ax,FL_BPBSEG
        mov     es,ax

        movzx   eax,byte ptr es:[FLB_SPC]
        mov     [fl_spc],eax
        movzx   eax,word ptr es:[FLB_SPT]
        mov     [fl_spt],eax
        movzx   eax,word ptr es:[FLB_HEADS]
        mov     [fl_heads],eax
        movzx   eax,word ptr es:[FLB_ROOTENT]
        mov     [fl_rootent],eax

        ; sectors per FAT: the 16-bit field, or the FAT32 one
        movzx   eax,word ptr es:[FLB_SPF16]
        or      eax,eax
        jnz     fi_spf
        mov     eax,dword ptr es:[FLB_SPF32]
fi_spf:
        mov     [fl_spf],eax

        ; total sectors: likewise
        movzx   eax,word ptr es:[FLB_TOTSEC16]
        or      eax,eax
        jnz     fi_tot
        mov     eax,dword ptr es:[FLB_TOTSEC32]
fi_tot:
        mov     [fl_totsec],eax

        mov     eax,dword ptr es:[FLB_HIDDEN]
        mov     [fl_hidden],eax
        mov     eax,dword ptr es:[FLB_ROOTCLUS]
        mov     [fl_rootclus],eax

        ; first FAT sector
        movzx   ecx,word ptr es:[FLB_RSVD]
        mov     [fl_rsvd],ecx
        mov     eax,[fl_hidden]
        add     eax,ecx
        mov     [fl_fatstart],eax

        ; root area = fatstart + nfats * spf
        movzx   eax,byte ptr es:[FLB_NFATS]
        mul     dword ptr [fl_spf]
        mov     [fl_fatsecs],eax                ; all copies together
        add     eax,[fl_fatstart]
        mov     [fl_rootlba],eax

        ; root directory sectors (zero on FAT32)
        mov     eax,[fl_rootent]
        shr     eax,4                           ; * 32 / 512
        mov     [fl_rootsecs],eax
        add     eax,[fl_rootlba]
        mov     [fl_datastart],eax

        ; cluster count decides the FAT width (the Microsoft rule)
        mov     eax,[fl_datastart]
        sub     eax,[fl_hidden]                 ; -> volume-relative
        mov     ecx,[fl_totsec]
        sub     ecx,eax
        mov     eax,ecx
        xor     edx,edx
        div     dword ptr [fl_spc]

        mov     byte ptr [fl_type],12
        mov     dword ptr [fl_eoc],0FF8h
        cmp     eax,4085
        jb      fi_typed
        mov     byte ptr [fl_type],16
        mov     dword ptr [fl_eoc],0FFF8h
        cmp     eax,65525
        jb      fi_typed
        mov     byte ptr [fl_type],32
        mov     dword ptr [fl_eoc],0FFFFFF8h
fi_typed:
        mov     dword ptr [fl_fatwin],-1        ; nothing cached yet
        pop     es
        popad
        ret

;=====================================================================
; fl_read - read ECX sectors from LBA EAX into ES:BX.
;
; BX comes back advanced past the data - the one register that must
; survive the wholesale PUSHAD/POPAD, so callers can keep feeding it
; back in to load a file cluster by cluster.
;
; Retries three times, resetting the controller between tries.
;=====================================================================
fl_read:
        pushad
fr_one:
        push    ecx
        mov     di,3
fr_try:
        push    di
        push    eax                     ; INT 13h returns status in AX,
                                        ; which would otherwise eat the LBA
        movzx   ecx,word ptr [fl_spt]
        xor     edx,edx
        div     ecx                     ; EAX = track, EDX = sector-1
        mov     cl,dl
        inc     cl                      ; CL = sector, 1-based
        xor     edx,edx
        div     dword ptr [fl_heads]    ; EAX = cylinder, EDX = head
        mov     dh,dl
        mov     dl,[fl_drive]
        mov     ch,al                   ; cylinder bits 0-7
        shr     eax,8
        and     al,3
        shl     al,6                    ; cylinder bits 8-9 -> CL bits 7-6
        or      cl,al
        mov     ax,0201h
        int     13h
        pop     eax
        pop     di
        jnc     fr_ok

        push    ax                      ; the reset call eats AX as well
        xor     ax,ax
        mov     dl,[fl_drive]
        int     13h
        pop     ax
        dec     di
        jnz     fr_try
        pop     ecx
        mov     [fl_bxsave],bx
        popad
        mov     bx,[fl_bxsave]
        stc
        ret
fr_ok:
        pop     ecx
        inc     eax
        add     bx,512
        loop    fr_one
        mov     [fl_bxsave],bx
        popad
        mov     bx,[fl_bxsave]
        clc
        ret

;=====================================================================
; fl_c2l - EAX = cluster -> EAX = the LBA of its first sector.
;=====================================================================
fl_c2l:
        push    ecx
        push    edx
        sub     eax,2
        mul     dword ptr [fl_spc]
        add     eax,[fl_datastart]
        pop     edx
        pop     ecx
        ret

;=====================================================================
; fl_next - EAX = cluster -> EAX = its successor.  A value at or above
;           [fl_eoc] ends the chain.  ES is left at 0.
;=====================================================================
fl_next:
        push    ebx
        push    ecx
        push    edx
        push    es
        xor     cx,cx
        mov     es,cx

        ; byte offset of this cluster's entry within the FAT
        cmp     byte ptr [fl_type],12
        je      fn_off12
        mov     ecx,eax
        shl     ecx,1
        cmp     byte ptr [fl_type],16
        je      fn_have_off
        shl     ecx,1                   ; FAT32: four bytes per entry
        jmp     short fn_have_off
fn_off12:
        mov     ecx,eax                 ; FAT12: one and a half
        shr     ecx,1
        add     ecx,eax
fn_have_off:

        ; make sure the two-sector window covers it
        mov     ebx,ecx
        shr     ebx,9
        cmp     ebx,[fl_fatwin]
        je      fn_windowed
        mov     [fl_fatwin],ebx
        push    eax
        push    ecx
        mov     eax,ebx
        add     eax,[fl_fatstart]
        mov     bx,FL_FATBUF
        mov     cx,2
        call    fl_read
        pop     ecx
        pop     eax
        jc      fn_ioerr
fn_windowed:
        and     ecx,511
        mov     bx,cx

        cmp     byte ptr [fl_type],12
        je      fn_get12
        cmp     byte ptr [fl_type],16
        je      fn_get16
        mov     eax,dword ptr es:[bx+FL_FATBUF]
        and     eax,0FFFFFFFh           ; the top nibble is reserved
        jmp     short fn_out
fn_get16:
        movzx   eax,word ptr es:[bx+FL_FATBUF]
        jmp     short fn_out
fn_get12:
        movzx   edx,word ptr es:[bx+FL_FATBUF]  ; may straddle into the
        test    al,1                            ; second cached sector
        jz      fn_even
        shr     edx,4                   ; odd cluster: the high 12 bits
        jmp     short fn_have12
fn_even:
        and     edx,0FFFh
fn_have12:
        mov     eax,edx
        jmp     short fn_out
fn_ioerr:
        mov     eax,[fl_eoc]            ; make the chain appear to end
fn_out:
        pop     es
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; fl_scan - read directory sector EAX into FL_DIRBUF and look for the
;           11-byte name at [fl_nameptr].  CF clear + BX = the entry's
;           offset within the buffer on a hit.
;=====================================================================
fl_scan:
        push    cx
        push    si
        push    di
        push    es
        xor     cx,cx
        mov     es,cx

        mov     bx,FL_DIRBUF
        mov     cx,1
        call    fl_read
        jc      fs_miss

        mov     bx,FL_DIRBUF
        mov     cx,512 / 32
fs_ent:
        push    cx
        mov     si,[fl_nameptr]
        mov     di,bx
        mov     cx,11
        repe    cmpsb                   ; DS:SI (ours) vs ES:DI (buffer)
        pop     cx
        je      fs_hit
        add     bx,32
        loop    fs_ent
fs_miss:
        stc
fs_out:
        pop     es                      ; POP leaves the flags alone
        pop     di
        pop     si
        pop     cx
        ret
fs_hit:
        clc
        jmp     short fs_out

;=====================================================================
; fl_find - DS:SI -> an 11-byte padded name.  Searches the root
;           directory.  CF clear: [fl_clus] and [fl_size] describe the
;           file.  CF set: no such entry.
;=====================================================================
fl_find:
        push    eax
        push    ecx
        push    ebx
        push    es
        mov     [fl_nameptr],si
        xor     ax,ax
        mov     es,ax

        cmp     byte ptr [fl_type],32
        je      ff_chain

;--- FAT12/16: a fixed run of sectors --------------------------------
        mov     eax,[fl_rootlba]
        mov     ecx,[fl_rootsecs]
ff_fixed:
        push    ecx
        push    eax
        call    fl_scan
        pop     eax
        pop     ecx
        jnc     ff_hit
        inc     eax
        loop    ff_fixed
        jmp     short ff_none

;--- FAT32: the root is an ordinary cluster chain --------------------
ff_chain:
        mov     eax,[fl_rootclus]
ff_clus:
        cmp     eax,2
        jb      ff_none
        cmp     eax,[fl_eoc]
        jae     ff_none
        push    eax
        call    fl_c2l
        mov     ecx,[fl_spc]
ff_csec:
        push    ecx
        push    eax
        call    fl_scan
        pop     eax
        pop     ecx
        jnc     ff_chain_hit
        inc     eax
        loop    ff_csec
        pop     eax
        call    fl_next
        jmp     short ff_clus
ff_chain_hit:
        pop     ecx                     ; discard the saved cluster

;--- BX -> the entry, inside FL_DIRBUF, ES = 0 -----------------------
ff_hit:
        movzx   eax,word ptr es:[bx+FLD_CLUSLO]
        cmp     byte ptr [fl_type],32
        jne     ff_clus_ok
        push    ecx
        movzx   ecx,word ptr es:[bx+FLD_CLUSHI]
        shl     ecx,16
        or      eax,ecx
        pop     ecx
ff_clus_ok:
        mov     [fl_clus],eax
        mov     eax,dword ptr es:[bx+FLD_SIZE]
        mov     [fl_size],eax
        pop     es
        pop     ebx
        pop     ecx
        pop     eax
        clc
        ret
ff_none:
        pop     es
        pop     ebx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; Loader state
;=====================================================================
fl_drive    db  0
fl_type     db  0                       ; 12, 16 or 32
            db  0,0
fl_nameptr  dw  0
fl_bxsave   dw  0                       ; carries BX across POPAD
fl_spc      dd  0
fl_spt      dd  0
fl_heads    dd  0
fl_rsvd     dd  0
fl_rootent  dd  0
fl_spf      dd  0
fl_fatsecs  dd  0
fl_totsec   dd  0
fl_hidden   dd  0
fl_fatstart dd  0
fl_rootlba  dd  0
fl_rootsecs dd  0
fl_rootclus dd  0
fl_datastart dd 0
fl_eoc      dd  0
fl_fatwin   dd  -1                      ; FAT sector pair in the window
fl_clus     dd  0                       ; result of fl_find
fl_size     dd  0
