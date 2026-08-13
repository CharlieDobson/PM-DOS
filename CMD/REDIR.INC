;=====================================================================
; REDIR.INC - redirection and pipes for PM-DOS COMMAND.COM
;
; The layer between get_line and exec_core.  Every line runs through
; exec_line below, which peels < file, > file, >> file and | off the
; text (compacting it in place) before the verb parser ever sees it,
; then points the standard handles where the tokens said and puts
; them back afterwards.
;
; The kernel side of this is 21h/45h (DUP) and 46h (FORCEDUP): the
; console is PARKED in a spare handle with 45h, the file is installed
; over handle 0 or 1 with 46h, and restoring is 46h back the other
; way - which is also what closes and flushes the file.  Programs
; never know: every 01h/02h/09h/0Ah in the kernel goes through the
; standard handles, so a child writes into the file as blindly as
; COMMAND itself does.
;
; Tokens only ever exist at the OUTERMOST level of a line.  This
; layer strips them before exec_core parses the verb, so a line
; re-fed by IF or FOR arrives here already clean and drops straight
; through.  That ordering is DOS's own: redirection is set up before
; IF even looks at its condition - a false IF still leaves an empty
; target file - and FOR's iterations all land in ONE open file
; rather than each reopening and truncating it.
;
; Pipes are DOS pipes: sequential, through temp files in the current
; directory (%PIPE1.$$$ and %PIPE2.$$$, alternating), stage by stage
; left to right.  Nothing runs concurrently, exactly like the real
; thing.
;
; Included by COMMAND.ASM.
;=====================================================================

RX_IN           EQU     1               ; rx_flags bits
RX_OUT          EQU     2
RX_APP          EQU     4
RXNAME_MAX      EQU     80

;=====================================================================
; exec_line - run the command line at ESI, honouring < > >> and |.
;=====================================================================
exec_line:
        push    esi
        xor     ah,ah                   ; AH = inside a quoted span
xl_pscan:
        mov     al,[esi]
        or      al,al
        jz      xl_plain
        cmp     byte ptr [adv_parse],0  ; in advanced mode a quoted
        je      xl_pnq                  ; '|' is text, not a pipe
        cmp     al,'"'
        jne     xl_pnq
        xor     ah,1
xl_pnq:
        or      ah,ah
        jnz     xl_pstep
        cmp     al,'|'
        je      xl_pipe
xl_pstep:
        inc     esi
        jmp     short xl_pscan
xl_plain:
        pop     esi
        mov     dword ptr [rx_pin],0    ; no pipe plumbing this run
        mov     dword ptr [rx_pout],0
        call    redir_parse
        cmp     byte ptr [rx_flags],0
        jne     xl_wrap
        jmp     exec_core               ; nothing redirected: zero cost
xl_wrap:
        jmp     redir_run
xl_pipe:
        pop     esi
        jmp     exec_pipeline

;---------------------------------------------------------------------
; redir_run - apply what redir_parse found (plus any pipe plumbing in
;             rx_pin/rx_pout), run exec_core on the stripped line at
;             ESI, and put the standard handles back.
;
; The parked handles live on the STACK, not in statics: exec_core can
; re-enter exec_line (IF, FOR), and an inner run zeroes the parse
; statics - an outer restore must not depend on them.
;---------------------------------------------------------------------
redir_run:
        call    redir_apply             ; -> EBX/ECX = parked 0 and 1
        jc      rdx_out                 ;    (-1 = never touched)
        push    ebx
        push    ecx
        call    exec_core
        pop     ecx
        pop     ebx
        call    redir_restore
rdx_out:
        ret

;---------------------------------------------------------------------
; redir_parse - strip < file, > file and >> file out of the line at
;               ESI, compacting it in place, and record what was
;               found.  The name may hug its redirector or stand
;               clear: DIR>F, DIR >F and DIR > F are all the same
;               command.  ESI is preserved.
;---------------------------------------------------------------------
redir_parse:
        push    eax
        push    esi
        push    edi
        mov     byte ptr [rx_flags],0
        mov     edi,esi                 ; write pointer chases read
        xor     ah,ah                   ; AH = inside a quoted span
rdp_lp:
        mov     al,[esi]
        or      al,al
        jz      rdp_end
        cmp     byte ptr [adv_parse],0  ; a quoted < > is text and is
        je      rdp_nq                  ; copied through, quotes kept
        cmp     al,'"'                  ; for the argument parsing
        jne     rdp_nq                  ; downstream
        xor     ah,1
        jmp     short rdp_copy
rdp_nq:
        or      ah,ah
        jnz     rdp_copy
        cmp     al,'<'
        je      rdp_in
        cmp     al,'>'
        je      rdp_out
rdp_copy:
        mov     [edi],al
        inc     esi
        inc     edi
        jmp     short rdp_lp
rdp_in:
        inc     esi
        push    edi
        mov     edi,OFFSET rx_inname
        call    rdp_name
        pop     edi
        or      byte ptr [rx_flags],RX_IN
        jmp     short rdp_lp
rdp_out:
        inc     esi
        cmp     byte ptr [esi],'>'      ; ">>" appends
        jne     rdp_one
        inc     esi
        or      byte ptr [rx_flags],RX_APP
rdp_one:
        push    edi
        mov     edi,OFFSET rx_outname
        call    rdp_name
        pop     edi
        or      byte ptr [rx_flags],RX_OUT
        jmp     short rdp_lp
rdp_end:
        mov     byte ptr [edi],0
        pop     edi
        pop     esi
        pop     eax
        ret

;---------------------------------------------------------------------
; rdp_name - copy the filename at ESI (leading blanks skipped) to
;            EDI, ASCIIZ.  Stops at a blank, another redirector, a
;            pipe, or the end of the line.
;---------------------------------------------------------------------
rdp_name:
        push    ecx
rdn_skip:
        cmp     byte ptr [esi],' '
        je      rdn_step
        cmp     byte ptr [esi],9
        jne     rdn_go
rdn_step:
        inc     esi
        jmp     short rdn_skip
rdn_go:
        xor     ecx,ecx
        cmp     byte ptr [adv_parse],0  ; a quoted name runs to the
        je      rdn_lp                  ; closing quote, spaces and
        cmp     byte ptr [esi],'"'      ; all, and the quotes are
        jne     rdn_lp                  ; shed - the kernel never
        inc     esi                     ; sees them
rdn_qlp:
        mov     al,[esi]
        or      al,al
        jz      rdn_end
        cmp     al,'"'
        je      rdn_qend
        cmp     ecx,RXNAME_MAX - 1
        jae     rdn_qsk
        mov     [edi],al
        inc     edi
        inc     ecx
rdn_qsk:
        inc     esi
        jmp     short rdn_qlp
rdn_qend:
        inc     esi
        jmp     short rdn_end
rdn_lp:
        mov     al,[esi]
        or      al,al
        jz      rdn_end
        cmp     al,' '
        je      rdn_end
        cmp     al,9
        je      rdn_end
        cmp     al,'<'
        je      rdn_end
        cmp     al,'>'
        je      rdn_end
        cmp     al,'|'
        je      rdn_end
        cmp     ecx,RXNAME_MAX - 1
        jae     rdn_full
        mov     [edi],al
        inc     edi
        inc     ecx
rdn_full:
        inc     esi
        jmp     short rdn_lp
rdn_end:
        mov     byte ptr [edi],0
        pop     ecx
        ret

;---------------------------------------------------------------------
; redir_apply - point handles 0/1 at the named files.  An explicit
;               < or > beats the pipe plumbing; the pipe fills
;               whichever direction the stage left open.
;               -> EBX = parked stdin, ECX = parked stdout, -1 for a
;               direction never touched.  CF set on failure with the
;               message printed and everything already undone - the
;               command must not run.
;---------------------------------------------------------------------
redir_apply:
        push    esi
        push    edi
        mov     ebx,-1
        mov     ecx,-1

        ; --- stdin -----------------------------------------------
        mov     edx,OFFSET rx_inname
        test    byte ptr [rx_flags],RX_IN
        jnz     rda_havein
        mov     edx,[rx_pin]
        or      edx,edx
        jz      rda_indone
rda_havein:
        cmp     byte ptr [edx],0        ; "DIR <" names nothing
        je      rda_syntax
        mov     ax,3D00h                ; open to read
        int     21h
        jc      rda_nofile
        movzx   edi,ax                  ; EDI = the source file

        mov     ebx,0
        mov     ah,45h                  ; park the real stdin
        int     21h
        jc      rda_full
        movzx   ebx,ax                  ; EBX = the parked copy

        push    ebx
        push    ecx
        mov     ebx,edi
        xor     ecx,ecx
        mov     ah,46h                  ; the file becomes handle 0
        int     21h
        mov     ah,3Eh                  ; the opening handle can go
        int     21h
        pop     ecx
        pop     ebx
rda_indone:

        ; --- stdout ----------------------------------------------
        mov     edx,OFFSET rx_outname
        test    byte ptr [rx_flags],RX_OUT
        jnz     rda_haveout
        mov     edx,[rx_pout]
        or      edx,edx
        jz      rda_outdone
rda_haveout:
        cmp     byte ptr [edx],0
        je      rda_syntax
        test    byte ptr [rx_flags],RX_APP
        jz      rda_creat
        mov     ax,3D01h                ; >>: open to write ...
        int     21h
        jc      rda_creat               ; ... or create if new
        movzx   edi,ax
        push    ebx
        push    ecx
        movzx   ebx,ax
        xor     ecx,ecx
        mov     ax,4202h                ; ... standing at the END
        int     21h
        pop     ecx
        pop     ebx
        jmp     short rda_gotout
rda_creat:
        push    ecx
        xor     ecx,ecx                 ; plain attributes
        mov     ah,3Ch                  ; create / truncate
        int     21h
        pop     ecx
        jc      rda_nocreate
        movzx   edi,ax
rda_gotout:
        push    ebx
        mov     ebx,1
        mov     ah,45h                  ; park the real stdout
        int     21h
        pop     ebx
        jc      rda_full
        movzx   ecx,ax                  ; ECX = the parked copy

        push    ebx
        push    ecx
        mov     ebx,edi
        mov     ecx,1
        mov     ah,46h                  ; the file becomes handle 1
        int     21h
        mov     ah,3Eh
        int     21h
        pop     ecx
        pop     ebx
rda_outdone:
        pop     edi
        pop     esi
        clc
        ret

        ; failures: close strays, put back whatever went in, say why
rda_full:
        push    ebx
        mov     ebx,edi                 ; the file we opened but never
        mov     ah,3Eh                  ; installed
        int     21h
        pop     ebx
        mov     edx,OFFSET msg_handles
        jmp     short rda_err
rda_syntax:
        mov     edx,OFFSET msg_syntax
        jmp     short rda_err
rda_nofile:
        mov     edx,OFFSET msg_notfound
        jmp     short rda_err
rda_nocreate:
        mov     edx,OFFSET msg_nocreate
rda_err:
        call    print
        call    redir_restore           ; the -1s make this safe here
        pop     edi
        pop     esi
        stc
        ret

;---------------------------------------------------------------------
; redir_restore - undo redir_apply.  EBX/ECX = the parked handles it
;                 returned; -1 means that direction was never
;                 touched.  Restoring is what CLOSES the target file:
;                 46h flushes and frees whatever the slot held before
;                 installing the parked console over it.
;---------------------------------------------------------------------
redir_restore:
        push    eax
        push    ebx
        push    ecx
        cmp     ecx,-1
        je      rdr_in
        push    ebx
        mov     ebx,ecx                 ; the parked stdout ...
        mov     ecx,1
        mov     ah,46h                  ; ... back over handle 1
        int     21h
        mov     ah,3Eh                  ; parking slot freed
        int     21h
        pop     ebx
rdr_in:
        cmp     ebx,-1
        je      rdr_done
        xor     ecx,ecx
        mov     ah,46h
        int     21h
        mov     ah,3Eh
        int     21h
rdr_done:
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; exec_pipeline - a line with '|' in it.  Stages run left to right,
; each one's stdout caught in a temp file the next one reads.  The
; statics are safe: an inner exec_line never sees a '|' (this split
; consumed them all), so pipelines cannot nest.
;=====================================================================
exec_pipeline:
        mov     [pip_cur],esi
        mov     byte ptr [pip_toggle],0
        mov     dword ptr [rx_pin],0    ; stage 1 keeps the real stdin
pip_stage:
        mov     esi,[pip_cur]
        mov     dword ptr [pip_next],0
        xor     ah,ah                   ; AH = inside a quoted span
pip_scan:
        mov     al,[esi]
        or      al,al
        jz      pip_cut
        cmp     byte ptr [adv_parse],0
        je      pip_nq
        cmp     al,'"'
        jne     pip_nq
        xor     ah,1
pip_nq:
        or      ah,ah
        jnz     pip_step
        cmp     al,'|'
        je      pip_bar
pip_step:
        inc     esi
        jmp     short pip_scan
pip_bar:
        mov     byte ptr [esi],0        ; this stage ends here
        inc     esi
        mov     [pip_next],esi
pip_cut:
        mov     dword ptr [rx_pout],0
        cmp     dword ptr [pip_next],0  ; last stage keeps real stdout
        je      pip_run
        call    pip_wname
        mov     [rx_pout],edx
pip_run:
        mov     esi,[pip_cur]
        call    redir_parse             ; the stage's own < and >
        call    redir_run               ; a failed stage still lets
                                        ; the rest of the line try
        cmp     dword ptr [pip_next],0
        je      pip_done
        call    pip_wname               ; what this stage wrote is
        mov     [rx_pin],edx            ; what the next one reads
        xor     byte ptr [pip_toggle],1
        mov     eax,[pip_next]
        mov     [pip_cur],eax
        jmp     short pip_stage
pip_done:
        mov     edx,OFFSET pipe1_name   ; take the plumbing down
        mov     ah,41h
        int     21h
        mov     edx,OFFSET pipe2_name
        mov     ah,41h
        int     21h
        ret

; pip_wname - EDX = the pipe file the CURRENT stage writes into
pip_wname:
        mov     edx,OFFSET pipe1_name
        cmp     byte ptr [pip_toggle],0
        je      pip_wout
        mov     edx,OFFSET pipe2_name
pip_wout:
        ret

;=====================================================================
; Redirection state and messages
;=====================================================================
rx_flags    db  0
pip_toggle  db  0
rx_inname   db  RXNAME_MAX dup(0)
rx_outname  db  RXNAME_MAX dup(0)
rx_pin      dd  0                       ; pipe feed for this stage, or 0
rx_pout     dd  0                       ; pipe catch for this stage, or 0
pip_cur     dd  0
pip_next    dd  0

pipe1_name  db  '%PIPE1.$$$',0
pipe2_name  db  '%PIPE2.$$$',0

msg_nocreate db 'File creation error',13,10,'$'
msg_handles db  'Too many open files',13,10,'$'
