;=====================================================================
; RMFILE2.INC - handles, and the file calls a program actually makes
;
; A HANDLE IS AN INDEX INTO A TABLE IN THE PSP, which is an index into
; a table here.  That double indirection is not decoration: it is what
; makes redirection survive an EXEC, because a child inherits its
; parent's first five JFT entries and they point at the same open
; files rather than at copies of them.
;=====================================================================

;=====================================================================
; rd_sftinit - the handle table, and the five handles every process
;              starts with.
;=====================================================================
rd_sftinit:
        pusha
        push    es
        mov     ax,cs
        mov     es,ax
        mov     di,OFFSET rd_sfts
        mov     cx,SFT_COUNT * SFT_LEN
        xor     al,al
        cld
        rep     stosb

        ; The three devices, opened once and shared.  CON is two
        ; handles because standard input and standard output are two
        ; handles, and closing one must not close the other.
        mov     si,OFFSET rd_sfts
        call    sfti_con                ; 0 - stdin
        add     si,SFT_LEN
        call    sfti_con                ; 1 - stdout
        add     si,SFT_LEN
        call    sfti_con                ; 2 - stderr
        add     si,SFT_LEN
        mov     al,DEV_AUX
        call    sfti_dev                ; 3
        add     si,SFT_LEN
        mov     al,DEV_PRN
        call    sfti_dev                ; 4

        ; And the process's own table, which points at them
        mov     es,[h_psp]
        mov     di,18h
        mov     cx,JFT_COUNT
        mov     al,0FFh
        rep     stosb
        mov     di,18h
        xor     al,al
sfti_jft:
        mov     es:[di],al
        inc     di
        inc     al
        cmp     al,5
        jb      sfti_jft
        mov     word ptr es:[32h],JFT_COUNT
        mov     word ptr es:[34h],18h
        mov     ax,[h_psp]
        mov     word ptr es:[36h],ax

        pop     es
        popa
        ret

sfti_con:
        mov     al,DEV_CON
sfti_dev:
        mov     word ptr [si+SFT_REF],1
        mov     byte ptr [si+SFT_FLAGS],SFF_DEVICE
        mov     [si+SFT_DRIVE],al
        mov     word ptr [si+SFT_MODE],2
        ret

;=====================================================================
; sft_get - BX = a handle -> SI = its table entry.  CF if the handle
;           is not open.
;=====================================================================
sft_get:
        push    es
        push    ax
        cmp     bx,JFT_COUNT
        jae     sg_bad
        mov     es,[rd_psp]
        mov     al,es:[bx+18h]
        cmp     al,0FFh
        je      sg_bad
        cmp     al,SFT_COUNT
        jae     sg_bad
        push    ax
        movzx   si,al
        mov     ax,SFT_LEN
        mul     si
        mov     si,ax
        pop     ax
        add     si,OFFSET rd_sfts
        cmp     word ptr [si+SFT_REF],0
        je      sg_bad
        pop     ax
        pop     es
        clc
        ret
sg_bad:
        pop     ax
        pop     es
        stc
        ret

;--- sft_alloc - a free table entry -> SI, AL = its index.  CF if full
sft_alloc:
        push    cx
        mov     si,OFFSET rd_sfts
        xor     al,al
        mov     cx,SFT_COUNT
sa_loop:
        cmp     word ptr [si+SFT_REF],0
        je      sa_got
        add     si,SFT_LEN
        inc     al
        loop    sa_loop
        pop     cx
        stc
        ret
sa_got:
        push    di
        push    ax
        mov     di,si
        push    es
        push    cs
        pop     es
        mov     cx,SFT_LEN
        push    di
        xor     al,al
        cld
        rep     stosb
        pop     di
        pop     es
        pop     ax
        mov     si,di
        pop     di
        mov     word ptr [si+SFT_REF],1
        pop     cx
        clc
        ret

;--- sft_addref / sft_release - AL = a table index -------------------
sft_addref:
        push    si
        push    ax
        cmp     al,SFT_COUNT
        jae     sar_done
        movzx   si,al
        mov     ax,SFT_LEN
        mul     si
        mov     si,ax
        add     si,OFFSET rd_sfts
        inc     word ptr [si+SFT_REF]
sar_done:
        pop     ax
        pop     si
        ret

; BX = a handle.  Drops one reference, closing the file if it was the
; last - which is the only place a directory entry is written back.
sft_release:
        push    si
        push    ax
        call    sft_get
        jc      srl_done
        cmp     word ptr [si+SFT_REF],0
        je      srl_done
        dec     word ptr [si+SFT_REF]
        cmp     word ptr [si+SFT_REF],0
        jne     srl_done
        call    file_commit
srl_done:
        pop     ax
        pop     si
        ret

;--- jft_alloc - a free slot in the process's table -> BX ------------
jft_alloc:
        push    es
        push    ax
        mov     es,[rd_psp]
        xor     bx,bx
ja_loop:
        cmp     bx,JFT_COUNT
        jae     ja_full
        cmp     byte ptr es:[bx+18h],0FFh
        je      ja_got
        inc     bx
        jmp     short ja_loop
ja_got:
        pop     ax
        pop     es
        clc
        ret
ja_full:
        pop     ax
        pop     es
        stc
        ret

;=====================================================================
; file_commit - SI = an SFT that is being closed.  Write its directory
;               entry back if anything about it changed.
;=====================================================================
file_commit:
        pusha
        test    byte ptr [si+SFT_FLAGS],SFF_DEVICE
        jnz     fcm_done
        test    byte ptr [si+SFT_FLAGS],SFF_WRITTEN
        jz      fcm_done

        mov     al,[si+SFT_DRIVE]
        push    si
        call    rd_getvol
        mov     di,si
        pop     si
        jc      fcm_done                ; DI = volume, SI = the SFT

        mov     eax,dword ptr [si+SFT_DIRLBA]
        push    si
        mov     si,di
        call    dir_win
        pop     si
        jc      fcm_done

        mov     bx,[si+SFT_DIROFF]
        mov     eax,dword ptr [si+SFT_SIZE]
        mov     dword ptr [rd_dirbuf + bx + DE_SIZE],eax
        mov     eax,dword ptr [si+SFT_START]
        mov     word ptr [rd_dirbuf + bx + DE_CLUSLO],ax
        shr     eax,16
        mov     word ptr [rd_dirbuf + bx + DE_CLUSHI],ax
        mov     ax,[si+SFT_TIME]
        mov     word ptr [rd_dirbuf + bx + DE_TIME],ax
        mov     ax,[si+SFT_DATE]
        mov     word ptr [rd_dirbuf + bx + DE_DATE],ax
        or      byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_ARCHIVE
        call    dir_commit
        call    rd_flush
fcm_done:
        popa
        ret

;=====================================================================
; sft_clus - SI = an SFT, EAX = a byte position -> EBX = the cluster
;            it is in.  CF if the chain ends before it.
;
; The walk starts from where the file already is when the position
; asked for is ahead of it, which is what makes reading a file
; straight through cost one step per cluster instead of one walk per
; read.
;=====================================================================
sft_clus:
        push    ecx
        push    edx
        push    edi
        push    si

        mov     al,[si+SFT_DRIVE]
        mov     di,si
        call    rd_getvol               ; -> SI = volume
        jc      sc_bad
        movzx   ecx,byte ptr [si+VOL_SPC]
        shl     ecx,9                   ; bytes per cluster
        mov     edx,eax
        xor     eax,eax
        xchg    eax,edx
        xor     edx,edx
        div     ecx                     ; EAX = which cluster of the file

        mov     ebx,dword ptr [di+SFT_CURCLUS]
        mov     edx,dword ptr [di+SFT_CURIDX]
        cmp     ebx,2
        jb      sc_fromstart
        cmp     eax,edx
        jae     sc_walk
sc_fromstart:
        mov     ebx,dword ptr [di+SFT_START]
        xor     edx,edx
        cmp     ebx,2
        jb      sc_bad
sc_walk:
        cmp     edx,eax
        je      sc_got
        push    eax
        mov     eax,ebx
        call    fat_get
        call    fat_eoc
        jc      sc_endpop
        cmp     eax,2
        jb      sc_endpop
        mov     ebx,eax
        pop     eax
        inc     edx
        jmp     short sc_walk
sc_endpop:
        pop     eax
        jmp     short sc_bad
sc_got:
        mov     dword ptr [di+SFT_CURCLUS],ebx
        mov     dword ptr [di+SFT_CURIDX],edx
        pop     si
        pop     edi
        pop     edx
        pop     ecx
        clc
        ret
sc_bad:
        pop     si
        pop     edi
        pop     edx
        pop     ecx
        stc
        ret

;--- sft_grow - SI = an SFT.  Add one cluster to the end of its chain
;--- and answer it in EBX.  CF if the volume is full.
sft_grow:
        push    eax
        push    ecx
        push    edx
        push    di
        mov     di,si
        mov     al,[si+SFT_DRIVE]
        call    rd_getvol
        jc      sg2_bad
        call    fat_alloc
        or      eax,eax
        jz      sg2_bad
        mov     ebx,eax

        mov     eax,dword ptr [di+SFT_START]
        cmp     eax,2
        jae     sg2_chain
        mov     dword ptr [di+SFT_START],ebx    ; the file had none at all
        or      byte ptr [di+SFT_FLAGS],SFF_WRITTEN
        jmp     short sg2_ok
sg2_chain:
        ; Walk to the end and hook it on
        mov     ecx,eax
sg2_walk:
        mov     eax,ecx
        call    fat_get
        call    fat_eoc
        jc      sg2_tail
        cmp     eax,2
        jb      sg2_tail
        mov     ecx,eax
        jmp     short sg2_walk
sg2_tail:
        mov     eax,ecx
        mov     edx,ebx
        call    fat_put
sg2_ok:
        mov     si,di
        pop     di
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret
sg2_bad:
        mov     si,di
        pop     di
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; file_io - SI = SFT, ES:DI = buffer, ECX = bytes, [rd_iowr] = 0 read
;           / 1 write.  Answers ECX = bytes actually moved.
;
; ONE SECTOR AT A TIME THROUGH A BUFFER, always, even for a whole
; sector.  It is slower than reading straight into the caller's memory
; and it is a third of the code, with no case where the caller's
; buffer straddles a segment and no case where a partial sector needs
; a different path from a whole one.  On a machine doing one BIOS call
; per sector anyway, the copy is not what costs.
;=====================================================================
file_io:
        push    eax
        push    ebx
        push    edx
        push    bp
        mov     dword ptr [rd_iodone],0
fio_loop:
        or      ecx,ecx
        jz      fio_done

        cmp     byte ptr [rd_iowr],0
        jne     fio_wr
        ; Reading stops at the end of the file, and that is not an
        ; error - it is how a program knows where the end was.
        mov     eax,dword ptr [si+SFT_SIZE]
        sub     eax,dword ptr [si+SFT_POS]
        jbe     fio_done
        cmp     ecx,eax
        jbe     fio_wr
        mov     ecx,eax
fio_wr:
        mov     eax,dword ptr [si+SFT_POS]
        call    sft_clus                ; -> EBX
        jnc     fio_have
        cmp     byte ptr [rd_iowr],0
        je      fio_done                ; reading past the chain: stop
        call    sft_grow                ; writing past it: extend
        jc      fio_full
fio_have:
        ; Which sector of that cluster, and how far into it?
        push    si
        mov     al,[si+SFT_DRIVE]
        mov     bp,si
        call    rd_getvol
        jc      fio_poperr
        mov     eax,dword ptr [bp+SFT_POS]
        movzx   edx,byte ptr [si+VOL_SPC]
        shl     edx,9
        dec     edx
        and     eax,edx                 ; the offset within the cluster
        shr     eax,9                   ; which sector of it
        push    eax
        mov     eax,ebx
        call    clus2lba
        pop     edx
        add     eax,edx
        mov     dword ptr [rd_iolba],eax

        mov     eax,dword ptr [bp+SFT_POS]
        and     eax,1FFh                ; where in the sector
        mov     dword ptr [rd_iooff],eax
        mov     edx,SECSIZE
        sub     edx,eax
        cmp     ecx,edx
        jbe     fio_chunk
        mov     ecx,edx
fio_chunk:
        mov     dword ptr [rd_iolen],ecx

        ; Read the sector unless the write covers all of it
        cmp     byte ptr [rd_iowr],0
        je      fio_needsec
        cmp     dword ptr [rd_iolen],SECSIZE
        je      fio_nosec
fio_needsec:
        push    es
        push    bx
        mov     eax,dword ptr [rd_iolba]
        mov     bx,cs
        mov     es,bx
        mov     bx,OFFSET rd_datbuf
        push    cx
        mov     cx,1
        call    rd_rdsec
        pop     cx
        pop     bx
        pop     es
        jc      fio_poperr
fio_nosec:
        pop     si

        ; Move the bytes
        push    ds
        push    es
        push    si
        push    di
        push    cx
        mov     ecx,dword ptr [rd_iolen]
        mov     bx,word ptr [rd_iooff]
        cmp     byte ptr [rd_iowr],0
        jne     fio_tobuf
        push    cs
        pop     ds
        mov     si,OFFSET rd_datbuf
        add     si,bx
        cld
        rep     movsb                   ; DS:SI -> ES:DI, the caller's
        jmp     short fio_moved
fio_tobuf:
        push    es
        pop     ds
        mov     si,di                   ; the caller's buffer is the source
        push    cs
        pop     es
        mov     di,OFFSET rd_datbuf
        add     di,bx
        cld
        rep     movsb
fio_moved:
        pop     cx
        pop     di
        pop     si
        pop     es
        pop     ds

        ; Write it back if we changed it
        cmp     byte ptr [rd_iowr],0
        je      fio_written
        push    es
        push    bx
        push    si
        mov     al,[si+SFT_DRIVE]
        call    rd_getvol
        jc      fio_poperr2
        mov     eax,dword ptr [rd_iolba]
        mov     bx,cs
        mov     es,bx
        mov     bx,OFFSET rd_datbuf
        push    cx
        mov     cx,1
        call    rd_wrsec
        pop     cx
        pop     si
        pop     bx
        pop     es
        jc      fio_err
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN
fio_written:
        mov     eax,dword ptr [rd_iolen]
        add     dword ptr [si+SFT_POS],eax
        add     dword ptr [rd_iodone],eax
        add     di,ax
        sub     ecx,eax

        ; A write past the old end makes the file longer
        cmp     byte ptr [rd_iowr],0
        je      fio_loop
        mov     eax,dword ptr [si+SFT_POS]
        cmp     eax,dword ptr [si+SFT_SIZE]
        jbe     fio_loop
        mov     dword ptr [si+SFT_SIZE],eax
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN
        jmp     fio_loop

fio_done:
        mov     ecx,dword ptr [rd_iodone]
        pop     bp
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret
fio_poperr2:
        pop     si
fio_poperr:
        pop     si
fio_err:
        mov     ecx,dword ptr [rd_iodone]
        pop     bp
        pop     edx
        pop     ebx
        pop     eax
        stc
        ret
fio_full:
        mov     ecx,dword ptr [rd_iodone]
        pop     bp
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret                             ; a short write, which is how DOS
                                        ; reports a full disk

;=====================================================================
; The calls
;=====================================================================

;--- 3Ch create / truncate; 5Bh create only if it is not there -------
fn3C:
        mov     byte ptr [rd_crnew],0
        jmp     short f3C_common
fn5B:
        mov     byte ptr [rd_crnew],1
f3C_common:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        mov     cx,[bp+F_CX]
        mov     [rd_crattr],cl
        call    sub_create
        jc      f3C_bad
        mov     [bp+F_AX],bx
        jmp     i21_ok
f3C_bad:
        jmp     i21_fail

;--- 5Ah create a uniquely named temporary file ----------------------
fn5A:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;--- 3Dh open --------------------------------------------------------
fn3D:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        mov     ax,[bp+F_AX]
        call    sub_open
        jc      f3D_bad
        mov     [bp+F_AX],bx
        jmp     i21_ok
f3D_bad:
        jmp     i21_fail

;--- 3Eh close -------------------------------------------------------
fn3E:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f3E_bad
        call    sft_release
        push    es
        mov     es,[rd_psp]
        mov     bx,[bp+F_BX]
        mov     byte ptr es:[bx+18h],0FFh
        pop     es
        jmp     i21_ok
f3E_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 3Fh read --------------------------------------------------------
fn3F:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f3F_bad
        movzx   ecx,word ptr [bp+F_CX]
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        test    byte ptr [si+SFT_FLAGS],SFF_DEVICE
        jnz     f3F_dev
        mov     byte ptr [rd_iowr],0
        call    file_io
        jc      f3F_ioerr
        mov     [bp+F_AX],cx
        jmp     i21_ok
f3F_dev:
        call    dev_read
        mov     [bp+F_AX],cx
        jmp     i21_ok
f3F_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail
f3F_ioerr:
        mov     ax,ERR_BADBLOCK
        jmp     i21_fail

;--- 40h write.  A count of zero TRUNCATES, which is the one thing
;--- this call does that its name does not suggest.
fn40:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f40_bad
        movzx   ecx,word ptr [bp+F_CX]
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        test    byte ptr [si+SFT_FLAGS],SFF_DEVICE
        jnz     f40_dev
        or      ecx,ecx
        jz      f40_trunc
        mov     byte ptr [rd_iowr],1
        call    file_io
        jc      f40_ioerr
        mov     [bp+F_AX],cx
        jmp     i21_ok
f40_trunc:
        call    file_trunc
        mov     word ptr [bp+F_AX],0
        jmp     i21_ok
f40_dev:
        call    dev_write
        mov     [bp+F_AX],cx
        jmp     i21_ok
f40_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail
f40_ioerr:
        mov     ax,ERR_BADBLOCK
        jmp     i21_fail

;--- file_trunc - SI = SFT.  Cut the file at the current position ----
file_trunc:
        pusha
        mov     eax,dword ptr [si+SFT_POS]
        mov     dword ptr [si+SFT_SIZE],eax
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN
        or      eax,eax
        jnz     ft_keep

        ; Truncated to nothing: the whole chain goes back
        mov     ebx,dword ptr [si+SFT_START]
        cmp     ebx,2
        jb      ft_done
        push    si
        mov     al,[si+SFT_DRIVE]
        call    rd_getvol
        jc      ft_pop
        mov     eax,ebx
        call    fat_freechain
ft_pop:
        pop     si
        mov     dword ptr [si+SFT_START],0
        mov     dword ptr [si+SFT_CURCLUS],0
        mov     dword ptr [si+SFT_CURIDX],0
ft_keep:
ft_done:
        popa
        ret

;--- 42h seek --------------------------------------------------------
fn42:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f42_bad
        movzx   eax,word ptr [bp+F_CX]
        shl     eax,16
        mov     ax,[bp+F_DX]
        mov     cl,byte ptr [bp+F_AX]
        cmp     cl,0
        je      f42_set
        cmp     cl,1
        je      f42_cur
        cmp     cl,2
        je      f42_end
        mov     ax,ERR_FUNC
        jmp     i21_fail
f42_cur:
        add     eax,dword ptr [si+SFT_POS]
        jmp     short f42_set
f42_end:
        add     eax,dword ptr [si+SFT_SIZE]
f42_set:
        mov     dword ptr [si+SFT_POS],eax
        mov     [bp+F_AX],ax
        shr     eax,16
        mov     [bp+F_DX],ax
        jmp     i21_ok
f42_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 45h duplicate a handle ------------------------------------------
fn45:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f45_bad
        push    bx
        call    jft_alloc               ; -> BX
        jc      f45_full
        mov     cx,bx
        pop     bx
        push    es
        mov     es,[rd_psp]
        mov     al,es:[bx+18h]
        mov     bx,cx
        mov     es:[bx+18h],al
        pop     es
        call    sft_addref
        mov     [bp+F_AX],bx
        jmp     i21_ok
f45_full:
        pop     bx
        mov     ax,ERR_NOHANDLES
        jmp     i21_fail
f45_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 46h force one handle to be another ------------------------------
fn46:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f46_bad
        mov     cx,[bp+F_CX]
        cmp     cx,JFT_COUNT
        jae     f46_bad

        ; If the target is open, close it first - which is exactly what
        ; makes "> file" work: handle 1 is forced onto the file, and
        ; the console it used to be goes away.
        push    bx
        mov     bx,cx
        call    sft_get
        jc      f46_free
        call    sft_release
f46_free:
        pop     bx
        push    es
        mov     es,[rd_psp]
        mov     al,es:[bx+18h]
        mov     bx,cx
        mov     es:[bx+18h],al
        pop     es
        call    sft_addref
        jmp     i21_ok
f46_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 41h delete ------------------------------------------------------
fn41:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    rd_path
        jc      f41_bad
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        call    dir_find
        jc      f41_nofile
        mov     bx,[rd_deoff]
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_DIR
        jnz     f41_nofile
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_READONLY
        jnz     f41_ro

        call    rd_declus               ; -> EBX
        mov     byte ptr [rd_dirbuf + DE_NAME],0E5h
        push    bx
        mov     bx,[rd_deoff]
        mov     byte ptr [rd_dirbuf + bx + DE_NAME],0E5h
        pop     bx
        call    dir_commit
        cmp     ebx,2
        jb      f41_nochain
        mov     eax,ebx
        call    fat_freechain
f41_nochain:
        call    rd_flush
        jmp     i21_ok
f41_ro:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f41_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f41_bad:
        jmp     i21_fail

;--- 43h get or set attributes ---------------------------------------
fn43:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    rd_path
        jc      f43_bad
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        call    dir_find
        jc      f43_nofile
        mov     bx,[rd_deoff]
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jnz     f43_set
        movzx   cx,byte ptr [rd_dirbuf + bx + DE_ATTR]
        mov     [bp+F_CX],cx
        jmp     i21_ok
f43_set:
        cmp     al,1
        jne     f43_fn
        mov     cl,byte ptr [bp+F_CX]
        and     cl,NOT (ATTR_DIR OR ATTR_VOLUME)
        mov     byte ptr [rd_dirbuf + bx + DE_ATTR],cl
        call    dir_commit
        call    rd_flush
        jmp     i21_ok
f43_fn:
        mov     ax,ERR_FUNC
        jmp     i21_fail
f43_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f43_bad:
        jmp     i21_fail

;--- 57h a file's time and date --------------------------------------
fn57:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f57_bad
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jnz     f57_set
        mov     ax,[si+SFT_TIME]
        mov     [bp+F_CX],ax
        mov     ax,[si+SFT_DATE]
        mov     [bp+F_DX],ax
        jmp     i21_ok
f57_set:
        cmp     al,1
        jne     f57_fn
        mov     ax,[bp+F_CX]
        mov     [si+SFT_TIME],ax
        mov     ax,[bp+F_DX]
        mov     [si+SFT_DATE],ax
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN
        jmp     i21_ok
f57_fn:
        mov     ax,ERR_FUNC
        jmp     i21_fail
f57_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 44h IOCTL, and only the parts that say what a handle IS ---------
; Windows asks 4400h about every handle it opens to find out whether
; it is talking to a file or to the console, and behaves differently
; for the answer.  The rest of the family is device control that has
; no device here to control.
fn44:
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jz      f44_get
        cmp     al,1
        je      f44_set
        cmp     al,6
        je      f44_instat
        cmp     al,7
        je      f44_outstat
        cmp     al,8
        je      f44_changeable
        mov     ax,ERR_FUNC
        jmp     i21_fail
f44_get:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f44_bad
        test    byte ptr [si+SFT_FLAGS],SFF_DEVICE
        jz      f44_isfile
        mov     dx,80D3h                ; a character device: console, and
        mov     al,[si+SFT_DRIVE]       ; not the end of a file
        cmp     al,DEV_CON
        je      f44_havedx
        mov     dx,8080h
        jmp     short f44_havedx
f44_isfile:
        movzx   dx,byte ptr [si+SFT_DRIVE]
f44_havedx:
        mov     [bp+F_DX],dx
        mov     [bp+F_AX],dx
        jmp     i21_ok
f44_set:
        jmp     i21_ok
f44_instat:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f44_bad
        mov     byte ptr [bp+F_AX],0FFh ; ready
        jmp     i21_ok
f44_outstat:
        mov     bx,[bp+F_BX]
        call    sft_get
        jc      f44_bad
        mov     byte ptr [bp+F_AX],0FFh
        jmp     i21_ok
f44_changeable:
        mov     word ptr [bp+F_AX],1    ; fixed, which every volume here is
        jmp     i21_ok
f44_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 39h, 3Ah, 3Bh - directories -------------------------------------
fn39:
fn3A:
        mov     ax,ERR_FUNC
        jmp     i21_fail

fn3B:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    rd_path
        jc      f3B_bad
        ; rd_path stopped at the last component, so look it up and
        ; stand there.  An empty last component means the caller named
        ; a directory with a trailing separator and is already there.
        cmp     byte ptr [rd_fcb],' '
        je      f3B_here
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        push    ax
        call    dir_find
        pop     ax
        jc      f3B_nopath
        push    bx
        mov     bx,[rd_deoff]
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_DIR
        pop     bx
        jz      f3B_nopath
        call    rd_declus               ; -> EBX
f3B_here:
        push    si
        mov     al,byte ptr [rp_drv]
        movzx   si,al
        mov     ax,CDS_LEN
        mul     si
        mov     si,ax
        add     si,OFFSET rd_cds
        mov     dword ptr [si+CDS_CLUS],ebx
        pop     si
        jmp     i21_ok
f3B_nopath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail
f3B_bad:
        jmp     i21_fail

;--- 47h where am I?  DL = drive, DS:SI -> 64 bytes ------------------
; The text path is kept alongside the cluster because there is no way
; to reconstruct it: a directory knows its parent through "..", but
; not its own name, so walking up gives the shape of the path and none
; of the words in it.
fn47:
        mov     al,byte ptr [bp+F_DX]
        or      al,al
        jnz     f47_named
        mov     al,[rd_drive]
        inc     al
f47_named:
        dec     al
        cmp     al,LASTDRIVE
        jae     f47_bad
        push    es
        movzx   si,al
        mov     ax,CDS_LEN
        mul     si
        mov     si,ax
        add     si,OFFSET rd_cds
        add     si,CDS_PATH
        mov     es,[bp+F_DS]
        mov     di,[bp+F_SI]
        mov     cx,64
        push    ds
        push    cs
        pop     ds
        cld
        rep     movsb
        pop     ds
        pop     es
        mov     word ptr [bp+F_AX],0100h
        jmp     i21_ok
f47_bad:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail

;--- 4Eh find first, 4Fh find next -----------------------------------
fn4E:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    rd_path
        jc      f4E_bad
        mov     [rd_ffvol],si
        mov     dword ptr [rd_ffdir],ebx
        mov     al,byte ptr [bp+F_CX]
        mov     [rd_ffattr],al

        push    cs
        pop     es
        mov     si,OFFSET rd_fcb
        mov     di,OFFSET rd_ffname
        mov     cx,11
        push    ds
        push    cs
        pop     ds
        cld
        rep     movsb
        pop     ds

        mov     si,[rd_ffvol]
        mov     eax,dword ptr [rd_ffdir]
        call    dir_first
        jc      f4E_none
        mov     eax,dword ptr [rd_dfsec]
        mov     dword ptr [rd_ffsec],eax
        mov     eax,dword ptr [rd_dfclus]
        mov     dword ptr [rd_ffclus],eax
        mov     eax,dword ptr [rd_dfleft]
        mov     dword ptr [rd_ffleft],eax
        mov     al,[rd_dfroot]
        mov     [rd_ffroot],al
        mov     word ptr [rd_ffent],0
        jmp     short f4F_scan
f4E_none:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f4E_bad:
        jmp     i21_fail

fn4F:
        mov     si,[rd_ffvol]
        or      si,si
        jz      f4F_none
f4F_scan:
        mov     si,[rd_ffvol]
        mov     eax,dword ptr [rd_ffsec]
        mov     dword ptr [rd_dfsec],eax
        mov     eax,dword ptr [rd_ffclus]
        mov     dword ptr [rd_dfclus],eax
        mov     eax,dword ptr [rd_ffleft]
        mov     dword ptr [rd_dfleft],eax
        mov     al,[rd_ffroot]
        mov     [rd_dfroot],al
f4F_sector:
        mov     eax,dword ptr [rd_dfsec]
        call    dir_win
        jc      f4F_none
        mov     bx,[rd_ffent]
f4F_entry:
        cmp     bx,SECSIZE
        jae     f4F_nextsec
        mov     al,byte ptr [rd_dirbuf + bx]
        or      al,al
        jz      f4F_none
        cmp     al,0E5h
        je      f4F_next
        mov     al,byte ptr [rd_dirbuf + bx + DE_ATTR]
        and     al,0Fh
        cmp     al,0Fh
        je      f4F_next
        push    bx
        call    ff_match
        pop     bx
        jc      f4F_next
        jmp     short f4F_found
f4F_next:
        add     bx,DE_LEN
        jmp     short f4F_entry
f4F_nextsec:
        mov     word ptr [rd_ffent],0
        call    dir_next
        jc      f4F_none
        mov     eax,dword ptr [rd_dfsec]
        mov     dword ptr [rd_ffsec],eax
        mov     eax,dword ptr [rd_dfclus]
        mov     dword ptr [rd_ffclus],eax
        mov     eax,dword ptr [rd_dfleft]
        mov     dword ptr [rd_ffleft],eax
        jmp     short f4F_sector
f4F_found:
        add     bx,DE_LEN
        mov     [rd_ffent],bx
        sub     bx,DE_LEN
        call    ff_fill
        jmp     i21_ok
f4F_none:
        mov     word ptr [rd_ffvol],0
        mov     ax,ERR_NOFILE
        jmp     i21_fail

;--- ff_match - BX = an entry.  CF set if it does not match the search
ff_match:
        push    si
        push    di
        push    cx
        ; the attribute rule: a plain search never returns a directory,
        ; a hidden file or a system file, but a search that asks for
        ; any of those gets ordinary files as well
        mov     al,byte ptr [rd_dirbuf + bx + DE_ATTR]
        mov     ah,[rd_ffattr]
        not     ah
        and     al,ATTR_DIR OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_VOLUME
        and     al,ah
        jnz     ffm_no

        lea     si,[rd_dirbuf + bx]
        mov     di,OFFSET rd_ffname
        mov     cx,11
ffm_cmp:
        mov     al,[di]
        cmp     al,'?'
        je      ffm_next
        cmp     al,[si]
        jne     ffm_no
ffm_next:
        inc     si
        inc     di
        loop    ffm_cmp
        pop     cx
        pop     di
        pop     si
        clc
        ret
ffm_no:
        pop     cx
        pop     di
        pop     si
        stc
        ret

;--- ff_fill - BX = the entry.  Write the caller's DTA ---------------
ff_fill:
        pusha
        push    es
        mov     es,[rd_dtaseg]
        mov     di,[rd_dtaoff]

        ; The first 21 bytes are DOS's own place to keep the search
        ; going, and a program that copies them about is entitled to
        ; expect 4Fh to carry on from wherever it put them back.
        push    di
        mov     al,[rd_drive]
        mov     es:[di],al
        inc     di
        mov     si,OFFSET rd_ffname
        mov     cx,11
        push    ds
        push    cs
        pop     ds
        cld
        rep     movsb
        pop     ds
        mov     ax,[rd_ffent]
        mov     es:[di],ax
        add     di,2
        mov     eax,dword ptr [rd_ffsec]
        mov     es:[di],eax
        add     di,4
        mov     eax,dword ptr [rd_ffclus]
        mov     es:[di],eax
        pop     di

        add     di,15h
        mov     al,byte ptr [rd_dirbuf + bx + DE_ATTR]
        mov     es:[di],al
        mov     ax,word ptr [rd_dirbuf + bx + DE_TIME]
        mov     es:[di+1],ax
        mov     ax,word ptr [rd_dirbuf + bx + DE_DATE]
        mov     es:[di+3],ax
        mov     eax,dword ptr [rd_dirbuf + bx + DE_SIZE]
        mov     es:[di+5],eax

        ; and the name, as text with a dot in it again
        add     di,9
        lea     si,[rd_dirbuf + bx]
        mov     cx,8
ffl_base:
        mov     al,[si]
        cmp     al,' '
        je      ffl_baseend
        mov     es:[di],al
        inc     di
        inc     si
        loop    ffl_base
ffl_baseend:
        lea     si,[rd_dirbuf + bx]
        add     si,8
        cmp     byte ptr [si],' '
        je      ffl_noext
        mov     byte ptr es:[di],'.'
        inc     di
        mov     cx,3
ffl_ext:
        mov     al,[si]
        cmp     al,' '
        je      ffl_noext
        mov     es:[di],al
        inc     di
        inc     si
        loop    ffl_ext
ffl_noext:
        mov     byte ptr es:[di],0
        pop     es
        popa
        ret

;--- 56h rename ------------------------------------------------------
fn56:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;--- 5Ch lock a region.  There is no sharing here, so every lock
;--- succeeds and every unlock succeeds, which is what a DOS without
;--- SHARE loaded has always done.
fn5C:
        jmp     i21_ok

;--- 67h how many handles.  The table is in the PSP and cannot grow
;--- past it without somewhere else to put it, so this answers what it
;--- can and refuses what it cannot.
fn67:
        mov     ax,[bp+F_BX]
        cmp     ax,JFT_COUNT
        jbe     f67_ok
        mov     ax,ERR_NOMEM
        jmp     i21_fail
f67_ok:
        jmp     i21_ok

;--- 68h / 6Ah commit a file: everything of ours goes out ------------
fn68:
        call    rd_flush
        jmp     i21_ok

;--- 6Ch the extended open, which says WHICH of create and open
;--- happened - the one thing 3Ch and 3Dh cannot say between them.
fn6C:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;=====================================================================
; The subroutine forms, for EXEC and anything else inside RMDOS
;=====================================================================

;--- sub_open - ES:SI -> name, AX = mode -> BX = handle.  CF, AX = why
sub_open:
        push    cx
        push    dx
        push    di
        push    si
        push    es
        mov     [rd_opmode],ax
        call    rd_path
        jc      so_bad
        mov     [rd_opvol],si
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        call    dir_find
        jc      so_nofile
        mov     bx,[rd_deoff]
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_DIR
        jnz     so_nofile
        call    sub_fromdir
        jc      so_nohandles
        pop     es
        pop     si
        pop     di
        pop     dx
        pop     cx
        clc
        ret
so_nofile:
        mov     ax,ERR_NOFILE
        jmp     short so_out
so_nohandles:
        mov     ax,ERR_NOHANDLES
        jmp     short so_out
so_bad:
        mov     ax,ERR_NOPATH
so_out:
        pop     es
        pop     si
        pop     di
        pop     dx
        pop     cx
        stc
        ret

;--- sub_fromdir - the entry found in rd_dirbuf at rd_deoff becomes an
;--- open file.  BX = the handle.
sub_fromdir:
        push    si
        push    di
        push    ax
        call    jft_alloc               ; -> BX
        jc      sfd_bad
        mov     [rd_ophnd],bx
        call    sft_alloc               ; -> SI, AL = index
        jc      sfd_bad
        mov     [rd_opidx],al
        mov     di,[rd_deoff]

        mov     al,[rp_drv]
        mov     [si+SFT_DRIVE],al
        mov     ax,[rd_opmode]
        mov     [si+SFT_MODE],ax
        mov     al,byte ptr [rd_dirbuf + di + DE_ATTR]
        mov     [si+SFT_ATTR],al
        mov     byte ptr [si+SFT_FLAGS],0
        mov     eax,dword ptr [rd_dirbuf + di + DE_SIZE]
        mov     dword ptr [si+SFT_SIZE],eax
        mov     dword ptr [si+SFT_POS],0
        movzx   eax,word ptr [rd_dirbuf + di + DE_CLUSHI]
        shl     eax,16
        mov     ax,word ptr [rd_dirbuf + di + DE_CLUSLO]
        mov     dword ptr [si+SFT_START],eax
        mov     dword ptr [si+SFT_CURCLUS],0
        mov     dword ptr [si+SFT_CURIDX],0
        mov     ax,word ptr [rd_dirbuf + di + DE_TIME]
        mov     [si+SFT_TIME],ax
        mov     ax,word ptr [rd_dirbuf + di + DE_DATE]
        mov     [si+SFT_DATE],ax
        mov     eax,dword ptr [rd_delba]
        mov     dword ptr [si+SFT_DIRLBA],eax
        mov     ax,[rd_deoff]
        mov     [si+SFT_DIROFF],ax

        push    es
        mov     es,[rd_psp]
        mov     bx,[rd_ophnd]
        mov     al,[rd_opidx]
        mov     es:[bx+18h],al
        pop     es
        pop     ax
        pop     di
        pop     si
        clc
        ret
sfd_bad:
        pop     ax
        pop     di
        pop     si
        stc
        ret

;--- sub_create - ES:SI -> name, [rd_crattr] = attributes ------------
sub_create:
        push    cx
        push    dx
        push    di
        push    si
        push    es
        call    rd_path
        jc      sc2_bad
        mov     [rd_opvol],si
        mov     dword ptr [rd_crdir],ebx

        ; Is it already there?  Then this is a truncate, unless the
        ; caller asked for 5Bh, in which case it is a refusal.
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        call    dir_find
        jc      sc2_new
        cmp     byte ptr [rd_crnew],0
        jne     sc2_exists
        mov     bx,[rd_deoff]
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_DIR
        jnz     sc2_exists
        mov     word ptr [rd_opmode],2
        call    sub_fromdir
        jc      sc2_nohandles
        mov     dword ptr [si+SFT_POS],0
        call    file_trunc
        jmp     short sc2_ok

sc2_new:
        mov     si,[rd_opvol]
        mov     eax,dword ptr [rd_crdir]
        call    dir_alloc
        jc      sc2_full

        ; Fill the new entry in
        mov     bx,[rd_deoff]
        push    di
        lea     di,[rd_dirbuf + bx]
        push    es
        push    cs
        pop     es
        mov     si,OFFSET rd_fcb
        mov     cx,11
        push    ds
        push    cs
        pop     ds
        cld
        rep     movsb
        pop     ds
        pop     es
        pop     di
        mov     al,[rd_crattr]
        and     al,NOT (ATTR_DIR OR ATTR_VOLUME)
        or      al,ATTR_ARCHIVE
        mov     byte ptr [rd_dirbuf + bx + DE_ATTR],al
        mov     dword ptr [rd_dirbuf + bx + DE_SIZE],0
        mov     word ptr [rd_dirbuf + bx + DE_CLUSLO],0
        mov     word ptr [rd_dirbuf + bx + DE_CLUSHI],0
        call    rd_stamp                ; -> AX = time, DX = date
        mov     word ptr [rd_dirbuf + bx + DE_TIME],ax
        mov     word ptr [rd_dirbuf + bx + DE_DATE],dx
        mov     cx,10
        push    di
        lea     di,[rd_dirbuf + bx + 0Ch]
        xor     al,al
        push    es
        push    cs
        pop     es
        cld
        rep     stosb
        pop     es
        pop     di
        call    dir_commit
        call    rd_flush

        mov     word ptr [rd_opmode],2
        mov     si,[rd_opvol]
        call    sub_fromdir
        jc      sc2_nohandles
        or      byte ptr [si+SFT_FLAGS],SFF_WRITTEN
sc2_ok:
        mov     bx,[rd_ophnd]
        pop     es
        pop     si
        pop     di
        pop     dx
        pop     cx
        clc
        ret
sc2_exists:
        mov     ax,ERR_EXISTS
        jmp     short sc2_out
sc2_full:
        mov     ax,ERR_NOHANDLES
        jmp     short sc2_out
sc2_nohandles:
        mov     ax,ERR_NOHANDLES
        jmp     short sc2_out
sc2_bad:
        mov     ax,ERR_NOPATH
sc2_out:
        pop     es
        pop     si
        pop     di
        pop     dx
        pop     cx
        stc
        ret

;--- rd_stamp - now, in directory-entry form -------------------------
rd_stamp:
        push    cx
        mov     ah,02h
        int     1Ah
        jc      rst_none
        mov     al,ch
        call    bcd2bin
        movzx   ax,al
        shl     ax,11
        mov     [rd_tmp],ax
        mov     al,cl
        call    bcd2bin
        movzx   ax,al
        shl     ax,5
        or      [rd_tmp],ax
        mov     al,dh
        call    bcd2bin
        shr     al,1
        movzx   ax,al
        or      ax,[rd_tmp]
        push    ax
        mov     ah,04h
        int     1Ah
        jc      rst_nodate
        mov     al,ch
        call    bcd2bin
        movzx   bx,al
        mov     al,cl
        call    bcd2bin
        movzx   ax,al
        push    ax
        mov     ax,bx
        mov     cx,100
        mul     cx
        pop     bx
        add     ax,bx
        sub     ax,1980
        shl     ax,9
        mov     [rd_tmp],ax
        mov     al,dh
        call    bcd2bin
        movzx   ax,al
        shl     ax,5
        or      [rd_tmp],ax
        mov     al,dl
        call    bcd2bin
        movzx   ax,al
        or      ax,[rd_tmp]
        mov     dx,ax
        pop     ax
        pop     cx
        ret
rst_nodate:
        pop     ax
        mov     dx,(1980-1980) SHL 9 OR (1 SHL 5) OR 1
        pop     cx
        ret
rst_none:
        xor     ax,ax
        mov     dx,(1 SHL 5) OR 1
        pop     cx
        ret

;--- sub_read / sub_write / sub_seek / sub_close ---------------------
; BX = handle, ECX = bytes, ES:DX = buffer.  For EXEC's use.
sub_read:
        push    si
        push    di
        call    sft_get
        jc      sr_bad
        mov     di,dx
        mov     byte ptr [rd_iowr],0
        call    file_io
        pop     di
        pop     si
        clc
        ret
sr_bad:
        pop     di
        pop     si
        stc
        ret

sub_seek:
        push    si
        call    sft_get
        jc      ss_bad
        mov     dword ptr [si+SFT_POS],eax
        pop     si
        clc
        ret
ss_bad:
        pop     si
        stc
        ret

sub_close:
        push    es
        call    sft_release
        mov     es,[rd_psp]
        cmp     bx,JFT_COUNT
        jae     sc3_done
        mov     byte ptr es:[bx+18h],0FFh
sc3_done:
        pop     es
        ret

;=====================================================================
; The devices
;=====================================================================
dev_read:
        push    di
        push    es
        mov     al,[si+SFT_DRIVE]
        cmp     al,DEV_CON
        jne     dr_none
        or      ecx,ecx
        jz      dr_done
        mov     dx,cx
        xor     cx,cx
dr_loop:
        push    dx
        push    cx
        push    di
        push    es
        call    con_in
        pop     es
        pop     di
        pop     cx
        pop     dx
        cmp     al,13
        je      dr_eol
        stosb
        inc     cx
        cmp     cx,dx
        jb      dr_loop
        jmp     short dr_fin
dr_eol:
        mov     al,13
        stosb
        inc     cx
        cmp     cx,dx
        jae     dr_fin
        mov     al,10
        stosb
        inc     cx
dr_fin:
        movzx   ecx,cx
dr_done:
        pop     es
        pop     di
        ret
dr_none:
        xor     ecx,ecx
        pop     es
        pop     di
        ret

dev_write:
        push    si
        push    ds
        mov     al,[si+SFT_DRIVE]
        cmp     al,DEV_NUL
        je      dw_sink
        cmp     al,DEV_CON
        jne     dw_sink
        or      ecx,ecx
        jz      dw_done
        push    ecx
        mov     si,di
        push    es
        pop     ds
dw_loop:
        lodsb
        mov     dl,al
        push    ecx
        push    si
        push    ds
        push    cs
        pop     ds
        call    con_out
        pop     ds
        pop     si
        pop     ecx
        dec     ecx
        jnz     dw_loop
        pop     ecx
dw_done:
        pop     ds
        pop     si
        ret
dw_sink:
        pop     ds
        pop     si
        ret

;=====================================================================
; Data
;=====================================================================
rd_iowr     db  0
rd_iolba    dd  0
rd_iooff    dd  0
rd_iolen    dd  0
rd_iodone   dd  0

rd_opmode   dw  0
rd_opvol    dw  0
rd_ophnd    dw  0
rd_opidx    db  0
rd_crattr   db  0
rd_crnew    db  0
rd_crdir    dd  0
rd_tmp      dw  0

rd_ffvol    dw  0
rd_ffdir    dd  0
rd_ffname   db  11 dup(0)
rd_ffattr   db  0
rd_ffsec    dd  0
rd_ffclus   dd  0
rd_ffleft   dd  0
rd_ffroot   db  0
rd_ffent    dw  0
