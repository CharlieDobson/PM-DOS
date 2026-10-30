;=====================================================================
; NDSTART.ASM - the start of a 32-bit DEVICE= driver written in C
;
; What TOOLS\NDTEST.ASM is by hand, for a driver whose body is C: the
; D32 driver header (INC\NDRV.INC) first, which is why this module is
; first on the link line; the four entries the kernel calls, each
; turned from its register contract into a C call; the kernel's
; services turned the other way, from a C call into the register
; contract; and the interrupt and clock handlers, which the kernel
; calls into here and which call the C driver's.  The driver's table
; is the C side's (nd_table, INC\NDRV.H), because the name in it is
; the driver's own and a table made here would have to be told it.
;
; THE C SIDE provides, cdecl (NDRV.H says the same):
;   int  nd_init( const char *line )     0 installed, else refused
;   void nd_park( int back )             0 going down, 1 come back
;   void nd_halt( void )
;   int  nd_call( u32 fn, void *buf, u32 len, u32 *answer )
;                                        0 and *answer is EAX, else ERR_*
;   int  nd_irq( u32 line )              1: the device was asking
;   void nd_tick( void )
; and gets nds_alloc, nds_free, nds_print, nds_ticks, nds_bus,
; nds_irqhook, nds_irqunhook, nds_tickhook, nds_tickunhook, and the
; kernel's row in nds_row.
;
; ES IS MADE DS at every entry, because C compiled flat assumes it
; and the kernel promises it only at INIT.  The C side preserves
; EBX, ESI, EDI and EBP as cdecl does, which is what the kernel
; needs of an entry (NDRV.INC: every register but the stack is the
; entry's to use, and the kernel keeps its own - except that the
; CALL path reads its frame through EBP after the entry returns).
;
; Compiled C: wcc386 -mf -3r -s -zl, as every PM-DOS native is; the
; register convention is the C side's own business, since everything
; that crosses this file is cdecl.  Linked twice, at 0 and at
; 10000000h, and put together by D32DRV with DRVSTUB.BIN in front -
; see BUILD.CMD under NE2000.SYS.  No BSS of its own: the link orders
; class D32END after BSS so that the file holds the zeros, and the
; header's BSS field is 0.
;=====================================================================
        .386p
        .model  flat

        include pmdos.inc
        include ndrv.inc

        extrn   _nd_table:byte
        extrn   _nd_init:near
        extrn   _nd_park:near
        extrn   _nd_halt:near
        extrn   _nd_call:near
        extrn   _nd_irq:near
        extrn   _nd_tick:near

        .code

        D32_DRVHEAD <OFFSET _nd_table>, 0

        public  _ndv_init, _ndv_park, _ndv_halt, _ndv_call
        public  _nds_alloc, _nds_free, _nds_print, _nds_ticks, _nds_bus
        public  _nds_irqhook, _nds_irqunhook, _nds_tickhook, _nds_tickunhook
        public  _nds_niccount, _nds_niccall, _nds_sockcall, _nds_flagcall

;=====================================================================
; THE ENTRIES - the kernel's register contracts, into C
;=====================================================================

;--- INIT: ESI -> the services, EDI -> the row, EDX -> the line -> CF
_ndv_init:
        mov     [nds_services],esi
        mov     [_nds_row],edi
        push    ds
        pop     es
        cld
        push    edx
        call    _nd_init
        add     esp,4
        or      eax,eax
        jnz     ndv_refused
        clc
        ret
ndv_refused:
        stc
        ret

;--- PARK: AL = 0 going down, 1 come back
_ndv_park:
        push    ds
        pop     es
        cld
        movzx   eax,al
        push    eax
        call    _nd_park
        add     esp,4
        ret

;--- HALT
_ndv_halt:
        push    ds
        pop     es
        cld
        call    _nd_halt
        ret

;--- CALL: EBX = the function, EDI -> the buffer, ECX = its length,
;--- ESI -> the row -> EAX, or CF and AX = ERR_*
_ndv_call:
        push    ds
        pop     es
        cld
        mov     dword ptr [nds_answer],0
        push    OFFSET nds_answer
        push    ecx
        push    edi
        push    ebx
        call    _nd_call
        add     esp,16
        or      eax,eax
        jnz     ndv_error
        mov     eax,[nds_answer]
        clc
        ret
ndv_error:
        stc
        ret

;--- the interrupt handler the kernel calls: EBX = the line, interrupts
;--- off -> CF clear if it was the device's
ndv_irq:
        push    ds
        pop     es
        cld
        push    ebx
        call    _nd_irq
        add     esp,4
        or      eax,eax
        jz      ndv_notmine
        clc
        ret
ndv_notmine:
        stc
        ret

;--- the clock, every whole tick, interrupts off
ndv_tick:
        push    ds
        pop     es
        cld
        call    _nd_tick
        ret

;=====================================================================
; THE SERVICES - C calls, into the kernel's register contracts.  Each
; service preserves what it does not return (NDRV.INC); what cdecl
; lets a callee change is EAX, ECX and EDX, so EBX, ESI and EDI are
; kept here when a contract uses them.
;=====================================================================

;--- void *nds_alloc( u32 bytes ) - 0 if none
_nds_alloc:
        push    ebx
        mov     ebx,[esp+8]
        mov     edx,[nds_services]
        call    dword ptr [edx+NDS_ALLOC]
        jnc     nda_got
        xor     eax,eax
nda_got:
        pop     ebx
        ret

;--- void nds_free( void *block )
_nds_free:
        mov     edx,[esp+4]
        mov     ecx,[nds_services]
        call    dword ptr [ecx+NDS_FREE]
        ret

;--- void nds_print( const char *text )
_nds_print:
        push    esi
        mov     esi,[esp+8]
        mov     edx,[nds_services]
        call    dword ptr [edx+NDS_PRINT]
        pop     esi
        ret

;--- u32 nds_ticks( void )
_nds_ticks:
        mov     edx,[nds_services]
        call    dword ptr [edx+NDS_TICKS]
        ret

;--- int nds_bus( REGS *regs ) - the registers in, the registers and
;--- the flags out; 1 if CF came back clear
_nds_bus:
        push    ebp
        push    ebx
        push    esi
        push    edi
        mov     ebp,[esp+20]            ; REGS *
        mov     ebx,[ebp+4]
        mov     ecx,[ebp+8]
        mov     edx,[ebp+12]
        mov     esi,[ebp+16]
        mov     edi,[ebp+20]
        mov     eax,[ebp]
        push    ebp
        mov     ebp,[nds_services]
        call    dword ptr [ebp+NDS_BUS]
        pop     ebp
        mov     [ebp],eax
        mov     [ebp+4],ebx
        mov     [ebp+8],ecx
        mov     [ebp+12],edx
        mov     [ebp+16],esi
        mov     [ebp+20],edi
        pushfd
        pop     eax
        mov     [ebp+24],eax
        and     eax,1
        xor     eax,1
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

;--- int nds_irqhook( u32 line ) - nd_irq on that line; 1 if hooked
_nds_irqhook:
        push    edi
        mov     eax,[esp+8]
        mov     edi,OFFSET ndv_irq
        mov     edx,[nds_services]
        call    dword ptr [edx+NDS_IRQHOOK]
        setnc   al
        movzx   eax,al
        pop     edi
        ret

;--- int nds_irqunhook( u32 line )
_nds_irqunhook:
        push    edi
        mov     eax,[esp+8]
        mov     edi,OFFSET ndv_irq
        mov     edx,[nds_services]
        call    dword ptr [edx+NDS_IRQUNHOOK]
        setnc   al
        movzx   eax,al
        pop     edi
        ret

;--- int nds_tickhook( void ) - nd_tick every tick; 1 if hooked
_nds_tickhook:
        push    edi
        mov     edi,OFFSET ndv_tick
        mov     edx,[nds_services]
        call    dword ptr [edx+NDS_TICKHOOK]
        setnc   al
        movzx   eax,al
        pop     edi
        ret

;--- int nds_tickunhook( void )
_nds_tickunhook:
        push    edi
        mov     edi,OFFSET ndv_tick
        mov     edx,[nds_services]
        call    dword ptr [edx+NDS_TICKUNHOOK]
        setnc   al
        movzx   eax,al
        pop     edi
        ret

;--- u32 nds_niccount( void ) - how many loaded drivers are NICs
_nds_niccount:
        mov     edx,[nds_services]
        call    dword ptr [edx+NDS_NICCOUNT]    ; -> EAX
        ret

;--- int nds_niccall( u32 idx, u32 fn, void *buf, u32 len, u32 *answer )
;--- 0 and *answer = what the card answered, else ERR_*
_nds_niccall:
        push    ebx
        push    esi
        push    edi
        mov     eax,[esp+16]                    ; idx
        mov     ebx,[esp+20]                    ; fn
        mov     edi,[esp+24]                    ; buf
        mov     ecx,[esp+28]                    ; len
        mov     edx,[nds_services]
        call    dword ptr [edx+NDS_NICCALL]     ; -> EAX, CF, AX = ERR_*
        jc      nnc_err
        mov     edx,[esp+32]                    ; *answer
        mov     [edx],eax
        xor     eax,eax
        jmp     short nnc_out
nnc_err:
        movzx   eax,ax
nnc_out:
        pop     edi
        pop     esi
        pop     ebx
        ret

;--- int nds_flagcall( u32 flag, u32 fn, void *buf, u32 len, u32 *answer )
;--- 0 and *answer = what the first driver with that NDTF_* flag
;--- answered, else ERR_*; ERR_FUNC from a kernel without NDS_FLAGCALL
_nds_flagcall:
        push    ebx
        push    esi
        push    edi
        mov     edx,[nds_services]
        cmp     word ptr [edx+NDS_VER],5        ; NDS_VERSION 5 brought it
        jb      nfc_old
        mov     eax,[esp+16]                    ; flag (NDTF_*)
        mov     ebx,[esp+20]                    ; fn
        mov     edi,[esp+24]                    ; buf
        mov     ecx,[esp+28]                    ; len
        call    dword ptr [edx+NDS_FLAGCALL]    ; -> EAX, CF, AX = ERR_*
        jc      nfc_err
        mov     edx,[esp+32]                    ; *answer
        mov     [edx],eax
        xor     eax,eax
        jmp     short nfc_out
nfc_old:
        mov     eax,ERR_FUNC
        jmp     short nfc_out
nfc_err:
        movzx   eax,ax
nfc_out:
        pop     edi
        pop     esi
        pop     ebx
        ret

;--- int nds_sockcall( u32 fn, void *buf, u32 len, u32 *answer )
;--- 0 and *answer = what the TCP/IP stack answered, else ERR_*
_nds_sockcall:
        push    ebx
        push    esi
        push    edi
        mov     ebx,[esp+16]                    ; fn (SOCK_*)
        mov     edi,[esp+20]                    ; buf
        mov     ecx,[esp+24]                    ; len
        mov     edx,[nds_services]
        call    dword ptr [edx+NDS_SOCKCALL]    ; -> EAX, CF, AX = ERR_*
        jc      nsc_err
        mov     edx,[esp+28]                    ; *answer
        mov     [edx],eax
        xor     eax,eax
        jmp     short nsc_out
nsc_err:
        movzx   eax,ax
nsc_out:
        pop     edi
        pop     esi
        pop     ebx
        ret

        .data
nds_services    dd      0               ; the kernel's table, for life
nds_answer      dd      0               ; what nd_call answered
        public  _nds_row
_nds_row        dd      0

; THE LAST BYTES OF THE IMAGE, so that wlink writes the BSS out as
; zeroes to reach them (the link orders class D32END after BSS).
D32END  segment dword public 'D32END'
        db      'D32END',0,0
D32END  ends

DGROUP  group   _TEXT, D32END

        end
