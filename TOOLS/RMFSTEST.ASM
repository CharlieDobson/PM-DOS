;=====================================================================
; RMFSTEST.ASM - RMFSTEST.COM, the file system calls a DOS program
;                makes of RMDOS: writing, naming, and the questions.
;
; Run it on its own (PM-DOS - the control) and as
; PARKTEST \TESTS\RMFSTEST.COM (RMDOS).  Every line says ok or FAILED,
; and the exit code is how many failed.  In the "rme9" build the RMDOS
; half is also in DOSBox-X's log, since 09h is traced.
;
; WHY IT EXISTS.  Windows 3.1 in standard mode runs its DOS sessions on
; RMDOS, and the first time one was started - through WINOLDAP, which
; begins by writing a swap-information file - RMDOS failed the write,
; and then, looked at closely, turned out never to have written a
; second cluster onto a FAT16 file, freed a chain, created a file or
; deleted one without damaging the directory around it.  Windows' own
; start-up only reads, and nothing else had ever asked.  So this asks:
;
;   the write      C:\RMFSDIR\~WOA100F.TMP and C:\RMFSTEST.TMP exactly
;                  as WINOLDAP writes one - create, seek to 4, write 16,
;                  8 and 4592 bytes - then read back and compared
;   the names      MD, CD and 47h; TRUENAME with ".." and "." and a
;                  device in it; REN in place and REN as a move; RD of
;                  a directory that is not empty, that is current, and
;                  that is empty
;   the questions  29h into an FCB, 1Ch, 6523h, 6601h, 69h - and that
;                  a file closed after 69h is still in its directory
;   the truncate   3Ch of a file that is there empties it, and leaves
;                  the volume readable
;
; What it cannot see - that the OTHER entries in a directory sector
; survived, and that no cluster was lost - has to be checked from
; outside, by listing the image's directories and walking its FAT
; before and after the run: two of RMDOS's bugs damaged only entries
; the program never looks at (see NOTES.TXT, v0.60).
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_RF     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_RF, DS:_RF, ES:_RF, SS:_RF
        ORG     0

BIGLEN  EQU     4592
TOTAL   EQU     16 + 8 + BIGLEN         ; what is written, from offset 4

start:
        mov     dx,OFFSET m_hi
        call    puts

        ; the pattern: byte i = i*7+3
        mov     di,OFFSET wbuf
        xor     ax,ax
        mov     cx,TOTAL
fill:
        imul    bx,ax,7
        add     bl,3
        mov     [di],bl
        inc     di
        inc     ax
        loop    fill

;=====================================================================
; the write
;=====================================================================
        mov     dx,OFFSET d_top
        mov     ah,39h
        int     21h
        mov     dx,OFFSET t_md
        call    expect_ok

        mov     dx,OFFSET f_sub
        call    one_file
        mov     dx,OFFSET f_root
        call    one_file

;=====================================================================
; the names
;=====================================================================
        ; MD of something that is there already is refused with 5
        mov     dx,OFFSET d_top
        mov     ah,39h
        int     21h
        mov     dx,OFFSET t_mdagain
        mov     bx,5
        call    expect_err

        ; CD there, and 47h says so
        mov     dx,OFFSET d_top
        mov     ah,3Bh
        int     21h
        mov     dx,OFFSET t_cd
        call    expect_ok
        mov     si,OFFSET got
        xor     dl,dl
        mov     ah,47h
        int     21h
        mov     si,OFFSET got
        mov     di,OFFSET s_cwd
        mov     dx,OFFSET t_cwd
        call    expect_str

        ; TRUENAME: relative, dotted, and a device
        mov     si,OFFSET n_rel
        mov     di,OFFSET s_rel
        mov     dx,OFFSET t_tnrel
        call    truename
        mov     si,OFFSET n_dots
        mov     di,OFFSET s_dots
        mov     dx,OFFSET t_tndots
        call    truename
        mov     si,OFFSET n_con
        mov     di,OFFSET s_con
        mov     dx,OFFSET t_tncon
        call    truename

        ; a file to rename: F1.TXT, in the current directory
        mov     dx,OFFSET n_f1
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      nm_nof1
        mov     bx,ax
        mov     dx,OFFSET wbuf
        mov     cx,100
        mov     ah,40h
        int     21h
        mov     ah,3Eh
        int     21h
nm_nof1:
        ; REN F1.TXT F2.TXT: in place
        mov     dx,OFFSET n_f1
        mov     di,OFFSET n_f2
        mov     ah,56h
        int     21h
        mov     dx,OFFSET t_ren
        call    expect_ok
        mov     dx,OFFSET n_f2
        mov     di,OFFSET t_renthere
        call    exists
        mov     dx,OFFSET n_f1
        mov     di,OFFSET t_rengone
        call    absent

        ; MD SUB, then move F2.TXT into it as F3.TXT
        mov     dx,OFFSET n_sub
        mov     ah,39h
        int     21h
        mov     dx,OFFSET t_mdsub
        call    expect_ok
        mov     dx,OFFSET n_f2
        mov     di,OFFSET n_f3
        mov     ah,56h
        int     21h
        mov     dx,OFFSET t_move
        call    expect_ok
        mov     dx,OFFSET n_f3
        mov     di,OFFSET t_movethere
        call    exists
        mov     dx,OFFSET n_f2
        mov     di,OFFSET t_movegone
        call    absent

        ; RD SUB is refused while F3.TXT is in it
        mov     dx,OFFSET n_sub
        mov     ah,3Ah
        int     21h
        mov     dx,OFFSET t_rdfull
        mov     bx,5
        call    expect_err
        mov     dx,OFFSET n_f3
        mov     ah,41h
        int     21h
        mov     dx,OFFSET n_sub
        mov     ah,3Ah
        int     21h
        mov     dx,OFFSET t_rdsub
        call    expect_ok

        ; RD of where we are standing is refused with 16
        mov     dx,OFFSET d_top
        mov     ah,3Ah
        int     21h
        mov     dx,OFFSET t_rdcur
        mov     bx,16
        call    expect_err

        ; CD .. goes up, and 47h follows
        mov     dx,OFFSET n_up
        mov     ah,3Bh
        int     21h
        mov     dx,OFFSET t_cdup
        call    expect_ok
        mov     si,OFFSET got
        xor     dl,dl
        mov     ah,47h
        int     21h
        mov     si,OFFSET got
        mov     di,OFFSET s_root
        mov     dx,OFFSET t_cwdroot
        call    expect_str

        ; and now it can go
        mov     dx,OFFSET d_top
        mov     ah,3Ah
        int     21h
        mov     dx,OFFSET t_rdtop
        call    expect_ok
        mov     dx,OFFSET d_top
        mov     ah,3Bh
        int     21h
        mov     dx,OFFSET t_rdgone
        mov     bx,3
        call    expect_err

;=====================================================================
; the questions
;=====================================================================
        ; 29h, bit 0 set: "c:file*.t?t" -> 3, FILE????, T?T, AL=1
        mov     si,OFFSET p_one
        mov     di,OFFSET fcb
        mov     ax,2901h
        int     21h
        mov     [res],al
        mov     si,OFFSET fcb
        mov     di,OFFSET e_one
        mov     cx,12
        mov     dx,OFFSET t_parse1
        call    expect_mem
        cmp     byte ptr [res],1
        mov     dx,OFFSET t_parse1w
        call    expect_z

        ; "x.y" with AL=0 -> 0, X, Y, AL=0
        mov     si,OFFSET p_two
        mov     di,OFFSET fcb
        mov     ax,2900h
        int     21h
        mov     [res],al
        mov     si,OFFSET fcb
        mov     di,OFFSET e_two
        mov     cx,12
        mov     dx,OFFSET t_parse2
        call    expect_mem
        cmp     byte ptr [res],0
        mov     dx,OFFSET t_parse2w
        call    expect_z

        ; "q:x" - there is no Q: - AL=FFh
        mov     si,OFFSET p_three
        mov     di,OFFSET fcb
        mov     ax,2900h
        int     21h
        cmp     al,0FFh
        mov     dx,OFFSET t_parse3
        call    expect_z

        ; 1Ch for C: - 512 bytes a sector and an F8h media byte
        push    ds
        mov     dl,3
        mov     ah,1Ch
        int     21h
        mov     ah,[bx]
        pop     ds
        cmp     cx,512
        jne     q1c_bad
        cmp     ah,0F8h
q1c_bad:
        mov     dx,OFFSET t_1c
        call    expect_z

        ; 6523h: y, n, and neither
        mov     dl,'y'
        mov     ax,6523h
        int     21h
        cmp     ax,1
        jne     q23_bad
        mov     dl,'N'
        mov     ax,6523h
        int     21h
        cmp     ax,0
        jne     q23_bad
        mov     dl,'x'
        mov     ax,6523h
        int     21h
        cmp     ax,2
q23_bad:
        mov     dx,OFFSET t_6523
        call    expect_z

        ; 6601h: the code page is 437
        mov     ax,6601h
        int     21h
        cmp     bx,437
        mov     dx,OFFSET t_6601
        call    expect_z

        ; 69h for C: - info level 0 and a FAT16 volume
        mov     bl,3
        mov     dx,OFFSET got
        mov     ax,6900h
        int     21h
        jc      q69_no
        cmp     word ptr [got],0
        jne     q69_bad
        mov     si,OFFSET got + 2 + 4 + 11
        mov     di,OFFSET s_fat16
        mov     cx,8
        repe    cmpsb
        jmp     short q69_bad
q69_no:
        or      sp,sp                   ; NZ: the carry says nothing of ZF
q69_bad:
        mov     dx,OFFSET t_69
        call    expect_z

        ; ...and the directory is still there afterwards.  PM-DOS's 69h
        ; read the boot sector into the kernel's DIRECTORY cache without
        ; saying so, and the next entry update wrote it over whichever
        ; directory sector the cache had held - found as a root with a
        ; boot sector where its second sector should be.  So: create a
        ; file, 69h, and the close that writes the file's size, then
        ; ask whether the file is still there and 20 bytes long.
        mov     dx,OFFSET f_69
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      q69d_no
        mov     [hnd],ax
        mov     bx,ax
        mov     dx,OFFSET wbuf
        mov     cx,20
        mov     ah,40h
        int     21h
        mov     bl,3
        mov     dx,OFFSET got
        mov     ax,6900h
        int     21h
        mov     bx,[hnd]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET f_69
        mov     ax,3D00h
        int     21h
        jc      q69d_no
        mov     bx,ax
        mov     ax,4202h
        xor     cx,cx
        xor     dx,dx
        int     21h
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        cmp     ax,20
        jmp     short q69d_z
q69d_no:
        or      sp,sp
q69d_z:
        mov     dx,OFFSET t_69dir
        call    expect_z

        ; 3Ch of a name that is there already TRUNCATES it.  RMDOS's did
        ; not: the SFT it wrote the new length through was the VOLUME
        ; RECORD, so the file kept its length and the volume lost the
        ; address of its FAT.  And every plain create set a bit in the
        ; top byte of the volume's start sector the same way, which is
        ; what the 69h above found.  So: create it again, it is empty,
        ; and 69h - which reads the boot sector through that record -
        ; still works afterwards.
        mov     dx,OFFSET f_69
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      q3c_no
        mov     bx,ax
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET f_69
        mov     ax,3D00h
        int     21h
        jc      q3c_no
        mov     bx,ax
        mov     ax,4202h
        xor     cx,cx
        xor     dx,dx
        int     21h
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        or      ax,ax
        jmp     short q3c_z
q3c_no:
        or      sp,sp
q3c_z:
        mov     dx,OFFSET t_trunc
        call    expect_z
        mov     bl,3
        mov     dx,OFFSET got
        mov     ax,6900h
        int     21h
        jc      q3c_vno
        cmp     word ptr [got],0
        jmp     short q3c_vz
q3c_vno:
        or      sp,sp
q3c_vz:
        mov     dx,OFFSET t_after
        call    expect_z

        mov     dx,OFFSET f_69
        mov     ah,41h
        int     21h

;=====================================================================
        mov     dx,OFFSET m_fails
        call    puts
        mov     al,[fails]
        call    put2
        call    crlf
        mov     al,[fails]
        mov     ah,4Ch
        int     21h

;--- one_file - DX = the name.  Written as WINOLDAP writes one, read
;--- back, compared, deleted.
one_file:
        mov     [fname],dx
        mov     dx,OFFSET m_file
        call    puts
        mov     si,[fname]
        call    putz
        call    crlf

        mov     dx,[fname]
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jnc     of_made
        mov     dx,OFFSET t_create
        call    fail
        call    crlf
        ret
of_made:
        mov     [hnd],ax
        mov     bx,ax
        mov     ax,4200h
        xor     cx,cx
        mov     dx,4
        int     21h
        mov     dx,OFFSET wbuf
        mov     cx,16
        call    wr
        jc      of_failw
        mov     dx,OFFSET wbuf + 16
        mov     cx,8
        call    wr
        jc      of_failw
        mov     dx,OFFSET wbuf + 24
        mov     cx,BIGLEN
        call    wr
        jc      of_failw
        mov     dx,OFFSET t_wrote
        call    pass
        mov     bx,[hnd]
        mov     ah,3Eh
        int     21h

        mov     dx,[fname]
        mov     ax,3D00h
        int     21h
        jc      of_noopen
        mov     [hnd],ax
        mov     bx,ax
        mov     dx,OFFSET rbuf
        mov     cx,TOTAL + 4 + 16       ; asking past the end is fine
        mov     ah,3Fh
        int     21h
        jc      of_short
        cmp     ax,TOTAL + 4
        jne     of_short
        mov     dx,OFFSET t_size
        call    pass
        mov     si,OFFSET wbuf
        mov     di,OFFSET rbuf + 4
        mov     cx,TOTAL
        repe    cmpsb
        mov     dx,OFFSET t_same
        call    expect_z
        jmp     short of_close
of_short:
        mov     dx,OFFSET t_size
        call    fail
        call    crlf
of_close:
        mov     bx,[hnd]
        mov     ah,3Eh
        int     21h
        mov     dx,[fname]
        mov     ah,41h
        int     21h
        mov     dx,OFFSET t_del
        call    expect_ok
        ret
of_noopen:
        mov     dx,OFFSET t_reopen
        call    fail
        call    crlf
        ret
of_failw:
        mov     [err],ax
        mov     dx,OFFSET t_write
        call    fail
        mov     ax,[err]
        call    put4
        call    crlf
        mov     bx,[hnd]
        mov     ah,3Eh
        int     21h
        mov     dx,[fname]
        mov     ah,41h
        int     21h
        ret

; wr - [hnd], DS:DX, CX.  CF and AX = the error; a short write is FFFFh.
wr:
        mov     bx,[hnd]
        push    cx
        mov     ah,40h
        int     21h
        pop     cx
        jc      wr_out
        cmp     ax,cx
        je      wr_out
        mov     ax,0FFFFh
        stc
wr_out:
        ret

;--- truename - SI = the name, DI = what it must come back as, DX = the
;--- label.
truename:
        push    dx
        push    di
        mov     di,OFFSET got
        mov     ah,60h
        int     21h
        pop     di
        pop     dx
        jc      tn_fail
        mov     si,OFFSET got
        jmp     expect_str
tn_fail:
        call    fail
        call    crlf
        ret

;--- exists / absent - DX = a name, DI = the label.  4Eh finds it or not.
exists:
        push    di
        mov     cx,27h
        mov     ah,4Eh
        int     21h
        pop     dx
        jmp     expect_ok
absent:
        push    di
        mov     cx,27h
        mov     ah,4Eh
        int     21h
        pop     dx
        jnc     ab_there
        jmp     pass
ab_there:
        call    fail
        jmp     crlf

;--- the verdicts.  DX = the label every time.
expect_ok:                              ; CF clear
        jc      ek_fail
        jmp     pass
ek_fail:
        mov     [err],ax
        call    fail
        mov     ax,[err]
        call    put4
        jmp     crlf
expect_err:                             ; CF set with AX = BX
        jnc     ee_fail
        cmp     ax,bx
        jne     ee_fail
        jmp     pass
ee_fail:
        mov     [err],ax
        call    fail
        mov     ax,[err]
        call    put4
        jmp     crlf
expect_z:                               ; ZF set
        jz      pass
        call    fail
        jmp     crlf
expect_str:                             ; ASCIIZ at SI equals ASCIIZ at DI
        push    si
esr_cmp:
        mov     al,[si]
        cmp     al,[di]
        jne     esr_bad
        inc     si
        inc     di
        or      al,al
        jnz     esr_cmp
        pop     si
        jmp     pass
esr_bad:
        pop     si
        call    fail
        mov     dl,'"'
        call    putc
        call    putz
        mov     dl,'"'
        call    putc
        jmp     crlf
expect_mem:                             ; CX bytes at SI equal DI's
        repe    cmpsb
        je      pass
        call    fail
        jmp     crlf

pass:
        call    puts
        mov     dx,OFFSET m_ok
        jmp     puts
fail:
        inc     byte ptr [fails]
        call    puts
        mov     dx,OFFSET m_failed
        jmp     puts

puts:
        mov     ah,09h
        int     21h
        ret
putz:
        lodsb
        or      al,al
        jz      pz_out
        mov     dl,al
        call    putc
        jmp     short putz
pz_out:
        ret
putc:
        mov     ah,02h
        int     21h
        ret
crlf:
        mov     dx,OFFSET m_crlf
        jmp     puts
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
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1_out
        add     al,7
p1_out:
        mov     dl,al
        jmp     putc

fails       db  0
res         db  0
hnd         dw  0
fname       dw  0
err         dw  0

m_hi        db  'RMFSTEST - writing, naming, and the questions',13,10,'$'
m_file      db  '  $'
m_ok        db  ' ok',13,10,'$'
m_failed    db  ' FAILED $'
m_fails     db  'checks failed: $'
m_crlf      db  13,10,'$'
t_md        db  '  39h MD C:\RMFSDIR$'
t_create    db  '    3Ch create$'
t_write     db  '    40h write 16, 8 and 4592 bytes - error$'
t_wrote     db  '    40h wrote 16, 8 and 4592 bytes$'
t_reopen    db  '    3Dh open it again$'
t_size      db  '    it is 4620 bytes long$'
t_same      db  '    and every byte after the first four is what was written$'
t_del       db  '    41h deleted it$'
t_mdagain   db  '  39h MD of a name already there says 5$'
t_cd        db  '  3Bh CD C:\RMFSDIR$'
t_cwd       db  '  47h says RMFSDIR$'
t_tnrel     db  '  60h TRUENAME x.txt$'
t_tndots    db  '  60h TRUENAME ..\A\.\B$'
t_tncon     db  '  60h TRUENAME con$'
t_ren       db  '  56h REN F1.TXT F2.TXT$'
t_renthere  db  '    F2.TXT is there$'
t_rengone   db  '    F1.TXT is not$'
t_mdsub     db  '  39h MD SUB$'
t_move      db  '  56h REN F2.TXT SUB\F3.TXT, a move$'
t_movethere db  '    SUB\F3.TXT is there$'
t_movegone  db  '    F2.TXT is not$'
t_rdfull    db  '  3Ah RD of a directory with a file in it says 5$'
t_rdsub     db  '  3Ah RD SUB once it is empty$'
t_rdcur     db  '  3Ah RD of the current directory says 16$'
t_cdup      db  '  3Bh CD ..$'
t_cwdroot   db  '  47h says the root$'
t_rdtop     db  '  3Ah RD C:\RMFSDIR$'
t_rdgone    db  '    and CD to it says 3$'
t_parse1    db  '  29h c:file*.t?t fills 3 FILE???? T?T$'
t_parse1w   db  '    and says it had wildcards$'
t_parse2    db  '  29h x.y fills 0 X Y$'
t_parse2w   db  '    and says it had none$'
t_parse3    db  '  29h q:x says FFh - there is no Q:$'
t_1c        db  '  1Ch C: - 512-byte sectors, media F8$'
t_6523      db  '  6523h y is yes, N is no, x is neither$'
t_6601      db  '  6601h the code page is 437$'
t_69        db  '  6900h C: - info level 0, FAT16$'
t_69dir     db  '    and a file closed after it is still in the directory$'
t_trunc     db  '  3Ch of a file that is there empties it$'
t_after     db  '    and 69h still reads the volume afterwards$'

d_top       db  'C:\RMFSDIR',0
f_sub       db  'C:\RMFSDIR\~WOA100F.TMP',0
f_root      db  'C:\RMFSTEST.TMP',0
f_69        db  'C:\RMFS69.TMP',0
s_cwd       db  'RMFSDIR',0
n_rel       db  'x.txt',0
s_rel       db  'C:\RMFSDIR\X.TXT',0
n_dots      db  '..\A\.\B',0
s_dots      db  'C:\A\B',0
n_con       db  'con',0
s_con       db  'C:/CON',0
n_f1        db  'F1.TXT',0
n_f2        db  'F2.TXT',0
n_f3        db  'SUB\F3.TXT',0
n_sub       db  'SUB',0
n_up        db  '..',0
s_root      db  0
s_fat16     db  'FAT16   '
p_one       db  'c:file*.t?t',0
e_one       db  3,'FILE????T?T'
p_two       db  'x.y',0
e_two       db  0,'X       Y  '
p_three     db  'q:x',0

fcb         db  37 dup (0)
got         db  128 dup (0)
wbuf        db  TOTAL dup (0)
rbuf        db  TOTAL + 4 + 16 dup (0)

_RF     ENDS
        END     start
