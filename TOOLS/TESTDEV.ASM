;=====================================================================
; TESTDEV.ASM - TESTDEV.SYS, a character device driver to load with
;               DEVICE=, and the only way to find out whether DEVICE=
;               works.
;
; A real DOS driver, written the way real ones are: a header with two
; far entry points, a strategy routine that does nothing but remember
; where the request packet is, and an interrupt routine that does the
; work.  Nothing here knows or cares that the DOS calling it is in
; protected mode.
;
; It answers three questions that between them cover the whole of the
; loading path:
;
;   INIT   prints a line through INT 21h.  That is a DOS call made
;          from inside a driver, before there is a shell, a PSP or a
;          program - the one thing in this kernel that had never
;          happened before - and if it comes out on the screen then
;          the excursion into V86 and the way back both work.
;   WRITE  counts the bytes it is given - and REFUSES a '!' every
;          other time, with "printer out of paper", which is the only
;          way to find out whether a character driver's error status
;          reaches INT 24h.  Alternate refusals rather than permanent
;          ones because a driver that always failed could only test
;          Ignore: a Retry would go round for ever.  DEVCRIT.COM is
;          what this is for.
;   READ   says how many.  So:
;
;               TYPE TESTDEV            "0 bytes written"
;               ECHO HI > TESTDEV
;               TYPE TESTDEV            "4 bytes written"
;
;          which is a round trip through dev_find, the SFT, dev_out,
;          dev_in and a request packet in each direction, and it
;          cannot be faked by a driver that merely loaded.
;
; Deliberately no INT 10h anywhere: BIOS teletype output would land at
; the BIOS's idea of the cursor rather than the kernel's, and a test
; whose output appears in the wrong place is a test that gets argued
; with.
;
; Linked with:  wlink form raw bin order clname CODE offset=0
;=====================================================================
        .386
_TD     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_TD, DS:_TD, ES:_TD, SS:_TD
        ORG     0

;--- the device header.  FFFF:FFFF because this file holds one driver.
header:
        dd      0FFFFFFFFh
        dw      8000h                   ; character device, nothing special
        dw      strat
        dw      intr
        db      'TESTDEV '

;--- what strategy remembers ------------------------------------------
reqoff  dw      0
reqseg  dw      0

;=====================================================================
; strategy - ES:BX is the request packet.  Save it and return.
;
; Two entries for one operation is an artefact of a 1981 plan for
; asynchronous drivers that never arrived; every driver ever shipped
; does exactly this and then does the work in the other one.
;=====================================================================
strat:
        mov     word ptr cs:[reqoff],bx
        mov     word ptr cs:[reqseg],es
        retf

;=====================================================================
; interrupt - do it.
;=====================================================================
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
        ASSUME  DS:_TD

        les     bx,dword ptr [reqoff]   ; ES:BX = the packet
        mov     al,es:[bx+2]            ; the command

        cmp     al,0
        je      do_init
        cmp     al,4
        je      do_read
        cmp     al,8
        je      do_write
        cmp     al,9
        je      do_write
        cmp     al,6
        je      do_instat

;--- everything else: done, and nothing happened ----------------------
do_done:
        mov     word ptr es:[bx+3],0100h
        jmp     intr_out

;=====================================================================
; INIT.  Print a line, then say where the resident part ends.
;
; The line goes out with AH=09h, which is a DOS call, which is the
; whole point of doing it here.
;=====================================================================
do_init:
        mov     dx,OFFSET msg_hello
        mov     ah,09h
        int     21h

        mov     ax,OFFSET endres                        ; keep this much
        mov     es:[bx+0Eh],ax
        mov     ax,cs
        mov     es:[bx+10h],ax
        mov     byte ptr es:[bx+0Dh],0                  ; no units: character
        mov     word ptr es:[bx+3],0100h
        jmp     intr_out

;=====================================================================
; READ.  Hand over the reply, a byte at a time or in one go - whatever
;        the packet asks for - and start again once it runs out, so
;        that a second TYPE says something rather than nothing.
;=====================================================================
do_read:
        call    build
        mov     cx,es:[bx+12h]          ; bytes wanted
        les     di,dword ptr es:[bx+0Eh]        ; where they go
        push    bx
        xor     dx,dx                   ; bytes actually given
        mov     si,OFFSET reply         ; ...as an address, not an index:
        add     si,[readpos]            ; wasm will not add a constant to
                                        ; OFFSET inside brackets
rd_lp:
        cmp     dx,cx
        jae     rd_end
        mov     ax,si
        sub     ax,OFFSET reply
        cmp     ax,[replylen]
        jae     rd_end
        mov     al,[si]
        mov     es:[di],al
        inc     di
        inc     si
        inc     dx
        jmp     short rd_lp
;--- EXHAUSTED MEANS ZERO BYTES, and only then does it rewind.  The
;--- first version rewound as soon as the text ran out, so TYPE never
;--- saw an end of file and printed the same line until the machine
;--- was switched off - which is a real driver bug and exactly the one
;--- a device that cannot say "no more" always has.
;--- EXHAUSTED STAYS EXHAUSTED.  The first version rewound as soon as
;--- the text ran out, so the device never reached an end of file and
;--- TYPE printed the same line until the machine was switched off.
;--- That is a real driver bug and exactly the one a device with no
;--- way of saying "no more" always has.  A WRITE is what rewinds it,
;--- because a write is what changes the answer.
rd_end:
        sub     si,OFFSET reply
        mov     [readpos],si
        pop     bx
        les     bx,dword ptr [reqoff]   ; ES was the caller's buffer
        mov     es:[bx+12h],dx
        mov     word ptr es:[bx+3],0100h
        jmp     intr_out

;=====================================================================
; WRITE.  Count them.  A device that swallowed its input and reported
;         nothing would pass a test that a broken one also passes.
;
; EXCEPT FOR '!', WHICH IT REFUSES EVERY OTHER TIME, and that is the
; only reason this driver has a failure mode at all: until v0.41 a
; character driver's error status was read out of the packet and
; thrown away, so there was no way to find out whether the kernel
; would do anything with one.  Now dl_out raises a critical error with
; it and acts on the answer, and a driver that always failed could
; only ever test Ignore - a Retry would go round for ever.  Refusing
; alternate attempts means the SAME byte offered twice succeeds the
; second time, so Retry has somewhere to arrive.
;
; The reason it gives is 09h, "printer out of paper", because that is
; the archetype: the error a character device raises that a human can
; actually do something about, which is the whole argument for asking
; rather than failing.
;=====================================================================
do_write:
        mov     word ptr [readpos],0    ; ANY write lets the reply be read
                                        ; again, refused or not - a test
                                        ; that compares the count before
                                        ; and after needs to be able to
                                        ; read it both times
        mov     cx,es:[bx+12h]
        jcxz    wr_count

        push    es                      ; the first byte of the buffer, and
        push    di                      ; les reloads ES over the packet's
        les     di,dword ptr es:[bx+0Eh]
        mov     al,es:[di]
        pop     di
        pop     es
        cmp     al,'!'
        jne     wr_count
        xor     byte ptr [bangfail],1
        cmp     byte ptr [bangfail],0
        je      wr_count                ; the second offer of it goes through
        mov     word ptr es:[bx+12h],0  ; nothing was transferred...
        mov     word ptr es:[bx+3],8109h        ; ...error, done, no paper
        jmp     intr_out

wr_count:
        mov     cx,es:[bx+12h]
        add     [written],cx
        mov     word ptr es:[bx+3],0100h
        jmp     intr_out

;=====================================================================
; INPUT STATUS.  The busy bit means "nothing to read".
;=====================================================================
do_instat:
        mov     word ptr es:[bx+3],0100h
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
; build - the reply text, rebuilt whenever a read starts from zero so
;         that the count in it is the count now.
;=====================================================================
build:
        cmp     word ptr [readpos],0
        jne     b_out
        push    ax
        push    bx
        push    cx
        push    dx
        push    di
        mov     di,OFFSET reply
        mov     si,OFFSET r_head
        call    b_copy
        mov     ax,[written]
        call    b_num
        mov     si,OFFSET r_tail
        call    b_copy
        mov     ax,di
        sub     ax,OFFSET reply
        mov     [replylen],ax
        pop     di
        pop     dx
        pop     cx
        pop     bx
        pop     ax
b_out:
        ret

;--- b_copy - ASCIIZ at SI to DI, DI left past it
b_copy:
        push    ax
bc_lp:
        mov     al,[si]
        or      al,al
        jz      bc_end
        mov     [di],al
        inc     si
        inc     di
        jmp     short bc_lp
bc_end:
        pop     ax
        ret

;--- b_num - AX in decimal at DI, DI left past it.  No leading zeroes,
;--- and zero itself still prints.
b_num:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx,10
        xor     cx,cx
bn_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     bn_div
bn_out:
        pop     ax
        add     al,'0'
        mov     [di],al
        inc     di
        loop    bn_out
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; data - ALL of it below endres, because everything above that address
;        is handed back to the arena the moment INIT returns.
;=====================================================================
msg_hello   db  'TESTDEV.SYS installed - a character device called TESTDEV',13,10,'$'
r_head      db  'TESTDEV: ',0
r_tail      db  ' bytes written to me',13,10,0
written     dw  0
readpos     dw  0
bangfail    db  0                       ; whether the next '!' is refused
            db  0
replylen    dw  0
reply       db  64 dup (0)

endres:

_TD     ENDS
        END     header
