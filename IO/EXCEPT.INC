;=====================================================================
; EXCEPT.INC - IDT construction, CPU exception stubs, IRQ stubs
;
; Included by IOPM.ASM.
;=====================================================================

;---------------------------------------------------------------------
; Stub macros.
;
; EXCN is for vectors where the CPU pushes no error code, so we push a
; zero to keep the frame layout identical for both kinds.  EXCE is for
; the vectors that do push one (8, 10-14, 17).
;---------------------------------------------------------------------
EXCN    MACRO   n
exc&n:
        push    0
        push    n
        jmp     exc_common
        ENDM

EXCE    MACRO   n
exc&n:
        push    n
        jmp     exc_common
        ENDM

IRQS    MACRO   n
irq&n:
        push    n
        jmp     irq_common
        ENDM

;---------------------------------------------------------------------
        IRP     v,<0,1,2,3,4,5,6,7,9,15,16,18,19,20,21,22,23,24,25,26,27,28,29,30,31>
        EXCN    %v
        ENDM

        IRP     v,<8,10,11,12,13,14,17>
        EXCE    %v
        ENDM

        IRP     v,<0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15>
        IRQS    %v
        ENDM

;=====================================================================
; exc_common - report the fault and stop.
;
; Stack on entry:  [esp+00] vector
;                  [esp+04] error code
;                  [esp+08] EIP
;                  [esp+0C] CS
;                  [esp+10] EFLAGS
;=====================================================================
exc_common:
        cli
IFDEF HANG_E9
        ; EVERY EXCEPTION BUT THE TWO A BOX TAKES ALL DAY, said on E9h
        ; before anything below can take a second one: registers, the
        ; stack and OUT, and nothing through DS, which out of V86 is a
        ; null selector.  A #GP is how every guest INT arrives and a
        ; #NM is the coprocessor changing hands, so those two would
        ; bury the line that matters - and slow the machine enough to
        ; move it.
        push    eax
        push    ebx
        push    ecx
        mov     eax,[esp+12]                    ; three pushes, then the vector
        cmp     eax,13
        je      exe9_out
        cmp     eax,7
        je      exe9_out
        mov     al,'E'
        out     0E9h,al
        mov     al,'X'
        out     0E9h,al
        mov     al,' '
        out     0E9h,al
        mov     eax,[esp+12]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,[esp+24]                    ; CS
        call    e9_hex32
        mov     al,':'
        out     0E9h,al
        mov     eax,[esp+20]                    ; EIP
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,[esp+16]                    ; the error code
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,[esp+28]                    ; EFLAGS
        call    e9_hex32
        ; Where the CPU put this frame - TSS ESP0, for anything out of
        ; ring 3 or a box - and for a page fault the address it wanted.
        mov     al,' '
        out     0E9h,al
        mov     al,'0'
        out     0E9h,al
        mov     al,'='
        out     0E9h,al
        mov     eax,dword ptr ss:[TSS_BASE + TSS_ESP0]
        call    e9_hex32
        cmp     dword ptr [esp+12],14
        jne     exe9_nocr2
        mov     al,' '
        out     0E9h,al
        mov     al,'2'
        out     0E9h,al
        mov     al,'='
        out     0E9h,al
        mov     eax,cr2
        call    e9_hex32
exe9_nocr2:
        ; FROM RING 0 there is no stack switch, so the frame sits on the
        ; stack the kernel was using: say where that is, and the eight
        ; dwords above the frame, which is where its return addresses are.
        test    dword ptr [esp+28],EFL_VM
        jnz     exe9_nostk
        test    byte ptr [esp+24],3
        jnz     exe9_nostk
        mov     al,' '
        out     0E9h,al
        mov     al,'s'
        out     0E9h,al
        mov     al,'='
        out     0E9h,al
        lea     ebx,[esp+32]                    ; the faulting code's ESP
        mov     eax,ebx
        call    e9_hex32
        mov     ecx,8
exe9_stk:
        push    ecx
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr ss:[ebx]
        call    e9_hex32
        add     ebx,4
        pop     ecx
        loop    exe9_stk
exe9_nostk:
        ; AN INVALID OPCODE IN A BOX: is it the program's own code, or
        ; memory that was something else when the program was loaded?
        ; The sixteen bytes at CS:IP, and the name in the arena header a
        ; paragraph below CS, which owns them when the program is a .COM.
        ; Read through SS, which is flat, because DS out of V86 is null.
        test    dword ptr [esp+28],EFL_VM
        jz      exe9_eol
        cmp     dword ptr [esp+12],6
        jne     exe9_eol
        mov     ebx,[esp+24]
        and     ebx,0FFFFh
        shl     ebx,4
        mov     eax,[esp+20]
        and     eax,0FFFFh
        add     ebx,eax                         ; EBX = linear CS:IP
        mov     al,' '
        out     0E9h,al
        mov     ecx,16
exe9_byte:
        push    ecx
        mov     al,byte ptr ss:[ebx]
        call    e9_hex8
        inc     ebx
        pop     ecx
        loop    exe9_byte
        mov     ebx,[esp+24]
        and     ebx,0FFFFh
        dec     ebx
        shl     ebx,4                           ; EBX = the header below CS
        mov     al,byte ptr ss:[ebx]
        cmp     al,'M'
        je      exe9_name
        cmp     al,'Z'
        jne     exe9_eol
exe9_name:
        mov     al,' '
        out     0E9h,al
        mov     ecx,8
exe9_nch:
        mov     al,byte ptr ss:[ebx+8]
        or      al,al
        jz      exe9_eol
        out     0E9h,al
        inc     ebx
        loop    exe9_nch
exe9_eol:
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
exe9_out:
        pop     ecx
        pop     ebx
        pop     eax
ENDIF

        ; A FAULT OUT OF V86 MODE ARRIVES WITH DS, ES, FS AND GS SET TO
        ; NULL SELECTORS.  The CPU does that deliberately on the way to
        ; a protected-mode handler, so that the handler cannot be handed
        ; the guest's 16-bit segments and quietly use them.  It means
        ; the very first thing here that touches memory through DS - the
        ; lodsb inside con_str, or any ds:[...] in the monitor - takes a
        ; second #GP on a null selector, which arrives back here, faults
        ; again, and triple-faults the machine.  No output, instant
        ; reboot; irq_common has always reloaded them for this reason.
        ;
        ; EAX is saved around it because it belongs to the faulting code
        ; and v86_fault's pushad has not run yet.
        ;
        ; THE FOUR ARE KEPT FIRST, because for one caller this is not a
        ; fault at all.  pm3_fault emulates CLI, STI, IN and OUT for
        ; ring-3 code and then RESUMES it - and an IRETD to a lower
        ; privilege level nulls any data selector whose DPL is below the
        ; level being returned to, which is all four of these once they
        ; hold the kernel's.  Ring-3 code resumed that way faults on its
        ; next memory reference, with a #GP and an error code of zero
        ; that names nothing.  They are stored through SS because DS
        ; cannot be trusted here: a fault out of V86 arrives with all
        ; four already null, which is the reason this block exists.
        mov     word ptr ss:[exc_sds],ds
        mov     word ptr ss:[exc_ses],es
        mov     word ptr ss:[exc_sfs],fs
        mov     word ptr ss:[exc_sgs],gs
        push    eax
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     es,ax
        mov     fs,ax
        mov     gs,ax
        pop     eax

        ; A fault with VM set did not come from the kernel - it came out
        ; of a V86 guest, and for that the monitor is the handler.  Most
        ; of them are not errors at all: a DOS program executing INT 21h
        ; arrives here, because that is exactly how the monitor gets to
        ; see it.
        test    dword ptr [esp+10h],EFL_VM
        jnz     v86_fault

        ; And a fault from CPL 3 without VM set is a DPMI client -
        ; protected-mode ring-3 code, which the kernel never is.  Most
        ; of those are not errors either: the client runs at IOPL 0 so
        ; that CLI, STI, IN and OUT come back to us, exactly as they
        ; do from a DOS box and for exactly the same reason.
        test    byte ptr [esp+0Ch],3
        jnz     pm3_fault

        mov     ebp,esp

        mov     esi,OFFSET msgExc
        call    con_str
        mov     eax,[ebp+0]             ; vector
        call    con_hex8
        mov     esi,OFFSET msgAt
        call    con_str
        mov     eax,[ebp+8]             ; faulting EIP
        call    con_hex32
        mov     esi,OFFSET msgErr
        call    con_str
        mov     eax,[ebp+4]             ; error code
        call    con_hex32

        ; A page fault says WHERE in CR2, and without it the report
        ; names the instruction but not the address it was reaching
        ; for - which is the half that identifies the bug.
        cmp     dword ptr [ebp+0],14
        jne     exc_nocr2
        mov     esi,OFFSET msgCR2
        call    con_str
        mov     eax,cr2
        call    con_hex32
exc_nocr2:
        mov     esi,OFFSET msgCR
        call    con_str

        ; A #DB with one of our four breakpoint bits set in DR6 is a
        ; write through a null pointer, and saying so is most of the
        ; value of catching it: "exception 01" describes the mechanism
        ; and names no bug at all.  nt_hit clears DR6 either way,
        ; because the bits are sticky and a stale one would make the
        ; next debug exception - anybody's - look like this one.
        cmp     dword ptr [ebp+0],1
        jne     exc_nodb
        call    nt_hit
        jz      exc_nodb
        mov     esi,OFFSET msgNull
        call    con_str
exc_nodb:

        ; A RING-0 FAULT USED TO END THE MACHINE, and the thing that
        ; caused it was nearly always an ordinary PM-DOS program: a
        ; native 32-bit program here runs in the kernel's own segment
        ; at ring 0, so its bugs and the kernel's bugs arrive at this
        ; same label and were treated the same way.  A 16-bit guest has
        ; been recoverable since the VDM existed - its box dies and the
        ; prompt comes back - and there was no good reason for the
        ; kernel's own software to be the one thing that could not be
        ; survived.
        ;
        ; So the DOS side gets to unwind to a fresh shell.  Nothing is
        ; repaired by this and nothing pretends to be: the program is
        ; gone, whatever it was in the middle of is abandoned, and the
        ; report above has already been printed.  It buys back the
        ; prompt, which on a development machine is the whole cost of
        ; every fault.
        cmp     dword ptr ds:[exc_hook],0
        je      exc_stop
        cmp     dword ptr ds:[exc_nested],0
        jne     exc_stop                ; the recovery itself faulted, and
                                        ; going round again would only do
                                        ; it faster
        mov     dword ptr ds:[exc_nested],1
        mov     esi,OFFSET msgRecover
        call    con_str
        call    dword ptr ds:[exc_hook] ; does not return

exc_stop:
        mov     esi,OFFSET msgHalted
        call    con_str
exc_hlt:
        hlt
        jmp     exc_hlt

;=====================================================================
; exc_sethook - IOS_EXCHOOK.  EDI = what to call after a ring-0 fault
;               has been reported.
;=====================================================================
exc_sethook:
        mov     dword ptr ds:[exc_hook],edi
        ret

;=====================================================================
; exc_clearnest - the DOS side calls this once it is safely back at a
;                 prompt, which is what makes the NEXT fault
;                 recoverable too.
;=====================================================================
exc_clearnest:
        mov     dword ptr ds:[exc_nested],0
        ret

exc_hook        dd      0
exc_nested      dd      0

IFDEF V86_WATCH
V86_WATCHDATA   EQU     1
ENDIF
IFDEF V86_WATCHQ
V86_WATCHDATA   EQU     1
ENDIF
IFDEF DBG_LPT
V86_WATCHDATA   EQU     1
ENDIF
IFDEF V86_WATCHDATA
V86_WATCHTICKS  EQU     36              ; two seconds at 18.2 Hz
DBG_DEADLINE    EQU     820             ; ...and forty-five seconds no
                                        ; matter what, because A GUEST
                                        ; THAT SPINS THROUGH THE MONITOR
                                        ; IS NEVER QUIET: it faults on
                                        ; every turn of its loop, clears
                                        ; the counter below, and defeats
                                        ; a trigger that waits for
                                        ; silence.  That is not a
                                        ; hypothetical - it is what
                                        ; Windows does after the logo.
DBG_QUIETTICKS  EQU     91              ; five seconds, and deliberately
                                        ; longer than the loud watchdog's
                                        ; two: this one fires ONCE and
                                        ; there is no second chance, so it
                                        ; waits until a slow disk read or a
                                        ; long BIOS call cannot be the
                                        ; explanation for the silence.
; NOT v86_watch, WHICH IT WAS.  wasm folds case, so that label was the
; symbol V86_WATCH, and every IFDEF V86_WATCH below it was true in a
; build that had only asked for the trace: the loud watchdog went in
; beside the quiet one and put this counter back to zero on every tick
; that did not find the guest in V86 mode.  The trace's quiet trigger
; could not fire, and only the deadline ever wrote one.
v86_wtick       dd      0               ; ticks since the monitor last ran
v86_wcs         dd      0
v86_weip        dd      0
msgV86Spin      db      'V86: the guest has run 2s without a single trap, at ',0
msgV86SpinCR    db      13,10,0
ENDIF

IFDEF HANG_E9
;---------------------------------------------------------------------
; The hang reporter's half in PMIO - see irq_timer, and GV_HANGT in
; PMDOS.INC for the mailbox.  Port E9h is the Bochs debug port: one OUT
; a character, no handshake, and nothing the machine has to be in the
; right state for, which is the whole requirement of a channel that
; reports on a machine in the wrong one.  DOSBox-X with "bochs debug
; port e9=true" writes what arrives into its log (DBXRUN.PS1 -E9); on
; real iron nothing answers at E9h and the OUTs go nowhere.
;---------------------------------------------------------------------
HANG_TICKS      EQU     91              ; five seconds of nobody asking,
HANG_EVERY      EQU     18              ; then a sample a second,
HANG_MAX        EQU     8               ; eight of them

HANG_STUBDW     EQU     ( ( exc_common - exc0 ) + 64 ) / 4  ; the stubs and
                                        ; exc_common's first instructions

hang_eip        dd      0
hang_cs         dd      0
hang_fl         dd      0
hang_esp        dd      0
hang_ss         dd      0
hang_armed      db      0
hang_idtr       dw      0               ; what SIDT said last tick...
                dd      0
hang_idtrnow    dw      0               ; ...and this one
                dd      0
hang_idtsum     dd      0
hang_stubsum    dd      0
hang_idtsnow    dd      0
hang_stubsnow   dd      0
hang_nchg       dd      0
msgChg          db      'CHG idtr ',0
msgChgIdt       db      ' idt ',0
msgChgStub      db      ' stubs ',0
msgChgWas       db      ' (was ',0
msgChgClose     db      ')',0
msgChgAt        db      ' | at ',0

; ESI -> the first dword, ECX = how many.  EAX = their sum.
hang_sum:
        xor     eax,eax
hs_lp:
        add     eax,dword ptr ds:[esi]
        add     esi,4
        loop    hs_lp
        ret

; The fault path moved: what it is now, what it was, who was running.
hang_chgreport:
        mov     esi,OFFSET msgChg
        call    e9_str
        movzx   eax,word ptr ds:[hang_idtrnow]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr ds:[hang_idtrnow+2]
        call    e9_hex32
        mov     esi,OFFSET msgChgIdt
        call    e9_str
        mov     eax,dword ptr ds:[hang_idtsnow]
        call    e9_hex32
        mov     esi,OFFSET msgChgStub
        call    e9_str
        mov     eax,dword ptr ds:[hang_stubsnow]
        call    e9_hex32
        mov     esi,OFFSET msgChgWas
        call    e9_str
        movzx   eax,word ptr ds:[hang_idtr]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr ds:[hang_idtr+2]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr ds:[hang_idtsum]
        call    e9_hex32
        mov     al,' '
        out     0E9h,al
        mov     eax,dword ptr ds:[hang_stubsum]
        call    e9_hex32
        mov     esi,OFFSET msgChgClose
        call    e9_str
        mov     esi,OFFSET msgChgAt
        call    e9_str
        mov     eax,dword ptr ds:[hang_cs]
        call    e9_hex32
        mov     al,':'
        out     0E9h,al
        mov     eax,dword ptr ds:[hang_eip]
        call    e9_hex32
        mov     esi,OFFSET msgHang21
        call    e9_str
        mov     eax,dword ptr ds:[GV_HANGAX]
        call    e9_hex32
        mov     esi,OFFSET msgHangFrom
        call    e9_str
        mov     eax,dword ptr ds:[GV_HANGCS]
        call    e9_hex32
        mov     al,':'
        out     0E9h,al
        mov     eax,dword ptr ds:[GV_HANGIP]
        call    e9_hex32
        mov     esi,OFFSET msgHangCR
        call    e9_str
        ret
msgHang         db      'HANG ',0
msgHangAt       db      ': at ',0
msgHangFl       db      ' fl ',0
msgHangSp       db      ' ss:esp ',0
msgHang21       db      ' | last 21h ax ',0
msgHangFrom     db      ' from ',0
msgHangIn       db      ' indos ',0
msgHangCR       db      13,10,0

; One line: the sample, then the last INT 21h and whether DOS is in one.
hang_report:
        mov     esi,OFFSET msgHang
        call    e9_str
        mov     eax,dword ptr ds:[GV_HANGN]
        call    e9_hex32
        mov     esi,OFFSET msgHangAt
        call    e9_str
        mov     eax,dword ptr ds:[hang_cs]
        call    e9_hex32
        mov     al,':'
        out     0E9h,al
        mov     eax,dword ptr ds:[hang_eip]
        call    e9_hex32
        mov     esi,OFFSET msgHangFl
        call    e9_str
        mov     eax,dword ptr ds:[hang_fl]
        call    e9_hex32
        ; Only a frame from ring 3 or a box has a stack of its own in it;
        ; from ring 0 those two dwords are somebody else's.
        test    dword ptr ds:[hang_fl],EFL_VM
        jnz     hr_stack
        test    byte ptr ds:[hang_cs],3
        jz      hr_nostack
hr_stack:
        mov     esi,OFFSET msgHangSp
        call    e9_str
        mov     eax,dword ptr ds:[hang_ss]
        call    e9_hex32
        mov     al,':'
        out     0E9h,al
        mov     eax,dword ptr ds:[hang_esp]
        call    e9_hex32
hr_nostack:
        mov     esi,OFFSET msgHang21
        call    e9_str
        mov     eax,dword ptr ds:[GV_HANGAX]
        call    e9_hex32
        mov     esi,OFFSET msgHangFrom
        call    e9_str
        mov     eax,dword ptr ds:[GV_HANGCS]
        call    e9_hex32
        mov     al,':'
        out     0E9h,al
        mov     eax,dword ptr ds:[GV_HANGIP]
        call    e9_hex32
        mov     esi,OFFSET msgHangIn
        call    e9_str
        movzx   eax,byte ptr ds:[DOS_INDOS]
        call    e9_hex32
        mov     esi,OFFSET msgHangCR
        call    e9_str
        ret

; ESI -> ASCIIZ, out through E9h.  AL and ESI are not kept.
e9_str:
        lodsb
        or      al,al
        jz      e9s_out
        out     0E9h,al
        jmp     short e9_str
e9s_out:
        ret

; AL as two hex digits through E9h.  AL is not kept.
e9_hex8:
        push    ebx
        mov     ebx,eax
        rol     bl,4
        mov     al,bl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     e9b_hi
        add     al,7
e9b_hi:
        out     0E9h,al
        rol     bl,4
        mov     al,bl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     e9b_lo
        add     al,7
e9b_lo:
        out     0E9h,al
        pop     ebx
        ret

; EAX as eight hex digits through E9h.  AL and ECX are not kept.
e9_hex32:
        push    ebx
        mov     ebx,eax
        mov     ecx,8
e9h_lp:
        rol     ebx,4
        mov     al,bl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     e9h_dig
        add     al,7
e9h_dig:
        out     0E9h,al
        loop    e9h_lp
        pop     ebx
        ret
ENDIF

; The segment registers the faulting code was holding, kept by
; exc_common and put back by pm3_fault when it resumes ring-3 code.
; Not a stack, because a fault taken inside this handler is fatal
; anyway and there is never a second set to keep.
exc_sds         dw      0
exc_ses         dw      0
exc_sfs         dw      0
exc_sgs         dw      0

;=====================================================================
; irq_common - dispatch to a handler, then acknowledge the PIC.
;
; Stack on entry:  [esp] IRQ number, then the usual interrupt frame.
;=====================================================================
irq_common:
        pushad
        push    ds
        push    es
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     es,ax
        cld

        mov     ebx,[esp+40]            ; IRQ number (8 pushad regs + 2 segs)
        call    dword ptr [irq_vectors + ebx*4]

        ; End of interrupt.  Slave first if this came from IRQ 8-15.
        ; THE NUMBER IS READ BACK RATHER THAN KEPT: a handler is under
        ; no obligation to preserve anything, and irq_keyboard does not
        ; - it leaves a scan code in EBX - so trusting the register
        ; here was asking whether the last key pressed was above 08h.
        ; Harmless only because a spurious EOI to the slave lands on a
        ; PIC with nothing in service.
        mov     ebx,[esp+40]
        cmp     ebx,8
        jb      irq_master
        mov     al,20h
        out     PIC2_CMD,al
irq_master:
        mov     al,20h
        out     PIC1_CMD,al

        ; ...and now, with the interrupt acknowledged, the guest can be
        ; given one of its own if it is owed one.  After the EOI rather
        ; than before, so that the guest's handler is entered on a PIC
        ; that has already been told this interrupt is finished with -
        ; its own EOI, which every DOS keyboard handler sends, is then
        ; the harmless second one rather than the necessary first.
        ;
        ; EBP is four bytes below the IRQ number so that FR_EIP and the
        ; rest of the fault-frame offsets address this frame: an IRQ
        ; pushes everything an exception does except the error code.
        mov     ebp,esp
        add     ebp,36
        call    v86_irqexit

        ; And a NATIVE program's mouse event handler, if one is owed and
        ; this interrupt stopped that program at ring 3 - the one moment
        ; its state is all in this frame.  ms_r3chk looks at the CS
        ; first and does nothing for anybody else.
        lea     ebx,[esp+8]             ; the PUSHAD block
        lea     ebp,[esp+44]            ; the IRETD frame
        call    ms_r3chk

        ; And a DPMI CLIENT, which is a DOS box that happens to be in
        ; protected mode: an interrupt it has a handler for is its to
        ; take here.  vpc_pmchk looks at the frame and does nothing for
        ; anybody else.
        call    vpc_pmchk

        pop     es
        pop     ds
        popad
        cmp     byte ptr ss:[fpu_owed],0
        jne     short irq_fperr
irq_away:
        add     esp,4                   ; discard the IRQ number
        iretd

        ; THE COPROCESSOR'S ERROR, ON A MACHINE WHERE IT IS AN INTERRUPT
        ; (fpu_line, FPU.INC).  It belongs to the program that was
        ; running, and from here on it is dealt with exactly as the
        ; exception would have been: the frame an interrupt leaves is an
        ; exception's without its error code, which the IRQ number's
        ; slot becomes.  If what was interrupted is the kernel, the
        ; program is inside a call and this waits for an interrupt that
        ; finds it outside one.
irq_fperr:
        test    dword ptr [esp+12],EFL_VM
        jnz     short irq_fpnow
        test    byte ptr [esp+8],3
        jz      irq_away
irq_fpnow:
        mov     byte ptr ss:[fpu_owed],0
        mov     dword ptr [esp],0
        push    16
        jmp     exc_common

;---------------------------------------------------------------------
; irq_timer - IRQ 0, 18.2 Hz.
;
; THIS HANDLER HAS TO DO THE BIOS'S JOB AS WELL AS ITS OWN, and until
; v0.32 it did not.  Owning IRQ 0 in protected mode means the BIOS's
; INT 8 never runs - it is real-mode code, reachable only during an
; excursion - so everything that handler was responsible for simply
; stopped happening, and both consequences showed up the first time
; PM-DOS was run on real hardware rather than an emulator:
;
;   THE FLOPPY MOTOR NEVER STOPS.  The BIOS turns the motor on for a
;   transfer and leaves it running, with a countdown at 40:40 for the
;   timer tick to wind down; when it reaches zero the tick turns the
;   motor off.  Nothing was decrementing it, so the drive light stayed
;   on from the first disk access until the machine was switched off.
;   That is not cosmetic on iron the way it looked in an emulator: it
;   is a motor and a head load left running against real media.
;
;   THE BIOS TICK COUNT AT 40:6C IS FROZEN.  The kernel kept its own
;   counter and left the BDA's alone, so every real-mode program that
;   times anything by reading 40:6C directly - which is most of them,
;   because it is cheaper than INT 1Ah - saw a clock that had stopped.
;   A delay loop written that way does not run slow, it never ends.
;
; The tick count now lives ONLY in the BDA - the kernel's private
; tick_count is gone and IOS_TICKS reads 40:6C - so a DOS box reading
; the BDA and the kernel answering IOS_TICKS cannot drift apart.
;
; AN INTERRUPT IS NOT ALWAYS A TICK.  A DOS box may run the timer
; faster for its own INT 08h (VTIMER.INC), and the clock is counted
; against the divisor it set, not against the interrupts: vt_tick says
; when 65536 counts of the timer's input have gone by.  Every interrupt
; is the box's, though - a game that asked for seventy a second gets
; seventy (vpc_tick).
;---------------------------------------------------------------------
irq_timer:
        lea     ebx,[esp+48]            ; what it interrupted: past ret, es,
        call    pm3_ifwatch             ; ds, pushad and irq#, the frame
        call    vpc_tick
        call    vt_tick
        setnc   byte ptr ds:[vt_whole]
        jc      it_noday
        mov     eax,dword ptr ds:[BDA_TICKS]
        inc     eax
        cmp     eax,TICKS_PER_DAY
        jb      it_noroll
        xor     eax,eax                 ; midnight: wrap and raise the
        mov     byte ptr ds:[BDA_TICKROLL],1    ; flag INT 1Ah/00h reads
it_noroll:
        mov     dword ptr ds:[BDA_TICKS],eax
it_noday:

IFDEF HANG_E9
;--- the hang reporter (BUILD hang) ----------------------------------
; Nothing has called INT 21h and no box has trapped for five seconds,
; so say where the processor is - once a second, HANG_MAX times, so
; that a loop shows as one address or a few and a stop as the same one
; over and over.  The kernel, a native program and a box are all
; reported, because which of them it is is the first thing a hang has
; to be told.  A machine stopped with interrupts off never gets here,
; and says so by saying nothing at all.
;
; PUSHAD first, so nothing below this block sees a register it did not
; set; the frame is 32 bytes further up for the same reason.
        pushad
        mov     eax,[esp+80]                    ; pushad, ret, es, ds, pushad,
        mov     dword ptr ds:[hang_eip],eax     ; irq#, then eip, cs, flags -
        mov     eax,[esp+84]                    ; and esp, ss from ring 3 or
        mov     dword ptr ds:[hang_cs],eax      ; a box
        mov     eax,[esp+88]
        mov     dword ptr ds:[hang_fl],eax
        mov     eax,[esp+92]
        mov     dword ptr ds:[hang_esp],eax
        mov     eax,[esp+96]
        mov     dword ptr ds:[hang_ss],eax

        ; AND THE FAULT PATH ITSELF, EVERY TICK: the IDT register, the
        ; IDT, and the stubs its exception gates point at.  A machine
        ; that triple-faults without ever reaching exc_common has had
        ; one of the three changed under it, and this says when - with
        ; the last INT 21h beside it, which says by whom.
        sidt    fword ptr ds:[hang_idtrnow]
        mov     esi,IDT_BASE
        mov     ecx,( IDT_LIMIT + 1 ) / 4
        call    hang_sum
        mov     ebx,eax                         ; EBX = the IDT's sum
        mov     esi,OFFSET exc0
        mov     ecx,HANG_STUBDW
        call    hang_sum
        mov     edx,eax                         ; EDX = the stubs' sum
        cmp     byte ptr ds:[hang_armed],0
        je      ihc_keep                        ; the first tick sets the baseline
        mov     eax,dword ptr ds:[hang_idtrnow+2]
        cmp     eax,dword ptr ds:[hang_idtr+2]
        jne     ihc_changed
        mov     ax,word ptr ds:[hang_idtrnow]
        cmp     ax,word ptr ds:[hang_idtr]
        jne     ihc_changed
        cmp     ebx,dword ptr ds:[hang_idtsum]
        jne     ihc_changed
        cmp     edx,dword ptr ds:[hang_stubsum]
        je      ihc_same
ihc_changed:
        cmp     dword ptr ds:[hang_nchg],HANG_MAX * 2
        jae     ihc_keep
        inc     dword ptr ds:[hang_nchg]
        mov     dword ptr ds:[hang_idtsnow],ebx
        mov     dword ptr ds:[hang_stubsnow],edx
        call    hang_chgreport
        mov     ebx,dword ptr ds:[hang_idtsnow]
        mov     edx,dword ptr ds:[hang_stubsnow]
ihc_keep:
        mov     byte ptr ds:[hang_armed],1
        mov     eax,dword ptr ds:[hang_idtrnow]
        mov     dword ptr ds:[hang_idtr],eax
        mov     ax,word ptr ds:[hang_idtrnow+4]
        mov     word ptr ds:[hang_idtr+4],ax
        mov     dword ptr ds:[hang_idtsum],ebx
        mov     dword ptr ds:[hang_stubsum],edx
ihc_same:

        inc     dword ptr ds:[GV_HANGT]
        mov     eax,dword ptr ds:[GV_HANGT]
        sub     eax,HANG_TICKS
        jb      ith_out
        xor     edx,edx
        mov     ecx,HANG_EVERY
        div     ecx
        or      edx,edx
        jnz     ith_out
        cmp     dword ptr ds:[GV_HANGN],HANG_MAX
        jae     ith_out
        inc     dword ptr ds:[GV_HANGN]
        call    hang_report
ith_out:
        popad
ENDIF

IFDEF V86_WATCH
;--- the spinning-guest sampler --------------------------------------
; A guest that CRASHES says so.  A guest that SPINS says nothing at
; all, and from outside the two are one symptom: a machine that has
; stopped.  The monitor's fault trace is no help either, because a
; program going round a loop of its own makes no calls and so causes
; no faults - the trace simply stops, which is the same thing the
; screen was already doing.
;
; The timer tick is the one thing still running, so it does the
; asking.  If the interrupted code was in V86 mode and the monitor has
; not been entered for two seconds, print where the guest is.  It
; prints a SAMPLE each time rather than once, because the question is
; not only "where" but "is it moving": three prints of the same
; address is a tight loop, three different ones inside a few hundred
; bytes is a loop with a body.
;
; The counter is cleared by v86_fault, so an ordinary program - which
; makes a DOS call every few milliseconds - never trips it.
        test    dword ptr [esp+56],EFL_VM       ; ret, es, ds, pushad, irq#,
        jz      it_nospin                       ; eip, cs, flags
        inc     dword ptr ds:[v86_wtick]
        cmp     dword ptr ds:[v86_wtick],V86_WATCHTICKS
        jb      it_spun
        mov     dword ptr ds:[v86_wtick],0
        mov     eax,[esp+48]
        mov     dword ptr ds:[v86_weip],eax
        mov     eax,[esp+52]
        mov     dword ptr ds:[v86_wcs],eax
        mov     esi,OFFSET msgV86Spin
        call    con_str
        mov     eax,dword ptr ds:[v86_wcs]
        call    con_hex32
        mov     al,':'
        call    con_out
        mov     eax,dword ptr ds:[v86_weip]
        call    con_hex32
        mov     esi,OFFSET msgV86SpinCR
        call    con_str
        jmp     short it_spun
it_nospin:
        mov     dword ptr ds:[v86_wtick],0
it_spun:
ENDIF

IFDEF DBG_LPT
;--- and the same sampler again, printing where nobody is looking ----
; THE RECORDING IS ALREADY DONE by the time this runs.  dbg_rec has
; been filling a ring buffer on every guest trap, at a cost of about a
; dozen instructions and no I/O, so nothing here has perturbed
; anything yet.  This is only the trigger: count ticks since the
; monitor was last entered, and when the guest has been silent for
; long enough that it is not coming back, print the ring to the
; printer port - ONCE.
;
; Printing after the guest has stopped is the whole trick.  There is
; no longer anything to disturb, so the trace can be as slow and as
; long as it likes.
        ; NOT "is the guest in V86".  That was the first version and it
        ; asks the wrong thing: if the machine has stopped with the CPU
        ; somewhere else - in the kernel, in a ring-3 program, or in a
        ; guest that has been given the machine outright - then the
        ; test fails on every tick and the trace never prints, which
        ; reads exactly like a channel that does not work.  Count the
        ; ticks whatever mode the interrupted code was in, and REPORT
        ; the mode: it is one of the things worth knowing.
        cmp     byte ptr ds:[dbg_dumped],0
        jne     itl_out
        ; ...and not before there is anything to say.  Nothing has run
        ; a guest yet during the kernel's own boot, so without this the
        ; five seconds elapse at the command prompt and the one dump
        ; there is gets spent on an idle machine.
        cmp     dword ptr ds:[dbg_count],0
        je      itl_out

        ; TWO TRIGGERS, and the second is the one that works.  Quiet
        ; means the guest stopped asking; the deadline means it never
        ; will stop asking and the answer has to be taken anyway.
        inc     dword ptr ds:[dbg_age]
        inc     dword ptr ds:[v86_wtick]
        cmp     dword ptr ds:[v86_wtick],DBG_QUIETTICKS
        jae     itl_fire
        cmp     dword ptr ds:[dbg_age],DBG_DEADLINE
        jb      itl_out
itl_fire:
        mov     byte ptr ds:[dbg_dumped],1

        ; Where it stopped goes out first, because it is the one fact
        ; the ring does NOT hold: the ring's last entry is the last
        ; trap the guest took, and a guest that is spinning took that
        ; one some time before it started spinning.
        mov     eax,[esp+52]
        mov     dword ptr ds:[v86_wcs],eax
        mov     eax,[esp+48]
        mov     dword ptr ds:[v86_weip],eax
        mov     esi,OFFSET dbg_mstop
        call    dbg_str
        mov     al,'R'                  ; real/V86...
        test    dword ptr [esp+56],EFL_VM
        jnz     itl_mode
        mov     al,'P'                  ; ...or protected
itl_mode:
        call    dbg_out
        mov     al,' '
        call    dbg_out
        mov     eax,dword ptr ds:[v86_wcs]
        call    dbg_hex16
        mov     al,':'
        call    dbg_out
        mov     eax,dword ptr ds:[v86_weip]
        call    dbg_hex32       ; 32 bits: in protected mode EIP is not a word
        call    dbg_crlf
        call    dbg_dump
itl_out:
ENDIF

IFDEF V86_WATCHQ
;--- the same sampler, without disturbing what it is sampling --------
; THE LOUD ONE CHANGES THE ANSWER.  V86_WATCH prints through con_str
; from inside the timer interrupt, which takes long enough that the
; guest runs measurably fewer instructions per tick - and a guest whose
; hang depends on timing then hangs somewhere else, or not at all.
; That is not a hypothetical: WIN.COM reaches its logo under the loud
; watchdog and stops before it without one.
;
; So this one writes sixteen bytes straight into the text page, which
; is nine instructions and no calls, and shows the last sample rather
; than a history.  A number that stops changing is a guest that has
; stopped; a number that keeps moving is a loop with a body.
        test    dword ptr [esp+56],EFL_VM
        jz      itq_out
        mov     edi,0B8000h + (80 - 12) * 2     ; top right, out of the way
        mov     eax,dword ptr ds:[v86_wcs]
        call    itq_word
        mov     al,':'
        mov     ah,4Fh
        stosw
        mov     eax,dword ptr ds:[v86_weip]
        call    itq_word

        ; NO BARS, AND THIS IS WHERE THEY WERE.  A version of this
        ; drew the same thirty-two bits into A0000 so that they
        ; would be visible when the guest had taken the screen into
        ; a graphics mode - and they came out unreadable, because
        ; the VGA's write mode, map mask and set/reset registers
        ; belong to the GUEST at that moment and a byte written to
        ; the framebuffer does not mean what the writer intended.
        ; Holding the guest in text mode is the way to see this, and
        ; that is what V86_NOMODE is for.
        jmp     short itq_out

; AX = the word, EDI = where.  Four nibbles, high one first.
itq_word:
        push    ecx
        push    edx
        mov     edx,eax
        mov     ecx,4
itq_dig:
        rol     dx,4
        mov     al,dl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     itq_ch
        add     al,7
itq_ch:
        mov     ah,4Fh
        stosw
        loop    itq_dig
        pop     edx
        pop     ecx
        ret
itq_out:
ENDIF

;--- the mouse's litter ----------------------------------------------
; THE 8042 HAS ONE OUTPUT BUFFER AND TWO DEVICES BEHIND IT, and a byte
; from the auxiliary one that nobody reads sits in it for ever - with
; every scan code queued up behind it, undelivered, because the
; controller has nowhere to put them.  IRQ 1 simply stops.  Not the
; box's keyboard: the MACHINE'S, and the first sign of it is a shell
; that has stopped echoing.
;
; It became reachable in v0.36 for a good reason.  A DOS box may talk
; to the mouse - vio_o64 hands D4h and its data byte to the real
; controller, because the kernel has taken the keyboard away from the
; guest and nothing else - so MSD's "is there a PS/2 mouse" now gets
; the true answer.  What it also gets is a mouse that is awake, and a
; mouse that is awake sends packets to a machine whose IRQ 12 is
; masked and whose kernel has no use for them.
;
; So the tick clears them, which is what the tick is for: it is
; already doing the BIOS's housekeeping for the floppy motor two
; instructions below.  ONLY bytes flagged as the auxiliary device's
; are taken - a keyboard byte belongs to irq_keyboard and is left
; exactly where it is - and they are drained in a loop, because a
; mouse in stream mode produces them faster than 18.2 times a second
; and clearing one per tick would leave the buffer blocked between
; them.
;
; It costs a guest that polls for a mouse reply a race it can lose:
; the tick could take the byte first.  In practice the poll follows
; the command within microseconds and the tick is 55 milliseconds
; wide, and the alternative - a keyboard that stops - is not a race,
; it is a certainty.
;
; The count is a bound and not a budget: a controller that answers
; "still full" for ever would otherwise turn the timer tick into the
; hang it was added to prevent, and an interrupt handler is the last
; place to write a loop that trusts hardware to stop.
;
; ONCE THE KERNEL DRIVES THE MOUSE ITSELF those bytes are packets, and
; they are handed to the driver rather than thrown away.  The tick still
; has to take them: a trip into the BIOS masks IRQ 12, and a byte that
; arrives then is left in the buffer with its interrupt already spent.
; While the driver is talking to the mouse (ms_probing) its replies are
; left alone altogether - they are what it is waiting for.
        cmp     byte ptr ds:[ms_probing],0
        jne     it_noaux
        mov     ecx,32
it_aux:
        in      al,PORT_KBCMD
        and     al,KBS_OBF OR KBS_AUXB
        cmp     al,KBS_OBF OR KBS_AUXB
        jne     it_noaux
        in      al,PORT_KBDATA
        cmp     byte ptr ds:[ms_hw],MS_PS2
        jne     it_auxnext
        call    ms_ps2byte              ; keeps every register
it_auxnext:
        loop    it_aux
it_noaux:

;--- the floppy motor-off countdown ----------------------------------
; Zero means "no motor running and nothing to do", which is also the
; state the count is left in after it fires, so this costs two
; instructions on the ticks where there is no floppy activity.  It is
; counted in ticks of the clock, like the BIOS's.
        cmp     byte ptr ds:[vt_whole],0
        je      it_out
        mov     al,byte ptr ds:[BDA_MOTORCNT]
        or      al,al
        jz      it_out
        dec     al
        mov     byte ptr ds:[BDA_MOTORCNT],al
        jnz     it_out
        and     byte ptr ds:[BDA_MOTORST],0F0h  ; no drive's motor is on
        mov     al,FDC_DOR_OFF
        mov     dx,FDC_DOR
        out     dx,al
it_out:
        ret

;---------------------------------------------------------------------
; irq_none - every IRQ we have nothing to say about.
;---------------------------------------------------------------------
irq_none:
        ret

;---------------------------------------------------------------------
; int_default - a software interrupt nobody claimed.  Ignore it rather
;               than fault, so a stray INT cannot take the system down.
;---------------------------------------------------------------------
int_default:
        iretd

;=====================================================================
; set_gate - install one IDT entry.
;            AL = vector, AH = type/attribute byte, EDX = handler.
;            EDX is destroyed.
;=====================================================================
set_gate:
        push    edi
        push    eax
        movzx   edi,al
        shl     edi,3
        add     edi,IDT_BASE
        mov     [edi+0],dx              ; offset 15:0
        mov     word ptr [edi+2],SEL_CODE32
        mov     byte ptr [edi+4],0      ; reserved / IST
        mov     [edi+5],ah              ; P, DPL, gate type
        shr     edx,16
        mov     [edi+6],dx              ; offset 31:16
        pop     eax
        pop     edi
        ret

;=====================================================================
; idt_init - build all 256 gates: exceptions, IRQs, then a harmless
;            default for everything else.
;=====================================================================
idt_init:
        pushad

        xor     ecx,ecx                 ; 1. default handler everywhere
idt_dflt:
        mov     edx,OFFSET int_default
        mov     al,cl
        mov     ah,8Eh
        call    set_gate
        inc     ecx
        cmp     ecx,256
        jb      idt_dflt

        xor     ecx,ecx                 ; 2. CPU exceptions 00h-1Fh
idt_exc:
        mov     edx,[exc_vectors + ecx*4]
        mov     al,cl
        mov     ah,8Eh
        call    set_gate
        inc     ecx
        cmp     ecx,32
        jb      idt_exc

        xor     ecx,ecx                 ; 3. hardware IRQs
idt_irq:
        mov     edx,[irq_stubs + ecx*4]
        mov     eax,ecx
        add     eax,IRQ_BASE
        cmp     ecx,8
        jb      idt_irq1
        add     eax,IRQ_BASE2 - IRQ_BASE - 8
idt_irq1:
        mov     ah,8Eh
        call    set_gate
        inc     ecx
        cmp     ecx,16
        jb      idt_irq

        popad
        ret

;=====================================================================
; Tables
;=====================================================================
exc_vectors:
        dd      exc0,  exc1,  exc2,  exc3,  exc4,  exc5,  exc6,  exc7
        dd      exc8,  exc9,  exc10, exc11, exc12, exc13, exc14, exc15
        dd      exc16, exc17, exc18, exc19, exc20, exc21, exc22, exc23
        dd      exc24, exc25, exc26, exc27, exc28, exc29, exc30, exc31

irq_stubs:
        dd      irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7
        dd      irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15

; Runtime dispatch targets, patchable later by device drivers.
irq_vectors:
        dd      irq_timer               ; 0  - system timer
        dd      irq_keyboard            ; 1  - keyboard
        dd      irq_none                ; 2  - cascade
        dd      irq_shared              ; 3  - everything from here on
        dd      irq_shared              ; 4    is whoever asks for it:
        dd      irq_shared              ; 5    a driver's handler, or a
        dd      irq_shared              ; 6    DOS box's.  See IRQ.INC
        dd      irq_shared              ; 7
        dd      irq_shared              ; 8  - RTC
        dd      irq_shared              ; 9
        dd      irq_shared              ; 10
        dd      irq_shared              ; 11
        dd      irq_shared              ; 12 - the mouse's, once found
        dd      irq_shared              ; 13
        dd      irq_shared              ; 14
        dd      irq_shared              ; 15

msgExc      db  13,10,'*** CPU exception ',0
msgAt       db  ' at ',0
msgErr      db  ' err ',0
msgCR2      db  ' addr ',0
msgCR       db  13,10,0
msgHalted   db  'System halted.',13,10,0
msgRecover  db  'Program terminated; returning to the command prompt.',13,10,0
msgNull     db  'Write through a null pointer.',13,10,0
