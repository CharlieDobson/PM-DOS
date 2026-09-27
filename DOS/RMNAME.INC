;=====================================================================
; RMNAME.INC - the calls that make, unmake and rename names:
;              39h MD, 3Ah RD, 56h rename and move, 60h TRUENAME, and
;              29h, which parses a name into an FCB.
;
; ALL FIVE WERE "FUNCTION NOT SUPPORTED" UNTIL v0.60.  Nothing that ran
; on RMDOS before then asked: Windows' start-up only reads.  The first
; thing that did was the 16-bit COMMAND.COM in a Windows DOS session,
; which is a DOS prompt - MD, RD and REN are three of its commands, and
; it asks 60h for the batch file it is running, for DIR's heading and
; for COPY's "a file cannot be copied onto itself".  A DOS box that
; cannot rename a file is not a DOS box.
;
; Built on the same three pieces as everything else here: rd_path for
; where a name lives, dir_find for whether it is there, dir_alloc for
; a new entry.  Long names are not made - RMDOS is an 8.3 DOS - but
; they are not orphaned either: an entry that is renamed or removed
; takes the long-name pieces in front of it with it.
;=====================================================================

IFNDEF ERR_CURDIR
ERR_CURDIR      EQU     16              ; attempt to remove the current directory
ENDIF
IFNDEF ERR_NOTSAME
ERR_NOTSAME     EQU     17              ; not the same device
ENDIF

;---------------------------------------------------------------------
; rn_badname - CF if rd_fcb cannot be the name of a new entry: blank,
;              a dot, or wildcards in it.
;---------------------------------------------------------------------
rn_badname:
        push    cx
        push    si
        mov     si,OFFSET rd_fcb
        cmp     byte ptr [si],' '
        je      rnb_bad
        cmp     byte ptr [si],'.'
        je      rnb_bad
        mov     cx,11
rnb_scan:
        cmp     byte ptr [si],'?'
        je      rnb_bad
        inc     si
        loop    rnb_scan
        pop     si
        pop     cx
        clc
        ret
rnb_bad:
        pop     si
        pop     cx
        stc
        ret

;---------------------------------------------------------------------
; rn_fill - a fresh directory entry at rd_dirbuf + BX: the eleven bytes
;           at DS:SI, attribute AL, first cluster EDX, size 0, now.
;---------------------------------------------------------------------
rn_fill:
        pushad
        push    es
        push    cs
        pop     es
        lea     di,[rd_dirbuf + bx]
        cld
        mov     cx,11
        rep     movsb
        stosb                           ; the attribute, at +0Bh
        xor     al,al
        mov     cx,DE_LEN - 12
        rep     stosb                   ; everything after it, zero
        mov     word ptr [rd_dirbuf + bx + DE_CLUSLO],dx
        shr     edx,16
        mov     word ptr [rd_dirbuf + bx + DE_CLUSHI],dx
        call    rd_stamp                ; -> AX time, DX date
        mov     word ptr [rd_dirbuf + bx + DE_TIME],ax
        mov     word ptr [rd_dirbuf + bx + DE_DATE],dx
        pop     es
        popad
        ret

;---------------------------------------------------------------------
; rn_droplfn - BX = an 8.3 entry in rd_dirbuf.  The long-name pieces
;              in front of it, in this sector, are marked deleted: an
;              entry renamed or removed by a DOS that cannot make long
;              names must not leave one pointing at the wrong file.
;              (Pieces in the sector before are left; their checksum
;              no longer matches anything, which is how an LFN reader
;              knows to ignore them.)
;---------------------------------------------------------------------
rn_droplfn:
        push    ax
        push    bx
rdl_back:
        sub     bx,DE_LEN
        jb      rdl_out
        mov     al,byte ptr [rd_dirbuf + bx + DE_ATTR]
        and     al,3Fh
        cmp     al,0Fh
        jne     rdl_out
        cmp     byte ptr [rd_dirbuf + bx],0E5h
        je      rdl_out
        mov     byte ptr [rd_dirbuf + bx],0E5h
        jmp     short rdl_back
rdl_out:
        pop     bx
        pop     ax
        ret

;=====================================================================
; 39h - make a directory.  DS:DX -> its name.
;
; A cluster for it, cleared; "." and ".." in its first sector; then its
; entry in the parent.  In that order, so that a parent that cannot
; take another entry costs a cluster given straight back and never a
; name that points at nothing.
;=====================================================================
fn39:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    rd_path                 ; -> EBX parent, rd_fcb, SI volume
        jc      md_nopath
        call    rn_badname
        jc      md_access
        mov     [rn_vol],si
        mov     dword ptr [rn_parent],ebx
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        call    dir_find
        jnc     md_access               ; a file or directory has the name

        call    fat_alloc               ; -> EAX, marked end of chain
        or      eax,eax
        jz      md_access               ; the disk is full
        mov     dword ptr [rn_clus],eax
        call    dir_zero
        jc      md_undo

        ; "." is itself and ".." its parent - 0 for the root, on FAT32
        ; as well, which is what DOS writes and what a walker expects
        mov     eax,dword ptr [rn_clus]
        call    clus2lba
        call    dir_win
        jc      md_undo
        xor     bx,bx
        mov     si,OFFSET rn_dot
        mov     al,ATTR_DIR
        mov     edx,dword ptr [rn_clus]
        call    rn_fill
        mov     bx,DE_LEN
        mov     si,OFFSET rn_dotdot
        mov     edx,dword ptr [rn_parent]
        call    rn_fill
        call    dir_commit

        ; and its name in the parent
        mov     si,[rn_vol]
        mov     eax,dword ptr [rn_parent]
        call    dir_alloc
        jc      md_undo
        mov     bx,[rd_deoff]
        mov     si,OFFSET rd_fcb
        mov     al,ATTR_DIR
        mov     edx,dword ptr [rn_clus]
        call    rn_fill
        call    dir_commit
        call    rd_flush
        jmp     i21_ok

md_undo:
        mov     si,[rn_vol]
        mov     eax,dword ptr [rn_clus]
        call    fat_freechain
        call    rd_flush
md_access:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
md_nopath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail

;=====================================================================
; 3Ah - remove a directory.  DS:DX -> its name.
;
; Refused, as DOS refuses it, when it is not empty (5), when it is where
; some drive is standing (16), and when it is the root.
;=====================================================================
fn3A:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    rd_path
        jc      rmd_nopath
        call    rn_badname              ; the root, ".", or wildcards
        jc      rmd_access
        mov     [rn_vol],si
        mov     dword ptr [rn_parent],ebx
        mov     di,OFFSET rd_fcb
        mov     eax,ebx
        call    dir_find
        jc      rmd_nopath
        mov     bx,[rd_deoff]
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_DIR
        jz      rmd_nopath
        call    rd_declus               ; -> EBX
        mov     dword ptr [rn_clus],ebx

        ; Somebody's current directory?  Every drive's, not just this
        ; one's: two letters can be one volume.
        xor     cx,cx
rmd_cds:
        mov     ax,CDS_LEN
        mul     cx
        mov     di,ax
        mov     eax,dword ptr [rd_cds + di + CDS_CLUS]
        cmp     eax,dword ptr [rn_clus]
        jne     rmd_cdsnext
        mov     al,cl
        call    rd_getvol               ; -> SI
        jc      rmd_cdsnext
        cmp     si,[rn_vol]
        je      rmd_curdir
rmd_cdsnext:
        inc     cx
        cmp     cx,LASTDRIVE
        jb      rmd_cds

        ; Empty?  Nothing in it but ".", ".." and deleted entries.
        mov     si,[rn_vol]
        mov     eax,dword ptr [rn_clus]
        call    dir_first
        jc      rmd_empty
rmd_sector:
        mov     eax,dword ptr [rd_dfsec]
        call    dir_win
        jc      rmd_access
        xor     bx,bx
rmd_entry:
        mov     al,byte ptr [rd_dirbuf + bx]
        or      al,al
        jz      rmd_empty               ; the end of the directory
        cmp     al,0E5h
        je      rmd_skip
        cmp     al,'.'
        je      rmd_skip                ; "." and ".."
        mov     al,byte ptr [rd_dirbuf + bx + DE_ATTR]
        and     al,3Fh
        cmp     al,0Fh
        jne     rmd_access              ; a real entry: not empty
rmd_skip:
        add     bx,DE_LEN
        cmp     bx,SECSIZE
        jb      rmd_entry
        call    dir_next
        jnc     rmd_sector
rmd_empty:
        ; The walk moved the window: find the entry again, then go
        mov     si,[rn_vol]
        mov     eax,dword ptr [rn_parent]
        mov     di,OFFSET rd_fcb
        call    dir_find
        jc      rmd_nopath
        mov     bx,[rd_deoff]
        call    rn_droplfn
        mov     byte ptr [rd_dirbuf + bx],0E5h
        call    dir_commit
        mov     eax,dword ptr [rn_clus]
        call    fat_freechain
        call    rd_flush
        jmp     i21_ok

rmd_curdir:
        mov     ax,ERR_CURDIR
        jmp     i21_fail
rmd_access:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
rmd_nopath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail

;=====================================================================
; 56h - rename.  DS:DX -> the old name, ES:DI -> the new one.
;
; In place when both are in one directory - the eleven bytes change and
; nothing else does, which is how a directory is renamed too.  A FILE
; can also move to another directory of the same drive, as DOS 3+
; allows: its entry is copied there under the new name and the old one
; deleted.  A directory cannot move - its ".." would lie - and DOS
; refuses that too.  No wildcards: COMMAND.COM expands REN's itself.
;=====================================================================
fn56:
        ; the new name first, while its pointer is in hand
        mov     es,[bp+F_ES]
        mov     si,[bp+F_DI]
        call    rd_path
        jc      ren_nopath
        call    rn_badname
        jc      ren_access
        mov     [rn_drv2],al
        mov     [rn_vol],si
        mov     dword ptr [rn_parent2],ebx
        mov     si,OFFSET rd_fcb
        mov     di,OFFSET rn_new
        call    rn_copy11

        mov     es,[bp+F_DS]
        mov     si,[bp+F_DX]
        call    rd_path
        jc      ren_nopath
        cmp     al,[rn_drv2]
        jne     ren_notsame
        call    rn_badname
        jc      ren_nofile
        mov     dword ptr [rn_parent],ebx
        mov     si,OFFSET rd_fcb
        mov     di,OFFSET rn_old
        call    rn_copy11

        ; the new name must be free...
        mov     si,[rn_vol]
        mov     eax,dword ptr [rn_parent2]
        mov     di,OFFSET rn_new
        call    dir_find
        jnc     ren_access
        ; ...and the old one there
        mov     eax,dword ptr [rn_parent]
        mov     di,OFFSET rn_old
        call    dir_find
        jc      ren_nofile
        mov     bx,[rd_deoff]
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_VOLUME
        jnz     ren_nofile

        mov     eax,dword ptr [rn_parent]
        cmp     eax,dword ptr [rn_parent2]
        jne     ren_move

        ; one directory: the name changes where it stands
        call    rn_droplfn
        mov     si,OFFSET rn_new
        lea     di,[rd_dirbuf + bx]
        call    rn_copy11
        call    dir_commit
        call    rd_flush
        jmp     i21_ok

ren_move:
        test    byte ptr [rd_dirbuf + bx + DE_ATTR],ATTR_DIR
        jnz     ren_access
        lea     si,[rd_dirbuf + bx]     ; the whole entry, before the
        mov     di,OFFSET rn_ent        ; window moves
        mov     cx,DE_LEN
        push    es
        push    cs
        pop     es
        cld
        rep     movsb
        pop     es
        mov     eax,dword ptr [rd_delba]
        mov     dword ptr [rn_oldlba],eax
        mov     [rn_oldoff],bx

        mov     si,[rn_vol]
        mov     eax,dword ptr [rn_parent2]
        call    dir_alloc
        jc      ren_access
        mov     bx,[rd_deoff]
        mov     si,OFFSET rn_ent
        lea     di,[rd_dirbuf + bx]
        mov     cx,DE_LEN
        push    es
        push    cs
        pop     es
        cld
        rep     movsb
        pop     es
        mov     si,OFFSET rn_new
        lea     di,[rd_dirbuf + bx]
        call    rn_copy11
        call    dir_commit

        mov     si,[rn_vol]
        mov     eax,dword ptr [rn_oldlba]
        call    dir_win
        jc      ren_access
        mov     bx,[rn_oldoff]
        call    rn_droplfn
        mov     byte ptr [rd_dirbuf + bx],0E5h
        call    dir_commit
        call    rd_flush
        jmp     i21_ok

ren_notsame:
        mov     ax,ERR_NOTSAME
        jmp     i21_fail
ren_access:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
ren_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
ren_nopath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail

;--- rn_copy11 - DS:SI -> DS:DI, eleven bytes.  All ours.
rn_copy11:
        push    cx
        push    si
        push    di
        push    es
        push    cs
        pop     es
        mov     cx,11
        cld
        rep     movsb
        pop     es
        pop     di
        pop     si
        pop     cx
        ret

;=====================================================================
; 60h - TRUENAME.  DS:SI -> a name, ES:DI -> 128 bytes for its one true
; spelling (rd_canon).  A device answers as DOS answers it: "C:/CON",
; with the forward slash that says it is not a file in any directory.
;=====================================================================
fn60:
        mov     es,[bp+F_DS]
        mov     si,[bp+F_SI]
        call    rd_canon
        jc      f60_bad

        ; the last component: a device?
        mov     si,OFFSET rd_canbuf + 2
        mov     di,si
f60_last:
        inc     si
        cmp     byte ptr [si],0
        je      f60_gotlast
        cmp     byte ptr [si],'\'
        jne     f60_last
        mov     di,si
        jmp     short f60_last
f60_gotlast:
        inc     di                      ; past the '\' before it
        push    di
        mov     si,di
        mov     di,OFFSET rc_n83
        push    es
        push    cs
        pop     es
        call    rd_name83
        pop     es
        pop     di
        mov     si,OFFSET rd_devtab
f60_dev:
        cmp     byte ptr [si],0FFh
        je      f60_copy
        push    si
        push    di
        inc     si
        mov     di,OFFSET rc_n83
        mov     cx,8
        push    es
        push    cs
        pop     es
        cld
        repe    cmpsb
        pop     es
        pop     di
        pop     si
        je      f60_isdev
        add     si,9
        jmp     short f60_dev
f60_isdev:
        ; "D:/NAME" - the drive, a slash, and the name without the path
        ; or any extension, as DOS 6 spells it
        mov     byte ptr [rd_canbuf + 2],'/'
        mov     si,OFFSET rc_n83
        mov     di,OFFSET rd_canbuf + 3
        mov     cx,8
f60_devname:
        mov     al,[si]
        cmp     al,' '
        je      f60_devend
        mov     [di],al
        inc     si
        inc     di
        loop    f60_devname
f60_devend:
        mov     byte ptr [di],0

f60_copy:
        mov     es,[bp+F_ES]
        mov     di,[bp+F_DI]
        mov     si,OFFSET rd_canbuf
        cld
f60_out:
        lodsb
        stosb
        or      al,al
        jnz     f60_out
        mov     word ptr [bp+F_AX],0
        jmp     i21_ok
f60_bad:
        jmp     i21_fail

;=====================================================================
; 29h - parse a file name into an FCB.  DS:SI -> the text, ES:DI -> the
; FCB, AL = what to leave alone:
;
;   bit 0   scan off one leading separator ( : . ; , = + )
;   bit 1   the drive, if the text names none
;   bit 2   the name, if the text has none
;   bit 3   the extension, if the text has none
;
; -> AL = 0, or 1 if a '?' or '*' was in it, or FFh if the drive letter
;    names no drive; DS:SI past what was parsed.  COMMAND.COM fills the
;    two FCBs of every program it starts this way.
;=====================================================================
fn29:
        push    fs
        mov     fs,[bp+F_DS]
        mov     si,[bp+F_SI]
        mov     es,[bp+F_ES]
        mov     di,[bp+F_DI]
        mov     cl,byte ptr [bp+F_AX]
        mov     byte ptr [p29_res],0
        mov     byte ptr [p29_wild],0
        call    p29_blanks
        test    cl,1
        jz      p29_drive
        mov     al,fs:[si]
        call    p29_issep
        jne     p29_drive
        inc     si
        call    p29_blanks
p29_drive:
        cmp     byte ptr fs:[si+1],':'
        jne     p29_nodrive
        mov     al,fs:[si]
        call    rd_upcase
        cmp     al,'A'
        jb      p29_nodrive
        cmp     al,'Z'
        ja      p29_nodrive
        sub     al,'A'
        add     si,2
        push    si
        push    ax
        movzx   si,al
        mov     ax,VOL_LEN
        mul     si
        mov     si,ax
        pop     ax
        cmp     al,LASTDRIVE
        jae     p29_baddrv
        test    byte ptr [rd_vols + si + VOL_FLAGS],VF_PRESENT
        jnz     p29_gooddrv
p29_baddrv:
        mov     byte ptr [p29_res],0FFh
p29_gooddrv:
        pop     si
        inc     al                      ; 1 = A in an FCB
        mov     es:[di],al
        jmp     short p29_name
p29_nodrive:
        test    cl,2
        jnz     p29_name
        mov     byte ptr es:[di],0
p29_name:
        push    di
        inc     di                      ; FCB+1, the name
        mov     bx,8
        mov     dl,cl
        and     dl,4
        call    p29_field
        pop     di
        push    di
        add     di,9                    ; FCB+9, the extension
        mov     bx,3
        cmp     byte ptr fs:[si],'.'
        jne     p29_noext
        inc     si
        xor     dl,dl                   ; a dot says there IS one, if empty
        jmp     short p29_ext
p29_noext:
        mov     dl,cl
        and     dl,8
p29_ext:
        call    p29_field
        pop     di
        mov     [bp+F_SI],si
        mov     al,[p29_res]            ; FFh for a bad drive, whatever else
        or      al,al
        jnz     p29_ans
        mov     al,[p29_wild]
p29_ans:
        mov     byte ptr [bp+F_AX],al
        pop     fs
        jmp     i21_ok

;--- p29_blanks - skip spaces and tabs at FS:SI
p29_blanks:
        mov     al,fs:[si]
        cmp     al,' '
        je      p29b_next
        cmp     al,9
        jne     p29b_out
p29b_next:
        inc     si
        jmp     short p29_blanks
p29b_out:
        ret

;--- p29_issep - ZF if AL is one of the separators bit 0 scans off
p29_issep:
        push    di
        push    cx
        push    es
        push    cs
        pop     es
        mov     di,OFFSET p29_seps
        mov     cx,P29_NSEPS
        cld
        repne   scasb
        pop     es
        pop     cx
        pop     di
        ret

;--- p29_isterm - ZF if AL ends a name: a control character, a space,
;--- or one of the characters no FCB name may hold.
p29_isterm:
        cmp     al,' '
        jbe     p29t_yes
        push    di
        push    cx
        push    es
        push    cs
        pop     es
        mov     di,OFFSET p29_terms
        mov     cx,P29_NTERMS
        cld
        repne   scasb
        pop     es
        pop     cx
        pop     di
        ret
p29t_yes:
        cmp     al,al                   ; ZF
        ret

;--- p29_field - FS:SI -> text, ES:DI -> a field of BX bytes.  The name
;--- up to a terminator, upper case, '*' filling the rest with '?',
;--- anything past the field's length skipped.  DL nonzero: leave the
;--- field as it was when the text has nothing for it.  SI moves on.
p29_field:
        mov     al,fs:[si]
        call    p29_isterm
        jne     pf_some
        or      dl,dl
        jnz     pf_out                  ; nothing given, and left alone
        mov     cx,bx
        mov     al,' '
        cld
        rep     stosb
        ret
pf_some:
        mov     cx,bx
        cld
pf_loop:
        mov     al,fs:[si]
        call    p29_isterm
        je      pf_pad
        inc     si
        cmp     al,'*'
        je      pf_star
        jcxz    pf_loop                 ; past the field: swallowed
        call    rd_upcase
        cmp     al,'?'
        jne     pf_store
        mov     byte ptr [p29_wild],1
pf_store:
        stosb
        dec     cx
        jmp     short pf_loop
pf_star:
        jcxz    pf_loop
        mov     byte ptr [p29_wild],1
        mov     al,'?'
        rep     stosb
        jmp     short pf_loop
pf_pad:
        mov     al,' '
        rep     stosb
pf_out:
        ret

;=====================================================================
; Data
;=====================================================================
rn_vol      dw  0
rn_parent   dd  0
rn_parent2  dd  0
rn_clus     dd  0
rn_oldlba   dd  0
rn_oldoff   dw  0
rn_drv2     db  0
rn_new      db  11 dup(' ')
rn_old      db  11 dup(' ')
rn_ent      db  DE_LEN dup(0)
rn_dot      db  '.          '
rn_dotdot   db  '..         '
p29_res     db  0                       ; FFh: the drive letter is no drive
p29_wild    db  0                       ; 1: a wildcard was in it
p29_seps    db  ':.;,=+'
P29_NSEPS   EQU $ - p29_seps
p29_terms   db  '."/\[]:|<>+=;,'
P29_NTERMS  EQU $ - p29_terms
