;=====================================================================
; VFTEST.ASM - VFTEST.COM, the clean-shutdown flag on C:, asked of the
;              kernel and read off the disk.
;
;   VFTEST          the checks below
;   VFTEST Q        one line: what the kernel says about C:, and what
;                   the bit on the disk says
;   VFTEST HANG     C:\VFHANG.DAT written and left open, and then the
;                   machine stops - for a test rig to pull the plug on,
;                   so that the next boot finds C: not shut down
;   VFTEST HANGC    the same, after a commit: the file's clusters are
;                   on the disk and its directory entry does not have
;                   them, so the next CHKDSK finds a lost chain
;
; SIXTEEN-BIT so that the same disk calls a DOS program makes are the
; ones tested: 21h/7305h through the DOS box reads the sectors, and
; 21h/F1h AL=1Dh is reached through the V86 bridge.  The bit is in FAT
; entry 1 - bit 15 on FAT16, bit 27 on FAT32 - and set means clean.
;
; EVERYTHING IT SAYS WAITS IN A BUFFER until the checks are over, and
; that is the test and not tidiness.  Its output is usually redirected
; to a file on C:, and a line written there is a write to C: - it
; turns the bit off, and the open file holds it off through every
; commit after.  The first version printed as it went and failed its
; own clean checks.  Q measures first and prints after, for the same
; reason.
;
; Seven checks:
;
;   1  F1h AL=1Dh answers for C:, and says C: has the bit
;   2  after a commit (21h/0Dh): not dirty, and the bit is on
;   3  a file written and left open: dirty, and the bit is off
;   4  a commit with that file still open: still both
;   5  the file closed and committed: clean, and the bit is on
;   6  the file deleted: dirty and off until the next commit, and
;      clean and on after it - a write with no file open at all
;   7  a letter past the last one is refused
;   8  a raw write (7305h) of the FAT's second sector, put back as it
;      was: dirty and off, then clean and on after a commit
;   9  the same for FAT sector 0, which holds the bit - put back with
;      the bit ON, as it was read: the kernel turns it off again
;  10  the same for the boot sector, which unmounts the volume: the
;      commit has to mount it again to turn the bit on
;
; VLF_CHECKED IS NOT ONE OF THEM, because a DOS box cannot ask for it:
; the function goes in bits 16-23 of EAX, as F1h's other families
; have it, and the V86 bridge hands the kernel AX (vdm_do21).  The
; first version tried, and VLF_CHECKED arrived as VLF_QUERY.  CHKDSK
; is native and calls it; NOTES.TXT has the boots that show it working.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_VF     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_VF, DS:_VF, ES:_VF, SS:_VF
        ORG     0

VLF_QUERY_AX    EQU     0F11Dh          ; with VLF_QUERY (0) above it
VLS_KEPT        EQU     01h
VLS_DIRTY       EQU     02h
VLS_HOLD        EQU     04h
DRIVE_C         EQU     3               ; 1 = A:

start:
        mov     si,81h                  ; the command tail
tl_skip:
        lodsb
        cmp     al,' '
        je      tl_skip
        cmp     al,9
        je      tl_skip
        and     al,0DFh
        cmp     al,'Q'
        je      do_query
        cmp     al,'H'
        je      do_hang
        jmp     do_checks

;=====================================================================
; VFTEST Q
;=====================================================================
do_query:
        call    ask                     ; both answers before a word
        setc    [qfail]                 ; is written anywhere
        mov     [state],al
        call    diskbit
        setc    [dfail]
        setnz   [doff]

        mov     dx,OFFSET msg_q
        call    puts
        cmp     byte ptr [qfail],0
        je      dq_ans
        mov     dx,OFFSET msg_qnone
        call    puts
        jmp     dq_disk
dq_ans:
        mov     dx,OFFSET msg_kept
        call    puts
        mov     al,[state]
        and     al,VLS_KEPT
        call    put01
        mov     dx,OFFSET msg_dirty
        call    puts
        mov     al,[state]
        and     al,VLS_DIRTY
        call    put01
        mov     dx,OFFSET msg_hold
        call    puts
        mov     al,[state]
        and     al,VLS_HOLD
        call    put01
dq_disk:
        mov     dx,OFFSET msg_disk
        call    puts
        cmp     byte ptr [dfail],0
        jne     dq_noread
        mov     dx,OFFSET msg_on
        cmp     byte ptr [doff],0
        je      dq_say
        mov     dx,OFFSET msg_off
dq_say:
        call    puts
        call    crlf
        mov     al,0
        jmp     quit
dq_noread:
        mov     dx,OFFSET msg_unread
        call    puts
        mov     al,1
        jmp     quit

;=====================================================================
; VFTEST HANG / HANGC
;=====================================================================
do_hang:
        lodsb                           ; "HANG" then maybe "C"
        lodsb
        lodsb
        lodsb
        and     al,0DFh
        mov     [hang_c],al
        call    mkfile
        jc      dh_fail
        cmp     byte ptr [hang_c],'C'
        jne     dh_stop
        mov     ax,4A10h                ; a commit that leaves the file
        mov     bx,1                    ; open: SMARTDRV's, since 0Dh
        int     2Fh                     ; would do the same
dh_stop:
        mov     dx,OFFSET msg_hung
        call    puts
        call    flushout
dh_spin:
        jmp     dh_spin                 ; until the plug is pulled
dh_fail:
        mov     dx,OFFSET msg_nowrite
        call    puts
        mov     al,1
        jmp     quit

;=====================================================================
; The checks
;=====================================================================
do_checks:
        mov     dx,OFFSET msg_hi
        call    puts

;--- 1 ---------------------------------------------------------------
        mov     dx,OFFSET msg_t1
        call    puts
        call    ask
        jc      c1_bad
        test    al,VLS_KEPT
        jz      c1_bad
        call    ok
        jmp     c2
c1_bad:
        call    bad
        jmp     c_bye                   ; nothing else means anything

;--- 2 ---------------------------------------------------------------
c2:
        mov     dx,OFFSET msg_t2
        call    puts
        call    commit
        mov     ah,0                    ; want: clean, bit on
        call    expect

;--- 3 ---------------------------------------------------------------
        mov     dx,OFFSET msg_t3
        call    puts
        call    mkfile
        jnc     c3_go
        mov     dx,OFFSET msg_nowrite
        call    puts
        jmp     c_bye
c3_go:
        mov     ah,1                    ; want: dirty, bit off
        call    expect

;--- 4 ---------------------------------------------------------------
        mov     dx,OFFSET msg_t4
        call    puts
        call    commit
        mov     ah,1
        call    expect

;--- 5 ---------------------------------------------------------------
        mov     dx,OFFSET msg_t5
        call    puts
        mov     bx,[handle]
        mov     ah,3Eh
        int     21h
        call    commit
        mov     ah,0
        call    expect

;--- 6 ---------------------------------------------------------------
        mov     dx,OFFSET msg_t6
        call    puts
        mov     dx,OFFSET wrname
        mov     ah,41h
        int     21h
        mov     ah,1
        call    expect
        mov     dx,OFFSET msg_t6b
        call    puts
        call    commit
        mov     ah,0
        call    expect

;--- 7 ---------------------------------------------------------------
        mov     dx,OFFSET msg_t7
        call    puts
        mov     ax,VLF_QUERY_AX
        mov     dl,27                   ; one past Z:
        int     21h
        jnc     c7_bad
        call    ok
        jmp     c8
c7_bad:
        call    bad

;--- 8, 9, 10: a sector read and written back raw --------------------
c8:
        call    fatsec0                 ; EAX = FAT sector 0's number
        jc      c_bye
        inc     eax
        mov     dx,OFFSET msg_t8
        call    rawback
        call    fatsec0
        mov     dx,OFFSET msg_t9
        call    rawback
        xor     eax,eax
        mov     dx,OFFSET msg_t10
        call    rawback

c_bye:
        mov     dx,OFFSET msg_sum
        call    puts
        mov     al,[failures]
        call    put2d
        mov     dx,OFFSET msg_failed
        call    puts
        mov     al,[failures]

; quit - AL = the exit code, after everything said has been written
quit:
        call    flushout
        mov     ah,4Ch
        int     21h

; flushout - the buffer to standard output, in one write, and emptied
flushout:
        push    ax
        mov     cx,[outp]
        sub     cx,OFFSET outbuf
        jz      fo_out
        mov     bx,1
        mov     dx,OFFSET outbuf
        mov     ah,40h
        int     21h
        mov     word ptr [outp],OFFSET outbuf
fo_out:
        pop     ax
        ret

;=====================================================================
; ask - F1h AL=1Dh VLF_QUERY on C:.  CF, or AL = the VLS_* bits.
;=====================================================================
ask:
        mov     eax,VLF_QUERY_AX
        mov     dl,DRIVE_C
        int     21h
        ret

;=====================================================================
; expect - AH = 0 for "clean and the bit on", 1 for "dirty and the bit
;          off".  Both the kernel's answer and the disk have to agree,
;          and the line says which did not.
;=====================================================================
expect:
        mov     [want],ah
        call    ask
        jc      ex_bad
        and     al,VLS_DIRTY OR VLS_HOLD
        mov     ah,0
        cmp     byte ptr [want],0
        je      ex_kwant
        mov     ah,VLS_DIRTY
ex_kwant:
        cmp     al,ah
        jne     ex_kbad
        call    diskbit                 ; ZF = on, NZ = off
        jc      ex_bad
        setnz   al
        cmp     al,[want]
        jne     ex_dbad
        call    ok
        ret
ex_kbad:
        mov     dx,OFFSET msg_kwrong
        call    puts
        jmp     ex_bad
ex_dbad:
        mov     dx,OFFSET msg_dwrong
        call    puts
ex_bad:
        call    bad
        ret

;=====================================================================
; diskbit - C:'s clean-shutdown bit, read off the disk.  CF if the
;           sectors would not read; else ZF if the bit is on (clean),
;           NZ if it is off.
;
; Sector 0 for the reserved count, then the first sector of the FAT.
; F1h AL=1 says the FAT width.
;=====================================================================
diskbit:
        mov     ax,0F101h
        mov     dl,DRIVE_C
        int     21h
        jc      db_out
        mov     [fatw],al
        xor     eax,eax
        call    rdsec
        jc      db_out
        movzx   eax,word ptr [scratch+0Eh]
        call    rdsec
        jc      db_out
        cmp     byte ptr [fatw],32
        je      db_32
        test    word ptr [scratch+2],8000h
        jmp     db_flip
db_32:
        test    dword ptr [scratch+4],08000000h
db_flip:
        ; TEST gave NZ for "on"; the answer wants ZF for "on"
        lahf
        xor     ah,40h                  ; ZF is bit 6
        sahf
        clc
db_out:
        ret

; rdsec - EAX = a sector of C:, into scratch.  21h/7305h.  CF on error.
; wrsec - the same sector, out of scratch.
rdsec:
        xor     si,si                   ; read
        jmp     rwsec
wrsec:
        mov     si,1                    ; write, what is being written unsaid
rwsec:
        mov     dword ptr [pk_lba],eax
        mov     word ptr [pk_cnt],1
        mov     word ptr [pk_off],OFFSET scratch
        mov     word ptr [pk_seg],cs
        mov     ax,7305h
        mov     cx,0FFFFh
        mov     dl,DRIVE_C
        mov     bx,OFFSET packet
        int     21h
        ret

; fatsec0 - EAX = the number of C:'s first FAT sector.  CF on error.
fatsec0:
        xor     eax,eax
        call    rdsec
        jc      fs0_out
        movzx   eax,word ptr [scratch+0Eh]
fs0_out:
        ret

;=====================================================================
; rawback - checks 8 to 10.  EAX = a sector of C:, DX = the check's
;           line.  From clean, the sector is read and written back
;           unchanged through 7305h: dirty and off; then a commit:
;           clean and on.
;=====================================================================
rawback:
        mov     [rb_lba],eax
        call    puts
        call    commit
        mov     eax,[rb_lba]
        call    rdsec
        jc      rb_bad
        mov     eax,[rb_lba]
        call    wrsec
        jc      rb_bad
        mov     ah,1
        call    expect
        mov     dx,OFFSET msg_rbc
        call    puts
        call    commit
        mov     ah,0
        call    expect
        ret
rb_bad:
        call    bad
        ret

;=====================================================================
; mkfile - C:\VFTEST.$$$ (or VFHANG.DAT) created and 4K written to it,
;          left open in [handle].  CF if it could not be.
;=====================================================================
mkfile:
        mov     dx,OFFSET wrname
        cmp     byte ptr [hang_c],0
        je      mk_go
        mov     dx,OFFSET hangname
mk_go:
        mov     ah,3Ch
        xor     cx,cx
        int     21h
        jc      mk_out
        mov     [handle],ax
        mov     bx,ax
        mov     cx,4096
        mov     dx,OFFSET filler
        mov     ah,40h
        int     21h
        jc      mk_out
        cmp     ax,4096
        jne     mk_short
        clc
mk_out:
        ret
mk_short:
        stc
        ret

commit:
        mov     ah,0Dh
        int     21h
        ret

;=====================================================================
ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        inc     byte ptr [failures]
        mov     dx,OFFSET msg_bad
        call    puts
        pop     dx
        ret

; put01 - AL zero or not, as "0" or "1"
put01:
        mov     dl,'0'
        or      al,al
        jz      p01_put
        mov     dl,'1'
p01_put:
        call    putc
        ret

;=====================================================================
; Output helpers: the usual names, but into outbuf - see the top.
; puts takes DX -> text ending in '$', putc DL.  A full buffer drops
; the rest rather than writing early.
;=====================================================================
puts:
        push    ax
        push    si
        push    di
        mov     si,dx
        mov     di,[outp]
ps_lp:
        lodsb
        cmp     al,'$'
        je      ps_end
        cmp     di,OFFSET outend
        jae     ps_lp
        mov     [di],al
        inc     di
        jmp     ps_lp
ps_end:
        mov     [outp],di
        pop     di
        pop     si
        pop     ax
        ret
putc:
        push    di
        mov     di,[outp]
        cmp     di,OFFSET outend
        jae     pc_out
        mov     [di],dl
        inc     di
        mov     [outp],di
pc_out:
        pop     di
        ret
crlf:
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
        ret

;--- AL in decimal, up to three digits, no padding -------------------
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
failures    db  0
state       db  0
want        db  0
fatw        db  0
hang_c      db  0                       ; 0 for the checks; 'C' or not
qfail       db  0                       ; Q: the kernel did not answer
dfail       db  0                       ; Q: the sectors did not read
doff        db  0                       ; Q: the bit on the disk is off
handle      dw  0
outp        dw  OFFSET outbuf           ; where the next character goes
rb_lba      dd  0                       ; rawback's sector

packet      LABEL BYTE                  ; 7305h's DISKIO packet
pk_lba      dd  0
pk_cnt      dw  0
pk_off      dw  0
pk_seg      dw  0

wrname      db  'C:\VFTEST.$$$',0
hangname    db  'C:\VFHANG.DAT',0

msg_hi      db  13,10,'VFTEST - the clean-shutdown flag on C:',13,10
            db  '--------------------------------------',13,10,'$'
msg_t1      db  'C: has the flag              $'
msg_t2      db  'committed: clean, bit on     $'
msg_t3      db  'file open, written: dirty    $'
msg_t4      db  '...committed, still dirty    $'
msg_t5      db  'closed, committed: clean     $'
msg_t6      db  'deleted: dirty               $'
msg_t6b     db  '...committed: clean          $'
msg_t7      db  'no such drive refused        $'
msg_t8      db  'raw FAT sector 1: dirty      $'
msg_t9      db  'raw FAT sector 0: dirty      $'
msg_t10     db  'raw boot sector: dirty       $'
msg_rbc     db  '...committed: clean          $'
msg_ok      db  'OK',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_kwrong  db  '(kernel) $'
msg_dwrong  db  '(disk) $'
msg_crlf    db  13,10,'$'
msg_nowrite db  'cannot write to C: - skipped',13,10,'$'
msg_sum     db  13,10,'VFTEST done, $'
msg_failed  db  ' failed',13,10,'$'
msg_q       db  'VFTEST Q: C:$'
msg_qnone   db  ' no answer$'
msg_kept    db  ' KEPT=$'
msg_dirty   db  ' DIRTY=$'
msg_hold    db  ' HOLD=$'
msg_disk    db  ' DISK=$'
msg_on      db  'ON$'
msg_off     db  'OFF$'
msg_unread  db  'UNREADABLE',13,10,'$'
msg_hung    db  'VFTEST: a written file is open and the machine has stopped',13,10,'$'

filler      db  4096 dup('V')
scratch     db  512 dup(0)
outbuf      db  2048 dup(0)
outend      LABEL BYTE

_VF     ENDS
        END     start
