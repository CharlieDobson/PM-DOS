;=====================================================================
; SRSTTEST.COM [cable] - resets an IDE cable the way a DOS driver
; does: SRST written to the device control register (3F6h, or 376h for
; cable 1), held, let go, and the master's alternate status watched
; until BSY clears.  Prints the status before and after.  For trying
; LRGDISK.386 in a DOS box: a reset of a cable it shares makes it tell
; its own drives their parameters again before its next command.
;=====================================================================
        .386
_SR     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_SR, DS:_SR, ES:_SR, SS:_SR
        ORG     0
start:
        mov     sp,OFFSET stk
        mov     si,81h
sr_arg:
        lodsb
        cmp     al,13
        je      sr_go
        cmp     al,'1'
        jne     sr_arg
        mov     word ptr [ctl],376h
        mov     word ptr [base],170h
        jmp     sr_arg
sr_go:
        mov     dx,OFFSET m_hello
        call    say
        mov     dx,[ctl]
        in      al,dx
        call    hex2
        mov     dx,OFFSET m_reset
        call    say
        mov     dx,[ctl]
        mov     al,0Eh                  ; SRST, nIEN
        out     dx,al
        mov     cx,2000                 ; well over 5us
sr_hold:
        in      al,dx
        loop    sr_hold
        mov     al,0Ah                  ; SRST off, nIEN stays
        out     dx,al
        push    es
        xor     ax,ax
        mov     es,ax
        mov     cx,180                  ; ten seconds of ticks
sr_tick:
        mov     ah,byte ptr es:[46Ch]
sr_poll:
        mov     dx,[ctl]
        in      al,dx
        test    al,80h
        jz      sr_ready
        cmp     ah,byte ptr es:[46Ch]
        je      sr_poll
        loop    sr_tick
sr_ready:
        pop     es
        push    ax
        mov     dx,OFFSET m_after
        call    say
        pop     ax
        call    hex2
        mov     dx,[base]               ; the status register: anything
        add     dx,7                    ; pending dropped
        in      al,dx
        mov     dx,[ctl]
        mov     al,08h                  ; interrupts on again
        out     dx,al
        mov     dx,OFFSET m_crlf
        call    say
        mov     ax,4C00h
        int     21h

say:
        mov     ah,9
        int     21h
        ret
hex2:
        push    ax
        shr     al,4
        call    digit
        pop     ax
        and     al,0Fh
digit:
        add     al,'0'
        cmp     al,'9'
        jbe     dg_put
        add     al,'A'-'0'-10
dg_put:
        mov     dl,al
        mov     ah,2
        int     21h
        ret

ctl     dw      3F6h
base    dw      1F0h
m_hello db      'SRSTTEST - the cable', 27h, 's status $'
m_reset db      ', reset$'
m_after db      ', after it $'
m_crlf  db      13, 10, '$'
        db      256 dup (0)
stk     LABEL   BYTE
_SR     ENDS
        END     start
