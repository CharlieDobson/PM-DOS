;=====================================================================
; APITEST.ASM - APITEST.COM, the v0.31 API test
;
; A 16-bit DOS program, so everything it calls goes through the VDM
; bridge as well as the kernel - and for this release that matters
; more than usual, because vdm_ptrtab had to grow at the same time as
; the dispatch table and a mismatch between the two is invisible from
; 32-bit code.
;
; Covers what v0.31 added:
;
;   30h / 3306h    both answer 6.22, and answer the SAME thing
;   4400h / 4401h  device info, and raw mode set and read back
;   4406h / 4407h  input and output status, on a file and at its end
;   4408h / 4409h  removable and remote, and a bad drive refused
;   440Bh / 440Eh  retry count, logical drive map
;   5Bh            create-new refuses to overwrite
;   5Ah            a unique name, appended to the caller's buffer
;   60h            TRUENAME of a relative path
;   68h            commit leaves the handle open and the data on disk
;   2Eh / 54h      the verify flag, set and read back
;   05h            printer output does not become a file
;
; Every check prints "ok" or "FAILED", so a clean run has no capitals.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_AT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_AT, DS:_AT, ES:_AT, SS:_AT
        ORG     0

VER_MAJ EQU     6
VER_MIN EQU     22

start:
        ; Clear the screen first.  Not decoration: the output is
        ; eighteen lines and the boot messages are twenty-five, so
        ; without this the top half of the run has scrolled away
        ; before anybody reads it.  INT 10h AH=0 goes through the
        ; VDM's BIOS reflection, which is its own small test.
        mov     ax,0003h
        int     10h

        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; 30h and 3306h - the version, twice, and they must agree
;=====================================================================
        mov     dx,OFFSET t_ver
        call    puts
        mov     ah,30h
        int     21h
        mov     [got_maj],al
        mov     [got_min],ah
        mov     al,[got_maj]
        call    pdec
        mov     dl,'.'
        call    putc
        mov     al,[got_min]
        call    pdec
        cmp     byte ptr [got_maj],VER_MAJ
        jne     ver_bad
        cmp     byte ptr [got_min],VER_MIN
        jne     ver_bad
        call    good
        jmp     short ver_true
ver_bad:
        call    bad

ver_true:
        mov     dx,OFFSET t_vertrue
        call    puts
        mov     ax,3306h
        int     21h
        mov     [got_maj],bl
        mov     [got_min],bh
        mov     al,bl
        call    pdec
        mov     dl,'.'
        call    putc
        mov     al,[got_min]
        call    pdec
        cmp     byte ptr [got_maj],VER_MAJ
        jne     vert_bad
        cmp     byte ptr [got_min],VER_MIN
        jne     vert_bad
        call    good
        jmp     short do_ioctl
vert_bad:
        call    bad

;=====================================================================
; 4400h / 4401h - device information, and raw mode
;
; Handle 1 is standard output.  Run normally it is the console, and
; bit 7 (character device) with bit 1 (is the console output) is what
; it must say; run with output redirected it is a file and bit 7 is
; clear, which is the whole reason the call exists - so the test
; checks bit 7 against what 4400h ALREADY said rather than assuming.
;=====================================================================
do_ioctl:
        mov     dx,OFFSET t_ioget
        call    puts
        mov     bx,1
        mov     ax,4400h
        int     21h
        jc      iog_bad
        mov     [devinfo],dx
        mov     ax,dx
        call    put4
        test    dx,0080h                ; a character device?
        jz      iog_file
        test    dx,0002h                ; then it must be the console out
        jz      iog_bad
iog_file:
        call    good
        jmp     short io_raw
iog_bad:
        call    bad

;--- raw mode, on and off again --------------------------------------
; Only a character device can be set, so this is skipped outright when
; stdout has been redirected to a file - "> APITEST.OUT" is a fair way
; to run this program and a skipped test is not a failed one.
io_raw:
        test    word ptr [devinfo],0080h
        jnz     io_raw_do
        mov     dx,OFFSET t_ioskip
        call    puts
        jmp     io_instat
io_raw_do:
        mov     dx,OFFSET t_ioraw
        call    puts
        mov     bx,1
        mov     dx,[devinfo]
        or      dx,0020h                ; binary
        and     dx,00FFh                ; DH must be zero
        mov     ax,4401h
        int     21h
        jc      ior_bad

        mov     bx,1                    ; and does 4400h agree?
        mov     ax,4400h
        int     21h
        jc      ior_bad
        test    dx,0020h
        jz      ior_bad

        mov     bx,1                    ; put it back, or the prompt
        mov     dx,[devinfo]            ; stops echoing for good
        and     dx,00FFh
        and     dx,NOT 0020h
        mov     ax,4401h
        int     21h
        jc      ior_bad

        mov     bx,1
        mov     ax,4400h
        int     21h
        test    dx,0020h
        jnz     ior_bad
        call    good
        jmp     short io_instat
ior_bad:
        call    bad

;=====================================================================
; 4406h / 4407h - is there input, is there room for output
;
; Done on a FILE, where the answer is knowable: a file just written and
; seeked back to the start has input, and the same file at its end has
; not.  On the console the answer depends on whether somebody is
; leaning on the keyboard.
;=====================================================================
io_instat:
        mov     dx,OFFSET t_iostat
        call    puts
        mov     byte ptr [step],'a'
        mov     dx,OFFSET n_temp
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      ios_bad
        mov     [h_tmp],ax
        mov     bx,ax
        mov     dx,OFFSET junk
        mov     cx,16
        mov     ah,40h
        int     21h
        jc      ios_bad

        mov     byte ptr [step],'b'     ; at the end: no input
        mov     bx,[h_tmp]
        mov     ax,4406h
        int     21h
        jc      ios_bad
        or      al,al
        jnz     ios_bad

        mov     byte ptr [step],'c'     ; back to the start: input
        mov     bx,[h_tmp]
        xor     cx,cx
        xor     dx,dx
        mov     ax,4200h
        int     21h
        mov     bx,[h_tmp]
        mov     ax,4406h
        int     21h
        jc      ios_bad
        cmp     al,0FFh
        jne     ios_bad

        mov     byte ptr [step],'d'     ; and output is always ready
        mov     bx,[h_tmp]
        mov     ax,4407h
        int     21h
        jc      ios_bad
        cmp     al,0FFh
        jne     ios_bad
        call    good
        jmp     short io_commit
ios_bad:
        call    atstep
        call    bad

;=====================================================================
; 68h - commit, with the handle still open afterwards
;
; The check that matters is the SECOND one: after the commit the file
; is written, and after writing more through the same handle and
; closing properly the file is longer still.  A commit that quietly
; closed the handle would pass the first check and fail this.
;=====================================================================
io_commit:
        mov     dx,OFFSET t_commit
        call    puts
        mov     byte ptr [step],'a'
        mov     bx,[h_tmp]
        xor     cx,cx                   ; to the end before adding more
        xor     dx,dx
        mov     ax,4202h
        int     21h
        mov     bx,[h_tmp]
        mov     ah,68h
        int     21h
        jc      com_bad

        mov     byte ptr [step],'b'     ; the handle still works
        mov     bx,[h_tmp]
        mov     dx,OFFSET junk
        mov     cx,16
        mov     ah,40h
        int     21h
        jc      com_bad
        cmp     ax,16
        jne     com_bad
        mov     bx,[h_tmp]
        mov     ah,3Eh
        int     21h

        mov     byte ptr [step],'c'     ; and 32 bytes are on the disk
        mov     dx,OFFSET n_temp
        mov     ax,3D00h
        int     21h
        jc      com_bad
        mov     bx,ax
        push    bx
        xor     cx,cx
        xor     dx,dx
        mov     ax,4202h                ; seek to end = the size
        int     21h
        pop     bx
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        cmp     ax,32
        jne     com_bad
        call    good
        jmp     short do_5b
com_bad:
        call    atstep
        call    bad

;=====================================================================
; 5Bh - create new.  The file from the last test is still there, so
;       this must be refused with error 80.
;=====================================================================
do_5b:
        mov     dx,OFFSET t_5b
        call    puts
        mov     dx,OFFSET n_temp
        xor     cx,cx
        mov     ah,5Bh
        int     21h
        jnc     f5b_bad                 ; it overwrote the file
        cmp     ax,80                   ; ERROR_FILE_EXISTS
        jne     f5b_bad
        call    good
        jmp     short f5b_new
f5b_bad:
        call    bad

;--- and it does create when there is nothing in the way --------------
f5b_new:
        mov     dx,OFFSET t_5bnew
        call    puts
        mov     dx,OFFSET n_temp2
        mov     ah,41h                  ; make sure of that
        int     21h
        mov     dx,OFFSET n_temp2
        xor     cx,cx
        mov     ah,5Bh
        int     21h
        jc      f5bn_bad
        mov     bx,ax
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET n_temp2
        mov     ah,41h
        int     21h
        call    good
        jmp     short do_5a
f5bn_bad:
        call    bad

;=====================================================================
; 5Ah - a temporary file.  The buffer holds "\" and comes back holding
;       "\XXXXXXXX", which is as much the answer as the handle is.
;=====================================================================
do_5a:
        mov     dx,OFFSET t_5a
        call    puts
        mov     dx,OFFSET tmpbuf
        xor     cx,cx
        mov     ah,5Ah
        int     21h
        jc      f5a_bad
        mov     bx,ax
        mov     ah,3Eh                  ; close it, then show the name
        int     21h
        mov     dx,OFFSET tmpbuf
        call    putz
        mov     al,[tmpbuf+1]           ; a name really was appended
        or      al,al
        jz      f5a_bad
        mov     dx,OFFSET tmpbuf        ; and it exists, and goes away
        mov     ah,41h
        int     21h
        jc      f5a_bad
        call    good
        jmp     short do_60
f5a_bad:
        call    bad

;=====================================================================
; 60h - TRUENAME.  A relative path with a "." and a ".." in it, which
;       is where a canonicaliser earns its keep.
;=====================================================================
do_60:
        mov     dx,OFFSET t_60
        call    puts
        mov     si,OFFSET n_rel
        mov     di,OFFSET tnbuf
        push    ds
        pop     es
        mov     ah,60h
        int     21h
        jc      f60_err
        mov     dx,OFFSET tnbuf
        call    putz
        cmp     byte ptr [tnbuf+1],':'  ; "X:\..." or it is not canonical
        jne     f60_bad
        cmp     byte ptr [tnbuf+2],'\'
        jne     f60_bad
        call    good
        jmp     short do_verify
f60_err:
        call    put4                    ; the error code, which is the
        mov     dl,'h'                  ; difference between "no such path"
        call    putc                    ; and "the call is missing"
f60_bad:
        call    bad

;=====================================================================
; 2Eh / 54h - the verify flag
;=====================================================================
do_verify:
        mov     dx,OFFSET t_verify
        call    puts
        mov     ah,54h                  ; what is it now
        int     21h
        mov     [wasverify],al
        mov     ax,2E01h                ; turn it on
        int     21h
        mov     ah,54h
        int     21h
        cmp     al,1
        jne     ver2_bad
        mov     ax,2E00h                ; and off
        int     21h
        mov     ah,54h
        int     21h
        or      al,al
        jnz     ver2_bad
        mov     al,[wasverify]          ; put it back as it was found
        mov     ah,2Eh
        int     21h
        call    good
        jmp     short do_drive
ver2_bad:
        call    bad

;=====================================================================
; 4408h / 4409h / 440Bh / 440Eh - the drive subfunctions
;
; The default drive is asked about rather than a named one, because
; APITEST runs off the floppy on one emulator and the hard disk on the
; other and both answers are correct.  What IS checked is that a drive
; that does not exist is refused - the failure that would otherwise let
; a program go on to use it.
;=====================================================================
do_drive:
        mov     dx,OFFSET t_drive
        call    puts
        mov     byte ptr [step],'a'
        xor     bl,bl                   ; the default drive
        mov     ax,4408h
        int     21h
        jc      drv_bad
        push    ax
        mov     dx,OFFSET msg_media
        call    puts
        pop     ax
        push    ax
        or      ax,ax
        jnz     drv_fixed
        mov     dx,OFFSET msg_removbl
        jmp     short drv_say
drv_fixed:
        mov     dx,OFFSET msg_fixed
drv_say:
        call    puts
        pop     ax

        mov     byte ptr [step],'b'     ; local, so bit 12 clear
        xor     bl,bl
        mov     ax,4409h
        int     21h
        jc      drv_bad
        test    dx,1000h
        jnz     drv_bad

        mov     byte ptr [step],'c'     ; a retry count is accepted
        mov     dx,3
        mov     ax,440Bh
        int     21h
        jc      drv_bad

        mov     byte ptr [step],'d'     ; and no drive has a second letter
        xor     bl,bl
        mov     ax,440Eh
        int     21h
        jc      drv_bad
        or      al,al
        jnz     drv_bad

        mov     byte ptr [step],'e'     ; drive 26 does not exist
        mov     bl,26
        mov     ax,4408h
        int     21h
        jnc     drv_bad
        call    good
        jmp     short do_prn
drv_bad:
        call    atstep
        call    bad

;=====================================================================
; 05h - printer output.  Nothing is checked but that it returns: with
;       no printer INT 17h times out, with one a form feed comes out,
;       and neither is a failure.  What is being tested is that the
;       call exists at all - before v0.31 it was "invalid function".
;=====================================================================
do_prn:
        mov     dx,OFFSET t_prn
        call    puts
        mov     dl,12                   ; form feed
        mov     ah,05h
        int     21h
        call    good

;=====================================================================
; 25h/35h ON VECTOR 0 - the one the null guard sits on.
;
; This is the regression test for the bug that killed QCONFIG.EXE on
; the first real machine PM-DOS ran on.  The guard watches linear 0 to
; 15 with four debug registers, which IS the first four IVT entries,
; so a guest hooking INT 0 - ordinary behaviour for anything that
; probes a CPU by dividing - looked exactly like a null-pointer write.
; The kernel serves 25h with the guard armed, so it fired and the
; program was terminated for doing something entirely legitimate.
;
; TWO THINGS ARE CHECKED, and the second is the one that could rot
; quietly.  That the hook works at all, by reading it back with 35h;
; and that the guard is ARMED AGAIN afterwards, because ivt_set stands
; it down across the store and a missed re-arm would disable the null
; guard for the rest of the session without any symptom whatsoever.
;
; NOTE WHAT THIS CANNOT DO HERE: no emulator implements data
; breakpoints, so under DOSBox or 86Box the guard would not have fired
; even before the fix.  This test passing on an emulator proves the
; bookkeeping, not the behaviour.  Only iron proves the behaviour.
;=====================================================================
do_vec0:
        mov     dx,OFFSET t_vec0
        call    puts

        mov     ax,3500h                ; save whatever is there
        int     21h
        mov     word ptr [old00],bx
        mov     word ptr [old00+2],es

        push    ds                      ; hook it with a known value
        mov     dx,OFFSET v0_stub
        mov     ax,2500h
        int     21h
        pop     ds

        mov     ax,3500h                ; and read it back
        int     21h
        mov     ax,es
        mov     cx,ds
        cmp     ax,cx
        jne     v0_bad
        cmp     bx,OFFSET v0_stub
        jne     v0_bad

        push    ds                      ; put the old one back first -
        lds     dx,dword ptr [old00]    ; a wrong INT 0 vector outlives
        mov     ax,2500h                ; this program
        int     21h
        pop     ds
        call    good
        jmp     short v0_armed
v0_bad:
        push    ds
        lds     dx,dword ptr [old00]
        mov     ax,2500h
        int     21h
        pop     ds
        call    bad

v0_armed:
        mov     dx,OFFSET t_vecarm
        call    puts
        mov     ax,0F107h               ; PM-DOS: null-guard state
        int     21h
        or      al,al                   ; AL = armed right now?
        jz      v0_off
        call    good
        jmp     short v0_done
v0_off:
        ; Not a failure if the guard was never wanted - CONFIG.SYS can
        ; turn it off with NULLTRAP=OFF, and then "not armed" is the
        ; correct answer rather than a leak.  Say so instead of crying.
        mov     dx,OFFSET t_vecnog
        call    puts
v0_done:

;=====================================================================
        mov     dx,OFFSET n_temp        ; tidy up
        mov     ah,41h
        int     21h

        mov     dx,OFFSET msg_bye
        call    puts
        movzx   ax,byte ptr [fails]
        mov     ah,4Ch
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

; which step failed, and what it said - the difference between "the
; drive tests failed" and knowing which of the five it was
atstep:
        push    ax
        mov     dl,'@'
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
        pop     ax
        ret

puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret

; putz - print an ASCIIZ string at DX.
;
; NOT puts.  5Ah and 60h hand back NUL-terminated strings, because that
; is what the whole handle-era API deals in, and printing one with
; 09h walks off the end looking for a '$' it will never find - it ran
; through the rest of this program and out into the leftover directory
; sectors sitting in memory, and printed the root directory of the
; floppy across the screen.  A spectacular failure for a missing four
; lines of code.
putz:
        push    ax
        push    si
        mov     si,dx
putz_lp:
        mov     al,[si]
        or      al,al
        jz      putz_end
        push    dx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     dx
        inc     si
        jmp     short putz_lp
putz_end:
        pop     si
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

; AL in decimal, no padding.  Two digits is all this needs.
pdec:
        push    ax
        push    bx
        movzx   ax,al
        mov     bl,10
        div     bl                      ; AL = tens, AH = units
        or      al,al
        jz      pd_units
        push    ax
        add     al,'0'
        mov     dl,al
        call    putc
        pop     ax
pd_units:
        mov     al,ah
        add     al,'0'
        mov     dl,al
        call    putc
        pop     bx
        pop     ax
        ret

;=====================================================================
; data
;=====================================================================
msg_hi      db  13,10,'APITEST - IOCTL, 5Ah/5Bh, TRUENAME, commit, verify, version',13,10,'$'
msg_bye     db  'APITEST done.',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_media   db  ' - $'
msg_removbl db  'removable$'
msg_fixed   db  'fixed$'

t_ver       db  '  30h says DOS $'
t_vertrue   db  '  3306h agrees: $'
t_ioget     db  '  4400h device info for stdout $'
t_ioraw     db  '  4401h raw mode on and off again$'
t_ioskip    db  '  4401h skipped: stdout is a file',13,10,'$'
t_iostat    db  '  4406h/07h input and output status$'
t_commit    db  '  68h commit leaves the handle open$'
t_5b        db  '  5Bh refuses to overwrite$'
t_5bnew     db  '  5Bh creates when nothing is there$'
t_5a        db  '  5Ah unique name: $'
t_60        db  '  60h TRUENAME of ".\..\": $'
t_verify    db  '  2Eh/54h verify flag$'
t_drive     db  '  44h drive subfunctions$'
t_prn       db  '  05h printer output$'
t_vec0      db  '  25h/35h hook INT 0 (the guarded vector)$'
t_vecarm    db  '  null guard re-armed after the hook$'
t_vecnog    db  ' n/a (NULLTRAP=OFF)',13,10,'$'
old00       dd  0
v0_stub     db  0CFh                    ; IRET - never executed, only
                                        ; a recognisable address

n_temp      db  'APITEST.$$$',0
n_temp2     db  'APITEST2.$$$',0
n_rel       db  '.\..\APITEST.COM',0
tmpbuf      db  '\',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

fails       db  0
step        db  '?'
wasverify   db  0
got_maj     db  0
got_min     db  0
devinfo     dw  0
h_tmp       dw  0
tnbuf       db  128 dup(0)
junk        db  'APITEST wrote this. 16 bytes exactly, twice over.',0

_AT     ENDS
        END
