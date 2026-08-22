;=====================================================================
; CTRYDMP.ASM - CTRYDMP.COM, the country block as bytes, nothing else.
;
; CTRYTEST checks the FIELDS a program is documented to read and says
; ok six times.  This prints the whole block in hex and says nothing,
; because the interesting question is not "are the fields we thought
; of right" but "is anything in here different from real DOS" - and
; the only way to ask that is to put the two side by side.
;
; It exists because MS-DOS 6.22's MEM.EXE formats every number it
; prints through the message service, which reads its thousands
; separator - and the fields around it - out of this block.  On PM-DOS
; MEM /C prints a figure with too many digits and then floods; the
; block is the one input to that formatter which PM-DOS supplies, so
; it is the one worth comparing byte for byte.
;
; Run it under DOSBox with "ver set 6 22" and under PM-DOS, redirect
; both, and diff.  A dump with no opinions in it is the point: a test
; that judges can only catch what its author thought of.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_CD     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_CD, DS:_CD, ES:_CD, SS:_CD
        ORG     0

CTRY_LEN        EQU     34              ; what 38h fills for AL=0

start:
        mov     dx,OFFSET msg_hi
        call    puts

;--- 21h/38h AL=0, the call the message service makes ----------------
        mov     byte ptr [blk],0FEh     ; poison, so "not written" is
        mov     cx,CTRY_LEN - 1         ; visible rather than mistaken
        mov     di,OFFSET blk           ; for a zero DOS meant
        mov     al,0FEh
        push    ds
        pop     es
        cld
        rep     stosb

        ; EVERY register, not just the documented one.  38h is
        ; specified to answer in BX and the buffer, and a caller has no
        ; business reading CX - but msgserv's number formatter takes
        ; its character count in CX and does not zero it, so whatever
        ; 38h leaves there is what the next figure MEM prints is
        ; counted with.  "Undefined" is not "the same everywhere", and
        ; the only way to find out what real DOS leaves is to look.
        mov     cx,0AA55h               ; a value neither DOS would pick,
        mov     si,0AA55h               ; so "untouched" is visible
        mov     di,0AA55h
        mov     bp,0AA55h
        mov     dx,OFFSET blk
        xor     al,al
        mov     ah,38h
        int     21h
        mov     [r_ax],ax
        mov     [r_bx],bx
        mov     [r_cx],cx
        mov     [r_dx],dx
        mov     [r_si],si
        mov     [r_di],di
        mov     [r_bp],bp
        pushf
        pop     ax
        mov     [r_fl],ax
        jnc     cd_got
        mov     dx,OFFSET msg_nocall
        call    puts
        jmp     cd_done
cd_got:
        mov     dx,OFFSET msg_regs
        call    puts
        mov     si,OFFSET r_names
        mov     di,OFFSET r_ax
cd_reg:
        mov     dl,[si]
        or      dl,dl
        jz      cd_regdone
        call    putc
        mov     dl,[si+1]
        call    putc
        mov     dl,'='
        call    putc
        mov     bx,di
        mov     ax,[bx]
        call    put4
        mov     dl,' '
        call    putc
        add     si,2
        add     di,2
        jmp     short cd_reg
cd_regdone:
        mov     dx,OFFSET msg_crlf
        call    puts

;--- the block, sixteen to a line, offset first ----------------------
        xor     si,si
cd_line:
        mov     ax,si
        call    put4
        mov     dl,' '
        call    putc
        mov     cx,16
cd_byte:
        cmp     si,CTRY_LEN
        jae     cd_pad
        mov     bx,si
        mov     al,[blk + bx]
        call    put2
        jmp     short cd_next
cd_pad:
        mov     dl,' '
        call    putc
        mov     dl,' '
        call    putc
cd_next:
        mov     dl,' '
        call    putc
        inc     si
        loop    cd_byte

        ; ...and the same sixteen as characters, because a separator
        ; is a character and reading it as 2C every time is a way to
        ; miss that it is not the one you meant
        mov     dl,' '
        call    putc
        mov     si,bx
        sub     si,15
        cmp     si,0
        jge     cd_txt
        xor     si,si
cd_txt:
        mov     cx,16
cd_tch:
        cmp     si,CTRY_LEN
        jae     cd_tdone
        mov     bx,si
        mov     dl,[blk + bx]
        cmp     dl,20h
        jb      cd_tdot
        cmp     dl,7Eh
        jbe     cd_tput
cd_tdot:
        mov     dl,'.'
cd_tput:
        call    putc
        inc     si
        loop    cd_tch
cd_tdone:
        mov     dx,OFFSET msg_crlf
        call    puts
        cmp     si,CTRY_LEN
        jb      cd_line

cd_done:
        mov     ax,4C00h
        int     21h

;=====================================================================
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
        mov     dl,al
        call    putc
        ret

;=====================================================================
msg_hi      db  'CTRYDMP - 21h/38h AL=0, all of it',13,10,'$'
msg_regs    db  'regs after 38h (AA55 = untouched): $'
msg_nocall  db  '38h REFUSED the call',13,10,'$'
msg_crlf    db  13,10,'$'
r_names     db  'AXBXCXDXSIDIBPFL',0,0
r_ax        dw  0
r_bx        dw  0
r_cx        dw  0
r_dx        dw  0
r_si        dw  0
r_di        dw  0
r_bp        dw  0
r_fl        dw  0
blk         db  CTRY_LEN dup (0FEh)

_CD     ENDS
        END     start
