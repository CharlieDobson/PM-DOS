;=====================================================================
; VM86.INC - the virtual-8086 monitor
;
; Runs 16-bit real-mode code inside protected mode.  This is the floor
; the whole VDM stands on: everything above it - the DOS API bridge,
; the .COM loader, XMS, EMS - is written against IOS_V86RUN.
;
; The shape of it is a VM exit loop.  The caller hands over a context
; (registers, segments, and a bitmap saying which interrupt vectors it
; wants to see), and IOS_V86RUN runs the guest until it does something
; the caller has to answer, then returns with the context updated.
; Call it again to resume.  The monitor deals with the machinery -
; CLI, STI, PUSHF, POPF, IRET, and reflecting untrapped interrupts to
; the guest's own IVT - and never troubles the caller with those.
;
; Two things make V86 mode work at all, and both are easy to forget:
;
;   - A TSS.  V86 code runs at CPL 3, so any fault switches stacks
;     through SS0:ESP0.  Without a loaded task register the first
;     interrupt a DOS program executes triple-faults the machine.
;   - IOPL 0 in the guest's flags.  That is what makes INT, IRET, CLI,
;     STI, PUSHF and POPF fault into the monitor instead of running
;     for real.  It is the whole mechanism; a guest with IOPL 3 would
;     dispatch its own interrupts through the IVT and we would never
;     see them.  I/O instructions are the exception - in V86 they are
;     governed by the TSS bitmap and not by IOPL - which is why the
;     guest can drive hardware directly, and why taking a port away
;     from it is three bits in that bitmap and eight opcodes here.
;
; The interrupt flag is virtualised.  The guest always runs with the
; real IF set, because a DOS program that disables interrupts must not
; be able to stop the system's timer or keyboard; its CLI and STI move
; a flag of ours instead, and PUSHF, POPF and IRET are doctored to
; show it that flag rather than the real one.
;
; Included by IOPM.ASM.
;=====================================================================

;---------------------------------------------------------------------
; The fault frame.  EBP points at the vector our exception stub pushed;
; above it is what the CPU pushed on the way out of V86, which includes
; the four segment registers that protected mode has no other way to
; hand back.
;---------------------------------------------------------------------
FR_VEC          EQU     00h
FR_ERR          EQU     04h
FR_EIP          EQU     08h
FR_CS           EQU     0Ch
FR_FLAGS        EQU     10h
FR_ESP          EQU     14h
FR_SS           EQU     18h
FR_ES           EQU     1Ch
FR_DS           EQU     20h
FR_FS           EQU     24h
FR_GS           EQU     28h

;---------------------------------------------------------------------
; The guest's general registers, as PUSHAD left them, addressed off the
; same EBP.  v86_fault pushes them and then sets EBP eight dwords past,
; so the block sits immediately BELOW the fault frame and the offsets
; are negative.  Only the four an IN or an OUT can name are listed.
;---------------------------------------------------------------------
PA_EAX          EQU     -4
PA_ECX          EQU     -8
PA_EDX          EQU     -12
PA_EBX          EQU     -16

;=====================================================================
; v86_init - build the TSS and load the task register.  Called once,
;            from pm_entry, before anything can enter V86 mode.
;=====================================================================
v86_init:
        pushad
        mov     edi,TSS_BASE            ; a TSS is mostly zeros
        xor     eax,eax
        mov     ecx,TSS_TOTAL / 4
        rep     stosd

        mov     word ptr ds:[TSS_BASE + TSS_SS0],SEL_DATA32
        mov     word ptr ds:[TSS_BASE + TSS_IOMAP],TSS_IOBASE
        ; The byte past the bitmap must be FFh: the CPU reads two bytes
        ; for a word-wide port access, and the last port in the map has
        ; no second byte unless one is put there.
        mov     byte ptr ds:[TSS_BASE + TSS_IOBASE + 8192],0FFh

        ; The three ports the guest is not allowed to reach.  See the
        ; TSS comment in PMDOS.INC for why these three and no others.
        mov     ax,PORT_KBDATA
        call    v86_claim
        mov     ax,PORT_KBCMD
        call    v86_claim
        mov     ax,PORT_SYSCTL
        call    v86_claim

        ; AND THE TWO INTERRUPT CONTROLLERS, which are four more.  A
        ; driver in a box unmasks its card's line and ends every
        ; interrupt with an EOI, and both used to reach the machine's
        ; own 8259s - whose vectors are somewhere the box has never
        ; heard of.  See VPIC.INC.
        mov     ax,PIC1_CMD
        call    v86_claim
        mov     ax,PIC1_DATA
        call    v86_claim
        mov     ax,PIC2_CMD
        call    v86_claim
        mov     ax,PIC2_DATA
        call    v86_claim

        ; AND TWO OF THE TIMER'S, which are watched rather than stood in
        ; for: what a box writes there reaches the 8254, and the divisor
        ; it sets is what the kernel's clock has to count against.  See
        ; VTIMER.INC.
        mov     ax,PIT_CH0
        call    v86_claim
        mov     ax,PIT_CTRL
        call    v86_claim

        ; AND BOTH DMA CONTROLLERS, which are stood in for: an address a
        ; box hands one is an address in its own picture of memory, and
        ; the page tables say where that is.  See VDMA.INC.
        call    vdma_claim

        mov     ax,SEL_TSS
        ltr     ax
        popad
        ret

;---------------------------------------------------------------------
; v86_claim - AX = a port number; set its bit so that a guest touching
;             it faults into vio_in / vio_out instead of the hardware.
;---------------------------------------------------------------------
v86_claim:
        push    ebx
        push    ecx
        movzx   ebx,ax
        mov     ecx,ebx
        and     cl,7                    ; bit within the byte
        shr     ebx,3                   ; byte within the bitmap
        add     ebx,TSS_BASE + TSS_IOBASE
        mov     al,1
        shl     al,cl
        or      byte ptr ds:[ebx],al
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; v86_run - IOS_V86RUN.  ESI -> V86 context.  Enters the guest and
;           returns when it does something the caller must answer:
;           EAX = V86R_*, EBX = vector.  The context is updated.
;=====================================================================
v86_run:
        pushad

        ; A STACK THE GUEST COULD NOT WRITE IS NOT ENTERED.  The context
        ; may hold one the guest never had - DPMI's 0300h and 0306h take
        ; a real-mode SS:SP from the client - and an interrupt delivered
        ; below would go onto it as a ring-0 write.  See v86_stkok.
        mov     eax,[esi+V86C_SS]
        mov     edx,[esi+V86C_ESP]
        call    v86_stkok
        jnc     vr_stkok
        popad
        mov     eax,V86R_FAULT
        mov     ebx,12                          ; a stack fault, as it would
        ret                                     ; have been on its first PUSH
vr_stkok:

        ; IRET WITH NT SET IS A TASK SWITCH, not a return.  The CPU
        ; follows the back-link field at the front of the current TSS,
        ; finds the zero we left there, and raises #TS(0) on the very
        ; instruction that was supposed to enter the guest.  Nothing
        ; here sets NT deliberately - it arrives from whatever the BIOS
        ; left in FLAGS before the switch to protected mode - so clear
        ; it and stop wondering.
        pushfd
        and     dword ptr [esp],0FFFFBFFFh      ; NOT EFL_NT
        popfd

        mov     dword ptr ds:[v86_ctx],esi
        mov     dword ptr ds:[v86_kesp],esp

        ; Getting back here IS progress: the guest asked the DOS side
        ; for something, so whatever it faulted over last time it is
        ; past now.  vf_exc's runaway count starts again.
        mov     dword ptr ds:[v86_fltn],0
        mov     dword ptr ds:[v86_flteip],-1

        ; THE NULL GUARD IS FOR OUR CODE, NOT THE GUEST'S.  Linear 0 to
        ; 15 is the guest's interrupt vector table - vectors 0 to 3 -
        ; and a 16-bit program hooking divide-by-zero writes it as a
        ; matter of course.  The debug registers are global and do not
        ; care what mode the CPU is in, so they come off for exactly as
        ; long as the guest is running and go back on at every exit
        ; below, which is once per trap.
        push    eax
        xor     al,al
        call    nt_arm
        pop     eax

        ; ...AND WHAT THE BOX HAS PUT THERE ITSELF GOES ON, with the MP,
        ; EM and TS it asked for.  Both come out again in v86_leave;
        ; neither does anything for a box that has asked for nothing.
        ; See vsr_emul.
        call    vbp_load
        call    vfp_in

        ; Faults out of the guest land on the stack we are standing on,
        ; just below this frame.  Nothing else is down there, and it
        ; means the monitor unwinds to exactly here on the way out.
        mov     dword ptr ds:[TSS_BASE + TSS_ESP0],esp

        ; The guest's idea of the interrupt flag, at the moment it runs
        mov     byte ptr ds:[v86_vif],0
        test    dword ptr [esi+V86C_FLAGS],EFL_IF
        jz      vr_noif
        mov     byte ptr ds:[v86_vif],1
vr_noif:
        ; ...and its idea of IOPL and NT, which it may read but must
        ; never run under.  Same shadow treatment as IF above.
        mov     eax,[esi+V86C_FLAGS]
        and     eax,V86_VFLAGS OR EFL_AC OR EFL_ID
        mov     dword ptr ds:[v86_vpriv],eax

        ; A KEY THAT ARRIVED WHILE THE GUEST WAS NOT RUNNING.  The DOS
        ; side has been holding the CPU - servicing a call, or reading
        ; the disk - and IRQ 1 came in with no guest to interrupt, so
        ; it was queued instead.  This is the moment it can be handed
        ; over: the context is complete, nothing has been pushed for
        ; the entry yet, and v86_intctx works on exactly this.
        ;
        ; Before the pushes below, because it MOVES the context: the
        ; stack it stacks the guest's return frame on is the one those
        ; pushes are about to copy out of it.
        mov     byte ptr ds:[v86_active],1
        cmp     byte ptr ds:[v86_irqx],0        ; an excursion for an
        jne     vr_nokey                        ; interrupt is nobody's
        call    v86_kbwant                      ; program: keys and mouse
        jc      vr_nokey                        ; events stay the kernel's
        mov     al,9
        call    v86_intctx
        mov     byte ptr ds:[v86_vif],0         ; an ISR starts closed, and
                                                ; v86_intctx has already said
                                                ; so in the context's flags
        jmp     short vr_go
vr_nokey:
        ; ...and the Break key, for a program that hooked INT 1Bh
        ; (KEYBD.INC, kb_break) - its handler runs as the BIOS would
        ; have run it, from inside the keyboard's interrupt...
        cmp     byte ptr ds:[v86_vif],0
        je      vr_go
        cmp     byte ptr ds:[v86_irqx],0
        jne     vr_no1b
        call    v86_brkwant
        jc      vr_no1b
        call    v86_intctx
        mov     byte ptr ds:[v86_vif],0
        jmp     short vr_go
vr_no1b:
        ; ...and an interrupt from a card the box is driving, the same
        ; way and through the same table (VPIC.INC, vpc_boxwant)...
        call    vpc_boxwant
        jc      vr_noirq
        call    v86_intctx
        mov     byte ptr ds:[v86_vif],0
        jmp     short vr_go
vr_noirq:
        ; ...and a mouse event its handler is owed, the same way - to the
        ; stub rather than through the IVT (MOUSE.INC, ms_boxwant).
        cmp     byte ptr ds:[v86_vif],0
        je      vr_go
        cmp     byte ptr ds:[v86_irqx],0
        jne     vr_go
        call    ms_boxwant
        jc      vr_go
        call    v86_ctxto
        mov     byte ptr ds:[v86_vif],0
vr_go:
        push    dword ptr [esi+V86C_GS]
        push    dword ptr [esi+V86C_FS]
        push    dword ptr [esi+V86C_DS]
        push    dword ptr [esi+V86C_ES]
        push    dword ptr [esi+V86C_SS]
        push    dword ptr [esi+V86C_ESP]

        ; VM to get there, IF because the guest never really runs with
        ; interrupts off, IOPL 0 so that everything interesting faults.
        mov     eax,[esi+V86C_FLAGS]
        and     eax,V86_UFLAGS
        or      eax,EFL_VM OR EFL_IF OR 2
        push    eax

        push    dword ptr [esi+V86C_CS]
        push    dword ptr [esi+V86C_EIP]

IFDEF V86_TRACE
        push    esi
        mov     esi,OFFSET msgV86In
        call    con_str
        pop     esi
ENDIF
        mov     eax,[esi+V86C_EAX]
        mov     ebx,[esi+V86C_EBX]
        mov     ecx,[esi+V86C_ECX]
        mov     edx,[esi+V86C_EDX]
        mov     ebp,[esi+V86C_EBP]
        mov     edi,[esi+V86C_EDI]
        mov     esi,[esi+V86C_ESI]
        iretd                           ; ...and we are 16-bit again

;---------------------------------------------------------------------
; v86_leave - hand control back to whoever called v86_run.
;             EAX = reason, EBX = vector.  Entered from the fault
;             handler with the guest's registers in a PUSHAD block at
;             ESP and EBP pointing at the fault frame.
;---------------------------------------------------------------------
v86_leave:
        mov     byte ptr ds:[v86_active],0      ; the guest has stopped, so a
                                                ; keystroke from here on is the
                                                ; kernel's to decode again
        mov     dword ptr ds:[v86_reason],eax
        mov     dword ptr ds:[v86_rvec],ebx

        ; The PUSHAD block is in V86C_EDI..V86C_EAX order already
        mov     esi,esp
        mov     edi,dword ptr ds:[v86_ctx]
        mov     ecx,8
        rep     movsd

        mov     edi,dword ptr ds:[v86_ctx]
        mov     eax,[ebp+FR_EIP]
        mov     [edi+V86C_EIP],eax
        mov     eax,[ebp+FR_CS]
        mov     [edi+V86C_CS],eax
        mov     eax,[ebp+FR_ESP]        ; the guest's SP, not the ring-0 one
        mov     [edi+V86C_ESP],eax
        mov     eax,[ebp+FR_SS]
        mov     [edi+V86C_SS],eax
        mov     eax,[ebp+FR_ES]
        mov     [edi+V86C_ES],eax
        mov     eax,[ebp+FR_DS]
        mov     [edi+V86C_DS],eax
        mov     eax,[ebp+FR_FS]
        mov     [edi+V86C_FS],eax
        mov     eax,[ebp+FR_GS]
        mov     [edi+V86C_GS],eax

        ; Show the caller the flags the GUEST believes it has
        mov     eax,[ebp+FR_FLAGS]
        and     eax,V86_UFLAGS
        or      eax,dword ptr ds:[v86_vpriv]
        cmp     byte ptr ds:[v86_vif],0
        je      vl_noif
        or      eax,EFL_IF
vl_noif:
        mov     [edi+V86C_FLAGS],eax

        mov     esp,dword ptr ds:[v86_kesp]
        call    vbp_out                 ; the box's breakpoints come out of
                                        ; the processor first (vsr_emul)
        push    eax                     ; the guest has stopped: guard our own
        mov     al,1                    ; code again
        call    nt_arm
        pop     eax
        call    vfp_out                 ; ...its MP, EM and TS with them
        call    fpu_reclaim             ; ...and send a native program's
                                        ; coprocessor instructions to the
                                        ; emulator again (FPU.INC)
        popad
        mov     eax,dword ptr ds:[v86_reason]
        mov     ebx,dword ptr ds:[v86_rvec]
        ret

;=====================================================================
; v86_intctx - IOS_V86INT.  ESI -> a context, AL = vector.  Put the
;              interrupt into the guest's own handler.
;
; This is v86_reflect done to a CONTEXT rather than to a fault frame,
; and it exists because the DOS side has to be able to change its mind.
; The trap bitmap is all-or-nothing per vector: PM-DOS claims INT 2Fh
; and INT 15h because it answers a handful of subfunctions on each,
; and everything else on those vectors belongs to whoever the guest
; has in its own IVT - the BIOS, or a TSR that hooked it.  Without a
; way to hand the interrupt back, claiming a vector would mean
; swallowing every other use of it, and INT 2Fh is the DOS multiplex:
; swallowing it silently breaks every TSR ever written.
;
; Safe to call from outside the monitor because everything it touches
; is in the context: the guest's stack, its CS:IP, and the flags
; v86_run will hand back to the CPU on the next resume.
;=====================================================================
v86_intctx:
        push    eax
        movzx   eax,al
        shl     eax,2                           ; the vector's IVT slot
        mov     eax,dword ptr ds:[eax]
        call    v86_ctxto
        pop     eax
        ret

;---------------------------------------------------------------------
; v86_ctxto - the same to an ADDRESS: EAX = seg:off.  The mouse's event
;             entry is in its stub, not in the IVT.
;---------------------------------------------------------------------
v86_ctxto:
        pushad
        mov     ebx,eax

        mov     eax,[esi+V86C_FLAGS]            ; the flags it believes in
        call    vic_push
        mov     eax,[esi+V86C_CS]
        call    vic_push
        mov     eax,[esi+V86C_EIP]              ; already past the INT: the
        call    vic_push                        ; monitor skipped it before
                                                ; it decided whose this was
        and     dword ptr [esi+V86C_FLAGS],NOT EFL_IF   ; an ISR starts closed

        movzx   edx,bx
        mov     [esi+V86C_EIP],edx
        shr     ebx,16
        mov     [esi+V86C_CS],ebx
        popad
        ret

;---------------------------------------------------------------------
; v86_stkok - EAX = a guest's SS, EDX = its SP.  CF unless the guest
;             could write the V86_STKROOM bytes under SS:SP itself.
;             Everything preserved.
;
; THE MONITOR WRITES ON THE GUEST'S STACK AS RING 0, and ring 0 is not
; stopped by a read-only page.  So a guest that loads SS:SP with
; 0100:0C20 and executes an INT has the reflection's FLAGS, CS and IP
; written into the GDT for it - by us, with the guest's own CS and IP.
; A guest cannot PUSH onto such a stack itself; it can only get the
; kernel to.
;
; Asked at the three doors through which the monitor comes to write
; there: a trap (v86_fault), an interrupt about to be delivered
; (v86_irqexit), and a context handed to v86_run.  The DOS side writes
; to the guest's stack only while answering a trap, so the first door
; covers it too.  The room is more than any of them pushes before the
; guest runs again.
;
; SP wraps at 64K, so room that does not fit under SP is the top of the
; same segment.  A guest stack in the HMA with A20 off is the alias of
; page 0 to 15, and usr_wok sees the alias.
;---------------------------------------------------------------------
V86_STKROOM     EQU     64

v86_stkok:
        push    eax
        push    ecx
        push    edx
        movzx   eax,ax
        shl     eax,4                           ; EAX = the segment
        movzx   edx,dx
        cmp     edx,V86_STKROOM
        jb      vso_wrap
        lea     eax,[eax + edx - V86_STKROOM]
        mov     ecx,V86_STKROOM
        call    usr_wok
        jmp     short vso_out
vso_wrap:
        mov     ecx,edx                         ; what fits under SP...
        call    usr_wok
        jc      vso_out
        lea     eax,[eax + edx + 10000h - V86_STKROOM]
        mov     ecx,V86_STKROOM                 ; ...and the rest, at the
        sub     ecx,edx                         ; top of the segment
        call    usr_wok
vso_out:
        pop     edx
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; vic_push - AX onto the guest's stack, through the context.  EBX and
;            EDX are saved because the caller is using EBX for the
;            vector it is in the middle of delivering.
;---------------------------------------------------------------------
vic_push:
        push    ebx
        push    edx
        mov     edx,[esi+V86C_ESP]
        sub     dx,2
        mov     [esi+V86C_ESP],edx
        movzx   ebx,dx
        mov     edx,[esi+V86C_SS]
        shl     edx,4
        add     ebx,edx
        mov     [ebx],ax
        pop     edx
        pop     ebx
        ret

;=====================================================================
; v86_fault - a CPU exception taken with EFLAGS.VM set.  Reached from
;             exc_common, which checks that bit before it decides the
;             machine is dead.
;=====================================================================
v86_fault:
        pushad                          ; the guest's registers, unsaved
        mov     ebp,esp                 ; by the CPU, are ours to keep
        add     ebp,32

        ; THE DIRECTION FLAG IS THE GUEST'S AND WE ARE STANDING IN IT.
        ; DF is not shadowed - it is the CPU's own and a 16-bit program
        ; is entitled to leave it set - and every string instruction
        ; from here to v86_leave inherits it.  v86_leave saves the
        ; guest's eight general registers with a REP MOVSD, so a guest
        ; that traps with STD in force has them written BACKWARDS, off
        ; the front of the context: EIP and CS come back correct
        ; because they are stored one at a time, and AX and DX come
        ; back as whatever they were before the guest ran.  A DOS
        ; program that calls INT 21h in the middle of a string copy is
        ; not exotic; it is a REP MOVSB with a CALL in the loop.
        cld
IFDEF V86_WATCH
        mov     dword ptr ds:[v86_wtick],0      ; the guest is still asking
ENDIF
IFDEF HANG_E9
        mov     dword ptr ds:[GV_HANGT],0       ; and so is anybody in a box
ENDIF
IFDEF DBG_LPT
; EVERY FAULT INTO THE RING, and nothing else here: no I/O, no
; formatting, no decisions.  What this costs is what the guest pays
; for being watched, and the whole point of the ring is that it is
; almost nothing.
IFNDEF DBG_NOREC
        call    dbg_rec
ENDIF
ENDIF
IFDEF V86_WATCHQ
; WHERE THE GUEST FAULTED, kept for the timer to display.  The spin
; counter this used to clear answered "is the guest making calls at
; all", and the answer for a guest that is looping THROUGH the
; monitor is yes - which is no answer.  The address is.
        pushad
        mov     eax,[ebp+FR_CS]
        mov     dword ptr ds:[v86_wcs],eax
        mov     eax,[ebp+FR_EIP]
        mov     dword ptr ds:[v86_weip],eax
        popad
ENDIF
IFDEF V86_TRACE
        pushad
        mov     esi,OFFSET msgV86Flt
        call    con_str
        mov     eax,[ebp+FR_VEC]
        call    con_hex8
        mov     al,' '
        call    con_out
        mov     eax,[ebp+FR_CS]
        call    con_hex32
        mov     al,':'
        call    con_out
        mov     eax,[ebp+FR_EIP]
        call    con_hex32
        ; ...and the instruction, because "vector 13 at 0C57:1234" does
        ; not say WHICH call, and on a #GP the whole question is which.
        ; Two bytes is an INT and its vector, which is what is nearly
        ; always being asked.
        mov     esi,OFFSET msgV86Op
        call    con_str
        mov     esi,[ebp+FR_CS]
        shl     esi,4
        movzx   eax,word ptr [ebp+FR_EIP]
        add     esi,eax
        movzx   eax,byte ptr [esi]
        call    con_hex8
        movzx   eax,byte ptr [esi+1]
        call    con_hex8
        ; AX as well: on an INT 21h the function IS in AX, and a trace
        ; that says a call happened without saying which one only moves
        ; the question.
        mov     esi,OFFSET msgV86Ax
        call    con_str
        mov     eax,[ebp+PA_EAX]
        shr     eax,8
        call    con_hex8
        mov     eax,[ebp+PA_EAX]
        call    con_hex8
        mov     esi,OFFSET msgV86CR
        call    con_str
        popad
ENDIF

IFDEF V86_CALLS
;---------------------------------------------------------------------
; THE SAME TRACE WITH THE NOISE TAKEN OUT.  V86_TRACE prints every
; fault, and for a program that prints anything that is one line per
; character - so the few calls where a program ASKS the machine
; something scroll off the top before they can be read.  The questions
; are what a bug like MEM's is made of; the printing is what the
; program does with the answers.
;
; So: software interrupts only, and not the ones that are output.  A
; DOS call is named by AH, which is why the filter cannot be by vector
; alone - 21h is both "how much memory is there" and "print this
; character".
;---------------------------------------------------------------------
        pushad
        mov     esi,[ebp+FR_CS]
        shl     esi,4
        movzx   eax,word ptr [ebp+FR_EIP]
        add     esi,eax
        cmp     byte ptr [esi],0CDh     ; INT n, or there is no call here
        jne     vc_skip
        movzx   ebx,byte ptr [esi+1]    ; ...and n is the vector
        cmp     ebx,10h                 ; video
        je      vc_skip
        cmp     ebx,16h                 ; the keyboard
        je      vc_skip
        cmp     ebx,28h                 ; the idle call, issued per wait
        je      vc_skip
        cmp     ebx,21h
        jne     vc_show
        mov     eax,[ebp+PA_EAX]
        shr     eax,8
        cmp     al,02h                  ; the console functions, which a
        je      vc_skip                 ; program uses once per character
        cmp     al,06h
        je      vc_skip
        cmp     al,09h
        je      vc_skip
        cmp     al,0Bh
        je      vc_skip
        cmp     al,0Ch
        je      vc_skip
        cmp     al,3Fh                  ; and read/write, which is the
        je      vc_skip                 ; same thing through a handle
        cmp     al,40h
        je      vc_skip
vc_show:
        ; AND THE SAME CALL TWICE RUNNING IS ONE LINE.  MEM asks 38h
        ; for the country before every number it prints, which is
        ; forty identical lines that push the ones that differ off the
        ; top - the same way the unfiltered trace pushes everything
        ; off with output.  A repeat says nothing the first one did
        ; not; what a trace is for is the call that CHANGES.
        mov     eax,[ebp+PA_EAX]
        xor     eax,[ebp+PA_EBX]
        xor     eax,[ebp+PA_ECX]
        xor     eax,[ebp+PA_EDX]
        xor     eax,ebx
        cmp     eax,dword ptr ds:[vc_last]
        je      vc_skip
        mov     dword ptr ds:[vc_last],eax

        mov     esi,OFFSET msgVcInt
        call    con_str
        mov     eax,ebx
        call    con_hex8
        mov     esi,OFFSET msgVcAx
        call    con_str
        mov     eax,[ebp+PA_EAX]
        call    con_hex16
        mov     esi,OFFSET msgVcBx
        call    con_str
        mov     eax,[ebp+PA_EBX]
        call    con_hex16
        mov     esi,OFFSET msgVcCx
        call    con_str
        mov     eax,[ebp+PA_ECX]
        call    con_hex16
        mov     esi,OFFSET msgVcDx
        call    con_str
        mov     eax,[ebp+PA_EDX]
        call    con_hex16
        mov     esi,OFFSET msgVcDs
        call    con_str
        mov     eax,[ebp+FR_DS]
        call    con_hex16
        mov     esi,OFFSET msgVcEs
        call    con_str
        mov     eax,[ebp+FR_ES]
        call    con_hex16
        mov     esi,OFFSET msgVcCR
        call    con_str
vc_skip:
        popad
ENDIF

        ; A STACK THE GUEST COULD NOT WRITE ITSELF ENDS IT HERE, before
        ; the monitor or the DOS side pushes anything on it for it - see
        ; v86_stkok.  Reported as the stack fault its own first PUSH
        ; would have been.
        push    eax
        push    edx
        mov     eax,[ebp+FR_SS]
        mov     edx,[ebp+FR_ESP]
        call    v86_stkok
        pop     edx
        pop     eax
        jc      vf_badstk

        mov     eax,[ebp+FR_VEC]
        cmp     eax,14                  ; a store into the kernel's low
        jne     vf_not14                ; pages, which is dropped as a
        call    vf_romw                 ; store into ROM would be
        jnc     v86_resume
        mov     eax,[ebp+FR_VEC]
vf_not14:
        cmp     eax,13
        je      vf_isgp                 ; only #GP means "emulate me"
        cmp     eax,16                  ; the coprocessor's error, which a
        je      vf_exc                  ; DOS program knows as an interrupt
        cmp     eax,7                   ; a coprocessor instruction with EM
        jne     vf_not7                 ; set: the box's own, or lent the
        cmp     byte ptr ds:[vfp_set],0 ; bare hardware and run again.  Under
        jne     vf_exc                  ; EM or TS the box set itself it is
                                        ; its own, always (vfp_put)
        call    fpu_boxnm
        jnc     v86_resume
        jmp     vf_exc
vf_not7:
        cmp     eax,7                   ; ...but some of the rest are the
        ja      vf_notgp                ; GUEST'S, and always were
        mov     ebx,1
        mov     ecx,eax
        shl     ebx,cl
        test    ebx,V86_EXCREFL
        jnz     vf_exc
        jmp     vf_notgp
vf_isgp:

        ; The faulting instruction, at CS:IP in the guest's address
        ; space - which, with no paging yet, is just a linear address.
        mov     esi,[ebp+FR_CS]
        shl     esi,4
        movzx   eax,word ptr [ebp+FR_EIP]
        add     esi,eax

        xor     ecx,ecx                 ; step over any prefixes
        mov     byte ptr ds:[v86_o32],0 ; and remember a 66h among them
vf_pfx:
        cmp     ecx,8                   ; a real instruction is shorter
        jae     vf_bad
        mov     al,[esi+ecx]
        inc     ecx
        cmp     al,66h                  ; operand size
        je      vf_o32
        cmp     al,67h                  ; address size
        je      vf_pfx
        cmp     al,0F2h
        je      vf_pfx
        cmp     al,0F3h
        je      vf_pfx
        cmp     al,26h                  ; segment overrides
        je      vf_pfx
        cmp     al,2Eh
        je      vf_pfx
        cmp     al,36h
        je      vf_pfx
        cmp     al,3Eh
        je      vf_pfx
        cmp     al,64h
        je      vf_pfx
        cmp     al,65h
        je      vf_pfx
        jmp     vf_op
vf_o32:
        mov     byte ptr ds:[v86_o32],1
        jmp     vf_pfx
vf_op:
                                        ; AL = opcode, ECX = length so far
        cmp     al,0CDh
        je      vf_int
        cmp     al,0CFh
        je      vf_iret
        cmp     al,0FAh
        je      vf_cli
        cmp     al,0FBh
        je      vf_sti
        cmp     al,9Ch
        je      vf_pushf
        cmp     al,9Dh
        je      vf_popf
        cmp     al,0CCh
        je      vf_int3
        cmp     al,0CEh
        je      vf_into
        cmp     al,0F4h
        je      vf_hlt
        cmp     al,0E4h                 ; IN  AL,imm8
        je      vf_inb_i
        cmp     al,0E5h                 ; IN  AX,imm8
        je      vf_inw_i
        cmp     al,0E6h                 ; OUT imm8,AL
        je      vf_outb_i
        cmp     al,0E7h                 ; OUT imm8,AX
        je      vf_outw_i
        cmp     al,0ECh                 ; IN  AL,DX
        je      vf_inb_d
        cmp     al,0EDh                 ; IN  AX,DX
        je      vf_inw_d
        cmp     al,0EEh                 ; OUT DX,AL
        je      vf_outb_d
        cmp     al,0EFh                 ; OUT DX,AX
        je      vf_outw_d
        cmp     al,0Fh                  ; the control registers and the
        je      vf_0f                   ; cache: vsr_emul, below

vf_bad:
        mov     eax,V86R_FAULT
        mov     ebx,[ebp+FR_VEC]
        jmp     v86_leave
vf_notgp:
        mov     eax,V86R_FAULT
        mov     ebx,[ebp+FR_VEC]
        jmp     v86_leave
vf_badstk:
        mov     eax,V86R_FAULT
        mov     ebx,12
        jmp     v86_leave
vf_hlt:
        mov     eax,V86R_HLT
        xor     ebx,ebx
        jmp     v86_leave

;=====================================================================
; THE SYSTEM REGISTERS - CONTROL, DEBUG, TEST AND MODEL-SPECIFIC - AND
; THE CACHE.
;
; A move to or from CR0 is privileged, so in a box - and in a DPMI
; client, which is ring 3 as well - it is a general protection fault,
; and until 2026-11-13 that fault ended the program.  It should not.
; EMM386, QEMM and Windows all answer these instructions, so a program
; written for DOS reads CR0 under a memory manager without asking
; first.  CHKCPU looks at SMSW before it touches an MSR or a test
; register, then reads CR0 for the cache bits with no check at all,
; and was ended there.  The rest of the privileged 0Fh instructions a
; DOS program has a use for followed on 2026-11-14.
;
; WHAT A GUEST IS SHOWN OF THE CONTROL REGISTERS is the register: CR0,
; CR2, CR3, and CR4 where the processor has one, as they are - except
; MP, EM and TS, which are the ones it runs under (vfp_view).
;
; WHAT A GUEST MAY CHANGE IN CR0 is CD, NW and ET for the machine and
; MP, EM and TS for itself.  CD and NW are the cache: a program that
; slows a 486 down by switching the cache off is a DOS program like any
; other.  ET is a bit only a 386 lets anybody change.  PE, PG, WP and
; NE are the kernel's.  A write leaves those as they are and is not an
; error, because a program that reads CR0, sets one bit and writes all
; of it back has written every one of them.  A write to CR4 changes
; nothing, for the same reason: the bit a DOS program sets there for
; itself (OSFXSR) the host has set already, and the rest are not a
; box's to have.  Loading CR2 or CR3 is still the fault it was.  A
; program doing that is on its way into protected mode by itself, and
; it cannot be let go on; LGDT and LIDT end it for the same reason.
;
; NW SET WITH CD CLEAR is a general protection fault on an Intel
; processor, here in ring 0, and a working cache mode on a Cyrix one.
; Nothing here can tell which it would be, so a write that asks for the
; pair leaves both bits as they are - unless the pair is in force
; already, and then the write is the read-modify-write above and goes
; through.  What that costs is a box on a Cyrix processor switching
; INTO that mode, which it cannot do.
;
; MP, EM AND TS ARE A BOX'S OWN, by LMSW or by a move to CR0, and CLTS
; clears its TS.  They are how a program that brings a coprocessor
; emulator has the instructions sent to it: EM set, and each one is
; exception 7, which the monitor has always handed to the box's INT 7.
; But the kernel keeps coprocessor states with FXSAVE and a native
; program computes with the real part, and neither can run under a
; box's EM.  So the three bits a box asks for are kept (vfp_bits), put
; into CR0 when a box is entered (v86_run) and taken out when it is
; left (v86_leave).  They are one set for every box, as CR0 is one
; register: an emulator that stays resident has its EM for the next
; DOS program too.  A box that never asks runs as it always did, and a
; write of the bits it already has asks for nothing (vfp_put).  A DPMI
; client's protected-mode code runs under the kernel's bits; from
; there the write is taken and changes nothing.
;
; THE DEBUG REGISTERS ARE A BOX'S OWN AS WELL, and its breakpoints are
; real.  The kernel has a use for DR0 to DR3 itself - the null-pointer
; guard (NULLTRAP.INC) - which it drops for exactly as long as a box
; runs; the box's four addresses and its DR7 go in in their place
; (vbp_load, from v86_run) and come out again (vbp_out, from
; v86_leave).  Three things keep that safe.  A breakpoint at or above
; BOX_TOP, where no box reaches and the kernel lives, is not enabled.
; GD is never set.  And a DATA breakpoint can go off under the monitor
; itself, which writes a box's stack for it at every INT: that is a
; debug exception in ring 0, exc_common takes it as the box's
; (vbp_owed), and the box is given its INT 1 on the way back in - after
; the instruction, which is where the processor would have put it.
; DR4 and DR5 are DR6 and DR7.  From ring 3 the registers hold what is
; put in them, and nothing is watched while a client's protected-mode
; code runs.  Nor does a breakpoint see what the DOS side writes for a
; box - the data of a read, say - because by then the box has been
; left and the registers are the kernel's again.
;
; AN INSTRUCTION BREAKPOINT IS A FAULT AND THE REST ARE TRAPS, which
; matters to vf_exc: it ends a box whose handler returns to the same
; fault thirty-two times, and a watchpoint that goes off at one
; instruction in a loop is not that (vbp_fault).
;
; THE MODEL-SPECIFIC REGISTERS ARE THE MACHINE'S, and RDMSR and WRMSR
; are run for the guest as they stand.  Whether the processor has the
; instruction at all, and whether it has that register, only the
; processor says - by faulting - so the one instruction is run under
; exc_guard (EXCEPT.INC) and what it raises is what the guest is given:
; invalid opcode, or general protection through its own vector 0Dh, as
; in real mode.
;
; THE TEST REGISTERS CAN BE READ AND NOT WRITTEN.  A write to TR6 puts
; an entry of the program's choosing into the translation buffer, and
; a write through TR5 a line into the cache; with paging off those are
; tests, and with it on they are the kernel's memory somewhere else.
; So a read is run for the guest, under the same guard - a Pentium has
; no test registers, and says so - and a write is taken and does
; nothing.
;
; A REGISTER THE PROCESSOR HAS NOT GOT - CR1, CR5 to CR7, CR4 on
; anything before the last 486s - is an invalid opcode in real mode,
; and the guest is given the same: exception 6, through its own vector.
;
; INVD AND WBINVD go with them.  WBINVD is run for the guest, since a
; program that changes CD goes on to flush the cache.  INVD is run as
; WBINVD: what it throws away is whatever the cache had not yet
; written, and with a write-back cache that is the kernel's memory as
; much as the program's.
;
; The same routine answers a DPMI client (pm3_0f).
;=====================================================================
CR0_ET          EQU     00000010h
CR0_NW          EQU     20000000h
CR0_CD          EQU     40000000h
CR0_BOX         EQU     CR0_ET OR CR0_NW OR CR0_CD      ; a guest's to change
VFP_MASK        EQU     0Eh             ; MP, EM and TS
VFP_TS          EQU     08h

CPUF_INCR4      EQU     010020DEh       ; CPUID's EDX: VME, DE, PSE, TSC,
                                        ; PAE, MCE, PGE and FXSR, each of
                                        ; which is switched by a bit of CR4

DR6_FIXED       EQU     0FFFF0FF0h      ; the bits of DR6 that read as 1
DR6_LIVE        EQU     0000E00Fh       ; B0-B3, BD, BS, BT: the processor's
DR7_FIXED       EQU     00000400h       ; ...and the one of DR7
DR7_GD          EQU     00002000h
DR7_OFF         EQU     00000400h       ; nothing enabled (NT_DR7OFF)
BOX_TOP         EQU     00110000h       ; a box reaches nothing from here up

VSR_A32         EQU     01h             ; vsr_mode: 32-bit addressing
VSR_BOX         EQU     02h             ; ...and from a box, not ring 3
VSR_GPF         EQU     010Dh           ; vsr_emul: general protection, and
                                        ; the guest's own

;---------------------------------------------------------------------
; vf_0f - a 0Fh opcode in a box.  ESI -> the instruction, ECX = its
;         length up to and including the 0Fh.  The prefixes are read
;         again for the two that matter to an operand in memory.
;---------------------------------------------------------------------
vf_0f:
        mov     byte ptr ds:[vsr_mode],VSR_BOX
        mov     byte ptr ds:[vsr_seg],0
        xor     edx,edx
vf_0fpfx:
        inc     edx
        cmp     edx,ecx
        jae     short vf_0fgo           ; the last of them is the 0Fh
        mov     al,[esi+edx-1]
        cmp     al,67h
        jne     short vf_0fseg
        or      byte ptr ds:[vsr_mode],VSR_A32
        jmp     short vf_0fpfx
vf_0fseg:
        mov     ah,FR_ES
        cmp     al,26h
        je      short vf_0fset
        mov     ah,FR_CS
        cmp     al,2Eh
        je      short vf_0fset
        mov     ah,FR_SS
        cmp     al,36h
        je      short vf_0fset
        mov     ah,FR_DS
        cmp     al,3Eh
        je      short vf_0fset
        mov     ah,FR_FS
        cmp     al,64h
        je      short vf_0fset
        mov     ah,FR_GS
        cmp     al,65h
        jne     short vf_0fpfx
vf_0fset:
        mov     byte ptr ds:[vsr_seg],ah
        jmp     short vf_0fpfx
vf_0fgo:
        lea     ebx,[esi+ecx]           ; ECX has counted the 0Fh
        call    vsr_emul
        jc      short vf_0fno
        add     ecx,edx
        call    v86_skip
        jmp     v86_resume
vf_0fno:
        cmp     eax,13
        je      vf_bad
        movzx   eax,al                  ; 6 or 13, and the guest's own: its
        mov     [ebp+FR_VEC],eax        ; INT 06h or INT 0Dh, if it has one
        jmp     vf_exc

;---------------------------------------------------------------------
; vsr_emul - a 0Fh instruction that faulted in a box or in ring 3.
;
;   EBX -> the byte after the 0Fh.  EBP -> the fault frame, with the
;   guest's registers below it as PUSHAD left them (v86_fault and
;   pm3_fault both).  vsr_mode and vsr_seg say what the caller knows.
;   CF clear: done, and EDX = how many bytes of it follow the 0Fh.
;   CF set: EAX = 13, not an instruction answered here; 6, one this
;   processor has not got; or VSR_GPF, one it refused.  The last two
;   are the guest's to be told.
;   EAX and EDX are used; nothing else is changed.
;---------------------------------------------------------------------
vsr_emul:
        mov     al,[ebx]
        cmp     al,20h                  ; MOV r32,CRn
        je      vsr_get
        cmp     al,22h                  ; MOV CRn,r32
        je      vsr_set
        cmp     al,21h                  ; MOV r32,DRn
        je      vsr_drget
        cmp     al,23h                  ; MOV DRn,r32
        je      vsr_drset
        cmp     al,24h                  ; MOV r32,TRn
        je      vsr_trget
        cmp     al,26h                  ; MOV TRn,r32
        je      vsr_trset
        cmp     al,32h                  ; RDMSR
        je      vsr_rdmsr
        cmp     al,30h                  ; WRMSR
        je      vsr_wrmsr
        cmp     al,01h                  ; the group LMSW is in
        je      vsr_lmsw
        cmp     al,06h                  ; CLTS
        je      short vsr_clts
        cmp     al,08h                  ; INVD
        je      short vsr_flush
        cmp     al,09h                  ; WBINVD
        je      short vsr_flush
vsr_no:
        mov     eax,13
        stc
        ret
vsr_ud:
        mov     eax,6
        stc
        ret
vsr_raised:                             ; EAX = what a guarded one raised
        cmp     eax,6
        je      vsr_ud
        mov     eax,VSR_GPF
        stc
        ret

vsr_flush:
        test    byte ptr ds:[fpu_cpu],FPC_486   ; a 386 has no cache of its
        jz      vsr_ud                          ; own and neither instruction
        db      0Fh,09h                         ; WBINVD
vsr_one:
        mov     edx,1
        clc
        ret

vsr_clts:
        test    byte ptr ds:[vsr_mode],VSR_BOX
        jz      vsr_one
        cmp     byte ptr ds:[vfp_set],0 ; TS is clear, and has never been
        je      vsr_one                 ; anything else
        and     byte ptr ds:[vfp_bits],NOT VFP_TS
        call    vfp_in
        jmp     vsr_one

;--- MOV r32,CRn: the mod r/m byte names the control register in bits
;--- 3 to 5 and the general one in bits 0 to 2
vsr_get:
        movzx   edx,byte ptr [ebx+1]
        shr     edx,3
        and     edx,7
        jz      short vsg_cr0
        cmp     edx,2
        je      short vsg_cr2
        cmp     edx,3
        je      short vsg_cr3
        cmp     edx,4
        jne     vsr_ud
        test    dword ptr ds:[fpu_feat],CPUF_INCR4
        jz      vsr_ud
        db      0Fh,20h,0E0h            ; MOV EAX,CR4
        jmp     short vsg_put
vsg_cr0:
        mov     eax,cr0
        and     eax,NOT VFP_MASK
        push    eax
        call    vfp_view
        pop     edx
        or      eax,edx
        jmp     short vsg_put
vsg_cr2:
        mov     eax,cr2
        jmp     short vsg_put
vsg_cr3:
        mov     eax,cr3
vsg_put:
        push    eax
        mov     al,[ebx+1]
        call    vsr_regp
        pop     eax
        mov     [edx],eax
        mov     edx,2
        clc
        ret

;--- MOV CRn,r32
vsr_set:
        movzx   edx,byte ptr [ebx+1]
        shr     edx,3
        and     edx,7
        jz      short vss_cr0
        cmp     edx,2                   ; CR2 and CR3: the fault it was
        je      vsr_no
        cmp     edx,3
        je      vsr_no
        cmp     edx,4
        jne     vsr_ud
        test    dword ptr ds:[fpu_feat],CPUF_INCR4
        jz      vsr_ud
        mov     edx,2                   ; CR4: taken, and nothing changed
        clc
        ret
vss_cr0:
        push    ecx
        mov     al,[ebx+1]
        call    vsr_regp
        mov     edx,[edx]               ; what the guest wants CR0 to be
        push    edx
        mov     eax,cr0
        xor     edx,eax                 ; of the bits that are its to
        and     edx,CR0_BOX             ; change, the ones that differ -
        xor     edx,eax                 ; EDX = CR0 with those changed
        mov     ecx,edx
        and     ecx,CR0_CD OR CR0_NW
        cmp     ecx,CR0_NW              ; NW without CD is asked for...
        jne     short vss_load
        mov     ecx,eax
        and     ecx,CR0_CD OR CR0_NW
        cmp     ecx,CR0_NW              ; ...which is no change, or else
        je      short vss_load          ; the two stay as they are
        and     edx,NOT (CR0_CD OR CR0_NW)
        or      edx,ecx
vss_load:
        mov     cr0,edx
        pop     eax                     ; ...and its MP, EM and TS
        call    vfp_put
        pop     ecx
        mov     edx,2
        clc
        ret

;--- LMSW r/m16: PE cannot be cleared by it and is never the guest's,
;--- so what it carries is MP, EM and TS
vsr_lmsw:
        mov     al,[ebx+1]
        mov     ah,al
        and     ah,38h
        cmp     ah,30h                  ; /6.  LGDT, LIDT and INVLPG are the
        jne     vsr_no                  ; fault they were
        call    vsr_rmlen
        push    edx
        test    byte ptr ds:[vsr_mode],VSR_BOX
        jz      short vsl_done          ; ring 3: taken, nothing changed
        call    vsr_rmword
        jc      short vsl_done          ; ...and an operand not read here
        call    vfp_put
vsl_done:
        pop     edx
        add     edx,2
        clc
        ret

;--- RDMSR and WRMSR, run for the guest with its own ECX and EDX:EAX
vsr_rdmsr:
        push    ecx
        mov     ecx,[ebp+PA_ECX]
        mov     dword ptr ds:[exc_gesp],esp
        mov     dword ptr ds:[exc_guard],OFFSET vsr_msrno
        db      0Fh,32h                 ; RDMSR
        mov     dword ptr ds:[exc_guard],0
        mov     [ebp+PA_EAX],eax
        mov     [ebp+PA_EDX],edx
        pop     ecx
        jmp     vsr_one
vsr_wrmsr:
        push    ecx
        mov     ecx,[ebp+PA_ECX]
        mov     eax,[ebp+PA_EAX]
        mov     edx,[ebp+PA_EDX]
        mov     dword ptr ds:[exc_gesp],esp
        mov     dword ptr ds:[exc_guard],OFFSET vsr_msrno
        db      0Fh,30h                 ; WRMSR
        mov     dword ptr ds:[exc_guard],0
        pop     ecx
        jmp     vsr_one
vsr_msrno:                              ; from exc_common: ESP as it was
        pop     ecx
        jmp     vsr_raised

;--- MOV r32,DRn.  DR4 and DR5 are other names for DR6 and DR7.
vsr_drget:
        movzx   edx,byte ptr [ebx+1]
        shr     edx,3
        and     edx,7
        cmp     edx,4
        jb      short vdg_addr
        test    dl,1
        jnz     short vdg_7
        mov     eax,dr6                 ; what the processor has seen, and
        and     eax,DR6_LIVE            ; what the guest last left there
        or      eax,dword ptr ds:[vbp_6]
        jmp     vsg_put
vdg_7:
        mov     eax,dword ptr ds:[vbp_7]
        jmp     vsg_put
vdg_addr:
        mov     eax,dword ptr ds:[vbp_reg+edx*4]
        jmp     vsg_put

;--- MOV DRn,r32
vsr_drset:
        mov     al,[ebx+1]
        call    vsr_regp
        mov     eax,[edx]
        movzx   edx,byte ptr [ebx+1]
        shr     edx,3
        and     edx,7
        cmp     edx,4
        jb      short vbs_addr
        test    dl,1
        jnz     short vbs_7
        or      eax,DR6_FIXED
        mov     dword ptr ds:[vbp_6],eax
        xor     eax,eax                 ; the processor's sticky bits are
        mov     dr6,eax                 ; cleared by writing it, so they are
        jmp     short vbs_done
vbs_7:
        and     eax,NOT DR7_GD
        or      eax,DR7_FIXED
        mov     dword ptr ds:[vbp_7],eax
        or      al,al                   ; L0 to G3: is anything enabled
        setnz   byte ptr ds:[vbp_live]
        jmp     short vbs_load
vbs_addr:
        mov     dword ptr ds:[vbp_reg+edx*4],eax
vbs_load:
        test    byte ptr ds:[vsr_mode],VSR_BOX
        jz      short vbs_done          ; ring 3: kept, and nothing watched
        call    vbp_out
        call    vbp_load
vbs_done:
        mov     edx,2
        clc
        ret

;--- MOV r32,TRn and MOV TRn,r32
vsr_trget:
        call    vsr_trread
        jc      vsr_raised
        jmp     vsg_put
vsr_trset:
        call    vsr_trread              ; is there one?  Then it is taken,
        jc      vsr_raised              ; and nothing is written
        mov     edx,2
        clc
        ret

;---------------------------------------------------------------------
; vsr_trread - the test register the mod r/m at [EBX+1] names, read.
;              CF clear and EAX = it; or CF set and EAX = the vector
;              the processor raised.  EDX is used.
;---------------------------------------------------------------------
vsr_trread:
        movzx   edx,byte ptr [ebx+1]
        shr     edx,3
        and     edx,7
        lea     edx,[vsr_trtab+edx*4]
        mov     dword ptr ds:[exc_gesp],esp
        mov     dword ptr ds:[exc_guard],OFFSET vtr_none
        call    edx
        mov     dword ptr ds:[exc_guard],0
        clc
        ret
vtr_none:                               ; from exc_common: ESP as it was
        stc
        ret

vsr_trtab       db      0Fh,24h,0C0h,0C3h       ; MOV EAX,TR0 and RET
                db      0Fh,24h,0C8h,0C3h       ; ...TR1, and so on
                db      0Fh,24h,0D0h,0C3h
                db      0Fh,24h,0D8h,0C3h
                db      0Fh,24h,0E0h,0C3h
                db      0Fh,24h,0E8h,0C3h
                db      0Fh,24h,0F0h,0C3h
                db      0Fh,24h,0F8h,0C3h

;---------------------------------------------------------------------
; vsr_regp - AL = a mod r/m byte.  -> EDX = where the guest's copy of
;            the general register in its low three bits is kept.
;---------------------------------------------------------------------
vsr_regp:
        movzx   edx,al
        and     edx,7
        cmp     edx,4
        je      short vrp_sp
        inc     edx                     ; EAX is the dword below EBP, ECX
        shl     edx,2                   ; the one below that, and so on in
        neg     edx                     ; the order PUSHAD stores them
        add     edx,ebp
        ret
vrp_sp:
        lea     edx,[ebp+FR_ESP]        ; ...but ESP is the one the
        ret                             ; processor stacked

;---------------------------------------------------------------------
; vsr_rmlen - AL = the mod r/m at [EBX+1].  -> EDX = how many bytes of
;             the instruction follow it: a SIB, a displacement.
;---------------------------------------------------------------------
vsr_rmlen:
        push    eax
        xor     edx,edx
        cmp     al,0C0h
        jae     short vrn_out           ; a register
        test    byte ptr ds:[vsr_mode],VSR_A32
        jnz     short vrn_32
        mov     ah,al
        and     ah,0C7h
        cmp     ah,06h                  ; mod 00, r/m 110: an address alone
        je      short vrn_two
        shr     al,6                    ; ...or mod is the count: 0, 1 or 2
        movzx   edx,al
        jmp     short vrn_out
vrn_two:
        mov     edx,2
        jmp     short vrn_out
vrn_32:
        mov     ah,al
        and     ah,7
        cmp     ah,4
        jne     short vrn_nosib
        inc     edx                     ; a SIB, whose base 101 with mod 00
        cmp     al,40h                  ; is an address alone
        jae     short vrn_mod
        mov     ah,[ebx+2]
        and     ah,7
        jmp     short vrn_alone
vrn_nosib:
        cmp     al,40h
        jae     short vrn_mod
vrn_alone:
        cmp     ah,5                    ; mod 00, r/m (or base) 101
        jne     short vrn_out
        add     edx,4
        jmp     short vrn_out
vrn_mod:
        inc     edx                     ; mod 01: one byte
        cmp     al,80h
        jb      short vrn_out
        add     edx,3                   ; mod 10: four
vrn_out:
        pop     eax
        ret

;---------------------------------------------------------------------
; vsr_rmword - the 16-bit operand the mod r/m at [EBX+1] names, for a
;              box.  CF clear and AX = it.  CF set: an operand in
;              memory with 32-bit addressing, which is not worked out
;              here - the instruction is then taken and changes nothing.
;
; The address is the 8086's: two registers of BX, BP, SI and DI or
; one, a displacement, and SS where BP is in it unless a prefix names
; another (vsr_seg).  It wraps at 64K, and is the box's own memory.
;---------------------------------------------------------------------
vsr_rmword:
        mov     al,[ebx+1]
        cmp     al,0C0h
        jb      short vmw_mem
        push    edx
        call    vsr_regp
        mov     ax,[edx]
        pop     edx
        clc
        ret
vmw_mem:
        test    byte ptr ds:[vsr_mode],VSR_A32
        jnz     short vmw_not
        push    ecx
        push    edx
        push    esi
        movzx   ecx,al
        and     ecx,7
        lea     esi,[vsr_ea16+ecx*4]
        xor     edx,edx                 ; DX = the offset
        mov     ah,al
        and     ah,0C7h
        cmp     ah,06h                  ; an address alone, in DS
        jne     short vmw_regs
        mov     dx,[ebx+2]
        mov     cl,FR_DS
        jmp     short vmw_seg
vmw_regs:
        movsx   ecx,byte ptr [esi]
        mov     dx,[ebp+ecx]
        movsx   ecx,byte ptr [esi+1]
        jecxz   short vmw_disp
        add     dx,[ebp+ecx]
vmw_disp:
        cmp     al,40h
        jb      short vmw_which
        cmp     al,80h
        jae     short vmw_d16
        movsx   cx,byte ptr [ebx+2]
        add     dx,cx
        jmp     short vmw_which
vmw_d16:
        add     dx,[ebx+2]
vmw_which:
        mov     cl,FR_DS
        cmp     byte ptr [esi+2],0
        je      short vmw_seg
        mov     cl,FR_SS
vmw_seg:
        cmp     byte ptr ds:[vsr_seg],0
        je      short vmw_read
        mov     cl,byte ptr ds:[vsr_seg]
vmw_read:
        movzx   ecx,cl
        movzx   ecx,word ptr [ebp+ecx]  ; the segment, a paragraph
        shl     ecx,4
        movzx   edx,dx
        mov     ax,[ecx+edx]
        pop     esi
        pop     edx
        pop     ecx
        clc
        ret
vmw_not:
        stc
        ret

; The eight r/m forms: where below EBP the first register is kept, the
; second or 0 for none, and whether the segment is SS.  BX is the dword
; at -16, BP at -24, SI at -28 and DI at -32 (PUSHAD).
vsr_ea16        db      -16,-28,0,0             ; BX+SI
                db      -16,-32,0,0             ; BX+DI
                db      -24,-28,1,0             ; BP+SI
                db      -24,-32,1,0             ; BP+DI
                db      -28,0,0,0               ; SI
                db      -32,0,0,0               ; DI
                db      -24,0,1,0               ; BP
                db      -16,0,0,0               ; BX

;=====================================================================
; A BOX'S MP, EM AND TS.
;
; vfp_view - EAX = the three bits the caller runs under.  For a box
;            that is what CR0 holds, its own or not (vfp_in).  For a
;            DPMI client it is too, except that with X87EMU.SYS loaded
;            EM is set for native programs and cleared at the client's
;            first coprocessor instruction (fpu_lend), so the EM shown
;            is the BIOS's.
; vfp_put  - AL = a machine status word a box has loaded.  Its MP, EM
;            and TS become the box's if they are not what it already
;            runs under.  From ring 3 nothing is done.
; vfp_in   - into CR0, on the way into a box.  A box that has asked
;            for nothing has the coprocessor lent to it here and not
;            at its first instruction, so that SMSW, which nothing
;            can answer for the processor, says what a move from CR0
;            does: a program that reads the one and loads the other
;            back has then asked for nothing.
; vfp_out  - and the kernel's back, on the way out.
; Everything preserved by the last three; vfp_view changes EAX only.
;=====================================================================
vfp_view:
        mov     eax,cr0
        test    byte ptr ds:[vsr_mode],VSR_BOX
        jnz     short vfv_out
        test    byte ptr ds:[fpu_flags],FPUF_EMU
        jz      short vfv_out
        and     eax,NOT CR0_EM
        push    edx
        mov     edx,dword ptr ds:[fpu_cr0boot]
        and     edx,CR0_EM
        or      eax,edx
        pop     edx
vfv_out:
        and     eax,VFP_MASK
        ret

vfp_put:
        test    byte ptr ds:[vsr_mode],VSR_BOX
        jz      short vfp_ret
        push    eax
        push    edx
        mov     dl,al
        and     dl,VFP_MASK
        call    vfp_view
        cmp     al,dl
        je      short vfp_same
        mov     byte ptr ds:[vfp_bits],dl
        mov     byte ptr ds:[vfp_set],1
        call    vfp_in
vfp_same:
        pop     edx
        pop     eax
vfp_ret:
        ret

vfp_in:
        cmp     byte ptr ds:[vfp_set],0
        jne     short vfp_own
        test    byte ptr ds:[fpu_flags],FPUF_EMU
        jz      short vfo_in
        test    byte ptr ds:[fpu_cr0boot],CR0_EM
        jnz     short vfo_in            ; the BIOS's EM: the box's INT 7
        call    fpu_lend
        ret
vfp_own:
        push    eax
        mov     eax,cr0
        and     eax,NOT VFP_MASK
        or      al,byte ptr ds:[vfp_bits]
        mov     cr0,eax
        pop     eax
vfo_in:
        ret

vfp_out:
        cmp     byte ptr ds:[vfp_set],0
        je      short vfo_out
        push    eax
        push    edx
        mov     eax,cr0
        and     eax,NOT VFP_MASK
        mov     edx,dword ptr ds:[fpu_cr0run]
        and     edx,VFP_MASK
        or      eax,edx
        mov     cr0,eax
        mov     byte ptr ds:[fpu_lent],0        ; EM is FPU.INC's own again
        pop     edx
        pop     eax
vfo_out:
        ret

;=====================================================================
; A BOX'S DEBUG REGISTERS.
;
; vbp_load  - the box's four addresses and its DR7 into the processor,
;             if its DR7 enables anything.  The null guard is off by
;             now (v86_run), so DR7 holds nothing while they change.
; vbp_out   - and the kernel's four addresses back, with DR7 holding
;             nothing: nt_arm puts the guard on after it.  What the
;             processor saw go off is kept for the box's DR6.
; vbp_fault - CF set if the debug exception in hand is one of the
;             box's instruction breakpoints, which is a fault and
;             comes back to the same place; CF clear for the rest,
;             which are traps.
; Everything preserved.
;=====================================================================
vbp_load:
        cmp     byte ptr ds:[vbp_live],0
        je      short vbl_out
        push    eax
        push    ecx
        push    edx
        mov     edx,dword ptr ds:[vbp_7]
        mov     ecx,3
vbl_one:
        cmp     dword ptr ds:[vbp_reg+ecx*4],BOX_TOP
        jb      short vbl_next
        mov     eax,3                   ; not where a box reaches: its L and
        shl     eax,cl                  ; G bits come off.  They are two
        shl     eax,cl                  ; apiece, hence the shift twice
        not     eax
        and     edx,eax
vbl_next:
        dec     ecx
        jns     short vbl_one
        mov     eax,dword ptr ds:[vbp_reg]
        mov     dr0,eax
        mov     eax,dword ptr ds:[vbp_reg+4]
        mov     dr1,eax
        mov     eax,dword ptr ds:[vbp_reg+8]
        mov     dr2,eax
        mov     eax,dword ptr ds:[vbp_reg+12]
        mov     dr3,eax
        mov     dr7,edx
        mov     byte ptr ds:[vbp_in],1
        pop     edx
        pop     ecx
        pop     eax
vbl_out:
        ret

vbp_out:
        cmp     byte ptr ds:[vbp_in],0
        je      short vbo_out
        push    eax
        mov     eax,DR7_OFF
        mov     dr7,eax
        mov     eax,dr6
        push    eax
        and     eax,0Fh
        or      dword ptr ds:[vbp_6],eax
        pop     eax
        and     al,0F0h
        mov     dr6,eax
        xor     eax,eax                 ; the guard's addresses (nt_init)
        mov     dr0,eax
        mov     al,4
        mov     dr1,eax
        mov     al,8
        mov     dr2,eax
        mov     al,12
        mov     dr3,eax
        mov     byte ptr ds:[vbp_in],0
        pop     eax
vbo_out:
        ret

vbp_fault:
        cmp     byte ptr ds:[vbp_in],0
        je      short vbg_trap
        push    eax
        push    ecx
        push    edx
        mov     eax,dr6
        mov     edx,dword ptr ds:[vbp_7]
        shr     edx,16                  ; RW and LEN, four bits a breakpoint
        mov     ecx,4
vbg_one:
        shr     eax,1                   ; B0, then B1...
        jnc     short vbg_next
        test    dl,3                    ; RW 00: on execution
        jz      short vbg_is
vbg_next:
        shr     edx,4
        loop    vbg_one
        pop     edx
        pop     ecx
        pop     eax
vbg_trap:
        clc
        ret
vbg_is:
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

vsr_mode        db      0               ; VSR_*, for the instruction in hand
vsr_seg         db      0               ; ...its segment prefix: FR_ES and so
                                        ; on, 0 for none
vfp_set         db      0               ; a box has MP, EM and TS of its own
vfp_bits        db      0               ; ...and these are they
vbp_live        db      0               ; a box's DR7 enables a breakpoint
vbp_in          db      0               ; the processor's are the box's now
vbp_owed        db      0               ; one went off under the monitor
                db      0
vbp_reg         dd      4 dup(0)        ; a box's DR0 to DR3
vbp_6           dd      DR6_FIXED       ; ...its DR6
vbp_7           dd      DR7_FIXED       ; ...and its DR7, as it wrote it

;=====================================================================
; IN and OUT.
;
; Only the three ports claimed in v86_init ever arrive here - every
; other port in the machine still belongs to the guest, at full speed,
; with no emulation in the way.  That is the whole reason the bitmap
; exists rather than a blanket IOPL trap: a DOS program driving a
; sound card or a serial port should not be paying for the A20 gate.
;
; Eight opcodes, and they divide into two questions: where is the port
; number (an immediate byte, or DX) and how wide is the transfer.  The
; wide forms are done as two byte transfers through the same routines,
; because that is what the bus does and because it means a word access
; that straddles into a claimed port is still emulated correctly.
;=====================================================================
vf_inb_i:
        movzx   edx,byte ptr [esi+ecx]  ; the port is the immediate byte
        inc     ecx
        call    v86_skip
        call    vio_in
        mov     byte ptr [ebp+PA_EAX],al
        jmp     v86_resume

vf_inw_i:
        movzx   edx,byte ptr [esi+ecx]
        inc     ecx
        call    v86_skip
        jmp     short vio_inw

vf_inb_d:
        call    v86_skip
        mov     edx,[ebp+PA_EDX]
        movzx   edx,dx
        call    vio_in
        mov     byte ptr [ebp+PA_EAX],al
        jmp     v86_resume

vf_inw_d:
        call    v86_skip
        mov     edx,[ebp+PA_EDX]
        movzx   edx,dx
vio_inw:
        call    vio_in
        mov     byte ptr [ebp+PA_EAX],al
        inc     edx
        call    vio_in
        mov     byte ptr [ebp+PA_EAX+1],al
        jmp     v86_resume

vf_outb_i:
        movzx   edx,byte ptr [esi+ecx]
        inc     ecx
        call    v86_skip
        mov     al,byte ptr [ebp+PA_EAX]
        call    vio_out
        jmp     v86_resume

vf_outw_i:
        movzx   edx,byte ptr [esi+ecx]
        inc     ecx
        call    v86_skip
        jmp     short vio_outw

vf_outb_d:
        call    v86_skip
        mov     edx,[ebp+PA_EDX]
        movzx   edx,dx
        mov     al,byte ptr [ebp+PA_EAX]
        call    vio_out
        jmp     v86_resume

vf_outw_d:
        call    v86_skip
        mov     edx,[ebp+PA_EDX]
        movzx   edx,dx
vio_outw:
        mov     al,byte ptr [ebp+PA_EAX]
        call    vio_out
        inc     edx
        mov     al,byte ptr [ebp+PA_EAX+1]
        call    vio_out
        jmp     v86_resume

;---------------------------------------------------------------------
; vio_in - DX = port, returns AL.
;
; The keyboard controller is SNOOPED, not emulated.  A DOS box reading
; scancodes off 60h or spinning on the status bits of 64h is entitled
; to an answer from the actual chip, and standing in for an 8042 is a
; job nobody should take on for the sake of one bit.  So everything
; goes through to the hardware except the two moments where the
; conversation is about A20, which the monitor has to answer itself
; because the real gate is not where the guest thinks it is.
;---------------------------------------------------------------------
vio_in:
IFDEF V86_KBIO
        call    vio_in1
        call    vio_traceI
        ret
vio_in1:
ENDIF
        cmp     dx,PORT_SYSCTL
        je      vio_i92
        cmp     dx,PORT_KBDATA
        je      vio_i60
        cmp     dx,PORT_KBCMD
        je      vio_i64
        call    vio_ispic
        je      vpc_in
        jmp     vdma_in                 ; the DMA controllers, and anything
                                        ; else goes to the hardware

vio_i92:
        mov     ah,byte ptr ds:[vio_porta]      ; whatever it last wrote...
        and     ah,NOT 02h
        call    pg_a20get                       ; ...but OUR bit 1
        shl     al,1
        or      al,ah
        ret

vio_i60:
        ; A read of 60h is a scancode unless a D0h - "give me the
        ; output port" - is outstanding, in which case the byte the
        ; guest is about to get is the one carrying A20 on bit 1.
        cmp     byte ptr ds:[vio_kbdrd],0
        jne     vio_i60out

        ; THE SCAN CODE IS NOT ON THE PORT ANY MORE.  IRQ 1 belongs to
        ; the kernel, and its handler reads 60h the instant the key
        ; arrives - which it must, or the 8042 would go on asserting
        ; the line - so by the time the guest's own INT 09h gets here
        ; the controller has nothing left to say.  Reading it for real
        ; would give the guest whatever byte happened to be sitting in
        ; the register: usually the right one, on the strength of the
        ; same code having been left there, which is a coincidence and
        ; not an answer.
        ;
        ; So the byte is the one latched when the interrupt was
        ; delivered, and it is returned as often as it is asked for -
        ; see kbq_take for why that repeat matters.
        ;
        ; But only when the byte the guest is after is a KEYBOARD byte.
        ; A reply the guest asked the controller for goes straight
        ; back to it, and so does anything the MOUSE has sent, which
        ; the status port distinguishes with a bit of its own: the
        ; kernel has taken the keyboard away from the box and nothing
        ; else, so standing in for the rest would be answering for
        ; devices it is not driving.
        cmp     byte ptr ds:[vio_kbdthru],0
        je      vio_i60aux
        mov     byte ptr ds:[vio_kbdthru],0
        jmp     short vio_ireal
vio_i60aux:
        ; ...except when the kernel is driving the mouse itself: then a
        ; mouse byte is the driver's (irq_mouse), and a guest that read
        ; it would take one byte out of the middle of a packet.
        cmp     byte ptr ds:[ms_hw],MS_PS2
        je      vio_i60kbd
        push    edx
        mov     dx,PORT_KBCMD
        in      al,dx
        pop     edx
        and     al,KBS_OBF OR KBS_AUXB
        cmp     al,KBS_OBF OR KBS_AUXB
        je      vio_ireal               ; the mouse is talking, not us
vio_i60kbd:

        cmp     byte ptr ds:[kb_lvalid],0
        je      vio_ireal
        mov     byte ptr ds:[kb_obf],0  ; reading it clears the status bit -
        mov     al,byte ptr ds:[kb_latch]       ; but not the register
        ret
vio_i60out:
        mov     byte ptr ds:[vio_kbdrd],0
        call    pg_a20get
        shl     al,1
        ; Bit 0 set means "CPU not in reset", which is a safe thing to
        ; be telling anybody.  The other bits are undefined on an AT
        ; and no A20 routine reads them: the sequence is read, change
        ; bit 1, write back, and the write is intercepted too.
        or      al,0Dh
        ret
vio_ireal:
        in      al,dx
        ret

vio_i64:
        ; The status register, passed through - except that a swallowed
        ; D0h has to look like it worked.  The canonical sequence is
        ; "ask for the output port, wait for bit 0, read 60h", and the
        ; real controller was never asked, so bit 0 would never come up
        ; and a program with an unbounded wait would sit there forever.
        ; Saying the byte is ready is true: it is, and vio_i60 is
        ; holding it.
        in      al,dx
        ; A mouse byte waiting for the kernel's driver is not a byte the
        ; guest can have, so the status does not offer it one.
        cmp     byte ptr ds:[ms_hw],MS_PS2
        jne     vio_i64kbd
        test    al,KBS_AUXB
        jz      vio_i64kbd
        and     al,NOT (KBS_OBF OR KBS_AUXB)
vio_i64kbd:
        cmp     byte ptr ds:[vio_kbdrd],0
        jne     vio_i64full
        ; The same is true of a keystroke we delivered.  A handler is
        ; entitled to check that there really is a byte before reading
        ; one, and on the hardware there would be: the interrupt and
        ; the full output buffer are the same event.  Ours says so from
        ; the moment of delivery until the guest reads the port, which
        ; is exactly how long the real bit lasts.
        cmp     byte ptr ds:[kb_obf],0
        je      vio_i64out
vio_i64full:
        or      al,01h
vio_i64out:
        ret

;---------------------------------------------------------------------
; vio_out - DX = port, AL = the byte.
;---------------------------------------------------------------------
vio_out:
IFDEF V86_KBIO
        call    vio_trace
ENDIF
        cmp     dx,PORT_SYSCTL
        je      vio_o92
        cmp     dx,PORT_KBCMD
        je      vio_o64
        cmp     dx,PORT_KBDATA
        je      vio_o60
        call    vio_ispic
        je      vpc_out
        cmp     dx,PIT_CH0
        je      vt_out
        cmp     dx,PIT_CTRL
        je      vt_out
        jmp     vdma_out                ; likewise

;--- vio_ispic - ZF set if DX is one of the four ports of the two
;--- interrupt controllers.
vio_ispic:
        cmp     dx,PIC1_CMD
        je      vip_yes
        cmp     dx,PIC1_DATA
        je      vip_yes
        cmp     dx,PIC2_CMD
        je      vip_yes
        cmp     dx,PIC2_DATA
vip_yes:
        ret

IFDEF V86_KBIO
;---------------------------------------------------------------------
; vio_trace - every byte the guest writes to a port the monitor stands
;             in for.  The 8042 is a conversation, not a register, and
;             a conversation is only debuggable as a transcript: which
;             command, in what order, and what was outstanding when the
;             next one arrived.
;
; Reads are traced too, and both are needed.  A guest spinning on a
; status bit makes no DOS calls at all, so the call trace goes silent
; at exactly the moment the interesting thing starts happening, and
; the fault trace drowns in the loop it is trying to show.
;---------------------------------------------------------------------
vio_trace:
        pushad
        mov     byte ptr ds:[vio_tval],al       ; before anything prints:
        mov     word ptr ds:[vio_tport],dx      ; con_str and con_hex both
                                                ; work in EAX, so the byte
                                                ; has to be put down first
        mov     esi,OFFSET msgKbO
        call    con_str
        movzx   eax,word ptr ds:[vio_tport]
        call    con_hex16
        mov     esi,OFFSET msgKbEq
        call    con_str
        movzx   eax,byte ptr ds:[vio_tval]
        call    con_hex8
        mov     esi,OFFSET msgKbCR
        call    con_str
        popad
        ret

vio_traceI:
        ; A GUEST WAITING ON A STATUS BIT READS THE SAME PORT FOREVER,
        ; and one line per read is not a transcript, it is the loop
        ; itself printed out.  A read that says what the last one said
        ; carries nothing, so only the ones that CHANGE get a line -
        ; the same rule the call trace settled on for the same reason.
        pushad
        movzx   ebx,dx
        shl     ebx,8
        movzx   eax,al
        or      ebx,eax
        cmp     ebx,dword ptr ds:[vio_last]
        je      vit_same
        mov     dword ptr ds:[vio_last],ebx
        mov     byte ptr ds:[vio_tval],al
        mov     word ptr ds:[vio_tport],dx
        mov     esi,OFFSET msgKbI
        call    con_str
        movzx   eax,word ptr ds:[vio_tport]
        call    con_hex16
        mov     esi,OFFSET msgKbTo
        call    con_str
        movzx   eax,byte ptr ds:[vio_tval]
        call    con_hex8
        mov     esi,OFFSET msgKbCR
        call    con_str
vit_same:
        popad
        ret

vio_last        dd      -1
vio_tport       dw      0
vio_tval        db      0
msgKbO          db      'kb out ',0
msgKbI          db      'kb in  ',0
msgKbEq         db      '=',0
msgKbTo         db      ' -> ',0
msgKbCR         db      13,10,0
ENDIF

;--- 92h: bit 1 is A20, bit 0 is the CPU reset line ------------------
; NEITHER OF THEM REACHES THE HARDWARE.  The gate is emulated in the
; page tables, and moving the real line would leave the kernel's view
; of memory and the guest's disagreeing about the same addresses; the
; reset line is worse still, because a DOS box asking to restart is
; asking to restart ITSELF and the real port would take the machine
; down, kernel and all.  So the reset bit is dropped and the A20 bit
; in the same write is still honoured, which is what a program that
; writes 02h expects and what one that writes 03h deserves.
vio_o92:
        and     al,0FEh
        mov     byte ptr ds:[vio_porta],al
        shr     al,1
        and     al,01h
        call    pg_a20
        ret

;--- 64h: commands ---------------------------------------------------
; THE KEYBOARD IS THE KERNEL'S; THE REST OF THE CONTROLLER IS STILL
; THE BOX'S.  That line is new in v0.36 and it is drawn command by
; command, because the 8042 is two devices behind one pair of ports
; and only one of them has been taken away.
;
; Delivering IRQ 1 into the box is what forced the question.  The
; guest's INT 09h handler is the ROM's, or chains to it, and the ROM's
; keyboard handler does not merely read the scan code: it talks BACK.
; It disables the keyboard while it works and enables it after, and it
; sends the lock lights their command whenever the shift state
; changes.  Those went to the real 8042 - the one this kernel is
; taking scan codes off and standing in for on port 60h - so the two
; halves of one conversation were happening on different devices.  The
; keyboard stopped scanning to wait for a byte nobody was going to
; send it, and the machine lost its keyboard: not the box's, the
; MACHINE'S, because there is only one and the shell was using it too.
;
; The first fix was to swallow every command, and it was too broad by
; exactly one device.  MSD asks the ROM whether there is a PS/2 mouse;
; the ROM answers by sending D4h and then FFh - "reset, and that byte
; is for the auxiliary device" - and a monitor that treats every write
; to port 60h as a keyboard command replies with the keyboard's ACK to
; a question the mouse was asked.  The BIOS then waits for a self-test
; result that is never coming, in a loop with no DOS call in it, and
; the box is gone.  A trace of the port transcript is what says so;
; nothing else could have.
;
; So: the three that would cost the kernel its keyboard are refused,
; and everything else goes to the hardware it was addressed to.
vio_o64:
        cmp     al,KBC_WROUT
        je      vio_c_d1
        cmp     al,KBC_RDOUT
        je      vio_c_d0
        cmp     al,KBC_A20OFF
        je      vio_c_dd
        cmp     al,KBC_A20ON
        je      vio_c_df

        mov     byte ptr ds:[vio_kbdwr],0       ; any other command ends
        mov     byte ptr ds:[vio_kbdrd],0       ; a half-done sequence
        mov     byte ptr ds:[vio_kbdcmd],0
        mov     byte ptr ds:[vio_kbdaux],0

        ; The refusals.  A box that switches the machine's keyboard off
        ; is the same offence as one that switches the timer off, and
        ; F0h-FFh pulse the output lines - FEh being the one that
        ; resets the CPU, which port 92h bit 0 is already refused for.
        cmp     al,KBC_KBDOFF
        je      vio_c_drop
        cmp     al,KBC_KBDON
        je      vio_c_drop
        cmp     al,KBC_PULSE
        jae     vio_c_drop

        ; AND THE MOUSE, once the kernel drives it.  It is the same
        ; argument one device along: a box that disables the aux port,
        ; fakes a mouse byte, or sends the mouse a reset has taken the
        ; machine's mouse away from everything else on it.  A command
        ; for the mouse (D4h) is swallowed WITH its data byte and never
        ; answered, which is exactly what a machine without a PS/2 port
        ; does - so a program probing for one concludes there is none,
        ; and uses INT 33h.
        cmp     byte ptr ds:[ms_hw],MS_PS2
        jne     vio_c_notours
        cmp     al,KBC_AUXOFF
        je      vio_c_drop
        cmp     al,KBC_AUXON
        je      vio_c_drop
        cmp     al,KBC_WRAUXOB
        je      vio_c_drop
        cmp     al,KBC_WRAUX
        jne     vio_c_notours
        mov     byte ptr ds:[vio_kbdaux],2
        ret
vio_c_notours:

        ; ...and the two whose DATA byte decides whose it is
        cmp     al,KBC_WRCMD
        je      vio_c_60
        cmp     al,KBC_WRAUX
        je      vio_c_d4

        ; Everything else is the controller's own business - the self
        ; tests, the aux interface, reading the command byte back - and
        ; whatever it answers with belongs to the guest that asked.
        mov     byte ptr ds:[vio_kbdthru],1
        out     dx,al
        ret
vio_c_drop:
        ret
vio_c_60:
        mov     byte ptr ds:[vio_kbdcmd],1
        out     dx,al
        ret
vio_c_d4:
        mov     byte ptr ds:[vio_kbdaux],1
        out     dx,al
        ret

; D1h and D0h are SWALLOWED rather than passed on.  Sending D1h to the
; real 8042 would leave it waiting for a data byte that is never going
; to arrive, and the next ordinary keystroke would be eaten as the
; answer - the controller would be wedged by our own interception.
vio_c_d1:
        mov     byte ptr ds:[vio_kbdwr],1
        ret
vio_c_d0:
        mov     byte ptr ds:[vio_kbdrd],1
        ret
vio_c_dd:
        xor     al,al
        call    pg_a20
        ret
vio_c_df:
        mov     al,1
        call    pg_a20
        ret

;--- 60h: data -------------------------------------------------------
; FOUR things can be written here, and what the guest last sent to 64h
; is what tells them apart: after a D1h this is the output port and
; carries A20, after a 60h it is the controller's command byte, after
; a D4h it is the mouse's, and after none of those it is a command to
; the KEYBOARD.
;
; Only the last is refused, and that is where the lock lights are.  It
; is ANSWERED rather than dropped: the reply is the ACK the keyboard
; would have sent, AND IT IS DELIVERED AS AN INTERRUPT, because that
; is how it would have arrived.  The ROM's routine sends the command,
; enables interrupts, and waits for its own INT 09h to see the FAh and
; set a flag; latching the byte without raising the interrupt would
; leave it waiting exactly as long as forwarding the command did.
vio_o60:
        cmp     byte ptr ds:[vio_kbdwr],0
        je      vio_o60c
        mov     byte ptr ds:[vio_kbdwr],0
        shr     al,1                    ; bit 1 is A20; bit 0 is reset,
        and     al,01h                  ; and 92h's reasoning applies
        call    pg_a20
        ret
vio_o60c:
        cmp     byte ptr ds:[vio_kbdcmd],0
        je      vio_o60a
        mov     byte ptr ds:[vio_kbdcmd],0
        ; The command byte goes through, with the two bits that would
        ; cost the kernel its keyboard put back the way they have to
        ; be.  A guest is welcome to the mouse interrupt and the rest;
        ; it is not welcome to turn IRQ 1 off underneath the machine.
        or      al,KBCB_KBDINT
        and     al,NOT KBCB_KBDOFF
        cmp     byte ptr ds:[ms_hw],MS_PS2      ; and the mouse's two, when
        jne     vio_o60cout                     ; it is the kernel's
        or      al,KBCB_AUXINT
        and     al,NOT KBCB_AUXOFF
vio_o60cout:
        out     dx,al
        ret
vio_o60a:
        cmp     byte ptr ds:[vio_kbdaux],0
        je      vio_o60k
        cmp     byte ptr ds:[vio_kbdaux],2
        mov     byte ptr ds:[vio_kbdaux],0
        je      vio_o60swallow          ; a refused D4h's data byte
        out     dx,al                   ; the mouse's, and the mouse is not
vio_o60swallow:                         ; a device this kernel has taken -
        ret                             ; unless it is
vio_o60k:
        mov     al,0FAh                 ; ACK, through the same path a scan
        call    kbq_put                 ; code takes
        ret

;--- INT n ------------------------------------------------------------
vf_int:
        mov     bl,[esi+ecx]            ; the vector byte
        inc     ecx
        call    v86_skip
        jmp     short v86_doint
vf_int3:
        mov     bl,3
        call    v86_skip
        jmp     short v86_doint

;--- INTO -------------------------------------------------------------
; The last of the INT family, and it was falling into vf_bad - so a
; guest that used the overflow trap at all was killed for it, and the
; message said "faulted" rather than "I do not know that instruction".
; One byte, and the flag it tests is a real one: arithmetic flags are
; the CPU's own in V86 mode, not something the monitor shadows.
vf_into:
        call    v86_skip                ; either way, INTO is finished with
        test    dword ptr [ebp+FR_FLAGS],EFL_OF
        jz      v86_resume
        mov     bl,4
        ; fall through

;---------------------------------------------------------------------
; v86_doint - BL = vector.  Either the caller wants it or the guest's
;             own IVT does.
;---------------------------------------------------------------------
v86_doint:
        movzx   eax,bl
        mov     edi,dword ptr ds:[v86_ctx]
        add     edi,V86C_TRAP
        mov     ecx,eax
        shr     ecx,3
        add     edi,ecx                 ; the byte holding this vector's bit
        mov     ecx,eax
        and     cl,7
        mov     dl,1
        shl     dl,cl
        test    byte ptr [edi],dl
        jz      v86_reflect             ; not ours - give it to the guest

        movzx   ebx,bl
        mov     eax,V86R_INT
        jmp     v86_leave

;=====================================================================
; vf_exc - an exception that REAL MODE delivers through the IVT, and
;          that the monitor therefore has no business eating.
;
; v0.34 and everything before it treated any fault that was not a #GP
; as the end of the DOS box.  That is right for a page fault and for
; the segment-and-stack family, which cannot happen to an 8086 and
; mean the guest has got somewhere it should not be - but vectors 0
; to 7 are ORDINARY on a real machine.  A 16-bit program that divides
; by zero gets INT 00h.  One that runs BOUND gets INT 05h.  And one
; that wants to know whether the CPU it is on has an instruction runs
; the instruction with its own handler on INT 06h and sees whether it
; comes back - which is how EVERY CPU detector written before CPUID
; was universal asks the question, QCONFIG on a 486 among them.
;
; So the answer to "QCONFIG crashed with vector 06" is that QCONFIG
; did not crash.  It asked a question, the way DOS software has always
; asked it, and the monitor answered by killing it.  The program never
; found out that it had been given the reply it was waiting for.
;
; Two guards, because reflecting blind is worse than not reflecting:
;
;   THE VECTOR MUST POINT SOMEWHERE.  A slot left at 0000:0000 would
;   send the guest to linear zero to execute the interrupt vector
;   table as instructions.
;
;   AND THE HANDLER MUST GET SOMEWHERE.  A fault pushes the address of
;   the instruction that faulted, so a handler which IRETs without
;   stepping over it takes the same fault again, for ever - and that
;   is not a hypothetical, it is precisely what the ROM's own dummy
;   INT 06h does.  Real hardware hangs there.  This kernel must not:
;   a spinning guest makes no DOS calls, so nothing ever looks at the
;   ^C flag, and the only way out of it is the power switch.  Count
;   the identical faults and give up while somebody can still be told
;   why.  The count is reset in v86_run, so it measures faults with no
;   progress BETWEEN them rather than faults over the program's life.
;=====================================================================
V86_EXCREFL     EQU     0FBh            ; 0,1,3,4,5,6,7 - every vector
                                        ; below 8 except #DF, which is
                                        ; nobody's idea of recoverable
V86_FLTMAX      EQU     32

vf_exc:
        ; A DEBUG EXCEPTION IS COUNTED ONLY IF IT IS A FAULT.  A single
        ; step and a data breakpoint are traps: the instruction is done,
        ; and one that comes round again in a loop - a watchpoint on a
        ; counter - is the program getting somewhere.  An instruction
        ; breakpoint whose handler returns without taking it off comes
        ; back to the same place for ever, and is counted like the rest.
        cmp     dword ptr [ebp+FR_VEC],1
        jne     vfe_count
        call    vbp_fault
        jnc     vfe_ivt
vfe_count:
        mov     eax,[ebp+FR_EIP]
        cmp     eax,dword ptr ds:[v86_flteip]
        jne     vfe_moved
        mov     eax,[ebp+FR_CS]
        cmp     eax,dword ptr ds:[v86_fltcs]
        jne     vfe_moved
        mov     eax,[ebp+FR_VEC]
        cmp     eax,dword ptr ds:[v86_fltvec]
        jne     vfe_moved
        cmp     eax,16
        je      vfe_fptime
        inc     dword ptr ds:[v86_fltn]
        cmp     dword ptr ds:[v86_fltn],V86_FLTMAX
        jb      vfe_ivt
vfe_stuck:
        mov     eax,V86R_FLTLOOP        ; stuck: the caller says so and ends
        mov     ebx,[ebp+FR_VEC]        ; the box
        jmp     v86_leave

        ; THE COPROCESSOR'S ERROR IS TIMED, NOT COUNTED.  It is reported
        ; at the instruction AFTER the one that caused it, so a loop
        ; that overflows on every turn and has a handler that deals
        ; with each one faults at one address as often as it likes and
        ; is getting somewhere.  What a stuck one looks like is nothing
        ; else happening at all: two seconds of this fault at this
        ; address, and not one call to DOS in them.
vfe_fptime:
        mov     eax,dword ptr ds:[BDA_TICKS]
        sub     eax,dword ptr ds:[v86_fltwhen]
        js      vfe_moved               ; midnight: the count began again
        cmp     eax,FPU_STUCK
        jb      vfe_ivt
        jmp     short vfe_stuck
vfe_moved:
        mov     eax,dword ptr ds:[BDA_TICKS]
        mov     dword ptr ds:[v86_fltwhen],eax
        mov     eax,[ebp+FR_EIP]
        mov     dword ptr ds:[v86_flteip],eax
        mov     eax,[ebp+FR_CS]
        mov     dword ptr ds:[v86_fltcs],eax
        mov     eax,[ebp+FR_VEC]
        mov     dword ptr ds:[v86_fltvec],eax
        mov     dword ptr ds:[v86_fltn],1
vfe_ivt:
        mov     eax,[ebp+FR_VEC]
        cmp     eax,16
        je      vfe_fperr
        shl     eax,2
        cmp     dword ptr ds:[eax],0
        je      vf_notgp                ; nothing there: it really is a crash
        mov     eax,[ebp+FR_VEC]
        jmp     short v86_reflect

        ; EXCEPTION 16 IS NOT INT 10h.  On the machine a DOS program
        ; was written for, the coprocessor's error comes in on
        ; interrupt line 13 and the vector is 75h - and 10h is the
        ; video BIOS.  So it goes in where the line would have put it,
        ; at once and whatever the box's interrupt flag and mask say:
        ; the instruction it stands on cannot be run until somebody has
        ; dealt with the error, so there is nothing to go back to and
        ; wait in.  See FPU.INC.
vfe_fperr:
        call    vpc_fpvec
        cmp     dword ptr ds:[eax*4],0
        je      vf_notgp
        call    vpc_fptake
        jmp     short v86_reflect

;---------------------------------------------------------------------
; v86_reflect - AL = vector.  Do what the 8086 would have done: stack
;               the flags and the return address, then take CS:IP from
;               the guest's own interrupt vector table at 0:0.
;
; The flags that go on the guest's stack are the ones it believes in,
; and the handler is entered with interrupts "off" - virtually.
;---------------------------------------------------------------------
v86_reflect:
        call    v86_deliver
        jmp     v86_resume

;---------------------------------------------------------------------
; v86_deliver - the body of it, as a subroutine, because a HARDWARE
;               interrupt has to do the same thing to the same kind of
;               frame and there is no second way to be right about it.
;
; EBP -> a frame laid out like the fault frame above.  The one an IRQ
; leaves is that frame less the error code, and v86_irqexit hands over
; an EBP four bytes low so that the FR_* offsets land on it.
;---------------------------------------------------------------------
v86_deliver:
        shl     eax,2                   ; vector * 4 = its IVT slot
        mov     eax,dword ptr ds:[eax]
        ; fall through, with the handler's address

;---------------------------------------------------------------------
; v86_deliverto - the same frame work to an address: EAX = seg:off.
;---------------------------------------------------------------------
v86_deliverto:
        push    eax                     ; where to

        mov     eax,[ebp+FR_FLAGS]
        and     eax,V86_UFLAGS
        or      eax,dword ptr ds:[v86_vpriv]
        or      eax,2                   ; bit 1, as the CPU pushes it
        cmp     byte ptr ds:[v86_vif],0
        je      vr_ioff
        or      eax,EFL_IF
vr_ioff:
        call    v86_push                ; flags
        mov     eax,[ebp+FR_CS]
        call    v86_push
        mov     eax,[ebp+FR_EIP]
        call    v86_push

        mov     byte ptr ds:[v86_vif],0 ; an ISR starts with IF clear

        pop     eax
        movzx   ebx,ax                  ; offset
        mov     [ebp+FR_EIP],ebx
        shr     eax,16                  ; segment
        mov     [ebp+FR_CS],eax
        ret

;=====================================================================
; IRQ 1 INTO THE DOS BOX.
;
; A software interrupt is easy: the guest asked for it, so it is
; standing at an instruction boundary of its own choosing and the
; monitor is already holding its frame.  A HARDWARE interrupt is the
; awkward one, because the guest did not ask and the moment is not its
; choice - and the three places it can be given the interrupt are the
; three places the guest's state is complete and writable:
;
;   THE IRQ ITSELF, if it interrupted the guest.  The frame the CPU
;   pushed is the guest's, VM and all, and rewriting CS:IP in it sends
;   the guest to its handler instead of back where it was.  This is
;   the case that matters, because a program spinning on a flag its
;   own handler sets never faults and so is never seen anywhere else.
;
;   A RESUME OUT OF THE MONITOR, where the guest is about to go back
;   after a fault we emulated.  This is how a key gets in promptly
;   after a CLI has been holding it off: the STI that ends the
;   critical section is itself a fault, and this runs on the way out
;   of it.
;
;   AN ENTRY TO v86_run, where the interrupt arrived while the DOS
;   side had the CPU and there was no guest to interrupt at all.
;
; Two conditions in all three places.  THE GUEST'S OWN INTERRUPT FLAG
; MUST BE SET - the virtual one, not the real one, which is the whole
; reason it is virtualised: a DOS program is entitled to a critical
; section, it just is not entitled to stop the machine's timer to get
; one.  AND THE VECTOR MUST POINT SOMEWHERE, for the reason vf_exc
; gives: an unset slot would send the guest to linear zero to execute
; its own vector table.
;=====================================================================

;---------------------------------------------------------------------
; v86_kbwant - CF clear if a keyboard interrupt is owed to the guest
;              and it can take one now.  Consumes the debt, so a caller
;              that gets CF clear must deliver.
;---------------------------------------------------------------------
v86_kbwant:
        cmp     byte ptr ds:[v86_vif],0
        je      vkw_no
        test    byte ptr ds:[vpc_imr],2 ; the box has masked the keyboard,
        jnz     vkw_no                  ; and means it
        test    byte ptr ds:[vpc_isr],1 ; ...or its timer handler is running,
        jnz     vkw_no                  ; which the 8259 would make it wait
                                        ; for - and whose EOI, non-specific,
                                        ; would otherwise end the key's
        cmp     dword ptr ds:[IVT_INT09],0
        je      vkw_no
        call    kbq_take                ; and it latches the code, because
        jc      vkw_ret                 ; that is what raising the interrupt
                                        ; does on the real part
        or      byte ptr ds:[vpc_isr],2 ; in service until the handler's
                                        ; EOI, which is how that EOI is
                                        ; known not to be some other
                                        ; line's (OR leaves CF clear)
vkw_ret:
        ret
vkw_no:
        stc
        ret

;---------------------------------------------------------------------
; v86_brkwant - CF clear and AL = 1Bh if the Break key is owed to the
;               box's INT 1Bh (kb_break, GV_BOX1B).  Consumes the debt,
;               so a caller that gets CF clear must deliver.  The caller
;               has seen to the guest's interrupt flag.
;---------------------------------------------------------------------
v86_brkwant:
        cmp     byte ptr ds:[GV_BOX1B],GVB_OWED
        jne     vbk_no
        mov     byte ptr ds:[GV_BOX1B],GVB_BOX
        cmp     dword ptr ds:[1Bh * 4],0
        je      vbk_no                  ; nowhere to go: vf_exc says why
        mov     al,1Bh
        clc
        ret
vbk_no:
        stc
        ret

;---------------------------------------------------------------------
; v86_ckkbd - EBP -> a fault frame belonging to the guest.  Deliver a
;             queued keystroke if there is one.
;---------------------------------------------------------------------
v86_ckkbd:
        pushad
        ; A BREAKPOINT OF THE BOX'S THAT WENT OFF UNDER THE MONITOR comes
        ; first, and whatever the box's interrupt flag says: it is a
        ; trap, and the processor would have taken it before anything
        ; else (exc_common, vsr_emul).
        cmp     byte ptr ds:[vbp_owed],0
        je      vck_nodb
        mov     byte ptr ds:[vbp_owed],0
        cmp     dword ptr ds:[1 * 4],0
        je      vck_nodb
        mov     eax,1
        call    v86_deliver
        jmp     vck_out
vck_nodb:
        cmp     byte ptr ds:[v86_irqx],0        ; not into an excursion's
        jne     vck_no                          ; guest: see v86_run
        call    v86_kbwant
        jc      vck_no
        mov     eax,9
        call    v86_deliver
        jmp     short vck_out           ; and the guest is in an ISR now
vck_no:
        ; The Break key, owed to a program's INT 1Bh (v86_brkwant).
        cmp     byte ptr ds:[v86_vif],0
        je      vck_out
        cmp     byte ptr ds:[v86_irqx],0
        jne     vck_card
        call    v86_brkwant
        jc      vck_card
        movzx   eax,al
        call    v86_deliver
        jmp     short vck_out
vck_card:
        ; An interrupt from a card the box is driving, through its own
        ; table like the keystroke.
        call    vpc_boxwant
        jc      vck_mouse
        call    v86_deliver
        jmp     short vck_out
vck_mouse:
        ; A mouse event its handler is owed goes the same way, to the
        ; stub's event entry rather than through the IVT.
        cmp     byte ptr ds:[v86_vif],0
        je      vck_out
        cmp     byte ptr ds:[v86_irqx],0
        jne     vck_out
        call    ms_boxwant
        jc      vck_out
        call    v86_deliverto
vck_out:
        popad
        ret

;---------------------------------------------------------------------
; v86_irqexit - called by irq_common on its way out, with EBP already
;               adjusted so that the FR_* offsets address the frame the
;               IRQ pushed.
;
; The VM test is not a nicety.  Off it hangs the difference between the
; guest's stack and the kernel's, and pushing a return frame onto the
; wrong one would be the monitor corrupting itself with the interrupt
; it was trying to be helpful about.
;---------------------------------------------------------------------
v86_irqexit:
        cmp     byte ptr ds:[v86_active],0
        je      vix_no
        test    dword ptr [ebp+FR_FLAGS],EFL_VM
        jz      vix_no                  ; ring 0: the monitor, not the guest
        push    eax                     ; and nothing goes onto a stack
        push    edx                     ; the guest could not write itself
        mov     eax,[ebp+FR_SS]         ; - it has not trapped, so nobody
        mov     edx,[ebp+FR_ESP]        ; has asked (v86_stkok).  Its next
        call    v86_stkok               ; trap ends it.
        pop     edx
        pop     eax
        jc      vix_no
        call    v86_ckkbd
vix_no:
        ret

;--- CLI / STI --------------------------------------------------------
vf_cli:
        mov     byte ptr ds:[v86_vif],0
        call    v86_skip
        jmp     v86_resume
vf_sti:
        mov     byte ptr ds:[v86_vif],1
        call    v86_skip
        jmp     v86_resume

;--- PUSHF / POPF -----------------------------------------------------
; WITH A 66h IN FRONT THEY ARE PUSHFD AND POPFD, four bytes and not two,
; and the decoder used to step over the prefix and do the two-byte
; thing.  The stack then came back two bytes out, and the RET at the
; end of the routine went somewhere else.  DOS/4GW tells a 486 from a
; 386 with exactly that pair, just before it asks for DPMI: every
; DOS/4GW program in a box - DOOM, Rise of the Triad, Magic Carpet -
; returned into its own raw mode switch and died on its LGDT.
;
; Bit 1 is set in every image a guest is shown, as every x86 sets it.
vf_pushf:
        mov     eax,[ebp+FR_FLAGS]
        and     eax,V86_UFLAGS
        or      eax,dword ptr ds:[v86_vpriv]    ; IOPL, NT, and AC and ID
        or      eax,2
        cmp     byte ptr ds:[v86_vif],0
        je      vp_ioff
        or      eax,EFL_IF
vp_ioff:
        cmp     byte ptr ds:[v86_o32],0
        je      vp_16
        push    eax                     ; PUSHFD: the high word first, so
        shr     eax,16                  ; that the dword lies the right way
        call    v86_push                ; round on the guest's stack
        pop     eax
vp_16:
        call    v86_push
        call    v86_skip
        jmp     v86_resume

vf_popf:
        cmp     byte ptr ds:[v86_o32],0
        jne     vpf_32
        call    v86_pop
        call    v86_setflags
        call    v86_skip
        jmp     v86_resume
vpf_32:
        call    v86_pop32
        call    v86_setfl32
        call    v86_skip
        jmp     v86_resume

;--- IRET -------------------------------------------------------------
; ...and IRETD the same: three dwords.  The offset is still an offset
; in a 64K segment, and the CS is still a paragraph.
vf_iret:
        cmp     byte ptr ds:[v86_o32],0
        jne     vfi_32
        call    v86_pop
        movzx   eax,ax
        mov     [ebp+FR_EIP],eax
        call    v86_pop
        movzx   eax,ax
        mov     [ebp+FR_CS],eax
        call    v86_pop
        call    v86_setflags
        jmp     v86_resume
vfi_32:
        call    v86_pop32
        movzx   eax,ax
        mov     [ebp+FR_EIP],eax
        call    v86_pop32
        movzx   eax,ax
        mov     [ebp+FR_CS],eax
        call    v86_pop32
        call    v86_setfl32
        jmp     v86_resume

;---------------------------------------------------------------------
; v86_setflags - AX = flags the guest wants.  Its IF becomes our
;                virtual one, its IOPL and NT become the other shadow,
;                and the rest it may keep.  VM, the real IOPL and the
;                real IF are not up for discussion.
;
; The two shadows are not the same kind of thing and it is worth being
; clear about which is which.  IF is virtualised because the monitor
; must ACT on it - an interrupt arrives and the guest's answer decides
; whether it sees it now or later.  IOPL and NT are virtualised for a
; duller reason: nothing acts on them at all, the guest simply expects
; to read back what it wrote, and a CPU detector is a program whose
; entire method is exactly that.  Storing them costs one dword and
; buys every 386 test in DOS.
;---------------------------------------------------------------------
;
; AC AND ID ARE SHADOWED THE SAME WAY, for the 486 test and the CPUID
; test, which are the same method one bit further up: flip it with
; POPFD, read it back with PUSHFD.  A 16-bit image cannot reach them,
; so v86_setflags keeps whatever the shadow held.  And they stick only
; where the processor's own would (fpu_cpu, from fpu_init): a shadow
; that let AC stick on a 386 would turn it into a 486 to every program
; that asks.
;---------------------------------------------------------------------
v86_setflags:
        push    eax
        push    edx
        movzx   eax,ax
        mov     edx,dword ptr ds:[v86_vpriv]
        and     edx,EFL_AC OR EFL_ID
        or      eax,edx
        call    v86_setfl32
        pop     edx
        pop     eax
        ret

;--- v86_setfl32 - EAX = all 32 bits the guest wants (POPFD, IRETD) ---
v86_setfl32:
        push    eax
        push    edx
        mov     byte ptr ds:[v86_vif],0
        test    ax,EFL_IF
        jz      vs_off
        mov     byte ptr ds:[v86_vif],1
vs_off:
        mov     edx,V86_VFLAGS          ; what it may think it has: IOPL
        test    byte ptr ds:[fpu_cpu],FPC_486   ; and NT always, AC on a
        jz      vs_x                            ; 486, ID where there is a
        or      edx,EFL_AC                      ; CPUID
        test    byte ptr ds:[fpu_cpu],FPC_CPUID
        jz      vs_x
        or      edx,EFL_ID
vs_x:
        push    eax
        and     eax,edx
        mov     dword ptr ds:[v86_vpriv],eax
        pop     eax
        and     eax,V86_UFLAGS          ; and what it actually runs under
        or      eax,EFL_VM OR EFL_IF OR 2
        mov     [ebp+FR_FLAGS],eax
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; v86_skip  - advance the guest's IP over ECX bytes
; v86_push  - AX onto the guest's stack
; v86_pop   - AX off it
;
; The stack pointer is 16-bit and wraps inside the segment, which is
; why the arithmetic is done in DX and written back as a dword: the
; high half of ESP belongs to the guest and must survive untouched.
;---------------------------------------------------------------------
v86_skip:
        push    eax
        mov     eax,[ebp+FR_EIP]
        add     ax,cx
        mov     [ebp+FR_EIP],eax
        pop     eax
        ret

v86_push:
        push    ebx
        push    edx
        mov     edx,[ebp+FR_ESP]
        sub     dx,2
        mov     [ebp+FR_ESP],edx
        movzx   ebx,dx
        mov     edx,[ebp+FR_SS]
        shl     edx,4
        add     ebx,edx
        mov     [ebx],ax
        pop     edx
        pop     ebx
        ret

v86_pop:
        push    ebx
        push    edx
        mov     edx,[ebp+FR_ESP]
        movzx   ebx,dx
        add     dx,2
        mov     [ebp+FR_ESP],edx
        mov     edx,[ebp+FR_SS]
        shl     edx,4
        add     ebx,edx
        mov     ax,[ebx]
        pop     edx
        pop     ebx
        ret

;--- v86_pop32 - EAX off it, as two words, low first -------------------
v86_pop32:
        push    edx
        call    v86_pop
        movzx   edx,ax
        call    v86_pop
        shl     eax,16
        mov     ax,dx
        pop     edx
        ret

;---------------------------------------------------------------------
; v86_resume - back into the guest, exactly where it left off
;---------------------------------------------------------------------
;=====================================================================
; vf_romw - a page fault in a box: is it a STORE INTO THE KERNEL'S LOW
;           PAGES, and can the instruction be stepped over?
;
;   EBP -> the fault frame.  CF clear: the store was dropped and the
;   guest's IP moved past the instruction - the caller resumes it.
;   CF set: something else, and the box ends as it always has.
;   EAX, EBX, ECX, EDX and ESI are trashed (the guest's own are in the
;   frame, which v86_resume puts back).
;
; 1000h-3FFFh is PMIO's stub, the GDT, the globals and XFER, and it is
; read-only to ring 3, the boxes included (pg_protect).  On MS-DOS the
; same addresses are DOS's own low data, which a stray store through a
; segment that is still zero lands in without anybody knowing.
; Wolfenstein 3-D does exactly that as a level ends: a segment variable
; of 0 and XOR [BX+5],AL with AL = 0 - a store that changes nothing on
; a real machine, and here ended the game with the screen left black
; (2026-11-10).  The page stays read-only and the store is dropped, as
; a store into ROM is: the program goes on, the kernel's pages are as
; they were.
;
; ONLY STORES THROUGH A MOD R/M OPERAND are stepped over, because those
; are the ones whose length the operand says and whose effect is the
; store and the flags - and the flags are left as they were, which for
; a stray store into memory that is not the program's is nothing it
; can be relying on.  XCHG, CMPXCHG and XADD write a register too, and
; a string instruction (and REP), a PUSH, POP or CALL onto that memory
; or anything this does not know moves registers of its own: those
; are left to end the box, since stepping over one would change what
; the program goes on with.
;
; Below 110000h is everything a box can reach - the megabyte and, with
; A20 on, the HMA - and the only pages there it may not write are the
; kernel's (or their A20 alias), so the address needs no finer test.
;=====================================================================
vf_romw:
        mov     eax,[ebp+FR_ERR]
        and     eax,3                   ; present, and a write
        cmp     eax,3
        jne     vrw_no
        mov     eax,cr2
        cmp     eax,110000h
        jae     vrw_no

        mov     esi,[ebp+FR_CS]
        shl     esi,4
        movzx   eax,word ptr [ebp+FR_EIP]
        add     esi,eax
        xor     ecx,ecx                 ; the length so far
        xor     edx,edx                 ; DL = operand size 32, DH = address
vrw_pfx:
        cmp     ecx,8
        jae     vrw_no
        mov     al,[esi+ecx]
        inc     ecx
        cmp     al,66h
        je      vrw_o32
        cmp     al,67h
        je      vrw_a32
        cmp     al,26h                  ; segment overrides and LOCK
        je      vrw_pfx
        cmp     al,2Eh
        je      vrw_pfx
        cmp     al,36h
        je      vrw_pfx
        cmp     al,3Eh
        je      vrw_pfx
        cmp     al,64h
        je      vrw_pfx
        cmp     al,65h
        je      vrw_pfx
        cmp     al,0F0h
        je      vrw_pfx
        jmp     short vrw_op
vrw_o32:
        mov     dl,1
        jmp     short vrw_pfx
vrw_a32:
        mov     dh,1
        jmp     short vrw_pfx

        ; AL = the opcode.  EBX = the immediate's bytes after the operand.
vrw_op:
        xor     ebx,ebx
        cmp     al,0Fh
        je      vrw_0f
        cmp     al,38h                  ; 00-37: ADD OR ADC SBB AND SUB XOR,
        jae     vrw_hi                  ; r/m first only (the opcode's low
        mov     ah,al                   ; three bits 0 or 1)
        and     ah,7
        cmp     ah,2
        jae     vrw_no
        jmp     vrw_modrm
vrw_hi:
        cmp     al,80h                  ; group 1, an imm8
        je      vrw_imm8
        cmp     al,82h
        je      vrw_imm8
        cmp     al,83h
        je      vrw_imm8
        cmp     al,81h                  ; group 1, a word or dword
        je      vrw_immw
        cmp     al,88h                  ; MOV r/m,reg
        jb      vrw_no
        cmp     al,89h
        jbe     vrw_modrm
        cmp     al,8Ch                  ; MOV r/m,sreg
        je      vrw_modrm
        cmp     al,0C0h                 ; shifts by an imm8
        je      vrw_imm8
        cmp     al,0C1h
        je      vrw_imm8
        cmp     al,0C6h                 ; MOV r/m,imm
        je      vrw_imm8
        cmp     al,0C7h
        je      vrw_immw
        cmp     al,0D0h                 ; shifts by 1 and by CL
        jb      vrw_no
        cmp     al,0D3h
        jbe     vrw_modrm
        mov     ah,[esi+ecx]            ; the rest are groups: the reg
        shr     ah,3                    ; field says which
        and     ah,7
        cmp     al,0F6h
        je      vrw_grp3
        cmp     al,0F7h
        je      vrw_grp3
        cmp     al,0FEh                 ; INC and DEC
        je      vrw_incdec
        cmp     al,0FFh
        je      vrw_incdec
        jmp     vrw_no
vrw_grp3:
        cmp     ah,2                    ; NOT and NEG store
        je      vrw_modrm
        cmp     ah,3
        je      vrw_modrm
        jmp     vrw_no                  ; TEST, MUL and DIV do not
vrw_incdec:
        cmp     ah,1
        jbe     vrw_modrm
        jmp     vrw_no
vrw_0f:
        mov     al,[esi+ecx]
        inc     ecx
        cmp     al,90h                  ; SETcc
        jb      vrw_no
        cmp     al,9Fh
        jbe     vrw_modrm
        cmp     al,0A4h                 ; SHLD, SHRD by an imm8
        je      vrw_imm8
        cmp     al,0ACh
        je      vrw_imm8
        cmp     al,0BAh                 ; BTS, BTR, BTC by an imm8
        je      vrw_imm8
        cmp     al,0A5h                 ; SHLD, SHRD by CL
        je      vrw_modrm
        cmp     al,0ADh
        je      vrw_modrm
        cmp     al,0ABh                 ; BTS, BTR, BTC
        je      vrw_modrm
        cmp     al,0B3h
        je      vrw_modrm
        cmp     al,0BBh
        je      vrw_modrm
        jmp     vrw_no
vrw_imm8:
        mov     ebx,1
        jmp     short vrw_modrm
vrw_immw:
        mov     ebx,2
        or      dl,dl
        jz      vrw_modrm
        mov     ebx,4

        ; The operand: a mod r/m byte, a SIB byte for some 32-bit forms,
        ; and a displacement.  Mod 3 is a register, which cannot have
        ; faulted on memory.
vrw_modrm:
        cmp     ecx,8
        jae     vrw_no
        mov     al,[esi+ecx]
        inc     ecx
        mov     ah,al
        shr     ah,6                    ; AH = mod
        and     al,7                    ; AL = r/m
        cmp     ah,3
        je      vrw_no
        or      dh,dh
        jnz     vrw_m32
        cmp     ah,1
        je      vrw_d8
        cmp     ah,2
        je      vrw_d16
        cmp     al,6                    ; mod 0, r/m 6: a bare disp16
        jne     vrw_len
vrw_d16:
        add     ecx,2
        jmp     short vrw_len
vrw_m32:
        cmp     al,4
        jne     vrw_nosib
        mov     al,[esi+ecx]            ; the SIB byte, whose base 5 with
        inc     ecx                     ; mod 0 is a bare disp32
        and     al,7
vrw_nosib:
        cmp     ah,1
        je      vrw_d8
        cmp     ah,2
        je      vrw_d32
        cmp     al,5
        jne     vrw_len
vrw_d32:
        add     ecx,4
        jmp     short vrw_len
vrw_d8:
        inc     ecx
vrw_len:
        add     ecx,ebx
        cmp     ecx,15                  ; the processor's own limit
        ja      vrw_no
        add     word ptr [ebp+FR_EIP],cx
        clc
        ret
vrw_no:
        stc
        ret

v86_resume:
        call    v86_ckkbd               ; EBP still points at the frame, and
                                        ; this is the only door back into the
                                        ; guest that a fault can leave by
        popad
        add     esp,8                   ; the vector and error code
        iretd

;=====================================================================
; Data
;=====================================================================
v86_ctx         dd      0               ; the context being run
v86_kesp        dd      0               ; where v86_run must unwind to
v86_reason      dd      0
v86_rvec        dd      0
v86_vpriv       dd      0               ; the guest's virtual IOPL and NT (and
                                        ; AC and ID): what it may read back,
                                        ; never what it runs under - see
                                        ; v86_setflags
v86_o32         db      0               ; the instruction in hand had a 66h
                db      0,0,0
v86_fltcs       dd      -1              ; the last exception reflected into the
v86_flteip      dd      -1              ; guest, and how many times running -
v86_fltvec      dd      -1              ; see vf_exc.  -1 so that the first one
v86_fltn        dd      0               ; can never match a real CS:IP
v86_fltwhen     dd      0               ; the tick that run of them began on
v86_vif         db      0               ; the guest's virtual interrupt flag
v86_active      db      0               ; a guest is executing, as opposed to
                                        ; merely loaded: set on the way into
                                        ; the guest and cleared the moment the
                                        ; monitor takes the CPU back, so that
                                        ; IRQ 1 knows whose keystroke it is
v86_irqx        db      0               ; ...and the guest is an excursion's,
                                        ; run for an interrupt and nobody's
                                        ; program (VPIC.INC, vpc_boxirq), so
                                        ; a keystroke is still the kernel's
vio_porta       db      0               ; port 92h as the guest last wrote it
vio_kbdwr       db      0               ; an 8042 D1h is outstanding
vio_kbdrd       db      0               ; ...or a D0h
vio_kbdcmd      db      0               ; ...or a 60h, and then the next byte
                                        ; to port 60h is the controller's
                                        ; command register
vio_kbdaux      db      0               ; ...or a D4h, and then it is the
                                        ; mouse's
vio_kbdthru     db      0               ; a command went to the real
                                        ; controller, so its reply is the
                                        ; guest's and not a scan code
IFDEF V86_TRACE
msgV86In        db      'V86: about to IRETD into the guest',13,10,0
msgV86Flt       db      'V86: fault vec ',0
msgV86Op        db      ' op ',0
msgV86Ax        db      ' ax ',0
msgV86CR        db      13,10,0
ENDIF

IFDEF V86_CALLS
msgVcInt        db      'int ',0
msgVcAx         db      ' ax=',0
msgVcBx         db      ' bx=',0
msgVcCx         db      ' cx=',0
msgVcDx         db      ' dx=',0
msgVcDs         db      ' ds=',0
msgVcEs         db      ' es=',0
msgVcCR         db      13,10,0
vc_last         dd      -1              ; the last call printed, folded
ENDIF                                   ; into one dword - see vc_show

;=====================================================================
; pm3_fault - a fault taken in ring-3 PROTECTED mode: a DPMI client.
;
; The monitor's whole argument applies again here, one level up.  A
; client could be given IOPL 3, and then CLI, STI, IN and OUT would
; simply work - but IOPL 3 in protected mode hands back every I/O
; port, including the three the A20 gate is reached through, and
; taking those away is what v0.24 was.  So the client runs at IOPL 0,
; those six instructions fault, and they are emulated here through the
; same vio_in and vio_out a DOS box goes through.
;
; The interrupt flag is virtual for the same reason it is in V86: a
; program must not be able to stop the machine's timer.  CLI and STI
; move pm3_vif and nothing else.
;
; Anything that is NOT one of those six is a real fault in the client,
; and the DOS side is told through the hook it registered - it is the
; half that knows there is a DOS box to kill.
;=====================================================================
P3_VEC          EQU     00h
P3_ERR          EQU     04h
P3_EIP          EQU     08h
P3_CS           EQU     0Ch
P3_FLAGS        EQU     10h
P3_ESP          EQU     14h
P3_SS           EQU     18h

pm3_fault:
        pushad
        mov     ebp,esp
        add     ebp,32                  ; EBP -> the vector, as v86_fault does

        cmp     dword ptr [ebp+P3_VEC],13       ; only #GP is ever emulable
        je      pm3_gp

        ; ...and #NM, a coprocessor instruction with EM set: a native
        ; program's goes to the emulator (fpu_nm, below).
        cmp     dword ptr [ebp+P3_VEC],7
        jne     pm3_notnm
        call    fpu_nm
        jc      pm3_real
        xor     ecx,ecx                         ; EIP is already past it
        jmp     pm3_done
pm3_notnm:

        ; The coprocessor's error in a DPMI client is its interrupt 13,
        ; as it is a DOS box's, and the DOS side - which has the
        ; client's vectors - delivers it.  What is kept here is the
        ; interrupt controller the client believes in.
        cmp     dword ptr [ebp+P3_VEC],16
        jne     pm3_real
        test    byte ptr [ebp+P3_CS],4          ; its selectors are in the LDT
        jz      pm3_real
        call    vpc_fptake
        jmp     pm3_real
pm3_gp:

;--- where is the instruction? ---------------------------------------
; CS is one of the client's own selectors, so its base comes out of
; the LDT the same way every other client address does.
        mov     ax,word ptr [ebp+P3_CS]
        call    pm3_base
        jc      pm3_real
        add     ebx,[ebp+P3_EIP]
        mov     esi,ebx
        xor     ecx,ecx                 ; prefix bytes stepped over
pm3_pfx:
        mov     al,[esi]
        cmp     al,66h                  ; operand size - the width is not
        je      pm3_skip                ; something these six care about
        cmp     al,67h
        je      pm3_skip
        cmp     al,0F3h                 ; REP, on a string I/O instruction
        je      pm3_skip
        cmp     al,0F2h
        je      pm3_skip
        jmp     short pm3_op
pm3_skip:
        inc     esi
        inc     ecx
        cmp     ecx,4
        jb      pm3_pfx
        jmp     short pm3_real

pm3_op:
        cmp     al,0FAh
        je      pm3_cli
        cmp     al,0FBh
        je      pm3_sti
        cmp     al,0E4h
        je      pm3_inb_i
        cmp     al,0E5h
        je      pm3_inw_i
        cmp     al,0E6h
        je      pm3_outb_i
        cmp     al,0E7h
        je      pm3_outw_i
        cmp     al,0ECh
        je      pm3_inb_d
        cmp     al,0EDh
        je      pm3_inw_d
        cmp     al,0EEh
        je      pm3_outb_d
        cmp     al,0EFh
        je      pm3_outw_d
        cmp     al,0Fh                  ; the control registers and the
        je      pm3_0f                  ; cache, as in a box: vsr_emul

;--- not ours: the client has really faulted -------------------------
pm3_real:
        cmp     dword ptr ds:[pm3_hook],0
        je      pm3_dead
        mov     eax,[ebp+P3_VEC]
        mov     edx,[ebp+P3_ERR]
        mov     ecx,[ebp+P3_EIP]
        movzx   ebx,word ptr [ebp+P3_CS]
        ; IT COMES BACK FOR ONE THING: an INT n.  A client's software
        ; interrupt through a gate it may not use is a #GP like any
        ; other, and the DOS side - which has the client's vectors -
        ; either points the frame at the client's own handler or runs
        ; the interrupt in real mode and puts the answers in the
        ; PUSHAD block.  That second one is a DOS box running, and
        ; every fault it takes comes through exc_common and leaves ITS
        ; four segment registers where the client's are being kept.
        push    dword ptr ds:[exc_sds]          ; ...and exc_ses
        push    dword ptr ds:[exc_sfs]          ; ...and exc_sgs
        call    dword ptr ds:[pm3_hook]
        pop     dword ptr ds:[exc_sfs]
        pop     dword ptr ds:[exc_sds]
        xor     ecx,ecx                         ; the DOS side has already
        jmp     near ptr pm3_done               ; stepped over it
pm3_dead:
        mov     esi,OFFSET msgPm3
        call    con_str
        mov     eax,[ebp+P3_VEC]
        call    con_hex8
pm3_stop:
        hlt
        jmp     pm3_stop

;--- the 0Fh ones: ESI -> the 0Fh, ECX = the prefixes before it -------
; The addressing is the code segment's - its D bit - turned round by a
; 67h among the prefixes; vsr_emul needs it to measure an operand in
; memory.
pm3_0f:
        mov     byte ptr ds:[vsr_mode],0
        mov     byte ptr ds:[vsr_seg],0
        movzx   eax,word ptr [ebp+P3_CS]
        lar     eax,eax
        jnz     short pm3_0fpfx
        test    eax,00400000h
        jz      short pm3_0fpfx
        mov     byte ptr ds:[vsr_mode],VSR_A32
pm3_0fpfx:
        mov     eax,ecx
pm3_0fone:
        or      eax,eax
        jz      short pm3_0fgo
        mov     edx,esi
        sub     edx,eax
        cmp     byte ptr [edx],67h
        jne     short pm3_0fnext
        xor     byte ptr ds:[vsr_mode],VSR_A32
        jmp     short pm3_0fgo
pm3_0fnext:
        dec     eax
        jmp     short pm3_0fone
pm3_0fgo:
        lea     ebx,[esi+1]
        call    vsr_emul
        jc      short pm3_0fno
        lea     ecx,[ecx+edx+1]         ; the prefixes, the 0Fh, the rest
        jmp     pm3_done
pm3_0fno:
        movzx   eax,al                  ; 13 as it was, or 6, or the 13 the
        mov     [ebp+P3_VEC],eax        ; processor answered with: the
        jmp     pm3_real                ; client's own fault each way

;--- the six that are emulated ---------------------------------------
pm3_cli:
        cmp     byte ptr ds:[pm3_vif],0         ; already off: inside
        je      pm3_cli1                        ; something that ends first
        mov     byte ptr ds:[pm3_vif],0
        mov     dword ptr ds:[pm3_offcnt],0     ; see pm3_ifwatch
        mov     dword ptr ds:[pm3_clia],ebx
        mov     eax,[ebp+P3_ESP]                ; where the stack was
        mov     dx,word ptr [ebp+P3_SS]
        call    pm3_offat
        call    pm3_known                       ; one a POPF undoes: the
        jnc     pm3_cli1                        ; stack says when
        mov     byte ptr ds:[pm3_watch],0       ; any other: it only teaches
pm3_cli1:
        inc     ecx
        jmp     pm3_done
pm3_sti:
        mov     byte ptr ds:[pm3_vif],1
        mov     byte ptr ds:[pm3_watch],0
        mov     dword ptr ds:[pm3_clia],0
        inc     ecx
        jmp     pm3_done

pm3_inb_i:
        movzx   edx,byte ptr [esi+1]
        call    vio_in
        mov     byte ptr [ebp+PA_EAX],al
        add     ecx,2
        jmp     pm3_done
pm3_inw_i:
        movzx   edx,byte ptr [esi+1]
        call    pm3_inw
        add     ecx,2
        jmp     short pm3_done
pm3_outb_i:
        movzx   edx,byte ptr [esi+1]
        mov     al,byte ptr [ebp+PA_EAX]
        call    vio_out
        add     ecx,2
        jmp     short pm3_done
pm3_outw_i:
        movzx   edx,byte ptr [esi+1]
        call    pm3_outw
        add     ecx,2
        jmp     short pm3_done

pm3_inb_d:
        mov     edx,[ebp+PA_EDX]
        movzx   edx,dx
        call    vio_in
        mov     byte ptr [ebp+PA_EAX],al
        inc     ecx
        jmp     short pm3_done
pm3_inw_d:
        mov     edx,[ebp+PA_EDX]
        movzx   edx,dx
        call    pm3_inw
        inc     ecx
        jmp     short pm3_done
pm3_outb_d:
        mov     edx,[ebp+PA_EDX]
        movzx   edx,dx
        mov     al,byte ptr [ebp+PA_EAX]
        call    vio_out
        inc     ecx
        jmp     short pm3_done
pm3_outw_d:
        mov     edx,[ebp+PA_EDX]
        movzx   edx,dx
        call    pm3_outw
        inc     ecx

;--- step over what was emulated and resume the client ---------------
; THE SEGMENT REGISTERS GO BACK FIRST, and they have to: exc_common
; replaced all four with the kernel's in order to be able to run at
; all, and the IRETD below is an inter-privilege return, which NULLS
; every data selector whose DPL is below the level being returned to.
; Ring-3 code resumed with a null DS faults on its next memory
; reference - a #GP with error code zero, pointing at an instruction
; that is perfectly correct.
;
; It cost a ^C.  COMMAND's break handler executes STI on its way home,
; which is emulated here, and the first thing it touched afterwards
; brought the interpreter down.  Every ring-3 fault before that had
; been a real one, which does not come back this way.
;
; DS is loaded LAST because these are read through it; SS is the
; kernel's either way, which is why the reads are SS-relative.
pm3_done:
        add     [ebp+P3_EIP],ecx
        ; An STI is one of the six, and it is the moment a client that
        ; has been holding an interrupt off can be given it.
        push    ebp
        add     ebp,P3_EIP              ; the IRETD frame
        call    vpc_pmchk
        pop     ebp
        mov     es,word ptr ss:[exc_ses]
        mov     fs,word ptr ss:[exc_sfs]
        mov     gs,word ptr ss:[exc_sgs]
        mov     ds,word ptr ss:[exc_sds]
        popad
        add     esp,8                   ; the vector and the error code
        iretd

;=====================================================================
; fpu_nm - exception 7 in ring-3 protected mode, with EBP at the frame
;          and the PUSHAD block below it.  CF clear: the instruction is
;          done with, and the frame's EIP is where to resume.  CF set:
;          the fault is the program's, and the frame's vector says which
;          one it is now.
;
; A native program's selectors are the GDT's; one in the LDT is a DPMI
; client's, which goes the way a DOS box does (IO\FPU.INC, fpu_boxnm).
; For a native program the six segment registers are looked up here,
; where pm3_base is, and the emulator (X87EMU.SYS, DRV\X87EMU\X87EMU.C,
; through fpu_emtrap; FPUF_EMU says it is there) is given the base,
; limit and selector of each.  One that describes nothing is marked so,
; and an operand through it is a general protection fault.
;=====================================================================
fpu_nm:
        test    byte ptr ds:[fpu_flags],FPUF_EMU
        jz      fnm_real
        test    byte ptr [ebp+P3_CS],4
        jz      fnm_native
        call    fpu_boxnm
        ret
fnm_native:
        movzx   eax,word ptr [ebp+P3_CS]
        and     eax,NOT 7
        cmp     eax,GDT_IDX_PROGCS * 8
        jne     fnm_real
        mov     edi,OFFSET fpu_emseg            ; ES CS SS DS FS GS
        mov     ax,word ptr ds:[exc_ses]
        call    fnm_seg
        mov     ax,word ptr [ebp+P3_CS]
        call    fnm_seg
        mov     ax,word ptr [ebp+P3_SS]
        call    fnm_seg
        mov     ax,word ptr ds:[exc_sds]
        call    fnm_seg
        mov     ax,word ptr ds:[exc_sfs]
        call    fnm_seg
        mov     ax,word ptr ds:[exc_sgs]
        call    fnm_seg
        push    ebp
        cld                                     ; the program's DF is in the
        lea     eax,[ebp-32]                    ; frame; the C is written for
        mov     edx,OFFSET fpu_emseg            ; this one
        mov     ebx,OFFSET fpu_emst
        call    dword ptr ds:[fpu_emtrap]       ; x87_trap, in X87EMU.SYS
        pop     ebp
        or      eax,eax
        jz      fnm_done
        mov     [ebp+P3_VEC],eax
        mov     dword ptr [ebp+P3_ERR],0
fnm_real:
        stc
        ret
fnm_done:
        clc
        ret

;--- AX = a selector.  Its record at EDI, and EDI past it.
fnm_seg:
        push    ebx
        mov     word ptr [edi+8],ax
        mov     byte ptr [edi+10],0
        call    pm3_base
        jc      fns_none
        mov     [edi],ebx
        movzx   eax,ax
        lsl     ebx,eax
        jnz     fns_none
        mov     [edi+4],ebx
        mov     byte ptr [edi+10],1
fns_none:
        pop     ebx
        add     edi,X87_SEGREC
        ret

;--- a word port, as two bytes, exactly as the V86 side does it ------
pm3_inw:
        call    vio_in
        mov     byte ptr [ebp+PA_EAX],al
        inc     edx
        call    vio_in
        mov     byte ptr [ebp+PA_EAX+1],al
        dec     edx
        ret
pm3_outw:
        mov     al,byte ptr [ebp+PA_EAX]
        call    vio_out
        inc     edx
        mov     al,byte ptr [ebp+PA_EAX+1]
        call    vio_out
        dec     edx
        ret

;---------------------------------------------------------------------
; pm3_base - AX = a ring-3 selector -> EBX = the base it describes.
;            CF if it is not one, or is not present.
;
; PMIO reads the descriptor table directly rather than asking the DOS
; side: both tables are at fixed addresses (PMDOS.INC) and this runs
; inside a fault handler, which is no place for a call chain.
;
; TWO TABLES, because there are two kinds of ring-3 code.  A DPMI
; client's selectors are its own, in the LDT; a native program's are
; SEL_PROG_CS and SEL_PROG_DS in the GDT.  Everything below the lookup
; is identical, which is the point - a native program's CLI, STI, IN
; and OUT are emulated by the same six cases and for the same reason,
; so the only thing that had to be taught anything new was the arrow
; from a selector to a base.
;---------------------------------------------------------------------
pm3_base:
        push    edi
        test    al,4                            ; LDT or GDT?
        jz      pm3_bgdt
        movzx   edi,ax
        shr     edi,3
        cmp     edi,DPMI_LDT_ENT
        jae     pm3_bbad
        shl     edi,3
        add     edi,DPMI_LDT
        jmp     short pm3_bread

;--- the GDT, and ONLY the program's two entries in it.  A ring-3 fault
;--- carrying any other GDT selector is not something to look up: it is
;--- the kernel's own selectors turning up where ring-3 code should
;--- never have had them.
pm3_bgdt:
        movzx   edi,ax
        and     edi,NOT 7
        cmp     edi,GDT_IDX_PROGCS * 8
        je      pm3_bok
        cmp     edi,GDT_IDX_PROGDS * 8
        jne     pm3_bbad
pm3_bok:
        add     edi,GDT_BASE
pm3_bread:
        test    byte ptr [edi+5],80h            ; present?
        jz      pm3_bbad
        movzx   ebx,byte ptr [edi+7]
        shl     ebx,8
        mov     bl,[edi+4]
        shl     ebx,16
        mov     bx,[edi+2]
        pop     edi
        clc
        ret
pm3_bbad:
        pop     edi
        stc
        ret

;=====================================================================
; pm3_sethook - IOS_PM3HOOK.  EDI = what to call when a ring-3 client
;               faults on something that is not emulable.
;=====================================================================
pm3_sethook:
        mov     dword ptr ds:[pm3_hook],edi
        ret

;=====================================================================
; A CLIENT'S POPF CANNOT TURN INTERRUPTS BACK ON, and nobody is told.
; At IOPL 0 a CLI faults and comes here, but a POPF or an IRET at ring
; 3 simply leaves IF alone - no fault, nothing to emulate - so a
; client that saves its flags, says CLI and puts the flags back is left
; with pm3_vif off for good, and so is one whose interrupt handler ends
; with a plain IRETD.  Both are the commonest way there is of writing
; the thing, and the specification's rule (STI, or 0900h-0902h) is not
; the one most extenders' libraries were written to: DMX, the sound
; library in DOOM and its relations, saves, CLIs and POPFs in the code
; that adds a timer service, and the game stopped dead at
; I_StartupTimer() with its next tick owed and never taken.
;
; THE STACK SAYS WHEN.  A handler's IRETD takes the frame off, and a
; routine's POPF comes before it returns: either way, once the client's
; ESP is above where it was when the flag went off, the flag has been
; put back.  So the moment it goes off is noted (pm3_offat) - the frame
; vpc_pmchk built, or a CLI already known to be one of these - and the
; first look after that (pm3_ifcheck: every tick, and every chance the
; monitor has to deliver an interrupt) that finds the stack above it
; turns the flag on again.  Only the outermost moment counts: a CLI
; with the flag already off is inside something that will end first.
;
; A CLI THAT IS NOT KNOWN YET is taken at its word, because most are
; paired with an STI, and a routine that turns interrupts off and
; returns leaving them off for its caller is a pattern too.  The timer
; watches instead (pm3_ifwatch).  Off for two clock ticks' worth of the
; timer's input - 110ms, however fast the program has it running -
; with the routine that said CLI already returned is a flag that was
; put back by an instruction that could not say so.  It is believed on
; again, once, and that CLI goes into pm3_sitetab, so that next time
; the stack is watched and nothing waits.  DOSEMU keeps a list of such
; CLIs for the same reason.  A program that keeps them off on purpose
; and waits where it is - DPMIIRQ's check 4 does, for a third of a
; second - is left alone, up to a second (PM3_OFFLONG), which nothing
; real ever asks for.  A watched one whose stack never comes back up
; within 110ms is given up on the same way.  Single-stepping to the
; POPF would be exact and was tried: DMX builds a volume table inside
; one of these, and a hundred thousand debug traps to start a sound is
; not a price.
;
; PM3_OFFMAX and PM3_SITES are in PMDOS.INC.
;=====================================================================

;---------------------------------------------------------------------
; pm3_ifwatch - irq_timer, once an interrupt.  EBX -> the IRETD frame
;               it took.  EAX is changed.
;---------------------------------------------------------------------
pm3_ifwatch:
        test    dword ptr [ebx+8],EFL_VM
        jnz     piw_none                ; a DOS box: its flag is exact
        mov     eax,dword ptr [ebx+4]
        test    al,4
        jz      piw_out                 ; the kernel: not the client's time
        and     al,3
        cmp     al,3
        jne     piw_out
        call    pm3_ifcheck
        cmp     byte ptr ds:[pm3_vif],0
        jne     piw_none
        mov     eax,dword ptr ds:[vt_div]       ; this interrupt's share
        add     eax,dword ptr ds:[pm3_offcnt]
        mov     dword ptr ds:[pm3_offcnt],eax
        cmp     eax,PM3_OFFLONG
        jae     piw_on                          ; nothing is off this long
        cmp     eax,PM3_OFFMAX
        jb      piw_out
        cmp     byte ptr ds:[pm3_watch],0       ; watched, and the stack
        jne     piw_on                          ; never came back up
        mov     eax,dword ptr [ebx+16]          ; a CLI not known yet: only
        cmp     ax,word ptr ds:[pm3_offss]      ; once its routine has
        jne     piw_out                         ; returned - which a POPF
        mov     eax,dword ptr [ebx+12]          ; section's does, and a
        cmp     eax,dword ptr ds:[pm3_offesp]   ; program waiting with them
        jbe     piw_out                         ; off on purpose does not
piw_on:
        mov     byte ptr ds:[pm3_vif],1
        mov     byte ptr ds:[pm3_watch],0
        inc     dword ptr ds:[pm3_revived]
        mov     eax,dword ptr ds:[pm3_clia]
        or      eax,eax
        jz      piw_none
        call    pm3_remember
piw_none:
        mov     dword ptr ds:[pm3_offcnt],0
piw_out:
        ret

;---------------------------------------------------------------------
; pm3_ifcheck - EBX -> a ring-3 client's IRETD frame (EIP, CS, EFLAGS,
;               ESP, SS).  If the flag is off and the stack has come
;               back above where it went off, it is on.  Everything
;               preserved.
;---------------------------------------------------------------------
pm3_ifcheck:
        cmp     byte ptr ds:[pm3_vif],0
        jne     pic_out
        cmp     byte ptr ds:[pm3_watch],0
        je      pic_out
        push    eax
        mov     eax,dword ptr [ebx+16]
        cmp     ax,word ptr ds:[pm3_offss]
        jne     pic_no
        mov     eax,dword ptr [ebx+12]
        cmp     eax,dword ptr ds:[pm3_offesp]
        jbe     pic_no
        mov     byte ptr ds:[pm3_vif],1
        mov     byte ptr ds:[pm3_watch],0
        mov     dword ptr ds:[pm3_offcnt],0
pic_no:
        pop     eax
pic_out:
        ret

;---------------------------------------------------------------------
; pm3_offat - the flag has just gone off, and comes back on when SS:ESP
;             is above DX:EAX.  Everything preserved.
;---------------------------------------------------------------------
pm3_offat:
        mov     dword ptr ds:[pm3_offesp],eax
        mov     word ptr ds:[pm3_offss],dx
        mov     byte ptr ds:[pm3_watch],1
        ret

;---------------------------------------------------------------------
; pm3_remember - EAX = the linear address of a CLI a POPF undoes.  Into
;                pm3_sitetab if it is not there already, over the oldest
;                if the table is full.  Everything preserved.
;---------------------------------------------------------------------
pm3_remember:
        push    ebx
        push    ecx
        mov     ebx,eax
        call    pm3_known
        jnc     prm_out
        mov     ecx,dword ptr ds:[pm3_snext]
        mov     dword ptr ds:[pm3_sitetab + ecx*4],eax
        inc     ecx
        cmp     ecx,PM3_SITES
        jb      prm_next
        xor     ecx,ecx
prm_next:
        mov     dword ptr ds:[pm3_snext],ecx
prm_out:
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; pm3_known - EBX = the linear address of a CLI.  CF clear if it is one
;             of pm3_sitetab.  Everything preserved.
;---------------------------------------------------------------------
pm3_known:
        push    ecx
        push    edi
        mov     edi,OFFSET pm3_sitetab
        mov     ecx,PM3_SITES
pkn_lp:
        cmp     dword ptr [edi],ebx
        je      pkn_yes
        add     edi,4
        loop    pkn_lp
        pop     edi
        pop     ecx
        stc
        ret
pkn_yes:
        pop     edi
        pop     ecx
        clc
        ret

;---------------------------------------------------------------------
; pm3_forget - a new program in the box: no sites, nothing watched.
;              From vpc_boxreset.  Everything preserved.
;---------------------------------------------------------------------
pm3_forget:
        push    eax
        push    ecx
        xor     eax,eax                 ; no string instruction: this is
        mov     ecx,PM3_SITES           ; called with whatever ES and DF
pfg_lp:                                 ; the DOS side had
        mov     dword ptr ds:[pm3_sitetab + ecx*4 - 4],eax
        loop    pfg_lp
        mov     dword ptr ds:[pm3_snext],eax
        mov     dword ptr ds:[pm3_clia],eax
        mov     byte ptr ds:[pm3_watch],al
        pop     ecx
        pop     eax
        ret

pm3_hook        dd      0
pm3_offcnt      dd      0               ; timer input since IF went off
pm3_revived     dd      0               ; how often pm3_ifwatch stepped in
pm3_clia        dd      0               ; the CLI that turned it off, or 0
pm3_offesp      dd      0               ; where the stack was when it did
pm3_snext       dd      0               ; where the next site goes
pm3_sitetab     dd      PM3_SITES dup(0)
pm3_offss       dw      0               ; ...in this stack segment
pm3_vif         db      1               ; the client's virtual interrupt flag
pm3_watch       db      0               ; 1 = pm3_offesp says when it is on
msgPm3          db      13,10,'PM-DOS: ring-3 fault with no handler, vector ',0
