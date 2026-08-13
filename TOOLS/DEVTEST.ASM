;=====================================================================
; DEVTEST.ASM - DEVTEST.COM, the v0.30 foundation test
;
; An ordinary 16-bit DOS program, so every call it makes goes through
; the VDM bridge as well as through the kernel - which is the point.
; It covers the four things v0.30 added:
;
;   the device namespace   NUL opens, swallows writes, reads as end of
;                          file, ignores its extension, and cannot be
;                          deleted or renamed into existence
;   21h/34h                the InDOS flag: a real address, and a count
;                          that comes back to zero
;   21h/59h                extended error: the code, class, action and
;                          locus of the failure just before it
;   21h/57h                a file's stamp survives being set, closed
;                          and re-opened
;
; Every check prints "ok" or "FAILED", so the whole run is one screen
; that either has no capitals in it or does.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_DT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_DT, DS:_DT, ES:_DT, SS:_DT
        ORG     0

; The stamp DEVTEST writes and expects to read back.  16:30:00 on
; 1 June 1996 - a date DOS can hold and nobody's clock will produce
; by accident, so a match cannot be a coincidence.
ST_TIME EQU     8400h                   ; 10000b:011110b:00000b
ST_DATE EQU     20C1h                   ; 0010000b:0110b:00001b

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; 34h - the InDOS flag
;=====================================================================
        mov     dx,OFFSET t_indos
        call    puts
        mov     ah,34h
        int     21h
        push    es
        push    bx
        mov     ax,es
        call    put4
        mov     dl,':'
        call    putc
        pop     bx
        push    bx
        mov     ax,bx
        call    put4
        pop     bx
        pop     es
        mov     dx,OFFSET msg_isnow
        call    puts
        mov     al,es:[bx]              ; and what does it say, out here?
        call    put2

        ; Zero is the right answer and the interesting one: it means
        ; the increment on the way into every call above has been
        ; matched on the way out of every one of them.
        cmp     byte ptr es:[bx],0
        je      ind_ok
        call    bad
        jmp     short ind_done
ind_ok:
        call    good
ind_done:

;=====================================================================
; NUL - open it, write to it, read from it
;=====================================================================
        mov     dx,OFFSET t_nulopen
        call    puts
        mov     dx,OFFSET n_nul
        mov     ax,3D02h                ; read/write
        int     21h
        jc      nul_no
        mov     [h_nul],ax
        call    good
        jmp     short nul_write
nul_no:
        call    bad
        jmp     nul_skip

nul_write:
        mov     dx,OFFSET t_nulwr
        call    puts
        mov     bx,[h_nul]
        mov     dx,OFFSET junk
        mov     cx,32
        mov     ah,40h
        int     21h
        jc      nulw_bad
        cmp     ax,32                   ; it must claim to have taken
        jne     nulw_bad                ; all of it, and lose all of it
        call    good
        jmp     short nul_read
nulw_bad:
        call    bad

nul_read:
        mov     dx,OFFSET t_nulrd
        call    puts
        mov     bx,[h_nul]
        mov     dx,OFFSET buf
        mov     cx,32
        mov     ah,3Fh
        int     21h
        jc      nulr_bad
        or      ax,ax                   ; end of file, immediately
        jnz     nulr_bad
        call    good
        jmp     short nul_close
nulr_bad:
        call    bad
nul_close:
        mov     bx,[h_nul]
        mov     ah,3Eh
        int     21h

;--- and the extension is not part of the name -----------------------
        mov     dx,OFFSET t_nuldot
        call    puts
        mov     dx,OFFSET n_nuldot
        mov     ax,3D01h
        int     21h
        jc      nuld_bad
        mov     bx,ax
        mov     ah,3Eh                  ; if this opened a FILE the disk
        int     21h                     ; now has a NUL.TXT on it, which
        call    good                    ; DIR will show
        jmp     short nul_del
nuld_bad:
        call    bad

;--- deleting it is refused, and that is what 59h then reports -------
nul_del:
        mov     dx,OFFSET t_nuldel
        call    puts
        mov     dx,OFFSET n_nul
        mov     ah,41h
        int     21h
        jnc     nuldel_bad              ; success here would be a disaster
        cmp     ax,5                    ; access denied
        jne     nuldel_bad
        call    good
        jmp     short nul_skip
nuldel_bad:
        call    bad

nul_skip:

;=====================================================================
; 59h - what was that failure, exactly?
;
; Asked immediately after the refused delete above, so the answer is
; known: code 5, class 03 (authorization), action 04 (abort after
; cleanup), locus 02 (block device).
;=====================================================================
        mov     dx,OFFSET t_exterr
        call    puts
        xor     bx,bx
        mov     ah,59h
        int     21h
        push    ax
        push    bx
        push    cx
        mov     dx,OFFSET msg_code
        call    puts
        pop     cx
        pop     bx
        pop     ax
        push    ax
        push    bx
        push    cx
        call    put2
        mov     dx,OFFSET msg_class
        call    puts
        pop     cx
        pop     bx
        push    bx
        push    cx
        mov     al,bh
        call    put2
        mov     dl,'/'
        call    putc
        pop     cx
        pop     bx
        push    bx
        push    cx
        mov     al,bl
        call    put2
        mov     dl,'/'
        call    putc
        pop     cx
        push    cx
        mov     al,ch
        call    put2
        pop     cx
        pop     bx
        pop     ax

        cmp     ax,5
        jne     xerr_bad
        cmp     bh,03h
        jne     xerr_bad
        cmp     bl,04h
        jne     xerr_bad
        cmp     ch,02h
        jne     xerr_bad
        call    good
        jmp     short do_stamp
xerr_bad:
        call    bad

;=====================================================================
; 57h - set a file's stamp, close it, open it again, read it back
;=====================================================================
do_stamp:
        mov     dx,OFFSET t_stamp
        call    puts
        mov     byte ptr [step],'a'     ; which call failed, if one does
        mov     dx,OFFSET n_temp
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      stamp_bad
        mov     bx,ax
        mov     [h_tmp],ax
        push    bx
        mov     dx,OFFSET junk          ; something to write back at close
        mov     cx,16
        mov     ah,40h
        int     21h
        pop     bx
        push    bx
        mov     byte ptr [step],'b'
        mov     cx,ST_TIME
        mov     dx,ST_DATE
        mov     ax,5701h
        int     21h
        pop     bx
        jc      stamp_bad
        mov     byte ptr [step],'c'
        mov     ah,3Eh                  ; the close is what writes it out
        int     21h
        jc      stamp_bad

        mov     byte ptr [step],'d'
        mov     dx,OFFSET n_temp
        mov     ax,3D00h
        int     21h
        jc      stamp_bad
        mov     bx,ax
        push    bx
        mov     byte ptr [step],'e'
        mov     ax,5700h
        int     21h
        pop     bx
        push    cx
        push    dx
        mov     ah,3Eh
        int     21h
        pop     dx
        pop     cx

        ; Park them in memory before printing.  put1 ends with "mov
        ; dl,al" and putc only saves DX around the INT 21h, so the hex
        ; printer walks off with DL - which cost an afternoon here,
        ; because the values it printed were CORRECT and the compare
        ; after it was reading a register the printing had eaten.
        mov     [got_time],cx
        mov     [got_date],dx
        mov     ax,dx                   ; show what came back
        call    put4
        mov     dl,'/'
        call    putc
        mov     ax,[got_time]
        call    put4

        mov     byte ptr [step],'f'     ; got this far: it is the VALUES
        mov     cx,[got_time]
        mov     dx,[got_date]
        cmp     cx,ST_TIME
        jne     stamp_bad
        cmp     dx,ST_DATE
        jne     stamp_bad
        call    good
        jmp     short stamp_gone
stamp_bad:
        push    ax
        mov     dl,'@'                  ; which step, and what it said
        call    putc
        mov     dl,[step]
        call    putc
        mov     dl,'='
        call    putc
        pop     ax
        push    ax
        call    put4
        mov     dl,'h'
        call    putc
        mov     dl,'#'
        call    putc
        mov     ax,[h_tmp]
        call    put4
        pop     ax
        call    bad
stamp_gone:
        mov     dx,OFFSET n_temp        ; tidy up after ourselves
        mov     ah,41h
        int     21h

;=====================================================================
; PRN, last, because it is the one that talks to hardware
;
; Nothing is checked here.  A machine with no printer answers INT 17h
; with a timeout and a machine with one prints a form feed, and there
; is no third outcome worth failing the run over - what is being
; tested is that the name resolves to a device at all rather than
; creating a file called PRN.
;=====================================================================
        mov     dx,OFFSET t_prn
        call    puts
        mov     dx,OFFSET n_prn
        mov     ax,3D01h
        int     21h
        jc      prn_bad
        mov     bx,ax
        mov     dx,OFFSET ff
        mov     cx,1
        mov     ah,40h
        int     21h
        mov     ah,3Eh
        int     21h
        call    good
        jmp     short done
prn_bad:
        call    bad

done:
        mov     dx,OFFSET msg_bye
        call    puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; helpers
;=====================================================================
good:
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

puts:
        push    ax
        mov     ah,09h
        int     21h
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
put4:                                   ; AX in hex
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:                                   ; AL in hex
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
; data
;=====================================================================
msg_hi      db  13,10,'DEVTEST - devices, InDOS, extended error, file stamps',13,10,'$'
msg_bye     db  'DEVTEST done.',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_isnow   db  ', now $'
msg_code    db  'AX=$'
msg_class   db  ' class/action/locus $'

t_indos     db  '  34h InDOS flag at $'
t_nulopen   db  '  3Dh open NUL$'
t_nulwr     db  '  40h write 32 bytes to NUL$'
t_nulrd     db  '  3Fh read NUL is end of file$'
t_nuldot    db  '  3Dh open NUL.TXT is the same device$'
t_nuldel    db  '  41h delete NUL is refused$'
t_exterr    db  '  59h says $'
t_stamp     db  '  57h stamp survives close and re-open $'
t_prn       db  '  3Dh open PRN and write to it$'

n_nul       db  'NUL',0
n_nuldot    db  'NUL.TXT',0
n_prn       db  'PRN',0
n_temp      db  'DEVTEST.$$$',0

ff          db  12                      ; form feed, for the printer
fails       db  0
step        db  '?'
h_nul       dw  0
h_tmp       dw  0
got_time    dw  0
got_date    dw  0
junk        db  'DEVTEST wrote this and nobody should ever read it.',0
buf         db  64 dup(0)

_DT     ENDS
        END
