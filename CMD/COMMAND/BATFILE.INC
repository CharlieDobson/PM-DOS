;=====================================================================
; BATFILE.INC - batch files: starting them, reading them a line at a
; time, %-expansion, and the commands that only mean something inside
; one - CALL, GOTO, SHIFT, IF and FOR.
;
; A batch file is read the way DOS read it: opened, positioned, one
; line taken, closed, for EVERY line.  That is slow and it is right -
; a batch file may be edited, replaced, or on a floppy that is taken
; out, while it runs, and DOS's answer to each of those ("Batch file
; missing", a changed file picked up at the next line) is the one
; batch files were written against.
;=====================================================================

;---------------------------------------------------------------------
; batch_start - prog_path is a .BAT to run; [verb_start] is the command
;               as it was typed, which becomes %0, %1 and the rest.
;
; From another batch file WITHOUT CALL it REPLACES the one running,
; which never comes back - DOS's rule, and the reason CALL exists.
;---------------------------------------------------------------------
batch_start:
        ; fully qualified, so that a CD inside the file cannot lose it
        mov     si,OFFSET prog_path
        mov     di,OFFSET bs_full
        mov     ah,60h
        int     21h
        jnc     bs_named
        mov     si,OFFSET prog_path
        mov     di,OFFSET bs_full
        call    strcpy
bs_named:
        cmp     word ptr [bat_depth],0
        je      bs_push
        cmp     byte ptr [call_flag],0
        je      bs_fill                 ; chain: the new one replaces it
bs_push:
        call    bat_push
        jc      bs_out
bs_fill:
        mov     si,OFFSET bs_full
        mov     di,OFFSET bat_cur + BCO_NAME
        call    strcpy
        mov     dword ptr [bat_cur + BCO_POS],0
        mov     word ptr [bat_cur + BCO_SHIFT],0
        mov     si,[verb_start]
        mov     di,OFFSET bat_cur + BCO_ARGS
        mov     cx,LINE_MAX
bs_args:
        lodsb
        or      al,al
        jz      bs_argsend
        stosb
        loop    bs_args
bs_argsend:
        mov     byte ptr [di],0
        mov     byte ptr [call_flag],0
bs_out:
        ret

;---------------------------------------------------------------------
; bat_push - make room for a new level: the running one, if there is
;            one, is parked in a block of its own.  CF if it cannot be.
; bat_pop  - the running level is finished: bring back the one that
;            CALLed it, or, if that was the last, end batch mode - and
;            ECHO goes back ON, which is what the prompt expects.
;---------------------------------------------------------------------
bat_push:
        mov     ax,[bat_depth]
        or      ax,ax
        jz      bp_first
        cmp     ax,BAT_MAX
        jae     bp_deep
        mov     bx,BCO_PARAS
        mov     ah,48h
        int     21h
        jc      bp_nomem
        mov     di,[bat_depth]
        dec     di
        shl     di,1
        mov     [bat_segs+di],ax
        push    es
        mov     es,ax
        xor     di,di
        mov     si,OFFSET bat_cur
        mov     cx,BCO_LEN
        rep     movsb
        pop     es
bp_first:
        inc     word ptr [bat_depth]
        clc
        ret
bp_deep:
        mov     si,OFFSET msg_batdeep
        call    puts_err
        stc
        ret
bp_nomem:
        mov     si,OFFSET msg_nomem
        call    puts_err
        stc
        ret

bat_pop:
        mov     ax,[bat_depth]
        or      ax,ax
        jz      bpp_out
        dec     ax
        mov     [bat_depth],ax
        jz      bpp_last
        mov     di,ax
        dec     di
        shl     di,1
        mov     ax,[bat_segs+di]
        push    ds
        push    es
        push    cs
        pop     es
        mov     ds,ax
        xor     si,si
        mov     di,OFFSET bat_cur
        mov     cx,BCO_LEN
        rep     movsb
        pop     es
        pop     ds
        push    es
        mov     es,ax
        mov     ah,49h
        int     21h
        pop     es
        ret
bpp_last:
        mov     byte ptr [echo_on],1
bpp_out:
        ret

bat_end_all:
        cmp     word ptr [bat_depth],0
        je      bea_out
        call    bat_pop
        jmp     bat_end_all
bea_out:
        ret

;---------------------------------------------------------------------
; bat_next - the running batch file's next line, expanded and echoed
;            as it should be.  get_command's contract: CF clear with
;            SI -> the line, CF set for nothing to run this time.
;
; A label is never run and never shown.  A leading @ keeps the line off
; the screen whatever ECHO says.  An echoed line is shown the way DOS
; showed it: a blank line, the prompt, the command.
;---------------------------------------------------------------------
bat_next:
        ; a ^C typed while a batch file runs is looked for here, as
        ; DOS looked for one between commands - 0Bh is the cheapest
        ; call that makes DOS check
        mov     ah,0Bh
        int     21h
        mov     byte ptr [bat_gone],0
        mov     eax,dword ptr [bat_cur + BCO_POS]
        call    bat_read
        jnc     bn_have
        cmp     byte ptr [bat_gone],0
        je      bn_eof
        mov     si,OFFSET msg_batmiss
        call    puts_err
        call    bat_end_all
        stc
        ret
bn_eof:
        call    bat_pop
        stc
        ret
bn_have:
        mov     dword ptr [bat_cur + BCO_POS],edx
        mov     si,OFFSET bat_line
        call    skip_bl
        cmp     byte ptr [si],':'
        je      bn_skip
        mov     si,OFFSET bat_line
        mov     di,OFFSET exp_line
        call    bat_expand
        mov     si,OFFSET exp_line
        call    skip_bl
        xor     dl,dl                   ; DL = 1: this line is silent
        cmp     byte ptr [si],'@'
        jne     bn_echo
        inc     si
        inc     dl
bn_echo:
        cmp     byte ptr [si],0
        je      bn_skip
        cmp     byte ptr [echo_on],0
        je      bn_run
        or      dl,dl
        jnz     bn_run
        push    si
        call    crlf
        call    show_prompt
        pop     si
        call    puts
        call    crlf
bn_run:
        clc
        ret
bn_skip:
        stc
        ret

;---------------------------------------------------------------------
; bat_read - EAX = a position in the running batch file.  The line
;            that starts there into bat_line, ASCIIZ, at most 127
;            characters.  EDX = where the line after it starts.  CF at
;            the end of the file - and [bat_gone] set too if the file
;            could not be opened at all.
;
; A line ends at CR, CR LF or a bare LF, so files written anywhere
; read the same; a ^Z is the end of the file wherever it is.
;---------------------------------------------------------------------
bat_read:
        mov     [br_pos],eax
        mov     dx,OFFSET bat_cur + BCO_NAME
        mov     ax,3D00h
        int     21h
        jnc     br_open
        mov     byte ptr [bat_gone],1
        stc
        ret
br_open:
        mov     bx,ax
        mov     [h_bat],ax
        mov     dx,word ptr [br_pos]
        mov     cx,word ptr [br_pos+2]
        mov     ax,4200h
        int     21h
        mov     dx,OFFSET bat_raw
        mov     cx,BATRAW_LEN
        mov     ah,3Fh
        int     21h
        pushf
        push    ax
        mov     ah,3Eh
        int     21h
        mov     word ptr [h_bat],-1
        pop     cx
        popf
        jc      br_eof
        jcxz    br_eof
        mov     si,OFFSET bat_raw
        mov     di,OFFSET bat_line
        xor     bx,bx                   ; bytes of the file used
br_scan:
        cmp     bx,cx
        jae     br_done                 ; no end in this piece: it is
        mov     al,[si+bx]              ; the last line, or too long
        cmp     al,1Ah
        je      br_ctlz
        cmp     al,0Dh
        je      br_cr
        cmp     al,0Ah
        je      br_lf
        inc     bx
        cmp     di,OFFSET bat_line + LINE_MAX - 1
        jae     br_scan                 ; too long: the rest is dropped
        mov     [di],al
        inc     di
        jmp     br_scan
br_ctlz:
        or      bx,bx
        jz      br_eof                  ; ^Z first thing: the end
        jmp     short br_done           ; the next read starts on it
br_cr:
        inc     bx
        cmp     bx,cx
        jae     br_done
        cmp     byte ptr [si+bx],0Ah
        jne     br_done
br_lf:
        inc     bx
br_done:
        mov     byte ptr [di],0
        movzx   edx,bx
        add     edx,[br_pos]
        clc
        ret
br_eof:
        stc
        ret

;---------------------------------------------------------------------
; bat_expand - the batch line at SI, with its % codes replaced, onto
;              DI (at most EXP_MAX characters).
;
;   %%       one %      - which is how FOR's %%F reaches FOR as %F
;   %0-%9    the arguments, counting from any SHIFT
;   %NAME%   the environment variable, or nothing if it is not set
;   % alone  dropped, which is what DOS did with "50%"
;---------------------------------------------------------------------
bat_expand:
        mov     [bx_end],di
        add     word ptr [bx_end],EXP_MAX
bx_loop:
        lodsb
        or      al,al
        jz      bx_done
        cmp     al,'%'
        je      bx_pct
bx_put:
        cmp     di,[bx_end]
        jae     bx_loop
        stosb
        jmp     bx_loop
bx_pct:
        mov     al,[si]
        or      al,al
        jz      bx_done                 ; a % at the very end: dropped
        cmp     al,'%'
        jne     bx_notpct
        inc     si
        jmp     bx_put
bx_notpct:
        cmp     al,'0'
        jb      bx_var
        cmp     al,'9'
        ja      bx_var
        inc     si
        sub     al,'0'
        call    bat_param
        jmp     bx_loop
bx_var:
        ; %NAME% - is there a closing % before the end of the line?
        mov     bx,si
bx_find:
        mov     al,[bx]
        or      al,al
        jz      bx_loop                 ; no: the % is dropped
        cmp     al,'%'
        je      bx_named
        inc     bx
        jmp     bx_find
bx_named:
        push    di
        mov     di,OFFSET env_name
        mov     cx,PATH_MAX - 1
bx_ncp:
        cmp     si,bx
        je      bx_nend
        lodsb
        call    upcase
        stosb
        loop    bx_ncp
bx_nend:
        mov     byte ptr [di],0
        pop     di
        mov     si,bx
        inc     si                      ; past the closing %
        push    si
        push    es
        push    di                      ; env_find answers in DI too
        mov     si,OFFSET env_name
        call    env_find
        pop     di
        jc      bx_novar
bx_vcp:
        mov     al,es:[bx]
        or      al,al
        jz      bx_novar
        inc     bx
        cmp     di,[bx_end]
        jae     bx_vcp
        push    es
        push    cs
        pop     es
        stosb
        pop     es
        jmp     bx_vcp
bx_novar:
        pop     es
        pop     si
        jmp     bx_loop
bx_done:
        mov     byte ptr [di],0
        ret

;---------------------------------------------------------------------
; bat_param - AL = 0-9: that argument of the running batch file, counted
;             from the current SHIFT, onto DI (not past [bx_end]).
;             Arguments are separated the way DOS separated them: by
;             blanks, commas, semicolons and equals signs.
;---------------------------------------------------------------------
bat_param:
        push    si
        movzx   bx,al
        add     bx,word ptr [bat_cur + BCO_SHIFT]
        mov     si,OFFSET bat_cur + BCO_ARGS
bpa_skip:
        call    skip_dl
        cmp     byte ptr [si],0
        je      bpa_out
        or      bx,bx
        jz      bpa_copy
bpa_over:
        mov     al,[si]
        or      al,al
        jz      bpa_out
        call    is_dl
        je      bpa_next
        inc     si
        jmp     bpa_over
bpa_next:
        dec     bx
        jmp     bpa_skip
bpa_copy:
        mov     al,[si]
        or      al,al
        jz      bpa_out
        call    is_dl
        je      bpa_out
        inc     si
        cmp     di,[bx_end]
        jae     bpa_copy
        stosb
        jmp     bpa_copy
bpa_out:
        pop     si
        ret

;---------------------------------------------------------------------
; CALL batchfile [args] - run it and come back.  Anything else CALLed
; just runs, which is what DOS did too.
;---------------------------------------------------------------------
cmd_call:
        call    skip_dl
        cmp     byte ptr [si],0
        je      call_out
        mov     byte ptr [call_flag],1
        call    dispatch
        mov     byte ptr [call_flag],0
call_out:
        ret

;---------------------------------------------------------------------
; SHIFT
;---------------------------------------------------------------------
cmd_shift:
        cmp     word ptr [bat_depth],0
        je      shf_out
        inc     word ptr [bat_cur + BCO_SHIFT]
shf_out:
        ret

;---------------------------------------------------------------------
; GOTO label
;
; From the top of the file, every time: the first line that is the
; label wins.  Eight characters of a label count, and case does not,
; which is DOS's rule.  Not found ends the batch file.
;---------------------------------------------------------------------
cmd_goto:
        cmp     word ptr [bat_depth],0
        je      gt_out
        call    skip_dl
        cmp     byte ptr [si],':'
        jne     gt_name
        inc     si
gt_name:
        mov     di,OFFSET gt_label
        mov     cx,8
gt_ncp:
        mov     al,[si]
        or      al,al
        jz      gt_nend
        call    is_dl
        je      gt_nend
        call    upcase
        mov     [di],al
        inc     si
        inc     di
        loop    gt_ncp
gt_nend:
        mov     byte ptr [di],0
        xor     eax,eax
gt_line:
        mov     byte ptr [bat_gone],0
        call    bat_read
        jc      gt_missing
        push    edx
        mov     si,OFFSET bat_line
        call    skip_bl
        cmp     byte ptr [si],':'
        jne     gt_next
        inc     si
        mov     di,OFFSET gt_label
        mov     cx,8
gt_cmp:
        mov     al,[di]
        or      al,al
        jz      gt_lend
        mov     ah,al
        mov     al,[si]
        call    upcase
        cmp     al,ah
        jne     gt_next
        inc     si
        inc     di
        loop    gt_cmp
        jmp     short gt_found          ; eight matched: the rest is noise
gt_lend:
        mov     al,[si]
        or      al,al
        jz      gt_found
        call    is_dl
        je      gt_found
gt_next:
        pop     eax
        jmp     gt_line
gt_found:
        pop     edx
        mov     dword ptr [bat_cur + BCO_POS],edx
gt_out:
        ret
gt_missing:
        cmp     byte ptr [bat_gone],0
        jne     gt_gone
        mov     si,OFFSET msg_nolabel
        call    puts_err
        jmp     bat_end_all
gt_gone:
        mov     si,OFFSET msg_batmiss
        call    puts_err
        jmp     bat_end_all

;---------------------------------------------------------------------
; IF [NOT] ERRORLEVEL n command
; IF [NOT] EXIST filespec command
; IF [NOT] string1==string2 command
;
; The command runs through dispatch and not exec_line: any redirection
; on the line was taken off the IF and is already in force, which is
; where DOS left it too.
;---------------------------------------------------------------------
cmd_if:
        mov     byte ptr [if_not],0
        call    skip_bl
        mov     di,OFFSET w_not
        call    word_is
        jne     if_what
        mov     byte ptr [if_not],1
        call    skip_bl
if_what:
        mov     di,OFFSET w_errorlevel
        call    word_is
        je      if_errlev
        mov     di,OFFSET w_exist
        call    word_is
        je      if_exist
        ; string1==string2
        mov     di,OFFSET if_str1
        xor     cx,cx
if_s1:
        mov     al,[si]
        or      al,al
        jz      if_syntax
        cmp     al,'='
        je      if_s1end
        cmp     al,' '
        je      if_s1end
        cmp     al,9
        je      if_s1end
        inc     si
        cmp     cx,LINE_MAX - 1
        jae     if_s1
        mov     [di],al
        inc     di
        inc     cx
        jmp     if_s1
if_s1end:
        mov     byte ptr [di],0
        or      cx,cx
        jz      if_syntax
        call    skip_bl
        cmp     word ptr [si],'=='
        jne     if_syntax
        add     si,2
        call    skip_bl
        mov     di,OFFSET if_str2
        xor     cx,cx
if_s2:
        mov     al,[si]
        or      al,al
        jz      if_s2end
        call    is_dl
        je      if_s2end
        inc     si
        cmp     cx,LINE_MAX - 1
        jae     if_s2
        mov     [di],al
        inc     di
        inc     cx
        jmp     if_s2
if_s2end:
        mov     byte ptr [di],0
        push    si
        mov     si,OFFSET if_str1
        mov     di,OFFSET if_str2
if_scmp:
        mov     al,[si]
        cmp     al,[di]
        jne     if_sne
        or      al,al
        jz      if_seq
        inc     si
        inc     di
        jmp     if_scmp
if_seq:
        pop     si
        mov     al,1
        jmp     short if_decide
if_sne:
        pop     si
        xor     al,al
        jmp     short if_decide

if_errlev:
        call    skip_dl
        call    get_num
        jc      if_syntax
        mov     cl,al
        xor     al,al
        cmp     [errlevel],cl
        jb      if_decide
        mov     al,1
        jmp     short if_decide

if_exist:
        mov     di,OFFSET if_str1
        call    get_arg
        or      cx,cx
        jz      if_syntax
        push    si
        call    set_dta
        mov     dx,OFFSET if_str1
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        pop     si
        mov     al,0
        jc      if_decide
        mov     al,1

if_decide:
        xor     al,[if_not]
        push    ax
        call    skip_dl
        pop     ax
        cmp     byte ptr [si],0
        je      if_syntax               ; a condition with nothing to do
        or      al,al
        jz      if_out
        jmp     dispatch
if_out:
        ret
if_syntax:
        mov     si,OFFSET msg_syntax
        jmp     puts_err

;---------------------------------------------------------------------
; FOR %v IN (set) DO command
;
; In a batch file it is written %%v, and bat_expand has already turned
; that into %v, so by here both look the same.  Each item of the set
; is substituted for %v in the command and the command is run as a
; line of its own - through exec_line, so the command's redirections
; apply to each run and not to the loop (see exec_simple).  An item
; with * or ? in it is every file it matches, path and all.
;
; FOR cannot be nested, which was DOS's rule as well, and a FOR whose
; command starts a batch file stops there: that batch file has
; replaced the one the FOR was in.
;---------------------------------------------------------------------
cmd_for:
        cmp     byte ptr [for_active],0
        je      for_parse
        mov     si,OFFSET msg_fornest
        jmp     puts_err
for_parse:
        call    skip_bl
        cmp     byte ptr [si],'%'
        jne     for_syntax
        mov     al,[si+1]
        or      al,al
        jz      for_syntax
        call    is_dl
        je      for_syntax
        mov     [for_var],al
        add     si,2
        call    skip_bl
        mov     di,OFFSET w_in
        call    word_is
        jne     for_syntax
        call    skip_bl
        cmp     byte ptr [si],'('
        jne     for_syntax
        inc     si
        mov     di,OFFSET for_set
        mov     cx,LINE_MAX
for_setcp:
        lodsb
        or      al,al
        jz      for_syntax
        cmp     al,')'
        je      for_setend
        stosb
        loop    for_setcp
        jmp     for_syntax
for_setend:
        mov     byte ptr [di],0
        call    skip_bl
        mov     di,OFFSET w_do
        call    word_is
        jne     for_syntax
        call    skip_bl
        cmp     byte ptr [si],0
        je      for_syntax
        mov     di,OFFSET for_body
        call    strcpy

        mov     byte ptr [for_active],1
        mov     ax,[bat_depth]          ; how to tell a batch file was
        mov     [for_bdepth],ax         ; started: the level changes, or
        mov     eax,dword ptr [bat_cur + BCO_POS]
        mov     [for_bpos],eax          ; the same level starts over
        mov     word ptr [for_item],OFFSET for_set
for_next:
        mov     si,[for_item]
        call    skip_dl
        cmp     byte ptr [si],0
        je      for_done
        mov     di,OFFSET for_val
        xor     cx,cx
for_icp:
        mov     al,[si]
        or      al,al
        jz      for_iend
        call    is_dl
        je      for_iend
        inc     si
        cmp     cx,PATH_MAX - 1
        jae     for_icp
        mov     [di],al
        inc     di
        inc     cx
        jmp     for_icp
for_iend:
        mov     byte ptr [di],0
        mov     [for_item],si
        mov     si,OFFSET for_val
        call    has_wild
        jc      for_wild
        call    for_run
        jc      for_done
        jmp     for_next
for_wild:
        ; every file it matches, with the item's own path in front
        mov     si,OFFSET for_val
        call    name_part
        mov     [for_namep],di
        mov     dx,OFFSET for_dta
        mov     ah,1Ah
        int     21h
        mov     dx,OFFSET for_val
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        jc      for_wdone
for_wone:
        mov     si,OFFSET for_val       ; path part, then the name
        mov     di,OFFSET for_file
for_wpath:
        cmp     si,[for_namep]
        je      for_wname
        movsb
        jmp     for_wpath
for_wname:
        mov     si,OFFSET for_dta + 1Eh
        call    strcpy
        mov     si,OFFSET for_file
        call    for_runf
        jc      for_done
        mov     dx,OFFSET for_dta
        mov     ah,1Ah
        int     21h
        mov     ah,4Fh
        int     21h
        jnc     for_wone
for_wdone:
        call    set_dta
        jmp     for_next
for_done:
        call    set_dta
        mov     byte ptr [for_active],0
        ret
for_syntax:
        mov     si,OFFSET msg_syntax
        jmp     puts_err

; for_run  - run the command once with for_val as %v.  CF: stop the loop.
; for_runf - the same with SI -> the value.
for_run:
        mov     si,OFFSET for_val
for_runf:
        mov     [for_valp],si
        mov     si,OFFSET for_body
        mov     di,OFFSET for_line
        mov     dx,OFFSET for_line + EXP_MAX
for_sub:
        lodsb
        or      al,al
        jz      for_subend
        cmp     al,'%'
        jne     for_subput
        mov     ah,[si]
        cmp     ah,[for_var]
        jne     for_subput
        inc     si
        push    si
        mov     si,[for_valp]
for_subv:
        lodsb
        or      al,al
        jz      for_subvend
        cmp     di,dx
        jae     for_subv
        stosb
        jmp     for_subv
for_subvend:
        pop     si
        jmp     for_sub
for_subput:
        cmp     di,dx
        jae     for_sub
        stosb
        jmp     for_sub
for_subend:
        mov     byte ptr [di],0
        mov     si,OFFSET for_line
        call    exec_line
        call    set_dta
        cmp     byte ptr [cc_broken],0  ; a ^C or an Abort in what it ran
        jne     for_stop                ; ends the loop, as it ended DOS's
        ; Did that start a batch file?  CALL made a level: run it to
        ; its end now and then carry on, which is DOS's order - the
        ; idiom FOR %%F IN (*.TXT) DO CALL ONE.BAT %%F depends on it.
        mov     ax,[bat_depth]
        cmp     ax,[for_bdepth]
        jb      for_stop                ; the batch file it was in ended
        je      for_same
        call    for_nest
        jc      for_stop
        mov     ax,[bat_depth]
        cmp     ax,[for_bdepth]
        jne     for_stop
for_same:
        ; the same level moved - a GOTO, or a batch file named without
        ; CALL, which replaced this one and starts at its top
        or      ax,ax
        jz      for_go
        mov     eax,dword ptr [bat_cur + BCO_POS]
        cmp     eax,[for_bpos]
        jne     for_stop
for_go:
        clc
        ret
for_stop:
        stc
        ret

;---------------------------------------------------------------------
; for_nest - the loop's command CALLed a batch file.  Park this FOR in a
;            block of its own - so that the called file may have FORs
;            of its own - run the called file to its end, and take the
;            FOR back.  CF if there was no memory to park it in, in
;            which case the loop stops and the main loop runs the file.
;---------------------------------------------------------------------
for_nest:
        mov     bx,OFFSET for_state_end
        sub     bx,OFFSET for_state
        add     bx,15
        shr     bx,4
        mov     ah,48h
        int     21h
        jc      fn_nomem
        cmp     word ptr [for_nsaved],FOR_PARKMAX
        jae     fn_full
        mov     di,[for_nsaved]
        shl     di,1
        mov     [for_saves+di],ax
        inc     word ptr [for_nsaved]
        push    es
        mov     es,ax
        xor     di,di
        mov     si,OFFSET for_state
        mov     cx,OFFSET for_state_end
        sub     cx,si
        rep     movsb
        pop     es
        mov     byte ptr [for_active],0

        mov     ax,[for_bdepth]
        call    run_until
        ; A ^C IN THE FILE IT CALLED DOES NOT STOP THIS FOR.  Its FOR state
        ; is this level's, and 6.22 clears the flag of the level the ^C
        ; came in - FOR %%F IN (1 2 3) DO CALL X.BAT runs X.BAT three
        ; times there however many of them are broken off.
        mov     byte ptr [cc_broken],0

        dec     word ptr [for_nsaved]
        mov     di,[for_nsaved]
        shl     di,1
        mov     ax,[for_saves+di]
        push    ds
        push    ax
        mov     ds,ax
        xor     si,si
        mov     di,OFFSET for_state
        mov     cx,OFFSET for_state_end
        sub     cx,di
        rep     movsb
        pop     ax
        pop     ds
        push    es
        mov     es,ax
        mov     ah,49h
        int     21h
        pop     es
        mov     byte ptr [for_active],1
        clc
        ret
fn_full:
        push    es
        mov     es,ax
        mov     ah,49h
        int     21h
        pop     es
fn_nomem:
        mov     si,OFFSET msg_nomem
        call    puts_err
        stc
        ret

;---------------------------------------------------------------------
; run_until - AX = a batch depth.  Run lines, exactly as the main loop
;             does, until the batch files above that depth have ended.
;---------------------------------------------------------------------
run_until:
        push    ax
ru_loop:
        pop     ax
        push    ax
        cmp     [bat_depth],ax
        jbe     ru_done
        mov     byte ptr [call_flag],0
        call    get_command
        jc      ru_loop
        call    exec_line
        cmp     byte ptr [crit_abort],0
        je      ru_loop
        mov     byte ptr [crit_abort],0
        call    ask_endbat
        jmp     ru_loop
ru_done:
        pop     ax
        ret

; for_unpark - abandon's half: every parked FOR goes
for_unpark:
        cmp     word ptr [for_nsaved],0
        je      fu_out
        dec     word ptr [for_nsaved]
        mov     di,[for_nsaved]
        shl     di,1
        push    es
        mov     es,[for_saves+di]
        mov     ah,49h
        int     21h
        pop     es
        jmp     for_unpark
fu_out:
        ret

;---------------------------------------------------------------------
; Data
;---------------------------------------------------------------------
msg_batmiss db  'Batch file missing',13,10,0
msg_nolabel db  'Label not found',13,10,0
msg_batdeep db  'Batch files nested too deeply',13,10,0
msg_fornest db  'FOR cannot be nested',13,10,0
w_not       db  'NOT',0
w_errorlevel db 'ERRORLEVEL',0
w_exist     db  'EXIST',0
w_in        db  'IN',0
w_do        db  'DO',0

if_not      db  0
br_pos      dd  0
bx_end      dw  0
for_nsaved  dw  0
for_saves   dw  FOR_PARKMAX dup(0)
gt_label    db  10 dup(0)
bs_full     db  NAME_MAX dup(0)
if_str1     db  LINE_MAX dup(0)
if_str2     db  LINE_MAX dup(0)
for_line    db  EXP_MAX + 3 dup(0)

; One FOR's whole state, in one piece so that for_nest can park it.
for_state   LABEL BYTE
for_var     db  0
for_bpos    dd  0
for_bdepth  dw  0
for_item    dw  0
for_namep   dw  0
for_valp    dw  0
for_set     db  LINE_MAX + 2 dup(0)
for_body    db  EXP_MAX + 2 dup(0)
for_val     db  PATH_MAX dup(0)
for_file    db  NAME_MAX dup(0)
for_dta     db  44 dup(0)
for_state_end LABEL BYTE
