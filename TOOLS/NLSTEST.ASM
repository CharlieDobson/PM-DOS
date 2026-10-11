;=====================================================================
; NLSTEST.ASM - NLSTEST.COM, the country as a program is told it.
;
; Asks 38h, 65h and 66h everything COUNTRY= in CONFIG.SYS changes the
; answer to, and prints the answers.  It judges nothing: what is right
; depends on the country, so what it prints is kept as a baseline for
; each - the United States with no COUNTRY= line, and one run for each
; COUNTRY= line tried.  CTRYTEST and CTRYDMP are the two that judge the
; country the kernel has built in.
;
; Three of the questions are about countries that are NOT the one in
; use - 38h for Germany, 6501h for France in code page 850 - which the
; kernel answers out of COUNTRY.SYS, and one is about a country no
; file has.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_NT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_NT, DS:_NT, ES:_NT, SS:_NT
        ORG     0

start:
        push    ds
        pop     es
        cld

;--- 38h: the country in use -------------------------------------------
        mov     dx,OFFSET t_38
        call    puts
        mov     dx,OFFSET blk
        mov     ax,3800h
        xor     bx,bx
        int     21h
        call    show_cf_ax_bx
        mov     si,OFFSET blk
        mov     cx,24
        call    dump

;--- 38h: Germany, by AL, and a country nobody has, by BX --------------
        mov     dx,OFFSET t_38de
        call    puts
        call    wipe
        mov     dx,OFFSET blk
        mov     ax,3831h                ; 49
        xor     bx,bx
        int     21h
        call    show_cf_ax_bx
        mov     si,OFFSET blk
        mov     cx,24
        call    dump

        mov     dx,OFFSET t_38no
        call    puts
        mov     dx,OFFSET blk
        mov     ax,38FFh
        mov     bx,998
        int     21h
        call    show_cf_ax_bx
        call    crlf

;--- 6501h: the one in use, then France in 850, then into ten bytes ----
        mov     dx,OFFSET t_6501
        call    puts
        call    wipe
        mov     ax,6501h
        mov     bx,0FFFFh
        mov     dx,0FFFFh
        mov     cx,41
        mov     di,OFFSET blk
        int     21h
        call    show_cf_cx
        mov     si,OFFSET blk
        mov     cx,25
        call    dump

        mov     dx,OFFSET t_6501fr
        call    puts
        call    wipe
        mov     ax,6501h
        mov     bx,850
        mov     dx,33
        mov     cx,41
        mov     di,OFFSET blk
        int     21h
        call    show_cf_cx
        mov     si,OFFSET blk
        mov     cx,25
        call    dump

        mov     dx,OFFSET t_6501sm
        call    puts
        call    wipe
        mov     ax,6501h
        mov     bx,0FFFFh
        mov     dx,0FFFFh
        mov     cx,10
        mov     di,OFFSET blk
        int     21h
        call    show_cf_cx
        mov     si,OFFSET blk
        mov     cx,12                   ; two past what was asked for
        call    dump

;--- 6502h and 6506h: the tables, by what is in them --------------------
        mov     dx,OFFSET t_6502
        call    puts
        mov     al,02h
        mov     cx,128
        call    table
        mov     dx,OFFSET t_6506
        call    puts
        mov     al,06h
        mov     cx,256
        call    table

;--- 6520h, and the far call in the country block: one answer? ----------
        mov     dx,OFFSET t_6520
        call    puts
        mov     si,OFFSET chars
c20_lp:
        lodsb
        or      al,al
        jz      c20_done
        mov     dl,al
        mov     ax,6520h
        int     21h
        mov     al,dl
        call    put2
        mov     dl,' '
        call    putc
        jmp     short c20_lp
c20_done:
        call    crlf

        mov     dx,OFFSET t_map
        call    puts
        mov     dx,OFFSET blk
        mov     ax,3800h
        int     21h
        mov     si,OFFSET chars
map_lp:
        lodsb
        or      al,al
        jz      map_done
        call    dword ptr [blk+12h]
        call    put2
        mov     dl,' '
        call    putc
        jmp     short map_lp
map_done:
        call    crlf

;--- 6521h: a string ------------------------------------------------------
        mov     dx,OFFSET t_6521
        call    puts
        mov     dx,OFFSET sample
        mov     cx,SAMPLELEN
        mov     ax,6521h
        int     21h
        mov     si,OFFSET sample
        mov     cx,SAMPLELEN
        call    dump

;--- 6523h: yes, no, neither ---------------------------------------------
        mov     dx,OFFSET t_6523
        call    puts
        mov     si,OFFSET yn
yn_lp:
        lodsb
        or      al,al
        jz      yn_done
        mov     dl,al
        call    putc
        mov     ax,6523h
        int     21h
        mov     dl,'='
        call    putc
        call    put1
        mov     dl,' '
        call    putc
        jmp     short yn_lp
yn_done:
        call    crlf

;--- 6601h -----------------------------------------------------------------
        mov     dx,OFFSET t_6601
        call    puts
        mov     ax,6601h
        xor     bx,bx
        xor     dx,dx
        int     21h
        push    dx
        mov     ax,bx
        call    putdec
        mov     dl,'/'
        call    putc
        pop     ax
        call    putdec
        call    crlf

        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; table - AL = 02h or 06h, CX = how long the table should be: where
;         65h says it is, its length word, a sum of its bytes, and the
;         sixteen that are 80h to 8Fh's.
;---------------------------------------------------------------------
table:
        push    cx
        mov     ah,65h
        mov     bx,0FFFFh
        mov     dx,0FFFFh
        mov     cx,5
        mov     di,OFFSET blk
        int     21h
        pop     cx
        jnc     tb_ok
        mov     dx,OFFSET t_refused
        call    puts
        ret
tb_ok:
        push    es
        les     bx,dword ptr [blk+1]
        mov     ax,es:[bx]              ; the length it says
        call    put4
        mov     dl,' '
        call    putc
        xor     dx,dx
        mov     si,2
tb_sum:
        xor     ax,ax
        mov     al,es:[bx+si]
        add     dx,ax
        rol     dx,1
        inc     si
        loop    tb_sum
        mov     ax,dx
        call    put4
        mov     dl,' '
        call    putc
        mov     cx,16
        mov     si,2
        cmp     word ptr es:[bx],256    ; the weights start at 00h, the
        jne     tb_row                  ; capitals at 80h
        mov     si,2 + 80h
tb_row:
        mov     al,es:[bx+si]
        call    put2
        mov     dl,' '
        call    putc
        inc     si
        loop    tb_row
        pop     es
        call    crlf
        ret

show_cf_ax_bx:
        pushf
        push    bx
        push    ax
        mov     dl,'C'
        jc      sc_cf
        mov     dl,'-'
sc_cf:
        call    putc
        mov     dl,' '
        call    putc
        pop     ax
        call    put4
        mov     dl,' '
        call    putc
        pop     ax
        call    put4
        mov     dl,' '
        call    putc
        popf
        ret

show_cf_cx:
        pushf
        push    cx
        mov     dl,'C'
        jc      sx_cf
        mov     dl,'-'
sx_cf:
        call    putc
        mov     dl,' '
        call    putc
        pop     ax
        call    put4
        mov     dl,' '
        call    putc
        popf
        ret

wipe:                                   ; the buffer, so "not written" shows
        push    ax
        push    cx
        push    di
        mov     di,OFFSET blk
        mov     cx,48
        mov     al,0FEh
        rep     stosb
        pop     di
        pop     cx
        pop     ax
        ret

dump:                                   ; CX bytes at SI, and a new line
        lodsb
        call    put2
        mov     dl,' '
        call    putc
        loop    dump
        call    crlf
        ret

puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret
putc:
        push    ax
        mov     ah,02h
        int     21h
        pop     ax
        ret
crlf:
        push    dx
        mov     dl,13
        call    putc
        mov     dl,10
        call    putc
        pop     dx
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
put1:
        push    ax
        push    dx
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1_out
        add     al,7
p1_out:
        mov     dl,al
        call    putc
        pop     dx
        pop     ax
        ret
putdec:                                 ; AX in decimal
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx,10
        xor     cx,cx
pd_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     pd_div
pd_out:
        pop     dx
        add     dl,'0'
        call    putc
        loop    pd_out
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

t_38        db  '38h, the country in use     $'
t_38de      db  '38h, country 49             $'
t_38no      db  '38h, country 998            $'
t_6501      db  '6501h, in use               $'
t_6501fr    db  '6501h, country 33, page 850 $'
t_6501sm    db  '6501h, into ten bytes       $'
t_6502      db  '6502h: length, sum, 80h-8Fh $'
t_6506      db  '6506h: length, sum, 80h-8Fh $'
t_6520      db  '6520h on 61 7A 81 82 84 87 94 A4 E1: $'
t_map       db  'the case-map call on the same:       $'
t_6521      db  '6521h on a string           $'
t_6523      db  '6523h: $'
t_6601      db  '6601h, in use/at startup    $'
t_refused   db  'refused',13,10,'$'
chars       db  61h,7Ah,81h,82h,84h,87h,94h,0A4h,0E1h,0
yn          db  'yYnNjJoOsSkKeEtTx',0
sample         db  'pm-dos',81h,84h,94h,82h
SAMPLELEN      EQU $ - sample
blk         db  48 dup(0FEh)

_NT     ENDS
        END     start
