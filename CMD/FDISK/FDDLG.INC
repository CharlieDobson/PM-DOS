;=====================================================================
; FDDLG.INC - the menus, the boxes and the text viewer.  Included by
;             FDTUI.INC.
;
; THE MENUS work the way DiskStat's do: F10 or Alt and a menu's letter
; from the keyboard, and with the mouse either press-drag-let go or a
; click to open and a click to choose.  An item hands back a command,
; and the session does with it exactly what it does with a key.
;
; THE BOXES are built a line at a time (ln_new and the lb_ routines),
; may have one typing field and one check box, and end in buttons;
; dg_run puts the box up and hands back the button.  A box sits in the
; lower half of the screen so the bar stays in sight, and a box with a
; field can have dg_pvfn redraw the bar from the field as it is typed.
;=====================================================================

;=====================================================================
; mu_run - EAX = the menu to open, EBX = 1 when it was opened by a
;          press of the mouse.  -> EAX = the command chosen, or
;          CMD_NONE.
;=====================================================================
mu_run:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        mov     [mu_open],eax
        mov     byte ptr [mu_dragged],0
        call    mu_first
mur_loop:
        call    tui_draw
        call    ev_get
        cmp     eax,EV_KEY
        je      mur_key
        cmp     eax,EV_DOWN
        je      mur_down
        cmp     eax,EV_DOUBLE
        je      mur_down
        cmp     eax,EV_DRAG
        je      mur_drag
        cmp     eax,EV_UP
        je      mur_up
        cmp     eax,EV_EOF
        je      mur_none
        jmp     short mur_loop

mur_key:
        mov     ax,[ev_kcode]
        cmp     al,1Bh
        je      mur_none
        cmp     al,0Dh
        je      mur_pick
        or      al,al
        jz      mur_grey
        call    fd_upper
        call    mu_bykey                ; an item's own letter picks it
        or      eax,eax
        js      mur_loop
        mov     [mu_hi],eax
        jmp     mur_pick
mur_grey:
        cmp     ah,SC_F10
        je      mur_none
        mov     ecx,-1
        cmp     ah,SC_UP
        je      mur_vert
        mov     ecx,1
        cmp     ah,SC_DOWN
        je      mur_vert
        mov     eax,[mu_open]
        cmp     byte ptr [ev_kcode+1],SC_LEFT
        je      mur_left
        cmp     byte ptr [ev_kcode+1],SC_RIGHT
        je      mur_right
        ; Alt and another menu's letter
        mov     al,byte ptr [ev_kcode+1]
        xor     ecx,ecx
mur_alt:
        cmp     ecx,MU_COUNT
        jae     mur_loop
        imul    esi,ecx,MU_LEN
        cmp     al,byte ptr [mu_tbl+esi+MU_ALT]
        je      mur_switch
        inc     ecx
        jmp     short mur_alt
mur_vert:
        call    mu_step
        jmp     mur_loop
mur_left:
        dec     eax
        jns     mur_lr
        mov     eax,MU_COUNT - 1
        jmp     short mur_lr
mur_right:
        inc     eax
        cmp     eax,MU_COUNT
        jb      mur_lr
        xor     eax,eax
mur_lr:
        mov     ecx,eax
mur_switch:
        mov     [mu_open],ecx
        call    mu_first
        jmp     mur_loop

mur_down:
        mov     ebx,[ev_row]
        mov     ecx,[ev_col]
        cmp     ebx,ROW_MENU
        jne     mur_inside
        call    mu_hit                  ; another name on the bar, or the same
        or      eax,eax
        js      mur_none
        mov     ecx,eax
        jmp     short mur_switch
mur_inside:
        call    mu_itemat
        cmp     eax,-2
        je      mur_none                ; a press outside it closes it
        or      eax,eax
        js      mur_loop
        mov     [mu_hi],eax
        jmp     mur_loop

mur_drag:
        mov     byte ptr [mu_dragged],1
        mov     ebx,[ev_row]
        mov     ecx,[ev_col]
        cmp     ebx,ROW_MENU
        jne     mur_dragin
        call    mu_hit
        or      eax,eax
        js      mur_loop
        cmp     eax,[mu_open]
        je      mur_loop
        mov     ecx,eax
        jmp     short mur_switch
mur_dragin:
        call    mu_itemat
        or      eax,eax
        js      mur_loop
        mov     [mu_hi],eax
        jmp     mur_loop

mur_up:
        mov     ebx,[ev_row]
        mov     ecx,[ev_col]
        cmp     ebx,ROW_MENU
        je      mur_loop                ; let go on its name: it stays open
        call    mu_itemat
        or      eax,eax
        js      mur_upoff
        mov     [mu_hi],eax
        jmp     short mur_pick
mur_upoff:
        cmp     byte ptr [mu_dragged],0
        je      mur_loop
        jmp     short mur_none          ; dragged off it and let go

mur_pick:
        mov     eax,[mu_open]
        imul    esi,eax,MU_LEN
        add     esi,OFFSET mu_tbl
        mov     eax,[mu_hi]
        imul    eax,MI_LEN
        add     eax,[esi+MU_ITEMS]
        mov     eax,[eax+MI_CMD]
        cmp     eax,CMD_SEP
        je      mur_loop
        jmp     short mur_out
mur_none:
        mov     eax,CMD_NONE
mur_out:
        mov     dword ptr [mu_open],-1
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- mu_first - the first item of the open menu lit ------------------
mu_first:
        mov     dword ptr [mu_hi],-1
        push    ecx
        mov     ecx,1
        call    mu_step
        pop     ecx
        ret

;--- mu_step - ECX = 1 or -1: the next item that can be chosen -------
mu_step:
        pushad
        mov     eax,[mu_open]
        imul    esi,eax,MU_LEN
        add     esi,OFFSET mu_tbl
        mov     ebx,[esi+MU_N]
        or      ebx,ebx
        jz      mst_out
        mov     edx,ebx                 ; tries
        mov     eax,[mu_hi]
mst_again:
        add     eax,ecx
        jns     mst_top
        lea     eax,[ebx-1]
mst_top:
        cmp     eax,ebx
        jb      mst_test
        xor     eax,eax
mst_test:
        imul    edi,eax,MI_LEN
        add     edi,[esi+MU_ITEMS]
        cmp     dword ptr [edi+MI_CMD],CMD_SEP
        jne     mst_set
        dec     edx
        jnz     mst_again
mst_set:
        mov     [mu_hi],eax
mst_out:
        popad
        ret

;--- mu_bykey - AL = a letter -> EAX = the open menu's item with it, -1
mu_bykey:
        push    ecx
        push    esi
        push    edi
        mov     ecx,[mu_open]
        imul    esi,ecx,MU_LEN
        add     esi,OFFSET mu_tbl
        xor     ecx,ecx
mbk_one:
        cmp     ecx,[esi+MU_N]
        jae     mbk_none
        imul    edi,ecx,MI_LEN
        add     edi,[esi+MU_ITEMS]
        cmp     al,byte ptr [edi+MI_KEY]
        je      mbk_found
        inc     ecx
        jmp     short mbk_one
mbk_found:
        mov     eax,ecx
        jmp     short mbk_out
mbk_none:
        mov     eax,-1
mbk_out:
        pop     edi
        pop     esi
        pop     ecx
        ret

;--- mu_hit - ECX = a column of the bar -> EAX = the menu named there,
;--- or -1
mu_hit:
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     edi,ecx
        xor     eax,eax
muh_one:
        cmp     eax,MU_COUNT
        jae     muh_none
        imul    esi,eax,MU_LEN
        add     esi,OFFSET mu_tbl
        cmp     edi,[esi+MU_COL]
        jb      muh_next
        mov     edx,[esi+MU_TITLE]
        call    sd_len
        add     ecx,2
        add     ecx,[esi+MU_COL]
        cmp     edi,ecx
        jb      muh_out
muh_next:
        inc     eax
        jmp     short muh_one
muh_none:
        mov     eax,-1
muh_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        ret

;--- mu_itemat - EBX = row, ECX = column -> EAX = the open menu's item
;--- there, -1 for its border or a line between items, -2 outside it
mu_itemat:
        push    edx
        push    esi
        mov     eax,[mu_open]
        imul    esi,eax,MU_LEN
        add     esi,OFFSET mu_tbl
        cmp     ecx,[esi+MU_COL]
        jb      mia_out
        mov     edx,[esi+MU_COL]
        add     edx,[esi+MU_W]
        inc     edx                     ; EDX = its right border
        cmp     ecx,edx
        ja      mia_out
        cmp     ebx,ROW_MENU + 1
        jb      mia_out
        mov     eax,[esi+MU_N]
        add     eax,ROW_MENU + 2        ; its bottom border
        cmp     ebx,eax
        ja      mia_out
        cmp     ecx,[esi+MU_COL]
        je      mia_edge
        cmp     ecx,edx
        je      mia_edge
        mov     eax,ebx
        sub     eax,ROW_MENU + 2
        jb      mia_edge
        cmp     eax,[esi+MU_N]
        jae     mia_edge
        mov     edx,eax
        imul    edx,MI_LEN
        add     edx,[esi+MU_ITEMS]
        cmp     dword ptr [edx+MI_CMD],CMD_SEP
        je      mia_edge
        pop     esi
        pop     edx
        ret
mia_edge:
        mov     eax,-1
        pop     esi
        pop     edx
        ret
mia_out:
        mov     eax,-2
        pop     esi
        pop     edx
        ret

;--- mu_draw - the open menu, dropped from its name ------------------
mu_draw:
        pushad
        mov     eax,[mu_open]
        or      eax,eax
        js      mud_out
        imul    esi,eax,MU_LEN
        add     esi,OFFSET mu_tbl
        mov     ecx,[esi+MU_COL]
        mov     ebx,ROW_MENU + 1
        mov     edx,ebx
        add     edx,[esi+MU_N]
        inc     edx
        mov     edi,ecx
        add     edi,[esi+MU_W]
        inc     edi
        push    esi
        mov     esi,edi
        mov     byte ptr [scr_cattr],A_MENU
        xor     eax,eax
        call    scr_box
        call    scr_shade
        pop     esi
        xor     ebp,ebp
mud_item:
        cmp     ebp,[esi+MU_N]
        jae     mud_out
        imul    edi,ebp,MI_LEN
        add     edi,[esi+MU_ITEMS]
        lea     ebx,[ebp+ROW_MENU+2]
        mov     ecx,[esi+MU_COL]
        cmp     dword ptr [edi+MI_CMD],CMD_SEP
        je      mud_sep
        mov     byte ptr [scr_cattr],A_MENU
        cmp     ebp,[mu_hi]
        jne     mud_attr
        mov     byte ptr [scr_cattr],A_MENUHI
mud_attr:
        inc     ecx
        call    scr_at
        mov     al,' '
        call    scr_putc
        mov     edx,[edi+MI_LABEL]
        call    scr_puts
        mov     al,' '
        call    scr_putc
        ; the disk on the screen is ticked in the Disk menu
        mov     eax,[edi+MI_CMD]
        sub     eax,CMD_DISK0
        jb      mud_next
        cmp     eax,[tui_disk]
        jne     mud_next
        inc     ecx
        call    scr_at
        mov     al,0FBh
        call    scr_putc
        jmp     short mud_next
mud_sep:
        mov     byte ptr [scr_cattr],A_MENU
        call    scr_at
        mov     al,0C3h
        call    scr_putc
        push    ecx
        mov     ecx,[esi+MU_W]
        mov     al,0C4h
        call    scr_fill
        pop     ecx
        mov     al,0B4h
        call    scr_putc
mud_next:
        inc     ebp
        jmp     short mud_item
mud_out:
        popad
        ret

;--- mu_disks - the Disk menu: an item for each disk, with its size,
;--- then the items that are always there
mu_disks:
        pushad
        mov     edi,OFFSET mi_disk
        xor     ebp,ebp
mdk_one:
        cmp     ebp,[tui_ndisks]
        jae     mdk_rest
        imul    esi,ebp,DM_W + 1
        add     esi,OFFSET dm_text
        mov     [edi+MI_LABEL],esi
        lea     eax,[ebp+CMD_DISK0]
        mov     [edi+MI_CMD],eax
        lea     eax,[ebp+'0']
        mov     [edi+MI_KEY],eax
        push    edi
        mov     edi,esi
        mov     ecx,DM_W
        call    lb_into
        mov     edx,OFFSET msg_mdiskn
        call    lb_puts
        mov     eax,ebp
        call    lb_num
        lea     edx,[ebp+80h]
        mov     edi,OFFSET mu_dsk
        mov     eax,0F10Ch
        int     21h
        jc      mdk_none
        test    byte ptr [mu_dsk+DSK_FLAGS],DSKF_PRESENT
        jz      mdk_none
        mov     eax,dword ptr [mu_dsk+DSK_TOTAL]
        mov     ebx,10
        call    fmt_mb
        call    lb_puts
        mov     edx,OFFSET msg_mbunit
        call    lb_puts
        jmp     short mdk_pad
mdk_none:
        mov     edx,OFFSET msg_mnone
        call    lb_puts
mdk_pad:
        mov     eax,[lb_ptr]
        cmp     eax,[lb_end]
        jae     mdk_next
        mov     al,' '
        call    lb_putc
        jmp     short mdk_pad
mdk_next:
        pop     edi
        add     edi,MI_LEN
        inc     ebp
        jmp     mdk_one
mdk_rest:
        mov     esi,OFFSET mi_diskfix
        mov     ecx,4 * MI_LEN / 4
        rep     movsd
        mov     eax,[tui_ndisks]
        add     eax,4
        mov     dword ptr [mu_tbl + MU_LEN + MU_N],eax
        popad
        ret

;=====================================================================
; THE BOXES.
;
;   dg_reset            start a new one: no lines, no field, no buttons
;   ln_new, lb_*        its lines, one at a time
;   dg_title, dg_flabel, dg_clabel, dg_btn, dg_focus, dg_esc, dg_attr,
;   dg_pvfn             what else it has
;   dg_run              -> EAX = the button: Enter is the one lit, Esc
;                          is dg_esc, a button's first letter is that
;                          button, and so is a click on it
;
; dg_fbuf, the field's text, and dg_cval, the check box, are left alone
; by dg_reset: a box that is put up again after a refusal keeps what was
; typed in it.
;=====================================================================
dg_reset:
        push    eax
        xor     eax,eax
        mov     [dg_n],eax
        mov     [dg_title],eax
        mov     [dg_flabel],eax
        mov     [dg_clabel],eax
        mov     [dg_nb],eax
        mov     [dg_focus],eax
        mov     [dg_esc],eax
        mov     [dg_pvfn],eax
        mov     byte ptr [dg_attr],A_DLG
        pop     eax
        ret

dg_btn:                                 ; EDX -> a button's name
        push    eax
        mov     eax,[dg_nb]
        cmp     eax,DG_MAXBTN
        jae     dgb_out
        mov     [dg_btns+eax*4],edx
        inc     dword ptr [dg_nb]
dgb_out:
        pop     eax
        ret

;--- tu_box - EDX -> a title, BL = its colour: the lines built, and OK
tu_box:
        mov     [dg_title],edx
        mov     [dg_attr],bl
        mov     edx,OFFSET msg_bok
        call    dg_btn
        mov     dword ptr [dg_focus],0
        mov     dword ptr [dg_esc],0
        jmp     dg_run

;--- tu_ask - EDX -> a title, BL = its colour, ECX = the button Enter
;--- presses: 0 Yes, 1 No.  CF clear on Yes.  Y and N answer it too,
;--- as they did every FDISK's questions.
tu_ask:
        push    eax
        mov     [dg_title],edx
        mov     [dg_attr],bl
        mov     edx,OFFSET msg_byes
        call    dg_btn
        mov     edx,OFFSET msg_bno
        call    dg_btn
        mov     [dg_focus],ecx
        mov     dword ptr [dg_esc],1
        call    dg_run
        or      eax,eax
        jz      tak_yes
        pop     eax
        stc
        ret
tak_yes:
        pop     eax
        clc
        ret

;=====================================================================
; dg_run - the box, until a button is chosen.  -> EAX = which.
;=====================================================================
dg_run:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        call    dg_layout
        mov     byte ptr [dg_armed],0
        mov     byte ptr [dg_fresh],1
        xor     ecx,ecx
dgr_len:
        cmp     byte ptr [dg_fbuf+ecx],0
        je      dgr_lend
        inc     ecx
        jmp     short dgr_len
dgr_lend:
        mov     [dg_flen],ecx
        call    dg_preview
dgr_loop:
        call    dg_draw
        call    ev_get
        cmp     eax,EV_KEY
        je      dgr_key
        cmp     eax,EV_DOWN
        je      dgr_down
        cmp     eax,EV_DOUBLE
        je      dgr_down
        cmp     eax,EV_UP
        je      dgr_up
        cmp     eax,EV_EOF
        je      dgr_esc
        jmp     short dgr_loop

dgr_down:
        call    dg_inside
        jc      dgr_loop
        ; ARMED BY A PRESS INSIDE IT, so the let-go of a double click that
        ; opened the box cannot press a button in it
        mov     byte ptr [dg_armed],1
        cmp     dword ptr [dg_clabel],0
        je      dgr_dbtn
        mov     eax,[ev_row]
        cmp     eax,[dg_crow]
        jne     dgr_dbtn
        xor     byte ptr [dg_cval],1
        call    dg_preview
        jmp     short dgr_loop
dgr_dbtn:
        call    dg_btnat
        or      eax,eax
        js      dgr_loop
        mov     [dg_focus],eax
        jmp     short dgr_loop
dgr_up:
        cmp     byte ptr [dg_armed],0
        je      dgr_loop
        mov     byte ptr [dg_armed],0
        call    dg_btnat
        or      eax,eax
        js      dgr_loop
        jmp     dgr_out

dgr_key:
        mov     ax,[ev_kcode]
        cmp     al,1Bh
        je      dgr_esc
        cmp     al,0Dh
        je      dgr_enter
        cmp     al,9
        je      dgr_next
        or      al,al
        jz      dgr_grey
        cmp     al,' '
        jne     dgr_field
        cmp     dword ptr [dg_clabel],0
        je      dgr_field
        xor     byte ptr [dg_cval],1
        call    dg_preview
        jmp     dgr_loop
dgr_field:
        cmp     dword ptr [dg_flabel],0
        je      dgr_letter
        cmp     al,8
        je      dgr_bs
        cmp     al,' '
        jbe     dgr_loop
        cmp     al,7Eh
        ja      dgr_loop
        cmp     byte ptr [dg_fresh],0
        je      dgr_add
        mov     dword ptr [dg_flen],0   ; typing over what was offered
        mov     byte ptr [dg_fresh],0
dgr_add:
        mov     ecx,[dg_flen]
        cmp     ecx,FIELD_MAX
        jae     dgr_loop
        mov     [dg_fbuf+ecx],al
        inc     ecx
        mov     byte ptr [dg_fbuf+ecx],0
        mov     [dg_flen],ecx
        call    dg_preview
        jmp     dgr_loop
dgr_bs:
        mov     byte ptr [dg_fresh],0
        mov     ecx,[dg_flen]
        or      ecx,ecx
        jz      dgr_loop
        dec     ecx
        mov     byte ptr [dg_fbuf+ecx],0
        mov     [dg_flen],ecx
        call    dg_preview
        jmp     dgr_loop
dgr_letter:
        call    fd_upper
        xor     ebx,ebx
dgr_lb:
        cmp     ebx,[dg_nb]
        jae     dgr_loop
        mov     edx,[dg_btns+ebx*4]
        mov     ah,[edx]
        cmp     ah,'a'
        jb      dgr_lbc
        cmp     ah,'z'
        ja      dgr_lbc
        sub     ah,20h
dgr_lbc:
        cmp     al,ah
        je      dgr_lhit
        inc     ebx
        jmp     short dgr_lb
dgr_lhit:
        mov     eax,ebx
        jmp     short dgr_out
dgr_grey:
        cmp     ah,SC_LEFT
        je      dgr_prev
        cmp     ah,SC_SHTAB
        je      dgr_prev
        cmp     ah,SC_RIGHT
        je      dgr_next
        jmp     dgr_loop
dgr_next:
        mov     eax,[dg_focus]
        inc     eax
        cmp     eax,[dg_nb]
        jb      dgr_setf
        xor     eax,eax
        jmp     short dgr_setf
dgr_prev:
        mov     eax,[dg_focus]
        or      eax,eax
        jnz     dgr_pdec
        mov     eax,[dg_nb]
dgr_pdec:
        dec     eax
dgr_setf:
        mov     [dg_focus],eax
        jmp     dgr_loop
dgr_enter:
        mov     eax,[dg_focus]
        jmp     short dgr_out
dgr_esc:
        mov     eax,[dg_esc]
dgr_out:
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

dg_preview:
        cmp     dword ptr [dg_pvfn],0
        je      dgp_out
        call    dword ptr [dg_pvfn]
dgp_out:
        ret

;--- dg_inside - CF clear if ev_row, ev_col is on the box ------------
dg_inside:
        push    eax
        mov     eax,[ev_row]
        cmp     eax,[dg_top]
        jb      dgi_no
        cmp     eax,[dg_bot]
        ja      dgi_no
        mov     eax,[ev_col]
        cmp     eax,[dg_left]
        jb      dgi_no
        cmp     eax,[dg_right]
        ja      dgi_no
        pop     eax
        clc
        ret
dgi_no:
        pop     eax
        stc
        ret

;--- dg_btnat - EAX = the button at ev_row, ev_col, or -1 ------------
dg_btnat:
        push    ebx
        push    ecx
        mov     ecx,[ev_col]
        mov     eax,[ev_row]
        cmp     eax,[dg_brow]
        jne     dba_none
        xor     ebx,ebx
dba_one:
        cmp     ebx,[dg_nb]
        jae     dba_none
        mov     eax,[dg_bcol+ebx*4]
        cmp     ecx,eax
        jb      dba_next
        add     eax,[dg_bw+ebx*4]
        cmp     ecx,eax
        jb      dba_hit
dba_next:
        inc     ebx
        jmp     short dba_one
dba_hit:
        mov     eax,ebx
        pop     ecx
        pop     ebx
        ret
dba_none:
        mov     eax,-1
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; dg_layout - how big the box is and where everything in it goes.  As
;             wide as the widest thing in it with a column of space
;             either side; down from the top a border, a blank row, the
;             lines, the field, the check box, a blank row, the buttons
;             and a border.
;---------------------------------------------------------------------
dg_layout:
        pushad
        xor     ebp,ebp                 ; EBP = the widest so far
        mov     edx,[dg_title]
        or      edx,edx
        jz      dgl_lines
        call    sd_len
        add     ecx,4
        mov     ebp,ecx
dgl_lines:
        xor     ebx,ebx
dgl_line:
        cmp     ebx,[dg_n]
        jae     dgl_field
        mov     edx,[dg_lines+ebx*4]
        call    sd_len
        cmp     ecx,ebp
        jbe     dgl_lnext
        mov     ebp,ecx
dgl_lnext:
        inc     ebx
        jmp     short dgl_line
dgl_field:
        mov     edx,[dg_flabel]
        or      edx,edx
        jz      dgl_check
        call    sd_len
        add     ecx,FIELD_W + 1
        cmp     ecx,ebp
        jbe     dgl_check
        mov     ebp,ecx
dgl_check:
        mov     edx,[dg_clabel]
        or      edx,edx
        jz      dgl_btns
        call    sd_len
        add     ecx,4
        cmp     ecx,ebp
        jbe     dgl_btns
        mov     ebp,ecx
dgl_btns:
        xor     ebx,ebx
        xor     edi,edi                 ; EDI = all the buttons, and the gaps
dgl_btn:
        cmp     ebx,[dg_nb]
        jae     dgl_bdone
        mov     edx,[dg_btns+ebx*4]
        call    sd_len
        add     ecx,4                   ; "[ " and " ]"
        mov     [dg_bw+ebx*4],ecx
        add     edi,ecx
        or      ebx,ebx
        jz      dgl_bnext
        add     edi,2
dgl_bnext:
        inc     ebx
        jmp     short dgl_btn
dgl_bdone:
        mov     [dg_btot],edi
        cmp     edi,ebp
        jbe     dgl_width
        mov     ebp,edi
dgl_width:
        add     ebp,2
        cmp     ebp,TUI_COLS - 6
        jbe     dgl_wok
        mov     ebp,TUI_COLS - 6
dgl_wok:
        mov     [dg_inner],ebp
        mov     eax,TUI_COLS - 2
        sub     eax,ebp
        shr     eax,1
        mov     [dg_left],eax
        add     eax,ebp
        inc     eax
        mov     [dg_right],eax
        ; how tall
        mov     eax,[dg_n]
        add     eax,5
        cmp     dword ptr [dg_flabel],0
        je      dgl_hcheck
        add     eax,2
dgl_hcheck:
        cmp     dword ptr [dg_clabel],0
        je      dgl_htop
        inc     eax
        cmp     dword ptr [dg_flabel],0
        jne     dgl_htop
        inc     eax
dgl_htop:
        ; in the lower half, under the bar, when it fits there
        mov     ecx,LP_BOT - LP_TOP + 1
        cmp     eax,ecx
        ja      dgl_tall
        sub     ecx,eax
        shr     ecx,1
        add     ecx,LP_TOP
        jmp     short dgl_top
dgl_tall:
        mov     ecx,ROW_STAT
        sub     ecx,eax
        jg      dgl_top
        mov     ecx,1
dgl_top:
        mov     [dg_top],ecx
        lea     eax,[ecx+2]
        mov     [dg_lrow],eax
        add     eax,[dg_n]
        cmp     dword ptr [dg_flabel],0
        je      dgl_rcheck
        inc     eax
        mov     [dg_frow],eax
        inc     eax
dgl_rcheck:
        cmp     dword ptr [dg_clabel],0
        je      dgl_rbtn
        cmp     dword ptr [dg_flabel],0
        jne     dgl_rcrow
        inc     eax
dgl_rcrow:
        mov     [dg_crow],eax
        inc     eax
dgl_rbtn:
        inc     eax
        mov     [dg_brow],eax
        inc     eax
        mov     [dg_bot],eax
        ; the buttons, in the middle of their row
        mov     eax,[dg_inner]
        sub     eax,[dg_btot]
        shr     eax,1
        add     eax,[dg_left]
        inc     eax
        xor     ebx,ebx
dgl_bpos:
        cmp     ebx,[dg_nb]
        jae     dgl_out
        mov     [dg_bcol+ebx*4],eax
        add     eax,[dg_bw+ebx*4]
        add     eax,2
        inc     ebx
        jmp     short dgl_bpos
dgl_out:
        popad
        ret

;--- dg_draw - the screen, and the box over it -----------------------
dg_draw:
        pushad
        call    tu_build
        mov     al,[dg_attr]
        mov     [scr_cattr],al
        mov     ebx,[dg_top]
        mov     ecx,[dg_left]
        mov     edx,[dg_bot]
        mov     esi,[dg_right]
        mov     eax,1
        call    scr_box
        call    scr_shade
        mov     edx,[dg_title]
        or      edx,edx
        jz      dgd_lines
        mov     ebx,[dg_top]
        mov     ecx,[dg_left]
        add     ecx,2
        call    scr_at
        mov     al,' '
        call    scr_putc
        call    scr_puts
        call    scr_putc
dgd_lines:
        xor     ebp,ebp
dgd_line:
        cmp     ebp,[dg_n]
        jae     dgd_field
        mov     ebx,[dg_lrow]
        add     ebx,ebp
        mov     ecx,[dg_left]
        add     ecx,2
        call    scr_at
        mov     edx,[dg_lines+ebp*4]
        call    scr_puts
        inc     ebp
        jmp     short dgd_line
dgd_field:
        cmp     dword ptr [dg_flabel],0
        je      dgd_check
        mov     ebx,[dg_frow]
        mov     ecx,[dg_left]
        add     ecx,2
        call    scr_at
        mov     edx,[dg_flabel]
        call    scr_puts
        mov     al,' '
        call    scr_putc
        mov     byte ptr [scr_cattr],A_FIELD
        mov     esi,OFFSET dg_fbuf
        mov     ecx,FIELD_W
dgd_fch:
        mov     al,[esi]
        or      al,al
        jz      dgd_fcur
        call    scr_putc
        inc     esi
        dec     ecx
        jnz     dgd_fch
        jmp     short dgd_fdone
dgd_fcur:
        mov     al,'_'
        call    scr_putc
        dec     ecx
        mov     al,' '
        call    scr_fill
dgd_fdone:
        mov     al,[dg_attr]
        mov     [scr_cattr],al
dgd_check:
        cmp     dword ptr [dg_clabel],0
        je      dgd_btns
        mov     ebx,[dg_crow]
        mov     ecx,[dg_left]
        add     ecx,2
        call    scr_at
        mov     edx,OFFSET msg_cboff
        cmp     byte ptr [dg_cval],0
        je      dgd_cbox
        mov     edx,OFFSET msg_cbon
dgd_cbox:
        call    scr_puts
        mov     edx,[dg_clabel]
        call    scr_puts
dgd_btns:
        xor     ebp,ebp
dgd_btn:
        cmp     ebp,[dg_nb]
        jae     dgd_flush
        mov     byte ptr [scr_cattr],A_BTN
        cmp     ebp,[dg_focus]
        jne     dgd_battr
        mov     byte ptr [scr_cattr],A_BTNHI
dgd_battr:
        mov     ebx,[dg_brow]
        mov     ecx,[dg_bcol+ebp*4]
        call    scr_at
        mov     edx,OFFSET msg_bopen
        call    scr_puts
        mov     edx,[dg_btns+ebp*4]
        call    scr_puts
        mov     edx,OFFSET msg_bclose
        call    scr_puts
        inc     ebp
        jmp     short dgd_btn
dgd_flush:
        call    scr_flush
        popad
        ret

;=====================================================================
; Building text a line at a time.  ln_new starts the next line of the
; box; lb_into points the builder at any other buffer.  The lb_
; routines append and keep a '$' after what they wrote, and stop at the
; end of the line rather than run past it.
;=====================================================================
ln_new:
        pushad
        mov     eax,[dg_n]
        cmp     eax,DG_MAXLINES
        jae     lnn_full
        imul    edi,eax,DG_LINEW + 1
        add     edi,OFFSET dg_text
        mov     [dg_lines+eax*4],edi
        inc     dword ptr [dg_n]
        jmp     short lnn_set
lnn_full:
        mov     edi,OFFSET lb_spill     ; a line too many goes nowhere
lnn_set:
        mov     ecx,DG_LINEW
        call    lb_into
        popad
        ret

lb_into:                                ; EDI -> ECX characters' room
        mov     [lb_ptr],edi
        mov     [lb_beg],edi
        mov     byte ptr [edi],'$'
        add     edi,ecx
        mov     [lb_end],edi
        sub     edi,ecx
        ret

lb_putc:                                ; AL
        push    edi
        mov     edi,[lb_ptr]
        cmp     edi,[lb_end]
        jae     lbc_out
        mov     [edi],al
        inc     edi
        mov     byte ptr [edi],'$'
        mov     [lb_ptr],edi
lbc_out:
        pop     edi
        ret

lb_puts:                                ; EDX -> a '$' string
        push    eax
        push    esi
        mov     esi,edx
lbs_one:
        mov     al,[esi]
        cmp     al,'$'
        je      lbs_out
        call    lb_putc
        inc     esi
        jmp     short lbs_one
lbs_out:
        pop     esi
        pop     eax
        ret

lb_mb:                                  ; EAX sectors, as "40.8 MB"
        call    lb_mbn
        push    edx
        mov     edx,OFFSET msg_mbunit
        call    lb_puts
        pop     edx
        ret

lb_mbn:                                 ; ...and as "40.8"
        push    ebx
        push    edx
        mov     ebx,1
        call    fmt_mb
        call    lb_puts
        pop     edx
        pop     ebx
        ret

lb_num:                                 ; EAX in decimal
        push    ebx
        push    edx
        mov     ebx,1
        call    fmt_n
        call    lb_puts
        pop     edx
        pop     ebx
        ret

lb_trim:                                ; the spaces off the end
        push    edi
        mov     edi,[lb_ptr]
lbt_one:
        cmp     edi,[lb_beg]
        jbe     lbt_out
        cmp     byte ptr [edi-1],' '
        jne     lbt_out
        dec     edi
        jmp     short lbt_one
lbt_out:
        mov     byte ptr [edi],'$'
        mov     [lb_ptr],edi
        pop     edi
        ret

;---------------------------------------------------------------------
; ln_captail - the last ECX lines that are not blank of what the verb
;              printed, each a line of the box, the indent taken off.
;---------------------------------------------------------------------
ln_captail:
        pushad
        mov     [ct_want],ecx
        call    vw_index
        mov     ebx,[vw_n]              ; walk back to the first one to take
        mov     ecx,[ct_want]
lct_back:
        or      ebx,ebx
        jz      lct_take
        or      ecx,ecx
        jz      lct_take
        dec     ebx
        call    vw_blank
        jc      lct_back
        dec     ecx
        jmp     short lct_back
lct_take:
        cmp     ebx,[vw_n]
        jae     lct_out
        call    vw_blank
        jc      lct_next
        call    ln_new
        mov     esi,[vw_starts+ebx*4]
        mov     edi,OFFSET cap_buf
        add     edi,[cap_len]           ; EDI = the end of it all
        xor     edx,edx                 ; EDX = indent skipped so far
lct_ch:
        cmp     esi,edi
        jae     lct_next
        mov     al,[esi]
        inc     esi
        cmp     al,10
        je      lct_next
        cmp     al,13
        je      lct_ch
        cmp     edx,2
        jae     lct_put
        inc     edx
        cmp     al,' '
        je      lct_ch
        mov     edx,2
lct_put:
        call    lb_putc
        jmp     short lct_ch
lct_next:
        inc     ebx
        jmp     short lct_take
lct_out:
        popad
        ret

;=====================================================================
; Numbers in the boxes.
;
; mb_text - EAX sectors as megabytes to a tenth, at EDI, ended with a 0:
;           what a field offers before anything is typed in it.
; atomb   - ESI -> what was typed: megabytes, a whole number or with up
;           to two places after the point, and an M or MB after it if
;           wanted.  -> EAX sectors; CF set if it is not that.
;=====================================================================
mb_text:
        pushad
        mov     ebx,1
        call    fmt_mb
mbt_one:
        mov     al,[edx]
        cmp     al,'$'
        je      mbt_out
        mov     [edi],al
        inc     edi
        inc     edx
        jmp     short mbt_one
mbt_out:
        mov     byte ptr [edi],0
        popad
        ret

atomb:
        push    ebx
        push    ecx
        push    edx
        push    esi
        xor     ebx,ebx                 ; EBX = whole megabytes
        xor     ecx,ecx                 ; ECX = digits seen
amb_lead:
        cmp     byte ptr [esi],' '
        jne     amb_whole
        inc     esi
        jmp     short amb_lead
amb_whole:
        movzx   eax,byte ptr [esi]
        sub     al,'0'
        jb      amb_point
        cmp     al,9
        ja      amb_point
        cmp     ebx,2097151 / 10 + 1    ; nothing past two terabytes
        jae     amb_bad
        imul    ebx,ebx,10
        add     ebx,eax
        inc     ecx
        inc     esi
        jmp     short amb_whole
amb_point:
        imul    ebx,ebx,100             ; hundredths from here
        cmp     byte ptr [esi],'.'
        jne     amb_tail
        inc     esi
        mov     edx,10                  ; what the next digit is worth
amb_frac:
        movzx   eax,byte ptr [esi]
        sub     al,'0'
        jb      amb_tail
        cmp     al,9
        ja      amb_tail
        imul    eax,edx
        add     ebx,eax
        inc     ecx
        inc     esi
        mov     eax,edx                 ; a tenth, then a hundredth, then
        xor     edx,edx                 ; nothing: further places are read
        push    ecx                     ; and not counted
        mov     ecx,10
        div     ecx
        pop     ecx
        mov     edx,eax
        jmp     short amb_frac
amb_tail:
        cmp     byte ptr [esi],' '
        jne     amb_unit
        inc     esi
        jmp     short amb_tail
amb_unit:
        mov     al,[esi]
        call    fd_upper
        cmp     al,'M'
        jne     amb_end
        inc     esi
        mov     al,[esi]
        call    fd_upper
        cmp     al,'B'
        jne     amb_end
        inc     esi
amb_end:
        cmp     byte ptr [esi],' '
        jne     amb_last
        inc     esi
        jmp     short amb_end
amb_last:
        cmp     byte ptr [esi],0
        jne     amb_bad
        or      ecx,ecx
        jz      amb_bad
        ; sectors = hundredths * 2048 / 100 = hundredths * 512 / 25
        mov     eax,ebx
        mov     ecx,512
        mul     ecx
        mov     ecx,25
        cmp     edx,ecx
        jae     amb_bad
        div     ecx
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
amb_bad:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; vw_show - EDX -> a title: all of cap_buf, a screen at a time.
;=====================================================================
vw_show:
        pushad
        mov     [vw_title],edx
        call    vw_index
        mov     dword ptr [vw_top],0
vws_loop:
        call    vw_draw
        call    ev_get
        cmp     eax,EV_KEY
        je      vws_key
        cmp     eax,EV_DOWN
        je      vws_click
        cmp     eax,EV_DOUBLE
        je      vws_click
        cmp     eax,EV_EOF
        je      vws_out
        jmp     short vws_loop
vws_click:
        mov     eax,[ev_row]
        cmp     eax,ROW_STAT
        je      vws_out                 ; the line that says how to leave
        or      eax,eax
        jnz     vws_page
        cmp     dword ptr [ev_col],3
        jb      vws_out                 ; the close box
        jmp     short vws_loop
vws_page:
        mov     ecx,-VW_ROWS            ; the top half a page back, the
        cmp     eax,TUI_ROWS / 2        ; bottom half a page on
        jb      vws_move
        mov     ecx,VW_ROWS
        jmp     short vws_move
vws_key:
        mov     ax,[ev_kcode]
        cmp     al,1Bh
        je      vws_out
        cmp     al,0Dh
        je      vws_out
        mov     ecx,VW_ROWS
        cmp     al,' '
        je      vws_move
        or      al,al
        jz      vws_grey
        call    fd_upper
        cmp     al,'Q'
        je      vws_out
        cmp     al,'V'
        je      vws_out
        jmp     short vws_loop
vws_grey:
        mov     ecx,-1
        cmp     ah,SC_UP
        je      vws_move
        mov     ecx,1
        cmp     ah,SC_DOWN
        je      vws_move
        mov     ecx,-VW_ROWS
        cmp     ah,SC_PGUP
        je      vws_move
        mov     ecx,VW_ROWS
        cmp     ah,SC_PGDN
        je      vws_move
        mov     ecx,-VW_MAXLINES
        cmp     ah,SC_HOME
        je      vws_move
        mov     ecx,VW_MAXLINES
        cmp     ah,SC_END
        je      vws_move
        jmp     vws_loop
vws_move:
        mov     eax,[vw_top]
        add     eax,ecx
        jns     vws_low
        xor     eax,eax
vws_low:
        mov     edx,[vw_n]              ; the last screenful stays full
        sub     edx,VW_ROWS
        jns     vws_max
        xor     edx,edx
vws_max:
        cmp     eax,edx
        jbe     vws_set
        mov     eax,edx
vws_set:
        mov     [vw_top],eax
        jmp     vws_loop
vws_out:
        popad
        ret

vw_draw:
        pushad
        mov     edi,OFFSET scr_buf
        mov     eax,(A_PANE SHL 8) OR ' '
        mov     ecx,TUI_CELLS
        rep     stosw
        mov     byte ptr [scr_cattr],A_DESK
        xor     ebx,ebx
        xor     ecx,ecx
        call    scr_at
        mov     al,' '
        mov     ecx,TUI_COLS
        call    scr_fill
        xor     ecx,ecx
        call    scr_at
        mov     edx,OFFSET msg_closebox
        call    scr_puts
        mov     edx,[vw_title]
        call    scr_centre
        mov     byte ptr [scr_cattr],A_PANE
        xor     ebp,ebp
vwd_row:
        cmp     ebp,VW_ROWS
        jae     vwd_foot
        mov     eax,[vw_top]
        add     eax,ebp
        cmp     eax,[vw_n]
        jae     vwd_foot
        lea     ebx,[ebp+1]
        xor     ecx,ecx                 ; the verbs print 80 wide: all of it
        call    scr_at
        mov     esi,[vw_starts+eax*4]
        mov     edi,OFFSET cap_buf
        add     edi,[cap_len]
        mov     ecx,TUI_COLS
vwd_ch:
        cmp     esi,edi
        jae     vwd_next
        mov     al,[esi]
        inc     esi
        cmp     al,10
        je      vwd_next
        cmp     al,13
        je      vwd_ch
        jecxz   vwd_ch
        call    scr_putc
        dec     ecx
        jmp     short vwd_ch
vwd_next:
        inc     ebp
        jmp     short vwd_row
vwd_foot:
        mov     byte ptr [scr_cattr],A_STAT
        mov     ebx,ROW_STAT
        xor     ecx,ecx
        call    scr_at
        mov     al,' '
        mov     ecx,TUI_COLS
        call    scr_fill
        mov     ecx,1
        call    scr_at
        mov     edx,OFFSET msg_vwfoot
        call    scr_puts
        call    scr_flush
        popad
        ret

;--- vw_index - where each line of cap_buf starts, up to VW_MAXLINES -
vw_index:
        pushad
        mov     dword ptr [vw_n],0
        mov     esi,OFFSET cap_buf
        mov     ecx,[cap_len]
        mov     edi,OFFSET vw_starts
        or      ecx,ecx
        jz      vwi_out
        mov     [edi],esi
        add     edi,4
        inc     dword ptr [vw_n]
vwi_one:
        or      ecx,ecx
        jz      vwi_out
        mov     al,[esi]
        inc     esi
        dec     ecx
        cmp     al,10
        jne     vwi_one
        or      ecx,ecx
        jz      vwi_out
        cmp     dword ptr [vw_n],VW_MAXLINES
        jae     vwi_out
        mov     [edi],esi
        add     edi,4
        inc     dword ptr [vw_n]
        jmp     short vwi_one
vwi_out:
        popad
        ret

;--- vw_blank - CF set if line EBX of cap_buf has nothing but spaces -
vw_blank:
        push    eax
        push    esi
        push    edi
        mov     esi,[vw_starts+ebx*4]
        mov     edi,OFFSET cap_buf
        add     edi,[cap_len]
vwb_one:
        cmp     esi,edi
        jae     vwb_yes
        mov     al,[esi]
        inc     esi
        cmp     al,10
        je      vwb_yes
        cmp     al,13
        je      vwb_one
        cmp     al,' '
        je      vwb_one
        pop     edi
        pop     esi
        pop     eax
        clc
        ret
vwb_yes:
        pop     edi
        pop     esi
        pop     eax
        stc
        ret

;=====================================================================
; Data for the menus, the boxes and the viewer.
;=====================================================================
mu_tbl      dd  msg_mtfile, 1, SC_ALTF, mi_file, 2, 22
            dd  msg_mtdisk, 7, SC_ALTD, mi_disk, 0, DM_W + 2
            dd  msg_mtpart, 13, SC_ALTP, mi_part, 6, 23
            dd  msg_mthelp, 24, SC_ALTH, mi_help, 2, 23

mi_file     dd  CMD_LOG, msg_milog, 'O'
            dd  CMD_EXIT, msg_miexit, 'X'
mi_part     dd  CMD_MOVE, msg_mimove, 'M'
            dd  CMD_SIZE, msg_misize, 'S'
            dd  CMD_CREATE, msg_micreate, 'C'
            dd  CMD_DELETE, msg_midelete, 'D'
            dd  CMD_ACTIVE, msg_miactive, 'A'
            dd  CMD_DETAILS, msg_midetails, 'P'
mi_help     dd  CMD_HELP, msg_mikeys, 'K'
            dd  CMD_ABOUT, msg_miabout, 'A'
mi_diskfix  dd  CMD_SEP, msg_misep, 0
            dd  CMD_NEXTDISK, msg_minext, 'N'
            dd  CMD_REREAD, msg_mireread, 'R'
            dd  CMD_BOOT, msg_miboot, 'B'
mi_disk     db  MU_DISKMAX * MI_LEN dup(0)
dm_text     db  MAX_DISKS * (DM_W + 1) dup(0)
mu_dsk      db  DSK_LEN dup(0)

mu_open     dd  -1                      ; the open menu, or -1
mu_hi       dd  0                       ; its item lit
mu_dragged  db  0

dg_attr     db  A_DLG
dg_cval     db  0                       ; the check box: 1 = ticked
dg_armed    db  0
dg_fresh    db  0                       ; the field still holds the offer
            db  0,0,0
dg_n        dd  0
dg_lines    dd  DG_MAXLINES dup(0)
dg_title    dd  0
dg_flabel   dd  0
dg_clabel   dd  0
dg_nb       dd  0
dg_btns     dd  DG_MAXBTN dup(0)
dg_focus    dd  0
dg_esc      dd  0
dg_pvfn     dd  0
dg_flen     dd  0
dg_inner    dd  0
dg_btot     dd  0
dg_top      dd  0
dg_left     dd  0
dg_bot      dd  0
dg_right    dd  0
dg_lrow     dd  0
dg_frow     dd  0
dg_crow     dd  0
dg_brow     dd  0
dg_bcol     dd  DG_MAXBTN dup(0)
dg_bw       dd  DG_MAXBTN dup(0)
dg_fbuf     db  FIELD_MAX + 2 dup(0)
            db  0,0
dg_text     db  DG_MAXLINES * (DG_LINEW + 1) dup(0)
lb_spill    db  DG_LINEW + 1 dup(0)
lb_ptr      dd  0
lb_beg      dd  0
lb_end      dd  0
ct_want     dd  0
vw_n        dd  0
vw_top      dd  0
vw_title    dd  0

msg_mtfile  db  'File$'
msg_mtdisk  db  'Disk$'
msg_mtpart  db  'Partition$'
msg_mthelp  db  'Help$'
msg_milog   db  'Output log         V$'
msg_miexit  db  'Exit               Q$'
msg_mimove  db  'Move...             M$'
msg_misize  db  'Resize...           S$'
msg_micreate db 'Create...           C$'
msg_midelete db 'Delete...         Del$'
msg_miactive db 'Make active         A$'
msg_midetails db 'Details...          P$'
msg_mikeys  db  'Keys and switches  F1$'
msg_miabout db  'About FDISK          $'
msg_misep   db  '$'
msg_minext  db  'Next disk         Tab$'
msg_mireread db 'Read it again      F5$'
msg_miboot  db  'Write boot code...  B$'
msg_mdiskn  db  '  Disk $'
msg_mnone   db  '   (none)$'

msg_bopen   db  '[ $'
msg_bclose  db  ' ]$'
msg_cboff   db  '[ ] $'
msg_cbon    db  '[X] $'
msg_vwfoot  db  'Up Down PgUp PgDn Home End to read      Esc to go back$'
