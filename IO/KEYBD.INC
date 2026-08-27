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
; AND A GUEST THAT HOOKS INT 09h ITSELF GETS THE INTERRUPT.  Filling
; the ring answers every program that asks the BIOS for a key, and
; answers none of the ones that ask the KEYBOARD - the full-screen
; programs that put their own handler on INT 09h because they want key
; up as well as key down, or a hot key that works while they are busy,
; or the raw scan code the ring cannot carry.  Those programs saw
; nothing at all: this driver had already taken the byte off port 60h
; before the guest could have looked, and no INT 09h was ever delivered
; into the box.  MSD is the one that showed it - it comes up, reports
; the machine correctly, and will not take a keystroke.
;
; The rule the split follows is ONE CONSUMER PER KEYSTROKE, decided at
; the instant the interrupt arrives, by asking whether a box is running
; and whether its INT 09h is still the ROM's:
;
;   NO BOX RUNNING, or the vector is the ROM's own - this driver
;   decodes the key and puts it in the ring, exactly as before.  That
;   is every case that worked in v0.34 and v0.35, and it still takes
;   the same path.
;
;   A BOX IS RUNNING AND IT HAS HOOKED INT 09h - the scan code is
;   QUEUED instead, and an INT 09h is delivered into the guest.  Its
;   handler reads port 60h, which the monitor traps, and gets the byte
;   this queue is holding; if it chains on to the ROM's handler, the
;   ROM fills the ring.  Either way the whole keystroke belongs to one
;   chain, and there is no second decoder producing a duplicate.
;
; The ROM's vector is captured at boot, before anything has had the
; chance to hook it, because "has this been hooked" has no other
; answer: a guest's IVT is the machine's IVT and the ROM address is
; not a constant.
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

KBQ_SIZE        EQU     32              ; scan codes held for a DOS box.  A
                                        ; key is at most three of them, and
                                        ; the guest drains it inside its own
                                        ; INT 09h, so this is deep enough to
                                        ; be an overflow nobody reaches

;=====================================================================
; kb_init - drain anything the BIOS left in the controller.
;=====================================================================
kb_init:
        ; The ROM's INT 09h, before there is any way for it to have
        ; been changed.  Everything after this compares against it to
        ; decide whether a DOS box has a keyboard handler of its own.
        mov     eax,dword ptr ds:[IVT_INT09]
        mov     dword ptr [kb_romint9],eax
        call    kb_boxreset

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
;
; Whose keystroke this is gets decided first, and once.  If it is the
; box's it goes on the queue and the decoding below runs for its side
; effects only: the modifier state, because Ctrl-Alt-Del has to be
; caught HERE - the ROM's handler would answer it by jumping to the
; power-on entry point, in V86, which is not a reboot but a dead box -
; and nothing else.  The translation, the ring and 40:17 are all left
; to the guest's own chain, which is the whole point of handing it
; over.
;
; The interrupt itself is not delivered from here.  irq_common does it
; on the way out, where the frame the CPU pushed says whether the code
; we interrupted was the guest - see v86_irqexit.
;=====================================================================
irq_keyboard:
        in      al,KB_DATA

        mov     byte ptr [kb_forbox],0
        cmp     byte ptr ds:[v86_active],0
        je      kb_decode               ; no box: the keyboard is all ours
        mov     ebx,dword ptr ds:[IVT_INT09]
        cmp     ebx,dword ptr [kb_romint9]
        je      kb_decode               ; a box, but it has not hooked 09h,
                                        ; so the ring is still the answer
        mov     byte ptr [kb_forbox],1
        call    kbq_put

kb_decode:
        cmp     al,0E0h                 ; extended key prefix
        jne     kb_notE0
        or      byte ptr [kb_flags],KBF_E0
        ret
kb_notE0:
        mov     ah,al
        and     ah,80h                  ; AH != 0 means this is a break
        and     al,7Fh

;--- the fake shifts, which are not keys ------------------------------
; A keyboard sends E0 2A in front of a grey key and E0 AA behind it -
; and E0 B6 / E0 36 for the right-hand one - so that a program reading
; 40:17 sees the shift state the key was pressed with rather than the
; one the numeric keypad would have needed.  They are an artefact of
; the wire protocol, and taking them for shift presses means the
; SHIFT STATE LATCHES ON the first time an arrow key is touched.
        test    byte ptr [kb_flags],KBF_E0
        jz      kb_realmod
        cmp     al,2Ah
        je      kb_clr_e0
        cmp     al,36h
        je      kb_clr_e0
kb_realmod:

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
        test    byte ptr [kb_flags],KBF_E0
        jnz     kb_grey                 ; the cursor pad and its neighbours

        cmp     al,3Bh                  ; THE FUNCTION KEYS ARE FOUR KEYS
        jb      kb_plain                ; EACH.  F1 is 3Bh, Shift-F1 is
        cmp     al,44h                  ; 54h, Ctrl-F1 5Eh and Alt-F1 68h,
        jbe     kb_fkey                 ; and a program that offers Alt-F4
        cmp     al,57h                  ; is reading for that last one and
        je      kb_fkey11               ; not for 3Bh with a flag set - the
        cmp     al,58h                  ; flags are gone by the time it
        je      kb_fkey11               ; looks
kb_plain:
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
        test    byte ptr [kb_flags],KBF_ALT
        jnz     kb_altkey
        test    byte ptr [kb_flags],KBF_CTRL
        jz      kb_store
        or      al,al
        jz      kb_store
        and     al,1Fh                  ; Ctrl-A becomes 01h, and so on
        jmp     short kb_store

;--- Alt and a key is an extended key, not a letter -------------------
; Alt-F is how half the DOS software ever written opens its File menu,
; and it arrives as ASCII zero with the SCAN code - which is the only
; way it can arrive, because "F" and "Alt-F" would otherwise be the
; same byte.  The digit row is the exception the BIOS makes: Alt-1 to
; Alt-0 have codes of their own at 78h..81h, with Alt-hyphen and
; Alt-equals behind them, rather than their unshifted scan codes.
;
; The keypad is left out of this on purpose.  Alt with a keypad digit
; is how a character is entered by number, and a real BIOS answers it
; by composing rather than by reporting anything at all; giving those
; keys an extended code would be inventing a third behaviour.  Without
; Num Lock tracking the honest thing is to let them stay digits.
kb_altkey:
        cmp     ah,47h
        jae     kb_store                ; keypad and beyond: leave it be
        cmp     ah,02h
        jb      kb_altout
        cmp     ah,0Dh
        ja      kb_altout
        add     ah,78h - 02h
kb_altout:
        xor     al,al

kb_store:
        cmp     byte ptr [kb_forbox],0
        jne     kb_clr_e0               ; the guest's chain fills the ring
        call    kb_put
kb_clr_e0:
        and     byte ptr [kb_flags],NOT KBF_E0
        ret

;=====================================================================
; THE GREY KEYS.
;
; An E0 prefix means the key is the one on the cursor pad rather than
; the one on the numeric keypad, and the two send THE SAME SCAN CODE.
; Until this release the prefix was tracked, cleared, and never once
; consulted - so the arrow keys went through the ordinary table and
; came out as the digits printed on the keypad.  Pressing Left typed a
; four.
;
; The scan code is the same for both and the BIOS distinguishes them
; in the ASCII byte: E0h for the grey key, 00h for the keypad one.
; That is what INT 16h function 10h reports and what function 00h
; folds back to a zero, and it is why kb_getchar folds it too - the
; classic call is what the DOS console is.
;
; CTRL GIVES EACH ONE A CODE OF ITS OWN, and they are not adjacent to
; anything: Ctrl-Left is 73h, Ctrl-Right 74h, Ctrl-Home 77h.  A text
; editor reads exactly those for "word left" and "top of file", so
; they are a table rather than a rule.
;=====================================================================
kb_grey:
        cmp     al,1Ch                  ; the keypad's Enter is an Enter
        je      kb_greyent
        cmp     al,35h                  ; and its slash is a slash
        je      kb_greysl
        cmp     al,47h
        jb      kb_clr_e0               ; Print Screen, Break, the Windows
        cmp     al,53h                  ; keys: nothing the ring can carry
        ja      kb_clr_e0

        mov     ah,al
        mov     al,0E0h                 ; "the grey one", not the keypad
        test    byte ptr [kb_flags],KBF_CTRL
        jz      kb_store
        movzx   ebx,ah
        sub     ebx,47h
        mov     ah,[kb_ctrlnav + ebx]
        or      ah,ah
        jz      kb_clr_e0               ; no Ctrl form of that one
        jmp     short kb_store

kb_greyent:
        mov     ah,1Ch
        mov     al,13
        jmp     kb_haveasc
kb_greysl:
        mov     ah,35h
        mov     al,'/'
        jmp     kb_haveasc

;---------------------------------------------------------------------
; kb_fkey - F1..F10 (3Bh..44h), shifted into whichever of the four
;           rows the modifiers ask for.  Alt wins over Ctrl over
;           Shift, which is the order the BIOS resolves them in.
;---------------------------------------------------------------------
kb_fkey:
        mov     ah,al
        xor     al,al
        test    byte ptr [kb_flags],KBF_ALT
        jnz     kf_alt
        test    byte ptr [kb_flags],KBF_CTRL
        jnz     kf_ctrl
        test    byte ptr [kb_flags],KBF_SHIFT
        jnz     kf_shift
        jmp     short kb_store
kf_shift:
        add     ah,54h - 3Bh
        jmp     short kb_store
kf_ctrl:
        add     ah,5Eh - 3Bh
        jmp     short kb_store
kf_alt:
        add     ah,68h - 3Bh
        jmp     short kb_store

;---------------------------------------------------------------------
; kb_fkey11 - F11 and F12, which are 57h and 58h on the wire and
;             85h..8Ch to a program.  They used to be reported as
;             their scan codes, which are the ones a 101-key BIOS
;             never sends and nothing looks for.
;---------------------------------------------------------------------
kb_fkey11:
        sub     al,57h                  ; 0 = F11, 1 = F12
        mov     ah,al
        add     ah,85h
        xor     al,al
        test    byte ptr [kb_flags],KBF_ALT
        jnz     kf11_alt
        test    byte ptr [kb_flags],KBF_CTRL
        jnz     kf11_ctrl
        test    byte ptr [kb_flags],KBF_SHIFT
        jnz     kf11_shift
        jmp     short kb_store
kf11_shift:
        add     ah,2
        jmp     short kb_store
kf11_ctrl:
        add     ah,4
        jmp     short kb_store
kf11_alt:
        add     ah,6
        jmp     short kb_store

; Ctrl and a cursor-pad key, indexed by scan code from 47h (Home).
; A zero means the BIOS has no code for that combination.
kb_ctrlnav:
        db      77h                     ; 47 Home    -> Ctrl-Home
        db      8Dh                     ; 48 Up
        db      84h                     ; 49 PgUp
        db      0                       ; 4A grey minus - keypad only
        db      73h                     ; 4B Left
        db      0                       ; 4C
        db      74h                     ; 4D Right
        db      0                       ; 4E grey plus - keypad only
        db      75h                     ; 4F End
        db      91h                     ; 50 Down
        db      76h                     ; 51 PgDn
        db      92h                     ; 52 Insert
        db      93h                     ; 53 Delete

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
        ; 40:17 has ONE writer at a time, for the reason the ring does.
        ; While the box owns the keystroke the ROM's handler is
        ; publishing the shift state - and it tracks Num Lock and
        ; Scroll Lock, which this driver does not, so writing over it
        ; here would turn its lock lights off on every shift key.
        cmp     byte ptr [kb_forbox],0
        jne     kb_clr_e0
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
; THE DOS BOX'S SCAN CODES.
;
; A queue and a LATCH, and the second one is the part that has to be
; got right, because it is where the 8042 is being stood in for.
;
; A real keyboard controller holds one byte in its output register and
; goes on holding it until the next one arrives.  READING IT DOES NOT
; TAKE IT AWAY.  That is not a detail: it is what makes a chained INT
; 09h handler possible at all.  A resident program's handler reads port
; 60h to see what was pressed and then jumps to the handler it replaced
; - which reads port 60h again, and must get the SAME byte, or the two
; halves of the chain are looking at two different keystrokes.  A queue
; that popped on every read would give the second reader the next key,
; or nothing, and a program that hooks the keyboard and passes the keys
; on is the commonest kind there is.
;
; So the queue exists only to hold codes that arrive faster than the
; guest is given interrupts for them, and it is emptied ONE CODE PER
; INT 09h DELIVERED, at the moment of delivery - which is exactly when
; the hardware would have latched it.  Everything the guest reads off
; the port between one interrupt and the next is the latch, as often as
; it likes.
;=====================================================================
kbq_put:
        push    eax
        push    ebx
        push    ecx
        movzx   ebx,byte ptr [kbq_tail]
        lea     ecx,[ebx+1]
        and     cl,KBQ_SIZE - 1
        cmp     cl,byte ptr [kbq_head]
        je      kqp_full                ; one slot stays empty, as the BIOS
                                        ; ring does, so that full and empty
                                        ; do not look alike
        mov     [kbq + ebx],al
        mov     byte ptr [kbq_tail],cl
kqp_full:
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; kbq_take - move the next queued scan code into the latch.  CF clear
;            if there was one, which is also the monitor's answer to
;            "is an INT 09h owed to the guest".
;---------------------------------------------------------------------
kbq_take:
        push    ebx
        movzx   ebx,byte ptr [kbq_head]
        cmp     bl,byte ptr [kbq_tail]
        je      kqt_empty
        mov     al,[kbq + ebx]
        mov     byte ptr [kb_latch],al
        mov     byte ptr [kb_lvalid],1
        mov     byte ptr [kb_obf],1     ; and the status bit that says so,
                                        ; until the guest reads the port
        inc     bl
        and     bl,KBQ_SIZE - 1
        mov     byte ptr [kbq_head],bl
        clc
        pop     ebx
        ret
kqt_empty:
        stc
        pop     ebx
        ret

;---------------------------------------------------------------------
; kb_boxreset - IOS_KBBOX.  A DOS box is starting; nothing queued for
;               the last one is meant for this one.
;
; The latch goes too.  Left standing, it would answer the new guest's
; first read of port 60h with a key pressed at the program before it -
; and a program that reads the port before any interrupt has been
; delivered is asking the hardware what it is holding, which at that
; point is nothing at all.
;---------------------------------------------------------------------
kb_boxreset:
        mov     byte ptr [kbq_head],0
        mov     byte ptr [kbq_tail],0
        mov     byte ptr [kb_lvalid],0
        mov     byte ptr [kb_obf],0
        mov     byte ptr [vio_kbdwr],0  ; and no half-finished 8042 sequence
        mov     byte ptr [vio_kbdrd],0  ; from the box that has just ended
        mov     byte ptr [vio_kbdcmd],0
        mov     byte ptr [vio_kbdaux],0
        mov     byte ptr [vio_kbdthru],0
        ret

;=====================================================================
; kb_typematic - IOS_KBRATE.  AL = the 8042's rate byte: bits 4-0 the
;                repeat rate (0 = 30 a second, 31 = 2), bits 6-5 the
;                delay before repeating (0 = 250ms, 3 = 1000ms).
;
; CONFIG.SYS's KEYRATE= is what calls this, and CONFIG.INC does the
; conversion from the numbers MODE CON uses to the byte the hardware
; wants - the kernel's side of the line is the hardware's units.
;
; INTERRUPTS OFF FOR THE WHOLE EXCHANGE, and the acknowledgements are
; swallowed here rather than left for the decoder.  A command to the
; keyboard is a CONVERSATION - F3h, an ACK, the argument, another ACK -
; and by the time this runs IRQ 1 belongs to irq_keyboard, which would
; take those ACKs off the port as though they were keystrokes.  They
; are harmless when it does (FAh has bit 7 set, so it reads as a break
; code for a key nobody pressed), but a HALF-DELIVERED conversation is
; not harmless at all: a keyboard left waiting for its argument treats
; the next thing it is sent as one.  So the two bytes go out together
; and the two answers come back here, and the controller is drained
; afterwards so that nothing is left in it when interrupts come back.
;
; A keyboard that will not talk simply keeps the rate it had.  Nothing
; here waits for ever and nothing here reports failure: a typematic
; rate is a preference, and a machine that boots slightly differently
; from the way somebody asked is better than one that does not boot.
;=====================================================================
kb_typematic:
        pushfd
        cli
        push    eax
        mov     ah,al
        mov     al,0F3h                 ; "set typematic rate/delay"
        call    kb_send
        call    kb_eat                  ; its ACK
        mov     al,ah
        call    kb_send
        call    kb_eat                  ; and the argument's
        pop     eax
        call    kb_drain                ; anything else it had to say
        popfd
        ret

;---------------------------------------------------------------------
; kb_eat - wait a moment for a byte from the keyboard and swallow it.
;          kb_drain - swallow whatever is there without waiting.
;---------------------------------------------------------------------
kb_eat:
        push    eax
        push    ecx
        mov     ecx,200000
ke_wait:
        in      al,KB_CMD
        test    al,1                    ; output buffer full?
        jnz     ke_take
        dec     ecx
        jnz     ke_wait
        jmp     short ke_out
ke_take:
        in      al,KB_DATA
ke_out:
        pop     ecx
        pop     eax
        ret

kb_drain:
        push    eax
kd_lp:
        in      al,KB_CMD
        test    al,1
        jz      kd_out
        in      al,KB_DATA
        jmp     short kd_lp
kd_out:
        pop     eax
        ret

;---------------------------------------------------------------------
; kb_send - wait for the 8042 to be ready for a byte, then write AL.
;           Gives up rather than hanging: a machine with no keyboard
;           controller must still boot.
;---------------------------------------------------------------------
kb_send:
        push    eax
        push    ecx
        mov     ecx,100000
ks_wait:
        in      al,KB_CMD
        test    al,2                    ; input buffer still full?
        jz      ks_go
        dec     ecx
        jnz     ks_wait
        pop     ecx
        pop     eax
        ret
ks_go:
        pop     ecx
        pop     eax
        out     KB_DATA,al
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

        ; E0h IN THE ASCII BYTE MEANS "THE GREY ONE", and this call is
        ; the classic one.  INT 16h function 00h folds it to a zero and
        ; function 10h does not; the DOS console API has no function
        ; 10h, and every program reading a key through INT 21h tests
        ; AL for zero to find an extended key.  Leaving E0h standing
        ; would make an arrow key look like a printable character.
        ; The scan code in AH is untouched and is what tells them
        ; apart anyway.
        cmp     al,0E0h
        jne     kg_notgrey
        xor     al,al
kg_notgrey:
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
kb_forbox   db  0                       ; this scan code belongs to the DOS
                                        ; box, so decode it for Ctrl-Alt-Del
                                        ; and the modifier state and stop
kb_romint9  dd  0                       ; INT 09h as the ROM left it, taken
                                        ; at boot: anything else in that slot
                                        ; is a guest's own handler
kb_latch    db  0                       ; the guest's port 60h, standing in
kb_lvalid   db  0                       ; for the 8042's output register...
kb_obf      db  0                       ; ...and its "there is a byte" bit
kbq_head    db  0
kbq_tail    db  0
kbq         db  KBQ_SIZE dup (0)

null_idtr:  dw  0
            dd  0
