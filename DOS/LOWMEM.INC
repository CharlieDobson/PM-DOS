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
;
; AND THE TOP OF IT IS THE BIOS'S TO SAY.  This used to end the arena
; at a hardcoded 640K, which is right on a 1985 machine and wrong on
; nearly everything since: a BIOS with an EXTENDED BIOS DATA AREA
; takes the top kilobyte or two of conventional memory for itself,
; puts what is left in the word at 40:13, and expects DOS to believe
; it.  The 486 and the 86Box machine here both report 639.
;
; So the last kilobyte of the arena was memory the BIOS owned.  A
; program that got that far up would overwrite the EBDA - where the
; mouse driver's state and the PS/2 handler's stack live - and the
; damage would appear somewhere else entirely, later.  Nothing here
; had ever allocated that high, which is the only reason it had not
; been seen: it wants a program asking for 600K and getting it.
;
; MS-DOS 6.22's MEM.EXE sees it immediately, and its complaint is
; exact even if it reads as nonsense: it finds an MCB chain that ends
; ABOVE the top of conventional memory, decides a chain like that is
; not the conventional one, and reports the whole arena as UPPER
; memory with conventional entirely used.  Two regions where there is
; one - which is what v0.35 left unfinished.
;=====================================================================
lm_init:
        pushad
        movzx   eax,word ptr ds:[BDA_MEMSIZE]
        shl     eax,10                          ; KB the BIOS admits to
        cmp     eax,V86_TPA_TOP
        jbe     li_notmore
        mov     eax,V86_TPA_TOP                 ; more than 640K of it is not
li_notmore:                                     ; conventional memory
        cmp     eax,V86_TPA + 10000h
        jae     li_sane
        mov     eax,V86_TPA_TOP                 ; a BIOS that says something
li_sane:                                        ; useless is not believed
        and     eax,NOT 15
        mov     [lm_top],eax

        mov     edi,V86_TPA
        mov     byte ptr [edi+RMCB_SIG],'Z'      ; the only block, so last
        mov     word ptr [edi+RMCB_OWN],0        ; and free
        sub     eax,V86_TPA + RMCB_SIZE
        shr     eax,4
        mov     [edi+RMCB_LEN],ax
        xor     eax,eax
        mov     [edi+RMCB_NAME],eax
        mov     [edi+RMCB_NAME+4],eax
        popad
        ret

lm_top      dd  V86_TPA_TOP             ; where the arena really ends

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
        ; ECX, NOT EAX: AX IS THE ANSWER BY THIS POINT.  The first
        ; version of these three lines zeroed EAX, which is the segment
        ; lm_alloc is about to return - so every allocation in the
        ; machine came back as segment zero and the first write through
        ; it faulted at FFFFFFF0.  Same shape as the 3Ch that returned
        ; a timestamp for a handle: the return value is set early and
        ; the register looks free.
        xor     ecx,ecx                         ; ...and starts NAMELESS.  The
        mov     [esi+RMCB_NAME],ecx             ; eight bytes at +8 are a field
        mov     [esi+RMCB_NAME+4],ecx           ; DOS 4 and later fill in with
                                                ; the program's name and every
                                                ; memory reporter DISPLAYS, so
                                                ; leaving whatever the last
                                                ; occupant left there publishes
                                                ; a dead program's name as a
                                                ; live one.  lm_setname puts
                                                ; the real one in.
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
; lm_sysalloc - EBX = paragraphs, for memory belonging to the MACHINE
;               rather than to any program.  AX = the payload segment.
;
; Owner 8 is how DOS says "this is the system's", and PM-DOS has said
; it since the XMS callback paragraph - but owner 8 is not only a
; label.  It is a PROMISE ABOUT THE CONTENTS, and PM-DOS was breaking
; it in a way that cost MS-DOS 6.22's MEM.EXE the whole machine.
;
; A DOS-owned block is not opaque.  MEM, MSD and every other memory
; reporter walk INSIDE it, because that is where MS-DOS keeps the
; things CONFIG.SYS asked for - the device drivers, the buffers, the
; FILES and FCBS and STACKS tables - each behind a sixteen-byte header
; laid out exactly like an MCB, with a letter in place of the 'M'.
; The walk is the obvious one: read the letter, add the length at
; offset 3, step on, stop at the end of the parent block.
;
; PM-DOS's system blocks had no such headers.  Their first bytes were
; simply the structure they hold - and the list of lists begins with
; the first-MCB word and then FFFF:FFFF, the "there is no drive
; parameter block" sentinel.  So MEM read a length of FFFFh, added one
; to step over the header, and got ZERO.  It never moved again: a
; guest going round a loop makes no DOS call, causes no fault, and
; says nothing, so from outside it is a machine that has stopped.
;
; The fix is not to hide the block but to describe it.  One paragraph
; in front of every system allocation, holding a header of the shape
; the walk expects and a length that tiles the block exactly, so the
; walk lands on the end and stops. The letter is deliberately one DOS
; does not define: PM-DOS's blocks are not buffers or FCBS or stacks,
; and a walker that knows the letters skips what it does not know.
; Claiming 'D' to be counted would be inventing a device driver, which
; is the same lie the list of lists refuses to tell with its pointers.
;=====================================================================
SUBBLK_TAG      EQU     'P'

lm_sysalloc:
        push    ecx
        push    edx
        inc     ebx                             ; ...and its header
        call    lm_alloc
        jc      lsa_out                          ; EBX = the best we could do,
                                                ; as lm_alloc leaves it
        movzx   edx,ax                          ; the MCB is the system's
        dec     edx
        shl     edx,4
        mov     word ptr [edx+RMCB_OWN],8

        movzx   edx,ax                          ; and the first paragraph
        shl     edx,4                           ; describes the rest
        dec     ebx
        mov     byte ptr [edx+RMCB_SIG],SUBBLK_TAG
        mov     word ptr [edx+RMCB_OWN],8
        mov     [edx+RMCB_LEN],bx
        xor     ecx,ecx
        mov     [edx+5],cx
        mov     byte ptr [edx+7],cl
        mov     [edx+RMCB_NAME],ecx
        mov     [edx+RMCB_NAME+4],ecx

        inc     ax                              ; the payload is one on
        clc
lsa_out:
        pop     edx
        pop     ecx
        ret

;=====================================================================
; lm_setname - AX = a block's data segment, ESI -> a path.  Writes the
;              base name of the path into the block's eight-byte name
;              field, upper-cased, blank-padded, extension dropped -
;              which is what DOS 4 and later put there and what MEM,
;              MSD and every other memory reporter print in the "Name"
;              column of their listings.
;
; PM-DOS never wrote it, so those listings named whichever program had
; last owned that memory.  The name is not load-bearing to the arena;
; it is load-bearing to everything that DESCRIBES the arena, which is
; the same argument as the sub-block headers and UMB_HEAD before it.
;=====================================================================
lm_setname:
        pushad
        movzx   edi,ax
        dec     edi
        shl     edi,4
        add     edi,RMCB_NAME

        mov     edx,esi                         ; find the last separator
ls_scan:
        mov     al,[esi]
        or      al,al
        jz      ls_base
        inc     esi
        cmp     al,'\'
        je      ls_mark
        cmp     al,'/'
        je      ls_mark
        cmp     al,':'
        jne     ls_scan
ls_mark:
        mov     edx,esi                         ; the name starts after it
        jmp     short ls_scan
ls_base:
        mov     esi,edx
        mov     ecx,8
ls_copy:
        mov     al,[esi]
        or      al,al
        jz      ls_pad
        cmp     al,'.'                          ; the extension is not part
        je      ls_pad                          ; of the name DOS stores
        inc     esi
        call    cfg_upcase
        mov     [edi],al
        inc     edi
        dec     ecx
        jnz     ls_copy
        jmp     short ls_done
ls_pad:
        mov     al,' '
        rep     stosb
ls_done:
        popad
        ret

;=====================================================================
; lm_setown - AX = the data segment of a block, DX = the PSP it now
;             belongs to.  CF on a bad address, as lm_free.
;
; WHOSE A BLOCK IS, IS NOT WHERE IT IS, and lm_alloc cannot know the
; difference.  It leaves every block owning ITSELF, which is exactly
; right for the one case it was written for - a program's own block,
; where the PSP sits in the first paragraph, so "owner = block + 1" IS
; the PSP - and that made a consequence look like the rule.  For
; memory a program merely ASKS for it is wrong, and wrong in the
; direction that does not announce itself: lm_freeown matches on the
; owner word, a self-owned block never matches the PSP that is
; terminating, and the block stands there for the rest of the session
; with nobody left who could free it.
;
; The environment block has been corrected this way since the loader
; was written - vdm_psp hands it to the PSP the moment there is one -
; and that correction being INLINE and unnamed is why the identical
; one was never made for 21h/48h.  It is a routine now, so there is
; one place that says what owning means.
;=====================================================================
lm_setown:
        push    eax
        movzx   eax,ax
        dec     eax
        shl     eax,4
        cmp     eax,V86_TPA
        jb      ls_bad
        cmp     eax,[lm_top]
        jae     ls_bad
        cmp     byte ptr [eax+RMCB_SIG],'M'
        je      ls_own
        cmp     byte ptr [eax+RMCB_SIG],'Z'
        jne     ls_bad
ls_own:
        mov     word ptr [eax+RMCB_OWN],dx
        pop     eax
        clc
        ret
ls_bad:
        pop     eax
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
        cmp     eax,[lm_top]
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
        cmp     esi,[lm_top]
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
        cmp     esi,[lm_top]
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
        ; ONE LEFT-OVER PARAGRAPH IS A HEADER, NOT A GAP.  This used to
        ; refuse to split when the remainder was a single paragraph -
        ; but the length above has ALREADY been shortened by then, so
        ; the paragraph was not kept, it was ORPHANED: the block said it
        ; ended one paragraph before the next header really began, and a
        ; walker stepping "this block plus its header" landed on the
        ; last sixteen bytes of somebody's data and read them as a
        ; signature and a length.  From there it goes wherever those
        ; bytes point, for ever, without making a single DOS call - and
        ; that is exactly what MS-DOS 6.22's MEM.EXE did on the 486.
        ;
        ; A zero-length block is a legal MCB and real DOS makes them all
        ; the time: the header IS the paragraph, and it describes no
        ; data at all.  The only case with nothing to write is an exact
        ; fit, and that one really does leave the chain alone.
        or      eax,eax
        jz      lr_ok
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
        cmp     edi,[lm_top]
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
        cmp     esi,[lm_top]
        jb      lc_scan
lc_done:
        popad
        ret
