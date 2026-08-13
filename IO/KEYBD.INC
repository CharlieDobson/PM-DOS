;=====================================================================
; KEYBD.INC - PS/2 keyboard driver (scan code set 1)
;
; The IRQ 1 handler translates make codes into an ASCII/scan-code pair
; and drops it in a ring buffer.  We do not go through the BIOS, so
; this keeps working with the machine in protected mode.
;
; Included by IOPM.ASM.
;=====================================================================

KB_BUFSZ        EQU     32              ; must stay a power of two
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
        mov     dword ptr [kb_head],0
        mov     dword ptr [kb_tail],0
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
kb_shift:
        test    ah,ah
        jnz     kb_shift_up
        or      byte ptr [kb_flags],KBF_SHIFT
        jmp     short kb_clr_e0
kb_shift_up:
        and     byte ptr [kb_flags],NOT KBF_SHIFT
        jmp     short kb_clr_e0

kb_ctrl:
        test    ah,ah
        jnz     kb_ctrl_up
        or      byte ptr [kb_flags],KBF_CTRL
        jmp     short kb_clr_e0
kb_ctrl_up:
        and     byte ptr [kb_flags],NOT KBF_CTRL
        jmp     short kb_clr_e0

kb_alt:
        test    ah,ah
        jnz     kb_alt_up
        or      byte ptr [kb_flags],KBF_ALT
        jmp     short kb_clr_e0
kb_alt_up:
        and     byte ptr [kb_flags],NOT KBF_ALT
        jmp     short kb_clr_e0

kb_caps:
        test    ah,ah
        jnz     kb_clr_e0               ; toggle on the make code only
        xor     byte ptr [kb_flags],KBF_CAPS
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
; kb_put - append AX (AL = ASCII, AH = scan code) to the ring buffer.
;          Silently drops the key if the buffer is full.
;=====================================================================
kb_put:
        push    ebx
        push    ecx
        mov     ebx,[kb_head]
        mov     ecx,ebx
        inc     ecx
        and     ecx,KB_BUFSZ - 1
        cmp     ecx,[kb_tail]
        je      kp_full
        mov     [kb_buf + ebx*2],ax
        mov     [kb_head],ecx
kp_full:
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; kb_status - ZF set if the buffer is empty (IOS_CONSTAT).
;=====================================================================
kb_status:
        push    eax
        mov     eax,[kb_head]
        cmp     eax,[kb_tail]
        pop     eax
        ret

;=====================================================================
; kb_getchar - block until a key arrives, then return it in AX
;              (AL = ASCII, AH = scan code).  IOS_CONIN.
;=====================================================================
kb_getchar:
        push    ebx
kg_wait:
        cli
        mov     ebx,[kb_tail]
        cmp     ebx,[kb_head]
        jne     kg_have
        sti
        hlt                             ; idle until the next interrupt
        jmp     short kg_wait
kg_have:
        movzx   eax,word ptr [kb_buf + ebx*2]
        inc     ebx
        and     ebx,KB_BUFSZ - 1
        mov     [kb_tail],ebx
        sti
        pop     ebx
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
kb_buf      dw  KB_BUFSZ dup(0)
kb_head     dd  0
kb_tail     dd  0
kb_flags    db  0

null_idtr:  dw  0
            dd  0
