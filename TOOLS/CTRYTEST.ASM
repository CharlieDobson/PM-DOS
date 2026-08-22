;=====================================================================
; CTRYTEST.ASM - CTRYTEST.COM, the country block a program formats its
;                numbers with.
;
; MS-DOS 6.22's MEM.EXE prints every figure it has with only the first
; group of digits - "1K" where it means 1,048,576 - and the monitor's
; call trace says why it might: MEM calls 21h/38h once per number it
; is about to print, and the only thing in that block a number
; formatter wants is the THOUSANDS SEPARATOR.  A separator that comes
; back wrong, or comes back as a NUL, ends the string it is inserted
; into - and the digits after it are never printed.
;
; So this asks the same question the same way and looks at the answer,
; because "the block is written by rep movsb from a table that is
; right in the source" is not the same statement as "a 16-bit program
; reads a comma out of offset 7".  Every field is dumped as well as
; judged: a test that says FAILED without showing what arrived leaves
; the next question unanswered.
;
; 65h/01h is asked the same thing, because it is the same block with a
; seven-byte header on the front and a program may use either.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_CT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_CT, DS:_CT, ES:_CT, SS:_CT
        ORG     0

; Offsets into the country block, as DOS lays it out
C_DATEFMT   EQU     00h
C_CURRENCY  EQU     02h
C_THOUSANDS EQU     07h
C_DECIMAL   EQU     09h
C_DATESEP   EQU     0Bh
C_TIMESEP   EQU     0Dh
C_CURRFMT   EQU     0Fh
C_CURRDIG   EQU     10h
C_TIMEFMT   EQU     11h
C_CASEMAP   EQU     12h
C_LISTSEP   EQU     16h
C_SIZE      EQU     22h

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; 1 - 38h answers at all.  A country DOS has no table for is refused,
;     and a refusal here means every following check is about a buffer
;     nothing wrote.
;=====================================================================
        mov     dx,OFFSET t_call
        call    puts
        mov     di,OFFSET ctry
        mov     cx,C_SIZE               ; poison it, so a call that
        mov     al,0FEh                 ; writes nothing cannot pass by
ct_fill:                                ; leaving zeros that look like
        mov     [di],al                 ; an answer
        inc     di
        loop    ct_fill

        mov     ah,38h
        mov     al,0                    ; the current country
        mov     dx,OFFSET ctry
        push    ds
        pop     es
        int     21h
        jc      ct_c_no
        mov     [ctrycode],bx
        call    ok
        jmp     short ct_dump
ct_c_no:
        call    put4                    ; AX is the error
        call    bad
        jmp     ct_done

;=====================================================================
; the block itself, byte by byte, before any of it is judged
;=====================================================================
ct_dump:
        mov     dx,OFFSET msg_raw
        call    puts
        mov     si,OFFSET ctry
        mov     cx,C_SIZE
ct_dlp:
        lodsb
        call    put2
        mov     dl,' '
        call    putc
        dec     cx
        jnz     ct_dlp
        mov     dx,OFFSET msg_crlf
        call    puts

;=====================================================================
; 2 - THE ONE THE BUG IS ABOUT.  A thousands separator is a string:
;     one character and a terminating zero.  Either byte wrong and a
;     formatter either prints nothing between the groups or stops
;     there.
;=====================================================================
        mov     dx,OFFSET t_thou
        call    puts
        mov     al,[ctry+C_THOUSANDS]
        call    putq
        cmp     al,','
        jne     ct_t_no
        cmp     byte ptr [ctry+C_THOUSANDS+1],0
        jne     ct_t_no
        call    ok
        jmp     short ct_dec
ct_t_no:
        call    bad

;=====================================================================
; 3 - and the other separators, which are the same kind of string and
;     would fail the same way
;=====================================================================
ct_dec:
        mov     dx,OFFSET t_sep
        call    puts
        mov     al,[ctry+C_DECIMAL]
        call    putq
        mov     al,[ctry+C_DATESEP]
        call    putq
        mov     al,[ctry+C_TIMESEP]
        call    putq
        mov     al,[ctry+C_LISTSEP]
        call    putq
        cmp     byte ptr [ctry+C_DECIMAL],'.'
        jne     ct_s_no
        cmp     byte ptr [ctry+C_DECIMAL+1],0
        jne     ct_s_no
        cmp     byte ptr [ctry+C_DATESEP],'-'
        jne     ct_s_no
        cmp     byte ptr [ctry+C_TIMESEP],':'
        jne     ct_s_no
        cmp     byte ptr [ctry+C_LISTSEP],','
        jne     ct_s_no
        call    ok
        jmp     short ct_code
ct_s_no:
        call    bad

;=====================================================================
; 4 - the country code came back in BX
;=====================================================================
ct_code:
        mov     dx,OFFSET t_code
        call    puts
        mov     ax,[ctrycode]
        call    put4
        cmp     ax,1
        jne     ct_k_no
        call    ok
        jmp     short ct_map
ct_k_no:
        call    bad

;=====================================================================
; 5 - the case map is a FAR CALL, and the only way to find out whether
;     the address is real is to make it.  A pointer that is merely
;     non-zero has been accepted by every test that never called it.
;=====================================================================
ct_map:
        mov     dx,OFFSET t_map
        call    puts
        mov     ax,word ptr [ctry+C_CASEMAP+2]
        call    put4
        mov     dl,':'
        call    putc
        mov     ax,word ptr [ctry+C_CASEMAP]
        call    put4
        mov     ax,word ptr [ctry+C_CASEMAP]
        or      ax,word ptr [ctry+C_CASEMAP+2]
        jz      ct_m_no
        mov     al,'a'
        call    dword ptr [ctry+C_CASEMAP]
        push    ax
        mov     dl,' '
        call    putc
        pop     ax
        push    ax
        mov     dl,al
        call    putc
        pop     ax
        cmp     al,'A'                  ; DOS's own map leaves ASCII
        je      ct_m_ok                 ; alone; either answer is legal
        cmp     al,'a'                  ; here, a CRASH is not
        jne     ct_m_no
ct_m_ok:
        call    ok
        jmp     short ct_ext
ct_m_no:
        call    bad

;=====================================================================
; 6 - 65h/01h, the same block with a header, since a program may ask
;     either way and they must not disagree
;=====================================================================
ct_ext:
        mov     dx,OFFSET t_ext
        call    puts
        mov     di,OFFSET ext
        mov     cx,C_SIZE + 7
        mov     al,0FEh
ct_efill:
        mov     [di],al
        inc     di
        loop    ct_efill

        mov     ax,6501h
        mov     bx,0FFFFh               ; the current codepage
        mov     dx,0FFFFh               ; ...and the current country
        mov     cx,C_SIZE + 7
        mov     di,OFFSET ext
        push    ds
        pop     es
        int     21h
        jc      ct_e_no
        mov     al,[ext]
        call    put2
        cmp     al,1                    ; info id 1
        jne     ct_e_no
        mov     ax,word ptr [ext+3]
        cmp     ax,1                    ; country
        jne     ct_e_no
        mov     ax,word ptr [ext+5]
        cmp     ax,437                  ; codepage
        jne     ct_e_no
        mov     al,[ext+7+C_THOUSANDS]  ; and the separator again, in
        call    putq                    ; the copy behind the header
        cmp     al,','
        jne     ct_e_no
        cmp     byte ptr [ext+7+C_THOUSANDS+1],0
        jne     ct_e_no
        call    ok
        jmp     short ct_done
ct_e_no:
        call    bad

ct_done:
        mov     dx,OFFSET msg_bye
        call    puts
        mov     al,[fails]
        or      al,al
        jz      ct_exit
        push    ax
        mov     dx,OFFSET msg_fails
        call    puts
        pop     ax
        call    put2
        mov     dx,OFFSET msg_crlf
        call    puts
ct_exit:
        mov     al,[fails]
        mov     ah,4Ch
        int     21h

;=====================================================================
; helpers
;=====================================================================
ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     dx
        ret

; putq - the character in AL as 'x' if it is printable and as a hex
;        pair if it is not, because the interesting values here are
;        exactly the ones with no glyph
putq:
        push    ax
        push    dx
        mov     dl,''''
        call    putc
        pop     dx
        pop     ax
        push    ax
        cmp     al,20h
        jb      putq_hex
        cmp     al,7Eh
        ja      putq_hex
        mov     dl,al
        call    putc
        jmp     short putq_end
putq_hex:
        mov     dl,'\'
        call    putc
        pop     ax
        push    ax
        call    put2
putq_end:
        push    dx
        mov     dl,''''
        call    putc
        mov     dl,' '
        call    putc
        pop     dx
        pop     ax
        ret

puts:
        push    ax
        push    ds
        push    cs
        pop     ds
        mov     ah,09h
        int     21h
        pop     ds
        pop     ax
        ret
putc:
        push    ax
        push    dx
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret
put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:
        push    ax
        shr     al,4
        call    put1
        pop     ax
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        push    dx
        mov     dl,al
        call    putc
        pop     dx
        ret

;=====================================================================
; data
;=====================================================================
ctrycode    dw  0
fails       db  0
ctry        db  C_SIZE dup(0)
ext         db  C_SIZE + 7 dup(0)

msg_hi      db  13,10,'CTRYTEST - the country block a number is formatted with',13,10,13,10,'$'
msg_bye     db  13,10,'CTRYTEST done.',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_fails   db  'checks failed: $'
msg_raw     db  '      $'

t_call      db  '  1 38h answers                       $'
t_thou      db  '  2 thousands separator is a string   $'
t_sep       db  '  3 decimal, date, time, list         $'
t_code      db  '  4 and the country code in BX        $'
t_map       db  '  5 the case map can be CALLED        $'
t_ext       db  '  6 65h/01h says the same thing       $'

_CT     ENDS
        END     start
