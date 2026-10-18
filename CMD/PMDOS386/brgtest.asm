;=====================================================================
; BRGTEST.ASM - BRGTEST.COM, a 16-bit DOS program that calls PMDOS.386's
;               V86 API from inside Windows for Workgroups 3.11 enhanced
;               mode and shows what the parked kernel handed back.
;
; It knows nothing about the bridge: it finds the VxD the way any V86
; program finds a VxD's call-in entry - INT 2Fh AX=1684h with the
; device id in BX, which hands back a far address in ES:DI (0:0 when
; there is no such device, which is also what a plain DOS session with
; no WIN386 under it gives).  Then it far-calls that entry with AX = the
; sub-function, ES:DX a buffer and CX its length, and reads the answer.
;
; B1 asks sub-function 0 (NIC_INFO): the API performs the same bridge
; call Device_Init proves at load, on this program's behalf, and copies
; the card's 52-byte info block into the buffer.  The six bytes at the
; front are the MAC.  So a round trip here is a V86 program reaching the
; parked 32-bit kernel's driver and back.
;
; Run from WINSTART.BAT, which WIN386 executes in the system VM once the
; VMM is up.  Its output is redirected to PRN so the headless rig reads
; it off LPT; the VxD's own trace comes out on port E9h.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_T      SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_T, DS:_T, ES:_T, SS:_T
        ORG     0

PMDOS_DEVICE_ID EQU     504Dh
PDAPI_NICINFO   EQU     0

start:
        mov     dx,OFFSET msg_hi
        mov     ah,09h
        int     21h

;--- the VxD's V86 API entry, if there is one ------------------------
        xor     di,di
        mov     es,di                   ; ES:DI = 0:0, to tell if VMM sets it
        mov     ax,1684h
        mov     bx,PMDOS_DEVICE_ID
        int     2Fh                     ; -> ES:DI = the entry, or 0:0
        mov     ax,es
        or      ax,di
        jnz     have_api
        mov     dx,OFFSET msg_noapi
        mov     ah,09h
        int     21h
        jmp     done

have_api:
        mov     word ptr [api_ent],di
        mov     word ptr [api_ent+2],es
        mov     dx,OFFSET msg_at
        mov     ah,09h
        int     21h
        mov     ax,es
        call    put4
        mov     dl,':'
        mov     ah,02h
        int     21h
        mov     ax,di
        call    put4
        call    crlf

;--- call it: AX = sub-function, ES:DX = buffer, CX = its length ------
        push    cs
        pop     es                      ; the 1684h call left ES on the VxD;
        mov     dx,OFFSET buf           ; point it back at this program's data
        mov     cx,BUF_LEN
        mov     ax,PDAPI_NICINFO
        call    dword ptr [api_ent]     ; -> AX = answer, CF if it failed
        mov     word ptr [r_ax],ax      ; (MOV keeps the carry)
        jc      call_failed

        mov     dx,OFFSET msg_ok
        mov     ah,09h
        int     21h
        mov     ax,word ptr [r_ax]
        call    put4
        mov     dx,OFFSET msg_mac
        mov     ah,09h
        int     21h
        mov     si,OFFSET buf
        mov     cx,6
mac_byte:
        lodsb
        call    put2
        dec     cx
        jz      mac_done
        mov     dl,':'
        mov     ah,02h
        int     21h
        jmp     mac_byte
mac_done:
        call    crlf
        jmp     done

call_failed:
        mov     dx,OFFSET msg_cf
        mov     ah,09h
        int     21h
done:
        mov     ax,4C00h
        int     21h

;--- hex and newline, as XMSTEST.COM writes them ---------------------
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
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        push    ax
        mov     ah,02h
        int     21h
        pop     ax
        ret
crlf:
        mov     dl,13
        mov     ah,02h
        int     21h
        mov     dl,10
        mov     ah,02h
        int     21h
        ret

;--- data ------------------------------------------------------------
BUF_LEN     equ 64
api_ent     dd  0
r_ax        dw  0
buf         db  BUF_LEN dup(0)

msg_hi      db  'BRGTEST: PMDOS.386 V86 API test',13,10,'$'
msg_noapi   db  'BRGTEST: no bridge API (INT 2Fh 1684h gave 0:0)',13,10,'$'
msg_at      db  'BRGTEST: API entry at ','$'
msg_ok      db  'BRGTEST: call ok, answer=','$'
msg_mac     db  ' MAC=','$'
msg_cf      db  'BRGTEST: call returned carry - failed',13,10,'$'
_T      ENDS
        END     start
