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
;   CRASH          a page fault - a write to unmapped memory
;   CRASH /D       divide by zero
;   CRASH /I       an invalid opcode
;   CRASH /N       a write through a null pointer
;   CRASH /A       crash twice in a row, to show the second one is
;                  caught as well as the first
;
; Nothing here is subtle on purpose.  The value of the test is not the
; kind of fault, it is that the shell is still there afterwards.
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_CR     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_CR, DS:_CR, ES:_CR, SS:_CR
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD PROG_BASE,<OFFSET cr_entry>,<0>,4096

cr_entry:
        mov     edx,OFFSET msg_hi
        call    puts

        ; The command tail lives at PSP+80h: a length byte, then the
        ; text.  All that is wanted from it is a letter after a slash.
        mov     esi,PROG_PSP + 81h
        movzx   ecx,byte ptr ds:[PROG_PSP + 80h]
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

;--- a write to unmapped memory: #PF, and CR2 says where -------------
; This is what the plain "CRASH" does because a NULL pointer, which was
; the first thing tried here, did not fault at all: linear zero is the
; real-mode interrupt vector table, it is mapped, and a D32 program is
; at ring 0 - so a null write quietly scribbled on the IVT and carried
; on.  v0.31 fixed that; see /N below.
;
; PG_MAXTAB page tables cover the first 256MB, so anything above that
; has no mapping at all.
        mov     edx,OFFSET msg_page
        call    puts
        mov     eax,0F0000000h
        mov     dword ptr [eax],0DEADBEEFh
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

;--- through a null pointer: #DB, from the debug registers ------------
; The bug this exists to catch is the commonest one there is, and
; before v0.31 it was also the quietest: linear 0 is mapped, this code
; is at ring 0, and the write landed on interrupt vectors 0 to 3 and
; came back with the carry flag clear.  The program went on to fail
; somewhere else entirely, minutes later.
;
; It is NOT a page fault even now, and the reason is worth knowing:
; page zero also holds the BIOS data area, InDOS and the real-mode
; thunk's register frame, so the kernel cannot unmap it.  Four debug
; registers watch the first sixteen bytes instead - IO\NULLTRAP.INC
; has the argument.
;
; So the reported vector is 01 rather than 0E, and there is no CR2,
; which is why the handler prints a line naming what it was.
cr_null:
        ; Say whether the guard is even armed before tripping it, so a
        ; survival can be read.  An emulator that does not implement
        ; debug registers reports "armed" here and then does not fault,
        ; which is a fact about the emulator; "not armed" would be a
        ; fact about PM-DOS.
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
msg_page    db  '  writing to unmapped memory at F0000000...',13,10,'$'
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
