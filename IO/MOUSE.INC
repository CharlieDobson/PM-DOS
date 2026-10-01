;=====================================================================
; MOUSE.INC - the mouse, and INT 33h the way Microsoft's driver
;             answers it
;
; A DOS machine gets its mouse from a TSR: MOUSE.COM takes IRQ 12 (or
; the serial port's IRQ), keeps a cursor on the screen, and answers
; INT 33h.  PM-DOS cannot load one.  The kernel owns every interrupt a
; mouse driver would need, and a native program has no real-mode IVT to
; find it through anyway.  So the driver is built in, like the keyboard
; driver, and it answers two kinds of program with the same state:
;
;   A 16-BIT PROGRAM IN A DOS BOX calls INT 33h through its own IVT,
;   which points at a stub below 1MB (MS_PARAS paragraphs the DOS side
;   buys at boot).  The stub's first byte is a HLT, the box's loop hands
;   that HLT to ms_hlt, and the registers go in and come back through
;   the V86 context.  A program that hooks INT 33h and chains on reaches
;   the same HLT, one step later.
;
;   A NATIVE PROGRAM calls INT 33h at ring 3 and lands in ms_int33
;   through a DPL 3 gate.  The registers are the same, widened: a
;   pointer in ES:DX is an offset in EDX, as it is for INT 21h.
;
; EVENT HANDLERS (0Ch, 14h, 18h) are called the Microsoft way: AX the
; conditions, BX the buttons, CX and DX the position, SI and DI the
; mickey counts, and a FAR return.  In a box that is a frame pushed on
; the guest's stack and a jump to the stub's second entry, which saves
; everything, calls the handler, and IRETs - see ms_boxwant.  A native
; program's handler is entered the way a signal would be: its context
; is kept here, its frame is rewritten, and its RETF lands on an
; INT 33h in its own PSP that brings the context back - see ms_r3chk.
; Either kind is only ever called while its own program is running,
; and never while another handler of this driver's is.
;
; THE CURSOR is drawn from the interrupt, as MOUSE.COM draws it: a
; software or hardware text cursor, and the 16x16 graphics cursor in
; the CGA, EGA and VGA modes a DOS mouse driver knew.  The kernel's own
; console hides it around every write it makes (ms_conhide), so a
; program that draws through 21h/F0h never has to; a DOS program
; writing video memory itself hides it first, as it always had to.
;
; THE HARDWARE is a PS/2 mouse on the 8042's auxiliary port, or failing
; that a Microsoft serial mouse on COM1 or COM2.  Once the kernel
; drives a PS/2 mouse the auxiliary device is the kernel's, as the
; keyboard already is: a box's commands to it are refused (vio_o64),
; and so is the BIOS's pointing-device interface (ms_i15), which would
; otherwise reset the mouse out from under the driver.
;
; Included by IOPM.ASM.
;=====================================================================

MGT_TEXT        EQU     0               ; ms_gtype: what the screen is
MGT_PLANAR      EQU     1               ; EGA/VGA 16-colour and 2-colour
MGT_256         EQU     2               ; mode 13h
MGT_CGA4        EQU     3               ; modes 04h and 05h
MGT_CGA2        EQU     4               ; mode 06h
MGT_NONE        EQU     5               ; a mode no mouse driver draws in

MDR_TEXT        EQU     1               ; ms_drawn: a software text cursor
MDR_HW          EQU     2               ; ...the CRTC's own cursor, moved
MDR_GFX         EQU     3               ; ...a graphics cursor

MSB_BOX         EQU     1               ; ms_busy: a box's handler is running
MSB_NATIVE      EQU     2               ; ...or a native program's

MS_VERSION      EQU     0820h           ; what 24h says: Microsoft's 8.20
MS_MAXFN        EQU     33h             ; the table below covers 00h-32h

;=====================================================================
; Data first, for the reason CONSOLE.INC gives: wasm fixes a symbol's
; type at its first mention.  Nothing falls into it - KEYBD.INC ends in
; its own data.
;
; ms_state .. MS_STATELEN is what 15h/16h/17h save and restore: the
; driver as a program sees it.  What follows it is the machinery.
;=====================================================================
ms_state:
ms_x            dw      320             ; the pointer, in virtual pixels
ms_y            dw      100
ms_xmin         dw      0
ms_xmax         dw      639
ms_ymin         dw      0
ms_ymax         dw      199
ms_mkx          dw      8               ; mickeys per 8 pixels, across...
ms_mky          dw      16              ; ...and down
ms_dspeed       dw      64              ; mickeys a second before doubling
ms_sensx        dw      50              ; 1Ah's speeds: 50 is as it comes
ms_sensy        dw      50
ms_accx         dd      0               ; mickeys too few to be a pixel yet,
ms_accy         dd      0               ; scaled - see ms_motion
ms_cntx         dw      0               ; 0Bh's counts, since it last asked
ms_cnty         dw      0
ms_rawx         dw      0               ; running totals, for SI and DI
ms_rawy         dw      0
ms_btn          dw      0               ; bit 0 left, 1 right, 2 middle
ms_pcnt         dw      3 dup(0)        ; presses since 05h asked...
ms_px           dw      3 dup(0)        ; ...and where the last one was
ms_py           dw      3 dup(0)
ms_rcnt         dw      3 dup(0)        ; releases, for 06h
ms_rx           dw      3 dup(0)
ms_ry           dw      3 dup(0)
ms_vis          dw      -1              ; 0 shown, below 0 hidden
ms_ctype        dw      0               ; 0 software text, 1 hardware text
ms_tscr         dw      77FFh           ; the text screen mask, or the
ms_tcur         dw      7700h           ; hardware cursor's first/last line
ms_hotx         dw      0               ; the graphics cursor's hot spot
ms_hoty         dw      0
ms_gmask        dw      32 dup(0)       ; its screen mask, then cursor mask
ms_cx1          dw      0               ; 10h's region
ms_cy1          dw      0
ms_cx2          dw      0
ms_cy2          dw      0
ms_condon       dw      0
ms_evmask       dw      0               ; 0Ch's handler
ms_evhand       dd      0               ; seg:off for a box, an offset for a
ms_evown        dd      0               ; native program; and whose it is
ms_altmask      dw      3 dup(0)        ; 18h's three
ms_althand      dd      3 dup(0)
ms_altown       dd      3 dup(0)
ms_rate         dw      3               ; 1Ch: 100 a second
ms_page         dw      0               ; 1Dh
ms_lang         dw      0               ; 22h: English is all there is
ms_lpen         dw      1               ; 0Dh / 0Eh
ms_rstown       dd      0               ; the program that last reset us
MS_STATELEN     EQU     $ - ms_state

;--- the hardware ----------------------------------------------------
ms_hw           db      MS_NONE
ms_nbtn         db      2
ms_irq          db      0
ms_off          db      0               ; 1Fh: disabled
ms_probing      db      0               ; talking to the mouse: its bytes
                                        ; are not packets, leave them
ms_busy         db      0               ; MSB_*: a handler is running
ms_wake         db      0               ; IOS_MSWAKE's news
ms_pktn         db      0               ; bytes of a packet so far
ms_pkt          db      4 dup(0)
ms_port         dd      0               ; the serial mouse's UART
ms_lasttick     dd      0               ; when the last packet byte came
ms_spdtick      dd      0               ; the tick ms_spdacc is counting
ms_spdacc       dd      0               ; mickeys moved in it
ms_pend         dw      0               ; conditions since a handler ran
ms_oldpos       dd      0               ; X and Y before a movement
ms_bytesin      dd      0               ; PS/2 bytes received, ever
ms_busyown      dd      0               ; whose handler is running

;--- what the DOS side told us ---------------------------------------
ms_seg          dd      0               ; the stub below 1MB, 0 = none
ms_fallback     dd      0               ; a DPMI client's INT 33h
ms_pdepth       dd      0               ; -> r3_depth
ms_pcall        dd      0               ; -> r3_calldep
ms_excl         dd      0               ; the console is writing: no cursor

;--- the screen as the cursor last saw it ----------------------------
ms_gmode        db      0FFh            ; the BIOS mode it was worked out for
ms_gtype        db      MGT_TEXT
ms_xshift       db      0               ; 1: the virtual x is twice the pixel
ms_cshift       db      3               ; text: virtual pixels a cell, as a
                                        ; shift - 3, or 4 on 40 columns
ms_cols         dd      80
ms_rows         dd      25
ms_vxmax        dd      639             ; the whole screen, virtually
ms_vymax        dd      199
ms_gheight      dd      200             ; pixels
ms_gbpr         dd      80              ; bytes a row
ms_gpoff        dd      0               ; the page's offset
ms_gpbase       dd      VGA_TEXT        ; ...and its address
ms_crtc         dd      CRTC_IDX

;--- the cursor as it is on the screen -------------------------------
ms_drawn        db      0               ; MDR_*, 0 = not drawn
ms_gundo        db      0
ms_gs           db      0
                db      0
ms_daddr        dd      0               ; a text cursor: the cell...
ms_dcell        dd      0
ms_dsave        dw      0               ; ...what was in it...
ms_dval         dw      0               ; ...and what we put there
ms_hwsave       db      4 dup(0)        ; CRTC 0Ah, 0Bh, 0Eh, 0Fh
ms_hwidx        db      0
ms_vsidx        db      0               ; VGA sequencer and graphics
ms_vmap         db      0               ; controller state, for a planar
ms_vgidx        db      0               ; cursor to put back
ms_vgcsave      db      5 dup(0)
ms_gx           dd      0               ; a graphics cursor: its top left,
ms_gy           dd      0               ; in pixels
ms_gbx          dd      0
ms_gk           dd      0
ms_gidx         dd      0
ms_gplane       dd      0
ms_grow         dd      0
ms_gand         dd      0
ms_gxor         dd      0
ms_gsave        db      256 dup(0)

;--- a native handler's interrupted context --------------------------
ms_r3regs       dd      8 dup(0)        ; its PUSHAD block
ms_r3eip        dd      0
ms_r3efl        dd      0
ms_r3esp        dd      0
ms_r3blk        dd      0
ms_dcond        dd      0
ms_dhand        dd      0
ms_dtok         dd      0

;--- one INT 33h call, whoever made it -------------------------------
msr_eax         dd      0
msr_ebx         dd      0
msr_ecx         dd      0
msr_edx         dd      0
msr_esi         dd      0
msr_edi         dd      0
msr_es          dd      0
msr_ptr         dd      0               ; ES:DX as a flat address...
msr_plen        dd      0               ; ...and how much is the caller's
msr_tok         dd      0               ; who is calling
msr_box         db      0               ; 1: a DOS box, and a handler is
                db      0,0,0           ; ES:DX

; The graphics modes: mode, type, x shift, bytes a row, height, base.
MSGM_LEN        EQU     12
ms_gmodes:
        db      04h, MGT_CGA4,   1, 0
        dw      80, 200
        dd      0B8000h
        db      05h, MGT_CGA4,   1, 0
        dw      80, 200
        dd      0B8000h
        db      06h, MGT_CGA2,   0, 0
        dw      80, 200
        dd      0B8000h
        db      0Dh, MGT_PLANAR, 1, 0
        dw      40, 200
        dd      0A0000h
        db      0Eh, MGT_PLANAR, 0, 0
        dw      80, 200
        dd      0A0000h
        db      0Fh, MGT_PLANAR, 0, 0
        dw      80, 350
        dd      0A0000h
        db      10h, MGT_PLANAR, 0, 0
        dw      80, 350
        dd      0A0000h
        db      11h, MGT_PLANAR, 0, 0
        dw      80, 480
        dd      0A0000h
        db      12h, MGT_PLANAR, 0, 0
        dw      80, 480
        dd      0A0000h
        db      13h, MGT_256,    1, 0
        dw      320, 200
        dd      0A0000h
        db      0FFh

; The arrow every mouse driver starts with: screen mask, cursor mask.
ms_defmask:
        dw      3FFFh, 1FFFh, 0FFFh, 07FFh, 03FFh, 01FFh, 00FFh, 007Fh
        dw      003Fh, 001Fh, 01FFh, 10FFh, 30FFh, 0F87Fh, 0F87Fh, 0FC3Fh
        dw      0000h, 4000h, 6000h, 7000h, 7800h, 7C00h, 7E00h, 7F00h
        dw      7F80h, 7C00h, 6C00h, 4600h, 0600h, 0300h, 0300h, 0000h

ms_vgcregs      db      1, 3, 4, 5, 8   ; the graphics controller registers
                                        ; a planar cursor changes

;---------------------------------------------------------------------
; The stub below 1MB, copied there by ms_setup.  Offsets are taken from
; the labels, so the code that knows them cannot drift from the bytes.
;---------------------------------------------------------------------
ms_stub:
mstub_entry:
        db      0F4h, 0CFh              ; INT 33h lands here: HLT (IRET
                                        ; unreached - ms_hlt pops the frame)
mstub_event:
        db      60h, 1Eh, 06h           ; an event: PUSHA, PUSH DS, PUSH ES
mstub_evhlt:
        db      0F4h                    ; HLT: the registers are loaded
mstub_call:
        db      2Eh, 0FFh, 1Eh          ; CALL FAR CS:[mstub_hptr]
        dw      mstub_hptr - ms_stub
mstub_done:
        db      0F4h                    ; HLT: the handler is back
        db      07h, 1Fh, 61h, 0CFh     ; POP ES, POP DS, POPA, IRET
mstub_hptr:
        dd      0                       ; the handler being called
mstub_iret:
        db      0CFh                    ; an IRET, for 1Fh to hand back
mstub_verstr:
        db      'V', 8, 20h, 0          ; 6Dh
mstub_copyr:
        db      'Copyright 2026 PM-DOS', 0      ; 4Dh
MS_STUBLEN      EQU     $ - ms_stub

MSS_ENTRY       EQU     mstub_entry - ms_stub
MSS_EVENT       EQU     mstub_event - ms_stub
MSS_EVHLT       EQU     mstub_evhlt - ms_stub
MSS_CALL        EQU     mstub_call - ms_stub
MSS_DONE        EQU     mstub_done - ms_stub
MSS_HPTR        EQU     mstub_hptr - ms_stub
MSS_IRET        EQU     mstub_iret - ms_stub
MSS_VERSTR      EQU     mstub_verstr - ms_stub
MSS_COPYR       EQU     mstub_copyr - ms_stub

IF MS_STUBLEN GT MS_PARAS * 16
        .ERR    <the INT 33h stub has outgrown MS_PARAS>
ENDIF

;=====================================================================
; ms_init - find the mouse.  Called once from pm_entry, with interrupts
;           ON: the clock is how every wait here is measured.
;
; PS/2 first, because a machine with an auxiliary port and a mouse on
; it answers in a few milliseconds and one without says so almost as
; fast.  A serial mouse is found by cutting its power and waiting for
; it to announce itself, which costs a third of a second a port and is
; only paid when there is no PS/2 mouse.
;=====================================================================
ms_init:
        pushad
        call    ms_ps2start
        jc      mi_serial
        mov     byte ptr [ms_hw],MS_PS2
        mov     dword ptr [irq_vectors + 12 * 4],OFFSET irq_mouse
        mov     ecx,12
        call    ms_irqon
        jmp     short mi_found
mi_serial:
        call    ms_serfind
        jc      mi_none
        mov     byte ptr [ms_hw],MS_SERIAL
        movzx   ecx,byte ptr [ms_irq]
        mov     dword ptr [irq_vectors + ecx * 4],OFFSET irq_serial
        call    ms_irqon
        mov     edx,dword ptr [ms_port]
        inc     edx                     ; IER: an interrupt for each byte
        mov     al,01h
        out     dx,al
mi_found:
mi_none:
        mov     al,byte ptr [ms_hw]
        mov     byte ptr [plog+PLM_HW],al
        mov     dword ptr [msr_tok],0
        call    ms_softreset
        popad
        ret

;---------------------------------------------------------------------
; ms_announce - say what the mouse is on, if there is one.  All
;               registers kept.
;
; A RELEASE BOOT SAYS ONE LINE AND THIS IS A SECOND, on a machine with
; a mouse and on no other.  It is here because the driver is in the
; kernel: there is no MOUSE.COM to load and so no banner to say that
; one was found, and "is there a mouse" was a question the machine
; could only be asked by running something.  A machine without one
; boots exactly as it did.
;---------------------------------------------------------------------
ms_announce:
        push    esi
        cmp     byte ptr [ms_hw],MS_NONE
        je      man_out
        mov     esi,OFFSET msgMouse
        call    con_str
        mov     esi,OFFSET msgMsPs2
        cmp     byte ptr [ms_hw],MS_PS2
        je      man_say
        mov     esi,OFFSET msgMsCom
man_say:
        call    con_str
man_out:
        pop     esi
        ret

msgMouse    db  'Mouse detected on ',0
msgMsPs2    db  'PS/2',13,10,0
msgMsCom    db  'COM'
msgMsComN   db  '1',13,10,0             ; ms_serfind's

;---------------------------------------------------------------------
; ms_irqon - ECX = an IRQ the mouse uses.  What the 8259s are told is
;            worked out in one place now (irq_sync, IRQ.INC), cascade
;            and the masks pic_to_pm puts back included; this says the
;            kernel wants the line.
;---------------------------------------------------------------------
ms_irqon:
        call    irq_want
        ret

;=====================================================================
; THE PS/2 MOUSE.
;
; The 8042 has one output buffer and two devices behind it, and the
; status register's bit 5 says whose byte is in it.  Everything here
; that waits for the MOUSE waits for that bit, and leaves a keyboard
; byte where it is: interrupts are on, and irq_keyboard takes it.
;
; What must NOT happen with interrupts on is asking the CONTROLLER
; something - its command byte, or the interface test - because its
; answer is a byte without bit 5, and IRQ 1 would take that for a scan
; code.  Those two exchanges are made with interrupts off and a bounded
; loop, as kb_typematic makes its own.
;
; ms_probing is up for the whole of any conversation with the mouse:
; irq_mouse and the timer then leave its bytes for the code that asked.
;=====================================================================

;---------------------------------------------------------------------
; ms_ps2start - the controller's aux interface on, the mouse reset and
;               told to report.  CF if there is no PS/2 mouse.
;---------------------------------------------------------------------
;
; WHAT A REAL MACHINE TAUGHT IT.  The first version was written against
; two emulators and found the mouse on both, and then did not find the
; one on a 486.  Three things in it were the emulators' manners and not
; the hardware's:
;
;   A9h, the interface test, was the gate, and its answer had to be
;   zero.  Real controllers answer it with an error on a port that is
;   working - often enough that Linux asks it only as a last resort -
;   so a good mouse was turned away before it had been spoken to.  The
;   gate is now whether the port's disable bit FOLLOWS A7h and A8h,
;   which is what tells a controller with a second port from one
;   without; see ms_kbcaux.
;
;   A mouse was given a second to test itself, and one reset.  A real
;   one can take longer than that from cold, and the first reset after
;   power-on is the one most often lost.  It gets two and a half
;   seconds, and a second reset if the first comes to nothing.
;
;   And nothing was written down.  PROBELOG.INC is what each step read.
;---------------------------------------------------------------------
MS_NRESETS       EQU     2
MS_BATTICKS     EQU     46              ; two and a half seconds
MS_IDTICKS      EQU     4
MS_ACKTICKS     EQU     6               ; a third of a second for an FAh

ms_ps2start:
        pushad
        mov     byte ptr [ms_probing],1
        mov     al,byte ptr ds:[BDA_EQUIP]
        mov     byte ptr [plog+PLM_EQUIP],al
        mov     byte ptr [plog+PLM_SEEN],0
        mov     byte ptr [plog+PLM_TRIES],0
        mov     byte ptr [plog+PLM_STAGE],'C'
        call    ms_auxdrain
        call    ms_kbcaux               ; the port exists, and is on
        jc      mps_no

        mov     byte ptr [ms_resets],MS_NRESETS
mps_reset:
        inc     byte ptr [plog+PLM_TRIES]
        mov     byte ptr [plog+PLM_STAGE],'R'
        call    ms_auxdrain
        mov     al,0FFh                 ; reset
        call    ms_auxcmd
        mov     ah,10h
        mov     edi,PLM_ACK
        call    ms_plogcmd
        jc      mps_again

        mov     byte ptr [plog+PLM_STAGE],'T'
        mov     ecx,MS_BATTICKS         ; its self-test
        call    ms_auxread
        mov     ah,20h
        mov     edi,PLM_BAT
        call    ms_plog
        jc      mps_again
        cmp     al,0AAh
        jne     mps_again

        ; its ID, 00h for a plain mouse.  Not waited on for long and not
        ; insisted on: a mouse that passed its self-test is a mouse
        mov     byte ptr [plog+PLM_STAGE],'I'
        mov     ecx,MS_IDTICKS
        call    ms_auxread
        mov     ah,40h
        mov     edi,PLM_ID
        call    ms_plog

        mov     byte ptr [plog+PLM_STAGE],'E'
        mov     byte ptr [ms_nbtn],2
        mov     al,0F4h                 ; and start sending
        call    ms_auxcmd
        mov     ah,80h
        mov     edi,PLM_ENABLE
        call    ms_plogcmd
        jnc     mps_yes
mps_again:
        dec     byte ptr [ms_resets]
        jnz     mps_reset
mps_no:
        mov     byte ptr [ms_probing],0
        popad
        stc
        ret
mps_yes:
        mov     byte ptr [plog+PLM_STAGE],'+'
        mov     byte ptr [ms_pktn],0
        mov     byte ptr [ms_probing],0
        popad
        clc
        ret

ms_resets       db      0
ms_lastrx       db      0               ; the last byte ms_auxcmd read...
ms_gotrx        db      0               ; ...if it read one

;--- ms_plog - AL = a byte that was read unless CF is set, AH = its bit
;--- in PLM_SEEN, EDI = its place in the log.  Everything kept, the
;--- carry included.
ms_plog:
        jc      mpl_out
        mov     byte ptr [plog+edi],al
        or      byte ptr [plog+PLM_SEEN],ah
        clc
mpl_out:
        ret

;--- ms_plogcmd - the same after ms_auxcmd, which is worth writing down
;--- when it FAILED: what the mouse said instead of FAh is the finding.
ms_plogcmd:
        pushfd
        cmp     byte ptr [ms_gotrx],0
        je      mpc_out
        push    eax
        mov     al,byte ptr [ms_lastrx]
        mov     byte ptr [plog+edi],al
        pop     eax
        or      byte ptr [plog+PLM_SEEN],ah
mpc_out:
        popfd
        ret

;---------------------------------------------------------------------
; ms_kbcaux - the controller's side: is there a second port, and if
;             there is, it is enabled, IRQ 12 is turned on and the
;             mouse's clock is set running.  CF if the controller has
;             no aux port, or does not answer.  Interrupts off.
;
; THE BIT THAT FOLLOWS.  Bit 5 of the command byte is the aux port's
; disable, A7h sets it and A8h clears it, and on a controller with no
; second port the two commands mean nothing and the bit stays where it
; was.  So the port is there if the bit is up after A7h and down after
; A8h.  The mouse cannot speak while this is asked - A7h stops its
; clock - so every answer read here is the controller's own, whichever
; way the status register flags it.
;
; A9h IS ASKED ONLY IF THE BIT DID NOT FOLLOW, and FAh is taken for
; "working" along with zero: some controllers answer every command
; with an acknowledge.
;---------------------------------------------------------------------
ms_kbcaux:
        pushfd
        cli
        push    eax
        push    ebx
        push    edi
        call    ms_kbcflush
        mov     al,KBC_RDCMD            ; as the BIOS left it, for the log
        call    ms_kbccmd
        call    ms_kbcany
        mov     ah,01h
        mov     edi,PLM_CMD0
        call    ms_plog

        mov     byte ptr [plog+PLM_STAGE],'7'
        mov     al,KBC_AUXOFF
        call    ms_kbccmd
        call    ms_kbcflush
        mov     al,KBC_RDCMD
        call    ms_kbccmd
        call    ms_kbcany
        mov     ah,02h
        mov     edi,PLM_CMDOFF
        call    ms_plog
        jc      mka_fail
        mov     bh,al

        mov     byte ptr [plog+PLM_STAGE],'8'
        mov     al,KBC_AUXON
        call    ms_kbccmd
        mov     al,KBC_RDCMD
        call    ms_kbccmd
        call    ms_kbcany
        mov     ah,04h
        mov     edi,PLM_CMDON
        call    ms_plog
        jc      mka_fail
        mov     bl,al

        test    bh,KBCB_AUXOFF
        jz      mka_ask
        test    bl,KBCB_AUXOFF
        jz      mka_have
mka_ask:
        mov     byte ptr [plog+PLM_STAGE],'9'
        mov     al,KBC_AUXTEST
        call    ms_kbccmd
        call    ms_kbcany
        mov     ah,08h
        mov     edi,PLM_TEST
        call    ms_plog
        jc      mka_fail
        or      al,al
        jz      mka_on
        cmp     al,0FAh
        jne     mka_fail                ; 01h-04h: a line is stuck
mka_on:
        mov     al,KBC_AUXON            ; the test leaves it as it likes
        call    ms_kbccmd
mka_have:
        mov     byte ptr [plog+PLM_STAGE],'W'
        mov     al,bl
        or      al,KBCB_AUXINT
        and     al,NOT KBCB_AUXOFF
        mov     bl,al
        mov     al,KBC_WRCMD
        call    ms_kbccmd
        call    ms_kbcwait
        mov     al,bl
        out     PORT_KBDATA,al
        call    ms_kbcwait
        pop     edi
        pop     ebx
        pop     eax
        popfd
        clc
        ret
mka_fail:
        mov     al,KBC_AUXON            ; as it was found, as near as can
        call    ms_kbccmd               ; be told: a BIOS that uses the
        pop     edi                     ; port left it on
        pop     ebx
        pop     eax
        popfd
        stc
        ret

;--- ms_kbcflush - whatever is in the output buffer goes, whoever sent
;--- it.  Interrupts off.
ms_kbcflush:
        push    eax
        push    ecx
        mov     ecx,16
mkf_lp:
        in      al,PORT_KBCMD
        test    al,KBS_OBF
        jz      mkf_out
        in      al,PORT_KBDATA
        call    io_delay
        loop    mkf_lp
mkf_out:
        pop     ecx
        pop     eax
        ret

;--- ms_kbcany - the next byte in the output buffer -> AL, CF if none
;--- came.  For the conversation above, where the mouse is switched off
;--- and the byte can only be the controller's.  Interrupts off.
ms_kbcany:
        push    ecx
        mov     ecx,200000
mky_lp:
        in      al,PORT_KBCMD
        test    al,KBS_OBF
        jnz     mky_have
        dec     ecx
        jnz     mky_lp
        pop     ecx
        stc
        ret
mky_have:
        in      al,PORT_KBDATA
        pop     ecx
        clc
        ret

;--- ms_kbcwait - until the controller will take a byte.  Gives up
;--- quietly: a controller that never gets ready is answered by the
;--- reply not coming, which every caller already handles.
ms_kbcwait:
        push    eax
        push    ecx
        mov     ecx,100000
mkw_lp:
        in      al,PORT_KBCMD
        test    al,KBS_IBF
        jz      mkw_out
        dec     ecx
        jnz     mkw_lp
mkw_out:
        pop     ecx
        pop     eax
        ret

;--- ms_kbccmd - AL = a command for the controller itself
ms_kbccmd:
        call    ms_kbcwait
        out     PORT_KBCMD,al
        ret

;--- ms_kbcreply - the controller's answer -> AL, CF if none.  Called
;--- with interrupts off.  A mouse byte that was already on its way is
;--- read and dropped rather than taken for the answer.
ms_kbcreply:
        push    ecx
        mov     ecx,200000
mkr_lp:
        in      al,PORT_KBCMD
        test    al,KBS_OBF
        jz      mkr_next
        test    al,KBS_AUXB
        jz      mkr_have
        in      al,PORT_KBDATA
mkr_next:
        dec     ecx
        jnz     mkr_lp
        pop     ecx
        stc
        ret
mkr_have:
        in      al,PORT_KBDATA
        pop     ecx
        clc
        ret

;--- ms_auxsend - AL = a byte for the mouse: D4h first, or the
;--- controller gives it to the keyboard.
ms_auxsend:
        push    eax
        mov     al,KBC_WRAUX
        call    ms_kbccmd
        call    ms_kbcwait
        pop     eax
        out     PORT_KBDATA,al
        ret

;--- ms_auxread - up to ECX ticks for a byte FROM THE MOUSE -> AL.  CF
;--- if none came.  Interrupts are turned on for the wait, whatever the
;--- caller had: the clock is what measures it.
ms_auxread:
        pushfd
        push    ebx
        push    edx
        sti
        mov     ebx,dword ptr ds:[BDA_TICKS]
mar_lp:
        in      al,PORT_KBCMD
        and     al,KBS_OBF OR KBS_AUXB
        cmp     al,KBS_OBF OR KBS_AUXB
        je      mar_have
        mov     edx,dword ptr ds:[BDA_TICKS]
        sub     edx,ebx
        cmp     edx,ecx
        jbe     mar_lp
        pop     edx
        pop     ebx
        popfd
        stc
        ret
mar_have:
        in      al,PORT_KBDATA
        pop     edx
        pop     ebx
        popfd
        clc
        ret

;--- ms_auxcmd - AL = a command for the mouse, and its FAh.  Bytes that
;--- come first are a packet already on its way, and are dropped; FEh
;--- asks for the command again, once.  CF if it never acknowledged.
ms_auxcmd:
        push    ecx
        push    edx
        mov     byte ptr [ms_gotrx],0
        mov     dl,al
        mov     dh,2
mac_send:
        mov     al,dl
        call    ms_auxsend
        mov     ecx,8
mac_read:
        push    ecx
        mov     ecx,MS_ACKTICKS
        call    ms_auxread
        pop     ecx
        jc      mac_fail
        mov     byte ptr [ms_lastrx],al
        mov     byte ptr [ms_gotrx],1
        cmp     al,0FAh
        je      mac_ok
        cmp     al,0FEh
        je      mac_again
        loop    mac_read
        jmp     short mac_fail
mac_again:
        dec     dh
        jnz     mac_send
mac_fail:
        pop     edx
        pop     ecx
        stc
        ret
mac_ok:
        pop     edx
        pop     ecx
        clc
        ret

;--- ms_auxdrain - throw away whatever the mouse has already said
ms_auxdrain:
        push    eax
        push    ecx
        mov     ecx,32
mad_lp:
        in      al,PORT_KBCMD
        and     al,KBS_OBF OR KBS_AUXB
        cmp     al,KBS_OBF OR KBS_AUXB
        jne     mad_out
        in      al,PORT_KBDATA
        loop    mad_lp
mad_out:
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; irq_mouse - IRQ 12.  One byte, if it is the mouse's.
;
; The status is checked first because the byte may already be gone:
; the timer tick takes a mouse byte it finds waiting (see irq_timer),
; and when both interrupts are pending the timer is served first.
;---------------------------------------------------------------------
irq_mouse:
        cmp     byte ptr [ms_probing],0
        jne     imo_out
        in      al,PORT_KBCMD
        and     al,KBS_OBF OR KBS_AUXB
        cmp     al,KBS_OBF OR KBS_AUXB
        jne     imo_out
        in      al,PORT_KBDATA
        call    ms_ps2byte
imo_out:
        ret

;---------------------------------------------------------------------
; ms_ps2byte - AL = one byte of a PS/2 packet.  All registers kept.
;
; Three bytes: buttons and signs, X, Y.  Bit 3 of the first is always
; set, and it is the only way to find the start of a packet after a
; byte has been lost; a pause of a tick or more starts a new one too,
; because a mouse sends a packet's three bytes together.
;---------------------------------------------------------------------
ms_ps2byte:
        inc     dword ptr [ms_bytesin]
        pushad
        mov     edx,dword ptr ds:[BDA_TICKS]
        mov     ebx,edx
        sub     edx,dword ptr [ms_lasttick]
        mov     dword ptr [ms_lasttick],ebx
        cmp     edx,2
        jb      mpb_keep
        mov     byte ptr [ms_pktn],0
mpb_keep:
        movzx   ebx,byte ptr [ms_pktn]
        or      ebx,ebx
        jnz     mpb_store
        test    al,08h
        jz      mpb_out                 ; not a first byte: wait for one
mpb_store:
        mov     byte ptr [ms_pkt+ebx],al
        inc     ebx
        mov     byte ptr [ms_pktn],bl
        cmp     ebx,3
        jb      mpb_out
        mov     byte ptr [ms_pktn],0

        movzx   ebx,byte ptr [ms_pkt]
        movzx   ecx,byte ptr [ms_pkt+1]
        movzx   edx,byte ptr [ms_pkt+2]
        test    bl,10h                  ; X's sign
        jz      mpb_xpos
        or      ecx,0FFFFFF00h
mpb_xpos:
        test    bl,20h                  ; Y's sign
        jz      mpb_ypos
        or      edx,0FFFFFF00h
mpb_ypos:
        neg     edx                     ; the mouse counts up the desk, the
                                        ; screen counts down it
        test    bl,0C0h                 ; an overflow: the counts are junk,
        jz      mpb_move                ; the buttons are not
        xor     ecx,ecx
        xor     edx,edx
mpb_move:
        and     ebx,7                   ; left, right, middle - which is
        call    ms_motion               ; Microsoft's order already
mpb_out:
        popad
        ret

;=====================================================================
; THE SERIAL MOUSE - Microsoft's protocol: 1200 baud, seven data bits,
; three bytes a packet with bit 6 marking the first.  A Logitech mouse
; adds a fourth byte, bit 5 its middle button, when that button moves.
;=====================================================================

;---------------------------------------------------------------------
; ms_serfind - COM1, then COM2, as the BIOS found them.  CF if neither
;              has a mouse on it; else ms_port and ms_irq are set.
;---------------------------------------------------------------------
ms_serfind:
        pushad
        mov     esi,BDA_COM1
        mov     ecx,2
sfd_port:
        movzx   edx,word ptr ds:[esi]
        or      edx,edx
        jz      sfd_next
        call    ms_sertry
        jnc     sfd_have
sfd_next:
        add     esi,2
        loop    sfd_port
        popad
        stc
        ret
sfd_have:
        mov     dword ptr [ms_port],edx
        mov     eax,esi                 ; which of the BIOS's ports it
        sub     eax,BDA_COM1            ; was, for ms_announce: COM1 is
        shr     eax,1                   ; the first word there
        add     al,'1'
        mov     byte ptr [msgMsComN],al
        ; 3F8h and 3E8h are IRQ 4, 2F8h and 2E8h IRQ 3: the two differ
        ; in bit 8 and nowhere else that matters
        mov     byte ptr [ms_irq],4
        test    edx,100h
        jnz     sfd_irq
        mov     byte ptr [ms_irq],3
sfd_irq:
        popad
        clc
        ret

;---------------------------------------------------------------------
; ms_sertry - EDX = a UART.  Set it up for a mouse, cycle the mouse's
;             power (it runs off DTR and RTS), and listen for the 'M'
;             it says on waking - with a '3' after it on a three-button
;             Logitech.  CF if nothing answered.
;---------------------------------------------------------------------
ms_sertry:
        pushad
        mov     ebx,edx
        lea     edx,[ebx+3]
        mov     al,80h                  ; DLAB: the divisor follows
        out     dx,al
        mov     edx,ebx
        mov     al,96                   ; 115200 / 1200
        out     dx,al
        lea     edx,[ebx+1]
        xor     al,al
        out     dx,al
        lea     edx,[ebx+3]
        mov     al,02h                  ; seven bits, no parity, one stop
        out     dx,al
        in      al,dx
        cmp     al,02h                  ; nothing reads back: no UART
        jne     mst_no
        lea     edx,[ebx+1]
        xor     al,al
        out     dx,al                   ; no interrupts yet
        lea     edx,[ebx+2]
        out     dx,al                   ; and no FIFO
        lea     edx,[ebx+4]
        out     dx,al                   ; DTR and RTS down: the mouse off...
        mov     ecx,2
        call    ms_ticks
        call    ms_serdrain
        lea     edx,[ebx+4]
        mov     al,0Bh                  ; ...and on again, OUT2 with them
        out     dx,al

        pushfd
        sti
        xor     ecx,ecx                 ; CL: an 'M' came, CH: then a '3'
        mov     edi,6                   ; a third of a second to say it
        mov     esi,dword ptr ds:[BDA_TICKS]
mst_poll:
        lea     edx,[ebx+5]
        in      al,dx
        test    al,1
        jz      mst_idle
        mov     edx,ebx
        in      al,dx
        and     al,7Fh
        cmp     al,'M'
        jne     mst_not_m
        mov     cl,1
        mov     edi,2                   ; and a little longer for a '3'
        mov     esi,dword ptr ds:[BDA_TICKS]
        jmp     short mst_poll
mst_not_m:
        cmp     al,'3'
        jne     mst_poll
        or      cl,cl
        jz      mst_poll
        mov     ch,1
        jmp     short mst_heard
mst_idle:
        mov     eax,dword ptr ds:[BDA_TICKS]
        sub     eax,esi
        cmp     eax,edi
        jb      mst_poll
mst_heard:
        popfd
        or      cl,cl
        jz      mst_no
        mov     byte ptr [ms_nbtn],2
        or      ch,ch
        jz      mst_two
        mov     byte ptr [ms_nbtn],3
mst_two:
        mov     byte ptr [ms_pktn],0
        popad
        clc
        ret
mst_no:
        popad
        stc
        ret

;--- ms_serdrain - EBX = the UART: empty its receiver
ms_serdrain:
        push    eax
        push    ecx
        push    edx
        mov     ecx,16
msd_lp:
        lea     edx,[ebx+5]
        in      al,dx
        test    al,1
        jz      msd_out
        mov     edx,ebx
        in      al,dx
        loop    msd_lp
msd_out:
        pop     edx
        pop     ecx
        pop     eax
        ret

;--- ms_ticks - wait ECX clock ticks, with interrupts on for it
ms_ticks:
        pushfd
        push    eax
        push    ebx
        sti
        mov     ebx,dword ptr ds:[BDA_TICKS]
mtk_lp:
        mov     eax,dword ptr ds:[BDA_TICKS]
        sub     eax,ebx
        cmp     eax,ecx
        jb      mtk_lp
        pop     ebx
        pop     eax
        popfd
        ret

;---------------------------------------------------------------------
; irq_serial - IRQ 3 or 4: every byte the UART is holding.
;---------------------------------------------------------------------
irq_serial:
        mov     ebx,dword ptr [ms_port]
        or      ebx,ebx
        jz      isr_out
        cmp     byte ptr [ms_probing],0
        jne     isr_out
        mov     ecx,16
isr_lp:
        lea     edx,[ebx+5]
        in      al,dx
        test    al,1
        jz      isr_out
        mov     edx,ebx
        in      al,dx
        call    ms_serbyte
        loop    isr_lp
isr_out:
        ret

;---------------------------------------------------------------------
; ms_serbyte - AL = a byte from a serial mouse.  All registers kept.
;---------------------------------------------------------------------
ms_serbyte:
        pushad
        and     al,7Fh
        test    al,40h
        jz      msb_data
        mov     byte ptr [ms_pkt],al    ; bit 6: a packet starts
        mov     byte ptr [ms_pktn],1
        jmp     msb_out
msb_data:
        movzx   ebx,byte ptr [ms_pktn]
        or      ebx,ebx
        jz      msb_extra
        mov     byte ptr [ms_pkt+ebx],al
        inc     ebx
        mov     byte ptr [ms_pktn],bl
        cmp     ebx,3
        jb      msb_out
        mov     byte ptr [ms_pktn],0

        movzx   esi,byte ptr [ms_pkt]
        mov     eax,esi                 ; X: bits 1-0 of the first byte are
        and     eax,03h                 ; its top two
        shl     eax,6
        mov     dl,byte ptr [ms_pkt+1]
        and     dl,3Fh
        or      al,dl
        movsx   ecx,al
        mov     eax,esi                 ; Y: bits 3-2
        and     eax,0Ch
        shl     eax,4
        mov     dl,byte ptr [ms_pkt+2]
        and     dl,3Fh
        or      al,dl
        movsx   edx,al                  ; serial Y already counts downward
        xor     ebx,ebx
        test    esi,20h                 ; bit 5 left, bit 4 right
        jz      msb_nol
        or      bl,1
msb_nol:
        test    esi,10h
        jz      msb_nor
        or      bl,2
msb_nor:
        mov     al,byte ptr [ms_btn]    ; the middle button is the fourth
        and     al,4                    ; byte's business, kept as it was
        or      bl,al
        call    ms_motion
        jmp     short msb_out
msb_extra:
        ; A byte with bit 6 clear and no packet open: a Logitech mouse's
        ; middle button, in bit 5.
        cmp     byte ptr [ms_nbtn],3
        jne     msb_out
        mov     bl,byte ptr [ms_btn]
        and     bl,3
        test    al,20h
        jz      msb_nomid
        or      bl,4
msb_nomid:
        xor     ecx,ecx
        xor     edx,edx
        call    ms_motion
msb_out:
        popad
        ret

;=====================================================================
; ms_motion - the mouse moved ECX mickeys across and EDX down, and its
;             buttons are BL now.  Interrupts off; registers not kept.
;
; Microsoft's arithmetic: a pixel is (mkx / 8) mickeys across and
; (mky / 8) down, scaled by 1Ah's speeds (50 is one to one), doubled
; while the mouse moves faster than 13h's threshold.  The remainders
; are carried, so slow movement adds up rather than being lost.  Speed
; is measured over the clock tick: the mickeys moved in the current
; 55ms, times eighteen.
;=====================================================================
ms_motion:
        cmp     byte ptr [ms_off],0
        jne     mmo_out
        mov     eax,dword ptr [ms_x]    ; X and Y together, to see if they
        mov     dword ptr [ms_oldpos],eax       ; change
        xor     ebp,ebp                 ; the conditions this adds up to
        mov     eax,ecx
        or      eax,edx
        jz      mmo_buttons

        add     word ptr [ms_cntx],cx
        add     word ptr [ms_cnty],dx
        add     word ptr [ms_rawx],cx
        add     word ptr [ms_rawy],dx
        or      ebp,1

        ; how fast
        mov     eax,dword ptr ds:[BDA_TICKS]
        cmp     eax,dword ptr [ms_spdtick]
        je      mmo_sametick
        mov     dword ptr [ms_spdtick],eax
        mov     dword ptr [ms_spdacc],0
mmo_sametick:
        mov     eax,ecx
        or      eax,eax
        jns     mmo_ax
        neg     eax
mmo_ax:
        add     dword ptr [ms_spdacc],eax
        mov     eax,edx
        or      eax,eax
        jns     mmo_ay
        neg     eax
mmo_ay:
        add     eax,dword ptr [ms_spdacc]
        mov     dword ptr [ms_spdacc],eax
        imul    eax,eax,18
        movzx   esi,word ptr [ms_dspeed]
        cmp     eax,esi
        jb      mmo_single
        sal     ecx,1
        sal     edx,1
mmo_single:
        push    edx
        mov     eax,ecx
        movzx   esi,word ptr [ms_sensx]
        call    ms_scale                ; EAX mickeys * 8 * speed
        add     eax,dword ptr [ms_accx]
        movzx   esi,word ptr [ms_mkx]
        imul    esi,esi,50
        cdq
        idiv    esi
        mov     dword ptr [ms_accx],edx
        mov     esi,OFFSET ms_x
        mov     edi,OFFSET ms_xmin
        call    ms_step
        pop     edx

        mov     eax,edx
        movzx   esi,word ptr [ms_sensy]
        call    ms_scale
        add     eax,dword ptr [ms_accy]
        movzx   esi,word ptr [ms_mky]
        imul    esi,esi,50
        cdq
        idiv    esi
        mov     dword ptr [ms_accy],edx
        mov     esi,OFFSET ms_y
        mov     edi,OFFSET ms_ymin
        call    ms_step

mmo_buttons:
        movzx   eax,bl
        and     eax,7
        movzx   esi,word ptr [ms_btn]
        xor     esi,eax                 ; the buttons that changed
        jz      mmo_nobtn
        mov     word ptr [ms_btn],ax
        xor     ecx,ecx                 ; the button
        mov     edx,2                   ; its "pressed" condition bit
mmo_one:
        bt      esi,ecx
        jnc     mmo_next
        bt      eax,ecx
        jnc     mmo_up
        inc     word ptr [ms_pcnt+ecx*2]
        mov     bx,word ptr [ms_x]
        mov     word ptr [ms_px+ecx*2],bx
        mov     bx,word ptr [ms_y]
        mov     word ptr [ms_py+ecx*2],bx
        or      ebp,edx
        jmp     short mmo_next
mmo_up:
        inc     word ptr [ms_rcnt+ecx*2]
        mov     bx,word ptr [ms_x]
        mov     word ptr [ms_rx+ecx*2],bx
        mov     bx,word ptr [ms_y]
        mov     word ptr [ms_ry+ecx*2],bx
        lea     ebx,[edx*2]             ; "released" is the next bit up
        or      ebp,ebx
mmo_next:
        shl     edx,2
        inc     ecx
        cmp     ecx,3
        jb      mmo_one
mmo_nobtn:
        or      ebp,ebp
        jz      mmo_out
        or      word ptr [ms_pend],bp
        mov     byte ptr [ms_wake],1
        mov     eax,dword ptr [ms_oldpos]
        cmp     eax,dword ptr [ms_x]
        je      mmo_out                 ; the pointer stayed where it was
        call    ms_undraw
        call    ms_draw
mmo_out:
        ret

;--- ms_scale - EAX = mickeys, ESI = a 1Ah speed -> EAX = mickeys * 8
;--- * speed, the numerator ms_motion divides by (ratio * 50).  A speed
;--- of zero would stop the pointer dead; it is taken as one.
ms_scale:
        or      esi,esi
        jnz     msc_go
        inc     esi
msc_go:
        cmp     esi,100
        jbe     msc_ok
        mov     esi,100
msc_ok:
        imul    eax,esi
        shl     eax,3
        ret

;--- ms_step - ESI -> ms_x or ms_y, EDI -> its range (the minimum, then
;--- the maximum), EAX = pixels to move it.  In 32 bits and clamped
;--- before it is stored: a count past what a word holds would otherwise
;--- wrap round to the other side of the screen.
ms_step:
        push    ebx
        movsx   ebx,word ptr [esi]
        add     eax,ebx
        movsx   ebx,word ptr [edi]
        cmp     eax,ebx
        jge     mse_lo
        mov     eax,ebx
mse_lo:
        movsx   ebx,word ptr [edi+2]
        cmp     eax,ebx
        jle     mse_hi
        mov     eax,ebx
mse_hi:
        mov     word ptr [esi],ax
        pop     ebx
        ret

;--- ms_clamp - the pointer back inside 07h's and 08h's ranges
ms_clamp:
        push    eax
        mov     ax,word ptr [ms_x]
        cmp     ax,word ptr [ms_xmin]
        jge     mcl_xlo
        mov     ax,word ptr [ms_xmin]
mcl_xlo:
        cmp     ax,word ptr [ms_xmax]
        jle     mcl_xhi
        mov     ax,word ptr [ms_xmax]
mcl_xhi:
        mov     word ptr [ms_x],ax
        mov     ax,word ptr [ms_y]
        cmp     ax,word ptr [ms_ymin]
        jge     mcl_ylo
        mov     ax,word ptr [ms_ymin]
mcl_ylo:
        cmp     ax,word ptr [ms_ymax]
        jle     mcl_yhi
        mov     ax,word ptr [ms_ymax]
mcl_yhi:
        mov     word ptr [ms_y],ax
        pop     eax
        ret

;=====================================================================
; THE CURSOR.
;
; ms_draw puts it on the screen if it should be there and is not;
; ms_undraw takes it off.  Both keep every register and both expect
; interrupts off.  Everything else goes through those two.
;
; The screen is looked at afresh on every draw, because a DOS program
; may change the video mode without calling the driver - a real one
; hooked INT 10h to notice, and this one asks the BIOS data area
; instead.  A cursor drawn in one mode is never "taken off" in another:
; the mode set cleared the screen, so what was saved is thrown away.
;=====================================================================
ms_draw:
        pushad
        cmp     word ptr [ms_vis],0
        jne     mdw_out
        cmp     dword ptr [ms_excl],0
        jne     mdw_out
        cmp     byte ptr [ms_off],0
        jne     mdw_out
        cmp     byte ptr [ms_drawn],0
        jne     mdw_out
        call    ms_geom

        ; 10h's region: the first time the pointer is inside it, the
        ; cursor goes off, and it stays off until 01h - which is a hide
        ; the program did not have to make itself.
        cmp     word ptr [ms_condon],0
        je      mdw_nocond
        mov     ax,word ptr [ms_x]
        cmp     ax,word ptr [ms_cx1]
        jl      mdw_nocond
        cmp     ax,word ptr [ms_cx2]
        jg      mdw_nocond
        mov     ax,word ptr [ms_y]
        cmp     ax,word ptr [ms_cy1]
        jl      mdw_nocond
        cmp     ax,word ptr [ms_cy2]
        jg      mdw_nocond
        mov     word ptr [ms_condon],0
        dec     word ptr [ms_vis]
        jmp     short mdw_out
mdw_nocond:
        mov     al,byte ptr [ms_gtype]
        cmp     al,MGT_TEXT
        je      mdw_text
        cmp     al,MGT_NONE
        je      mdw_out
        call    ms_gpos
        xor     al,al
        call    ms_gwalk
        mov     byte ptr [ms_drawn],MDR_GFX
        jmp     short mdw_out
mdw_text:
        call    ms_cell
        jc      mdw_out
        cmp     word ptr [ms_ctype],1
        je      mdw_hw
        mov     ax,word ptr [edi]
        mov     word ptr [ms_dsave],ax
        and     ax,word ptr [ms_tscr]
        xor     ax,word ptr [ms_tcur]
        mov     word ptr [edi],ax
        mov     word ptr [ms_dval],ax
        mov     dword ptr [ms_daddr],edi
        mov     byte ptr [ms_drawn],MDR_TEXT
        jmp     short mdw_out
mdw_hw:
        call    ms_hwcur
mdw_out:
        popad
        ret

ms_undraw:
        pushad
        movzx   eax,byte ptr [ms_drawn]
        or      eax,eax
        jz      mud_out
        mov     byte ptr [ms_drawn],0
        mov     bl,byte ptr ds:[BDA_VMODE]
        and     bl,7Fh
        cmp     bl,byte ptr [ms_gmode]
        jne     mud_out                 ; a new mode: nothing to put back
        cmp     al,MDR_TEXT
        je      mud_text
        cmp     al,MDR_HW
        je      mud_hw
        mov     al,1
        call    ms_gwalk
        jmp     short mud_out
mud_text:
        ; Only if the cell still holds what we put there.  A program
        ; that wrote over the cursor has the cell it wanted; putting
        ; back what was under the cursor would undo its write.
        mov     edi,dword ptr [ms_daddr]
        mov     ax,word ptr [edi]
        cmp     ax,word ptr [ms_dval]
        jne     mud_out
        mov     ax,word ptr [ms_dsave]
        mov     word ptr [edi],ax
        jmp     short mud_out
mud_hw:
        call    ms_hwback
mud_out:
        popad
        ret

;---------------------------------------------------------------------
; ms_conhide / ms_conshow - the console is about to write the screen,
;                           and has finished.  CONSOLE.INC brackets
;                           every routine that touches video memory,
;                           so a native program drawing with 21h/F0h
;                           never draws over the cursor or reads it
;                           back as a character.
;---------------------------------------------------------------------
ms_conhide:
        pushfd
        cli
        inc     dword ptr [ms_excl]
        call    ms_undraw
        popfd
        ret

ms_conshow:
        pushfd
        cli
        dec     dword ptr [ms_excl]
        jnz     mcs_out
        call    ms_draw
mcs_out:
        popfd
        ret

;---------------------------------------------------------------------
; ms_geom - work out the screen from the BIOS data area: its type, how
;           big it is in virtual pixels, where the page 1Dh chose is.
;---------------------------------------------------------------------
ms_geom:
        pushad
        mov     al,byte ptr ds:[BDA_VMODE]
        and     al,7Fh
        mov     byte ptr [ms_gmode],al
        movzx   ebx,word ptr [ms_page]
        and     ebx,7
        movzx   ecx,word ptr ds:[BDA_VPGSZ]
        imul    ebx,ecx
        mov     dword ptr [ms_gpoff],ebx
        mov     byte ptr [ms_xshift],0
        mov     dword ptr [ms_vxmax],639
        cmp     al,3
        jbe     mge_text
        cmp     al,7
        je      mge_text

        mov     esi,OFFSET ms_gmodes
mge_find:
        cmp     byte ptr [esi],0FFh
        je      mge_none
        cmp     al,byte ptr [esi]
        je      mge_found
        add     esi,MSGM_LEN
        jmp     short mge_find
mge_found:
        mov     al,byte ptr [esi+1]
        mov     byte ptr [ms_gtype],al
        mov     al,byte ptr [esi+2]
        mov     byte ptr [ms_xshift],al
        movzx   eax,word ptr [esi+4]
        mov     dword ptr [ms_gbpr],eax
        movzx   eax,word ptr [esi+6]
        mov     dword ptr [ms_gheight],eax
        dec     eax
        mov     dword ptr [ms_vymax],eax
        mov     eax,dword ptr [esi+8]
        add     eax,ebx
        mov     dword ptr [ms_gpbase],eax
        jmp     mge_out
mge_none:
        mov     byte ptr [ms_gtype],MGT_NONE
        mov     dword ptr [ms_vymax],199
        jmp     mge_out

mge_text:
        mov     byte ptr [ms_gtype],MGT_TEXT
        movzx   ecx,word ptr ds:[BDA_VCOLS]
        or      ecx,ecx
        jz      mge_defc
        cmp     ecx,SCR_MAXCOLS
        jbe     mge_cols
mge_defc:
        mov     ecx,SCR_COLS
mge_cols:
        mov     dword ptr [ms_cols],ecx
        movzx   edx,byte ptr ds:[BDA_VROWS]
        inc     edx
        cmp     edx,2
        jb      mge_defr
        cmp     edx,SCR_MAXROWS
        jbe     mge_rows
mge_defr:
        mov     edx,SCR_ROWS
mge_rows:
        mov     dword ptr [ms_rows],edx
        ; Eight virtual pixels a cell, sixteen across on a 40-column
        ; screen: the virtual screen is 640 wide either way
        mov     byte ptr [ms_cshift],3
        cmp     ecx,40
        ja      mge_shift
        mov     byte ptr [ms_cshift],4
mge_shift:
        push    ecx
        mov     cl,byte ptr [ms_cshift]
        pop     eax                     ; the columns
        shl     eax,cl
        dec     eax
        mov     dword ptr [ms_vxmax],eax
        lea     eax,[edx*8-1]
        mov     dword ptr [ms_vymax],eax
        mov     eax,VGA_TEXT
        mov     edx,CRTC_IDX
        cmp     byte ptr [ms_gmode],7
        jne     mge_colour
        mov     eax,VGA_MONO
        mov     edx,3B4h
mge_colour:
        mov     dword ptr [ms_crtc],edx
        add     eax,ebx
        mov     dword ptr [ms_gpbase],eax
mge_out:
        popad
        ret

;---------------------------------------------------------------------
; ms_cell - the text cell under the pointer: EDI = its address, and
;           ms_dcell = its number.  CF if it is off the screen.
;---------------------------------------------------------------------
ms_cell:
        movsx   eax,word ptr [ms_y]
        sar     eax,3
        js      mce_no
        cmp     eax,dword ptr [ms_rows]
        jae     mce_no
        movsx   edx,word ptr [ms_x]
        mov     cl,byte ptr [ms_cshift]
        sar     edx,cl
        js      mce_no
        cmp     edx,dword ptr [ms_cols]
        jae     mce_no
        imul    eax,dword ptr [ms_cols]
        add     eax,edx
        mov     dword ptr [ms_dcell],eax
        lea     edi,[eax*2]
        add     edi,dword ptr [ms_gpbase]
        clc
        ret
mce_no:
        stc
        ret

;---------------------------------------------------------------------
; ms_hwcur / ms_hwback - 0Ah's HARDWARE cursor: the CRT controller's
;                        own cursor moved to the pointer and given 0Ah's
;                        scan lines, and everything it was put back.
;---------------------------------------------------------------------
ms_hwcur:
        mov     edx,dword ptr [ms_crtc]
        in      al,dx
        mov     byte ptr [ms_hwidx],al
        mov     esi,OFFSET ms_hwsave
        mov     al,0Ah
        call    ms_crtcget
        mov     al,0Bh
        call    ms_crtcget
        mov     al,0Eh
        call    ms_crtcget
        mov     al,0Fh
        call    ms_crtcget
        mov     ebx,dword ptr [ms_gpoff]
        shr     ebx,1
        add     ebx,dword ptr [ms_dcell]        ; the adapter counts cells
        mov     al,0Eh                          ; from its start, not the
        mov     ah,bh                           ; page's
        out     dx,ax
        mov     al,0Fh
        mov     ah,bl
        out     dx,ax
        mov     al,0Ah
        mov     ah,byte ptr [ms_tscr]
        out     dx,ax
        mov     al,0Bh
        mov     ah,byte ptr [ms_tcur]
        out     dx,ax
        mov     al,byte ptr [ms_hwidx]
        out     dx,al
        mov     byte ptr [ms_drawn],MDR_HW
        ret

; AL = a CRTC register -> its value at [ESI], and ESI moves on
ms_crtcget:
        out     dx,al
        inc     edx
        in      al,dx
        dec     edx
        mov     byte ptr [esi],al
        inc     esi
        ret

ms_hwback:
        mov     edx,dword ptr [ms_crtc]
        mov     al,0Ah
        mov     ah,byte ptr [ms_hwsave]
        out     dx,ax
        mov     al,0Bh
        mov     ah,byte ptr [ms_hwsave+1]
        out     dx,ax
        mov     al,0Eh
        mov     ah,byte ptr [ms_hwsave+2]
        out     dx,ax
        mov     al,0Fh
        mov     ah,byte ptr [ms_hwsave+3]
        out     dx,ax
        mov     al,byte ptr [ms_hwidx]
        out     dx,al
        ret

;---------------------------------------------------------------------
; ms_gpos - where the graphics cursor's top left goes: the pointer, in
;           real pixels, less the hot spot.  Either can be off the
;           screen, and the walk clips.
;---------------------------------------------------------------------
ms_gpos:
        movsx   eax,word ptr [ms_x]
        mov     cl,byte ptr [ms_xshift]
        sar     eax,cl
        movsx   ecx,word ptr [ms_hotx]
        sub     eax,ecx
        mov     dword ptr [ms_gx],eax
        movsx   eax,word ptr [ms_y]
        movsx   ecx,word ptr [ms_hoty]
        sub     eax,ecx
        mov     dword ptr [ms_gy],eax
        ret

;---------------------------------------------------------------------
; ms_gwalk - put the graphics cursor on the screen (AL = 0) or take it
;            off (AL = 1).  Both walk the same bytes in the same order,
;            which is what lets the save area be a plain list.
;
; For the planar modes the VGA's registers are the program's, and the
; cursor can arrive in the middle of it programming them - so what is
; changed is read first and put back after, indexes included.  An EGA
; cannot be read back, which is why Microsoft's driver needed the EGA
; Register Interface; a machine this kernel runs on has a VGA.
;---------------------------------------------------------------------
ms_gwalk:
        pushad
        mov     byte ptr [ms_gundo],al
        mov     dword ptr [ms_gidx],0
        mov     al,byte ptr [ms_gtype]
        cmp     al,MGT_256
        je      mgw_256
        cmp     al,MGT_PLANAR
        jne     mgw_flat
        call    ms_vgasave
        mov     dword ptr [ms_gplane],0
mgw_plane:
        mov     edx,3CEh                ; read this plane...
        mov     al,4
        mov     ah,byte ptr [ms_gplane]
        out     dx,ax
        mov     edx,3C4h                ; ...and write only it
        mov     cl,byte ptr [ms_gplane]
        mov     ah,1
        shl     ah,cl
        mov     al,2
        out     dx,ax
        call    ms_gbits
        inc     dword ptr [ms_gplane]
        cmp     dword ptr [ms_gplane],4
        jb      mgw_plane
        call    ms_vgarest
        jmp     short mgw_out
mgw_flat:
        call    ms_gbits
        jmp     short mgw_out
mgw_256:
        call    ms_g256
mgw_out:
        popad
        ret

;---------------------------------------------------------------------
; ms_gbits - one bit-mapped plane: a planar mode's, mode 6's, or the
;            CGA's four-colour one, where Microsoft's masks go onto the
;            screen two bits to a pixel as they are - eight pixels wide.
;
; Each cursor row is a 16-bit mask that starts at any bit of a byte, so
; it is laid out as 24 bits over three bytes: the first byte's in bits
; 23-16.  The bits it does not cover keep what is there.
;---------------------------------------------------------------------
ms_gbits:
        mov     eax,dword ptr [ms_gx]
        cmp     byte ptr [ms_gtype],MGT_CGA4
        jne     mgb_1bpp
        shl     eax,1
mgb_1bpp:
        mov     ecx,eax
        and     ecx,7
        mov     byte ptr [ms_gs],cl
        sar     eax,3
        mov     dword ptr [ms_gbx],eax
        mov     dword ptr [ms_grow],0
mgb_row:
        mov     eax,dword ptr [ms_gy]
        add     eax,dword ptr [ms_grow]
        js      mgb_nextrow
        cmp     eax,dword ptr [ms_gheight]
        jae     mgb_nextrow
        cmp     byte ptr [ms_gtype],MGT_PLANAR
        je      mgb_linear
        mov     edi,eax                 ; the CGA's: even rows from 0,
        and     edi,1                   ; odd ones from 2000h
        shl     edi,13
        shr     eax,1
        imul    eax,eax,80
        add     edi,eax
        jmp     short mgb_haverow
mgb_linear:
        mov     edi,eax
        imul    edi,dword ptr [ms_gbpr]
mgb_haverow:
        add     edi,dword ptr [ms_gpbase]

        mov     ebx,dword ptr [ms_grow]
        movzx   eax,word ptr [ms_gmask+ebx*2]           ; AND
        movzx   edx,word ptr [ms_gmask+32+ebx*2]        ; XOR
        mov     cl,8
        sub     cl,byte ptr [ms_gs]
        shl     eax,cl
        shl     edx,cl
        mov     esi,0FFFFh              ; the bits outside the cursor:
        shl     esi,cl                  ; AND 1, so they stay
        not     esi
        and     esi,0FFFFFFh
        or      eax,esi
        mov     dword ptr [ms_gand],eax
        mov     dword ptr [ms_gxor],edx

        mov     dword ptr [ms_gk],0
mgb_col:
        mov     esi,dword ptr [ms_gbx]
        add     esi,dword ptr [ms_gk]
        js      mgb_nextcol
        cmp     esi,dword ptr [ms_gbpr]
        jae     mgb_nextcol
        add     esi,edi
        mov     ecx,2                   ; this byte's share: the first is
        sub     ecx,dword ptr [ms_gk]   ; bits 23-16, the last 7-0
        shl     ecx,3
        mov     eax,dword ptr [ms_gand]
        shr     eax,cl
        mov     edx,dword ptr [ms_gxor]
        shr     edx,cl
        mov     ebx,dword ptr [ms_gidx]
        cmp     byte ptr [ms_gundo],0
        jne     mgb_back
        mov     ah,byte ptr [esi]
        mov     byte ptr [ms_gsave+ebx],ah
        and     ah,al
        xor     ah,dl
        mov     byte ptr [esi],ah
        jmp     short mgb_counted
mgb_back:
        mov     al,byte ptr [ms_gsave+ebx]
        mov     byte ptr [esi],al
mgb_counted:
        inc     dword ptr [ms_gidx]
mgb_nextcol:
        inc     dword ptr [ms_gk]
        cmp     dword ptr [ms_gk],3
        jb      mgb_col
mgb_nextrow:
        inc     dword ptr [ms_grow]
        cmp     dword ptr [ms_grow],16
        jb      mgb_row
        ret

;---------------------------------------------------------------------
; ms_g256 - mode 13h: a byte a pixel.  The screen mask keeps a pixel or
;           clears it and the cursor mask flips its low four bits, so
;           the default arrow is white (15) with a black border.
;---------------------------------------------------------------------
ms_g256:
        mov     dword ptr [ms_grow],0
mg2_row:
        mov     eax,dword ptr [ms_gy]
        add     eax,dword ptr [ms_grow]
        js      mg2_nextrow
        cmp     eax,dword ptr [ms_gheight]
        jae     mg2_nextrow
        imul    edi,eax,320
        add     edi,dword ptr [ms_gpbase]
        mov     ebx,dword ptr [ms_grow]
        movzx   eax,word ptr [ms_gmask+ebx*2]
        mov     dword ptr [ms_gand],eax
        movzx   eax,word ptr [ms_gmask+32+ebx*2]
        mov     dword ptr [ms_gxor],eax
        xor     ecx,ecx                 ; the column
mg2_col:
        mov     esi,dword ptr [ms_gx]
        add     esi,ecx
        js      mg2_nextcol
        cmp     esi,320
        jae     mg2_nextcol
        add     esi,edi
        mov     ebx,dword ptr [ms_gidx]
        inc     dword ptr [ms_gidx]
        cmp     byte ptr [ms_gundo],0
        jne     mg2_back
        mov     al,byte ptr [esi]
        mov     byte ptr [ms_gsave+ebx],al
        mov     edx,15
        sub     edx,ecx                 ; bit 15 is the left pixel
        bt      dword ptr [ms_gand],edx
        jc      mg2_keep
        xor     al,al
mg2_keep:
        bt      dword ptr [ms_gxor],edx
        jnc     mg2_put
        xor     al,0Fh
mg2_put:
        mov     byte ptr [esi],al
        jmp     short mg2_nextcol
mg2_back:
        mov     al,byte ptr [ms_gsave+ebx]
        mov     byte ptr [esi],al
mg2_nextcol:
        inc     ecx
        cmp     ecx,16
        jb      mg2_col
mg2_nextrow:
        inc     dword ptr [ms_grow]
        cmp     dword ptr [ms_grow],16
        jb      mg2_row
        ret

;--- ms_vgasave / ms_vgarest - the VGA state a planar walk changes:
;--- the sequencer's map mask, and the graphics controller's set/reset
;--- enable, rotate, read map, mode and bit mask.  The walk wants write
;--- mode 0 with nothing done to the data on its way in.
ms_vgasave:
        mov     edx,3C4h
        in      al,dx
        mov     byte ptr [ms_vsidx],al
        mov     al,2
        out     dx,al
        inc     edx
        in      al,dx
        mov     byte ptr [ms_vmap],al
        mov     edx,3CEh
        in      al,dx
        mov     byte ptr [ms_vgidx],al
        mov     esi,OFFSET ms_vgcregs
        mov     edi,OFFSET ms_vgcsave
        mov     ecx,5
mvs_one:
        mov     al,byte ptr [esi]
        out     dx,al
        inc     edx
        in      al,dx
        dec     edx
        mov     byte ptr [edi],al
        inc     esi
        inc     edi
        loop    mvs_one
        mov     ax,0001h                ; no set/reset
        out     dx,ax
        mov     ax,0003h                ; no rotate, no logic
        out     dx,ax
        mov     al,5                    ; read and write mode 0
        mov     ah,byte ptr [ms_vgcsave+3]
        and     ah,0F0h
        out     dx,ax
        mov     ax,0FF08h               ; every bit
        out     dx,ax
        ret

ms_vgarest:
        mov     edx,3CEh
        mov     esi,OFFSET ms_vgcregs
        mov     edi,OFFSET ms_vgcsave
        mov     ecx,5
mvr_one:
        mov     al,byte ptr [esi]
        mov     ah,byte ptr [edi]
        out     dx,ax
        inc     esi
        inc     edi
        loop    mvr_one
        mov     al,byte ptr [ms_vgidx]
        out     dx,al
        mov     edx,3C4h
        mov     al,2
        mov     ah,byte ptr [ms_vmap]
        out     dx,ax
        mov     al,byte ptr [ms_vsidx]
        out     dx,al
        ret

;=====================================================================
; ms_api - one INT 33h call, with the caller's registers in msr_* and
;          the answer going back the same way.  Interrupts off.
;
; The registers are kept whole - EAX to EDI as the caller had them -
; and each function writes what it returns as a whole register, so a
; native program reading ECX gets a number and not a number with its
; own high half left in it.  A box only gets the low words back.
;
; A function Microsoft's driver does not have, or that this one does
; not do, changes nothing: that is what every mouse driver does with a
; number it does not know.
;=====================================================================
ms_api:
        cld
        movzx   eax,word ptr [msr_eax]
        cmp     eax,4Dh
        je      msf_4D
        cmp     eax,6Dh
        je      msf_6D
        cmp     eax,MS_MAXFN
        jae     msf_nop
        jmp     dword ptr [ms_ftab+eax*4]
msf_nop:
        ret

ms_ftab:
        dd      msf_00, msf_01, msf_02, msf_03, msf_04, msf_05, msf_06, msf_07
        dd      msf_08, msf_09, msf_0A, msf_0B, msf_0C, msf_0D, msf_0E, msf_0F
        dd      msf_10, msf_nop, msf_nop, msf_13, msf_14, msf_15, msf_16, msf_17
        dd      msf_18, msf_19, msf_1A, msf_1B, msf_1C, msf_1D, msf_1E, msf_1F
        dd      msf_20, msf_21, msf_22, msf_23, msf_24, msf_25, msf_26, msf_27
        dd      msf_nop, msf_nop, msf_2A, msf_nop, msf_nop, msf_nop, msf_nop, msf_2F
        dd      msf_nop, msf_31, msf_32

;--- 00h: reset and status.  AX = FFFFh and BX = the buttons if there is
;--- a mouse, AX = 0 if there is not.
msf_00:
        call    ms_softreset
msf_status:
        cmp     byte ptr [ms_hw],MS_NONE
        je      m00_none
        mov     dword ptr [msr_eax],0FFFFh
        movzx   eax,byte ptr [ms_nbtn]
        mov     dword ptr [msr_ebx],eax
        ret
m00_none:
        mov     dword ptr [msr_eax],0
        mov     dword ptr [msr_ebx],0
        ret

;--- 21h: the same reset without the hardware - which was never touched
;--- by 00h here anyway.  AX = 21h says there is no mouse.
msf_21:
        call    ms_softreset
        cmp     byte ptr [ms_hw],MS_NONE
        jne     msf_status
        mov     dword ptr [msr_eax],21h
        ret

;---------------------------------------------------------------------
; ms_softreset - everything 00h promises: the cursor hidden, in the
;                middle of a screen measured afresh, the default masks
;                and ratios, no handlers, page 0.  The caller becomes
;                the program whose ending hides it again (ms_exit).
;---------------------------------------------------------------------
ms_softreset:
        call    ms_undraw
        mov     eax,dword ptr [msr_tok]
        mov     dword ptr [ms_rstown],eax
        mov     word ptr [ms_vis],-1
        mov     word ptr [ms_page],0
        call    ms_geom
        xor     eax,eax
        mov     word ptr [ms_xmin],ax
        mov     word ptr [ms_ymin],ax
        mov     eax,dword ptr [ms_vxmax]
        mov     word ptr [ms_xmax],ax
        inc     eax
        shr     eax,1
        mov     word ptr [ms_x],ax
        mov     eax,dword ptr [ms_vymax]
        mov     word ptr [ms_ymax],ax
        inc     eax
        shr     eax,1
        mov     word ptr [ms_y],ax
        xor     eax,eax
        mov     dword ptr [ms_accx],eax
        mov     dword ptr [ms_accy],eax
        mov     dword ptr [ms_cntx],eax         ; and ms_cnty
        mov     dword ptr [ms_rawx],eax         ; and ms_rawy
        mov     edi,OFFSET ms_pcnt
        mov     ecx,18                          ; the press and release
        rep     stosw                           ; records, all six arrays
        mov     word ptr [ms_mkx],8
        mov     word ptr [ms_mky],16
        mov     word ptr [ms_dspeed],64
        mov     word ptr [ms_ctype],0
        mov     word ptr [ms_tscr],77FFh
        mov     word ptr [ms_tcur],7700h
        mov     word ptr [ms_hotx],ax
        mov     word ptr [ms_hoty],ax
        mov     esi,OFFSET ms_defmask
        mov     edi,OFFSET ms_gmask
        mov     ecx,16
        rep     movsd
        mov     word ptr [ms_condon],ax
        mov     word ptr [ms_evmask],ax
        mov     dword ptr [ms_evhand],eax
        mov     dword ptr [ms_evown],eax
        mov     edi,OFFSET ms_altmask
        mov     ecx,3
        rep     stosw
        mov     edi,OFFSET ms_althand
        mov     ecx,6                           ; and ms_altown
        rep     stosd
        mov     word ptr [ms_pend],ax
        mov     word ptr [ms_lpen],1
        ret

;--- 01h: show the cursor.  The counter never goes above zero, and this
;--- also ends 10h's region.
msf_01:
        mov     word ptr [ms_condon],0
        cmp     word ptr [ms_vis],0
        jge     m01_out
        inc     word ptr [ms_vis]
        jnz     m01_out
        call    ms_draw
m01_out:
        ret

;--- 02h: hide it.  Calls pile up and each 01h takes one away.
msf_02:
        cmp     word ptr [ms_vis],0
        jne     m02_dec
        call    ms_undraw
m02_dec:
        cmp     word ptr [ms_vis],-32768
        je      m02_out
        dec     word ptr [ms_vis]
m02_out:
        ret

;--- 03h: BX = buttons, CX, DX = where
msf_03:
        movzx   eax,word ptr [ms_btn]
        mov     dword ptr [msr_ebx],eax
        movsx   eax,word ptr [ms_x]
        movsx   edx,word ptr [ms_y]
        call    ms_gran
        mov     dword ptr [msr_ecx],eax
        mov     dword ptr [msr_edx],edx
        ret

;--- ms_gran - EAX, EDX = a position -> as the mode can show it: a whole
;--- cell in text, an even X where the virtual X is twice the pixel.
ms_gran:
        cmp     byte ptr [ms_gtype],MGT_TEXT
        jne     mgr_gfx
        mov     cl,byte ptr [ms_cshift]
        sar     eax,cl
        shl     eax,cl
        and     edx,NOT 7
        ret
mgr_gfx:
        cmp     byte ptr [ms_xshift],0
        je      mgr_out
        and     eax,NOT 1
mgr_out:
        ret

;--- 04h: move the pointer to CX, DX
msf_04:
        mov     ax,word ptr [msr_ecx]
        mov     word ptr [ms_x],ax
        mov     ax,word ptr [msr_edx]
        mov     word ptr [ms_y],ax
ms_clampmove:
        call    ms_undraw
        call    ms_clamp
        jmp     ms_draw

;--- 05h, 06h: BX = a button -> AX = the buttons, BX = presses (or
;--- releases) since last asked, CX, DX = where the last one was
msf_05:
        mov     esi,OFFSET ms_pcnt
        jmp     short m056
msf_06:
        mov     esi,OFFSET ms_rcnt
m056:
        movzx   eax,word ptr [ms_btn]
        mov     dword ptr [msr_eax],eax
        movzx   ebx,word ptr [msr_ebx]
        cmp     ebx,2
        ja      m056_bad
        movzx   eax,word ptr [esi+ebx*2]
        mov     word ptr [esi+ebx*2],0
        mov     dword ptr [msr_ebx],eax
        movsx   eax,word ptr [esi+6+ebx*2]
        movsx   edx,word ptr [esi+12+ebx*2]
        call    ms_gran
        mov     dword ptr [msr_ecx],eax
        mov     dword ptr [msr_edx],edx
        ret
m056_bad:
        mov     dword ptr [msr_ebx],0
        ret

;--- 07h, 08h: the ranges, CX to DX, and the pointer pulled inside them
msf_07:
        mov     esi,OFFSET ms_xmin
        jmp     short m078
msf_08:
        mov     esi,OFFSET ms_ymin
m078:
        mov     ax,word ptr [msr_ecx]
        mov     dx,word ptr [msr_edx]
        cmp     ax,dx
        jle     m078_ok
        xchg    ax,dx
m078_ok:
        mov     word ptr [esi],ax       ; the min, and the max two words on
        mov     word ptr [esi+2],dx
        jmp     ms_clampmove

;--- 09h: the graphics cursor.  BX, CX = its hot spot, ES:DX -> sixteen
;--- words of screen mask and sixteen of cursor mask.
msf_09:
        cmp     dword ptr [msr_plen],64
        jb      m09_out
        call    ms_undraw
        mov     ax,word ptr [msr_ebx]
        mov     word ptr [ms_hotx],ax
        mov     ax,word ptr [msr_ecx]
        mov     word ptr [ms_hoty],ax
        mov     esi,dword ptr [msr_ptr]
        mov     edi,OFFSET ms_gmask
        mov     ecx,16
        rep     movsd
        call    ms_draw
m09_out:
        ret

;--- 0Ah: the text cursor.  BX = 0 software, CX = screen mask, DX =
;--- cursor mask; BX = 1 hardware, CX and DX its first and last lines.
msf_0A:
        call    ms_undraw
        movzx   eax,word ptr [msr_ebx]
        cmp     eax,1
        jbe     m0A_type
        xor     eax,eax
m0A_type:
        mov     word ptr [ms_ctype],ax
        mov     ax,word ptr [msr_ecx]
        mov     word ptr [ms_tscr],ax
        mov     ax,word ptr [msr_edx]
        mov     word ptr [ms_tcur],ax
        jmp     ms_draw

;--- 0Bh: CX, DX = mickeys moved since last asked
msf_0B:
        movsx   eax,word ptr [ms_cntx]
        mov     dword ptr [msr_ecx],eax
        movsx   eax,word ptr [ms_cnty]
        mov     dword ptr [msr_edx],eax
        mov     dword ptr [ms_cntx],0   ; both
        ret

;--- 0Ch: the event handler: CX = the conditions, ES:DX = the handler
msf_0C:
        mov     ax,word ptr [msr_ecx]
        mov     word ptr [ms_evmask],ax
        call    ms_hptr
        mov     dword ptr [ms_evhand],eax
        mov     eax,dword ptr [msr_tok]
        mov     dword ptr [ms_evown],eax
        mov     word ptr [ms_pend],0
        ret

;--- ms_hptr - the handler in ES:DX, as it is kept: seg:off for a box,
;--- an offset in its slot for a native program
ms_hptr:
        cmp     byte ptr [msr_box],0
        je      mhp_native
        mov     eax,dword ptr [msr_es]
        shl     eax,16
        mov     ax,word ptr [msr_edx]
        ret
mhp_native:
        mov     eax,dword ptr [msr_edx]
        ret

;--- 0Dh, 0Eh: light-pen emulation on and off.  Kept, and nothing else:
;--- there is no light pen here to emulate one for.
msf_0D:
        mov     word ptr [ms_lpen],1
        ret
msf_0E:
        mov     word ptr [ms_lpen],0
        ret

;--- 0Fh: mickeys per 8 pixels, CX across and DX down
msf_0F:
        mov     ax,word ptr [msr_ecx]
        or      ax,ax
        jle     m0F_y
        mov     word ptr [ms_mkx],ax
m0F_y:
        mov     ax,word ptr [msr_edx]
        or      ax,ax
        jle     m0F_out
        mov     word ptr [ms_mky],ax
m0F_out:
        ret

;--- 10h: hide the cursor while the pointer is in CX,DX - SI,DI
msf_10:
        mov     ax,word ptr [msr_ecx]
        mov     dx,word ptr [msr_esi]
        cmp     ax,dx
        jle     m10_x
        xchg    ax,dx
m10_x:
        mov     word ptr [ms_cx1],ax
        mov     word ptr [ms_cx2],dx
        mov     ax,word ptr [msr_edx]
        mov     dx,word ptr [msr_edi]
        cmp     ax,dx
        jle     m10_y
        xchg    ax,dx
m10_y:
        mov     word ptr [ms_cy1],ax
        mov     word ptr [ms_cy2],dx
        mov     word ptr [ms_condon],1
        call    ms_undraw               ; and at once, if it is in there now
        jmp     ms_draw

;--- 13h: the double-speed threshold, DX mickeys a second; 0 is 64
msf_13:
        mov     ax,word ptr [msr_edx]
        or      ax,ax
        jnz     m13_set
        mov     ax,64
m13_set:
        mov     word ptr [ms_dspeed],ax
        ret

;--- 14h: exchange handlers - CX, ES:DX the new, and the old back in them
msf_14:
        movzx   eax,word ptr [ms_evmask]
        push    eax
        push    dword ptr [ms_evhand]
        push    dword ptr [ms_evown]
        call    msf_0C
        pop     ecx                     ; the old owner
        pop     eax                     ; the old handler
        pop     edx                     ; the old mask
        mov     dword ptr [msr_ecx],edx
        cmp     byte ptr [msr_box],0
        je      m14_native
        cmp     ecx,MSTOK_BOX           ; a native program's means nothing
        jb      m14_none                ; to a box, and the other way round
        movzx   edx,ax
        mov     dword ptr [msr_edx],edx
        shr     eax,16
        mov     dword ptr [msr_es],eax
        ret
m14_native:
        cmp     ecx,MSTOK_BOX
        jae     m14_none
        mov     dword ptr [msr_edx],eax
        ret
m14_none:
        mov     dword ptr [msr_edx],0
        cmp     byte ptr [msr_box],0
        je      m14_out
        mov     dword ptr [msr_es],0
m14_out:
        ret

;--- 15h: BX = the bytes 16h needs
msf_15:
        mov     dword ptr [msr_ebx],MS_STATELEN
        ret

;--- 16h: the state into ES:DX
msf_16:
        cmp     dword ptr [msr_plen],MS_STATELEN
        jb      m16_out
        mov     esi,OFFSET ms_state
        mov     edi,dword ptr [msr_ptr]
        mov     ecx,MS_STATELEN
        rep     movsb
m16_out:
        ret

;--- 17h: and back again, from ES:DX
msf_17:
        cmp     dword ptr [msr_plen],MS_STATELEN
        jb      m17_out
        call    ms_undraw
        mov     esi,dword ptr [msr_ptr]
        mov     edi,OFFSET ms_state
        mov     ecx,MS_STATELEN
        rep     movsb
        ; what a program kept is believed only as far as it is possible
        cmp     word ptr [ms_vis],0
        jle     m17_vis
        mov     word ptr [ms_vis],0
m17_vis:
        cmp     word ptr [ms_ctype],1
        jbe     m17_type
        mov     word ptr [ms_ctype],0
m17_type:
        and     word ptr [ms_page],7
        call    ms_draw
m17_out:
        ret

;--- 18h: an alternate handler, for events with shift keys held.  CX =
;--- the conditions, with 20h Shift, 40h Ctrl, 80h Alt; ES:DX = where.
;--- Three of them, one per combination.  AX = 18h, or FFFFh.
msf_18:
        movzx   eax,word ptr [msr_ecx]
        mov     ebx,eax
        and     ebx,0E0h
        jz      m18_fail
        xor     esi,esi
m18_same:
        movzx   edx,word ptr [ms_altmask+esi*2]
        and     edx,0E0h
        cmp     edx,ebx
        je      m18_put
        inc     esi
        cmp     esi,3
        jb      m18_same
        xor     esi,esi
m18_free:
        cmp     word ptr [ms_altmask+esi*2],0
        je      m18_put
        inc     esi
        cmp     esi,3
        jb      m18_free
m18_fail:
        mov     dword ptr [msr_eax],0FFFFh
        ret
m18_put:
        mov     word ptr [ms_altmask+esi*2],ax
        call    ms_hptr
        mov     dword ptr [ms_althand+esi*4],eax
        mov     eax,dword ptr [msr_tok]
        mov     dword ptr [ms_altown+esi*4],eax
        mov     dword ptr [msr_eax],18h
        ret

;--- 19h: CX = the shift keys of an alternate handler -> CX = its whole
;--- mask (0 if there is none), BX:DX = it
msf_19:
        movzx   ebx,word ptr [msr_ecx]
        and     ebx,0E0h
        xor     esi,esi
m19_find:
        movzx   eax,word ptr [ms_altmask+esi*2]
        or      eax,eax
        jz      m19_next
        mov     edx,eax
        and     edx,0E0h
        cmp     edx,ebx
        je      m19_hit
m19_next:
        inc     esi
        cmp     esi,3
        jb      m19_find
        mov     dword ptr [msr_ecx],0
        ret
m19_hit:
        mov     dword ptr [msr_ecx],eax
        mov     eax,dword ptr [ms_althand+esi*4]
        cmp     byte ptr [msr_box],0
        je      m19_native
        movzx   edx,ax
        mov     dword ptr [msr_edx],edx
        shr     eax,16
        mov     dword ptr [msr_ebx],eax
        ret
m19_native:
        mov     dword ptr [msr_edx],eax
        mov     dword ptr [msr_ebx],0
        ret

;--- 1Ah: BX, CX = the speeds across and down (50 is as it comes), DX =
;--- the double-speed threshold.  1Bh gives them back.
msf_1A:
        mov     ax,word ptr [msr_ebx]
        mov     word ptr [ms_sensx],ax
        mov     ax,word ptr [msr_ecx]
        mov     word ptr [ms_sensy],ax
        jmp     msf_13
msf_1B:
        movzx   eax,word ptr [ms_sensx]
        mov     dword ptr [msr_ebx],eax
        movzx   eax,word ptr [ms_sensy]
        mov     dword ptr [msr_ecx],eax
        movzx   eax,word ptr [ms_dspeed]
        mov     dword ptr [msr_edx],eax
        ret

;--- 1Ch: the interrupt rate, BX = 0-4.  Only Microsoft's InPort mouse
;--- ever honoured it; it is kept, for 25h to report.
msf_1C:
        movzx   eax,word ptr [msr_ebx]
        cmp     eax,4
        ja      m1C_out
        mov     word ptr [ms_rate],ax
m1C_out:
        ret

;--- 1Dh, 1Eh: the display page the cursor is drawn on
msf_1D:
        call    ms_undraw
        mov     ax,word ptr [msr_ebx]
        and     ax,7
        mov     word ptr [ms_page],ax
        jmp     ms_draw
msf_1E:
        movzx   eax,word ptr [ms_page]
        mov     dword ptr [msr_ebx],eax
        ret

;--- 1Fh: disable the driver.  AX = 1Fh, ES:BX = "the INT 33h vector
;--- before the driver", which for a driver that was never loaded is an
;--- IRET - a program that puts it back unhooks us, as it would have
;--- unhooked MOUSE.COM.  20h enables it again.
msf_1F:
        call    ms_undraw
        mov     byte ptr [ms_off],1
        mov     dword ptr [msr_eax],1Fh
        cmp     byte ptr [msr_box],0
        je      m1F_native
        mov     eax,dword ptr [ms_seg]
        mov     dword ptr [msr_es],eax
        mov     dword ptr [msr_ebx],MSS_IRET
        ret
m1F_native:
        mov     dword ptr [msr_ebx],0
        ret
msf_20:
        mov     byte ptr [ms_off],0
        jmp     ms_draw

;--- 22h, 23h: the language, and English is the only one there is
msf_22:
        mov     ax,word ptr [msr_ebx]
        mov     word ptr [ms_lang],ax
        ret
msf_23:
        mov     dword ptr [msr_ebx],0
        ret

;--- 24h: BX = the version, CH = the mouse's type, CL = its IRQ (0 for a
;--- PS/2 mouse, which is how Microsoft's driver reports one)
msf_24:
        mov     dword ptr [msr_ebx],MS_VERSION
        call    ms_devtype
        shl     eax,8
        cmp     byte ptr [ms_hw],MS_SERIAL
        jne     m24_irq
        mov     al,byte ptr [ms_irq]
m24_irq:
        mov     dword ptr [msr_ecx],eax
        ret

;--- ms_devtype -> EAX = the type 24h and 2Ah report: 2 serial, 4 PS/2
ms_devtype:
        xor     eax,eax
        cmp     byte ptr [ms_hw],MS_PS2
        jne     mdt_ser
        mov     al,4
mdt_ser:
        cmp     byte ptr [ms_hw],MS_SERIAL
        jne     mdt_out
        mov     al,2
mdt_out:
        ret

;--- 25h: general information.  AX: bit 14, an integrated driver; bits
;--- 13-12 the cursor, 00 software text, 01 hardware, 10 graphics; bits
;--- 11-8 the rate.  BX, CX, DX: the driver is not locked, not busy.
msf_25:
        movzx   eax,word ptr [ms_ctype]
        cmp     byte ptr [ms_gtype],MGT_TEXT
        je      m25_text
        mov     eax,2
m25_text:
        shl     eax,12
        movzx   edx,word ptr [ms_rate]
        shl     edx,8
        or      eax,edx
        or      eax,4000h
        mov     dword ptr [msr_eax],eax
        xor     eax,eax
        mov     dword ptr [msr_ebx],eax
        mov     dword ptr [msr_ecx],eax
        mov     dword ptr [msr_edx],eax
        ret

;--- 26h: BX = disabled, CX, DX = the largest virtual position
msf_26:
        movzx   eax,byte ptr [ms_off]
        mov     dword ptr [msr_ebx],eax
        mov     eax,dword ptr [ms_vxmax]
        mov     dword ptr [msr_ecx],eax
        mov     eax,dword ptr [ms_vymax]
        mov     dword ptr [msr_edx],eax
        ret

;--- 27h: AX, BX = the text cursor's masks (or lines), CX, DX = 0Bh's
;--- counts, which it consumes as 0Bh does
msf_27:
        movzx   eax,word ptr [ms_tscr]
        mov     dword ptr [msr_eax],eax
        movzx   eax,word ptr [ms_tcur]
        mov     dword ptr [msr_ebx],eax
        jmp     msf_0B

;--- 2Ah: AX = the visibility counter, BX, CX = the hot spot, DX = the
;--- mouse's type
msf_2A:
        movsx   eax,word ptr [ms_vis]
        mov     dword ptr [msr_eax],eax
        movsx   eax,word ptr [ms_hotx]
        mov     dword ptr [msr_ebx],eax
        movsx   eax,word ptr [ms_hoty]
        mov     dword ptr [msr_ecx],eax
        call    ms_devtype
        mov     dword ptr [msr_edx],eax
        ret

;--- 2Fh: reset the hardware.  AX = FFFFh if it answered, 0 if not.
msf_2F:
        call    ms_hwreset
        mov     eax,0FFFFh
        jnc     m2F_out
        xor     eax,eax
m2F_out:
        mov     dword ptr [msr_eax],eax
        ret

;--- 31h: AX, BX = the range's minimums, CX, DX = its maximums
msf_31:
        movsx   eax,word ptr [ms_xmin]
        mov     dword ptr [msr_eax],eax
        movsx   eax,word ptr [ms_ymin]
        mov     dword ptr [msr_ebx],eax
        movsx   eax,word ptr [ms_xmax]
        mov     dword ptr [msr_ecx],eax
        movsx   eax,word ptr [ms_ymax]
        mov     dword ptr [msr_edx],eax
        ret

;--- 32h: AX = which of 25h-34h are here, one bit each from bit 15 down:
;--- 25h, 26h, 27h, 2Ah, 2Fh, 31h, 32h
msf_32:
        mov     dword ptr [msr_eax],0E42Ch
        xor     eax,eax
        mov     dword ptr [msr_ebx],eax
        mov     dword ptr [msr_ecx],eax
        mov     dword ptr [msr_edx],eax
        ret

;--- 4Dh, 6Dh: ES:DI -> the copyright string, and the version as 'V'
;--- and two bytes.  They live in the stub, so only a box can have them:
;--- a native program cannot address anything outside its own slot.
msf_4D:
        mov     eax,MSS_COPYR
        jmp     short m4D6D
msf_6D:
        mov     eax,MSS_VERSTR
m4D6D:
        cmp     byte ptr [msr_box],0
        je      m4D6D_out
        mov     dword ptr [msr_edi],eax
        mov     eax,dword ptr [ms_seg]
        mov     dword ptr [msr_es],eax
m4D6D_out:
        ret

;---------------------------------------------------------------------
; ms_hwreset - 2Fh: start the hardware again.  CF if it will not.
;---------------------------------------------------------------------
ms_hwreset:
        cmp     byte ptr [ms_hw],MS_PS2
        je      ms_ps2start
        cmp     byte ptr [ms_hw],MS_SERIAL
        je      mhr_serial
        stc
        ret
mhr_serial:
        mov     edx,dword ptr [ms_port]
        call    ms_sertry
        jc      mhr_out
        push    eax
        push    edx
        inc     edx
        mov     al,01h
        out     dx,al
        pop     edx
        pop     eax
        clc
mhr_out:
        ret

;=====================================================================
; THE WAY IN FROM A NATIVE PROGRAM - ms_int33, the IDT gate for 33h.
;
; The gate is DPL 3 and it is the only one the vector has, so three
; kinds of caller reach it and the CS says which.  A native program is
; ours.  A DPMI client is the DPMI host's, which reflects it into the
; box like any other of its BIOS vectors - that is where it went before
; there was a driver, and ms_fallback is that code.  The kernel itself
; never calls INT 33h.
;
; And one more thing arrives here from a native program: the RETF at
; the end of its own event handler, which lands on the INT 33h
; ms_r3chk wrote into its PSP.  The address it comes from says so.
;=====================================================================
ms_int33:
        cmp     word ptr ss:[esp+4],SEL_PROG_CS
        jne     msi_other
        pushad
        push    ds
        push    es
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     es,ax
        cld
        ; [esp] ES, DS, the PUSHAD block at +8, then EIP at +40, CS,
        ; EFLAGS, ESP and SS
        cmp     dword ptr [esp+40],SLOT_PSP + PSP_MSRET + 2
        jne     msi_call
        cmp     byte ptr [ms_busy],MSB_NATIVE
        je      msi_back
msi_call:
        mov     esi,esp
        add     esi,8
        mov     eax,dword ptr [esi+28]
        mov     dword ptr [msr_eax],eax
        mov     eax,dword ptr [esi+16]
        mov     dword ptr [msr_ebx],eax
        mov     eax,dword ptr [esi+24]
        mov     dword ptr [msr_ecx],eax
        mov     eax,dword ptr [esi+20]
        mov     dword ptr [msr_edx],eax
        mov     eax,dword ptr [esi+4]
        mov     dword ptr [msr_esi],eax
        mov     eax,dword ptr [esi]
        mov     dword ptr [msr_edi],eax
        mov     dword ptr [msr_es],0
        mov     byte ptr [msr_box],0
        ; whose: the program in the slot the descriptors describe
        mov     eax,1
        mov     edx,dword ptr [ms_pdepth]
        or      edx,edx
        jz      msi_tok
        add     eax,dword ptr [edx]
msi_tok:
        mov     dword ptr [msr_tok],eax
        ; EDX as a pointer: an offset in the slot, and only as much of
        ; the slot as lies past it
        mov     dword ptr [msr_ptr],0
        mov     dword ptr [msr_plen],0
        push    esi
        call    ms_slot                 ; EDI = its base, ECX = its size
        pop     esi
        jc      msi_noptr
        mov     eax,dword ptr [msr_edx]
        cmp     eax,ecx
        jae     msi_noptr
        sub     ecx,eax
        mov     dword ptr [msr_plen],ecx
        add     eax,edi
        mov     dword ptr [msr_ptr],eax
msi_noptr:
        push    esi
        call    ms_api
        pop     esi
        mov     eax,dword ptr [msr_eax]
        mov     dword ptr [esi+28],eax
        mov     eax,dword ptr [msr_ebx]
        mov     dword ptr [esi+16],eax
        mov     eax,dword ptr [msr_ecx]
        mov     dword ptr [esi+24],eax
        mov     eax,dword ptr [msr_edx]
        mov     dword ptr [esi+20],eax
        mov     eax,dword ptr [msr_esi]
        mov     dword ptr [esi+4],eax
        mov     eax,dword ptr [msr_edi]
        mov     dword ptr [esi],eax
        pop     es
        pop     ds
        popad
        iretd

;--- the handler has finished: the program goes back to exactly where
;--- the event interrupted it, every register as it was
msi_back:
        mov     esi,OFFSET ms_r3regs
        mov     edi,esp
        add     edi,8
        mov     ecx,8
        rep     movsd
        mov     eax,dword ptr [ms_r3eip]
        mov     dword ptr [esp+40],eax
        mov     eax,dword ptr [ms_r3efl]
        mov     dword ptr [esp+48],eax
        mov     eax,dword ptr [ms_r3esp]
        mov     dword ptr [esp+52],eax
        mov     byte ptr [ms_busy],0
        mov     dword ptr [ms_busyown],0
        pop     es
        pop     ds
        popad
        iretd

msi_other:
        test    byte ptr ss:[esp+4],3
        jz      msi_none                ; the kernel's own: nobody to serve
        cmp     dword ptr cs:[ms_fallback],0
        je      msi_none
        jmp     dword ptr cs:[ms_fallback]
msi_none:
        iretd

;---------------------------------------------------------------------
; ms_slot - the running native program's slot, out of the descriptor
;           r3_setdesc wrote: EDI = its base, ECX = its size.  CF if
;           there is none.  EAX and ESI are used.
;---------------------------------------------------------------------
ms_slot:
        mov     esi,GDT_BASE + GDT_IDX_PROGDS * 8
        test    byte ptr [esi+5],80h
        jz      msl_no
        movzx   edi,byte ptr [esi+7]
        shl     edi,8
        movzx   eax,byte ptr [esi+4]
        or      edi,eax
        shl     edi,16
        mov     di,word ptr [esi+2]
        movzx   ecx,byte ptr [esi+6]
        and     ecx,0Fh
        shl     ecx,16
        mov     cx,word ptr [esi]
        inc     ecx                     ; byte granular: the limit + 1
        clc
        ret
msl_no:
        stc
        ret

;=====================================================================
; THE WAY IN FROM A DOS BOX - ms_hlt, IOS_MSHLT.  ESI -> the context,
;                             stopped at a HLT; EAX = whose program it
;                             is.  CF clear if the HLT was the stub's.
;
; Three HLTs in the stub, told apart by address:
;
;   mstub_entry   INT 33h itself.  The frame the INT pushed comes off the
;               guest's stack first, so the program resumes after its
;               call with its own flags, and the registers go through
;               ms_api.
;   mstub_evhlt   an event was delivered (ms_boxwant) and the stub has
;               saved the guest's registers: load the handler's, and
;               the far pointer the CALL after this HLT goes through.
;   mstub_done    the handler has come back.
;=====================================================================
ms_hlt:
        push    edx
        mov     edx,dword ptr [ms_seg]
        or      edx,edx
        jz      mh_no
        cmp     dword ptr [esi+V86C_CS],edx
        jne     mh_no
        movzx   edx,word ptr [esi+V86C_EIP]
        cmp     edx,MSS_ENTRY
        je      mh_goapi
        cmp     edx,MSS_EVHLT
        je      mh_goevent
        cmp     edx,MSS_DONE
        je      mh_godone
mh_no:
        pop     edx
        stc
        ret
mh_goapi:
        pop     edx
        jmp     mh_api
mh_goevent:
        pop     edx
        jmp     mh_event
mh_godone:
        pop     edx
        jmp     mh_done

mh_api:
        pushad
        mov     dword ptr [msr_tok],eax
        call    ms_cpop                 ; the INT's frame: IP...
        mov     word ptr [esi+V86C_EIP],ax
        call    ms_cpop                 ; ...CS...
        mov     word ptr [esi+V86C_CS],ax
        call    ms_cpop                 ; ...and the caller's own flags
        mov     word ptr [esi+V86C_FLAGS],ax
        mov     eax,dword ptr [esi+V86C_EAX]
        mov     dword ptr [msr_eax],eax
        mov     eax,dword ptr [esi+V86C_EBX]
        mov     dword ptr [msr_ebx],eax
        mov     eax,dword ptr [esi+V86C_ECX]
        mov     dword ptr [msr_ecx],eax
        mov     eax,dword ptr [esi+V86C_EDX]
        mov     dword ptr [msr_edx],eax
        mov     eax,dword ptr [esi+V86C_ESI]
        mov     dword ptr [msr_esi],eax
        mov     eax,dword ptr [esi+V86C_EDI]
        mov     dword ptr [msr_edi],eax
        movzx   eax,word ptr [esi+V86C_ES]
        mov     dword ptr [msr_es],eax
        mov     byte ptr [msr_box],1
        shl     eax,4                   ; ES:DX, flat, and everything up to
        movzx   edx,word ptr [esi+V86C_EDX]     ; the end of what a real-
        add     eax,edx                         ; mode pointer can name
        mov     dword ptr [msr_ptr],eax
        mov     edx,HMA_BASE + 10000h
        sub     edx,eax
        mov     dword ptr [msr_plen],edx
        pushfd
        cli
        push    esi
        call    ms_api
        pop     esi
        popfd
        mov     ax,word ptr [msr_eax]
        mov     word ptr [esi+V86C_EAX],ax
        mov     ax,word ptr [msr_ebx]
        mov     word ptr [esi+V86C_EBX],ax
        mov     ax,word ptr [msr_ecx]
        mov     word ptr [esi+V86C_ECX],ax
        mov     ax,word ptr [msr_edx]
        mov     word ptr [esi+V86C_EDX],ax
        mov     ax,word ptr [msr_esi]
        mov     word ptr [esi+V86C_ESI],ax
        mov     ax,word ptr [msr_edi]
        mov     word ptr [esi+V86C_EDI],ax
        mov     eax,dword ptr [msr_es]
        mov     dword ptr [esi+V86C_ES],eax
        popad
        clc
        ret

mh_event:
        pushad
        pushfd
        cli
        push    esi                     ; the context: ms_pick uses ESI
        call    ms_pick
        pop     esi
        or      eax,eax
        jz      mhe_none
        cmp     ecx,MSTOK_BOX           ; the pick has changed hands since
        jb      mhe_none                ; the event was delivered
        mov     word ptr [esi+V86C_EAX],ax
        movzx   edi,word ptr [ms_seg]
        shl     edi,4
        mov     dword ptr [edi+MSS_HPTR],ebx
        mov     ax,word ptr [ms_btn]
        mov     word ptr [esi+V86C_EBX],ax
        movsx   eax,word ptr [ms_x]
        movsx   edx,word ptr [ms_y]
        call    ms_gran
        mov     word ptr [esi+V86C_ECX],ax
        mov     word ptr [esi+V86C_EDX],dx
        mov     ax,word ptr [ms_rawx]
        mov     word ptr [esi+V86C_ESI],ax
        mov     ax,word ptr [ms_rawy]
        mov     word ptr [esi+V86C_EDI],ax
        mov     eax,dword ptr [ms_seg]  ; DS: the driver's data segment,
        mov     dword ptr [esi+V86C_DS],eax     ; as Microsoft's gives it
        mov     word ptr [ms_pend],0
        mov     word ptr [esi+V86C_EIP],MSS_CALL
        popfd
        popad
        clc
        ret
mhe_none:
        mov     word ptr [esi+V86C_EIP],MSS_DONE
        popfd
        popad
        clc
        ret

mh_done:
        mov     byte ptr [ms_busy],0
        mov     dword ptr [ms_busyown],0
        mov     word ptr [esi+V86C_EIP],MSS_DONE + 1
        clc
        ret

;--- ms_cpop - ESI -> a context: AX = a word off its stack
ms_cpop:
        push    ebx
        push    edx
        mov     edx,dword ptr [esi+V86C_ESP]
        movzx   ebx,dx
        add     dx,2
        mov     dword ptr [esi+V86C_ESP],edx
        mov     edx,dword ptr [esi+V86C_SS]
        shl     edx,4
        add     ebx,edx
        mov     ax,word ptr [ebx]
        pop     edx
        pop     ebx
        ret

;---------------------------------------------------------------------
; ms_pick - who gets the events that are waiting, if anybody: EAX = the
;           conditions to pass (0: nobody), EBX = the handler, ECX =
;           its owner.  EDX, ESI, EDI are used.
;
; An alternate handler (18h) whose shift keys are exactly the ones held
; comes first; otherwise the one 0Ch installed.  Either is only given
; the conditions it asked for.
;---------------------------------------------------------------------
ms_pick:
        movzx   edx,word ptr [ms_pend]
        mov     al,byte ptr ds:[BDA_KBFLAG]
        xor     esi,esi
        test    al,03h
        jz      mpk_ns
        or      esi,20h
mpk_ns:
        test    al,04h
        jz      mpk_nc
        or      esi,40h
mpk_nc:
        test    al,08h
        jz      mpk_na
        or      esi,80h
mpk_na:
        or      esi,esi
        jz      mpk_main
        xor     edi,edi
mpk_alt:
        movzx   eax,word ptr [ms_altmask+edi*2]
        mov     ebx,eax
        and     ebx,0E0h
        cmp     ebx,esi
        jne     mpk_anext
        and     eax,edx
        and     eax,1Fh
        jz      mpk_anext
        mov     ebx,dword ptr [ms_althand+edi*4]
        mov     ecx,dword ptr [ms_altown+edi*4]
        or      ebx,ebx
        jnz     mpk_out
mpk_anext:
        inc     edi
        cmp     edi,3
        jb      mpk_alt
mpk_main:
        movzx   eax,word ptr [ms_evmask]
        and     eax,edx
        mov     ebx,dword ptr [ms_evhand]
        mov     ecx,dword ptr [ms_evown]
        or      ebx,ebx
        jnz     mpk_out
        xor     eax,eax
mpk_out:
        ret

;---------------------------------------------------------------------
; ms_boxwant - a box's event handler is owed a call and the guest can
;              take an interrupt now: CF clear, EAX = the stub's event
;              entry, and the driver is marked busy - the caller must
;              deliver.  Interrupts off.  Only EAX is changed.
;
; The same test the keyboard's v86_kbwant makes, at the same three
; places: the IRQ that interrupted the guest, a resume out of the
; monitor, and an entry to v86_run.  The guest's own interrupt flag has
; already been checked by the caller.
;---------------------------------------------------------------------
ms_boxwant:
        cmp     byte ptr [ms_busy],0
        jne     mbw_no
        cmp     word ptr [ms_pend],0
        je      mbw_no
        cmp     dword ptr [ms_seg],0
        je      mbw_no
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        call    ms_pick
        pop     edi
        pop     esi
        pop     edx
        or      eax,eax
        jz      mbw_none
        cmp     ecx,MSTOK_BOX
        jb      mbw_none
        mov     byte ptr [ms_busy],MSB_BOX
        mov     dword ptr [ms_busyown],ecx
        pop     ecx
        pop     ebx
        mov     eax,dword ptr [ms_seg]
        shl     eax,16
        mov     ax,MSS_EVENT
        clc
        ret
mbw_none:
        pop     ecx
        pop     ebx
mbw_no:
        stc
        ret

;=====================================================================
; ms_r3chk - IOS_MSR3, and irq_common on every way out.  EBX -> the
;            PUSHAD block of a program about to be resumed, EBP -> the
;            IRETD frame (EIP, CS, EFLAGS, ESP, SS) it will be resumed
;            through.  If that program is native, owns the handler an
;            event is waiting for, and is not already in one of the
;            kernel's handler calls, send it to the handler instead.
;
; What it is doing is kept here, the frame is pointed at the handler
; with the event's registers, and on the program's own stack goes a far
; return to PSP+56h, where an INT 33h brings it all back (ms_int33).
; The handler runs at ring 3 in the program's own segment, exactly as
; a handler called from Microsoft's IRQ routine ran in the program's.
;=====================================================================
ms_r3chk:
        cmp     word ptr [ebp+4],SEL_PROG_CS
        jne     mrc_ret
        test    dword ptr [ebp+8],EFL_VM        ; a box whose CS happens to
        jnz     mrc_ret                         ; be the same number
        cmp     byte ptr [ms_busy],0
        jne     mrc_ret
        cmp     word ptr [ms_pend],0
        jne     mrc_try
mrc_ret:
        ret
mrc_try:
        pushad
        mov     dword ptr [ms_r3blk],ebx
        mov     eax,dword ptr [ms_pcall]
        or      eax,eax
        jz      mrc_out
        cmp     dword ptr [eax],0
        jne     mrc_out                 ; inside an INT 23h/24h handler
        call    ms_pick
        or      eax,eax
        jz      mrc_out
        mov     edx,dword ptr [ms_pdepth]
        mov     edx,dword ptr [edx]
        inc     edx
        cmp     ecx,edx
        jne     mrc_out                 ; somebody else's
        mov     dword ptr [ms_dcond],eax
        mov     dword ptr [ms_dhand],ebx
        mov     dword ptr [ms_dtok],ecx
        call    ms_slot                 ; EDI = the slot, ECX = its size
        jc      mrc_out
        mov     esi,dword ptr [ebp+12]  ; its stack, with room for a far
        cmp     esi,ecx                 ; return below it
        ja      mrc_out
        cmp     esi,SLOT_IMAGE + 8
        jb      mrc_out

        mov     eax,dword ptr [ebp]
        mov     dword ptr [ms_r3eip],eax
        mov     eax,dword ptr [ebp+8]
        mov     dword ptr [ms_r3efl],eax
        mov     dword ptr [ms_r3esp],esi
        push    esi
        push    edi
        mov     esi,dword ptr [ms_r3blk]
        mov     edi,OFFSET ms_r3regs
        mov     ecx,8
        rep     movsd
        pop     edi
        pop     esi

        mov     word ptr [edi+SLOT_PSP+PSP_MSRET],33CDh ; INT 33h
        sub     esi,8
        mov     dword ptr [edi+esi],SLOT_PSP + PSP_MSRET
        mov     dword ptr [edi+esi+4],SEL_PROG_CS
        mov     dword ptr [ebp+12],esi
        mov     eax,dword ptr [ms_dhand]
        mov     dword ptr [ebp],eax
        mov     dword ptr [ebp+8],EFL_IF OR 2

        mov     edi,dword ptr [ms_r3blk]
        mov     eax,dword ptr [ms_dcond]
        mov     dword ptr [edi+28],eax          ; EAX
        movzx   eax,word ptr [ms_btn]
        mov     dword ptr [edi+16],eax          ; EBX
        movsx   eax,word ptr [ms_x]
        movsx   edx,word ptr [ms_y]
        call    ms_gran
        mov     dword ptr [edi+24],eax          ; ECX
        mov     dword ptr [edi+20],edx          ; EDX
        movsx   eax,word ptr [ms_rawx]
        mov     dword ptr [edi+4],eax           ; ESI
        movsx   eax,word ptr [ms_rawy]
        mov     dword ptr [edi],eax             ; EDI
        mov     word ptr [ms_pend],0
        mov     byte ptr [ms_busy],MSB_NATIVE
        mov     eax,dword ptr [ms_dtok]
        mov     dword ptr [ms_busyown],eax
mrc_out:
        popad
        ret

;=====================================================================
; ms_setup - IOS_MSINIT.  AX = the stub's segment (0: none), EDI = where
;            a DPMI client's INT 33h goes, ESI -> r3_depth, EDX ->
;            r3_calldep.
;
; The IVT entry is set ONCE, like the DOS vectors in vdm_stubs: the IVT
; is the machine's, and a TSR in a box that hooks INT 33h must still be
; hooked when the next box starts.
;=====================================================================
ms_setup:
        pushad
        mov     dword ptr [ms_fallback],edi
        mov     dword ptr [ms_pdepth],esi
        mov     dword ptr [ms_pcall],edx
        movzx   eax,ax
        mov     dword ptr [ms_seg],eax
        or      eax,eax
        jz      msu_nostub
        mov     edi,eax
        shl     edi,4
        mov     esi,OFFSET ms_stub
        mov     ecx,MS_STUBLEN
        rep     movsb
        shl     eax,16                  ; seg:MSS_ENTRY, which is 0
        mov     dword ptr ds:[33h*4],eax
msu_nostub:
        mov     al,33h
        mov     edi,OFFSET ms_int33
        call    set_vector
        popad
        ret

;=====================================================================
; ms_exit - IOS_MSEXIT.  EAX = the token of a program that has ended.
;
; DOS leaves a dead program's handler installed and the next event
; jumps into whatever is loaded there now; Microsoft's documentation
; tells programs to reset the driver on the way out, and not all of
; them do.  Here the handlers go with the program, and so does the
; cursor of a program that reset the driver and left it showing.  A
; program that went resident is not ended, and its handler stays.
;=====================================================================
ms_exit:
        pushfd
        cli
        pushad
        cmp     dword ptr [ms_evown],eax
        jne     mx_alt
        mov     word ptr [ms_evmask],0
        mov     dword ptr [ms_evhand],0
        mov     dword ptr [ms_evown],0
mx_alt:
        xor     ecx,ecx
mx_one:
        cmp     dword ptr [ms_altown+ecx*4],eax
        jne     mx_next
        mov     word ptr [ms_altmask+ecx*2],0
        mov     dword ptr [ms_althand+ecx*4],0
        mov     dword ptr [ms_altown+ecx*4],0
mx_next:
        inc     ecx
        cmp     ecx,3
        jb      mx_one
        cmp     dword ptr [ms_busyown],eax      ; it ended inside its handler
        jne     mx_cursor
        mov     byte ptr [ms_busy],0
        mov     dword ptr [ms_busyown],0
mx_cursor:
        cmp     dword ptr [ms_rstown],eax
        jne     mx_out
        call    ms_undraw
        mov     word ptr [ms_vis],-1
        mov     word ptr [ms_condon],0
        mov     byte ptr [ms_off],0
        mov     dword ptr [ms_rstown],0
mx_out:
        popad
        popfd
        ret

;=====================================================================
; ms_wakeq - IOS_MSWAKE.  AL = 1 if the mouse moved or a button changed
;            since the last time this was asked.  21h/F0h AL = 8 uses it
;            to wake a full-screen program for the mouse as well as for
;            a key.
;=====================================================================
ms_wakeq:
        pushfd
        cli
        mov     al,byte ptr [ms_wake]
        mov     byte ptr [ms_wake],0
        popfd
        ret

;=====================================================================
; ms_inject - IOS_MSINJECT.  Movement or bytes as if the mouse had sent
;             them, for the test programs: what a headless emulator
;             cannot do is move the mouse, and everything above the
;             wire can be tested from here.
;
;   BH = 0: BL = buttons, CX = mickeys across, DX = down
;   BH = 1: CL, CH, DL = a PS/2 packet
;   BH = 2: CL, CH, DL, DH = BL bytes from a serial mouse
;   BH = 3: CL, CH, DL = a PS/2 packet, THROUGH THE CONTROLLER: each
;           byte goes into the 8042's output buffer with D3h, which is
;           the controller's own "as if the mouse had sent this", and
;           arrives the way a real one does - IRQ 12 behind the
;           cascade, irq_mouse, the packet assembler.  Only a PS/2
;           mouse, and only a controller that has D3h.
;
; Sixteen bits, all of it: a DOS box's INT 21h reaches the kernel with
; the high halves of its registers gone, and the counts are signed.
;=====================================================================
ms_inject:
        cmp     bh,3
        je      ms_injkbc
        pushfd
        cli
        pushad
        cmp     bh,1
        je      mij_ps2
        cmp     bh,2
        je      mij_ser
        movsx   ecx,cx
        movsx   edx,dx
        call    ms_motion
        jmp     short mij_out
mij_ps2:
        mov     byte ptr [ms_pktn],0
        mov     esi,3
        jmp     short mij_bytes
mij_ser:
        movzx   esi,bl
        or      esi,esi
        jz      mij_out
        cmp     esi,4
        jbe     mij_bytes
        mov     esi,4
mij_bytes:
        shl     edx,16                  ; the four bytes in order, CL first
        mov     dx,cx
        mov     ecx,edx
mij_one:
        mov     al,cl
        cmp     bh,1
        jne     mij_serb
        call    ms_ps2byte
        jmp     short mij_next
mij_serb:
        call    ms_serbyte
mij_next:
        shr     ecx,8
        dec     esi
        jnz     mij_one
mij_out:
        popad
        popfd
        ret

;--- BH = 3: with interrupts ON, which is the point - the IRQ has to be
;--- taken - and each byte waits for the last to have been read, because
;--- the output buffer holds one byte and D3h would write over it.
ms_injkbc:
        cmp     byte ptr [ms_hw],MS_PS2
        jne     mik_ret
        pushfd
        pushad
        mov     byte ptr [ms_pktn],0
        shl     edx,16
        mov     dx,cx
        mov     esi,3
mik_one:
        cli
        mov     al,KBC_WRAUXOB
        call    ms_kbccmd
        call    ms_kbcwait
        mov     edi,dword ptr [ms_bytesin]
        mov     al,dl
        out     PORT_KBDATA,al
        sti
        mov     ebx,dword ptr ds:[BDA_TICKS]
mik_wait:
        cmp     edi,dword ptr [ms_bytesin]
        jne     mik_next                ; the driver has it
        mov     eax,dword ptr ds:[BDA_TICKS]
        sub     eax,ebx
        cmp     eax,3
        jb      mik_wait
mik_next:
        shr     edx,8
        dec     esi
        jnz     mik_one
        popad
        popfd
mik_ret:
        ret

;=====================================================================
; ms_i15 - IOS_MSI15.  ESI -> a box's context at INT 15h.  The BIOS's
;          pointing-device interface (AH = C2h) is refused while the
;          kernel drives a PS/2 mouse: it would reset the mouse and
;          point it at a handler of its own through an IRQ the box
;          never sees.  CF set and AH = 86h is what a BIOS without the
;          interface answers, and the program goes to INT 33h instead.
;          CF clear if answered.
;=====================================================================
ms_i15:
        cmp     byte ptr [esi+V86C_EAX+1],0C2h
        jne     mi15_no
        cmp     byte ptr [ms_hw],MS_PS2
        jne     mi15_no
        mov     byte ptr [esi+V86C_EAX+1],86h
        or      dword ptr [esi+V86C_FLAGS],1    ; CF
        clc
        ret
mi15_no:
        stc
        ret

;=====================================================================
; ms_park - IOS_MSPARK.  AL = 0: the machine is about to be handed to
;           real-mode code (RMDOS, and Windows on it); AL = 1: it is
;           back.
;
; Down, the mouse is told to stop sending: a mouse byte nobody reads
; blocks the keyboard behind it, and nothing down there reads IRQ 12
; until a mouse driver of its own asks for it.  INT 33h is pointed at
; the stub's IRET, because the HLT only means anything to the monitor.
; Up, the mouse is started again from the beginning - whatever ran
; down there may have reprogrammed it - and the cursor, if it was
; showing, is drawn afresh on a screen that has certainly changed.
;=====================================================================
ms_park:
        pushad
        mov     ebx,dword ptr [ms_seg]
        shl     ebx,16                  ; seg:MSS_ENTRY
        or      al,al
        jnz     mpa_up

        or      ebx,ebx
        jz      mpa_downhw
        cmp     dword ptr ds:[33h*4],ebx
        jne     mpa_downhw
        mov     bx,MSS_IRET
        mov     dword ptr ds:[33h*4],ebx
mpa_downhw:
        pushfd
        cli
        call    ms_undraw
        popfd
        cmp     byte ptr [ms_hw],MS_PS2
        jne     mpa_downser
        mov     byte ptr [ms_probing],1
        mov     al,0F5h                 ; stop reporting
        call    ms_auxcmd
        call    ms_auxdrain
        jmp     short mpa_out           ; probing stays up until it is back
mpa_downser:
        cmp     byte ptr [ms_hw],MS_SERIAL
        jne     mpa_out
        mov     edx,dword ptr [ms_port]
        inc     edx
        xor     al,al                   ; IER: no more interrupts
        out     dx,al
        jmp     short mpa_out

mpa_up:
        or      ebx,ebx
        jz      mpa_uphw
        mov     eax,ebx
        mov     ax,MSS_IRET
        cmp     dword ptr ds:[33h*4],eax
        jne     mpa_uphw
        mov     dword ptr ds:[33h*4],ebx
mpa_uphw:
        call    ms_hwreset              ; PS/2 or serial, from the start
        mov     byte ptr [ms_probing],0
        pushfd
        cli
        mov     byte ptr [ms_drawn],0
        mov     byte ptr [ms_gmode],0FFh
        call    ms_draw
        popfd
mpa_out:
        popad
        ret
