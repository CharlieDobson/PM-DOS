;=====================================================================
; FDTUI.INC - THE SCREEN: what FDISK shows when it is run with nothing
;             after it.
;
; Included by FDISK.ASM after the verbs and before the data.  It
; replaces the line-at-a-time menu of v0.60, and it keeps the one idea
; that menu was built on: IT IS A SHELL OVER THE COMMAND LINE AND NOT
; A SECOND IMPLEMENTATION.  Every key here fills fd_args exactly as
; the command line would, jumps to the same verb, and reads the same
; fd_done or fd_fail at the end of it.  The verbs still print - what
; they print is what a person needs to read - and while the screen is
; up puts and putc put the text in cap_buf instead of on the console,
; where the panel under the table shows the end of it and V shows all
; of it.
;
; WHAT THE SCREEN ADDS is the thing a transcript cannot: the disk as a
; bar, each partition a run of cells the length of its share of the
; disk, and a second bar under it that shows WHAT THE DISK WOULD LOOK
; LIKE after the move, resize, create or delete being considered.
; That bar is drawn from the numbers the verb's DRY RUN left behind -
; pl_nstart and pl_nlen for a move or a resize, cr_start and cr_len
; or lg_start and lg_len for a create - so the picture is of what the
; engine would actually write, not of what was typed.  Green when the
; dry run said yes, red when it refused, and the refusal itself is in
; the panel below.
;
; WRITES ARE ARMED AS THEY ALWAYS WERE.  Nothing here writes a sector
; on one key: a change is a dry run, then Enter, then a question, and
; if the writes are not armed the question says so and asks.  W arms
; them for the session and the title bar says so in red.
;
; TESTABLE FROM A FILE, like the menu it replaces.  With FDTEST=path
; in the environment the keys come from standard input as EDIT reads
; them (TOOLS\MKEDKEYS.PL: a byte a key, 0 and a scan code for a grey
; one), FFh FFh writes the screen to the path, and an input that runs
; out is Esc from everything and then Q - so a script that ends early
; ends the program rather than hanging it.  DISK\FDMTEST.BAT and
; TOOLS\MKFDKEYS.PL are that script.
;=====================================================================

TUI_COLS        EQU     80
TUI_ROWS        EQU     25
TUI_CELLS       EQU     TUI_COLS * TUI_ROWS
BAR_COL         EQU     6               ; the bars start here...
BAR_W           EQU     70              ; ...and are this wide
ROW_NOW         EQU     2
ROW_THEN        EQU     3
ROW_SCALE       EQU     4
ROW_THEAD       EQU     5
ROW_TFIRST      EQU     6
TBL_ROWS        EQU     12              ; rows 6..17
ROW_OUTHEAD     EQU     18
ROW_OUT         EQU     19
OUT_ROWS        EQU     4               ; rows 19..22
ROW_HELP        EQU     23
ROW_STAT        EQU     24

ATTR_NORM       EQU     07h
ATTR_DIM        EQU     08h
ATTR_TITLE      EQU     70h
ATTR_SEL        EQU     70h
ATTR_HEAD       EQU     0Fh
ATTR_STAT       EQU     0Eh
ATTR_ARMED      EQU     4Fh
ATTR_PVOK       EQU     2Fh             ; white on green: a dry run that
ATTR_PVNO       EQU     4Fh             ; said yes, and on red, one that did not
ATTR_CONT       EQU     07h             ; an extended container's own cells

CAP_MAX         EQU     8192
VW_MAXLINES     EQU     512             ; the viewer indexes this many lines
TUI_NCOLORS     EQU     4
KEY_EOF         EQU     0FFFFh          ; the key file ran out

;--- one row of the table: a slot, a link, or a place to create ------
TR_KIND         EQU     00h             ; dd - TK_*
TR_NUM          EQU     04h             ; dd - what the command line calls it
TR_TYPE         EQU     08h             ; dd - the type byte
TR_ACT          EQU     0Ch             ; dd - 1 if active
TR_START        EQU     10h             ; dd - absolute first sector
TR_SECS         EQU     14h             ; dd
TR_LETTER       EQU     18h             ; dd - 0, or 'C'
TR_ALIGN        EQU     1Ch             ; dd - 1 if off a cylinder boundary
TR_LEN          EQU     20h
TR_MAX          EQU     PART_MAX + EBR_MAX + 1

TK_EMPTY        EQU     0               ; an empty primary slot
TK_PRIMARY      EQU     1
TK_CONTAINER    EQU     2               ; the extended partition itself
TK_LOGICAL      EQU     3
TK_FREELINK     EQU     4               ; where a new logical drive would go

PV_NONE         EQU     0
PV_REPLACE      EQU     1               ; the selected row, somewhere else
PV_ADD          EQU     2               ; a new span
PV_OMIT         EQU     3               ; the selected row, gone

;=====================================================================
; fd_tui - the whole session.  Entered from fd_entry; leaves through
;          fd_done with fd_menu cleared, so that is a real exit.
;=====================================================================
fd_tui:
        mov     byte ptr [fd_menu],1
        mov     byte ptr [tui_on],1
        call    vol_load
        call    tui_ktinit

        ; how many disks the kernel will talk about
        mov     edi,OFFSET dsk_rec
        mov     edx,80h
        mov     eax,0F10Ch
        int     21h
        mov     [tui_ndisks],ecx
        cmp     ecx,MAX_DISKS
        jbe     tui_nd_ok
        mov     dword ptr [tui_ndisks],MAX_DISKS
tui_nd_ok:
        or      ecx,ecx
        jnz     tui_nd_some
        mov     dword ptr [tui_ndisks],1  ; one row saying "no disk" beats
tui_nd_some:                              ; a screen with nothing on it
        mov     dword ptr [tui_disk],0
        mov     dword ptr [tr_sel],0
        mov     dword ptr [tr_top],0
        mov     dword ptr [cap_len],0
        mov     dword ptr [tui_stat],0
        mov     dword ptr [pv_mode],PV_NONE
        call    tui_hidecur
        call    tui_load

tui_loop:
        call    tui_draw
        mov     dword ptr [tui_stat],0
        call    tui_key
        cmp     ax,KEY_EOF
        je      tui_quit
        cmp     al,1Bh
        je      tui_quit
        cmp     al,9
        je      tui_nextdisk
        cmp     al,0
        jne     tui_ascii
        cmp     ah,48h                  ; up
        je      tui_up
        cmp     ah,50h                  ; down
        je      tui_down
        cmp     ah,49h                  ; PgUp
        je      tui_pgup
        cmp     ah,51h                  ; PgDn
        je      tui_pgdn
        cmp     ah,47h                  ; Home
        je      tui_home
        cmp     ah,4Fh                  ; End
        je      tui_end
        cmp     ah,3Bh                  ; F1
        je      tui_help
        cmp     ah,3Ch                  ; F2 = the same as V
        je      tui_view
        jmp     tui_loop
tui_ascii:
        call    fd_upper
        cmp     al,'Q'
        je      tui_quit
        cmp     al,'W'
        je      tui_arm
        cmp     al,'V'
        je      tui_view
        cmp     al,'P'
        je      tui_plan
        cmp     al,'M'
        je      tui_move
        cmp     al,'S'
        je      tui_size
        cmp     al,'C'
        je      tui_create
        cmp     al,'D'
        je      tui_delete
        cmp     al,'A'
        je      tui_active
        cmp     al,'B'
        je      tui_mbr
        cmp     al,'?'
        je      tui_help
        cmp     al,'R'
        je      tui_reload
        cmp     al,'N'
        je      tui_nextdisk
        mov     dword ptr [tui_stat],OFFSET msg_tnokey
        jmp     tui_loop

tui_quit:
        mov     byte ptr [tui_on],0
        mov     byte ptr [fd_menu],0
        call    tui_showcur
        mov     eax,0F000h              ; clear the screen
        int     21h
        jmp     fd_done

;--- moving about ------------------------------------------------------
tui_up:
        cmp     dword ptr [tr_sel],0
        je      tui_loop
        dec     dword ptr [tr_sel]
        jmp     tui_loop
tui_down:
        mov     eax,[tr_sel]
        inc     eax
        cmp     eax,[tr_n]
        jae     tui_loop
        mov     [tr_sel],eax
        jmp     tui_loop
tui_pgup:
        mov     eax,[tr_sel]
        sub     eax,TBL_ROWS
        jns     tui_setsel
        xor     eax,eax
        jmp     short tui_setsel
tui_pgdn:
        mov     eax,[tr_sel]
        add     eax,TBL_ROWS
        cmp     eax,[tr_n]
        jb      tui_setsel
tui_end:
        mov     eax,[tr_n]
        or      eax,eax
        jz      tui_loop
        dec     eax
        jmp     short tui_setsel
tui_home:
        xor     eax,eax
tui_setsel:
        mov     [tr_sel],eax
        jmp     tui_loop

tui_nextdisk:
        mov     eax,[tui_disk]
        inc     eax
        cmp     eax,[tui_ndisks]
        jb      tui_nd_set
        xor     eax,eax
tui_nd_set:
        mov     [tui_disk],eax
        mov     dword ptr [tr_sel],0
        mov     dword ptr [tr_top],0
tui_reload:
        call    tui_load
        jmp     tui_loop

;--- W: arm or disarm, the one thing the screen owns -----------------
tui_arm:
        xor     byte ptr [dk_armed],1
        mov     dword ptr [tui_stat],OFFSET msg_tarmed
        cmp     byte ptr [dk_armed],0
        jne     tui_loop
        mov     dword ptr [tui_stat],OFFSET msg_tdisarm
        jmp     tui_loop

;--- F1: the usage screen, read in the viewer ---------------------------
tui_help:
        mov     dword ptr [cap_len],0
        mov     edx,OFFSET msg_usage
        call    puts
        mov     edx,OFFSET msg_tkeys
        call    puts
tui_view:
        call    tui_viewer
        jmp     tui_loop

;--- P: what /P says about the selected partition ----------------------
tui_plan:
        call    tui_selvol              ; a partition with a volume in it
        jc      tui_loop
        mov     dword ptr [cap_len],0
        call    tui_setdp
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_PLAN
        mov     eax,OFFSET fd_plan
        call    tui_run
        call    tui_viewer
        jmp     tui_loop

;--- M and S: a move or a resize, watched on the second bar -----------
tui_move:
        mov     dword ptr [adj_mode],FDC_MOVE
        jmp     short tui_adjust
tui_size:
        mov     dword ptr [adj_mode],FDC_SIZE
tui_adjust:
        call    tui_selvol
        jc      tui_loop
        ; the plan first, so the limits are in the panel from the start
        mov     dword ptr [cap_len],0
        call    tui_setdp
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_PLAN
        mov     eax,OFFSET fd_plan
        call    tui_dryrun
        mov     eax,[cap_len]           ; the plan stays in the capture;
        mov     [cap_mark],eax          ; each dry run is appended after it
        call    tui_selrow              ; ESI -> the row
        mov     eax,[esi+TR_START]
        cmp     dword ptr [adj_mode],FDC_MOVE
        je      tui_adj_have
        mov     eax,[esi+TR_SECS]
tui_adj_have:
        mov     [adj_cur],eax
        call    fd_spcyl                ; ECX = a cylinder
        or      ecx,ecx
        jnz     tui_adj_step
        mov     ecx,1
tui_adj_step:
        mov     [adj_step],ecx
        mov     byte ptr [adj_dirty],1
tui_adj_loop:
        cmp     byte ptr [adj_dirty],0
        je      tui_adj_show
        call    tui_adjplan             ; the dry run, and the bar from it
tui_adj_show:
        mov     dword ptr [tui_stat],OFFSET msg_tmovekeys
        cmp     dword ptr [adj_mode],FDC_MOVE
        je      tui_adj_draw
        mov     dword ptr [tui_stat],OFFSET msg_tsizekeys
tui_adj_draw:
        call    tui_draw
        call    tui_key
        cmp     ax,KEY_EOF
        je      tui_adj_out
        cmp     al,1Bh
        je      tui_adj_out
        cmp     al,0Dh
        je      tui_adj_enter
        cmp     al,'0'
        jb      tui_adj_grey
        cmp     al,'9'
        ja      tui_adj_grey
        ; a digit opens the number line with that digit already on it
        mov     [adj_first],al
        call    tui_adjtype
        jmp     short tui_adj_loop
tui_adj_grey:
        cmp     al,0
        jne     tui_adj_loop
        mov     ecx,[adj_step]
        cmp     ah,4Bh                  ; left
        je      tui_adj_less
        cmp     ah,4Dh                  ; right
        je      tui_adj_more
        imul    ecx,10
        cmp     ah,49h                  ; PgUp
        je      tui_adj_less
        cmp     ah,51h                  ; PgDn
        je      tui_adj_more
        cmp     ah,47h                  ; Home
        je      tui_adj_home
        cmp     ah,4Fh                  ; End
        je      tui_adj_end
        jmp     short tui_adj_loop
tui_adj_less:
        mov     eax,[adj_cur]
        sub     eax,ecx
        jbe     tui_adj_floor
        cmp     eax,[adj_step]
        jae     tui_adj_set
tui_adj_floor:
        mov     eax,[adj_step]
        jmp     short tui_adj_set
tui_adj_more:
        mov     eax,[adj_cur]
        add     eax,ecx
        jc      tui_adj_loop
        cmp     eax,dword ptr [dsk_rec+DSK_TOTAL]
        jae     tui_adj_loop
        jmp     short tui_adj_set
tui_adj_home:
        mov     eax,[adj_step]          ; the first cylinder boundary
        jmp     short tui_adj_set
tui_adj_end:
        ; a move: the last place the partition still fits; a resize: as
        ; far as the disk goes - both rounded down to a cylinder, and
        ; the engine says what it thinks of them
        call    tui_selrow
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        cmp     dword ptr [adj_mode],FDC_MOVE
        jne     tui_adj_endsz
        sub     eax,[esi+TR_SECS]
        jbe     tui_adj_loop
        jmp     short tui_adj_endcyl
tui_adj_endsz:
        sub     eax,[esi+TR_START]
        jbe     tui_adj_loop
tui_adj_endcyl:
        xor     edx,edx
        div     dword ptr [adj_step]
        mul     dword ptr [adj_step]
        or      eax,eax
        jz      tui_adj_loop
tui_adj_set:
        cmp     eax,[adj_cur]
        je      tui_adj_loop
        mov     [adj_cur],eax
        mov     byte ptr [adj_dirty],1
        jmp     tui_adj_loop

tui_adj_enter:
        cmp     byte ptr [fd_rc],0
        jne     tui_adj_refused
        mov     edx,OFFSET msg_tqmove
        cmp     dword ptr [adj_mode],FDC_MOVE
        je      tui_adj_ask
        mov     edx,OFFSET msg_tqsize
tui_adj_ask:
        call    tui_confirm             ; asks, arms if it has to
        jc      tui_adj_loop
        call    tui_setdp
        mov     eax,[adj_cur]
        call    tui_setnum
        mov     dword ptr [fd_nargs],2
        mov     eax,[adj_mode]
        mov     [fd_cmd],al
        mov     eax,OFFSET fd_change
        call    tui_write               ; for real, then the table again
        jmp     short tui_adj_out
tui_adj_refused:
        mov     dword ptr [tui_stat],OFFSET msg_trefused
        call    tui_draw
        call    tui_key
        jmp     tui_adj_loop
tui_adj_out:
        mov     dword ptr [pv_mode],PV_NONE
        jmp     tui_loop

;--- the dry run behind the second bar --------------------------------
tui_adjplan:
        mov     byte ptr [adj_dirty],0
        mov     eax,[cap_mark]
        mov     [cap_len],eax
        call    tui_setdp
        mov     eax,[adj_cur]
        call    tui_setnum
        mov     dword ptr [fd_nargs],2
        mov     eax,[adj_mode]
        mov     [fd_cmd],al
        mov     eax,OFFSET fd_change
        call    tui_dryrun
        ; fd_change sets pl_nstart and pl_nlen before it plans, so they
        ; hold the proposal whether the plan was accepted or not
        mov     dword ptr [pv_mode],PV_REPLACE
        mov     eax,[pl_nstart]
        mov     [pv_start],eax
        mov     eax,[pl_nlen]
        mov     [pv_len],eax
        call    tui_pvattr
        ret

;--- a number typed in full ---------------------------------------------
tui_adjtype:
        mov     edx,OFFSET msg_tstartq
        cmp     dword ptr [adj_mode],FDC_MOVE
        je      tui_adjt_ask
        mov     edx,OFFSET msg_tsizeq
tui_adjt_ask:
        mov     edi,OFFSET fd_args + FD_ARGLEN
        mov     al,[adj_first]
        call    tui_input
        jc      tui_adjt_out
        mov     esi,OFFSET fd_args + FD_ARGLEN
        call    fd_atosz                ; sectors, or 40M
        jc      tui_adjt_bad
        or      eax,eax
        jz      tui_adjt_bad
        mov     [adj_cur],eax
        mov     byte ptr [adj_dirty],1
tui_adjt_out:
        ret
tui_adjt_bad:
        mov     dword ptr [tui_stat],OFFSET msg_tbadnum
        call    tui_draw
        call    tui_key
        ret

;--- C: create, in an empty slot or on a free link ---------------------
tui_create:
        call    tui_selrow
        cmp     dword ptr [esi+TR_KIND],TK_EMPTY
        je      tui_cr_ok
        cmp     dword ptr [esi+TR_KIND],TK_FREELINK
        je      tui_cr_ok
        mov     dword ptr [tui_stat],OFFSET msg_tnotempty
        jmp     tui_loop
tui_cr_ok:
        mov     dword ptr [cap_len],0
        call    tui_setdp
        mov     dword ptr [fd_nargs],1
        mov     edx,OFFSET msg_tcrsize
        mov     edi,OFFSET fd_args + FD_ARGLEN
        mov     al,0
        call    tui_input
        jnc     tui_cr_size
        cmp     al,1Bh
        je      tui_cr_out              ; never mind
        jmp     short tui_cr_go         ; blank: the whole gap
tui_cr_size:
        mov     dword ptr [fd_nargs],2
        mov     edx,OFFSET msg_tcrtype
        mov     edi,OFFSET fd_args + FD_ARGLEN * 2
        mov     al,0
        call    tui_input
        jnc     tui_cr_type
        cmp     al,1Bh
        je      tui_cr_out              ; never mind
        jmp     short tui_cr_go         ; blank: fd_newtype decides
tui_cr_type:
        mov     dword ptr [fd_nargs],3
tui_cr_go:
        mov     byte ptr [fd_cmd],FDC_CREATE
        mov     eax,OFFSET fd_create
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     tui_cr_refused
        ; the picture comes from what the verb worked out
        call    tui_selrow
        mov     eax,[cr_start]
        mov     ecx,[cr_len]
        cmp     dword ptr [esi+TR_KIND],TK_FREELINK
        jne     tui_cr_span
        mov     eax,[lg_start]
        mov     ecx,[lg_len]
tui_cr_span:
        mov     [pv_start],eax
        mov     [pv_len],ecx
        mov     dword ptr [pv_mode],PV_ADD
        call    tui_pvattr
        mov     edx,OFFSET msg_tqcreate
        call    tui_confirm
        jc      tui_cr_out
        mov     eax,OFFSET fd_create
        call    tui_write
        jmp     short tui_cr_out
tui_cr_refused:
        mov     dword ptr [tui_stat],OFFSET msg_trefused
        call    tui_draw
        call    tui_key
tui_cr_out:
        mov     dword ptr [pv_mode],PV_NONE
        jmp     tui_loop

;--- D: delete the selected partition ----------------------------------
tui_delete:
        call    tui_selrow
        mov     eax,[esi+TR_KIND]
        cmp     eax,TK_EMPTY
        je      tui_del_none
        cmp     eax,TK_FREELINK
        jne     tui_del_ok
tui_del_none:
        mov     dword ptr [tui_stat],OFFSET msg_tnothing
        jmp     tui_loop
tui_del_ok:
        mov     dword ptr [cap_len],0
        call    tui_setdp
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_DELETE
        mov     eax,OFFSET fd_delete
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     tui_del_refused
        mov     dword ptr [pv_mode],PV_OMIT
        mov     edx,OFFSET msg_tqdelete
        call    tui_confirm
        jc      tui_del_out
        mov     eax,OFFSET fd_delete
        call    tui_write
        jmp     short tui_del_out
tui_del_refused:
        mov     dword ptr [tui_stat],OFFSET msg_trefused
        call    tui_draw
        call    tui_key
tui_del_out:
        mov     dword ptr [pv_mode],PV_NONE
        jmp     tui_loop

;--- A: make the selected primary the active one -----------------------
tui_active:
        call    tui_selrow
        cmp     dword ptr [esi+TR_KIND],TK_PRIMARY
        je      tui_act_ok
        mov     dword ptr [tui_stat],OFFSET msg_tnotprim
        jmp     tui_loop
tui_act_ok:
        mov     dword ptr [cap_len],0
        call    tui_setdp
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_ACTIVE
        mov     eax,OFFSET fd_active
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     tui_act_refused
        mov     edx,OFFSET msg_tqactive
        call    tui_confirm
        jc      tui_loop
        mov     eax,OFFSET fd_active
        call    tui_write
        jmp     tui_loop
tui_act_refused:
        mov     dword ptr [tui_stat],OFFSET msg_trefused
        call    tui_draw
        call    tui_key
        jmp     tui_loop

;--- B: the boot code of this disk's master boot record ----------------
tui_mbr:
        mov     dword ptr [cap_len],0
        mov     eax,[tui_disk]
        mov     edi,OFFSET fd_args
        call    tui_putdec0
        mov     dword ptr [fd_nargs],1
        mov     byte ptr [fd_cmd],FDC_MBR
        mov     eax,OFFSET fd_mbr
        call    tui_dryrun
        cmp     byte ptr [fd_rc],0
        jne     tui_mbr_refused
        mov     edx,OFFSET msg_tqmbr
        call    tui_confirm
        jc      tui_loop
        mov     eax,OFFSET fd_mbr
        call    tui_write
        jmp     tui_loop
tui_mbr_refused:
        mov     dword ptr [tui_stat],OFFSET msg_trefused
        call    tui_draw
        call    tui_key
        jmp     tui_loop

;=====================================================================
; Running a verb from here.
;
; tui_run    - EAX = the verb.  fd_args and fd_cmd are set; the verb
;              runs, prints into cap_buf, and comes back here through
;              fd_done or fd_fail with fd_rc saying which.  The stack
;              is put back where it was, not unwound - see fd_back.
; tui_dryrun - the same with the writes held off whatever W said,
;              which is what makes a preview safe to draw on every key
; tui_write  - the same with the writes on, and the table reloaded
;              afterwards because the disk has changed
;=====================================================================
tui_run:
        mov     [fd_mstack],esp
        jmp     eax                     ; the caller decides what is in
                                        ; cap_buf already: a dry run is
                                        ; appended to the plan before it
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

tui_write:
        push    eax
        mov     dword ptr [tui_stat],OFFSET msg_tworking
        call    tui_draw
        pop     eax
        push    ebx
        movzx   ebx,byte ptr [dk_armed]
        push    ebx
        mov     byte ptr [dk_armed],1
        call    tui_run
        pop     ebx
        ; W's arming comes back as it was; a one-off does not stay on
        mov     [dk_armed],bl
        pop     ebx
        mov     dword ptr [pv_mode],PV_NONE
        call    tui_load
        mov     dword ptr [tui_stat],OFFSET msg_twritten
        cmp     byte ptr [fd_rc],0
        je      tui_wr_out
        mov     dword ptr [tui_stat],OFFSET msg_tfailed
tui_wr_out:
        call    tui_draw
        call    tui_key                 ; let it be read
        ret

;--- tui_confirm - EDX -> the question.  CF clear = yes, and the
;--- writes are on for what follows (armed by W, or by the second
;--- question, which is asked only when they are not).
tui_confirm:
        call    tui_yesno
        jc      tui_cf_out
        cmp     byte ptr [dk_armed],0
        jne     tui_cf_yes
        mov     edx,OFFSET msg_tqarm
        call    tui_yesno
        jc      tui_cf_out
tui_cf_yes:
        clc
tui_cf_out:
        ret

;--- tui_yesno - EDX -> a question on the status row.  CF clear on Y.
tui_yesno:
        push    eax
        mov     [tui_stat],edx
        mov     byte ptr [tui_statattr],ATTR_ARMED
tui_yn_again:
        call    tui_draw
        call    tui_key
        cmp     ax,KEY_EOF
        je      tui_yn_no
        cmp     al,1Bh
        je      tui_yn_no
        call    fd_upper
        cmp     al,'N'
        je      tui_yn_no
        cmp     al,'Y'
        jne     tui_yn_again
        mov     byte ptr [tui_statattr],ATTR_STAT
        pop     eax
        clc
        ret
tui_yn_no:
        mov     byte ptr [tui_statattr],ATTR_STAT
        mov     dword ptr [tui_stat],0
        pop     eax
        stc
        ret

;--- the selected row must hold a volume the verbs can plan on -------
tui_selvol:
        call    tui_selrow
        cmp     dword ptr [esi+TR_KIND],TK_PRIMARY
        je      tui_sv_yes
        cmp     dword ptr [esi+TR_KIND],TK_LOGICAL
        je      tui_sv_yes
        mov     dword ptr [tui_stat],OFFSET msg_tnotvol
        stc
        ret
tui_sv_yes:
        clc
        ret

;--- tui_selrow - ESI -> the selected row of tr_tbl -------------------
tui_selrow:
        mov     esi,[tr_sel]
        imul    esi,TR_LEN
        add     esi,OFFSET tr_tbl
        ret

;--- tui_setdp - "d:p" for the selected row into fd_args[0] -----------
tui_setdp:
        pushad
        mov     edi,OFFSET fd_args
        mov     eax,[tui_disk]
        call    tui_putdec
        mov     al,':'
        stosb
        call    tui_selrow
        mov     eax,[esi+TR_NUM]
        call    tui_putdec
        mov     byte ptr [edi],0
        popad
        ret

;--- tui_setnum - EAX in decimal into fd_args[1] -----------------------
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
        xor     ebx,ebx                 ; digits pushed
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

;--- tui_pvattr - green or red, as fd_rc says -------------------------
tui_pvattr:
        mov     byte ptr [pv_attr],ATTR_PVOK
        cmp     byte ptr [fd_rc],0
        je      tui_pva_out
        mov     byte ptr [pv_attr],ATTR_PVNO
tui_pva_out:
        ret

;=====================================================================
; tui_load - the table for the disk in tui_disk: the four slots, the
;            container's chain, and the places a create could go.
;=====================================================================
tui_load:
        pushad
        mov     dword ptr [tr_n],0
        mov     byte ptr [tui_dstate],0
        mov     edx,[tui_disk]
        add     edx,80h
        mov     [pl_drive],dl
        mov     edi,OFFSET dsk_rec
        mov     eax,0F10Ch
        int     21h
        jc      tl_nodisk
        test    byte ptr [dsk_rec+DSK_FLAGS],DSKF_PRESENT
        jz      tl_nodisk

        mov     al,[pl_drive]
        xor     ebx,ebx
        mov     ecx,1
        mov     edi,[mbr_buf]
        call    dk_read
        jc      tl_unread
        mov     edi,[mbr_buf]
        cmp     word ptr [edi+1FEh],MBR_SIG
        jne     tl_notable

        ;--- the four slots, empty ones included: a create needs a row
        xor     ebp,ebp
tl_slot:
        imul    esi,ebp,PE_LEN
        add     esi,[mbr_buf]
        add     esi,PART_OFF
        call    tui_newrow              ; EDI -> a zeroed row
        lea     eax,[ebp+1]
        mov     [edi+TR_NUM],eax
        movzx   eax,byte ptr [esi+PE_TYPE]
        mov     [edi+TR_TYPE],eax
        or      eax,eax
        jz      tl_slotnext             ; TK_EMPTY, and nothing else to say
        mov     dword ptr [edi+TR_KIND],TK_PRIMARY
        call    fd_isext                ; AL = the type byte; CF clear
        jc      tl_notcont              ; when it IS an extended one
        mov     dword ptr [edi+TR_KIND],TK_CONTAINER
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
        jc      tl_noletter
        movzx   eax,al
        mov     [edi+TR_LETTER],eax
tl_noletter:
        push    ebx
        mov     eax,[esi+PE_START]
        mov     edx,eax
        add     edx,[esi+PE_SECS]
        movzx   ebx,word ptr [dsk_rec+DSK_SPT]
        call    fd_aligned
        pop     ebx
        jnc     tl_slotnext
        mov     dword ptr [edi+TR_ALIGN],1
tl_slotnext:
        inc     ebp
        cmp     ebp,PART_MAX
        jb      tl_slot

        ;--- the chain
        call    fd_chain
        jc      tl_done                 ; no container on this disk
        xor     ebp,ebp
tl_link:
        cmp     ebp,[chn_n]
        jae     tl_linksdone
        imul    esi,ebp,CH_LEN
        add     esi,[chn_tbl]
        call    tui_newrow
        lea     eax,[ebp+5]
        mov     [edi+TR_NUM],eax
        mov     dword ptr [edi+TR_KIND],TK_FREELINK
        cmp     dword ptr [esi+CH_VSTART],0
        je      tl_linknext             ; a link with nothing in it
        mov     dword ptr [edi+TR_KIND],TK_LOGICAL
        movzx   eax,byte ptr [esi+CH_TYPE]
        mov     [edi+TR_TYPE],eax
        mov     eax,[esi+CH_VSTART]
        mov     [edi+TR_START],eax
        mov     ecx,[esi+CH_VSECS]
        mov     [edi+TR_SECS],ecx
        push    ebx
        mov     ebx,[tui_disk]
        call    vol_letter
        pop     ebx
        jc      tl_lnoletter
        movzx   eax,al
        mov     [edi+TR_LETTER],eax
tl_lnoletter:
        push    ebx
        mov     eax,[esi+CH_EBR]
        mov     edx,eax
        add     edx,[esi+CH_EXTENT]
        mov     ebx,[chn_base]
        call    fd_aligned
        pop     ebx
        jnc     tl_linknext
        mov     dword ptr [edi+TR_ALIGN],1
tl_linknext:
        inc     ebp
        jmp     short tl_link
tl_linksdone:
        ; and one more, where the next logical drive would go - unless
        ; the walk stopped short, in which case nothing is created in a
        ; chain whose shape could not be established
        cmp     dword ptr [chn_st],CHN_OK
        jne     tl_done
        cmp     ebp,EBR_MAX
        jae     tl_done
        call    tui_newrow
        lea     eax,[ebp+5]
        mov     [edi+TR_NUM],eax
        mov     dword ptr [edi+TR_KIND],TK_FREELINK
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

;=====================================================================
; tui_draw - the whole screen, built in scr_buf and put up in one call.
;=====================================================================
tui_draw:
        pushad
        ;--- clear
        mov     edi,OFFSET scr_buf
        mov     eax,(ATTR_NORM SHL 8) OR ' '
        mov     ecx,TUI_CELLS
        rep     stosw

        ;--- the title bar
        mov     byte ptr [scr_cattr],ATTR_TITLE
        mov     ebx,0
        mov     ecx,0
        call    scr_at
        mov     al,' '
        mov     ecx,TUI_COLS
        call    scr_fill
        mov     ebx,0
        mov     ecx,1
        call    scr_at
        mov     edx,OFFSET msg_ttitle
        call    scr_puts
        mov     ebx,0
        mov     ecx,50
        call    scr_at
        mov     edx,OFFSET msg_tdiskn
        call    scr_puts
        mov     eax,[tui_disk]
        mov     ebx,1
        call    scr_dec
        mov     edx,OFFSET msg_tof
        call    scr_puts
        mov     eax,[tui_ndisks]
        mov     ebx,1
        call    scr_dec
        mov     ebx,0
        mov     ecx,66
        call    scr_at
        mov     edx,OFFSET msg_twoff
        cmp     byte ptr [dk_armed],0
        je      tui_dr_wr
        mov     byte ptr [scr_cattr],ATTR_ARMED
        mov     edx,OFFSET msg_twon
tui_dr_wr:
        call    scr_puts

        ;--- the disk
        mov     byte ptr [scr_cattr],ATTR_NORM
        mov     ebx,1
        mov     ecx,1
        call    scr_at
        mov     edx,OFFSET msg_tdisk
        call    scr_puts
        mov     eax,[tui_disk]
        mov     ebx,1
        call    scr_dec
        mov     al,':'
        call    scr_putc
        cmp     byte ptr [tui_dstate],0
        jne     tui_dr_dstate
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        call    sec2mb
        mov     ebx,7
        call    scr_dec
        mov     edx,OFFSET msg_tmbsp
        call    scr_puts
        movzx   eax,word ptr [dsk_rec+DSK_CYLS]
        mov     ebx,1
        call    scr_dec
        mov     edx,OFFSET msg_tcyl
        call    scr_puts
        movzx   eax,word ptr [dsk_rec+DSK_HEADS]
        call    scr_dec
        mov     edx,OFFSET msg_theads
        call    scr_puts
        movzx   eax,word ptr [dsk_rec+DSK_SPT]
        call    scr_dec
        mov     edx,OFFSET msg_tspt
        call    scr_puts
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        call    scr_dec
        mov     edx,OFFSET msg_tsecs
        call    scr_puts
        mov     eax,dword ptr [dsk_rec+DSK_CHS]
        cmp     eax,dword ptr [dsk_rec+DSK_TOTAL]
        jae     tui_dr_bars
        mov     edx,OFFSET msg_treach
        call    scr_puts
        call    scr_dec
        jmp     short tui_dr_bars
tui_dr_dstate:
        mov     edx,OFFSET msg_tnodisk
        cmp     byte ptr [tui_dstate],1
        je      tui_dr_dsay
        mov     edx,OFFSET msg_tunread
        cmp     byte ptr [tui_dstate],2
        je      tui_dr_dsay
        mov     edx,OFFSET msg_tnotable
tui_dr_dsay:
        call    scr_puts

        ;--- the two bars and the scale under them
tui_dr_bars:
        mov     byte ptr [scr_cattr],ATTR_NORM
        mov     ebx,ROW_NOW
        mov     ecx,0
        call    scr_at
        mov     edx,OFFSET msg_tnow
        call    scr_puts
        mov     ebx,ROW_THEN
        mov     ecx,0
        call    scr_at
        mov     byte ptr [scr_cattr],ATTR_DIM
        cmp     dword ptr [pv_mode],PV_NONE
        je      tui_dr_thenlbl
        mov     byte ptr [scr_cattr],ATTR_NORM
tui_dr_thenlbl:
        mov     edx,OFFSET msg_tthen
        call    scr_puts
        mov     al,' '                  ; an empty bracket until there is
        mov     ecx,BAR_W               ; something to show in it
        call    scr_fill
        mov     al,']'
        call    scr_putc
        mov     byte ptr [scr_cattr],ATTR_NORM
        cmp     byte ptr [tui_dstate],0
        jne     tui_dr_table
        mov     ebx,ROW_NOW
        xor     ebp,ebp                 ; as it is
        call    tui_bar
        cmp     dword ptr [pv_mode],PV_NONE
        je      tui_dr_scale
        mov     ebx,ROW_THEN
        mov     ebp,1                   ; as it would be
        call    tui_bar
tui_dr_scale:
        mov     byte ptr [scr_cattr],ATTR_DIM
        mov     ebx,ROW_SCALE
        mov     ecx,BAR_COL
        call    scr_at
        mov     al,'0'
        call    scr_putc
        mov     eax,dword ptr [dsk_rec+DSK_TOTAL]
        call    sec2mb
        mov     ebx,BAR_W - 4           ; right-aligned under the bar's end
        call    scr_dec
        mov     edx,OFFSET msg_tmb
        call    scr_puts

        ;--- the table
tui_dr_table:
        mov     byte ptr [scr_cattr],ATTR_HEAD
        mov     ebx,ROW_THEAD
        mov     ecx,0
        call    scr_at
        mov     edx,OFFSET msg_tthead
        call    scr_puts
        ; keep the selection on the screen
        mov     eax,[tr_sel]
        cmp     eax,[tr_top]
        jae     tui_dr_topok
        mov     [tr_top],eax
tui_dr_topok:
        sub     eax,TBL_ROWS - 1
        js      tui_dr_topok2
        cmp     eax,[tr_top]
        jbe     tui_dr_topok2
        mov     [tr_top],eax
tui_dr_topok2:
        xor     ebp,ebp                 ; EBP = screen row within the table
tui_dr_row:
        cmp     ebp,TBL_ROWS
        jae     tui_dr_out
        mov     esi,[tr_top]
        add     esi,ebp
        cmp     esi,[tr_n]
        jae     tui_dr_out
        mov     byte ptr [scr_cattr],ATTR_NORM
        cmp     esi,[tr_sel]
        jne     tui_dr_rowattr
        mov     byte ptr [scr_cattr],ATTR_SEL
tui_dr_rowattr:
        lea     ebx,[ebp+ROW_TFIRST]
        mov     ecx,0
        call    scr_at
        mov     al,' '
        mov     ecx,TUI_COLS
        call    scr_fill
        lea     ebx,[ebp+ROW_TFIRST]
        mov     ecx,0
        call    scr_at
        imul    esi,TR_LEN
        add     esi,OFFSET tr_tbl
        call    tui_row
        inc     ebp
        jmp     short tui_dr_row

        ;--- the panel: the end of what the last verb said
tui_dr_out:
        mov     byte ptr [scr_cattr],ATTR_DIM
        mov     ebx,ROW_OUTHEAD
        mov     ecx,0
        call    scr_at
        mov     al,0C4h
        mov     ecx,TUI_COLS
        call    scr_fill
        mov     ebx,ROW_OUTHEAD
        mov     ecx,2
        call    scr_at
        mov     edx,OFFSET msg_toutput
        call    scr_puts
        mov     byte ptr [scr_cattr],ATTR_NORM
        call    tui_panel

        ;--- the keys, and the status line
        mov     byte ptr [scr_cattr],ATTR_DIM
        mov     ebx,ROW_HELP
        mov     ecx,0
        call    scr_at
        mov     edx,OFFSET msg_thelp
        call    scr_puts
        mov     edx,[tui_stat]
        or      edx,edx
        jz      tui_dr_flush
        mov     al,[tui_statattr]
        mov     [scr_cattr],al
        mov     ebx,ROW_STAT
        mov     ecx,0
        call    scr_at
        call    scr_puts
tui_dr_flush:
        call    scr_flush
        popad
        ret

;---------------------------------------------------------------------
; tui_bar - one bar on row EBX.  EBP = 0 for the disk as it is, 1 for
;           the disk as pv_* says it would be.
;
; A partition is a run of cells from start*W/total to end*W/total,
; and never fewer than one cell, so a tiny partition is a mark rather
; than nothing.  Containers go down first and the logical drives are
; drawn over them, which is where they are.
;---------------------------------------------------------------------
tui_bar:
        pushad
        mov     ecx,BAR_COL
        call    scr_at
        mov     byte ptr [scr_cattr],ATTR_DIM
        mov     al,0FAh                 ; free space: a dot
        mov     ecx,BAR_W
        call    scr_fill
        mov     byte ptr [scr_cattr],ATTR_NORM
        mov     al,']'
        call    scr_putc
        mov     [bar_row],ebx
        mov     [bar_mode],ebp

        ; pass 1: containers; pass 2: everything with a volume
        mov     dword ptr [bar_pass],TK_CONTAINER
tb_pass:
        xor     ebp,ebp
tb_row:
        cmp     ebp,[tr_n]
        jae     tb_passdone
        imul    esi,ebp,TR_LEN
        add     esi,OFFSET tr_tbl
        mov     eax,[esi+TR_KIND]
        cmp     dword ptr [bar_pass],TK_CONTAINER
        jne     tb_p2
        cmp     eax,TK_CONTAINER
        jne     tb_next
        jmp     short tb_span
tb_p2:
        cmp     eax,TK_PRIMARY
        je      tb_vol
        cmp     eax,TK_LOGICAL
        jne     tb_next
tb_vol:
        ; in the preview the selected row may be elsewhere or gone
        cmp     dword ptr [bar_mode],0
        je      tb_span
        cmp     ebp,[tr_sel]
        jne     tb_span
        cmp     dword ptr [pv_mode],PV_REPLACE
        je      tb_next
        cmp     dword ptr [pv_mode],PV_OMIT
        je      tb_next
tb_span:
        mov     eax,[esi+TR_START]
        mov     ecx,[esi+TR_SECS]
        call    tui_barcells            ; -> EDI first cell, ECX count
        mov     dl,ATTR_CONT
        mov     al,0B0h
        cmp     dword ptr [bar_pass],TK_CONTAINER
        je      tb_paint
        mov     eax,[esi+TR_NUM]
        call    tui_numchar
        push    eax
        mov     eax,ebp
        call    tui_color
        mov     dl,al
        pop     eax
tb_paint:
        call    tui_paint
tb_next:
        inc     ebp
        jmp     short tb_row
tb_passdone:
        cmp     dword ptr [bar_pass],TK_CONTAINER
        jne     tb_preview
        mov     dword ptr [bar_pass],TK_PRIMARY
        jmp     short tb_pass

tb_preview:
        cmp     dword ptr [bar_mode],0
        je      tb_out
        cmp     dword ptr [pv_mode],PV_REPLACE
        je      tb_pvspan
        cmp     dword ptr [pv_mode],PV_ADD
        jne     tb_out
tb_pvspan:
        mov     eax,[pv_start]
        mov     ecx,[pv_len]
        call    tui_barcells
        mov     dl,[pv_attr]
        mov     al,'+'
        cmp     dword ptr [pv_mode],PV_ADD
        je      tb_pvpaint
        call    tui_selrow
        mov     eax,[esi+TR_NUM]
        call    tui_numchar
tb_pvpaint:
        call    tui_paint
tb_out:
        popad
        ret

;--- tui_barcells - EAX = start, ECX = sectors -> EDI = the first cell
;--- of the bar (an offset into scr_buf), ECX = how many cells
tui_barcells:
        push    eax
        push    edx
        push    ebx
        mov     ebx,dword ptr [dsk_rec+DSK_TOTAL]
        or      ebx,ebx
        jnz     tbc_total
        inc     ebx
tbc_total:
        push    ecx
        mov     ecx,BAR_W
        mul     ecx
        div     ebx
        mov     edi,eax                 ; the first cell
        pop     ecx
        pop     ebx
        pop     edx
        pop     eax
        push    eax
        push    edx
        push    ebx
        mov     ebx,dword ptr [dsk_rec+DSK_TOTAL]
        or      ebx,ebx
        jnz     tbc_total2
        inc     ebx
tbc_total2:
        add     eax,ecx                 ; the end
        jnc     tbc_noc
        mov     eax,ebx
tbc_noc:
        mov     ecx,BAR_W
        mul     ecx
        div     ebx
        cmp     eax,BAR_W
        jbe     tbc_endok
        mov     eax,BAR_W
tbc_endok:
        sub     eax,edi
        ja      tbc_haslen
        mov     eax,1
tbc_haslen:
        mov     ecx,eax
        cmp     edi,BAR_W
        jb      tbc_inside
        mov     edi,BAR_W - 1
        mov     ecx,1
tbc_inside:
        mov     eax,edi
        add     eax,ecx
        cmp     eax,BAR_W
        jbe     tbc_fits
        mov     ecx,BAR_W
        sub     ecx,edi
tbc_fits:
        add     edi,BAR_COL
        mov     eax,[bar_row]
        imul    eax,TUI_COLS
        add     edi,eax
        shl     edi,1
        add     edi,OFFSET scr_buf
        pop     ebx
        pop     edx
        pop     eax
        ret

;--- tui_paint - ECX cells at EDI with AL and attribute DL --------------
tui_paint:
        push    eax
        push    ecx
        push    edi
        mov     ah,dl
tp_one:
        mov     [edi],ax
        add     edi,2
        dec     ecx
        jnz     tp_one
        pop     edi
        pop     ecx
        pop     eax
        ret

;--- tui_numchar - EAX = a partition number -> AL = its character -----
tui_numchar:
        cmp     eax,9
        ja      tnc_letter
        add     al,'0'
        ret
tnc_letter:
        sub     eax,10
        add     al,'A'
        ret

;--- tui_color - EAX = a row index -> AL = its attribute ---------------
tui_color:
        push    ebx
        push    edx
        xor     edx,edx
        mov     ebx,TUI_NCOLORS
        div     ebx
        mov     al,[tui_colors+edx]
        pop     edx
        pop     ebx
        ret

;---------------------------------------------------------------------
; tui_row - one row of the table, ESI -> the row.  The cursor is at
;           the start of the line, with the attribute set.
;---------------------------------------------------------------------
tui_row:
        pushad
        mov     al,' '
        call    scr_putc
        mov     eax,[esi+TR_NUM]
        mov     ebx,2
        call    scr_dec
        mov     al,' '
        call    scr_putc
        mov     eax,[esi+TR_KIND]
        cmp     eax,TK_EMPTY
        je      tr_empty
        cmp     eax,TK_FREELINK
        je      tr_freelink
        mov     al,' '
        cmp     dword ptr [esi+TR_ACT],0
        je      trw_act
        mov     al,'A'
trw_act:
        call    scr_putc
        mov     al,' '
        call    scr_putc
        call    scr_putc
        mov     eax,[esi+TR_TYPE]
        call    scr_hex2
        mov     al,' '
        call    scr_putc
        mov     eax,[esi+TR_TYPE]
        call    tn_find                 ; EDX -> the name, nine wide
        call    scr_puts
        mov     eax,[esi+TR_START]
        mov     ebx,11
        call    scr_dec
        mov     eax,[esi+TR_SECS]
        mov     ebx,12
        call    scr_dec
        mov     eax,[esi+TR_SECS]
        call    sec2mb
        mov     ebx,7
        call    scr_dec
        mov     edx,OFFSET msg_tmbsp
        call    scr_puts
        mov     al,' '
        cmp     dword ptr [esi+TR_ALIGN],0
        je      trw_align
        mov     al,'*'
trw_align:
        call    scr_putc
        mov     al,' '
        call    scr_putc
        mov     eax,[esi+TR_LETTER]
        or      eax,eax
        jz      tr_out
        call    scr_putc
        mov     al,':'
        call    scr_putc
        jmp     short tr_out
tr_empty:
        mov     edx,OFFSET msg_tempty
        call    scr_puts
        jmp     short tr_out
tr_freelink:
        mov     edx,OFFSET msg_tfreelink
        call    scr_puts
tr_out:
        popad
        ret

;---------------------------------------------------------------------
; tui_panel - the last OUT_ROWS lines of cap_buf on the panel rows.
;---------------------------------------------------------------------
tui_panel:
        pushad
        mov     ecx,[cap_len]
        or      ecx,ecx
        jz      tpn_out
        ; count back OUT_ROWS line starts from the end
        mov     esi,OFFSET cap_buf
        add     esi,ecx                 ; one past the end
        cmp     byte ptr [esi-1],10     ; a trailing new line is not a line
        jne     tpn_nolf
        dec     esi
        dec     ecx
        cmp     byte ptr [esi-1],13
        jne     tpn_nolf
        dec     esi
        dec     ecx
tpn_nolf:
        or      ecx,ecx
        jz      tpn_out
        mov     ebx,OUT_ROWS
tpn_back:
        or      ecx,ecx
        jz      tpn_start
        cmp     byte ptr [esi-1],10
        jne     tpn_step
        dec     ebx
        jz      tpn_found
tpn_step:
        dec     esi
        dec     ecx
        jmp     short tpn_back
tpn_start:
        dec     ebx                     ; the first line has no LF before it
tpn_found:
        ; ESI -> the first line to show; EBX = how many rows are unused
        mov     ebp,OUT_ROWS
        sub     ebp,ebx                 ; lines to print
        mov     ebx,ROW_OUT
        mov     edi,OFFSET cap_buf
        add     edi,[cap_len]           ; the end
tpn_line:
        or      ebp,ebp
        jz      tpn_out
        push    ebx
        mov     ecx,0
        call    scr_at
        pop     ebx
        xor     ecx,ecx                 ; columns used
tpn_ch:
        cmp     esi,edi
        jae     tpn_lineend
        mov     al,[esi]
        inc     esi
        cmp     al,10
        je      tpn_lineend
        cmp     al,13
        je      tpn_ch
        cmp     ecx,TUI_COLS
        jae     tpn_ch
        call    scr_putc
        inc     ecx
        jmp     short tpn_ch
tpn_lineend:
        inc     ebx
        dec     ebp
        jmp     short tpn_line
tpn_out:
        popad
        ret

;=====================================================================
; tui_viewer - the whole of cap_buf, a screen at a time.
;=====================================================================
tui_viewer:
        pushad
        ; count the lines and note where each starts, up to VW_MAXLINES
        mov     dword ptr [vw_n],0
        mov     esi,OFFSET cap_buf
        mov     ecx,[cap_len]
        mov     edi,OFFSET vw_starts
        or      ecx,ecx
        jz      tv_counted
        mov     [edi],esi
        add     edi,4
        inc     dword ptr [vw_n]
tv_count:
        or      ecx,ecx
        jz      tv_counted
        mov     al,[esi]
        inc     esi
        dec     ecx
        cmp     al,10
        jne     tv_count
        or      ecx,ecx
        jz      tv_counted
        cmp     dword ptr [vw_n],VW_MAXLINES
        jae     tv_counted
        mov     [edi],esi
        add     edi,4
        inc     dword ptr [vw_n]
        jmp     short tv_count
tv_counted:
        mov     dword ptr [vw_top],0
tv_loop:
        ; draw
        mov     edi,OFFSET scr_buf
        mov     eax,(ATTR_NORM SHL 8) OR ' '
        mov     ecx,TUI_CELLS
        rep     stosw
        mov     byte ptr [scr_cattr],ATTR_NORM
        xor     ebp,ebp                 ; row
tv_row:
        cmp     ebp,TUI_ROWS - 1
        jae     tv_foot
        mov     eax,[vw_top]
        add     eax,ebp
        cmp     eax,[vw_n]
        jae     tv_foot
        mov     esi,[vw_starts+eax*4]
        mov     ebx,ebp
        mov     ecx,0
        call    scr_at
        xor     ecx,ecx
        mov     edi,OFFSET cap_buf
        add     edi,[cap_len]
tv_ch:
        cmp     esi,edi
        jae     tv_rownext
        mov     al,[esi]
        inc     esi
        cmp     al,10
        je      tv_rownext
        cmp     al,13
        je      tv_ch
        cmp     ecx,TUI_COLS
        jae     tv_ch
        call    scr_putc
        inc     ecx
        jmp     short tv_ch
tv_rownext:
        inc     ebp
        jmp     short tv_row
tv_foot:
        mov     byte ptr [scr_cattr],ATTR_TITLE
        mov     ebx,TUI_ROWS - 1
        mov     ecx,0
        call    scr_at
        mov     al,' '
        mov     ecx,TUI_COLS
        call    scr_fill
        mov     ebx,TUI_ROWS - 1
        mov     ecx,1
        call    scr_at
        mov     edx,OFFSET msg_tviewfoot
        call    scr_puts
        call    scr_flush
        ; keys
        call    tui_key
        cmp     ax,KEY_EOF
        je      tv_out
        cmp     al,1Bh
        je      tv_out
        cmp     al,0Dh
        je      tv_out
        cmp     al,' '
        je      tv_pgdn
        cmp     al,0
        jne     tv_ascii
        cmp     ah,48h
        je      tv_up
        cmp     ah,50h
        je      tv_down
        cmp     ah,49h
        je      tv_pgup
        cmp     ah,51h
        je      tv_pgdn
        cmp     ah,47h
        je      tv_home
        cmp     ah,4Fh
        je      tv_end
        jmp     short tv_loop
tv_ascii:
        call    fd_upper
        cmp     al,'Q'
        je      tv_out
        cmp     al,'V'
        je      tv_out
        jmp     short tv_loop
tv_up:
        cmp     dword ptr [vw_top],0
        je      tv_loop
        dec     dword ptr [vw_top]
        jmp     short tv_loop
tv_down:
        mov     eax,[vw_top]
        inc     eax
        jmp     short tv_settop
tv_pgup:
        mov     eax,[vw_top]
        sub     eax,TUI_ROWS - 1
        jns     tv_settop
        xor     eax,eax
        jmp     short tv_settop
tv_pgdn:
        mov     eax,[vw_top]
        add     eax,TUI_ROWS - 1
        jmp     short tv_settop
tv_home:
        xor     eax,eax
        jmp     short tv_settop
tv_end:
        mov     eax,[vw_n]
tv_settop:
        ; the last screenful stays full: top is at most n - rows + 1
        mov     ecx,[vw_n]
        sub     ecx,TUI_ROWS - 1
        jns     tv_havemax
        xor     ecx,ecx
tv_havemax:
        cmp     eax,ecx
        jbe     tv_topok
        mov     eax,ecx
tv_topok:
        mov     [vw_top],eax
        jmp     tv_loop
tv_out:
        popad
        ret

;=====================================================================
; tui_input - EDX -> a prompt, EDI -> FD_ARGLEN bytes for the answer,
;             AL = a first character already typed, or 0.  CF set
;             when there is no answer: AL = 0 for Enter on nothing,
;             which a caller may read as "you decide", and AL = 1Bh
;             for Esc or a key file that ran out, which is "never
;             mind".
;=====================================================================
tui_input:
        push    ebx
        push    ecx
        push    esi
        mov     [in_prompt],edx
        mov     [in_buf],edi
        xor     ecx,ecx                 ; ECX = length so far
        or      al,al
        jz      ti_loop
        mov     [edi],al
        inc     ecx
ti_loop:
        mov     byte ptr [edi+ecx],0
        call    tui_draw
        ; the prompt and the text so far on the status row, over
        ; whatever tui_draw put there
        mov     byte ptr [scr_cattr],ATTR_STAT
        mov     ebx,ROW_STAT
        push    ecx
        mov     ecx,0
        call    scr_at
        mov     al,' '
        mov     ecx,TUI_COLS
        call    scr_fill
        mov     ebx,ROW_STAT
        mov     ecx,0
        call    scr_at
        mov     edx,[in_prompt]
        call    scr_puts
        pop     ecx
        push    ecx
        mov     esi,edi
ti_echo:
        or      ecx,ecx
        jz      ti_echoed
        mov     al,[esi]
        call    scr_putc
        inc     esi
        dec     ecx
        jmp     short ti_echo
ti_echoed:
        pop     ecx
        mov     al,'_'
        call    scr_putc
        call    scr_flush
        call    tui_key
        cmp     ax,KEY_EOF
        je      ti_abandon
        cmp     al,1Bh
        je      ti_abandon
        cmp     al,0Dh
        je      ti_enter
        cmp     al,8
        je      ti_bs
        cmp     al,' '
        jb      ti_loop
        cmp     al,7Eh
        ja      ti_loop
        cmp     ecx,FD_ARGLEN - 1
        jae     ti_loop
        mov     [edi+ecx],al
        inc     ecx
        jmp     short ti_loop
ti_bs:
        or      ecx,ecx
        jz      ti_loop
        dec     ecx
        jmp     short ti_loop
ti_enter:
        mov     byte ptr [edi+ecx],0
        or      ecx,ecx
        jz      ti_empty
        pop     esi
        pop     ecx
        pop     ebx
        clc
        ret
ti_abandon:
        mov     al,1Bh
        jmp     short ti_none
ti_empty:
        xor     al,al
ti_none:
        mov     byte ptr [edi],0
        pop     esi
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; The keyboard, and the key file that stands in for it in a test.
;=====================================================================
tui_key:
        push    ebx
        push    ecx
        push    edx
        cmp     byte ptr [kt_on],0
        jne     tk_file
        mov     eax,0F007h
        int     21h
        jmp     short tk_out
tk_file:
        ; EDIT's format (TOOLSMKEDKEYS.PL): one byte for a key with an
        ; ASCII code, 0 and a scan code for a grey one, FFh FFh for a
        ; picture of the screen
        call    tk_byte
        jc      tk_eof
        or      al,al
        jz      tk_grey
        cmp     al,0FFh
        jne     tk_plain
        call    tk_byte
        jc      tk_eof
        cmp     al,0FFh
        jne     tk_file                 ; a shift-state escape: not used here
        call    tui_dump
        jmp     short tk_file
tk_grey:
        call    tk_byte
        jc      tk_eof
        mov     ah,al
        xor     al,al
        jmp     short tk_out
tk_plain:
        xor     ah,ah
        jmp     short tk_out
tk_eof:
        mov     ax,KEY_EOF
tk_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- tk_byte - one byte of standard input into AL; CF at the end -----
tk_byte:
        mov     ebx,0
        mov     ecx,1
        mov     edx,OFFSET kt_rec
        mov     eax,3F00h
        int     21h
        jc      tkb_eof
        or      eax,eax
        jz      tkb_eof
        mov     al,byte ptr [kt_rec]
        clc
        ret
tkb_eof:
        stc
        ret

;--- tui_ktinit - FDTEST=path in the environment turns the file on ----
tui_ktinit:
        pushad
        mov     esi,SLOT_PSP + PSP_ENV  ; a bracketed constant would be an
        mov     esi,[esi]               ; immediate: load the address first
        or      esi,esi
        jz      tki_out
tki_var:
        cmp     byte ptr [esi],0
        je      tki_out
        mov     edi,OFFSET msg_tenv
        mov     ebx,esi
tki_cmp:
        mov     al,[edi]
        or      al,al
        jz      tki_match
        mov     ah,[ebx]
        cmp     ah,'a'
        jb      tki_cmpc
        cmp     ah,'z'
        ja      tki_cmpc
        sub     ah,20h
tki_cmpc:
        cmp     al,ah
        jne     tki_next
        inc     edi
        inc     ebx
        jmp     short tki_cmp
tki_next:
        ; to the end of this variable
        cmp     byte ptr [esi],0
        je      tki_skipped
        inc     esi
        jmp     short tki_next
tki_skipped:
        inc     esi
        jmp     short tki_var
tki_match:
        ; EBX -> the path.  Create it (truncating), keep the handle.
        mov     edx,ebx
        mov     ecx,0
        mov     eax,3C00h
        int     21h
        jc      tki_out
        mov     [kt_h],eax
        mov     byte ptr [kt_on],1
tki_out:
        popad
        ret

;--- tui_dump - the screen as text, appended to the log --------------
tui_dump:
        pushad
        cmp     byte ptr [kt_on],0
        je      td_out
        mov     esi,OFFSET scr_buf
        xor     ebp,ebp
td_row:
        mov     edi,OFFSET kt_line
        mov     ecx,TUI_COLS
td_ch:
        lodsw
        stosb
        dec     ecx
        jnz     td_ch
        ; trailing blanks off, then CR LF
        mov     ecx,TUI_COLS
td_trim:
        cmp     byte ptr [edi-1],' '
        jne     td_trimmed
        dec     edi
        dec     ecx
        jnz     td_trim
td_trimmed:
        mov     byte ptr [edi],13
        mov     byte ptr [edi+1],10
        add     ecx,2
        mov     edx,OFFSET kt_line
        mov     ebx,[kt_h]
        mov     eax,4000h
        int     21h
        inc     ebp
        cmp     ebp,TUI_ROWS
        jb      td_row
        mov     edx,OFFSET msg_tdumpend
        mov     ecx,6                   ; four dashes and a new line
        mov     ebx,[kt_h]
        mov     eax,4000h
        int     21h
td_out:
        popad
        ret

;=====================================================================
; The screen buffer.  Nothing is drawn on the screen until scr_flush
; puts all of scr_buf there in one call.
;=====================================================================
scr_at:                                 ; EBX = row, ECX = column
        push    eax
        mov     eax,ebx
        imul    eax,TUI_COLS
        add     eax,ecx
        shl     eax,1
        add     eax,OFFSET scr_buf
        mov     [scr_pos],eax
        pop     eax
        ret

scr_putc:                               ; AL, with scr_cattr
        push    eax
        push    edi
        mov     edi,[scr_pos]
        cmp     edi,OFFSET scr_buf + TUI_CELLS * 2
        jae     spc_out
        mov     ah,[scr_cattr]
        mov     [edi],ax
        add     edi,2
        mov     [scr_pos],edi
spc_out:
        pop     edi
        pop     eax
        ret

scr_puts:                               ; EDX -> a '$' string
        push    eax
        push    esi
        mov     esi,edx
sps_one:
        mov     al,[esi]
        cmp     al,'$'
        je      sps_out
        call    scr_putc
        inc     esi
        jmp     short sps_one
sps_out:
        pop     esi
        pop     eax
        ret

scr_fill:                               ; AL, ECX times
        push    ecx
sf_one:
        or      ecx,ecx
        jz      sf_out
        call    scr_putc
        dec     ecx
        jmp     short sf_one
sf_out:
        pop     ecx
        ret

scr_dec:                                ; EAX, right-aligned in EBX
        push    edx
        call    fmt_n
        call    scr_puts
        pop     edx
        ret

scr_hex2:                               ; AL as two hex digits
        push    eax
        push    eax
        shr     al,4
        call    scr_hex1
        pop     eax
        call    scr_hex1
        pop     eax
        ret
scr_hex1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     sh1_put
        add     al,7
sh1_put:
        call    scr_putc
        ret

scr_flush:
        pushad
        mov     esi,OFFSET scr_buf
        xor     ebx,ebx
        mov     ecx,TUI_CELLS
        mov     eax,0F004h
        int     21h
        popad
        ret

tui_hidecur:
        pushad
        mov     ecx,2000h
        mov     eax,0F006h
        int     21h
        popad
        ret

tui_showcur:
        pushad
        mov     eax,0F003h              ; CL = the character height
        int     21h
        movzx   ecx,cl
        cmp     ecx,4
        jb      tsc_def
        cmp     ecx,32
        jbe     tsc_have
tsc_def:
        mov     ecx,16
tsc_have:
        mov     eax,ecx
        dec     eax                     ; the last line...
        sub     ecx,2                   ; ...and the one above it
        shl     ecx,8
        or      ecx,eax
        mov     eax,0F006h
        int     21h
        popad
        ret

;=====================================================================
; Data for the screen.
;=====================================================================
tui_on      db  0                       ; puts and putc go to cap_buf
tui_dstate  db  0                       ; 0 fine, 1 no disk, 2 unreadable,
                                        ; 3 no table
tui_statattr db ATTR_STAT
kt_on       db  0
fd_rc       db  0                       ; what the last verb ended with
adj_first   db  0
pv_attr     db  ATTR_PVOK
tui_disk    dd  0
tui_ndisks  dd  0
tr_n        dd  0
tr_sel      dd  0
tr_top      dd  0
tui_stat    dd  0
pv_mode     dd  0
pv_start    dd  0
pv_len      dd  0
adj_mode    dd  0
adj_cur     dd  0
adj_step    dd  0
adj_dirty   db  0
            db  0,0,0
bar_row     dd  0
bar_mode    dd  0
bar_pass    dd  0
cap_len     dd  0
cap_mark    dd  0                       ; where the plan ends and the dry
                                        ; runs begin
vw_n        dd  0
vw_top      dd  0
in_prompt   dd  0
in_buf      dd  0
kt_h        dd  0
kt_rec      dw  0
            dw  0
scr_pos     dd  0
scr_cattr    db  ATTR_NORM
            db  0,0,0

tui_colors  db  1Fh,3Fh,5Fh,6Fh         ; white on blue, cyan, magenta,
                                        ; brown - green and red are the
                                        ; preview's

msg_ttitle  db  'FDISK - PM-DOS partition editor$'
msg_tdiskn  db  'Disk $'
msg_tof     db  ' of $'
msg_twoff   db  ' writes OFF $'
msg_twon    db  ' WRITES ARMED $'
msg_tdisk   db  'Disk $'
msg_tmbsp   db  ' MB $'
msg_tmb     db  ' MB$'
msg_tcyl    db  ' cyl x $'
msg_theads  db  ' heads x $'
msg_tspt    db  ' sectors = $'
msg_tsecs   db  ' sectors$'
msg_treach  db  ', CHS reaches $'
msg_tnodisk db  '  no disk$'
msg_tunread db  '  LBA 0 will not read$'
msg_tnotable db '  no partition table (no AA55 signature)$'
msg_tnow    db  'Now  [$'
msg_tthen   db  'Then [$'
msg_tthead  db  ' #  A  Type              Start     Sectors   Size      Vol$'
msg_tempty  db  '   (empty)$'
msg_tfreelink db '   (free - C creates a logical drive here)$'
msg_toutput db  ' output $'
msg_thelp   db  'M)ove S)ize C)reate D)elete A)ctive B)oot P)lan V)iew W)rite'
            db  ' Tab Q)uit F1=help$'
msg_tnokey  db  'Not one of the keys - F1 lists them.$'
msg_tarmed  db  'Writes ARMED for this session.  What you confirm now happens.$'
msg_tdisarm db  'Writes off.  From here on nothing is written without asking.$'
msg_tnotvol db  'That row is not a partition with a volume in it.$'
msg_tnotempty db 'C creates in an empty slot or on the free row after the logical drives.$'
msg_tnothing db 'Nothing there to delete.$'
msg_tnotprim db 'Only a primary partition can be made active.$'
msg_trefused db 'Refused - the reason is in the panel.  Any key.$'
msg_tbadnum db  'Not a number.  Sectors, or megabytes written 40M.  Any key.$'
msg_tmovekeys db 'Move: Left/Right a cylinder, PgUp/PgDn ten, Home/End, a number, Enter, Esc$'
msg_tsizekeys db 'Size: Left/Right a cylinder, PgUp/PgDn ten, Home/End, a number, Enter, Esc$'
msg_tstartq db  'Start at sector: $'
msg_tsizeq  db  'Size, in sectors or 40M: $'
msg_tcrsize db  'Size, in sectors or 40M (blank = the whole gap): $'
msg_tcrtype db  'Type byte in hex (blank = work it out): $'
msg_tqmove  db  'Move it there?  Y/N $'
msg_tqsize  db  'Resize it?  Y/N $'
msg_tqcreate db 'Create it?  Y/N $'
msg_tqdelete db 'Delete it?  Y/N $'
msg_tqactive db 'Make it the active partition?  Y/N $'
msg_tqmbr   db  'Replace the boot code of this disk?  Y/N $'
msg_tqarm   db  'Writes are OFF.  Arm them for this one change and write?  Y/N $'
msg_tworking db 'Working...$'
msg_twritten db 'Done.  The table has been read back.  Any key.$'
msg_tfailed db  'That did not happen - the panel says why.  Any key.$'
msg_tviewfoot db 'Up/Down PgUp/PgDn Home/End to read, Esc to go back$'
msg_tenv    db  'FDTEST=',0
msg_tdumpend db '----',13,10
msg_tkeys   db  13,10,'  ON THE SCREEN: Up and Down pick a partition, Tab the '
            db  'next disk.',13,10
            db  '  M moves it and S resizes it: the second bar shows where '
            db  'it would',13,10
            db  '  go, green if the engine agrees and red if it refuses, '
            db  'and Enter asks',13,10
            db  '  before anything is written.  C creates in an empty slot '
            db  'or on the',13,10
            db  '  free row, D deletes, A makes a primary active, B rewrites '
            db  'the boot',13,10
            db  '  code, P shows the plan, V shows all of the last output, '
            db  'W arms the',13,10
            db  '  writes for the session, Q leaves.',13,10,'$'

; The screen's buffers are in the BSS, after img_end: see the end of
; FDISK.ASM.  Sixteen kilobytes of zeros would otherwise be in the file
; and on the floppy, which is full.
