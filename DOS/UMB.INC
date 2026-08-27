;=====================================================================
; UMB.INC - upper memory blocks, and the arena that describes them
;
; DEVICEHIGH= and LOADHIGH have meant "load it low and say nothing"
; since they were first recognised, which is what a real DOS with no
; upper-memory provider does and was the only honest answer while
; there was no upper memory.  This file is the upper memory.
;
; THREE THINGS HAVE TO AGREE AND ONLY ONE OF THEM IS THE HARD PART.
;
; Finding the memory is easy here in a way it never was on a real DOS.
; A real-mode memory manager can only offer upper memory that some
; chipset has already been talked into putting RAM behind - which is
; what EMM386 spent its life doing, and why it needed V86 mode to do
; it.  PM-DOS is already paging.  An address in C0000h-EFFFFh that
; nothing decodes is an address with a page table entry pointing at
; itself and no memory on the other end, and pointing it at a page out
; of the arena instead costs one dword.  See pg_umbprobe and pg_map.
;
; Describing the memory is the hard part, because the description is
; an MCB CHAIN and an MCB chain is CONTIGUOUS.  Every block's header
; sits one paragraph before its data and the next block begins exactly
; where this one ends, so a chain cannot skip over the display, the
; video BIOS, or a gap between two runs of usable pages - it has to
; cover them, with blocks that are owned so that nothing allocates
; them.  Those filler blocks are the whole of the complication here.
;
; AND A FILLER BLOCK'S HEADER CANNOT LIVE IN THE HOLE IT DESCRIBES.
; Sixteen bytes have to be stored somewhere, and the first paragraph
; of a hole is by definition a paragraph nothing will store anything
; in.  So each filler's header goes in the LAST PARAGRAPH OF THE RUN
; BELOW IT, which is memory we do have.  That is why umb_head points
; at the last paragraph of conventional memory rather than at the
; first upper block: the first filler covers the display and the ROMs,
; and its header is the top sixteen bytes of the transient program
; area.  MS-DOS does precisely this - sysconf.asm, LinkFirstUMB, "cx =
; ax - 1" where ax is the size of memory in paragraphs - and it reads
; like an off-by-one in that source too.
;
; Linking is then one byte.  The last conventional block's signature
; is 'Z' when the upper chain is detached and 'M' when it is joined
; on, and everything that walks the chain - lm_alloc, MEM, a program
; counting its memory - follows it or stops at it without knowing that
; a decision was made.  Real DOS does exactly and only this (alloc.asm,
; LinkUmbs), and it is worth admiring: the state is stored in the same
; place that acts on it, so the two cannot disagree.
;
; Included by DOSMAIN.ASM.
;=====================================================================

UMB_NONE        EQU     0FFFFh          ; what umb_head says when there is
                                        ; no upper memory, and what DOS's
                                        ; own kernel tests for

;---------------------------------------------------------------------
; The runs pg_umbprobe found, as segments.  A run is a maximal stretch
; of pages that are usable - either because RAM was already there or
; because one has been mapped under them.
;---------------------------------------------------------------------
umb_nrun    dd  0
umb_rstart  dd  UMB_MAXRUN dup (0)      ; first paragraph of the run
umb_rend    dd  UMB_MAXRUN dup (0)      ; one paragraph past its last

umb_state   db  UMB_SCAN_PAGES dup (0)  ; one UMBP_* per page
            db  0,0,0,0                 ; and alignment after it

umb_probed  dd  0                       ; the measurement has been made

umb_have    dd  0                       ; non-zero once there are UMBs
umb_linked  dd  0                       ; the link state 58h/02h reports
umb_head    dd  0                       ; the filler block below the first
                                        ; run - what GLOL_UMBHEAD holds
umb_ceil    dd  0                       ; one past the last paragraph the
                                        ; chain describes, as a linear
                                        ; address: the walk ceiling
umb_backed  dd  0                       ; pages taken out of the arena
umb_wanted  dd  0                       ; DOS=UMB was seen

;=====================================================================
; umb_init - probe upper memory, back what is empty, and build the
;            chain.  Called once, from CONFIG.SYS processing, and only
;            if DOS=UMB asked for it.
;
; DOS=UMB IS THE ESCAPE HATCH AND THAT IS NOT LAZINESS.  Deciding what
; a machine has at D800h by writing to it and seeing what happens is a
; measurement, and a measurement can be wrong on hardware nobody here
; has - an adapter with a window and no option ROM in front of it will
; look exactly like shadow RAM.  A machine where this goes wrong is a
; machine that will not boot, and the only useful answer to that is a
; line the user can take out of CONFIG.SYS.  Real DOS has the same
; arrangement for the same reason and calls it EMM386.
;
; CF set if there was nothing to be had; umb_have says so afterwards.
;=====================================================================
umb_init:
        pushad
        cmp     dword ptr [umb_have],0
        jne     ui_already

;--- what is out there ------------------------------------------------
; Not necessarily for the first time: EMS= reserves the page frame
; out of this same measurement before this routine runs, and it has to
; be the SAME measurement or the frame would be offered twice.
        call    umb_probe

;--- back every empty page with one out of the arena -------------------
; A SPAN AT A TIME, and the span is the run of consecutive pages that
; nothing decodes.  The pages do not have to be contiguous in physical
; memory - that is the whole freedom paging buys here - but asking for
; them one at a time costs a wasted page EACH, because the arena is
; sixteen-byte granular and the only way to get a page-aligned address
; out of it is to ask for one page more and round up.  One span, one
; rounding.
;
; A span that cannot be backed is marked TAKEN rather than abandoning
; the whole idea: it costs that much upper memory and nothing else,
; and the run builder below already knows how to walk round it.
        xor     ebx,ebx
ui_back:
        cmp     ebx,UMB_SCAN_PAGES
        jae     ui_backed
        cmp     byte ptr [umb_state+ebx],UMBP_EMPTY
        jne     ui_bnext
        mov     ecx,ebx                         ; how far does it run?
ui_bspan:
        inc     ecx
        cmp     ecx,UMB_SCAN_PAGES
        jae     ui_bgot
        cmp     byte ptr [umb_state+ecx],UMBP_EMPTY
        je      ui_bspan
ui_bgot:
        sub     ecx,ebx                         ; pages in it

        push    ebx
        push    ecx
        mov     ebx,ecx
        shl     ebx,12
        add     ebx,1000h                       ; and room to align the front
        call    arena_alloc
        pop     ecx
        pop     ebx
        jc      ui_bfail

        add     eax,0FFFh
        and     eax,NOT 0FFFh                   ; page aligned, at a cost of
        mov     edx,eax                         ; less than one page
        mov     eax,ebx
        shl     eax,12
        add     eax,UMB_SCAN_LO
        add     [umb_backed],ecx
        call    dword ptr ds:[IOS_PGMAP]
        add     ebx,ecx                         ; past the whole span
        jmp     short ui_back

ui_bfail:
        mov     byte ptr [umb_state+ebx],UMBP_TAKEN
        inc     ebx
        dec     ecx
        jnz     ui_bfail
        jmp     short ui_back
ui_bnext:
        inc     ebx
        jmp     short ui_back
ui_backed:

;--- the usable pages, gathered into runs -----------------------------
        call    umb_runs
        cmp     dword ptr [umb_nrun],0
        je      ui_nothing

;--- and the chain that describes them --------------------------------
        call    umb_chain
        jc      ui_nothing

        mov     dword ptr [umb_have],1

; AND THEY START JOINED ON, because the only thing that calls this is
; DOS=UMB, and DOS=UMB is the statement that asks for them to be.  Real
; DOS builds the chain linked and unlinks it afterwards if the line was
; absent (sysconf.asm, LinkFirstUMB); here the line is what got us in
; at the door, so there is nothing to undo.
        call    umb_link
        popad
        clc
        ret

ui_already:
        popad
        clc
        ret
ui_nothing:
        popad
        stc
        ret

;=====================================================================
; umb_probe - fill in umb_state, ONCE.
;
; The probe is a MEASUREMENT OF HARDWARE - it writes a pattern into
; each page and sees whether anything stores it - so running it twice
; is not merely wasteful, it is wrong the second time: by then some of
; those pages have a page out of the arena underneath them and would
; measure as RAM that was always there.  Two callers want the answer
; and the second must get the first one's.
;=====================================================================
umb_probe:
        cmp     dword ptr [umb_probed],0
        jne     up_done
        pushad
        mov     edi,OFFSET umb_state
        call    dword ptr ds:[IOS_UMBPROBE]
        popad
        mov     dword ptr [umb_probed],1
up_done:
        ret

;=====================================================================
; umb_reserve - EAX = linear base, ECX = bytes.  Take that much of
;               upper memory out of what may be offered as UMBs.
;
; TAKEN is exactly the right word for it: the pages are spoken for by
; something that is not a UMB, which is the same thing an option ROM
; is as far as this file is concerned.  The chain builder already
; knows how to walk round a stretch of TAKEN pages - that is what the
; filler blocks are for - so a reservation costs nothing but the
; memory it reserves.
;
; The caller has already established that the range is worth having;
; this does not check, because the one caller measured it out of
; umb_state itself.  It must run BEFORE umb_init, and cfg_upper is
; where that order is kept.
;=====================================================================
umb_reserve:
        pushad
        sub     eax,UMB_SCAN_LO
        shr     eax,12                          ; the first page's index
        add     ecx,0FFFh
        shr     ecx,12                          ; and how many
        jecxz   urs_out
urs_mark:
        cmp     eax,UMB_SCAN_PAGES
        jae     urs_out
        mov     byte ptr [umb_state+eax],UMBP_TAKEN
        inc     eax
        loop    urs_mark
urs_out:
        popad
        ret

;=====================================================================
; umb_runs - turn umb_state into umb_rstart / umb_rend, in paragraphs.
;
; A run ends when a page is not usable OR when there are already
; UMB_MAXRUN of them, and the second is not a truncation worth
; apologising for: the runs are gathered lowest first, and by the time
; there are eight of them what is left is measured in single pages.
;=====================================================================
umb_runs:
        pushad
        mov     dword ptr [umb_nrun],0
        xor     ebx,ebx                         ; page index
        xor     esi,esi                         ; 0 = not in a run
ur_page:
        cmp     ebx,UMB_SCAN_PAGES
        jae     ur_tail
        cmp     byte ptr [umb_state+ebx],UMBP_TAKEN
        je      ur_gap
        or      esi,esi
        jnz     ur_next                         ; already in one
        mov     esi,ebx                         ; a run starts here
        inc     esi                             ; ...+1, so that zero can go
        jmp     short ur_next                   ; on meaning "not in a run"
ur_gap:
        or      esi,esi
        jz      ur_next
        call    ur_close
        xor     esi,esi
        cmp     dword ptr [umb_nrun],UMB_MAXRUN
        jae     ur_out
ur_next:
        inc     ebx
        jmp     short ur_page
ur_tail:
        or      esi,esi
        jz      ur_out
        call    ur_close
ur_out:
        popad
        ret

;--- ur_close - the run that started at page ESI-1 ends at page EBX ---
ur_close:
        push    eax
        push    edx
        mov     edx,[umb_nrun]
        cmp     edx,UMB_MAXRUN
        jae     urc_out
        mov     eax,esi
        dec     eax
        shl     eax,12
        add     eax,UMB_SCAN_LO
        shr     eax,4                           ; as a segment
        mov     [umb_rstart+edx*4],eax
        mov     eax,ebx
        shl     eax,12
        add     eax,UMB_SCAN_LO
        shr     eax,4
        mov     [umb_rend+edx*4],eax
        inc     dword ptr [umb_nrun]
urc_out:
        pop     edx
        pop     eax
        ret

;=====================================================================
; umb_chain - lay the MCB chain over the runs.
;
; It runs from the last paragraph of conventional memory to the end of
; the highest run, alternating filler and free:
;
;   umb_head     'M' owner 8 'SC'   the display, the ROMs, the gap
;   run 1 start  'M' owner 0        free, all but its last paragraph
;   run 1 end-1  'M' owner 8 'SC'   the gap to run 2
;   run 2 start  'M' owner 0        free
;   ...
;   run n start  'Z' owner 0        free, and the end of the chain
;
; EVERY FILLER'S HEADER IS THE LAST PARAGRAPH OF THE RUN BEFORE IT,
; which is what the file header explains and what costs each run its
; top sixteen bytes.  A run of exactly one page has fifteen usable
; paragraphs left out of two hundred and fifty-six, so the arithmetic
; below never has to worry about a run too small to carve; a run is
; never smaller than a page.
;
; CF if the machine's conventional memory does not end where the arena
; thinks it does, which would mean laying the first header somewhere
; that is not ours.
;=====================================================================
umb_chain:
        pushad

;--- the header of the first filler: the top paragraph of the TPA -----
        mov     eax,[lm_top]
        shr     eax,4                           ; conventional, in paragraphs
        cmp     eax,V86_TPA_SEG + 100h
        jbe     uc_bad                          ; nothing sane is down there
        dec     eax                             ; and the header goes in the
        mov     [umb_head],eax                  ; last paragraph of it

;--- give that paragraph back from the arena --------------------------
; THE ARENA HAS TO STOP ONE PARAGRAPH SOONER, or the last free block
; in conventional memory and the first filler's header are the same
; sixteen bytes and whichever is written second wins.  lm_shrinktop
; takes it off the end properly - by shortening whatever block is
; sitting there - rather than by moving a number and hoping.
        call    lm_shrinktop
        jc      uc_bad

;--- filler one: from the top of conventional memory to run one -------
        mov     esi,[umb_head]
        shl     esi,4                           ; flat, for the header
        mov     eax,[umb_rstart]
        mov     ebx,[umb_head]
        inc     ebx                             ; the first paragraph it covers
        sub     eax,ebx                         ; its length, in paragraphs
        call    umb_filler

;--- then a free block per run, each followed by the next filler ------
        xor     edi,edi                         ; run index
uc_run:
        mov     eax,[umb_rstart+edi*4]
        mov     esi,eax
        shl     esi,4
        mov     ebx,[umb_rend+edi*4]
        sub     ebx,eax                         ; paragraphs in the run
        dec     ebx                             ; less this block's own header

        mov     eax,edi
        inc     eax
        cmp     eax,[umb_nrun]
        jae     uc_last

        ; Not the last: the run gives up its top paragraph to be the
        ; header of the filler that reaches the next run.
        dec     ebx
        mov     byte ptr [esi+RMCB_SIG],'M'
        call    umb_free

        mov     esi,[umb_rend+edi*4]
        dec     esi
        mov     ebx,esi                         ; the header's own paragraph
        shl     esi,4
        mov     eax,[umb_rstart+edi*4+4]
        inc     ebx
        sub     eax,ebx
        call    umb_filler
        inc     edi
        jmp     short uc_run

uc_last:
        mov     byte ptr [esi+RMCB_SIG],'Z'
        call    umb_free
        mov     eax,[umb_rend+edi*4]
        shl     eax,4
        mov     [umb_ceil],eax
        mov     [lm_chtop],eax                  ; every walk may now go this far

;--- and tell the world where upper memory starts ---------------------
        mov     edi,DOSDATA
        mov     eax,[umb_head]
        mov     word ptr [edi+GLOL_UMBHEAD],ax
        popad
        clc
        ret

uc_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; umb_filler - ESI = flat address of the header, EAX = paragraphs it
;              covers.  A block the system owns and nothing allocates.
;
; Named 'SC' because that is what MS-DOS calls its own (sysconf.asm
; again), and because a memory reporter that knows the name prints
; something recognisable rather than eight bytes of whatever.
;---------------------------------------------------------------------
umb_filler:
        push    eax
        push    ecx
        mov     byte ptr [esi+RMCB_SIG],'M'
        mov     word ptr [esi+RMCB_OWN],8
        mov     [esi+RMCB_LEN],ax
        xor     ecx,ecx
        mov     [esi+5],cx
        mov     byte ptr [esi+7],cl
        mov     dword ptr [esi+RMCB_NAME],'  CS'
        mov     dword ptr [esi+RMCB_NAME+4],'    '
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; umb_free - ESI = flat address of a header whose signature is already
;            written, EBX = paragraphs.  Free, and nameless.
;---------------------------------------------------------------------
umb_free:
        push    ecx
        mov     word ptr [esi+RMCB_OWN],0
        mov     [esi+RMCB_LEN],bx
        xor     ecx,ecx
        mov     [esi+RMCB_NAME],ecx
        mov     [esi+RMCB_NAME+4],ecx
        pop     ecx
        ret

;=====================================================================
; umb_link / umb_unlink - join the upper chain onto the conventional
;                         one, or take it off again.  One byte each.
;
; The last conventional block is found by WALKING, every time, and not
; remembered: which block is last changes with every allocation and
; every free, and a remembered answer would be stale the first time a
; program asked for memory.  Real DOS walks it too (GetLastArena).
;
; CF if there is no upper memory or the chain does not lead where it
; should, which is what 58h/03h reports as an invalid function and a
; trashed arena respectively - and here as the one thing, because a
; caller that has been told no cannot use the difference.
;=====================================================================
umb_link:
        cmp     dword ptr [umb_have],0
        je      ul_bad
        cmp     dword ptr [umb_linked],0
        jne     ul_done
        call    umb_lastconv
        jc      ul_bad
        mov     byte ptr [esi+RMCB_SIG],'M'
        mov     dword ptr [umb_linked],1
ul_done:
        clc
        ret
ul_bad:
        stc
        ret

umb_unlink:
        cmp     dword ptr [umb_have],0
        je      ul_bad
        cmp     dword ptr [umb_linked],0
        je      uu_done
        call    umb_lastconv
        jc      ul_bad
        mov     byte ptr [esi+RMCB_SIG],'Z'
        mov     dword ptr [umb_linked],0
uu_done:
        clc
        ret

;---------------------------------------------------------------------
; umb_lastconv - ESI = the header of the last block below umb_head.
;                CF if the chain does not arrive there.
;
; The terminating condition depends on the link state, which is the
; one piece of this that cannot be inferred from the chain: unlinked,
; the walk stops at the 'Z'; linked, it stops at the block whose
; successor IS umb_head.  Getting that backwards walks into upper
; memory looking for a 'Z' that is up there and finds it, and the
; block "flipped" is then a real UMB.
;---------------------------------------------------------------------
umb_lastconv:
        push    eax
        push    ebx
        mov     esi,V86_TPA
ulc_walk:
        cmp     byte ptr [esi+RMCB_SIG],'M'
        je      ulc_live
        cmp     byte ptr [esi+RMCB_SIG],'Z'
        jne     ulc_bad
ulc_live:
        movzx   eax,word ptr [esi+RMCB_LEN]
        inc     eax
        shl     eax,4
        lea     ebx,[esi+eax]                   ; where the next one would be
        cmp     dword ptr [umb_linked],0
        jne     ulc_islast

        cmp     byte ptr [esi+RMCB_SIG],'Z'     ; unlinked: the 'Z' is the end
        je      ulc_got
        jmp     short ulc_step
ulc_islast:
        mov     eax,[umb_head]                  ; linked: the end is the block
        shl     eax,4                           ; that leads to umb_head
        cmp     ebx,eax
        je      ulc_got
ulc_step:
        mov     esi,ebx
        cmp     esi,[lm_top]
        jb      ulc_walk
ulc_bad:
        pop     ebx
        pop     eax
        stc
        ret
ulc_got:
        pop     ebx
        pop     eax
        clc
        ret

;=====================================================================
; umb_isupper - EAX = a block's HEADER, flat.  ZF clear if it is in
;               upper memory.  The one place the question is asked, so
;               that "upper" means one thing.
;=====================================================================
umb_isupper:
        push    edx
        xor     edx,edx
        cmp     dword ptr [umb_have],0
        je      uiu_out
        mov     edx,[umb_head]
        shl     edx,4
        cmp     eax,edx
        setae   dl
        movzx   edx,dl
uiu_out:
        or      edx,edx
        pop     edx
        ret

