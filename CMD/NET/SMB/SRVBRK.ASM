;
; SRVBRK.ASM - SMBSRV's INT 23h handler
;
; Ctrl+Break is counted here and the DOS call it arrived in carries on,
; so SMBSRV can close its connections before it ends instead of being
; ended by the default handler in the middle of a reply.  A Ctrl+C
; arrives the same way: a native program has no way to tell the two
; apart (40:71 bit 7 is set by every Ctrl+Break and never cleared).
;
; The kernel loads DS with the program's data before it calls the
; handler (r3_enter), and the handler returns with IRETD.
;
        .386p
        .model  flat

        .code

        public  _srv_brkstub
_srv_brkstub proc near
        inc     dword ptr [_srv_breaks]
        iretd
_srv_brkstub endp

        .data

        public  _srv_breaks
_srv_breaks     dd      0

        end
