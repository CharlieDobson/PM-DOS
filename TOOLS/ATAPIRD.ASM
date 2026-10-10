;=====================================================================
; ATAPIRD.COM [unit [blocks [passes]]] - reads a CD-ROM on an IDE cable
; through the ports, as a DOS CD-ROM driver does: ATAPI PACKET
; commands, PIO, polled (the drive's interrupt left on, so that whoever
; owns IRQ 14 or 15 sees them).  unit 0 or 1 of the first cable, 2 or
; 3 of the second (1), blocks from the
; start of the disc (all of it), passes (3).  A checksum a pass, over
; every dword in order (rol 5, add), which the host works out from the
; image (atapisum.pl); every pass must give the same one.  Made to run
; in a DOS box beside others that use the hard disk, to try LRGDISK
; sharing the cable.
;=====================================================================
        .486
_AR     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_AR, DS:_AR, ES:_AR, SS:_AR
        ORG     0

CHUNK   EQU     16                      ; blocks a READ(10)

start:
        mov     sp,OFFSET stk
        mov     ax,OFFSET prog_end
        add     ax,15
        shr     ax,4
        mov     bx,cs
        add     ax,bx
        mov     [bufseg],ax

        mov     si,81h
        call    getnum
        jc      a_unit
        mov     [unit],al
        cmp     al,2                    ; 2 and 3: the second cable
        jb      a_unit
        mov     word ptr [base],170h
        mov     word ptr [ctl],376h
a_unit:
        call    getnum
        jc      a_blocks
        mov     [want],eax
a_blocks:
        call    getnum
        jc      a_passes
        mov     [passes],ax
a_passes:
        mov     dx,OFFSET m_hello
        call    say
        mov     al,[unit]
        add     al,'0'
        call    putc

        ; IDENTIFY PACKET DEVICE: the model
        call    sel
        jc      e_nodev
        mov     dx,[base]
        add     dx,7
        mov     al,0A1h
        out     dx,al
        call    delay
        call    waitnb
        jc      e_nodev
        mov     dx,[base]
        add     dx,7
        in      al,dx
        test    al,01h
        jnz     e_noatapi
        test    al,08h
        jz      e_noatapi
        mov     di,OFFSET ident
        mov     cx,256
        mov     dx,[base]
        cld
        rep     insw
        mov     dx,OFFSET m_model
        call    say
        mov     si,OFFSET ident+27*2
        mov     cx,20
a_model:
        lodsw
        xchg    al,ah
        call    putc
        mov     al,ah
        call    putc
        loop    a_model
        mov     dx,OFFSET m_crlf
        call    say

        ; READ CAPACITY: the last block
        mov     si,OFFSET pk_cap
        call    command
        jc      e_cap
        push    es
        mov     es,[bufseg]
        mov     eax,dword ptr es:[0]
        pop     es
        bswap   eax
        inc     eax
        mov     [blocks],eax
        mov     dx,OFFSET m_blocks
        call    say
        mov     eax,[blocks]
        call    putdec
        mov     dx,OFFSET m_crlf
        call    say
        mov     eax,[want]
        or      eax,eax
        jz      a_all
        cmp     eax,[blocks]
        ja      a_all
        mov     [blocks],eax
a_all:

        ; the passes
        mov     word ptr [pass],0
a_pass:
        inc     word ptr [pass]
        mov     dword ptr [csum],0
        mov     dword ptr [lba],0
        mov     word ptr [errs],0
        call    ticks
        mov     [t0],eax
a_chunk:
        mov     eax,[blocks]
        sub     eax,[lba]
        jbe     a_passend
        cmp     eax,CHUNK
        jbe     a_n
        mov     eax,CHUNK
a_n:
        mov     [n],ax
        mov     eax,[lba]               ; READ(10): LBA and count, big-endian
        bswap   eax
        mov     dword ptr [pk_read+2],eax
        mov     ax,[n]
        xchg    al,ah
        mov     word ptr [pk_read+7],ax
        mov     si,OFFSET pk_read
        call    command
        jc      a_bad
        mov     ax,[n]                  ; all of it?
        shl     ax,11
        cmp     ax,[got]
        jne     a_short
        call    sumbuf
        jmp     a_next
a_short:
        mov     byte ptr [why],'S'
a_bad:
        inc     word ptr [errs]
        cmp     word ptr [errs],4
        ja      a_next
        mov     dx,OFFSET m_err
        call    say
        mov     eax,[lba]
        call    putdec
        mov     dx,OFFSET m_errst
        call    say
        mov     al,[why]
        call    putc
        mov     al,' '
        call    putc
        movzx   eax,word ptr [lastst]
        mov     cl,4
        call    puthex
        mov     dx,OFFSET m_crlf
        call    say
a_next:
        movzx   eax,word ptr [n]
        add     [lba],eax
        jmp     a_chunk
a_passend:
        call    ticks
        sub     eax,[t0]
        mov     [t0],eax
        mov     dx,OFFSET m_pass
        call    say
        movzx   eax,word ptr [pass]
        call    putdec
        mov     dx,OFFSET m_sum
        call    say
        mov     eax,[csum]
        mov     cl,8
        call    puthex
        mov     dx,OFFSET m_errors
        call    say
        movzx   eax,word ptr [errs]
        call    putdec
        mov     dx,OFFSET m_ticks
        call    say
        mov     eax,[t0]
        call    putdec
        mov     dx,OFFSET m_crlf
        call    say
        mov     ax,[pass]
        cmp     ax,[passes]
        jb      a_pass
        mov     ax,4C00h
        int     21h

e_nodev:
        mov     dx,OFFSET m_nodev
        jmp     e_out
e_noatapi:
        mov     dx,OFFSET m_noatapi
        jmp     e_out
e_cap:
        mov     dx,OFFSET m_nocap
e_out:
        call    say
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; command - the packet at SI, its data into bufseg:0, up to four tries
; (REQUEST SENSE between: a fresh disc reports UNIT ATTENTION once).
; [got] = bytes; CF, [lastst] = status and error, [why] = a letter.
;---------------------------------------------------------------------
command:
        mov     byte ptr [tries],4
cm_try:
        push    si
        call    packet
        pop     si
        jnc     cm_ok
        dec     byte ptr [tries]
        jz      cm_fail
        push    si
        push    word ptr [lastst]
        mov     si,OFFSET pk_sense
        call    packet
        pop     word ptr [lastst]
        pop     si
        jmp     cm_try
cm_ok:
        clc
cm_fail:
        ret

;---------------------------------------------------------------------
; packet - one PACKET command: the 12 bytes at SI, the data read into
; bufseg:0, [got] = bytes.  CF on failure, [lastst] = status:error.
;---------------------------------------------------------------------
packet:
        mov     word ptr [got],0
        mov     byte ptr [why],'B'
        call    sel
        jc      pk_fail
        mov     dx,[base]
        add     dx,1
        xor     al,al
        out     dx,al                   ; PIO
        inc     dx
        out     dx,al
        inc     dx
        out     dx,al
        inc     dx
        out     dx,al                   ; byte count limit 8000h
        inc     dx
        mov     al,80h
        out     dx,al
        mov     dx,[base]
        add     dx,7
        mov     al,0A0h
        out     dx,al
        call    delay
        mov     byte ptr [why],'P'
        call    waitnb
        jc      pk_fail
        test    al,08h
        jz      pk_status
        mov     dx,[base]
        mov     cx,6
        cld
        rep     outsw
        xor     di,di
pk_phase:
        call    delay
        mov     byte ptr [why],'D'
        call    waitnb
        jc      pk_fail
        mov     dx,[base]
        add     dx,7
        in      al,dx
        mov     byte ptr [lastst+1],al
        test    al,01h
        jnz     pk_err
        test    al,08h
        jz      pk_done
        mov     dx,[base]
        add     dx,4
        in      al,dx
        mov     cl,al
        inc     dx
        in      al,dx
        mov     ch,al
        inc     cx
        shr     cx,1
        mov     ax,di
        add     ax,cx
        add     ax,cx
        jc      pk_over
        cmp     ax,CHUNK*2048
        ja      pk_over
        mov     dx,[base]
        push    es
        mov     es,[bufseg]
        rep     insw
        pop     es
        mov     [got],di
        jmp     pk_phase
pk_done:
        clc
        ret
pk_over:
        mov     byte ptr [why],'O'
        jmp     pk_fail
pk_status:
        mov     dx,[base]
        add     dx,7
        in      al,dx
        mov     byte ptr [lastst+1],al
pk_err:
        mov     byte ptr [why],'E'
pk_fail:
        mov     dx,[base]
        add     dx,1
        in      al,dx
        mov     byte ptr [lastst],al
        mov     dx,[base]                 ; (the interrupt, if any, ended)
        add     dx,7
        in      al,dx
        stc
        ret

; sel - the unit selected and not busy; CF if it stays busy
sel:
        mov     al,[unit]
        and     al,1
        shl     al,4
        or      al,0A0h
        mov     dx,[base]
        add     dx,6
        out     dx,al
        call    delay
        jmp     waitnb

; waitnb - the alternate status until BSY goes, five seconds at most;
; AL = status, CF if it stayed
waitnb:
        push    cx
        push    es
        xor     cx,cx
        mov     es,cx
        mov     cx,91
        mov     dx,[ctl]
wn_tick:
        mov     ah,byte ptr es:[46Ch]
wn_poll:
        in      al,dx
        test    al,80h
        jz      wn_ok
        cmp     ah,byte ptr es:[46Ch]
        je      wn_poll
        loop    wn_tick
        stc
        jmp     wn_out
wn_ok:
        clc
wn_out:
        pop     es
        pop     cx
        ret

delay:
        push    ax
        push    dx
        mov     dx,[ctl]
        in      al,dx
        in      al,dx
        in      al,dx
        in      al,dx
        pop     dx
        pop     ax
        ret

; sumbuf - [got] bytes of bufseg:0 into the checksum
sumbuf:
        push    ds
        mov     cx,[got]
        shr     cx,2
        mov     ebx,[csum]
        mov     ds,[bufseg]
        xor     si,si
        cld
sb_loop:
        lodsd
        rol     ebx,5
        add     ebx,eax
        loop    sb_loop
        pop     ds
        mov     [csum],ebx
        ret

ticks:
        push    es
        xor     ax,ax
        mov     es,ax
        mov     eax,dword ptr es:[46Ch]
        pop     es
        ret

; getnum - a decimal number from the command tail at SI: EAX, CF if none
getnum:
        xor     eax,eax
        xor     ecx,ecx
gn_skip:
        mov     cl,[si]
        cmp     cl,' '
        jne     gn_first
        inc     si
        jmp     gn_skip
gn_first:
        sub     cl,'0'
        cmp     cl,9
        ja      gn_none
gn_digit:
        mov     cl,[si]
        sub     cl,'0'
        cmp     cl,9
        ja      gn_done
        imul    eax,eax,10
        add     eax,ecx
        inc     si
        jmp     gn_digit
gn_done:
        clc
        ret
gn_none:
        stc
        ret

say:
        mov     ah,09h
        int     21h
        ret

putc:
        push    ax
        push    dx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret

puthex:                                 ; EAX, CL digits
        push    eax
        push    cx
        push    edx
        mov     edx,eax
        movzx   cx,cl
ph_digit:
        push    cx
        dec     cx
        shl     cx,2
        mov     eax,edx
        shr     eax,cl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph_put
        add     al,'A'-'0'-10
ph_put:
        call    putc
        pop     cx
        loop    ph_digit
        pop     edx
        pop     cx
        pop     eax
        ret

putdec:                                 ; EAX
        push    eax
        push    cx
        push    edx
        push    ebx
        xor     cx,cx
        mov     ebx,10
pd_div:
        xor     edx,edx
        div     ebx
        push    dx
        inc     cx
        or      eax,eax
        jnz     pd_div
pd_put:
        pop     ax
        add     al,'0'
        call    putc
        loop    pd_put
        pop     ebx
        pop     edx
        pop     cx
        pop     eax
        ret

unit    db      1
base    dw      1F0h                    ; the cable's ports
ctl     dw      3F6h
tries   db      0
why     db      0
passes  dw      3
pass    dw      0
errs    dw      0
n       dw      0
got     dw      0
lastst  dw      0
bufseg  dw      0
want    dd      0
blocks  dd      0
lba     dd      0
csum    dd      0
t0      dd      0
pk_cap  db      25h, 11 dup (0)
pk_sense db     03h, 0, 0, 0, 18, 7 dup (0)
pk_read db      28h, 11 dup (0)
m_hello db      'ATAPIRD - unit $'
m_model db      ': $'
m_blocks db     '  blocks: $'
m_pass  db      'pass $'
m_sum   db      ': checksum $'
m_errors db     ', errors $'
m_ticks db      ', ticks $'
m_err   db      '  failed at block $'
m_errst db      ': $'
m_nodev db      13, 10, 'Nothing answers at that unit.', 13, 10, '$'
m_noatapi db    13, 10, 'That unit is not an ATAPI device.', 13, 10, '$'
m_nocap db      'READ CAPACITY failed.', 13, 10, '$'
m_crlf  db      13, 10, '$'
ident   dw      256 dup (0)
        dw      256 dup (0)
stk     LABEL   WORD
prog_end LABEL  BYTE
_AR     ENDS
        END     start
