;=====================================================================
; TLSSTK.ASM - run a TLS-core routine on a big private stack.
;
; The TLS handshake and record crypto (RSA, ECDHE, AES, SHA) have deep
; call trees with kilobyte stack frames - a BIGNUM alone is 512 bytes.
; A socket call reaches the parked stack through the WfW bridge on a
; tiny scratch stack (PMDOS.386's low page, about 1.5K), far too small
; for that.  So the driver hands the crypto its own big stack out of the
; kernel arena, and this switches to it for one run-to-completion call
; and back - no fibre is needed, because a TLS step never blocks (it
; returns WANT_READ and is re-entered later).
;
; Pure Open Watcom (wasm), flat 32-bit.  The seam is __cdecl, like the
; rest of the driver's asm-to-C contract (DRV\NDSTART.ASM).
;=====================================================================
        .386p
        .model  flat
        .code

        public  _tls_run
;--- u32 __cdecl tls_run( void *newtop, int (__cdecl *fn)( void * ), void *arg )
;    Switch ESP to newtop, call fn( arg ), restore ESP, return fn's EAX.
;    fn is __cdecl, so it preserves EBX, ESI, EDI and EBP - which is how
;    the caller's ESP, kept in ESI across the switch, survives the call.
_tls_run:
        push    ebp
        mov     ebp,esp
        push    esi
        push    edi
        push    ebx
        mov     edi,[ebp+8]             ; newtop (top of the big stack)
        mov     eax,[ebp+12]            ; fn
        mov     edx,[ebp+16]            ; arg
        mov     esi,esp                 ; remember the caller's stack
        mov     esp,edi                 ; onto the big stack
        push    edx                     ; fn( arg )   - cdecl
        call    eax
        mov     esp,esi                 ; back to the caller's stack
        pop     ebx
        pop     edi
        pop     esi
        pop     ebp
        ret
        end
