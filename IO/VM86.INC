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
        and     eax,V86_VFLAGS
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
        call    v86_kbwant
        jc      vr_nokey
        mov     al,9
        call    v86_intctx
        mov     byte ptr ds:[v86_vif],0         ; an ISR starts closed, and
                                                ; v86_intctx has already said
                                                ; so in the context's flags
vr_nokey:
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
        push    eax                     ; the guest has stopped: guard our own
        mov     al,1                    ; code again
        call    nt_arm
        pop     eax
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
        pushad
        movzx   ebx,al

        mov     eax,[esi+V86C_FLAGS]            ; the flags it believes in
        call    vic_push
        mov     eax,[esi+V86C_CS]
        call    vic_push
        mov     eax,[esi+V86C_EIP]              ; already past the INT: the
        call    vic_push                        ; monitor skipped it before
                                                ; it decided whose this was
        and     dword ptr [esi+V86C_FLAGS],NOT EFL_IF   ; an ISR starts closed

        shl     ebx,2                           ; the vector's IVT slot
        mov     eax,dword ptr ds:[ebx]
        movzx   edx,ax
        mov     [esi+V86C_EIP],edx
        shr     eax,16
        mov     [esi+V86C_CS],eax
        popad
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
        mov     dword ptr ds:[v86_watch],0      ; the guest is still asking
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

        mov     eax,[ebp+FR_VEC]
        cmp     eax,13
        je      vf_isgp                 ; only #GP means "emulate me"
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
vf_pfx:
        cmp     ecx,8                   ; a real instruction is shorter
        jae     vf_bad
        mov     al,[esi+ecx]
        inc     ecx
        cmp     al,66h                  ; operand size
        je      vf_pfx
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

vf_bad:
        mov     eax,V86R_FAULT
        mov     ebx,[ebp+FR_VEC]
        jmp     v86_leave
vf_notgp:
        mov     eax,V86R_FAULT
        mov     ebx,[ebp+FR_VEC]
        jmp     v86_leave
vf_hlt:
        mov     eax,V86R_HLT
        xor     ebx,ebx
        jmp     v86_leave

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
        in      al,dx
        ret

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
        push    edx
        mov     dx,PORT_KBCMD
        in      al,dx
        pop     edx
        and     al,KBS_OBF OR KBS_AUXB
        cmp     al,KBS_OBF OR KBS_AUXB
        je      vio_ireal               ; the mouse is talking, not us

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
        out     dx,al
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
        out     dx,al
        ret
vio_o60a:
        cmp     byte ptr ds:[vio_kbdaux],0
        je      vio_o60k
        mov     byte ptr ds:[vio_kbdaux],0
        out     dx,al                   ; the mouse's, and the mouse is not
        ret                             ; a device this kernel has taken
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
        mov     eax,[ebp+FR_EIP]
        cmp     eax,dword ptr ds:[v86_flteip]
        jne     vfe_moved
        mov     eax,[ebp+FR_CS]
        cmp     eax,dword ptr ds:[v86_fltcs]
        jne     vfe_moved
        mov     eax,[ebp+FR_VEC]
        cmp     eax,dword ptr ds:[v86_fltvec]
        jne     vfe_moved
        inc     dword ptr ds:[v86_fltn]
        cmp     dword ptr ds:[v86_fltn],V86_FLTMAX
        jb      vfe_ivt
        mov     eax,V86R_FLTLOOP        ; stuck: the caller says so and ends
        mov     ebx,[ebp+FR_VEC]        ; the box
        jmp     v86_leave
vfe_moved:
        mov     eax,[ebp+FR_EIP]
        mov     dword ptr ds:[v86_flteip],eax
        mov     eax,[ebp+FR_CS]
        mov     dword ptr ds:[v86_fltcs],eax
        mov     eax,[ebp+FR_VEC]
        mov     dword ptr ds:[v86_fltvec],eax
        mov     dword ptr ds:[v86_fltn],1
vfe_ivt:
        mov     eax,[ebp+FR_VEC]
        shl     eax,2
        cmp     dword ptr ds:[eax],0
        je      vf_notgp                ; nothing there: it really is a crash
        mov     eax,[ebp+FR_VEC]
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
        push    eax                     ; vector

        mov     eax,[ebp+FR_FLAGS]
        and     eax,V86_UFLAGS
        or      eax,dword ptr ds:[v86_vpriv]
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
        shl     eax,2                   ; vector * 4 = its IVT slot
        mov     eax,dword ptr ds:[eax]
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
        cmp     dword ptr ds:[IVT_INT09],0
        je      vkw_no
        call    kbq_take                ; and it latches the code, because
        ret                             ; that is what raising the interrupt
vkw_no:                                 ; does on the real part
        stc
        ret

;---------------------------------------------------------------------
; v86_ckkbd - EBP -> a fault frame belonging to the guest.  Deliver a
;             queued keystroke if there is one.
;---------------------------------------------------------------------
v86_ckkbd:
        pushad
        call    v86_kbwant
        jc      vck_no
        mov     eax,9
        call    v86_deliver
vck_no:
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
vf_pushf:
        mov     eax,[ebp+FR_FLAGS]
        and     eax,V86_UFLAGS
        or      eax,dword ptr ds:[v86_vpriv]
        cmp     byte ptr ds:[v86_vif],0
        je      vp_ioff
        or      eax,EFL_IF
vp_ioff:
        call    v86_push
        call    v86_skip
        jmp     v86_resume

vf_popf:
        call    v86_pop
        call    v86_setflags
        call    v86_skip
        jmp     v86_resume

;--- IRET -------------------------------------------------------------
vf_iret:
        call    v86_pop
        movzx   eax,ax
        mov     [ebp+FR_EIP],eax
        call    v86_pop
        movzx   eax,ax
        mov     [ebp+FR_CS],eax
        call    v86_pop
        call    v86_setflags
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
v86_setflags:
        push    eax
        mov     byte ptr ds:[v86_vif],0
        test    ax,EFL_IF
        jz      vs_off
        mov     byte ptr ds:[v86_vif],1
vs_off:
        movzx   eax,ax
        push    eax
        and     eax,V86_VFLAGS          ; what it thinks IOPL and NT are
        mov     dword ptr ds:[v86_vpriv],eax
        pop     eax
        and     eax,V86_UFLAGS          ; and what it actually runs under
        or      eax,EFL_VM OR EFL_IF OR 2
        mov     [ebp+FR_FLAGS],eax
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

;---------------------------------------------------------------------
; v86_resume - back into the guest, exactly where it left off
;---------------------------------------------------------------------
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
v86_vpriv       dd      0               ; the guest's virtual IOPL and NT: what
                                        ; it may read back, never what it runs
                                        ; under - see v86_setflags
v86_fltcs       dd      -1              ; the last exception reflected into the
v86_flteip      dd      -1              ; guest, and how many times running -
v86_fltvec      dd      -1              ; see vf_exc.  -1 so that the first one
v86_fltn        dd      0               ; can never match a real CS:IP
v86_vif         db      0               ; the guest's virtual interrupt flag
v86_active      db      0               ; a guest is executing, as opposed to
                                        ; merely loaded: set on the way into
                                        ; the guest and cleared the moment the
                                        ; monitor takes the CPU back, so that
                                        ; IRQ 1 knows whose keystroke it is
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
        jne     pm3_real

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

;--- not ours: the client has really faulted -------------------------
pm3_real:
        cmp     dword ptr ds:[pm3_hook],0
        je      pm3_dead
        mov     eax,[ebp+P3_VEC]
        mov     edx,[ebp+P3_ERR]
        mov     ecx,[ebp+P3_EIP]
        movzx   ebx,word ptr [ebp+P3_CS]
        call    dword ptr ds:[pm3_hook]         ; does not return
pm3_dead:
        mov     esi,OFFSET msgPm3
        call    con_str
        mov     eax,[ebp+P3_VEC]
        call    con_hex8
pm3_stop:
        hlt
        jmp     pm3_stop

;--- the six that are emulated ---------------------------------------
pm3_cli:
        mov     byte ptr ds:[pm3_vif],0
        inc     ecx
        jmp     pm3_done
pm3_sti:
        mov     byte ptr ds:[pm3_vif],1
        inc     ecx
        jmp     pm3_done

pm3_inb_i:
        movzx   edx,byte ptr [esi+1]
        call    vio_in
        mov     byte ptr [ebp+PA_EAX],al
        add     ecx,2
        jmp     short pm3_done
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
        mov     es,word ptr ss:[exc_ses]
        mov     fs,word ptr ss:[exc_sfs]
        mov     gs,word ptr ss:[exc_sgs]
        mov     ds,word ptr ss:[exc_sds]
        popad
        add     esp,8                   ; the vector and the error code
        iretd

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

pm3_hook        dd      0
pm3_vif         db      1               ; the client's virtual interrupt flag
                db      0,0,0
msgPm3          db      13,10,'PM-DOS: ring-3 fault with no handler, vector ',0
