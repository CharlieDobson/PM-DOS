;=====================================================================
; INIT16.INC - start-up.  It runs once, from the part of the file the
; stack is laid over afterwards, on a small stack of its own at the
; very end of the image.
;
; THE ORDER IS FORCED.  A .COM owns all of memory when it starts, so
; nothing can be allocated until the image has been shrunk to itself -
; but the image still includes this code, which is running.  So: shrink
; to the whole image, allocate the environment at the TOP of memory
; (last fit), do everything else, and only then shrink to the resident
; part, leaving one free block between it and the environment for the
; programs this shell runs.
;=====================================================================
init:
        cld
        mov     sp,OFFSET init_top
        mov     ah,30h
        int     21h
        cmp     al,3
        jae     in_verok
        mov     dx,OFFSET msg_badver
        mov     ah,09h
        int     21h
        int     20h
in_verok:
        mov     bx,OFFSET image_end
        add     bx,15
        shr     bx,4
        mov     ah,4Ah                  ; ES is our PSP: DOS said so
        int     21h
        push    cs
        pop     es

        call    in_selfpath
        mov     ax,ds:[PSP_PARENT]
        mov     [orig_parent],ax
        mov     ax,ds:[PSP_TERM]
        mov     word ptr [orig_term],ax
        mov     ax,ds:[PSP_TERM+2]
        mov     word ptr [orig_term+2],ax
        call    in_switches
        call    in_country
        call    in_env
        cmp     byte ptr [in_comdir],0
        je      in_nocomspec
        cmp     byte ptr [permanent],0
        je      in_nocomspec
        call    in_comspec
in_nocomspec:

        ; OUR OWN PARENT, as DOS's COMMAND.COM was.  DOS decides what an
        ; Abort terminates by the parent field: a program that is its
        ; own parent keeps its memory and files and is sent to its
        ; terminate address - abort_entry - instead of disappearing.
        ; exit_shell puts both fields back before a real exit.
        mov     ds:[PSP_PARENT],cs
        mov     word ptr ds:[PSP_TERM],OFFSET abort_entry
        mov     ds:[PSP_TERM+2],cs
        call    set_vectors
        call    set_dta

        cmp     byte ptr [permanent],0
        je      in_final
        call    in_autoexec
in_final:
        mov     bx,OFFSET resident_end + STACK_SIZE
        add     bx,15
        shr     bx,4
        mov     ah,4Ah
        int     21h
        mov     sp,OFFSET resident_end + STACK_SIZE
        mov     [main_sp],sp
        jmp     main_loop

;---------------------------------------------------------------------
; in_selfpath - the path DOS appends to a program's environment, after
;               the empty string and a count of 1: this file.  /? reads
;               its help back out of it.
;---------------------------------------------------------------------
in_selfpath:
        mov     ax,ds:[PSP_ENV]
        or      ax,ax
        jz      isp_out
        push    ds
        mov     ds,ax
        xor     si,si
isp_scan:
        cmp     byte ptr [si],0
        je      isp_end
isp_str:
        lodsb
        or      al,al
        jnz     isp_str
        jmp     isp_scan
isp_end:
        inc     si
        cmp     word ptr [si],1
        jne     isp_none
        add     si,2
        mov     di,OFFSET self_path
        mov     cx,PATH_MAX - 1
isp_cp:
        lodsb
        stosb
        or      al,al
        jz      isp_none
        loop    isp_cp
        mov     byte ptr es:[di],0
isp_none:
        pop     ds
isp_out:
        ret

;---------------------------------------------------------------------
; in_switches - the command tail:
;
;   COMMAND [[drive:]path] [device] [/E:nnnnn] [/P] [/F]
;           [/C command | /K command]
;
; A device is a CTTY; a path is where COMMAND.COM lives, which a
; permanent shell turns into COMSPEC.  /Y, /D and /MSG are DOS 6's and
; are accepted without doing anything.  /C and /K take the whole rest
; of the line.
;---------------------------------------------------------------------
in_switches:
        mov     si,PSP_TAIL + 1
        movzx   bx,byte ptr ds:[PSP_TAIL]
        cmp     bx,7Eh
        jbe     ins_len
        mov     bx,7Eh
ins_len:
        mov     byte ptr [si+bx],0
ins_loop:
        call    skip_dl
        mov     al,[si]
        or      al,al
        jz      ins_done
        cmp     al,'/'
        je      ins_sw
        mov     di,OFFSET arg1
        call    get_arg
        push    si
        mov     dx,OFFSET arg1
        mov     ax,3D02h
        int     21h
        jc      ins_path
        mov     bx,ax
        mov     ax,4400h
        int     21h
        jc      ins_notdev
        test    dl,80h
        jz      ins_notdev
        xor     cx,cx                   ; a device: it is the console now
ins_ctty:
        mov     ah,46h
        int     21h
        inc     cx
        cmp     cx,3
        jb      ins_ctty
ins_notdev:
        mov     ah,3Eh
        int     21h
        pop     si
        jmp     ins_loop
ins_path:
        mov     si,OFFSET arg1
        mov     di,OFFSET in_comdir
        call    strcpy
        pop     si
        jmp     ins_loop
ins_sw:
        inc     si
        mov     al,[si]
        inc     si
        call    upcase
        cmp     al,'C'
        je      ins_c
        cmp     al,'K'
        je      ins_k
        cmp     al,'P'
        je      ins_p
        cmp     al,'E'
        je      ins_e
        cmp     al,'F'
        je      ins_f
        cmp     al,'?'
        je      ins_help
ins_skip:                               ; anything else: to the next word
        mov     al,[si]
        or      al,al
        jz      ins_done
        call    is_dl
        je      ins_loop
        cmp     al,'/'
        je      ins_loop
        inc     si
        jmp     ins_skip
ins_p:
        mov     byte ptr [permanent],1
        jmp     ins_skip
ins_f:
        mov     byte ptr [crit_fail],1
        jmp     ins_skip
ins_e:
        cmp     byte ptr [si],':'
        jne     ins_enum
        inc     si
ins_enum:
        call    get_num
        jc      ins_skip
        mov     [in_envreq],ax
        jmp     ins_skip
ins_k:
        mov     byte ptr [single_mode],SM_KEEP
        jmp     short ins_rest
ins_c:
        mov     byte ptr [single_mode],SM_C
ins_rest:
        call    skip_bl
        mov     di,OFFSET single_cmd
        mov     cx,LINE_MAX - 1
ins_cp:
        lodsb
        or      al,al
        jz      ins_cpend
        stosb
        loop    ins_cp
ins_cpend:
        mov     byte ptr [di],0
ins_done:
        ret
ins_help:
        ; COMMAND /? - there is no row for COMMAND in int_table, so a
        ; row is made up here for show_help
        mov     word ptr [in_hrow+IT_HELP],hl_command - start
        mov     word ptr [in_hrow+IT_HLEN],hl_command_end - hl_command
        mov     bx,OFFSET in_hrow
        call    show_help
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; in_country - the date order, the separators and the clock from 38h;
;              DOS's USA defaults if it will not say.
;---------------------------------------------------------------------
in_country:
        mov     dx,OFFSET in_ctry
        mov     ax,3800h
        int     21h
        jc      ic_out
        mov     ax,word ptr [in_ctry]
        cmp     ax,2
        ja      ic_out
        mov     [ctry_date],ax
        mov     bx,ax
        shl     bx,1
        mov     ax,[ic_fmts+bx]
        mov     [date_fmts],ax
        mov     al,[in_ctry+07h]
        mov     [ctry_thou],al
        mov     al,[in_ctry+09h]
        mov     [ctry_dec],al
        mov     al,[in_ctry+0Bh]
        mov     [ctry_dsep],al
        mov     al,[in_ctry+0Dh]
        mov     [ctry_tsep],al
        mov     al,[in_ctry+11h]
        and     al,1
        mov     [ctry_24h],al
ic_out:
        ret

;---------------------------------------------------------------------
; in_env - an environment of our own, big enough to SET things in.
;
; What EXEC handed us is exactly as big as what was in it.  /E: says
; how big (160 to 32768 bytes, as DOS allowed); otherwise it is what
; was there and 256 bytes more, and never less than 512.
;---------------------------------------------------------------------
in_env:
        mov     ax,ds:[PSP_ENV]
        mov     [ie_old],ax
        mov     cx,1                    ; the bytes in use
        or      ax,ax
        jz      ie_sized
        push    es
        mov     es,ax
        xor     di,di
ie_scan:
        cmp     byte ptr es:[di],0
        je      ie_end
ie_str:
        inc     di
        cmp     byte ptr es:[di-1],0
        jne     ie_str
        jmp     ie_scan
ie_end:
        pop     es
        lea     cx,[di+1]
ie_sized:
        mov     [ie_used],cx
        mov     ax,[in_envreq]
        or      ax,ax
        jz      ie_default
        cmp     ax,160
        jae     ie_min
        mov     ax,160
ie_min:
        cmp     ax,32768
        jbe     ie_req
        mov     ax,32768
ie_req:
        cmp     ax,cx                   ; never smaller than what it holds
        jae     ie_round
        mov     ax,cx
        jmp     short ie_round
ie_default:
        mov     ax,cx
        add     ax,256
        cmp     ax,512
        jae     ie_round
        mov     ax,512
ie_round:
        add     ax,15
        and     ax,0FFF0h
        mov     [env_size],ax

        ; at the top of memory, so the resident part can shrink under it
        mov     ax,5800h
        int     21h
        mov     [ie_strat],ax
        mov     ax,5801h
        mov     bx,2
        int     21h
        mov     bx,[env_size]
        shr     bx,4
        mov     ah,48h
        int     21h
        pushf
        push    ax
        mov     ax,5801h
        mov     bx,[ie_strat]
        int     21h
        pop     ax
        popf
        jc      ie_keep
        mov     [env_seg],ax
        push    es
        mov     es,ax
        xor     di,di
        mov     cx,[env_size]
        xor     al,al
        rep     stosb
        mov     ax,[ie_old]
        or      ax,ax
        jz      ie_copied
        push    ds
        mov     ds,ax
        xor     si,si
        xor     di,di
        mov     cx,cs:[ie_used]
        rep     movsb
        pop     ds
        mov     es,ax                   ; EXEC made the old one ours
        mov     ah,49h
        int     21h
ie_copied:
        pop     es
        mov     ax,[env_seg]
        mov     ds:[PSP_ENV],ax
        ret
ie_keep:
        ; no memory: the block we were given, as big as its MCB says
        mov     ax,[ie_old]
        mov     [env_seg],ax
        push    es
        dec     ax
        mov     es,ax
        mov     ax,es:[3]
        pop     es
        shl     ax,4
        mov     [env_size],ax
        ret

;---------------------------------------------------------------------
; in_comspec - COMMAND C:\DOS /P: COMSPEC is C:\DOS\COMMAND.COM
;---------------------------------------------------------------------
in_comspec:
        mov     di,OFFSET set_buf
        mov     si,OFFSET lit_comspec
        call    strcpy
        mov     si,OFFSET in_comdir
        call    strcpy
        cmp     byte ptr [di-1],'\'
        je      icp_name
        cmp     byte ptr [di-1],':'
        je      icp_name
        mov     byte ptr [di],'\'
        inc     di
icp_name:
        mov     si,OFFSET lit_command
        call    strcpy
        mov     si,OFFSET set_buf
        jmp     env_set

;---------------------------------------------------------------------
; in_autoexec - the permanent shell's AUTOEXEC.BAT, from the root of the
;               drive the machine started from.  With none, DOS asked
;               for the date and the time, and so does this.
;---------------------------------------------------------------------
in_autoexec:
        mov     ax,3305h
        int     21h
        mov     al,dl
        add     al,'A' - 1
        mov     [in_autoname],al
        call    set_dta
        mov     dx,OFFSET in_autoname
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        jc      ia_none
        mov     si,OFFSET in_autoname
        mov     di,OFFSET prog_path
        call    strcpy
        mov     word ptr [verb_start],OFFSET in_autoname
        mov     byte ptr [call_flag],0
        jmp     batch_start
ia_none:
        mov     si,OFFSET in_empty
        call    cmd_date
        mov     si,OFFSET in_empty
        jmp     cmd_time

;---------------------------------------------------------------------
; Start-up's own data.  It is in the part the stack overlays, and none
; of it is looked at again once the shell is running.
;---------------------------------------------------------------------
msg_badver  db  'Incorrect DOS version',13,10,'$'
lit_comspec db  'COMSPEC=',0
lit_command db  'COMMAND.COM',0
in_autoname db  'A:\AUTOEXEC.BAT',0
in_empty    db  0
ic_fmts     dw  OFFSET dfmt_mdy, OFFSET dfmt_dmy, OFFSET dfmt_ymd
in_envreq   dw  0
ie_old      dw  0
ie_used     dw  0
ie_strat    dw  0
in_hrow     dw  0, 0, 0, 0, 0
in_ctry     db  34 dup(0)
in_comdir   db  PATH_MAX dup(0)
