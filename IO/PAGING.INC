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
