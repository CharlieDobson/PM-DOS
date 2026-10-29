;=====================================================================
; SETVER.ASM - SETVER.EXE: the version table, as a driver and as a
;              command
;
; ONE FILE, TWO JOBS, as it has been since DOS 5.
;
;   DEVICE=C:\PMDOS\SETVER.EXE in CONFIG.SYS loads the front of this
;   file as a character driver.  Its INIT points the list of lists'
;   SETVER field (LoL+37h) at the table below and keeps the table
;   resident up to its last entry.  The kernel looks each program up
;   in it as it loads it and 30h tells the program what it found
;   (sv_find in VDM.INC; sv_ver in RMMEM.INC, under Windows).
;
;   SETVER at the prompt is the same file run as a program.  It lists
;   the table, or adds, changes or removes an entry - IN THE FILE, not
;   in memory, so a change takes effect at the next boot.  It finds the
;   table in the file through the bytes behind the device header,
;   which are laid out as MS-DOS 5's SETVER.EXE lays them out, so
;   either command edits either file.
;
;   "*" is a name only PM-DOS uses: the version for every program the
;   table does not name.  See the table below.
;
; 16-BIT, AND NOT A D32 PROGRAM, because the driver half has to be
; real-mode code in conventional memory, where RMDOS.SYS still reads
; the table after a handover to Windows; and the command half is small
; enough that a 32-bit copy of it would be the bigger thing to keep.
; It runs in a DOS box on PM-DOS, and on MS-DOS as it is.
;
; THE COMMAND LINE is MS-DOS 6.22's, and so are the exit codes, which a
; batch file may test:
;
;   SETVER [drive:path]                              list the table
;   SETVER [drive:path] name.ext n.nn                add or change
;   SETVER [drive:path] name.ext /DELETE [/QUIET]    remove
;
;   0 done             5 not in the table      10 cannot read
;   1 bad switch       6 file not found        11 table damaged
;   2 bad file name    7 bad drive             12 not a SETVER.EXE
;   4 bad version      8 too many parameters   13 table full
;                      9 nothing after a name  14 cannot write
;                                              15 path too long
;
; Built with:  wasm setver.asm / wlink system dos file setver.obj
;=====================================================================

TABLE_LEN   EQU 2048            ; the table's room in the file
PATH_MAX    EQU 68              ; [drive:path] as typed, NUL included
NAME_MAX    EQU 12              ; name.ext
TOK_MAX     EQU 6               ; words on a line; one more is too many

SV_SWITCH   EQU 1               ; the exit codes, and the message
SV_NAME     EQU 2               ; table's index
SV_MEMORY   EQU 3
SV_VERSION  EQU 4
SV_NOENTRY  EQU 5
SV_NOFILE   EQU 6
SV_DRIVE    EQU 7
SV_TOOMANY  EQU 8
SV_MISSING  EQU 9
SV_READ     EQU 10
SV_DAMAGED  EQU 11
SV_NOTOURS  EQU 12
SV_FULL     EQU 13
SV_WRITE    EQU 14
SV_LONGPATH EQU 15

OP_LIST     EQU 0
OP_HELP     EQU 1
OP_ADD      EQU 2
OP_DEL      EQU 3
OP_QDEL     EQU 4               ; /DELETE /QUIET

; The device header and the five fields behind it, as read out of the
; file: offsets from the header.
DVX_NAME    EQU 0Ah
DVX_MAJOR   EQU 13h             ; 1, the layout this command knows
DVX_TBLOFF  EQU 14h             ; dd - the table, from the header
DVX_TBLLEN  EQU 18h             ; dw - its room
DVX_LEN     EQU 1Ah

_TEXT   SEGMENT WORD PUBLIC 'CODE'
        ASSUME  CS:_TEXT, DS:_TEXT, ES:NOTHING, SS:NOTHING

;=====================================================================
; THE DRIVER.  The header is the first byte of the image, which is
; where DOS looks for it in an .EXE that a DEVICE= line names.
;=====================================================================
dev_hdr     dw  0FFFFh, 0FFFFh          ; no next driver
            dw  8000h                   ; a character device
            dw  OFFSET dev_strat
            dw  OFFSET dev_intr
dev_name    db  'SETVERXX'
dev_minor   db  0                       ; the layout's version, 1.0
dev_major   db  1
dev_tbloff  dd  OFFSET ver_tab          ; where the table is, from the
dev_tbllen  dw  TABLE_LEN               ; header, and its room
IF ($ - dev_hdr) NE DVX_LEN
        .ERR    <SETVER.ASM: the header is not the length DVX_LEN says>
ENDIF
dev_pkt     dd  0

dev_strat:
        mov     word ptr cs:[dev_pkt],bx
        mov     word ptr cs:[dev_pkt+2],es
        retf

dev_intr:
        push    bx
        push    ds
        lds     bx,cs:[dev_pkt]
        cmp     byte ptr [bx+2],0       ; INIT is all it answers to, and
        jne     dvi_unknown             ; INIT is past the table: it goes
        jmp     dev_init                ; when INIT is done
dvi_unknown:
        mov     word ptr [bx+3],8103h   ; error, done: unknown command
        pop     ds
        pop     bx
        retf

;=====================================================================
; THE TABLE.  A length byte, the name, then the major and the minor
; version; a zero length after the last entry.  The entries are the
; ones MS-DOS 6.22 ships: programs of the period that would not run on
; a DOS newer than the one they were written for.
;
; AND "*", PM-DOS's own: the version for every program the table does
; not name.  PM-DOS's real version is 0.x, which a program reads as
; DOS 1 and refuses, so the table starts out telling everything 6.22
; (sv_find, VDM.INC).  MS-DOS's SETVER lists the entry and never
; matches it, since no file can be called "*".
;=====================================================================
ver_tab     db  1,'*',6,22
            db  10,'KERNEL.EXE',5,0
            db  10,'DOSOAD.SYS',5,0
            db  11,'EXTDISK.SYS',6,0
            db  11,'REDIR50.EXE',5,0
            db  10,'REDIR5.EXE',5,0
            db  12,'REDIRALL.EXE',5,0
            db  12,'REDIRNP4.EXE',5,0
            db  9,'EDLIN.EXE',5,0
            db  10,'BACKUP.EXE',5,0
            db  10,'ASSIGN.COM',5,0
            db  11,'EXE2BIN.EXE',5,0
            db  8,'JOIN.EXE',5,0
            db  11,'RECOVER.EXE',5,0
            db  12,'GRAFTABL.COM',5,0
            db  11,'LMSETUP.EXE',5,0
            db  11,'STACKER.COM',5,0
            db  10,'NCACHE.EXE',5,0
            db  11,'NCACHE2.EXE',5,0
            db  12,'IBMCACHE.SYS',5,0
            db  11,'XTRADRV.SYS',5,0
            db  8,'2XON.COM',5,0
            db  11,'WINWORD.EXE',4,10
            db  9,'EXCEL.EXE',4,10
            db  7,'LL3.EXE',4,1
            db  10,'REDIR4.EXE',4,0
            db  11,'REDIR40.EXE',4,0
            db  11,'MSREDIR.EXE',4,0
            db  10,'WIN200.BIN',3,40
            db  9,'METRO.EXE',3,31
            db  0
            db  TABLE_LEN - ($ - ver_tab) dup(0)
tab_end:

;=====================================================================
; dev_init - DS:BX -> the INIT packet, with BX and DS on the stack.
;            Keep the driver up to the end of the table in use, and
;            point the list of lists at the table.  Everything from
;            here on is let go when INIT returns.
;=====================================================================
dev_init:
        push    ax
        push    cx
        push    si
        push    es
        push    bx                      ; 30h answers in BX and CX too,
        mov     ah,30h                  ; and BX is the packet
        int     21h
        pop     bx
        cmp     al,5                    ; DOS 5 or later: before 5 that
        jae     dvn_dos5                ; field of the list of lists is
                                        ; something else
        push    bx                      ; or PM-DOS, which says 0.x at
        push    dx                      ; 30h.  A DOS that answers 3306h
        mov     bx,0FFFFh               ; is DOS 5 or later, whatever
        mov     ax,3306h                ; 30h says; DOS 1 to 4 leave BX
        int     21h                     ; as it was.  It answers in DX
        cmp     bx,0FFFFh               ; too.
        pop     dx
        pop     bx
        je      dvn_refuse
dvn_dos5:
        mov     si,OFFSET ver_tab
dvn_walk:
        mov     al,cs:[si]
        or      al,al
        jz      dvn_end
        xor     ah,ah
        add     si,ax
        add     si,3
        cmp     si,OFFSET tab_end
        jb      dvn_walk
        jmp     short dvn_refuse        ; no end in its room: damaged
dvn_end:
        inc     si                      ; the zero stays as well
        push    bx
        mov     ah,52h
        int     21h                     ; ES:BX -> the list of lists, or CF
        jc      dvn_nolol               ; (PM-DOS's, with no low memory)
        mov     word ptr es:[bx+37h],OFFSET ver_tab
        mov     word ptr es:[bx+39h],cs
        pop     bx
        mov     word ptr [bx+0Eh],si    ; the end of what is kept
        mov     word ptr [bx+10h],cs
        mov     word ptr [bx+3],0100h   ; done
        jmp     short dvn_out
dvn_nolol:
        pop     bx
dvn_refuse:
        mov     word ptr [bx+0Eh],0     ; keep nothing: not loaded
        mov     word ptr [bx+10h],cs
        mov     word ptr [bx+3],810Ch   ; error, done: general failure
dvn_out:
        pop     es
        pop     si
        pop     cx
        pop     ax
        pop     ds
        pop     bx
        retf

;=====================================================================
; THE COMMAND.  DOS starts it here with ES = DS = the PSP.
;=====================================================================
start:
        mov     ax,cs
        mov     ds,ax
        mov     [psp_seg],es
        cld
        call    self_path               ; the default file: this one
        call    parse                   ; -> [op], or CF and AL = a code
        jc      fail
        push    cs                      ; ES = DS = this segment from
        pop     es                      ; here on
        cmp     byte ptr [op],OP_HELP
        jne     st_work
        mov     dx,OFFSET msg_help
        call    say
        xor     al,al
        jmp     short sv_exit

st_work:
        cmp     byte ptr [op],OP_ADD    ; the caution comes first, as it
        jne     st_read                 ; does on MS-DOS: before the file
        mov     dx,OFFSET msg_warn      ; is even opened
        call    say
st_read:
        call    tbl_read
        jc      fail
        cmp     byte ptr [op],OP_LIST
        jne     st_change
        call    tbl_list
        jmp     short st_note

st_change:
        call    tbl_find                ; -> SI, CF if it is not there
        jnc     st_found
        cmp     byte ptr [op],OP_ADD
        je      st_add
        mov     al,SV_NOENTRY
        jmp     short fail
st_found:
        call    tbl_remove              ; a change is a removal and an add,
        cmp     byte ptr [op],OP_ADD    ; so the entry moves to the end
        jne     st_write
st_add:
        call    tbl_append
        jc      fail
st_write:
        call    tbl_write
        jc      fail
        cmp     byte ptr [op],OP_QDEL
        je      st_ok
        mov     dx,OFFSET msg_done
        call    say
        call    dev_loaded
        jnc     st_blank
        mov     dx,OFFSET msg_restart
        call    say
        jmp     short st_ok
st_blank:
        mov     dx,OFFSET str_crlf      ; the listing ends with a blank
        call    say                     ; line; the update message does not
        jmp     short st_notload
st_note:
        call    dev_loaded
        jc      st_ok
st_notload:
        mov     dx,OFFSET msg_notload
        call    say
st_ok:
        xor     al,al
sv_exit:
        mov     ah,4Ch
        int     21h

fail:
        push    ax
        mov     dx,OFFSET str_crlf
        call    say
        pop     ax
        push    ax
        xor     ah,ah
        mov     bx,ax
        shl     bx,1
        mov     dx,[msg_errs+bx]
        call    say
        mov     dx,OFFSET msg_usehelp
        call    say
        pop     ax
        jmp     short sv_exit

;=====================================================================
; self_path - path_buf = this program's own path, out of the end of
;             its environment, or plain SETVER.EXE if there is none.
;=====================================================================
self_path:
        push    ds
        push    cs
        pop     es
        mov     di,OFFSET path_buf
        mov     ds,[psp_seg]
        mov     ax,ds:[2Ch]
        or      ax,ax
        jz      sp_default
        mov     ds,ax                   ; DS = the environment
        xor     si,si
        cmp     byte ptr [si],0         ; no strings at all: the zero
        je      sp_strend               ; that ends them is the first byte
sp_find:
        cmp     si,7FF0h                ; 32K is the most there can be
        jae     sp_default
        lodsb
        or      al,al
        jnz     sp_find
        cmp     byte ptr [si],0         ; a second zero ends the strings
        jne     sp_find
sp_strend:
        inc     si
        lodsw                           ; how many strings follow: one
        or      ax,ax
        jz      sp_default
        mov     cx,PATH_MAX + 12        ; no more than path_buf holds
sp_copy:
        lodsb
        stosb
        or      al,al
        jz      sp_done
        loop    sp_copy
sp_default:                             ; none, or too long: the copy in
        pop     ds                      ; the current directory
        push    ds
        mov     di,OFFSET path_buf
        mov     si,OFFSET str_setver
sp_dcopy:
        lodsb
        stosb
        or      al,al
        jnz     sp_dcopy
sp_done:
        pop     ds
        ret

;=====================================================================
; parse - the command tail -> [op], [name_p], [name_len], [new_ver]
;         and path_buf.  CF and AL = an exit code if it is not one
;         this command takes.
;
; The tail is copied, upper-cased and cut into words at spaces and
; tabs; toks holds where each word starts.  Then MS-DOS 6.22's rules:
; /? alone; a word with a colon in it first is [drive:path]; then the
; name; then n.nn, or /DELETE with /QUIET before or after it.
;=====================================================================
parse:
        mov     es,[psp_seg]
        xor     cx,cx
        mov     cl,es:[80h]
        cmp     cl,126
        jbe     pa_len
        mov     cl,126
pa_len:
        mov     si,81h
        mov     di,OFFSET tail_buf
        xor     bx,bx                   ; words so far
pa_skip:
        jcxz    pa_cut
        mov     al,es:[si]
        cmp     al,13
        je      pa_cut
        cmp     al,' '
        je      pa_space
        cmp     al,9
        je      pa_space
        cmp     bx,TOK_MAX
        jae     pa_over
        shl     bx,1
        mov     [toks+bx],di
        shr     bx,1
        inc     bx
pa_word:
        mov     al,es:[si]
        cmp     al,13
        je      pa_wend
        cmp     al,' '
        je      pa_wend
        cmp     al,9
        je      pa_wend
        call    upcase
        mov     [di],al
        inc     di
        inc     si
        loop    pa_word
pa_wend:
        mov     byte ptr [di],0
        inc     di
        jmp     short pa_skip
pa_space:
        inc     si
        dec     cx
        jmp     short pa_skip
pa_over:
        inc     bx                      ; TOK_MAX + 1: too many
pa_cut:
        mov     [ntok],bl
        push    cs
        pop     es

        mov     byte ptr [op],OP_LIST
        or      bx,bx
        jz      pa_ok                   ; no words: list
        cmp     bx,TOK_MAX
        ja      pa_toomany

        mov     si,[toks]               ; /? - and nothing with it
        mov     di,OFFSET sw_help
        call    streq
        jne     pa_nothelp
        cmp     bx,1
        ja      pa_toomany
        mov     byte ptr [op],OP_HELP
        jmp     pa_ok

pa_nothelp:
        mov     word ptr [tokn],0       ; the next word to look at
        mov     si,[toks]
        call    has_colon
        jnc     pa_name
        call    take_path               ; [drive:path] - CF, AL if bad
        jc      pa_ret
        inc     word ptr [tokn]
pa_name:
        call    next_tok                ; -> SI, CF if there are no more
        jc      pa_ok                   ; a path alone: list it
        call    valid_name
        jc      pa_badname
        mov     [name_p],si
        mov     [name_len],cx
        call    next_tok
        jc      pa_missing
        mov     di,OFFSET sw_d
        call    is_switch
        je      pa_del
        mov     di,OFFSET sw_q
        call    is_switch
        je      pa_quiet
        cmp     byte ptr [si],'/'
        je      pa_badswitch
        call    parse_ver               ; -> AX, CF if not a version
        jc      pa_badver
        mov     [new_ver],ax
        mov     byte ptr [op],OP_ADD
        jmp     short pa_last

pa_del:
        mov     byte ptr [op],OP_DEL
        call    next_tok
        jc      pa_ok
        mov     di,OFFSET sw_q
        call    is_switch
        jne     pa_toomany
        mov     byte ptr [op],OP_QDEL
        jmp     short pa_last
pa_quiet:
        call    next_tok                ; /QUIET has to have /DELETE
        jc      pa_badswitch            ; after it
        mov     di,OFFSET sw_d
        call    is_switch
        jne     pa_badswitch
        mov     byte ptr [op],OP_QDEL
pa_last:
        call    next_tok                ; and nothing after that
        jnc     pa_toomany
pa_ok:
        clc
pa_ret:
        ret
pa_toomany:
        mov     al,SV_TOOMANY
        stc
        ret
pa_badname:
        mov     al,SV_NAME
        stc
        ret
pa_missing:
        mov     al,SV_MISSING
        stc
        ret
pa_badswitch:
        mov     al,SV_SWITCH
        stc
        ret
pa_badver:
        mov     al,SV_VERSION
        stc
        ret

;--- next_tok - SI = the next word, CF if there are none -------------
next_tok:
        mov     bx,[tokn]
        cmp     bl,[ntok]
        jae     nt_none
        inc     word ptr [tokn]
        shl     bx,1
        mov     si,[toks+bx]
        clc
        ret
nt_none:
        stc
        ret

;--- is_switch - SI = a word, DI = a short and a long form, each ------
;--- NUL-ended, one after the other.  ZF if the word is either.
is_switch:
        push    di
        call    streq
        pop     di
        je      isw_out
isw_skip:
        cmp     byte ptr [di],0
        je      isw_long
        inc     di
        jmp     short isw_skip
isw_long:
        inc     di
        call    streq
isw_out:
        ret

;--- streq - ZF if the strings at SI and DI are the same --------------
streq:
        push    si
        push    di
seq_loop:
        mov     al,[si]
        cmp     al,[di]
        jne     seq_out
        inc     si
        inc     di
        or      al,al
        jnz     seq_loop
seq_out:
        pop     di
        pop     si
        ret

;--- has_colon - CF if the word at SI has a ':' in it -----------------
has_colon:
        push    si
hc_loop:
        lodsb
        cmp     al,':'
        je      hc_yes
        or      al,al
        jnz     hc_loop
        pop     si
        clc
        ret
hc_yes:
        pop     si
        stc
        ret

;--- upcase - AL, a-z made A-Z ----------------------------------------
upcase:
        cmp     al,'a'
        jb      uc_out
        cmp     al,'z'
        ja      uc_out
        sub     al,'a' - 'A'
uc_out:
        ret

;=====================================================================
; take_path - SI -> a word with a colon in it: [drive:path].  path_buf
;             = it, a backslash if it needs one, and SETVER.EXE.  CF
;             and AL = SV_DRIVE or SV_LONGPATH if it will not do.
;
; The drive is asked of DOS with 29h, which says FFh for a letter with
; no drive behind it and touches no disk to say so.
;=====================================================================
take_path:
        cmp     byte ptr [si+1],':'
        jne     tp_baddrive
        mov     al,[si]
        cmp     al,'A'
        jb      tp_baddrive
        cmp     al,'Z'
        ja      tp_baddrive
        push    si
        mov     [drv_str],al
        mov     si,OFFSET drv_str
        mov     di,OFFSET fcb_buf
        mov     ax,2900h
        int     21h
        pop     si
        cmp     al,0FFh
        je      tp_baddrive

        mov     di,si                   ; how long it is
        xor     cx,cx
tp_len:
        cmp     byte ptr [di],0
        je      tp_lenok
        inc     di
        inc     cx
        jmp     short tp_len
tp_lenok:
        cmp     cx,PATH_MAX - 1
        ja      tp_long
        mov     di,OFFSET path_buf
        rep     movsb
        cmp     byte ptr [di-1],'\'     ; C:\SUB wants one, C:\ and C:
        je      tp_named                ; do not
        mov     ax,di
        sub     ax,OFFSET path_buf
        cmp     ax,2
        jbe     tp_named
        mov     byte ptr [di],'\'
        inc     di
tp_named:
        mov     si,OFFSET str_setver
tp_copy:
        lodsb
        stosb
        or      al,al
        jnz     tp_copy
        clc
        ret
tp_baddrive:
        mov     al,SV_DRIVE
        stc
        ret
tp_long:
        mov     al,SV_LONGPATH
        stc
        ret

;=====================================================================
; valid_name - SI -> a word.  CF unless it is a plain name.ext: one to
;              eight characters, an optional dot and one to three
;              more, nothing a file name may not hold, no path, no
;              wildcard, and not a device's name.  Or "*" alone, the
;              entry for every program not named.  CX = its length.
;=====================================================================
valid_name:
        cmp     word ptr [si],'*'       ; "*" and the NUL that ends it
        jne     vn_file
        mov     cx,1
        clc
        ret
vn_file:
        push    si
        xor     cx,cx                   ; characters so far
        xor     dx,dx                   ; DL = in the name, DH = after the dot
vn_loop:
        lodsb
        or      al,al
        jz      vn_end
        inc     cx
        cmp     al,'.'
        je      vn_dot
        cmp     al,' '
        jbe     vn_bad
        push    cx
        push    di
        mov     di,OFFSET bad_chars
        mov     cx,BAD_CHARS_LEN
        push    es
        push    cs
        pop     es
        repne   scasb
        pop     es
        pop     di
        pop     cx
        je      vn_bad
        or      dh,dh
        jnz     vn_ext
        inc     dl
        cmp     dl,8
        ja      vn_bad
        jmp     short vn_loop
vn_ext:
        inc     dh
        cmp     dh,4                    ; DH counts the dot as one
        ja      vn_bad
        jmp     short vn_loop
vn_dot:
        or      dl,dl
        jz      vn_bad                  ; nothing before it
        or      dh,dh
        jnz     vn_bad                  ; a second one
        mov     dh,1
        jmp     short vn_loop
vn_end:
        or      dl,dl
        jz      vn_bad
        cmp     dh,1
        je      vn_bad                  ; a dot with nothing after it
        pop     si
        call    is_device
        jc      vn_badkept
        clc
        ret
vn_bad:
        pop     si
vn_badkept:
        stc
        ret

;--- is_device - SI -> a valid name.ext.  CF if its name is a device's -
is_device:
        push    cx
        push    di
        mov     di,OFFSET dev_names
id_next:
        cmp     byte ptr [di],0
        je      id_no
        push    si
id_cmp:
        mov     al,[si]
        cmp     al,'.'
        jne     id_notdot
        xor     al,al                   ; the name ends at the dot
id_notdot:
        cmp     al,[di]
        jne     id_differ
        inc     si
        inc     di
        or      al,al
        jnz     id_cmp
        pop     si
        stc
        jmp     short id_out
id_differ:
        pop     si
id_skip:
        cmp     byte ptr [di],0
        je      id_skipped
        inc     di
        jmp     short id_skip
id_skipped:
        inc     di
        jmp     short id_next
id_no:
        clc
id_out:
        pop     di
        pop     cx
        ret

;=====================================================================
; parse_ver - SI -> a word.  AX = the version it spells, major in AL
;             and minor in AH, or CF.  5 is 5.00, 4.1 is 4.10, and it
;             has to come out from 2.11 to 9.99 - the versions DOS has
;             had or could have, in what one digit and two can hold.
;=====================================================================
parse_ver:
        xor     bx,bx                   ; BL = major, BH = minor
        xor     cx,cx                   ; digits read
pv_major:
        lodsb
        or      al,al
        jz      pv_check
        cmp     al,'.'
        je      pv_dot
        sub     al,'0'
        jb      pv_bad
        cmp     al,9
        ja      pv_bad
        inc     cx
        cmp     cx,2
        ja      pv_bad
        xchg    al,bl                   ; major = major * 10 + digit
        mov     ah,10
        mul     ah
        add     bl,al
        jmp     short pv_major
pv_dot:
        jcxz    pv_bad                  ; nothing before the dot
        lodsb
        or      al,al
        jz      pv_check                ; "5." is 5.00
        sub     al,'0'
        jb      pv_bad
        cmp     al,9
        ja      pv_bad
        mov     ah,10
        mul     ah
        mov     bh,al                   ; one digit is tenths
        lodsb
        or      al,al
        jz      pv_check
        sub     al,'0'
        jb      pv_bad
        cmp     al,9
        ja      pv_bad
        add     bh,al
        cmp     byte ptr [si],0         ; and two is all there may be
        jne     pv_bad
pv_check:
        jcxz    pv_bad
        mov     al,bl                   ; major * 100 + minor, from 211
        mov     ah,100                  ; to 999
        mul     ah
        add     al,bh
        adc     ah,0
        cmp     ax,211
        jb      pv_bad
        cmp     ax,999
        ja      pv_bad
        mov     ax,bx
        clc
        ret
pv_bad:
        stc
        ret

;=====================================================================
; tbl_read - read the table out of the file path_buf names into
;            ver_tab, after checking the file is a SETVER.EXE with one
;            and that the table ends inside its room.  [tbl_len] and
;            [tbl_pos] say where it was.  CF and AL = a code if not.
;=====================================================================
tbl_read:
        mov     dx,OFFSET path_buf
        mov     ax,3D00h
        int     21h
        mov     bl,SV_NOFILE
        jc      trd_fail
        mov     [fh],ax

        mov     dx,OFFSET hdr_buf       ; the MZ header
        mov     cx,1Ch
        call    rd_exact
        jc      trd_readerr
        mov     ax,word ptr [hdr_buf]
        cmp     ax,'ZM'
        je      trd_mz
        cmp     ax,'MZ'
        jne     trd_notours
trd_mz:
        mov     ax,word ptr [hdr_buf+8] ; the image starts after the
        mov     dx,ax                   ; header's paragraphs: DX:AX
        mov     cl,4
        shl     ax,cl
        mov     cl,12
        shr     dx,cl
        mov     word ptr [img_pos],ax
        mov     word ptr [img_pos+2],dx
        call    seek_to
        jc      trd_readerr
        mov     dx,OFFSET dvx_buf       ; the device header
        mov     cx,DVX_LEN
        call    rd_exact
        jc      trd_readerr
        mov     si,OFFSET dvx_buf + DVX_NAME
        mov     di,OFFSET dev_name
        mov     cx,8
        repe    cmpsb
        jne     trd_notours
        cmp     byte ptr [dvx_buf+DVX_MAJOR],1
        jne     trd_notours
        mov     cx,word ptr [dvx_buf+DVX_TBLLEN]
        jcxz    trd_damaged
        cmp     cx,TABLE_LEN
        ja      trd_damaged
        mov     [tbl_len],cx
        mov     ax,word ptr [img_pos]
        mov     dx,word ptr [img_pos+2]
        add     ax,word ptr [dvx_buf+DVX_TBLOFF]
        adc     dx,word ptr [dvx_buf+DVX_TBLOFF+2]
        jc      trd_damaged
        mov     word ptr [tbl_pos],ax
        mov     word ptr [tbl_pos+2],dx
        call    seek_to
        jc      trd_readerr
        mov     dx,OFFSET ver_tab
        mov     cx,[tbl_len]
        call    rd_exact
        jc      trd_readerr
        call    fh_close

        call    tbl_end                 ; and it ends where it should
        jc      trd_damagedc
        clc
        ret

trd_readerr:
        mov     bl,SV_READ
        jmp     short trd_close
trd_notours:
        mov     bl,SV_NOTOURS
        jmp     short trd_close
trd_damaged:
        mov     bl,SV_DAMAGED
trd_close:
        call    fh_close
trd_fail:
        mov     al,bl
        stc
        ret
trd_damagedc:
        mov     al,SV_DAMAGED
        stc
        ret

;--- rd_exact - CX bytes from [fh] into DX; CF unless all arrived -----
rd_exact:
        push    cx
        mov     bx,[fh]
        mov     ah,3Fh
        int     21h
        pop     cx
        jc      rx_out
        cmp     ax,cx
        je      rx_out                  ; CF is clear from the compare
        stc
rx_out:
        ret

;--- seek_to - [fh] to DX:AX from the start; CF if refused ------------
seek_to:
        mov     cx,dx
        mov     dx,ax
        mov     bx,[fh]
        mov     ax,4200h
        int     21h
        ret

;--- fh_close - close [fh] if it is open -------------------------------
fh_close:
        mov     bx,[fh]
        or      bx,bx
        jz      fc_out
        mov     ah,3Eh
        int     21h
        mov     word ptr [fh],0
fc_out:
        ret

;=====================================================================
; tbl_end - SI -> the zero that ends the table in ver_tab; CF if no
;           zero comes before the end of [tbl_len], or an entry runs
;           past it.
;=====================================================================
tbl_end:
        mov     si,OFFSET ver_tab
        mov     dx,si
        add     dx,[tbl_len]            ; DX = the end of its room
te_loop:
        cmp     si,dx
        jae     te_bad
        mov     al,[si]
        or      al,al
        jz      te_ok
        xor     ah,ah
        add     si,ax
        add     si,3
        jmp     short te_loop
te_ok:
        clc
        ret
te_bad:
        stc
        ret

;=====================================================================
; tbl_find - SI -> the entry for [name_p], CF if there is none
;=====================================================================
tbl_find:
        mov     si,OFFSET ver_tab
tf_loop:
        mov     al,[si]
        or      al,al
        jz      tf_none
        xor     ah,ah
        cmp     ax,[name_len]
        jne     tf_next
        push    si
        inc     si
        mov     di,[name_p]
        mov     cx,ax
        repe    cmpsb
        pop     si
        je      tf_got
tf_next:
        mov     al,[si]
        xor     ah,ah
        add     si,ax
        add     si,3
        jmp     short tf_loop
tf_got:
        clc
        ret
tf_none:
        stc
        ret

;=====================================================================
; tbl_remove - SI -> an entry.  Close the table up over it and zero
;              what that leaves at the end of its room.
;=====================================================================
tbl_remove:
        mov     al,[si]
        xor     ah,ah
        add     ax,3                    ; AX = the entry's length
        mov     di,si
        add     si,ax
        mov     cx,OFFSET ver_tab
        add     cx,[tbl_len]
        sub     cx,si                   ; what is behind it
        rep     movsb
        mov     cx,ax
        xor     al,al
        rep     stosb
        ret

;=====================================================================
; tbl_append - add [name_p] at [new_ver] after the last entry.  CF and
;              AL = SV_FULL if the table's room will not take it.
;=====================================================================
tbl_append:
        call    tbl_end                 ; SI -> the zero at the end
        mov     ax,si
        add     ax,[name_len]
        add     ax,4                    ; length, version, and a new zero
        mov     dx,OFFSET ver_tab
        add     dx,[tbl_len]
        cmp     ax,dx
        ja      ta_full
        mov     di,si
        mov     ax,[name_len]
        stosb
        mov     cx,ax
        mov     si,[name_p]
        rep     movsb
        mov     ax,[new_ver]
        stosw
        mov     byte ptr [di],0
        clc
        ret
ta_full:
        mov     al,SV_FULL
        stc
        ret

;=====================================================================
; tbl_write - ver_tab back where tbl_read found it.  CF and AL =
;             SV_WRITE if it cannot be done.
;
; THE FILE KEEPS ITS DATE AND ITS ATTRIBUTES.  A read-only SETVER.EXE
; is made writable for the length of the write and read-only again
; after, which is what MS-DOS's SETVER does too.
;=====================================================================
tbl_write:
        mov     dx,OFFSET path_buf
        mov     ax,4300h
        int     21h
        jc      twr_fail
        mov     [fattr],cx
        test    cx,07h                  ; read-only, hidden, system
        jz      twr_open
        mov     ax,4301h
        xor     cx,cx
        int     21h
        jc      twr_fail
twr_open:
        mov     dx,OFFSET path_buf
        mov     ax,3D02h
        int     21h
        jc      twr_attr
        mov     [fh],ax
        mov     bx,ax
        mov     ax,5700h
        int     21h
        mov     [ftime],cx
        mov     [fdate],dx
        mov     ax,word ptr [tbl_pos]
        mov     dx,word ptr [tbl_pos+2]
        call    seek_to
        jc      twr_close
        mov     dx,OFFSET ver_tab
        mov     cx,[tbl_len]
        mov     bx,[fh]
        mov     ah,40h
        int     21h
        jc      twr_close
        cmp     ax,[tbl_len]
        jne     twr_close
        mov     bx,[fh]
        mov     cx,[ftime]
        mov     dx,[fdate]
        mov     ax,5701h
        int     21h
        call    fh_close
        call    twr_reattr
        clc
        ret
twr_close:
        call    fh_close
twr_attr:
        call    twr_reattr
twr_fail:
        mov     al,SV_WRITE
        stc
        ret

twr_reattr:
        mov     cx,[fattr]
        test    cx,07h
        jz      twa_out
        mov     dx,OFFSET path_buf
        mov     ax,4301h
        int     21h
twa_out:
        ret

;=====================================================================
; tbl_list - the table, one entry a line: the name, in a column
;            sixteen wide, then the version as n.nn.  A blank line
;            before and after.
;=====================================================================
tbl_list:
        mov     dx,OFFSET str_crlf
        call    say
        mov     si,OFFSET ver_tab
tl_entry:
        mov     al,[si]
        or      al,al
        jz      tl_end
        xor     ah,ah
        mov     cx,ax
        push    si
        inc     si
        mov     di,OFFSET line_buf
        cmp     cx,20                   ; no name is longer than 12, but
        jbe     tl_copy                 ; the file is somebody's data
        mov     cx,20
tl_copy:
        mov     bx,cx
        rep     movsb
tl_pad:
        mov     byte ptr [di],' '
        inc     di
        inc     bx
        cmp     bx,16
        jb      tl_pad
        pop     si
        push    si
        mov     al,[si]
        xor     ah,ah
        add     si,ax
        mov     al,[si+1]               ; the major
        call    put_dec
        mov     byte ptr [di],'.'
        inc     di
        mov     al,[si+2]               ; and the minor, always two digits
        aam
        add     ax,3030h
        xchg    al,ah
        stosw
        mov     word ptr [di],0A0Dh
        mov     byte ptr [di+2],0
        mov     dx,OFFSET line_buf
        call    say
        pop     si
        mov     al,[si]
        xor     ah,ah
        add     si,ax
        add     si,3
        jmp     short tl_entry
tl_end:
        mov     dx,OFFSET str_crlf
        call    say
        ret

;--- put_dec - AL as one or two decimal digits at DI, DI past them ----
put_dec:
        aam                             ; AH = tens, AL = units
        or      ah,ah
        jz      pd_units
        push    ax
        mov     al,ah
        add     al,'0'
        stosb
        pop     ax
pd_units:
        add     al,'0'
        stosb
        ret

;=====================================================================
; dev_loaded - CF if a SETVER driver is in DOS's device chain: the
;              chain walked from NUL, the way MS-DOS's SETVER asks.
;=====================================================================
dev_loaded:
        push    es
        mov     ah,52h
        int     21h                     ; ES:BX -> the list of lists
        add     bx,22h                  ; NUL's header
        mov     dx,64                   ; a chain longer than this is a loop
dl_next:
        cmp     bx,0FFF0h               ; a header has to fit in its segment
        ja      dl_no
        mov     si,OFFSET dev_name
        lea     di,[bx+0Ah]
        mov     cx,8
        repe    cmpsb
        je      dl_yes
        les     bx,es:[bx]
        cmp     bx,0FFFFh
        je      dl_no
        dec     dx
        jnz     dl_next
dl_no:
        pop     es
        clc
        ret
dl_yes:
        pop     es
        stc
        ret

;--- say - the ASCIIZ text at DX to standard output -------------------
say:
        push    si
        push    di
        mov     si,dx
        xor     cx,cx
say_len:
        cmp     byte ptr [si],0
        je      say_put
        inc     si
        inc     cx
        jmp     short say_len
say_put:
        jcxz    say_out
        mov     bx,1
        mov     ah,40h
        int     21h
say_out:
        pop     di
        pop     si
        ret

;=====================================================================
; Text.
;=====================================================================
msg_help    db  'Sets the DOS version number reported to a program.',13,10,13,10
            db  'SETVER [drive:path]                            Lists the version table.',13,10
            db  'SETVER [drive:path] filename n.nn              Adds or changes an entry.',13,10
            db  'SETVER [drive:path] filename /DELETE [/QUIET]  Removes an entry.',13,10,13,10
            db  '  [drive:path]   Where the SETVER.EXE to work on is, if not this one.',13,10
            db  '  filename       The program''s file name and extension, with no path.',13,10
            db  '                 * sets the version for every program not in the table.',13,10
            db  '  n.nn           The version to tell the program, from 2.11 to 9.99.',13,10
            db  '  /DELETE or /D  Removes the program''s entry.',13,10
            db  '  /QUIET or /Q   Says nothing when an entry is removed.',13,10,13,10
            db  'The table is in use only when CONFIG.SYS loads SETVER.EXE with a',13,10
            db  'DEVICE= line.  A change takes effect when the computer restarts.',13,10,0

msg_warn    db  13,10,'A program told a version it was not written for can misbehave',13,10
            db  'and damage its files.  Check with the program''s maker first.',13,10,0
msg_done    db  13,10,'The version table has been updated.',13,10,0
msg_restart db  'The change takes effect when the computer restarts.',13,10,0
msg_notload db  'SETVER.EXE is not loaded, so the table is not in use.  To use the',13,10
            db  'table, add a DEVICE= line for SETVER.EXE to CONFIG.SYS and restart.',13,10,0
msg_usehelp db  'Type SETVER /? for help.',13,10,0

msg_errs    dw  OFFSET msg_e0, OFFSET msg_e1, OFFSET msg_e2, OFFSET msg_e3
            dw  OFFSET msg_e4, OFFSET msg_e5, OFFSET msg_e6, OFFSET msg_e7
            dw  OFFSET msg_e8, OFFSET msg_e9, OFFSET msg_e10, OFFSET msg_e11
            dw  OFFSET msg_e12, OFFSET msg_e13, OFFSET msg_e14, OFFSET msg_e15
msg_e0      db  0
msg_e1      db  'Invalid switch.',13,10,0
msg_e2      db  'Invalid file name.  Give the program''s name and extension only.',13,10,0
msg_e3      db  'Not enough memory.',13,10,0
msg_e4      db  'Invalid version.  Give a number from 2.11 to 9.99.',13,10,0
msg_e5      db  'That program is not in the version table.',13,10,0
msg_e6      db  'SETVER.EXE was not found.',13,10,0
msg_e7      db  'Invalid drive.',13,10,0
msg_e8      db  'Too many parameters.',13,10,0
msg_e9      db  'Give a version number or /DELETE after the file name.',13,10,0
msg_e10     db  'SETVER.EXE could not be read.',13,10,0
msg_e11     db  'The version table in SETVER.EXE is damaged.',13,10,0
msg_e12     db  'That file is not a SETVER.EXE with a version table.',13,10,0
msg_e13     db  'The version table is full.',13,10,0
msg_e14     db  'SETVER.EXE could not be written.',13,10,0
msg_e15     db  'The path is too long.',13,10,0

str_setver  db  'SETVER.EXE',0
str_crlf    db  13,10,0
sw_help     db  '/?',0
sw_d        db  '/D',0,'/DELETE',0
sw_q        db  '/Q',0,'/QUIET',0
bad_chars   db  '\/:*?"<>|+,;=[]'
BAD_CHARS_LEN EQU $ - bad_chars
dev_names   db  'CON',0,'AUX',0,'PRN',0,'NUL',0
            db  'COM1',0,'COM2',0,'COM3',0,'COM4',0
            db  'LPT1',0,'LPT2',0,'LPT3',0,'CLOCK$',0,0
drv_str     db  'A:',0

;=====================================================================
; Data.  None of it is kept by the driver: it is all past the table.
;=====================================================================
psp_seg     dw  0
fh          dw  0
fattr       dw  0
ftime       dw  0
fdate       dw  0
op          db  0
ntok        db  0
tokn        dw  0
name_p      dw  0
name_len    dw  0
new_ver     dw  0
tbl_len     dw  0
img_pos     dd  0
tbl_pos     dd  0
toks        dw  TOK_MAX dup(0)
hdr_buf     db  1Ch dup(0)
dvx_buf     db  DVX_LEN dup(0)
fcb_buf     db  40 dup(0)
line_buf    db  48 dup(0)
path_buf    db  PATH_MAX + 16 dup(0)
tail_buf    db  200 dup(0)

_TEXT   ENDS

STACK   SEGMENT WORD STACK 'STACK'
        db      512 dup(0)
STACK   ENDS

        END     start
