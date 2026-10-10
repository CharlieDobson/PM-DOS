;=====================================================================
; NETTEST.ASM - NETTEST.COM, a redirector of its own, on the DOS under
;               Windows: a drive letter taken the way IFSMGR.386 takes
;               one, and every call RMDOS makes for it checked
;
; Windows for Workgroups' 32-bit file access takes a hard disk away from
; DOS by marking its current directory structure as a network drive's,
; and from then on DOS sends everything about that drive to INT 2Fh
; AH=11h, with its arguments in the swappable data area where MS-DOS 5
; keeps them.  RMDOS learned to do that on 2026-10-07 (DOS\RMNET.INC).
; This is the test, with no Windows in it: a redirector small enough to
; read, serving a handful of files out of its own memory, which checks
; every call it is handed against what MS-DOS would have handed it.
;
; ON THE KERNEL it only parks itself, as CDSTEST does, and prints what
; the copy on RMDOS wrote to C:\NETTEST.RM - the kernel has no
; redirector interface and nothing here asks it to.  ON RMDOS it:
;
;   takes the highest letter with no drive behind it (flags C000h, the
;   text "X:\", the root backslash at 2 - IFSMGR's own three writes),
;   hooks INT 2Fh, and makes the calls a program makes: create, write,
;   seek, close, open, read, dup, the attributes, find first and next,
;   rename, the directories and a relative name inside one, free space,
;   TRUENAME, the IOCTLs that ask "remote?", EXEC of a program on the
;   drive, and a child that ends with a file there still open;
;
;   then lets go of INT 2Fh with the drive still marked, so that RMDOS's
;   own handler is what answers - an open must fail and give its handle
;   and its file table entry back - and puts the drive back as it was.
;
; EACH STEP PRINTS THE 11xxh CALLS IT CAUSED, in order, and fails if
; they are not the ones MS-DOS makes, if the answer is wrong, or if the
; handler found anything out of place while the call was in it: the
; name not at WFP_START, THISCDS or ES:DI not the drive's structure or
; the file's entry, a new entry not marked busy, the handle not pointing
; at it, a read's buffer not brought under 16, a search's DTA not
; SEARCHBUF, PROC_ID not the current process.  The handler itself never
; calls DOS - DOS is not re-entrant, and RMDOS is DOS - so it only
; records, and the steps read the record.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_NT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_NT, DS:_NT, ES:_NT, SS:_NT
        ORG     0

CDS_LEN         EQU     58h
CDS_FLAGS       EQU     43h
CDS_BACK        EQU     4Fh

;--- the SDA, as MS-DOS 5 lays it out (RMINFO.INC's SD_*)
S_USERAX        EQU     1Ah
S_DTA           EQU     0Ch
S_PSP           EQU     10h
S_PROCID        EQU     1Ch
S_FN1           EQU     9Eh
S_FN2           EQU     11Eh
S_SRCH          EQU     19Eh
S_SATTR         EQU     24Dh
S_THISSFT       EQU     27Eh
S_THISCDS       EQU     282h
S_SFN           EQU     28Ah
S_JFN           EQU     28Ch
S_PJFN          EQU     28Eh
S_WFP           EQU     292h
S_RENWFP        EQU     294h

;--- what the handler can find wrong, a bit each
A_PROC          EQU     0001h           ; PROC_ID is not the SDA's PSP
A_WFP           EQU     0002h           ; WFP_START is not FN1
A_CDS           EQU     0004h           ; THISCDS is not the drive's
A_ESDI          EQU     0008h           ; ES:DI is not what DOS passes
A_BUSY          EQU     0010h           ; a new entry not marked busy
A_JFT           EQU     0020h           ; the handle not at the entry
A_DTA           EQU     0040h           ; a search's DTA not SEARCHBUF
A_ALIGN         EQU     0080h           ; a transfer's offset not < 16
A_UNK           EQU     0100h           ; a function nobody should send
A_SIG           EQU     0200h           ; an entry that is not ours
A_REN           EQU     0400h           ; REN_WFP is not FN2

;--- the files this redirector keeps
E_PATH          EQU     00h             ; 48 bytes, "X:\DIR\NAME.EXT"
E_ATTR          EQU     30h
E_SIZE          EQU     32h             ; dw
E_TIME          EQU     34h
E_DATE          EQU     36h
E_DATA          EQU     40h             ; 128 bytes
E_MAX           EQU     128
E_LEN           EQU     0C0h
N_FILES         EQU     8

;--- a file table entry, as DOS 4+ lays one out
SF_REF          EQU     00h
SF_MODE         EQU     02h
SF_ATTR         EQU     04h
SF_FLAGS        EQU     05h
SF_DEVPTR       EQU     07h
SF_TIME         EQU     0Dh
SF_DATE         EQU     0Fh
SF_SIZE         EQU     11h
SF_POS          EQU     15h
SF_NAME         EQU     20h

FTIME           EQU     6000h           ; 12:00:00
FDATE           EQU     5947h           ; 2024-10-07

start:
        mov     bx,1000h                ; 64K: room for the park to load
        mov     ah,4Ah                  ; RMDOS, and for EXEC over there
        int     21h

        mov     word ptr [outh],1
        mov     ax,0F100h
        xor     bx,bx
        int     21h
        or      bx,bx
        jz      rm_side

;--- on the kernel -----------------------------------------------------
        mov     dx,OFFSET m_hi
        call    say
        mov     dx,OFFSET m_park
        call    say
        call    ownpath
        jc      k_nopath
        mov     ax,0F10Bh
        int     21h
        jc      k_noparkd
        mov     [rmexit],al

        mov     dx,OFFSET n_rep
        mov     ax,3D00h
        int     21h
        jc      k_norep
        mov     [repfh],ax
k_copy:
        mov     bx,[repfh]
        mov     cx,128
        mov     dx,OFFSET buf2
        mov     ah,3Fh
        int     21h
        jc      k_copied
        or      ax,ax
        jz      k_copied
        mov     cx,ax
        mov     bx,[outh]
        mov     dx,OFFSET buf2
        mov     ah,40h
        int     21h
        jmp     short k_copy
k_copied:
        mov     bx,[repfh]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET n_rep
        mov     ah,41h
        int     21h
        mov     dx,OFFSET m_rmexit
        call    say
        mov     al,[rmexit]
        call    put2
        call    crlf
        jmp     short k_end
k_nopath:
        mov     dx,OFFSET m_nopath
        call    say
        mov     byte ptr [rmexit],1
        jmp     short k_end
k_noparkd:
        mov     dx,OFFSET m_nopark
        call    say
        call    put4
        call    crlf
        mov     byte ptr [rmexit],1
        jmp     short k_end
k_norep:
        mov     dx,OFFSET m_norep
        call    say
        mov     byte ptr [rmexit],1
k_end:
        mov     al,[rmexit]
        or      al,al
        jnz     k_somebad
        mov     dx,OFFSET m_allok
        call    say
        jmp     short k_bye
k_somebad:
        mov     dx,OFFSET m_fails
        call    say
        mov     al,[rmexit]
        call    put2
        call    crlf
k_bye:
        mov     dx,OFFSET m_bye
        call    say
        mov     al,[rmexit]
        mov     ah,4Ch
        int     21h

;--- on RMDOS ----------------------------------------------------------
rm_side:
        mov     dx,OFFSET n_rep
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      rm_nofile
        mov     [outh],ax
        mov     dx,OFFSET m_rmhi
        call    say
        call    checks
        mov     bx,[outh]
        mov     ah,3Eh
        int     21h
rm_out:
        mov     al,[fails]
        mov     ah,4Ch
        int     21h
rm_nofile:
        mov     byte ptr [fails],0FFh
        jmp     short rm_out

;=====================================================================
; checks
;=====================================================================
checks:
        mov     byte ptr [fails],0

;--- the SDA, and a letter to take --------------------------------------
        mov     dx,OFFSET t_sda
        call    say
        push    ds
        mov     ax,5D06h
        int     21h
        mov     ax,ds
        pop     ds
        jc      sd_bad
        mov     [sdaseg],ax
        mov     [sdaoff],si
        call    put4
        mov     dl,':'
        call    putc
        mov     ax,si
        call    put4
        call    ok
        jmp     short sd_out
sd_bad:
        call    bad
        ret
sd_out:

        mov     dx,OFFSET t_take
        call    say
        call    pickdrv                 ; -> [drv], ES:DI its structure
        jc      tk_none
        mov     dl,[letter]
        call    putc
        mov     dx,OFFSET m_colon
        call    say
        ; kept, to be put back; then IFSMGR's own three writes
        push    ds
        push    es
        pop     ds
        mov     si,di
        push    cs
        pop     es
        push    di
        mov     di,OFFSET savecds
        mov     cx,CDS_LEN
        cld
        rep     movsb
        pop     di
        pop     ds
        mov     es,[cdsseg]
        mov     al,[letter]
        mov     byte ptr es:[di],al
        mov     word ptr es:[di+1],'\:'
        mov     byte ptr es:[di+3],0
        mov     word ptr es:[di+CDS_FLAGS],0C000h
        mov     word ptr es:[di+CDS_BACK],2
        push    cs
        pop     es
        call    ok
        jmp     short tk_out
tk_none:
        mov     dx,OFFSET m_none
        call    say
        call    bad
        ret
tk_out:
        ; the names, with that letter in them
        mov     al,[letter]
        mov     si,OFFSET n_first
tk_patch:
        mov     di,[si]
        or      di,di
        jz      tk_patched
        mov     [di],al
        add     si,2
        jmp     short tk_patch
tk_patched:
        mov     [ucom_name],al

;--- 1100h before and after -----------------------------------------------
        mov     dx,OFFSET t_inst0
        call    say
        mov     ax,1100h
        int     2Fh
        call    put2
        cmp     al,0
        call    okif

        mov     ax,352Fh
        int     21h
        mov     word ptr [old2f],bx
        mov     word ptr [old2f+2],es
        push    cs
        pop     es
        mov     dx,OFFSET nt_i2f
        mov     ax,252Fh
        int     21h

        mov     dx,OFFSET t_inst1
        call    say
        mov     ax,1100h
        int     2Fh
        call    put2
        cmp     al,0FFh
        call    okif

;--- 3Ch create, 40h write, 42h, 3Eh -------------------------------------
        mov     dx,OFFSET t_create
        mov     si,OFFSET x_create
        call    step
        mov     dx,OFFSET n_net1
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        mov     [h1],ax
        setc    byte ptr [rescf]
        call    endstep
        jc      cr_bad
        cmp     byte ptr [rescf],0
        jne     cr_bad
        cmp     word ptr [lparm],0020h  ; the attribute, archive; AH=0
        jne     cr_bad
        cmp     byte ptr [lsattr],06h
        jne     cr_bad
        cmp     byte ptr [lusrah],3Ch
        jne     cr_bad
        mov     si,OFFSET lfn1
        mov     di,OFFSET n_net1
        call    strsame
        jne     cr_bad
        call    ok
        jmp     short cr_out
cr_bad:
        call    bad
cr_out:

        mov     dx,OFFSET t_write
        mov     si,OFFSET x_write
        call    step
        mov     bx,[h1]
        mov     dx,OFFSET hello
        mov     cx,11
        mov     ah,40h
        int     21h
        mov     [res],ax
        setc    byte ptr [rescf]
        call    endstep
        jc      wr_bad
        cmp     byte ptr [rescf],0
        jne     wr_bad
        cmp     word ptr [res],11
        jne     wr_bad
        call    ok
        jmp     short wr_out
wr_bad:
        call    bad
wr_out:

        mov     dx,OFFSET t_seekend
        mov     si,OFFSET x_none
        call    step
        mov     bx,[h1]
        xor     cx,cx
        xor     dx,dx
        mov     ax,4202h                ; compatibility mode: the entry's size
        int     21h
        mov     [res],ax
        call    endstep
        jc      se_bad
        cmp     word ptr [res],11
        jne     se_bad
        call    ok
        jmp     short se_out
se_bad:
        call    bad
se_out:

        mov     dx,OFFSET t_close
        mov     si,OFFSET x_close
        call    step
        mov     bx,[h1]
        mov     ah,3Eh
        int     21h
        call    endstep
        call    okifnc

;--- 3Dh open, 42h from the end, 3Fh read, 57h, 4400h, 440Ah, 45h --------
        mov     dx,OFFSET t_open
        mov     si,OFFSET x_open
        call    step
        mov     dx,OFFSET n_net1
        mov     ax,3D40h                ; read, deny none
        int     21h
        mov     [h1],ax
        setc    byte ptr [rescf]
        call    endstep
        jc      op_bad
        cmp     byte ptr [rescf],0
        jne     op_bad
        cmp     word ptr [lparm],0040h
        jne     op_bad
        cmp     byte ptr [lsattr],16h
        jne     op_bad
        call    ok
        jmp     short op_out
op_bad:
        call    bad
op_out:

        mov     dx,OFFSET t_lseek
        mov     si,OFFSET x_lseek
        call    step
        mov     bx,[h1]
        mov     cx,0FFFFh
        mov     dx,0FFFDh               ; three back from the end
        mov     ax,4202h                ; deny none: the redirector says
        int     21h
        mov     [res],ax
        call    endstep
        jc      ls_bad
        cmp     word ptr [res],8
        jne     ls_bad
        call    ok
        jmp     short ls_out
ls_bad:
        call    bad
ls_out:

        mov     dx,OFFSET t_read
        mov     si,OFFSET x_read
        call    step
        mov     bx,[h1]
        xor     cx,cx
        xor     dx,dx
        mov     ax,4200h
        int     21h
        mov     bx,[h1]
        mov     dx,OFFSET buf2 + 5      ; an offset that is not a paragraph's
        mov     cx,100
        mov     ah,3Fh
        int     21h
        mov     [res],ax
        call    endstep
        jc      rd_bad
        cmp     word ptr [res],11
        jne     rd_bad
        mov     si,OFFSET buf2 + 5
        mov     di,OFFSET hello
        mov     cx,11
        push    cs
        pop     es
        cld
        repe    cmpsb
        jne     rd_bad
        call    ok
        jmp     short rd_out
rd_bad:
        call    bad
rd_out:

        mov     dx,OFFSET t_times
        mov     si,OFFSET x_none
        call    step
        mov     bx,[h1]
        mov     ax,5700h
        int     21h
        mov     [res],cx
        mov     [res2],dx
        call    endstep
        jc      tm_bad
        cmp     word ptr [res],FTIME
        jne     tm_bad
        cmp     word ptr [res2],FDATE
        jne     tm_bad
        call    ok
        jmp     short tm_out
tm_bad:
        call    bad
tm_out:

        mov     dx,OFFSET t_info
        mov     si,OFFSET x_none
        call    step
        mov     bx,[h1]
        mov     ax,4400h
        int     21h
        mov     [res],dx
        mov     bx,[h1]
        mov     ax,440Ah
        int     21h
        mov     [res2],dx
        call    endstep
        jc      in_bad
        mov     ax,[res]
        call    put4
        mov     dl,' '
        call    putc
        mov     ax,[res2]
        call    put4
        test    word ptr [res],8000h
        jz      in_bad
        test    word ptr [res2],8000h
        jz      in_bad
        call    ok
        jmp     short in_out
in_bad:
        call    bad
in_out:

        mov     dx,OFFSET t_dup
        mov     si,OFFSET x_dup
        call    step
        mov     bx,[h1]
        mov     ah,45h
        int     21h
        jc      du_fail
        mov     bx,ax
        mov     ah,3Eh                  ; the copy: one 1106h
        int     21h
        mov     bx,[h1]
        mov     ah,3Eh                  ; and the file: another
        int     21h
du_fail:
        call    endstep
        call    okifnc

;--- 5Bh on a name that is there: the redirector refuses -----------------
        mov     dx,OFFSET t_new
        mov     si,OFFSET x_create
        call    step
        mov     dx,OFFSET n_net1
        xor     cx,cx
        mov     ah,5Bh
        int     21h
        mov     [res],ax
        pushf
        call    endstep
        popf
        jnc     nw_bad
        cmp     word ptr [res],50h
        jne     nw_bad
        cmp     word ptr [lparm],0120h  ; AH=1: an existing file is refused
        jne     nw_bad
        call    ok
        jmp     short nw_out
nw_bad:
        call    bad
nw_out:

;--- 4409h, 43h get and set ----------------------------------------------
        mov     dx,OFFSET t_remote
        mov     si,OFFSET x_none
        call    step
        mov     bl,[drv]
        inc     bl
        mov     ax,4409h
        int     21h
        mov     [res],dx
        call    endstep
        jc      rm9_bad
        mov     ax,[res]
        call    put4
        cmp     word ptr [res],1000h
        jne     rm9_bad
        call    ok
        jmp     short rm9_out
rm9_bad:
        call    bad
rm9_out:

        mov     dx,OFFSET t_attr
        mov     si,OFFSET x_attr
        call    step
        mov     dx,OFFSET n_net1
        mov     cx,0021h                ; read-only and archive
        mov     ax,4301h
        int     21h
        jc      at_fail
        mov     dx,OFFSET n_net1
        mov     ax,4300h
        int     21h
        mov     [res],cx
at_fail:
        call    endstep
        jc      at_bad
        cmp     word ptr [res],0021h
        jne     at_bad
        cmp     word ptr [lparm],0021h  ; what 110Eh was pushed
        jne     at_bad
        cmp     byte ptr [lsattr],16h
        jne     at_bad
        call    ok
        jmp     short at_out
at_bad:
        call    bad
at_out:

;--- 4Eh and 4Fh ------------------------------------------------------------
        mov     dx,OFFSET t_find
        mov     si,OFFSET x_find
        call    step
        mov     dx,OFFSET dta
        mov     ah,1Ah
        int     21h
        mov     dx,OFFSET n_all
        mov     cx,0
        mov     ah,4Eh
        int     21h
        jc      fd_fail
        mov     si,OFFSET dta + 1Eh
        mov     di,OFFSET n_found
        call    strsame
        jne     fd_fail
        cmp     word ptr [dta + 1Ah],11
        jne     fd_fail
        test    byte ptr [dta],80h
        jz      fd_fail
        mov     ah,4Fh
        int     21h
        mov     [res],ax
        jnc     fd_fail                 ; one file: the next is none
        call    endstep
        jc      fd_bad
        cmp     word ptr [res],12h
        jne     fd_bad
        cmp     byte ptr [lsattr],0
        jne     fd_bad
        mov     si,OFFSET lfn1
        mov     di,OFFSET n_allc
        call    strsame
        jne     fd_bad
        call    ok
        jmp     short fd_out
fd_fail:
        call    endstep
fd_bad:
        call    bad
fd_out:

;--- 56h -----------------------------------------------------------------
        mov     dx,OFFSET t_ren
        mov     si,OFFSET x_ren
        call    step
        mov     dx,OFFSET n_net1
        mov     di,OFFSET n_net2
        push    cs
        pop     es
        mov     ah,56h
        int     21h
        call    endstep
        jc      rn_bad
        mov     si,OFFSET lfn2
        mov     di,OFFSET n_net2
        call    strsame
        jne     rn_bad
        call    ok
        jmp     short rn_out
rn_bad:
        call    bad
rn_out:

;--- 39h, 3Bh, 47h, a relative name, 41h, 3Ah --------------------------
        mov     dx,OFFSET t_dirs
        mov     si,OFFSET x_dirs
        call    step
        mov     dx,OFFSET n_sub
        mov     ah,39h
        int     21h
        jc      dr_fail
        mov     dx,OFFSET n_sub
        mov     ah,3Bh
        int     21h
        jc      dr_fail
        mov     ah,19h
        int     21h
        mov     [olddrv],al
        mov     dl,[drv]
        mov     ah,0Eh
        int     21h
        mov     dx,OFFSET n_rel         ; "REL.TXT", in the directory just
        xor     cx,cx                   ; made current
        mov     ah,3Ch
        int     21h
        jc      dr_fail
        mov     bx,ax
        mov     ah,3Eh
        int     21h
        mov     si,OFFSET lfn1          ; the name the create was given
        mov     di,OFFSET n_relfull
        call    strsame
        jne     dr_fail
        mov     si,OFFSET buf
        mov     dl,0
        mov     ah,47h
        int     21h
        jc      dr_fail
        mov     si,OFFSET buf
        mov     di,OFFSET n_subonly
        call    strsame
        jne     dr_fail
        mov     dx,OFFSET n_relfull
        mov     ah,41h
        int     21h
        jc      dr_fail
        mov     dx,OFFSET n_root
        mov     ah,3Bh
        int     21h
        jc      dr_fail
        mov     dl,[olddrv]
        mov     ah,0Eh
        int     21h
        mov     dx,OFFSET n_sub
        mov     ah,3Ah
        int     21h
        jc      dr_fail
        call    endstep
        jc      dr_bad
        cmp     byte ptr [lsattr],06h   ; 41h's, the last that set it
        jne     dr_bad
        call    ok
        jmp     short dr_out
dr_fail:
        mov     dl,[olddrv]
        mov     ah,0Eh
        int     21h
        call    endstep
dr_bad:
        call    bad
dr_out:

;--- 36h and 1Ch ---------------------------------------------------------
        mov     dx,OFFSET t_space
        mov     si,OFFSET x_space
        call    step
        mov     dl,[drv]
        inc     dl
        mov     ah,36h
        int     21h
        mov     [res],ax
        mov     [res2],bx
        mov     [res3],dx
        push    ds
        mov     dl,[drv]
        inc     dl
        mov     ah,1Ch
        int     21h
        mov     ah,[bx]
        pop     ds
        mov     [res4],ax
        call    endstep
        jc      sp_bad
        cmp     word ptr [res],4
        jne     sp_bad
        cmp     word ptr [res2],500
        jne     sp_bad
        cmp     word ptr [res3],1000
        jne     sp_bad
        cmp     word ptr [res4],0F804h
        jne     sp_bad
        call    ok
        jmp     short sp_out
sp_bad:
        call    bad
sp_out:

;--- 60h -----------------------------------------------------------------
        mov     dx,OFFSET t_true
        mov     si,OFFSET x_none
        call    step
        mov     si,OFFSET n_tname
        mov     di,OFFSET buf
        push    cs
        pop     es
        mov     ah,60h
        int     21h
        call    endstep
        jc      tn_bad
        mov     si,OFFSET buf
        mov     di,OFFSET n_net2
        call    strsame
        jne     tn_bad
        call    ok
        jmp     short tn_out
tn_bad:
        call    bad
tn_out:

;--- a name with a wildcard is refused before the redirector hears ------
        mov     dx,OFFSET t_wild
        mov     si,OFFSET x_none
        call    step
        mov     dx,OFFSET n_wild
        mov     ax,3D00h
        int     21h
        mov     [res],ax
        pushf
        call    endstep
        popf
        jnc     wl_bad
        cmp     word ptr [res],2
        jne     wl_bad
        call    ok
        jmp     short wl_out
wl_bad:
        call    bad
wl_out:

;--- EXEC of a program on the drive ------------------------------------
        mov     dx,OFFSET t_exec
        mov     si,OFFSET n_tcom
        mov     di,OFFSET tcom
        mov     cx,TCOM_LEN
        call    putfile                 ; prints the title
        mov     si,OFFSET x_exec
        call    stepq
        mov     dx,OFFSET n_tcom
        call    run                     ; FFFFh if it could not
        mov     [res],ax
        call    endstep
        jc      ex_bad
        cmp     word ptr [res],002Ah    ; T.COM's own exit code
        jne     ex_bad
        call    ok
        jmp     short ex_out
ex_bad:
        call    bad
ex_out:

;--- a child that ends with a file on the drive still open -------------
        mov     dx,OFFSET t_orphan
        mov     si,OFFSET n_ucom
        mov     di,OFFSET ucom
        mov     cx,UCOM_LEN
        call    putfile
        mov     si,OFFSET x_orphan
        call    stepq
        mov     dx,OFFSET n_ucom
        call    run
        mov     [res],ax
        call    endstep
        jc      or_bad
        cmp     word ptr [res],0
        jne     or_bad
        call    ok
        jmp     short or_out
or_bad:
        call    bad
or_out:

;--- 41h the files, as the next test would want them gone --------------
        mov     dx,OFFSET n_net2
        mov     ah,41h
        int     21h
        mov     dx,OFFSET n_tcom
        mov     ah,41h
        int     21h
        mov     dx,OFFSET n_ucom
        mov     ah,41h
        int     21h

;--- nobody answering: RMDOS's own INT 2Fh says no ---------------------
        push    ds
        lds     dx,dword ptr [old2f]
        mov     ax,252Fh
        int     21h
        pop     ds
        mov     dx,OFFSET t_orph2
        call    say
        call    freejft
        mov     [res2],ax
        mov     dx,OFFSET n_net2
        mov     ax,3D00h
        int     21h
        mov     [res],ax
        pushf
        call    freejft
        mov     [res3],ax
        mov     ax,[res]
        call    put4
        popf
        jnc     nb_bad
        cmp     word ptr [res],1
        jne     nb_bad
        mov     ax,[res2]
        cmp     ax,[res3]
        jne     nb_bad
        call    ok
        jmp     short nb_out
nb_bad:
        call    bad
nb_out:

;--- and the letter back as it was -------------------------------------
        mov     dx,OFFSET t_back
        call    say
        les     di,dword ptr [cdsptr]
        mov     si,OFFSET savecds
        mov     cx,CDS_LEN
        cld
        rep     movsb
        push    cs
        pop     es
        mov     dx,OFFSET n_net2
        mov     ax,3D00h
        int     21h
        mov     [res],ax
        pushf
        call    put4
        popf
        jnc     bk_bad
        cmp     word ptr [res],3        ; a letter with no drive: no path
        jne     bk_bad
        call    ok
        jmp     short bk_out
bk_bad:
        call    bad
bk_out:

        call    disk

        mov     dx,OFFSET m_count
        call    say
        mov     al,[fails]
        call    put2
        call    crlf
        ret

;=====================================================================
; disk - what IFSMGR.386 asks of a hard disk before it takes one for
; 32-bit file access: is it fixed (4408h); has it a parameter block
; (32h), and what driver does the block name; and which BIOS disk is
; it - which it finds out by putting a hook on INT 13h and reading the
; drive's sector 0 through INT 25h into a buffer at offset 0012h,
; taking only an AX=0201h at that very ES:BX as the answer.  And 2Fh
; AH=13h, which Windows' BLOCKDEV.386 uses to put itself in front of
; every disk call the DOS makes.  C: is the drive asked about.
;=====================================================================
disk:
        mov     dx,OFFSET m_disk
        call    say

;--- 4408h and 32h -----------------------------------------------------------
        mov     dx,OFFSET t_dpb
        call    say
        mov     bl,3
        mov     ax,4408h
        int     21h
        mov     [res],ax                ; 1: not removable
        push    ds
        mov     dl,3
        mov     ah,32h
        int     21h
        mov     cx,ds
        pop     ds
        mov     [dpboff],bx
        mov     [dpbseg],cx
        call    put2                    ; AL
        cmp     al,0
        jne     dp_bad
        cmp     word ptr [res],1
        jne     dp_bad
        les     bx,dword ptr [dpboff]
        cmp     byte ptr es:[bx],2      ; drive C
        jne     dp_bad
        cmp     word ptr es:[bx+2],512
        jne     dp_bad
        cmp     byte ptr es:[bx+18h],0  ; read
        jne     dp_bad
        les     bx,dword ptr es:[bx+13h]        ; its driver's header
        test    word ptr es:[bx+4],8000h        ; ...a block device's
        jnz     dp_bad
        mov     dl,' '
        call    putc
        mov     cx,7                    ; and the name in it
dp_name:
        mov     dl,es:[bx+0Bh]
        call    putc
        inc     bx
        loop    dp_name
        push    cs
        pop     es
        call    ok
        jmp     short dp_out
dp_bad:
        push    cs
        pop     es
        call    bad
dp_out:

;--- the chain from 52h reaches it, and C:'s CDS names it ----------------
        mov     dx,OFFSET t_chain
        call    say
        mov     ah,52h
        int     21h
        les     di,dword ptr es:[bx]    ; LoL+0, the first block
        mov     cx,26
        mov     byte ptr [res],0
ch_walk:
        cmp     di,0FFFFh
        je      ch_end
        cmp     di,[dpboff]
        jne     ch_next
        mov     ax,es
        cmp     ax,[dpbseg]
        jne     ch_next
        mov     byte ptr [res],1
ch_next:
        les     di,dword ptr es:[di+19h]
        loop    ch_walk
        jmp     short ch_bad            ; no end to it
ch_end:
        push    cs
        pop     es
        cmp     byte ptr [res],1
        jne     ch_bad
        les     di,dword ptr [cdsoff]   ; C:'s CDS, entry 2
        add     di,2 * CDS_LEN
        mov     ax,word ptr es:[di+45h]
        mov     dx,word ptr es:[di+47h]
        push    cs
        pop     es
        cmp     ax,[dpboff]
        jne     ch_bad
        cmp     dx,[dpbseg]
        jne     ch_bad
        call    ok
        jmp     short ch_out
ch_bad:
        push    cs
        pop     es
        call    bad
ch_out:

;--- INT 25h, through a hook on INT 13h ---------------------------------
        mov     dx,OFFSET t_probe
        call    say
        mov     bx,40h                  ; somewhere to read into, at 0012h
        mov     ah,48h
        int     21h
        jnc     pr_mem
        call    bad                     ; no memory: neither read can be made
        mov     dx,OFFSET t_d13
        call    say
        jmp     bad
pr_mem:
        mov     [bufseg],ax
        mov     word ptr [p13n],0
        call    hook13
        push    ds
        mov     ds,[bufseg]
        mov     bx,0012h
        mov     al,2                    ; C:
        mov     cx,1
        xor     dx,dx
        int     25h
        pop     dx                      ; the flags INT 25h leaves
        pop     ds
        call    unhook13
        mov     ax,[p13ax]
        call    put4
        mov     dl,' '
        call    putc
        mov     ax,[p13es]
        cmp     ax,[bufseg]
        jne     pr_es
        mov     dx,OFFSET m_hisseg      ; the segment is the buffer's, which
        call    say                     ; moves with memory: printed as such
        jmp     short pr_bx
pr_es:
        call    put4
pr_bx:
        mov     dl,':'
        call    putc
        mov     ax,[p13bx]
        call    put4
        mov     dl,' '
        call    putc
        mov     al,byte ptr [p13dx]
        call    put2
        cmp     word ptr [p13ax],0201h
        jne     pr_bad
        mov     ax,[p13es]
        cmp     ax,[bufseg]
        jne     pr_bad
        cmp     word ptr [p13bx],0012h
        jne     pr_bad
        cmp     byte ptr [p13dx],80h
        jne     pr_bad
        call    ok
        jmp     short pr_out
pr_bad:
        call    bad
pr_out:

;--- 2Fh AH=13h: a handler of our own as Orig13 sees the next disk call -
        mov     dx,OFFSET t_d13
        call    say
        mov     word ptr [o13n],0
        push    ds
        push    es
        mov     dx,OFFSET o13
        mov     bx,OFFSET o13
        push    cs
        pop     es
        mov     ah,13h
        int     2Fh                     ; -> the old pair
        mov     word ptr cs:[o13old],dx
        mov     word ptr cs:[o13old+2],ds
        mov     word ptr cs:[o13boot],bx
        mov     word ptr cs:[o13boot+2],es
        pop     es
        pop     ds
        push    ds
        mov     ds,[bufseg]
        mov     bx,0012h
        mov     al,2
        mov     cx,1
        xor     dx,dx
        int     25h
        pop     dx
        pop     ds
        push    ds
        push    es
        lds     dx,dword ptr cs:[o13old]
        les     bx,dword ptr cs:[o13boot]
        mov     ah,13h
        int     2Fh                     ; put back -> ours again
        mov     ax,ds
        pop     es
        pop     ds
        mov     [res],dx
        mov     [res2],ax
        mov     ax,[o13n]
        call    put4
        mov     ax,word ptr [o13old+2]
        mov     dx,cs
        cmp     ax,dx                   ; the old one was not ours...
        je      d13_bad
        cmp     word ptr [o13n],0       ; ...ours saw the read...
        je      d13_bad
        cmp     word ptr [res],OFFSET o13       ; ...and came back out
        jne     d13_bad
        mov     ax,cs
        cmp     [res2],ax
        jne     d13_bad
        call    ok
        jmp     short d13_out
d13_bad:
        call    bad
d13_out:
        mov     es,[bufseg]
        mov     ah,49h
        int     21h
        push    cs
        pop     es
        ret

;--- hook13 / unhook13 - this program's own INT 13h in the table, as
;--- IFSMGR puts one there: it records the first AH=02h it sees and goes
;--- on to whatever was there before.  Not through 25h: IFSMGR does not.
hook13:
        push    es
        xor     ax,ax
        mov     es,ax
        cli
        mov     ax,word ptr es:[13h*4]
        mov     word ptr [prev13],ax
        mov     ax,word ptr es:[13h*4+2]
        mov     word ptr [prev13+2],ax
        mov     word ptr es:[13h*4],OFFSET p13
        mov     word ptr es:[13h*4+2],cs
        sti
        pop     es
        ret
unhook13:
        push    es
        push    ax
        xor     ax,ax
        mov     es,ax
        cli
        mov     ax,word ptr [prev13]
        mov     word ptr es:[13h*4],ax
        mov     ax,word ptr [prev13+2]
        mov     word ptr es:[13h*4+2],ax
        sti
        pop     ax
        pop     es
        ret

p13:
        cmp     ah,02h
        jne     p13_on
        cmp     word ptr cs:[p13n],0
        jne     p13_on
        inc     word ptr cs:[p13n]
        mov     word ptr cs:[p13ax],ax
        mov     word ptr cs:[p13bx],bx
        mov     word ptr cs:[p13dx],dx
        push    ax
        mov     ax,es
        mov     word ptr cs:[p13es],ax
        pop     ax
p13_on:
        jmp     dword ptr cs:[prev13]

;--- o13 - the handler handed to 2Fh AH=13h: counts, and goes on to the
;--- one it replaced
o13:
        inc     word ptr cs:[o13n]
        jmp     dword ptr cs:[o13old]

;=====================================================================
; The steps
;=====================================================================
;--- step - DX = what is being done, SI = the calls expected (FFh ends
;--- them).  Prints the first, marks where the record is.  stepq marks
;--- without printing, for a step whose title is already out.
step:
        call    say
stepq:
        mov     [expect],si
        mov     al,[logn]
        mov     [mark],al
        mov     word ptr [anom],0
        ret

;--- endstep - the calls since the mark, printed; CF if they are not the
;--- ones expected or the handler found anything wrong.  Every register
;--- but the flags is kept.
endstep:
        pusha
        mov     dx,OFFSET m_calls
        call    say
        movzx   bx,byte ptr [mark]
        mov     si,[expect]
        mov     byte ptr [same],1
es_one:
        cmp     bl,[logn]
        jae     es_end
        mov     al,[logbuf + bx]
        call    put2
        mov     dl,' '
        call    putc
        cmp     al,[si]
        je      es_match
        mov     byte ptr [same],0
        jmp     short es_next
es_match:
        inc     si
es_next:
        inc     bx
        jmp     short es_one
es_end:
        cmp     byte ptr [si],0FFh      ; every expected call was seen
        je      es_all
        mov     byte ptr [same],0
es_all:
        cmp     word ptr [anom],0
        je      es_noanom
        mov     dx,OFFSET m_anom
        call    say
        mov     ax,[anom]
        call    put4
        mov     dl,' '
        call    putc
        mov     byte ptr [same],0
es_noanom:
        cmp     byte ptr [same],1
        popa
        je      es_good
        stc
        ret
es_good:
        clc
        ret

okifnc:
        jnc     ok
        jmp     bad

;--- putfile - SI = a name, DI = bytes, CX = how many: a file on the
;--- drive, through 3Ch/40h/3Eh.  DX is kept.  CF if it did not happen.
putfile:
        push    dx
        push    cx
        mov     dx,si
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        pop     cx
        jc      pf_out
        mov     bx,ax
        mov     dx,di
        mov     ah,40h
        int     21h
        pushf
        mov     ah,3Eh
        int     21h
        popf
pf_out:
        pop     dx
        call    say                     ; the step's own title, now
        ret

;--- run - DX = a program: 4B00h, then 4Dh -> AX, FFFFh if EXEC failed
run:
        mov     word ptr [epb],0        ; the environment: ours
        mov     word ptr [epb+2],OFFSET n_tail
        mov     word ptr [epb+4],cs
        mov     word ptr [epb+6],5Ch
        mov     word ptr [epb+8],cs
        mov     word ptr [epb+10],6Ch
        mov     word ptr [epb+12],cs
        mov     bx,OFFSET epb
        push    cs
        pop     es
        mov     ax,4B00h
        int     21h
        jc      rn_fail
        mov     ah,4Dh
        int     21h
        xor     ah,ah
        ret
rn_fail:
        mov     ax,0FFFFh
        ret

;--- freejft - AX = how many of this process's handles are free
freejft:
        push    bx
        push    cx
        push    di
        push    es
        mov     ah,62h
        int     21h
        mov     es,bx
        mov     cx,es:[32h]
        les     di,dword ptr es:[34h]
        xor     ax,ax
fj_one:
        cmp     byte ptr es:[di],0FFh
        jne     fj_next
        inc     ax
fj_next:
        inc     di
        loop    fj_one
        pop     es
        pop     di
        pop     cx
        pop     bx
        ret

;--- pickdrv - the highest letter under LASTDRIVE with nothing behind
;--- it -> [drv], [letter], [cdsptr]; ES:DI its structure.  CF if none.
pickdrv:
        mov     ah,52h
        int     21h
        mov     ax,word ptr es:[bx+16h]
        mov     [cdsoff],ax
        mov     ax,word ptr es:[bx+18h]
        mov     [cdsseg],ax
        movzx   cx,byte ptr es:[bx+21h]
pd_try:
        dec     cx
        cmp     cx,2
        jb      pd_none
        mov     ax,CDS_LEN
        mul     cx
        add     ax,[cdsoff]
        mov     di,ax
        mov     es,[cdsseg]
        test    word ptr es:[di+CDS_FLAGS],0F000h
        jnz     pd_try
        mov     [drv],cl
        mov     al,'A'
        add     al,cl
        mov     [letter],al
        mov     word ptr [cdsptr],di
        mov     word ptr [cdsptr+2],es
        clc
        ret
pd_none:
        push    cs
        pop     es
        stc
        ret

;--- strsame - DS:SI and ES:DI (both ours) the same ASCIIZ?  ZF if so
strsame:
        push    si
        push    di
        push    es
        push    cs
        pop     es
ss_one:
        mov     al,[si]
        cmp     al,es:[di]
        jne     ss_out
        inc     si
        inc     di
        or      al,al
        jnz     ss_one
ss_out:
        pop     es
        pop     di
        pop     si
        ret

;=====================================================================
; The redirector
;
; Entered with the caller's registers and, above the interrupt's three
; words, the word DOS pushed.  Everything it is given is kept in r_*,
; the work is done with DS = this segment, and r_* goes back - with the
; carry in the flags the INT pushed if r_err is not 0.
;=====================================================================
nt_i2f:
        cmp     ah,11h
        je      ni_ours
        jmp     dword ptr cs:[old2f]
ni_ours:
        or      al,al
        jne     ni_call
        mov     al,0FFh                 ; installed
        iret
ni_call:
        push    bp
        mov     bp,sp
        mov     cs:[r_ax],ax
        mov     cs:[r_bx],bx
        mov     cs:[r_cx],cx
        mov     cs:[r_dx],dx
        mov     cs:[r_si],si
        mov     cs:[r_di],di
        mov     cs:[r_es],es
        mov     cs:[r_ds],ds
        mov     ax,[bp+8]               ; the word DOS pushed
        mov     cs:[r_parm],ax
        push    cs
        pop     ds
        push    cs
        pop     es
        mov     word ptr [r_err],0
        cld
        call    nt_do
        mov     ax,[r_err]
        or      ax,ax
        jz      ni_good
        mov     [r_ax],ax
        or      byte ptr [bp+6],01h     ; the carry, in the pushed flags
        jmp     short ni_back
ni_good:
        and     byte ptr [bp+6],0FEh
ni_back:
        mov     ax,cs:[r_ax]
        mov     bx,cs:[r_bx]
        mov     cx,cs:[r_cx]
        mov     dx,cs:[r_dx]
        mov     si,cs:[r_si]
        mov     di,cs:[r_di]
        mov     es,cs:[r_es]
        mov     ds,cs:[r_ds]
        pop     bp
        iret

;--- nt_do - the call in r_*.  Records it, checks what every call is
;--- given, and does it.
nt_do:
        mov     al,byte ptr [r_ax]
        movzx   bx,byte ptr [logn]
        cmp     bx,LOGMAX
        jae     nd_full
        mov     [logbuf + bx],al
        inc     byte ptr [logn]
nd_full:
        mov     fs,[sdaseg]
        mov     si,[sdaoff]
        mov     ax,word ptr fs:[si+S_PSP]
        cmp     ax,word ptr fs:[si+S_PROCID]
        je      nd_proc
        or      word ptr [anom],A_PROC
nd_proc:
        mov     al,byte ptr fs:[si+S_USERAX+1]
        mov     [lusrah],al
        mov     ax,[r_parm]
        mov     [lparm],ax
        mov     al,byte ptr fs:[si+S_SATTR]
        mov     [lsattr],al

        mov     al,byte ptr [r_ax]
        mov     di,OFFSET nt_tab
nd_find:
        cmp     byte ptr [di],0
        je      nd_unknown
        cmp     al,[di]
        je      nd_have
        add     di,4
        jmp     short nd_find
nd_have:
        test    byte ptr [di+1],1       ; a call about a name: its checks
        jz      nd_notpath
        call    nt_pathchk
nd_notpath:
        test    byte ptr [di+1],2       ; a call about an entry: its checks
        jz      nd_notsft
        call    nt_sftchk
nd_notsft:
        jmp     word ptr [di+2]
nd_unknown:
        or      word ptr [anom],A_UNK
        mov     word ptr [r_err],1
        ret

;--- the calls: function, what to check (1 a name, 2 an entry), where
nt_tab:
        db      01h, 1
        dw      OFFSET nt_rmdir
        db      03h, 1
        dw      OFFSET nt_mkdir
        db      05h, 1
        dw      OFFSET nt_chdir
        db      06h, 2
        dw      OFFSET nt_close
        db      08h, 2
        dw      OFFSET nt_read
        db      09h, 2
        dw      OFFSET nt_write
        db      0Ch, 0
        dw      OFFSET nt_dinfo
        db      0Eh, 1
        dw      OFFSET nt_setattr
        db      0Fh, 1
        dw      OFFSET nt_getattr
        db      11h, 1
        dw      OFFSET nt_rename
        db      13h, 1
        dw      OFFSET nt_delete
        db      16h, 1
        dw      OFFSET nt_open
        db      17h, 1
        dw      OFFSET nt_create
        db      1Bh, 1
        dw      OFFSET nt_first
        db      1Ch, 0
        dw      OFFSET nt_next
        db      1Dh, 0
        dw      OFFSET nt_nothing
        db      21h, 2
        dw      OFFSET nt_seekend
        db      22h, 0
        dw      OFFSET nt_nothing
        db      0

;--- nt_pathchk - WFP_START is FN1, THISCDS is the drive's structure,
;--- and ES:DI is that (or, for 16h and 17h, THISSFT).  FN1 into lfn1.
nt_pathchk:
        push    di
        mov     si,[sdaoff]
        lea     ax,[si+S_FN1]
        cmp     ax,word ptr fs:[si+S_WFP]
        je      npc_wfp
        or      word ptr [anom],A_WFP
npc_wfp:
        mov     ax,word ptr fs:[si+S_THISCDS]
        cmp     ax,word ptr [cdsptr]
        jne     npc_cdsbad
        mov     ax,word ptr fs:[si+S_THISCDS+2]
        cmp     ax,word ptr [cdsptr+2]
        je      npc_cds
npc_cdsbad:
        or      word ptr [anom],A_CDS
npc_cds:
        mov     bx,S_THISCDS
        mov     al,byte ptr [r_ax]
        cmp     al,16h
        je      npc_sft
        cmp     al,17h
        jne     npc_esdi
npc_sft:
        mov     bx,S_THISSFT
npc_esdi:
        add     bx,si
        mov     ax,word ptr fs:[bx]
        cmp     ax,[r_di]
        jne     npc_esdibad
        mov     ax,word ptr fs:[bx+2]
        cmp     ax,[r_es]
        je      npc_copy
npc_esdibad:
        or      word ptr [anom],A_ESDI
npc_copy:
        mov     bx,word ptr fs:[si+S_WFP]
        mov     di,OFFSET lfn1
        call    nt_fscopy
        pop     di
        ret

;--- nt_fscopy - FS:BX's ASCIIZ into DS:DI, at most 79 characters
nt_fscopy:
        push    cx
        mov     cx,79
nfc_one:
        mov     al,fs:[bx]
        mov     [di],al
        or      al,al
        jz      nfc_out
        inc     bx
        inc     di
        loop    nfc_one
        mov     byte ptr [di],0
nfc_out:
        pop     cx
        ret

;--- nt_sftchk - ES:DI is THISSFT, and the entry is one of ours
nt_sftchk:
        mov     si,[sdaoff]
        mov     ax,word ptr fs:[si+S_THISSFT]
        cmp     ax,[r_di]
        jne     nsc_bad
        mov     ax,word ptr fs:[si+S_THISSFT+2]
        cmp     ax,[r_es]
        je      nsc_esdi
nsc_bad:
        or      word ptr [anom],A_ESDI
nsc_esdi:
        mov     gs,[r_es]
        mov     bx,[r_di]
        mov     ax,word ptr gs:[bx+SF_DEVPTR+2]
        mov     dx,cs
        cmp     ax,dx
        je      nsc_out
        or      word ptr [anom],A_SIG
nsc_out:
        ret

;--- nt_entry - lfn1's file -> BX, CF if there is none
nt_entry:
        mov     bx,OFFSET files
        mov     cx,N_FILES
ne_one:
        mov     si,OFFSET lfn1
        lea     di,[bx+E_PATH]
        call    strsame
        je      ne_got
        add     bx,E_LEN
        loop    ne_one
        stc
        ret
ne_got:
        clc
        ret

;--- nt_free - an unused file -> BX, CF if none
nt_free:
        mov     bx,OFFSET files
        mov     cx,N_FILES
nf_one:
        cmp     byte ptr [bx+E_PATH],0
        je      nf_got
        add     bx,E_LEN
        loop    nf_one
        stc
        ret
nf_got:
        clc
        ret

;--- nt_setpath - lfn1 into BX's path
nt_setpath:
        push    si
        push    di
        mov     si,OFFSET lfn1
        lea     di,[bx+E_PATH]
        mov     cx,47
nsp_one:
        lodsb
        stosb
        or      al,al
        jz      nsp_out
        loop    nsp_one
        mov     byte ptr [di],0
nsp_out:
        pop     di
        pop     si
        ret

;--- the calls ------------------------------------------------------------
nt_nothing:
        ret

nt_mkdir:
        call    nt_entry
        jnc     nt_denied
        call    nt_free
        jc      nt_denied
        call    nt_setpath
        mov     byte ptr [bx+E_ATTR],10h
        mov     word ptr [bx+E_SIZE],0
        ret

nt_rmdir:
        call    nt_entry
        jc      nt_nopath
        test    byte ptr [bx+E_ATTR],10h
        jz      nt_nopath
        mov     byte ptr [bx+E_PATH],0  ; empty or not, as this test uses it
        ret

nt_chdir:
        mov     si,OFFSET lfn1
        cmp     byte ptr [si+3],0       ; "X:\" is always there
        je      ncd_ok
        call    nt_entry
        jc      nt_nopath
        test    byte ptr [bx+E_ATTR],10h
        jz      nt_nopath
ncd_ok:
        ret

nt_delete:
        call    nt_entry
        jc      nt_nofile
        test    byte ptr [bx+E_ATTR],10h
        jnz     nt_nofile
        mov     byte ptr [bx+E_PATH],0
        ret

nt_getattr:
        call    nt_entry
        jc      nt_nofile
        movzx   ax,byte ptr [bx+E_ATTR]
        mov     [r_ax],ax
        mov     ax,[bx+E_TIME]
        mov     [r_cx],ax
        mov     ax,[bx+E_DATE]
        mov     [r_dx],ax
        mov     ax,[bx+E_SIZE]
        mov     [r_di],ax
        mov     word ptr [r_bx],0
        ret

nt_setattr:
        call    nt_entry
        jc      nt_nofile
        mov     al,byte ptr [r_parm]
        and     al,27h
        and     byte ptr [bx+E_ATTR],10h
        or      [bx+E_ATTR],al
        ret

nt_rename:
        mov     si,[sdaoff]
        lea     ax,[si+S_FN2]
        cmp     ax,word ptr fs:[si+S_RENWFP]
        je      nrn_wfp
        or      word ptr [anom],A_REN
nrn_wfp:
        push    bx
        mov     bx,word ptr fs:[si+S_RENWFP]
        mov     di,OFFSET lfn2
        call    nt_fscopy
        pop     bx
        call    nt_entry
        jc      nt_nofile
        push    bx
        mov     si,OFFSET lfn2
        mov     di,OFFSET lfn1
        mov     cx,80
        rep     movsb
        pop     bx
        call    nt_setpath
        ret

nt_dinfo:
        mov     word ptr [r_ax],0F804h  ; 4 a cluster, media F8h
        mov     word ptr [r_bx],1000    ; clusters
        mov     word ptr [r_cx],512
        mov     word ptr [r_dx],500     ; free
        ret

nt_denied:
        mov     word ptr [r_err],5
        ret
nt_nofile:
        mov     word ptr [r_err],2
        ret
nt_nopath:
        mov     word ptr [r_err],3
        ret

;--- 16h and 17h: the entry DOS made, checked and filled in -------------
nt_open:
        call    nt_newsft
        call    nt_entry
        jc      nt_nofile
        test    byte ptr [bx+E_ATTR],10h
        jnz     nt_denied
        jmp     short nt_fillsft

nt_create:
        call    nt_newsft
        call    nt_entry
        jc      ncr_new
        cmp     byte ptr [r_parm+1],1   ; AH=1: refuse what is there
        je      ncr_exists
        mov     word ptr [bx+E_SIZE],0
        jmp     short nt_fillsft
ncr_exists:
        mov     word ptr [r_err],50h
        ret
ncr_new:
        call    nt_free
        jc      nt_denied
        call    nt_setpath
        mov     al,byte ptr [r_parm]
        mov     [bx+E_ATTR],al
        mov     word ptr [bx+E_SIZE],0
        mov     word ptr [bx+E_TIME],FTIME
        mov     word ptr [bx+E_DATE],FDATE
        ; fall into nt_fillsft

;--- nt_fillsft - BX = the file: ES:DI's entry, as a redirector fills it
nt_fillsft:
        mov     gs,[r_es]
        mov     di,[r_di]
        mov     al,[bx+E_ATTR]
        mov     byte ptr gs:[di+SF_ATTR],al
        mov     al,[drv]
        or      al,40h                  ; clean
        mov     byte ptr gs:[di+SF_FLAGS],al
        mov     byte ptr gs:[di+SF_FLAGS+1],80h ; a network file
        mov     word ptr gs:[di+SF_DEVPTR],bx   ; this redirector's file
        mov     word ptr gs:[di+SF_DEVPTR+2],cs
        mov     ax,[bx+E_TIME]
        mov     word ptr gs:[di+SF_TIME],ax
        mov     ax,[bx+E_DATE]
        mov     word ptr gs:[di+SF_DATE],ax
        movzx   eax,word ptr [bx+E_SIZE]
        mov     dword ptr gs:[di+SF_SIZE],eax
        mov     dword ptr gs:[di+SF_POS],0
        ret

;--- nt_newsft - the entry DOS hands an open or a create: busy, and the
;--- handle the SDA names pointing at it with the number the SDA gives
nt_newsft:
        push    bx
        mov     gs,[r_es]
        mov     bx,[r_di]
        cmp     word ptr gs:[bx+SF_REF],0FFFFh
        je      nns_busy
        or      word ptr [anom],A_BUSY
nns_busy:
        mov     si,[sdaoff]
        les     bx,dword ptr fs:[si+S_PJFN]
        mov     al,es:[bx]
        push    cs
        pop     es
        cmp     al,byte ptr fs:[si+S_SFN]
        je      nns_out
        or      word ptr [anom],A_JFT
nns_out:
        pop     bx
        ret

;--- 06h, 08h, 09h, 21h: the entry is ours (nt_sftchk) ------------------
nt_close:
        mov     gs,[r_es]
        mov     di,[r_di]
        cmp     word ptr gs:[di+SF_REF],0
        je      ncl_out
        dec     word ptr gs:[di+SF_REF]
ncl_out:
        ret

;--- nt_xfer - the DTA the SDA gives, checked: -> GS:BX the buffer
nt_xfer:
        mov     si,[sdaoff]
        mov     bx,word ptr fs:[si+S_DTA]
        mov     gs,word ptr fs:[si+S_DTA+2]
        cmp     bx,16
        jb      nx_out
        or      word ptr [anom],A_ALIGN
nx_out:
        ret

nt_read:
        call    nt_xfer                 ; GS:BX the buffer
        push    gs
        push    bx
        mov     gs,[r_es]
        mov     di,[r_di]
        mov     si,word ptr gs:[di+SF_DEVPTR]   ; our file
        mov     ax,[si+E_SIZE]
        sub     ax,word ptr gs:[di+SF_POS]
        jae     nrd_left
        xor     ax,ax
nrd_left:
        mov     cx,[r_cx]
        cmp     cx,ax
        jbe     nrd_n
        mov     cx,ax
nrd_n:
        mov     [r_cx],cx
        mov     ax,word ptr gs:[di+SF_POS]
        add     word ptr gs:[di+SF_POS],cx
        pop     bx
        pop     es
        lea     si,[si+E_DATA]
        add     si,ax
        mov     di,bx
        rep     movsb                   ; DS:SI ours -> the caller's
        push    cs
        pop     es
        ret

nt_write:
        call    nt_xfer
        mov     es,[r_es]
        mov     di,[r_di]
        mov     si,word ptr es:[di+SF_DEVPTR]
        mov     ax,word ptr es:[di+SF_POS]
        mov     cx,[r_cx]
        jcxz    nwr_trunc
        mov     dx,E_MAX
        sub     dx,ax
        cmp     cx,dx
        jbe     nwr_n
        mov     cx,dx
nwr_n:
        mov     [r_cx],cx
        add     word ptr es:[di+SF_POS],cx
        mov     dx,word ptr es:[di+SF_POS]
        cmp     dx,[si+E_SIZE]
        jbe     nwr_size
        mov     [si+E_SIZE],dx
nwr_size:
        movzx   edx,word ptr [si+E_SIZE]
        mov     dword ptr es:[di+SF_SIZE],edx
        lea     di,[si+E_DATA]
        add     di,ax
        push    cs
        pop     es
        push    ds
        push    gs
        pop     ds
        mov     si,bx
        rep     movsb                   ; the caller's -> ours
        pop     ds
        ret
nwr_trunc:
        mov     [si+E_SIZE],ax          ; a write of nothing cuts it here
        movzx   eax,ax
        mov     dword ptr es:[di+SF_SIZE],eax
        push    cs
        pop     es
        ret

nt_seekend:
        mov     es,[r_es]
        mov     di,[r_di]
        mov     si,word ptr es:[di+SF_DEVPTR]
        push    cs
        pop     es
        mov     ax,[r_dx]
        mov     dx,[r_cx]
        add     ax,[si+E_SIZE]
        adc     dx,0
        mov     [r_ax],ax
        mov     [r_dx],dx
        ret

;--- 1Bh and 1Ch: the search's state is ours, in the 21 bytes ----------
;--- byte 0 = 80h plus the drive, 1-11 the template, 12 the attribute,
;--- 13 the next file to look at.  The directory is kept here (srchdir),
;--- since one search at a time is all this test makes.
nt_first:
        mov     si,[sdaoff]
        lea     ax,[si+S_SRCH]
        cmp     ax,word ptr fs:[si+S_DTA]
        jne     nfi_dta
        mov     ax,fs
        cmp     ax,word ptr fs:[si+S_DTA+2]
        je      nfi_dtaok
nfi_dta:
        or      word ptr [anom],A_DTA
nfi_dtaok:
        ; the directory part, "X:\" up to the last backslash, and the
        ; template out of what follows it
        mov     si,OFFSET lfn1
        mov     di,OFFSET srchdir
        mov     bx,di
nfi_copy:
        lodsb
        stosb
        cmp     al,'\'
        jne     nfi_nosep
        mov     bx,di                   ; just past the last one
nfi_nosep:
        or      al,al
        jnz     nfi_copy
        mov     si,bx
        sub     si,OFFSET srchdir
        add     si,OFFSET lfn1          ; the last component, in lfn1
        mov     byte ptr [bx],0
        mov     di,OFFSET srchtpl
        call    nt_fcbname
        mov     byte ptr [srchidx],0
        jmp     short nt_scan

nt_next:
        ; the state comes back in SEARCHBUF; the DTA names it
        mov     si,[sdaoff]
        les     bx,dword ptr fs:[si+S_DTA]
        mov     al,es:[bx+13]
        push    cs
        pop     es
        mov     [srchidx],al

nt_scan:
        movzx   cx,byte ptr [srchidx]
nsn_try:
        cmp     cx,N_FILES
        jae     nsn_none
        mov     ax,E_LEN
        mul     cx
        mov     bx,ax
        add     bx,OFFSET files
        inc     cx
        cmp     byte ptr [bx+E_PATH],0
        je      nsn_try
        ; in the directory?
        mov     si,OFFSET srchdir
        lea     di,[bx+E_PATH]
nsn_dir:
        mov     al,[si]
        or      al,al
        jz      nsn_indir
        cmp     al,[di]
        jne     nsn_try
        inc     si
        inc     di
        jmp     short nsn_dir
nsn_indir:
        ; and nothing deeper: no backslash in the rest
        push    di
nsn_rest:
        mov     al,[di]
        or      al,al
        jz      nsn_flat
        cmp     al,'\'
        je      nsn_deep
        inc     di
        jmp     short nsn_rest
nsn_deep:
        pop     di
        jmp     short nsn_try
nsn_flat:
        pop     si                      ; the name, after the directory
        push    bx
        push    cx
        mov     di,OFFSET srchname
        call    nt_fcbname
        pop     cx
        pop     bx
        ; against the template
        mov     si,OFFSET srchtpl
        mov     di,OFFSET srchname
        push    cx
        mov     cx,11
nsn_cmp:
        mov     al,[si]
        cmp     al,'?'
        je      nsn_cnext
        cmp     al,[di]
        jne     nsn_cfail
nsn_cnext:
        inc     si
        inc     di
        loop    nsn_cmp
        pop     cx
        ; and the attribute: a directory, hidden or system file only if
        ; the search asked for it
        mov     si,[sdaoff]
        mov     al,[bx+E_ATTR]
        mov     ah,byte ptr fs:[si+S_SATTR]
        not     ah
        and     al,16h
        and     al,ah
        jnz     nsn_try
        jmp     short nsn_found
nsn_cfail:
        pop     cx
        jmp     nsn_try
nsn_none:
        mov     word ptr [r_err],12h
        ret
nsn_found:
        ; the 21 bytes and the entry, at the DTA (which is SEARCHBUF)
        mov     [srchidx],cl
        mov     si,[sdaoff]
        les     di,dword ptr fs:[si+S_DTA]
        mov     al,[drv]
        or      al,80h
        stosb
        mov     si,OFFSET srchtpl
        push    cx
        mov     cx,11
        rep     movsb
        pop     cx
        push    si
        mov     si,[sdaoff]
        mov     al,byte ptr fs:[si+S_SATTR]
        pop     si
        stosb                           ; 12
        mov     al,cl
        stosb                           ; 13: where to go on from
        xor     al,al
        mov     cx,7
        rep     stosb                   ; to 21
        mov     si,OFFSET srchname      ; the entry: name...
        mov     cx,11
        rep     movsb
        mov     al,[bx+E_ATTR]          ; ...attribute
        stosb
        xor     al,al
        mov     cx,10
        rep     stosb
        mov     ax,[bx+E_TIME]          ; 16h
        stosw
        mov     ax,[bx+E_DATE]          ; 18h
        stosw
        xor     ax,ax                   ; 1Ah, no cluster
        stosw
        movzx   eax,word ptr [bx+E_SIZE]
        stosd                           ; 1Ch
        push    cs
        pop     es
        ret

;--- nt_fcbname - DS:SI "NAME.EXT" (to a NUL or a backslash) -> eleven
;--- bytes at DS:DI, blank padded
nt_fcbname:
        push    di
        mov     cx,11
        mov     al,' '
        rep     stosb
        pop     di
        mov     cx,8
nfn_base:
        mov     al,[si]
        or      al,al
        jz      nfn_out
        cmp     al,'\'
        je      nfn_out
        inc     si
        cmp     al,'.'
        je      nfn_ext
        jcxz    nfn_base
        mov     [di],al
        inc     di
        dec     cx
        jmp     short nfn_base
nfn_ext:
        add     di,cx                   ; to the extension's three
        mov     cx,3
nfn_eone:
        mov     al,[si]
        or      al,al
        jz      nfn_out
        cmp     al,'\'
        je      nfn_out
        inc     si
        jcxz    nfn_eone
        mov     [di],al
        inc     di
        dec     cx
        jmp     short nfn_eone
nfn_out:
        ret

;=====================================================================
; helpers
;=====================================================================
;--- ownpath - DS:DX = this program's path, out of the environment
ownpath:
        push    es
        mov     es,word ptr ds:[2Ch]
        xor     di,di
        mov     cx,8000h
        xor     al,al
        cld
op_var:
        cmp     byte ptr es:[di],0
        je      op_end
        repne   scasb
        jmp     short op_var
op_end:
        inc     di
        cmp     word ptr es:[di],1
        jb      op_none
        add     di,2
        mov     si,OFFSET pathbuf
op_copy:
        mov     al,es:[di]
        mov     [si],al
        inc     di
        inc     si
        or      al,al
        jnz     op_copy
        pop     es
        mov     dx,OFFSET pathbuf
        clc
        ret
op_none:
        pop     es
        stc
        ret

okif:
        je      ok
bad:
        push    dx
        mov     dx,OFFSET m_bad
        call    say
        pop     dx
        inc     byte ptr [fails]
        ret
ok:
        push    dx
        mov     dx,OFFSET m_ok
        call    say
        pop     dx
        ret

say:
        push    ax
        push    bx
        push    cx
        push    di
        push    es
        push    ds
        pop     es
        mov     di,dx
        mov     al,'$'
        mov     cx,0FFFFh
        cld
        repne   scasb
        mov     cx,di
        sub     cx,dx
        dec     cx
        mov     bx,[outh]
        mov     ah,40h
        int     21h
        pop     es
        pop     di
        pop     cx
        pop     bx
        pop     ax
        ret
putc:
        push    ax
        push    bx
        push    cx
        push    dx
        push    ds
        push    cs
        pop     ds
        mov     [onech],dl
        mov     dx,OFFSET onech
        mov     cx,1
        mov     bx,[outh]
        mov     ah,40h
        int     21h
        pop     ds
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret
crlf:
        push    dx
        mov     dx,OFFSET m_crlf
        call    say
        pop     dx
        ret
put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:
        push    ax
        shr     al,4
        call    put1
        pop     ax
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1:
        push    dx
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1a
        add     al,7
p1a:
        mov     dl,al
        call    putc
        pop     dx
        ret

;=====================================================================
m_hi        db  'NETTEST - a redirector of its own, on the DOS under Windows',13,10,'$'
m_park      db  'parking the kernel, to make the calls there...',13,10,'$'
m_rmhi      db  'on RMDOS:',13,10,'$'
m_bye       db  'NETTEST done',13,10,'$'
m_ok        db  ' ok',13,10,'$'
m_bad       db  ' FAILED',13,10,'$'
m_crlf      db  13,10,'$'
m_colon     db  ':, taken$'
m_none      db  'none free$'
m_calls     db  ' calls $'
m_anom      db  ' wrong $'
m_count     db  '  checks failed here: $'
m_fails     db  'checks failed: $'
m_allok     db  'all checks passed.',13,10,'$'
m_rmexit    db  'the far side ended with $'
m_nopath    db  'FAILED - no path in the environment to park with',13,10,'$'
m_nopark    db  'FAILED - the park was refused, error $'
m_norep     db  'FAILED - the far side left no report',13,10,'$'

t_sda       db  '  5D06h, the SDA at $'
t_take      db  '  the highest letter with no drive: $'
t_inst0     db  '  1100h with no redirector: AL=$'
t_inst1     db  '  1100h with this one hooked: AL=$'
t_create    db  '  3Ch create a file:$'
t_write     db  '  40h eleven bytes into it:$'
t_seekend   db  '  4202h, compatibility mode, from the entry:$'
t_close     db  '  3Eh:$'
t_open      db  '  3Dh open it to read, deny none:$'
t_lseek     db  '  4202h, deny none, from the redirector:$'
t_read      db  '  3Fh read it into an odd offset:$'
t_times     db  '  5700h, the date it was given:$'
t_info      db  '  4400h and 440Ah, bit 15:$'
t_dup       db  '  45h, and both closed:$'
t_new       db  '  5Bh on a name that is there:$'
t_remote    db  '  4409h on the drive: $'
t_attr      db  '  4301h and 4300h:$'
t_find      db  '  4Eh and 4Fh, *.*:$'
t_ren       db  '  56h to a new name:$'
t_dirs      db  '  39h, 3Bh, a relative 3Ch, 47h, 41h, 3Bh \, 3Ah:$'
t_space     db  '  36h and 1Ch:$'
t_true      db  '  60h:$'
t_wild      db  '  3Dh with a wildcard, refused before it is asked:$'
t_exec      db  '  4B00h, a program on the drive:$'
t_orphan    db  '  4B00h, a child that ends with a file there open:$'
t_orph2     db  '  with nobody on INT 2Fh, 3Dh fails and gives the handle back: $'
t_back      db  '  the letter put back, 3Dh on it: $'
m_disk      db  'and what IFSMGR asks of a hard disk before it takes one:',13,10,'$'
t_dpb       db  '  4408h and 32h on C:, its block and the block',39,'s driver: AL=$'
t_chain     db  '  the chain from LoL+0 reaches it, and C:',39,'s CDS names it:$'
t_probe     db  '  INT 25h sector 0 into xxxx:0012h, seen on INT 13h: $'
t_d13       db  '  2Fh AH=13h, and a handler of ours sees the next read: $'
m_hisseg    db  'buffer$'

; every name below with its drive letter at its first byte, patched
n_first     dw  OFFSET n_net1, OFFSET n_net2, OFFSET n_all, OFFSET n_allc
            dw  OFFSET n_sub, OFFSET n_relfull, OFFSET n_root
            dw  OFFSET n_tname, OFFSET n_wild, OFFSET n_tcom, OFFSET n_ucom, 0
n_net1      db  'H:\NET1.TXT',0
n_net2      db  'H:\NET2.TXT',0
n_all       db  'H:\*.*',0
n_allc      db  'H:\????????.???',0
n_found     db  'NET1.TXT',0
n_sub       db  'H:\SUB',0
n_subonly   db  'SUB',0
n_rel       db  'REL.TXT',0
n_relfull   db  'H:\SUB\REL.TXT',0
n_root      db  'H:\',0
n_tname     db  'H:NET2.TXT',0
n_wild      db  'H:\N?T2.TXT',0
n_tcom      db  'H:\T.COM',0
n_ucom      db  'H:\U.COM',0
n_tail      db  0,13
n_rep       db  'C:\NETTEST.RM',0
hello       db  'hello world'

;--- T.COM: mov ax,4C2Ah / int 21h
tcom        db  0B8h, 2Ah, 4Ch, 0CDh, 21h
TCOM_LEN    EQU 5
;--- U.COM: opens H:\NET2.TXT, and ends without closing it
ucom        db  0BAh, 0Dh, 01h          ; mov dx,010Dh
            db  0B8h, 00h, 3Dh          ; mov ax,3D00h
            db  0CDh, 21h
            db  0B8h, 00h, 4Ch          ; mov ax,4C00h
            db  0CDh, 21h
ucom_name   db  'H:\NET2.TXT',0
UCOM_LEN    EQU 0Dh + 12

;--- the calls each step should cause (FFh ends each list)
x_none      db  0FFh
x_create    db  17h, 0FFh
x_write     db  09h, 0FFh
x_close     db  06h, 0FFh
x_open      db  16h, 0FFh
x_lseek     db  21h, 0FFh
x_read      db  08h, 0FFh
x_dup       db  06h, 06h, 0FFh
x_attr      db  0Eh, 0Fh, 0FFh
x_find      db  1Bh, 1Ch, 0FFh
x_ren       db  11h, 0FFh
x_dirs      db  03h, 05h, 17h, 06h, 13h, 05h, 01h, 0FFh
x_space     db  0Ch, 0Ch, 0FFh
; EXEC: the open, the header, the image, the close; then the child's
; 4Ch - the process ending (22h), its files closed, all its remote
; files gone (1Dh) - and in the second, the child's own open between
x_exec      db  16h, 08h, 08h, 06h, 22h, 1Dh, 0FFh
x_orphan    db  16h, 08h, 08h, 06h, 16h, 22h, 06h, 1Dh, 0FFh

LOGMAX      EQU 120

outh        dw  1
repfh       dw  0
fails       db  0
rmexit      db  0
onech       db  0
drv         db  0
letter      db  0
olddrv      db  0
cdsoff      dw  0
cdsseg      dw  0
cdsptr      dd  0
sdaoff      dw  0
sdaseg      dw  0
old2f       dd  0
h1          dw  0
res         dw  0
res2        dw  0
res3        dw  0
res4        dw  0
rescf       db  0
dpboff      dw  0
dpbseg      dw  0
bufseg      dw  0
prev13      dd  0
p13n        dw  0
p13ax       dw  0
p13bx       dw  0
p13dx       dw  0
p13es       dw  0
o13n        dw  0
o13old      dd  0
o13boot     dd  0
expect      dw  0
mark        db  0
same        db  0
logn        db  0
logbuf      db  LOGMAX dup (0)
anom        dw  0
lparm       dw  0
lsattr      db  0
lusrah      db  0
lfn1        db  80 dup (0)
lfn2        db  80 dup (0)
r_ax        dw  0
r_bx        dw  0
r_cx        dw  0
r_dx        dw  0
r_si        dw  0
r_di        dw  0
r_es        dw  0
r_ds        dw  0
r_parm      dw  0
r_err       dw  0
srchdir     db  80 dup (0)
srchtpl     db  11 dup (0)
srchname    db  11 dup (0)
srchidx     db  0
epb         dw  7 dup (0)
savecds     db  CDS_LEN dup (0)
dta         db  43 dup (0)
buf         db  80 dup (0)
pathbuf     db  128 dup (0)
buf2        db  128 dup (0)
files       db  N_FILES * E_LEN dup (0)

_NT     ENDS
        END     start
