;=====================================================================
; EMSTEST.ASM - EMSTEST.COM, expanded memory measured from a DOS box.
;
; EMS is the one memory API where a manager can answer every question
; correctly and still be useless, because the answers are about a
; window and the window is a piece of hardware.  A manager that keeps
; a handle table, hands out handles, remembers what 44h was told and
; reports it faithfully through 4Ch and 4Eh will pass any test built
; out of return codes - and the frame will contain nothing at all.
;
; So the checks that matter here WRITE THROUGH THE WINDOWS AND READ
; BACK, and the one that cannot be faked at all is check 9: four
; windows, four different logical pages, four different patterns, all
; visible at once.  Nothing but four separate mappings produces that.
;
; Fourteen checks:
;
;    1  the INT 67h vector's segment is named EMMXXXX0
;    2  46h reports a version
;    3  41h hands back a 64K-aligned frame in upper memory
;    4  42h: the system has pages, and free is not more than total
;    5  43h allocates four pages and answers with a handle
;    6  4Ch says that handle owns four
;    7  4Bh counts one open handle
;    8  44h maps logical 0 into window 0, and the memory is there
;    9  four windows show four different pages AT ONCE
;   10  remapping a window changes what is behind it, and the page it
;       stopped showing still holds what was written to it
;   11  47h parks the page map under the handle
;   12  45h refuses to deallocate while it is parked
;   13  48h puts the map back, and the windows show what they did
;   14  45h gives the handle back, and 4Ch then calls it invalid
;
; Check 10 is the second one worth arguing about: a manager that maps
; the window once and then quietly ignores further 44h calls passes
; check 8 and check 9 and fails this one.  Check 12 is there because
; the rule it tests looks pedantic and is not - a program that saved
; a map and never restored it has left state behind, and letting the
; handle go would strand it.
;
; On a machine without EMS= this prints "no expanded memory" and
; stops, which is not a failure - it is the answer to a different
; question.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_ET     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_ET, DS:_ET, ES:_ET, SS:_ET
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results

;=====================================================================
; 1 - the vector, and the name behind it.
;
; THIS IS HOW A PROGRAM IS SUPPOSED TO FIND EMS and the specification
; says so: read the segment of INT 67h and look for EMMXXXX0 at offset
; 0Ah, which is where a character device driver keeps its name.  If it
; is not there, there is no manager, and calling INT 67h to ask would
; be calling whatever the BIOS left on the vector.
;=====================================================================
        mov     ax,3567h
        int     21h
        mov     ax,es
        mov     [emmseg],ax
        or      ax,ax
        jz      t1_no

        mov     si,OFFSET emmname
        mov     di,0Ah
        mov     cx,8
t1cmp:
        mov     al,[si]
        cmp     al,byte ptr es:[di]
        jne     t1_no
        inc     si
        inc     di
        dec     cx
        jnz     t1cmp
        jmp     short t1_yes

t1_no:                                  ; a trampoline, because no_ems is
        jmp     no_ems                  ; a long way below a short jump
t1_yes:
        mov     dx,OFFSET msg_t1
        call    puts
        call    good

;=====================================================================
; 2 - the version.  AL is BCD, so 3.2 is 32h.
;=====================================================================
        mov     dx,OFFSET msg_t2
        call    puts
        mov     ah,46h
        int     67h
        or      ah,ah
        jnz     t2bad
        mov     [version],al
        or      al,al
        jz      t2bad
        call    good
        jmp     short t3
t2bad:
        call    bad
t3:

;=====================================================================
; 3 - the page frame.  Upper memory, and 16K aligned.
;
; SIXTEEN AND NOT SIXTY-FOUR, which is a deliberate loosening: the
; automatic search picks a 64K-aligned frame because every manager
; ever shipped did and there is software that quietly assumes it, but
; EMS=nnnn in CONFIG.SYS is held only to what the specification asks
; for.  A test that insisted on the convention would fail a machine
; that had been configured exactly as the kernel allows, which is a
; test asserting the default rather than the contract.
;=====================================================================
        mov     dx,OFFSET msg_t3
        call    puts
        mov     ah,41h
        int     67h
        or      ah,ah
        jnz     t3bad
        mov     [frameseg],bx
        cmp     bx,0C000h
        jb      t3bad
        cmp     bx,0E000h               ; and 64K of frame has to fit below
        ja      t3bad                   ; the system BIOS
        test    bx,03FFh                ; 16K, as paragraphs
        jnz     t3bad
        call    good
        jmp     short t4
t3bad:
        call    bad
t4:

;=====================================================================
; 4 - how much is there
;=====================================================================
        mov     dx,OFFSET msg_t4
        call    puts
        mov     ah,42h
        int     67h
        or      ah,ah
        jnz     t4bad
        mov     [pgfree],bx
        mov     [pgtotal],dx
        or      dx,dx
        jz      t4bad                   ; a manager with no memory in it
        cmp     bx,dx
        ja      t4bad                   ; more free than exists
        call    good
        jmp     short t5
t4bad:
        call    bad
t5:

;=====================================================================
; 5 - four pages, please
;=====================================================================
        mov     dx,OFFSET msg_t5
        call    puts
        mov     ah,43h
        mov     bx,4
        int     67h
        or      ah,ah
        jnz     t5bad
        mov     [handle],dx
        or      dx,dx
        jz      t5bad                   ; handle 0 belongs to nobody
        call    good
        jmp     short t6
t5bad:
        call    bad
        jmp     wrapup                  ; nothing below this means anything
                                        ; without a handle
t6:

;=====================================================================
; 6 - and it says it owns four
;=====================================================================
        mov     dx,OFFSET msg_t6
        call    puts
        mov     ah,4Ch
        mov     dx,[handle]
        int     67h
        or      ah,ah
        jnz     t6bad
        cmp     bx,4
        jne     t6bad
        call    good
        jmp     short t7
t6bad:
        call    bad
t7:

;=====================================================================
; 7 - one handle open, which is ours
;=====================================================================
        mov     dx,OFFSET msg_t7
        call    puts
        mov     ah,4Bh
        int     67h
        or      ah,ah
        jnz     t7bad
        cmp     bx,1
        jne     t7bad
        call    good
        jmp     short t8
t7bad:
        call    bad
t8:

;=====================================================================
; 8 - map logical 0 into window 0, and then find out whether anything
;     is actually there.  Both ends of the window, because a decoder
;     that answers for part of a page is not the same as memory.
;=====================================================================
        mov     dx,OFFSET msg_t8
        call    puts
        mov     al,0
        mov     bx,0
        call    emsmap
        jc      t8bad
        mov     bl,0
        mov     bh,0A5h
        call    wrpat
        mov     bl,0
        mov     bh,0A5h
        call    ckpat
        jne     t8bad
        call    good
        jmp     short t9
t8bad:
        call    bad
t9:

;=====================================================================
; 9 - FOUR WINDOWS, FOUR PAGES, FOUR PATTERNS, ALL AT ONCE.
;
; The check a manager that only remembers what it was told cannot
; pass.  Each window gets its own logical page and its own byte; then
; every one of them is read back with the others still mapped.  If any
; two windows share memory - or if only the last mapping took - the
; patterns collide and this fails.
;=====================================================================
        mov     dx,OFFSET msg_t9
        call    puts
        xor     cx,cx
t9map:
        mov     al,cl                   ; window CL <- logical page CL
        mov     bx,cx
        call    emsmap
        jc      t9bad
        mov     bl,cl
        mov     bh,cl
        add     bh,11h                  ; 11h, 12h, 13h, 14h
        call    wrpat
        inc     cx
        cmp     cx,4
        jb      t9map

        xor     cx,cx
t9chk:
        mov     bl,cl
        mov     bh,cl
        add     bh,11h
        call    ckpat
        jne     t9bad
        inc     cx
        cmp     cx,4
        jb      t9chk
        call    good
        jmp     short t10
t9bad:
        call    bad
t10:

;=====================================================================
; 10 - remap window 0 onto logical 1 and it must show logical 1's
;      pattern - the one window 1 wrote.  Then put it back, and
;      logical 0 must still hold its own.
;
; This is the check that catches a manager which maps once and then
; ignores the rest, and it is also the check that catches one whose
; logical pages overlap: if page 0 and page 1 were the same memory,
; the second half of this reads 12h where it wants 11h.
;=====================================================================
        mov     dx,OFFSET msg_t10
        call    puts
        mov     al,0
        mov     bx,1
        call    emsmap
        jc      t10bad
        mov     bl,0
        mov     bh,12h                  ; what window 1 wrote to logical 1
        call    ckpat
        jne     t10bad

        mov     al,0
        mov     bx,0
        call    emsmap
        jc      t10bad
        mov     bl,0
        mov     bh,11h                  ; and logical 0 is untouched
        call    ckpat
        jne     t10bad
        call    good
        jmp     short t11
t10bad:
        call    bad
t11:

;=====================================================================
; 11 - park the page map
;=====================================================================
        mov     dx,OFFSET msg_t11
        call    puts
        mov     ah,47h
        mov     dx,[handle]
        int     67h
        or      ah,ah
        jnz     t11bad
        call    good
        jmp     short t12
t11bad:
        call    bad
t12:

;=====================================================================
; 12 - and now the handle cannot be given back.  86h, and only 86h:
;      any other refusal is the right answer for the wrong reason.
;=====================================================================
        mov     dx,OFFSET msg_t12
        call    puts
        mov     ah,45h
        mov     dx,[handle]
        int     67h
        cmp     ah,86h
        jne     t12bad
        call    good
        jmp     short t13
t12bad:
        call    bad
t13:

;=====================================================================
; 13 - DISTURB THE MAP FIRST, and only then restore it.
;
; Restoring a map that was never changed cannot tell a working 48h
; from one that does nothing at all, and "nothing at all" is exactly
; what an unimplemented restore looks like.  So window 0 is pointed at
; logical 3 and checked to have moved, and the restore has something
; to undo.
;=====================================================================
        mov     dx,OFFSET msg_t13
        call    puts
        mov     al,0
        mov     bx,3
        call    emsmap
        jc      t13bad
        mov     bl,0
        mov     bh,14h                  ; what window 3 wrote to logical 3
        call    ckpat
        jne     t13bad

        mov     ah,48h
        mov     dx,[handle]
        int     67h
        or      ah,ah
        jnz     t13bad
        xor     cx,cx
t13chk:
        mov     bl,cl
        mov     bh,cl
        add     bh,11h
        call    ckpat
        jne     t13bad
        inc     cx
        cmp     cx,4
        jb      t13chk
        call    good
        jmp     short t14
t13bad:
        call    bad
t14:

;=====================================================================
; 14 - and now it goes back, and stops being a handle
;=====================================================================
        mov     dx,OFFSET msg_t14
        call    puts
        mov     ah,45h
        mov     dx,[handle]
        int     67h
        or      ah,ah
        jnz     t14bad
        mov     ah,4Ch
        mov     dx,[handle]
        int     67h
        cmp     ah,83h                  ; invalid handle, which it now is
        jne     t14bad
        call    good
        jmp     short wrapup
t14bad:
        call    bad

wrapup:
        mov     dx,OFFSET msg_frame
        call    puts
        mov     ax,[frameseg]
        call    put4
        mov     dx,OFFSET msg_pages
        call    puts
        mov     ax,[pgtotal]
        call    put4
        mov     dx,OFFSET msg_ver
        call    puts
        mov     al,[version]
        call    put2

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

no_ems:
        mov     dx,OFFSET msg_none
        call    puts
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; emsmap - AL = window, BX = logical page, [handle] = the handle.
;          CF set if the manager refused.
;---------------------------------------------------------------------
emsmap:
        push    dx
        mov     dx,[handle]
        mov     ah,44h
        int     67h
        pop     dx
        or      ah,ah
        jz      emm_ok
        stc
        ret
emm_ok:
        clc
        ret

;---------------------------------------------------------------------
; setwin - AL = window.  ES addresses it.  A window is 4000h bytes,
;          which is 400h paragraphs, which is a shift of ten.
;---------------------------------------------------------------------
setwin:
        push    ax
        push    bx
        movzx   bx,al
        shl     bx,10
        add     bx,[frameseg]
        mov     es,bx
        pop     bx
        pop     ax
        ret

;---------------------------------------------------------------------
; wrpat - BL = window, BH = the byte.  Written at BOTH ENDS of the
;         window, because a mapping that only took for the first page
;         of the four would pass a test that looked at one of them.
;---------------------------------------------------------------------
wrpat:
        push    ax
        mov     al,bl
        call    setwin
        mov     al,bh
        mov     ah,bh
        mov     word ptr es:[0],ax
        mov     word ptr es:[3FFEh],ax
        pop     ax
        ret

;---------------------------------------------------------------------
; ckpat - BL = window, BH = the byte.  ZF set if both ends agree.
;---------------------------------------------------------------------
ckpat:
        push    ax
        mov     al,bl
        call    setwin
        mov     al,bh
        mov     ah,bh
        cmp     word ptr es:[0],ax
        jne     ckp_out
        cmp     word ptr es:[3FFEh],ax
ckp_out:
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
emmseg      dw  0
frameseg    dw  0
handle      dw  0
pgfree      dw  0
pgtotal     dw  0
version     db  0
fails       db  0
resptr      dw  0

emmname     db  'EMMXXXX0'

msg_hi      db  'EMSTEST - expanded memory, from inside a DOS box',13,10,13,10,'$'
msg_none    db  'There is no expanded memory on this machine.',13,10
            db  'Put EMS=AUTO in CONFIG.SYS and boot again - or EMS= and a',13,10
            db  'segment, if this machine needs telling where the frame goes.'
            db  13,10,'$'

msg_t1      db  ' 1 the vector names EMMXXXX0         ','$'
msg_t2      db  ' 2 46h reports a version             ','$'
msg_t3      db  ' 3 41h hands back a page frame       ','$'
msg_t4      db  ' 4 42h counts the pages              ','$'
msg_t5      db  ' 5 43h allocates four of them        ','$'
msg_t6      db  ' 6 4Ch says the handle owns four     ','$'
msg_t7      db  ' 7 4Bh counts one open handle        ','$'
msg_t8      db  ' 8 a mapped window is really there   ','$'
msg_t9      db  ' 9 four windows, four pages, at once ','$'
msg_t10     db  '10 remapping changes what is behind  ','$'
msg_t11     db  '11 47h parks the page map            ','$'
msg_t12     db  '12 45h refuses while it is parked    ','$'
msg_t13     db  '13 48h puts the windows back         ','$'
msg_t14     db  '14 45h releases, and the handle dies ','$'

msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'

msg_frame   db  13,10,'page frame at segment ','$'
msg_pages   db  ', pages in the system ','$'
msg_ver     db  ', version ','$'
msg_res     db  13,10,'result: ','$'
msg_fails   db  13,10,'failures: ','$'
msg_crlf    db  13,10,'$'

results     db  32 dup(0)

_ET     ENDS
        END     start
