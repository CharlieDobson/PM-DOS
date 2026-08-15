;=====================================================================
; CHOICE.ASM - CHOICE.COM, an EXTERNAL PM-DOS command
;
;   CHOICE [/C[:]keys] [/N] [/S] [/T[:]c,nn] [text]
;
;   /C[:]keys   the keys to accept (default YN)
;   /N          do not display the [Y,N]? list
;   /S          case sensitive
;   /T[:]c,nn   pick c automatically after nn seconds
;
; Exits with ERRORLEVEL set to the 1-based position of the key in the
; list, which is the whole point of it: this is how a batch file asks
; a question.
;
; External on purpose.  MS-DOS 6.22 ships CHOICE.COM rather than
; building it into COMMAND.COM, and a command that needs no privilege
; and no interpreter state has no business living in the shell.  It
; also makes this the first ordinary PM-DOS program: it is loaded into
; the transient slot by 21h/4Bh, reads its arguments from the PSP
; command tail, and terminates through 21h/4Ch like anything else.
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

KEYS_MAX        EQU     32
TEXT_MAX        EQU     128

_CHOICE SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_CHOICE, DS:_CHOICE, ES:_CHOICE, SS:_CHOICE
        ORG     0

;=====================================================================
; D32 header - the file is the memory image
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET ch_entry>,<0>,4096

;=====================================================================
; Entry
;=====================================================================
ch_entry:
;--- copy the PSP command tail out to a string we can walk -----------
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
        mov     edi,OFFSET tail
        cmp     ecx,TEXT_MAX - 1
        jbe     tl_ok
        mov     ecx,TEXT_MAX - 1
tl_ok:
        jecxz   tl_done
tl_cp:
        mov     al,[esi]
        cmp     al,0Dh                  ; the tail's own terminator
        je      tl_done
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     tl_cp
tl_done:
        mov     byte ptr [edi],0

;--- switches --------------------------------------------------------
        mov     esi,OFFSET tail
sw_lp:
        call    skip_blanks
        cmp     byte ptr [esi],'/'
        jne     sw_done
        inc     esi
        mov     al,[esi]
        call    upcase
        inc     esi
        cmp     al,'N'
        je      sw_n
        cmp     al,'S'
        je      sw_s
        cmp     al,'C'
        je      sw_c
        cmp     al,'T'
        je      sw_t
        jmp     sw_lp                   ; anything else: ignored
sw_n:
        mov     byte ptr [show_list],0
        jmp     sw_lp
sw_s:
        mov     byte ptr [case_sens],1
        jmp     sw_lp

sw_c:
        call    eat_colon
        mov     edi,OFFSET keys
        xor     ecx,ecx
sw_ckey:
        mov     al,[esi]
        or      al,al
        jz      sw_ckd
        cmp     al,' '
        je      sw_ckd
        cmp     al,9
        je      sw_ckd
        cmp     al,'/'
        je      sw_ckd
        cmp     ecx,KEYS_MAX - 1
        jae     sw_cksk
        mov     [edi],al
        inc     edi
        inc     ecx
sw_cksk:
        inc     esi
        jmp     short sw_ckey
sw_ckd:
        mov     byte ptr [edi],0
        jmp     sw_lp

sw_t:
        call    eat_colon
        mov     al,[esi]
        or      al,al
        jz      sw_lp
        mov     [tmo_key],al
        inc     esi
        cmp     byte ptr [esi],','
        jne     sw_tnum
        inc     esi
sw_tnum:
        call    parse_num
        jc      sw_lp
        mov     [tmo_secs],eax
        jmp     sw_lp

sw_done:
;--- the prompt ------------------------------------------------------
        call    print_asciiz            ; whatever is left of the tail
        cmp     byte ptr [show_list],0
        je      ask

        mov     al,'['
        call    putch
        mov     esi,OFFSET keys
li_lp:
        mov     al,[esi]
        or      al,al
        jz      li_done
        cmp     esi,OFFSET keys
        je      li_nocomma
        push    eax
        mov     al,','
        call    putch
        pop     eax
li_nocomma:
        call    putch
        inc     esi
        jmp     short li_lp
li_done:
        mov     al,']'
        call    putch
        mov     al,'?'
        call    putch

;--- wait for one of them --------------------------------------------
ask:
        cmp     dword ptr [tmo_secs],0
        je      ask_wait
        call    secs_now
        mov     [tmo_start],eax
ask_poll:
        mov     ah,06h                  ; direct console input, no wait
        mov     dl,0FFh
        int     21h
        jnz     ask_have
        call    secs_now
        sub     eax,[tmo_start]
        jns     ask_nowrap
        add     eax,86400               ; the clock went past midnight
ask_nowrap:
        cmp     eax,[tmo_secs]
        jb      ask_poll
        mov     al,[tmo_key]            ; timed out: take the default
        jmp     short ask_have
ask_wait:
        mov     ah,08h                  ; console input, no echo
        int     21h
        cmp     al,1Ah                  ; ^Z: stdin has run out
        je      ask_eof

ask_have:
        mov     [got],al
        mov     esi,OFFSET keys
        xor     ecx,ecx
mt_lp:
        mov     ah,[esi]
        or      ah,ah
        jz      ask                     ; not one of ours: ask again
        inc     ecx
        mov     al,[got]
        cmp     byte ptr [case_sens],0
        jne     mt_exact
        call    upcase
        xchg    al,ah
        call    upcase
        xchg    al,ah
mt_exact:
        cmp     al,ah
        je      mt_hit
        inc     esi
        jmp     short mt_lp
mt_hit:
        mov     al,[esi]                ; echo it as it was listed
        call    putch
        mov     edx,OFFSET crlf
        mov     ah,09h
        int     21h

        mov     al,cl                   ; ERRORLEVEL = which key
        mov     ah,4Ch
        int     21h                     ; no return

;--- stdin has run out -----------------------------------------------
; A redirected stdin that has reached its end reads as ^Z forever, so
; asking again would ask forever - and the loop above would never
; finish, because ^Z is not one of the choices and never will be.  The
; answer that is not coming is the default: /T's key if one was given,
; otherwise the first of the list.
;
; Found by "CHOICE < NUL", which only became possible to write in
; v0.30 - but the same hang was always there behind "CHOICE < " any
; empty file, waiting for somebody to type it.
ask_eof:
        mov     al,[tmo_key]
        or      al,al
        jnz     ask_have
        mov     al,[keys]
        jmp     short ask_have

;=====================================================================
; Helpers
;=====================================================================

; putch - write AL, preserving everything
putch:
        push    eax
        push    edx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     edx
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

; eat_colon - step past the optional ':' or '=' after a switch letter
eat_colon:
        cmp     byte ptr [esi],':'
        je      ec_eat
        cmp     byte ptr [esi],'='
        je      ec_eat
        ret
ec_eat:
        inc     esi
        ret

; parse_num - unsigned decimal at ESI -> EAX; ESI past it.  CF if none.
parse_num:
        push    ebx
        push    ecx
        xor     eax,eax
        xor     ebx,ebx
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
        or      ebx,ebx
        pop     ecx
        pop     ebx
        jz      pn_none
        clc
        ret
pn_none:
        stc
        ret

; secs_now - EAX = seconds since midnight
secs_now:
        push    ebx
        push    ecx
        push    edx
        mov     ah,2Ch
        int     21h
        movzx   eax,ch
        imul    eax,3600
        movzx   ebx,cl
        imul    ebx,60
        add     eax,ebx
        movzx   ebx,dh
        add     eax,ebx
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; Data
;=====================================================================
crlf        db  13,10,'$'
keys        db  'YN',0
            db  KEYS_MAX - 3 dup(0)
show_list   db  1
case_sens   db  0
tmo_key     db  0
got         db  0
tmo_secs    dd  0
tmo_start   dd  0
tail        db  TEXT_MAX dup(0)

_CHOICE ENDS
        END
