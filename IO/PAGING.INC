;=====================================================================
; PAGING.INC - the page tables, and the A20 gate they exist to fake
;
; PM-DOS ran flat and unpaged from the first day, and the flatness is
; worth keeping: every address in PMDOS.INC means what it says, a
; linear address IS a physical one, and a bus-master device can be
; handed our own pointers.  So the map built here is the IDENTITY,
; everywhere, and turning paging on changes nothing that anything can
; observe.
;
; Except for sixteen entries.  THE POINT OF THIS FILE IS THE A20 GATE.
; (And, since v0.38, for whatever upper memory has been backed with:
; see pg_map at the foot of this file.  Both exceptions are below 1MB
; and neither is ever put on the bus.)
;
; On an 8086 there were twenty address lines, so FFFF:0010 came out at
; 00000h rather than 100000h, and a great deal of software written
; before 1985 depends on that wrap - it is how a program tests for
; extended memory, and it is what the A20 gate was invented to switch
; off.  A DOS box has to have it, and it cannot be faked by trapping:
; there is no way to intercept an ordinary MOV.
;
; With paging there is nothing to intercept.  A 16-bit segment:offset
; reaches 10FFEFh at the very most, so the whole of the wrap lives in
; the sixteen pages from 1MB, and switching A20 is switching sixteen
; page table entries between "point at yourself" and "point at zero".
;
; This is also why PMDOS.SYS was moved off 1MB (see PMDOS.INC): with
; the kernel there, aliasing those pages would have unmapped the
; kernel from underneath the code doing the aliasing.
;
; Included by IOPM.ASM.
;=====================================================================

;=====================================================================
; pg_init - build the identity map and switch paging on.
;
; Called from pm_entry once the console exists, so that a failure has
; somewhere to be reported, and before v86_init because a DOS box must
; never run with the wrap unemulated.
;=====================================================================
pg_init:
        pushad
        mov     dword ptr ds:[GV_PGDIR],0       ; nothing is paging yet, and
                                                ; pg_a20 below asks

;--- the page directory: one entry per table, all present ------------
        mov     edi,PG_DIR
        mov     eax,PG_TABS + PG_PRESENT
        mov     ecx,PG_MAXTAB
pgi_dir:
        stosd
        add     eax,1000h
        loop    pgi_dir
        mov     ecx,1024 - PG_MAXTAB            ; the rest of the 4G is not
        xor     eax,eax                         ; there and says so
        rep     stosd

;--- and the tables themselves, identity throughout ------------------
        mov     edi,PG_TABS
        mov     eax,PG_PRESENT                  ; physical 0, present, writable
        mov     ecx,PG_MAXTAB * 1024
pgi_tab:
        stosd
        add     eax,1000h
        loop    pgi_tab

;--- the guard page at the foot of each program slot -----------------
; THE NULL-POINTER GUARD THE ROADMAP WANTED, and it is two cleared
; bits.  A native program runs at ring 3 out of a segment based at its
; slot, so ITS address zero is this page and nothing else in the
; machine is - unlike the machine's page zero, which holds the IVT, the
; BIOS data area, InDOS at 4F7h and the real-mode frame at 500h, and
; which NULLTRAP.INC argues at length cannot be taken away.
;
; Both slots get one because both hold a program: the interpreter is
; not a special case, it is the program that happens to be running when
; no other one is.
        mov     eax,SHELL_SLOT + SLOT_GUARD
        call    pg_absent
        mov     eax,PROG_SLOT + SLOT_GUARD
        call    pg_absent

;--- A20 starts OFF, which is how a PC starts ------------------------
; The gate is a switch software throws deliberately; a DOS box that
; came up with it already open would hide every bug in every program
; that forgets to ask.
        xor     al,al
        call    pg_a20

;--- and go ----------------------------------------------------------
        mov     eax,PG_DIR
        mov     cr3,eax
        mov     dword ptr ds:[GV_PGDIR],eax     ; the thunk reads this
        mov     eax,cr0
        or      eax,80000000h
        mov     cr0,eax
        jmp     pgi_flush                       ; flush the prefetch queue -
pgi_flush:                                      ; the manual asks for it and
                                                ; the 386 means it
        popad
        ret

;=====================================================================
; pg_absent - EAX = a linear address.  Take the page containing it out
;             of the map, for good.
;
; Called before CR3 is loaded, so there is no TLB to flush: the two
; pages this is used on are decided at build time and never come back.
; The identity map means the entry to clear is simply the address
; itself, shifted - table (addr / 4MB), entry (addr / 4K) within it,
; which for one flat table array is one index.
;=====================================================================
pg_absent:
        push    eax
        push    edi
        shr     eax,12
        cmp     eax,PG_MAXTAB * 1024            ; past what we map at all?
        jae     pga_nothing                     ; then it is already absent
        mov     edi,PG_TABS
        mov     dword ptr [edi+eax*4],0
pga_nothing:
        pop     edi
        pop     eax
        ret

;=====================================================================
; pg_a20 - IOS_A20.  AL = 0 for the 8086 wrap, 1 for flat addressing.
;
; Sixteen entries, and a TLB flush.  The 386 has no INVLPG - that
; arrived with the 486 - so the flush is a reload of CR3, which is
; both correct and cheap enough for something a program does once.
;=====================================================================
pg_a20:
        pushad
        mov     byte ptr ds:[pg_a20st],al

        ; entry 256 of table 0 is linear 100000h: table 0 covers the
        ; first 4MB, and 100000h / 1000h is 256.
        ; The state is read back out of the byte just stored, because
        ; the obvious spelling - zero EAX and then test AL - destroys
        ; the argument before it is looked at, and the result is a gate
        ; that is always shut and looks like it works because shut is
        ; also the default.
        mov     edi,PG_TABS + 256 * 4
        xor     eax,eax                         ; A20 off: back to zero
        cmp     byte ptr ds:[pg_a20st],0
        je      pga_go
        mov     eax,HMA_BASE                    ; A20 on: at themselves
pga_go:
        or      eax,PG_PRESENT
        mov     ecx,HMA_PAGES
pga_one:
        stosd
        add     eax,1000h
        loop    pga_one

        mov     eax,dword ptr ds:[GV_PGDIR]     ; nothing to flush before
        or      eax,eax                         ; paging is even on
        jz      pga_done
        mov     cr3,eax
pga_done:
        popad
        ret

;=====================================================================
; pg_a20get - AL = the state of the gate as far as anyone can tell.
;=====================================================================
pg_a20get:
        mov     al,byte ptr ds:[pg_a20st]
        ret

pg_a20st        db      0
                db      0,0,0

;=====================================================================
; pg_map - IOS_PGMAP.  EAX = linear, EDX = physical, ECX = pages.
;          Point that many page table entries somewhere other than at
;          themselves, and flush.
;
; THIS IS THE SECOND AND LAST HOLE IN THE IDENTITY MAP, and the file
; header's promise is now worth restating rather than deleting: a
; linear address is a physical one EXCEPT for the sixteen pages of the
; A20 alias and whatever upper memory has been backed with.  Both
; exceptions are below 1MB and neither is ever handed to hardware.
;
; That last clause is load-bearing and is not an accident.  Anything
; that puts an address on the bus - a bus-master controller, the DMA
; page registers, a real-mode BIOS call made through rm_int with
; paging switched off - would be given a number that no longer means
; what it says.  Nothing here does: the ATA driver is polled PIO, the
; floppy path stages every transfer through XFER_BUF down in the first
; sixteen kilobytes, and rm_int passes registers and that same buffer.
; A driver loaded into upper memory that programs the DMA controller
; itself is the case this does not survive, and it is the same case
; EMM386 needed virtual DMA services for.
;
; Both addresses must be page aligned; the caller is the kernel and
; the kernel knows it.
;=====================================================================
pg_map:
        pushad
        or      ecx,ecx
        jz      pgm_done
        shr     eax,12                          ; linear page number = the
        cmp     eax,PG_MAXTAB * 1024            ; index, the map being flat
        jae     pgm_done
        mov     edi,PG_TABS
        lea     edi,[edi+eax*4]
        mov     eax,edx
        and     eax,NOT 0FFFh
        or      eax,PG_PRESENT
pgm_one:
        stosd
        add     eax,1000h
        loop    pgm_one

        mov     eax,dword ptr ds:[GV_PGDIR]     ; no TLB before paging is on
        or      eax,eax
        jz      pgm_done
        mov     cr3,eax
pgm_done:
        popad
        ret

;=====================================================================
; pg_umbprobe - IOS_UMBPROBE.  EDI -> UMB_SCAN_PAGES bytes, one per 4K
;               page of C0000h-EFFFFh, filled in with a UMBP_*.
;
; The question is NOT "is there memory here" but "does anything at all
; answer to this address", and they are different questions with
; different consequences.  Memory that answers is memory somebody may
; be using; an address nothing answers to is free by definition, and
; on a paging kernel it is also usable, which is the whole point.
;
; Two passes, and the order matters.
;
; FIRST THE ROMs, BY THEIR OWN DECLARATION.  An option ROM begins on a
; 2K boundary with 55 AA and a length in 512-byte blocks at offset 2,
; and that length is the only honest way to know where it ends - the
; tail of a ROM is very often FFh padding, which is indistinguishable
; from nothing at all by measurement.  A video BIOS that ends halfway
; through a page would otherwise donate the rest of that page to a
; program, and the program would write into the ROM's own scratch.
;
; THEN THE WRITE TEST, ON WHAT IS LEFT.  Write a pattern, read it
; back, put back what was there.  It comes back: something is storing
; it, so somebody may own it - UMBP_TAKEN, and a machine with shadow
; RAM already switched on over its ROMs loses that RAM, which is the
; safe direction to be wrong in.  It does not come back: nothing
; decodes the address and it is UMBP_EMPTY, and a page from the arena
; will be put underneath it.
;
; The pattern is written and read as a DWORD at two places in the
; page, at the front and near the end, because a decoder that answers
; for part of a page is a decoder that answers.
;
; What this cannot see through is a WRITE-BACK CACHE holding the
; pattern for a read that never reached the bus.  On the 386 there is
; none; on a 486 the chipset does not cache what it does not decode,
; which is the same fact this routine is measuring.  It is worth
; knowing that the failure mode is a page reported as RAM that is not
; - which loses memory rather than corrupting it.
;=====================================================================
pg_umbprobe:
        pushad
        push    edi

;--- everything is empty until something claims it --------------------
        mov     ecx,UMB_SCAN_PAGES
        mov     al,UMBP_EMPTY
        rep     stosb
        pop     edi

;--- pass one: option ROM headers, every 2K --------------------------
        mov     esi,UMB_SCAN_LO
pgu_rom:
        cmp     esi,UMB_SCAN_HI
        jae     pgu_write
        cmp     word ptr [esi],0AA55h
        jne     pgu_rnext
        movzx   ecx,byte ptr [esi+2]            ; length, 512-byte blocks
        or      ecx,ecx
        jz      pgu_rnext                       ; a header claiming nothing
        shl     ecx,9                           ; ...in bytes
        mov     eax,esi
        add     eax,ecx
        add     eax,0FFFh                       ; and round UP to whole pages:
        and     eax,NOT 0FFFh                   ; a ROM ending mid-page keeps
        mov     edx,esi                         ; the rest of that page
pgu_rmark:
        cmp     edx,UMB_SCAN_HI
        jae     pgu_rdone
        cmp     edx,eax
        jae     pgu_rdone
        mov     ebx,edx
        sub     ebx,UMB_SCAN_LO
        shr     ebx,12
        mov     byte ptr [edi+ebx],UMBP_TAKEN
        add     edx,1000h
        jmp     short pgu_rmark
pgu_rdone:
        mov     esi,eax                         ; carry on past it, not into
        jmp     short pgu_rom                   ; the middle of it
pgu_rnext:
        add     esi,800h
        jmp     short pgu_rom

;--- pass two: does anything store what we write? --------------------
pgu_write:
        xor     ebx,ebx                         ; page index
pgu_page:
        cmp     ebx,UMB_SCAN_PAGES
        jae     pgu_out
        cmp     byte ptr [edi+ebx],UMBP_EMPTY
        jne     pgu_pnext                       ; a ROM already spoke for it
        mov     esi,ebx
        shl     esi,12
        add     esi,UMB_SCAN_LO
        call    pgu_test                        ; front of the page
        jc      pgu_isram
        add     esi,0FF0h                       ; and the last dword of it
        call    pgu_test
        jnc     pgu_pnext
pgu_isram:
        mov     byte ptr [edi+ebx],UMBP_RAM
pgu_pnext:
        inc     ebx
        jmp     short pgu_page
pgu_out:
        popad
        ret

;---------------------------------------------------------------------
; pgu_test - ESI -> a dword.  CF set if writing to it stuck.  What was
;            there is put back either way, because a ROM that ignores
;            the write is not the only thing this runs over.
;---------------------------------------------------------------------
pgu_test:
        push    eax
        push    edx
        mov     edx,[esi]                       ; what is there now
        mov     eax,05AA5F00Fh
        mov     [esi],eax
        cmp     [esi],eax
        jne     pgu_tno
        not     eax                             ; and again, inverted, so that
        mov     [esi],eax                       ; an address that happens to
        cmp     [esi],eax                       ; hold the pattern already
        jne     pgu_tno                         ; cannot pass by standing still
        mov     [esi],edx
        pop     edx
        pop     eax
        stc
        ret
pgu_tno:
        mov     [esi],edx
        pop     edx
        pop     eax
        clc
        ret
