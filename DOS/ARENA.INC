;=====================================================================
; ARENA.INC - the DOS memory arena
;
; A first-fit heap running from ARENA_BASE to the top of RAM that the
; BIOS reported.  Blocks carry a 16-byte header, the flat-mode moral
; equivalent of the DOS MCB paragraph:
;
;   +0  dd  signature: 'M' in-use, 'Z' free, 'E' end-of-arena
;   +4  dd  payload size in bytes (16-byte granular)
;   +8  dd  owner: the [cur_own] of whoever allocated it, 0 = kernel
;   +12 dd  reserved
;
; The owner is what makes 4Ch able to reclaim memory a program never
; freed itself.  It is an OWNER ID, not a PSP address, because nested
; programs share one slot and therefore one PSP address - see the note
; on cur_own in INT21.INC.
;
; Included by DOSMAIN.ASM.
;=====================================================================

MCB_SIG         EQU     0
MCB_SIZE        EQU     4
MCB_OWNER       EQU     8
MCB_LEN         EQU     16

SIG_USED        EQU     'M'
SIG_FREE        EQU     'Z'
SIG_END         EQU     'E'

;=====================================================================
; arena_init - one giant free block, capped by GV_MEMTOP.
;=====================================================================
arena_init:
        pushad
        mov     eax,dword ptr ds:[GV_MEMTOP]
        and     eax,NOT 15
        mov     [arena_top],eax

        ; CAP IT AT WHAT THE PAGE TABLES REACH.  The arena is the only
        ; thing that hands out addresses this high, and an address
        ; nothing maps is a page fault taken somewhere unrelated with
        ; the reason long gone.  A machine with more memory than
        ; PG_MAXLIN simply does not get to use the rest.
        cmp     eax,PG_MAXLIN
        jbe     ai_mapped
        mov     eax,PG_MAXLIN
        mov     [arena_top],eax
ai_mapped:

        ; guard: if the BIOS reported nonsense, take 1M of arena and hope
        cmp     eax,ARENA_BASE + 10000h
        jae     ai_sane
        mov     eax,ARENA_BASE + 100000h
        mov     [arena_top],eax
ai_sane:
        mov     ebx,ARENA_BASE
        mov     dword ptr [ebx+MCB_SIG],SIG_FREE
        sub     eax,ARENA_BASE + MCB_LEN + MCB_LEN
        mov     [ebx+MCB_SIZE],eax
        mov     dword ptr [ebx+MCB_OWNER],0

        ; terminating header at the very top
        mov     ebx,[arena_top]
        sub     ebx,MCB_LEN
        mov     dword ptr [ebx+MCB_SIG],SIG_END
        mov     dword ptr [ebx+MCB_SIZE],0
        popad
        ret

;=====================================================================
; arena_alloc - EBX = bytes wanted.
;               CF clear: EAX = payload address.
;               CF set:   EAX = largest free payload available.
;
; Which fitting block wins is [alloc_strat], set through 21h/58h:
;
;   0  first fit - the lowest block big enough.  Stops at the first
;                  candidate, so it is also the cheapest.
;   1  best fit  - the SMALLEST block big enough, which leaves the big
;                  ones whole at the cost of walking the whole arena.
;   2  last fit  - the highest block big enough.
;
; DOS offered exactly these three and PM-DOS honours them for real
; rather than storing the setting and ignoring it, because a program
; that asks for best fit is usually about to allocate a great many
; small blocks and has a reason.
;=====================================================================
arena_alloc:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        add     ebx,15                  ; round the request up
        and     ebx,NOT 15
        or      ebx,ebx
        jnz     aa_sized
        mov     ebx,16                  ; zero-byte requests get a sliver
aa_sized:
        xor     edx,edx                 ; EDX = largest free seen
        xor     edi,edi                 ; EDI = chosen block, 0 = none yet
        mov     esi,ARENA_BASE

aa_walk:
        mov     eax,[esi+MCB_SIG]
        cmp     eax,SIG_END
        je      aa_chosen
        cmp     eax,SIG_FREE
        jne     aa_next

        call    aa_coalesce             ; merge the free run at ESI

        mov     ecx,[esi+MCB_SIZE]
        cmp     ecx,edx
        jbe     aa_szok
        mov     edx,ecx
aa_szok:
        cmp     ecx,ebx
        jb      aa_next                 ; too small to be a candidate

        or      edi,edi
        jz      aa_pick                 ; nothing to beat yet
        cmp     dword ptr [alloc_strat],2
        je      aa_pick                 ; last fit: the higher one wins
        cmp     ecx,[edi+MCB_SIZE]      ; best fit: only if tighter
        jb      aa_pick
        jmp     short aa_next
aa_pick:
        mov     edi,esi
        cmp     dword ptr [alloc_strat],0
        je      aa_chosen               ; first fit: stop looking
aa_next:
        add     esi,[esi+MCB_SIZE]
        add     esi,MCB_LEN
        cmp     esi,[arena_top]         ; see the note on arena_freeown:
        jae     aa_chosen               ; a chain walk ends AT THE TOP as
        jmp     aa_walk                 ; well as at the end marker

aa_chosen:
        or      edi,edi
        jz      aa_nomem
        mov     esi,edi

;--- carve the block at ESI ------------------------------------------
        mov     ecx,[esi+MCB_SIZE]
        sub     ecx,ebx
        cmp     ecx,MCB_LEN + 16        ; enough left to be worth splitting?
        jb      aa_whole

        ; split: new free header right after our payload
        lea     eax,[esi+MCB_LEN+ebx]
        mov     dword ptr [eax+MCB_SIG],SIG_FREE
        sub     ecx,MCB_LEN
        mov     [eax+MCB_SIZE],ecx
        mov     dword ptr [eax+MCB_OWNER],0
        mov     [esi+MCB_SIZE],ebx
aa_whole:
        mov     dword ptr [esi+MCB_SIG],SIG_USED
        mov     eax,dword ptr [cur_own]         ; who to reclaim it from
        mov     [esi+MCB_OWNER],eax
        lea     eax,[esi+MCB_LEN]
        clc
        jmp     short aa_out

aa_nomem:
        mov     eax,edx
        stc
aa_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; aa_coalesce - absorb every free block that directly follows ESI.
;---------------------------------------------------------------------
aa_coalesce:
        push    eax
        push    ecx
ac_lp:
        mov     eax,esi
        add     eax,[esi+MCB_SIZE]
        add     eax,MCB_LEN             ; EAX = next header
        cmp     eax,[arena_top]         ; bounded, as every walk is
        jae     ac_done
        cmp     dword ptr [eax+MCB_SIG],SIG_FREE
        jne     ac_done
        mov     ecx,[eax+MCB_SIZE]
        add     ecx,MCB_LEN
        add     [esi+MCB_SIZE],ecx
        jmp     short ac_lp
ac_done:
        pop     ecx
        pop     eax
        ret

;=====================================================================
; arena_free - EDX = payload address from arena_alloc.
;              CF set if it does not look like one of our blocks.
;=====================================================================
arena_free:
        push    esi
        lea     esi,[edx-MCB_LEN]
        cmp     dword ptr [esi+MCB_SIG],SIG_USED
        jne     af_bad
        mov     dword ptr [esi+MCB_SIG],SIG_FREE
        mov     dword ptr [esi+MCB_OWNER],0
        pop     esi
        clc
        ret
af_bad:
        pop     esi
        stc
        ret

;=====================================================================
; arena_freeown - EAX = an owner id; free every block it still holds.
;
; This is what makes termination clean up after a program that
; allocated and never freed - the memory half of the handle sweep 4Ch
; has always done.  Blocks owned by 0 (the kernel's own) are never
; touched.  Preserves all registers.
;=====================================================================
arena_freeown:
        pushad
        or      eax,eax
        jz      afo_out                 ; owner 0 is the kernel's
        mov     esi,ARENA_BASE
afo_lp:
        mov     ebx,[esi+MCB_SIG]
        cmp     ebx,SIG_END
        je      afo_out
        cmp     ebx,SIG_USED
        jne     afo_next
        cmp     [esi+MCB_OWNER],eax
        jne     afo_next
        mov     dword ptr [esi+MCB_SIG],SIG_FREE
        mov     dword ptr [esi+MCB_OWNER],0
afo_next:
        ; EVERY WALK OF THIS CHAIN STOPS AT THE TOP, not only at the
        ; end marker.  The chain is data, a wild program can damage it,
        ; and a walk with no bound then steps through memory sixteen
        ; bytes at a time for as long as it takes to hit something.
        ; Before paging that was an unnoticed hang; with paging it is a
        ; page fault in the kernel, which is how the bug above - the
        ; end marker being written past the end of RAM - was finally
        ; caught rather than merely survived.
        add     esi,[esi+MCB_SIZE]
        add     esi,MCB_LEN
        cmp     esi,[arena_top]
        jae     afo_out
        jmp     short afo_lp
afo_out:
        popad
        ret

;=====================================================================
; arena_resize - EDX = payload, EBX = new size.
;                Shrinks in place, or grows if free space follows.
;                CF set (and EAX = max possible) if it cannot.
;=====================================================================
arena_resize:
        push    esi
        push    ecx
        push    ebx
        push    edx

        add     ebx,15
        and     ebx,NOT 15
        lea     esi,[edx-MCB_LEN]
        cmp     dword ptr [esi+MCB_SIG],SIG_USED
        jne     ar_bad

        ; Pull in any free neighbours, then compare.  Note what this
        ; does to a request that then turns out to be too big: the
        ; block has ALREADY absorbed the free space next door, so a
        ; failed grow must hand it back (ar_toobig) or every refused
        ; 4Ah would quietly eat a piece of the arena.
        mov     edx,[esi+MCB_SIZE]      ; what it was before coalescing
        push    dword ptr [esi+MCB_SIG]
        mov     dword ptr [esi+MCB_SIG],SIG_FREE
        call    aa_coalesce
        pop     dword ptr [esi+MCB_SIG]

        mov     ecx,[esi+MCB_SIZE]
        cmp     ecx,ebx
        jb      ar_toobig

        call    ar_giveback             ; the tail we no longer need
        pop     edx
        pop     ebx
        pop     ecx
        pop     esi
        clc
        ret

ar_toobig:
        mov     eax,ecx                 ; the best we could have offered
        mov     ebx,edx                 ; shrink back to the original size,
        call    ar_giveback             ; releasing what coalescing took
ar_bad:
        pop     edx
        pop     ebx
        pop     ecx
        pop     esi
        stc
        ret

;---------------------------------------------------------------------
; ar_giveback - ESI = header, EBX = size to keep.  Splits the surplus
;               off as a free block.  Preserves EAX, which is the
;               answer the caller is about to return.
;---------------------------------------------------------------------
ar_giveback:
        push    eax
        push    ecx
        mov     ecx,[esi+MCB_SIZE]
        sub     ecx,ebx
        cmp     ecx,MCB_LEN + 16
        jb      arg_out                 ; nothing worth splitting off
        lea     eax,[esi+MCB_LEN+ebx]
        mov     dword ptr [eax+MCB_SIG],SIG_FREE
        sub     ecx,MCB_LEN
        mov     [eax+MCB_SIZE],ecx
        mov     dword ptr [eax+MCB_OWNER],0
        mov     [esi+MCB_SIZE],ebx
arg_out:
        pop     ecx
        pop     eax
        ret

;=====================================================================
; arena_avail - EAX = largest single free block in the arena.
;=====================================================================
arena_avail:
        push    ebx
        mov     ebx,7FFFFFFFh           ; impossible request
        call    arena_alloc             ; must fail, returns the max
        jnc     av_oops                 ; (someone has 2G of RAM?)
        pop     ebx
        ret
av_oops:
        mov     edx,eax                 ; give it straight back
        call    arena_free
        mov     eax,7FFFFFFFh
        pop     ebx
        ret

arena_top   dd  0

; 21h/58h's allocation strategy: 0 first fit, 1 best fit, 2 last fit.
; Global, as it was in DOS - but reset to first fit whenever the shell
; restarts, so a program that sets it and dies cannot leave the whole
; system on a strategy nobody asked for.
alloc_strat dd  0
