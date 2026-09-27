;=====================================================================
; BUILTIN.INC - the internal commands that are one DOS call or a few:
; everything but the batch language (BATFILE.INC), DIR (LISTDIR.INC)
; and COPY (FILECOPY.INC).  Each is entered with SI -> its arguments,
; which start right after the command's name.
;=====================================================================

;---------------------------------------------------------------------
; on_off - SI -> the arguments of BREAK, VERIFY or ECHO.
;          CF clear: AL = 1 for ON, 0 for OFF, and nothing after it.
;          CF set:   AL = 0 if there was nothing at all, 1 otherwise.
;---------------------------------------------------------------------
on_off:
        call    skip_dl
        cmp     byte ptr [si],0
        je      oo2_none
        mov     di,OFFSET w_on
        call    word_is
        mov     al,1
        je      oo2_end
        mov     di,OFFSET w_off
        call    word_is
        mov     al,0
        je      oo2_end
oo2_other:
        mov     al,1
        stc
        ret
oo2_none:
        xor     al,al
        stc
        ret
oo2_end:
        push    ax
        call    skip_dl
        cmp     byte ptr [si],0
        pop     ax
        jne     oo2_other
        clc
        ret

; put_onoff - "on" or "off" for AL, and the line ends
put_onoff:
        mov     si,OFFSET w_onlc
        or      al,al
        jnz     pof_say
        mov     si,OFFSET w_offlc
pof_say:
        call    puts
        jmp     crlf

;---------------------------------------------------------------------
; BREAK [ON|OFF]  and  VERIFY [ON|OFF]
;---------------------------------------------------------------------
cmd_break:
        call    on_off
        jc      cbk_show
        mov     dl,al
        mov     ax,3301h
        int     21h
        ret
cbk_show:
        or      al,al
        jnz     onoff_err
        mov     ax,3300h
        int     21h
        mov     si,OFFSET msg_breakis
        call    puts
        mov     al,dl
        jmp     put_onoff
onoff_err:
        mov     si,OFFSET msg_onoff
        jmp     puts_err

cmd_verify:
        call    on_off
        jc      cvf_show
        xor     dl,dl
        mov     ah,2Eh
        int     21h
        ret
cvf_show:
        or      al,al
        jnz     onoff_err
        mov     ah,54h
        int     21h
        push    ax
        mov     si,OFFSET msg_verifyis
        call    puts
        pop     ax
        jmp     put_onoff

;---------------------------------------------------------------------
; ECHO [ON|OFF|message]
;
; The character after ECHO is a separator and is thrown away - exactly
; one of them, which is DOS's rule and the reason "ECHO." prints an
; empty line and "ECHO   x" keeps two of its three blanks.
;---------------------------------------------------------------------
cmd_echo:
        push    si
        call    on_off
        pop     si
        jc      ce2_text
        mov     [echo_on],al
        ret
ce2_text:
        or      al,al
        jnz     ce2_print
        ; nothing but blanks: the state
        mov     si,OFFSET msg_echois
        call    puts
        mov     al,[echo_on]
        jmp     put_onoff
ce2_print:
        cmp     byte ptr [si],0
        je      ce2_line
        inc     si
ce2_line:
        call    puts
        jmp     crlf

;---------------------------------------------------------------------
; REM - nothing.  (It was echoed already, if echoing was on.)
;---------------------------------------------------------------------
cmd_rem:
        ret

;---------------------------------------------------------------------
; PAUSE
;---------------------------------------------------------------------
cmd_pause:
        mov     si,OFFSET msg_pause
        call    puts
        call    get_key
        jmp     crlf

;---------------------------------------------------------------------
; EXIT
;---------------------------------------------------------------------
cmd_exit:
        jmp     exit_shell

;---------------------------------------------------------------------
; CD [path]  /  CHDIR [path]  /  CD X:
;---------------------------------------------------------------------
cmd_cd:
        call    skip_dl
        cmp     byte ptr [si],0
        jne     cd_arg
        xor     dl,dl                   ; the current drive's
        jmp     short cd_show
cd_arg:
        mov     di,OFFSET arg1
        call    get_arg
        ; "X:" and nothing else asks, rather than tells
        cmp     byte ptr [arg1+1],':'
        jne     cd_set
        cmp     byte ptr [arg1+2],0
        jne     cd_set
        mov     dl,[arg1]
        call    upcase_dl
        sub     dl,'A' - 1
        jmp     short cd_show
cd_set:
        mov     dx,OFFSET arg1
        mov     ah,3Bh
        int     21h
        jnc     cd_out
        mov     si,OFFSET msg_baddir
        call    puts_err
cd_out:
        ret

; cd_show - DL = the drive (0 = current): print its current directory
cd_show:
        mov     al,dl
        or      al,al
        jnz     cds_drv
        mov     ah,19h
        int     21h
        inc     al
cds_drv:
        mov     [cd_drv],al
        mov     dl,al
        mov     si,OFFSET pm_cwd
        mov     ah,47h
        int     21h
        jc      cds_bad
        mov     al,[cd_drv]
        add     al,'A' - 1
        call    putc
        mov     al,':'
        call    putc
        mov     al,'\'
        call    putc
        mov     si,OFFSET pm_cwd
        call    puts
        jmp     crlf
cds_bad:
        mov     si,OFFSET msg_baddrv
        jmp     puts_err

upcase_dl:
        xchg    al,dl
        call    upcase
        xchg    al,dl
        ret

;---------------------------------------------------------------------
; MD path / MKDIR path    and    RD path / RMDIR path
;---------------------------------------------------------------------
cmd_md:
        mov     di,OFFSET arg1
        call    get_arg
        jcxz    need_arg
        mov     dx,OFFSET arg1
        mov     ah,39h
        int     21h
        jnc     md_out
        mov     si,OFFSET msg_mdfail
        call    puts_err
md_out:
        ret

cmd_rd:
        mov     di,OFFSET arg1
        call    get_arg
        jcxz    need_arg
        mov     dx,OFFSET arg1
        mov     ah,3Ah
        int     21h
        jnc     rd_out
        mov     si,OFFSET msg_rdfail
        call    puts_err
rd_out:
        ret

need_arg:
        mov     si,OFFSET msg_reqparm
        jmp     puts_err

;---------------------------------------------------------------------
; CLS
;
; Through the BIOS when standard output is the screen, in the colour
; already under the cursor; as ESC [2J when it is anything else, which
; is what DOS sent - a file or a terminal gets a clear-screen it can
; act on, and nobody gets a BIOS call they cannot see.
;---------------------------------------------------------------------
cmd_cls:
        mov     ax,4400h
        mov     bx,1
        int     21h
        jc      cls_esc
        test    dl,80h                  ; a device...
        jz      cls_esc
        test    dl,03h                  ; ...that is the console
        jz      cls_esc
        mov     ah,0Fh
        int     10h                     ; AH = columns, BH = page
        mov     [cls_cols],ah
        mov     ah,08h
        int     10h                     ; AH = the attribute here
        mov     bh,ah
        push    es
        mov     ax,40h
        mov     es,ax
        mov     dh,es:[84h]             ; rows - 1, if the BIOS keeps it
        pop     es
        or      dh,dh
        jnz     cls_rows
        mov     dh,24
cls_rows:
        mov     dl,[cls_cols]
        dec     dl
        xor     cx,cx
        mov     ax,0600h
        int     10h
        mov     ah,0Fh
        int     10h
        xor     dx,dx
        mov     ah,02h
        int     10h
        ret
cls_esc:
        mov     si,OFFSET str_cls
        jmp     puts

;---------------------------------------------------------------------
; CTTY device
;---------------------------------------------------------------------
cmd_ctty:
        mov     di,OFFSET arg1
        call    get_arg
        jcxz    need_arg
        mov     dx,OFFSET arg1
        mov     ax,3D02h
        int     21h
        jc      ctty_bad
        mov     bx,ax
        mov     ax,4400h
        int     21h
        jc      ctty_close
        test    dl,80h
        jz      ctty_close              ; a file is not a terminal
        xor     cx,cx
ctty_dup:
        mov     ah,46h
        int     21h
        inc     cx
        cmp     cx,3
        jb      ctty_dup
        mov     ah,3Eh
        int     21h
        ret
ctty_close:
        mov     ah,3Eh
        int     21h
ctty_bad:
        mov     si,OFFSET msg_baddev
        jmp     puts_err

;---------------------------------------------------------------------
; SET [name=[value]]
;
; The name goes to upper case and the value does not, trailing blanks
; and all - DOS kept them, and batch files have depended on both.
;---------------------------------------------------------------------
cmd_set:
        call    skip_bl
        cmp     byte ptr [si],0
        je      set_list
        mov     di,OFFSET set_buf
        xor     cx,cx
set_name:
        lodsb
        or      al,al
        jz      set_syntax
        cmp     al,'='
        je      set_value
        call    upcase
        stosb
        inc     cx
        cmp     cx,PATH_MAX - 1
        jb      set_name
        jmp     short set_syntax
set_value:
        or      cx,cx
        jz      set_syntax              ; "SET =x"
        stosb                           ; the '='
set_vcp:
        lodsb
        stosb
        or      al,al
        jz      set_do
        cmp     di,OFFSET set_buf + LINE_MAX + PATH_MAX
        jb      set_vcp
        mov     byte ptr [di],0
set_do:
        mov     si,OFFSET set_buf
        jmp     env_set
set_syntax:
        mov     si,OFFSET msg_syntax
        jmp     puts_err

set_list:
        push    ds
        mov     ds,cs:[env_seg]
        xor     si,si
sl2_loop:
        cmp     byte ptr [si],0
        je      sl2_done
        call    strlen
        call    puts_n
        add     si,cx
        inc     si
        push    ds
        push    cs
        pop     ds
        call    crlf
        pop     ds
        jmp     sl2_loop
sl2_done:
        pop     ds
        ret

;---------------------------------------------------------------------
; PATH [[;] | dirs]
;---------------------------------------------------------------------
cmd_path:
        call    skip_bl
        cmp     byte ptr [si],'='
        jne     pth_1
        inc     si
        call    skip_bl
pth_1:
        cmp     byte ptr [si],0
        je      pth_show
        cmp     byte ptr [si],';'
        jne     pth_set
        push    si
        inc     si
        call    skip_bl
        cmp     byte ptr [si],0
        pop     si
        jne     pth_set
        mov     si,OFFSET name_path     ; "PATH ;": none at all
        jmp     env_del
pth_set:
        mov     di,OFFSET set_buf
        push    si
        mov     si,OFFSET lit_pathe     ; "PATH="
        call    strcpy
        pop     si
pth_cp:
        lodsb
        call    upcase                  ; DOS gave PATH in capitals
        mov     [di],al
        or      al,al
        jz      pth_trim
        inc     di
        cmp     di,OFFSET set_buf + LINE_MAX + PATH_MAX
        jb      pth_cp
        mov     byte ptr [di],0
pth_trim:
        cmp     di,OFFSET set_buf + 5
        jbe     pth_go
        cmp     byte ptr [di-1],' '
        jne     pth_go
        dec     di
        mov     byte ptr [di],0
        jmp     pth_trim
pth_go:
        mov     si,OFFSET set_buf
        jmp     env_set
pth_show:
        push    es
        mov     si,OFFSET name_path
        call    env_find
        jc      pth_none
        cmp     byte ptr es:[bx],0
        je      pth_none
        mov     si,OFFSET lit_pathe
        call    puts
        push    ds
        push    es
        pop     ds
        mov     si,bx
        call    puts
        pop     ds
        pop     es
        jmp     crlf
pth_none:
        pop     es
        mov     si,OFFSET msg_nopath
        call    puts
        jmp     crlf                    ; DOS 6.22 left a blank line here

;---------------------------------------------------------------------
; PROMPT [text]
;---------------------------------------------------------------------
cmd_prompt:
        call    skip_bl
        cmp     byte ptr [si],'='
        jne     prm_1
        inc     si
        call    skip_bl
prm_1:
        cmp     byte ptr [si],0
        jne     prm_set
        mov     si,OFFSET name_prompt
        jmp     env_del
prm_set:
        mov     di,OFFSET set_buf
        push    si
        mov     si,OFFSET lit_prompte   ; "PROMPT="
        call    strcpy
        pop     si
        call    strcpy
        mov     si,OFFSET set_buf
        jmp     env_set

;---------------------------------------------------------------------
; VER
;
; On PM-DOS the kernel is asked (21h/F1h AL=0) and its version is what
; is printed, as CMD.EXE prints it: the system's version, not a claim
; about this file.  Anywhere else - RMDOS under Windows, or a real DOS
; this was copied to - it says what it is and what it is running on.
; F1h is only asked when 30h's OEM byte says PM-DOS, because on
; another DOS an unknown function is not an error and leaves BX alone.
;---------------------------------------------------------------------
cmd_ver:
        call    crlf
        call    put_version
        call    crlf
        jmp     crlf

put_version:
        mov     ah,30h
        int     21h
        mov     [ver_dos],ax
        cmp     bh,03h                  ; PMDOS_OEM
        jne     pv_other
        mov     bx,0FFFFh
        mov     ax,0F100h
        int     21h
        jc      pv_other
        cmp     bx,0FFFFh
        je      pv_other
        push    bx
        mov     si,OFFSET msg_pmver
        call    puts
        pop     bx
        movzx   eax,bh
        call    put_dec
        mov     al,'.'
        call    putc
        mov     al,bl
        jmp     put_2d
pv_other:
        mov     si,OFFSET msg_c16ver
        call    puts
        mov     eax,PMDOS_MAJOR
        call    put_dec
        mov     al,'.'
        call    putc
        mov     al,PMDOS_MINOR
        call    put_2d
        mov     si,OFFSET msg_runson
        call    puts
        movzx   eax,byte ptr [ver_dos]
        call    put_dec
        mov     al,'.'
        call    putc
        mov     al,byte ptr [ver_dos+1]
        jmp     put_2d

;---------------------------------------------------------------------
; VOL [X:]
;---------------------------------------------------------------------
cmd_vol:
        call    skip_dl
        xor     dl,dl
        cmp     byte ptr [si],0
        je      vol_go
        cmp     byte ptr [si+1],':'
        jne     vol_bad
        mov     dl,[si]
        call    upcase_dl
        sub     dl,'A' - 1
vol_go:
        call    call_crlf_vol
        ret
vol_bad:
        mov     si,OFFSET msg_baddrv
        jmp     puts_err

call_crlf_vol:
        call    crlf
        jmp     vol_header              ; LISTDIR.INC

;---------------------------------------------------------------------
; TYPE file
;
; To the first ^Z, which is where DOS's TYPE stopped: it is an ASCII
; command, and the ^Z is the end of an ASCII file.
;---------------------------------------------------------------------
cmd_type:
        mov     di,OFFSET arg1
        call    get_arg
        or      cx,cx
        jz      type_need
        mov     si,OFFSET arg1
        call    has_wild
        jc      type_nf
        mov     dx,OFFSET arg1
        mov     ax,3D00h
        int     21h
        jc      type_nf
        mov     [h_src],ax
        mov     bx,800h                 ; 32K, or whatever there is
        call    work_big
        jc      type_close
type_rd:
        mov     bx,[h_src]
        mov     cx,[work_len]
        push    ds
        mov     ds,cs:[work_seg]
        xor     dx,dx
        mov     ah,3Fh
        int     21h
        pop     ds
        jc      type_done
        or      ax,ax
        jz      type_done
        mov     cx,ax
        ; to the ^Z, if there is one in this piece
        push    es
        mov     es,[work_seg]
        xor     di,di
        mov     al,1Ah
        mov     bx,cx
        repne   scasb
        pop     es
        jne     type_whole
        mov     cx,di
        dec     cx                      ; up to, not including, the ^Z
        mov     byte ptr [type_eof],1
        jmp     short type_wr
type_whole:
        mov     cx,bx
        mov     byte ptr [type_eof],0
type_wr:
        push    ds
        mov     ds,cs:[work_seg]
        xor     dx,dx
        mov     bx,1
        mov     ah,40h
        int     21h
        pop     ds
        cmp     byte ptr [type_eof],0
        je      type_rd
type_done:
        call    work_free
type_close:
        mov     bx,[h_src]
        mov     ah,3Eh
        int     21h
        mov     word ptr [h_src],-1
        ret
type_need:
        jmp     need_arg
type_nf:
        mov     si,OFFSET msg_notfound
        jmp     puts_err

;---------------------------------------------------------------------
; work_big - a work block of up to BX paragraphs, or the largest there
;            is if that is less (but at least 32).  [work_len] = its
;            size in bytes, rounded down to 512 so a read never
;            straddles a sector for no reason.
;---------------------------------------------------------------------
work_big:
        push    ax
        push    bx
        call    work_free
        mov     ah,48h
        int     21h
        jnc     wb_got
        cmp     bx,32
        jb      wb_fail
        mov     ah,48h
        int     21h
        jc      wb_fail
wb_got:
        mov     [work_seg],ax
        cmp     bx,0FFFh
        jbe     wb_len
        mov     bx,0FFFh
wb_len:
        shl     bx,4
        and     bx,0FE00h
        mov     [work_len],bx
        pop     bx
        pop     ax
        clc
        ret
wb_fail:
        mov     si,OFFSET msg_nomem
        call    puts_err
        pop     bx
        pop     ax
        stc
        ret

;---------------------------------------------------------------------
; TRUENAME [path]
;---------------------------------------------------------------------
cmd_truename:
        mov     di,OFFSET arg1
        call    get_arg
        or      cx,cx
        jnz     tn_go
        mov     word ptr [arg1],'.'     ; no argument: where we are
tn_go:
        mov     si,OFFSET arg1
        mov     di,OFFSET arg2
        mov     ah,60h
        int     21h
        jc      tn_bad
        mov     si,OFFSET arg2
        call    puts
        jmp     crlf
tn_bad:
        mov     si,OFFSET msg_badpath
        jmp     puts_err

;---------------------------------------------------------------------
; CHCP [nnn]
;---------------------------------------------------------------------
cmd_chcp:
        call    skip_dl
        cmp     byte ptr [si],0
        jne     chcp_set
        mov     ax,6601h
        int     21h
        jc      chcp_bad
        push    bx
        mov     si,OFFSET msg_codepage
        call    puts
        pop     ax
        movzx   eax,ax
        call    put_dec
        jmp     crlf
chcp_set:
        call    get_num
        jc      chcp_bad
        mov     bx,ax
        mov     ax,6602h
        int     21h
        jnc     chcp_out
chcp_bad:
        mov     si,OFFSET msg_badcp
        call    puts_err
chcp_out:
        ret

; get_num - a decimal number at SI into AX; CF if there are no digits
get_num:
        xor     ax,ax
        xor     cx,cx
gn_loop:
        mov     dl,[si]
        sub     dl,'0'
        cmp     dl,9
        ja      gn_end
        inc     si
        inc     cx
        push    dx
        mov     dx,10
        mul     dx
        pop     dx
        xor     dh,dh
        add     ax,dx
        jmp     gn_loop
gn_end:
        or      cx,cx
        jnz     gn_ok
        stc
        ret
gn_ok:
        clc
        ret

;---------------------------------------------------------------------
; LH [/L:...] [/S] program  /  LOADHIGH
;
; Upper memory first, conventional if it is full: link the UMBs in and
; ask for "first fit, high then low", run the program, and put both
; settings back.  /L and /S place a program in a chosen region; PM-DOS
; has one UMB pool, so they are accepted and do nothing more.
;---------------------------------------------------------------------
cmd_lh:
        call    skip_dl
        cmp     byte ptr [si],'/'
        jne     lh_prog
lh_sw:
        inc     si
lh_swskip:
        mov     al,[si]
        or      al,al
        jz      lh_prog
        call    is_dl
        je      cmd_lh
        cmp     al,'/'
        je      cmd_lh
        inc     si
        jmp     lh_swskip
lh_prog:
        call    skip_dl
        cmp     byte ptr [si],0
        jne     lh_run
        jmp     need_arg
lh_run:
        push    si
        mov     ax,5800h
        int     21h
        mov     [lh_strat],ax
        mov     ax,5802h
        int     21h
        mov     [lh_link],al
        mov     ax,5803h
        mov     bx,1
        int     21h
        mov     ax,5801h
        mov     bx,80h
        int     21h
        pop     si
        call    dispatch
        mov     ax,5801h
        mov     bx,[lh_strat]
        int     21h
        mov     ax,5803h
        movzx   bx,byte ptr [lh_link]
        int     21h
        ret

;---------------------------------------------------------------------
; DATE [date]  and  TIME [time]
;---------------------------------------------------------------------
cmd_date:
        call    skip_dl
        cmp     byte ptr [si],0
        jne     date_try
        mov     si,OFFSET msg_curdate
        call    puts
        call    put_date_now
        call    crlf
date_ask:
        mov     si,OFFSET msg_newdate
        call    puts
        mov     si,[date_fmts]
        call    put_datefmt
        mov     si,OFFSET str_colsp
        call    puts
        call    read_short
        call    skip_dl
        cmp     byte ptr [si],0
        je      date_out
date_try:
        call    parse_date
        jnc     date_out
        mov     si,OFFSET msg_baddate
        call    puts_err
        jmp     date_ask
date_out:
        ret

cmd_time:
        call    skip_dl
        cmp     byte ptr [si],0
        jne     time_try
        mov     si,OFFSET msg_curtime
        call    puts
        call    put_time_12
        call    crlf
time_ask:
        mov     si,OFFSET msg_newtime
        call    puts
        call    read_short
        call    skip_dl
        cmp     byte ptr [si],0
        je      time_out
time_try:
        call    parse_time
        jnc     time_out
        mov     si,OFFSET msg_badtime
        call    puts_err
        jmp     time_ask
time_out:
        ret

; read_short - a line of up to 30 characters from the keyboard; SI -> it
read_short:
        mov     byte ptr [short_buf],30
        mov     byte ptr [short_buf+1],0
        mov     dx,OFFSET short_buf
        mov     ah,0Ah
        int     21h
        call    crlf
        movzx   bx,byte ptr [short_buf+1]
        mov     byte ptr [short_buf+2+bx],0
        mov     si,OFFSET short_buf+2
        ret

; put_datefmt - "mm-dd-yy" in the country's order and separator
put_datefmt:
        mov     cx,3
pdf_loop:
        lodsw                           ; two letters
        push    ax
        call    putc
        pop     ax
        mov     al,ah
        call    putc
        dec     cx
        jz      pdf_out
        mov     al,[ctry_dsep]
        call    putc
        jmp     pdf_loop
pdf_out:
        ret

;---------------------------------------------------------------------
; put_date_now - "Fri 09-25-2026": the day and the date, in the
;                country's order, with a four-digit year.
; put_date     - CX year, DH month, DL day; [pd_year4] says how wide.
;---------------------------------------------------------------------
put_date_now:
        mov     ah,2Ah
        int     21h
        push    dx
        push    cx
        movzx   bx,al
        shl     bx,2
        lea     si,[day_names+bx]
        call    puts
        mov     al,' '
        call    putc
        pop     cx
        pop     dx
        mov     byte ptr [pd_year4],1
        ; fall into put_date
put_date:
        mov     [pd_y],cx
        mov     [pd_m],dh
        mov     [pd_d],dl
        mov     ax,[ctry_date]
        cmp     ax,1
        je      pd_dmy
        cmp     ax,2
        je      pd_ymd
        mov     al,[pd_m]               ; USA: m-d-y
        call    put_2d
        call    pd_sep
        mov     al,[pd_d]
        call    put_2d
        call    pd_sep
        jmp     pd_year
pd_dmy:
        mov     al,[pd_d]
        call    put_2d
        call    pd_sep
        mov     al,[pd_m]
        call    put_2d
        call    pd_sep
        jmp     pd_year
pd_ymd:
        call    pd_year
        call    pd_sep
        mov     al,[pd_m]
        call    put_2d
        call    pd_sep
        mov     al,[pd_d]
        jmp     put_2d
pd_sep:
        mov     al,[ctry_dsep]
        jmp     putc
pd_year:
        mov     ax,[pd_y]
        cmp     byte ptr [pd_year4],0
        je      pdy_2
        movzx   eax,ax
        jmp     put_dec
pdy_2:
        xor     dx,dx
        mov     bx,100
        div     bx
        mov     al,dl
        jmp     put_2d

;---------------------------------------------------------------------
; put_time_12 - " 4:39:05.77p" the way TIME shows it: twelve-hour with
;               a/p unless the country keeps a 24-hour clock.
; put_time_now - "16:39:05.77", which is PROMPT's $T.
;---------------------------------------------------------------------
put_time_12:
        mov     ah,2Ch
        int     21h
        mov     byte ptr [pt_ap],0
        cmp     byte ptr [ctry_24h],0
        jne     pt_any
        mov     byte ptr [pt_ap],'a'
        cmp     ch,12
        jb      pt_am
        mov     byte ptr [pt_ap],'p'
        sub     ch,12
pt_am:
        or      ch,ch
        jnz     pt_any
        mov     ch,12
        jmp     short pt_any
put_time_now:
        mov     ah,2Ch
        int     21h
        mov     byte ptr [pt_ap],0
pt_any:
        push    dx
        mov     al,ch
        cmp     al,10
        jae     pt_h2
        push    ax
        mov     al,' '
        call    putc
        pop     ax
        add     al,'0'
        call    putc
        jmp     short pt_m
pt_h2:
        call    put_2d
pt_m:
        mov     al,[ctry_tsep]
        call    putc
        mov     al,cl
        call    put_2d
        mov     al,[ctry_tsep]
        call    putc
        pop     dx
        mov     al,dh
        call    put_2d
        mov     al,[ctry_dec]
        call    putc
        mov     al,dl
        call    put_2d
        mov     al,[pt_ap]
        or      al,al
        jz      pt_out
        call    putc
pt_out:
        ret

;---------------------------------------------------------------------
; parse_date - SI -> a date in the country's order, with '-', '/' or
;              '.' (or the country's own) between the parts.  Set it.
;              CF if it is not one.  Two-digit years are 1980-2079.
;---------------------------------------------------------------------
parse_date:
        call    get_num
        jc      pdt_bad
        mov     [pdn_a],ax
        call    date_sep
        jc      pdt_bad
        call    get_num
        jc      pdt_bad
        mov     [pdn_b],ax
        call    date_sep
        jc      pdt_bad
        call    get_num
        jc      pdt_bad
        mov     [pdn_c],ax
        call    skip_dl
        cmp     byte ptr [si],0
        jne     pdt_bad
        ; in m, d, y
        mov     ax,[ctry_date]
        cmp     ax,1
        je      pdt_dmy
        cmp     ax,2
        je      pdt_ymd
        mov     dh,byte ptr [pdn_a]
        mov     dl,byte ptr [pdn_b]
        mov     cx,[pdn_c]
        jmp     short pdt_year
pdt_dmy:
        mov     dl,byte ptr [pdn_a]
        mov     dh,byte ptr [pdn_b]
        mov     cx,[pdn_c]
        jmp     short pdt_year
pdt_ymd:
        mov     cx,[pdn_a]
        mov     dh,byte ptr [pdn_b]
        mov     dl,byte ptr [pdn_c]
pdt_year:
        cmp     cx,100
        jae     pdt_set
        add     cx,1900
        cmp     cx,1980
        jae     pdt_set
        add     cx,100
pdt_set:
        mov     ah,2Bh
        int     21h
        or      al,al
        jnz     pdt_bad
        clc
        ret
pdt_bad:
        stc
        ret

date_sep:
        mov     al,[si]
        cmp     al,'-'
        je      ds_ok
        cmp     al,'/'
        je      ds_ok
        cmp     al,'.'
        je      ds_ok
        cmp     al,[ctry_dsep]
        je      ds_ok
        stc
        ret
ds_ok:
        inc     si
        clc
        ret

;---------------------------------------------------------------------
; parse_time - SI -> h[:m[:s[.cc]]][a|p].  Set it; CF if it is not one.
;---------------------------------------------------------------------
parse_time:
        mov     word ptr [ptn_h],0
        mov     word ptr [ptn_m],0
        mov     word ptr [ptn_s],0
        mov     word ptr [ptn_c],0
        call    get_num
        jc      ptm_bad
        mov     [ptn_h],ax
        call    time_sep
        jc      ptm_ap
        call    get_num
        jc      ptm_bad
        mov     [ptn_m],ax
        call    time_sep
        jc      ptm_ap
        call    get_num
        jc      ptm_bad
        mov     [ptn_s],ax
        mov     al,[si]
        cmp     al,'.'
        je      ptm_cs
        cmp     al,[ctry_dec]
        jne     ptm_ap
ptm_cs:
        inc     si
        call    get_num
        jc      ptm_bad
        mov     [ptn_c],ax
ptm_ap:
        call    skip_bl
        mov     al,[si]
        call    upcase
        cmp     al,'A'
        je      ptm_am
        cmp     al,'P'
        jne     ptm_end
        cmp     word ptr [ptn_h],12
        jae     ptm_skipm
        add     word ptr [ptn_h],12
        jmp     short ptm_skipm
ptm_am:
        cmp     word ptr [ptn_h],12
        jne     ptm_skipm
        mov     word ptr [ptn_h],0
ptm_skipm:
        inc     si
        mov     al,[si]
        call    upcase
        cmp     al,'M'
        jne     ptm_end
        inc     si
ptm_end:
        call    skip_dl
        cmp     byte ptr [si],0
        jne     ptm_bad
        mov     ch,byte ptr [ptn_h]
        mov     cl,byte ptr [ptn_m]
        mov     dh,byte ptr [ptn_s]
        mov     dl,byte ptr [ptn_c]
        mov     ah,2Dh
        int     21h
        or      al,al
        jnz     ptm_bad
        clc
        ret
ptm_bad:
        stc
        ret

time_sep:
        mov     al,[si]
        cmp     al,':'
        je      ts_ok
        cmp     al,[ctry_tsep]
        je      ts_ok
        stc
        ret
ts_ok:
        inc     si
        clc
        ret

;---------------------------------------------------------------------
; DEL / ERASE filespec [/P]
;
; A directory means everything in it, as it did in DOS 6.22, and
; everything in a directory - *.* - is asked about first.  Files are
; deleted as they are found: DOS's own search carries on correctly
; past an entry that has just been removed.
;---------------------------------------------------------------------
cmd_del:
        mov     byte ptr [del_ask],0
        mov     byte ptr [arg1],0
del_args:
        mov     di,OFFSET arg2
        call    get_arg
        jcxz    del_argsdone
        cmp     byte ptr [arg2],'/'
        jne     del_spec
        mov     al,[arg2+1]
        call    upcase
        cmp     al,'P'
        jne     del_badsw
        cmp     byte ptr [arg2+2],0
        jne     del_badsw
        mov     byte ptr [del_ask],1
        jmp     del_args
del_spec:
        cmp     byte ptr [arg1],0
        jne     del_toomany
        push    si
        mov     si,OFFSET arg2
        mov     di,OFFSET arg1
        call    strcpy
        pop     si
        jmp     del_args
del_badsw:
        mov     si,OFFSET msg_badsw
        call    puts_err
        mov     si,OFFSET arg2
        call    puts_err
        jmp     crlf_err
del_toomany:
        mov     si,OFFSET msg_toomany
        jmp     puts_err
del_argsdone:
        cmp     byte ptr [arg1],0
        jne     del_have
        jmp     need_arg
del_have:
        mov     si,OFFSET arg1
        call    dir_to_spec             ; a directory -> dir\*.*
        ; *.* is everything, and asked about
        mov     si,OFFSET arg1
        call    name_part
        mov     si,di
        mov     di,OFFSET pat_all
        call    stricmp
        jne     del_go
        cmp     byte ptr [del_ask],0
        jne     del_go                  ; /P asks about each anyway
        mov     si,OFFSET msg_delall
        call    ask_yn
        cmp     al,'Y'
        jne     del_out
del_go:
        mov     si,OFFSET arg1
        call    name_part
        mov     [del_namep],di          ; the directory part ends here
        mov     byte ptr [del_found],0
        call    set_dta
        mov     dx,OFFSET arg1
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        jc      del_none
del_each:
        mov     byte ptr [del_found],1
        ; the full name: the directory part, then what was found
        mov     si,OFFSET arg1
        mov     di,OFFSET arg2
del_dir:
        cmp     si,[del_namep]
        je      del_dirdone
        movsb
        jmp     del_dir
del_dirdone:
        mov     si,OFFSET dta + 1Eh
        call    strcpy
        cmp     byte ptr [del_ask],0
        je      del_now
        mov     si,OFFSET arg2
        call    puts
        mov     si,OFFSET msg_delq
        call    ask_yn
        cmp     al,'Y'
        jne     del_next
del_now:
        mov     dx,OFFSET arg2
        mov     ah,41h
        int     21h
        jnc     del_next
        mov     si,OFFSET msg_denied
        call    puts_err
del_next:
        call    set_dta
        mov     ah,4Fh
        int     21h
        jnc     del_each
del_none:
        cmp     byte ptr [del_found],0
        jne     del_out
        mov     si,OFFSET msg_notfound
        call    puts_err
del_out:
        ret

;---------------------------------------------------------------------
; dir_to_spec - the path at SI names a directory (it is one, or it
;               ends in '\' or ':', or its last part is . or ..): add
;               *.* to it, in place.  Room for it is the caller's.
;---------------------------------------------------------------------
dir_to_spec:
        push    si
        call    strlen
        mov     di,si
        add     di,cx                   ; DI -> the NUL
        or      cx,cx
        jz      dts_add                 ; nothing: everything here
        mov     al,[di-1]
        cmp     al,'\'
        je      dts_add
        cmp     al,':'
        je      dts_add
        call    has_wild
        jc      dts_out
        ; "." or ".." as the last part
        push    di
        call    name_part
        cmp     byte ptr [di],'.'
        jne     dts_attr
        cmp     byte ptr [di+1],0
        je      dts_dotdir
        cmp     byte ptr [di+1],'.'
        jne     dts_attr
        cmp     byte ptr [di+2],0
        je      dts_dotdir
dts_attr:
        pop     di
        push    di
        mov     dx,si
        mov     ax,4300h
        int     21h
        pop     di
        jc      dts_out
        test    cl,10h
        jz      dts_out
        jmp     short dts_slash
dts_dotdir:
        pop     di
dts_slash:
        mov     byte ptr [di],'\'
        inc     di
dts_add:
        push    si
        mov     si,OFFSET pat_all
        call    strcpy
        pop     si
dts_out:
        pop     si
        ret

;---------------------------------------------------------------------
; REN / RENAME old new
;
; Wildcards on both sides, with DOS's rules for what a '?' or a '*' in
; the new name takes from the old (apply_pattern), and directories as
; well as files - which DOS 6.22's REN refused and its MOVE did not.
; The new name is a NAME: a path in it is refused, as DOS refused it,
; because REN renames in place and never moves.
;
; The matches are collected before any of them is renamed.  Renaming
; while the search is still open would show the search a name it has
; not reached yet, and REN *.TXT *.TX would then see its own work.
;---------------------------------------------------------------------
cmd_ren:
        mov     di,OFFSET arg1
        call    get_arg
        jcxz    ren_need
        mov     di,OFFSET arg2
        call    get_arg
        jcxz    ren_need
        push    si
        mov     di,OFFSET arg3
        call    get_arg
        pop     si
        or      cx,cx
        jz      ren_2args
        mov     si,OFFSET msg_toomany
        jmp     puts_err
ren_need:
        jmp     need_arg
ren_2args:
        mov     si,OFFSET arg2
ren_chk:
        lodsb
        or      al,al
        jz      ren_ok
        cmp     al,'\'
        je      ren_badparm
        cmp     al,':'
        jne     ren_chk
ren_badparm:
        mov     si,OFFSET msg_badparm
        jmp     puts_err
ren_ok:
        mov     si,OFFSET arg1
        call    name_part
        mov     [del_namep],di
        ; collect: 13 bytes a name, in a work block
        mov     bx,1000h
        call    work_big
        jc      ren_out
        xor     ax,ax
        mov     [ren_count],ax
        call    set_dta
        mov     dx,OFFSET arg1
        mov     cx,10h                  ; directories too
        mov     ah,4Eh
        int     21h
        jc      ren_none
ren_collect:
        mov     si,OFFSET dta + 1Eh
        cmp     byte ptr [si],'.'       ; never . or ..
        je      ren_cnext
        mov     ax,[ren_count]
        mov     cx,13
        mul     cx
        add     ax,13
        cmp     ax,[work_len]
        ja      ren_cnext               ; full: the rest are not done
        sub     ax,13
        mov     di,ax
        push    es
        mov     es,[work_seg]
        mov     cx,13
        rep     movsb
        pop     es
        inc     word ptr [ren_count]
ren_cnext:
        mov     ah,4Fh
        int     21h
        jnc     ren_collect
        cmp     word ptr [ren_count],0
        je      ren_none
        ; now rename them
        mov     byte ptr [ren_err],0
        xor     bx,bx
ren_each:
        cmp     bx,[ren_count]
        jae     ren_done
        push    bx
        mov     ax,13
        mul     bx
        mov     si,ax
        push    ds
        mov     ds,cs:[work_seg]
        mov     di,OFFSET ren_old
        mov     cx,13
        rep     movsb
        pop     ds
        ; the old name and the new one, both with the directory part
        mov     si,OFFSET arg1
        mov     di,OFFSET arg3
        call    ren_dirpart
        mov     si,OFFSET ren_old
        call    strcpy
        mov     si,OFFSET arg1
        mov     di,OFFSET arg4
        call    ren_dirpart
        mov     si,OFFSET ren_old
        mov     bx,OFFSET arg2
        call    apply_pattern           ; onto DI
        mov     dx,OFFSET arg3
        mov     di,OFFSET arg4
        mov     ah,56h
        int     21h
        pop     bx
        jnc     ren_next
        mov     byte ptr [ren_err],1
ren_next:
        inc     bx
        jmp     ren_each
ren_done:
        call    work_free
        cmp     byte ptr [ren_err],0
        je      ren_out
        jmp     short ren_dup
ren_none:
        call    work_free
ren_dup:
        mov     si,OFFSET msg_dupfile
        call    puts_err
ren_out:
        ret

; ren_dirpart - copy SI up to [del_namep] onto DI (DI left after it)
ren_dirpart:
        cmp     si,[del_namep]
        je      rdp_out
        movsb
        jmp     ren_dirpart
rdp_out:
        mov     byte ptr [di],0
        ret

;---------------------------------------------------------------------
; apply_pattern - SI -> a name ("NAME.EXT"), BX -> a pattern for it,
;                 DI -> where the result goes (DI left on its NUL).
;
; DOS's rule, one field at a time - the name, then the extension: a
; '?' in the pattern takes the character in the same place in the
; source, a '*' takes the rest of the source's field, and anything
; else is itself.  So *.BAK keeps the name and changes the extension,
; and A?C.* keeps the second letter.
;---------------------------------------------------------------------
apply_pattern:
        push    ax
        push    bx
        push    cx
        push    si
        ; the two fields of the source: name at SI, extension at [ap_ext]
        push    si
ap_findext:
        mov     al,[si]
        or      al,al
        jz      ap_noext
        inc     si
        cmp     al,'.'
        jne     ap_findext
        mov     [ap_ext],si
        jmp     short ap_gotext
ap_noext:
        mov     [ap_ext],si             ; the NUL: an empty extension
ap_gotext:
        pop     si
        mov     [ap_out],di
        ; the name field
        call    ap_field
        ; the pattern's extension, if it has one
        cmp     byte ptr [bx],'.'
        jne     ap_done
        inc     bx
        mov     byte ptr [di],'.'
        inc     di
        mov     [ap_dot],di
        mov     si,[ap_ext]
        call    ap_field
        cmp     di,[ap_dot]             ; nothing after the dot: no dot
        jne     ap_done
        dec     di
ap_done:
        mov     byte ptr [di],0
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

; ap_field - one field: pattern at BX (to '.' or NUL), source at SI
;            (to '.' or NUL), output at DI.  BX is left on the '.'.
ap_field:
        mov     al,[bx]
        or      al,al
        jz      apf_out
        cmp     al,'.'
        je      apf_out
        inc     bx
        cmp     al,'*'
        je      apf_star
        cmp     al,'?'
        je      apf_q
        mov     [di],al                 ; a literal
        inc     di
        call    apf_step
        jmp     ap_field
apf_q:
        call    apf_src                 ; the source's character, if any
        jmp     ap_field
apf_star:
        call    apf_src
        jnc     apf_star
        ; skip the rest of this field of the pattern
apf_skip:
        mov     al,[bx]
        or      al,al
        jz      apf_out
        cmp     al,'.'
        je      apf_out
        inc     bx
        jmp     apf_skip
apf_out:
        ret
; apf_src - copy the source character here, if the field has one; CF not
apf_src:
        mov     al,[si]
        or      al,al
        jz      apfs_none
        cmp     al,'.'
        je      apfs_none
        mov     [di],al
        inc     di
        inc     si
        clc
        ret
apfs_none:
        stc
        ret
; apf_step - a literal used up a place: the source moves past one too
apf_step:
        mov     al,[si]
        or      al,al
        jz      apst_out
        cmp     al,'.'
        je      apst_out
        inc     si
apst_out:
        ret

;---------------------------------------------------------------------
; Data
;---------------------------------------------------------------------
w_on        db  'ON',0
w_off       db  'OFF',0
w_onlc      db  'on',0
w_offlc     db  'off',0
msg_breakis db  'BREAK is ',0
msg_verifyis db 'VERIFY is ',0
msg_echois  db  'ECHO is ',0
msg_onoff   db  'Must specify ON or OFF',13,10,0
msg_pause   db  'Press any key to continue . . . ',0
msg_baddir  db  'Invalid directory',13,10,0
msg_mdfail  db  'Unable to create directory',13,10,0
msg_rdfail  db  'Invalid path, not directory,',13,10
            db  'or directory not empty',13,10,0
msg_baddev  db  'Invalid device',13,10,0
msg_nopath  db  'No Path',13,10,0
msg_badpath db  'Invalid path',13,10,0
msg_pmver   db  'PM-DOS version ',0
msg_c16ver  db  'PM-DOS COMMAND.COM version ',0
msg_runson  db  ', running on DOS ',0
msg_codepage db 'Active code page: ',0
msg_badcp   db  'Invalid code page',13,10,0
msg_curdate db  'Current date is ',0
msg_newdate db  'Enter new date (',0
msg_baddate db  'Invalid date',13,10,0
msg_curtime db  'Current time is ',0
msg_newtime db  'Enter new time: ',0
msg_badtime db  'Invalid time',13,10,0
msg_delall  db  'All files in directory will be deleted!',13,10
            db  'Are you sure (Y/N)? ',0
msg_delq    db  ',    Delete (Y/N)? ',0
msg_dupfile db  'Duplicate file name or file not found',13,10,0
str_colsp   db  '): ',0
str_cls     db  27,'[2J',0
lit_pathe   db  'PATH=',0
lit_prompte db  'PROMPT=',0
pat_all     db  '*.*',0
day_names   db  'Sun',0,'Mon',0,'Tue',0,'Wed',0,'Thu',0,'Fri',0,'Sat',0
date_fmts   dw  OFFSET dfmt_mdy
dfmt_mdy    db  'mmddyy'
dfmt_dmy    db  'ddmmyy'
dfmt_ymd    db  'yymmdd'

cls_cols    db  0
cd_drv      db  0
type_eof    db  0
del_ask     db  0
del_found   db  0
ren_err     db  0
lh_link     db  0
pd_year4    db  0
pd_m        db  0
pd_d        db  0
pt_ap       db  0
ver_dos     dw  0
lh_strat    dw  0
pd_y        dw  0
pdn_a       dw  0
pdn_b       dw  0
pdn_c       dw  0
ptn_h       dw  0
ptn_m       dw  0
ptn_s       dw  0
ptn_c       dw  0
work_len    dw  0
del_namep   dw  0
ren_count   dw  0
ap_ext      dw  0
ap_out      dw  0
ap_dot      dw  0
arg1        db  NAME_MAX dup(0)
arg2        db  NAME_MAX dup(0)
arg3        db  NAME_MAX dup(0)
arg4        db  NAME_MAX dup(0)
ren_old     db  14 dup(0)
set_buf     db  LINE_MAX + PATH_MAX + 2 dup(0)
short_buf   db  34 dup(0)
