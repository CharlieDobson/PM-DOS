;=====================================================================
; KEYBD.INC - PS/2 keyboard driver (scan code set 1)
;
; The IRQ 1 handler translates make codes into an ASCII/scan-code pair
; and drops it in a ring buffer.  We do not go through the BIOS, so
; this keeps working with the machine in protected mode.
;
; AND THE RING IS THE BIOS'S OWN, at 40:1E, which it was not until
; v0.34.  This driver kept a private 32-entry buffer and left the BDA
; alone, and the consequence is the same one v0.32 found for the timer
; wearing different clothes: OWNING AN INTERRUPT MEANS OWNING WHAT THE
; BIOS'S HANDLER DID.  IRQ 1 belongs to this code, so the ROM's INT 09h
; never runs, so nothing ever wrote 40:1A, 40:1C or the sixteen words
; between them.
;
; A 16-bit program asking for a key does it through INT 16h, which is
; reflected to the ROM, which looks at exactly those bytes - finds head
; equal to tail, concludes no key has been pressed, and waits.  It
; waits for ever, because the thing that would have changed its mind is
; the handler that no longer exists.  Every full-screen DOS program
; hung at its first keystroke; a 486 with MSD on it is where that was
; found, and "16-bit programs stopped working" is what it looked like
; from outside.  Programs that read 40:17 for the shift state, or peek
; at 40:1A themselves rather than calling anybody, were being lied to
; by the same silence.
;
; So there is ONE buffer now and it is the machine's.  The kernel's
; console reads it, the BIOS reads it, and a program reading it
; directly reads it - which also means the shell and a DOS box cannot
; disagree about what was typed, the way two buffers eventually would.
; The private one is deleted rather than mirrored, for the reason
; tick_count was deleted: a copy is a thing that can drift.
;
; Included by IOPM.ASM.
;=====================================================================

KB_DATA         EQU     60h
KB_CMD          EQU     64h

KBF_SHIFT       EQU     01h
KBF_CTRL        EQU     02h
KBF_ALT         EQU     04h
KBF_E0          EQU     08h
KBF_CAPS        EQU     10h

;=====================================================================
; kb_init - drain anything the BIOS left in the controller.
;=====================================================================
kb_init:
        ; The ring, as the ROM would have set it up.  Both bounds are
        ; written rather than trusted: a BIOS is entitled to move the
        ; buffer, and kb_put honours wherever it says the buffer is, so
        ; this states the arrangement instead of assuming it.
        mov     word ptr ds:[BDA_KBSTART],KB_RINGLO
        mov     word ptr ds:[BDA_KBEND],KB_RINGHI
        mov     word ptr ds:[BDA_KBHEAD],KB_RINGLO
        mov     word ptr ds:[BDA_KBTAIL],KB_RINGLO
        mov     byte ptr ds:[BDA_KBFLAG],0
        mov     byte ptr ds:[BDA_KBFLAG2],0
        mov     byte ptr [kb_flags],0
ki_drain:
        in      al,KB_CMD
        test    al,1                    ; output buffer full?
        jz      ki_done
        in      al,KB_DATA
        jmp     short ki_drain
ki_done:
        ret

;=====================================================================
; irq_keyboard - IRQ 1 service routine.
;=====================================================================
irq_keyboard:
        in      al,KB_DATA

        cmp     al,0E0h                 ; extended key prefix
        jne     kb_notE0
        or      byte ptr [kb_flags],KBF_E0
        ret
kb_notE0:
        mov     ah,al
        and     ah,80h                  ; AH != 0 means this is a break
        and     al,7Fh

;--- modifier keys ----------------------------------------------------
        cmp     al,2Ah                  ; left shift
        je      kb_shift
        cmp     al,36h                  ; right shift
        je      kb_shift
        cmp     al,1Dh                  ; ctrl
        je      kb_ctrl
        cmp     al,38h                  ; alt
        je      kb_alt
        cmp     al,3Ah                  ; caps lock
        je      kb_caps

        test    ah,ah
        jnz     kb_clr_e0               ; break code for an ordinary key

;--- Ctrl-Alt-Del -----------------------------------------------------
        cmp     al,53h
        jne     kb_normal
        mov     ah,byte ptr [kb_flags]
        and     ah,KBF_CTRL OR KBF_ALT
        cmp     ah,KBF_CTRL OR KBF_ALT
        je      kb_reboot

;--- ordinary key -----------------------------------------------------
kb_normal:
        cmp     al,59h                  ; beyond our translation tables?
        jae     kb_clr_e0

        movzx   ebx,al
        mov     ah,al                   ; keep the scan code in AH
        test    byte ptr [kb_flags],KBF_SHIFT
        jnz     kb_shifted
        mov     al,[kb_lower + ebx]
        test    byte ptr [kb_flags],KBF_CAPS
        jz      kb_haveasc
        cmp     al,'a'                  ; caps lock affects letters only
        jb      kb_haveasc
        cmp     al,'z'
        ja      kb_haveasc
        sub     al,32
        jmp     short kb_haveasc
kb_shifted:
        mov     al,[kb_upper + ebx]
        test    byte ptr [kb_flags],KBF_CAPS
        jz      kb_haveasc
        cmp     al,'A'
        jb      kb_haveasc
        cmp     al,'Z'
        ja      kb_haveasc
        add     al,32

kb_haveasc:
        test    byte ptr [kb_flags],KBF_CTRL
        jz      kb_store
        or      al,al
        jz      kb_store
        and     al,1Fh                  ; Ctrl-A becomes 01h, and so on

kb_store:
        call    kb_put
kb_clr_e0:
        and     byte ptr [kb_flags],NOT KBF_E0
        ret

;--- modifier handling ------------------------------------------------
; Every one of these ends at kb_mod, which publishes the state to
; 40:17: a modifier that only this driver knows about is a modifier a
; 16-bit program cannot see, and "is Shift down?" is a question half
; the DOS software ever written asks by reading that byte.
kb_shift:
        test    ah,ah
        jnz     kb_shift_up
        or      byte ptr [kb_flags],KBF_SHIFT
        jmp     short kb_mod
kb_shift_up:
        and     byte ptr [kb_flags],NOT KBF_SHIFT
        jmp     short kb_mod

kb_ctrl:
        test    ah,ah
        jnz     kb_ctrl_up
        or      byte ptr [kb_flags],KBF_CTRL
        jmp     short kb_mod
kb_ctrl_up:
        and     byte ptr [kb_flags],NOT KBF_CTRL
        jmp     short kb_mod

kb_alt:
        test    ah,ah
        jnz     kb_alt_up
        or      byte ptr [kb_flags],KBF_ALT
        jmp     short kb_mod
kb_alt_up:
        and     byte ptr [kb_flags],NOT KBF_ALT
        jmp     short kb_mod

kb_caps:
        test    ah,ah
        jnz     kb_clr_e0               ; toggle on the make code only
        xor     byte ptr [kb_flags],KBF_CAPS

kb_mod:
        call    kb_bdaflags
        jmp     short kb_clr_e0

;=====================================================================
; kb_reboot - Ctrl-Alt-Del.  Ask the 8042 to pulse the reset line, and
;             if it will not, force a triple fault.
;=====================================================================
kb_reboot:
        cli
        mov     al,0FEh
        out     KB_CMD,al
        lidt    fword ptr [null_idtr]   ; no valid gates anywhere
        int     3                       ; -> triple fault -> reset
kbr_hang:
        hlt
        jmp     kbr_hang

;=====================================================================
; kb_put - append AX (AL = ASCII, AH = scan code) to the BIOS ring at
;          40:1E.  Silently drops the key if it is full, which is what
;          the ROM does apart from the beep.
;
; The advance is "add two, and if that reaches the end go back to the
; start" rather than a mask, because the buffer's bounds are read out
; of 40:80 and 40:82 and are not required to be a power of two.
;=====================================================================
kb_put:
        push    ebx
        push    ecx
        movzx   ebx,word ptr ds:[BDA_KBTAIL]
        lea     ecx,[ebx+2]
        cmp     cx,word ptr ds:[BDA_KBEND]
        jb      kp_nowrap
        movzx   ecx,word ptr ds:[BDA_KBSTART]
kp_nowrap:
        cmp     cx,word ptr ds:[BDA_KBHEAD]
        je      kp_full                 ; one slot always stays empty, or
                                        ; full and empty would look alike
        mov     word ptr ds:[ebx+BDA_BASE],ax
        mov     word ptr ds:[BDA_KBTAIL],cx
kp_full:
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; kb_status - ZF set if the buffer is empty (IOS_CONSTAT).
;=====================================================================
kb_status:
        push    eax
        mov     ax,word ptr ds:[BDA_KBHEAD]
        cmp     ax,word ptr ds:[BDA_KBTAIL]
        pop     eax
        ret

;=====================================================================
; kb_getchar - block until a key arrives, then return it in AX
;              (AL = ASCII, AH = scan code).  IOS_CONIN.
;=====================================================================
kb_getchar:
        push    ebx
        push    ecx
kg_wait:
        cli
        mov     bx,word ptr ds:[BDA_KBHEAD]
        cmp     bx,word ptr ds:[BDA_KBTAIL]
        jne     kg_have
        sti
        hlt                             ; idle until the next interrupt
        jmp     short kg_wait
kg_have:
        movzx   ebx,bx
        movzx   eax,word ptr ds:[ebx+BDA_BASE]
        lea     ecx,[ebx+2]
        cmp     cx,word ptr ds:[BDA_KBEND]
        jb      kg_nowrap
        movzx   ecx,word ptr ds:[BDA_KBSTART]
kg_nowrap:
        mov     word ptr ds:[BDA_KBHEAD],cx
        sti
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; kb_bdaflags - mirror the modifier state into 40:17, where a program
;               that wants to know whether Shift is down looks for it.
;
; The internal byte stays the master because it carries KBF_E0, which
; is a decoding state and not a fact about the keyboard.  Only the bits
; this driver genuinely tracks are written: Num Lock and Scroll Lock
; are not tracked, and setting their bits would be a claim rather than
; a report.  Left and right shift are not told apart here either, so
; both bits move together - a program testing either one gets the right
; answer, and one testing them separately learns that both are down,
; which is the least wrong thing available.
;=====================================================================
kb_bdaflags:
        push    eax
        push    ebx
        mov     bl,[kb_flags]
        xor     al,al
        test    bl,KBF_SHIFT
        jz      kbf_noshift
        or      al,BDAK_RSHIFT OR BDAK_LSHIFT
kbf_noshift:
        test    bl,KBF_CTRL
        jz      kbf_noctrl
        or      al,BDAK_CTRL
kbf_noctrl:
        test    bl,KBF_ALT
        jz      kbf_noalt
        or      al,BDAK_ALT
kbf_noalt:
        test    bl,KBF_CAPS
        jz      kbf_nocaps
        or      al,BDAK_CAPS
kbf_nocaps:
        mov     byte ptr ds:[BDA_KBFLAG],al
        pop     ebx
        pop     eax
        ret

;=====================================================================
; Scan code translation, indexed by make code 00h..58h.
; Zero means "no ASCII equivalent" - the caller still gets the scan
; code in AH, which is how function and cursor keys are reported.
;=====================================================================
kb_lower:
        db      0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8
        db      9,'q','w','e','r','t','y','u','i','o','p','[',']',13
        db      0                                       ; 1D ctrl
        db      'a','s','d','f','g','h','j','k','l',';',27h,60h
        db      0                                       ; 2A left shift
        db      5Ch                                     ; 2B backslash
        db      'z','x','c','v','b','n','m',',','.','/'
        db      0                                       ; 36 right shift
        db      '*'                                     ; 37 keypad star
        db      0                                       ; 38 alt
        db      ' '                                     ; 39 space
        db      0                                       ; 3A caps lock
        db      0,0,0,0,0,0,0,0,0,0                     ; 3B-44 F1-F10
        db      0,0                                     ; 45 num, 46 scroll
        db      '7','8','9','-','4','5','6','+','1','2','3','0','.'
        db      0,0,0                                   ; 54-56
        db      0,0                                     ; 57-58 F11, F12

kb_upper:
        db      0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8
        db      9,'Q','W','E','R','T','Y','U','I','O','P','{','}',13
        db      0
        db      'A','S','D','F','G','H','J','K','L',':',22h,7Eh
        db      0
        db      7Ch                                     ; 2B pipe
        db      'Z','X','C','V','B','N','M','<','>','?'
        db      0
        db      '*'
        db      0
        db      ' '
        db      0
        db      0,0,0,0,0,0,0,0,0,0
        db      0,0
        db      '7','8','9','-','4','5','6','+','1','2','3','0','.'
        db      0,0,0
        db      0,0

;=====================================================================
kb_flags    db  0                       ; the decoder's state.  The buffer
                                        ; it fills is the machine's, at
                                        ; 40:1E - see the head comment

null_idtr:  dw  0
            dd  0
