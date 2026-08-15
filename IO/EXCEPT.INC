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
        cmp     ebx,8
        jb      irq_master
        mov     al,20h
        out     PIC2_CMD,al
irq_master:
        mov     al,20h
        out     PIC1_CMD,al

        pop     es
        pop     ds
        popad
        add     esp,4                   ; discard the IRQ number
        iretd

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
;---------------------------------------------------------------------
irq_timer:
        mov     eax,dword ptr ds:[BDA_TICKS]
        inc     eax
        cmp     eax,TICKS_PER_DAY
        jb      it_noroll
        xor     eax,eax                 ; midnight: wrap and raise the
        mov     byte ptr ds:[BDA_TICKROLL],1    ; flag INT 1Ah/00h reads
it_noroll:
        mov     dword ptr ds:[BDA_TICKS],eax

;--- the floppy motor-off countdown ----------------------------------
; Zero means "no motor running and nothing to do", which is also the
; state the count is left in after it fires, so this costs two
; instructions on the ticks where there is no floppy activity.
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
        dd      irq_none                ; 3
        dd      irq_none                ; 4
        dd      irq_none                ; 5
        dd      irq_none                ; 6  - floppy (BIOS handles it)
        dd      irq_none                ; 7
        dd      irq_none                ; 8  - RTC
        dd      irq_none                ; 9
        dd      irq_none                ; 10
        dd      irq_none                ; 11
        dd      irq_none                ; 12
        dd      irq_none                ; 13
        dd      irq_none                ; 14
        dd      irq_none                ; 15

msgExc      db  13,10,'*** CPU exception ',0
msgAt       db  ' at ',0
msgErr      db  ' err ',0
msgCR2      db  ' addr ',0
msgCR       db  13,10,0
msgHalted   db  'System halted.',13,10,0
msgRecover  db  'Program terminated; returning to the command prompt.',13,10,0
msgNull     db  'Write through a null pointer.',13,10,0
