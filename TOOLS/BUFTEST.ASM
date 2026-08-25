;=====================================================================
; BUFTEST.ASM - BUFTEST.COM, the buffer a DOS call is given, measured.
;
; Every pointer that reaches INT 21h came from somebody else, and for
; a DOS box the arithmetic looks safe: a segment:offset stops at
; 10FFEFh and PMDOS.SYS starts at 110000h, so the guest cannot POINT
; at the kernel however hard it tries.
;
; It can ask for FFFFh BYTES to be read into FFFF:FF00, though, and
; then the first sixteen land in the guest's own HMA and the next
; sixty-four kilobytes land in the kernel - with the read reporting
; success, because from inside a copy loop the second half of a buffer
; looks exactly like the first.  The machine dies later and somewhere
; else, and the only evidence is that a file was read.
;
; The other end of the same hole is nearer: 0000:0600 is the thunk's
; register frame and 0000:0800 is the real-mode stack the kernel is
; standing on WHILE IT SERVICES THE CALL, and any program that has
; ever zeroed a segment register by accident has pointed there.
;
; So there are two questions here and they are not the same question:
; is the pointer somewhere the kernel lives, and does the pointer plus
; the length get there.  Eight checks, and the ones that matter fail
; in one of two ways - "FAILED" if the guard answered wrongly, and a
; dead machine if there is no guard at all.
;
; Nothing here is a bug in a real program.  It is what a real program
; does on a bad day, which is the only kind of day a kernel has to
; survive.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_BT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_BT, DS:_BT, ES:_BT, SS:_BT
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results

;=====================================================================
; a file to read, so that 3Fh has something to be refused about
;=====================================================================
        mov     dx,OFFSET fname
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      no_file
        mov     bx,ax

        ; EIGHT KILOBYTES, AND THE SIZE IS THE POINT.  Check 3 asks
        ; for FFFFh bytes into FFFF:FF00, which is 1110h bytes short
        ; of PMDOS.SYS - so a file shorter than that overruns nothing,
        ; the read returns happily, and an unguarded kernel passes a
        ; test it should have died on.  A short test file would have
        ; made this whole program agree with the bug.
        mov     cx,128
padlp:
        push    cx
        mov     dx,OFFSET pattern
        mov     cx,64
        mov     ah,40h
        int     21h
        pop     cx
        loop    padlp
        mov     ah,3Eh
        int     21h

        mov     dx,OFFSET fname
        mov     ax,3D00h
        int     21h
        jc      no_file
        mov     [fh],ax

;=====================================================================
; 1 - 40h whose SOURCE is the kernel.  The safe one: at worst an
;     unguarded kernel prints sixteen bytes of its own thunk frame to
;     the screen, which is a leak and not a crash, and it is first
;     because it is the only check here that can be run without
;     risking the machine.
;=====================================================================
        mov     dx,OFFSET t1
        call    puts
        mov     bx,1
        mov     cx,16
        push    ds
        xor     ax,ax
        mov     ds,ax
        mov     dx,0600h
        mov     ah,40h
        int     21h
        pop     ds                      ; a pop does not touch the flags
        call    grab
        call    want5

;=====================================================================
; 2 - 3Fh whose DESTINATION is the kernel.  Unguarded, this writes
;     sixteen bytes of a file over the register frame the PM->RM thunk
;     uses, and the machine dies at the next one with an instruction
;     pointer in the middle of the vector table.
;=====================================================================
        mov     dx,OFFSET t2
        call    puts
        mov     bx,[fh]
        mov     cx,16
        push    ds
        xor     ax,ax
        mov     ds,ax
        mov     dx,0600h
        mov     ah,3Fh
        int     21h
        pop     ds
        call    grab
        call    want5

;=====================================================================
; 3 - THE ONE A POINTER CHECK CANNOT CATCH.  FFFF:FF00 is a perfectly
;     ordinary address that the guest is entitled to use; it is the
;     COUNT that walks out of the megabyte and into PMDOS.SYS.  Any
;     guard that looks only at pointers passes this one.
;=====================================================================
        mov     dx,OFFSET t3
        call    puts
        mov     bx,[fh]
        mov     cx,0FFFFh
        push    ds
        mov     ax,0FFFFh
        mov     ds,ax
        mov     dx,0FF00h
        mov     ah,3Fh
        int     21h
        pop     ds
        call    grab
        call    want5

;=====================================================================
; 4 - 1Ah pointed at the kernel is refused, AND THE OLD DTA SURVIVES.
;     Half a refusal is worse than none: a call that says "denied" and
;     has already moved the transfer address leaves every later 4Eh
;     writing somewhere nobody chose.
;=====================================================================
        mov     dx,OFFSET t4
        call    puts
        mov     dx,80h                  ; a known DTA to start from
        mov     ah,1Ah
        int     21h

        ; ...and ASK where that landed rather than assuming.  PM-DOS
        ; holds the DTA flat and normalises it on the way out, so 2Fh
        ; answers PSP+8:0000 and not PSP:0080 - the same address, and
        ; not the same two numbers.  A test that knows the answer in
        ; advance is testing its own arithmetic.
        mov     ah,2Fh
        int     21h
        mov     ax,es
        mov     [dtaseg],ax
        mov     [dtaoff],bx

        push    ds
        xor     ax,ax
        mov     ds,ax
        mov     dx,0600h
        mov     ah,1Ah
        int     21h
        pop     ds
        call    grab
        cmp     byte ptr [savcf],0
        je      f4bad
        cmp     word ptr [savax],5
        jne     f4bad
        mov     ah,2Fh                  ; and where is the DTA now?
        int     21h
        mov     ax,es
        cmp     ax,[dtaseg]
        jne     f4bad
        cmp     bx,[dtaoff]
        jne     f4bad
        call    good
        jmp     short f5
f4bad:
        call    bad

;=====================================================================
; 5 - a DTA sixteen bytes below the kernel is ACCEPTED.  It has to be:
;     one byte there is one byte of the guest's own HMA, and a guard
;     that refuses an address for what some later call might do with
;     it is a guard that breaks working programs.
;=====================================================================
f5:
        mov     dx,OFFSET t5
        call    puts
        push    ds
        mov     ax,0FFFFh
        mov     ds,ax
        mov     dx,0FFF0h
        mov     ah,1Ah
        int     21h
        pop     ds
        call    grab
        call    wantok

;=====================================================================
; 6 - ...and the 4Eh that would write forty-three bytes there is not.
;     THIS is where the length is known, and it is a different call
;     from the one that was given the pointer - which is the whole
;     reason the measurement cannot live in the bridge.
;=====================================================================
        mov     dx,OFFSET t6
        call    puts
        mov     dx,OFFSET pat_all
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        call    grab
        call    want5

;=====================================================================
; 7 - the DTA put back, and an ordinary find works again.  A guard
;     that leaves the machine unable to list a directory has not
;     fixed anything.
;=====================================================================
        mov     dx,OFFSET t7
        call    puts
        mov     dx,80h
        mov     ah,1Ah
        int     21h
        mov     dx,OFFSET pat_all
        xor     cx,cx
        mov     ah,4Eh
        int     21h
        call    grab
        call    wantok

;=====================================================================
; 8 - and the file is still at byte zero with its contents intact.
;     Two reads were refused above; a refusal that had half happened
;     would show up here as a short read or a moved file pointer, and
;     nowhere else at all.
;=====================================================================
        mov     dx,OFFSET t8
        call    puts
        mov     bx,[fh]
        mov     cx,16
        mov     dx,OFFSET rbuf
        mov     ah,3Fh
        int     21h
        call    grab
        cmp     byte ptr [savcf],0
        jne     f8bad
        cmp     word ptr [savax],16
        jne     f8bad
        mov     si,OFFSET pattern
        mov     di,OFFSET rbuf
        mov     cx,16
f8lp:
        mov     al,[si]
        cmp     al,[di]
        jne     f8bad
        inc     si
        inc     di
        loop    f8lp
        call    good
        jmp     short done
f8bad:
        call    bad

done:
        mov     bx,[fh]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET fname
        mov     ah,41h
        int     21h

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
        mov     ax,4C00h
        int     21h

no_file:
        mov     dx,OFFSET msg_nofile
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; grab - the answer, before anything can disturb it.  AX and the carry
;        flag are the whole of a DOS return value and both are gone
;        the moment the next instruction runs.
;=====================================================================
grab:
        mov     [savax],ax
        setc    byte ptr [savcf]
        ret

;--- want5 - refused, with "access denied".  Real DOS has no error for
;--- this because on a real machine there is nothing to protect, so
;--- the code is an invention; 5 is the one the kernel invented.
want5:
        cmp     byte ptr [savcf],0
        je      w5bad
        cmp     word ptr [savax],5
        jne     w5bad
        call    good
        ret
w5bad:
        call    bad
        ret

;--- wantok - and the other half of every guard: the calls that were
;--- always going to work still do.
wantok:
        cmp     byte ptr [savcf],0
        jne     wokbad
        call    good
        ret
wokbad:
        call    bad
        ret

;--- good / bad - the verdict, and one character of it in a line that
;--- survives the screen scrolling.
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
fh          dw  0
savax       dw  0
savcf       db  0
dtaseg      dw  0
dtaoff      dw  0
fails       db  0
resptr      dw  0

fname       db  'BUFTEST.TMP',0
pat_all     db  '*.*',0
pattern     db  'BUFTEST pattern 0123456789abcdef0123456789abcdef0123456789ab'
            db  0Dh,0Ah,1Ah,0
rbuf        db  32 dup (0)
results     db  16 dup ('$')

msg_hi      db  13,10,'BUFTEST - what a DOS call may be told to write on',13,10,13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_res     db  13,10,'tests: $'
msg_fails   db  '  failures: $'
msg_nofile  db  'cannot make BUFTEST.TMP - is the disk writable?',13,10,'$'

t1          db  '  1 40h reading the thunk frame       $'
t2          db  '  2 3Fh writing the thunk frame       $'
t3          db  '  3 3Fh whose COUNT reaches the kernel$'
t4          db  '  4 1Ah refused, old DTA intact       $'
t5          db  '  5 a DTA in the HMA is allowed       $'
t6          db  '  6 ...but 4Eh will not fill it       $'
t7          db  '  7 DTA back, ordinary find works     $'
t8          db  '  8 file still at 0 and unchanged     $'

_BT     ENDS
        END     start
