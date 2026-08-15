;=====================================================================
; RING3.INC - native programs, at ring 3, in a segment of their own
;
; Until this release a D32 program ran at ring 0 in the kernel's own
; segment.  Everything worked, and nothing was protected: a program's
; wild pointer and the kernel's were the same event, its bugs and the
; kernel's arrived at the same fault handler, and the recovery v0.30
; built could only ever be a best effort - the program might have
; scribbled on the kernel before it faulted, and there was no way from
; inside to know.  The least protected thing in PM-DOS was PM-DOS's
; own software.
;
; So a native program now runs where a DPMI client and a DOS box
; already ran: at ring 3, out of descriptors that describe its slot and
; nothing else.  Three things follow, and the third is the one that was
; worth the release:
;
;   IT CANNOT REACH THE KERNEL.  Not to read it, not to write it, not
;   by accident with a stale pointer.  The kernel is below every slot
;   base and outside every slot limit; there is no address a program
;   can form that names it.
;
;   IT CANNOT REACH ANOTHER PROGRAM.  The shell's slot and the
;   transient slot are separate segments.  This is also why the
;   environment is now COPIED into a child rather than pointed at, and
;   why 48h allocates inside the slot rather than out of the kernel's
;   arena: a pointer a program cannot follow is worse than no pointer.
;
;   ITS ADDRESS ZERO IS A PAGE THAT IS NOT THERE.  That is the real
;   null-pointer guard, and it is the one v0.31 wanted and could not
;   build.  Page zero of the MACHINE is load-bearing - the IVT, the
;   BIOS data area, InDOS at 4F7h, the real-mode frame at 500h - so it
;   could not be unmapped, and four debug registers watching sixteen
;   bytes were what was left.  Page zero of a PROGRAM is nothing at
;   all.  A store through a null pointer is now an ordinary page fault
;   at a known address, it is caught whatever the width and whatever
;   the offset, and - unlike DR7, which no emulator implements - it can
;   be tested here rather than only on iron.
;
; What a program gives up is the ability to do I/O and to stop
; interrupts, and it does not quite give those up: CLI, STI, IN and OUT
; fault into pm3_fault and are emulated there, exactly as they are for
; a DPMI client and for the same reason - IOPL 0 is what keeps the A20
; ports out of a program's hands, and the interrupt flag a program
; believes in has to be a fiction or a program could stop the machine's
; timer.
;
; Included by DOSMAIN.ASM, after INT21.INC.
;=====================================================================

;---------------------------------------------------------------------
; One record per ring-3 handler call in flight - see r3_thunk.
;---------------------------------------------------------------------
RC_ESP          EQU     0               ; dd - the kernel ESP to resume at
RC_ESP0         EQU     4               ; dd - the TSS_ESP0 to put back
RC_VEC          EQU     8               ; dd - which vector this was
RC_DEPTH        EQU     12              ; dd - and the slot that was current
RC_LASTESP      EQU     16              ; dd - and where its program's stack was
RC_LEN          EQU     20
R3_CALLMAX      EQU     4               ; nested handler calls

; Where PUSHAD leaves EAX, counted from the bottom of the eight.
R3_PAEAX        EQU     28

;=====================================================================
; r3_useslot - EAX = how many programs deep EXEC is.  Point the two
;              descriptors at the slot that depth means, and remember
;              its base and size for everything else here.
;
; Depth zero is the interpreter, which has a slot of its own; anything
; deeper is a transient program, and every level of nesting shares the
; one transient slot because nesting is done by parking the occupant.
; So this is a two-way choice and not a table.
;=====================================================================
r3_useslot:
        push    eax
        push    ebx
        mov     [r3_depth],eax          ; whose slot the descriptors describe,
                                        ; which is not always [exec_depth] -
                                        ; see r3_thunk
        or      eax,eax
        jnz     rus_prog
        mov     eax,SHELL_SLOT
        mov     ebx,SHELL_SIZE
        jmp     short rus_set
rus_prog:
        mov     eax,PROG_SLOT
        mov     ebx,PROG_SIZE
rus_set:
        mov     [r3_base],eax
        mov     [r3_size],ebx
        call    r3_setdesc
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; r3_setdesc - write SEL_PROG_CS and SEL_PROG_DS from [r3_base] and
;              [r3_size].
;
; The GDT is writable memory in the real-mode stub, so this is four
; stores twice over.  It is safe to do at any time because the only
; code holding these selectors is ring-3 code, which is not running
; while the kernel is: every entry to ring 3 goes through an IRETD that
; reloads CS and SS from the table as it stands.
;
; Byte granularity, deliberately: a slot is 256K or 512K and a 4K-
; granular limit could describe either, but byte granularity means the
; limit is the size and the arithmetic anybody reads here is the
; arithmetic the CPU does.
;---------------------------------------------------------------------
r3_setdesc:
        pushad
        mov     ebx,[r3_base]
        mov     ecx,[r3_size]
        dec     ecx                             ; a limit is the last valid byte

        mov     edi,GDT_BASE + GDT_IDX_PROGCS * 8
        mov     dl,DACC_CODE                    ; present, DPL 3, code, readable
        call    r3_wrdesc
        mov     edi,GDT_BASE + GDT_IDX_PROGDS * 8
        mov     dl,DACC_DATA
        call    r3_wrdesc
        popad
        ret

; EDI -> the descriptor, EBX = base, ECX = limit, DL = access byte.
r3_wrdesc:
        mov     [edi],cx                        ; limit 15:0
        mov     [edi+2],bx                      ; base 15:0
        mov     eax,ebx
        shr     eax,16
        mov     [edi+4],al                      ; base 23:16
        mov     [edi+5],dl
        mov     eax,ecx
        shr     eax,16
        and     al,0Fh                          ; limit 19:16, and the D bit
        or      al,DGRAN_32                     ; beside it: 32-bit, byte granular
        mov     [edi+6],al
        mov     eax,ebx
        shr     eax,24
        mov     [edi+7],al                      ; base 31:24
        ret

;=====================================================================
; r3_start - hand control to a program that has just been loaded.
;            EAX = its entry point, as a slot offset.
;
; Never returns.  The stack starts at the top of the slot with the PSP
; offset on it, so that a program which simply RETs lands on the INT
; 20h at PSP+0 and terminates - which is what a .COM did, and what the
; kernel has always relied on.
;
; The other seven registers are zeroed rather than left holding
; whatever the loader had.  A program is entitled to find its machine
; in a stated condition, and "whatever EXEC was using" is not one.
;=====================================================================
r3_start:
        cli
        mov     [r3_eip],eax
        mov     eax,[r3_size]
        sub     eax,4                           ; the PSP offset goes on the
        mov     [r3_esp],eax                    ; stack for that RET
        mov     ebx,[r3_base]
        mov     dword ptr [ebx+eax],SLOT_PSP

        ; A fresh program is not inside anybody's handler, so anything
        ; left over from one the previous program abandoned goes here.
        ;
        ; [r3_esp0] IS NOT RESET, and that is the whole of what a child
        ; program has to inherit: it names the level of the one ring-0
        ; stack that every ring-3 program's DOS calls run on, and 4Bh
        ; has already moved it BELOW the frame its caller is waiting in.
        ; Resetting it here to the top of the stack is what a first
        ; version did, and it put the child's first INT 21h frame
        ; exactly on top of its parent's 4Bh - so 4Ch returned the
        ; parent to a frame full of the child's DOS call, and the shell
        ; resumed in the middle of its own message table.
        mov     dword ptr [r3_calldep],0
        mov     dword ptr [r3_active],1

        xor     eax,eax
        xor     ebx,ebx
        xor     ecx,ecx
        xor     edx,edx
        xor     esi,esi
        xor     edi,edi
        xor     ebp,ebp
        ; falls into r3_enter

;=====================================================================
; r3_enter - into the program at [r3_eip] with [r3_esp], preserving
;            every general register.
;
; THE RING-0 STACK POINTER IN THE TSS IS SET FIRST AND EVERY TIME, for
; the reason dpmi_toring3 states: everything that interrupts ring-3
; code switches stacks through it, including the timer and the keyboard,
; which arrive whether anybody wanted them or not.  v86_run leaves it
; pointing at its own frame, so it cannot be assumed.
;
; NOTHING BELOW THE SEGMENT LOADS MAY USE A GENERAL REGISTER, and that
; is why the last half-dozen instructions read their operands out of
; memory.  The registers belong to the program by then - for a handler
; call they are the ones the kernel wants the handler to see - and
; there is no scratch left.  SS is still the kernel's until the IRETD,
; which is what makes the SS-relative reads work.
;=====================================================================
r3_enter:
        cli

        ; AND NT MUST BE CLEAR IN THE FLAGS WE ARE STANDING IN, which
        ; has nothing to do with the flags being pushed.  With NT set,
        ; IRETD is not a return at all - it is a TASK SWITCH, and it
        ; follows the back-link at the front of the TSS, finds the zero
        ; left there, and raises exception 0Ah with error code 0.
        ;
        ; This is the second time this project has met that instruction
        ; wearing the other hat.  The first was the IRETD that should
        ; have entered V86 mode, on the VDM's first evening; v86_run
        ; clears NT for exactly this reason and has ever since.  The bit
        ; is never set on purpose - it arrives from whatever the BIOS
        ; left in FLAGS before the switch to protected mode - and it
        ; survives all the way to here because nothing in between has
        ; any reason to touch it.
        ;
        ; It cost one boot: the first entry to ring 3 faulted, and the
        ; recovery worked so well that the SECOND one succeeded, because
        ; taking an interrupt gate is itself what clears NT.  A bug that
        ; fixes itself after one failure is a bug that gets shipped.
        pushfd
        and     dword ptr [esp],NOT EFL_NT
        popfd

        push    eax
        mov     eax,[r3_esp0]
        mov     dword ptr ds:[TSS_BASE + TSS_ESP0],eax
        pop     eax

        ; DS and ES have to be loaded HERE and not left to the IRETD.
        ; An inter-privilege IRET nulls any of DS, ES, FS and GS whose
        ; DPL is below the level being returned to, and SEL_DATA32 is a
        ; ring-0 selector - so all four would arrive at the program as
        ; zero, and its first memory reference would fault on a null
        ; selector rather than run.
        mov     ds,word ptr ss:[r3_seldata]
        mov     es,word ptr ss:[r3_seldata]
        mov     fs,word ptr ss:[r3_selnull]
        mov     gs,word ptr ss:[r3_selnull]

        push    dword ptr ss:[r3_seldata]       ; SS
        push    dword ptr ss:[r3_esp]
        ; IOPL 0, so CLI, STI, IN and OUT come back to us; IF set,
        ; because the machine's timer must keep running whatever the
        ; program thinks it has done to the flag.
        push    EFL_IF OR 2
        push    dword ptr ss:[r3_selcode]       ; CS
        push    dword ptr ss:[r3_eip]
        iretd

;=====================================================================
; Pointer translation.
;
; A program's pointers are offsets in its own segment and the kernel's
; handlers want flat addresses, which is the same question the DOS box
; bridge and the DPMI host each answer for their own callers - so it is
; the same table, vdm_flags, and this is its third reader.
;
; r3_xlat: CF set if a pointer is outside the slot, which is a program
; bug rather than a fact about the file system, and is answered with
; "access denied" rather than by faulting the program: the call has not
; happened, nothing is half-done, and a program that gets an error back
; can say so.
;=====================================================================
r3_xlat:
        pushad
        call    r3_pflags                       ; -> BL = which registers
        call    r3_pfput                        ; ...remembered for the way out
        or      bl,bl
        jz      rxl_done

        test    bl,P_DX
        jz      rxl_nodx
        mov     eax,dword ptr [ebp+F_EDX]
        call    r3_flat
        jc      rxl_bad
        mov     dword ptr [ebp+F_EDX],eax
rxl_nodx:
        test    bl,P_SI
        jz      rxl_nosi
        mov     eax,dword ptr [ebp+F_ESI]
        call    r3_flat
        jc      rxl_bad
        mov     dword ptr [ebp+F_ESI],eax
rxl_nosi:
        test    bl,P_DI
        jz      rxl_nodi
        mov     eax,dword ptr [ebp+F_EDI]
        call    r3_flat
        jc      rxl_bad
        mov     dword ptr [ebp+F_EDI],eax
rxl_nodi:
        test    bl,P_BX
        jz      rxl_done
        mov     eax,dword ptr [ebp+F_EBX]
        call    r3_flat
        jc      rxl_bad
        mov     dword ptr [ebp+F_EBX],eax
rxl_done:
        popad
        clc
        ret
rxl_bad:
        popad
        stc
        ret

;---------------------------------------------------------------------
; r3_pflags - which of this call's registers hold pointers.  BL, in the
;             P_* bits; everything else preserved.
;
; It is mostly vdm_flags, which is the table the DOS box bridge and the
; DPMI host already read, and TWO FUNCTIONS THAT ARE NOT IN IT AND
; CANNOT BE - the same two the bridge answers by hand in vdm_country
; and vdm_ctryext, for the same reasons:
;
;   38h takes a buffer in DS:DX EXCEPT when DX is FFFFh, where it is a
;   sentinel meaning "set".  A blanket P_DX turns the sentinel into an
;   ordinary address, the kernel reads a set as a get, and 34 bytes go
;   somewhere nobody asked for.
;
;   65h disagrees with itself about which register: 01h takes ES:DI,
;   21h and 22h take DS:DX, and 20h and 23h take a character in DL and
;   no pointer at all.  One flag byte keyed on the function has to be
;   wrong for at least two of those.
;
; BOTH DIRECTIONS ASK THE SAME QUESTION, which is why this is a routine
; and not two copies: a register converted on the way in and not
; converted back on the way out is a program that finds its own pointer
; has moved.
;---------------------------------------------------------------------
r3_pflags:
        mov     al,byte ptr [ebp+F_EAX+1]       ; AH: the function
        mov     ah,byte ptr [ebp+F_EAX]         ; AL: the subfunction
        cmp     al,38h
        je      rpf_country
        cmp     al,65h
        je      rpf_ctryext
        call    vdm_flags
        ret

rpf_country:
        mov     bl,P_DX
        cmp     dword ptr [ebp+F_EDX],0FFFFh
        jne     rpf_out
        xor     bl,bl                           ; a set: leave DX alone
        ret

rpf_ctryext:
        xor     bl,bl
        cmp     ah,01h
        je      rpf_ce_di
        cmp     ah,21h
        je      rpf_ce_dx
        cmp     ah,22h
        jne     rpf_out                         ; 20h, 23h and the refused
        jmp     short rpf_ce_dx                 ; ones carry no pointer
rpf_ce_di:
        mov     bl,P_DI
        ret
rpf_ce_dx:
        mov     bl,P_DX
rpf_out:
        ret

;--- r3_pfput / r3_pfget - BL, kept for the length of one call -------
; The slot is (EXEC depth, handler depth), which is exactly the two
; ways one of these calls can be open inside another.
r3_pfput:
        push    eax
        call    r3_pfslot
        mov     [eax],bl
        pop     eax
        ret

r3_pfget:
        push    eax
        call    r3_pfslot
        mov     bl,[eax]
        pop     eax
        ret

r3_pfslot:
        mov     eax,[exec_depth]
        cmp     eax,EXEC_MAX
        jb      rps_ok
        xor     eax,eax                         ; cannot happen; if it did,
rps_ok:                                         ; one slot is better than
        imul    eax,R3_CALLMAX + 1              ; somebody else's memory
        add     eax,[r3_calldep]
        add     eax,OFFSET r3_pf
        ret

;---------------------------------------------------------------------
; r3_unxlat - the converted registers, back as the program left them.
;
; IT SUBTRACTS RATHER THAN REMEMBERING THE ORIGINALS.  The DPMI host
; keeps copies because a selector:offset pair cannot be recovered from
; the flat address it made; here the conversion is one addition, so its
; inverse is one subtraction and there is nothing to keep.  The test
; before each subtraction is the other half of that: a register is put
; back only if it still holds the address this call put there, so a
; handler that answered in one of them keeps its answer.
;
; WHAT IT DOES HAVE TO REMEMBER IS WHICH REGISTERS, and that is not the
; same thing at all.  r3_pflags reads the function number out of AH,
; and by the time a call returns AH is the ANSWER - so asking the same
; question on the way out gets an answer about a function nobody
; called.  That is the trap vdm_result's comment describes, arrived at
; from the other side.
;
; It is remembered PER CALL LEVEL rather than in one place, because
; these calls nest two ways: a critical error runs a handler that makes
; DOS calls of its own, and a program that EXECs another leaves its own
; call open for the whole of the child's life.  One byte per
; (EXEC depth, handler depth) pair covers both, and needs no unwinding
; when an abort throws a level away - the next call at that level
; simply writes over it.
;---------------------------------------------------------------------
r3_unxlat:
        pushad
        call    r3_pfget
        or      bl,bl
        jz      rux_done

        test    bl,P_DX
        jz      rux_nodx
        mov     eax,dword ptr [ebp+F_EDX]
        call    r3_off
        jc      rux_nodx
        mov     dword ptr [ebp+F_EDX],eax
rux_nodx:
        test    bl,P_SI
        jz      rux_nosi
        mov     eax,dword ptr [ebp+F_ESI]
        call    r3_off
        jc      rux_nosi
        mov     dword ptr [ebp+F_ESI],eax
rux_nosi:
        test    bl,P_DI
        jz      rux_nodi
        mov     eax,dword ptr [ebp+F_EDI]
        call    r3_off
        jc      rux_nodi
        mov     dword ptr [ebp+F_EDI],eax
rux_nodi:
        test    bl,P_BX
        jz      rux_done
        mov     eax,dword ptr [ebp+F_EBX]
        call    r3_off
        jc      rux_done
        mov     dword ptr [ebp+F_EBX],eax
rux_done:
        popad
        ret

;---------------------------------------------------------------------
; r3_flat - EAX = a slot offset -> EAX = the flat address.  CF if it is
;           not an offset this program could have meant.
;
; Zero is refused first and by name.  It is the commonest wrong pointer
; there is, the guard page would catch the program dereferencing it,
; and this is the other half of the same guard: a null handed to DOS is
; refused rather than turned into a perfectly good address at the foot
; of the slot.
;---------------------------------------------------------------------
r3_flat:
        or      eax,eax
        jz      rf_bad
        cmp     eax,[r3_size]
        jae     rf_bad
        add     eax,[r3_base]
        clc
        ret
rf_bad:
        stc
        ret

;--- r3_off - the inverse, and CF says "this is not one of ours" rather
;--- than being an error: the caller uses it to decide whether to undo.
r3_off:
        sub     eax,[r3_base]
        jc      ro_bad                          ; below the slot
        cmp     eax,[r3_size]
        jae     ro_bad
        clc
        ret
ro_bad:
        stc
        ret

;=====================================================================
; The vectors a program hooks, and how the kernel gets INTO one.
;
; A program hooks INT 23h and INT 24h through the ordinary 25h call,
; and until this release the handler was simply an address in an IDT
; gate, because a program's code and the kernel's were the same code at
; the same privilege.  They are not any more, and AN INTERRUPT GATE
; CANNOT REACH RING 3: control does not travel outward through one, so
; `int 24h` from inside the disk layer can no longer arrive at a
; handler that lives in a program.
;
; So a vector a ring-3 program hooks gets a KERNEL stub in the gate,
; the offset is remembered here, and the stub does what everything else
; that enters ring 3 does - builds a frame and IRETDs.  The way back is
; the two bytes at PSP+54h: the handler ends in the IRET its
; specification requires, and the address that IRET returns to is an
; INT 21h the kernel recognises by the EIP it arrives from.  DPMI's
; real-mode callbacks solve the identical problem the identical way,
; which is the argument for doing it like this and not inventing a
; second mechanism.
;
; WHICH VECTORS: the six that DOS raises INTO a program.  Any other
; vector a program sets is remembered, so that 35h answers what 25h
; stored and a save-and-restore pair is honest, but no gate is
; installed for it - hooking the keyboard interrupt from a program is
; exactly the thing this release exists to stop, and a program that
; tries has not been lied to, it has been ignored.
;=====================================================================
r3_hookable:
        db      1Bh, 1Ch, 22h, 23h, 24h, 28h
R3_HOOKS        EQU     6

;--- one stub per hookable vector: all it does is say which one ------
; Written out longhand rather than made with a macro.  Six of them is
; not enough repetition to be worth a macro that has to build labels
; out of hex digits, and the table below has to name them anyway.
r3s1B:  push    1Bh
        jmp     r3_thunk
r3s1C:  push    1Ch
        jmp     r3_thunk
r3s22:  push    22h
        jmp     r3_thunk
r3s23:  push    23h
        jmp     r3_thunk
r3s24:  push    24h
        jmp     r3_thunk
r3s28:  push    28h
        jmp     r3_thunk

r3_stubs:
        dd      OFFSET r3s1B, OFFSET r3s1C, OFFSET r3s22
        dd      OFFSET r3s23, OFFSET r3s24, OFFSET r3s28

;---------------------------------------------------------------------
; r3_stubfor - AL = a vector -> EAX = the stub that serves it, or CF if
;              this is not a vector a program may be entered through.
;---------------------------------------------------------------------
r3_stubfor:
        push    ecx
        push    esi
        movzx   eax,al
        mov     esi,OFFSET r3_hookable
        xor     ecx,ecx
rsf_scan:
        cmp     al,[esi+ecx]
        je      rsf_hit
        inc     ecx
        cmp     ecx,R3_HOOKS
        jb      rsf_scan
        pop     esi
        pop     ecx
        stc
        ret
rsf_hit:
        mov     eax,[r3_stubs+ecx*4]
        pop     esi
        pop     ecx
        clc
        ret

;---------------------------------------------------------------------
; r3_hook / r3_unhook - remember (or forget) that vector AL belongs to
;                       the program, at offset EDI.
;
; Called from set_vec, which is the one place that knows how a vector
; is stored - the PSP save and restore go through it too, so a vector
; hooked by a child and put back by 4Ch takes the same route out as it
; took in.
;---------------------------------------------------------------------
r3_hook:
        pushad
        movzx   ebx,al
        mov     [r3vec+ebx*4],edi

        ; WHOSE handler it is, and not just where.  An offset means
        ; nothing without the slot it is an offset into, and the two
        ; slots hold different programs: the interpreter's INT 24h
        ; handler has to be reachable while a child program is running,
        ; which is most of when disks fail.  The depth names the slot.
        mov     eax,[exec_depth]
        mov     [r3vdep+ebx*4],eax

        movzx   eax,bl
        call    r3_stubfor
        jc      rh_nogate                       ; remembered, not installed
        mov     edi,eax
        movzx   eax,bl
        call    dword ptr ds:[IOS_SETVEC]
rh_nogate:
        popad
        ret

r3_unhook:
        pushad
        movzx   ebx,al
        mov     dword ptr [r3vec+ebx*4],0
        mov     dword ptr [r3vdep+ebx*4],0
        popad
        ret

;=====================================================================
; r3_thunk - the kernel has executed `int n` and vector n belongs to a
;            ring-3 program.
;
; Entered at ring 0 from ring-0 code, so there is no stack switch and
; no SS:ESP on the frame: [esp] is the vector this stub pushed, and the
; three dwords above it are the ordinary EIP, CS and EFLAGS of the INT.
;
; The general registers ARE THE ARGUMENT.  crit_raise sets AH, AL and
; DI and then executes INT 24h; the handler must see exactly those, so
; they are carried across untouched and come back the same way, with
; only EAX replaced by whatever the handler answered.
;
; The handler runs on the PROGRAM'S OWN STACK, below wherever it was
; when it last called into the kernel, which is where DOS always ran
; one and is the only stack a ring-3 handler could use.
;=====================================================================
r3_thunk:
        pushad
        cld

        ; WHICH DIRECTION IS THIS GOING?  All six of these vectors are
        ; ones DOS raises INTO a program, and the frame says whether
        ; that is what happened: a ring-0 INT pushes three dwords and a
        ; ring-3 one pushes five.  A program executing INT 24h itself
        ; is not a thing DOS ever asked for, and answering it would
        ; mean running the program's handler on a stack whose depth we
        ; do not know - so it returns, doing nothing.
        ;
        ; The test is made through SS, before DS is touched, because
        ; the way back from here must leave the segment registers
        ; exactly as they arrived: an inter-privilege IRETD nulls any
        ; data selector whose DPL is too low, so a program sent back
        ; holding the kernel's DS would fault on its next instruction.
        test    byte ptr ss:[esp+40],3          ; CS of the interrupted code
        jnz     rt_fromr3

        ; From here on the caller is kernel code, so DS and ES are
        ; already the kernel's - and they stay the kernel's, because
        ; whatever resumes at this frame is kernel code too.
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     es,ax

        ; Is there a handler, and is there room to call it?  Neither
        ; can normally fail - the gate is only installed while a
        ; handler is hooked - so both are answered by doing nothing,
        ; which for these vectors means "the program was not
        ; interested" and is a safe answer.
        mov     ebx,[esp+32]                    ; the vector, above pushad
        mov     eax,[r3vec+ebx*4]
        or      eax,eax
        jz      rt_ignore
        cmp     dword ptr [r3_calldep],R3_CALLMAX
        jae     rt_ignore
        cmp     dword ptr [r3_active],0
        je      rt_ignore

        ; The record that lets the return find its way back here, and
        ; put back what the excursion is about to change.
        mov     edi,[r3_calldep]
        imul    edi,RC_LEN
        add     edi,OFFSET r3_calls
        mov     [edi+RC_VEC],ebx
        mov     ecx,[r3_esp0]
        mov     [edi+RC_ESP0],ecx
        mov     ecx,[r3_depth]
        mov     [edi+RC_DEPTH],ecx
        mov     ecx,[r3_lastesp]
        mov     [edi+RC_LASTESP],ecx
        mov     [edi+RC_ESP],esp

        ; WHOSE HANDLER IS IT?  An offset is an offset into a slot, and
        ; the slot that is loaded is not always the slot the handler
        ; was installed from: the commonest critical error of all is a
        ; disk failing under a CHILD program, and the handler that has
        ; to answer for it is the interpreter's, one slot over.
        ;
        ; Three cases, and the third is the interesting one.
        mov     edx,[r3vdep+ebx*4]
        cmp     edx,[r3_depth]
        je      rt_here                         ; ours: run it where we are
        or      edx,edx
        jnz     rt_ignore

        ; The interpreter's handler, over a running program.  Its slot
        ; is untouched - the shell has one of its own, which is the
        ; whole reason it has one - so switching the descriptors to it
        ; makes the handler's code and its data reachable again, and
        ; the stack it runs on is the shell's own, exactly where the
        ; shell was when it called EXEC.  That is where DOS ran one.
        ;
        ; A handler belonging to any OTHER depth is refused, and the
        ; refusal is honest rather than cautious: nesting is done by
        ; PARKING the occupant, so a program at depth 1 with a child
        ; running has had its code copied into the arena and overwritten
        ; - there is nothing at that offset to call any more.  The
        ; ring-0 build had the same hole and no way to notice it.
        xor     eax,eax
        call    r3_useslot                      ; the shell's slot, and its
                                                ; two descriptors
        mov     ecx,dword ptr [exec_stk+XN_EBP] ; the shell's INT 21h frame,
        or      ecx,ecx                         ; parked by its own 4Bh
        jz      rt_unwind
        mov     ecx,[ecx+F_ESP3]                ; and its ring-3 ESP in it
        mov     [r3_lastesp],ecx
        mov     eax,[r3vec+ebx*4]               ; (r3_useslot left EAX alone,
                                                ; but say it rather than rely
                                                ; on it)
rt_here:
        ; Where the handler's stack starts: below the program's own
        ; ESP as of its last entry to the kernel.  Three dwords are the
        ; frame its IRET will pop.
        mov     ecx,[r3_lastesp]
        cmp     ecx,12
        jb      rt_unwind
        cmp     ecx,[r3_size]
        ja      rt_unwind
        sub     ecx,12
        mov     edx,[r3_base]
        add     edx,ecx
        mov     dword ptr [edx],SLOT_PSP + PSP_R3RET    ; EIP: the trampoline
        mov     dword ptr [edx+4],SEL_PROG_CS
        mov     dword ptr [edx+8],EFL_IF OR 2

        inc     dword ptr [r3_calldep]

        ; EVERY DOS CALL THE HANDLER MAKES MUST LAND BELOW THIS FRAME,
        ; not on top of it.  dpmi_docall had to learn the same thing:
        ; the ring-0 stack pointer in the TSS names the TOP of a stack
        ; that is already in use here, and leaving it there would have
        ; the handler's first INT 21h - or the first timer tick - write
        ; over the frame this call has to come back to.
        mov     dword ptr ds:[r3_esp0],esp

        mov     [r3_eip],eax                    ; the handler
        mov     [r3_esp],ecx                    ; on its own stack

        ; The registers the handler is to see, back into the CPU, and
        ; then away.  THE STACK IS THEN DROPPED CLEAR OF THE FRAME:
        ; r3_enter builds an IRETD frame of its own with five more
        ; pushes, and those would land exactly on the registers just
        ; restored - which nothing would notice until the handler
        ; returned and resumed the kernel with eight dwords of stale
        ; interrupt frame in its registers.
        mov     esp,[edi+RC_ESP]
        popad
        sub     esp,64
        jmp     r3_enter

;--- the switch was made and then something else refused: put the
;--- descriptors back before leaving, or the running program would
;--- resume looking at the interpreter's memory.
rt_unwind:
        mov     eax,[edi+RC_DEPTH]
        call    r3_useslot
        mov     eax,[edi+RC_LASTESP]
        mov     [r3_lastesp],eax

;--- nobody to call, or no room to call them.  ANSWER FAIL rather than
;--- returning whatever was in EAX: the one caller that reads an answer
;--- is crit_raise, and the answer it would otherwise take is the drive
;--- and flags it passed IN, masked to two bits - which is a random
;--- choice between retrying for ever and terminating the program.
;--- Fail is what int24_default answers, and it is what "nobody was
;--- listening" ought to mean.  For 1Bh, 1Ch and 28h nothing reads it.
rt_ignore:
        mov     dword ptr ss:[esp+R3_PAEAX],CERR_FAIL
rt_fromr3:
        popad
        add     esp,4                           ; the vector
        iretd

;=====================================================================
; r3_ret - the trampoline at PSP+54h has executed its INT 21h, so the
;          handler has finished.  EBP -> the frame int21_entry built.
;
; Recognised by the EIP it arrives from and not by a function number,
; because the registers belong to the handler by now: an INT 24h
; handler answers in AL, and a trampoline that loaded a function number
; into AX would destroy the answer it exists to carry.
;=====================================================================
r3_ret:
        cli
        cmp     dword ptr [r3_calldep],0
        je      r3r_stray

        dec     dword ptr [r3_calldep]
        mov     edi,[r3_calldep]
        imul    edi,RC_LEN
        add     edi,OFFSET r3_calls

        mov     eax,[edi+RC_ESP0]               ; the stack the interrupted
        mov     [r3_esp0],eax                   ; level was using

        ; ...and the slot it was in, which the handler may not have
        ; been.  This is the other end of the interpreter's handler
        ; running over a child: the descriptors go back to the child
        ; before it is resumed, or it comes back to life looking at the
        ; shell's memory through its own pointers.
        mov     eax,[edi+RC_LASTESP]
        mov     [r3_lastesp],eax
        mov     eax,[edi+RC_DEPTH]
        call    r3_useslot

        ; This trampoline call is an INT 21h that will never reach
        ; i21_r0, so its own increment of the InDOS count has to come
        ; off here.  The count belongs to the DOS call the handler
        ; interrupted, which is still open and still counted.
        dec     byte ptr ds:[DOS_INDOS]

        mov     eax,[ebp+F_EAX]                 ; what the handler answered
        mov     esp,[edi+RC_ESP]
        mov     [esp+R3_PAEAX],eax              ; ...into the frame that
        popad                                   ; resumes the INT n
        add     esp,4                           ; the vector
        iretd

;--- an INT 21h from the trampoline address with no call outstanding.
;--- A program can do this: the two bytes are in its own segment and
;--- nothing stops it executing them.  It is not a way into the kernel
;--- and it is not an error worth killing anybody over, so it is
;--- answered like any unimplemented function.
r3r_stray:
        sti
        mov     ax,ERR_FUNC
        jmp     i21_fail

;=====================================================================
; r3_abandon - a handler has decided not to come back.
;
; COMMAND does this for both of its handlers: a ^C at the prompt and an
; Abort at the critical-error prompt both reset the stack and jump to
; the main loop rather than IRETing, which is the oldest trick in
; interpreter writing and works perfectly well at ring 3 - the program
; abandons its own stack frames and the kernel's are simply never
; resumed.
;
; What it cannot do is leave the ring-0 side believing a handler is
; still running.  Called from 21h/F1h AL=5, which COMMAND already
; issues as its first act on the way home for exactly this kind of
; reason.
;
; THE RING-0 STACK LEVEL IS RESTORED, NOT RESET, and the difference is
; a whole class of bug.  The first version put it back to the top on
; the reasoning that the program is starting again from its main loop -
; true for the interpreter with nothing running under it, and wrong the
; moment anything is.  Consider the commonest use of this call there
; is: a disk fails under a CHILD program, COMMAND's INT 24h handler
; asks, the answer is Abort.  The handler then terminates the child
; with 4Ch - and the shell's own 4Bh is still open, its frame sitting
; at the top of this very stack, waiting for that 4Ch to return
; through it.  Starting the 4Ch at the top writes the new frame exactly
; over the old one, so the EXEC comes back to a frame full of its own
; terminate call.  That is bug two of v0.33 arriving from a second
; direction, and it would have been bug three.
;
; What it should be is simply what it was before the excursion started,
; which is the one thing r3_thunk already wrote down.  The OUTERMOST
; record holds it: handler calls nest, and undoing all of them means
; going back to the level the first one interrupted.
;=====================================================================
r3_abandon:
        cmp     dword ptr [r3_calldep],0
        je      ra_none                 ; no handler running, nothing to undo -
                                        ; and in particular nothing to assume
                                        ; about where the stack should be
        push    eax
        mov     eax,dword ptr [r3_calls+RC_ESP0]
        mov     [r3_esp0],eax
        pop     eax
        mov     dword ptr [r3_calldep],0
ra_none:
        ret

;=====================================================================
; The in-slot heap - 21h/48h, 49h and 4Ah for a native program.
;
; A program used to be handed memory out of the kernel's arena, above
; 2MB, and now it cannot see the kernel's arena.  Rather than answer
; with an address that would fault the moment it was used, 48h
; allocates out of the program's own slot: the space between the end of
; its BSS and the stack reservation at the top.
;
; That is what DOS did.  A .COM program owned its segment and every
; allocation came out of it, which is why 4Ah "shrink yourself" existed
; at all.  It also travels correctly: EXEC parks the whole slot, so a
; nested program's heap goes into the park and comes back with it,
; where an arena block would have needed an owner and a sweep.
;
; The header is the arena's, sixteen bytes with the same signatures,
; because two block formats in one kernel is one too many.  The owner
; field is unused here - a slot has exactly one program in it - and is
; kept at zero rather than removed so a dump of either heap reads the
; same way.
;=====================================================================

;---------------------------------------------------------------------
; r3h_init - EAX = the first free offset (the end of the BSS), EBX =
;            bytes of stack to keep at the top.  One free block.
;---------------------------------------------------------------------
r3h_init:
        pushad
        add     eax,15                          ; headers stay 16-aligned
        and     eax,NOT 15
        mov     [r3h_lo],eax

        mov     ecx,[r3_size]
        cmp     ebx,1000h                       ; whatever the header asked
        jae     rhi_stk                         ; for, and never less than 4K
        mov     ebx,1000h
rhi_stk:
        sub     ecx,ebx
        and     ecx,NOT 15
        mov     [r3h_hi],ecx

        ; No room for a header and a byte?  Then there is no heap, and
        ; 48h will say so rather than build one that overlaps a stack.
        mov     edx,eax
        add     edx,MCB_LEN + MCB_LEN
        cmp     edx,ecx
        jbe     rhi_room
        mov     [r3h_hi],eax
        popad
        ret
rhi_room:
        mov     edi,[r3_base]
        add     edi,eax
        mov     dword ptr [edi+MCB_SIG],SIG_FREE
        sub     ecx,eax
        sub     ecx,MCB_LEN
        mov     [edi+MCB_SIZE],ecx
        mov     dword ptr [edi+MCB_OWNER],0
        popad
        ret

;---------------------------------------------------------------------
; r3h_alloc - EBX = bytes.  CF clear: EAX = the offset of the payload.
;             CF set: EAX = the largest payload that would have fitted.
;
; First fit, and only first fit: 58h's strategies are about an arena
; several programs share, and a slot belongs to one program.
;---------------------------------------------------------------------
r3h_alloc:
        push    ebx
        push    ecx
        push    edx
        push    edi
        add     ebx,15
        and     ebx,NOT 15
        xor     edx,edx                         ; the largest seen
        mov     ecx,[r3h_lo]
rha_walk:
        cmp     ecx,[r3h_hi]
        jae     rha_none
        mov     edi,[r3_base]
        add     edi,ecx
        cmp     dword ptr [edi+MCB_SIG],SIG_FREE
        jne     rha_next
        call    r3h_join                        ; free neighbours first
        mov     eax,[edi+MCB_SIZE]
        cmp     eax,edx
        jbe     rha_notbig
        mov     edx,eax
rha_notbig:
        cmp     eax,ebx
        jb      rha_next
        jmp     short rha_take
rha_next:
        add     ecx,MCB_LEN
        add     ecx,[edi+MCB_SIZE]
        jmp     short rha_walk

rha_take:
        ; Split it, if what is left over is worth a header.
        mov     eax,[edi+MCB_SIZE]
        sub     eax,ebx
        cmp     eax,MCB_LEN + 16
        jb      rha_whole
        sub     eax,MCB_LEN
        push    ecx
        lea     ecx,[edi+MCB_LEN]
        add     ecx,ebx                         ; where the remainder starts
        mov     dword ptr [ecx+MCB_SIG],SIG_FREE
        mov     [ecx+MCB_SIZE],eax
        mov     dword ptr [ecx+MCB_OWNER],0
        pop     ecx
        mov     [edi+MCB_SIZE],ebx
rha_whole:
        mov     dword ptr [edi+MCB_SIG],SIG_USED
        lea     eax,[ecx+MCB_LEN]               ; the payload, as an offset
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret

rha_none:
        mov     eax,edx
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;--- r3h_join - EDI -> a free header; swallow the free blocks after it.
r3h_join:
        push    eax
        push    ebx
        push    ecx
rhj_more:
        mov     eax,[edi+MCB_SIZE]              ; where the next one is
        lea     ebx,[edi+MCB_LEN]
        add     ebx,eax
        mov     ecx,ebx
        sub     ecx,[r3_base]
        cmp     ecx,[r3h_hi]
        jae     rhj_done
        cmp     dword ptr [ebx+MCB_SIG],SIG_FREE
        jne     rhj_done
        mov     ecx,[ebx+MCB_SIZE]
        add     ecx,MCB_LEN
        add     [edi+MCB_SIZE],ecx
        jmp     short rhj_more
rhj_done:
        pop     ecx
        pop     ebx
        pop     eax
        ret

;--- r3h_find - EDX = a payload offset -> EDI = its header, flat.
;--- CF if that is not the address of a block this heap handed out.
r3h_find:
        push    eax
        cmp     edx,MCB_LEN
        jb      rhf_bad
        mov     eax,edx
        sub     eax,MCB_LEN
        cmp     eax,[r3h_lo]
        jb      rhf_bad
        cmp     eax,[r3h_hi]
        jae     rhf_bad
        mov     edi,[r3_base]
        add     edi,eax
        cmp     dword ptr [edi+MCB_SIG],SIG_USED
        jne     rhf_bad
        pop     eax
        clc
        ret
rhf_bad:
        pop     eax
        stc
        ret

;--- r3h_free - EDX = the payload offset.  CF if it is not one.
r3h_free:
        call    r3h_find
        jc      rhr_bad
        mov     dword ptr [edi+MCB_SIG],SIG_FREE
        call    r3h_join
        clc
        ret

;--- r3h_resize - EDX = the payload offset, EBX = the new size.
;--- CF with EAX = the largest it could have grown to.
r3h_resize:
        call    r3h_find
        jc      rhr_bad
        add     ebx,15
        and     ebx,NOT 15

        ; Growing means taking in the free block above, if there is
        ; one.  IT IS JOINED FIRST AND GIVEN BACK IF THE SUM IS STILL
        ; NOT ENOUGH, and the giving back is the half that matters: the
        ; arena had exactly this bug once, where a refused grow kept
        ; the space it had absorbed, so every refusal ate the heap a
        ; little.  The original size is remembered before anything is
        ; joined, which is what makes the undo exact.
        push    ebx
        mov     ebx,[edi+MCB_SIZE]              ; what it was
        call    r3h_join                        ; (the sig stays 'M': join
                                                ; walks forward from here and
                                                ; never looks at this one)
        mov     eax,[edi+MCB_SIZE]              ; the most it could be
        pop     ecx                             ; what was asked for
        cmp     ecx,eax
        ja      rhr_giveback

        mov     ebx,ecx                         ; it fits: cut it to size
        call    rhr_split
        clc
        ret

rhr_giveback:
        call    rhr_split                       ; EBX is still the old size
        stc                                     ; EAX = the largest possible
        ret
rhr_bad:
        xor     eax,eax
        stc
        ret

;--- rhr_split - EDI -> a used header, EBX = the size it should be.
;--- Anything left over becomes a free block.
rhr_split:
        push    eax
        push    ecx
        mov     eax,[edi+MCB_SIZE]
        sub     eax,ebx
        cmp     eax,MCB_LEN + 16
        jb      rhs_out
        sub     eax,MCB_LEN
        lea     ecx,[edi+MCB_LEN]
        add     ecx,ebx
        mov     dword ptr [ecx+MCB_SIG],SIG_FREE
        mov     [ecx+MCB_SIZE],eax
        mov     dword ptr [ecx+MCB_OWNER],0
        mov     [edi+MCB_SIZE],ebx
rhs_out:
        pop     ecx
        pop     eax
        ret

;=====================================================================
; pm3_dispatch - IOS_PM3HOOK.  Ring-3 code has faulted on something the
;                monitor could not emulate, and there are two kinds of
;                ring-3 code now.
;
;   EAX = vector, EDX = error code, ECX = EIP, EBX = the CS.
;
; The selector says which, and it says it exactly: a native program
; runs in SEL_PROG_CS and a DPMI client runs out of its own LDT, so
; there is no case where the answer is a guess.  Neither branch
; returns.
;=====================================================================
pm3_dispatch:
        cmp     bx,SEL_PROG_CS
        je      r3_fault
        jmp     dpmi_fault

;=====================================================================
; r3_fault - a native ring-3 program has faulted on something PMIO
;            could not emulate.  EAX = vector, EDX = error code,
;            ECX = EIP, EBX = the CS it faulted in.
;
; Reached from dpmi_pm3 - the one hook PMIO has for a ring-3 fault -
; which sorts the two kinds of ring-3 code by the selector.  This never
; returns: the program is finished either way.
;
; AND THIS IS THE ONE THAT IS DIFFERENT FROM v0.30.  A ring-0 program's
; fault was survivable but not contained: it had had the run of the
; machine right up to the instant it faulted, so the recovery could
; only say that it had bought back a prompt, not that anything was
; intact.  This one is contained by construction.  The program could
; not have written anything outside its own slot, and the slot is about
; to be handed back or reloaded - so for the first time the honest
; report is that nothing else is affected.
;=====================================================================
r3_fault:
        cli
        mov     [r3_fvec],eax
        mov     [r3_ferr],edx
        mov     [r3_feip],ecx

        ; THE LEVEL, NOT THE TOP - and this cost a release.  The ring-0
        ; stack is ONE stack shared by every EXEC level: 4Bh hands the
        ; child the parent's frame base as its ceiling and the child
        ; works downwards, so a program at depth 1 has its parent's
        ; unfinished 4Bh frame sitting immediately ABOVE it.
        ;
        ; Setting ESP to R3_KSTACK put the stack pointer back at the
        ; SHELL's level, above all of that - and the dozen nested calls
        ; below, which only print a message, then wrote their return
        ; addresses straight down through the waiting 4Bh frame.  The
        ; report came out perfectly; it was the exit that died, on the
        ; IRETD at the end of int21_entry, returning through a frame
        ; this routine had just overwritten.  From outside: a program
        ; faults, the fault is reported correctly, and THEN the machine
        ; says "CPU exception" on its own account and reloads the
        ; interpreter - which is AUTOEXEC.BAT running a second time.
        ;
        ; r3_esp0 is the running program's own level, kept by 4Bh and
        ; lowered again while one of its handlers is in flight, so it
        ; is below the parent's frame in every case.  At depth 0 it IS
        ; R3_KSTACK, which is why the shell's own crashes always looked
        ; fine and hid this.
        ;
        ; v0.34 fixed exactly this mistake in r3_abandon, four hundred
        ; lines away, and did not think to look here.
        mov     esp,[r3_esp0]                   ; ours again, whatever the
        sti                                     ; program was doing on it

        mov     esi,OFFSET msgR3Fault
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,[r3_fvec]
        call    vdm_hex8
        mov     esi,OFFSET msgR3At
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,[r3_feip]
        call    vdm_hex32
        mov     esi,OFFSET msgR3Err
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,[r3_ferr]
        call    vdm_hex8

        ; A page fault at an offset in the first page is a store or a
        ; load through a null pointer, and saying so is most of the
        ; value of catching it - "exception 0E" names the mechanism and
        ; no bug at all.  CR2 is a linear address, so the offset is
        ; what it says minus the slot base.
        cmp     dword ptr [r3_fvec],14
        jne     r3f_nopf
        mov     eax,cr2
        sub     eax,[r3_base]
        cmp     eax,1000h
        jae     r3f_notnull
        mov     esi,OFFSET msgR3Null
        call    dword ptr ds:[IOS_CONSTR]
        jmp     short r3f_nopf
r3f_notnull:
        mov     esi,OFFSET msgR3Addr
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,cr2
        sub     eax,[r3_base]
        call    vdm_hex32
r3f_nopf:
        mov     esi,OFFSET msgR3CR
        call    dword ptr ds:[IOS_CONSTR]

        ; And out through the ordinary exit, exactly as v0.30 does it
        ; for a ring-0 fault: the program terminates with errorlevel
        ; 255, its parent's EXEC returns, and the batch file that
        ; called it carries on to the next line.
        jmp     dos_fault

;=====================================================================
; Data
;=====================================================================
r3_base         dd  0                   ; the running slot, flat
r3_size         dd  0                   ; and how big it is
r3_depth        dd  0                   ; and whose it is, as an EXEC depth
r3_active       dd  0                   ; a native program has been started
r3_lastesp      dd  0                   ; its ESP when it last called in
r3_esp0         dd  R3_KSTACK           ; the ring-0 stack it will use
r3_calldep      dd  0                   ; handler calls in flight
r3_eip          dd  0                   ; where r3_enter is going
r3_esp          dd  0                   ; and on what stack

; Read by r3_enter when there is no register left to hold them.
r3_selcode      dw  SEL_PROG_CS
                dw  0
r3_seldata      dw  SEL_PROG_DS
                dw  0
r3_selnull      dw  0
                dw  0

r3h_lo          dd  0                   ; the slot heap, as offsets
r3h_hi          dd  0

r3_fvec         dd  0
r3_ferr         dd  0
r3_feip         dd  0

r3_pf           db  EXEC_MAX * (R3_CALLMAX + 1) dup(0)
r3_calls        db  R3_CALLMAX * RC_LEN dup(0)
r3vec           dd  256 dup(0)          ; a program's own handlers...
r3vdep          dd  256 dup(0)          ; ...and which program's

msgR3Fault      db  13,10,'*** Program fault: exception ',0
msgR3At         db  ' at ',0
msgR3Err        db  ' err ',0
msgR3Addr       db  ' addr ',0
msgR3Null       db  ' - through a null pointer',0
msgR3CR         db  13,10,0
