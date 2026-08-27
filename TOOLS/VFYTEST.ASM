;=====================================================================
; VFYTEST.ASM - VFYTEST.EXE, does VERIFY verify?
;
; 21h/2Eh has set a flag since v0.31 and 54h has reported it, and for
; eleven releases nothing read it.  That was said plainly in the
; source rather than hidden, but "the flag is remembered" and "writes
; are verified" are different claims, and a program that turns VERIFY
; on and copies a file is asking for the second one.
;
; TESTING IT NEEDS A SECTOR THAT LIES, and nothing in this machine
; could be made to.  A disk that reads back what was written is what
; every disk here does; a disk that does not is a disk with a fault
; in it, and there is no way to ask for one.  So TESTBLK.SYS - the RAM
; disk that exists to be a block driver - has eight bytes of test
; apparatus in it: a sector written with the signature 'POISON!!' at
; the front reads back ONCE with a byte changed, and then behaves.
; The data on the disk is what was written; only the reading of it,
; once, is wrong.  That is precisely the fault VERIFY exists to catch
; and precisely the one nothing else can produce.
;
; So this program needs TESTBLK.SYS loaded, and says so if it is not.
; It finds the drive by asking the kernel which of them is served by a
; loaded block driver (F1h AL=6, DRVF_DRIVER), rather than by knowing
; a letter - the letter is different on the floppy build and the hard
; disk build, and a test that knows one of them passes on one machine.
;
; Eight checks:
;
;   1  a drive served by a block driver exists
;   2  2Eh and 54h agree, both ways
;   3  a poisoned write with VERIFY OFF succeeds - the flag is what
;      makes the difference, not the poison
;   4  the sector on the disk really does hold what was written
;   5  a poisoned write with VERIFY ON FAILS
;   6  ...and the disk still holds what was written, because the write
;      happened: a verify failure is a report, not a rollback
;   7  an unpoisoned write with VERIFY ON succeeds
;   8  and a file written with VERIFY ON reads back correctly, which
;      is the filesystem's path through the same code
;
; Check 5 is the one the other seven exist to make believable.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_VF     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_VF, DS:_VF, ES:_VF, SS:_VF
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET vf_entry>,<0>,4096

TESTSEC         EQU     32              ; a data sector of the RAM disk,
                                        ; well clear of its FAT and root
FILEBYTES       EQU     3000            ; enough to be more than one
                                        ; cluster on anything

vf_entry:
        mov     edx,OFFSET msg_hi
        call    puts

        ; INT 24h IS OURS, and it has to be.  Half of what this program
        ; does is make writes fail on purpose, and a critical error that
        ; reaches COMMAND's handler puts "Abort, Retry, Fail?" on the
        ; screen and waits for a key - which with nobody at the keyboard
        ; is a test that never ends.  Answering FAIL silently is what
        ; every disk utility of the era did, and for this reason.
        mov     edx,OFFSET my24
        mov     ax,2524h
        int     21h

;=====================================================================
; 1 - which drive is the block driver's?
;=====================================================================
        mov     edx,OFFSET msg_t1
        call    puts
        xor     ebx,ebx                 ; EBX = 0-based drive
t1_lp:
        cmp     ebx,LASTDRIVE
        jae     t1_no
        mov     edx,ebx                 ; F1h AL=6 is 0-BASED, unlike AL=1
                                        ; and AL=2 beside it, and getting
                                        ; that wrong here found the driver
                                        ; and then wrote to the drive below
        mov     edi,OFFSET drv_rec
        mov     eax,0F106h
        int     21h
        jc      t1_next
        test    byte ptr [drv_rec+DRV_FLAGS],DRVF_PRESENT
        jz      t1_next
        test    byte ptr [drv_rec+DRV_FLAGS],DRVF_DRIVER
        jnz     t1_found
t1_next:
        inc     ebx
        jmp     short t1_lp
t1_no:
        call    bad
        mov     edx,OFFSET msg_nodrv
        call    puts
        jmp     wrapup
t1_found:
        mov     [tstdrv],ebx
        mov     eax,ebx
        add     al,'A'
        mov     [msg_which+11],al
        call    good
        mov     edx,OFFSET msg_which
        call    puts

;=====================================================================
; 2 - the flag, both ways.  It has always worked; it is here because a
;     test of what VERIFY DOES should start by proving that the thing
;     being switched is the thing being read.
;=====================================================================
        mov     edx,OFFSET msg_t2
        call    puts
        mov     ax,2E01h
        int     21h
        mov     ah,54h
        int     21h
        cmp     al,1
        jne     t2_bad
        mov     ax,2E00h
        int     21h
        mov     ah,54h
        int     21h
        or      al,al
        jne     t2_bad
        call    good
        jmp     short t3
t2_bad:
        call    bad
t3:

;=====================================================================
; 3 - poisoned, VERIFY off.  This must SUCCEED, or check 5 proves
;     nothing: a write that fails whatever the flag says is a write
;     that fails.
;=====================================================================
        mov     edx,OFFSET msg_t3
        call    puts
        mov     ax,2E00h
        int     21h
        call    fillbuf
        mov     eax,TESTSEC
        call    writesec
        jc      t3_bad
        call    good
        jmp     short t4
t3_bad:
        call    bad
t4:

;=====================================================================
; 4 - THE APPARATUS ITSELF, which has to be tested before anything can
;     be tested with it.  The write is over and nothing has read the
;     sector, so the poison is still armed: this read must come back
;     DIFFERENT, and the one after it must come back RIGHT.  A lie
;     told once, and a disk that still holds what it was given.
;
;     If this check fails, check 5 means nothing - it would be a
;     verify that passed because there was nothing to catch.
;=====================================================================
        mov     edx,OFFSET msg_t4
        call    puts
        mov     eax,TESTSEC
        call    readsec
        jc      t4_bad
        call    cmpbuf
        jnc     t4_bad                  ; it agreed: the poison did nothing
        mov     eax,TESTSEC
        call    readsec
        jc      t4_bad
        call    cmpbuf
        jc      t4_bad                  ; and now it must agree again
        call    good
        jmp     short t5
t4_bad:
        call    bad
t5:

;=====================================================================
; 5 - THE CHECK.  Poisoned, VERIFY on: the write must be reported as
;     a failure, because the sector cannot be shown to hold what it
;     was given.
;
; 26h reports its own errors rather than raising a critical error -
; absolute disk I/O always has, and SYS and FORMAT would rather hear
; "that sector would not write" than have the shell ask the user - so
; this is CF and an error code and nothing appears on the screen.
;=====================================================================
        mov     edx,OFFSET msg_t5
        call    puts
        mov     ax,2E01h
        int     21h
        call    fillbuf
        mov     eax,TESTSEC
        call    writesec
        jnc     t5_bad
        call    good
        jmp     short t6
t5_bad:
        call    bad
t6:

;=====================================================================
; 6 - and the sector STILL holds what was written.  A verify failure
;     says the write cannot be confirmed; it does not undo it, and
;     nothing in DOS ever pretended it did.
;=====================================================================
        mov     edx,OFFSET msg_t6
        call    puts
        mov     ax,2E00h
        int     21h
        mov     eax,TESTSEC
        call    readsec
        jc      t6_bad
        call    cmpbuf
        jc      t6_bad
        call    good
        jmp     short t7
t6_bad:
        call    bad
t7:

;=====================================================================
; 7 - clean sector, VERIFY on.  The read-back agrees, so the write
;     succeeds - which is the case that had better be the usual one.
;=====================================================================
        mov     edx,OFFSET msg_t7
        call    puts
        mov     ax,2E01h
        int     21h
        call    fillbuf
        mov     byte ptr [secbuf],'-'   ; not the signature any more
        mov     eax,TESTSEC
        call    writesec
        pushfd
        push    eax
        mov     ax,2E00h
        int     21h
        pop     eax
        popfd
        jc      t7_bad
        call    good
        jmp     short t8
t7_bad:
        call    bad
t8:

;=====================================================================
; 8 - the OTHER path through the same code.  26h goes round the
;     filesystem; an ordinary file write goes through it, and the two
;     reach dio_verify by different doors.  Nothing here is poisoned,
;     so what this proves is that a verified filesystem write still
;     writes what it was given - which is what would break if the
;     read-back trod on a register somebody was using.
;=====================================================================
;--- the control, first: the same file with VERIFY OFF ---------------
; Without it check 8 could only ever say "a file did not survive",
; which is a sentence with two halves and this is the one that says
; which half.
        mov     edx,OFFSET msg_t8a
        call    puts
        mov     ax,2E00h
        int     21h
        mov     byte ptr [filename+3],'A'
        call    filetest
        jc      t8a_bad
        call    good
        jmp     short t8b
t8a_bad:
        call    bad
        call    saystep
t8b:
        mov     edx,OFFSET msg_t8
        call    puts
        mov     ax,2E01h
        int     21h
        mov     byte ptr [filename+3],'B'
        call    filetest
        jc      t8_bad2
        call    good
        jmp     short t10
t8_bad2:
        call    bad
        call    saystep
t10:

;=====================================================================
; 10 - AND THE SAME AGAIN ON A REAL DISK, which is what tells a fault
;      in the verify from a fault in the driver path underneath it.
;      The block layer and a loaded block driver are two different
;      routes to a sector and dio_verify is above both of them.
;=====================================================================
        mov     edx,OFFSET msg_t10
        call    puts
        mov     ax,2E01h
        int     21h
        mov     ah,19h                  ; the drive we were started from
        int     21h
        movzx   eax,al
        mov     [tstdrv],eax
        mov     byte ptr [filename+3],'C'
        call    filetest
        pushfd
        push    eax
        mov     ax,2E00h
        int     21h
        pop     eax
        popfd
        jc      t10_bad
        call    good
        jmp     t8_end
t10_bad:
        call    bad
        call    saystep
        jmp     t8_end

;---------------------------------------------------------------------
; filetest - write FILEBYTES to the driver's drive, read them back and
;            compare.  CF set if anything did not survive; [step] says
;            how far it got.
;---------------------------------------------------------------------
filetest:
        mov     eax,[tstdrv]
        add     al,'A'
        mov     [filename],al
        mov     byte ptr [step],'1'     ; which stage of check 8 broke, since
                                        ; "a file did not survive" has five
                                        ; places to have gone wrong in
        mov     edx,OFFSET filename
        xor     ecx,ecx
        mov     ah,3Ch
        int     21h
        jc      t8_bad
        mov     [filehdl],eax

        mov     edi,OFFSET bigbuf       ; a pattern with a position in it,
        xor     ecx,ecx                 ; so a displaced block shows up
t8_fill:
        cmp     ecx,FILEBYTES
        jae     t8_wr
        mov     eax,ecx
        xor     al,ah
        add     al,'A'
        mov     [edi+ecx],al
        inc     ecx
        jmp     short t8_fill
t8_wr:
        mov     byte ptr [step],'2'
        mov     ebx,[filehdl]
        mov     ecx,FILEBYTES
        mov     edx,OFFSET bigbuf
        mov     ah,40h
        int     21h
        jc      t8_close
        mov     byte ptr [step],'3'
        cmp     eax,FILEBYTES
        jne     t8_close

        mov     ebx,[filehdl]
        mov     ah,3Eh
        int     21h
        mov     dword ptr [filehdl],-1

        mov     byte ptr [step],'4'
        mov     edx,OFFSET filename     ; and back again
        mov     ax,3D00h
        int     21h
        jc      t8_bad
        mov     [filehdl],eax
        mov     ebx,eax
        mov     ecx,FILEBYTES
        mov     edx,OFFSET rdbuf2
        mov     ah,3Fh
        int     21h
        jc      t8_close
        mov     byte ptr [step],'5'
        cmp     eax,FILEBYTES
        jne     t8_close
        mov     byte ptr [step],'6'
        mov     esi,OFFSET bigbuf
        mov     edi,OFFSET rdbuf2
        mov     ecx,FILEBYTES
        repe    cmpsb
        je      t8_same
        sub     esi,OFFSET bigbuf + 1   ; WHERE it differs, which is the
        mov     [ftwhere],esi           ; question a byte total cannot answer
        movzx   eax,byte ptr [esi+bigbuf]
        mov     [ftsent],eax
        movzx   eax,byte ptr [esi+rdbuf2]
        mov     [ftgot],eax
        jmp     short t8_close
t8_same:
        mov     byte ptr [ftfail],0
        jmp     short t8_tidy
t8_close:
t8_bad:
        mov     byte ptr [ftfail],1
t8_tidy:
        mov     ebx,[filehdl]
        cmp     ebx,-1
        je      t8_gone
        mov     ah,3Eh
        int     21h
        mov     dword ptr [filehdl],-1
t8_gone:
        mov     edx,OFFSET filename
        mov     ah,41h
        int     21h
        cmp     byte ptr [ftfail],0
        je      ft_ok
        stc
        ret
ft_ok:
        clc
        ret

t8_end:
        mov     ax,2E00h                ; and VERIFY back off, whatever
        int     21h                     ; happened above

;=====================================================================
wrapup:
        mov     edx,OFFSET msg_res
        call    puts
        mov     edx,OFFSET results
        call    puts
        mov     edx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        jne     wr_fail
        mov     edx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h
wr_fail:
        mov     edx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h

; saystep - which stage of check 8 gave up.  1 create, 2 write, 3 the
;           write was short, 4 reopen and read, 5 the read was short,
;           6 the bytes came back different.
saystep:
        push    eax
        push    edx
        mov     al,[step]
        mov     [msg_step+11],al
        mov     edx,OFFSET msg_step
        call    puts
        cmp     byte ptr [step],'6'
        jne     ss_out
        mov     edx,OFFSET msg_at       ; and for a mismatch, where and what
        call    puts
        mov     eax,[ftwhere]
        call    putdec
        mov     edx,OFFSET msg_sent
        call    puts
        mov     eax,[ftsent]
        call    putdec
        mov     edx,OFFSET msg_got
        call    puts
        mov     eax,[ftgot]
        call    putdec
        mov     edx,OFFSET msg_crlf
        call    puts
ss_out:
        cmp     byte ptr [crit_seen],0
        je      ss_done
        mov     edx,OFFSET msg_crit
        call    puts
ss_done:
        pop     edx
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
        call    puts
        popad
        ret

;---------------------------------------------------------------------
; my24 - the critical-error handler.  Fail, always, and remember that
;        it happened so a check can ask.
;---------------------------------------------------------------------
my24:
        mov     byte ptr [crit_seen],1
        mov     al,CERR_FAIL
        iretd

;=====================================================================
; The RAM disk, a sector at a time, through 25h and 26h.
;=====================================================================
writesec:                               ; EAX = sector.  CF on failure
        push    ebx
        push    ecx
        push    edx
        mov     edx,eax
        mov     ecx,1
        mov     ebx,OFFSET secbuf
        mov     eax,[tstdrv]
        int     26h
        pop     edx
        pop     ecx
        pop     ebx
        ret

readsec:                                ; EAX = sector.  CF on failure
        push    ebx
        push    ecx
        push    edx
        mov     edx,eax
        mov     ecx,1
        mov     ebx,OFFSET rdbuf
        mov     eax,[tstdrv]
        int     25h
        pop     edx
        pop     ecx
        pop     ebx
        ret

; fillbuf - secbuf = the poison signature and a pattern behind it
fillbuf:
        pushad
        mov     esi,OFFSET poisonsig
        mov     edi,OFFSET secbuf
        mov     ecx,8
        rep     movsb
        mov     ecx,8
fb_lp:
        cmp     ecx,512
        jae     fb_done
        mov     eax,ecx
        add     al,'a'
        mov     [secbuf+ecx],al
        inc     ecx
        jmp     short fb_lp
fb_done:
        popad
        ret

; cmpbuf - CF set if what was read back differs from what was written
cmpbuf:
        push    ecx
        push    esi
        push    edi
        mov     esi,OFFSET secbuf
        mov     edi,OFFSET rdbuf
        mov     ecx,512
        repe    cmpsb
        pop     edi
        pop     esi
        pop     ecx
        je      cb_same
        stc
        ret
cb_same:
        clc
        ret

;=====================================================================
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

;=====================================================================
msg_hi      db  'VFYTEST - does VERIFY verify?',13,10,13,10,'$'
msg_t1      db  ' 1 a block driver has a drive letter      ','$'
msg_which   db  '    (it is X:)',13,10,'$'
msg_t2      db  ' 2 2Eh and 54h agree, both ways           ','$'
msg_t3      db  ' 3 poisoned write, VERIFY off: succeeds   ','$'
msg_t4      db  ' 4 the poison lies once, and only once    ','$'
msg_t5      db  ' 5 poisoned write, VERIFY ON: FAILS       ','$'
msg_t6      db  ' 6 ...and the write still happened        ','$'
msg_t7      db  ' 7 clean write, VERIFY on: succeeds       ','$'
msg_t8a     db  ' 8 a file, VERIFY OFF, reads back right   ','$'
msg_t8      db  ' 9 the same file, VERIFY ON               ','$'
msg_t10     db  '10 ...and on the boot disk, VERIFY ON     ','$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_step    db  '    (stage X)',13,10,'$'
msg_at      db  '    first difference at byte ','$'
msg_sent    db  ': sent ','$'
msg_got     db  ', got ','$'
msg_crit    db  '    (a critical error was raised)',13,10,'$'
msg_nodrv   db  13,10,'  No drive is served by a loaded block driver.',13,10
            db  '  Uncomment DEVICE=\TESTBLK.SYS in CONFIG.SYS.',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_pass    db  'VFYTEST: all checks passed',13,10,'$'
msg_fail    db  'VFYTEST: FAILURES',13,10,'$'

poisonsig   db  'POISON!!'
filename    db  'X:\VFYTEST.TMP',0

tstdrv      dd  0
filehdl     dd  -1
crit_seen   db  0                       ; the handler above was reached
ftfail      db  0                       ; filetest's answer, kept across the
                                        ; tidying up that follows it
step        db  '0'                      ; how far check 8 got
            db  0,0
ftwhere     dd  0                       ; and where the bytes parted company
ftsent      dd  0
ftgot       dd  0
decbuf      db  12 dup(0)
fails       db  0
            db  0,0,0
resptr      dd  OFFSET results
results     db  16 dup('$')
drv_rec     db  DRV_LEN dup(0)
secbuf      db  512 dup(0)
rdbuf       db  512 dup(0)
bigbuf      db  FILEBYTES dup(0)
rdbuf2      db  FILEBYTES dup(0)

_VF     ENDS
        END
