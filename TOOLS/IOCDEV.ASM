;=====================================================================
; IOCDEV.ASM - IOCDEV.SYS, a character driver that is spoken to the
;              way Windows for Workgroups speaks to IFSHLP.SYS
;
; TESTDEV.SYS proves that a driver loads and can be read and written.
; This one is for everything else DOS sends a character driver, and for
; the other DOS: the one under Windows (RMDOS.SYS), which inherits the
; drivers CONFIG.SYS loaded and has to be able to talk to them too.
; DRVTEST.COM asks it the same questions on both sides of the handover.
;
; ITS HEADER ASKS FOR ALL OF IT: IOCTL read and write, open and close,
; and the generic IOCTL.  What it does with them:
;
;   IOCTL READ   eight bytes starting 'SIGN' are answered 'OKAY' and a
;                far pointer to an entry point - the handshake IFSMGR.386
;                makes with IFSHLP.SYS.  Anything else gets the status
;                block (IOS_*): opens, closes, bytes written, the last
;                IOCTL write, and the InDOS and list-of-lists addresses
;                DOS gave it at INIT.
;   IOCTL WRITE  up to sixteen bytes kept for the status block; a first
;                byte 'R' also rewinds what READ hands out.
;   OPEN, CLOSE  counted.
;   READ         "LINE1",CR,"LINE2",CR,^Z, and then nothing: enough for
;                DOS's cooked rules (a line at a time, ^Z the end) to be
;                seen, and for raw mode to be seen not to apply them.
;   WRITE        counted.
;   GENERIC      category AEh, function 01h: 'GENI' into the caller's
;                block.  Any other: "unknown command", which DOS turns
;                into error 16h.
;
; AND AT INIT IT DOES WHAT IFSHLP DOES: asks 34h and 52h and keeps the
; answers, and hooks INT 2Fh.  After the handover those answers have to
; mean the same thing - they are why the kernel's DOS data segment and
; RMDOS's are laid out alike (RMDOS.INC) - and the hook has to still be
; there.  INT 2Fh AX=DF00h is it: AL=FFh, BX='IO', CX the opens and DX
; the closes.
;
; Linked with:  wlink form raw bin order clname CODE offset=0
;=====================================================================
        .386
_ID     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_ID, DS:_ID, ES:_ID, SS:_ID
        ORG     0

header:
        dd      0FFFFFFFFh
        dw      0C840h                  ; character; IOCTL; open and close;
                                        ; generic IOCTL
        dw      strat
        dw      intr
        db      'IOCDEV$ '

reqoff  dw      0
reqseg  dw      0

; what READ hands out - up here, ahead of the code that measures it,
; because wasm will not take an equate before it is defined
text        db  'LINE1',13,'LINE2',13,1Ah
TEXT_LEN    EQU $ - text

;--- the status block an IOCTL read is given --------------------------
IOS_SIG         EQU     0               ; dd 'IOCD'
IOS_OPENS       EQU     4               ; dw
IOS_CLOSES      EQU     6               ; dw
IOS_WRITTEN     EQU     8               ; dw bytes WRITE has been given
IOS_IOLEN       EQU     0Ah             ; dw the last IOCTL write's length
IOS_IOBUF       EQU     0Ch             ; 16 bytes of it
IOS_INDOS       EQU     1Ch             ; dd what 34h said at INIT
IOS_LOL         EQU     20h             ; dd what 52h said at INIT
IOS_LEN         EQU     24h

MPX_FN          EQU     0DF00h          ; INT 2Fh: is the hook still there?

strat:
        mov     word ptr cs:[reqoff],bx
        mov     word ptr cs:[reqseg],es
        retf

intr:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        push    cs
        pop     ds

        les     bx,dword ptr [reqoff]
        mov     al,es:[bx+2]
        cmp     al,0
        je      do_init
        cmp     al,3
        je      do_ioin
        cmp     al,4
        je      do_read
        cmp     al,6
        je      do_instat
        cmp     al,8
        je      do_write
        cmp     al,9
        je      do_write
        cmp     al,0Ah
        je      do_done
        cmp     al,0Ch
        je      do_ioout
        cmp     al,0Dh
        je      do_open
        cmp     al,0Eh
        je      do_close
        cmp     al,13h
        je      do_generic
        mov     word ptr es:[bx+3],8103h        ; unknown command
        jmp     intr_out

do_done:
        mov     word ptr es:[bx+3],0100h
        jmp     intr_out

do_open:
        inc     word ptr [stat + IOS_OPENS]
        jmp     short do_done
do_close:
        inc     word ptr [stat + IOS_CLOSES]
        jmp     short do_done

;=====================================================================
; IOCTL READ.  The handshake, or the status block.
;=====================================================================
do_ioin:
        mov     cx,es:[bx+12h]
        push    es
        push    bx
        les     di,dword ptr es:[bx+0Eh]
        cmp     cx,8
        jne     ioin_stat
        cmp     dword ptr es:[di],'NGIS'        ; 'SIGN' as it lies in memory
        jne     ioin_stat
        mov     dword ptr es:[di],'YAKO'        ; 'OKAY'
        mov     word ptr es:[di+4],OFFSET entry
        mov     word ptr es:[di+6],cs
        jmp     short ioin_out
ioin_stat:
        cmp     cx,IOS_LEN
        jbe     ioin_cnt
        mov     cx,IOS_LEN
ioin_cnt:
        mov     si,OFFSET stat
        cld
        push    cx
        rep     movsb
        pop     cx
ioin_out:
        pop     bx
        pop     es
        mov     es:[bx+12h],cx
        jmp     do_done

;=====================================================================
; IOCTL WRITE.  Keep it; 'R' first also rewinds READ.
;=====================================================================
do_ioout:
        mov     cx,es:[bx+12h]
        cmp     cx,16
        jbe     ioout_cnt
        mov     cx,16
ioout_cnt:
        mov     word ptr [stat + IOS_IOLEN],cx
        push    es
        push    bx
        push    ds
        lds     si,dword ptr es:[bx+0Eh]
        push    cs
        pop     es
        mov     di,OFFSET stat + IOS_IOBUF
        cld
        push    cx
        rep     movsb
        pop     cx
        pop     ds
        pop     bx
        pop     es
        mov     es:[bx+12h],cx
        cmp     byte ptr [stat + IOS_IOBUF],'R'
        jne     do_done
        mov     word ptr [readpos],0
        jmp     do_done

;=====================================================================
; READ - what is left of the text, as much as was asked for.
;=====================================================================
do_read:
        mov     cx,es:[bx+12h]
        push    es
        push    bx
        les     di,dword ptr es:[bx+0Eh]
        xor     dx,dx
rd_lp:
        cmp     dx,cx
        jae     rd_end
        mov     si,[readpos]
        cmp     si,TEXT_LEN
        jae     rd_end
        mov     al,[text + si]
        mov     es:[di],al
        inc     di
        inc     word ptr [readpos]
        inc     dx
        jmp     short rd_lp
rd_end:
        pop     bx
        pop     es
        mov     es:[bx+12h],dx
        jmp     do_done

;--- INPUT STATUS: busy is "nothing to read" --------------------------
do_instat:
        mov     ax,0100h
        cmp     word ptr [readpos],TEXT_LEN
        jb      ins_put
        mov     ax,0300h
ins_put:
        mov     es:[bx+3],ax
        jmp     intr_out

;=====================================================================
; WRITE - counted.
;=====================================================================
do_write:
        mov     cx,es:[bx+12h]
        add     word ptr [stat + IOS_WRITTEN],cx
        jmp     do_done

;=====================================================================
; GENERIC IOCTL (13h).  The block is the far pointer at 13h.
;=====================================================================
do_generic:
        cmp     word ptr es:[bx+0Dh],01AEh      ; category AEh, function 01h
        jne     gen_bad
        push    es
        push    bx
        les     bx,dword ptr es:[bx+13h]
        mov     dword ptr es:[bx],'INEG'        ; 'GENI'
        pop     bx
        pop     es
        jmp     do_done
gen_bad:
        mov     word ptr es:[bx+3],8103h
        jmp     intr_out

intr_out:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        retf

;=====================================================================
; entry - the far procedure the handshake hands out, called with one
;         word on the stack as IFSMGR calls IFSHLP's.  AX = 0 and
;         DX = 'IO' back; the word is the caller's to remove.
;=====================================================================
entry:
        xor     ax,ax
        mov     dx,'IO'
        retf

;=====================================================================
; The INT 2Fh hook INIT installs.
;=====================================================================
mpx:
        cmp     ax,MPX_FN
        je      mpx_mine
        jmp     dword ptr cs:[oldmpx]
mpx_mine:
        mov     al,0FFh
        mov     bx,'IO'
        mov     cx,word ptr cs:[stat + IOS_OPENS]
        mov     dx,word ptr cs:[stat + IOS_CLOSES]
        iret

;=====================================================================
; data - all of it below endres
;=====================================================================
oldmpx      dd  0
readpos     dw  0
stat        db  'IOCD'
            dw  0, 0, 0, 0
            db  16 dup (0)
            dd  0, 0
IF ($ - stat) NE IOS_LEN
        .ERR    <IOCDEV: the status block is not IOS_LEN bytes>
ENDIF

endres:

;=====================================================================
; INIT - after endres, so it is handed back when INIT returns.
;=====================================================================
do_init:
        mov     dx,OFFSET msg_hello
        mov     ah,09h
        int     21h

        ; what IFSHLP keeps: the InDOS flag and the list of lists
        push    es
        push    bx
        mov     ah,34h
        int     21h
        mov     word ptr [stat + IOS_INDOS],bx
        mov     word ptr [stat + IOS_INDOS + 2],es
        mov     ah,52h
        int     21h
        mov     word ptr [stat + IOS_LOL],bx
        mov     word ptr [stat + IOS_LOL + 2],es

        ; and the hook
        mov     ax,352Fh
        int     21h
        mov     word ptr [oldmpx],bx
        mov     word ptr [oldmpx + 2],es
        mov     dx,OFFSET mpx
        mov     ax,252Fh
        int     21h
        pop     bx
        pop     es

        mov     word ptr es:[bx+0Eh],OFFSET endres
        mov     word ptr es:[bx+10h],cs
        mov     byte ptr es:[bx+0Dh],0
        jmp     do_done

msg_hello   db  'IOCDEV.SYS installed - a device taking IOCTL requests',13,10,'$'

_ID     ENDS
        END     header
