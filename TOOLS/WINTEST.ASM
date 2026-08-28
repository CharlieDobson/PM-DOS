;=====================================================================
; WINTEST.ASM - WINTEST.COM, the swappable data area and the Windows
;               handshake
;
; A 16-bit DOS program, and it has to be: everything here is served by
; the VDM bridge, and half of it - the INT 2Fh 16xx family - does not
; exist for a 32-bit caller at all.
;
; TWO GROUPS OF CHECKS.
;
;   5D06h and what it points at.  The swappable data area is the block
;   a multitasker saves and restores around a task switch, and its
;   first two bytes are the critical-error flag and InDOS - the same
;   two bytes 34h has been handing out the address of since v0.25.
;   That overlap is the interesting part and it is what the middle
;   checks are for: 34h's answer must be the SDA's second byte and not
;   merely a byte that looks like it.  Then the three fields the kernel
;   keeps its master copy of elsewhere - drive, PSP, DTA - are read out
;   of the structure and compared against the calls that own them, so a
;   win_sdasync that stopped running would be caught rather than
;   producing a structure that is merely out of date.
;
;   The INT 2Fh Windows family.  Four of those calls are questions with
;   the answer "no", and the fifth is WIN386.EXE asking permission to
;   take the machine.  PM-DOS refuses that one, and REFUSING IS THE
;   BEHAVIOUR UNDER TEST: before there was code for it the call reached
;   an IRET and WIN386 read its verdict out of a register nobody had
;   written.  Running this test prints the kernel's refusal message,
;   which is expected and is not a failure.
;
; Every check prints "ok" or "FAILED", so the whole run is one screen
; that either has no capitals in it or does.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_WT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_WT, DS:_WT, ES:_WT, SS:_WT
        ORG     0

SDA_LEN     EQU 1Ah                     ; what the kernel publishes, and the
                                        ; one number here that is a contract
                                        ; between this file and PMDOS.INC

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; 5D06h - where is the swappable data area?
;
; DS IS WHAT THIS CALL MOVES, so it is pushed around the call and the
; answer is stashed through CS.  Reading it back through DS afterwards
; would read the structure the call just pointed at rather than this
; program's variables - which is the trap LOLTEST hit on 1Bh and 63h.
;=====================================================================
        mov     dx,OFFSET t_sda
        call    puts
        push    ds
        mov     ax,5D06h
        xor     si,si
        int     21h
        mov     bp,ds                   ; out through a register the call
        pop     ds                      ; does not touch
        jc      sda_no
        mov     [sda_seg],bp
        mov     [sda_off],si
        mov     [sda_cx],cx
        mov     [sda_dx],dx

        mov     ax,bp
        call    put4
        mov     dl,':'
        call    putc
        mov     ax,si
        call    put4

        ; Below A000h is the part of the machine a real-mode program
        ; can address.  A flat kernel pointer truncated to sixteen bits
        ; could land here by chance, so this is the cheap check and the
        ; ones below are the proof.
        cmp     bp,0A000h
        jae     sda_bad
        call    ok
        jmp     short sda_cxdx
sda_no:
        mov     dx,OFFSET msg_refused
        call    puts
        inc     byte ptr [fails]
        jmp     sda_skip
sda_bad:
        call    bad

;---------------------------------------------------------------------
; CX is the length and DX is how much of it must always be swapped.
; PM-DOS has no second half for DX to stop short of, so the two are
; the same number - which is what "all of it, always" looks like from
; out here.
;---------------------------------------------------------------------
sda_cxdx:
        mov     dx,OFFSET t_len
        call    puts
        mov     ax,[sda_cx]
        call    put4
        mov     dl,'/'
        call    putc
        mov     ax,[sda_dx]
        call    put4
        mov     ax,[sda_cx]
        cmp     ax,SDA_LEN
        jne     len_bad
        cmp     ax,[sda_dx]
        jne     len_bad
        call    ok
        jmp     short sda_indos
len_bad:
        call    bad

;=====================================================================
; 34h must answer the SECOND BYTE OF THIS STRUCTURE.
;
; Not a byte holding the same value - the same byte.  Both are compared
; as twenty-bit addresses, because seg:off is not unique and two
; different pairs naming one byte would pass a register comparison and
; mean nothing.
;
; This is the check the whole layout exists for.  MS-DOS puts ErrorMode
; and INDOS at the head of its own swappable area in that order, and
; the reason a great deal of TSR code finds the critical-error flag by
; subtracting one from what 34h returned is that in real DOS it IS the
; byte before.  PM-DOS kept these two at 4F6h in the BIOS data area
; until there was a structure for them to be the head of.
;=====================================================================
sda_indos:
        mov     dx,OFFSET t_indos
        call    puts
        mov     ah,34h
        int     21h                     ; ES:BX
        mov     eax,0
        mov     ax,es
        shl     eax,4
        movzx   edx,bx
        add     eax,edx
        mov     [in_lin],eax

        call    sdalin                  ; EAX = linear address of the SDA
        inc     eax
        cmp     eax,[in_lin]
        jne     indos_bad
        call    ok
        jmp     short sda_flags
indos_bad:
        call    bad

;---------------------------------------------------------------------
; And the two flags read what they must read from out here: nobody is
; inside a DOS call at this instant, and nobody is inside a critical
; error either.
;---------------------------------------------------------------------
sda_flags:
        mov     dx,OFFSET t_flags
        call    puts
        call    sdaes                   ; ES:BX -> the SDA
        mov     al,es:[bx]              ; +00 critical error in progress
        or      al,al
        jnz     flags_bad
        mov     al,es:[bx+1]            ; +01 InDOS depth
        or      al,al
        jnz     flags_bad
        call    ok
        jmp     short sda_wperr
flags_bad:
        call    bad

;---------------------------------------------------------------------
; +02 is the write-protect error drive, and MS-DOS initialises it to
; FFh rather than to zero because drive zero is a real drive.  PM-DOS
; copies that, so this byte is how a test tells "win_sdainit ran" from
; "the paragraph happened to be zero".
;---------------------------------------------------------------------
sda_wperr:
        mov     dx,OFFSET t_wperr
        call    puts
        call    sdaes
        mov     al,es:[bx+2]
        call    put2
        cmp     al,0FFh
        jne     wperr_bad
        call    ok
        jmp     short sda_drive
wperr_bad:
        call    bad

;=====================================================================
; The three fields the kernel keeps its master copy of somewhere else,
; against the calls that own them.  These are what win_sdasync is for,
; and a sync that stopped running would show up here and nowhere else -
; the structure would still be there, still be the right length, and
; still be quietly describing the machine as it was some calls ago.
;=====================================================================
sda_drive:
        mov     dx,OFFSET t_drive
        call    puts
        mov     ah,19h                  ; current drive, 0 = A
        int     21h
        mov     ah,al
        call    sdaes
        mov     al,es:[bx+16h]
        push    ax
        call    put2
        pop     ax
        cmp     al,ah
        jne     drive_bad
        call    ok
        jmp     short sda_psp
drive_bad:
        call    bad

sda_psp:
        mov     dx,OFFSET t_psp
        call    puts
        mov     ah,62h                  ; our PSP
        int     21h
        mov     bp,bx
        call    sdaes
        mov     ax,es:[bx+10h]
        push    ax
        call    put4
        pop     ax
        cmp     ax,bp
        jne     psp_bad
        call    ok
        jmp     short sda_dta
psp_bad:
        call    bad

;---------------------------------------------------------------------
; The DTA, compared as a linear address for the same reason 34h's was:
; DOS stores this one offset-first, and 2Fh answers it in ES:BX, so
; two different pairs for one byte is exactly what a lazy comparison
; would let through.
;---------------------------------------------------------------------
sda_dta:
        mov     dx,OFFSET t_dta
        call    puts
        mov     ah,2Fh                  ; ES:BX = the DTA
        int     21h
        mov     eax,0
        mov     ax,es
        shl     eax,4
        movzx   edx,bx
        add     eax,edx
        mov     [in_lin],eax

        call    sdaes
        movzx   eax,word ptr es:[bx+0Eh]        ; segment
        shl     eax,4
        movzx   edx,word ptr es:[bx+0Ch]        ; offset
        add     eax,edx
        push    eax
        shr     eax,16
        call    put4
        pop     eax
        push    eax
        call    put4
        pop     eax
        cmp     eax,[in_lin]
        jne     dta_bad
        call    ok
        jmp     short win_1600
dta_bad:
        call    bad

sda_skip:

;=====================================================================
; The INT 2Fh Windows family.
;
; The four questions first.  Every one of them is specified so that
; "no" is the registers coming back unchanged, which means all four
; were answering correctly before there was any code for them - the
; IRET stub the VDM puts on 2Fh for a guest changes nothing.  They are
; checked anyway, because "right by accident" and "right on purpose"
; look identical from here and only one of them survives the next
; change to the stub.
;=====================================================================
win_1600:
        mov     dx,OFFSET t_1600
        call    puts
        mov     ax,1600h                ; is 386 enhanced mode running?
        int     2Fh
        push    ax
        call    put2                    ; AL is the answer
        pop     ax
        or      al,al                   ; 00h = no
        jnz     w1600_bad
        call    ok
        jmp     short win_160A
w1600_bad:
        call    bad

win_160A:
        mov     dx,OFFSET t_160A
        call    puts
        mov     ax,160Ah                ; get version / instance data
        int     2Fh
        or      ax,ax                   ; non-zero = not running
        jz      w160A_bad
        call    ok
        jmp     short win_4680
w160A_bad:
        call    bad

win_4680:
        mov     dx,OFFSET t_4680
        call    puts
        mov     ax,4680h                ; is 3.0 real or standard mode up?
        int     2Fh
        or      ax,ax                   ; non-zero = not running
        jz      w4680_bad
        call    ok
        jmp     short win_1680
w4680_bad:
        call    bad

win_1680:
        mov     dx,OFFSET t_1680
        call    puts
        mov     ax,1680h                ; release the current time slice
        int     2Fh
        push    ax
        call    put2
        pop     ax
        cmp     al,80h                  ; 80h = nobody is listening
        jne     w1680_bad
        call    ok
        jmp     short win_1605
w1680_bad:
        call    bad

;=====================================================================
; 1605h - the one with teeth.
;
; This is WIN386.EXE announcing that it is about to take the machine.
; A resident on the chain may veto the startup by returning CX
; non-zero, and PM-DOS does, because WIN386 is itself a V86 monitor and
; this kernel is already the one running the machine.
;
; THE KERNEL PRINTS ITS REASON WHEN THIS RUNS.  That is the refusal
; working, not a fault in the test, and it happens once per boot.
;
; ES:BX is zeroed on the way in the way WIN386's first broadcast zeroes
; it, so that anything reading the startup structure reads a null
; pointer rather than this program's stack.
;=====================================================================
win_1605:
        mov     dx,OFFSET t_1605
        call    puts
        push    ds
        xor     bx,bx
        mov     es,bx
        mov     ds,bx
        xor     si,si
        xor     di,di
        xor     cx,cx                   ; WIN386 asks with CX clear and reads
                                        ; the answer back out of it
        mov     ax,1605h
        int     2Fh
        mov     bp,cx
        pop     ds
        mov     ax,bp
        call    put4
        or      bp,bp                   ; non-zero = do not start
        jz      w1605_bad
        call    ok
        jmp     short done
w1605_bad:
        call    bad

;---------------------------------------------------------------------
done:
        mov     dx,OFFSET msg_crlf
        call    puts
        mov     al,[fails]
        or      al,al
        jz      all_ok
        mov     dx,OFFSET msg_fails
        call    puts
        mov     al,[fails]
        call    put2
        mov     dx,OFFSET msg_crlf
        call    puts
        jmp     short bye
all_ok:
        mov     dx,OFFSET msg_allok
        call    puts
bye:
        mov     dx,OFFSET msg_bye
        call    puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; helpers
;=====================================================================

;--- ES:BX -> the swappable data area.  Every field check goes through
;--- this rather than keeping a live pointer, because the calls being
;--- compared against move ES themselves.
sdaes:
        mov     bx,[sda_seg]
        mov     es,bx
        mov     bx,[sda_off]
        ret

;--- EAX = the SDA as a twenty-bit address
sdalin:
        push    edx
        movzx   eax,word ptr [sda_seg]
        shl     eax,4
        movzx   edx,word ptr [sda_off]
        add     eax,edx
        pop     edx
        ret

ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     dx
        ret

puts:
        push    ax
        push    ds
        push    cs
        pop     ds
        mov     ah,09h
        int     21h
        pop     ds
        pop     ax
        ret
putc:
        push    ax
        push    dx
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret
put4:                                   ; AX in hex
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:                                   ; AL in hex
        push    ax
        shr     al,4
        call    put1
        pop     ax
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        call    putc
        ret

;=====================================================================
; data
;=====================================================================
msg_hi      db  13,10,'WINTEST - the swappable data area, and the Windows handshake',13,10,'$'
msg_bye     db  'WINTEST done.',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_fails   db  'checks failed: $'
msg_allok   db  'all checks passed.',13,10,'$'
msg_refused db  'REFUSED - there is no swappable data area',13,10,'$'

t_sda       db  '  5D06h swappable data area at $'
t_len       db  '  its length / always-swap $'
t_indos     db  '  34h answers its second byte$'
t_flags     db  '  and both flags read zero out here$'
t_wperr     db  '  +02 write-protect drive is $'
t_drive     db  '  +16h current drive matches 19h: $'
t_psp       db  '  +10h current PSP matches 62h: $'
t_dta       db  '  +0Ch DTA matches 2Fh: $'
t_1600      db  '  2Fh/1600h enhanced mode running: $'
t_160A      db  '  2Fh/160Ah version says not running$'
t_4680      db  '  2Fh/4680h 3.0 standard mode not running$'
t_1680      db  '  2Fh/1680h release time slice: $'
t_1605      db  '  2Fh/1605h Windows/386 startup is vetoed: $'

fails       db  0
sda_seg     dw  0
sda_off     dw  0
sda_cx      dw  0
sda_dx      dw  0
in_lin      dd  0

_WT     ENDS
        END
