;=====================================================================
; LOWMEM.INC - the conventional-memory arena
;
; A real DOS memory arena below 1MB: 16-byte memory control blocks
; chained head to tail, first fit, coalescing on free.  The kernel has
; had an arena of its own since the beginning (ARENA.INC, above 2MB,
; 32-bit, owner-id based) and this is deliberately NOT that.  It is
; the one DOS programs can see.  MEM walks it, some installers walk
; it, and a good deal of software decides how much memory it has by
; following the chain rather than by asking, so the chain has to be
; the genuine article - 'M' blocks ending in a 'Z', owner = PSP
; segment, length in paragraphs, not counting the header.
;
; Included by DOSMAIN.ASM.
;=====================================================================

;=====================================================================
; lm_init - one free block covering the whole transient program area.
;=====================================================================
lm_init:
        pushad
        mov     edi,V86_TPA
        mov     byte ptr [edi+RMCB_SIG],'Z'      ; the only block, so last
        mov     word ptr [edi+RMCB_OWN],0        ; and free
        mov     eax,V86_TPA_TOP - V86_TPA - RMCB_SIZE
        shr     eax,4
        mov     [edi+RMCB_LEN],ax
        xor     eax,eax
        mov     [edi+RMCB_NAME],eax
        mov     [edi+RMCB_NAME+4],eax
        popad
        ret

;=====================================================================
; lm_alloc - EBX = paragraphs wanted.
;            CF clear: AX = segment of the DATA (the paragraph after
;            the header), and the block owns itself.
;            CF set:   BX = the largest run available, as DOS does, so
;            the caller can report or retry.
;=====================================================================
lm_alloc:
        push    ecx
        push    edx
        push    esi
        push    edi
        xor     edi,edi                         ; largest free seen
        mov     esi,V86_TPA
la_scan:
        cmp     byte ptr [esi+RMCB_SIG],'M'
        je      la_live
        cmp     byte ptr [esi+RMCB_SIG],'Z'
        jne     la_none                         ; chain is broken - refuse
la_live:
        cmp     word ptr [esi+RMCB_OWN],0
        jne     la_next
        movzx   eax,word ptr [esi+RMCB_LEN]
        cmp     eax,edi
        jbe     la_nomax
        mov     edi,eax
la_nomax:
        cmp     eax,ebx
        jae     la_found
la_next:
        cmp     byte ptr [esi+RMCB_SIG],'Z'
        je      la_none
        movzx   eax,word ptr [esi+RMCB_LEN]
        inc     eax                             ; step over the header too
        shl     eax,4
        add     esi,eax
        jmp     la_scan

la_found:
        ; Split, unless the remainder is too small to be worth a header
        movzx   eax,word ptr [esi+RMCB_LEN]
        sub     eax,ebx
        cmp     eax,1
        jbe     la_whole
        mov     edx,ebx
        inc     edx
        shl     edx,4
        add     edx,esi                         ; EDX -> the new free block
        mov     cl,[esi+RMCB_SIG]
        mov     [edx+RMCB_SIG],cl                ; the tail inherits M or Z
        mov     word ptr [edx+RMCB_OWN],0
        dec     eax                             ; less its own header
        mov     [edx+RMCB_LEN],ax
        mov     byte ptr [esi+RMCB_SIG],'M'
        mov     [esi+RMCB_LEN],bx
la_whole:
        mov     eax,esi
        shr     eax,4
        inc     eax                             ; the data, one para on
        mov     [esi+RMCB_OWN],ax                ; a block owns itself
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        clc
        ret

la_none:
        mov     ebx,edi                         ; the best we could have done
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        stc
        ret

;=====================================================================
; lm_free - AX = the data segment of a block.  CF on a bad address,
;           which is how DOS answers 49h for a pointer it never gave
;           out and how a program with a wild pointer finds out.
;=====================================================================
lm_free:
        push    eax
        movzx   eax,ax
        dec     eax
        shl     eax,4
        cmp     eax,V86_TPA
        jb      lf_bad
        cmp     eax,V86_TPA_TOP
        jae     lf_bad
        cmp     byte ptr [eax+RMCB_SIG],'M'
        je      lf_ok
        cmp     byte ptr [eax+RMCB_SIG],'Z'
        jne     lf_bad
lf_ok:
        mov     word ptr [eax+RMCB_OWN],0
        call    lm_coalesce
        pop     eax
        clc
        ret
lf_bad:
        pop     eax
        stc
        ret

;=====================================================================
; lm_freeown - AX = a PSP segment; release everything it still holds.
;              A program that exits without freeing its own memory is
;              the normal case, not the exception.
;=====================================================================
lm_freeown:
        pushad
        mov     dx,ax
        mov     esi,V86_TPA
lo_scan:
        cmp     word ptr [esi+RMCB_OWN],dx
        jne     lo_next
        mov     word ptr [esi+RMCB_OWN],0
lo_next:
        cmp     byte ptr [esi+RMCB_SIG],'Z'
        je      lo_done
        movzx   eax,word ptr [esi+RMCB_LEN]
        inc     eax
        shl     eax,4
        add     esi,eax
        cmp     esi,V86_TPA_TOP
        jb      lo_scan
lo_done:
        call    lm_coalesce
        popad
        ret

;=====================================================================
; lm_resize - AX = data segment, EBX = paragraphs wanted.
;             CF set and BX = what it could have had, exactly as 4Ah
;             does, because "shrink me to my real size" is the first
;             thing a .COM does and "grow me" has to be able to fail.
;=====================================================================
lm_resize:
        push    ecx
        push    edx
        push    esi
        push    edi
        movzx   esi,ax
        dec     esi
        shl     esi,4
        cmp     esi,V86_TPA
        jb      lr_bad
        cmp     esi,V86_TPA_TOP
        jae     lr_bad

        movzx   ecx,word ptr [esi+RMCB_LEN]      ; what we have now
        cmp     ebx,ecx
        jbe     lr_shrink

        ; Growing: only into a free block immediately behind us
        cmp     byte ptr [esi+RMCB_SIG],'Z'
        je      lr_toobig
        mov     eax,ecx
        inc     eax
        shl     eax,4
        lea     edi,[esi+eax]
        cmp     word ptr [edi+RMCB_OWN],0
        jne     lr_toobig
        movzx   edx,word ptr [edi+RMCB_LEN]
        add     edx,ecx
        inc     edx                             ; the header we would absorb
        cmp     ebx,edx
        ja      lr_toobig
        mov     dl,[edi+RMCB_SIG]                ; swallow it whole...
        mov     [esi+RMCB_SIG],dl
        movzx   eax,word ptr [edi+RMCB_LEN]
        add     eax,ecx
        inc     eax
        mov     [esi+RMCB_LEN],ax
        mov     ecx,eax                         ; ...then fall into the split

lr_shrink:
        mov     [esi+RMCB_LEN],bx
        mov     eax,ecx
        sub     eax,ebx
        cmp     eax,1
        jbe     lr_ok                           ; no room for a new header
        mov     edx,ebx
        inc     edx
        shl     edx,4
        add     edx,esi
        mov     cl,[esi+RMCB_SIG]
        mov     [edx+RMCB_SIG],cl
        mov     word ptr [edx+RMCB_OWN],0
        dec     eax
        mov     [edx+RMCB_LEN],ax
        mov     byte ptr [esi+RMCB_SIG],'M'
        call    lm_coalesce
lr_ok:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        clc
        ret

lr_toobig:
        movzx   ebx,word ptr [esi+RMCB_LEN]      ; the most it can be
        cmp     byte ptr [esi+RMCB_SIG],'Z'
        je      lr_bad2
        mov     eax,ebx
        inc     eax
        shl     eax,4
        lea     edi,[esi+eax]
        cmp     word ptr [edi+RMCB_OWN],0
        jne     lr_bad2
        movzx   eax,word ptr [edi+RMCB_LEN]
        add     ebx,eax
        inc     ebx
lr_bad2:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        stc
        ret
lr_bad:
        xor     ebx,ebx
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        stc
        ret

;=====================================================================
; lm_coalesce - merge every run of adjacent free blocks.  Called after
;               a free, because two free blocks side by side are one
;               free block that nobody can allocate.
;=====================================================================
lm_coalesce:
        pushad
        mov     esi,V86_TPA
lc_scan:
        cmp     byte ptr [esi+RMCB_SIG],'Z'
        je      lc_done
        cmp     word ptr [esi+RMCB_OWN],0
        jne     lc_next
        movzx   eax,word ptr [esi+RMCB_LEN]
        inc     eax
        shl     eax,4
        lea     edi,[esi+eax]
        cmp     edi,V86_TPA_TOP
        jae     lc_done
        cmp     word ptr [edi+RMCB_OWN],0
        jne     lc_next
        movzx   eax,word ptr [esi+RMCB_LEN]
        movzx   edx,word ptr [edi+RMCB_LEN]
        add     eax,edx
        inc     eax                             ; plus the header we absorbed
        mov     [esi+RMCB_LEN],ax
        mov     dl,[edi+RMCB_SIG]
        mov     [esi+RMCB_SIG],dl                ; inherit Z from the last one
        jmp     lc_scan                         ; and try this block again
lc_next:
        movzx   eax,word ptr [esi+RMCB_LEN]
        inc     eax
        shl     eax,4
        add     esi,eax
        cmp     esi,V86_TPA_TOP
        jb      lc_scan
lc_done:
        popad
        ret
