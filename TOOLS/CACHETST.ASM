;=====================================================================
; CACHETST.ASM - CACHETST.COM, the disk cache asked the questions
;                SMARTDRV.EXE and SMARTMON ask it.
;
; SIXTEEN-BIT ON PURPOSE, for the same reason CDXTEST is: the
; interface being tested is INT 2Fh AX=4A10h, it is answered by
; DOS\CACHE.INC out of the V86 monitor's own 2Fh chain, and a 32-bit
; native program cannot reach that path at all.  So the only place it
; can be asked from is a DOS box.
;
; The register conventions here are not invented.  They come out of
; S:\DOS6SRC\dev\smartdrv\int2f.asm and its header block, which is
; SMARTDRV 4.0 itself, and are the same ones SMARTMON's int2f.asm
; calls through.
;
; Ten checks, in the order a program would really make them:
;
;   1  4A10h/0000h  is a cache there - AX = BABEh, and BP the version
;   2  4A10h/0004h  the element size and how many elements
;   3  read a file, then ask again: hits + misses must have MOVED.
;      A cache that answers the interface and does not cache is the
;      failure this catches and nothing else would
;   4  AH=0Dh then 4A10h/0000h: dirty elements must be ZERO.  That is
;      the whole promise the write cache is allowed to exist on
;   5  write a file, and the dirty count must go UP without a commit
;   6  commit again, and it must come back to zero
;   7  4A10h/0003h on C: - a fixed disk, so read AND write caching
;   8  4A10h/0003h on A: - a floppy, so read caching and NOT write
;   9  turn write caching off on C: and read the state back
;  10  turn it on again, because a test that leaves the machine
;      configured differently is not a test, it is a change
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_CT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_CT, DS:_CT, ES:_CT, SS:_CT
        ORG     0

BAMBI           EQU     4A10h
BAMBI_SIG       EQU     0BABEh

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; 1 - is there a cache?
;=====================================================================
        mov     dx,OFFSET msg_t1
        call    puts
        call    getstats
        cmp     ax,BAMBI_SIG
        je      c1_yes
        mov     dx,OFFSET msg_nocache
        call    puts
        jmp     c_bye
c1_yes:
        call    ok
        mov     dx,OFFSET msg_ver
        call    puts
        mov     al,byte ptr [s_ver+1]   ; BCD: 04 00 -> "4.00"
        call    put2
        mov     dl,'.'
        call    putc
        mov     al,byte ptr [s_ver]
        call    put2
        call    crlf

;=====================================================================
; 2 - the element size and the element count
;=====================================================================
        mov     dx,OFFSET msg_t2
        call    puts
        mov     ax,BAMBI
        mov     bx,4
        int     2Fh
        mov     [e_size],cx
        mov     [e_count],ax
        mov     ax,cx
        call    put4
        mov     dx,OFFSET msg_bytesx
        call    puts
        mov     ax,[e_count]
        call    put4
        call    crlf
        cmp     word ptr [e_size],0
        je      c2_bad
        cmp     word ptr [e_count],0
        je      c2_bad
        call    ok
        jmp     short c3
c2_bad:
        call    bad

;=====================================================================
; 3 - does it actually cache?  Read a file twice and watch the
;     counters move.  Both of them, because a cache that only ever
;     misses is as wrong as one that never runs.
;=====================================================================
c3:
        mov     dx,OFFSET msg_t3
        call    puts
        call    getstats
        mov     ax,[s_hitlo]
        mov     [p_hit],ax
        mov     ax,[s_misslo]
        mov     [p_miss],ax

        call    readself
        jnc     c3_read
        mov     dx,OFFSET msg_noopen
        call    puts
        jmp     short c4
c3_read:
        call    readself                ; twice: the second is the hit
        call    getstats
        mov     ax,[s_hitlo]
        cmp     ax,[p_hit]
        jne     c3_moved
        mov     ax,[s_misslo]
        cmp     ax,[p_miss]
        je      c3_still
c3_moved:
        call    ok
        jmp     short c4
c3_still:
        call    bad

;=====================================================================
; 4 - AH=0Dh, and then nothing is owed
;=====================================================================
c4:
        mov     dx,OFFSET msg_t4
        call    puts
        mov     ah,0Dh
        int     21h
        call    getstats
        cmp     word ptr [s_dirty],0
        jne     c4_bad
        call    ok
        jmp     short c5
c4_bad:
        mov     ax,[s_dirty]
        call    put4
        call    bad

;=====================================================================
; 5 - write something, and it is owed
;
; The file is created, written and CLOSED, all of which the cache is
; allowed to defer; a non-zero dirty count here is the write cache
; doing its job, and a zero one means every write went to the medium
; as it was made, which is correct behaviour for CACHE=...,OFF and a
; failure for the default.
;=====================================================================
c5:
        mov     dx,OFFSET msg_t5
        call    puts
        call    writescratch
        jnc     c5_wrote
        mov     dx,OFFSET msg_nowrite
        call    puts
        jmp     short c7
c5_wrote:
        call    getstats
        mov     ax,[s_dirty]
        call    put4
        call    crlf
        cmp     word ptr [s_dirty],0
        je      c5_none
        call    ok
        jmp     short c6
c5_none:
        mov     dx,OFFSET msg_lazyoff
        call    puts

;=====================================================================
; 6 - and one commit later, nothing is
;=====================================================================
c6:
        mov     dx,OFFSET msg_t6
        call    puts
        mov     ax,BAMBI                ; through the interface this time,
        mov     bx,1                    ; not through AH=0Dh - they have to
        int     2Fh                     ; mean the same thing
        call    getstats
        cmp     word ptr [s_dirty],0
        jne     c6_bad
        call    ok
        jmp     short c7
c6_bad:
        call    bad

;=====================================================================
; 7 - C: is a fixed disk: read AND write caching
;
; DL comes back in SMARTDRV's inverted spelling: bit 7 set means read
; caching is OFF, bit 6 set means write caching is off.  So a fixed
; disk with both on answers with neither bit.
;=====================================================================
c7:
        mov     dx,OFFSET msg_t7
        call    puts
        mov     bp,2                    ; unit 2 = C:
        xor     dx,dx                   ; get, change nothing
        call    drvstate
        jc      c7_bad
        call    showstate
        test    dl,0C0h
        jnz     c7_bad
        call    ok
        jmp     short c8
c7_bad:
        call    bad

;=====================================================================
; 8 - A: is a floppy: read caching and NOT write caching
;=====================================================================
c8:
        mov     dx,OFFSET msg_t8
        call    puts
        mov     bp,0                    ; unit 0 = A:
        xor     dx,dx
        call    drvstate
        jc      c8_skip
        call    showstate
        test    dl,80h                  ; read caching must be ON
        jnz     c8_bad
        test    dl,40h                  ; write caching must be OFF
        jz      c8_bad
        call    ok
        jmp     short c9
c8_skip:
        mov     dx,OFFSET msg_nodrv     ; no A: on this machine, which is
        call    puts                    ; not a failure of the cache
        jmp     short c9
c8_bad:
        call    bad

;=====================================================================
; 9 - turn write caching off on C: and read it back
;=====================================================================
c9:
        mov     dx,OFFSET msg_t9
        call    puts
        mov     bp,2
        mov     dx,4                    ; disable write caching
        call    drvstate
        jc      c9_bad
        mov     bp,2
        xor     dx,dx
        call    drvstate
        jc      c9_bad
        call    showstate
        test    dl,40h
        jz      c9_bad
        call    ok
        jmp     short c10
c9_bad:
        call    bad

;=====================================================================
; 10 - and back on, because the machine must be left as it was found
;=====================================================================
c10:
        mov     dx,OFFSET msg_t10
        call    puts
        mov     bp,2
        mov     dx,3                    ; enable write caching
        call    drvstate
        jc      c10_bad
        mov     bp,2
        xor     dx,dx
        call    drvstate
        jc      c10_bad
        call    showstate
        test    dl,40h
        jnz     c10_bad
        call    ok
        jmp     short c_done
c10_bad:
        call    bad

c_done:
        mov     dx,OFFSET msg_sum
        call    puts
        mov     al,[failures]
        call    put2d
        mov     dx,OFFSET msg_failed
        call    puts
c_bye:
        mov     ax,4C00h
        int     21h

;=====================================================================
; getstats - 4A10h/0000h into the s_ variables.  AX = the signature.
;=====================================================================
getstats:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        mov     ax,BAMBI
        xor     bx,bx
        int     2Fh
        mov     [s_hitlo],bx
        mov     [s_hithi],dx
        mov     [s_misslo],si
        mov     [s_misshi],di
        mov     [s_dirty],cx
        mov     [s_ver],bp
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

;=====================================================================
; drvstate - BP = unit, DX = the function.  DL = the state on return,
;            CF set if the drive does not exist or is not cachable.
;=====================================================================
drvstate:
        push    bx
        push    cx
        push    si
        push    di
        mov     ax,BAMBI
        mov     bx,3
        int     2Fh
        cmp     ax,BAMBI_SIG
        jne     ds_no
        cmp     dl,0FFh
        je      ds_no
        pop     di
        pop     si
        pop     cx
        pop     bx
        clc
        ret
ds_no:
        pop     di
        pop     si
        pop     cx
        pop     bx
        stc
        ret

;--- print DL as "r+w+", "r+w-" and so on ----------------------------
showstate:
        push    ax
        push    dx
        mov     [st_byte],dl
        mov     dl,'r'
        call    putc
        mov     dl,'+'
        test    byte ptr [st_byte],80h
        jz      ss_r
        mov     dl,'-'
ss_r:
        call    putc
        mov     dl,'w'
        call    putc
        mov     dl,'+'
        test    byte ptr [st_byte],40h
        jz      ss_w
        mov     dl,'-'
ss_w:
        call    putc
        mov     dl,' '
        call    putc
        pop     dx
        pop     ax
        ret

;=====================================================================
; readself - open this program's own file and read it to the end.
;            CF set if it would not open.
;
; Its own file because it is the one file that is certainly there,
; certainly on the drive the machine booted from, and certainly big
; enough to be more than one cache block.
;=====================================================================
readself:
        push    bx
        push    cx
        push    dx
        mov     dx,OFFSET selfname
        mov     ax,3D00h
        int     21h
        jc      rs_no
        mov     bx,ax
rs_lp:
        mov     ah,3Fh
        mov     cx,512
        mov     dx,OFFSET scratch
        int     21h
        jc      rs_shut
        or      ax,ax
        jnz     rs_lp
rs_shut:
        mov     ah,3Eh
        int     21h
        pop     dx
        pop     cx
        pop     bx
        clc
        ret
rs_no:
        pop     dx
        pop     cx
        pop     bx
        stc
        ret

;=====================================================================
; writescratch - create, write and close a small file.  CF if any of
;                that failed.  The file is deleted again afterwards,
;                which is itself more writing and therefore more of
;                what is being measured.
;=====================================================================
writescratch:
        push    bx
        push    cx
        push    dx
        mov     dx,OFFSET wrname
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      ws_no
        mov     bx,ax
        mov     ah,40h
        mov     cx,512
        mov     dx,OFFSET scratch
        int     21h
        jc      ws_shut
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET wrname
        mov     ah,41h                  ; and take it away again
        int     21h
        pop     dx
        pop     cx
        pop     bx
        clc
        ret
ws_shut:
        mov     ah,3Eh
        int     21h
ws_no:
        pop     dx
        pop     cx
        pop     bx
        stc
        ret

;=====================================================================
; The three verdicts, so that every check reads the same way down the
; screen and a missing one is a gap rather than prose.
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

;--- AX in hex, four digits ------------------------------------------
put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
        call    put2
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
st_byte     db  0
s_hitlo     dw  0
s_hithi     dw  0
s_misslo    dw  0
s_misshi    dw  0
s_dirty     dw  0
s_ver       dw  0
e_size      dw  0
e_count     dw  0
p_hit       dw  0
p_miss      dw  0

selfname    db  'C:\TESTS\CACHETST.COM',0
wrname      db  'C:\CACHETMP.$$$',0

msg_hi      db  13,10,'CACHETST - the disk cache through INT 2Fh '
            db  'AX=4A10h',13,10
            db  '-------------------------------------------------'
            db  13,10,'$'
msg_t1      db  '4A10/0000 installed?    $'
msg_t2      db  '4A10/0004 element size  $'
msg_t3      db  'does it really cache?   $'
msg_t4      db  '21h/0Dh commits         $'
msg_t5      db  'a write is owed         $'
msg_t6      db  '4A10/0001 commits       $'
msg_t7      db  'C: read+write cached    $'
msg_t8      db  'A: read cached only     $'
msg_t9      db  'write caching off on C: $'
msg_t10     db  '...and on again         $'
msg_ok      db  'OK',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_ver     db  '          version       $'
msg_bytesx  db  ' bytes x $'
msg_nocache db  'no cache installed - nothing to test',13,10,'$'
msg_noopen  db  'cannot open my own file - skipped',13,10,'$'
msg_nowrite db  'cannot write to C: - skipped',13,10,'$'
msg_nodrv   db  'no such drive - skipped',13,10,'$'
msg_lazyoff db  'nothing owed: lazy writing is off',13,10,'$'
msg_sum     db  13,10,'CACHETST done, $'
msg_failed  db  ' failed',13,10,'$'

scratch     db  512 dup(0)

_CT     ENDS
        END     start
