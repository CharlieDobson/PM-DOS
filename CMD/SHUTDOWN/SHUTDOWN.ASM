;=====================================================================
; SHUTDOWN.ASM - SHUTDOWN.EXE, switch the computer off
;
;   SHUTDOWN        write everything still owed to the disks, then
;                   switch the power off through ACPI
;
; THE KERNEL DOES ALL OF IT, in one call: a program at ring 3 can
; reach neither the ACPI tables nor the port that switches the power
; off, and only the kernel knows what it still owes the disks.  This
; parses a command line and says what went wrong when the call comes
; back - which it only ever does when the machine is still running.
;
; NOTHING IS WRITTEN WHEN THERE IS NO ACPI.  The kernel looks for it
; before touching a disk, so "ACPI not found." leaves the machine
; exactly as it was.
;
; /? ANYWHERE IS HELP, and any other argument is refused.  A command
; that switches the machine off must not do it because a switch it
; does not know was mistyped - FORMAT A: /? is the precedent.
;
; What it asks:
;   21h/F1h AL=1Eh   PWF_OFF: find ACPI, write every file and cache,
;                    switch the power off.  CF and AX = why not,
;                    otherwise no return at all.  What ACPI gave is
;                    HWPROBE's to show (PWF_QUERY)
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_SHD    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_SHD, DS:_SHD, ES:_SHD, SS:_SHD
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET sd_entry>,<0>,4096

sd_entry:
;--- the command tail: /? wins wherever it is ------------------------
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
        xor     ebx,ebx                         ; BL = something else was typed
sa_lp:
        or      ecx,ecx
        jz      sa_done
        mov     al,[esi]
        cmp     al,0Dh
        je      sa_done
        cmp     al,' '
        je      sa_next
        cmp     al,9
        je      sa_next
        cmp     al,'/'
        jne     sa_other
        cmp     ecx,2
        jb      sa_other
        cmp     byte ptr [esi+1],'?'
        je      sd_help
sa_other:
        mov     bl,1
sa_next:
        inc     esi
        dec     ecx
        jmp     short sa_lp
sa_done:
        or      bl,bl
        jz      sd_off
        mov     edx,OFFSET msg_badparm
        jmp     short sd_fail

;--- switch off ------------------------------------------------------
sd_off:
        mov     eax,0F11Eh + (PWF_OFF SHL 16)   ; all of EAX: bits 16-23
        int     21h                             ; say which function

;--- still running, and AX says why ------------------------------------
        mov     edx,OFFSET msg_noacpi
        cmp     ax,ERR_NOACPI
        je      sd_fail
        mov     edx,OFFSET msg_nooff
        cmp     ax,ERR_NOSOFTOFF
        je      sd_fail
        mov     edx,OFFSET msg_wrfault
        cmp     ax,ERR_WRFAULT
        je      sd_fail
        mov     edx,OFFSET msg_stayed
        cmp     ax,ERR_STAYEDON
        je      sd_fail
        mov     edx,OFFSET msg_kernel           ; a kernel without AL=1Eh
sd_fail:
        mov     ah,09h
        int     21h
        mov     ax,4C01h
        int     21h

sd_help:
        mov     edx,OFFSET msg_usage
        mov     ah,09h
        int     21h
        mov     ax,4C00h
        int     21h

;=====================================================================
; Data
;=====================================================================
msg_badparm db  'Invalid parameter',13,10,'$'
msg_noacpi  db  'ACPI not found.',13,10,'$'
msg_nooff   db  'ACPI soft-off not supported.',13,10,'$'
msg_wrfault db  'A disk could not be written. The computer was not switched off.',13,10,'$'
msg_stayed  db  'The computer did not switch off. Everything has been written',13,10
            db  'to the disks, so the power can be switched off by hand.',13,10,'$'
msg_kernel  db  'This version of PM-DOS cannot switch the computer off.',13,10,'$'
msg_usage   db  'Writes everything still owed to the disks, then switches the',13,10
            db  'computer off.',13,10,13,10
            db  'SHUTDOWN',13,10,13,10
            db  'The power is switched off through ACPI. On a computer without',13,10
            db  'ACPI, nothing is written and the computer keeps running.',13,10,'$'

_SHD    ENDS
        END
