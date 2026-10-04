;=====================================================================
; EMS4TEST.ASM - EMS4TEST.COM, the LIM 4.0 half of expanded memory.
;
; EMSTEST covers what 3.2 had.  This covers what 4.0 added, and as
; there, the checks that matter look at MEMORY rather than at return
; codes: a handle that grew must still hold what was written to it, a
; window onto a handle that had to move must follow it, a move must
; put the bytes where the descriptor said, and a routine 56h calls
; must see one map while its caller gets the other one back.
;
; Twenty-two checks:
;
;    1  46h says 4.0
;    2  4Dh lists handle 0, the system's
;    3  5Ah makes a handle with no pages, and 4Bh counts it
;    4  51h grows it to four pages, and they hold what is written
;    5  51h grows it again with a neighbour in the way, and a window
;       onto it is still onto the same page afterwards - where the
;       page is now, not where it was
;    6  51h shrinks it, and the windows onto lost pages come down
;    7  53h names it, and no other handle can take the name
;    8  54h finds it by name and lists it, and refuses an empty name
;    9  52h: volatile, and non-volatile refused
;   10  58h lists the four windows of the frame
;   11  50h maps by number and by segment, and all or nothing
;   12  4Fh puts back the windows it saved and only those
;   13  57h moves across a page line and back
;   14  57h refuses what the specification says it refuses
;   15  57h moves within a handle as memmove does, and says 92h
;   16  57h exchanges, and refuses a window onto the other side
;   17  55h maps and jumps
;   18  56h maps and calls, and the RETF brings the old map back
;   19  59h and 5Bh describe the hardware and keep a context area
;   20  5Dh locks the OS/E functions behind a key
;   21  45h empties handle 0 and leaves it open
;   22  everything given back, and 4Bh is what it was
;
; Check 5 cannot force the move it is there for - where the arena puts
; a block is the arena's business - but a second handle is allocated
; straight after the first to stand in the way, and the check is a
; correct check either way.
;
; On a machine without EMS= this prints "no expanded memory" and
; stops, which is not a failure.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_ET     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_ET, DS:_ET, ES:_ET, SS:_ET
        ORG     0

PMBUF_LEN   EQU     32                  ; room for a 4Fh or 4Eh array

start:
        mov     [mypsp],cs
        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results

        ; Is there a manager at all?  The vector's segment, and the name.
        mov     ax,3567h
        int     21h
        mov     di,0Ah
        mov     si,OFFSET emmname
        mov     cx,8
        cld
        repe    cmpsb
        push    ds
        pop     es
        je      have_ems
        jmp     no_ems
have_ems:
        mov     ah,41h
        int     67h
        mov     [frameseg],bx

;=====================================================================
; 1 - the version.  4.0 in BCD is 40h.
;=====================================================================
        mov     dx,OFFSET msg_t1
        call    puts
        mov     ah,46h
        int     67h
        mov     [version],al
        or      ah,ah
        jnz     t1bad
        cmp     al,40h
        jb      t1bad
        call    good
        jmp     t2
t1bad:
        call    bad
        jmp     wrapup                  ; a 3.2 manager has none of the rest

;=====================================================================
; 2 - handle 0 is open.  4Dh lists every open handle, and the system's
;     is one of them on a 4.0 manager whatever else is running.
;=====================================================================
t2:
        mov     dx,OFFSET msg_t2
        call    puts
        mov     ah,4Dh
        mov     di,OFFSET hbuf
        int     67h
        or      ah,ah
        jnz     t2bad
        mov     cx,bx
        mov     si,OFFSET hbuf
t2look:
        jcxz    t2bad
        cmp     word ptr [si],0
        je      t2good
        add     si,4
        dec     cx
        jmp     t2look
t2good:
        call    good
        jmp     t3
t2bad:
        call    bad

;=====================================================================
; 3 - a handle with no pages, and it is counted
;=====================================================================
t3:
        mov     dx,OFFSET msg_t3
        call    puts
        mov     ah,4Bh
        int     67h
        mov     [nopen],bx
        or      ah,ah
        jnz     t3bad
        mov     ax,5A00h
        xor     bx,bx
        int     67h
        or      ah,ah
        jnz     t3bad
        mov     [hA],dx
        mov     ah,4Ch
        int     67h
        or      ah,ah
        jnz     t3bad
        or      bx,bx
        jnz     t3bad
        mov     ah,4Bh
        int     67h
        or      ah,ah
        jnz     t3bad
        dec     bx
        cmp     bx,[nopen]
        jne     t3bad
        call    good
        jmp     t4
t3bad:
        call    bad
        jmp     wrapup                  ; nothing below means anything
                                        ; without the handle

;=====================================================================
; 4 - grow it to four pages, and stamp each one
;=====================================================================
t4:
        mov     dx,OFFSET msg_t4
        call    puts
        mov     bx,4
        call    realloc
        jc      t4bad
        mov     ah,4Ch
        mov     dx,[hA]
        int     67h
        or      ah,ah
        jnz     t4bad
        cmp     bx,4
        jne     t4bad
        xor     bx,bx
t4put:
        call    stamppg                 ; page BX gets 'A' + BX
        jc      t4bad
        inc     bx
        cmp     bx,4
        jb      t4put
        xor     bx,bx
t4chk:
        call    checkpg
        jne     t4bad
        inc     bx
        cmp     bx,4
        jb      t4chk
        call    good
        jmp     t5
t4bad:
        call    bad

;=====================================================================
; 5 - something in the way, then grow again.  Window 1 is looking at
;     page 2 the whole time, and must still be looking at page 2.
;
; READING 'C' THROUGH IT PROVES NOTHING ON ITS OWN: if the handle moved
; and the window was left behind, it is looking at the freed block,
; which still holds every byte it held.  So a byte is WRITTEN through
; window 1 and read through window 2, mapped afresh after the grow -
; the two agree only if both are looking at where page 2 is now.
;=====================================================================
t5:
        mov     dx,OFFSET msg_t5
        call    puts
        mov     ah,43h
        mov     bx,1
        int     67h
        or      ah,ah
        jnz     t5bad
        mov     [hB],dx

        mov     al,1
        mov     bx,2
        mov     dx,[hA]
        call    emsmap
        jc      t5bad
        mov     bx,8
        call    realloc
        jc      t5bad
        mov     bl,1
        mov     bh,'C'
        call    ckpat
        jne     t5bad
        mov     bl,1
        mov     bh,'c'
        call    wrpat
        mov     al,2
        mov     bx,2
        mov     dx,[hA]
        call    emsmap
        jc      t5bad
        mov     bl,2
        mov     bh,'c'
        call    ckpat
        jne     t5bad
        mov     bl,2
        mov     bh,'C'                  ; and page 2's own pattern back
        call    wrpat
        xor     bx,bx
t5chk:
        call    checkpg                 ; and every page kept its own
        jne     t5bad
        inc     bx
        cmp     bx,4
        jb      t5chk
        call    good
        jmp     t6
t5bad:
        call    bad

;=====================================================================
; 6 - shrink to two.  Windows 2 and 3 were showing pages 2 and 3,
;     which have gone: the windows read FFh, and 44h refuses page 2.
;=====================================================================
t6:
        mov     dx,OFFSET msg_t6
        call    puts
        mov     cx,4
        xor     bx,bx
t6map:
        mov     al,bl
        mov     dx,[hA]
        call    emsmap
        jc      t6bad
        inc     bx
        loop    t6map
        mov     bx,2
        call    realloc
        jc      t6bad
        mov     bl,2
        mov     bh,0FFh
        call    ckpat
        jne     t6bad
        mov     bl,3
        mov     bh,0FFh
        call    ckpat
        jne     t6bad
        mov     bl,0
        mov     bh,'A'
        call    ckpat
        jne     t6bad
        mov     bl,1
        mov     bh,'B'
        call    ckpat
        jne     t6bad
        mov     ah,44h
        mov     al,2
        mov     bx,2
        mov     dx,[hA]
        int     67h
        cmp     ah,8Ah
        jne     t6bad
        call    good
        jmp     t7
t6bad:
        call    bad

;=====================================================================
; 7 - a name, read back; the same name on another handle is A1h, and
;     on the same handle again is no change at all
;=====================================================================
t7:
        mov     dx,OFFSET msg_t7
        call    puts
        mov     ax,5301h
        mov     dx,[hA]
        mov     si,OFFSET hname
        int     67h
        or      ah,ah
        jnz     t7bad
        mov     ax,5300h
        mov     dx,[hA]
        mov     di,OFFSET nbuf
        int     67h
        or      ah,ah
        jnz     t7bad
        mov     si,OFFSET hname
        mov     di,OFFSET nbuf
        mov     cx,8
        repe    cmpsb
        jne     t7bad
        mov     ax,5301h
        mov     dx,[hB]
        mov     si,OFFSET hname
        int     67h
        cmp     ah,0A1h
        jne     t7bad
        mov     ax,5301h
        mov     dx,[hA]
        mov     si,OFFSET hname
        int     67h
        or      ah,ah
        jnz     t7bad
        call    good
        jmp     t8
t7bad:
        call    bad

;=====================================================================
; 8 - the directory.  By name; by no name, which is A1h; by a name
;     nobody has, which is A0h; and the whole list, with ours in it.
;=====================================================================
t8:
        mov     dx,OFFSET msg_t8
        call    puts
        mov     ax,5401h
        mov     si,OFFSET hname
        int     67h
        or      ah,ah
        jnz     t8bad
        cmp     dx,[hA]
        jne     t8bad
        mov     ax,5401h
        mov     si,OFFSET noname
        int     67h
        cmp     ah,0A1h
        jne     t8bad
        mov     ax,5401h
        mov     si,OFFSET nosuch
        int     67h
        cmp     ah,0A0h
        jne     t8bad

        mov     ax,5400h
        mov     di,OFFSET dbuf
        int     67h
        or      ah,ah
        jnz     t8bad
        xor     ah,ah
        mov     cx,ax                   ; how many
        mov     [ndir],ax
        mov     bx,OFFSET dbuf
t8look:
        jcxz    t8bad
        mov     ax,[bx]
        cmp     ax,[hA]
        je      t8name
        add     bx,10
        dec     cx
        jmp     t8look
t8name:
        lea     si,[bx+2]
        mov     di,OFFSET hname
        mov     cx,8
        repe    cmpsb
        jne     t8bad
        mov     ax,5402h
        int     67h
        or      ah,ah
        jnz     t8bad
        cmp     bx,[ndir]
        jb      t8bad
        call    good
        jmp     t9
t8bad:
        call    bad

;=====================================================================
; 9 - attributes.  Volatile; asking for non-volatile is 91h, for
;     something undefined 90h; and the capability is volatile only.
;=====================================================================
t9:
        mov     dx,OFFSET msg_t9
        call    puts
        mov     ax,5200h
        mov     dx,[hA]
        int     67h
        or      ah,ah
        jnz     t9bad
        or      al,al
        jnz     t9bad
        mov     ax,5201h
        mov     dx,[hA]
        mov     bl,1
        int     67h
        cmp     ah,91h
        jne     t9bad
        mov     ax,5201h
        mov     dx,[hA]
        mov     bl,2
        int     67h
        cmp     ah,90h
        jne     t9bad
        mov     ax,5201h
        mov     dx,[hA]
        mov     bl,0
        int     67h
        or      ah,ah
        jnz     t9bad
        mov     ax,5202h
        int     67h
        or      ah,ah
        jnz     t9bad
        or      al,al
        jnz     t9bad
        call    good
        jmp     t10
t9bad:
        call    bad

;=====================================================================
; 10 - the mappable pages: four, the frame's, in order
;=====================================================================
t10:
        mov     dx,OFFSET msg_t10
        call    puts
        mov     ax,5801h
        int     67h
        or      ah,ah
        jnz     t10bad
        cmp     cx,4
        jne     t10bad
        mov     ax,5800h
        mov     di,OFFSET hbuf
        int     67h
        or      ah,ah
        jnz     t10bad
        cmp     cx,4
        jne     t10bad
        mov     si,OFFSET hbuf
        mov     ax,[frameseg]
        xor     bx,bx
t10one:
        cmp     [si],ax
        jne     t10bad
        cmp     [si+2],bx
        jne     t10bad
        add     ax,400h
        add     si,4
        inc     bx
        cmp     bx,4
        jb      t10one
        call    good
        jmp     t11
t10bad:
        call    bad

;=====================================================================
; 11 - several at once.  By number: page 1 into window 3 and page 0
;      into window 2.  By segment: pages 0 and 1 into windows 0 and 1.
;      Then a list with one bad window in it: 8Bh, and window 0, which
;      its first pair names, unchanged.  Last, FFFFh unmaps window 3.
;=====================================================================
t11:
        mov     dx,OFFSET msg_t11
        call    puts
        mov     word ptr [lst],1
        mov     word ptr [lst+2],3
        mov     word ptr [lst+4],0
        mov     word ptr [lst+6],2
        mov     ax,5000h
        mov     cx,2
        call    maplist
        jnz     t11bad
        mov     bl,3
        mov     bh,'B'
        call    ckpat
        jne     t11bad
        mov     bl,2
        mov     bh,'A'
        call    ckpat
        jne     t11bad

        mov     ax,[frameseg]
        mov     word ptr [lst],0
        mov     [lst+2],ax
        add     ax,400h
        mov     word ptr [lst+4],1
        mov     [lst+6],ax
        mov     ax,5001h
        mov     cx,2
        call    maplist
        jnz     t11bad
        mov     bl,0
        mov     bh,'A'
        call    ckpat
        jne     t11bad
        mov     bl,1
        mov     bh,'B'
        call    ckpat
        jne     t11bad

        mov     word ptr [lst],1
        mov     word ptr [lst+2],0
        mov     word ptr [lst+4],0
        mov     word ptr [lst+6],9
        mov     ax,5000h
        mov     cx,2
        call    maplist
        cmp     ah,8Bh
        jne     t11bad
        mov     bl,0
        mov     bh,'A'                  ; still page 0, not page 1
        call    ckpat
        jne     t11bad

        mov     word ptr [lst],0FFFFh
        mov     word ptr [lst+2],3
        mov     ax,5000h
        mov     cx,1
        call    maplist
        jnz     t11bad
        mov     bl,3
        mov     bh,0FFh
        call    ckpat
        jne     t11bad
        call    good
        jmp     t12
t11bad:
        call    bad

;=====================================================================
; 12 - a partial map.  Windows 0 and 3 are saved (pages 0 and 1); 0, 1
;      and 3 are all moved; the restore must put back 0 and 3 and leave
;      window 1 where it was moved to.
;=====================================================================
t12:
        mov     dx,OFFSET msg_t12
        call    puts
        mov     ax,4F02h
        mov     bx,2
        int     67h
        or      ah,ah
        jnz     t12bad
        cmp     al,PMBUF_LEN
        ja      t12bad                  ; more than this program kept room for
        mov     al,3
        mov     bx,1
        mov     dx,[hA]
        call    emsmap
        jc      t12bad
        mov     ax,[frameseg]
        mov     [pmlist+2],ax
        add     ax,0C00h
        mov     [pmlist+4],ax
        mov     ax,4F00h
        mov     si,OFFSET pmlist
        mov     di,OFFSET pmbuf
        int     67h
        or      ah,ah
        jnz     t12bad

        mov     al,0
        mov     bx,1
        mov     dx,[hA]
        call    emsmap
        jc      t12bad
        mov     al,1
        mov     bx,0
        call    emsmap
        jc      t12bad
        mov     al,3
        mov     bx,0
        call    emsmap
        jc      t12bad

        mov     ax,4F01h
        mov     si,OFFSET pmbuf
        int     67h
        or      ah,ah
        jnz     t12bad
        mov     bl,0
        mov     bh,'A'
        call    ckpat
        jne     t12bad
        mov     bl,3
        mov     bh,'B'
        call    ckpat
        jne     t12bad
        mov     bl,1
        mov     bh,'A'                  ; not saved, so not put back
        call    ckpat
        jne     t12bad
        call    good
        jmp     t13
t12bad:
        call    bad

;=====================================================================
; 13 - 32 bytes from conventional memory to page 0 at 3FF0h, which is
;      sixteen bytes in each of pages 0 and 1; then back into a second
;      buffer.  Then the windows, to see that the halves really are in
;      two pages.
;=====================================================================
t13:
        mov     dx,OFFSET msg_t13
        call    puts
        mov     si,OFFSET line32
        call    srcconv
        mov     bx,0
        mov     di,3FF0h
        call    dstexp
        mov     al,0
        mov     cx,32
        call    move57
        or      ah,ah
        jnz     t13bad
        mov     bx,0
        mov     si,3FF0h
        call    srcexp
        mov     di,OFFSET buf32
        call    dstconv
        mov     al,0
        mov     cx,32
        call    move57
        or      ah,ah
        jnz     t13bad
        mov     si,OFFSET line32
        mov     di,OFFSET buf32
        mov     cx,32
        call    same
        jne     t13bad

        mov     al,0
        mov     bx,0
        mov     dx,[hA]
        call    emsmap
        jc      t13bad
        mov     al,1
        mov     bx,1
        call    emsmap
        jc      t13bad
        mov     ax,[frameseg]
        mov     es,ax
        mov     si,OFFSET line32
        mov     di,3FF0h
        mov     cx,16
        repe    cmpsb
        jne     t13es
        add     ax,400h
        mov     es,ax
        xor     di,di
        mov     cx,16
        repe    cmpsb
        jne     t13es
        push    ds
        pop     es
        call    good
        jmp     t14
t13es:
        push    ds
        pop     es
t13bad:
        call    bad

;=====================================================================
; 14 - the refusals, one call each: an offset past the end of a page
;      (95h), a region past the end of the handle (93h), more than a
;      megabyte (96h), a type that is neither (98h), conventional
;      memory past the megabyte (A2h), a handle nobody has (83h), and
;      a page the handle has not got (8Ah).
;=====================================================================
t14:
        mov     dx,OFFSET msg_t14
        call    puts
        mov     si,OFFSET line32
        call    srcconv
        mov     bx,0
        mov     di,4000h
        call    dstexp
        mov     cx,16
        mov     ah,95h
        call    refuse
        jne     t14bad

        mov     bx,1
        mov     di,3FF0h
        call    dstexp
        mov     cx,32
        mov     ah,93h
        call    refuse
        jne     t14bad

        mov     bx,0
        mov     di,0
        call    dstexp
        mov     word ptr [mx_len],1
        mov     word ptr [mx_len+2],10h
        mov     ax,5700h
        mov     si,OFFSET mx_len
        int     67h
        cmp     ah,96h
        jne     t14bad

        mov     byte ptr [mx_stype],2
        mov     cx,16
        mov     ah,98h
        call    refuse
        jne     t14bad

        mov     si,0010h
        call    srcconv
        mov     word ptr [mx_sseg],0FFFFh
        mov     cx,100h
        mov     ah,0A2h
        call    refuse
        jne     t14bad

        mov     si,OFFSET line32
        call    srcconv
        mov     word ptr [mx_dhand],0FFFFh
        mov     cx,16
        mov     ah,83h
        call    refuse
        jne     t14bad

        mov     bx,5
        mov     di,0
        call    dstexp
        mov     cx,16
        mov     ah,8Ah
        call    refuse
        jne     t14bad
        call    good
        jmp     t15
t14bad:
        call    bad

;=====================================================================
; 15 - within one handle, overlapping: sixteen bytes at page 0 offset
;      0 moved four bytes up.  92h, and the result is what memmove
;      gives - "0123" and then all sixteen.  The same as an exchange
;      is 97h.
;=====================================================================
t15:
        mov     dx,OFFSET msg_t15
        call    puts
        mov     si,OFFSET line16
        call    srcconv
        mov     bx,0
        mov     di,0
        call    dstexp
        mov     al,0
        mov     cx,16
        call    move57
        or      ah,ah
        jnz     t15bad

        mov     bx,0
        mov     si,0
        call    srcexp
        mov     bx,0
        mov     di,4
        call    dstexp
        mov     al,0
        mov     cx,16
        call    move57
        cmp     ah,92h
        jne     t15bad

        mov     bx,0
        mov     si,0
        call    srcexp
        mov     di,OFFSET buf32
        call    dstconv
        mov     al,0
        mov     cx,20
        call    move57
        or      ah,ah
        jnz     t15bad
        mov     si,OFFSET line16
        mov     di,OFFSET buf32
        mov     cx,4
        call    same
        jne     t15bad
        mov     si,OFFSET line16
        mov     di,OFFSET buf32+4
        mov     cx,16
        call    same
        jne     t15bad

        mov     bx,0
        mov     si,0
        call    srcexp
        mov     bx,0
        mov     di,4
        call    dstexp
        mov     al,1
        mov     cx,16
        call    move57
        cmp     ah,97h
        jne     t15bad
        call    good
        jmp     t16
t15bad:
        call    bad

;=====================================================================
; 16 - an exchange between conventional memory and page 1 at 100h,
;      checked from both sides.  Then a move whose conventional side is
;      window 0 while window 0 shows the very page the expanded side
;      names: 94h.  The same move into page 1, which window 0 is not
;      showing, is allowed.
;=====================================================================
t16:
        mov     dx,OFFSET msg_t16
        call    puts
        mov     si,OFFSET line16
        call    srcconv
        mov     bx,1
        mov     di,100h
        call    dstexp
        mov     al,0
        mov     cx,16
        call    move57
        or      ah,ah
        jnz     t16bad

        mov     si,OFFSET xline
        mov     di,OFFSET buf32
        mov     cx,16
        rep     movsb                   ; a copy, since it is about to change
        mov     si,OFFSET buf32
        call    srcconv
        mov     bx,1
        mov     di,100h
        call    dstexp
        mov     al,1
        mov     cx,16
        call    move57
        or      ah,ah
        jnz     t16bad
        mov     si,OFFSET line16
        mov     di,OFFSET buf32
        mov     cx,16
        call    same
        jne     t16bad
        mov     bx,1
        mov     si,100h
        call    srcexp
        mov     di,OFFSET buf32
        call    dstconv
        mov     al,0
        mov     cx,16
        call    move57
        or      ah,ah
        jnz     t16bad
        mov     si,OFFSET xline
        mov     di,OFFSET buf32
        mov     cx,16
        call    same
        jne     t16bad

        mov     al,0
        mov     bx,0
        mov     dx,[hA]
        call    emsmap
        jc      t16bad
        mov     byte ptr [mx_stype],0
        mov     word ptr [mx_soff],100h
        mov     ax,[frameseg]
        mov     [mx_sseg],ax
        mov     bx,0
        mov     di,0
        call    dstexp
        mov     cx,16
        mov     ah,94h
        call    refuse
        jne     t16bad
        mov     bx,1
        mov     di,200h
        call    dstexp
        mov     al,0
        mov     cx,16
        call    move57
        or      ah,ah
        jnz     t16bad
        call    good
        jmp     t17
t16bad:
        call    bad

;=====================================================================
; 17 - map and jump.  Window 0 shows page 0 going in; the jump puts
;      page 1 there, by number, and the code after the INT is reached
;      only if the manager refused.
;=====================================================================
t17:
        mov     dx,OFFSET msg_t17
        call    puts
        call    restamp
        jc      t17bad
        mov     al,0
        mov     bx,0
        mov     dx,[hA]
        call    emsmap
        jc      t17bad
        mov     word ptr [mj_tgt],OFFSET t17land
        mov     [mj_tgt+2],cs
        mov     word ptr [mj_map],OFFSET lst
        mov     [mj_map+2],ds
        mov     byte ptr [mj_len],1
        mov     word ptr [lst],1
        mov     word ptr [lst+2],0
        mov     ax,5500h
        mov     dx,[hA]
        mov     si,OFFSET mj_tgt
        int     67h
        jmp     t17bad                  ; refused, or ignored
t17land:
        or      ah,ah
        jnz     t17bad
        mov     bl,0
        mov     bh,'B'
        call    ckpat
        jne     t17bad
        call    good
        jmp     t18
t17bad:
        call    bad

;=====================================================================
; 18 - map and call.  Window 1 shows page 1 going in; the routine is
;      called with page 0 there and must see 'A'; when it returns the
;      old map says page 1 again, so the caller sees 'B'.  The routine
;      leaves CX = 4EADh, which must reach the caller, and the stack
;      must be where it was.
;=====================================================================
t18:
        mov     dx,OFFSET msg_t18
        call    puts
        mov     ax,5602h
        int     67h
        or      ah,ah
        jnz     t18bad
        or      bx,bx
        jz      t18bad

        mov     al,1
        mov     bx,1
        mov     dx,[hA]
        call    emsmap
        jc      t18bad
        mov     word ptr [mc_tgt],OFFSET t18sub
        mov     [mc_tgt+2],cs
        mov     byte ptr [mc_newlen],1
        mov     word ptr [mc_newmap],OFFSET lst
        mov     [mc_newmap+2],ds
        mov     byte ptr [mc_oldlen],1
        mov     word ptr [mc_oldmap],OFFSET lst2
        mov     [mc_oldmap+2],ds
        mov     word ptr [lst],0        ; page 0 -> window 1 for the call
        mov     word ptr [lst+2],1
        mov     word ptr [lst2],1       ; page 1 -> window 1 after it
        mov     word ptr [lst2+2],1
        mov     byte ptr [saw],0
        mov     [spwas],sp
        xor     cx,cx
        mov     ax,5600h
        mov     dx,[hA]
        mov     si,OFFSET mc_tgt
        int     67h
        or      ah,ah
        jnz     t18bad
        cmp     sp,[spwas]
        jne     t18bad
        cmp     cx,4EADh
        jne     t18bad
        cmp     byte ptr [saw],'A'
        jne     t18bad
        mov     bl,1
        mov     bh,'B'
        call    ckpat
        jne     t18bad
        call    good
        jmp     t19
t18bad:
        call    bad
        jmp     t19

; what 56h calls: it records what window 1 shows and goes back
t18sub:
        push    es
        mov     ax,[frameseg]
        add     ax,400h
        mov     es,ax
        mov     al,es:[0]
        mov     [saw],al
        pop     es
        mov     cx,4EADh
        retf

;=====================================================================
; 19 - the hardware, and set 0.  59h/00: raw pages of 400h paragraphs,
;      no alternate or DMA register sets, a context area the size 4Eh
;      says.  59h/01 agrees with 42h.  5Bh: set 0 with no area, an
;      area's size, no set to allocate, set 1 refused (9Ch); then an
;      area taken with 4Eh is handed to 5Bh/01, which must put that
;      map back, and 5Bh/00 must give the same pointer back; and 0:0
;      forgets it.
;=====================================================================
t19:
        mov     dx,OFFSET msg_t19
        call    puts
        mov     ax,4E03h
        int     67h
        or      ah,ah
        jnz     t19bad
        mov     [ctxlen],al
        mov     ax,5900h
        mov     di,OFFSET hbuf
        int     67h
        or      ah,ah
        jnz     t19bad
        cmp     word ptr [hbuf],400h
        jne     t19bad
        cmp     word ptr [hbuf+2],0
        jne     t19bad
        mov     al,[ctxlen]
        xor     ah,ah
        cmp     [hbuf+4],ax
        jne     t19bad
        cmp     word ptr [hbuf+6],0
        jne     t19bad
        mov     ah,42h
        int     67h
        mov     [pgfree],bx
        mov     [pgtotal],dx
        mov     ax,5901h
        int     67h
        or      ah,ah
        jnz     t19bad
        cmp     bx,[pgfree]
        jne     t19bad
        cmp     dx,[pgtotal]
        jne     t19bad

        mov     ax,5B00h
        int     67h
        or      ah,ah
        jnz     t19bad
        or      bl,bl
        jnz     t19bad
        mov     ax,es
        or      ax,di
        push    ds
        pop     es
        jnz     t19bad                  ; nobody has named an area yet
        mov     ax,5B02h
        int     67h
        or      ah,ah
        jnz     t19bad
        mov     al,[ctxlen]
        xor     ah,ah
        cmp     dx,ax
        jne     t19bad
        mov     ax,5B03h
        mov     bl,0FFh
        int     67h
        or      ah,ah
        jnz     t19bad
        or      bl,bl
        jnz     t19bad
        mov     ax,5B01h
        mov     bl,1
        int     67h
        cmp     ah,9Ch
        jne     t19bad

        call    t19ctx
        jnc     t19ok
t19bad:
        push    ds
        pop     es
        call    bad
        jmp     t20
t19ok:
        call    good
        jmp     t20

; the save area half of 19, as a subroutine so that its several ways
; to fail can all just return with CF
t19ctx:
        mov     al,0
        mov     bx,0
        mov     dx,[hA]
        call    emsmap                  ; window 0 shows page 0 ...
        jc      t19cno
        mov     ax,4E00h
        mov     di,OFFSET ctxbuf
        int     67h                     ; ... in the area ...
        or      ah,ah
        jnz     t19cno
        mov     al,0
        mov     bx,1
        call    emsmap                  ; ... and then it does not
        jc      t19cno
        mov     ax,5B01h
        mov     bl,0
        mov     di,OFFSET ctxbuf
        int     67h
        or      ah,ah
        jnz     t19cno
        mov     bl,0
        mov     bh,'A'
        call    ckpat
        jne     t19cno
        mov     ax,5B00h
        int     67h
        or      ah,ah
        jnz     t19cno
        mov     ax,es
        push    ds
        pop     es
        cmp     ax,[mypsp]
        jne     t19cno
        cmp     di,OFFSET ctxbuf
        jne     t19cno
        mov     ax,5B01h
        mov     bl,0
        xor     di,di
        push    di
        pop     es
        int     67h
        push    ds
        pop     es
        or      ah,ah
        jnz     t19cno
        mov     ax,5B00h
        int     67h
        mov     ax,es
        or      ax,di
        push    ds
        pop     es
        jnz     t19cno
        clc
        ret
t19cno:
        push    ds
        pop     es
        stc
        ret

;=====================================================================
; 20 - the key.  The first switch hands one out; with the functions
;      off, 59h/00 and 5Bh are A4h; the wrong key is A4h; the right
;      one switches them back on; 5Dh/02 gives it back.
;=====================================================================
t20:
        mov     dx,OFFSET msg_t20
        call    puts
        mov     ax,5D01h
        int     67h
        or      ah,ah
        jnz     t20bad
        mov     [keyhi],bx
        mov     [keylo],cx
        mov     ax,5900h
        mov     di,OFFSET hbuf
        int     67h
        cmp     ah,0A4h
        jne     t20bad
        mov     ax,5B02h
        int     67h
        cmp     ah,0A4h
        jne     t20bad
        mov     ax,5D00h
        mov     bx,[keyhi]
        mov     cx,[keylo]
        inc     cx
        int     67h
        cmp     ah,0A4h
        jne     t20bad
        mov     ax,5D00h
        mov     bx,[keyhi]
        mov     cx,[keylo]
        int     67h
        or      ah,ah
        jnz     t20bad
        mov     ax,5900h
        mov     di,OFFSET hbuf
        int     67h
        or      ah,ah
        jnz     t20bad
        mov     ax,5D02h
        mov     bx,[keyhi]
        mov     cx,[keylo]
        int     67h
        or      ah,ah
        jnz     t20bad
        call    good
        jmp     t21
t20bad:
        mov     ax,5D02h                ; whatever happened, give it back
        mov     bx,[keyhi]
        mov     cx,[keylo]
        int     67h
        call    bad

;=====================================================================
; 21 - handle 0 given back: its pages go, it does not
;=====================================================================
t21:
        mov     dx,OFFSET msg_t21
        call    puts
        mov     ah,45h
        xor     dx,dx
        int     67h
        or      ah,ah
        jnz     t21bad
        mov     ah,4Ch
        xor     dx,dx
        int     67h
        or      ah,ah
        jnz     t21bad
        or      bx,bx
        jnz     t21bad
        call    good
        jmp     t22
t21bad:
        call    bad

;=====================================================================
; 22 - give everything back.  A raw page on the way past, which must
;      also come and go; 43h still refuses zero pages (89h); 5Ch
;      answers; and 4Bh ends where it started.
;=====================================================================
t22:
        mov     dx,OFFSET msg_t22
        call    puts
        mov     ah,45h
        mov     dx,[hA]
        int     67h
        or      ah,ah
        jnz     t22bad
        mov     ah,45h
        mov     dx,[hB]
        int     67h
        or      ah,ah
        jnz     t22bad
        mov     ax,5A01h
        mov     bx,1
        int     67h
        or      ah,ah
        jnz     t22bad
        mov     ah,45h
        int     67h
        or      ah,ah
        jnz     t22bad
        mov     ah,43h
        xor     bx,bx
        int     67h
        cmp     ah,89h
        jne     t22bad
        mov     ah,5Ch
        int     67h
        or      ah,ah
        jnz     t22bad
        mov     ah,4Bh
        int     67h
        or      ah,ah
        jnz     t22bad
        cmp     bx,[nopen]
        jne     t22bad
        call    good
        jmp     wrapup
t22bad:
        call    bad

wrapup:
        mov     dx,OFFSET msg_frame
        call    puts
        mov     ax,[frameseg]
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

;=====================================================================
; Helpers
;=====================================================================

;--- emsmap - AL = window, BX = logical page, DX = the handle.  CF set
;--- if the manager refused.
emsmap:
        mov     ah,44h
        int     67h
        or      ah,ah
        jz      emm_ok
        stc
        ret
emm_ok:
        clc
        ret

;--- realloc - BX = the pages [hA] should have.  CF if refused.
realloc:
        mov     ah,51h
        mov     dx,[hA]
        int     67h
        or      ah,ah
        jz      emm_ok
        stc
        ret

;--- maplist - AX = 5000h or 5001h, CX = pairs at lst.  AH = status,
;--- ZF set if it is 0.
maplist:
        mov     dx,[hA]
        mov     si,OFFSET lst
        int     67h
        or      ah,ah
        ret

;--- stamppg - BX = a page of [hA]: 'A' + BX at both ends of it, through
;--- window 0.  checkpg - ZF if it is still there.  CF if refused.
stamppg:
        push    bx
        mov     al,0
        mov     dx,[hA]
        call    emsmap
        pop     bx
        jc      spg_out
        push    bx
        mov     bh,bl
        add     bh,'A'
        mov     bl,0
        call    wrpat
        pop     bx
        clc
spg_out:
        ret

checkpg:
        push    bx
        mov     al,0
        mov     dx,[hA]
        call    emsmap
        pop     bx
        jc      cpg_bad
        push    bx
        mov     bh,bl
        add     bh,'A'
        mov     bl,0
        call    ckpat
        pop     bx
        ret
cpg_bad:
        or      sp,sp                   ; NZ: not there
        ret

;--- restamp - pages 0 and 1 get 'A' and 'B' again, which the moves
;--- between have written over.  CF if refused.
restamp:
        mov     bx,0
        call    stamppg
        jc      rst_out
        mov     bx,1
        call    stamppg
rst_out:
        ret

;--- setwin - AL = window.  ES addresses it.
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

;--- wrpat - BL = window, BH = the byte, at both ends of the window.
;--- ckpat - ZF set if both ends hold it.  ES comes back as DS.
wrpat:
        push    ax
        mov     al,bl
        call    setwin
        mov     al,bh
        mov     ah,bh
        mov     word ptr es:[0],ax
        mov     word ptr es:[3FFEh],ax
        push    ds
        pop     es
        pop     ax
        ret

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
        push    ds
        pop     es
        pop     ax
        ret

;--- the two sides of 57h's descriptor.  Conventional: DS:SI or DS:DI.
;--- Expanded: [hA], page BX, offset SI or DI.
srcconv:
        mov     byte ptr [mx_stype],0
        mov     [mx_soff],si
        mov     [mx_sseg],ds
        ret
dstconv:
        mov     byte ptr [mx_dtype],0
        mov     [mx_doff],di
        mov     [mx_dseg],ds
        ret
srcexp:
        mov     byte ptr [mx_stype],1
        push    ax
        mov     ax,[hA]
        mov     [mx_shand],ax
        pop     ax
        mov     [mx_soff],si
        mov     [mx_sseg],bx
        ret
dstexp:
        mov     byte ptr [mx_dtype],1
        push    ax
        mov     ax,[hA]
        mov     [mx_dhand],ax
        pop     ax
        mov     [mx_doff],di
        mov     [mx_dseg],bx
        ret

;--- move57 - AL = 0 move, 1 exchange; CX bytes.  AH = the status.
move57:
        mov     [mx_len],cx
        mov     word ptr [mx_len+2],0
        mov     ah,57h
        mov     si,OFFSET mx_len
        int     67h
        ret

;--- refuse - a move of CX bytes that should be refused with AH.  ZF
;--- if it was.
refuse:
        mov     [want],ah
        mov     al,0
        call    move57
        cmp     ah,[want]
        ret

;--- same - ZF if CX bytes at SI and DI agree.
same:
        push    ds
        pop     es
        repe    cmpsb
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
frameseg    dw  0
hA          dw  0
hB          dw  0
nopen       dw  0
ndir        dw  0
pgfree      dw  0
pgtotal     dw  0
spwas       dw  0
keyhi       dw  0
keylo       dw  0
mypsp       dw  0
version     db  0
fails       db  0
want        db  0
saw         db  0
ctxlen      db  0
resptr      dw  0

emmname     db  'EMMXXXX0'
hname       db  'EMS4TEST'
noname      db  8 dup (0)
nosuch      db  'NOSUCH!!'
nbuf        db  8 dup (0)

line32      db  'MOVE-ACROSS-A-PAGE-LINE-0123456!'
line16      db  '0123456789ABCDEF'
xline       db  'XCHG-LEFT-SIDE!!'
buf32       db  32 dup (0)

; 57h's descriptor
mx_len      dw  0, 0
mx_stype    db  0
mx_shand    dw  0
mx_soff     dw  0
mx_sseg     dw  0
mx_dtype    db  0
mx_dhand    dw  0
mx_doff     dw  0
mx_dseg     dw  0

; 55h's
mj_tgt      dw  0, 0
mj_len      db  0
mj_map      dw  0, 0

; 56h's
mc_tgt      dw  0, 0
mc_newlen   db  0
mc_newmap   dw  0, 0
mc_oldlen   db  0
mc_oldmap   dw  0, 0
mc_rsvd     dw  4 dup (0)

lst         dw  8 dup (0)               ; map lists: logical, physical
lst2        dw  8 dup (0)
pmlist      dw  2, 0, 0                 ; 4Fh: two windows, by segment
pmbuf       db  PMBUF_LEN dup (0)
ctxbuf      db  PMBUF_LEN dup (0)
hbuf        dw  64 * 2 dup (0)          ; 4Dh, 58h, 59h
dbuf        db  64 * 10 dup (0)         ; 54h

msg_hi      db  'EMS4TEST - LIM 4.0 expanded memory, from inside a DOS box',13,10,13,10,'$'
msg_none    db  'There is no expanded memory on this machine.',13,10
            db  'Put EMS=AUTO in CONFIG.SYS and boot again.',13,10,'$'

msg_t1      db  ' 1 46h says 4.0                       ','$'
msg_t2      db  ' 2 4Dh lists the system handle 0      ','$'
msg_t3      db  ' 3 5Ah makes an empty handle          ','$'
msg_t4      db  ' 4 51h grows it to four pages         ','$'
msg_t5      db  ' 5 51h grows it past a neighbour      ','$'
msg_t6      db  ' 6 51h shrinks it, windows come down  ','$'
msg_t7      db  ' 7 53h names it, once                 ','$'
msg_t8      db  ' 8 54h finds and lists it             ','$'
msg_t9      db  ' 9 52h says volatile                  ','$'
msg_t10     db  '10 58h lists the four windows         ','$'
msg_t11     db  '11 50h maps several, all or nothing   ','$'
msg_t12     db  '12 4Fh restores just what it saved    ','$'
msg_t13     db  '13 57h moves across a page line       ','$'
msg_t14     db  '14 57h refuses what it should         ','$'
msg_t15     db  '15 57h overlap: memmove and 92h       ','$'
msg_t16     db  '16 57h exchanges, and 94h             ','$'
msg_t17     db  '17 55h maps and jumps                 ','$'
msg_t18     db  '18 56h maps, calls, and maps back     ','$'
msg_t19     db  '19 59h and 5Bh                        ','$'
msg_t20     db  '20 5Dh locks the OS/E functions       ','$'
msg_t21     db  '21 45h empties handle 0               ','$'
msg_t22     db  '22 all given back                     ','$'

msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'

msg_frame   db  13,10,'page frame at segment ','$'
msg_ver     db  ', version ','$'
msg_res     db  13,10,'result: ','$'
msg_fails   db  13,10,'failures: ','$'
msg_crlf    db  13,10,'$'

results     db  32 dup(0)

_ET     ENDS
        END     start
