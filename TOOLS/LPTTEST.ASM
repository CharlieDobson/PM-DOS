;=====================================================================
; LPTTEST.ASM - LPTTEST.COM, the positive control the PRINTER channel
;               never had.
;
; WHY IT IS WORTH ASKING AGAIN.  DBGSINK.INC says the printer was
; tried first, that six OUTs a byte delivered nothing, that the
; machine's own BIOS through INT 17h delivered nothing either, and
; that the cause was 86Box dropping lpt1_enabled out of its
; configuration file every time it rewrote it on exit - so the port
; was off on every run after the first and neither result was ever a
; statement about the code.  The configuration is one line and the run
; scripts can put it back before every launch; what is missing is a
; program that says, in one boot, whether the port is THERE.
;
; And it is worth the trouble, because the disk channel this project
; settled on costs an eight-kilobyte INT 13h write per DOS call and
; eight kilobytes of RMDOS to hold the buffer, and the build that
; carries it does not reach the same place as the build that does not.
; A printer channel costs six OUTs a byte and no memory at all.
;
; THREE QUESTIONS, IN ORDER.  What the BIOS thinks is at 0040:0008 -
; zero there means the emulator has no port and nothing below can
; work.  Then the status register, which on a port with nothing
; attached floats and on 86Box's Generic Text Printer does not.  Then
; the same line sent twice, once by hand and once through INT 17h,
; with the tick count either took: an instrument that costs a tick a
; byte is not one you can leave switched on.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_LT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_LT, DS:_LT, ES:_LT, SS:_LT
        ORG     0

LINES   EQU     64                      ; lines sent by each method

start:
        mov     dx,OFFSET m_hi
        call    puts

;--- what the BIOS found at boot ------------------------------------
        mov     dx,OFFSET m_bda
        call    puts
        push    ds
        mov     ax,40h
        mov     ds,ax
        mov     ax,ds:[8]
        pop     ds
        mov     [port],ax
        call    put4
        call    crlf
        or      ax,ax
        jnz     lt_have
        mov     dx,OFFSET m_noport
        call    puts
        mov     word ptr [port],378h    ; try it anyway - the BDA is
                                        ; the BIOS's opinion, not the bus
lt_have:

;--- and what the status register says -------------------------------
        mov     dx,OFFSET m_stat
        call    puts
        mov     dx,[port]
        inc     dx
        in      al,dx
        mov     ah,0
        call    put4
        call    crlf

;--- does the data latch even take a byte? ---------------------------
; The first run of this program put SIXTY-FOUR LINES on the host, and
; every printable character in them came out as 0Ch - the value the
; control register is left at - while the CR and the LF of each line
; came through correctly.  The BIOS through INT 17h produced exactly
; the same thing, and the BIOS is the reference implementation, so
; whatever is wrong is not in the strobe above.  Reading the data
; register back separates the two remaining possibilities: a latch
; that never took the byte, and a latch that took it and was not
; consulted.
        mov     dx,OFFSET m_latch
        call    puts
        mov     dx,[port]
        mov     al,41h
        out     dx,al
        in      al,dx
        mov     ah,0
        call    put4
        mov     dx,OFFSET m_sp
        call    puts
        mov     dx,[port]
        mov     al,55h
        out     dx,al
        in      al,dx
        mov     ah,0
        call    put4
        call    crlf

;--- four ways to strobe, each with its own marker -------------------
; A is what the BIOS does and what this program did: the byte, then
; idle, strobe, idle.  B sets the control register idle BEFORE the
; byte, in case the byte is only latched while the port is selected.
; C leaves the leading idle out, so that the only edge is the one that
; matters.  D holds the strobe down for a while, in case the printer
; samples on a clock rather than an edge.
        mov     dx,OFFSET m_ways
        call    puts
        mov     si,OFFSET m_a
        mov     byte ptr [way],0
        call    prn_str
        mov     si,OFFSET m_b
        mov     byte ptr [way],1
        call    prn_str
        mov     si,OFFSET m_c
        mov     byte ptr [way],2
        call    prn_str
        mov     si,OFFSET m_d
        mov     byte ptr [way],3
        call    prn_str
        mov     byte ptr [way],0

;--- and through the BIOS, which is the reference implementation -----
        mov     dx,OFFSET m_bios
        call    puts
        mov     si,OFFSET m_line2
lt_bch:
        lodsb
        or      al,al
        jz      lt_bdone
        mov     ah,0
        xor     dx,dx                   ; LPT1
        int     17h
        jmp     short lt_bch
lt_bdone:

        mov     dx,OFFSET m_bye
        call    puts
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; prn_str - DS:SI, terminated by a zero, straight at the port.
;---------------------------------------------------------------------
prn_str:
        push    ax
        push    si
ps_loop:
        lodsb
        or      al,al
        jz      ps_done
        call    prn_ch
        jmp     short ps_loop
ps_done:
        pop     si
        pop     ax
        ret

;---------------------------------------------------------------------
; prn_ch - AL out of the parallel port, by whichever of four strobes
;          [way] names.
;
; THE WAIT IS BOUNDED AND THE BYTE GOES ANYWAY.  A printer that is
; never ready and a port that is not there read the same way, and an
; instrument that hangs when its channel is missing is worse than one
; that drops a byte.
;---------------------------------------------------------------------
prn_ch:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bl,al                   ; the byte, out of harm's way
        mov     dx,cs:[port]
        inc     dx                      ; the status register
        mov     cx,400h
pc_wait:
        in      al,dx
        test    al,80h                  ; nBUSY - high means ready
        jnz     pc_ready
        loop    pc_wait
pc_ready:
        mov     dx,cs:[port]
        add     dx,2                    ; the control register
        mov     al,cs:[way]
        cmp     al,1
        je      pc_b
        cmp     al,2
        je      pc_c
        cmp     al,3
        je      pc_d

;--- A: the byte, then idle, strobe, idle - what the BIOS does
pc_a:
        sub     dx,2
        mov     al,bl
        out     dx,al
        add     dx,2
        mov     al,0Ch
        out     dx,al
        mov     al,0Dh
        out     dx,al
        mov     al,0Ch
        out     dx,al
        jmp     short pc_out

;--- B: idle FIRST, in case the latch only takes a byte while selected
pc_b:
        mov     al,0Ch
        out     dx,al
        sub     dx,2
        mov     al,bl
        out     dx,al
        add     dx,2
        mov     al,0Dh
        out     dx,al
        mov     al,0Ch
        out     dx,al
        jmp     short pc_out

;--- C: one edge and no other
pc_c:
        sub     dx,2
        mov     al,bl
        out     dx,al
        add     dx,2
        mov     al,0Dh
        out     dx,al
        mov     al,0Ch
        out     dx,al
        jmp     short pc_out

;--- D: the strobe held down, in case it is sampled and not caught
pc_d:
        sub     dx,2
        mov     al,bl
        out     dx,al
        add     dx,2
        mov     al,0Ch
        out     dx,al
        mov     al,0Dh
        out     dx,al
        mov     cx,64
pc_dhold:
        push    ax
        pop     ax
        loop    pc_dhold
        mov     al,0Ch
        out     dx,al
pc_out:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
ticks:
        push    cx
        push    dx
        xor     ah,ah
        int     1Ah                     ; CX:DX = the tick count
        mov     ax,dx
        pop     dx
        pop     cx
        ret

puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret

crlf:
        push    ax
        push    dx
        mov     dx,OFFSET m_crlf
        mov     ah,09h
        int     21h
        pop     dx
        pop     ax
        ret

put4:
        push    ax
        push    cx
        push    dx
        mov     cx,4
p4_loop:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p4_dig
        add     al,7
p4_dig:
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     ax
        loop    p4_loop
        pop     dx
        pop     cx
        pop     ax
        ret

;---------------------------------------------------------------------
port        dw  0
way         db  0                       ; which strobe prn_ch uses

m_hi        db  'LPTTEST - is there a printer port on this machine?',13,10,'$'
m_bda       db  'BIOS says LPT1 is at port: $'
m_noport    db  '  ...which is zero: the emulator has no LPT1.  Trying 378h.',13,10,'$'
m_stat      db  'status register reads:     $'
m_latch     db  'data register reads back:  $'
m_sp        db  '  and  $'
m_ways      db  'four strobes, A B C D, sent.',13,10,'$'
m_bios      db  'and one line through INT 17h.',13,10,'$'
m_bye       db  'Look in VMs\PM-DOS\printer for a .txt naming which one worked.',13,10,'$'
m_crlf      db  13,10,'$'

m_a         db  'AAAA byte-idle-strobe-idle',13,10,0
m_b         db  'BBBB idle-byte-strobe-idle',13,10,0
m_c         db  'CCCC byte-strobe-idle',13,10,0
m_d         db  'DDDD byte-idle-strobe-held',13,10,0
m_line2     db  'EEEE through INT 17h',13,10,0

_LT     ENDS
        END     start
