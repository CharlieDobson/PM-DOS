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
; Where everything goes is the identity; WHO MAY WRITE is not.  The
; kernel's own pages are read-only to ring 3 - see pg_protect - and
; usr_wok is how the kernel asks the same question before it writes
; into memory a program named.
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

;--- and whatever the machine has past what those reach --------------
        mov     dword ptr ds:[GV_PGHILO],0      ; none, until pg_hiram says
        mov     dword ptr ds:[GV_PGHIHI],0
        call    pg_hiram

;--- the kernel's own pages, which ring 3 may read and not write -----
; Before the A20 alias below is built, because the alias copies these
; entries rather than making its own.
        call    pg_protect

;--- the guard page at the foot of each program slot -----------------
; THE NULL-POINTER GUARD THE ROADMAP WANTED, and it is two cleared
; bits.  A native program runs at ring 3 out of a segment based at its
; slot, so ITS address zero is this page and nothing else in the
; machine is - unlike the machine's page zero, which holds the IVT, the
; BIOS data area, InDOS at 4F7h and the real-mode frame at 500h, and
; which NULLTRAP.INC argues at length cannot be taken away.
;
; The interpreter's slot is the one that is fixed, so its guard is made
; here, once.  Every other program's slot is a block of the arena and
; gets its guard from slot_new, through pg_guard, wherever it lands.
; (The second fixed slot that used to be at PROG_SLOT is the page
; directory now - taking ITS first page away would unmap the map.)
        mov     eax,SHELL_SLOT + SLOT_GUARD
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
        call    pg_pseinit                      ; and whether device memory
                                                ; can be mapped 4MB at a time
        popad
        ret

;=====================================================================
; pg_hiram - map the memory above PG_MAXLIN, identity like the rest.
;            Called from pg_init with paging still off.
;
; THE 64 TABLES AT PG_TABS ARE A FIXED 256MB, and for a long time that
; was the ceiling on everything: arena_init stopped there, so a 1GB
; machine and a 3GB machine both had 256MB of arena.  The tables for
; the rest cannot go beside those - KBSS_BASE and ARENA_BASE follow
; PG_TOP and are numbers every file is assembled with, MEM.EXE
; included - and how many there are is not known until the BIOS has
; been asked.  So they go at the TOP of memory, one 4K table for every
; 4MB above PG_MAXLIN, and GV_MEMTOP comes down below them.  The
; arena then ends where it always has, at GV_MEMTOP, and never hands
; out a table.
;
; Ordinary 4K tables even where the processor has 4MB pages.  It costs
; a megabyte of tables for every gigabyte of memory, and buys an
; address space where any page can be taken out of the map - which the
; guard page at the foot of a program slot is, wherever that slot is.
;
; Each table maps itself, being identity like everything else here.
; The last one may run past the end of memory; those entries name
; addresses nothing answers to, exactly as the 64 below do on a
; machine with less than 256MB.
;=====================================================================
pg_hiram:
        mov     eax,dword ptr ds:[GV_MEMTOP]
        cmp     eax,PG_MAXLIN
        jbe     pgh_done                        ; the 64 already reach it
        mov     ecx,eax
        sub     ecx,PG_MAXLIN - 003FFFFFh
        shr     ecx,22                          ; ECX = tables, one per 4MB
        mov     edx,ecx
        shl     edx,12
        sub     eax,edx
        and     eax,NOT 0FFFh                   ; EAX = where the first goes
        mov     dword ptr ds:[GV_MEMTOP],eax    ; and memory ends below them
        mov     dword ptr ds:[GV_PGHILO],eax    ; which pg_protect and buf_ok
        add     edx,eax                         ; need to know
        mov     dword ptr ds:[GV_PGHIHI],edx

        mov     edi,eax                         ; the tables, one after another
        mov     esi,PG_DIR + PG_MAXTAB * 4      ; their directory entries
        mov     edx,PG_MAXLIN + PG_PRESENT      ; the first page they map
pgh_tab:
        lea     eax,[edi + PG_PRESENT]
        mov     [esi],eax
        add     esi,4
        push    ecx
        mov     eax,edx
        mov     ecx,1024
pgh_ent:
        stosd
        add     eax,1000h
        loop    pgh_ent
        mov     edx,eax
        pop     ecx
        loop    pgh_tab
pgh_done:
        ret

;=====================================================================
; pg_guard - IOS_PGPRES.  EAX = a linear address, DL = 0 to take the
;              page holding it out of the map, anything else to put it
;              back as itself.  CF if no 4K table covers it.
;
; THE GUARD PAGE OF A SLOT THAT MOVES.  A transient program's slot is a
; block of the arena now, somewhere new for every EXEC, so its first
; page is taken out of the map when the slot is made and put back when
; it is freed - before the arena can hand that memory to anybody else.
; pg_absent below does the same for the shell's slot, once, at boot.
;
; The directory is walked rather than PG_TABS indexed, because a slot
; above 256MB is in one of pg_hiram's tables at the top of memory.  A
; 4MB page has no entry to clear and is refused; the arena is never
; mapped with one.  One page and a reload of CR3: the 386 has no
; INVLPG.
;=====================================================================
pg_guard:
        push    eax
        push    ebx
        push    ecx
        mov     ecx,eax
        shr     ecx,22
        mov     ebx,dword ptr [PG_DIR + ecx*4]
        test    bl,1
        jz      pgr_no                          ; no table here at all
        test    bl,80h
        jnz     pgr_no                          ; a 4MB page: nothing to clear
        and     ebx,NOT 0FFFh                   ; EBX = the table
        and     eax,NOT 0FFFh                   ; EAX = the page
        mov     ecx,eax
        shr     ecx,12
        and     ecx,3FFh                        ; ECX = its entry
        or      dl,dl
        jz      pgr_out
        or      eax,PG_PRESENT                  ; present, as itself
        jmp     short pgr_put
pgr_out:
        xor     eax,eax                         ; absent
pgr_put:
        mov     dword ptr [ebx + ecx*4],eax
        mov     eax,cr3
        mov     cr3,eax
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
pgr_no:
        pop     ecx
        pop     ebx
        pop     eax
        stc
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
; pg_protect - take the write bit off every page the kernel lives in.
;              Called from pg_init with paging still off.
;
; RING 3 CAN NAME THE KERNEL, so the page has to be what refuses.  A
; DPMI client with a flat descriptor reaches all of memory, and so does
; a DOS box below 1MB, where the stub, the GDT and the globals CR3 is
; reloaded from all sit at 1000h.  With this bit clear a write from any
; of them is a page fault in that program, and the fault ends it the
; way any other does; reads still work, and ring 0 is not affected
; (see PG_WRITABLE).
;
; The four ranges are KPROT_* and the hiram tables, which pg_hiram has
; just placed.  Those may be above 256MB, so the directory is walked.
;=====================================================================
pg_protect:
        mov     eax,KPROT_LO1
        mov     edx,KPROT_HI1
        call    pgk_range
        mov     eax,KPROT_LO2
        mov     edx,KPROT_HI2
        call    pgk_range
        mov     eax,KPROT_LO3
        mov     edx,KPROT_HI3
        call    pgk_range
        mov     eax,dword ptr ds:[GV_PGHILO]
        mov     edx,dword ptr ds:[GV_PGHIHI]
        call    pgk_range                       ; empty if there are none
        ret

;--- one range.  EAX = its first byte, EDX = one past its last, both
;--- whole pages.  EBX and ECX trashed.
pgk_range:
        cmp     eax,edx
        jae     pgk_done
        mov     ecx,eax
        shr     ecx,22
        mov     ebx,dword ptr [PG_DIR + ecx*4]
        and     ebx,NOT 0FFFh                   ; EBX = the table
        mov     ecx,eax
        shr     ecx,12
        and     ecx,3FFh                        ; ECX = the entry
        and     dword ptr [ebx + ecx*4],NOT PG_WRITABLE
        add     eax,1000h
        jmp     short pgk_range
pgk_done:
        ret

;=====================================================================
; usr_wok - IOS_USRWOK.  EAX = a linear address, ECX = how many bytes.
;           CF unless ring 3 could have written every one of them
;           itself: each page present, user and writable, in the
;           directory and in its table.  Everything preserved.
;
; THE QUESTION THE KERNEL HAS TO ASK BEFORE IT WRITES FOR A PROGRAM.
; pg_protect stops the program's own stores; it cannot stop the
; kernel's, and the kernel stores into memory a program chose all the
; time - an interrupt frame on its stack, the words 0300h copies onto a
; real-mode stack.  A guest whose SS:SP is 0100:0C20 is asking for the
; GDT to be overwritten by the next INT it executes.  Asked here, the
; answer is the page table's, which already knows everything: the
; kernel's ranges, a slot's guard page, and the A20 alias of either.
;
; Empty is yes.  A range that wraps past 4GB is no.
;=====================================================================
usr_wok:
        push    eax
        push    ebx
        push    ecx
        push    edx
        or      ecx,ecx
        jz      uwk_yes
        lea     ecx,[eax + ecx - 1]             ; ECX = the last byte
        cmp     ecx,eax
        jb      uwk_no                          ; ...which is past 4GB
        and     eax,NOT 0FFFh
uwk_page:
        mov     edx,eax
        shr     edx,22
        mov     ebx,dword ptr [PG_DIR + edx*4]
        mov     edx,ebx
        and     edx,PG_PRESENT
        cmp     edx,PG_PRESENT
        jne     uwk_no                          ; the directory says no
        test    bl,80h
        jnz     uwk_next                        ; a 4MB page: it said yes
        and     ebx,NOT 0FFFh
        mov     edx,eax
        shr     edx,12
        and     edx,3FFh
        mov     ebx,dword ptr [ebx + edx*4]
        and     ebx,PG_PRESENT
        cmp     ebx,PG_PRESENT
        jne     uwk_no                          ; and so does the table
uwk_next:
        add     eax,1000h
        jc      uwk_yes                         ; the last page there is
        cmp     eax,ecx
        jbe     uwk_page
uwk_yes:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
uwk_no:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
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
        mov     ecx,HMA_PAGES
        cld
        cmp     byte ptr ds:[pg_a20st],0
        je      pga_wrap
        mov     eax,HMA_BASE OR PG_PRESENT      ; A20 on: at themselves
pga_one:
        stosd
        add     eax,1000h
        loop    pga_one
        jmp     short pga_flush

        ; A20 OFF: THE FIRST SIXTEEN PAGES AGAIN, ENTRY FOR ENTRY, and
        ; not "the same addresses, writable".  The stub, the GDT and the
        ; globals are on those pages, read-only to ring 3, and an alias
        ; that granted what the page itself refuses would put the GDT
        ; one FFFF:1C10 away from every DOS program in the machine.
pga_wrap:
        mov     esi,PG_TABS
        rep     movsd

pga_flush:
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

;=====================================================================
; pg_mapphys - IOS_MAPPHYS.  EBX = a physical address, ECX = how many
;              bytes from there -> EAX = where they can be reached.
;              CF if they cannot.  Everything else preserved.
;
; THE ANSWER IS THE QUESTION.  Device memory is mapped where it is, so
; the identity holds all the way up and a driver's pointer to its
; card's registers is still a number that could go on the bus.  What
; is being asked for is not an address but PERMISSION: until this has
; been called the directory entries past PG_MAXTAB are absent, and
; touching a frame buffer at E0000000h is a page fault.
;
; FOUR MEGABYTES AT A TIME, because that is what a directory entry
; covers and nothing finer is worth having: what is next to a card's
; registers is more of that card, or another one.  A processor that
; has 4MB pages is given one and the entry is the whole of the cost.
; One that has not takes a table of a thousand ordinary pages from
; the pool at PG_DEVTABS, and when the pool has run out the call
; fails - which a driver can do something about, unlike a fault.
;
; Memory below PG_MAXLIN is mapped already, and asking for it again
; succeeds without changing anything.  Nothing is ever unmapped: an
; address that was reachable a moment ago going absent is a fault in
; whoever else was using it, and there is no count of who is.
;=====================================================================
pg_mapphys:
        push    ebx
        push    ecx
        push    esi
        push    edi
        or      ecx,ecx
        jz      pgd_done
        lea     edi,[ebx+ecx-1]                 ; the last byte of it
        cmp     edi,ebx
        jb      pgd_no                          ; ...which is past 4GB
        mov     esi,ebx
        shr     esi,22
        shr     edi,22
pgd_one:
        test    dword ptr [PG_DIR + esi*4],1
        jnz     pgd_next
        call    pgd_add
        jc      pgd_no
pgd_next:
        inc     esi
        cmp     esi,edi
        jbe     pgd_one
        mov     eax,cr3                         ; the 386 has no INVLPG
        mov     cr3,eax
pgd_done:
        mov     eax,ebx
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        clc
        ret
pgd_no:
        xor     eax,eax
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; pgd_add - ESI = a directory entry that is absent: make it cover its
;           own four megabytes.  CF if there is nothing to make it of.
;---------------------------------------------------------------------
pgd_add:
        push    eax
        cmp     byte ptr ds:[pg_pse],0
        je      pgd_table
        mov     eax,esi
        shl     eax,22
        or      eax,PG_PRESENT OR 80h           ; ...and the page size bit
        mov     dword ptr [PG_DIR + esi*4],eax
        pop     eax
        clc
        ret
pgd_table:
        mov     eax,dword ptr ds:[pg_devused]
        cmp     eax,PG_DEVTAB
        jae     pgd_none
        inc     dword ptr ds:[pg_devused]
        shl     eax,12
        add     eax,PG_DEVTABS
        push    ecx
        push    edi
        pushfd
        cld
        mov     edi,eax
        push    eax
        mov     eax,esi
        shl     eax,22
        or      eax,PG_PRESENT
        mov     ecx,1024
pgd_fill:
        stosd
        add     eax,1000h
        loop    pgd_fill
        pop     eax                             ; the table, now that it
        or      eax,PG_PRESENT                  ; says something
        mov     dword ptr [PG_DIR + esi*4],eax
        popfd
        pop     edi
        pop     ecx
        pop     eax
        clc
        ret
pgd_none:
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; pg_pseinit - has this processor 4MB pages?  Called from pg_init.
;
; CPUID says, and whether there is a CPUID is said by bit 21 of the
; flags: a processor that lets it be changed has the instruction.  A
; 386 and most 486s do not, and they use the pool.  CR4 does not exist
; on those either, which is why nothing here touches it before the
; answer is known.  The three instructions are spelled in bytes
; because the file is assembled for a 386.
;---------------------------------------------------------------------
pg_pseinit:
        pushad
        pushfd
        pop     eax
        mov     ecx,eax
        xor     eax,00200000h
        push    eax
        popfd
        pushfd
        pop     eax
        push    ecx
        popfd
        xor     eax,ecx
        test    eax,00200000h
        jz      pgp_out
        xor     eax,eax
        db      0Fh,0A2h                        ; CPUID
        or      eax,eax
        jz      pgp_out
        mov     eax,1
        db      0Fh,0A2h
        test    edx,08h                         ; PSE
        jz      pgp_out
        db      0Fh,20h,0E0h                    ; MOV EAX,CR4
        or      eax,10h
        db      0Fh,22h,0E0h                    ; MOV CR4,EAX
        mov     byte ptr ds:[pg_pse],1
pgp_out:
        popad
        ret

pg_pse          db      0                       ; 4MB pages are there to use
                db      0,0,0
pg_devused      dd      0                       ; tables taken from the pool
