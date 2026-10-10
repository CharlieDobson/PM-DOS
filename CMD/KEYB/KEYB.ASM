;=====================================================================
; KEYB.ASM - KEYB.COM, the keyboard's layout
;
;   KEYB                        which layout is in use
;   KEYB GR                     the German keyboard, in the code page
;                               in use
;   KEYB GR,850                 ...in code page 850
;   KEYB GR,,C:\KEYBOARD.SYS    ...out of that file
;   KEYB IT,,/ID:142            the second Italian keyboard
;   KEYB US                     the built-in keyboard again
;
; MS-DOS's KEYB STAYED IN MEMORY, on INT 09h, with the tables it had
; read from KEYBOARD.SYS.  This one finds the layout in KEYBOARD.SYS
; and hands it to the kernel, which owns the keyboard and decodes with
; it from then on (21h/F1h AL=22h; IO\KEYBD.INC), and then it ends.
; Nothing of it is left behind, so there is nothing to load high and
; nothing for INSTALL= to do: KEYB goes in AUTOEXEC.BAT.
;
; KEYBOARD.SYS IS PM-DOS'S OWN (TOOLS\MKNLS.PS1 writes it from
; NLS\KEYBOARD.DEF): a list of layout, keyboard ID and code page, and
; behind each a record that is passed to the kernel as it stands - KBL_
; in PMDOS.INC says what is in one.  MS-DOS's KEYBOARD.SYS is a
; different file and is refused by its first bytes.
;
; The file is looked for where the command interpreter is - COMSPEC's
; directory, which is where the system's files are - and then in the
; root of the drive the system started from.
;
; /E is taken and does nothing: it told MS-DOS's KEYB that the
; keyboard had 101 keys, which this kernel's driver takes for granted.
;
; Says nothing when it works, as MS-DOS's did not.  ERRORLEVEL 1 is a
; bad command line or a layout or code page the file has not got, 2 a
; file that is missing or not a KEYBOARD.SYS.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

KSYS_HDR        EQU     10              ; 'PMKEYB',1Ah,1 and a count
KSYS_ENT        EQU     12              ; an entry: two letters, ID, page,
KSE_CODE        EQU     0               ; length, where
KSE_ID          EQU     2
KSE_PAGE        EQU     4
KSE_LEN         EQU     6
KSE_AT          EQU     8
KSYS_MAXENT     EQU     2000            ; more than this is not a list
PATH_MAX        EQU     128

_KB     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_KB, DS:_KB, ES:_KB, SS:_KB
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET kb_entry>,<0>,4096

kb_entry:
;--- the command tail, kept as a string with a zero on the end --------
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
        mov     edi,OFFSET tail
ke_copy:
        or      ecx,ecx
        jz      ke_copied
        mov     al,[esi]
        cmp     al,0Dh
        je      ke_copied
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jmp     ke_copy
ke_copied:
        mov     byte ptr [edi],0

;--- xx[,[page][,file]] and the switches, in any order ----------------
        mov     esi,OFFSET tail
kp_next:
        call    skip_blanks
        mov     al,[esi]
        or      al,al
        jz      kp_done
        cmp     al,'/'
        je      kp_switch
        cmp     byte ptr [have_arg],0
        jne     kb_badparm              ; two layouts on one line
        mov     byte ptr [have_arg],1

        call    get_letter              ; the two letters
        jc      kb_badparm
        mov     byte ptr [want_code],al
        call    get_letter
        jc      kb_badparm
        mov     byte ptr [want_code+1],al
        call    skip_blanks
        cmp     byte ptr [esi],','
        jne     kp_argend
        inc     esi
        call    skip_blanks
        cmp     byte ptr [esi],','
        je      kp_file
        call    get_num                 ; the code page, if there is one
        jc      kp_argend
        or      eax,eax
        jz      kb_badparm
        mov     dword ptr [want_page],eax
        call    skip_blanks
        cmp     byte ptr [esi],','
        jne     kp_argend
kp_file:
        inc     esi
        call    skip_blanks
        mov     edi,OFFSET file_path    ; the file, to a blank or a switch
        xor     ecx,ecx
kp_name:
        mov     al,[esi]
        or      al,al
        jz      kp_named
        cmp     al,' '
        je      kp_named
        cmp     al,9
        je      kp_named
        cmp     al,'/'
        je      kp_named
        cmp     ecx,PATH_MAX - 1
        jae     kb_badparm
        mov     [edi+ecx],al
        inc     ecx
        inc     esi
        jmp     kp_name
kp_named:
        mov     byte ptr [edi+ecx],0
        or      ecx,ecx
        jz      kp_next
        mov     byte ptr [file_given],1
        jmp     kp_next
kp_argend:
        mov     al,[esi]                ; what follows the layout is a
        or      al,al                   ; blank, a switch or the end
        jz      kp_next
        cmp     al,'/'
        je      kp_next
        cmp     al,' '
        je      kp_next
        cmp     al,9
        je      kp_next
        jmp     kb_badparm

kp_switch:
        inc     esi
        mov     al,[esi]
        call    upcase
        cmp     al,'?'
        je      kb_help
        cmp     al,'E'
        je      kp_sw_e
        cmp     al,'I'
        jne     kb_badparm
        mov     al,[esi+1]
        call    upcase
        cmp     al,'D'
        jne     kb_badparm
        cmp     byte ptr [esi+2],':'
        jne     kb_badparm
        add     esi,3
        call    get_num
        jc      kb_badparm
        mov     dword ptr [want_id],eax
        jmp     kp_swend
kp_sw_e:
        inc     esi
kp_swend:
        mov     al,[esi]
        or      al,al
        jz      kp_next
        cmp     al,' '
        je      kp_next
        cmp     al,9
        je      kp_next
        cmp     al,'/'
        je      kp_next
        jmp     kb_badparm

kp_done:
        cmp     byte ptr [have_arg],0
        je      kb_report

;--- KEYB US: the keyboard the kernel has built in --------------------
        cmp     word ptr [want_code],5355h      ; 'U','S'
        jne     kb_load
        cmp     dword ptr [want_id],0
        jne     kb_load
        mov     eax,0F122h + (KYF_OFF SHL 16)
        int     21h
        jc      kb_nokernel
        mov     ax,4C00h
        int     21h

;--- the code page: the one named, or the one in use ------------------
kb_load:
        cmp     dword ptr [want_page],0
        jne     kl_file
        mov     dword ptr [want_page],CTRY_CP437
        mov     eax,6601h
        int     21h
        jc      kl_file
        movzx   eax,bx
        mov     dword ptr [want_page],eax

;--- the file: the one named, or KEYBOARD.SYS beside the interpreter,
;--- or in the root of the drive the system started from --------------
kl_file:
        cmp     byte ptr [file_given],0
        jne     kl_open
        call    path_comspec
        mov     edx,OFFSET file_path
        mov     eax,3D00h
        int     21h
        jnc     kl_opened
        call    path_boot
kl_open:
        mov     edx,OFFSET file_path
        mov     eax,3D00h
        int     21h
        jc      kb_nofile
kl_opened:
        movzx   eax,ax
        mov     dword ptr [hnd],eax

        mov     ecx,KSYS_HDR
        mov     edx,OFFSET hdr
        call    read_n
        jc      kb_badfile
        cmp     dword ptr [hdr],454B4D50h       ; 'PMKE'
        jne     kb_badfile
        cmp     dword ptr [hdr+4],011A4259h     ; 'YB',1Ah,1
        jne     kb_badfile
        movzx   ebx,word ptr [hdr+8]            ; how many records
        cmp     ebx,KSYS_MAXENT
        ja      kb_badfile

;--- the first record with the two letters, the ID if one was asked
;--- for, and the code page ---------------------------------------------
kl_entry:
        or      ebx,ebx
        jz      kl_none
        mov     ecx,KSYS_ENT
        mov     edx,OFFSET ent
        call    read_n
        jc      kb_badfile
        dec     ebx
        mov     ax,word ptr [ent+KSE_CODE]
        cmp     ax,word ptr [want_code]
        jne     kl_entry
        mov     eax,dword ptr [want_id]
        or      eax,eax
        jz      kl_idok
        cmp     ax,word ptr [ent+KSE_ID]
        jne     kl_entry
        jmp     kl_page
kl_idok:
        ; with no ID asked for, the first one in the file is the
        ; country's own keyboard: a later ID is another keyboard
        cmp     byte ptr [code_seen],0
        je      kl_first
        mov     ax,word ptr [ent+KSE_ID]
        cmp     ax,word ptr [first_id]
        jne     kl_entry
        jmp     kl_page
kl_first:
        mov     ax,word ptr [ent+KSE_ID]
        mov     word ptr [first_id],ax
kl_page:
        mov     byte ptr [code_seen],1
        mov     eax,dword ptr [want_page]
        cmp     ax,word ptr [ent+KSE_PAGE]
        jne     kl_entry

        movzx   ecx,word ptr [ent+KSE_LEN]
        cmp     ecx,KBL_KEYS
        jb      kb_badfile
        cmp     ecx,KBL_MAX
        ja      kb_badfile
        mov     dword ptr [rec_len],ecx
        mov     ebx,dword ptr [hnd]
        mov     ecx,dword ptr [ent+KSE_AT]      ; 42h takes the whole offset
        mov     eax,4200h                       ; in ECX
        int     21h
        jc      kb_badfile
        mov     ecx,dword ptr [rec_len]
        mov     edx,OFFSET rec
        call    read_n
        jc      kb_badfile
        call    close_file

        mov     edx,OFFSET rec
        mov     ecx,dword ptr [rec_len]
        mov     eax,0F122h + (KYF_SET SHL 16)
        int     21h
        jnc     kl_set
        cmp     ax,ERR_BADDATA
        je      kb_badrec
        jmp     kb_nokernel
kl_set:
        mov     ax,4C00h
        int     21h

;--- the file has no such layout, or not in that code page ------------
kl_none:
        call    close_file
        cmp     byte ptr [code_seen],0
        jne     kl_nopage
        mov     edx,OFFSET msg_nolay1
        call    puts
        call    put_code
        cmp     dword ptr [want_id],0
        je      kl_noid
        mov     edx,OFFSET msg_withid
        call    puts
        mov     eax,dword ptr [want_id]
        call    pdec
kl_noid:
        mov     edx,OFFSET msg_in
        call    puts
        call    put_path
        call    put_crlf
        mov     ax,4C01h
        int     21h
kl_nopage:
        mov     edx,OFFSET msg_nopage1
        call    puts
        call    put_code
        mov     edx,OFFSET msg_nopage2
        call    puts
        mov     eax,dword ptr [want_page]
        call    pdec
        call    put_crlf
        mov     ax,4C01h
        int     21h

;=====================================================================
; KEYB alone: what the keyboard is using
;=====================================================================
kb_report:
        mov     edi,OFFSET rec
        mov     eax,0F122h + (KYF_GET SHL 16)
        int     21h
        jc      kb_nokernel
        mov     ebx,eax
        mov     edx,OFFSET msg_layout
        call    puts
        or      ebx,ebx
        jnz     kr_loaded
        mov     edx,OFFSET msg_builtin
        call    puts
        mov     ax,4C00h
        int     21h
kr_loaded:
        mov     ax,word ptr [rec+KBL_CODE]
        mov     word ptr [want_code],ax
        call    put_code
        movzx   eax,word ptr [rec+KBL_ID]
        or      eax,eax
        jz      kr_page
        push    eax
        mov     edx,OFFSET msg_id
        call    puts
        pop     eax
        call    pdec
kr_page:
        mov     edx,OFFSET msg_page
        call    puts
        movzx   eax,word ptr [rec+KBL_PAGE]
        call    pdec
        call    put_crlf
        cmp     ebx,1
        jne     kr_out
        mov     edx,OFFSET msg_usnow
        call    puts
kr_out:
        mov     ax,4C00h
        int     21h

;=====================================================================
; The ways out with something wrong
;=====================================================================
kb_help:
        mov     edx,OFFSET msg_usage
        call    puts
        mov     ax,4C00h
        int     21h
kb_badparm:
        mov     edx,OFFSET msg_badparm
        call    puts
        mov     ax,4C01h
        int     21h
kb_nofile:
        mov     edx,OFFSET msg_nofile
        call    puts
        call    put_path
        call    put_crlf
        mov     ax,4C02h
        int     21h
kb_badfile:
        call    close_file
kb_badrec:
        call    put_path
        mov     edx,OFFSET msg_badfile
        call    puts
        mov     ax,4C02h
        int     21h
kb_nokernel:
        mov     edx,OFFSET msg_kernel
        call    puts
        mov     ax,4C02h
        int     21h

;=====================================================================
; Helpers
;=====================================================================

; read_n - ECX bytes from the file to EDX.  CF if there are not that
;          many.  EBX is kept.
read_n:
        push    ebx
        push    ecx
        mov     ebx,dword ptr [hnd]
        mov     eax,3F00h
        int     21h
        pop     ecx
        pop     ebx
        jc      rn_out
        cmp     eax,ecx
        je      rn_out                  ; CF is clear
        stc
rn_out:
        ret

close_file:
        push    eax
        push    ebx
        mov     ebx,dword ptr [hnd]
        mov     eax,3E00h
        int     21h
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; path_comspec - file_path = COMSPEC's directory and KEYBOARD.SYS, or
;                the bare name when there is no COMSPEC to read.
;---------------------------------------------------------------------
path_comspec:
        mov     edi,OFFSET file_path
        xor     edx,edx                 ; EDX = length of the directory part
        ; ds: is load-bearing wherever a PSP field is read - see
        ; inherit_env in COMMAND.ASM
        mov     esi,ds:[SLOT_PSP + PSP_ENV]
        or      esi,esi
        jz      pc_bare
pc_var:
        cmp     byte ptr [esi],0
        je      pc_bare                 ; the end of the block
        push    esi
        mov     ecx,OFFSET s_comspec
pc_cmp:
        mov     al,[ecx]
        or      al,al
        jz      pc_hit
        cmp     al,[esi]
        jne     pc_nextvar
        inc     esi
        inc     ecx
        jmp     pc_cmp
pc_nextvar:
        pop     esi
pc_skip:
        mov     al,[esi]
        inc     esi
        or      al,al
        jnz     pc_skip
        jmp     pc_var
pc_hit:
        add     esp,4                   ; ESI is past the '=' already
        xor     ecx,ecx
pc_scan:
        mov     al,[esi+ecx]
        or      al,al
        jz      pc_cut
        cmp     ecx,PATH_MAX - 16
        jae     pc_cut
        mov     [edi+ecx],al
        inc     ecx
        cmp     al,'\'
        je      pc_mark
        cmp     al,':'
        jne     pc_scan
pc_mark:
        mov     edx,ecx                 ; keep everything up to here
        jmp     pc_scan
pc_cut:
        add     edi,edx
pc_bare:
        mov     esi,OFFSET s_keyboard
pc_name:
        lodsb
        stosb
        or      al,al
        jnz     pc_name
        ret

;---------------------------------------------------------------------
; path_boot - file_path = \KEYBOARD.SYS on the drive the system
;             started from.
;---------------------------------------------------------------------
path_boot:
        mov     eax,3305h
        int     21h                     ; DL = the boot drive, 1 = A:
        add     dl,'A' - 1
        mov     edi,OFFSET file_path
        mov     [edi],dl
        mov     byte ptr [edi+1],':'
        mov     byte ptr [edi+2],'\'
        add     edi,3
        jmp     pc_bare

skip_blanks:
        cmp     byte ptr [esi],' '
        je      sb_step
        cmp     byte ptr [esi],9
        jne     sb_out
sb_step:
        inc     esi
        jmp     skip_blanks
sb_out:
        ret

; get_letter - the letter at ESI, upper case, in AL, and ESI past it.
;              CF if it is not a letter.
get_letter:
        mov     al,[esi]
        call    upcase
        cmp     al,'A'
        jb      gl_bad
        cmp     al,'Z'
        ja      gl_bad
        inc     esi
        clc
        ret
gl_bad:
        stc
        ret

; get_num - the decimal number at ESI in EAX, and ESI past it.  CF if
;           there is no digit there, or five of them are not enough.
get_num:
        push    ecx
        push    edx
        xor     eax,eax
        xor     ecx,ecx
gn_lp:
        movzx   edx,byte ptr [esi]
        sub     edx,'0'
        cmp     edx,9
        ja      gn_end
        cmp     ecx,5
        jae     gn_bad
        imul    eax,10
        add     eax,edx
        inc     esi
        inc     ecx
        jmp     gn_lp
gn_end:
        or      ecx,ecx
        jz      gn_bad
        pop     edx
        pop     ecx
        clc
        ret
gn_bad:
        pop     edx
        pop     ecx
        stc
        ret

upcase:
        cmp     al,'a'
        jb      uc_out
        cmp     al,'z'
        ja      uc_out
        sub     al,20h
uc_out:
        ret

puts:                                   ; EDX -> text ending in '$'
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

put_crlf:
        push    edx
        mov     edx,OFFSET msg_crlf
        call    puts
        pop     edx
        ret

put_code:                               ; the two letters asked for
        push    eax
        push    edx
        mov     dl,byte ptr [want_code]
        mov     ah,02h
        int     21h
        mov     dl,byte ptr [want_code+1]
        mov     ah,02h
        int     21h
        pop     edx
        pop     eax
        ret

put_path:                               ; file_path, which ends in a zero
        push    eax
        push    edx
        push    esi
        mov     esi,OFFSET file_path
pp_lp:
        mov     dl,[esi]
        or      dl,dl
        jz      pp_out
        mov     ah,02h
        int     21h
        inc     esi
        jmp     pp_lp
pp_out:
        pop     esi
        pop     edx
        pop     eax
        ret

pdec:                                   ; EAX in decimal
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ebx,10
        xor     ecx,ecx
pd_div:
        xor     edx,edx
        div     ebx
        push    edx
        inc     ecx
        or      eax,eax
        jnz     pd_div
pd_out:
        pop     edx
        add     dl,'0'
        mov     ah,02h
        int     21h
        dec     ecx
        jnz     pd_out
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; Data
;=====================================================================
s_comspec   db  'COMSPEC=',0
s_keyboard  db  'KEYBOARD.SYS',0

msg_crlf    db  13,10,'$'
msg_badparm db  'Invalid parameter',13,10,'$'
msg_nofile  db  'Cannot open $'
msg_badfile db  ' is not a file of keyboard layouts',13,10,'$'
msg_nolay1  db  'No keyboard layout $'
msg_withid  db  ' with ID $'
msg_in      db  ' in $'
msg_nopage1 db  'Keyboard layout $'
msg_nopage2 db  ' has no table for code page $'
msg_kernel  db  'This version of PM-DOS cannot load a keyboard layout.',13,10,'$'
msg_layout  db  'Keyboard layout: $'
msg_builtin db  'US (built in)',13,10,'$'
msg_id      db  '   ID: $'
msg_page    db  '   Code page: $'
msg_usnow   db  'The US keyboard is in use. Ctrl+Alt+F2 switches to the layout.',13,10,'$'
msg_usage   db  'Loads a keyboard layout.',13,10,13,10
            db  'KEYB [xx[,[page][,file]]] [/ID:nnn]',13,10,13,10
            db  '  xx       The two letters of the layout: GR, FR, UK and so on.',13,10
            db  '  page     The code page. Without one, the code page in use.',13,10
            db  '  file     The file of layouts. Without one, KEYBOARD.SYS.',13,10
            db  '  /ID:nnn  Which keyboard, for a country that has two.',13,10,13,10
            db  'KEYB US loads the built-in US keyboard again.',13,10
            db  'KEYB without parameters shows the layout in use.',13,10,13,10
            db  'Ctrl+Alt+F1 switches to the US keyboard, and Ctrl+Alt+F2 back to',13,10
            db  'the layout.',13,10,'$'

have_arg    db  0
file_given  db  0
code_seen   db  0
            db  0
want_code   db  0,0                     ; the two letters
first_id    dw  0
want_page   dd  0
want_id     dd  0
hnd         dd  0
rec_len     dd  0
hdr         db  KSYS_HDR dup(0)
            db  0,0
ent         db  KSYS_ENT dup(0)
tail        db  132 dup(0)
file_path   db  PATH_MAX dup(0)
rec         db  KBL_MAX dup(0)

_KB     ENDS
        END
