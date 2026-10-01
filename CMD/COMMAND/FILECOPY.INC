;=====================================================================
; FILECOPY.INC - COPY.
;
;   COPY [/A|/B] source [/A|/B] [+ source [/A|/B] [+ ...]]
;        [destination [/A|/B]] [/V] [/Y|/-Y]
;
; Two different commands share the name.  A PLAIN copy takes each file
; the source matches to a file of the same name in a directory, to a
; name made from a wildcard pattern, or to one file.  A CONCATENATION -
; sources joined by '+', or a wildcard source copied onto one file -
; writes every source, one after another, into a single destination,
; and if no destination is given, into the first source ("COPY A+B"
; appends B to A).
;
; /A and /B are the ASCII/binary modes: an ASCII source is read up to
; its first ^Z, and an ASCII destination gets one written at the end.
; A plain copy is binary unless told otherwise; a concatenation is
; ASCII, because what it was invented for was joining text files.  A
; switch after a name applies to that name and the ones after it; a
; switch before any name, to all of them.
;
; Overwriting an existing file is asked about - "Overwrite X
; (Yes/No/All)?" - at the prompt and not in a batch file, which is
; where DOS 6.2 drew the line.  COPYCMD and /Y /-Y move it.
;=====================================================================

CPI_NAME        EQU     0
CPI_MODE        EQU     PATH_MAX
CPI_PLUS        EQU     PATH_MAX + 1
CPI_LEN         EQU     PATH_MAX + 2
CP_MAXITEMS     EQU     12

CPM_ASCII       EQU     0
CPM_BINARY      EQU     1
CPM_UNSET       EQU     0FFh

; where a plain copy puts each file
CPD_DIR         EQU     0               ; dest dir + the source's name
CPD_PATTERN     EQU     1               ; dest dir + a pattern applied
CPD_FILE        EQU     2               ; the destination, exactly

cmd_copy:
        mov     [cp_argp],si
        mov     word ptr [cp_n],0
        mov     word ptr [cp_count],0
        mov     byte ptr [cp_curmode],CPM_UNSET
        mov     byte ptr [cp_pendplus],0
        mov     byte ptr [cp_verify],0
        mov     byte ptr [cp_lastfile],0
        ; asked about overwriting: at the prompt yes, in a batch file no
        mov     al,1
        cmp     word ptr [bat_depth],0
        je      cpy_pdef
        xor     al,al
cpy_pdef:
        mov     [cp_prompt],al
        ; COPYCMD's /Y or /-Y, before the command line's
        push    es
        mov     si,OFFSET name_copycmd
        call    env_find
        jc      cpy_noenv
cpy_env:
        mov     al,es:[bx]
        or      al,al
        jz      cpy_noenv
        inc     bx
        cmp     al,'/'
        jne     cpy_env
        mov     ax,es:[bx]
        call    upcase
        cmp     al,'Y'
        jne     cpy_envminus
        mov     byte ptr [cp_prompt],0
        jmp     cpy_env
cpy_envminus:
        cmp     al,'-'
        jne     cpy_env
        mov     al,ah
        call    upcase
        cmp     al,'Y'
        jne     cpy_env
        mov     byte ptr [cp_prompt],1
        jmp     cpy_env
cpy_noenv:
        pop     es

        mov     si,[cp_argp]
        call    cp_parse
        jc      cp_quit
        cmp     word ptr [cp_n],0
        jne     cpy_have
        jmp     need_arg
cpy_have:
        ; the destination is the last name, if there are two or more
        ; and the last is not joined on with '+'
        mov     word ptr [cp_dest],-1
        mov     ax,[cp_n]
        mov     [cp_nsrc],ax
        cmp     ax,2
        jb      cpy_nodest
        dec     ax
        call    cp_item                 ; BX -> item AX
        cmp     byte ptr [bx+CPI_PLUS],0
        jne     cpy_nodest
        mov     [cp_dest],ax
        mov     [cp_nsrc],ax
cpy_nodest:
        ; a concatenation?  Any '+' among the sources...
        mov     byte ptr [cp_concat],0
        mov     cx,1
cpy_plus:
        cmp     cx,[cp_nsrc]
        jae     cpy_plusdone
        mov     ax,cx
        call    cp_item
        cmp     byte ptr [bx+CPI_PLUS],0
        je      cpy_plusnext
        mov     byte ptr [cp_concat],1
cpy_plusnext:
        inc     cx
        jmp     cpy_plus
cpy_plusdone:
        cmp     byte ptr [cp_concat],0
        jne     cpy_modes
        cmp     word ptr [cp_nsrc],1
        je      cpy_one
        mov     si,OFFSET msg_toomany
        call    puts_err
        jmp     cp_quit
cpy_one:
        ; ...or one wildcard source onto one file that is not a
        ; directory: COPY *.TXT ALL.TXT joins them
        cmp     word ptr [cp_dest],-1
        je      cpy_modes
        xor     ax,ax
        call    cp_item
        mov     si,bx
        call    has_wild
        jnc     cpy_modes
        mov     ax,[cp_dest]
        call    cp_item
        mov     si,bx
        call    has_wild
        jc      cpy_modes
        call    cp_isdir
        jnc     cpy_modes
        mov     byte ptr [cp_concat],1
cpy_modes:
        ; the default mode for anything no switch reached
        mov     dl,CPM_BINARY
        cmp     byte ptr [cp_concat],0
        je      cpy_mfill
        mov     dl,CPM_ASCII
cpy_mfill:
        xor     cx,cx
cpy_mloop:
        cmp     cx,[cp_n]
        jae     cpy_mdone
        mov     ax,cx
        call    cp_item
        cmp     byte ptr [bx+CPI_MODE],CPM_UNSET
        jne     cpy_mnext
        mov     [bx+CPI_MODE],dl
cpy_mnext:
        inc     cx
        jmp     cpy_mloop
cpy_mdone:
        mov     bx,0FFFh
        call    work_big
        jc      cp_quit
        ; /V: verify on for the length of the copy
        mov     ah,54h
        int     21h
        mov     [cp_oldverify],al
        cmp     byte ptr [cp_verify],0
        je      cpy_run
        mov     ax,2E01h
        xor     dl,dl
        int     21h
cpy_run:
        cmp     byte ptr [cp_concat],0
        je      cpy_plain
        call    cp_join
        jmp     short cpy_done
cpy_plain:
        call    cp_plain
cpy_done:
        mov     al,[cp_oldverify]
        mov     ah,2Eh
        xor     dl,dl
        int     21h
        call    work_free
        mov     eax,0
        mov     ax,[cp_count]
        mov     bx,9
        call    put_num
        mov     si,OFFSET msg_copied
        call    puts
cp_quit:
        ret

; cp_item - AX = an item's number; BX -> it
cp_item:
        push    ax
        push    dx
        mov     bx,CPI_LEN
        mul     bx
        mov     bx,ax
        add     bx,OFFSET cp_items
        pop     dx
        pop     ax
        ret

;---------------------------------------------------------------------
; cp_parse - the arguments at SI into cp_items.  CF (with the message
;            printed) if a switch is not one or there are too many.
;---------------------------------------------------------------------
cp_parse:
        call    skip_dl
        mov     al,[si]
        or      al,al
        jz      cpa_done
        cmp     al,'+'
        jne     cpa_notplus
        inc     si
        mov     byte ptr [cp_pendplus],1
        mov     byte ptr [cp_lastfile],0
        jmp     cp_parse
cpa_notplus:
        cmp     al,'/'
        je      cpa_switch
        ; a name, up to a delimiter, a '+' or a '/'
        mov     ax,[cp_n]
        cmp     ax,CP_MAXITEMS
        jae     cpa_toomany
        call    cp_item
        mov     di,bx
        xor     cx,cx
cpa_name:
        mov     al,[si]
        or      al,al
        jz      cpa_nend
        call    is_dl
        je      cpa_nend
        cmp     al,'+'
        je      cpa_nend
        cmp     al,'/'
        je      cpa_nend
        inc     si
        cmp     cx,PATH_MAX - 1
        jae     cpa_name
        mov     [di],al
        inc     di
        inc     cx
        jmp     cpa_name
cpa_nend:
        mov     byte ptr [di],0
        mov     al,[cp_curmode]
        mov     [bx+CPI_MODE],al
        mov     al,[cp_pendplus]
        mov     [bx+CPI_PLUS],al
        mov     byte ptr [cp_pendplus],0
        mov     byte ptr [cp_lastfile],1
        inc     word ptr [cp_n]
        jmp     cp_parse
cpa_switch:
        inc     si
        xor     dl,dl                   ; DL = 1: the /- form
        cmp     byte ptr [si],'-'
        jne     cpa_sw1
        inc     si
        inc     dl
cpa_sw1:
        mov     al,[si]
        inc     si
        call    upcase
        mov     ah,[si]                 ; nothing may follow the letter
        or      ah,ah
        jz      cpa_swok
        push    ax
        mov     al,ah
        call    is_dl
        pop     ax
        je      cpa_swok
        cmp     ah,'/'
        je      cpa_swok
        cmp     ah,'+'
        jne     cpa_badsw
cpa_swok:
        cmp     al,'Y'
        jne     cpa_notY
        xor     dl,1                    ; /Y: do not ask, /-Y: ask
        mov     [cp_prompt],dl
        jmp     cp_parse
cpa_notY:
        or      dl,dl
        jnz     cpa_badsw
        cmp     al,'V'
        jne     cpa_notV
        mov     byte ptr [cp_verify],1
        jmp     cp_parse
cpa_notV:
        mov     dl,CPM_ASCII
        cmp     al,'A'
        je      cpa_mode
        mov     dl,CPM_BINARY
        cmp     al,'B'
        jne     cpa_badsw
cpa_mode:
        mov     [cp_curmode],dl
        cmp     byte ptr [cp_lastfile],0 ; right after a name: that name
        je      cp_parse
        mov     ax,[cp_n]
        dec     ax
        call    cp_item
        mov     [bx+CPI_MODE],dl
        jmp     cp_parse
cpa_done:
        clc
        ret
cpa_badsw:
        push    ax                      ; AL is still the letter
        mov     si,OFFSET msg_badsw
        call    puts_err
        mov     al,'/'
        call    putc_err
        pop     ax
        call    putc_err
        call    crlf_err
        stc
        ret
cpa_toomany:
        mov     si,OFFSET msg_toomany
        call    puts_err
        stc
        ret

;---------------------------------------------------------------------
; cp_isdir - CF clear if the path at SI is a directory: it ends in '\'
;            or ':', its last part is . or .., or DOS says so.
;---------------------------------------------------------------------
cp_isdir:
        push    ax
        push    cx
        push    dx
        push    di
        call    strlen
        jcxz    cid_yes                 ; nothing: the current directory
        mov     di,si
        add     di,cx
        mov     al,[di-1]
        cmp     al,'\'
        je      cid_yes
        cmp     al,':'
        je      cid_yes
        call    name_part
        cmp     byte ptr [di],'.'
        jne     cid_ask
        cmp     byte ptr [di+1],0
        je      cid_yes
        cmp     word ptr [di+1],'.'     ; ".." and its NUL
        je      cid_yes
cid_ask:
        mov     dx,si
        mov     ax,4300h
        int     21h
        jc      cid_no
        test    cl,10h
        jz      cid_no
cid_yes:
        clc
        jmp     short cid_out
cid_no:
        stc
cid_out:
        pop     di
        pop     dx
        pop     cx
        pop     ax
        ret

;---------------------------------------------------------------------
; cp_plain - each file the one source matches, to its destination.
;---------------------------------------------------------------------
cp_plain:
        xor     ax,ax
        call    cp_item
        mov     al,[bx+CPI_MODE]
        mov     [cp_smode],al
        mov     si,bx
        mov     di,OFFSET cp_src
        call    strcpy
        ; a directory as the source is everything in it
        mov     byte ptr [cp_show],0
        mov     si,OFFSET cp_src
        call    has_wild
        jnc     cpp_nowild
        mov     byte ptr [cp_show],1
cpp_nowild:
        call    cp_isdir
        jc      cpp_srcok
        call    dir_to_spec
        mov     byte ptr [cp_show],1
cpp_srcok:
        mov     si,OFFSET cp_src
        call    name_part
        mov     [cp_srcname],di

        ; the destination, and what kind
        mov     byte ptr [cp_dst],0
        mov     byte ptr [cp_dmode],CPM_BINARY
        mov     ax,[cp_dest]
        cmp     ax,-1
        je      cpp_dstkind
        call    cp_item
        mov     al,[bx+CPI_MODE]
        mov     [cp_dmode],al
        mov     si,bx
        mov     di,OFFSET cp_dst
        call    strcpy
cpp_dstkind:
        mov     si,OFFSET cp_dst
        call    cp_isdir
        jc      cpp_notdir
        mov     byte ptr [cp_dkind],CPD_DIR
        ; the prefix: the directory with a '\' after it, unless it is
        ; empty or already ends in one
        call    strlen
        mov     di,si
        add     di,cx
        jcxz    cpp_prefix
        mov     al,[di-1]
        cmp     al,'\'
        je      cpp_prefix
        cmp     al,':'
        je      cpp_prefix
        mov     word ptr [di],'\'
cpp_prefix:
        jmp     short cpp_go
cpp_notdir:
        mov     byte ptr [cp_dkind],CPD_FILE
        call    has_wild
        jnc     cpp_go
        mov     byte ptr [cp_dkind],CPD_PATTERN
        call    name_part               ; the pattern moves out, the
        push    di                      ; directory part stays
        mov     si,di
        mov     di,OFFSET cp_pat
        call    strcpy
        pop     di
        mov     byte ptr [di],0
cpp_go:
        call    set_dta
        mov     dx,OFFSET cp_src
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        jnc     cpp_each
        mov     si,OFFSET msg_fnf2
        call    puts_err
        xor     ax,ax
        call    cp_item
        mov     si,bx
        call    puts_err
        jmp     crlf_err
cpp_each:
        ; from: the source's directory part and the name found
        mov     si,OFFSET cp_src
        mov     di,OFFSET cp_from
        mov     cx,[cp_srcname]
        sub     cx,si
        rep     movsb
        mov     si,OFFSET dta + 1Eh
        call    strcpy
        ; to
        mov     si,OFFSET cp_dst
        mov     di,OFFSET cp_to
        call    strcpy
        cmp     byte ptr [cp_dkind],CPD_FILE
        je      cpp_toset
        mov     si,OFFSET dta + 1Eh
        cmp     byte ptr [cp_dkind],CPD_DIR
        jne     cpp_topat
        call    strcpy
        jmp     short cpp_toset
cpp_topat:
        mov     bx,OFFSET cp_pat
        call    apply_pattern
cpp_toset:
        ; the same file both ways is refused, and ends the copy
        call    cp_same
        jnc     cpp_notsame
        mov     si,OFFSET msg_self
        call    puts_err
        ret
cpp_notsame:
        call    cp_mayover
        jc      cpp_next
        cmp     byte ptr [cp_show],0
        je      cpp_copy
        mov     si,OFFSET cp_from
        call    puts
        call    crlf
cpp_copy:
        call    cp_file
        jc      cpp_stop
        inc     word ptr [cp_count]
cpp_next:
        call    set_dta
        mov     ah,4Fh
        int     21h
        jnc     cpp_each
cpp_stop:
        ret

;---------------------------------------------------------------------
; cp_same - CF if cp_from and cp_to are one file.  TRUENAME settles it
;           where it can; a DOS that will not say is compared as typed.
;---------------------------------------------------------------------
cp_same:
        mov     si,OFFSET cp_from
        mov     di,OFFSET cp_true1
        mov     ah,60h
        int     21h
        jc      cps_raw
        mov     si,OFFSET cp_to
        mov     di,OFFSET cp_true2
        mov     ah,60h
        int     21h
        jc      cps_raw
        mov     si,OFFSET cp_true1
        mov     di,OFFSET cp_true2
        jmp     short cps_cmp
cps_raw:
        mov     si,OFFSET cp_from
        mov     di,OFFSET cp_to
cps_cmp:
        call    stricmp
        jne     cps_no
        stc
        ret
cps_no:
        clc
        ret

;---------------------------------------------------------------------
; cp_mayover - cp_to exists and overwriting it is to be asked about:
;              ask.  CF if the answer is No.  "All" stops the asking.
;---------------------------------------------------------------------
cp_mayover:
        cmp     byte ptr [cp_prompt],0
        je      cmo_go
        mov     dx,OFFSET cp_to
        mov     ax,4300h
        int     21h
        jc      cmo_go                  ; not there: nothing to ask
        test    cl,10h
        jnz     cmo_go
        ; a device is written to, not overwritten
        mov     dx,OFFSET cp_to
        mov     ax,3D00h
        int     21h
        jc      cmo_ask
        mov     bx,ax
        mov     ax,4400h
        int     21h
        pushf
        push    dx
        mov     ah,3Eh
        int     21h
        pop     dx
        popf
        jc      cmo_ask
        test    dl,80h
        jnz     cmo_go
cmo_ask:
        mov     si,OFFSET msg_ovw1
        call    puts
        mov     si,OFFSET cp_to
        call    puts
        mov     si,OFFSET msg_ovw2
        call    puts
cmo_key:
        call    get_key
        call    upcase
        cmp     al,'Y'
        je      cmo_ans
        cmp     al,'N'
        je      cmo_ans
        cmp     al,'A'
        jne     cmo_key
        mov     byte ptr [cp_prompt],0
cmo_ans:
        call    putc
        call    crlf
        cmp     al,'N'
        je      cmo_no
cmo_go:
        clc
        ret
cmo_no:
        stc
        ret

;---------------------------------------------------------------------
; cp_file - cp_from to cp_to, [cp_smode] and [cp_dmode].  CF if it went
;           wrong (with the message printed), which ends the copy.
;
; A binary copy of a file keeps the file's date and time; anything
; involving a device does not, having no date and time to keep.
;---------------------------------------------------------------------
cp_file:
        mov     dx,OFFSET cp_from
        mov     ax,3D00h
        int     21h
        jnc     cpf_src
        mov     si,OFFSET msg_notfound
        call    puts_err
        stc
        ret
cpf_src:
        mov     [h_src],ax
        mov     bx,ax
        call    cp_isdev
        mov     [cp_srcdev],al
        or      al,al
        jz      cpf_srcmode
        mov     byte ptr [cp_smode],CPM_ASCII   ; a device is text
cpf_srcmode:
        mov     dx,OFFSET cp_to
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jnc     cpf_dst
        call    cp_closesrc
        mov     si,OFFSET msg_denied
        call    puts_err
        stc
        ret
cpf_dst:
        mov     [h_dst],ax
        mov     bx,ax
        call    cp_isdev
        mov     [cp_dstdev],al
        mov     al,[cp_smode]
        call    cp_pump
        jc      cpf_full
        cmp     byte ptr [cp_dmode],CPM_ASCII
        jne     cpf_stamp
        call    cp_ctlz
        jc      cpf_full
cpf_stamp:
        mov     al,[cp_srcdev]
        or      al,[cp_dstdev]
        jnz     cpf_close
        mov     bx,[h_src]
        mov     ax,5700h
        int     21h
        jc      cpf_close
        mov     bx,[h_dst]
        mov     ax,5701h
        int     21h
cpf_close:
        call    cp_closedst
        call    cp_closesrc
        clc
        ret
cpf_full:
        call    cp_closedst
        call    cp_closesrc
        cmp     byte ptr [cp_dstdev],0
        jne     cpf_fullmsg
        mov     dx,OFFSET cp_to         ; half a file is not a copy, and
        call    del_nokeep              ; not worth keeping either
cpf_fullmsg:
        mov     si,OFFSET msg_diskfull
        call    puts_err
        stc
        ret

;---------------------------------------------------------------------
; del_nokeep - delete DS:DX so that PM-DOS does not keep it for
;              UNDELETE (21h/F1h AL=19h).  Any other DOS, or a PM-DOS
;              without that call, gets a plain 41h.  Trashes AX-CX.
;---------------------------------------------------------------------
del_nokeep:
        push    dx
        mov     ah,30h
        int     21h
        pop     dx
        cmp     bh,03h                  ; PMDOS_OEM - F1h is ours alone
        jne     dnk_plain
        mov     ax,0F119h
        int     21h
        jnc     dnk_out
        cmp     ax,1                    ; "invalid function": an older kernel
        jne     dnk_fail
dnk_plain:
        mov     ah,41h
        int     21h
dnk_out:
        ret
dnk_fail:
        stc
        ret

cp_closesrc:
        mov     bx,[h_src]
        cmp     bx,-1
        je      ccs_out
        mov     ah,3Eh
        int     21h
        mov     word ptr [h_src],-1
ccs_out:
        ret
cp_closedst:
        mov     bx,[h_dst]
        cmp     bx,-1
        je      ccd_out
        mov     ah,3Eh
        int     21h
        mov     word ptr [h_dst],-1
ccd_out:
        ret

; cp_isdev - AL = 1 if handle BX is a device
cp_isdev:
        mov     ax,4400h
        int     21h
        mov     al,0
        jc      cid2_out
        test    dl,80h
        jz      cid2_out
        mov     al,1
cid2_out:
        ret

;---------------------------------------------------------------------
; cp_pump - everything from [h_src] onto [h_dst]; AL = the source's
;           mode.  An ASCII source stops at its first ^Z.  CF if a
;           write came up short: the disk is full.
;---------------------------------------------------------------------
cp_pump:
        mov     [cp_pmode],al
cpu_loop:
        mov     bx,[h_src]
        mov     cx,[work_len]
        push    ds
        mov     ds,cs:[work_seg]
        xor     dx,dx
        mov     ah,3Fh
        int     21h
        pop     ds
        jc      cpu_done
        or      ax,ax
        jz      cpu_done
        mov     cx,ax
        mov     byte ptr [cp_eof],0
        cmp     byte ptr [cp_pmode],CPM_ASCII
        jne     cpu_write
        push    es
        push    cx
        mov     es,[work_seg]
        xor     di,di
        mov     al,1Ah
        repne   scasb
        pop     cx
        pop     es
        jne     cpu_write
        mov     cx,di                   ; up to the ^Z
        dec     cx
        mov     byte ptr [cp_eof],1
cpu_write:
        jcxz    cpu_wrote
        push    cx
        mov     bx,[h_dst]
        push    ds
        mov     ds,cs:[work_seg]
        xor     dx,dx
        mov     ah,40h
        int     21h
        pop     ds
        pop     cx
        jc      cpu_full
        cmp     ax,cx
        jb      cpu_full
cpu_wrote:
        cmp     byte ptr [cp_eof],0
        je      cpu_loop
cpu_done:
        clc
        ret
cpu_full:
        stc
        ret

; cp_ctlz - one ^Z onto [h_dst].  CF if it would not go.
cp_ctlz:
        mov     byte ptr [cp_z],1Ah
        mov     bx,[h_dst]
        mov     dx,OFFSET cp_z
        mov     cx,1
        mov     ah,40h
        int     21h
        jc      ccz_bad
        cmp     ax,1
        jb      ccz_bad
        clc
        ret
ccz_bad:
        stc
        ret

;---------------------------------------------------------------------
; cp_join - the concatenation: every source, in order, into one file.
;
; With no destination the first source IS the destination, and is
; appended to rather than copied onto itself.  Any other source that
; turns out to be the destination - COPY *.TXT ALL.TXT meeting ALL.TXT
; - is skipped with DOS's own complaint, because by then it has been
; half overwritten.
;---------------------------------------------------------------------
cp_join:
        mov     byte ptr [cp_append],0
        mov     byte ptr [cp_opened],0
        ; the destination's name
        mov     ax,[cp_dest]
        cmp     ax,-1
        jne     cpj_named
        xor     ax,ax                   ; none: the first source
        mov     byte ptr [cp_append],1
cpj_named:
        call    cp_item
        mov     al,[bx+CPI_MODE]
        mov     [cp_dmode],al
        mov     si,bx
        mov     di,OFFSET cp_to
        call    strcpy
        cmp     byte ptr [cp_append],0
        jne     cpj_true
        ; into a directory: under the first source's name
        mov     si,OFFSET cp_to
        call    cp_isdir
        jc      cpj_true
        call    strlen
        mov     di,si
        add     di,cx
        jcxz    cpj_dirname
        mov     al,[di-1]
        cmp     al,'\'
        je      cpj_dirname
        cmp     al,':'
        je      cpj_dirname
        mov     byte ptr [di],'\'
        inc     di
cpj_dirname:
        push    di
        xor     ax,ax
        call    cp_item
        mov     si,bx
        call    name_part
        mov     si,di
        pop     di
        call    strcpy
cpj_true:
        mov     si,OFFSET cp_to
        mov     di,OFFSET cp_dtrue
        mov     ah,60h
        int     21h
        jnc     cpj_sources
        mov     si,OFFSET cp_to
        mov     di,OFFSET cp_dtrue
        call    strcpy
cpj_sources:
        mov     word ptr [cp_i],0
cpj_item:
        mov     ax,[cp_i]
        cmp     ax,[cp_nsrc]
        jae     cpj_end
        call    cp_item
        mov     al,[bx+CPI_MODE]
        mov     [cp_smode],al
        mov     si,bx
        mov     di,OFFSET cp_src
        call    strcpy
        mov     si,OFFSET cp_src
        call    cp_isdir
        jc      cpj_srcok
        call    dir_to_spec
cpj_srcok:
        mov     si,OFFSET cp_src
        call    name_part
        mov     [cp_srcname],di
        call    set_dta
        mov     dx,OFFSET cp_src
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        jc      cpj_inext
cpj_file:
        mov     si,OFFSET cp_src
        mov     di,OFFSET cp_from
        mov     cx,[cp_srcname]
        sub     cx,si
        rep     movsb
        mov     si,OFFSET dta + 1Eh
        call    strcpy
        ; is it the destination?
        mov     si,OFFSET cp_from
        mov     di,OFFSET cp_true1
        mov     ah,60h
        int     21h
        jnc     cpj_cmp
        mov     si,OFFSET cp_from
        mov     di,OFFSET cp_true1
        call    strcpy
cpj_cmp:
        mov     si,OFFSET cp_true1
        mov     di,OFFSET cp_dtrue
        call    stricmp
        jne     cpj_copy
        cmp     byte ptr [cp_append],0
        je      cpj_lost
        cmp     word ptr [cp_i],0
        jne     cpj_lost
        ; the first source, appended to: its name, and that is all
        mov     si,OFFSET cp_from
        call    puts
        call    crlf
        call    cpj_open
        jc      cpj_fail
        jmp     short cpj_fnext
cpj_lost:
        mov     si,OFFSET msg_lost
        call    puts_err
        jmp     short cpj_fnext
cpj_copy:
        mov     si,OFFSET cp_from
        call    puts
        call    crlf
        call    cpj_open
        jc      cpj_fail
        mov     dx,OFFSET cp_from
        mov     ax,3D00h
        int     21h
        jc      cpj_fnext
        mov     [h_src],ax
        mov     bx,ax
        call    cp_isdev
        or      al,al
        mov     al,[cp_smode]
        jz      cpj_pump
        mov     al,CPM_ASCII
cpj_pump:
        call    cp_pump
        pushf
        call    cp_closesrc
        popf
        jc      cpj_full
cpj_fnext:
        call    set_dta
        mov     ah,4Fh
        int     21h
        jnc     cpj_file
cpj_inext:
        inc     word ptr [cp_i]
        jmp     cpj_item
cpj_end:
        cmp     byte ptr [cp_opened],0
        je      cpj_none
        cmp     byte ptr [cp_dmode],CPM_ASCII
        jne     cpj_close
        call    cp_ctlz
        jc      cpj_full
cpj_close:
        call    cp_closedst
        mov     word ptr [cp_count],1
        ret
cpj_none:
        mov     si,OFFSET msg_notfound
        jmp     puts_err
cpj_full:
        call    cp_closedst
        mov     si,OFFSET msg_diskfull
        jmp     puts_err
cpj_fail:
        mov     si,OFFSET msg_denied
        jmp     puts_err

; cpj_open - the destination, opened the first time it is needed: made
;            new, or, when appending, opened and wound to its end (back
;            over a ^Z there if it is being added to as text).
cpj_open:
        cmp     byte ptr [cp_opened],0
        jne     cpo_ok
        cmp     byte ptr [cp_append],0
        je      cpo_create
        mov     dx,OFFSET cp_to
        mov     ax,3D02h
        int     21h
        jc      cpo_bad
        mov     [h_dst],ax
        mov     bx,ax
        xor     cx,cx
        xor     dx,dx
        mov     ax,4202h
        int     21h
        mov     cx,dx
        mov     dx,ax
        or      ax,cx
        jz      cpo_done
        cmp     byte ptr [cp_dmode],CPM_ASCII
        jne     cpo_done
        sub     dx,1
        sbb     cx,0
        push    cx
        push    dx
        mov     ax,4200h
        int     21h
        mov     dx,OFFSET cp_z
        mov     cx,1
        mov     ah,3Fh
        int     21h
        pop     dx
        pop     cx
        cmp     byte ptr [cp_z],1Ah
        jne     cpo_end
        mov     ax,4200h
        int     21h
        jmp     short cpo_done
cpo_end:
        xor     cx,cx
        xor     dx,dx
        mov     ax,4202h
        int     21h
        jmp     short cpo_done
cpo_create:
        mov     dx,OFFSET cp_to
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      cpo_bad
        mov     [h_dst],ax
cpo_done:
        mov     byte ptr [cp_opened],1
cpo_ok:
        clc
        ret
cpo_bad:
        stc
        ret

;---------------------------------------------------------------------
; Data
;---------------------------------------------------------------------
msg_copied  db  ' file(s) copied',13,10,0
msg_ovw1    db  'Overwrite ',0
msg_ovw2    db  ' (Yes/No/All)?',0
msg_self    db  'File cannot be copied onto itself',13,10,0
msg_lost    db  'Content of destination lost before copy',13,10,0
msg_diskfull db 'Insufficient disk space',13,10,0
msg_fnf2    db  'File not found - ',0

cp_argp     dw  0
cp_n        dw  0
cp_nsrc     dw  0
cp_dest     dw  0
cp_count    dw  0
cp_srcname  dw  0
cp_i        dw  0
cp_curmode  db  0
cp_pendplus db  0
cp_lastfile db  0
cp_verify   db  0
cp_oldverify db 0
cp_prompt   db  0
cp_concat   db  0
cp_show     db  0
cp_smode    db  0
cp_dmode    db  0
cp_dkind    db  0
cp_srcdev   db  0
cp_dstdev   db  0
cp_pmode    db  0
cp_eof      db  0
cp_append   db  0
cp_opened   db  0
cp_z        db  0
cp_items    db  CP_MAXITEMS * CPI_LEN dup(0)
cp_src      db  NAME_MAX dup(0)
cp_dst      db  NAME_MAX dup(0)
cp_pat      db  16 dup(0)
cp_from     db  NAME_MAX dup(0)
cp_to       db  NAME_MAX dup(0)
cp_true1    db  NAME_MAX dup(0)
cp_true2    db  NAME_MAX dup(0)
cp_dtrue    db  NAME_MAX dup(0)
