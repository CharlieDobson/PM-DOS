;=====================================================================
; VDSTEST.ASM - VDSTEST.COM, does a driver get told where its buffer
;               really is?
;
; A card that moves its own data is handed a PHYSICAL address, and a
; real-mode driver works one out as segment times sixteen plus offset.
; That is wrong for a buffer in upper memory, which is somewhere above
; 1MB and only appears at C8000h; Virtual DMA Services (INT 4Bh AH=81h,
; IO\VDS.INC) is how a driver asks instead.
;
; The checks that matter are the last two.  The rest establish that
; the interface answers as its specification says; those two take a
; block of upper memory, ask where it is, and then READ THAT ADDRESS
; by another road - from protected mode, as a DPMI client, through a
; descriptor whose base is the address VDS gave - to see whether what
; was written through C800:0000 is what is there.  An answer that was
; merely plausible would not survive it.
;
; Twelve checks:
;
;    1  the BIOS data area says VDS is here (40:7Bh bit 5)
;    2  8102h answers with a version, and no buffer to lend
;    3  8103h: a conventional buffer is where segment arithmetic says
;    4  ...a region across a 64K line is refused, with how much fits
;    5  8105h: the same buffer as one piece
;    6  ...and as page table entries, with the offset into the first
;    7  ...and with a table too short, which says how long it must be
;    8  8107h: no buffer, and the specification's code for that
;    9  a function that does not exist is refused
;   10  INT 4Bh that is not VDS comes back
;   11  8103h: a block of UPPER memory is somewhere else...
;   12  ...and that is where its contents are
;
; Without DOS=UMB there is no upper memory and the last two are
; skipped, which is not a failure; so is the last without DPMI.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_VT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_VT, DS:_VT, ES:_VT, SS:_VT
        ORG     0

VDS_FLAG        EQU     20h             ; of 40:7Bh
F_64K           EQU     0010h
F_PTES          EQU     0040h
E_ALIGN         EQU     02h
E_NOBUF         EQU     04h
E_TABLE         EQU     09h
E_UNSUP         EQU     0Fh
BUFLEN          EQU     3000h
PATLEN          EQU     256

start:
        mov     ah,4Ah                  ; a .COM is given all the memory
        mov     bx,2400h                ; there is, and the DPMI host will
        int     21h                     ; want a little of it back

        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results

        ; a .COM owns everything up to the top of conventional memory,
        ; and the buffers below are in it, past the program's own 64K
        mov     ax,cs
        add     ax,1000h
        mov     [bufseg],ax

;=====================================================================
; 1 - the flag a driver looks at before it asks at all
;=====================================================================
        mov     dx,OFFSET msg_t1
        call    puts
        push    es
        mov     ax,40h
        mov     es,ax
        mov     al,es:[7Bh]
        pop     es
        test    al,VDS_FLAG
        jz      t1_bad
        call    good
        jmp     short t2
t1_bad:
        call    bad
        jmp     finish                  ; and nothing below would answer

;=====================================================================
; 2 - who is there
;=====================================================================
t2:
        mov     dx,OFFSET msg_t2
        call    puts
        mov     ax,8102h
        xor     dx,dx
        mov     si,0FFFFh
        mov     di,0FFFFh
        int     4Bh
        jc      t2_bad
        cmp     ah,1
        jne     t2_bad
        or      si,di                   ; the buffer it has to lend: none
        jnz     t2_bad
        call    good
        jmp     short t3
t2_bad:
        call    bad

;=====================================================================
; 3 - a conventional buffer
;=====================================================================
t3:
        mov     dx,OFFSET msg_t3
        call    puts
        mov     dword ptr [dds_size],BUFLEN
        mov     dword ptr [dds_off],0123h
        mov     ax,[bufseg]
        mov     [dds_seg],ax
        mov     dword ptr [dds_phys],-1
        mov     di,OFFSET dds
        xor     dx,dx
        mov     ax,8103h
        int     4Bh
        jc      t3_bad
        call    linear                  ; EAX = segment * 16 + offset
        cmp     eax,[dds_phys]
        jne     t3_bad
        cmp     word ptr [dds_buf],0
        jne     t3_bad
        mov     ax,8104h                ; and let go of it
        xor     dx,dx
        mov     di,OFFSET dds
        int     4Bh
        jc      t3_bad
        call    good
        jmp     short t4
t3_bad:
        call    bad

;=====================================================================
; 4 - across a 64K line.  The offset is chosen so that the region
;     starts a page short of one.
;=====================================================================
t4:
        mov     dx,OFFSET msg_t4
        call    puts
        movzx   eax,word ptr [bufseg]
        shl     eax,4
        neg     eax
        and     eax,0FFFFh              ; from the segment to the next line
        add     eax,0F000h              ; ...and on to a page before the
                                        ; one after that
        mov     [dds_off],eax
        mov     dword ptr [dds_size],BUFLEN
        mov     di,OFFSET dds
        mov     dx,F_64K
        mov     ax,8103h
        int     4Bh
        jnc     t4_bad
        cmp     al,E_ALIGN
        jne     t4_bad
        cmp     dword ptr [dds_size],1000h
        jne     t4_bad
        call    good
        jmp     short t5
t4_bad:
        call    bad

;=====================================================================
; 5 - one piece, as a list
;=====================================================================
t5:
        mov     dx,OFFSET msg_t5
        call    puts
        call    edds_new
        mov     word ptr [dds_avail],8
        mov     di,OFFSET dds
        xor     dx,dx
        mov     ax,8105h
        int     4Bh
        jc      t5_bad
        cmp     word ptr [dds_used],1
        jne     t5_bad
        call    linear
        cmp     eax,[dds_table]
        jne     t5_bad
        cmp     dword ptr [dds_table + 4],BUFLEN
        jne     t5_bad
        mov     ax,8106h
        xor     dx,dx
        mov     di,OFFSET dds
        int     4Bh
        jc      t5_bad
        call    good
        jmp     short t6
t5_bad:
        call    bad

;=====================================================================
; 6 - page table entries
;=====================================================================
t6:
        mov     dx,OFFSET msg_t6
        call    puts
        call    edds_new
        mov     word ptr [dds_avail],8
        mov     di,OFFSET dds
        mov     dx,F_PTES
        mov     ax,8105h
        xor     bx,bx
        int     4Bh
        jc      t6_bad
        call    linear
        mov     ecx,eax
        and     ecx,0FFFh
        cmp     bx,cx                   ; the offset into the first page
        jne     t6_bad
        add     ecx,BUFLEN + 0FFFh
        shr     ecx,12                  ; pages it touches
        cmp     cx,[dds_used]
        jne     t6_bad
        and     eax,0FFFFF000h
        or      eax,1
        mov     si,OFFSET dds_table
t6_lp:
        cmp     eax,[si]
        jne     t6_bad
        add     eax,1000h
        add     si,4
        loop    t6_lp
        call    good
        jmp     short t7
t6_bad:
        call    bad

;=====================================================================
; 7 - a table with room for one entry, and four pages to describe
;=====================================================================
t7:
        mov     dx,OFFSET msg_t7
        call    puts
        call    edds_new
        mov     word ptr [dds_avail],1
        mov     di,OFFSET dds
        mov     dx,F_PTES
        mov     ax,8105h
        int     4Bh
        jnc     t7_bad
        cmp     al,E_TABLE
        jne     t7_bad
        call    linear
        and     eax,0FFFh
        add     eax,BUFLEN + 0FFFh
        shr     eax,12
        cmp     ax,[dds_used]
        jne     t7_bad
        call    good
        jmp     short t8
t7_bad:
        call    bad

;=====================================================================
; 8 - a buffer to borrow
;=====================================================================
t8:
        mov     dx,OFFSET msg_t8
        call    puts
        call    edds_new
        mov     di,OFFSET dds
        xor     dx,dx
        mov     ax,8107h
        int     4Bh
        jnc     t8_bad
        cmp     al,E_NOBUF
        jne     t8_bad
        call    good
        jmp     short t9
t8_bad:
        call    bad

;=====================================================================
; 9 - no such function
;=====================================================================
t9:
        mov     dx,OFFSET msg_t9
        call    puts
        mov     ax,81F0h
        xor     dx,dx
        int     4Bh
        jnc     t9_bad
        cmp     al,E_UNSUP
        jne     t9_bad
        call    good
        jmp     short t10
t9_bad:
        call    bad

;=====================================================================
; 10 - not VDS at all.  Whatever had the vector answers, or nothing
;      does; either way it has to come back, with the stack where it
;      was.
;=====================================================================
t10:
        mov     dx,OFFSET msg_t10
        call    puts
        mov     [keepsp],sp
        mov     ax,7F00h
        int     4Bh
        cmp     sp,[keepsp]
        jne     t10_bad
        call    good
        jmp     short t11
t10_bad:
        mov     sp,[keepsp]
        call    bad

;=====================================================================
; 11 - upper memory.  48h with the strategy set to "upper memory
;      only", which fails rather than quietly handing out low memory.
;=====================================================================
t11:
        mov     dx,OFFSET msg_t11
        call    puts
        mov     ax,5800h
        int     21h
        mov     [strat],ax
        mov     ax,5802h
        int     21h
        mov     [linked],al
        mov     ax,5803h
        mov     bx,1
        int     21h
        mov     ax,5801h
        mov     bx,0040h                ; upper memory only, first fit
        int     21h
        mov     ah,48h
        mov     bx,0200h                ; 8K: two pages at the least
        int     21h
        pushf
        push    ax
        mov     ax,5801h
        mov     bx,[strat]
        int     21h
        mov     ax,5803h
        movzx   bx,byte ptr [linked]
        int     21h
        pop     ax
        popf
        jc      t11_none
        cmp     ax,0A000h
        jb      t11_low
        mov     [umbseg],ax

        push    es                      ; something to recognise
        mov     es,ax
        xor     di,di
        mov     cx,PATLEN
        mov     al,5Ah
t11_fill:
        mov     es:[di],al
        add     al,7
        inc     di
        loop    t11_fill
        pop     es

        mov     dword ptr [dds_size],PATLEN
        mov     dword ptr [dds_off],0
        mov     ax,[umbseg]
        mov     [dds_seg],ax
        mov     dword ptr [dds_phys],-1
        mov     di,OFFSET dds
        xor     dx,dx
        mov     ax,8103h
        int     4Bh
        jc      t11_bad
        mov     eax,[dds_phys]
        call    put8
        call    linear
        cmp     eax,[dds_phys]
        je      t11_bad                 ; at C8000h there is a hole
        cmp     dword ptr [dds_phys],00100000h
        jb      t11_bad
        call    good
        jmp     short t12
t11_low:
        push    es
        mov     es,ax
        mov     ah,49h
        int     21h
        pop     es
t11_none:
        mov     dl,'-'
        call    tally
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_noumb
        call    puts
        jmp     finish
t11_bad:
        call    bad
        jmp     t12_free

;=====================================================================
; 12 - and is that where it is?  Into protected mode, where an
;      address is whatever a descriptor says it is.  Everything from
;      in_pm on runs there, the messages included; the block of upper
;      memory goes back with the rest when the program ends.
;=====================================================================
t12:
        mov     dx,OFFSET msg_t12
        call    puts
        mov     ax,1687h
        int     2Fh
        or      ax,ax
        jnz     t12_none
        mov     [entry_off],di
        mov     [entry_seg],es
        push    cs
        pop     es
        mov     bx,si                   ; the host's own data, if it wants
        or      bx,bx                   ; any
        jz      t12_switch
        mov     ah,48h
        int     21h
        jc      t12_none
        mov     es,ax
t12_switch:
        xor     ax,ax                   ; a 16-bit client
        call    dword ptr [entry_off]
        jc      t12_none

        mov     ax,0000h                ; a descriptor...
        mov     cx,1
        int     31h
        jc      t12_bad
        mov     [physsel],ax
        mov     bx,ax
        mov     cx,word ptr [dds_phys + 2]
        mov     dx,word ptr [dds_phys]
        mov     ax,0007h                ; ...based where VDS said
        int     31h
        jc      t12_bad
        mov     bx,[physsel]
        xor     cx,cx
        mov     dx,PATLEN - 1
        mov     ax,0008h
        int     31h
        jc      t12_bad

        push    es
        mov     es,[physsel]
        xor     di,di
        mov     cx,PATLEN
        mov     al,5Ah
t12_cmp:
        cmp     es:[di],al
        jne     t12_differ
        add     al,7
        inc     di
        loop    t12_cmp
        pop     es
        call    good
        jmp     finish
t12_differ:
        pop     es
t12_bad:
        call    bad
        jmp     finish
t12_none:
        push    cs
        pop     es
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_nodpmi
        call    puts
        jmp     finish
t12_free:
        push    es
        mov     es,[umbseg]
        mov     ah,49h
        int     21h
        pop     es

finish:
        mov     dx,OFFSET msg_res
        call    puts
        mov     dx,OFFSET results
        call    puts
        mov     dx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        jne     failed
        mov     dx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h
failed:
        mov     dx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; edds_new - the descriptor, for the conventional buffer, with an
;            empty table behind it
;=====================================================================
edds_new:
        push    ax
        push    cx
        push    di
        mov     dword ptr [dds_size],BUFLEN
        mov     dword ptr [dds_off],0123h
        mov     ax,[bufseg]
        mov     [dds_seg],ax
        mov     word ptr [dds_buf],0
        mov     word ptr [dds_avail],0
        mov     word ptr [dds_used],0
        mov     di,OFFSET dds_table
        mov     cx,32
        xor     ax,ax
        rep     stosw
        pop     di
        pop     cx
        pop     ax
        ret

;--- linear - EAX = the descriptor's segment * 16 + its offset
linear:
        movzx   eax,word ptr [dds_seg]
        shl     eax,4
        add     eax,[dds_off]
        ret

;=====================================================================
; helpers
;=====================================================================
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
put8:                                   ; EAX in hex, and a space
        push    eax
        push    cx
        mov     cx,8
p8_lp:
        rol     eax,4
        push    eax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p8_d
        add     al,7
p8_d:
        mov     dl,al
        call    putc
        pop     eax
        loop    p8_lp
        mov     dl,' '
        call    putc
        pop     cx
        pop     eax
        ret

;=====================================================================
; data
;=====================================================================
; The descriptor, which is two structures that begin the same way: the
; plain one ends with the physical address, the extended one with two
; counts and a table.
dds:
dds_size    dd  0
dds_off     dd  0
dds_seg     dw  0
dds_buf     dw  0
dds_phys    label dword
dds_avail   dw  0
dds_used    dw  0
dds_table   dd  16 dup(0)

entry_off   dw  0                       ; the host's mode switch
entry_seg   dw  0
physsel     dw  0
bufseg      dw  0
umbseg      dw  0
strat       dw  0
keepsp      dw  0
resptr      dw  0
linked      db  0
fails       db  0
results     db  16 dup('$')

msg_hi      db  13,10,'VDSTEST - is a driver told where its buffer is?'
            db  13,10,13,10,'$'
msg_t1      db  ' 1 40:7Bh says VDS is here               $'
msg_t2      db  ' 2 8102h: a version, and no buffer       $'
msg_t3      db  ' 3 8103h: conventional memory is itself  $'
msg_t4      db  ' 4 ...a 64K line is a line               $'
msg_t5      db  ' 5 8105h: one piece                      $'
msg_t6      db  ' 6 ...as page table entries              $'
msg_t7      db  ' 7 ...and a table too short              $'
msg_t8      db  ' 8 8107h: no buffer to borrow            $'
msg_t9      db  ' 9 no such function                      $'
msg_t10     db  '10 INT 4Bh that is not VDS comes back    $'
msg_t11     db  '11 8103h: upper memory is elsewhere      $'
msg_t12     db  '12 ...and that is where its contents are $'
msg_noumb   db  'no upper memory (skipped)',13,10,'$'
msg_nodpmi  db  'no DPMI host (skipped)',13,10,'$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_pass    db  'VDSTEST: all checks passed',13,10,'$'
msg_fail    db  'VDSTEST: FAILURES',13,10,'$'

_VT     ENDS
        END     start
