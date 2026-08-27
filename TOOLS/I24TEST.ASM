;=====================================================================
; I24TEST.ASM - I24TEST.COM, a DOS BOX'S OWN INT 24h handler.
;
; INT 24h has worked since v0.20 for the caller it was written for and
; for no other.  A native program's 21h/25h installs a protected-mode
; IDT gate, which is the table crit_raise's INT 24h goes through, and
; R3TEST's fourteenth check has proved that path since the ring-3
; release.  A 16-BIT GUEST'S 25h WRITES A DIFFERENT TABLE ENTIRELY -
; the real-mode IVT at 0:0, which is where the monitor reflects an
; untrapped interrupt - and until v0.41 nothing ever read it.  So a
; DOS program that hooked 24h to handle its own disk errors quietly
; got COMMAND's "Abort, Retry, Fail?" prompt instead, and a program
; that meant to fail silently asked the user a question.
;
; It is deliberately a 16-BIT program: a native one cannot exercise
; this path, which is exactly how the hole stayed open.
;
; TESTDEV.SYS IS THE FAILURE, and it is here rather than a dead disk
; because it is the only one that can be RELIED ON.  The obvious
; choice is a read of drive B:, which is what R3TEST uses - but B:
; exists as a letter whether or not the machine has the drive, and on
; a single-floppy machine DOS gives both letters to the one drive, so
; on the floppy build "B:\*.*" reads the boot disk and succeeds.  A
; test whose premise depends on how many drives are fitted is a test
; that passes for the wrong reason somewhere.  TESTDEV refuses a '!'
; every other time, on every machine, always.  Check 8 tries the disk
; anyway and says so when there was nothing to fail.
;
; Eight checks:
;
;   1  25h and 35h round-trip a handler of ours
;   2  a refused write reaches it
;   3  ...exactly once, and not once per attempt
;   4  AH bit 7 is SET: the error was a character device
;   5  DI is the driver's own reason, 09h
;   6  Retry was obeyed - so the byte went on the second offer
;   7  Ignore was obeyed - so the one after it did not
;   8  a disk error reaches it too, where there is a disk that fails
;
; CHECKS 6 AND 7 ARE THE ONES THAT MEAN ANYTHING, and they are a
; counting argument rather than a report.  TESTDEV alternates: the
; first '!' is refused, the next is accepted, and so on.  So three
; rounds of one '!' each, answering Retry, then Ignore, then anything,
; must call the handler exactly ONCE, ONCE and NOT AT ALL.
;
;   If the kernel ignored the answer and always retried, the third
;   round would find the latch set and call the handler again.
;   If it ignored the answer and never retried, the second round would
;   find the latch set and NOT call the handler.
;
; Only obeying both answers produces 1, 1, 0 - which is why the test
; is three rounds and not one.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_IT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_IT, DS:_IT, ES:_IT, SS:_IT
        ORG     0

CE_CHARDEV      EQU     80h             ; AH bit 7: not a disk
CERR_PAPER      EQU     09h             ; printer out of paper
CERR_IGNORE     EQU     0
CERR_RETRY      EQU     1
CERR_FAIL       EQU     3

start:
        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results

;--- the device, before anything else: without it nothing below runs
        mov     dx,OFFSET dev_name
        mov     ax,3D02h
        int     21h
        jc      no_dev
        mov     [devhdl],ax

;=====================================================================
; 1 - the vector goes in and comes back out.
;
; For a guest this is vdm_setvec and vdm_getvec, which read and write
; the real-mode table rather than the kernel's own - a different pair
; of routines from the ones a native program reaches, and the reason
; this check is here and not left to R3TEST.
;=====================================================================
        mov     dx,OFFSET msg_t1
        call    puts
        mov     ax,3524h
        int     21h
        mov     [old24of],bx
        mov     [old24sg],es

        push    ds
        pop     es
        mov     dx,OFFSET my24
        mov     ax,2524h
        int     21h

        mov     ax,3524h
        int     21h
        mov     ax,es
        mov     dx,cs
        cmp     ax,dx
        jne     t1bad
        cmp     bx,OFFSET my24
        jne     t1bad
        call    good
        jmp     short t2
t1bad:
        call    bad
t2:

;=====================================================================
; 2 to 5 - round one.  Answer RETRY, and the byte goes through on the
;          second offer.
;=====================================================================
        mov     byte ptr [answer],CERR_RETRY
        mov     byte ptr [ran],0
        mov     byte ptr [sawah],0
        mov     word ptr [sawdi],0FFFFh
        call    writebang
        mov     al,[ran]
        mov     [ranA],al

        mov     dx,OFFSET msg_t2
        call    puts
        cmp     byte ptr [ranA],0
        je      t2bad
        call    good
        jmp     short t3
t2bad:
        call    bad
t3:
        mov     dx,OFFSET msg_t3
        call    puts
        cmp     byte ptr [ranA],1
        jne     t3bad
        call    good
        jmp     short t4
t3bad:
        call    bad
t4:
        mov     dx,OFFSET msg_t4
        call    puts
        test    byte ptr [sawah],CE_CHARDEV
        jz      t4bad
        call    good
        jmp     short t5
t4bad:
        call    bad
t5:
        mov     dx,OFFSET msg_t5
        call    puts
        cmp     byte ptr [sawdi],CERR_PAPER
        jne     t5bad
        call    good
        jmp     short t6
t5bad:
        call    bad
t6:

;=====================================================================
; 6 - round two.  The latch is back where round one found it ONLY if
;     round one retried, so a handler call here says Retry was obeyed.
;=====================================================================
        mov     dx,OFFSET msg_t6
        call    puts
        mov     byte ptr [answer],CERR_IGNORE
        mov     byte ptr [ran],0
        call    writebang
        mov     al,[ran]
        mov     [ranB],al
        cmp     byte ptr [ranB],1
        jne     t6bad
        call    good
        jmp     short t7
t6bad:
        call    bad
t7:

;=====================================================================
; 7 - round three.  Round two answered Ignore, so it did NOT retry,
;     so the latch is set and this '!' is accepted without a murmur.
;     A handler call here means the Ignore was thrown away.
;=====================================================================
        mov     dx,OFFSET msg_t7
        call    puts
        mov     byte ptr [answer],CERR_IGNORE
        mov     byte ptr [ran],0
        call    writebang
        mov     al,[ran]
        mov     [ranC],al
        cmp     byte ptr [ranC],0
        jne     t7bad
        call    good
        jmp     short t8
t7bad:
        call    bad
t8:

;=====================================================================
; 8 - and a DISK error, if this machine has a drive that will fail.
;
; B: on a machine with two floppy letters and one drive is the same
; drive as A:, and reads perfectly well; on one with a real empty
; second drive, or none, it does not.  Both are ordinary machines, so
; a read that succeeds here is reported as "no failing drive" rather
; than as a failure - the same answer CFGTEST gives when the thing it
; wanted to look at is not installed.
;=====================================================================
        mov     dx,OFFSET msg_t8
        call    puts
        mov     dx,OFFSET dta
        mov     ah,1Ah
        int     21h

        ; The character round's registers are the interesting ones and
        ; the handler only has the one set of variables, so they are
        ; parked before this round overwrites them.
        mov     al,[sawah]
        mov     [sawA],al
        mov     ax,[sawdi]
        mov     [sawdiA],ax

        mov     byte ptr [answer],CERR_FAIL
        mov     byte ptr [ran],0
        mov     byte ptr [sawah],0FFh
        mov     dx,OFFSET path_b
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        jnc     t8_none                 ; the drive read: nothing failed
        cmp     byte ptr [ran],0
        je      t8_none                 ; ...or it failed without asking,
                                        ; which is "invalid drive" and is
                                        ; not a critical error either
        mov     byte ptr [disk8],1
        test    byte ptr [sawah],CE_CHARDEV
        jnz     t8bad                   ; a disk must not claim to be a
        call    good                    ; character device
        jmp     short i24_done
t8bad:
        call    bad
        jmp     short i24_done
t8_none:
        call    skip

i24_done:
;--- the handler back, and the device closed -------------------------
        push    ds
        mov     dx,[old24of]
        mov     ax,[old24sg]
        mov     ds,ax
        mov     ax,2524h
        int     21h
        pop     ds

        mov     bx,[devhdl]
        mov     ah,3Eh
        int     21h

        mov     dx,OFFSET msg_saw
        call    puts
        mov     al,[sawA]
        call    put2
        mov     dx,OFFSET msg_sawdi
        call    puts
        mov     ax,[sawdiA]
        call    put4

        cmp     byte ptr [disk8],0
        je      i24_nodisk
        mov     dx,OFFSET msg_disk
        call    puts
        mov     al,[sawah]
        call    put2
        mov     dx,OFFSET msg_sawdi
        call    puts
        mov     ax,[sawdi]
        call    put4
i24_nodisk:
        mov     dx,OFFSET msg_rounds
        call    puts
        mov     al,[ranA]
        call    put1lo
        mov     al,[ranB]
        call    put1lo
        mov     al,[ranC]
        call    put1lo

        mov     dx,OFFSET msg_res
        call    puts
        mov     dx,OFFSET results
        call    puts
        mov     dx,OFFSET msg_fails
        call    puts
        mov     al,[fails]
        call    put2
        mov     dx,OFFSET msg_crlf
        call    puts

        mov     al,[fails]
        mov     ah,4Ch
        int     21h

no_dev:
        mov     dx,OFFSET msg_nodev
        call    puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; my24 - the handler.
;
; It runs in real mode, in this program's own segment, entered by the
; monitor reflecting INT 24h through the table the 25h above wrote.
; The answer goes back in AL and it ends with the IRET the
; specification demands.
;
; EVERY ACCESS IS cs: BECAUSE DS IS NOT PROMISED.  It happens to
; arrive holding what the program was using - the kernel builds the
; register set out of the guest's own parked context, deliberately, so
; that a handler sees a plausible machine rather than a set of zeroes
; - but "happens to" is not a contract, and a .COM's CS is the one
; segment that always addresses its own data.
;
; It calls no DOS function.  A handler may (real DOS allowed 01h to
; 0Ch) and this one has nothing to say, and the call it interrupted is
; still open.
;=====================================================================
my24:
        inc     byte ptr cs:[ran]
        mov     byte ptr cs:[sawah],ah
        mov     word ptr cs:[sawdi],di
        mov     al,byte ptr cs:[answer]
        iret

;---------------------------------------------------------------------
; writebang - offer the device a single '!'.  40h has no error return
;             for a character device, so what comes back says nothing;
;             the handler count is the whole of the answer.
;---------------------------------------------------------------------
writebang:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx,[devhdl]
        mov     cx,1
        mov     dx,OFFSET bang
        mov     ah,40h
        int     21h
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
good:
        push    dx
        mov     dl,'.'
        call    tally
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dl,'X'
        call    tally
        mov     dx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     dx
        ret
skip:
        push    dx
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_skip
        call    puts
        pop     dx
        ret
tally:
        push    bx
        mov     bx,[resptr]
        mov     [bx],dl
        inc     bx
        mov     byte ptr [bx],'$'
        mov     [resptr],bx
        pop     bx
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
put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
        call    put2
        ret
put2:
        push    ax
        shr     al,4
        call    put1
        pop     ax
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1lo:
        push    ax
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
old24of     dw  0
old24sg     dw  0
sawdi       dw  0
sawdiA      dw  0
devhdl      dw  0
resptr      dw  0
ran         db  0
ranA        db  0
ranB        db  0
ranC        db  0
sawah       db  0
sawA        db  0
disk8       db  0
answer      db  0
fails       db  0

bang        db  '!'
dev_name    db  'TESTDEV',0
path_b      db  'B:\*.*',0
dta         db  48 dup (0)

msg_hi      db  'I24TEST - a DOS box.s own INT 24h handler',13,10,13,10,'$'
msg_nodev   db  'TESTDEV.SYS is not loaded.  Put DEVICE=\TESTDEV.SYS in',13,10
            db  'CONFIG.SYS and boot again: it is the only failure in this',13,10
            db  'machine that happens on every machine.',13,10,'$'

msg_t1      db  ' 1 25h and 35h round-trip our handler','$'
msg_t2      db  ' 2 a refused write reaches it        ','$'
msg_t3      db  ' 3 ...exactly once                   ','$'
msg_t4      db  ' 4 AH bit 7: a character device      ','$'
msg_t5      db  ' 5 DI is the driver.s own 09h        ','$'
msg_t6      db  ' 6 Retry was obeyed                  ','$'
msg_t7      db  ' 7 Ignore was obeyed                 ','$'
msg_t8      db  ' 8 a disk error reaches it too       ','$'

msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_skip    db  'no failing drive',13,10,'$'

msg_saw     db  13,10,'the driver round saw AH=','$'
msg_disk    db  ', the disk round AH=','$'
msg_sawdi   db  ' DI=','$'
msg_rounds  db  ', rounds ','$'
msg_res     db  13,10,'result: ','$'
msg_fails   db  13,10,'failures: ','$'
msg_crlf    db  13,10,'$'

results     db  16 dup(0)

_IT     ENDS
        END     start
