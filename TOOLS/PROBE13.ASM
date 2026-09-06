;=====================================================================
; PROBE13.ASM - PROBE13.COM, the positive control the trace channel
;               never had, and then the measurement it turned into.
;
; RMDBG's disk trace has been silent since v0.50 and the notes said,
; correctly, that this proves nothing: nothing had ever been SEEN to
; write to that gap, so a missing trace was a statement about the
; instrument and not about the machine.  The first version of this
; program settled that in one boot - both writes landed, on the host
; file, at the LBAs the arithmetic names - and turned the question
; into a different one.
;
; BECAUSE THE WRITES STOP.  The first few INT 13h calls after the park
; succeed and the ones after them do not, which is why the trace holds
; an early state and says nothing about the rest of the run.  A count
; is the whole answer here: "the disk does not work after the park"
; and "the disk works three times after the park" are different
; faults, and only the second one is a clue.
;
; SO IT IS A LOOP NOW, on both drives.  81h is MULTI.IMG, the scratch
; disk, where RMDBG's gap is; 80h is the one RMDOS reads its files
; from and the one Windows will use.  Both are written in the gap
; between the master boot record and the first partition, which starts
; at LBA 63 on either.  Every line is one write, one read back, and
; what the BIOS said about each - printed, because a program that
; reports its disk trouble THROUGH the disk has nothing to say when
; the disk is the trouble.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_PB     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_PB, DS:_PB, ES:_PB, SS:_PB
        ORG     0

NTRY    EQU     6                       ; passes per drive
SEC0    EQU     20                      ; first CHS sector used

start:
        mov     dx,OFFSET msg_hi
        call    puts

;--- room for RMDOS to be put somewhere, as PARKEXEC does -------------
        mov     bx,1000h
        mov     ah,4Ah
        int     21h

;=====================================================================
; Phase 1 - PM-DOS answering.  A guest's INT 13h here is trapped by
; the V86 monitor and served by PMIO's own ATA driver, which is a
; different piece of code from the one that answers after the park -
; so this is not merely a control, it is half the comparison.
;=====================================================================
        mov     dx,OFFSET msg_ph1
        call    puts
        call    sayoem
        call    showvecs
        call    diag
        call    dosread
        mov     dl,81h
        call    runs

;=====================================================================
; The park.  The same broadcast Windows makes.
;=====================================================================
        mov     dx,OFFSET msg_call
        call    puts
        push    ds
        push    es
        xor     ax,ax
        mov     es,ax
        xor     bx,bx
        xor     si,si
        mov     ds,ax
        xor     cx,cx
        mov     di,0A03h
        mov     dx,1                    ; bit 0: standard mode
        mov     ax,1605h
        int     2Fh
        mov     word ptr cs:[got_cx],cx
        pop     es
        pop     ds

        smsw    ax
        test    al,1
        jz      pb_parked
        mov     dx,OFFSET msg_nopark
        call    puts
        jmp     pb_end
pb_parked:
        mov     ax,word ptr cs:[got_cx]
        or      ax,ax
        jz      pb_ph2
        mov     dx,OFFSET msg_vetoed
        call    puts
        jmp     pb_end

;=====================================================================
; Phase 2 - RMDOS answering, and INT 13h is the machine's own BIOS
; with no monitor between.  This is the exact path rdbg_flush takes.
;=====================================================================
pb_ph2:
        push    cs
        pop     ds
        push    cs
        pop     es
        mov     dx,OFFSET msg_ph2
        call    puts
        call    sayoem
        call    showvecs
        call    diag
        mov     byte ptr [drive],80h
        mov     si,OFFSET script
sc_next:
        mov     al,[si]
        or      al,al
        jz      sc_done
        mov     byte ptr [op],al
        inc     si
        mov     al,[si]
        mov     byte ptr [sect],al
        inc     si
        push    si
        call    one
        pop     si
        jmp     short sc_next
sc_done:
        call    dosread

pb_end:
        mov     dx,OFFSET msg_bye
        call    puts
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; dosread - the SAME disk, reached the way RMDOS reaches it.
;
; THE CONTRADICTION THIS EXISTS TO SETTLE.  RMDISK.INC reads with
; INT 13h AH=02h, CHS, one sector - the identical call this program
; makes and which comes back AH=80h here - and RMTEST reads a file off
; real FAT16 on the far side of the park and passes.  Both of those
; cannot be true of the same machine, so one of them is not being
; measured properly, and the cheapest way to find out which is to make
; both calls in the same run, seconds apart, and print both answers.
;
; If the file read works, the difference between two INT 13h calls is
; a real difference and it can be diffed.  If it does not, then a test
; that has reported 8 of 8 for two releases has been reading something
; other than the disk.
;---------------------------------------------------------------------
dosread:
        mov     dx,OFFSET msg_dos
        call    puts
        mov     dx,OFFSET fname
        mov     ax,3D00h
        int     21h
        jnc     dr_open
        mov     dx,OFFSET msg_noopen
        call    puts
        call    put4
        call    crlf
        ret
dr_open:
        mov     bx,ax
        call    wipe
        mov     dx,OFFSET buf
        mov     cx,512
        mov     ah,3Fh
        int     21h
        pushf
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        popf
        jnc     dr_got
        mov     dx,OFFSET msg_noread
        call    puts
        call    put4
        call    crlf
        ret
dr_got:
        mov     dx,OFFSET msg_got
        call    puts
        call    put4                    ; bytes actually read
        mov     dx,OFFSET msg_first
        call    puts
        mov     si,OFFSET buf
        mov     cx,6
dr_show:
        push    cx
        mov     al,[si]
        call    put2
        pop     cx
        inc     si
        loop    dr_show
        call    crlf
        ret
;---------------------------------------------------------------------
; bdaline - what the BIOS itself thinks happened.
;
; 0040:0074 is the status it last recorded for a fixed disk, 0040:0075
; is HOW MANY IT BELIEVES ARE THERE, and 0040:008E is the byte its
; IRQ 14 handler sets to say the drive has finished.  Printed AFTER
; each operation, because these three separate the failures that all
; come back as AH=80h: a BIOS that never started (0075 wrong), one
; that started and was not woken (008E untouched), and one that ran
; and failed for a reason it has written down (0074).
;---------------------------------------------------------------------
bdaline:
        push    ds
        push    ax
        mov     dx,OFFSET msg_bda
        call    puts
        xor     ax,ax
        mov     ds,ax
        mov     al,byte ptr ds:[474h]
        call    put2
        push    dx
        mov     dl,'/'
        call    putc
        pop     dx
        mov     al,byte ptr ds:[475h]
        call    put2
        push    dx
        mov     dl,'/'
        call    putc
        pop     dx
        mov     al,byte ptr ds:[48Eh]
        call    put2
        pop     ax
        pop     ds
        ret
;---------------------------------------------------------------------
; diag - the three things the BIOS's disk wait depends on, none of
;        which survives being assumed.
;
; FLAGS, because a wait for an interrupt with IF clear is a hang and
; nothing else.  THE TICK, twice with a delay between, because the
; BIOS times its disk operations against 0040:006C and a counter that
; does not advance turns every timeout into a wait for ever - and
; IRQ 0 is the interrupt everything else here has been assuming.
; And INT 15h AH=90h, the device-wait hook the BIOS calls in the
; middle of INT 13h, taken on its own: RMDOS is on that vector for
; XMS's sake and chains what it does not implement, and "chains" is a
; claim about code that has never been run from this side.
;---------------------------------------------------------------------
diag:
        push    ds
        mov     dx,OFFSET msg_fl
        call    puts
        pushf
        pop     ax
        call    put4

;--- the PIC masks, read rather than assumed.  pic_to_rm sets them on
;--- the way out and it is the only thing that claims to; a mask read
;--- back from the chip is the only version of that claim worth having.
        mov     dx,OFFSET msg_pic
        call    puts
        in      al,21h
        call    put2
        push    dx
        mov     dl,'/'
        call    putc
        pop     dx
        in      al,0A1h
        call    put2

;--- and the tick, with a delay that is actually longer than one.  The
;--- first version of this spun 128K times, which is under a
;--- millisecond on this machine, and reported the same number twice -
;--- which reads exactly like a timer that has stopped.  An
;--- instrument that cannot tell "no change" from "no time" is not
;--- measuring anything.  Two hundred passes of it, and a tick is 55ms.
        mov     dx,OFFSET msg_tick
        call    puts
        xor     ax,ax
        mov     ds,ax
        mov     ax,word ptr ds:[46Ch]
        call    put4
        mov     bx,200
dg_outer:
        mov     cx,0
dg_spin:
        loop    dg_spin
        dec     bx
        jnz     dg_outer
        push    dx
        mov     dl,'/'
        call    putc
        pop     dx
        mov     ax,word ptr ds:[46Ch]
        call    put4
        pop     ds

        mov     dx,OFFSET msg_i15
        call    puts
        mov     ax,9000h
        mov     dl,0Dh                  ; device type: fixed disk
        int     15h
        call    saystat
        call    crlf

;--- and the drive itself, which is the one party to this that has not
;--- been asked.  Everything the BIOS needs is now demonstrably in
;--- place - IF set, the tick advancing, IRQ 14 unmasked, the vectors
;--- all still ROM - and the read still says AH=80h, "the drive did
;--- not become ready".  It is worth considering that the drive did
;--- not become ready.  Alternate status first, at 3F6h, because
;--- reading it does NOT acknowledge the interrupt and so does not
;--- change what the next question would have measured.
;---   80 BSY   40 DRDY   08 DRQ   01 ERR
        mov     dx,OFFSET msg_ata
        call    puts
        mov     dx,3F6h
        in      al,dx
        call    put2
        push    dx
        mov     dl,'/'
        call    putc
        pop     dx
        mov     dx,1F6h                 ; which device is selected
        in      al,dx
        call    put2
        push    dx
        mov     dl,'/'
        call    putc
        pop     dx
        mov     dx,1F1h                 ; and the error register
        in      al,dx
        call    put2
        call    crlf
        ret
;---------------------------------------------------------------------
; showvecs - the vectors the BIOS's own disk code goes through.
;
; THE PARK RESTORES WHERE THE TABLE IS, NOT WHAT IS IN IT.  LIDT puts
; the interrupt descriptor table back at 0000:0000 with a 3FF limit,
; which makes the machine read vectors from the same place a BIOS
; does - and says nothing about whether the entries still point at
; BIOS code.  INT 13h on this machine waits for the drive through
; INT 15h AH=90h and is woken through AH=91h, and 76h is the hard
; disk's own IRQ.  Any one of those still pointing into a kernel that
; has left protected mode and stopped existing is a disk that does not
; work, and it would look exactly like a drive that will not answer.
;---------------------------------------------------------------------
showvecs:
        push    ds
        mov     dx,OFFSET msg_vec
        call    puts
        xor     ax,ax
        mov     ds,ax
        mov     si,13h*4
        call    v1
        mov     si,15h*4
        call    v1
        mov     si,76h*4
        call    v1
        mov     si,08h*4
        call    v1
        mov     si,41h*4
        call    v1
        mov     si,46h*4
        call    v1
        pop     ds
        call    crlf
        ret
v1:
        mov     ax,[si+2]
        call    put4
        push    dx
        mov     dl,':'
        call    putc
        pop     dx
        mov     ax,[si]
        call    put4
        push    dx
        mov     dl,' '
        call    putc
        pop     dx
        ret
;---------------------------------------------------------------------
; one - a single BIOS operation from the script, and what it said.
;
; A SCRIPT AND NOT A LOOP, because the loop could only ever measure
; the pair.  Post-park the write returned, the read timed out and the
; next write hung the machine, and a write-then-read loop cannot say
; whether reads are broken or whether the FIRST WRITE broke them -
; those are different faults and it always ran them in that order.
; Reads first, then a write, then reads again.
;---------------------------------------------------------------------
one:
        mov     dx,OFFSET msg_op
        call    puts
        mov     al,byte ptr [op]
        mov     dl,al
        call    putc
        mov     dx,OFFSET msg_at
        call    puts
        mov     al,byte ptr [sect]
        call    put2

        cmp     byte ptr [op],'x'
        jne     on_notrst
        xor     ah,ah                   ; RESET DISK SYSTEM, on its own
        mov     dl,byte ptr [drive]
        int     13h
        call    saystat
        call    bdaline
        call    crlf
        ret
on_notrst:
        cmp     byte ptr [op],'w'
        jne     on_read
        call    fill
        mov     ah,03h
        jmp     short on_go
on_read:
        call    wipe
        mov     ah,02h
on_go:
        call    dio
        call    saystat
        call    bdaline
        call    crlf
        ret

;---------------------------------------------------------------------
; runs - DL = drive.  NTRY passes, one line each, then a tally.
;---------------------------------------------------------------------
runs:
        mov     byte ptr [drive],dl
        mov     byte ptr [nok],0
        mov     dx,OFFSET msg_drv
        call    puts
        mov     al,byte ptr [drive]
        call    put2
        call    crlf

        mov     byte ptr [pass],0
rn_loop:
        call    trial
        inc     byte ptr [pass]
        cmp     byte ptr [pass],NTRY
        jb      rn_loop

        mov     dx,OFFSET msg_of
        call    puts
        mov     al,byte ptr [nok]
        call    put2
        mov     dx,OFFSET msg_slash
        call    puts
        mov     al,NTRY
        call    put2
        call    crlf
        ret

;---------------------------------------------------------------------
; trial - one write, one read back, one line.
;
; THE BUFFER IS WIPED BETWEEN THE TWO.  A readback into a buffer that
; already holds the pattern agrees with itself whatever the disk did,
; which is the same class of mistake as an instrument whose only ever
; reading is the silence it was built to detect.
;---------------------------------------------------------------------
trial:
        mov     al,byte ptr [pass]
        add     al,SEC0
        mov     byte ptr [sect],al
        mov     dx,OFFSET msg_pass
        call    puts
        mov     al,byte ptr [pass]
        add     al,'0'
        mov     dl,al
        call    putc

        call    fill
        mov     dx,OFFSET msg_w
        call    puts
        mov     ah,03h
        call    dio
        call    saystat

        call    wipe
        mov     dx,OFFSET msg_r
        call    puts
        mov     ah,02h
        call    dio
        call    saystat

;--- and the verdict, which is the only column that has to be read ---
        mov     si,OFFSET buf
        mov     di,OFFSET sig
        mov     cx,7
        cld
        repe    cmpsb
        jne     tr_bad
        mov     al,byte ptr [buf+7]
        cmp     al,byte ptr [pass]
        jne     tr_bad
        mov     dx,OFFSET msg_ok
        call    puts
        inc     byte ptr [nok]
        ret
tr_bad:
        mov     dx,OFFSET msg_bad
        call    puts
        ret

;---------------------------------------------------------------------
; dio - AH = 02h or 03h.  One sector, cylinder 0, head 0, the sector
;       taken from the pass number so that no two passes share one.
;---------------------------------------------------------------------
dio:
        push    bx
        push    cx
        push    dx
        push    es
        push    cs
        pop     es
        mov     bx,OFFSET buf
        mov     al,1
        mov     ch,0
        mov     cl,byte ptr cs:[sect]
        mov     dh,0
        mov     dl,byte ptr cs:[drive]
        int     13h
        pop     es
        pop     dx
        pop     cx
        pop     bx
        ret

;--- fill / wipe ------------------------------------------------------
fill:
        push    ax
        push    cx
        push    si
        push    di
        push    es
        push    cs
        pop     es
        mov     si,OFFSET sig
        mov     di,OFFSET buf
        mov     cx,7
        cld
        rep     movsb
        mov     al,byte ptr [pass]
        stosb
        mov     al,'-'
        mov     cx,512-8
        rep     stosb
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret
wipe:
        push    ax
        push    cx
        push    di
        push    es
        push    cs
        pop     es
        mov     di,OFFSET buf
        mov     al,0
        mov     cx,512
        cld
        rep     stosb
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

;---------------------------------------------------------------------
; saystat - the carry and AH, in that order, because a BIOS that fails
;           without setting carry is a thing that happens here: v0.48
;           found AH=43h returning success and moving nothing.
;---------------------------------------------------------------------
saystat:
        pushf
        popf
        jc      ss_carry
        mov     dl,'.'
        jmp     short ss_pc
ss_carry:
        mov     dl,'C'
ss_pc:
        call    putc
        mov     al,ah
        call    put2
        ret

sayoem:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     ah,30h
        int     21h
        mov     ax,bx
        call    put4
        call    crlf
        pop     dx
        pop     cx
        pop     bx
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
crlf:
        push    ax
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
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
put1:
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1_go
        add     al,7
p1_go:
        mov     dl,al
        call    putc
        pop     ax
        ret

got_cx      dw  0
drive       db  0
op          db  0
sect        db  0
script      db  'r',20, 'r',21, 'r',22, 'w',23, 'r',23, 'r',20, 0
pass        db  0
nok         db  0
sig         db  'PROBE13'

msg_hi      db  13,10,'PROBE13 - how many BIOS disk calls survive the park?',13,10,'$'
msg_ph1     db  13,10,'before the park.  DOS says BX = $'
msg_ph2     db  13,10,'after the park.   DOS says BX = $'
msg_call    db  13,10,'broadcasting 1605h, standard mode...',13,10,'$'
msg_nopark  db  'FAILED: still in protected mode - no handover happened.',13,10,'$'
msg_vetoed  db  'FAILED: vetoed, CX came back non-zero.',13,10,'$'
msg_drv     db  '  drive $'
msg_op      db  '   $'
msg_at      db  ' sector $'
msg_vec     db  '   13/15/76/08/41/46',13,10,'   $'
msg_fl      db  '   flags $'
msg_pic     db  '  pic $'
msg_tick    db  '  tick $'
msg_i15     db  '  i15/90 $'
msg_ata     db  '   ata alt/dev/err $'
msg_bda     db  '  bda $'
msg_dos     db  '   via DOS: $'
msg_noopen  db  'open failed AX=$'
msg_noread  db  'read failed AX=$'
msg_got     db  'read $'
msg_first   db  ' bytes, first $'
fname       db  'C:\PROBE13.COM',0
msg_pass    db  '   pass $'
msg_w       db  '  w $'
msg_r       db  '  r $'
msg_ok      db  '  ok',13,10,'$'
msg_bad     db  '  BAD',13,10,'$'
msg_of      db  '   -> $'
msg_slash   db  ' of $'
msg_crlf    db  13,10,'$'
msg_bye     db  13,10,'done.',13,10,'$'

        ALIGN   16
buf         db  512 dup (0)

_PB     ENDS
        END     start
