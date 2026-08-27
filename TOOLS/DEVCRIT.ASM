;=====================================================================
; DEVCRIT.ASM - DEVCRIT.EXE, the OTHER half of INT 24h.
;
; A critical error on a DISK has worked since v0.20 and R3TEST's
; fourteenth check has exercised the call-out ever since.  A critical
; error on a CHARACTER DEVICE had nothing to raise it: CON and NUL
; cannot fail, PRN and AUX are reached through a BIOS that answers by
; not answering, and a DEVICE= driver's error status was read out of
; the request packet and thrown away.  This is what tests the path
; v0.41 built, and it needs TESTDEV.SYS loaded, because TESTDEV is the
; only device in the machine that can be made to say no.
;
; TESTDEV REFUSES A '!' EVERY OTHER TIME, with "printer out of paper".
; That alternation is the whole reason Retry can be tested at all - a
; driver that always failed would send a Retry round for ever - and it
; is why the checks below come in the order they do, because each one
; leaves the driver's latch where the next one expects it.
;
; IT IS A NATIVE PROGRAM AND IT HAS TO BE, which is worth a paragraph
; because the first version of it was not and hung the machine.  A
; 16-bit guest's 21h/25h writes the REAL-MODE IVT at 0:0, which is
; where the monitor reflects an untrapped interrupt - and crit_raise
; executes INT 24h from 32-bit kernel code, which goes through the
; IDT.  Those are two different tables.  A guest's handler is
; therefore not reached at all today (there is a roadmap entry for
; it), COMMAND's own handler answers instead, and COMMAND's handler
; asks the user - so a test with nobody at the keyboard waits for
; ever.  A native program's 25h installs the gate the kernel actually
; enters, which is the mechanism R3TEST proves and this one uses.
;
; Six checks:
;
;   1  TESTDEV is loaded and opens
;   2  writing '!' calls this program's INT 24h handler
;   3  ...with bit 7 of AH set, which is DOS for "not a disk"
;   4  ...and error 09h, which is what the driver actually said
;   5  answering Retry gets the byte through - the counter moves
;   6  answering Ignore does not - the counter stands still
;
; Checks 5 and 6 are the ones that mean something, and they are
; measured rather than reported: TESTDEV's own reply text says how
; many bytes it has been given, and the test reads it before and after
; and COMPARES THE TWO STRINGS.  A byte that went through changes the
; count; one that was ignored does not.  No number is parsed and no
; answer is taken on trust.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_DC     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_DC, DS:_DC, ES:_DC, SS:_DC
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET dc_entry>,<0>,4096

RDBUF_MAX   EQU 96

dc_entry:
        mov     edx,OFFSET msg_hi
        call    puts

;=====================================================================
; 1 - the device.  Opened for writing, which is what the rest of this
;     program does to it.
;=====================================================================
        mov     edx,OFFSET msg_t1
        call    puts
        mov     edx,OFFSET dev_name
        mov     ax,3D02h
        int     21h
        jc      t1bad
        mov     [devhdl],eax
        call    good
        jmp     t2
t1bad:
        call    bad
        mov     edx,OFFSET msg_nodev
        call    puts
        jmp     wrapup
t2:

;=====================================================================
; hook INT 24h.  The handler answers whatever [answer] holds and
; records what it was shown.  It goes away again before this program
; ends - the kernel restores the PSP's copy at 4Ch, but a program that
; leaves a handler pointing into a slot it no longer owns has already
; lost the argument.
;=====================================================================
        mov     ax,3524h
        int     21h
        mov     [old24],ebx
        mov     edx,OFFSET my24
        mov     ax,2524h
        int     21h

;=====================================================================
; 2, 3, 4 - one refused write, and what the handler was told.
;
; Retry is the answer, so the byte goes through on the second offer
; and the write returns rather than looping.  If the kernel ignored
; the driver's status the handler is never called at all, and check 2
; is the one that says so.
;=====================================================================
        call    readcount               ; the count before, as text
        mov     esi,OFFSET rdbuf
        mov     edi,OFFSET before
        call    copybuf

        mov     byte ptr [answer],CERR_RETRY
        mov     byte ptr [called],0
        call    writebang

        mov     edx,OFFSET msg_t2
        call    puts
        cmp     byte ptr [called],0
        je      t2bad
        call    good
        jmp     short t3
t2bad:
        call    bad
t3:
        mov     edx,OFFSET msg_t3
        call    puts
        test    byte ptr [sawah],CE_CHARDEV
        jz      t3bad
        call    good
        jmp     short t4
t3bad:
        call    bad
t4:
        mov     edx,OFFSET msg_t4
        call    puts
        cmp     byte ptr [sawdi],CERR_PAPER
        jne     t4bad
        call    good
        jmp     short t5
t4bad:
        call    bad
t5:

;=====================================================================
; 5 - and the retried byte actually arrived
;=====================================================================
        mov     edx,OFFSET msg_t5
        call    puts
        call    readcount
        mov     esi,OFFSET rdbuf
        mov     edi,OFFSET before
        call    samebuf
        je      t5bad                   ; unchanged means it never got there
        mov     esi,OFFSET rdbuf        ; and this is the new baseline
        mov     edi,OFFSET before
        call    copybuf
        call    good
        jmp     short t6
t5bad:
        call    bad
t6:

;=====================================================================
; 6 - Ignore, and the byte is lost.
;
; TESTDEV's latch is back where check 2 found it, so this '!' is
; refused as well - which is what makes the two checks a pair rather
; than a sequence that happens to work once.
;=====================================================================
        mov     edx,OFFSET msg_t6
        call    puts
        mov     byte ptr [answer],CERR_IGNORE
        mov     byte ptr [called],0
        call    writebang
        cmp     byte ptr [called],0
        je      t6bad                   ; it was not even refused
        call    readcount
        mov     esi,OFFSET rdbuf
        mov     edi,OFFSET before
        call    samebuf
        jne     t6bad                   ; it got through, and should not have
        call    good
        jmp     short t7
t6bad:
        call    bad
t7:

;--- the handler back, and the device closed -------------------------
        mov     edx,[old24]
        mov     ax,2524h
        int     21h
        mov     ebx,[devhdl]
        mov     ah,3Eh
        int     21h

wrapup:
        mov     edx,OFFSET msg_res
        call    puts
        mov     edx,OFFSET results
        call    puts
        mov     edx,OFFSET msg_fails
        call    puts
        movzx   eax,byte ptr [fails]
        call    putdec
        mov     edx,OFFSET msg_crlf
        call    puts
        movzx   eax,byte ptr [fails]
        mov     ah,4Ch
        int     21h

;=====================================================================
; my24 - the critical error handler.
;
; It runs at ring 3, on this program's own stack, entered from ring 0
; several calls down inside the device layer, and it ends with the
; IRETD its specification demands.  The answer goes back in EAX.
;
; Nothing here calls INT 21h.  A handler may - COMMAND's writes its
; message and reads its key that way - but this one has nothing to
; say, and the DOS call it interrupted is still open.
;=====================================================================
my24:
        mov     byte ptr [called],1
        mov     byte ptr [sawah],ah
        mov     eax,edi                 ; the code, whose low byte is all
        mov     byte ptr [sawdi],al     ; of it
        movzx   eax,byte ptr [answer]
        iretd

;---------------------------------------------------------------------
; writebang - offer the device a single '!'.
;
; 40h has no error return for a character device and never had one,
; which is the whole reason the critical error exists: what comes back
; here says nothing about whether the byte arrived.  That is what
; checks 5 and 6 are for.
;---------------------------------------------------------------------
writebang:
        pushad
        mov     ebx,[devhdl]
        mov     ecx,1
        mov     edx,OFFSET bang
        mov     ah,40h
        int     21h
        popad
        ret

;---------------------------------------------------------------------
; readcount - TESTDEV's reply text into rdbuf, NUL terminated.
;
; Opened fresh each time: the driver rewinds its reply on every write,
; and a handle that has already read to the end would go on reporting
; end of file.
;---------------------------------------------------------------------
readcount:
        pushad
        mov     byte ptr [rdbuf],0
        mov     edx,OFFSET dev_name
        mov     ax,3D00h
        int     21h
        jc      rc_out
        mov     ebx,eax
        mov     ecx,RDBUF_MAX - 1
        mov     edx,OFFSET rdbuf
        mov     ah,3Fh
        int     21h
        jc      rc_close
        mov     esi,OFFSET rdbuf
        add     esi,eax
        mov     byte ptr [esi],0
rc_close:
        mov     ah,3Eh
        int     21h
rc_out:
        popad
        ret

;--- copybuf / samebuf - ESI and EDI, ASCIIZ.  samebuf sets ZF if they
;--- are the same string.
copybuf:
        push    eax
        push    esi
        push    edi
cb_lp:
        mov     al,[esi]
        mov     [edi],al
        inc     esi
        inc     edi
        or      al,al
        jnz     cb_lp
        pop     edi
        pop     esi
        pop     eax
        ret

samebuf:
        push    eax
        push    esi
        push    edi
sb_lp:
        mov     al,[esi]
        cmp     al,[edi]
        jne     sb_out
        or      al,al
        jz      sb_out                  ; both ended, and ZF is set
        inc     esi
        inc     edi
        jmp     short sb_lp
sb_out:
        pop     edi
        pop     esi
        pop     eax
        ret

;---------------------------------------------------------------------
good:
        push    edx
        mov     dl,'.'
        call    tally
        mov     edx,OFFSET msg_ok
        call    puts
        pop     edx
        ret
bad:
        push    edx
        mov     dl,'X'
        call    tally
        mov     edx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     edx
        ret
tally:
        push    ebx
        mov     ebx,[resptr]
        mov     [ebx],dl
        inc     ebx
        mov     byte ptr [ebx],'$'
        mov     [resptr],ebx
        pop     ebx
        ret

puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret


;--- putdec - EAX in decimal, no padding ----------------------------
putdec:
        pushad
        mov     edi,OFFSET decbuf + 10
        mov     byte ptr [edi],'$'
        mov     ebx,10
pd_lp:
        xor     edx,edx
        div     ebx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        or      eax,eax
        jnz     pd_lp
        mov     edx,edi
        mov     ah,09h
        int     21h
        popad
        ret

;=====================================================================
; data
;=====================================================================
devhdl      dd  0
old24       dd  0
resptr      dd  OFFSET results
called      db  0
sawah       db  0
sawdi       db  0
answer      db  0
fails       db  0

bang        db  '!'
dev_name    db  'TESTDEV',0
decbuf      db  12 dup (0)
rdbuf       db  RDBUF_MAX dup (0)
before      db  RDBUF_MAX dup (0)

msg_hi      db  'DEVCRIT - a character device that says no, and INT 24h',13,10,13,10,'$'
msg_nodev   db  13,10,'TESTDEV.SYS is not loaded.  Put DEVICE=\TESTDEV.SYS in',13,10
            db  'CONFIG.SYS and boot again: there is nothing else in this',13,10
            db  'machine that can be made to fail on purpose.',13,10,'$'

msg_t1      db  ' 1 TESTDEV is loaded and opens       ','$'
msg_t2      db  ' 2 a refused write reaches INT 24h   ','$'
msg_t3      db  ' 3 AH bit 7 says it was not a disk   ','$'
msg_t4      db  ' 4 the code is the driver.s own 09h  ','$'
msg_t5      db  ' 5 Retry gets the byte through       ','$'
msg_t6      db  ' 6 Ignore loses it, as it should     ','$'

msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_res     db  13,10,'result: ','$'
msg_fails   db  13,10,'failures: ','$'
msg_crlf    db  13,10,'$'

results     db  16 dup(0)

_DC     ENDS
        END
