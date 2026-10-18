;=====================================================================
; DRVFIB.ASM - a two-context cooperative fibre for the SMBFS driver
;
; The SMB conversation is written straight-line C: it sends a request
; and waits for the reply in a spin loop that the clock tick pumps the
; stack under (DRV\SMBFS\DRVSOCK.C).  That is exactly right when the
; native kernel calls it, interrupts on.  It cannot work through the WfW
; bridge (PMDOS.386), whose window runs interrupts off: the wait would
; spin for ever with nothing advancing the stack.
;
; So, when the bridge asks for it (REDIR_RESUME, INC\REDIR.H), the driver
; runs the op on a WORKER FIBRE - a second stack of its own - and the
; socket waits, instead of spinning, SWITCH BACK to the caller when there
; is no data yet.  The caller (the bridge's poll, which lets Windows run
; between calls so the tick and the card interrupt advance the parked
; stack) switches back IN, and the wait resumes where it left off.  The
; SMB2/SMB1 engines never know; only the leaf wait yields.
;
; It is the classic setjmp/longjmp context switch, two ways: fib_enter
; runs the worker (save the main context, load the worker's), fib_yield
; gives control back (save the worker's, load the main's).  Each saves
; the four callee-saved registers and ESP; the return address rides on
; the stack ESP points at, so a plain RET lands in the other context.
; fib_init lays out the worker's first stack so the first fib_enter rets
; into its entry function.
;
; Pure Open Watcom wasm, like the rest of PM-DOS; cdecl names and stack
; arguments, as DRV\NDSTART.ASM's services are, so the C side reaches
; them the same way.  No header or D32END of its own - NDSTART.ASM, first
; on the link line, carries those.
;=====================================================================
        .386p
        .model  flat

        .code

        public  _fib_init, _fib_enter, _fib_yield

;--- the two saved contexts: EBX, ESI, EDI, EBP, ESP (the callee-saved
;--- registers and the stack; EIP rides on the stack) --------------------
FC_EBX          EQU     0
FC_ESI          EQU     4
FC_EDI          EQU     8
FC_EBP          EQU     12
FC_ESP          EQU     16

;=====================================================================
; void __cdecl fib_init( u32 stacktop, void (*entry)(void) )
;
; Lay out the worker's initial stack so that the first fib_enter's RET
; lands in `entry` with a clean frame.  `stacktop` is one past the top
; of the worker's stack buffer (the stack grows down from it).
;=====================================================================
_fib_init:
        mov     eax,[esp+4]             ; stacktop
        mov     edx,[esp+8]             ; entry
        sub     eax,4
        mov     dword ptr [eax],0       ; a guard return slot (entry never returns)
        sub     eax,4
        mov     [eax],edx               ; fib_enter's RET pops this -> entry
        mov     [fib_work+FC_ESP],eax
        xor     ecx,ecx                 ; the callee-saved registers start clear
        mov     [fib_work+FC_EBX],ecx
        mov     [fib_work+FC_ESI],ecx
        mov     [fib_work+FC_EDI],ecx
        mov     [fib_work+FC_EBP],ecx
        ret

;=====================================================================
; void __cdecl fib_enter( void ) - save the main context, run the worker.
; Returns when the worker calls fib_yield (or finishes and yields).
;=====================================================================
_fib_enter:
        mov     [fib_main+FC_EBX],ebx
        mov     [fib_main+FC_ESI],esi
        mov     [fib_main+FC_EDI],edi
        mov     [fib_main+FC_EBP],ebp
        mov     [fib_main+FC_ESP],esp
        mov     ebx,[fib_work+FC_EBX]
        mov     esi,[fib_work+FC_ESI]
        mov     edi,[fib_work+FC_EDI]
        mov     ebp,[fib_work+FC_EBP]
        mov     esp,[fib_work+FC_ESP]
        ret                             ; -> the worker, where it last was

;=====================================================================
; void __cdecl fib_yield( void ) - save the worker, hand back to main.
; Returns (in the worker) when main calls fib_enter again.
;=====================================================================
_fib_yield:
        mov     [fib_work+FC_EBX],ebx
        mov     [fib_work+FC_ESI],esi
        mov     [fib_work+FC_EDI],edi
        mov     [fib_work+FC_EBP],ebp
        mov     [fib_work+FC_ESP],esp
        mov     ebx,[fib_main+FC_EBX]
        mov     esi,[fib_main+FC_ESI]
        mov     edi,[fib_main+FC_EDI]
        mov     ebp,[fib_main+FC_EBP]
        mov     esp,[fib_main+FC_ESP]
        ret                             ; -> main, after its fib_enter

        .data
fib_main        dd      5 dup(0)        ; the caller's context
fib_work        dd      5 dup(0)        ; the worker fibre's context

        end
