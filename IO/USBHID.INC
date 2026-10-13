;=====================================================================
; USBHID.INC - boot-protocol keyboards and mice
;
; A USB keyboard does not send scan codes.  It sends REPORTS - eight
; bytes: the modifiers as bits, then up to six keys that are down, as
; HID usages - whenever something changes, and nothing while nothing
; does.  A mouse sends buttons and two signed distances.  What the
; kernel has is a keyboard driver that eats set-1 scan codes off an
; 8042 (kb_byte, KEYBD.INC) and a mouse driver that assembles PS/2
; packets (ms_ps2byte, MOUSE.INC), and both of those feed everything
; above them: the BIOS ring at 40:1E, the DOS box's INT 09h and its
; port 60h, INT 33h, the layouts, the lights.  So a report is turned
; into what the 8042 would have delivered - a make or break code for
; every key that changed, three bytes of packet for every mouse
; report - and handed to those two.  Nothing above the wire knows.
;
; WHAT THE WIRE DOES NOT DO FOR US.  A USB keyboard does not repeat a
; held key: the host does, and usbhid_repeat does it here, to the rate
; KEYRATE= set (kb_ratewant, in the 8042's units).  The lights are
; not a keyboard command but a SET_REPORT control transfer, sent from
; the tick when 40:17 disagrees with what the keyboard was last told,
; exactly as kb_tick does for the PS/2 one.  And a key that is down
; when the keyboard is unplugged never sends its break code, so
; usbhid_kbdgone sends them.
;
; The reports arrive in a RING of interrupt descriptors the controller
; turns one a frame (hc_ringadd); the tick takes what has come, in
; order, and re-arms each one (usbhid_harvest).
;
; Included by IOPM.ASM after OHCI.INC.
;=====================================================================

HID_MODMASK     EQU     0E0h            ; usages E0h-E7h are the modifiers
HID_PHANTOM     EQU     01h             ; every key slot 01h: too many down

;=====================================================================
; usbhid_arm - EBX -> a configured keyboard or mouse: its interrupt
;              queue and ring into the schedule.  CF if the pool is
;              empty.  Everything preserved.
;=====================================================================
usbhid_arm:
        pushad
        call    dev_hc
        mov     dword ptr [ebx+USBD_RING],0
        call    hc_intrq                ; -> EDI = the queue
        jc      uha_fail
        mov     dword ptr [ebx+USBD_QH],edi
        ; the ring's buffers: this slot's share of SCR_RINGS
        call    dev_slot
        movzx   edx,al
        imul    edx,RING_N * RING_BUF
        add     edx,dword ptr [usb_scr]
        add     edx,SCR_RINGS
        movzx   ebp,word ptr [ebx+USBD_MAXPIN]
        cmp     ebp,RING_BUF
        jbe     uha_len
        mov     ebp,RING_BUF
uha_len:
        xor     ecx,ecx                 ; ECX = how many so far
uha_lp:
        call    el_alloc
        jc      uha_short
        mov     dword ptr [eax+EL_BUF],edx
        mov     word ptr [eax+EL_LEN16],bp
        mov     byte ptr [eax+EL_TOG],cl        ; alternating, for UHCI
        and     byte ptr [eax+EL_TOG],1
        call    hc_ringadd
        add     edx,RING_BUF
        inc     ecx
        cmp     ecx,RING_N
        jb      uha_lp
        mov     byte ptr [ebx+USBD_RINGI],0
        mov     byte ptr [ebx+USBD_RINGN],RING_N
        mov     byte ptr [ebx+USBD_LEDS],0FFh   ; unknown: set them first
        mov     byte ptr [ebx+USBD_REPKEY],0
        lea     edi,[ebx+USBD_REPORT]
        xor     eax,eax
        stosd
        stosd
        popad
        clc
        ret
uha_short:
        ; a ring that did not fill: what there is comes back, and the
        ; queue with it, and the device is failed
        mov     eax,dword ptr [ebx+USBD_RING]
        or      eax,eax
        jz      uha_noring
        ; the circle is of ECX elements, not RING_N
uha_freelp:
        push    ecx
        mov     ecx,dword ptr [eax+EL_NEXT]
        call    el_free
        mov     eax,ecx
        pop     ecx
        dec     ecx
        jnz     uha_freelp
        mov     dword ptr [ebx+USBD_RING],0
uha_noring:
        call    hc_intrdel
        mov     eax,edi
        call    el_free
        mov     dword ptr [ebx+USBD_QH],0
uha_fail:
        popad
        stc
        ret

;=====================================================================
; usbhid_harvest - once a tick: every report every keyboard and mouse
;                  has sent since the last one, in order, acted on.
;=====================================================================
usbhid_harvest:
        pushad
        mov     ebx,OFFSET usb_devs
        mov     ecx,USB_MAXDEV
uhh_dev:
        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     uhh_next
        mov     al,[ebx+USBD_CLASS]
        cmp     al,USBC_KBD
        je      uhh_this
        cmp     al,USBC_MOUSE
        jne     uhh_next
uhh_this:
        cmp     dword ptr [ebx+USBD_RING],0
        je      uhh_next
        push    ecx
        call    dev_hc
        mov     edi,dword ptr [ebx+USBD_QH]
        mov     ebp,RING_N              ; at most the whole ring a tick
uhh_one:
        call    uhh_current             ; -> EAX = the next to look at
        call    hc_ringst               ; -> AL = XS_*, ECX' = bytes
        cmp     al,XS_PENDING
        je      uhh_devdone
        ; done or failed: the report, if it is one, into usbhid_rep
        mov     edx,ecx                 ; EDX = bytes, 0 on an error
        cmp     al,XS_DONE
        je      uhh_copy
        xor     edx,edx
uhh_copy:
        push    esi
        push    edi
        call    uhh_current
        mov     esi,dword ptr [eax+EL_BUF]
        mov     edi,OFFSET usbhid_rep
        xor     eax,eax
        stosd
        stosd
        mov     edi,OFFSET usbhid_rep
        mov     ecx,edx
        cmp     ecx,RING_BUF
        jbe     uhh_cplen
        mov     ecx,RING_BUF
uhh_cplen:
        rep     movsb
        pop     edi
        pop     esi
        ; the descriptor goes round again
        call    uhh_current
        call    hc_rearm
        mov     al,[ebx+USBD_RINGI]
        inc     al
        cmp     al,[ebx+USBD_RINGN]
        jb      uhh_ringi
        xor     al,al
uhh_ringi:
        mov     [ebx+USBD_RINGI],al
        ; and the report is acted on
        or      edx,edx
        jz      uhh_acted
        cmp     byte ptr [ebx+USBD_CLASS],USBC_KBD
        jne     uhh_mouse
        cmp     edx,8
        jb      uhh_acted               ; a keyboard report is eight
        call    usbhid_kbd
        jmp     uhh_acted
uhh_mouse:
        cmp     edx,3
        jb      uhh_acted
        call    usbhid_mouse
uhh_acted:
        dec     ebp
        jz      uhh_devdone
        jmp     uhh_one
uhh_devdone:
        pop     ecx
uhh_next:
        add     ebx,USBD_LEN
        dec     ecx
        jz      uhh_alldone
        jmp     uhh_dev
uhh_alldone:
        popad
        ret

;--- uhh_current - EAX = the ring descriptor at USBD_RINGI
uhh_current:
        push    ecx
        mov     eax,dword ptr [ebx+USBD_RING]
        movzx   ecx,byte ptr [ebx+USBD_RINGI]
        jecxz   uhc_out
uhc_walk:
        mov     eax,dword ptr [eax+EL_NEXT]
        loop    uhc_walk
uhc_out:
        pop     ecx
        ret

;=====================================================================
; usbhid_kbd - EBX -> the keyboard, usbhid_rep = its new report.  Every
;              key that changed goes to kb_byte as the 8042 would have
;              sent it, and the report is kept for the next comparison.
;=====================================================================
usbhid_kbd:
        pushad
IFDEF HANG_E9
        mov     al,'K'
        out     0E9h,al
        mov     esi,OFFSET usbhid_rep
        mov     ecx,8
ukb_tr:
        lodsb
        movzx   eax,al
        call    e9_hex8
        loop    ukb_tr
        mov     al,13
        out     0E9h,al
        mov     al,10
        out     0E9h,al
ENDIF
        ; too many keys down: the keyboard says so with 01h in every
        ; slot, and the report describes nothing
        mov     esi,OFFSET usbhid_rep
        mov     ecx,6
        lea     edi,[esi+2]
ukb_phantom:
        cmp     byte ptr [edi],HID_PHANTOM
        jne     ukb_real
        inc     edi
        loop    ukb_phantom
        jmp     ukb_out
ukb_real:
        ; the modifiers: a bit each, make or break for each that moved
        mov     al,[esi]
        mov     ah,[ebx+USBD_REPORT]
        xor     ah,al                   ; AH = the ones that changed
        xor     ecx,ecx                 ; for each bit 0-7 of AH
ukb_mods:
        mov     dl,ah
        shr     dl,cl
        test    dl,1
        jz      ukb_modsnext
        mov     dl,al
        shr     dl,cl
        and     dl,1
        xor     dl,1                    ; DL = 0 pressed (make), 1 released
        shl     dl,7                    ; ...as the break bit
        push    eax
        push    ecx
        mov     al,cl
        or      al,HID_MODMASK          ; the usage, E0h and up
        call    usbhid_emit
        pop     ecx
        pop     eax
ukb_modsnext:
        inc     ecx
        cmp     ecx,8
        jb      ukb_mods

        ; keys released: in the old report and not in the new
        lea     edi,[ebx+USBD_REPORT+2]
        mov     ecx,6
ukb_rel:
        mov     al,[edi]
        or      al,al
        jz      ukb_relnext
        lea     edx,[esi+2]
        call    ukb_has                 ; is AL among the six at EDX?
        jnc     ukb_relnext             ; still down
        push    ecx
        mov     dl,80h
        call    usbhid_emit
        cmp     al,[ebx+USBD_REPKEY]
        jne     ukb_relkept
        mov     byte ptr [ebx+USBD_REPKEY],0
ukb_relkept:
        pop     ecx
ukb_relnext:
        inc     edi
        loop    ukb_rel

        ; keys pressed: in the new and not in the old.  The last one
        ; pressed is the one that repeats
        lea     edi,[esi+2]
        mov     ecx,6
ukb_prs:
        mov     al,[edi]
        or      al,al
        jz      ukb_prsnext
        lea     edx,[ebx+USBD_REPORT+2]
        call    ukb_has
        jnc     ukb_prsnext             ; was down already
        push    ecx
        xor     dl,dl
        call    usbhid_emit
        call    usbhid_repeats          ; CF if not a key that repeats
        jc      ukb_norep
        mov     [ebx+USBD_REPKEY],al
        mov     word ptr [ebx+USBD_REPMS],0
ukb_norep:
        pop     ecx
ukb_prsnext:
        inc     edi
        loop    ukb_prs

        ; and the report is the state from now on
        lea     edi,[ebx+USBD_REPORT]
        mov     ecx,2
        rep     movsd
ukb_out:
        popad
        ret

;--- ukb_has - CF clear if the usage AL is among the six bytes at EDX
ukb_has:
        push    ecx
        push    edx
        mov     ecx,6
ukh_lp:
        cmp     [edx],al
        je      ukh_yes
        inc     edx
        loop    ukh_lp
        pop     edx
        pop     ecx
        stc
        ret
ukh_yes:
        pop     edx
        pop     ecx
        clc
        ret

;--- usbhid_repeats - CF if the usage AL is not one that repeats: the
;--- locks, and anything the table has no code for
usbhid_repeats:
        cmp     al,39h                  ; Caps Lock
        je      ukr_no
        cmp     al,47h                  ; Scroll Lock
        je      ukr_no
        cmp     al,53h                  ; Num Lock
        je      ukr_no
        cmp     al,48h                  ; Pause
        je      ukr_no
        cmp     al,HID_TABLEN
        jae     ukr_no
        push    ebx
        movzx   ebx,al
        cmp     byte ptr [usbhid_tab+ebx],0
        pop     ebx
        je      ukr_no
        clc
        ret
ukr_no:
        stc
        ret

;=====================================================================
; usbhid_emit - AL = a HID usage, DL = 0 for a make and 80h for a
;               break: the set-1 scan codes for it, to kb_byte.  The
;               modifiers are usages E0h-E7h; Pause is three bytes
;               with its own prefix; the grey keys carry E0h in front.
;               Everything preserved.
;=====================================================================
usbhid_emit:
        pushad
        mov     dh,al
        cmp     al,HID_MODMASK
        jb      uhe_key
        ; a modifier
        and     eax,7
        mov     al,byte ptr [usbhid_modtab+eax]
        jmp     uhe_code
uhe_key:
        cmp     al,HID_TABLEN
        jae     uhe_out
        movzx   eax,al
        mov     al,byte ptr [usbhid_tab+eax]
        or      al,al
        jz      uhe_out                 ; nothing a PC keyboard has
        cmp     al,0FFh
        je      uhe_pause
uhe_code:
        test    al,80h
        jz      uhe_plain
        and     al,7Fh
        push    eax
        mov     al,0E0h
        call    usbhid_put
        pop     eax
uhe_plain:
        or      al,dl
        call    usbhid_put
        jmp     uhe_out
uhe_pause:
        ; E1 1D 45 down, E1 9D C5 up: what the key itself sends
        mov     al,0E1h
        call    usbhid_put
        mov     al,1Dh
        or      al,dl
        call    usbhid_put
        mov     al,45h
        or      al,dl
        call    usbhid_put
uhe_out:
        popad
        ret

;--- usbhid_put - AL = a byte, as the 8042 would have handed it over
usbhid_put:
        pushad
IFDEF HANG_E9
        push    eax
        mov     ah,al
        mov     al,'k'
        out     0E9h,al
        movzx   eax,ah
        call    e9_hex8
        mov     al,' '
        out     0E9h,al
        pop     eax
ENDIF
        call    kb_byte
        popad
        ret

;=====================================================================
; usbhid_repeat - once a tick: a key held on a USB keyboard repeats
;                 after the delay and at the rate KEYRATE= chose, which
;                 kb_ratewant holds in the 8042's own encoding.
;=====================================================================
usbhid_repeat:
        pushad
        mov     ebx,OFFSET usb_devs
        mov     ecx,USB_MAXDEV
uhr_dev:
        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     uhr_next
        cmp     byte ptr [ebx+USBD_CLASS],USBC_KBD
        jne     uhr_next
        mov     al,[ebx+USBD_REPKEY]
        or      al,al
        jz      uhr_next
        add     word ptr [ebx+USBD_REPMS],55
        call    usbhid_rate             ; -> DX = delay ms, SI = period ms
        cmp     word ptr [ebx+USBD_REPMS],dx
        jl      uhr_next
        sub     word ptr [ebx+USBD_REPMS],si
        push    ecx
        xor     dl,dl
        call    usbhid_emit
        pop     ecx
uhr_next:
        add     ebx,USBD_LEN
        loop    uhr_dev
        popad
        ret

;---------------------------------------------------------------------
; usbhid_rate - DX = the repeat delay and SI = the period, both in
;               milliseconds, out of kb_ratewant: bits 5-6 are the
;               delay in quarter seconds less one, bits 0-4 the rate as
;               (8 + low three) * 2^(next two) / 240 seconds a key.
;---------------------------------------------------------------------
usbhid_rate:
        push    eax
        push    ecx
        movzx   eax,byte ptr [kb_ratewant]
        mov     ecx,eax
        shr     ecx,5
        and     ecx,3
        inc     ecx
        imul    edx,ecx,250             ; the delay: a quarter second a step
        mov     ecx,eax
        shr     ecx,3
        and     ecx,3                   ; CL = the power of two
        and     eax,7
        add     eax,8
        shl     eax,cl
        imul    eax,25
        push    edx
        mov     ecx,6
        xor     edx,edx
        div     ecx                     ; EAX = (8+n) * 2^m * 1000 / 240
        pop     edx
        mov     esi,eax
        or      si,si
        jnz     urt_out
        mov     si,33                   ; never faster than thirty a second
urt_out:
        pop     ecx
        pop     eax
        ret

;=====================================================================
; usbhid_mouse - EBX -> the mouse, usbhid_rep = its report: buttons,
;                X and Y as signed bytes, perhaps a wheel.  Into
;                ms_ps2byte as the three bytes of a PS/2 packet, which
;                is the same information in the other order of sign.
;=====================================================================
usbhid_mouse:
        pushad
        mov     esi,OFFSET usbhid_rep
        mov     al,[esi]
        and     al,7                    ; left, right, middle: PS/2's order
        or      al,08h                  ; the always-set bit
        mov     cl,[esi+1]              ; X, as it is
        mov     dl,[esi+2]              ; Y, which PS/2 counts up the desk
        cmp     dl,80h
        jne     uhm_ynot128
        mov     dl,81h                  ; -128 has no negation in a byte
uhm_ynot128:
        neg     dl
        test    cl,80h
        jz      uhm_xpos
        or      al,10h
uhm_xpos:
        test    dl,80h
        jz      uhm_ypos
        or      al,20h
uhm_ypos:
        call    ms_ps2byte
        mov     al,cl
        call    ms_ps2byte
        mov     al,dl
        call    ms_ps2byte
        popad
        ret

;=====================================================================
; usbhid_leds - once a tick when nothing else is out: a keyboard whose
;               lights disagree with 40:17 is told.  CF if a transfer
;               was begun.
; usbhid_led_cont - and when that transfer has ended (AL = XS_*).
;=====================================================================
usbhid_leds:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     al,byte ptr ds:[BDA_KBFLAG]
        mov     dl,al
        xor     dh,dh
        test    dl,BDAK_NUM
        jz      ule_nonum
        or      dh,1
ule_nonum:
        test    dl,BDAK_CAPS
        jz      ule_nocaps
        or      dh,2
ule_nocaps:
        test    dl,BDAK_SCROLL
        jz      ule_noscroll
        or      dh,4
ule_noscroll:
        mov     byte ptr [usbhid_ledwant],dh
        mov     ebx,OFFSET usb_devs
        mov     ecx,USB_MAXDEV
ule_dev:
        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     ule_next
        cmp     byte ptr [ebx+USBD_CLASS],USBC_KBD
        jne     ule_next
        cmp     [ebx+USBD_LEDS],dh
        jne     ule_send
ule_next:
        add     ebx,USBD_LEN
        loop    ule_dev
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
ule_send:
        ; SET_REPORT(output, 1 byte): the lights
        mov     edi,dword ptr [usb_scr]
        add     edi,SCR_LED
        mov     [edi],dh
        movzx   edx,byte ptr [ebx+USBD_IFACE]
        mov     al,21h
        mov     ah,UREQ_SETREPORT
        mov     cx,0200h                ; output report, id 0
        push    edi
        mov     di,1
        call    usb_setup
        pop     edi
        mov     ecx,1
        mov     byte ptr [usb_ctlown],1
        call    usb_ctl_start
        jc      ule_nostart
        mov     byte ptr [usb_job],JOB_LED
        call    dev_slot
        mov     byte ptr [usb_jobdev],al
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret
ule_nostart:
        mov     byte ptr [usb_ctlown],0
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret

usbhid_led_cont:
        ; done or not, what was asked for is what the keyboard is taken
        ; to show: a keyboard that refuses is not asked every tick
        mov     al,byte ptr [usb_jobdev]
        call    dev_row
        cmp     byte ptr [ebx+USBD_STATE],USBS_READY
        jne     ut_out
        mov     al,byte ptr [usbhid_ledwant]
        mov     [ebx+USBD_LEDS],al
        jmp     ut_out

;=====================================================================
; usbhid_kbdgone - EBX -> a keyboard that has been unplugged: every key
;                  its last report had down comes up, or the shift
;                  state would hold for ever.
;=====================================================================
usbhid_kbdgone:
        pushad
        mov     edi,OFFSET usbhid_rep
        xor     eax,eax
        stosd
        stosd
        call    usbhid_kbd              ; an empty report: all breaks
        mov     byte ptr [ebx+USBD_REPKEY],0
        popad
        ret

;=====================================================================
; The tables.
;
; usbhid_modtab - the modifiers, usages E0h-E7h: left Ctrl, Shift, Alt
;                 and GUI, then the right-hand four.  Bit 7 means an
;                 E0h prefix.
; usbhid_tab    - usages 00h-65h to set-1 make codes.  Bit 7 means an
;                 E0h prefix; FFh is Pause; 0 is a key a PC has not.
;=====================================================================
usbhid_modtab:
        db      1Dh, 2Ah, 38h, 0DBh, 9Dh, 36h, 0B8h, 0DCh

usbhid_tab:
        db      0, 0, 0, 0                              ; 00-03: none, rollover...
        db      1Eh,30h,2Eh,20h,12h,21h,22h,23h         ; 04-0B: a b c d e f g h
        db      17h,24h,25h,26h,32h,31h,18h,19h         ; 0C-13: i j k l m n o p
        db      10h,13h,1Fh,14h,16h,2Fh,11h,2Dh         ; 14-1B: q r s t u v w x
        db      15h,2Ch                                 ; 1C-1D: y z
        db      02h,03h,04h,05h,06h,07h,08h,09h,0Ah,0Bh ; 1E-27: 1 2 3 4 5 6 7 8 9 0
        db      1Ch,01h,0Eh,0Fh,39h                     ; 28-2C: Enter Esc BS Tab Space
        db      0Ch,0Dh,1Ah,1Bh,2Bh,2Bh                 ; 2D-32: - = [ ] \ and the
                                                        ;        European #~ key
        db      27h,28h,29h,33h,34h,35h                 ; 33-38: ; ' ` , . /
        db      3Ah                                     ; 39: Caps Lock
        db      3Bh,3Ch,3Dh,3Eh,3Fh,40h,41h,42h,43h,44h ; 3A-43: F1-F10
        db      57h,58h                                 ; 44-45: F11 F12
        db      0B7h                                    ; 46: Print Screen (E0 37)
        db      46h                                     ; 47: Scroll Lock
        db      0FFh                                    ; 48: Pause
        db      0D2h,0C7h,0C9h,0D3h,0CFh,0D1h           ; 49-4E: Ins Home PgUp Del
                                                        ;        End PgDn
        db      0CDh,0CBh,0D0h,0C8h                     ; 4F-52: Right Left Down Up
        db      45h                                     ; 53: Num Lock
        db      0B5h,37h,4Ah,4Eh,9Ch                    ; 54-58: KP/ KP* KP- KP+ KPEnter
        db      4Fh,50h,51h,4Bh,4Ch,4Dh,47h,48h,49h     ; 59-61: KP1-KP9
        db      52h,53h                                 ; 62-63: KP0 KP.
        db      56h                                     ; 64: the <>| key
        db      0DDh                                    ; 65: Application (E0 5D)
HID_TABLEN      EQU     66h

usbhid_rep      db      8 dup(0)        ; the report being acted on
usbhid_ledwant  db      0               ; the lights the job out asks for
