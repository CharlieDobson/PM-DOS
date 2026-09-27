;=====================================================================
; COMMAND.ASM - COMMAND.COM, the 16-bit command interpreter
;
; The shell DOS PROGRAMS get.  PM-DOS's own interpreter is CMD.EXE, a
; native 32-bit program (CMD\CMD.ASM).  This one exists because a DOS
; program that shells out runs whatever COMSPEC names, as a real-mode
; program in its DOS box - and under Windows 3.1 standard mode the
; kernel is parked and RMDOS runs the machine, so nothing 32-bit can
; run at all.  Every MS-DOS Prompt Windows opens is this program.
;
; So it is written against the DOS API and nothing else.  It asks no
; PM-DOS question it cannot do without (VER is the one place it asks
; at all), and it runs on real MS-DOS too - which is how its output is
; checked against MS-DOS 6.22's own COMMAND.COM.  It does assume a 386,
; which PM-DOS requires anyway, for the 32-bit arithmetic DIR and COPY
; need.
;
; The command set is DOS 6.22's:
;   BREAK CALL CD CHCP CHDIR CLS COPY CTTY DATE DEL DIR ECHO ERASE EXIT
;   FOR GOTO IF LH LOADHIGH MD MKDIR PATH PAUSE PROMPT RD REM REN RENAME
;   RMDIR SET SHIFT TIME TRUENAME TYPE VER VERIFY VOL,  plus  X:  drive
;   changes.
; No long names, no FOR /F, and no editor of its own: a DOSKEY that has
; been loaded is asked for the line (INT 2Fh AX=4810h), exactly as
; DOS's COMMAND.COM asked it, and DOS's own 0Ah edits it otherwise.
;
; The file, and memory, from 100h up:
;   the resident part   code, data and zeroed work areas
;   resident_end        the stack, which OVERLAYS what follows - both
;                       of these are only ever needed once:
;   help texts          read back out of the file when /? asks
;   init                runs once, on its own small stack at the end
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
        INCLUDE PMVER.INC

LINE_MAX        EQU     128             ; DOS's own: 127 characters + CR
TAIL_MAX        EQU     126             ; what a PSP command tail holds
PATH_MAX        EQU     80              ; a DOS path: d:\ + 64 + a name
NAME_MAX        EQU     96              ; a path we build, with room
EXP_MAX         EQU     255             ; a batch line after %-expansion
VERB_MAX        EQU     66
STACK_SIZE      EQU     3072
BAT_MAX         EQU     32              ; CALL nesting
RD_MAX          EQU     8               ; redirections in force at once
BATRAW_LEN      EQU     256             ; one read of a batch file

; The PSP, as this program reads it.  Absolute, with ds: - the image
; is linked at 100h from ORG 0, so a bare [2Ch] would be relocated.
PSP_TERM        EQU     0Ah
PSP_PARENT      EQU     16h
PSP_ENV         EQU     2Ch
PSP_JFTPTR      EQU     34h
PSP_TAIL        EQU     80h

; single_mode: what /C and /K asked for
SM_NONE         EQU     0               ; interactive
SM_C            EQU     1               ; /C: one command, then leave
SM_KEEP         EQU     2               ; /K: one command, then stay
SM_DONE         EQU     3               ; /C's command has been run

; One batch level - see bat_push.  Only the running level is addressed
; directly (bat_cur); the ones it CALLed from are parked in blocks of
; their own, so a level costs nothing until it is nested.
BCO_NAME        EQU     0               ; the file, fully qualified
BCO_POS         EQU     96              ; dd: where its next line starts
BCO_SHIFT       EQU     100             ; dw: SHIFTs so far
BCO_ARGS        EQU     102             ; "%0 %1 ...", as it was called
BCO_LEN         EQU     232
BCO_PARAS       EQU     15

; A row of int_table
IT_NAME         EQU     0
IT_FUNC         EQU     2
IT_HELP         EQU     4               ; the help text: FILE offset
IT_HLEN         EQU     6               ; and length
IT_FLAGS        EQU     8
IT_SIZE         EQU     10
ITF_PREFIX      EQU     1               ; "CD.." and "ECHO." - see find_internal

OPEN_SLOTS      EQU     4               ; open_hdls - see abandon
FOR_PARKMAX     EQU     16              ; FORs parked by for_nest at once -
                                        ; NOT FOR_SAVES: wasm folds case, so
                                        ; that name WAS the table for_saves,
                                        ; and [for_saves+di] assembled as
                                        ; [di+10h], into our own PSP's saved
                                        ; vectors, parent and handle table

; INT 24h's AH
CE_WRITE        EQU     01h
CE_FAIL_OK      EQU     08h
CE_RETRY_OK     EQU     10h
CE_IGNORE_OK    EQU     20h
CE_NOTDISK      EQU     80h
CE_MAXCODE      EQU     0Fh

_TEXT   SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_TEXT, DS:_TEXT, ES:_TEXT, SS:_TEXT
        ORG     0

start:
        jmp     init

;=====================================================================
; The read-eval loop.  SP is put back at the top of every turn, because
; this is also where a ^C and an Abort land, and they arrive with
; whatever was on the stack when the command was abandoned.
;=====================================================================
main_loop:
        mov     sp,[main_sp]
        mov     byte ptr [cc_broken],0
        mov     byte ptr [call_flag],0
        call    get_command
        jc      main_loop
        call    exec_line
        cmp     byte ptr [crit_abort],0
        je      main_loop
        ; Abort, answered at the Abort, Retry, Fail? prompt in one of
        ; our own commands.  The handler turned it into Fail so that DOS
        ; would not terminate the shell (see int24_hand); the command
        ; has now failed its way out, and in a batch file the question
        ; DOS asked after an Abort is still owed.
        mov     byte ptr [crit_abort],0
        call    ask_endbat
        jmp     main_loop

;---------------------------------------------------------------------
; get_command - the next line to run.  CF clear: SI -> it, ASCIIZ.
;               CF set: nothing this time round.
;
; A batch file outranks everything, /C's command included - COMMAND /C
; X.BAT has to run X.BAT to its end before it leaves.
;---------------------------------------------------------------------
get_command:
        cmp     word ptr [bat_depth],0
        je      gc_nobat
        jmp     bat_next
gc_nobat:
        mov     al,[single_mode]
        cmp     al,SM_NONE
        je      gc_prompt
        cmp     al,SM_DONE
        je      gc_exit
        mov     byte ptr [single_mode],SM_DONE
        cmp     al,SM_KEEP
        jne     gc_single
        mov     byte ptr [single_mode],SM_NONE
gc_single:
        mov     si,OFFSET single_cmd
        clc
        ret
gc_exit:
        jmp     exit_shell

gc_prompt:
        ; NOTHING IS OWED TO A DISK WHEN THE PROMPT APPEARS.  Disk
        ; reset is where PM-DOS's write cache commits, exactly as
        ; CMD.EXE uses it, and on any other DOS it is a flush.
        mov     ah,0Dh
        int     21h
        cmp     byte ptr [echo_on],0    ; ECHO OFF at the prompt hides
        je      gcp_read                ; the prompt, as it always has
        call    crlf
        call    show_prompt
gcp_read:
        call    read_line
        jnc     gcp_have
        ; Redirected input has run out.  A shell reading a script from
        ; a file has nothing left to do; a permanent one cannot leave,
        ; so it goes back to the console instead.
        cmp     byte ptr [permanent],0
        je      gc_exit
        call    stdin_to_con
        stc
        ret
gcp_have:
        clc
        ret

;---------------------------------------------------------------------
; read_line - one line from standard input into line_buf.  CF: end of
;             file.  SI -> the text, ASCIIZ.
;
; From the console, DOSKEY first and DOS's 0Ah if it is not there -
; AX=4810h comes back unchanged when nobody answers it, which is the
; test DOS's COMMAND.COM made.  From a FILE, byte by byte: a 0Ah on a
; redirected stdin never reports end of file, and a shell fed a script
; must see the end of it.  Those lines are echoed, so a transcript
; reads the way the session would have.
;---------------------------------------------------------------------
read_line:
        mov     ax,4400h
        xor     bx,bx
        int     21h
        jc      rl_file
        test    dl,80h
        jz      rl_file

        mov     byte ptr [line_buf],LINE_MAX
        mov     byte ptr [line_buf+1],0
        mov     dx,OFFSET line_buf
        mov     ax,4810h
        int     2Fh
        or      ax,ax
        jz      rl_got
        mov     dx,OFFSET line_buf
        mov     ah,0Ah
        int     21h
rl_got:
        mov     al,0Ah                  ; 0Ah echoed the CR and stopped
        call    putc
        movzx   bx,byte ptr [line_buf+1]
        mov     byte ptr [line_buf+2+bx],0
        mov     si,OFFSET line_buf+2
        clc
        ret

rl_file:
        mov     di,OFFSET line_buf+2
        xor     cx,cx
rlf_next:
        push    cx
        mov     dx,OFFSET rl_byte
        mov     cx,1
        xor     bx,bx
        mov     ah,3Fh
        int     21h
        pop     cx
        jc      rlf_eof
        or      ax,ax
        jz      rlf_eof
        mov     al,[rl_byte]
        cmp     al,1Ah
        je      rlf_eof
        cmp     al,0Ah
        je      rlf_end
        cmp     al,0Dh
        je      rlf_next
        cmp     cx,LINE_MAX - 1
        jae     rlf_next
        stosb
        inc     cx
        jmp     rlf_next
rlf_eof:
        or      cx,cx
        jnz     rlf_end                 ; a last line with no newline
        stc
        ret
rlf_end:
        mov     byte ptr [di],0
        mov     si,OFFSET line_buf+2
        call    puts
        call    crlf
        mov     si,OFFSET line_buf+2
        clc
        ret

;---------------------------------------------------------------------
; stdin_to_con - point standard input back at the console.
;---------------------------------------------------------------------
stdin_to_con:
        mov     dx,OFFSET dev_con
        mov     ax,3D00h
        int     21h
        jc      stc_out
        mov     bx,ax
        xor     cx,cx
        mov     ah,46h
        int     21h
        mov     ah,3Eh
        int     21h
stc_out:
        ret

;=====================================================================
; show_prompt - PROMPT's text, with its $ codes, or $P$G if it is unset.
;
; $P$G and not DOS's bare "C>": a DOS box is opened FROM CMD.EXE, whose
; prompt is $P$G, and a second shell that looked different from the
; first would only make the user wonder which one they were in.
;=====================================================================
show_prompt:
        push    es
        push    si
        push    di
        mov     si,OFFSET name_prompt
        call    env_find                ; ES:BX -> the value
        jc      sp_dflt
        cmp     byte ptr es:[bx],0
        jne     sp_have
sp_dflt:
        push    cs
        pop     es
        mov     bx,OFFSET dflt_prompt
sp_have:
        mov     di,bx
sp_loop:
        mov     al,es:[di]
        inc     di
        or      al,al
        jz      sp_done
        cmp     al,'$'
        je      sp_meta
        call    putc
        jmp     sp_loop
sp_meta:
        mov     al,es:[di]
        or      al,al
        jz      sp_done
        inc     di
        call    upcase
        mov     si,OFFSET prompt_codes
sp_find:
        cmp     byte ptr [si],0
        je      sp_loop                 ; not a code: DOS drops it
        cmp     al,[si]
        je      sp_code
        add     si,3
        jmp     sp_find
sp_code:
        push    di
        call    word ptr [si+1]
        pop     di
        jmp     sp_loop
sp_done:
        pop     di
        pop     si
        pop     es
        ret

pm_path:                                ; $P
        push    es
        push    cs
        pop     es
        mov     ah,19h
        int     21h
        mov     dl,al
        add     al,'A'
        call    putc
        mov     al,':'
        call    putc
        mov     al,'\'
        call    putc
        inc     dl
        mov     si,OFFSET pm_cwd
        mov     ah,47h
        int     21h
        jc      pmp_out
        call    puts
pmp_out:
        pop     es
        ret
pm_drive:                               ; $N
        mov     ah,19h
        int     21h
        add     al,'A'
        jmp     putc
pm_gt:  mov     al,'>'
        jmp     putc
pm_lt:  mov     al,'<'
        jmp     putc
pm_bar: mov     al,'|'
        jmp     putc
pm_eq:  mov     al,'='
        jmp     putc
pm_dol: mov     al,'$'
        jmp     putc
pm_esc: mov     al,1Bh
        jmp     putc
pm_bs:  mov     al,8                    ; $H: back over the last character
        call    putc
        mov     al,' '
        call    putc
        mov     al,8
        jmp     putc
pm_nl:  jmp     crlf
pm_time:                                ; $T
        call    put_time_now
        ret
pm_date:                                ; $D
        call    put_date_now
        ret
pm_ver:                                 ; $V
        call    put_version
        ret

;=====================================================================
; exit_shell - EXIT, or /C with nothing left to do.
;
; What our PSP said on the way in goes back first: this shell has been
; its own parent all along (see init), and DOS decides where a
; terminating program returns to from exactly those two fields.
;=====================================================================
exit_shell:
        cmp     byte ptr [permanent],0
        jne     xs_perm
        mov     ax,[orig_parent]
        mov     ds:[PSP_PARENT],ax
        mov     ax,word ptr [orig_term]
        mov     ds:[PSP_TERM],ax
        mov     ax,word ptr [orig_term+2]
        mov     ds:[PSP_TERM+2],ax
        mov     al,[errlevel]
        mov     ah,4Ch
        int     21h
xs_perm:
        mov     byte ptr [single_mode],SM_NONE
        ret

;=====================================================================
; exec_line - run one line: pipes first, then each stage on its own.
;=====================================================================
exec_line:
        push    si
        call    find_pipe
        pop     si
        or      di,di
        jnz     pipeline
        jmp     exec_simple

; find_pipe - DI -> the first '|' in the ASCIIZ at SI, or 0.  Quotes
;             mean nothing here, as they meant nothing to DOS 6.22.
find_pipe:
        mov     di,si
fpp_loop:
        mov     al,[di]
        or      al,al
        jz      fpp_none
        cmp     al,'|'
        je      fpp_out
        inc     di
        jmp     fpp_loop
fpp_none:
        xor     di,di
fpp_out:
        ret

;---------------------------------------------------------------------
; pipeline - A | B | C, the way DOS did it: each stage runs to the end
;            with its output in a temporary file, and the next stage
;            reads that file.  DOS is single-tasking; there is no other
;            way.  Each file is deleted as soon as its reader is done.
;---------------------------------------------------------------------
pipeline:
        mov     [pl_this],si            ; the stage about to run
        mov     byte ptr [pl_first],1
pl_stage:
        mov     si,[pl_this]
        call    find_pipe
        mov     [pl_bar],di
        or      di,di
        jz      pl_last
        mov     byte ptr [di],0         ; this stage ends here...
        inc     di
        mov     [pl_rest],di            ; ...and the next starts here
pl_last:
        xor     cx,cx                   ; redirections this stage made
        cmp     byte ptr [pl_first],0
        jne     pl_noin
        mov     dx,OFFSET pa_name       ; the last stage's output
        mov     ax,3D00h
        int     21h
        jc      pl_noin
        xor     bx,bx
        call    redir_push
        jc      pl_noin
        inc     cx
pl_noin:
        cmp     word ptr [pl_bar],0
        je      pl_run
        mov     di,OFFSET pb_name
        call    pipe_new
        jc      pl_nofile
        mov     byte ptr [pb_live],1
        mov     bx,1
        call    redir_push
        jc      pl_run
        inc     cx
pl_run:
        push    cx
        mov     si,[pl_this]
        call    exec_simple
        pop     cx
        cmp     byte ptr [cc_broken],0  ; a stage ended by ^C or Abort ends
        je      pl_unwind               ; the pipe: no stage after it runs
        mov     word ptr [pl_bar],0
        jmp     short pl_unwind
pl_nofile:
        mov     si,OFFSET msg_pipefail
        call    puts_err
        mov     word ptr [pl_bar],0     ; and no further stages
pl_unwind:
        jcxz    pl_popped
pl_pop:
        call    redir_pop
        loop    pl_pop
pl_popped:
        cmp     byte ptr [pa_live],0    ; the input file has been read
        je      pl_next
        mov     dx,OFFSET pa_name
        mov     ah,41h
        int     21h
        mov     byte ptr [pa_live],0
pl_next:
        mov     byte ptr [pl_first],0
        cmp     word ptr [pl_bar],0
        je      pl_done
        ; this stage's output is the next one's input
        mov     si,OFFSET pb_name
        mov     di,OFFSET pa_name
        call    strcpy
        mov     al,[pb_live]
        mov     [pa_live],al
        mov     byte ptr [pb_live],0
        mov     ax,[pl_rest]
        mov     [pl_this],ax
        jmp     pl_stage
pl_done:
        ret

;---------------------------------------------------------------------
; pipe_new - create a temporary file for a pipe.  DI -> where its name
;            goes.  AX = the handle, CF on failure.
;
; In TEMP if that is set and usable, in the root of the current drive
; otherwise, which is where DOS put them.  The name is ours rather than
; 5Ah's: RMDOS has no 5Ah, and a counter with 5Bh (create NEW) is just
; as unique - two shells in one pipeline cannot pick the same name,
; because the second 5Bh fails and it moves on.
;---------------------------------------------------------------------
pipe_new:
        push    bx
        push    cx
        push    si
        mov     [pn_dest],di
        mov     byte ptr [pn_triedroot],0
        mov     si,OFFSET name_temp
        call    env_find
        jc      pn_root
        ; TEMP's value, and a backslash if it has none
        mov     di,[pn_dest]            ; (env_find answered in DI)
        mov     si,bx
        mov     cx,PATH_MAX - 14
pn_tcp:
        mov     al,es:[si]
        or      al,al
        jz      pn_tend
        mov     [di],al
        inc     si
        inc     di
        loop    pn_tcp
pn_tend:
        push    cs
        pop     es
        cmp     di,[pn_dest]
        je      pn_root
        cmp     byte ptr [di-1],'\'
        je      pn_name
        mov     byte ptr [di],'\'
        inc     di
        jmp     short pn_name
pn_root:
        push    cs
        pop     es
        mov     byte ptr [pn_triedroot],1
        mov     di,[pn_dest]
        mov     ah,19h
        int     21h
        add     al,'A'
        mov     [di],al
        mov     byte ptr [di+1],':'
        mov     byte ptr [di+2],'\'
        add     di,3
pn_name:
        mov     [pn_tail],di
        mov     cx,200                  ; tries before giving up
pn_try:
        mov     di,[pn_tail]
        mov     si,OFFSET pipe_stem     ; "%PIPE"
        call    strcpy
        inc     word ptr [pipe_ctr]
        mov     ax,[pipe_ctr]
        and     ax,0FFFh
        call    hex3                    ; three hex digits at DI
        mov     si,OFFSET pipe_ext      ; ".$$$"
        call    strcpy
        push    cx
        mov     dx,[pn_dest]
        xor     cx,cx
        mov     ah,5Bh
        int     21h
        pop     cx
        jnc     pn_ok
        cmp     ax,50h                  ; exists: another number
        je      pn_again
        cmp     byte ptr [pn_triedroot],0
        jne     pn_fail
        jmp     pn_root                 ; TEMP names nowhere usable
pn_again:
        loop    pn_try
pn_fail:
        stc
pn_ok:
        pop     si
        pop     cx
        pop     bx
        ret

; hex3 - AX (0-FFFh) as three hex digits at DI; DI after them
hex3:
        push    cx
        mov     cx,3
        shl     ax,4
h3_loop:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     h3_d
        add     al,7
h3_d:
        mov     [di],al
        inc     di
        pop     ax
        loop    h3_loop
        mov     byte ptr [di],0
        pop     cx
        ret

;=====================================================================
; exec_simple - one command with no '|' in it: take < > >> off it, put
;               them in force, run what is left, and take them away.
;
; FOR keeps its redirections.  DOS applied them to each command the
; loop ran, not to the loop - which is why FOR ... DO TYPE %F >> ALL
; is written with >> - so the body is left to find them itself.
;=====================================================================
exec_simple:
        push    si
        call    skip_dl
        mov     di,OFFSET cn_for
        call    word_is
        pop     si
        jnz     es_redir
        jmp     dispatch

es_redir:
        mov     di,OFFSET seg_buf
        mov     byte ptr [rd_inname],0
        mov     byte ptr [rd_outname],0
        mov     byte ptr [rd_append],0
es_copy:
        lodsb
        or      al,al
        jz      es_copied
        cmp     al,'<'
        je      es_lt
        cmp     al,'>'
        je      es_gt
        cmp     di,OFFSET seg_buf + EXP_MAX
        jae     es_copy
        stosb
        jmp     es_copy
es_lt:
        mov     bx,OFFSET rd_inname
        call    rd_name
        jmp     es_copy
es_gt:
        mov     byte ptr [rd_append],0
        cmp     byte ptr [si],'>'
        jne     es_gt1
        inc     si
        mov     byte ptr [rd_append],1
es_gt1:
        ; The redirection is taken out and nothing put in its place, as
        ; DOS did: "ECHO text>F" writes "text" with no blank after it,
        ; and the blank that ends the file name in "PROG>F ARG" still
        ; keeps PROG and ARG apart.
        mov     bx,OFFSET rd_outname
        call    rd_name
        jmp     es_copy
es_copied:
        mov     byte ptr [di],0

        xor     cx,cx
        cmp     byte ptr [rd_inname],0
        je      es_noin
        mov     dx,OFFSET rd_inname
        mov     ax,3D00h
        int     21h
        jc      es_innf
        xor     bx,bx
        call    redir_push
        jc      es_run
        inc     cx
es_noin:
        cmp     byte ptr [rd_outname],0
        je      es_run
        mov     dx,OFFSET rd_outname
        call    open_output
        jc      es_nocreate
        mov     bx,1
        call    redir_push
        jc      es_run
        inc     cx
es_run:
        push    cx
        mov     si,OFFSET seg_buf
        call    dispatch
        pop     cx
es_unwind:
        jcxz    es_out
es_pop:
        call    redir_pop
        loop    es_pop
es_out:
        ret
es_innf:
        mov     si,OFFSET msg_notfound
        call    puts_err
        jmp     es_unwind
es_nocreate:
        mov     si,OFFSET msg_nocreate
        call    puts_err
        jmp     es_unwind

; rd_name - the file after < or >: SI after the sign, BX -> buffer.
;           Blanks first, then everything up to a blank, a sign or the
;           end.  SI is left after it.
rd_name:
        call    skip_bl
        push    di
        mov     di,bx
        xor     cx,cx
rdn_loop:
        mov     al,[si]
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
        inc     si
        cmp     cx,PATH_MAX - 1
        jae     rdn_loop
        mov     [di],al
        inc     di
        inc     cx
        jmp     rdn_loop
rdn_end:
        mov     byte ptr [di],0
        pop     di
        ret

;---------------------------------------------------------------------
; open_output - DX -> a name; [rd_append] says >>.  AX = the handle,
;               CF if it could not be made.
;
; >> onto a file that ends in ^Z backs up over it, as DOS did, or
; everything appended would sit after the end-of-file mark where TYPE
; and every ASCII copy would never see it.
;---------------------------------------------------------------------
open_output:
        push    bx
        push    cx
        push    dx
        cmp     byte ptr [rd_append],0
        je      oo_create
        mov     ax,3D02h                ; read AND write, for the ^Z
        int     21h
        jc      oo_wronly
        mov     bx,ax
        xor     cx,cx
        xor     dx,dx
        mov     ax,4202h
        int     21h
        jc      oo_have
        mov     cx,dx
        mov     dx,ax
        or      ax,cx
        jz      oo_have                 ; empty: nothing to back over
        sub     dx,1
        sbb     cx,0
        push    cx
        push    dx
        mov     ax,4200h
        int     21h
        mov     dx,OFFSET rl_byte
        mov     cx,1
        mov     ah,3Fh
        int     21h
        pop     dx
        pop     cx
        jc      oo_end
        cmp     byte ptr [rl_byte],1Ah
        jne     oo_end
        mov     ax,4200h                ; onto the ^Z, to write over it
        int     21h
        jmp     short oo_have
oo_end:
        xor     cx,cx
        xor     dx,dx
        mov     ax,4202h
        int     21h
        jmp     short oo_have
oo_wronly:
        pop     dx
        push    dx
        mov     ax,3D01h                ; a read-only... no: a device
        int     21h                     ; that will not be read
        jc      oo_create
        mov     bx,ax
        xor     cx,cx
        xor     dx,dx
        mov     ax,4202h
        int     21h
        jmp     short oo_have
oo_create:
        pop     dx
        push    dx
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      oo_fail
        mov     bx,ax
oo_have:
        mov     ax,bx
        clc
oo_fail:
        pop     dx
        pop     cx
        pop     bx
        ret

;---------------------------------------------------------------------
; redir_push - BX = 0 or 1, AX = an open file.  Make the file that
;              standard handle, remembering what it was.  The file's
;              own handle is closed: from here on only the standard one
;              refers to it.  CF on failure (the file is closed anyway).
;              Everything but AX preserved.
;
; redir_pop  - undo the most recent one.
;
; A stack rather than locals, so that a ^C can undo every redirection
; in force, from the inside out, without knowing whose they were.
;---------------------------------------------------------------------
redir_push:
        push    bx
        push    cx
        push    dx
        push    di
        mov     dx,ax                   ; DX = the file
        cmp     word ptr [rd_depth],RD_MAX
        jae     rp_fail
        mov     ah,45h                  ; the standard handle, kept
        int     21h
        jc      rp_fail
        mov     di,[rd_depth]
        shl     di,2
        mov     [rd_stack+di],bx
        mov     [rd_stack+di+2],ax
        inc     word ptr [rd_depth]
        mov     cx,bx                   ; the file becomes it
        mov     bx,dx
        mov     ah,46h
        int     21h
        mov     ah,3Eh
        int     21h
        clc
        jmp     short rp_out
rp_fail:
        mov     bx,dx
        mov     ah,3Eh
        int     21h
        stc
rp_out:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret

redir_pop:
        push    ax
        push    bx
        push    cx
        push    di
        cmp     word ptr [rd_depth],0
        je      rpp_out
        dec     word ptr [rd_depth]
        mov     di,[rd_depth]
        shl     di,2
        mov     cx,[rd_stack+di]        ; which standard handle
        mov     bx,[rd_stack+di+2]      ; what it was
        mov     ah,46h
        int     21h
        mov     ah,3Eh
        int     21h
rpp_out:
        pop     di
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; dispatch - one plain command: a drive change, an internal command,
;            or a program to find.  SI -> the text, ASCIIZ.
;=====================================================================
dispatch:
        call    skip_dl
        mov     al,[si]
        or      al,al
        jz      dp_none
        cmp     al,':'                  ; a label: nothing to run
        je      dp_none
        mov     [verb_start],si
        call    get_verb
        mov     [args_ptr],si

        cmp     byte ptr [verb_buf+2],0 ; X:
        jne     dp_notdrv
        cmp     byte ptr [verb_buf+1],':'
        jne     dp_notdrv
        jmp     drive_change
dp_notdrv:
        call    find_internal
        or      bx,bx
        jz      dp_external
        mov     si,[args_ptr]
        call    want_help
        jc      dp_help
        mov     si,[args_ptr]
        call    word ptr [bx+IT_FUNC]
dp_none:
        ret
dp_help:
        jmp     show_help
dp_external:
        jmp     run_external

;---------------------------------------------------------------------
; get_verb - the command word at SI into verb_buf, upper case.  SI is
;            left on the character that ended it, which is where the
;            arguments start - "DIR/W" hands DIR "/W".
;---------------------------------------------------------------------
get_verb:
        mov     di,OFFSET verb_buf
        xor     cx,cx
gv_loop:
        mov     al,[si]
        or      al,al
        jz      gv_end
        call    is_dl
        je      gv_end
        cmp     al,'/'
        je      gv_end
        cmp     al,'+'
        je      gv_end
        cmp     al,'"'
        je      gv_end
        inc     si
        cmp     cx,VERB_MAX - 1
        jae     gv_loop
        call    upcase
        mov     [di],al
        inc     di
        inc     cx
        jmp     gv_loop
gv_end:
        mov     byte ptr [di],0
        ret

;---------------------------------------------------------------------
; find_internal - BX = the int_table row verb_buf names, or 0.
;
; "CD..", "CD\DOS" and "ECHO." are one word to get_verb and a command
; plus its argument to DOS.  So a row flagged ITF_PREFIX also matches a
; verb that STARTS with its name and goes on with '.' or '\', and the
; arguments are moved back to begin there.  Only those rows: "CLS.BAT"
; is a batch file, not CLS.
;---------------------------------------------------------------------
find_internal:
        mov     bx,OFFSET int_table
fi_row:
        mov     si,[bx+IT_NAME]
        or      si,si
        jz      fi_none
        mov     di,OFFSET verb_buf
fi_cmp:
        mov     al,[si]
        or      al,al
        jz      fi_nameend
        cmp     al,[di]
        jne     fi_next
        inc     si
        inc     di
        jmp     fi_cmp
fi_nameend:
        mov     al,[di]
        or      al,al
        jz      fi_hit
        test    byte ptr [bx+IT_FLAGS],ITF_PREFIX
        jz      fi_next
        cmp     al,'.'
        je      fi_prefix
        cmp     al,'\'
        je      fi_prefix
fi_next:
        add     bx,IT_SIZE
        jmp     fi_row
fi_prefix:
        sub     di,OFFSET verb_buf      ; get_verb copied one for one,
        add     di,[verb_start]         ; so this is the same place in
        mov     [args_ptr],di           ; the line
fi_hit:
        ret
fi_none:
        xor     bx,bx
        ret

; want_help - CF if the arguments at SI begin with /?
want_help:
        call    skip_bl
        cmp     byte ptr [si],'/'
        jne     wh_no
        cmp     byte ptr [si+1],'?'
        jne     wh_no
        stc
        ret
wh_no:
        clc
        ret

;---------------------------------------------------------------------
; show_help - BX = the command's row.  Its text is in this file, past
;             the end of what stays in memory (see the header), so it
;             is read back from the file named when we were started.
;---------------------------------------------------------------------
show_help:
        mov     cx,[bx+IT_HLEN]
        jcxz    shl_none
        mov     [hl_len],cx
        mov     ax,[bx+IT_HELP]
        mov     [hl_pos],ax
        cmp     byte ptr [self_path],0
        je      shl_none
        mov     bx,cx
        add     bx,15
        shr     bx,4
        call    work_alloc
        jc      shl_none
        mov     dx,OFFSET self_path
        mov     ax,3D00h
        int     21h
        jc      shl_free
        mov     bx,ax
        mov     [h_misc],ax
        xor     cx,cx
        mov     dx,[hl_pos]
        mov     ax,4200h
        int     21h
        push    ds
        mov     ds,cs:[work_seg]
        xor     dx,dx
        mov     cx,cs:[hl_len]
        mov     ah,3Fh
        int     21h
        pop     ds
        pushf
        push    ax
        mov     ah,3Eh
        int     21h
        mov     word ptr [h_misc],-1
        pop     cx
        popf
        jc      shl_free
        push    ds
        mov     ds,cs:[work_seg]
        xor     dx,dx
        mov     bx,1
        mov     ah,40h
        int     21h
        pop     ds
shl_free:
        call    work_free
        ret
shl_none:
        mov     si,OFFSET msg_nohelp
        jmp     puts_err

;---------------------------------------------------------------------
; drive_change - "X:".  The drive is taken if DOS says it now is the
;                current one, which is DOS's own test: 0Eh reports how
;                many drives there are, not whether this one exists.
;---------------------------------------------------------------------
drive_change:
        mov     dl,[verb_buf]
        sub     dl,'A'
        cmp     dl,26
        jae     dc_bad
        mov     ah,0Eh
        int     21h
        mov     ah,19h
        int     21h
        cmp     al,dl
        je      dc_out
dc_bad:
        mov     si,OFFSET msg_baddrv
        call    puts_err
dc_out:
        ret

;=====================================================================
; run_external - not internal: find it and run it, or say so.
;=====================================================================
run_external:
        call    find_program
        jc      rx_bad
        cmp     byte ptr [prog_isbat],0
        jne     rx_bat
        jmp     exec_program
rx_bat:
        jmp     batch_start
rx_bad:
        mov     si,OFFSET msg_badcmd
        jmp     puts_err

;---------------------------------------------------------------------
; find_program - verb_buf as a program.  CF clear: prog_path names it
;                and prog_isbat says whether it is a batch file.
;
; Where it has a path, there and only there.  Otherwise the current
; directory, then each PATH entry in order - and in each directory
; .COM, .EXE and .BAT in that order, which is the order DOS used and
; the reason a FOO.COM hides a FOO.EXE beside it.  A name with an
; extension of its own is looked for with that extension only, and
; anything but those three is not a program.
;---------------------------------------------------------------------
find_program:
        mov     si,OFFSET verb_buf
fpg_wild:
        lodsb
        or      al,al
        jz      fpg_nowild
        cmp     al,'*'
        je      fpg_no
        cmp     al,'?'
        je      fpg_no
        jmp     fpg_wild
fpg_nowild:
        ; where does the name part start, and has it an extension?
        mov     si,OFFSET verb_buf
        call    name_part               ; DI -> the name within it
        mov     [fp_name],di
        mov     byte ptr [fp_hasext],0
fpg_ext:
        mov     al,[di]
        or      al,al
        jz      fpg_extdone
        inc     di
        cmp     al,'.'
        jne     fpg_ext
        mov     byte ptr [fp_hasext],1
        mov     [fp_extp],di
        mov     si,OFFSET ext_list
fpg_extok:
        cmp     byte ptr [si],0
        je      fpg_no                  ; .TXT is not a program
        push    si
        push    di
        call    stricmp
        pop     di
        pop     si
        je      fpg_extdone
        add     si,5
        jmp     fpg_extok
fpg_extdone:
        mov     di,[fp_name]
        cmp     di,OFFSET verb_buf
        je      fpg_search
        ; a path of its own: that directory and nowhere else
        mov     si,OFFSET verb_buf
        mov     di,OFFSET fp_dir
fpg_cpdir:
        cmp     si,[fp_name]
        je      fpg_dirend
        movsb
        jmp     fpg_cpdir
fpg_dirend:
        mov     byte ptr [di],0
        call    fp_trydir
        ret
fpg_search:
        mov     byte ptr [fp_dir],0     ; the current directory first
        call    fp_trydir
        jnc     fpg_found
        mov     word ptr [fp_pathpos],0
fpg_pathnext:
        call    fp_nextpath             ; fp_dir <- the next PATH entry
        jc      fpg_no
        call    fp_trydir
        jc      fpg_pathnext
fpg_found:
        ret
fpg_no:
        stc
        ret

; fp_trydir - look in fp_dir for the name.  CF clear if found.
fp_trydir:
        mov     di,OFFSET prog_path
        mov     si,OFFSET fp_dir
        call    strcpy
        cmp     di,OFFSET prog_path
        je      fpt_noslash
        mov     al,[di-1]
        cmp     al,'\'
        je      fpt_noslash
        cmp     al,':'
        je      fpt_noslash
        mov     byte ptr [di],'\'
        inc     di
fpt_noslash:
        mov     si,[fp_name]
        call    strcpy                  ; DI at its NUL
        mov     [fp_end],di
        cmp     byte ptr [fp_hasext],0
        je      fpt_each
        call    fp_exists
        jc      fpt_out
        mov     si,[fp_extp]
        jmp     short fpt_kind
fpt_each:
        mov     si,OFFSET ext_list
fpt_next:
        cmp     byte ptr [si],0
        je      fpt_none
        mov     di,[fp_end]
        mov     byte ptr [di],'.'
        inc     di
        push    si
        call    strcpy
        pop     si
        call    fp_exists
        jnc     fpt_kind
        add     si,5
        jmp     fpt_next
fpt_none:
        stc
fpt_out:
        ret
fpt_kind:
        ; SI -> the extension it was found with
        mov     byte ptr [prog_isbat],0
        mov     di,OFFSET ext_bat
        push    si
        call    stricmp
        pop     si
        jne     fpt_prog
        mov     byte ptr [prog_isbat],1
fpt_prog:
        clc
        ret

; fp_exists - CF clear if prog_path names a file
fp_exists:
        push    si
        call    set_dta
        mov     dx,OFFSET prog_path
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        pop     si
        ret

; fp_nextpath - the next ';'-separated entry of PATH into fp_dir.  CF
;               when there are no more.  [fp_pathpos] is how far in.
fp_nextpath:
        push    es
        mov     si,OFFSET name_path
        call    env_find
        jc      fnp_none
        add     bx,[fp_pathpos]
fnp_skip:
        mov     al,es:[bx]
        or      al,al
        jz      fnp_none
        cmp     al,';'
        je      fnp_sep
        cmp     al,' '
        jne     fnp_copy
fnp_sep:
        inc     bx
        inc     word ptr [fp_pathpos]
        jmp     fnp_skip
fnp_copy:
        mov     di,OFFSET fp_dir
        xor     cx,cx
fnp_cp:
        mov     al,es:[bx]
        or      al,al
        jz      fnp_end
        cmp     al,';'
        je      fnp_end
        inc     bx
        inc     word ptr [fp_pathpos]
        cmp     cx,PATH_MAX - 14
        jae     fnp_cp
        mov     [di],al
        inc     di
        inc     cx
        jmp     fnp_cp
fnp_end:
        mov     byte ptr [di],0
        pop     es
        clc
        ret
fnp_none:
        pop     es
        stc
        ret

;---------------------------------------------------------------------
; exec_program - run prog_path with the arguments at [args_ptr].
;
; The tail is the text after the name exactly as it was typed, blank
; and all - "DIR /W" hands DIR " /W" and "PROG/X" hands PROG "/X" -
; and the two FCBs are its first two words, parsed by 29h, because
; programs from DOS 1 still read their arguments out of those.
;---------------------------------------------------------------------
exec_program:
        mov     si,[args_ptr]
        mov     di,OFFSET exec_tail+1
        xor     cx,cx
ep_tail:
        lodsb
        or      al,al
        jz      ep_tailend
        cmp     cx,TAIL_MAX
        jae     ep_tailend
        stosb
        inc     cx
        jmp     ep_tail
ep_tailend:
        mov     byte ptr [di],0Dh
        mov     [exec_tail],cl

        mov     di,OFFSET fcb1
        mov     cx,74
        xor     al,al
        rep     stosb
        mov     si,[args_ptr]
        mov     di,OFFSET fcb1
        mov     ax,2901h
        int     21h
        mov     di,OFFSET fcb2
        mov     ax,2901h
        int     21h

        mov     ax,[env_seg]
        mov     [pb_env],ax
        mov     word ptr [pb_tail],OFFSET exec_tail
        mov     [pb_tail+2],cs
        mov     word ptr [pb_fcb1],OFFSET fcb1
        mov     [pb_fcb1+2],cs
        mov     word ptr [pb_fcb2],OFFSET fcb2
        mov     [pb_fcb2+2],cs

        ; DOS 2 lost every register but CS:IP across an EXEC, SS:SP
        ; included, and nothing since has promised otherwise.
        mov     [exec_sp],sp
        mov     dx,OFFSET prog_path
        mov     bx,OFFSET parm_block
        mov     ax,4B00h
        int     21h
        cli
        mov     bx,cs
        mov     ss,bx
        mov     sp,cs:[exec_sp]
        sti
        mov     ds,bx
        mov     es,bx
        cld
        jc      ep_fail

        mov     ah,4Dh
        int     21h
        mov     [errlevel],al
        call    set_dta                 ; the child had its own
        call    set_vectors
        cmp     ah,1                    ; ended by ^C, or by Abort at
        je      ep_broken               ; the critical error prompt
        cmp     ah,2
        jne     ep_out
ep_broken:
        mov     byte ptr [cc_broken],1  ; its FOR or pipeline goes with it,
        call    ask_endbat              ; whatever the batch answer
ep_out:
        ret

ep_fail:
        mov     si,OFFSET msg_badcmd
        cmp     ax,2
        je      ep_say
        cmp     ax,3
        je      ep_say
        mov     si,OFFSET msg_denied
        cmp     ax,5
        je      ep_say
        mov     si,OFFSET msg_toobig
        cmp     ax,8
        je      ep_say
        mov     si,OFFSET msg_execfail
ep_say:
        jmp     puts_err

;=====================================================================
; The ^C and critical-error handlers, and where they come home to.
;=====================================================================

;---------------------------------------------------------------------
; int23_hand - ^C.  Whose program is running decides everything.
;
; A CHILD'S: return with RETF and the carry set, which is how an INT
; 23h handler tells DOS to terminate the program - DOS then comes back
; through our EXEC and 4Dh says it was a ^C.
;
; OURS: abandon the command.  DOS calls this handler with the stack as
; it was when we made the DOS call, having finished with its own, so
; putting SP back at the top of the main loop leaves nothing of DOS's
; behind.
;---------------------------------------------------------------------
int23_hand:
        push    ax
        push    bx
        mov     ah,62h
        int     21h
        mov     ax,cs
        cmp     bx,ax
        pop     bx
        pop     ax
        je      cmd_home
        stc
        retf

;---------------------------------------------------------------------
; cmd_home - abandon the command and go back to the loop, asking the
;            batch question on the way if a batch file is running.
; abort_entry - the same, arrived at from DOS: our PSP's terminate
;               address points here (see init), and this is where DOS
;               sends us after it has "terminated" us for an Abort we
;               could not turn into a Fail.  Being our own parent is
;               what makes DOS leave our memory and files alone.
;---------------------------------------------------------------------
abort_entry:
        cli
        mov     ax,cs
        mov     ss,ax
        mov     sp,cs:[main_sp]
        sti
        mov     ds,ax
        mov     es,ax
        cld
        call    set_vectors             ; DOS put the parent's back
        jmp     short ch_common
cmd_home:
        cli
        mov     ax,cs
        mov     ss,ax
        mov     sp,cs:[main_sp]
        sti
        mov     ds,ax
        mov     es,ax
        cld
ch_common:
        call    abandon
        call    crlf
        call    ask_endbat
        jmp     main_loop

;---------------------------------------------------------------------
; abandon - undo everything a command in progress may have left: the
;           files it had open, the redirections in force, the pipe
;           files, the memory it borrowed, the DTA.
;---------------------------------------------------------------------
abandon:
        mov     byte ptr [for_active],0
        mov     byte ptr [call_flag],0
        mov     si,OFFSET open_hdls
        mov     cx,OPEN_SLOTS
ab_close:
        mov     bx,[si]
        cmp     bx,-1
        je      ab_nc
        mov     ah,3Eh
        int     21h
        mov     word ptr [si],-1
ab_nc:
        add     si,2
        loop    ab_close
ab_redir:
        cmp     word ptr [rd_depth],0
        je      ab_pipes
        call    redir_pop
        jmp     ab_redir
ab_pipes:
        cmp     byte ptr [pa_live],0
        je      ab_pb
        mov     dx,OFFSET pa_name
        mov     ah,41h
        int     21h
        mov     byte ptr [pa_live],0
ab_pb:
        cmp     byte ptr [pb_live],0
        je      ab_mem
        mov     dx,OFFSET pb_name
        mov     ah,41h
        int     21h
        mov     byte ptr [pb_live],0
ab_mem:
        call    work_free
        call    for_unpark
        call    set_dta
        ret

;---------------------------------------------------------------------
; ask_endbat - "Terminate batch job (Y/N)?", if a batch file is running.
;---------------------------------------------------------------------
ask_endbat:
        cmp     word ptr [bat_depth],0
        je      aeb_out
        ; UNDER /C THERE IS NO QUESTION: the batch file ends.  That is
        ; 6.22's ContC (SingleCom set, straight to ContCTerm), and the
        ; /C command ends with it.
        cmp     byte ptr [single_mode],SM_DONE
        je      aeb_end
        ; ...AND WHAT WAS TYPED AHEAD DOES NOT ANSWER IT.  6.22 asks with
        ; 0C01h, flushing the keyboard before it reads, so a key pressed
        ; before the question was on the screen cannot end the batch.
        mov     ax,0C00h
        int     21h
        mov     si,OFFSET msg_termbat
        call    ask_yn_err
        cmp     al,'Y'
        jne     aeb_out
aeb_end:
        call    bat_end_all
aeb_out:
        ret

;---------------------------------------------------------------------
; int24_hand - Abort, Retry, Ignore, Fail?
;
; ONLY 01h-0Ch MAY BE CALLED HERE, which is DOS's rule and not ours:
; the handler runs inside DOS, on DOS's own stack.  Those functions
; read and write standard input and output - which the command that
; hit the error may well have redirected to a file on the very disk
; that failed.  So, as DOS's COMMAND.COM did, the current program's
; handles 0 and 1 are pointed at OUR standard error for the length of
; the question (savhand/resthand), and put back after.
;
; ABORT in one of our own commands would have DOS terminate the shell.
; Where Fail is allowed, the answer becomes Fail and crit_abort is set:
; the command fails its way out and the main loop asks the batch
; question an Abort would have.  Where it is not, Abort it is, and DOS
; brings us back through abort_entry.  In a child's command, Abort is
; Abort: the child ends and our EXEC returns.
;---------------------------------------------------------------------
int24_hand:
        sti
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        push    cs
        pop     ds
        cld
        mov     [ce_flags],ah
        mov     [ce_drive],al
        mov     [ce_code],di
        mov     [ce_devoff],si
        mov     [ce_devseg],bp

        cmp     byte ptr [crit_fail],0  ; COMMAND /F: never ask
        je      ce_ask
        mov     al,3
        test    byte ptr [ce_flags],CE_FAIL_OK
        jnz     ce_answer
        mov     al,2
        jmp     ce_answer

ce_ask:
        call    savhand
        mov     ah,0Fh                  ; start on a line of our own
        int     10h
        mov     ah,03h
        int     10h
        or      dl,dl
        jz      ce_col0
        mov     si,OFFSET ce_crlf
        call    ce_puts
ce_col0:
        mov     bx,[ce_code]
        xor     bh,bh
        cmp     bl,CE_MAXCODE
        jbe     ce_known
        mov     bl,0Ch
ce_known:
        shl     bx,1
        mov     si,[ce_msgs+bx]
        call    ce_puts
        mov     si,OFFSET ce_reading
        test    byte ptr [ce_flags],CE_WRITE
        jz      ce_rw
        mov     si,OFFSET ce_writing
ce_rw:
        call    ce_puts
        test    byte ptr [ce_flags],CE_NOTDISK
        jz      ce_disk
        les     di,dword ptr [ce_devoff]
        test    byte ptr es:[di+5],80h  ; a character device: its name
        jz      ce_disk
        mov     si,OFFSET ce_device
        call    ce_puts
        mov     cx,8
ce_devname:
        mov     dl,es:[di+0Ah]
        cmp     dl,' '
        je      ce_nl
        mov     ah,02h
        int     21h
        inc     di
        loop    ce_devname
        jmp     short ce_nl
ce_disk:
        mov     si,OFFSET ce_drvword
        call    ce_puts
        mov     dl,[ce_drive]
        add     dl,'A'
        mov     ah,02h
        int     21h
ce_nl:
        push    cs
        pop     es
        mov     si,OFFSET ce_crlf
        call    ce_puts

        ; the question, with only the answers AH allows
        mov     si,OFFSET ce_abort
        call    ce_puts
        test    byte ptr [ce_flags],CE_RETRY_OK
        jz      ce_q2
        mov     si,OFFSET ce_retry
        call    ce_puts
ce_q2:
        test    byte ptr [ce_flags],CE_IGNORE_OK
        jz      ce_q3
        mov     si,OFFSET ce_ignore
        call    ce_puts
ce_q3:
        test    byte ptr [ce_flags],CE_FAIL_OK
        jz      ce_q4
        mov     si,OFFSET ce_fail
        call    ce_puts
ce_q4:
        mov     si,OFFSET ce_query
        call    ce_puts
ce_key:
        mov     ah,08h                  ; no echo: a key that means
        int     21h                     ; nothing is read again in silence
        call    upcase
        mov     ah,[ce_flags]
        mov     bl,2
        cmp     al,'A'
        je      ce_keyok
        mov     bl,1
        cmp     al,'R'
        jne     ce_notr
        test    ah,CE_RETRY_OK
        jnz     ce_keyok
        jmp     ce_key
ce_notr:
        xor     bl,bl
        cmp     al,'I'
        jne     ce_noti
        test    ah,CE_IGNORE_OK
        jnz     ce_keyok
        jmp     ce_key
ce_noti:
        mov     bl,3
        cmp     al,'F'
        jne     ce_key
        test    ah,CE_FAIL_OK
        jz      ce_key
ce_keyok:
        mov     dl,al
        mov     ah,02h
        int     21h
        mov     si,OFFSET ce_crlf
        call    ce_puts
        call    resthand
        mov     al,bl
        cmp     al,2
        jne     ce_answer
        push    ax
        mov     ah,62h                  ; Abort: whose command?
        int     21h
        pop     ax
        mov     cx,cs
        cmp     bx,cx
        jne     ce_answer               ; a child's: let DOS end it
        test    byte ptr [ce_flags],CE_FAIL_OK
        jz      ce_answer               ; must be Abort: see abort_entry
        mov     byte ptr [crit_abort],1
        mov     al,3
ce_answer:
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        iret

; ce_puts - ASCIIZ at SI, through 02h: the only output allowed here
ce_puts:
        push    ax
        push    dx
cep_loop:
        mov     dl,[si]
        or      dl,dl
        jz      cep_out
        mov     ah,02h
        int     21h
        inc     si
        jmp     cep_loop
cep_out:
        pop     dx
        pop     ax
        ret

; savhand - the current program's handles 0 and 1 -> our standard error
savhand:
        push    ax
        push    bx
        push    si
        push    es
        mov     ah,62h
        int     21h
        mov     es,bx
        les     si,es:[PSP_JFTPTR]
        mov     word ptr [ce_jft],si
        mov     word ptr [ce_jft+2],es
        mov     ax,es:[si]
        mov     [ce_save01],ax
        push    es
        les     bx,ds:[PSP_JFTPTR]      ; ours
        mov     al,es:[bx+2]
        pop     es
        mov     ah,al
        mov     es:[si],ax
        pop     es
        pop     si
        pop     bx
        pop     ax
        ret

; resthand - and back
resthand:
        push    ax
        push    si
        push    es
        les     si,dword ptr [ce_jft]
        mov     ax,[ce_save01]
        mov     es:[si],ax
        pop     es
        pop     si
        pop     ax
        ret

;---------------------------------------------------------------------
; set_vectors - INT 23h and 24h are ours.  Again after every program:
;               DOS restores them from the child's PSP, which is right,
;               but a child that hooked them and never went through
;               termination would leave them pointing at nothing.
;---------------------------------------------------------------------
set_vectors:
        push    ax
        push    dx
        mov     dx,OFFSET int23_hand
        mov     ax,2523h
        int     21h
        mov     dx,OFFSET int24_hand
        mov     ax,2524h
        int     21h
        pop     dx
        pop     ax
        ret

; set_dta - the DTA back to ours
set_dta:
        push    ax
        push    dx
        mov     dx,OFFSET dta
        mov     ah,1Ah
        int     21h
        pop     dx
        pop     ax
        ret

;=====================================================================
; Output.  Everything goes through handle 1 with 40h, which is what
; makes > work on it.
;=====================================================================

; putc - AL to standard output.  Everything preserved.
putc:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     [pc_byte],al
        mov     dx,OFFSET pc_byte
        mov     cx,1
        mov     bx,1
        mov     ah,40h
        int     21h
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; puts - the ASCIIZ at DS:SI.  Everything preserved.
; puts_err - the same, to standard error.  EVERY ERROR MESSAGE GOES
;            THERE, as DOS 6's did (its CError path is Std_EPrintf):
;            "Bad command or file name" is not part of a command's
;            output and must not land in the file it was redirected to.
puts_err:
        push    ax
        push    bx
        mov     bx,2
        jmp     short pts_h
puts:
        push    ax
        push    bx
        mov     bx,1
pts_h:
        push    cx
        push    dx
        push    di
        mov     di,si
        xor     cx,cx
pts_len:
        cmp     byte ptr [di],0
        je      pts_go
        inc     di
        inc     cx
        jmp     pts_len
pts_go:
        jcxz    pts_out
        mov     dx,si
        mov     ah,40h
        int     21h
pts_out:
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; puts_n - CX bytes at DS:SI
puts_n:
        push    ax
        push    bx
        push    dx
        jcxz    ptn_out
        mov     dx,si
        mov     bx,1
        mov     ah,40h
        int     21h
ptn_out:
        pop     dx
        pop     bx
        pop     ax
        ret

crlf:
        push    si
        mov     si,OFFSET str_crlf
        call    puts
        pop     si
        ret

crlf_err:
        push    si
        mov     si,OFFSET str_crlf
        call    puts_err
        pop     si
        ret

; putc_err - AL to standard error
putc_err:
        push    si
        mov     [pce_buf],al
        mov     si,OFFSET pce_buf
        call    puts_err
        pop     si
        ret

; putsp - CX blanks
putsp:
        jcxz    psp_out
        push    ax
        push    cx
psp_loop:
        mov     al,' '
        call    putc
        loop    psp_loop
        pop     cx
        pop     ax
psp_out:
        ret

;---------------------------------------------------------------------
; Numbers.
;
; fmt_num - EAX in decimal at num_buf, with the country's thousands
;           separator if [fn_sep] says so.  SI -> the digits, CX = how
;           many characters.
; put_num - the same, printed right-justified in a field of BX.
; put_dec - EAX, plain, no field.
; put_2d  - AL as two digits, zero-filled.
;---------------------------------------------------------------------
fmt_num:
        push    eax
        push    edx
        push    ebx
        push    di
        push    bp
        mov     di,OFFSET num_buf + 19
        mov     byte ptr [di],0
        xor     cx,cx                   ; characters
        xor     bp,bp                   ; digits since the last separator
        mov     ebx,10
fn_digit:
        xor     edx,edx
        div     ebx
        cmp     bp,3
        jne     fn_nosep
        cmp     byte ptr [fn_sep],0
        je      fn_nosep
        dec     di
        push    ax
        mov     al,[ctry_thou]
        mov     [di],al
        pop     ax
        inc     cx
        xor     bp,bp
fn_nosep:
        add     dl,'0'
        dec     di
        mov     [di],dl
        inc     cx
        inc     bp
        or      eax,eax
        jnz     fn_digit
        mov     si,di
        pop     bp
        pop     di
        pop     ebx
        pop     edx
        pop     eax
        ret

put_num:
        push    cx
        push    si
        call    fmt_num
        push    cx
        neg     cx
        add     cx,bx
        jle     pnm_nopad
        call    putsp
pnm_nopad:
        pop     cx
        call    puts_n
        pop     si
        pop     cx
        ret

put_dec:
        push    cx
        push    si
        push    word ptr [fn_sep]
        mov     byte ptr [fn_sep],0
        call    fmt_num
        call    puts_n
        pop     word ptr [fn_sep]
        pop     si
        pop     cx
        ret

put_2d:
        push    ax
        aam                             ; AH = tens, AL = units
        xchg    al,ah
        add     ax,'00'
        call    putc
        mov     al,ah
        call    putc
        pop     ax
        ret

;=====================================================================
; Strings and characters.
;=====================================================================

; upcase - AL to upper case (ASCII only - the country table's case map
;          is a far call nobody on a DOS 6 machine would notice missing)
upcase:
        cmp     al,'a'
        jb      uc_out
        cmp     al,'z'
        ja      uc_out
        sub     al,20h
uc_out:
        ret

; is_dl - ZF if AL is one of DOS's argument delimiters
is_dl:
        cmp     al,' '
        je      idl_out
        cmp     al,9
        je      idl_out
        cmp     al,','
        je      idl_out
        cmp     al,';'
        je      idl_out
        cmp     al,'='
idl_out:
        ret

; skip_bl - SI past blanks and tabs
skip_bl:
        cmp     byte ptr [si],' '
        je      sb_step
        cmp     byte ptr [si],9
        jne     sb_out
sb_step:
        inc     si
        jmp     skip_bl
sb_out:
        ret

; skip_dl - SI past every delimiter
skip_dl:
        mov     al,[si]
        or      al,al
        jz      sd_out
        call    is_dl
        jne     sd_out
        inc     si
        jmp     skip_dl
sd_out:
        ret

; strcpy - ASCIIZ from SI to DI.  DI is left ON the copied NUL, so a
;          second strcpy appends.  SI after the source's NUL.
strcpy:
        lodsb
        mov     [di],al
        or      al,al
        jz      scp_out
        inc     di
        jmp     strcpy
scp_out:
        ret

; strlen - CX = length of the ASCIIZ at SI
strlen:
        push    si
        xor     cx,cx
sl_loop:
        cmp     byte ptr [si],0
        je      sl_out
        inc     si
        inc     cx
        jmp     sl_loop
sl_out:
        pop     si
        ret

; stricmp - ZF if the ASCIIZ at SI and DI are equal, case ignored
stricmp:
        push    ax
sic_loop:
        mov     al,[si]
        call    upcase
        mov     ah,al
        mov     al,[di]
        call    upcase
        cmp     al,ah
        jne     sic_out
        or      al,al
        jz      sic_out
        inc     si
        inc     di
        jmp     sic_loop
sic_out:
        pop     ax
        ret

;---------------------------------------------------------------------
; word_is - ZF if the text at SI starts with the ASCIIZ word at DI (in
;           upper case) followed by something that ends a word.  SI is
;           advanced past the word when it matches.
;---------------------------------------------------------------------
word_is:
        push    si
        push    ax
wi_loop:
        mov     al,[di]
        or      al,al
        jz      wi_end
        mov     ah,al
        mov     al,[si]
        call    upcase
        cmp     al,ah
        jne     wi_no
        inc     si
        inc     di
        jmp     wi_loop
wi_end:
        mov     al,[si]
        or      al,al
        jz      wi_yes
        call    is_dl
        je      wi_yes
        cmp     al,'/'
        je      wi_yes
wi_no:
        pop     ax
        pop     si
        or      sp,sp                   ; NZ
        ret
wi_yes:
        pop     ax
        add     sp,2                    ; keep the advanced SI
        cmp     ax,ax                   ; ZF
        ret

;---------------------------------------------------------------------
; name_part - DI -> the last component of the path at SI: after the
;             last '\' or ':', or SI itself.
;---------------------------------------------------------------------
name_part:
        mov     di,si
        push    si
np_loop:
        lodsb
        or      al,al
        jz      np_out
        cmp     al,'\'
        je      np_mark
        cmp     al,':'
        jne     np_loop
np_mark:
        mov     di,si
        jmp     np_loop
np_out:
        pop     si
        ret

; has_wild - CF if the ASCIIZ at SI has * or ? in it
has_wild:
        push    si
        push    ax
hw_loop:
        lodsb
        or      al,al
        jz      hw_no
        cmp     al,'*'
        je      hw_yes
        cmp     al,'?'
        jne     hw_loop
hw_yes:
        stc
        jmp     short hw_out
hw_no:
        clc
hw_out:
        pop     ax
        pop     si
        ret

;---------------------------------------------------------------------
; get_arg - the next argument at SI into the buffer at DI (at most
;           PATH_MAX - 1 characters), ended by a delimiter or a '/'.
;           SI is left after it; CX = its length (0: there was none).
;---------------------------------------------------------------------
get_arg:
        call    skip_dl
        push    di
        xor     cx,cx
ga_loop:
        mov     al,[si]
        or      al,al
        jz      ga_end
        call    is_dl
        je      ga_end
        cmp     al,'/'
        jne     ga_take
        or      cx,cx                   ; a '/' ends a word, and starts
        jnz     ga_end                  ; a switch that is a word itself
ga_take:
        inc     si
        cmp     cx,PATH_MAX - 1
        jae     ga_loop
        mov     [di],al
        inc     di
        inc     cx
        cmp     al,'/'                  ; "/X": the switch letter too
        jne     ga_loop
        mov     al,[si]
        or      al,al
        jz      ga_end
        inc     si
        mov     [di],al
        inc     di
        inc     cx
        ; and "/A:HS" - a switch's own argument, up to the next word
ga_swarg:
        mov     al,[si]
        or      al,al
        jz      ga_end
        call    is_dl
        je      ga_end
        cmp     al,'/'
        je      ga_end
        inc     si
        cmp     cx,PATH_MAX - 1
        jae     ga_swarg
        mov     [di],al
        inc     di
        inc     cx
        jmp     ga_swarg
ga_end:
        mov     byte ptr [di],0
        pop     di
        ret

;---------------------------------------------------------------------
; Keyboard.
;
; get_key - one key, no echo, through 08h so that ^C still works.
; ask_yn  - SI -> a question; Y or N, echoed, in AL (upper case).
;---------------------------------------------------------------------
get_key:
        mov     ah,08h
        int     21h
        or      al,al
        jnz     gk_out
        mov     ah,08h                  ; an extended key: its second
        int     21h                     ; half, thrown away
        xor     al,al
gk_out:
        ret

ask_yn:
        call    puts
        call    ayn_key
        call    putc
        jmp     crlf

; ask_yn_err - the same, asked on standard error (the batch question)
ask_yn_err:
        call    puts_err
        call    ayn_key
        call    putc_err
        jmp     crlf_err

ayn_key:
        call    get_key
        call    upcase
        cmp     al,'Y'
        je      ayn_got
        cmp     al,'N'
        jne     ayn_key
ayn_got:
        ret

;---------------------------------------------------------------------
; Memory for a command's working space: one block at a time, freed by
; the command when it is done and by abandon when it is not.
;
; work_alloc - BX paragraphs.  CF (and "Insufficient memory") if not.
;---------------------------------------------------------------------
work_alloc:
        push    ax
        call    work_free
        mov     ah,48h
        int     21h
        jc      wa_fail
        mov     [work_seg],ax
        pop     ax
        clc
        ret
wa_fail:
        mov     si,OFFSET msg_nomem
        call    puts_err
        pop     ax
        stc
        ret

work_free:
        push    ax
        push    es
        mov     ax,[work_seg]
        or      ax,ax
        jz      wf_out
        mov     es,ax
        mov     ah,49h
        int     21h
        mov     word ptr [work_seg],0
wf_out:
        pop     es
        pop     ax
        ret

;=====================================================================
; The environment.  It lives in a block of its own, [env_seg], of
; [env_size] bytes: NAME=value strings, each ASCIIZ, and an empty one
; to end them.  Children are handed it by EXEC, which copies it.
;=====================================================================

;---------------------------------------------------------------------
; env_find - SI -> a name.  CF clear: found, ES:DI -> its entry and
;            ES:BX -> its value.  CF set: ES:DI -> the empty string at
;            the end.  ES is the environment's either way - callers put
;            it back.
;---------------------------------------------------------------------
env_find:
        push    ax
        push    si
        mov     es,[env_seg]
        xor     di,di
ef_entry:
        cmp     byte ptr es:[di],0
        je      ef_none
        mov     bx,di
        push    si
ef_cmp:
        mov     al,[si]
        or      al,al
        jz      ef_nameend
        call    upcase
        mov     ah,al
        mov     al,es:[bx]
        call    upcase
        cmp     al,ah
        jne     ef_diff
        inc     si
        inc     bx
        jmp     ef_cmp
ef_nameend:
        cmp     byte ptr es:[bx],'='
        jne     ef_diff
        pop     si
        inc     bx
        pop     si
        pop     ax
        clc
        ret
ef_diff:
        pop     si
ef_skip:
        cmp     byte ptr es:[di],0
        je      ef_skipped
        inc     di
        jmp     ef_skip
ef_skipped:
        inc     di
        jmp     ef_entry
ef_none:
        pop     si
        pop     ax
        stc
        ret

;---------------------------------------------------------------------
; env_set - SI -> "NAME=value" (NAME already upper case).  An empty
;           value removes NAME.  CF and "Out of environment space" if
;           it does not fit - in which case NAME keeps the value it had.
;---------------------------------------------------------------------
env_set:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es
        ; the name, on its own, for the search
        mov     di,OFFSET env_name
        mov     bx,si
es1_name:
        mov     al,[bx]
        or      al,al
        jz      es1_bad
        cmp     al,'='
        je      es1_named
        mov     [di],al
        inc     di
        inc     bx
        cmp     di,OFFSET env_name + PATH_MAX - 1
        jb      es1_name
es1_bad:
        jmp     esx_ok                  ; no '=': nothing to do
es1_named:
        mov     byte ptr [di],0
        mov     dx,bx                   ; DX -> the '='
        ; how much room is there, counting what NAME already holds?
        push    si
        mov     si,OFFSET env_name
        call    env_find
        pop     si
        mov     cx,0                    ; CX = bytes NAME frees
        jc      es1_nosize
        push    di
es1_old:
        inc     cx
        cmp     byte ptr es:[di],0
        je      es1_olddone
        inc     di
        jmp     es1_old
es1_olddone:
        pop     di
es1_nosize:
        call    env_end                 ; AX = offset of the final NUL
        mov     bx,dx
        cmp     byte ptr [bx+1],0       ; "NAME=": remove, nothing more
        je      es1_remove
        push    cx
        call    strlen                  ; CX = length of the new entry
        mov     bx,cx
        pop     cx
        inc     bx                      ; its NUL
        ; need: AX - CX (after removal) + BX + 1 (the final NUL) <= size
        push    ax
        sub     ax,cx
        add     ax,bx
        inc     ax
        cmp     ax,[env_size]
        pop     ax
        ja      esx_full
es1_remove:
        push    si
        mov     si,OFFSET env_name
        call    env_del
        pop     si
        mov     bx,dx
        cmp     byte ptr [bx+1],0
        je      esx_ok
        call    env_end
        mov     di,ax                   ; the new entry goes at the end
        mov     es,[env_seg]
es1_copy:
        lodsb
        stosb
        or      al,al
        jnz     es1_copy
        mov     byte ptr es:[di],0      ; and the end after it
esx_ok:
        clc
        jmp     short esx_out
esx_full:
        mov     si,OFFSET msg_envfull
        call    puts_err
        stc
esx_out:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

; env_end - AX = the offset of the environment's final (empty) string
env_end:
        push    es
        push    di
        mov     es,[env_seg]
        xor     di,di
ee_entry:
        cmp     byte ptr es:[di],0
        je      ee_out
ee_skip:
        inc     di
        cmp     byte ptr es:[di-1],0
        jne     ee_skip
        jmp     ee_entry
ee_out:
        mov     ax,di
        pop     di
        pop     es
        ret

; env_del - SI -> a name; remove it if it is there
env_del:
        push    ax
        push    bx
        push    cx
        push    si
        push    di
        push    ds
        push    es
        call    env_find
        jc      ed_out
        mov     si,di                   ; ES:SI -> after the entry
ed_skip:
        inc     si
        cmp     byte ptr es:[si-1],0
        jne     ed_skip
        call    env_end                 ; AX = the final NUL
        mov     cx,ax
        sub     cx,si
        inc     cx                      ; up to and including it
        push    es
        pop     ds
        rep     movsb
        ; zero what is left over, so the block never holds a ghost
        mov     cx,cs:[env_size]
        sub     cx,di
        jbe     ed_out
        xor     al,al
        rep     stosb
ed_out:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

        INCLUDE BUILTIN.INC
        INCLUDE BATFILE.INC
        INCLUDE LISTDIR.INC
        INCLUDE FILECOPY.INC

;=====================================================================
; The table of internal commands.  A name, its routine, where its /?
; text is in the file and how long, and whether it takes "CD.." forms.
;=====================================================================
int_table:
        dw      OFFSET cn_break, OFFSET cmd_break, hl_break - start, hl_break_end - hl_break, 0
        dw      OFFSET cn_call, OFFSET cmd_call, hl_call - start, hl_call_end - hl_call, 0
        dw      OFFSET cn_cd, OFFSET cmd_cd, hl_cd - start, hl_cd_end - hl_cd, ITF_PREFIX
        dw      OFFSET cn_chcp, OFFSET cmd_chcp, hl_chcp - start, hl_chcp_end - hl_chcp, 0
        dw      OFFSET cn_chdir, OFFSET cmd_cd, hl_cd - start, hl_cd_end - hl_cd, ITF_PREFIX
        dw      OFFSET cn_cls, OFFSET cmd_cls, hl_cls - start, hl_cls_end - hl_cls, 0
        dw      OFFSET cn_copy, OFFSET cmd_copy, hl_copy - start, hl_copy_end - hl_copy, 0
        dw      OFFSET cn_ctty, OFFSET cmd_ctty, hl_ctty - start, hl_ctty_end - hl_ctty, 0
        dw      OFFSET cn_date, OFFSET cmd_date, hl_date - start, hl_date_end - hl_date, 0
        dw      OFFSET cn_del, OFFSET cmd_del, hl_del - start, hl_del_end - hl_del, 0
        dw      OFFSET cn_dir, OFFSET cmd_dir, hl_dir - start, hl_dir_end - hl_dir, ITF_PREFIX
        dw      OFFSET cn_echo, OFFSET cmd_echo, hl_echo - start, hl_echo_end - hl_echo, ITF_PREFIX
        dw      OFFSET cn_erase, OFFSET cmd_del, hl_del - start, hl_del_end - hl_del, 0
        dw      OFFSET cn_exit, OFFSET cmd_exit, hl_exit - start, hl_exit_end - hl_exit, 0
        dw      OFFSET cn_for, OFFSET cmd_for, hl_for - start, hl_for_end - hl_for, 0
        dw      OFFSET cn_goto, OFFSET cmd_goto, hl_goto - start, hl_goto_end - hl_goto, 0
        dw      OFFSET cn_if, OFFSET cmd_if, hl_if - start, hl_if_end - hl_if, 0
        dw      OFFSET cn_lh, OFFSET cmd_lh, hl_lh - start, hl_lh_end - hl_lh, 0
        dw      OFFSET cn_loadhigh, OFFSET cmd_lh, hl_lh - start, hl_lh_end - hl_lh, 0
        dw      OFFSET cn_md, OFFSET cmd_md, hl_md - start, hl_md_end - hl_md, ITF_PREFIX
        dw      OFFSET cn_mkdir, OFFSET cmd_md, hl_md - start, hl_md_end - hl_md, ITF_PREFIX
        dw      OFFSET cn_path, OFFSET cmd_path, hl_path - start, hl_path_end - hl_path, 0
        dw      OFFSET cn_pause, OFFSET cmd_pause, hl_pause - start, hl_pause_end - hl_pause, 0
        dw      OFFSET cn_prompt, OFFSET cmd_prompt, hl_prompt - start, hl_prompt_end - hl_prompt, 0
        dw      OFFSET cn_rd, OFFSET cmd_rd, hl_rd - start, hl_rd_end - hl_rd, ITF_PREFIX
        dw      OFFSET cn_rem, OFFSET cmd_rem, hl_rem - start, hl_rem_end - hl_rem, 0
        dw      OFFSET cn_ren, OFFSET cmd_ren, hl_ren - start, hl_ren_end - hl_ren, 0
        dw      OFFSET cn_rename, OFFSET cmd_ren, hl_ren - start, hl_ren_end - hl_ren, 0
        dw      OFFSET cn_rmdir, OFFSET cmd_rd, hl_rd - start, hl_rd_end - hl_rd, ITF_PREFIX
        dw      OFFSET cn_set, OFFSET cmd_set, hl_set - start, hl_set_end - hl_set, 0
        dw      OFFSET cn_shift, OFFSET cmd_shift, hl_shift - start, hl_shift_end - hl_shift, 0
        dw      OFFSET cn_time, OFFSET cmd_time, hl_time - start, hl_time_end - hl_time, 0
        dw      OFFSET cn_truename, OFFSET cmd_truename, hl_truename - start, hl_truename_end - hl_truename, 0
        dw      OFFSET cn_type, OFFSET cmd_type, hl_type - start, hl_type_end - hl_type, 0
        dw      OFFSET cn_ver, OFFSET cmd_ver, hl_ver - start, hl_ver_end - hl_ver, 0
        dw      OFFSET cn_verify, OFFSET cmd_verify, hl_verify - start, hl_verify_end - hl_verify, 0
        dw      OFFSET cn_vol, OFFSET cmd_vol, hl_vol - start, hl_vol_end - hl_vol, 0
        dw      0

cn_break    db  'BREAK',0
cn_call     db  'CALL',0
cn_cd       db  'CD',0
cn_chcp     db  'CHCP',0
cn_chdir    db  'CHDIR',0
cn_cls      db  'CLS',0
cn_copy     db  'COPY',0
cn_ctty     db  'CTTY',0
cn_date     db  'DATE',0
cn_del      db  'DEL',0
cn_dir      db  'DIR',0
cn_echo     db  'ECHO',0
cn_erase    db  'ERASE',0
cn_exit     db  'EXIT',0
cn_for      db  'FOR',0
cn_goto     db  'GOTO',0
cn_if       db  'IF',0
cn_lh       db  'LH',0
cn_loadhigh db  'LOADHIGH',0
cn_md       db  'MD',0
cn_mkdir    db  'MKDIR',0
cn_path     db  'PATH',0
cn_pause    db  'PAUSE',0
cn_prompt   db  'PROMPT',0
cn_rd       db  'RD',0
cn_rem      db  'REM',0
cn_ren      db  'REN',0
cn_rename   db  'RENAME',0
cn_rmdir    db  'RMDIR',0
cn_set      db  'SET',0
cn_shift    db  'SHIFT',0
cn_time     db  'TIME',0
cn_truename db  'TRUENAME',0
cn_type     db  'TYPE',0
cn_ver      db  'VER',0
cn_verify   db  'VERIFY',0
cn_vol      db  'VOL',0

; the $ codes of PROMPT: a letter and its routine
prompt_codes:
        db      'P'
        dw      OFFSET pm_path
        db      'G'
        dw      OFFSET pm_gt
        db      'L'
        dw      OFFSET pm_lt
        db      'B'
        dw      OFFSET pm_bar
        db      'Q'
        dw      OFFSET pm_eq
        db      '$'
        dw      OFFSET pm_dol
        db      'T'
        dw      OFFSET pm_time
        db      'D'
        dw      OFFSET pm_date
        db      'V'
        dw      OFFSET pm_ver
        db      'N'
        dw      OFFSET pm_drive
        db      '_'
        dw      OFFSET pm_nl
        db      'E'
        dw      OFFSET pm_esc
        db      'H'
        dw      OFFSET pm_bs
        db      0

; the program extensions, in the order they are tried - five bytes each
ext_list    db  'COM',0,0
ext_exe     db  'EXE',0,0
ext_bat     db  'BAT',0,0
            db  0

;=====================================================================
; Messages - DOS 6.22's words wherever DOS 6.22 had them.
;=====================================================================
str_crlf    db  13,10,0
msg_badcmd  db  'Bad command or file name',13,10,0
msg_notfound db 'File not found',13,10,0
msg_baddrv  db  'Invalid drive specification',13,10,0
msg_denied  db  'Access denied',13,10,0
msg_toobig  db  'Program too big to fit in memory',13,10,0
msg_execfail db 'EXEC failure',13,10,0
msg_nocreate db 'File creation error',13,10,0
msg_pipefail db 'Intermediate file error during pipe',13,10,0
msg_nomem   db  'Insufficient memory',13,10,0
msg_envfull db  'Out of environment space',13,10,0
msg_termbat db  'Terminate batch job (Y/N)? ',0
msg_nohelp  db  'Help is not available',13,10,0
msg_syntax  db  'Syntax error',13,10,0
msg_reqparm db  'Required parameter missing',13,10,0
msg_badparm db  'Invalid parameter',13,10,0
msg_toomany db  'Too many parameters',13,10,0
msg_badsw   db  'Invalid switch - ',0

ce_msgs     dw  OFFSET ce_m00, OFFSET ce_m01, OFFSET ce_m02, OFFSET ce_m03
            dw  OFFSET ce_m04, OFFSET ce_m05, OFFSET ce_m06, OFFSET ce_m07
            dw  OFFSET ce_m08, OFFSET ce_m09, OFFSET ce_m0A, OFFSET ce_m0B
            dw  OFFSET ce_m0C, OFFSET ce_m0D, OFFSET ce_m0E, OFFSET ce_m0F
ce_m00      db  'Write protect error',0
ce_m01      db  'Invalid unit',0
ce_m02      db  'Not ready',0
ce_m03      db  'Invalid device request',0
ce_m04      db  'Data error',0
ce_m05      db  'Invalid device request parameters',0
ce_m06      db  'Seek error',0
ce_m07      db  'Invalid media type',0
ce_m08      db  'Sector not found',0
ce_m09      db  'Printer out of paper error',0
ce_m0A      db  'Write fault error',0
ce_m0B      db  'Read fault error',0
ce_m0C      db  'General failure',0
ce_m0D      db  'Sharing violation',0
ce_m0E      db  'Lock violation',0
ce_m0F      db  'Invalid disk change',0
ce_reading  db  ' reading',0
ce_writing  db  ' writing',0
ce_drvword  db  ' drive ',0
ce_device   db  ' device ',0
ce_abort    db  'Abort',0
ce_retry    db  ', Retry',0
ce_ignore   db  ', Ignore',0
ce_fail     db  ', Fail',0
ce_query    db  '?',0
ce_crlf     db  13,10,0

name_prompt db  'PROMPT',0
name_path   db  'PATH',0
name_temp   db  'TEMP',0
name_comspec db 'COMSPEC',0
name_dircmd db  'DIRCMD',0
name_copycmd db 'COPYCMD',0
dflt_prompt db  '$P$G',0
dev_con     db  'CON',0
pipe_stem   db  '%PIPE',0
pipe_ext    db  '.$$$',0

;=====================================================================
; State
;=====================================================================
main_sp     dw  0
env_seg     dw  0
env_size    dw  0
orig_parent dw  0
orig_term   dd  0
bat_depth   dw  0
rd_depth    dw  0
work_seg    dw  0
pipe_ctr    dw  0
permanent   db  0
single_mode db  SM_NONE
echo_on     db  1
errlevel    db  0
crit_fail   db  0
crit_abort  db  0
call_flag   db  0
for_active  db  0
cc_broken   db  0                       ; a child ended by ^C or Abort: the
                                        ; FOR or pipe it was part of stops
bat_gone    db  0
prog_isbat  db  0
pa_live     db  0
pb_live     db  0
pl_first    db  0
fp_hasext   db  0
rd_append   db  0
pn_triedroot db 0
fn_sep      db  1, 0                    ; a word: put_dec saves it whole

; the files our own commands have open, for abandon to close
open_hdls   LABEL WORD
h_src       dw  -1
h_dst       dw  -1
h_bat       dw  -1
h_misc      dw  -1

; the country: filled in by init from 38h, DOS's USA values until then
ctry_date   dw  0                       ; 0 m-d-y, 1 d-m-y, 2 y-m-d
ctry_thou   db  ',',0
ctry_dsep   db  '-',0
ctry_tsep   db  ':',0
ctry_dec    db  '.',0
ctry_24h    db  0

; INT 24h's
ce_flags    db  0
ce_drive    db  0
ce_code     dw  0
ce_devoff   dw  0
ce_devseg   dw  0
ce_jft      dd  0
ce_save01   dw  0

;=====================================================================
; Work areas.  Zero in the file; nothing in them survives a command
; except where a comment says so.
;=====================================================================
pc_byte     db  0
pce_buf     db  0, 0
rl_byte     db  0
args_ptr    dw  0
verb_start  dw  0
pl_rest     dw  0
pl_bar      dw  0
pl_this     dw  0
pn_dest     dw  0
pn_tail     dw  0
fp_name     dw  0
fp_extp     dw  0
fp_end      dw  0
fp_pathpos  dw  0
hl_pos      dw  0
hl_len      dw  0
exec_sp     dw  0
parm_block  LABEL WORD
pb_env      dw  0
pb_tail     dw  0, 0
pb_fcb1     dw  0, 0
pb_fcb2     dw  0, 0
rd_stack    dw  RD_MAX * 2 dup(0)
bat_segs    dw  BAT_MAX dup(0)
self_path   db  PATH_MAX dup(0)         ; this file - for /? (set by init)
dta         db  44 dup(0)
line_buf    db  LINE_MAX + 4 dup(0)
single_cmd  db  LINE_MAX + 2 dup(0)
seg_buf     db  EXP_MAX + 3 dup(0)
exp_line    db  EXP_MAX + 3 dup(0)
bat_raw     db  BATRAW_LEN + 2 dup(0)
bat_line    db  LINE_MAX + 2 dup(0)
bat_cur     db  BCO_LEN dup(0)
verb_buf    db  VERB_MAX + 2 dup(0)
prog_path   db  NAME_MAX dup(0)
fp_dir      db  PATH_MAX dup(0)
exec_tail   db  TAIL_MAX + 4 dup(0)
fcb1        db  37 dup(0)
fcb2        db  37 dup(0)
rd_inname   db  PATH_MAX dup(0)
rd_outname  db  PATH_MAX dup(0)
pa_name     db  NAME_MAX dup(0)
pb_name     db  NAME_MAX dup(0)
env_name    db  PATH_MAX dup(0)
pm_cwd      db  68 dup(0)
num_buf     db  20 dup(0)

;=====================================================================
; resident_end - everything above this line stays in memory.  The
; stack is the next STACK_SIZE bytes, laid over the help texts and the
; start-up code below, neither of which is wanted once it is running.
;=====================================================================
resident_end    LABEL BYTE

        INCLUDE HELPTEXT.INC

        INCLUDE INIT16.INC

init_stack  db  512 dup(0)
init_top    LABEL BYTE
image_end   LABEL BYTE

_TEXT   ENDS
        END     start
