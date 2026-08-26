;=====================================================================
; MCBTEST.ASM - MCBTEST.COM, the memory arena as a 16-bit program sees
;               it.
;
; MS-DOS 6.22's MEM.EXE locks a PM-DOS machine up, and the monitor's
; own trace says where: MEM asks 58h for the UMB link state, asks 52h
; for the list of lists, asks 30h for the version - and then makes no
; DOS call ever again, while burning the CPU.  A guest that spins
; without calling anybody is walking something in memory, and the
; thing MEM walks after 52h is the memory control block chain.
;
; So this walks it the same way and, unlike MEM, counts.  Every check
; here is a way the chain can be wrong that a walker cannot survive:
;
;   1  52h answers, and [ES:BX-2] names a first block worth following
;   2  every header says 'M' or 'Z' - anything else and a walker is
;      reading whatever happens to be there as a length
;   3  a 'Z' arrives.  THE LOOP THE WHOLE FILE IS ABOUT: no 'Z' means
;      no end, and MEM's walk is a for(;;)
;   4  the chain lands exactly on the top of conventional memory, so
;      the blocks tile the arena with no gap and no overlap
;   5  every owner is either free, DOS, or a PSP inside the arena
;
; Bounded at 64 blocks, because a test for an endless walk must not be
; an endless walk.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_MC     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_MC, DS:_MC, ES:_MC, SS:_MC
        ORG     0

MAXBLK  EQU     64

start:
        mov     dx,OFFSET msg_hi
        call    puts

        ; WHERE CONVENTIONAL MEMORY ENDS IS A QUESTION, NOT A CONSTANT.
        ; This file used to test the chain against a hardcoded 0A000h,
        ; and passed on a machine whose BIOS had taken the top
        ; kilobyte for its extended data area and said so - the arena
        ; really did run past the end of memory the BIOS owned, and
        ; the test agreed with the bug because it shared its
        ; assumption.  INT 12h is the machine's own answer.
        int     12h                     ; AX = KB of conventional memory
        mov     cx,64                   ; ...and 1K is 64 paragraphs
        mul     cx
        mov     [tpatop],ax

;=====================================================================
; 1 - where the chain starts.  52h hands back a pointer INTO the list
;     of lists, and the first MCB is the word in front of it - which
;     is the one field of the structure that is not at a positive
;     offset, and therefore the one most often got wrong.
;=====================================================================
        mov     dx,OFFSET t_first
        call    puts
        mov     ah,52h
        int     21h
        mov     ax,es
        or      ax,bx
        jz      mc_f_no                 ; 0000:0000 is not a list of lists
        mov     ax,es:[8Ch]             ; and where upper memory begins,
        mov     [umbhead],ax            ; which decides where the chain may
                                        ; legitimately end - see check 4
        mov     ax,es:[bx-2]
        mov     [first],ax
        call    put4
        or      ax,ax
        jz      mc_f_no
        cmp     ax,[tpatop]
        jae     mc_f_no
        call    ok
        jmp     short mc_walk
mc_f_no:
        call    bad
        jmp     mc_done

;=====================================================================
; the walk itself, printing as it goes.  The four remaining checks are
; all answered by what it finds, so they are judged after it stops.
;=====================================================================
mc_walk:
        mov     dx,OFFSET msg_head
        call    puts
        mov     ax,[first]
        mov     [cur],ax
        mov     word ptr [count],0
mc_lp:
        mov     es,[cur]
        mov     al,es:[0]               ; the signature
        mov     [sig],al
        mov     ax,es:[1]               ; the owning PSP
        mov     [own],ax
        mov     ax,es:[3]               ; length in paragraphs
        mov     [len],ax

        mov     dx,OFFSET msg_ind       ; ...one line about it
        call    puts
        mov     ax,[cur]
        call    put4
        mov     dl,' '
        call    putc
        mov     dl,[sig]
        cmp     dl,20h                  ; a signature that is not printable
        jb      mc_p_dot                ; is the interesting case, so show
        cmp     dl,7Eh                  ; SOMETHING rather than move the
        jbe     mc_p_ch                 ; cursor about
mc_p_dot:
        mov     dl,'?'
mc_p_ch:
        call    putc
        mov     dx,OFFSET msg_own
        call    puts
        mov     ax,[own]
        call    put4
        mov     dx,OFFSET msg_len
        call    puts
        mov     ax,[len]
        call    put4

        ; ...AND THE NAME, which is the field that says whose a block
        ; is when the owner word only says which PSP - and a PSP that
        ; has exited is a number pointing at nothing.  DOS 4 and later
        ; put the program's name in the eight bytes at +8 and every
        ; memory reporter prints it; a block still standing after its
        ; program has gone is exactly the case where the number cannot
        ; answer and the name can.
        mov     dx,OFFSET msg_nm
        call    puts
        xor     bx,bx
mc_nm:
        mov     dl,es:[bx+8]
        or      dl,dl
        jz      mc_nmsp                 ; a short name pads with blanks
        cmp     dl,20h
        jb      mc_nmdot
        cmp     dl,7Eh
        jbe     mc_nmch
mc_nmdot:
        mov     dl,'.'
mc_nmch:
        call    putc
        jmp     short mc_nmnext
mc_nmsp:
        mov     dl,' '
        call    putc
mc_nmnext:
        inc     bx
        cmp     bx,8
        jb      mc_nm

        mov     dx,OFFSET msg_crlf
        call    puts

        ; check 2 - the signature has to be one of the two
        cmp     byte ptr [sig],'M'
        je      mc_sigok
        cmp     byte ptr [sig],'Z'
        je      mc_sigok
        mov     byte ptr [badsig],1
        jmp     short mc_stop           ; and there is no length to trust
mc_sigok:
        ; check 5 - and the owner has to be somebody
        cmp     word ptr [own],0
        je      mc_ownok
        cmp     word ptr [own],8        ; DOS itself
        je      mc_ownok
        mov     ax,[own]
        cmp     ax,[first]
        jb      mc_ownbad
        cmp     ax,[maxown]             ; THE CEILING IS NOT THE TOP OF
        jbe     mc_ownok                ; CONVENTIONAL MEMORY ANY MORE.  A
        mov     [maxown],ax             ; program loaded high has a PSP up
        jmp     short mc_ownok          ; there, so the honest test is that
mc_ownbad:                              ; no owner is above the LAST BLOCK,
        mov     byte ptr [badown],1     ; judged where that is known
mc_ownok:
        mov     ax,[cur]                ; did the walk pass through the
        cmp     ax,[umbhead]            ; paragraph upper memory starts at?
        jne     mc_nothead
        mov     byte ptr [sawumb],1
mc_nothead:
        cmp     byte ptr [sig],'Z'
        je      mc_stop                 ; the end, and check 3 passes

        mov     ax,[cur]                ; next = this + length + its header
        add     ax,[len]
        inc     ax
        jc      mc_stop                 ; wrapped: it cannot be a segment
        cmp     ax,[cur]
        jbe     mc_stop                 ; ...nor can it go backwards
        mov     [cur],ax
        inc     word ptr [count]
        mov     ax,[count]
        cmp     ax,MAXBLK
        jb      mc_lp
        mov     byte ptr [noend],1      ; THE BUG THIS FILE IS FOR
mc_stop:

;=====================================================================
; 3 - was there an end at all?
;=====================================================================
        mov     dx,OFFSET t_end
        call    puts
        cmp     byte ptr [noend],0
        je      mc_e_ok
        mov     dx,OFFSET msg_noend
        call    puts
        call    bad
        jmp     short mc_sig
mc_e_ok:
        cmp     byte ptr [sig],'Z'
        jne     mc_e_no
        call    ok
        jmp     short mc_sig
mc_e_no:
        call    bad

;=====================================================================
; 2 - and were the signatures sound on the way?
;=====================================================================
mc_sig:
        mov     dx,OFFSET t_sig
        call    puts
        cmp     byte ptr [badsig],0
        jne     mc_s_no
        call    ok
        jmp     short mc_top
mc_s_no:
        call    bad

;=====================================================================
; 4 - the blocks have to tile memory exactly.  cur + len + 1 for the
;     LAST block is the paragraph after the chain: short of the end
;     leaves memory nobody can reach, past it hands out memory that is
;     not there.
;
;     WHICH END, THOUGH, IS UMB_HEAD'S TO SAY.  With no upper memory
;     the chain stops at the top of conventional memory and this is
;     the test it always was.  With upper memory the chain runs on
;     THROUGH it, and what has to be true instead is three things: the
;     walk passed through umb_head, umb_head is the last paragraph of
;     conventional memory (which is the contract, and reads like an
;     off-by-one until you ask where that block's header could
;     otherwise live), and the chain ends somewhere above it.
;
;     This test used to encode "the chain ends at the top of
;     conventional memory" as if it were arithmetic.  It was a fact
;     about a machine with one region.
;=====================================================================
mc_top:
        mov     dx,OFFSET t_top
        call    puts
        mov     ax,[cur]
        add     ax,[len]
        inc     ax
        mov     [chainend],ax
        call    put4
        mov     dl,'/'
        call    putc

        cmp     word ptr [umbhead],0FFFFh
        jne     mc_t_umb

        mov     ax,[tpatop]             ; no upper memory: the old test
        call    put4
        mov     ax,[chainend]
        cmp     ax,[tpatop]
        jne     mc_t_no
        call    ok
        jmp     short mc_own

mc_t_umb:
        mov     ax,[umbhead]
        call    put4
        cmp     byte ptr [sawumb],0     ; the walk has to have gone through
        je      mc_t_no                 ; the block upper memory starts at
        mov     ax,[umbhead]
        inc     ax
        cmp     ax,[tpatop]             ; ...and that block's header is the
        jne     mc_t_no                 ; last paragraph of the TPA
        mov     ax,[chainend]
        cmp     ax,[tpatop]
        jbe     mc_t_no                 ; and the chain ends above it
        call    ok
        jmp     short mc_own
mc_t_no:
        call    bad

;=====================================================================
; 5 - the owners
;=====================================================================
mc_own:
        mov     dx,OFFSET t_own
        call    puts
        mov     ax,[maxown]             ; nobody may own memory past the
        cmp     ax,[chainend]           ; end of the chain
        jbe     mc_o_inrange
        mov     byte ptr [badown],1
mc_o_inrange:
        cmp     byte ptr [badown],0
        jne     mc_o_no
        call    ok
        jmp     short mc_done
mc_o_no:
        call    bad

mc_done:
        mov     dx,OFFSET msg_bye
        call    puts
        mov     al,[fails]
        or      al,al
        jz      mc_exit
        push    ax
        mov     dx,OFFSET msg_fails
        call    puts
        pop     ax
        call    put2
        mov     dx,OFFSET msg_crlf
        call    puts
mc_exit:
        mov     al,[fails]
        mov     ah,4Ch
        int     21h

;=====================================================================
; helpers
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
        inc     byte ptr [fails]
        pop     dx
        ret

puts:
        push    ax
        push    ds
        push    cs
        pop     ds
        mov     ah,09h
        int     21h
        pop     ds
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
first       dw  0
tpatop      dw  0A000h                  ; INT 12h's answer, in paragraphs
cur         dw  0
len         dw  0
own         dw  0
count       dw  0
umbhead     dw  0FFFFh                  ; DOSDATA:008Ch, or FFFFh for none
chainend    dw  0
maxown      dw  0
sig         db  0
badsig      db  0
badown      db  0
sawumb      db  0
noend       db  0
fails       db  0

msg_hi      db  13,10,'MCBTEST - the memory arena, walked the way MEM walks it',13,10,13,10,'$'
msg_bye     db  13,10,'MCBTEST done.',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_fails   db  'checks failed: $'
msg_head    db  13,10,'      seg sig  owner   paras  name',13,10,'$'
msg_ind     db  '      $'
msg_own     db  '    $'
msg_len     db  '    $'
msg_nm      db  '   $'
msg_noend   db  ' no Z in 64 blocks -$'

t_first     db  '  1 52h, and [ES:BX-2] says           $'
t_end       db  13,10,'  2 the chain ends in a Z             $'
t_sig       db  '  3 every signature was M or Z        $'
t_top       db  '  4 and they tile to the end: got/want $'
t_own       db  '  5 every owner is free, DOS or a PSP $'

_MC     ENDS
        END     start
