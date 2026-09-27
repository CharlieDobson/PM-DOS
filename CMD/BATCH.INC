;=====================================================================
; BATCH.INC - batch file (.BAT) interpretation for PM-DOS COMMAND.COM
;
; Everything MS-DOS 6.22 does in a batch file:
;
;   @               suppress the echo of one line
;   ECHO ON/OFF     and ECHO text / ECHO. (in COMMAND.ASM)
;   REM             comment
;   PAUSE           wait for a key
;   :label / GOTO   jump about the file
;   IF              [NOT] ERRORLEVEL n / EXIST file / str1==str2
;   FOR %v IN (set) DO command      (wildcards in the set expand)
;   SHIFT           slide %1..%9 down onto %0..%8
;   CALL            run a batch file and come back
;   CTTY            console redirection (see the note on cmd_ctty)
;   %0-%9, %VAR%    argument and environment substitution
;   AUTOEXEC.BAT    run at startup by the permanent (/P) shell
;
; The parameters of a batch file live in a frame; CALL stacks frames,
; which is the whole difference between CALL and plain chaining.
;
; Included by COMMAND.ASM.
;=====================================================================

BAT_MAX         EQU     8               ; how deep CALL may nest
BAT_ATEXT       EQU     144             ; bytes of %0..%9 text per frame
BAT_LINE        EQU     160             ; longest line we will run
BAT_CHUNK       EQU     256             ; read granularity
TOK_MAX         EQU     144
GOTOLBL_MAX     EQU     32
FORLIST_MAX     EQU     160
FORBODY_MAX     EQU     160
FORLINE_MAX     EQU     200
FORNAMES_MAX    EQU     1024

; Frame layout.
;
; Every argument is kept, not just ten, and SHIFT moves a window over
; them rather than sliding the array down - which is how MS-DOS lets
; SHIFT reach an eleventh argument that %9 could never name.
BF_ARGC         EQU     32              ; arguments remembered per frame
BF_PATHMAX      EQU     128             ; the file's name, fully qualified
BF_HANDLE       EQU     0               ; dd - unused: see bat_rawline
BF_POS          EQU     4               ; dd - offset of the NEXT line
BF_BASE         EQU     8               ; dd - which argument %0 is now
BF_ARGV         EQU     12              ; BF_ARGC dd, 0 = absent
BF_TEXT         EQU     BF_ARGV + BF_ARGC * 4
BF_CMD          EQU     BF_TEXT + BAT_ATEXT     ; dd - 1 = a .CMD file
BF_PATH         EQU     BF_CMD + 4              ; BF_PATHMAX bytes
BF_LEN          EQU     BF_PATH + BF_PATHMAX

;=====================================================================
; cmd_startup - read our own command tail and act on it.
;
; The kernel starts us with "/P", the same marker CONFIG.SYS's SHELL=
; line used, and it means two things: run AUTOEXEC.BAT, and refuse to
; EXIT because there is nothing underneath us to go back to.
;=====================================================================
cmd_startup:
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
cs_lp:
        or      ecx,ecx
        jz      cs_done
        cmp     byte ptr [esi],'/'
        jne     cs_next
        mov     al,[esi+1]
        call    upcase
        cmp     al,'?'                  ; the interpreter is a command
        je      cs_help                 ; too, and answers like one
        cmp     al,'C'
        je      cs_once
        cmp     al,'P'
        jne     cs_next
        mov     byte ptr [perm_flag],1
cs_next:
        inc     esi
        dec     ecx
        jmp     short cs_lp
cs_done:
        cmp     byte ptr [perm_flag],0
        je      cs_out

        ; AUTOEXEC.BAT off the root of the drive we booted from.  Its
        ; absence is not an error - it never was.
        mov     esi,OFFSET null_str
        mov     [arg_ptr],esi
        mov     edx,OFFSET autoexec_name
        call    bat_launch
cs_out:
        ret

; COMMAND /C string - run the string as a command line, and when it is
; over, exit with its errorlevel.  It is how a program shells out to
; run one command, and how FOR /F runs the one in its set.
;
; EVERYTHING AFTER THE /C IS THE COMMAND'S.  The scan stops here, or
; "COMMAND /C DIR /P" would find the /P and take it for the shell's
; own, and never give the prompt back.  AUTOEXEC.BAT is not run - DOS
; does not run it for /C either.
cs_once:
        add     esi,2                   ; past the "/C"
        sub     ecx,2
        jbe     cs_out                  ; nothing after it
        mov     edi,OFFSET c_line
        xor     edx,edx
co_cp:
        mov     al,[esi]
        cmp     al,13                   ; the tail ends at its CR
        je      co_end
        cmp     edx,CMDLINE_MAX
        jae     co_end
        mov     [edi+edx],al
        inc     edx
        inc     esi
        dec     ecx
        jnz     co_cp
co_end:
        mov     byte ptr [edi+edx],0
        mov     byte ptr [c_once],1
        ret

; TERMINATE, do not return: a shell that printed its own help and then
; went on to put up a prompt would have started the nested interpreter
; the user was asking about instead of being given.
cs_help:
        mov     esi,OFFSET hlp_command
        call    print_asciiz
        mov     ax,4C00h
        int     21h

;=====================================================================
; bat_launch - EDX -> the path of a .BAT file, [arg_ptr] -> its
;              arguments.  CF set if the file will not open.
;
; Running a batch file from inside another REPLACES it: control never
; comes back.  That is what chaining meant, and precisely why CALL had
; to be invented - so [call_flag] decides which of the two happens.
;=====================================================================
bat_launch:
        pushad
        mov     [bl_path],edx

        mov     ax,3D00h
        int     21h
        jc      bl_fail
        movzx   eax,ax
        mov     [bl_handle],eax

        ; chain unless this is a CALL: the running file is finished
        cmp     byte ptr [call_flag],0
        jne     bl_nochain
        cmp     dword ptr [bat_cur],0
        je      bl_nochain
        call    bat_pop
bl_nochain:

        cmp     dword ptr [bat_lvl],BAT_MAX
        jae     bl_deep

        mov     edi,[bat_lvl]
        imul    edi,BF_LEN
        add     edi,OFFSET bat_stk
        mov     dword ptr [edi+BF_POS],0

        ; THE FILE IS NOT KEPT OPEN.  Its fully qualified name goes in
        ; the frame and the handle is closed now: bat_rawline opens it
        ; again for every line, which is what DOS's COMMAND.COM has
        ; always done.  Holding it open for the length of the batch
        ; file meant a file WAS open while every command in it ran -
        ; and the handover that starts Windows refuses while one is, so
        ; WIN in AUTOEXEC.BAT could never start it.  The qualified name
        ; is what a CD inside the file cannot take away from us.
        push    esi
        push    edi
        mov     esi,[bl_path]
        add     edi,BF_PATH
        mov     ah,60h                  ; TRUENAME
        int     21h
        pop     edi
        pop     esi
        jnc     bl_named2
        push    esi                     ; no answer: the name as given,
        push    edi                     ; which is right unless the file
        mov     esi,[bl_path]           ; changes directory
        add     edi,BF_PATH
        mov     ecx,BF_PATHMAX - 1
bl_pcp:
        mov     al,[esi]
        mov     [edi],al
        or      al,al
        jz      bl_pcpd
        inc     esi
        inc     edi
        loop    bl_pcp
        mov     byte ptr [edi],0
bl_pcpd:
        pop     edi
        pop     esi
bl_named2:
        push    ebx
        mov     ebx,[bl_handle]
        mov     ah,3Eh
        int     21h
        pop     ebx

        ; which batch class is this?  A .CMD file gets the advanced
        ; (quote-aware) parsing on every line it runs; a .BAT keeps
        ; the strict 8.3 parsing real DOS had - decided up front, so
        ; a batch file carried over from a real install cannot change
        ; meaning here.
        push    esi
        mov     esi,[bl_path]
        push    edx
        mov     edx,OFFSET lit_cmd
        call    ends_with
        pop     edx
        pop     esi
        mov     dword ptr [edi+BF_CMD],0
        jc      bl_notcmd
        mov     dword ptr [edi+BF_CMD],1
bl_notcmd:
        inc     dword ptr [bat_lvl]
        mov     [bat_cur],edi

        ; %0 is the name the user typed, which is not always the path
        ; we had to build to find the file
        mov     esi,[bl_name0]
        or      esi,esi
        jnz     bl_named
        mov     esi,[bl_path]
bl_named:
        call    bat_setargs
        mov     dword ptr [bl_name0],0
        popad
        clc
        ret

bl_deep:
        mov     ebx,[bl_handle]
        mov     ah,3Eh
        int     21h
        mov     edx,OFFSET msg_batdeep
        call    print
        mov     dword ptr [bl_name0],0
        popad
        clc                             ; reported; not "no such command"
        ret
bl_fail:
        ; the common case: this was not a batch file at all, and the
        ; caller is about to say "bad command".  Leave nothing behind.
        mov     dword ptr [bl_name0],0
        popad
        stc
        ret

;---------------------------------------------------------------------
; bat_setargs - ESI -> the text for %0; [arg_ptr] supplies %1..%9.
;               Fills the frame [bat_cur] points at.
;---------------------------------------------------------------------
bat_setargs:
        pushad
        mov     edi,[bat_cur]

        mov     dword ptr [edi+BF_BASE],0
        push    edi                     ; clear the pointer array
        lea     edi,[edi+BF_ARGV]
        xor     eax,eax
        mov     ecx,BF_ARGC
        rep     stosd
        pop     edi

        lea     ebx,[edi+BF_TEXT]       ; EBX = write cursor
        mov     eax,ebx
        add     eax,BAT_ATEXT - 1
        mov     [bsa_lim],eax

        mov     [edi+BF_ARGV],ebx       ; %0
bsa_zero:
        mov     al,[esi]
        call    bsa_put
        inc     esi
        or      al,al
        jnz     bsa_zero

        mov     esi,[arg_ptr]
        mov     edx,1
bsa_next:
        call    bat_sep
        cmp     byte ptr [esi],0
        je      bsa_done
        cmp     edx,BF_ARGC
        jae     bsa_done
        mov     [edi+BF_ARGV+edx*4],ebx
        xor     ah,ah                   ; AH = inside a quoted span
bsa_cp:
        mov     al,[esi]
        or      al,al
        jz      bsa_endtok
        cmp     dword ptr [edi+BF_CMD],0        ; only a .CMD's
        je      bsa_noq                         ; arguments group by
        cmp     al,'"'                          ; quotes - and the
        jne     bsa_noq                         ; quotes are KEPT, so
        xor     ah,1                            ; %1 re-parses right
        jmp     short bsa_keep                  ; wherever it lands
bsa_noq:
        or      ah,ah
        jnz     bsa_keep                ; inside quotes nothing splits
        call    bat_issep
        jnc     bsa_endtok
bsa_keep:
        call    bsa_put
        inc     esi
        jmp     short bsa_cp
bsa_endtok:
        xor     al,al
        call    bsa_put
        inc     edx
        jmp     short bsa_next
bsa_done:
        popad
        ret

; bsa_put - store AL at EBX if there is room (arguments past the end
;           of the text area simply come out empty, never corrupt)
bsa_put:
        cmp     ebx,[bsa_lim]
        jae     bsp_out
        mov     [ebx],al
        inc     ebx
bsp_out:
        ret

;---------------------------------------------------------------------
; bat_issep  - CF clear if AL separates batch arguments
; bat_sep    - step ESI past any run of separators
;---------------------------------------------------------------------
bat_issep:
        cmp     al,' '
        je      bis_yes
        cmp     al,9
        je      bis_yes
        cmp     al,','
        je      bis_yes
        cmp     al,';'
        je      bis_yes
        stc
        ret
bis_yes:
        clc
        ret

bat_sep:
        push    eax
bsx_lp:
        mov     al,[esi]
        or      al,al
        jz      bsx_out
        call    bat_issep
        jc      bsx_out
        inc     esi
        jmp     short bsx_lp
bsx_out:
        pop     eax
        ret

;---------------------------------------------------------------------
; bat_pop - close the current batch file and fall back to whatever
;           CALLed it, or to the keyboard.
;---------------------------------------------------------------------
bat_pop:
        pushad
        mov     esi,[bat_cur]
        or      esi,esi
        jz      bpop_out
        mov     eax,[bat_lvl]           ; nothing to close: see bat_launch
        dec     eax
        mov     [bat_lvl],eax
        or      eax,eax
        jz      bpop_none
        dec     eax
        imul    eax,BF_LEN
        add     eax,OFFSET bat_stk
        mov     [bat_cur],eax
        jmp     short bpop_out
bpop_none:
        mov     dword ptr [bat_cur],0
bpop_out:
        popad
        ret

;---------------------------------------------------------------------
; bat_popall - abandon every level (an error deep in a CALL chain
;              should not drop back into the caller as if all was
;              well)
;---------------------------------------------------------------------
bat_popall:
        cmp     dword ptr [bat_cur],0
        je      bpa_out
        call    bat_pop
        jmp     short bat_popall
bpa_out:
        ret

;=====================================================================
; bat_rawline - the next raw line of the current batch file into
;               line_buf, BF_POS advanced past it.  CF set at EOF.
;
; Nothing is cached between calls: the file position in the frame IS
; the interpreter's program counter, so GOTO is just a store to it.
;=====================================================================
bat_rawline:
        pushad
        mov     esi,[bat_cur]
        lea     edx,[esi+BF_PATH]       ; opened for this line and closed
        mov     ax,3D00h                ; again before it runs - see
        int     21h                     ; bat_launch
        jc      brl_gone
        movzx   ebx,ax
        mov     [brl_hdl],ebx
        mov     ecx,[esi+BF_POS]
        mov     ax,4200h
        int     21h
        jc      brl_closeof

        mov     ebx,[brl_hdl]
        mov     ecx,BAT_CHUNK
        mov     edx,OFFSET bat_raw
        mov     ah,3Fh
        int     21h
        pushfd
        push    eax
        mov     ebx,[brl_hdl]
        mov     ah,3Eh
        int     21h
        pop     eax
        popfd
        jc      brl_eof
        or      eax,eax
        jz      brl_eof

        mov     ecx,eax                 ; bytes in hand
        mov     esi,OFFSET bat_raw
        mov     edi,OFFSET line_buf
        xor     edx,edx                 ; bytes consumed
brl_scan:
        or      ecx,ecx
        jz      brl_lineend
        mov     al,[esi]
        cmp     al,1Ah                  ; Ctrl-Z still ends a text file
        je      brl_eof
        cmp     al,13
        je      brl_term
        cmp     al,10
        je      brl_term
        cmp     edx,BAT_LINE - 1
        jae     brl_drop
        mov     [edi],al
        inc     edi
brl_drop:
        inc     esi
        inc     edx
        dec     ecx
        jmp     short brl_scan

brl_term:
        inc     esi
        inc     edx
        dec     ecx
        cmp     al,13                   ; CR, CRLF and bare LF all end a line
        jne     brl_lineend
        or      ecx,ecx
        jz      brl_lineend
        cmp     byte ptr [esi],10
        jne     brl_lineend
        inc     edx
brl_lineend:
        mov     byte ptr [edi],0
        mov     esi,[bat_cur]
        add     [esi+BF_POS],edx
        popad
        clc
        ret

brl_closeof:
        mov     ebx,[brl_hdl]
        mov     ah,3Eh
        int     21h
        jmp     short brl_eof
brl_gone:
        ; THE FILE HAS GONE since the last line, which reopening every
        ; line makes possible and DOS answers the way it always has.
        mov     edx,OFFSET msg_batmiss
        call    print
brl_eof:
        mov     byte ptr [line_buf],0
        popad
        stc
        ret

brl_hdl     dd  0

;=====================================================================
; bat_getline - the next command from the running batch file, expanded
;               and echoed.  CF clear: ESI -> exp_buf.  CF set: this
;               line produced nothing to run (label, blank, or EOF,
;               in which case the level has been popped).
;
; Called straight from get_line, so it must return the way get_line's
; keyboard path does.
;=====================================================================
bat_getline:
        call    bat_rawline
        jc      bgl_eof

        ; the line parses by its file's class: .CMD advanced, .BAT strict
        mov     esi,[bat_cur]
        mov     eax,[esi+BF_CMD]
        mov     [adv_parse],al

        mov     esi,OFFSET line_buf
        mov     edi,OFFSET exp_buf
        call    bat_expand

        mov     esi,OFFSET exp_buf
        call    skip_blanks
        cmp     byte ptr [esi],0
        je      bgl_nothing
        cmp     byte ptr [esi],':'      ; a label is a destination,
        je      bgl_nothing             ; never a command

        ; echo it, unless @ or ECHO OFF says otherwise
        cmp     byte ptr [esi],'@'
        je      bgl_run
        cmp     byte ptr [echo_flag],0
        je      bgl_run
        push    esi
        call    put_prompt
        pop     esi
        push    esi
        call    print_asciiz
        call    put_crlf
        pop     esi
bgl_run:
        clc
        ret

bgl_nothing:
        stc
        ret
bgl_eof:
        call    bat_pop
        stc
        ret

;=====================================================================
; bat_expand - ESI -> raw line, EDI -> exp_buf.  Substitutes:
;
;   %%      a literal %
;   %0-%9   the batch arguments (an absent one vanishes)
;   %NAME%  the environment variable, or nothing if undefined
;
; A % with no closing partner stands as written, which is what lets
; "50%" survive a batch file intact.
;=====================================================================
bat_expand:
        pushad
be_lp:
        mov     al,[esi]
        or      al,al
        jz      be_end
        cmp     al,'%'
        je      be_pct
        call    be_put
        inc     esi
        jmp     short be_lp

be_pct:
        inc     esi
        mov     al,[esi]
        or      al,al
        jz      be_lone
        cmp     al,'%'
        je      be_dbl
        cmp     al,'0'
        jb      be_name
        cmp     al,'9'
        ja      be_name

        sub     al,'0'                  ; %0..%9, offset by SHIFT
        movzx   eax,al
        inc     esi
        mov     ebx,[bat_cur]
        or      ebx,ebx
        jz      be_lp                   ; not in a batch file: nothing
        add     eax,[ebx+BF_BASE]
        cmp     eax,BF_ARGC
        jae     be_lp                   ; shifted off the end
        mov     ebx,[ebx+BF_ARGV+eax*4]
        or      ebx,ebx
        jz      be_lp
be_arg:
        mov     al,[ebx]
        or      al,al
        jz      be_lp
        call    be_put
        inc     ebx
        jmp     short be_arg

be_dbl:
        mov     al,'%'
        call    be_put
        inc     esi
        jmp     short be_lp

be_lone:
        mov     al,'%'
        call    be_put
        jmp     short be_lp             ; ESI is already on the NUL

be_name:
        push    esi                     ; where the name started
        mov     ebx,OFFSET be_namebuf
        xor     ecx,ecx
be_nlp:
        mov     al,[esi]
        or      al,al
        jz      be_noclose
        cmp     al,'%'
        je      be_nclose
        call    upcase
        cmp     ecx,31
        jae     be_nskip
        mov     [ebx+ecx],al
        inc     ecx
be_nskip:
        inc     esi
        jmp     short be_nlp

be_nclose:
        mov     byte ptr [ebx+ecx],0
        inc     esi                     ; past the closing %
        mov     [be_resume],esi
        pop     eax                     ; the start is no longer wanted
        mov     esi,OFFSET be_namebuf
        call    env_find
        jc      be_nundef
be_nval:
        mov     al,[esi]
        or      al,al
        jz      be_nundef
        call    be_put
        inc     esi
        jmp     short be_nval
be_nundef:
        mov     esi,[be_resume]
        jmp     be_lp

be_noclose:
        pop     esi                     ; back to just after the %
        mov     al,'%'
        call    be_put
        jmp     be_lp

be_end:
        mov     byte ptr [edi],0
        popad
        ret

; be_put - append AL to exp_buf, never past the end
be_put:
        cmp     edi,OFFSET exp_buf + BAT_LINE - 1
        jae     bep_out
        mov     [edi],al
        inc     edi
bep_out:
        ret

;=====================================================================
; GOTO label
;
; The label is found by reading the file from the top, so a batch file
; can jump backwards as freely as forwards.
;=====================================================================
cmd_goto:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        je      goto_syntax
        cmp     dword ptr [bat_cur],0
        je      goto_out                ; nothing to jump around in

        cmp     byte ptr [esi],':'
        jne     goto_name
        inc     esi
goto_name:
        mov     edi,OFFSET goto_lbl
        xor     ecx,ecx
gt_cp:
        mov     al,[esi]
        or      al,al
        jz      gt_cpd
        cmp     al,' '
        je      gt_cpd
        cmp     al,9
        je      gt_cpd
        call    upcase
        cmp     ecx,GOTOLBL_MAX - 1
        jae     gt_skip
        mov     [edi],al
        inc     edi
        inc     ecx
gt_skip:
        inc     esi
        jmp     short gt_cp
gt_cpd:
        mov     byte ptr [edi],0
        or      ecx,ecx
        jz      goto_syntax

        mov     esi,[bat_cur]
        mov     dword ptr [esi+BF_POS],0
gt_scan:
        call    bat_rawline
        jc      goto_nolabel
        mov     esi,OFFSET line_buf
        call    skip_blanks
        cmp     byte ptr [esi],':'
        jne     gt_scan
        inc     esi
        mov     edi,OFFSET goto_lbl
        call    lbl_match
        jc      gt_scan
goto_out:                               ; BF_POS is now just past it
        ret

goto_nolabel:
        mov     edx,OFFSET msg_nolabel2
        call    print
        call    bat_pop                 ; MS-DOS ends the file here
        ret
goto_syntax:
        mov     edx,OFFSET msg_syntax
        call    print
        ret

;---------------------------------------------------------------------
; lbl_match - ESI -> a label in the file, EDI -> the wanted one
;             (uppercase).  CF clear if they are the same word.
;---------------------------------------------------------------------
lbl_match:
        push    esi
        push    edi
        push    eax
lm_lp:
        mov     ah,[edi]
        mov     al,[esi]
        or      al,al
        jz      lm_srcend
        cmp     al,' '
        je      lm_srcend
        cmp     al,9
        je      lm_srcend
        call    upcase
        cmp     al,ah
        jne     lm_no
        inc     esi
        inc     edi
        jmp     short lm_lp
lm_srcend:
        or      ah,ah                   ; both ended together?
        jnz     lm_no
        pop     eax
        pop     edi
        pop     esi
        clc
        ret
lm_no:
        pop     eax
        pop     edi
        pop     esi
        stc
        ret

;=====================================================================
; SHIFT - %1 becomes %0, %2 becomes %1, and a new argument slides into
;         %9 from beyond the ten a batch file can otherwise name.
;=====================================================================
cmd_shift:
        mov     edi,[bat_cur]
        or      edi,edi
        jz      sh_out
        mov     eax,[edi+BF_BASE]
        cmp     eax,BF_ARGC
        jae     sh_out                  ; everything has been shifted away
        inc     eax
        mov     [edi+BF_BASE],eax
sh_out:
        ret

;=====================================================================
; CALL command
;
; Runs the rest of the line as usual, but with [call_flag] set so that
; a batch target stacks a frame instead of replacing this one.
;=====================================================================
cmd_call:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        je      call_out
        mov     byte ptr [call_flag],1
        call    exec_line
        mov     byte ptr [call_flag],0
call_out:
        ret

;=====================================================================
; IF [NOT] ERRORLEVEL n command
; IF [NOT] EXIST file command
; IF [NOT] str1==str2 command
;=====================================================================
cmd_if:
        mov     esi,[arg_ptr]
        mov     byte ptr [if_not],0
if_again:
        call    skip_blanks
        cmp     byte ptr [esi],0
        je      if_syntax
        mov     edi,OFFSET lit_not
        call    kw_match
        jc      if_test
        xor     byte ptr [if_not],1     ; NOT NOT is not an error
        jmp     short if_again

if_test:
        mov     edi,OFFSET lit_errlvl
        call    kw_match
        jnc     if_errlvl
        mov     edi,OFFSET lit_exist
        call    kw_match
        jnc     if_exist
        jmp     if_strcmp

;--- IF ERRORLEVEL n: true when the code is n OR HIGHER, which is why
;--- the idiom tests downwards from the largest value
if_errlvl:
        call    parse_num
        jc      if_syntax
        movzx   ebx,byte ptr [errorlevel]
        cmp     ebx,eax
        jae     if_true
        jmp     if_false

;--- IF EXIST: plain files only, as in MS-DOS - which is exactly why
;--- everyone learned to write "IF EXIST dir\NUL".  The test walks
;--- the long find so a (possibly quoted) long name tests true.
if_exist:
        call    next_arg
        jc      if_syntax
        push    esi
        mov     edx,OFFSET tok_buf
        xor     ecx,ecx                 ; plain files only
        call    xfind_first             ; (classic under LFN=OFF)
        pop     esi
        jc      if_false
        push    esi
        mov     bx,ax                   ; found - the handle can go
        call    xfind_close
        pop     esi
        jmp     if_true

;--- IF str1==str2: case sensitive, as MS-DOS 6.22 has it
if_strcmp:
        mov     edi,OFFSET if_s1
        call    if_grab
        cmp     word ptr [esi],3D3Dh    ; "=="
        jne     if_syntax
        add     esi,2
        mov     edi,OFFSET if_s2
        call    if_grab
        call    skip_blanks

        push    esi
        mov     esi,OFFSET if_s1
        mov     edi,OFFSET if_s2
ifc_lp:
        mov     al,[esi]
        cmp     al,[edi]
        jne     ifc_ne
        or      al,al
        jz      ifc_eq
        inc     esi
        inc     edi
        jmp     short ifc_lp
ifc_eq:
        pop     esi
        jmp     short if_true
ifc_ne:
        pop     esi
        jmp     short if_false

if_true:
        cmp     byte ptr [if_not],0
        je      if_run
        ret
if_false:
        cmp     byte ptr [if_not],0
        jne     if_run
        ret
if_run:
        call    skip_blanks
        cmp     byte ptr [esi],0
        je      if_none
        call    exec_line
if_none:
        ret
if_syntax:
        mov     edx,OFFSET msg_syntax
        call    print
        ret

;---------------------------------------------------------------------
; if_grab - copy one comparison operand from ESI to EDI, stopping at
;           '=', a blank or the end.  Verbatim: the comparison is
;           case sensitive, so nothing may be folded on the way in.
;---------------------------------------------------------------------
if_grab:
        push    ecx
        xor     ecx,ecx
ig_lp:
        mov     al,[esi]
        or      al,al
        jz      ig_end
        cmp     al,'='
        je      ig_end
        cmp     al,' '
        je      ig_end
        cmp     al,9
        je      ig_end
        cmp     ecx,TOK_MAX - 1
        jae     ig_skip
        mov     [edi],al
        inc     edi
        inc     ecx
ig_skip:
        inc     esi
        jmp     short ig_lp
ig_end:
        mov     byte ptr [edi],0
        pop     ecx
        ret

;=====================================================================
; FOR %v IN (item item ...) DO command
;
; The set is expanded in full before the body runs even once, because
; the body is free to use the DTA - and would otherwise walk all over
; the very search that is feeding it.
;=====================================================================
cmd_for:
        cmp     byte ptr [for_busy],0
        jne     for_nested
        mov     esi,[arg_ptr]
        call    skip_blanks
        cmp     byte ptr [esi],'/'      ; FOR /F: lines and tokens, below
        jne     fr_classic
        mov     al,[esi+1]
        call    upcase
        cmp     al,'F'
        jne     for_syntax
        mov     al,[esi+2]
        cmp     al,' '
        je      fr_slashf
        cmp     al,9
        jne     for_syntax
fr_slashf:
        add     esi,2
        jmp     ff_start
fr_classic:
        cmp     byte ptr [esi],'%'
        jne     for_syntax
        inc     esi
        mov     al,[esi]
        or      al,al
        jz      for_syntax
        mov     [for_var],al
        inc     esi
        call    skip_blanks

        mov     edi,OFFSET lit_in
        call    kw_match
        jc      for_syntax
        cmp     byte ptr [esi],'('
        jne     for_syntax
        inc     esi

        mov     edi,OFFSET for_list     ; the set, up to ')'
        xor     ecx,ecx
fr_set:
        mov     al,[esi]
        or      al,al
        jz      for_syntax
        cmp     al,')'
        je      fr_setend
        cmp     ecx,FORLIST_MAX - 1
        jae     fr_setskip
        mov     [edi],al
        inc     edi
        inc     ecx
fr_setskip:
        inc     esi
        jmp     short fr_set
fr_setend:
        mov     byte ptr [edi],0
        inc     esi
        call    skip_blanks

        mov     edi,OFFSET lit_do
        call    kw_match
        jc      for_syntax
        cmp     byte ptr [esi],0
        je      for_syntax

        mov     edi,OFFSET for_body     ; and the body
        xor     ecx,ecx
fr_bod:
        mov     al,[esi]
        or      al,al
        jz      fr_bodend
        cmp     ecx,FORBODY_MAX - 1
        jae     fr_bodend
        mov     [edi],al
        inc     edi
        inc     ecx
        inc     esi
        jmp     short fr_bod
fr_bodend:
        mov     byte ptr [edi],0

;--- flatten the set, expanding any wildcards, into for_names --------
        mov     edx,OFFSET dta
        mov     ah,1Ah
        int     21h

        mov     edi,OFFSET for_names
        mov     eax,edi
        add     eax,FORNAMES_MAX - 2
        mov     [fr_lim],eax
        mov     esi,OFFSET for_list
fr_item:
        call    bat_sep
        cmp     byte ptr [esi],0
        je      fr_items_done

        mov     ebx,OFFSET tok_buf      ; one item of the set
        xor     ecx,ecx
        xor     ah,ah                   ; AH = inside a quoted span
fr_icp:
        mov     al,[esi]
        or      al,al
        jz      fr_icpd
        cmp     byte ptr [adv_parse],0  ; advanced mode: "a b" is ONE
        je      fr_inoq                 ; item, quotes kept so the
        cmp     al,'"'                  ; substituted body re-parses
        jne     fr_inoq
        xor     ah,1
        jmp     short fr_ikeep
fr_inoq:
        or      ah,ah
        jnz     fr_ikeep
        call    bat_issep
        jnc     fr_icpd
fr_ikeep:
        cmp     ecx,TOK_MAX - 1
        jae     fr_icsk
        mov     [ebx+ecx],al
        inc     ecx
fr_icsk:
        inc     esi
        jmp     short fr_icp
fr_icpd:
        mov     byte ptr [ebx+ecx],0

        push    esi
        mov     esi,OFFSET tok_buf
        call    has_wild
        jnc     fr_plain

        ; a pattern: every match becomes an item of its own
        mov     edx,OFFSET tok_buf
        mov     ecx,ATTR_DIR
        mov     ah,4Eh
        int     21h
        jc      fr_nomatch
fr_match:
        mov     esi,OFFSET dta + 1Eh
        call    fr_addname
        mov     ah,4Fh
        int     21h
        jnc     fr_match
fr_nomatch:
        pop     esi
        jmp     short fr_item

fr_plain:
        mov     esi,OFFSET tok_buf
        call    fr_addname
        pop     esi
        jmp     short fr_item

fr_items_done:
        mov     byte ptr [edi],0        ; the extra NUL ends the list

;--- and now run the body once for each -----------------------------
        mov     byte ptr [for_busy],1
        mov     dword ptr [for_nv],1    ; one variable: the item
        mov     esi,OFFSET for_names
fr_run:
        cmp     byte ptr [esi],0
        je      fr_done
        push    esi
        mov     [for_vp],esi
        call    for_subst
        mov     esi,OFFSET for_line
        call    exec_line
        pop     esi
fr_step:
        lodsb
        or      al,al
        jnz     fr_step
        jmp     short fr_run
fr_done:
        mov     byte ptr [for_busy],0
        ret

for_syntax:
        mov     edx,OFFSET msg_syntax
        call    print
        ret
for_nested:
        mov     edx,OFFSET msg_fornest
        call    print
        ret

;---------------------------------------------------------------------
; fr_addname - append the ASCIIZ at ESI to for_names at EDI
;---------------------------------------------------------------------
fr_addname:
        push    eax
fa_lp:
        mov     al,[esi]
        cmp     edi,[fr_lim]
        jae     fa_out
        mov     [edi],al
        inc     edi
        inc     esi
        or      al,al
        jnz     fa_lp
        pop     eax
        ret
fa_out:
        mov     byte ptr [edi],0
        pop     eax
        ret

;---------------------------------------------------------------------
; for_subst - for_line = for_body with every variable replaced by its
;             value.  Rebuilt per iteration because running the body is
;             free to chew up the copy.
;
; [for_nv] variables, the first named [for_var] and the rest the
; letters after it - which is how FOR /F's tokens=1,2,3 become %a, %b
; and %c - with their values at [for_vp].  The ordinary FOR has one.
; Case sensitive, as MS-DOS has it: %a and %A are two variables.
;---------------------------------------------------------------------
for_subst:
        pushad
        mov     esi,OFFSET for_body
        mov     edi,OFFSET for_line
fs_lp:
        mov     al,[esi]
        or      al,al
        jz      fs_end
        cmp     al,'%'
        jne     fs_lit
        movzx   ecx,byte ptr [esi+1]
        movzx   edx,byte ptr [for_var]
        sub     ecx,edx                 ; which of them, from the first
        jb      fs_lit
        cmp     ecx,[for_nv]
        jae     fs_lit
        mov     ebx,[for_vp+ecx*4]
        add     esi,2
        push    ebx
fs_item:
        mov     al,[ebx]
        or      al,al
        jz      fs_itemd
        call    fs_put
        inc     ebx
        jmp     short fs_item
fs_itemd:
        pop     ebx
        jmp     short fs_lp
fs_lit:
        call    fs_put
        inc     esi
        jmp     short fs_lp
fs_end:
        mov     byte ptr [edi],0
        popad
        ret

fs_put:
        cmp     edi,OFFSET for_line + FORLINE_MAX - 1
        jae     fsp_out
        mov     [edi],al
        inc     edi
fsp_out:
        ret

;=====================================================================
; FOR /F ["options"] %v IN (set) DO command
;
; Windows NT's, which MS-DOS never had: the set is TEXT, taken apart a
; line at a time and each line cut into tokens.  It is here because a
; batch file that has to read a file, or the output of a command, had
; no other way to do it on this system.
;
;   (file1 file2 ...)   the lines of those files
;   ("string")          the one line in the quotes
;   ('command')         the lines the command writes - run through
;                       COMSPEC /C with its output in a temporary file,
;                       which is how NT runs it too, so that a batch file
;                       or a pipeline there works as it would anywhere
;
; and with usebackq the quotes move: "file with spaces", 'string',
; `command`.
;
;   eol=c       a line starting with c is a comment (default ;)
;   skip=n      the first n lines are skipped
;   delims=xxx  what separates tokens (default space and tab).  It
;               runs to the closing quote, or to a blank that another
;               option follows - so "delims=, tokens=2" is a comma and
;               "tokens=2 delims=, " is a comma and a space
;   tokens=x,y,m-n[*]   which tokens, each into the next letter after
;               %v; a '*' after the last one takes the rest of the
;               line.  "tokens=*" is the whole line.  Default 1.
;
; Blank lines are skipped, and so is a line with no first token in it.
; Everything else the body sees through for_subst like any FOR.
;=====================================================================
FFQ_MAXTOK      EQU     31              ; the most tokens=, as in NT
FFQ_VBUF        EQU     512             ; one line's token values
FFQ_TMPMAX      EQU     96              ; the temporary file's path
FFCLINE_MAX     EQU     256             ; COMSPEC /C command > file

ff_start:
        call    skip_blanks
        mov     byte ptr [ff_eol],';'
        mov     dword ptr [ff_skip],0
        mov     byte ptr [ff_backq],0
        mov     dword ptr [ff_tmask],2  ; bit n = token n: token 1
        mov     dword ptr [ff_tmax],1
        mov     byte ptr [ff_star],0
        mov     dword ptr [ff_delims],0920h     ; blank, tab, NUL
        cmp     byte ptr [esi],'"'
        jne     ff_var
        inc     esi
        call    ff_options
        jc      for_syntax
        call    skip_blanks
ff_var:
        cmp     byte ptr [esi],'%'
        jne     for_syntax
        inc     esi
        mov     al,[esi]
        or      al,al
        jz      for_syntax
        mov     [for_var],al
        inc     esi
        call    skip_blanks
        mov     edi,OFFSET lit_in
        call    kw_match
        jc      for_syntax
        cmp     byte ptr [esi],'('
        jne     for_syntax
        inc     esi
        call    ff_setend               ; EDI -> the ')' before DO
        jc      for_syntax
        mov     ecx,edi
        sub     ecx,esi
        cmp     ecx,FORLIST_MAX - 1
        jb      ff_lcp
        mov     ecx,FORLIST_MAX - 1
ff_lcp:
        push    edi
        mov     edi,OFFSET for_list
        rep     movsb
        mov     byte ptr [edi],0
        pop     esi
        inc     esi                     ; past the ')'
        call    skip_blanks
        mov     edi,OFFSET lit_do
        call    kw_match
        jc      for_syntax
        cmp     byte ptr [esi],0
        je      for_syntax
        mov     edi,OFFSET for_body
        xor     ecx,ecx
ff_bod:
        mov     al,[esi]
        or      al,al
        jz      ff_bodend
        cmp     ecx,FORBODY_MAX - 1
        jae     ff_bodend
        mov     [edi+ecx],al
        inc     ecx
        inc     esi
        jmp     short ff_bod
ff_bodend:
        mov     byte ptr [edi+ecx],0

        ; how many variables the tokens make: one per token, and one
        ; more for a '*'
        mov     eax,[ff_tmask]
        xor     ecx,ecx
ff_count:
        or      eax,eax
        jz      ff_counted
        shr     eax,1
        adc     ecx,0
        jmp     short ff_count
ff_counted:
        movzx   eax,byte ptr [ff_star]
        add     ecx,eax
        mov     [ff_nv],ecx

        mov     byte ptr [for_busy],1
        mov     esi,OFFSET for_list
        call    skip_blanks
        mov     al,[esi]
        cmp     byte ptr [ff_backq],0
        jne     ff_bq
        cmp     al,'"'
        je      ff_string
        cmp     al,"'"
        je      ff_command
        jmp     short ff_files
ff_bq:
        cmp     al,"'"
        je      ff_string
        cmp     al,'`'
        je      ff_command
        jmp     short ff_files

ff_string:
        call    ff_unquote              ; ESI -> the text, its quote cut off
        call    ff_text
        jmp     short ff_end
ff_command:
        call    ff_unquote
        call    ff_runcmd
        jmp     short ff_end
ff_files:
        call    skip_blanks
        cmp     byte ptr [esi],0
        je      ff_end
        mov     edi,OFFSET tok_buf      ; one name, quoted only under
        xor     ecx,ecx                 ; usebackq
        xor     ah,ah
        cmp     byte ptr [ff_backq],0
        je      ff_fname
        cmp     byte ptr [esi],'"'
        jne     ff_fname
        inc     esi
        mov     ah,1
ff_fname:
        mov     al,[esi]
        or      al,al
        jz      ff_fend
        or      ah,ah
        jnz     ff_fquoted
        cmp     al,' '
        je      ff_fend
        cmp     al,9
        je      ff_fend
        jmp     short ff_fkeep
ff_fquoted:
        cmp     al,'"'
        jne     ff_fkeep
        inc     esi                     ; the closing quote goes too
        jmp     short ff_fend
ff_fkeep:
        cmp     ecx,TOK_MAX - 1
        jae     ff_fskip
        mov     [edi+ecx],al
        inc     ecx
ff_fskip:
        inc     esi
        jmp     short ff_fname
ff_fend:
        mov     byte ptr [edi+ecx],0
        mov     edx,OFFSET tok_buf
        call    ff_file                 ; ESI survives: it is for_list
        jmp     short ff_files
ff_end:
        mov     byte ptr [for_busy],0
        ret

;---------------------------------------------------------------------
; ff_unquote - ESI -> an opening quote.  ESI comes back on the text
;              after it, with the matching closing quote (the LAST one,
;              so a command may quote things inside itself) cut to a
;              NUL.  No closing quote: to the end.
;---------------------------------------------------------------------
ff_unquote:
        push    eax
        push    edx
        push    edi
        mov     al,[esi]
        inc     esi
        mov     edi,esi
        xor     edx,edx                 ; the last one seen
fu_lp:
        cmp     byte ptr [edi],0
        je      fu_cut
        cmp     [edi],al
        jne     fu_next
        mov     edx,edi
fu_next:
        inc     edi
        jmp     short fu_lp
fu_cut:
        or      edx,edx
        jz      fu_out
        mov     byte ptr [edx],0
fu_out:
        pop     edi
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; ff_setend - ESI -> just inside the set's '('.  EDI -> the ')' that
;             closes it, which is the first one followed by DO: a
;             command in the set may have brackets of its own.  CF if
;             there is none.
;---------------------------------------------------------------------
ff_setend:
        push    esi
        mov     edi,esi
fse_lp:
        cmp     byte ptr [edi],0
        je      fse_none
        cmp     byte ptr [edi],')'
        jne     fse_next
        lea     esi,[edi+1]
        call    skip_blanks
        push    edi
        mov     edi,OFFSET lit_do
        call    kw_match
        pop     edi
        jnc     fse_hit
fse_next:
        inc     edi
        jmp     short fse_lp
fse_hit:
        pop     esi
        clc
        ret
fse_none:
        pop     esi
        stc
        ret

;---------------------------------------------------------------------
; ff_options - ESI -> just inside the options' opening quote.  They are
;              read into ff_*, and ESI comes back past the closing
;              quote.  CF on anything that is not an option.
;---------------------------------------------------------------------
ff_options:
fo_lp:
        call    skip_blanks
        mov     al,[esi]
        or      al,al
        jz      fo_bad
        cmp     al,'"'
        je      fo_end
        mov     edi,OFFSET kwf_eol
        call    ff_kw
        jnc     fo_eol
        mov     edi,OFFSET kwf_skip
        call    ff_kw
        jnc     fo_skip
        mov     edi,OFFSET kwf_delims
        call    ff_kw
        jnc     fo_delims
        mov     edi,OFFSET kwf_tokens
        call    ff_kw
        jnc     fo_tokens
        mov     edi,OFFSET kwf_backq
        call    ff_kw
        jnc     fo_backq
fo_bad:
        stc
        ret
fo_end:
        inc     esi
        clc
        ret

fo_eol:
        mov     al,[esi]
        or      al,al
        jz      fo_bad
        cmp     al,'"'                  ; "eol=" and nothing: no comments
        je      fo_eolnone
        mov     [ff_eol],al
        inc     esi
        jmp     short fo_lp
fo_eolnone:
        mov     byte ptr [ff_eol],0
        jmp     short fo_lp

fo_skip:
        call    parse_num
        jc      fo_bad
        mov     [ff_skip],eax
        jmp     short fo_lp

fo_backq:
        mov     byte ptr [ff_backq],1
        jmp     short fo_lp

fo_delims:
        mov     edi,OFFSET ff_delims
        xor     ecx,ecx
fd_lp:
        mov     al,[esi]
        or      al,al
        jz      fd_end
        cmp     al,'"'
        je      fd_end
        cmp     al,' '
        jne     fd_take
        call    ff_optnext              ; a blank with another option after
        jnc     fd_end                  ; it ends the set; any other is in it
fd_take:
        cmp     ecx,31
        jae     fd_skip
        mov     [edi+ecx],al
        inc     ecx
fd_skip:
        inc     esi
        jmp     short fd_lp
fd_end:
        mov     byte ptr [edi+ecx],0
        jmp     fo_lp

fo_tokens:
        mov     dword ptr [ff_tmask],0
        mov     dword ptr [ff_tmax],0
        mov     byte ptr [ff_star],0
ftk_spec:
        cmp     byte ptr [esi],'*'
        je      ftk_star
        call    parse_num
        jc      fo_bad
        mov     ebx,eax                 ; from
        mov     edx,eax                 ; to
        cmp     byte ptr [esi],'-'
        jne     ftk_range
        inc     esi
        call    parse_num
        jc      fo_bad
        mov     edx,eax
ftk_range:
        or      ebx,ebx
        jz      fo_bad
        cmp     edx,FFQ_MAXTOK
        ja      fo_bad
        cmp     ebx,edx
        ja      fo_bad
ftk_bit:
        bts     dword ptr [ff_tmask],ebx
        cmp     ebx,[ff_tmax]
        jbe     ftk_nomax
        mov     [ff_tmax],ebx
ftk_nomax:
        inc     ebx
        cmp     ebx,edx
        jbe     ftk_bit
        cmp     byte ptr [esi],','
        jne     ftk_after
        inc     esi
        jmp     short ftk_spec
ftk_after:
        cmp     byte ptr [esi],'*'
        jne     fo_lp
ftk_star:
        mov     byte ptr [ff_star],1
        inc     esi
        jmp     fo_lp

; ff_kw - is the option word at ESI the one at EDI (upper case)?  An
;         '=' ends the ones that take a value; "usebackq" must end at a
;         blank or the quote.  ESI past it and CF clear if so.
ff_kw:
        push    eax
        push    ecx
        xor     ecx,ecx
fk_lp:
        mov     ah,[edi+ecx]
        or      ah,ah
        jz      fk_end
        mov     al,[esi+ecx]
        call    upcase
        cmp     al,ah
        jne     fk_no
        inc     ecx
        jmp     short fk_lp
fk_end:
        cmp     byte ptr [edi+ecx-1],'='
        je      fk_yes
        mov     al,[esi+ecx]
        cmp     al,' '
        je      fk_yes
        cmp     al,'"'
        je      fk_yes
        or      al,al
        jz      fk_yes
fk_no:
        pop     ecx
        pop     eax
        stc
        ret
fk_yes:
        add     esi,ecx
        pop     ecx
        pop     eax
        clc
        ret

; ff_optnext - ESI -> a blank inside delims=.  CF clear if, after the
;              blanks, another option starts - which ends the set.
;              ESI is left where it was.
ff_optnext:
        push    esi
        push    edi
        call    skip_blanks
        mov     edi,OFFSET kwf_eol
        call    ff_kw
        jnc     fon_yes
        mov     edi,OFFSET kwf_skip
        call    ff_kw
        jnc     fon_yes
        mov     edi,OFFSET kwf_delims
        call    ff_kw
        jnc     fon_yes
        mov     edi,OFFSET kwf_tokens
        call    ff_kw
        jnc     fon_yes
        mov     edi,OFFSET kwf_backq
        call    ff_kw
        jnc     fon_yes
        pop     edi
        pop     esi
        stc
        ret
fon_yes:
        pop     edi
        pop     esi
        clc
        ret

;---------------------------------------------------------------------
; ff_file - EDX -> a file name: its lines, through the body.  A file
;           that will not open is said so and passed over.
;
; READ WHOLE FIRST, and then run: the body is free to write to the
; very file being read - "FOR /F %a IN (LIST) DO ECHO %a >> LIST" - and
; a file read a line at a time underneath that would never end.  NT
; reads it first for the same reason.
;---------------------------------------------------------------------
ff_file:
        pushad
        mov     ax,3D00h
        int     21h
        jc      ffl_none
        movzx   ebx,ax
        mov     [ff_hdl],ebx
        xor     ecx,ecx
        xor     edx,edx
        mov     ax,4202h                ; how long it is
        int     21h
        jc      ffl_close
        mov     [ff_size],eax
        mov     ebx,[ff_hdl]
        xor     ecx,ecx
        xor     edx,edx
        mov     ax,4200h
        int     21h
        mov     ebx,[ff_size]
        or      ebx,ebx
        jz      ffl_close               ; nothing in it: no lines
        add     ebx,2
        mov     ah,48h
        int     21h
        jc      ffl_nomem
        mov     [ff_blk],eax
        mov     edx,eax
        mov     ecx,[ff_size]
        mov     ebx,[ff_hdl]
        mov     ah,3Fh
        int     21h
        jc      ffl_free
        mov     esi,[ff_blk]
        mov     byte ptr [esi+eax],0    ; what was read, and not a byte more
        mov     ebx,[ff_hdl]            ; closed BEFORE the body runs: the
        mov     ah,3Eh                  ; body may want it
        int     21h
        mov     dword ptr [ff_hdl],-1
        call    ff_text
ffl_free:
        mov     edx,[ff_blk]
        mov     ah,49h
        int     21h
ffl_close:
        mov     ebx,[ff_hdl]
        cmp     ebx,-1
        je      ffl_out
        mov     ah,3Eh
        int     21h
ffl_out:
        popad
        ret
ffl_none:
        push    edx
        mov     edx,OFFSET msg_ffnone
        call    print
        pop     esi
        call    print_asciiz
        call    put_crlf
        popad
        ret
ffl_nomem:
        mov     edx,OFFSET msg_ffmem
        call    print
        jmp     short ffl_close

;---------------------------------------------------------------------
; ff_runcmd - ESI -> a command.  It runs through COMSPEC /C with its
;             standard output in a temporary file, which is then read
;             like any other and deleted.
;
; The file goes where TEMP says, as the pipes' would on DOS, and in the
; current directory when there is no TEMP - which is where this
; shell's own pipe files go.
;---------------------------------------------------------------------
ff_runcmd:
        pushad
        mov     [ff_cmd],esi
        mov     edi,OFFSET ff_tmp       ; the file: TEMP\%FORF.$$$
        push    esi
        mov     esi,OFFSET ffn_temp
        call    env_find
        jc      frc_here
        mov     ecx,FFQ_TMPMAX - 16
frc_tdir:
        lodsb
        or      al,al
        jz      frc_tend
        stosb
        dec     ecx
        jnz     frc_tdir
frc_tend:
        cmp     edi,OFFSET ff_tmp
        je      frc_here
        cmp     byte ptr [edi-1],'\'
        je      frc_here
        mov     al,'\'
        stosb
frc_here:
        mov     esi,OFFSET ffn_tmpname
frc_tn:
        lodsb
        stosb
        or      al,al
        jnz     frc_tn
        pop     esi

        ; The line: THIS interpreter /C command > the file.  Not COMSPEC,
        ; which names the 16-bit COMMAND.COM DOS programs get (see
        ; set_comspec) - FOR /F's command is a CMD.EXE command line, and
        ; it is the kernel that knows where CMD.EXE is.
        mov     edi,OFFSET ff_cline
        mov     edx,edi
        mov     ax,0F103h
        int     21h
        jc      frc_dflt
        cmp     byte ptr [edi],0
        je      frc_dflt
frc_end:
        cmp     byte ptr [edi],0
        je      frc_cs
        inc     edi
        jmp     short frc_end
frc_dflt:
        mov     esi,OFFSET ffn_command  ; an older kernel: the usual name
        call    ff_cat
frc_cs:
        mov     esi,OFFSET ffn_slashc
        call    ff_cat
        mov     esi,[ff_cmd]
        call    ff_cat
        mov     esi,OFFSET ffn_to
        call    ff_cat
        mov     esi,OFFSET ff_tmp
        call    ff_cat
        mov     byte ptr [edi],0

        mov     esi,OFFSET ff_cline
        call    exec_line
        mov     edx,OFFSET ff_tmp
        call    ff_file
        mov     edx,OFFSET ff_tmp
        mov     ah,41h
        int     21h
        popad
        ret

; ff_cat - the ASCIIZ at ESI onto ff_cline at EDI, while there is room
ff_cat:
        lodsb
        or      al,al
        jz      fc_out
        cmp     edi,OFFSET ff_cline + FFCLINE_MAX - 1
        jae     ff_cat
        stosb
        jmp     short ff_cat
fc_out:
        ret

;---------------------------------------------------------------------
; ff_text - ESI -> text, NUL-terminated: each line of it through the
;           body.  CR LF, a bare LF and a bare CR all end a line, and a
;           Ctrl+Z ends the text, as it ends a text file.
;---------------------------------------------------------------------
ff_text:
        pushad
ftx_line:
        mov     al,[esi]
        or      al,al
        jz      ftx_done
        cmp     al,1Ah
        je      ftx_done
        mov     edi,esi
ftx_end:
        mov     al,[edi]
        or      al,al
        jz      ftx_cut
        cmp     al,13
        je      ftx_cut
        cmp     al,10
        je      ftx_cut
        cmp     al,1Ah
        je      ftx_cut
        inc     edi
        jmp     short ftx_end
ftx_cut:
        mov     byte ptr [edi],0
        push    eax
        push    edi
        call    ff_line
        pop     edi
        pop     eax
        mov     esi,edi
        or      al,al
        jz      ftx_done
        cmp     al,1Ah
        je      ftx_done
        inc     esi
        cmp     al,13
        jne     ftx_line
        cmp     byte ptr [esi],10       ; CR LF is one line end
        jne     ftx_line
        inc     esi
        jmp     short ftx_line
ftx_done:
        popad
        ret

;---------------------------------------------------------------------
; ff_line - ESI -> one line.  Skipped if it is empty, one of the first
;           skip= lines, or a comment; otherwise tokenised and run.
;---------------------------------------------------------------------
ff_line:
        pushad
        cmp     byte ptr [esi],0
        je      fl_out
        cmp     dword ptr [ff_skip],0
        je      fl_noskip
        dec     dword ptr [ff_skip]
        jmp     short fl_out
fl_noskip:
        mov     al,[ff_eol]
        or      al,al
        jz      fl_tok
        cmp     [esi],al
        je      fl_out
fl_tok:
        call    ff_tokens
        jc      fl_out                  ; not even its first token
        call    for_subst
        mov     esi,OFFSET for_line
        call    exec_line
fl_out:
        popad
        ret

;---------------------------------------------------------------------
; ff_tokens - ESI -> a line: the tokens asked for into ff_vbuf, and
;             for_vp/for_nv pointing at them.  CF if the first variable
;             got nothing, which is a line the body does not run for.
;---------------------------------------------------------------------
ff_tokens:
        pushad
        mov     ecx,[ff_nv]             ; every variable starts empty
        xor     ebx,ebx
ftk_clr:
        cmp     ebx,ecx
        jae     ftk_clred
        mov     dword ptr [for_vp+ebx*4],OFFSET ff_empty
        inc     ebx
        jmp     short ftk_clr
ftk_clred:
        mov     [for_nv],ecx
        mov     dword ptr [ff_vp],OFFSET ff_vbuf
        xor     ebx,ebx                 ; the next variable to fill
        cmp     dword ptr [ff_tmask],0
        jne     ftk_walk
        call    ff_skipdel              ; "tokens=*": the whole line,
        jmp     short ftk_rest          ; leading separators dropped
ftk_walk:
        mov     edx,1                   ; the token number
ftk_lp:
        cmp     edx,FFQ_MAXTOK
        ja      ftk_done
        call    ff_skipdel
        cmp     byte ptr [esi],0
        je      ftk_done
        bt      dword ptr [ff_tmask],edx
        jnc     ftk_pass
        call    ff_word                 ; into variable EBX, EBX + 1
        cmp     edx,[ff_tmax]
        jne     ftk_next
        cmp     byte ptr [ff_star],0
        je      ftk_done
        call    ff_skipdel              ; and the rest, after the last one
        jmp     short ftk_rest
ftk_pass:
        call    ff_skipword
ftk_next:
        inc     edx
        jmp     short ftk_lp
ftk_rest:
        cmp     byte ptr [esi],0
        je      ftk_done
        mov     edi,[ff_vp]
        mov     [for_vp+ebx*4],edi
        inc     ebx
ftk_rcp:
        lodsb
        or      al,al
        jz      ftk_rend
        cmp     edi,OFFSET ff_vbuf + FFQ_VBUF - 1
        jae     ftk_rcp
        stosb
        jmp     short ftk_rcp
ftk_rend:
        mov     byte ptr [edi],0
ftk_done:
        or      ebx,ebx                 ; tokens fill the variables in
        popad                           ; order, so any at all means the
        jz      ftk_none                ; first one was filled
        clc
        ret
ftk_none:
        stc
        ret

; ff_word - the token at ESI into variable EBX; ESI past it, EBX + 1
ff_word:
        push    eax
        push    edi
        mov     edi,[ff_vp]
        mov     [for_vp+ebx*4],edi
        inc     ebx
fw_lp:
        mov     al,[esi]
        or      al,al
        jz      fw_end
        call    ff_isdel
        je      fw_end
        cmp     edi,OFFSET ff_vbuf + FFQ_VBUF - 1
        jae     fw_step
        mov     [edi],al
        inc     edi
fw_step:
        inc     esi
        jmp     short fw_lp
fw_end:
        mov     byte ptr [edi],0
        inc     edi
        mov     [ff_vp],edi
        pop     edi
        pop     eax
        ret

; ff_skipword - ESI past the token it is on
ff_skipword:
        push    eax
fsw_lp:
        mov     al,[esi]
        or      al,al
        jz      fsw_out
        call    ff_isdel
        je      fsw_out
        inc     esi
        jmp     short fsw_lp
fsw_out:
        pop     eax
        ret

; ff_skipdel - ESI past any separators
ff_skipdel:
        push    eax
fsd_lp:
        mov     al,[esi]
        or      al,al
        jz      fsd_out
        call    ff_isdel
        jne     fsd_out
        inc     esi
        jmp     short fsd_lp
fsd_out:
        pop     eax
        ret

; ff_isdel - ZF if AL is one of the separators in ff_delims
ff_isdel:
        push    esi
        push    edx
        mov     esi,OFFSET ff_delims
fid_lp:
        mov     dl,[esi]
        or      dl,dl
        jz      fid_no
        cmp     al,dl
        je      fid_out                 ; ZF set: it is one
        inc     esi
        jmp     short fid_lp
fid_no:
        or      esi,esi                 ; ZF clear: it is not
fid_out:
        pop     edx
        pop     esi
        ret

;=====================================================================
; CTTY device - move the console onto another character device.
;
; IT IS THREE DOS CALLS AND NOT A KERNEL FEATURE, which is worth
; saying because the entry for it sat on the roadmap for eight
; releases waiting for "a device chain to move the console onto".  The
; chain arrived in v0.37 and the calls this needs were all already
; here: open the device, force the handle over 0, 1 and 2 with 46h,
; close the original.  Standard input and output are SFT slots, every
; console call in the kernel goes through those slots, and pointing
; them somewhere else is the whole of what CTTY has ever meant.  Real
; DOS does exactly this (tcmd2b.asm) and reaches handle 0 the long way
; round, by closing it and dupping three times so that the new handle
; lands in the lowest free slot - which does not work here, because
; PM-DOS refuses to close the standard handles.  46h says the same
; thing without the trick.
;
; 44h IS THE CHECK THAT MATTERS.  A name that is not a device opens
; perfectly well as a file, and "CTTY AUTOEXEC.BAT" would otherwise
; install a file as the console - reads coming back as the text of the
; batch file and writes appending to it.  Bit 7 of DL is the answer
; and real DOS tests the same bit in the same place.
;
; The name is REMEMBERED, and that is not bookkeeping either: two
; places in this interpreter open the console by name rather than
; using handle 1 - the ^C handler, putting the standard handles back
; after a redirection, and the critical-error prompt, which
; deliberately asks its question somewhere that cannot have been
; redirected into the file on the disk that just failed.  Both used
; to open the literal string "CON", and after a CTTY both would have
; quietly moved the console back.
;
; WHAT IT CANNOT DO IS UNDO ITSELF.  "CTTY AUX" on a machine with
; nothing on the serial port leaves an interpreter reading a device
; that has nothing to say, and there is no keyboard left to type
; "CTTY CON" on.  That is authentic - it is the oldest joke in DOS -
; and the answer is the same as it was in 1983.
;=====================================================================
cmd_ctty:
        call    need_arg
        jc      ctty_out
        call    next_arg
        jc      ctty_out

        mov     edx,OFFSET tok_buf
        mov     ax,3D02h                ; read and write, as a console is
        int     21h
        jc      ctty_nodev
        movzx   eax,ax
        mov     [ctty_hdl],eax

        mov     ebx,eax
        mov     ax,4400h
        int     21h
        jc      ctty_shut
        test    dl,80h                  ; a character device, or a file
        jz      ctty_shut               ; wearing the name of one?

        xor     ecx,ecx                 ; stdin
        call    ctty_force
        mov     ecx,1                   ; stdout
        call    ctty_force
        mov     ecx,2                   ; and stderr, which real DOS moves
        call    ctty_force              ; too: a console you cannot be told
                                        ; anything on is half a console
        mov     ebx,[ctty_hdl]
        mov     ah,3Eh
        int     21h

        mov     esi,OFFSET tok_buf      ; and remember what it is called
        mov     edi,OFFSET ctty_name
        mov     ecx,CTTYNAME_MAX - 1
ctty_cp:
        mov     al,[esi]
        or      al,al
        jz      ctty_cpend
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     ctty_cp
ctty_cpend:
        mov     byte ptr [edi],0
ctty_out:
        ret

ctty_shut:
        mov     ebx,[ctty_hdl]
        mov     ah,3Eh
        int     21h
ctty_nodev:
        mov     edx,OFFSET msg_noctty
        call    print
        ret

;--- ctty_force - the opened handle over standard handle ECX ---------
ctty_force:
        push    ecx
        mov     ebx,[ctty_hdl]
        mov     ah,46h
        int     21h
        pop     ecx
        ret

;=====================================================================
; Shared parsing helpers
;=====================================================================

;---------------------------------------------------------------------
; kw_match - if the word at ESI is the ASCIIZ uppercase keyword at
;            EDI, step ESI past it and any blanks and return CF clear.
;            Otherwise ESI is left exactly where it was, CF set.
;---------------------------------------------------------------------
kw_match:
        push    eax
        push    esi
        push    edi
km_lp:
        mov     ah,[edi]
        or      ah,ah
        jz      km_end
        mov     al,[esi]
        call    upcase
        cmp     al,ah
        jne     km_no
        inc     esi
        inc     edi
        jmp     short km_lp
km_end:
        mov     al,[esi]                ; it has to be a whole word
        or      al,al
        jz      km_yes
        cmp     al,' '
        je      km_yes
        cmp     al,9
        je      km_yes
        cmp     al,'('                  ; ...and "IN(" is a whole word
        je      km_yes
        cmp     al,':'                  ; ...as is "CON:"
        je      km_yes
        jmp     short km_no
km_yes:
        call    skip_blanks
        pop     edi
        add     esp,4                   ; keep the advanced ESI
        pop     eax
        clc
        ret
km_no:
        pop     edi
        pop     esi
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; next_arg - copy the next blank-delimited word at ESI into tok_buf
;            verbatim, ESI past it.  CF set if there is none.
;---------------------------------------------------------------------
next_arg:
        call    skip_blanks
        cmp     byte ptr [esi],0
        je      nx_fail
        push    ecx
        mov     edi,OFFSET tok_buf
        xor     ecx,ecx
        cmp     byte ptr [adv_parse],0  ; a quoted word is one word,
        je      nx_cp                   ; quotes shed on the way in
        cmp     byte ptr [esi],'"'
        jne     nx_cp
        inc     esi
nx_qcp:
        mov     al,[esi]
        or      al,al
        jz      nx_end                  ; unterminated: to the end
        cmp     al,'"'
        je      nx_qend
        cmp     ecx,TOK_MAX - 1
        jae     nx_qsk
        mov     [edi],al
        inc     edi
        inc     ecx
nx_qsk:
        inc     esi
        jmp     short nx_qcp
nx_qend:
        inc     esi
        jmp     short nx_end
nx_cp:
        mov     al,[esi]
        or      al,al
        jz      nx_end
        cmp     al,' '
        je      nx_end
        cmp     al,9
        je      nx_end
        cmp     ecx,TOK_MAX - 1
        jae     nx_skip
        mov     [edi],al
        inc     edi
        inc     ecx
nx_skip:
        inc     esi
        jmp     short nx_cp
nx_end:
        mov     byte ptr [edi],0
        pop     ecx
        call    skip_blanks
        clc
        ret
nx_fail:
        stc
        ret


;=====================================================================
; Batch data
;=====================================================================
bat_cur     dd  0                       ; -> active frame, 0 = keyboard
bat_lvl     dd  0
bl_path     dd  0
bl_handle   dd  0
bl_name0    dd  0                       ; %0 override, set by the caller
bsa_lim     dd  0
be_resume   dd  0
fr_lim      dd  0

errorlevel  db  0                       ; last external command's code
perm_flag   db  0                       ; started with /P
c_once      db  0                       ; /C: 1 = its command is waiting,
                                        ; 2 = it has run, so leave
c_line      db  CMDLINE_MAX + 2 dup(0)  ; ...and what it is
call_flag   db  0
if_not      db  0
for_var     db  0
for_busy    db  0
            db  0                       ; pad

lit_not     db  'NOT',0
lit_errlvl  db  'ERRORLEVEL',0
lit_exist   db  'EXIST',0
lit_in      db  'IN',0
lit_do      db  'DO',0
lit_con     db  'CON',0

; WHERE THE CONSOLE IS, as a name, because two places reopen it by
; name rather than by handle.  CTTY writes here; everything else reads
; it and would have read the literal 'CON' above.  It goes back to CON
; when the interpreter is reloaded, which is the right recovery: a
; fresh shell has fresh standard handles.
CTTYNAME_MAX    EQU     16
ctty_name   db  'CON',0
            db  (CTTYNAME_MAX - 4) dup (0)
ctty_hdl    dd  0
lit_bat     db  '.BAT',0
lit_cmd     db  '.CMD',0
lit_echodot db  'ECHO.',0
null_str    db  0
autoexec_name db '\AUTOEXEC.BAT',0

msg_syntax  db  'Syntax error',13,10,'$'
msg_nolabel2 db 'Label not found',13,10,'$'
msg_batdeep db  'Batch files nested too deeply',13,10,'$'
msg_batmiss db  'Batch file missing',13,10,'$'
msg_fornest db  'FOR cannot be nested',13,10,'$'
msg_noctty  db  'Invalid device',13,10,'$'

bat_raw     db  BAT_CHUNK dup(0)
line_buf    db  BAT_LINE dup(0)
exp_buf     db  BAT_LINE dup(0)
be_namebuf  db  33 dup(0)
tok_buf     db  TOK_MAX dup(0)
goto_lbl    db  GOTOLBL_MAX dup(0)
if_s1       db  TOK_MAX dup(0)
if_s2       db  TOK_MAX dup(0)
for_list    db  FORLIST_MAX dup(0)
for_body    db  FORBODY_MAX dup(0)
for_line    db  FORLINE_MAX dup(0)
for_names   db  FORNAMES_MAX dup(0)

; The variables for_subst fills in: how many, and where each value is.
; An ordinary FOR has one; FOR /F one per token it was asked for.
for_nv      dd  0
for_vp      dd  FFQ_MAXTOK + 1 dup(0)

; FOR /F's options and working state
ff_eol      db  0                       ; eol=, 0 = none
ff_backq    db  0                       ; usebackq
ff_star     db  0                       ; tokens= ended with '*'
ff_empty    db  0                       ; the value of a token not there
ff_skip     dd  0                       ; skip=, counting down
ff_tmask    dd  0                       ; tokens=, bit n for token n
ff_tmax     dd  0                       ; ...and the highest of them
ff_nv       dd  0                       ; variables they make
ff_vp       dd  0                       ; where the next value goes
ff_hdl      dd  0
ff_size     dd  0
ff_blk      dd  0
ff_cmd      dd  0
ff_delims   db  32 dup(0)               ; delims=, NUL-terminated
ff_vbuf     db  FFQ_VBUF dup(0)
ff_tmp      db  FFQ_TMPMAX dup(0)
ff_cline    db  FFCLINE_MAX dup(0)

kwf_eol     db  'EOL=',0
kwf_skip    db  'SKIP=',0
kwf_delims  db  'DELIMS=',0
kwf_tokens  db  'TOKENS=',0
kwf_backq   db  'USEBACKQ',0
ffn_temp    db  'TEMP',0
ffn_command db  'CMD.EXE',0
ffn_slashc  db  ' /C ',0
ffn_to      db  ' > ',0
ffn_tmpname db  '%FORF.$$$',0
msg_ffnone  db  'File not found - ','$'
msg_ffmem   db  'Insufficient memory',13,10,'$'
bat_stk     db  BAT_MAX * BF_LEN dup(0)
