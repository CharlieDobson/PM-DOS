;=====================================================================
; WSKGLUE.ASM - the thin assembler glue between WINSOCK.DLL (16-bit C,
;               Open Watcom) and PMDOS.386's PM API, the bridge into the
;               parked PM-DOS kernel's TCP/IP stack.
;
; Three routines, all called far (large model) with their arguments in
; registers (declared with #pragma aux in WINSOCK.C, so there are no
; stack parameters to clean and no reliance on DS):
;
;   wsk_getapi() -> DX:AX = the VxD's PM API call-in address (ES:DI from
;     INT 2Fh AX=1684h, BX=504Dh), or 0:0 if there is no bridge under us.
;
;   wsk_call(fn, len, buflin, apiseg, apioff) -> DX:AX, where AX is the
;     socket op's answer and DX is the carry (0 or 1).  Sets AX=PDAPI_SOCK,
;     BX=fn, CX=len, EDX=buflin (the request buffer's flat linear address,
;     in conventional memory), then far-calls the PM API.
;
;   wsk_yield() - INT 2Fh AX=1680h, release this VM's time slice.
;
; Assembled with wasm; USE16, .386 (the bridge passes EDX, so a 32-bit op).
;=====================================================================
        .386
_TEXT   SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_TEXT

        PUBLIC  wsk_getapi_
        PUBLIC  wsk_call_
        PUBLIC  wsk_yield_

PDAPI_SOCK      EQU     3
PMDOS_DEVICE_ID EQU     504Dh

;--- wsk_getapi() -> DX:AX = ES:DI (the PM API entry; 0:0 if none) ----
wsk_getapi_:
        xor     di,di
        mov     es,di
        mov     ax,1684h
        mov     bx,PMDOS_DEVICE_ID
        int     2Fh                     ; ES:DI = call-in address
        mov     dx,es
        mov     ax,di
        retf

;--- wsk_call(fn[bx], len[cx], buflin[dx:ax], apiseg[si], apioff[di]) --
;    -> DX:AX (AX = answer, DX = carry 0/1)
wsk_call_:
        push    bp
        ; compose EDX = buflin (DX = high word, AX = low word)
        push    bx                      ; save fn
        push    cx                      ; save len
        movzx   edx,dx                  ; edx = 0000:HIGH
        shl     edx,16                  ; edx = HIGH:0000
        mov     dx,ax                   ; edx = HIGH:LOW = buflin
        pop     cx                      ; CX = len
        pop     bx                      ; BX = fn
        mov     ax,PDAPI_SOCK           ; AX = the sub-function
        push    si                      ; the API selector (high of the far ptr)
        push    di                      ; the API offset    (low)
        mov     bp,sp                   ; BP -> the far pointer on the stack
        call    dword ptr [bp]          ; far-call the PM API (SS:[bp])
        add     sp,4                    ; drop the far pointer
        sbb     dx,dx                   ; DX = 0xFFFF if CF, else 0
        and     dx,1                    ; DX = the carry (0 or 1)
        pop     bp
        retf

;--- wsk_yield() - release this VM's time slice ----------------------
wsk_yield_:
        mov     ax,1680h
        int     2Fh
        retf

_TEXT   ENDS
        END
