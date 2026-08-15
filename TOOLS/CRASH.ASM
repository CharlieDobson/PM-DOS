;=====================================================================
; CRASH.ASM - CRASH.EXE, a native 32-bit program that faults on purpose
;
; Before v0.30 this file could not usefully exist.  A D32 program runs
; in the kernel's own code segment at ring 0, so a bug in one and a
; bug in the kernel arrive at the same exception handler - and that
; handler printed the fault and HLTed.  One wild pointer in one
; utility ended the machine.
;
; v0.30 gave the DOS side a hook there (IOS_EXCHOOK -> dos_fault), so
; the fault is reported and then unwound to a fresh command prompt.
; This is what proves it: it crashes, and the prompt comes back.
;
; AND THEN THE PREMISE CHANGED.  Native programs run at ring 3 in a
; segment that describes their own slot now, so none of these faults
; can reach anything outside this program any more - which is what the
; new /K tests, and what turns every other case here from "the machine
; survived" into "the machine was never in danger".
;
;   CRASH          a general protection fault - a write outside the
;                  slot, which used to be a page fault at F0000000
;   CRASH /D       divide by zero
;   CRASH /I       an invalid opcode
;   CRASH /N       a write through a null pointer
;   CRASH /K       a READ of the kernel, which is the one that stopped
;                  being possible
;   CRASH /A       crash twice in a row, to show the second one is
;                  caught as well as the first
;
; Nothing here is subtle on purpose.  The value of the test is not the
; kind of fault, it is that the shell is still there afterwards.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_CR     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_CR, DS:_CR, ES:_CR, SS:_CR
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET cr_entry>,<0>,4096

cr_entry:
        mov     edx,OFFSET msg_hi
        call    puts

        ; The command tail lives at PSP+80h: a length byte, then the
        ; text.  All that is wanted from it is a letter after a slash.
        mov     esi,SLOT_PSP + 81h
        movzx   ecx,byte ptr ds:[SLOT_PSP + 80h]
        xor     bl,bl                   ; BL = the switch letter, if any
cr_tail:
        or      ecx,ecx
        jz      cr_go
        mov     al,[esi]
        cmp     al,'/'
        jne     cr_next
        mov     bl,[esi+1]
        or      bl,20h                  ; lower case, so /D and /d agree
cr_next:
        inc     esi
        dec     ecx
        jmp     short cr_tail

cr_go:
        cmp     bl,'d'
        je      cr_div
        cmp     bl,'i'
        je      cr_bad
        cmp     bl,'a'
        je      cr_again
        cmp     bl,'n'
        je      cr_null
        cmp     bl,'k'
        je      cr_kernel

;--- a write miles outside the slot ----------------------------------
; This used to be a PAGE fault: a D32 program was flat and could name
; every address the page tables described, so the way to find one that
; was not there was to go above the 256MB they cover.  It is a GENERAL
; PROTECTION fault now, and from a much shorter distance - a program's
; segment stops at the end of its slot, so F0000000h is refused by the
; segment limit long before any page table is consulted.
        mov     edx,OFFSET msg_page
        call    puts
        mov     eax,0F0000000h
        mov     dword ptr [eax],0DEADBEEFh
        jmp     cr_survived

;--- and the one that is the whole point of the release ---------------
; A READ, not a write, and of the kernel rather than of nothing: the
; first byte of PMDOS.SYS.  Until this release that read SUCCEEDED - a
; native program shared the kernel's address space and its ring, so it
; could read the kernel, write the kernel, and scribble on the arena on
; its way past.  There is now no offset in this program's segment that
; names any of it, and this is what proves it: the fault happens at the
; instruction that tries, and the offset it names is one this program
; is entitled to hold and not entitled to use.
cr_kernel:
        mov     edx,OFFSET msg_kern
        call    puts
        mov     eax,DOS_BASE
        mov     eax,dword ptr [eax]
        call    puthex32                ; if this prints, containment failed
        mov     edx,OFFSET msg_crlf
        call    puts
        jmp     cr_survived

;--- divide by zero: #DE ---------------------------------------------
cr_div:
        mov     edx,OFFSET msg_div
        call    puts
        xor     edx,edx
        mov     eax,1
        xor     ecx,ecx
        div     ecx
        jmp     cr_survived

;--- an opcode the CPU has no meaning for: #UD -----------------------
cr_bad:
        mov     edx,OFFSET msg_ud
        call    puts
        db      0Fh, 0Bh                ; UD2
        jmp     cr_survived

;--- through a null pointer: #PF, at last -----------------------------
; THIS IS THE THIRD ANSWER THIS TEST HAS HAD, and the three are a
; history of the guard.  Before v0.31 the write did not fault at all:
; linear zero is the real-mode interrupt vector table, it is mapped,
; and a ring-0 program could write it - so a null store scribbled on
; vectors 0 to 3, returned with the carry clear, and the program failed
; somewhere else entirely, minutes later.  v0.31 caught it with four
; debug registers, because page zero of the MACHINE holds the IVT, the
; BIOS data area, InDOS and the real-mode frame and could not be taken
; away; that reported exception 01, watched only the first sixteen
; bytes, saw only writes, and could not be exercised on an emulator at
; all.
;
; A program's address zero is its own page now, and it is not there.
; So this is exception 0E with CR2 pointing at the foot of the slot -
; whatever the width of the access, whatever the offset within the
; page, read or write - and every emulator implements it.  The debug
; registers stay armed for the KERNEL's own null pointers, which is the
; job they are now the right size for.
cr_null:
        ; The guard's state is still worth printing, because it is
        ; still guarding something - just not this program.
        mov     ax,0F107h
        int     21h
        push    ebx
        mov     edx,OFFSET msg_narmed
        or      al,al
        jnz     cr_nsay
        mov     edx,OFFSET msg_noff
cr_nsay:
        call    puts
        mov     edx,OFFSET msg_ndr7
        call    puts
        pop     eax
        call    puthex32
        mov     edx,OFFSET msg_crlf
        call    puts

        mov     edx,OFFSET msg_null
        call    puts
        xor     eax,eax
        mov     dword ptr [eax],0DEADBEEFh
        jmp     cr_survived

;--- twice, to prove the recovery re-arms ----------------------------
; The FIRST fault sets the nested guard in PMIO, and only a successful
; return to the prompt clears it.  If the second run of this halted
; the machine, the guard was never cleared and the recovery would have
; been a one-shot.
cr_again:
        mov     edx,OFFSET msg_twice
        call    puts
        mov     eax,0F0000000h
        mov     dword ptr [eax],1
        jmp     cr_survived

;--- if this ever prints, the fault did not happen -------------------
cr_survived:
        mov     edx,OFFSET msg_alive
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; EAX as eight hex digits
puthex32:
        push    ecx
        mov     ecx,8
ph_lp:
        rol     eax,4
        push    eax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph_out
        add     al,7
ph_out:
        push    edx
        movzx   edx,al
        mov     ah,02h
        int     21h
        pop     edx
        pop     eax
        loop    ph_lp
        pop     ecx
        ret

msg_hi      db  13,10,'CRASH - a 32-bit program about to fault on purpose.',13,10
            db  'The prompt should come back; the machine should not stop.',13,10,'$'
msg_page    db  '  writing outside the slot, at F0000000...',13,10,'$'
msg_kern    db  '  reading the kernel at DOS_BASE - it should not be there.',13,10
            db  '  what came back: $'
msg_div     db  '  dividing by zero...',13,10,'$'
msg_ud      db  '  executing an invalid opcode...',13,10,'$'
msg_null    db  '  writing through a null pointer...',13,10,'$'
msg_narmed  db  '  the null guard reports itself ARMED',13,10,'$'
msg_noff    db  '  the null guard reports itself OFF',13,10,'$'
msg_ndr7    db  '  DR7 as the CPU holds it: $'
msg_crlf    db  13,10,'$'
msg_twice   db  '  faulting (run me again to fault a second time)...',13,10,'$'
msg_alive   db  '  *** it did not fault - nothing was tested ***',13,10,'$'

_CR     ENDS
        END
