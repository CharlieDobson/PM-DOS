;=====================================================================
; CDXTEST.ASM - CDXTEST.COM, the MSCDEX API asked the questions a real
;               program asks, from inside a DOS box.
;
; This is a SIXTEEN-BIT program on purpose, and that is the whole
; point of it: the MSCDEX API exists for real-mode software, it is
; answered by DOS\MSCDEX.INC out of the V86 monitor's INT 2Fh chain,
; and a 32-bit native program cannot reach that path at all.  So the
; only place the shim can be tested from is a guest.
;
; Nine checks, in the order a CD-ROM installer would actually make
; them: is a driver there, which version, how many drives and where,
; is THIS letter one of them, is THAT one not, and then two different
; ways of reading the same block off the disc so the answers can be
; compared with each other rather than with a hope.
;
;   1500h  the installation check - and it is BX CHANGING that means
;          "installed", which is why BX goes in as zero
;   150Ch  the version
;   150Dh  the drive letters, one byte each
;   150Bh  the letter 150Dh named - expect ADADh and a yes
;   150Bh  drive C: - expect ADADh and a NO.  A shim that says yes to
;          everything passes every other check in this program
;   1505h  the volume descriptor at block 16, which must say type 1
;          and carry "CD001"
;   1508h  the same block by absolute read, which must be BYTE FOR
;          BYTE what 1505h returned.  Two paths, one answer
;   1502h  the copyright file name out of the descriptor
;   1509h  absolute WRITE, which must be refused
;
; Run it in a DOS box with a disc in the drive.  With no disc the
; drive letter still exists, so 1500h and 150Dh answer and the two
; reads report not-ready - which is itself the right answer and worth
; seeing.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_CX     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_CX, DS:_CX, ES:_CX, SS:_CX
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; 1500h - is there a CD-ROM driver, and how many letters has it?
;
; BX IS SET TO ZERO FIRST AND THAT IS THE TEST.  The call reports
; "installed" by CHANGING BX, so a machine where nothing answers
; leaves the zero in place and the program correctly concludes there
; is no driver.  A program that does not zero it first reads whatever
; it happened to be carrying.
;=====================================================================
        mov     dx,OFFSET msg_1500
        call    puts
        xor     bx,bx
        mov     ax,1500h
        int     2Fh
        mov     [cd_count],bx
        mov     [cd_first],cx
        or      bx,bx
        jnz     c_have
        mov     dx,OFFSET msg_nodrv
        call    puts
        jmp     c_bye
c_have:
        mov     ax,bx
        call    put2
        mov     dx,OFFSET msg_drvfrom
        call    puts
        mov     ax,cx
        add     al,'A'
        mov     dl,al
        call    putc
        mov     dl,':'
        call    putc
        call    ok

;=====================================================================
; 150Ch - the version.  BH.BL, and 2.25 is what to expect.
;=====================================================================
        mov     dx,OFFSET msg_150C
        call    puts
        mov     ax,150Ch
        int     2Fh
        mov     al,bh
        call    put2d
        mov     dl,'.'
        call    putc
        mov     al,bl
        call    put2d
        call    ok

;=====================================================================
; 150Dh - the letters themselves, one byte each.
;
; This call exists because 1500h's "count and first letter" cannot
; describe every arrangement: CDROM= in CONFIG.SYS can put the first
; drive anywhere, so the letters need not be where counting from the
; first one would put them.
;=====================================================================
        mov     dx,OFFSET msg_150D
        call    puts
        mov     bx,OFFSET letters
        mov     ax,150Dh
        int     2Fh
        jc      c_150Dbad
        mov     si,OFFSET letters
        mov     cx,[cd_count]
c_150Done:
        mov     al,[si]
        add     al,'A'
        mov     dl,al
        call    putc
        mov     dl,':'
        call    putc
        mov     dl,' '
        call    putc
        inc     si
        loop    c_150Done
        call    ok
        jmp     short c_0B
c_150Dbad:
        call    bad
        jmp     c_bye

;=====================================================================
; 150Bh - is the letter 150Dh just named one of ours?
;
; BX MUST COME BACK ADADh WHETHER THE ANSWER IS YES OR NO.  The
; signature says "something answered this call at all" and is about
; the driver; AX is the answer to the question that was asked, and
; about the drive.  Two facts, two registers, and a shim that
; conflates them passes this check and fails the next.
;=====================================================================
c_0B:
        mov     dx,OFFSET msg_150B
        call    puts
        movzx   cx,byte ptr [letters]
        mov     ax,150Bh
        int     2Fh
        cmp     bx,0ADADh
        jne     c_0Bbad
        or      ax,ax
        jz      c_0Bbad
        call    ok
        jmp     short c_0Bc
c_0Bbad:
        call    bad

;--- ...and the negative control.  C: is a hard disk here, and a shim
;--- that says yes to every letter would have passed everything above.
c_0Bc:
        mov     dx,OFFSET msg_0Bno
        call    puts
        mov     cx,2                    ; C:
        mov     ax,150Bh
        int     2Fh
        cmp     bx,0ADADh
        jne     c_0Bcbad
        or      ax,ax
        jnz     c_0Bcbad
        call    ok
        jmp     short c_05
c_0Bcbad:
        call    bad

;=====================================================================
; 1505h - read the volume descriptor at block 16.
;
; It must say type 1 and carry "CD001", which is the ISO 9660
; signature: those six bytes are how anything decides a disc is a
; filesystem at all rather than a coaster.
;=====================================================================
c_05:
        mov     dx,OFFSET msg_1505
        call    puts
        movzx   cx,byte ptr [letters]
        xor     dx,dx                   ; the first descriptor
        mov     bx,OFFSET buf1
        mov     ax,1505h
        int     2Fh
        jc      c_05notready
        cmp     al,1
        jne     c_05bad
        mov     si,OFFSET buf1 + 1
        mov     di,OFFSET msg_cd001
        mov     cx,5
        repe    cmpsb
        jne     c_05bad
        call    ok
        jmp     short c_08
c_05notready:
        call    notready
        jmp     c_done                  ; no disc: the reads below cannot say
                                        ; anything and 1502h needs one too
c_05bad:
        call    bad

;=====================================================================
; 1508h - the SAME block, by absolute read, compared byte for byte.
;
; TWO PATHS TO ONE ANSWER, which is worth more than either path
; agreeing with what this program expected.  1505h reads a descriptor
; by index and 1508h reads a sector by number; block 16 is both, so
; the two buffers have to be identical, and if they are not then one
; of the two is addressing the disc wrongly and it does not matter
; which.
;=====================================================================
c_08:
        mov     dx,OFFSET msg_1508
        call    puts
        movzx   cx,byte ptr [letters]
        xor     si,si                   ; SI:DI = block 16
        mov     di,16
        mov     dx,1                    ; one block
        mov     bx,OFFSET buf2
        mov     ax,1508h
        int     2Fh
        jc      c_08bad
        mov     si,OFFSET buf1
        mov     di,OFFSET buf2
        mov     cx,2048
        repe    cmpsb
        jne     c_08diff
        call    ok
        jmp     short c_02
c_08diff:
        mov     dx,OFFSET msg_differ
        call    puts
        jmp     short c_02
c_08bad:
        call    bad

;=====================================================================
; 1502h - the copyright file name out of the descriptor.
;
; Usually empty, and printing an empty field is still the check: the
; call has to come back without an error and with a terminated string
; where the buffer was, and a shim that copied 37 bytes from the wrong
; offset would print the volume identifier or the publisher instead.
;=====================================================================
c_02:
        mov     dx,OFFSET msg_1502
        call    puts
        movzx   cx,byte ptr [letters]
        mov     bx,OFFSET buf2
        mov     ax,1502h
        int     2Fh
        jc      c_02bad
        mov     dl,'['
        call    putc
        mov     si,OFFSET buf2
        mov     cx,37
c_02ch:
        mov     al,[si]
        or      al,al
        jz      c_02end
        cmp     al,' '
        jb      c_02skip
        mov     dl,al
        call    putc
c_02skip:
        inc     si
        loop    c_02ch
c_02end:
        mov     dl,']'
        call    putc
        call    ok
        jmp     short c_09
c_02bad:
        call    bad

;=====================================================================
; 1509h - absolute WRITE, which must be refused.
;
; A check that something is NOT there.  Real MSCDEX refused it on
; every drive anybody ever had, so a shim that quietly succeeded here
; would be claiming a capability no CD-ROM has ever had - and the
; program that believed it would think it had written.
;=====================================================================
c_09:
        mov     dx,OFFSET msg_1509
        call    puts
        movzx   cx,byte ptr [letters]
        xor     si,si
        mov     di,16
        mov     dx,1
        mov     bx,OFFSET buf2
        mov     ax,1509h
        int     2Fh
        jnc     c_09bad
        call    ok
        jmp     short c_done
c_09bad:
        call    bad

c_done:
        mov     dx,OFFSET msg_done
        call    puts
c_bye:
        mov     ax,4C00h
        int     21h

;=====================================================================
; The three verdicts, so that every check reads the same way down the
; screen and a missing one is visible as a gap rather than as prose.
;=====================================================================
ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dx,OFFSET msg_bad
        call    puts
        pop     dx
        inc     byte ptr [failures]
        ret
notready:
        push    dx
        mov     dx,OFFSET msg_notrdy
        call    puts
        pop     dx
        ret

;=====================================================================
; Output helpers, the same ones every 16-bit test in this tree uses.
;=====================================================================
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
crlf:
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
        ret

;--- AL in hex, two digits -------------------------------------------
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

;--- AL in decimal, up to three digits, no padding -------------------
; The version is 2.25 and printing it as 0219h would be reporting the
; bytes rather than the version.
put2d:
        push    ax
        push    bx
        push    cx
        push    dx
        movzx   ax,al
        mov     bx,10
        xor     cx,cx
d_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     d_div
d_put:
        pop     ax
        add     al,'0'
        mov     dl,al
        call    putc
        loop    d_put
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; Data
;=====================================================================
cd_count    dw  0
cd_first    dw  0
failures    db  0
letters     db  16 dup(0)

msg_hi      db  'CDXTEST - the MSCDEX API (INT 2Fh AH=15h), from a DOS box'
            db  13,10,13,10,'$'
msg_crlf    db  13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_notrdy  db  ' not ready - no disc in the drive',13,10,'$'
msg_nodrv   db  'BX came back zero: no CD-ROM driver is installed.',13,10,'$'

msg_1500    db  '1500h  install check:     $'
msg_drvfrom db  ' drive(s), from $'
msg_150C    db  '150Ch  version:           $'
msg_150D    db  '150Dh  drive letters:     $'
msg_150B    db  '150Bh  and it IS a CD:    $'
msg_0Bno    db  '150Bh  C: is NOT a CD:    $'
msg_1505    db  '1505h  descriptor 0:      $'
msg_1508    db  '1508h  same block, absolute: $'
msg_1502    db  '1502h  copyright name:    $'
msg_1509    db  '1509h  write is refused:  $'
msg_differ  db  ' FAILED - the two reads disagree',13,10,'$'
msg_cd001   db  'CD001'
msg_done    db  13,10,'CDXTEST done.',13,10,'$'

buf1        db  2048 dup(0)
buf2        db  2048 dup(0)

_CX     ENDS
        END
