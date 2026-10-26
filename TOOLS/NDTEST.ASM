;=====================================================================
; NDTEST.ASM - NDTEST.SYS, a 32-bit driver to load with DEVICE=
;
; The native counterpart of TESTDEV.SYS: a D32 image of type driver
; (INC\NDRV.INC), built as every 32-bit driver is - linked twice and
; put together by D32DRV, see BUILD.CMD - loaded above the megabyte,
; relocated, and called through its table.  It uses every service
; the kernel offers a driver and keeps count, and NDRVTEST.EXE reads
; the counts back through 21h/F1h AL=25h.
;
;   INIT    prints a line through NDS_PRINT - a service call made at
;           CONFIG.SYS time - keeps the line it was given, takes a
;           block through NDS_ALLOC and gives it back through NDS_FREE,
;           takes another and writes a pattern in it, hooks
;           the clock, and hooks the interrupt line a /I:n on its
;           line names: one with nothing on it, so that the count
;           stays at nought and the hook itself is what is tested.
;   PARK    counts the calls on either side of a handover.
;   HALT    counts the call; there is nothing of its own to stop.
;   CALL    fills the caller's buffer with all of that (NDTEST.INC),
;           and with the one thing only a relocated driver can answer:
;           the address of its table as a dword the linker wrote,
;           against the address the code finds itself at.  If the
;           loader applied the table, the two agree.
;
; Linked with:  wlink form raw bin option offset=0x0, and again with
;               offset=0x10000000; D32DRV makes NDTEST.SYS of the two
;=====================================================================
        .386P
        INCLUDE PMDOS.INC
        INCLUDE NDRV.INC
        INCLUDE NDTEST.INC

_ND     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_ND, DS:_ND, ES:_ND, SS:_ND
        ORG     0

image:
        D32_DRVHEAD <OFFSET drvtab>, 0

;--- the driver table: what the header's entry field names ------------
drvtab:
        dd      NDT_SIGVAL
        dw      NDT_VERSION
        dw      0
        db      'NDTEST  '
        dd      OFFSET nd_init
        dd      OFFSET nd_park
        dd      OFFSET nd_halt
        dd      OFFSET nd_call

;=====================================================================
; INIT - ESI -> the services, EDI -> the row, EDX -> the line.
;=====================================================================
nd_init:
        mov     [services],esi
        mov     [row],edi

        ; the line, up to the CR the loader ends it with
        push    esi
        push    edi
        mov     esi,edx
        mov     edi,OFFSET linebuf
        mov     ecx,79
nil_copy:
        lodsb
        cmp     al,13
        je      nil_lend
        or      al,al
        jz      nil_lend
        stosb
        loop    nil_copy
nil_lend:
        mov     byte ptr [edi],0
        pop     edi
        pop     esi

        ; /I:n on it, if there is one
        call    nd_findirq
        mov     esi,[services]

        ; A BLOCK GIVEN BACK AT ONCE, before the one that is kept.  The
        ; kernel counts what a driver holds (NDF_MEM) by both calls, so
        ; NDRVTEST seeing one block of 4096 bytes says the free was
        ; taken off as well as the allocations put on; and first fit
        ; puts the kept block where this one was.
        mov     ebx,512
        call    dword ptr [esi+NDS_ALLOC]
        jc      nil_nofree
        mov     edx,eax
        call    dword ptr [esi+NDS_FREE]
nil_nofree:

        ; a block, and a pattern in it that is read back at once
        mov     ebx,4096
        call    dword ptr [esi+NDS_ALLOC]
        jc      nil_noblock
        mov     [block],eax
        push    edi
        mov     edi,eax
        mov     ecx,1024
        mov     eax,0C0DEF00Dh
        cld
        rep     stosd
        pop     edi
        mov     esi,[block]
        mov     ecx,1024
nil_check:
        cmp     dword ptr [esi],0C0DEF00Dh
        jne     nil_nopattern
        add     esi,4
        loop    nil_check
        mov     dword ptr [blockok],1
nil_nopattern:
nil_noblock:
        mov     esi,[services]

        ; the clock
        mov     edi,OFFSET nd_tick
        call    dword ptr [esi+NDS_TICKHOOK]
        setc    byte ptr [tickhook]

        ; the line /I: named
        cmp     dword ptr [irqline],-1
        je      nil_noirq
        mov     eax,[irqline]
        mov     edi,OFFSET nd_irq
        call    dword ptr [esi+NDS_IRQHOOK]
        setc    byte ptr [irqhook]
nil_noirq:

        ; and a word on the way in
        mov     edx,[services]
        mov     esi,OFFSET msg_loaded
        call    dword ptr [edx+NDS_PRINT]
        clc
        ret

;--- nd_findirq - "/I:n" anywhere on the line -> [irqline], else -1 ---
nd_findirq:
        mov     dword ptr [irqline],-1
        mov     esi,OFFSET linebuf
nfi_scan:
        mov     al,[esi]
        or      al,al
        jz      nfi_done
        cmp     al,'/'
        jne     nfi_next
        mov     al,[esi+1]
        or      al,20h
        cmp     al,'i'
        jne     nfi_next
        cmp     byte ptr [esi+2],':'
        jne     nfi_next
        add     esi,3
        xor     eax,eax
        xor     ecx,ecx
nfi_digit:
        mov     cl,[esi]
        sub     cl,'0'
        cmp     cl,9
        ja      nfi_got
        imul    eax,10
        add     eax,ecx
        inc     esi
        jmp     short nfi_digit
nfi_got:
        cmp     eax,15
        ja      nfi_done
        mov     [irqline],eax
        ret
nfi_next:
        inc     esi
        jmp     short nfi_scan
nfi_done:
        ret

;=====================================================================
; The clock, the line, a park and a halt: counted and nothing more.
; The interrupt handler answers CF set: nothing of its own was asking,
; which is the truth on a line with nothing on it, and what a handler
; on a shared line owes the others.
;=====================================================================
nd_tick:
        inc     dword ptr [ticks]
        ret

nd_irq:
        inc     dword ptr [irqs]
        stc
        ret

nd_park:
        or      al,al
        jnz     ndp_up
        inc     dword ptr [parks]
        ret
ndp_up:
        inc     dword ptr [unparks]
        ret

nd_halt:
        inc     dword ptr [halts]
        ret

;=====================================================================
; CALL - EBX = NDC_REPORT, EDI -> ECX bytes: the report.
;=====================================================================
nd_call:
        inc     dword ptr [calls]
        cmp     ebx,NDC_REPORT
        jne     ndc_what
        cmp     ecx,NDTR_LEN
        jb      ndc_short
        mov     eax,[linked]
        mov     [edi+NDTR_LINKED],eax
        call    ndc_here                ; where the code IS: the return
ndc_here:                               ; address, less a constant
        pop     eax
        sub     eax,OFFSET ndc_here - OFFSET drvtab
        mov     [edi+NDTR_RUNNING],eax
        mov     eax,dword ptr [image+D32H_BASE]
        mov     [edi+NDTR_HDRBASE],eax
        mov     eax,dword ptr [image+D32H_ENTRY]
        mov     [edi+NDTR_HDRENTRY],eax
        mov     eax,dword ptr [image+D32H_NRELOC]
        mov     [edi+NDTR_NRELOC],eax
        mov     eax,[services]
        mov     [edi+NDTR_SERVICES],eax
        mov     eax,[row]
        mov     [edi+NDTR_ROW],eax
        mov     eax,[block]
        mov     [edi+NDTR_BLOCK],eax
        mov     eax,[blockok]
        mov     [edi+NDTR_BLOCKOK],eax
        mov     eax,[ticks]
        mov     [edi+NDTR_TICKS],eax
        movzx   eax,byte ptr [tickhook]
        mov     [edi+NDTR_TICKHOOK],eax
        mov     eax,[parks]
        mov     [edi+NDTR_PARKS],eax
        mov     eax,[unparks]
        mov     [edi+NDTR_UNPARKS],eax
        mov     eax,[halts]
        mov     [edi+NDTR_HALTS],eax
        mov     eax,[irqline]
        mov     [edi+NDTR_IRQ],eax
        movzx   eax,byte ptr [irqhook]
        mov     [edi+NDTR_IRQHOOK],eax
        mov     eax,[irqs]
        mov     [edi+NDTR_IRQS],eax
        mov     eax,[calls]
        mov     [edi+NDTR_CALLS],eax
        push    edi
        add     edi,NDTR_LINE
        mov     esi,OFFSET linebuf
        mov     ecx,80 / 4
        cld
        rep     movsd
        pop     edi
        mov     eax,NDTR_LEN
        clc
        ret
ndc_short:
        mov     eax,ERR_BADDATA
        stc
        ret
ndc_what:
        mov     eax,ERR_FUNC
        stc
        ret

;=====================================================================
; Data
;=====================================================================
linked      dd  OFFSET drvtab         ; relocated by the loader, or not
services    dd  0
row         dd  0
block       dd  0
blockok     dd  0
ticks       dd  0
parks       dd  0
unparks     dd  0
halts       dd  0
irqs        dd  0
calls       dd  0
irqline     dd  -1
tickhook    db  0
irqhook     db  2                       ; not asked, until it is
            db  0, 0
linebuf     db  80 dup(0)
msg_loaded  db  'NDTEST.SYS: a 32-bit driver, loaded',13,10,0

_ND     ENDS
        END
