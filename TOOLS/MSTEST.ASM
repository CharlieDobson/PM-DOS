;=====================================================================
; MSTEST.ASM - MSTEST.COM, the mouse driver as a DOS program sees it
;
; INT 33h from a 16-bit program in a DOS box, against what Microsoft's
; driver answers.  Nobody can move a mouse in a headless emulator, so
; movement and buttons come from 21h/F0h AL=0Bh, which hands them to the
; driver exactly where a packet from the mouse would arrive: everything
; above the wire is what is being tested - the arithmetic, the ranges,
; the counters, the cursor on the screen, the event handler called in
; the box - and the wire itself is ms_init's business at boot.
;
;   MSTEST      the whole run
;   MSTEST H    reset, install a handler, show the cursor, and leave
;               without undoing any of it
;   MSTEST C    after an H: the handler and the cursor went with it
;
; Every line prints "ok" or "FAILED" with the first value that was
; wrong; the exit code is the number of FAILED lines.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_MT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_MT, DS:_MT, ES:_MT, SS:_MT
        ORG     0

MS      MACRO   fn
        mov     ax,fn
        int     33h
        ENDM

; movement as if the mouse had sent it: buttons, mickeys across, down
INJ     MACRO   btn, mx, my
        mov     bx,btn
        mov     ecx,mx
        mov     edx,my
        mov     ax,0F00Bh
        int     21h
        ENDM

; one value on the current line: what came back, and what should have
EXPECT     MACRO   got, want
        mov     ax,got
        mov     bx,want
        call    chk
        ENDM

SAY     MACRO   txt
        mov     dx,OFFSET txt
        call    step
        call    puts
        ENDM

CELL    EQU     (10 * 80 + 20) * 2      ; row 10, column 20
CELLX   EQU     20 * 8                  ; ...as the driver counts it
CELLY   EQU     10 * 8
STATEMAX EQU    400                     ; room for 16h's state

start:
        ; which run: the first thing in the command tail
        mov     si,81h
tail_sp:
        lodsb
        cmp     al,' '
        je      tail_sp
        and     al,0DFh
        cmp     al,'H'
        je      run_leave
        cmp     al,'C'
        je      run_check

        SAY     msg_hi

;=====================================================================
; Is there a driver, and is it Microsoft's
;=====================================================================
        SAY     t_vec
        mov     ax,3533h
        int     21h
        mov     ax,es
        push    ds
        pop     es
        mov     [vecseg],ax
        or      ax,bx
        jz      vec_bad
        push    ds
        mov     ds,[vecseg]
        mov     al,[bx]
        pop     ds
        movzx   ax,al
        EXPECT     ax,0F4h                 ; the stub's HLT, not an IRET
        call    verdict
        jmp     short vec_done
vec_bad:
        call    bad
vec_done:

        SAY     t_reset
        MS      0
        mov     si,bx
        EXPECT     ax,0FFFFh
        EXPECT     si,2
        call    verdict

        ; 24h: the version, and the mouse's type and IRQ - a PS/2 mouse is
        ; type 4 with an IRQ of 0 by Microsoft's convention, a serial one
        ; type 2 on its port's IRQ.  Which one this machine has decides
        ; the checks further down that only a PS/2 mouse can pass.
        SAY     t_ver
        MS      24h
        mov     byte ptr [mtype],ch
        mov     si,cx
        EXPECT  bx,0820h
        cmp     byte ptr [mtype],2
        je      ver_serial
        EXPECT  si,0400h
        jmp     short ver_done
ver_serial:
        mov     ax,si
        cmp     al,4                    ; IRQ 3 or 4, either of them: 4
        jne     ver_irq                 ; is counted as 3
        dec     al
ver_irq:
        EXPECT  ax,0203h
ver_done:
        call    verdict

        SAY     t_soft
        MS      21h
        mov     si,bx
        EXPECT     ax,0FFFFh
        EXPECT     si,2
        call    verdict

        SAY     t_hot
        MS      2Ah
        mov     si,bx
        EXPECT     ax,0FFFFh               ; hidden, once
        EXPECT     si,0
        EXPECT     cx,0
        mov     ax,dx                   ; the type 24h said
        movzx   bx,byte ptr [mtype]
        call    chk
        call    verdict

;=====================================================================
; Position and ranges, in 80x25 text: 640 by 200, a cell eight by eight
;=====================================================================
        SAY     t_centre
        MS      3
        EXPECT     bx,0
        EXPECT     cx,320
        EXPECT     dx,96                   ; 100, as a whole row
        call    verdict

        SAY     t_setpos
        mov     cx,100
        mov     dx,50
        MS      4
        MS      3
        EXPECT     cx,96
        EXPECT     dx,48
        call    verdict

        SAY     t_virt
        MS      26h
        EXPECT     bx,0
        EXPECT     cx,639
        EXPECT     dx,199
        call    verdict

        SAY     t_hrange
        mov     cx,320                  ; given backwards: the driver swaps
        mov     dx,160
        MS      7
        MS      3
        EXPECT     cx,160                  ; pulled inside
        MS      31h
        EXPECT     ax,160
        EXPECT     cx,320
        call    verdict

        SAY     t_vrange
        mov     cx,64
        mov     dx,128
        MS      8
        MS      3
        EXPECT     dx,64
        MS      31h
        mov     si,bx
        EXPECT     si,64
        EXPECT     dx,128
        call    verdict
        xor     cx,cx
        mov     dx,639
        MS      7
        xor     cx,cx
        mov     dx,199
        MS      8

;=====================================================================
; Movement.  Doubling off, so the arithmetic is Microsoft's plain one:
; 8 mickeys to 8 pixels across, 16 to 8 down.
;=====================================================================
        mov     dx,7FFFh
        MS      13h

        SAY     t_move
        mov     cx,320
        mov     dx,96
        MS      4
        INJ     0, 16, 16
        MS      3
        EXPECT     cx,336
        EXPECT     dx,104
        call    verdict

        SAY     t_count
        MS      0Bh
        EXPECT     cx,16
        EXPECT     dx,16
        MS      0Bh
        EXPECT     cx,0
        EXPECT     dx,0
        call    verdict

        SAY     t_ratio
        mov     cx,16
        mov     dx,16
        MS      0Fh
        INJ     0, 16, 16
        MS      3
        EXPECT     cx,344
        EXPECT     dx,112
        call    verdict
        mov     cx,8
        mov     dx,16
        MS      0Fh

        SAY     t_speed
        mov     bx,100
        mov     cx,100
        mov     dx,7FFFh
        MS      1Ah
        INJ     0, 8, 16
        MS      3
        EXPECT     cx,360
        EXPECT     dx,128
        MS      1Bh
        EXPECT     bx,100
        EXPECT     cx,100
        EXPECT     dx,7FFFh
        call    verdict
        SAY     t_speed50
        mov     bx,50
        mov     cx,50
        mov     dx,7FFFh
        MS      1Ah
        MS      1Bh
        EXPECT  bx,50
        EXPECT  cx,50
        EXPECT  dx,7FFFh
        call    verdict

        SAY     t_back
        INJ     0, -24, -32
        MS      3
        EXPECT     cx,336
        EXPECT     dx,112
        call    verdict

        SAY     t_edge
        mov     cx,630
        mov     dx,190
        MS      4
        INJ     0, 100, 100
        MS      3
        EXPECT     cx,632                  ; 639, as a whole cell
        EXPECT     dx,192
        call    verdict

        ; a slow move adds up rather than getting lost: 3 mickeys down
        ; is a pixel and a half, and the half is kept for the next one
        SAY     t_frac
        mov     cx,320
        mov     dx,96
        MS      4
        INJ     0, 0, 3
        INJ     0, 0, 3
        INJ     0, 0, 3
        INJ     0, 0, 3
        INJ     0, 0, 3
        INJ     0, 0, 1
        MS      3
        EXPECT     dx,104                  ; 16 mickeys: 8 pixels
        call    verdict

;=====================================================================
; The wire.  A PS/2 packet and a serial one, through their decoders;
; then a PS/2 packet through the 8042 itself, which raises IRQ 12 for
; each byte as it would for the mouse's - the path from the interrupt
; up, cascade and all.
;=====================================================================
        SAY     t_pkt
        mov     cx,320
        mov     dx,96
        MS      4
        mov     bx,0100h                ; PS/2: left down, 16 right, and
        mov     cx,1029h                ; 16 down - which a PS/2 mouse
        mov     dx,00F0h                ; counts as -16
        mov     ax,0F00Bh
        int     21h
        MS      3
        EXPECT  bx,1
        EXPECT  cx,336
        EXPECT  dx,104
        mov     bx,0203h                ; serial: no buttons, 16 left, 16
        mov     cx,3043h                ; down - X's top two bits are in
        mov     dx,0010h                ; the first byte
        mov     ax,0F00Bh
        int     21h
        MS      3
        EXPECT  bx,0
        EXPECT  cx,320
        EXPECT  dx,112
        call    verdict

        SAY     t_irq
        call    ps2only
        jc      irq_skip
        mov     bx,0300h                ; 8 right, through IRQ 12
        mov     cx,0808h
        mov     dx,0000h
        mov     ax,0F00Bh
        int     21h
        MS      3
        EXPECT  cx,328
        EXPECT  dx,112
        call    verdict
irq_skip:

;=====================================================================
; Buttons
;=====================================================================
        SAY     t_left
        mov     bx,0                    ; what the packets above counted,
        MS      5                       ; collected and so forgotten
        mov     bx,0
        MS      6
        mov     cx,200
        mov     dx,48
        MS      4
        INJ     1, 0, 0
        MS      3
        EXPECT     bx,1
        mov     bx,0
        MS      5
        mov     si,bx
        EXPECT     ax,1
        EXPECT     si,1
        EXPECT     cx,200
        EXPECT     dx,48
        mov     bx,0
        MS      5
        EXPECT     bx,0                    ; counted once, and reset by asking
        INJ     0, 8, 0
        mov     bx,0
        MS      6
        mov     si,bx
        EXPECT     ax,0
        EXPECT     si,1
        EXPECT     cx,208                  ; where it was let go
        call    verdict

        SAY     t_right
        INJ     2, 0, 0
        INJ     6, 0, 0                 ; and the middle one with it
        INJ     0, 0, 0
        mov     bx,1
        MS      5
        EXPECT     bx,1
        mov     bx,2
        MS      5
        EXPECT     bx,1
        mov     bx,1
        MS      6
        EXPECT     bx,1
        mov     bx,2
        MS      6
        EXPECT     bx,1
        call    verdict

;=====================================================================
; The text cursor on the screen.  Nothing is printed between drawing it
; and looking at it: printing would scroll the cell being looked at.
;=====================================================================
        MS      0
        mov     dx,7FFFh
        MS      13h
        push    0B800h
        pop     es
        mov     word ptr es:[CELL],1741h
        mov     word ptr es:[CELL+2],1742h
        mov     cx,CELLX
        mov     dx,CELLY
        MS      4
        MS      1
        mov     ax,es:[CELL]
        mov     [r1],ax                 ; drawn: 1741h AND 77FFh XOR 7700h
        MS      2
        mov     ax,es:[CELL]
        mov     [r2],ax                 ; and put back
        MS      1
        INJ     0, 8, 0                 ; a cell to the right
        mov     ax,es:[CELL]
        mov     [r3],ax
        mov     ax,es:[CELL+2]
        mov     [r4],ax
        mov     word ptr es:[CELL+2],1F58h      ; the program writes there
        MS      2
        mov     ax,es:[CELL+2]
        mov     [r5],ax                 ; and keeps what it wrote
        push    ds
        pop     es
        SAY     t_textcur
        EXPECT     [r1],6041h
        EXPECT     [r2],1741h
        EXPECT     [r3],1741h
        EXPECT     [r4],6042h
        EXPECT     [r5],1F58h
        call    verdict

        ; the hardware cursor: the CRTC's, moved, and moved back
        push    0B800h
        pop     es
        call    crtcpos
        mov     [r1],ax
        mov     bx,1
        mov     cx,0
        mov     dx,7
        MS      0Ah
        MS      1
        call    crtcpos
        mov     [r2],ax
        mov     ax,es:[CELL+2]
        mov     [r3],ax
        MS      2
        call    crtcpos
        mov     [r4],ax
        mov     bx,0
        mov     cx,77FFh
        mov     dx,7700h
        MS      0Ah
        push    ds
        pop     es
        SAY     t_hwcur
        EXPECT     [r2],10 * 80 + 21
        EXPECT     [r3],1F58h              ; the cell itself left alone
        mov     ax,[r1]
        EXPECT     [r4],ax
        call    verdict

        ; 10h: a region the cursor may not be in
        push    0B800h
        pop     es
        MS      1
        mov     cx,CELLX
        mov     dx,CELLY
        mov     si,CELLX + 15
        mov     di,CELLY + 7
        MS      10h
        mov     ax,es:[CELL+2]
        mov     [r1],ax                 ; hidden, so the program's cell
        MS      2Ah
        mov     [r2],ax                 ; and the counter says so
        MS      1
        mov     ax,es:[CELL+2]
        mov     [r3],ax                 ; 01h brings it back
        MS      2Ah
        mov     [r4],ax
        MS      2
        push    ds
        pop     es
        SAY     t_region
        EXPECT     [r1],1F58h
        EXPECT     [r2],0FFFFh
        EXPECT     [r3],6058h              ; 1F58h AND 77FFh XOR 7700h
        EXPECT     [r4],0
        call    verdict

        SAY     t_page
        mov     bx,1
        MS      1Dh
        MS      1Eh
        EXPECT     bx,1
        mov     bx,0
        MS      1Dh
        call    verdict

        SAY     t_state
        MS      15h
        mov     [r1],bx
        cmp     word ptr [r1],0
        je      st_bad
        cmp     word ptr [r1],STATEMAX
        ja      st_bad
        mov     cx,111
        mov     dx,57
        MS      4
        push    ds
        pop     es
        mov     dx,OFFSET statebuf
        MS      16h
        mov     cx,0
        mov     dx,0
        MS      4
        mov     dx,OFFSET statebuf
        MS      17h
        MS      3
        EXPECT     cx,104
        EXPECT     dx,56
        call    verdict
        jmp     short st_done
st_bad:
        call    bad
st_done:

;=====================================================================
; The event handler, called in the box as Microsoft's IRQ routine would
; call it.  The injection is an INT 21h, and the handler has run by the
; time the instruction after it does.
;=====================================================================
        MS      0
        mov     dx,7FFFh
        MS      13h
        mov     cx,320
        mov     dx,96
        MS      4
        mov     word ptr [h_count],0
        push    ds
        pop     es
        mov     cx,7Fh
        mov     dx,OFFSET handler
        MS      0Ch

        SAY     t_evmove
        INJ     0, 16, 0
        EXPECT     [h_count],1
        EXPECT     [h_ax],1                ; moved
        EXPECT     [h_bx],0
        EXPECT     [h_cx],336
        EXPECT     [h_dx],96
        EXPECT     [h_si],16               ; the mickeys, running
        EXPECT     [h_di],0
        mov     ax,[vecseg]
        EXPECT     [h_ds],ax               ; DS = the driver's
        call    verdict

        SAY     t_evbtn
        INJ     1, 0, 0
        EXPECT     [h_count],2
        EXPECT     [h_ax],2                ; left down
        EXPECT     [h_bx],1
        INJ     0, 0, 0
        EXPECT     [h_count],3
        EXPECT     [h_ax],4                ; left up
        INJ     2, 0, 0
        EXPECT     [h_ax],8
        INJ     0, 0, 0
        EXPECT     [h_ax],16
        call    verdict

        SAY     t_evmask
        mov     cx,2                    ; left presses only
        mov     dx,OFFSET handler
        MS      0Ch
        mov     word ptr [h_count],0
        INJ     0, 16, 0
        EXPECT     [h_count],0
        INJ     1, 0, 0
        EXPECT     [h_count],1
        EXPECT     [h_ax],2
        INJ     0, 0, 0
        EXPECT     [h_count],1
        call    verdict

        SAY     t_swap
        xor     cx,cx
        xor     dx,dx
        push    0
        pop     es
        MS      14h
        mov     ax,es
        mov     [r1],ax
        push    ds
        pop     es
        EXPECT     cx,2
        EXPECT     dx,<OFFSET handler>
        mov     ax,cs
        EXPECT     [r1],ax
        INJ     1, 0, 0
        INJ     0, 0, 0
        EXPECT     [h_count],1             ; nobody called
        call    verdict

        SAY     t_alt
        mov     word ptr [h_count],0
        mov     word ptr [h2_count],0
        mov     cx,41h                  ; Ctrl, and movement
        mov     dx,OFFSET handler2
        MS      18h
        EXPECT     ax,18h
        mov     cx,40h
        MS      19h
        mov     [r1],bx
        EXPECT     cx,41h
        EXPECT     dx,<OFFSET handler2>
        mov     ax,cs
        EXPECT     [r1],ax
        mov     cx,7Fh
        mov     dx,OFFSET handler
        MS      0Ch
        push    40h
        pop     es
        or      byte ptr es:[17h],04h   ; Ctrl down, as the BIOS says it
        INJ     0, 8, 0
        and     byte ptr es:[17h],NOT 04h
        push    ds
        pop     es
        EXPECT     [h2_count],1
        EXPECT     [h_count],0
        INJ     0, 8, 0
        EXPECT     [h2_count],1
        EXPECT     [h_count],1
        mov     cx,1Fh                  ; no shift key: refused
        mov     dx,OFFSET handler2
        MS      18h
        EXPECT     ax,0FFFFh
        call    verdict

        SAY     t_off
        MS      0
        MS      1Fh
        mov     [r1],ax
        mov     al,es:[bx]
        push    ds
        pop     es
        movzx   ax,al
        EXPECT     ax,0CFh                 ; an IRET to put back
        EXPECT     [r1],1Fh
        MS      3
        mov     [r2],cx
        INJ     0, 16, 0
        MS      3
        EXPECT     cx,[r2]                 ; nothing moves while it is off
        MS      20h
        INJ     0, 16, 0
        MS      3
        mov     ax,[r2]
        add     ax,16
        EXPECT     cx,ax
        call    verdict

        SAY     t_i15
        call    ps2only
        jc      i15_skip
        mov     ax,0C200h
        mov     bh,1
        stc
        int     15h
        mov     [r1],ax
        sbb     ax,ax
        EXPECT     ax,0FFFFh               ; CF
        mov     al,byte ptr [r1+1]
        movzx   ax,al
        EXPECT     ax,86h
        call    verdict
i15_skip:

        ; The mouse behind the 8042 is the kernel's: a command for it
        ; from a box (D4h, then E9h, "send your status") is swallowed and
        ; never answered, as on a machine without the port.  Had it gone
        ; through, the reply would have reached the kernel's driver as a
        ; packet - FAh first, which reads as the right button down.
        SAY     t_8042
        call    ps2only
        jc      kbc_skip
        mov     al,0D4h
        out     64h,al
        mov     al,0E9h
        out     60h,al
        push    40h
        pop     es
        mov     bx,es:[6Ch]
kb_wait:
        mov     ax,es:[6Ch]
        sub     ax,bx
        cmp     ax,3
        jb      kb_wait
        push    ds
        pop     es
        MS      3
        EXPECT  bx,0
        mov     dx,7FFFh
        MS      13h
        mov     bx,0300h                ; and the kernel's mouse still works
        mov     cx,0808h
        xor     dx,dx
        mov     ax,0F00Bh
        int     21h
        MS      3
        mov     [r1],cx
        mov     bx,0300h
        mov     cx,0808h
        xor     dx,dx
        mov     ax,0F00Bh
        int     21h
        MS      3
        sub     cx,[r1]
        EXPECT  cx,8
        call    verdict
kbc_skip:

        SAY     t_info
        MS      32h
        EXPECT     ax,0E42Ch
        MS      25h
        and     ax,4000h
        EXPECT     ax,4000h
        MS      6Dh
        mov     al,es:[di]
        mov     ah,es:[di+1]
        mov     [r1],ax
        mov     al,es:[di+2]
        movzx   ax,al
        mov     [r2],ax
        MS      4Dh
        mov     ax,es:[di]
        mov     [r3],ax
        push    ds
        pop     es
        EXPECT     [r1],0856h              ; 'V', 8
        EXPECT     [r2],20h
        EXPECT     [r3],6F43h              ; "Co"
        call    verdict

;=====================================================================
; Graphics: mode 13h, a byte a pixel
;=====================================================================
        mov     ax,0013h
        int     10h
        MS      0
        MS      26h
        mov     [r1],cx
        mov     [r2],dx
        push    0A000h
        pop     es
        xor     di,di
        mov     cx,64000
        mov     al,1
        rep     stosb
        mov     cx,0
        mov     dx,0
        MS      4
        MS      1
        mov     al,es:[0]               ; (0,0): the arrow's tip, black
        mov     byte ptr [r3],al
        mov     al,es:[320 + 1]         ; (1,1): inside it, white
        mov     byte ptr [r3+1],al
        mov     al,es:[15]              ; (15,0): outside it, untouched
        mov     byte ptr [r4],al
        MS      2
        mov     al,es:[320 + 1]
        mov     byte ptr [r4+1],al      ; and put back
        ; a cursor of our own: a solid square, hot spot in the middle
        push    ds
        pop     es
        mov     bx,8
        mov     cx,8
        mov     dx,OFFSET blockcur
        MS      9
        mov     cx,200                  ; pixel (100, 100)
        mov     dx,100
        MS      4
        MS      1
        push    0A000h
        pop     es
        mov     al,es:[92 * 320 + 92]
        mov     byte ptr [r5],al
        mov     al,es:[107 * 320 + 107]
        mov     byte ptr [r5+1],al
        mov     al,es:[92 * 320 + 91]
        mov     byte ptr [r6],al
        MS      2
        mov     al,es:[100 * 320 + 100]
        mov     byte ptr [r6+1],al
        mov     cx,101
        mov     dx,50
        MS      4
        MS      3
        mov     [r7],cx
        push    ds
        pop     es
        mov     ax,0003h
        int     10h
        SAY     t_g13
        EXPECT     [r1],639
        EXPECT     [r2],199
        EXPECT     [r3],0F00h
        EXPECT     [r4],0101h
        EXPECT     [r5],0F0Fh
        EXPECT     [r6],0101h
        EXPECT     [r7],100                ; an even X: two per pixel
        call    verdict

;=====================================================================
; Graphics: mode 12h, four planes, and the VGA's registers the program
; had set must be what it finds afterwards
;=====================================================================
        mov     ax,0012h
        int     10h
        MS      0
        mov     dx,7FFFh
        MS      13h
        mov     cx,100
        mov     dx,100
        MS      4
        MS      1
        ; the program's own settings, left mid-sequence
        mov     dx,3C4h
        mov     ax,0302h                ; map mask 3
        out     dx,ax
        mov     dx,3CEh
        mov     ax,0205h                ; write mode 2
        out     dx,ax
        mov     ax,5508h                ; bit mask 55h, and index 8 left
        out     dx,ax                   ; selected
        INJ     0, 8, 0                 ; the cursor moves: off, and on
        mov     dx,3CEh
        in      al,dx
        mov     byte ptr [r1],al
        inc     dx
        in      al,dx
        mov     byte ptr [r1+1],al
        dec     dx
        mov     al,5
        out     dx,al
        inc     dx
        in      al,dx
        mov     byte ptr [r2],al
        mov     dx,3C4h
        in      al,dx
        mov     byte ptr [r2+1],al
        inc     dx
        in      al,dx
        mov     byte ptr [r3],al
        ; the BIOS's defaults back, so that INT 10h can read pixels
        mov     dx,3C4h
        mov     ax,0F02h
        out     dx,ax
        mov     dx,3CEh
        mov     ax,0005h
        out     dx,ax
        mov     ax,0FF08h
        out     dx,ax
        mov     cx,109                  ; the pointer is at (108,100) now:
        mov     dx,101                  ; (109,101) is inside the arrow
        mov     ah,0Dh
        xor     bh,bh
        int     10h
        mov     byte ptr [r4],al
        MS      2
        mov     cx,109
        mov     dx,101
        mov     ah,0Dh
        xor     bh,bh
        int     10h
        mov     byte ptr [r4+1],al
        mov     ax,0003h
        int     10h
        SAY     t_g12
        EXPECT     [r1],5508h
        EXPECT     [r2],0202h     ; mode 2; the sequencer at 2
        movzx   ax,byte ptr [r3]
        EXPECT     ax,03h
        EXPECT     [r4],000Fh
        call    verdict

;=====================================================================
; Graphics: mode 4, CGA - two bits a pixel, odd rows at 2000h
;=====================================================================
        mov     ax,0004h
        int     10h
        MS      0
        push    0B800h
        pop     es
        xor     di,di
        mov     cx,4000h
        mov     al,0FFh
        rep     stosb
        xor     cx,cx
        xor     dx,dx
        MS      4
        MS      1
        mov     al,es:[0]               ; row 0: AND 3Fh, XOR 0
        mov     byte ptr [r1],al
        mov     al,es:[2000h]           ; row 1: AND 1Fh, XOR 40h
        mov     byte ptr [r1+1],al
        MS      2
        mov     al,es:[2000h]
        mov     byte ptr [r2],al
        push    ds
        pop     es
        mov     ax,0003h
        int     10h
        SAY     t_g4
        EXPECT     [r1],5F3Fh
        movzx   ax,byte ptr [r2]
        EXPECT     ax,0FFh
        call    verdict

        MS      0
        movzx   ax,byte ptr [fails]
        push    ax
        SAY     msg_bye
        pop     ax
        mov     ah,4Ch
        int     21h

;=====================================================================
; MSTEST H - leave things behind, as a careless program would
;=====================================================================
run_leave:
        MS      0
        push    ds
        pop     es
        mov     cx,7Fh
        mov     dx,OFFSET handler
        MS      0Ch
        MS      1
        SAY     msg_left
        mov     ax,4C00h
        int     21h

;=====================================================================
; MSTEST C - and see that they went with it
;=====================================================================
run_check:
        SAY     t_gone
        MS      2Ah
        EXPECT     ax,0FFFFh               ; hidden again
        xor     cx,cx
        xor     dx,dx
        push    0
        pop     es
        MS      14h
        mov     ax,es
        mov     [r1],ax
        push    ds
        pop     es
        EXPECT     cx,0                    ; no handler left to swap out
        EXPECT     dx,0
        EXPECT     [r1],0
        call    verdict
        movzx   ax,byte ptr [fails]
        mov     ah,4Ch
        int     21h

;=====================================================================
; The two event handlers.  Far calls, DS the driver's: our data is
; reached through CS, which in a .COM is the same segment.
;=====================================================================
handler:
        mov     cs:[h_ax],ax
        mov     cs:[h_bx],bx
        mov     cs:[h_cx],cx
        mov     cs:[h_dx],dx
        mov     cs:[h_si],si
        mov     cs:[h_di],di
        mov     cs:[h_ds],ds
        inc     word ptr cs:[h_count]
        ; a handler may use every register: the driver keeps the ones
        ; the program was interrupted with
        xor     ax,ax
        mov     bx,ax
        mov     cx,ax
        mov     dx,ax
        mov     si,ax
        mov     di,ax
        mov     bp,ax
        mov     es,ax
        retf

handler2:
        inc     word ptr cs:[h2_count]
        retf

;=====================================================================
; helpers
;=====================================================================

; step - count the lines printed, and show the count in the bottom right
; corner of the screen itself: the output is usually going to a file,
; and a run that stops shows on the screen where it stopped.
step:
        push    ax
        push    es
        inc     byte ptr [stepn]
        push    0B800h
        pop     es
        mov     al,[stepn]
        shr     al,4
        call    hexch
        mov     es:[24 * 160 + 156],al
        mov     byte ptr es:[24 * 160 + 157],4Fh
        mov     al,[stepn]
        call    hexch
        mov     es:[24 * 160 + 158],al
        mov     byte ptr es:[24 * 160 + 159],4Fh
        pop     es
        pop     ax
        ret
hexch:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     hexch_ok
        add     al,7
hexch_ok:
        ret

; ps2only - CF, and the line says so, when the mouse is not a PS/2 one:
; the checks that talk to the 8042 or to INT 15h have nothing to check
ps2only:
        cmp     byte ptr [mtype],4
        je      p2_yes
        push    dx
        mov     dx,OFFSET msg_skip
        call    puts
        pop     dx
        stc
        ret
p2_yes:
        clc
        ret

; crtcpos - AX = where the CRT controller's cursor is, in cells
crtcpos:
        push    dx
        mov     dx,3D4h
        mov     al,0Eh
        out     dx,al
        inc     dx
        in      al,dx
        mov     ah,al
        dec     dx
        mov     al,0Fh
        out     dx,al
        inc     dx
        in      al,dx
        pop     dx
        ret

; chk - AX = what came back, BX = what should have.  The first
; difference on a line is kept for verdict to report.
chk:
        cmp     ax,bx
        je      chk_out
        cmp     byte ptr [miss],0
        jne     chk_out
        mov     byte ptr [miss],1
        mov     [miss_got],ax
        mov     [miss_want],bx
        mov     al,[chkn]
        mov     [miss_n],al
chk_out:
        inc     byte ptr [chkn]
        ret

; verdict - "ok", or FAILED with which check it was and the two values
verdict:
        cmp     byte ptr [miss],0
        jne     vd_bad
        call    good
        jmp     short vd_reset
vd_bad:
        mov     dx,OFFSET msg_chk
        call    puts
        mov     al,[miss_n]
        call    pdec
        mov     dx,OFFSET msg_got
        call    puts
        mov     ax,[miss_got]
        call    put4
        mov     dx,OFFSET msg_want
        call    puts
        mov     ax,[miss_want]
        call    put4
        call    bad
vd_reset:
        mov     byte ptr [miss],0
        mov     byte ptr [chkn],0
        ret

good:
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
        mov     ah,09h
        int     21h
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
pdec:                                   ; AL in decimal, two digits at most
        push    ax
        movzx   ax,al
        mov     dl,10
        div     dl
        or      al,al
        jz      pd_units
        push    ax
        add     al,'0'
        mov     dl,al
        call    putc
        pop     ax
pd_units:
        mov     al,ah
        add     al,'0'
        mov     dl,al
        call    putc
        pop     ax
        ret

;=====================================================================
; data
;=====================================================================
msg_hi      db  13,10,'MSTEST - INT 33h from a DOS box',13,10,'$'
msg_bye     db  'MSTEST done.',13,10,'$'
msg_left    db  'MSTEST H: a handler and a cursor left behind',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_skip    db  ' skipped: not a PS/2 mouse',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_chk     db  ' #$'
msg_got     db  ' got $'
msg_want    db  ' want $'

t_vec       db  '  INT 33h points at a driver$'
t_reset     db  '  00h reset: AX FFFFh, two buttons$'
t_ver       db  '  24h version 8.20, and the type$'
t_soft      db  '  21h software reset$'
t_hot       db  '  2Ah hidden, hot spot 0,0, the type again$'
t_centre    db  '  03h after a reset: the middle, no buttons$'
t_setpos    db  '  04h, and 03h rounds to a cell$'
t_virt      db  '  26h 640 by 200$'
t_hrange    db  '  07h range, and the pointer inside it$'
t_vrange    db  '  08h range$'
t_move      db  '  movement: 8 mickeys a cell across, 16 down$'
t_count     db  '  0Bh counts, and starts again$'
t_ratio     db  '  0Fh ratio$'
t_speed     db  '  1Ah/1Bh speed 100 moves twice as far$'
t_speed50   db  '  1Ah back to 50$'
t_back      db  '  back up and left$'
t_edge      db  '  stops at the edge of the range$'
t_frac      db  '  slow movement adds up$'
t_pkt       db  '  a PS/2 packet and a serial one, decoded$'
t_irq       db  '  a PS/2 packet through the 8042 and IRQ 12$'
t_left      db  '  05h/06h left press and release$'
t_right     db  '  05h/06h right and middle$'
t_textcur   db  '  text cursor drawn, moved, and kept off a write$'
t_hwcur     db  '  0Ah hardware cursor$'
t_region    db  '  10h region hides it, 01h shows it$'
t_page      db  '  1Dh/1Eh page$'
t_state     db  '  15h/16h/17h save and restore$'
t_evmove    db  '  0Ch handler on movement, with the registers$'
t_evbtn     db  '  0Ch handler on the buttons$'
t_evmask    db  '  0Ch mask chooses the events$'
t_swap      db  '  14h swaps the handler out$'
t_alt       db  '  18h/19h Ctrl handler$'
t_off       db  '  1Fh/20h disable and enable$'
t_i15       db  '  INT 15h C2h refused$'
t_8042      db  '  the 8042''s mouse is out of a box''s reach$'
t_info      db  '  32h, 25h, 6Dh, 4Dh$'
t_g13       db  '  mode 13h cursor$'
t_g12       db  '  mode 12h cursor, VGA registers kept$'
t_g4        db  '  mode 4 cursor$'
t_gone      db  '  MSTEST H left nothing behind$'

; a 16x16 cursor that is all cursor: white wherever it is
blockcur    dw  16 dup(0)
            dw  16 dup(0FFFFh)

fails       db  0
mtype       db  4                       ; 24h's type: 4 PS/2, 2 serial
stepn       db  0
miss        db  0
chkn        db  0
miss_n      db  0
miss_got    dw  0
miss_want   dw  0
vecseg      dw  0
r1          dw  0
r2          dw  0
r3          dw  0
r4          dw  0
r5          dw  0
r6          dw  0
r7          dw  0
h_count     dw  0
h2_count    dw  0
h_ax        dw  0
h_bx        dw  0
h_cx        dw  0
h_dx        dw  0
h_si        dw  0
h_di        dw  0
h_ds        dw  0
statebuf    db  STATEMAX dup(0)

_MT     ENDS
        END
