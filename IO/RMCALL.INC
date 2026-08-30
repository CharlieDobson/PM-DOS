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

        mov     byte ptr ds:[RMR_INTNO],al
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
        popfd
        popad
        ret

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
