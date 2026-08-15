;=====================================================================
; MORE.ASM - MORE.COM, the classic pager, an EXTERNAL PM-DOS command
;
;   MORE            page standard input:  TYPE F | MORE,  MORE < F
;   MORE file       page the named file (a convenience the real one
;                   never had; quotes around a long name are fine)
;
; Reads through handle 0 unless a filename argument says otherwise,
; writes through handle 1, and pauses every 24 lines with the classic
; "-- More --".  The keystroke that continues comes from CON, opened
; directly - standard input is the very thing being paged, so the
; keyboard has to be reached AROUND it.  That is what the real MORE
; did too, and it is why redirection can feed it at all.
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

INBUF_SIZE      EQU     2048
PAGE_ROWS       EQU     24
PAGE_COLS       EQU     80
FNAME_MAX       EQU     80

_MORE   SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_MORE, DS:_MORE, ES:_MORE, SS:_MORE
        ORG     0

;=====================================================================
; D32 header - the file is the memory image
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET mo_entry>,<0>,4096

;=====================================================================
mo_entry:
;--- an optional filename from the PSP tail --------------------------
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
mo_skipb:
        or      ecx,ecx
        jz      mo_noname
        cmp     byte ptr [esi],' '
        je      mo_skipstep
        cmp     byte ptr [esi],9
        jne     mo_name
mo_skipstep:
        inc     esi
        dec     ecx
        jmp     short mo_skipb
mo_name:
        cmp     byte ptr [esi],0Dh
        je      mo_noname
        xor     ah,ah                   ; AH = saw a leading quote
        cmp     byte ptr [esi],'"'
        jne     mo_ncp
        inc     ah
        inc     esi
        dec     ecx
mo_ncp:
        mov     edi,OFFSET fname
        xor     edx,edx
mo_nlp:
        or      ecx,ecx
        jz      mo_nend
        mov     al,[esi]
        cmp     al,0Dh
        je      mo_nend
        cmp     al,'"'
        je      mo_nend                 ; closing quote ends the name
        or      ah,ah
        jnz     mo_nkeep                ; quoted: blanks belong to it
        cmp     al,' '
        je      mo_nend
        cmp     al,9
        je      mo_nend
mo_nkeep:
        cmp     edx,FNAME_MAX - 1
        jae     mo_nskip
        mov     [edi],al
        inc     edi
        inc     edx
mo_nskip:
        inc     esi
        dec     ecx
        jmp     short mo_nlp
mo_nend:
        mov     byte ptr [edi],0
        or      edx,edx
        jz      mo_noname

        mov     edx,OFFSET fname        ; input = the named file
        mov     ax,3D00h
        int     21h
        jc      mo_cant
        movzx   eax,ax
        mov     [in_hdl],eax
mo_noname:

;--- CON, for the keystroke that turns the page ----------------------
        mov     edx,OFFSET con_name
        mov     ax,3D00h
        int     21h
        jc      mo_pump                 ; no CON?  page without pausing
        movzx   eax,ax
        mov     [key_hdl],eax

;--- the pump --------------------------------------------------------
mo_pump:
        mov     ebx,[in_hdl]
        mov     ecx,INBUF_SIZE
        mov     edx,OFFSET inbuf
        mov     ah,3Fh
        int     21h
        jc      mo_done
        or      eax,eax
        jz      mo_done
        mov     ecx,eax
        mov     esi,OFFSET inbuf
mo_ch:
        mov     al,[esi]
        cmp     al,1Ah                  ; ^Z ends a text file
        je      mo_done
        call    mo_put
        inc     esi
        dec     ecx
        jnz     mo_ch
        jmp     short mo_pump

mo_done:
        mov     ax,4C00h
        int     21h

mo_cant:
        mov     edx,OFFSET msg_nofile
        mov     ah,09h
        int     21h
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; mo_put - one character to stdout, pausing FIRST when the previous
;          output filled the page (so a file ending exactly on a page
;          boundary never asks for a key it does not need).
;---------------------------------------------------------------------
mo_put:
        push    eax
        push    ebx
        push    ecx
        push    edx

        ; The character goes into out_ch BEFORE the pause, because the
        ; pause is an INT 21h round trip and AL does not survive it -
        ; which used to eat whichever character the page boundary
        ; happened to land on and print the byte count in its place.
        mov     [out_ch],al
        cmp     dword ptr [rows],PAGE_ROWS
        jb      mp_write
        call    mo_pause

mp_write:
        mov     ebx,1
        mov     ecx,1
        mov     edx,OFFSET out_ch
        mov     ah,40h
        int     21h

        mov     al,[out_ch]             ; row and column bookkeeping
        cmp     al,13
        jne     mp_notcr
        mov     dword ptr [cols],0
        jmp     short mp_out
mp_notcr:
        cmp     al,10
        jne     mp_notlf
        inc     dword ptr [rows]
        mov     dword ptr [cols],0
        jmp     short mp_out
mp_notlf:
        inc     dword ptr [cols]
        cmp     dword ptr [cols],PAGE_COLS
        jb      mp_out
        inc     dword ptr [rows]        ; the console wrapped for us
        mov     dword ptr [cols],0
mp_out:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; mo_pause - "-- More --", one key from CON, erase, carry on
;---------------------------------------------------------------------
mo_pause:
        pushad                          ; nothing of the caller's survives
        mov     dword ptr [rows],0      ; an INT 21h, least of all AL
        cmp     dword ptr [key_hdl],-1
        je      mps_out                 ; keyless: just keep rolling

        mov     ebx,[key_hdl]
        mov     ecx,more_len
        mov     edx,OFFSET msg_more
        mov     ah,40h                  ; the prompt, straight to CON
        int     21h

        mov     ebx,[key_hdl]
        mov     ecx,1
        mov     edx,OFFSET key_ch       ; its OWN buffer: sharing out_ch
        mov     ah,3Fh                  ; with the pending character was
        int     21h                     ; the other half of the same bug

        mov     ebx,[key_hdl]
        mov     ecx,wipe_len
        mov     edx,OFFSET msg_wipe     ; CR, blanks, CR: the prompt
        mov     ah,40h                  ; (and the echo) vanish
        int     21h
mps_out:
        popad
        ret

;=====================================================================
; Data
;=====================================================================
in_hdl      dd  0                       ; stdin unless a file was named
key_hdl     dd  -1
rows        dd  0
cols        dd  0
out_ch      db  0                       ; the character being written
key_ch      db  0                       ; the keystroke that turns a page

con_name    db  'CON',0
fname       db  FNAME_MAX dup(0)
msg_nofile  db  'File not found',13,10,'$'
msg_more    db  '-- More --'
more_len    EQU $ - msg_more
msg_wipe    db  13,'            ',13
wipe_len    EQU $ - msg_wipe

inbuf       db  INBUF_SIZE dup(0)

_MORE   ENDS
        END
