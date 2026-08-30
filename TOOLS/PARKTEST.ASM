;=====================================================================
; PARKTEST.ASM - PARKTEST.EXE, the launcher for the handover
;
; A native 32-bit PM-DOS program that asks the kernel to LEAVE.  It
; calls 21h/F1h AL=0Bh, which loads RMDOS.SYS into the DOS box's
; arena, drops paging, leaves protected mode for good, and starts
; RMTEST.COM on the real-mode DOS that is left behind.  When RMTEST
; ends, the machine comes back and this program is still standing
; where it was.
;
; THE INTERESTING PART IS THAT THIS PROGRAM SURVIVES AT ALL.  It is
; 32-bit code at ring 3 in a paged flat address space, and between the
; call below and the instruction after it the processor has been in
; real mode, an entirely different kernel has owned the interrupt
; table, and a program that knows nothing about any of this has run to
; completion on it.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_PT     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_PT, DS:_PT, ES:_PT, SS:_PT
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET pt_entry>,<0>,4096

pt_entry:
        mov     edx,OFFSET msg_hi
        call    puts

        ; The program to run over there, from the command tail, so that
        ; one launcher can put any 16-bit program on the other side of
        ; the switch.  Nothing after this point cares which.
        call    pt_arg
        mov     edx,OFFSET msg_prog
        call    puts
        mov     edx,[progptr]
        call    putz
        mov     edx,OFFSET msg_crlf
        call    puts

        ; A marker before and after, so a screen that stops here says
        ; which side of the switch it stopped on.
        mov     edx,OFFSET msg_going
        call    puts

        mov     edx,[progptr]
        mov     ax,0F10Bh
        int     21h
        jc      pt_failed

        mov     [exitcode],eax
        mov     edx,OFFSET msg_back
        call    puts
        mov     eax,[exitcode]
        call    put8
        mov     edx,OFFSET msg_crlf
        call    puts

        ; And prove the kernel is alive rather than merely returning:
        ; a DOS call that touches a disk, made after the excursion,
        ; through the caches it invalidated.
        mov     edx,OFFSET msg_alive
        call    puts
        mov     edx,OFFSET selfname
        mov     ax,3D00h
        int     21h
        jc      pt_dead
        mov     ebx,eax
        mov     ax,3E00h
        int     21h
        mov     edx,OFFSET msg_ok
        call    puts
        jmp     short pt_done
pt_dead:
        mov     edx,OFFSET msg_bad
        call    puts
        jmp     short pt_done

pt_failed:
        mov     [exitcode],eax
        mov     edx,OFFSET msg_refused
        call    puts
        mov     eax,[exitcode]
        call    put8
        mov     edx,OFFSET msg_crlf
        call    puts

pt_done:
        mov     ax,4C00h
        int     21h

;=====================================================================
; pt_arg - the command tail, if there is one, into [progptr].
;---------------------------------------------------------------------
pt_arg:
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        or      ecx,ecx
        jz      pa_done
        inc     esi
        mov     edi,OFFSET argbuf
pa_skip:
        cmp     byte ptr [esi],' '
        jne     pa_copy
        inc     esi
        dec     ecx
        jnz     pa_skip
        jmp     short pa_done
pa_copy:
        mov     al,[esi]
        cmp     al,' '
        jbe     pa_end
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     pa_copy
pa_end:
        mov     byte ptr [edi],0
        cmp     edi,OFFSET argbuf
        je      pa_done
        mov     dword ptr [progptr],OFFSET argbuf
pa_done:
        ret

;--- putz - EDX -> an ASCIIZ string ----------------------------------
putz:
        push    eax
        push    edx
        mov     esi,edx
pz_loop:
        mov     al,[esi]
        or      al,al
        jz      pz_done
        push    esi
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     esi
        inc     esi
        jmp     short pz_loop
pz_done:
        pop     edx
        pop     eax
        ret

;=====================================================================
puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

put8:
        push    eax
        push    edx
        mov     ecx,8                   ; all eight nibbles: this is a
        mov     edx,eax
p4_loop:
        rol     edx,4
        mov     al,dl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p4_ch
        add     al,7
p4_ch:
        push    ecx
        push    edx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     edx
        pop     ecx
        loop    p4_loop
        pop     edx
        pop     eax
        ret

;=====================================================================
msg_hi      db  13,10,'PARKTEST - asking the kernel to leave',13,10,13,10,'$'
msg_going   db  'Parking.  Everything below this line is RMDOS.',13,10,'$'
msg_back    db  13,10,'Back in protected mode.  It exited with $'
msg_crlf    db  13,10,'$'
msg_alive   db  'and the kernel still opens a file: $'
msg_ok      db  'yes',13,10,'$'
msg_bad     db  'NO',13,10,'$'
msg_refused db  'The kernel refused to park, error $'

msg_prog    db  'The program on the other side: $'

progptr     dd  OFFSET progname
progname    db  '\RMTEST.COM',0
argbuf      db  80 dup(0)
selfname    db  '\PMDOS.SYS',0
exitcode    dd  0

_PT     ENDS
        END
