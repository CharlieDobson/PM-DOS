;=====================================================================
; FCBTEST.ASM - FCBTEST.COM, the file control block API
;
; An ordinary 16-bit DOS program, so every call it makes crosses the
; VDM bridge as well as the kernel - which is half the point, because
; an FCB is a pointer and the bridge is what turns a guest's DS:DX
; into an address the kernel can use.  21h/29h is the one call in DOS
; whose ANSWER is a pointer register, and it is tested here for that
; reason before anything else is tested at all.
;
; What it covers, in the order the file below runs them:
;
;   29h  parse - a plain name, a drive letter, wildcards, the
;        deliberate quirk about names longer than the field, and a
;        drive that does not exist
;   16h  create, 0Fh open, 10h close
;   15h/14h  sequential write and read, with a record size the
;        program chose rather than the default
;   22h/21h  random write and read
;   28h/27h  block write and read, including what CX comes back as
;   23h  file size in records, 24h set relative record
;   11h/12h  find first and find next, through a wildcard FCB
;   17h  rename, 13h delete - both of which take wildcards where the
;        handle API refuses them
;   14h  reading the last, short record: AL = 3 and the rest zeroed
;
; And the one that is really about the design rather than the API:
;
;   FORTY FCBs OPEN AT ONCE on a kernel with thirty-two table entries.
;   A CP/M-era program opens FCBs and never closes them - that is not
;   a leak, it is how the API was used - so the kernel recycles the
;   entry underneath and re-opens from the name in the FCB when the
;   program comes back to it.  The check is that FCB number one still
;   reads its own data after thirty-nine others have been opened on
;   top of it.  Nothing else in this file would notice if that broke.
;
; Every check prints "ok" or "FAILED", so a clean run is a screen with
; no capital letters in it.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_FT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_FT, DS:_FT, ES:_FT, SS:_FT
        ORG     0

RECSZ   EQU     16                      ; our record size, not DOS's 128
NRECS   EQU     4                       ; and how many we write

start:
        mov     dx,OFFSET msg_hi
        call    puts

        ; the DTA is ours from here on: FCB I/O has nowhere else to
        ; put anything, and the default is the command tail
        mov     dx,OFFSET dta
        mov     ah,1Ah
        int     21h

        ; leave nothing behind from a previous run
        mov     dx,OFFSET fcbwild
        mov     ah,13h
        int     21h

;=====================================================================
; 29h - parse a filename
;=====================================================================
        mov     dx,OFFSET t_p1
        call    puts
        mov     si,OFFSET p_plain
        mov     di,OFFSET pfcb
        mov     ax,2900h
        int     21h
        or      al,al
        jnz     p1_bad                  ; 0 = a name with no wildcards
        mov     si,OFFSET pfcb
        mov     di,OFFSET e_plain
        mov     cx,12
        call    cmpbuf
        jne     p1_bad
        call    good
        jmp     short p2
p1_bad:
        call    bad
p2:
        mov     dx,OFFSET t_p2
        call    puts
        mov     si,OFFSET p_drive
        mov     di,OFFSET pfcb
        mov     ax,2900h
        int     21h
        or      al,al
        jnz     p2_bad
        mov     si,OFFSET pfcb
        mov     di,OFFSET e_drive
        mov     cx,12
        call    cmpbuf
        jne     p2_bad
        call    good
        jmp     short p3
p2_bad:
        call    bad
p3:
        mov     dx,OFFSET t_p3
        call    puts
        mov     si,OFFSET p_star
        mov     di,OFFSET pfcb
        mov     ax,2900h
        int     21h
        cmp     al,1                    ; 1 = it has wildcards in it
        jne     p3_bad
        mov     si,OFFSET pfcb
        mov     di,OFFSET e_star
        mov     cx,12
        call    cmpbuf
        jne     p3_bad
        call    good
        jmp     short p4
p3_bad:
        call    bad

;--- the quirk: a name longer than the field is truncated, and SI is
;--- left past ALL of it rather than in the middle of it
p4:
        mov     dx,OFFSET t_p4
        call    puts
        mov     si,OFFSET p_long
        mov     di,OFFSET pfcb
        mov     ax,2900h
        int     21h
        mov     [savsi],si
        mov     si,OFFSET pfcb
        mov     di,OFFSET e_long
        mov     cx,12
        call    cmpbuf
        jne     p4_bad
        cmp     [savsi],OFFSET p_long_end
        jne     p4_bad
        call    good
        jmp     short p5
p4_bad:
        call    bad
p5:
        mov     dx,OFFSET t_p5
        call    puts
        mov     si,OFFSET p_baddrv
        mov     di,OFFSET pfcb
        mov     ax,2900h
        int     21h
        cmp     al,0FFh                 ; no such drive
        jne     p5_bad
        call    good
        jmp     short mk
p5_bad:
        call    bad

;=====================================================================
; 16h - create, and what an open leaves in the FCB
;=====================================================================
mk:
        mov     dx,OFFSET t_create
        call    puts
        call    initfcb
        mov     dx,OFFSET fcb1
        mov     ah,16h
        int     21h
        or      al,al
        jnz     mk_bad
        cmp     word ptr [fcb1+0Eh],128 ; the record size an open writes
        jne     mk_bad
        cmp     word ptr [fcb1+10h],0   ; a new file is empty
        jne     mk_bad
        cmp     word ptr [fcb1+12h],0
        jne     mk_bad
        call    good
        jmp     short wr
mk_bad:
        call    bad

;=====================================================================
; 15h - four sequential writes, of a record size we chose
;=====================================================================
wr:
        mov     dx,OFFSET t_write
        call    puts
        mov     word ptr [fcb1+0Eh],RECSZ
        mov     byte ptr [fcb1+20h],0   ; the record byte an open leaves
                                        ; alone: DOS's rule, so ours
        mov     cx,NRECS
        mov     al,'0'
wr_lp:
        push    cx
        push    ax
        mov     cx,RECSZ
        call    fillrec
        mov     dx,OFFSET fcb1
        mov     ah,15h
        int     21h
        or      al,al                   ; BEFORE the pops: AL is the answer
        pop     ax                      ; and the pop puts the fill byte
        pop     cx                      ; back over it
        jnz     wr_bad
        inc     al
        loop    wr_lp
        call    good
        jmp     short clos
wr_bad:
        call    bad

;=====================================================================
; 10h - close, and the size it leaves behind
;=====================================================================
clos:
        mov     dx,OFFSET t_close
        call    puts
        mov     dx,OFFSET fcb1
        mov     ah,10h
        int     21h
        or      al,al
        jnz     cl_bad
        cmp     word ptr [fcb1+10h],NRECS * RECSZ
        jne     cl_bad
        cmp     word ptr [fcb1+12h],0
        jne     cl_bad
        call    good
        jmp     short op
cl_bad:
        call    bad

;=====================================================================
; 0Fh - open it again and read it back sequentially
;=====================================================================
op:
        mov     dx,OFFSET t_open
        call    puts
        call    initfcb
        mov     dx,OFFSET fcb1
        mov     ah,0Fh
        int     21h
        or      al,al
        jnz     op_bad
        cmp     word ptr [fcb1+10h],NRECS * RECSZ
        jne     op_bad
        call    good
        jmp     short rd
op_bad:
        call    bad

rd:
        mov     dx,OFFSET t_read
        call    puts
        mov     word ptr [fcb1+0Eh],RECSZ
        mov     byte ptr [fcb1+20h],0
        mov     word ptr [fcb1+0Ch],0
        mov     cx,NRECS
        mov     al,'0'
rd_lp:
        push    cx
        push    ax
        mov     dx,OFFSET fcb1
        mov     ah,14h
        int     21h
        or      al,al
        pop     ax
        pop     cx
        jnz     rd_bad
        push    cx
        push    ax
        mov     cx,RECSZ
        call    checkrec                ; is the whole record AL?
        pop     ax
        pop     cx
        jne     rd_bad
        inc     al
        loop    rd_lp
        call    good
        jmp     short rr
rd_bad:
        call    bad

;=====================================================================
; 21h - random read.  Record 2 without having read 0 and 1 first.
;=====================================================================
rr:
        mov     dx,OFFSET t_ranrd
        call    puts
        mov     word ptr [fcb1+21h],2   ; the random record field...
        mov     word ptr [fcb1+23h],0
        mov     dx,OFFSET fcb1
        mov     ah,21h
        int     21h
        or      al,al
        jnz     rr_bad
        mov     al,'2'
        mov     cx,RECSZ
        call    checkrec
        jne     rr_bad
        ; ...and a random read moves the sequential position to match,
        ; which is the whole reason a program can mix the two
        cmp     word ptr [fcb1+0Ch],0
        jne     rr_bad
        cmp     byte ptr [fcb1+20h],2
        jne     rr_bad
        call    good
        jmp     short rw
rr_bad:
        call    bad

;=====================================================================
; 22h - random write, over record 1
;=====================================================================
rw:
        mov     dx,OFFSET t_ranwr
        call    puts
        mov     word ptr [fcb1+21h],1
        mov     word ptr [fcb1+23h],0
        mov     al,'X'
        mov     cx,RECSZ
        call    fillrec
        mov     dx,OFFSET fcb1
        mov     ah,22h
        int     21h
        or      al,al
        jnz     rw_bad
        mov     word ptr [fcb1+21h],1   ; and read it straight back
        mov     word ptr [fcb1+23h],0
        mov     dx,OFFSET fcb1
        mov     ah,21h
        int     21h
        or      al,al
        jnz     rw_bad
        mov     al,'X'
        mov     cx,RECSZ
        call    checkrec
        jne     rw_bad
        call    good
        jmp     short br
rw_bad:
        call    bad

;=====================================================================
; 27h - block read: three records in one call, and CX says how many
;=====================================================================
br:
        mov     dx,OFFSET t_blkrd
        call    puts
        mov     word ptr [fcb1+21h],0
        mov     word ptr [fcb1+23h],0
        mov     cx,3
        mov     dx,OFFSET fcb1
        mov     ah,27h
        int     21h
        or      al,al
        jnz     br_bad
        cmp     cx,3                    ; all three
        jne     br_bad
        cmp     byte ptr [dta],'0'      ; record 0...
        jne     br_bad
        cmp     byte ptr [dta+RECSZ],'X'        ; ...1 as 22h left it...
        jne     br_bad
        cmp     byte ptr [dta+2*RECSZ],'2'      ; ...and 2
        jne     br_bad
        ; a block call also advances the random record field, which a
        ; single random call does not
        cmp     word ptr [fcb1+21h],3
        jne     br_bad
        call    good
        jmp     short bw
br_bad:
        call    bad

;=====================================================================
; 28h - block write of two records past the end: the file grows
;=====================================================================
bw:
        mov     dx,OFFSET t_blkwr
        call    puts
        mov     word ptr [fcb1+21h],NRECS
        mov     word ptr [fcb1+23h],0
        mov     al,'Y'
        mov     cx,2 * RECSZ
        call    fillrec
        mov     cx,2
        mov     dx,OFFSET fcb1
        mov     ah,28h
        int     21h
        or      al,al
        jnz     bw_bad
        cmp     cx,2
        jne     bw_bad
        cmp     word ptr [fcb1+10h],(NRECS + 2) * RECSZ
        jne     bw_bad
        call    good
        jmp     short sz
bw_bad:
        call    bad

;=====================================================================
; 23h - the size in records, and 24h - where we are as a record number
;=====================================================================
sz:
        mov     dx,OFFSET t_size
        call    puts
        ; 23h READS THE DIRECTORY, so the file has to be closed first:
        ; the 96 bytes the block write made are in the kernel's table
        ; entry until then and the entry on the disk still says 64.
        ; Real DOS is the same - this is what a close is for - and the
        ; first version of this test asked before closing and failed,
        ; which is a test bug that reads exactly like a kernel one.
        mov     dx,OFFSET fcb1
        mov     ah,10h
        int     21h
        call    initfcb                 ; 23h wants an UNOPENED FCB
        mov     word ptr [fcb2+0Eh],RECSZ
        mov     dx,OFFSET fcb2
        mov     ah,23h
        int     21h
        or      al,al
        jnz     sz_bad
        cmp     word ptr [fcb2+21h],NRECS + 2
        jne     sz_bad
        cmp     word ptr [fcb2+23h],0
        jne     sz_bad
        call    good
        jmp     short srr
sz_bad:
        call    bad
srr:
        mov     dx,OFFSET t_setrr
        call    puts
        mov     word ptr [fcb1+0Ch],1   ; block 1, record 3 = record 131
        mov     byte ptr [fcb1+20h],3
        mov     word ptr [fcb1+21h],0
        mov     word ptr [fcb1+23h],0
        mov     dx,OFFSET fcb1
        mov     ah,24h
        int     21h
        cmp     word ptr [fcb1+21h],131
        jne     srr_bad
        cmp     word ptr [fcb1+23h],0
        jne     srr_bad
        call    good
        jmp     short shrt
srr_bad:
        call    bad

;=====================================================================
; 14h - the last record of a file that does not divide evenly.  With a
;       record size of 48 the 96-byte file is two whole records, so
;       ask for 40 instead: record 2 then has 16 bytes in it and the
;       answer is 3 - a partial record, zero filled to the end.
;=====================================================================
shrt:
        mov     dx,OFFSET t_part
        call    puts
        mov     word ptr [fcb1+0Eh],40
        mov     word ptr [fcb1+0Ch],0
        mov     byte ptr [fcb1+20h],2   ; bytes 80..119 of a 96-byte file
        mov     al,0FFh                 ; poison the DTA first, so that
        mov     cx,40                   ; "zero filled" means something
        call    fillrec
        mov     dx,OFFSET fcb1
        mov     ah,14h
        int     21h
        cmp     al,3
        jne     shrt_bad
        cmp     byte ptr [dta],'Y'      ; the 16 bytes that were there
        jne     shrt_bad
        cmp     byte ptr [dta+15],'Y'
        jne     shrt_bad
        cmp     byte ptr [dta+16],0     ; and the rest of the record
        jne     shrt_bad
        cmp     byte ptr [dta+39],0
        jne     shrt_bad
        call    good
        jmp     short eof
shrt_bad:
        call    bad

;--- and one past the end is 1, with nothing read at all
eof:
        mov     dx,OFFSET t_eof
        call    puts
        mov     dx,OFFSET fcb1
        mov     ah,14h
        int     21h
        cmp     al,1
        jne     eof_bad
        call    good
        jmp     short cl2
eof_bad:
        call    bad
cl2:
        mov     dx,OFFSET fcb1
        mov     ah,10h
        int     21h

;=====================================================================
; 11h / 12h - find first and find next
;
; Two files match FCB?????.DAT once the second one exists, and the
; answer lands in the DTA as an unopened FCB: drive byte, then the
; directory entry whole.
;=====================================================================
        mov     dx,OFFSET t_find
        call    puts
        call    makesecond
        mov     dx,OFFSET fcbwild
        mov     ah,11h
        int     21h
        or      al,al
        jnz     fnd_bad
        cmp     byte ptr [dta],0        ; the drive, and not zero: an FCB
        je      fnd_bad                 ; search answers with a real one
        mov     si,OFFSET dta           ; the name it found
        inc     si                      ; (OFFSET label+n is a syntax
                                        ;  error to wasm - two steps)
        mov     di,OFFSET n_fcbt
        mov     cx,3
        call    cmpbuf
        jne     fnd_bad
        mov     byte ptr [found],1
fnd_next:
        mov     dx,OFFSET fcbwild
        mov     ah,12h
        int     21h
        or      al,al
        jnz     fnd_done
        inc     byte ptr [found]
        cmp     byte ptr [found],10     ; a runaway search is a hang, and
        jb      fnd_next                ; a hang is not a test result
fnd_done:
        cmp     byte ptr [found],2
        jne     fnd_bad
        call    good
        jmp     short ren
fnd_bad:
        call    bad

;=====================================================================
; 17h - rename, with a wildcard in both halves
;=====================================================================
ren:
        mov     dx,OFFSET t_ren
        call    puts
        mov     dx,OFFSET fcbren
        mov     ah,17h
        int     21h
        or      al,al
        jnz     ren_bad
        call    initfcb                 ; is the new name there?
        mov     si,OFFSET n_bak
        mov     di,OFFSET fcb2
        add     di,9
        mov     cx,3
        call    copybuf
        mov     dx,OFFSET fcb2
        mov     ah,0Fh
        int     21h
        or      al,al
        jnz     ren_bad
        mov     dx,OFFSET fcb2
        mov     ah,10h
        int     21h
        call    good
        jmp     short del
ren_bad:
        call    bad

;=====================================================================
; 13h - delete everything that matches
;=====================================================================
del:
        mov     dx,OFFSET t_del
        call    puts
        mov     dx,OFFSET fcbwild2      ; FCB?????.*  - both of them
        mov     ah,13h
        int     21h
        or      al,al
        jnz     del_bad
        mov     dx,OFFSET fcbwild2      ; and nothing is left to delete
        mov     ah,13h
        int     21h
        cmp     al,0FFh
        jne     del_bad
        call    good
        jmp     short many
del_bad:
        call    bad

;=====================================================================
; FORTY FCBs, AND A KERNEL WITH THIRTY-TWO TABLE ENTRIES
;
; Make one file, then open it through forty FCBs and close none of
; them.  Every entry in the table is taken long before the fortieth,
; so the kernel has to close the FCB that has waited longest and
; re-open it from the name when it is used again.  The check is that
; the FIRST FCB - certainly recycled by now - still reads its own
; record.  This is the only test here that would notice.
;=====================================================================
many:
        mov     dx,OFFSET t_many
        call    puts
        call    makesecond              ; FCBTES2.DAT, 48 bytes of 'Y'
        mov     cx,40
        mov     di,OFFSET fcbpool
mny_lp:
        push    cx
        push    di
        mov     si,OFFSET fcb2nd
        mov     cx,37
        call    copybuf                 ; DI advances past the FCB
        pop     dx                      ; ...so the copy's start is the FCB
        push    dx
        mov     ah,0Fh
        int     21h
        or      al,al
        pop     di
        pop     cx
        jnz     mny_bad
        add     di,37
        loop    mny_lp

        ; and now the first one, which the kernel has certainly taken
        ; the table entry away from
        mov     word ptr [fcbpool+0Eh],RECSZ
        mov     word ptr [fcbpool+0Ch],0
        mov     byte ptr [fcbpool+20h],0
        mov     al,0FFh
        mov     cx,RECSZ
        call    fillrec
        mov     dx,OFFSET fcbpool
        mov     ah,14h
        int     21h
        or      al,al
        jnz     mny_bad
        mov     al,'Y'
        mov     cx,RECSZ
        call    checkrec
        jne     mny_bad
        call    good
        jmp     short fin
mny_bad:
        call    bad

fin:
        ; tidy up: the forty stay open on purpose and the exit sweep
        ; is what closes them, which is itself worth doing once
        mov     dx,OFFSET fcbwild2
        mov     ah,13h
        int     21h

        mov     dx,OFFSET msg_res
        call    puts
        mov     dx,OFFSET resbuf        ; one character per test, in order
        call    puts
        mov     dx,OFFSET msg_fails
        call    puts
        mov     al,[fails]
        call    put2
        mov     dx,OFFSET msg_bye
        call    puts
        mov     al,[fails]
        mov     ah,4Ch
        int     21h

;=====================================================================
; helpers
;=====================================================================

;--- initfcb - fcb1 and fcb2 back to a fresh unopened FCBTEST.DAT.
;--- Needed more often than it looks: a find writes its own state over
;--- the name field, and an open writes the drive it resolved to.
initfcb:
        push    ax
        push    cx
        push    si
        push    di
        mov     si,OFFSET fcb2blank
        mov     di,OFFSET fcb1
        mov     cx,37
        call    copybuf
        mov     si,OFFSET fcb2blank
        mov     di,OFFSET fcb2
        mov     cx,37
        call    copybuf
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

;--- makesecond - FCBTES2.DAT, a second file for the wildcards to find
;--- and rename and delete, with 48 bytes of 'Y' in it so that the
;--- forty-FCB test has something to read back
makesecond:
        push    ax
        push    cx
        push    dx
        push    si
        push    di
        mov     si,OFFSET fcb2nd
        mov     di,OFFSET fcbsec
        mov     cx,37
        call    copybuf
        mov     dx,OFFSET fcbsec
        mov     ah,16h
        int     21h
        mov     word ptr [fcbsec+0Eh],RECSZ
        mov     byte ptr [fcbsec+20h],0
        mov     cx,3
ms_lp:
        push    cx
        mov     al,'Y'
        mov     cx,RECSZ
        call    fillrec
        mov     dx,OFFSET fcbsec
        mov     ah,15h
        int     21h
        pop     cx
        loop    ms_lp
        mov     dx,OFFSET fcbsec
        mov     ah,10h
        int     21h
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     ax
        ret

;--- fillrec - CX bytes of AL at the DTA
fillrec:
        push    cx
        push    di
        mov     di,OFFSET dta
fr_lp:
        mov     [di],al
        inc     di
        loop    fr_lp
        pop     di
        pop     cx
        ret

;--- checkrec - are CX bytes at the DTA all AL?  ZF says.
checkrec:
        push    cx
        push    si
        mov     si,OFFSET dta
cr_lp:
        cmp     [si],al
        jne     cr_out
        inc     si
        loop    cr_lp
        cmp     al,al                   ; ZF, and CX is zero
cr_out:
        pop     si
        pop     cx
        ret

;--- cmpbuf - CX bytes at DS:SI against DS:DI.  ZF says.
cmpbuf:
        push    cx
        push    si
        push    di
cb_lp:
        mov     al,[si]
        cmp     al,[di]
        jne     cb_out
        inc     si
        inc     di
        loop    cb_lp
        cmp     al,al
cb_out:
        pop     di
        pop     si
        pop     cx
        ret

;--- copybuf - CX bytes from DS:SI to DS:DI, both left past the end
copybuf:
        push    ax
        push    cx
cp_lp:
        mov     al,[si]
        mov     [di],al
        inc     si
        inc     di
        loop    cp_lp
        pop     cx
        pop     ax
        ret

;--- good / bad - print the verdict, and ALSO leave one character of
;--- it in a line that survives the screen scrolling.  Twenty-two
;--- tests is more than a screen, and "which one failed" is not a
;--- question a photograph of the last twenty-five lines can answer.
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

;=====================================================================
; data
;=====================================================================
msg_hi      db  13,10,'FCBTEST - the file control block API',13,10,'$'
msg_bye     db  13,10,'FCBTEST done.',13,10,'$'
msg_res     db  13,10,'tests: $'
msg_fails   db  '  failures: $'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'

t_p1        db  '  29h parse MYFILE.TXT$'
t_p2        db  '  29h parse B:PROG.EXE$'
t_p3        db  '  29h parse *.* is eleven question marks$'
t_p4        db  '  29h a too-long name truncates and SI clears it$'
t_p5        db  '  29h a drive that does not exist answers FF$'
t_create    db  '  16h create, and the record size it writes$'
t_write     db  '  15h four sequential writes$'
t_close     db  '  10h close, and the size in the FCB$'
t_open      db  '  0Fh open it again$'
t_read      db  '  14h four sequential reads$'
t_ranrd     db  '  21h random read of record 2$'
t_ranwr     db  '  22h random write and read back$'
t_blkrd     db  '  27h block read of three records$'
t_blkwr     db  '  28h block write past the end$'
t_size      db  '  23h file size in records$'
t_setrr     db  '  24h set relative record$'
t_part      db  '  14h a short last record is 3, zero filled$'
t_eof       db  '  14h past the end is 1$'
t_find      db  '  11h/12h find first and find next$'
t_ren       db  '  17h rename through a wildcard$'
t_del       db  '  13h delete everything matching$'
t_many      db  '  40 FCBs open at once, and the first still reads$'

;--- 29h test material.  The expected images are twelve bytes: the
;--- drive byte, eight of name and three of extension.
p_plain     db  'MYFILE.TXT',0
e_plain     db  0,'MYFILE  TXT'
p_drive     db  'B:PROG.EXE',0
e_drive     db  2,'PROG    EXE'
p_star      db  '*.*',0
e_star      db  0,'???????????'
p_long      db  'LONGFILENAME.TXT'
p_long_end  db  0
e_long      db  0,'LONGFILETXT'
p_baddrv    db  'Z:FOO.BAR',0

n_fcbt      db  'FCB'
n_bak       db  'BAK'

;--- the FCBs.  Thirty-seven bytes each: drive, eight of name, three
;--- of extension, and twenty-five the kernel fills in.
fcb2blank   db  0,'FCBTEST DAT'
            db  25 dup(0)
fcb2nd      db  0,'FCBTES2 DAT'
            db  25 dup(0)
fcb1        db  0,'FCBTEST DAT'
            db  25 dup(0)
fcb2        db  0,'FCBTEST DAT'
            db  25 dup(0)
fcbsec      db  0,'FCBTES2 DAT'
            db  25 dup(0)
fcbwild     db  0,'FCB?????DAT'
            db  25 dup(0)
fcbwild2    db  0,'FCB????????'
            db  25 dup(0)

;--- a rename FCB carries both names: the pattern at +1 and what it
;--- becomes at +11h, with the fields an open would use in between.
;--- '?' in the NEW name means "keep whatever this file had here", so
;--- this renames both FCBTEST.DAT and FCBTES2.DAT to .BAK and leaves
;--- the names themselves alone - which is what the shell's REN *.DAT
;--- *.BAK comes down to.
fcbren      db  0,'FCBTES? DAT'
            db  5 dup(0)
            db  '????????BAK'
            db  9 dup(0)

pfcb        db  37 dup(0)               ; 29h's target
savsi       dw  0
found       db  0
fails       db  0
resptr      dw  OFFSET resbuf
resbuf      db  '$'
            db  40 dup(0)

dta         db  256 dup(0)
fcbpool     db  40 * 37 dup(0)

_FT     ENDS
        END
