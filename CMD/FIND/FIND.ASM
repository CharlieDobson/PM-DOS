;=====================================================================
; FIND.ASM - FIND.EXE, the line searcher, an EXTERNAL command
;
;   FIND [/V] [/C] [/N] [/I] "string" [file ...]
;
; MS-DOS 6.22's FIND, behaviour for behaviour, because batch files
; test what it prints and the errorlevel it leaves:
;
;   - the switches may stand anywhere on the line, and apply to every
;     file; each is one letter, /N/I glued is two of them, /VCN is an
;     invalid switch
;   - the string is the first thing that is not a switch and it must
;     be in double quotes; "" inside it is one quote character.  A
;     name before it, or a second string, is "Parameter format not
;     correct"
;   - each file is searched as the parser reaches it, so a missing one
;     is reported between the output of its neighbours, and FIND goes
;     on to the next.  No wildcards: 6.22 opened the name as typed
;   - every file gets a heading: a blank line and "---------- NAME",
;     the name in capitals; with /C the count goes on the end of it.
;     Standard input gets no heading, and /C prints the bare number
;   - a line is what ends at a line feed.  It is matched without its
;     CR LF and written out exactly as it came, terminator and all; a
;     last line with no terminator is given CR LF.  ^Z ends the file.
;     An empty line contains nothing, so "" matches no line at all
;   - errorlevel 0 if any line contained the string (with /V too),
;     1 if none did, 2 for an error - but a match in a later file
;     sets it back to 0, as 6.22's did.  /? leaves 1
;   - errors go to standard error.  "FIND: " goes in front of one
;     only while no file has been named yet, which is 6.22's rule
;
; /I compares through the country's upper-case table, which 21h/65h
; fills in: one AX=6521h call over a 256-byte string of every byte
; value turns it into the table.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

BUF_SIZE        EQU     10000h          ; a line longer than this is
                                        ; taken in pieces
STR_MAX         EQU     128
NAME_MAX        EQU     260

SW_V            EQU     01h             ; /V  the lines without it
SW_C            EQU     02h             ; /C  a count only
SW_N            EQU     04h             ; /N  line numbers
SW_I            EQU     08h             ; /I  case does not matter

RC_FOUND        EQU     0
RC_NONE         EQU     1
RC_ERROR        EQU     2

; Errors go to standard error.  -dERR_TO_OUT builds a copy that puts
; them on standard output, for a test run whose shell has no 2>.
IFDEF ERR_TO_OUT
ERR_HDL         EQU     1
ELSE
ERR_HDL         EQU     2
ENDIF

_FIND   SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_FIND, DS:_FIND, ES:_FIND, SS:_FIND
        ORG     0

;=====================================================================
; D32 header - the input buffer lives in the BSS
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET fi_entry>,<BUF_SIZE + 4>,4096

;=====================================================================
fi_entry:
        call    up_init

;--- the tail, as a string we can walk -------------------------------
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
        mov     edi,OFFSET tail
        rep     movsb
        mov     byte ptr [edi],0
        mov     [tail_end],edi

;--- first pass: the switches, wherever they are ---------------------
        mov     esi,OFFSET tail
pp_lp:
        mov     al,[esi]
        or      al,al
        jz      pp_done
        cmp     al,'"'
        je      pp_quote
        cmp     al,'/'
        je      pp_switch
        inc     esi
        jmp     short pp_lp
pp_quote:                               ; a / inside a string is text
        inc     esi
pp_qlp:
        mov     al,[esi]
        or      al,al
        jz      pp_done                 ; unterminated: the second pass
        inc     esi                     ; will say so
        cmp     al,'"'
        jne     pp_qlp
        cmp     byte ptr [esi],'"'      ; "" is a quote inside it
        jne     pp_lp
        inc     esi
        jmp     short pp_qlp
pp_switch:
        mov     al,[esi+1]
        call    up_ascii
        mov     ah,[esi+2]              ; the switch must end here
        call    is_delim
        je      pp_known
        cmp     ah,'/'
        je      pp_known
        cmp     ah,'"'
        jne     pp_bad
pp_known:
        mov     bl,SW_V
        cmp     al,'V'
        je      pp_set
        mov     bl,SW_C
        cmp     al,'C'
        je      pp_set
        mov     bl,SW_N
        cmp     al,'N'
        je      pp_set
        mov     bl,SW_I
        cmp     al,'I'
        je      pp_set
        cmp     al,'?'
        je      fi_help
pp_bad:
        mov     edx,OFFSET msg_badsw
        jmp     parse_die
pp_set:
        or      [switches],bl
        add     esi,2
        jmp     short pp_lp
pp_done:

;--- second pass: the string, and each file as it comes --------------
        mov     esi,OFFSET tail
ps_lp:
        mov     ah,[esi]
        or      ah,ah
        jz      ps_end
        call    is_delim
        je      ps_skip
        cmp     ah,'/'
        je      ps_sw
        cmp     ah,'"'
        je      ps_string
        jmp     ps_file
ps_skip:
        inc     esi
        jmp     short ps_lp
ps_sw:
        add     esi,2                   ; checked by the first pass
        jmp     short ps_lp

ps_string:
        cmp     byte ptr [have_str],0
        jne     ps_format               ; a second string
        inc     esi
        mov     edi,OFFSET srch
        xor     ecx,ecx
ps_slp:
        mov     al,[esi]
        or      al,al
        jz      ps_format               ; no closing quote
        inc     esi
        cmp     al,'"'
        jne     ps_sput
        cmp     byte ptr [esi],'"'
        jne     ps_sdone
        inc     esi                     ; "" - one quote, kept
ps_sput:
        cmp     ecx,STR_MAX
        jae     ps_slp
        mov     [edi+ecx],al
        inc     ecx
        jmp     short ps_slp
ps_sdone:
        mov     [srch_len],ecx
        mov     byte ptr [have_str],1
        test    byte ptr [switches],SW_I
        jz      ps_lp
        mov     ebx,OFFSET up_tbl       ; the string, folded once
ps_fold:
        jecxz   ps_lp
        dec     ecx
        mov     al,[edi+ecx]
        xlat
        mov     [edi+ecx],al
        jmp     short ps_fold

ps_file:
        cmp     byte ptr [have_str],0
        je      ps_format               ; a name before the string
        mov     edi,OFFSET fname
        xor     ecx,ecx
ps_flp:
        mov     ah,[esi]
        or      ah,ah
        jz      ps_fend
        call    is_delim
        je      ps_fend
        cmp     ah,'"'
        je      ps_fend
        cmp     ah,'/'
        je      ps_fend
        mov     al,ah
        mov     ebx,OFFSET up_tbl       ; names are shown in capitals
        xlat
        cmp     ecx,NAME_MAX - 1
        jae     ps_fskip
        mov     [edi+ecx],al
        inc     ecx
ps_fskip:
        inc     esi
        jmp     short ps_flp
ps_fend:
        mov     byte ptr [edi+ecx],0
        mov     [fname_len],ecx
        mov     byte ptr [have_file],1
        push    esi
        call    do_file
        pop     esi
        jmp     ps_lp

ps_end:
        cmp     byte ptr [have_str],0
        je      ps_missing
        cmp     byte ptr [have_file],0
        jne     fi_exit
        xor     ebx,ebx                 ; no file: standard input
        call    do_handle
fi_exit:
        mov     al,[rc]
        mov     ah,4Ch
        int     21h

ps_missing:
        mov     edx,OFFSET msg_missing
        jmp     short parse_die
ps_format:
        mov     edx,OFFSET msg_format
parse_die:
        call    err_prefix
        call    err_puts
        mov     ax,4C00h + RC_ERROR
        int     21h

fi_help:
        mov     edx,OFFSET msg_usage
        call    out_puts
        mov     ax,4C00h + RC_NONE      ; 6.22 left its "no match" here
        int     21h

;=====================================================================
; do_file - search the file named in fname.
;=====================================================================
do_file:
        mov     edx,OFFSET fname
        mov     ax,3D00h
        int     21h
        jc      df_fail
        movzx   ebx,ax
        push    ebx
        call    do_handle
        pop     ebx
        mov     ah,3Eh
        int     21h
        ret
df_fail:
        mov     edx,OFFSET msg_nofile
        cmp     ax,ERR_ACCESS
        jne     df_say
        mov     edx,OFFSET msg_denied
df_say:
        call    err_puts
        mov     edx,OFFSET msg_dash
        call    err_puts
        mov     edx,OFFSET fname
        mov     ecx,[fname_len]
        mov     ebx,ERR_HDL
        mov     ah,40h
        int     21h
        mov     edx,OFFSET crlf_z
        call    err_puts
        mov     byte ptr [rc],RC_ERROR
        ret

;=====================================================================
; do_handle - EBX = an open handle (0 = standard input).  The heading,
;             the lines, the count.
;=====================================================================
do_handle:
        mov     [in_hdl],ebx
        mov     dword ptr [line_no],0
        mov     dword ptr [hits],0
        mov     dword ptr [buf_pos],0
        mov     dword ptr [buf_end],0
        mov     byte ptr [at_eof],0

        or      ebx,ebx
        jz      dh_noname
        mov     edx,OFFSET heading      ; CR LF and the dashes
        call    out_puts
        mov     edx,OFFSET fname
        mov     ecx,[fname_len]
        call    out_write
        test    byte ptr [switches],SW_C
        jnz     dh_noname
        mov     edx,OFFSET crlf_z
        call    out_puts
dh_noname:

dh_line:
        call    next_line               ; ESI = line, ECX = its length,
        jc      dh_done                 ; EDX = length without CR LF,
                                        ; AL = 1 if it had a terminator
        mov     [ln_ptr],esi
        mov     [ln_len],ecx
        mov     [ln_term],al
        inc     dword ptr [line_no]

        mov     ecx,edx                 ; what is matched
        call    match                   ; CF = it contains the string
        jnc     dh_nomatch
        mov     byte ptr [rc],RC_FOUND
        test    byte ptr [switches],SW_V
        jnz     dh_line
        jmp     short dh_show
dh_nomatch:
        test    byte ptr [switches],SW_V
        jz      dh_line
dh_show:
        test    byte ptr [switches],SW_C
        jz      dh_print
        inc     dword ptr [hits]
        jmp     short dh_line
dh_print:
        test    byte ptr [switches],SW_N
        jz      dh_text
        mov     dl,'['
        call    out_char
        mov     eax,[line_no]
        call    out_dec
        mov     dl,']'
        call    out_char
dh_text:
        mov     edx,[ln_ptr]
        mov     ecx,[ln_len]
        call    out_write
        cmp     byte ptr [ln_term],0
        jne     dh_line
        mov     edx,OFFSET crlf_z       ; a line the file did not end
        call    out_puts
        jmp     dh_line

dh_done:
        test    byte ptr [switches],SW_C
        jz      dh_out
        cmp     dword ptr [in_hdl],0
        je      dh_count
        mov     edx,OFFSET colon
        call    out_puts
dh_count:
        mov     eax,[hits]
        call    out_dec
        mov     edx,OFFSET crlf_z
        call    out_puts
dh_out:
        ret

;=====================================================================
; next_line - the next line of [in_hdl].
;             CF set at the end of the input.  Otherwise ESI -> the
;             line, ECX = its length as it is to be written, EDX =
;             the length that is matched (no LF, and no CR before it),
;             AL = 1 if it ended in a line feed.
;
; The buffer holds [buf_pos, buf_end).  A line that is not all there
; is moved to the front and the rest read in behind it; a line longer
; than the whole buffer is handed over in buffer-sized pieces, each
; one written with a CR LF of its own, which is what 6.22 did with a
; line longer than its 4K.
;=====================================================================
next_line:
        mov     esi,OFFSET buf
        add     esi,[buf_pos]
        mov     ecx,[buf_end]
        sub     ecx,[buf_pos]
        jz      nl_more
        mov     edi,esi                 ; a line feed in what is here?
        mov     al,10
        mov     edx,ecx
        repne   scasb
        jne     nl_nolf
        mov     ecx,edi
        sub     ecx,esi                 ; the line, LF included
        add     [buf_pos],ecx
        mov     edx,ecx
        dec     edx                     ; not the LF...
        jz      nl_lf
        cmp     byte ptr [esi+edx-1],13
        jne     nl_lf
        dec     edx                     ; ...nor one CR before it
nl_lf:
        mov     al,1
        clc
        ret
nl_nolf:
        mov     ecx,edx                 ; the scan used ECX up
        cmp     byte ptr [at_eof],0
        jne     nl_last
        cmp     dword ptr [buf_pos],0
        jne     nl_more
        cmp     dword ptr [buf_end],BUF_SIZE
        jb      nl_more
        mov     dword ptr [buf_pos],BUF_SIZE    ; a piece of a long line
        mov     ecx,BUF_SIZE
        mov     edx,ecx
        xor     al,al
        clc
        ret
nl_last:                                ; the end, with no LF to end it
        mov     eax,[buf_end]
        mov     [buf_pos],eax
        mov     edx,ecx
        xor     al,al
        clc
        ret

nl_more:
        cmp     byte ptr [at_eof],0
        jne     nl_eof
        ; slide what is left to the front, then fill behind it
        mov     esi,OFFSET buf
        add     esi,[buf_pos]
        mov     ecx,[buf_end]
        sub     ecx,[buf_pos]
        mov     edi,OFFSET buf
        mov     [buf_end],ecx
        mov     dword ptr [buf_pos],0
        rep     movsb
        mov     edx,OFFSET buf
        add     edx,[buf_end]
        mov     ecx,BUF_SIZE
        sub     ecx,[buf_end]
        mov     ebx,[in_hdl]
        mov     ah,3Fh
        int     21h
        jc      nl_hiteof               ; a read error ends it quietly
        or      eax,eax
        jz      nl_hiteof
        ; ^Z ends the file: cut the buffer there and read no more
        mov     edi,OFFSET buf
        add     edi,[buf_end]
        mov     ecx,eax
        add     [buf_end],eax
        mov     al,1Ah
        repne   scasb
        jne     next_line
        dec     edi
        sub     edi,OFFSET buf
        mov     [buf_end],edi
nl_hiteof:
        mov     byte ptr [at_eof],1
        jmp     next_line
nl_eof:
        stc
        ret

;=====================================================================
; match - ESI -> a line, ECX = its length.  CF if the string is in it.
;         An empty string is in nothing.
;=====================================================================
match:
        pushad
        mov     edx,[srch_len]
        or      edx,edx
        jz      mt_no
        sub     ecx,edx
        jb      mt_no
        inc     ecx                     ; ECX = places it could start
        mov     ebx,OFFSET up_tbl
mt_start:
        xor     edi,edi
mt_cmp:
        mov     al,[esi+edi]
        test    byte ptr [switches],SW_I
        jz      mt_case
        xlat
mt_case:
        cmp     al,[srch+edi]
        jne     mt_next
        inc     edi
        cmp     edi,edx
        jb      mt_cmp
        popad
        stc
        ret
mt_next:
        inc     esi
        dec     ecx
        jnz     mt_start
mt_no:
        popad
        clc
        ret

;=====================================================================
; up_init - up_tbl[n] = n in capitals, by the country's rules.  If the
;           kernel will not say, A-Z is still right.
;=====================================================================
up_init:
        mov     edi,OFFSET up_tbl
        xor     eax,eax
ui_lp:
        mov     [edi+eax],al
        inc     al
        jnz     ui_lp
        mov     edx,edi
        mov     ecx,256
        mov     ax,6521h
        int     21h
        jnc     ui_out
        mov     edi,OFFSET up_tbl + 'a'
        mov     ecx,26
ui_az:
        sub     byte ptr [edi],20h
        inc     edi
        loop    ui_az
ui_out:
        ret

; up_ascii - AL lower -> upper, a-z only (switch letters)
up_ascii:
        cmp     al,'a'
        jb      ua_out
        cmp     al,'z'
        ja      ua_out
        sub     al,20h
ua_out:
        ret

; is_delim - ZF if AH is one of the parser's separators, or the end
is_delim:
        cmp     ah,' '
        je      id_out
        cmp     ah,9
        je      id_out
        cmp     ah,','
        je      id_out
        cmp     ah,';'
        je      id_out
        cmp     ah,'='
        je      id_out
        or      ah,ah
id_out:
        ret

;=====================================================================
; Output.  Matches go to handle 1, errors to handle 2.
;=====================================================================
; err_prefix - "FIND: ", until a file has been named
err_prefix:
        cmp     byte ptr [have_file],0
        jne     ep_out
        push    edx
        mov     edx,OFFSET msg_find
        call    err_puts
        pop     edx
ep_out:
        ret

; err_puts / out_puts - the zero-terminated string at EDX
err_puts:
        push    ebx
        mov     ebx,ERR_HDL
        jmp     short pz_go
out_puts:
        push    ebx
        mov     ebx,1
pz_go:
        push    eax
        push    ecx
        push    edi
        mov     edi,edx
        xor     al,al
        mov     ecx,-1
        repne   scasb
        not     ecx
        dec     ecx
        mov     ah,40h
        int     21h
        pop     edi
        pop     ecx
        pop     eax
        pop     ebx
        ret

; out_write - ECX bytes at EDX
out_write:
        pushad
        jecxz   ow_out
        mov     ebx,1
        mov     ah,40h
        int     21h
ow_out:
        popad
        ret

; out_char - the byte in DL
out_char:
        pushad
        mov     [one_char],dl
        mov     edx,OFFSET one_char
        mov     ecx,1
        call    out_write
        popad
        ret

; out_dec - EAX in decimal
out_dec:
        pushad
        mov     edi,OFFSET dec_buf + 11
        mov     byte ptr [edi],0
        mov     ebx,10
od_div:
        xor     edx,edx
        div     ebx
        dec     edi
        add     dl,'0'
        mov     [edi],dl
        or      eax,eax
        jnz     od_div
        mov     edx,edi
        call    out_puts
        popad
        ret

;=====================================================================
; Data
;=====================================================================
rc          db  RC_NONE
switches    db  0
have_str    db  0
have_file   db  0
at_eof      db  0
ln_term     db  0
one_char    db  0,0
in_hdl      dd  0
line_no     dd  0
hits        dd  0
buf_pos     dd  0
buf_end     dd  0
ln_ptr      dd  0
ln_len      dd  0
srch_len    dd  0
fname_len   dd  0
tail_end    dd  0

heading     db  13,10,'---------- ',0
colon       db  ': ',0
crlf_z      db  13,10,0
msg_find    db  'FIND: ',0
msg_dash    db  ' - ',0
msg_nofile  db  'File not found',0
msg_denied  db  'Access denied',0
msg_badsw   db  'Invalid switch',13,10,0
msg_missing db  'Required parameter missing',13,10,0
msg_format  db  'Parameter format not correct',13,10,0
msg_usage   db  'Searches files for lines that contain a given string.',13,10,13,10
            db  'FIND [/V] [/C] [/N] [/I] "string" [[drive:][path]filename[ ...]]',13,10,13,10
            db  '  /V        Shows the lines that do NOT contain the string.',13,10
            db  '  /C        Shows only how many lines contain the string.',13,10
            db  '  /N        Puts each line',39,'s number in front of it.',13,10
            db  '  /I        Treats capitals and small letters as the same.',13,10
            db  '  "string"  The text to look for.  "" stands for a quote in it.',13,10
            db  '  [drive:][path]filename',13,10
            db  '            One or more files to search.',13,10,13,10
            db  'With no file named, FIND searches what is typed at the keyboard',13,10
            db  'or what another command sends it through a pipe.',13,10,0

dec_buf     db  12 dup(0)
up_tbl      db  256 dup(0)
srch        db  STR_MAX dup(0)
fname       db  NAME_MAX dup(0)
tail        db  130 dup(0)

img_end:                                ; the BSS begins here
buf         LABEL BYTE                  ; BUF_SIZE bytes of input

_FIND   ENDS
        END
