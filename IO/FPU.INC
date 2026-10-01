;=====================================================================
; FPU.INC - the coprocessor, MMX and SSE
;
; PM-DOS runs one program at a time, so there is no task switch here to
; save a floating-point state around and the kernel never has to: it
; executes no floating-point instruction of its own outside this file,
; and the registers are simply the running program's.  What a system
; that owns the processor does owe a program is three smaller things,
; and until this file it gave none of them:
;
;   THE ERROR HAS TO ARRIVE.  A program that unmasks divide-by-zero
;   wants to be told, and on the machine DOS programs were written for
;   it is told by an interrupt: the coprocessor's error pin is wired to
;   line 13, the BIOS's handler on INT 75h passes it to INT 02h, and a
;   run-time library has its handler on one or the other.  Line 13 was
;   masked here and nothing had asked for the other route, so the error
;   went nowhere at all.
;
;   Every processor since the 486 has the other route: with NE set in
;   CR0 the error is exception 16, raised by the processor at the next
;   floating-point instruction, no interrupt controller involved.  That
;   is what is used.  The monitor turns it back into the interrupt a DOS
;   box expects (VM86.INC, vfe_fperr), a DPMI client is given it on the
;   same vector (DPMI.INC), and a native program is ended with the
;   message any other fault of its gets.
;
;   A 386 has no NE.  There the line is opened and is the kernel's
;   (fpu_line), and irq_common turns the interrupt into the same
;   exception on its way out.  BUILD fpuirq makes any machine do it that
;   way, which is the only way that path is ever run on an emulator.
;
;   SSE HAS TO BE SWITCHED ON, and only ring 0 can: with OSFXSR clear in
;   CR4 every SSE instruction is an invalid opcode.  Under plain DOS a
;   program sets the bit itself.  In a DOS box or as a DPMI client it
;   cannot - a move to CR4 faults there - so the host does it or nobody
;   does.  MMX needs nothing: it lives in the coprocessor's registers
;   and was designed to need nothing.
;
;   A CHILD MUST NOT LEAVE ITS STATE TO ITS PARENT.  EXEC is the one
;   place a second program runs in front of a first, and a child that
;   ends with values on the stack, a changed control word or MMX
;   registers it never emptied hands all of that back.  DOS has always
;   allowed it to.  IOS_FPU is how the DOS side keeps the parent's state
;   at each level and gives the child a fresh one.
;
; AND PARKED, THE MACHINE IS AS THE BIOS LEFT IT.  What runs then is
; real DOS software and Windows, which look for the error on line 13
; and set CR4 for themselves (fpu_park, fpu_unpark).
;
; Included by IOPM.ASM, after VPIC.INC.
;=====================================================================

CR0_MP          EQU     00000002h
CR0_EM          EQU     00000004h
CR0_TS          EQU     00000008h
CR0_NE          EQU     00000020h
CR4_OSFXSR      EQU     00000200h
CR4_OSXMM       EQU     00000400h

EFL_AC          EQU     00040000h               ; can be changed: a 486
EFL_ID          EQU     00200000h               ; ...and this one: a CPUID

CPUF_MMX        EQU     00800000h               ; CPUID's EDX
CPUF_FXSR       EQU     01000000h
CPUF_SSE        EQU     02000000h
CPUF_SSE2       EQU     04000000h

PORT_FPCLR      EQU     0F0h                    ; any write lets go of line 13

FPC_486         EQU     01h                     ; fpu_cpu
FPC_CPUID       EQU     02h
FPC_CR4         EQU     04h                     ; there is one, and it is ours

MXCSR_RESET     EQU     00001F80h               ; every exception masked

;=====================================================================
; fpu_init - once, from pm_entry, with interrupts off.
;=====================================================================
fpu_init:
        pushad
        mov     eax,cr0
        mov     dword ptr [fpu_cr0boot],eax
        mov     dword ptr [fpu_cr0run],eax

        ; Where a state is kept across a park: FXSAVE wants a 16-byte
        ; boundary and says so with a fault.
        mov     eax,OFFSET fpu_parkbuf + 15
        and     eax,NOT 15
        mov     dword ptr [fpu_parkat],eax

;--- what processor is it ---------------------------------------------
        mov     ebx,EFL_AC
        call    fpu_flagbit
        jz      fpi_probe
        or      byte ptr [fpu_cpu],FPC_486
        mov     ebx,EFL_ID
        call    fpu_flagbit
        jz      fpi_probe
        xor     eax,eax
        db      0Fh,0A2h                        ; CPUID
        or      eax,eax
        jz      fpi_probe
        mov     eax,1
        db      0Fh,0A2h
        mov     dword ptr [fpu_feat],edx
        or      byte ptr [fpu_cpu],FPC_CPUID

;--- is there a coprocessor -------------------------------------------
; Asked of the part, the way Intel's own note does it: after FNINIT the
; status word is zero and the control word has six mask bits set and
; bit 12 clear.  With nothing in the socket the two stores store
; nothing, and what was put in the word beforehand is still there.
fpi_probe:
        mov     eax,cr0
        and     eax,NOT (CR0_EM OR CR0_TS)
        mov     cr0,eax
        fninit
        mov     word ptr [fpu_word],5A5Ah
        fnstsw  word ptr [fpu_word]
        cmp     word ptr [fpu_word],0
        jne     fpi_none
        fnstcw  word ptr [fpu_word]
        mov     ax,word ptr [fpu_word]
        and     ax,103Fh
        cmp     ax,003Fh
        jne     fpi_none
        or      byte ptr [fpu_flags],FPUF_X87

;--- how its errors arrive --------------------------------------------
        mov     eax,cr0
        or      eax,CR0_MP
IFNDEF FPU_BYIRQ
        test    byte ptr [fpu_cpu],FPC_486
        jz      fpi_byline
        or      eax,CR0_NE
        mov     cr0,eax
        or      byte ptr [fpu_flags],FPUF_NE
        jmp     short fpi_cr4
ENDIF
fpi_byline:
        ; Bit 5 of a 386's CR0 is not NE, it is nothing, and is left
        ; as it was found.
        test    byte ptr [fpu_cpu],FPC_486
        jz      fpi_is386
        and     eax,NOT CR0_NE
fpi_is386:
        mov     cr0,eax
        or      byte ptr [fpu_flags],FPUF_IRQ13
        xor     al,al
        out     PORT_FPCLR,al
        mov     dword ptr [irq_vectors + FPU_IRQ * 4],OFFSET fpu_line
        mov     ecx,FPU_IRQ
        call    irq_want

;--- MMX and SSE ------------------------------------------------------
fpi_cr4:
        mov     eax,cr0
        mov     dword ptr [fpu_cr0run],eax
        test    byte ptr [fpu_cpu],FPC_CPUID
        jz      fpi_fresh
        mov     edx,dword ptr [fpu_feat]
        test    edx,CPUF_MMX
        jz      fpi_nommx
        or      byte ptr [fpu_flags],FPUF_MMX
fpi_nommx:
        test    edx,CPUF_FXSR
        jz      fpi_fresh
        db      0Fh,20h,0E0h                    ; MOV EAX,CR4
        mov     dword ptr [fpu_cr4boot],eax
        or      eax,CR4_OSFXSR
        or      byte ptr [fpu_flags],FPUF_FXSR
        test    edx,CPUF_SSE
        jz      fpi_setcr4
        ; OSXMMEXCPT IS LEFT CLEAR, ON PURPOSE.  With it set an SSE
        ; error a program unmasked is exception 19, and vector 13h is
        ; the disk BIOS: the same clash as fpu_gate below settles for
        ; the coprocessor, with no interrupt line in a PC's history to
        ; turn it into.  Clear, the processor reports it as an invalid
        ; opcode, which is nobody's BIOS call - a DOS box is given it
        ; on INT 06h and anything else is ended.
        or      byte ptr [fpu_flags],FPUF_SSE
        test    edx,CPUF_SSE2
        jz      fpi_setcr4
        or      byte ptr [fpu_flags],FPUF_SSE2
fpi_setcr4:
        db      0Fh,22h,0E0h                    ; MOV CR4,EAX
        mov     dword ptr [fpu_cr4run],eax
        or      byte ptr [fpu_cpu],FPC_CR4
fpi_fresh:
        call    fpu_new
        popad
        ret

        ; No coprocessor.  CR0 goes back to what the BIOS decided about
        ; that - some set EM so that an emulator can be loaded, some do
        ; not - and nothing else here is touched.
fpi_none:
        mov     eax,dword ptr [fpu_cr0boot]
        mov     cr0,eax
        popad
        ret

;---------------------------------------------------------------------
; fpu_flagbit - EBX = a bit of EFLAGS.  ZF set if it cannot be changed.
;               EAX and ECX are used.
;---------------------------------------------------------------------
fpu_flagbit:
        pushfd
        pop     eax
        mov     ecx,eax
        xor     eax,ebx
        push    eax
        popfd
        pushfd
        pop     eax
        push    ecx
        popfd
        xor     eax,ecx
        test    eax,ebx
        ret

;=====================================================================
; fpu_svc - IOS_FPU.  AL = an FPU_* function (PMDOS.INC).  With no
;           coprocessor all but FPU_INFO do nothing.
;
; NOTHING HERE WAITS.  A waiting instruction is one that looks for an
; error left outstanding before it does its work, and the state being
; put away or thrown away is exactly the one that may have left it.
; So the state is kept with the forms that do not look, and the
; registers are cleared before another state is put into them.
;=====================================================================
fpu_svc:
        cmp     al,FPU_INFO
        je      fpv_info
        test    byte ptr [fpu_flags],FPUF_X87
        jz      fpv_out
        cmp     al,FPU_KEEP
        je      fpv_keep
        cmp     al,FPU_BACK
        je      fpv_back
        cmp     al,FPU_FRESH
        je      fpu_new
fpv_out:
        ret

fpv_info:
        movzx   eax,byte ptr [fpu_flags]
        mov     ebx,FPU_AREA
        ret

fpv_keep:
        test    byte ptr [fpu_flags],FPUF_FXSR
        jz      fpv_keep87
        db      0Fh,0AEh,07h                    ; FXSAVE [EDI]
        ; THE STATUS WORD AGAIN, BY THE INSTRUCTION THAT ONLY STORES
        ; THAT.  On a processor it writes the two bytes FXSAVE has just
        ; written.  On 86Box without fpu_softfloat it writes the right
        ; ones: that FXSAVE leaves the top-of-stack field out of the
        ; image, so a state put back from it has its registers and has
        ; forgotten which of them is the top.  (Found with a program
        ; that does nothing else, under a kernel that had none of this
        ; in it.)
        fnstsw  word ptr [edi+2]
        jmp     short fpu_new
fpv_keep87:
        fnsave  [edi]
        ; fall through

;---------------------------------------------------------------------
; fpu_new - a state nobody has used.  Everything preserved.
;---------------------------------------------------------------------
fpu_new:
        fninit
        test    byte ptr [fpu_flags],FPUF_SSE
        jz      fpn_out
        db      0Fh,0AEh,15h                    ; LDMXCSR [fpu_mxcsr]
        dd      OFFSET fpu_mxcsr
fpn_out:
        ret

fpv_back:
        fninit
        test    byte ptr [fpu_flags],FPUF_FXSR
        jz      fpv_back87
        db      0Fh,0AEh,0Eh                    ; FXRSTOR [ESI]
        ret
fpv_back87:
        frstor  [esi]
        ret

;=====================================================================
; VECTOR 10h IS TWO THINGS, and has been since the PC was designed:
; Intel reserved it for the coprocessor's error and IBM gave it to the
; video BIOS.  In real mode nothing collides, because there the error
; comes in on interrupt line 13.  Here it does - the DPMI host has a
; gate on 10h that ring 3 may call, so that a client's INT 10h reaches
; the BIOS, and with NE set the processor raises exception 16 through
; the same gate.  The first kernel with NE in it sent a client's
; divide-by-zero to the video BIOS, which returned to the instruction
; that had faulted, for ever.
;
; So the gate is this file's, and what the DOS side asks IOS_SETVEC to
; put on vector 10h is kept and jumped to (fpu_setsoft, from
; set_vector).  Which of the two an arrival is:
;
;   FROM A DOS BOX OR FROM RING 0, the error.  A box's INT 10h is a
;   general protection fault - that is how the monitor sees any of a
;   box's interrupts - and the kernel calls nobody.
;
;   FROM RING 3 WITH NO ERROR OUTSTANDING, a call.  The status word
;   says, and FNSTSW is one of the instructions that does not mind.
;
;   WITH ONE OUTSTANDING, the instruction decides.  The address the
;   processor stacked for an error is the coprocessor instruction it
;   stopped at; for a call it is the one after CD 10.  Both at once -
;   an INT 10h, made with an error outstanding, and a coprocessor
;   instruction straight after it - is read as the error, which is
;   then reported one instruction early and costs the program that
;   one call.  Read the other way it would be a call made for ever.
;=====================================================================
fpu_setsoft:
        mov     dword ptr [fpu_soft],edi
        push    eax
        push    edx
        mov     edx,OFFSET fpu_gate
        mov     ah,0EEh
        call    set_gate
        pop     edx
        pop     eax
        ret

fpu_gate:
        test    dword ptr [esp+8],EFL_VM
        jnz     short fpg_error
        test    byte ptr [esp+4],3
        jz      short fpg_error
        test    byte ptr ss:[fpu_flags],FPUF_X87
        jz      short fpg_call
        push    eax
        fnstsw  ax
        test    al,80h                  ; ES: an unmasked one is waiting
        pop     eax
        jz      short fpg_call

        push    eax
        push    ebx
        push    ds
        mov     ax,ss
        mov     ds,ax
        mov     ax,word ptr [esp+16]    ; the CS it came from
        call    pm3_base
        jc      short fpg_look
        add     ebx,dword ptr [esp+12]
        cmp     word ptr [ebx-2],10CDh
        jne     short fpg_look
        mov     al,byte ptr [ebx]
        cmp     al,9Bh                  ; FWAIT
        je      short fpg_look
        and     al,0F8h
        cmp     al,0D8h                 ; D8 to DF: the coprocessor's
        je      short fpg_look
        pop     ds
        pop     ebx
        pop     eax
fpg_call:
        cmp     dword ptr ss:[fpu_soft],0
        je      short fpg_nobody
        jmp     dword ptr ss:[fpu_soft]
fpg_nobody:
        iretd
fpg_look:
        pop     ds
        pop     ebx
        pop     eax
fpg_error:
        push    0
        push    16
        jmp     exc_common

;=====================================================================
; fpu_line - interrupt line 13, on a machine whose coprocessor reports
;            its errors there.  From irq_common, which sends the EOI.
;
; The write lets go of the line, which is otherwise held until the
; error is cleared - and the handler that will clear it has not run
; yet.  Whose error it is is not decided here: irq_common has the
; frame, and looks at fpu_owed on its way out.
;=====================================================================
fpu_line:
        xor     al,al
        out     PORT_FPCLR,al
        mov     byte ptr [fpu_owed],1
        ret

;=====================================================================
; fpu_park   - rm_park, on its way down: the running program's state is
;              put away, and CR0 and CR4 are the BIOS's again.
; fpu_unpark - and on its way back.  Everything preserved.
;=====================================================================
fpu_park:
        test    byte ptr [fpu_flags],FPUF_X87
        jz      fpk_out
        push    eax
        push    edi
        mov     edi,dword ptr [fpu_parkat]
        mov     al,FPU_KEEP
        call    fpu_svc
        test    byte ptr [fpu_cpu],FPC_CR4
        jz      fpk_cr0
        mov     eax,dword ptr [fpu_cr4boot]
        db      0Fh,22h,0E0h                    ; MOV CR4,EAX
fpk_cr0:
        ; PE and PG are not the BIOS's to have back yet: the stub
        ; clears them one at a time, in the order that works.
        mov     eax,cr0
        and     eax,NOT (CR0_MP OR CR0_EM OR CR0_TS OR CR0_NE)
        mov     edi,dword ptr [fpu_cr0boot]
        and     edi,CR0_MP OR CR0_EM OR CR0_TS OR CR0_NE
        or      eax,edi
        mov     cr0,eax
        pop     edi
        pop     eax
fpk_out:
        ret

fpu_unpark:
        test    byte ptr [fpu_flags],FPUF_X87
        jz      fpu_upout
        push    eax
        push    esi
        mov     eax,cr0
        and     eax,NOT (CR0_MP OR CR0_EM OR CR0_TS OR CR0_NE)
        mov     esi,dword ptr [fpu_cr0run]
        and     esi,CR0_MP OR CR0_EM OR CR0_TS OR CR0_NE
        or      eax,esi
        mov     cr0,eax
        test    byte ptr [fpu_cpu],FPC_CR4
        jz      fpu_upstate
        ; Whatever had the machine may have changed the rest of CR4,
        ; and the rest is not ours to put back.
        db      0Fh,20h,0E0h                    ; MOV EAX,CR4
        and     eax,NOT (CR4_OSFXSR OR CR4_OSXMM)
        mov     esi,dword ptr [fpu_cr4run]
        and     esi,CR4_OSFXSR OR CR4_OSXMM
        or      eax,esi
        db      0Fh,22h,0E0h                    ; MOV CR4,EAX
fpu_upstate:
        mov     esi,dword ptr [fpu_parkat]
        mov     al,FPU_BACK
        call    fpu_svc
        pop     esi
        pop     eax
fpu_upout:
        ret

;=====================================================================
; State
;=====================================================================
fpu_cr0boot     dd      0               ; CR0 as the BIOS left it
fpu_cr0run      dd      0               ; ...and as PM-DOS runs with it
fpu_cr4boot     dd      0
fpu_cr4run      dd      0
fpu_feat        dd      0               ; CPUID's EDX, 0 without one
fpu_mxcsr       dd      MXCSR_RESET
fpu_parkat      dd      0               ; the state a park put away
fpu_soft        dd      0               ; what INT 10h is, when it is called
fpu_word        dw      0
fpu_flags       db      0               ; FPUF_*
fpu_cpu         db      0               ; FPC_*
fpu_owed        db      0               ; line 13 has interrupted, and the
                                        ; program has not been told
                db      3 dup(0)
fpu_parkbuf     db      (FPU_AREA + 16) dup(0)
