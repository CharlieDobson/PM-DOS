;=====================================================================
; COMMAND.ASM - COMMAND.COM, the PM-DOS command interpreter
;
; A D32 executable (it is really a COMMAND.EXE; the .COM name is kept
; for compatibility, exactly like MS-DOS did in reverse).  Loaded by
; PMDOS into the shell slot at SHELL_BASE and re-loaded whenever it
; terminates.
;
; Talks to the system exclusively through INT 21h - if a command
; cannot be written against the DOS API, the API is what gets fixed.
;
; Internal commands:
;   BREAK CALL CD CHDIR CLS COPY CTTY DATE DEL DIR ECHO ERASE EXIT
;   FOR GOTO IF LH LOADHIGH MD MKDIR PATH PAUSE PROMPT RD REM REN
;   RENAME RMDIR SET SHIFT TIME TYPE VER VERIFY VOL  plus  A: / B:  drive
;   changes.  CHOICE and MEM are deliberately NOT here: both are
;   externals in MS-DOS and externals here - CMD\CHOICE.ASM and
;   CMD\MEM.ASM.
; Anything else is handed to INT 21h/4Bh EXEC as NAME.COM / NAME.EXE,
; or interpreted as NAME.BAT - see BATCH.INC.
;
; Linked with:  wlink form raw bin option offset=0x141000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

CMDLINE_MAX     EQU     120
ENV_SIZE        EQU     1024
QVERB_MAX       EQU     128             ; a quoted verb (a long path)
PATHDIR_MAX     EQU     128             ; one PATH= directory
COPYBUF_SIZE    EQU     8192

; Command switches, as sw_scan reports them: one bit per letter, bit 0
; for /A up to bit 25 for /Z.  Up here rather than beside sw_scan
; because wasm cannot size a forward-referenced EQU used as an
; immediate - the uses come hundreds of lines before the definition.
SW_A            EQU     00000001h       ; bit 'A' - 'A'
SW_B            EQU     00000002h       ; bit 'B' - 'A'
SW_O            EQU     00004000h       ; bit 'O' - 'A'
SW_P            EQU     00008000h       ; bit 'P' - 'A'
SW_S            EQU     00040000h       ; bit 'S' - 'A'
SW_V            EQU     00200000h       ; bit 'V' - 'A'
SW_W            EQU     00400000h       ; bit 'W' - 'A'

SW_ARGMAX       EQU     8               ; per-switch argument text

DIR_PAGE        EQU     SCR_ROWS - 2    ; listing lines between /P stops
DIR_MAXKEY      EQU     8               ; sort keys in one /O
DIR_MAXDEPTH    EQU     32              ; how far /S will descend
DIR_SUBNAME     EQU     16              ; one subdirectory's 8.3 name

; COPY's /A and /B, per item.  UNSET is a third state and has to be:
; the DEFAULT depends on whether the whole command turned out to be a
; concatenation, which is not known until the line has been read.
CPM_ASCII       EQU     0
CPM_BINARY      EQU     1
CPM_UNSET       EQU     0FFh

; /O's collected entry.  A find record is 318 bytes and a directory
; can hold thousands of them, so what is kept is the six fields the
; listing actually prints and nothing else - and printing puts them
; back into a find record, so that the two code paths that draw a row
; stay one piece of code.
DR_ATTR         EQU     0               ; db
DR_TIME         EQU     2               ; dw
DR_DATE         EQU     4               ; dw
DR_SIZE         EQU     8               ; dd
DR_NAME         EQU     12              ; 13 bytes - the 8.3 alias
DR_LONG         EQU     28              ; 132 bytes - the long name
DR_LEN          EQU     160

_CMD    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_CMD, DS:_CMD, ES:_CMD, SS:_CMD
        ORG     0

;=====================================================================
; D32 header - the file is the memory image
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET cmd_entry>,<0>,32768

;=====================================================================
; Entry
;=====================================================================
; NO BANNER.  The interpreter says nothing on load - which matters
; more than it looks, because cmd_entry runs again on every reload of
; the shell slot, so a banner is printed on every EXIT as well as at
; boot.  VER still reports the version, and that is where a version
; belongs.
cmd_entry:
        mov     edx,OFFSET dta
        mov     ah,1Ah                  ; our DTA
        int     21h

        ; ^C is OURS: hook INT 23h so a break lands back at the
        ; prompt (or terminates a running child) instead of taking
        ; the whole interpreter down through the kernel's default.
        ; The saved ESP is the longjmp target - the entry-level
        ; stack IS the main-loop-level stack.
        mov     [break_esp],esp
        mov     edx,OFFSET int23_hand
        mov     ax,2523h
        int     21h

        ; ...and so is the Abort, Retry, Fail? prompt.  The kernel's
        ; own INT 24h answers "fail" silently, which is no use to a
        ; person standing in front of an empty floppy drive.
        mov     edx,OFFSET int24_hand
        mov     ax,2524h
        int     21h

        call    inherit_env             ; what CONFIG.SYS SET= left us
        call    set_comspec             ; before AUTOEXEC can read it
        call    cmd_startup             ; /P, /C and AUTOEXEC.BAT

;=====================================================================
; The read-eval loop.  Getting the line and running it are separate so
; that a batch file can be the source just as easily as the keyboard,
; and so that IF and FOR can feed a line straight back in.
;=====================================================================
main_loop:
        call    get_line
        jc      main_loop               ; blank, label, or batch EOF
        call    exec_line
        jmp     main_loop

;---------------------------------------------------------------------
; inherit_env - seed our environment block from the one we were given.
;
; A PROGRAM'S ENVIRONMENT ARRIVES BEHIND ITS PSP and the interpreter
; is a program like any other: PSP+2Ch holds the offset of a run of
; ASCIIZ "NAME=value" strings ended by an empty one.  The kernel fills
; it in from CONFIG.SYS's SET= lines, so PATH and TEMP set before the
; shell existed are here waiting when it starts.
;
; It runs BEFORE set_comspec on purpose.  COMSPEC is what this
; interpreter actually is, and a stale one inherited from anywhere
; would name a program that may not be running - so it is written
; afterwards and wins.
;
; Nothing here fails loudly.  An empty or absent block (no SET= line,
; which is the usual case) leaves the environment exactly as it was.
;---------------------------------------------------------------------
inherit_env:
        push    eax
        push    ecx
        push    esi
        push    edi
        ; ds: IS LOAD-BEARING AND ITS ABSENCE COST AN AFTERNOON.  Without
        ; the override wasm makes this a reference INTO _CMD, and wlink's
        ; "option offset=0x2000" then biases it - so the read lands two
        ; pages inside our own image, finds whatever is there, and the
        ; environment silently does not exist.  R3TEST.ASM writes every
        ; one of its PSP reads the same way for the same reason.
        mov     esi,ds:[SLOT_PSP + PSP_ENV]
        or      esi,esi
        jz      ie_out                  ; no environment: nothing to take
ie_str:
        cmp     byte ptr [esi],0
        je      ie_out                  ; the empty string ends the block

        ; env_set wants one ASCIIZ string and this block is a run of
        ; them, so each is copied out before it is offered - env_set
        ; writes into the block it is scanning and must not be handed
        ; a pointer into another one.
        mov     edi,OFFSET set_scratch
        mov     ecx,SCRATCH_MAX - 1
ie_cp:
        mov     al,[esi]
        mov     [edi],al
        inc     esi
        or      al,al
        jz      ie_cpend
        inc     edi
        dec     ecx
        jnz     ie_cp
        mov     byte ptr [edi],0        ; a variable longer than the buffer
ie_skip:                                ; is not silently half-taken: the
        mov     al,[esi]                ; rest of it is stepped over and the
        inc     esi                     ; entry is dropped
        or      al,al
        jnz     ie_skip
        jmp     short ie_str
ie_cpend:

        push    esi
        mov     esi,OFFSET set_scratch
        call    env_set
        pop     esi
        jmp     short ie_str
ie_out:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; set_comspec - COMSPEC = the interpreter that is actually running.
;
; A CONFIG.SYS SHELL= line can name any interpreter, so COMMAND cannot
; assume its own path: it asks the kernel (21h/F1h AL=3), which is
; what loaded it.  Set before AUTOEXEC.BAT runs, because %COMSPEC% in
; an AUTOEXEC is the whole point - and it is how a program shells out
; to a copy of the interpreter.
;---------------------------------------------------------------------
set_comspec:
        mov     esi,OFFSET lit_comspec  ; "COMSPEC=" into the scratch
        mov     edi,OFFSET set_scratch
        mov     ecx,COMSPEC_PFX
sc_pfx:
        lodsb
        stosb
        loop    sc_pfx
        mov     edx,edi                 ; the kernel appends the path
        mov     ax,0F103h
        int     21h
        jc      sc_out                  ; an older kernel: no COMSPEC
        cmp     byte ptr [edi],0        ; nor an empty one
        je      sc_out
        mov     esi,OFFSET set_scratch
        call    env_set
sc_out:
        ret

;---------------------------------------------------------------------
; get_line - the next command to run.  CF clear: ESI -> ASCIIZ text.
;---------------------------------------------------------------------
get_line:
        cmp     dword ptr [bat_cur],0
        jne     bat_getline             ; a batch file is running
        mov     byte ptr [adv_parse],1  ; the console speaks quotes
        call    put_prompt

        mov     esi,OFFSET cmd_buf
        mov     byte ptr [esi],CMDLINE_MAX
        mov     edx,esi
        mov     ah,0Ah
        int     21h
        call    put_crlf

        movzx   ecx,byte ptr [esi+1]
        mov     byte ptr [esi+ecx+2],0  ; CR -> NUL
        lea     esi,[esi+2]
        clc
        ret

;=====================================================================
; int23_hand - the INT 23h handler.  Two very different ^Cs:
;
; A child program running: the break belongs to IT.  Terminate it
; with 4Ch right from here - the kernel unwinds into our own EXEC
; frame and try_exec returns normally, which is precisely what the
; default kernel handler would have done.
;
; Our own command, batch file or prompt: longjmp back to the main
; loop.  The stack is reset wholesale (both interrupt frames and
; whatever parsing was underway are abandoned - nothing in them owns
; anything), every batch level is popped (a ^C ends a batch job),
; and the standard handles are forced back onto CON, because a break
; in the middle of a redirected command would otherwise leave a FILE
; installed as stdout.  Forcing CON over the slots also closes -
; and flushes - that file on the way.
;=====================================================================
int23_hand:
        cmp     byte ptr [in_child],0
        je      cmd_gohome
        mov     ax,4C00h                ; the child dies; we come back
        int     21h                     ; through our own EXEC call

;---------------------------------------------------------------------
; cmd_gohome - abandon whatever was running and come up at the prompt.
;
; Both a ^C and an Abort at the critical-error prompt end here, and
; the first thing either of them does is tell the kernel that the
; handler it called is not coming back.  The kernel holds INT 24h
; locked against itself for the length of a handler, and a longjmp
; would carry that lock off with it - after which every critical
; error would quietly answer itself "fail" forever.  For a plain ^C
; the call is a no-op, which is the cheapest kind of correctness.
;---------------------------------------------------------------------
cmd_gohome:
        mov     ax,0F105h
        int     21h

        mov     esp,[break_esp]
        sti                             ; the gate cleared IF; nothing
                                        ; below IRETs to restore it
        call    bat_popall
        mov     byte ptr [for_busy],0
        mov     byte ptr [call_flag],0

        mov     edx,OFFSET ctty_name    ; consoles back over 0 and 1 - and
                                        ; the CONSOLE, which is not always
                                        ; CON once CTTY has been used
        mov     ax,3D02h
        int     21h
        jc      cb_prompt               ; no CON?  soldier on
        movzx   eax,ax
        mov     [cb_hdl],eax
        mov     ebx,eax
        xor     ecx,ecx
        mov     ah,46h
        int     21h
        mov     ebx,[cb_hdl]
        mov     ecx,1
        mov     ah,46h
        int     21h
        mov     ebx,[cb_hdl]
        mov     ah,3Eh
        int     21h
cb_prompt:
        jmp     main_loop

;=====================================================================
; int24_hand - the INT 24h handler: Abort, Retry, Fail?
;
; AH says what happened and what may be answered, AL is the drive and
; DI the error code; PMDOS.INC has the bit layout.  The message goes
; to STDERR and the answer is read from CON opened for the purpose,
; because the command that hit the error is quite likely to have had
; its output redirected into a file - on the very disk that just
; failed, at that.
;
; Abort is the interesting answer.  With a child program running the
; error belongs to IT: answer 2 and the kernel terminates it, handing
; us back through our own EXEC exactly as a ^C does.  For our own
; commands we go home ourselves instead, because answering 2 would
; have the kernel terminate the SHELL - which means reloading
; COMMAND.COM, and running AUTOEXEC.BAT again, off a disk drive that
; has just proved it cannot be read.
;
; Only the answers AH allows are offered, and Abort is always one of
; them - DOS never made abort optional and neither does this.
;=====================================================================
int24_hand:
        sti                             ; the gate cleared IF and the
                                        ; keyboard is about to be asked
        mov     [ce_flags],ah
        mov     [ce_drive],al
        mov     [ce_code],di

;--- the message starts on a line of its own.  Whatever was running
;--- when the disk gave out was most likely half way through a line
;--- of its own output, and " Volume in drive is Not ready reading
;--- drive B" is not a sentence.
        mov     ax,0F001h               ; where is the cursor?
        int     21h
        or      dl,dl
        jz      ce_atcol0
        mov     esi,OFFSET ce_crlf
        call    ce_puts
ce_atcol0:

;--- "Not ready reading drive A" -------------------------------------
        movzx   eax,byte ptr [ce_code]
        cmp     al,CERR_MAXCODE
        jbe     ce_known
        mov     al,CERR_GENERAL
ce_known:
        mov     esi,dword ptr [ce_msgtbl+eax*4]
        call    ce_puts
        test    byte ptr [ce_flags],CE_CHARDEV
        jnz     ce_nodrive              ; a device has no drive letter
        mov     esi,OFFSET ce_reading
        test    byte ptr [ce_flags],CE_WRITE
        jz      ce_dir
        mov     esi,OFFSET ce_writing
ce_dir:
        call    ce_puts
        mov     al,[ce_drive]
        add     al,'A'
        mov     [ce_letter],al
        mov     esi,OFFSET ce_letter
        call    ce_puts
ce_nodrive:
        mov     esi,OFFSET ce_crlf
        call    ce_puts

;--- The console, for the answer.  No console, no question: fail and
;--- get on.  By NAME and not through handle 2, because the command
;--- that hit the error has very likely had its output redirected
;--- into a file on the disk that just failed - and by the name CTTY
;--- last set, because after a CTTY the console is not CON.
        mov     edx,OFFSET ctty_name
        mov     ax,3D00h
        int     21h
        jc      ce_nocon
        movzx   eax,ax
        mov     [ce_hdl],eax

;--- "Abort, Retry, Fail? " ------------------------------------------
ce_ask:
        mov     esi,OFFSET ce_abort
        call    ce_puts
        test    byte ptr [ce_flags],CE_RETRY_OK
        jz      ce_p2
        mov     esi,OFFSET ce_retry
        call    ce_puts
ce_p2:
        test    byte ptr [ce_flags],CE_IGNORE_OK
        jz      ce_p3
        mov     esi,OFFSET ce_ignore
        call    ce_puts
ce_p3:
        test    byte ptr [ce_flags],CE_FAIL_OK
        jz      ce_p4
        mov     esi,OFFSET ce_fail
        call    ce_puts
ce_p4:
        mov     esi,OFFSET ce_query
        call    ce_puts

;--- ...and it is asked ONCE.  A key that means nothing here is read
;--- again in silence, which is what DOS did: reprinting the question
;--- after every stray keystroke turns a leaned-on keyboard into a
;--- screenful of prompts.
ce_key1:
        mov     ebx,[ce_hdl]
        mov     ecx,1
        mov     edx,OFFSET ce_key
        mov     ah,3Fh
        int     21h
        jc      ce_badkey
        or      eax,eax
        jz      ce_badkey               ; CON at end of file: no answer
        mov     al,[ce_key]
        call    upcase
        mov     ah,[ce_flags]
        cmp     al,'A'                  ; abort is never disallowed
        jne     ce_notA
        mov     al,CERR_ABORT
        jmp     short ce_answer
ce_notA:
        cmp     al,'R'
        jne     ce_notR
        test    ah,CE_RETRY_OK
        jz      ce_key1
        mov     al,CERR_RETRY
        jmp     short ce_answer
ce_notR:
        cmp     al,'I'
        jne     ce_notI
        test    ah,CE_IGNORE_OK
        jz      ce_key1
        mov     al,CERR_IGNORE
        jmp     short ce_answer
ce_notI:
        cmp     al,'F'
        jne     ce_key1
        test    ah,CE_FAIL_OK
        jz      ce_key1
        mov     al,CERR_FAIL

ce_answer:
        mov     [ce_resp],al
        mov     esi,OFFSET ce_crlf      ; a device read echoes the key
        call    ce_puts                 ; itself; the line break is ours
        mov     ebx,[ce_hdl]
        mov     ah,3Eh
        int     21h
        cmp     byte ptr [ce_resp],CERR_ABORT
        je      ce_aborted
        movzx   eax,byte ptr [ce_resp]
        iretd

ce_badkey:
        mov     ebx,[ce_hdl]
        mov     ah,3Eh
        int     21h
ce_nocon:
        mov     eax,CERR_FAIL
        iretd

;--- Abort.  Whose program is it? ------------------------------------
ce_aborted:
        cmp     byte ptr [in_child],0
        je      ce_ourown
        mov     eax,CERR_ABORT          ; the kernel ends the child and
        iretd                           ; try_exec returns as usual
ce_ourown:
        jmp     cmd_gohome              ; our own command: straight
                                        ; back to the prompt

;---------------------------------------------------------------------
; ce_puts - the ASCIIZ string at ESI to STDERR, which redirection
;           never touches.  Preserves everything.
;---------------------------------------------------------------------
ce_puts:
        pushad
        mov     edi,esi
        xor     ecx,ecx
cep_len:
        cmp     byte ptr [edi],0
        je      cep_go
        inc     edi
        inc     ecx
        jmp     short cep_len
cep_go:
        jecxz   cep_out
        mov     edx,esi
        mov     ebx,2
        mov     ah,40h
        int     21h
cep_out:
        popad
        ret

ce_msgtbl:
        dd      OFFSET ce_m00, OFFSET ce_m01, OFFSET ce_m02, OFFSET ce_m03
        dd      OFFSET ce_m04, OFFSET ce_m05, OFFSET ce_m06, OFFSET ce_m07
        dd      OFFSET ce_m08, OFFSET ce_m09, OFFSET ce_m0A, OFFSET ce_m0B
        dd      OFFSET ce_m0C

ce_m00      db  'Write protect error',0
ce_m01      db  'Invalid unit',0
ce_m02      db  'Not ready',0
ce_m03      db  'Unknown command',0
ce_m04      db  'Data error',0
ce_m05      db  'Bad request structure length',0
ce_m06      db  'Seek error',0
ce_m07      db  'Invalid media type',0
ce_m08      db  'Sector not found',0
ce_m09      db  'Printer out of paper',0
ce_m0A      db  'Write fault',0
ce_m0B      db  'Read fault',0
ce_m0C      db  'General failure',0

ce_reading  db  ' reading drive ',0
ce_writing  db  ' writing drive ',0
ce_abort    db  'Abort',0
ce_retry    db  ', Retry',0
ce_ignore   db  ', Ignore',0
ce_fail     db  ', Fail',0
ce_query    db  '? ',0
ce_crlf     db  13,10,0
ce_letter   db  0,0
ce_key      db  0,0

ce_flags    db  0
ce_drive    db  0
ce_resp     db  0
            db  0
ce_code     dw  0
            dw  0
ce_hdl      dd  0

;=====================================================================
; exec_core - run the single command at ESI.  Callers go through
; exec_line (REDIR.INC), which peels < > >> and | off the line first;
; by the time control gets here the line is plain.
;
; Re-entrant as far as IF, FOR and CALL need: the parse buffers are
; only live between here and the handler's first instruction, and
; anything a handler wants to survive a nested command it keeps on
; its own stack.
;=====================================================================
exec_core:
        call    skip_blanks
        cmp     byte ptr [esi],0
        je      el_done
        cmp     byte ptr [esi],'@'      ; echo suppression, already
        jne     el_go                   ; acted on by bat_getline
        inc     esi
        call    skip_blanks
        cmp     byte ptr [esi],0
        je      el_done
el_go:

;--- "ECHO." and "ECHO.text" -----------------------------------------
; Caught here, before the verb is uppercased, because the whole point
; of the form is to echo text that has no space to separate it - and
; uppercasing it on the way past would be no kindness.
        push    esi
        mov     edi,OFFSET lit_echodot
        mov     ecx,5
el_ed:
        mov     al,[esi]
        call    upcase
        cmp     al,[edi]
        jne     el_ed_no
        inc     esi
        inc     edi
        loop    el_ed
        pop     eax                     ; drop the saved start
        mov     [arg_ptr],esi
        call    cmd_echodot
        ret
el_ed_no:
        pop     esi

;--- a quoted verb: "C:\Some Dir\prog" args --------------------------
; The quoted span is a program path, taken as typed - long names keep
; their case, and the kernel matches case-insensitively anyway.  An
; internal command is never quoted (quotes make it a FILE, as NT's
; shell had it), so the table and the drive-change forms are skipped.
; Advanced parsing only: to a .BAT file a quote is just a character.
        mov     byte ptr [verb_quoted],0
        cmp     byte ptr [adv_parse],0
        je      vb_plain
        cmp     byte ptr [esi],'"'
        jne     vb_plain
        inc     esi
        mov     edi,OFFSET qverb_buf
        xor     ecx,ecx
qvb_lp:
        mov     al,[esi]
        or      al,al
        jz      qvb_end
        cmp     al,'"'
        je      qvb_close
        cmp     ecx,QVERB_MAX - 1
        jae     qvb_skip
        mov     [edi],al
        inc     edi
        inc     ecx
qvb_skip:
        inc     esi
        jmp     short qvb_lp
qvb_close:
        inc     esi                     ; past the closing quote
qvb_end:
        mov     byte ptr [edi],0
        or      ecx,ecx
        jz      bad_command             ; a bare "" runs nothing
        mov     byte ptr [verb_quoted],1
        mov     [arg_raw],esi
        call    skip_blanks
        mov     [arg_ptr],esi
        jmp     external

vb_plain:
;--- verb -> verb_buf, uppercased ------------------------------------
        mov     edi,OFFSET verb_buf
        xor     ecx,ecx
vb_lp:
        mov     al,[esi]
        or      al,al
        jz      vb_end
        cmp     al,' '
        je      vb_end
        cmp     al,9
        je      vb_end
        cmp     al,'='                  ; separators, not name
        je      vb_end                  ; characters: PATH=C:\DOS is
        cmp     al,';'                  ; the form every AUTOEXEC
        je      vb_end                  ; uses, and PATH; clears it
        cmp     al,','                  ; a comma is a DOS delimiter like
        je      vb_end                  ; any other - which is the whole
                                        ; mechanism behind "DIR," (see
                                        ; cmd_dir), and costs nothing
                                        ; anywhere else: no verb anybody
                                        ; types has a comma in it
        call    upcase
        cmp     ecx,12
        jae     vb_skip
        mov     [edi],al
        inc     edi
        inc     ecx
vb_skip:
        inc     esi
        jmp     short vb_lp
vb_end:
        mov     byte ptr [edi],0
        cmp     byte ptr [esi],'='      ; the separator itself is not
        jne     vb_noeq                 ; part of the argument
        inc     esi
vb_noeq:
        mov     [arg_raw],esi           ; before the blanks are eaten -
        call    skip_blanks             ; ECHO has to keep its indentation
        mov     [arg_ptr],esi           ; the rest of the line

;--- drive change: "A:" or "B:" --------------------------------------
        mov     esi,OFFSET verb_buf
        cmp     byte ptr [esi+1],':'
        jne     not_drive
        cmp     byte ptr [esi+2],0
        jne     not_drive
        mov     dl,[esi]
        sub     dl,'A'
        cmp     dl,4                    ; A: B: C: D:
        jae     bad_command
        mov     bl,dl                   ; remember what we asked for
        mov     ah,0Eh
        int     21h
        mov     ah,19h                  ; did the volume mount?
        int     21h
        cmp     al,bl
        je      drv_ok
        mov     edx,OFFSET msg_baddrv
        call    print
drv_ok:
        ret
not_drive:

;--- internal command table ------------------------------------------
        mov     ebx,OFFSET cmd_table
tbl_lp:
        mov     esi,[ebx]
        or      esi,esi
        jz      external               ; table exhausted
        mov     edi,OFFSET verb_buf
tbl_cmp:
        mov     al,[esi]
        cmp     al,[edi]
        jne     tbl_miss
        or      al,al
        jz      tbl_hit
        inc     esi
        inc     edi
        jmp     short tbl_cmp
tbl_miss:
        add     ebx,8
        jmp     short tbl_lp
tbl_hit:
        call    dword ptr [ebx+4]
el_done:
        ret

bad_command:
        mov     edx,OFFSET msg_bad
        call    print
        ret

;=====================================================================
; External command: try VERB.COM then VERB.EXE via EXEC, with the rest
; of the line as the tail.  A verb with a dot is tried literally.
;=====================================================================
external:
        ; build the DOS-style tail once: len, ' ' + args, 0Dh
        mov     edi,OFFSET tail_buf
        mov     esi,[arg_ptr]
        xor     ecx,ecx
        cmp     byte ptr [esi],0
        je      ext_tail_done
        mov     byte ptr [edi+1],' '
        inc     ecx
ext_tail_cp:
        mov     al,[esi]
        or      al,al
        jz      ext_tail_done
        cmp     ecx,7Eh
        jae     ext_tail_done
        mov     [edi+ecx+1],al
        inc     ecx
        inc     esi
        jmp     short ext_tail_cp
ext_tail_done:
        mov     [edi],cl
        mov     byte ptr [edi+ecx+1],0Dh

        ; which buffer holds the verb?
        mov     esi,OFFSET verb_buf
        cmp     byte ptr [verb_quoted],0
        je      ext_haveverb
        mov     esi,OFFSET qverb_buf
ext_haveverb:
        mov     [verb_src],esi

        ; does the verb already have an extension?  A dot only counts
        ; after the LAST path separator - a dot in a directory name
        ; ("C:\V1.2\TOOL") is not one.
        xor     ebx,ebx
ext_dot:
        mov     al,[esi]
        or      al,al
        jz      ext_dotdone
        cmp     al,'.'
        jne     ext_dsep
        mov     ebx,1
        jmp     short ext_dstep
ext_dsep:
        cmp     al,'\'
        je      ext_dclr
        cmp     al,':'
        jne     ext_dstep
ext_dclr:
        xor     ebx,ebx
ext_dstep:
        inc     esi
        jmp     short ext_dot
ext_dotdone:
        or      ebx,ebx
        jz      ext_search

        ; a name that already says .BAT/.CMD is a batch file to
        ; interpret, not an image to load
        mov     esi,[verb_src]
        call    is_batname
        jc      ext_asis_exec
        mov     edx,[verb_src]
        mov     eax,[verb_src]
        mov     [bl_name0],eax          ; %0 as typed
        call    bat_launch
        jnc     ext_ran
        jmp     bad_command
ext_asis_exec:
        mov     edx,[verb_src]
        call    try_exec
        jnc     ext_ran
        jmp     bad_command

;--- no extension: the current directory first, then every PATH= -----
;--- directory, trying VERB.COM/.EXE/.BAT/.CMD in each (NT's order) --
ext_search:
        mov     edi,OFFSET path_buf     ; empty prefix = current dir
        call    try_exts
        jnc     ext_ran

        ; a verb that carries its own directory is not PATH's business
        mov     esi,[verb_src]
ext_sep:
        mov     al,[esi]
        or      al,al
        jz      ext_path
        cmp     al,'\'
        je      ext_fail
        cmp     al,':'
        je      ext_fail
        inc     esi
        jmp     short ext_sep

ext_path:
        mov     esi,OFFSET name_path
        call    env_find
        jc      ext_fail
ext_pdir:
        cmp     byte ptr [esi],0
        je      ext_fail
        cmp     byte ptr [esi],';'      ; empty elements step over
        jne     ext_pcopy
        inc     esi
        jmp     short ext_pdir
ext_pcopy:
        mov     edi,OFFSET path_buf     ; the directory...
        xor     ecx,ecx
ext_pcp:
        mov     al,[esi]
        or      al,al
        jz      ext_pcend
        cmp     al,';'
        je      ext_pcend
        cmp     ecx,PATHDIR_MAX
        jae     ext_pskip
        mov     [edi],al
        inc     edi
        inc     ecx
ext_pskip:
        inc     esi
        jmp     short ext_pcp
ext_pcend:
        cmp     byte ptr [edi-1],'\'    ; ...with one trailing slash
        je      ext_pnosl
        cmp     byte ptr [edi-1],':'    ; "C:" stays drive-relative
        je      ext_pnosl
        mov     byte ptr [edi],'\'
        inc     edi
ext_pnosl:
        push    esi
        call    try_exts
        pop     esi
        jnc     ext_ran
        jmp     short ext_pdir

ext_fail:
        jmp     bad_command
ext_ran:
        ret

;---------------------------------------------------------------------
; try_exts - path_buf holds a directory prefix ending at EDI (empty
;            for the current directory).  Glue the verb on and try
;            .COM, .EXE, .BAT, .CMD.  CF set if none of them ran.
;---------------------------------------------------------------------
try_exts:
        mov     esi,[verb_src]
tx_vcp:
        mov     al,[esi]
        or      al,al
        jz      tx_vend
        mov     [edi],al
        inc     edi
        inc     esi
        jmp     short tx_vcp
tx_vend:
        mov     [ext_at],edi            ; the dot goes here, four times
        mov     ebx,OFFSET ext_tbl
tx_next:
        mov     esi,[ebx]
        or      esi,esi
        jz      tx_none
        mov     edi,[ext_at]
        mov     ecx,5                   ; ".XXX",0
tx_ecp:
        lodsb
        mov     [edi],al
        inc     edi
        loop    tx_ecp
        cmp     dword ptr [ebx+4],0     ; 0 = EXEC it, 1 = interpret it
        jne     tx_bat
        mov     edx,OFFSET path_buf
        push    ebx                     ; try_exec loads EBX with the
        call    try_exec                ; parameter block - the table
        pop     ebx                     ; cursor does not survive it
        jnc     tx_ran
        jmp     short tx_step
tx_bat:
        mov     edx,OFFSET path_buf
        mov     eax,[verb_src]          ; %0 as typed
        mov     [bl_name0],eax
        push    ebx
        call    bat_launch
        pop     ebx
        jnc     tx_ran
tx_step:
        add     ebx,8
        jmp     short tx_next
tx_none:
        stc
        ret
tx_ran:
        clc
        ret

ext_tbl:
        dd      OFFSET lit_xcom, 0
        dd      OFFSET lit_xexe, 0
        dd      OFFSET lit_bat,  1
        dd      OFFSET lit_cmd,  1
        dd      0, 0
lit_xcom    db  '.COM',0
lit_xexe    db  '.EXE',0

;---------------------------------------------------------------------
; try_exec - EDX -> path.  CF set if it could not be run.
;---------------------------------------------------------------------
try_exec:
        mov     eax,OFFSET tail_buf
        mov     [parm_blk+4],eax
        mov     eax,OFFSET env_blk      ; a 16-bit child is given a copy of
        mov     [parm_blk],eax          ; this below 1MB; a D32 one ignores
        mov     ebx,OFFSET parm_blk     ; it and reads ours through 21h
        mov     byte ptr [in_child],1   ; a ^C now belongs to the child
        mov     ax,4B00h
        int     21h
        mov     byte ptr [in_child],0
        jc      te_out

        ; Latch the exit code.  21h/4Dh clears it as it reads, but a
        ; run of consecutive IF ERRORLEVEL lines - the whole idiom -
        ; must all see the same number, so COMMAND keeps its own copy.
        push    eax
        mov     ah,4Dh
        int     21h
        mov     [errorlevel],al
        pop     eax
        clc
te_out:
        ret

;---------------------------------------------------------------------
; is_batname - CF clear if the ASCIIZ at ESI ends in ".BAT" or ".CMD"
;              (both are batch classes; bat_launch tells them apart)
;---------------------------------------------------------------------
is_batname:
        push    edx
        mov     edx,OFFSET lit_bat
        call    ends_with
        jnc     ib_yes
        mov     edx,OFFSET lit_cmd
        call    ends_with
        jnc     ib_yes
        pop     edx
        stc
        ret
ib_yes:
        pop     edx
        clc
        ret

;---------------------------------------------------------------------
; ends_with - CF clear if the ASCIIZ at ESI ends with the 4-character
;             uppercase suffix at EDX.  Preserves everything.
;---------------------------------------------------------------------
ends_with:
        push    eax
        push    ecx
        push    esi
        push    edi
        mov     edi,esi
ib_end:
        cmp     byte ptr [edi],0
        je      ib_back
        inc     edi
        jmp     short ib_end
ib_back:
        sub     edi,4
        cmp     edi,esi
        jb      ib_no
        mov     esi,edx
        mov     ecx,4
ib_cmp:
        mov     al,[edi]
        call    upcase
        cmp     al,[esi]
        jne     ib_no
        inc     edi
        inc     esi
        loop    ib_cmp
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        clc
        ret
ib_no:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; The find family, LFN first with a classic fallback.  When the
; kernel answers 714Eh with AX=7100h (LFN=OFF in CONFIG.SYS), the
; walk drops to the classic 4Eh/4Fh pair and the result is recast
; into the same find_data record - the long-name field then carries
; the 8.3 text, so DIR's long column simply disappears.  Adaptive
; per call: no startup probe, no flag to go stale.
;
; xfind_first: EDX -> pattern, CL = allowable attributes.
;              Fills find_data.  CF set: nothing found.
;              AX = handle for xfind_next / xfind_close.
; xfind_next:  BX = the handle.  CF set: no more.
; xfind_close: BX = the handle.
;=====================================================================
XFIND_CLASSIC   EQU     0FFFFh

xfind_first:
        mov     edi,OFFSET find_data
        mov     ax,714Eh
        int     21h
        jnc     xf_out
        cmp     ax,7100h                ; unsupported - not "no files"
        jne     xf_none
        push    edx
        push    ecx
        mov     edx,OFFSET dta          ; our own DTA - a child may
        mov     ah,1Ah                  ; have moved the kernel's
        int     21h
        pop     ecx
        pop     edx
        movzx   ecx,cl
        mov     ah,4Eh
        int     21h
        jc      xf_none
        call    xf_recast
        mov     ax,XFIND_CLASSIC
xf_out:
        clc
        ret
xf_none:
        stc
        ret

xfind_next:
        cmp     bx,XFIND_CLASSIC
        je      xn_classic
        movzx   ebx,bx
        mov     edi,OFFSET find_data
        mov     ax,714Fh
        int     21h
        ret                             ; CF is the answer
xn_classic:
        push    edx
        mov     edx,OFFSET dta          ; 4Fh resumes from the DTA -
        mov     ah,1Ah                  ; make sure it is still ours
        int     21h
        pop     edx
        mov     ah,4Fh
        int     21h
        jc      xf_none
        call    xf_recast
        clc
        ret

xfind_close:
        cmp     bx,XFIND_CLASSIC
        je      xc_out                  ; nothing to give back
        movzx   ebx,bx
        mov     ax,71A1h
        int     21h
xc_out:
        ret

; xf_recast - the classic DTA result into the find_data record
xf_recast:
        push    eax
        push    esi
        push    edi
        movzx   eax,byte ptr [dta+15h]  ; attributes
        mov     dword ptr [find_data],eax
        mov     ax,word ptr [dta+16h]   ; time
        mov     word ptr [find_data+14h],ax
        mov     ax,word ptr [dta+18h]   ; date
        mov     word ptr [find_data+16h],ax
        mov     eax,dword ptr [dta+1Ah] ; size
        mov     dword ptr [find_data+20h],eax
        mov     esi,OFFSET dta + 1Eh    ; the 8.3, into BOTH name
        mov     edi,OFFSET find_data + 2Ch      ; fields
xr_cp:
        lodsb
        mov     [edi],al
        mov     [edi+104h],al           ; +130h = +2Ch + 104h
        inc     edi
        or      al,al
        jnz     xr_cp
        pop     edi
        pop     esi
        pop     eax
        ret

;=====================================================================
; DIR [pattern]
;=====================================================================
cmd_dir:
        mov     edx,OFFSET dta     ; a child may have moved it
        mov     ah,1Ah
        int     21h

        ; "DIR," - MS-DOS's undocumented "show me everything".  A comma
        ; where the argument should be means hidden and system files are
        ; listed too, and it survives because it is quicker to type than
        ; /A:HS and because a comma is a delimiter, so the shell hands it
        ; over as an argument in the first place rather than looking for
        ; a command called "DIR,".
        ;
        ; THE COMMA IS THE WHOLE ARGUMENT OR IT IS NOTHING.  It is not a
        ; prefix and it does not combine: "DIR, /P", "DIR, /W" and
        ; "DIR,*.SYS" are all invalid under 6.x, and treating the comma
        ; as a switch that happens to be spelled oddly - which is what a
        ; first version here did - invents a form of the command that no
        ; DOS ever had.  So anything at all after it, bar the trailing
        ; blanks nobody can see, and this is not the form: the comma is
        ; left where it is and goes on to be a pattern that matches
        ; nothing, which is the failure it deserves.
        mov     dword ptr [dir_all],0
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],','
        jne     dir_notall
        inc     esi
        call    skip_blanks             ; a space at the end is not a parameter
        cmp     byte ptr [esi],0
        jne     dir_notall
        mov     [arg_ptr],esi           ; nothing left: list the cwd
        mov     dword ptr [dir_all],ATTR_HIDDEN OR ATTR_SYSTEM
dir_notall:

        mov     esi,[arg_ptr]           ; the switches come out of the
        call    sw_scan                 ; argument before anything reads
        mov     [dir_sw],eax            ; it as a pattern
        mov     eax,[sw_negm]
        mov     [dir_neg],eax
        call    dir_dircmd              ; ...and DIRCMD underneath them
        mov     dword ptr [dir_lines],0
        mov     dword ptr [dir_col],0
        mov     dword ptr [dir_total],0
        mov     dword ptr [dir_tbytes],0
        mov     dword ptr [dir_depth],0
        call    dir_order               ; /O, into a list of sort keys

;--- header: volume label + directory line ---------------------------
; The label is looked for BEFORE a word of the header is printed.  It
; is the first thing DIR touches on the disk, so it is where a dead
; drive announces itself - and half a header followed by "Not ready
; reading drive B" is not a line anybody wants to read.
        call    arg_drive               ; EDX -> "\*.*" on the listed drive
        mov     ecx,ATTR_VOLUME
        mov     ah,4Eh
        int     21h
        jnc     dir_haslabel
        cmp     ax,ERR_BADDRIVE         ; no disk, or no such drive:
        je      dir_baddrv              ; there is nothing to list
        mov     edx,OFFSET msg_vol1
        call    print
        mov     edx,OFFSET msg_nolabel
        call    print
        jmp     short dir_hdr2
dir_baddrv:
        mov     edx,OFFSET msg_baddrv
        call    print
        ret
dir_haslabel:
        mov     edx,OFFSET msg_vol1
        call    print
        mov     esi,OFFSET dta + 1Eh
        call    print_asciiz
dir_hdr2:

;--- work out WHAT is being listed before saying where it is ---------
; The "Directory of" line used to print the listed drive's current
; directory whatever the argument said, so DIR C:\SUB headlined
; C:\<cwd>.  The pattern has to be resolved first, and then the line
; comes out of the pattern rather than out of the cwd.
        mov     dword ptr [dir_count],0
        mov     dword ptr [dir_bytes],0

        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        jne     dir_havearg
        mov     esi,OFFSET pat_all
        jmp     dir_pat

dir_havearg:
        ; "DIR SOMEDIR" means the contents, not the entry: when the
        ; argument has no wildcards and names a directory, list
        ; SOMEDIR\*.* instead.  The probe goes through the long find
        ; so a quoted "DIR "Long Directory"" resolves too.
        call    unquote_arg

        ; "DIR D:" and "DIR PATH\" already SAY directory - glue *.*
        ; straight on, no probing.  No backslash after the colon, so
        ; the drive form lists that drive's CURRENT directory,
        ; exactly as DOS does.
        push    esi
        mov     edi,esi
dir_tail:
        cmp     byte ptr [edi+1],0
        je      dir_lastch
        inc     edi
        jmp     short dir_tail
dir_lastch:
        mov     al,[edi]
        pop     esi
        cmp     al,':'
        je      dir_glue
        cmp     al,'\'
        je      dir_glue
        call    has_wild
        jnc     dir_probe
        jmp     dir_pat
dir_glue:
        push    esi
        mov     edi,OFFSET dir_scratch
dirg_cp:
        lodsb
        or      al,al
        jz      dirg_add
        stosb
        jmp     short dirg_cp
dirg_add:
        mov     byte ptr [edi],'*'
        mov     byte ptr [edi+1],'.'
        mov     byte ptr [edi+2],'*'
        mov     byte ptr [edi+3],0
        pop     esi
        mov     esi,OFFSET dir_scratch
        jmp     dir_pat
dir_probe:
        mov     edx,esi
        mov     ecx,ATTR_DIR            ; CL = allowable
        push    esi
        call    xfind_first
        pop     esi
        jc      dir_pat                 ; nothing there - let it fail below
        push    esi
        mov     bx,ax                   ; the probe handle can go at once
        call    xfind_close
        pop     esi
        test    byte ptr [find_data],ATTR_DIR
        jz      dir_pat
        push    esi
        mov     edi,OFFSET dir_scratch  ; arg + "\*.*"
dirx_cp:
        lodsb
        or      al,al
        jz      dirx_add
        stosb
        jmp     short dirx_cp
dirx_add:
        mov     byte ptr [edi],'\'
        mov     byte ptr [edi+1],'*'
        mov     byte ptr [edi+2],'.'
        mov     byte ptr [edi+3],'*'
        mov     byte ptr [edi+4],0
        pop     esi
        mov     esi,OFFSET dir_scratch

dir_pat:
        ; THE PATTERN IS SPLIT ONCE, HERE, and everything below works
        ; from the two halves rather than from the text as typed.  That
        ; is what /S needs: "list *.TXT in this directory" and "and in
        ; every directory under it" are the same operation with the
        ; directory half changed, and nothing else about the listing
        ; knows or cares which directory it is being run in.
        call    dir_resolve
        jc      dir_pat_plain

        mov     dword ptr [dir_found],0
        mov     dword ptr [walk_act],OFFSET dir_one
        mov     dword ptr [walk_rec],0
        test    dword ptr [dir_sw],SW_S
        jz      dir_go
        mov     dword ptr [walk_rec],1
dir_go:
        call    tree_walk
        cmp     dword ptr [dir_found],0
        je      dir_none

        test    dword ptr [dir_sw],SW_S  ; the grand total, which only a
        jz      dir_free                 ; recursive listing has
        mov     edx,OFFSET msg_totlist
        call    print
        mov     eax,[dir_total]
        call    pdec10
        mov     edx,OFFSET msg_files
        call    print
        mov     eax,[dir_tbytes]
        call    pdec10
        mov     edx,OFFSET msg_bytes
        call    print
        jmp     dir_free

; 7160h would not truename it - a drive with nothing mounted, most
; likely.  Fall back to the listing this command has always done: the
; pattern exactly as typed, in one directory, which is right for every
; form with no path in it and is most of them.
dir_pat_plain:
        call    dir_sayplace            ; ESI is the pattern as typed
        mov     dword ptr [dir_hdrdone],1
        mov     edx,esi
        call    dir_list
        jc      dir_none
        call    dir_subtot
        jmp     dir_free

;---------------------------------------------------------------------
; dir_list - list one directory's worth of the pattern in EDX.
;            CF set if the find found nothing at all.
;
; Sorted first when /O asked for it, which is the only reason this is
; two loops instead of one: an order cannot be decided until every
; entry has been seen, so /O collects the whole directory before it
; prints a line of it and the plain listing streams as it always did.
;---------------------------------------------------------------------
dir_list:
        cmp     dword ptr [dir_nkey],0
        jne     dir_sorted

        ; the listing walks the LONG-name find (714Eh): same entries,
        ; but each row arrives with the long name alongside the 8.3.
        ; (Under LFN=OFF the wrapper drops to the classic 4Eh/4Fh.)
        mov     ecx,ATTR_DIR            ; CL = allowable: directories too,
        or      ecx,[dir_all]           ; and everything else if "DIR,"
        call    xfind_first
        jc      dir_listnone
        mov     word ptr [dir_hdl],ax
dir_row:
        call    dir_drawrow
        mov     bx,word ptr [dir_hdl]
        call    xfind_next
        jnc     dir_row
        mov     bx,word ptr [dir_hdl]   ; the walk is over - let the
        call    xfind_close             ; kernel have the handle back
        clc
        ret
dir_listnone:
        stc
        ret

;---------------------------------------------------------------------
; dir_drawrow - one line of the listing, out of find_data.
;
; A subroutine rather than the middle of the loop because the SORTED
; listing draws the same rows from somewhere else - it puts a stored
; record back into find_data and calls this.  One piece of code draws
; every row in either order, which is the point.
;---------------------------------------------------------------------
dir_drawrow:
        call    dir_heading             ; with /S, the first match is what
                                        ; proves the directory is worth
                                        ; naming - see dir_walk
        test    byte ptr [find_data],ATTR_DIR   ; the byte total counts
        jnz     dir_fmt                         ; files in either format
        mov     eax,dword ptr [find_data+20h]
        add     [dir_bytes],eax
dir_fmt:
        test    dword ptr [dir_sw],SW_W
        jz      dir_full
        call    dir_wide
        jmp     dir_counted
dir_full:
        mov     esi,OFFSET find_data + 130h     ; the 8.3 name
        xor     ecx,ecx
dir_name:
        mov     dl,[esi]
        or      dl,dl
        jz      dir_pad
        mov     ah,02h
        int     21h
        inc     esi
        inc     ecx
        jmp     short dir_name
dir_pad:
        cmp     ecx,14
        jae     dir_size
        mov     dl,' '
        mov     ah,02h
        int     21h
        inc     ecx
        jmp     short dir_pad
dir_size:
        test    byte ptr [find_data],ATTR_DIR
        jz      dir_isfile
        mov     edx,OFFSET msg_isdir    ; "<DIR>" instead of a size
        call    print
        jmp     short dir_stamp
dir_isfile:
        mov     eax,dword ptr [find_data+20h]
        call    pdec10
dir_stamp:
        mov     edx,OFFSET msg_2sp
        call    print

        movzx   eax,word ptr [find_data+16h]    ; date
        mov     ebx,eax
        shr     ebx,5
        and     ebx,0Fh
        push    eax
        mov     eax,ebx
        call    pdec2
        mov     dl,'-'
        mov     ah,02h
        int     21h
        pop     eax
        push    eax
        and     eax,1Fh
        call    pdec2
        mov     dl,'-'
        mov     ah,02h
        int     21h
        pop     eax
        shr     eax,9
        add     eax,1980
        call    pdec4
        mov     edx,OFFSET msg_2sp
        call    print

        movzx   eax,word ptr [find_data+14h]    ; time
        mov     ebx,eax
        shr     ebx,11
        push    eax
        mov     eax,ebx
        call    pdec2
        mov     dl,':'
        mov     ah,02h
        int     21h
        pop     eax
        shr     eax,5
        and     eax,3Fh
        call    pdec2
        call    dir_lfncol              ; the long name, when it has one
        call    dir_endline
dir_counted:
        inc     dword ptr [dir_count]
        ret

;---------------------------------------------------------------------
; dir_subtot - the two lines under one directory's listing.
;---------------------------------------------------------------------
dir_subtot:
        cmp     dword ptr [dir_col],0   ; /W may have left a part row
        je      dir_totals
        mov     dword ptr [dir_col],0
        call    dir_endline
dir_totals:
        mov     eax,[dir_count]
        call    pdec10
        mov     edx,OFFSET msg_files
        call    print
        mov     eax,[dir_bytes]
        call    pdec10
        mov     edx,OFFSET msg_bytes
        call    print
        ret

dir_free:
        ; Free space through 7303h, whose fields are 32 bits wide.  36h
        ; would do the same arithmetic in 16-bit registers and saturate
        ; at 65535 clusters, which is how a 40MB volume used to report
        ; itself as 32MB.  It is still the fallback: an older kernel
        ; answers 7303h with CF.
        mov     al,[dir_drv]            ; the listed drive, not the cwd's
        or      al,al
        jnz     dir_fsltr
        mov     ah,19h
        int     21h
        add     al,'A'
        jmp     short dir_fspath
dir_fsltr:
        add     al,'A'-1
dir_fspath:
        mov     [dir_fsbuf],al
        mov     byte ptr [dir_fsbuf+1],':'
        mov     byte ptr [dir_fsbuf+2],'\'
        mov     byte ptr [dir_fsbuf+3],0
        mov     edx,OFFSET dir_fsbuf
        mov     edi,OFFSET dir_fsrec
        mov     ecx,F73_SIZE
        mov     ax,7303h
        int     21h
        jc      dir_fs36
        mov     eax,dword ptr [dir_fsrec+F73_FREECL]
        imul    eax,dword ptr [dir_fsrec+F73_SPC]
        imul    eax,dword ptr [dir_fsrec+F73_BPS]
        jmp     short dir_fsout
dir_fs36:
        mov     dl,[dir_drv]            ; AX=spc BX=free CX=bps DX=total
        mov     ah,36h
        int     21h
        cmp     ax,0FFFFh
        je      dir_done
        movzx   eax,ax
        movzx   ebx,bx
        movzx   ecx,cx
        imul    eax,ebx
        imul    eax,ecx
dir_fsout:
        ; UNDER THE BYTES, NOT UNDER THE FILE COUNT.  Both totals are
        ; printed right-justified in ten columns, so the free figure on
        ; its own started at column 0 and lined up with the number of
        ; files - two quantities that have nothing to do with each other.
        ; The one it wants to be read against is the byte total above it,
        ; which pdec10 put in columns 20 to 29 because " file(s)  " is
        ; ten characters wide.  So the line is indented by exactly that
        ; much and the two columns of bytes sit under one another.
        mov     edx,OFFSET msg_freepad
        call    print
        call    pdec10
        mov     edx,OFFSET msg_free
        call    print
dir_done:
        ret
dir_none:
        mov     edx,OFFSET msg_nofiles
        call    print
        ret

; dir_lfncol - two spaces and the long name, when the entry has one
; (the kernel hands back the 8.3 in both fields when it does not)
;---------------------------------------------------------------------
; dir_sayplace - the "Directory of" line, taken from the PATTERN in
; ESI rather than from the current directory.  21h/7160h renders the
; pattern absolute (its final component as typed, wildcards and all),
; and cutting that final component off leaves precisely the directory
; being listed - which is what DIR C:\SUB should have said all along
; instead of headlining the drive's cwd.  If the kernel will not
; truename it, the cwd line is still right for every form with no
; path in it, which is most of them.
;---------------------------------------------------------------------
dir_sayplace:
        pushad
        mov     edi,OFFSET dir_hdrbuf
        mov     ecx,2                   ; CL = 2, the long form
        mov     ax,7160h
        int     21h
        jc      dsp_cwd

        mov     edi,OFFSET dir_hdrbuf
        xor     edx,edx                 ; EDX = index just past the last '\'
        xor     ecx,ecx
dsp_scan:
        mov     al,[edi+ecx]
        or      al,al
        jz      dsp_cut
        inc     ecx
        cmp     al,'\'
        jne     dsp_scan
        mov     edx,ecx
        jmp     short dsp_scan
dsp_cut:
        or      edx,edx
        jz      dsp_cwd                 ; no separator at all
        cmp     edx,3                   ; "X:\" keeps its backslash;
        jbe     dsp_keep                ; anything deeper drops it
        dec     edx
dsp_keep:
        mov     byte ptr [edi+edx],0
        mov     edx,OFFSET msg_dirof
        call    print
        mov     esi,OFFSET dir_hdrbuf
        call    print_asciiz
        jmp     short dsp_tail
dsp_cwd:
        mov     edx,OFFSET msg_dirof
        call    print
        call    put_dcwd
dsp_tail:
        mov     edx,OFFSET msg_dirof2
        call    print
        popad
        ret

;---------------------------------------------------------------------
; dir_endline - end one line of the listing, and with /P stop for a
;               key every screenful.  It counts LINES rather than
;               entries, so the wide form pauses in the same places.
;---------------------------------------------------------------------
dir_endline:
        call    put_crlf
        test    dword ptr [dir_sw],SW_P
        jz      dle_out
        inc     dword ptr [dir_lines]

        ; How tall is the screen?  Asked, not assumed: a program run
        ; in a DOS box can have set 43 or 50 lines and left them that
        ; way, and a /P that counts 25 of them stops twice in the
        ; middle of a page and calls it a screenful.
        push    eax
        push    edx
        mov     eax,0F003h
        int     21h
        movzx   eax,dh
        sub     eax,2                   ; the heading and the prompt
        cmp     eax,2
        jae     dle_page
        mov     eax,DIR_PAGE            ; a screen too short to page
dle_page:
        cmp     dword ptr [dir_lines],eax
        pop     edx
        pop     eax
        jb      dle_out
        mov     dword ptr [dir_lines],0
        push    edx
        mov     edx,OFFSET msg_pause
        call    print
        mov     ah,08h
        int     21h
        call    put_crlf
        pop     edx
dle_out:
        ret

;---------------------------------------------------------------------
; dir_wide - one entry as a /W column: the 8.3 name, a directory in
;            [brackets], five to a line.
;---------------------------------------------------------------------
DIR_WCOLS       EQU     5
DIR_WWIDE       EQU     15              ; 5 x 15 = 75, clear of the wrap

dir_wide:
        mov     esi,OFFSET find_data + 130h
        xor     ecx,ecx                 ; ECX = characters emitted
        test    byte ptr [find_data],ATTR_DIR
        jz      dw_name
        mov     dl,'['
        mov     ah,02h
        int     21h
        inc     ecx
dw_name:
        mov     dl,[esi]
        or      dl,dl
        jz      dw_close
        mov     ah,02h
        int     21h
        inc     esi
        inc     ecx
        jmp     short dw_name
dw_close:
        test    byte ptr [find_data],ATTR_DIR
        jz      dw_pad
        mov     dl,']'
        mov     ah,02h
        int     21h
        inc     ecx
dw_pad:
        cmp     ecx,DIR_WWIDE
        jae     dw_col
        mov     dl,' '
        mov     ah,02h
        int     21h
        inc     ecx
        jmp     short dw_pad
dw_col:
        inc     dword ptr [dir_col]
        cmp     dword ptr [dir_col],DIR_WCOLS
        jb      dw_out
        mov     dword ptr [dir_col],0
        call    dir_endline
dw_out:
        ret

dir_lfncol:
        mov     esi,OFFSET find_data + 2Ch
        mov     edi,OFFSET find_data + 130h
dlc_cmp:
        mov     al,[esi]
        cmp     al,[edi]
        jne     dlc_show
        or      al,al
        jz      dlc_out                 ; identical: nothing to add
        inc     esi
        inc     edi
        jmp     short dlc_cmp
dlc_show:
        mov     edx,OFFSET msg_2sp
        call    print
        mov     esi,OFFSET find_data + 2Ch
        call    print_asciiz
dlc_out:
        ret

;=====================================================================
; DIRCMD - the switches a user wants on every listing.
;
; MS-DOS reads this variable and puts its contents in front of the
; command line, which is a description of the effect rather than of
; the mechanism: what is in it is switches, and switches are already
; lifted out of the line and turned into a mask before anything else
; happens.  So the merge is done on the masks, where the rule can be
; stated exactly - THE COMMAND LINE WINS - instead of on text, where
; it would depend on which of two /O switches came first.
;
; The rule is the whole reason "/-x" exists.  A DIRCMD of "/W" with no
; way to say "not this time" is a variable nobody would set.
;---------------------------------------------------------------------
dir_dircmd:
        pushad
        mov     esi,OFFSET env_dircmd
        call    env_find                ; CF: no such variable
        jc      dc_out
        mov     edi,OFFSET dir_ecmd     ; copied out, because sw_scan
        mov     edx,edi                 ; rewrites what it is given and
        mov     ecx,143                 ; the environment is not scratch
        call    dir_cpn

        mov     esi,OFFSET dir_ecmd     ; its arguments are kept aside,
        call    sw_scan                 ; because the command line's scan
        mov     ebx,eax                 ; is about to overwrite them
        push    esi
        push    edi
        mov     esi,OFFSET sw_args
        mov     edi,OFFSET dir_eargs
        mov     ecx,(26 * SW_ARGMAX) / 4
        rep     movsd
        pop     edi
        pop     esi

        ; the environment's, minus what it turned off, minus what the
        ; command line mentioned either way
        mov     eax,[sw_negm]
        not     eax
        and     ebx,eax
        mov     eax,[dir_sw]
        or      eax,[dir_neg]
        not     eax
        and     ebx,eax                 ; EBX = what only DIRCMD asked for
        or      [dir_sw],ebx

        ; and those switches' arguments, for the same letters
        xor     ecx,ecx
dc_arg:
        cmp     ecx,26
        jae     dc_done
        bt      ebx,ecx
        jnc     dc_next
        push    ecx
        mov     esi,ecx
        shl     esi,3
        mov     edi,esi
        add     esi,OFFSET dir_eargs
        add     edi,OFFSET sw_args
        mov     ecx,SW_ARGMAX
        rep     movsb
        pop     ecx
dc_next:
        inc     ecx
        jmp     short dc_arg
dc_done:
        mov     eax,[dir_neg]           ; and last of all, said again: a
        not     eax                     ; "/-x" on the line beats anything
        and     [dir_sw],eax            ; DIRCMD had to say about it
dc_out:
        popad
        ret

;=====================================================================
; /O - the sort order, as a list of keys.
;
;   N name   E extension   D date and time   S size   G directories
;   first, and a '-' in front of any of them reverses it.
;
; A bare /O is /OGN, which is what MS-DOS 6 documents it as and is the
; order anybody typing /O without thinking about it means.
;=====================================================================
dir_order:
        pushad
        mov     dword ptr [dir_nkey],0
        test    dword ptr [dir_sw],SW_O
        jz      do_out

        mov     al,'O'
        call    sw_arg                  ; ESI -> what was stuck to it
        cmp     byte ptr [esi],0
        jne     do_parse
        mov     esi,OFFSET dir_dfltord  ; "GN"
do_parse:
        cmp     byte ptr [esi],':'
        jne     do_lp
        inc     esi
do_lp:
        mov     al,[esi]
        or      al,al
        jz      do_out
        mov     ah,0                    ; ascending unless a sign says not
        cmp     al,'-'
        jne     do_nosign
        mov     ah,1
        inc     esi
        mov     al,[esi]
do_nosign:
        cmp     al,'+'
        jne     do_have
        inc     esi
        mov     al,[esi]
do_have:
        or      al,al
        jz      do_out
        call    upcase
        inc     esi
        cmp     al,'N'
        je      do_add
        cmp     al,'E'
        je      do_add
        cmp     al,'D'
        je      do_add
        cmp     al,'S'
        je      do_add
        cmp     al,'G'
        jne     do_lp                   ; anything else is not a key
do_add:
        mov     ecx,[dir_nkey]
        cmp     ecx,DIR_MAXKEY
        jae     do_lp
        shl     ecx,1
        mov     [dir_keys+ecx],al
        mov     [dir_keys+ecx+1],ah
        inc     dword ptr [dir_nkey]
        jmp     short do_lp
do_out:
        popad
        ret

dir_dfltord db  'GN',0

;=====================================================================
; dir_resolve - split the pattern in ESI into the DIRECTORY it lives
;               in and the PATTERN within that directory.
;
; 21h/7160h renders the thing absolute, wildcards and all, and the
; last backslash is the join between the two halves.  Splitting once
; here is what lets /S exist: every directory of the walk lists the
; same dir_fpat, and nothing below this knows where it is.
;=====================================================================
dir_resolve:
        pushad
        mov     edi,OFFSET dir_hdrbuf
        mov     ecx,2                   ; CL = 2, the long form
        mov     ax,7160h
        int     21h
        jc      drs_fail

        mov     esi,OFFSET dir_hdrbuf
        xor     edx,edx                 ; index just past the last '\'
        xor     ecx,ecx
drs_scan:
        mov     al,[esi+ecx]
        or      al,al
        jz      drs_split
        inc     ecx
        cmp     al,'\'
        jne     drs_scan
        mov     edx,ecx
        jmp     short drs_scan
drs_split:
        or      edx,edx
        jz      drs_fail                ; no separator at all
        cmp     byte ptr [esi+edx],0
        je      drs_fail                ; nothing after it to match

        mov     esi,OFFSET dir_hdrbuf   ; the tail is the pattern
        add     esi,edx
        mov     edi,OFFSET dir_fpat
drs_cp:
        lodsb
        stosb
        or      al,al
        jnz     drs_cp

        cmp     edx,3                   ; "X:\" keeps its backslash;
        jbe     drs_keep                ; anything deeper drops it
        dec     edx
drs_keep:
        mov     esi,OFFSET dir_hdrbuf
        mov     edi,OFFSET dir_dirpath
        mov     ecx,edx
        rep     movsb
        mov     byte ptr [edi],0
        popad
        clc
        ret
drs_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; dir_join - dir_scratch = dir_dirpath + '\' + dir_fpat
;---------------------------------------------------------------------
dir_join:
        pushad
        mov     esi,OFFSET dir_dirpath
        mov     edi,OFFSET dir_scratch
        xor     ecx,ecx
dj_cp:
        mov     al,[esi+ecx]
        or      al,al
        jz      dj_end
        mov     [edi+ecx],al
        inc     ecx
        jmp     short dj_cp
dj_end:
        or      ecx,ecx
        jz      dj_pat
        cmp     byte ptr [edi+ecx-1],'\'
        je      dj_pat
        mov     byte ptr [edi+ecx],'\'
        inc     ecx
dj_pat:
        add     edi,ecx
        mov     esi,OFFSET dir_fpat
dj_cp2:
        lodsb
        stosb
        or      al,al
        jnz     dj_cp2
        popad
        ret

;=====================================================================
; tree_walk - do [walk_act] to dir_dirpath, and if walk_rec says so to
;             every directory under it.
;
; ONE WALKER, TWO COMMANDS.  DIR /S and DEL /S are the same traversal
; with a different thing done at each stop, and the traversal is the
; part with the difficulties in it - so it is written once and the
; command supplies the verb.  dir_one lists; del_one deletes.
;
; RECURSIVE, AND THE FIND IS NOT HELD OPEN ACROSS THE RECURSION.  It
; cannot be: under LFN=OFF the wrapper falls back to 4Eh/4Fh, which
; resume out of one DTA, so a second search started inside the first
; would take the first one's place.  So a directory's subdirectory
; NAMES are collected into a block first, the find is closed, and the
; walk goes down the list - which costs one allocation per level and
; works the same whether long names are on or off.
;
; A directory that will not open is stepped over in silence, which is
; what a walk of a tree with something unreadable in it should do: the
; unreadable part is not the part that was asked for.
;=====================================================================
tree_walk:
        push    ebp
        push    ebx
        push    esi
        push    edi
        sub     esp,8
        mov     ebp,esp                 ; [ebp] = block, [ebp+4] = count

        call    dword ptr [walk_act]    ; whatever this walk is FOR

        cmp     dword ptr [walk_rec],0
        jz      dw_done
        mov     eax,[dir_depth]
        cmp     eax,DIR_MAXDEPTH
        jae     dw_done

        call    dir_subcount            ; -> ECX subdirectories
        or      ecx,ecx
        jz      dw_done
        mov     dword ptr [ebp+4],ecx
        mov     eax,ecx
        imul    eax,DIR_SUBNAME
        mov     ebx,eax
        mov     ah,48h
        int     21h
        jc      dw_done                 ; no room to remember them: this
        mov     [ebp],eax               ; branch is as far as the walk goes
        mov     edi,eax
        mov     ecx,[ebp+4]
        call    dir_subfill

        xor     ebx,ebx
dw_each:
        cmp     ebx,[ebp+4]
        jae     dw_free
        mov     esi,[ebp]
        mov     eax,ebx
        imul    eax,DIR_SUBNAME
        add     esi,eax
        call    dir_descend
        inc     ebx
        jmp     short dw_each
dw_free:
        mov     edx,[ebp]
        mov     ah,49h
        int     21h
dw_done:
        add     esp,8
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

;---------------------------------------------------------------------
; dir_one - what DIR does at one stop of the walk.
;---------------------------------------------------------------------
dir_one:
        mov     dword ptr [dir_count],0
        mov     dword ptr [dir_bytes],0
        mov     dword ptr [dir_hdrdone],0

        test    dword ptr [dir_sw],SW_S ; WITHOUT /S the heading comes
        jnz     do1_defer               ; first, so that a listing with
        call    dir_heading             ; nothing in it still says where
do1_defer:                              ; it looked.  WITH /S it waits for
                                        ; the first match, or a walk of a
                                        ; whole disk is one heading per
                                        ; directory and five matches
        call    dir_join
        mov     edx,OFFSET dir_scratch
        call    dir_list
        jc      do1_out

        inc     dword ptr [dir_found]
        call    dir_subtot
        mov     eax,[dir_count]
        add     [dir_total],eax
        mov     eax,[dir_bytes]
        add     [dir_tbytes],eax
do1_out:
        ret

;---------------------------------------------------------------------
; dir_descend - append the 8.3 name at ESI to dir_dirpath, walk what
;               is now named, and cut the path back to what it was.
;---------------------------------------------------------------------
dir_descend:
        push    ebx
        push    esi
        push    edi
        mov     edi,OFFSET dir_dirpath
        xor     ecx,ecx
dd_len:
        cmp     byte ptr [edi+ecx],0
        je      dd_got
        inc     ecx
        jmp     short dd_len
dd_got:
        push    ecx                     ; where to cut back to
        or      ecx,ecx
        jz      dd_app
        cmp     byte ptr [edi+ecx-1],'\'
        je      dd_app
        mov     byte ptr [edi+ecx],'\'
        inc     ecx
dd_app:
        add     edi,ecx
dd_cp:
        lodsb
        stosb
        or      al,al
        jnz     dd_cp

        inc     dword ptr [dir_depth]
        call    tree_walk
        dec     dword ptr [dir_depth]

        pop     ecx
        mov     byte ptr [dir_dirpath+ecx],0
        pop     edi
        pop     esi
        pop     ebx
        ret

;---------------------------------------------------------------------
; dir_subcount / dir_subfill - the subdirectories of dir_dirpath, in
;                              two passes over the same enumeration:
;                              how many there are, then what they are
;                              called.  Two passes rather than a guess
;                              at how many a directory might hold.
;---------------------------------------------------------------------
dir_subcount:
        push    eax
        push    ebx
        push    edx
        push    esi
        push    edi
        mov     dword ptr [dir_nfill],0
        call    dir_subpat
        mov     edx,OFFSET dir_scratch
        mov     ecx,ATTR_DIR
        call    xfind_first
        jc      dsc_out
        mov     word ptr [dir_hdl],ax
dsc_lp:
        call    dir_issub
        jc      dsc_next
        inc     dword ptr [dir_nfill]
dsc_next:
        mov     bx,word ptr [dir_hdl]
        call    xfind_next
        jnc     dsc_lp
        mov     bx,word ptr [dir_hdl]
        call    xfind_close
dsc_out:
        mov     ecx,[dir_nfill]
        pop     edi
        pop     esi
        pop     edx
        pop     ebx
        pop     eax
        ret

dir_subfill:                            ; EDI -> the block, ECX = how many
        pushad
        mov     [dir_nrec],ecx
        mov     [dir_subblk],edi        ; KEPT, not carried in EDI: the find
                                        ; points EDI at find_data every time
                                        ; it fetches an entry, so a block
                                        ; pointer left in it is a block
                                        ; pointer that becomes find_data and
                                        ; every name comes back empty
        mov     dword ptr [dir_nfill],0
        call    dir_subpat
        mov     edx,OFFSET dir_scratch
        mov     ecx,ATTR_DIR
        call    xfind_first
        jc      dsf_out
        mov     word ptr [dir_hdl],ax
dsf_lp:
        call    dir_issub
        jc      dsf_next
        mov     eax,[dir_nfill]
        cmp     eax,[dir_nrec]
        jae     dsf_close               ; more than the first pass saw
        imul    eax,DIR_SUBNAME
        add     eax,[dir_subblk]
        mov     edi,eax
        mov     esi,OFFSET find_data + 130h
        mov     ecx,DIR_SUBNAME
dsf_cp:
        lodsb
        stosb
        dec     ecx
        jz      dsf_cpend
        or      al,al
        jnz     dsf_cp
dsf_cpend:
        inc     dword ptr [dir_nfill]
dsf_next:
        mov     bx,word ptr [dir_hdl]
        call    xfind_next
        jnc     dsf_lp
dsf_close:
        mov     bx,word ptr [dir_hdl]
        call    xfind_close
dsf_out:
        popad
        ret

; dir_subpat - dir_scratch = dir_dirpath + "\*.*"
dir_subpat:
        pushad
        mov     eax,dword ptr [dir_fpat]        ; the listing's own pattern,
        push    eax                             ; four bytes of which is all
                                                ; "*.*" needs to borrow
        mov     dword ptr [dir_fpat],002A2E2Ah  ; '*' '.' '*' 0
        call    dir_join
        pop     eax
        mov     dword ptr [dir_fpat],eax
        popad
        ret

; dir_issub - CF clear if find_data is a real subdirectory and not
;             one of the two that lead back up.
dir_issub:
        test    byte ptr [find_data],ATTR_DIR
        jz      dis_no
        cmp     byte ptr [find_data+130h],'.'
        jne     dis_yes
dis_no:
        stc
        ret
dis_yes:
        clc
        ret

;---------------------------------------------------------------------
; dir_heading - "Directory of X:\WHEREVER", once per directory.
;---------------------------------------------------------------------
dir_heading:
        cmp     dword ptr [dir_hdrdone],0
        jne     dh_out
        mov     dword ptr [dir_hdrdone],1
        push    edx
        push    esi
        mov     edx,OFFSET msg_dirof
        call    print
        mov     esi,OFFSET dir_dirpath
        call    print_asciiz
        mov     edx,OFFSET msg_dirof2
        call    print
        pop     esi
        pop     edx
dh_out:
        ret

;=====================================================================
; /O - the same listing, in an order.
;
; Two passes and a block: count the directory, take memory for that
; many records, fill it, sort an index over it, then draw the rows.
; A sort cannot start until the last entry has been seen, which is the
; only reason the plain listing and this one are different code at all
; - and why the plain one is kept: it streams, and streaming a large
; directory needs no memory and shows the first line at once.
;
; NO MEMORY MEANS NO ORDER, NOT NO LISTING.  A directory too big to
; collect is listed unsorted rather than refused, because what the
; user asked for was to see it.
;=====================================================================
dir_sorted:
        push    edx
        mov     dword ptr [dir_nrec],0
        mov     ecx,ATTR_DIR
        or      ecx,[dir_all]
        call    xfind_first
        jnc     ds_counting
        pop     edx
        jmp     dir_listnone
ds_counting:
        mov     word ptr [dir_hdl],ax
ds_count:
        inc     dword ptr [dir_nrec]
        mov     bx,word ptr [dir_hdl]
        call    xfind_next
        jnc     ds_count
        mov     bx,word ptr [dir_hdl]
        call    xfind_close
        pop     edx

        mov     eax,[dir_nrec]
        imul    eax,DR_LEN + 4
        mov     ebx,eax
        mov     ah,48h
        int     21h
        jc      ds_plain
        mov     [dir_recs],eax
        mov     ecx,[dir_nrec]
        imul    ecx,DR_LEN
        add     ecx,eax
        mov     [dir_idx],ecx

        push    edx
        mov     ecx,ATTR_DIR
        or      ecx,[dir_all]
        call    xfind_first
        pop     edx
        jc      ds_freeplain
        mov     word ptr [dir_hdl],ax
        mov     dword ptr [dir_nfill],0
ds_fill:
        mov     eax,[dir_nfill]
        cmp     eax,[dir_nrec]
        jae     ds_filled
        imul    eax,DR_LEN              ; WORKED OUT AFRESH EVERY TIME, not
        add     eax,[dir_recs]          ; carried in EDI: xfind_next points
        mov     edi,eax                 ; EDI at find_data on its way past
        call    dir_record
        mov     eax,[dir_nfill]
        mov     ecx,[dir_idx]
        mov     [ecx+eax*4],edi
        inc     dword ptr [dir_nfill]
        mov     bx,word ptr [dir_hdl]
        call    xfind_next
        jnc     ds_fill
ds_filled:
        mov     bx,word ptr [dir_hdl]
        call    xfind_close

        call    dir_sortidx
        mov     dword ptr [dir_nrec],0
ds_show:
        mov     eax,[dir_nrec]
        cmp     eax,[dir_nfill]
        jae     ds_shown
        mov     ecx,[dir_idx]
        mov     esi,[ecx+eax*4]
        call    dir_unrecord            ; the record back into find_data
        call    dir_drawrow
        inc     dword ptr [dir_nrec]
        jmp     short ds_show
ds_shown:
        mov     edx,[dir_recs]
        mov     ah,49h
        int     21h
        clc
        ret

; the records were taken and the second pass found nothing after all
ds_freeplain:
        push    edx
        mov     edx,[dir_recs]
        mov     ah,49h
        int     21h
        pop     edx
        jmp     dir_listnone

; there was no room to collect the directory.  List it unsorted rather
; than not at all: what was asked for was to see it.
ds_plain:
        mov     dword ptr [dir_nkey],0  ; and do not ask again this run
        jmp     dir_list

;---------------------------------------------------------------------
; dir_record / dir_unrecord - the six fields that get printed, out of
;                             a find record and back into one.
;---------------------------------------------------------------------
dir_record:                             ; EDI -> the record
        pushad
        push    edi
        xor     eax,eax
        mov     ecx,DR_LEN / 4
        rep     stosd
        pop     edi
        mov     al,[find_data]
        mov     [edi+DR_ATTR],al
        mov     ax,word ptr [find_data+14h]
        mov     [edi+DR_TIME],ax
        mov     ax,word ptr [find_data+16h]
        mov     [edi+DR_DATE],ax
        mov     eax,dword ptr [find_data+20h]
        mov     [edi+DR_SIZE],eax

        mov     esi,OFFSET find_data + 130h
        lea     edx,[edi+DR_NAME]
        mov     ecx,13
        call    dir_cpn
        mov     esi,OFFSET find_data + 2Ch
        lea     edx,[edi+DR_LONG]
        mov     ecx,132
        call    dir_cpn
        popad
        ret

; dir_cpn - at most ECX bytes of the ASCIIZ at ESI to EDX, always
;           terminated.  A long name past the record's room is cut
;           rather than allowed to run into the next one.
dir_cpn:
        push    edi
        mov     edi,edx
        dec     ecx
dcn_lp:
        or      ecx,ecx
        jz      dcn_end
        lodsb
        or      al,al
        jz      dcn_end
        stosb
        dec     ecx
        jmp     short dcn_lp
dcn_end:
        mov     byte ptr [edi],0
        pop     edi
        ret

dir_unrecord:                           ; ESI -> the record
        pushad
        push    esi
        mov     edi,OFFSET find_data
        xor     eax,eax
        mov     ecx,318 / 2
        rep     stosw
        pop     esi
        mov     al,[esi+DR_ATTR]
        mov     [find_data],al
        mov     ax,[esi+DR_TIME]
        mov     word ptr [find_data+14h],ax
        mov     ax,[esi+DR_DATE]
        mov     word ptr [find_data+16h],ax
        mov     eax,[esi+DR_SIZE]
        mov     dword ptr [find_data+20h],eax
        push    esi
        lea     esi,[esi+DR_NAME]
        mov     edx,OFFSET find_data + 130h
        mov     ecx,13
        call    dir_cpn
        pop     esi
        lea     esi,[esi+DR_LONG]
        mov     edx,OFFSET find_data + 2Ch
        mov     ecx,132
        call    dir_cpn
        popad
        ret

;---------------------------------------------------------------------
; dir_sortidx - insertion sort over the index, which moves four bytes
;               a step rather than a hundred and sixty.  A directory
;               is a few hundred entries; the algorithm is not what
;               anybody will wait for.
;---------------------------------------------------------------------
dir_sortidx:
        pushad
        mov     ebx,1                   ; EBX = the entry being placed
dsi_out:
        cmp     ebx,[dir_nfill]
        jae     dsi_done
        mov     edi,[dir_idx]
        mov     ebp,[edi+ebx*4]         ; the record it points at
        mov     ecx,ebx
dsi_in:
        or      ecx,ecx
        jz      dsi_put
        mov     esi,[edi+ecx*4-4]       ; the one before it
        push    ecx
        mov     edx,ebp
        call    dir_cmprec              ; ESI against EDX
        pop     ecx
        jle     dsi_put                 ; in place already
        mov     [edi+ecx*4],esi
        dec     ecx
        jmp     short dsi_in
dsi_put:
        mov     [edi+ecx*4],ebp
        inc     ebx
        jmp     short dsi_out
dsi_done:
        popad
        ret

;---------------------------------------------------------------------
; dir_cmprec - compare the records at ESI and EDX by every key in
;              dir_keys until one of them says something.  Sets the
;              flags for a signed test: positive if ESI sorts after.
;---------------------------------------------------------------------
dir_cmprec:
        push    ebx
        push    ecx
        push    esi
        push    edi
        xor     ecx,ecx
dcr_key:
        cmp     ecx,[dir_nkey]
        jae     dcr_same
        push    ecx
        mov     ebx,ecx
        shl     ebx,1
        mov     al,[dir_keys+ebx]
        mov     ah,[dir_keys+ebx+1]     ; 1 = reversed
        call    dir_key1                ; -> EAX = -1, 0 or 1
        pop     ecx
        or      eax,eax
        jnz     dcr_out
        inc     ecx
        jmp     short dcr_key

; EVERY KEY AGREED, SO THE NAME DECIDES.  Without this the order of
; equal entries is whatever the directory happened to hold - /O:E puts
; three directories in front, all with no extension, and they come out
; in no order at all.  DOS breaks the same tie the same way, and it is
; also what makes a listing REPRODUCIBLE, which a sorted listing had
; better be.  The tie-break is never reversed: "-E" means backwards by
; extension, not backwards by everything.
dcr_same:
        mov     al,'N'
        xor     ah,ah
        call    dir_key1
dcr_out:
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        or      eax,eax                 ; the flags the caller tests
        ret

;---------------------------------------------------------------------
; dir_key1 - one key.  AL = its letter, AH = 1 if reversed.
;            ESI and EDX are the two records; EAX comes back -1, 0
;            or 1 for "ESI first", "no opinion", "EDX first".
;---------------------------------------------------------------------
dir_key1:
        push    ebx
        push    ecx
        push    esi
        push    edi
        mov     bl,ah                   ; the direction, kept
        cmp     al,'G'
        je      dk_group
        cmp     al,'S'
        je      dk_size
        cmp     al,'D'
        je      dk_date
        cmp     al,'E'
        je      dk_ext
                                        ; 'N' and anything unknown
dk_name:
        lea     esi,[esi+DR_LONG]
        lea     edi,[edx+DR_LONG]
        call    dir_stricmp
        jmp     short dk_signed

dk_ext:
        lea     esi,[esi+DR_NAME]
        call    dir_extof
        push    esi
        lea     esi,[edx+DR_NAME]
        call    dir_extof
        mov     edi,esi
        pop     esi
        call    dir_stricmp
        jmp     short dk_signed

dk_size:
        mov     eax,[esi+DR_SIZE]
        cmp     eax,[edx+DR_SIZE]
        jb      dk_less
        ja      dk_more
        jmp     short dk_equal

dk_date:
        movzx   eax,word ptr [esi+DR_DATE]
        shl     eax,16
        mov     ax,word ptr [esi+DR_TIME]
        movzx   ecx,word ptr [edx+DR_DATE]
        shl     ecx,16
        mov     cx,word ptr [edx+DR_TIME]
        cmp     eax,ecx
        jb      dk_less
        ja      dk_more
        jmp     short dk_equal

; GROUPING IS NOT A SORT KEY LIKE THE OTHERS: it has two values and
; directories come first, which is the one order anybody wants.  A
; '-' in front of it puts the files first, because a key that cannot
; be reversed would be the only one in the list that could not.
dk_group:
        mov     al,[esi+DR_ATTR]
        and     al,ATTR_DIR
        mov     ah,[edx+DR_ATTR]
        and     ah,ATTR_DIR
        cmp     al,ah
        je      dk_equal
        or      al,al
        jnz     dk_less                 ; ESI is a directory: it goes first
        jmp     short dk_more

dk_signed:
        or      eax,eax
        js      dk_less
        jnz     dk_more
dk_equal:
        xor     eax,eax
        jmp     short dk_out
dk_less:
        mov     eax,-1
        jmp     short dk_rev
dk_more:
        mov     eax,1
dk_rev:
        or      bl,bl
        jz      dk_out
        neg     eax
dk_out:
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        ret

; dir_extof - ESI -> an 8.3 name; leaves ESI on its extension, or on
;             the terminator when it has none.
dir_extof:
        push    eax
        cmp     byte ptr [esi],0
        je      de_out
        inc     esi                     ; A LEADING DOT IS NOT A SEPARATOR.
                                        ; "." and ".." are directory entries
                                        ; and DOS stores them with a blank
                                        ; extension; reading the second dot
                                        ; of ".." as one gave them an
                                        ; extension nothing else had and put
                                        ; them in the middle of a /O:E
                                        ; listing
de_lp:
        mov     al,[esi]
        or      al,al
        jz      de_out
        cmp     al,'.'
        je      de_dot
        inc     esi
        jmp     short de_lp
de_dot:
        inc     esi
de_out:
        pop     eax
        ret

; dir_stricmp - ESI against EDI, case-insensitively.  EAX comes back
;               negative, zero or positive the way strcmp does.
dir_stricmp:
        push    ebx
        push    ecx
dsc_lp2:
        movzx   eax,byte ptr [esi]
        movzx   ecx,byte ptr [edi]
        cmp     al,'a'
        jb      dsc_a
        cmp     al,'z'
        ja      dsc_a
        sub     al,32
dsc_a:
        cmp     cl,'a'
        jb      dsc_b
        cmp     cl,'z'
        ja      dsc_b
        sub     cl,32
dsc_b:
        cmp     al,cl
        jne     dsc_diff
        or      al,al
        jz      dsc_eq
        inc     esi
        inc     edi
        jmp     short dsc_lp2
dsc_diff:
        sub     eax,ecx
        pop     ecx
        pop     ebx
        ret
dsc_eq:
        xor     eax,eax
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; TYPE file
;=====================================================================
cmd_type:
        call    need_arg
        jc      ct_out
        mov     edx,esi
        mov     ax,3D00h
        int     21h
        jc      ct_cant
        movzx   ebx,ax
ct_lp:
        mov     ecx,COPYBUF_SIZE
        mov     edx,OFFSET copy_buf
        mov     ah,3Fh
        int     21h
        jc      ct_close
        or      eax,eax
        jz      ct_close
        mov     ecx,eax
        push    ebx
        mov     ebx,1                   ; stdout
        mov     ah,40h
        int     21h
        pop     ebx
        jmp     short ct_lp
ct_close:
        mov     ah,3Eh
        int     21h
ct_out:
        ret
ct_cant:
        mov     edx,OFFSET msg_notfound
        call    print
        ret

;=====================================================================
; COPY source [+ source ...] [destination]
;
; The tail is cut into ITEMS at whitespace, and a '+' glues an item to
; the one before it - so "A + B", "A+ B", "A +B" and "A+B" are all the
; same request, which is how DOS read them.  What is left is a list of
; sources and, unless a '+' claimed it, a destination.
;
; The destination decides the shape of the whole operation.  If it is
; a PLACE - a directory, a bare drive, a trailing backslash, or absent
; altogether - then every source keeps its own name and lands there,
; which is the commonest COPY form there is and the one PM-DOS could
; not do before v0.21.  If it is a FILE, then there is one output and
; everything is concatenated into it, which is why "COPY *.TXT ALL"
; joins rather than overwrites: same rule, not a special case.
;
; "COPY A+B" with no destination appends B onto A.  The destination is
; the first source opened for APPEND and the copying starts at the
; second - doing it that way, rather than reading and writing one file
; at two positions, is what keeps it safe.
;
; A source that would be read into its own destination is skipped with
; a warning rather than growing forever.
;=====================================================================
CP_MAXITEM      EQU     16

cmd_copy:
        mov     dword ptr [cp_count],0
        mov     dword ptr [cp_first],0
        mov     dword ptr [cp_dstptr],0
        mov     byte ptr [cp_append],0
        mov     byte ptr [cp_glued],0
        mov     byte ptr [cp_opened],0
        mov     byte ptr [cp_verify],0
        mov     byte ptr [cp_curmode],CPM_UNSET
        mov     byte ptr [cp_ztail],0
        mov     dword ptr [copy_src],-1
        mov     dword ptr [copy_dst],-1

        mov     esi,[arg_ptr]
        call    skip_blanks
        cmp     byte ptr [esi],0
        je      cp_usage
        call    cp_parse
        jc      cp_usage

;--- who is the destination? -----------------------------------------
        mov     ebx,[cp_nitem]
        cmp     ebx,1
        jbe     cp_nodest
        dec     ebx
        cmp     byte ptr [cp_glue+ebx],0
        jne     cp_nodest               ; a '+' claimed the last item
        mov     eax,[cp_items+ebx*4]
        mov     [cp_dstptr],eax
        mov     [cp_nitem],ebx
        mov     al,[cp_mode+ebx]        ; the destination's own /A or /B
        mov     [cp_dmode],al
        jmp     short cp_havedest
cp_nodest:
        mov     byte ptr [cp_dmode],CPM_UNSET
        cmp     dword ptr [cp_nitem],1
        jbe     cp_havedest             ; one source, no destination:
                                        ; the current directory
        mov     eax,[cp_items]          ; "COPY A+B" - append onto A
        mov     [cp_dstptr],eax
        mov     byte ptr [cp_append],1
        mov     dword ptr [cp_first],1
cp_havedest:

;--- a place to put files, or one file to make? ----------------------
        call    cp_isplace
        mov     byte ptr [cp_todir],0
        jc      cp_notdir
        mov     byte ptr [cp_todir],1
cp_notdir:
        ; One output file unless every source keeps its own name.  A
        ; '+' asks for a join even into a directory, where DOS named
        ; the result after the first source.
        mov     al,1
        cmp     byte ptr [cp_todir],0
        je      cp_setcat
        mov     al,[cp_glued]
cp_setcat:
        mov     [cp_concat],al

;--- what /A and /B mean when nobody said ----------------------------
; DOS's defaults, and they are not arbitrary.  A plain copy is BINARY:
; the point of it is that the file at the far end is the same file, and
; a byte that happens to be 1Ah is a byte.  A CONCATENATION is ASCII,
; because joining files is a text operation - it exists so that three
; text files become one, and a text file of that era ended at its
; first Ctrl-Z whatever came after.
        mov     al,CPM_BINARY
        cmp     byte ptr [cp_glued],0
        je      cp_dflt
        mov     al,CPM_ASCII
cp_dflt:
        mov     ah,al
        xor     ecx,ecx
cp_dlp:
        cmp     ecx,[cp_nitem]
        jae     cp_dend
        cmp     byte ptr [cp_mode+ecx],CPM_UNSET
        jne     cp_dnext
        mov     [cp_mode+ecx],al
cp_dnext:
        inc     ecx
        jmp     short cp_dlp
cp_dend:
        cmp     byte ptr [cp_dmode],CPM_UNSET
        jne     cp_dhave
        mov     [cp_dmode],ah
cp_dhave:

; /V turns VERIFY on for the length of the copy and puts it back
; afterwards, which is exactly what it has always meant - and which
; means something now that the kernel reads the flag.
        mov     ah,54h
        int     21h
        mov     [cp_oldvfy],al
        cmp     byte ptr [cp_verify],0
        je      cp_novfy
        mov     ax,2E01h
        int     21h
cp_novfy:

;--- walk the sources ------------------------------------------------
        mov     ebx,[cp_first]
cp_item:
        cmp     ebx,[cp_nitem]
        jae     cp_finish
        mov     esi,[cp_items+ebx*4]
        mov     al,[cp_mode+ebx]        ; this source's own reading of
        mov     [cp_srcmode],al         ; where its end is
        push    ebx
        call    cp_source
        pop     ebx
        jc      cp_stop
        inc     ebx
        jmp     short cp_item

cp_stop:
        call    cp_shut
        call    cp_vfyback
        ret

cp_finish:
        call    cp_shut
        call    cp_vfyback
        cmp     dword ptr [cp_count],0
        jne     cp_report
        mov     edx,OFFSET msg_notfound
        call    print
        ret
cp_report:
        mov     eax,[cp_count]
        call    pdec10
        mov     edx,OFFSET msg_copied
        call    print
        ret

; VERIFY goes back to whatever it was, however the copy ended.  A /V
; that left the flag standing would make every write in the machine
; twice as slow until the next reboot, which is a thing nobody would
; connect to a COPY they typed an hour ago.
cp_vfyback:
        push    eax
        mov     al,[cp_oldvfy]
        and     al,1
        mov     ah,2Eh
        int     21h
        pop     eax
        ret

cp_usage:
        mov     edx,OFFSET msg_needarg
        call    print
        ret

;---------------------------------------------------------------------
; cp_parse - cut [arg_ptr] into cp_items / cp_glue / cp_nitem.
;            CF set if there is nothing to do.  Quoted items survive
;            whole in advanced mode, spaces and all.
;---------------------------------------------------------------------
cp_parse:
        xor     ebx,ebx
        mov     byte ptr [cp_pend],0
cpp_next:
        call    skip_blanks
        mov     al,[esi]
        or      al,al
        jz      cpp_done
        cmp     al,'/'
        je      cpp_sw
        cmp     al,'+'
        jne     cpp_item
        mov     byte ptr [cp_pend],1    ; a '+' standing on its own
        mov     byte ptr [cp_glued],1
        inc     esi
        jmp     short cpp_next

; COPY'S SWITCHES ARE PARSED HERE AND NOT BY sw_scan, because two of
; the three are POSITIONAL and sw_scan's answer is a set.  "/A applies
; to the file it follows and to every file after it until another /A
; or /B" is a rule about WHERE the switch was written, and a bit in a
; mask has forgotten that by the time anybody reads it.
cpp_sw:
        mov     al,[esi+1]
        call    upcase
        call    cpp_isswl
        jnc     cpp_item                ; not one of ours: a path, then
        call    cpp_apply
        add     esi,2
        jmp     short cpp_next

cpp_item:
        cmp     ebx,CP_MAXITEM
        jae     cpp_done
        cmp     byte ptr [adv_parse],0
        je      cpp_bare
        cmp     al,'"'
        jne     cpp_bare
        inc     esi
        call    cpp_record
        inc     ebx
cpp_q:
        mov     al,[esi]
        or      al,al
        jz      cpp_done
        cmp     al,'"'
        je      cpp_qend
        inc     esi
        jmp     short cpp_q
cpp_qend:
        mov     byte ptr [esi],0
        inc     esi
        jmp     short cpp_next
cpp_bare:
        call    cpp_record
        inc     ebx
cpp_scan:
        mov     al,[esi]
        or      al,al
        jz      cpp_done
        cmp     al,' '
        je      cpp_cut
        cmp     al,9
        je      cpp_cut
        cmp     al,'+'
        je      cpp_plus
        cmp     al,'/'                  ; "FILE.TXT/B" is a real form, and
        jne     cpp_step                ; the only way to say which of two
        call    cpp_issw                ; joined files is which
        jc      cpp_swcut
cpp_step:
        inc     esi
        jmp     short cpp_scan
cpp_swcut:
        mov     byte ptr [esi],0        ; the name ends where the switch
        inc     esi                     ; begins, and the '/' is what ends
        mov     al,[esi]                ; it - so the letter is read from
        call    upcase                  ; where the '/' used to be plus one
        call    cpp_apply
        inc     esi
        jmp     short cpp_next
cpp_plus:
        mov     byte ptr [esi],0        ; "A+B" - cut here and glue on
        inc     esi
        mov     byte ptr [cp_pend],1
        mov     byte ptr [cp_glued],1
        jmp     short cpp_next
cpp_cut:
        mov     byte ptr [esi],0
        inc     esi
        jmp     cpp_next
cpp_done:
        mov     [cp_nitem],ebx
        or      ebx,ebx
        jz      cpp_fail
        clc
        ret
cpp_fail:
        stc
        ret

cpp_record:
        mov     [cp_items+ebx*4],esi
        mov     al,[cp_pend]
        mov     [cp_glue+ebx],al
        mov     al,[cp_curmode]         ; whichever of /A and /B is in
        mov     [cp_mode+ebx],al        ; force where this item was written
        mov     byte ptr [cp_pend],0
        ret

;---------------------------------------------------------------------
; cpp_isswl - AL is an uppercased letter; CF set if it is one of
;             COPY's three.
;---------------------------------------------------------------------
cpp_isswl:
        cmp     al,'V'
        je      cis_yes
        cmp     al,'A'
        je      cis_yes
        cmp     al,'B'
        je      cis_yes
        clc
        ret
cis_yes:
        stc
        ret

;---------------------------------------------------------------------
; cpp_issw - ESI -> a '/' inside a name.  CF set if what follows is
;            one of COPY's switches AND nothing but a delimiter comes
;            after it, which is what tells "FILE/B" from a path with
;            a forward slash in it.
;---------------------------------------------------------------------
cpp_issw:
        push    eax
        mov     al,[esi+1]
        call    upcase
        call    cpp_isswl
        jnc     cis_no
        mov     al,[esi+2]
        or      al,al
        jz      cis_ok
        cmp     al,' '
        je      cis_ok
        cmp     al,9
        je      cis_ok
        cmp     al,'+'
        je      cis_ok
        cmp     al,'/'
        je      cis_ok
cis_no:
        pop     eax
        clc
        ret
cis_ok:
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; cpp_apply - AL = V, A or B.  /V is about the whole copy; /A and /B
;             are about this point in the line and everything after
;             it - AND about the file just before it, which is the
;             half that makes them positional.
;---------------------------------------------------------------------
cpp_apply:
        cmp     al,'V'
        jne     cpa_ab
        mov     byte ptr [cp_verify],1
        ret
cpa_ab:
        mov     ah,CPM_BINARY
        cmp     al,'B'
        je      cpa_set
        mov     ah,CPM_ASCII
cpa_set:
        mov     [cp_curmode],ah
        or      ebx,ebx
        jz      cpa_out
        mov     [cp_mode+ebx-1],ah
cpa_out:
        ret

;---------------------------------------------------------------------
; cp_isplace - CF clear if the destination names somewhere to put
;              files rather than a file to make: absent, a bare drive,
;              a trailing backslash, or an existing directory.
;---------------------------------------------------------------------
cp_isplace:
        push    esi
        push    edi
        mov     esi,[cp_dstptr]
        or      esi,esi
        jz      cpi_yes                 ; absent = the current directory
        mov     edi,esi
cpi_end:
        cmp     byte ptr [edi],0
        je      cpi_last
        inc     edi
        jmp     short cpi_end
cpi_last:
        cmp     edi,esi
        je      cpi_yes                 ; empty text, same thing
        dec     edi
        cmp     byte ptr [edi],'\'
        je      cpi_yes
        cmp     byte ptr [edi],':'
        je      cpi_yes

        mov     edx,esi                 ; ask the filesystem
        mov     ecx,ATTR_DIR
        call    xfind_first
        jc      cpi_no
        mov     bx,ax
        call    xfind_close
        test    byte ptr [find_data],ATTR_DIR
        jz      cpi_no
cpi_yes:
        pop     edi
        pop     esi
        clc
        ret
cpi_no:
        pop     edi
        pop     esi
        stc
        ret

;---------------------------------------------------------------------
; cp_source - ESI -> one source item.  A wildcard is walked; anything
;             else is copied as it stands.  CF set = give up entirely
;             (out of space, or the destination will not open).
;---------------------------------------------------------------------
cp_source:
        call    has_wild
        jnc     cp_plain

        push    esi                     ; remember the pattern's own
        call    cp_srcdir               ; directory - the find only
        pop     esi                     ; ever answers with bare names
        mov     edx,esi
        xor     ecx,ecx                 ; ordinary files, no directories
        call    xfind_first
        jc      cps_none
        mov     [cp_hdl],ax
cps_lp:
        mov     esi,OFFSET find_data + 2Ch
        call    cp_fullsrc              ; cp_srcbuf = directory + name
        mov     esi,OFFSET cp_srcbuf
        call    cp_file
        jc      cps_stop
        mov     bx,[cp_hdl]
        call    xfind_next
        jnc     cps_lp
        mov     bx,[cp_hdl]
        call    xfind_close
cps_none:
        clc
        ret
cps_stop:
        mov     bx,[cp_hdl]
        call    xfind_close
        stc
        ret

cp_plain:
        call    cp_file
        ret

;---------------------------------------------------------------------
; cp_srcdir - ESI -> a path; cp_dirbuf gets everything up to and
;             including its last '\' or ':' (empty if it has neither).
;---------------------------------------------------------------------
cp_srcdir:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     edi,OFFSET cp_dirbuf
        mov     byte ptr [edi],0
        xor     edx,edx                 ; EDX = length to keep
        xor     ecx,ecx
csd_scan:
        mov     al,[esi+ecx]
        or      al,al
        jz      csd_have
        inc     ecx
        cmp     al,'\'
        je      csd_mark
        cmp     al,':'
        jne     csd_scan
csd_mark:
        mov     edx,ecx
        jmp     short csd_scan
csd_have:
        xor     ecx,ecx
csd_cp:
        cmp     ecx,edx
        jae     csd_end
        mov     al,[esi+ecx]
        mov     [edi+ecx],al
        inc     ecx
        jmp     short csd_cp
csd_end:
        mov     byte ptr [edi+ecx],0
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; cp_fullsrc - cp_srcbuf = cp_dirbuf + the name at ESI.
;---------------------------------------------------------------------
cp_fullsrc:
        push    eax
        push    esi
        push    edi
        mov     edi,OFFSET cp_srcbuf
        push    esi
        mov     esi,OFFSET cp_dirbuf
cpf_d:
        lodsb
        or      al,al
        jz      cpf_dend
        stosb
        jmp     short cpf_d
cpf_dend:
        pop     esi
cpf_n:
        lodsb
        stosb
        or      al,al
        jnz     cpf_n
        pop     edi
        pop     esi
        pop     eax
        ret

;---------------------------------------------------------------------
; cp_basename - ESI -> a path; returns ESI -> the part after its last
;               '\' or ':'.
;---------------------------------------------------------------------
cp_basename:
        push    eax
        push    ecx
        mov     ecx,esi
cpb_lp:
        mov     al,[esi]
        or      al,al
        jz      cpb_done
        inc     esi
        cmp     al,'\'
        je      cpb_mark
        cmp     al,':'
        jne     cpb_lp
cpb_mark:
        mov     ecx,esi
        jmp     short cpb_lp
cpb_done:
        mov     esi,ecx
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; cp_makedst - cp_dest = where the file at ESI is to go.  Either the
;              destination as typed, or the destination directory with
;              the source's own name on the end.
;---------------------------------------------------------------------
cp_makedst:
        push    eax
        push    ecx
        push    esi
        push    edi
        mov     edi,OFFSET cp_dest
        cmp     byte ptr [cp_todir],0
        jne     cpm_dir

        mov     esi,[cp_dstptr]         ; a file, named outright
cpm_cp:
        lodsb
        stosb
        or      al,al
        jnz     cpm_cp
        jmp     short cpm_out

cpm_dir:
        push    esi
        mov     esi,[cp_dstptr]
        or      esi,esi
        jz      cpm_nodir               ; the current directory
        xor     ecx,ecx                 ; ECX = last character written
cpm_dcp:
        lodsb
        or      al,al
        jz      cpm_dend
        stosb
        mov     ecx,eax
        jmp     short cpm_dcp
cpm_dend:
        cmp     cl,'\'                  ; one separator, never two
        je      cpm_nodir
        cmp     cl,':'
        je      cpm_nodir
        mov     al,'\'
        stosb
cpm_nodir:
        pop     esi
        call    cp_basename
cpm_ncp:
        lodsb
        stosb
        or      al,al
        jnz     cpm_ncp
cpm_out:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; cp_samefile - CF clear if the path at ESI and cp_dest name the same
;               thing, letter for letter, ignoring case.  Not a
;               complete answer (".\A" and "A" are the same file and
;               do not compare equal), but it catches COPY A A and,
;               more importantly, a wildcard that has just matched the
;               file it is being concatenated into.
;---------------------------------------------------------------------
cp_samefile:
        push    esi
        push    edi
        mov     edi,OFFSET cp_dest
cps_cmp:
        mov     al,[esi]
        mov     ah,[edi]
        call    upcase
        xchg    al,ah
        call    upcase
        cmp     al,ah
        jne     cps_differ
        or      al,al
        jz      cps_same
        inc     esi
        inc     edi
        jmp     short cps_cmp
cps_same:
        pop     edi
        pop     esi
        clc
        ret
cps_differ:
        pop     edi
        pop     esi
        stc
        ret

;---------------------------------------------------------------------
; cp_file - copy the one file named at ESI.  CF set = abandon the
;           whole command; a source that simply will not open is
;           reported and stepped over.
;---------------------------------------------------------------------
cp_file:
        cmp     byte ptr [cp_opened],0
        jne     cpx_chosen              ; the output is open, and named
        call    cp_makedst
cpx_chosen:
        call    cp_samefile
        jc      cpx_go
        mov     edx,OFFSET msg_self     ; reading into our own output
        call    print
        clc
        ret

cpx_go:
        push    esi
        mov     edx,esi
        mov     ax,3D00h                ; the source
        int     21h
        pop     esi
        jc      cpx_nosrc
        movzx   eax,ax
        mov     [copy_src],eax

        cmp     byte ptr [cp_opened],0  ; one output, opened once
        jne     cpx_pump
        call    cp_opendst
        jc      cpx_nodst
        mov     byte ptr [cp_opened],1

cpx_pump:
        mov     ebx,[copy_src]
        mov     ecx,COPYBUF_SIZE
        mov     edx,OFFSET copy_buf
        mov     ah,3Fh
        int     21h
        jc      cpx_werr
        or      eax,eax
        jz      cpx_done
        mov     ecx,eax

        ; /A: A CTRL-Z ENDS THE FILE, and does not travel.  This is
        ; what "ASCII" meant when it was written on a switch: a text
        ; file of the CP/M line ends at the first 1Ah because its
        ; length was only ever known to the nearest 128 bytes, and
        ; COPY has honoured that ever since.  A binary copy sees the
        ; same byte as a byte, which is why the two modes exist.
        cmp     byte ptr [cp_srcmode],CPM_ASCII
        jne     cpx_write
        push    ecx
        mov     edi,OFFSET copy_buf
        mov     al,1Ah
        repne   scasb
        pop     ecx                     ; the count back - POP leaves the
        jne     cpx_write               ; flags scasb set alone
        mov     ecx,edi
        sub     ecx,OFFSET copy_buf
        dec     ecx                     ; ...and the Ctrl-Z itself stays
        mov     byte ptr [cp_ztail],1   ; behind
cpx_write:
        or      ecx,ecx
        jz      cpx_chkz
        mov     ebx,[copy_dst]
        mov     ah,40h
        int     21h
        jc      cpx_werr
        cmp     eax,ecx
        jb      cpx_full
cpx_chkz:
        cmp     byte ptr [cp_ztail],0
        jne     cpx_done
        jmp     short cpx_pump

cpx_done:
        mov     byte ptr [cp_ztail],0
        inc     dword ptr [cp_count]
        call    cp_shutsrc
        cmp     byte ptr [cp_concat],0
        jne     cpx_keep                ; the output stays open for the
        call    cp_shutdst              ; next source
cpx_keep:
        clc
        ret

cpx_nosrc:
        mov     edx,OFFSET msg_notfound
        call    print
        clc                             ; one bad name is not the end
        ret
cpx_nodst:
        call    cp_shutsrc
        mov     edx,OFFSET msg_nodest
        call    print
        stc
        ret
cpx_full:
        call    cp_shutsrc
        mov     edx,OFFSET msg_full
        call    print
        stc
        ret
cpx_werr:
        call    cp_shutsrc
        mov     edx,OFFSET msg_wrerr
        call    print
        stc
        ret

;---------------------------------------------------------------------
; cp_opendst - open or make cp_dest.  Appending seeks to the end
;              first, which is what makes "COPY A+B" safe.
;---------------------------------------------------------------------
cp_opendst:
        cmp     byte ptr [cp_append],0
        jne     cpo_append
        mov     edx,OFFSET cp_dest
        xor     ecx,ecx
        mov     ah,3Ch
        int     21h
        jc      cpo_fail
        movzx   eax,ax
        mov     [copy_dst],eax
        clc
        ret
cpo_append:
        mov     edx,OFFSET cp_dest
        mov     ax,3D01h                ; write access to what is there
        int     21h
        jc      cpo_fail
        movzx   eax,ax
        mov     [copy_dst],eax
        mov     ebx,eax
        xor     ecx,ecx
        mov     ax,4202h                ; ...positioned at its end
        int     21h
        clc
        ret
cpo_fail:
        stc
        ret

;---------------------------------------------------------------------
; cp_shutsrc / cp_shutdst / cp_shut - close what is open, once.
;---------------------------------------------------------------------
cp_shutsrc:
        push    eax
        push    ebx
        mov     ebx,[copy_src]
        cmp     ebx,-1
        je      cpss_out
        mov     ah,3Eh
        int     21h
        mov     dword ptr [copy_src],-1
cpss_out:
        pop     ebx
        pop     eax
        ret

cp_shutdst:
        push    eax
        push    ebx
        mov     ebx,[copy_dst]
        cmp     ebx,-1
        je      cpsd_out

        ; /A on the DESTINATION puts the Ctrl-Z back on, which is the
        ; other half of the same convention: a text file made by
        ; joining text files should end the way one does.
        cmp     byte ptr [cp_dmode],CPM_ASCII
        jne     cpsd_close
        push    ecx
        push    edx
        mov     byte ptr [cp_zbyte],1Ah
        mov     edx,OFFSET cp_zbyte
        mov     ecx,1
        mov     ah,40h
        int     21h
        pop     edx
        pop     ecx
        mov     ebx,[copy_dst]
cpsd_close:
        mov     ah,3Eh
        int     21h
        mov     dword ptr [copy_dst],-1
        mov     byte ptr [cp_opened],0
cpsd_out:
        pop     ebx
        pop     eax
        ret

cp_shut:
        call    cp_shutsrc
        call    cp_shutdst
        ret

;=====================================================================
; DEL / ERASE pattern
;=====================================================================
cmd_del:
        call    need_arg
        jc      cd_out

        call    sw_scan                 ; /P before the name is read
        mov     [del_sw],eax
        cmp     byte ptr [esi],0        ; "DEL /P" and nothing else
        je      cd_usage

        ; deleting everything wants a nod first - unless /P is going
        ; to ask about every one of them anyway
        test    dword ptr [del_sw],SW_P
        jnz     cd_go
        mov     edi,esi
        call    is_star
        jnc     cd_go
        mov     edx,OFFSET msg_sure
        call    print
        mov     ah,01h
        int     21h
        call    put_crlf
        call    upcase
        cmp     al,'Y'
        jne     cd_out

cd_go:
        ; /S: THE SAME DELETION IN EVERY DIRECTORY UNDERNEATH, over the
        ; walker DIR /S is built on.  It is not an MS-DOS 6 switch -
        ; DELTREE is what that DOS offered and it deletes DIRECTORIES,
        ; which is a different and much more dangerous verb.  This is
        ; the one everybody means: the same pattern, everywhere below
        ; here.  It goes through dir_resolve, so "DEL /S *.BAK" starts
        ; at the current directory and "DEL /S C:\*.BAK" at a root.
        test    dword ptr [del_sw],SW_S
        jz      cd_flat
        mov     esi,[arg_ptr]
        call    dir_resolve
        jc      cd_flat                 ; not truenameable: one directory
        mov     dword ptr [del_count],0
        mov     dword ptr [dir_depth],0
        mov     dword ptr [walk_act],OFFSET del_one
        mov     dword ptr [walk_rec],1
        call    tree_walk
        cmp     dword ptr [del_count],0
        je      cd_none
        ret

cd_flat:
        ; the find gives back bare names - keep the directory part of
        ; the argument to glue in front of each one
        mov     esi,[arg_ptr]
        mov     edi,OFFSET del_path
        xor     ecx,ecx                 ; ECX = chars after the last \ or :
        xor     ebx,ebx                 ; EBX = prefix length so far
cdp_scan:
        mov     al,[esi]
        or      al,al
        jz      cdp_done
        mov     [edi],al
        inc     edi
        inc     esi
        inc     ecx
        cmp     al,'\'
        je      cdp_mark
        cmp     al,'/'
        je      cdp_mark
        cmp     al,':'
        jne     cdp_scan
cdp_mark:
        add     ebx,ecx                 ; prefix now includes this char
        xor     ecx,ecx
        jmp     short cdp_scan
cdp_done:
        mov     [del_pfx],ebx

        ; enumerate through the long-name find, so DEL sees what was
        ; typed - the classic find is pure 8.3 by design
        mov     edx,[arg_ptr]
        xor     ecx,ecx                 ; plain files only
        call    xfind_first
        jc      cd_none
        mov     [del_hdl],ax
        xor     ebp,ebp                 ; deleted count
cd_each:
        ; del_path = prefix + the hit's 8.3 alias (unambiguous, and
        ; 41h tombstones the chain along with the file)
        mov     edi,OFFSET del_path
        add     edi,[del_pfx]
        mov     esi,OFFSET find_data + 130h
cd_glue:
        lodsb
        mov     [edi],al
        inc     edi
        or      al,al
        jnz     cd_glue

        test    dword ptr [del_sw],SW_P ; /P asks about each one
        jz      cd_kill
        mov     esi,OFFSET del_path
        call    print_asciiz
        mov     edx,OFFSET msg_delq
        call    print
        mov     ah,01h
        int     21h
        call    put_crlf
        call    upcase
        cmp     al,'Y'
        jne     cd_skip

cd_kill:
        mov     edx,OFFSET del_path
        mov     ah,41h
        int     21h
        jc      cd_skip
        inc     ebp
cd_skip:
        mov     bx,word ptr [del_hdl]
        call    xfind_next
        jnc     cd_each
        mov     bx,word ptr [del_hdl]
        call    xfind_close

        or      ebp,ebp
        jz      cd_none
cd_out:
        ret
cd_none:
        mov     edx,OFFSET msg_notfound
        call    print
        ret
cd_usage:
        mov     edx,OFFSET msg_needarg
        call    print
        ret

;---------------------------------------------------------------------
; del_one - what DEL /S does at one stop of the walk: the pattern in
;           dir_fpat, in the directory dir_dirpath, and each name it
;           deletes is printed IN FULL.
;
; The name is printed because a recursive delete that says nothing is
; a recursive delete nobody can check afterwards, and the bare 8.3
; names a flat DEL would print do not say which directory they were
; in - which is the only interesting thing about them here.
;---------------------------------------------------------------------
del_one:
        call    dir_join                ; dir_scratch = this directory's
        mov     edx,OFFSET dir_scratch  ; worth of the pattern
        xor     ecx,ecx                 ; plain files only
        call    xfind_first
        jc      d1_out
        mov     [del_hdl],ax
d1_each:
        ; del_path = the directory + the hit's 8.3 alias, which is
        ; unambiguous where a long name may not be
        mov     esi,OFFSET dir_dirpath
        mov     edi,OFFSET del_path
        xor     ecx,ecx
d1_dir:
        mov     al,[esi+ecx]
        or      al,al
        jz      d1_dend
        mov     [edi+ecx],al
        inc     ecx
        jmp     short d1_dir
d1_dend:
        or      ecx,ecx
        jz      d1_name
        cmp     byte ptr [edi+ecx-1],'\'
        je      d1_name
        mov     byte ptr [edi+ecx],'\'
        inc     ecx
d1_name:
        add     edi,ecx
        mov     esi,OFFSET find_data + 130h
d1_glue:
        lodsb
        stosb
        or      al,al
        jnz     d1_glue

        test    dword ptr [del_sw],SW_P ; /P asks about each one
        jz      d1_kill
        mov     esi,OFFSET del_path
        call    print_asciiz
        mov     edx,OFFSET msg_delq
        call    print
        mov     ah,01h
        int     21h
        call    put_crlf
        call    upcase
        cmp     al,'Y'
        jne     d1_next

d1_kill:
        mov     edx,OFFSET del_path
        mov     ah,41h
        int     21h
        jc      d1_next
        inc     dword ptr [del_count]
        test    dword ptr [del_sw],SW_P ; /P has already shown the name
        jnz     d1_next
        mov     esi,OFFSET del_path
        call    print_asciiz
        call    put_crlf
d1_next:
        mov     bx,word ptr [del_hdl]
        call    xfind_next
        jnc     d1_each
        mov     bx,word ptr [del_hdl]
        call    xfind_close
d1_out:
        ret

; is_star - CF set if the arg at EDI is "*" or "*.*"
is_star:
        cmp     word ptr [edi],002Ah    ; "*",0
        je      is_yes
        cmp     dword ptr [edi],2A2E2Ah ; "*.*",0
        je      is_yes
        clc
        ret
is_yes:
        stc
        ret

;=====================================================================
; REN / RENAME old new
;=====================================================================
cmd_ren:
        call    two_args
        jc      rn_usage
        mov     edx,[arg1_ptr]
        mov     edi,[arg2_ptr]
        mov     ah,56h
        int     21h
        jc      rn_fail
        ret
rn_fail:
        cmp     ax,ERR_EXISTS
        je      rn_dup
        mov     edx,OFFSET msg_notfound
        call    print
        ret
rn_dup:
        mov     edx,OFFSET msg_dup
        call    print
        ret
rn_usage:
        mov     edx,OFFSET msg_needarg
        call    print
        ret

;=====================================================================
; CLS
;=====================================================================
cmd_cls:
        mov     ax,0F000h               ; PM-DOS console extension
        int     21h
        ret

;=====================================================================
; VER - the SYSTEM's version, which is the kernel's.
;
; COMMAND.COM and PMDOS.SYS are separate binaries with separate
; version numbers, so the interpreter asks (21h/F1h) rather than
; reporting itself.  On the rare occasion the two disagree it says so,
; which beats quietly answering the wrong question.
;=====================================================================
cmd_ver:
        call    put_crlf
        mov     edx,OFFSET msg_ver1
        call    print
        mov     ax,0F100h
        int     21h
        jc      ver_nokern
        movzx   eax,bh
        call    pdec1
        mov     dl,'.'
        mov     ah,02h
        int     21h
        movzx   eax,bl
        call    pdec1
        call    put_crlf

        ; ONE LINE, DELIBERATELY.  VER reports the SYSTEM's version and
        ; the system is PM-DOS.  What 21h/30h answers when a program
        ; asks what DOS this is - 6.22, a compatibility target rather
        ; than a fact about this code - is a different question, and
        ; printing both here invited the reader to take them as two
        ; halves of one answer.  Anything that wants the 30h number
        ; asks 30h.
        cmp     bh,PMDOS_MAJOR
        jne     ver_differ
        cmp     bl,PMDOS_MINOR
        jne     ver_differ
        call    put_crlf
        ret
ver_differ:
        mov     edx,OFFSET msg_ver2
        call    print
        call    put_crlf
        ret
ver_nokern:
        mov     edx,OFFSET msg_verunk
        call    print
        call    put_crlf
        ret

;=====================================================================
; VOL
;=====================================================================
cmd_vol:
        mov     edx,OFFSET dta
        mov     ah,1Ah
        int     21h
        call    arg_drive               ; "VOL C:" asks about C:
        mov     ecx,ATTR_VOLUME
        mov     ah,4Eh
        int     21h                     ; asked before anything is
        jnc     vol_have                ; printed, as in DIR
        cmp     ax,ERR_BADDRIVE
        je      vol_baddrv
        mov     edx,OFFSET msg_vol1
        call    print
        mov     edx,OFFSET msg_nolabel
        call    print
        call    put_crlf
        ret
vol_baddrv:
        mov     edx,OFFSET msg_baddrv
        call    print
        ret
vol_have:
        mov     edx,OFFSET msg_vol1
        call    print
        mov     esi,OFFSET dta + 1Eh
        call    print_asciiz
        call    put_crlf
        ret

;=====================================================================
; DATE [mm-dd-yyyy]
;=====================================================================
cmd_date:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        jne     date_set

        mov     edx,OFFSET msg_curdate
        call    print
        mov     ah,2Ah
        int     21h
        push    ecx
        push    edx
        movzx   eax,dh
        call    pdec2
        mov     dl,'-'
        mov     ah,02h
        int     21h
        pop     eax                     ; saved EDX
        push    eax
        movzx   eax,al                  ; day
        call    pdec2
        mov     dl,'-'
        mov     ah,02h
        int     21h
        pop     edx
        pop     ecx
        movzx   eax,cx
        call    pdec4
        call    put_crlf
        ret

date_set:
        ; parse mm-dd-yyyy (also accepts / as the separator)
        call    parse_num               ; -> EAX, ESI past digits
        jc      date_bad
        mov     [tmp_a],eax
        call    skip_sep
        call    parse_num
        jc      date_bad
        mov     [tmp_b],eax
        call    skip_sep
        call    parse_num
        jc      date_bad
        cmp     eax,100                 ; two-digit years live in 19xx/20xx
        jae     date_year
        cmp     eax,80
        jb      date_20xx
        add     eax,1900
        jmp     short date_year
date_20xx:
        add     eax,2000
date_year:
        mov     ecx,eax                 ; CX = year
        mov     eax,[tmp_a]
        mov     dh,al                   ; month
        mov     eax,[tmp_b]
        mov     dl,al                   ; day
        mov     ah,2Bh
        int     21h
        or      al,al
        jnz     date_bad
        ret
date_bad:
        mov     edx,OFFSET msg_baddate
        call    print
        ret

;=====================================================================
; TIME [hh:mm]
;=====================================================================
cmd_time:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        jne     time_set

        mov     edx,OFFSET msg_curtime
        call    print
        mov     ah,2Ch
        int     21h
        push    ecx
        push    edx
        movzx   eax,ch
        call    pdec2
        mov     dl,':'
        mov     ah,02h
        int     21h
        pop     edx
        pop     ecx
        push    edx
        movzx   eax,cl
        call    pdec2
        mov     dl,':'
        mov     ah,02h
        int     21h
        pop     edx
        movzx   eax,dh
        call    pdec2
        call    put_crlf
        ret

time_set:
        call    parse_num
        jc      time_bad
        mov     [tmp_a],eax             ; hour
        call    skip_sep
        call    parse_num
        jc      time_bad
        mov     cl,al                   ; CL = minute
        mov     eax,[tmp_a]
        mov     ch,al                   ; CH = hour
        xor     dx,dx                   ; seconds = 0
        mov     ah,2Dh
        int     21h
        or      al,al
        jnz     time_bad
        ret
time_bad:
        mov     edx,OFFSET msg_badtime
        call    print
        ret

;=====================================================================
; PAUSE
;=====================================================================
cmd_pause:
        mov     edx,OFFSET msg_pause
        call    print
        mov     ah,08h
        int     21h
        call    put_crlf
        ret

;=====================================================================
; REM
;=====================================================================
cmd_rem:
        ret

;=====================================================================
; EXIT - terminate; the kernel reloads a fresh interpreter.
;
; The permanent shell (the one started with /P) has nowhere to go
; back to, so it ignores EXIT entirely, exactly as MS-DOS does.
;=====================================================================
cmd_exit:
        cmp     byte ptr [perm_flag],0
        jne     exit_no
        mov     ax,4C00h
        int     21h                     ; no return
exit_no:
        ret

;=====================================================================
; ECHO [ON | OFF | text]
;=====================================================================
cmd_echo:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        je      echo_state

        mov     edi,esi
        call    word_is_on
        jnc     echo_on
        call    word_is_off
        jnc     echo_off

        ; ECHO text - measured from one delimiter past the verb, not
        ; from the first non-blank, so that the leading spaces batch
        ; files indent their output with actually come out
        mov     esi,[arg_raw]
        cmp     byte ptr [esi],0
        je      echo_text
        inc     esi
echo_text:
        call    print_asciiz
        call    put_crlf
        ret
echo_on:
        mov     byte ptr [echo_flag],1
        ret
echo_off:
        mov     byte ptr [echo_flag],0
        ret
;---------------------------------------------------------------------
; ECHO. - the one form that prints an empty line rather than the
;         state, and the reason batch files can produce blank output
;         at all.  Text may follow the dot with no space.
;---------------------------------------------------------------------
cmd_echodot:
        mov     esi,[arg_ptr]
        call    print_asciiz
        call    put_crlf
        ret

echo_state:
        mov     edx,OFFSET msg_echois
        call    print
        cmp     byte ptr [echo_flag],0
        je      echo_soff
        mov     edx,OFFSET msg_on
        jmp     short echo_sprint
echo_soff:
        mov     edx,OFFSET msg_off
echo_sprint:
        call    print
        call    put_crlf
        ret

;=====================================================================
; BREAK / VERIFY [ON | OFF] - accepted and remembered, nothing more
;=====================================================================
cmd_break:
        mov     ecx,OFFSET break_flag
        mov     edx,OFFSET msg_breakis
        jmp     short onoff_common
cmd_verify:
        mov     ecx,OFFSET verify_flag
        mov     edx,OFFSET msg_verifyis
onoff_common:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        je      onoff_show
        mov     edi,esi
        call    word_is_on
        jnc     onoff_on
        call    word_is_off
        jnc     onoff_off
        mov     edx,OFFSET msg_badparm
        call    print
        ret
onoff_on:
        mov     byte ptr [ecx],1
        ret
onoff_off:
        mov     byte ptr [ecx],0
        ret
onoff_show:
        call    print
        cmp     byte ptr [ecx],0
        je      onoff_soff
        mov     edx,OFFSET msg_on
        jmp     short onoff_sprint
onoff_soff:
        mov     edx,OFFSET msg_off
onoff_sprint:
        call    print
        call    put_crlf
        ret

;=====================================================================
; SET [NAME=value] - and PATH / PROMPT, which are env vars in disguise
;=====================================================================
cmd_path:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        je      path_show

        ; "PATH;" (or "PATH ;") clears the search path, as DOS has
        ; always had it - the verb scanner ends a verb at ';' without
        ; eating it, so both spellings arrive here the same way.
        cmp     byte ptr [esi],';'
        jne     path_set
        cmp     byte ptr [esi+1],0
        jne     path_set
        mov     esi,OFFSET lit_pathdel   ; "PATH=" - env_set deletes
        call    env_set
        ret

path_set:
        mov     edi,OFFSET set_scratch
        mov     byte ptr [edi],'P'
        mov     byte ptr [edi+1],'A'
        mov     byte ptr [edi+2],'T'
        mov     byte ptr [edi+3],'H'
        mov     byte ptr [edi+4],'='
        add     edi,5
        jmp     set_build
path_show:
        mov     esi,OFFSET name_path
        call    env_find
        jc      path_none
        push    esi                     ; DOS echoes it back in the
        mov     edx,OFFSET msg_pathis   ; form you would type
        call    print
        pop     esi
        call    print_asciiz
        call    put_crlf
        ret
path_none:
        mov     edx,OFFSET msg_nopath
        call    print
        ret

cmd_prompt:
        mov     esi,[arg_ptr]
        mov     edi,OFFSET set_scratch
        mov     byte ptr [edi],'P'
        mov     byte ptr [edi+1],'R'
        mov     byte ptr [edi+2],'O'
        mov     byte ptr [edi+3],'M'
        mov     byte ptr [edi+4],'P'
        mov     byte ptr [edi+5],'T'
        mov     byte ptr [edi+6],'='
        add     edi,7
        jmp     set_build               ; empty arg deletes = default

cmd_set:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        jne     set_assign

        ; no argument: list the environment
        mov     esi,OFFSET env_blk
set_list:
        cmp     byte ptr [esi],0
        je      set_done
        call    print_asciiz
        call    put_crlf
set_skip:
        lodsb
        or      al,al
        jnz     set_skip
        jmp     short set_list
set_done:
        ret

set_assign:
        mov     edi,OFFSET set_scratch
set_build:
        ; copy "NAME=value" (name uppercased) into set_scratch at EDI
        xor     ecx,ecx
        mov     bl,0                    ; before the '='
sb_lp:
        mov     al,[esi]
        or      al,al
        jz      sb_end
        or      bl,bl
        jnz     sb_store               ; past '=': verbatim
        cmp     al,'='
        jne     sb_up
        mov     bl,1
        jmp     short sb_store
sb_up:
        call    upcase
sb_store:
        cmp     ecx,120
        jae     sb_end
        mov     [edi],al
        inc     edi
        inc     ecx
        inc     esi
        jmp     short sb_lp
sb_end:
        mov     byte ptr [edi],0
        mov     esi,OFFSET set_scratch
        call    env_set
        jc      sb_full
        ret
sb_full:
        mov     edx,OFFSET msg_envfull
        call    print
        ret

;=====================================================================
; MD / MKDIR
;=====================================================================
cmd_md:
        call    need_arg
        jc      md_out
        mov     edx,esi
        mov     ah,39h
        int     21h
        jc      md_fail
md_out:
        ret
md_fail:
        mov     edx,OFFSET msg_mdfail
        call    print
        ret

;=====================================================================
; CD / CHDIR - change, or show the current directory
;=====================================================================
cmd_cd:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        je      cd_show
        call    unquote_arg
        mov     edx,esi
        mov     ah,3Bh
        int     21h
        jc      cd_fail
        ret
cd_fail:
        mov     edx,OFFSET msg_cdfail
        call    print
        ret
cd_show:
        call    put_cwd
        call    put_crlf
        ret

;=====================================================================
; RD / RMDIR
;=====================================================================
cmd_rd:
        call    need_arg
        jc      rd_out
        mov     edx,esi
        mov     ah,3Ah
        int     21h
        jc      rd_fail
rd_out:
        ret
rd_fail:
        mov     edx,OFFSET msg_rdfail
        call    print
        ret

;---------------------------------------------------------------------
; put_cwd - print "X:\PATH"
;---------------------------------------------------------------------
;---------------------------------------------------------------------
; arg_drive - look for a leading "X:" on the first argument.
;             [dir_drv] = 1-based drive for INT 21h/36h (0 = default),
;             EDX -> the "\*.*" root pattern on that drive, for the
;             volume-label search.
;---------------------------------------------------------------------
arg_drive:
        push    eax
        push    esi
        mov     byte ptr [dir_drv],0
        mov     edx,OFFSET pat_root
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi+1],':'
        jne     ad_out
        mov     al,[esi]
        call    upcase
        sub     al,'A'
        cmp     al,LASTDRIVE            ; and NOT a number of its own
        jae     ad_out
        mov     ah,al
        add     ah,'A'
        mov     [dir_lbl],ah            ; patch the pattern's drive letter
        inc     al
        mov     [dir_drv],al
        mov     edx,OFFSET dir_lbl
ad_out:
        pop     esi
        pop     eax
        ret

;---------------------------------------------------------------------
; put_dcwd - like put_cwd, but for the drive found by arg_drive: the
;            DIR header should name the drive being listed, not the
;            one we happen to stand on.
;---------------------------------------------------------------------
put_dcwd:
        push    eax
        push    edx
        push    esi
        mov     al,[dir_drv]
        or      al,al
        jz      pd_default
        add     al,'A'-1
        jmp     short pd_letter
pd_default:
        mov     ah,19h
        int     21h
        add     al,'A'
pd_letter:
        mov     dl,al
        mov     ah,02h
        int     21h
        mov     dl,':'
        mov     ah,02h
        int     21h
        mov     dl,'\'
        mov     ah,02h
        int     21h
        mov     esi,OFFSET cwd_buf
        mov     dl,[dir_drv]            ; that drive's cwd (0 = default)
        mov     ah,47h
        int     21h
        mov     esi,OFFSET cwd_buf
        call    print_asciiz
        pop     esi
        pop     edx
        pop     eax
        ret

put_cwd:
        push    eax
        push    edx
        push    esi
        mov     ah,19h
        int     21h
        add     al,'A'
        mov     dl,al
        mov     ah,02h
        int     21h
        mov     dl,':'
        mov     ah,02h
        int     21h
        mov     dl,'\'
        mov     ah,02h
        int     21h
        mov     esi,OFFSET cwd_buf
        mov     dl,0                    ; the default drive's cwd
        mov     ah,47h
        int     21h
        mov     esi,OFFSET cwd_buf
        call    print_asciiz
        pop     esi
        pop     edx
        pop     eax
        ret

;=====================================================================
; Environment block routines
;
; env_blk: back-to-back ASCIIZ "NAME=value" strings, empty string ends.
;=====================================================================

;---------------------------------------------------------------------
; env_find - ESI -> variable name (ASCIIZ, caller-uppercased).
;            CF clear: ESI -> its value text.  CF set: not defined.
;---------------------------------------------------------------------
env_find:
        push    ebx
        push    edi
        mov     ebx,OFFSET env_blk
ef_entry:
        cmp     byte ptr [ebx],0
        je      ef_miss
        push    esi
        mov     edi,ebx
ef_cmp:
        mov     al,[esi]
        or      al,al
        jz      ef_nameend
        cmp     al,[edi]
        jne     ef_next
        inc     esi
        inc     edi
        jmp     short ef_cmp
ef_nameend:
        cmp     byte ptr [edi],'='
        jne     ef_next
        pop     esi
        lea     esi,[edi+1]
        pop     edi
        pop     ebx
        clc
        ret
ef_next:
        pop     esi
ef_skip:
        mov     al,[ebx]
        inc     ebx
        or      al,al
        jnz     ef_skip
        jmp     short ef_entry
ef_miss:
        pop     edi
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; env_set - ESI -> "NAME=value" (or "NAME=" to delete).  Replaces any
;           existing entry.  CF set if the block is full.
;---------------------------------------------------------------------
env_set:
        pushad

        ; find and remove an existing entry with this name
        mov     ebx,OFFSET env_blk
es_scan:
        cmp     byte ptr [ebx],0
        je      es_removed
        push    esi
        mov     edi,ebx
es_cmp:
        mov     al,[esi]
        cmp     al,'='
        je      es_nameend
        cmp     al,[edi]
        jne     es_next
        inc     esi
        inc     edi
        jmp     short es_cmp
es_nameend:
        cmp     byte ptr [edi],'='
        je      es_kill
es_next:
        pop     esi
es_walk:                                ; step EBX past the current entry
        mov     al,[ebx]
        inc     ebx
        or      al,al
        jnz     es_walk
        jmp     short es_scan

es_kill:
        pop     esi
        ; slide the rest of the block down over the doomed entry
        push    esi
        mov     edi,ebx                 ; EDI = hole start
        mov     esi,ebx
es_measure:
        lodsb                           ; ESI ends up past the entry
        or      al,al
        jnz     es_measure
es_slide:
        mov     al,[esi]
        mov     [edi],al
        inc     edi
        inc     esi
        or      al,al
        jnz     es_slide                ; keep going inside a string
        cmp     byte ptr [esi],0        ; two NULs = end of block moved
        jne     es_slide
        mov     byte ptr [edi],0
        pop     esi

es_removed:
        ; nothing to add if the value part is empty
        push    esi
es_vfind:
        lodsb
        or      al,al
        jz      es_vnone                ; malformed - treat as delete
        cmp     al,'='
        jne     es_vfind
        cmp     byte ptr [esi],0
        je      es_vnone                ; "NAME=" - delete only
        pop     esi

        ; find the end of the block and append
        mov     edi,OFFSET env_blk
es_end:
        cmp     byte ptr [edi],0
        je      es_have_end
es_end2:
        mov     al,[edi]
        inc     edi
        or      al,al
        jnz     es_end2
        jmp     short es_end
es_have_end:
        ; room?  need strlen(new)+1 plus the terminator
        push    esi
        mov     ecx,0
es_len:
        lodsb
        inc     ecx
        or      al,al
        jnz     es_len
        pop     esi
        mov     eax,edi
        add     eax,ecx
        inc     eax
        cmp     eax,OFFSET env_blk + ENV_SIZE
        jae     es_full
es_copy:
        lodsb
        stosb
        or      al,al
        jnz     es_copy
        mov     byte ptr [edi],0        ; new end-of-block
        popad
        clc
        ret
es_vnone:
        pop     esi
        popad
        clc
        ret
es_full:
        popad
        stc
        ret

;=====================================================================
; The prompt.  PROMPT codes: $P path, $N drive, $G > $L < $B | $Q =
; $D date, $T time, $V version, $H backspace, $_ newline, $E escape,
; $$ dollar.  That is the whole MS-DOS 6.22 set.  Default $P$G.
;=====================================================================
put_prompt:
        mov     esi,OFFSET name_prompt
        call    env_find
        jnc     pp_have
        mov     esi,OFFSET dflt_prompt
pp_have:
pp_lp:
        lodsb
        or      al,al
        jz      pp_done
        cmp     al,'$'
        je      pp_code
        mov     dl,al
        mov     ah,02h
        int     21h
        jmp     short pp_lp
pp_code:
        lodsb
        or      al,al
        jz      pp_done
        call    upcase
        cmp     al,'P'
        je      pp_path
        cmp     al,'N'
        je      pp_drive
        cmp     al,'G'
        je      pp_gt
        cmp     al,'L'
        je      pp_lt
        cmp     al,'B'
        je      pp_bar
        cmp     al,'Q'
        je      pp_eq
        cmp     al,'D'
        je      pp_date
        cmp     al,'T'
        je      pp_time
        cmp     al,'_'
        je      pp_nl
        cmp     al,'E'
        je      pp_esc
        cmp     al,'H'
        je      pp_bs
        cmp     al,'V'
        je      pp_ver
        cmp     al,'$'
        je      pp_dollar
        jmp     short pp_lp             ; unknown code: swallowed

pp_path:
        push    esi
        call    put_cwd
        pop     esi
        jmp     short pp_lp
pp_drive:
        call    pp_put_drive
        jmp     short pp_lp
pp_gt:
        mov     dl,'>'
        jmp     short pp_ch
pp_lt:
        mov     dl,'<'
        jmp     short pp_ch
pp_bar:
        mov     dl,'|'
        jmp     short pp_ch
pp_eq:
        mov     dl,'='
        jmp     short pp_ch
pp_esc:
        mov     dl,27
        jmp     short pp_ch
pp_bs:
        mov     dl,8                    ; the console's own backspace
        jmp     short pp_ch             ; erases, so $H needs no help
pp_dollar:
        mov     dl,'$'
pp_ch:
        mov     ah,02h
        int     21h
        jmp     short pp_lp
pp_nl:
        call    put_crlf
        jmp     short pp_lp
pp_date:
        push    esi
        mov     ah,2Ah
        int     21h
        push    ecx
        push    edx
        movzx   eax,dh
        call    pdec2
        mov     dl,'-'
        mov     ah,02h
        int     21h
        pop     eax
        push    eax
        movzx   eax,al
        call    pdec2
        mov     dl,'-'
        mov     ah,02h
        int     21h
        pop     edx
        pop     ecx
        movzx   eax,cx
        call    pdec4
        pop     esi
        jmp     pp_lp
pp_time:
        push    esi
        mov     ah,2Ch
        int     21h
        movzx   eax,ch
        call    pdec2
        mov     dl,':'
        mov     ah,02h
        int     21h
        movzx   eax,cl
        call    pdec2
        pop     esi
        jmp     pp_lp
pp_ver:
        ; the same question VER asks, and the same answer: the KERNEL's
        ; version, not the interpreter's idea of it
        push    esi
        mov     edx,OFFSET msg_ver1
        call    print
        mov     ax,0F100h
        int     21h
        jc      ppv_out                 ; no answer, nothing to show
        movzx   eax,bh
        call    pdec1
        mov     dl,'.'
        mov     ah,02h
        int     21h
        movzx   eax,bl
        call    pdec1
ppv_out:
        pop     esi
        jmp     pp_lp

pp_done:
        ret

pp_put_drive:
        push    esi
        mov     ah,19h
        int     21h
        add     al,'A'
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     esi
        ret

;=====================================================================
; Helpers
;=====================================================================

; print - 21h/09h on the '$' string at EDX
print:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; print_asciiz - the NUL-terminated string at ESI
print_asciiz:
        push    eax
        push    edx
        push    esi
pa_lp:
        mov     dl,[esi]
        or      dl,dl
        jz      pa_done
        mov     ah,02h
        int     21h
        inc     esi
        jmp     short pa_lp
pa_done:
        pop     esi
        pop     edx
        pop     eax
        ret

put_crlf:
        push    eax
        push    edx
        mov     edx,OFFSET msg_crlf
        mov     ah,09h
        int     21h
        pop     edx
        pop     eax
        ret

; upcase - AL lower -> upper
upcase:
        cmp     al,'a'
        jb      uc_out
        cmp     al,'z'
        ja      uc_out
        sub     al,20h
uc_out:
        ret

; skip_blanks - ESI past spaces and tabs
skip_blanks:
        cmp     byte ptr [esi],' '
        je      sk_step
        cmp     byte ptr [esi],9
        jne     sk_out
sk_step:
        inc     esi
        jmp     short skip_blanks
sk_out:
        ret

; need_arg - ESI = [arg_ptr], surrounding quotes stripped in advanced
;            mode; CF + message if there is none.  [arg_ptr] is
;            updated too, so a handler re-reading it sees the same
;            unquoted text.
need_arg:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        je      na_none
        call    unquote_arg
        mov     [arg_ptr],esi
        clc
        ret
na_none:
        mov     edx,OFFSET msg_needarg
        call    print
        stc
        ret

;---------------------------------------------------------------------
; unquote_arg - in advanced mode, a leading '"' at ESI is stripped and
;               its closing partner (or the end of the text) becomes
;               the terminator, IN PLACE.  ESI comes back pointing at
;               the bare text.  Strict (.BAT) mode returns unchanged:
;               quotes are ordinary characters there.
;---------------------------------------------------------------------
unquote_arg:
        cmp     byte ptr [adv_parse],0
        je      uq_out
        cmp     byte ptr [esi],'"'
        jne     uq_out
        inc     esi
        push    esi
uq_find:
        mov     al,[esi]
        or      al,al
        jz      uq_done                 ; unterminated: to the end
        cmp     al,'"'
        je      uq_close
        inc     esi
        jmp     short uq_find
uq_close:
        mov     byte ptr [esi],0
uq_done:
        pop     esi
uq_out:
        ret

; two_args - split [arg_ptr] into [arg1_ptr] / [arg2_ptr].  CF if
;            either is missing.  In advanced mode a "quoted argument"
;            is one argument, spaces and all, and the quotes are
;            stripped; otherwise the split is at the first blank,
;            exactly as it always was.
two_args:
        mov     esi,[arg_ptr]
        cmp     byte ptr [esi],0
        je      ta_fail

        cmp     byte ptr [adv_parse],0
        je      ta_bare1
        cmp     byte ptr [esi],'"'
        jne     ta_bare1
        inc     esi                     ; first argument, quoted
        mov     [arg1_ptr],esi
ta_q1:
        mov     al,[esi]
        or      al,al
        jz      ta_fail                 ; unterminated, nothing follows
        cmp     al,'"'
        je      ta_q1end
        inc     esi
        jmp     short ta_q1
ta_q1end:
        mov     byte ptr [esi],0
        inc     esi
        jmp     short ta_gap
ta_bare1:
        mov     [arg1_ptr],esi
ta_find:
        mov     al,[esi]
        or      al,al
        jz      ta_fail                 ; no second argument
        cmp     al,' '
        je      ta_split
        cmp     al,9
        je      ta_split
        inc     esi
        jmp     short ta_find
ta_split:
        mov     byte ptr [esi],0
        inc     esi
ta_gap:
        call    skip_blanks
        cmp     byte ptr [esi],0
        je      ta_fail
        call    unquote_arg             ; the second may be quoted too
        mov     [arg2_ptr],esi
        clc
        ret
ta_fail:
        stc
        ret

;---------------------------------------------------------------------
; sw_scan - lift every "/x" switch out of the ASCIIZ text at ESI.
;
; The text is compacted in place and returned trimmed of the blanks
; the removal left behind, so what is left is purely the argument the
; command wanted; EAX comes back with a bit per letter, bit 0 for /A
; up to bit 25 for /Z, which is what the SW_* equates index.  A '/'
; that is not followed by a letter is left alone - it is a path
; separator often enough to matter.
;
; TWO THINGS A BIT PER LETTER CANNOT SAY, and DIR needs both.
;
; A SWITCH CAN CARRY AN ARGUMENT - /O:GN, /A:-D, /ON - so whatever is
; stuck to the letter is lifted with it and left in sw_args, eight
; bytes a letter, indexed the same way the bits are.  It runs to the
; next blank or the next '/', which is what makes "DIR /O *.TXT" the
; pattern it looks like and not a sort order called "*.TXT".
;
; AND A SWITCH CAN BE TURNED OFF.  "/-W" clears a bit instead of
; setting one, and it exists for DIRCMD: a variable that puts /W on
; every listing is no use without a way to say "not this one".  The
; negatives come back in sw_negm rather than in EAX, because a caller
; that merges two sources has to be able to tell "asked for off" from
; "did not ask" - which is the whole difference between DIRCMD and the
; command line.
;---------------------------------------------------------------------
sw_scan:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     [sw_start],esi
        mov     dword ptr [sw_negm],0
        push    esi
        mov     edi,OFFSET sw_args      ; last call's arguments are not
        xor     eax,eax                 ; this call's
        mov     ecx,(26 * SW_ARGMAX) / 4
        rep     stosd
        pop     esi
        mov     edi,esi
        xor     ebx,ebx
sws_lp:
        mov     al,[esi]
        or      al,al
        jz      sws_done
        cmp     al,'/'
        jne     sws_keep
        mov     edx,1                   ; how far past the '/' the letter
        mov     al,[esi+1]              ; is: one, or two for "/-x"
        cmp     al,'-'
        jne     sws_letter
        mov     edx,2
        mov     al,[esi+2]
sws_letter:
        call    upcase
        sub     al,'A'
        cmp     al,26
        jae     sws_keep                ; not a letter: ordinary text
        movzx   ecx,al
        mov     eax,1
        shl     eax,cl
        cmp     edx,1
        je      sws_on
        or      [sw_negm],eax           ; "/-x": asked for OFF
        jmp     short sws_after
sws_on:
        or      ebx,eax
sws_after:
        lea     esi,[esi+edx+1]         ; past the '/', the '-' and the letter

        ; ...and whatever is stuck to the letter, which belongs to it
        push    edi
        mov     edi,OFFSET sw_args
        shl     ecx,3                   ; eight bytes a letter
        add     edi,ecx
        xor     ecx,ecx
sws_arg:
        mov     al,[esi]
        or      al,al
        jz      sws_argend
        cmp     al,' '
        je      sws_argend
        cmp     al,9
        je      sws_argend
        cmp     al,'/'
        je      sws_argend
        cmp     ecx,SW_ARGMAX - 1
        jae     sws_argskip
        mov     [edi+ecx],al
        inc     ecx
sws_argskip:
        inc     esi
        jmp     short sws_arg
sws_argend:
        mov     byte ptr [edi+ecx],0
        pop     edi
        jmp     sws_lp
sws_keep:
        mov     al,[esi]
        mov     [edi],al
        inc     edi
        inc     esi
        jmp     short sws_lp
sws_done:
        mov     esi,[sw_start]          ; trim what the removal left
sws_trim:
        cmp     edi,esi
        jbe     sws_end
        mov     al,[edi-1]
        cmp     al,' '
        je      sws_cut
        cmp     al,9
        jne     sws_end
sws_cut:
        dec     edi
        jmp     short sws_trim
sws_end:
        mov     byte ptr [edi],0
        mov     eax,ebx
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; sw_arg - ESI -> the argument text of switch AL ('A'..'Z'), which is
;          an empty string when the switch was given without one.
;---------------------------------------------------------------------
sw_arg:
        push    eax
        call    upcase
        sub     al,'A'
        movzx   esi,al
        shl     esi,3
        add     esi,OFFSET sw_args
        pop     eax
        ret

; has_wild - CF set if the ASCIIZ at ESI contains * or ?
has_wild:
        push    esi
hw_lp:
        mov     al,[esi]
        or      al,al
        jz      hw_no
        cmp     al,'*'
        je      hw_yes
        cmp     al,'?'
        je      hw_yes
        inc     esi
        jmp     short hw_lp
hw_no:
        pop     esi
        clc
        ret
hw_yes:
        pop     esi
        stc
        ret

; word_is_on / word_is_off - CF clear if the word at EDI is ON / OFF
word_is_on:
        mov     al,[edi]
        call    upcase
        cmp     al,'O'
        jne     wo_no
        mov     al,[edi+1]
        call    upcase
        cmp     al,'N'
        jne     wo_no
        cmp     byte ptr [edi+2],0
        jne     wo_no
        clc
        ret
wo_no:
        stc
        ret

word_is_off:
        mov     al,[edi]
        call    upcase
        cmp     al,'O'
        jne     wf_no
        mov     al,[edi+1]
        call    upcase
        cmp     al,'F'
        jne     wf_no
        mov     al,[edi+2]
        call    upcase
        cmp     al,'F'
        jne     wf_no
        cmp     byte ptr [edi+3],0
        jne     wf_no
        clc
        ret
wf_no:
        stc
        ret

; parse_num - unsigned decimal at ESI -> EAX; ESI past it.  CF if none.
parse_num:
        push    ebx
        push    ecx
        xor     eax,eax
        xor     ebx,ebx                 ; digits consumed
pn_lp:
        movzx   ecx,byte ptr [esi]
        sub     ecx,'0'
        cmp     ecx,9
        ja      pn_end
        imul    eax,10
        add     eax,ecx
        inc     ebx
        inc     esi
        jmp     short pn_lp
pn_end:
        or      ebx,ebx                 ; ZF = no digits at all
        pop     ecx
        pop     ebx
        jz      pn_none
        clc
        ret
pn_none:
        stc
        ret

; skip_sep - one -, /, :, or . separator
skip_sep:
        mov     al,[esi]
        cmp     al,'-'
        je      ss_eat
        cmp     al,'/'
        je      ss_eat
        cmp     al,':'
        je      ss_eat
        cmp     al,'.'
        je      ss_eat
        ret
ss_eat:
        inc     esi
        ret

;---------------------------------------------------------------------
; pdec10 / pdec4 / pdec2 - decimal output of EAX
;---------------------------------------------------------------------
pdec1:
        push    ecx
        mov     ecx,1                   ; no padding at all
        jmp     short pdecN
pdec10:
        push    ecx
        mov     ecx,10
        jmp     short pdecN
pdec4:
        push    ecx
        mov     ecx,-4
        jmp     short pdecN
pdec2:
        push    ecx
        mov     ecx,-2
pdecN:
        pushad
        mov     edi,OFFSET dec_buf + 11
        mov     byte ptr [edi],'$'
        mov     ebx,10
        mov     esi,ecx
        or      esi,esi
        jns     pd_w
        neg     esi
pd_w:
        xor     ecx,ecx
pd_div:
        xor     edx,edx
        div     ebx
        dec     edi
        add     dl,'0'
        mov     [edi],dl
        inc     ecx
        or      eax,eax
        jnz     pd_div
pd_pad:
        cmp     ecx,esi
        jae     pd_out
        dec     edi
        mov     byte ptr [edi],' '
        cmp     dword ptr [esp+24],0
        jns     pd_sp
        mov     byte ptr [edi],'0'
pd_sp:
        inc     ecx
        jmp     short pd_pad
pd_out:
        mov     edx,edi
        mov     ah,09h
        int     21h
        popad
        pop     ecx
        ret

;=====================================================================
; Command table
;=====================================================================
;=====================================================================
; LOADHIGH cmd / LH cmd - run a command with its memory taken from
;                         upper memory if any will fit it.
;
; THE WHOLE COMMAND IS FOUR CALLS TO 58h AND A RE-ENTRY.  Set the
; allocation strategy to "upper first", join the upper chain onto the
; conventional one so that there is anything up there to find, run the
; command exactly as it would have been run anyway, and put both back.
; The kernel does the rest in lm_alloc, which is where the decision
; belongs - a shell that tried to place a program itself would have to
; know how big the program was before loading it.
;
; UPPER FIRST AND NOT UPPER ONLY, so that a program too big for what
; is free up there loads low and runs.  That is what LOADHIGH has
; always done and it is why the command is not usually worth an error
; message: the user asked for a preference and got the best available
; answer to it.  MEM is where to find out what actually happened.
;
; PUT BACK IN THE OPPOSITE ORDER AND UNCONDITIONALLY.  The strategy
; and the link state are system-wide settings that a program can read,
; and a shell that leaves its own values in them has changed the
; machine on behalf of a command that has already finished.
;=====================================================================
cmd_loadhigh:
        mov     esi,[arg_ptr]
        call    skip_blanks
        cmp     byte ptr [esi],0
        je      lh_bare

        mov     ax,5800h                ; what the strategy is now
        int     21h
        movzx   ebx,ax
        push    ebx
        mov     ax,5802h                ; and whether they are linked
        int     21h
        movzx   ebx,al
        push    ebx

        mov     ax,5803h                ; link them.  A machine with no
        mov     bx,1                    ; upper memory refuses, and the
        int     21h                     ; strategy below then finds none
                                        ; and comes back low - which is
                                        ; the right answer, so the error
                                        ; is not one
        mov     ax,5801h
        mov     bx,40h                  ; upper first, first fit
        int     21h

        mov     esi,[arg_ptr]
        call    skip_blanks
        call    exec_core

        pop     ebx                     ; the link state, then the strategy
        mov     ax,5803h
        int     21h
        pop     ebx
        mov     ax,5801h
        int     21h
        ret

lh_bare:
        mov     edx,OFFSET msg_lhbare
        call    print
        ret

cmd_table:
        dd      OFFSET cn_dir,    OFFSET cmd_dir
        dd      OFFSET cn_type,   OFFSET cmd_type
        dd      OFFSET cn_copy,   OFFSET cmd_copy
        dd      OFFSET cn_del,    OFFSET cmd_del
        dd      OFFSET cn_erase,  OFFSET cmd_del
        dd      OFFSET cn_ren,    OFFSET cmd_ren
        dd      OFFSET cn_rename, OFFSET cmd_ren
        dd      OFFSET cn_cls,    OFFSET cmd_cls
        dd      OFFSET cn_ver,    OFFSET cmd_ver
        dd      OFFSET cn_vol,    OFFSET cmd_vol
        dd      OFFSET cn_date,   OFFSET cmd_date
        dd      OFFSET cn_time,   OFFSET cmd_time
        dd      OFFSET cn_pause,  OFFSET cmd_pause
        dd      OFFSET cn_rem,    OFFSET cmd_rem
        dd      OFFSET cn_exit,   OFFSET cmd_exit
        dd      OFFSET cn_echo,   OFFSET cmd_echo
        dd      OFFSET cn_break,  OFFSET cmd_break
        dd      OFFSET cn_verify, OFFSET cmd_verify
        dd      OFFSET cn_set,    OFFSET cmd_set
        dd      OFFSET cn_path,   OFFSET cmd_path
        dd      OFFSET cn_prompt, OFFSET cmd_prompt
        dd      OFFSET cn_md,     OFFSET cmd_md
        dd      OFFSET cn_mkdir,  OFFSET cmd_md
        dd      OFFSET cn_cd,     OFFSET cmd_cd
        dd      OFFSET cn_chdir,  OFFSET cmd_cd
        dd      OFFSET cn_rd,     OFFSET cmd_rd
        dd      OFFSET cn_rmdir,  OFFSET cmd_rd
        dd      OFFSET cn_goto,   OFFSET cmd_goto
        dd      OFFSET cn_if,     OFFSET cmd_if
        dd      OFFSET cn_for,    OFFSET cmd_for
        dd      OFFSET cn_shift,  OFFSET cmd_shift
        dd      OFFSET cn_call,   OFFSET cmd_call
        dd      OFFSET cn_ctty,   OFFSET cmd_ctty
        dd      OFFSET cn_lh,     OFFSET cmd_loadhigh
        dd      OFFSET cn_loadhigh, OFFSET cmd_loadhigh
        dd      0, 0

cn_dir      db  'DIR',0
cn_type     db  'TYPE',0
cn_copy     db  'COPY',0
cn_del      db  'DEL',0
cn_erase    db  'ERASE',0
cn_ren      db  'REN',0
cn_rename   db  'RENAME',0
cn_cls      db  'CLS',0
cn_ver      db  'VER',0
cn_vol      db  'VOL',0
cn_date     db  'DATE',0
cn_time     db  'TIME',0
cn_pause    db  'PAUSE',0
cn_rem      db  'REM',0
cn_exit     db  'EXIT',0
cn_echo     db  'ECHO',0
cn_break    db  'BREAK',0
cn_verify   db  'VERIFY',0
cn_set      db  'SET',0
cn_path     db  'PATH',0
cn_prompt   db  'PROMPT',0
cn_md       db  'MD',0
cn_mkdir    db  'MKDIR',0
cn_cd       db  'CD',0
cn_chdir    db  'CHDIR',0
cn_rd       db  'RD',0
cn_rmdir    db  'RMDIR',0
cn_goto     db  'GOTO',0
cn_if       db  'IF',0
cn_for      db  'FOR',0
cn_shift    db  'SHIFT',0
cn_call     db  'CALL',0
cn_ctty     db  'CTTY',0
cn_lh       db  'LH',0
cn_loadhigh db  'LOADHIGH',0

;=====================================================================
; Messages
;=====================================================================
msg_crlf    db  13,10,'$'
msg_2sp     db  '  ','$'
msg_bad     db  'Bad command or file name',13,10,'$'
msg_lhbare  db  'Required parameter missing',13,10,'$'
msg_needarg db  'Required parameter missing',13,10,'$'
msg_notfound db 'File not found',13,10,'$'
msg_nofiles db  'No files found',13,10,'$'
msg_files   db  ' file(s)  ','$'
msg_bytes   db  ' bytes',13,10,'$'
msg_free    db  ' bytes free',13,10,'$'
; Ten columns for the file count and ten for " file(s)  ": what the
; byte total on the line above is indented by.
msg_freepad db  '                    ','$'
msg_vol1    db  ' Volume in drive is ','$'
msg_nolabel db  'unlabelled','$'
msg_dirof   db  13,10,' Directory of ','$'
msg_dirof2  db  13,10,13,10,'$'
msg_isdir   db  '     <DIR>','$'
msg_totlist db  13,10,'Total files listed:',13,10,'$'
env_dircmd  db  'DIRCMD',0
msg_mdfail  db  'Unable to create directory',13,10,'$'
msg_cdfail  db  'Invalid directory',13,10,'$'
msg_rdfail  db  'Invalid path, not directory,',13,10
            db  'or directory not empty.',13,10,'$'
msg_copied  db  ' file(s) copied',13,10,'$'
msg_self    db  'File cannot be copied onto itself',13,10,'$'
msg_nodest  db  'Unable to create destination',13,10,'$'
msg_wrerr   db  'Write error',13,10,'$'
msg_full    db  'Insufficient disk space',13,10,'$'
msg_dup     db  'Duplicate file name',13,10,'$'
msg_sure    db  'All files in the directory will be deleted!',13,10
            db  'Are you sure (Y/N)? ','$'
msg_curdate db  'Current date is ','$'
msg_baddate db  'Invalid date',13,10,'$'
msg_curtime db  'Current time is ','$'
msg_badtime db  'Invalid time',13,10,'$'
msg_pause   db  'Press any key to continue . . . ','$'
msg_delq    db  ', Delete (Y/N)? ','$'
msg_echois  db  'ECHO is ','$'
msg_breakis db  'BREAK is ','$'
msg_verifyis db 'VERIFY is ','$'
msg_on      db  'on','$'
msg_off     db  'off','$'
msg_badparm db  'Parameter must be ON or OFF',13,10,'$'
msg_envfull db  'Out of environment space',13,10,'$'
msg_nopath  db  'No Path',13,10,'$'
msg_pathis  db  'PATH=','$'
msg_baddrv  db  'Invalid drive specification',13,10,'$'
msg_ver1    db  'PM-DOS version ','$'
; No number in this one.  It said "version 0.29" for two releases after
; 0.29, which is the exact failure IORM's banner was cured of: a
; version written as text cannot be checked by the assembler and so is
; only ever as current as the last person to remember it.  The message
; is about a MISMATCH, and saying that is the whole job.
msg_ver2    db  'COMMAND.COM was built against a different kernel',13,10,'$'
msg_verunk  db  'unknown (this kernel does not answer 21h/F1h)',13,10,'$'

pat_all     db  '*.*',0
pat_root    db  '\*.*',0
dir_lbl     db  'A:\*.*',0              ; drive letter patched by arg_drive
dir_drv     db  0                       ; 1-based drive of the listing
dir_hdl     dw  0                       ; the 714Eh find handle
            dw  0
find_data   db  318 dup(0)              ; one 714Eh/714Fh result record
name_path   db  'PATH',0
name_prompt db  'PROMPT',0
name_comspec db 'COMSPEC',0
dflt_prompt db  '$P$G',0
lit_pathdel db  'PATH=',0               ; a value-less entry = delete
lit_comspec db  'COMSPEC='              ; 8 bytes, then the path
COMSPEC_PFX     EQU     8

;=====================================================================
; Data
;=====================================================================
echo_flag   db  1
break_flag  db  0
verify_flag db  0
adv_parse   db  1                       ; quotes group arguments: the
                                        ; console and .CMD files yes,
                                        ; .BAT files never

break_esp   dd  0                       ; cmd_break's longjmp target
cb_hdl      dd  0
in_child    db  0                       ; a 4Bh child is running
            db  0,0,0

arg_ptr     dd  0
arg_raw     dd  0                       ; args before leading blanks are cut
arg1_ptr    dd  0
arg2_ptr    dd  0
copy_src    dd  0
copy_dst    dd  0

; COPY's working set.  cp_items points into the command line itself,
; which cp_parse has cut into ASCIIZ pieces in place.
cp_items    dd  CP_MAXITEM dup(0)
cp_glue     db  CP_MAXITEM dup(0)
cp_mode     db  CP_MAXITEM dup(0)       ; each item's /A or /B
cp_curmode  db  CPM_UNSET               ; the one in force while parsing
cp_srcmode  db  CPM_UNSET               ; ...the source being read now
cp_dmode    db  CPM_UNSET               ; ...and the destination's
cp_verify   db  0                       ; /V was given
cp_oldvfy   db  0                       ; and what VERIFY was before it
cp_ztail    db  0                       ; a Ctrl-Z ended this source
cp_zbyte    db  0                       ; and the one written to end the
            db  0                       ; destination
cp_nitem    dd  0
cp_first    dd  0                       ; first source item to copy
cp_count    dd  0                       ; files copied, for the report
cp_dstptr   dd  0                       ; 0 = no destination named
cp_hdl      dw  0
cp_pend     db  0                       ; a '+' is waiting for its item
cp_glued    db  0                       ; a '+' appeared anywhere
cp_append   db  0                       ; "COPY A+B": open A, seek end
cp_todir    db  0                       ; the destination is a place
cp_concat   db  0                       ; ...so there is one output
cp_opened   db  0                       ; and it is already open
            db  0
cp_dest     db  144 dup(0)
cp_srcbuf   db  144 dup(0)
cp_dirbuf   db  144 dup(0)
dir_sw      dd  0                       ; the /switches DIR was given
dir_neg     dd  0                       ; ...and the ones it was refused
dir_all     dd  0                       ; the attributes "DIR," adds
dir_total   dd  0                       ; /S: every directory's files
dir_tbytes  dd  0                       ; ...and every directory's bytes
dir_depth   dd  0                       ; how far down the walk is
dir_found   dd  0                       ; directories that had a match
dir_hdrdone dd  0                       ; this one's heading is printed
dir_nrec    dd  0                       ; /O: entries in this directory
dir_nfill   dd  0                       ; ...and how many are stored
dir_recs    dd  0                       ; the block holding them
dir_idx     dd  0                       ; and the index that gets sorted
dir_nkey    dd  0                       ; /O: how many sort keys
dir_keys    db  (DIR_MAXKEY * 2) dup(0) ; each a letter and a direction
dir_dirpath db  144 dup(0)              ; the directory being listed
dir_fpat    db  80 dup(0)               ; and the pattern within it
dir_subblk  dd  0                       ; where dir_subfill is putting names
walk_act    dd  0                       ; what tree_walk does at each stop
walk_rec    dd  0                       ; ...and whether it goes deeper
dir_ecmd    db  144 dup(0)              ; DIRCMD's value, scanned apart
dir_eargs   db  (26 * SW_ARGMAX) dup(0) ; and the arguments it carried
dir_lines   dd  0                       ; lines since the last /P pause
dir_col     dd  0                       ; /W column, 0 = start of a row
sw_start    dd  0                       ; sw_scan's text, for the trim
sw_negm     dd  0                       ; the "/-x" switches of that scan
sw_args     db  (26 * SW_ARGMAX) dup(0) ; and what was stuck to each letter
del_sw      dd  0
del_count   dd  0                       ; what DEL /S has deleted so far
dir_count   dd  0
dir_bytes   dd  0
tmp_a       dd  0
tmp_b       dd  0

parm_blk    dd  0                       ; environment, filled per EXEC
            dd  0                       ; tail pointer, filled per EXEC

cmd_buf     db  CMDLINE_MAX + 3 dup(0)
cwd_buf     db  68 dup(0)
del_path    db  144 dup(0)
del_pfx     dd  0
del_hdl     dw  0                       ; DEL's 714Eh find handle
dir_scratch db  144 dup(0)
dir_hdrbuf  db  144 dup(0)              ; the truenamed "Directory of"
dir_fsbuf   db  8 dup(0)                ; "X:\" for 7303h
dir_fsrec   db  F73_SIZE dup(0)
verb_buf    db  16 dup(0)
qverb_buf   db  QVERB_MAX + 2 dup(0)    ; a quoted verb, as typed
verb_quoted db  0                       ; this command's verb was quoted
            db  0,0,0
verb_src    dd  0                       ; -> verb_buf or qverb_buf
ext_at      dd  0                       ; where try_exts writes ".XXX"
path_buf    db  PATHDIR_MAX + QVERB_MAX + 8 dup(0)      ; dir + verb + ext
tail_buf    db  130 dup(0)
dec_buf     db  12 dup(0)
dta         db  43 dup(0)
SCRATCH_MAX     EQU     176             ; wider than a CONFIG.SYS line, so
set_scratch db  SCRATCH_MAX dup(0)      ; that inherit_env never has to trim
env_blk     db  ENV_SIZE dup(0)
copy_buf    db  COPYBUF_SIZE dup(0)

        INCLUDE BATCH.INC
        INCLUDE REDIR.INC

_CMD    ENDS
        END
