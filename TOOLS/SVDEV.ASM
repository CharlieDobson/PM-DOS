;=====================================================================
; SVDEV.ASM - SVDEV.SYS: what 30h tells a driver while its INIT runs
;
; A character driver that does nothing but ask.  Its INIT calls 30h and
; prints one line to LPT1 through INT 17h - the one way out of a
; driver's INIT that a headless run can read (DBXRUN -Lpt):
;
;   SVDEV.SYS INIT 30h 4.00
;
; MS-DOS gives a driver SETVER's entry for its file name for the length
; of its INIT, and so does PM-DOS (dl_load, sv_initver), so with
; SVDEV.SYS 4.00 in the table this says 4.00 and without it 6.22.
; After INIT the driver keeps nothing but its header and answers every
; request with "done".
;
; Linked with:  wlink form raw bin option offset=0x0
;=====================================================================
        .386
_SVD    SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_SVD, DS:NOTHING, ES:NOTHING, SS:NOTHING
        ORG     0

hdr         dw  0FFFFh, 0FFFFh
            dw  8000h                   ; a character device
            dw  OFFSET strat
            dw  OFFSET intr
            db  'SVDEV$$$'
pkt         dd  0

strat:
        mov     word ptr cs:[pkt],bx
        mov     word ptr cs:[pkt+2],es
        retf

intr:
        push    bx
        push    ds
        lds     bx,cs:[pkt]
        cmp     byte ptr [bx+2],0
        jne     done
        call    init
done:
        mov     word ptr [bx+3],0100h
        pop     ds
        pop     bx
        retf
keep_end:

init:
        push    ax
        push    dx
        push    si
        mov     word ptr [bx+0Eh],OFFSET keep_end
        mov     word ptr [bx+10h],cs
        mov     si,OFFSET msg
        call    lpt_str
        mov     ax,3000h
        int     21h
        push    ax
        aam                             ; the major, one digit or two
        add     ax,3030h
        cmp     ah,'0'
        je      in_units
        xchg    al,ah
        call    lpt_chr
        xchg    al,ah
in_units:
        call    lpt_chr
        mov     al,'.'
        call    lpt_chr
        pop     ax
        mov     al,ah                   ; and the minor, always two
        aam
        add     ax,3030h
        xchg    al,ah
        call    lpt_chr
        xchg    al,ah
        call    lpt_chr
        mov     si,OFFSET msg_crlf
        call    lpt_str
        pop     si
        pop     dx
        pop     ax
        ret

lpt_str:
        mov     al,cs:[si]
        or      al,al
        jz      ls_out
        call    lpt_chr
        inc     si
        jmp     short lpt_str
ls_out:
        ret

lpt_chr:
        push    ax
        push    dx
        xor     ah,ah
        xor     dx,dx                   ; LPT1
        int     17h
        pop     dx
        pop     ax
        ret

msg         db  'SVDEV.SYS INIT 30h ',0
msg_crlf    db  13,10,0

_SVD    ENDS
        END
