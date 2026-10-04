;=====================================================================
; RMCALL.INC - the 32-bit side of the protected-mode to real-mode thunk
;
; Lets protected-mode code call BIOS services.  The heavy lifting is in
; IORM.ASM (pm2rm); this is the wrapper that hands control over and
; picks the machine back up afterwards.
;
; Included by IOPM.ASM.
;=====================================================================

;=====================================================================
; rm_clear - zero the register frame.  Call this before setting up a
;            BIOS call so no stale values leak through.
;=====================================================================
rm_clear:
        push    eax
        push    ecx
        push    edi
        mov     edi,RMFRAME
        xor     eax,eax
        mov     ecx,RMFRAME_LEN / 4
        rep     stosd
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; rm_int - invoke real-mode interrupt AL with the registers in RMFRAME.
;
; On return RMFRAME holds what the BIOS gave back, including the flags
; (so CF lives in bit 0 of RMR_FLAGS).  Every 32-bit register is
; preserved across the call.
;
; This drops the CPU all the way back to real mode, so it must not be
; called from an interrupt handler.
;=====================================================================
rm_int:
        pushad
        pushfd
        cli

        ; THE NULL GUARD COMES OFF FOR THE EXCURSION.  Debug registers
        ; are not affected by the mode switch, so a BIOS routine that
        ; writes one of the first four interrupt vectors - which is
        ; entirely its business - would otherwise take a #DB in real
        ; mode and go wherever the real-mode vector 1 points.  What is
        ; being guarded is our own software, and none of ours runs
        ; between here and rmc_back.
        push    eax
        xor     al,al
        call    nt_arm
        pop     eax
        call    vd_rmdown               ; and VDS, which a box needs and a
                                        ; ROM in real mode must not see

        mov     byte ptr ds:[RMR_INTNO],al
        call    rq_down                 ; and the timer's count with it
        mov     dword ptr ds:[RMSAVE_ESP],esp
        mov     dword ptr ds:[RMRET],OFFSET rmc_back
        mov     word ptr ds:[RMRET+4],SEL_CODE32

        ; Hand over to the 16-bit stub.  It ends by far-jumping through
        ; RMRET, which lands us at rmc_back below.
        db      0EAh                    ; JMP FAR SEL_CODE16:RMT_PM2RM
        dd      RMT_PM2RM
        dw      SEL_CODE16

rmc_back:
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     es,ax
        mov     fs,ax
        mov     gs,ax
        mov     ss,ax
        mov     esp,dword ptr ds:[RMSAVE_ESP]
        cld
        push    eax
        mov     al,1                    ; and back on, if it is wanted at all
        call    nt_arm
        pop     eax
        call    vd_rmup
        call    rq_up                   ; and what interrupted the call
        popfd
        popad
        ret

;=====================================================================
; rq_down / rq_up - either side of a BIOS call.  Interrupts off.
;                   Everything preserved.
;
; The timer's count goes down to rq_timer (IORM.ASM) and comes back.
; And the lines that interrupted while the call was running are handed
; on: the RMT_IRQS entries noted them in GV_RMOWED, and the BIOS's own
; handlers dealt with the devices.
;
; A tick is owed to a DOS box the way any tick is (vpc_tick).  A box's
; line is owed to it if it would have been in protected mode
; (vpc_raise): the box's handler is not in the table the BIOS was
; given, so this is the only way the box hears of it.  A line the
; kernel owns outright was its owner's, and is not handed on.  Two on
; one line during one call are one, as they are on the real part while
; it is masked.
;=====================================================================
rq_down:
        push    eax
        mov     eax,dword ptr [vt_div]
        mov     dword ptr ds:[GV_VTDIV],eax
        mov     eax,dword ptr [vt_acc]
        mov     dword ptr ds:[GV_VTACC],eax
        mov     word ptr ds:[GV_RMOWED],0
        mov     eax,dword ptr [vpc_imr] ; the lines a box listens on, for
        not     eax                     ; rq_rtc
        and     eax,VP_GUEST
        mov     word ptr ds:[GV_RMBOX],ax
        pop     eax
        ret

rq_up:
        push    eax
        push    ebx
        mov     eax,dword ptr ds:[GV_VTACC]
        mov     dword ptr [vt_acc],eax
        movzx   eax,word ptr ds:[GV_RMOWED]
        btr     eax,0
        jnc     rqu_lines
        call    vpc_tick
rqu_lines:
        and     eax,VP_GUEST
rqu_next:
        bsf     ebx,eax
        jz      rqu_out
        btr     eax,ebx
        cmp     dword ptr [irq_vectors + ebx*4],OFFSET irq_shared
        jne     rqu_next
        call    vpc_raise
        jmp     short rqu_next
rqu_out:
        pop     ebx
        pop     eax
        ret

;=====================================================================
; rm_freeze - IOS_RMFREEZE, once, from PMDOS when CONFIG.SYS is
;             finished.  From here on a BIOS call runs with a copy of
;             the vector table as it is now (BIOS_IVT), and not with
;             the table at 0:0.
;
; THE TABLE AT 0:0 IS EVERY PROGRAM'S.  A DOS box writes it with 25h,
; a DPMI client with 0201h, and rm_int handed it to the BIOS as it
; found it - with interrupts on, because the BIOS's disk code waits on
; them.  So a handler a program had put on anything the BIOS reaches
; during a call ran in real mode, inside the kernel, with paging off:
; the timer, INT 1Ch, the clock's interrupt, INT 15h, and INT 13h, 1Ah,
; 14h and 17h themselves, which the kernel calls by number.  No page
; is read-only down there, so that was the end of the kernel's
; protection from its programs.  A program that died with INT 1Ch
; hooked took the machine with it at the next diskette read.  And an
; interrupt that protected mode had already owed to a box was run
; down there as well, and the box had it twice.
;
; THE COPY IS TAKEN NOW because CONFIG.SYS is where the machine is set
; up.  A DEVICE= driver or an INSTALL= program that hooks the BIOS is
; in it, and the BIOS goes on reaching it; what AUTOEXEC.BAT runs, and
; everything after, is a program and is not.  A box's own INT 13h, 1Ah
; and the rest still go through the box's own table: the monitor sends
; them there.
;
; TWO THINGS IN THE COPY ARE CHANGED.  INT 4Bh is what it was before
; the kernel put the VDS stub there, which is what vd_rmdown gives a
; BIOS call anyway: the stub is a HLT that means something only in a
; box.  And the sixteen interrupt lines go to IORM.ASM's RMT_IRQS
; entries, which note the line for rq_up and pass it on to where it
; went in the copy - all but the clock's, which goes there only when
; it is the BIOS's (rq_rtc).  They are put at IRQ_BASE and up, where
; protected mode has the 8259s send them, because from now on a BIOS
; call does not move the controllers' bases: moving them threw away
; whatever interrupt was waiting (pic_down, IORM.ASM).  08h-0Fh and
; 70h-77h in the copy are left as they were, for whatever calls them
; as software interrupts.
;
; THE VECTORS THE BIOS READS AS DATA - the diskette table at 1Eh, the
; fixed disks' at 41h and 46h - are read at 0:0 by address and not
; through the table an INT uses, so GV_RWDPT and the rest still work.
; And a park does not use this: it hands the machine over, and the
; table at 0:0 goes with it (park_land).
;=====================================================================
rm_freeze:
        pushad
        pushfd
        cli
        xor     al,al                   ; vectors 0 to 3 are the null
        call    nt_arm                  ; guard's to watch
        cld
        xor     esi,esi
        mov     edi,BIOS_IVT
        mov     ecx,256
        rep     movsd
        mov     al,1
        call    nt_arm
        mov     edi,BIOS_IVT
        call    vd_freeze               ; INT 4Bh
        xor     ebx,ebx
        mov     eax,(IO_SEG SHL 16) + RMT_IRQS
rfz_one:
        lea     edx,[ebx + 08h]         ; the line's vector in the BIOS's
        cmp     ebx,8                   ; layout: 08h to 0Fh, then 70h to
        jb      rfz_vec                 ; 77h - which stay as they are
        add     edx,70h - 10h
rfz_vec:
        mov     ecx,dword ptr ds:[BIOS_IVT + edx*4]
        mov     dword ptr ds:[IO_BASE + RMT_IRQORIG + ebx*4],ecx
        mov     dword ptr ds:[BIOS_IVT + IRQ_BASE*4 + ebx*4],eax
        add     eax,RQ_LEN
        inc     ebx
        cmp     ebx,16
        jb      rfz_one
        mov     word ptr ds:[GV_BIDTR],03FFh
        mov     dword ptr ds:[GV_BIDTR + 2],BIOS_IVT
        mov     byte ptr ds:[GV_PICKEEP],1
        popfd
        popad
        ret
.ERRNZ  IRQ_BASE2 - (IRQ_BASE + 8), <rm_freeze puts the sixteen lines in one run from IRQ_BASE>

;=====================================================================
; rm_cf - ZF clear if the last rm_int came back with carry set.
;=====================================================================
rm_cf:
        test    dword ptr ds:[RMR_FLAGS],1
        ret

;=====================================================================
; rm_park - hand the machine to real-mode code, and get it back.
;
;   ESI -> GVP_LEN bytes: CS, IP, DS, ES, SS, SP, active, AX
;
; This is rm_int with its ending taken off.  It goes down to real mode
; the same way and through the same code, but instead of running one
; interrupt and coming straight back it FAR JUMPS to the address the
; caller named and does not return until that code jumps to the unpark
; entry - which may be a very long time, because what it is for is
; letting Windows own the machine.
;
; From up here it is one slow subroutine call: RMSAVE_ESP and RMRET
; are set exactly as rm_int sets them, and the return lands at rmp_back
; with the stack where it was left.
;
; MUST NOT BE CALLED FROM AN INTERRUPT HANDLER, for the same reason
; rm_int must not, and rather more so.
;=====================================================================
rm_park:
        pushad
        pushfd
        cli

        ; The null guard comes off for the excursion, as it does for
        ; any trip into real mode: what it guards is our own software,
        ; and none of ours runs between here and rmp_back.
        push    eax
        xor     al,al
        call    nt_arm
        pop     eax

        push    esi
        push    edi
        push    ecx
        mov     edi,GV_PARK
        mov     ecx,GVP_LEN
        cld
        rep     movsb
        pop     ecx
        pop     edi
        pop     esi

        mov     dword ptr ds:[RMSAVE_ESP],esp
        mov     dword ptr ds:[RMRET],OFFSET rmp_back
        mov     word ptr ds:[RMRET+4],SEL_CODE32

        ; What is about to own the machine expects the coprocessor the
        ; way the BIOS left it - its errors on interrupt line 13, where
        ; every DOS extender and Windows itself look for them.
        call    fpu_park

        db      0EAh                    ; JMP FAR SEL_CODE16:RMT_PARK
        dd      RMT_PARK
        dw      SEL_CODE16

rmp_back:
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     es,ax
        mov     fs,ax
        mov     gs,ax
        mov     ss,ax
        mov     esp,dword ptr ds:[RMSAVE_ESP]
        cld
        call    fpu_unpark
        push    eax
        mov     al,1
        call    nt_arm
        pop     eax
        popfd
        popad
        ret

;=====================================================================
; rm_unparkaddr - where RMDOS has to jump to give the machine back.
;                 -> EAX = segment in the high half, offset in the low.
;
; Handed over rather than compiled in, because RMDOS.SYS is built
; separately and has no way to know where PMIO ended up.
;=====================================================================
rm_unparkaddr:
        mov     eax,(IO_SEG SHL 16) OR RMT_UNPARK
        ret
