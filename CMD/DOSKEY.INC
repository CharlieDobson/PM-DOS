;=====================================================================
; DOSKEY.INC - DOSKEY, built into the interpreter: the line editor,
;              the command history and the macros.
;
; In MS-DOS these are a TSR.  COMMAND.COM asks for a line through
; INT 2Fh AX=4810h, DOSKEY answers if it is loaded, and 21h/0Ah's own
; editor - backspace and the F1-F6 template keys - answers if it is
; not.  Here they are part of the shell and always there, which is
; what DOSKEY in AUTOEXEC.BAT amounted to on every machine that had it,
; and the DOSKEY command is what sets them up: /INSERT and /OVERSTRIKE,
; /BUFSIZE, /HISTORY, /MACROS, /REINSTALL and macro definitions.
;
; WRITTEN AGAINST S:\DOS6SRC\cmd\doskey, NOT FROM MEMORY.  The keys are
; template.asm's table, key for key, and so are the less obvious rules
; in it: a line is edited over a TEMPLATE, which starts as the previous
; line and is hidden until F1, F3 or the right arrow brings it back;
; Backspace at the end of the line hands the character back to the
; template instead of destroying it; Down with nothing typed shows the
; line it is on rather than the next one; F8 searches backwards and
; wraps; F9 counts from the oldest line.  Macros are packed the way
; trans.asm packs them - $G $L $B $T become > < | and Ctrl+T when they
; are DEFINED, $1-$9 $* and $$ stay as written until they are used -
; and listed the way dumpmacs lists them, turned back.
;
; WHAT IS DIFFERENT, AND WHY:
;
;   - The editor only runs when standard input AND standard output are
;     the console (4400h: device, and the console bit on each).  With
;     either redirected - COMMAND < FILE, a CTTY to a serial port - the
;     line comes from 21h/0Ah as it always did, because a file cannot
;     press F3 and a terminal on COM1 cannot be told where its cursor
;     is.
;   - The cursor is PLACED rather than walked.  DOSKEY moves it with
;     INT 10h and so could this, but this program talks to the system
;     only through INT 21h and the kernel's F0h console calls are the
;     INT 21h way to do it.  Every change redraws from the first
;     character it touched to the end of the line, then asks the
;     console where the cursor finished up - which is how a line that
;     wrapped at the bottom of the screen and scrolled it keeps its
;     place: the start row is worked out again from the answer.
;   - Insert mode does not change the cursor's shape.  There is no
;     INT 21h call for the cursor's shape, and ringing a bell on a full
;     line has the same problem: this console has no bell.
;   - A tab is entered as a space.  DOSKEY draws a tab as up to eight
;     columns, which makes every column after it depend on where the
;     line started; nothing typed at a prompt needs one.
;
; THE BUFFER is DOSKEY's: one block of /BUFSIZE bytes, 512 unless told
; otherwise, holding the macros at the front and the history behind
; them.  A new macro that needs room takes it from the oldest history
; lines, as DOSKEY's does.  The block comes from 48h when the shell
; starts; without it the editor still works and remembers nothing.
;
; Included by COMMAND.ASM.
;=====================================================================

KYQ_LINE        EQU     CMDLINE_MAX     ; the longest line the editor makes
KYQ_DEFSIZE     EQU     512             ; DOSKEY's own default /BUFSIZE
KYQ_MINSIZE     EQU     256             ; ...room for two whole lines
KYQ_MAXSIZE     EQU     16384
KYQ_PEND        EQU     512             ; commands a macro or ^T made, waiting
KYQ_SEP         EQU     14h             ; Ctrl+T, DOSKEY's command separator
KYQ_NUMLEN      EQU     13              ; "Line number: ", which F9 wipes

; DOSKEY's switches, as bits.  Up here and not beside the table because
; wasm cannot size a forward-referenced EQU used as an immediate.
KYO_REINST      EQU     01h
KYO_BUFSIZE     EQU     02h
KYO_HIST        EQU     04h
KYO_MACS        EQU     08h
KYO_INSERT      EQU     10h
KYO_OVER        EQU     20h

;---------------------------------------------------------------------
; ky_init - the pool, when the shell starts.
;---------------------------------------------------------------------
ky_init:
        pushad
        xor     eax,eax
        mov     [ky_pool],eax
        mov     [ky_size],eax
        mov     [ky_mused],eax
        mov     [ky_hused],eax
        mov     [ky_hcount],eax
        mov     [ky_hpos],eax
        mov     [ky_lastn],eax
        mov     [ky_pendp],eax
        mov     ebx,KYQ_DEFSIZE
        mov     ah,48h
        int     21h
        jc      kyi_out                 ; no pool: editing, but no memory
        mov     [ky_pool],eax
        mov     dword ptr [ky_size],KYQ_DEFSIZE
kyi_out:
        popad
        ret

;---------------------------------------------------------------------
; ky_pending - a command a macro or a Ctrl+T left waiting.  CF clear:
;              it is in cmd_buf, shown after a prompt as though it had
;              been typed there, and ESI -> it.  CF: none waiting.
;
; DOSKEY does exactly this: it hands COMMAND an empty line so that the
; prompt comes back, and then answers the next request with the next
; piece, echoed.  Each piece of a macro is therefore a command of its
; own, with its own prompt, its own redirection and its own ^C.
;---------------------------------------------------------------------
ky_pending:
        mov     esi,[ky_pendp]
        or      esi,esi
        jz      kyp_none
        push    esi
        call    put_prompt
        pop     esi
        mov     edi,OFFSET cmd_buf + 2
        xor     ecx,ecx
kyp_cp:
        lodsb
        or      al,al
        jz      kyp_end
        cmp     ecx,CMDLINE_MAX
        jae     kyp_cp                  ; too long for a line: cut
        mov     [edi+ecx],al
        inc     ecx
        jmp     short kyp_cp
kyp_end:
        mov     byte ptr [edi+ecx],0
        mov     [cmd_buf+1],cl
        cmp     byte ptr [esi],0        ; an empty one ends the list
        jne     kyp_more
        xor     esi,esi
kyp_more:
        mov     [ky_pendp],esi
        mov     esi,OFFSET cmd_buf + 2
        call    print_asciiz
        call    put_crlf
        clc
        ret
kyp_none:
        stc
        ret

;=====================================================================
; ky_readline - read a line into cmd_buf, in 21h/0Ah's format, with
;               the editor.  CF: stdin or stdout is not the console,
;               and 0Ah has to be used instead - nothing was read.
;=====================================================================
ky_readline:
        mov     ax,4400h                ; both ends the console?
        xor     ebx,ebx
        int     21h
        jc      kyr_not
        and     dl,81h                  ; a device, and console input
        cmp     dl,81h
        je      kyr_inok

        ; ...UNLESS KYTEST IS SET, which is how the editor is tested.
        ; 07h reads a redirected standard input a byte at a time, and a
        ; zero followed by a scan code is exactly how an extended key
        ; arrives from the keyboard - so "COMMAND < KEYTEST.KEY" types
        ; every key the editor knows, Up and F8 and Ctrl+Left included,
        ; which no emulator's keystroke injection here would do.  Only
        ; the INPUT may be a file: the output still has to be the
        ; screen, because the editor places the cursor on it.
        push    esi
        mov     esi,OFFSET ky_testvar
        call    env_find
        pop     esi
        jc      kyr_not
kyr_inok:
        mov     ax,4400h
        mov     ebx,1
        int     21h
        jc      kyr_not
        and     dl,82h                  ; a device, and console output
        cmp     dl,82h
        jne     kyr_not

        mov     ax,0F003h               ; the screen as it is NOW - a DOS
        int     21h                     ; box may have left it 43 or 50
        movzx   eax,dl                  ; lines, or 40 columns
        cmp     eax,20
        jae     kyr_cols
        mov     eax,80
kyr_cols:
        mov     [ky_cols],eax
        movzx   eax,dh
        cmp     eax,2
        jae     kyr_rows
        mov     eax,25
kyr_rows:
        mov     [ky_rows],eax
        mov     ax,0F001h               ; and where the prompt ended
        int     21h
        movzx   eax,dh
        mov     [ky_row0],eax
        movzx   eax,dl
        mov     [ky_col0],eax
        mov     [ky_shown],eax          ; nothing of the line on it yet

        call    ky_copyin               ; the last line: the template
        xor     eax,eax
        mov     [ky_disp],eax
        mov     [ky_cur],eax
        mov     al,[ky_insdef]
        mov     [ky_ins],al
        mov     eax,[ky_hcount]         ; past the newest: Up gets that
        mov     [ky_hpos],eax

kyr_key:
        call    ky_getkey
        or      ah,ah
        jnz     kyr_ext
        cmp     al,13
        je      kyr_enter
        cmp     al,10                   ; DOSKEY ignores these two
        je      kyr_key
        cmp     al,6
        je      kyr_key
        cmp     al,27
        je      kyr_esc
        cmp     al,8
        je      kyr_bs
        cmp     al,3
        je      kyr_break
        cmp     al,9
        jne     kyr_char
        mov     al,' '                  ; see the note on tabs above
kyr_char:
        call    ky_char
        jmp     short kyr_key
kyr_esc:
        call    ky_esc
        jmp     short kyr_key
kyr_bs:
        call    ky_bs
        jmp     short kyr_key

kyr_ext:
        mov     esi,OFFSET ky_extkeys
kyr_scan:
        cmp     byte ptr [esi],0
        je      kyr_key                 ; not one of DOSKEY's: nothing
        cmp     al,[esi]
        je      kyr_found
        add     esi,5
        jmp     short kyr_scan
kyr_found:
        call    dword ptr [esi+1]
        jc      kyr_done                ; F7 and F9 can end the line
        jmp     short kyr_key

kyr_enter:
        mov     ebx,[ky_disp]           ; to the end of the line, as DOSKEY
        mov     [ky_cur],ebx            ; does before it lets go of it
        call    ky_goto
        call    ky_saveline             ; the next template, the newest line
kyr_done:
        mov     ecx,[ky_disp]
        mov     esi,OFFSET ky_buf
        mov     edi,OFFSET cmd_buf + 2
        mov     [cmd_buf+1],cl
        rep     movsb
        mov     byte ptr [edi],13
        clc
        ret

; ^C.  What 0Ah does: "^C" and a new line, and the line is abandoned -
; which here is COMMAND's own ^C, so it goes where that goes.
kyr_break:
        mov     ebx,[ky_disp]
        mov     [ky_cur],ebx
        call    ky_goto
        mov     edx,OFFSET msg_kybrk
        call    print
        jmp     cmd_gohome

kyr_not:
        stc
        ret

; The extended keys, by scan code: template.asm's alt_key_table.
ky_extkeys:
        db      3Bh
        dd      OFFSET ky_right         ; F1: one character of the template
        db      4Dh
        dd      OFFSET ky_right         ; Right: the same
        db      4Bh
        dd      OFFSET ky_left
        db      47h
        dd      OFFSET ky_home
        db      4Fh
        dd      OFFSET ky_end
        db      3Ch
        dd      OFFSET ky_f2            ; copy up to a character
        db      3Dh
        dd      OFFSET ky_f3            ; copy the rest of the template
        db      3Eh
        dd      OFFSET ky_f4            ; delete up to a character
        db      3Fh
        dd      OFFSET ky_f5            ; the line becomes the template
        db      40h
        dd      OFFSET ky_f6            ; a Ctrl+Z
        db      41h
        dd      OFFSET ky_f7            ; list the history
        db      42h
        dd      OFFSET ky_f8            ; search it
        db      43h
        dd      OFFSET ky_f9            ; pick a line by number
        db      48h
        dd      OFFSET ky_up
        db      50h
        dd      OFFSET ky_down
        db      49h
        dd      OFFSET ky_pgup          ; the oldest line
        db      51h
        dd      OFFSET ky_pgdn          ; the newest
        db      52h
        dd      OFFSET ky_insert
        db      53h
        dd      OFFSET ky_del
        db      73h
        dd      OFFSET ky_wleft         ; Ctrl+Left
        db      74h
        dd      OFFSET ky_wright        ; Ctrl+Right
        db      75h
        dd      OFFSET ky_ctrlend       ; delete to the end
        db      77h
        dd      OFFSET ky_ctrlhome      ; delete to the start
        db      6Eh
        dd      OFFSET ky_af7           ; Alt+F7: forget the history
        db      71h
        dd      OFFSET ky_af10          ; Alt+F10: forget the macros
        db      0

;---------------------------------------------------------------------
; ky_getkey - AL = the key; AH = 1 when it is extended and AL is its
;             scan code.  DOSKEY's getkey, through 07h rather than 08h
;             because ^C is the editor's to handle.
;---------------------------------------------------------------------
ky_getkey:
        mov     ah,07h
        int     21h
        or      al,al
        jz      kyg_ext
        xor     ah,ah
        ret
kyg_ext:
        mov     ah,07h
        int     21h
        mov     ah,1
        ret

;---------------------------------------------------------------------
; ky_out - AL onto the screen as the editor shows it: a control
;          character as ^ and its letter, two columns.
;---------------------------------------------------------------------
ky_out:
        push    eax
        push    edx
        cmp     al,20h
        jae     kyo_plain
        push    eax
        mov     dl,'^'
        mov     ah,02h
        int     21h
        pop     eax
        add     al,40h
kyo_plain:
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; ky_lin - EBX = an index into the line -> EAX = the screen column it
;          starts at, counted from the start of the prompt's row and
;          straight on across the wraps.
;---------------------------------------------------------------------
ky_lin:
        push    ecx
        mov     eax,[ky_col0]
        xor     ecx,ecx
kyl_lp:
        cmp     ecx,ebx
        jae     kyl_out
        inc     eax
        cmp     byte ptr [ky_buf+ecx],20h
        jae     kyl_one
        inc     eax                     ; ^X is two columns
kyl_one:
        inc     ecx
        jmp     short kyl_lp
kyl_out:
        pop     ecx
        ret

;---------------------------------------------------------------------
; ky_goto - the cursor to the character at index EBX.
;---------------------------------------------------------------------
ky_goto:
        pushad
        call    ky_lin
        xor     edx,edx
        div     dword ptr [ky_cols]     ; EAX = rows down, EDX = column
        add     eax,[ky_row0]
        mov     ecx,[ky_rows]
        dec     ecx
        cmp     eax,ecx
        jbe     kgo_row
        mov     eax,ecx
kgo_row:
        mov     dh,al
        mov     ax,0F002h
        int     21h
        popad
        ret

;---------------------------------------------------------------------
; ky_draw - the line from index EBX to its end, and blanks over
;           whatever the line used to occupy beyond that; then the
;           cursor back to [ky_cur].
;
; THE START ROW IS RE-DERIVED EVERY TIME, from where the console says
; the cursor stopped.  Printing past the bottom row scrolls the screen
; and nothing tells the program so - except that the cursor is then on
; a row that does not add up.  The printing ended at a known distance
; from the start of the line, so the row the line starts on is the row
; the cursor is on less that many rows.
;---------------------------------------------------------------------
ky_draw:
        pushad
        push    ebx
        call    ky_goto
        mov     ebx,[ky_disp]
        call    ky_lin
        mov     [ky_newend],eax
        pop     esi                     ; the first index to print
kyd_lp:
        cmp     esi,[ky_disp]
        jae     kyd_pad
        mov     al,[ky_buf+esi]
        call    ky_out
        inc     esi
        jmp     short kyd_lp
kyd_pad:
        xor     edx,edx                 ; blanks printed
        mov     ecx,[ky_shown]
        sub     ecx,[ky_newend]
        jbe     kyd_where               ; the line did not get shorter
        mov     edx,ecx
kyd_sp:
        mov     al,' '
        call    ky_out
        loop    kyd_sp
kyd_where:
        mov     ecx,[ky_newend]
        add     ecx,edx                 ; where the printing stopped
        mov     ax,0F001h
        int     21h
        movzx   ebx,dh                  ; ...and the row that is on
        mov     eax,ecx
        xor     edx,edx
        div     dword ptr [ky_cols]
        sub     ebx,eax
        jnc     kyd_row0
        xor     ebx,ebx
kyd_row0:
        mov     [ky_row0],ebx
        mov     eax,[ky_newend]
        mov     [ky_shown],eax
        mov     ebx,[ky_cur]
        call    ky_goto
        popad
        ret

;---------------------------------------------------------------------
; ky_copyin - the template becomes the previous line again: all of it
;             in the buffer, none of it on the screen.
;---------------------------------------------------------------------
ky_copyin:
        pushad
        mov     ecx,[ky_lastn]
        mov     [ky_len],ecx
        mov     esi,OFFSET ky_last
        mov     edi,OFFSET ky_buf
        rep     movsb
        popad
        ret

;---------------------------------------------------------------------
; ky_char - AL typed: over the character under the cursor, or in front
;           of it in insert mode (template.asm's char_t).
;---------------------------------------------------------------------
ky_char:
        pushad
        mov     ebx,[ky_cur]
        cmp     byte ptr [ky_ins],0
        jne     kyc_ins
        cmp     ebx,KYQ_LINE - 1
        jae     kyc_out                 ; full
        mov     [ky_buf+ebx],al
        inc     ebx
        mov     [ky_cur],ebx
        cmp     ebx,[ky_disp]
        jbe     kyc_draw
        mov     [ky_disp],ebx
        cmp     ebx,[ky_len]
        jbe     kyc_draw
        mov     [ky_len],ebx
kyc_draw:
        dec     ebx
        call    ky_draw
kyc_out:
        popad
        ret
kyc_ins:
        cmp     dword ptr [ky_disp],KYQ_LINE - 1
        jae     kyc_out
        mov     ecx,[ky_len]            ; the template grows by one; a full
        cmp     ecx,KYQ_LINE - 1        ; one loses its last character
        je      kyc_mv
        inc     ecx
        mov     [ky_len],ecx
kyc_mv:
        cmp     ecx,ebx
        jbe     kyc_put
        mov     dl,[ky_buf+ecx-1]
        mov     [ky_buf+ecx],dl
        dec     ecx
        jmp     short kyc_mv
kyc_put:
        mov     [ky_buf+ebx],al
        inc     dword ptr [ky_disp]
        inc     ebx
        mov     [ky_cur],ebx
        jmp     short kyc_draw

;---------------------------------------------------------------------
; ky_delete - ECX characters out at the cursor, the template closing up
;             behind them (delete_cx_chars).  The display shrinks by as
;             many, but never to the left of the cursor.
;---------------------------------------------------------------------
ky_delete:
        pushad
        mov     ebx,[ky_cur]
        mov     eax,[ky_len]
        sub     eax,ebx
        jbe     kdl_out                 ; nothing there to delete
        cmp     ecx,eax
        jbe     kdl_n
        mov     ecx,eax
kdl_n:
        mov     eax,[ky_disp]
        sub     eax,ecx
        jc      kdl_dcur
        cmp     eax,ebx
        jae     kdl_dset
kdl_dcur:
        mov     eax,ebx
kdl_dset:
        mov     [ky_disp],eax
        lea     esi,[ebx+ecx]
        mov     edi,ebx
kdl_cp:
        cmp     esi,[ky_len]
        jae     kdl_cpend
        mov     al,[ky_buf+esi]
        mov     [ky_buf+edi],al
        inc     esi
        inc     edi
        jmp     short kdl_cp
kdl_cpend:
        mov     [ky_len],edi
        call    ky_draw                 ; EBX is still the cursor
kdl_out:
        popad
        ret

;---------------------------------------------------------------------
; ky_step - one character right: along the line, or past its end into
;           the template, which then becomes part of the line.  CF if
;           there was nowhere to go.  (right_t, which F1 is too.)
;---------------------------------------------------------------------
ky_step:
        pushad
        mov     ebx,[ky_cur]
        cmp     ebx,[ky_disp]
        jb      kst_move
        cmp     ebx,[ky_len]
        jae     kst_end
        inc     dword ptr [ky_disp]     ; one more of the template
        inc     ebx
        mov     [ky_cur],ebx
        dec     ebx
        call    ky_draw
        popad
        clc
        ret
kst_move:
        inc     ebx
        mov     [ky_cur],ebx
        call    ky_goto
        popad
        clc
        ret
kst_end:
        popad
        stc
        ret

;--- the handlers: each is called from ky_extkeys and says CF clear, or
;--- CF set to end the line with whatever it has left in it ---------

ky_right:
        call    ky_step
        clc
        ret

ky_left:
        mov     ebx,[ky_cur]
        or      ebx,ebx
        jz      kyl2_out
        dec     ebx
        mov     [ky_cur],ebx
        call    ky_goto
kyl2_out:
        clc
        ret

ky_home:
        xor     ebx,ebx
        mov     [ky_cur],ebx
        call    ky_goto
        clc
        ret

ky_end:                                 ; the end of the LINE, not of the
        mov     ebx,[ky_disp]           ; template: end_t
        mov     [ky_cur],ebx
        call    ky_goto
        clc
        ret

ky_f3:                                  ; the whole template, to its end
        mov     ebx,[ky_disp]
        mov     eax,[ky_len]
        cmp     eax,ebx
        jbe     kf3_out
        mov     [ky_disp],eax
        mov     [ky_cur],eax
        call    ky_draw
kf3_out:
        clc
        ret

;--- F2 x: the template up to (not including) the next x --------------
ky_f2:
        call    ky_find
        jc      kf2_out
        mov     [ky_tmp],ebx
kf2_step:
        mov     eax,[ky_cur]
        cmp     eax,[ky_tmp]
        jae     kf2_out
        call    ky_step
        jnc     kf2_step
kf2_out:
        clc
        ret

;--- F4 x: delete from the cursor up to (not including) the next x ----
ky_f4:
        call    ky_find
        jc      kf4_out
        mov     ecx,ebx
        sub     ecx,[ky_cur]
        call    ky_delete
kf4_out:
        clc
        ret

; ky_find - read the character F2 and F4 are given; EBX = where it next
;           occurs in the template after the cursor, CF if nowhere or if
;           the key was not a character.
ky_find:
        call    ky_getkey
        or      ah,ah
        jnz     kfd_no
        mov     ebx,[ky_cur]
kfd_scan:
        inc     ebx
        cmp     ebx,[ky_len]
        jae     kfd_no
        cmp     [ky_buf+ebx],al
        jne     kfd_scan
        clc
        ret
kfd_no:
        stc
        ret

;--- F5: the line is wiped from the screen and kept as the template ---
ky_f5:
        push    dword ptr [ky_len]
        xor     eax,eax
        mov     [ky_cur],eax
        mov     [ky_disp],eax
        xor     ebx,ebx
        call    ky_draw
        pop     dword ptr [ky_len]
        clc
        ret

ky_f6:
        mov     al,1Ah
        call    ky_char
        clc
        ret

;--- Esc: the line goes and the template is the last line again ------
ky_esc:
        xor     eax,eax
        mov     [ky_cur],eax
        mov     [ky_disp],eax
        mov     [ky_len],eax
        xor     ebx,ebx
        call    ky_draw
        call    ky_copyin
        clc
        ret

;--- Backspace: the character to the left.  At the end of the line it
;--- goes back into the template, where F1 can bring it back ---------
ky_bs:
        pushad
        mov     ebx,[ky_cur]
        or      ebx,ebx
        jz      kyb_out
        dec     ebx
        mov     [ky_cur],ebx
        mov     eax,[ky_disp]
        dec     eax
        cmp     eax,ebx
        jne     kyb_del
        mov     [ky_disp],eax
        call    ky_draw
        jmp     short kyb_out
kyb_del:
        mov     ecx,1
        call    ky_delete
kyb_out:
        popad
        ret

ky_del:
        mov     ecx,1
        call    ky_delete
        clc
        ret

ky_insert:
        xor     byte ptr [ky_ins],1
        clc
        ret

ky_ctrlend:                             ; everything from the cursor on,
        mov     ebx,[ky_cur]            ; template included
        mov     [ky_len],ebx
        mov     [ky_disp],ebx
        call    ky_draw
        clc
        ret

ky_ctrlhome:                            ; everything before the cursor
        mov     ecx,[ky_cur]
        xor     eax,eax
        mov     [ky_cur],eax
        call    ky_delete
        clc
        ret

;--- Ctrl+Left: to the start of this word, or of the one before -------
ky_wleft:
kwl_back:
        call    ky_left
        mov     ebx,[ky_cur]
        or      ebx,ebx
        jz      kwl_out
        mov     al,[ky_buf+ebx]
        call    ky_white
        je      kwl_back                ; still in the blanks
kwl_word:
        mov     ebx,[ky_cur]
        or      ebx,ebx
        jz      kwl_out
        mov     al,[ky_buf+ebx-1]
        call    ky_white
        je      kwl_out
        call    ky_left
        jmp     short kwl_word
kwl_out:
        clc
        ret

;--- Ctrl+Right: to the start of the next word, template and all ------
ky_wright:
kwr_word:
        mov     ebx,[ky_cur]
        cmp     ebx,[ky_len]
        jae     kwr_out
        mov     al,[ky_buf+ebx]
        call    ky_white
        je      kwr_blank
        call    ky_step
        jmp     short kwr_word
kwr_blank:
        call    ky_step
        jc      kwr_out
        mov     ebx,[ky_cur]
        mov     al,[ky_buf+ebx]
        call    ky_white
        je      kwr_blank
kwr_out:
        clc
        ret

; ky_white - ZF if AL is a blank or a tab
ky_white:
        cmp     al,' '
        je      kwh_out
        cmp     al,9
kwh_out:
        ret

;--- Up and Down, PgUp and PgDn: through the history ------------------
ky_up:
        mov     eax,[ky_hpos]
        cmp     dword ptr [ky_hcount],0
        je      kup_out
        or      eax,eax
        jz      kup_load                ; the oldest: it stays, reloaded
        dec     eax
        mov     [ky_hpos],eax
kup_load:
        call    ky_loadh
kup_out:
        clc
        ret

; Down with nothing typed shows the line it is on rather than moving -
; which is how Up, edit it away, Down gets the line back.
ky_down:
        cmp     dword ptr [ky_disp],0
        je      kdn_load
        mov     eax,[ky_hpos]
        cmp     eax,[ky_hcount]
        jae     kdn_load
        inc     eax
        mov     [ky_hpos],eax
kdn_load:
        mov     eax,[ky_hpos]
        call    ky_loadh
        clc
        ret

ky_pgup:
        cmp     dword ptr [ky_hcount],0
        je      kpu_out
        xor     eax,eax
        mov     [ky_hpos],eax
        call    ky_loadh
kpu_out:
        clc
        ret

ky_pgdn:
        mov     eax,[ky_hcount]
        or      eax,eax
        jz      kpd_out
        dec     eax
        mov     [ky_hpos],eax
        call    ky_loadh
kpd_out:
        clc
        ret

;---------------------------------------------------------------------
; ky_loadh - EAX = a history line -> it is the line and the template,
;            shown, with the cursor at its end.  Past the newest is an
;            empty line.
;---------------------------------------------------------------------
ky_loadh:
        pushad
        call    ky_hptr
        mov     edi,OFFSET ky_buf
        mov     ecx,0
        jc      klh_set
klh_cp:
        mov     al,[esi+ecx]
        or      al,al
        jz      klh_set
        cmp     ecx,KYQ_LINE - 1
        jae     klh_set
        mov     [edi+ecx],al
        inc     ecx
        jmp     short klh_cp
klh_set:
        mov     [ky_len],ecx
        mov     [ky_disp],ecx
        mov     [ky_cur],ecx
        xor     ebx,ebx
        call    ky_draw
        popad
        ret

;--- F8: back through the history for a line that begins with what is
;--- typed so far, wrapping round once, the cursor kept where it was -
ky_f8:
        mov     edx,[ky_hcount]
        or      edx,edx
        jz      kf8_out
        mov     eax,[ky_hpos]
kf8_next:
        or      eax,eax
        jnz     kf8_dec
        mov     eax,[ky_hcount]
kf8_dec:
        dec     eax
        call    ky_hmatch
        jnc     kf8_hit
        dec     edx
        jnz     kf8_next
        jmp     short kf8_out
kf8_hit:
        mov     [ky_hpos],eax
        push    dword ptr [ky_cur]
        call    ky_loadh
        pop     ebx
        mov     [ky_cur],ebx
        call    ky_goto
kf8_out:
        clc
        ret

; ky_hmatch - CF clear if history line EAX begins with the first
;             [ky_cur] characters of the line, whatever their case.
ky_hmatch:
        pushad
        call    ky_hptr
        jc      khm_no
        xor     ecx,ecx
khm_lp:
        cmp     ecx,[ky_cur]
        jae     khm_yes
        mov     al,[esi+ecx]
        or      al,al
        jz      khm_no
        call    upcase
        mov     ah,al
        mov     al,[ky_buf+ecx]
        call    upcase
        cmp     al,ah
        jne     khm_no
        inc     ecx
        jmp     short khm_lp
khm_yes:
        popad
        clc
        ret
khm_no:
        popad
        stc
        ret

;--- F7: the history, numbered from the oldest, the current line marked
;--- with '>', a "-- More --" every screenful - and an empty line back,
;--- so the prompt comes round again underneath it ------------------
ky_f7:
        call    ky_esc
        xor     ebx,ebx
        mov     dword ptr [ky_nline],0
kf7_lp:
        cmp     ebx,[ky_hcount]
        jae     kf7_end
        mov     eax,[ky_nline]
        inc     eax
        mov     [ky_nline],eax
        mov     ecx,[ky_rows]
        dec     ecx
        cmp     eax,ecx
        jb      kf7_line
        mov     edx,OFFSET msg_kymore
        call    print
        call    ky_getkey
        mov     dword ptr [ky_nline],1
kf7_line:
        call    put_crlf
        lea     eax,[ebx+1]
        call    pdec1
        mov     dl,':'
        mov     ah,02h
        int     21h
        mov     dl,' '
        cmp     ebx,[ky_hpos]
        jne     kf7_mark
        mov     dl,'>'
kf7_mark:
        mov     ah,02h
        int     21h
        mov     eax,ebx
        call    ky_hptr
kf7_text:
        mov     al,[esi]
        or      al,al
        jz      kf7_next
        call    ky_out
        inc     esi
        jmp     short kf7_text
kf7_next:
        inc     ebx
        jmp     short kf7_lp
kf7_end:
        xor     eax,eax
        mov     [ky_disp],eax
        stc                             ; end the line, empty
        ret

;--- F9: "Line number:", and that line from the oldest ----------------
ky_f9:
        cmp     dword ptr [ky_hcount],0
        je      kf9_out
        call    ky_esc
        mov     edx,OFFSET msg_kynum
        call    print
        xor     ecx,ecx                 ; digits typed
kf9_key:
        call    ky_getkey
        or      ah,ah
        jnz     kf9_key
        cmp     al,27
        je      kf9_esc
        cmp     al,13
        je      kf9_cr
        cmp     al,8
        je      kf9_bs
        cmp     al,'0'
        jb      kf9_key
        cmp     al,'9'
        ja      kf9_key
        cmp     ecx,4                   ; f9_buf's size
        jae     kf9_key
        mov     [ky_f9buf+ecx],al
        inc     ecx
        mov     dl,al
        mov     ah,02h
        int     21h
        jmp     short kf9_key
kf9_bs:
        or      ecx,ecx
        jz      kf9_key
        dec     ecx
        mov     dl,8                    ; the console's backspace blanks
        mov     ah,02h                  ; as it goes
        int     21h
        jmp     short kf9_key
kf9_cr:
        or      ecx,ecx                 ; no digits: as Esc, DOSKEY says
        jz      kf9_esc
        xor     eax,eax
        xor     esi,esi
kf9_num:
        imul    eax,10
        movzx   edx,byte ptr [ky_f9buf+esi]
        sub     edx,'0'
        add     eax,edx
        inc     esi
        cmp     esi,ecx
        jb      kf9_num
        or      eax,eax                 ; 0 and 1 are both the oldest
        jz      kf9_clamp
        dec     eax
kf9_clamp:
        cmp     eax,[ky_hcount]
        jb      kf9_have
        mov     eax,[ky_hcount]         ; past the newest: the newest
        dec     eax
kf9_have:
        mov     [ky_hpos],eax
        call    kf9_wipe
        mov     eax,[ky_hpos]
        call    ky_loadh
kf9_out:
        clc
        ret
kf9_esc:
        call    kf9_wipe
        clc
        ret

; the prompt and the digits, blanked, and the cursor back where the
; line starts
kf9_wipe:
        pushad
        xor     ebx,ebx
        call    ky_goto
        add     ecx,KYQ_NUMLEN
kfw_sp:
        mov     dl,' '
        mov     ah,02h
        int     21h
        loop    kfw_sp
        xor     ebx,ebx
        call    ky_goto
        popad
        ret

;--- Alt+F7 and Alt+F10 ------------------------------------------------
ky_af7:
        xor     eax,eax
        mov     [ky_hused],eax
        mov     [ky_hcount],eax
        mov     [ky_hpos],eax
        clc
        ret

ky_af10:
        call    ky_mclear
        clc
        ret

;=====================================================================
; The pool: macros at the front, history behind them.
;=====================================================================

;---------------------------------------------------------------------
; ky_saveline - the line just entered is the next template, and the
;               newest line of history.
;---------------------------------------------------------------------
ky_saveline:
        pushad
        mov     ecx,[ky_disp]
        mov     [ky_lastn],ecx
        mov     esi,OFFSET ky_buf
        mov     edi,OFFSET ky_last
        rep     movsb
        mov     ecx,[ky_disp]
        mov     esi,OFFSET ky_buf
        call    ky_hadd
        popad
        ret

;---------------------------------------------------------------------
; ky_hptr - EAX = a history line -> ESI -> it.  CF if there is no such
;           line.  EAX is lost.
;---------------------------------------------------------------------
ky_hptr:
        cmp     eax,[ky_hcount]
        jae     khp_none
        push    ecx
        mov     ecx,eax
        mov     esi,[ky_pool]
        add     esi,[ky_mused]
khp_lp:
        or      ecx,ecx
        jz      khp_at
khp_skip:
        lodsb
        or      al,al
        jnz     khp_skip
        dec     ecx
        jmp     short khp_lp
khp_at:
        pop     ecx
        clc
        ret
khp_none:
        stc
        ret

;---------------------------------------------------------------------
; ky_hadd - ESI = a line, ECX = its length: it becomes the newest line
;           of history, the oldest going to make room.  An empty line
;           is not kept, and nor is one longer than the whole pool.
;---------------------------------------------------------------------
ky_hadd:
        pushad
        or      ecx,ecx
        jz      kha_out
        cmp     dword ptr [ky_pool],0
        je      kha_out
        lea     edx,[ecx+1]             ; its size, NUL and all
        mov     eax,[ky_size]
        sub     eax,[ky_mused]
        cmp     edx,eax
        ja      kha_out
kha_room:
        mov     eax,[ky_mused]
        add     eax,[ky_hused]
        add     eax,edx
        cmp     eax,[ky_size]
        jbe     kha_fits
        call    ky_hdrop
        jmp     short kha_room
kha_fits:
        mov     edi,[ky_pool]
        add     edi,[ky_mused]
        add     edi,[ky_hused]
        rep     movsb
        mov     byte ptr [edi],0
        add     [ky_hused],edx
        inc     dword ptr [ky_hcount]
kha_out:
        popad
        ret

;---------------------------------------------------------------------
; ky_hdrop - the oldest history line goes.
;---------------------------------------------------------------------
ky_hdrop:
        pushad
        cmp     dword ptr [ky_hcount],0
        je      khd_out
        mov     esi,[ky_pool]
        add     esi,[ky_mused]
        mov     edi,esi
khd_len:
        lodsb
        or      al,al
        jnz     khd_len
        mov     edx,esi
        sub     edx,edi                 ; what the first one took
        mov     ecx,[ky_hused]
        sub     ecx,edx
        rep     movsb                   ; the rest, down over it
        sub     [ky_hused],edx
        dec     dword ptr [ky_hcount]
        cmp     dword ptr [ky_hpos],0
        je      khd_out
        dec     dword ptr [ky_hpos]
khd_out:
        popad
        ret

;---------------------------------------------------------------------
; ky_mfind - ESI -> a name, ECX = its length (not terminated).  EDI ->
;            its entry, CF if there is none.  Names are kept in upper
;            case and compared without regard to it.
;---------------------------------------------------------------------
ky_mfind:
        push    eax
        push    ebx
        push    edx
        mov     edi,[ky_pool]
        or      edi,edi
        jz      kmf_none
        mov     ebx,edi
        add     ebx,[ky_mused]          ; the end of the macros
kmf_entry:
        cmp     edi,ebx
        jae     kmf_none
        xor     edx,edx
kmf_cmp:
        cmp     edx,ecx
        jae     kmf_endname
        mov     al,[esi+edx]
        call    upcase
        cmp     al,[edi+edx]
        jne     kmf_skip
        inc     edx
        jmp     short kmf_cmp
kmf_endname:
        cmp     byte ptr [edi+edx],0    ; the stored one ends here too
        je      kmf_hit
kmf_skip:
        call    ky_mnext
        jmp     short kmf_entry
kmf_hit:
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret
kmf_none:
        pop     edx
        pop     ebx
        pop     eax
        stc
        ret

; ky_mnext - EDI -> an entry -> EDI -> the one after it
ky_mnext:
        push    eax
kmn_name:
        mov     al,[edi]
        inc     edi
        or      al,al
        jnz     kmn_name
kmn_text:
        mov     al,[edi]
        inc     edi
        or      al,al
        jnz     kmn_text
        pop     eax
        ret

;---------------------------------------------------------------------
; ky_mdel - EDI -> an entry: it goes, and everything behind it in the
;           pool - later macros and the whole history - closes up.
;---------------------------------------------------------------------
ky_mdel:
        pushad
        mov     esi,edi
        call    ky_mnext
        xchg    esi,edi                 ; ESI = the next entry, EDI = this
        mov     edx,esi
        sub     edx,edi                 ; the entry's size
        mov     ecx,[ky_pool]
        add     ecx,[ky_mused]
        add     ecx,[ky_hused]
        sub     ecx,esi                 ; what lies behind it
        rep     movsb
        sub     [ky_mused],edx
        popad
        ret

;---------------------------------------------------------------------
; ky_mclear - every macro goes; the history moves to the front.
;---------------------------------------------------------------------
ky_mclear:
        pushad
        mov     edi,[ky_pool]
        or      edi,edi
        jz      kmc_out
        mov     esi,edi
        add     esi,[ky_mused]
        mov     ecx,[ky_hused]
        rep     movsb
        mov     dword ptr [ky_mused],0
kmc_out:
        popad
        ret

;---------------------------------------------------------------------
; ky_madd - define a macro: ESI -> the name, ECX = its length, and the
;           packed text is ASCIIZ in ky_work with its length in EDX.
;           No text deletes the macro.  CF: no room for it, even with
;           the whole history given up.
;---------------------------------------------------------------------
ky_madd:
        pushad
        call    ky_mfind
        jc      kma_new
        call    ky_mdel
kma_new:
        or      edx,edx
        jz      kma_ok
        cmp     dword ptr [ky_pool],0
        je      kma_full
        lea     ebx,[ecx+edx+2]         ; name, NUL, text, NUL
        cmp     ebx,[ky_size]
        ja      kma_full
kma_room:
        mov     eax,[ky_mused]
        add     eax,[ky_hused]
        add     eax,ebx
        cmp     eax,[ky_size]
        jbe     kma_fits
        cmp     dword ptr [ky_hcount],0
        je      kma_full
        call    ky_hdrop                ; history makes way, oldest first
        jmp     short kma_room
kma_fits:
        ; the history moves up by the entry's size - from the top down,
        ; since the two ranges overlap
        push    esi
        push    ecx
        mov     ecx,[ky_hused]
        mov     esi,[ky_pool]
        add     esi,[ky_mused]
        add     esi,ecx
        dec     esi                     ; its last byte
        lea     edi,[esi+ebx]
        std
        rep     movsb
        cld
        pop     ecx
        pop     esi
        mov     edi,[ky_pool]
        add     edi,[ky_mused]
kma_name:
        lodsb                           ; the name, upper case
        call    upcase
        stosb
        loop    kma_name
        xor     al,al
        stosb
        mov     esi,OFFSET ky_work
        mov     ecx,edx
        rep     movsb
        xor     al,al
        stosb
        add     [ky_mused],ebx
kma_ok:
        popad
        clc
        ret
kma_full:
        popad
        stc
        ret

;=====================================================================
; ky_expand - ESI -> a line typed at the prompt.  CF clear: nothing to
;             do, run it as it is.  CF set: it has been turned into
;             one or more commands, waiting in ky_pend for ky_pending.
;
; Two things make a line more than one command: a Ctrl+T typed in it,
; which DOSKEY treats as a separator, and a first word that names a
; macro.  The line is taken apart at its separators; each piece whose
; first word is a macro becomes that macro's text - itself possibly
; several commands, $T being a Ctrl+T - with $1-$9 and $* filled in
; from the rest of the piece.  A piece that STARTS with a blank is
; never a macro, which is DOSKEY's way of letting a macro called DIR
; stand in front of the real DIR.
;=====================================================================
ky_expand:
        pushad
        mov     [ky_src],esi
        ; anything to do at all?
        mov     edi,esi
kex_sep:
        mov     al,[edi]
        or      al,al
        jz      kex_nosep
        cmp     al,KYQ_SEP
        je      kex_go
        inc     edi
        jmp     short kex_sep
kex_nosep:
        call    ky_namelen              ; ECX = the first word's length
        or      ecx,ecx
        jz      kex_plain
        call    ky_mfind
        jc      kex_plain
kex_go:
        mov     edi,OFFSET ky_pend
        mov     [ky_outp],edi
        mov     esi,[ky_src]
kex_piece:
        mov     [ky_src],esi            ; this piece starts here
        call    ky_namelen
        or      ecx,ecx
        jz      kex_lit
        call    ky_mfind
        jc      kex_lit
        ; a macro: its text, with the rest of the piece as arguments
        add     esi,ecx
        mov     [ky_args],esi
        call    ky_mnexthalf            ; EDI -> the macro's text
        call    ky_domacro
        jmp     short kex_skip
kex_lit:
        mov     esi,[ky_src]            ; not a macro: the piece as it is
        call    ky_pstart
kex_litcp:
        lodsb
        or      al,al
        jz      kex_litend
        cmp     al,KYQ_SEP
        je      kex_litend
        call    ky_pput
        jmp     short kex_litcp
kex_litend:
        call    ky_pend1
        dec     esi                     ; back on the separator or NUL
        jmp     short kex_next
kex_skip:
        mov     esi,[ky_src]            ; past the piece the macro used
kex_skp:
        mov     al,[esi]
        or      al,al
        jz      kex_next
        cmp     al,KYQ_SEP
        je      kex_next
        inc     esi
        jmp     short kex_skp
kex_next:
        cmp     byte ptr [esi],KYQ_SEP
        jne     kex_done
        inc     esi
        jmp     short kex_piece
kex_done:
        mov     edi,[ky_outp]
        mov     byte ptr [edi],0        ; the list ends with an empty one
        mov     dword ptr [ky_pendp],0
        cmp     edi,OFFSET ky_pend
        je      kex_none                ; nothing but separators
        mov     dword ptr [ky_pendp],OFFSET ky_pend
kex_none:
        popad
        stc
        ret
kex_plain:
        popad
        clc
        ret

; ky_namelen - ESI -> a piece.  ECX = how long its first word is, which
;              is zero when it starts with a blank.  A name ends at a
;              blank, a tab, an '=', a separator or the end.
ky_namelen:
        push    eax
        xor     ecx,ecx
kyn_lp:
        mov     al,[esi+ecx]
        or      al,al
        jz      kyn_out
        cmp     al,' '
        je      kyn_out
        cmp     al,9
        je      kyn_out
        cmp     al,'='
        je      kyn_out
        cmp     al,KYQ_SEP
        je      kyn_out
        inc     ecx
        jmp     short kyn_lp
kyn_out:
        pop     eax
        ret

; ky_mnexthalf - EDI -> an entry -> EDI -> its text
ky_mnexthalf:
        push    eax
kmh_lp:
        mov     al,[edi]
        inc     edi
        or      al,al
        jnz     kmh_lp
        pop     eax
        ret

;---------------------------------------------------------------------
; ky_domacro - the macro text at EDI, expanded into the pending list:
;              $1-$9 and $* from [ky_args] (which runs to a separator
;              or the end), $$ a dollar, and each Ctrl+T in it the end
;              of one command and the start of the next.
;---------------------------------------------------------------------
ky_domacro:
        pushad
        mov     esi,edi
        call    ky_pstart
kdm_lp:
        lodsb
        or      al,al
        jz      kdm_end
        cmp     al,KYQ_SEP
        je      kdm_sep
        cmp     al,'$'
        jne     kdm_put
        lodsb
        or      al,al
        jz      kdm_end
        cmp     al,'*'
        je      kdm_all
        cmp     al,'1'
        jb      kdm_put                 ; "$$" is a dollar
        cmp     al,'9'
        ja      kdm_put
        sub     al,'0'
        movzx   ecx,al
        call    ky_argn
        jmp     short kdm_lp
kdm_all:
        call    ky_argall
        jmp     short kdm_lp
kdm_put:
        call    ky_pput
        jmp     short kdm_lp
kdm_sep:
        call    ky_pend1
        call    ky_pstart
        jmp     short kdm_lp
kdm_end:
        call    ky_pend1
        popad
        ret

; ky_argn - ECX = n: the nth blank-separated word of [ky_args] out
ky_argn:
        pushad
        mov     esi,[ky_args]
kan_word:
        mov     al,[esi]                ; the blanks in front of it
        or      al,al
        jz      kan_out
        cmp     al,KYQ_SEP
        je      kan_out
        cmp     al,' '
        je      kan_blank
        cmp     al,9
        jne     kan_start
kan_blank:
        inc     esi
        jmp     short kan_word
kan_start:
        dec     ecx
        jz      kan_copy
kan_over:                               ; not this one: past it
        mov     al,[esi]
        or      al,al
        jz      kan_out
        cmp     al,KYQ_SEP
        je      kan_out
        cmp     al,' '
        je      kan_word
        cmp     al,9
        je      kan_word
        inc     esi
        jmp     short kan_over
kan_copy:
        mov     al,[esi]
        or      al,al
        jz      kan_out
        cmp     al,KYQ_SEP
        je      kan_out
        cmp     al,' '
        je      kan_out
        cmp     al,9
        je      kan_out
        call    ky_pput
        inc     esi
        jmp     short kan_copy
kan_out:
        popad
        ret

; ky_argall - $*: everything after the name, leading blanks dropped
ky_argall:
        pushad
        mov     esi,[ky_args]
kaa_skip:
        mov     al,[esi]
        cmp     al,' '
        je      kaa_step
        cmp     al,9
        jne     kaa_copy
kaa_step:
        inc     esi
        jmp     short kaa_skip
kaa_copy:
        mov     al,[esi]
        or      al,al
        jz      kaa_out
        cmp     al,KYQ_SEP
        je      kaa_out
        call    ky_pput
        inc     esi
        jmp     short kaa_copy
kaa_out:
        popad
        ret

;--- the pending list is built a command at a time ---------------------
; ky_pstart - a new command begins at [ky_outp]
ky_pstart:
        push    eax
        mov     eax,[ky_outp]
        mov     [ky_cmdp],eax
        pop     eax
        ret

; ky_pput - AL onto the command being built, while there is room
ky_pput:
        push    edi
        mov     edi,[ky_outp]
        cmp     edi,OFFSET ky_pend + KYQ_PEND - 2
        jae     kpp_out
        mov     [edi],al
        inc     edi
        mov     [ky_outp],edi
kpp_out:
        pop     edi
        ret

; ky_pend1 - the command being built is finished.  An empty one is not
;            kept: an empty string is what ends the list.
ky_pend1:
        push    eax
        push    edi
        mov     edi,[ky_outp]
        cmp     edi,[ky_cmdp]
        je      kpe_out
        mov     byte ptr [edi],0
        inc     edi
        mov     [ky_outp],edi
kpe_out:
        pop     edi
        pop     eax
        ret

;=====================================================================
; DOSKEY [/REINSTALL] [/BUFSIZE=size] [/MACROS] [/HISTORY]
;        [/INSERT | /OVERSTRIKE] [macroname=[text]]
;
; In the order DOSKEY does them, whatever order they are typed in:
; /REINSTALL, /BUFSIZE, the modes, the macro, then /HISTORY and then
; /MACROS.  A macro definition takes the whole of the rest of the line,
; which is why the switches have to come first.
;=====================================================================
cmd_doskey:
        mov     byte ptr [ky_opts],0
        mov     dword ptr [ky_newsize],0
        mov     dword ptr [ky_defp],0
        mov     esi,[arg_ptr]
kdo_next:
        call    skip_blanks
        mov     al,[esi]
        or      al,al
        jz      kdo_run
        cmp     al,'/'
        je      kdo_switch
        mov     [ky_defp],esi           ; a macro: the rest of the line
        jmp     short kdo_run
kdo_switch:
        inc     esi
        mov     edi,OFFSET ky_swtab
kdo_try:
        cmp     byte ptr [edi],0
        je      kdo_badsw
        call    ky_swmatch              ; ESI past it, CF if not this one
        pushfd
kdo_toname:
        cmp     byte ptr [edi],0        ; to the end of this entry's name,
        je      kdo_atflag              ; where its flag is
        inc     edi
        jmp     short kdo_toname
kdo_atflag:
        popfd
        jnc     kdo_hit
        add     edi,3                   ; NUL, flag, spare: the next entry
        jmp     short kdo_try
kdo_hit:
        mov     al,[edi+1]              ; the flag behind the name's NUL
        or      [ky_opts],al
        test    al,KYO_BUFSIZE
        jz      kdo_next
        cmp     byte ptr [esi],'='      ; /BUFSIZE=n
        jne     kdo_badsw
        inc     esi
        call    parse_num
        jc      kdo_badsw
        mov     [ky_newsize],eax
        jmp     short kdo_next
kdo_badsw:
        mov     edx,OFFSET msg_kysw
        call    print
        ret

kdo_run:
        test    byte ptr [ky_opts],KYO_REINST
        jz      kdo_nore
        call    ky_mclear               ; a fresh DOSKEY: nothing kept
        call    ky_af7
kdo_nore:
        cmp     dword ptr [ky_newsize],0
        je      kdo_nosize
        call    ky_resize
        jc      kdo_nomem
kdo_nosize:
        test    byte ptr [ky_opts],KYO_INSERT
        jz      kdo_noins
        mov     byte ptr [ky_insdef],1
kdo_noins:
        test    byte ptr [ky_opts],KYO_OVER
        jz      kdo_noover
        mov     byte ptr [ky_insdef],0
kdo_noover:
        cmp     dword ptr [ky_defp],0
        je      kdo_nodef
        call    ky_define
        jc      kdo_out
kdo_nodef:
        test    byte ptr [ky_opts],KYO_HIST
        jz      kdo_nohist
        call    ky_listhist
kdo_nohist:
        test    byte ptr [ky_opts],KYO_MACS
        jz      kdo_out
        call    ky_listmacs
kdo_out:
        ret
kdo_nomem:
        mov     edx,OFFSET msg_kysize
        call    print
        ret

; ky_swmatch - is the switch word at ESI the whole of the name at EDI,
;              and nothing more?  CF if not; otherwise ESI is past it.
;              EDI is left where it was.
ky_swmatch:
        push    ecx
        push    edx
        xor     ecx,ecx
ksm_lp:
        mov     al,[esi+ecx]
        call    upcase
        mov     dl,[edi+ecx]
        or      dl,dl
        jz      ksm_end
        cmp     al,dl
        jne     ksm_no
        inc     ecx
        jmp     short ksm_lp
ksm_end:
        call    ky_swend                ; the word must stop here
        jc      ksm_no
        add     esi,ecx
        pop     edx
        pop     ecx
        clc
        ret
ksm_no:
        pop     edx
        pop     ecx
        stc
        ret

; ky_swend - CF unless the character at [ESI+ECX] ends a switch word
ky_swend:
        mov     al,[esi+ecx]
        or      al,al
        jz      kse_yes
        cmp     al,' '
        je      kse_yes
        cmp     al,9
        je      kse_yes
        cmp     al,'/'
        je      kse_yes
        cmp     al,'='
        je      kse_yes
        stc
        ret
kse_yes:
        clc
        ret

; Each name, NUL, its flag, and a spare byte.  /H and /M are DOSKEY's
; own short forms (trans.asm's dh1_string and dm1_string), so they are
; names of their own and not an abbreviation rule.
ky_swtab:
        db      'REINSTALL',0,KYO_REINST,0
        db      'BUFSIZE',0,KYO_BUFSIZE,0
        db      'HISTORY',0,KYO_HIST,0
        db      'H',0,KYO_HIST,0
        db      'MACROS',0,KYO_MACS,0
        db      'M',0,KYO_MACS,0
        db      'INSERT',0,KYO_INSERT,0
        db      'OVERSTRIKE',0,KYO_OVER,0
        db      0

;---------------------------------------------------------------------
; ky_define - the macro definition at [ky_defp]: "name=text", blanks
;             allowed round the '='.  CF if it was refused (and said so).
;---------------------------------------------------------------------
ky_define:
        mov     esi,[ky_defp]
        call    ky_namelen
        or      ecx,ecx
        jz      kdf_bad
        mov     [ky_dname],esi
        mov     [ky_dnlen],ecx
        add     esi,ecx
        call    skip_blanks
        cmp     byte ptr [esi],'='
        jne     kdf_bad
        inc     esi
        call    skip_blanks
        call    ky_pack                 ; -> ky_work, EDX = its length
        mov     esi,[ky_dname]
        mov     ecx,[ky_dnlen]
        call    ky_madd
        jc      kdf_full
        clc
        ret
kdf_bad:
        mov     edx,OFFSET msg_kydef
        call    print
        stc
        ret
kdf_full:
        mov     edx,OFFSET msg_kymfull
        call    print
        stc
        ret

;---------------------------------------------------------------------
; ky_pack - the macro text at ESI into ky_work, as trans.asm packs it:
;           $G $L $B $T become > < | and Ctrl+T now, $$ $* and $1-$9
;           are kept for when the macro is used, and any other $x is
;           thrown away.  EDX = the length.
;---------------------------------------------------------------------
ky_pack:
        push    eax
        push    edi
        mov     edi,OFFSET ky_work
        xor     edx,edx
kpk_lp:
        lodsb
        or      al,al
        jz      kpk_end
        cmp     al,'$'
        jne     kpk_put
        lodsb
        or      al,al
        jz      kpk_end
        cmp     al,'$'
        je      kpk_keep
        cmp     al,'*'
        je      kpk_keep
        cmp     al,'1'
        jb      kpk_code
        cmp     al,'9'
        jbe     kpk_keep
kpk_code:
        call    upcase
        mov     ah,'|'
        cmp     al,'B'
        je      kpk_sub
        mov     ah,'<'
        cmp     al,'L'
        je      kpk_sub
        mov     ah,'>'
        cmp     al,'G'
        je      kpk_sub
        mov     ah,KYQ_SEP
        cmp     al,'T'
        je      kpk_sub
        jmp     short kpk_lp            ; $<anything else>: dropped
kpk_sub:
        mov     al,ah
        jmp     short kpk_put
kpk_keep:
        push    eax
        mov     al,'$'
        call    kpk_store
        pop     eax
kpk_put:
        call    kpk_store
        jmp     short kpk_lp
kpk_end:
        mov     byte ptr [edi],0
        pop     edi
        pop     eax
        ret
kpk_store:
        cmp     edx,KYQ_PEND - 2
        jae     kps_out
        mov     [edi],al
        inc     edi
        inc     edx
kps_out:
        ret

;---------------------------------------------------------------------
; ky_listhist / ky_listmacs - /HISTORY and /MACROS.  The macros come
; out as they would have to be typed: dumpmacs turns > < | and Ctrl+T
; back into $g $l $b and $t on the way.
;---------------------------------------------------------------------
ky_listhist:
        pushad
        xor     ebx,ebx
klh2_lp:
        cmp     ebx,[ky_hcount]
        jae     klh2_out
        mov     eax,ebx
        call    ky_hptr
        call    print_asciiz
        call    put_crlf
        inc     ebx
        jmp     short klh2_lp
klh2_out:
        popad
        ret

ky_listmacs:
        pushad
        mov     esi,[ky_pool]
        or      esi,esi
        jz      klm_out
        mov     ebx,esi
        add     ebx,[ky_mused]
klm_entry:
        cmp     esi,ebx
        jae     klm_out
        call    print_asciiz            ; the name
klm_name:
        lodsb
        or      al,al
        jnz     klm_name
        mov     dl,'='
        mov     ah,02h
        int     21h
klm_text:
        lodsb
        or      al,al
        jz      klm_eol
        mov     ah,'b'
        cmp     al,'|'
        je      klm_code
        mov     ah,'g'
        cmp     al,'>'
        je      klm_code
        mov     ah,'l'
        cmp     al,'<'
        je      klm_code
        mov     ah,'t'
        cmp     al,KYQ_SEP
        je      klm_code
        cmp     al,'$'                  ; "$x" as stored: shown as it is
        jne     klm_char
        mov     dl,al
        mov     ah,02h
        int     21h
        lodsb
        jmp     short klm_char
klm_code:
        push    eax
        mov     dl,'$'
        mov     ah,02h
        int     21h
        pop     eax
        mov     al,ah
klm_char:
        mov     dl,al
        mov     ah,02h
        int     21h
        jmp     short klm_text
klm_eol:
        call    put_crlf
        jmp     short klm_entry
klm_out:
        popad
        ret

;---------------------------------------------------------------------
; ky_resize - /BUFSIZE=[ky_newsize]: a new pool, the macros and as much
;             of the newest history as fits carried across.  CF if the
;             memory is not there or the macros alone would not fit, in
;             which case nothing has changed.
;
; DOSKEY could not do this once it was resident - "Cannot change
; BUFSIZE" - because a TSR cannot move itself.  A pool inside the shell
; is just a block, and a block can be replaced.
;---------------------------------------------------------------------
ky_resize:
        pushad
        mov     ebx,[ky_newsize]
        cmp     ebx,KYQ_MINSIZE
        jae     krs_lo
        mov     ebx,KYQ_MINSIZE
krs_lo:
        cmp     ebx,KYQ_MAXSIZE
        jbe     krs_hi
        mov     ebx,KYQ_MAXSIZE
krs_hi:
        cmp     ebx,[ky_mused]
        jb      krs_fail
        mov     [ky_newsize],ebx
        mov     ah,48h
        int     21h
        jc      krs_fail
        mov     edi,eax                 ; the new block
        mov     [ky_tmp],eax
        ; the oldest history goes until macros and history fit
krs_fit:
        mov     eax,[ky_mused]
        add     eax,[ky_hused]
        cmp     eax,[ky_newsize]
        jbe     krs_copy
        call    ky_hdrop
        jmp     short krs_fit
krs_copy:
        mov     esi,[ky_pool]
        mov     ecx,[ky_mused]
        add     ecx,[ky_hused]
        or      esi,esi
        jz      krs_nold
        rep     movsb
        mov     edx,[ky_pool]
        mov     ah,49h
        int     21h
krs_nold:
        mov     eax,[ky_tmp]
        mov     [ky_pool],eax
        mov     eax,[ky_newsize]
        mov     [ky_size],eax
        popad
        clc
        ret
krs_fail:
        popad
        stc
        ret

;=====================================================================
; The editor's messages and state.
;=====================================================================
msg_kybrk   db  '^C',13,10,'$'
msg_kymore  db  '-- More --','$'
msg_kynum   db  'Line number: ','$'     ; KYQ_NUMLEN characters
msg_kysw    db  'Invalid switch',13,10,'$'
msg_kydef   db  'Invalid macro definition',13,10,'$'
msg_kymfull db  'Insufficient memory to store macro.  Use the DOSKEY command with',13,10
            db  'the /BUFSIZE switch to increase available memory.',13,10,'$'
msg_kysize  db  'Insufficient memory for that buffer size',13,10,'$'
ky_testvar  db  'KYTEST',0              ; see ky_readline

ky_pool     dd  0                       ; macros, then history
ky_size     dd  0                       ; ...its size, /BUFSIZE
ky_mused    dd  0                       ; bytes of macros at the front
ky_hused    dd  0                       ; bytes of history behind them
ky_hcount   dd  0                       ; lines of history
ky_hpos     dd  0                       ; the one Up and Down are on
ky_len      dd  0                       ; the template's length
ky_disp     dd  0                       ; ...how much of it is the line
ky_cur      dd  0                       ; the cursor, as an index
ky_row0     dd  0                       ; where the line starts on screen
ky_col0     dd  0
ky_cols     dd  80
ky_rows     dd  25
ky_shown    dd  0                       ; the column the line ends at now
ky_newend   dd  0
ky_lastn    dd  0                       ; the previous line's length
ky_tmp      dd  0
ky_nline    dd  0                       ; F7's lines since a "More"
ky_pendp    dd  0                       ; next waiting command, 0 = none
ky_src      dd  0                       ; ky_expand's working pointers
ky_args     dd  0
ky_outp     dd  0
ky_cmdp     dd  0
ky_defp     dd  0                       ; DOSKEY: the macro definition
ky_dname    dd  0
ky_dnlen    dd  0
ky_newsize  dd  0
ky_insdef   db  0                       ; /INSERT: insert unless Ins says not
ky_ins      db  0                       ; ...and this line's mode
ky_opts     db  0                       ; DOSKEY's switches, KYO_*
            db  0
ky_f9buf    db  4 dup(0)
ky_buf      db  KYQ_LINE + 4 dup(0)     ; the line and the template
ky_last     db  KYQ_LINE + 4 dup(0)     ; the previous line
ky_pend     db  KYQ_PEND dup(0)         ; commands a macro made, waiting
ky_work     db  KYQ_PEND dup(0)         ; a macro's text, being packed
