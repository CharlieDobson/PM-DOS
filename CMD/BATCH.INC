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
BF_HANDLE       EQU     0               ; dd - open file handle
BF_POS          EQU     4               ; dd - offset of the NEXT line
BF_BASE         EQU     8               ; dd - which argument %0 is now
BF_ARGV         EQU     12              ; BF_ARGC dd, 0 = absent
BF_TEXT         EQU     BF_ARGV + BF_ARGC * 4
BF_CMD          EQU     BF_TEXT + BAT_ATEXT     ; dd - 1 = a .CMD file
BF_LEN          EQU     BF_CMD + 4

;=====================================================================
; cmd_startup - read our own command tail and act on it.
;
; The kernel starts us with "/P", the same marker CONFIG.SYS's SHELL=
; line used, and it means two things: run AUTOEXEC.BAT, and refuse to
; EXIT because there is nothing underneath us to go back to.
;=====================================================================
cmd_startup:
        mov     esi,SHELL_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
cs_lp:
        or      ecx,ecx
        jz      cs_done
        cmp     byte ptr [esi],'/'
        jne     cs_next
        mov     al,[esi+1]
        call    upcase
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
        mov     eax,[bl_handle]
        mov     [edi+BF_HANDLE],eax
        mov     dword ptr [edi+BF_POS],0

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
        mov     ebx,[esi+BF_HANDLE]
        mov     ah,3Eh
        int     21h
        mov     eax,[bat_lvl]
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
        mov     ebx,[esi+BF_HANDLE]
        mov     ecx,[esi+BF_POS]
        mov     ax,4200h
        int     21h
        jc      brl_eof

        mov     ebx,[esi+BF_HANDLE]
        mov     ecx,BAT_CHUNK
        mov     edx,OFFSET bat_raw
        mov     ah,3Fh
        int     21h
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

brl_eof:
        mov     byte ptr [line_buf],0
        popad
        stc
        ret

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
        mov     esi,OFFSET for_names
fr_run:
        cmp     byte ptr [esi],0
        je      fr_done
        push    esi
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
; for_subst - for_line = for_body with every %<for_var> replaced by
;             the item at ESI.  Rebuilt per iteration because running
;             the body is free to chew up the copy.
;---------------------------------------------------------------------
for_subst:
        pushad
        mov     ebx,esi
        mov     esi,OFFSET for_body
        mov     edi,OFFSET for_line
fs_lp:
        mov     al,[esi]
        or      al,al
        jz      fs_end
        cmp     al,'%'
        jne     fs_lit
        mov     ah,[esi+1]
        cmp     ah,[for_var]            ; case sensitive, as MS-DOS has it
        jne     fs_lit
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
; CTTY device
;
; Honest about its limits: CON is where console I/O already goes, so
; "CTTY CON" is correct and does nothing.  Anything else means moving
; the console onto another device, and PM-DOS has no device chain to
; move it onto yet - so it says so rather than pretending.
;=====================================================================
cmd_ctty:
        call    need_arg
        jc      ctty_out
        call    next_arg
        jc      ctty_out
        mov     esi,OFFSET tok_buf
        mov     edi,OFFSET lit_con
        call    kw_match                ; whole-word, case insensitive
        jc      ctty_nodev
ctty_out:
        ret
ctty_nodev:
        mov     edx,OFFSET msg_noctty
        call    print
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
lit_bat     db  '.BAT',0
lit_cmd     db  '.CMD',0
lit_echodot db  'ECHO.',0
null_str    db  0
autoexec_name db '\AUTOEXEC.BAT',0

msg_syntax  db  'Syntax error',13,10,'$'
msg_nolabel2 db 'Label not found',13,10,'$'
msg_batdeep db  'Batch files nested too deeply',13,10,'$'
msg_fornest db  'FOR cannot be nested',13,10,'$'
msg_noctty  db  'Console can only be CON: no device chain yet',13,10,'$'

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
bat_stk     db  BAT_MAX * BF_LEN dup(0)
