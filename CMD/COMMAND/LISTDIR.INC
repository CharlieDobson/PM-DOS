;=====================================================================
; LISTDIR.INC - DIR, and the volume header VOL shares with it.
;
; The layout is MS-DOS 6.22's to the column, because this is the shell
; DOS programs get and the one thing a DOS program might do with DIR's
; output is read it:
;
;   COMMAND  COM        54,645 05-31-94   6:22a
;   DOS          <DIR>         01-01-95  12:00a
;           2 file(s)         54,645 bytes
;                         12,345,678 bytes free
;
; Switches: /P /W /A[[:]attr] /O[[:]order] /S /B /L, each undone by its
; /- form, DIRCMD read first so the command line overrides it.  /C and
; /CH - DoubleSpace's compression ratio - are accepted and do nothing.
;=====================================================================

DRF_P           EQU     0001h
DRF_W           EQU     0002h
DRF_B           EQU     0004h
DRF_L           EQU     0008h
DRF_S           EQU     0010h
DRF_O           EQU     0020h

; One entry, as DIR keeps it: the name the way it is printed - blank-
; padded 8 and 3, which is also the order DOS sorted by - and the four
; things a row shows.
DR_NAME         EQU     0
DR_EXT          EQU     8
DR_ATTR         EQU     11
DR_TIME         EQU     12
DR_DATE         EQU     14
DR_SIZE         EQU     16
DR_LEN          EQU     20

DIR_MAXKEYS     EQU     5
DIR_MAXDEPTH    EQU     16
DIR_WCOLS       EQU     5

cmd_dir:
        mov     [dir_argp],si
        mov     word ptr [dir_flags],0
        mov     byte ptr [dir_aset],0
        mov     byte ptr [dir_areq],0
        mov     byte ptr [dir_aexc],0
        mov     byte ptr [dir_nkeys],0
        mov     byte ptr [dir_spec],0

        ; DIRCMD first, so that the command line wins
        mov     byte ptr [dir_cmdtxt],0
        push    es
        mov     si,OFFSET name_dircmd
        call    env_find
        jc      dir_noenv
        mov     di,OFFSET dir_cmdtxt
        mov     cx,LINE_MAX - 1
dir_envcp:
        mov     al,es:[bx]
        mov     [di],al
        or      al,al
        jz      dir_noenv
        inc     bx
        inc     di
        loop    dir_envcp
        mov     byte ptr [di],0
dir_noenv:
        pop     es
        mov     byte ptr [dir_fromenv],1
        mov     si,OFFSET dir_cmdtxt
        call    dir_args
        jc      dir_quit
        mov     byte ptr [dir_fromenv],0
        mov     si,[dir_argp]
        call    dir_args
        jc      dir_quit

        ; what to list: a directory is everything in it, and a name
        ; with no extension is NAME.* - "DIR COMMAND" finds COMMAND.COM
        mov     si,OFFSET dir_spec
        call    dir_to_spec
        call    name_part
        mov     si,di
dir_dot:
        lodsb
        or      al,al
        jz      dir_nodot
        cmp     al,'.'
        jne     dir_dot
        jmp     short dir_split
dir_nodot:
        mov     word ptr [si-1],'*.'
        mov     byte ptr [si+1],0
dir_split:
        ; which drive: for the header and the free space
        mov     al,[dir_spec]
        call    upcase
        sub     al,'A' - 1
        cmp     byte ptr [dir_spec+1],':'
        je      dir_drvgot
        mov     ah,19h
        int     21h
        inc     al
dir_drvgot:
        mov     [dir_drv],al
        ; a drive that is not there says so before anything else
        mov     dl,al
        mov     ah,36h
        int     21h
        cmp     ax,0FFFFh
        jne     dir_drvok
        mov     si,OFFSET msg_baddrv
        call    puts_err
        jmp     dir_quit
dir_drvok:
        ; the pattern is the last part; the directory, fully qualified
        ; where DOS will say what it is, is everything before it
        mov     si,OFFSET dir_spec
        call    name_part
        push    di
        mov     si,di
        mov     di,OFFSET dir_pat
        call    strcpy
        pop     di
        mov     si,OFFSET dir_spec
        mov     di,OFFSET dir_full
        mov     ah,60h
        int     21h
        jc      dir_astyped
        mov     si,OFFSET dir_full
        call    name_part               ; DI -> after the last '\'
        mov     byte ptr [di],0
        mov     si,OFFSET dir_full
        mov     di,OFFSET dir_cur
        call    strcpy
        jmp     short dir_ready
dir_astyped:
        ; DOS would not say: the directory part as it was typed
        mov     si,OFFSET dir_spec
        call    name_part
        mov     cx,di
        sub     cx,si
        mov     di,OFFSET dir_cur
        rep     movsb
        mov     byte ptr [di],0
dir_ready:
        mov     word ptr [dir_lines],0
        mov     byte ptr [dir_col],0
        mov     word ptr [dir_hdrs],0
        mov     dword ptr [dir_tcount],0
        mov     dword ptr [dir_tbytes],0
        mov     byte ptr [dir_depth],0
        mov     byte ptr [pd_year4],0

        test    word ptr [dir_flags],DRF_B
        jnz     dir_nohead
        call    crlf
        mov     dl,[dir_drv]
        call    vol_header
        mov     word ptr [dir_lines],3
dir_nohead:
        test    word ptr [dir_flags],DRF_O
        jz      dir_go
        mov     bx,0FFFh
        call    work_big
        jc      dir_quit
dir_go:
        call    dir_level

        ; the end: a grand total under /S, the free space, or nothing
        ; was found at all
        cmp     dword ptr [dir_tcount],0
        je      dir_nf
        test    word ptr [dir_flags],DRF_B
        jnz     dir_end
        test    word ptr [dir_flags],DRF_S
        jz      dir_free
        call    dir_endline
        mov     si,OFFSET msg_totlist
        call    puts
        call    dir_endline
        mov     eax,[dir_tcount]
        mov     ecx,[dir_tbytes]
        call    dir_sumline
dir_free:
        call    dir_freeline
        jmp     short dir_end
dir_nf:
        mov     si,OFFSET msg_notfound
        call    puts_err
dir_end:
        call    work_free
dir_quit:
        ret

;---------------------------------------------------------------------
; dir_args - SI -> switches and (unless [dir_fromenv]) a filespec.  CF
;            if one of them was wrong, with the message printed.
;---------------------------------------------------------------------
dir_args:
        mov     di,OFFSET dir_word
        call    get_arg
        jcxz    da_done
        cmp     byte ptr [dir_word],'/'
        je      da_switch
        cmp     byte ptr [dir_fromenv],0
        jne     dir_args                ; DIRCMD holds switches only
        cmp     byte ptr [dir_spec],0
        jne     da_toomany
        push    si
        mov     si,OFFSET dir_word
        mov     di,OFFSET dir_spec
        call    strcpy
        pop     si
        jmp     dir_args
da_switch:
        push    si
        mov     si,OFFSET dir_word + 1
        call    dir_switch
        pop     si
        jnc     dir_args
        mov     si,OFFSET msg_badsw
        jmp     short da_say
da_toomany:
        mov     si,OFFSET msg_toomany2
da_say:
        call    puts_err
        mov     si,OFFSET dir_word
        call    puts_err
        call    crlf_err
        stc
        ret
da_done:
        clc
        ret

;---------------------------------------------------------------------
; dir_switch - SI -> one switch, after its '/'.  CF if it is not one.
;---------------------------------------------------------------------
dir_switch:
        xor     bl,bl                   ; BL = 1: the /- form
        cmp     byte ptr [si],'-'
        jne     dsw_letter
        inc     si
        inc     bl
dsw_letter:
        mov     al,[si]
        inc     si
        call    upcase
        mov     dx,DRF_P
        cmp     al,'P'
        je      dsw_flag
        mov     dx,DRF_W
        cmp     al,'W'
        je      dsw_flag
        mov     dx,DRF_B
        cmp     al,'B'
        je      dsw_flag
        mov     dx,DRF_L
        cmp     al,'L'
        je      dsw_flag
        mov     dx,DRF_S
        cmp     al,'S'
        je      dsw_flag
        cmp     al,'A'
        je      dsw_attr
        cmp     al,'O'
        je      dsw_order
        cmp     al,'C'
        jne     dsw_bad
        mov     al,[si]                 ; /C or /CH: accepted, ignored
        call    upcase
        cmp     al,'H'
        jne     dsw_end
        inc     si
        jmp     short dsw_end
dsw_flag:
        or      bl,bl
        jnz     dsw_clear
        or      [dir_flags],dx
        jmp     short dsw_end
dsw_clear:
        not     dx
        and     [dir_flags],dx
dsw_end:
        cmp     byte ptr [si],0         ; nothing may follow the letter
        jne     dsw_bad
        clc
        ret
dsw_bad:
        stc
        ret

dsw_attr:
        mov     byte ptr [dir_areq],0
        mov     byte ptr [dir_aexc],0
        mov     byte ptr [dir_aset],0
        or      bl,bl
        jnz     dsw_end                 ; /-A: back to the default
        mov     byte ptr [dir_aset],1
        cmp     byte ptr [si],':'
        jne     dsa_loop
        inc     si
dsa_loop:
        mov     al,[si]
        or      al,al
        jz      dsw_ok
        inc     si
        xor     bh,bh                   ; BH = 1: excluded
        cmp     al,'-'
        jne     dsa_one
        inc     bh
        mov     al,[si]
        inc     si
dsa_one:
        call    upcase
        mov     dl,02h
        cmp     al,'H'
        je      dsa_bit
        mov     dl,04h
        cmp     al,'S'
        je      dsa_bit
        mov     dl,10h
        cmp     al,'D'
        je      dsa_bit
        mov     dl,20h
        cmp     al,'A'
        je      dsa_bit
        mov     dl,01h
        cmp     al,'R'
        jne     dsw_bad
dsa_bit:
        or      bh,bh
        jnz     dsa_exc
        or      [dir_areq],dl
        jmp     dsa_loop
dsa_exc:
        or      [dir_aexc],dl
        jmp     dsa_loop
dsw_ok:
        clc
        ret

dsw_order:
        and     word ptr [dir_flags],NOT DRF_O
        mov     byte ptr [dir_nkeys],0
        or      bl,bl
        jnz     dsw_end                 ; /-O: unsorted
        or      word ptr [dir_flags],DRF_O
        cmp     byte ptr [si],':'
        jne     dso_first
        inc     si
dso_first:
        cmp     byte ptr [si],0
        jne     dso_loop
        mov     word ptr [dir_keys],'NG' ; /O alone is /OGN
        mov     byte ptr [dir_nkeys],2
        clc
        ret
dso_loop:
        mov     al,[si]
        or      al,al
        jz      dsw_ok
        inc     si
        xor     ah,ah
        cmp     al,'-'
        jne     dso_one
        mov     ah,80h
        mov     al,[si]
        inc     si
dso_one:
        call    upcase
        cmp     al,'C'                  ; compression: nothing to sort by
        je      dso_loop
        cmp     al,'N'
        je      dso_key
        cmp     al,'E'
        je      dso_key
        cmp     al,'S'
        je      dso_key
        cmp     al,'D'
        je      dso_key
        cmp     al,'G'
        jne     dsw_bad
dso_key:
        movzx   bx,byte ptr [dir_nkeys]
        cmp     bx,DIR_MAXKEYS
        jae     dso_loop
        or      al,ah
        mov     [dir_keys+bx],al
        inc     byte ptr [dir_nkeys]
        jmp     dso_loop

;---------------------------------------------------------------------
; dir_level - list [dir_cur] and, under /S, every directory below it.
;
; Each level scans for its subdirectories with a DTA of its own, on
; the stack, because the level below uses the search it is still in
; the middle of.
;---------------------------------------------------------------------
dir_level:
        push    bp
        call    dir_files
        test    word ptr [dir_flags],DRF_S
        jz      dlv_out
        cmp     byte ptr [dir_depth],DIR_MAXDEPTH
        jae     dlv_out
        sub     sp,44
        mov     bp,sp
        mov     dx,bp
        mov     ah,1Ah
        int     21h
        mov     si,OFFSET dir_cur
        mov     di,OFFSET dir_srch
        call    strcpy
        mov     si,OFFSET pat_all
        call    strcpy
        mov     dx,OFFSET dir_srch
        mov     cx,16h
        mov     ah,4Eh
        int     21h
        jc      dlv_nosub
dlv_sub:
        test    byte ptr [bp+15h],10h
        jz      dlv_next
        cmp     byte ptr [bp+1Eh],'.'
        je      dlv_next
        mov     si,OFFSET dir_cur
        call    strlen
        push    cx
        lea     si,[bp+1Eh]
        push    cx
        call    strlen
        pop     ax
        add     ax,cx
        cmp     ax,PATH_MAX - 16        ; room for it, a '\' and a name
        pop     cx
        jae     dlv_next
        push    cx
        mov     di,OFFSET dir_cur
        add     di,cx
        call    strcpy
        mov     word ptr [di],'\'       ; '\' and a NUL
        inc     byte ptr [dir_depth]
        call    dir_level
        dec     byte ptr [dir_depth]
        pop     cx
        mov     di,OFFSET dir_cur
        add     di,cx
        mov     byte ptr [di],0
        mov     dx,bp
        mov     ah,1Ah
        int     21h
dlv_next:
        mov     ah,4Fh
        int     21h
        jnc     dlv_sub
dlv_nosub:
        add     sp,44
        call    set_dta
dlv_out:
        pop     bp
        ret

;---------------------------------------------------------------------
; dir_files - the matching entries of [dir_cur], and the line that
;             totals them.  Without /S the heading comes first even if
;             nothing matches (and "File not found" follows at the
;             end); with /S a directory with nothing in it says nothing.
;---------------------------------------------------------------------
dir_files:
        mov     dword ptr [dir_lcount],0
        mov     dword ptr [dir_lbytes],0
        mov     byte ptr [dir_lhdr],0
        test    word ptr [dir_flags],DRF_S
        jnz     dfl_search
        call    dir_heading
dfl_search:
        mov     si,OFFSET dir_cur
        mov     di,OFFSET dir_srch
        call    strcpy
        mov     si,OFFSET dir_pat
        call    strcpy
        call    set_dta
        mov     dx,OFFSET dir_srch
        mov     cx,16h
        mov     ah,4Eh
        int     21h
        jc      dfl_end
        test    word ptr [dir_flags],DRF_O
        jnz     dfl_collect
dfl_stream:
        call    dta_to_rec
        call    dir_want
        jc      dfl_snext
        mov     si,OFFSET dir_rec
        call    dir_count
        call    dir_heading
        call    dir_show
dfl_snext:
        mov     ah,4Fh
        int     21h
        jnc     dfl_stream
        jmp     short dfl_end

dfl_collect:
        mov     word ptr [dir_n],0
dfc_one:
        call    dta_to_rec
        call    dir_want
        jc      dfc_next
        mov     ax,[dir_n]
        inc     ax
        imul    ax,ax,DR_LEN
        cmp     ax,[work_len]
        ja      dfc_next                ; full: the rest are not shown
        sub     ax,DR_LEN
        mov     di,ax
        mov     si,OFFSET dir_rec
        mov     cx,DR_LEN
        push    es
        mov     es,[work_seg]
        rep     movsb
        pop     es
        inc     word ptr [dir_n]
dfc_next:
        mov     ah,4Fh
        int     21h
        jnc     dfc_one
        call    dir_sort
        xor     bx,bx
dfc_show:
        cmp     bx,[dir_n]
        jae     dfl_end
        imul    si,bx,DR_LEN
        push    ds
        mov     ds,cs:[work_seg]
        mov     di,OFFSET dir_rec
        mov     cx,DR_LEN
        rep     movsb
        pop     ds
        push    bx
        mov     si,OFFSET dir_rec
        call    dir_count
        call    dir_heading
        call    dir_show
        pop     bx
        inc     bx
        jmp     dfc_show

dfl_end:
        cmp     dword ptr [dir_lcount],0
        je      dfl_out
        mov     eax,[dir_lcount]
        add     [dir_tcount],eax
        mov     eax,[dir_lbytes]
        add     [dir_tbytes],eax
        test    word ptr [dir_flags],DRF_B
        jnz     dfl_out
        cmp     byte ptr [dir_col],0    ; /W left part of a row
        je      dfl_sum
        mov     byte ptr [dir_col],0
        call    dir_endline
dfl_sum:
        mov     eax,[dir_lcount]
        mov     ecx,[dir_lbytes]
        call    dir_sumline
dfl_out:
        ret

; dir_count - one more entry of SI's record in this directory's totals
dir_count:
        inc     dword ptr [dir_lcount]
        test    byte ptr [si+DR_ATTR],10h
        jnz     dct_out
        mov     eax,[si+DR_SIZE]
        add     [dir_lbytes],eax
dct_out:
        ret

;---------------------------------------------------------------------
; dir_heading - " Directory of X:\PATH", once per directory, and never
;               under /B.  Every one after the first has a blank line
;               in front, which is what sets the /S listing apart.
;---------------------------------------------------------------------
dir_heading:
        cmp     byte ptr [dir_lhdr],0
        jne     dh_out
        mov     byte ptr [dir_lhdr],1
        test    word ptr [dir_flags],DRF_B
        jnz     dh_out
        push    si
        mov     si,OFFSET msg_dirof
        test    word ptr [dir_flags],DRF_S
        jz      dh_say
        call    dir_endline             ; /S: a blank line, and the words
        inc     si                      ; start in column 0 - DOS 6.22
dh_say:
        call    puts
        call    dir_putcur
        call    dir_endline
        call    dir_endline
        pop     si
dh_out:
        ret

; dir_putcur - the directory being listed, as a name: without its
;              trailing '\' unless it is a root ("C:\")
dir_putcur:
        mov     si,OFFSET dir_cur
        call    strlen
        jcxz    dpc_cwd
        cmp     cx,3
        jbe     dpc_all
        mov     di,si
        add     di,cx
        cmp     byte ptr [di-1],'\'
        jne     dpc_all
        dec     cx
dpc_all:
        jmp     puts_n
dpc_cwd:
        ; it was never qualified and it is the current directory
        mov     dl,[dir_drv]
        mov     al,dl
        add     al,'A' - 1
        call    putc
        mov     al,':'
        call    putc
        mov     al,'\'
        call    putc
        mov     si,OFFSET pm_cwd
        mov     ah,47h
        int     21h
        jc      dpc_out
        call    puts
dpc_out:
        ret

;---------------------------------------------------------------------
; dir_sumline - EAX files, ECX bytes: "       N file(s)       B bytes"
; dir_freeline - "                 F bytes free", under the bytes
;---------------------------------------------------------------------
dir_sumline:
        push    ecx
        mov     bx,9
        call    put_num
        mov     si,OFFSET msg_files
        call    puts
        pop     eax
        mov     bx,15
        call    put_num
        mov     si,OFFSET msg_bytes
        call    puts
        jmp     dir_endline

dir_freeline:
        mov     al,[dir_drv]
        add     al,'A' - 1
        mov     [fs_root],al
        ; 7303h first: DOS 7.1's answer is 32 bits wide, where 36h's
        ; saturates at 65535 clusters and reports a large volume as a
        ; small one.  Real DOS 6 does not have it, and says so by not
        ; filling in the structure's length.
        push    es
        push    cs
        pop     es
        mov     di,OFFSET fs_buf
        mov     cx,44
        xor     al,al
        rep     stosb
        mov     di,OFFSET fs_buf
        mov     dx,OFFSET fs_root
        mov     cx,44
        mov     ax,7303h
        int     21h
        pop     es
        jc      dfr_36
        cmp     word ptr [fs_buf],0
        je      dfr_36
        mov     eax,dword ptr [fs_buf+04h]      ; sectors per cluster
        mul     dword ptr [fs_buf+08h]          ; bytes per sector
        or      edx,edx
        jnz     dfr_sat
        mul     dword ptr [fs_buf+0Ch]          ; clusters free
        or      edx,edx
        jnz     dfr_sat
        jmp     short dfr_say
dfr_36:
        mov     dl,[dir_drv]
        mov     ah,36h
        int     21h
        cmp     ax,0FFFFh
        je      dfr_out
        movzx   eax,ax
        movzx   ebx,bx
        movzx   ecx,cx
        mul     ebx
        mul     ecx
        or      edx,edx
        jz      dfr_say
dfr_sat:
        mov     eax,0FFFFFFFFh
dfr_say:
        mov     bx,32
        call    put_num
        mov     si,OFFSET msg_free
        call    puts
        call    dir_endline
dfr_out:
        ret

;---------------------------------------------------------------------
; dir_endline - end a line; under /P, stop for a key every screenful.
;               The screen's height is the BIOS's, so a DOS box a
;               program left in 50 lines pages at 50.
;---------------------------------------------------------------------
dir_endline:
        call    crlf
        test    word ptr [dir_flags],DRF_P
        jz      del2_out
        inc     word ptr [dir_lines]
        push    es
        mov     ax,40h
        mov     es,ax
        movzx   ax,byte ptr es:[84h]    ; rows - 1
        pop     es
        or      ax,ax
        jnz     del2_rows
        mov     ax,24
del2_rows:
        cmp     [dir_lines],ax          ; a page is the screen less one
        jb      del2_out
        mov     si,OFFSET msg_pause
        call    puts
        call    get_key
        call    crlf
        call    crlf
        mov     si,OFFSET msg_contin
        call    puts
        call    dir_putcur
        mov     al,')'
        call    putc
        call    crlf
        mov     word ptr [dir_lines],2
del2_out:
        ret

;---------------------------------------------------------------------
; dta_to_rec - the entry in our DTA, as a record in dir_rec
;---------------------------------------------------------------------
dta_to_rec:
        mov     di,OFFSET dir_rec
        mov     cx,11
        mov     al,' '
        rep     stosb
        mov     si,OFFSET dta + 1Eh
        mov     di,OFFSET dir_rec + DR_NAME
        cmp     byte ptr [si],'.'       ; "." and "..": all name
        je      dtr_dots
        mov     cx,8
dtr_name:
        lodsb
        or      al,al
        jz      dtr_fields
        cmp     al,'.'
        je      dtr_ext
        mov     [di],al
        inc     di
        loop    dtr_name
dtr_skip:
        lodsb                           ; (a name cannot be longer)
        or      al,al
        jz      dtr_fields
        cmp     al,'.'
        jne     dtr_skip
dtr_ext:
        mov     di,OFFSET dir_rec + DR_EXT
        mov     cx,3
dtr_e:
        lodsb
        or      al,al
        jz      dtr_fields
        mov     [di],al
        inc     di
        loop    dtr_e
        jmp     short dtr_fields
dtr_dots:
        movsb
        cmp     byte ptr [si],'.'
        jne     dtr_fields
        movsb
dtr_fields:
        mov     al,[dta+15h]
        mov     [dir_rec+DR_ATTR],al
        mov     ax,word ptr [dta+16h]
        mov     word ptr [dir_rec+DR_TIME],ax
        mov     ax,word ptr [dta+18h]
        mov     word ptr [dir_rec+DR_DATE],ax
        mov     eax,dword ptr [dta+1Ah]
        mov     dword ptr [dir_rec+DR_SIZE],eax
        ret

;---------------------------------------------------------------------
; dir_want - CF if dir_rec's attributes keep it out of the listing.
;
; No /A: hidden and system entries stay hidden.  /A alone: everything.
; /A:list: every attribute named must be set and every -attribute
; clear, which is how /A:D lists directories and /A:-D everything else.
; /B never lists . and .., which are not files anyone can name.
;---------------------------------------------------------------------
dir_want:
        mov     al,[dir_rec+DR_ATTR]
        test    word ptr [dir_flags],DRF_B
        jz      dw2_attrs
        cmp     byte ptr [dir_rec+DR_NAME],'.'
        je      dw2_no
dw2_attrs:
        cmp     byte ptr [dir_aset],0
        jne     dw2_set
        test    al,06h
        jnz     dw2_no
        clc
        ret
dw2_set:
        mov     ah,al
        and     ah,[dir_areq]
        cmp     ah,[dir_areq]
        jne     dw2_no
        test    al,[dir_aexc]
        jnz     dw2_no
        clc
        ret
dw2_no:
        stc
        ret

;---------------------------------------------------------------------
; dir_show - one entry (SI -> its record), in the form the switches ask
;---------------------------------------------------------------------
dir_show:
        test    word ptr [dir_flags],DRF_B
        jnz     dsh_bare
        test    word ptr [dir_flags],DRF_W
        jnz     dsh_wide
        mov     di,OFFSET dir_line      ; the name columns, 8 + 1 + 3
        mov     cx,8
        push    si
        rep     movsb
        mov     al,' '
        stosb
        mov     cx,3
        rep     movsb
        pop     si
        mov     byte ptr [di],0
        push    si
        mov     si,OFFSET dir_line
        call    dir_lower
        call    puts
        pop     si
        test    byte ptr [si+DR_ATTR],10h
        jz      dsh_size
        push    si
        mov     si,OFFSET msg_isdir
        call    puts
        pop     si
        jmp     short dsh_date
dsh_size:
        mov     eax,[si+DR_SIZE]
        mov     bx,14
        call    put_num
        mov     al,' '
        call    putc
dsh_date:
        mov     ax,[si+DR_DATE]
        mov     cx,ax
        shr     cx,9
        add     cx,1980
        mov     dx,ax
        shr     dx,5
        and     dl,0Fh
        mov     dh,dl                   ; DH = month
        and     al,1Fh
        mov     dl,al                   ; DL = day
        push    si
        call    put_date
        pop     si
        mov     al,' '
        call    putc
        call    putc
        mov     ax,[si+DR_TIME]
        mov     cx,ax
        shr     cx,11                   ; hours
        shr     ax,5
        and     al,3Fh
        mov     [dsh_min],al
        mov     al,cl
        cmp     byte ptr [ctry_24h],0
        jne     dsh_24
        mov     byte ptr [dsh_ap],'a'
        cmp     al,12
        jb      dsh_am
        mov     byte ptr [dsh_ap],'p'
        sub     al,12
dsh_am:
        or      al,al
        jnz     dsh_hr
        mov     al,12
        jmp     short dsh_hr
dsh_24:
        mov     byte ptr [dsh_ap],0
        call    put_2d
        jmp     short dsh_mins
dsh_hr:
        cmp     al,10
        jae     dsh_h2
        push    ax
        mov     al,' '
        call    putc
        pop     ax
        add     al,'0'
        call    putc
        jmp     short dsh_mins
dsh_h2:
        call    put_2d
dsh_mins:
        mov     al,[ctry_tsep]
        call    putc
        mov     al,[dsh_min]
        call    put_2d
        mov     al,[dsh_ap]
        or      al,al
        jz      dsh_eol
        call    putc
dsh_eol:
        jmp     dir_endline

dsh_bare:
        test    word ptr [dir_flags],DRF_S
        jz      dsb_name
        push    si                      ; /S /B: the whole path
        mov     si,OFFSET dir_cur
        call    strlen
        mov     di,OFFSET dir_line
        rep     movsb
        pop     si
        call    rec_name
        jmp     short dsb_say
dsb_name:
        mov     di,OFFSET dir_line
        call    rec_name
dsb_say:
        mov     si,OFFSET dir_line
        call    dir_lower
        call    puts
        jmp     dir_endline

dsh_wide:
        ; DOS 6.22's own layout: a TAB between the columns, and each
        ; name padded to twelve - fourteen in brackets - so that the
        ; tab stops every eight columns line them up at sixteen.
        cmp     byte ptr [dir_col],0
        je      dsw_first
        mov     al,9
        call    putc
dsw_first:
        mov     di,OFFSET dir_line
        test    byte ptr [si+DR_ATTR],10h
        jz      dsw_nm
        mov     byte ptr [di],'['
        inc     di
dsw_nm:
        call    rec_name
        test    byte ptr [si+DR_ATTR],10h
        jz      dsw_say
        mov     word ptr [di],']'
        inc     di
dsw_say:
        mov     si,OFFSET dir_line
        call    dir_lower
        call    puts
        call    strlen
        neg     cx
        add     cx,12
        cmp     byte ptr [dir_line],'['
        jne     dsw_pad
        add     cx,2
dsw_pad:
        or      cx,cx
        jle     dsw_col
        call    putsp
dsw_col:
        inc     byte ptr [dir_col]
        cmp     byte ptr [dir_col],DIR_WCOLS
        jb      dsw_out2
        mov     byte ptr [dir_col],0
        call    dir_endline
dsw_out2:
        ret

; rec_name - the record at SI as "NAME.EXT" onto DI (DI left on the NUL)
rec_name:
        push    si
        mov     cx,8
rn_name:
        lodsb
        cmp     al,' '
        je      rn_nameend
        mov     [di],al
        inc     di
        loop    rn_name
rn_nameend:
        pop     si
        push    si
        add     si,DR_EXT
        cmp     byte ptr [si],' '
        je      rn_out
        mov     byte ptr [di],'.'
        inc     di
        mov     cx,3
rn_ext:
        lodsb
        cmp     al,' '
        je      rn_out
        mov     [di],al
        inc     di
        loop    rn_ext
rn_out:
        mov     byte ptr [di],0
        pop     si
        ret

; dir_lower - /L: the ASCIIZ at SI to lower case, in place
dir_lower:
        test    word ptr [dir_flags],DRF_L
        jz      dlw_out
        push    si
dlw_loop:
        mov     al,[si]
        or      al,al
        jz      dlw_done
        cmp     al,'A'
        jb      dlw_next
        cmp     al,'Z'
        ja      dlw_next
        add     al,20h
        mov     [si],al
dlw_next:
        inc     si
        jmp     dlw_loop
dlw_done:
        pop     si
dlw_out:
        ret

;---------------------------------------------------------------------
; dir_sort - [dir_n] records in the work block, into /O's order.
;
; Shell sort: in place, no second buffer, and fast enough for the three
; thousand entries 64K holds.
;---------------------------------------------------------------------
dir_sort:
        push    es
        mov     es,[work_seg]
        mov     ax,[dir_n]
        shr     ax,1
dso2_gap:
        or      ax,ax
        jz      dso2_done
        mov     [dir_gap],ax
        mov     cx,ax                   ; i = gap
dso2_i:
        cmp     cx,[dir_n]
        jae     dso2_next
        mov     bx,cx
        sub     bx,[dir_gap]            ; j = i - gap
dso2_j:
        imul    si,bx,DR_LEN
        mov     di,bx
        add     di,[dir_gap]
        imul    di,di,DR_LEN
        call    dir_cmp
        cmp     dl,1
        jne     dso2_inext
        call    rec_swap
        sub     bx,[dir_gap]
        jns     dso2_j
dso2_inext:
        inc     cx
        jmp     dso2_i
dso2_next:
        mov     ax,[dir_gap]
        shr     ax,1
        jmp     dso2_gap
dso2_done:
        pop     es
        ret

; rec_swap - exchange the records at ES:SI and ES:DI
rec_swap:
        push    cx
        push    si
        push    di
        mov     cx,DR_LEN
rsw_loop:
        mov     al,es:[si]
        xchg    al,es:[di]
        mov     es:[si],al
        inc     si
        inc     di
        loop    rsw_loop
        pop     di
        pop     si
        pop     cx
        ret

;---------------------------------------------------------------------
; dir_cmp - the records at ES:SI and ES:DI, by /O's keys in order and
;           the name last.  DL = 0FFh, 0 or 1 as the first sorts
;           before, level with, or after the second.
;---------------------------------------------------------------------
dir_cmp:
        push    ax
        push    bx
        push    cx
        xor     bx,bx
dcm_key:
        cmp     bl,[dir_nkeys]
        jae     dcm_last
        mov     al,[dir_keys+bx]
        inc     bx
        push    ax
        and     al,7Fh
        call    dcm_one
        pop     ax
        or      dl,dl
        jz      dcm_key
        test    al,80h
        jz      dcm_out
        neg     dl
        jmp     short dcm_out
dcm_last:
        mov     cx,11                   ; name then extension
        call    dcm_bytes
dcm_out:
        pop     cx
        pop     bx
        pop     ax
        ret

; dcm_one - one key, AL, into DL
dcm_one:
        cmp     al,'N'
        jne     dco_e
        mov     cx,8
        jmp     short dcm_bytes
dco_e:
        cmp     al,'E'
        jne     dco_s
        push    si
        push    di
        add     si,DR_EXT
        add     di,DR_EXT
        mov     cx,3
        call    dcm_bytes
        pop     di
        pop     si
        ret
dco_s:
        cmp     al,'S'
        jne     dco_d
        mov     eax,es:[si+DR_SIZE]
        cmp     eax,es:[di+DR_SIZE]
        jmp     short dcm_flags
dco_d:
        cmp     al,'D'
        jne     dco_g
        mov     ax,es:[si+DR_DATE]
        cmp     ax,es:[di+DR_DATE]
        jne     dcm_flags
        mov     ax,es:[si+DR_TIME]
        cmp     ax,es:[di+DR_TIME]
        jmp     short dcm_flags
dco_g:
        xor     dl,dl
        cmp     al,'G'
        jne     dco_out
        mov     al,es:[si+DR_ATTR]
        mov     ah,es:[di+DR_ATTR]
        and     ax,1010h
        cmp     al,ah
        je      dco_out
        mov     dl,0FFh                 ; a directory first
        or      al,al
        jnz     dco_out
        mov     dl,1
dco_out:
        ret
; dcm_flags - an unsigned CMP just done, as DL
dcm_flags:
        mov     dl,0
        je      dcf_out
        mov     dl,0FFh
        jb      dcf_out
        mov     dl,1
dcf_out:
        ret
; dcm_bytes - CX bytes at ES:SI against ES:DI, as DL
dcm_bytes:
        push    si
        push    di
dcb_loop:
        mov     al,es:[si]
        cmp     al,es:[di]
        jne     dcb_diff
        inc     si
        inc     di
        loop    dcb_loop
        xor     dl,dl
        jmp     short dcb_out
dcb_diff:
        call    dcm_flags
dcb_out:
        pop     di
        pop     si
        ret

;---------------------------------------------------------------------
; vol_header - DL = a drive (1 = A:, 0 = the current one):
;
;    Volume in drive C is MS-DOS_6
;    Volume Serial Number is 1A2B-3C4D
;
; The label is found as DOS kept it, a directory entry with the volume
; attribute; the serial number from 6900h, or 440Dh/0866h where there
; is no 6900h.  A disk with no serial number has no second line.
;---------------------------------------------------------------------
vol_header:
        or      dl,dl
        jnz     vh_have
        mov     ah,19h
        int     21h
        inc     al
        mov     dl,al
vh_have:
        mov     [vh_drv],dl
        mov     al,dl
        add     al,'A' - 1
        mov     [vh_spec],al
        mov     [vh_letter],al
        call    set_dta
        mov     dx,OFFSET vh_spec
        mov     cx,08h
        mov     ah,4Eh
        int     21h
        pushf
        mov     si,OFFSET msg_vol1
        call    puts
        mov     si,OFFSET vh_letter
        call    puts
        popf
        jc      vh_nolabel
        test    byte ptr [dta+15h],08h
        jz      vh_nolabel
        mov     si,OFFSET msg_volis
        call    puts
        ; the name without the '.' a search puts after eight characters,
        ; and without the blanks that pad it
        mov     si,OFFSET dta + 1Eh
        mov     di,OFFSET dir_line
vh_lcp:
        lodsb
        or      al,al
        jz      vh_lend
        cmp     al,'.'
        je      vh_lcp
        stosb
        jmp     vh_lcp
vh_lend:
        cmp     di,OFFSET dir_line
        je      vh_lput
        cmp     byte ptr [di-1],' '
        jne     vh_lput
        dec     di
        jmp     vh_lend
vh_lput:
        mov     byte ptr [di],0
        mov     si,OFFSET dir_line
        call    puts
        jmp     short vh_serial
vh_nolabel:
        mov     si,OFFSET msg_vnolabel
        call    puts
vh_serial:
        call    crlf
        mov     di,OFFSET vh_media
        mov     cx,26
        xor     al,al
        rep     stosb
        mov     bl,[vh_drv]
        xor     bh,bh
        mov     dx,OFFSET vh_media
        mov     ax,6900h
        int     21h
        jnc     vh_ser
        mov     bl,[vh_drv]
        xor     bh,bh
        mov     cx,0866h
        mov     dx,OFFSET vh_media
        mov     ax,440Dh
        int     21h
        jc      vh_out
vh_ser:
        mov     si,OFFSET msg_serial
        call    puts
        mov     ax,word ptr [vh_media+4]
        call    put_hex4
        mov     al,'-'
        call    putc
        mov     ax,word ptr [vh_media+2]
        call    put_hex4
        call    crlf
vh_out:
        ret

; put_hex4 - AX as four hex digits
put_hex4:
        push    cx
        mov     cx,4
ph4_loop:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph4_d
        add     al,7
ph4_d:
        call    putc
        pop     ax
        loop    ph4_loop
        pop     cx
        ret

;---------------------------------------------------------------------
; Data
;---------------------------------------------------------------------
msg_vol1    db  ' Volume in drive ',0
msg_volis   db  ' is ',0
msg_vnolabel db  ' has no label',0
msg_serial  db  ' Volume Serial Number is ',0
msg_dirof   db  ' Directory of ',0
msg_isdir   db  ' <DIR>         ',0
msg_files   db  ' file(s)',0
msg_bytes   db  ' bytes',0
msg_free    db  ' bytes free',0
msg_totlist db  'Total files listed:',0
msg_contin  db  '(continuing ',0
msg_toomany2 db 'Too many parameters - ',0
vh_spec     db  'A:\*.*',0
vh_letter   db  'A',0

dir_flags   dw  0
dir_argp    dw  0
dir_lines   dw  0
dir_hdrs    dw  0
dir_n       dw  0
dir_gap     dw  0
dir_tcount  dd  0
dir_tbytes  dd  0
dir_lcount  dd  0
dir_lbytes  dd  0
dir_aset    db  0
dir_areq    db  0
dir_aexc    db  0
dir_nkeys   db  0
dir_drv     db  0
dir_col     db  0
dir_depth   db  0
dir_lhdr    db  0
dir_fromenv db  0
dsh_min     db  0
dsh_ap      db  0
vh_drv      db  0
dir_keys    db  DIR_MAXKEYS + 1 dup(0)
dir_rec     db  DR_LEN dup(0)
fs_root     db  'A:\',0
fs_buf      db  44 dup(0)
vh_media    db  26 dup(0)
dir_word    db  PATH_MAX dup(0)
dir_spec    db  NAME_MAX dup(0)
dir_pat     db  16 dup(0)
dir_full    db  NAME_MAX + 16 dup(0)
dir_cur     db  NAME_MAX + 16 dup(0)
dir_srch    db  NAME_MAX + 16 dup(0)
dir_line    db  NAME_MAX + 32 dup(0)
dir_cmdtxt  db  LINE_MAX dup(0)
