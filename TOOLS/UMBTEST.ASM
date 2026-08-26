;=====================================================================
; UMBTEST.ASM - UMBTEST.COM, upper memory measured from a DOS box.
;
; Upper memory has three ways of being asked about and they are all
; the same memory: 21h/58h and 48h with the strategy bits, XMS
; functions 10h-12h, and the MCB chain itself, which a program is
; entitled to walk and which MEM does walk.  Nothing here trusts a
; report - every check that says a block was allocated goes on to
; WRITE TO IT AND READ IT BACK, because a UMB is the one allocation
; in this kernel whose address is not backed by the memory that lives
; at that address on a real machine, and "the page tables were never
; touched" looks exactly like success right up to the first store.
;
; Eleven checks:
;
;    1  58h/02h says the chains are linked
;    2  58h/00h and /01h round-trip the whole strategy byte
;    3  48h with strategy 80h comes back at or above C000h
;    4  ...and that memory holds what is written to it
;    5  49h gives it back
;    6  unlinking makes 58h/02h say so
;    7  ...and 48h with strategy 80h then fails
;    8  relinking works, and the block comes back
;    9  the MCB chain walks from the first block to a 'Z' in upper
;       memory, passing through umb_head
;   10  XMS 10h hands out a UMB, and it too is writable
;   11  XMS 11h gives that one back
;
; Check 7 is the one that cannot be faked by a kernel that simply
; hands out conventional memory and calls it upper: with the chains
; detached there is no path to a UMB at all, and a "yes" there means
; the strategy bits were never read.
;
; On a machine without DOS=UMB this prints "no upper memory" and stops,
; which is not a failure - it is the answer to a different question.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_UT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_UT, DS:_UT, ES:_UT, SS:_UT
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results

;=====================================================================
; Is there any?  52h hands back ES:BX, and umb_head is a WORD IN THE
; SEGMENT rather than a field of the structure BX points at - offset
; 008Ch, which is where MS-DOS's own MEM.EXE reads it and the only
; reason the offset is a contract.  FFFFh means there is none.
;=====================================================================
        mov     ah,52h
        int     21h
        mov     ax,es
        mov     [lolseg],ax
        mov     ax,es:[8Ch]
        mov     [umbhead],ax
        cmp     ax,0FFFFh
        jne     have_umb

        mov     dx,OFFSET msg_none
        call    puts
        mov     ax,4C00h
        int     21h

have_umb:
        mov     dx,OFFSET msg_head
        call    puts
        mov     ax,[umbhead]
        call    put4
        mov     dx,OFFSET msg_crlf
        call    puts

;=====================================================================
; 1 - they start linked, because DOS=UMB is what built them
;=====================================================================
        mov     dx,OFFSET msg_t1
        call    puts
        call    getlink
        cmp     al,1
        jne     t1bad
        call    good
        jmp     short t2
t1bad:
        call    bad
t2:

;=====================================================================
; 2 - the strategy byte survives being set, HIGH BITS AND ALL.  That
;     is the whole of what a kernel has to store for LOADHIGH to be
;     able to ask for anything, and storing only the low two bits is
;     the failure that looks like success everywhere else.
;=====================================================================
        mov     dx,OFFSET msg_t2
        call    puts
        mov     ax,5800h                ; keep the real one
        int     21h
        mov     [savstrat],ax

        mov     ax,5801h
        mov     bx,41h                  ; upper first, best fit
        int     21h
        jc      t2bad
        mov     ax,5800h
        int     21h
        cmp     ax,41h
        jne     t2bad

        mov     ax,5801h                ; and 3 is not a fit
        mov     bx,03h
        int     21h
        jnc     t2bad
        call    good
        jmp     short t3
t2bad:
        call    bad
t3:

;=====================================================================
; 3 - 48h, upper only.  A segment below umb_head here means the bits
;     were stored and not acted on.
;=====================================================================
        mov     dx,OFFSET msg_t3
        call    puts
        mov     ax,5801h
        mov     bx,80h                  ; upper only, first fit
        int     21h
        mov     bx,40h                  ; 1K: small enough to fit anywhere
        mov     ah,48h
        int     21h
        jc      t3bad
        mov     [umbseg],ax
        cmp     ax,[umbhead]
        jbe     t3bad
        call    good
        jmp     short t4
t3bad:
        mov     word ptr [umbseg],0
        call    bad
t4:

;=====================================================================
; 4 - and it is real memory.  A page table entry that was never
;     repointed leaves an address that reads back FFFFh whatever is
;     stored to it, which is precisely the state this is here to tell
;     apart from working.
;=====================================================================
        mov     dx,OFFSET msg_t4
        call    puts
        cmp     word ptr [umbseg],0
        je      t4bad
        push    es
        mov     es,[umbseg]
        mov     di,0
        mov     cx,512                  ; the whole kilobyte, as words
        mov     ax,05A5Ah
t4fill:
        stosw
        xor     ax,0FFFFh               ; alternating, so that a stuck bus
        inc     ax                      ; cannot pass by holding still
        loop    t4fill

        mov     di,0
        mov     cx,512
        mov     ax,05A5Ah
t4chk:
        cmp     es:[di],ax
        jne     t4popbad
        add     di,2
        xor     ax,0FFFFh
        inc     ax
        loop    t4chk
        pop     es
        call    good
        jmp     short t5
t4popbad:
        pop     es
t4bad:
        call    bad
t5:

;=====================================================================
; 5 - 49h takes it back
;=====================================================================
        mov     dx,OFFSET msg_t5
        call    puts
        cmp     word ptr [umbseg],0
        je      t5bad
        push    es
        mov     es,[umbseg]
        mov     ah,49h
        int     21h
        pop     es
        jc      t5bad
        call    good
        jmp     short t6
t5bad:
        call    bad
t6:

;=====================================================================
; 6 - unlink, and be told so
;=====================================================================
        mov     dx,OFFSET msg_t6
        call    puts
        mov     ax,5803h
        mov     bx,0
        int     21h
        jc      t6bad
        call    getlink
        or      al,al
        jnz     t6bad
        call    good
        jmp     short t7
t6bad:
        call    bad
t7:

;=====================================================================
; 7 - AND NOW THERE IS NO WAY UP THERE.  The chain ends at the 'Z' in
;     conventional memory, so an upper-only allocation has nothing to
;     walk to.  A kernel that answered this one has never looked at
;     the link state at all.
;=====================================================================
        mov     dx,OFFSET msg_t7
        call    puts
        mov     ax,5801h
        mov     bx,80h
        int     21h
        mov     bx,40h
        mov     ah,48h
        int     21h
        jnc     t7bad                   ; it succeeded: nothing is enforced
        call    good
        jmp     short t8
t7bad:
        push    es                      ; ...and give back what we got, so
        mov     es,ax                   ; that the rest of the run is clean
        mov     ah,49h
        int     21h
        pop     es
        call    bad
t8:

;=====================================================================
; 8 - link again, and it works again
;=====================================================================
        mov     dx,OFFSET msg_t8
        call    puts
        mov     ax,5803h
        mov     bx,1
        int     21h
        jc      t8bad
        mov     bx,40h
        mov     ah,48h
        int     21h
        jc      t8bad
        mov     [umbseg],ax
        cmp     ax,[umbhead]
        jbe     t8bad
        push    es
        mov     es,ax
        mov     ah,49h
        int     21h
        pop     es
        call    good
        jmp     short t9
t8bad:
        call    bad
t9:

;=====================================================================
; 9 - the chain itself, walked the way MEM walks it: from the first
;     MCB at [ES:BX-2] of 52h, along the lengths, until a 'Z'.  It has
;     to reach umb_head and it has to end ABOVE it, or the two halves
;     are not joined however cheerfully 58h/02h answers.
;=====================================================================
        mov     dx,OFFSET msg_t9
        call    puts
        mov     ah,52h
        int     21h
        mov     ax,es:[bx-2]            ; the first MCB in the chain
        mov     [mcbseg],ax
        xor     dx,dx                   ; DL: seen umb_head.  DH: guard
        push    es
t9step:
        mov     es,[mcbseg]
        mov     ax,[mcbseg]
        cmp     ax,[umbhead]
        jne     t9nothead
        mov     dl,1
t9nothead:
        cmp     byte ptr es:[0],'Z'
        je      t9last
        cmp     byte ptr es:[0],'M'
        jne     t9broken
        mov     ax,es:[3]               ; paragraphs
        add     ax,[mcbseg]
        jc      t9broken                ; wrapped: the chain is nonsense
        inc     ax
        mov     [mcbseg],ax
        inc     dh
        cmp     dh,200                  ; a chain longer than this is a loop
        jb      t9step
t9broken:
        pop     es
        call    bad
        jmp     short t10
t9last:
        pop     es
        or      dl,dl
        jz      t9no                    ; never passed umb_head
        mov     ax,[mcbseg]
        cmp     ax,[umbhead]
        jbe     t9no                    ; the last block is not up there
        call    good
        jmp     short t10
t9no:
        call    bad
t10:

;=====================================================================
; 10 - the other door.  XMS function 10h asks the same pool for the
;      same memory, and a kernel with two pools hands out a paragraph
;      that 48h has already given to somebody.
;=====================================================================
        mov     dx,OFFSET msg_t10
        call    puts
        mov     ax,4300h                ; is there an XMS driver?
        int     2Fh
        cmp     al,80h
        jne     t10bad
        mov     ax,4310h
        int     2Fh
        mov     word ptr [xmsent],bx
        mov     word ptr [xmsent+2],es

        mov     ah,10h
        mov     dx,40h                  ; 1K again
        call    dword ptr [xmsent]
        or      ax,ax
        jz      t10bad
        mov     [xmbseg],bx
        cmp     bx,[umbhead]
        jbe     t10bad

        push    es                      ; and writable, as before
        mov     es,bx
        mov     word ptr es:[0],1234h
        mov     word ptr es:[2],5678h
        cmp     word ptr es:[0],1234h
        jne     t10popbad
        cmp     word ptr es:[2],5678h
        jne     t10popbad
        pop     es
        call    good
        jmp     short t11
t10popbad:
        pop     es
t10bad:
        mov     word ptr [xmbseg],0
        call    bad
t11:

;=====================================================================
; 11 - and XMS 11h releases it
;=====================================================================
        mov     dx,OFFSET msg_t11
        call    puts
        cmp     word ptr [xmbseg],0
        je      t11bad
        mov     ah,11h
        mov     dx,[xmbseg]
        call    dword ptr [xmsent]
        or      ax,ax
        jz      t11bad
        call    good
        jmp     short done
t11bad:
        call    bad
done:

;=====================================================================
; put the machine back the way it was found
;=====================================================================
        mov     ax,5801h
        mov     bx,[savstrat]
        int     21h
        mov     ax,5803h
        mov     bx,1
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

        mov     al,[fails]
        mov     ah,4Ch
        int     21h

;---------------------------------------------------------------------
; getlink - AL = the UMB link state as 58h/02h reports it
;---------------------------------------------------------------------
getlink:
        mov     ax,5802h
        int     21h
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
lolseg      dw  0
umbhead     dw  0
umbseg      dw  0
xmbseg      dw  0
mcbseg      dw  0
savstrat    dw  0
xmsent      dd  0
fails       db  0
resptr      dw  0

msg_hi      db  'UMBTEST - upper memory, from inside a DOS box',13,10,13,10,'$'
msg_none    db  'There is no upper memory on this machine.',13,10
            db  'Put DOS=UMB in CONFIG.SYS and boot again.',13,10,'$'
msg_head    db  'upper memory begins at segment ','$'

msg_t1      db  ' 1 they start linked                 ','$'
msg_t2      db  ' 2 the strategy byte round-trips     ','$'
msg_t3      db  ' 3 48h upper-only lands up there     ','$'
msg_t4      db  ' 4 and the memory is really there    ','$'
msg_t5      db  ' 5 49h takes it back                 ','$'
msg_t6      db  ' 6 58h/03h unlinks them              ','$'
msg_t7      db  ' 7 unlinked, upper-only must fail    ','$'
msg_t8      db  ' 8 relinked, and it works again      ','$'
msg_t9      db  ' 9 the MCB chain runs through it     ','$'
msg_t10     db  '10 XMS 10h hands out the same pool   ','$'
msg_t11     db  '11 XMS 11h releases it               ','$'

msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_res     db  13,10,'results: ','$'
msg_fails   db  '   failures: ','$'
msg_crlf    db  13,10,'$'
results     db  24 dup (0)

_UT     ENDS
        END     start
