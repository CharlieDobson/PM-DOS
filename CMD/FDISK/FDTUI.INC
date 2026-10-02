;=====================================================================
; FDTUI.INC - THE SCREEN: what FDISK shows when it is run with nothing
;             after it.
;
; Included by FDISK.ASM after the verbs and before the data.  It is laid
; out the way DiskStat is - a title, a menu bar, panes on a blue desktop
; and a status line, with the mouse working everywhere - and where
; DiskStat has its usage chart this has THE DISK AS A BAR, each partition
; a block the length of its share of the disk.  A block is clicked to
; select it and DRAGGED ALONG THE BAR TO MOVE THE PARTITION, as far as
; the free space on either side of it goes.  Sizes are typed in
; megabytes and the nearest cylinder boundary is found for them.
;
; IT IS STILL A SHELL OVER THE COMMAND LINE AND NOT A SECOND
; IMPLEMENTATION.  Every change fills fd_args exactly as the command line
; would, jumps to the same verb, and reads the same fd_done or fd_fail at
; the end of it.  Each change runs twice: once as a DRY RUN, whose
; verdict decides whether it is offered at all and whose numbers are
; what the bar shows, and once for real after a question that has to be
; answered yes.  While the screen is up the verbs print into cap_buf;
; File > Output log shows the last of it.
;
; TESTABLE FROM A FILE.  With FDTEST=path in the environment the keys and
; the mouse come from standard input - FDEVENT.INC says how - and
; DISK\FDMTEST.BAT with TOOLS\MKFDKEYS.PL is that test.
;
; The pieces: this file has the session and what each command does;
; FDDRAW.INC draws the screen, FDDLG.INC has the menus, the boxes and
; the text viewer, and FDEVENT.INC the keyboard, the mouse and the key
; file.  Every equate they use is here at the top, because wasm will not
; take one it has not met yet.
;=====================================================================

TUI_COLS        EQU     80
TUI_ROWS        EQU     25
TUI_CELLS       EQU     TUI_COLS * TUI_ROWS

;--- where everything is ---------------------------------------------
ROW_MENU        EQU     1
DP_TOP          EQU     2               ; the disk pane, border to border
DP_BOT          EQU     9
DP_LEFT         EQU     1
DP_RIGHT        EQU     78
ROW_LEGEND      EQU     3
ROW_BAR         EQU     4               ; the bar is three rows deep...
BAR_ROWS        EQU     3
ROW_LABEL       EQU     5               ; ...and the middle one is labelled
ROW_MARK        EQU     7               ; the bracket under the selection
ROW_SCALE       EQU     8
BAR_COL         EQU     3
BAR_W           EQU     74
LP_TOP          EQU     10              ; the list of partitions
LP_BOT          EQU     23
LP_LEFT         EQU     1
LP_RIGHT        EQU     47
ROW_LHEAD       EQU     11
ROW_LFIRST      EQU     12
LIST_ROWS       EQU     LP_BOT - ROW_LFIRST
LIST_COL        EQU     LP_LEFT + 1
LIST_W          EQU     44              ; columns 2 to 45; 46 is the scroll bar
SCROLL_COL      EQU     LP_RIGHT - 1
LSC_NUM         EQU     3               ; the list's columns
LSC_ACT         EQU     6
LSC_DRV         EQU     8
LSC_TYPE        EQU     12
LSC_SIZE        EQU     22
LSC_START       EQU     32
IP_LEFT         EQU     49              ; what the selection is
IP_RIGHT        EQU     78
IP_TEXT         EQU     IP_LEFT + 2
IP_VALUE        EQU     IP_LEFT + 13
ROW_STAT        EQU     24
VW_ROWS         EQU     TUI_ROWS - 2    ; the viewer's text rows

;--- DiskStat's colours ----------------------------------------------
A_DESK          EQU     17h             ; white on blue
A_MENU          EQU     70h             ; black on grey
A_MENUHI        EQU     07h             ; grey on black: the one lit
A_PANE          EQU     70h
A_PHEAD         EQU     71h             ; blue on grey: headings, scroll bar
A_PDIM          EQU     78h             ; dark grey on grey
A_SEL           EQU     1Fh             ; bright white on blue
A_STAT          EQU     3Bh             ; light cyan on cyan
A_DLG           EQU     1Fh             ; a box: bright white on blue...
A_DLGERR        EQU     4Fh             ; ...or on red, when it says no
A_BTN           EQU     70h
A_BTNHI         EQU     3Fh             ; the button Enter presses
A_FIELD         EQU     0Fh
A_SHADOW        EQU     08h
A_FREE          EQU     78h             ; free space, a light shade...
A_CONT          EQU     07h             ; ...the extended partition a darker one
A_PVOK          EQU     2Fh             ; white on green: it fits
A_PVNO          EQU     4Fh             ; white on red: it does not
GL_FREE         EQU     0B0h
GL_CONT         EQU     0B1h
TUI_NCOLORS     EQU     4

CAP_MAX         EQU     8192
VW_MAXLINES     EQU     512

;--- one row of the table: a partition, or a run of free space -------
TR_KIND         EQU     00h             ; dd - TK_*
TR_NUM          EQU     04h             ; dd - what the command line calls
                                        ;      it; 0 for free space
TR_TYPE         EQU     08h             ; dd - the type byte
TR_ACT          EQU     0Ch             ; dd - 1 if active
TR_START        EQU     10h             ; dd - absolute first sector
TR_SECS         EQU     14h             ; dd
TR_LETTER       EQU     18h             ; dd - 0, or 'C'
TR_EBR          EQU     1Ch             ; dd - a logical drive's own record
TR_ATTR         EQU     20h             ; dd - its colour on the bar
TR_RSVD         EQU     24h
TR_LEN          EQU     28h
TR_MAX          EQU     PART_MAX + EBR_MAX * 2 + 8

TK_PRIMARY      EQU     1
TK_CONTAINER    EQU     2               ; the extended partition itself
TK_LOGICAL      EQU     3
TK_FREE         EQU     4               ; space outside every partition
TK_LFREE        EQU     5               ; space inside the extended one

PV_NONE         EQU     0
PV_REPLACE      EQU     1               ; the selected row, somewhere else
PV_ADD          EQU     2               ; a new span
PV_OMIT         EQU     3               ; the selected row, gone

IV_MAX          EQU     EBR_MAX + 4     ; spans the gap finder sorts

;--- events (FDEVENT.INC) --------------------------------------------
EV_KEY          EQU     1
EV_DOWN         EQU     2
EV_UP           EQU     3
EV_DRAG         EQU     4
EV_DOUBLE       EQU     5
EV_EOF          EQU     6
MQ_MAX          EQU     8               ; mouse events queued
MQ_LEN          EQU     12              ; kind, row, column
DBL_TIME        EQU     40              ; hundredths: a second press inside
                                        ; this is a double click
SHIFT_KEYS      EQU     0Fh             ; 40:17's shift, Ctrl and Alt bits

;--- scan codes ------------------------------------------------------
SC_SHTAB        EQU     0Fh
SC_ALTP         EQU     19h
SC_ALTD         EQU     20h
SC_ALTF         EQU     21h
SC_ALTH         EQU     23h
SC_ALTX         EQU     2Dh
SC_F1           EQU     3Bh
SC_F5           EQU     3Fh
SC_F10          EQU     44h
SC_HOME         EQU     47h
SC_UP           EQU     48h
SC_PGUP         EQU     49h
SC_LEFT         EQU     4Bh
SC_RIGHT        EQU     4Dh
SC_END          EQU     4Fh
SC_DOWN         EQU     50h
SC_PGDN         EQU     51h
SC_DEL          EQU     53h

;--- what a key, a menu item or a click asks for ---------------------
CMD_NONE        EQU     0
CMD_EXIT        EQU     1
CMD_LOG         EQU     2
CMD_NEXTDISK    EQU     3
CMD_PREVDISK    EQU     4
CMD_REREAD      EQU     5
CMD_BOOT        EQU     6
CMD_MOVE        EQU     7
CMD_SIZE        EQU     8
CMD_CREATE      EQU     9
CMD_DELETE      EQU     10
CMD_ACTIVE      EQU     11
CMD_DETAILS     EQU     12
CMD_HELP        EQU     13
CMD_ABOUT       EQU     14
CMD_SEP         EQU     15              ; a line between menu items
CMD_MENU0       EQU     16              ; open menu 0..3
CMD_MENU3       EQU     19
CMD_DISK0       EQU     32              ; look at disk 0, 1, ...

;--- the menus (FDDLG.INC) -------------------------------------------
MU_COUNT        EQU     4
MU_TITLE        EQU     00h             ; dd -> its name
MU_COL          EQU     04h             ; dd - where " name " starts on the bar
MU_ALT          EQU     08h             ; dd - the scan code Alt+letter gives
MU_ITEMS        EQU     0Ch             ; dd -> its items
MU_N            EQU     10h             ; dd - how many
MU_W            EQU     14h             ; dd - the box's inside width
MU_LEN          EQU     18h
MI_CMD          EQU     00h             ; dd - CMD_*
MI_LABEL        EQU     04h             ; dd -> the text, padded to the width
MI_KEY          EQU     08h             ; dd - the letter that picks it
MI_LEN          EQU     0Ch
MU_DISKMAX      EQU     MAX_DISKS + 4
DM_W            EQU     21              ; a Disk menu item's text

;--- the boxes (FDDLG.INC) -------------------------------------------
DG_MAXLINES     EQU     8
DG_LINEW        EQU     70
DG_MAXBTN       EQU     3
FIELD_W         EQU     12              ; the typing field, on the screen...
FIELD_MAX       EQU     10              ; ...and how much it takes

;=====================================================================
; fd_tui - the whole session.  Entered from fd_entry; leaves through
;          fd_done with fd_menu cleared, so that is a real exit.
;=====================================================================
fd_tui:
        mov     byte ptr [fd_menu],1
        mov     byte ptr [tui_on],1
        call    vol_load
        call    tui_ktinit
        call    ev_init

        ; how many disks the kernel will talk about
        mov     edi,OFFSET dsk_rec
        mov     edx,80h
        mov     eax,0F10Ch
        int     21h
        mov     [tui_ndisks],ecx
        cmp     ecx,MAX_DISKS
        jbe     tu_nd_ok
        mov     dword ptr [tui_ndisks],MAX_DISKS
tu_nd_ok:
        or      ecx,ecx
        jnz     tu_nd_some
        mov     dword ptr [tui_ndisks],1  ; one disk saying "no disk" beats
tu_nd_some:                               ; a screen with nothing on it
        call    mu_disks                ; the Disk menu, an item a disk
        xor     eax,eax
        mov     [tui_disk],eax
        mov     [tr_sel],eax
        mov     [ls_top],eax
        mov     [cap_len],eax
        mov     [tu_stat],eax
        mov     [pv_mode],eax
        mov     dword ptr [mu_open],-1
        mov     dword ptr [rs_start],-1
        call    tui_hidecur
        call    tui_load

tu_loop:
        call    tui_draw
        mov     dword ptr [tu_stat],0
        call    ev_get
        cmp     eax,EV_KEY
        je      tu_key
        cmp     eax,EV_DOWN
        je      tu_down
        cmp     eax,EV_DOUBLE
        je      tu_double
        cmp     eax,EV_EOF
        jne     tu_loop                 ; a let-go or a drag with nothing
        mov     eax,CMD_EXIT            ; held: nothing to do
        jmp     short tu_cmd
tu_key:
        call    tu_keycmd               ; EAX = a command, or CMD_NONE
        jmp     short tu_cmd
tu_down:
        call    tu_click
        jmp     short tu_cmd
tu_double:
        call    tu_dclick
tu_cmd:
        call    tu_do
        jmp     tu_loop

;--- tu_do - EAX = a command.  The menus hand back another one. ------
tu_do:
        cmp     eax,CMD_DISK0
        jae     tdo_disk
        cmp     eax,CMD_MENU0
        jb      tdo_plain
        cmp     eax,CMD_MENU3
        ja      tdo_none
        sub     eax,CMD_MENU0
        xor     ebx,ebx                 ; opened from the keyboard
        call    mu_run                  ; -> EAX = what was picked
        jmp     short tu_do
tdo_plain:
        cmp     eax,CMD_SEP
        jae     tdo_none
        jmp     dword ptr [cmd_tbl+eax*4]
tdo_disk:
        sub     eax,CMD_DISK0
        cmp     eax,[tui_ndisks]
        jae     tdo_none
        jmp     tu_seldisk
tdo_none:
        ret

;--- tu_keycmd - the key in ev_kcode -> EAX = a command ----------------
tu_keycmd:
        mov     ax,[ev_kcode]
        cmp     al,1Bh
        je      tkc_exit
        cmp     al,9
        je      tkc_next
        cmp     al,0Dh
        je      tkc_details
        or      al,al
        jz      tkc_grey
        call    fd_upper
        mov     esi,OFFSET key_tbl
tkc_look:
        cmp     byte ptr [esi],0
        je      tkc_unknown
        cmp     al,[esi]
        je      tkc_found
        add     esi,2
        jmp     short tkc_look
tkc_found:
        movzx   eax,byte ptr [esi+1]
        ret
tkc_exit:
        mov     eax,CMD_EXIT
        ret
tkc_next:
        mov     eax,CMD_NEXTDISK
        ret
tkc_details:
        mov     eax,CMD_DETAILS
        ret
tkc_grey:
        ; moving about the table first
        mov     ecx,-1
        cmp     ah,SC_UP
        je      tkc_step
        cmp     ah,SC_LEFT
        je      tkc_step
        mov     ecx,1
        cmp     ah,SC_DOWN
        je      tkc_step
        cmp     ah,SC_RIGHT
        je      tkc_step
        mov     ecx,-LIST_ROWS
        cmp     ah,SC_PGUP
        je      tkc_step
        mov     ecx,LIST_ROWS
        cmp     ah,SC_PGDN
        je      tkc_step
        mov     ecx,-TR_MAX
        cmp     ah,SC_HOME
        je      tkc_step
        mov     ecx,TR_MAX
        cmp     ah,SC_END
        je      tkc_step
        mov     esi,OFFSET gkey_tbl
tkc_glook:
        cmp     byte ptr [esi],0
        je      tkc_none
        cmp     ah,[esi]
        je      tkc_found
        add     esi,2
        jmp     short tkc_glook
tkc_step:
        call    tu_step
tkc_none:
        mov     eax,CMD_NONE
        ret
tkc_unknown:
        mov     dword ptr [tu_stat],OFFSET msg_tnokey
        mov     eax,CMD_NONE
        ret

;--- tu_step - ECX rows on (or back), stopping at either end ---------
tu_step:
        push    eax
        mov     eax,[tr_sel]
        add     eax,ecx
        jns     tst_low
        xor     eax,eax
tst_low:
        cmp     eax,[tr_n]
        jb      tst_set
        mov     eax,[tr_n]
        or      eax,eax
        jz      tst_set
        dec     eax
tst_set:
        mov     [tr_sel],eax
        pop     eax
        ret

;---------------------------------------------------------------------
; tu_click - the left button pressed at ev_row, ev_col.  -> EAX = a
;            command.  A press on a partition on the bar can turn into
;            a drag, which is followed to its end here.
;---------------------------------------------------------------------
tu_click:
        mov     ebx,[ev_row]
        mov     ecx,[ev_col]
        or      ebx,ebx
        jnz     tcl_menu
        cmp     ecx,3                   ; the close box in the corner
        jae     tcl_none
        mov     eax,CMD_EXIT
        ret
tcl_menu:
        cmp     ebx,ROW_MENU
        jne     tcl_bar
        call    mu_hit                  ; -> EAX = a menu, or -1
        or      eax,eax
        js      tcl_none
        mov     ebx,1                   ; opened with the button down
        call    mu_run
        ret
tcl_bar:
        call    br_hit                  ; -> EAX = a row, or -1
        or      eax,eax
        js      tcl_list
        mov     [tr_sel],eax
        call    tui_selrow
        cmp     dword ptr [esi+TR_KIND],TK_PRIMARY
        je      tcl_drag
        cmp     dword ptr [esi+TR_KIND],TK_LOGICAL
        jne     tcl_none
tcl_drag:
        call    tu_drag
        jmp     short tcl_none
tcl_list:
        call    ls_hit                  ; -> EAX = a row, or -1
        or      eax,eax
        js      tcl_none
        mov     [tr_sel],eax
tcl_none:
        mov     eax,CMD_NONE
        ret

;--- tu_dclick - pressed twice: what Enter would do to what is under it
tu_dclick:
        mov     ebx,[ev_row]
        mov     ecx,[ev_col]
        call    br_hit
        or      eax,eax
        jns     tdc_row
        call    ls_hit
        or      eax,eax
        js      tu_click                ; not on a row: an ordinary press
tdc_row:
        mov     [tr_sel],eax
        mov     eax,CMD_DETAILS
        ret

;=====================================================================
; THE DRAG.  The button went down on a partition's block; from here
; every cell the mouse crosses moves a copy of the block along the bar,
; snapped to a cylinder and held inside the free space either side of
; it, and letting go is the move - asked about first, like every other.
;=====================================================================
tu_drag:
        pushad
        call    tui_selrow
        mov     eax,[esi+TR_START]
        mov     [dr_start0],eax
        mov     eax,[esi+TR_SECS]
        mov     [dr_len],eax
        mov     eax,[ev_col]
        mov     [dr_col0],eax
        mov     byte ptr [dr_moved],0
        call    tu_room                 ; mv_lo..mv_hi: the room it has
tdg_loop:
        call    tui_draw
        call    ev_get
        cmp     eax,EV_DRAG
        je      tdg_drag
        cmp     eax,EV_UP
        je      tdg_up
        cmp     eax,EV_EOF
        je      tdg_cancel
        cmp     eax,EV_KEY
        jne     tdg_loop
        cmp     byte ptr [ev_kcode],1Bh
        jne     tdg_loop
tdg_cancel:
        mov     dword ptr [tu_stat],OFFSET msg_tleft
        jmp     short tdg_out
tdg_drag:
        mov     ecx,[ev_col]
        cmp     byte ptr [dr_moved],0
        jne     tdg_track
        cmp     ecx,[dr_col0]
        je      tdg_loop                ; the same cell: still only a click
        mov     byte ptr [dr_moved],1
tdg_track:
        call    dr_ghost
        call    dr_status
        jmp     tdg_loop
tdg_up:
        cmp     byte ptr [dr_moved],0
        je      tdg_out                 ; it was a click
        mov     ecx,[ev_col]
        call    dr_ghost
        mov     eax,[pv_start]
        cmp     eax,[dr_start0]
        je      tdg_cancel              ; back where it started, or no room
        call    tu_canmove
        jc      tdg_notfat
        call    tu_trymove              ; asks, and does it
        jmp     short tdg_out
tdg_notfat:
        mov     dword ptr [pl_status],RL_NOTFAT
        call    tu_refusal
tdg_out:
        mov     dword ptr [pv_mode],PV_NONE
        popad
        ret

;--- dr_ghost - ECX = the mouse's column -> pv_* -----------------------
dr_ghost:
        pushad
        mov     eax,ecx
        sub     eax,[dr_col0]           ; cells, either way
        mov     ebx,eax
        or      eax,eax
        jns     drg_mag
        neg     eax
drg_mag:
        cmp     eax,BAR_W               ; never more than the whole bar, so
        jbe     drg_cells               ; the division below cannot overflow
        mov     eax,BAR_W
drg_cells:
        mul     dword ptr [dsk_rec+DSK_TOTAL]
        mov     ecx,BAR_W
        div     ecx                     ; EAX = that many cells in sectors
        mov     edx,[dr_start0]
        or      ebx,ebx
        js      drg_left
        add     edx,eax
        jnc     drg_have
        mov     edx,[mv_hi]
        jmp     short drg_have
drg_left:
        sub     edx,eax
        jnc     drg_have
        xor     edx,edx
drg_have:
        mov     eax,edx
        mov     ecx,[dr_len]
        call    mv_snap                 ; inside mv_lo..mv_hi
        mov     [pv_start],eax
        mov     eax,[dr_len]
        mov     [pv_len],eax
        mov     dword ptr [pv_mode],PV_REPLACE
        mov     byte ptr [pv_attr],A_PVOK
        call    tu_canmove
        jnc     drg_out
        mov     byte ptr [pv_attr],A_PVNO
drg_out:
        popad
        ret

;--- dr_status - where the block is, on the status line --------------
dr_status:
        pushad
        mov     edi,OFFSET st_buf
        mov     ecx,TUI_COLS - 2
        call    lb_into
        mov     edx,OFFSET msg_dmove
        call    lb_puts
        call    lb_name
        mov     edx,OFFSET msg_qto
        call    lb_puts
        mov     eax,[pv_start]
        call    lb_mb
        mov     edx,OFFSET msg_dlet
        call    lb_puts
        mov     dword ptr [tu_stat],OFFSET st_buf
        popad
        ret

;=====================================================================
; THE ROOM A PARTITION HAS, and where a move may put it.
;
; tu_region - mv_lo..mv_hi = where the selected partition may be at all:
;             the disk past its first track for a primary, and for a
;             logical drive the container past its own boot record -
;             which stays where it is, a track in front of the volume.
; tu_room   - the same, narrowed to the free space around it: the
;             walls are the partitions at the same level, and for a
;             logical drive each other link is its record and its
;             volume together.
; Both also set mv_cur, where it starts now.
;=====================================================================
tu_region:
        pushad
        call    tui_selrow
        mov     eax,[esi+TR_START]
        mov     [mv_cur],eax
        movzx   ecx,word ptr [dsk_rec+DSK_SPT]
        or      ecx,ecx
        jnz     trg_spt
        inc     ecx
trg_spt:
        cmp     dword ptr [esi+TR_KIND],TK_LOGICAL
        je      trg_log
        mov     ebx,ecx                 ; a primary: past the first track
        mov     edx,dword ptr [dsk_rec+DSK_TOTAL]
        jmp     short trg_lo
trg_log:
        mov     ebx,[esi+TR_EBR]
        add     ebx,ecx
        mov     edx,[tu_cend]
trg_lo:
        cmp     ebx,eax
        jbe     trg_set
        mov     ebx,eax                 ; it already starts nearer than that
trg_set:
        mov     [mv_lo],ebx
        mov     [mv_hi],edx
        popad
        ret

tu_room:
        pushad
        call    tu_region
        call    tui_selrow
        mov     eax,[esi+TR_START]
        add     eax,[esi+TR_SECS]
        mov     [rm_end],eax
        xor     edi,edi                 ; EDI = 1 for a primary's walls
        cmp     dword ptr [esi+TR_KIND],TK_LOGICAL
        je      trm_mode
        inc     edi
trm_mode:
        xor     ebp,ebp
trm_one:
        cmp     ebp,[tr_n]
        jae     trm_done
        cmp     ebp,[tr_sel]
        je      trm_next
        imul    ebx,ebp,TR_LEN
        add     ebx,OFFSET tr_tbl
        call    tu_wall                 ; -> EAX..ECX, or CF
        jc      trm_next
        cmp     ecx,[mv_cur]
        ja      trm_ahead
        cmp     ecx,[mv_lo]             ; behind it: the room starts past it
        jbe     trm_next
        mov     [mv_lo],ecx
        jmp     short trm_next
trm_ahead:
        cmp     eax,[rm_end]
        jb      trm_next
        cmp     eax,[mv_hi]
        jae     trm_next
        mov     [mv_hi],eax
trm_next:
        inc     ebp
        jmp     short trm_one
trm_done:
        popad
        ret

;--- tu_wall - EBX -> a row, EDI = 1 for the walls a primary meets and
;--- 0 for a logical drive's.  CF clear and EAX..ECX = the wall.
tu_wall:
        mov     eax,[ebx+TR_KIND]
        or      edi,edi
        jz      twl_log
        cmp     eax,TK_PRIMARY
        je      twl_span
        cmp     eax,TK_CONTAINER
        je      twl_span
        stc
        ret
twl_span:
        mov     eax,[ebx+TR_START]
        mov     ecx,eax
        add     ecx,[ebx+TR_SECS]
        clc
        ret
twl_log:
        cmp     eax,TK_LOGICAL
        je      twl_link
        stc
        ret
twl_link:
        mov     eax,[ebx+TR_EBR]
        mov     ecx,[ebx+TR_START]
        add     ecx,[ebx+TR_SECS]
        clc
        ret

;--- tu_fits - EAX = a start, ECX = a length for the selected partition.
;--- CF clear if nothing is in the way and it stays where it may be.
tu_fits:
        pushad
        mov     edx,eax
        add     edx,ecx                 ; EDX = its end
        jc      tfi_no
        mov     [ft_start],eax
        mov     [ft_end],edx
        call    tui_selrow
        xor     edi,edi
        cmp     dword ptr [esi+TR_KIND],TK_LOGICAL
        je      tfi_log
        inc     edi
        or      eax,eax
        jz      tfi_no
        cmp     edx,dword ptr [dsk_rec+DSK_TOTAL]
        ja      tfi_no
        jmp     short tfi_walls
tfi_log:
        cmp     eax,[esi+TR_EBR]
        jbe     tfi_no
        cmp     edx,[tu_cend]
        ja      tfi_no
tfi_walls:
        xor     ebp,ebp
tfi_one:
        cmp     ebp,[tr_n]
        jae     tfi_yes
        cmp     ebp,[tr_sel]
        je      tfi_next
        imul    ebx,ebp,TR_LEN
        add     ebx,OFFSET tr_tbl
        call    tu_wall
        jc      tfi_next
        cmp     eax,[ft_end]            ; overlapping, unless one ends
        jae     tfi_next                ; before the other starts
        cmp     ecx,[ft_start]
        ja      tfi_no
tfi_next:
        inc     ebp
        jmp     short tfi_one
tfi_yes:
        popad
        clc
        ret
tfi_no:
        popad
        stc
        ret

;---------------------------------------------------------------------
; mv_snap - EAX = where a partition of ECX sectors should start, more or
;           less -> EAX = where it will.  Its END goes to the nearest
;           cylinder boundary, which is where every partition this
;           program or DOS made ends - a volume a track behind its
;           record, or the first one a track into the disk, keeps that
;           shape - and then the whole is held inside mv_lo..mv_hi.
;           When it cannot be, the answer is mv_cur: where it is now.
;---------------------------------------------------------------------
mv_snap:
        push    ebx
        push    ecx
        push    edx
        push    esi
        mov     esi,ecx                 ; ESI = the length
        call    fd_spcyl                ; ECX = a cylinder
        mov     ebx,eax
        add     ebx,esi                 ; EBX = where it would end
        jc      mvs_top
        mov     eax,ebx
        call    cyl_near
        cmp     eax,esi
        jae     mvs_end
        mov     eax,esi                 ; rounded to before its own length
        call    fd_cylup
mvs_end:
        mov     ebx,eax
        sub     eax,esi                 ; the start that gives
        cmp     eax,[mv_lo]
        jae     mvs_hi
        mov     eax,[mv_lo]
        add     eax,esi
        call    fd_cylup
        mov     ebx,eax
mvs_hi:
        cmp     ebx,[mv_hi]
        jbe     mvs_done
mvs_top:
        mov     eax,[mv_hi]
        call    fd_cyldn
        mov     ebx,eax
mvs_done:
        mov     eax,ebx
        sub     eax,esi
        jc      mvs_none
        cmp     eax,[mv_lo]
        jae     mvs_out
mvs_none:
        mov     eax,[mv_cur]            ; no room anywhere in it
mvs_out:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- cyl_near - EAX to the nearest multiple of ECX; ECX = 0 leaves it -
cyl_near:
        or      ecx,ecx
        jz      cyn_out
        push    edx
        push    eax
        mov     edx,ecx
        shr     edx,1
        add     eax,edx
        jc      cyn_pop
        xor     edx,edx
        div     ecx
        mul     ecx
        add     esp,4                   ; the saved EAX, not wanted
        pop     edx
        ret
cyn_pop:
        pop     eax
        pop     edx
cyn_out:
        ret

;--- tu_canmove - CF clear if the selected partition holds a volume the
;--- engine moves and resizes: FAT12, FAT16 or FAT32.
tu_canmove:
        push    eax
        push    esi
        call    tui_selrow
        mov     eax,[esi+TR_TYPE]
        call    rl_isfat
        pop     esi
        pop     eax
        ret

;=====================================================================
; M - a move typed in megabytes, for the keyboard.  The mouse does the
;     same thing by dragging.
;=====================================================================
tu_move:
        pushad
        call    tui_selvol
        jc      tmo_out
        call    tu_canmove
        jc      tmo_notfat
        call    tu_room                 ; for the line about the room...
        mov     eax,[mv_lo]
        mov     [mo_rlo],eax
        mov     eax,[mv_hi]
        mov     [mo_rhi],eax
        call    tu_region               ; ...and a typed start may go anywhere
        mov     edi,OFFSET dg_fbuf
        mov     eax,[mv_cur]
        call    mb_text                 ; where it is, as the first offer
tmo_ask:
        call    mo_build
        call    dg_run
        or      eax,eax
        jnz     tmo_out
        mov     esi,OFFSET dg_fbuf
        call    atomb
        jc      tmo_bad
        call    tui_selrow
        mov     ecx,[esi+TR_SECS]
        call    mv_snap
        cmp     eax,[mv_cur]
        je      tmo_same
        call    tu_trymove
        jc      tmo_ask                 ; refused: the box again, to fix it
        jmp     short tmo_out
tmo_bad:
        call    tu_badmb
        jmp     short tmo_ask
tmo_same:
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_xthere
        call    lb_puts
        mov     edx,OFFSET msg_tmove
        mov     bl,A_DLG
        call    tu_box
        jmp     short tmo_ask
tmo_notfat:
        mov     dword ptr [pl_status],RL_NOTFAT
        call    tu_refusal
tmo_out:
        mov     dword ptr [pv_mode],PV_NONE
        popad
        ret

mo_build:
        call    dg_reset
        mov     dword ptr [dg_title],OFFSET msg_tmove
        call    ln_new
        call    lb_whole
        call    ln_new
        mov     edx,OFFSET msg_lstarts
        call    lb_puts
        mov     eax,[mv_cur]
        call    lb_mb
        call    ln_new
        mov     edx,OFFSET msg_lroom
        call    lb_puts
        mov     eax,[mo_rlo]
        call    lb_mbn
        mov     edx,OFFSET msg_lroomto
        call    lb_puts
        mov     eax,[mo_rhi]
        call    lb_mb
        call    ln_new
        call    ln_new
        mov     edx,OFFSET msg_ldrag
        call    lb_puts
        mov     dword ptr [dg_flabel],OFFSET msg_fstart
        mov     dword ptr [dg_pvfn],OFFSET mo_preview
        jmp     tu_okcancel

;--- mo_preview - the start being typed, on the bar ------------------
mo_preview:
        pushad
        mov     dword ptr [pv_mode],PV_NONE
        mov     esi,OFFSET dg_fbuf
        call    atomb
        jc      mop_out
        call    tui_selrow
        mov     ecx,[esi+TR_SECS]
        call    mv_snap
        mov     [pv_start],eax
        mov     [pv_len],ecx
        mov     dword ptr [pv_mode],PV_REPLACE
        mov     byte ptr [pv_attr],A_PVOK
        call    tu_fits
        jnc     mop_out
        mov     byte ptr [pv_attr],A_PVNO
mop_out:
        popad
        ret

;---------------------------------------------------------------------
; tu_trymove - EAX = the new start of the selected partition.  The dry
;              run, the question, and the move.  CF set when the engine
;              refused it (and has said why), clear otherwise.
;---------------------------------------------------------------------
tu_trymove:
        pushad
        mov     [mv_new],eax
        call    tm_args
        mov     eax,OFFSET fd_change
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     ttm_refused
        mov     eax,[mv_new]
        mov     [pv_start],eax
        call    tui_selrow
        mov     eax,[esi+TR_SECS]
        mov     [pv_len],eax
        mov     dword ptr [pv_mode],PV_REPLACE
        mov     byte ptr [pv_attr],A_PVOK
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_qmove
        call    lb_puts
        call    lb_name
        mov     edx,OFFSET msg_qfrom
        call    lb_puts
        call    tui_selrow
        mov     eax,[esi+TR_START]
        call    lb_mb
        mov     edx,OFFSET msg_qto
        call    lb_puts
        mov     eax,[mv_new]
        call    lb_mb
        mov     al,'?'
        call    lb_putc
        call    ln_new
        mov     edx,OFFSET msg_qmove2
        call    lb_puts
        mov     edx,OFFSET msg_tmove
        mov     bl,A_DLG
        xor     ecx,ecx
        call    tu_ask
        jc      ttm_done
        mov     eax,[mv_new]
        mov     [rs_start],eax
        call    tm_args
        mov     eax,OFFSET fd_change
        call    tu_write
ttm_done:
        popad
        clc
        ret
ttm_refused:
        call    tu_refusal
        popad
        stc
        ret

tm_args:
        mov     dword ptr [cap_len],0
        mov     dword ptr [pl_status],-1
        call    tui_setdp
        mov     eax,[mv_new]
        call    tui_setnum
        mov     dword ptr [fd_nargs],2
        mov     byte ptr [fd_cmd],FDC_MOVE
        ret

;=====================================================================
; S - a resize, typed in megabytes.  /P works out how small and how big
;     it can be first, so the box can say so and the bar can turn red
;     before the engine is even asked.
;=====================================================================
tu_size:
        pushad
        call    tui_selvol
        jc      tsz_out
        call    tu_canmove
        jc      tsz_notfat
        mov     dword ptr [cap_len],0
        mov     dword ptr [pl_status],-1
        mov     dword ptr [pl_minsz],0
        mov     dword ptr [pl_maxsz],0
        call    tui_setdp
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_PLAN
        mov     eax,OFFSET fd_plan
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     tsz_cant
        mov     eax,[pl_minsz]
        mov     [sz_min],eax
        mov     eax,[pl_maxsz]
        mov     [sz_max],eax
        call    tui_selrow
        mov     eax,[esi+TR_START]
        mov     [sz_start],eax
        mov     eax,[esi+TR_SECS]
        mov     [sz_cur],eax
        mov     edi,OFFSET dg_fbuf
        call    mb_text                 ; the size it has, as the first offer
tsz_ask:
        call    sz_build
        call    dg_run
        or      eax,eax
        jnz     tsz_out
        mov     esi,OFFSET dg_fbuf
        call    atomb
        jc      tsz_bad
        or      eax,eax
        jz      tsz_bad
        call    sz_snap
        cmp     eax,[sz_cur]
        je      tsz_same
        mov     [sz_new],eax
        call    sz_args
        mov     eax,OFFSET fd_change
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     tsz_refused
        mov     eax,[sz_start]
        mov     [pv_start],eax
        mov     eax,[sz_new]
        mov     [pv_len],eax
        mov     dword ptr [pv_mode],PV_REPLACE
        mov     byte ptr [pv_attr],A_PVOK
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_qsize
        call    lb_puts
        call    lb_name
        mov     edx,OFFSET msg_qfrom
        call    lb_puts
        mov     eax,[sz_cur]
        call    lb_mb
        mov     edx,OFFSET msg_qto
        call    lb_puts
        mov     eax,[sz_new]
        call    lb_mb
        mov     al,'?'
        call    lb_putc
        mov     edx,OFFSET msg_tsize
        mov     bl,A_DLG
        xor     ecx,ecx
        call    tu_ask
        jc      tsz_out
        mov     eax,[sz_start]
        mov     [rs_start],eax
        call    sz_args
        mov     eax,OFFSET fd_change
        call    tu_write
        jmp     short tsz_out
tsz_refused:
        call    tu_refusal
        jmp     tsz_ask
tsz_bad:
        call    tu_badmb
        jmp     tsz_ask
tsz_same:
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_xsize
        call    lb_puts
        mov     edx,OFFSET msg_tsize
        mov     bl,A_DLG
        call    tu_box
        jmp     tsz_ask
tsz_notfat:
        mov     dword ptr [pl_status],RL_NOTFAT
tsz_cant:
        call    tu_refusal
tsz_out:
        mov     dword ptr [pv_mode],PV_NONE
        popad
        ret

sz_args:
        mov     dword ptr [cap_len],0
        mov     dword ptr [pl_status],-1
        call    tui_setdp
        mov     eax,[sz_new]
        call    tui_setnum
        mov     dword ptr [fd_nargs],2
        mov     byte ptr [fd_cmd],FDC_SIZE
        ret

sz_build:
        call    dg_reset
        mov     dword ptr [dg_title],OFFSET msg_tsize
        call    ln_new
        call    lb_whole
        call    ln_new
        mov     edx,OFFSET msg_lnow
        call    lb_puts
        mov     eax,[sz_cur]
        call    lb_mb
        cmp     dword ptr [sz_min],0
        je      szb_max
        call    ln_new
        mov     edx,OFFSET msg_lsmall
        call    lb_puts
        mov     eax,[sz_min]
        call    lb_mb
szb_max:
        cmp     dword ptr [sz_max],0
        je      szb_field
        call    ln_new
        mov     edx,OFFSET msg_llarge
        call    lb_puts
        mov     eax,[sz_max]
        call    lb_mb
szb_field:
        mov     dword ptr [dg_flabel],OFFSET msg_fsize
        mov     dword ptr [dg_pvfn],OFFSET sz_preview
        jmp     tu_okcancel

sz_preview:
        pushad
        mov     dword ptr [pv_mode],PV_NONE
        mov     esi,OFFSET dg_fbuf
        call    atomb
        jc      szp_out
        or      eax,eax
        jz      szp_out
        call    sz_snap
        mov     [pv_len],eax
        mov     ecx,[sz_start]
        mov     [pv_start],ecx
        mov     dword ptr [pv_mode],PV_REPLACE
        mov     byte ptr [pv_attr],A_PVOK
        cmp     eax,[sz_min]
        jb      szp_no
        cmp     dword ptr [sz_max],0
        je      szp_out
        cmp     eax,[sz_max]
        jbe     szp_out
szp_no:
        mov     byte ptr [pv_attr],A_PVNO
szp_out:
        popad
        ret

;---------------------------------------------------------------------
; sz_snap - EAX = a length asked for -> EAX = the length it will be:
;           ending on the cylinder boundary nearest to where it was
;           asked to end, but not past the largest it can be nor short
;           of the smallest.  A geometry with no cylinder, or a size so
;           small that rounding leaves nothing, is taken as typed.
;---------------------------------------------------------------------
sz_snap:
        push    ebx
        push    ecx
        push    edx
        mov     [szs_req],eax
        mov     ebx,[sz_start]
        call    fd_spcyl                ; ECX = a cylinder
        or      ecx,ecx
        jz      szs_raw
        add     eax,ebx
        jc      szs_raw
        call    cyl_near                ; the end, on a boundary
        cmp     dword ptr [sz_max],0
        je      szs_lower
        mov     edx,ebx
        add     edx,[sz_max]            ; the furthest it may end
        cmp     eax,edx
        jbe     szs_lower
        mov     eax,edx
        call    fd_cyldn
szs_lower:
        mov     edx,ebx
        add     edx,[sz_min]            ; the nearest it may end
        cmp     eax,edx
        jae     szs_check
        mov     eax,edx
        call    fd_cylup
szs_check:
        cmp     eax,ebx
        jbe     szs_raw
        sub     eax,ebx
        jmp     short szs_out
szs_raw:
        mov     eax,[szs_req]
szs_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; C - create, in the free space that is selected.  The engine is told
;     which run of free space by a sector in the middle of it (gp_want),
;     and puts the start where it always does - on a cylinder, or a
;     track into the disk, or a track behind a new logical drive's
;     record.
;=====================================================================
tu_create:
        pushad
        call    tui_selrow
        mov     eax,[esi+TR_KIND]
        cmp     eax,TK_FREE
        je      tcr_free
        cmp     eax,TK_LFREE
        je      tcr_lfree
        mov     dword ptr [tu_stat],OFFSET msg_tpickfree
        jmp     tcr_out
tcr_free:
        mov     eax,[tu_slot]           ; the first empty slot, 1 to 4
        or      eax,eax
        jz      tcr_noslot
        jmp     short tcr_num
tcr_lfree:
        mov     eax,[tu_lnum]
        or      eax,eax
        jz      tcr_nolink
tcr_num:
        mov     [cr_num],eax
        mov     eax,[esi+TR_KIND]
        mov     [cg_kind],eax
        mov     eax,[esi+TR_START]
        mov     [cg_start],eax
        add     eax,[esi+TR_SECS]
        mov     [cg_end],eax
        mov     byte ptr [dg_cval],0
        call    cr_avail                ; EAX = what there is room for
        mov     edi,OFFSET dg_fbuf
        call    mb_text
tcr_ask:
        call    cr_build
        call    dg_run
        or      eax,eax
        jnz     tcr_out
        mov     esi,OFFSET dg_fbuf
        call    atomb
        jc      tcr_bad
        or      eax,eax
        jz      tcr_bad
        mov     [cr_req],eax
        call    cr_args
        mov     eax,OFFSET fd_create
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     tcr_refused
        ; the picture: what the engine worked out
        mov     eax,[cr_start]
        mov     ecx,[cr_len]
        cmp     dword ptr [cg_kind],TK_LFREE
        jne     tcr_span
        mov     eax,[lg_start]
        mov     ecx,[lg_len]
tcr_span:
        mov     [pv_start],eax
        mov     [pv_len],ecx
        mov     dword ptr [pv_mode],PV_ADD
        mov     byte ptr [pv_attr],A_PVOK
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_qcreate
        call    lb_puts
        mov     eax,[pv_len]
        call    lb_mb
        mov     edx,OFFSET msg_qcrpart
        cmp     byte ptr [dg_cval],0
        je      tcr_notext
        mov     edx,OFFSET msg_qcrext
tcr_notext:
        cmp     dword ptr [cg_kind],TK_LFREE
        jne     tcr_what
        mov     edx,OFFSET msg_qcrlog
tcr_what:
        call    lb_puts
        mov     eax,[pv_start]
        call    lb_mb
        mov     al,'?'
        call    lb_putc
        cmp     byte ptr [dg_cval],0
        jne     tcr_question
        call    ln_new
        mov     edx,OFFSET msg_qcreate2
        call    lb_puts
tcr_question:
        mov     edx,OFFSET msg_tcreate
        mov     bl,A_DLG
        xor     ecx,ecx
        call    tu_ask
        jc      tcr_out
        mov     eax,[pv_start]
        mov     [rs_start],eax
        call    cr_args
        mov     eax,OFFSET fd_create
        call    tu_write
        jmp     short tcr_out
tcr_refused:
        call    tu_captail
        jmp     tcr_ask
tcr_bad:
        call    tu_badmb
        jmp     tcr_ask
tcr_noslot:
        mov     edx,OFFSET msg_xnoslot
        jmp     short tcr_say
tcr_nolink:
        mov     edx,OFFSET msg_xnolink
tcr_say:
        push    edx
        call    dg_reset
        call    ln_new
        pop     edx
        call    lb_puts
        mov     edx,OFFSET msg_tcreate
        mov     bl,A_DLGERR
        call    tu_box
tcr_out:
        mov     dword ptr [gp_want],0
        mov     dword ptr [pv_mode],PV_NONE
        popad
        ret

;--- cr_avail - EAX = how much of the selected run a new partition can
;--- have once its start is aligned as the engine aligns it; [ca_start]
;--- = that start.
cr_avail:
        push    ecx
        push    edx
        call    fd_spcyl                ; ECX = a cylinder
        movzx   edx,word ptr [dsk_rec+DSK_SPT]
        or      edx,edx
        jnz     cra_spt
        inc     edx
cra_spt:
        mov     eax,[cg_start]
        cmp     dword ptr [cg_kind],TK_LFREE
        je      cra_log
        cmp     eax,edx                 ; the first track's end stays put
        je      cra_have
        call    fd_cylup
        jmp     short cra_have
cra_log:
        cmp     eax,[tu_cbase]          ; so does the container's start
        je      cra_rec
        call    fd_cylup
cra_rec:
        add     eax,edx                 ; the volume a track behind its record
cra_have:
        mov     [ca_start],eax
        mov     edx,[cg_end]
        sub     edx,eax
        ja      cra_room
        xor     edx,edx
cra_room:
        mov     eax,edx
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; cr_fit - EAX = the size asked for -> EAX = the length the new
;          partition will have: ending on the cylinder boundary nearest
;          to where it was asked to end, as a resize does.  CF set when
;          that is all of the free space - and asking for up to a
;          megabyte more than there is counts as all of it, because the
;          box offers the space to a tenth of a megabyte and rounds.
;          CF clear and EAX = 0 when it asks for more than that.
;---------------------------------------------------------------------
cr_fit:
        push    ebx
        push    ecx
        push    edx
        mov     ebx,eax                 ; EBX = asked for
        call    cr_avail                ; EAX = the room, ca_start = the start
        mov     edx,eax
        add     edx,[ca_start]          ; EDX = the end of the room
        mov     ecx,eax
        add     ecx,2048
        cmp     ebx,ecx
        ja      crf_toobig
        push    ebx
        mov     eax,edx
        call    fd_spcyl                ; ECX = a cylinder
        call    fd_cyldn
        mov     edx,eax                 ; EDX = where "all of it" ends
        pop     eax
        add     eax,[ca_start]          ; where it was asked to end
        call    cyl_near
        cmp     eax,edx
        jae     crf_all
        sub     eax,[ca_start]
        ja      crf_part
        mov     eax,[ca_start]          ; rounded to nothing: one cylinder
        inc     eax
        call    fd_cylup
        cmp     eax,edx
        jae     crf_all
        sub     eax,[ca_start]
crf_part:
        clc
        jmp     short crf_out
crf_all:
        mov     eax,edx
        sub     eax,[ca_start]
        stc
        jmp     short crf_out
crf_toobig:
        xor     eax,eax
        clc
crf_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- cr_args - fd_args for the create in hand ------------------------
;--- All of the free space is no size at all, which the engine reads as
;--- "all of it"; too much is passed on as typed, for the engine to say
;--- so.
cr_args:
        pushad
        mov     dword ptr [cap_len],0
        mov     edi,OFFSET fd_args
        mov     eax,[tui_disk]
        call    tui_putdec
        mov     al,':'
        stosb
        mov     eax,[cr_num]
        call    tui_putdec0
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_CREATE
        mov     eax,[cg_start]
        mov     ecx,[cg_end]
        sub     ecx,eax
        shr     ecx,1
        add     eax,ecx
        mov     [gp_want],eax           ; this run of free space, not the
                                        ; biggest one
        mov     eax,[cr_req]
        call    cr_fit
        mov     ebx,eax
        jc      cra_size                ; all of it: no size
        mov     dword ptr [fd_nargs],2
        or      ebx,ebx
        jnz     cra_size
        mov     ebx,[cr_req]            ; more than there is
cra_size:
        cmp     byte ptr [dg_cval],0
        je      cra_sized
        mov     dword ptr [fd_nargs],3  ; an extended one needs its type, and
cra_sized:                              ; a type needs a size in front of it
        cmp     dword ptr [fd_nargs],1
        je      cra_out
        mov     eax,ebx
        mov     edi,OFFSET fd_args + FD_ARGLEN
        call    tui_putdec0
        cmp     dword ptr [fd_nargs],3
        jne     cra_out
        ; 05h, or 0Fh when it ends where only LBA reaches
        mov     edi,OFFSET fd_args + FD_ARGLEN * 2
        mov     eax,[ca_start]
        add     eax,ebx
        mov     byte ptr [edi],'0'
        mov     byte ptr [edi+1],'5'
        cmp     eax,dword ptr [dsk_rec+DSK_CHS]
        jbe     cra_type
        mov     byte ptr [edi+1],'F'
cra_type:
        mov     byte ptr [edi+2],0
cra_out:
        popad
        ret

cr_build:
        call    dg_reset
        mov     dword ptr [dg_title],OFFSET msg_tcreate
        call    ln_new
        mov     edx,OFFSET msg_lfree
        call    lb_puts
        mov     eax,[cg_end]
        sub     eax,[cg_start]
        call    lb_mb
        call    ln_new
        mov     edx,OFFSET msg_lfrom
        call    lb_puts
        mov     eax,[cg_start]
        call    lb_mbn
        mov     edx,OFFSET msg_lroomto
        call    lb_puts
        mov     eax,[cg_end]
        call    lb_mb
        cmp     dword ptr [cg_kind],TK_LFREE
        jne     crb_field
        call    ln_new
        mov     edx,OFFSET msg_linext
        call    lb_puts
crb_field:
        mov     dword ptr [dg_flabel],OFFSET msg_fcrsize
        mov     dword ptr [dg_pvfn],OFFSET cr_preview
        ; an extended partition can be asked for where there is none yet
        cmp     dword ptr [cg_kind],TK_FREE
        jne     tu_okcancel
        cmp     byte ptr [tu_hascont],0
        jne     tu_okcancel
        mov     dword ptr [dg_clabel],OFFSET msg_cext
        jmp     tu_okcancel

;--- cr_preview - the new partition as the engine will lay it out ----
cr_preview:
        pushad
        mov     dword ptr [pv_mode],PV_NONE
        mov     esi,OFFSET dg_fbuf
        call    atomb
        jc      crp_out
        or      eax,eax
        jz      crp_out
        mov     byte ptr [pv_attr],A_PVOK
        call    cr_fit
        or      eax,eax
        jnz     crp_len
        mov     byte ptr [pv_attr],A_PVNO   ; more than there is: all of it,
        call    cr_avail                    ; in red
        mov     edx,eax
        add     eax,[ca_start]
        call    fd_spcyl
        call    fd_cyldn
        sub     eax,[ca_start]
        jbe     crp_out
crp_len:
        mov     [pv_len],eax
        mov     eax,[ca_start]
        mov     [pv_start],eax
        mov     dword ptr [pv_mode],PV_ADD
crp_out:
        popad
        ret

;=====================================================================
; D or Del - delete the selected partition, after a red question whose
;            Enter is Cancel.
;=====================================================================
tu_delete:
        pushad
        call    tui_selrow
        mov     eax,[esi+TR_KIND]
        cmp     eax,TK_PRIMARY
        je      tdl_ok
        cmp     eax,TK_LOGICAL
        je      tdl_ok
        cmp     eax,TK_CONTAINER
        je      tdl_ok
        mov     dword ptr [tu_stat],OFFSET msg_tnothing
        jmp     tdl_out
tdl_ok:
        call    dl_args
        mov     eax,OFFSET fd_delete
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     tdl_refused
        mov     dword ptr [pv_mode],PV_OMIT
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_qdelete
        call    lb_puts
        call    lb_name
        mov     al,'?'
        call    lb_putc
        call    ln_new
        call    lb_what
        call    ln_new
        mov     edx,OFFSET msg_qdelete2
        call    tui_selrow
        cmp     dword ptr [esi+TR_KIND],TK_CONTAINER
        jne     tdl_lost
        mov     edx,OFFSET msg_qdelext
tdl_lost:
        call    lb_puts
        mov     edx,OFFSET msg_tdelete
        mov     bl,A_DLGERR
        mov     ecx,1                   ; Enter is No
        call    tu_ask
        jc      tdl_out
        call    tui_selrow
        mov     eax,[esi+TR_START]
        mov     [rs_start],eax
        call    dl_args
        mov     eax,OFFSET fd_delete
        call    tu_write
        jmp     short tdl_out
tdl_refused:
        call    tu_captail
tdl_out:
        mov     dword ptr [pv_mode],PV_NONE
        popad
        ret

dl_args:
        mov     dword ptr [cap_len],0
        call    tui_setdp
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_DELETE
        ret

;=====================================================================
; A - make the selected primary the one the machine starts from.
;=====================================================================
tu_active:
        pushad
        call    tui_selrow
        cmp     dword ptr [esi+TR_KIND],TK_PRIMARY
        je      tac_prim
        mov     dword ptr [tu_stat],OFFSET msg_tnotprim
        jmp     tac_out
tac_prim:
        cmp     dword ptr [esi+TR_ACT],0
        je      tac_ok
        mov     dword ptr [tu_stat],OFFSET msg_talready
        jmp     tac_out
tac_ok:
        call    ac_args
        mov     eax,OFFSET fd_active
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     tac_refused
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_qactive
        call    lb_puts
        call    lb_name
        mov     edx,OFFSET msg_qactive2
        call    lb_puts
        call    ln_new
        mov     edx,OFFSET msg_qactive3
        call    lb_puts
        mov     edx,OFFSET msg_tactive
        mov     bl,A_DLG
        xor     ecx,ecx
        call    tu_ask
        jc      tac_out
        call    tui_selrow
        mov     eax,[esi+TR_START]
        mov     [rs_start],eax
        call    ac_args
        mov     eax,OFFSET fd_active
        call    tu_write
        jmp     short tac_out
tac_refused:
        call    tu_captail
tac_out:
        popad
        ret

ac_args:
        mov     dword ptr [cap_len],0
        call    tui_setdp
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_ACTIVE
        ret

;=====================================================================
; B (Disk > Write boot code) - this disk's master boot record gets
;     FDISK's loader again; the table in it stays as it is.
;=====================================================================
tu_boot:
        pushad
        cmp     byte ptr [tui_dstate],0
        je      tbo_ok
        mov     dword ptr [tu_stat],OFFSET msg_tnoboot
        jmp     tbo_out
tbo_ok:
        call    bo_args
        mov     eax,OFFSET fd_mbr
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     tbo_refused
        ; the verb read the record into mbr_buf: is it ours already?
        mov     esi,OFFSET mbr_code
        mov     edi,[mbr_buf]
        mov     ecx,MBRW_LEN
        cld
        repe    cmpsb
        jne     tbo_ask
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_xbootsame
        call    lb_puts
        mov     edx,OFFSET msg_tboot
        mov     bl,A_DLG
        call    tu_box
        jmp     short tbo_out
tbo_ask:
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_qboot
        call    lb_puts
        mov     eax,[tui_disk]
        call    lb_num
        mov     al,'?'
        call    lb_putc
        call    ln_new
        mov     edx,OFFSET msg_qboot2
        call    lb_puts
        call    ln_new
        mov     edx,OFFSET msg_qboot3
        call    lb_puts
        mov     edx,OFFSET msg_tboot
        mov     bl,A_DLG
        mov     ecx,1
        call    tu_ask
        jc      tbo_out
        call    bo_args
        mov     eax,OFFSET fd_mbr
        call    tu_write
        jmp     short tbo_out
tbo_refused:
        call    tu_captail
tbo_out:
        popad
        ret

bo_args:
        mov     dword ptr [cap_len],0
        mov     eax,[tui_disk]
        mov     edi,OFFSET fd_args
        call    tui_putdec0
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_MBR
        ret

;=====================================================================
; Enter, P, or a double click: what /P says about a partition, in the
; viewer.  On free space it is a create.
;=====================================================================
tu_details:
        call    tui_selrow
        mov     eax,[esi+TR_KIND]
        cmp     eax,TK_FREE
        je      tu_create
        cmp     eax,TK_LFREE
        je      tu_create
        cmp     eax,TK_CONTAINER
        je      tde_cont
        cmp     eax,TK_PRIMARY
        je      tde_vol
        cmp     eax,TK_LOGICAL
        je      tde_vol
        ret
tde_cont:
        mov     dword ptr [tu_stat],OFFSET msg_tpickvol
        ret
tde_vol:
        pushad
        mov     dword ptr [cap_len],0
        call    tui_setdp
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_PLAN
        mov     eax,OFFSET fd_plan
        call    tui_dryrun
        mov     edi,OFFSET vt_buf
        mov     ecx,40
        call    lb_into
        mov     edx,OFFSET msg_vpart
        call    lb_puts
        call    tui_selrow
        mov     eax,[esi+TR_NUM]
        call    lb_num
        mov     edx,OFFSET vt_buf
        call    vw_show
        popad
        ret

;--- V (File > Output log): what the last verb said ------------------
tu_log:
        cmp     dword ptr [cap_len],0
        jne     tlg_show
        mov     dword ptr [tu_stat],OFFSET msg_tnolog
        ret
tlg_show:
        mov     edx,OFFSET msg_vlog
        jmp     vw_show

;--- F1: the keys, and the command line ------------------------------
tu_help:
        mov     dword ptr [cap_len],0
        mov     edx,OFFSET msg_tkeys
        call    puts
        mov     edx,OFFSET msg_usage
        call    puts
        mov     edx,OFFSET msg_vhelp
        jmp     vw_show

tu_about:
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_about1
        call    lb_puts
        call    ln_new
        call    ln_new
        mov     edx,OFFSET msg_about2
        call    lb_puts
        mov     edx,OFFSET msg_tabout
        mov     bl,A_DLG
        jmp     tu_box

;--- the disks -------------------------------------------------------
tu_nextdisk:
        mov     eax,[tui_disk]
        inc     eax
        cmp     eax,[tui_ndisks]
        jb      tu_seldisk
        xor     eax,eax
        jmp     short tu_seldisk
tu_prevdisk:
        mov     eax,[tui_disk]
        or      eax,eax
        jnz     tpd_back
        mov     eax,[tui_ndisks]
tpd_back:
        dec     eax
tu_seldisk:
        mov     [tui_disk],eax
        mov     dword ptr [tr_sel],0
        mov     dword ptr [ls_top],0
tu_reread:
        call    tui_load
        ret

tu_quit:
        call    ev_done
        mov     byte ptr [tui_on],0
        mov     byte ptr [fd_menu],0
        call    tui_showcur
        mov     eax,0F000h              ; clear the screen
        int     21h
        jmp     fd_done

tu_ignore:
        ret

;=====================================================================
; The boxes the commands share.
;=====================================================================

;--- tu_refusal - the engine said no to a move or a resize -----------
;--- One line saying why, from pl_status - or, when the verb stopped
;--- before it planned anything, what it printed.
tu_refusal:
        pushad
        mov     eax,[pl_status]
        cmp     eax,RL_FATTYPE
        ja      tu_captail_in           ; -1: it never got as far as a plan
        call    dg_reset
        call    ln_new
        mov     edx,[rf_tbl+eax*4]
        call    lb_puts
        mov     edx,OFFSET msg_trefused
        mov     bl,A_DLGERR
        call    tu_box
        popad
        ret

;--- tu_captail - the end of what the verb printed, in a red box -----
tu_captail:
        pushad
tu_captail_in:
        call    dg_reset
        mov     ecx,6
        call    ln_captail
        mov     edx,OFFSET msg_trefused
        mov     bl,A_DLGERR
        call    tu_box
        popad
        ret

tu_badmb:
        pushad
        call    dg_reset
        call    ln_new
        mov     edx,OFFSET msg_xbadmb
        call    lb_puts
        mov     edx,OFFSET msg_tbadmb
        mov     bl,A_DLGERR
        call    tu_box
        popad
        ret

;--- tu_okcancel - the two buttons every box with a field has --------
tu_okcancel:
        mov     edx,OFFSET msg_bok
        call    dg_btn
        mov     edx,OFFSET msg_bcancel
        call    dg_btn
        mov     dword ptr [dg_focus],0
        mov     dword ptr [dg_esc],1
        ret

;--- lb_name - "partition 5 (D:)" for the selected row; lb_namec with
;--- a capital, and lb_whole "Partition 5 (D:), FAT16, 40.8 MB"
lb_whole:
        call    lb_namec
        push    edx
        mov     edx,OFFSET msg_comma3
        call    lb_puts
        pop     edx
        jmp     lb_what
lb_namec:
        push    eax
        push    edx
        push    esi
        mov     edx,OFFSET msg_npartc
        jmp     short lbn_word
lb_name:
        push    eax
        push    edx
        push    esi
        mov     edx,OFFSET msg_npart
lbn_word:
        call    tui_selrow
        call    lb_puts
        mov     eax,[esi+TR_NUM]
        call    lb_num
        mov     eax,[esi+TR_LETTER]
        or      eax,eax
        jz      lbn_out
        mov     edx,OFFSET msg_nopen
        call    lb_puts
        call    lb_putc
        mov     edx,OFFSET msg_nclose
        call    lb_puts
lbn_out:
        pop     esi
        pop     edx
        pop     eax
        ret

;--- lb_what - "FAT16, 40.8 MB" for the selected row -----------------
lb_what:
        push    eax
        push    edx
        push    esi
        call    tui_selrow
        mov     eax,[esi+TR_TYPE]
        call    tn_find
        call    lb_puts
        call    lb_trim
        mov     edx,OFFSET msg_comma3
        call    lb_puts
        mov     eax,[esi+TR_SECS]
        call    lb_mb
        pop     esi
        pop     edx
        pop     eax
        ret

;=====================================================================
; Running a verb from here.
;
; tui_run    - EAX = the verb.  fd_args and fd_cmd are set; the verb
;              runs, prints into cap_buf, and comes back here through
;              fd_done or fd_fail with fd_rc saying which.  The stack
;              is put back where it was, not unwound - see fd_back.
; tui_dryrun - the same with the writes held off, which is what makes a
;              verb safe to run to find out what it would do
; tu_write   - the same with the writes on, then the table read again
;              and what the verb said in a box
;=====================================================================
tui_run:
        mov     [fd_mstack],esp
        jmp     eax
tui_after:                              ; fd_back lands here: ESP is as it
        ret                             ; was at tui_run, so this returns
                                        ; to tui_run's caller

tui_dryrun:
        push    ebx
        movzx   ebx,byte ptr [dk_armed]
        push    ebx
        mov     byte ptr [dk_armed],0
        call    tui_run
        pop     ebx
        mov     [dk_armed],bl
        pop     ebx
        ret

tu_write:
        pushad
        mov     [wr_verb],eax
        mov     dword ptr [tu_stat],OFFSET msg_tworking
        call    tui_draw                ; the change still drawn on the bar
        movzx   ebx,byte ptr [dk_armed]
        push    ebx
        mov     byte ptr [dk_armed],1
        mov     eax,[wr_verb]
        call    tui_run
        pop     ebx
        mov     [dk_armed],bl
        mov     dword ptr [tu_stat],0
        mov     dword ptr [pv_mode],PV_NONE
        call    tui_load
        call    tu_reselect
        call    dg_reset
        mov     ecx,6
        call    ln_captail
        mov     edx,OFFSET msg_tdone
        mov     bl,A_DLG
        cmp     byte ptr [fd_rc],0
        je      twr_box
        mov     edx,OFFSET msg_tfailed
        mov     bl,A_DLGERR
twr_box:
        call    tu_box
        popad
        ret

;--- tu_reselect - after a change, the selection goes to what holds
;--- rs_start: a partition if one does, else the free space that does.
tu_reselect:
        pushad
        cmp     dword ptr [rs_start],-1
        je      trs_out
        mov     edi,1                   ; first pass: partitions only
trs_pass:
        xor     ebp,ebp
trs_one:
        cmp     ebp,[tr_n]
        jae     trs_passdone
        imul    esi,ebp,TR_LEN
        add     esi,OFFSET tr_tbl
        or      edi,edi
        jz      trs_test
        cmp     dword ptr [esi+TR_KIND],TK_PRIMARY
        je      trs_test
        cmp     dword ptr [esi+TR_KIND],TK_LOGICAL
        jne     trs_next
trs_test:
        mov     eax,[rs_start]
        cmp     eax,[esi+TR_START]
        jb      trs_next
        mov     edx,[esi+TR_START]
        add     edx,[esi+TR_SECS]
        cmp     eax,edx
        jae     trs_next
        mov     [tr_sel],ebp
        jmp     short trs_out
trs_next:
        inc     ebp
        jmp     short trs_one
trs_passdone:
        or      edi,edi
        jz      trs_out
        xor     edi,edi
        jmp     short trs_pass
trs_out:
        mov     dword ptr [rs_start],-1
        popad
        ret

;--- the selected row ------------------------------------------------
tui_selrow:                             ; -> ESI
        mov     esi,[tr_sel]
        imul    esi,TR_LEN
        add     esi,OFFSET tr_tbl
        ret

tui_selvol:                             ; CF clear if it holds a volume
        push    esi
        call    tui_selrow
        cmp     dword ptr [esi+TR_KIND],TK_PRIMARY
        je      tsv_yes
        cmp     dword ptr [esi+TR_KIND],TK_LOGICAL
        je      tsv_yes
        mov     dword ptr [tu_stat],OFFSET msg_tnotvol
        pop     esi
        stc
        ret
tsv_yes:
        pop     esi
        clc
        ret

;--- tui_setdp - "d:p" for the selected row into fd_args[0] ----------
tui_setdp:
        pushad
        mov     edi,OFFSET fd_args
        mov     eax,[tui_disk]
        call    tui_putdec
        mov     al,':'
        stosb
        call    tui_selrow
        mov     eax,[esi+TR_NUM]
        call    tui_putdec0
        popad
        ret

;--- tui_setnum - EAX in decimal into fd_args[1] ---------------------
tui_setnum:
        pushad
        mov     edi,OFFSET fd_args + FD_ARGLEN
        call    tui_putdec0
        popad
        ret

;--- tui_putdec - EAX as digits at EDI, EDI past them; ...0 ends it --
tui_putdec0:
        call    tui_putdec
        mov     byte ptr [edi],0
        ret
tui_putdec:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ecx,10
        xor     ebx,ebx
tpd_div:
        xor     edx,edx
        div     ecx
        push    edx
        inc     ebx
        or      eax,eax
        jnz     tpd_div
tpd_pop:
        pop     eax
        add     al,'0'
        stosb
        dec     ebx
        jnz     tpd_pop
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; tui_load - the table for the disk in tui_disk, in DISK ORDER: the
;            primaries and the container, the logical drives in it,
;            and every run of free space a cylinder or more long, at
;            the level it belongs to.
;=====================================================================
tui_load:
        pushad
        xor     eax,eax
        mov     [tr_n],eax
        mov     [tu_lnum],eax
        mov     [tu_slot],eax
        mov     [tu_cbase],eax
        mov     [tu_cend],eax
        mov     byte ptr [tui_dstate],0
        mov     byte ptr [tu_hascont],0
        mov     byte ptr [tu_chok],0
        mov     edi,OFFSET tr_tbl       ; an empty table still has a row 0
        mov     ecx,TR_LEN / 4          ; to point at, and it is nothing
        rep     stosd
        mov     edx,[tui_disk]
        add     edx,80h
        mov     [pl_drive],dl
        mov     edi,OFFSET dsk_rec
        mov     eax,0F10Ch
        int     21h
        jc      tl_nodisk
        test    byte ptr [dsk_rec+DSK_FLAGS],DSKF_PRESENT
        jz      tl_nodisk
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        mov     [phy_total],eax
        mov     eax,dword ptr [dsk_rec+DSK_CHS]
        mov     [phy_chs],eax

        mov     al,[pl_drive]
        xor     ebx,ebx
        mov     ecx,1
        mov     edi,[mbr_buf]
        call    dk_read
        jc      tl_unread
        mov     edi,[mbr_buf]
        cmp     word ptr [edi+1FEh],MBR_SIG
        jne     tl_notable

        ;--- the four slots
        xor     ebp,ebp
tl_slot:
        imul    esi,ebp,PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        movzx   eax,byte ptr [esi+PE_TYPE]
        or      eax,eax
        jnz     tl_used
        cmp     dword ptr [tu_slot],0   ; the first empty one is where a
        jne     tl_slotnext             ; new primary goes
        lea     eax,[ebp+1]
        mov     [tu_slot],eax
        jmp     short tl_slotnext
tl_used:
        call    tui_newrow              ; EDI -> a zeroed row
        mov     [edi+TR_TYPE],eax
        lea     eax,[ebp+1]
        mov     [edi+TR_NUM],eax
        mov     dword ptr [edi+TR_KIND],TK_PRIMARY
        mov     al,[esi+PE_TYPE]
        call    fd_isext                ; CF clear when it IS extended
        jc      tl_notcont
        mov     dword ptr [edi+TR_KIND],TK_CONTAINER
        mov     byte ptr [tu_hascont],1
tl_notcont:
        cmp     byte ptr [esi+PE_BOOT],80h
        jne     tl_notact
        mov     dword ptr [edi+TR_ACT],1
tl_notact:
        mov     eax,[esi+PE_START]
        mov     [edi+TR_START],eax
        mov     ecx,[esi+PE_SECS]
        mov     [edi+TR_SECS],ecx
        push    ebx
        mov     ebx,[tui_disk]
        call    vol_letter
        pop     ebx
        jc      tl_slotnext
        movzx   eax,al
        mov     [edi+TR_LETTER],eax
tl_slotnext:
        inc     ebp
        cmp     ebp,PART_MAX
        jb      tl_slot

        ;--- the chain
        cmp     byte ptr [tu_hascont],0
        je      tl_gaps
        call    fd_chain
        jc      tl_gaps
        mov     eax,[chn_base]
        mov     [tu_cbase],eax
        add     eax,[chn_ext]
        mov     [tu_cend],eax
        xor     ebp,ebp
tl_link:
        cmp     ebp,[chn_n]
        jae     tl_linksdone
        imul    esi,ebp,CH_LEN
        add     esi,[chn_tbl]
        cmp     dword ptr [esi+CH_VSTART],0
        jne     tl_live
        cmp     dword ptr [tu_lnum],0   ; a link with no drive in it is the
        jne     tl_linknext             ; number a new one takes
        lea     eax,[ebp+PART_MAX+1]
        mov     [tu_lnum],eax
        jmp     short tl_linknext
tl_live:
        call    tui_newrow
        lea     eax,[ebp+PART_MAX+1]
        mov     [edi+TR_NUM],eax
        mov     dword ptr [edi+TR_KIND],TK_LOGICAL
        movzx   eax,byte ptr [esi+CH_TYPE]
        mov     [edi+TR_TYPE],eax
        mov     eax,[esi+CH_EBR]
        mov     [edi+TR_EBR],eax
        mov     eax,[esi+CH_VSTART]
        mov     [edi+TR_START],eax
        mov     ecx,[esi+CH_VSECS]
        mov     [edi+TR_SECS],ecx
        push    ebx
        mov     ebx,[tui_disk]
        call    vol_letter
        pop     ebx
        jc      tl_linknext
        movzx   eax,al
        mov     [edi+TR_LETTER],eax
tl_linknext:
        inc     ebp
        jmp     short tl_link
tl_linksdone:
        ; A CHAIN WHOSE SHAPE COULD NOT BE ESTABLISHED is shown as far
        ; as it was read and nothing is created in it: no free space is
        ; offered inside the container, and no number for a new drive.
        cmp     dword ptr [chn_st],CHN_OK
        je      tl_chainok
        mov     dword ptr [tu_lnum],0
        jmp     short tl_gaps
tl_chainok:
        mov     byte ptr [tu_chok],1
        cmp     dword ptr [tu_lnum],0
        jne     tl_gaps
        mov     eax,[chn_n]
        cmp     eax,EBR_MAX
        jae     tl_gaps
        add     eax,PART_MAX+1          ; on the end of the chain
        mov     [tu_lnum],eax

tl_gaps:
        call    tl_topgaps
        cmp     byte ptr [tu_chok],0
        je      tl_sorted
        call    tl_contgaps
tl_sorted:
        call    tl_sort
        call    tl_colours
tl_done:
        ; the selection has to name a row that exists
        mov     eax,[tr_sel]
        cmp     eax,[tr_n]
        jb      tl_selok
        mov     eax,[tr_n]
        or      eax,eax
        jz      tl_selzero
        dec     eax
tl_selzero:
        mov     [tr_sel],eax
tl_selok:
        popad
        ret
tl_nodisk:
        mov     byte ptr [tui_dstate],1
        jmp     short tl_done
tl_unread:
        mov     byte ptr [tui_dstate],2
        jmp     short tl_done
tl_notable:
        mov     byte ptr [tui_dstate],3
        jmp     short tl_done

;--- tui_newrow - EDI -> a fresh, zeroed row; tr_n counted up --------
tui_newrow:
        push    eax
        push    ecx
        mov     edi,[tr_n]
        cmp     edi,TR_MAX
        jb      tnr_room
        dec     edi                     ; never past the table: the last row
        dec     dword ptr [tr_n]        ; is used again
tnr_room:
        imul    edi,TR_LEN
        add     edi,OFFSET tr_tbl
        push    edi
        mov     ecx,TR_LEN / 4
        xor     eax,eax
        rep     stosd
        pop     edi
        inc     dword ptr [tr_n]
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; THE FREE SPACE.  The spans that are taken at one level are put in
; order, and whatever lies between them - and before the first and
; after the last - is free.  At the top that is the primaries and the
; container, from the end of the first track to the end of the disk;
; inside the container it is the links, each its record AND its volume,
; and a link carrying nothing and leading nowhere takes no room at all
; (fd_lgap's rule, so the screen offers the space the verb will use).
;---------------------------------------------------------------------
tl_topgaps:
        pushad
        mov     dword ptr [iv_n],0
        xor     ebp,ebp
ttg_one:
        cmp     ebp,[tr_n]
        jae     ttg_sweep
        imul    esi,ebp,TR_LEN
        add     esi,OFFSET tr_tbl
        mov     eax,[esi+TR_KIND]
        cmp     eax,TK_PRIMARY
        je      ttg_take
        cmp     eax,TK_CONTAINER
        jne     ttg_next
ttg_take:
        mov     eax,[esi+TR_START]
        mov     ecx,eax
        add     ecx,[esi+TR_SECS]
        call    iv_add
ttg_next:
        inc     ebp
        jmp     short ttg_one
ttg_sweep:
        movzx   eax,word ptr [dsk_rec+DSK_SPT]
        or      eax,eax
        jnz     ttg_lo
        inc     eax
ttg_lo:
        mov     ecx,dword ptr [dsk_rec+DSK_TOTAL]
        mov     edx,TK_FREE
        call    iv_sweep
        popad
        ret

tl_contgaps:
        pushad
        mov     dword ptr [iv_n],0
        xor     ebp,ebp
tcg_one:
        cmp     ebp,[chn_n]
        jae     tcg_sweep
        imul    esi,ebp,CH_LEN
        add     esi,[chn_tbl]
        cmp     dword ptr [esi+CH_VSTART],0
        jne     tcg_take
        cmp     dword ptr [esi+CH_NEXT],0
        je      tcg_next
tcg_take:
        mov     eax,[esi+CH_EBR]
        mov     ecx,eax
        add     ecx,[esi+CH_EXTENT]
        call    iv_add
tcg_next:
        inc     ebp
        jmp     short tcg_one
tcg_sweep:
        mov     eax,[tu_cbase]
        mov     ecx,[tu_cend]
        mov     edx,TK_LFREE
        call    iv_sweep
        popad
        ret

;--- iv_add - the span EAX..ECX is taken -----------------------------
iv_add:
        push    edi
        mov     edi,[iv_n]
        cmp     edi,IV_MAX
        jae     iva_full
        mov     [iv_tbl+edi*8],eax
        mov     [iv_tbl+edi*8+4],ecx
        inc     dword ptr [iv_n]
iva_full:
        pop     edi
        ret

;--- iv_sweep - the free runs of EAX..ECX that no span covers, as rows
;--- of kind EDX.  Anything shorter than a cylinder is not offered.
iv_sweep:
        pushad
        mov     [sw_lo],eax
        mov     [sw_hi],ecx
        mov     [sw_kind],edx
        call    fd_spcyl
        or      ecx,ecx
        jnz     ivs_min
        inc     ecx
ivs_min:
        mov     [sw_min],ecx
        ; in order of where they start: an insertion sort of a few spans
        mov     ebx,1
ivs_outer:
        cmp     ebx,[iv_n]
        jae     ivs_walk
        mov     eax,[iv_tbl+ebx*8]
        mov     edx,[iv_tbl+ebx*8+4]
        mov     ebp,ebx
ivs_inner:
        or      ebp,ebp
        jz      ivs_place
        cmp     [iv_tbl+ebp*8-8],eax
        jbe     ivs_place
        mov     ecx,[iv_tbl+ebp*8-8]
        mov     [iv_tbl+ebp*8],ecx
        mov     ecx,[iv_tbl+ebp*8-4]
        mov     [iv_tbl+ebp*8+4],ecx
        dec     ebp
        jmp     short ivs_inner
ivs_place:
        mov     [iv_tbl+ebp*8],eax
        mov     [iv_tbl+ebp*8+4],edx
        inc     ebx
        jmp     short ivs_outer
ivs_walk:
        mov     ebx,[sw_lo]             ; EBX = the cursor
        xor     ebp,ebp
ivs_span:
        cmp     ebp,[iv_n]
        jae     ivs_last
        mov     eax,[iv_tbl+ebp*8]
        cmp     eax,ebx
        jbe     ivs_past
        mov     ecx,eax                 ; free from the cursor to this one
        cmp     ecx,[sw_hi]
        jbe     ivs_gap
        mov     ecx,[sw_hi]
ivs_gap:
        call    ivs_free                ; EBX..ECX
ivs_past:
        mov     eax,[iv_tbl+ebp*8+4]
        cmp     eax,ebx
        jbe     ivs_nextspan
        mov     ebx,eax
ivs_nextspan:
        inc     ebp
        jmp     short ivs_span
ivs_last:
        mov     ecx,[sw_hi]
        call    ivs_free
        popad
        ret

ivs_free:                               ; EBX..ECX free, if it is enough
        push    eax
        push    edi
        cmp     ecx,ebx
        jbe     ivf_out
        mov     eax,ecx
        sub     eax,ebx
        cmp     eax,[sw_min]
        jb      ivf_out
        call    tui_newrow
        mov     [edi+TR_SECS],eax
        mov     [edi+TR_START],ebx
        mov     eax,[sw_kind]
        mov     [edi+TR_KIND],eax
ivf_out:
        pop     edi
        pop     eax
        ret

;--- tl_sort - the rows in disk order: by start, a container in front
;--- of whatever starts where it does
tl_sort:
        pushad
        mov     ebx,1
tso_outer:
        cmp     ebx,[tr_n]
        jae     tso_done
        imul    esi,ebx,TR_LEN
        add     esi,OFFSET tr_tbl
        mov     edi,OFFSET ts_tmp
        mov     ecx,TR_LEN / 4
        rep     movsd
        mov     ebp,ebx
tso_inner:
        or      ebp,ebp
        jz      tso_place
        imul    esi,ebp,TR_LEN
        add     esi,OFFSET tr_tbl - TR_LEN
        call    tso_after               ; is row ESI after the one held?
        jnc     tso_place
        lea     edi,[esi+TR_LEN]
        mov     ecx,TR_LEN / 4
        rep     movsd
        dec     ebp
        jmp     short tso_inner
tso_place:
        imul    edi,ebp,TR_LEN
        add     edi,OFFSET tr_tbl
        mov     esi,OFFSET ts_tmp
        mov     ecx,TR_LEN / 4
        rep     movsd
        inc     ebx
        jmp     short tso_outer
tso_done:
        popad
        ret

tso_after:                              ; CF set if ESI's row goes after
        push    eax
        mov     eax,[esi+TR_START]
        cmp     eax,dword ptr [ts_tmp+TR_START]
        ja      tsa_yes
        jb      tsa_no
        cmp     dword ptr [ts_tmp+TR_KIND],TK_CONTAINER
        je      tsa_yes                 ; the container goes in front
tsa_no:
        pop     eax
        clc
        ret
tsa_yes:
        pop     eax
        stc
        ret

;--- tl_colours - each partition a colour by its NUMBER, not by where it
;--- is, so that it keeps the colour it had when the partitions round it
;--- come and go
tl_colours:
        pushad
        xor     ebp,ebp
tco_one:
        cmp     ebp,[tr_n]
        jae     tco_done
        imul    esi,ebp,TR_LEN
        add     esi,OFFSET tr_tbl
        mov     eax,[esi+TR_KIND]
        mov     dl,A_FREE
        cmp     eax,TK_FREE
        je      tco_set
        mov     dl,A_CONT
        cmp     eax,TK_CONTAINER
        je      tco_set
        cmp     eax,TK_LFREE
        je      tco_set
        mov     eax,[esi+TR_NUM]
        dec     eax
        cmp     eax,PART_MAX
        jb      tco_pick
        dec     eax                     ; the logical drives one on, so the
tco_pick:                               ; first is not slot 1's colour
        and     eax,TUI_NCOLORS - 1
        mov     dl,[tui_colors+eax]
tco_set:
        movzx   edx,dl
        mov     [esi+TR_ATTR],edx
        inc     ebp
        jmp     short tco_one
tco_done:
        popad
        ret

        INCLUDE FDDRAW.INC
        INCLUDE FDDLG.INC
        INCLUDE FDEVENT.INC

;=====================================================================
; Data for the screen.
;=====================================================================
cmd_tbl     dd  tu_ignore               ; CMD_NONE
            dd  tu_quit
            dd  tu_log
            dd  tu_nextdisk
            dd  tu_prevdisk
            dd  tu_reread
            dd  tu_boot
            dd  tu_move
            dd  tu_size
            dd  tu_create
            dd  tu_delete
            dd  tu_active
            dd  tu_details
            dd  tu_help
            dd  tu_about

key_tbl     db  'Q',CMD_EXIT,    'M',CMD_MOVE,     'S',CMD_SIZE
            db  'C',CMD_CREATE,  'D',CMD_DELETE,   'A',CMD_ACTIVE
            db  'B',CMD_BOOT,    'P',CMD_DETAILS,  'V',CMD_LOG
            db  'R',CMD_REREAD,  'N',CMD_NEXTDISK, '?',CMD_HELP
            db  0
gkey_tbl    db  SC_DEL,CMD_DELETE,       SC_F1,CMD_HELP
            db  SC_F5,CMD_REREAD,        SC_SHTAB,CMD_PREVDISK
            db  SC_ALTX,CMD_EXIT,        SC_F10,CMD_MENU0
            db  SC_ALTF,CMD_MENU0,       SC_ALTD,CMD_MENU0+1
            db  SC_ALTP,CMD_MENU0+2,     SC_ALTH,CMD_MENU0+3
            db  0

rf_tbl      dd  msg_rfnone, msg_rfsame, msg_rfnotfat, msg_rfread
            dd  msg_rfwrite, msg_rftoosmall, msg_rfoverlap, msg_rfbounds
            dd  msg_rfgeom, msg_rfarmed, msg_rfremap, msg_rffattype

tui_on      db  0                       ; puts and putc go to cap_buf
tui_dstate  db  0                       ; 0 fine, 1 no disk, 2 unreadable,
                                        ; 3 no table
fd_rc       db  0                       ; what the last verb ended with
pv_attr     db  A_PVOK
tu_hascont  db  0                       ; the disk has an extended partition
tu_chok     db  0                       ; and its chain read cleanly
dr_moved    db  0
            db  0
tui_disk    dd  0
tui_ndisks  dd  0
tr_n        dd  0
tr_sel      dd  0
ls_top      dd  0
tu_stat     dd  0                       ; -> what the status line says
tu_slot     dd  0                       ; the first empty primary slot, or 0
tu_lnum     dd  0                       ; the number a new logical drive takes
tu_cbase    dd  0                       ; the container, start to end
tu_cend     dd  0
pv_mode     dd  0
pv_start    dd  0
pv_len      dd  0
rs_start    dd  -1                      ; select what holds this after a change
wr_verb     dd  0
dr_start0   dd  0                       ; the drag: where it began
dr_len      dd  0
dr_col0     dd  0
mv_lo       dd  0                       ; where a move may put it
mv_hi       dd  0
mv_cur      dd  0
mv_new      dd  0
mo_rlo      dd  0
mo_rhi      dd  0
rm_end      dd  0
ft_start    dd  0
ft_end      dd  0
sz_start    dd  0                       ; the resize in hand
sz_cur      dd  0
sz_new      dd  0
sz_min      dd  0
sz_max      dd  0
szs_req     dd  0
cr_num      dd  0                       ; the create in hand
cr_req      dd  0
cg_kind     dd  0
cg_start    dd  0
cg_end      dd  0
ca_start    dd  0
iv_n        dd  0
sw_lo       dd  0
sw_hi       dd  0
sw_kind     dd  0
sw_min      dd  0
cap_len     dd  0
iv_tbl      dd  IV_MAX * 2 dup(0)
ts_tmp      db  TR_LEN dup(0)
st_buf      db  TUI_COLS dup(0)
vt_buf      db  44 dup(0)

tui_colors  db  1Fh,3Fh,5Fh,6Fh         ; white on blue, cyan, magenta and
                                        ; brown - green and red are the
                                        ; preview's

msg_ttitle  db  'FDISK - Partition Editor$'
msg_closebox db '[',0C4h,']$'
msg_tnodisk db  'No disk.$'
msg_tunread db  'Sector 0 of this disk will not read.$'
msg_tnotable db 'This disk has no partition table (no AA55 signature).$'
msg_shints  db  'F1 Help  F10 Menus  Tab Disk  M Move  S Resize  C Create  '
            db  'Del Delete  Q Quit$'
msg_tnokey  db  'Not a key FDISK uses.  F1 lists them.$'
msg_tnotvol db  'Select a partition first.$'
msg_tpickfree db 'Select free space first - click it on the bar.$'
msg_tpickvol db 'Select one of the logical drives inside it.$'
msg_tnothing db 'Select a partition to delete.$'
msg_tnotprim db 'Only a primary partition can be the active one.$'
msg_talready db 'It is the active partition already.$'
msg_tnoboot db  'There is no partition table on this disk to keep.$'
msg_tnolog  db  'Nothing has been run yet.$'
msg_tleft   db  'Left where it was.$'
msg_tworking db 'Working...$'

msg_dmove   db  'Move $'
msg_dlet    db  ' - let go to move it, Esc to leave it$'
msg_npart   db  'partition $'
msg_npartc  db  'Partition $'
msg_nopen   db  ' ($'
msg_nclose  db  ':)$'
msg_comma3  db  ', $'

msg_tmove   db  'Move Partition$'
msg_tsize   db  'Resize Partition$'
msg_tcreate db  'Create Partition$'
msg_tdelete db  'Delete Partition$'
msg_tactive db  'Active Partition$'
msg_tboot   db  'Boot Code$'
msg_tabout  db  'About FDISK$'
msg_tdone   db  'Done$'
msg_tfailed db  'Not Done$'
msg_trefused db 'Not Possible$'
msg_tbadmb  db  'Size$'

msg_bok     db  'OK$'
msg_bcancel db  'Cancel$'
msg_byes    db  'Yes$'
msg_bno     db  'No$'

msg_lstarts db  'Starts at   $'
msg_lroom   db  'Free room   $'
msg_lroomto db  ' to $'
msg_ldrag   db  'Dragging it along the bar does the same.$'
msg_lnow    db  'Size now    $'
msg_lsmall  db  'Smallest    $'
msg_llarge  db  'Largest     $'
msg_lfree   db  'Free space  $'
msg_lfrom   db  'From        $'
msg_linext  db  'It goes inside the extended partition.$'
msg_fstart  db  'New start in MB:$'
msg_fsize   db  'New size in MB:$'
msg_fcrsize db  'Size in MB:$'
msg_cext    db  'Extended partition, to hold logical drives$'

msg_qmove   db  'Move $'
msg_qsize   db  'Resize $'
msg_qfrom   db  ' from $'
msg_qto     db  ' to $'
msg_qmove2  db  'All of it is copied, which can take a while.$'
msg_qcreate db  'Create a $'
msg_qcrpart db  ' partition at $'
msg_qcrext  db  ' extended partition at $'
msg_qcrlog  db  ' logical drive at $'
msg_qcreate2 db 'It needs FORMAT, after a restart, before it holds files.$'
msg_qdelete db  'Delete $'
msg_qdelete2 db 'Everything stored on it will be lost.$'
msg_qdelext db  'It holds no logical drives.$'
msg_qactive db  'Make $'
msg_qactive2 db ' the active partition?$'
msg_qactive3 db 'The computer starts from the active partition.$'
msg_qboot   db  'Replace the boot code on disk $'
msg_qboot2  db  'The partition table is kept as it is.$'
msg_qboot3  db  'A boot manager kept in the boot code is lost.$'

msg_xthere  db  'It is there already.$'
msg_xsize   db  'It is that size already.$'
msg_xbadmb  db  'Type a number of megabytes, such as 40 or 40.5.$'
msg_xnoslot db  'All four primary partition slots are in use.$'
msg_xnolink db  'The extended partition cannot take another logical drive.$'
msg_xbootsame db 'The boot code on this disk is FDISK',27h,'s already.$'

msg_rfnone  db  'The engine did not say why.$'
msg_rfsame  db  'Nothing would change.$'
msg_rfnotfat db 'There is no FAT12, FAT16 or FAT32 volume in it to move or resize.$'
msg_rfread  db  'A sector would not read.$'
msg_rfwrite db  'A sector would not write.  The disk may be damaged.$'
msg_rftoosmall db 'Files are stored past that point, so it cannot be that small.$'
msg_rfoverlap db 'It would run into the partition next to it.$'
msg_rfbounds db 'That is outside the space it may use.$'
msg_rfgeom  db  'No FAT layout fits that size.$'
msg_rfarmed db  'Writing is turned off.$'
msg_rfremap db  'The cluster size would have to change.  FDISK does not do that.$'
msg_rffattype db 'The FAT type would have to change.  FDISK does not do that.$'

msg_vpart   db  'Partition $'
msg_vlog    db  'Output Log$'
msg_vhelp   db  'FDISK Help$'
msg_about1  db  'FDISK, the PM-DOS partition editor$'
msg_about2  db  'Moves and resizes FAT partitions$'

msg_tenv    db  'FDTEST=',0
msg_thold   db  'FDHOLD=',0
msg_tdumpend db '----',13,10
msg_tkeys   db  13,10,'  ON THE SCREEN',13,10,13,10
            db  '  Drag a block along the bar to move the partition.',13,10,13,10
            db  '    M        Move it, to a start typed in megabytes',13,10
            db  '    S        Resize it, to a size in megabytes',13,10
            db  '    C        Create a partition in the free space selected',13,10
            db  '    Del, D   Delete it',13,10
            db  '    A        Mark partition as active',13,10
            db  '    P,       Enter how far the partition can grow or shrink',13,10
            db  '    B        Write Master Boot Record',13,10
            db  '    V        Show the output of the last change',13,10
            db  '    Tab      Move to next disk',13,10
            db  '    F5, R    Read the disk again',13,10
            db  '    F10, Alt Access menus',13,10
            db  '    Q, Esc   End FDISK',13,10,13,10
            db  '  Only FAT12, FAT16 and FAT32 partitions can be moved or '
            db  'resized.',13,10
            db  '  Restart after a change before using the disk.',13,10
            db  13,10,'  FROM THE COMMAND LINE',13,10,13,10,'$'
