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
        mov     esi,OFFSET msgV86CR
        call    con_str
        popad
ENDIF

        cmp     dword ptr [ebp+FR_VEC],13
        jne     vf_notgp                ; only #GP means "emulate me"

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
        je      vio_ireal
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
        je      vio_i64out
        or      al,01h
vio_i64out:
        ret

;---------------------------------------------------------------------
; vio_out - DX = port, AL = the byte.
;---------------------------------------------------------------------
vio_out:
        cmp     dx,PORT_SYSCTL
        je      vio_o92
        cmp     dx,PORT_KBCMD
        je      vio_o64
        cmp     dx,PORT_KBDATA
        je      vio_o60
        out     dx,al
        ret

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

;--- 64h: commands.  Four of them are about A20 ----------------------
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

;--- 60h: data.  Interesting only straight after a D1h ---------------
vio_o60:
        cmp     byte ptr ds:[vio_kbdwr],0
        je      vio_o60r
        mov     byte ptr ds:[vio_kbdwr],0
        shr     al,1                    ; bit 1 is A20; bit 0 is reset,
        and     al,01h                  ; and 92h's reasoning applies
        call    pg_a20
        ret
vio_o60r:
        out     dx,al
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

;---------------------------------------------------------------------
; v86_reflect - AL = vector.  Do what the 8086 would have done: stack
;               the flags and the return address, then take CS:IP from
;               the guest's own interrupt vector table at 0:0.
;
; The flags that go on the guest's stack are the ones it believes in,
; and the handler is entered with interrupts "off" - virtually.
;---------------------------------------------------------------------
v86_reflect:
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
        jmp     v86_resume

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
v86_vif         db      0               ; the guest's virtual interrupt flag
vio_porta       db      0               ; port 92h as the guest last wrote it
vio_kbdwr       db      0               ; an 8042 D1h is outstanding
vio_kbdrd       db      0               ; ...or a D0h
IFDEF V86_TRACE
msgV86In        db      'V86: about to IRETD into the guest',13,10,0
msgV86Flt       db      'V86: fault vec ',0
msgV86CR        db      13,10,0
ENDIF

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
