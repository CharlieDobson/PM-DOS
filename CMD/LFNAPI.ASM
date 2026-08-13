;=====================================================================
; LFNAPI.ASM - LFNAPI.COM, the LFN API exerciser
;
; COMMAND.COM reaches the classic calls and the 714Eh find family in
; the course of ordinary work, but nothing in the shell speaks 7147h,
; 7160h, 716Ch, 43h or 7141h - so this external command does, one
; line of output per probe:
;
;   71A0h  volume information (the filesystem name and flags)
;   7147h  long current directory, from inside "Long Directory"
;   7160h  truename both ways: an alias to its long form and a long
;          name to its 8.3 form
;   716Ch  create a long-named file, write to it, reopen it by long
;          name and read the text back
;   43h    get attributes, set read-only, see it stick, clear it
;   7141h  delete the file by its long name, then prove it is gone
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_LFNAPI SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_LFNAPI, DS:_LFNAPI, ES:_LFNAPI, SS:_LFNAPI
        ORG     0

;=====================================================================
; D32 header - the file is the memory image
;=====================================================================
        D32_HEAD PROG_BASE,<OFFSET la_entry>,<0>,4096

;=====================================================================
la_entry:
;--- 71A0h volume information ----------------------------------------
        mov     esi,OFFSET m_a0
        call    print_asciiz
        mov     edi,OFFSET fsname
        mov     ecx,16
        mov     ax,71A0h
        int     21h
        jc      p_fail
        mov     esi,OFFSET fsname
        call    print_asciiz
        mov     esi,OFFSET m_flags
        call    print_asciiz
        mov     ax,bx
        call    phex16
        call    crlf_out

;--- 7147h long cwd, seen from inside a long-named directory ---------
; The directory is our own: 7139h makes it (proving that dispatch on
; the way), 3Bh steps in by the long name, and 713Ah removes it after.
        mov     esi,OFFSET m_47
        call    print_asciiz
        mov     edx,OFFSET dirname
        mov     ax,7139h                ; LFN MKDIR
        int     21h
        jc      p_fail
        mov     edx,OFFSET dirname      ; CHDIR by the long name
        mov     ah,3Bh
        int     21h
        jc      p_fail
        mov     esi,OFFSET pathbuf
        xor     dl,dl                   ; the default drive
        mov     ax,7147h
        int     21h
        jc      p_fail
        mov     esi,OFFSET pathbuf
        call    print_asciiz
        call    crlf_out
        mov     edx,OFFSET rootpath     ; back to the root, and the
        mov     ah,3Bh                  ; directory goes away again
        int     21h
        mov     edx,OFFSET dirname
        mov     ax,713Ah                ; LFN RMDIR
        int     21h

;--- 7160h truename: the alias renders long... -----------------------
        mov     esi,OFFSET m_60a
        call    print_asciiz
        mov     esi,OFFSET aliasname
        mov     edi,OFFSET pathbuf
        mov     cl,2                    ; the long form
        mov     ax,7160h
        int     21h
        jc      p_fail
        mov     esi,OFFSET pathbuf
        call    print_asciiz
        call    crlf_out

;--- ...and the long name renders 8.3 --------------------------------
        mov     esi,OFFSET m_60b
        call    print_asciiz
        mov     esi,OFFSET longname
        mov     edi,OFFSET pathbuf
        mov     cl,1                    ; the 8.3 form
        mov     ax,7160h
        int     21h
        jc      p_fail
        mov     esi,OFFSET pathbuf
        call    print_asciiz
        call    crlf_out

;--- 716Ch create a long-named file and write to it ------------------
        mov     esi,OFFSET m_6cc
        call    print_asciiz
        mov     esi,OFFSET apiname
        mov     ebx,2                   ; read/write
        mov     ecx,ATTR_ARCHIVE
        mov     edx,12h                 ; create it, truncate a leftover
        mov     ax,716Ch
        int     21h
        jc      p_fail
        movzx   ebx,ax                  ; the handle
        push    ecx                     ; the action taken
        mov     ecx,api_text_len
        mov     edx,OFFSET api_text
        mov     ah,40h
        int     21h
        mov     ah,3Eh
        int     21h
        mov     esi,OFFSET m_act
        call    print_asciiz
        pop     eax
        add     al,'0'
        call    putch
        call    crlf_out

;--- 716Ch reopen it by the long name and read it back ---------------
        mov     esi,OFFSET m_6co
        call    print_asciiz
        mov     esi,OFFSET apiname
        xor     ebx,ebx                 ; read
        xor     ecx,ecx
        mov     edx,1                   ; open-if-exists only
        mov     ax,716Ch
        int     21h
        jc      p_fail
        movzx   ebx,ax
        mov     ecx,79
        mov     edx,OFFSET pathbuf
        mov     ah,3Fh
        int     21h
        jc      p_fail
        mov     byte ptr [pathbuf+eax],0
        mov     ah,3Eh
        int     21h
        mov     esi,OFFSET pathbuf
        call    print_asciiz            ; api_text ends in CR LF itself

;--- 43h attributes: get, set read-only, get, clear ------------------
        mov     esi,OFFSET m_43
        call    print_asciiz
        mov     edx,OFFSET apiname      ; by the LONG name, of course
        xor     al,al
        mov     ah,43h
        int     21h
        jc      p_fail
        mov     ax,cx
        call    phex16
        mov     esi,OFFSET m_43r
        call    print_asciiz
        mov     ecx,ATTR_ARCHIVE OR ATTR_READONLY
        mov     al,1
        mov     ah,43h
        int     21h
        jc      p_fail
        xor     al,al                   ; read it back
        mov     ah,43h
        int     21h
        jc      p_fail
        mov     ax,cx
        call    phex16
        call    crlf_out
        mov     ecx,ATTR_ARCHIVE        ; clear it so the delete works
        mov     al,1
        mov     ah,43h
        int     21h

;--- 7141h delete by the long name, and prove it went ----------------
        mov     esi,OFFSET m_41
        call    print_asciiz
        mov     edx,OFFSET apiname
        xor     esi,esi
        mov     ax,7141h
        int     21h
        jc      p_fail
        mov     edx,OFFSET apiname      ; a reopen must now miss
        mov     ax,3D00h
        int     21h
        jnc     p_stillthere
        mov     esi,OFFSET m_gone
        call    print_asciiz

        mov     esi,OFFSET m_done
        call    print_asciiz
        xor     al,al
        mov     ah,4Ch
        int     21h

p_stillthere:
        mov     ah,3Eh                  ; tidy the handle it should
        int     21h                     ; never have produced
        mov     esi,OFFSET m_still
        call    print_asciiz
        mov     al,1
        mov     ah,4Ch
        int     21h

p_fail:
        push    eax
        mov     esi,OFFSET m_fail
        call    print_asciiz
        pop     eax
        call    phex16
        call    crlf_out
        mov     al,1
        mov     ah,4Ch
        int     21h

;=====================================================================
; Helpers
;=====================================================================
putch:
        push    eax
        push    edx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     edx
        pop     eax
        ret

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

phex16:                                 ; AX as 4 hex digits
        push    eax
        push    ecx
        push    edx
        movzx   edx,ax
        mov     ecx,4
ph_lp:
        rol     dx,4
        mov     al,dl
        and     al,0Fh
        cmp     al,10
        jb      ph_dig
        add     al,'A'-'0'-10
ph_dig:
        add     al,'0'
        call    putch
        loop    ph_lp
        pop     edx
        pop     ecx
        pop     eax
        ret

crlf_out:
        push    eax
        mov     al,13
        call    putch
        mov     al,10
        call    putch
        pop     eax
        ret

;=====================================================================
; Data
;=====================================================================
m_a0        db  '71A0 volume: fs=',0
m_flags     db  ' flags=',0
m_47        db  '7147 cwd:    ',0
m_60a       db  '7160 long:   ',0
m_60b       db  '7160 short:  ',0
m_6cc       db  '716C create: ',0
m_act       db  'ok action=',0
m_6co       db  '716C reopen: ',0
m_43        db  '43h  attrs:  ',0
m_43r       db  ' +R->',0
m_41        db  '7141 delete: ',0
m_gone      db  'gone',13,10,0
m_still     db  'STILL THERE',13,10,0
m_done      db  'LFNAPI done',13,10,0
m_fail      db  'FAIL ax=',0

dirname     db  'Api Long Directory',0
rootpath    db  '\',0
aliasname   db  'LONGFI~1.TXT',0
longname    db  'Long File Name.txt',0
apiname     db  'ApiCreatedLongName.txt',0
api_text    db  'written through 716Ch',13,10
api_text_len EQU $ - api_text

fsname      db  16 dup(0)
pathbuf     db  LFN_MAX + 4 dup(0)

_LFNAPI ENDS
        END
